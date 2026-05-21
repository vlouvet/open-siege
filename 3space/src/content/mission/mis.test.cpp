// Unit tests for the Tribes 1 .mis ConsoleScript parser.
//
// Two layers:
//   1. In-memory smoke tests with hand-crafted strings.
//   2. Real-file checks against the 42 .mis files in
//      /Users/v/code/tribes-emscripten/tribes-game/base/missions/.
//      Skipped (with WARN) when the directory is absent.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "content/mission/mis.hpp"

using namespace studio::content::mission;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static mis_file parse_str(const std::string& s)
{
  std::istringstream ss(s);
  return read_mis_file(ss);
}

static bool is_mis_str(const std::string& s)
{
  std::istringstream ss(s);
  return is_mis_file(ss);
}

// Minimal well-formed .mis with one SimGroup root and one child.
static const char* kMinimal =
  "//--- export object begin ---//\n"
  "instant SimGroup \"MissionGroup\" {\n"
  "    instant SimTerrain \"Terrain\" {\n"
  "        tedFileName = \"test.dtf\";\n"
  "    };\n"
  "};\n"
  "//--- export object end ---//\n"
  "exec(objectives);\n"
  "$Game::missionType = \"CTF\";\n"
  "$teamScoreLimit = 5;\n"
  "$cdTrack = 4;\n"
  "$cdPlayMode = 1;\n";

static const char* kCRLF =
  "//--- export object begin ---//\r\n"
  "instant SimGroup \"MissionGroup\" {\r\n"
  "    instant Sky \"Sky\" {\r\n"
  "        textures[0] = \"0\";\r\n"
  "        textures[1] = \"1\";\r\n"
  "    };\r\n"
  "};\r\n"
  "//--- export object end ---//\r\n";

// ---------------------------------------------------------------------------
// is_mis_file
// ---------------------------------------------------------------------------

TEST_CASE("is_mis_file detects begin magic", "[mis]")
{
  REQUIRE(is_mis_str("//--- export object begin ---//\n"));
  REQUIRE_FALSE(is_mis_str("// something else\n"));
  REQUIRE_FALSE(is_mis_str(""));
}

TEST_CASE("is_mis_file restores stream position", "[mis]")
{
  std::istringstream ss("//--- export object begin ---//\n");
  is_mis_file(ss);
  REQUIRE(ss.tellg() == std::streampos(0));
}

// ---------------------------------------------------------------------------
// Basic parsing
// ---------------------------------------------------------------------------

TEST_CASE("parse_mis: root object class and instance name", "[mis]")
{
  auto f = parse_str(kMinimal);
  REQUIRE(f.root.class_name == "SimGroup");
  REQUIRE(f.root.instance_name.has_value());
  REQUIRE(*f.root.instance_name == "MissionGroup");
  REQUIRE_FALSE(f.root.children.empty());
}

TEST_CASE("parse_mis: child objects are parsed", "[mis]")
{
  auto f = parse_str(kMinimal);
  REQUIRE(f.root.children.size() == 1);
  REQUIRE(f.root.children[0].class_name == "SimTerrain");
  REQUIRE(f.root.children[0].instance_name == "Terrain");
  REQUIRE(f.root.children[0].properties.size() == 1);
  REQUIRE(f.root.children[0].properties[0].key == "tedFileName");
  REQUIRE(f.root.children[0].properties[0].value == "test.dtf");
}

TEST_CASE("parse_mis: CRLF line endings handled", "[mis]")
{
  auto f = parse_str(kCRLF);
  REQUIRE(f.root.class_name == "SimGroup");
  REQUIRE(f.root.children.size() == 1);
  REQUIRE(f.root.children[0].class_name == "Sky");
}

// ---------------------------------------------------------------------------
// Array-index properties
// ---------------------------------------------------------------------------

TEST_CASE("parse_mis: array-index properties", "[mis]")
{
  auto f = parse_str(kCRLF);
  const auto& sky = f.root.children[0];
  REQUIRE(sky.properties.size() == 2);
  REQUIRE(sky.properties[0].key == "textures");
  REQUIRE(sky.properties[0].array_index.has_value());
  REQUIRE(*sky.properties[0].array_index == 0);
  REQUIRE(sky.properties[0].value == "0");

  REQUIRE(sky.properties[1].key == "textures");
  REQUIRE(sky.properties[1].array_index.has_value());
  REQUIRE(*sky.properties[1].array_index == 1);
  REQUIRE(sky.properties[1].value == "1");
}

