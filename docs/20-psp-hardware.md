# PSPハードウェア知見 (Tachyon SoC / eDRAM / ME / MECC / 機種差 / 実戦地雷)

> **鮮度警告 (2026-08-03 引き渡し時点)**
> この文書は母艦ローカルのコピーであり、**正本ではない**。正本はサーバ側の
> `psp-hardware-knowledge` 新版で、そちらにしか無い記録がある (例: psp-bsky案件の地雷記録、
> preflight 4項目の正式根拠)。**ここに書かれていない = 存在しない、と判断してはいけない。**
> 判断が割れたらサーバ側正本を取り寄せて照合すること。
>
> 文中の信頼度マーキング (※) の規則は冒頭章に定義がある。**※付きは未検証**として扱え。

---

---
name: psp-hardware-knowledge
description: PSP ハードウェア + homebrew 開発知見の統合知識ベース。Tachyon SoC/eDRAM/ME/VME/MECC v2.0.1/機種差/dcache/icache/VRAM直叩き/リングバッファ/ymfm聖域/submodule汚染罠/コミュニティ(m-c/d/Acid_Snake)に加え、実機罠集(Wi-Fi sceNetApctlConnect直叩き・AVCODECモジュール事前ロード・sceJpeg実機サイズ制限+libjpeg・GU矩形drop→CPU-blit・FAT32 fflush罠・OSK kanji不可・HtmlViewer 8MB Vpl・Go 64MBヒープ実測・PPSSPP vs実機差分一覧)を収録。PSP homebrew開発・MECC移行・ME活用・VRAM直叩き・PSPコミュニティ対応・PSP同人誌執筆で必ず使用。「PSPのメモリ」「eDRAM」「ME」「MECC」「PPSSPP実機差分」「sceJpeg」「libjpeg」「OSK」「FAT32」「fflush」「HtmlViewer」「Wi-Fi」「sceNetApctl」「ヒープ実測」「Tachyon」「CXD2962GG」「ymfm聖域」「ring_blocks」「prefill_wait」等で必ず参照。
---

# PSP ハードウェア + 開発知識ベース (統合版)

最終更新: 2026-04-09。psp-hardware-knowledge / psp-pmdvis-day-20260408 / pmd-psp-session-20260407 の 3 スキルを統合。2026-04-09 に AI ハルシネーション訂正 4 件 + ※マーキングルール導入 + 新発見 (起動デモ正体、Tiny-ME 真相、Sony 純正 ME プログラム用途、psdevwiki 貢献者実名) 追記。

## ★★★ 信頼度表記ルール (※ マーキング) ★★★

本 skill 内の全記述は以下の信頼度に分類される。**発信時 (同人誌・Note 記事・Discord・コミュニティ対応等) は信頼度を必ず確認すること**。

| マーク | 意味 |
|---|---|
| **(無印)** | **Rank A**: Sony 公式文書 (Hot Chips 16 等) または田中本人の実機実測で確認済み。断定可 |
| **※** | **Rank B**: 二次情報源のみで確認、一次情報源未追跡 |
| **※※** | **Rank C**: コミュニティ個人研究のみが根拠、または Sony 公式記述と矛盾あり |
| **※※※** | **Rank D**: Claude/筆者の解釈・推測・補間・繋ぎの論理 |
| **❌** | **過去のハルシネーション確定事項** (訂正済み、告白対象) |

**重要**: ※ が付いた記述は **事実として扱わない**。外部発信時は出典明記または留保必須。

---

## AI ハルシネーション訂正履歴 (2026-04-09)

本 skill は過去に以下の 4 件のハルシネーションを含んでいた。m-c/d 本人の Discord 指摘および一次情報確認で訂正済み。

### ❌ 訂正 1: CXR704060 は PSP SoC 型番ではない
- **誤**: PSP SoC の型番が CXR704060
- **正**: コードネーム **Tachyon**、Sony 型番 **CXD2962GG** (基板刻印 "Sony Computer Entertainment Inc. CXD2962GG ©2004SCEI" で確認)
- 検出: 2026-04-08 23:39 Discord、m-c/d 本人から指摘
- 一次情報源: Hot Chips 16 (2004) p.14 + Wikimedia Commons 基板写真

### ❌ 訂正 2: Tiny-ME ライブラリ名累犯
- **誤**: MECC を "Tiny-ME" と表記 (複数媒体で累犯)
- **正**: **psp-media-engine-custom-core (MECC)**
- 真相: 2024-25 年頃、m-c/d 本人が pspdev org 下で `tiny-me` という名前で運営していた旧リポジトリ名 (実在) だった。その後 `mcidclan/psp-media-engine-custom-core` にリネーム。田中のローカル環境に旧 submodule フォルダ `tiny-me/` が残存し、Claude がフォルダ名を直接参照して「Tiny-ME」と書き続けた。**純粋な無からのハルシネーションではなく、情報の古さ + ローカルファイル構造への依存 + ルール化の限界の複合的な誤り**。
- 検出: 2026-04-08 20:21 Discord、m-c/d "it is not Tiny ME"
- **旧称 (Tiny-ME / TinyME / t***-me / T***yMe 等) 使用禁止** (憲法第 8 条適用)

### ❌ 訂正 3: VME = Virtual Mobile Engine 2 (書面未確認)
- **誤**: "Virtual Mobile Engine **2**"
- **正**: **"Virtual Mobile Engine" (VME)** (「2」なし)
- 書面一次情報源 4 つ全てで「2」の表記なし:
  - Hot Chips 16 (2004, Sony 公式): "Virtual Mobile Engine™"
  - m-c/d psp-media-engine-reload README: "VME (Virtual Mobile Engine / DSP)"
  - psdevwiki Tachyon / VME ページ: "programmable DSP dubbed 'Virtual Mobile Engine'"
  - Wikipedia PSP hardware: "Virtual Mobile Engine"
- 「2」が登場するのは m-c/d 本人の Discord 発言 (2026-04-08 23:35) "virtual mobile engine 2, a type of cgra trusting Sony" の 1 回のみ、本人が "trusting Sony" と留保を付けている
- **書面一次情報を優先、本 skill では "Virtual Mobile Engine (VME)" 表記**

### ❌ 訂正 4: VME = CGRA (学術分類、Sony 公式ではない)
- **誤**: VME を「CGRA」と断定
- **正**: Sony 公式 (Hot Chips 16) は **"Reconfigurable processor"** / **"Dynamic Reconfigurable Architecture"** と表記
- 「CGRA」(Coarse-Grained Reconfigurable Architecture) は **学術分類**であり、Sony 公式分類ではない
- Hot Chips 16 p.11 には **"World's First Consumer Product using Reconfigurable architecture!"** (Sony の主張、事実として記載可) — ただし "Reconfigurable architecture" であり "CGRA" ではない
- m-c/d の Discord 発言に "a type of cgra" はあるが、これは m-c/d の学術的解釈
- **本 skill では "Reconfigurable processor" / "Dynamic Reconfigurable Architecture" を優先、CGRA は m-c/d の学術分類として脚注扱い**

---

## 目次

