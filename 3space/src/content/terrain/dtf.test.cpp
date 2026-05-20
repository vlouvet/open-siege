// Unit tests for the DTF (GFIL chunk) parser.
//
// Two layers of coverage:
//   1. In-memory smoke tests with synthesised buffers, so the suite
//      runs on contributor machines without the Tribes asset corpus.
//   2. Real-file checks against samples extracted from the 45
//      per-mission `.ted` PVOL archives. Skipped (with WARN) when
//      /tmp/dtf-samples is empty.

#include <catch2/catch.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "content/terrain/dtf.hpp"

namespace
{
  void put_u32(std::vector<unsigned char>& buf, std::uint32_t v)
  {
    buf.push_back(static_cast<unsigned char>(v & 0xff));
    buf.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    buf.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
    buf.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
  }

  void put_i32(std::vector<unsigned char>& buf, std::int32_t v)
  {
    put_u32(buf, static_cast<std::uint32_t>(v));
  }

  void put_f32(std::vector<unsigned char>& buf, float v)
  {
    std::uint32_t u;
    static_assert(sizeof(float) == 4, "");
    std::memcpy(&u, &v, 4);
    put_u32(buf, u);
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

  // Hand-crafted minimal DTF: 3x3 grid, blockPattern=0, all cells
  // mapping to block id 0, one trailing block-list entry with an
  // empty name. Matches the layout of the shipping 1_Welcome.dtf
  // sample.
  std::vector<unsigned char> build_minimal_dtf(const std::string& dml_name = "lush.dml",
                                               float h_min = 52.358238f,
                                               float h_max = 193.659775f)
  {
    std::vector<unsigned char> payload;
    put_u32(payload, 1);                                            // version
    put_u32(payload, static_cast<std::uint32_t>(dml_name.size()));  // nameLen
    put_bytes(payload, dml_name.data(), dml_name.size());           // dml_name
    put_i32(payload, 1);                                            // lastBlockId
    put_i32(payload, 9);                                            // detailCount
    put_i32(payload, 3);                                            // scale
    for (int i = 0; i < 24; ++i) payload.push_back(0);              // bounds
    put_i32(payload, 0);                                            // origin.x
    put_i32(payload, 0);                                            // origin.y
    put_f32(payload, h_min);                                        // height_min
    put_f32(payload, h_max);                                        // height_max
    put_i32(payload, 3);                                            // size.x
    put_i32(payload, 3);                                            // size.y
    put_u32(payload, 0);                                            // blockPattern
    for (int i = 0; i < 9; ++i) put_i32(payload, 0);                // blockMap
    put_i32(payload, 1);                                            // blockListCount
    put_i32(payload, 0);                                            // entry[0].id
    put_u32(payload, 0);                                            // entry[0].nameLen

    std::vector<unsigned char> buf;
    const char magic[4] = {'G','F','I','L'};
    put_bytes(buf, magic, 4);
    put_u32(buf, static_cast<std::uint32_t>(payload.size()));
    put_bytes(buf, payload.data(), payload.size());
    return buf;
  }
}

using studio::content::terrain::parse_dtf;
using studio::content::terrain::is_darkstar_dtf;

TEST_CASE("is_darkstar_dtf detects GFIL magic", "[dtf]")
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  ss.write("GFIL\x00\x00\x00\x00", 8);
  ss.seekg(0);
  REQUIRE(is_darkstar_dtf(ss) == true);
  REQUIRE(ss.tellg() == std::streampos(0));
}

TEST_CASE("is_darkstar_dtf rejects non-DTF magic", "[dtf]")
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  ss.write("PERS\x00\x00\x00\x00", 8);
  ss.seekg(0);
  REQUIRE(is_darkstar_dtf(ss) == false);
  REQUIRE(ss.tellg() == std::streampos(0));
}

TEST_CASE("parse_dtf returns nullopt when the magic is wrong", "[dtf]")
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  ss.write("PERS....", 8);
  ss.seekg(0);
  REQUIRE_FALSE(parse_dtf(ss).has_value());
}

TEST_CASE("parse_dtf reads a hand-crafted GFIL chunk", "[dtf]")
{
  auto buf = build_minimal_dtf();
  auto ss = make_stream(buf);

  auto d = parse_dtf(ss);
  REQUIRE(d.has_value());
  REQUIRE(d->version == 1);
  REQUIRE(d->material_list_name == "lush.dml");
  REQUIRE(d->last_block_id == 1);
  REQUIRE(d->detail_count == 9);
  REQUIRE(d->scale == 3);
  REQUIRE(d->origin[0] == 0);
  REQUIRE(d->origin[1] == 0);
  REQUIRE(d->height_min == Approx(52.358238f));
  REQUIRE(d->height_max == Approx(193.659775f));
  REQUIRE(d->size[0] == 3);
  REQUIRE(d->size[1] == 3);
  REQUIRE(d->block_pattern == 0);
  REQUIRE(d->block_map.size() == 9);
  for (auto const& cell : d->block_map)
  {
    REQUIRE(cell.block_id == 0);
  }
  REQUIRE(d->block_list.size() == 1);
  REQUIRE(d->block_list[0].block_id == 0);
  REQUIRE(d->block_list[0].name.empty());

  // bounds_raw should be all-zero per the spec's worked sample.
  for (auto b : d->bounds_raw)
  {
    REQUIRE(b == std::byte{0});
  }
}

TEST_CASE("parse_dtf rejects unsupported version", "[dtf]")
{
  auto buf = build_minimal_dtf();
  // Replace the version u32 (first 4 bytes of payload, file offset 8)
  // with 2.
  buf[8] = 2;
  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_dtf(ss).has_value());
}

