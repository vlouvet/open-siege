# Building on Linux (x86_64)

Validated on **Ubuntu 26.04 LTS "Resolute Raccoon"** with gcc 15.2, Conan 1.66,
CMake 3.22.0 (Conan-managed; system CMake 4.2.3 is too new — see below).

Only the `3space` library and `dts-viewer` example have been tested.
`siege-installer`, `siege-launcher`, and `siege-studio` are not part of this build.

## Prerequisites

```sh
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake git python3-pip python3-venv \
    libsdl2-dev libgl1-mesa-dev libglu1-mesa-dev libglew-dev \
    pkg-config ninja-build
```

Expected versions on Ubuntu 26.04:
- gcc 15.2
- system cmake 4.2.3 (too new — the Conan workaround below is required)
- libsdl2-dev 2.32.x
- libglew-dev 2.2.0

## One-time Python / Conan setup

```sh
python3 -m venv ~/.venv
source ~/.venv/bin/activate
pip install 'conan==1.66'     # must be v1; v2 is incompatible with this codebase
conan --version               # should print "Conan version 1.66.0"
```

## One-time Conan profile setup

Conan 1.66's built-in compiler list stops at gcc 14. Ubuntu 26.04 ships gcc 15.
Add it to `~/.conan/settings.yml` and create a default profile:

```sh
# Add gcc 15 to settings.yml
sed -i 's/"14", "14.1"]/\0, "15", "15.1", "15.2"]/' ~/.conan/settings.yml

# Auto-detect the profile (gcc 15, libstdc++11, x86_64)
conan profile new default --detect
conan profile update settings.compiler.libcxx=libstdc++11 default

# Verify
conan profile show default
```

Expected profile output:
```
[settings]
os=Linux
os_build=Linux
arch=x86_64
arch_build=x86_64
compiler=gcc
compiler.version=15
compiler.libcxx=libstdc++11
build_type=Release
```

## CMake 4.x workaround

Ubuntu 26.04's packaged CMake is 4.x, which rejects the `cmake_minimum_required`
calls in transitive Conan recipe dependencies (bzip2, OpenSSL, etc.). Conan
fetches its own CMake 3.22.0 during the first `conan install`. Use that:

```sh
export CMAKE322="$(ls -d ~/.conan/data/cmake/3.22.0/_/_/package/*/bin | head -1)"
export PATH="$CMAKE322:$PATH"
cmake --version   # should print "cmake version 3.22.0"
```

Add this export to your shell profile or repeat it each session before building.

## Build `3space`

```sh
cd ~/open-siege/3space

# Fetch dependencies (downloads + builds all Conan packages on first run;
# ~15 minutes on a 2-core VM, ~5 minutes on 4+ cores)
conan install . --install-folder=build --build=missing

# Configure (use cmake 3.22 from PATH)
cmake -S . -B build -DCMAKE_MODULE_PATH=$(pwd)/build/cmake -DCMAKE_BUILD_TYPE=Release

# Build lib3space.a (~3 min on 2 cores)
cmake --build build --target 3space -j$(nproc)

# Build the cscript VM library (optional; required for dts-viewer)
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
cd ~/open-siege/examples/dts-viewer

cmake -S . -B build
cmake --build build -j$(nproc)
```

Expected output:
```
[100%] Built target dts-viewer
```

## Build all three binaries (track 26)

After `3space` is built, the dts-viewer plus the new
`open-siege-t1-client` / `open-siege-t1-server` binaries can be built in
one shot via the top-level CMakeLists option:

```sh
cd ~/open-siege
cmake -S . -B build \
    -DOPEN_SIEGE_BUILD_UPSTREAM=OFF \
    -DOPEN_SIEGE_BUILD_APPS=ON \
    -DCMAKE_MODULE_PATH=$(pwd)/3space/build/cmake
cmake --build build -j$(nproc)
# Binaries at:
#   build/examples/dts-viewer/dts-viewer
#   build/apps/open-siege-t1-client/open-siege-t1-client
#   build/apps/open-siege-t1-server/open-siege-t1-server
#   build/examples/net-test-client/net-test-client
```

Verify:
```sh
./build/dts-viewer
# prints: usage: ./build/dts-viewer <path-to-vol> ...
```

## Running against Tribes 1.41 assets

Copy your Tribes 1.41 freeware install's `base/` directory to a reachable path.
The viewer accepts a VOL file as its first argument:

```sh
./build/dts-viewer /path/to/tribes-game/base/Entities.vol larmor
```

A display server (X11 or Wayland via XWayland) is required for the SDL2 window.
On a headless server, set `DISPLAY=:0` or use a virtual framebuffer (`Xvfb`).

## Troubleshooting

### gcc version not found by Conan
If `conan install` fails with `Invalid setting '15' is not a valid 'settings.compiler.version'`:
```sh
grep '"14", "14.1"' ~/.conan/settings.yml
# If the sed above didn't work, edit manually to append "15", "15.1", "15.2"
# after "14.1" in the gcc version list
```

### CMake 4.x rejects old recipes
If cmake fails with `cmake_minimum_required` policy errors, the system CMake
is being used instead of Conan's cmake 3.22. Re-run `export PATH=...` (see
CMake 4.x workaround above) and retry.

### `libstdc++.so.6: GLIBCXX_3.4.XX not found`
If the binary fails on another machine: that machine has an older glibc/libstdc++.
Use the AppImage distribution (spec 04) which bundles its own C++ runtime.

### Conan v2 accidentally installed
If `conan install` fails with `from conans import ...` errors:
```sh
pip show conan   # check version
pip install 'conan==1.66' --force-reinstall
```

## Notes

- Ubuntu 24.04 LTS is an older but also supported target. The build recipe is
  identical; apt package versions differ slightly (libsdl2-dev 2.28.x, gcc 13).
- Ubuntu 26.04 LTS is the primary validation target for this recipe.
- AppImage build deps (`linuxdeploy`, `appimagetool`) are covered in
  `packaging/linux/README.md` (spec 04).
- The build was developed in a 2-CPU / 4 GB RAM VM; expect ~20 minutes total
  on first run (Conan package compilation). Subsequent builds are fast.
