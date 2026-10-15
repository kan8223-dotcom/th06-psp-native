# MIDIプレビュー / MIDI preview

## 使い方

1. 自分のWindows PCの`C:\Windows\System32\drivers\gm.dls`を用意します。
2. メモリースティックの`PSP/GAME/TH06PSP/th6/gm.dls`へコピーします。
   別の原作データフォルダを選んでいる場合は、そのフォルダの直下へ置きます。
3. ゲームを起動し、初回の音色キャッシュ生成を待ちます。
4. タイトルのOptionでBGMを「MIDI」にします。配置後に起動し直す必要があり、起動中の追加では有効になりません。

`MIDICACHE.BIN`はEBOOTと同じフォルダへ生成します。標準の音源では約1.2MBです。
キャッシュ作成後も`gm.dls`を残してください。原作DAT6個とWAV17曲も引き続き必要です。
音源を置き換えても古い音色が使われる場合は、ゲームを終了して`MIDICACHE.BIN`だけを削除し、
再起動すると再生成します。元の音源や原作データを削除する必要はありません。

MIDIはPSPの合成器でリアルタイム演奏します。WindowsのMSGSプログラムそのものを移植・実行する方式では
ありません。`gm.dls`が無い、読み込めない、またはMIDIプレイヤーを使えない場合はWAVへ戻ります。
PSP-1000ではMIDI合成にMEが必要です。PSP-2000/3000などの64MB機にはSC合成経路もあります。
通常版に自動操作・無敵・試験用の自動終了はありません。

## 確認範囲と既知の制約（2026-10-15）

- MIDI再生はリアルタイム合成の負荷により、ゲームプレイのパフォーマンスを低下させます。
  ME使用時も処理落ちが増える場合があります。パフォーマンスを優先する場合はBGMをWAVにしてください。
- 待機修正前のMIDI開発版は、PSP-1000の通常プレイで全6面、ED、結果、タイトル復帰、スコア保存を
  実機ログで確認しています。
- 現行版はME完了待ちのbusy pollingをやめ、協調待機に戻しています。現行PSP-1000ログは
  5面進行後のタイトル復帰までです。操作が軽くなったとの利用者報告もあります。
- 現行PSP-3000での通しプレイ成功は利用者からの報告です。独立したログ計測結果ではありません。
- 処理落ちとMIDI音切れは残ります。現行PSP-1000ログにも4面の区間で音声供給不足が1回あります。
  この版は60fps、音切れゼロ、Windows MSGSとの完全なPCM一致を保証しません。
- PPSSPPやホスト側の試験は実機のME性能・音声連続性の証明ではありません。
- MEの応答が無い異常時は、参照中の音源・曲メモリを保護するため通常終了を抑止し、
  手動の電源断を求める場合があります。その場合は画面の案内に従ってください。

一般利用のために開発用の診断試験を実行する必要はありません。不具合を報告する際も、音源や原作データ、
キャッシュは添付しないでください。

## Setup and limitations

Copy your own Windows `C:\Windows\System32\drivers\gm.dls` to `PSP/GAME/TH06PSP/th6/gm.dls` before starting
the game. If you selected another original-data folder, place it directly inside that folder instead.
The first launch builds `MIDICACHE.BIN` beside `EBOOT.PBP`; then select MIDI in the title screen's Option menu.
Keep the original `gm.dls`, all six DAT archives, and all 17 WAV tracks on the Memory Stick.
If stale instruments remain after replacing the file, exit the game, remove only `MIDICACHE.BIN`, and restart.

This uses a PSP synthesizer, not the Windows MSGS executable. Unavailable MIDI falls back to WAV.
PSP-1000 requires the ME; 64 MB models such as PSP-2000/3000 also have a main-CPU synthesis path.

Real-time MIDI playback reduces gameplay performance. Using the ME does not eliminate the synthesis overhead,
and additional slowdown may occur. Select WAV BGM if gameplay performance is your priority.

PSP-1000 logs from the earlier MIDI development build cover the full game, ending, result screen, and score save.
After replacing busy polling with cooperative waits, current PSP-1000 logs cover play into stage 5 and return to
title. The user reports improved responsiveness on PSP-1000 and a complete run on PSP-3000 with the current build.
These reports are not a controlled performance measurement. Frame-rate drops and audio interruptions remain;
the current PSP-1000 log still records one underrun in the stage 4 interval.
No constant frame rate, uninterrupted audio, or exact Windows MSGS PCM match is promised.
If the ME stops acknowledging work, the safety path may prevent a normal exit and request a manual power-off
to protect memory still in use. Follow the on-screen instructions in that case.
Development diagnostics are not a prerequisite for using this preview.

## ソースと配布 / Source and distribution

共通合成器・シーケンサは[`third_party/touhou_dls_common`](https://github.com/kan8223-dotcom/th06-psp-native/blob/v1.1.0-preview/third_party/touhou_dls_common/README.md)に
GPLv3で同梱しています。ビルドに利用者の音源は不要です。
`gm.dls`、音色・波形データ、生成キャッシュ、原作MIDI/DAT/WAVは公開物へ含めません。

The shared synthesizer and sequencer are bundled under GPLv3. Building the program does not need an instrument
file. Do not upload `gm.dls`, instrument/waveform data, caches, or original MIDI/DAT/WAV files to the repository.
