# PSP Media Engine (ME) 知見

> **鮮度警告**: ローカルコピー。正本はサーバ側。TH06移植では **ME は当面不要** と考えてよい
> (ME は sceJpeg/sceMpeg の正規用途か、MECC による非正規の第2コア利用)。
> 60fps が CPU 単体で足りなくなった時に初めて開く章。先に測ること。

---

---
name: psp-me-knowledge
description: PSP Media Engine (ME) 完全知見ベース。Hot Chips 16一次資料、eDRAM、4層メモリ階層、FPU(VFPU無し・通常FPUはメインCPUと同等)、icache汚染、MECC v2.0.1統合、SC↔ME通信、性能実測、OC、ARK-4 CXPL罠、PMDVIS実装アーキテクチャ、sceMpeg NAL Auパス (psp-moonlight戦記含む)、NID表、CFW差異、H.264デコード知見、ME正規用途(sceJpeg/sceMpeg)vs非正規用途(MECC直叩き)整理、sceJpeg 480×272上限の真因(MEハードIP制約)、AVCODEC moduleがME駆動入口である理由、sceJpegエラー時のDelete+Create再initパターンを統合。「PSP」「ME」「MECC」「eDRAM」「sceMpeg」「H.264」「Moonlight」「ymfm」「icache」「PMDVIS」「VFPU」「FPU」「sceJpeg」「AVCODEC」「ME正規用途」で必ず参照。
---

# PSP Media Engine ナレッジ — PSPVIS EDITION

田中がPSP PMD Visualizer開発 (2026-04-03〜04-11) を通じて実証・発見したME知見の蒸留。
Hot Chips 16一次資料 + m-c/d (MECC作者) との技術議論 + 実機3台での実験結果に基づく。

---

## 1. MEとは何か — Hot Chips 16 (2004) 一次資料

**出典**: Hot Chips 16 (2004年8月), Sony Corporation 合同発表
**PDF**: https://old.hotchips.org/wp-content/uploads/hc_archives/hc16/3_Tue/8_HC16_Sess8_Pres1_bw.pdf

PSPチップの「Media Processing Unit」は**3つの独立コンポーネント**で構成される:

| コンポーネント | 正体 | 周波数 | homebrew利用 |
|---------------|------|--------|-------------|
| **Media Engine (ME)** | MIPS R4000 32bit + FPU (COP1) | 1-333MHz | 汎用利用中 (田中含む) |
| **AVC** | H.264 ハードウェアデコーダ | — | 未開拓 |
| **VME** | CGRA 再構成可能プロセッサ, 24bit幅 | 166MHz | 未開拓 |

- 3つとも**独立電源ON/OFF制御**可能
- VMEの正式名 = **Virtual Mobile Engine** (「Virtual ME」ではない)
- VME = 世界初の民生CGRA。Walkman→PSPと搭載。スペックは非公開 (m-c/d: "spec are not public")
- **「Sonyは隠してなかった。Hot Chips 16で2004年に堂々と発表。ただしユーザーには使わせなかった」**

---

## 2. eDRAM — 設計時点で4MB、2分割

```
┌──────────────────────────────────┐
│         PSP SoC eDRAM 4MB        │
├─────────────────┬────────────────┤
│  Game側 2MB     │  Media側 2MB   │
│  GE VRAM        │  ME / VME      │
│  0x04000000     │  0x00000000    │
│                 │                │
│  Slim+: 拡張可  │  Slim+: 4MB   │
│  sceGeEdramSet  │                │
│  Size(0x400000) │                │
└─────────────────┴────────────────┘
```

- **初代PSP-1000から物理的に4MB存在** (Sony公式では「2MB VRAM」としか記載せず)
- Game側: `0x04000000` base — 3D GPU (フレームバッファ/Z/テクスチャ)
- Media側: `0x00000000` base — ME専用高速メモリ
- Slim+: ME側4MBに拡張 (SC 4MB + ME 4MB = 合計8MB)
- ME スタックは eDRAM **末尾から下に成長** (Fat: 0x200000, Slim: 0x400000)
- **末尾 ~256KB はスタック用に避ける** (m-c/d助言)
- `sceGeEdramSetSize(0x400000)` = 非公開APIでGame側を4MBに拡張可 (Slim以降)

