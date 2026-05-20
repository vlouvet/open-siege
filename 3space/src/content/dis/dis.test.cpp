// Unit tests for the DIS manifest parser.
//
// Two layers of coverage:
//   1. In-memory smoke tests with synthesised buffers, so the suite
//      runs on contributor machines without the Tribes asset corpus.
//   2. Real-file checks against samples extracted from the per-world
//      `human1DML.vol`. Skipped (with WARN) when /tmp/dis-samples is
//      empty.

#include <catch2/catch.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "content/dis/dis.hpp"

namespace
{
  void put_u32(std::vector<unsigned char>& buf, std::uint32_t v)
  {
    buf.push_back(static_cast<unsigned char>(v & 0xff));
    buf.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    buf.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
    buf.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
  }

  void put_bytes(std::vector<unsigned char>& buf,
                 const void* data,
                 std::size_t n)
  {
    auto const* p = static_cast<const unsigned char*>(data);
    buf.insert(buf.end(), p, p + n);
  }

  std::stringstream make_stream(const std::vector<unsigned char>& buf)
  {
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    ss.write(reinterpret_cast<const char*>(buf.data()),
             static_cast<std::streamsize>(buf.size()));
    ss.seekg(0);
    return ss;
  }

  // Hand-crafted minimal DIS file: 1 LOD, 1 light state, names match
  // the worked example layout from DIS-DIL.md but trimmed.
  std::vector<unsigned char> build_minimal_dis()
  {
    // Name table layout:
    //   +0   "State 0\0"               (8 bytes)
    //   +8   "shape-00.dig\0"          (13 bytes)
    //   +21  "shape-000.dil\0"         (14 bytes)
    //   +35  "shape.dml\0"             (10 bytes)
    //   +45  "default\0"               (8 bytes)
    // total = 53 bytes
    const char name_table[] = {
      'S','t','a','t','e',' ','0','\0',
      's','h','a','p','e','-','0','0','.','d','i','g','\0',
      's','h','a','p','e','-','0','0','0','.','d','i','l','\0',
      's','h','a','p','e','.','d','m','l','\0',
      'd','e','f','a','u','l','t','\0'
    };
    constexpr std::uint32_t name_table_size = sizeof(name_table);
    static_assert(name_table_size == 53, "Recompute offsets if layout changes");

    std::vector<unsigned char> buf;
    // Magic.
    const char magic[4] = {'I','T','R','s'};
    put_bytes(buf, magic, 4);
    // chunkSize (placeholder — patch at the end).
    std::size_t chunk_size_pos = buf.size();
    put_u32(buf, 0);
    put_u32(buf, 3);      // version
    put_u32(buf, 1);      // numStates
    put_u32(buf, 0);      // reserved
    put_u32(buf, 0);      // reserved
    put_u32(buf, 1);      // numLods
    put_u32(buf, 1);      // numLods_dup
    // LOD record:
    put_u32(buf, 100);    // minPixels
    put_u32(buf, 8);      // geomNameOffset -> "shape-00.dig"
    put_u32(buf, 0);      // lightStateIdx
    put_u32(buf, 0xFC);   // linkableFaces
    // numLightStates + DIL offsets.
    put_u32(buf, 1);      // numLightStates
    put_u32(buf, 21);     // dilOffset[0] -> "shape-000.dil"
    put_u32(buf, 1);      // constant 1
    put_u32(buf, 45);     // defaultStateNameOffset -> "default"
    put_u32(buf, name_table_size);
    // Name table.
    put_bytes(buf, name_table, name_table_size);
    // Trailer.
    put_u32(buf, 35);     // materialListNameOffset -> "shape.dml"
    buf.push_back(0);     // linkedInterior

    // Patch chunkSize = file_size - 8.
    std::uint32_t chunk_size = static_cast<std::uint32_t>(buf.size() - 8);
    buf[chunk_size_pos + 0] = static_cast<unsigned char>(chunk_size & 0xff);
    buf[chunk_size_pos + 1] = static_cast<unsigned char>((chunk_size >> 8) & 0xff);
    buf[chunk_size_pos + 2] = static_cast<unsigned char>((chunk_size >> 16) & 0xff);
    buf[chunk_size_pos + 3] = static_cast<unsigned char>((chunk_size >> 24) & 0xff);
    return buf;
  }
}

