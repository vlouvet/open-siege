#ifndef OSENGINE_SCOREBOARD_HPP
#define OSENGINE_SCOREBOARD_HPP

// Spec 28/10 — scoreboard snapshot builder. Pure derived view of
// SessionTable + MatchState; no state of its own.

#include <osengine/session_table.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace dts_viewer
{

class MatchState;

struct ScoreboardRow
{
    std::uint16_t slot     = 0;
    Team          team     = Team::Spectator;
    std::string   name;          // v1: "Player N"
    std::uint16_t kills    = 0;
    std::uint16_t deaths   = 0;
    std::uint16_t captures = 0;  // per-session capture count (placeholder; not yet tracked)
    std::uint16_t ping_ms  = 0;  // 0 = unknown
};

std::vector<ScoreboardRow> build_scoreboard(const SessionTable& sessions,
                                            const MatchState&   match);

int scoreboard_selftest();

} // namespace dts_viewer

#endif // OSENGINE_SCOREBOARD_HPP