### eDRAM実測 (2026-04-08)
- PSP Go: `sceGeEdramSetSize(0x400000)` 動作OK
- PSP-1000: 描画差異あり → Acid_Snakeが「1Kが嫌がるかも」と慎重姿勢

---

## 3. PSPメモリ4層階層 (2026-04-09 確定)

| メモリ | 所有者 | サイズ | アドレス | SC | ME | レイテンシ |
|--------|--------|--------|----------|----|----|-----------|
| スクラッチパッド | **SC専用** | 16KB | 0x00010000 | 最速 | 遅い(クロスバス) | ~1cyc |
| ME eDRAM | **ME専用** | 2-4MB | 0x00000000 | 不可(推定) | 高速 | ~数cyc |
| メインRAM | 共有 | 32/64MB | 0x08000000 | ○ | ○(uncached推奨) | ~数十cyc |
| GE VRAM | GPU | 2-4MB | 0x04000000 | ○(空き分) | 不明 | — |

### 鉄則 (前科付き)
- **scratchpad = SC専用。MEに使うな。** クロスバスアクセスで逆に遅くなる
  - 前科: FMテーブル(2.5KB)をME scratchpadにコピー → 24ms→39.6msに悪化。即リバート
- **eDRAM = ME専用。** SCから直接読み取りは不可能の可能性が高い
  - 根拠: m-c/dのedram-dumpサンプルがmeCoreMemcpyでメインRAMに一旦コピーしてから読む
- **SC↔ME共有データはメインRAM uncachedで。** dcache writeback/invalidateを適切に

### SC→ME eDRAM直接読み取り (2026-04-09) — 未完了
- テストコード実装 → ARK-4 CXPLの.text問題で起動不可 → 棚上げ
- 結論: eDRAMリングバッファ構想は保留

---

## 4. ME FPU — VFPU無し、通常FPUはメインCPUと同等

**田中証言 (確定)**: 「MEはVFPUが使えないだけであとはメインCPUと同じだ」

- **COP1 (32bit float) あり、VFPU なし** — スカラFPUはAllegrexと同等
- 命令セットはメインCPUとほぼ同じ、icache/dcacheサイズも同じ
- 違いはVFPU命令と一部特殊命令のみ
- **「MEはFPUが弱い」は誤り。VFPUとFPUを混同するな。**
- **double = ソフトウェアエミュレーション (libgcc)** → 1命令50-100サイクル vs float 1-4サイクル

**含意**: mbedTLS等FPUを使うライブラリはME上で動く。将来構想: ME並列HTTPSスタック (curl + mbedTLS をME側で実行、メインCPU=UI/描画専念、ME=ネットI/O+JSONパース)。懸念: eDRAM 2MBへの収まりとSC↔ME通信オーバーヘッド。

### TSF (TinySoundFont) で踏んだ地雷

| アプローチ | 結果 |
|-----------|------|
| 全doubleをfloat一括変換 | 音が完全に壊れる (サンプル位置精度不足、ピッチズレ、IIR発振) |
| lowpassだけ無効化+double維持 | 毎サンプル4-6回のdouble演算残存、voice増加で破綻 |
| **int+frac分離方式 (正解)** | pitchRatio→int+float分離、sourceSamplePosition→int+float分離 |

### 正解パターン: int+frac分離
```
ピッチ計算 (tsf_timecents2Secsd): doubleのまま (1回/ブロック/voice)
pitchRatio → unsigned int pitchStepI + float pitchFracF
sourceSamplePosition → unsigned int tmpPos + float tmpFrac
レンダループ内: double演算ゼロ。int加算+float加算のみ
```

- lowpass biquad IIR: float精度で発振するため**無効化が正解** (音色への影響は軽微)

---

## 5. ME icache 16KB汚染 — ymfmホットパス問題

### 前科: ソロ機能 (2026-04-06〜07) — 14時間浪費

ymfm_opn.cppの`generate()`/`clock_fm_and_adpcm()`は毎サンプル呼ばれるホットパス。
ME (333MHz, **icache 16KB**) ではif分岐1個の追加コストが致命的。

| 変更 | ME blk/sec | 影響 |
|------|-----------|------|
| 変更前 | 44 | 正常 |
| ソロ分岐追加 (solo_mask==0でも) | 39 | デコード29秒→33秒→ループキャッシュ破綻 |
| ソロ全削除 | 44 | 復帰 |

