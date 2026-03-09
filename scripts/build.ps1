#Requires -Version 5.1
<#
.SYNOPSIS
    Kairos build script for Windows.

.DESCRIPTION
    Configures and compiles the Kairos orchestration daemon on Windows using CMake.
    Mirrors the functionality of scripts/build.sh for Linux/macOS.

.EXAMPLE
    .\scripts\build.ps1                        # Default: debug build
    .\scripts\build.ps1 -Release               # Release build
    .\scripts\build.ps1 -Profile san           # Sanitizer profile (MSVC ASan)
    .\scripts\build.ps1 -Http                  # Enable HTTP server + Web UI
    .\scripts\build.ps1 -Vault                 # Enable Ansible Vault (OpenSSL)
    .\scripts\build.ps1 -Clean                 # Wipe build dir first
    .\scripts\build.ps1 -ShowHelp              # Full option reference
#>

param(
    # Build type
    [switch]$Debug,
    [switch]$Release,
    [switch]$RelWithDebInfo,
    [switch]$MinSizeRel,

    # Features
    [switch]$Tests,
    [switch]$NoTests,
    [switch]$Http,
    [switch]$Otel,
    [switch]$Vault,
    [switch]$Tui,

    # Sanitizers (MSVC supports ASan)
    [switch]$Asan,

    # Profiles
    [ValidateSet("", "dev", "san", "release", "ci", "full")]
    [string]$Profile = "",

    # Toolchain
    [ValidateSet("", "msvc", "clang-cl", "clang", "gcc")]
    [string]$Compiler = "",
    [int]$Jobs = 0,

    # Directories
    [string]$BuildDir = "",
    [string]$Prefix = "",

    # Actions
    [switch]$Clean,
    [switch]$ConfigureOnly,
    [switch]$Install,
    [switch]$DryRun,
    [switch]$Verbose,

    # Help
    [switch]$ShowHelp
)

# =============================================================================
# Color helpers
# =============================================================================
$esc = [char]27

function Log
{ param([string]$Msg) Write-Host "$esc[36m$esc[1m>$esc[0m $Msg"
}
function Ok
{ param([string]$Msg) Write-Host "$esc[32m$esc[1m+$esc[0m $Msg"
}
function Warn
{ param([string]$Msg) Write-Host "$esc[33m$esc[1m!$esc[0m $Msg"
}
function Err
{ param([string]$Msg) Write-Host "$esc[31m$esc[1mX$esc[0m $Msg"
}
function Hdr
{ param([string]$Msg) Write-Host ""; Write-Host "$esc[1m$esc[35m--- $Msg ---$esc[0m"
}
function CfgLine
{
    param([string]$Label, [string]$Value, [string]$Color = "white")
    $c = @{ "white"="$esc[37m"; "bold"="$esc[1m"; "green"="$esc[32m"; "dim"="$esc[2m";
        "yellow"="$esc[33m"; "red"="$esc[31m"; "magenta"="$esc[35m"; "cyan"="$esc[36m"
    }
    $cv = if ($c.ContainsKey($Color))
    { $c[$Color]
    } else
    { ""
    }
    Write-Host "  $esc[2m$($Label.PadRight(20))$esc[0m ${cv}${Value}$esc[0m"
}
function Die
{ param([string]$Msg) Err $Msg; exit 1
}

