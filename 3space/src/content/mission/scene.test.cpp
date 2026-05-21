// Smoke tests for the build_scene() scene-graph builder.
//
// Two layers:
//   1. In-memory tests with hand-crafted mis_file ASTs.
//   2. Real-file checks against the .mis files in
//      /Users/v/code/tribes-emscripten/tribes-game/base/missions/.
//      Skipped (with WARN) when the directory is absent.

#include <catch2/catch.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "content/mission/mis.hpp"
#include "content/mission/scene.hpp"

using namespace studio::content::mission;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static mis_file parse_str(const std::string& s)
{
  std::istringstream ss(s);
  return read_mis_file(ss);
}

// ---------------------------------------------------------------------------
// In-memory smoke tests
// ---------------------------------------------------------------------------

TEST_CASE("build_scene: monostate for SimGroup root", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" { };\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  REQUIRE(sg.root.class_name == "SimGroup");
  REQUIRE(std::holds_alternative<std::monostate>(sg.root.payload));
  REQUIRE(sg.terrain  == nullptr);
  REQUIRE(sg.sky      == nullptr);
  REQUIRE(sg.palette  == nullptr);
  REQUIRE(sg.center   == nullptr);
  REQUIRE(sg.volumes_in_order.empty());
}

TEST_CASE("build_scene: SimVolume collected in DFS order", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant SimGroup \"Volumes\" {\n"
    "        instant SimVolume \"V1\" { fileName = \"a.vol\"; };\n"
    "        instant SimVolume \"V2\" { fileName = \"b.vol\"; };\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  REQUIRE(sg.volumes_in_order.size() == 2);
  REQUIRE(sg.volumes_in_order[0].file_name == "a.vol");
  REQUIRE(sg.volumes_in_order[1].file_name == "b.vol");
}

TEST_CASE("build_scene: SimTerrain pointer and fields", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant SimTerrain \"Terrain\" {\n"
    "        tedFileName = \"test.dtf\";\n"
    "        visibleDistance = \"700\";\n"
    "        hazeDistance = \"500\";\n"
    "        hazeVerticalMin = \"1.0\";\n"
    "        hazeVerticalMax = \"2.0\";\n"
    "        contGravity = \"0 0 -20\";\n"
    "        contDrag = \"0.5\";\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  REQUIRE(sg.terrain != nullptr);
  REQUIRE(sg.terrain->ted_filename == "test.dtf");
  REQUIRE(sg.terrain->gravity == Approx(-20.0f));
  REQUIRE(sg.terrain->drag    == Approx(0.5f));
  REQUIRE(sg.terrain->fog[0]  == Approx(700.0f));
  REQUIRE(sg.terrain->fog[1]  == Approx(500.0f));
  REQUIRE(sg.terrain->fog[2]  == Approx(1.0f));
  REQUIRE(sg.terrain->fog[3]  == Approx(2.0f));
}

TEST_CASE("build_scene: Sky pointer and dmlName", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant Sky \"Sky\" {\n"
    "        dmlName = \"litesky.dml\";\n"
    "        textures[0] = \"3\";\n"
    "        textures[15] = \"7\";\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  REQUIRE(sg.sky != nullptr);
  REQUIRE(sg.sky->dml_name == "litesky.dml");
  REQUIRE(sg.sky->textures[0]  == 3);
  REQUIRE(sg.sky->textures[15] == 7);
}

TEST_CASE("build_scene: SimPalette pointer", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant SimPalette \"Palette\" { fileName = \"lush.day.ppl\"; };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  REQUIRE(sg.palette != nullptr);
  REQUIRE(sg.palette->ppl_filename == "lush.day.ppl");
}

TEST_CASE("build_scene: MissionCenterPos pointer and fields", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant MissionCenterPos \"MissionCenter\" {\n"
    "        x = \"-150\"; y = \"-250\"; w = \"300\"; h = \"300\";\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  REQUIRE(sg.center != nullptr);
  REQUIRE(sg.center->x == Approx(-150.0f));
  REQUIRE(sg.center->y == Approx(-250.0f));
  REQUIRE(sg.center->w == Approx(300.0f));
  REQUIRE(sg.center->h == Approx(300.0f));
}

TEST_CASE("build_scene: StaticShape typed payload", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant StaticShape \"Gen1\" {\n"
    "        dataBlock = \"Generator\";\n"
    "        position = \"10 20 30\";\n"
    "        rotation = \"0 0 1.57\";\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  REQUIRE(sg.root.children.size() == 1);
  const auto& node = sg.root.children[0];
  REQUIRE(std::holds_alternative<node_static_shape>(node.payload));
  const auto& ss = std::get<node_static_shape>(node.payload);
  REQUIRE(ss.data_block.name == "Generator");
  REQUIRE(ss.xf.position[0] == Approx(10.0f));
  REQUIRE(ss.xf.position[2] == Approx(30.0f));
  REQUIRE(ss.xf.rotation[2] == Approx(1.57f).epsilon(0.001f));
  REQUIRE(ss.xf.rotation[3] == Approx(1.0f));  // identity w
}

