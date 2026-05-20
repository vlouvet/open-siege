### PAL File Format

PAL files are typically palettes for use with other image files present in whichever games use them.

Depending on the version of the engine, the format of the PAL file can vary greatly.

#### Variations

Here are some of the most common palette formats present across Dynamix's games:
* The PAL: tagged format.
    * The variant of the format may contain colours for EGA, CGA and VGA (sometimes altogether).
    * This variant can be found in the following games:
        * A-10 Tank Killer 1.0
        * David Wolf Secret Agent
        * Die Hard
        * MechWarrior
        * F-14 Tomcat
        * Red Baron
        * Stellar 7
        * Heart of China
        * A-10 Tank Killer 1.5
        * Nova 9: The Return of Gir Draxon
        * Aces of the Pacific
        * Aces Over Europe
        * Betrayal at Krondor
        * Aces of the Deep
* The DPL file format, with the { 0x0f, 0x00, 0x28, 0x00 } tag.
    * This variant can be found in the following games:
        * MetalTech: Earthsiege
        * MetalTech: Battledrome
        * Earthsiege 2
* The RIFF Microsoft PAL format.
    * The file extension could either be PAL or PPL, depending on the game.
    * This variant can be found in the following games:
        * A-10 Tank Killer 2: Silent Thunder
        * Trophy Bass 3D
        * Trophy Bass 4
        * Driver's Education '98
        * Driver's Education '99
        * Starsiege
        * Starsiege: Tribes
* The PPAL tagged format.
    * This is known as the Phoenix Palette format.
    * This variant can be found in the following games:
        * Front Page Sports: Ski Racing
        * A-10 Tank Killer 2: Silent Thunder
* The PL98 tagged format.
    * The file extension will either be PPL or IPL.
    * This variant can be found in the following games:
        * Starsiege
        * Starsiege: Tribes

#### Disambiguating variants on disk

Multiple variants share the same file extension (`.pal` or `.ppl`). Sniff the first 4 bytes:

| Bytes (ASCII) | Hex                 | Format                           |
|---------------|---------------------|----------------------------------|
| `RIFF`        | 52 49 46 46         | RIFF Microsoft PAL               |
| `PPAL`        | 50 50 41 4c         | Phoenix Palette                  |
| `PL98`        | 50 4c 39 38         | PL98 (multi-palette pack)        |
| `PAL:`        | 50 41 4c 3a         | Older Dynamix PAL                |
| `0f 00 28 00` | 0f 00 28 00         | DPL (Earthsiege branch)          |

#### PL98 layout (Starsiege / Starsiege: Tribes)

Observed on `Shell.ppl` extracted from the Starsiege: Tribes 1.41 freeware release. Parsed by `studio::content::pal::get_ppl_data()` in `lib3space`, which returns a `std::vector<palette>` where each `palette` exposes `colours`, `index`, and `type`.

```
offset  size  field                              example bytes
   0     4    "PL98" magic                       50 4c 39 38
   4     4    palette count (LE u32)             05 00 00 00   (5 palettes)
   8     …    sequence of N palette records
```

Each palette record begins with a small fixed preamble (containing the palette's lookup `index` and a `type` discriminator) followed by 256 four-byte colour entries (red, green, blue, flags). The exact preamble layout should be confirmed against the `get_ppl_data` implementation.

PBMP files reference palettes by `index` via their `PiDX` chunk; see [BMP](BMP) and [PPL](PPL).
