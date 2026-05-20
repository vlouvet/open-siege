#ifndef DARKSTAR_CONTENT_TERRAIN_DTF_HPP
#define DARKSTAR_CONTENT_TERRAIN_DTF_HPP

// Parser for Tribes 1 per-mission terrain header files (`*.dtf`).
//
// A DTF resource is a single `GFIL`-tagged chunk that lives inside a
// per-mission `.ted` PVOL archive. It is the "table of contents" of a
// terrain block: it names the material list (DML) and tells the loader
// which compressed `.dtb` block ids back the mission's grid of blocks.
//
// DTF is NOT `PERS`-wrapped; the first four bytes are the ASCII tag
// `GFIL` followed by a u32 chunk-size-minus-8 and then a versioned
// payload. Shipping freeware content always uses payload version 1 and
// a 3 by 3 block grid where every cell maps to block id 0.
//
// Byte layout was reverse-engineered from hex inspection of all 45
// `.dtf` files shipped in the Tribes 1.41 freeware corpus. The spec
// followed is `docs/clean-room-specs/TERRAIN.md`. No leaked engine
// source was consulted by this implementation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace studio::content::terrain
{
  // One entry of the DTF block-map: identifies which named block in
  // the trailing block list backs this grid cell.
  struct dtf_block_ref
  {
    std::int32_t block_id = 0;
  };

  // One entry of the DTF trailing block list. Shipping files have
  // exactly one entry with id 0 and an empty name; the name is paired
  // with the `block-<id>` nameId field in the corresponding `.dtb`.
  struct dtf_block_list_entry
  {
    std::int32_t block_id = 0;
    std::string  name;
  };

  // Decoded DTF header. The renderer cares about `material_list_name`
  // (the DML to load alongside) plus `height_min`/`height_max` (the
  // altitude envelope for the mission's heightmap, which must agree
  // bit-exactly with the same fields inside the sibling `.dtb`).
  struct dtf_file
  {
    // Always 1 in shipping Tribes content; preserved verbatim so a
    // future version-2 reader can branch on it.
    std::uint32_t version = 0;

    // The material list resource name, e.g. "lush.dml" or
    // "desert.dml". The corresponding DML lives in the per-world
    // `<world>Terrain.vol` archive.
    std::string material_list_name;

    // Highest block id referenced by the block-map. Observed = 1 in
    // every shipping mission (a single block, id 0, is referenced by
    // all 9 grid cells).
    std::int32_t last_block_id = 0;

    // Number of LOD detail levels. Observed = 9 everywhere.
    std::int32_t detail_count = 0;

    // Power-of-two world-units-per-tile: 1 tile = (1 << scale) metres.
    // Observed = 3 (= 8 m per tile).
    std::int32_t scale = 0;

    // 24-byte axis-aligned-bounds region. The spec calls this opaque:
    // every shipping mission stores all zeros here. Surfaced raw so
    // a future revision can interpret it without an ABI break.
    std::array<std::byte, 24> bounds_raw{};

    // Block-space origin offset (2 i32). Always (0, 0) in shipping
    // content; surfaced for forward-compatibility.
    std::array<std::int32_t, 2> origin{};

    // Altitude envelope in metres for the mission terrain. Bit-exact
    // f32 values; cross-checked against the same fields inside the
    // sibling `.dtb`.
    float height_min = 0.0f;
    float height_max = 0.0f;

    // Grid dimensions in block-units. Observed (3, 3) in every
    // shipping mission, so `block_map.size() == 9`.
    std::array<std::int32_t, 2> size{};

    // Block layout enum. Only value 0 (= "every cell maps to one
    // common block") is supported by v1. Non-zero values produce a
    // warn-and-continue at parse time.
    std::uint32_t block_pattern = 0;

    // size[0] * size[1] entries. Every cell holds the block id of
    // the named block in `block_list` that backs it.
    std::vector<dtf_block_ref> block_map;

    // Trailing list of named blocks. Shipping content has exactly
    // one entry with id 0 and an empty name; the entry is paired
    // with the matching nameId field in the sibling `.dtb`.
    std::vector<dtf_block_list_entry> block_list;
  };

  // Returns true and leaves the stream position unchanged when the
  // next four bytes are the `GFIL` magic. Returns false otherwise
  // (also restoring the original position).
  bool is_darkstar_dtf(std::istream& stream);

  // Parse a DTF chunk from `src` starting at the current stream
  // position. Returns std::nullopt when:
  //   - the file does not begin with `GFIL`,
  //   - the chunk size does not match the file's remaining length,
  //   - the payload version is not 1,
  //   - any length-prefixed string overflows the chunk payload,
  //   - `size` falls outside the sanity range (1..16 per axis),
  //   - the stream fails before the trailer (truncated file).
  // On success, the stream is left positioned immediately after the
  // last byte of the GFIL chunk payload.
  std::optional<dtf_file> parse_dtf(std::istream& src);
}

#endif
