#ifndef DARKSTAR_CONTENT_TERRAIN_WORLD_DATS_HPP
#define DARKSTAR_CONTENT_TERRAIN_WORLD_DATS_HPP

// Parsers for the three per-world data files inside <world>Terrain.vol:
//   - <world>.Terrain.dat   - texture record array
//   - <world>.Grid.dat      - per-corner texture pick tables
//   - <world>.Rules.dat     - binary procedural surface rules
//
// Layout was derived from hex inspection of the six worlds shipped in
// the Tribes 1.41 freeware corpus (alien, desert, ice, lush, mars,
// mud), cross-checked against the clean-room spec at
// `docs/clean-room-specs/TERRAIN.md`. No leaked engine source was
// consulted while writing this parser.
//
// The DML for each world is parsed by the existing
// `studio::content::dts::darkstar::material_list` reader; that linkage
// lives outside this header.

#include <array>
#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace studio::content::terrain
{
  // ---------------------------------------------------------------
  // <world>.Terrain.dat
  // ---------------------------------------------------------------

  // One 276-byte record per texture in the world's terrain palette.
  struct terrain_dat_record
  {
    // ASCII texture filename, NUL-trimmed. Resolves into the parent
    // world's DML by exact-name match (e.g. "lCCCC.BMP").
    std::string bitmap_name;

    // 128 bytes of always-zero space inside each on-disk record.
    // Preserved as opaque per the spec's recommendation; semantics
    // not pinned down (likely an engine-time cache region that the
    // serialiser never populates for shipping content).
    std::array<std::uint8_t, 128> reserved_block{};

    // Per-corner terrain-type tags in the order (ul, ur, lr, ll).
    // Each byte indexes a row in `terrain_dat::type_descriptions`,
    // and the same 4-byte tuple appears in `grid_dat::per_texture_block`
    // for the same texture index.
    std::array<std::uint8_t, 4> corner_type_tags{};

    // u32 bitmask describing texture-edge / orientation flags.
    // Observed values look like a packed bitset with a trailing
    // 0xff sentinel (`0x000014ff`, `0x000019ff`, ...). Preserved
    // raw; the renderer does not need it for v1.
    std::uint32_t sides = 0;

    // 4-byte classification word at on-disk offset +264. Semantics
    // are deliberately under-specified - observed values cluster as
    // small integers in the low byte with the upper three bytes
    // zero. Surface to downstream code unmodified.
    std::uint32_t classifier_word = 0;

    // Physical-material constants in the engine's surface model.
    // Observed: 0.5 elasticity and 1.0 friction in every shipping
    // record. Preserved as f32 in case modded content varies them.
    float elasticity = 0.0f;
    float friction = 0.0f;
  };

  struct terrain_dat
  {
    // NUL-trimmed ASCII description lines, one per terrain type.
    // Length equals on-disk `numTypes`; each on-disk slot is a
    // fixed 32-byte field.
    std::vector<std::string> type_descriptions;

    // One record per texture. Length equals on-disk `numTextures`.
    std::vector<terrain_dat_record> records;
  };

  // Parse a `<world>.Terrain.dat`. Returns std::nullopt when:
  //   - the file is shorter than the 8-byte fixed header,
  //   - any of the count words exceeds the sanity caps
  //     (numTypes <= 64, numTextures <= 4096),
  //   - the computed prefix + records size does not equal the
  //     stream's end-of-data,
  //   - any read fails mid-stream.
  // On success the stream is left at end-of-file.
  std::optional<terrain_dat> parse_terrain_dat(std::istream& src);

  // ---------------------------------------------------------------
  // <world>.Grid.dat
  // ---------------------------------------------------------------

  // One (texture-index, orientation-flags) pair in the pick list.
  struct grid_pick
  {
    // Index into the parent world's Terrain.dat / DML texture array.
    std::uint8_t texture_index = 0;

    // Low 3 bits use the same plain/rotate/flipX/flipY encoding as
    // the `materialmap.flags` field documented in the DTB spec.
    std::uint8_t flags = 0;
  };

  struct grid_dat
  {
    // On-disk grid layout version; 3 is the only value observed in
    // the freeware corpus.
    std::uint32_t version = 0;

    // Number of terrain types referenced by `tex_combos`. Equal to
    // the `numTypes` field of the matching Terrain.dat.
    std::uint32_t num_base_types = 0;

    // Number of textures referenced by `per_texture_block`. Equal
    // to the `numTextures` field of the matching Terrain.dat.
    std::uint32_t num_base_textures = 0;

    // u32 header word labelled `numPicks` on disk. Always zero in
    // shipping content - retained so that callers wanting the raw
    // header word can read it; do NOT use it to size `pick_list`.
    std::uint32_t num_picks_header = 0;

    // Duplicates the type descriptions found in the matching
    // Terrain.dat (one 32-byte NUL-padded slot per type).
    std::vector<std::string> type_descriptions;

    // For each texture, the same (ul, ur, lr, ll) corner-type tuple
    // exposed at `terrain_dat_record::corner_type_tags`. Length is
    // `num_base_textures`.
    std::vector<std::array<std::uint8_t, 4>> per_texture_block;

    // (numBaseTypes + 1)^4 entries indexed by the packed key
    //   key = ul + ur*B + lr*B^2 + ll*B^3
    // where each component is 0..numBaseTypes and B = numBaseTypes + 1.
    // `tex_combos[key]` is the starting index into `pick_list`.
    std::vector<std::uint32_t> tex_combos;

    // (numBaseTypes + 1)^4 + 1 entries. `pick_offs[k]` is the
    // index in `pick_list` one past the end of the slice for combo
    // `k`. The trailing sentinel equals `pick_list.size()`.
    std::vector<std::uint32_t> pick_offs;

    // Variable-length tail; each entry is (texture_index, flags).
    // Sized as `(file_size - fixed_prefix) / 2`.
    std::vector<grid_pick> pick_list;
  };

  // Parse a `<world>.Grid.dat`. Returns std::nullopt when:
  //   - the file is shorter than the 16-byte fixed header,
  //   - `version != 3`,
  //   - count words exceed sanity caps,
  //   - the computed pickList byte length is negative or odd,
  //   - the trailing `pick_offs` sentinel disagrees with the
  //     pickList length,
  //   - any read fails mid-stream.
  // On success the stream is left at end-of-file.
  std::optional<grid_dat> parse_grid_dat(std::istream& src);

  // ---------------------------------------------------------------
  // <world>.Rules.dat
  // ---------------------------------------------------------------

  // One 52-byte procedural-landscaping rule. Carries the altitude
  // and slope distribution criteria the editor uses when scattering
  // textures across the heightmap.
  struct rules_dat_record
  {
    std::int32_t group_num = 0;

    float alt_min = 0.0f;
    float alt_max = 0.0f;
    float alt_mean = 0.0f;
    float alt_sdev = 0.0f;
    float alt_weight = 0.0f;
    std::int32_t adj_heights = 0;

    float slope_min = 0.0f;
    float slope_max = 0.0f;
    float slope_mean = 0.0f;
    float slope_sdev = 0.0f;
    float slope_weight = 0.0f;
    std::int32_t adj_slopes = 0;
  };

  struct rules_dat
  {
    // No persisted version word for this format; included here for
    // future-proofing if a v2 file ever appears.
    std::uint32_t version = 0;

    std::vector<rules_dat_record> rules;
  };

  // Parse a `<world>.Rules.dat`. Returns std::nullopt when the
  // first u32 (`numRules`) is greater than 256 or when the file
  // size does not equal `4 + numRules * 52`.
  std::optional<rules_dat> parse_rules_dat(std::istream& src);
}

#endif
