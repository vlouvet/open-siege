#ifndef OSENGINE_GHOST_EMITTER_HPP
#define OSENGINE_GHOST_EMITTER_HPP

// Spec 28/04 — per-client ghost emission.
// Spec 28/04b — wire format is now real T1 1.41 ghost-stream bytes,
// produced by net20::encode_scope_always_burst / encode_normal_delta.
// The OSGB native format that 28/04 shipped is gone; the emitter
// orchestration (per-session view-state, dirty tracking, kill
// propagation, listener integration) is preserved.
//
// Each session owns one GhostEmitter. On every other server tick the
// listener builds a ServerWorldSnapshot and calls emit() on every
// active emitter. The emitter:
//   1. Walks the snapshot, computes which ghosts are dirty for this
//      client (moved more than ε since last emit, or never sent).
//   2. For each dirty ghost: build a GhostPlayer struct from
//      Session.player_state and queue it.
//   3. First emit to a client → encode_scope_always_burst (mass intro).
//      Subsequent emits → encode_normal_delta. Newly-joined peers ride
//      the delta path with full_update=true so they're introduced mid-
//      stream without forcing a full re-burst.

#include "content/net/udp_socket.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dts_viewer
{

struct Session;
struct ServerWorldSnapshot;

// What each client knows about a given ghost.
struct GhostViewEntry
{
    float    last_pos_x = 0.0f;
    float    last_pos_y = 0.0f;
    float    last_pos_z = 0.0f;
    float    last_yaw   = 0.0f;
    std::uint16_t last_sent_seq = 0;
    bool     introduced = false;        // receiver knows this ghost_id + class_tag
};

struct GhostEmitterStats
{
    std::uint64_t packets_emitted   = 0;
    std::uint64_t bytes_emitted     = 0;
    std::uint64_t records_emitted   = 0;
    std::uint64_t kills_emitted     = 0;
    std::uint64_t bursts_emitted    = 0;   // scope-always packets (first emit)
};

// Server-side: class-tag we assign to every Player ghost. Matches the
// spec 28 capture analysis (tag 960 = Player).
constexpr std::uint16_t kServerPlayerClassTag = 960;

class GhostEmitter
{
public:
    using Sink = std::function<bool(const studio::content::net::Endpoint&,
                                    const std::uint8_t*, std::size_t)>;

    GhostEmitter(Session* session, Sink sink);

    // Build + send one ghost packet for this client.
    void emit(const ServerWorldSnapshot& world);

    void on_client_ack(std::uint16_t recv_seq);

    const GhostEmitterStats& stats() const noexcept { return stats_; }

    // Where in the most-recent emitted packet the ghost sub-stream body
    // starts. The encoder always writes a fixed prefix length (VC header
    // with no ack runs + rate prefix + sub-stream presence), so receivers
    // can decode without the heuristic scanner.
    static constexpr std::size_t kGhostStreamStartBit = 29;

private:
    Session* session_;
    Sink     sink_;
    std::unordered_map<std::uint16_t, GhostViewEntry> view_;
    bool     first_emit_pending_ = true;
    GhostEmitterStats stats_;
};

// Spec 28/04 in-process selftest, updated for the spec 28/04b wire
// format. Spawns 3 sessions at distinct positions, runs 5 emit
// cycles, captures the bytes; for each captured packet the receiving
// session's view is rebuilt from the decoded GhostPlayer records and
// position fidelity is asserted.
int ghost_emitter_selftest();

} // namespace dts_viewer

#endif // OSENGINE_GHOST_EMITTER_HPP
