#include "content/terrain/world_dats.hpp"

#include <cstring>

namespace studio::content::terrain
{
  namespace
  {
    // ----- low-level little-endian readers ---------------------------

    bool read_bytes(std::istream& s, void* dst, std::size_t n)
    {
      s.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
      return static_cast<bool>(s);
    }

    bool read_u8(std::istream& s, std::uint8_t& out)
    {
      char c;
      if (!s.get(c)) return false;
      out = static_cast<std::uint8_t>(c);
      return true;
    }

    bool read_u32(std::istream& s, std::uint32_t& out)
    {
      unsigned char b[4];
      if (!read_bytes(s, b, 4)) return false;
      out = static_cast<std::uint32_t>(b[0])
          | (static_cast<std::uint32_t>(b[1]) << 8)
          | (static_cast<std::uint32_t>(b[2]) << 16)
          | (static_cast<std::uint32_t>(b[3]) << 24);
      return true;
    }

    bool read_i32(std::istream& s, std::int32_t& out)
    {
      std::uint32_t u;
      if (!read_u32(s, u)) return false;
      std::memcpy(&out, &u, 4);
      return true;
    }

    bool read_f32(std::istream& s, float& out)
    {
      std::uint32_t u;
      if (!read_u32(s, u)) return false;
      std::memcpy(&out, &u, 4);
      return true;
    }

    // Strip trailing NULs from a fixed-width ASCII field.
    std::string trim_to_string(const char* data, std::size_t cap)
    {
      std::size_t len = 0;
      while (len < cap && data[len] != '\0') ++len;
      return std::string(data, len);
    }

    // Number of bytes between the current stream position and EOF.
    // Returns -1 on stream failure.
    std::int64_t bytes_remaining(std::istream& s)
    {
      auto cur = s.tellg();
      if (cur == std::streampos(-1)) return -1;
      s.seekg(0, std::ios::end);
      auto end = s.tellg();
      s.seekg(cur, std::ios::beg);
      if (end == std::streampos(-1)) return -1;
      return static_cast<std::int64_t>(end - cur);
    }
  }

  std::optional<terrain_dat> parse_terrain_dat(std::istream& src)
  {
    auto start = src.tellg();
    auto fail = [&]() -> std::optional<terrain_dat> {
      src.clear();
      if (start != std::streampos(-1)) src.seekg(start, std::ios::beg);
      return std::nullopt;
    };

    std::uint32_t num_types = 0;
    std::uint32_t num_textures = 0;
    if (!read_u32(src, num_types)) return fail();
    if (!read_u32(src, num_textures)) return fail();

    // Sanity caps from the spec.
    if (num_types > 64) return fail();
    if (num_textures > 4096) return fail();

    // Total payload remaining must be exactly
    //   num_types * 32 + num_textures * 276
    auto remaining = bytes_remaining(src);
    if (remaining < 0) return fail();
    const std::uint64_t expected =
        static_cast<std::uint64_t>(num_types) * 32ull
      + static_cast<std::uint64_t>(num_textures) * 276ull;
    if (static_cast<std::uint64_t>(remaining) != expected) return fail();

    terrain_dat out;
    out.type_descriptions.reserve(num_types);

    char slot[32];
    for (std::uint32_t i = 0; i < num_types; ++i)
    {
      if (!read_bytes(src, slot, 32)) return fail();
      out.type_descriptions.push_back(trim_to_string(slot, 32));
    }

    out.records.reserve(num_textures);
    char filename_field[128];
    for (std::uint32_t i = 0; i < num_textures; ++i)
    {
      terrain_dat_record r;
      if (!read_bytes(src, filename_field, 128)) return fail();
      r.bitmap_name = trim_to_string(filename_field, 128);

      if (!read_bytes(src, r.reserved_block.data(), 128)) return fail();

      if (!read_bytes(src, r.corner_type_tags.data(), 4)) return fail();
      if (!read_u32(src, r.sides)) return fail();
      if (!read_u32(src, r.classifier_word)) return fail();
      if (!read_f32(src, r.elasticity)) return fail();
      if (!read_f32(src, r.friction)) return fail();

      out.records.push_back(std::move(r));
    }

    // After reading, the stream should be at EOF (we already
    // size-checked).
    return out;
  }

