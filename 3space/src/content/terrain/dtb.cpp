#include "content/terrain/dtb.hpp"
#include "content/compression/lzh.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <stdexcept>

// Implementation note: authored from the clean-room spec at
// `docs/clean-room-specs/TERRAIN.md` §2 and verified against all 45
// per-mission `.dtb` files extracted from the Tribes 1.41 freeware
// corpus. No leaked engine source was consulted.

namespace studio::content::terrain
{
  namespace
  {
    constexpr std::array<char, 4> gblk_magic{'G', 'B', 'L', 'K'};

    // Sanity cap for the chunk payload. A real DTB is ~300 KiB;
    // reject anything over 64 MiB to guard against corrupt headers.
    constexpr std::uint32_t max_payload = 64u * 1024u * 1024u;

    // Maximum tile count per axis.
    constexpr std::int32_t max_axis_tiles = 1024;

    // Number of detail levels whose pinMap entries follow the materialmap
    // (always 11 entries for classVersion 5).
    constexpr int pin_map_entries = 11;

    bool read_u32(std::istream& s, std::uint32_t& out)
    {
      unsigned char bytes[4];
      if (!s.read(reinterpret_cast<char*>(bytes), 4)) return false;
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

    bool read_u16(std::istream& s, std::uint16_t& out)
    {
      unsigned char bytes[2];
      if (!s.read(reinterpret_cast<char*>(bytes), 2)) return false;
      out = static_cast<std::uint16_t>(bytes[0])
          | (static_cast<std::uint16_t>(bytes[1]) << 8);
      return true;
    }

    bool read_f32(std::istream& s, float& out)
    {
      std::uint32_t u = 0;
      if (!read_u32(s, u)) return false;
      static_assert(sizeof(float) == 4, "f32 round-trip requires 4-byte float");
      std::memcpy(&out, &u, 4);
      return true;
    }

    // Each Darkstar LZH blob is followed by a 1-byte trailing flush word.
    // After lzh_decompress the stream sits on that byte; skip it.
    bool skip_lzh_trailer(std::istream& s)
    {
      s.clear();
      return static_cast<bool>(s.seekg(1, std::ios::cur));
    }
  } // anonymous namespace

  bool is_darkstar_dtb(std::istream& stream)
  {
    auto start = stream.tellg();
    char magic[4];
    bool ok = static_cast<bool>(stream.read(magic, 4))
              && std::memcmp(magic, gblk_magic.data(), 4) == 0;
    stream.clear();
    stream.seekg(start, std::ios::beg);
    return ok;
  }

