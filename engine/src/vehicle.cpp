#include "vehicle.hpp"
#include "height_sampler.hpp"

#include <algorithm>
#include <cmath>

namespace dts_viewer
{

namespace
{

// Tribes Z-up MIS coords -> GL Y-up world coords. Matches the conversion
// used by mis_pos_to_gl elsewhere; duplicated here to keep vehicle.cpp
// dependency-free against the wider MIS axes header.
glm::vec3 tribes_to_gl(const std::array<float, 3>& p)
{
    return glm::vec3(p[0], p[2], p[1]);
}

} // anonymous namespace

glm::vec3 vehicle_world_pos_gl(const VehiclePlaceholderState& v)
{
    if (v.dynamic) return v.dyn_pos_gl;
    glm::vec3 base = tribes_to_gl(v.pad_xf.position);
    base.y += 1.5f;   // sit on top of the pad (matches spec 14/08 render)
    return base;
}

int vehicle_find_mountable(
    const std::vector<VehiclePlaceholderState>& vehs,
    const glm::vec3& player_pos_gl,
    float mount_radius)
{
    int best = -1;
    float best_d2 = mount_radius * mount_radius;
    for (std::size_t i = 0; i < vehs.size(); ++i) {
        glm::vec3 vp = vehicle_world_pos_gl(vehs[i]);
        glm::vec3 d  = vp - player_pos_gl;
        float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
        if (d2 < best_d2) { best_d2 = d2; best = static_cast<int>(i); }
    }
    return best;
}

void vehicle_tick(VehiclePlaceholderState& v,
                  const VehicleTuning&     t,
                  const VehicleInput&      in,
                  const HeightSampler&     terrain,
                  float                    dt)
{
    if (!v.dynamic) {
        // First tick on this vehicle — seed dynamic position from the pad.
        v.dyn_pos_gl = vehicle_world_pos_gl(v);
        v.dynamic = true;
    }

    // Yaw snaps to the camera target. Vehicle ICR is at the chassis
    // centre — no spring lag in v1.
    v.yaw = in.target_yaw;

    // Local frame: forward is camera-relative +Z; right is +X.
    const float cy = std::cos(v.yaw);
    const float sy = std::sin(v.yaw);
    const glm::vec3 fwd  { sy, 0.0f, cy };
    const glm::vec3 right{ cy, 0.0f, -sy };

    // Apply gravity + thrust + lateral input as instantaneous accelerations.
    glm::vec3 accel { 0.0f, -t.gravity, 0.0f };
    if (in.up)    accel.y += t.max_thrust;
    if (in.down)  accel.y -= t.descend_accel;
    if (in.fwd)   accel  += fwd   *  t.forward_accel;
    if (in.back)  accel  += fwd   * -t.forward_accel;
    if (in.left)  accel  += right * -t.lateral_accel;
    if (in.right) accel  += right *  t.lateral_accel;

    v.vel += accel * dt;

    // Linear drag (per-axis, same coefficient — keeps it isotropic).
    const float damp = std::max(0.0f, 1.0f - t.linear_drag * dt);
    v.vel.x *= damp;
    v.vel.z *= damp;
    // Vertical drag is lighter so hover feels responsive.
    v.vel.y *= std::max(0.0f, 1.0f - 0.3f * dt);

    // Cap speeds.
    const float h_speed = std::sqrt(v.vel.x * v.vel.x + v.vel.z * v.vel.z);
    if (h_speed > t.top_speed) {
        const float k = t.top_speed / h_speed;
        v.vel.x *= k; v.vel.z *= k;
    }
    if (v.vel.y >  t.top_vertical) v.vel.y =  t.top_vertical;
    if (v.vel.y < -t.top_vertical) v.vel.y = -t.top_vertical;

    v.dyn_pos_gl += v.vel * dt;

    // Soft terrain floor — clamp to ground_clearance above terrain.
    if (terrain.valid()) {
        const float ty = terrain.sample(v.dyn_pos_gl.x, v.dyn_pos_gl.z);
        const float min_y = ty + t.ground_clearance;
        if (v.dyn_pos_gl.y < min_y) {
            v.dyn_pos_gl.y = min_y;
            if (v.vel.y < 0.0f) v.vel.y = 0.0f;
        }
    }
}

glm::vec3 vehicle_dismount_pos(const VehiclePlaceholderState& v,
                               const VehicleTuning& t)
{
    glm::vec3 base = vehicle_world_pos_gl(v);
    const float cy = std::cos(v.yaw);
    const float sy = std::sin(v.yaw);
    // Drop the pilot off the vehicle's local +X (right of the chassis).
    glm::vec3 right{ cy, 0.0f, -sy };
    base += right * t.dismount_offset;
    return base;
}

} // namespace dts_viewer