using studio::content::dis::parse_dis;
using studio::content::dis::is_darkstar_dis;

TEST_CASE("is_darkstar_dis detects ITRs magic", "[dis]")
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  ss.write("ITRs\x00\x00\x00\x00", 8);
  ss.seekg(0);
  REQUIRE(is_darkstar_dis(ss) == true);
  REQUIRE(ss.tellg() == std::streampos(0));
}

TEST_CASE("is_darkstar_dis rejects non-DIS magic", "[dis]")
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  ss.write("PERS\x00\x00\x00\x00", 8);
  ss.seekg(0);
  REQUIRE(is_darkstar_dis(ss) == false);
  REQUIRE(ss.tellg() == std::streampos(0));
}

TEST_CASE("parse_dis returns nullopt when the magic is wrong", "[dis]")
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  ss.write("PERS....", 8);
  ss.seekg(0);
  REQUIRE_FALSE(parse_dis(ss).has_value());
}

TEST_CASE("parse_dis reads a hand-crafted manifest", "[dis]")
{
  auto buf = build_minimal_dis();
  auto ss = make_stream(buf);

  auto m = parse_dis(ss);
  REQUIRE(m.has_value());
  REQUIRE(m->version == 3);
  REQUIRE(m->num_states == 1);
  REQUIRE(m->default_state_name == "default");
  REQUIRE(m->lods.size() == 1);
  REQUIRE(m->lods[0].min_pixels == 100);
  REQUIRE(m->lods[0].geometry_file == "shape-00.dig");
  REQUIRE(m->lods[0].light_state_index == 0);
  REQUIRE(m->lods[0].linkable_faces_flags == 0xFC);
  REQUIRE(m->lightmap_files.size() == 1);
  REQUIRE(m->lightmap_files[0] == "shape-000.dil");
  REQUIRE(m->material_list_file == "shape.dml");
  REQUIRE(m->linked_interior == 0);

  REQUIRE(m->lod_dig_files().size() == 1);
  REQUIRE(m->lod_dig_files()[0] == "shape-00.dig");
  REQUIRE(m->lod_distances().size() == 1);
  REQUIRE(m->lod_distances()[0] == 100.0f);
  REQUIRE(m->lightmap_file() == "shape-000.dil");
}

TEST_CASE("parse_dis rejects out-of-range name offsets", "[dis]")
{
  auto buf = build_minimal_dis();
  // Trash the materialListNameOffset (4 bytes before the final
  // linkedInterior byte). The buffer is `... nameTable, u32 mlo, u8 li`.
  std::uint32_t evil = 0xdeadbeef;
  std::size_t mlo_pos = buf.size() - 5;
  buf[mlo_pos + 0] = static_cast<unsigned char>(evil & 0xff);
  buf[mlo_pos + 1] = static_cast<unsigned char>((evil >> 8) & 0xff);
  buf[mlo_pos + 2] = static_cast<unsigned char>((evil >> 16) & 0xff);
  buf[mlo_pos + 3] = static_cast<unsigned char>((evil >> 24) & 0xff);
  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_dis(ss).has_value());
}

TEST_CASE("parse_dis rejects mismatched numLods/numLods_dup", "[dis]")
{
  auto buf = build_minimal_dis();
  // Flip numLods_dup (at offset 4+4+4+4+4+4+4 = 28 from file start)
  // from 1 to 2 — should now mismatch numLods=1.
  constexpr std::size_t dup_pos = 28;
  buf[dup_pos + 0] = 2;
  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_dis(ss).has_value());
}

