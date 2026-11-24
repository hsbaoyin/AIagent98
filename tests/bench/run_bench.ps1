# Reset a benchmark task workspace, start the OpenAI-compatible bridge,
# run a98.exe from tests\bench\run, score the result, and stop the bridge.
#
# Examples (from the repository root, or anywhere):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\bench\run_bench.ps1 -Mock
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\bench\run_bench.ps1 -Task bugfix -Mock
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\bench\run_bench.ps1 -Task csv_parse   # live
param(
    [ValidateSet('csv_parse', 'bugfix', 'multifile')]
    [string]$Task = 'csv_parse',
    [switch]$Mock,
    [int]$Port = 8801,
    [int]$TimeoutSec = 300
)

$ErrorActionPreference = 'Stop'

$bench = $PSScriptRoot
$root = Split-Path -Parent $bench
$taskDir = Join-Path $bench (Join-Path 'tasks' $Task)
$ws = Join-Path $bench 'ws'
$hidden = Join-Path $bench 'hidden'
$runDir = Join-Path $bench 'run'
$exe = Join-Path $runDir 'a98.exe'
$ini = Join-Path $runDir 'agent98.ini'
$bridge = Join-Path $bench 'bench_proxy.js'
$score = Join-Path $bench 'score.ps1'
$taskLine = Join-Path $taskDir 'task_one_line.txt'
$proxyLog = Join-Path $bench 'proxy.log'

foreach ($needed in @($taskDir, $exe, $bridge, $score, $taskLine)) {
    if (-not (Test-Path -LiteralPath $needed)) { throw "missing $needed" }
}

Write-Host "== Benchmark: $Task ($(if ($Mock) { 'mock' } else { 'live' })) =="

# --- reset the active workspace and hidden tests from the task fixture -----
foreach ($dir in @($ws, $hidden)) {
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    $fixture = Join-Path $taskDir (Split-Path -Leaf $dir)
    Copy-Item -LiteralPath $fixture -Destination $dir -Recurse -Force
}

# agent98.ini is generated so the workspace path and port always match.
$iniText = @"
[api]
base=http://127.0.0.1:$Port/v1
key=test
model=bench-model
stream=0
max_tokens=2048
temperature=0.2
[agent]
workspace=$ws
allow_shell=1
allow_outside=0
max_steps=12
auto_confirm=1
connect_timeout=10
request_timeout=180
[tls]
verify=0
[log]
level=3
"@
Set-Content -LiteralPath $ini -Value $iniText -Encoding ascii

foreach ($stale in @(
    $proxyLog,
    (Join-Path $runDir 'agent98.log'),
    (Join-Path $bench 'agent_output.txt'),
    (Join-Path $bench 'visible_test.exe'),
    (Join-Path $bench 'hidden_test.exe')
)) {
    Remove-Item -LiteralPath $stale -Force -ErrorAction SilentlyContinue
}

# --- start the bridge -------------------------------------------------------
$nodeCmd = Get-Command node -ErrorAction Stop
$nodeArgs = @("`"$bridge`"")
if ($Mock) { $nodeArgs += "--mock=$Task" }
$nodeArgs += "--port=$Port"
$proxy = Start-Process -FilePath $nodeCmd.Source -ArgumentList $nodeArgs -PassThru -WindowStyle Hidden

try {
    $deadline = (Get-Date).AddSeconds(30)
    $ready = $false
    while ((Get-Date) -lt $deadline -and -not $ready) {
        try {
            $client = New-Object Net.Sockets.TcpClient
            $client.Connect('127.0.0.1', $Port)
            $client.Close()
            $ready = $true
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    if (-not $ready) { throw "bridge did not start on port $Port (see $proxyLog)" }

    # --- run the agent from tests\bench\run ----------------------------------
    $line = (Get-Content -LiteralPath $taskLine -Raw).Trim()
    Push-Location $runDir
    try {
        $agentOut = ($line + "`n/quit`n" | & $exe 2>&1 | Out-String)
    } finally {
        Pop-Location
    }
    Set-Content -LiteralPath (Join-Path $bench 'agent_output.txt') -Value $agentOut -Encoding utf8
    Write-Host "Agent output: $(Join-Path $bench 'agent_output.txt')"

    # --- score and print -----------------------------------------------------
    & $score -Task $Task
    if (-not $Mock) {
        Write-Host 'Live run complete. Model rounds are in tests\bench\proxy.log.'
    }
}
finally {
    if ($proxy -and -not $proxy.HasExited) {
        Stop-Process -Id $proxy.Id -Force -ErrorAction SilentlyContinue
    }
}
