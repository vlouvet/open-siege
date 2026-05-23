#ifndef DTS_VIEWER_INV_STATION_HPP
#define DTS_VIEWER_INV_STATION_HPP

// Inventory station restock — Track 12 spec 04.
//
// Scans the scene for StaticShapes with dataBlock == "InventoryStation".
// When the player walks within `restock_radius`, all weapons refill +
// jet fuel + health restore.  Per-station cooldown prevents fountain
// abuse.

#include "player_controller.hpp"
#include "content/mission/scene.hpp"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace dts_viewer
{

struct InvStation
{
    glm::vec3 pos { 0.0f };
    float     cooldown_remaining = 0.0f;
};

struct InvStationSystem
{
    std::vector<InvStation> stations;
    float restock_radius   = 3.0f;
    float restock_cooldown = 2.0f;
    bool  player_in_aura   = false;  // for HUD readout
};

// Collect every InventoryStation in the scene.
InvStationSystem inv_stations_load(
    const studio::content::mission::scene_graph& scene);

// Per fixed-step: walk stations, fire restock on entering-radius.
void inv_stations_update(
    InvStationSystem& sys,
    PlayerState&      player,
    const PlayerTuning& tune,
    float             dt);

} // namespace dts_viewer

#endif // DTS_VIEWER_INV_STATION_HPP
