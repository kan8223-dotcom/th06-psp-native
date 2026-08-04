# PSP移植の判断記録

## D-001: 開発版はメモリースティック起動

2026-08-04、田中の指示。開発中の EBOOT はメモリースティックから起動し、CD/UMD 認証・原作データ供給の実装は最後の配布工程まで保留する。したがって初期 backend は `ms0:` を前提にせず、相対パスを使う engine の接続前に起動・描画・入力・音声を独立して検証する。

## D-002: 最初の成果物は PSP 起動ブートストラップ

`psp/main.cpp` は engine をまだリンクしない。PSP SDK / EBOOT 生成 / GU ダブルバッファ / vblank / コントローラ / 終了 callback を実機で先に確立するための最小アプリである。原作データは含めない。

解像度方針 (640x480 内部描画か 480x272 直描きか) と MIDI/SF2 の採否は、engine 接続前の未決定事項として残す。

## D-003: backend実装の前にMEを含む設計を固定する

2026-08-04、田中の指示。`docs/11-psp-backend-design.md` を実装の基準にする。MECCはPMDVISで実績のある、明示的なcommand mailbox・uncached共有領域・PCMリング・SC/ME cache protocolの形でのみ用いる。採否は実機のSC-only計測後に決める。

## D-005: 起動診断は実機が単独で証言できる形にする

2026-08-04。実機初回の症状は「青画面で停止、HOMEも効かない」だけで、crashなのかhangなのか、
どの段階なのかを一切分離できなかった。USBを抜いてログを取りに行く往復も、強制シャットダウンも、
すべてここから来ている。

そこで `psp/` に起動診断層を置いた (`diag.cpp` / `text.cpp`)。方針は3つ。

1. **画面が第一の証言者にする。** boot noteはログと同時に画面へ描く (`psp/text.cpp` の4x6ビットマップ)。
   VRAM直書きでkernel呼び出しを一切しないので、他が動かない状態でも出る。USB往復が要らない。
2. **hangは自分で名乗って帰る。** boot watchdog (既定30秒) がengineより高い優先度で走り、
   停止したら最後のstage名をログと画面に出してXMBへ戻る。強制シャットダウンを不要にする。
   crashとhangはこれで分離できる: hangなら必ず `WATCHDOG TIMEOUT` 行が残り、crashなら残らない。
3. **診断は本番に残さない。** overlayは最初のフレーム提示で画面を engine に返し、
   ログ・fopenトレース・watchdogは60フレーム提示で終わる。時間ではなく進捗で退場する。

加えて、engineが押し込む脱出口として L+R+START の常駐スレッド (優先度0x11) を入れた。
engine側スレッドが暴走していても効く。

**CPU例外ハンドラは入れられない。** `pspDebugInstallErrorHandler` は
`sceKernelRegisterDefaultExceptionHandler` を要求し、これは libpspkernel の kernel import である。
user-modeのEBOOTにリンクするとモジュールロード自体が失敗する (0x8002013C — IMPORT_STARTの前科と同じ穴)。
EPCが要るなら kernel PRX を別に持つしかなく、それは今回の範囲外とした。

## D-004: PSPの出力は480x272直描き

2026-08-04、田中の決定。ゲーム論理座標640x480は維持し、PSPビルドだけ`GAME_WINDOW_WIDTH_REAL=480`、`GAME_WINDOW_HEIGHT_REAL=272`を指定する。既存のviewport scaleによってSoftware rendererを480x272へ直接出力させ、640x480からの縮小コピーをしない。`src/`は変更しない。

## D-006: PSP本体描画はSoftwareではなくGE/GUを使う

2026-08-04。ユーザー所有の原作データを外部PPSSPP memstickへ置いた状態で、Software rendererは
タイトル画面まで到達したが約5fpsだった。画面も青寄りで、`ZunColor=0xAARRGGBB`を
PSP `8888=0xAABBGGRR`へ無変換コピーしたR/B逆転を確認した。これはpresentだけの問題ではなく、
全ピクセルをSCでラスタライズする経路そのものが性能要件を満たさない。

田中の指示により`GfxInterface`をPSP GE/GUへ直接実装する。`src/`は変えず、既存の
FixedFunctionGL候補を`psp/graphics/`から差し替える。Softwareは比較・非常用に残す。
色変換、逆depth、alpha ref=4、16bit texture変換を明示し、PPSSPPで色とfpsを再測定してから
実機へ進む。

## D-007: 静止surfaceはSDL世代番号でGE cacheへ固定する