- **分岐命令自体がicache/パイプラインを汚す** — `solo_mask==0なら高速パス`は不十分
- ソロ再実装するならymfm**外部** (Mix後のバッファ) でゲートする設計にすべき

### 教訓
**ymfmは聖域。generate()に一切の分岐を追加するな。**

---

## 6. MECC (me-custom-core) 統合知見

### 正式名称 (絶対遵守)
- フルネーム: **psp-media-engine-custom-core**
- 略称: **MECC**
- 廃止済みの旧リポジトリ名は**使用禁止** (m-c/dから直接指摘済み)

### MECC v2.0.1 + 田中パッチ4点

| ファイル | パッチ内容 |
|---------|-----------|
| me-core-custom.c | meLibLoadPrx()戻り値チェック除去 (auto-load対応) |
| me-lib.c | sceKernelExitGame()削除 |
| me-lib.h | #ifdef __cplusplus + (uintptr_t)キャスト + meLibMakeSharedUncachedMemマクロ |
| kcall.S | 田中版独自ファイル |

### auto-load方式 (seplugins不要)
```
起動 → writePrx() → 2,198B kcall.prx をms0:に書き出し
     → pspSdkLoadStartModule()
     → ME初期化完了 — EBOOT.PBP単体でME ON
```
- embedded_kcall: **2,198B = v2.0.1正解版**。旧1,740Bは壊れている
- Acid_Snake助言: 「sepluginやめろ、RAM食う」

### MECC API方針 (m-c/d確定 2026-04-09)
**原則: meCore系を使え。meLib系はmeCoreに該当がない場合のみ。**

| 項目 | 推奨API | 理由 |
|------|---------|------|
| ME側 dcache | `meCoreDcache...` | meLib版はcache命令を**2倍撃ち** (defensive)。meCore版はネイティブsyscall 1発 |
| SC側 dcache | `sceKernelDcache...` | PSP公式SDK |
| uncachedバッファ | writeback/invalidate**不要** | 呼び出し自体削除可 |
| ME側 icache | `meLibIcacheInvalidateAll()` | meCore版が存在しない |

### v1.0.2 dcacheリファクタ実測 (2026-04-09)
- meLibDcache 17箇所 → meCoreDcache置換
- **パフォーマンス向上**: TH1が2ループ→3ループまで持つように
- ソロ導入時の速度低下の一部もdcacheオーバーヘッドだった可能性

### VME停止
- VMEがデフォルトでeDRAMにバックグラウンド処理
- `meLibSetMinimalVmeConfig()` で停止 (MECC標準手順)

---

## 7. SC↔ME通信パターン (田中実装)

### 共有メモリマップ (uncachedメインRAM)
```
sh[0..7]    = (予約)
sh[8..30]   = vis (ビジュアライザデータ: 周波数/音量/ノート)
sh[40..56]  = 制御 (コマンド/ステータス/パラメータ)
sh[60..103] = FMP (FMPステータス表示データ)
```
**領域を侵すな。前科1回: VIS_SIZE拡大でsh[40+]上書き → MEクラッシュ+爆音**

### コマンド通信フロー
```
SC: sh[CMD] = ME_CMD_XXX; dcache writeback
    → ME: polling sh[CMD] → 処理 → sh[STATUS] = DONE; dcache writeback
    → SC: sh[STATUS] 読み取り (uncachedアドレス)
```

### ME側カーネル関数呼び出し (コミュニティ議論段階)
1. SC側: `pspXploitFindFunction()` でsce関数のカーネルアドレスを解決
2. 共有メモリ経由でMEにアドレスを渡す
3. ME側: `jalr`で直接呼び出し
- **ME側にNIDsリゾルバが存在しない** → SC側でアドレス解決必須
- ファイルI/O系は動く可能性あり、HWレジスタ依存関数は不明

### POPS (PS1エミュ) のME利用
- PS1サウンド処理にMEを使用
- ME固有コードが**$ra (リターンアドレスレジスタ) を演算に流用**
- SC↔ME間でコードは単純に入れ替え不可 → ABIが異なる実例

---

## 8. ME性能実測

