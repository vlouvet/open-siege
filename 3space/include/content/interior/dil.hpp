#ifndef DARKSTAR_CONTENT_INTERIOR_DIL_HPP
#define DARKSTAR_CONTENT_INTERIOR_DIL_HPP

// Parser for Tribes 1 interior lighting files (`*.dil`).
//
// A DIL file wraps a `PERS` persistent-object container carrying one of
// two class variants:
//
//   ITRLighting         (11 chars, v7) — per-world static bake
//   ITRMissionLighting  (18 chars, v7) — per-mission instance delta
//
// The payload holds an animated-light state machine, a per-surface
// lightmap binding table, a Huffman-compressed lightmap bit-stream, and
// (for mission files) an LZH-compressed IndexEntry remap tail.
//
// Byte layout specified in docs/clean-room-specs/DIL-INNER.md.
// Element sizes (State=16, StateData=8, Light=24, Surface=12,
// HuffmanNode=8, HuffmanLeaf=4) were verified by corpus ablation across
// 487 shipping Tribes 1.41 DIL files. No leaked Dynamix engine source
// was consulted.

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace studio::content::interior
{
  // One keyframe in an animated light's modulation cycle. 16 bytes on disk.
  struct dil_state
  {
    std::uint16_t red            = 0;  // colour channel, fixed-point
    std::uint16_t green          = 0;
    std::uint16_t blue           = 0;
    std::uint16_t mono           = 0;  // brightness channel
    float         animation_time = 0.0f; // keyframe wall-clock time (s)
    std::uint16_t data_count     = 0;  // how many StateData entries this state owns
    std::uint16_t data_index     = 0;  // base index into state_data[]
  };

  // Per-state, per-(surface, light) record. 8 bytes on disk.
  struct dil_state_data
  {
    std::uint16_t surface     = 0;    // index into surfaces[]
    std::uint16_t light_index = 0;    // index into lights[]
    std::uint32_t map_index   = 0;    // byte offset into map_data[]
  };

  // One animated light source. 24 bytes on disk.
  struct dil_light
  {
    std::uint32_t id                 = 0;  // engine-side unique ID
    std::uint32_t name_index         = 0;  // byte offset into name_buffer
    std::uint32_t state_count        = 0;
    std::uint32_t state_index        = 0;  // base index into states[]
    float         animation_duration = 0.0f; // total cycle length (s)
    std::uint32_t animation_flags    = 0;  // bit 0 = autostart; treat as opaque
  };

  // Per-DIG-surface lightmap binding. 12 bytes on disk.
  //
  // map_index_or_color encoding:
  //   bit 30 set   -> low 30 bits are a byte offset into map_data[]
  //                   where this surface's Huffman bit-stream starts
  //   bit 30 clear -> low 16 bits are a flat IRGB 4:4:4:4 colour
  //                   (surface has no baked lightmap)
  struct dil_surface
  {
    std::uint32_t map_index_or_color = 0;
    std::uint16_t light_count        = 0;
    std::uint16_t light_index        = 0;  // base index into lights[]
    std::uint8_t  map_size_x         = 0;  // lightmap width in texels
    std::uint8_t  map_size_y         = 0;  // lightmap height in texels
    std::uint8_t  map_offset_x       = 0;  // atlas U offset
    std::uint8_t  map_offset_y       = 0;  // atlas V offset
  };

  // Mission-lighting surface remap entry. 8 bytes on disk (decoded).
  struct dil_index_entry
  {
    std::int32_t src_index  = 0;  // surface index in the stock ITRLighting
    std::int32_t dest_index = 0;  // substitute surface to use instead
  };

  // Huffman tree node. 8 bytes on disk.
  // A non-negative child index is a node-table reference.
  // A negative child index encodes a leaf: leaf_index = ~child.
  struct dil_huffman_node
  {
    std::int32_t index_zero = 0;  // child when next bit is 0
    std::int32_t index_one  = 0;  // child when next bit is 1
  };

  // Huffman tree leaf. 4 bytes on disk.
  struct dil_huffman_leaf
  {
    std::uint16_t colour  = 0;   // IRGB 4:4:4:4 lightmap pixel
    std::uint16_t padding = 0;   // always 0 in shipping content; discard
  };

  struct dil_file
  {
    // True when the PERS class is ITRMissionLighting.
    bool is_mission_lighting = false;

    // Payload header fields (32 bytes).
    std::int32_t geometry_build_id  = 0;  // must match the DIG's build_id
    std::int32_t light_scale_shift  = 0;  // always 4 in shipping content
    std::int32_t lightmap_count     = 0;  // opaque hint; no array on disk

    // Record arrays.
    std::vector<dil_state>      states;
    std::vector<dil_state_data> state_data;
    std::vector<dil_light>      lights;
    std::vector<dil_surface>    surfaces;

    // Huffman-compressed lightmap bit-stream (raw bytes, map_data_size bytes).
    std::vector<std::byte> map_data;

    // Concatenated NUL-terminated ASCII light names. Use
    // dil_light::name_index as a byte offset into this buffer.
    std::string name_buffer;

    // Huffman tree (stored for later decode; see DIL-INNER.md §6).
    // Root node is nodes.back() — the engine writes the tree bottom-up.
    std::vector<dil_huffman_node> huffman_nodes;
    std::vector<dil_huffman_leaf> huffman_leaves;

    // Mission-lighting remap table (empty for stock ITRLighting).
    // On-disk format: s32 index_array_size + LZH-compressed blob.
    std::vector<dil_index_entry> index_remap;
  };

  // Peek at the stream and return true if the next bytes are PERS magic
  // followed by either "ITRLighting" or "ITRMissionLighting".
  // Stream position is unchanged on both return values.
  bool is_darkstar_dil(std::istream& src);

  // Parse a DIL file from `src` starting at the current stream position.
  // Returns std::nullopt when:
  //   - PERS magic is absent,
  //   - the class name is neither ITRLighting nor ITRMissionLighting,
  //   - the version field is not 7,
  //   - the stream is truncated,
  //   - the Huffman tree violates the L == N+1 invariant,
  //   - the LZH decoder (mission tail) fails.
  // On success the stream is left positioned immediately after the last
  // consumed byte (end of ITRLighting data, or end of LZH tail for
  // ITRMissionLighting).
  std::optional<dil_file> parse_dil(std::istream& src);

} // namespace studio::content::interior

#endif // DARKSTAR_CONTENT_INTERIOR_DIL_HPP
