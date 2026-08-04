# Fable引継ぎ — PSP初期起動・実機検証（2026-08-04）

---

> ## 【追記 2026-08-04 / 母艦(Claude)】この文書より下は引継ぎ時点の記録である
>
> 起動診断層を入れ、PPSSPPで実証した。**実機はまだ動かしていない。**
>
> - 実装: `psp/diag.cpp` / `psp/text.cpp` — 画面onのboot log、boot watchdog、脱出コンボ、
>   ログのms刻印と1世代退避。判断根拠は `DECISIONS.md` D-005、読み方は `docs/51-boot-diagnostics.md`。
> - 実証: `docs/50-validation.md` V-002 / V-003 (PPSSPP)。engineはconfig→window→renderer→
>   sound→inputまで到達し、停止理由は原作データ `紅魔郷IN.DAT` の不在だった。
> - **本文の「engineへ入った直後に止まる」は、breadcrumbがno-opでFOPENログも無い版での観測である。**
>   その版は停止段階を表現できなかった。今の版は表現できる。実機で取り直すこと。
> - 下の「A. HOME確認だけを先に終える」は不要になった。watchdogと脱出コンボがあるので、
>   本体版を直接1回流せば結論が出る。
>
> 次にやること: `./psp_go.sh --drive H` で本体を配達 (SHA-256照合つき) → 実機で起動 →
> 画面のboot noteを読む、またはUSBで `H:\TH06PSP_BOOT.LOG` を取る。

---

## まず結論

TH06 PSP本体はまだ実機で正常起動していない。最初の黒画面の原因の一つは、表示初期化の不足だった。
**engineを外した起動プローブは実機で緑画面まで到達した。** 一方、本体版はengineへ入った直後に止まり、HOMEから戻れない版を一度配達してしまった。

田中は「もう良い、Fableに任せる」と明示した。Sol/Codexはここから実装・実機配達を続けない。

## ユーザーの確定方針

- 開発版はメモリースティック起動。CD認証は最後。
- 出力は480x272。
- PPSSPP確認は行うが、実機を最終判定にする。
- 原作データ・DAT/WAV・SF2をrepo/EBOOTへ入れない。
- `src/` は直接変更しない。PSP適応は `psp/` と `shim/`。
- MEはまだ使わない。PMDVISのSC-only実績を先に守る。

`AGENTS.md` と `docs/00-INDEX.md` を最初に読むこと。

## 現在のローカル状態

ローカルの `EBOOT.PBP` は**本体版ではない**。Moonlight型callbackを入れた
`BOOT_PROBE=1` のプローブで、サイズは **130,596 bytes**。

プローブの目的は、次の二点だけを実機で確認すること。

1. 緑画面が出ること。
2. HOMEで必ずXMBへ戻ること。

このプローブはまだ実機へ転送していない。田中が強制シャットダウンした後、USB再接続前に作成された。

最後に実機 `H:\PSP\GAME\TH06PSP\EBOOT.PBP` へ配達済みだった本体版は、Moonlight表示経路を入れた **2,187,836 bytes**、SHA-256:

```
7D8CD9ECFF0F9724380EF05730824980754EEE5B296D445274B9366F00012886
```

ただしその本体版はHOME復帰不能だった。後でcallbackを修正したが、本体としては未ビルド・未配達である。

## 実機で得られた確定事実

### 1. 最初の本体版

- 症状: 黒画面、XMBへ戻る/停止に見える。
- 初期の直接VRAM + `sceDisplaySetFrameBuf` 実装ではLCD表示が成立しなかった。

### 2. engineなしプローブ（GU版）

- 田中の報告: **緑**。
- USB復帰後に取得した `H:\TH06PSP_BOOT.LOG`:

```text
ef0:/PSP/GAME/TH06PSP
ENTRY clock=333
VIDEO OK
BOOT PROBE MAIN OK
```

従って、EBOOTローダ、C++ runtime、`main()`、ストレージ判別、ログ書込み、基本VRAM書込みまでは正常。

### 3. 本体版（GU表示初期化入り）

- 田中の報告: 青画面で停止。HOMEで戻れない。
- USB復帰後のログ:

```text
ef0:/PSP/GAME/TH06PSP
ENTRY clock=333
VIDEO OK
ENGINE ENTER
```

`ENGINE RETURN` は無い。青はengine終了画面ではなく、engineへ渡す直前に塗った色だった。
つまり停止点は **`th06_engine_main()` に入った後、engineの最初期**。

`src/main.cpp` の最初の実処理は `g_Supervisor.LoadConfig(TH_CONFIG_FILE)`。そこで
`FileSystem::OpenPath`/`fopen`、既存configの検証、必要なら `bgm/th06_01.wav` の確認をする。

## 反省点（繰り返さないこと）

1. PMDVISの `running=0` callbackを、engine側に監視ループが無いまま移植した。
   TH06 engineは `gRunning` を見ない。HOMEで戻れなくなった。
2. MoonlightのH.264テストを先に確認すべきだった。Moonlightはcallback内で
   `running=0` **と** `sceKernelExitGame()` の両方を実行している。
3. Moonlight型表示へ切り替えた時、一度 `make -B` の既定対象に頼り、実際には型エラーで
   新EBOOTが生成されていないのに古い版を配達した。以降は必ず次を守ること:

```bash
make -B all -j2
./psp_preflight.sh --post
stat -c '%y %s' EBOOT.PBP
```

生成したELFの実装確認も行う。ビルドエラーは `/tmp/th06psp-build.log` に残りうる。

## いまコードに入っているもの

### `psp/main.cpp`