### 全モデル等価実証 (2026-04-07)
| 機種 | CFW | ME blk/sec | 結果 |
|------|-----|-----------|------|
| PSP-1000 | PRO-B10 | ~42-44 | 同等 |
| PSP-3000 | ARK-4 r205 | ~42-44 | 同等 |
| PSP Go | ARK-4 CXPL | ~42-44 | 同等 |

- kcall.prx: 全機種同一バイナリで動作
- **ME 333MHzの物理限界**: ~0.86xリアルタイム (ymfm YM2608)

### ME Benchmark (公開済み)
- https://github.com/kan8223-dotcom/psp-me-bench
- SC/DUAL × 222/333MHz、PI/PRIME/MANDEL 3テスト
- **DUAL 333MHz = SC 222MHz比で平均×3.48**

### ME速度の壁 (確定)
- v1.1-stable-ring256 = ME 333MHzでのベストバランス
- prefill=32は却下: 起動0.85秒だが1周目後半でr=0到達
- prefill=255採用: 起動~6秒、途切れず優先

---

## 9. MEオーバークロック (2026-04-09) — 棚上げ

### PLL方式
- PLL基準 = 37MHz水晶
- `PLL_FREQ = 37 × (NUM / DEN)`
- 333MHz: NUM=153/DEN=17、407MHz: NUM=187/DEN=17
- **MEもSCも同じPLLから派生** → PLLをOCすればMEも速くなる

### HWレジスタ
| レジスタ | 用途 |
|----------|------|
| 0xBC1000FC | PLL乗数 (NUM/DEN) |
| 0xBC100068 | PLL制御 |
| 0xBC200000 | CPUドメイン比率 |
| 0xBC200004 | バスドメイン比率 |

### PSP-3000テスト結果 — 失敗
- expover.prx (m-c/d作、6,718B) seplugin読み込み成功
- L+R+○ でOC発動 → **画面崩れ+フリーズ** (強制電源オフ必要)
- 原因: マザーボードリビジョンとPLL DEN=17の相性 (KOMSTATION記事)
- **棚上げ** — PMDVISには使わない

---

## 10. ARK-4 CXPL .text問題 (前科2回)

ME関連コードの追加を阻むPSP Go固有の壁。

- **症状**: コードロジック追加で起動時黒画面→強制シャットダウン
- **原因**: .textセクションのアドレス変更。ARK-4 CXPLのELFローダーバグの可能性
- **安全な変更**: 文字列変更(.rodata)、未使用関数定義
- **危険な変更**: コードロジック追加は全て起動コケ
- **前科1**: SEARCH機能 (04-08) — 6回全滅、1時間浪費
- **前科2**: eDRAMテスト (04-09) — 54行追加で起動不可
- **-O3インライン化崩壊** (関連): static関数の2箇所目参照追加でインライン→非インライン化→.textレイアウト激変→起動クラッシュ

### PSP Goにコード追加禁止。PSP-1000 PRO-B10で開発すること。

**→ §14 参照**: psp-moonlight 実機検証で ARK-4 は sceMpegAvcDecode エラー時にハング→ウォッチドッグ強制シャットダウンすることが確定 (PRO-B10 は正常にエラー値返却)。ARK-4 不適・PRO-B10 開発機の根拠がさらに強化された。

---

## 11. PMDVISでのME活用パターン (実装済みアーキテクチャ)

### 3スレッド + ME + リングバッファ + ループキャッシュ
```
┌─ audio_producer (0x20) ─┐     ┌─ ring 256blk ─┐     ┌─ audio_output (0x10) ─┐
│ ME render (ymfm YM2608) │────▶│ PCM 22050Hz   │────▶│ sceAudio出力          │
│ → upsample 2x (int演算) │     │               │     │ + vis_current更新     │
│ → ring書き込み           │     │               │     └───────────────────────┘
│ + visスナップショット    │     └───────────────┘              │ vis_current
└─────────────────────────┘                                    ▼
                                                     ┌─ main (0x20) ─┐
                                                     │ ビジュアライザ │
                                                     │ 描画 (sceGu)  │
                                                     └───────────────┘
```

### ループキャッシュ
- ME FM合成が実時間より遅い (~0.86x)
- 解決: **60秒分PCMを事前デコード** (eDRAMストリーミング)
- ME_CMD_DECODE_LOOP: MEにブロック一括デコードさせるコマンド
- 動的確保: 64MB機=60秒, 32MB機=30秒

