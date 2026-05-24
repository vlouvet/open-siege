#ifndef OSENGINE_TEAM_ASSIGNER_HPP
#define OSENGINE_TEAM_ASSIGNER_HPP

// Spec 28/05 — team membership + spawn placement.
//
// Walks the mission's scene graph for "DropPoints" SimGroup containers,
// classifies each Marker child by data_block name as Red / Blue /
// Neutral, then assigns incoming sessions to the team with fewer
// members (round-robin balance). Each session's spawn_pos /
// spawn_yaw / player_state.pos are populated from the chosen marker.

#include "content/mission/scene.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace dts_viewer
{

struct LoadedMission;
class  SessionTable;
struct Session;
enum class Team : std::uint8_t;

struct SpawnPoint
{
    glm::vec3 pos{0.0f};
    float     yaw_radians = 0.0f;
    Team      team        = Team{0};      // Spectator means neutral DM marker
    std::string marker_name;
};

// Walk the scene graph, extract every Marker living under a DropPoints
// SimGroup, classify the team from the data_block name. Markers whose
// data_block name contains "team1"/"red" map to Red; "team2"/"blue"
// map to Blue; everything else is neutral (Spectator team value).
std::vector<SpawnPoint> extract_spawn_points(const LoadedMission& mission);

// Pick a team for a freshly-connected session. round_robin = true
// balances Red/Blue by member count; false always picks Red.
Team pick_team(const SessionTable& sessions, bool round_robin);

// Place `session` at a spawn point matching its team. If no team-matched
// marker exists, falls back to any neutral marker, then to a synthesised
// (0, 0, 0) position. Updates session.spawn_pos / spawn_yaw /
// player_state.pos / player_state.yaw.
void place_at_spawn(Session& session,
                    const std::vector<SpawnPoint>& spawns);

// Re-pick the spawn slot (e.g. on respawn). Same logic as
// place_at_spawn — exists as a named entry point so the respawn cycle
// (spec 28/07) reads cleanly.
void respawn(Session& session, const std::vector<SpawnPoint>& spawns);

// Selftest — synthesises 4 fake spawn points (2 Red, 2 Blue), allocates
// 4 sessions through a SessionTable, runs pick_team + place_at_spawn,
// asserts the distribution is 2/2 and each session.player_state.pos
// matches a spawn point.
int team_assigner_selftest();

} // namespace dts_viewer

#endif // OSENGINE_TEAM_ASSIGNER_HPP
