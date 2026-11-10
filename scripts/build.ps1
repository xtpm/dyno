[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$Run
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer was not found. Install Visual Studio with Desktop development with C++."
}

# Any VS 2022+ edition works, including Build Tools and Preview installs.
$installationPath = & $vswhere -latest -prerelease -products * `
    -version "[17.0," `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath |
    Select-Object -First 1

if (-not $installationPath) {
    throw "MSVC was not found. Install Visual Studio 2022 or newer (Community or Build Tools) with the Desktop development with C++ workload."
}

$sdkInclude = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include"
$sdks = @(Get-ChildItem -LiteralPath $sdkInclude -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^10\.0\.(\d+)\.' -and [int]$Matches[1] -ge 20348 })
if ($sdks.Count -eq 0) {
    throw "A Windows SDK 10.0.20348 or newer was not found. Add a Windows 10 or Windows 11 SDK in Visual Studio Installer."
}

$msbuild = Join-Path $installationPath "MSBuild\Current\Bin\MSBuild.exe"
$project = Join-Path $projectRoot "DynamicIsland.vcxproj"

& $msbuild $project /m /restore `
    "/p:Configuration=$Configuration" `
    "/p:Platform=x64" `
    /verbosity:minimal

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

$executable = Join-Path $projectRoot "build\$Configuration\DynamicIsland.exe"
Write-Host "Built: $executable"

if ($Run) {
    Start-Process -FilePath $executable
}
