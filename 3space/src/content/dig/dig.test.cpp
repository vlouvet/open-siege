// Unit tests for the DIG (ITRGeometry v7) parser.
//
// Two layers of coverage:
//   1. In-memory smoke tests with synthesised buffers — no asset
//      corpus needed.
//   2. Real-file checks against samples extracted from per-world
//      `*DML.vol` archives via:
//          vol-list --extract ...DML.vol .dig /tmp/dig-samples
//      Skipped (with WARN) when /tmp/dig-samples is empty.

#include <catch2/catch.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "content/dig/dig.hpp"

namespace
{
  void put_u8(std::vector<unsigned char>& buf, std::uint8_t v)
  {
    buf.push_back(v);
  }

  void put_u16(std::vector<unsigned char>& buf, std::uint16_t v)
  {
    buf.push_back(static_cast<unsigned char>(v & 0xff));
    buf.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
  }

  void put_u32(std::vector<unsigned char>& buf, std::uint32_t v)
  {
    buf.push_back(static_cast<unsigned char>(v & 0xff));
    buf.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    buf.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
    buf.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
  }

  void put_s32(std::vector<unsigned char>& buf, std::int32_t v)
  {
    put_u32(buf, static_cast<std::uint32_t>(v));
  }

  void put_f32(std::vector<unsigned char>& buf, float v)
  {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    put_u32(buf, bits);
  }

  void put_bytes(std::vector<unsigned char>& buf, const void* p, std::size_t n)
  {
    auto const* b = static_cast<const unsigned char*>(p);
    buf.insert(buf.end(), b, b + n);
  }

  std::stringstream make_stream(const std::vector<unsigned char>& buf)
  {
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    ss.write(reinterpret_cast<const char*>(buf.data()),
             static_cast<std::streamsize>(buf.size()));
    ss.seekg(0);
    return ss;
  }