// ---------------------------------------------------------------------------
// Trailer parsing
// ---------------------------------------------------------------------------

TEST_CASE("parse_mis: trailer fields", "[mis]")
{
  auto f = parse_str(kMinimal);
  REQUIRE(f.trailer.exec_idents.size() == 1);
  REQUIRE(f.trailer.exec_idents[0] == "objectives");
  REQUIRE(f.trailer.game_mission_type.has_value());
  REQUIRE(*f.trailer.game_mission_type == "CTF");
  REQUIRE(f.trailer.team_score_limit.has_value());
  REQUIRE(*f.trailer.team_score_limit == 5);
  REQUIRE(f.trailer.cd_track.has_value());
  REQUIRE(*f.trailer.cd_track == 4);
  REQUIRE(f.trailer.cd_play_mode.has_value());
  REQUIRE(*f.trailer.cd_play_mode == 1);
}

TEST_CASE("parse_mis: DM score limit", "[mis]")
{
  const char* dm =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" { };\n"
    "//--- export object end ---//\n"
    "exec(dm);\n"
    "$DMScoreLimit = 0;\n"
    "$Game::missionType = \"DM\";\n"
    "$cdTrack = 3;\n"
    "$cdPlayMode = 1;\n";
  auto f = parse_str(dm);
  REQUIRE(f.trailer.dm_score_limit.has_value());
  REQUIRE(*f.trailer.dm_score_limit == 0);
  REQUIRE(f.trailer.game_mission_type.has_value());
  REQUIRE(*f.trailer.game_mission_type == "DM");
}

// ---------------------------------------------------------------------------
// Empty-body objects
// ---------------------------------------------------------------------------

TEST_CASE("parse_mis: empty body instant (no braces)", "[mis]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant SimGroup \"Lights\";\n"
    "};\n"
    "//--- export object end ---//\n";
  auto f = parse_str(src);
  REQUIRE(f.root.children.size() == 1);
  REQUIRE(f.root.children[0].class_name == "SimGroup");
  REQUIRE(*f.root.children[0].instance_name == "Lights");
  REQUIRE(f.root.children[0].properties.empty());
  REQUIRE(f.root.children[0].children.empty());
}

// ---------------------------------------------------------------------------
// Case-insensitive class name (the `simGroup` edge case in 1_Welcome.mis)
// ---------------------------------------------------------------------------

TEST_CASE("parse_mis: lower-case class name does not error", "[mis]")
{
  const char* src =
    "//--- export object begin ---//\n"
    "instant SimGroup \"MissionGroup\" {\n"
    "    instant simGroup \"sensor\" {\n"
    "        instant Sensor \"S1\" {\n"
    "            dataBlock = \"PulseSensor\";\n"
    "        };\n"
    "    };\n"
    "};\n"
    "//--- export object end ---//\n";
  auto f = parse_str(src);
  REQUIRE(f.root.children.size() == 1);
  // class_name is stored verbatim (lower-case as found in file)
  REQUIRE(f.root.children[0].class_name == "simGroup");
  REQUIRE(f.root.children[0].children.size() == 1);
  REQUIRE(f.root.children[0].children[0].class_name == "Sensor");
}

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------

TEST_CASE("parse_float: plain numbers", "[mis][helpers]")
{
  REQUIRE(parse_float("1.5").has_value());
  REQUIRE(*parse_float("1.5") == Approx(1.5f));
  REQUIRE(*parse_float("0") == Approx(0.0f));
  REQUIRE(*parse_float("-20") == Approx(-20.0f));
}

TEST_CASE("parse_float: scientific notation", "[mis][helpers]")
{
  REQUIRE(parse_float("1.03844e-38").has_value());
  REQUIRE(parse_float("8.62918e-38").has_value());
  REQUIRE(*parse_float("1.03844e-38") == Approx(1.03844e-38f).epsilon(1e-6));
}

TEST_CASE("parse_float: -NAN literal", "[mis][helpers]")
{
  auto v = parse_float("-NAN");
  REQUIRE(v.has_value());
  REQUIRE(std::isnan(*v));
}

TEST_CASE("parse_float: NAN literal (case-insensitive)", "[mis][helpers]")
{
  REQUIRE(std::isnan(*parse_float("nan")));
  REQUIRE(std::isnan(*parse_float("NAN")));
  REQUIRE(std::isnan(*parse_float("-NAN")));
}

