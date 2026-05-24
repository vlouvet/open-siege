#include <osengine/match_state.hpp>

#include <osengine/session_table.hpp>

#include <cstdio>

namespace dts_viewer
{

MatchState::MatchState(MatchConfig cfg) : cfg_(cfg) {}

std::size_t MatchState::count_active_players(const SessionTable& sessions) const
{
    auto& mut = const_cast<SessionTable&>(sessions);
    std::size_t n = 0;
    for (auto* s : mut.active_sessions()) {
        if (s && s->team != Team::Spectator) ++n;
    }
    return n;
}

std::uint64_t MatchState::time_remaining_ms(std::uint64_t now_ms) const noexcept
{
    if (phase_ != MatchPhase::Live) return 0;
    const std::uint64_t elapsed = now_ms - live_started_ms_;
    if (elapsed >= cfg_.time_limit_ms) return 0;
    return cfg_.time_limit_ms - elapsed;
}

Team MatchState::winner() const noexcept
{
    if (phase_ != MatchPhase::EndHold) return Team::Spectator;
    return winner_;
}

void MatchState::on_capture(const CaptureEvent& e, std::uint64_t /*now_ms*/)
{
    if (phase_ != MatchPhase::Live) return;        // Warmup / EndHold dropped
    // The flag_taken in CaptureEvent is the team whose flag was carried;
    // credit the OPPOSITE team's score.
    if (e.flag_taken == Team::Red)       ++blue_score_;
    else if (e.flag_taken == Team::Blue) ++red_score_;
    std::fprintf(stderr,
        "[score] slot %u capped %s flag — Red %u | Blue %u\n",
        (unsigned)e.capturer_slot,
        (e.flag_taken == Team::Red ? "Red" : "Blue"),
        (unsigned)red_score_, (unsigned)blue_score_);
}

void MatchState::transition_to_end_hold(std::uint64_t now_ms)
{
    if (red_score_ > blue_score_)      winner_ = Team::Red;
    else if (blue_score_ > red_score_) winner_ = Team::Blue;
    else                                winner_ = Team::Spectator;
    phase_          = MatchPhase::EndHold;
    end_started_ms_ = now_ms;
    const char* wname = (winner_ == Team::Red) ? "Red"
                      : (winner_ == Team::Blue) ? "Blue"
                      : "DRAW";
    std::fprintf(stderr,
        "[match] OVER — winner=%s Red=%u Blue=%u  (hold %u ms before reset)\n",
        wname, (unsigned)red_score_, (unsigned)blue_score_,
        (unsigned)cfg_.end_hold_ms);
}

void MatchState::tick(const SessionTable& sessions, std::uint64_t now_ms)
{
    const std::size_t n_players = count_active_players(sessions);
    switch (phase_) {
        case MatchPhase::Warmup: {
            if (n_players >= cfg_.min_players) {
                phase_           = MatchPhase::Live;
                live_started_ms_ = now_ms;
                red_score_       = 0;
                blue_score_      = 0;
                std::fprintf(stderr,
                    "[match] LIVE — %zu players, time-limit %u ms, cap-limit %u\n",
                    n_players, (unsigned)cfg_.time_limit_ms,
                    (unsigned)cfg_.cap_limit);
            }
            break;
        }
        case MatchPhase::Live: {
            if (n_players < cfg_.min_players) {
                // Drop back to Warmup; scores reset on next Live entry.
                phase_ = MatchPhase::Warmup;
                std::fprintf(stderr,
                    "[match] WARMUP — only %zu players (need %u)\n",
                    n_players, (unsigned)cfg_.min_players);
                return;
            }
            if (red_score_ >= cfg_.cap_limit
                || blue_score_ >= cfg_.cap_limit
                || (now_ms - live_started_ms_) >= cfg_.time_limit_ms)
            {
                transition_to_end_hold(now_ms);
            }
            break;
        }
        case MatchPhase::EndHold: {
            if (now_ms - end_started_ms_ >= cfg_.end_hold_ms) {
                phase_      = MatchPhase::Warmup;
                red_score_  = 0;
                blue_score_ = 0;
                winner_     = Team::Spectator;
                std::fputs("[match] WARMUP — end-hold elapsed, reset\n", stderr);
            }
            break;
        }
    }
}

int MatchState::selftest()
{
    // Common scaffolding: 2 sessions, both Red/Blue, populate in place.
    auto populate = [](SessionTable& t) {
        const std::uint8_t n[3] = {1,2,3};
        Session* a = t.allocate({"127.0.0.1", 63001}, n, 0);
        Session* b = t.allocate({"127.0.0.1", 63002}, n, 0);
        a->team = Team::Red;
        b->team = Team::Blue;
    };

    // Test 1 — cap-limit win.
    {
        MatchState ms(MatchConfig{/*cap*/2, /*time*/60'000, /*end_hold*/100});
        SessionTable t(4);
        populate(t);
        ms.tick(t, 1000);        // warmup → live
        if (ms.phase() != MatchPhase::Live) {
            std::fputs("[match-selftest] expected Live after 2 players\n", stderr);
            return 1;
        }
        CaptureEvent e1{0, Team::Blue};   // Red captured Blue's flag → Red +1
        CaptureEvent e2{0, Team::Blue};   // again → Red +1, cap-limit hit
        ms.on_capture(e1, 1100);
        ms.on_capture(e2, 1200);
        ms.tick(t, 1300);
        if (ms.phase() != MatchPhase::EndHold) {
            std::fprintf(stderr, "[match-selftest] expected EndHold after cap-limit, got %u\n",
                         (unsigned)ms.phase());
            return 1;
        }
        if (ms.winner() != Team::Red) {
            std::fputs("[match-selftest] expected Red winner\n", stderr);
            return 1;
        }
        // After end_hold (100 ms), drops back to Warmup.
        ms.tick(t, 1400 + 200);
        if (ms.phase() != MatchPhase::Warmup) {
            std::fputs("[match-selftest] expected Warmup after end-hold\n", stderr);
            return 1;
        }
        if (ms.red_score() != 0 || ms.blue_score() != 0) {
            std::fputs("[match-selftest] scores not reset on warmup return\n", stderr);
            return 1;
        }
    }

    // Test 2 — time-limit expiry with Blue ahead.
    {
        MatchState ms(MatchConfig{/*cap*/10, /*time*/100, /*end_hold*/100});
        SessionTable t(4);
        populate(t);
        ms.tick(t, 0);
        CaptureEvent e{0, Team::Red};     // Blue captured Red flag → Blue +1
        ms.on_capture(e, 50);
        ms.tick(t, 250);                  // > 100 ms after live start
        if (ms.phase() != MatchPhase::EndHold) {
            std::fputs("[match-selftest] expected EndHold after time-limit\n", stderr);
            return 1;
        }
        if (ms.winner() != Team::Blue) {
            std::fprintf(stderr, "[match-selftest] expected Blue winner, got %u\n",
                         (unsigned)ms.winner());
            return 1;
        }
    }

    // Test 3 — warmup with 1 player.
    {
        MatchState ms(MatchConfig{});
        SessionTable t(4);
        const std::uint8_t n[3] = {0,0,0};
        Session* solo = t.allocate({"127.0.0.1", 63003}, n, 0);
        solo->team = Team::Red;
        ms.tick(t, 0);
        if (ms.phase() != MatchPhase::Warmup) {
            std::fputs("[match-selftest] expected Warmup with 1 player\n", stderr);
            return 1;
        }
        // A capture during Warmup is dropped.
        ms.on_capture(CaptureEvent{0, Team::Blue}, 100);
        if (ms.red_score() != 0) {
            std::fputs("[match-selftest] warmup capture should not score\n", stderr);
            return 1;
        }
    }

    // Test 4 — drop back to Warmup if a player leaves mid-Live.
    {
        MatchState ms(MatchConfig{});
        SessionTable t(4);
        populate(t);
        ms.tick(t, 0);
        if (ms.phase() != MatchPhase::Live) {
            std::fputs("[match-selftest] T4 expected Live\n", stderr); return 1;
        }
        // Simulate one player leaving: drop one session from the table.
        t.drop({"127.0.0.1", 63002});
        ms.tick(t, 100);
        if (ms.phase() != MatchPhase::Warmup) {
            std::fputs("[match-selftest] T4 expected Warmup after player drop\n", stderr);
            return 1;
        }
    }

    std::fputs("[match-selftest] OK — cap, time, warmup, drop-out\n", stderr);
    return 0;
}

} // namespace dts_viewer
