#include "player_controller.hpp"
#include "mission_bounds.hpp"

#include <algorithm>
#include <cmath>

namespace dts_viewer
{

// Track 09 specs 02 + 03 — gravity, ground detection, coyote-time jump,
// horizontal walking with friction and air control.  Y-up convention.
//
// Subsequent specs in this track plug in:
//   04 jetpack.md              -> upward thrust while jet held + fuel burn/regen
//   05 collision-terrain.md    -> slope_deg + edge cases at the heightmap boundary
//   06 collision-interiors.md  -> capsule sweep against BSP
void player_update(PlayerState&        ps,
                   const PlayerTuning& t,
                   const InputState&   in,
                   const HeightSampler& terrain,
                   const MissionBounds* bounds,
                   float               dt)
{
    if (dt <= 0.0f) return;

    // ---- Horizontal: wish velocity, acceleration, friction ----
    // Yaw convention: forward = (sin(yaw), 0, cos(yaw)); right = (cos(yaw), 0, -sin(yaw)).
    const glm::vec3 fwd_h  { std::sin(ps.yaw), 0.0f,  std::cos(ps.yaw) };
    const glm::vec3 right_h{ std::cos(ps.yaw), 0.0f, -std::sin(ps.yaw) };

    glm::vec3 wishdir(0.0f);
    if (in.fwd)   wishdir += fwd_h;
    if (in.back)  wishdir -= fwd_h;
    if (in.right) wishdir += right_h;
    if (in.left)  wishdir -= right_h;
    const bool has_input = glm::dot(wishdir, wishdir) > 0.0f;
    if (has_input) wishdir = glm::normalize(wishdir);

    const float speed_cap = t.max_walk_speed * (in.sprint ? t.sprint_mult : 1.0f);
    const glm::vec3 wish_vel = wishdir * speed_cap;

    const float accel = ps.on_ground ? t.ground_accel
                                     : (t.ground_accel * t.air_control);

    // Lerp horizontal velocity toward wish_vel at `accel`.  When grounded
    // and no input, apply friction to bleed off momentum.
    glm::vec3 vel_h{ ps.vel.x, 0.0f, ps.vel.z };
    if (has_input) {
        glm::vec3 dv = wish_vel - vel_h;
        const float max_step = accel * dt;
        const float dv_len = glm::length(dv);
        if (dv_len > max_step && dv_len > 1e-6f) {
            dv *= (max_step / dv_len);
        }
        vel_h += dv;
    } else if (ps.on_ground) {
        const float decay = std::max(0.0f, 1.0f - t.friction * dt);
        vel_h *= decay;
    }

    // Ground speed cap (does NOT apply while airborne — preserves
    // jump-momentum and is the prerequisite for skiing in Track 10).
    if (ps.on_ground) {
        const float h_speed = std::sqrt(vel_h.x * vel_h.x + vel_h.z * vel_h.z);
        if (h_speed > speed_cap && h_speed > 1e-6f) {
            const float k = speed_cap / h_speed;
            vel_h.x *= k;
            vel_h.z *= k;
        }
    }

    ps.vel.x = vel_h.x;
    ps.vel.z = vel_h.z;

    // ---- Vertical: gravity + jet (spec 09/04) ----
    // Shared Space key: a brief tap fires the jump impulse (handled at end);
    // a held Space (past `jet_tap_seconds`) flips into jet thrust mode.
    if (in.jet) {
        ps.jet_hold_timer += dt;
    } else {
        ps.jet_hold_timer = 0.0f;
    }

    if (ps.jet_lockout > 0.0f) ps.jet_lockout = std::max(0.0f, ps.jet_lockout - dt);

    const bool jet_eligible = (ps.jet_hold_timer >= t.jet_tap_seconds) && in.jet;
    const bool jet_has_fuel = (ps.jet_fuel > 0.0f) &&
                              (ps.jet_lockout <= 0.0f);

    ps.jet_active = jet_eligible && jet_has_fuel;
    if (ps.jet_active) {
        const bool burst = in.sprint;
        const float thrust = burst ? t.jet_thrust_burst : t.jet_thrust;
        const float burn   = t.jet_fuel_burn * (burst ? t.jet_burst_mult : 1.0f);
        ps.vel.y += thrust * dt;
        if (ps.vel.y > t.jet_upward_cap) ps.vel.y = t.jet_upward_cap;
        ps.jet_fuel = std::max(0.0f, ps.jet_fuel - burn * dt);
        if (ps.jet_fuel <= 0.0f) {
            ps.jet_lockout = t.jet_lockout_sec;
            ps.jet_active  = false;
        }
        // Don't apply gravity while jetting — thrust is net upward.
    } else {
        ps.vel.y -= t.gravity * dt;
        // Fuel regen only when grounded AND not pressing jet.
        if (ps.on_ground && !in.jet) {
            ps.jet_fuel = std::min(t.jet_fuel_max,
                                   ps.jet_fuel + t.jet_fuel_regen * dt);
        }
        // Lockout clears once fuel regenerates above the resume threshold.
        if (ps.jet_lockout <= 0.0f && ps.jet_fuel >= t.jet_resume_fuel) {
            // no-op; gate above re-enables jet next eligible step
        }
    }

    // ---- Integrate ----
    ps.pos.x += ps.vel.x * dt;
    ps.pos.y += ps.vel.y * dt;
    ps.pos.z += ps.vel.z * dt;

    // ---- Bounds clamp (XZ only; Y is owned by terrain contact) ----
    if (bounds) {
        std::array<float, 3> p{ ps.pos.x, ps.pos.y, ps.pos.z };
        bool clamped = false;
        p = clamp_to_bounds(p, *bounds, &clamped);
        if (clamped) {
            if (p[0] != ps.pos.x) ps.vel.x = 0.0f;
            if (p[2] != ps.pos.z) ps.vel.z = 0.0f;
            ps.pos.x = p[0];
            ps.pos.z = p[2];
        }
    }

    // ---- Ground contact ----
    const float terrain_y = terrain.valid()
        ? terrain.sample(ps.pos.x, ps.pos.z)
        : 0.0f;
    const bool was_on_ground = ps.on_ground;
    if (ps.pos.y <= terrain_y + t.ground_slop && ps.vel.y <= 0.0f) {
        ps.pos.y    = terrain_y;
        ps.vel.y    = 0.0f;
        ps.on_ground = true;
    } else {
        ps.on_ground = false;
    }

    // Coyote timer
    if (was_on_ground && !ps.on_ground) {
        ps.coyote_timer = t.coyote_seconds;
    } else if (!ps.on_ground) {
        ps.coyote_timer = std::max(0.0f, ps.coyote_timer - dt);
    } else {
        ps.coyote_timer = 0.0f;
    }

    // ---- Edge-triggered jump ----
    const bool jump_edge = in.jump && !ps.prev_jump;
    ps.prev_jump = in.jump;
    ps.jump_latch = false;
    if (jump_edge && (ps.on_ground || ps.coyote_timer > 0.0f)) {
        ps.vel.y    = t.jump_speed;
        ps.on_ground = false;
        ps.coyote_timer = 0.0f;
        ps.jump_latch = true;
    }
}

} // namespace dts_viewer
