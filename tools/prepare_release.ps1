# Prepare a complete AIagent98 release commit.
# Builds all binaries with OpenWatcom, refreshes dist/ and the sample config,
# so `git add -A && git commit && git push` ships source + runnable files together.
#
# Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File tools\prepare_release.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

Write-Output "== Building all targets (OpenWatcom) =="
& cmd.exe /c "build_ow.cmd"
if ($LASTEXITCODE -ne 0) { throw "build_ow.cmd failed: $LASTEXITCODE" }
& cmd.exe /c "build_ow.cmd probe" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "build_ow.cmd probe failed: $LASTEXITCODE" }

$ow = Join-Path $root 'build\ow'
$dist = Join-Path $root 'dist'
Write-Output "== Refreshing dist/ =="
foreach ($p in @(
    @('agent98.exe', 'AGENT98.EXE'),
    @('a98.exe',      'A98.EXE'),
    @('tlsprobe.exe', 'TLSPROBE.EXE')
)) {
    $src = Join-Path $ow $p[0]
    if (-not (Test-Path $src)) { throw "missing build output: $src" }
    Copy-Item -LiteralPath $src -Destination (Join-Path $dist $p[1]) -Force
    $a = (Get-FileHash -LiteralPath $src).Hash
    $b = (Get-FileHash -LiteralPath (Join-Path $dist $p[1])).Hash
    if ($a -ne $b) { throw "copy mismatch for $($p[1])" }
    Write-Output ("  {0}: {1} bytes" -f $p[1], (Get-Item (Join-Path $dist $p[1])).Length)
}

Write-Output "== Verifying tracked release files =="
& git status --short -- dist/ README.md TOOLS.md
Write-Output ""
Write-Output "Now run:"
Write-Output "  git add -A"
Write-Output "  git commit -m ""<message>"""
Write-Output "  git push origin main"
