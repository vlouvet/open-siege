// Unit tests for the per-world Terrain.dat / Grid.dat / Rules.dat
// parsers.
//
// Layout:
//   1. In-memory tests against hand-crafted minimal buffers so the
//      suite runs on contributor machines without the Tribes asset
//      corpus.
//   2. Real-file sweep against samples extracted with
//        ./vol-list --extract <world>Terrain.vol .dat /tmp/worlddats-<world>
//      Skipped (with WARN) when the directory is absent.

#include <catch2/catch.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "content/terrain/world_dats.hpp"

namespace
{
  void put_u8(std::vector<unsigned char>& buf, std::uint8_t v)
  {
    buf.push_back(v);
  }

  void put_u32(std::vector<unsigned char>& buf, std::uint32_t v)
  {
    buf.push_back(static_cast<unsigned char>(v & 0xff));
    buf.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    buf.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
    buf.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
  }

  void put_i32(std::vector<unsigned char>& buf, std::int32_t v)
  {
    std::uint32_t u;
    std::memcpy(&u, &v, 4);
    put_u32(buf, u);
  }

  void put_f32(std::vector<unsigned char>& buf, float v)
  {
    std::uint32_t u;
    std::memcpy(&u, &v, 4);
    put_u32(buf, u);
  }

  void put_bytes(std::vector<unsigned char>& buf, std::size_t n,
                 unsigned char value = 0)
  {
    buf.insert(buf.end(), n, value);
  }

  void put_string_field(std::vector<unsigned char>& buf,
                        const std::string& s,
                        std::size_t cap)
  {
    REQUIRE(s.size() <= cap);
    for (char c : s) buf.push_back(static_cast<unsigned char>(c));
    put_bytes(buf, cap - s.size());
  }

  std::stringstream make_stream(const std::vector<unsigned char>& buf)
  {
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    ss.write(reinterpret_cast<const char*>(buf.data()),
             static_cast<std::streamsize>(buf.size()));
    ss.seekg(0);
    return ss;
  }
}

using studio::content::terrain::parse_terrain_dat;
using studio::content::terrain::parse_grid_dat;
using studio::content::terrain::parse_rules_dat;

// ----- Terrain.dat -----------------------------------------------------

TEST_CASE("parse_terrain_dat reads a hand-crafted file", "[terrain][dat]")
{
  std::vector<unsigned char> buf;
  put_u32(buf, 2);  // numTypes
  put_u32(buf, 3);  // numTextures

  // Type descriptions (2 x 32 bytes).
  put_string_field(buf, " 1 Dirt", 32);
  put_string_field(buf, " 2 Rock", 32);

  // Three texture records (each 276 bytes).
  for (std::uint32_t i = 0; i < 3; ++i)
  {
    const std::string name = "tex" + std::to_string(i) + ".BMP";
    put_string_field(buf, name, 128);
    put_bytes(buf, 128);                               // reserved
    put_u8(buf, 2); put_u8(buf, 2);
    put_u8(buf, 2); put_u8(buf, 2);                    // cornerTypeTags
    put_u32(buf, 0x000014ff);                          // sides
    put_u32(buf, 0x0000000d);                          // classifierWord
    put_f32(buf, 0.5f);                                // elasticity
    put_f32(buf, 1.0f);                                // friction
  }

  REQUIRE(buf.size() == 8 + 2 * 32 + 3 * 276);

  auto ss = make_stream(buf);
  auto td = parse_terrain_dat(ss);
  REQUIRE(td.has_value());
  REQUIRE(td->type_descriptions.size() == 2);
  REQUIRE(td->type_descriptions[0] == " 1 Dirt");
  REQUIRE(td->type_descriptions[1] == " 2 Rock");
  REQUIRE(td->records.size() == 3);
  REQUIRE(td->records[0].bitmap_name == "tex0.BMP");
  REQUIRE(td->records[0].corner_type_tags == std::array<std::uint8_t, 4>{2, 2, 2, 2});
  REQUIRE(td->records[0].sides == 0x000014ff);
  REQUIRE(td->records[0].classifier_word == 0x0000000d);
  REQUIRE(td->records[0].elasticity == 0.5f);
  REQUIRE(td->records[0].friction == 1.0f);
  for (auto byte : td->records[0].reserved_block)
  {
    REQUIRE(byte == 0);
  }
}

TEST_CASE("parse_terrain_dat rejects an absurd numTypes",
          "[terrain][dat]")
{
  std::vector<unsigned char> buf;
  put_u32(buf, 1000);  // way above the 64 cap
  put_u32(buf, 1);
  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_terrain_dat(ss).has_value());
}

