# PSP SDK / homebrew 実戦知見 (sceMpeg・CFW差異・デバッグ全記録)

> **鮮度警告**: ローカルコピー。正本はサーバ側。
> 大半は psp-moonlight (動画再生) 由来の記録で、TH06移植に直接効くのは
> モジュールインポート、メモリ、実機とPPSSPPの差、デバッグ手段の章。

---

---
name: psp
description: PSP全技術知見統合。psp-pmdvis ME/MECC/オーディオ + psp-moonlight sceMpeg NAL Auパス/エラーコード/CFW差異/参照実装分析 + 全地雷記録。
metadata: 
  node_type: memory
  type: reference
  originSessionId: f2b56396-073b-47eb-bcb4-99dc2c084167
---

最終更新: 2026-04-11

## PMD Visualizer PSP — 機能仕様 (v37187f2 2026-04-08)

### エンジン
- **pmdmini (ymfm fork)**: YM2608 FM音源エミュレーション
- **Media Engine (ME)**: FM合成をMEに委譲、SC(メインCPU)と並列動作
- **MECC v2.0.1** + 田中パッチ4点 (auto-load方式、seplugins不要)
- **ループキャッシュ**: 60秒分のPCMを事前デコード、ME→eDRAMストリーミング
- **サンプルレート**: 再生22050Hz / WAVエクスポート44100Hz
- **ソロモード**: 11パート個別ON/OFF (FM6+SSG3+ADPCM+Rhythm)
- ソース(開発): `/home/kan82/pmd_psp/` (master = 安定版)
- ソース(公開): `/home/kan82/psp-pmdvis-public/` → https://github.com/kan8223-dotcom/psp-pmdvis

### 画面
- **曲一覧 (LIST)**: 曲名リスト、スクロール、カーソル選択
- **ピアノロール (KEYBOARD)**: 鍵盤ビジュアライザ
- **FMPステータス (FMP)**: チャネル別 TONE/VOL/KEY/FREQ 表示

### 操作 — リスト画面
| ボタン | 機能 |
|--------|------|
| ○ | 曲再生 |
| × | アプリ終了 |
| ↑↓ | カーソル移動 |
| L/R | ページ送り/戻り |
| □ | WAVエクスポート |
| △ | 曲削除 (システムダイアログ確認→メモステから削除) |
| START | ME ON/OFF切替 (警告ダイアログ) |
| SELECT | USB接続切替 |

### 操作 — 再生画面
| ボタン | 機能 |
|--------|------|
| ○ | 一時停止/再開 |
| × | リストに戻る |
| ←→ | ビジュアルモード切替 (KEYBOARD⇔FMP) |
| ↑↓ | ソロカーソル移動 |
| △ | トラックミュートトグル |
| □ | ソロ全復帰 |
| L/R | 曲送り/戻り |
| L+R同時 | スクリーンショット保存 |
| SELECT | USB接続切替 |

### WAVエクスポート設定画面
| ボタン | 機能 |
|--------|------|
| △ | ループ回数切替 (1⇔2) |
| □ | フェードアウト ON/OFF |
| ○ | エクスポート開始 |
| × | キャンセル |

### 曲管理
- **読み込み**: `ms0:/PSP/GAME/PMDVIS/songs/*.M` を自動読み込み (MAX 64曲)
- **削除**: リスト画面△で日本語確認ダイアログ→ファイル削除+リスト除去
- **追加**: USBでsongsフォルダに.Mコピー→再起動で反映
- **WAV出力**: `ms0:/MUSIC/曲名.wav` に44100Hz/16bit/stereo出力

### システム
- ○×ボタン: システム設定に従う (日本=○決定)
- ME ON/OFF: 起動時ON、STARTで切替 (OFFだと遅延あり)
- USB: SELECT切替、内部ストレージをPCにマウント
- スクリーンショット: `ms0:/PICTURE/pmdvis_XXXX.bmp`
- ログ: `ms0:/pmd_log.txt` (RAMバッファ→終了時書き出し)

### 未実装/頓挫
- **SEARCH (メモステ検索)**: 頓挫。PSP Go ARK-4でコード追加すると起動コケ。
- **◯×リージョン対応アプリ操作**: ダイアログは対応済み、アプリ内操作は未対応。
- **音色プレビューモード**: ブランチ `feat/voice-preview`、D-pad左右反応しない不具合。棚上げ。

## オーディオアーキテクチャ (2026-04-05 確定)
**3スレッド + リングバッファ + 動的yield + vis同期**

| スレッド | 優先度 | 役割 |
|----------|--------|------|
| audio_output | 0x10 | ring読み→sceAudio + vis_current更新 |
| audio_producer | 0x20 | ME render→upsample→ring書き込み + visスナップショット |
| main | 0x20 | ビジュアライザ描画 (vis_currentから読む) |

- **動的リングバッファ**: 256ブロック (BSS→heap移行で32MB機も確保成功)
- prefill=ring_blocks-1 (=255)、再生開始まで約6秒 (ME 42blk/sec)
- upsample: 22050→44100 2x複製 (整数演算、float不使用)
- PSP_SAMPLERATE: 22050, RENDER_SAMPLES: 512, AUDIO_SAMPLES: 1024 (2x upsample)