### eDRAM使用
- edram_buf = 0x00000400 から約2KBのみ使用 (残り大量に空き)
- eDRAM API不使用 (直接アドレス指定。APIはMEクラッシュ)

---

## 12. 未解明領域 (2026-04-11時点)

- **sceMpegAvcDecode 0x80628001 の真因** (§14 参照): pmplayer-advance完全準拠のAPI呼出・NAL Au構築・Mp4AvcNalStruct配置でも発生。検証済み無効仮説8件 (Annex B変換/AU初期化/DecodeMode/mpeg_mode 4-5/SEI/CABAC-CAVLC/Ringbuffer Construct/PSPネイティブMP4)。残選択肢: pmplayer実機ビルド / PPSSPP HLE精読 / MECC経由。

| 領域 | 状態 | 障壁 |
|------|------|------|
| VME (CGRA) プログラミング | **完全未開拓** | スペック非公開。mcidclanでもCSCのみ達成 |
| AVC H.264 HWデコーダ直接制御 | sceMpeg API経由のみ | VME/AVC/ME連携の内部動作は不透明 |
| SC→ME eDRAM直接読み取り | 不可能の可能性高 | meCoreMemcpyでメインRAMに一旦コピー必要 |
| ME側NIDsリゾルバ | 存在しない | SC側でアドレス解決→共有メモリ経由 |
| MEドメイン個別OC | レジスタ未文書化 | PLLはSC/ME共有 |
| 4層メモリ全活用プロジェクト | 誰もやったことがない | scratchpad+eDRAM+メインRAM+VRAM同時活用 |

---

## 13. コミュニティ関係

### キーパーソン
- **m-c/d (mcidclan)**: MECC作者。ME低レベル知見、CSCレジスタ、VME研究。Hot Chips 16 PDF共有者
- **Acid_Snake**: ARK-4メイン開発者。seplugin→auto-load方式、eDRAM拡張方針を助言

### 田中の立ち位置 (2026-04-08以降)
- **MECC実用ユーザー第1号**: psp-pmdvis = MECC v2.0.1 + auto-loadの初の実用アプリ
- PSP-1000 eDRAMテストデータ提供 → Acid_Snakeの設計判断材料
- embedded_kcall旧版問題報告 → m-c/dがupstreamにガード実装

---

## 14. sceMpeg NAL Auパス — psp-moonlight戦記 (2026-04-11)

> **本章は psp-moonlight プロジェクト (PSP用Moonlightクライアント、世界初) の Phase 0-3 戦記で判明した sceMpeg H.264デコード知見。** §10 (ARK-4 CXPL問題) および §12 (未解明領域) と密接に関連。2026-04-11 時点で 0x80628001 未解決、継続調査中。


### sceMpeg NAL Auパス — 確定した事実

#### API呼出順序 (pmplayer-advance完全準拠で確認)
```
sceUtilityLoadAvModule(AVCODEC)       → 0x00000000
sceUtilityLoadAvModule(MPEGBASE)      → 0x00000000 (PRO-B10)
sceMpegInit()                         → 0x00000000
ddrtop = memalign(0x400000, 0x200000) → 4MB境界、2MBブロック
mpeg_data = malloc_64(sceMpegQueryMemSize(mode))
au_buffer = ddrtop + 0x10000
sceMpegRingbufferQueryMemSize(100)    → 215200 (PRO-B10)
sceMpegRingbufferConstruct()          → 0x00000000 (PRO-B10)
sceMpegCreate()                       → 0x00000000
// sceMpegAvcDecodeMode は呼ばない (pmplayer準拠)
au = malloc_64(64); memset(au, 0xFF, 64)
sceMpegInitAu()                       → 0x00000000

// Per frame:
sceMpegGetAvcNalAu()                  → 0x00000000 (auSize正常)
sceMpegAvcDecode()                    → 0x80628001 ★ここで失敗
```