# =============================================================================
# Help
# =============================================================================
if ($ShowHelp)
{
    Write-Host ""
    Write-Host "$esc[1m$esc[36m  Kairos Build Script (Windows / PowerShell)$esc[0m"
    Write-Host ""
    Write-Host "  USAGE"
    Write-Host "    .\scripts\build.ps1 [OPTIONS...]"
    Write-Host ""
    Write-Host "  BUILD TYPE"
    Write-Host "    -Debug                 Debug build (default)"
    Write-Host "    -Release               Release build (-O2, NDEBUG)"
    Write-Host "    -RelWithDebInfo        Release with debug info"
    Write-Host "    -MinSizeRel            Minimum-size release"
    Write-Host ""
    Write-Host "  FEATURES"
    Write-Host "    -Tests / -NoTests      Build test suite (default: on)"
    Write-Host "    -Http                  Build HTTP server + Web UI"
    Write-Host "    -Otel                  Build with OpenTelemetry tracing"
    Write-Host "    -Vault                 Build with Ansible Vault support (OpenSSL)"
    Write-Host "    -Tui                   Build TUI dashboard (FTXUI)"
    Write-Host ""
    Write-Host "  SANITIZERS"
    Write-Host "    -Asan                  Enable AddressSanitizer (MSVC)"
    Write-Host ""
    Write-Host "  PROFILES  (predefined option bundles)"
    Write-Host "    -Profile dev           Debug + tests (default)"
    Write-Host "    -Profile san           Debug + ASan + tests"
    Write-Host "    -Profile release       Release, no tests"
    Write-Host "    -Profile ci            Debug + ASan + tests"
    Write-Host "    -Profile full          Release + HTTP + tests"
    Write-Host ""
    Write-Host "  TOOLCHAIN"
    Write-Host "    -Compiler msvc         Use MSVC (default)"
    Write-Host "    -Compiler clang-cl     Use clang-cl with MSVC ABI"
    Write-Host "    -Jobs N                Parallel build jobs (default: CPU count)"
    Write-Host ""
    Write-Host "  DIRECTORIES"
    Write-Host "    -BuildDir <path>       Custom build directory"
    Write-Host "    -Prefix <path>         Install prefix"
    Write-Host ""
    Write-Host "  ACTIONS"
    Write-Host "    -Clean                 Remove build dir before configuring"
    Write-Host "    -ConfigureOnly         Configure but don't build"
    Write-Host "    -Install               Build then install"
    Write-Host "    -DryRun                Print CMake command without executing"
    Write-Host "    -Verbose               Verbose build output"
    Write-Host ""
    Write-Host "  EXAMPLES"
    Write-Host "    .\scripts\build.ps1                             # Quick debug build"
    Write-Host "    .\scripts\build.ps1 -Release -Prefix C:\kairos  # Release + install"
    Write-Host "    .\scripts\build.ps1 -Profile san                # Sanitizer build"
    Write-Host "    .\scripts\build.ps1 -Profile full               # Release + HTTP"
    Write-Host ""
    exit 0
}

# =============================================================================
# Locate project root
# =============================================================================
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

# =============================================================================
# Apply profiles and flags
# =============================================================================
$BuildType = "Debug"
$EnableTests = "ON"
$EnableHttp = "OFF"
$EnableOtel = "OFF"
$EnableVault = "OFF"
$EnableTui = "OFF"
$EnableAsan = "OFF"

switch ($Profile)
{
    "dev"
    { $BuildType = "Debug"; $EnableTests = "ON"
    }
    "san"
    { $BuildType = "Debug"; $EnableAsan = "ON"; $EnableTests = "ON"
    }
    "release"
    { $BuildType = "Release"; $EnableTests = "OFF"
    }
    "ci"
    { $BuildType = "Debug"; $EnableAsan = "ON"; $EnableTests = "ON"
    }
    "full"
    { $BuildType = "Release"; $EnableHttp = "ON"; $EnableOtel = "ON"; $EnableVault = "ON"; $EnableTests = "ON"
    }
}

if ($Debug)
{ $BuildType = "Debug"
}
if ($Release)
{ $BuildType = "Release"
}
if ($RelWithDebInfo)
{ $BuildType = "RelWithDebInfo"
}
if ($MinSizeRel)
{ $BuildType = "MinSizeRel"
}
if ($Tests)
{ $EnableTests = "ON"
}
if ($NoTests)
{ $EnableTests = "OFF"
}
if ($Http)
{ $EnableHttp = "ON"
}
if ($Otel)
{ $EnableOtel = "ON"
}
if ($Vault)
{ $EnableVault = "ON"
}
if ($Tui)
{ $EnableTui = "ON"
}
if ($Asan)
{ $EnableAsan = "ON"
}

# Build directory
if (-not $BuildDir)
{
    $suffix = $BuildType.ToLower()
    if ($EnableAsan -eq "ON")
    { $suffix += "-asan"
    }
    $BuildDir = Join-Path $ProjectRoot "build\$suffix"
}

# Job count
if ($Jobs -le 0)
{
    $Jobs = [Environment]::ProcessorCount
    if ($Jobs -le 0)
    { $Jobs = 4
    }
}