## 確定パラメータ (v1.1-stable-ring256)
| パラメータ | 値 |
|-----------|-----|
| ring_blocks | 256 |
| lc_prefill_wait | 255 |
| LC_SAFETY_MARGIN | 1 |
| LC_MAX_SEC | 60 (64MB) / 30 (32MB) |
| SetVolumeFM | 10 (+10dB) |
| SetVolumePSG | -18 |

## ME + eDRAM
- eDRAM直接アドレス 0x00000400 (API不使用、APIはMEクラッシュ)
- ME_CMD_FLUSH: トラック変更時のみ全dcache writeback+invalidate
- **ME FPU**: COP1 (32bit float) あり、VFPU なし。doubleはソフトエミュ50-100倍遅
- **メモリ住み分け**: scratchpad=SC専用、eDRAM=ME専用

## ME eDRAM アーキテクチャ (2026-04-09 確定)
- **ME eDRAM と GE VRAM は別物** (別の物理メモリ)
- ME eDRAM: 0x00000000 base, Fat=2MB / Slim+=4MB
- GE VRAM: 0x04000000 base, sceGeEdramSetSize()で拡張可能
- Slim+: SC 4MB + ME 4MB = **合計 8MB** (m-c/d確認)
- ME スタックは eDRAM 末尾から下に成長 (Fat: 0x200000, Slim: 0x400000)
- **末尾 ~256KB はスタック用に避ける** (m-c/d助言)
- 田中の現在の使用: edram_buf = 0x00000400 から約2KBのみ → 残り大量に空き
- VME がデフォルトで eDRAM にバックグラウンド処理 → meLibSetMinimalVmeConfig() で停止 (MECC標準)

## PSPメモリ階層 (2026-04-09 確定)
| メモリ | 所有者 | サイズ | アドレス | SC読み書き | ME読み書き |
|--------|--------|--------|----------|-----------|-----------|
| スクラッチパッド | SC専用 | 16KB | 0x00010000 | ○ (最速) | ✕ (遅い) |
| ME eDRAM | ME専用 | 2-4MB | 0x00000000 | 未検証(不可の可能性高) | ○ (高速) |
| メインRAM | 共有 | 32/64MB | 0x08000000 | ○ | ○ (uncached推奨) |
| GE VRAM | GPU | 2-4MB | 0x04000000 | ○ (GE空き分利用可) | 不明 |

## MECC v2.0.1 移行 ✅ (2026-04-08)
- 田中独自パッチ4箇所 (me-core-custom.c / me-lib.c / me-lib.h / kcall.S)
- auto-load完全動作 (seplugins不要、EBOOT.PBP単体でME ON)
- embedded_kcall: 2,198B = v2.0.1正解版

## MECC API 方針 (m-c/d確定 2026-04-09)
**原則: meCore 系を使え。meLib 系は meCore に該当関数がない場合のみ。**
| 項目 | 推奨 |
|---|---|
| ME側 dcache | `meCoreDcache...` (MECC syscall) ✅ v1.0.2で置換完了 |
| SC側 dcache | `sceKernelDcache...` (PSP公式SDK) |
| uncached バッファ | writeback/invalidate **不要** (呼び出し自体削除可) |
| ME側 Icache | `meLibIcacheInvalidateAll()` 維持 (meCore版が無い場合) |

## v1.0.2 dcache リファクタ (2026-04-09) ✅
- meLibDcacheWritebackInvalidateAll ×6 → meCoreDcacheWritebackInvalidateAll
- meLibDcacheWritebackRange ×11 → meCoreDcacheWritebackRange (キャスト void*)
- meLibIcacheInvalidateAll ×5 → 維持
- **体感パフォーマンス向上**: TH1が2ループ→3ループまで持つように改善
- 原因: meLib版はcache命令を2倍撃ち、meCore版はネイティブsyscall 1発

## SC→ME eDRAM読み取りテスト (2026-04-09) — 未完了
- ARK-4 CXPLの.text問題で検証不可。棚上げ。
- m-c/dのedram-dumpサンプルがmeCoreMemcpy使用 → SC直接読み不可の可能性高

## ME OC (407MHz) テスト (2026-04-09) — 棚上げ
- psp-444mhz-plugin (m-c/d作) PSP-3000で読み込み成功だがOC発動でフリーズ
- PMDVISには使わない (田中判断)

## PSP全モデルME性能等価実証 (2026-04-07) ✅
- PSP-1000/3000/Go: ME blocks/sec 全同等 (~42-44)
- kcall.prx: 全機種同一バイナリで動作

## ビルド・デプロイ
```
cd /home/kan82/pmd_psp && make      # 開発リポ (auto deploy to E:/G:)
cd /home/kan82/psp-pmdvis-public    # 公開リポ (deploy.shのパスが開発リポを指すので注意)
```
deploy.shのソースパスは `/home/kan82/pmd_psp/EBOOT.PBP`。公開リポのEBOOTをデプロイするにはPowerShell直接コピー。