TEST_CASE("build_scene: InteriorShape typed payload", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant InteriorShape \"bunker1\" {\n"
    "        fileName = \"bunker2.0.dis\";\n"
    "        lightParams = \"0 \";\n"
    "        position = \"1 2 3\";\n"
    "        rotation = \"0 0 0\";\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  REQUIRE(sg.root.children.size() == 1);
  const auto& node = sg.root.children[0];
  REQUIRE(std::holds_alternative<node_interior>(node.payload));
  const auto& ni = std::get<node_interior>(node.payload);
  REQUIRE(ni.shape_name   == "bunker2.0.dis");
  REQUIRE(ni.light_params == "0 ");
  REQUIRE(node.extra_properties.empty());
}

TEST_CASE("build_scene: Moveable typed payload", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant Moveable \"elev1\" {\n"
    "        dataBlock = \"elevator6x5\";\n"
    "        position = \"0 0 0\";\n"
    "        rotation = \"0 0 0\";\n"
    "        Status = \"up\";\n"
    "        delayTime = \"612.314\";\n"
    "        closeTime = \"5.0\";\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  const auto& node = sg.root.children[0];
  REQUIRE(std::holds_alternative<node_moveable>(node.payload));
  const auto& nm = std::get<node_moveable>(node.payload);
  REQUIRE(nm.status          == "up");
  REQUIRE(nm.delay_time      == Approx(612.314f).epsilon(0.001f));
  REQUIRE(nm.close_time      == Approx(5.0f));
  REQUIRE(nm.data_block.name == "elevator6x5");
  REQUIRE(node.extra_properties.empty());
}

TEST_CASE("build_scene: SimPath typed payload", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant SimPath \"Path1\" {\n"
    "        isLooping = \"False\";\n"
    "        isCompressed = \"False\";\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  const auto& node = sg.root.children[0];
  REQUIRE(std::holds_alternative<node_path>(node.payload));
  const auto& np = std::get<node_path>(node.payload);
  REQUIRE(np.is_looping    == false);
  REQUIRE(np.is_compressed == false);
  REQUIRE(node.extra_properties.empty());
}

TEST_CASE("build_scene: Trigger typed payload", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant Trigger \"T1\" {\n"
    "        dataBlock = \"GroupTrigger\";\n"
    "        position = \"0 0 0\";\n"
    "        rotation = \"0 0 0\";\n"
    "        boundingBox = \"-12.5 -15 -12.5 12.5 15 12.5\";\n"
    "        isSphere = \"True\";\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  const auto& node = sg.root.children[0];
  REQUIRE(std::holds_alternative<node_trigger>(node.payload));
  const auto& nt = std::get<node_trigger>(node.payload);
  REQUIRE(nt.is_sphere        == true);
  REQUIRE(nt.bounding_box[0]  == Approx(-12.5f));
  REQUIRE(nt.bounding_box[5]  == Approx(12.5f));
  REQUIRE(node.extra_properties.empty());
}