# =============================================================================
# Banner + summary
# =============================================================================
Write-Host ""
Write-Host "$esc[1m$esc[36m  Kairos$esc[0m"
Write-Host "$esc[2m  Build System (Windows)$esc[0m"
Write-Host ""

Hdr "Configuration"
CfgLine "Build type" $BuildType "bold"
CfgLine "Build directory" $BuildDir
CfgLine "Parallel jobs" $Jobs
if ($Compiler)
{ CfgLine "Compiler" $Compiler "yellow"
}

$features = @()
if ($EnableTests -eq "ON")
{ $features += "tests"
}
if ($EnableHttp -eq "ON")
{ $features += "http"
}
if ($EnableOtel -eq "ON")
{ $features += "otel"
}
if ($EnableVault -eq "ON")
{ $features += "vault"
}
if ($EnableTui -eq "ON")
{ $features += "tui"
}
CfgLine "Features" (($features -join " ") + $(if ($features.Count -eq 0)
        { "none"
        } else
        { ""
        }))

if ($EnableAsan -eq "ON")
{ CfgLine "Sanitizers" "ASan" "red"
} else
{ CfgLine "Sanitizers" "none" "dim"
}

if ($Profile)
{ CfgLine "Profile" $Profile "magenta"
}
Write-Host ""

# =============================================================================
# CMake arguments
# =============================================================================
$CmakeArgs = @(
    "-S", $ProjectRoot,
    "-B", $BuildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    "-DKAIROS_BUILD_TESTS=$EnableTests",
    "-DKAIROS_HTTP=$EnableHttp",
    "-DKAIROS_OTEL=$EnableOtel",
    "-DKAIROS_VAULT=$EnableVault",
    "-DKAIROS_TUI=$EnableTui"
)

$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if ($ninja)
{ $CmakeArgs += @("-G", "Ninja")
}

switch ($Compiler)
{
    "clang-cl"
    { $CmakeArgs += @("-T", "ClangCL")
    }
    "clang"
    { $CmakeArgs += @("-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++")
    }
    "gcc"
    { $CmakeArgs += @("-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++")
    }
}

if ($EnableAsan -eq "ON")
{
    $CmakeArgs += "-DCMAKE_CXX_FLAGS=/fsanitize=address"
}

if ($Prefix)
{ $CmakeArgs += "-DCMAKE_INSTALL_PREFIX=$Prefix"
}

