#ifndef OSENGINE_SERVER_WORLD_SNAPSHOT_HPP
#define OSENGINE_SERVER_WORLD_SNAPSHOT_HPP

// Spec 28/04 — per-tick read-only view of authoritative world state
// fed into each session's GhostEmitter.
//
// The snapshot is a thin handle around references the listener already
// owns (the SessionTable's Session vector). We pass it as a struct so
// later additions (entity list, projectile list, scores) only require
// touching this header plus the snapshot builder.

#include <cstdint>
#include <vector>

namespace dts_viewer
{

struct Session;

struct ServerWorldSnapshot
{
    // One entry per currently-connected client. The receiving emitter
    // iterates these and emits a ghost record for every entry whose
    // ghost_id != the emitter's owning-session ghost_id (clients
    // simulate their own player locally; they don't need a ghost echo
    // of themselves).
    std::vector<const Session*> players;

    // Monotonic clock value at the moment the snapshot was taken.
    // Lets emitters tag packets with server time so client-side
    // interpolation has a stable reference.
    std::uint64_t server_time_ms = 0;

    // Server-side tick counter. Used by the emitter for rate-throttle
    // ("emit every other tick" without needing a local accumulator).
    std::uint64_t tick = 0;
};

} // namespace dts_viewer

#endif // OSENGINE_SERVER_WORLD_SNAPSHOT_HPP