TEST_CASE("parse_float: invalid returns nullopt", "[mis][helpers]")
{
  REQUIRE_FALSE(parse_float("abc").has_value());
  REQUIRE_FALSE(parse_float("").has_value());
}

TEST_CASE("parse_bool: true/false/1/0 case-insensitive", "[mis][helpers]")
{
  REQUIRE(*parse_bool("True") == true);
  REQUIRE(*parse_bool("true") == true);
  REQUIRE(*parse_bool("FALSE") == false);
  REQUIRE(*parse_bool("1") == true);
  REQUIRE(*parse_bool("0") == false);
  REQUIRE_FALSE(parse_bool("yes").has_value());
}

TEST_CASE("parse_vec3: space-separated floats", "[mis][helpers]")
{
  auto v = parse_vec3("0 0 -20");
  REQUIRE(v.has_value());
  REQUIRE((*v)[0] == Approx(0.0f));
  REQUIRE((*v)[1] == Approx(0.0f));
  REQUIRE((*v)[2] == Approx(-20.0f));
}

TEST_CASE("parse_vec3: insufficient tokens returns nullopt", "[mis][helpers]")
{
  REQUIRE_FALSE(parse_vec3("0 0").has_value());
}

TEST_CASE("parse_vec6: six floats", "[mis][helpers]")
{
  auto v = parse_vec6("1 2 3 4 5 6");
  REQUIRE(v.has_value());
  REQUIRE((*v)[5] == Approx(6.0f));
}

// ---------------------------------------------------------------------------
// Real-file corpus sweep
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

TEST_CASE("parse_mis: is_mis_file on real files", "[mis][corpus]")
{
  auto files = collect_mis_files();
  if (files.empty())
  {
    WARN("no .mis files found in " + kMissionsDir.string() + " — corpus skipped");
    return;
  }
  for (const auto& p : files)
  {
    std::ifstream f(p);
    REQUIRE(f.is_open());
    INFO("checking " << p.filename().string());
    REQUIRE(is_mis_file(f));
    // Stream position must be restored.
    REQUIRE(f.tellg() == std::streampos(0));
  }
}

TEST_CASE("parse_mis: all real .mis files parse without throwing", "[mis][corpus]")
{
  auto files = collect_mis_files();
  if (files.empty())
  {
    WARN("no .mis files found — corpus skipped");
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

      // Root must be SimGroup (case-insensitive comparison).
      std::string cn = mf.root.class_name;
      std::transform(cn.begin(), cn.end(), cn.begin(), [](unsigned char c){ return std::tolower(c); });
      if (cn != "simgroup")
      {
        failures.push_back(p.filename().string() + " (root not SimGroup: " + mf.root.class_name + ")");
        continue;
      }

      // Must have at least one child.
      if (mf.root.children.empty())
      {
        failures.push_back(p.filename().string() + " (root has no children)");
        continue;
      }

      ++ok;
    }
    catch (const std::exception& e)
    {
      failures.push_back(p.filename().string() + " (" + e.what() + ")");
    }
  }

  INFO("MIS corpus: " << ok << "/" << files.size() << " parsed cleanly");
  if (!failures.empty())
  {
    std::string msg = "failures: ";
    for (std::size_t i = 0; i < failures.size() && i < 10; ++i)
    {
      if (i) msg += "; ";
      msg += failures[i];
    }
    FAIL(msg);
  }
  REQUIRE(ok == files.size());
}

TEST_CASE("parse_mis: exec_calls present in each real mission", "[mis][corpus]")
{
  auto files = collect_mis_files();
  if (files.empty())
  {
    WARN("no .mis files found — skipped");
    return;
  }
  for (const auto& p : files)
  {
    std::ifstream f(p);
    if (!f) continue;
    auto mf = read_mis_file(f);
    INFO("checking exec_idents in " << p.filename().string());
    REQUIRE_FALSE(mf.trailer.exec_idents.empty());
  }
}

TEST_CASE("parse_mis: at least one mission has game_mission_type", "[mis][corpus]")
{
  auto files = collect_mis_files();
  if (files.empty()) { WARN("skipped"); return; }

  bool found = false;
  for (const auto& p : files)
  {
    std::ifstream f(p);
    if (!f) continue;
    auto mf = read_mis_file(f);
    if (mf.trailer.game_mission_type.has_value())
    {
      found = true;
      break;
    }
  }
  REQUIRE(found);
}
