#ifndef OSENGINE_ENTITY_STATE_HPP
#define OSENGINE_ENTITY_STATE_HPP

// Track 14 — entity stubs. Each scene-graph object type gets a state
// struct + collect + tick declaration. Rendering is intentionally split
// to apps/dts-viewer/entity_renderer.hpp (track 26 spec 01).

#include <array>
#include <deque>
#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "content/mission/scene.hpp"
#include "osengine/player_controller.hpp"

namespace dts_viewer
{

// ---- Static shapes (spec 14/01) -------------------------------------------

struct StaticShapeState
{
    studio::content::mission::transform xf;
    std::string data_block_name;
};

std::vector<StaticShapeState> collect_static_shapes(
    const studio::content::mission::scene_graph& scene);

// ---- Items (spec 14/02) ---------------------------------------------------

struct ItemState
{
    studio::content::mission::transform xf;
    std::string data_block_name;
    float respawn_remaining = 0.0f;
    bool  active = true;
};

std::vector<ItemState> collect_items(
    const studio::content::mission::scene_graph& scene);

void tick_items(std::vector<ItemState>& items,
                PlayerState& player,
                const PlayerTuning& tune,
                std::deque<std::string>& feed,
                float dt);

// ---- Turrets (spec 14/03) -------------------------------------------------

struct TurretState
{
    studio::content::mission::transform xf;
    std::string data_block_name;
    int   team = 0;
    float health = 100.0f;
    float fire_cooldown = 0.0f;
    float scan_range = 200.0f;
    bool  destroyed = false;
    bool  script_fire_latch = false;
};

std::vector<TurretState> collect_turrets(
    const studio::content::mission::scene_graph& scene);

using DamageBearingCallback = void(*)(float bearing_from_player_yaw);
struct HeightSampler;
void tick_turrets(std::vector<TurretState>& turrets,
                  PlayerState& player,
                  std::deque<std::string>& feed,
                  bool team_has_power,
                  float dt,
                  DamageBearingCallback on_damage = nullptr,
                  const HeightSampler* terrain = nullptr);

using EntityIndexCallback = void(*)(int entity_idx);

// ---- Moveables (spec 14/04) -----------------------------------------------

struct MoveableState
{
    studio::content::mission::transform xf;
    std::string data_block_name;
    glm::vec3   endpoint_a { 0.0f };
    glm::vec3   endpoint_b { 0.0f };
    enum class Phase { AtA, MovingToB, AtB, MovingToA } phase = Phase::AtA;
    float t = 0.0f;
    float close_time = 4.0f;
    float dwell_remaining = 0.0f;
    float dwell_time = 2.0f;
    std::vector<glm::vec3> waypoints;
    bool   loop_path = false;
    int    wp_index  = 0;
    float  wp_t      = 0.0f;
};

std::vector<MoveableState> collect_moveables(
    const studio::content::mission::scene_graph& scene);

void tick_moveables(std::vector<MoveableState>& m,
                    const PlayerState& player,
                    float dt);

glm::vec3 moveable_position(const MoveableState& m);

// ---- Generators (spec 14/05) ----------------------------------------------

struct GeneratorState
{
    studio::content::mission::transform xf;
    std::string data_block_name;
    int   team = 0;
    float health = 250.0f;
    float health_max = 250.0f;
    bool  destroyed = false;
    bool  is_portable = false;
};

std::vector<GeneratorState> collect_generators(
    const studio::content::mission::scene_graph& scene);

void apply_damage_generator(GeneratorState& g, float dmg,
                            void (*on_destroyed)(const GeneratorState&) = nullptr);

bool team_has_power(int team_id, const std::vector<GeneratorState>& gens);

// ---- Triggers (spec 14/06) ------------------------------------------------

struct TriggerState
{
    studio::content::mission::transform xf;
    std::string data_block_name;
    std::array<float, 6> bbox {};
    bool is_sphere = false;
    bool was_inside = false;
    bool active = true;
    int  fire_count = 0;
};

std::vector<TriggerState> collect_triggers(
    const studio::content::mission::scene_graph& scene);

void tick_triggers(std::vector<TriggerState>& triggers,
                   const PlayerState& player,
                   std::deque<std::string>& feed,
                   float dt,
                   EntityIndexCallback on_enter = nullptr);

// ---- Vehicle placeholders (spec 14/08, driving in spec 14/13) ------------

struct VehiclePlaceholderState
{
    studio::content::mission::transform pad_xf;
    std::string pad_data_block;
    std::string vehicle_dts;
    bool visible = true;
    bool       dynamic     = false;
    bool       piloted     = false;
    glm::vec3  dyn_pos_gl  { 0.0f };
    glm::vec3  vel         { 0.0f };
    float      yaw         = 0.0f;
};

std::vector<VehiclePlaceholderState> collect_vehicle_placeholders(
    const studio::content::mission::scene_graph& scene);

} // namespace dts_viewer

#endif // OSENGINE_ENTITY_STATE_HPP
