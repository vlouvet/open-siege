// Unit tests for the DTB (GBLK chunk) parser.
//
// Two layers of coverage:
//   1. In-memory smoke tests with synthesised buffers (run without the
//      Tribes asset corpus).
//   2. Real-file checks against DTB samples extracted from all 45
//      per-mission `.ted` PVOL archives shipped with Tribes 1.41.
//      Skipped (with WARN) when /tmp/dtb-samples is empty.
//
// Validation spec: docs/clean-room-specs/TERRAIN.md §2 and §9.2.

#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "content/terrain/dtb.hpp"

using studio::content::terrain::grid_block;
using studio::content::terrain::is_darkstar_dtb;
using studio::content::terrain::parse_dtb;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

namespace
{
  std::size_t for_each_dtb(
    const std::filesystem::path& root,
    const std::function<void(const std::filesystem::path&)>& visit)
  {
    if (!std::filesystem::is_directory(root)) return 0;
    std::size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root))
    {
      if (!entry.is_regular_file()) continue;
      if (entry.path().extension() != ".dtb") continue;
      visit(entry.path());
      ++n;
    }
    return n;
  }
} // anonymous namespace

// ---------------------------------------------------------------------------
// magic detection
// ---------------------------------------------------------------------------

TEST_CASE("is_darkstar_dtb detects GBLK magic", "[dtb]")
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  ss.write("GBLK\x00\x00\x00\x00", 8);
  ss.seekg(0);
  REQUIRE(is_darkstar_dtb(ss) == true);
  // Stream position must be restored.
  REQUIRE(ss.tellg() == std::streampos(0));
}

TEST_CASE("is_darkstar_dtb rejects non-DTB magic", "[dtb]")
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  ss.write("GFIL\x00\x00\x00\x00", 8);
  ss.seekg(0);
  REQUIRE(is_darkstar_dtb(ss) == false);
  REQUIRE(ss.tellg() == std::streampos(0));
}

// ---------------------------------------------------------------------------
// parse_dtb: null / bad-magic
// ---------------------------------------------------------------------------

TEST_CASE("parse_dtb returns nullopt for wrong magic", "[dtb]")
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  ss.write("PERS....", 8);
  ss.seekg(0);
  REQUIRE_FALSE(parse_dtb(ss).has_value());
}

// ---------------------------------------------------------------------------
// Real-file corpus sweep (skipped if samples not present)
// ---------------------------------------------------------------------------

TEST_CASE("parse_dtb: 1_Welcome#0.dtb matches spec's worked example",
          "[dtb][corpus]")
{
  std::ifstream f("/tmp/dtb-samples/1_Welcome#0.dtb", std::ios::binary);
  if (!f)
  {
    WARN("1_Welcome#0.dtb not present — run vol-list --extract on "
         "missions/*.ted first");
    return;
  }

  // T-DTB-1: basic header fields.
  auto blk = parse_dtb(f);
  REQUIRE(blk.has_value());
  REQUIRE(blk->version == 5);
  REQUIRE(blk->name == "block-0");
  REQUIRE(blk->detail_count == 9);
  REQUIRE(blk->light_scale == 0);
  REQUIRE(blk->size[0] == 256);
  REQUIRE(blk->size[1] == 256);

  // T-DTB-1: heightRange from the spec's worked example.
  REQUIRE(blk->height_min == 52.358238f);
  REQUIRE(blk->height_max == 193.659775f);

  // T-DTB-2: heightmap vertex count.
  REQUIRE(blk->heights.size() == 257u * 257u);

  // materialmap and lightmap dimensions.
  REQUIRE(blk->materials.size() == 256u * 256u);
  REQUIRE(blk->lightmap_dim == 257);
  REQUIRE(blk->lightmap.size() == 257u * 257u);

  // T-DTB-3: decoded heightmap min/max must match the header range.
  float hmin = std::numeric_limits<float>::max();
  float hmax = std::numeric_limits<float>::lowest();
  for (float h : blk->heights)
  {
    REQUIRE(std::isfinite(h));
    hmin = std::min(hmin, h);
    hmax = std::max(hmax, h);
  }
  REQUIRE(hmin == blk->height_min);
  REQUIRE(hmax == blk->height_max);

  // HRLM trailer: 20 bytes, all zero except hrlmVersion (offset 0) = 3.
  REQUIRE(blk->hrlm_raw.size() == 20u);
  // Bytes 0..3 = hrlmVersion = 3 in LE.
  REQUIRE(static_cast<uint8_t>(blk->hrlm_raw[0]) == 3);
  REQUIRE(static_cast<uint8_t>(blk->hrlm_raw[1]) == 0);
  REQUIRE(static_cast<uint8_t>(blk->hrlm_raw[2]) == 0);
  REQUIRE(static_cast<uint8_t>(blk->hrlm_raw[3]) == 0);
  // Remaining 16 bytes must all be zero.
  for (std::size_t i = 4; i < 20; ++i)
  {
    REQUIRE(blk->hrlm_raw[i] == std::byte{0});
  }

  // T-DTB-6: stream at end of chunk (tellg == file_size after parse).
  REQUIRE(f.peek() == std::char_traits<char>::eof());
}

