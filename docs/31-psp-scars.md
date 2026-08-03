# PSP開発の前科集 (同じ穴に落ちるな)

> 母艦のfeedback記憶をそのまま引き渡したもの。いずれも**実際に踏んだ**地雷であり、
> 一般論ではない。着手前に必ず読むこと。

---

---
name: PSP IMPORT_START フラグ必須知識
description: PSP開発でIMPORT_STARTのフラグを間違えると0x8002013C (LIBRARY_NOTFOUND)で起動不能になる。sceMpeg等の動的ロードモジュールには必ず0x00090000 (SCE_LIB_WEAK_IMPORT)を使え。0x40010000は絶対NG。
type: feedback
---

# PSP IMPORT_START フラグ — 二度と間違えるな

## 事件 (2026-04-11)
psp-moonlight H.264デコードテストで、カスタムimportスタブの`IMPORT_START`フラグを`0x40010000`と書いて3回連続起動失敗（0x8002013C）。田中に「馬鹿じゃねえの」と言われた。当然。

## 原因
`0x40010000`にはSCE_LIB_WEAK_IMPORT (0x8) ビットがない。PSPカーネルはPRXロード時に全importを解決しようとするが、sceMpeg/sceMpegbase等のAVモジュールは起動時に未ロード。WEAK_IMPORTフラグがないと即死する。

## 正解
```asm
// sceMpeg等の動的ロードモジュール → 0x00090000 (WEAK_IMPORT)
IMPORT_START "sceMpeg", 0x00090000
IMPORT_START "sceMpegbase", 0x00090000

// sceDisplay等の常駐モジュール → 0x40010000 でOK
IMPORT_START "sceDisplay", 0x40010000
```

## フラグの意味
- `0x00090000` = SCE_LIB_AUTO_EXPORT(0x1) | SCE_LIB_WEAK_IMPORT(0x8) → モジュール未ロードでも起動OK、後でsceUtilityLoadAvModule()呼べばカーネルが遅延バインド
- `0x40010000` = 常駐モジュール向け。起動時に解決必須。

## ルール
1. **カスタムimportスタブを書く前に、SDK内の既存スタブ(.S/.a)のフラグを必ず確認しろ**
2. sceMpeg, sceMpegbase, sceVideocodec等のAVモジュール → 必ず`0x00090000`
3. main()内で`sceUtilityLoadAvModule(PSP_AV_MODULE_MPEGBASE)`と`sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC)`を呼んでからsceMpeg関数を使う
4. SDK側の-lpspmpeg/-lpspmpegbaseと自作スタブを混在させるな。全部自作に統合するか、SDK側だけ使え（混在するとpsp-fixup-importsが"stubs out of order"警告→壊れる）

## 前科
- 3回連続起動失敗
- 田中に「マジで調べてからやってほしい」「馬鹿じゃねえの」と言われた

---

---
name: ARK-4 sceUtilityLoadAvModule(MPEGBASE) 不具合と対処
description: PSP Go ARK-4でsceUtilityLoadAvModule(PSP_AV_MODULE_MPEGBASE)が0x80020F3Cで失敗する。flash0:/kd/mpeg_vsh.prxを直接ロードで回避。全PSPプロジェクト共通知見。
type: feedback
---

# ARK-4 MPEGBASE ロード不具合 (2026-04-11 発見)

## 症状
- `sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC)` → 0x00000000 (成功)
- `sceUtilityLoadAvModule(PSP_AV_MODULE_MPEGBASE)` → 0x80020F3C (失敗)
- その後 `sceMpegInit()` → 0x8002013A (SCE_KERNEL_ERROR_LIBRARY_NOT_YET_LINKED)

## 原因
ARK-4 CFW (PSP Go) で MPEGBASE のユーティリティロードが壊れている。
AVCODECは avcodec.prx (sceMpegbase) をロードするだけで、sceMpeg本体 (mpeg_vsh.prx) は MPEGBASE 経由でロードされるはず。

## 対処 (pmplayer-advance方式)
```c
ret = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);  // これは成功する
ret = sceUtilityLoadAvModule(PSP_AV_MODULE_MPEGBASE);
if (ret < 0) {
    // flash0から直接ロード
    SceUID modid = sceKernelLoadModule("flash0:/kd/mpeg_vsh.prx", 0, NULL);
    if (modid >= 0) {
        int status = 0;
        sceKernelStartModule(modid, 0, NULL, &status, NULL);
    } else {
        // 代替パス
        modid = sceKernelLoadModule("flash0:/kd/mpeg.prx", 0, NULL);
        if (modid >= 0) {
            int status = 0;
            sceKernelStartModule(modid, 0, NULL, &status, NULL);
        }
    }
}
```

## 補足: sceMpegGetAvcNalAu のデータ形式
- `nal_buffer` は AVCC形式 (4バイトBEレングスプレフィクス + NALデータ)
- Annex B (00 00 00 01 スタートコード) は NG → 0x80618007 (ERROR_MPEG_ILLEGAL_STREAM)
- `sps_buffer`/`pps_buffer` はプレフィクスなし生NALデータ

---

---
name: feedback_psp_ark4_hang
description: ARK-4のsceMpegはデコードエラー時にハングする。PRO-B10で開発すべし。PSP Go/3000共通。6回連続チェックポイント9停止で8時間浪費の前科。
type: feedback
---

# ARK-4 sceMpeg ハング問題

## 症状
- sceMpegAvcDecode がエラー (0x80628001/0x80628002) を返す時、ARK-4ではハングしてウォッチドッグ強制シャットダウン
- PSP Go (ARK-4) と PSP-3000 (ARK-4) で再現確認
- ログも書けない (log_resume の sceIoOpen APPEND も失敗)

## 対策
- 開発機を PSP-1000 PRO-B10 に切替
- PRO-B10 では同エラーでもクラッシュせず最後まで走り、ログ完全取得可能
- ARK-4 は将来サポート目標 (デコード成功後に再検証)

## 教訓
- 症状(ハング)ではなく根本原因(デコードエラー)を見ろ
- CFW切替で初めて真のエラーコードが判明
- 6回連続同一箇所停止で8時間浪費した前科

---


# ドライブレター固定の思い込み禁止 (前科1回 2026-07-04)

記録上「E:=PSP-1000 / G:=PSP Go」だったが、2026-07-04時点ではE:はSSDが占有し、PSPはG:で列挙された。
Windowsのドライブレターは接続機器の構成で変わる。

**Why:** 古いメモリのレター割当を信じてdeploy.sh Eを実行 → SSDのE:\PSP\GAME\MOONTEST に誤書き込み (即掃除して実害なし)。

**How to apply:**
- デプロイ前にユーザーに「今日はどのレターか」を確認するか、PowerShellで各ドライブの中身 (PSP\GAME の既存フォルダ等) を見て実機か判定してから書く
- [[psp-templates]] の psp_go.sh CONFIG のレターも「目安」であり絶対ではない
- どのPSP実機(1000/Go/3000)がそのレターかもセットで確認 — ARK-4機(Go)はsceMpegテスト不適 [[KNOWLEDGE_PSP]]

