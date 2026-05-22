# packaging/windows/build-zip.ps1
#
# Produces Open-Siege-win64.zip from a completed UCRT64 build.
#
# Usage (from repo root, in a PowerShell or pwsh session):
#   PowerShell -ExecutionPolicy Bypass -File packaging\windows\build-zip.ps1
#
# Preconditions:
#   - 3space was built: 3space/build/lib3space.a + cscript/libcscript_core.a
#   - dts-viewer was built in UCRT64 shell (cmake + cmake --build)
#   - MSYS2 UCRT64 is installed at C:\msys64

param(
    [string]$BuildDir = "$PSScriptRoot\..\..\examples\dts-viewer\build",
    [string]$OutputDir = "$PSScriptRoot\build"
)

$ErrorActionPreference = 'Stop'

$ZipName   = 'Open-Siege-win64'
$StageDir  = Join-Path $OutputDir $ZipName
$ZipPath   = Join-Path $OutputDir "$ZipName.zip"
$Exe       = Join-Path $BuildDir 'dts-viewer.exe'
$Ucrt64Bin = 'C:\msys64\ucrt64\bin'

if (-not (Test-Path $Exe)) {
    Write-Error "dts-viewer.exe not found at: $Exe`nRun the UCRT64 cmake build first."
}

# Runtime DLLs required by a UCRT64 MinGW-w64 build of dts-viewer.
# This list was derived from: ldd build/dts-viewer.exe (filtered to non-system DLLs)
$RuntimeDlls = @(
    'SDL2.dll',
    'glew32.dll',
    'libgcc_s_seh-1.dll',
    'libstdc++-6.dll',
    'libwinpthread-1.dll'
)

Write-Host "Staging to: $StageDir"
if (Test-Path $StageDir) {
    Remove-Item -Recurse -Force $StageDir
}
New-Item -ItemType Directory -Force $StageDir | Out-Null

Copy-Item $Exe $StageDir

foreach ($dll in $RuntimeDlls) {
    $src = Join-Path $Ucrt64Bin $dll
    if (Test-Path $src) {
        Write-Host "  bundling $dll"
        Copy-Item $src $StageDir
    } else {
        Write-Warning "  NOT FOUND in ${Ucrt64Bin}: $dll (skipping)"
    }
}

# Verify no msys-2.0.dll snuck in (MSYS2 native env leak)
if (Test-Path (Join-Path $StageDir 'msys-2.0.dll')) {
    Write-Error "msys-2.0.dll found in bundle -- rebuild in the UCRT64 shell, not the MSYS2 native shell."
}

$repoRoot = Resolve-Path "$PSScriptRoot\..\.."
Copy-Item (Join-Path $PSScriptRoot 'README-Windows.txt') $StageDir
$licPath = Join-Path $repoRoot 'LICENSE'
if (Test-Path $licPath) {
    Copy-Item $licPath $StageDir
}

if (Test-Path $ZipPath) {
    Remove-Item $ZipPath
}
Compress-Archive -Path "$StageDir\*" -DestinationPath $ZipPath
Write-Host "Created: $ZipPath"

$sizeMb = [math]::Round((Get-Item $ZipPath).Length / 1MB, 1)
Write-Host "Zip size: ${sizeMb} MB"
if ($sizeMb -gt 80) {
    Write-Warning "Zip exceeds 80 MB -- check that no Tribes game data was bundled."
}
