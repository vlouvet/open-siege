#include "player_controller.hpp"
#include "mission_bounds.hpp"
#include "physics_capsule.hpp"

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

    // ---- Integrate with capsule slide (spec 09/06) ----
    // Capsule centre = feet position + half_h on Y.  Sweep the centre
    // along the velocity vector; on contact, project remaining motion
    // onto the contact plane and retry up to `max_slide_iters` times.
    {
        glm::vec3 remaining_vel = ps.vel;
        glm::vec3 capsule_from{ ps.pos.x, ps.pos.y + t.capsule_half_h, ps.pos.z };
        glm::vec3 displacement = remaining_vel * dt;
        int iters = 0;
        while (iters < t.max_slide_iters &&
               glm::dot(displacement, displacement) > 1e-10f)
        {
            glm::vec3 capsule_to = capsule_from + displacement;
            CapsuleHitInfo hit;
            capsule_sweep_interior(capsule_from, capsule_to,
                t.capsule_radius, t.capsule_half_h, hit);
            // Advance up to the contact point (or full move if no hit).
            float advance_t = hit.hit ? std::max(0.0f, hit.time - 0.001f) : 1.0f;
            capsule_from += displacement * advance_t;
            if (!hit.hit) break;
            // Project remaining motion onto the contact plane.
            glm::vec3 leftover = displacement * (1.0f - advance_t);
            float into = glm::dot(leftover, hit.contact_normal);
            leftover -= hit.contact_normal * into;
            // Also kill velocity into the surface so subsequent steps
            // don't re-accumulate.
            float vinto = glm::dot(remaining_vel, hit.contact_normal);
            if (vinto < 0.0f) remaining_vel -= hit.contact_normal * vinto;
            displacement = leftover;
            ++iters;
        }
        ps.pos.x = capsule_from.x;
        ps.pos.y = capsule_from.y - t.capsule_half_h;
        ps.pos.z = capsule_from.z;
        ps.vel = remaining_vel;
    }

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

    // ---- Ground contact + slope (spec 09/05) ----
    const float terrain_y = terrain.valid()
        ? terrain.sample(ps.pos.x, ps.pos.z)
        : 0.0f;
    float normal[3] { 0.0f, 1.0f, 0.0f };
    if (terrain.valid()) terrain.sample_normal(ps.pos.x, ps.pos.z, normal);
    // Hard floor: never tolerate the player below terrain (covers
    // tunneling at high downward speeds, not just first-contact snap).
    bool was_below = ps.pos.y < terrain_y;
    const bool was_on_ground = ps.on_ground;
    if (was_below || (ps.pos.y <= terrain_y + t.ground_slop && ps.vel.y <= 0.0f)) {
        ps.pos.y     = terrain_y;
        if (ps.vel.y < 0.0f) ps.vel.y = 0.0f;
        ps.on_ground = true;
    } else {
        ps.on_ground = false;
    }
    // Slope angle: 0° on flat ground, 90° on a wall.
    const float dot_up = std::clamp(normal[1], -1.0f, 1.0f);
    const float slope_rad = std::acos(dot_up);
    ps.slope_deg = slope_rad * (180.0f / 3.14159265358979323846f);

    // Slope walk-clamp: too-steep, grounded, not jetting, not skiing →
    // can't climb; instead, project velocity onto slope tangent and add
    // downhill gravity component.
    if (ps.on_ground && !ps.jet_active && !ps.skiing &&
        ps.slope_deg > t.max_walk_slope)
    {
        // Project (vel.x, vel.y, vel.z) onto tangent plane defined by normal.
        glm::vec3 N{ normal[0], normal[1], normal[2] };
        glm::vec3 v = ps.vel;
        v -= glm::dot(v, N) * N;
        // Slide friction damps slide momentum a bit.
        v *= std::max(0.0f, 1.0f - t.slide_friction * dt);
        // Add gravity component along slope: g * sin(slope), pointing downhill.
        // Downhill direction is the projection of -Y onto the tangent plane.
        glm::vec3 down{ 0.0f, -1.0f, 0.0f };
        glm::vec3 downhill = down - glm::dot(down, N) * N;
        float dlen = glm::length(downhill);
        if (dlen > 1e-4f) {
            downhill /= dlen;
            v += downhill * (t.gravity * std::sin(slope_rad) * dt);
        }
        ps.vel = v;
        // Re-clamp Y to terrain since velocity reprojection may have lifted us slightly.
        ps.pos.y = terrain_y;
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