# =============================================================================
# Dry-run
# =============================================================================
if ($DryRun)
{
    Hdr "Dry Run -- commands that would execute"
    Write-Host ""
    if ($Clean)
    { Write-Host "  Remove-Item -Recurse -Force `"$BuildDir`""
    }
    Write-Host "  cmake $($CmakeArgs -join ' ')"
    if (-not $ConfigureOnly)
    { Write-Host "  cmake --build `"$BuildDir`" --config $BuildType -j $Jobs"
    }
    if ($Install)
    { Write-Host "  cmake --install `"$BuildDir`" --config $BuildType"
    }
    Write-Host ""; exit 0
}

# =============================================================================
# Verify cmake
# =============================================================================
if (-not (Get-Command cmake -ErrorAction SilentlyContinue))
{
    Die "cmake not found in PATH. Install CMake or add it to PATH."
}

# =============================================================================
# Ensure MSVC environment
# =============================================================================
$clCmd = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $clCmd)
{
    Log "MSVC environment not detected -- searching for Visual Studio..."
    $vswherePaths = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    $vswhere = $null
    foreach ($p in $vswherePaths)
    { if (Test-Path $p)
        { $vswhere = $p; break
        }
    }
    if (-not $vswhere)
    { Die "Cannot find vswhere.exe. Install Visual Studio Build Tools."
    }

    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if (-not $vsPath)
    { $vsPath = & $vswhere -latest -products * -property installationPath 2>$null
    }
    if (-not $vsPath)
    { Die "No Visual Studio installation found."
    }

    $vcvarsall = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
    if (-not (Test-Path $vcvarsall))
    { Die "vcvarsall.bat not found at: $vcvarsall"
    }

    $arch = if ([Environment]::Is64BitOperatingSystem)
    { "x64"
    } else
    { "x86"
    }
    Log "Loading MSVC environment from: $vsPath ($arch)..."

    $envDump = cmd /c "`"$vcvarsall`" $arch >nul 2>&1 && set" 2>$null
    if ($LASTEXITCODE -ne 0)
    { Die "vcvarsall.bat failed."
    }

    $imported = 0
    foreach ($line in $envDump)
    {
        if ($line -match '^([^=]+)=(.*)$')
        {
            [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
            $imported++
        }
    }
    Ok "MSVC loaded ($imported env vars imported)"
} else
{
    Ok "MSVC detected: cl.exe"
}

# =============================================================================
# Clean
# =============================================================================
if ($Clean)
{
    if (Test-Path $BuildDir)
    {
        Log "Cleaning $BuildDir..."
        Remove-Item -Recurse -Force $BuildDir
        Ok "Clean complete"
    } else
    { Warn "Build directory doesn't exist, nothing to clean"
    }
}

# =============================================================================
# Configure
# =============================================================================
Hdr "Configure"
$configStart = Get-Date

& cmake @CmakeArgs 2>&1 | ForEach-Object { Write-Host "  $esc[2m$_$esc[0m" }
if ($LASTEXITCODE -ne 0)
{ Die "CMake configuration failed (exit code $LASTEXITCODE)"
}

$configTime = [math]::Round(((Get-Date) - $configStart).TotalSeconds, 1)
Ok "Configured in ${configTime}s"

if ($ConfigureOnly)
{ Write-Host ""; Ok "Configure-only mode -- skipping build"; exit 0
}

# =============================================================================
# Build
# =============================================================================
Hdr "Build"
$buildArgs = @("--build", $BuildDir, "--config", $BuildType, "-j", $Jobs)
if ($Verbose)
{ $buildArgs += "--verbose"
}

$buildStart = Get-Date

& cmake @buildArgs 2>&1 | ForEach-Object {
    $line = $_
    if ($line -match "error[:\s]|Error")
    { Write-Host "  $esc[31m$line$esc[0m"
    } elseif ($line -match "warning[:\s]")
    { Write-Host "  $esc[33m$line$esc[0m"
    } elseif ($line -match "Building|Linking|Built target")
    { Write-Host "  $esc[32m$line$esc[0m"
    } else
    { Write-Host "  $esc[2m$line$esc[0m"
    }
}
if ($LASTEXITCODE -ne 0)
{ Die "Build failed (exit code $LASTEXITCODE)"
}

$buildTime = [math]::Round(((Get-Date) - $buildStart).TotalSeconds, 1)
Ok "Built in ${buildTime}s"

# =============================================================================
# Install (optional)
# =============================================================================
if ($Install)
{
    Hdr "Install"
    $installPrefix = if ($Prefix)
    { $Prefix
    } else
    { "C:\Program Files\Kairos"
    }
    Log "Installing to $installPrefix..."
    & cmake --install $BuildDir --config $BuildType 2>&1 | ForEach-Object { Write-Host "  $esc[2m$_$esc[0m" }
    if ($LASTEXITCODE -ne 0)
    { Die "Install failed"
    }
    Ok "Installed"
}

# =============================================================================
# Summary
# =============================================================================
Write-Host ""
Write-Host "  $esc[42m$esc[1m$esc[37m BUILD SUCCEEDED $esc[0m"
Write-Host ""
CfgLine "Build type"   $BuildType
CfgLine "Build dir"    $BuildDir
CfgLine "Config time"  "${configTime}s"
CfgLine "Build time"   "${buildTime}s"

$kairosBin = Join-Path $BuildDir "$BuildType\kairos.exe"
if (-not (Test-Path $kairosBin))
{ $kairosBin = Join-Path $BuildDir "kairos.exe"
}
if (Test-Path $kairosBin)
{ CfgLine "Binary" $kairosBin
}

if ($EnableTests -eq "ON")
{
    $testCount = (Get-ChildItem -Path $BuildDir -Recurse -Filter "test_*.exe" -ErrorAction SilentlyContinue).Count
    CfgLine "Test binaries" "$testCount executables"
}

Write-Host ""
Write-Host "  $esc[2mNext steps:$esc[0m"
Write-Host "    $esc[36m.\scripts\test.ps1$esc[0m                  # Run all tests"
Write-Host "    $esc[36m.\scripts\test.ps1 -Filter 'Sha256'$esc[0m # Filter by name"
Write-Host ""