#### NID確定値 (PRO-B10 FW6.60で動作確認済み)
| NID | 関数名 | 状態 |
|-----|--------|------|
| 0x682A619B | sceMpegInit | 動作 |
| 0x874624D6 | sceMpegFinish | 動作 |
| 0xD8C5F121 | sceMpegCreate | 動作 |
| 0x606A4649 | sceMpegDelete | 動作 |
| 0xD7A29F46 | sceMpegRingbufferQueryMemSize | 動作 |
| 0x37295ED8 | sceMpegRingbufferConstruct | 動作 |
| 0x13407F13 | sceMpegRingbufferDestruct | 動作 |
| 0xC132E22F | sceMpegQueryMemSize | 動作 |
| 0xA11C7026 | sceMpegAvcDecodeMode | 動作(不要) |
| 0x167AFD9E | sceMpegInitAu | 動作 |
| 0x11F95CF1 | sceMpegGetAvcNalAu | 動作(成功返す) |
| 0x0E3C2E9D | sceMpegAvcDecode | 動作(0x80628001返す) |
| 0x707B7629 | sceMpegFlushAllStream | 動作 |
| 0xCF3547A2 | sceMpegAvcDecodeDetail2 | **NID未リンク** (0x8002013A) |
| 0x0F6C18D7 | sceMpegAvcDecodeDetail | 動作(デコード失敗時0x806201FE) |
| 0x91929A21 | sceMpegBaseCscAvc | 未到達 |

#### PRO-B10 固有の挙動
- sceMpegAvcDecodeDetail2 (NID 0xCF3547A2) が**未リンク**。FW6.60のsceMpegモジュールに存在しない可能性
- sceMpegAvcDecodeDetail (NID 0x0F6C18D7) はリンク済みだが、デコード未成功時は0x806201FE
- sceMpegAvcDecode エラー時もハングしない (ARK-4はハングする)

#### エラーコード実測
| コード | 発生条件 (実測) |
|--------|----------------|
| 0x80628001 | Main Profile Level 3.0 480x272 → VIDEO_ERROR |
| 0x80628002 | Baseline Profile Level 3.0 480x272 → VIDEO_FATAL (悪化) |
| 0x80628002 | Main Profile 720x480 + mpeg_mode=4 → サイズ不整合 |
| 0x8002013A | NID未リンク (Detail2) |
| 0x806201FE | デコード未成功状態でDetail呼出 |

#### sceMpegQueryMemSize 実測
| mode | 返却サイズ | 用途 |
|------|-----------|------|
| 4 | 65536 | 480x272まで |
| 5 | 65536 | 720x480まで (Level 3.0) |

#### sceMpegRingbufferQueryMemSize 実測
| packets | 返却サイズ |
|---------|-----------|
| 100 | 215200 |

### CFW差異 (実機検証)

#### ARK-4 (PSP Go / PSP-3000)
- sceMpegAvcDecode エラー → **ハング** → ウォッチドッグ強制シャットダウン
- sceUtilityLoadAvModule(MPEGBASE) → **0x80020F3C** (壊れてる)
  - 回避: flash0:/kd/mpeg_vsh.prx を sceKernelLoadModule+StartModule
- sceIoOpen APPEND → **失敗** (log_resume不可)
- **開発機として不適**

#### PRO-B10 (PSP-1000)
- sceMpegAvcDecode エラー → **正常に値返却** (クラッシュなし)
- sceUtilityLoadAvModule(MPEGBASE) → **0x00000000** (正常)
- sceIoOpen APPEND → 正常動作
- ログ完全取得可能
- **開発機確定**

### PSP互換H.264生成

