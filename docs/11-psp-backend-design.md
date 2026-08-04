# PSP backend 設計 — TH06

**状態: 公開preview実装の設計・実績基準**

**作成: 2026-08-04**
**根拠: `pmd_psp` / `psp-pmdvis-public` の MECC 実機実績、PS3 backend、TH06 engine**

この文書は、TH06 engine (`src/`) を変更せず、PSP固有部分を `shim/` と `psp/` に閉じ込めるための設計である。
原作 DAT/WAV、製品ISO、SF2をrepo/EBOOTへ含めない。EBOOTはメモリースティックから起動し、
利用者が所有するPC版TH6フォルダを`TH06PSP/th6`へ丸ごとコピーして供給する。

## 1. 決定済みの前提と未決定事項

| 項目 | 状態 | 扱い |
|---|---|---|
| 開発起動 | 決定 | メモリースティックのEBOOT。認証なし。 |
| engineの改変 | 禁止 | `src/` にPSP分岐を足さない。必要な適応は `shim/` / `psp/`。 |
| 原作データ | 決定 | repo/EBOOTへ含めず、利用者所有のPC版TH6フォルダを`th6`へ丸ごと置く。 |
| 出力解像度 | 決定 | 480x272。2026-08-04に田中が決定。 |
| MIDI/SF2 | **未決定** | SF2 5.7MBを載せるかはメモリ実測後、田中が決める。 |
| 描画backend | 決定 | PSP GE/GUで直接描画。Software rendererは比較・非常用だけ。 |
| MECC | 採用済み | m-c/dの現行MECCを使い、実機ではPCM音声mixだけをMEへ渡す。失敗時はSCへfallback。 |

ゲーム座標系は640x480のまま保持する。`GAME_WINDOW_WIDTH_REAL` / `HEIGHT_REAL` を480x272にして、既存のviewport scalingでSoftware rendererの出力だけをPSP画面寸法にする。engineのゲーム座標・原作データ座標は変更しない。

## 2. 境界

```
src/ TH06 engine
  │ SDL API / GfxInterface
  ▼
shim/                 SDLの互換実装、LE endian、RWops、SDL_QueueAudio
  │
  ├── psp/graphics    GfxInterface → PSP GE/GU（480x272へ直接描画）
  ├── psp/video       boot overlayとSoftware比較経路だけ
  ├── psp/input       sceCtrl → SDL scancode配列
  ├── psp/audio       SC出力thread / PCMリング / sceAudio
  └── psp/audio_me    MECC worker（純粋なPCM処理、SC fallbackあり）
```

- SC（main CPU）が engine、ファイルI/O、入力、GU、sceAudio呼出しを所有する。
- MEは engineのC++オブジェクト、ファイル、GE VRAM、SC scratchpadを所有しない。
- MEへ渡すのは、物理アドレス化できるPCMブロックと小さなuncached command mailboxだけである。
- `src/GameWindow.cpp` のFixedFunctionGL backend列挙を、include優先順位で
  `psp/graphics/FixedFunctionGL.hpp` に差し替え、PSP GE backendを返す。engineの条件式は書き換えない。

## 3. 描画

### GE直接描画（2026-08-04、Software版のPPSSPP実測を受けて変更）

Software rendererは原作データを入れたPPSSPP v1.20.4のタイトルで約5fpsだった。
またSoftware framebufferはengineの`0xAARRGGBB`をPSPの`8888`へ無変換で書き、R/Bが逆転した。
したがってCPU framebufferの提示を最適化する案は棄却し、次の直接経路にする。

1. `GfxInterface`の三角形、テクスチャ、行列、depth、blend、fogを`psp/graphics/PspGuGfx.cpp`で受ける。
2. engineの独立attribute配列を、GE規約の`UV / ABGR color / XYZ`へ一度だけ詰め直す。
3. 480x272の8888 color 2面と16bit depth 1面をGE eDRAMに置く。実機稼働済みPMDVISと同じ
   `draw=frame 0 / display=frame 1 / depth=frame 2`を使う。ゲーム座標640x480は既存viewport/matrixで480x272へ写す。
4. `ZunColor=0xAARRGGBB`は境界で`GU_COLOR_8888=0xAABBGGRR`へ変換する。RGBAテクスチャのメモリ上のバイト列は`R,G,B,A`。
5. PS3 backendで原因確定済みの透明texelによるZ汚染を避けるため、`GEQUAL, ref=4`のalpha testを常時使う。
6. engineのLE 565/5551/4444素材はGEの各16bit並びへ変換し、32bitへ膨張させない。非2冪だけpaddingする。
7. `finish → sync → vblank → swap`は1フレームに一度。最初のGE swapをboot診断の「画面引渡し」として数える。
8. GEのhardware fogは通常のPSP前方=-Zを前提にする。TH06は前方=+Zなので、`sceGuFog(-near, -far)`として
   `coef=(far-viewZ)/(far-near)`を再現する。これはSoftware rendererとPS3 shaderの式に一致する。
