#ifndef OSENGINE_MATCH_STATE_HPP
#define OSENGINE_MATCH_STATE_HPP

// Spec 28/09 — match scoring + match-end transitions.
//
// Owns the Red/Blue score and the match phase (Warmup → Live →
// EndHold). Drains CaptureEvents from spec 28/08 to credit captures
// to the right team. Tick() drives the phase machine: Warmup waits
// for ≥ 2 connected players; Live begins the timer; EndHold fires
// when cap_limit or time_limit hits and holds for end_hold_ms before
// returning to Warmup (spec 28/11 will pick up to cycle the map).

#include <osengine/flag_state.hpp>
#include <osengine/session_table.hpp>

#include <cstdint>

namespace dts_viewer
{

struct MatchConfig
{
    std::uint16_t cap_limit       = 5;
    std::uint32_t time_limit_ms   = 25u * 60u * 1000u;
    std::uint32_t end_hold_ms     = 10u * 1000u;
    std::uint8_t  min_players     = 2;
};

enum class MatchPhase : std::uint8_t {
    Warmup   = 0,
    Live     = 1,
    EndHold  = 2,
};

class MatchState
{
public:
    explicit MatchState(MatchConfig cfg);

    void on_capture(const CaptureEvent& e, std::uint64_t now_ms);

    // Drive the phase machine. Must be called every server tick.
    void tick(const SessionTable& sessions, std::uint64_t now_ms);

    MatchPhase    phase()      const noexcept { return phase_; }
    std::uint16_t red_score()  const noexcept { return red_score_; }
    std::uint16_t blue_score() const noexcept { return blue_score_; }
    std::uint64_t live_started_ms() const noexcept { return live_started_ms_; }
    std::uint64_t time_remaining_ms(std::uint64_t now_ms) const noexcept;

    // Spectator means draw or not-yet-over.
    Team winner() const noexcept;

    const MatchConfig& config() const noexcept { return cfg_; }

    static int selftest();

private:
    MatchConfig   cfg_;
    MatchPhase    phase_           = MatchPhase::Warmup;
    std::uint16_t red_score_       = 0;
    std::uint16_t blue_score_      = 0;
    std::uint64_t live_started_ms_ = 0;
    std::uint64_t end_started_ms_  = 0;
    Team          winner_          = Team::Spectator;

    std::size_t count_active_players(const SessionTable& sessions) const;
    void        transition_to_end_hold(std::uint64_t now_ms);
};

} // namespace dts_viewer

#endif // OSENGINE_MATCH_STATE_HPP