  std::optional<grid_dat> parse_grid_dat(std::istream& src)
  {
    auto start = src.tellg();
    auto fail = [&]() -> std::optional<grid_dat> {
      src.clear();
      if (start != std::streampos(-1)) src.seekg(start, std::ios::beg);
      return std::nullopt;
    };

    grid_dat out;
    if (!read_u32(src, out.version)) return fail();
    if (!read_u32(src, out.num_base_types)) return fail();
    if (!read_u32(src, out.num_base_textures)) return fail();
    if (!read_u32(src, out.num_picks_header)) return fail();

    // v1 supports only gridVersion == 3 (the only value observed in
    // shipping content).
    if (out.version != 3) return fail();
    if (out.num_base_types > 64) return fail();
    if (out.num_base_textures > 4096) return fail();

    const std::uint64_t b = static_cast<std::uint64_t>(out.num_base_types) + 1ull;
    const std::uint64_t b4 = b * b * b * b;

    // Sanity: with num_base_types <= 64 we have b4 <= 65^4 ~= 17.8M
    // entries. Each tex_combos / pick_offs entry is a u32, so total
    // table cost is bounded by ~143 MB. That is comfortably larger
    // than any shipping world, so add a tighter cap.
    constexpr std::uint64_t b4_cap = 200000ull;  // covers num_base_types <= ~20
    if (b4 > b4_cap) return fail();

    // Read the duplicated type descriptions.
    out.type_descriptions.reserve(out.num_base_types);
    char slot[32];
    for (std::uint32_t i = 0; i < out.num_base_types; ++i)
    {
      if (!read_bytes(src, slot, 32)) return fail();
      out.type_descriptions.push_back(trim_to_string(slot, 32));
    }

    // Per-texture corner-tag tuples (4 bytes each).
    out.per_texture_block.resize(out.num_base_textures);
    for (auto& t : out.per_texture_block)
    {
      if (!read_bytes(src, t.data(), 4)) return fail();
    }

    // tex_combos: b4 u32s.
    out.tex_combos.resize(static_cast<std::size_t>(b4));
    for (auto& v : out.tex_combos)
    {
      if (!read_u32(src, v)) return fail();
    }

    // pick_offs: b4 + 1 u32s.
    out.pick_offs.resize(static_cast<std::size_t>(b4) + 1);
    for (auto& v : out.pick_offs)
    {
      if (!read_u32(src, v)) return fail();
    }

    // Remaining bytes form the pick list: each byte is a texture index.
    // pick_offs values are byte offsets into this array, so the trailing
    // sentinel equals the byte count (i.e. pick_list.size()).
    auto remaining = bytes_remaining(src);
    if (remaining < 0) return fail();
    const std::uint64_t entry_count = static_cast<std::uint64_t>(remaining);
    out.pick_list.resize(static_cast<std::size_t>(entry_count));
    if (entry_count > 0)
    {
      if (!read_bytes(src, out.pick_list.data(), entry_count)) return fail();
    }

    // Trailing sentinel must match pickList byte length.
    if (!out.pick_offs.empty()
        && out.pick_offs.back() != static_cast<std::uint32_t>(entry_count))
    {
      return fail();
    }

    return out;
  }

  std::optional<rules_dat> parse_rules_dat(std::istream& src)
  {
    auto start = src.tellg();
    auto fail = [&]() -> std::optional<rules_dat> {
      src.clear();
      if (start != std::streampos(-1)) src.seekg(start, std::ios::beg);
      return std::nullopt;
    };

    std::uint32_t num_rules = 0;
    if (!read_u32(src, num_rules)) return fail();
    if (num_rules > 256) return fail();

    auto remaining = bytes_remaining(src);
    if (remaining < 0) return fail();
    const std::uint64_t expected = static_cast<std::uint64_t>(num_rules) * 52ull;
    if (static_cast<std::uint64_t>(remaining) != expected) return fail();

    rules_dat out;
    out.version = 0;
    out.rules.reserve(num_rules);

    for (std::uint32_t i = 0; i < num_rules; ++i)
    {
      rules_dat_record r;
      if (!read_i32(src, r.group_num)) return fail();
      if (!read_f32(src, r.alt_min)) return fail();
      if (!read_f32(src, r.alt_max)) return fail();
      if (!read_f32(src, r.alt_mean)) return fail();
      if (!read_f32(src, r.alt_sdev)) return fail();
      if (!read_f32(src, r.alt_weight)) return fail();
      if (!read_i32(src, r.adj_heights)) return fail();
      if (!read_f32(src, r.slope_min)) return fail();
      if (!read_f32(src, r.slope_max)) return fail();
      if (!read_f32(src, r.slope_mean)) return fail();
      if (!read_f32(src, r.slope_sdev)) return fail();
      if (!read_f32(src, r.slope_weight)) return fail();
      if (!read_i32(src, r.adj_slopes)) return fail();
      out.rules.push_back(r);
    }

    return out;
  }
}
