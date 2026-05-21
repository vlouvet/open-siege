#include "projectile.hpp"
#include "player_controller.hpp"

#include <algorithm>
#include <cmath>

namespace dts_viewer
{

namespace
{

// Apply splash damage + impulse to the player when an explosion at
// `origin` is within `radius`.  Damage falloff is linear (1 - d/r);
// impulse scales the same way and points outward from `origin`.  The
// owner_id is used to apply the self-damage coefficient.
void apply_splash(PlayerState& p,
                  const glm::vec3& origin,
                  float radius,
                  float max_dmg,
                  float max_impulse,
                  int owner_id,
                  float self_dmg_coef,
                  int player_id = 0)
{
    glm::vec3 d = p.pos - origin;
    float dist = glm::length(d);
    if (dist >= radius) return;
    float falloff = 1.0f - (dist / radius);
    if (falloff < 0.0f) falloff = 0.0f;

    const bool self_hit = (owner_id == player_id);
    const float dmg_coef = self_hit ? self_dmg_coef : 1.0f;
    // Damage routing through the HUD/health system lands in Track 13;
    // for now we just push the impulse, which is the immediately visible
    // half of the spec.
    (void)max_dmg; (void)dmg_coef;

    glm::vec3 push_dir = (dist > 1e-4f) ? (d / dist) : glm::vec3{0.0f, 1.0f, 0.0f};
    player_apply_impulse(p, push_dir * (max_impulse * falloff));
}

// Linear ray vs heightmap — march by `step` metres until we either hit
// terrain or exceed `max_dist`.  Returns the contact point or the
// terminus if no hit.
glm::vec3 ray_terrain_hit(const HeightSampler& terrain,
                          const glm::vec3& origin,
                          const glm::vec3& dir,
                          float max_dist,
                          bool& hit_out,
                          float step = 0.5f)
{
    hit_out = false;
    if (!terrain.valid() || max_dist <= 0.0f) return origin + dir * max_dist;
    const float n = std::max(1.0f, max_dist / std::max(0.05f, step));
    for (float t = 0.0f; t <= max_dist; t += max_dist / n) {
        glm::vec3 p = origin + dir * t;
        float ty = terrain.sample(p.x, p.z);
        if (p.y <= ty) {
            hit_out = true;
            p.y = ty;
            return p;
        }
    }
    return origin + dir * max_dist;
}

} // anonymous namespace

bool projectile_fire(
    ProjectileSystem&         sys,
    const ProjectileTuning&   t,
    ProjType                  type,
    const glm::vec3&          origin,
    const glm::vec3&          aim_dir,
    int                       owner_id,
    glm::vec3*                out_hit_pos)
{
    glm::vec3 dir = (glm::dot(aim_dir, aim_dir) > 1e-6f)
        ? glm::normalize(aim_dir)
        : glm::vec3{0.0f, 0.0f, 1.0f};

    auto spawn = [&](ProjType pt, float init_speed, float lifetime,
                     int bounces, bool toss_arc) {
        Projectile p;
        p.type = pt;
        p.pos = origin;
        glm::vec3 d = toss_arc
            ? glm::normalize(dir + glm::vec3{0.0f, 0.17f, 0.0f})
            : dir;
        p.vel = d * init_speed;
        p.lifetime_left = lifetime;
        p.bounces_left = bounces;
        p.owner_id = owner_id;
        sys.alive.push_back(p);
    };

    switch (type) {
    case ProjType::Disc:
        if (sys.cooldown_disc > 0.0f) return false;
        spawn(ProjType::Disc, t.disc_init_speed, t.disc_lifetime, 0, false);
        sys.cooldown_disc = t.disc_fire_interval;
        return true;
    case ProjType::Grenade:
        if (sys.cooldown_grenade > 0.0f) return false;
        spawn(ProjType::Grenade, t.gren_init_speed,
              t.gren_fuse_seconds, t.gren_max_bounces, true);
        sys.cooldown_grenade = t.gren_fire_interval;
        return true;
    case ProjType::Plasma:
        spawn(ProjType::Plasma, t.plasma_init_speed,
              t.plasma_lifetime, 0, false);
        return true;
    case ProjType::Mortar:
        spawn(ProjType::Mortar, t.mortar_init_speed,
              t.mortar_fuse_seconds, t.mortar_max_bounces, true);
        return true;
    case ProjType::ChainBullet:
        if (sys.cooldown_chain > 0.0f) return false;
        sys.cooldown_chain = t.chain_fire_interval;
        if (out_hit_pos) *out_hit_pos = origin + dir * t.chain_range;
        return true;
    }
    return false;
}

void projectiles_update(
    ProjectileSystem&        sys,
    const ProjectileTuning&  t,
    const HeightSampler&     terrain,
    PlayerState&             player,
    float                    dt)
{
    sys.cooldown_disc    = std::max(0.0f, sys.cooldown_disc    - dt);
    sys.cooldown_grenade = std::max(0.0f, sys.cooldown_grenade - dt);
    sys.cooldown_chain   = std::max(0.0f, sys.cooldown_chain   - dt);

    for (auto& p : sys.alive) {
        if (!p.alive) continue;

        // Integrate
        glm::vec3 g{0.0f, -20.0f, 0.0f};
        float g_scale;
        switch (p.type) {
            case ProjType::Disc:    g_scale = t.disc_gravity_scale;    break;
            case ProjType::Plasma:  g_scale = t.plasma_gravity_scale;  break;
            case ProjType::Grenade: g_scale = t.gren_gravity_scale;    break;
            case ProjType::Mortar:  g_scale = t.mortar_gravity_scale;  break;
            default:                g_scale = 0.0f;                    break;
        }
        p.vel += g * (g_scale * dt);
        if (p.type == ProjType::Grenade) {
            p.vel *= std::max(0.0f, 1.0f - t.gren_drag * dt);
        } else if (p.type == ProjType::Mortar) {
            p.vel *= std::max(0.0f, 1.0f - t.mortar_drag * dt);
        }
        glm::vec3 new_pos = p.pos + p.vel * dt;

        // Terrain check at the destination
        float ty = terrain.valid() ? terrain.sample(new_pos.x, new_pos.z) : 0.0f;
        bool ground_hit = new_pos.y <= ty;
        if (ground_hit) new_pos.y = ty;

        p.pos = new_pos;

        // Lifetime / fuse
        p.lifetime_left -= dt;
        const bool fuse_done = (p.lifetime_left <= 0.0f);

        bool detonate = false;
        const bool bounces_type =
            (p.type == ProjType::Grenade) || (p.type == ProjType::Mortar);
        if (!bounces_type) {
            detonate = ground_hit || fuse_done;
        } else {
            if (fuse_done) {
                detonate = true;
            } else if (ground_hit) {
                const float decay = (p.type == ProjType::Mortar)
                    ? t.mortar_bounce_decay : t.gren_bounce_decay;
                if (p.bounces_left > 0) {
                    p.vel.y = -p.vel.y * decay;
                    p.vel.x *= decay;
                    p.vel.z *= decay;
                    --p.bounces_left;
                } else {
                    detonate = true;
                }
            }
        }

        if (detonate) {
            float r, dmg, imp;
            switch (p.type) {
                case ProjType::Disc:
                    r = t.disc_splash_radius;  dmg = t.disc_splash_dmg;
                    imp = t.disc_splash_impulse; break;
                case ProjType::Plasma:
                    r = t.plasma_splash_radius; dmg = t.plasma_splash_dmg;
                    imp = t.plasma_splash_impulse; break;
                case ProjType::Grenade:
                    r = t.gren_splash_radius;  dmg = t.gren_splash_dmg;
                    imp = t.gren_splash_impulse; break;
                case ProjType::Mortar:
                    r = t.mortar_splash_radius; dmg = t.mortar_splash_dmg;
                    imp = t.mortar_splash_impulse; break;
                default:
                    r = 0.0f; dmg = 0.0f; imp = 0.0f; break;
            }
            apply_splash(player, p.pos, r, dmg, imp,
                p.owner_id, t.disc_self_dmg_coef);
            p.alive = false;
        }
    }

    // Compact
    sys.alive.erase(
        std::remove_if(sys.alive.begin(), sys.alive.end(),
            [](const Projectile& p) { return !p.alive; }),
        sys.alive.end());
}

} // namespace dts_viewer