## 東方MIDI→PMD変換
- **MIDI抽出**: THTK (`/tmp/thtk/build/thdat/thdat -x{N}`) で紅魔郷〜花映塚の68 MIDI抽出済み → `/mnt/c/Users/kan82/Desktop/touhou_midi/`
- **変換**: `/tmp/midi2pmd.py` — MIDI ch→FM6+SSG3自動割当、テンポ半減ルール(MIDI BPM÷2=PMD t値)、CC#2でループ設定
- **セプテット (th6_13)**: FM6+SSG3、t72、PSP動作OK。SSG音量小さい(v12)
- **U.N.オーエン (th6_15)**: t76、埋め込みだとフリーズ、メモステだと再生OK
- **PMDリズム(K channel)**: PMDDotNETとpmdminiで互換性なし。未解決

## FC/NES ppmck MML → PMD MML 変換
成功例: torii_686bgm, Stage1n。変換ツール: `/tmp/ppmck2pmd.py`

### 注意点
- **テンポ2倍速問題**: ppmckテンポ÷2必須 (t144→t72)。前科2回
- ppmck `<`=オクターブ上, `>`=下 → **PMDと逆**
- ppmck固有コマンド (`@v`,`@@`,`MP`,`EN`,`SA`,`k`,`q`,`OP`,`w`) 全ストリップ
- `k`→`r`, 多重ドット→シングルドット, 64/128分→32分が最小

### チャンネルマッピング例
- ppmck A → PMD A (@012), F → B (@101), G → C (@074), O → D (@169), M → E (@023)

## ZUN王道FM音色 (PC-98 東方旧作)
6音色で東方サウンド再現。全てALG4/ALG0、FB7。

```
; @012 リードメロディ — 鋭く突き抜ける
@012 004 007
021 004 002 004 002 040 000 012 003 000
024 012 004 006 002 000 000 004 007 001
021 004 002 004 002 035 000 006 007 000
024 012 004 006 002 000 000 002 003 001

; @023 サステインリード — 滑らかな持続音
@023 004 007
031 000 000 002 000 030 003 004 003 000
031 002 000 007 003 000 000 004 003 000
031 000 000 002 000 031 003 002 007 000
031 002 000 007 003 000 000 004 007 000

; @074 ソフトパッド — 暖かいハーモニー (ALG0直列)
@074 000 004
031 007 007 009 002 028 003 006 006 000
031 006 006 009 001 058 003 005 006 000
031 009 006 009 001 022 002 000 006 000
031 006 008 009 015 000 002 001 006 000

; @101 ベース — 深く響く低音
@101 004 007
028 000 008 000 003 035 002 012 003 000
026 010 007 006 002 000 001 004 003 000
028 000 008 000 003 033 002 012 007 000
026 010 007 006 002 000 001 004 007 000

; @169 ブライトリード — 明るい変奏メロディ
@169 004 007
031 018 012 004 012 040 000 008 007 000
031 014 009 008 007 000 000 004 007 000
031 018 012 004 012 040 000 008 003 000
031 014 009 008 007 000 000 004 003 000

; @181 サステインブラス — 金管系持続音
@181 004 007
031 000 000 000 000 027 000 008 007 000
031 016 000 008 002 000 000 004 007 000
031 000 000 000 000 030 000 008 003 000
031 016 000 008 002 000 000 004 003 000
```

MIDI→PMD音色割当: メロディ→@012, ベース→@101, カウンター→@169/@023, パッド→@074, ブラス→@181

## PSP ME Benchmark
- https://github.com/kan8223-dotcom/psp-me-bench
- SC/DUAL × 222/333MHz、PI/PRIME/MANDEL 3テスト
- DUAL 333MHz = SC 222MHz比で平均×3.48
- ソース: `/home/kan82/psp_me_bench/`
- PSP-3000: 新ビルド(v2.0.1後)はXMBリターン、Git初期版(162,622B)は動作

## ダブルバッファ・ループキャッシュ (v1.1)
- ME FM合成が実時間より遅い (~0.86x) → 2面PCMキャッシュで事前デコード
- ME_CMD_DECODE_LOOP: MEにブロック一括デコードさせるコマンド
- 動的確保: 64MB機=60秒, 32MB機=30秒

## 田中独自パッチ (MECC v2.0.1に適用済み、4箇所)
1. `me-core-custom.c`: meLibLoadPrx()の戻り値チェック除去 (auto-load方式対応)
2. `me-lib.c`: sceKernelExitGame()削除
3. `me-lib.h`: #ifdef __cplusplus ガード + (uintptr_t)キャスト + meLibMakeSharedUncachedMemマクロ追加
4. `kcall.S`: 田中版独自ファイル
- 旧mystery版バックアップ: `/home/kan82/mecc_tanaka_custom_20260408_205517/`

