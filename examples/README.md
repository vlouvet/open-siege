# Examples — macOS arm64

Two standalone consumers of `lib3space.a` that demonstrate the parsers end-to-end on macOS arm64 against authentic Tribes 1 data.

Prerequisite: build `3space/` first per [BUILD-MACOS.md](../BUILD-MACOS.md). Both examples link against `3space/build/lib3space.a` and reuse the `Find*.cmake` files Conan generated during that build.

## `vol-list/` — VOL inspector and DTS parser sweep

CLI that exercises the Darkstar VOL container parser and the DTS mesh parser.

```sh
cd examples/vol-list
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .

./vol-list /path/to/Tribes/base/Entities.vol            # list contained files
./vol-list --dts /path/to/Tribes/base/Entities.vol      # parse every DTS, print geometry stats
```

Validated against the official Tribes 1 freeware release: 90/90 VOLs parse, 209/209 DTS files parse including detail-level and animation-sequence enumeration plus full geometry traversal via a counting `shape_renderer`.

## `dts-viewer/` — minimal SDL2 + OpenGL DTS viewer

Standalone viewer that opens a VOL, picks a DTS by substring match, and renders it on screen with flat-shaded normals and an orbit camera.

```sh
brew install sdl2 glm
cd examples/dts-viewer
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build .

./dts-viewer /path/to/Tribes/base/Entities.vol chainturret
```

Controls: left-drag orbits, scroll zooms, Esc/Q quits.

Tested on macOS 26 / Apple M4 / OpenGL 4.1 (Metal backend). Geometry pipeline: `make_shape` → custom buffering `shape_renderer` that fan-triangulates polygons and computes per-face normals → static VAO/VBO upload → single `glDrawArrays` per frame.

## Why this exists

The upstream `siege-studio` GUI viewer pulls in `wxwidgets`, `SFML`, `imgui-sfml` via Conan. On macOS arm64 today:

- The Conan v1 toolchain Open Siege uses is on a deprecation path.
- `imgui-sfml` is no longer published on conan-center.
- `wxwidgets 3.2.6` (latest on conan-center) does not compile against the macOS 26 SDK — it calls `CGDisplayCreateImage`, which Apple removed in macOS 15.0.

A WIP attempt at porting `siege-studio` (vendoring `imgui-sfml`, bumping Conan recipes) lives on the `siege-studio-port-wip` branch; it documents the dead ends but does not build to completion.

These two examples bypass that stack entirely and give a working "Tribes data on macOS" path for asset inspection and basic mesh viewing.

## Known limitations

- **No textures yet.** `dts-viewer` renders flat-shaded geometry only. Wiring up Tribes textures requires (a) capturing UVs in the `shape_renderer` (trivial — it's a one-line addition), (b) per-object multi-draw so each material gets its own draw call, (c) BMP+PAL loading per material filename (3space has parsers for both), and (d) world-specific palette resolution, which goes through the DML format that `lib3space` does not yet parse. The siege-studio viewer also does not paint textures — its `gl_renderer` discards UVs and uses hashed-by-name solid colors per face.
- **Fan triangulation assumes convex polygons.** Tribes DTS faces are generally convex; if not, the resulting triangles overlap visually.
- **No animation.** Sequences are enumerated but `dts-viewer` always renders the bind pose.
