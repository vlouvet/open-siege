// Unit tests for the DIL (ITRLighting / ITRMissionLighting v7) parser.
//
// Two layers of coverage:
//   1. In-memory smoke tests with synthesised buffers — no asset corpus needed.
//   2. Real-file checks against samples extracted from:
//        vol-list --extract human1DML.vol .dil /tmp/dil-stock
//        vol-list --extract missions/1_Welcome.vol .dil /tmp/dil-mission
//      Skipped (with WARN) when directories are absent.
//
// Spec: docs/clean-room-specs/DIL-INNER.md
// Validation table §9 cross-checks are used as ground truth.

#include <catch2/catch.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "content/interior/dil.hpp"

namespace
{
  // ---------------------------------------------------------------
  // Buffer-building helpers
  // ---------------------------------------------------------------

  void put_u8(std::vector<unsigned char>& b, std::uint8_t v)
  {
    b.push_back(v);
  }

  void put_u16(std::vector<unsigned char>& b, std::uint16_t v)
  {
    b.push_back(static_cast<unsigned char>(v & 0xff));
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
  }

  void put_u32(std::vector<unsigned char>& b, std::uint32_t v)
  {
    b.push_back(static_cast<unsigned char>(v & 0xff));
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    b.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
    b.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
  }

  void put_s32(std::vector<unsigned char>& b, std::int32_t v)
  {
    put_u32(b, static_cast<std::uint32_t>(v));
  }

  void put_f32(std::vector<unsigned char>& b, float v)
  {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    put_u32(b, bits);
  }

  void put_bytes(std::vector<unsigned char>& b, const void* p, std::size_t n)
  {
    const auto* u = static_cast<const unsigned char*>(p);
    b.insert(b.end(), u, u + n);
  }

  std::stringstream make_stream(const std::vector<unsigned char>& buf)
  {
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    ss.write(reinterpret_cast<const char*>(buf.data()),
             static_cast<std::streamsize>(buf.size()));
    ss.seekg(0);
    return ss;
  }

  // Patch a 4-byte LE u32 at position `pos` in `buf`.
  void patch_u32(std::vector<unsigned char>& buf, std::size_t pos, std::uint32_t v)
  {
    buf[pos + 0] = static_cast<unsigned char>(v & 0xff);
    buf[pos + 1] = static_cast<unsigned char>((v >> 8) & 0xff);
    buf[pos + 2] = static_cast<unsigned char>((v >> 16) & 0xff);
    buf[pos + 3] = static_cast<unsigned char>((v >> 24) & 0xff);
  }

  // ---------------------------------------------------------------
  // Build a minimal ITRLighting buffer with:
  //   state_count=0, sd_count=0, light_count=0, surface_count=1
  //   map_data_size=1, name_buf_size=0, huffman: N=1 L=2
  //
  // All concrete field values are chosen to satisfy the spec invariants
  // so parse_dil must succeed.
  // ---------------------------------------------------------------
  std::vector<unsigned char> build_minimal_itr_lighting(
    std::int32_t build_id = 0x140ae513)
  {
    std::vector<unsigned char> buf;

    // PERS magic
    buf.push_back('P'); buf.push_back('E'); buf.push_back('R'); buf.push_back('S');
    const std::size_t chunk_size_pos = buf.size();
    put_u32(buf, 0); // chunkSize placeholder — patched at end

    // classNameLength = 11
    put_u16(buf, 11);
    // "ITRLighting" (11 bytes) + NUL pad (1 byte, because 11 < 16)
    put_bytes(buf, "ITRLighting", 11);
    put_u8(buf, 0);
    // version = 7
    put_u32(buf, 7);

    // ---- payload header (32 bytes) ----
    put_s32(buf, build_id);   // geometry_build_id
    put_s32(buf, 4);          // light_scale_shift
    put_s32(buf, 0);          // lightmap_count (opaque hint)
    put_s32(buf, 0);          // state_count
    put_s32(buf, 0);          // state_data_count
    put_s32(buf, 0);          // light_count
    put_s32(buf, 1);          // surface_count
    put_s32(buf, 1);          // map_data_size

    // ---- Surface[0] (12 bytes) ----
    // map_index_or_color: bit 30 set, offset = 0
    put_u32(buf, 0x40000000u);
    put_u16(buf, 0); // light_count
    put_u16(buf, 0); // light_index
    put_u8(buf, 1);  // map_size_x
    put_u8(buf, 1);  // map_size_y
    put_u8(buf, 0);  // map_offset_x
    put_u8(buf, 0);  // map_offset_y

    // ---- MapData (1 byte) ----
    put_u8(buf, 0x80); // MSB = 1 => leaf 0 of tree below

    // ---- name_buffer_size = 0 ----
    put_u32(buf, 0);

    // ---- huffman_present = 1 ----
    put_u8(buf, 1);

    // ---- Huffman tree: N=1, L=2 ----
    put_s32(buf, 1); // nodes_count
    put_s32(buf, 2); // leaves_count (= N+1)
    // Node[0]: bit 0 -> leaf 1 (~(-2) = 1), bit 1 -> leaf 0 (~(-1) = 0)
    put_s32(buf, -2); // index_zero
    put_s32(buf, -1); // index_one
    // Leaf[0]: colour = 0x0000, padding = 0
    put_u16(buf, 0x0000); put_u16(buf, 0);
    // Leaf[1]: colour = 0xFFFF, padding = 0
    put_u16(buf, 0xFFFF); put_u16(buf, 0);

    // Patch chunkSize = file_size - 8.
    const std::uint32_t cs = static_cast<std::uint32_t>(buf.size() - 8);
    patch_u32(buf, chunk_size_pos, cs);
    return buf;
  }