## auto-load MECC 完全動作確認 (2026-04-08) ✅
- seplugins空、EBOOT.PBP のみで ME ON 成功 (PSP Go + ARK-4)
- **根本原因**: 古い EBOOT.PBP (旧 embedded_kcall 1,740B) がデプロイされてただけ
- v2.0.1 の正しい EBOOT.PBP で writePrx() → 2,198B kcall.prx → pspSdkLoadStartModule 成功
- seplugins/kcall.prx は不要。PLUGINS.TXT方式のワークアラウンドは不要

---

## psp-moonlight — sceMpeg NAL Au パス (H.264 デコード)

### API呼出順序 (pmplayer-advance 参照実装準拠)
```
sceUtilityLoadAvModule(AVCODEC)
sceUtilityLoadAvModule(MPEGBASE)  // ARK-4では失敗→flash0直接ロード
sceMpegInit()
ddrtop = memalign(0x400000, 0x200000)  // 4MB境界、2MBブロック
mpeg_data = malloc_64(sceMpegQueryMemSize(mode))
au_buffer = ddrtop + 0x10000
// ringbuffer: memset 0 のみ、sceMpegRingbufferConstruct は呼ばない
sceMpegCreate(&mpeg, mpeg_data, size, &ringbuffer, 512, mode, ddrtop)
// sceMpegAvcDecodeMode は pmplayer では呼ばない (要検証)
au = malloc_64(64); memset(au, 0xFF, 64)  // pmplayer方式
sceMpegInitAu(&mpeg, au_buffer, &au)

// Per frame:
nal.mode = (first_frame ? 3 : 0)
sceMpegGetAvcNalAu(&mpeg, &nal, &au)
sceMpegAvcDecode(&mpeg, &au, 512, 0, &pic_num)
sceMpegAvcDecodeDetail2(&mpeg, &detail)  // 毎フレーム呼ぶ
if (pic_num > 0) sceMpegBaseCscAvc(rgb, 0, 512, &csc)
```

### Mp4AvcNalStruct (32バイト)
```
offset 0x00: sps_buffer (生データ、長プレフィクスなし)
offset 0x04: sps_size
offset 0x08: pps_buffer (生データ)
offset 0x0C: pps_size
offset 0x10: nal_prefix_size (通常4)
offset 0x14: nal_buffer (AVCC形式: 4バイトBE長+NALデータ)
offset 0x18: nal_size (プレフィクス含む全長)
offset 0x1C: mode (3=初回IDR, 0=以降)
```

### エラーコード (uofw公式定義)
| コード | 名前 | 意味 |
|--------|------|------|
| 0x80628001 | SCE_MPEG_ERROR_VIDEO_ERROR | 回復可能デコードエラー。MEがNALを不正と判断 |
| 0x80628002 | SCE_MPEG_ERROR_VIDEO_FATAL | 致命的デコードエラー。リングバッファ空等 |
| 0x806201FE | SCE_MPEG_ERROR_VIDEO_INVALID_VALUE | 不正な値 |
| 0x8002013A | (NID未リンク) | 関数NIDが解決できない |

### PPSSPP HLE内部条件
- 0x80628002発生: `ringbuffer->packetsRead == 0` の時
- sceMpegGetAvcNalAu: 完全UNIMPL (NID 0x11F95CF1)
- sceMpegBaseCscAvc: UNIMPL
- **PPSSPP Core/HLE/sceMpeg.cpp はsceMpegエラーコードの最良情報源。** 新規バグでは最初にgrepすべし。
- NAL Auパスの実機検証にはPPSSPP使用不可、PSP-1000 PRO-B10必須

### IMPORT_STARTフラグ
- sceMpeg, sceMpegbase: **0x00090000 (WEAK_IMPORT) 必須**
- 0x40010000で起動失敗 (0x8002013C)。前科3回。

### CFW差異 (実測結果 2026-04-11)

#### ARK-4 (PSP Go / PSP-3000)
- sceMpegAvcDecode でエラー時に**ハングする** (ウォッチドッグ強制シャットダウン)
- sceUtilityLoadAvModule(MPEGBASE) が壊れている → flash0直接ロード必須
- log_resume (sceIoOpen APPEND) が失敗する → ログ取得不可
- **開発機としては不適**

#### PRO-B10 (PSP-1000)
- sceMpegAvcDecode でエラー時も**クラッシュせず最後まで走る**
- sceUtilityLoadAvModule(MPEGBASE) 正常動作
- ログ完全取得可能
- **開発機確定**
- sceMpegAvcDecodeDetail2 の NID が未リンク (0x8002013A) → Detail fallback必要

### Phase 0-3 デバッグ記録 (2026-04-11)

