<#
.SYNOPSIS
Builds and installs ArmoniK.Api (client) and the ArmoniK SDK (Client + Common) with MSVC.

.DESCRIPTION
Runs the same steps as the build-windows-msvc CI job. gRPC, protobuf, abseil,
nlohmann_json and zlib come from a conda environment created from
tools/windows/environment.yml, or from any other CMake prefix passed with
-DepsCMakePrefix (vcpkg, source builds). ArmoniK.Api and the SDK are installed into -Prefix.

Requirements: Visual Studio 2022 (C++ workload), CMake >= 3.22, git, and either
micromamba/conda or your own install of the dependencies.

.EXAMPLE
micromamba create -f tools\windows\environment.yml
micromamba activate armonik-sdk-windows
.\tools\windows\build.ps1 -Prefix C:\armonik

Then in your project: -DCMAKE_PREFIX_PATH="C:\armonik;$env:CONDA_PREFIX\Library"
and find_package(ArmoniK.SDK.Client CONFIG REQUIRED).

.EXAMPLE
.\tools\windows\build.ps1 -Prefix C:\armonik -DepsCMakePrefix C:\vcpkg\installed\x64-windows
#>
[CmdletBinding()]
param(
    # Install prefix for ArmoniK.Api and the SDK. Default: <repo>\install-windows
    [string]$Prefix,
    # Root of the conda environment providing the dependencies. Default: $env:CONDA_PREFIX
    [string]$DepsPrefix,
    # CMake prefix providing the dependencies, used as is (e.g. <vcpkg>\installed\x64-windows).
    # Overrides -DepsPrefix. Its bin\ directory, if any, is put on PATH.
    [string]$DepsCMakePrefix,
    # Build directory. Default: <repo>\build-windows
    [string]$WorkDir,
    # ArmoniK.Api tag to build. Default: ARMONIK_API_VERSION_DEFAULT from tools/common.sh
    [string]$ApiVersion,
    # Existing ArmoniK.Api checkout. Default: clone $ApiVersion into $WorkDir
    [string]$ApiSource,
    # SDK version. Default: ARMONIK_SDK_VERSION_DEFAULT from tools/common.sh
    [string]$Version,
    # Debug is not offered: conda-forge ships release (/MD) binaries, mixing them with /MDd breaks the STL ABI
    [ValidateSet('Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Release',
    # Build and run a small downstream project against the installed SDK
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Works on Windows PowerShell 5.1, which ignores $PSNativeCommandUseErrorActionPreference
function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments)
    Write-Host "> $Exe $($Arguments -join ' ')"
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe exited with code $LASTEXITCODE" }
}

function Get-CommonShDefault([string]$Name) {
    $content = Get-Content (Join-Path $RepoRoot 'tools\common.sh') -Raw
    if ($content -notmatch "$Name=`"([^`"]+)`"") { throw "$Name not found in tools/common.sh" }
    return $Matches[1]
}

function Invoke-CMakeProject {
    param([string]$Source, [string]$Build, [string[]]$Options, [switch]$Install)
    Invoke-Native cmake (@('-S', $Source, '-B', $Build) + $Options)
    Invoke-Native cmake @('--build', $Build, '--config', $Config, '--parallel')
    if ($Install) { Invoke-Native cmake @('--install', $Build, '--config', $Config, '--prefix', $Prefix) }
}

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

if ($DepsCMakePrefix) {
    $DepsCMake = (Resolve-Path $DepsCMakePrefix).Path
} else {
    if (-not $DepsPrefix) { $DepsPrefix = $env:CONDA_PREFIX }
    if (-not $DepsPrefix) { throw 'No dependency environment: activate it, or pass -DepsPrefix or -DepsCMakePrefix' }
    # conda on Windows installs C++ libraries under <env>\Library
    $DepsCMake = Join-Path (Resolve-Path $DepsPrefix).Path 'Library'
    if (-not (Test-Path (Join-Path $DepsCMake 'bin\grpc_cpp_plugin.exe'))) {
        throw "grpc_cpp_plugin.exe not found in $DepsCMake\bin, was the environment created from tools\windows\environment.yml?"
    }
}
# protoc and grpc_cpp_plugin need the environment DLLs at build time, the -Verify executable at run time
$env:PATH = "$DepsCMake\bin;$env:PATH"

if (-not $ApiVersion) { $ApiVersion = Get-CommonShDefault 'ARMONIK_API_VERSION_DEFAULT' }
if (-not $Version) { $Version = Get-CommonShDefault 'ARMONIK_SDK_VERSION_DEFAULT' }
if (-not $Prefix) { $Prefix = Join-Path $RepoRoot 'install-windows' }
if (-not $WorkDir) { $WorkDir = Join-Path $RepoRoot 'build-windows' }
New-Item -ItemType Directory -Force -Path $Prefix, $WorkDir | Out-Null
$Prefix = (Resolve-Path $Prefix).Path
$WorkDir = (Resolve-Path $WorkDir).Path

if (-not $ApiSource) {
    $ApiSource = Join-Path $WorkDir "ArmoniK.Api-$ApiVersion"
    if (Test-Path $ApiSource) {
        Write-Host "Reusing $ApiSource"
    } else {
        Invoke-Native git @('clone', '--depth', '1', '--branch', $ApiVersion,
            'https://github.com/aneoconsulting/ArmoniK.Api.git', $ApiSource)
    }
}

Write-Host "ArmoniK.Api $ApiVersion, SDK $Version, $Config, prefix $Prefix"

Invoke-CMakeProject -Install -Source (Join-Path $ApiSource 'packages\cpp') -Build (Join-Path $WorkDir 'api') -Options @(
    '-DBUILD_CLIENT=ON', '-DBUILD_WORKER=OFF', '-DBUILD_TEST=OFF',
    "-DCMAKE_PREFIX_PATH=$DepsCMake")

Invoke-CMakeProject -Install -Source $RepoRoot -Build (Join-Path $WorkDir 'sdk') -Options @(
    "-DARMONIK_API_DIR=$Prefix", "-DCMAKE_PREFIX_PATH=$DepsCMake",
    '-DBUILD_WORKER=OFF', '-DBUILD_DYNAMICWORKER=OFF', "-DVERSION=$Version")

if ($Verify) {
    $consumerBuild = Join-Path $WorkDir 'consumer'
    Invoke-CMakeProject -Source (Join-Path $PSScriptRoot 'consumer') -Build $consumerBuild -Options @(
        "-DCMAKE_PREFIX_PATH=$Prefix;$DepsCMake")
    $reported = & (Join-Path $consumerBuild "$Config\armonik_sdk_consumer.exe")
    if ($LASTEXITCODE -ne 0) { throw "armonik_sdk_consumer.exe exited with code $LASTEXITCODE" }
    if ($reported -ne $Version) { throw "Consumer reports SDK version '$reported', expected '$Version'" }
    Write-Host "Consumer project built and ran against SDK $reported"
}

Write-Host "Done. Use -DCMAKE_PREFIX_PATH=`"$Prefix;$DepsCMake`" in your project."
