#ifndef DTS_VIEWER_ENTITY_RENDERER_HPP
#define DTS_VIEWER_ENTITY_RENDERER_HPP

// Track 14 — entity stubs.  Each scene-graph object type gets a state
// struct + tick + render pair.  Rendering is intentionally minimal:
// coloured wireframe cubes per entity type, sized to suggest scale.
// Full DTS prop loading is a follow-up polish pass (the existing
// dts-viewer DTS pipeline can render any of these meshes; wiring the
// resolver across the mission's mounted VOLs is the missing piece).

#include <deque>
#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "content/mission/scene.hpp"
#include "player_controller.hpp"

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

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
};

std::vector<TurretState> collect_turrets(
    const studio::content::mission::scene_graph& scene);

// Track 14 spec 05 / 14 spec 03 — turrets fire only when their team
// still has power.  Passing `bool team_has_power_team_id` is a tiny
// callback shape; we just thread a bool here.
// `on_damage` (optional) is invoked with the bearing (radians) of
// the attacker relative to the player's facing — feeds the HUD
// damage reticle (Track 13 spec 05).
using DamageBearingCallback = void(*)(float bearing_from_player_yaw);
void tick_turrets(std::vector<TurretState>& turrets,
                  PlayerState& player,
                  std::deque<std::string>& feed,
                  bool team_has_power,
                  float dt,
                  DamageBearingCallback on_damage = nullptr);

// ---- Moveables (spec 14/04) -----------------------------------------------

struct MoveableState
{
    studio::content::mission::transform xf;
    std::string data_block_name;
    glm::vec3   endpoint_a { 0.0f };
    glm::vec3   endpoint_b { 0.0f };   // a + (0, 10, 0) for v1
    enum class Phase { AtA, MovingToB, AtB, MovingToA } phase = Phase::AtA;
    float t = 0.0f;
    float close_time = 4.0f;
    float dwell_remaining = 0.0f;
    float dwell_time = 2.0f;
};

std::vector<MoveableState> collect_moveables(
    const studio::content::mission::scene_graph& scene);

void tick_moveables(std::vector<MoveableState>& m,
                    const PlayerState& player,
                    float dt);

// Helper: current world-space position of a moveable (caller renders here).
glm::vec3 moveable_position(const MoveableState& m);

// ---- Generators (spec 14/05) ----------------------------------------------

struct GeneratorState
{
    studio::content::mission::transform xf;
    std::string data_block_name;     // "Generator" or "PortGenerator"
    int   team = 0;
    float health = 250.0f;
    float health_max = 250.0f;
    bool  destroyed = false;
    bool  is_portable = false;
};

std::vector<GeneratorState> collect_generators(
    const studio::content::mission::scene_graph& scene);

void apply_damage_generator(GeneratorState& g, float dmg);

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
                   float dt);

// ---- Vehicle placeholders (spec 14/08) ------------------------------------

struct VehiclePlaceholderState
{
    studio::content::mission::transform pad_xf;
    std::string pad_data_block;       // "vehiclePad"
    std::string vehicle_dts;          // "hover_apc.dts" placeholder
    bool visible = true;
};

std::vector<VehiclePlaceholderState> collect_vehicle_placeholders(
    const studio::content::mission::scene_graph& scene);

// ---- Shared rendering -----------------------------------------------------

// Draws a single coloured wireframe cube at `world_pos`, scaled by `size`.
// Caller binds a flat-color shader and sets u_mvp / u_color per draw.
void render_entity_cube(
    const glm::vec3& world_pos,
    float size,
    const std::array<float, 3>& color,
    GLint u_mvp_loc,
    GLint u_color_loc,
    const glm::mat4& view_proj);

} // namespace dts_viewer

#endif // DTS_VIEWER_ENTITY_RENDERER_HPP
