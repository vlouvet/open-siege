// Engine-side entity collect+tick implementations.
// Split from apps/dts-viewer/entity_renderer.cpp in track 26 spec 01.

#include "osengine/entity_state.hpp"
#include "osengine/height_sampler.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <variant>

#include <glm/gtc/matrix_transform.hpp>


namespace dts_viewer
{

namespace
{

// Walk helper.
template <typename F>
void walk(const studio::content::mission::scene_node& n, F&& fn)
{
    fn(n);
    for (auto& c : n.children) walk(c, fn);
}

// Pull a numeric extra_property by name (returns the default when missing).
int extra_property_int(
    const studio::content::mission::scene_node& n,
    const std::string& key,
    int dflt = 0)
{
    for (const auto& p : n.extra_properties) {
        if (p.key == key) {
            try { return std::stoi(p.value); }
            catch (...) { return dflt; }
        }
    }
    return dflt;
}

bool extra_property_bool(
    const studio::content::mission::scene_node& n,
    const std::string& key,
    bool dflt = false)
{
    for (const auto& p : n.extra_properties) {
        if (p.key == key) {
            std::string v = p.value;
            for (auto& c : v) c = static_cast<char>(std::tolower((unsigned char)c));
            return v == "true" || v == "1";
        }
    }
    return dflt;
}

const std::set<std::string>& turret_datablocks()
{
    static const std::set<std::string> s = {
        "TurretBasePermanent", "TurretBase",
        "AATurret", "MissileTurret", "ELFTurret",
    };
    return s;
}

bool is_turret_datablock(const std::string& s)
{
    return turret_datablocks().count(s) > 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Collectors
// ---------------------------------------------------------------------------

std::vector<StaticShapeState> collect_static_shapes(
    const studio::content::mission::scene_graph& scene)
{
    std::vector<StaticShapeState> out;
    walk(scene.root, [&](const auto& n) {
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T,
                studio::content::mission::node_static_shape>) {
                out.push_back({ p.xf, p.data_block.name });
            }
        }, n.payload);
    });
    return out;
}

std::vector<ItemState> collect_items(
    const studio::content::mission::scene_graph& scene)
{
    std::vector<ItemState> out;
    walk(scene.root, [&](const auto& n) {
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T,
                studio::content::mission::node_item>) {
                ItemState s;
                s.xf = p.xf;
                s.data_block_name = p.data_block.name;
                out.push_back(s);
            }
        }, n.payload);
    });
    return out;
}

std::vector<TurretState> collect_turrets(
    const studio::content::mission::scene_graph& scene)
{
    std::vector<TurretState> out;
    walk(scene.root, [&](const auto& n) {
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T,
                studio::content::mission::node_turret>) {
                TurretState s;
                s.xf = p.xf;
                s.data_block_name = p.data_block.name;
                s.team = extra_property_int(n, "team", 0);
                out.push_back(s);
            } else if constexpr (std::is_same_v<T,
                studio::content::mission::node_static_shape>) {
                if (is_turret_datablock(p.data_block.name)) {
                    TurretState s;
                    s.xf = p.xf;
                    s.data_block_name = p.data_block.name;
                    s.team = extra_property_int(n, "team", 0);
                    out.push_back(s);
                }
            }
        }, n.payload);
    });
    return out;
}

