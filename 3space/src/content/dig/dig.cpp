#include "content/dig/dig.hpp"

#include <array>
#include <cstring>
#include <limits>

namespace studio::content::dig
{
  namespace
  {
    constexpr std::array<char, 4> pers_magic{'P', 'E', 'R', 'S'};
    constexpr char itr_geometry_name[] = "ITRGeometry";
    constexpr std::uint16_t itr_geometry_name_len = 11;
    constexpr std::uint32_t expected_version = 7;

    // ---------------- Little-endian primitive readers ----------------

    bool read_u8(std::istream& s, std::uint8_t& out)
    {
      char c;
      if (!s.get(c)) return false;
      out = static_cast<std::uint8_t>(c);
      return true;
    }

    bool read_u16(std::istream& s, std::uint16_t& out)
    {
      unsigned char b[2];
      if (!s.read(reinterpret_cast<char*>(b), 2)) return false;
      out = static_cast<std::uint16_t>(b[0])
          | (static_cast<std::uint16_t>(b[1]) << 8);
      return true;
    }

    bool read_s16(std::istream& s, std::int16_t& out)
    {
      std::uint16_t u;
      if (!read_u16(s, u)) return false;
      out = static_cast<std::int16_t>(u);
      return true;
    }

    bool read_u32(std::istream& s, std::uint32_t& out)
    {
      unsigned char b[4];
      if (!s.read(reinterpret_cast<char*>(b), 4)) return false;
      out = static_cast<std::uint32_t>(b[0])
          | (static_cast<std::uint32_t>(b[1]) << 8)
          | (static_cast<std::uint32_t>(b[2]) << 16)
          | (static_cast<std::uint32_t>(b[3]) << 24);
      return true;
    }

    bool read_s32(std::istream& s, std::int32_t& out)
    {
      std::uint32_t u;
      if (!read_u32(s, u)) return false;
      out = static_cast<std::int32_t>(u);
      return true;
    }

    bool read_f32(std::istream& s, float& out)
    {
      std::uint32_t u;
      if (!read_u32(s, u)) return false;
      static_assert(sizeof(float) == 4, "float must be 4 bytes");
      std::memcpy(&out, &u, 4);
      return true;
    }

    bool read_vec3(std::istream& s, vec3& v)
    {
      return read_f32(s, v.x) && read_f32(s, v.y) && read_f32(s, v.z);
    }

    bool read_vec2(std::istream& s, vec2& v)
    {
      return read_f32(s, v.x) && read_f32(s, v.y);
    }

    // ---------------- Element-level readers ----------------

    bool read_surface(std::istream& s, surface& out)
    {
      std::uint8_t flags = 0;
      if (!read_u8(s, flags)) return false;
      // Kaitai uses MSB-first bit packing here. Mirror that exactly.
      out.type               = (flags & 0x80) != 0;
      out.texture_scale_shift = static_cast<std::uint8_t>((flags >> 3) & 0x0f);
      out.apply_ambient      = (flags & 0x04) != 0;
      out.visible_to_outside = (flags & 0x02) != 0;
      out.plane_front        = (flags & 0x01) != 0;

      if (!read_u8(s, out.material)) return false;
      if (!read_u8(s, out.texture_size_x)) return false;
      if (!read_u8(s, out.texture_size_y)) return false;
      if (!read_u8(s, out.texture_offset_x)) return false;
      if (!read_u8(s, out.texture_offset_y)) return false;
      if (!read_u16(s, out.plane_index)) return false;
      if (!read_u32(s, out.vertex_index)) return false;
      if (!read_u32(s, out.point_index)) return false;
      if (!read_u8(s, out.vertex_count)) return false;
      if (!read_u8(s, out.point_count)) return false;
      std::uint16_t dummy;
      if (!read_u16(s, dummy)) return false;
      return true;
    }

