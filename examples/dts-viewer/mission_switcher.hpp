#ifndef DTS_VIEWER_MISSION_SWITCHER_HPP
#define DTS_VIEWER_MISSION_SWITCHER_HPP

// Mission switcher — Spec 05 (08-walkable-viewer track).
//
// Provides a registry of mission short-names (sorted natural-alphabetically)
// and a cycle helper for the `[` / `]` hotkeys.  The actual GL resource
// teardown + reload lives in main.cpp; this module just owns the list and
// the index pointer.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dts_viewer
{

struct MissionRegistry
{
    std::vector<std::string>      short_names;       // natural-sorted
    std::vector<std::filesystem::path> ted_paths;    // matching .ted abs paths
    std::size_t                   current_index = 0;
};

// Build the registry by enumerating `<dir>/*.ted` (case-insensitive),
// pairing each .ted with its matching .mis presence (advisory only).
MissionRegistry scan_missions(const std::filesystem::path& missions_dir);

// Move `current_index` by `delta` (wrapping at both ends) and invoke
// `load_callback(short_name)`; if the callback returns false the index
// is rewound and an empty string returned.  Returns the new short-name
// on success.
std::string cycle_mission(
    MissionRegistry& reg,
    int delta,
    std::function<bool(const std::string&, const std::filesystem::path&)> load_callback);

// Print a list of all missions (highlighting the current one) to stderr.
// Cheap; called when `M` is pressed.
void print_mission_list(const MissionRegistry& reg);

} // namespace dts_viewer

#endif // DTS_VIEWER_MISSION_SWITCHER_HPP
