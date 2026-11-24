# Prepare a complete AIagent98 release commit.
# Regenerates the OpenWatcom build files, rebuilds every target from clean,
# verifies the PE images, runs the C unit tests and the PS/Python smokes, and
# only then refreshes dist/ and the sample config, so `git add -A &&
# git commit && git push` ships source + runnable files together.
#
# Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File tools\prepare_release.ps1
#         (add -AllowEmojiSkip on headless hosts without a window station)
param(
    [switch]$AllowEmojiSkip
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Assert-Step([string]$label) {
    if ($LASTEXITCODE -ne 0) { throw "$label failed (exit $LASTEXITCODE)" }
}

function Invoke-PythonScript([string]$path) {
    # Prefer the real launcher; the WindowsApps "python" alias is not a Python.
    $py = Get-Command 'py' -ErrorAction SilentlyContinue
    if ($null -ne $py) {
        & py -3 $path
        if ($LASTEXITCODE -eq 0) { return }
        throw "$path failed (exit $LASTEXITCODE)"
    }
    $python = Get-Command 'python' -ErrorAction SilentlyContinue
    if ($null -eq $python) { throw "no Python interpreter found for $path" }
    & python $path
    Assert-Step $path
}

Write-Output "== Regenerating ow/rules.mk and link response files =="
& powershell -NoProfile -ExecutionPolicy Bypass -File 'tools\gen_ow_build.ps1'
Assert-Step 'gen_ow_build.ps1'

Write-Output "== Full clean rebuild (OpenWatcom) =="
# clean no longer removes build\ow\agent98.ini, so the operator's config
# survives the rebuild (see Makefile).
& cmd.exe /c "build_ow.cmd clean"
Assert-Step 'build_ow.cmd clean'
& cmd.exe /c "build_ow.cmd"
Assert-Step 'build_ow.cmd'
& cmd.exe /c "build_ow.cmd probe"
Assert-Step 'build_ow.cmd probe'

$ow = Join-Path $root 'build\ow'
$dist = Join-Path $root 'dist'

Write-Output "== PE compatibility check =="
& powershell -NoProfile -ExecutionPolicy Bypass -File 'tools\check_pe.ps1' `
    (Join-Path $ow 'a98.exe') `
    (Join-Path $ow 'agent98.exe') `
    (Join-Path $ow 'tlsprobe.exe')
Assert-Step 'check_pe.ps1'

Write-Output "== C unit tests (host/CMake) =="
& cmake -B 'build\cmake' -G 'MinGW Makefiles'
Assert-Step 'cmake configure'
& cmake --build 'build\cmake' --target test_tools test_emoji
Assert-Step 'cmake build (test_tools, test_emoji)'
& (Join-Path $root 'build\cmake\test_tools.exe')
Assert-Step 'test_tools'
$emojiArgs = @()
if ($AllowEmojiSkip) { $emojiArgs = @('--allow-skip') }
& (Join-Path $root 'build\cmake\test_emoji.exe') @emojiArgs
Assert-Step 'test_emoji'

Write-Output "== Python / PowerShell smoke tests =="
Invoke-PythonScript 'tests\smoke_tools.py'

# The GUI smokes drive a prepared directory; refresh its EXEs from this build
# but keep its agent98.ini (it points at the mock server on port 8787/8788).
$testRun = Join-Path $root 'build\testrun'
if (-not (Test-Path (Join-Path $testRun 'agent98.ini'))) {
    throw "missing $testRun\agent98.ini - prepare build/testrun first (see README)"
}
foreach ($name in @('a98.exe', 'agent98.exe')) {
    Copy-Item -LiteralPath (Join-Path $ow $name) `
        -Destination (Join-Path $testRun $name) -Force
}
& powershell -NoProfile -ExecutionPolicy Bypass -File 'tests\gui_smoke.ps1' -Dir $testRun
Assert-Step 'gui_smoke.ps1'
& powershell -NoProfile -ExecutionPolicy Bypass -File 'tests\settings_smoke.ps1' -Dir $testRun
Assert-Step 'settings_smoke.ps1'

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
