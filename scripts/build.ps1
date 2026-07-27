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

$installationPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath

if (-not $installationPath) {
    throw "MSVC was not found. Add the Desktop development with C++ workload in Visual Studio Installer."
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