- `PSP_MODULE_INFO`、callback thread、333MHz、起動ログ、起動色、engine呼出し。
- 現在はMoonlight H.264型に合わせて、callbackは `sceKernelExitGame()` を呼ぶよう修正済み。
- callback threadもMoonlightと同じ `"cb_thread", priority 0x11, stack 0xFA0, attr 0` に修正済み。
- `BOOT_PROBE=1` ならengineを完全に外し、緑表示のまま待つ。

### `psp/video.cpp`

現在はMoonlight直系へ変更済み（ただしローカルのプローブでのみ未実機確認）。

```cpp
const auto edram = reinterpret_cast<std::uintptr_t>(sceGeEdramGetAddr());
gDraw = reinterpret_cast<volatile std::uint32_t *>(0x40000000u | edram);
sceDisplaySetMode(0, 480, 272);
sceDisplaySetFrameBuf(..., 512, PSP_DISPLAY_PIXEL_FORMAT_8888,
                      PSP_DISPLAY_SETBUF_NEXTFRAME);
```

- GUを使わない単一VRAM buffer。
- Software rendererの480x272 pixelsを直接コピーする。
- これは `psp-moonlight/test/h264_decode_test/main.c` / `net_stream_test/main.c` の実機経路をC++として型安全に移したもの。

### `psp/fileio.cpp`

- PMDVIS流の `ef0:/PSP/GAME/TH06PSP` 存在判定、なければ `ms0:`。
- `--wrap=fopen` で相対パスをEBOOTディレクトリに解決し、engineの `src/FileSystem.cpp` は変更しない。
- `TH06PSP_BOOT.LOG` をストレージ直下へ出す。
- 現在は各 `fopen` の入口と成否もこのログへ書く診断を入れている。
  起動初期限定であり、audio/render hot pathではない。

## Fableが最初にやる順序

### A. HOME確認だけを先に終える

1. PSPをUSB接続してH:が現れたことをPowerShellで確認する。
2. 現在のローカルプローブを、PowerShell UNC経由でのみ `H:\PSP\GAME\TH06PSP\EBOOT.PBP` にコピーする。
   WSLから `H:`/`/mnt/h` を直接触らない。
3. 緑画面を確認後、**HOMEでXMBへ戻れることだけ**を田中に確認してもらう。
4. 戻れないなら本体へ進まず、`psp-moonlight/test/h264_decode_test/main.c:174-195` と
   `psp/main.cpp` の差だけを調べる。

### B. 本体を戻す

1. `make -B all -j2` （`BOOT_PROBE`無し）で本体を明示ビルド。
2. post preflight、サイズ、SHA-256、ELFの更新時刻を確認。
3. PowerShell UNCで転送。
4. 停止後に強制シャットダウンせず、まずHOME復帰を試す。戻れたらUSB接続し、
   `H:\TH06PSP_BOOT.LOG` を読む。
5. ログの `FOPEN ...`/`FOPEN OK|NG` で最初の停止箇所を特定する。

## engine初期化を調べる時の候補

`LoadConfig` は `src/Supervisor.cpp:657`。本体ログは `ENGINE ENTER` までで止まった。

- configファイルは実機に既存（表示名はWindows側で文字化けして見えたが、サイズ56 bytes）。
- `GameConfiguration` の期待サイズは0x38 (56)。
- configが正常なら次は `GameWindow::CreateGameWindow()`、`GameWindow::InitD3dRendering()`、
  `g_SoundPlayer.InitializeDSound()`、`Supervisor::RegisterChain()` と進む。
- `src/main.cpp` にPS3由来の `th06_breadcrumb` 呼出しが一部無条件で残る。PSP側は
  `psp/platform_hooks.cpp` のno-opで受けている。

engineを直接変えずに段階を可視化する必要があれば、既存無条件breadcrumbを
`psp/platform_hooks.cpp` から `th06_psp_boot_note()` に流す方法はある。ただし、
PS3専用guard内のbreadcrumbはPSPビルドでは実行されない。

## 参考にすべき実績ソース

- Moonlight direct VRAM + callback:
  - `/home/kan82/psp-moonlight/test/h264_decode_test/main.c`
  - callback: およそ174–195行
  - `vram_init`: およそ96–102行
- Moonlight stage-color診断:
  - `/home/kan82/psp-moonlight/test/net_stream_test/main.c`
  - VRAM: およそ78–90行、callback: およそ179–199行
- PMDVIS storage prefix/logging:
  - `/home/kan82/pmd_psp/main.c:24-70`
  - `/home/kan82/psp-pmdvis-public/main.c:25-70`

Moonlightの `running` を使うなら、対象main loopが実際にそれを監視することを必ず確認する。
TH06 engineは監視しないため、callback内 `sceKernelExitGame()` が必要。

## デプロイ作法

H: は今回の実機メモリースティックとして田中が指定した。ドライブレターは変動しうるので、毎回確認する。

```powershell
$src='\\wsl.localhost\Ubuntu-24.04\home\kan82\th06_psp\EBOOT.PBP'
$dst='H:\PSP\GAME\TH06PSP\EBOOT.PBP'
Copy-Item -LiteralPath $src -Destination $dst -Force
Get-FileHash -LiteralPath $dst -Algorithm SHA256
```

既存の `東方紅魔郷.cfg`（Windows上で文字化け表示）は削除・上書きしていない。

## 検証済み

- PSP GCC 15.2.0で本体・プローブともリンク成功。
- `./psp_preflight.sh --post`: 違反0、警告0。
- `--wrap=fopen` が最終ELFで `FileSystem::FopenUTF8` と `SDL_RWFromFile` から呼ばれることを逆アセンブルで確認済み。
- PPSSPPは旧版で、原作データ未配置時に `data/wav/plst00.wav` と `data/text.anm` が無いと記録した。
  実機の停止原因をPPSSPP結果で断定してはならない。