    bool read_bsp_node(std::istream& s, bsp_node& out)
    {
      return read_u16(s, out.plane_index)
          && read_s16(s, out.front)
          && read_s16(s, out.back)
          && read_s16(s, out.fill);
    }

    bool read_bsp_solid_leaf(std::istream& s, bsp_solid_leaf& out)
    {
      return read_u16(s, out.dummy)
          && read_u16(s, out.surface_index)
          && read_u16(s, out.dummy2)
          && read_u16(s, out.plane_index)
          && read_u16(s, out.surface_count)
          && read_u16(s, out.plane_count);
    }

    bool read_bsp_empty_leaf(std::istream& s, bsp_empty_leaf& out)
    {
      std::uint16_t packed = 0;
      if (!read_u16(s, packed)) return false;
      // Kaitai bit layout (MSB-first): 1 bit flags, 15 bits pvs_count.
      // After a 2-byte big-endian-style bit-read the stream aligns to
      // the next byte; in practice we just read those 2 bytes as a
      // little-endian u16 and unpack manually. The packed u16 is
      // stored as `(flags_bit << 15) | pvs_count` in the on-disk
      // little-endian order — same numeric value the Kaitai reader
      // ends up producing.
      out.flags = (packed & 0x8000) != 0;
      out.pvs_count = static_cast<std::uint16_t>(packed & 0x7fff);

      if (!read_u16(s, out.surface_count)) return false;
      if (!read_u32(s, out.pvs_index)) return false;
      if (!read_u32(s, out.surface_index)) return false;
      if (!read_u32(s, out.plane_index)) return false;
      if (!read_f32(s, out.box_min_x)) return false;
      if (!read_f32(s, out.box_min_y)) return false;
      if (!read_f32(s, out.box_min_z)) return false;
      if (!read_f32(s, out.box_max_x)) return false;
      if (!read_f32(s, out.box_max_y)) return false;
      if (!read_f32(s, out.box_max_z)) return false;
      if (!read_u16(s, out.plane_count)) return false;
      if (!read_u16(s, out.dummy)) return false;
      return true;
    }

    bool read_packed_vertex(std::istream& s, packed_vertex& out)
    {
      return read_u16(s, out.point_index)
          && read_u16(s, out.texture_index);
    }

    bool read_plane(std::istream& s, plane& out)
    {
      return read_vec3(s, out.point) && read_f32(s, out.d);
    }

    // ---------------- PERS wrapper ----------------

    // Consume the PERS header and confirm class name + version. On
    // success, leaves the stream positioned at the start of the
    // payload and writes the chunk's declared size (everything after
    // the first 8 bytes) to `chunk_size`. On failure, restores the
    // initial stream position.
    bool read_pers_header(std::istream& s,
                          std::streampos start,
                          std::uint32_t& chunk_size)
    {
      char magic[4];
      if (!s.read(magic, 4))
      {
        s.clear();
        s.seekg(start, std::ios::beg);
        return false;
      }
      if (std::memcmp(magic, pers_magic.data(), 4) != 0)
      {
        s.clear();
        s.seekg(start, std::ios::beg);
        return false;
      }
      if (!read_u32(s, chunk_size))
      {
        s.clear();
        s.seekg(start, std::ios::beg);
        return false;
      }
      std::uint16_t name_len = 0;
      if (!read_u16(s, name_len))
      {
        s.clear();
        s.seekg(start, std::ios::beg);
        return false;
      }
      if (name_len != itr_geometry_name_len)
      {
        s.clear();
        s.seekg(start, std::ios::beg);
        return false;
      }
      // The Darkstar serialiser pads the class name to an even byte
      // count: padded_len = (name_len + 1) & ~1. For "ITRGeometry"
      // (11 chars) that is 12 bytes — one NUL pad byte.
      const std::uint16_t padded_len =
        static_cast<std::uint16_t>((name_len + 1) & ~1);
      char name_buf[32];
      if (padded_len > sizeof(name_buf))
      {
        s.clear();
        s.seekg(start, std::ios::beg);
        return false;
      }
      if (!s.read(name_buf, padded_len))
      {
        s.clear();
        s.seekg(start, std::ios::beg);
        return false;
      }
      if (std::memcmp(name_buf, itr_geometry_name, name_len) != 0)
      {
        s.clear();
        s.seekg(start, std::ios::beg);
        return false;
      }
      std::uint32_t version = 0;
      if (!read_u32(s, version))
      {
        s.clear();
        s.seekg(start, std::ios::beg);
        return false;
      }
      if (version != expected_version)
      {
        s.clear();
        s.seekg(start, std::ios::beg);
        return false;
      }
      return true;
    }