9. `ApplySurfaceToColorBuffer`が使う640x480静止画は、論理1024x512を物理512x512/565へ初回だけ縮小し、
   SDL surfaceの世代番号をキーに2面キャッシュする。2回目以降はsurface抽出の900KiBコピーも省略する。
   surface解放・cache evictionで印を外し、同じheap addressへ別画像が来ても取り違えない。
10. 毎フレーム先頭でLCD全体をGEで黒クリアするだけでなく、swap直前にも左右59pxの柱だけを
    scissor clearする。engineの一時fullscreen drawが後から柱へ書いても、中央362x272へ触れず黒へ戻す。

Software backendは色・性能比較とGE初期化失敗時の非常経路として残すが、PSPの本番描画経路にはしない。

この変更は`src/`を改変せずに実現する。PPSSPPの見た目だけで完了とせず、実機でframe time、99パーセンタイル、処理落ち率を取る。

## 4. 入力・時刻・ファイル

- `sceCtrl`を毎フレーム一度だけ読み、十字+アナログを矢印、×をZ/Enter、○をX、□/L/RをShift、STARTをEscへ写す。
- `SDL_GetTicks64`は`sceKernelGetSystemTimeWide()/1000`、`SDL_Delay`は`sceKernelDelayThread`に対応させる。0固定のstubを残さない。
- PSPは実用的なprocess CWDを持たない。`psp/fileio.cpp` が GNU ld の
  `--wrap=fopen` でengineの `FileSystem` とSDL RWopsを同時に受ける。XMBが`argv[0]`で渡したEBOOTの
  親をwritable state root、同階層の`th6`をoriginal data rootとして分離する。`src/`には分岐を加えない。
  6 DATは日本語long filenameを直接開き、PSP FAT APIで開けない場合は1.02 retailの固有byte sizeと
  `PBG3` magicでdirectory entryを対応させる。17 WAVは`th6/bgm`から読む。原作rootへ書き込まない。
  `ef0:`側の同名ディレクトリが存在しても、`ms0:`から起動した版をshadowさせない。`argv[0]`が無い
  特殊launcherの場合だけ、従来の`ef0:`存在確認へfallbackする。
- 開発中は起動ごとに `<storage>:/TH06PSP_BOOT.LOG` を切り詰めて書く。engineへ
  入る前のI/Oなので、音声・描画中に診断I/Oを混ぜない。正式版では不要になれば外す。
- `ms0:`固定のデータパスやCD探索をengineへ漏らさない。PSP Goの`ef0:`も実際の`argv[0]`に従う。
- 6 DATと17 WAVが揃わない場合はengineへ入らず、boot overlayへインストール不足を表示してXMBへ戻る。

### 日本語フォント

- PSPでもshimのFreeType実装を使う。PS3限定guardのまま空8x8 surfaceを返すTTF stubへ落とさない。
- FreeType内部のファイルopenはlinkerの`--wrap=fopen`を通らないため、font名をEBOOTディレクトリへ
  shim側で明示解決する。
- 探索順はengine既定の`msgothic.ttc`、次に`NotoSansJP-Regular.ttf`。公開ZIPはMicrosoft fontを含めず、
  Noto Sans CJK JPをCP932全域へsubsetした4,491,696-byte fontをSIL OFL 1.1とともに同梱する。
- PS3で実装済みのglyph prewarmとdynamic text texture生成はそのまま使う。

## 5. 音声とME

### 5.1 SC側の必須構成

TH06の`SoundPlayer`は44.1kHz S16 stereo PCMを`SDL_QueueAudio`へ送る。BGM/WAVは既にRAM全量ロードであり、ミキサmutex内でメモリースティックを読まない。

```
SoundPlayer worker (SC) ── PCM block ──► PSP PCM ring ──► output thread (SC) ──► sceAudio
```

- `sceAudioOutputBlocking`だけを行うoutput threadを最優先にする。描画・I/O・ログを混ぜない。
- producerとoutputの間はS16 stereo PCMリング。ブロック長、リング深さ、起動prefillは実測で決める。
- アンダーラン時の回数、リング最小残量、queue drop、最大render時間をRAMカウンタへ記録する。診断I/Oでaudio threadを止めない。

