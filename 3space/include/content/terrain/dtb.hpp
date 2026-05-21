#ifndef DARKSTAR_CONTENT_TERRAIN_DTB_HPP
#define DARKSTAR_CONTENT_TERRAIN_DTB_HPP

// Parser for Tribes 1 per-mission terrain blob files (`*.dtb`).
//
// A DTB resource is a single `GBLK`-tagged chunk that lives inside a
// per-mission `.ted` PVOL archive alongside the matching `.dtf` header.
// It carries three independently LZH-compressed data layers:
//   - heightmap:   (sx+1)*(sy+1) little-endian f32 altitude values
//   - materialmap: (sx+1)*(sy+1) pairs of (flags u8, index u8)
//   - lightmap:    lmW*lmW little-endian u16 values (IIII RRRR GGGG BBBB)
// followed by 11 pinMap subrecords (editor data, skipped) and a
// 24-byte HRLM trailer (zero in every shipped freeware mission).
//
// Shipping Tribes 1.41 freeware content always uses classVersion 5 and
// a 256×256 grid (257×257 vertex array = 66 049 entries per layer).
//
// Byte layout and validation vectors are from
// `docs/clean-room-specs/TERRAIN.md` §2. Hex-verified against all 45
// per-mission `.dtb` files shipped in the Tribes 1.41 freeware corpus.
// No leaked engine source was consulted.

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace studio::content::terrain
{
  // One vertex-grid entry in the decoded materialmap.
  struct material_cell
  {
    // Orientation and render flags (bits 0..2: rotation group;
    // bits 3..5: empty level; bits 6..7: editor markers).
    std::uint8_t flags = 0;

    // Index into the parent world's DML material list.
    // 0xff means "no material" (used with empty-level flag).
    std::uint8_t index = 0;
  };

  // Decoded contents of one DTB (GBLK) chunk.
  struct grid_block
  {
    // Always 5 in shipping Tribes content. Other values are rejected.
    std::uint32_t version = 0;

    // Block name, e.g. "block-0". Read from the 16-byte fixed NUL-padded
    // field. Every shipped mission stores "block-0".
    std::string name;

    // Number of LOD detail levels. Observed = 9 in every shipping file;
    // must match the DTF's detailCount.
    std::int32_t detail_count = 0;

    // Lightmap scale factor. Observed = 0 everywhere (lightmap is 1:1
    // with the height grid, so lmW = (size[0] << 0) + 1 = 257).
    std::int32_t light_scale = 0;

    // Altitude envelope in metres. Bit-exact f32 values; must agree with
    // the sibling DTF and with the min/max of the decoded heightmap.
    float height_min = 0.0f;
    float height_max = 0.0f;

    // Grid tile dimensions. Observed (256, 256) in every shipping file,
    // giving a 257×257 vertex array.
    std::array<std::int32_t, 2> size{};

    // Decoded heightmap: (size[0]+1) * (size[1]+1) altitude f32 values,
    // row-major with x varying fastest. Every value must lie within
    // [height_min, height_max].
    std::vector<float> heights;

    // Decoded materialmap: (size[0]+1) * (size[1]+1) material_cell pairs
    // (flags + index), row-major with x varying fastest.
    std::vector<material_cell> materials;

    // Decoded lightmap: lmW * lmW u16 entries (IIII RRRR GGGG BBBB per
    // entry), where lmW = (size[0] << light_scale) + 1. Empty if absent
    // (lightScale == -1; never observed in shipping files).
    std::vector<std::uint16_t> lightmap;

    // lightmap_dim == lmW above. 0 if lightmap is absent.
    std::int32_t lightmap_dim = 0;

    // HRLM opaque trailer bytes (read-and-stored; not interpreted by v1).
    // 20 bytes in every shipping file (all zero).
    std::vector<std::byte> hrlm_raw;
  };

  // Returns true and leaves the stream position unchanged when the next
  // four bytes are the `GBLK` magic. Returns false otherwise (also
  // restoring the original position).
  bool is_darkstar_dtb(std::istream& stream);

  // Parse a DTB chunk from `src` starting at the current stream
  // position. Returns std::nullopt when:
  //   - the file does not begin with `GBLK`,
  //   - the chunk size is unreasonably large (> 64 MiB),
  //   - the payload classVersion is not 5,
  //   - any LZH decompression fails,
  //   - the HRLM trailer has a non-3 version or non-zero array sizes
  //     (v1 limitation),
  //   - the stream fails before the trailer.
  // On success, the stream is left positioned immediately after the last
  // byte of the GBLK chunk payload.
  std::optional<grid_block> parse_dtb(std::istream& src);
}

#endif
