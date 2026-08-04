# 引き渡し目録 — 読む順序

2026-08-03、母艦(Claude)からSol(Codex)へのPSP知識一括引き渡し。
`AGENTS.md` を読んでからここに来ること。

## 読む順序

| # | 文書 | 中身 | 誰が書いたか |
|---|---|---|---|
| 1 | `10-th06-engine.md` | **TH06エンジンの構造と、PS3移植で判ったこと** | 母艦の書き下ろし (PS3移植の当事者記録) |
| 2 | `11-psp-backend-design.md` | **TH06 PSP backendの設計基準** (GU、音声、MECC、検証ゲート) | 2026-08-04書き下ろし |
| 3 | `40-toolchain.md` | 母艦のPSP開発環境の実態 (SDK・Makefile定型・憲兵・デプロイ経路・PPSSPP) | 2026-08-03に実地調査したもの |
| 4 | `31-psp-scars.md` | **PSP開発の前科集**。短い。着手前に必ず読め | 母艦のfeedback記憶をそのまま |
| 5 | `20-psp-hardware.md` | PSPハード知見 (Tachyon/eDRAM/ME/MECC/機種差/実戦Tips) 1100行 | 知識ベースのコピー (**stale**) |
| 6 | `30-psp-sdk-knowledge.md` | PSP SDK/homebrew実戦知見 (sceMpeg・CFW差異・デバッグ記録) 600行 | 知識ベースのコピー (**stale**) |
| 7 | `21-psp-me.md` | Media Engine 詳細。PMDVISの実績と`11`を優先して引く | 知識ベースのコピー (**stale**) |
| 8 | `50-validation.md` | 実行検証の事実記録（PPSSPPと実機を混同しない） | PSP移植作業記録 |
| 9 | `12-me-audio-design.md` | PSPPMD実績を使うME音声ワーカー、SC fallback、timeoutの設計契約 | 2026-08-04実機計測後 |
| 9 | `51-boot-diagnostics.md` | **起動診断の読み方**。実機が止まった時に最初に開くもの | 2026-08-04 母艦 |

4〜6は通読するものではない。**引く**もの。着手前とハマった時に `grep` しろ。

## 鮮度について (重要)

`20` / `21` / `30` は母艦ローカルのコピーであり **正本ではない**。正本はサーバ側の
`psp-hardware-knowledge` 新版で、そちらにしか無い記録がある
(psp-bsky案件の地雷記録、preflight 4項目の正式根拠など)。

**「ここに書いていない = 存在しない」と判断するな。** 割れたらサーバ側を取り寄せて照合しろ。
田中はこの前提を承知の上で「ローカル版で進めろ」と判断している (2026-08-03)。

各文書の冒頭にも鮮度警告を貼ってある。信頼度マーキング (※) の規則は
`20-psp-hardware.md` の冒頭章にあり、**※付きは未検証**として扱うこと。

## このrepoに入っているコード

| 場所 | 何か |
|---|---|
| `src/` | TH06エンジン本体 (GPLv3、上流decomp由来)。**直接書き換えるな** |
| `shim/` | SDL2 APIの最小自前実装。プラットフォーム差はここと `psp/` で吸収する |
| `psp/` | **まだ無い。お前が書く**プラットフォームbackend |

`src/` からはGL系バックエンド (`FixedFunctionGL`/`WebGL`/`GLFunc`) と
他OS向けMIDI (`MidiAlsa`/`MidiCoreAudio`/`MidiWin32`) を削除済み。
ただし **`src/GameWindow.cpp` は `#ifndef TH06_PS3` でGLを include している** ので、
PSPビルドではそのままでは落ちる。詳細と対処の方向は `10-th06-engine.md` §5.5。

原作データ (DAT/WAV) とSF2サウンドフォントは**意図的に入れていない**。
実体は `~/th06_ps3_public/assets/`、供給方法は田中の判断事項。