#### 検証済み仮説 (全て無効 — 再試行禁止)
1. 案D: idr_bufs 独立バッファ (バッファ衝突仮説) → 無効
2. sceMpegRingbufferConstruct 削除 → 無効 (ARK-4)
3. sceMpegAvcDecodeMode 削除 → 無効 (ARK-4+PRO-B10)
4. SceMpegAu 0xFF初期化 → 無効 (ARK-4+PRO-B10, malloc_64(64)も検証済み)
5. Detail2 毎フレーム呼出 → 無効 (ARK-4)
6. 上記3+4+5全適用 → 無効 (ARK-4)
7. 単一SPS/PPSテストファイル → エラーコード変化するがデコード失敗
8. Annex B→AVCC変換 → MP4直接パース(pmplayer同一データパス)でも同じ0x80628001
9. SEI NAL除去 → 無効 (PRO-B10)
10. CABAC→CAVLC (Main→Baseline Profile) → 無効、むしろ0x80628002に悪化
11. mpeg_mode 4→5 → 無効 (PRO-B10)
12. sceMpegRingbufferConstruct 追加(正しいNID) → 無効 (PRO-B10)
13. PSPネイティブMP4データ使用 → 無効
14. PSP動画プレーヤーで再生確認済みMP4からの直接抽出 → 無効

#### 現在の実機エラー (PSP-1000 PRO-B10)
- Frame[0] NalAu: 成功 (auSize=114, 正常)
- Frame[0] Decode: **0x80628001** (VIDEO_ERROR)
- 全フレーム同一エラー
- MEはデコードを試みているがNALデータを不正と判断
- データパスはpmplayer-advance完全一致確認済み
- NID修正済み: RingbufferQueryMemSize=0xD7A29F46, RingbufferConstruct=0x37295ED8

#### 残る未検証候補 (明日以降)
1. PPSSPP HLE sceMpeg.cpp 精読 — 0x80628001発生条件の網羅的調査
2. pmplayer-advance 実機ビルド — 同一PSP-1000 PRO-B10でpmplayer自体が動くか検証
3. sceMpegAvcDecode の第4引数 — pmplayerは0を渡すが実機で挙動が異なる可能性
4. SceMpegRingbuffer のフィールド個別設定 — packetsRead等の非ゼロ化
5. MECC経由のHW H.264デコード (Route Cの再評価)

#### 【2026-07-04 実機確定】仮説15的中: 0x80628001の真因 = sceMpegモジュール提供元
**PSP Go (ARK-4) 実機で Decode: 0x00000000 を確認。3ヶ月・14仮説の壁を突破。**
- 真因: **NAL AuパスはVSH版mpegモジュール(flash0:/kd/mpeg_vsh.prx)の機能。ゲーム版mpeg.prxではMEがNAL Auを拒否(0x80628001)**
- ロード方法: user modeのsceKernelLoadModuleは**0x80020148で拒否** → **kuKernelLoadModule (kubridge)** で成功。SDK: sdk/include/kubridge.h + -lpspkubridge。ARK-4/PRO両対応
- 証拠: mpeg_vsh版は sceMpegQueryMemSize(4)=**49839** (ゲーム版は65536) — 実装が入れ替わった動かぬ証拠
- MPEGBASE utilityは**呼んではいけない** (先に呼ぶとゲーム版mpeg.prxがsceMpegを提供してしまう)
- **【Phase 0-3 COMPLETE 2026-07-04】** PSP Go実機でH.264 MEハードウェアデコード→CscAvc→VRAM表示まで全パス成功
  - **0x80618007の正体 = B-frame**: bframes>0だとDPB並べ替え保留中の次AU投入をGetAvcNalAuが拒否。**bframes=0必須** (Moonlight実運用は元々bframes=0なので問題なし)。リトライ/待ち時間では解決しない(20回×10ms無効)
  - **pic_num遅延 = 4 AU** (AU[0]投入→AU[3]でpic_num=1)。bframes=0でもデコーダ内部パイプライン遅延あり。30fpsで約100ms相当 → Moonlightレイテンシ設計の基準値
  - テストファイル: 自前パーサ使用なら`-f psp`不要 (XMB再生互換用でしかない)。映像のみ`-f mp4`が最も安全 (音声トラック入りはmdatインターリーブで簡易パーサの連続配置前提が崩れる)
  - Detail2はmpeg_vsh版で正常動作 (info/yuvバッファ取得OK、480x272正値)
  - 成功画面: 緑枠+デコード画像。checkpoint()は画像を黒塗りするので成功パスでは使わない
  - **この知見はコミュニティ未文書化の可能性大** (m-c/dはHW直叩き側、sceMpeg NALパスはpmplayerソースに暗黙に埋まってただけ) → Discord #media-engine 共有ネタ
- **ARK-4が2026-07時点で大幅改善** (4月の記録と別物): MPEGBASE utility 0x00000000で成功 / sceMpegAvcDecodeエラーでもハングせず生還 / sceIoOpen APPEND動作(ログ完全取得) → **PSP GoがsceMpegテスト機として使用可能に**
- ドライブ注意: この日はE:=SSD、PSP Go=G:。レター固定思い込み禁止