  // Build a minimal but structurally complete DIG buffer:
  //   1 surface, 1 BSP node, 1 solid leaf, 1 empty leaf,
  //   0 bitlist bytes, 1 vertex, 1 point3, 1 point2, 1 plane.
  std::vector<unsigned char> build_minimal_dig()
  {
    std::vector<unsigned char> buf;
    // PERS magic.
    const char magic[4] = {'P','E','R','S'};
    put_bytes(buf, magic, 4);
    // chunkSize placeholder — patch at the end.
    const std::size_t chunk_size_pos = buf.size();
    put_u32(buf, 0);
    // classname_len + "ITRGeometry" + NUL pad.
    put_u16(buf, 11);
    const char cn[] = "ITRGeometry"; // 12 bytes including the literal's terminator
    put_bytes(buf, cn, 12);
    // version
    put_u32(buf, 7);

    // ---- payload ----
    put_s32(buf, 0x140b9c4f);       // build_id
    put_f32(buf, 32.0f);            // texture_scale
    // bbox_min
    put_f32(buf, -1.0f); put_f32(buf, -2.0f); put_f32(buf, -3.0f);
    // bbox_max
    put_f32(buf, 1.0f); put_f32(buf, 2.0f); put_f32(buf, 3.0f);
    // 9 array sizes.
    put_s32(buf, 1);   // surfaces
    put_s32(buf, 1);   // nodes
    put_s32(buf, 1);   // solid leaves
    put_s32(buf, 1);   // empty leaves
    put_s32(buf, 0);   // bitlist
    put_s32(buf, 1);   // vertices
    put_s32(buf, 1);   // points3
    put_s32(buf, 1);   // points2
    put_s32(buf, 1);   // planes

    // Surface (20 bytes). Pack: type=1, scale_shift=12, ambient=0,
    // visible=1, plane_front=0 → 1 11000 0 1 0 = 0xE2.
    put_u8(buf, 0xE2);             // flags
    put_u8(buf, 7);                // material
    put_u8(buf, 16); put_u8(buf, 32); // texture_size
    put_u8(buf, 1);  put_u8(buf, 2);  // texture_offset
    put_u16(buf, 5);                // plane_index
    put_u32(buf, 11);               // vertex_index
    put_u32(buf, 22);               // point_index
    put_u8(buf, 4);                 // vertex_count
    put_u8(buf, 1);                 // point_count
    put_u16(buf, 0);                // dummy

    // BSP node (8 bytes).
    put_u16(buf, 3);               // plane_index
    put_u16(buf, 0xFFFE);          // front  (-2)
    put_u16(buf, 0x0001);          // back
    put_u16(buf, 0);               // fill

    // Solid leaf (12 bytes).
    put_u16(buf, 0); put_u16(buf, 9); put_u16(buf, 0);
    put_u16(buf, 4); put_u16(buf, 3); put_u16(buf, 2);

    // Empty leaf (44 bytes). flags=1 + pvs_count=0x1234 → packed
    // u16 with bit 15 set and the lower 15 bits = 0x1234.
    put_u16(buf, static_cast<std::uint16_t>(0x8000 | 0x1234));
    put_u16(buf, 6);               // surface_count
    put_u32(buf, 7);               // pvs_index
    put_u32(buf, 8);               // surface_index
    put_u32(buf, 9);               // plane_index
    // box (6 floats)
    put_f32(buf, -1.0f); put_f32(buf, -1.0f); put_f32(buf, -1.0f);
    put_f32(buf,  1.0f); put_f32(buf,  1.0f); put_f32(buf,  1.0f);
    put_u16(buf, 10);              // plane_count
    put_u16(buf, 0);               // dummy

    // 1 packed vertex (4 bytes).
    put_u16(buf, 13); put_u16(buf, 14);

    // 1 point3 (12 bytes).
    put_f32(buf, 1.5f); put_f32(buf, 2.5f); put_f32(buf, 3.5f);

    // 1 point2 (8 bytes).
    put_f32(buf, 0.25f); put_f32(buf, 0.75f);

    // 1 plane (16 bytes).
    put_f32(buf, 0.0f); put_f32(buf, 0.0f); put_f32(buf, 1.0f);
    put_f32(buf, 4.0f);

    // Trailer.
    put_s32(buf, 2);               // highest_mip_level
    put_u32(buf, 0xDEADBEEF);      // flags

    // Patch chunkSize = file_size - 8.
    std::uint32_t cs = static_cast<std::uint32_t>(buf.size() - 8);
    buf[chunk_size_pos + 0] = static_cast<unsigned char>(cs & 0xff);
    buf[chunk_size_pos + 1] = static_cast<unsigned char>((cs >> 8) & 0xff);
    buf[chunk_size_pos + 2] = static_cast<unsigned char>((cs >> 16) & 0xff);
    buf[chunk_size_pos + 3] = static_cast<unsigned char>((cs >> 24) & 0xff);
    return buf;
  }
}

using studio::content::dig::is_dig_file;
using studio::content::dig::read_dig_file;

TEST_CASE("is_dig_file detects PERS+ITRGeometry magic", "[dig]")
{
  auto buf = build_minimal_dig();
  auto ss = make_stream(buf);
  REQUIRE(is_dig_file(ss) == true);
  REQUIRE(ss.tellg() == std::streampos(0));
}

TEST_CASE("is_dig_file rejects wrong PERS class", "[dig]")
{
  std::vector<unsigned char> buf;
  const char magic[4] = {'P','E','R','S'};
  // header looking like a PERS chunk but for a different class
  buf.insert(buf.end(), magic, magic + 4);
  for (int i = 0; i < 4; ++i) buf.push_back(0);
  // classname_len=9, name "TS::Shape"
  buf.push_back(9); buf.push_back(0);
  const char other[] = "TS::Shape";
  buf.insert(buf.end(), other, other + 9);
  buf.push_back(0); // pad
  for (int i = 0; i < 4; ++i) buf.push_back(0);
  auto ss = make_stream(buf);
  REQUIRE(is_dig_file(ss) == false);
  REQUIRE(ss.tellg() == std::streampos(0));
}

TEST_CASE("is_dig_file rejects non-PERS magic", "[dig]")
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  ss.write("ITRs\x00\x00\x00\x00", 8);
  ss.seekg(0);
  REQUIRE(is_dig_file(ss) == false);
}

TEST_CASE("read_dig_file returns nullopt on wrong magic", "[dig]")
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  ss.write("ITRsXXXXX", 9);
  ss.seekg(0);
  REQUIRE_FALSE(read_dig_file(ss).has_value());
}

