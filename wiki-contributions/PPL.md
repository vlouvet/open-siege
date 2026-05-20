### PPL File Format

PPL is a palette pack file: a single file containing multiple 256-colour palettes, each addressable by a numeric ID. Phoenix-engine bitmaps ([PBMP](BMP)) reference a palette by ID through their `PiDX` chunk, and the engine resolves that ID against a PPL loaded for the current world or shell.

### Variants

The `.ppl` extension is shared by more than one underlying format. See [PAL](PAL) for the full table of palette variants across Dynamix's games. The two PPL variants are:

* **RIFF Microsoft PAL** with the `.ppl` extension — used in some Darkstar-branch games.
* **PL98 tagged** — the Phoenix Engine palette pack format. Used in Starsiege and Starsiege: Tribes.

Sniff the first 4 bytes to disambiguate: `RIFF` for the Microsoft variant, `PL98` for the Phoenix variant.

### PL98 layout (Starsiege / Starsiege: Tribes)

The PL98 variant is parsed by `studio::content::pal::get_ppl_data()` in `lib3space`, which returns a `std::vector<palette>`, each palette carrying `colours`, `index` (the lookup ID), and `type` fields.

Header observed on `Shell.ppl` from the Starsiege: Tribes 1.41 freeware release:

```
offset  size  field                                       example bytes
   0     4    "PL98" magic                                50 4c 39 38
   4     4    palette count (little-endian u32)           05 00 00 00   (5 palettes)
   8     …    sequence of per-palette records
```

Per-palette record observed shape (offset relative to start of record):

```
offset  size  field
   0     4    reserved / padding
   4     4    palette index (little-endian u32)
   8     4    palette type (little-endian u32)
  12     8    boundary marker / per-palette metadata
  20   1024   256 × 4-byte colour entries (R, G, B, flags)
```

The exact layout of the per-palette preamble before the colour entries should be confirmed against the `pal::get_ppl_data` implementation; the above reflects what was observed at the beginning of `Shell.ppl` and what the public `palette` struct exposes.

### Picking a palette

A model's PBMP files reference palette IDs from the PPL the game has loaded at render time. The game loads world-specific PPLs at mission start, so the same model can appear with different colour skins on different terrains (e.g. lush vs desert vs alien).

For standalone viewers and exporters that operate outside the engine's world-loading flow, picking a sensible default PPL (such as `Shell.ppl` for shell/UI assets, or one of the per-world `<world>DML.vol/<world>Terrain.ppl` files for terrain-skinned models) usually produces correct colours for non-terrain props.

### See Also

* [BMP](BMP) — Phoenix Bitmap (PBMP) format; references palettes by ID via the `PiDX` chunk
* [PAL](PAL) — palette format variants across the Dynamix games