TEST_CASE("parse_terrain_dat rejects a truncated file",
          "[terrain][dat]")
{
  std::vector<unsigned char> buf;
  put_u32(buf, 1);
  put_u32(buf, 1);
  // Should be 32 + 276 more bytes, but we only emit 10.
  put_bytes(buf, 10);
  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_terrain_dat(ss).has_value());
}

// ----- Grid.dat --------------------------------------------------------

TEST_CASE("parse_grid_dat reads a hand-crafted file", "[terrain][grid]")
{
  // numBaseTypes = 1, so B = 2, B^4 = 16.
  std::vector<unsigned char> buf;
  put_u32(buf, 3);   // gridVersion
  put_u32(buf, 1);   // numBaseTypes
  put_u32(buf, 2);   // numBaseTextures
  put_u32(buf, 0);   // numPicks (always 0 on disk)

  put_string_field(buf, " 1 Dirt", 32);

  // per-texture block (2 textures x 4 bytes).
  for (std::uint8_t tex = 0; tex < 2; ++tex)
  {
    put_u8(buf, tex); put_u8(buf, tex); put_u8(buf, tex); put_u8(buf, tex);
  }

  // tex_combos: 16 u32s. Combo 0 maps to start-of-list (0),
  // every other combo maps to position 4 (= end of slice).
  for (std::uint32_t k = 0; k < 16; ++k)
  {
    put_u32(buf, k == 0 ? 0u : 4u);
  }

  // pick_offs: 17 u32s. combo 0 ends at 4, all later combos end
  // at 4 too. Trailing sentinel = 4.
  for (std::uint32_t k = 0; k < 17; ++k)
  {
    put_u32(buf, 4u);
  }

  // pick_list: 4 pairs.
  for (std::uint8_t i = 0; i < 4; ++i)
  {
    put_u8(buf, i);   // texture_index
    put_u8(buf, 0);   // flags
  }

  auto ss = make_stream(buf);
  auto gd = parse_grid_dat(ss);
  REQUIRE(gd.has_value());
  REQUIRE(gd->version == 3);
  REQUIRE(gd->num_base_types == 1);
  REQUIRE(gd->num_base_textures == 2);
  REQUIRE(gd->tex_combos.size() == 16);
  REQUIRE(gd->pick_offs.size() == 17);
  REQUIRE(gd->pick_list.size() == 4);
  REQUIRE(gd->pick_offs.back() == gd->pick_list.size());
  REQUIRE(gd->pick_list[0].texture_index == 0);
  REQUIRE(gd->pick_list[3].texture_index == 3);
}

TEST_CASE("parse_grid_dat rejects an unsupported version",
          "[terrain][grid]")
{
  std::vector<unsigned char> buf;
  put_u32(buf, 2);   // version != 3 -> rejected for v1
  put_u32(buf, 1);
  put_u32(buf, 1);
  put_u32(buf, 0);
  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_grid_dat(ss).has_value());
}

TEST_CASE("parse_grid_dat rejects a sentinel/list-length mismatch",
          "[terrain][grid]")
{
  std::vector<unsigned char> buf;
  put_u32(buf, 3);
  put_u32(buf, 1);
  put_u32(buf, 1);
  put_u32(buf, 0);
  put_string_field(buf, "type", 32);
  put_u8(buf, 0); put_u8(buf, 0); put_u8(buf, 0); put_u8(buf, 0);
  for (std::uint32_t k = 0; k < 16; ++k) put_u32(buf, 0);
  // pick_offs: declare 99 pairs but emit 1
  for (std::uint32_t k = 0; k < 16; ++k) put_u32(buf, 99);
  put_u32(buf, 99);
  put_u8(buf, 0); put_u8(buf, 0);
  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_grid_dat(ss).has_value());
}

// ----- Rules.dat -------------------------------------------------------

