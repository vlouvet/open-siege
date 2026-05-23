#ifndef DTS_VIEWER_PREDICTED_PLAYER_HPP
#define DTS_VIEWER_PREDICTED_PLAYER_HPP

// Spec 22/01 — client-side input prediction.
//
// Wraps the existing PlayerState/PlayerTuning physics step
// (`player_update`) with a move-sequence-stamped input history and a
// reconcile-on-snapshot loop. Per clean-room spec §9.3, the binding
// key is the 32-bit move sequence the server echoes back as
// "last-applied". We replay every input *after* that sequence to
// reconstruct what the server should converge to.
//
// Header-only — runs in pure simulation against a flat HeightSampler,
// so the prediction loop is unit-testable without networking. The
// integration into dts-viewer's net path is a follow-up once the
// client connects as an authoritative player rather than spectator.
//
// Snap thresholds (per spec §10):
//   - mismatch < 5 cm  → silent accept (replayed state replaces local)
//   - mismatch < 50 cm → blend (50% toward replayed)
//   - mismatch ≥ 50 cm → hard snap (replayed wins outright)
//
// Rollback bound: spec §9.3 caps replay at ~1 s of input. We use 64
// pending moves as a conservative cap (~ 2 s at 30 Hz). Inputs older
// than that are dropped silently — they were never going to be
// acknowledged anyway.

#include "player_controller.hpp"

#include <cstdint>
#include <deque>

namespace dts_viewer {

struct PredictedPlayer
{
    PlayerState  predicted;
    PlayerTuning tuning;

    struct StampedInput
    {
        std::uint32_t seq = 0;
        InputState    in{};
        float         dt = 0.0f;
    };

    // Inputs we've applied locally but the server hasn't acknowledged
    // yet. Sorted by seq ascending. Drained on reconcile.
    std::deque<StampedInput> pending;

    // Next sequence number to stamp on the next apply_local() call.
    // Starts at 1; matches the 32-bit move-seq on the wire.
    std::uint32_t next_seq = 1;

    // Thresholds (metres) — see file header.
    float smooth_threshold = 0.05f;
    float snap_threshold   = 0.50f;

    // Hard cap on pending history. Once exceeded, the oldest inputs are
    // dropped (we'd never be able to reconcile to them anyway).
    std::size_t max_pending = 64;

    // Diagnostics — bumped by reconcile().
    std::uint32_t reconciliations  = 0;
    std::uint32_t snaps            = 0;
    std::uint32_t blends           = 0;
    std::uint32_t silent_accepts   = 0;
    float         total_error_m    = 0.0f;
    float         max_error_m      = 0.0f;

    // Step the local prediction forward by one input. Returns the seq
    // assigned to this input — the caller embeds this in the c→s wire
    // packet so the server can echo it back as `last_applied`.
    std::uint32_t apply_local(const InputState& in, float dt,
                              const HeightSampler& terrain,
                              const MissionBounds* bounds)
    {
        StampedInput s{ next_seq++, in, dt };
        player_update(predicted, tuning, in, terrain, bounds, dt);
        pending.push_back(s);
        // Trim history.
        while (pending.size() > max_pending) pending.pop_front();
        return s.seq;
    }

    // Reconcile against an authoritative server snapshot. The server
    // reports `last_applied_seq`; we drop every pending input ≤ that
    // seq, then replay the rest starting from the server's state.
    //
    // The comparison metric is straight 3D position distance — yaw/pitch
    // mismatch is intentionally ignored (the client owns view orientation
    // locally; servers don't correct view).
    void reconcile(const PlayerState& authoritative,
                   std::uint32_t last_applied_seq,
                   const HeightSampler& terrain,
                   const MissionBounds* bounds)
    {
        ++reconciliations;

        // Drop pending inputs that the server has already integrated.
        while (!pending.empty() && pending.front().seq <= last_applied_seq)
            pending.pop_front();

        // Re-simulate from `authoritative` forward through every
        // unacked input. The result is what `predicted` SHOULD have
        // been if our local sim agreed with the server's.
        PlayerState replayed = authoritative;
        for (const auto& s : pending)
            player_update(replayed, tuning, s.in, terrain, bounds, s.dt);

        // Compare predicted vs replayed.
        const float dx = replayed.pos.x - predicted.pos.x;
        const float dy = replayed.pos.y - predicted.pos.y;
        const float dz = replayed.pos.z - predicted.pos.z;
        const float err = std::sqrt(dx * dx + dy * dy + dz * dz);
        total_error_m += err;
        if (err > max_error_m) max_error_m = err;

        if (err < smooth_threshold) {
            // Within tolerance — accept silently.
            predicted = replayed;
            ++silent_accepts;
        } else if (err < snap_threshold) {
            // Mid-band — blend halfway.
            predicted.pos = 0.5f * (predicted.pos + replayed.pos);
            predicted.vel = 0.5f * (predicted.vel + replayed.vel);
            predicted.jet_fuel = 0.5f * (predicted.jet_fuel + replayed.jet_fuel);
            ++blends;
        } else {
            // Large mismatch — hard snap, keep view orientation.
            const float saved_yaw   = predicted.yaw;
            const float saved_pitch = predicted.pitch;
            predicted = replayed;
            predicted.yaw   = saved_yaw;
            predicted.pitch = saved_pitch;
            ++snaps;
        }
    }

    // Reset diagnostics (call between independent tests).
    void reset_diagnostics()
    {
        reconciliations = snaps = blends = silent_accepts = 0;
        total_error_m = max_error_m = 0.0f;
    }
};

} // namespace dts_viewer

#endif // DTS_VIEWER_PREDICTED_PLAYER_HPP
