#ifndef DTS_VIEWER_AI_PLAYER_HPP
#define DTS_VIEWER_AI_PLAYER_HPP

// Spec 18/02 — AIPlayer SimObject: a Player subclass with a directive
// queue (waypoints + forced target) consumed by the AI tick loop
// (spec 18/03). Variable bag and tick state get added incrementally
// by later specs.

#include "player_simobject.hpp"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

class AIPlayer : public Player
{
    typedef Player Parent;

public:
    struct Waypoint
    {
        std::array<float, 3> pos{};
        int order = 0;
    };

    // Spec 18/02 — directive queue. Waypoints are kept sorted by `order`
    // ascending (DirectiveWaypoint inserts then re-sorts).
    std::vector<Waypoint> mWaypoints;
    int                   mForcedTargetClientId = -1;

    // Spec 18/02 — engine-side name (the script-side registered name).
    // Mirrored from SimObject::getName() so spec 18/03's lookups don't
    // pay the StringTable cost on every tick.
    std::string mAIName;

    // Spec 18/02 — armor key ("larmor" / "marmor" / "harmor") passed to
    // AI::spawn. v1: cached but not interpreted; spec 18/03 reads it
    // for run-speed lookup.
    std::string mArmorKey;

    // Spec 18/03 placeholders (populated by specs 03/04 — kept here so
    // the AIPlayer header is the single source of truth for its state).
    std::unordered_map<std::string, std::string> mVars;
    int   mPathCursor    = 0;
    int   mPathDirection = +1;  // +1 forward, -1 reverse (twoWay paths)
    bool  mAutoTargets   = false;
    int   mCurrentTargetClientId = -1;
    bool  mCurrentTargetLOSHeld  = false;
    int   mLastLostTargetClientId = -1;

    // Spec 18/03 — periodic-callback record. fn_name == "" means no
    // callback. accumulator counts down from `period_sec` each tick;
    // when it reaches 0 the script is fired and the accumulator resets.
    std::string mPeriodicFn;
    float       mPeriodicPeriod      = 0.0f;
    float       mPeriodicAccumulator = 0.0f;

    // Spec 18/03 — live-position cache for the tick loop. The tick
    // reads/writes these directly; spec 18/05 syncs them into mLive
    // (the bridged PlayerState) so the gameplay layer renders them.
    float mTickPos[3] = {0.0f, 0.0f, 0.0f};

    AIPlayer();

    static void initPersistFields();

    DECLARE_CONOBJECT(AIPlayer);
};

namespace dts_viewer {

// Linker anchor — same pattern as anchorPlayerClass().
void anchorAIPlayerClass();

// Spec 18/05 — engine-side mission AI spawn. For each BotPath extracted
// in spec 18/01, creates an AIPlayer (mirror of AI::spawn) and queues
// its waypoints. Returns the number of bots created.
struct BotPath;
int  spawn_bots_from_paths(const std::vector<BotPath>& paths);

// Spec 18/05 — manually trigger the on-death pipeline for `bot`. Fires
// `AI::onDroneKilled(name)` (script side) and unregisters the AIPlayer.
// Designed to be called from Player::kill when the dying object is an
// AIPlayer.
void notify_drone_killed(AIPlayer& bot);

// Spec 18/02 — name → AIPlayer registry. `AI::getId` does the lookup.
// `AI::spawn` inserts; the AIPlayer destructor removes.
AIPlayer* find_ai_player_by_name(const std::string& name);
void      register_ai_player(AIPlayer* p);
void      unregister_ai_player(const std::string& name);

// Spec 18/03 — iterate all currently-spawned AIPlayers (for the tick).
const std::vector<AIPlayer*>& all_ai_players();

} // namespace dts_viewer

#endif // DTS_VIEWER_AI_PLAYER_HPP
