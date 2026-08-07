# th06-psp-native

東方紅魔郷をPSP上でネイティブ動作させる、非公式・開発途中の移植です。エミュレータではありません。
過去に公開された類似名の`th06_PSP`とは別のプロジェクトです。

> [!WARNING]
> このリポジトリと配布EBOOTには、東方紅魔郷の原作データを一切含みません。
> 動作には、利用者自身が所有するPC版東方紅魔郷のインストールフォルダが必要です。

## 特徴

- D3D/OpenGLなどの汎用グラフィックスAPIや外部描画ラッパーを使わず、PSP専用backendから
  PSPSDKのlibGU/libGUMを介してGraphics Engine（GE）へコマンドを送るネイティブ描画です。
- m-c/dの[Media Engine Custom Core（MECC）](https://github.com/mcidclan/psp-media-engine-custom-core)を使い、
  PSP実機ではMedia Engine（ME）にPCM音声ミキシングを担当させます。MEを利用できない場合は
  メインCPU（SC）へ安全にフォールバックします。
- ゲームの論理解像度640x480を保ち、PSPの480x272へ4:3表示または全画面引き伸ばしで出力します。

## 現在の状態

PSP-3000実機で1面から6面、レミリア撃破、エンディング、タイトル復帰まで完走しています。
PSP-1000実機でも32MB RAM向けのキャッシュ解放後、タイトルデモ終了からタイトルへ復帰し、
そのまま継続動作することを確認しています。PSP-3000より若干遅く、Memory Stick読み込み中の
短い引っかかりはまだ残っています。
ただし、これは完成版ではありません。

- PSPのGraphics Engine（GE）を使用していますが、GPU/GE向け最適化は未完です。
- 背景・立ち絵・大量の弾消去など描画量の多い場面では、60fpsから約30fpsへ落ちることがあります。
- 音声、スタッフロール表示、PSP-1000での長時間プレイには未解決・未検証部分があります。
- PPSSPPでの動作だけでは実機互換を保証しません。
- セーブデータやリプレイの互換性・完全性は保証しません。

不完全なプレビュー版であることを理解したうえで使用してください。

## 更新履歴

日付ごとの変更内容と実機確認範囲は[CHANGELOG.md](CHANGELOG.md)を参照してください。

## インストール

必要なもの:

- CFWでhomebrewを起動できるPSP
- 本リポジトリのReleasesにある配布ZIP（EBOOT.PBP同梱）
- 利用者自身が所有するPC版東方紅魔郷のインストールフォルダ

手順:

1. 配布ZIPの`TH06PSP`フォルダを、メモリースティックの`PSP/GAME/`へコピーします。
2. PC版東方紅魔郷のインストールフォルダを、フォルダごと`TH06PSP`の中へコピーします。
3. コピーした原作フォルダの名前を半角ASCIIの`th6`にします。
4. XMBのゲーム一覧から`Touhou 6 PSP Native`を起動します。

コピー元とコピー先を並べた例です。左側のPC版フォルダ（この例では`kouma`）を右側の
`TH06PSP`へフォルダごとコピーし、コピー後のフォルダ名を`th6`にします。

![PC版の原作フォルダをTH06PSPへコピーする例](docs/images/install-01-copy-folder.png)

最終的な配置は次の形です。

```text
PSP/GAME/TH06PSP/
├── EBOOT.PBP
├── NotoSansJP-Regular.ttf
├── CREDITS.md
├── LICENSE
├── licenses/
└── th6/                         ← 利用者が自分でコピーする原作フォルダ
    ├── 東方紅魔郷.exe
    ├── 紅魔郷CM.DAT
    ├── 紅魔郷ED.DAT
    ├── 紅魔郷IN.DAT
    ├── 紅魔郷MD.DAT
    ├── 紅魔郷ST.DAT
    ├── 紅魔郷TL.DAT
    └── bgm/
        ├── th06_01.wav
        └── ... th06_17.wav
```

原作フォルダ内にある6個のDATファイルを確認する例です。

![原作フォルダ内の6個のDATファイル](docs/images/install-02-dat-files-reference.png)

> [!NOTE]
> 画像右側の`KOUMAKYO_*.dat`は開発時に英名へ変更して確認した配置です。この配置でも動作しますが、
> 現行版では左側の日本語ファイル名のままフォルダごとコピーできるため、個別の選別や改名は不要です。

原作フォルダの中身を選別・改名する必要はありません。日本語名DATは、PSPのFATドライバで直接開けない
場合も、ファイルサイズと`PBG3`ヘッダから自動的に識別します。原作フォルダは読み取り専用として扱い、
PSP側の設定、スコア、新しく保存したリプレイは`TH06PSP`直下へ分離します。

6個の原作DATと17曲のWAVが揃っていない場合、ゲーム本体へ入らず、画面に不足を表示して終了します。

## 含まれないもの

- 東方紅魔郷のEXE、DAT、WAV、画像、音楽、その他の原作データ
- 原作または開発中に作成したリプレイ（`.rpy`）
- スコア、設定、ログなどのユーザーデータ
- Microsoftのフォント

公開配布物にリプレイは混ぜません。`replay/`はゲームが利用者のメモリースティック上で必要に応じて使用します。

## 非公式プロジェクトについて

本プロジェクトは上海アリス幻樂団およびZUN氏による公式製品ではなく、承認・支援・保証を受けたものでも
ありません。本プロジェクトについて、上海アリス幻樂団、ZUN氏、GensokyoClub、m-c/d、PSPDEV、
PPSSPP、または下記の参照プロジェクトの作者・保守担当者へ問い合わせないでください。

質問や不具合報告を受け付ける場合の窓口は、このリポジトリ自身のIssuesだけです。原作製品やCFWの入手方法、
原作データの共有に関する質問は扱いません。

## ビルド

PSPDEV/PSPSDKが`/usr/local/pspdev`に導入済みで、CMake、Python、`xxd`が使える環境で実行します。
同梱したm-c/d MECCも初回の`make`でsourceからbuildします。

```sh
make -j"$(nproc)" all
./psp_preflight.sh --post
make release
```

`make release`は`dist/th06-psp-native-preview.zip`を作ります。ZIPにはEBOOT、再配布可能な日本語フォント、
README、Credits、各ライセンスだけが入り、原作データとリプレイは入りません。

## 参照・謝辞

実装の出所と参照したリポジトリは[CREDITS.md](CREDITS.md)にURL・用途とともに記載しています。

## ライセンス

本プロジェクト本体は、ベースである[GensokyoClub/th06](https://github.com/GensokyoClub/th06)の
portable branchに準じてGNU General Public License v3.0です。全文は[LICENSE](LICENSE)を参照してください。

同梱third-partyとフォントはそれぞれのライセンスに従います。原作データの権利は本ライセンスの対象外です。