2026-08-04。640x480 JPEGを表示できるよう論理1024x512 textureを物理512x512へ縮約した後も、
タイトルでは30fpsだった。`AnmManager::ApplySurfaceToColorBuffer()`が静止画を毎フレーム
921,600 bytes抽出し、一時textureを作成・破棄するためである。`src/`は変更せず、SDL surfaceへ
PSP固有の世代番号を付け、GE backendの2面565 cacheと対応させる。初回だけ縮小し、cache hit時は
`SDL_LockSurface`から`ExtractSurfacePixels`の再コピーも省略する。surface解放またはcache evictionで
cache済み印を解除する。

PPSSPP v1.20.4で変更前の起動ロゴは30.00fps、変更後は59.88fps。stage 3 demoは59.88〜60.00fps。
実機性能は未確認。

## D-008: PSPの動的日本語文字はFreeType shimを使う

2026-08-04。PS3で直した`TextHelper`の合成・downsample処理は既に`src/`へ入っていたが、
`shim/sdl_stub.cpp`の実TTF実装が`__PSL1GHT__`限定で、PSPは空の8x8 surfaceを返すstubだった。
PSPでもFreeTypeを有効化し、font pathをEBOOTディレクトリへ明示解決する。PPSSPPの外部memstickへ
ユーザー所有`msgothic.ttc`を置いた開発確認で、敵スペル名「霊符『夢想封印』」の表示を確認した。
fontはrepo/EBOOTへ同梱していない。最終配布fontは未決定。

## D-009: 相対ファイルは起動したEBOOTの隣へ解決する

2026-08-04。PSP Goで`ef0:/PSP/GAME/TH06PSP`が存在するだけで内蔵側を選ぶ方式は、
メモリースティック側から起動しても古い内蔵版のデータを読む。実機ログで、H:へ配達した版が
`ef0:/PSP/GAME/TH06PSP`を選び、同梱していない`bgm/th06_01.wav`を見失うことを確認した。

以後はXMBが渡す`argv[0]`から、実際に起動したEBOOTの親ディレクトリとdevice prefixを取る。
`ef0:`存在確認は`argv[0]`が無い特殊launcher向けfallbackだけにする。XMB上の識別も曖昧にしないため、
開発版の表示名を`Touhou 6 PSP GE`とし、PS3実配布版の桜柄ICON0をPSP用144x80へ縮小して使う。

## D-010: GEの初期バッファ配置は実機稼働済みの順序へ合わせる

2026-08-04。PPSSPPで通ったGE版を実機へ配達すると、`GE BACKEND OK`の直後、最初の
`SCREEN HANDED TO ENGINE`より前にcrashした。watchdog行がないためhangではない。旧実装はboot overlayを
残すため`draw=frame 1 / display=frame 0`としていたが、実機稼働済みPMDVISは一貫して
`draw=frame 0 / display=frame 1 / depth=frame 2`である。

boot overlayの保持より既知の実機経路を優先して同じ配置へ戻す。最初のpresentだけ
`GE FIRST SUBMIT` / `GE FIRST SYNC OK` / `GE FIRST SWAP OK`をboot logへ残す。
修正後の実機結果が出るまでは、原因確定ではなく差分を潰した段階として扱う。

## D-011: 初回stage camera方向をGameManagerへ同期する

2026-08-04。タイトル無操作デモは全asset/texture uploadとGameManager登録を完走した後、最初の
`GameManager::OnUpdate`からbackend `Clear`へ入る前にPSP-3000実機だけprocess-wide停止した。
sourceとPPSSPP raw-bitログを照合すると、`Stage::AddedCallback`は初期方向`(0,0,1)`をStage内にだけ置き、
`g_GameManager.stageCameraFacingDir`は`(0,0,0)`だった。calc優先度はGameManager=4、Stage=6なので、
初回だけGameManagerが先にゼロ方向を正規化する。

`src/`は変えず、PSPのGameManager登録wrapperで全成分ゼロの場合だけz=1へ同期する。修正後の実機は
最初のclear/swap/drawを通り、デモ完走後にタイトルへ復帰した。以降のscripted camera値は変更しない。

## D-012: PSP音声producerは壁時計ではなくPCMリング残量で駆動する

2026-08-04。旧実装は壁時計との差から毎回の生成量を決め、23ms以上遅れると音源cursorを進めずに
経過時間だけ捨てていた。出力側も1024 frames未満なら残存PCMを使わず1024 frames丸ごと無音にした。
PSP-3000実機で約192秒間に追加underrun 955 blocks（約22.2秒の無音）を記録し、細切れとBGM遅延の
双方を説明できた。

