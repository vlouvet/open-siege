#include "content/dis/dis.hpp"

#include <array>
#include <cstring>

namespace studio::content::dis
{
  namespace
  {
    constexpr std::array<char, 4> itrs_magic{'I', 'T', 'R', 's'};

    // Read a little-endian u32 from the stream. Returns false (and
    // leaves the result unchanged) on stream failure.
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

    bool read_u8(std::istream& s, std::uint8_t& out)
    {
      char c;
      if (!s.get(c))
      {
        return false;
      }
      out = static_cast<std::uint8_t>(c);
      return true;
    }

    // Look up a NUL-terminated ASCII string at `offset` inside a flat
    // name-table buffer. Returns false if the offset is out of range
    // or no NUL terminator is found before the end of the buffer.
    bool lookup_name(const std::vector<char>& table,
                     std::uint32_t offset,
                     std::string& out)
    {
      if (offset >= table.size())
      {
        return false;
      }
      // Find NUL terminator inside the table.
      std::size_t end = offset;
      while (end < table.size() && table[end] != '\0')
      {
        ++end;
      }
      if (end == table.size())
      {
        // Missing terminator.
        return false;
      }
      out.assign(table.data() + offset, end - offset);
      return true;
    }
  }

  bool is_darkstar_dis(std::istream& stream)
  {
    auto start = stream.tellg();
    char magic[4];
    bool ok = static_cast<bool>(stream.read(magic, 4))
              && std::memcmp(magic, itrs_magic.data(), 4) == 0;
    stream.clear();
    stream.seekg(start, std::ios::beg);
    return ok;
  }

  std::optional<dis_manifest> parse_dis(std::istream& src)
  {
    auto start = src.tellg();

    auto fail = [&]() -> std::optional<dis_manifest> {
      src.clear();
      src.seekg(start, std::ios::beg);
      return std::nullopt;
    };

    // Magic.
    char magic[4];
    if (!src.read(magic, 4)) return fail();
    if (std::memcmp(magic, itrs_magic.data(), 4) != 0) return fail();

    // chunkSize (advisory — equals file_size - 8 in every observed file).
    std::uint32_t chunk_size = 0;
    if (!read_u32(src, chunk_size)) return fail();

    dis_manifest m;

    if (!read_u32(src, m.version)) return fail();
    if (m.version != 3) return fail();

    if (!read_u32(src, m.num_states)) return fail();

    // 8 reserved zero bytes (likely placeholders for state-name and
    // LOD-index of state 0 — never populated in shipping content).
    std::uint32_t rsv0 = 0, rsv1 = 0;
    if (!read_u32(src, rsv0)) return fail();
    if (!read_u32(src, rsv1)) return fail();

    std::uint32_t num_lods = 0, num_lods_dup = 0;
    if (!read_u32(src, num_lods)) return fail();
    if (!read_u32(src, num_lods_dup)) return fail();
    if (num_lods != num_lods_dup) return fail();
    if (num_lods == 0) return fail();

    // Defensive cap — any real DIS has <= 3 LODs; refuse absurd
    // counts to avoid runaway allocations on a malformed file.
    constexpr std::uint32_t max_reasonable_lods = 64;
    if (num_lods > max_reasonable_lods) return fail();

    // LOD records (16 bytes each).
    struct lod_raw
    {
      std::uint32_t min_pixels;
      std::uint32_t geom_name_offset;
      std::uint32_t light_state_idx;
      std::uint32_t linkable_faces;
    };
    std::vector<lod_raw> raw_lods(num_lods);
    for (auto& r : raw_lods)
    {
      if (!read_u32(src, r.min_pixels)) return fail();
      if (!read_u32(src, r.geom_name_offset)) return fail();
      if (!read_u32(src, r.light_state_idx)) return fail();
      if (!read_u32(src, r.linkable_faces)) return fail();
    }

    std::uint32_t num_light_states = 0;
    if (!read_u32(src, num_light_states)) return fail();
    if (num_light_states == 0 || num_light_states > max_reasonable_lods)
    {
      return fail();
    }

    std::vector<std::uint32_t> dil_offsets(num_light_states);
    for (auto& o : dil_offsets)
    {
      if (!read_u32(src, o)) return fail();
    }

    // The "constant 1" word. Carried in shipping content as exactly 1;
    // some sources call it `writeNameTable` (boolean) or
    // `numNameBuffers` (always one buffer). We accept any value but
    // refuse 0 because then there would be no name table to resolve
    // offsets against.
    std::uint32_t name_buffer_count = 0;
    if (!read_u32(src, name_buffer_count)) return fail();
    if (name_buffer_count == 0) return fail();

    std::uint32_t default_state_name_offset = 0;
    if (!read_u32(src, default_state_name_offset)) return fail();

    std::uint32_t name_table_size = 0;
    if (!read_u32(src, name_table_size)) return fail();

    // Sanity-check the name table size against the remaining file
    // length: name_table_size + 4 (materialListNameOffset) + 1
    // (linkedInterior) bytes must remain after this point.
    auto cur = src.tellg();
    src.seekg(0, std::ios::end);
    auto file_end = src.tellg();
    src.seekg(cur, std::ios::beg);
    if (file_end == std::streampos(-1)) return fail();
    auto remaining = static_cast<std::uint64_t>(file_end - cur);
    if (remaining < static_cast<std::uint64_t>(name_table_size) + 5)
    {
      return fail();
    }

    std::vector<char> name_table(name_table_size);
    if (name_table_size > 0)
    {
      if (!src.read(name_table.data(),
                    static_cast<std::streamsize>(name_table_size)))
      {
        return fail();
      }
    }

    std::uint32_t material_list_offset_raw = 0;
    if (!read_u32(src, material_list_offset_raw)) return fail();

    if (!read_u8(src, m.linked_interior)) return fail();

    // Resolve all name-table offsets. Any failure here means the
    // file claims a string at an offset that doesn't terminate
    // inside the table — treat as malformed.
    if (!lookup_name(name_table, default_state_name_offset,
                     m.default_state_name))
    {
      return fail();
    }

    m.lods.reserve(raw_lods.size());
    for (auto const& r : raw_lods)
    {
      lod_record lod;
      lod.min_pixels = r.min_pixels;
      lod.light_state_index = r.light_state_idx;
      lod.linkable_faces_flags = r.linkable_faces;
      if (!lookup_name(name_table, r.geom_name_offset, lod.geometry_file))
      {
        return fail();
      }
      m.lods.push_back(std::move(lod));
    }

    m.lightmap_files.reserve(dil_offsets.size());
    for (auto const& off : dil_offsets)
    {
      std::string name;
      if (!lookup_name(name_table, off, name))
      {
        return fail();
      }
      m.lightmap_files.push_back(std::move(name));
    }

    if (!lookup_name(name_table, material_list_offset_raw,
                     m.material_list_file))
    {
      return fail();
    }

    // chunk_size is documented to equal file_size - 8. Use it only
    // as a soft consistency check (some tooling may regenerate
    // chunkSize loosely). Skip enforcement here.
    (void)chunk_size;
    (void)rsv0;
    (void)rsv1;

    return m;
  }
}
