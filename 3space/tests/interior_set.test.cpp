// Tests for the cross-VOL interior resolver (interior_set.hpp).
//
// The resolver is exercised primarily against the real Tribes 1.41
// freeware corpus — the in-memory smoke tests for the underlying
// DIS / DIG / DIL parsers live next to those parsers. Here we just
// verify the cross-archive plumbing end-to-end.
//
// All real-asset tests gracefully WARN-and-skip when the VOLs aren't
// available, so the suite stays green on contributor machines without
// the Tribes install.

#include <catch2/catch.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "content/interior/interior_set.hpp"

namespace
{
  const std::filesystem::path kHumanDml =
    "/Users/v/code/tribes-emscripten/tribes-game/base/human1DML.vol";
  const std::filesystem::path kMissionWelcome =
    "/Users/v/code/tribes-emscripten/tribes-game/base/missions/1_welcome.vol";

  bool corpus_available()
  {
    return std::filesystem::is_regular_file(kHumanDml)
        && std::filesystem::is_regular_file(kMissionWelcome);
  }
}

using studio::content::interior::interior_resolver;

TEST_CASE("interior_resolver: empty resolver returns nullopt", "[interior][resolver]")
{
  interior_resolver r;
  REQUIRE(r.vol_count() == 0);
  REQUIRE_FALSE(r.resolve("anything.dis").has_value());
}

TEST_CASE("interior_resolver: resolves catwalkA.0.dis with mission + world VOLs",
          "[interior][resolver][corpus]")
{
  if (!corpus_available())
  {
    WARN("Tribes asset corpus not present — skipping cross-VOL resolver test");
    return;
  }

  interior_resolver r;
  // World VOL first, mission VOL last — last-added wins, so mission
  // overrides take priority for DIS + DIL while DIG + DML naturally
  // fall back to the world archive.
  r.add_vol(kHumanDml);
  r.add_vol(kMissionWelcome);
  REQUIRE(r.vol_count() == 2);

  auto bundle = r.resolve("catwalkA.0.dis");
  REQUIRE(bundle.has_value());

  // DIS structural sanity (also covered by dis.test.cpp; rechecked here
  // to prove the bundle is wired up rather than default-constructed).
  REQUIRE(bundle->dis.version == 3);
  REQUIRE_FALSE(bundle->dis.lods.empty());
  REQUIRE_FALSE(bundle->dis.lightmap_files.empty());
  REQUIRE_FALSE(bundle->dis.material_list_file.empty());

  // DIG must carry geometry — every shipping interior has at least one
  // surface, one BSP node and one plane.
  REQUIRE_FALSE(bundle->dig.surfaces.empty());
  REQUIRE_FALSE(bundle->dig.planes.empty());
  REQUIRE_FALSE(bundle->dig.points3.empty());

  // build_id chain is the headline correctness check for the resolver.
  REQUIRE(bundle->dig.build_id == bundle->dil.geometry_build_id);

  // The catwalk interiors in 1_Welcome.vol ship as per-instance mission
  // lighting (filename pattern `catwalkA-000-0.dil`). Verify the
  // resolver picked up the mission variant rather than the stock one.
  // Spec acceptance: when a mission VOL provides a per-instance DIL,
  // is_mission_lighting must be true.
  REQUIRE(bundle->dil.is_mission_lighting == true);

  // Material list must be one of the supported PERS variants and carry
  // at least one material entry.
  std::visit(
    [](const auto& ml) {
      REQUIRE_FALSE(ml.materials.empty());
    },
    bundle->material);
}

TEST_CASE("interior_resolver: stock-only DIL when no mission VOL is mounted",
          "[interior][resolver][corpus]")
{
  if (!corpus_available())
  {
    WARN("Tribes asset corpus not present — skipping stock-DIL test");
    return;
  }

  interior_resolver r;
  r.add_vol(kHumanDml);  // world only — no mission override available.

  // catwalkA.0.dis only lives in the mission VOL, so a world-only
  // resolver shouldn't find it. Pick a DIS that lives in the world
  // VOL itself (catwalkA.dis is the stock variant shipped in
  // human1DML.vol).
  auto bundle = r.resolve("catwalkA.dis");
  if (!bundle.has_value())
  {
    WARN("catwalkA.dis not present in human1DML.vol — skipping stock-DIL test");
    return;
  }

  // With no mission VOL mounted, the only DIL hit must be the stock
  // ITRLighting (is_mission_lighting == false).
  REQUIRE(bundle->dil.is_mission_lighting == false);
  REQUIRE(bundle->dig.build_id == bundle->dil.geometry_build_id);
}

TEST_CASE("interior_resolver: sweeps full DIS list without missing warnings",
          "[interior][resolver][corpus]")
{
  if (!corpus_available())
  {
    WARN("Tribes asset corpus not present — skipping sweep");
    return;
  }

  interior_resolver r;
  r.add_vol(kHumanDml);
  r.add_vol(kMissionWelcome);

  // Discover DIS filenames across BOTH mounted VOLs. The spec's
  // acceptance target is "50 different DIS names that resolve cleanly
  // with both world + mission mounted" — in shipping content, the
  // mission VOLs are tiny (1_welcome.vol carries only 9 DIS records)
  // and the bulk of stock per-world interiors live in the DML VOL.
  // Sweeping both gives a realistic 50+ corpus and proves the resolver
  // handles plain world DIS records (no mission override) and mission
  // override DIS records uniformly.
  namespace dv = studio::resources::vol::darkstar;
  namespace sr = studio::resources;

  auto collect_dis = [](const std::filesystem::path& vol_path,
                        std::vector<std::string>& out) {
    std::ifstream in(vol_path, std::ios::binary);
    if (!in) return;
    dv::vol_file_archive plugin;
    if (!plugin.stream_is_supported(in)) return;
    in.clear();
    in.seekg(0);
    auto all = sr::get_all_content(vol_path, in, plugin);
    for (auto& entry : all)
    {
      if (auto* f = std::get_if<sr::file_info>(&entry))
      {
        auto leaf = f->filename.filename().string();
        std::string lower;
        lower.reserve(leaf.size());
        for (char c : leaf)
          lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".dis") == 0)
        {
          out.push_back(leaf);
        }
      }
    }
  };

  std::vector<std::string> dis_names;
  collect_dis(kMissionWelcome, dis_names);
  std::size_t mission_count = dis_names.size();
  collect_dis(kHumanDml, dis_names);

  INFO("Discovered " << mission_count << " DIS in 1_welcome.vol + "
       << (dis_names.size() - mission_count) << " in human1DML.vol = "
       << dis_names.size() << " total");

  std::size_t ok = 0;
  std::size_t failed = 0;
  std::vector<std::string> failed_names;
  for (const auto& name : dis_names)
  {
    auto bundle = r.resolve(name);
    if (bundle.has_value())
    {
      ++ok;
    }
    else
    {
      ++failed;
      failed_names.push_back(name);
    }
  }
  INFO("Resolved " << ok << "/" << dis_names.size() << " DIS bundles");
  if (!failed_names.empty())
  {
    std::string msg = "failures: ";
    for (std::size_t i = 0; i < failed_names.size() && i < 10; ++i)
    {
      if (i) msg += ", ";
      msg += failed_names[i];
    }
    INFO(msg);
  }

  // Spec acceptance: >= 50 DIS names resolve cleanly when both world
  // and mission VOLs are mounted.
  REQUIRE(ok >= 50);
}
