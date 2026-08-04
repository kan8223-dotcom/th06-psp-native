# Credits / References

## 原作とエンジン

- [上海アリス幻樂団](https://www16.big.or.jp/~zun/) / ZUN — 東方紅魔郷の原作。
  原作データは本プロジェクトに含まれません。
- [GensokyoClub/th06](https://github.com/GensokyoClub/th06) portable branch @ `9a1c50b` —
  本移植のエンジン本体。プロジェクト本体のGPLv3ライセンスはこの上流に準じます。

## 直接参照した移植・PSP実装

- [kan8223-dotcom/th06_ps3](https://github.com/kan8223-dotcom/th06_ps3) —
  TH06のプラットフォーム分離、音声、入力、ファイル境界、全編実機検証の直接の土台。
- [Swiizyu/th06-switch](https://github.com/Swiizyu/th06-switch) —
  portable engineの別プラットフォーム実装とFAT向けファイル処理の参照。
- [kan8223-dotcom/psp-pmdvis](https://github.com/kan8223-dotcom/psp-pmdvis) @ `18fb0b1` —
  PSP実機で稼働済みのMECC初期化、SC/ME共有、キャッシュ同期、配達手順の基準。
- [mcidclan/psp-media-engine-custom-core](https://github.com/mcidclan/psp-media-engine-custom-core) —
  m-c/dによるPSP Media Engine Custom Core（MECC）。vendored copyのライセンスは
  `psp/third_party/me-custom-core/LICENSE.md`を参照してください。

## SDK・検証環境

- [PSPDEV/PSPSDK](https://github.com/pspdev/pspsdk) — PSP homebrew SDK、libGU、PSP API headersとsamples。
- [PPSSPP](https://github.com/hrydgard/ppsspp) — 高速な事前検証に使用。最終判定はPSP実機です。
- PSP homebrew community、m-c/d、Acid_Snake、およびPSPハードウェア研究の各貢献者。

## 同梱またはリンクするthird-party

- FreeType — 動的日本語テキスト描画。
- Noto Sans CJK JP — Copyright 2010–2012 Google Corporation、SIL Open Font License 1.1。
  配布版はCP932で表現可能な文字へsubsetした`NotoSansJP-Regular.ttf`を含みます。
- libpng、libjpeg、zlib、bzip2 — 原作assetのdecodeに使用。
- TinySoundFont — Bernhard Schelling / SFZero: Steve Folta、MIT License。
  MIDI経路のコード上の依存です。サウンドフォントは配布しません。

本プロジェクトについて、上記の原作者・上流・参照先へ問い合わせないでください。
これらのプロジェクトは本移植のサポート窓口ではありません。
