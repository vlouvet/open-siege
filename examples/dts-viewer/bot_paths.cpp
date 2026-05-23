// Spec 18/01 — extract bot path data from a mission's SimGroup hierarchy.
//
// MissionGroup
//   Teams
//     team0
//       AI
//         guardOne                 <- the per-drone group
//           <spawnMarker>          <- index 0
//           <waypoint1>            <- index 1
//           <waypoint2>            <- index 2
//           ...
//         guardTwo
//           ...
//     team1
//       ...
//   AIGraph
//     <navNode0>
//     <navNode1>
//     ...

#include "bot_paths.hpp"

#include <cctype>

namespace mission_ns = studio::content::mission;

namespace dts_viewer {

namespace {

bool iequals(const std::string& a, const char* b)
{
    std::size_t i = 0;
    for (; b[i] && i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return b[i] == '\0' && i == a.size();
}

// Find a direct-child SimGroup container by class_name OR instance_name
// (case-insensitive). Returns nullptr if absent. SimGroup containers carry
// std::monostate payloads (see scene_node docs).
const mission_ns::scene_node* find_group_child(
    const std::vector<mission_ns::scene_node>& children,
    const char* name)
{
    for (const auto& c : children)
    {
        if (!std::holds_alternative<std::monostate>(c.payload)) continue;
        if (c.instance_name && iequals(*c.instance_name, name)) return &c;
        if (iequals(c.class_name, name)) return &c;
    }
    return nullptr;
}

// Marker-payload child accessor. Returns nullptr if `n`'s payload is not a
// node_marker.
const mission_ns::node_marker* as_marker(const mission_ns::scene_node& n)
{
    return std::get_if<mission_ns::node_marker>(&n.payload);
}

} // namespace

namespace {

// Collect drones from an `AI` SimGroup. Each direct child is a per-drone
// SimGroup; index 0 marker is spawn, indices 1..N-1 are waypoints.
void collect_drones_from_ai_group(
    const mission_ns::scene_node& ai_group,
    int team,
    std::vector<BotPath>& out)
{
    for (const auto& drone : ai_group.children)
    {
        if (!std::holds_alternative<std::monostate>(drone.payload)) continue;
        if (drone.children.empty()) continue;

        const auto* spawn = as_marker(drone.children.front());
        if (!spawn) continue;

        BotPath bp;
        bp.drone_name = drone.instance_name.value_or(drone.class_name);
        bp.team       = team;
        bp.spawn_pos  = spawn->xf.position;
        bp.spawn_rot  = spawn->xf.rotation;

        for (std::size_t i = 1; i < drone.children.size(); ++i)
        {
            if (const auto* wm = as_marker(drone.children[i]))
                bp.waypoints.push_back(wm->xf.position);
        }
        out.push_back(std::move(bp));
    }
}

} // namespace

std::vector<BotPath> load_bot_paths(
    const std::vector<mission_ns::scene_node>& root_children)
{
    std::vector<BotPath> out;

    // Convention A (multiplayer, per ai.cs L85): MissionGroup\Teams\team<N>\AI.
    if (const auto* teams = find_group_child(root_children, "Teams"))
    {
        for (int t = 0; t < 8; ++t)
        {
            std::string team_name = "team" + std::to_string(t);
            const auto* team_node = find_group_child(teams->children, team_name.c_str());
            if (!team_node) continue;
            const auto* ai_group = find_group_child(team_node->children, "AI");
            if (!ai_group) continue;
            collect_drones_from_ai_group(*ai_group, t, out);
        }
    }

    // Convention B (single-player / training, per Training_AI.cs L34):
    // MissionGroup\AI\guard<N>. Team unspecified — Training::setupAI
    // assigns team 1 explicitly after createAI, so we default to team 1
    // for this convention.
    if (const auto* ai_root = find_group_child(root_children, "AI"))
    {
        collect_drones_from_ai_group(*ai_root, /*team=*/1, out);
    }

    return out;
}

NavGraph load_nav_graph(
    const std::vector<mission_ns::scene_node>& root_children)
{
    NavGraph g;
    const auto* graph = find_group_child(root_children, "AIGraph");
    if (!graph) return g;

    for (const auto& n : graph->children)
        if (const auto* m = as_marker(n))
            g.nodes.push_back(m->xf.position);

    return g;
}

} // namespace dts_viewer