TEST_CASE("build_scene: SimLight typed payload", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant SimLight {\n"
    "        type = \"2\";\n"
    "        range = \"3\";\n"
    "        color = \"1 0 0\";\n"
    "        position = \"10 20 30\";\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  const auto& node = sg.root.children[0];
  REQUIRE(std::holds_alternative<node_sim_light>(node.payload));
  const auto& nl = std::get<node_sim_light>(node.payload);
  REQUIRE(nl.type      == 2);
  REQUIRE(nl.radius    == Approx(3.0f));
  REQUIRE(nl.color[0]  == Approx(1.0f));
  REQUIRE(nl.color[1]  == Approx(0.0f));
}

TEST_CASE("build_scene: Snowfall typed payload", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant Snowfall \"Snow1\" {\n"
    "        intensity = \"1\";\n"
    "        wind = \"1 2 -35\";\n"
    "        rain = \"False\";\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  const auto& node = sg.root.children[0];
  REQUIRE(std::holds_alternative<node_snowfall>(node.payload));
  const auto& ns = std::get<node_snowfall>(node.payload);
  REQUIRE(ns.intensity    == Approx(1.0f));
  REQUIRE(ns.wind[2]      == Approx(-35.0f));
  REQUIRE(ns.is_rain      == false);
  REQUIRE(node.extra_properties.empty());
}

TEST_CASE("build_scene: StarField typed payload", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant StarField \"stars1\" {\n"
    "        inFrontOfSky = \"True\";\n"
    "        colors[0] = \"1 1 1\";\n"
    "        colors[1] = \"0.5 0.5 0.5\";\n"
    "        colors[2] = \"0.25 0.25 0.25\";\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  const auto& node = sg.root.children[0];
  REQUIRE(std::holds_alternative<node_star_field>(node.payload));
  const auto& nsf = std::get<node_star_field>(node.payload);
  REQUIRE(nsf.in_front_of_sky     == true);
  REQUIRE(nsf.colors[0][0]        == Approx(1.0f));
  REQUIRE(nsf.colors[1][0]        == Approx(0.5f));
  REQUIRE(nsf.colors[2][0]        == Approx(0.25f));
  REQUIRE(node.extra_properties.empty());
}

TEST_CASE("build_scene: TeamGroup typed payload", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant TeamGroup \"team0\" { };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  const auto& node = sg.root.children[0];
  REQUIRE(std::holds_alternative<node_team_group>(node.payload));
  REQUIRE(std::get<node_team_group>(node.payload).name == "team0");
}

TEST_CASE("build_scene: unknown class preserved as monostate with extra_properties", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant UnknownThing \"Foo\" {\n"
    "        someKey = \"someValue\";\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  const auto& node = sg.root.children[0];
  REQUIRE(std::holds_alternative<std::monostate>(node.payload));
  REQUIRE(node.extra_properties.size() == 1);
  REQUIRE(node.extra_properties[0].key   == "someKey");
  REQUIRE(node.extra_properties[0].value == "someValue");
}

TEST_CASE("build_scene: consumed props absent from extra_properties", "[scene]")
{
  // StaticShape consumes position, rotation, dataBlock.
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant StaticShape \"Obj\" {\n"
    "        dataBlock = \"Foo\";\n"
    "        position = \"0 0 0\";\n"
    "        rotation = \"0 0 0\";\n"
    "        destroyable = \"True\";\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto sg = build_scene(parse_str(src));
  const auto& node = sg.root.children[0];
  REQUIRE(std::holds_alternative<node_static_shape>(node.payload));
  // Only unconsumed "destroyable" should remain.
  REQUIRE(node.extra_properties.size() == 1);
  REQUIRE(node.extra_properties[0].key == "destroyable");
}

TEST_CASE("build_scene: trailer propagated", "[scene]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" { };\n"
    "//--- export object end ---//\n"
    "exec(Training_welcome);\n"
    "$Game::missionType = \"CTF\";\n"
    "$teamScoreLimit = 5;\n"
    "$cdTrack = 4;\n"
    "$cdPlayMode = 1;\n";
  auto sg = build_scene(parse_str(src));
  REQUIRE(sg.trailer.exec_idents.size() == 1);
  REQUIRE(sg.trailer.exec_idents[0] == "Training_welcome");
  REQUIRE(sg.trailer.game_mission_type.has_value());
  REQUIRE(*sg.trailer.game_mission_type == "CTF");
  REQUIRE(*sg.trailer.team_score_limit  == 5);
  REQUIRE(*sg.trailer.cd_track          == 4);
}

// ---------------------------------------------------------------------------
// Real-file corpus
// ---------------------------------------------------------------------------

namespace
{
  const std::filesystem::path kMissionsDir =
    "/Users/v/code/tribes-emscripten/tribes-game/base/missions";

  std::vector<std::filesystem::path> collect_mis_files()
  {
    std::vector<std::filesystem::path> out;
    if (!std::filesystem::is_directory(kMissionsDir)) return out;
    for (const auto& e : std::filesystem::directory_iterator(kMissionsDir))
    {
      if (!e.is_regular_file()) continue;
      if (e.path().extension() == ".mis") out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
  }
}

TEST_CASE("build_scene: 1_Welcome.mis singletons and volume count", "[scene][corpus]")
{
  const auto path = kMissionsDir / "1_Welcome.mis";
  if (!std::filesystem::is_regular_file(path))
  {
    WARN("1_Welcome.mis not found — skipping");
    return;
  }
  std::ifstream f(path);
  REQUIRE(f.is_open());
  auto mf = read_mis_file(f);
  auto sg  = build_scene(mf);

  REQUIRE(sg.volumes_in_order.size() >= 5);
  REQUIRE(sg.terrain  != nullptr);
  REQUIRE(sg.sky      != nullptr);
  REQUIRE(sg.palette  != nullptr);
  REQUIRE(sg.center   != nullptr);
}

TEST_CASE("build_scene: all real .mis files build without throwing", "[scene][corpus]")
{
  auto files = collect_mis_files();
  if (files.empty())
  {
    WARN("no .mis files found in " + kMissionsDir.string() + " — corpus skipped");
    return;
  }

  std::size_t ok = 0;
  std::vector<std::string> failures;

  for (const auto& p : files)
  {
    std::ifstream f(p);
    if (!f)
    {
      failures.push_back(p.filename().string() + " (open failed)");
      continue;
    }
    try
    {
      auto mf = read_mis_file(f);
      auto sg  = build_scene(mf);
      (void)sg;
      ++ok;
    }
    catch (const std::exception& e)
    {
      failures.push_back(p.filename().string() + " (" + e.what() + ")");
    }
  }

  INFO("scene corpus: " << ok << "/" << files.size() << " built cleanly");
  if (!failures.empty())
  {
    std::string msg = "build_scene failures: ";
    for (std::size_t i = 0; i < failures.size() && i < 10; ++i)
    {
      if (i) msg += "; ";
      msg += failures[i];
    }
    FAIL(msg);
  }
  REQUIRE(ok == files.size());
}
