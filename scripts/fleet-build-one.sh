#!/usr/bin/env bash
# Per-host build script — track 27 spec 02.
#
# Runs ON the remote host (or locally for the macOS-as-self case). The
# orchestrator scp's this file fresh each run so iterating on the build
# recipe doesn't require a per-box git pull first.
#
# Args:
#   $1  repo_path (absolute, native style for the remote OS)
#   $2  git ref to checkout (branch name or sha)
#   $3  OS tag: macos | linux | windows

set -euo pipefail

REPO="${1:?repo_path required}"
REF="${2:?git ref required}"
OS="${3:?os tag required}"

cd "$REPO/open-siege"

echo "[fleet-build-one $OS] git fetch + checkout $REF"
git fetch origin
git checkout "$REF"
git pull --ff-only origin "$REF" || true   # already-up-to-date is fine

# Activate venv + conan-managed cmake. The venv lives at parent level
# in our tree convention (~/code/tribes-emscripten/.venv).
if [ -f "$REPO/.venv/bin/activate" ]; then
    # shellcheck disable=SC1091
    source "$REPO/.venv/bin/activate"
fi

# Find conan-managed cmake 3.22 (Conan 1.66 recipes need it; system
# cmake 4.x rejects old cmake_minimum_required directives).
if [ -d "$HOME/.conan/data/cmake/3.22.0" ]; then
    CONAN_CMAKE_BIN=$(ls -d "$HOME"/.conan/data/cmake/3.22.0/_/_/package/*/bin 2>/dev/null | head -1 || true)
    [ -n "$CONAN_CMAKE_BIN" ] && export PATH="$CONAN_CMAKE_BIN:$PATH"
fi

cd 3space

echo "[fleet-build-one $OS] conan install (if needed) + 3space build"
case "$OS" in
    macos|linux)
        # First-time setup needs conan install; subsequent runs are no-ops.
        if [ ! -f build/cmake/conanbuildinfo.cmake ] && [ ! -f build/conanbuildinfo.cmake ]; then
            conan install . --install-folder=build --build=missing >/dev/null
        fi
        cmake -S . -B build \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_MODULE_PATH="$(pwd)/build/cmake"
        cmake --build build -j --target 3space cscript_core
        ;;
    windows)
        # MSYS2 UCRT64 path — see BUILD-WINDOWS.md.
        export PATH="${CONAN_CMAKE_BIN:-}:/ucrt64/bin:$PATH"
        if [ ! -f build/cmake/conanbuildinfo.cmake ] && [ ! -f build/conanbuildinfo.cmake ]; then
            conan install . --install-folder=build --build=missing >/dev/null
        fi
        cmake -S . -B build \
            -G "MinGW Makefiles" \
            -DCMAKE_MAKE_PROGRAM=/ucrt64/bin/mingw32-make.exe \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_MODULE_PATH="$(pwd)/build/cmake"
        cmake --build build -j --target 3space cscript_core
        ;;
    *)
        echo "[fleet-build-one] unknown OS tag: $OS" >&2
        exit 2
        ;;
esac

echo "[fleet-build-one $OS] top-level cmake (apps on)"
cd ..
case "$OS" in
    macos|linux)
        cmake -S . -B build \
            -DOPEN_SIEGE_BUILD_UPSTREAM=OFF \
            -DOPEN_SIEGE_BUILD_APPS=ON \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_MODULE_PATH="$(pwd)/3space/build/cmake"
        ;;
    windows)
        cmake -S . -B build \
            -G "MinGW Makefiles" \
            -DCMAKE_MAKE_PROGRAM=/ucrt64/bin/mingw32-make.exe \
            -DOPEN_SIEGE_BUILD_UPSTREAM=OFF \
            -DOPEN_SIEGE_BUILD_APPS=ON \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_MODULE_PATH="$(pwd)/3space/build/cmake"
        ;;
esac
cmake --build build -j

# Sanity: list the four expected binaries.
EXT=""
[ "$OS" = "windows" ] && EXT=".exe"
echo "[fleet-build-one $OS] binaries:"
ls -la \
    "build/examples/dts-viewer/dts-viewer$EXT" \
    "build/apps/open-siege-t1-server/open-siege-t1-server$EXT" \
    "build/apps/open-siege-t1-client/open-siege-t1-client$EXT" \
    "build/examples/net-test-client/net-test-client$EXT" 2>&1 | head -10
