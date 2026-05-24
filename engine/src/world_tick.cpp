#include <osengine/world_tick.hpp>

#include <osengine/session_table.hpp>

namespace dts_viewer
{

namespace
{

// Translate one net20::MoveInput into the player_controller InputState.
// Axes come in as analog [0,1]; the controller's InputState is boolean
// (Tribes treats movement as digital under the hood). We binarize at
// 0.5 — clients normally send 0.0 or 1.0 anyway.
InputState translate_move(const net20::MoveInput& m)
{
    InputState in{};
    in.fwd    = m.forward  >= 0.5f;
    in.back   = m.backward >= 0.5f;
    in.left   = m.left     >= 0.5f;
    in.right  = m.right    >= 0.5f;
    in.jump   = m.jump;
    in.jet    = m.jet;
    in.sprint = false;        // no sprint bit in net20::MoveInput today
    // The controller expects pixel deltas (it multiplies by tuning's
    // mouse_sens). The client sends radian deltas via yaw/pitch_delta.
    // For server-authority we want the controller to APPLY the radian
    // delta directly. Bypass the mouse_sens conversion by populating
    // a "pixel" value that, after the controller's scale, lands on the
    // exact radian delta. Cleanest path: write yaw/pitch directly after
    // player_update returns (see apply_view below).
    in.mouse_dx = 0.0f;
    in.mouse_dy = 0.0f;
    return in;
}

void apply_view(PlayerState& ps, const net20::MoveInput& m)
{
    ps.yaw   += m.yaw_delta;
    ps.pitch += m.pitch_delta;
    // Clamp pitch to ~+/- 89 deg so the camera never inverts.
    constexpr float kPitchLimit = 1.55f;
    if (ps.pitch >  kPitchLimit) ps.pitch =  kPitchLimit;
    if (ps.pitch < -kPitchLimit) ps.pitch = -kPitchLimit;
}

} // anonymous namespace

void world_tick(SessionTable& sessions,
                const WorldTickContext& ctx,
                float dt_sec)
{
    auto active = sessions.active_sessions();
    for (Session* s : active) {
        int applied = 0;
        while (!s->pending_moves.empty() && applied < ctx.max_moves_per_tick) {
            const net20::MoveInput m = s->pending_moves.front();
            s->pending_moves.pop_front();

            // Apply view delta first so the resulting yaw drives movement.
            apply_view(s->player_state, m);

            const InputState in = translate_move(m);
            player_update(s->player_state, ctx.tuning, in,
                          ctx.terrain, ctx.bounds, dt_sec);

            s->last_applied_move_seq += 1;
            ++applied;
        }
    }
}

} // namespace dts_viewer