std::vector<MoveableState> collect_moveables(
    const studio::content::mission::scene_graph& scene)
{
    using namespace studio::content::mission;
    std::vector<MoveableState> out;

    // 14/11 — moveables and their SimPath siblings live under the same
    // parent SimGroup. Walk groups, then for each group gather any path
    // (with its marker-child waypoints) so we can attach it to sibling
    // moveables in the same group.
    auto gather_waypoints = [](const scene_node& path_node)
        -> std::pair<std::vector<glm::vec3>, bool>
    {
        std::vector<glm::vec3> wps;
        bool loop = false;
        if (const auto* p = std::get_if<node_path>(&path_node.payload)) {
            loop = p->is_looping;
        }
        for (const auto& child : path_node.children) {
            if (const auto* m = std::get_if<node_marker>(&child.payload)) {
                wps.emplace_back(m->xf.position[0],
                                 m->xf.position[2],
                                 m->xf.position[1]);
            }
        }
        return { std::move(wps), loop };
    };

    auto visit_group = [&](const scene_node& parent, auto& self) -> void {
        // First pass: pick the first SimPath sibling (Tribes elevators
        // typically have at most one path per group).
        std::vector<glm::vec3> wps;
        bool loop = false;
        for (const auto& child : parent.children) {
            if (std::holds_alternative<node_path>(child.payload)) {
                auto [w, l] = gather_waypoints(child);
                if (!w.empty()) { wps = std::move(w); loop = l; break; }
            }
        }
        // Second pass: emit moveable states, attaching waypoints when found.
        for (const auto& child : parent.children) {
            if (const auto* p = std::get_if<node_moveable>(&child.payload)) {
                MoveableState s;
                s.xf = p->xf;
                s.data_block_name = p->data_block.name;
                s.endpoint_a = glm::vec3{ p->xf.position[0],
                                          p->xf.position[2],
                                          p->xf.position[1] };
                s.endpoint_b = s.endpoint_a + glm::vec3{ 0.0f, 10.0f, 0.0f };
                s.close_time = p->close_time > 0.1f ? p->close_time : 4.0f;
                s.dwell_time = p->delay_time > 0.0f ? p->delay_time : 2.0f;
                if (p->status == "up") {
                    s.phase = MoveableState::Phase::AtB;
                    s.t = 1.0f;
                }
                if (!wps.empty()) {
                    s.waypoints = wps;
                    s.loop_path = loop;
                }
                out.push_back(std::move(s));
            }
            // Recurse into sub-groups (SimGroups, TeamGroups, etc.) so we
            // also find moveables nested deeper.
            if (!child.children.empty()
                && !std::holds_alternative<node_path>(child.payload)) {
                self(child, self);
            }
        }
    };
    visit_group(scene.root, visit_group);
    std::size_t with_path = 0;
    for (const auto& mv : out) if (!mv.waypoints.empty()) ++with_path;
    if (with_path > 0) {
        std::fprintf(stderr,
            "moveables: %zu total, %zu linked to SimPath waypoints\n",
            out.size(), with_path);
    }
    return out;
}

std::vector<GeneratorState> collect_generators(
    const studio::content::mission::scene_graph& scene)
{
    std::vector<GeneratorState> out;
    walk(scene.root, [&](const auto& n) {
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T,
                studio::content::mission::node_static_shape>) {
                if (p.data_block.name == "Generator" ||
                    p.data_block.name == "PortGenerator")
                {
                    GeneratorState g;
                    g.xf = p.xf;
                    g.data_block_name = p.data_block.name;
                    g.team = extra_property_int(n, "team", 0);
                    g.is_portable = (p.data_block.name == "PortGenerator");
                    out.push_back(g);
                }
            }
        }, n.payload);
    });
    return out;
}

std::vector<TriggerState> collect_triggers(
    const studio::content::mission::scene_graph& scene)
{
    std::vector<TriggerState> out;
    walk(scene.root, [&](const auto& n) {
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T,
                studio::content::mission::node_trigger>) {
                TriggerState s;
                s.xf = {};                          // triggers carry no xf
                s.bbox = p.bounding_box;
                s.is_sphere = p.is_sphere;
                s.data_block_name = "Trigger";      // node carries no datablock
                out.push_back(s);
            }
        }, n.payload);
    });
    return out;
}

std::vector<VehiclePlaceholderState> collect_vehicle_placeholders(
    const studio::content::mission::scene_graph& scene)
{
    std::vector<VehiclePlaceholderState> out;
    walk(scene.root, [&](const auto& n) {
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T,
                studio::content::mission::node_static_shape>) {
                if (p.data_block.name == "vehiclePad") {
                    VehiclePlaceholderState v;
                    v.pad_xf = p.xf;
                    v.pad_data_block = p.data_block.name;
                    v.vehicle_dts = "hover_apc.dts";
                    out.push_back(v);
                }
            }
        }, n.payload);
    });
    return out;
}

// ---------------------------------------------------------------------------
// Tickers
// ---------------------------------------------------------------------------

void tick_items(std::vector<ItemState>& items,
                PlayerState& player,
                const PlayerTuning& tune,
                std::deque<std::string>& feed,
                float dt)
{
    constexpr float kPickupRadius = 1.5f;
    constexpr float kRespawnTime  = 30.0f;
    for (auto& it : items) {
        if (!it.active) {
            it.respawn_remaining -= dt;
            if (it.respawn_remaining <= 0.0f) it.active = true;
            continue;
        }
        glm::vec3 pos{ it.xf.position[0], it.xf.position[2], it.xf.position[1] };
        float dist = glm::length(player.pos - pos);
        if (dist > kPickupRadius) continue;
        // Effect by datablock
        if (it.data_block_name == "HealthPatch" ||
            it.data_block_name == "RepairKit")
        {
            player.health = std::min(player.health_max, player.health + 25.0f);
        }
        else if (it.data_block_name == "AmmoPack")
        {
            for (auto& w : player.inventory.weapons) {
                if (w.equipped) {
                    w.ammo = std::min(w.max_ammo,
                        w.ammo + std::max(1, w.max_ammo / 2));
                }
            }
        }
        else
        {
            player.jet_fuel = std::min(tune.jet_fuel_max,
                player.jet_fuel + tune.jet_fuel_max * 0.5f);
        }
        feed.push_back("picked up " + it.data_block_name);
        if (feed.size() > 100) feed.pop_front();
        it.active = false;
        it.respawn_remaining = kRespawnTime;
    }
}

