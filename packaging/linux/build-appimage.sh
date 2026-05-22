#!/usr/bin/env bash
# Build Open-Siege-x86_64.AppImage
#
# Prerequisites (see README.md for full instructions):
#   - A prebuilt dts-viewer binary at BINARY (default: ../../examples/dts-viewer/build/dts-viewer)
#   - linuxdeploy and appimagetool available on PATH or in this directory
#   - libfuse2 or --appimage-extract-and-run on container hosts without FUSE

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
APPDIR="${BUILD_DIR}/AppDir"
BINARY="${BINARY:-${SCRIPT_DIR}/../../examples/dts-viewer/build/dts-viewer}"
OUTPUT="${OUTPUT:-${SCRIPT_DIR}/../../Open-Siege-x86_64.AppImage}"

# Resolve tools
LINUXDEPLOY="${SCRIPT_DIR}/linuxdeploy-x86_64.AppImage"
APPIMAGETOOL="${SCRIPT_DIR}/appimagetool-x86_64.AppImage"

download_tool() {
    local name="$1" url="$2" dest="$3"
    if [[ ! -f "$dest" ]]; then
        echo "Downloading $name..."
        curl -fsSL -o "$dest" "$url"
        chmod +x "$dest"
    fi
}

download_tool linuxdeploy \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" \
    "$LINUXDEPLOY"

download_tool appimagetool \
    "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage" \
    "$APPIMAGETOOL"

# Verify binary
if [[ ! -f "$BINARY" ]]; then
    echo "ERROR: dts-viewer binary not found at $BINARY"
    echo "Build it first with: cmake --build examples/dts-viewer/build -j\$(nproc)"
    exit 1
fi

echo "==> Preparing AppDir at $APPDIR"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"
mkdir -p "$APPDIR/usr/share/doc/open-siege"

cp "$BINARY" "$APPDIR/usr/bin/dts-viewer"

# License
cat > "$APPDIR/usr/share/doc/open-siege/LICENSE" <<'EOF'
Open Siege is released under the MIT License.
See https://github.com/electricintel/open-siege/blob/master/LICENSE

The cscript/ subdirectory contains a fork of the Torque 3D console VM,
also released under the MIT License.
See https://github.com/TorqueGameEngines/Torque3D/blob/development/LICENSE
EOF

echo "==> Running linuxdeploy"
"$LINUXDEPLOY" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/dts-viewer" \
    --desktop-file "${SCRIPT_DIR}/open-siege.desktop" \
    --icon-file "${SCRIPT_DIR}/open-siege.png" \
    --output appimage 2>&1 | tee "${BUILD_DIR}/linuxdeploy.log" || true

# linuxdeploy with --output appimage calls appimagetool itself; if the
# output file was not created (e.g. FUSE unavailable), fall back to
# running appimagetool directly on the AppDir.
if [[ ! -f "${SCRIPT_DIR}/../../Open_Siege-x86_64.AppImage" ]] && \
   [[ ! -f "${SCRIPT_DIR}/Open_Siege-x86_64.AppImage" ]]; then
    echo "==> linuxdeploy --output appimage did not produce the AppImage; running appimagetool directly"
    ARCH=x86_64 "$APPIMAGETOOL" --no-appstream "$APPDIR" "$OUTPUT" 2>&1
fi

# Locate the actual output (linuxdeploy names it with underscores)
for candidate in \
    "${SCRIPT_DIR}/../../Open_Siege-x86_64.AppImage" \
    "${SCRIPT_DIR}/Open_Siege-x86_64.AppImage" \
    "$OUTPUT"; do
    if [[ -f "$candidate" ]]; then
        FINAL="$candidate"
        break
    fi
done

if [[ -z "${FINAL:-}" ]]; then
    echo "ERROR: AppImage was not produced. Check ${BUILD_DIR}/linuxdeploy.log"
    exit 1
fi

SIZE_MB=$(du -m "$FINAL" | cut -f1)
echo ""
echo "==> AppImage produced: $FINAL  (${SIZE_MB} MB)"
if (( SIZE_MB > 150 )); then
    echo "WARNING: size ${SIZE_MB} MB exceeds 150 MB target — check bundled library set"
fi

# Verify Tribes data is NOT bundled
echo "==> Checking for accidental Tribes data inclusion..."
TRIBES_FILES="Entities.vol scripts.vol"
for tf in $TRIBES_FILES; do
    if "$FINAL" --appimage-mount 2>/dev/null | grep -qi "$tf" || \
       (command -v unsquashfs &>/dev/null && \
        unsquashfs -l "$FINAL" 2>/dev/null | grep -qi "$tf"); then
        echo "ERROR: Found $tf inside AppImage — Tribes game data must NOT be bundled"
        exit 1
    fi
done
echo "    OK — no Tribes game data found inside the AppImage"
