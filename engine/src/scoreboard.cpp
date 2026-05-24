#include <osengine/scoreboard.hpp>

#include <osengine/match_state.hpp>
#include <osengine/session_table.hpp>

#include <algorithm>
#include <cstdio>

namespace dts_viewer
{

std::vector<ScoreboardRow> build_scoreboard(const SessionTable& sessions,
                                            const MatchState&   /*match*/)
{
    auto& mut = const_cast<SessionTable&>(sessions);
    std::vector<ScoreboardRow> rows;
    for (auto* s : mut.active_sessions()) {
        if (!s) continue;
        ScoreboardRow r;
        r.slot   = s->player_slot;
        r.team   = s->team;
        r.name   = "Player " + std::to_string(s->player_slot);
        r.kills  = s->kills;
        r.deaths = s->deaths;
        // Per-session capture count is tracked alongside kills in
        // 28/09's MatchState aggregate; per-session it's a future
        // attribution problem. v1: leave 0.
        r.captures = 0;
        r.ping_ms  = 0;            // RTT smoothing landed in 28/01 follow-on
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(),
        [](const ScoreboardRow& a, const ScoreboardRow& b) {
            if (a.team != b.team) return static_cast<int>(a.team) < static_cast<int>(b.team);
            return a.slot < b.slot;
        });
    return rows;
}

int scoreboard_selftest()
{
    SessionTable table(4);
    const std::uint8_t n[3] = { 1, 2, 3 };
    Session* s1 = table.allocate({"127.0.0.1", 65001}, n, 0);
    Session* s2 = table.allocate({"127.0.0.1", 65002}, n, 0);
    Session* s3 = table.allocate({"127.0.0.1", 65003}, n, 0);
    s1->team = Team::Red;   s1->kills = 2; s1->deaths = 1;
    s2->team = Team::Blue;  s2->kills = 1; s2->deaths = 3;
    s3->team = Team::Blue;  s3->kills = 0; s3->deaths = 0;

    MatchState ms(MatchConfig{});
    auto rows = build_scoreboard(table, ms);
    if (rows.size() != 3) {
        std::fprintf(stderr, "[scoreboard-selftest] expected 3 rows got %zu\n",
                     rows.size());
        return 1;
    }
    // Sorted: Red (s1) first, then Blue by slot (s2, s3).
    if (rows[0].team != Team::Red || rows[0].slot != s1->player_slot
        || rows[0].kills != 2 || rows[0].deaths != 1) {
        std::fputs("[scoreboard-selftest] row 0 mismatch\n", stderr); return 1;
    }
    if (rows[1].team != Team::Blue || rows[1].kills != 1) {
        std::fputs("[scoreboard-selftest] row 1 mismatch\n", stderr); return 1;
    }
    if (rows[2].team != Team::Blue || rows[2].kills != 0) {
        std::fputs("[scoreboard-selftest] row 2 mismatch\n", stderr); return 1;
    }
    if (rows[0].name != "Player " + std::to_string(s1->player_slot)) {
        std::fprintf(stderr, "[scoreboard-selftest] name mismatch: %s\n",
                     rows[0].name.c_str());
        return 1;
    }
    std::fputs("[scoreboard-selftest] OK — 3 rows, team-sorted, kill/death pass-through\n",
               stderr);
    return 0;
}

} // namespace dts_viewer
