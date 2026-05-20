### DML File Format

DML is short for Dynamix material list, and contains multiple references to textures and the properties of a model or
object in the game world.

### Role in the engine

DML carries per-material metadata layered on top of the bitmap and palette files — properties such as transparency, blending modes, and texture animation parameters that the engine consumes at runtime.

For **static asset viewing** (extracting models, exporting to other formats, painting a model with its base textures), DML is not strictly required:

- The DTS file itself enumerates the material names a model uses (exposed via `studio::content::renderable_shape::get_materials()` in `lib3space`, which returns each material's filename and metadata).
- Each Phoenix BMP (PBMP) referenced by a material carries its own palette ID in a `PiDX` chunk — see [BMP](BMP).
- That palette ID is resolved against a [PPL](PPL) (PL98 tagged) palette pack.

DML matters for **faithful in-engine behaviour** — material overrides, animated textures, alpha masking effects — but is not on the bitmap → palette critical path.

### Format details

The byte-level layout of DML files in the Darkstar branch (Starsiege, Starsiege: Tribes) has not been documented in this wiki yet. `lib3space` does not currently expose a DML parser. Contributions welcome.

### See Also

DML files are typically embedded into [VOL files](VOL) or into [DTS files](DTS).

The core game engine:

* [Darkstar Game Engine](darkstar)
