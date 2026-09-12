<#
.SYNOPSIS
    Fast local build of the 3.4.3 server (worldserver + bnetserver + AI playerbots).

.DESCRIPTION
    Wraps the CMake configure + build steps with the switches that matter for
    wall-clock time on a Windows / MSVC machine:

      * Ninja instead of MSBuild  - Ninja keeps every core busy across project
        boundaries (MSBuild only parallelises inside one project) and skips the
        per-project overhead.  It also lets us drop /MP, which would otherwise
        oversubscribe the machine on top of Ninja's own parallelism.
      * sccache / ccache          - reuses object files from previous builds.
      * unity build (optional)    - merges the 258 small playerbot sources into
        batches so the core headers are parsed ~8x less often.
      * PCH                       - always on (USE_COREPCH / USE_SCRIPTPCH).
        Never turn these off: the core headers depend on the PCH include set.

    Everything here is optional - the normal
        cmake -B build -S . -G "Visual Studio 17 2022" -A x64 ...
        cmake --build build --config RelWithDebInfo
    flow keeps working unchanged.

.EXAMPLE
    .\Build-Fast.ps1
    Full rebuild with the defaults (Ninja, RelWithDebInfo, all cores).

.EXAMPLE
    .\Build-Fast.ps1 -Unity -Cores 6
    Also merge the playerbot sources into unity translation units.

.EXAMPLE
    .\Build-Fast.ps1 -Generator "Visual Studio 17 2022"
    Same options, but with the MSBuild generator.

.EXAMPLE
    .\Build-Fast.ps1 -Clean
    Throw the build directory away first.
#>

[CmdletBinding()]
param(
    [ValidateSet("RelWithDebInfo", "Release", "Debug")]
    [string] $Config = "RelWithDebInfo",

    [string] $Generator = "Ninja",

    [string] $BuildDir = "build-fast",

    # Number of parallel compile jobs.  Defaults to all cores but one, which
    # leaves a core for the UI - on a small machine that is worth more than the
    # last few percent of build throughput.
    [int] $Cores = 0,

    [switch] $Unity,

    # Use /Z7 debug info instead of /Zi: no shared mspdbsrv.exe bottleneck.
    [switch] $FastDebugInfo,

    # Link with /DEBUG:FASTLINK: much faster links for worldserver.
    [switch] $FastLink,

    [string] $CompilerCache = "AUTO",

    [switch] $Clean,

    [switch] $Reconfigure,

    [switch] $SkipConfigure
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

function Write-Step([string] $message) {
    Write-Host ""
    Write-Host "=== $message ===" -ForegroundColor Cyan
}

function Find-VsDevCmd {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found - is Visual Studio 2022 installed?"
    }

    $installPath = & $vswhere -latest `
        -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath

    if (-not $installPath) {
        throw "No Visual Studio installation with the C++ x64 toolset was found."
    }

    $vcvars = Join-Path $installPath "VC\Auxiliary\Build\vcvarsall.bat"
    if (-not (Test-Path $vcvars)) {
        throw "vcvarsall.bat not found under '$installPath'."
    }

    return $vcvars
}

if ($Cores -le 0) {
    $Cores = [math]::Max(1, $env:NUMBER_OF_PROCESSORS - 1)
}

$buildPath = Join-Path $root $BuildDir
if ($Clean -and (Test-Path $buildPath)) {
    Write-Step "Removing $buildPath"
    Remove-Item -Recurse -Force $buildPath
}

$cmakeArgs = @(
    "-S", "`"$root`"",
    "-B", "`"$buildPath`"",
    "-G", "`"$Generator`"",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DSERVERS=1",
    "-DTOOLS=0",
    "-DSCRIPTS=static",
    "-DWITH_DYNAMIC_LINKING=0",
    "-DUSE_COREPCH=1",
    "-DUSE_SCRIPTPCH=1",
    "-DWITH_WARNINGS=0",
    "-DWITH_COMPILER_CACHE=$CompilerCache",
    "-DCOPY_CONF=1"
)

if ($Unity)            { $cmakeArgs += "-DWITH_UNITY_BUILD=1" }
if ($FastDebugInfo)    { $cmakeArgs += "-DWITH_FAST_DEBUGINFO=1" }
if ($FastLink)         { $cmakeArgs += "-DWITH_FASTLINK=1" }

if ($Generator -match "Visual Studio") {
    $cmakeArgs += "-A", "x64"
}

$needConfigure = $Reconfigure -or -not (Test-Path (Join-Path $buildPath "CMakeCache.txt"))

# The compiler has to be on PATH for the Ninja generator; vcvarsall.bat sets
# that up (and everything else the MSVC toolchain needs).
$vcvars = Find-VsDevCmd
$configureLine = if ($needConfigure) { "cmake " + ($cmakeArgs -join " ") } else { "" }
$buildArgs = @("--build", "`"$buildPath`"", "--parallel", $Cores)
if ($Generator -match "Visual Studio" -or $Generator -match "Ninja Multi-Config") {
    $buildArgs += "--config", $Config
}
$buildLine = "cmake " + ($buildArgs -join " ")

$script = @()
if ($configureLine) { $script += $configureLine }
$script += $buildLine
$commandLine = ($script -join " && ")
$fullCommand = "`"$vcvars`" x64 && $commandLine"

Write-Step "Build settings"
Write-Host "  generator      : $Generator"
Write-Host "  config         : $Config"
Write-Host "  parallel jobs  : $Cores"
Write-Host "  unity build    : $([bool]$Unity)"
Write-Host "  /Z7 debug info : $([bool]$FastDebugInfo)"
Write-Host "  /DEBUG:FASTLINK: $([bool]$FastLink)"
Write-Host "  compiler cache : $CompilerCache"
Write-Host "  build dir      : $buildPath"

Write-Step "Building"
cmd /c $fullCommand
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

if ($Generator -match "Visual Studio" -or $Generator -match "Ninja Multi-Config") {
    $outDir = Join-Path $buildPath "bin\$Config"
}
else {
    $outDir = Join-Path $buildPath "bin"
}

Write-Step "Done"
Write-Host "  binaries: $outDir"
Write-Host "  Tip: run .\Build-Fast.ps1 again for incremental builds; add -Clean for a fresh tree."
