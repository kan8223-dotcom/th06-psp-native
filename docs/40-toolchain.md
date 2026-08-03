# 母艦のPSP開発環境 (2026-08-03 実地調査)

引き渡し時点で母艦(WSL / Ubuntu-24.04)を実際に叩いて確認した状態。
**推測は「未確認」と明記してある。書いていないことを埋めるな、確かめろ。**

## 1. ツールチェーン

| 項目 | 値 |
|---|---|
| インストール先 | `/usr/local/pspdev` |
| コンパイラ | `psp-gcc (GCC) 15.2.0` (`/usr/local/pspdev/bin/psp-gcc`) |
| SDKパス | `psp-config --pspsdk-path` → `/usr/local/pspdev/psp/sdk` |
| 環境変数 | `PSPDEV=/usr/local/pspdev` は設定済み。**`PSPSDK` は未設定** |
| 構成マニフェスト | `/usr/local/pspdev/build.txt` (pspsdk 2026-02-25、binutils Allegrex 2.44、psplinkusb / ebootsigner / psp-pacman 同梱) |
| 展開時期 | 本体 2026-03-02、トップ階層のみ 2026-04-08 に更新 (原因未確認) |

`PSPSDK` が未設定なのは問題ではない。既存プロジェクトのMakefileは毎回
`$(shell psp-config --pspsdk-path)` で解決している。

## 2. Makefileの定型 (既存4プロジェクトで共通)

先頭2行と末尾2行はどのプロジェクトも同一:

```make
export PATH := /usr/local/pspdev/bin:$(PATH)
export PSPDEV := /usr/local/pspdev
...
PSPSDK=$(shell /usr/local/pspdev/bin/psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
```

共通して定義されているもの: `EXTRA_TARGETS = EBOOT.PBP`、`PSP_EBOOT_TITLE`、
`CFLAGS` に `-G0 -Wall -DPSP`、`LIBS` に `-lpspge -lpspctrl -lpsppower -lm`。

プロジェクト間の差分:

| | pmd_psp / psp-pmdvis-public | moonlight の各テスト |
|---|---|---|
| `BUILD_PRX` | `0` | `1` |
| `PSP_FW_VERSION` | `660` | `660` (h264_decode_test だけ未定義、意図か抜けか未確認) |
| 最適化 | `-O3 -fno-pic -funroll-loops` | `-O2` |
| C++ | `CXXFLAGS` に `-fno-exceptions -fno-rtti -fpermissive` | 使っていない |
| 特徴的なLIBS | `-lme-core -lintrafont -lstdc++ -lsupc++` | `-lpspkubridge -lpspvideocodec -lpspnet_*` |

**TH06はC++なので `pmd_psp` 系の CXXFLAGS が参考になる。**
`-fno-exceptions -fno-rtti` で通るかは engine 側の実装次第なので、最初に確かめろ。

## 3. 雛形と憲兵 (`~/psp_templates/`)

2026-07-04 に整備された5点。**サブディレクトリは無く、これで全部。**

| ファイル | 役割 |
|---|---|
| `psp_preflight.sh` (13KB) | **PSP版 憲兵 v1.0**。ビルド前 / `--post` でデプロイ前に既知の地雷を全数検査 |
| `psp_go.sh` | 検査→ビルド→検査→デプロイの一発ラッパー (4フェーズ) |
| `psp_newproj.sh` | 新規プロジェクト生成。`~/pmd_psp/Makefile` をベースにsedで書き換える |
| `main_minimal_template.c` | ME不使用の最小雛形 (出典: psp-moonlight のH264テスト) |
| `me_mecc_template.c` | MECC v2.0.1 でMEを使う雛形 (出典: pmd_psp/main.c) |

`psp_preflight.sh` が検査する項目 (= **これらは全部、過去に実際に踏んだ地雷**):

1. ビルド系: `build.mak` include、EBOOT.PBP 生成経路、`PSP_FW_VERSION`、`-lme-core` 使用時の `libme-core.a` 実在
2. モジュールマクロ: `PSP_MODULE_INFO` 必須 (無ければFAIL)
3. **importスタブの `IMPORT_START` アドレス**。`0x40010000` は誤り、正解は `0x00090000`。**前科3回**
4. MECC/ME系: 共有バッファの `aligned(64)+section(".uncached")`、uncachedアクセスの `0x40000000|`、
   初期化順序、ME側 scratchpad禁止、ME側 double禁止、ヒープ負値 など
