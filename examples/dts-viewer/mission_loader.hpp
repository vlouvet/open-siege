#ifndef DTS_VIEWER_MISSION_LOADER_HPP
#define DTS_VIEWER_MISSION_LOADER_HPP

// Mission loader for the dts-viewer --mission-info flag.
//
// Parses a Tribes 1 .mis file into a scene_graph, mounts SimVolume
// entries in declared order, and prints a human-readable summary.
// v1 does not render anything — it proves the parse + mount pipeline.

#include "content/mission/mis.hpp"
#include "content/mission/scene.hpp"

#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
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
