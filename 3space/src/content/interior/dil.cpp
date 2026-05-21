#include "content/interior/dil.hpp"
#include "content/compression/lzh.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

// DIL parser — spec: docs/clean-room-specs/DIL-INNER.md
//
// Supports both PERS class variants:
//   ITRLighting        (11 chars, NUL-padded to 12 bytes, payload @ +26)
//   ITRMissionLighting (18 chars, no pad,                 payload @ +32)
//
// PERS wrapper layout:
//   4  PERS magic
//   4  chunkSize (u32 LE) = file_size - 8
//   2  classNameLength (u16 LE)
//   N  class name bytes (no NUL written by engine)
//   P  NUL pad byte if classNameLength < 16, else none
//   4  version (u32 LE) = 7

namespace studio::content::interior
{
  namespace
  {
    constexpr std::array<char, 4> pers_magic{'P', 'E', 'R', 'S'};
    constexpr char itr_lighting_name[]         = "ITRLighting";
    constexpr char itr_mission_lighting_name[] = "ITRMissionLighting";
    constexpr std::uint16_t itr_lighting_name_len         = 11;
    constexpr std::uint16_t itr_mission_lighting_name_len = 18;
    constexpr std::uint32_t expected_version = 7;

    // Maximum array sizes to guard against malformed files.
    constexpr std::int32_t max_surface_count   = 65536;
    constexpr std::int32_t max_state_count     = 65536;
    constexpr std::int32_t max_sd_count        = 1 << 20; // 1 M
    constexpr std::int32_t max_light_count     = 65536;
    constexpr std::int32_t max_map_data_size   = 64 * 1024 * 1024; // 64 MiB
    constexpr std::int32_t max_name_buf_size   = 4 * 1024 * 1024;  // 4 MiB
    constexpr std::int32_t max_huffman_nodes   = 65536;
    constexpr std::int32_t max_index_entries   = 1 << 20;

    // ----------------------------------------------------------------
    // Primitive readers — little-endian, return false on stream failure
    // ----------------------------------------------------------------

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

    // ----------------------------------------------------------------
    // PERS header reader
    //
    // On success: stream is positioned at the start of the payload;
    //             is_mission set; chunk_size written.
    // On failure: stream position restored to `start`.
    // ----------------------------------------------------------------

    bool read_pers_header(std::istream& s,
                          std::streampos start,
                          bool& is_mission,
                          std::uint32_t& chunk_size)
    {
      auto restore = [&]() {
        s.clear();
        s.seekg(start, std::ios::beg);
      };

      char magic[4];
      if (!s.read(magic, 4)) { restore(); return false; }
      if (std::memcmp(magic, pers_magic.data(), 4) != 0) { restore(); return false; }

      if (!read_u32(s, chunk_size)) { restore(); return false; }

      std::uint16_t name_len = 0;
      if (!read_u16(s, name_len)) { restore(); return false; }

      // Accept only the two known class name lengths.
      if (name_len != itr_lighting_name_len &&
          name_len != itr_mission_lighting_name_len)
      {
        restore(); return false;
      }

      // The Darkstar serialiser pads the name with a NUL byte when
      // classNameLength < 16 (odd or even — the rule is strictly
      // "length < 16 => one pad byte").  For ITRLighting (11 < 16) one
      // NUL follows; for ITRMissionLighting (18 >= 16) none.
      const bool has_pad = (name_len < 16);
      const std::uint16_t read_len =
        static_cast<std::uint16_t>(name_len + (has_pad ? 1 : 0));

      char name_buf[32] = {};
      if (read_len > 32) { restore(); return false; }
      if (!s.read(name_buf, read_len)) { restore(); return false; }

      // Verify class name bytes (padding byte not checked — always 0).
      if (name_len == itr_lighting_name_len)
      {
        if (std::memcmp(name_buf, itr_lighting_name, itr_lighting_name_len) != 0)
        {
          restore(); return false;
        }
        is_mission = false;
      }
      else
      {
        if (std::memcmp(name_buf,
                        itr_mission_lighting_name,
                        itr_mission_lighting_name_len) != 0)
        {
          restore(); return false;
        }
        is_mission = true;
      }

      std::uint32_t version = 0;
      if (!read_u32(s, version)) { restore(); return false; }
      if (version != expected_version) { restore(); return false; }

      return true;
    }

    // ----------------------------------------------------------------
    // Record readers
    // ----------------------------------------------------------------

    bool read_state(std::istream& s, dil_state& out)
    {
      return read_u16(s, out.red)
          && read_u16(s, out.green)
          && read_u16(s, out.blue)
          && read_u16(s, out.mono)
          && read_f32(s, out.animation_time)
          && read_u16(s, out.data_count)
          && read_u16(s, out.data_index);
    }