PSPだけを実リング残量駆動へ変更する。hardware blockは512 frames、通常watermarkと起動/再bufferの
prefillは3072 frames、producer優先度は0x18、outputは従来どおり0x10とする。変更後の実機は起動後
約194秒で追加underrun 10回。残件はすべて64KiB BGM readの68～228ms spikeと一致し、queue dropは0。
田中の聴感確認は「OK」。次段はread前だけwatermarkを上げるかI/O分離で扱い、通常SE latencyを
恒常的に増やさない。

## D-013: PSPの画面モードはFULL STRETCHと4:3 FITの二択

2026-08-04。PS3版と同様、ゲーム座標640x480は変えずbackend境界だけで出力layoutを切り替える。
既存optionの`FULLSCREEN`（cfg.windowed=0）を480x272全面引き伸ばし、`WINDOWED`（=1）を
362x272の4:3維持＋左右pillarとする。PC由来の表示名だけでは意味が曖昧なため、PSP wrapperから
`FULL STRETCH` / `4:3 FIT`をoptions行へ併記する。

640x480の静止surfaceは従来、論理1024幅を物理512幅へ縮める過程でcontentが320幅になり、そこから
362幅へ再拡大されていた。選択した表示寸法（480x272または362x272）へ一度だけbilinear縮小し、
texture scaleで元の論理UVへ対応させる。PPSSPP v1.20.4で全面表示、静止画、ゲーム描画の崩れなしを
確認済み。実機画質・性能は別途測定する。

デモ/リプレイの無入力中にLCDが消灯するため、既存の常駐診断threadから1秒ごとに
`scePowerTick(PSP_POWER_TICK_ALL)`を送る。描画・音声threadには処理を追加しない。

## D-014: ME音声はPSPPMD公開版を固定し、常にSCへ退避可能にする

2026-08-04。独自raw kcall方式は採用しない。公開`kan8223-dotcom/psp-pmdvis` commit
`18fb0b1`同梱のMECC libraryと`meLibDefaultInit()`をそのまま使い、MEにはPCM加算だけを渡す。
file I/O、BGM状態、`sceAudioOutputBlocking`はSCが所有する。起動時のSC/ME同一vector比較、jobの
8ms timeout、独立したME出力bufferを設け、失敗後はそのboot中ずっとSCで同じjobを再計算する。

PPSSPP v1.20.4はMECC初期化がerror returnせず停止するため、標準`ppsspp.ini` markerを検出した時は
MECCを呼ばずSC mixerへ直行する。markerの誤検出はoptional acceleratorを無効化するだけであり、
危険側には倒れない。ME local eDRAMの作業域はPSPPMDが実際に使う2KiBだけ、`[0x400,0xc00)`に固定する。
1024-frame jobは256-frameずつ処理し、終端をcompile-time assertionで固定する。

## D-015: GU_DIRECTの外部vertexはdraw commandより前に毎回writebackする

2026-08-04。SC側DRAW時間を減らすためvertex arenaのdcache反映をframe末尾へまとめた版は、
PSP-3000で音声に同期するようなgeometry破壊を起こした。ME OFFかつ旧音声mixerでも再現し、
最適化前EBOOTへ戻すと消えたため、原因はMEや音声ではなくGU_DIRECTのlive listへ未反映vertexの
commandを公開したこと。

以後、`sceGumDrawArray`より前にそのdrawの`vertices/required`をwritebackする。一括化するなら
GU_DIRECTのarenaとは別のdouble-buffered command/vertex所有設計が必要で、単に
`sceGuFinish()`直前へ移す変更は禁止する。

## D-016: 周期性能ログは通常版でMemory Stickへ書かない

2026-08-04。実機のboot logは各行ごとにFAT上のファイルをopen/write/closeする。5秒ごとの
`PERF/SC` 2行と`RUN/AUD/ME AUD` 3行は低頻度でも同時刻に集中し、同じMemory Stickから読む
BGM streamerとI/Oを競合する。実機値でもBGM read最大296ms、producer gap最大297ms、
underrun累計39を確認した。

通常版はこの5行をファイルへ書かず、counterの収集・窓resetだけ行う。専用計測時だけEBOOT隣へ
`TH06PSP_DIAG.ON`を置いて再度有効にする。boot、asset境界、異常、stage遷移など一度きりの記録は
維持する。BGM cacheは256KiBから384KiBへ増やしてread頻度を下げるが、同一mixer thread上の
同期readをなくすものではない。根治は非同期prefetchとして別に扱う。

