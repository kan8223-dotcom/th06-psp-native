# 検証記録

## V-001: engine接続後のPPSSPPデータ欠損経路

- 日付: 2026-08-04
- 実行物: 開発EBOOT（原作データ・SF2なし）
- 実行環境: PPSSPP v1.20.4、portable版の `memstick/PSP/GAME/TH06PSP/EBOOT.PBP`
- 結果: プロセスは起動5秒後も存続。`th06.cfg`（56 bytes）と`log.txt`（605 bytes）を生成。
- 到達確認: config初期化、32-bit画面初期化、DirectSound相当の初期化、DirectInput相当の初期化。
- 停止理由: `data/wav/plst00.wav` と `data/text.anm` が無い。いずれも意図どおりrepo/EBOOTに含めていない原作データである。
- 判定: P2の「engine無改変で接続し、データ欠損を安全に終了する」予選を通過。実機判定は未実施。

この記録はPPSSPPの結果であり、描画性能・入力・音声・MEの実機確認を意味しない。
## 2026-08-04 — 実機初回黒画面への起動診断追加

- 症状: 初回EBOOTは実機で黒画面の後にXMBへ戻った。原作データ未配置時にengineが
  終了することとは区別できず、相対ファイルパスの実機CWDも未保証だった。
- 過去実績: `pmd_psp` / `psp-pmdvis-public` は `ef0:` を検出し、`ms0:`/`ef0:`の
  絶対パスでファイルとログを扱う。HOME callbackも即時 `sceKernelExitGame()` ではなく
  `running = 0` にする。
- 対応: `psp/fileio.cpp` の `--wrap=fopen` で全engine相対パスをEBOOTディレクトリへ
  解決。起動ログと事前の単色画面を追加し、engineが返った時は3秒だけ結果画面を残す。
  engine (`src/`) は未変更。
- 静的確認: 最終ELFで `FileSystem::FopenUTF8`、`SDL_RWFromFile` が
  `__wrap_fopen` を呼ぶことを確認。
- 実機再確認待ち: `H:\TH06PSP_BOOT.LOG` の内容と単色画面の有無。

## V-002: 起動診断層をPPSSPPで実証 (2026-08-04)

- 実行環境: PPSSPP v1.20.4 portable、`memstick/PSP/GAME/TH06PSP/EBOOT.PBP`。**実機ではない。**
- 実行物: 本体版 EBOOT.PBP 2,192,172 bytes /
  SHA-256 `95d6a91c75cfd24f819035c740a47d692fecfd54d64c98c500d9d4247f7ba4ef`
- 得られた `TH06PSP_BOOT.LOG` (時刻はms):

```text
[    21] ms0:/PSP/GAME/TH06PSP
[    32] BUILD Aug  4 2026 02:12:10
[    44] CLOCK 333MHz BUS 166MHz
[    56] SYSMEM LEFT 760KB MAXBLOCK 512KB
[    68] HOLD L+R+START TO QUIT
[    79] ENGINE ENTER
[   101] FOPEN 東方紅魔郷.cfg OK
[   126] FOPEN 東方紅魔郷.cfg OK
[   139] SDL INIT
[   151] SDL INIT
[   163] SDL CREATEWINDOW
[   175] SDL CREATERENDERER
[   188] SDL CREATETEXTURE
[   204] SCREEN HANDED TO ENGINE
[   243] SOUND INIT OK
[   254] INPUT INIT OK
[   275] FOPEN 紅魔郷IN.DAT NG
[   322] FOPEN 東方紅魔郷.cfg OK
[   345] FOPEN ./log.txt OK
[   357] ENGINE RETURN 0
```

- 判定: config読込・window/renderer/texture生成・音声初期化・入力初期化まで到達し、
  停止理由は原作データ `紅魔郷IN.DAT` の不在である (意図どおりrepo/EBOOTに入れていない)。
  engineの `log.txt` も同じことを言っている
  (`error : Sound ファイルが読み込めない data/wav/plst00.wav` / `data/text.anm が読み込めません`)。
- 併せて確認したもの:
  - ログのms刻印と、`TH06PSP_BOOT_PREV.LOG` への1世代退避 (2回目の実行で確認)。
  - `th06_breadcrumb` 経由の engine stage (`SOUND INIT OK` / `INPUT INIT OK`) がPSPログに出ること。
    `CHAIN INIT OK` が無いのは `Supervisor::RegisterChain()` がデータ不在で失敗したためで、
    `log.txt` と整合する。
- **未確認 (実機でのみ判定できるもの)**: 画面overlayの実描画、脱出コンボ、実描画性能、音声、入力。

## V-003: boot watchdogの動作実証 (2026-08-04、PPSSPP)

- 実行物: `make -B BOOT_PROBE=1 SELFTEST=1` — main threadをkernel呼び出しの無い無限ループにする。
- 結果:

```text
[    91] SELFTEST: DELIBERATE HANG
[  5330] WATCHDOG TIMEOUT 5s AFTER: SELFTEST: DELIBERATE HANG
```

- 判定: 暴走中のmain threadをwatchdog thread (優先度0x12) が確かに横取りし、
  最後のstage名を添えて記録した。engine側が完全に止まってもXMBへ戻せる経路が成立する。
- 4x6フォントは別途ホスト上でPPMへ描いて全グリフを目視確認した (`/` `.` `W` 等の欠落を修正済み)。

## V-004: GE描画・fog・静止surface・日本語文字 (2026-08-04、PPSSPP)

- 実行環境: PPSSPP v1.20.4 portable。ユーザー所有の原作データと`msgothic.ttc`は
  外部`memstick/PSP/GAME/TH06PSP/`へ置き、repo/EBOOTへは含めていない。
- 実行物: EBOOT.PBP 2,952,228 bytes / SHA-256
  `6f2bde8bfb1561d09f2986d569c737b2104602375b62584df56880368c555386`。
- GEの最初の失敗: float UVへtexture pixel寸法を掛け、atlasが反復して横線状に崩れた。
  PSPSDK sample同様のnormalized UVへ直し、色のR/B反転も境界変換で解消した。
- fog: TH06の前方+Zに合わせ`(-near,-far)`をGEへ渡した。stage 3図書館の霧をユーザーが確認。
- 日本語: EBOOT内に`FT_New_Face`が存在し、boot logに`TTF FONT OK msgothic.ttc`を記録。
  stage 3 demoの敵スペル名「霊符『夢想封印』」を画面で確認した。
- 静止surface: 変更前は起動ロゴ30.00fps。SDL surface世代cache後は起動ロゴ59.88fps。
  stage 3 demoの連続5画面は59.88 / 59.88 / 60.00 / 59.88 / 59.88fpsだった。
- 大型surface textureは論理1024x512、物理512x512/565、cache 1面512KiB。boot中に
  logoとtitle背景の2面だけ生成し、同じsurfaceの毎フレーム変換・転送は発生しなかった。
- frame先頭の全画面clearだけでは、後段のengine drawが右pillarへstage fog色を書き、title復帰後も
  変色が残った。swap直前に左右59pxをscissor clearする修正を追加。PPSSPP/実機の再確認待ち。
- preflight `--post`: 違反0、警告0。`git diff --check`: 合格。

これはPPSSPP予選の記録である。実機のframe time、RAM、入力、音声、fog、FreeType負荷は未確認。

## V-005: 初回GE実機ログと再配備前の是正 (2026-08-04)

- 実行物: V-004と同じ EBOOT.PBP 2,952,228 bytes / SHA-256
  `6f2bde8bfb1561d09f2986d569c737b2104602375b62584df56880368c555386`。
- 実機ログは`GE INIT`、`GE BACKEND OK`まで進み、`SCREEN HANDED TO ENGINE`とwatchdog timeoutを
  残さず終了した。よってengine突入前でもhangでもなく、最初のGE presentまでのcrashである。
- 同じログの先頭は`ef0:/PSP/GAME/TH06PSP`だった。メモリースティックへ配達した版でも、旧fileioが
  `ef0:`の同名ディレクトリを存在だけで優先したためで、`bgm/th06_01.wav NG`も記録された。
- XMBには`TH06PSP` 2,952,228 bytesと旧`TH06_PSP` 2,192,172 bytesの二版が同名表示されていた。
  田中の指示で両ディレクトリを削除し、ルートのboot logだけ保存した。