TEST_CASE("read_dig_file parses a hand-crafted DIG", "[dig]")
{
  auto buf = build_minimal_dig();
  auto ss = make_stream(buf);

  auto dig = read_dig_file(ss);
  REQUIRE(dig.has_value());
  REQUIRE(dig->build_id == 0x140b9c4f);
  REQUIRE(dig->texture_scale == Approx(32.0f));
  REQUIRE(dig->bbox_min.x == Approx(-1.0f));
  REQUIRE(dig->bbox_max.z == Approx(3.0f));

  REQUIRE(dig->surfaces.size() == 1);
  auto const& s = dig->surfaces[0];
  REQUIRE(s.type == true);
  REQUIRE(s.texture_scale_shift == 12);
  REQUIRE(s.apply_ambient == false);
  REQUIRE(s.visible_to_outside == true);
  REQUIRE(s.plane_front == false);
  REQUIRE(s.material == 7);
  REQUIRE(s.texture_size_x == 16);
  REQUIRE(s.texture_size_y == 32);
  REQUIRE(s.texture_offset_x == 1);
  REQUIRE(s.texture_offset_y == 2);
  REQUIRE(s.plane_index == 5);
  REQUIRE(s.vertex_index == 11);
  REQUIRE(s.point_index == 22);
  REQUIRE(s.vertex_count == 4);
  REQUIRE(s.point_count == 1);

  REQUIRE(dig->bsp_nodes.size() == 1);
  REQUIRE(dig->bsp_nodes[0].plane_index == 3);
  REQUIRE(dig->bsp_nodes[0].front == -2);
  REQUIRE(dig->bsp_nodes[0].back == 1);

  REQUIRE(dig->bsp_solid_leaves.size() == 1);
  REQUIRE(dig->bsp_solid_leaves[0].surface_index == 9);
  REQUIRE(dig->bsp_solid_leaves[0].plane_count == 2);

  REQUIRE(dig->bsp_empty_leaves.size() == 1);
  auto const& el = dig->bsp_empty_leaves[0];
  REQUIRE(el.flags == true);
  REQUIRE(el.pvs_count == 0x1234);
  REQUIRE(el.surface_count == 6);
  REQUIRE(el.pvs_index == 7);
  REQUIRE(el.surface_index == 8);
  REQUIRE(el.plane_index == 9);
  REQUIRE(el.box_min_x == Approx(-1.0f));
  REQUIRE(el.box_max_z == Approx(1.0f));
  REQUIRE(el.plane_count == 10);

  REQUIRE(dig->vertices.size() == 1);
  REQUIRE(dig->vertices[0].point_index == 13);
  REQUIRE(dig->vertices[0].texture_index == 14);

  REQUIRE(dig->points3.size() == 1);
  REQUIRE(dig->points3[0].y == Approx(2.5f));

  REQUIRE(dig->points2.size() == 1);
  REQUIRE(dig->points2[0].x == Approx(0.25f));

  REQUIRE(dig->planes.size() == 1);
  REQUIRE(dig->planes[0].point.z == Approx(1.0f));
  REQUIRE(dig->planes[0].d == Approx(4.0f));

  REQUIRE(dig->highest_mip_level == 2);
  REQUIRE(dig->flags == 0xDEADBEEF);

  // Stream should now be at the end of the declared chunk (= EOF
  // for this minimal buffer).
  REQUIRE(ss.tellg() == std::streampos(static_cast<std::streamoff>(buf.size())));
}

TEST_CASE("read_dig_file rejects an oversized array_size", "[dig]")
{
  auto buf = build_minimal_dig();
  // The 9 size fields start at file-offset 26 (PERS header) + 32
  // (build_id + texture_scale + bbox). surface_count is the first
  // one. Trash it to a huge value to trigger the chunk-bounds check.
  constexpr std::size_t sizes_off = 26 + 32;
  std::uint32_t huge = 0x40000000u;
  buf[sizes_off + 0] = static_cast<unsigned char>(huge & 0xff);
  buf[sizes_off + 1] = static_cast<unsigned char>((huge >> 8) & 0xff);
  buf[sizes_off + 2] = static_cast<unsigned char>((huge >> 16) & 0xff);
  buf[sizes_off + 3] = static_cast<unsigned char>((huge >> 24) & 0xff);
  auto ss = make_stream(buf);
  REQUIRE_FALSE(read_dig_file(ss).has_value());
}

