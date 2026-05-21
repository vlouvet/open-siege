#pragma once

// Spec 25/05 — Asset Browser side panel. Lists every entry of every
// mounted VOL as a collapsible tree (grouped by VOL, then by path
// prefix). Double-click loads .mis / .dts; right-click → Extract,
// Hex dump.

#include <filesystem>
#include <string>
#include <vector>

namespace dts_viewer {

struct MountedVol
{
    std::filesystem::path path;
    std::string           short_name;   // filename only, lowercased
};

void asset_browser_init(const std::vector<MountedVol>& mounts);

// Each frame inside ImGui. Caller controls visibility via a bool ref
// (so the View > Asset Browser menu item can toggle it).
void asset_browser_draw(bool& visible);

} // namespace dts_viewer