- 再配備前の是正: fileioは`argv[0]`の親を使う。GE color bufferは実機稼働済みPMDVISと同じ
  `draw 0 / display 1`へ戻し、first submit/sync/swapを個別記録する。PS3配布版の桜柄ICON0と
  `Touhou 6 PSP GE`の表示名で一版だけを識別可能にする。
- 判定: ここまでの実機観測は確定。是正後EBOOTの実機結果は未確認。

## V-006: 実機だけ即終了する経路の一次資料・機械語照合 (2026-08-04)

### 実機ログで狭めた区間

実機EBOOT `472be78302a43094e6216575fa294253aa0369560f9b9a092e14f52d9d4acfe0` は
`GE DUMMY IMAGE OK` まで出て、直後の `GE VIEWPORT` を出さずに即終了した。watchdog行がないため
hangではない。この区間を最終ELFの逆アセンブルで確認した。

### 提示された4候補の静的判定

- VFPUアラインメント: `lv.q` / `ulv.q` は停止区間に存在しない。行列生成・GUM呼出しより前である。
  したがって [pspsdk issue #30](https://github.com/pspdev/pspsdk/issues/30) は後段の監査項目だが、
  今回の停止命令ではない。
- VFPUスレッド属性: engineはPSPSDK crt0が作るmain thread上で直接動く。
  `psp/main.cpp` は `PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU)` を宣言し、
  最終ELFの `sce_newlib_attribute` は `0x80004000` (USER | VFPU) だった。
  [PSPSDK pspvfpu.h](https://github.com/pspdev/pspsdk/blob/master/src/vfpu/pspvfpu.h) の
  「calling threadのVFPU属性」という条件を満たす。
- double: immediate pathにはdouble helper呼出しがない。doubleはSDL audio converterに残っているため、
  [psptoolchain issue #85](https://github.com/pspdev/psptoolchain/issues/85) に従い後段でfloat化する。
- stack: [PSPSDK crt0.c](https://github.com/pspdev/pspsdk/blob/master/src/startup/crt0.c) の既定値は
  256KiB。停止区間の `GameWindow::InitD3dRendering()` のstack frameは160 bytesであり、今回の原因ではない。

### 停止区間で見つかった未定義入力

PSP backendの初期 `GetViewport()` はpanel全体 `(0,0,480,272)` を返していた。一方、engineの
`ZunViewport::Get()` はxを `u32` として `(x - 59) / (362 / 640)` で論理座標へ戻す。
最終ELFでは次の命令列になった。

```text
0883dc74  lw        v0,0(sp)       # x = 0
0883dc78  addiu     v0,v0,-59      # u32 0xffffffc5
0883dd40  andi/srl/or ...           # unsigned -> float変換
0883dd64  div.s     f0,f0,f1
0883dd68  trunc.w.s f0,f0           # signed 32-bit範囲外
```

PPSSPP v1.20.4の `Core/MIPS/MIPSInt.cpp` と `Core/MIPS/IR/IRInterpreter.cpp` は、正側overflowの
`trunc.w.s` を `INT_MAX` に明示的に飽和する。このためPPSSPPでは同じ不正値が停止にならない。
[PSPSDK pspfpu.h](https://github.com/pspdev/pspsdk/blob/master/src/fpu/pspfpu.h) は
FCR31のinvalid-operation enable/cause bitを定義する。この時点では例外を隠さず、backendの
初期viewportをengineが実際に設定する物理4:3領域 `(59,0,362,272)` と一致させた。`src/`は未変更。

### 2026-08-04: Windows/PPSSPP/実機のFPU環境差を入口で解消

その後、実機だけでゼロベクトル正規化による除算例外がもう一件発生し、ルーミア周辺でも同系統を
疑う停止が続いたため、個別式ではなく実行環境を一次資料から再調査した。

- PSP実機出力を基準にする `pspautotests/tests/cpu/fpu/fcr.expected` の初期FCR31は
  `0x00000e00`。enable field（bit 7から）をPSPSDKの定義で読むと、overflow `0x04`、
  divide-by-zero `0x08`、invalid operation `0x10` が有効である。
- PPSSPPのMIPS全体のresetは `Core/MIPS/MIPS.cpp` でFCR31を `0` にするが、PSP threadの
  初期化は `Core/HLE/sceKernelThread.cpp` で実機同様の `0x00000e00` を設定する。今回の
  PPSSPP起動ログでも変更前は `0x00000e00` だった。一方、FPU interpreterはenable fieldを
  trap判定に使わず、NaN/Infと正側 `trunc.w.s` overflowを整数範囲へ明示的に丸める。
- MicrosoftのWindows浮動小数点環境の資料は、プロセス初期状態で全FPU例外がmaskされ、
  異常演算は例外ではなくNaN/Infinityになると明記する。これはTH06原作側の前提である。
- PSPSDKは `pspSdkDisableFPUExceptions()` を公式に提供し、GUの `spharm` サンプルも
  `sceGuInit()` より前に同じ処理を呼ぶ。実装はFCR31のcondition bit 23だけを残して他を消す。

したがってPSP入口 `psp/main.cpp` の最初で `pspSdkDisableFPUExceptions()` を呼ぶ。FCR31は
thread contextの一部なので、floatのfade/gain計算を行うBGM workerも
`psp/SoundPlayerPsp.cpp` のthread entryで同じ設定にする。
これはルーミア個別の原因を確定したという意味ではない。Windows原作・PPSSPP・PSP実機で異なっていた
浮動小数点例外方針を一致させ、同じ種類の実機専用停止を上流で防ぐ変更である。起動ログの
`FPU FCR31 before>after` で実機値と変更結果を一度だけ記録する。ゼロ除算・非有限値の局所guardは、
NaNをGEへ渡さないため削除しない。

### FPU互換版の判定

- 通常版 `EBOOT.PBP`: 2,997,618 bytes / SHA-256
  `05321f74deda7096319c55c7cca11e4157e05928e259ee485c99f45c65420b94`
- `psp_preflight.sh --post`: 違反0、警告0。
- 最終ELFを逆アセンブルし、mainの最初と `SoundPlayer::BackgroundMusicPlayerThread()` の最初に
  `pspSdkDisableFPUExceptions()` 呼出しが各1本あることを確認した。
- PPSSPP v1.20.4の通常版起動ログは `FPU FCR31 00000E00>00000000` を記録し、GE first swap、
  sound/input、DAT、chain init、`BGM OPEN` まで通過した。保存ログは
  `backups/PPSSPP_FPU_MASK_BOOT_20260804.LOG`。PPSSPPは旧版でもFPU trapを起こさないため、
  これは回帰確認であってルーミア停止解消の証明ではない。最終判定は実機で行う。
- `REAL_th6_01.rpy` は意図的にゲームオーバーになる一面リプレイであり、一面突破・stage transitionの
  oracleには使わない。ルーミア付近までの再現にだけ使える。PPSSPP検証ディレクトリの
  `replay/th6_01.rpy` は元の複数面版へ復元した。

### 修正版の判定

- EBOOT.PBP: 2,964,994 bytes / SHA-256
  `666faba4dc93546dcb786fcbaebfa12d29d75167bacbe345473a2b8eff0c4e2c`
- preflight `--post`: 違反0、警告0。
- PPSSPP v1.20.4: `GE VIEWPORT 59,0 362x272`、first swap、sound/input、DAT、chain initまで通過。
  PPSSPPは旧版も通るので、これは回帰確認であり修正成立の最終証明ではない。
- PSP実機: `H:\PSP\GAME\TH06PSP\EBOOT.PBP` へ上記SHAを照合して配達した。旧版は
  `GE DUMMY IMAGE OK` の次で即終了したが、修正版は次を記録した。

```text
[   638] GE DUMMY IMAGE OK
[   656] GE VIEWPORT 59,0 362x272
[   672] GE CLEAR ENTER 3
[   688] GE FIRST SWAP ENTER
[   705] GE FIRST SUBMIT
[   721] GE FIRST SYNC OK
[   741] GE FIRST SWAP OK
[   758] SCREEN HANDED TO ENGINE
[   776] SOUND INIT OK
[   790] INPUT INIT OK
[   817] FOPEN 紅魔郷IN.DAT NG
[   966] ENGINE RETURN 0
```

  即終了は解消し、GE first swapより後へ進んだ。今回XMBへ戻ったのはcrashではなく、実機側の
  EBOOTディレクトリに外部原作DATをまだ配達していなかったためのengine正常returnである。
  旧版と修正版の停止位置および単一のviewport初期値変更が対応するため、初期viewportの
  out-of-range変換を実機即終了の原因と判定する。

## V-007: 外部原作データの実機配達をUSB I/O障害で中断 (2026-08-04)

- V-006後のXMB returnは、実機EBOOTディレクトリに外部DATが無いことが直接原因だった。
  PPSSPP用の外部データ（DAT 6本、外部font、WAV 16本、合計約300MiB）から、EBOOTへ埋め込まず
  実機ストレージへ配達を開始した。
- Windows System event logは配達時刻に、`SONY "PSP" SS USB Device` (`H:`) に対する
  disk Event 51を連発し、Event 153でLBA `0x407c80` / `0x409340` のI/O retryを記録した。
  `H:` は書込み中に消え、数秒後に再列挙された。単なるdrive letter誤認ではない。
- 書込みなしの `chkdsk H:` はFAT32のfile/directory構造を最後まで検査し、問題なしと報告した。
- SHA-256照合済み: DAT 6本 (`CM/ED/IN/MD/ST/TL`) と `msgothic.ttc`。
  WAVは転送セッションを中断したため、全体照合が済むまで配達済みと扱わない。
- 判定: 反復書込みはFAT破損リスクを増やすため停止。ケーブルまたはUSB portを変更して接続が
  安定した後、WAVを1本ずつ転送後SHA照合して再開する。その後、ユーザー確認により切断原因は
  PSP Goの電池切れと判明した。

## V-008: PSP-3000メモリースティックへ開発版一式を配達 (2026-08-04)

- Windowsの機器情報で `G:` を `SONY "PSP" MS USB Device`、容量2,015,193,600 bytesとして識別した。
  PSP Goの `SONY "PSP" SS` 内蔵ストレージとは別機器である。
- EBOOT.PBP 2,964,994 bytes / SHA-256
  `666faba4dc93546dcb786fcbaebfa12d29d75167bacbe345473a2b8eff0c4e2c` を転送後照合した。
- 外部原作データはEBOOT/repoへ同梱せず、同じGAMEディレクトリへDAT 6本、`msgothic.ttc`、
  WAV 16本を配置した。23ファイル合計330,706,458 bytesを各ファイル転送後SHA-256で照合した。
- `psp_go.sh --drive G` がdrive letterだけでPSP Goと誤認してARK-4警告を出したため、警告条件を
  明示的な `--go` / `--all` に限定した。`--drive` はその場で機器確認済みの経路として扱う。
- PSP-3000実機での起動結果待ち。

## V-009: PSP-3000初回本編起動、BGM是正、runtime診断追加 (2026-08-04)

- ASCII filesystem境界版でPSP-3000はDAT、font、title BGMを読み、本編画面まで起動した。
  旧設定は `musicMode=2` (MIDI) だったが、PSP版のMIDI deviceはPCMを生成しないため無音だった。
  また外部WAVは`th06_02.wav`以降しかなく、titleが要求する`th06_01.wav`も欠けていた。
- PS3実績資産の`th06_01.wav` (11,206,778 bytes / SHA-256
  `e14a4c1604300d7790b73d47a30f3475b696f09d038d1a6bb8d8a2fa8e253bfb`) は、既存`th06_02.wav`と
  同じ資産系列であることをSHA比較した。repo/EBOOTへ入れず、PPSSPP/実機の外部`bgm`へ配達した。
  設定は元を`.MIDI.BAK`へ残してWAVへ変更。PPSSPPログで
  `BGM OPEN bgm/th06_01.wav 10944KB C128KB`を確認した。
- PSP-3000初回本編ログは`BOOT PHASE END`で止まり、その後ユーザー観測は「重い、PSボタン無反応、
  強制shutdown」。60-frame値は3.1fpsだったが、測定窓にfont生成約15秒を含み定常fpsではない。
  runtime情報が存在しなかったため、この時点では重さと終了不能の原因を断定しない。
- 追加した低頻度診断:
  - 5秒ごとにpresent frame増分、audio queue/underrun/read/write cursor、free/max memory。
  - すべてのBGM open/close/invalid。
  - HOME callback thread create/start、callback register、wake、exit request、exit call return。
  - 診断/脱出threadをaudio outputより高優先度化。L+R+START約0.8秒の脱出を維持。
  - runtimeで30秒present進捗がなければ記録して自動exit。
  - kernel semaphoreで複数threadのログ1行を直列化。
- PPSSPP v1.20.4で診断版を検証。callback register `R0`、BGM open、heartbeatを確認し、
  boot後の5秒窓は`+300/+301 frames` (約60fps)。ログ行の混線修正後も起動した。
- 実機配達物: EBOOT.PBP 2,966,594 bytes / SHA-256
  `df75c45ed6e5acf45da860242c0e8e9ca1557bb4c0b4066a44d9143d7d92f2df`。実機再測定待ち。

## V-010: タイトル無操作デモ遷移を停止点として確定、archive/GE境界を追加 (2026-08-04)

- V-009診断版のPSP-3000実機ログは、タイトル定常区間で5秒あたり`+287/+287 frames`
  (約57fps) を記録した後、`F724`で途絶えた。OOMを意味しないkernel側残量表示は全窓で
  `MEM700/512KB`のまま、audio queueも空ではなかった。
- `MainMenu::OnUpdate`の無操作デモ遷移条件は`idleFrames >= 720`。遷移時に
  `data/demo/demo00.rpy`を指定してGameManagerへ移る。実機の最終frame `724`と一致するため、
  「通常タイトル描画中」ではなく「無操作デモ遷移中」で停止したことを確定した。ただし、
  遷移内部のどのasset/登録処理で停止したかは旧ログからは判別不能。
- `src/`を変更せず、GNU ldの`--wrap`で`Pbg3Archive::Load`と
  `Pbg3Archive::ReadDecompressEntry`をPSP側から挟んだ。runtimeの各archive load/decodeについて
  `ARC ... BEGIN/END`、64KiB以上のC++ newとGE texture確保について`LARGE NEW`/`GE TEX`を記録する。
  BEGINだけで終わればその処理内、END後なら次の処理と機械的に判定できる。
- PPSSPP v1.20.4で無操作720frame越えを実行。`CM/ST` archive、`demo00.rpy`、stage4 background、
  player、bullet、enemy、ECL、effect、GUI/messageをすべてBEGIN/END対で通過し、F1059以降も
  5秒あたり`+300 frames`へ復帰した。デモ遷移時のGE texture使用量はログ上
  `1440KB -> 4128KB`へ増加した。これはPPSSPP回帰確認であり、実機成功の証明ではない。
- 実機診断版: EBOOT.PBP 2,967,506 bytes / SHA-256
  `52aacd2cee4f601dcf318b7c99e4007d8aae5eb46bfd4f16bd09701f7e35b889`。
  `G:\PSP\GAME\TH06PSP\EBOOT.PBP`へ転送後SHA-256一致を確認した。

### V-010実機結果

- PSP-3000では`demo00.rpy`から`msg4.dat`まで全archive entryがBEGIN/END対で成功し、
  64/128/256KiBのC++ allocationおよびGE texture allocationもすべてOKだった。最終記録は
  `GE TEX END 256x256 OK T4128KB`。従ってarchive decode失敗、asset欠損、当該allocationのOOMは除外。
- frameは`F724`のまま停止し、その後30秒待っても`RUNTIME STALL`も`EXIT REQUEST`も記録されず、
  XMBへ戻らなかった。main threadだけの通常hangなら高優先度diag threadが記録するはずなので、
  runtime watchdogが動けないprocess-wide停止/crashとして扱う。kernel側残量`MEM700/512KB`は
  newlibがheapを確保した後の値であり、heap空き量でもOOMの証拠でもない。

## V-011: デモ登録後のcalc/draw/GE一回目を分離 (2026-08-04)

- runtime stall閾値を30秒から8秒へ短縮。ただしV-010ではdiag thread自体が動かなかったため、
  同型停止からの自動復帰を保証する変更ではない。
- `Chain::RunCalcChain`/`RunDrawChain`もPSP linker wrapで前後を記録し、デモtrace中の最初の
  transform/clear/draw/swapを一度だけ記録するようにした。`src/`は未変更。
- PPSSPP v1.20.4でデモ遷移を再検証。asset終了後の正常順序は
  `TRACE GE CLEAR -> CHAIN CALC END R11 -> TRACE GE SWAP BEGIN/END -> CHAIN DRAW BEGIN ->`
  `TRACE GE TRANSFORM -> TRACE GE DRAW -> CHAIN DRAW END R13`だった。
- 実機配達物: EBOOT.PBP 2,968,418 bytes / SHA-256
  `e791982783b4f5c01c65ea59f1f6e8bc495e714053a2eeb4a6083d703817898c`。
  `G:\PSP\GAME\TH06PSP\EBOOT.PBP`へ転送後SHA-256一致を確認した。

### V-011初回実機結果とwatchdog是正

- 8秒版はXMB復帰に成功したが、ログでは`face08a.png`処理中にもarchive/texture処理が進み続けていた。
  `F724`から8秒というframeだけの条件が、正当な長時間asset登録をhangと誤判定したもの。
  `RUNTIME STALL 8s`の直後にもmain threadはallocationを完了しており、故障箇所の証拠には使えない。
- archive decode、64KiB以上のallocation、GE texture allocationを明示的なruntime progressとし、
  frameまたはこのprogressのどちらかが動けばstall時計を更新するよう変更。heartbeat/log自身はprogressに
  含めないため、本物の停止を永久にpetすることはない。
- PPSSPPで再検証。デモasset登録が8秒を越えても誤終了せず、全asset、CALC、SWAP、DRAWを通過後、
  `RUN F1048 +300`へ復帰した。
- 進捗連動8秒版: EBOOT.PBP 2,968,658 bytes / SHA-256
  `3bbbab68a05bbc9b4d9399f7e53987b6871c2d730dc6f6709f7e054a1b70e974`。
  `G:\PSP\GAME\TH06PSP\EBOOT.PBP`へ転送後SHA-256一致を確認した。

### 進捗連動版の実機結果

- PSP-3000でも全archive decodeと最後のGUI用256x256 texture allocationまでは成功したが、
  最終行は再び`GE TEX END ... T4128KB`で、次の`TRACE GE CLEAR`へ到達せずprocess-wide停止した。
  進捗連動watchdogも動けなかったため、Memory Stick読込速度やarchive処理ではなく、最後の
  texture allocation直後から次のGE clearまでが停止区間。

## V-012: RGBA8 texture upload高速経路とupload内分割 (2026-08-04)

- 従来の`GE TEX END`はmemalign成功直後であり、その後に全領域memset、1 pixelずつの
  RGBA read/write変換、dcache writebackが残っていた。実機停止区間はこの内部を含んでいた。
- `PIXEL_RGBA + PIXEL_UNSIGNED_BYTE + GU_PSM_8888`かつstorageにpadding/縮小がない場合、
  sourceとGE textureは同じRGBA byte layoutなので、二重走査をやめて境界付き`memcpy`を使う。
  それ以外は従来変換を維持。uploadを`BEGIN/COPY|ZERO|CONVERT/CACHE/END`へ分割し、source/dest
  pointerも記録する。
- PPSSPP v1.20.4で最後のGUI atlasを含む全uploadが`COPY OK -> CACHE -> END`を通り、
  CALC/SWAP/DRAWへ進むことを確認。
- 実機配達物: EBOOT.PBP 2,969,634 bytes / SHA-256
  `9802d8be362e96b132a4d1aa3daf10b63c3b1d8706b172a11c603c8b44ebddad`。
  `G:\PSP\GAME\TH06PSP\EBOOT.PBP`へ転送後SHA-256一致を確認した。

### V-012実機結果 / V-013 GUI後段境界

- PSP-3000では最後のGUI atlasを含む全uploadが`COPY OK -> CACHE -> END`を完走し、旧版より
  後ろへ進んだ。ただし`TRACE GE CLEAR`には届かずprocess-wide停止。RGBA upload内部は除外した。
- GameManagerの残りは`Gui::RegisterChain`のreturn、各状態field設定、
  `AsciiManager::InitializeVms`、returnのみ。`Gui::RegisterChain`と`AsciiManager::InitializeVms`を
  linker wrapし、BEGIN/ENDを追加した。PPSSPPで正常順序
  `GUI REGISTER END -> ASCII INIT BEGIN/END -> GE CLEAR -> CALC END -> SWAP -> DRAW`を確認。
- 実機配達物: EBOOT.PBP 2,969,906 bytes / SHA-256
  `f474038f42ead35f052cd4e61c44b8c1ed56889d2d5edbeba99e558b9a3d8a1d`。
  `G:\PSP\GAME\TH06PSP\EBOOT.PBP`へ転送後SHA-256一致を確認した。

### V-013実機結果 / V-014 eDRAM実測と初回camera入力 (2026-08-04)

- PSP-3000では`GUI REGISTER END`、`ASCII INIT END`、GameManagerのcalc/draw chain登録returnまで完走。
  stack freeは一貫して236KiBだった。その後、同じ`Chain::RunCalcChain`内の
  `GameManager::OnUpdate`から最初のbackend `Clear`へ入る前にprocess-wide停止した。従って停止区間は
  asset、GUI/ASCII登録、stackではなく、初回camera/viewport設定へ狭まった。
- 旧ログの`T4128KB`は`PspGuGfx::mTextureBytes`、すなわち`memalign`したmain RAM textureの
  生存bytes累計だった。実アドレスも`0x09xxxxxx`で、eDRAMの`0x04xxxxxx`とは別領域。
  誤読を防ぐため表示名を`RAMxxxxKB`へ変更した。
- PSPSDK `pspge.h`の`sceGeEdramGetSize()`で実値を取得。PPSSPP v1.20.4では2,048KiBだった。
  GE固定配置は512x272x32bitのframebuffer 2面と512x272x16bitのdepthで
  1,392,640 bytes (1,360KiB)、残り688KiB。textureを`RAM1440KB -> RAM4128KB`まで積んでも
  eDRAMは全allocationで`E1360/2048KB`のままなので、4MiB境界越えという仮説は否定された。
  `sceGeEdramSetSize()`は従来も現在も呼んでいない。2MiBで固定配置が収まり、PSP-1000では
  4MiB指定がエラーになるため、不必要な4MiB化は行わない。
- source確認では`Stage::AddedCallback`が初期camera facing `(0,0,1)`をStage内にだけ設定する一方、
  `g_GameManager.stageCameraFacingDir`はglobal constructor以来の全ゼロだった。calc優先度は
  GameManager=4、Stage=6なので、初回はStageが転記する前にGameManagerがゼロ方向を
  `createViewMatrix`で正規化する。PPSSPPログでも登録直後の生bitが
  `GM CAMERA RAW 00000000 00000000 00000000`と実測された。
- `src/`は変更せず、GameManager登録のPSP linker wrapperで全ゼロの初回だけz=1へ同期する。
  PPSSPPでは補正直後に`TRACE GE CLEAR -> CHAIN CALC END -> SWAP -> DRAW`を通過した。
- 実機配達物: EBOOT.PBP 2,971,250 bytes / SHA-256
  `af3fab011ef7c712e8c3a6cf7344279e95da4d4c5d3e9c3c7a93d0a4257a964d`。
  `G:\PSP\GAME\TH06PSP\EBOOT.PBP`へ転送後SHA-256一致を確認。

### V-014実機結果 / V-015 入力とframe内訳計測 (2026-08-04)

- PSP-3000実機ログで`GM CAMERA RAW 00000000 00000000 00000000`、
  `GM CAMERA INIT 0,0,1`の直後に`TRACE GE CLEAR -> CHAIN CALC END -> SWAP -> DRAW`を完走。
  その後デモを最後まで再生してタイトルへ復帰し、frame heartbeatも継続した。従って従来の
  process-wide停止原因は初回ゼロcamera方向の正規化で確定。eDRAMは実機でも
  `SIZE 2048KB USED 1360KB FREE 688KB`だった。
- 詳細診断版の実機デモ移行は34.556秒から51.054秒まで約16.5秒、タイトル復帰は
  130.548秒から136.555秒まで約6.0秒。各診断行が同じMemory Stick上のログを
  open/write/closeするため、asset/BGM I/Oへ約200回割り込んでいた。原因確定後は
  per-asset/texture/upload診断を既定OFFとし、`DEMO LOAD END <ms> A<count> <KB>`の集計1行へ変更。
  PPSSPPでは同じデモloadが詳細診断時約4.7秒から1.590秒へ短縮した。実機再測定待ち。
- PSP padからSDL scancodeへの変換は既に`psp/input.cpp`にあったが、engineの
  `Controller::GetInput()`が毎frame `SDL_GetKeyboardState()`を呼ぶ分岐は`TH06_PS3`限定だった。
  PSPでは起動時に取得した全ゼロ配列を読み続けていた。PSP linker wrapperでengine input sample直前に
  pollする。PPSSPPへDOWNを700ms入力し、`INPUT FIRST B00000040 A128,128`とデモからタイトルへの
  復帰を確認した。
- ME/GE判定用に300frame窓の低頻度`PERF`を追加した。`FPS / CPU(frame開始からsubmitまで) /
  GE(sceGuSync待ち) / VB(vblank待ち) / draws per frame / vertices per frame`を記録する。
  PPSSPPデモ定常値の一例は`59.9FPS CPU6.3ms GE0.0ms VB10.2ms D29 V2349`。
  これは実機性能判定には使わず、計測器の回帰確認だけとする。
- 入力・計測版の実機配達物: EBOOT.PBP 2,973,074 bytes / SHA-256
  `3031b06b2922e649db7eff5ada21997577f2b508361e0c81c1877cb53e1c27b7`。
  `G:\PSP\GAME\TH06PSP\EBOOT.PBP`へ転送後SHA-256一致。

### V-015実機結果

- PSP-3000で十字/analog、決定、ショットを使って通常ゲームを操作できた。最初の実入力は
  `INPUT FIRST B00002000 A140,130`として記録された。
- 代表的な300frame窓は次の通り。`CPU`はframe開始からsubmit、`GE`は`sceGuSync`待ち、`VB`は
  vblank待ち。値は1frame平均。

```text
ロゴ/タイトル30fps: 31.0FPS CPU1.0ms  GE16.5ms VB14.4ms D7  V142
複雑な背景30fps:   29.9FPS CPU7.4ms  GE13.4ms VB12.3ms D49 V1939
単純な背景60fps:   59.3FPS CPU10.9ms GE2.1ms  VB3.5ms  D29 V3926
単純な背景60fps:   59.3FPS CPU13.2ms GE2.1ms  VB1.3ms  D30 V4897
```

- CPUは軽い背景でも10〜13msあるが、背景変化に伴ってGE待ちが約2msから13〜20msへ増えた時に
  30fps化している。ロゴ/タイトルはdraw 5〜7、vertex 100前後でもGE待ち15〜16msなので、
  弾座標計算やdraw-call数では説明できない。現段階ではME offloadよりGE texture sampling/
  bandwidthの是正を先にする。
- backendはsource形式に応じ8888/5650/5551/4444を既に使い分けるが、全textureが
  `sceGuTexMode(..., GU_FALSE)`のlinear配置で、swizzle未実装。fullscreen surface cacheは5650だが
  512x512 linear textureである。次の安い比較対象はstatic/fullscreen textureのswizzle化。
- 詳細診断OFF後、タイトル`MainMenu::RegisterChain`実測は329ms。旧詳細版のタイトル復帰約6秒は
  per-assetログI/Oを含む値で、通常版のasset load時間ではない。一方、復帰演出中にも
  `CPU2.7ms GE19.8ms`、`CPU8.7ms GE17.5ms`の約30fps窓があり、見えている遷移の重さもGE側。

### V-016 swizzle、音声watermark、出力layout (2026-08-04)

- PSPSDK GU sampleと同じ16-byte x 8-row blockでmain-RAM textureをswizzle化。PSP-3000の定常本編で
  GE waitは変更前約2.1～13.4msから、変更後の多くの窓で0.2～0.4msへ低下した。複雑背景でも概ね
  3ms以下となり、田中の実機所感も「かなり軽くなった」「ゲームはほぼ快適」。
- swizzle後の残りは代表窓で`56.5FPS CPU13.3ms GE0.2ms VB3.8ms`。GEではなくSCが支配的になったため、
  次版で`Chain::RunCalcChain` / `RunDrawChain` / 残余を300frame窓に分離する。ME採否はこの実機値で決める。
- 音声watermark版（SHA-256
  `9586ed914e68e2fc0c40dd68522fba9276e680b111c1eafcacc6c44943038870`）のPSP-3000ログでは、起動後
  約194秒で追加underrun 10、drop 0。underrun窓のBGM read最大値は68～228ms、非underrun窓は概ね
  16～51msだった。旧版の約192秒/追加955 blocksから大幅に減り、田中の聴感確認は「OK」。
- FULL STRETCH / 4:3 FIT、静止surface一段bilinear縮小、無入力時power tick版はPPSSPP v1.20.4で
  480x272全面描画と`GE SURFACE 640x480>480x272`を確認。実機配達物は2,976,978 bytes / SHA-256
  `e7bd4ac90b677861920997e673ba453d377a2c8b4cc558648985961bd78683b4`。実機の画質・性能・消灯防止は
  再確認待ち。

### V-017 PSPPMD ME音声版のPPSSPP退避 (2026-08-04)

- PSPPMD公開版commit `18fb0b1`が同梱する`libme-core.a`を無改変でvendorした。双方のSHA-256は
  `d87cbffdaf685b5229a77cc1873fcbadc8140555eef1e53fdd0ad1b1a11c1f71`で一致し、TH06と
  PSPPMDのELFには`kcall_2/3`、`kinit`、embedded PRX、`meLibDefaultInit`が同型で入っている。
  TH06独自のraw kcall呼出しはない。
- PPSSPP v1.20.4へ最初のME版を入れると、MECCが`kcall.prx`を展開した後、
  `meLibDefaultInit()`から戻らず`SOUND INIT`へ進まなかった。ME処理を実行できないだけでなく、
  初期化失敗がreturnするという仮定も成立しない。
- `ms0:/PSP/SYSTEM/ppsspp.ini`がある時はMECCへ入る前にSC mixerを選ぶ。再試験では
  `ME AUDIO OFF (PPSSPP -> SC) -> SOUND INIT OK -> INPUT INIT OK`を通り、原作archive、font、
  BGMを開いて60frame boot診断を完走した。`ME AUD J0 SC357 TO0 W0us`も確認。
- MEローカルeDRAMはPSPPMDと同じ`0x00000400`を開始点にする。TH06の最大8KiB accumulatorは
  `[0x400,0x2400)`、MECCの最小stack topはPSP-1000の`0x200000`。compile-time assertionで
  footprintを固定した。実機のME self-test・連続音声は未確認。
- 検証EBOOT: 2,992,562 bytes / SHA-256
  `a51eee5476f28955f408e48d5e30b84fd9ff5f91bd1a5f7e58ff59bb9e624ab4`。PPSSPPの
  portable memstick側でもhash一致。実機未配達。

### V-018 音声同期に見えた描画破壊はGU_DIRECT頂点cache (2026-08-04)

- ME有効版で「音に合わせて描画が壊れる」。MEを初期化しない安全A/B版でも同じ破壊が再現し、
  実機ログは`ME AUDIO OFF`、全窓`ME AUD J0`だった。従ってME local eDRAMとME処理は除外。
- 新音声変更を含まないSHA `1c3451...`へ戻しても再現。この版にも入っていた共通差分は、
  vertex arenaのdcache writebackを毎drawから`sceGuFinish()`直前へ遅らせた最適化だった。
- さらにその最適化前のSHA
  `2bc944f28a156cbbae6c1974ac1b829a456ea3a0f34b90e7f3dcc21f3f083d23`へ戻すと、
  PSP-3000実機で田中が「壊れない」と確認。原因を頂点cacheの公開順序に確定した。
- `GU_DIRECT`ではGEがlive listを消費する。各drawの頂点を書いた直後、draw commandを積む前に
  `sceKernelDcacheWritebackRange(vertices, required)`を行う元の契約へ復旧。frame末尾一括反映は
  再採用禁止。音声threadはGEの先行タイミングを変えて症状を同期させただけで、破壊元ではなかった。
- 復旧ソースのPPSSPP v1.20.4回帰はarchive/font/BGMを開き、60frame提示まで完走。実機候補は
  2,992,242 bytes / SHA-256
  `f5e337e41402b933d4c017fc41775a0ad0cd019743f3c422fd2c20967e4ebaaa`。MEはmarkerなしでOFF。

## V-019 ME実働、Rumia撃破境界、周期ログ退場 (2026-08-04)

- PSP-3000実機のSHA-256 `2bd114411704e97fab60f4a17717c6472509ff849b1cb8548780c264462e1f7e`
  のログを回収。`ME AUDIO INIT R2 -> ME AUDIO ON MAP2`の後、5秒窓ごとのME jobは412～537、
  全窓`SC0 TO0`だった。MEは実行され、SC fallback/timeoutはともに0。
- 同じログの音切れはunderrun累計39。BGM read最大値は35～296ms、producer gap/mix最大値とほぼ一致。
  ME計算時間ではなく、mixer thread上の同期Memory Stick read中に69.7msのPCM watermarkが尽きている。
- Rumia撃破報告時の最終行は178.730秒の通常音声統計で、`STAGE END SIGNAL`、BGM close、stage reinitは
  一行もない。次stage loadではなく、boss撃破frameのEnemy/effect処理より前に停止している。
- ABI監査で、GNU ld `--wrap`は別objectからのcallを正しく差し替える一方、`EffectManager.o`内の
  `g_Effects` callback表と各manager object内のstatic chain elementは元関数アドレスを保持することを
  ELF relocation/disassemblyで確認。従来の`EffectUpdateCallback4`ゼロ軸保護と`Gui::OnUpdate`境界は
  実際には呼ばれていなかった。
- `Chain::AddToCalcChain/AddToDrawChain`のPSP wrapperでGui/Enemy/Effect callbackを登録時に差し替え、
  effect ID 13～15生成後はcallback4を安全wrapperへ置換する。致死damage時だけboss death traceを2frame
  有効にし、弾消去・粒子・SE・death ECL・effect update/drawをbegin/endで記録する。
- 通常版は5秒周期の5行をMemory Stickへ書かない。`TH06PSP_DIAG.ON`がある専用計測時だけ復活。
  BGM cacheは384KiBへ増量。PPSSPP v1.20.4では`C384KB`、MEのSC退避、60frame提示を確認。
- 実機配達物: EBOOT.PBP 2,996,082 bytes / SHA-256
  `f218f96d545f58ad7957687c41633fd9a38af73c945d71e1fd2b93ff8cec086b`。
  `G:\PSP\GAME\TH06PSP\EBOOT.PBP`へ転送後SHA-256一致、周期診断markerなしを確認。

### V-019初回実機結果 / 診断退行の即時是正

- 初回実機runは道中からMemory Stick lampが連続点灯して激重になり、Rumia戦途中で終了した。
  回収logは59,354 bytes / 1,775行で、そのうち1,466行が`BOSS` trace。周期5行は0で、周期ログ停止は
  機能していた。
- boss scriptは無敵遷移中に`life=1`、damage受付flag `unk10=0`を使う。traceの予測はdamage量とlifeだけを
  比較していたため、実際にはEnemyManagerがdamageを引かないhitまで致死と誤認し、数秒おきに4frameの
  begin/end記録をFATへ書いた。激重とlamp連打は診断コードが作った退行。
- 致死判定をengineの実条件`active && unk6 && unk10`へ一致させ、traceを2frameへ短縮。修正版は
  EBOOT.PBP 2,996,082 bytes / SHA-256
  `cf96032da69dbf974dcf6ceeae71ed5334f372f17ea879e198a054197edea9eb`。
  G:配達後SHA-256一致を確認した。初回退行版とlogは`backups/`へ保存。

### 通常版からboss traceを完全退場

- 修正版でも本物の中boss撃破時には2frame分のbegin/endを同期FAT記録するため、撃破直後の重さを
  診断自身が作り得る。通常版では`gBossDeathTraceFrames`を一切armせず、callback4補正の通知も書かない。
  `TH06PSP_DIAG.ON`を明示配置した専用runだけ詳細traceを有効にする。callback4のゼロ軸補正そのものは
  ログなしで常時維持する。

## V-020 stage 1→2再現とmain-RAM texture断片化修正 (2026-08-04)

- 母艦にあった紅魔郷1.02 replay `th6_01.rpy`を復号せずheader規則どおり解析し、Normal/ReimuA、
  stage offset `80,4944,11184`（stage 1～3収録）を確認した。原作replayはrepoへ入れず、PPSSPPの
  外部memstickにだけ置いた。
- `STAGE_SOAK=1`のPPSSPP専用EBOOTでtitle操作を飛ばし、このreplayへ直結した。移植版のframe cadenceで
  replayが被弾desyncするため、検証版だけplayerをinvulnerableにし、会話待ちへshoot/skipを注入した。
  通常EBOOTにはいずれも入らない。
- 修正前は`STAGE END SIGNAL OLD1`、全teardown成功、stage 2のStage/Player/Bullet/Enemy/ECL/Effect登録成功後、
  Gui登録中に`GE TEX OOM 256x256 262144B`。`GUI REGISTER END R-1`、`GM REGISTER END R-1 NOW2`で
  engineが終了した。stack freeは236KiBで一定。失敗はeDRAMではなくGE texture用main-RAM
  `memalign(16, 262144)`だった。
- stage teardownは旧textureを`free`するが、stage 2資産は別順序・別サイズで再確保されるため、最後の
  GUI atlas用256KiB連続blockが残らなかった。backendに解放texture blockのbest-fit再利用poolを追加し、
  GEが参照中のblockは従来どおりsubmit/sync後にpoolへ戻す。
- pool版（PPSSPP専用EBOOT SHA-256
  `7ea9b8c24161247cdb536146d52ffa687c315082648f2d9ff494f36e062a9a67`）は
  `GUI REGISTER END R0`、`BGM OPEN th06_04.wav`、`STAGE REINIT REGISTER END R0 NOW2`、
  `SOAK STAGE 2 ENTER`を通過。そのままstage 3、stage 4登録もR0で通った。
- 通常版のstage遷移詳細ログは`TH06PSP_DIAG.ON`時だけに限定する。通常時はstage endとreinit結果の
  最小行だけを残し、Memory Stick上でBGM readと数十回の同期log writeを競合させない。
- 通常版はtest-only文字列0件、preflight違反0/警告0。EBOOT.PBP 2,997,426 bytes / SHA-256
  `575c58c7c5f4f0f29ee125685830510550b6704f930491784eedc1e39fa5f0e6`。
  `G:\PSP\GAME\TH06PSP\EBOOT.PBP`へ転送後SHA-256一致。実機stage 1→2は未測定。

## V-021 実機stage teardown停止点とECL固定arena (2026-08-04)

- PSP-3000実機の魔理沙playはルーミア撃破後に`STAGE END SIGNAL OLD1`、`BGM CLOSE`まで通った。
  区間計測版ではStage/Bullet/Player/Enemyのcutが全て完了し、最終行は`unload_ecl BEGIN`だった。
  `unload_ecl END`は無く、停止を二面load前の`EclManager::Unload()`内へ限定した。証拠logは
  `backups/REAL_MARISA_STAGE1_LATEST_20260804.LOG`と`backups/REAL_BAD_SOAK_FREEZE_20260804.LOG`。
- PPSSPP v1.20.4の同じteardown経路は`unload_ecl END`から二面登録を通るため、実機だけで露出する
  ECL heap lifetime問題として、PSP wrapperに64KiB固定arenaと4096-entry固定sub tableを実装した。
  `src/EclManager.cpp`は無改変。stage終了時に`free()`せず、全engine-visible pointerをnullにする。
- 実機上の製品版`TH06ST.DAT`を読み取り専用で展開・検査した。各ECLのbytes/sub数は
  `24562/24, 26880/33, 33070/35, 60796/61, 30392/52, 30866/46, 59890/70`。
  全7本でheader、timeline offset、全sub offsetがファイル範囲内で、arenaのruntime guardを通る。
- 通常版はtest-only `STAGE SOAK`文字列なし、preflight違反0/警告0。EBOOT.PBPは2,998,130 bytes、
  SHA-256 `b775e7d5c62fce0de7a587f73f80960744f5cda16a0fda9b8750ec1f5ea03ad7`。
  `G:\PSP\GAME\TH06PSP\EBOOT.PBP`へ転送しSHA-256一致を確認した。
- 同EBOOTをPSP-3000実機でユーザーが通常playし、一面終了後に二面へ到達した。固定arena変更前の
  同じ実機・同じ進行では`unload_ecl BEGIN`で停止しており、変更後は一面→二面遷移を通過した。

## V-022 PSP-3000全6面clear、30fps段差、staff roll修正 (2026-08-04)

- ECL固定arena版をPSP-3000実機で通常playし、stage 1からstage 6、Remilia撃破、ending、staff roll、
  title復帰まで完走した。全stageのECL bytesは順に`24562, 26880, 33070, 60796, 30392, 30866`、
  各stage終了後の次asset/BGM openと、最後の`bgm/th06_16.wav`/`th06_17.wav`をlogで確認した。
  HOME終了も正常。証拠は`backups/REAL_FULL_CLEAR_ENDING_20260804.LOG`、SHA-256
  `8c9d211d5293ddd867055196be56c7a87a7d59adad3e75876981922c592092c0`。
- 実機報告ではstage 5/6や弾消去時の低下が約30fpsで止まる。backendは各presentで
  `sceGuFinish()`、`sceGuSync()`の後に無条件で`sceDisplayWaitVblankStart()`を呼ぶため、60Hzの
  16.7ms期限を少しでも越したframeは次のVBlankへ送られ、60から30へ量子化される。これは未知の
  30fps limiterではない。安全なdouble-buffer表示契約は崩さず、まず期限超過を作る仕事を減らす。
- `BulletManager::DespawnBullets()`は最大640発について、点itemと総bonusとは別に個別得点popupを作り、
  各popupの4～5文字を60frame描く。PSP wrapperは密集cancel時の個別popupだけを間引いて約48個へ抑える。
  得点、点item、弾状態、総bonusは変更しない。通常pathにはFAT logを追加しない。
- 実機clear logは通常ending surfaceを`640x480>480x272`、staff rollだけを
  `640x780>480x272`としていた。さらに継承surface helperは指定されたsource cropを無視し、毎frame全面UVを
  出していた。PSP wrapperでcrop UVを適用し、surface cacheは480pxを272pxにする同じ倍率のまま
  780pxを442pxへ保持する。これによりscroll用640x480窓を縦長画像から正しく切り出す。
- PS3版由来の未改変Extra/Marisa A replay（17,440 bytes、SHA-256
  `ae47eeaa959be264f56a639c0c1094fdca37ecf263cdb84bf91964e8c2a9548f`）は通常のReplay一覧用として
  `replay/th6_01.rpy`へ置く。自動起動EBOOTは要求の読み違いだったため撤去した。
- 最終配達物は通常起動、test-only `PS3 REPLAY AUTO`/`STAGE SOAK`文字列なし、post-preflight違反0/警告0。
  EBOOT.PBP 3,000,306 bytes、SHA-256
  `f00805874a1fdfc64adb2103e689a0b351d015df2c58f36d261d89c7654ac147`。G:上の通常EBOOTとreplayの
  SHA-256一致を確認した。popup低減とstaff roll表示の実機結果は未測定。

## V-023 公開用data配置、命名、配布物監査 (2026-08-04)

- 公開名を既存の類似名と区別できる`th06-psp-native`、XMB表示を`Touhou 6 PSP Native`とした。
  install folderは`PSP/GAME/TH06PSP`を維持する。
- PPSSPP v1.20.4の隔離memstickへ、利用者所有のPC版TH6 folderを
  `PSP/GAME/TH06PATHCOPY/th6`として丸ごとcopyして検証した。日本語名DATを改名せず、logで
  `DAT6/6 WAV17/17`、`ENGINE ENTER`、`FOPEN DATA 紅魔郷IN.DAT OK`、
  `TTF FONT OK NotoSansJP-Regular.ttf`、`CHAIN INIT OK`、`bgm/th06_01.wav OK`、
  `BOOT PHASE END: 60 FRAMES PRESENTED`を確認した。原作folderはrepoと配布物の外に隔離した。
- 描画実装はPSP専用backendがPSPSDK libGU/libGUMを直接呼び、D3D/OpenGL等の汎用graphics APIを
  介さない。ME audioは実機検証V-019の`ME AUDIO ON MAP2`、5秒窓412～537 job、`SC0 TO0`を根拠に、
  m-c/dの現行MECCによるPCM mixとSC fallbackをREADMEの特徴へ記載した。
- 公開EBOOTは3,003,190 bytes、SHA-256
  `bd055782722e1dd0c738fe2065ddd9b519d0d87303ad3e2ca528f6456d989fc4`。
  CP932全域subset fontは4,491,696 bytes、SHA-256
  `6ab1664d8adc20b19237ddc451c94e31f493cb851a1917242debf66f9af6da05`。
- `dist/th06-psp-native-preview.zip`は5,122,844 bytes、SHA-256
  `473de6c6dded76504ce3d55bf94e264a6b29080d2616502e96493a3a19dca9c2`。
  ZIP内EBOOTのSHA-256は単体と一致した。内容はEBOOT、README、CREDITS、GPLv3、Noto font/OFL、
  MECC licenseだけで、原作DAT/WAV/EXE、replay、score、SF2を含まない。
- `release_audit.sh`と`psp_preflight.sh --post`は違反0・警告0。通常EBOOTに
  `PS3 REPLAY AUTO`、`STAGE SOAK`、`EXACT REPLAY TRACE`のtest-only markerは無い。
  廃止済みMECC repository名はsource、文書、配布ZIPの全てで0件。

## V-024 PSP-1000 32MBでのheap観測とsurface cache退役 (2026-08-04)

- 当初回収logはtitle/demo assetを読み、`T2208KB`まで通過していたが、
  `GE TEX OOM`、`GE SURFACE OOM`、`CXX OOM`、watchdogのどれも無く、OOMと断定できなかった。
  `sceKernelTotalFreeMemSize()`はnewlib heap外の残量だったため、`mallinfo()`のarena/used/free/
  largest top chunk/free block数を起動と既存のasset境界logへ追加した。
- PPSSPP v1.20.4の`PSPModel=0`（32MB）stage soakで、未退役版のstage 1は
  `T3872KB H14602/187KB`、退役版は`T3360KB H14090KB`となり、512KiBを回収した。
  通常title demoでは`X1024KB`を回収し、title復帰後の`GE SURFACE`再生成、次のdemoの
  `X512KB`回収まで通過した。
- PSP-1000実機へ3,006,710 bytes / SHA-256
  `031da8db38e6d80eb57e7a00934c0943eef6d6484b7e0ce67684a6ab93bcc716`を配達後、田中の実機確認で
  title demoからtitleへ戻り、引き続き動作した。PSP-3000より若干遅いとの観測がある。
- Memory Stickのaccess lamp点灯と短い停止は同期して残る。通常版の周期logは無効のままであり、
  現行BGM cacheは192KiB×2をmixer threadが同期補充する。以後のI/O分離は本validationと混ぜない。
- 実機確認後のclean buildはEBOOT 3,006,710 bytes / SHA-256
  `163e4e41a9a486e84a52d4e28b1b92668a9132a2c766308419ad12b785559525`、配布ZIPは
  `1a33160f03668b9c22bb81eed242d37393eecf0f285435598cb77f5a938f3f49`。preflightは違反0・警告0、
  release auditは原作data/replay/score/廃止済みMECC名の混入が0件だった。

## V-025 PSP-1000録画領域縮小とBGM非同期prefetch (2026-08-04)

- PPSSPP v1.20.4の`PSPModel=0`通常録画testで面1・2・3を順に終了し、各面の
  `StopRecording()`後に`RC421KB`、計約1.23MiBを回収した。続く面4の
  Stage/Player/Bullet/Enemy/ECL/Effect/Gui登録と`bgm/th06_08.wav`読込は全て成功し、
  `STAGE REINIT REGISTER END R0 NOW4`まで通った。証拠は
  `backups/PPSSPP_PSP1000_RECORD_COMPACT_3STAGE_20260804.LOG`。PSP-1000実機の三面→四面は未確認。
- BGM block readを専用threadへ移したPPSSPP 32MB診断では、最大readが262,978usの窓でも
  mixer最大169us、producer gap最大2,757us、PCM queue 3,072 framesを維持し、起動後のunderrun増分は
  0だった。証拠は`backups/PPSSPP_PSP1000_ASYNC_BGM_20260804.LOG`。Memory Stick実機のlamp/stutterは
  未確認。
- block容量は192KiB×2のままで、曲開始時だけ二blockを同期充填する。定常mixer pathに
  `SDL_RWseek/read`は無く、原作WAVと一時圧縮fileを変更・生成しない。

## V-026 VFPU版libGUM予選 (2026-08-04)

- main thread属性は従来どおり`USER | VFPU`。linkを`libpspgum_vfpu + libpspvfpu`へ変更した最終ELFで
  `pspvfpu_initcontext`、`pspvfpu_use_matrices`、`sceGumLoadMatrix`を解決し、disassemblyに
  `vmidt.q`、`lv.q`、`sv.q`が存在することを確認した。
- PPSSPP v1.20.4の64MB modelで原作archive、font、title BGMを開き、初回GE submit/sync/swapと
  60frame boot診断を完走した。証拠は`backups/PPSSPP_VFPU_GUM_DEPLOYED_HASH_SMOKE_20260804.LOG`。
- EBOOT.PBPは3,010,358 bytes、SHA-256
  `0b11919fe97e73ff88038fb928488e69adbba9ad773f11834954c12bca80a27c`。G:へ転送後のSHA-256一致を確認。
  PSP-3000実機のFPS差と
  PSP-1000実機のVFPU/alignment挙動は未測定であり、PPSSPP結果を実機性能値として扱わない。

## V-027 GE command削減、BGM六分割先読み、game-over result予選 (2026-08-04)

- PSP-3000実機でstage 5死亡後にContinue「No」を選んだrunは、最後の記録が
  `BGM CLOSE R33 S0`だった。BGM closeは完了しており、その後のGameManager破棄から
  ResultScreen登録までが未観測だったため、各破棄処理、result surface、result ANM 4本を
  begin/endで囲んだ。音声は通常5秒窓をRAM flight recorderへ蓄積し、Continue/面遷移/HOME/
  watchdog時だけ`AUD!`一行へ確定する。ME timeoutのmixer hot pathからFAT書込みも除いた。
- `RESULT_SOAK=1`は通常録画gameを開始し、300frame後に実際の
  `SUPERVISOR_STATE_RESULTSCREEN_FROMGAME`へ入るPPSSPP専用test。最初のtestはtitle updateを
  省いたためTL archiveが無く、`result.jpg`不在という偽陽性になった。TL archiveを通常pathと
  同じく開くよう修正後、game teardown全工程、`result.jpg`、`result00..03.anm`、
  `RESULT REGISTER END R0`まで完走した。証拠は
  `backups/PPSSPP_RESULT_CONTINUE_NO_RGB24_20260804.LOG`。
- 640x480 JPEG surfaceは旧pathでRGBA 1.2MiBとRGB24 0.9MiBを同時確保していた。PSP wrapperは
  libjpegから最終RGB24へ直接scanline decodeし、SDL surfaceのcalloc失敗もNULLで返す。
  result surface登録時に必要なheap arena増加はPPSSPP 32MBで約2.1MiBから約0.9MiBへ減った。
- GE backendは同一texture/color stateの再発行を省き、通常UV頂点を24 bytesから20 bytesへ縮小。
  draw直前のdcache writeback契約は維持した。BGMは総量384KiBのまま192KiB×2から64KiB×6へ変更し、
  I/O workerをpriority `0x1c`へ上げ、曲開始同期readを384KiBから128KiBへ減らした。
- 通常EBOOTのPPSSPP v1.20.4 / `PSPModel=0`回帰はDAT 6本、font、title BGM、TL archive、
  60frame提示まで完走。証拠は`backups/PPSSPP_NORMAL_32MB_PARALLEL_OPT_20260804.LOG`。
  preflightは違反0/警告0、release auditは原作data/replay/score混入0件。通常EBOOTは
  3,015,542 bytes / SHA-256 `3f31fa097ba11e2036e91d2ccb6a5e02a9b10dcd3b947300295fbc0060f0da2f`。
  G:は最初の配達中にmedia disconnectしたが、C:へ退避した同一EBOOTを`EBOOT.NEW`へ全量copyし、
  SHA-256一致後に`EBOOT.PBP`へ置換して復旧した。G:上の3,015,542 bytesと上記SHA-256は一致。
  3D背景/BGM/resultの本版による実機確認は未完了。

## V-028 result全面描画OOMとBGM補充priority (2026-08-05)

- PSP実機でContinue「No」を選んだrunは、GameManager teardown、`result.jpg`、`result00..03.anm`、
  `RESULT REGISTER END R0`まで全て通過した直後、`CXX OOM REQ921600B PC088089A4`でXMBへ戻った。
  `0x088089A4`は最終ELFで`AnmManager::ExtractSurfacePixels()`内の`new[]`直後であり、要求量は
  640x480xRGB24と一致する。ResultScreenが呼ぶ`CopySurfaceToBackBuffer()`は同じengine object内から
  矩形helperを呼ぶため、既存の矩形helperだけのGNU ld wrapperを迂回していた。
- `src/`は変更せず、全面helperもPSP wrapperから既存のGE直接uploadへ渡す。最終ELFの
  `ResultScreen::OnDraw()`は`__wrap__...CopySurfaceToBackBuffer`をcallしており、900KiBの一時複製を
  通らない。田中の実機確認でPSP-3000とPSP-1000の両方がContinue「No」から落ちなくなった。
- 同じ実機記録は2,820.672秒でBGM read 7,580回、平均間隔372.1msだった。44.1kHz stereo s16の
  64KiB消費周期371.5msと一致する一方、marker無効のため5秒周期`PERF/RUN/AUD`書込みは0行だった。
  定常補充threadがpriority `0x1c`でmain `0x20`を毎回横取りしていたため、初期window充填後は
  `0x21`へ下げ、実際のcache miss時だけ`0x1c`へ戻す。384KiB先読み量とME/SC fallbackは変えない。
- 修正版をPSPへ配達後、田中の実機所感は音切れなし、表示FPSは「60張り付き近い」。これは厳密な
  frame-time採取ではないため60fps達成値とは扱わず、周期stutter低減の実機確認とする。
  通常EBOOTは3,015,734 bytes、SHA-256
  `bd643ddd9131f1db74b7d4cab14dad310c796149789bd98e9ad1ccd4f6639d8d`。preflight違反0/警告0、
  release auditは原作data/replay/score/test marker混入0件で、G:上のhashも一致した。

## V-029 replay保存二重終端の実機回帰 (2026-08-05)

- PSP-3000のLunatic通しplayは6面・ending・staff rollまで完走した後、
  `REPLAY SAVE BEGIN ./replay/th6_02.rpy` の直後に停止した。保存前に完了面の録画bufferを
  `realloc`縮小していた一方、`SaveReplay()`内の同一翻訳単位からの二度目の
  `StopRecording()`はlinker wrapperを迂回し、縮小後のallocation外へ終端recordを書いていた。
- 保存完了まで録画bufferを元サイズで保持する修正後、専用testは通常録画を開始し、300frameで
  wrapper経由の一度目のStopと`SaveReplay()`内部の二度目のStopを連続実行した。PPSSPP v1.20.4と
  PSP-3000実機の両方で`REPLAY SAVE END`へ到達し、生成fileの再読込は
  `SAVE REGRESSION VERIFY R0 B136`となった。
- 実機証拠は`C:\Users\kan82\TH06PSP_SAVE_REGRESSION_HW_20260805.LOG`、SHA-256
  `1c379e69e308e291b591efae43d5b140026322c550f8d3b7e326b98c31999c54`。
  test EBOOTと`SAVEFIX.RPY`は実機から撤去し、無限残機＋保存修正版EBOOT
  (`71172f8dc5e46c43da27a187d48246ba901ccb932e97f0d027d78f730d7645a9`)へ復元した。
  test用compile pathは通常sourceから除去済みで、配布物には含めない。