5. `kcall.prx` のサイズ検査 (2,198B=正常 / 1,740B=壊れ)
6. **ymfm聖域** — 検出したら警告のみ。自動修正禁止 (**前科: 14時間浪費**)
7. リングバッファ既定値
8. 実機/PPSSPP差分: sceJpegの480超、非テクスチャ矩形がPPSSPPでdropされる、FAT32の`fflush`罠、
   HtmlViewerのVpl、USBカーネルモジュール手動ロード、`ms0:`の2fd同時オープン禁止
9. **`/mnt/e` `/mnt/g` の直接参照禁止** (デプロイ系で見つけたらFAIL。**前科12回**)
10. `--post`: EBOOT.PBP の存在と、ソースより新しいか (stale検出)

**このrepoにも `psp_preflight.sh` / `psp_go.sh` を配備して使え。** ゲートを通さない配達をするな。

注意: これらの雛形が実際に使われて生成されたプロジェクトは**確認できた範囲では存在しない**
(既存3プロジェクトは雛形整備より前の4月製)。つまり **雛形自体は実戦で回っていない**。
最初に使うのはお前だ。壊れていたら直せ。

## 4. 実機へのデプロイ経路

**WSLから直接ドライブを触るな。** PowerShell経由でUNCパスからコピーするのが確立された作法:

- PowerShell: `/mnt/c/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe`
- WSL側のパスは `\\wsl.localhost\Ubuntu-24.04\...` として渡す
- 転送先: `<ドライブ>:\PSP\GAME\<GAME_NAME>\EBOOT.PBP`
- **`/mnt/e` `/mnt/g` の直接参照は禁止** (preflight がFAILにする。前科12回)

ドライブレターの割り当て (スクリプトのコメントより):

- `E:` = PSP-1000 / CFW PRO-B10 — **開発・検証機**
- `G:` = PSP Go / ARK-4 — 「コード追加で起動コケ前科2回」。E:で検証してから回す

**レターは接続構成で変わる。** 実際、調査時点の `E:` はPSPではなく別の外部ストレージだった
(`$RECYCLE.BIN` / mp3 / ps3知識バックアップ 等)。`/mnt/g` は存在すらしない。
**デプロイ前に必ず実物を確認しろ** ([[drive-letters-shift]] の前科)。

PSP側のストレージprefixはコードで分岐する作法: `ef0:` が開ければ PSP Go、駄目なら `ms0:`
(実例: `~/pmd_psp/main.c:24-38`)。

実機からのログ回収は `~/pmd_psp/fetch_log.sh` が手本 (E:/G: 両対応でUNC経由で吸い上げる)。

## 5. エミュレータ (PPSSPP)

**PPSSPPの実行ファイルは母艦に見つからなかった。** WSL側にもWindows側にも無い
(`/mnt/c/RetroArch-Win64/info/ppsspp_libretro.info` は情報ファイルだけで、コア本体は無い)。

ただし **メモリスティックディレクトリは実在する**: `~/.ppsspp/`。
2026-04-11 に moonlight のH264テストを走らせたログが残っている
(`~/.ppsspp/moonlight_h264_test.log`)。つまり当時は動くPPSSPPがあった。

**PPSSPPで予選をやりたいなら、まず入れ直すところから。** 田中に確認しろ。
そして入れても、preflightが警告する通り **PPSSPPと実機は挙動が違う**。最終判定は必ず実機。

## 6. 既存のPSPプロジェクト (参考実装)

| repo | 内容 | HEAD |
|---|---|---|
| `~/pmd_psp` | PMD音源プレイヤ。MECC(ME)を使う。Makefileの手本 | `1fab562` 2026-04-09 |
| `~/psp-pmdvis-public` | 上の公開版。MECCディレクトリ名だけが差分 | `18fb0b1` 2026-04-09 |
| `~/psp-moonlight` | 動画ストリーミング。PRXビルド + videocodec系 | `db8d792` 2026-04-11 |
| `~/psp_me_bench`, `~/psp-444mhz-plugin`, `~/pmd_psp_fmp`, `~/pmd_psp_mecc` | 未調査 |

MECCのビルド済みライブラリ実体: `~/pmd_psp/tiny-me/build/libme-core.a`、
`~/psp-pmdvis-public/me-custom-core/build/libme-core.a`
