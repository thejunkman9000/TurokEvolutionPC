<#
  Builds Turok: Evolution (recompiled) from a clean checkout.

    .\build.ps1 -Iso "D:\path\Turok - Evolution NTSC-U.iso"

  Steps: extract main.dol from your disc image -> generate C from it ->
  build turok_core.dll (CMake + MSVC) -> build the MonoGame host.
  Requires: Python 3, Visual Studio 2022+/Build Tools with C++ (MSVC, CMake,
  Ninja), .NET 9 SDK. The MonoGame shader compiler (mgfxc) is installed
  locally into tools/bin/dotnet on first run.

  Output: host/TurokHost/bin/Release/net9.0-windows/TurokEvolution.exe
#>
param(
    [Parameter(Mandatory = $true)][string]$Iso,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root

function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

if (-not (Test-Path $Iso)) { throw "Disc image not found: $Iso" }

# 1. main.dol from the user's disc
Step 'Extracting main.dol'
New-Item -ItemType Directory -Force extracted | Out-Null
python -I tools/extract_dol.py $Iso extracted/main.dol
if ($LASTEXITCODE) { throw 'extract failed' }

# 2. recompile PPC -> C
Step 'Generating C (ppcrecomp)'
python -I tools/recomp/ppcrecomp.py extracted/main.dol decomp/config/GTKE51/symbols.txt recomp/hle_overrides.txt recomp/generated
if ($LASTEXITCODE) { throw 'code generation failed' }

# 3. native core with MSVC (vcvars located through vswhere)
Step 'Building turok_core.dll'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools not found (install "Desktop development with C++" or the Build Tools)' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
if ($Clean -and (Test-Path recomp/build)) { Remove-Item -Recurse -Force recomp/build }
$cmd = "call `"$vcvars`" >nul && set `"PATH=$cmakeDir\Ninja;$cmakeDir\CMake\bin;%PATH%`" && " +
       "cmake -S recomp -B recomp\build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C recomp\build"
cmd /c $cmd
if ($LASTEXITCODE) { throw 'native build failed' }

# 4. shader compiler + MonoGame host
if (-not (Test-Path tools/bin/dotnet/mgfxc.exe)) {
    Step 'Installing MonoGame shader compiler (mgfxc)'
    dotnet tool install dotnet-mgfxc --version 3.8.6 --tool-path tools/bin/dotnet
    if ($LASTEXITCODE) { throw 'mgfxc install failed' }
}
Step 'Building the MonoGame host'
dotnet build host/TurokHost -c Release
if ($LASTEXITCODE) { throw 'host build failed' }

$exe = Join-Path $root 'host\TurokHost\bin\Release\net9.0-windows\TurokEvolution.exe'
Write-Host ''
Write-Host "Done: $exe" -ForegroundColor Green
Write-Host "Run:  & `"$exe`" `"$Iso`" [--fullscreen] [--scale 2]"