TEST_CASE("parse_dtb: 3_Vehicle#0.dtb height range cross-check", "[dtb][corpus]")
{
  std::ifstream f("/tmp/dtb-samples/3_Vehicle#0.dtb", std::ios::binary);
  if (!f)
  {
    WARN("3_Vehicle#0.dtb not present");
    return;
  }
  auto blk = parse_dtb(f);
  REQUIRE(blk.has_value());
  REQUIRE(blk->height_min == Approx(100.0f));
  REQUIRE(blk->height_max == Approx(286.2694f));

  float hmin = std::numeric_limits<float>::max();
  float hmax = std::numeric_limits<float>::lowest();
  for (float h : blk->heights)
  {
    hmin = std::min(hmin, h);
    hmax = std::max(hmax, h);
  }
  REQUIRE(hmin == blk->height_min);
  REQUIRE(hmax == blk->height_max);
}

TEST_CASE("parse_dtb: 5_CTF#0.dtb height range cross-check", "[dtb][corpus]")
{
  std::ifstream f("/tmp/dtb-samples/5_CTF#0.dtb", std::ios::binary);
  if (!f)
  {
    WARN("5_CTF#0.dtb not present");
    return;
  }
  auto blk = parse_dtb(f);
  REQUIRE(blk.has_value());
  REQUIRE(blk->height_min == Approx(88.99797f));
  REQUIRE(blk->height_max == Approx(253.68361f));

  float hmin = std::numeric_limits<float>::max();
  float hmax = std::numeric_limits<float>::lowest();
  for (float h : blk->heights)
  {
    hmin = std::min(hmin, h);
    hmax = std::max(hmax, h);
  }
  REQUIRE(hmin == blk->height_min);
  REQUIRE(hmax == blk->height_max);
}

TEST_CASE("parse_dtb sweeps the full /tmp/dtb-samples corpus", "[dtb][corpus]")
{
  std::size_t ok = 0;
  std::size_t fail = 0;
  std::vector<std::string> failures;
  std::size_t total_bytes_decompressed = 0;

  std::size_t total = for_each_dtb(
    "/tmp/dtb-samples",
    [&](const std::filesystem::path& p) {
      std::ifstream f(p, std::ios::binary);
      if (!f)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (open)");
        return;
      }

      auto blk = parse_dtb(f);
      if (!blk)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (parse)");
        return;
      }

      // Structural sanity checks.
      bool good = true;

      // version
      good = good && (blk->version == 5);

      // name is non-empty
      good = good && !blk->name.empty();

      // grid dimensions
      good = good && (blk->size[0] == 256 && blk->size[1] == 256);

      // heights array size
      const std::size_t expected_verts = 257u * 257u;
      const std::size_t expected_quads = 256u * 256u;
      good = good && (blk->heights.size() == expected_verts);

      // materials array size (quad-indexed: sx*sy, not vertex-indexed)
      good = good && (blk->materials.size() == expected_quads);

      // lightmap present with correct dimension
      good = good && (blk->lightmap_dim == 257);
      good = good && (blk->lightmap.size() == expected_verts);

      // All heights finite and within declared range.
      float hmin = std::numeric_limits<float>::max();
      float hmax = std::numeric_limits<float>::lowest();
      for (float h : blk->heights)
      {
        if (!std::isfinite(h)) { good = false; break; }
        hmin = std::min(hmin, h);
        hmax = std::max(hmax, h);
      }
      if (good)
      {
        good = good && (hmin == blk->height_min);
        good = good && (hmax == blk->height_max);
      }

      // HRLM trailer present.
      good = good && (blk->hrlm_raw.size() == 20u);

      if (good)
      {
        ++ok;
        // Accumulate decompressed byte count (3 layers per file).
        total_bytes_decompressed +=
          blk->heights.size() * sizeof(float)
          + blk->materials.size() * 2
          + blk->lightmap.size() * sizeof(std::uint16_t);
      }
      else
      {
        ++fail;
        failures.push_back(p.filename().string() + " (sanity)");
      }
    });

  if (total == 0)
  {
    WARN("no DTB samples present at /tmp/dtb-samples — corpus sweep skipped");
    return;
  }

  INFO("DTB corpus: parsed " << ok << "/" << total << " files cleanly; "
       << fail << " failed; "
       << total_bytes_decompressed << " total bytes decompressed");

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