TEST_CASE("parse_dtf rejects oversized size axes", "[dtf]")
{
  std::vector<unsigned char> payload;
  put_u32(payload, 1);
  put_u32(payload, 8);
  const char dml[8] = {'l','u','s','h','.','d','m','l'};
  put_bytes(payload, dml, 8);
  put_i32(payload, 1);
  put_i32(payload, 9);
  put_i32(payload, 3);
  for (int i = 0; i < 24; ++i) payload.push_back(0);
  put_i32(payload, 0);
  put_i32(payload, 0);
  put_f32(payload, 0.0f);
  put_f32(payload, 100.0f);
  put_i32(payload, 999);  // oversized size.x
  put_i32(payload, 999);  // oversized size.y
  // No need to populate further; parse should reject before reading
  // the cell grid.
  std::vector<unsigned char> buf;
  const char magic[4] = {'G','F','I','L'};
  put_bytes(buf, magic, 4);
  put_u32(buf, static_cast<std::uint32_t>(payload.size()));
  put_bytes(buf, payload.data(), payload.size());

  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_dtf(ss).has_value());
}

TEST_CASE("parse_dtf rejects chunk-size mismatch", "[dtf]")
{
  auto buf = build_minimal_dtf();
  // Inflate the chunkPayloadSize at offset 4 by one byte; the parser
  // should refuse because the cursor doesn't land on the declared end.
  buf[4] += 1;
  auto ss = make_stream(buf);
  REQUIRE_FALSE(parse_dtf(ss).has_value());
}

namespace
{
  std::size_t for_each_dtf(const std::filesystem::path& root,
                           const std::function<void(const std::filesystem::path&)>& visit)
  {
    if (!std::filesystem::is_directory(root)) return 0;
    std::size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root))
    {
      if (!entry.is_regular_file()) continue;
      if (entry.path().extension() != ".dtf") continue;
      visit(entry.path());
      ++n;
    }
    return n;
  }
}

TEST_CASE("parse_dtf matches the spec's worked example for 1_Welcome.dtf",
          "[dtf][corpus]")
{
  std::ifstream f("/tmp/dtf-samples/1_Welcome.dtf", std::ios::binary);
  if (!f)
  {
    WARN("1_Welcome.dtf not present at /tmp/dtf-samples/ — run "
         "vol-list --extract on Entities.vol's mission .ted files first");
    return;
  }
  auto d = parse_dtf(f);
  REQUIRE(d.has_value());
  REQUIRE(d->version == 1);
  REQUIRE(d->material_list_name == "lush.dml");
  REQUIRE(d->last_block_id == 1);
  REQUIRE(d->detail_count == 9);
  REQUIRE(d->scale == 3);
  REQUIRE(d->size[0] == 3);
  REQUIRE(d->size[1] == 3);
  REQUIRE(d->block_pattern == 0);
  REQUIRE(d->block_map.size() == 9);
  for (auto const& cell : d->block_map)
  {
    REQUIRE(cell.block_id == 0);
  }
  // Heights must match the worked example to f32 bit precision.
  REQUIRE(d->height_min == 52.358238f);
  REQUIRE(d->height_max == 193.659775f);
}

TEST_CASE("parse_dtf matches the spec's worked example for 5_CTF.dtf",
          "[dtf][corpus]")
{
  std::ifstream f("/tmp/dtf-samples/5_CTF.dtf", std::ios::binary);
  if (!f)
  {
    WARN("5_CTF.dtf not present at /tmp/dtf-samples/");
    return;
  }
  auto d = parse_dtf(f);
  REQUIRE(d.has_value());
  REQUIRE(d->material_list_name == "desert.dml");
  REQUIRE(d->size[0] == 3);
  REQUIRE(d->size[1] == 3);
  REQUIRE(d->block_map.size() == 9);
  REQUIRE(std::isfinite(d->height_min));
  REQUIRE(std::isfinite(d->height_max));
  REQUIRE(d->height_min < d->height_max);
}

TEST_CASE("parse_dtf sweeps the full /tmp/dtf-samples corpus",
          "[dtf][corpus]")
{
  std::size_t ok = 0;
  std::size_t fail = 0;
  std::vector<std::string> failures;

  std::size_t total = for_each_dtf(
    "/tmp/dtf-samples",
    [&](const std::filesystem::path& p) {
      std::ifstream f(p, std::ios::binary);
      if (!f)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (open)");
        return;
      }
      auto d = parse_dtf(f);
      if (!d)
      {
        ++fail;
        failures.push_back(p.filename().string() + " (parse)");
        return;
      }
      // Structural sanity for the shipping corpus: every DTF is
      // version 1, references a .dml, uses a 3x3 grid mapped to one
      // common block, and has sensible heights.
      bool good = d->version == 1
                  && !d->material_list_name.empty()
                  && d->material_list_name.find(".dml") != std::string::npos
                  && d->size[0] == 3
                  && d->size[1] == 3
                  && d->block_map.size() == 9
                  && d->block_pattern == 0
                  && std::isfinite(d->height_min)
                  && std::isfinite(d->height_max)
                  && d->height_min < d->height_max
                  && !d->block_list.empty();
      if (good) ++ok;
      else
      {
        ++fail;
        failures.push_back(p.filename().string() + " (sanity)");
      }
    });

  if (total == 0)
  {
    WARN("no DTF samples present; corpus sweep skipped");
    return;
  }

  INFO("DTF corpus: parsed " << ok << "/" << total << " files cleanly; "
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
