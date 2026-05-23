// Spec 18/01 — extract bot path data from a mission's SimGroup hierarchy.
//
// Retail Tribes 1.41 places bots via:
//   - MissionGroup\Teams\team<N>\AI\<droneName> — per-drone marker queue;
//     index 0 is the spawn marker, indices 1..N-1 are waypoints in declared
//     order.
//   - MissionGroup\AIGraph — optional flat list of nav-node markers used
//     by the engine for obstacle avoidance between waypoints.
//
// This header exposes pure-data extraction; engine bindings + the tick
// loop are in spec 18/02 (ai_bindings) and spec 18/03 (ai_tick).
#pragma once

#include "content/mission/scene.hpp"

#include <array>
#include <string>
#include <vector>

namespace dts_viewer {

struct BotPath
{
    std::string drone_name;
    int team = -1;
    std::array<float, 3> spawn_pos{};
    std::array<float, 4> spawn_rot{};        // quaternion x y z w as in scene_node
    std::vector<std::array<float, 3>> waypoints;
};

struct NavGraph
{
    std::vector<std::array<float, 3>> nodes;
};

// Walk `root` (the scene_graph's root.children) and collect every
// MissionGroup\Teams\team<N>\AI\<droneName> sub-group into a BotPath.
std::vector<BotPath> load_bot_paths(
    const std::vector<studio::content::mission::scene_node>& root_children);

// Walk `root` for MissionGroup\AIGraph. Returns an empty NavGraph if not
// present.
NavGraph load_nav_graph(
    const std::vector<studio::content::mission::scene_node>& root_children);

} // namespace dts_viewer