## D-017: stage間のmain-RAM textureはfree/reallocせずblockを再利用する

2026-08-04。stage 1→2のPPSSPP replay再現で、stage 1 teardown後のstage 2 GUI atlas
`256x256x4 = 262,144 bytes`だけ`memalign(16)`に失敗した。eDRAMはframe/depth専用であり、
`TxxxxKB`とこのOOMはmain RAM textureの話である。各managerの`ReleaseAnm`は呼ばれていたが、
解放後に別順序・別寸法でstage 2 textureを確保したためheapが断片化していた。

PSP GE backendはDeleteTexture時にGE submit/syncを完了した後、allocationをbest-fit poolへ戻す。
次のSetTextureImageは十分な大きさの旧blockを優先再利用し、通常のstage遷移でnewlib heapへ返さない。
PPSSPP専用replay soakでstage 1→2→3→4の連続登録を確認した。通常版の詳細stage traceは
`TH06PSP_DIAG.ON`時だけとし、常用時は同期FAT書込みを増やさない。

## D-018: PSPのECLは64KiB固定arenaを全stageで再利用する

2026-08-04。FPU例外mask後のPSP-3000実機は一面終了の`STAGE END SIGNAL OLD1`と音声停止を通ったが、
二面登録前に停止した。stage teardownを区間計測した実機log
`backups/REAL_BAD_SOAK_FREEZE_20260804.LOG`の最終行は`unload_ecl BEGIN`で、同じ版をPPSSPPで走らせると
`unload_ecl END`以降を通る。`EclManager::Unload()`の本体はECL本体とsub pointer tableに対する二回の
`free()`だけであり、停止は旧stageのECL解放内に限定された。停止直前のmain thread stack freeは236KiBで、
二面assetの確保にも未到達なのでOOMとは扱わない。

PSP wrapperは`src/`を変えず、ECL本体を64KiB static arena、sub tableを4096 pointerのstatic tableへ置く。
PBGが返す一時bufferはarenaへcopyした直後、stage scriptを実行する前に解放し、stage終了時はengine-visible
pointerをnullにするだけでheapを触らない。実機上の製品版`TH06ST.DAT`を全件解析した結果、ECLは7本、
最大60,796 bytes、sub最大70であり、全timeline/sub offsetを含め上限内だった。上限外または壊れたECLは
load時に拒否し、arena overflowを許さない。

## D-019: 30fpsはVBlank量子化として、まず16.7ms期限超過を削る

2026-08-04。PSP-3000実機でstage 5/6や大量の弾を点へ変換するframeが、連続値ではなく約30fpsへ落ちる。
backendのpresentは毎frame `sceGuFinish()`→`sceGuSync()`→`sceDisplayWaitVblankStart()`→
`sceGuSwapBuffers()`を行う。従って60Hzのdeadlineを僅かに越すだけでも次のVBlankまで待ち、観測fpsは
60から30へ段階的に落ちる。これは30fps固定logicやME不足を示さない。

double bufferのままVBlank待ちだけを外すとtearingやdisplay中bufferへの描画を招くため採用しない。
まずSC側の頂点生成量とdraw負荷を減らし、16.7ms未満へ戻す。なおGE commandを発行できないMEは、
この段差そのものの解決には使わない。将来triple bufferへ移る場合は、frame/depthのeDRAM配置と
buffer所有を別設計として実機検証する。

## D-020: 密集bullet cancelでは個別得点popupだけを約48個へ制限する

2026-08-04。`BulletManager::DespawnBullets()`は最大640発を点itemへ変える処理とは別に、各弾の位置へ
個別の得点文字を生成する。各popupは60frame残り、4～5桁を個別spriteとして描くため、大量cancel後は
数千頂点のSC変換を1秒間追加し、D-019のVBlank期限を越えやすい。

PSP wrapperはcancel開始時のactive bullet数からstrideを決め、個別popupを最大約48個に間引く。
score加算、point item、bullet消去、画面中央のaggregate bonusはengine本体の処理をそのまま通す。
通常frameへ同期logは追加しない。

## D-021: staff roll surfaceは全高を同じ縮小率でcacheし、crop UVを適用する

2026-08-04。実機clear logで通常endingは640x480、staff rollは640x780のsurfaceだったが、旧cacheは
どちらも272px高へ潰していた。また`CopySurfaceRectToBackBuffer()`から渡るscroll cropを継承helperが
無視して全面UVを出すため、staff画像が縦縞状に重複して見えた。