### 5.2 MECCを使う場合の責務

PMDVISで実証済みの形を踏襲する。ただしPMDVISのPMD/ymfm状態をTH06へコピーするのではなく、TH06で必要な**純粋PCM block処理**だけをMEへ出す。

| SC | ME |
|---|---|
| command発行、engine状態、PCMリング所有、sceAudio出力 | 指定ブロックのPCM変換/合成、完了通知 |
| 曲切替時のengine側停止/ロード | `FLUSH`を受けてME側状態をRAMへ確定 |
| SC dcache管理、計測、フォールバック | `meCoreDcache...`、必要時のみME icache invalidate |

MIDIを採用する場合は、TinySoundFontをMEへ載せる前に、SF2を含むRAM収支とME側で完結する状態同期方式を設計レビューする。SF2なし・MIDI無効の開発版ではこのworkerを起動しない。

### 5.3 ME command mailbox

PMDVISと同じく、commandとstatusはMECCが割り当てるuncached共有領域に置く。通常の共有PCMは64-byte整列する。

| command | 意味 |
|---|---|
| `NONE` | idle |
| `RENDER` | 指定PCMブロックを処理する |
| `FLUSH` | 曲/モード切替前にME側状態を確定する |
| `STOP` | worker停止 |

発行順序は `引数書込み → sync → command書込み`、完了確認は `status == DONE && command == NONE` とする。SCがME生成のcached PCMを読む前に該当範囲をinvalidateする。uncached mailboxにはflush/invalidateを重ねない。

### 5.4 メモリとキャッシュの禁止事項

- MECCは `meCore...` APIを優先し、古い`meLibDcache...`を増やさない。PMDVISではこの置換で実効速度が改善した。
- ME側でdoubleを使わない。ME FPUはfloatのみで、doubleは極端に遅い。
- MEにSC scratchpadを使わせない。PMDVISでは24msが39.6msへ悪化した実測がある。
- ME eDRAMを使う場合はMEの一時作業領域だけに限定し、末尾約256KBをME stack用に空ける。GE VRAMとは別物であり、SCから直接読む前提にしない。
- ymfmや同等のhot loopへ診断分岐を追加しない。PMDVISではME icache汚染で44→39 blocks/secまで落ちた。

### 5.5 採否ゲート

MECCは「使えるから使う」のではなく、次をSCのみの実機計測で満たせない時に有効化する。

1. audio outputのアンダーランが0でない。
2. 音声生成の99p時間が1ブロックの再生時間を越える。
3. 音声と描画の同時実行で60Hz維持に失敗する。

有効化後は、SC-onlyとME版を同じシーンで比較し、最低限 `audio underrun`、`PCM ring minimum`、`audio block 99p µs`、`frame 99p µs` を記録する。性能改善がなければME workerを外す。

## 6. 実装フェーズと完了条件

| フェーズ | 内容 | 完了条件 |
|---|---|---|
| P0 | toolchain、EBOOT、PPSSPPの最小起動 | preflight通過、PPSSPPで即死なし |
| P1 | LE endian、時刻、sceCtrl、GE直接描画 | PPSSPPで色/fpsを数値化後、実機で入力・表示・終了を確認 |
| P2 | engineを無改変で接続、データ欠損エラー経路 | 原作データを含めずに起動し、エラーを安全に表示 |
| P3 | ユーザー提供データでメニュー/ステージ、SC音声 | 実機計測ログ、音切れ/処理落ちを数値化 |
| P4 | ME採否ゲート | SC-onlyの測定に基づきMECC workerを入れる/入れない |
| P5 | 田中のMIDI判断を反映 | RAMとframe timeを実測して選択を記録 |
| P6 | CD/UMD認証・配布 | 開発版とは別工程。データをrepoへ入れない |

P0〜P2のPPSSPPは予選である。P1以降の完了判定はPSP-1000 PRO-B10実機を優先し、ARK-4機は後追い互換確認にする。

## 7. 検証記録の形式

各実機テストは少なくとも次を`docs/`または専用ログへ残す。

- 機種、CFW、CPUクロック、EBOOTのgit revision
- 実行区間、総frame数、平均/99p/max frame time、60Hz未満frame数
- audio underrun、ring最小残量、queue drop、audio block平均/99p/max
- 使用RAM（起動後・ステージ開始後・MIDI/SF2有無）
- PPSSPP結果と実機結果を別欄にする

この設計の変更は、理由・計測値・影響範囲を`DECISIONS.md`へ追記してから行う。