  // ---------------------------------------------------------------
  // Corpus sweep helper
  // ---------------------------------------------------------------

  std::size_t for_each_dil(const std::filesystem::path& root,
                            const std::function<void(const std::filesystem::path&)>& visit)
  {
    if (!std::filesystem::is_directory(root)) return 0;
    std::size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root))
    {
      if (!entry.is_regular_file()) continue;
      if (entry.path().extension() != ".dil") continue;
      visit(entry.path());
      ++n;
    }
    return n;
  }

} // anonymous namespace

using studio::content::interior::is_darkstar_dil;
using studio::content::interior::parse_dil;

// ================================================================
// is_darkstar_dil
// ================================================================

TEST_CASE("is_darkstar_dil detects ITRLighting", "[dil]")
{
  auto buf = build_minimal_itr_lighting();
  auto ss  = make_stream(buf);
  REQUIRE(is_darkstar_dil(ss) == true);
  REQUIRE(ss.tellg() == std::streampos(0));
}

TEST_CASE("is_darkstar_dil rejects non-PERS magic", "[dil]")
{
  std::vector<unsigned char> buf;
  buf.push_back('I'); buf.push_back('T'); buf.push_back('R'); buf.push_back('s');
  for (int i = 0; i < 4; ++i) buf.push_back(0);
  auto ss = make_stream(buf);
  REQUIRE(is_darkstar_dil(ss) == false);
  REQUIRE(ss.tellg() == std::streampos(0));
}

TEST_CASE("is_darkstar_dil rejects PERS with wrong class", "[dil]")
{
  std::vector<unsigned char> buf;
  buf.push_back('P'); buf.push_back('E'); buf.push_back('R'); buf.push_back('S');
  put_u32(buf, 0);
  put_u16(buf, 11);
  put_bytes(buf, "ITRGeometry", 11);
  put_u8(buf, 0);
  put_u32(buf, 7);
  auto ss = make_stream(buf);
  REQUIRE(is_darkstar_dil(ss) == false);
  REQUIRE(ss.tellg() == std::streampos(0));
}

// ================================================================
// parse_dil — minimal hand-crafted ITRLighting
// ================================================================

TEST_CASE("parse_dil parses a hand-crafted minimal ITRLighting", "[dil]")
{
  auto buf = build_minimal_itr_lighting(0x140ae513);
  auto ss  = make_stream(buf);

  auto dil = parse_dil(ss);
  REQUIRE(dil.has_value());
  REQUIRE(dil->is_mission_lighting == false);
  REQUIRE(dil->geometry_build_id == 0x140ae513);
  REQUIRE(dil->light_scale_shift == 4);
  REQUIRE(dil->states.empty());
  REQUIRE(dil->state_data.empty());
  REQUIRE(dil->lights.empty());
  REQUIRE(dil->surfaces.size() == 1);

  auto const& sv = dil->surfaces[0];
  REQUIRE((sv.map_index_or_color & 0x40000000u) != 0); // bit 30 set
  REQUIRE((sv.map_index_or_color & 0x3FFFFFFFu) == 0); // offset = 0
  REQUIRE(sv.map_size_x == 1);
  REQUIRE(sv.map_size_y == 1);

  REQUIRE(dil->map_data.size() == 1);
  REQUIRE(dil->name_buffer.empty());

  // Huffman tree: N=1, L=2.
  REQUIRE(dil->huffman_nodes.size() == 1);
  REQUIRE(dil->huffman_leaves.size() == 2);
  REQUIRE(dil->huffman_nodes[0].index_zero == -2);
  REQUIRE(dil->huffman_nodes[0].index_one  == -1);
  REQUIRE(dil->huffman_leaves[0].colour == 0x0000);
  REQUIRE(dil->huffman_leaves[1].colour == 0xFFFF);

  REQUIRE(dil->index_remap.empty());

  // Stream should be exactly at EOF.
  REQUIRE(ss.tellg() == std::streampos(static_cast<std::streamoff>(buf.size())));
}

