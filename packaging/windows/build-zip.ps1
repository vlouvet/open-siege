# packaging/windows/build-zip.ps1
#
# Produces Open-Siege-win64.zip from a completed UCRT64 build.
#
# Usage (from repo root, in a PowerShell or pwsh session):
#   packaging\windows\build-zip.ps1
#
# Or with explicit build dir:
#   packaging\windows\build-zip.ps1 -BuildDir C:\path\to\examples\dts-viewer\build
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

$ZipName    = 'Open-Siege-win64'
$StageDir   = Join-Path $OutputDir $ZipName
$ZipPath    = Join-Path $OutputDir "$ZipName.zip"
$Exe        = Join-Path $BuildDir 'dts-viewer.exe'
$Ucrt64Bin  = 'C:\msys64\ucrt64\bin'

# Validate inputs
if (-not (Test-Path $Exe)) {
    Write-Error "dts-viewer.exe not found at: $Exe`nRun the UCRT64 cmake build first."
}

# Discover runtime DLL deps via objdump (ships with MSYS2 binutils)
function Get-RuntimeDlls {
    param([string]$ExePath)
    $objdump = 'C:\msys64\ucrt64\bin\objdump.exe'
    if (-not (Test-Path $objdump)) {
        Write-Error "objdump not found at $objdump — install mingw-w64-ucrt-x86_64-binutils"
    }
    $output = & $objdump -p $ExePath 2>&1
    $dlls = @()
    foreach ($line in $output) {
        if ($line -match '^\s+DLL Name:\s+(.+)$') {
            $dlls += $Matches[1].Trim()
        }
    }
    return $dlls
}

# System DLLs that must NOT be bundled (Windows provides them)
$SystemDlls = @(
    'KERNEL32.DLL','KERNELBASE.DLL','msvcrt.dll','ntdll.dll',
    'USER32.DLL','GDI32.DLL','ADVAPI32.DLL','SHELL32.DLL',
    'ole32.dll','OLEAUT32.DLL','SHLWAPI.DLL','WS2_32.DLL',
    'WINMM.DLL','DBGHELP.DLL','PSAPI.DLL',
    'OPENGL32.DLL','SETUPAPI.DLL','IMM32.DLL','VERSION.DLL',
    'api-ms-win-*.dll','ext-ms-win-*.dll'
)

function Is-SystemDll {
    param([string]$DllName)
    $upper = $DllName.ToUpper()
    foreach ($pattern in $SystemDlls) {
        if ($upper -like $pattern.ToUpper()) { return $true }
    }
    # UCRTBase / vcruntime — these come from Windows 10+ universal CRT
    if ($upper -like 'UCRTBASE.DLL') { return $true }
    if ($upper -like 'VCRUNTIME*.DLL') { return $true }
    return $false
}

Write-Host "Staging to: $StageDir"
if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
New-Item -ItemType Directory -Force $StageDir | Out-Null

# Copy the executable
Copy-Item $Exe $StageDir

# Discover and copy runtime DLLs from UCRT64
$deps = Get-RuntimeDlls $Exe
Write-Host "Runtime DLL deps: $($deps -join ', ')"

foreach ($dll in $deps) {
    if (Is-SystemDll $dll) { continue }
    $src = Join-Path $Ucrt64Bin $dll
    if (Test-Path $src) {
        Write-Host "  bundling $dll"
        Copy-Item $src $StageDir
    } else {
        Write-Warning "  NOT FOUND in ${Ucrt64Bin}: $dll (skipping)"
    }
}

# Copy user-facing README and LICENSE
$repoRoot = Resolve-Path "$PSScriptRoot\..\.."
Copy-Item (Join-Path $PSScriptRoot 'README-Windows.txt') $StageDir
Copy-Item (Join-Path $repoRoot 'LICENSE') $StageDir -ErrorAction SilentlyContinue

# Verify no msys-2.0.dll snuck in (MSYS2 native env leak)
if (Test-Path (Join-Path $StageDir 'msys-2.0.dll')) {
    Write-Error "msys-2.0.dll found in bundle — rebuild in the UCRT64 shell, not the MSYS2 native shell."
}

# Zip it
if (Test-Path $ZipPath) { Remove-Item $ZipPath }
Compress-Archive -Path "$StageDir\*" -DestinationPath $ZipPath
Write-Host "Created: $ZipPath"

# Report size
$sizeMb = [math]::Round((Get-Item $ZipPath).Length / 1MB, 1)
Write-Host "Zip size: ${sizeMb} MB"
if ($sizeMb -gt 80) {
    Write-Warning "Zip exceeds 80 MB — check that no Tribes game data was bundled."
}