TEST_CASE("read_dig_file rejects a negative array_size", "[dig]")
{
  auto buf = build_minimal_dig();
  constexpr std::size_t sizes_off = 26 + 32;
  std::uint32_t neg = 0xFFFFFFFFu;
  buf[sizes_off + 0] = static_cast<unsigned char>(neg & 0xff);
  buf[sizes_off + 1] = static_cast<unsigned char>((neg >> 8) & 0xff);
  buf[sizes_off + 2] = static_cast<unsigned char>((neg >> 16) & 0xff);
  buf[sizes_off + 3] = static_cast<unsigned char>((neg >> 24) & 0xff);
  auto ss = make_stream(buf);
  REQUIRE_FALSE(read_dig_file(ss).has_value());
}

TEST_CASE("read_dig_file rejects truncated input", "[dig]")
{
  auto buf = build_minimal_dig();
  // Lop off the trailing 8 bytes (highest_mip_level + flags). The
  // chunk_size field is still the old value so the parser will
  // discover that the chunk extends past the actual stream end at
  // the trailer read step.
  buf.resize(buf.size() - 8);
  auto ss = make_stream(buf);
  REQUIRE_FALSE(read_dig_file(ss).has_value());
}

namespace
{
  std::size_t for_each_dig(const std::filesystem::path& root,
                            const std::function<void(const std::filesystem::path&)>& visit)
  {
    if (!std::filesystem::is_directory(root)) return 0;
    std::size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root))
    {
      if (!entry.is_regular_file()) continue;
      if (entry.path().extension() != ".dig") continue;
      visit(entry.path());
      ++n;
    }
    return n;
  }
}

TEST_CASE("read_dig_file succeeds on the catwalkA sample with the documented build_id",
          "[dig][corpus]")
{
  const char* path = "/tmp/dig-samples/catwalkA-00.dig";
  std::ifstream f(path, std::ios::binary);
  if (!f)
  {
    WARN(std::string("sample not present: ") + path);
    return;
  }
  REQUIRE(is_dig_file(f));
  auto dig = read_dig_file(f);
  REQUIRE(dig.has_value());
  // Documented in docs/research/DIS-DIL.md — the DIG's build_id must
  // match the corresponding DIL's geometryBuildId.
  REQUIRE(dig->build_id == 0x140b9c4f);
  REQUIRE_FALSE(dig->surfaces.empty());
  REQUIRE_FALSE(dig->bsp_nodes.empty());
  REQUIRE_FALSE(dig->vertices.empty());
  REQUIRE_FALSE(dig->points3.empty());
  REQUIRE_FALSE(dig->planes.empty());
  // Hex-verified surface layout: the first surface's material index
  // is 0 (`xxd -s 0x68` shows `e0 00` — flags byte then material).
  REQUIRE(dig->surfaces[0].material == 0);
}

TEST_CASE("read_dig_file sweeps the full /tmp/dig-samples corpus",
          "[dig][corpus]")
{
  std::size_t ok = 0;
  std::size_t fail = 0;
  std::vector<std::string> failures;

  const std::size_t total = for_each_dig(
    "/tmp/dig-samples",
    [&](const std::filesystem::path& p) {
      std::ifstream f(p, std::ios::binary);
      if (!f)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (open)");
        return;
      }
      const auto file_size = std::filesystem::file_size(p);
      auto dig = read_dig_file(f);
      if (!dig)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (parse)");
        return;
      }
      // The parser leaves the stream at the declared chunk end —
      // which, for a well-formed shipping DIG, equals file_size.
      const auto end_pos = f.tellg();
      if (end_pos != std::streampos(static_cast<std::streamoff>(file_size)))
      {
        ++fail;
        failures.push_back(p.filename().string() + " (chunk end)");
        return;
      }
      ++ok;
    });

  if (total == 0)
  {
    WARN("no DIG samples present; corpus sweep skipped "
         "(run vol-list --extract on a *DML.vol to populate /tmp/dig-samples)");
    return;
  }

  INFO("DIG corpus: parsed " << ok << "/" << total << " files cleanly; "
       << fail << " failed");
  if (!failures.empty())
  {
    std::string msg = "first failing files: ";
    for (std::size_t i = 0; i < failures.size() && i < 10; ++i)
    {
      if (i) msg += ", ";
      msg += failures[i];
    }
    INFO(msg);
  }
  REQUIRE(fail == 0);
}
