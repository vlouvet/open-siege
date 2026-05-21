#include "inv_station.hpp"

#include <algorithm>
#include <cstdio>
#include <variant>

namespace dts_viewer
{

namespace
{

void walk_collect(
    const studio::content::mission::scene_node& n,
    std::vector<InvStation>& out)
{
    using namespace studio::content::mission;
    std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, node_static_shape>) {
            if (p.data_block.name == "InventoryStation") {
                InvStation s;
                s.pos = glm::vec3{ p.xf.position[0], p.xf.position[1], p.xf.position[2] };
                out.push_back(s);
            }
        }
    }, n.payload);
    for (auto& c : n.children) walk_collect(c, out);
}

void restock_player(PlayerState& player, const PlayerTuning& tune)
{
    for (auto& w : player.inventory.weapons) {
        if (w.equipped) w.ammo = w.max_ammo;
    }
    player.jet_fuel = tune.jet_fuel_max;
    player.jet_lockout = 0.0f;
    player.health = player.health_max;
}

} // anonymous namespace

InvStationSystem inv_stations_load(
    const studio::content::mission::scene_graph& scene)
{
    InvStationSystem sys;
    walk_collect(scene.root, sys.stations);
    std::fprintf(stderr,
        "inv-station: registered %zu stations\n", sys.stations.size());
    return sys;
}

void inv_stations_update(
    InvStationSystem& sys,
    PlayerState&      player,
    const PlayerTuning& tune,
    float             dt)
{
    bool any_in_range = false;
    for (auto& st : sys.stations) {
        if (st.cooldown_remaining > 0.0f) {
            st.cooldown_remaining = std::max(0.0f, st.cooldown_remaining - dt);
            continue;
        }
        glm::vec3 d = player.pos - st.pos;
        float dist = glm::length(d);
        if (dist <= sys.restock_radius) {
            restock_player(player, tune);
            st.cooldown_remaining = sys.restock_cooldown;
            any_in_range = true;
            std::fprintf(stderr,
                "inv-station: restock (dist=%.1f)\n", dist);
        } else if (dist <= sys.restock_radius * 1.5f) {
            any_in_range = true;   // hover-ready, no restock
        }
    }
    sys.player_in_aura = any_in_range;
}

} // namespace dts_viewer
