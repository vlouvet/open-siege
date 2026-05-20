### DML File Format

DML is short for Dynamix Material List. A DML is a flat array of per-material records — bitmap filename plus surface-physics and slot-status metadata — used by the Darkstar-branch engines (Starsiege, Starsiege: Tribes) to bind a model's faces to textures and to tag each surface for footstep, impact, and friction behaviour at runtime.

The data appears in two places on disk:

* **Standalone `.dml` files** packed inside [VOL](VOL) archives — one DML per object, terrain world, or skin set (`tribes-game/base/Entities.vol` ships 259 of them in the 1.41 freeware release).
* **Inline trailers** at the end of a [DTS](DTS) shape — the same serialised `TS::MaterialList` object emitted by the engine's persistence layer immediately after the shape data.

The same parser handles both: when the engine reads a PERS object whose class name is `TS::MaterialList`, it dispatches to the material-list reader regardless of whether the object stood alone in a file or was the tail of a DTS.

### Container model

DML is **not** RIFF-chunked and **not** tag-delimited inside the record array. Each file is a single Dynamix tag-stream PERS object: an 8-byte PERS wrapper, a length-prefixed class name, a class version, and then a fixed-stride record array. The only structural tokens are the leading `PERS` magic and the literal class name `TS::MaterialList`. There are no inner chunks to enumerate.

The file size is exactly `38 + num_materials * record_size_for_version` — verified against all 259 standalone DMLs in the Tribes 1.41 corpus with zero mismatches.

### File header — fixed 38 bytes

Layout is identical across versions 2, 3, and 4:

```
offset  size  field                                example bytes (human1.dml)
   0     4    "PERS" magic                         50 45 52 53
   4     4    payload size, file_size - 8 (LE u32) 78 3f 00 00   (16248 bytes follow)
   8     2    class-name length (LE i16)           10 00         (16)
  10    16    ASCII class name                     54 53 3a 3a 4d 61 74 65
                                                   72 69 61 6c 4c 69 73 74   ("TS::MaterialList")
  26     4    class version (LE u32)               04 00 00 00   (v4)
  30     4    num_details (LE u32)                 01 00 00 00   (always 1 in corpus)
  34     4    num_materials (LE u32)               ff 00 00 00   (255 records)
```

`num_details` is `1` in every observed file; lib3space exposes it but no consumer varies on it.

### Per-version record layout

Each version inherits the previous version's fields and appends new ones at the tail, so the record stride grows with the version number. Field offsets below are relative to the start of a record.

```
v2 record (48 bytes) — 4 files in corpus (sky / lensflare effects)
  off  size  field                  type
    0    4   flags                  i32 LE
    4    4   alpha                  f32 LE
    8    4   index                  i32 LE
   12    4   rgb_data               4 bytes (packed colour)
   16   32   file_name              ASCIIZ, NUL-padded

v3 record (60 bytes) — 1 file in corpus (deserttansky.dml)
  off  size  field                  type
    0   48   ...v2 fields...
   48    4   type                   i32 LE  (surface-category enum)
   52    4   elasticity             f32 LE
   56    4   friction               f32 LE

v4 record (64 bytes) — 254 files in corpus (everything else)
  off  size  field                  type
    0   60   ...v3 fields...
   60    4   use_default_properties u32 LE
```

Version distribution in `tribes-game/base/`:

| version | record size | count | typical role |
|---|---|---|---|
| 2 | 48 B | 4 | sky / lensflare effects |
| 3 | 60 B | 1 | sky (`deserttansky.dml`) |
| 4 | 64 B | 254 | objects, bases, terrain, everything else |

### Worked example — first v4 record from `human1.dml`

```
offset  bytes                                              decoded
  38    03 01 00 00                                        flags         = 0x00000103
  42    00 00 00 00                                        alpha         = 0.0f
  46    00 00 00 00                                        index         = 0
  50    00 00 00 00                                        rgb_data      = 00 00 00 00
  54    63 6f 6c 64 5f 31 36 62 2e 62 6d 70 00 ... 00      file_name     = "cold_16b.bmp"
  86    03 00 00 00                                        type          = 3 (metal)
  90    00 00 80 3f                                        elasticity    = 1.0f
  94    00 00 80 3f                                        friction      = 1.0f
  98    01 00 00 00                                        use_default_properties = 1
```

The next record starts at offset 102 (38 + 64), and so on for `num_materials` iterations.

### Field semantics