PSP backendでは480→272と同じ縦倍率で780→442へ縮小し、512-row texture内に全scroll sourceを保持する。
PSP wrapperが`rectLeft/rectTop/rectWidth/rectHeight`からUVを作り、要求された640x480窓だけを描く。
通常640x480のtitle/ending stillは従来どおり272pxとなる。`src/`は変更しない。

## D-022: 公開版は利用者所有のPC版TH6フォルダを`th6`へ丸ごと置く

2026-08-04、田中の決定。GitHub/release/EBOOTには原作EXE、DAT、WAV、score、replayを含めない。
配布ZIPの`TH06PSP`を`PSP/GAME`へ置き、利用者が自分のPC版TH6インストールフォルダをフォルダごと
`PSP/GAME/TH06PSP/th6`へコピーする。公開物にはEBOOTと再配布可能な日本語fontを含める。

`psp/fileio.cpp`はEBOOTの親をwritable state root、`th6`をread-only original data rootとする。
日本語名の6 DATはUTF-8 pathを最初に試し、PSP FAT driverで開けない時はdirectory enumerationの返却名を
1.02 retail固有byte sizeと`PBG3` magicで対応させる。BGM 17 WAVはASCIIの`bgm/th06_XX.wav`から読む。
設定・score・新規replay・logは原作rootへ書かずEBOOT側へ分離する。旧flat配置は開発互換だけ残す。

6 DATまたは17 WAVが欠ける時はengineへ入れず、画面へ原作必須と配置先を表示する。公開READMEには
非公式、不完全preview、GE/GPU最適化未完、原作必須、replay非同梱、参照先へ問い合わせ禁止を明記する。
本体licenseはGensokyoClub/th06 portable branchに準じるGPLv3。MECCはm-c/dの現行repository名だけを使う。

## D-023: 公開名は`th06-psp-native`、描画とMEの境界を正確に表記する

2026-08-04、田中の決定。既存の類似名`th06_PSP`との混同を避けるため、公開repositoryと配布物は
`th06-psp-native`、XMB表示は`Touhou 6 PSP Native`とする。短いinstall folder名`TH06PSP`は維持する。

描画はD3D/OpenGLなどの汎用graphics APIや外部描画wrapperを介さず、PSP専用backendがPSPSDKの
libGU/libGUMを呼んでGE commandを生成する。これはGE native描画だが、GE registerの直接書込みではない。
音声はm-c/dの現行`psp-media-engine-custom-core`（MECC）を介してMedia EngineへPCM mixを渡し、
MEが利用できない場合やtimeout時はSC mixerへfallbackする。この二点をREADMEの特徴として明記する。

## D-024: PSP-1000では静止surface cacheをGameManager登録前に退役させる

2026-08-04。PSP-1000での停止報告に対し、既存logの`SYSMEM LEFT`は
`PSP_HEAP_SIZE_KB`がnewlib heapを予約した後のkernel partition残量であり、process heapの
空きではないと分離した。newlib `mallinfo()`によるheap snapshotと`std::new_handler`を追加し、
実際のC++ OOMは残量をlog/画面に残してXMBへ戻す。周期的なMemory Stick書き込みは追加しない。

GE backendの静止画resample cacheは1entry 512KiB、2entryをmenu/ending後も保持していた。
GameManagerの同期登録境界でGE listを完了させ、生存textureが借用中のentryは残し、
非借用entryだけを解放する。PPSSPPの32MB modelでstage 1登録時のtexture累計は
3872KiBから3360KiB、heap使用量は14602KiBから14090KiBへ低下した。通常demo登録では
1024KiBを解放し、その後title用cacheを再生成して次のdemo登録まで通過した。

PSP-1000実機は同じ版でtitle demoを完走し、titleへ復帰後も継続動作した。
Memory Stick lampと同期する短い停止は残っており、D-016で残件としたBGM同期readの
非同期prefetch化として別に扱う。

## D-025: 終了面のreplay録画領域は実使用範囲へ縮小する

2026-08-04。PSP-1000の通常playは3面終了後、4面ECLの60,796 bytes固定arenaへのcopyまで
成功したが、次のEffectManager登録中に停止した。同じ順序のPPSSPP 32MB soakは4面まで通るが、
そちらはreplay再生であり、通常playの録画用確保を通らない。

