#ifndef DTS_VIEWER_LAG_COMPENSATION_HPP
#define DTS_VIEWER_LAG_COMPENSATION_HPP

// Spec 22/02 — server-side lag compensation.
//
// Maintains a 1-second rolling history of each replicated object's pose
// (position + yaw) sampled at the server's snapshot rate (default 10 Hz
// = 10 frames). On a hit-event arrival, the server computes
//
//   rewind_delta_ms = min(ping/2 + interp_delay, kMaxRewindMs)
//
// and queries `lookup(t_now_ms - rewind_delta_ms)` to recover the
// pose the client believes the target had at the time the trigger
// was pulled. Hit-detection runs against THAT pose, not the server's
// current state — this is what makes high-ping play feasible.
//
// Anti-cheat clamp (kMaxRewindMs = 500): clients can't request unlimited
// rewind. Per the spec, clients reporting RTT > 1000ms should have hit
// events outright rejected by a higher layer; this header just bounds
// the lookup itself.
//
// The data structure is a tiny ring buffer with linear interpolation
// between adjacent samples. No allocations after construction; lookup
// is O(N) over a ≤16-entry buffer (effectively O(1)).
//
// Pose type is templated so callers can use whatever vec3/quaternion
// representation they want. The default `LagPose` carries pos + yaw,
// which is what Tribes hit-detection needs (capsule-vs-ray intersection
// only depends on capsule centre + uprightness; the player capsule is
// rotationally symmetric so only yaw matters).

#include <array>
#include <cstdint>
#include <cstddef>

namespace dts_viewer {

struct LagPose
{
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float pos_z = 0.0f;
    float yaw   = 0.0f;
};

// Tunables — exported so tests + callers can refer to them.
inline constexpr std::uint32_t kMaxRewindMs = 500;     // anti-cheat cap
inline constexpr std::uint32_t kHardRejectRttMs = 1000; // reject hits above this
inline constexpr std::size_t   kHistoryFrames = 16;    // ~1.6s @ 10Hz

template <std::size_t N = kHistoryFrames>
struct RewindHistory
{
    struct Sample {
        std::uint32_t t_ms = 0;
        LagPose       pose{};
        bool          valid = false;
    };

    std::array<Sample, N> samples{};
    std::size_t write_idx = 0;   // next slot to write
    std::size_t count     = 0;   // # valid samples (≤ N)

    void record(std::uint32_t t_ms, const LagPose& pose) noexcept
    {
        samples[write_idx] = { t_ms, pose, true };
        write_idx = (write_idx + 1) % N;
        if (count < N) ++count;
    }

    void clear() noexcept
    {
        for (auto& s : samples) s.valid = false;
        write_idx = 0;
        count = 0;
    }

    // Look up the pose at `t_ms`, interpolating between the two adjacent
    // recorded samples. Behavior at the edges:
    //   - `t_ms` newer than the newest recorded: returns the newest.
    //   - `t_ms` older than the oldest recorded: returns the oldest.
    //   - In between: linearly interpolates.
    // Returns {} if no samples have been recorded.
    LagPose lookup(std::uint32_t t_ms) const noexcept
    {
        if (count == 0) return LagPose{};

        // Collect valid samples into ascending-time order. With a small
        // ring this is fine — no allocations.
        std::array<const Sample*, N> sorted{};
        std::size_t sorted_n = 0;
        for (std::size_t i = 0; i < N; ++i) {
            if (samples[i].valid) sorted[sorted_n++] = &samples[i];
        }
        // Tiny insertion sort (N ≤ 16).
        for (std::size_t i = 1; i < sorted_n; ++i) {
            for (std::size_t j = i; j > 0 && sorted[j-1]->t_ms > sorted[j]->t_ms; --j) {
                const auto* tmp = sorted[j-1];
                sorted[j-1] = sorted[j];
                sorted[j] = tmp;
            }
        }

        if (t_ms <= sorted[0]->t_ms) return sorted[0]->pose;
        if (t_ms >= sorted[sorted_n - 1]->t_ms) return sorted[sorted_n - 1]->pose;

        // Find the bracketing pair (linear, N is tiny).
        for (std::size_t i = 0; i + 1 < sorted_n; ++i) {
            const auto& a = *sorted[i];
            const auto& b = *sorted[i + 1];
            if (t_ms >= a.t_ms && t_ms <= b.t_ms) {
                if (b.t_ms == a.t_ms) return a.pose;
                const float u = float(t_ms - a.t_ms) / float(b.t_ms - a.t_ms);
                LagPose out;
                out.pos_x = a.pose.pos_x + (b.pose.pos_x - a.pose.pos_x) * u;
                out.pos_y = a.pose.pos_y + (b.pose.pos_y - a.pose.pos_y) * u;
                out.pos_z = a.pose.pos_z + (b.pose.pos_z - a.pose.pos_z) * u;
                out.yaw   = a.pose.yaw   + (b.pose.yaw   - a.pose.yaw  ) * u;
                return out;
            }
        }
        return sorted[sorted_n - 1]->pose;
    }
};

// Compute the rewind delta the server should use for a hit-event from
// a client with `client_rtt_ms` round-trip time and `interp_delay_ms`
// of buffer-induced display lag. The result is clamped to kMaxRewindMs.
// Per-spec acceptance: 200ms-ping and 50ms-ping players should both
// land hits on the same target.
inline std::uint32_t compute_rewind_ms(std::uint32_t client_rtt_ms,
                                       std::uint32_t interp_delay_ms) noexcept
{
    const std::uint32_t want = (client_rtt_ms / 2) + interp_delay_ms;
    return want > kMaxRewindMs ? kMaxRewindMs : want;
}

// Anti-cheat gate: returns false if the client's RTT is so far outside
// reasonable bounds that we should reject the hit event outright
// (per spec §"Anti-cheat clamp": ≥ 1000ms).
inline bool hit_should_be_accepted(std::uint32_t client_rtt_ms) noexcept
{
    return client_rtt_ms < kHardRejectRttMs;
}

} // namespace dts_viewer

#endif // DTS_VIEWER_LAG_COMPENSATION_HPP