These are derived from byte-pattern analysis of 4229 records across 259 shipping DMLs — type / elasticity correlations, source-VOL provenance, and BMP-name patterns. No leaked source was consulted. **No consumer code in Open Siege branches on any of these fields** (see "Runtime consumption" below); the semantics are best-effort interpretation of empirical regularities. Confidence ratings reflect that.

#### `flags` (i32 at record+0) — slot status

| value | count | name field | interpretation | confidence |
|---|---|---|---|---|
| `0x00000103` | 4132 | non-empty filename | valid material — the only flags value seen on records that carry a BMP name | very high |
| `0x0000F000` | 73 | empty (all NUL) | unused slot, upper-nibble form (all of `human1.dml`'s holes) | high |
| `0x00000000` | 62 | empty (all NUL) | unused slot, all-zero form (seen in `titan.dml`, `savana.dml`) | high |

Both empty-slot encodings coexist in shipping data and accompany identical all-zero record bodies, so the engine's emptiness check almost certainly inspects `name[0] == '\0'` (or `(flags & MAT_VALID) == 0`) rather than testing equality against a single sentinel. **Renderers should skip records where `name[0] == '\0'` to handle both forms uniformly.** Individual bit meanings inside `0x103` are unknown — the value is constant across all 4132 valid records, providing no internal variation to disambiguate.

#### `alpha` (f32 at record+4), `index` (i32 at record+8), `rgb_data` (4 bytes at record+12)

Always zero in every observed shipping record. The engine evidently does not source per-material alpha from this field — `0.0f` cannot be the multiplicative default (it would render everything invisible). Treat as opaque metadata only; rely on the bitmap's own 0-magenta / 0-alpha key for transparency.

#### `file_name` (32-byte ASCIIZ at record+16)

The PBMP filename to bind for this material slot, e.g. `dropeagle4.BMP`, `cold_16b.BMP`. Mixed case is normal across the corpus (both `.BMP` and `.bmp` occur). NUL-terminated and NUL-padded to 32 bytes.

#### `type` (i32 at v3/v4 record+48) — surface category, 14 of 15 enum slots used

The single most discriminating field. Correlates strongly with the source VOL (terrain vs object), with the elasticity float, and with BMP-name patterns (Tribes terrain BMPs use deterministic 4-letter blend codes where the leading letter is the world and the next four encode the tile family).

| type | count | dominant elasticity | source VOLs | hypothesis | confidence |
|---|---|---|---|---|---|
| 0 | 166 | 1.0 | world skies, human1DML | sky / billboard / non-physical | medium |
| 1 | 152 | mixed (0.5 + 1.0) | desertTerrain, human1DML | concrete / hard masonry | medium-high |
| 2 | 65 | 1.0 | human1DML | carpet (every BMP literally named `carpet_*`) | very high |
| 3 | 1618 | 1.0 | human1DML | metal / generic interior structural | very high |
| 4 | 389 | 1.0 | human1DML | window / translucent panel / HUD display | high |
| 5 | 1 | 1.0 | human1DML | unknown — single-record outlier referencing `light_metal.bmp` | low |
| 6 | 7 | 1.0 | human1DML | wood (all 7 records reference `base_wood.bmp`) | very high |
| 7 | 22 | 1.0 | human1DML | marble (all 22 records reference a `marble_*` BMP) | very high |
| 8 | 81 | 0.5 | iceTerrain | ice / snow terrain | high |
| 9 | 15 | 0.5 | iceTerrain | deep ice / water | high |
| 10 | 116 | 0.5 | alienTerrain, desertTerrain | soft terrain — grass, loose ground, alien crater | high |
| 11 | 0 | — | — | absent in corpus — gap in enum, possibly reserved | unknown |
| 12 | 542 | mixed (0.5 + 1.0) | human1DML, desertTerrain, iceTerrain | stone / rock / emblem decal | high |
| 13 | 41 | 0.5 | alienTerrain | alien grass / soft alien terrain | medium-high |
| 14 | 66 | 0.5 | alienTerrain, desertTerrain, iceTerrain | dirt / loose-ground terrain blend | medium |

The strong elasticity correlation (terrain types 8/9/10/13/14 are almost all `0.5f`; object types 2/3/4/6/7 are all `1.0f`) supports a physics-tag interpretation: terrain absorbs more impact energy than rigid base walls. Types 1 and 12 sit on the boundary — `0.5` when used as terrain ground tiles, `1.0` when used as base structure — consistent with the tag being per-record rather than per-type.

Best interpretation: `type` is a **surface category for physics and sound** (footstep, impact spark, dust puff), not a render-mode hint. Runtime consultation likely happens in Tribes' CScript `setMaterialMapping` / footstep tables (in `tribes-game/base/*.cs`), not in lib3space.

#### `elasticity` (f32 at v3/v4 record+52)

Two observed values across the corpus: `1.0f` (3278 records, rigid surfaces and base walls) and `0.5f` (959 records, concentrated in terrain DMLs). See the `type` table above for the per-category breakdown.

#### `friction` (f32 at v3/v4 record+56)

`1.0f` in **every single record** across the corpus. Field exists but is never varied in shipping data. Either the engine uses a per-`type` lookup at runtime and only consults this field as an override (which authors never used), or the field is vestigial in v3/v4.

#### `use_default_properties` (u32 at v4 record+60)

`1` in every observed v4 record across the full corpus (4229 records). Never observed `0`.

Plausible interpretation (high confidence based on the field name + the always-default per-record floats): when `1`, the engine ignores the per-record `friction` and `elasticity` and looks up defaults keyed by the `type` enum; when `0`, it honours the per-record overrides. This explains why `friction == 1.0f` everywhere and why `elasticity` only takes two distinct values: the floats are either dead-store defaults or a thin fingerprint of two physical buckets (rigid vs spongy). The v3→v4 schema change apparently added an override capability that ship content never exercised.

### Runtime consumption

**As of this writing, no consumer code in Open Siege branches on any of `flags`, `alpha`, `type`, `friction`, `elasticity`, or `use_default_properties`.** A complete grep of `open-siege/` finds exactly one read-site for each — emplacement into a `std::unordered_map<string_view, variant>` in `dts_renderable_shape::get_materials()` — and exactly two consumers of that metadata map:

* `siege-studio/src/views/dts_view.cpp` displays the values via `ImGui::Text` for inspector purposes only — no conditionals.
* `siege-studio/tools/dts-to-obj/src/convert_dts.cpp` accepts a `.dml` as input and then carries a `// TODO generate a MTL file` placeholder — read path works, write path is a stub.

The dts-viewer in this fork (`examples/dts-viewer/main.cpp`) only consumes `material.filename`. The semantics documented above are correct as far as the byte patterns go, but **lib3space has decoded these fields' bytes correctly since 2022-11-22 (commit `06d69a3`) and has never used their values**. Renderer / physics integration is pending.

### Parsing in lib3space

DML has been parsed in lib3space since 2022-11-22. Two equivalent entry points:

* **`studio::content::dml::is_dml(std::istream&)`** and **`studio::content::dml::get_dml_data(std::istream&)`** — thin discoverability wrappers in `3space/include/content/dml/dml.hpp` (commit `0e4724f`). `get_dml_data` returns `std::optional<material_list_variant>` (a `std::variant<v2, v3, v4>`) so callers can `std::visit` over version-specific record shapes. The stream is consumed on success and rewound on failure.
* **`studio::content::dts::darkstar::read_shape(std::istream&)`** — the underlying engine entry point in `3space/src/content/dts/darkstar.cpp`. It detects standalone DML files (where the PERS class name is `TS::MaterialList`) and dispatches to `read_material_list`, returning a `shape_or_material_list` variant. The `dml::` wrappers exist so future readers searching for "dml" land on the right entry points without needing to know the parser lives under the `dts/` namespace.

For renderable access, **`renderable_shape::get_materials()`** in `3space/include/content/renderable_shape.hpp` surfaces each material as a struct with `filename`, `texture_file_name`, and a `metadata` map keyed by `flags`, `alpha`, `type`, `friction`, `elasticity`, `useDefaultProperties`. The mapping from `material_list_variant` to `renderable_shape::material` lives in `3space/src/content/dts/dts_renderable_shape.cpp`.

A round-trip serialiser (`write_material_list`) and JSON converters (`include/content/json_boost.hpp`, used by `dts-to-json`) are also in tree.

### See Also

* [BMP](BMP) — Phoenix Bitmap (PBMP) format; the `file_name` field in each DML record names a PBMP
* [PPL](PPL) — palette pack referenced by each PBMP's `PiDX` chunk
* [DTS](DTS) — shape format that can carry an inline `TS::MaterialList` trailer
* [VOL](VOL) — archive format that holds standalone `.dml` files

DML files are typically embedded into [VOL files](VOL) or trail a [DTS](DTS) shape.

The core game engine:

* [Darkstar Game Engine](darkstar)