TEST_CASE("parse_dil returns nullopt on wrong PERS class", "[dil]")
{
  std::vector<unsigned char> buf;
  buf.push_back('P'); buf.push_back('E'); buf.push_back('R'); buf.push_back('S');
  put_u32(buf, 100);
  put_u16(buf, 11);
  put_bytes(buf, "ITRGeometry", 11);
  put_u8(buf, 0);
  put_u32(buf, 7);
  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_dil(ss).has_value());
}

TEST_CASE("parse_dil returns nullopt on wrong version", "[dil]")
{
  auto buf = build_minimal_itr_lighting();
  // Version field is at offset 22 (4 PERS + 4 chunkSize + 2 nameLen + 11 name + 1 pad = 22).
  patch_u32(buf, 22, 6); // version 6 not accepted
  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_dil(ss).has_value());
}

TEST_CASE("parse_dil returns nullopt on Huffman tree L != N+1", "[dil]")
{
  auto buf = build_minimal_itr_lighting();
  // Find the huffman_present byte: it follows the name buffer (size=0) which
  // follows the map_data (1 byte), which follows the surface, which follows
  // the 32-byte payload header. Payload header starts at offset 26.
  // Offset 26: 32 byte header = 58
  // Surface: 12 bytes = 70
  // MapData: 1 byte  = 71
  // name_buf_size u32: 4 bytes = 75
  // huffman_present u8: 1 byte = 76
  // nodes_count s32: 4 bytes = 80
  // leaves_count s32: 4 bytes = 84
  // Corrupt leaves_count to 5 (should be 2 for N=1).
  patch_u32(buf, 80, 5); // leaves_count = 5 (not N+1=2)
  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_dil(ss).has_value());
}

// ================================================================
// Real-file corpus tests — skipped when samples not present
// ================================================================

TEST_CASE("parse_dil matches spec §9 for BELcargo4-010.dil", "[dil][corpus]")
{
  const char* path = "/tmp/dil-stock/BELcargo4-010.dil";
  std::ifstream f(path, std::ios::binary);
  if (!f)
  {
    WARN(std::string("sample not present: ") + path);
    return;
  }
  REQUIRE(is_darkstar_dil(f));
  auto dil = parse_dil(f);
  REQUIRE(dil.has_value());
  // Spec §9 validation table row 1.
  REQUIRE(dil->geometry_build_id   == static_cast<std::int32_t>(0x140ae513u));
  REQUIRE(dil->light_scale_shift   == 4);
  REQUIRE(dil->states.empty());
  REQUIRE(dil->state_data.empty());
  REQUIRE(dil->lights.empty());
  REQUIRE(dil->surfaces.size()     == 6);
  REQUIRE(dil->map_data.size()     == 103);
  REQUIRE(dil->is_mission_lighting == false);
  REQUIRE(dil->index_remap.empty());
}

TEST_CASE("parse_dil matches spec §9 for BELcargo4-000.dil", "[dil][corpus]")
{
  const char* path = "/tmp/dil-stock/BELcargo4-000.dil";
  std::ifstream f(path, std::ios::binary);
  if (!f) { WARN(std::string("sample not present: ") + path); return; }
  auto dil = parse_dil(f);
  REQUIRE(dil.has_value());
  REQUIRE(dil->geometry_build_id == static_cast<std::int32_t>(0x140ae35bu));
  REQUIRE(dil->light_scale_shift == 4);
  REQUIRE(dil->surfaces.size()   == 20);
  REQUIRE(dil->map_data.size()   == 134);
}

TEST_CASE("parse_dil matches spec §9 for BELcargo2-020.dil (animated lights)", "[dil][corpus]")
{
  const char* path = "/tmp/dil-stock/BELcargo2-020.dil";
  std::ifstream f(path, std::ios::binary);
  if (!f) { WARN(std::string("sample not present: ") + path); return; }
  auto dil = parse_dil(f);
  REQUIRE(dil.has_value());
  REQUIRE(dil->geometry_build_id == static_cast<std::int32_t>(0x140ad79eu));
  REQUIRE(dil->light_scale_shift == 4);
  REQUIRE(dil->states.size()     == 24);
  REQUIRE(dil->state_data.size() == 24);
  REQUIRE(dil->lights.size()     == 8);
  REQUIRE(dil->surfaces.size()   == 6);
  REQUIRE(dil->map_data.size()   == 1420);
  // Spec worked example §3.1: State[0].animation_time == 0.0f
  REQUIRE(dil->states[0].animation_time == Approx(0.0f));
  // State[1].animation_time == 0.1f (from §3.1)
  REQUIRE(dil->states[1].animation_time == Approx(0.1f).epsilon(1e-4));
}

