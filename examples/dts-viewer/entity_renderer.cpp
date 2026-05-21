#include "entity_renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <variant>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

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
    std::vector<MoveableState> out;
    walk(scene.root, [&](const auto& n) {
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T,
                studio::content::mission::node_moveable>) {
                MoveableState s;
                s.xf = p.xf;
                s.data_block_name = p.data_block.name;
                s.endpoint_a = glm::vec3{ p.xf.position[0],
                                          p.xf.position[1],
                                          p.xf.position[2] };
                s.endpoint_b = s.endpoint_a + glm::vec3{ 0.0f, 10.0f, 0.0f };
                s.close_time = p.close_time > 0.1f ? p.close_time : 4.0f;
                s.dwell_time = p.delay_time > 0.0f ? p.delay_time : 2.0f;
                if (p.status == "up") { s.phase = MoveableState::Phase::AtB; s.t = 1.0f; }
                out.push_back(s);
            }
        }, n.payload);
    });
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
        glm::vec3 pos{ it.xf.position[0], it.xf.position[1], it.xf.position[2] };
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
                  DamageBearingCallback on_damage)
{
    for (auto& t : turrets) {
        if (t.destroyed) continue;
        if (!team_has_power_flag) {
            t.fire_cooldown = std::max(0.0f, t.fire_cooldown - dt);
            continue;
        }
        glm::vec3 pos{ t.xf.position[0], t.xf.position[1], t.xf.position[2] };
        glm::vec3 d = pos - player.pos;
        float dist = glm::length(d);
        t.fire_cooldown = std::max(0.0f, t.fire_cooldown - dt);
        if (dist > t.scan_range) continue;
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
    for (auto& mv : m) {
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

void apply_damage_generator(GeneratorState& g, float dmg)
{
    if (g.destroyed) return;
    g.health = std::max(0.0f, g.health - dmg);
    if (g.health <= 0.0f) g.destroyed = true;
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
                   float /*dt*/)
{
    for (auto& tr : triggers) {
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
        } else if (!inside && tr.was_inside) {
            feed.push_back("trigger exit: " + tr.data_block_name);
            if (feed.size() > 100) feed.pop_front();
        }
        tr.was_inside = inside;
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

namespace
{

const float kCubeEdges[] = {
    -0.5f, -0.5f, -0.5f,   0.5f, -0.5f, -0.5f,
     0.5f, -0.5f, -0.5f,   0.5f, -0.5f,  0.5f,
     0.5f, -0.5f,  0.5f,  -0.5f, -0.5f,  0.5f,
    -0.5f, -0.5f,  0.5f,  -0.5f, -0.5f, -0.5f,
    -0.5f,  0.5f, -0.5f,   0.5f,  0.5f, -0.5f,
     0.5f,  0.5f, -0.5f,   0.5f,  0.5f,  0.5f,
     0.5f,  0.5f,  0.5f,  -0.5f,  0.5f,  0.5f,
    -0.5f,  0.5f,  0.5f,  -0.5f,  0.5f, -0.5f,
    -0.5f, -0.5f, -0.5f,  -0.5f,  0.5f, -0.5f,
     0.5f, -0.5f, -0.5f,   0.5f,  0.5f, -0.5f,
     0.5f, -0.5f,  0.5f,   0.5f,  0.5f,  0.5f,
    -0.5f, -0.5f,  0.5f,  -0.5f,  0.5f,  0.5f,
};

} // anonymous namespace

void render_entity_cube(
    const glm::vec3& world_pos,
    float size,
    const std::array<float, 3>& color,
    GLint u_mvp_loc,
    GLint u_color_loc,
    const glm::mat4& view_proj)
{
    static GLuint vao = 0, vbo = 0;
    if (vao == 0) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeEdges), kCubeEdges, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    }
    glm::mat4 M = glm::translate(glm::mat4(1.0f), world_pos);
    M = glm::scale(M, glm::vec3(size));
    glm::mat4 MVP = view_proj * M;
    if (u_mvp_loc >= 0)
        glUniformMatrix4fv(u_mvp_loc, 1, GL_FALSE, glm::value_ptr(MVP));
    if (u_color_loc >= 0)
        glUniform3fv(u_color_loc, 1, color.data());
    glBindVertexArray(vao);
    glDrawArrays(GL_LINES, 0, 24);
    glBindVertexArray(0);
}

} // namespace dts_viewer