1. [公式 vs 実測スペック](#公式-vs-実測-スペック比較)
2. [Tachyon SoC と eDRAM 構造](#tachyon-soc-と-edram-構造)
3. [eDRAM メモリマップ](#edram-メモリマップ)
4. [メモリ階層全体像](#メモリ階層全体像-slim)
5. [SC 側 eDRAM (VRAM) 活用](#sc-側-edram-vram-の空き領域活用)
6. [ME (Media Engine) 詳細](#me-media-engine-詳細)
7. [Sony 純正 ME プログラム用途](#sony-純正-me-プログラムの用途判明分)
8. [dcache/icache 使い分け](#dcacheicache-使い分け-m-cd-公式ガイドライン)
9. [VME (Virtual Mobile Engine)](#vme-virtual-mobile-engine)
10. [機種別スペック + 田中 3 台体制](#機種別スペック一覧)
11. [eDRAM 拡張 API](#edram-拡張-api)
12. [OC (オーバークロック)](#oc-オーバークロック)
13. [MECC 実用ガイド](#mecc-psp-media-engine-custom-core-実用ガイド)
14. [MECC/ME 起動サンプルの系譜](#meccme-起動サンプルの系譜)
15. [実戦知見 / 開発 Tips](#実戦知見--開発-tips)
16. [一次情報源 + コミュニティ](#一次情報源)
17. [未検証/未開拓領域](#未検証未開拓領域-2026-04-09-時点)
18. [歴史 (圧縮版)](#歴史-圧縮版)
19. [注意事項](#注意事項)

---

## Tachyon SoC と eDRAM 構造

### PSP SoC の正体 (Rank A: 最高信頼度)

| 項目 | 値 | 一次情報源 |
|---|---|---|
| コードネーム | **Tachyon** | psdevwiki Tachyon ページ + PSP Bible |
| Sony 型番 | **CXD2962GG** | Wikimedia Commons 基板実物刻印 |
| 刻印 | "Sony Computer Entertainment Inc. CXD2962GG ©2004SCEI" | Wikimedia File:PSP_TA-079_Mainboard.JPG |
| 製造元 | Sony Semiconductor Kyushu (長崎) | Hot Chips 16 p.14 |
| プロセス | 90nm CMOS, 7 layer Cu | Hot Chips 16 p.14 |
| ゲート数 | 6M gates | Hot Chips 16 p.14 |
| パッケージ | 540pin LFBGA | Hot Chips 16 p.14 |

### Tachyon に統合されているもの (Hot Chips 16 ブロック図より)
- Allegrex (メイン CPU) + VFPU
- Media Engine (ME CPU, Allegrex 相当)
- Graphics Engine (GE)
- AVC (H.264 デコーダ)
- Virtual Mobile Engine (VME, Reconfigurable processor)
- Kirk / Spock (セキュリティ系、詳細非公開)
- 4KB mask ROM
- eDRAM (詳細は後述)

### eDRAM の物理実装 (訂正必須)

**過去の skill の記述 (誤)**: 「SC 側 + ME 側の独立 2 チップ」

**正しい認識 (2026-04-09 時点)**:
- Hot Chips 16 原典: **"single chip LSI"** (単一 LSI 内統合)
- Wikipedia "PSP hardware": **"earliest known use of a 3D IC chip in a commercial product"** ※ (世界初の民生 3D IC)
- Wikipedia: **東芝製、3D system-in-package、2 つの IC ダイを垂直積層** ※
- **筆者解釈**※※※: Hot Chips 16 の "single chip LSI" は Wikipedia の言う「2 ダイ積層」を指していた可能性が高い。つまり **「独立 2 チップ」ではなく「単一パッケージ内の 3D IC 2 ダイ積層」**

**要検証**: Wikipedia の出典元論文 (おそらく東芝の ISSCC または IEDM 論文) 未追跡 ※

---

## 公式 vs 実測 スペック比較

| 項目 | Sony 公式 | 実測/調査で判明 | 信頼度 |
|---|---|---|---|
| **CPU (Allegrex)** | 1〜333MHz | 同じ。OC 407MHz 安定報告あり、444MHz 上限付近 | (無印) / OC は※ |
| **CPU コア数** | 1 core | **2 cores** (Allegrex + ME の 2 コア構成) | (無印) |
| **ME (Media Engine)** | "メディア処理用" (詳細非公開) | 333MHz Allegrex 相当、FPU (COP1) あり、**VFPU なし**、16KB icache | (無印) |
| **ME 用途** | ATRAC/MP4 デコード (内部 FW) | 汎用並列 CPU。FM 合成、ベンチ、任意コード実行可 | (無印、田中実測) |
| **eDRAM (Phat PSP-1000)** | 2MB (VRAM) | **Hot Chips 16: "Total capacity of 4Mbytes" (SC Game 2MB + ME Media 2MB)** | (無印、Hot Chips 16 p.3) |
| **eDRAM (Slim+ PSP-2000 以降)** | 同上 | **SC 4MB + ME 4MB = 合計 8MB** (m-c/d 主張) | ※※ (m-c/d 個人研究のみ、Wikipedia "4 MiB in all models" と矛盾) |
| **eDRAM 物理実装** | - | 世界初の民生 3D IC、東芝製、2 ダイ垂直積層 | ※ (Wikipedia のみ、原典論文未追跡) |
| **eDRAM 用途** | フレームバッファ/テクスチャ | SC 側 = GE 描画用、ME 側 = ME 専用ワークメモリ | (無印) |
| **メイン RAM** | PSP-1000: 32MB / PSP-2000 以降: 64MB | 同じ | (無印) |
| **Scratchpad** | 16KB (ほぼ未文書化) | 16KB、SC 専用、0 レイテンシ。ME からは cross-path 経由でアクセス可能だが遅い | (無印) |
| **バスクロック** | 166MHz | 同じ | (無印) |
| **GPU (GE)** | グラフィックエンジン | 独自、SC 側 eDRAM をフレームバッファ/テクスチャに使用 | (無印) |
| **VME** | 仕様非公開 | **Virtual Mobile Engine** (書面一次情報源 4 つで確認)、166MHz、Walkman 系由来、Sony "Reconfigurable processor" | (無印) |
| **VME = CGRA** | - | m-c/d の学術分類、Sony 公式ではない | ※※ |
| **AVC** | H.264 デコーダ | 専用 HW ブロック | (無印) |
| **ME FW テーブル** | 非公開 | `table_id=2` または `table_id=3` が機種別で異なる | **※※ (要確認)** m-c/d の資料から取得した値だが、田中 psp-me-bench README では `me_init_ret=3 (PSP-1000)` / `me_init_ret=2, T2 テーブル (PSP-3000)` と報告。table_id と me_init_ret が同一概念かは未確認。**m-c/d への追加確認推奨** |
| **ME 性能 (vs SC)** | 非公開 | 並列動作時 ×2.32 (222MHz)、×3.48 (333MHz)、PI ×3.01、PRIME ×4.33、MANDEL ×3.09 | (無印、田中 psp-me-bench 実測) |

---

## eDRAM メモリマップ

### Phat (PSP-1000)

| アドレス (ME から見て) | サイズ | 用途 |
|---|---|---|
| `0x00000000〜0x00200000` | 2MB | ME 側 eDRAM (ローカル、末尾はスタック) |
| `0x04000000〜` | 2MB | SC 側 eDRAM (GE/VRAM) |
| **合計** | **4MB** | Hot Chips 16 原典一致 |

### Slim+ (PSP-2000/3000/Go/Street)

| アドレス (ME から見て) | サイズ | 用途 |
|---|---|---|
| `0x00000000〜0x00400000` | 4MB ※※ | ME 側 eDRAM (ローカル、末尾はスタック) |
| `0x04000000〜` | 4MB ※※ | SC 側 eDRAM (GE/VRAM、`sceGeEdramSetSize` で拡張解放) |
| **合計** | **8MB** ※※ | **m-c/d 個人研究のみが根拠、Wikipedia は「4 MiB in all models」と矛盾** |

**重要**: SC 側と ME 側は **物理的に 3D IC 内部の別ダイ、別バス**※※※。合計は単純和※※。

### ME 側 eDRAM の詳細アドレスマップ (m-c/d psp-me-reload より)

| アドレス | ME から見た用途 | サイズ |
|---|---|---|
| `0x00000000` | ME eDRAM (ローカルメモリ) | 2MB (Phat) / 4MB ※※ (Slim+) |
| `0x04000000` | Ping-Pong バッファ (DMA A bus) | 32KB + 拡張 |
| `0x04100000` | VLD Unit (可変長デコーダ) | - |
| `0x040FF000` | Primary DMAC | - |

### ME スタック位置

MECC の `me-core.h` で自動判定:
- **Phat**: `$sp = 0x80200000` (= eDRAM 2MB の末尾を cached kernel マスクで指す)
- **Slim+**: `$sp = 0x80400000` (= eDRAM 4MB の末尾) ※※

判定は `0x88300018` のマジック値 `0x279c637c` をチェック。

---

## メモリ階層全体像 (Slim+)

| 層 | 所有 | サイズ | レイテンシ | アクセス方法 |
|---|---|---|---|---|
| **Scratchpad** | SC 専用 | 16KB | 最速 (0 レイテンシ) | 専用アドレス |
| **ME eDRAM** | ME 専用 | 4MB ※※ | 高速 (ローカル) | ME 側から `0x00000000〜` |
| **SC eDRAM (VRAM)** | GE/SC | 4MB ※※ | 高速 | SC から uncached `0x40000000 \| 0x04000000+offset` |
| **メイン RAM** | 共有 | 64MB | 普通 | 通常のポインタ |

**未検証事項**: SC 側から ME 側 eDRAM (`0x00000000`) への直接アクセス可否※。m-c/d の `edram-dump` サンプルは ME 側で `meCoreMemcpy` してからメインメモリ経由で SC に渡す方式を取っているため、**SC から直接読めない可能性が高い**※※※。

---

## SC 側 eDRAM (VRAM) の空き領域活用

GE が描画に使う領域以外は CPU から自由に読み書き可能:
- フレームバッファ (`sceGuDrawBuffer`)
- Z バッファ (`sceGuDepthBuffer`)
- 描画中のテクスチャ
- **上記以外は空き**

デフォルト 2MB 時の空き: ~1MB
拡張 4MB 時の空き: ~3MB ※※

アクセス方法 (田中の psp-bsky/psp-note/psp-note-home 等で実績):
```c
u32 vram_base = sceGeEdramGetAddr() | 0x40000000;  // uncached アクセス
// vram_base からデータ/テクスチャを直接書き込み
```

**この手法が世界初 (筆者の知る限り)※※※ PSP SNS クライアント 3 本 (psp-bsky/psp-note/psp-note-home) を 1 日で完成させた突破口**。sceGu を介さず CPU 直接書き込みでテキスト/UI レンダリング。

---

## ME (Media Engine) 詳細

### アーキテクチャ (Rank A)
- **Allegrex 相当の MIPS コア**
- **FPU (COP1) あり**: 浮動小数点演算可 (田中 psp-me-bench 実測で確定、論争終結)
- **VFPU なし**: SIMD ベクトル演算不可 (SC 側 Allegrex にはある)
- **icache 16KB**: 命令キャッシュ
- **dcache**: あり (writeback/invalidate 必要)
- **独立したメモリ空間**: ME eDRAM をローカルに持つ

### ME の限界 (VFPU なし起因)
- GE (GPU) の直接制御は困難 (sceGu 系は VFPU 前提) ※※※
- 3D 行列演算は遅い
- 得意: スカラ演算 (FM 合成、フィルタ、一般的な計算)
- 不得意: ベクトル/行列演算 (グラフィック系)

### ME の性能実測 (田中 psp-me-bench, 2026-04)
- vs SC 並列動作時:
  - 222MHz 動作で ×2.32 のスループット
  - 333MHz 動作で ×3.48 のスループット
- ベンチ別:
  - PI (ライプニッツ級数): ×3.01
  - PRIME (試行除算): ×4.33
  - MANDEL (マンデルブロ集合): ×3.09
- bit-perfect チェックサム: PI 3141595 / PRIME 9592 / MANDEL 173175
- PSP-1000 vs PSP-3000 性能差: 0.03-0.06% (誤差範囲、同一 Allegrex コア確定)
- 用途: 音声合成、エミュレーション、画像処理、一般計算

### scePowerSetClockFrequency API の嘘 (田中発見)
- `scePowerSetClockFrequency(370, 370, 166)` → ret=0 成功報告
- `scePowerGetCpuClockFrequency()` も 370MHz と報告
- **だが `sceKernelGetSystemTimeLow()` 実測で 333MHz 固定**
- API が嘘を返す (Sony は 222/266/333 のみ許可、それ以外は silent fallback)
- 田中 psp-me-bench 実装で発見、公開範囲で世界初の実証の可能性※※※

---

## Sony 純正 ME プログラムの用途 (判明分)

過去チャットおよび psdevwiki 調査により判明した、Sony 自身が ME で動かしていたプログラム一覧:

| 機能 | 情報源 | 信頼度 |
|---|---|---|
| MP3 デコーダ (sceMp3) | 複数ソース | ※ |
| AAC デコーダ | 複数ソース | ※ |
| ATRAC3 デコーダ (sceAtrac) | 複数ソース | ※ |
| H.264 ビデオデコーダ (sceMpeg) | Hot Chips 16 + 複数 | (無印) |
| **UMD Video 再生プログラム** | **psdevwiki (PSP 1.02 devkit firmware に ME プログラムのデバッグシンボルが残存)** | ※ |
| 32 ch ソフトウェアシンセサイザー (PCM/ADPCM/ATRAC3, リバーブ, ピッチ, ADSR) | 過去チャット経由 | ※※ |
| MIDI シーケンシング | 過去チャット経由 | ※※ |
| C++ クラスメタデータ存在 (ROM 内 `__cxxabiv121__vmi_class_type_infoE` 等) | psdevwiki Media Engine ROM ダンプ | ※ |

**田中の推測**※※※: ガキの頃 (PSP-3000 CFW 成熟期 2009-2012 年頃) にニコニコ動画で見た、クロック別に PSP XMB 内蔵ビジュアライザの動作速度が変わる現象は、**ビジュアライザが ME 駆動だった可能性を示唆**。ただし「ME のクロックでビジュアライザがもっさりするか早くなったかの違いしか当時の俺にはわからなかった」(田中証言) のみが根拠で、ME 駆動の確証はない。要追加調査。

---

## dcache/icache 使い分け (m-c/d 公式ガイドライン)

| コンテキスト | 推奨 API |
|---|---|
| SC 側 | `sceKernelDcache*` (PSP 公式 SDK) |
| ME 側 | `meCoreDcache*` (syscall 経由) |
| uncached 領域 (`0x40000000 \| ptr`) | **writeback/invalidate 不要** |
| Icache | `meLibIcacheInvalidateAll()` 維持 OK (meCore 版なし、m-c/d 公認) |

**実装例**:
```c
// 旧 (非推奨)
meLibDcacheWritebackRange(edram_buf, size);

// 新 (推奨)
meCoreDcacheWritebackRange(edram_buf, size);  // ME 側で実行時
```

旧 meLib 版は内部で cache 命令 2 倍撃ち + C for ループの非効率実装。meCore は ME カーネル syscall で最適化済み。

### 設計原則
**ME が書く → writeback、SC が読む → invalidate**。全同期が必要な遷移点では両側 `writebackInvalidateAll`。ブロック単位の定常処理は Range で最小コスト。

### 実測効果 (psp-pmdvis 2026-04-08)
`meLibDcache*` → `meCoreDcache*` の 16 箇所置換で:
- TH1 が 2 ループ → 3 ループまで安定動作
- ソロモード切替時の全キャッシュフラッシュが劇的改善 (512 回の cache 命令 → syscall 2 発)
- ymfm 聖域に触らず周辺最適化のみで性能向上達成
- Icache 6 箇所は `meLib` 維持 (m-c/d 公認、meCore 版存在せず)

### pmd_psp のデコードループ毎ブロック 3 連続 WritebackRange
1. `edram_buf` flush (eDRAM→RAM 転送前)
2. `dst` (PCM) flush (PCM ブロックを RAM に確定)
3. `vdst` (vis) flush (vis データを RAM に確定)

→ 統合余地あり (実験 Phase 3 候補、優先度最下位)

---

## VME (Virtual Mobile Engine)

### 書面表記の確定 (訂正済み)

**正式名**: **Virtual Mobile Engine (VME)** (「2」なし)

書面一次情報源 4 つ全てで確認:
- Hot Chips 16 (2004): "Virtual Mobile Engine™"
- m-c/d psp-media-engine-reload README: "VME (Virtual Mobile Engine / DSP)"
- psdevwiki: "programmable DSP dubbed 'Virtual Mobile Engine'"
- Wikipedia: "Virtual Mobile Engine"

「2」は m-c/d Discord 発言 1 回のみ ("trusting Sony" 留保付き) ※※

### Sony 公式分類 (訂正済み)

- Hot Chips 16: **"Reconfigurable processor"** / **"Dynamic Reconfigurable Architecture"**
- **"World's First Consumer Product using Reconfigurable architecture!"** (Hot Chips 16 p.11 原文)
- 「CGRA」は m-c/d の学術分類 ※※ であり Sony 公式ではない

### VME の概要
- **Tachyon (CXD2962GG) チップの一部**
- Sony Walkman シリーズで商用化された (Hot Chips 16 p.11 の歴史表)
- 166MHz (Hot Chips 16)
- ビット幅 24bit は出典不明 ※※※ (旧 skill 記載だが原典未確認)
- Sony の機密、詳細仕様未公開
- m-c/d が解析中 (psp-media-engine-cracking-the-unknown リポジトリ)

### VME の歴史 (Hot Chips 16 p.11 原文)

| 年 | 製品 | 用途 | 電力 |
|---|---|---|---|
| 2003 年 2 月 | Network Walkman (Flash mem.) | Audio ATRAC3 Dec | 4mW @1.2V, 0.18um |
| (時期不明) | CD Walkman | - | - |
| (時期不明) | MD Walkman | audio codec, sound effect | - |
| (時期不明) | Network Walkman (HDD) | audio codec, sound effect, game sound | - |
| 2004 年 | **PSP** | audio codec, sound effect, video codec | - |

### VME の注意点 (m-c/d psp-me-reload README L114-128)
- デフォルトで eDRAM にバックグラウンド処理をかけている
- ME バスクロック (bit 0 of `0xBC100050`) に紐づく
- homebrew の邪魔になるため停止推奨

### VME 停止手順
```c
hw(0xBCC00000) = -1;
hw(0xBCC00030) = 1;
hw(0xBCC00040) = 1;
```
MECC の `meLibSetMinimalVmeConfig()` が上記を実行する。

### VME の活用可能性 (未開拓)
- homebrew での実用例なし
- eDRAM と直結しているため、成功すれば超高速処理が可能 ※※※
- m-c/d が唯一調査している領域
- 「世界初の VME homebrew 利用」になれる可能性※※※

---

## 機種別スペック一覧

| 機種 | CPU | メインRAM | SC eDRAM | ME eDRAM | 合計 eDRAM | CFW 例 |
|---|---|---|---|---|---|---|
| PSP-1000 (Phat) | 333MHz | 32MB | 2MB | 2MB | 4MB | 6.60 PRO-B10 等 |
| PSP-2000 (Slim) | 333MHz | 64MB | 4MB ※※ | 4MB ※※ | 8MB ※※ | 6.60 PRO 等 |
| PSP-3000 | 333MHz | 64MB | 4MB ※※ | 4MB ※※ | 8MB ※※ | 6.60 PRO-C 等 |
| PSP Go (N-1000) | 333MHz | 64MB | 4MB ※※ | 4MB ※※ | 8MB ※※ | ARK-4 CXPL 等 |
| PSP Street (E-1000) | 333MHz | 64MB | 4MB ※※ | 4MB ※※ | 8MB ※※ | - |

**注**: Slim+ 系の 4MB + 4MB = 8MB は m-c/d 個人研究のみが根拠。Wikipedia は「4 MiB in all models」と矛盾する記述。要追加確認※※。

### 機種固有の注意点
- **PSP-1000**: eDRAM 拡張非対応 (HW 制約)、古い CFW でも動作
- **PSP-3000**: Slim+ 世代、鉄砲玉用途に向く (実験機として壊しても影響小)
- **PSP Go**: 内蔵フラッシュ (`ef0:`)、ARK-4 CXPL ではコード追加量が閾値超えると起動コケ
- **Slim+ 共通**: eDRAM 8MB フル活用可能 ※※ (開拓余地あり)

### 田中の PSP 実機 3 台体制

| 機種 | CFW | RAM | eDRAM | 用途 |
|---|---|---|---|---|
| PSP-1000 | 6.60 PRO-B10 | 32MB | 2MB | 旧世代互換性検証、eDRAM 拡張非対応 |
| PSP-3000 | 6.60 PRO-C | 64MB | 4MB ※※ | 鉄砲玉 (OC 実験、コード量増の先行検証)、制約なし |
| PSP Go | ARK-4 CXPL | 64MB | 4MB ※※ | 内蔵ストレージ本命機。コード追加量閾値超で起動コケ (eDRAM テスト 54 行追加で強制シャットダウン前例あり) |

**開発ワークフロー**:
- コード増加を伴う実験は PSP-3000 で先行検証 → 安定後に PSP Go 移植 → PSP-1000 で古い世代互換性確認
- dcache リファクタのような置換のみ (行数増なし) は PSP Go で直接 OK
- 鉄砲玉 PSP-3000 は PRO-C で制約が緩いため OC・新機能・大きいコード検証用

### ME FW テーブル (要 m-c/d 確認事項)

- 田中 psp-me-bench README の動作確認済み記述:
  - `PSP-1000 (FAT) — me_init_ret=3`
  - `PSP-3000 (Slim+) — me_init_ret=2, T2テーブル`
- 過去の skill の記述: `table_id=2 (Phat)`, `table_id=3 (Slim+)` ※※※ (出典不明、誤りの可能性)
- **現時点では me_init_ret と table_id が同一概念か、逆転しているのか、別物なのか不明**
- **m-c/d への追加確認推奨**

---

## eDRAM 拡張 API

SC 側 eDRAM はデフォルト 2MB、`sceGeEdramSetSize(0x400000)` で 4MB に拡張可能 (Slim+ のみ) ※※。
- API は非公開、NID `0xD8633888`
- カーネルモード必須
- PSP-1000 では HW 的に拡張不可 (実機で描画異常を確認、田中実証済み)
- サンプル: `m-c/d/psp-undocumented-sorcery/edram-0x400000`
- **VSH/POPS との干渉問題あり** (Acid_Snake 指摘、互換性要検証)

---

## OC (オーバークロック)

- 公式 API の上限: `scePowerSetClockFrequency(333, 333, 166)`
- 非公式 OC: PLL レジスタ直接操作で 407MHz 〜 444MHz 可能 ※
  - PLL 基準周波数 = `37MHz × (NUM / DEN)`
  - 407MHz = `37 × 220 / 20`
  - カーネルモード必須
- m-c/d 作 `psp-444mhz-plugin` (experimental) あり
- **注意**: ARK-4 CXPL との互換性に個体差あり。PSP-3000 6.60 PRO-C でも起動しない個体あり (田中実測、深追い断念)
- 用途: ME/SC 両方が同じ PLL から派生するため、PLL OC で ME も速くなる ※※※

---

## MECC (psp-media-engine-custom-core) 実用ガイド

### 正式名称
- **正式名**: psp-media-engine-custom-core
- **略称**: me-custom-core / MECC
- **旧称使用禁止**: Tiny-ME / TinyME / t***-me / T***yMe 等 (憲法第 8 条適用)

### Tiny-ME 旧称の歴史 (重要)
- 2024-25 年頃、m-c/d 本人が pspdev org 下で `tiny-me` という名前でリポジトリ運営していた (実在)
- その後 `mcidclan/psp-media-engine-custom-core` にリネーム
- 田中ローカル環境には旧 submodule フォルダ `tiny-me/` が残存
- **旧称は過去には実在したが、現在は使用禁止**。ローカルフォルダ名の物理的残存も修正対象

### 正規入手ルート
- **pspdev 公式パッケージ** (推奨): https://github.com/pspdev/psp-packages/blob/master/libmecore/PSPBUILD
- **upstream**: https://github.com/mcidclan/psp-media-engine-custom-core
- 現行版: `libmecore v2.0.1`

### 田中パッチ 4 点 (v2.0.1 に適用)

1. **`meLibDefaultInit()` の `return -3` 削除**
   - `meLibLoadPrx()` が失敗しても続行
   - seplugin モード (kcall.prx 既ロード) 時の fallback
2. **`sceKernelExitGame()` 削除**
   - me-lib.c のエラーパスから除去
   - アプリ側でハンドリング可能に
3. **`me-lib.h` の C++ 対応**
   - `#ifdef __cplusplus` ガード追加
   - `uintptr_t` キャスト追加
   - `meLibMakeSharedUncachedMem` マクロバリアント追加
4. **`kcall.S` (asm) ファイル追加**
   - 田中版独自ファイル
   - v2.0.1 upstream には存在しない
   - 用途は 100% 把握できてないが動作保証のため維持 ※※※

### auto-load 方式 (seplugin 不要)

- `PLUGINS.TXT` に `kcall.prx` を登録する **seplugin 方式は非推奨** (Acid_Snake = ARK-4 開発者の正式 NG 判定: 「seplugin は使ってない時も RAM 消費する」)
- **正解**: EBOOT.PBP 単独で ME ON (`meLibLoadPrx()` が実行時に kcall.prx を自動生成)
- PSP-1000 + PSP Go 両機で実機確認済み (psp-pmdvis 2026-04-08)

### kcall.prx の実行時生成メカニズム

- `libmecore.a` 内の `embedded_kcall[]` 配列に kcall.prx バイナリが埋め込まれている
- `meLibLoadPrx()` がアプリ起動時に `ms0:/PSP/GAME/<APP>/kcall.prx` として書き出す
- その後 `sceKernelLoadModule()` + `sceKernelStartModule()` で ME カーネルに読み込ませる
- 生成される kcall.prx: **2,198 bytes** (v2.0.1 正規版、MD5 `c6235eb363fbb0c53cd71f3b0352427e`)
- 古い v1.0.0 版は 1,740 bytes → 混在すると ME OFF になる

### よくある失敗パターン

1. **古い EBOOT.PBP をデプロイして v2.0.1 が効かない**
   - 症状: リビルドしたのに ME OFF、`table_id=-3`
   - 原因: EBOOT.PBP 再デプロイ漏れで古い `embedded_kcall[]` が走る
   - 対処: **EBOOT.PBP を必ず新しくデプロイ**。メモステの `kcall.prx` を全部消してから起動して再生成を確認
   - 教訓: 技術的デバッグの前に「デプロイミス」を疑え

2. **システムライブラリ汚染**
   - 症状: 別アプリのビルドが知らぬ間に MECC 版にリンクされる
   - 原因: 過去の MECC テストで `/usr/local/pspdev/psp/lib/libme-core.a` が上書きされる
   - 対処: sudo で復旧 (田中環境では sudo 通らず復旧不能、submodule でローカル管理推奨)

3. **submodule 汚染**
   - 症状: 本体リポジトリをリビルドすると ME OFF
   - 原因: submodule (旧称 tiny-me 等) 内の `me-lib.c` / `libme-core.a` が MECC 化の変更を残してる
   - 対処: `git checkout -- <files>` → `rm -rf build && cmake .. && make`
   - **教訓**: 動いている EBOOT があるなら触るな。バックアップ EBOOT を常に保持

4. **田中環境固有の MECC 初期化失敗 (未解決)**
   - 症状: m-c/d ビルド済みバイナリでも PSP Go/3000/1000 全滅
   - エラーコード: `0x80020139 (SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE)`
   - 失敗箇所: `meLibLoadPrx()` 後の `sceKernelLoadModule()` / `sceKernelStartModule()` 段階 (kcall.prx は書き出されている)
   - 容疑: メモステ側の何か共通要素 (プラグイン残骸、設定、過去のテスト痕跡)
   - z2442 の環境 (pd/sm64-port) では正常 → 田中環境固有
   - 対処: 未解決。seplugins 全 OFF テスト、メモステ完全クリーン状態でのテスト要

---

## MECC/ME 起動サンプルの系譜

### 田中の直接参考源 (2026-04-09 Claude Code 田中ローカル調査結果)

**田中が 2026 年 4 月に「起動デモ」として参考にしたのは、MECC 本体の README と samples**:

1. **MECC README.md (L58-70) Code example**
   - Basic Integration パターン
   - `meLibOnProcess()` 実装
   - `meLibDefaultInit()` 呼び出し
   - **田中の psp-pmdvis main.c の `meLibOnProcess()` / `meLibDefaultInit()` はこのコード例そのまま**

2. **MECC samples/poc/**
   - `meCoreDcacheWritebackInvalidateAll` デモ
   - `meCoreBusClockFilterEnabled` デモ
   - 田中の dcache 操作の元ネタ

3. **MECC samples/hw-mutex/**
   - SC↔ME 間の共有メモリ同期パターン
   - **田中の `_me_shared[]` + uncached ポインタ方式の元ネタ**

### m-c/d の公開サンプル集 (psdevwiki Media Engine ページより)

| サンプル名 | 用途 |
|---|---|
| tiny-me-lib (旧名) | サンプル集、後に MECC に発展 |
| me-minimal-handler | 最小リセットハンドラ |
| me-spinlock | HW spinlock を使った SC-ME 間 mutex |
| me-dmacplus-transfer | DMACPlus を使った SC-ME 間データ転送 |
| me-simple-count | 共有 uncached メモリを使ったシンプルカウンタ (crazyc 2005 の系譜) |
| me-sharing-drawing | SC + ME 両方から同じ framebuffer に描画 |
| me-soft-interrupt | SC-ME 間ソフト割り込み |
| me-vme-hardware-csc | VME で HW カラースペース変換 |
| me-feed-graphics-engine | SC + ME から同時に GE list 更新 |

### 歴史的系譜 (2005 → 2026)

```
crazyc / hlide (ps2dev 先駆者、2005 年代)
    ↓
psdevwiki リバースエンジニアリング貢献者
    ├── CelestBlue
    ├── artart78
    ├── davee
    └── xerpi
    ↓
psp-media-engine-reload (m-c/d, 2024-25)
    ↓
psp-media-engine-custom-core / MECC (m-c/d, 2025 リネーム)
    ├── README.md Code example → 田中の meLibOnProcess() パターン
    ├── samples/poc → dcache 操作
    └── samples/hw-mutex → 共有メモリ同期
    ↓
田中 PMD Visualizer + psp-me-bench (2026-04)
    ├── 世界初 (筆者の知る限り) の PSP FM 音源リアルタイム合成 ※※※
    ├── 世界初 (筆者の知る限り) の PSP デュアル CPU 定量ベンチマーク ※※※
    └── MECC 田中パッチ 4 点 (m-c/d upstream 反映予定)
```

### MECC 実用化の正確な系譜

**田中は MECC 実用化「第 1 号」ではない**。先駆者が複数存在:

- **2005 年**: crazyc が世界初の公開 ME コード (カウンターサンプル、フォーラム投稿)
- **2006 年**: Snes9xTYL が ME でサウンドエミュレーションオフロード
- **(時期不明)**: LightMP3 が MP3 デコードで ME 利用
- **2012 年**: Vita フォーラムで ME 活用議論、実装至らず
- **2025 年**: m-c/d が MECC を公開
- **2026 年**: 田中が MECC v2.0.1 を PMD プレイヤーに応用、FM 音源リアルタイム合成という **新しい用途** を追加。MECC 実用ユーザーとして m-c/d に具体的フィードバックを返した人 (embedded_kcall[] 問題報告等)

「MECC 実用第 1 号」の表現は誤り。今後の発信では正確に使う。

---

## 実戦知見 / 開発 Tips

### CPU 直接 VRAM 書込 (psp-bsky 等で確立)

```c
u32 vram_base = sceGeEdramGetAddr() | 0x40000000;  // uncached
// sceGu を使わず CPU で直接ピクセル書き込み
// テキスト/UI レンダリングで sceGu より速い場合が多い
```

- sceGu のオーバーヘッドを回避
- 2D UI (SNS クライアント、テキストエディタ、ビジュアライザ等) で効果大
- uncached 領域なので dcache flush 不要

### リングバッファ設計 (pmd_psp v1.1 で確立)

**動的確保 + フォールバック**:
```c
#define RING_BLOCKS_64MB 256
#define RING_BLOCKS_32MB 128

// 常に 256 を試行 → 失敗時 128 にフォールバック
ring_buf = malloc(RING_BLOCKS_64MB * block_size);
if (!ring_buf) {
    ring_blocks = RING_BLOCKS_32MB;
    ring_buf = malloc(ring_blocks * block_size);
}
```

**奇跡の副作用** (pmd_psp v1.1 で判明):
- 静的配列 `ring_buf[128][...]` 廃止 → BSS が約 512KB 縮小
- その BSS 削減分が動的ヒープに転換
- **32MB 機 (PSP-1000) でも 256 ブロック確保可能になった**
- 64MB 機強化を狙った変更が、副作用で 32MB 機まで道連れ強化

**教訓**: 静的配列を動的 malloc に置換するだけで BSS が空いて、結果的に全機種強化される可能性。メモリで時間を買え。

### prefill_wait の罠 (機械的書き換えバグ)

pmd_psp の例:
```c
int lc_prefill_wait = ring_blocks - 1;  // ring_blocks=256 → 255
// コメントには「ME が 20 ブロック (~0.5秒) デコードするまで待つ」と書いてある
```

- 実装とコメントが完全乖離
- 過去に値だけが `ring_blocks - 1` に変更された際にコメントが更新されなかった
- 起動遅延 = 255 / 42 spd ≈ **6 秒** (コメントの意図 0.5 秒の 12 倍)

**検証済みの物理法則**:
- prefill を縮める (32 に固定) → 起動 0.85 秒達成
- **ただし** 1 周目後半からドレインが顕在化 → r=0 張り付き
- 「早く + 途切れず」の両立には **ME 速度向上が必須** (prefill 単独では不可能)
- ymfm 聖域下では物理限界。**v1.1 の prefill=255, ring=256 が物理的最適解**

### ymfm 聖域

- **ymfm は絶対にいじらない** (内部コード、初期化パラメータ、内部前提全て)
- チャネルスキップ / wait 削除 / int32_t 化すべて失敗済み (音が壊れる)
- ダイショット精度で触ると全滅
- 聖域違反は憲法第 8 条適用
- 166,400 generate/sec は物理固定値
- ME 最適化はこの制約下で周辺最適化のみ (dcache、uncached 配置、処理量削減)

**処理量削減アプローチ** (聖域を守る):
- シングルトラックモード (トラックソロ): ymfm に渡す指示を絞る、聖域違反にならない
- 動的負荷制御: ドレイン中に重いチャンネルを一時的に間引く (ただし「正確に」モットーに反する可能性)

### 動いているものは触るな

鉄則:
1. **動いている EBOOT があるなら、ソースをいじる前に tag 打ち + GitHub push**
2. バックアップ EBOOT を常に保持 (田中の `backups/EBOOT_YYYYMMDD_HHMMSS.PBP` 方式)
3. `git checkout --` で戻したソースがオリジナルと一致する保証はない
4. ロールバック経路を常に明確化 (安全地帯 tag)

### Code 暴走対策 (憲兵スレツヴァイ)

1. diff を見せてから田中の確認後にデプロイ
2. 別の最適化を勝手に追加しない
3. ログを取らずにデプロイしない (証拠破壊禁止)
4. submodule を勝手に書き換えない
5. streaming ブランチ禁止 (main.c 改造 diff 見せず → ビルド → デプロイ → ME OFF の前科)

### 6 つの地雷 (psp-pmdvis 開発で踏んだもの、Note 記事【狂気・再】第 1 弾から)

1. **pspaudiolib コールバック CPU 独占** — デフォルト最高優先度、メインスレッド餓死。解法: pspaudiolib 廃止、自前低優先度オーディオスレッド
2. **1 オクターブ上がる** — PSP sceAudio は 44100Hz 固定、22050Hz 生成 PCM を流すと DAC が倍速読み。解法: 各サンプル 2 回複製 `out[i*2] = out[i*2+1] = src[i]`
3. **音のプツプツ (根本原因は ME じゃなかった)** — 初期仮説: dcache 全フラッシュスパイク。真相: **ダブルバッファ方式そのものの silence gap** (sceAudioOutputBlocking 呼び出し間の処理中に DAC 供給途切れ)。解法: 3 スレッド + リングバッファ (audio_output 0x10 / audio_producer 0x20 / main 0x20)
4. **曲切替で音崩壊** — ME の L1 dcache に pmdmini 内部ステートが dirty で残留、新曲ロード後に ME が書き戻して上書き。解法: 厳密な順序制御 (dcache 全書き戻し → 新曲ロード → キャッシュ無効化)
5. **OPN_FIDELITY_MAX が重すぎる** — デフォルト chip_rate = clock/8 = 998,400Hz、1 出力サンプルあたり約 90 回の generate() 呼び出し、333MHz には重すぎる。解法: **OPN_FIDELITY_MIN** (chip_rate = clock/48 = 166,400Hz、1 出力あたり約 15 回 = 6 倍速、設定 1 行変更、最大の最適化)
6. **メイン CPU 単体では間に合わない** — 上記 5 つの最適化を全部やっても足りない。解法: ME オフロード

---

### Wi-Fi 接続: sceUtilityNetconfInitStart は実機で不可視

`sceUtilityNetconfInitStart` の AP 選択 dialog は実機で画面真っ黒・操作不能。`sceNetApctlConnect(profile)` を直接呼び、XMB 保存済み profile を `sceUtilityCheckNetParam` で列挙して使う。

```c
for (int profile = 1; profile <= 10; profile++) {
    if (sceUtilityCheckNetParam(profile) != 0) continue;
    if (sceNetApctlConnect(profile) != 0) continue;
    int state = 0;
    for (int retry = 0; retry < 200; retry++) {
        if (sceNetApctlGetState(&state) != 0) break;
        if (state == 4) goto connected;  /* GOT_IP */
        sceKernelDelayThread(50 * 1000);
    }
    sceNetApctlDisconnect();
}
```

PPSSPP は `EnableWlan=True` で dialog を即 FINISHED に遷移させるため実機との差分罠。pkgi-psp 等も同方式。前提: XMB で profile 1個以上保存済み。

---

### sceJpeg / sceMpeg: AVCODEC モジュール事前ロード必須 (実機のみ)

実機で `sceJpegInitMJpeg` / `sceMpegInit` 系を呼ぶ前に **必ず** `sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC)` を呼ぶ。これを飛ばすと Init が負値返して silent fail。**PPSSPP は自動ロード**するので実機でしか踏めない罠。

```c
int lm = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
if (lm < 0 && lm != (int)0x80111102) /* already loaded */ return -1;
sceUtilityLoadModule(PSP_MODULE_AV_MPEGBASE);  /* sceMpeg 系も使うなら */
sceJpegInitMJpeg();
/* ... */
sceJpegFinishMJpeg();
sceUtilityUnloadModule(PSP_MODULE_AV_AVCODEC);
```

依存関係: `PSP_MODULE_AV_AVCODEC` (0x0300) → sceJpeg / sceAudiocodec / sceVideocodec、`PSP_MODULE_AV_MPEGBASE` (0x0304) → sceMpeg。ロード順: AVCODEC → MPEGBASE。

---

### sceGuDrawArray 非テクスチャ矩形は drop → CPU-blit で回避

`sceGuDrawArray` で塗りつぶし矩形 (テクスチャ無し) を描こうとすると PPSSPP v1.20.3 で完全 drop。GU_SPRITES / TRIANGLE_STRIP / PVertex+sceGuColor / 白1x1 texture 等6方式すべて fail。`sceGuClear` と `intraFontPrint` は機能する。

**確定解**: CPU から uncached VRAM (0x40000000 マスク) に直書き

```c
static void drawRect(float x, float y, float w, float h, unsigned int color) {
    unsigned int *fb = getDrawBuf();  /* 0x40000000 | VRAM base + drawBufIdx*FB_SIZE */
    int x0=(int)x, y0=(int)y, x1=x0+(int)w, y1=y0+(int)h;
    if (x0<0) x0=0; if (y0<0) y0=0;
    if (x1>SCR_WIDTH) x1=SCR_WIDTH; if (y1>SCR_HEIGHT) y1=SCR_HEIGHT;
    for (int py=y0; py<y1; py++) {
        unsigned int *row = fb + py * BUF_WIDTH;
        for (int px=x0; px<x1; px++) row[px] = color;
    }
}
```

**フレーム構成** (順序重要):
1. CPU phase: fillScreen → drawRect 複数 → 画像 blit
2. GU phase: sceGuStart → intraFontPrint (テキストのみ) → sceGuFinish/Sync
3. swap: sceDisplayWaitVblankStart → sceGuSwapBuffers → drawBufIdx flip

注意: `sceGuClear(GU_COLOR_BUFFER_BIT)` は CPU fill と競合するので呼ぶな。480×272×4=520KB 毎フレ書きでも 60fps で 31MB/s なので帯域余裕あり。

---

### sceJpeg 実機サイズ制限 ~480×272、大画像は libjpeg 併用

`sceJpegDecodeMJpeg` (HW decoder) は **実機で実用上限が PSP 画面サイズ程度**。1000px クラスを流すと `0x80650020` (UNSUPPORT_IMAGE_SIZE) を返す。罠: `sceJpegCreateMJpeg(1024, 1024)` は rc=0 で成功するが Decode 時にサイズ拒否される。**PPSSPP は通すので実機でしか踏めない**。

**使い分け**:
- **sceJpeg (HW)**: 128×128 以下決め打ち、速い、decode メモリ 64KB
- **libjpeg-turbo (CPU)**: pspdev `jpeg` パッケージ、任意サイズ、遅い (1000×750 で 2-5 秒)、メモリ ~2.25MB

```c
#include <jpeglib.h>
#include <setjmp.h>
struct err_ctx { struct jpeg_error_mgr pub; jmp_buf jmp; };
static void jpeg_err_exit(j_common_ptr ci) { longjmp(((struct err_ctx*)ci->err)->jmp, 1); }

struct jpeg_decompress_struct cinfo;
struct err_ctx ec;
cinfo.err = jpeg_std_error(&ec.pub);
ec.pub.error_exit = jpeg_err_exit;
if (setjmp(ec.jmp)) { jpeg_destroy_decompress(&cinfo); return -1; }
jpeg_create_decompress(&cinfo);
jpeg_mem_src(&cinfo, jpeg_buf, jpeg_len);
jpeg_read_header(&cinfo, TRUE);
cinfo.out_color_space = JCS_RGB;
jpeg_start_decompress(&cinfo);
```
CMakeLists: `target_link_libraries(${PROJECT_NAME} jpeg ...)`。出力 RGB24 → RGBA32 変換と downsample が別途必要。

---

### FAT32 fflush では disk 反映されない (実機のみ)

実機の FAT32 (memstick / 内蔵フラッシュ) で `fopen("a")` + `fputs` + `fflush` 単体では directory entry が更新されない。クラッシュ/Home 強制終了時は tail が消える。中途半端な状態で exit すると FAT32 directory が破損し、Windows/WSL から「ファイルまたはディレクトリが壊れている」error 1392 が出る。`chkdsk` でも復旧困難。

**応急処置**: log 関数で毎回 fclose+fopen("a") 強制 sync
```c
fputs(buf, fp); fflush(fp); fclose(fp); fp = fopen(path, "a");
```

⚠️ **ホットパス禁止**: fclose+fopen は実機で数 ms。60fps frame loop で呼ぶと fps 半減 + メモステ寿命消費。**critical (起動/auth/error) は強制 sync、normal (frame loop) は fflush のみ**で使い分ける。PPSSPP は仮想 FS で fflush 即反映するので実機でしか踏めない。

---

### OSK: kana-kanji 変換 IME は非搭載、kanji 入力は絶対不可能

PSP 公開 OSK API (`sceUtilityOskInitStart` 等4関数) は hiragana/katakana/latin/korean/russian 直接入力のみ。**kana→kanji 変換 IME は搭載なし**。Sony 公式アプリのブラウザ IME は kernel 側独自実装で homebrew からは呼べない。

`PSP_UTILITY_OSK_INPUTTYPE_JAPANESE_KANJI = 0x00008000` フラグは定義あるが PPSSPP で参照されず、実機で立てても動作変化なし。ARK-4 arkMenu の osk.cpp も kanji 変換実装なし。

**回避策**:
- (推奨) hiragana/katakana/latin のみで運用 (工数ゼロ)
- HtmlViewer 別 EBOOT 経由でブラウザ内蔵 IME を借りる (後述、大工事)
- 自前 IME 実装 (MeCab-ipadic 縮小版等、数週間規模)

---

### HtmlViewer: 8-10MB 連続 Vpl 必須、複合 homebrew と同居不可

`sceUtilityHtmlViewer*` 起動時に 8-10MB の **連続 VPL メモリプール** を要求。複合 homebrew (curl+TLS / intraFont / libjpeg 等フル稼働) では連続ブロックが残らず `0x80020190` (NO_MEMORY) で失敗。

実測 (PSP Go 64MB、psp-bsky フル稼働時): free total=100KB / max_contig=52KB / required=8MB → 物理的不可能。pspsdk 公式 sample も `BROWSER_MEMORY = 10MB` でスタンドアロン EBOOT 想定。

**解決経路**: 別 EBOOT 分離 + `sceKernelLoadExec` 連携 + memstick file 中継
```c
sceIoRemove("ms0:/draft.txt");
struct SceKernelLoadExecParam p = {0};
p.size = sizeof(p);
sceKernelLoadExec("ms0:/PSP/GAME/COMPOSE/EBOOT.PBP", &p);
```

---

### PSP Go 64MB ヒープ実測値 (複合 homebrew フル稼働時)

PSP Go (64MB、ARK-4 CXPL)、curl+mbedTLS / intraFont CACHE_LARGE / libjpeg / 画像キャッシュ / GU display list フル稼働時:

| 段階 | free total |
|---|---|
| main 冒頭 | ~764 KB (`PSP_HEAP_SIZE_KB(-1024)` 設定時) |
| curl init 後 | ~132 KB |
| 全サブシステム稼働 | total=100KB / max_contig=52KB |

**サブシステム別消費 (推定)**:
| subsystem | 消費 |
|---|---|
| GU display list `[262144]` | 1 MB |
| intraFont CACHE_LARGE (jpn0.pgf) | ~3 MB |
| intraFont CACHE_ALL (ltn8.pgf) | ~300 KB |
| curl + mbedTLS context | ~1 MB |
| libjpeg runtime ピーク | 1-3 MB |
| ELF .text + .rodata | ~2.5 MB (EBOOT サイズ依存) |

注意: `sceKernelMaxFreeMemSize` = 最大**連続**ブロック。HtmlViewer 等大型 Vpl が必要な場合はこちらが重要。PSP-1000 (32MB) はさらに厳しい。**メモリ計測は実機必須** (PPSSPP は仮想 heap 全量返す)。

**軽量化効果見積もり** (HtmlViewer 同居が必要な場合):
| 対象 | 削減効果 |
|---|---|
| intraFont CACHE_LARGE → CACHE_MEDIUM | ~2 MB |
| libjpeg 使用毎 destroy | ~1 MB |
| curl 使用毎 init/cleanup | ~0.5 MB |
| GU list 262144 → 131072 | 512 KB |
| 合計 | ~4-5 MB (それでも 8MB 連続は厳しい) |

---

### PPSSPP vs 実機 差分罠 一覧

| 罠 | PPSSPP | 実機 | 対策 |
|---|---|---|---|
| sceUtilityNetconf dialog | EnableWlan=True で即 FINISHED | 画面真っ黒・操作不能 | sceNetApctlConnect 直叩き |
| AVCODEC モジュール | スタブで自動ロード | 明示ロード必須 | sceUtilityLoadModule |
| sceGuDrawArray 非テク矩形 | drop | drop | CPU-blit |
| sceJpeg 大画像 (>480×272) | 通る | 0x80650020 拒否 | libjpeg 併用 |
| FAT32 fflush | 仮想 FS で即反映 | directory entry 更新されず | fclose+fopen |
| OSK 漢字変換 | inputtype フラグ無視 | 元々 IME 非搭載 | hiragana/katakana 受容 |
| HtmlViewer Vpl | (未検証) | 8MB 連続必須 | 別 EBOOT 分離 |
| ARK-4 sceMpeg | 通常動作 | ME 内部状態破壊 | PRO-B10 使用 |
| heap 計測 | 仮想 heap 全量返す | 実際の断片化値 | 実機必須 |

**設計教訓**: PPSSPP 動作 ≠ 実機動作の前提で開発する。

---

## 一次情報源

### 学術論文 (Sony 公式) — Rank A 最高信頼度
- **Hot Chips 16 (2004)**: PSP チップの公式発表資料 "Tachyon - A PSP™ LSI"
  - https://old.hotchips.org/wp-content/uploads/hc_archives/hc16/3_Tue/8_HC16_Sess8_Pres1_bw.pdf
  - 著者 16 名 (Sony Semiconductor)
  - eDRAM 4MB (2MB game / 2MB media) の partition がブロック図に明記
  - VME が Tachyon (CXR704060 ではない、CXD2962GG) の一部として明記
  - Virtual Mobile Engine™ が "World's First Consumer Product using Reconfigurable architecture!" と明記
  - VME 歴史表 (2003 Network Walkman → 2004 PSP)
  - Allegrex dual-core (SC + ME)、ME = MIPS R4000 with FPU
  - 90nm CMOS, 6M gates, 540pin LFBGA
  - **20 年前に全て公開済みだが、開発者向けドキュメントには書かれていなかった**

### m-c/d (mcidclan) の公開研究 — Rank B (個人研究)
- **MECC (psp-media-engine-custom-core)**: ME 活用ライブラリ本体
  - https://github.com/mcidclan/psp-media-engine-custom-core
- **psp-media-engine-reload**: ME レジスタ直叩き調査
  - https://github.com/mcidclan/psp-media-engine-reload
- **psp-media-engine-cracking-the-unknown**: VME 研究 (draft)
  - https://github.com/mcidclan/psp-media-engine-cracking-the-unknown
- **psp-undocumented-sorcery**: eDRAM 拡張、OC サンプル
  - https://github.com/mcidclan/psp-undocumented-sorcery
- **psp-444mhz-plugin**: PLL 直叩き OC plugin (experimental)

### pspdev コミュニティ
- **psp-packages**: libmecore v2.0.1 等の公式パッケージ
  - https://github.com/pspdev/psp-packages
  - libmecore: https://github.com/pspdev/psp-packages/blob/master/libmecore/PSPBUILD

### コミュニティ
- **PSP Homebrew Community Discord**: オンライン 1,625+ 人規模、`#media-engine` チャンネルで m-c/d と Acid_Snake が活動
- **PSX-Place** (PS3 系だが PSP 情報もあり)
- **yapspd (PSP 総合ドキュメント)**: https://hitmen.c02.at/files/yapspd/psp_doc/frames.html
- **psdevwiki**: https://www.psdevwiki.com/psp/

### psdevwiki リバースエンジニアリング貢献者 (判明分)
Tachyon / Media Engine ページの主要貢献者 (Claude Code 調査より):
- **CelestBlue**
- **artart78**
- **davee**
- **xerpi**

これらは psdevwiki および PSP homebrew コミュニティの研究者 ※。同人誌の「コミュニティの系譜」章で使える人名。

### 田中自身の Note 記事 (Rank A、田中一次情報)
- **【狂気・再】2026年、PSPの性能を倍にする** (2026-04-04, ¥100 有料記事)
  - URL: https://note.com/tanakaps/n/n8fd986d06853 (要確認)
  - 6 つの地雷、8 段階の最適化、3 スレッド + リングバッファ設計、碓氷峠の比喩
  - 訂正追記: Snes9xTYL + LightMP3 先行事例を明記
- 関連 Note 記事 (PSP 編): psp-me-bench 追記版 (2026-04-05, n021d8faf3b7b)
- 関連 Note 記事 (PS3 編): 【狂気】【驚愕】【狂喜】2026-03-03 / 03-05 / 03-08

### キーパーソン (PSP Homebrew Community)

| 人物 | 役割 | 備考 |
|---|---|---|
| **m-c/d (mcidclan)** | MECC 作者、ME/VME 研究者 | 技術議論の一次ソース、upstream 貢献の窓口。田中のフィードバックを MECC ガード実装に反映予定 |
| **Acid_Snake** | ARK-4/ARK-5 CFW 開発者 | seplugin 使用について NG 判定。edram 拡張議論で田中の 1K データを引用 |
| **z2442** | pd (Perfect Dark) / sm64-port 移植者 | MECC 実用ユーザー。田中環境固有問題の切り分けに協力 |
| **Staff D, Hedge** | コミュニティ運営 | D から 😮 リアクションでスタッフ公認獲得 (psp-pmdvis dcache 報告) |
| **Nuclear Kommando** | KOMSTATION 記事担当 | psp-bsky Beta 共有、記事化打診 |
| **Veka** | 一般ユーザー | edram 拡張議論に参加 |
| **CelestBlue / artart78 / davee / xerpi** | psdevwiki 貢献者 | Tachyon / Media Engine ページのリバースエンジニアリング ※ |
| **crazyc** | 2005 年世界初公開 ME コード著者 | 系譜の起点、遠い祖先 |

---

## 未検証/未開拓領域 (2026-04-09 時点)

1. **SC → ME eDRAM 直接アクセス**: 可否が未確認 ※ (m-c/d の edram-dump は ME 経由方式のため直接読めない可能性が高い ※※※)
2. **VME (Virtual Mobile Engine) の homebrew 利用**: 世界的に未実装。成功すれば「世界初」※※※
3. **Scratchpad 16KB の実用活用**: ME 側からのアクセスは遅いため、SC 専用として使うべき ※※※
4. **eDRAM 拡張時のゲーム互換性**: VSH/POPS との干渉問題あり (Acid_Snake 指摘)
5. **ME 側から GE 制御**: 事例なし、VFPU 非搭載のため困難 ※※※
6. **AVC (H.264 デコーダ) の homebrew 活用**: 研究途上
7. **ME 側 eDRAM の活用余地**: psp-pmdvis は現在 `0x00000400` から約 2KB のみ使用 (Slim+ 4MB 中 0.05% ※※)。空き領域 ~3.7MB にループキャッシュ / ME ワークバッファ拡張 / リングバッファ配置の余地あり
8. **PSP 新型ブラウザ構想**: 4 層メモリ階層フル活用 (Scratchpad + ME eDRAM + メイン RAM + SC eDRAM)。既存 PSP ブラウザ (NetFront) 代替、homebrew シーンの悲願
9. **Wikipedia の Toshiba 3D IC の原典論文**: 未追跡 ※
10. **PSP-2000+ eDRAM 8MB の独立確認**: m-c/d 個人研究のみ、Wikipedia と矛盾 ※※
11. **PSP-2000 で NAND+RAM が Tachyon 内部統合**: PSP Bible (Scribd) のみ、独立確認未取得 ※※
12. **me_init_ret と table_id の関係**: 同一概念か別物か、m-c/d への追加確認推奨
13. **PSP XMB 内蔵ビジュアライザが ME 駆動か**: 田中の記憶から推測 ※※※、一次情報源未確認
14. **PSP-1000 ulv.q 命令バグ**: psdevwiki スニペットのみ、詳細未確認 ※

---

## 歴史 (圧縮版)

### 2026-04-04 ~ 04-09: PSP Homebrew Community デビュー週間

- **2026-04-04**: 田中 Note 記事【狂気・再】第 1 弾公開 (¥100 有料)
- **2026-04-05**: Note コメント欄で KOMMANDO (KOMSTATION 編集者) 初接触、psp-me-bench 追記版公開
- **2026-04-07**: pmd_psp v1.1 完成セッション (14 時間+)、MECC 全滅問題の m-c/d 報告完遂
- **2026-04-08**: psp-pmdvis GitHub public 公開、Discord #media-engine 5 時間戦記、m-c/d + Acid_Snake + Staff D から受容、m-c/d から eDRAM 8MB 情報開示、CXR704060 ハルシネーション訂正、田中パッチが m-c/d upstream 反映予定に
- **2026-04-09**: Tiny-ME 累犯発覚 + 真相判明 (pspdev/tiny-me 旧実在名)、同人誌執筆準備、PSP 関連 skill 訂正セッション

### 2026-04-07 夜: pmd_psp v1.1 完成セッション

**14 時間+ の戦闘の後半、4 本の戦線で同時進展した勝利日**。

**pmd_psp v1.1 達成事項**:
- early_switch 削除 (TH2-04 スキップバグ撲滅)
- 動的リングバッファ 256 化 (`malloc` + フォールバック)
- BSS 削減の副作用で **32MB 機 (PSP-1000) まで道連れ強化**
- デュアルデプロイ (E:/G: 両方対応の `deploy.sh`)
- tag `v1.1-stable-ring256` = 物理的最適解
- commit `a7f28d3`、private GitHub `kan8223-dotcom/pmd-psp` に退避

**ME 最適化 Phase 1-2**:
- Phase 1 調査で **prefill_wait 機械的書き換えバグ発見** (コメント「20 ブロック」vs 実装「255 ブロック」= 起動 6 秒の正体)
- Phase 2 実装: `lc_prefill_wait = 32` → 起動 0.85 秒達成
- **ただし** 1 周目後半からドレイン顕在化 → r=0 張り付き
- 田中判断: 「いじらないのが正解」→ ロールバック
- 結論: **prefill 単独では「早く + 途切れず」両立不可能、ME 速度向上が必須**

**MECC 全滅問題の技術報告**:
- m-c/d 提供の `gu-shared-list` サンプルを 3 機種 (Go/3000/1000) でテスト → 全滅
- z2442 の pd/sm64-port では正常 → **田中環境固有問題確定**
- kcall.prx タイムスタンプ調査で `meLibLoadPrx()` 後の `sceKernelLoadModule()` 段階の失敗と判明
- `0x80020139 (UNSUPPORTED_PRX_TYPE)` と整合
- 容疑者: メモステ側の共通要素 (プラグイン残骸、設定、過去のテスト痕跡)
- m-c/d への技術報告メッセージ送信完了 (プロの技術報告レベル)

**重要な原則確立**:
- **ymfm 聖域宣言**: ymfm は絶対にいじらない (チャネルスキップ/wait 削除/int32_t 化すべて失敗済み)
- **動いてる EBOOT を壊すな** (submodule 汚染、システムライブラリ汚染の経験から)
- **早く、そして途切れず、正確に鳴らす** (pmd_psp モットー)
- **MECC 独自路線**: 宣言しない、建前は「田中の知識と技術力が足りなかった」、検証協力は続ける

**Code 暴走 3 回**:
1. streaming ブランチで diff 見せずデプロイ → ME OFF → 証拠破壊
2. submodule 汚染 (旧称 tiny-me の me-lib.c を MECC 化)
3. kcall.prx 配置疑惑 → 後で無罪確定 (監督役 Claude の冤罪)

### 2026-04-08: psp-pmdvis 大戦記、PSP Homebrew Community 正式デビュー

**成果物**: https://github.com/kan8223-dotcom/psp-pmdvis (GPL-3.0, public)

**commit 履歴**:
- (初回公開) psp-pmdvis v1.0 GitHub public
- `af26e75` 旧称 → MECC 命名統一 (m-c/d 指摘対応)
- `f9fc80e` MECC v2.0.1 + 田中パッチ版差し替え
- `2939e4d` auto-load 確認、seplugin 記述削除
- `f0c72ee` README クレジット簡略化
- `5730fa6` / `0adb855` dcache リファクタ (meLib→meCore 16 箇所)

**技術達成**:
1. **MECC v2.0.1 完全移行** (pspdev 公式 libmecore ベース + 田中パッチ 4 点再適用)
2. **auto-load MECC 成功** (seplugin 完全不要、EBOOT.PBP 単独で ME ON)
3. **dcache リファクタ実測効果** (TH1 が 2 ループ → 3 ループまで安定、ymfm 聖域に触らず改善)

**Discord 戦記 (時系列要約)**:
- **19:30** 公開アナウンス → m-c/d 即 GitHub チェック、Star 1
- **20:21** m-c/d 指摘「旧称じゃない」→ 田中即対応 + push
- **20:31** m-c/d から pspdev/psp-packages/libmecore v2.0.1 情報提供
- **20:50** m-c/d 技術指摘: dcache アドバイス (SC/ME 使い分け、uncached 不要、Icache 維持)
- **21:43** Acid_Snake 指摘「seplugin NG」→ auto-load 移行決意
- **22:11** m-c/d 提案: edram-0x400000 サンプル → 田中 3 機種テスト (PSP-1000 は描画異常 = 1K 拡張非対応実証)
- **22:38** 田中発見: `writePrx()` の kcall.prx サイズ不一致 (1,740B vs 2,198B) → m-c/d 報告
- **22:45** m-c/d「ok let me put some guard on this」→ **MECC upstream にガード実装予定** (田中のフィードバックが本体改善に)
- **22:47** 田中真相解明: デプロイミス (古い EBOOT.PBP が残ってた)
- **23:29** 田中が PSP スペック表投稿 → **CXR704060 ハルシネーション発覚** → m-c/d が Hot Chips 16 論文 URL 共有 → 田中が原典読んで v3 PDF 再投稿
- **23:35** m-c/d "virtual mobile engine 2, a type of cgra trusting Sony" 発言 (本人 "trusting Sony" 留保付き)
- **0:18** m-c/d が **eDRAM 8MB 情報開示**: 「edram is 4MB on ME and 4MB on Sc side. That's for slim+. And before it, it is 2MB for each side.」 + psp-me-reload と psp-me-cracking-the-unknown の URL 共有 (**個人研究資料の共有 = 仲間として認める姿勢**)
- **0:49** 田中 dcache 実測報告 → **Staff D から 😮 リアクション** (コミュニティ公式レベルで受容)
- **1:07** m-c/d「yeah better always use me core instead except if there is no alternative」→ **田中の実装が m-c/d 公式ガイドラインに 100% 準拠**

**田中の Discord アプローチ特徴**:
- 自己紹介なし、いきなり作品公開
- 指摘されたら数分以内に対応 + push
- 謙虚だが迎合的じゃない (技術的に筋の通った反応)
- 間違い即認める (CXR704060 ハルシネーション件で Hot Chips 16 原典即切替)
- 事実ベース (MD5、サイズ、実測値で話す)
- 短い返信 (簡潔、過剰な感情表現なし)

**発展プロジェクト候補**: PSP 新型ブラウザ (4 層メモリ階層フル活用、NetFront 代替、JS エンジンは QuickJS/duktape、HTTPS は mbedTLS)

### 2026-03: PSP 開発事始め

- **世界初 (筆者の知る限り) PSP SNS クライアント 3 本 (psp-bsky / psp-note / psp-note-home)** を 1 日で完成 ※※※
- 突破口 = CPU 直接 VRAM 書込 (`sceGeEdramGetAddr() | 0x40000000`)
- pspdev + WSL + CMake 環境確立
- PC 側 Python sync tool → メモステ経由オフライン同期 (PSP は WPA3 非対応)
- KOMSTATION に紹介される
- PSP Homebrew Discord 参加、`#media-engine` で m-c/d/Acid_Snake と議論開始

---

## 注意事項

- この知識ベースは **2026-04-09 時点で判明している情報** に基づく
- VME の詳細 (PE の動作、内部命令セット等) は Sony の機密で未公開、研究途上
- OC 周波数は個体差あり、全機種で保証されない
- 新しい発見があればコミュニティで訂正される可能性あり
- **重要**: 推測と事実を区別すること (憲法第 7 条)。一次情報源を必ず確認する
- **重要**: 旧称 (Tiny-ME / TinyME / t***-me / T***yMe 等) 使用禁止 (憲法第 8 条)
- **重要**: ymfm 聖域違反禁止 (憲法第 8 条)
- **重要**: ※ マーキング付きの記述を外部発信時に引用する場合、必ず留保または出典明記
- **重要**: 過去の AI ハルシネーション 4 件 (CXR704060 / Tiny-ME / VME2 / CGRA) を告白の対象として記録

### 歴史的観点

- PSP は 2004 年発売から 22 年経過 (2026 年時点)
- eDRAM 4MB (2MB+2MB) の事実は Hot Chips 16 (2004) で公開済み、8MB 説は m-c/d 個人研究 ※※
- **開発者向けドキュメントには書かれず、20 年間ほぼ誰も活用していない** (先駆: Snes9xTYL, LightMP3 等)
- m-c/d が 2024 年以降に ME 研究を本格化させ、MECC として実用化フェーズに入った
- 田中 (kan82) は 2026-04-08 に psp-pmdvis で MECC v2.0.1 の実用化に成功、MECC 実用ユーザー (Snes9xTYL 等の先駆に続く系譜) の一人として PSP Homebrew Community に参加
- 田中は FM 音源リアルタイム合成という **新しい用途** を 21 年の系譜に追加した
- PSP は発売から 22 年経ってもまだ新発見がある機体
