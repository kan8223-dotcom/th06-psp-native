#!/bin/bash
# psp_preflight — PSP版 憲兵 v1.0 (2026-07-04 Fable期遺産)
# ビルド前・デプロイ前に既知の地雷を全数検査する。PS3版 ps3_preflight.sh と同思想。
#
# 根拠ソース (推測で書いた項目はその旨明記):
#   [KP]   KNOWLEDGE_PSP.md (踏んだ地雷 全記録)
#   [ME]   psp-me-knowledge.md (icache/dcache/eDRAM/MECC/sh[]マップ/地雷早見表)
#   [HW]   psp-hardware-knowledge (メモリ階層/scratchpad)
#   [大]   PSPPMDVIS大百科.md (I/O競合・/mnt直接参照禁止 前科12回)
#   [FB]   feedback_psp_import_stubs (IMPORT_START 0x00090000必須 前科3回)
#   [実]   pmd_psp 実コード (動作実績パターン: main.c / me-lib.h / Makefile)
#   [HW §] psp-hardware-knowledge 新版「実機罠集」(サーバ側スキル。ローカルコピーはstaleで
#          この節を含まない — 2026-07-04確認。psp-bsky実測 PSP Go 64MB §817)
#
# 使い方: プロジェクトルートで ./psp_preflight.sh          … ビルド前検査
#                             ./psp_preflight.sh --post    … ビルド後・デプロイ前検査
set -u
MODE="${1:-pre}"
FAIL=0; WARN=0
ok()   { echo "  [OK]   $1"; }
ng()   { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
warn() { echo "  [WARN] $1"; WARN=$((WARN+1)); }
note() { echo "  [NOTE] $1"; }   # 集計外。人間向けメモ

SRC_C=$(ls *.c *.cpp 2>/dev/null || true)

echo "=== psp_preflight (${MODE}) <●><●> 憲兵は全てを見ている ==="

# ---------- Makefile / ビルド系 ----------
if [ -f Makefile ]; then
  grep -q 'build\.mak' Makefile && ok "PSPSDK build.mak include" \
    || warn "Makefileがbuild.makをincludeしていない (pspsdk標準構成でない)"

  # EBOOT.PBP生成経路: EXTRA_TARGETS方式[実 pmd_psp] or pack-pbp直叩き[実 moonlight]
  if grep -q "EXTRA_TARGETS.*EBOOT.PBP" Makefile || grep -q "pack-pbp" Makefile; then
    ok "EBOOT.PBP生成経路あり (EXTRA_TARGETS or pack-pbp) [実]"
  else
    warn "MakefileにEBOOT.PBP生成経路が見えない"
  fi

  grep -q "PSP_FW_VERSION" Makefile && ok "PSP_FW_VERSION定義あり [実: 660]" \
    || note "PSP_FW_VERSION未定義 (pmd_psp/moonlightは660)"

  # -lme-core を使うなら libme-core.a が実在すること [実 pmd_psp Makefile]
  if grep -q -- "-lme-core" Makefile; then
    LIBDIRS=$(grep -E "^LIBDIR" Makefile | sed 's/.*=//' )
    FOUND=0
    for d in $LIBDIRS; do
      [ -f "$d/libme-core.a" ] && FOUND=1
    done
    find . -name "libme-core.a" -not -path "./backups/*" 2>/dev/null | grep -q . && FOUND=1
    [ "$FOUND" -eq 1 ] && ok "-lme-core: libme-core.a 実在" \
      || ng "-lme-coreがLIBSにあるが libme-core.a が見つからない (MECCビルド忘れ)"
  fi
else
  warn "Makefileが無い"
fi

# ---------- モジュールマクロ [実 pmd_psp main.c:113-115 / moonlight main.c:37-39] ----------
if [ -n "$SRC_C" ]; then
  grep -q "PSP_MODULE_INFO" $SRC_C 2>/dev/null && ok "PSP_MODULE_INFO あり" \
    || ng "PSP_MODULE_INFOがどのソースにも無い (EBOOTが起動しない)"
  grep -q "PSP_MAIN_THREAD_ATTR" $SRC_C 2>/dev/null && ok "PSP_MAIN_THREAD_ATTR あり" \
    || note "PSP_MAIN_THREAD_ATTR無し (実績値: THREAD_ATTR_USER | THREAD_ATTR_VFPU)"
fi

# ---------- importスタブ .S [FB: 前科3回] ----------
for s in $(ls *.S 2>/dev/null || true); do
  if grep -q "IMPORT_START" "$s"; then
    if grep -E 'IMPORT_START.*0x40010000' "$s" >/dev/null; then
      ng "${s}: IMPORT_STARTフラグ 0x40010000 → 起動失敗0x8002013C [FB 前科3回]"
    else
      ok "${s}: 0x40010000フラグなし"
    fi
    # sceMpeg / sceMpegbase は 0x00090000 (WEAK) 必須 [FB]
    BAD=$(grep -E 'IMPORT_START.*"sceMpeg(base)?"' "$s" | grep -v "0x00090000" || true)
    if [ -n "$BAD" ]; then
      ng "${s}: sceMpeg/sceMpegbase のIMPORT_STARTが0x00090000でない [FB 前科3回]"
    fi
  fi
done

# ---------- MECC / ME 系 ----------
MECC_USED=""
if [ -n "$SRC_C" ] && grep -q "me-core.h\|meLibDefaultInit" $SRC_C 2>/dev/null; then
  MECC_USED=1
  ME_FILE=$(grep -l "meLibDefaultInit" $SRC_C 2>/dev/null | head -1)

  # 共有バッファ宣言: aligned(64) + section(".uncached") [実 me-lib.h:22 / pmd_psp main.c:1264]
  if grep -E 'aligned\(64\).*section\("\.uncached"\)|section\("\.uncached"\).*aligned\(64\)' $SRC_C >/dev/null 2>&1; then
    ok "SC↔ME共有バッファ: aligned(64)+section(\".uncached\") [実]"
  else
    ng "共有バッファに aligned(64)+section(\".uncached\") 宣言が無い [実 me-lib.h必須パターン]"
  fi

  # uncachedアクセス: 0x40000000 OR [実 pmd_psp main.c:3259,1279]
  grep -q "0x40000000 *|" $SRC_C 2>/dev/null && ok "uncachedアドレス (0x40000000 OR) 使用 [実]" \
    || ng "0x40000000 ORによるuncachedアクセスが見えない (cached直読みはSC↔ME通信破綻) [ME]"

  if [ -n "$ME_FILE" ]; then
    # 初期化順序: memset+DcacheWritebackInvalidateAll → meLibDefaultInit [実 main.c:3260-3264]
    L_INIT=$(grep -n "meLibDefaultInit" "$ME_FILE" | head -1 | cut -d: -f1)
    L_WB=$(grep -n "sceKernelDcacheWritebackInvalidateAll" "$ME_FILE" | head -1 | cut -d: -f1)
    if [ -n "$L_WB" ] && [ -n "$L_INIT" ] && [ "$L_WB" -lt "$L_INIT" ]; then
      ok "MECC初期化順序: DcacheWritebackInvalidateAll → meLibDefaultInit [実]"
    else
      warn "meLibDefaultInit前のsceKernelDcacheWritebackInvalidateAllが確認できない [実 main.c:3261]"
    fi
    # 戻り値チェック (table_id>=0でフォールバック) [実 main.c:3264-3271]
    grep -E "= *meLibDefaultInit" "$ME_FILE" >/dev/null && ok "meLibDefaultInit戻り値を受けている [実]" \
      || warn "meLibDefaultInitの戻り値未使用 (ME失敗時のSCフォールバック不能) [実]"
  fi

  # meLibOnProcess (ME側エントリ) の存在 [実 main.c:1278]
  if grep -q "void meLibOnProcess" $SRC_C 2>/dev/null; then
    ok "meLibOnProcess 定義あり [実]"
    OP_FILE=$(grep -l "void meLibOnProcess" $SRC_C | head -1)
    OP_BODY=$(awk '/void meLibOnProcess/{f=1} f{print} f&&/^\}/{exit}' "$OP_FILE")
    # scratchpad(0x00010000)をME側で使用 → クロスバスで逆に遅い [HW/ME 前科1回: 24ms→39.6ms]
    echo "$OP_BODY" | grep -q "0x00010000" \
      && ng "meLibOnProcess内でscratchpad(0x00010000)使用 → MEからは遅い [ME 前科1回]" \
      || ok "ME側でscratchpad不使用 [ME]"
    # ME上のdouble演算 → ソフトエミュ50-100倍遅 [ME §4]
    echo "$OP_BODY" | grep -qE '\bdouble\b' \
      && warn "meLibOnProcess内にdouble → MEはCOP1 float専用、50-100倍遅 [ME §4]" \
      || ok "ME側にdouble演算なし [ME]"
    # 終了経路: ME_CMD_STOP → meLibHalt [実 main.c:1361-1365]
    echo "$OP_BODY" | grep -q "meLibHalt" \
      && ok "ME終了経路 (meLibHalt) あり [実]" \
      || note "meLibHaltによる終了経路が無い (STOPコマンド未実装?)"
  else
    ng "MECC使用中なのに meLibOnProcess が未定義 (ME側エントリ必須) [実]"
  fi

  # sh[]共有メモリマップ領域侵犯 [ME §7 前科1回: MEクラッシュ+爆音]
  # sh[8..30]=vis(23), sh[40..56]=制御, sh[60..103]=FMP(44)
  V=$(grep -h "#define VIS_SIZE" $SRC_C 2>/dev/null | grep -oE '[0-9]+' | head -1 || true)
  [ -n "${V:-}" ] && { [ "$V" -le 23 ] && ok "VIS_SIZE=${V} ≤ 23 (sh[8..30]内) [ME §7]" \
    || ng "VIS_SIZE=${V} > 23 → sh[40+]制御領域を侵犯 [ME §7 前科1回]"; }
  F=$(grep -h "#define FMP_SIZE" $SRC_C 2>/dev/null | grep -oE '[0-9]+' | head -1 || true)
  [ -n "${F:-}" ] && { [ "$F" -le 44 ] && ok "FMP_SIZE=${F} ≤ 44 (sh[60..103]内) [ME §7]" \
    || ng "FMP_SIZE=${F} > 44 → sh[]末尾超過 [ME §7]"; }

  # PSP_HEAP_SIZE_KB: ME使用時は負値でMEスタック確保 [実 main.c:115]
  if grep -qE "PSP_HEAP_SIZE_KB\(-[0-9]+\)" $SRC_C 2>/dev/null; then
    ok "PSP_HEAP_SIZE_KB 負値指定 (MEスタック確保) [実 -2048]"
  else
    warn "MECC使用中だがPSP_HEAP_SIZE_KBが負値でない (実績: -2048) [実]"
  fi
fi

# ---------- embedded_kcall / kcall.prx サイズ [KP/ME: 2,198B=正解、1,740B=壊れ] ----------
if [ -f kcall.prx ]; then
  KSZ=$(stat -c %s kcall.prx)
  if [ "$KSZ" -eq 2198 ]; then ok "kcall.prx = 2,198B (v2.0.1正解版) [ME §6]"
  else ng "kcall.prx = ${KSZ}B ≠ 2,198B (旧版1,740Bは壊れている) [ME §6 前科1回]"; fi
fi

# ---------- ymfm聖域 (検出→警告のみ。自動修正禁止) [ME §5 前科1回: 14時間浪費] ----------
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  YDIFF=$( { git status --porcelain 2>/dev/null; git diff --name-only HEAD 2>/dev/null; } \
           | grep -iE "ymfm|fmgen" | sort -u || true)
  if [ -n "$YDIFF" ]; then
    warn "ymfm聖域に差分検出 (generate()への分岐追加はicache 16KB汚染) [ME §5 前科1回]:"
    echo "$YDIFF" | sed 's/^/         /'
  else
    ok "ymfm聖域: 差分なし [ME §5]"
  fi
else
  note "gitリポジトリでないためymfm聖域diff検査スキップ"
fi

# ---------- リングバッファ既定値 [KP 確定パラメータ / 実 main.c:181-182,710] ----------
if [ -n "$SRC_C" ] && grep -q "RING_BLOCKS_64MB" $SRC_C 2>/dev/null; then
  R64=$(grep -h "#define RING_BLOCKS_64MB" $SRC_C | head -1 | awk '{print $3}')
  R32=$(grep -h "#define RING_BLOCKS_32MB" $SRC_C | head -1 | awk '{print $3}' || true)
  [ "${R64:-0}" -eq 256 ] && ok "RING_BLOCKS_64MB=256 (v1.1-stable-ring256) [KP]" \
    || warn "RING_BLOCKS_64MB=${R64} ≠ 256 (安定実績値から逸脱) [KP]"
  [ -n "${R32:-}" ] && { [ "$R32" -eq 128 ] && ok "RING_BLOCKS_32MB=128 [実]" \
    || warn "RING_BLOCKS_32MB=${R32} ≠ 128 (実績値から逸脱) [実]"; }
  grep -q "ring_blocks - 1" $SRC_C && ok "prefill = ring_blocks - 1 パターン [実 main.c:710]" \
    || warn "prefill_wait = ring_blocks - 1 の実績パターンが見えない [KP]"
fi

# ---------- 実機/PPSSPP差分系 [HW新版 実機罠集 §710-880 (サーバ側スキル、ローカル未同期)] ----------
if [ -n "$SRC_C" ]; then
  JHIT=$(grep -hnE "sceJpeg[A-Za-z]*\(" $SRC_C 2>/dev/null || true)
  if [ -n "$JHIT" ]; then
    if echo "$JHIT" | grep -qE '\b(48[1-9]|4[9][0-9]|[5-9][0-9]{2}|[0-9]{4,})\b'; then
      ng "sceJpegDecodeMJpegで480超サイズ疑い → 実用上限はPSP画面サイズ、超えると0x80650020 [HW §758]"
    else
      note "sceJpeg使用あり。480x272以下か確認 (1000px級で0x80650020 UNSUPPORT_IMAGE_SIZE) [HW §758]"
    fi
  fi
  grep -qE "GU_SPRITES|sceGuDrawArray" $SRC_C 2>/dev/null \
    && note "sceGuDrawArray矩形使用: 非テクスチャ矩形はPPSSPPで完全drop (実機はOK、エミュ差分)。PPSSPP検証時に消えても慌てるな [HW §730]"
  grep -qE '\bfflush *\(' $SRC_C 2>/dev/null \
    && warn "fflush使用: FAT32ではfflush単体でdirectory entry未更新→強制終了でtail消失。fclose+再fopenで確定させろ [HW §787]"
  grep -q "sceUtilityHtmlViewer" $SRC_C 2>/dev/null \
    && note "HtmlViewer使用: 起動時8-10MBの連続Vpl要求。複合homebrewで0x80020190 (NO_MEMORY) [HW §813]"

  # USBカーネルモジュール手動ロード → sceIo競合起動クラッシュ [KP 前科1回]
  grep -hE "sceKernelLoadModule.*(usb|semawm)" $SRC_C 2>/dev/null | grep -q . \
    && ng "USB系カーネルモジュール手動ロード → sceIo競合で起動クラッシュ [KP 前科1回]" || true

  # ms0: 複数fd同時オープン [大: 前科1回 ウォッチドッグ強制シャットダウン]
  NFD=$(grep -h 'sceIoOpen *( *"ms0:' $SRC_C 2>/dev/null | wc -l)
  [ "$NFD" -ge 2 ] && note "ms0:へのsceIoOpenが${NFD}箇所: 2fd同時オープン禁止 (suspend/resume方式を使え) [大 前科1回]"
fi

# ---------- /mnt/e /mnt/g 直接参照禁止 [大: 前科12回 (デプロイ)] ----------
# デプロイ/ビルド系はFAIL、その他スクリプト(ログ取得等)は読み取りでも不安定なのでWARN
MNT_BAD=0
for f in $(ls *.sh Makefile 2>/dev/null || true); do
  if grep -qE "/mnt/[eg]/" "$f" 2>/dev/null; then
    MNT_BAD=1
    case "$f" in
      deploy*|build*|Makefile|psp_go*)
        ng "${f}: /mnt/e|/mnt/g 直接参照 → PowerShell UNC方式を使え [大 前科12回]" ;;
      *)
        warn "${f}: /mnt/e|/mnt/g 直接参照 (WSL drvfsは不安定、UNC方式推奨) [大]" ;;
    esac
  fi
done
[ "$MNT_BAD" -eq 0 ] && ok "/mnt/e|/mnt/g 直接参照なし [大]"

# ---------- --post: ビルド後・デプロイ前 ----------
if [ "$MODE" = "--post" ]; then
  if [ -f EBOOT.PBP ]; then
    ok "EBOOT.PBP存在 ($(stat -c %s EBOOT.PBP) bytes)"
    STALE=$(find . -maxdepth 2 \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.S" \) \
            -newer EBOOT.PBP -not -path "./backups/*" 2>/dev/null | head -5)
    if [ -n "$STALE" ]; then
      ng "EBOOT.PBPよりソースが新しい → makeし直せ: $(echo $STALE | tr '\n' ' ')"
    else
      ok "EBOOT.PBPは全ソースより新しい"
    fi
  else
    ng "EBOOT.PBPが無い。デプロイ不能"
  fi
fi

echo "=============================================="
if [ "$FAIL" -gt 0 ]; then
  echo "判定: 違反 ${FAIL}件 / 警告 ${WARN}件 — デプロイ禁止。<●><●> 直してから来い。"
  exit 1
else
  echo "判定: 違反0件 / 警告 ${WARN}件 — 通行を許可する。"
  exit 0
fi
