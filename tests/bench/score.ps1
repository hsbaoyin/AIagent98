# 100-point benchmark score (compile 25, visible 20, hidden 40,
# efficiency 10, integrity 5).  Task-specific compile / integrity details
# come from tasks\<Task>\score_hook.ps1, which can replace the $cfg hashtable.
param(
    [string]$Task = 'csv_parse'
)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$ws = Join-Path $root 'ws'
$hidden = Join-Path $root 'hidden'
$taskDir = Join-Path $root (Join-Path 'tasks' $Task)
if (-not (Test-Path -LiteralPath $taskDir)) {
    throw "unknown task '$Task' (expected $taskDir)"
}

# Defaults match the csv_parse fixture; other tasks override in their hook.
$cfg = @{
    Sources            = @('src\strutil.c')
    IncludeDir         = 'include'
    VisibleTest        = 'tests\test_visible.c'
    HiddenTest         = 'test_hidden.c'
    VisiblePassPattern = 'all visible tests passed'
    HiddenScorePattern = 'HIDDEN SCORE:\s*(\d+)/(\d+)'
    Required           = @('Makefile', 'include\strutil.h', 'tests\test_visible.c', 'run_tests.bat')
    Integrity          = @(
        @{ Workspace = 'include\strutil.h'; Pristine = 'pristine_strutil.h' }
    )
}

$hook = Join-Path $taskDir 'score_hook.ps1'
if (Test-Path -LiteralPath $hook) {
    . $hook
}

# Native tools write compiler diagnostics to stderr; keep those from being
# turned into terminating errors by $ErrorActionPreference = 'Stop'.
function Invoke-Native([string[]]$arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $arguments[0] @($arguments[1..($arguments.Length - 1)]) 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    return @{ code = $code; out = ($output | Out-String) }
}

function Get-Compile([string]$testFile, [string]$outFile) {
    $cmd = @('gcc', '-std=c99', '-Wall', '-I', (Join-Path $ws $cfg.IncludeDir), $testFile)
    foreach ($s in $cfg.Sources) { $cmd += (Join-Path $ws $s) }
    $cmd += @('-o', $outFile)
    $r = Invoke-Native $cmd
    return @{ ok = ($r.code -eq 0); out = $r.out }
}

Write-Output '== Benchmark score =='

# --- compile (25) -----------------------------------------------------------
$hiddenExe = Join-Path $root 'hidden_test.exe'
$c = Get-Compile (Join-Path $hidden $cfg.HiddenTest) $hiddenExe
if (-not $c.ok) {
    Write-Output 'COMPILE: FAIL'
    Write-Output $c.out
    Write-Output 'TOTAL: 25/100'
    exit 0
}
Write-Output 'COMPILE: PASS (+25)'
$score = 25

# --- visible tests (20) -----------------------------------------------------
$visExe = Join-Path $root 'visible_test.exe'
$c2 = Get-Compile (Join-Path $ws $cfg.VisibleTest) $visExe
$visOut = ''
if ($c2.ok) { $visOut = (Invoke-Native @($visExe)).out }
if ($c2.ok -and $visOut -match $cfg.VisiblePassPattern) {
    Write-Output 'VISIBLE TESTS: PASS (+20)'
    $score += 20
} else {
    Write-Output 'VISIBLE TESTS: FAIL (+0)'
    if (-not $c2.ok) { Write-Output $c2.out }
    Write-Output $visOut
}

# --- hidden tests (40) ------------------------------------------------------
$hidOut = (Invoke-Native @($hiddenExe)).out
$hidPts = 0
if ($hidOut -match $cfg.HiddenScorePattern) {
    $got = [int]$Matches[1]
    $tot = [int]$Matches[2]
    if ($tot -gt 0) { $hidPts = [math]::Round(40.0 * $got / $tot) }
}
Write-Output "HIDDEN TESTS: $hidPts/40"
Write-Output $hidOut
$score += $hidPts

# --- efficiency (10) --------------------------------------------------------
$logPath = Join-Path $root 'proxy.log'
$rounds = 0
if (Test-Path -LiteralPath $logPath) {
    $rounds = (Select-String -LiteralPath $logPath -Pattern '^REQ ').Count
}
if ($rounds -le 10) { $eff = 10 } elseif ($rounds -le 12) { $eff = 6 } else { $eff = 0 }
Write-Output "EFFICIENCY: $rounds model rounds (+$eff/10)"
$score += $eff

# --- integrity (5) ----------------------------------------------------------
$filesOk = $true
foreach ($required in $cfg.Required) {
    if (-not (Test-Path -LiteralPath (Join-Path $ws $required))) { $filesOk = $false }
}
foreach ($entry in $cfg.Integrity) {
    $pristine = Join-Path $hidden $entry.Pristine
    $current = Join-Path $ws $entry.Workspace
    if (-not (Test-Path -LiteralPath $pristine) -or
        -not (Test-Path -LiteralPath $current) -or
        (Get-FileHash -LiteralPath $pristine -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $current -Algorithm SHA256).Hash) {
        $filesOk = $false
    }
}
if ($filesOk) {
    Write-Output 'INTEGRITY: header/provided files intact (+5)'
    $score += 5
} else {
    Write-Output 'INTEGRITY: changed header or removed provided files (+0)'
}

Write-Output "TOTAL: $score/100"