void tick_turrets(std::vector<TurretState>& turrets,
                  PlayerState& player,
                  std::deque<std::string>& feed,
                  bool team_has_power_flag,
                  float dt,
                  DamageBearingCallback on_damage,
                  const HeightSampler* terrain)
{
    for (auto& t : turrets) {
        if (t.destroyed) continue;
        // Spec 16/10 — script-driven fire bypasses team-power + range + LoS.
        // Forces an immediate shot at the player no matter where they are.
        if (t.script_fire_latch) {
            t.script_fire_latch = false;
            player.health = std::max(0.0f, player.health - 15.0f);
            t.fire_cooldown = 2.5f;
            feed.push_back("turret fire (script): " + t.data_block_name);
            if (feed.size() > 100) feed.pop_front();
            if (on_damage) {
                glm::vec3 pos{ t.xf.position[0], t.xf.position[2], t.xf.position[1] };
                glm::vec3 d = pos - player.pos;
                float bearing_world = std::atan2(d.x, d.z);
                on_damage(bearing_world - player.yaw);
            }
            continue;
        }
        if (!team_has_power_flag) {
            t.fire_cooldown = std::max(0.0f, t.fire_cooldown - dt);
            continue;
        }
        glm::vec3 pos{ t.xf.position[0], t.xf.position[2], t.xf.position[1] };
        glm::vec3 d = pos - player.pos;
        float dist = glm::length(d);
        t.fire_cooldown = std::max(0.0f, t.fire_cooldown - dt);
        if (dist > t.scan_range) continue;
        // 14/10 — line-of-sight raycast from turret muzzle (~3m above
        // deck) to player torso (~1m above feet). If terrain blocks
        // the segment, the turret may scan but does not fire.
        if (terrain && terrain->valid()) {
            const float mx = pos.x;
            const float my = pos.y + 3.0f;
            const float mz = pos.z;
            const float px = player.pos.x;
            const float py = player.pos.y + 1.0f;
            const float pz = player.pos.z;
            if (!terrain->has_terrain_los(mx, my, mz, px, py, pz)) continue;
        }
        if (t.fire_cooldown <= 0.0f) {
            player.health = std::max(0.0f, player.health - 15.0f);
            t.fire_cooldown = 2.5f;
            feed.push_back("turret fire from " + t.data_block_name);
            if (feed.size() > 100) feed.pop_front();
            if (on_damage) {
                // Bearing FROM player TO turret, relative to player yaw.
                float bearing_world = std::atan2(d.x, d.z);
                float relative = bearing_world - player.yaw;
                on_damage(relative);
            }
        }
    }
}

