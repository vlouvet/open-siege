#ifndef OSENGINE_WORLD_TICK_HPP
#define OSENGINE_WORLD_TICK_HPP

// Server-authoritative world tick — spec 28/03.
//
// Walks every active Session in a SessionTable, drains its
// pending_moves queue, and calls the existing player_controller's
// player_update() for each move. Updates session.player_state in
// place; the snapshot is then read by spec 28/04's ghost emitter.

#include <osengine/height_sampler.hpp>
#include <osengine/player_controller.hpp>

namespace dts_viewer
{

class SessionTable;
struct MissionBounds;

struct WorldTickContext
{
    HeightSampler        terrain;        // default-constructed = no terrain
    const MissionBounds* bounds = nullptr;
    PlayerTuning         tuning;         // default tuning unless caller overrides

    // Cap moves applied per session per tick. The client emits at ~33 Hz
    // and the server ticks at 32 Hz, so steady-state depth is 1; this
    // bound just prevents a misbehaving / catching-up client from
    // saturating one server tick.
    int max_moves_per_tick = 4;
};

// Drain pending_moves for every session and advance their PlayerState.
// dt_sec is the per-move integration step (typically 1/32 == 0.03125s).
void world_tick(SessionTable& sessions,
                const WorldTickContext& ctx,
                float dt_sec);

} // namespace dts_viewer

#endif // OSENGINE_WORLD_TICK_HPP
