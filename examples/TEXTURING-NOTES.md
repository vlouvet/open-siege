# Texturing Tribes 1 models — pipeline notes

**Status: COMPLETE** — milestone shipped in spec 08 (`textures: render meshes
with per-material textures`). `dts-viewer` now renders Tribes meshes with
per-bucket palettized PBMP textures sampled from `Entities.vol`/`Shell.ppl`/
`<world>.day.ppl`. Screenshots at `docs/done/01-textures/08-*.png`.

Notes from investigating what it would take to paint textures on the DTS models that `dts-viewer/` currently renders flat-shaded. Reflects state of the upstream Open Siege wiki and code as observed during this work; the implementation that landed this milestone follows the pipeline below.

## TL;DR

The texturing pipeline does **not** require DML parsing. `lib3space` already parses every format involved:

```
DTS material list  →  PBMP file (Phoenix BMP)  →  PiDX chunk gives palette ID
                                              →  pixels[] = 8-bit indices
PPL file           →  PL98 tag, N (palette index, 256 colours) records
                   →  look up palette by ID, decode pixels → RGBA8
```

3space provides `bmp::get_pbmp_data()` and `pal::get_ppl_data()`; both already work. The remaining work is plumbing them into `dts-viewer`.

## Wiki coverage assessment

[`open-siege/open-siege.wiki`](https://github.com/open-siege/open-siege/wiki) is mostly skeletal. The state on inspection:

| Page | Size | Useful? |
|---|---|---|
| `DML.md` | 10 lines | No — just describes purpose, no byte-level info |
| `PPL.md` | 3 lines | Redirects to PAL.md |
| `PAL.md` | 50 lines | Yes — names variants per game; Tribes uses **PL98** (PPL/IPL extension) |
| `BMP.md` | 30 lines | Yes — describes both Windows BMP and **PBMP** (Phoenix); Tribes uses the latter |
| `DTS.md` | 53 lines | Useful for orientation; Tribes uses the "Darkstar PERS/TS::Shape" variant |
| `VOL.md` | 24 lines | Orientation only |
| `darkstar.md` | 35 lines | Engine overview; lists common formats — **DML is not listed** |
| `PBM.md` | (doesn't exist) | — |
| Many others | 0–2 lines | Empty stubs (DSO, MIS, RMF, JPG, PNG, etc.) |

## Wiki claim worth flagging

`BMP.md` states:
> "Reserved fields in the header are used to store the ID of the palette to use, depending on the game."

This is **true only for the Windows BMP variant** (`BM` magic). Tribes does not use Windows BMPs. Every Tribes "BMP" file is actually a **PBMP** (Phoenix Bitmap), which has a chunked layout and stores the palette ID in a dedicated `PiDX` chunk, not in a reserved header field. The wiki does describe this correctly in its PBMP section — but the reserved-field sentence above applies to a different format.

## Verification against real data

Extracted samples from `Entities.vol` (Starsiege Tribes 1.41, freeware release) via `vol-list --extract`:

```
ammo.bmp      magic = "PBMP"   (0x50 0x42 0x4d 0x50)
antenna1.bmp  magic = "PBMP"
disc.bmp      magic = "PBMP"
grenammo.bmp  magic = "PBMP"
lr_ammo.bmp   magic = "PBMP"
plasammo.bmp  magic = "PBMP"
```

PBMP structure observed:
```
offset  size  field
   0     4    "PBMP" magic
   4     4    total file size minus 8 (RIFF-style)
   8     4    "head" chunk tag
  12     4    head chunk size = 0x14 (20 bytes)
  16     4    version (= 3)
  20     4    width
  24     4    height
  28     4    bit depth (= 8 for 256-colour indexed)
   …          subsequent chunks: data, DETL (mipmap count), PiDX (palette id)
```

PPL sample from `Shell.vol`:
```
Shell.ppl  magic = "PL98"   (0x50 0x4c 0x39 0x38)
           count = 5
           (subsequent bytes: per-palette index, type, 256×colour records)
```

This matches PAL.md's claim that Tribes PPLs use the PL98 tagged format.

## DML's actual role

Per DML.md: *"contains multiple references to textures and the properties of a model or object in the game world."*

The "properties" part is what matters: DML carries per-material metadata (transparency, blending, animation parameters) that the game engine consumes at runtime. The "references to textures" part — the bitmap filename for material N — is already in the DTS file itself, exposed via `renderable_shape::get_materials()`. So:

- **For a static textured viewer**: skip DML. The DTS provides bitmap filenames; the PBMPs carry their own palette IDs.
- **For faithful in-engine behaviour**: DML matters — material overrides, animated textures, alpha masking. Real reverse-engineering work; out of scope here.

## Planned pipeline for `dts-viewer`

(Estimated 2–4 hours of focused work. Not implemented yet.)

1. Capture UVs in the existing `buffering_renderer`. The `emit_texture_vertex` callback is currently empty. Add a parallel `std::vector<float> uvs` and push `(v.x, v.y)` for each face vertex. Triangulate UVs the same way positions are triangulated.

2. Switch from one flat positions+normals buffer to one buffer per object. `update_object` already names the boundary; restart the per-object accumulator there. Each object will become its own `glDrawArrays` call.

3. For each material returned by `shape->get_materials()`:
   - Find the BMP file by name across the loaded VOLs (extend `vol-list`'s search or load multiple VOLs at startup).
   - Call `bmp::get_pbmp_data()` to get `pixels[]`, `palette_index`, dimensions.

4. Load one or more PPL files (`Shell.ppl` covers many Entities; world packs like `lushDML.vol/lushTerrain.ppl` cover terrain-skinned models). Call `pal::get_ppl_data()`. Build an unordered_map from palette index → 256-colour array.

5. For each PBMP: look up its `palette_index`, expand 8-bit indices through the palette to a flat `std::vector<uint8_t>` of RGBA. Upload via `glTexImage2D`.

6. Shader changes:
   - Vertex shader: pass UV through
   - Fragment shader: sample `texture(u_tex, v_uv)` instead of the current Lambert constant. Keep the Lambert factor for shading.
   - Bind correct texture per object before each draw call.

## Open questions for when this gets implemented

- **Which PPL is "right" for which model?** Tribes loads world-specific PPLs at mission start (different palettes for lush, desert, alien terrain). A standalone viewer needs to pick one; `Shell.ppl` is a reasonable default for non-terrain assets. A `--palette <ppl-path>` flag lets the user override.
- **Mipmaps**: the `DETL` chunk indicates how many mip levels the PBMP carries. The viewer can either upload only the base level or upload all of them as a proper mip chain. Cleanly out of scope for a first pass.
- **Texture coordinate animation**: DML territory; ignore for now.
- **PBMPs that aren't in `Entities.vol`**: terrain textures, player-skin textures, etc. live in world VOLs. Loading multiple VOLs at startup or accepting `--vol` repeated is straightforward.

## What's already in tree to support this

- `examples/vol-list/` now has `--extract <vol> <substr> <out-dir>` (added in the same commit as this doc) for ad-hoc inspection.
- `examples/dts-viewer/main.cpp` has the buffering renderer scaffold; UV plumbing is the immediate change point.
