### BMP File Format

BMP files, short for Bitmap, is a basic image format, typically used without compression and with widespread support.

For most Dynamix games, before the introduction of the Torque engine, the BMP format was one of the most commonly used file formats used, especially for Windows based games.

While there may be a shared extension, there are various internal representations of BMP, which are somewhat similar but with differences in features:

* Windows BMP
  * The most supported format across games and software alike.
  * It is common for most of the BMP files to have 8-bit indexed colour, supporting up to 256 colours.
  * Despite having an internal palette, some games, such as Starsiege, may force usage of a different palette, in either [PAL](PAL) or [PPL](PPL) format. In this instance, if a new image is created, the palette needs to be remapped to the palette used by the game.
  * Reserved fields in the header are used to store the ID of the palette to use, depending on the game.
  * The magic file header is **BM**. Information about the data structures can be found here:
    * https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapfileheader
    * https://docs.microsoft.com/en-us/previous-versions/dd183376(v=vs.85)
    * https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapinfo
* Phoenix BMP
  * This format is fundamentally the same as normal BMP files, except with different headers and additional features.
  * Just like Windows BMP files, it is common for these files to make use of 8-bit indexed colour.
  * Palette information is not typically stored inside of these files, and thus are often used in tandem with [PAL](PAL) or [PPL](PPL) files. However, embedding of Microsoft [PAL](PAL) files is supported for Phoenix bitmaps.
  * Mipmaps are stored directly in the file, so that the game does not have to generate smaller versions of the texture dynamically. Each mipmap will be half the size of the previous image, if present. If the main image is 256x256, and if there are two additional detail levels, there will be a 128x128 image and a 64x64 image.
  * The magic file header is **PBMP**, with multiple chunks being present in the file, similar to the RIFF format.
    * There is a **head** chunk, which contains information about the image.
    * There is a **data** chunk, which contains the raw pixels of the image. If there are mipmaps, they will follow the main image and then each other sequentially.
    * There is a **DETL** chunk, which contains the number of detail levels/mip maps in the image (including the primary image).
    * There is a **PiDX** chunk, which contains an ID for which palette to use. [PPL](PPL) files contain multiple palettes, each with a unique ID.
* Other BMP formats
  * Depending on the game, there are other variations of the BMP format, which have not been documented. When more information is known about them, it will be added here.

### Which variant a given game uses

The Starsiege-branch Darkstar engine games (Starsiege, Starsiege: Tribes) use the **Phoenix BMP (PBMP)** variant for all their image assets, even though the files have a `.bmp` extension. Inspection of every `.bmp` file extracted from the Starsiege: Tribes 1.41 freeware `Entities.vol` confirms a `PBMP` magic header — there are no Windows-BMP-with-reserved-palette-ID files in the Tribes asset set. The "reserved fields hold the palette ID" mechanism described above for the Windows BMP variant therefore does not apply to Tribes; the palette ID lives in the `PiDX` chunk instead.

### Observed PBMP byte layout (Starsiege / Starsiege: Tribes)

The PBMP container is RIFF-style: a fixed magic and overall-size, followed by a sequence of tagged chunks. The `head` chunk always appears first and has a fixed layout matching `studio::content::bmp::pbmp_header` in `lib3space`.

```
offset  size  field                              example bytes (ammo.bmp)
   0     4    "PBMP" magic                       50 42 4d 50
   4     4    file size minus 8 (LE u32)         20 40 00 00  (≈ 16k, matches file)
   8     4    "head" chunk tag                   68 65 61 64
  12     4    head chunk size = 0x14 (LE u32)    14 00 00 00
  16     4    version (LE u32, observed = 3)     03 00 00 00
  20     4    width (LE u32)                     80 00 00 00  (128)
  24     4    height (LE u32)                    80 00 00 00  (128)
  28     4    bit depth (LE u32, 8 = indexed)    08 00 00 00
  32     …    subsequent chunks: "data", "DETL", "PiDX"
```

`PiDX` contains the palette ID; `DETL` contains the mipmap level count; `data` contains the 8-bit indexed pixels followed by mipmap chains. The `studio::content::bmp::pbmp_data` struct in `lib3space` exposes the parsed result with a `palette_index` field that matches a palette `index` in the active PPL.

### See Also

* [PAL](PAL) — palette format variants
* [PPL](PPL) — multi-palette pack, indexed by `PiDX`
