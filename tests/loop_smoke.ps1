# Loop-control smoke test for AIagent98.
#
# Starts tests\mock_openai.js and runs the CLI four times:
#   1. max_steps=0 with a task that needs 40 tool rounds (must not be cut
#      at the old 8/24/32 limits),
#   1b. the same task with no max_steps key (the default must be unlimited),
#   2. a model that repeats the exact same tool call (the loop guard must
#      block the third call and stop the run on the fourth),
#   3. max_steps=3 with the same 40-round task (must stop after three).
#
# Expects a98.exe in -Dir (default build\testrun_loop); the script writes
# agent98.ini next to it.

param(
    [string]$Dir = "build\testrun_loop",
    [int]$Port = 8791,
    [int]$TimeoutSec = 90
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path $Dir)) {
    New-Item -ItemType Directory -Path $Dir | Out-Null
}
$exe = Join-Path $Dir 'a98.exe'
if (-not (Test-Path $exe)) {
    throw "missing $exe"
}
$exe = (Resolve-Path $exe).Path
$Dir = (Resolve-Path $Dir).Path
$mock = Join-Path $root 'tests\mock_openai.js'
$log = Join-Path $Dir 'agent98.log'
$ini = Join-Path $Dir 'agent98.ini'

function Write-Ini([int]$Steps, [switch]$OmitSteps) {
    $stepsLine = if ($OmitSteps) { '' } else { "max_steps=$Steps" }
    $text = @"
[api]
base=http://127.0.0.1:$Port/v1
key=test
model=mock-model
stream=1
max_tokens=256
temperature=0.7
[agent]
workspace=.\workspace
allow_shell=0
allow_outside=0
$stepsLine
connect_timeout=10
request_timeout=30
[tls]
verify=0
[log]
level=3
"@
    Set-Content -Path $ini -Value $text -Encoding ascii
}

function Run-Agent([string]$inputText) {
    Push-Location $Dir
    try {
        return ($inputText + "`n/stats`n/quit`n" | & $exe 2>&1 | Out-String)
    } finally {
        Pop-Location
    }
}

function Round-Count([string]$out) {
    return ([regex]::Matches($out, 'Thinking \(round \d+(?:/\d+)?\)')).Count
}

$node = Start-Process -FilePath 'node' -ArgumentList @("`"$mock`"", "$Port") -PassThru -WindowStyle Hidden
try {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $ready = $false
    while ((Get-Date) -lt $deadline -and -not $ready) {
        try {
            $c = New-Object Net.Sockets.TcpClient
            $c.Connect('127.0.0.1', $Port)
            $c.Close()
            $ready = $true
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    if (-not $ready) {
        throw "mock server did not start on port $Port"
    }

    Write-Ini 0
    Remove-Item $log -Force -ErrorAction SilentlyContinue

    # 1. Unlimited mode: 40 tool rounds plus the final answer.
    $out = Run-Agent 'count up 40'
    if ($out -notmatch 'Counted 40 steps\.') {
        throw "unlimited run did not finish:`n$out"
    }
    $rounds = Round-Count $out
    if ($rounds -ne 41) {
        throw "expected 41 rounds with max_steps=0, got ${rounds}:`n$out"
    }
    if ($out -match 'max_steps') {
        throw "unlimited run was stopped by max_steps:`n$out"
    }
    if ($out -notmatch 'Run completed: 41 round') {
        throw "run summary was not emitted:`n$out"
    }
    if ($out -notmatch 'Last run: 41 round\(s\), 40 tool call\(s\)') {
        throw "CLI /stats did not report the last run:`n$out"
    }
    Write-Host 'PASS: run summary and CLI /stats report tool rounds'
    Write-Host 'PASS: max_steps=0 allows 40 tool rounds'

    # 1b. Default config (no max_steps key) must be unlimited as well.
    Write-Ini 0 -OmitSteps
    Remove-Item $log -Force -ErrorAction SilentlyContinue
    $out = Run-Agent 'count up 40'
    if ($out -notmatch 'Counted 40 steps\.') {
        throw "default max_steps run did not finish:`n$out"
    }
    $rounds = Round-Count $out
    if ($rounds -ne 41) {
        throw "expected 41 rounds with default max_steps, got ${rounds}:`n$out"
    }
    if ($out -match 'max_steps') {
        throw "default max_steps was stopped by the old cap:`n$out"
    }
    Write-Host 'PASS: default max_steps allows 40 tool rounds'

    # 2. Loop guard: the third identical call is blocked, the fourth stops.
    $out = Run-Agent 'loop forever'
    if ($out -notmatch 'blocked by loop guard') {
        throw "loop guard did not block the repeated call:`n$out"
    }
    if ($out -notmatch 'identical arguments 3 times') {
        throw "loop guard did not report the third repeat:`n$out"
    }
    if ($out -notmatch 'loop guard stopped the run') {
        throw "loop guard did not stop the run:`n$out"
    }
    $rounds = Round-Count $out
    if ($rounds -ne 4) {
        throw "expected 4 rounds before the loop guard stopped, got ${rounds}:`n$out"
    }
    Write-Host 'PASS: loop guard blocked the third identical call'
    Write-Host 'PASS: loop guard stopped the run on the fourth call'

    # 3. Bounded mode still works.
    Write-Ini 3
    $out = Run-Agent 'count up 40'
    if ($out -notmatch 'stopped after 3 tool rounds') {
        throw "max_steps=3 did not stop the run:`n$out"
    }
    $rounds = Round-Count $out
    if ($rounds -ne 3) {
        throw "expected 3 rounds with max_steps=3, got ${rounds}:`n$out"
    }
    Write-Host 'PASS: max_steps=3 stops after three rounds'

    $logText = Get-Content $log -Raw
    if ($logText -notmatch 'loop guard: blocked repeated tool call') {
        throw "loop guard was not logged in $log"
    }
    Write-Host 'PASS: loop guard logged the repeated call'

    Write-Host 'loop smoke test passed'
}
finally {
    if ($node -and -not $node.HasExited) {
        Stop-Process -Id $node.Id -Force
    }
}