    bool read_state_data(std::istream& s, dil_state_data& out)
    {
      return read_u16(s, out.surface)
          && read_u16(s, out.light_index)
          && read_u32(s, out.map_index);
    }

    bool read_light(std::istream& s, dil_light& out)
    {
      return read_u32(s, out.id)
          && read_u32(s, out.name_index)
          && read_u32(s, out.state_count)
          && read_u32(s, out.state_index)
          && read_f32(s, out.animation_duration)
          && read_u32(s, out.animation_flags);
    }

    bool read_surface(std::istream& s, dil_surface& out)
    {
      return read_u32(s, out.map_index_or_color)
          && read_u16(s, out.light_count)
          && read_u16(s, out.light_index)
          && read_u8(s, out.map_size_x)
          && read_u8(s, out.map_size_y)
          && read_u8(s, out.map_offset_x)
          && read_u8(s, out.map_offset_y);
    }

    bool read_huffman_node(std::istream& s, dil_huffman_node& out)
    {
      return read_s32(s, out.index_zero)
          && read_s32(s, out.index_one);
    }

    bool read_huffman_leaf(std::istream& s, dil_huffman_leaf& out)
    {
      return read_u16(s, out.colour)
          && read_u16(s, out.padding);
    }

  } // anonymous namespace

  // ------------------------------------------------------------------

  bool is_darkstar_dil(std::istream& src)
  {
    auto start = src.tellg();
    src.clear();

    char magic[4];
    if (!src.read(magic, 4)) { src.clear(); src.seekg(start); return false; }
    if (std::memcmp(magic, pers_magic.data(), 4) != 0)
    {
      src.clear(); src.seekg(start); return false;
    }

    // Skip chunkSize.
    std::uint32_t dummy = 0;
    if (!read_u32(src, dummy)) { src.clear(); src.seekg(start); return false; }

    std::uint16_t name_len = 0;
    if (!read_u16(src, name_len)) { src.clear(); src.seekg(start); return false; }

    bool is_dil = false;
    if (name_len == itr_lighting_name_len || name_len == itr_mission_lighting_name_len)
    {
      const bool has_pad = (name_len < 16);
      const auto read_len = static_cast<std::uint16_t>(name_len + (has_pad ? 1 : 0));
      char name_buf[32] = {};
      if (read_len <= 32 && src.read(name_buf, read_len))
      {
        if (name_len == itr_lighting_name_len)
        {
          is_dil = (std::memcmp(name_buf, itr_lighting_name, itr_lighting_name_len) == 0);
        }
        else
        {
          is_dil = (std::memcmp(name_buf,
                                itr_mission_lighting_name,
                                itr_mission_lighting_name_len) == 0);
        }
      }
    }

    src.clear();
    src.seekg(start);
    return is_dil;
  }

  // ------------------------------------------------------------------