  std::optional<grid_block> parse_dtb(std::istream& src)
  {
    auto start = src.tellg();
    auto fail = [&]() -> std::optional<grid_block> {
      src.clear();
      src.seekg(start, std::ios::beg);
      return std::nullopt;
    };

    // ----------------------------------------------------------------
    // 1. Chunk container: magic + payload size.
    // ----------------------------------------------------------------
    char magic[4];
    if (!src.read(magic, 4)) return fail();
    if (std::memcmp(magic, gblk_magic.data(), 4) != 0) return fail();

    std::uint32_t chunk_payload_size = 0;
    if (!read_u32(src, chunk_payload_size)) return fail();
    if (chunk_payload_size > max_payload) return fail();

    const auto payload_begin = src.tellg();

    // ----------------------------------------------------------------
    // 2. Fixed 44-byte header (spec §2.2).
    // ----------------------------------------------------------------
    grid_block blk;

    if (!read_u32(src, blk.version)) return fail();
    if (blk.version != 5) return fail();

    // nameId: 16-byte NUL-padded field.
    char name_buf[16] = {};
    if (!src.read(name_buf, 16)) return fail();
    blk.name.assign(name_buf, strnlen(name_buf, 16));

    if (!read_i32(src, blk.detail_count)) return fail();
    if (!read_i32(src, blk.light_scale)) return fail();
    if (!read_f32(src, blk.height_min)) return fail();
    if (!read_f32(src, blk.height_max)) return fail();
    if (!read_i32(src, blk.size[0])) return fail();
    if (!read_i32(src, blk.size[1])) return fail();

    // Sanity-bound the grid dimensions.
    if (blk.size[0] <= 0 || blk.size[1] <= 0
        || blk.size[0] > max_axis_tiles || blk.size[1] > max_axis_tiles)
    {
      return fail();
    }

    const std::size_t verts_x = static_cast<std::size_t>(blk.size[0]) + 1;
    const std::size_t verts_y = static_cast<std::size_t>(blk.size[1]) + 1;
    const std::size_t vertex_count = verts_x * verts_y;
    const std::size_t quad_count   = static_cast<std::size_t>(blk.size[0])
                                   * static_cast<std::size_t>(blk.size[1]);

    // ----------------------------------------------------------------
    // 3. Heightmap LZH blob (spec §2.3 sub-1).
    //    u32 uncompressed size precedes the bitstream.
    //    Each Darkstar LZH blob ends with a 1-byte flush trailer; skip it.
    // ----------------------------------------------------------------
    std::uint32_t height_uncompressed_size = 0;
    if (!read_u32(src, height_uncompressed_size)) return fail();
    if (height_uncompressed_size != vertex_count * 4) return fail();

    std::vector<std::byte> height_bytes;
    try
    {
      height_bytes = compression::lzh_decompress(src, height_uncompressed_size);
    }
    catch (const std::exception&)
    {
      return fail();
    }

    if (height_bytes.size() != height_uncompressed_size) return fail();
    if (!skip_lzh_trailer(src)) return fail();

    blk.heights.resize(vertex_count);
    std::memcpy(blk.heights.data(), height_bytes.data(), height_bytes.size());

    // ----------------------------------------------------------------
    // 4. Materialmap LZH blob (spec §2.3 sub-2).
    //    u32 uncompressed size precedes the bitstream.
    //    Materialmap is quad-indexed: size[0]*size[1] records of 2 bytes.
    // ----------------------------------------------------------------
    std::uint32_t mat_uncompressed_size = 0;
    if (!read_u32(src, mat_uncompressed_size)) return fail();
    if (mat_uncompressed_size != quad_count * 2) return fail();

    std::vector<std::byte> mat_bytes;
    try
    {
      mat_bytes = compression::lzh_decompress(src, mat_uncompressed_size);
    }
    catch (const std::exception&)
    {
      return fail();
    }

    if (mat_bytes.size() != mat_uncompressed_size) return fail();
    if (!skip_lzh_trailer(src)) return fail();

    blk.materials.resize(quad_count);
    for (std::size_t i = 0; i < quad_count; ++i)
    {
      blk.materials[i].flags = static_cast<std::uint8_t>(mat_bytes[i * 2]);
      blk.materials[i].index = static_cast<std::uint8_t>(mat_bytes[i * 2 + 1]);
    }

    // ----------------------------------------------------------------
    // 5. PinMap subrecords (spec §2.4).
    //    Eleven entries of (u16 size, [size] bytes).
    // ----------------------------------------------------------------
    src.clear();
    for (int pm = 0; pm < pin_map_entries; ++pm)
    {
      std::uint16_t pm_size = 0;
      if (!read_u16(src, pm_size)) return fail();
      if (pm_size > 0)
      {
        src.clear();
        if (!src.seekg(pm_size, std::ios::cur)) return fail();
      }
    }

    // ----------------------------------------------------------------
    // 6. Lightmap LZH blob (spec §2.3 sub-4).
    //    Only present when light_scale != -1; always present in shipped
    //    files (light_scale == 0).
    // ----------------------------------------------------------------
    src.clear();
    if (blk.light_scale != -1)
    {
      if (blk.light_scale < 0) return fail();
      const std::size_t lm_dim =
        (static_cast<std::size_t>(blk.size[0]) << blk.light_scale) + 1;
      blk.lightmap_dim = static_cast<std::int32_t>(lm_dim);

      std::uint32_t lightmap_uncompressed_size = 0;
      if (!read_u32(src, lightmap_uncompressed_size)) return fail();
      if (lightmap_uncompressed_size != lm_dim * lm_dim * 2) return fail();

      std::vector<std::byte> lightmap_bytes;
      try
      {
        lightmap_bytes = compression::lzh_decompress(src, lightmap_uncompressed_size);
      }
      catch (const std::exception&)
      {
        return fail();
      }

      if (lightmap_bytes.size() != lightmap_uncompressed_size) return fail();
      if (!skip_lzh_trailer(src)) return fail();

      src.clear();
      const std::size_t lm_entry_count = lm_dim * lm_dim;
      blk.lightmap.resize(lm_entry_count);
      for (std::size_t i = 0; i < lm_entry_count; ++i)
      {
        blk.lightmap[i] =
          static_cast<std::uint16_t>(lightmap_bytes[i * 2])
          | (static_cast<std::uint16_t>(lightmap_bytes[i * 2 + 1]) << 8);
      }
    }
    else
    {
      blk.lightmap_dim = 0;
    }

    // ----------------------------------------------------------------
    // 7. hiresLightMapSize + HRLM trailer (spec §2.5).
    //    i32 hiresLightMapSize (= 0 in every shipped file).
    //    Then 20-byte HRLM section.
    // ----------------------------------------------------------------
    src.clear();
    std::int32_t hires_lm_size = 0;
    if (!read_i32(src, hires_lm_size)) return fail();
    if (hires_lm_size < 0) return fail();
    if (hires_lm_size > 0)
    {
      src.clear();
      if (!src.seekg(static_cast<std::streamoff>(hires_lm_size), std::ios::cur))
        return fail();
    }

    // Store the 20-byte HRLM trailer as opaque bytes.
    std::array<std::byte, 20> hrlm_buf{};
    if (!src.read(reinterpret_cast<char*>(hrlm_buf.data()), 20)) return fail();

    // Decode the five i32s for validation.
    auto read_i32_from_bytes = [](const std::byte* p) -> std::int32_t {
      std::uint32_t u = static_cast<std::uint32_t>(p[0])
                      | (static_cast<std::uint32_t>(p[1]) << 8)
                      | (static_cast<std::uint32_t>(p[2]) << 16)
                      | (static_cast<std::uint32_t>(p[3]) << 24);
      return static_cast<std::int32_t>(u);
    };

    const std::int32_t hrlm_version    = read_i32_from_bytes(hrlm_buf.data() +  0);
    const std::int32_t num_hrlms       = read_i32_from_bytes(hrlm_buf.data() +  4);
    const std::int32_t color_pool_size = read_i32_from_bytes(hrlm_buf.data() +  8);
    const std::int32_t index_tbl_size  = read_i32_from_bytes(hrlm_buf.data() + 12);
    const std::int32_t tree_tbl_size   = read_i32_from_bytes(hrlm_buf.data() + 16);

    if (hrlm_version != 3) return fail();
    if (num_hrlms != 0 || color_pool_size != 0
        || index_tbl_size != 0 || tree_tbl_size != 0)
    {
      return fail();
    }

    blk.hrlm_raw.assign(hrlm_buf.begin(), hrlm_buf.end());

    // ----------------------------------------------------------------
    // 8. Verify we consumed exactly chunk_payload_size bytes.
    // ----------------------------------------------------------------
    auto end = src.tellg();
    if (end == std::streampos(-1)) return fail();
    auto consumed = static_cast<std::uint64_t>(end - payload_begin);
    if (consumed != static_cast<std::uint64_t>(chunk_payload_size)) return fail();

    return blk;
  }
} // namespace studio::content::terrain