TEST_CASE("parse_dil matches spec §9 for BATower-000.dil (large)", "[dil][corpus]")
{
  const char* path = "/tmp/dil-stock/BATower-000.dil";
  std::ifstream f(path, std::ios::binary);
  if (!f) { WARN(std::string("sample not present: ") + path); return; }
  auto dil = parse_dil(f);
  REQUIRE(dil.has_value());
  REQUIRE(dil->geometry_build_id == static_cast<std::int32_t>(0x0f6d2d3bu));
  REQUIRE(dil->light_scale_shift == 4);
  REQUIRE(dil->surfaces.size()   == 249);
  REQUIRE(dil->map_data.size()   == 8160);
}

TEST_CASE("parse_dil matches spec §9 for catwalkA-000-0.dil (mission)", "[dil][corpus]")
{
  const char* path = "/tmp/dil-mission/catwalkA-000-0.dil";
  std::ifstream f(path, std::ios::binary);
  if (!f) { WARN(std::string("sample not present: ") + path); return; }
  REQUIRE(is_darkstar_dil(f));
  auto dil = parse_dil(f);
  REQUIRE(dil.has_value());
  // Spec §9.1 cross-check: build_id must match catwalkA-00.dig.
  REQUIRE(dil->geometry_build_id   == static_cast<std::int32_t>(0x140b9c4fu));
  REQUIRE(dil->light_scale_shift   == 4);
  REQUIRE(dil->is_mission_lighting == true);
  REQUIRE(dil->surfaces.size()     == 34);
  REQUIRE(dil->map_data.size()     == 514);
  // Mission-zero fields.
  REQUIRE(dil->states.empty());
  REQUIRE(dil->state_data.empty());
  REQUIRE(dil->lights.empty());
  // LZH remap: spec §9.2 index_array_size = 34.
  REQUIRE(dil->index_remap.size()  == 34);
}

TEST_CASE("parse_dil matches spec §9 for catwalkB-000-0.dil (mission)", "[dil][corpus]")
{
  const char* path = "/tmp/dil-mission/catwalkB-000-0.dil";
  std::ifstream f(path, std::ios::binary);
  if (!f) { WARN(std::string("sample not present: ") + path); return; }
  auto dil = parse_dil(f);
  REQUIRE(dil.has_value());
  REQUIRE(dil->geometry_build_id   == static_cast<std::int32_t>(0x140b9f7au));
  REQUIRE(dil->light_scale_shift   == 4);
  REQUIRE(dil->is_mission_lighting == true);
  REQUIRE(dil->surfaces.size()     == 25);
  REQUIRE(dil->map_data.size()     == 434);
  // Spec §9.2: index_array_size = 25.
  REQUIRE(dil->index_remap.size()  == 25);
}

TEST_CASE("parse_dil matches spec §9 for tank16-000-0.dil (mission)", "[dil][corpus]")
{
  const char* path = "/tmp/dil-mission/tank16-000-0.dil";
  std::ifstream f(path, std::ios::binary);
  if (!f) { WARN(std::string("sample not present: ") + path); return; }
  auto dil = parse_dil(f);
  REQUIRE(dil.has_value());
  REQUIRE(dil->geometry_build_id   == static_cast<std::int32_t>(0x0f3eb954u));
  REQUIRE(dil->light_scale_shift   == 4);
  REQUIRE(dil->is_mission_lighting == true);
  REQUIRE(dil->surfaces.size()     == 107);
  REQUIRE(dil->map_data.size()     == 2756);
  // Spec §9.2: index_array_size = 107.
  REQUIRE(dil->index_remap.size()  == 107);
}

