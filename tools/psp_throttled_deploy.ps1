[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z]$')]
    [string]$DriveLetter,

    [Parameter(Mandatory = $true)]
    [string]$Source,

    [Parameter(Mandatory = $true)]
    [string]$BackupDirectory,

    [ValidateRange(4, 1024)]
    [int]$ChunkKiB = 32,

    [ValidateRange(1, 5000)]
    [int]$DelayMs = 250,

    [string]$GameName = 'TH06PSP'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function ConvertTo-HexString {
    param([byte[]]$Bytes)

    return -join ($Bytes | ForEach-Object { $_.ToString('X2') })
}

function Copy-FileThrottled {
    param(
        [string]$SourcePath,
        [string]$DestinationPath,
        [int]$ChunkBytes,
        [int]$PauseMs,
        [string]$Label
    )

    $inputStream = [System.IO.FileStream]::new(
        $SourcePath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read,
        $ChunkBytes,
        [System.IO.FileOptions]::SequentialScan)
    $outputStream = $null
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $buffer = New-Object byte[] $ChunkBytes
    $total = 0L
    $nextReport = 262144L

    try {
        $length = $inputStream.Length
        $outputStream = [System.IO.FileStream]::new(
            $DestinationPath,
            [System.IO.FileMode]::Create,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None,
            $ChunkBytes,
            [System.IO.FileOptions]::WriteThrough)

        while (($read = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            [void]$sha256.TransformBlock($buffer, 0, $read, $buffer, 0)
            $outputStream.Write($buffer, 0, $read)
            $outputStream.Flush($true)
            $total += $read

            if ($total -ge $nextReport -or $total -eq $length) {
                Write-Host ("  {0}: {1:N0} / {2:N0} bytes" -f $Label, $total, $length)
                $nextReport += 262144L
            }
            if ($PauseMs -gt 0) {
                Start-Sleep -Milliseconds $PauseMs
            }
        }

        [void]$sha256.TransformFinalBlock($buffer, 0, 0)
        if ($total -ne $length) {
            throw "$Label length mismatch: copied $total of $length bytes"
        }
        return ConvertTo-HexString -Bytes $sha256.Hash
    }
    finally {
        if ($null -ne $outputStream) {
            $outputStream.Dispose()
        }
        $inputStream.Dispose()
        $sha256.Dispose()
    }
}

function Get-FileHashThrottled {
    param(
        [string]$Path,
        [int]$ChunkBytes,
        [int]$PauseMs,
        [string]$Label
    )

    $stream = [System.IO.FileStream]::new(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read,
        $ChunkBytes,
        [System.IO.FileOptions]::SequentialScan)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $buffer = New-Object byte[] $ChunkBytes
    $total = 0L
    $nextReport = 262144L

    try {
        $length = $stream.Length
        while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            [void]$sha256.TransformBlock($buffer, 0, $read, $buffer, 0)
            $total += $read

            if ($total -ge $nextReport -or $total -eq $length) {
                Write-Host ("  {0}: {1:N0} / {2:N0} bytes" -f $Label, $total, $length)
                $nextReport += 262144L
            }
            if ($PauseMs -gt 0) {
                Start-Sleep -Milliseconds $PauseMs
            }
        }

        [void]$sha256.TransformFinalBlock($buffer, 0, 0)
        return ConvertTo-HexString -Bytes $sha256.Hash
    }
    finally {
        $stream.Dispose()
        $sha256.Dispose()
    }
}

$DriveLetter = $DriveLetter.ToUpperInvariant()
$chunkBytes = $ChunkKiB * 1024
$sourcePath = (Resolve-Path -LiteralPath $Source).Path
$backupRoot = [System.IO.Path]::GetFullPath($BackupDirectory)
$root = "${DriveLetter}:\"

if (!(Test-Path -LiteralPath $root)) {
    throw "Drive $root is not connected"
}

$volume = Get-Volume -DriveLetter $DriveLetter[0]
if ($volume.FileSystem -notin @('FAT', 'FAT32')) {
    throw "Drive $root is $($volume.FileSystem), not PSP FAT/FAT32"
}
if (!(Test-Path -LiteralPath (Join-Path $root 'PSP'))) {
    throw "Drive $root has no PSP directory"
}

