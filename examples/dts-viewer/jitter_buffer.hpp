#ifndef DTS_VIEWER_JITTER_BUFFER_HPP
#define DTS_VIEWER_JITTER_BUFFER_HPP

// Spec 22/03 — jitter buffer + extrapolation fallback.
//
// Per-ghost snapshot history that:
//   - Buffers ~100 ms of incoming server snapshots before display
//     (caller picks the buffer depth via `display_delay_ms`).
//   - Linearly interpolates between adjacent snapshots when the
//     display-time falls between them (smooth motion under jitter).
//   - Extrapolates (extends last known velocity) when the display
//     overshoots the newest snapshot (handles dropped packets).
//   - Clamps extrapolation to `max_extrap_ms` to prevent rubber-
//     banding when a player just keeps not-receiving snapshots.
//
// Snapshot shape — pos + velocity. Yaw is interpolated linearly (good
// enough for the 8-bit yaw quantisation we already have on the wire;
// adjacent snapshots are rarely more than a few degrees apart).
//
// Design choices:
//   - One ring buffer per ghost; small fixed N (=8 frames @ 10 Hz = 800 ms).
//   - No allocations after construction.
//   - Caller is responsible for choosing the display-time offset:
//
//       display_t_ms = network_now_ms() - display_delay_ms
//
//     A typical setting is 100 ms which is one snapshot interval at the
//     default 10 Hz rate plus a small safety margin.

#include <array>
#include <cstdint>
#include <cstddef>
#include <unordered_map>

namespace dts_viewer {

struct JitterSnapshot
{
    float pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;
    float vel_x = 0.0f, vel_y = 0.0f, vel_z = 0.0f;
    float yaw   = 0.0f;
};

inline constexpr std::size_t   kJitterFrames     = 8;     // ~800ms @ 10Hz
inline constexpr std::uint32_t kMaxExtrapolateMs = 200;   // clamp per spec

template <std::size_t N = kJitterFrames>
struct GhostJitterBuffer
{
    struct Slot {
        std::uint32_t  t_ms = 0;
        JitterSnapshot snap{};
        bool           valid = false;
    };
    std::array<Slot, N> slots{};
    std::size_t write_idx = 0;
    std::size_t count = 0;

    void record(std::uint32_t t_ms, const JitterSnapshot& s) noexcept
    {
        slots[write_idx] = { t_ms, s, true };
        write_idx = (write_idx + 1) % N;
        if (count < N) ++count;
    }

    void clear() noexcept
    {
        for (auto& s : slots) s.valid = false;
        write_idx = 0;
        count = 0;
    }

    // Returns the interpolated/extrapolated snapshot at display-time
    // `t_ms`. `was_extrapolated` is set true if the answer is an
    // extrapolation past the newest sample (caller can use this to
    // visually mark "stale" players).
    JitterSnapshot sample(std::uint32_t t_ms,
                          bool* was_extrapolated = nullptr,
                          std::uint32_t max_extrap_ms = kMaxExtrapolateMs) const noexcept
    {
        if (was_extrapolated) *was_extrapolated = false;
        if (count == 0) return JitterSnapshot{};

        // Sort valid samples ascending by time (N tiny — no allocs).
        std::array<const Slot*, N> sorted{};
        std::size_t sorted_n = 0;
        for (std::size_t i = 0; i < N; ++i)
            if (slots[i].valid) sorted[sorted_n++] = &slots[i];
        for (std::size_t i = 1; i < sorted_n; ++i)
            for (std::size_t j = i; j > 0 && sorted[j-1]->t_ms > sorted[j]->t_ms; --j) {
                const auto* tmp = sorted[j-1];
                sorted[j-1] = sorted[j];
                sorted[j] = tmp;
            }

        const Slot& newest = *sorted[sorted_n - 1];

        // Display-time earlier than oldest → use oldest verbatim (no
        // backward extrapolation; player just spawned / connected).
        if (t_ms <= sorted[0]->t_ms) return sorted[0]->snap;

        // Within the recorded range → interpolate between adjacent.
        if (t_ms <= newest.t_ms) {
            for (std::size_t i = 0; i + 1 < sorted_n; ++i) {
                const auto& a = *sorted[i];
                const auto& b = *sorted[i + 1];
                if (t_ms >= a.t_ms && t_ms <= b.t_ms) {
                    if (b.t_ms == a.t_ms) return a.snap;
                    const float u = float(t_ms - a.t_ms) / float(b.t_ms - a.t_ms);
                    JitterSnapshot out;
                    out.pos_x = a.snap.pos_x + (b.snap.pos_x - a.snap.pos_x) * u;
                    out.pos_y = a.snap.pos_y + (b.snap.pos_y - a.snap.pos_y) * u;
                    out.pos_z = a.snap.pos_z + (b.snap.pos_z - a.snap.pos_z) * u;
                    out.vel_x = a.snap.vel_x + (b.snap.vel_x - a.snap.vel_x) * u;
                    out.vel_y = a.snap.vel_y + (b.snap.vel_y - a.snap.vel_y) * u;
                    out.vel_z = a.snap.vel_z + (b.snap.vel_z - a.snap.vel_z) * u;
                    out.yaw   = a.snap.yaw   + (b.snap.yaw   - a.snap.yaw  ) * u;
                    return out;
                }
            }
        }

        // Past the newest → extrapolate by velocity, clamped.
        const std::uint32_t overshoot = t_ms - newest.t_ms;
        const std::uint32_t use = overshoot > max_extrap_ms ? max_extrap_ms : overshoot;
        if (was_extrapolated) *was_extrapolated = true;
        JitterSnapshot out = newest.snap;
        const float dt = static_cast<float>(use) / 1000.0f;
        out.pos_x += newest.snap.vel_x * dt;
        out.pos_y += newest.snap.vel_y * dt;
        out.pos_z += newest.snap.vel_z * dt;
        return out;
    }
};

// Multi-ghost helper: one JitterBuffer per ghost_id. Add records as
// snapshots arrive; sample() at display-time for rendering.
struct JitterBufferRegistry
{
    std::unordered_map<std::uint16_t, GhostJitterBuffer<>> per_ghost;
    std::uint32_t default_display_delay_ms = 100;

    void record(std::uint16_t ghost_id, std::uint32_t t_ms,
                const JitterSnapshot& s)
    {
        per_ghost[ghost_id].record(t_ms, s);
    }

    JitterSnapshot sample(std::uint16_t ghost_id, std::uint32_t now_ms,
                          bool* was_extrapolated = nullptr) const
    {
        auto it = per_ghost.find(ghost_id);
        if (it == per_ghost.end()) {
            if (was_extrapolated) *was_extrapolated = false;
            return JitterSnapshot{};
        }
        const std::uint32_t display_t = now_ms > default_display_delay_ms
            ? now_ms - default_display_delay_ms : 0;
        return it->second.sample(display_t, was_extrapolated);
    }
};

} // namespace dts_viewer

#endif // DTS_VIEWER_JITTER_BUFFER_HPP