`StageReplayData`は1面ごとに432,000 bytesを確保し、入力変化だけを追記するにもかかわらず
replay保存まで最大領域を保持する。3面終了時は約1.24MiBが旧面の予備領域として残る。
製品replayの各面section実使用量は約5–11KiBで、`SaveReplay()`もbookmarkまでしか出力しない。

PSPの`ReplayManager::StopRecording()` wrapperは終端record追加後、完了面のbufferをbookmarkまで
`realloc`縮小し、`stageReplayData`、`replayInputs`、`replayInputStageBookmarks`を移動後の住所へ更新する。
現在面は従来どおり最大領域を持ち、縮小失敗時も元のpointer/容量で続行する。
`src/`、録画入力、disk format、scoreは変更しない。

## D-026: BGMのMemory Stick readはmixer threadから分離する

2026-08-04。PSP-1000ではBGM再生中のMemory Stick lamp点灯と短い停止が同期し、既存計測でも
192KiB blockのreadに最大296msを要した。旧実装は音声生成thread自身が次blockを同期readするため、
cache量を増やしてもread中はPCM供給が止まる。

PSP版は既存の192KiB×2 blockを増やさず、専用user threadが次blockを先読みする。mixerはFAT I/Oを
一切行わず、未着時はそのpassの不足部分を無音にして要求だけをqueueする。曲切替はgenerationで旧要求を
失効させ、MEへ渡したblockはjob完了まで上書き対象にしない。原作WAVはread-onlyのままである。

WAVから圧縮streamへの実行時変換はこの段階では採らず、まず責務分離で直す。PSPSDK標準MP3 APIが
decoderのみでも、既存homebrew encoderの移植は可能であり、技術的に不可能という判断ではない。
PSP-1000で非同期readとpriority調整後も帯域起因の停止が残る場合は、原作WAVを変更せず一時圧縮fileを
生成・破棄する案を再評価する。codec license、変換時間、ME側実装の実機確認はその時点の要件とする。

## D-028: BGM I/Oは通常mainより低priority、cache miss時だけ緊急昇格する

2026-08-05。六分割384KiB cacheは音切れを止めたが、64KiBを消費する約372msごとにMemory Stick lampと
frame落ちが同期した。実機記録の2,820.672秒/7,580 read = 372.1msはPCM消費周期371.5msと一致し、
周期diagnostic logは無効だった。原因はI/O worker `0x1c`がgame main `0x20`を定常的に横取りする設計である。

workerは曲開始時だけ`0x1c`で最初のread-aheadを作り、batch完了後は`0x21`へ下げる。mixerが実際に
cache missを観測した時だけ`0x1c`へ戻す。DAC `0x10`、mixer `0x18`、384KiB cache、原作WAV read-only、
miss時の無音fallbackは維持する。これにより通常補充はmainのGE/vblank待ち時間を使い、描画frameを
372ms周期で奪わない。

## D-029: 全面surface helperもPSPのGE直接uploadへwrapする

2026-08-05。ResultScreen登録後の実機OOMは921,600 bytes、呼出元は
`AnmManager::ExtractSurfacePixels()`だった。既存PSP wrapperは矩形surface helperだけを差し替えていたが、
全面helperから矩形helperへのcallは同じengine object内にありGNU ld `--wrap`の対象外だった。そのため
result.jpgを保持するRGB24 surfaceとは別に、初回drawで同じ640x480x3を一時複製していた。

全面helper自体をPSP wrapperへ追加し、矩形版と同じGE upload/cache pathへ渡す。`src/`は変更せず、
crop、FULL STRETCH/4:3、surface lifetime、draw直前dcache公開契約を維持する。PSP-3000とPSP-1000の
両方でContinue「No」後にOOMせず進むことを実機確認した。

## D-027: PSP描画backendはVFPU版libGUMを使う

2026-08-04。main threadは当初から`THREAD_ATTR_VFPU`を持つ一方、Makefileはscalar版
`libpspgum`へlinkしていた。PSPSDK同梱の`libpspgum_vfpu`と`libpspvfpu`へ切り替え、GUMの
matrix state/copy/multiplyをVFPUで実行する。`ScePspFMatrix4`とbackend一時matrixはいずれも
16-byte alignmentを満たす。

これはGE描画やSC側の全処理を自動でSIMD化するものではない。現在の`Draw()`はengineのinterleaved
vertexをGE layoutへC++ loopで詰め替えており、draw chain全体にもsprite生成が残る。従って
PSP-3000の16.7ms超過を減らす安全な一段目として採用し、改善量は同じ実機PERF窓で比較する。
