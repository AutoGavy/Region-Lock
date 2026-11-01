# Copyright (C) 2026 AutoGavy
# SPDX-License-Identifier: AGPL-3.0-only

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$SkipTests,
    [switch]$NoDeploy
)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildDirectory = Join-Path $repoRoot 'build'
$distributionDirectory = Join-Path $repoRoot 'dist'
$buildExecutable = Join-Path $buildDirectory "$Configuration\RegionLock.exe"
$distributionExecutable = Join-Path $distributionDirectory 'RegionLock.exe'
$catalogSource = Join-Path $repoRoot 'assets\ip-ranges.json'
if (-not (Test-Path -LiteralPath $catalogSource -PathType Leaf)) {
    throw 'The bundled catalog assets/ip-ranges.json was not found.'
}

# Never terminate an application to make a build or deployment succeed.
$running = @(Get-Process -Name RegionLock -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw 'RegionLock.exe is running. Close it before rebuilding or replacing the package.'
}

$vswhereCandidates = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
    (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
)
$vswherePath = $vswhereCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $vswherePath) { throw 'vswhere.exe was not found. Install Visual Studio with Desktop development with C++.' }
$installationPath = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installationPath) { throw 'A Visual Studio C++ installation was not found.' }
$versionText = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
$vsMajor = ([version]$versionText).Major
$generator = switch ($vsMajor) {
    18 { 'Visual Studio 18 2026' }
    17 { 'Visual Studio 17 2022' }
    default { throw "Unsupported Visual Studio version: $versionText. Use Visual Studio 2022 or newer." }
}

$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmakePath = if ($cmakeCommand) { $cmakeCommand.Source } else { Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe' }
if (-not (Test-Path -LiteralPath $cmakePath -PathType Leaf)) { throw 'CMake was not found.' }
$ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctestPath -PathType Leaf)) { throw 'ctest.exe was not found beside cmake.exe.' }

function Invoke-Checked {
    param([string]$Executable, [string[]]$Arguments, [string]$Label)
    $commandOutput = & $Executable @Arguments 2>&1
    $commandExitCode = $LASTEXITCODE
    if ($commandExitCode -ne 0) {
        $commandOutput | ForEach-Object { Write-Host $_ }
        throw "$Label failed (exit $commandExitCode)."
    }
    $warningLines = @($commandOutput | Where-Object { [string]$_ -match '(?i)\bwarning\b|警告' } | Select-Object -Unique)
    if ($warningLines.Count -gt 0) {
        Write-Host "${Label} warnings:"
        $warningLines | ForEach-Object { Write-Host $_ }
    }
    Write-Host "$Label succeeded."
}

Invoke-Checked -Executable $cmakePath -Arguments @(
    '-S', $repoRoot, '-B', $buildDirectory, '-G', $generator, '-A', 'x64',
    "-DCMAKE_GENERATOR_INSTANCE=$installationPath", '-DBUILD_TESTING=ON'
) -Label 'Configure'
Invoke-Checked -Executable $cmakePath -Arguments @(
    '--build', $buildDirectory, '--config', $Configuration, '--parallel'
) -Label 'Build'
# Seed the development executable and UI smoke target from the same catalog
# that will ship in dist, even when no C++ sources changed on this invocation.
Copy-Item -LiteralPath $catalogSource -Destination (Join-Path $buildDirectory "$Configuration\ip-ranges.json") -Force
if (-not $SkipTests) {
    Invoke-Checked -Executable $ctestPath -Arguments @(
        '--test-dir', $buildDirectory, '-C', $Configuration, '--output-on-failure'
    ) -Label 'Tests'
}
if (-not $NoDeploy) {
    # Recheck immediately before copying in case it started while compiling.
    if (Get-Process -Name RegionLock -ErrorAction SilentlyContinue) {
        throw 'RegionLock.exe started during the build. Close it before deployment.'
    }
    New-Item -ItemType Directory -Path $distributionDirectory -Force | Out-Null
    Copy-Item -LiteralPath $buildExecutable -Destination $distributionExecutable -Force
    Copy-Item -LiteralPath $catalogSource -Destination (Join-Path $distributionDirectory 'ip-ranges.json') -Force
    Copy-Item -LiteralPath (Join-Path $repoRoot 'README.md') -Destination (Join-Path $distributionDirectory 'README.md') -Force
    Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $distributionDirectory 'LICENSE') -Force
    $licenseDirectory = Join-Path $distributionDirectory 'licenses'
    New-Item -ItemType Directory -Path $licenseDirectory -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot 'third_party\yyjson\LICENSE') -Destination (Join-Path $licenseDirectory 'yyjson-LICENSE.txt') -Force
    Write-Host "Package: $distributionExecutable"
}