#### ffmpeg正解コマンド (-f psp 必須)
```bash
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

#### PSP動画再生の必須条件
- `-f psp` がないと「非対応データ」(前科2回)
- 60fps → 不可、29.97fps以下
- weightp=0, b-pyramid=none (PSP非対応機能)
- 最大ビットレート ~1500kbps
- Baseline Profile → 0x80628002 (FATAL) に悪化。**Main Profile推奨**

### Mp4AvcNalStruct (32バイト、pmplayer-advance準拠)
```c
typedef struct {
    void*    sps_buffer;      // offset 0x00: 生SPS (NALヘッダ含む、スタートコードなし)
    int      sps_size;        // offset 0x04
    void*    pps_buffer;      // offset 0x08: 生PPS
    int      pps_size;        // offset 0x0C
    int      nal_prefix_size; // offset 0x10: avcCから取得 (通常4)
    void*    nal_buffer;      // offset 0x14: AVCC形式 (4バイトBE長+NALデータ)
    int      nal_size;        // offset 0x18: prefix含む全長
    int      mode;            // offset 0x1C: 3=初回IDR, 0=以降
} Mp4AvcNalStruct;
```

### PPSSPP HLE制限
- sceMpegGetAvcNalAu (0x11F95CF1): **完全UNIMPL**
- sceMpegBaseCscAvc (0x91929A21): **UNIMPL**
- sceMpegBaseCscVme: **Unknown syscall**
- sceMpegBaseYCrCbCopyVme: **Unknown syscall**
- NAL Auパスの実機検証はPPSSPP不可、PSP実機必須
- ロジック層 (パース/I/O/メモリ管理) のみPPSSPP検証可

### ルート評価 (確定)
- **Route A (sceMpeg API)**: 唯一の現実的ルート。0x80628001の解決が必須
- **Route B (MECC + SW H.264)**: 却下。333MHz MIPS32 SIMDなしで5-10fps
- **Route C (MECC + HW VME)**: 却下。VME DSPプログラミング文書なし

### 参照実装
- **pmplayer-advance** (DavisDev/pmplayer-advance): NAL Auパス唯一の公開実装
  - ppa/mod/mp4avcdecoder.c: デコーダ本体
  - libmp4info.psp/atom.c: avcCパーサー (read_avcC_atom)
  - ppa/mod/mp4_read.c: mdatサンプル読み出し
- **mcidclan**: ME低レベル (MECC、CSCレジスタ、safe-task)
- **opentube-psp** (yne): NAL Auパスの別実装 (参考)

## 付録: 地雷早見表 (ME関連のみ)

| 地雷 | 被害 | 教訓 |
|------|------|------|
| ymfm generate()に分岐追加 | 14時間 | **ymfmは聖域** |
| scratchpadをMEに使用 | 24ms→39.6ms | **scratchpad=SC、eDRAM=ME** |
| ME共有メモリ領域侵犯 | MEクラッシュ+爆音 | **sh[]マップ厳守** |
| embedded_kcall旧版 | ME起動失敗 | **2,198B = 正解版** |
| meLibDcache使用 | TH1 2ループ限界 | **meCoreDcache使え** |
| double演算 (ME上) | 50-100倍遅 | **float + int+frac分離** |
| ARK-4 CXPLコード追加 | 起動不可×2 | **PSP Goにコード追加禁止** |
| ME OC 407MHz | フリーズ | **個体差大、棚上げ** |
| sceJpeg>480×272 | 0x80650020 | **libjpeg (CPU) に切り替え** |
| AVCODEC未ロードでsceJpeg | silent fail | **sceUtilityLoadModule必須** |

---

## 15. ME 正規用途 vs 非正規用途

Sony設計時にMEはメディア処理専用コプロセッサとして想定された。

| 用途 | API | Sony想定 | 田中プロジェクト例 |
|------|-----|----------|------------------|
| JPEG画像表示 | sceJpeg | 正規 | psp-bskyアバター |
| 動画再生 | sceMpeg | 正規 | — |
| ATRAC音声デコード | sceAtrac3plus | 正規 | — |
| FM音源合成 | MECC直叩き | 非正規 | psp-pmdvis |
| H.264ストリーム | sceMpeg悪用 | 非正規 | psp-moonlight |
| HTTPS+JSONパース | (構想) | 非正規 | 将来 |

**競合注意**: 正規API (sceJpeg/sceMpeg) とMECC直叩きは同じMEハードを別経路で使う。同時起動で競合する可能性大。psp-pmdvis直後にpsp-bsky起動するとME状態残留でsceJpeg failする症状の可能性あり (未検証)。

---

## 16. sceJpeg 480×272上限はMEハード仕様

`sceJpegDecodeMJpeg` の実機上限はME内部のMJPEG decoder ハードウェアIP制約。SonyがPSP画面サイズ+UMD MJPEG movie仕様にロックしている。

- sceJpeg = MEのHW decoder IP直叩き
- libjpeg = メインCPU (Allegrex) ソフトデコード
- 大画像はMEハードが使えないのでCPU側で処理するしかない
- **並列化案**: メインCPUでlibjpeg走らせている間にMEをsceAtrac等別用途で並走させられる可能性あり

---

## 17. AVCODEC モジュール = ME駆動の入口 (真因)

`PSP_MODULE_AV_AVCODEC` はME上で動くcodec系 (sceJpeg/sceMpeg/sceAudiocodec/sceVideocodec) の共通基盤。明示ロードしないとME関連APIが使えない理由は、**カーネルmoduleがME起動シーケンスを担当している**から。

PPSSPPはMEを完全エミュレートしていないのでmoduleロード省略でもcodec APIが動く (host OSのlibjpeg/ffmpeg等で代替実行)。実機は本物のMEを起動する必要があるのでmodule必須。

**シャットダウン順序** (逆順でME state残留リスク):
```c
sceJpegDeleteMJpeg();   /* decoder context解放 */
sceJpegFinishMJpeg();   /* sceJpeg subsystem終了 */
sceUtilityUnloadModule(PSP_MODULE_AV_AVCODEC);  /* ME module解放 */
```

---

## 18. sceJpeg エラー時 Delete+Create で再 init

ARK-4 CXPL / PSP Go でsceJpeg系エラー後にME stuck する可能性あり (sceMpegと同じMEを使う系統)。エラーハンドリングはdecoder作り直しで対応。

```c
int dres = sceJpegDecodeMJpeg(buf, len, out, 0);
if (dres <= 0) {
    /* 再decodeは避ける、decoder作り直し */
    sceJpegDeleteMJpeg();
    sceJpegCreateMJpeg(maxW, maxH);
    /* 失敗フラグ立てて呼び出し側に返す */
}
```

---

---

## 14補遺: 0x80628001 完全解決 + Phase 1a/1b (2026-07-04 実機確定)

### 真因 (仮説15的中)
**NAL AuパスはVSH版mpegモジュール (flash0:/kd/mpeg_vsh.prx) 限定の機能。ゲーム版mpeg.prxではMEがNAL Auを拒否し0x80628001を返す。**
- pmplayer-advance codec_prx.c が根拠: sceUtilityLoadAvModule(0)→mpeg_vsh.prxを必ずロード、**MPEGBASE utilityは呼ばない** (呼ぶとゲーム版がsceMpegを提供してしまう)
- ロード: user modeのsceKernelLoadModuleは**0x80020148で拒否** → **kuKernelLoadModule (kubridge)** で成功。SDK: sdk/include/kubridge.h + -lpspkubridge。PRO/ARK-4両対応
- 実装フィンガープリント: sceMpegQueryMemSize(4) = **49839 (VSH版)** / 65536 (ゲーム版)。この値で「どっちのsceMpegに繋がってるか」判別可能

### 追加確定事項
- **0x80618007 = B-frame起因**: DPBに並べ替え保留があると次AUのGetAvcNalAuが拒否される。**bframes=0必須** (リトライ20回×10msでは解決しない)。Moonlight実運用はbframes=0なので実害なし
- **pic_num遅延 = 4AU** (bframes=0でも)。30fps換算で約100msのデコーダ内部遅延。レイテンシ設計の基準値
- **SPS HRDでMEフリーズ**: ffmpegのmaxrate/bufsize (VUI/HRD生成) 付きストリームでME内ハング (強制電源断)。rc-lookahead=0/sync-lookahead=0は**無罪** (実機確認)。Sunshine実ストリーム対応にはSPS patching (本家moonlightクライアント方式) が必要
- **ARK-4大幅改善 (2026-07時点)**: sceMpegエラーでハングせず正常返却 / MPEGBASE utility 0x00000000 / sceIoOpen APPEND動作。**PSP GoがsceMpegテスト機として使用可能に** (4月の記録は旧版ARK-4の話)

### Phase 1a/1b 成立 (test/net_stream_test/)
- PC ffmpeg → UDP生H.264 → PSP WiFi受信 → Annex Bパース → AVCC変換 → mpeg_vshデコード → CscAvc → VRAM、**30fps張り付き**
- デスクトップミラーリング動作 (Windows gdigrab、YouTube再生可)。PSPパッド→PCマウス制御60Hz (hello方式でFW穴あけ、全二重同一ソケット)
- 診断HUD手法: 画面に心拍/rx/nal/au/err + ステージ色矩形 → フリーズ画面自体が死因を語る (ログ不要)
- PSPはiPhoneテザリング (WPA2) に接続できた実機事実あり (通説に反する。互換性優先設定、ch6)
