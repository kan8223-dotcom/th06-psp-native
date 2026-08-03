#!/bin/bash
# psp_go — 検査→ビルド→検査→一発デプロイ (2026-07-04 Fable期遺産、PS3版ps3_go.shと同思想)
# デプロイはPowerShell UNC方式 (根拠: PSPPMDVIS大百科「/mnt/e /mnt/g直接参照全面禁止 前科12回」、
# pmd_psp/deploy.sh 実績パターン)。
#
# 使い方: プロジェクトルートで ./psp_go.sh            … PSP-1000 (E:) へ一発 [開発機 PRO-B10]
#                             ./psp_go.sh --go       … PSP Go (G:) へ (ARK-4警告あり)
#                             ./psp_go.sh --all      … E: と G: 両方
#                             ./psp_go.sh --clean    … クリーンビルドから (E:)
set -eu

# ===== CONFIG (2026-07-04 田中確認済み) =====================================
# 実機転送: USB PowerShellコピー (確認済み)。PPSSPPデプロイ: 不要 (確認済み)。
# ドライブ: E:=PSP-1000 PRO-B10 (開発機) / G:=PSP Go ARK-4 (確認済み、2台のみ登録)
# ⚠ レターは接続構成で変わる (前科: E:にSSDが来てPSPがG:に)。デプロイ前に実確認
GAME_NAME=""            # PSP/GAME/<GAME_NAME>/ に配置。空ならディレクトリ名を大文字化
DRIVE_DEV="E"           # 開発機 (PSP-1000 PRO-B10)
DRIVE_GO="G"            # PSP Go (ARK-4 — コード追加起動コケ前科2回、検証はE:が先)
BACKUP_KEEP=10          # EBOOTバックアップ世代数 (pmd_psp/deploy.sh実績)
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

if [ -z "$GAME_NAME" ]; then
  GAME_NAME=$(basename "$SCRIPT_DIR" | tr '[:lower:]' '[:upper:]' | tr -cd 'A-Z0-9_' | cut -c1-16)
fi

MODE="${1:-dev}"
DRIVES=()
case "$MODE" in
  --go)  DRIVES=("$DRIVE_GO") ;;
  --all) DRIVES=("$DRIVE_DEV" "$DRIVE_GO") ;;
  *)     DRIVES=("$DRIVE_DEV") ;;
esac

JOBS=$(nproc)

echo "◆ Phase 1/4: ビルド前検査"
./psp_preflight.sh || { echo "憲兵に止められた。修正しろ。"; exit 1; }

echo "◆ Phase 2/4: ビルド (-j${JOBS})"
[ "$MODE" = "--clean" ] && make clean
# 'make all' 明示: build-and-deployが.DEFAULT_GOALのプロジェクトで二重デプロイを防ぐ
make -j"${JOBS}" all 2>&1 | tee /tmp/psp_go_build.log
grep -qE "error:|Error 1" /tmp/psp_go_build.log && { echo "[FAIL] ビルドエラー。ログ: /tmp/psp_go_build.log"; exit 1; }

echo "◆ Phase 3/4: ビルド後検査 (デプロイ資格審査)"
./psp_preflight.sh --post || { echo "デプロイ資格なし。"; exit 1; }

# ARK-4警告 (根拠: KNOWLEDGE_PSP「ARK-4 CXPL .text問題 前科2回 / sceMpegハング」)
for d in "${DRIVES[@]}"; do
  if [ "$d" = "$DRIVE_GO" ]; then
    echo ""
    echo "⚠ PSP Go (ARK-4) デプロイ: コード追加は起動コケ前科2回。E:(PRO-B10)で検証済みか？"
  fi
done

echo "◆ Phase 4/4: デプロイ → PSP/GAME/${GAME_NAME}/ (${DRIVES[*]}:)"

# バックアップ (pmd_psp/deploy.sh実績: タイムスタンプ付き、N世代保持)
mkdir -p backups
TS=$(date +%Y%m%d_%H%M%S)
cp EBOOT.PBP "backups/EBOOT_${TS}.PBP"
ls -t backups/EBOOT_*.PBP 2>/dev/null | tail -n +$((BACKUP_KEEP+1)) | xargs -r rm -f

# WSL→UNCパス変換 (直接 /mnt/e 参照は禁止 [大 前科12回])
WSL_DISTRO="${WSL_DISTRO_NAME:-Ubuntu-24.04}"
EBOOT_UNC="\\\\wsl.localhost\\${WSL_DISTRO}${SCRIPT_DIR}/EBOOT.PBP"
DRIVE_LIST=$(printf "'%s'," "${DRIVES[@]}" | sed "s/,$//")

/mnt/c/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe -Command "
\$src = '${EBOOT_UNC}'
\$ok = 0
foreach (\$d in @(${DRIVE_LIST})) {
    for (\$i=0; \$i -lt 5; \$i++) {
        if (Test-Path \"\$(\$d):\\\") { break }
        Start-Sleep -Seconds 1
    }
    if (Test-Path \"\$(\$d):\\\") {
        \$dir = \"\$(\$d):\\PSP\\GAME\\${GAME_NAME}\"
        if (!(Test-Path \$dir)) { New-Item -ItemType Directory -Path \$dir -Force | Out-Null }
        Copy-Item \$src \"\$dir\\EBOOT.PBP\" -Force
        Write-Host \"Deploy OK: \$(\$d): -> PSP\GAME\${GAME_NAME}\"
        \$ok++
    } else {
        Write-Host \"Drive \$(\$d): not found (USB接続確認)\"
    }
}
if (\$ok -eq 0) { exit 1 }
" || { echo "[FAIL] デプロイ失敗: ドライブ未接続"; exit 1; }

echo ""
echo "=== 完了。PSPで PSP/GAME/${GAME_NAME} を起動しろ。 ==="