TEST_CASE("parse_rules_dat reads a hand-crafted file",
          "[terrain][rules]")
{
  std::vector<unsigned char> buf;
  put_u32(buf, 2);   // numRules

  put_i32(buf, 0);                 // groupNum
  put_f32(buf, 50.0f);             // alt_min
  put_f32(buf, 350.0f);            // alt_max
  put_f32(buf, 150.0f);            // alt_mean
  put_f32(buf, 0.5f);              // alt_sdev
  put_f32(buf, 0.5f);              // alt_weight
  put_i32(buf, 0);                 // adj_heights
  put_f32(buf, 0.0f);              // slope_min
  put_f32(buf, 8.0f);              // slope_max
  put_f32(buf, 1.5f);              // slope_mean
  put_f32(buf, 0.5f);              // slope_sdev
  put_f32(buf, 0.5f);              // slope_weight
  put_i32(buf, 0);                 // adj_slopes

  // Second rule - all zero.
  put_bytes(buf, 52);

  REQUIRE(buf.size() == 4 + 2 * 52);

  auto ss = make_stream(buf);
  auto rd = parse_rules_dat(ss);
  REQUIRE(rd.has_value());
  REQUIRE(rd->rules.size() == 2);
  REQUIRE(rd->rules[0].group_num == 0);
  REQUIRE(rd->rules[0].alt_min == 50.0f);
  REQUIRE(rd->rules[0].alt_max == 350.0f);
  REQUIRE(rd->rules[0].alt_mean == 150.0f);
  REQUIRE(rd->rules[0].alt_sdev == 0.5f);
  REQUIRE(rd->rules[0].alt_weight == 0.5f);
  REQUIRE(rd->rules[0].slope_max == 8.0f);
  REQUIRE(rd->rules[0].slope_mean == 1.5f);
}

TEST_CASE("parse_rules_dat rejects a wrong-sized file",
          "[terrain][rules]")
{
  std::vector<unsigned char> buf;
  put_u32(buf, 2);
  // Only one rule's worth of bytes, not two.
  put_bytes(buf, 52);
  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_rules_dat(ss).has_value());
}

TEST_CASE("parse_rules_dat rejects an absurd numRules",
          "[terrain][rules]")
{
  std::vector<unsigned char> buf;
  put_u32(buf, 999);   // > 256 sanity cap
  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_rules_dat(ss).has_value());
}

// ----- Corpus sweep ---------------------------------------------------

namespace
{
  // Six shipping worlds and the per-world counts derived from the
  // spec (verified by hex inspection in this PR).
  struct world_case
  {
    const char* world;
    std::uint32_t num_types;
    std::uint32_t num_textures;
    std::uint32_t num_rules;
    std::size_t terrain_size;
    std::size_t grid_size;
    std::size_t rules_size;
  };

  constexpr world_case kWorlds[] = {
    {"alien",  6, 100, 4, 27800, 21214, 212},
    {"desert", 8, 171, 6, 47460, 56178, 316},
    {"ice",    6, 132, 6, 36632, 22194, 316},
    {"lush",   9, 184, 6, 51080, 83822, 316},
    {"mars",   6, 136, 6, 37736, 23542, 316},
    {"mud",    5, 142, 5, 39360, 13694, 264},
  };

  std::string corpus_path(const char* world, const char* suffix)
  {
    std::string p = "/tmp/worlddats-";
    p += world;
    p += "/";
    p += world;
    p += suffix;
    return p;
  }
}

TEST_CASE("parse_terrain_dat sweeps all six shipping worlds",
          "[terrain][dat][corpus]")
{
  std::size_t hit = 0;
  for (auto const& w : kWorlds)
  {
    const auto path = corpus_path(w.world, ".Terrain.dat");
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
      WARN("sample not present: " << path);
      continue;
    }
    auto td = parse_terrain_dat(f);
    REQUIRE(td.has_value());
    REQUIRE(td->type_descriptions.size() == w.num_types);
    REQUIRE(td->records.size() == w.num_textures);
    // Every BMP filename should be non-empty ASCII and end in .BMP
    // (case-sensitive in the corpus).
    for (auto const& r : td->records)
    {
      REQUIRE_FALSE(r.bitmap_name.empty());
      REQUIRE(r.bitmap_name.find(".BMP") != std::string::npos);
    }
    // 128-byte reserved region is always all-zero in the corpus.
    for (auto const& r : td->records)
    {
      for (auto b : r.reserved_block)
      {
        REQUIRE(b == 0);
      }
    }
    // corner_type_tags entries must be within the type table.
    for (auto const& r : td->records)
    {
      for (auto tag : r.corner_type_tags)
      {
        REQUIRE(tag <= w.num_types);  // 0..numTypes inclusive (tag 0 = "no material")
      }
    }
    ++hit;
  }
  if (hit == 0)
  {
    WARN("no Terrain.dat samples present at /tmp/worlddats-*/ - run "
         "vol-list --extract on the six *Terrain.vol archives first");
  }
}

