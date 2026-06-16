#ifndef DTS_VIEWER_MISSION_LOADER_HPP
#define DTS_VIEWER_MISSION_LOADER_HPP

// Mission loader for the dts-viewer --mission-info flag.
//
// Parses a Tribes 1 .mis file into a scene_graph, mounts SimVolume
// entries in declared order, and prints a human-readable summary.
// v1 does not render anything — it proves the parse + mount pipeline.

#include "content/mission/mis.hpp"
#include "content/mission/scene.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dts_viewer
{

struct LoadedMission
{
    studio::content::mission::scene_graph scene;
    std::filesystem::path mis_path;
    std::vector<std::filesystem::path> mounted_vols;   // resolved, in mount order
    std::string mission_type;                          // from trailer; "" if absent
};

// 14c-I-6 — scope-always object discovered in the mission scene graph.
//
// One entry per .mis object that the server must emit as a scope-always
// ghost intro at session start (StaticShape, Turret, Item-with-flag-role,
// Sensor, Marker). Position/rotation come from the .mis transform; the
// datablock_name is the script-level name (e.g. "PlasmaTurret",
// "InventoryStation") used to look up the catalogue index later.
//
// Reference: TRIBES-GHOST-CLASSES.md §2.1 (scope-always set rules).
struct ScopeAlwaysIntro
{
    enum class Kind : std::uint8_t {
        StaticShape = 0,  // mission-pinned static (turret base, generator, etc.)
        Turret      = 1,  // active turret (derives from StaticShape per §3.9)
        Marker      = 2,  // CTF zone, drop-point, spawn marker, path marker
        Item        = 3,  // CTF flag (scope-always per §2.1; not generic items)
        Sensor      = 4,  // mission-pinned sensor station (§3.8)
    };

    Kind kind = Kind::StaticShape;
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};
    std::string datablock_name;       // e.g. "PlasmaTurret", "flag", "PathMarker"
    std::string instance_name;        // .mis instance label (for debug log)
    std::uint8_t team_id = 0;         // 0 = neutral, 1 = red, 2 = blue (per FlagWorld)
};

// Walk the scene graph and collect every scope-always object the server
// must replicate at session start. Order is depth-first declaration
// order, which matches how a freshly-loaded engine instantiates objects
// (and therefore the order TAH's intro registry will see them).
//
// Per TRIBES-GHOST-CLASSES.md §2.1:
//   - StaticShape (incl. Turret subclass), Sensor, Marker → always
//   - Item → only if its datablock indicates a CTF flag (datablock name
//     contains "flag" case-insensitively); other items are distance-
//     scoped at scope-in time, not in the initial burst.
std::vector<ScopeAlwaysIntro>
scope_always_objects(const LoadedMission& m);

// Collect every datablock name referenced (directly) by the mission's
// scope-always objects. Used by `build_mission_catalogue` to filter the
// global catalogue to just the records this mission needs.
std::unordered_set<std::string>
required_datablock_names(const LoadedMission& m);

// Selftest: parses 5_CTF.mis (assumes tribes-game/base/missions/ is
// resolvable from `tribes_dir`) and asserts scope_always_objects().size() >= 8
// per spec 14c-I-6 acceptance.
int mission_loader_selftest();

// Find missions_dir/<name>.mis case-insensitively.  Returns the resolved
// absolute path, or empty if not found.
std::filesystem::path find_mis(
    const std::filesystem::path& missions_dir,
    std::string_view mission_short_name);

// Load a mission by short name (e.g. "1_Welcome" or "5_CTF").
// base_dir is tribes-game/base/ — world VOLs are resolved there.
// missions_dir is tribes-game/base/missions/ — per-mission VOLs live there.
// Returns nullopt if the MIS file cannot be found or parsed.
std::optional<LoadedMission> load_mission(
    const std::filesystem::path& missions_dir,
    const std::filesystem::path& base_dir,
    std::string_view mission_short_name);

// Print a human-readable summary of what was loaded.
void print_mission_summary(const LoadedMission& m, std::ostream& os);

} // namespace dts_viewer

#endif // DTS_VIEWER_MISSION_LOADER_HPP