namespace
{
  // Walk /tmp/dis-samples, calling `visit(path)` for every entry
  // whose extension is `.dis`. Returns the number of entries visited
  // so the corpus test can WARN when the directory is empty.
  std::size_t for_each_dis(const std::filesystem::path& root,
                            const std::function<void(const std::filesystem::path&)>& visit)
  {
    if (!std::filesystem::is_directory(root)) return 0;
    std::size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root))
    {
      if (!entry.is_regular_file()) continue;
      if (entry.path().extension() != ".dis") continue;
      visit(entry.path());
      ++n;
    }
    return n;
  }
}

TEST_CASE("parse_dis succeeds on real DIS samples", "[dis][corpus]")
{
  // Specifically-named samples — verify each carries the expected
  // material list and at least one DIG/DIL entry.
  struct named_case
  {
    const char* path;
    const char* expected_dml;
    std::size_t min_lods;
  };

  const named_case named[] = {
    {"/tmp/dis-samples/catwalkA.dis", "catwalkA.dml", 1},
    {"/tmp/dis-samples/catwalkB.dis", "catwalkB.dml", 1},
    {"/tmp/dis-samples/command1.dis", "command1.dml", 1},
    {"/tmp/dis-samples/command2.dis", "command2.dml", 1},
    {"/tmp/dis-samples/fcomm.dis",    "fcomm.dml",    1},
  };

  std::size_t hit = 0;
  for (auto const& tc : named)
  {
    std::ifstream f(tc.path, std::ios::binary);
    if (!f)
    {
      WARN(std::string("sample not present: ") + tc.path);
      continue;
    }
    auto m = parse_dis(f);
    REQUIRE(m.has_value());
    REQUIRE(m->version == 3);
    REQUIRE(m->num_states == 1);
    REQUIRE(m->default_state_name == "default");
    REQUIRE(m->lods.size() >= tc.min_lods);
    REQUIRE(m->material_list_file == tc.expected_dml);
    REQUIRE_FALSE(m->lightmap_files.empty());
    // Every LOD must reference a `.dig` file and a valid linkable
    // faces flag (universal 0xFC across all 517 shipping files).
    for (auto const& lod : m->lods)
    {
      REQUIRE(lod.geometry_file.find(".dig") != std::string::npos);
      REQUIRE(lod.linkable_faces_flags == 0xFC);
    }
    // Every DIL filename ends in ".dil".
    for (auto const& dil : m->lightmap_files)
    {
      REQUIRE(dil.find(".dil") != std::string::npos);
    }
    ++hit;
  }

  if (hit == 0)
  {
    WARN("no named DIS samples present at /tmp/dis-samples/ — "
         "run vol-list --extract on human1DML.vol first");
  }
}

TEST_CASE("parse_dis sweeps the full /tmp/dis-samples corpus",
          "[dis][corpus]")
{
  std::size_t ok = 0;
  std::size_t fail = 0;
  std::vector<std::string> failures;

  std::size_t total = for_each_dis(
    "/tmp/dis-samples",
    [&](const std::filesystem::path& p) {
      std::ifstream f(p, std::ios::binary);
      if (!f)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (open)");
        return;
      }
      auto m = parse_dis(f);
      if (!m)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (parse)");
        return;
      }
      // Structural sanity: every LOD references a .dig, every
      // light state references a .dil, exactly one .dml.
      bool good = m->version == 3
                  && !m->lods.empty()
                  && m->lods.size() == m->lightmap_files.size()
                  && !m->material_list_file.empty();
      for (auto const& lod : m->lods)
      {
        if (lod.geometry_file.find(".dig") == std::string::npos)
        {
          good = false;
          break;
        }
      }
      for (auto const& dil : m->lightmap_files)
      {
        if (dil.find(".dil") == std::string::npos)
        {
          good = false;
          break;
        }
      }
      if (good) ++ok;
      else
      {
        ++fail;
        failures.push_back(p.filename().string() + " (sanity)");
      }
    });

  if (total == 0)
  {
    WARN("no DIS samples present; corpus sweep skipped");
    return;
  }

  // Emit a status line that the corpus run-log captures verbatim.
  INFO("DIS corpus: parsed " << ok << "/" << total << " files cleanly; "
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