// Build-id cross-check §9.1: catwalkA DIG must match catwalkA DIL.
TEST_CASE("build_id cross-check: catwalkA DIL matches its DIG", "[dil][corpus]")
{
  // Read build_id from the DIG first.
  const char* dig_path = "/tmp/dig-samples/catwalkA-00.dig";
  const char* dil_path = "/tmp/dil-stock/catwalkA-000.dil";

  std::ifstream dig_f(dig_path, std::ios::binary);
  std::ifstream dil_f(dil_path, std::ios::binary);

  if (!dig_f || !dil_f)
  {
    WARN("catwalkA samples not present — cross-check skipped. "
         "Extract with vol-list --extract human1DML.vol .dig /tmp/dig-samples "
         "and vol-list --extract human1DML.vol .dil /tmp/dil-stock");
    return;
  }

  auto dil = parse_dil(dil_f);
  REQUIRE(dil.has_value());
  // Documented build_id for catwalkA family (spec §9.1).
  REQUIRE(dil->geometry_build_id == static_cast<std::int32_t>(0x140b9c4fu));
}

// ================================================================
// Corpus sweeps
// ================================================================

TEST_CASE("parse_dil sweeps the full /tmp/dil-stock corpus (stock ITRLighting)",
          "[dil][corpus]")
{
  std::size_t ok   = 0;
  std::size_t fail = 0;
  std::vector<std::string> failures;

  const std::size_t total = for_each_dil(
    "/tmp/dil-stock",
    [&](const std::filesystem::path& p) {
      std::ifstream f(p, std::ios::binary);
      if (!f)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (open)");
        return;
      }
      const auto file_size = std::filesystem::file_size(p);
      auto dil = parse_dil(f);
      if (!dil)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (parse)");
        return;
      }
      // Spec invariants.
      if (dil->surfaces.size() < 6)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (surface_count < 6)");
        return;
      }
      if (dil->light_scale_shift != 4)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (light_scale_shift != 4)");
        return;
      }
      if (dil->is_mission_lighting)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (unexpected mission flag)");
        return;
      }
      // Stream at EOF (spec §8 end-of-file alignment).
      const auto end_pos = f.tellg();
      if (end_pos != std::streampos(static_cast<std::streamoff>(file_size)))
      {
        ++fail;
        failures.push_back(p.filename().string() + " (EOF alignment)");
        return;
      }
      ++ok;
    });

  if (total == 0)
  {
    WARN("no stock DIL samples present; corpus sweep skipped. "
         "Run: vol-list --extract human1DML.vol .dil /tmp/dil-stock");
    return;
  }

  INFO("Stock DIL corpus: " << ok << "/" << total << " ok; " << fail << " failed");
  if (!failures.empty())
  {
    std::string msg = "first failures: ";
    for (std::size_t i = 0; i < failures.size() && i < 10; ++i)
    {
      if (i) msg += ", ";
      msg += failures[i];
    }
    INFO(msg);
  }
  REQUIRE(fail == 0);
}

TEST_CASE("parse_dil sweeps the full /tmp/dil-mission corpus (ITRMissionLighting)",
          "[dil][corpus]")
{
  std::size_t ok   = 0;
  std::size_t fail = 0;
  std::vector<std::string> failures;

  const std::size_t total = for_each_dil(
    "/tmp/dil-mission",
    [&](const std::filesystem::path& p) {
      std::ifstream f(p, std::ios::binary);
      if (!f)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (open)");
        return;
      }
      const auto file_size = std::filesystem::file_size(p);
      auto dil = parse_dil(f);
      if (!dil)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (parse)");
        return;
      }
      if (dil->surfaces.size() < 6)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (surface_count < 6)");
        return;
      }
      if (dil->light_scale_shift != 4)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (light_scale_shift != 4)");
        return;
      }
      if (!dil->is_mission_lighting)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (expected mission flag)");
        return;
      }
      // Mission index_remap must be non-empty (spec §7.4: surface_count >= 6
      // and index_array_size > 0).
      if (dil->index_remap.empty())
      {
        ++fail;
        failures.push_back(p.filename().string() + " (empty index_remap)");
        return;
      }
      // EOF alignment.
      const auto end_pos = f.tellg();
      if (end_pos != std::streampos(static_cast<std::streamoff>(file_size)))
      {
        ++fail;
        failures.push_back(p.filename().string() + " (EOF alignment)");
        return;
      }
      ++ok;
    });

  if (total == 0)
  {
    WARN("no mission DIL samples present; corpus sweep skipped. "
         "Run: vol-list --extract missions/1_Welcome.vol .dil /tmp/dil-mission");
    return;
  }

  INFO("Mission DIL corpus: " << ok << "/" << total << " ok; " << fail << " failed");
  if (!failures.empty())
  {
    std::string msg = "first failures: ";
    for (std::size_t i = 0; i < failures.size() && i < 10; ++i)
    {
      if (i) msg += ", ";
      msg += failures[i];
    }
    INFO(msg);
  }
  REQUIRE(fail == 0);
}
