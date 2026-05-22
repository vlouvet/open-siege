# Windows portable zip — developer notes

## Prerequisites

- MSYS2 UCRT64 with the toolchain installed (see `BUILD-WINDOWS.md`)
- A completed `dts-viewer.exe` build (see `BUILD-WINDOWS.md`)

## Build the zip locally

From a PowerShell or cmd window at the repo root:

```powershell
pwsh packaging\windows\build-zip.ps1
```

Output: `packaging/windows/build/Open-Siege-win64.zip`

The script:
1. Copies `dts-viewer.exe` from `examples/dts-viewer/build/`
2. Discovers runtime DLL deps via `objdump -p` (binutils from UCRT64)
3. Copies non-system DLLs from `C:\msys64\ucrt64\bin\`
4. Adds `README-Windows.txt` and `LICENSE`
5. Zips everything into `Open-Siege-win64.zip`

## Testing the zip

On Windows: extract to a fresh folder and run `dts-viewer.exe`.

Under Wine on macOS/Linux:

```sh
wine dts-viewer.exe --help
```

## What NOT to bundle

- `opengl32.dll` — Windows ships it; bundling a third-party copy causes problems
- `msys-2.0.dll` — if this appears, the binary was built in the MSYS2 native
  shell; rebuild in the UCRT64 shell (`C:\msys64\ucrt64.exe`)
- Tribes game data — the zip must not contain any `.vol` / `.dts` files

## CI

The GitHub Actions workflow `.github/workflows/build-windows.yml` runs this
script on `windows-latest` runners. Artifacts are uploaded on every push to
`macos-arm64`; a GitHub Release is created automatically on `v*` tags.

See the CI section at the bottom of this file for the cache strategy.

## Cache strategy

Conan packages are cached with `actions/cache@v4`. The cache key includes
`${{ runner.os }}` to prevent Linux and Windows caches from colliding.

First-build time: ~25 minutes (Conan downloads + compiles boost, openssl, etc.).
Cache-hit build time: ~8 minutes.
