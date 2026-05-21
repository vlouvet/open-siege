#ifndef STUDIO_CONTENT_MISSION_MIS_HPP
#define STUDIO_CONTENT_MISSION_MIS_HPP

// Parser for Tribes 1 `.mis` ConsoleScript mission files.
//
// `.mis` files are ASCII text with a specific layout:
//
//   //--- export object begin ---//
//   instant SimGroup "MissionGroup" {
//     instant ClassName "InstanceName" {
//       key = "value";
//       arrayKey[N] = "value";
//       ...
//     };
//     instant ClassName "InstanceName";   // empty body
//     ...
//   };
//   //--- export object end ---//
//   exec(scriptIdent);
//   $Game::missionType = "CTF";
//   $teamScoreLimit = 5;
//   $cdTrack = 4;
//   $cdPlayMode = 1;
//
// All property values inside the object tree are double-quoted strings.
// Trailer lines (after the end marker) use unquoted numeric literals and
// quoted string literals.
//
// Format reverse-engineered by ASCII/hex inspection of the 42 `.mis`
// files shipped with Tribes 1.41 freeware.  No leaked Dynamix source
// was consulted.

#include <array>
#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace studio::content::mission
{
  // A single key=value pair on an object.  Array subscripts (`key[N]`)
  // are split into `key` + `array_index`; simple keys have a nullopt
  // array_index.
  struct mis_property
  {
    std::string key;
    std::optional<std::int32_t> array_index;  // set when property was `key[N]`
    std::string value;                        // raw quoted-string content (no quotes)
  };

  // One instantiated object node in the hierarchy.
  struct mis_object
  {
    std::string class_name;                          // e.g. "SimGroup", "Sky"
    std::optional<std::string> instance_name;        // absent when the decl had no quotes
    std::vector<mis_property> properties;
    std::vector<mis_object> children;
  };

  // Metadata that appears after the `//--- export object end ---//` marker.
  struct mis_trailer
  {
    std::optional<std::string> game_mission_type;  // value of $Game::missionType
    std::optional<std::int32_t> team_score_limit;  // $teamScoreLimit
    std::optional<std::int32_t> dm_score_limit;    // $DMScoreLimit
    std::optional<std::int32_t> cd_track;          // $cdTrack
    std::optional<std::int32_t> cd_play_mode;      // $cdPlayMode
    std::vector<std::string> exec_idents;          // each exec(IDENT) target, in order
  };

  // Top-level result of parsing a `.mis` file.
  struct mis_file
  {
    mis_object root;     // always class_name == "SimGroup", instance_name == "MissionGroup"
    mis_trailer trailer;
  };

  // Returns true when `in` begins with the export magic line.
  // The stream position is restored before returning.
  bool is_mis_file(std::istream& in);

  // Parse the entire `.mis` from `in`.  Throws std::runtime_error on
  // malformed input.  The stream is left at EOF on success.
  mis_file read_mis_file(std::istream& in);

  // ---------------------------------------------------------------------------
  // Helper value-converters (inline, work on raw property strings).
  // ---------------------------------------------------------------------------

  // Tolerates "NaN", "-NAN", scientific notation, and plain decimals.
  std::optional<float> parse_float(std::string_view sv);

  // Case-insensitive; recognises "true"/"false"/"1"/"0".
  std::optional<bool> parse_bool(std::string_view sv);

  // Three space-separated floats (e.g. "0 0 -20" or "0.3 0.3 0.3").
  std::optional<std::array<float, 3>> parse_vec3(std::string_view sv);

  // Six space-separated floats.
  std::optional<std::array<float, 6>> parse_vec6(std::string_view sv);

}  // namespace studio::content::mission

#endif  // STUDIO_CONTENT_MISSION_MIS_HPP
