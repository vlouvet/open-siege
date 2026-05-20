# Building on macOS (Apple Silicon)

Validated on macOS 26 / Apple Silicon (arm64) with apple-clang 17, Conan 1.66, CMake 3.22.

Only the `3space` library subproject has been tested. `siege-installer`, `siege-launcher`, and `siege-studio` are Windows-centric and have not been ported.

## Prerequisites

```sh
brew install python git
python3 -m venv .venv
source .venv/bin/activate
pip install 'conan<2'         # Conan 2 is not compatible with this codebase
```

## One-time Conan setup

Apple-clang 17 is newer than Conan 1.66's built-in compiler list. Add it to `~/.conan/settings.yml`:

```sh
python3 - <<'PY'
import yaml, pathlib
p = pathlib.Path.home() / '.conan/settings.yml'
s = yaml.safe_load(p.read_text())
v = s['compiler']['apple-clang']['version']
for x in ('17', '17.0', '18', '18.0'):
    if x not in v: v.append(x)
p.write_text(yaml.safe_dump(s, sort_keys=False))
PY

conan profile new default --detect    # creates an armv8 / apple-clang profile
```

## Build `3space`

The recipe pulls a Conan-managed CMake 3.22.0, but it does not put it on `PATH`. Homebrew CMake 4.x will reject the legacy `cmake_minimum_required(VERSION 2.x)` in the bzip2 transitive recipe, so the Conan-managed CMake must be used:

```sh
cd 3space
mkdir -p build && cd build

# Use the Conan-provided CMake for everything in this shell.
export PATH="$(ls -d ~/.conan/data/cmake/3.22.0/_/_/package/*/bin | head -1):$PATH"

conan install .. --build=missing
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_MODULE_PATH="$(pwd)/cmake"
cmake --build . -j8
./3space-tests                 # 54 assertions
```

## What works

End-to-end validated against the official Tribes 1 freeware release:

- `darkstar_volume` parser: 90/90 VOL files listed without errors.
- `dts` parser (factory + `render_shape` walk): 209/209 DTS files across `Entities.vol` and `Editor.vol` parse cleanly, including detail levels, materials, sequences, and full geometry traversal.

## What is patched relative to upstream

- [3space/src/resources/cab_volume.cpp](3space/src/resources/cab_volume.cpp): one-line fix changing `auto&` to `const auto&` when iterating a `std::filesystem::path`. Apple-clang 17's libc++ returns the path iterator's reference as a prvalue, which cannot bind to a non-const lvalue reference.

## Known gaps

- `siege-installer`, `siege-launcher`, `siege-studio` not attempted on macOS.
- The Conan v1 toolchain is on a deprecation path; a future port to Conan v2 (and a non-bincrafters recipe set) is the long-term direction.