$gameDirectory = Join-Path $root "PSP\GAME\$GameName"
$livePath = Join-Path $gameDirectory 'EBOOT.PBP'
$stagePath = Join-Path $gameDirectory 'EBOOT.NEW'
if (!(Test-Path -LiteralPath $livePath)) {
    throw "Live EBOOT is missing: $livePath"
}

New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
$timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$rollbackPath = Join-Path $backupRoot "EBOOT_${DriveLetter}_before_throttled_${timestamp}.PBP"
$expectedHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
$expectedLength = (Get-Item -LiteralPath $sourcePath).Length

Write-Host "Target: $root ($($volume.FileSystem), $($volume.HealthStatus))"
Write-Host "Payload: $expectedLength bytes / SHA256 $expectedHash"
Write-Host "Throttle: $ChunkKiB KiB then ${DelayMs}ms pause (about $([math]::Round($ChunkKiB * 1000.0 / $DelayMs)) KiB/s before flush overhead)"

# Preserve the currently bootable file on the host before any Memory Stick
# write. The copy itself is throttled, and the host copy is hashed separately.
$oldHash = Copy-FileThrottled -SourcePath $livePath -DestinationPath $rollbackPath `
    -ChunkBytes $chunkBytes -PauseMs $DelayMs -Label 'rollback read'
$rollbackHash = (Get-FileHash -LiteralPath $rollbackPath -Algorithm SHA256).Hash
if ($rollbackHash -ne $oldHash) {
    throw "Host rollback hash mismatch: source $oldHash, backup $rollbackHash"
}
Write-Host "Rollback OK: $rollbackPath / SHA256 $rollbackHash"

# A complete stage left by an interrupted connection is useful only after a
# full read-back hash. Reusing it avoids another Memory Stick write entirely.
$stageIsVerified = $false
$stagedHash = $null
if (Test-Path -LiteralPath $stagePath) {
    $existingStageLength = (Get-Item -LiteralPath $stagePath).Length
    if ($existingStageLength -eq $expectedLength) {
        Write-Host 'Existing full-size EBOOT.NEW found; verifying slowly before any rewrite'
        $stagedHash = Get-FileHashThrottled -Path $stagePath -ChunkBytes $chunkBytes `
            -PauseMs $DelayMs -Label 'existing stage verify'
        $stageIsVerified = ($stagedHash -eq $expectedHash)
        if ($stageIsVerified) {
            Write-Host 'Existing EBOOT.NEW is exact; stage rewrite skipped'
        }
    }
}

if (!$stageIsVerified) {
    # EBOOT.NEW is deliberately expendable. A disconnect here never streams
    # data over the live filename. FileMode.Create replaces any partial or
    # mismatched prior stage.
    $copiedSourceHash = Copy-FileThrottled -SourcePath $sourcePath -DestinationPath $stagePath `
        -ChunkBytes $chunkBytes -PauseMs $DelayMs -Label 'stage write'
    if ($copiedSourceHash -ne $expectedHash) {
        throw "Source changed during staged copy: got $copiedSourceHash"
    }

    $stagedLength = (Get-Item -LiteralPath $stagePath).Length
    if ($stagedLength -ne $expectedLength) {
        throw "Staged length mismatch: got $stagedLength, expected $expectedLength"
    }
    $stagedHash = Get-FileHashThrottled -Path $stagePath -ChunkBytes $chunkBytes `
        -PauseMs $DelayMs -Label 'stage verify'
    if ($stagedHash -ne $expectedHash) {
        throw "Staged hash mismatch: got $stagedHash, expected $expectedHash"
    }
}
Write-Host "Stage OK: $stagePath / SHA256 $stagedHash"

# This same-directory rename is the first operation that changes the live
# filename. The verified host rollback remains available if the device resets.
Move-Item -LiteralPath $stagePath -Destination $livePath -Force
Start-Sleep -Seconds 1

$finalHash = Get-FileHashThrottled -Path $livePath -ChunkBytes $chunkBytes `
    -PauseMs $DelayMs -Label 'final verify'
if ($finalHash -ne $expectedHash) {
    throw "Final hash mismatch: got $finalHash, expected $expectedHash; use host rollback"
}

Write-Host "DEPLOY OK: $livePath / SHA256 $finalHash"