#### 【2026-07-04】仮説15の調査過程 (参考)
pmplayer-advance原典をclone精読 (過去の「完全一致」は目視比較で、モジュールロード部を見落としてた):
- **pmplayer codec_prx.c**: `sceUtilityLoadAvModule(0)` → **flash0:/kd/mpeg_vsh.prx を必ずロード** (FW5.00+、旧FWは同梱mpeg_vsh330/350/370.prx)。**MPEGBASE utilityは絶対に呼ばない**
- うちのテスト: MPEGBASE utilityがPRO-B10で成功 → mpeg_vshフォールバック不発 → sceMpegGetAvcNalAuが**ゲーム版mpeg.prx**に解決されてた
- NAL AuパスはXMB動画プレーヤー用のVSH版mpegモジュールの機能である可能性大。ゲーム版はリンクは通るがMEがNAL Auを拒否 = 0x80628001と整合
- **mpeg_mode選択はlevelでなく解像度基準**: pmplayer mp4_decode.c `(width>480||height>272) ? 5 : 4`。test.mp4(480x272)なら4
- **me_boot_start(type)**: cooleyesBridge.prx (カーネル) 経由で sceMeCore_driver の sceMeBootStart660 (NID 0x5DFF5C50, FW6.60+) を呼びMEリブート。type: 1=高解像度(>480x272)、3=Main@SD(デフォルト、no-op)、4=Baseline。**Main@480x272はMEリブート不要** → test.mp4はcooleyesBridge無しでいけるはず
- main.c改修済み (mpeg_vsh直接ロード + mode解像度基準 + ringbuffer memset0のみ)、ビルド済み、**実機デプロイ待ち**
- 懸念: PRO-B10 user modeでflash0:/kd/mpeg_vsh.prxのsceKernelLoadModuleが弾かれる可能性 → その場合はkubridge (kuKernelLoadModule) 追加
- 原典クローン: scratchpad/pmplayer-advance (セッション限り)。GitHub: DavisDev/pmplayer-advance

### PSP互換MP4エンコード (2026-04-11 別セッションで実証済み)
```
ffmpeg -y -i source.mp4 \
  -vf "scale=720:480:force_original_aspect_ratio=decrease,pad=720:480:-1:-1:black" \
  -c:v libx264 -profile:v main -level 3.0 \
  -pix_fmt yuv420p -r 29.97 \
  -b:v 1200k -maxrate 1500k -bufsize 1500k \
  -x264-params ref=3:bframes=2:weightp=0:b-pyramid=none \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -movflags +faststart \
  -f psp output.mp4
```
**必須条件**:
- `-f psp` (PSP用MP4コンテナ形式) — これなしでは非対応データ
- Main Profile Level 3.0
- 29.97fps以下 (60fpsは不可)
- weightp=0, b-pyramid=none (PSP非対応機能無効化)
- 最大ビットレート 1500kbps
- 音声: AAC 128kbps 48kHz stereo

### 参照実装

#### pmplayer-advance (cooleyes) — NAL Auパス唯一の参照
- GitHub: DavisDev/pmplayer-advance
- ファイル: ppa/mod/mp4avcdecoder.c
- ringbuffer操作: 一切なし (memset 0のまま)
- sceMpegAvcDecodeMode: 呼ばない
- SceMpegAu: malloc_64(64) + memset 0xFF
- Detail2: 毎フレーム呼ぶ
- mode: timestamp==0 ? 3 : 0

#### mcidclan (m-c/d) — ME低レベル
- MECC: sceMpegと排他 (MEリセットベクタ上書き)
- CSCレジスタ直接制御: 0xBC800120-0xBC800160 (完全文書化、動作実績あり)
- VME DSP: ~~未解明 (CSCのみ達成)~~ → 2026年5-6月に大進展 (下記)
- safe-task: Sony ME FWのsyscallハンドラをパッチして共存

##### 【2026-07-04確認】VME解析の現在地
- 本丸リポジトリ2つ:
  - psp-media-engine-cracking-the-unknown (RE文書、star 23) — VMEデータパス/オペコード文書化
  - psp-virtual-mobile-engine-ext (2026-04-29新設) — **実用段階**。vme-ext.hライブラリ + 動作実績あるコード例5本 (mat4x4×vec変換×2、第2FUサチュレート/クランプ、RAMコンテキスト切替、MECC経由PoC)
- VME = "VME2" 再構成可能CGRA と正式に呼称。24bit Q23、48bit+アキュムレータ、PE×4
- bitstream仕様 v0.1→v0.3: PE毎descriptor (routing<<24 | transform<<8 | k) + DSPレジスタ(a,b)構造をマッピング。DMACへの誤アドレス指定が内部24bitリングバッファのデータ変換を偶然誘発したのが発見の起点
- 6月コミット: Saturator、MUXセレクタ、Pixel ALU Operationsセクション、back buffer clamp等の精緻化
- **H.264/VLD: 専用文書なし、未攻略** (READMEで最終ターゲット宣言のみ)
- 全リポジトリの活動が2026-06-24で一時停止 (7/4時点で約10日静か)

#### ルート評価 (2026-04-11確定)
- Route A (sceMpeg API): **唯一の現実的ルート**
- Route B (MECC+SW H.264): 性能不足 (333MHz MIPS32で5-10fps)
- Route C (MECC+HW blocks): VME文書なしで不可
- 【2026-07-04追記】Route C再評価余地あり: VME命令セット/データパスが公開文書化され「VME文書なし」の却下理由は消滅。ただしH.264/VLDデコードパス自体は未解明のため、moonlightの0x80628001の解には現時点で未達

