#!/bin/bash
# psp_go — 検査→ビルド→検査→一発デプロイ
# デプロイはWindows PowerShellから32KiB単位で低速stage/verifyする。
# /mnt/e /mnt/gを直接書かず、切断時もlive EBOOTへstreamしない。
#
# 使い方: ./psp_go.sh --drive H          … Windowsで確認したPSPへ配達
#         ./psp_go.sh --drive H --clean  … clean buildしてから配達
#         ./psp_go.sh --help             … 何も変更せずusage表示
set -eu

# ===== CONFIG ===============================================================
# 実機転送: USB PowerShellコピー (確認済み)。PPSSPPデプロイ: 不要 (確認済み)。
# ⚠ レターは接続構成で変わる (前科: E:にSSDが来てPSPがG:に)。デプロイ前に実確認
# fileio.cpp と診断仕様がこのディレクトリ名を基準にする。repo名からの自動生成だと
# TH06_PSP になり、実行時の TH06PSP と食い違うため固定する。
GAME_NAME="TH06PSP"     # PSP/GAME/TH06PSP/
BACKUP_KEEP=10          # EBOOTバックアップ世代数 (pmd_psp/deploy.sh実績)
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

if [ -z "$GAME_NAME" ]; then
  GAME_NAME=$(basename "$SCRIPT_DIR" | tr '[:lower:]' '[:upper:]' | tr -cd 'A-Z0-9_' | cut -c1-16)
fi

usage() {
  echo "usage: ./psp_go.sh --drive <letter> [--clean]"
  echo "       ./psp_go.sh --help"
}

if [ "${1:-}" = "--help" ]; then
  usage
  exit 0
fi
if [ "${1:-}" != "--drive" ] || [ -z "${2:-}" ]; then
  echo "[FAIL] ドライブレターは機体識別ではない。Windowsで確認して --drive H の形で明示しろ。"
  usage
  exit 2
fi

DRIVE=$(printf '%s' "$2" | tr '[:lower:]' '[:upper:]')
case "$DRIVE" in
  [A-Z]) ;;
  *) echo "[FAIL] 不正なドライブレター: $2"; exit 2 ;;
esac
DRIVES=("$DRIVE")
CLEAN=0
if [ "${3:-}" = "--clean" ]; then
  CLEAN=1
elif [ -n "${3:-}" ]; then
  echo "[FAIL] 不明な引数: $3"
  usage
  exit 2
fi

JOBS=$(nproc)

echo "◆ Phase 1/4: ビルド前検査"
./psp_preflight.sh || { echo "憲兵に止められた。修正しろ。"; exit 1; }

echo "◆ Phase 2/4: ビルド (-j${JOBS})"
[ "$CLEAN" -eq 1 ] && make clean
# 'make all' 明示: build-and-deployが.DEFAULT_GOALのプロジェクトで二重デプロイを防ぐ
make -j"${JOBS}" all 2>&1 | tee /tmp/psp_go_build.log
grep -qE "error:|Error 1" /tmp/psp_go_build.log && { echo "[FAIL] ビルドエラー。ログ: /tmp/psp_go_build.log"; exit 1; }

echo "◆ Phase 3/4: ビルド後検査 (デプロイ資格審査)"
./psp_preflight.sh --post || { echo "デプロイ資格なし。"; exit 1; }

echo "◆ Phase 4/4: デプロイ → PSP/GAME/${GAME_NAME}/ (${DRIVES[*]}:)"

# バックアップ (pmd_psp/deploy.sh実績: タイムスタンプ付き、N世代保持)
mkdir -p backups
TS=$(date +%Y%m%d_%H%M%S)
cp EBOOT.PBP "backups/EBOOT_${TS}.PBP"
ls -t backups/EBOOT_*.PBP 2>/dev/null | tail -n +$((BACKUP_KEEP+1)) | xargs -r rm -f

# PowerShell側はWindowsネイティブパスだけを使う。WSL共有UNC経由だと
# PSPからのロールバック退避にAccess Deniedが起きえる。
EBOOT_WIN=$(wslpath -w "${SCRIPT_DIR}/EBOOT.PBP")
BACKUP_WIN=$(wslpath -w "${SCRIPT_DIR}/backups")
DEPLOY_SCRIPT_WIN=$(wslpath -w "${SCRIPT_DIR}/tools/psp_throttled_deploy.ps1")

# 配達物の同一性は「コピーした」ではなく実機側のハッシュで示す。
# 前科: 型エラーで新EBOOTが生成されていないのに古い版を配達した (2026-08-04)。
LOCAL_SHA=$(sha256sum EBOOT.PBP | cut -d' ' -f1 | tr 'a-f' 'A-F')
LOCAL_SIZE=$(stat -c %s EBOOT.PBP)
echo "  ローカル EBOOT.PBP: ${LOCAL_SIZE} bytes / SHA256 ${LOCAL_SHA}"

/mnt/c/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe \
  -NoProfile -ExecutionPolicy Bypass -File "${DEPLOY_SCRIPT_WIN}" \
  -DriveLetter "${DRIVE}" -Source "${EBOOT_WIN}" -BackupDirectory "${BACKUP_WIN}" \
  -GameName "${GAME_NAME}" -ChunkKiB 32 -DelayMs 250 \
  || { echo "[FAIL] 低速stage配備失敗: live EBOOTはhost rollbackか旧版のまま"; exit 1; }

echo ""
echo "=== 完了。PSPで PSP/GAME/${GAME_NAME} を起動しろ。 ==="
echo "    起動後に読むもの: <ドライブ>:\\TH06PSP_BOOT.LOG (前回分は TH06PSP_BOOT_PREV.LOG)"
