#Requires -Version 5.1
<#
.SYNOPSIS
    Kairos test runner for Windows.

.DESCRIPTION
    Runs, filters, and inspects the Kairos test suite on Windows using CTest.
    Mirrors the functionality of scripts/test.sh for Linux/macOS.

.EXAMPLE
    .\scripts\test.ps1                         # Run all tests
    .\scripts\test.ps1 -Filter "Sha256"        # Tests matching regex
    .\scripts\test.ps1 -List                   # List all tests without running
    .\scripts\test.ps1 -ShowHelp               # Full option reference
#>

param(
    # Name filters
    [string]$Filter = "",
    [string]$Exclude = "",

    # Suite shortcuts (convenience)
    [switch]$Tui,
    [switch]$Kel,
    [switch]$Watch,
    [switch]$Mcp,
    [switch]$Engine,
    [switch]$Migration,
    [switch]$E2E,

    # Build selection
    [string]$BuildDir = "",
    [switch]$San,
    [switch]$UseRelease,
    [switch]$ClearCache,

    # Build config (for multi-config generators like MSVC)
    [ValidateSet("", "Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "",

    # Execution
    [int]$Repeat = 1,
    [int]$Parallel = 0,
    [switch]$StopOnFail,
    [switch]$RerunFailed,

    # Output
    [switch]$Verbose,
    [switch]$Quiet,
    [switch]$List,
    [switch]$DryRun,

    # Help
    [switch]$ShowHelp
)

# =============================================================================
# Color helpers
# =============================================================================
$esc = [char]27

function Log   { param([string]$Msg) Write-Host "$esc[36m$esc[1m>$esc[0m $Msg" }
function Ok    { param([string]$Msg) Write-Host "$esc[32m$esc[1m+$esc[0m $Msg" }
function Warn  { param([string]$Msg) Write-Host "$esc[33m$esc[1m!$esc[0m $Msg" }
function Err   { param([string]$Msg) Write-Host "$esc[31m$esc[1mX$esc[0m $Msg" }
function Hdr   { param([string]$Msg) Write-Host ""; Write-Host "$esc[1m$esc[35m--- $Msg ---$esc[0m" }
function CfgLine {
    param([string]$Label, [string]$Value, [string]$Color = "white")
    $c = @{ "white"="$esc[37m"; "bold"="$esc[1m"; "green"="$esc[32m"; "dim"="$esc[2m";
            "yellow"="$esc[33m"; "red"="$esc[31m"; "magenta"="$esc[35m"; "cyan"="$esc[36m" }
    $cv = if ($c.ContainsKey($Color)) { $c[$Color] } else { "" }
    Write-Host "  $esc[2m$($Label.PadRight(20))$esc[0m ${cv}${Value}$esc[0m"
}
function Die { param([string]$Msg) Err $Msg; exit 1 }

# =============================================================================
# Help
# =============================================================================
if ($ShowHelp) {
    Write-Host ""
    Write-Host "$esc[1m$esc[36m  Kairos Test Runner (Windows / PowerShell)$esc[0m"
    Write-Host ""
    Write-Host "  USAGE"
    Write-Host "    .\scripts\test.ps1 [OPTIONS...]"
    Write-Host ""
    Write-Host "  NAME FILTERS"
    Write-Host "    -Filter <regex>        Only tests whose name matches regex"
    Write-Host "    -Exclude <regex>       Exclude tests whose name matches regex"
    Write-Host ""
    Write-Host "  SUITE SHORTCUTS (convenience filters)"
    Write-Host "    -Tui                   Run TUI dashboard tests only"
    Write-Host "    -Kel                   Run KEL (expression language) tests only"
    Write-Host "    -Watch                 Run watch engine tests only"
    Write-Host "    -Mcp                   Run MCP server tests only"
    Write-Host "    -Engine                Run engine (pipeline/DAG/scheduler) tests only"
    Write-Host "    -Migration             Run migration tool tests only"
    Write-Host "    -E2E                   Run end-to-end tests only"
    Write-Host ""
    Write-Host "  BUILD SELECTION"
    Write-Host "    -BuildDir <path>       Explicit build directory"
    Write-Host "    -San                   Use sanitizer build (build\debug-asan)"
    Write-Host "    -UseRelease            Use release build (build\release)"
    Write-Host "    -Config <cfg>          Build config: Debug, Release, etc."
    Write-Host "    -ClearCache            Clear cached build directory selection"
    Write-Host ""
    Write-Host "  EXECUTION"
    Write-Host "    -Repeat <N>            Repeat each test N times"
    Write-Host "    -Parallel <N>          Run N tests in parallel"
    Write-Host "    -StopOnFail            Stop on first failure"
    Write-Host "    -RerunFailed           Only rerun previously-failed tests"
    Write-Host ""
    Write-Host "  OUTPUT"
    Write-Host "    -Verbose               Show all test output"
    Write-Host "    -Quiet                 Suppress test output on failure too"
    Write-Host "    -List                  List matching tests without running"
    Write-Host "    -DryRun                Print CTest command without executing"
    Write-Host ""
    Write-Host "  EXAMPLES"
    Write-Host "    .\scripts\test.ps1                              # Run everything"
    Write-Host "    .\scripts\test.ps1 -Filter 'Sha256|ContentId'   # Filter by name"
    Write-Host "    .\scripts\test.ps1 -List                        # List all tests"
    Write-Host "    .\scripts\test.ps1 -San -StopOnFail             # Sanitizer, stop early"
    Write-Host "    .\scripts\test.ps1 -Config Release              # Release build tests"
    Write-Host "    .\scripts\test.ps1 -Repeat 5                    # Stress test"
    Write-Host "    .\scripts\test.ps1 -Tui                          # TUI tests only"
    Write-Host ""
    exit 0
}

# =============================================================================
# Locate project root
# =============================================================================
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$BuildCache  = Join-Path $ProjectRoot ".builddir_cache"

# =============================================================================
# Clear cache
# =============================================================================
if ($ClearCache) {
    if (Test-Path $BuildCache) {
        Remove-Item $BuildCache -Force
        Ok "Build directory cache cleared: $BuildCache"
    } else { Log "No build directory cache to clear." }
    exit 0
}

# =============================================================================
# Discover build directories
# =============================================================================
function Find-BuildDirs {
    $buildBase = Join-Path $ProjectRoot "build"
    if (-not (Test-Path $buildBase)) { return @() }
    $dirs = @()
    Get-ChildItem -Path $buildBase -Recurse -Depth 2 -Filter "CTestTestfile.cmake" `
        -ErrorAction SilentlyContinue | ForEach-Object { $dirs += $_.DirectoryName }
    return $dirs | Sort-Object -Unique
}

function Detect-BuildConfig {
    param([string]$Dir)
    if ($Config) { return $Config }
    $configDirs = @("Debug", "Release", "RelWithDebInfo", "MinSizeRel")
    foreach ($c in $configDirs) {
        $testPath = Join-Path $Dir "tests\$c"
        if (Test-Path $testPath) {
            $exes = Get-ChildItem -Path $testPath -Filter "test_*.exe" -ErrorAction SilentlyContinue
            if ($exes.Count -gt 0) { return $c }
        }
    }
    $cache = Join-Path $Dir "CMakeCache.txt"
    if (Test-Path $cache) {
        $line = Get-Content $cache | Where-Object { $_ -match "^CMAKE_BUILD_TYPE:" }
        if ($line -match "=(.+)$") { return $Matches[1] }
    }
    return "Debug"
}

function Select-BuildDir {
    param([string[]]$Dirs)
    if ($Dirs.Count -eq 0) { return "" }
    Write-Host ""; Write-Host "$esc[1mMultiple build directories found:$esc[0m"; Write-Host ""
    for ($i = 0; $i -lt $Dirs.Count; $i++) {
        $dir = $Dirs[$i]; $relDir = $dir.Replace("$ProjectRoot\", "").Replace("$ProjectRoot/", "")
        $marker = ""
        if ((Test-Path $BuildCache) -and (Get-Content $BuildCache -ErrorAction SilentlyContinue) -eq $dir) {
            $marker = " $esc[32m(last used)$esc[0m"
        }
        # Read build type from CMakeCache.txt.
        $buildType = ""
        $cacheFile = Join-Path $dir "CMakeCache.txt"
        if (Test-Path $cacheFile) {
            $line = Get-Content $cacheFile | Where-Object { $_ -match "^CMAKE_BUILD_TYPE:" }
            if ($line -match "=(.+)$") { $buildType = $Matches[1] }
        }
        $typeTag = if ($buildType) { " $esc[2m[$buildType]$esc[0m" } else { "" }
        Write-Host "  $esc[36m$($i + 1)$esc[0m) $esc[1m$relDir$esc[0m$typeTag$marker"
    }
    Write-Host ""
    while ($true) {
        Write-Host "$esc[33mSelect build directory$esc[0m (1-$($Dirs.Count), or Enter for last used): " -NoNewline
        $choice = Read-Host

        # Bare Enter: try cache, then fall back to first option.
        if ([string]::IsNullOrWhiteSpace($choice)) {
            if (Test-Path $BuildCache) {
                $cached = Get-Content $BuildCache -ErrorAction SilentlyContinue
                if ($cached -and ($Dirs -contains $cached)) { return $cached }
                Write-Host "$esc[33mCached directory no longer valid, please select manually.$esc[0m"
                continue
            }
            # No cache — pick first directory.
            $selected = $Dirs[0]
            Set-Content -Path $BuildCache -Value $selected
            return $selected
        }

        $num = 0
        if ([int]::TryParse($choice, [ref]$num) -and $num -ge 1 -and $num -le $Dirs.Count) {
            $selected = $Dirs[$num - 1]
            Set-Content -Path $BuildCache -Value $selected
            return $selected
        }
        Write-Host "$esc[31mInvalid selection. Enter a number between 1 and $($Dirs.Count).$esc[0m"
    }
}

# =============================================================================
# Find build directory
# =============================================================================
if (-not $BuildDir) {
    if ($San) {
        $candidates = @((Join-Path $ProjectRoot "build\debug-asan"), (Join-Path $ProjectRoot "build\debug-san"))
        foreach ($d in $candidates) {
            if (Test-Path (Join-Path $d "CTestTestfile.cmake")) { $BuildDir = $d; break }
        }
        if (-not $BuildDir) { Die "No sanitizer build found. Run .\scripts\build.ps1 -Asan first." }
    } elseif ($UseRelease) {
        $BuildDir = Join-Path $ProjectRoot "build\release"
    } else {
        $discovered = Find-BuildDirs
        if ($discovered.Count -eq 0) { Die "No build directory found. Run .\scripts\build.ps1 first." }
        elseif ($discovered.Count -eq 1) { $BuildDir = $discovered[0]; Set-Content -Path $BuildCache -Value $BuildDir }
        else { $BuildDir = Select-BuildDir $discovered; if (-not $BuildDir) { Die "No build directory selected." } }
    }
}

if (-not (Test-Path (Join-Path $BuildDir "CTestTestfile.cmake"))) {
    Die "Build directory is invalid: $BuildDir (missing CTestTestfile.cmake)"
}

$DetectedConfig = Detect-BuildConfig $BuildDir

# =============================================================================
# Apply suite shortcuts (override -Filter if a shortcut was used)
# =============================================================================
if ($Tui)       { $Filter = "Tui" }
if ($Kel)       { $Filter = "Kel|Lexer|Parser|Evaluator" }
if ($Watch)     { $Filter = "Watch|Scan|Inotify|Debounce|Hash" }
if ($Mcp)       { $Filter = "Mcp" }
if ($Engine)    { $Filter = "Pipeline|Dag|Scheduler|Trigger|Execution" }
if ($Migration) { $Filter = "Migration" }
if ($E2E)       { $Filter = "E2E|DaemonLifecycle" }

# =============================================================================
# CTest arguments
# =============================================================================
$ctestArgs = [System.Collections.Generic.List[string]]::new()
$ctestArgs.Add("--test-dir"); $ctestArgs.Add($BuildDir)

if ($DetectedConfig) { $ctestArgs.Add("-C"); $ctestArgs.Add($DetectedConfig) }
if ($Filter)         { $ctestArgs.Add("-R"); $ctestArgs.Add($Filter) }
if ($Exclude)        { $ctestArgs.Add("-E"); $ctestArgs.Add($Exclude) }

if ($Verbose)                      { $ctestArgs.Add("-V") }
elseif (-not $Quiet)               { $ctestArgs.Add("--output-on-failure") }
if ($Repeat -gt 1)                 { $ctestArgs.Add("--repeat"); $ctestArgs.Add("until-fail:$Repeat") }
if ($Parallel -gt 0)               { $ctestArgs.Add("--parallel"); $ctestArgs.Add("$Parallel") }
if ($StopOnFail)                   { $ctestArgs.Add("--stop-on-failure") }
if ($RerunFailed)                  { $ctestArgs.Add("--rerun-failed") }

# =============================================================================
# Banner + summary
# =============================================================================
Write-Host ""
Write-Host "$esc[1m$esc[36m  Kairos$esc[0m"
Write-Host "$esc[2m  Test Runner (Windows)$esc[0m"
Write-Host ""

CfgLine "Build directory" $BuildDir
if ($DetectedConfig) { CfgLine "Build config" $DetectedConfig "cyan" }
if ($Filter)         { CfgLine "Name filter" $Filter "yellow" }
if ($Exclude)        { CfgLine "Exclude" $Exclude "yellow" }
if ($San)            { CfgLine "Sanitizers" "enabled (ASan)" "red" }
if ($Repeat -gt 1)   { CfgLine "Repeat" "${Repeat}x" "magenta" }

# =============================================================================
# List mode
# =============================================================================
if ($List) {
    Hdr "Available Tests"; Write-Host ""
    $listArgs = @("--test-dir", $BuildDir, "-N")
    if ($DetectedConfig) { $listArgs += @("-C", $DetectedConfig) }
    if ($Filter)  { $listArgs += @("-R", $Filter) }
    if ($Exclude) { $listArgs += @("-E", $Exclude) }

    $testCount = 0
    & ctest @listArgs 2>&1 | ForEach-Object {
        $line = "$_"
        if ($line -match '^\s*Test\s+#(\d+):\s+(.+)') {
            $num = $Matches[1]; $name = $Matches[2]; $testCount++
            if     ($name -match "Sha256|Hex|ContentId|RunId|Correlation") { $color = "$esc[34m"; $badge = "id  " }
            elseif ($name -match "Config|Validation")                     { $color = "$esc[32m"; $badge = "cfg " }
            elseif ($name -match "Path|Platform")                         { $color = "$esc[35m"; $badge = "plat" }
            elseif ($name -match "Schema|Migration|Database")             { $color = "$esc[33m"; $badge = "db  " }
            elseif ($name -match "Json|Log")                              { $color = "$esc[36m"; $badge = "log " }
            elseif ($name -match "Exit")                                  { $color = "$esc[31m"; $badge = "exit" }
            elseif ($name -match "Tui|Dashboard")                         { $color = "$esc[35m"; $badge = "tui " }
            elseif ($name -match "Mcp")                                   { $color = "$esc[36m"; $badge = "mcp " }
            elseif ($name -match "Watch|Scan|Inotify|Debounce")           { $color = "$esc[32m"; $badge = "wtch" }
            elseif ($name -match "Kel|Lexer|Parser|Evaluator")            { $color = "$esc[34m"; $badge = "kel " }
            elseif ($name -match "Pipeline|Dag|Scheduler|Trigger")        { $color = "$esc[33m"; $badge = "eng " }
            elseif ($name -match "Runner|Process|Docker")                 { $color = "$esc[31m"; $badge = "exec" }
            else                                                          { $color = "$esc[37m"; $badge = "    " }
            Write-Host ("  $esc[2m{0,3}$esc[0m  ${color}{1,-4}$esc[0m  {2}" -f $num, $badge, $name)
        }
    }
    Write-Host ""; Write-Host "  $esc[1m${testCount}$esc[0m test(s) matched"; Write-Host ""
    exit 0
}

# =============================================================================
# Dry-run
# =============================================================================
if ($DryRun) {
    Hdr "Dry Run -- command that would execute"
    Write-Host ""; Write-Host "  $esc[2mctest$esc[0m $($ctestArgs -join ' ')"; Write-Host ""
    exit 0
}

if (-not (Get-Command ctest -ErrorAction SilentlyContinue)) { Die "ctest not found in PATH." }

# =============================================================================
# Run tests
# =============================================================================
Hdr "Running Tests"; Write-Host ""

$testStart = Get-Date
$overallExit = 0

& ctest @ctestArgs 2>&1 | ForEach-Object {
    $line = "$_"
    if     ($line -match "100% tests passed")               { Write-Host "  $esc[32m$esc[1m$line$esc[0m" }
    elseif ($line -match "Passed")                          { Write-Host "  $esc[32m$line$esc[0m" }
    elseif ($line -match "Failed|FAILED|Error|\*\*\*")      { Write-Host "  $esc[31m$esc[1m$line$esc[0m" }
    elseif ($line -match "tests passed")                    { Write-Host "  $esc[33m$esc[1m$line$esc[0m" }
    elseif ($line -match "Total Test time")                 { Write-Host "  $esc[36m$line$esc[0m" }
    elseif ($line -match "^\s*\d+/\d+.*Passed")             { Write-Host "  $esc[32m$line$esc[0m" }
    elseif ($line -match "^\s*\d+/\d+.*(Failed|FAILED)")    { Write-Host "  $esc[31m$esc[1m$line$esc[0m" }
    else                                                     { Write-Host "  $esc[2m$line$esc[0m" }
}

if ($LASTEXITCODE -ne 0) { $overallExit = 1 }
$elapsed = [math]::Round(((Get-Date) - $testStart).TotalSeconds, 1)

# =============================================================================
# Summary
# =============================================================================
Write-Host ""
if ($overallExit -eq 0) {
    Write-Host "  $esc[42m$esc[1m$esc[37m ALL TESTS PASSED $esc[0m  $esc[2m(${elapsed}s)$esc[0m"
} else {
    Write-Host "  $esc[41m$esc[1m$esc[37m SOME TESTS FAILED $esc[0m  $esc[2m(${elapsed}s)$esc[0m"
}

Write-Host ""
CfgLine "Build dir" $BuildDir
CfgLine "Elapsed" "${elapsed}s"
if ($DetectedConfig) { CfgLine "Config" $DetectedConfig }
if ($Filter)  { CfgLine "Filter"  $Filter }
if ($Exclude) { CfgLine "Exclude" $Exclude }
Write-Host ""

if ($overallExit -ne 0) {
    Write-Host "  $esc[2mRerun failures:$esc[0m  .\scripts\test.ps1 -RerunFailed"
    Write-Host "  $esc[2mVerbose rerun:$esc[0m   .\scripts\test.ps1 -RerunFailed -Verbose"
    Write-Host ""
}

exit $overallExit