  std::optional<dil_file> parse_dil(std::istream& src)
  {
    auto start = src.tellg();
    auto fail  = [&]() -> std::optional<dil_file> {
      src.clear();
      src.seekg(start, std::ios::beg);
      return std::nullopt;
    };

    // ---- PERS wrapper ----
    bool is_mission   = false;
    std::uint32_t chunk_size = 0;
    if (!read_pers_header(src, start, is_mission, chunk_size))
    {
      return fail();
    }

    dil_file f;
    f.is_mission_lighting = is_mission;

    // ---- 32-byte payload header ----
    if (!read_s32(src, f.geometry_build_id))   return fail();
    if (!read_s32(src, f.light_scale_shift))   return fail();
    if (!read_s32(src, f.lightmap_count))      return fail(); // no array follows

    std::int32_t state_count     = 0;
    std::int32_t sd_count        = 0;
    std::int32_t light_count     = 0;
    std::int32_t surface_count   = 0;
    std::int32_t map_data_size   = 0;

    if (!read_s32(src, state_count))   return fail();
    if (!read_s32(src, sd_count))      return fail();
    if (!read_s32(src, light_count))   return fail();
    if (!read_s32(src, surface_count)) return fail();
    if (!read_s32(src, map_data_size)) return fail();

    // Sanity bounds.
    if (state_count   < 0 || state_count   > max_state_count)   return fail();
    if (sd_count      < 0 || sd_count      > max_sd_count)      return fail();
    if (light_count   < 0 || light_count   > max_light_count)   return fail();
    if (surface_count < 0 || surface_count > max_surface_count) return fail();
    if (map_data_size < 0 || map_data_size > max_map_data_size) return fail();

    // ---- State array ----
    f.states.resize(static_cast<std::size_t>(state_count));
    for (auto& st : f.states)
    {
      if (!read_state(src, st)) return fail();
    }

    // ---- StateData array ----
    f.state_data.resize(static_cast<std::size_t>(sd_count));
    for (auto& sd : f.state_data)
    {
      if (!read_state_data(src, sd)) return fail();
    }

    // ---- Light array ----
    f.lights.resize(static_cast<std::size_t>(light_count));
    for (auto& lt : f.lights)
    {
      if (!read_light(src, lt)) return fail();
    }

    // ---- Surface array ----
    f.surfaces.resize(static_cast<std::size_t>(surface_count));
    for (auto& sv : f.surfaces)
    {
      if (!read_surface(src, sv)) return fail();
    }

    // ---- MapData (raw Huffman bit-stream) ----
    if (map_data_size > 0)
    {
      f.map_data.resize(static_cast<std::size_t>(map_data_size));
      if (!src.read(reinterpret_cast<char*>(f.map_data.data()),
                    static_cast<std::streamsize>(map_data_size)))
      {
        return fail();
      }
    }

    // ---- Name buffer ----
    std::uint32_t name_buf_size = 0;
    if (!read_u32(src, name_buf_size)) return fail();
    if (static_cast<std::int32_t>(name_buf_size) > max_name_buf_size) return fail();

    if (name_buf_size > 0)
    {
      f.name_buffer.resize(name_buf_size);
      if (!src.read(&f.name_buffer[0],
                    static_cast<std::streamsize>(name_buf_size)))
      {
        return fail();
      }
    }

    // ---- Huffman-present flag ----
    std::uint8_t huffman_present = 0;
    if (!read_u8(src, huffman_present)) return fail();

    if (huffman_present != 0)
    {
      std::int32_t nodes_count  = 0;
      std::int32_t leaves_count = 0;
      if (!read_s32(src, nodes_count))  return fail();
      if (!read_s32(src, leaves_count)) return fail();

      if (nodes_count < 0 || nodes_count > max_huffman_nodes) return fail();
      if (leaves_count < 0 || leaves_count > max_huffman_nodes + 1) return fail();

      // Binary-tree invariant: L == N + 1.
      if (leaves_count != nodes_count + 1) return fail();

      f.huffman_nodes.resize(static_cast<std::size_t>(nodes_count));
      for (auto& nd : f.huffman_nodes)
      {
        if (!read_huffman_node(src, nd)) return fail();
      }

      f.huffman_leaves.resize(static_cast<std::size_t>(leaves_count));
      for (auto& lf : f.huffman_leaves)
      {
        if (!read_huffman_leaf(src, lf)) return fail();
      }
    }

    // ---- ITRMissionLighting tail: LZH-compressed IndexEntry remap ----
    if (is_mission)
    {
      std::int32_t index_array_size = 0;
      if (!read_s32(src, index_array_size)) return fail();
      if (index_array_size < 0 || index_array_size > max_index_entries) return fail();

      if (index_array_size > 0)
      {
        // The LZH stream immediately follows; uncompressed output is
        // index_array_size * 8 bytes (two s32 per IndexEntry).
        const std::size_t expected_bytes =
          static_cast<std::size_t>(index_array_size) * 8;

        std::vector<std::byte> decoded;
        try
        {
          decoded = studio::content::compression::lzh_decompress(
            src, expected_bytes);
        }
        catch (const std::exception&)
        {
          return fail();
        }

        // The LZH bit-reader may have over-read past EOF while topping
        // up its internal bit-buffer. The LZH tail is the last thing in
        // a mission DIL, so the correct stream position is the declared
        // chunk end: start + 8 (magic + chunk_size field) + chunk_size.
        // Clear any stream error and seek there explicitly.
        src.clear();
        src.seekg(start + static_cast<std::streamoff>(8 + chunk_size),
                  std::ios::beg);

        if (decoded.size() != expected_bytes) return fail();

        f.index_remap.resize(static_cast<std::size_t>(index_array_size));
        for (std::size_t i = 0; i < static_cast<std::size_t>(index_array_size); ++i)
        {
          std::uint32_t src_raw  = 0;
          std::uint32_t dest_raw = 0;
          const std::size_t base = i * 8;
          // Little-endian decode from decoded byte buffer.
          for (int b = 0; b < 4; ++b)
          {
            src_raw  |= static_cast<std::uint32_t>(
                          static_cast<std::uint8_t>(decoded[base + b])) << (b * 8);
            dest_raw |= static_cast<std::uint32_t>(
                          static_cast<std::uint8_t>(decoded[base + 4 + b])) << (b * 8);
          }
          f.index_remap[i].src_index  = static_cast<std::int32_t>(src_raw);
          f.index_remap[i].dest_index = static_cast<std::int32_t>(dest_raw);
        }
      }
    }

    return f;
  }

} // namespace studio::content::interior
