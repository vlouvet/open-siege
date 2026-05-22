# Building on Windows (x86_64)

Validated on **Windows 11** with MSYS2 UCRT64 GCC 15.2, Conan 1.66,
CMake 3.22.0 (Conan-managed; MSYS2's system CMake 4.2.3 is too new — see
CMake workaround below).

Only the `3space` library and `dts-viewer` example are covered here.
`siege-installer`, `siege-launcher`, and `siege-studio` are not part of
this build.

## Prerequisites — MSYS2 UCRT64 toolchain

1. Install [MSYS2](https://www.msys2.org/) (or via `winget install MSYS2.MSYS2`).

2. Open an **UCRT64** shell (`C:\msys64\ucrt64.exe`) and install the build
   dependencies:

```sh
pacman -S --needed \
    mingw-w64-ucrt-x86_64-toolchain \
    mingw-w64-ucrt-x86_64-cmake \
    mingw-w64-ucrt-x86_64-SDL2 \
    mingw-w64-ucrt-x86_64-glew \
    mingw-w64-ucrt-x86_64-python \
    mingw-w64-ucrt-x86_64-python-pip
```

Expected versions (as of 2026-05):
- GCC 15.2.0
- CMake 4.2.3 (too new — Conan workaround required; see below)
- SDL2 2.30.x
- GLEW 2.2.0
- Python 3.12.x

> **Why UCRT64 and not MINGW64?**
> UCRT64 links against the Windows Universal CRT (ucrt.dll), which is
> present on all Windows 10/11 systems. MINGW64 links against the older
> msvcrt.dll. UCRT64 binaries need fewer redistributable DLLs in the
> portable zip and have better Unicode handling.

> **Do not use the MSYS2 native shell** (the green icon) for builds — its
> compiler targets a POSIX-emulation layer and produces non-portable
> `msys-2.0.dll`-dependent binaries. Only the UCRT64 shell produces a
> native Windows binary.

## One-time Conan setup

All commands below run inside the **UCRT64** shell.

```sh
pip install 'conan==1.66'    # must be v1; v2 is incompatible with this codebase
conan --version              # should print "Conan version 1.66.0"
```

## One-time Conan profile setup

Conan 1.66's built-in compiler list does not include GCC 15. Add it and
create the default profile:

```sh
# Append GCC 15 entries to settings.yml
python - <<'PY'
import yaml, pathlib, os
p = pathlib.Path.home() / '.conan/settings.yml'
s = yaml.safe_load(p.read_text())
v = s['compiler']['gcc']['version']
for x in ('15', '15.1', '15.2'):
    if x not in v: v.append(x)
p.write_text(yaml.safe_dump(s, sort_keys=False))
PY

# Auto-detect: Conan sees gcc 15 and Windows, sets os=Windows, arch=x86_64
conan profile new default --detect
conan profile update settings.compiler.libcxx=libstdc++11 default
```

Expected `conan profile show default` output:
```
[settings]
os=Windows
os_build=Windows
arch=x86_64
arch_build=x86_64
compiler=gcc
compiler.version=15
compiler.libcxx=libstdc++11
build_type=Release
```

A checked-in profile copy lives at
`open-siege/packaging/windows/conan-profile-windows` for reference.

## CMake 4.x workaround

MSYS2's CMake 4.2.3 rejects the legacy `cmake_minimum_required` in
transitive Conan recipe dependencies. Conan fetches its own CMake 3.22.0
during the first `conan install`. Use that for all build steps:

```sh
export CMAKE322="$(ls -d ~/.conan/data/cmake/3.22.0/_/_/package/*/bin 2>/dev/null | head -1)"
export PATH="$CMAKE322:$PATH"
cmake --version   # should print "cmake version 3.22.0"
```

> **Note:** The cmake binary is only in `~/.conan/data/cmake/` after a first
> `conan install` run. If the path is empty, run `conan install` once first
> (it will fail on bzip2, but cmake will be downloaded). Then set the PATH and
> retry.

Add this export to `~/.bashrc` or repeat it each UCRT64 session.

## Build `3space`

```sh
cd ~/src/open-siege/3space   # or wherever you cloned the repo

# Bootstrap: fetch cmake 3.22.0 into the Conan cache before building packages.
# The system CMake 4.x breaks transitive recipes (bzip2, etc.) so Conan's own
# cmake 3.22 must be on PATH when packages are compiled.
conan install cmake/3.22.0@ --build=missing

# Put cmake 3.22.0 first on PATH for this shell session
export CMAKE322="$(ls -d ~/.conan/data/cmake/3.22.0/_/_/package/*/bin 2>/dev/null | head -1)"
export PATH="$CMAKE322:$PATH"
cmake --version   # should print "cmake version 3.22.0"

# Fetch and build all 3space dependencies (~15-25 min on first run)
mkdir -p build
conan install . --install-folder=build --build=missing

# Configure (use cmake 3.22 from PATH)
cmake -S . -B build -DCMAKE_MODULE_PATH=$(pwd)/build/cmake -DCMAKE_BUILD_TYPE=Release

# Build lib3space.a and the cscript VM (~5 min on 4 cores)
cmake --build build --target 3space -j$(nproc)
cmake --build build --target cscript_core -j$(nproc)
```

Expected output:
```
[100%] Built target 3space
[100%] Built target cscript_core
```

Files produced:
- `build/lib3space.a`
- `build/cscript/libcscript_core.a`

## Build `dts-viewer`

```sh
cd ~/src/open-siege/examples/dts-viewer

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DSDL2_DIR=/ucrt64/lib/cmake/SDL2 \
    -DGLEW_DIR=/ucrt64/lib/cmake/glew
cmake --build build -j$(nproc)
```

Expected output:
```
[100%] Built target dts-viewer
```

Verify:
```sh
./build/dts-viewer.exe
# prints: usage: ./build/dts-viewer.exe <path-to-vol> ...
```

## Running against Tribes 1.41 assets

The viewer prompts for the Tribes 1.41 install directory on first run and
caches the path in `%APPDATA%\open-siege\config.toml`.

```sh
./build/dts-viewer.exe --tribes-dir 'C:/Games/Tribes' Entities.vol larmor
```

Or without `--tribes-dir`, the viewer shows an interactive prompt.

## Troubleshooting

### GCC version not found by Conan
If `conan install` fails with `Invalid setting '15' is not a valid 'settings.compiler.version'`:
- Re-run the `python - <<'PY' ...` snippet above to patch `settings.yml`.

### CMake 4.x rejects old recipes
If cmake fails with `cmake_minimum_required` policy errors, the MSYS2
system CMake is ahead of PATH. Re-export `CMAKE322` (see above) and retry.

### Missing SDL2 or GLEW at runtime
The viewer needs `SDL2.dll` and `glew32.dll` at runtime. Copy them from
`C:\msys64\ucrt64\bin\` next to `dts-viewer.exe`, or use the portable zip
produced by `packaging/windows/build-zip.ps1` (spec 03).

### `msys-2.0.dll` dependency
If `ntldd dts-viewer.exe` or `objdump -p dts-viewer.exe` lists `msys-2.0.dll`,
the binary was built in the MSYS2 native shell instead of the UCRT64 shell.
Rebuild in the UCRT64 shell (`C:\msys64\ucrt64.exe`).

### Conan v2 accidentally installed
```sh
pip show conan
pip install 'conan==1.66' --force-reinstall
```

## Notes

- The Torque3D-derived cscript platform layer uses MinGW-w64's POSIX
  shims (`<unistd.h>`, `<sys/stat.h>`, etc.) rather than a vendored
  `platformWin32/`. MinGW-w64's UCRT64 provides sufficient POSIX
  compatibility for the VM runtime.
- The `execv` calls in `dts-viewer/main.cpp` (mission-switch) map to
  `_execv` on Windows, which creates a child process and exits — same
  observable behaviour as POSIX exec.
- Mission switching is fully functional; it re-launches the viewer into
  a new mission file.
- AppImage packaging (spec 23/04) is Linux-only. The Windows equivalent
  is the portable zip from `packaging/windows/build-zip.ps1` (spec 03).
