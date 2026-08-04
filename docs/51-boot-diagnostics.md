# 起動診断の読み方

実機が単独で「どこで止まったか」を証言できるようにするための層。判断根拠は `DECISIONS.md` D-005。

## 実機に置かれるもの

| ファイル | 中身 |
|---|---|
| `<ドライブ>:\TH06PSP_BOOT.LOG` | 今回の起動記録 (ms刻印つき) |
| `<ドライブ>:\TH06PSP_BOOT_PREV.LOG` | **前回**の起動記録。再現を試みても前回の証拠は消えない |

XMBが実際に起動したEBOOTのdevice (`ms0:`または`ef0:`) 直下へ書く。ログ先頭のパスは、
そのEBOOTの親ディレクトリである。別storageに同名フォルダがあってもそちらへ切り替えない。

## 画面

起動中は画面上部に最新12行のboot noteが出る (最新行は緑)。**USBを繋がなくても停止段階が読める。**
engineが最初のフレームを出した時点でoverlayは画面をengineへ返す。

## ログ行の意味

| 行 | 意味 |
|---|---|
| `BUILD <date> <time>` | **配達物の同一性確認**。古いEBOOTを配達した前科があるので必ず突き合わせる |
| `CLOCK 333MHz BUS 166MHz` | `scePowerSetClockFrequency` が効いた |
| `SYSMEM LEFT ... MAXBLOCK ...` | newlibがヒープを取った**後**の残り。ヒープサイズではない |
| `ARC LOAD BEGIN/END` | runtimeでCM/ST等のPBG3 archive自体を開く境界。BEGINだけならarchive open/header処理内で停止 |
| `ARC BEGIN/END <name>` | PBG3内の1 entryを展開する境界。BEGINだけなら、その名前のentry展開中に停止 |
| `LARGE NEW ENTER/OK` | 64KiB以上(runtime)または1MiB以上(boot)のC++ allocation境界。ENTERだけなら確保処理中 |
| `GE TEX BEGIN/END` | 64KiB以上のruntime texture確保境界。BEGINだけならtexture allocation中、`GE TEX OOM`なら確保失敗 |
| `DEMO TRACE ARMED` | `demo00.rpy`を見つけ、デモ登録後最初のcalc/draw/GE一回目の短期traceを開始 |
| `CHAIN CALC/DRAW BEGIN/END` | デモ登録後の更新・描画chain境界。BEGINだけならそのchain callback内でprocess-wide停止 |
| `TRACE GE CLEAR/TRANSFORM/DRAW/SWAP` | デモ登録後に初めて到達したGE操作。SWAPはBEGIN/ENDで`finish/sync/vblank/swap`区間も判定 |
| `ENGINE ENTER` | `th06_engine_main()` へ入った |
| `FOPEN <path> OK\|NG` | engineが開いたファイルと結果。boot期間だけ記録する |
| `SDL INIT` / `SDL CREATEWINDOW` / `SDL CREATERENDERER` / `SDL CREATETEXTURE` | `GameWindow::CreateGameWindow()` → `Software::Init()` の内訳 |
| `SCREEN HANDED TO ENGINE` | engineが最初のフレームを出した。overlay終了 |
| `GE FIRST SUBMIT` / `SYNC OK` / `SWAP OK` | 最初のGE提示の三段階。途中で切れた場合は次の未到達操作がcrash/hang地点 |
| `SOUND INIT OK` / `INPUT INIT OK` / `CHAIN INIT OK` | engine自身のbreadcrumb (`src/main.cpp`)。`src/` は無改変 |
| `BOOT PHASE END: N FRAMES PRESENTED` | 診断終了。ここから先は通常動作でありログは書かれない |
| `WATCHDOG TIMEOUT <n>s AFTER: <stage>` | **hangした**。この行の`<stage>`が最後に通過した段階 |
| `ENGINE RETURN <n>` | engineが正常に戻った |

## 症状の切り分け

| 実機の様子 | ログ | 意味 |
|---|---|---|
| 青画面のまま固まる → 数十秒でXMBへ戻る | `WATCHDOG TIMEOUT` あり | **hang**。`AFTER:` の段階を疑う |
| 突然XMBへ戻る、またはCFWのダンプ画面 | 最終行が途中で切れ、`WATCHDOG` 無し | **crash**。最終行の直後の処理を疑う |
| 画面にboot noteが並んで止まる | 同上 | overlayが生きている＝engineはまだ画面を取っていない |
| `ENGINE RETURN 0` | `FOPEN ... NG` あり | 原作データ不足。これは想定どおりの終了 |

CPU例外ハンドラは user-mode EBOOT では登録できない (D-005)。EPCが欲しくなったら kernel PRX が要る。

## 脱出

**L + R + START を約1秒**保持すると、engineが暴走していてもXMBへ戻る (優先度0x11の常駐スレッド)。
強制シャットダウンの前にこれを試すこと。

## 診断層そのものを検証し直す

```bash
make -B BOOT_PROBE=1 SELFTEST=1 all -j4   # わざとhangする。WATCHDOG TIMEOUTが出れば健全
make -B BOOT_PROBE=1 all -j4              # engine抜きプローブ。△でわざと不正ストア
make -B all -j4                           # 本体
```

`SELFTEST=1` を配達してはならない。