TEST_CASE("parse_grid_dat sweeps all six shipping worlds",
          "[terrain][grid][corpus]")
{
  std::size_t hit = 0;
  for (auto const& w : kWorlds)
  {
    const auto path = corpus_path(w.world, ".Grid.dat");
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
      WARN("sample not present: " << path);
      continue;
    }
    auto gd = parse_grid_dat(f);
    REQUIRE(gd.has_value());
    REQUIRE(gd->version == 3);
    REQUIRE(gd->num_base_types == w.num_types);
    REQUIRE(gd->num_base_textures == w.num_textures);
    const std::uint64_t b = w.num_types + 1ull;
    const std::uint64_t b4 = b * b * b * b;
    REQUIRE(gd->tex_combos.size() == b4);
    REQUIRE(gd->pick_offs.size() == b4 + 1);
    REQUIRE(gd->pick_offs.back() == gd->pick_list.size());
    ++hit;
  }
  if (hit == 0)
  {
    WARN("no Grid.dat samples present");
  }
}

TEST_CASE("parse_rules_dat sweeps all six shipping worlds",
          "[terrain][rules][corpus]")
{
  std::size_t hit = 0;
  for (auto const& w : kWorlds)
  {
    const auto path = corpus_path(w.world, ".Rules.dat");
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
      WARN("sample not present: " << path);
      continue;
    }
    auto rd = parse_rules_dat(f);
    REQUIRE(rd.has_value());
    REQUIRE(rd->rules.size() == w.num_rules);
    for (auto const& r : rd->rules)
    {
      // All numeric values should be finite (no NaN / infinity)
      // in shipping content.
      REQUIRE(std::isfinite(r.alt_min));
      REQUIRE(std::isfinite(r.alt_max));
      REQUIRE(std::isfinite(r.alt_mean));
      REQUIRE(std::isfinite(r.alt_sdev));
      REQUIRE(std::isfinite(r.alt_weight));
      REQUIRE(std::isfinite(r.slope_min));
      REQUIRE(std::isfinite(r.slope_max));
      REQUIRE(std::isfinite(r.slope_mean));
      REQUIRE(std::isfinite(r.slope_sdev));
      REQUIRE(std::isfinite(r.slope_weight));
    }
    ++hit;
  }
  if (hit == 0)
  {
    WARN("no Rules.dat samples present");
  }
}

TEST_CASE("lush.Rules.dat rule 0 matches the spec's worked example",
          "[terrain][rules][corpus]")
{
  std::ifstream f("/tmp/worlddats-lush/lush.Rules.dat", std::ios::binary);
  if (!f)
  {
    WARN("lush.Rules.dat not extracted");
    return;
  }
  auto rd = parse_rules_dat(f);
  REQUIRE(rd.has_value());
  REQUIRE(rd->rules.size() == 6);
  auto const& r = rd->rules[0];
  REQUIRE(r.group_num == 0);
  REQUIRE(r.alt_min == 50.0f);
  REQUIRE(r.alt_max == 350.0f);
  REQUIRE(r.alt_mean == 150.0f);
  REQUIRE(r.alt_sdev == 0.5f);
  REQUIRE(r.alt_weight == 0.5f);
  REQUIRE(r.adj_heights == 0);
  REQUIRE(r.slope_min == 0.0f);
  REQUIRE(r.slope_max == 8.0f);
  REQUIRE(r.slope_mean == 1.5f);
  REQUIRE(r.slope_sdev == 0.5f);
  REQUIRE(r.slope_weight == 0.5f);
  REQUIRE(r.adj_slopes == 0);
}

TEST_CASE("lush.Terrain.dat record 0 matches the spec's worked example",
          "[terrain][dat][corpus]")
{
  std::ifstream f("/tmp/worlddats-lush/lush.Terrain.dat", std::ios::binary);
  if (!f)
  {
    WARN("lush.Terrain.dat not extracted");
    return;
  }
  auto td = parse_terrain_dat(f);
  REQUIRE(td.has_value());
  REQUIRE_FALSE(td->records.empty());
  auto const& r = td->records.front();
  REQUIRE(r.bitmap_name == "lCCCC.BMP");
  REQUIRE(r.corner_type_tags == std::array<std::uint8_t, 4>{2, 2, 2, 2});
  REQUIRE(r.sides == 0x000014ff);
  REQUIRE(r.classifier_word == 0x0000000d);
  REQUIRE(r.elasticity == 0.5f);
  REQUIRE(r.friction == 1.0f);
}
