#ifndef DARKSTAR_CONTENT_DIG_HPP
#define DARKSTAR_CONTENT_DIG_HPP

// Parser for Tribes 1 interior GEOMETRY files (`*.dig`).
//
// DIG is a `PERS`-wrapped binary blob carrying a class named
// `ITRGeometry` version 7. After the standard Darkstar PERS
// container (`PERS` + chunkSize + className + version), the payload
// holds the BSP-partitioned static interior mesh:
//
//   - a content-hash `build_id` that pairs the DIG with its DIL,
//   - a global UV texture scale + axis-aligned bounding box,
//   - nine `s32` count fields, one per following array,
//   - surfaces      (textured polygons referencing the DML),
//   - BSP nodes     (8-byte interior nodes),
//   - BSP solid leaves (12 bytes each),
//   - BSP empty leaves (44 bytes each — includes per-leaf AABB),
//   - PVS bitlist   (raw `u8` array, not decoded here),
//   - packed vertices (point-index + texture-index pairs),
//   - 3D points     (float32 × 3 per vertex position),
//   - 2D points     (float32 × 2 per UV),
//   - planes        (point + signed distance, used by surfaces/BSP),
//   - trailer       (highestMipLevel + flags).
//
// Element byte layouts mirror tekrog's Kaitai Struct definition for
// `ItrGeometry` (community RE, MIT-style on GitHub) — see
// https://github.com/tekrog/TribesToBlender/blob/master/dts.py
// — and were independently hex-verified against all 526 DIG files in
// the Tribes 1.41 freeware corpus. No leaked Dynamix source was
// consulted.

#include <cstdint>
#include <istream>
#include <optional>
#include <vector>

namespace studio::content::dig
{
  // 1 byte of bit-packed flags followed by the per-surface index
  // fields. Total on disk: 20 bytes. The packing order matches the
  // Kaitai spec which uses MSB-first bit-reads:
  //
  //   bit 7    : type
  //   bits 6-3 : texture_scale_shift (4 bits)
  //   bit 2    : apply_ambient
  //   bit 1    : visible_to_outside
  //   bit 0    : plane_front
  struct surface
  {
    bool          type = false;
    std::uint8_t  texture_scale_shift = 0;
    bool          apply_ambient = false;
    bool          visible_to_outside = false;
    bool          plane_front = false;

    // Index into the parent interior's DML.
    std::uint8_t  material = 0;

    // Texture mapping in the surface's local texture space. Both are
    // u8 × 2 on disk (1 byte each component).
    std::uint8_t  texture_size_x = 0;
    std::uint8_t  texture_size_y = 0;
    std::uint8_t  texture_offset_x = 0;
    std::uint8_t  texture_offset_y = 0;

    std::uint16_t plane_index = 0;
    std::uint32_t vertex_index = 0;
    std::uint32_t point_index = 0;
    std::uint8_t  vertex_count = 0;
    std::uint8_t  point_count = 0;
  };

  // BSP interior node — 8 bytes on disk.
  struct bsp_node
  {
    std::uint16_t plane_index = 0;
    std::int16_t  front = 0;
    std::int16_t  back = 0;
    std::int16_t  fill = 0;
  };

  // BSP solid leaf — 12 bytes on disk. The two `dummy` fields are
  // preserved verbatim so callers can sanity-check exotic files.
  struct bsp_solid_leaf
  {
    std::uint16_t dummy = 0;
    std::uint16_t surface_index = 0;
    std::uint16_t dummy2 = 0;
    std::uint16_t plane_index = 0;
    std::uint16_t surface_count = 0;
    std::uint16_t plane_count = 0;
  };

  // BSP empty leaf — 44 bytes on disk. Holds a per-leaf AABB, the
  // surface/plane ranges that bound the leaf and the PVS bitlist
  // offset used to ask "which other empty leaves can be seen from
  // this one".
  //
  // On-disk packing of the leading flags word:
  //   bit 15   : flags
  //   bits 14-0: pvs_count (15-bit unsigned)
  struct bsp_empty_leaf
  {
    bool          flags = false;
    std::uint16_t pvs_count = 0;
    std::uint16_t surface_count = 0;
    std::uint32_t pvs_index = 0;
    std::uint32_t surface_index = 0;
    std::uint32_t plane_index = 0;
    float         box_min_x = 0.0f;
    float         box_min_y = 0.0f;
    float         box_min_z = 0.0f;
    float         box_max_x = 0.0f;
    float         box_max_y = 0.0f;
    float         box_max_z = 0.0f;
    std::uint16_t plane_count = 0;
    std::uint16_t dummy = 0;
  };

  // Packed (point_index, texture_index) pair — 4 bytes on disk.
  struct packed_vertex
  {
    std::uint16_t point_index = 0;
    std::uint16_t texture_index = 0;
  };

  struct vec3
  {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
  };

  struct vec2
  {
    float x = 0.0f;
    float y = 0.0f;
  };

  // Plane in point-normal form. The `point` is a precomputed reference
  // point on the plane (essentially the plane's normal vector) and `d`
  // is the signed distance from the origin.
  struct plane
  {
    vec3  point;
    float d = 0.0f;
  };

  struct dig_file
  {
    // Must equal the corresponding DIL's geometryBuildId for the pair
    // to be considered up-to-date (see docs/research/DIS-DIL.md).
    std::int32_t build_id = 0;

    // Global UV scale applied to every surface (a single float; the
    // engine combines this with each surface's texture_scale_shift).
    float        texture_scale = 0.0f;

    vec3         bbox_min;
    vec3         bbox_max;

    std::vector<surface>        surfaces;
    std::vector<bsp_node>       bsp_nodes;
    std::vector<bsp_solid_leaf> bsp_solid_leaves;
    std::vector<bsp_empty_leaf> bsp_empty_leaves;
    std::vector<std::uint8_t>   pvs_bitlist;
    std::vector<packed_vertex>  vertices;
    std::vector<vec3>           points3;
    std::vector<vec2>           points2;
    std::vector<plane>          planes;

    std::int32_t  highest_mip_level = 0;
    std::uint32_t flags = 0;
  };

  // Returns true (leaving the stream position unchanged) when the
  // next bytes are a PERS-wrapped `ITRGeometry` class. Returns false
  // for any other magic or class name and on read failure.
  bool is_dig_file(std::istream& stream);

  // Parse a DIG file from `src` starting at the current stream
  // position. Returns std::nullopt when:
  //   - the file does not begin with `PERS`,
  //   - the embedded class name is not `ITRGeometry`,
  //   - the version is not 7,
  //   - any array size is negative or would overflow the chunk,
  //   - the stream fails mid-parse.
  // On success, the stream is left positioned at the end of the
  // declared PERS chunk (header + chunkSize bytes).
  std::optional<dig_file> read_dig_file(std::istream& src);
}

#endif