    // Defensive cap — every shipping DIG fits comfortably in this.
    constexpr std::int32_t max_array_size = 10'000'000;

    bool valid_count(std::int32_t v)
    {
      return v >= 0 && v <= max_array_size;
    }
  }

  bool is_dig_file(std::istream& stream)
  {
    const auto start = stream.tellg();
    char header[26];
    bool ok = static_cast<bool>(stream.read(header, sizeof(header)));
    if (ok)
    {
      ok = std::memcmp(header, pers_magic.data(), 4) == 0
        && std::memcmp(header + 10, itr_geometry_name,
                       itr_geometry_name_len) == 0;
    }
    stream.clear();
    stream.seekg(start, std::ios::beg);
    return ok;
  }

  std::optional<dig_file> read_dig_file(std::istream& src)
  {
    const auto start = src.tellg();

    auto fail = [&]() -> std::optional<dig_file> {
      src.clear();
      src.seekg(start, std::ios::beg);
      return std::nullopt;
    };

    std::uint32_t chunk_size = 0;
    if (!read_pers_header(src, start, chunk_size))
    {
      return std::nullopt;
    }

    // Anchor: position at the start of the payload. chunk_size counts
    // every byte after the first 8 (`PERS` + chunk_size_field).
    const auto payload_start = src.tellg();

    dig_file out;

    if (!read_s32(src, out.build_id)) return fail();
    if (!read_f32(src, out.texture_scale)) return fail();
    if (!read_vec3(src, out.bbox_min)) return fail();
    if (!read_vec3(src, out.bbox_max)) return fail();

    std::int32_t surface_count = 0, node_count = 0, solid_leaf_count = 0,
                  empty_leaf_count = 0, bitlist_size = 0, vertex_count = 0,
                  point3_count = 0, point2_count = 0, plane_count = 0;
    if (!read_s32(src, surface_count)) return fail();
    if (!read_s32(src, node_count)) return fail();
    if (!read_s32(src, solid_leaf_count)) return fail();
    if (!read_s32(src, empty_leaf_count)) return fail();
    if (!read_s32(src, bitlist_size)) return fail();
    if (!read_s32(src, vertex_count)) return fail();
    if (!read_s32(src, point3_count)) return fail();
    if (!read_s32(src, point2_count)) return fail();
    if (!read_s32(src, plane_count)) return fail();

    if (!valid_count(surface_count) || !valid_count(node_count)
        || !valid_count(solid_leaf_count) || !valid_count(empty_leaf_count)
        || !valid_count(bitlist_size) || !valid_count(vertex_count)
        || !valid_count(point3_count) || !valid_count(point2_count)
        || !valid_count(plane_count))
    {
      return fail();
    }

    // Determine the declared chunk end so we can refuse arrays that
    // would extend past it. `chunk_size` is the byte count of
    // everything after the first 8 bytes of the file (i.e. starting
    // at byte offset 8, which is `classname_len`). Hence the chunk
    // ends at `start + 8 + chunk_size`.
    if (start == std::streampos(-1)) return fail();
    const auto chunk_end =
      start + static_cast<std::streamoff>(8)
            + static_cast<std::streamoff>(chunk_size);

    auto bytes_remaining = [&](std::streampos pos) -> std::int64_t {
      return static_cast<std::int64_t>(chunk_end - pos);
    };

    // Helper that asserts at least `n` bytes remain inside the chunk
    // before reading them.
    auto need = [&](std::int64_t n) -> bool {
      const auto cur = src.tellg();
      if (cur == std::streampos(-1)) return false;
      return bytes_remaining(cur) >= n;
    };

    // Reserve modestly — capped to avoid huge speculative allocations
    // on a corrupt file.
    auto safe_reserve = [](auto& vec, std::int32_t n) {
      constexpr std::size_t reserve_cap = 100'000;
      vec.reserve(static_cast<std::size_t>(
        n > 0 && static_cast<std::size_t>(n) < reserve_cap
          ? static_cast<std::size_t>(n)
          : 0));
    };

    out.surfaces.resize(static_cast<std::size_t>(surface_count));
    if (!need(static_cast<std::int64_t>(surface_count) * 20)) return fail();
    for (auto& s : out.surfaces)
    {
      if (!read_surface(src, s)) return fail();
    }

    out.bsp_nodes.resize(static_cast<std::size_t>(node_count));
    if (!need(static_cast<std::int64_t>(node_count) * 8)) return fail();
    for (auto& n : out.bsp_nodes)
    {
      if (!read_bsp_node(src, n)) return fail();
    }

    out.bsp_solid_leaves.resize(static_cast<std::size_t>(solid_leaf_count));
    if (!need(static_cast<std::int64_t>(solid_leaf_count) * 12)) return fail();
    for (auto& l : out.bsp_solid_leaves)
    {
      if (!read_bsp_solid_leaf(src, l)) return fail();
    }

    out.bsp_empty_leaves.resize(static_cast<std::size_t>(empty_leaf_count));
    if (!need(static_cast<std::int64_t>(empty_leaf_count) * 44)) return fail();
    for (auto& l : out.bsp_empty_leaves)
    {
      if (!read_bsp_empty_leaf(src, l)) return fail();
    }

    if (bitlist_size > 0)
    {
      if (!need(bitlist_size)) return fail();
      out.pvs_bitlist.resize(static_cast<std::size_t>(bitlist_size));
      if (!src.read(reinterpret_cast<char*>(out.pvs_bitlist.data()),
                    static_cast<std::streamsize>(bitlist_size)))
      {
        return fail();
      }
    }

    out.vertices.resize(static_cast<std::size_t>(vertex_count));
    if (!need(static_cast<std::int64_t>(vertex_count) * 4)) return fail();
    for (auto& v : out.vertices)
    {
      if (!read_packed_vertex(src, v)) return fail();
    }

    out.points3.resize(static_cast<std::size_t>(point3_count));
    if (!need(static_cast<std::int64_t>(point3_count) * 12)) return fail();
    for (auto& p : out.points3)
    {
      if (!read_vec3(src, p)) return fail();
    }

    out.points2.resize(static_cast<std::size_t>(point2_count));
    if (!need(static_cast<std::int64_t>(point2_count) * 8)) return fail();
    for (auto& p : out.points2)
    {
      if (!read_vec2(src, p)) return fail();
    }

    out.planes.resize(static_cast<std::size_t>(plane_count));
    if (!need(static_cast<std::int64_t>(plane_count) * 16)) return fail();
    for (auto& p : out.planes)
    {
      if (!read_plane(src, p)) return fail();
    }

    if (!need(8)) return fail();
    if (!read_s32(src, out.highest_mip_level)) return fail();
    if (!read_u32(src, out.flags)) return fail();

    // Leave the stream positioned at the declared chunk end so the
    // caller can chain multiple PERS objects from the same source. In
    // a well-formed file, tellg() already equals chunk_end here.
    src.clear();
    src.seekg(chunk_end, std::ios::beg);

    (void)payload_start;
    (void)safe_reserve;
    return out;
  }
}
