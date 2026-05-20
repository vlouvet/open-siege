#include "content/terrain/dtf.hpp"

#include <array>
#include <cstring>

// Implementation note: this parser was authored from the clean-room
// spec at `docs/clean-room-specs/TERRAIN.md` plus real sample bytes
// extracted from the 45 mission `.ted` PVOL archives shipped with the
// Tribes 1.41 freeware build. No leaked engine source was consulted.

namespace studio::content::terrain
{
  namespace
  {
    constexpr std::array<char, 4> gfil_magic{'G', 'F', 'I', 'L'};

    // Sanity caps. The shipping corpus uses 3x3 everywhere; allow up
    // to 16x16 to accommodate hypothetical mods while still bounding
    // memory.
    constexpr std::int32_t max_axis = 16;

    // Read a little-endian u32 from the stream. Returns false on
    // stream failure.
    bool read_u32(std::istream& s, std::uint32_t& out)
    {
      unsigned char bytes[4];
      if (!s.read(reinterpret_cast<char*>(bytes), 4))
      {
        return false;
      }
      out = static_cast<std::uint32_t>(bytes[0])
          | (static_cast<std::uint32_t>(bytes[1]) << 8)
          | (static_cast<std::uint32_t>(bytes[2]) << 16)
          | (static_cast<std::uint32_t>(bytes[3]) << 24);
      return true;
    }

    bool read_i32(std::istream& s, std::int32_t& out)
    {
      std::uint32_t u = 0;
      if (!read_u32(s, u)) return false;
      out = static_cast<std::int32_t>(u);
      return true;
    }

    // Read a little-endian f32 from the stream.
    bool read_f32(std::istream& s, float& out)
    {
      std::uint32_t u = 0;
      if (!read_u32(s, u)) return false;
      static_assert(sizeof(float) == 4, "f32 round-trip assumes 4-byte float");
      std::memcpy(&out, &u, 4);
      return true;
    }

    // Read a length-prefixed ASCII string: u32 length followed by
    // that many bytes (not NUL-terminated on disk). `max_remaining`
    // caps how many bytes the string is allowed to consume, used to
    // reject lengths that would overflow the enclosing chunk.
    bool read_length_prefixed_string(std::istream& s,
                                     std::uint64_t max_remaining,
                                     std::string& out)
    {
      std::uint32_t len = 0;
      if (!read_u32(s, len)) return false;
      // 4 bytes already consumed for the length itself.
      if (max_remaining < 4 || len > max_remaining - 4) return false;
      out.resize(len);
      if (len > 0)
      {
        if (!s.read(out.data(), static_cast<std::streamsize>(len)))
        {
          return false;
        }
      }
      return true;
    }
  }

  bool is_darkstar_dtf(std::istream& stream)
  {
    auto start = stream.tellg();
    char magic[4];
    bool ok = static_cast<bool>(stream.read(magic, 4))
              && std::memcmp(magic, gfil_magic.data(), 4) == 0;
    stream.clear();
    stream.seekg(start, std::ios::beg);
    return ok;
  }

  std::optional<dtf_file> parse_dtf(std::istream& src)
  {
    auto start = src.tellg();
    auto fail = [&]() -> std::optional<dtf_file> {
      src.clear();
      src.seekg(start, std::ios::beg);
      return std::nullopt;
    };

    // Magic.
    char magic[4];
    if (!src.read(magic, 4)) return fail();
    if (std::memcmp(magic, gfil_magic.data(), 4) != 0) return fail();

    // chunkPayloadSize: u32, equals file_size - 8 in every shipping
    // DTF. Use it to bound the payload reads.
    std::uint32_t chunk_payload_size = 0;
    if (!read_u32(src, chunk_payload_size)) return fail();

    // Defensive cap: a shipping DTF is <= 256 bytes; refuse anything
    // larger than 64 KiB to avoid runaway allocations on a corrupt
    // header.
    constexpr std::uint32_t max_reasonable_payload = 64u * 1024u;
    if (chunk_payload_size > max_reasonable_payload) return fail();

    // Track bytes consumed inside the payload so we can both bound
    // length-prefixed strings and verify we end exactly at the end
    // of the chunk.
    const auto payload_begin = src.tellg();
    auto remaining_in_payload = [&]() -> std::uint64_t {
      auto cur = src.tellg();
      if (cur == std::streampos(-1)) return 0;
      auto consumed = static_cast<std::uint64_t>(cur - payload_begin);
      if (consumed > chunk_payload_size) return 0;
      return static_cast<std::uint64_t>(chunk_payload_size) - consumed;
    };

    dtf_file d;

    if (!read_u32(src, d.version)) return fail();
    if (d.version != 1) return fail();

    // matListName: u32 length + that many ASCII bytes.
    if (!read_length_prefixed_string(src, remaining_in_payload(),
                                     d.material_list_name))
    {
      return fail();
    }
    // Sanity: matListName is at most 16 chars in the shipping corpus;
    // accept up to 256 for mod safety.
    if (d.material_list_name.size() > 256) return fail();

    if (!read_i32(src, d.last_block_id)) return fail();
    if (!read_i32(src, d.detail_count)) return fail();
    if (!read_i32(src, d.scale)) return fail();

    // 24-byte opaque bounds region.
    if (!src.read(reinterpret_cast<char*>(d.bounds_raw.data()), 24))
    {
      return fail();
    }

    // Origin: 2 x i32.
    if (!read_i32(src, d.origin[0])) return fail();
    if (!read_i32(src, d.origin[1])) return fail();

    // heightRange: 2 x f32.
    if (!read_f32(src, d.height_min)) return fail();
    if (!read_f32(src, d.height_max)) return fail();

    // size: 2 x i32. Sanity-bound to avoid pathological allocations.
    if (!read_i32(src, d.size[0])) return fail();
    if (!read_i32(src, d.size[1])) return fail();
    if (d.size[0] <= 0 || d.size[1] <= 0
        || d.size[0] > max_axis || d.size[1] > max_axis)
    {
      return fail();
    }

    // blockPattern: u32. Only value 0 is fully supported; non-zero
    // values are passed through but the renderer treats them as
    // single-block.
    if (!read_u32(src, d.block_pattern)) return fail();

    // blockMap: size[0] * size[1] entries of i32 block id.
    std::uint64_t cell_count =
      static_cast<std::uint64_t>(d.size[0]) * static_cast<std::uint64_t>(d.size[1]);
    if (cell_count * 4u > remaining_in_payload()) return fail();
    d.block_map.resize(static_cast<std::size_t>(cell_count));
    for (auto& cell : d.block_map)
    {
      if (!read_i32(src, cell.block_id)) return fail();
    }

    // Trailing GridBlockList: i32 count, then count entries of
    // (i32 block_id, length-prefixed string).
    std::int32_t block_list_count = 0;
    if (!read_i32(src, block_list_count)) return fail();
    if (block_list_count < 0 || block_list_count > 256) return fail();

    d.block_list.resize(static_cast<std::size_t>(block_list_count));
    for (auto& entry : d.block_list)
    {
      if (!read_i32(src, entry.block_id)) return fail();
      if (!read_length_prefixed_string(src, remaining_in_payload(),
                                       entry.name))
      {
        return fail();
      }
    }

    // Verify we landed exactly at the declared chunk end.
    auto end = src.tellg();
    if (end == std::streampos(-1)) return fail();
    auto consumed = static_cast<std::uint64_t>(end - payload_begin);
    if (consumed != chunk_payload_size) return fail();

    return d;
  }
}