---

## 開発タイムライン

### 2026-04-03: PMD Visualizer PS3版 ビルド完了
- pmdmini + tiny3D + cellAudio でPS3版先行

### 2026-04-05: PSP版 初期実装 + ME統合
- pmdmini + sceGu + sceAudio + MECC
- 3スレッドオーディオアーキテクチャ確定
- ME FM合成成功、リングバッファ256ブロック
- prefill=255 (途切れず優先、起動6秒許容)
- PSP ME Benchmark 公開 (GitHub)

### 2026-04-06: ソロモード + ビジュアライザ
- 11パート個別ON/OFF (FM6+SSG3+ADPCM+Rhythm)
- ピアノロール (KEYBOARD) + FMPステータス (FMP) 表示
- **地雷: ymfm generate()に分岐追加** → ME icache 16KB汚染、44→39 blk/sec。14時間浪費

### 2026-04-07: 全モデル性能等価実証 + ループキャッシュ
- PSP-1000/3000/Go: ME ~42-44 blk/sec 全同等
- ループキャッシュ (v1.1): 60秒事前デコード
- **地雷: scratchpadをMEに使用** → 24ms→39.6ms。クロスバスで逆に遅い

### 2026-04-08: 公開リリース + MECC v2.0.1移行 + コミュニティ参加
- GitHub公開 (psp-pmdvis)
- PSP homebrew Discord 参加初日で m-c/d + Acid_Snake に認識
- m-c/dから現行名はMECCだと指摘 → 即修正
- Acid_Snakeから「sepluginやめろ、RAM食う」→ auto-load方式に移行
- v2.0.1 + 田中パッチ4点でリビルド → PSP Go実機確認
- auto-load MECC完全動作確認 (embedded_kcall問題の根本原因特定)
- メモステ曲読み込み機能追加 (songs/*.M 自動読み込み)
- WAVエクスポート機能
- 曲削除機能 (日本語確認ダイアログ)
- edram-0x400000 テスト (m-c/d依頼): PSP Go/1000両方OK
- **地雷: SEARCH機能** → PSP Go ARK-4 CXPLでコード追加すると起動コケ。6回ビルド・デプロイ全滅、1時間浪費
- **地雷: USBカーネルモジュール** → sceIo系と競合して起動クラッシュ。USB削除指示を無視して6回失敗
- **地雷: -O3インライン化崩壊** → static関数の2箇所目参照で.textレイアウト激変→起動クラッシュ

### 2026-04-09: dcacheリファクタ + eDRAM調査 + OC実験
- v1.0.2 dcache リファクタ: meLibDcache 17箇所 → meCoreDcache置換
  - **パフォーマンス向上**: TH1が2ループ→3ループまで持つように
  - meLib版は2倍撃ち、meCore版はネイティブsyscall 1発
- ME eDRAMアーキテクチャ確定: ME 4MB + GE 4MB = 8MB (Slim+)
- Hot Chips 16 (2004) 一次資料発見: PSPチップは最初から4MB eDRAM
- **地雷: eDRAMテストコード追加** → PSP Go ARK-4で起動不可(54行追加)。前科2回目
- **地雷: ME OC (407MHz)** → PSP-3000で画面崩れ+フリーズ。棚上げ
- MECC API方針確定 (m-c/d): meCore優先、meLib例外のみ
- PSPメモリ4層階層確定 (スクラッチパッド/ME eDRAM/メインRAM/GE VRAM)
- PSP-3000でME_BENCH新ビルド起動不可 (原因未特定)
- ナレッジ管理システム刷新 (CLAUDE.md=憲法, KNOWLEDGE=知見)

### 2026-04-11: psp-moonlight Phase 0-3 デバッグ (11時間連続、未完了)
- sceMpeg NAL Auパスで H.264 デコード検証
- ARK-4 sceMpegハング発見 → 開発機をPSP-1000 PRO-B10に切替
- PPSSPPHeadless + auto_test.sh 開発基盤確立
- MP4直接パーサー実装 (avcC/stco/stsz/mdat、pmplayer完全同一データパス)
- PSP互換MP4生成確定 (ffmpeg -f psp必須)
- NID修正: RingbufferQueryMemSize=0xD7A29F46, RingbufferConstruct=0x37295ED8
- SceMpegAu malloc_64(64)+0xFF化 (pmplayer準拠)
- **検証済み仮説14件すべて無効** (再試行禁止)
- 0x80628001 (VIDEO_ERROR) の真因未特定
- mcidclan/pmplayer-advance/opentube-psp 全調査完了
- Route B (MECC+SW) / Route C (MECC+HW VME) 却下確定
- git commit db8d792 (master root)
- ハンドオフ文書: ~/Desktop/handoff_psp-moonlight_20260411/

---

## 踏んだ地雷 全記録

### PSP Go ARK-4 CXPL .text問題 (前科2回)
- **症状**: コードロジック追加で起動時黒画面→強制シャットダウン
- **原因**: .textセクションのアドレス変更。ARK-4 CXPLのELFローダーのアラインメントバグの可能性
- **安全な変更**: 文字列変更(.rodata)、未使用関数定義
- **危険な変更**: コードロジック追加は全て起動コケ
- **前科1**: SEARCH機能 (2026-04-08) — 6回全滅、1時間浪費
- **前科2**: eDRAMテスト (2026-04-09) — 54行追加で起動不可

### ARK-4 sceMpegハング (前科1回 2026-04-11)
- **症状**: sceMpegAvcDecodeエラー時にハング→ウォッチドッグ強制シャットダウン
- **影響**: PSP Go/PSP-3000共通。6回連続チェックポイント9停止で8時間浪費
- **教訓**: 症状(ハング)ではなく根本原因(デコードエラー)を見ろ。CFW切替で初めて判明

### ymfm generate() ホットパス汚染 (前科1回)
- **症状**: ME blk/sec が44→39に低下
- **原因**: ソロ機能の分岐追加でME icache 16KB汚染
- **結果**: 14時間浪費
- **教訓**: ymfmは聖域。一切の改変禁止

### scratchpadをMEに使用 (前科1回)
- **症状**: 24ms→39.6msに悪化
- **原因**: scratchpadはSCローカル。MEからはクロスバスで逆に遅い
- **教訓**: scratchpad=SC専用、eDRAM=ME専用

### USBカーネルモジュール (前科1回)
- **症状**: 起動クラッシュ
- **原因**: semawm/usbstor等がsceIo系と競合
- **結果**: SEARCH機能で1時間浪費、USB削除指示を無視して6回失敗

### -O3インライン化崩壊 (前科1回)
- **症状**: 起動クラッシュ
- **原因**: 1箇所からしか呼ばれないstatic関数に2箇所目参照追加 → インライン→非インライン化 → .textレイアウト激変
- **教訓**: 再呼び出しせずその場でデータ追加

### ME共有メモリ衝突 (前科1回)
- **症状**: MEクラッシュ+爆音
- **原因**: VIS_SIZE拡大でsh[40+]の制御スロットを上書き
- **教訓**: sh[8..30]=vis、sh[40..56]=制御、sh[60..103]=FMP。絶対に領域を侵すな

### embedded_kcall バージョン不整合 (2026-04-08)
- **症状**: 0x80020139 (SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE)
- **原因**: libme-core.a内のembedded_kcallが旧版(1,740B)のまま。v2.0.1のkcall.prx(2,198B)と不一致
- **解決**: 正しいEBOOT.PBPをデプロイ(ビルドし直し忘れ)

### U.N.オーエン フリーズ (2026-04-08)
- **症状**: 埋め込み(psp-objcopy)だとPSPフリーズ
- **回避**: メモステ(malloc)だと再生OK
- **原因(仮説)**: .rodataとヒープのメモリ領域差

### MECC命名違反 (前科複数回)
- **症状**: m-c/dから現行名への訂正を直接受けた
- **原因**: Claudeが廃止済みの旧リポジトリ名を使い続けた
- **修正**: psp-me-bench README日英 + コード全箇所

### ME OC PSP-3000フリーズ (2026-04-09)
- **症状**: 画面崩れ+フリーズ (expover.prx L+R+○)
- **原因(仮説)**: マザーボードリビジョンとPLL DEN=17の相性

### PSP-3000 ME_BENCH 新ビルド起動不可 (2026-04-09)
- **症状**: XMBリターン
- **原因**: 未特定。旧版(162,622B)は動く、新版(202,513B)は動かない
- **回避**: Git初期版EBOOTを使用

---

## 棚上げ項目
| 項目 | 理由 | 再開条件 |
|------|------|----------|
| 音色プレビューモード | D-pad左右反応しない不具合 | MECC移行後 (移行は完了) |
| SEARCH (メモステ検索) | ARK-4 CXPLコード追加で起動コケ | ARK-4修正 or PSP-1000で検証 |
| SC→ME eDRAM読み取り | ARK-4コード追加不可 + SC直読み不可の可能性 | m-c/dに確認 |
| ME OC (407MHz) | PSP-3000でフリーズ | 別のPSPモデル or m-c/dに相談 |
| eDRAMリングバッファ | SC直読み未検証 | SC→ME eDRAM読み取りの解決後 |
| ◯×リージョン対応 | ダイアログは対応済み、アプリ操作は未対応 | 優先度低 |
| PMDリズム(K channel) | PMDDotNET/pmdmini互換性なし | 上流修正待ち |

## 新規PSPプロジェクト開始時の必須順序
1. CLAUDE.md読込
2. github.com/mcidclan 全リポ読込
3. github.com/kan8223-dotcom 関連リポ読込
4. PPSSPPHeadless + auto_test.sh セットアップ
5. main.c実装開始
cooleyes系は補助参照のみ。