void tick_moveables(std::vector<MoveableState>& m,
                    const PlayerState& player,
                    float dt)
{
    (void)player;
    for (auto& mv : m) {
        // 14/11 — when a SimPath was attached, play through its
        // waypoints continuously (no player gating; matches Tribes'
        // elevators that ride on their own clock).
        if (!mv.waypoints.empty()) {
            const int n = static_cast<int>(mv.waypoints.size());
            if (n == 1) {
                mv.endpoint_a = mv.endpoint_b = mv.waypoints[0];
                mv.t = 0.0f;
                continue;
            }
            int next = mv.wp_index + 1;
            if (next >= n) next = mv.loop_path ? 0 : n - 1;
            // Dwell at each waypoint.
            if (mv.dwell_remaining > 0.0f) {
                mv.dwell_remaining -= dt;
                mv.endpoint_a = mv.endpoint_b = mv.waypoints[mv.wp_index];
                mv.t = 0.0f;
                continue;
            }
            const float seg_time = std::max(0.5f, mv.close_time);
            mv.wp_t += dt / seg_time;
            if (mv.wp_t >= 1.0f) {
                mv.wp_t = 0.0f;
                mv.wp_index = next;
                mv.dwell_remaining = mv.dwell_time;
                // If we hit the terminal waypoint of a non-looping path,
                // stay there (dwell forever; effectively parked).
                if (!mv.loop_path && mv.wp_index >= n - 1) {
                    mv.dwell_remaining = 1e9f;
                }
                next = mv.wp_index + 1;
                if (next >= n) next = mv.loop_path ? 0 : n - 1;
            }
            mv.endpoint_a = mv.waypoints[mv.wp_index];
            mv.endpoint_b = mv.waypoints[next];
            mv.t = mv.wp_t;
            continue;
        }

        // Fallback: original endpoint_a -> endpoint_b player-triggered ride.
        glm::vec3 a = mv.endpoint_a;
        glm::vec3 b = mv.endpoint_b;
        glm::vec3 cur = glm::mix(a, b, mv.t);

        const float dx = player.pos.x - cur.x;
        const float dz = player.pos.z - cur.z;
        const bool xy_ok = (dx * dx + dz * dz) < (4.0f * 4.0f);
        const bool y_ok  = (player.pos.y >= cur.y - 1.0f) &&
                           (player.pos.y <= cur.y + 4.0f);
        const bool standing = xy_ok && y_ok;

        switch (mv.phase) {
            case MoveableState::Phase::AtA:
                if (standing) mv.phase = MoveableState::Phase::MovingToB;
                break;
            case MoveableState::Phase::MovingToB:
                mv.t += dt / std::max(0.5f, mv.close_time);
                if (mv.t >= 1.0f) {
                    mv.t = 1.0f;
                    mv.phase = MoveableState::Phase::AtB;
                    mv.dwell_remaining = mv.dwell_time;
                }
                break;
            case MoveableState::Phase::AtB:
                mv.dwell_remaining -= dt;
                if (mv.dwell_remaining <= 0.0f)
                    mv.phase = MoveableState::Phase::MovingToA;
                break;
            case MoveableState::Phase::MovingToA:
                mv.t -= dt / std::max(0.5f, mv.close_time);
                if (mv.t <= 0.0f) {
                    mv.t = 0.0f;
                    mv.phase = MoveableState::Phase::AtA;
                }
                break;
        }
    }
}

glm::vec3 moveable_position(const MoveableState& m)
{
    return glm::mix(m.endpoint_a, m.endpoint_b, m.t);
}

void apply_damage_generator(GeneratorState& g, float dmg,
                            void (*on_destroyed)(const GeneratorState&))
{
    if (g.destroyed) return;
    g.health = std::max(0.0f, g.health - dmg);
    if (g.health <= 0.0f) {
        g.destroyed = true;
        // Spec 16/10 — fire the script-side callback on the transition.
        if (on_destroyed) on_destroyed(g);
    }
}

bool team_has_power(int team_id, const std::vector<GeneratorState>& gens)
{
    if (gens.empty()) return true;   // no-generator missions default to powered
    for (const auto& g : gens) {
        if (g.destroyed) continue;
        if (g.team == 0 || g.team == team_id) return true;
    }
    return false;
}

void tick_triggers(std::vector<TriggerState>& triggers,
                   const PlayerState& player,
                   std::deque<std::string>& feed,
                   float /*dt*/,
                   EntityIndexCallback on_enter)
{
    for (std::size_t i = 0; i < triggers.size(); ++i) {
        auto& tr = triggers[i];
        if (!tr.active) continue;
        bool inside = false;
        if (!tr.is_sphere) {
            inside =
                player.pos.x >= tr.bbox[0] && player.pos.x <= tr.bbox[3] &&
                player.pos.y >= tr.bbox[1] && player.pos.y <= tr.bbox[4] &&
                player.pos.z >= tr.bbox[2] && player.pos.z <= tr.bbox[5];
        } else {
            float cx = 0.5f * (tr.bbox[0] + tr.bbox[3]);
            float cy = 0.5f * (tr.bbox[1] + tr.bbox[4]);
            float cz = 0.5f * (tr.bbox[2] + tr.bbox[5]);
            float r  = std::max({ tr.bbox[3] - cx,
                                  tr.bbox[4] - cy,
                                  tr.bbox[5] - cz });
            float dx = player.pos.x - cx;
            float dy = player.pos.y - cy;
            float dz = player.pos.z - cz;
            inside = (dx * dx + dy * dy + dz * dz) <= (r * r);
        }
        if (inside && !tr.was_inside) {
            feed.push_back("trigger enter: " + tr.data_block_name);
            if (feed.size() > 100) feed.pop_front();
            ++tr.fire_count;
            // Spec 16/10 — fire the script-side onEnter callback on
            // the rising edge.
            if (on_enter) on_enter(static_cast<int>(i));
        } else if (!inside && tr.was_inside) {
            feed.push_back("trigger exit: " + tr.data_block_name);
            if (feed.size() > 100) feed.pop_front();
        }
        tr.was_inside = inside;
    }
}

} // namespace dts_viewer
