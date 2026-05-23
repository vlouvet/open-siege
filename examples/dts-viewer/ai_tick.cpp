// Spec 18/03 — AI tick: walk every AIPlayer toward its current waypoint
// per its pathType, decrement periodic-callback timers and fire scripts.

#include "ai_tick.hpp"
#include "ai_player.hpp"
#include "ai_perception.hpp"

#include "console/console.h"
#include "console/script.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr float kWaypointReachedDist = 2.0f;        // XY-plane metres
constexpr float kDefaultRunSpeed     = 7.0f;        // m/s (larmor)
constexpr float kRunSpeedHarmor      = 5.0f;        // m/s (heavy is slower)

float run_speed_for(const std::string& armor)
{
    if (armor == "harmor") return kRunSpeedHarmor;
    if (armor == "marmor") return (kDefaultRunSpeed + kRunSpeedHarmor) * 0.5f;
    return kDefaultRunSpeed; // larmor + unknown
}

int read_path_type(const AIPlayer& bot)
{
    auto it = bot.mVars.find("pathType");
    if (it == bot.mVars.end()) return 2; // ai.cs L21 default: twoWay
    return std::atoi(it->second.c_str());
}

// Advance the cursor by direction. Respects pathType:
//   0 circular  — wrap to 0 after last
//   1 oneWay    — stop at last
//   2 twoWay    — reverse direction at endpoints
void advance_waypoint(AIPlayer& bot)
{
    const int N = static_cast<int>(bot.mWaypoints.size());
    if (N <= 1) return;
    const int pt = read_path_type(bot);

    int next = bot.mPathCursor + bot.mPathDirection;
    if (pt == 0) {
        if (next >= N) next = 0;
        if (next < 0) next = N - 1;
    } else if (pt == 1) {
        if (next >= N) next = N - 1;
        if (next < 0) next = 0;
    } else { // twoWay
        if (next >= N) { bot.mPathDirection = -1; next = N - 2; }
        if (next < 0)  { bot.mPathDirection = +1; next = 1;     }
    }
    bot.mPathCursor = next;
}

void tick_one(AIPlayer& bot, float dt)
{
    // Spec 18/04 — perception first, so target callbacks fire before
    // any movement reacts to them.
    dts_viewer::run_perception_tick(bot);

    // Periodic callback next — fires even if no waypoints.
    if (!bot.mPeriodicFn.empty() && bot.mPeriodicPeriod > 0.0f) {
        bot.mPeriodicAccumulator -= dt;
        if (bot.mPeriodicAccumulator <= 0.0f) {
            // Fire `periodicFn(\"botName\");`
            char cmd[256];
            std::snprintf(cmd, sizeof(cmd), "%s(\"%s\");",
                          bot.mPeriodicFn.c_str(), bot.mAIName.c_str());
            Con::evaluate(cmd, false, "AI::periodic");
            bot.mPeriodicAccumulator = bot.mPeriodicPeriod;
        }
    }

    if (bot.mWaypoints.empty()) return;
    if (bot.mPathCursor < 0 ||
        bot.mPathCursor >= static_cast<int>(bot.mWaypoints.size())) {
        bot.mPathCursor = 0;
    }

    const auto& wp = bot.mWaypoints[bot.mPathCursor].pos;
    const float dx = wp[0] - bot.mTickPos[0];
    const float dy = wp[1] - bot.mTickPos[1];
    const float xy_dist = std::sqrt(dx * dx + dy * dy);

    if (xy_dist < kWaypointReachedDist) {
        // Snap onto the waypoint and try to advance. If advance_waypoint
        // leaves cursor unchanged (oneWay terminal), keep position locked
        // there — no oscillation.
        const int before = bot.mPathCursor;
        bot.mTickPos[0] = wp[0];
        bot.mTickPos[1] = wp[1];
        bot.mTickPos[2] = wp[2];
        advance_waypoint(bot);
        if (bot.mPathCursor == before) {
            // Terminal — refresh the StringTable mirror and stop.
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%g %g %g",
                          bot.mTickPos[0], bot.mTickPos[1], bot.mTickPos[2]);
            bot.mPos = StringTable->insert(buf);
        }
        return;
    }

    const float speed = run_speed_for(bot.mArmorKey);
    const float step  = speed * dt;
    if (step >= xy_dist) {
        bot.mTickPos[0] = wp[0];
        bot.mTickPos[1] = wp[1];
        bot.mTickPos[2] = wp[2];
    } else {
        const float k = step / xy_dist;
        bot.mTickPos[0] += dx * k;
        bot.mTickPos[1] += dy * k;
        // Z follows waypoint Z at the same proportion — good enough for
        // sloped terrain; real physics integration is out of v1 scope.
        bot.mTickPos[2] += (wp[2] - bot.mTickPos[2]) * k;
    }

    // Mirror back into the StringTable-backed mPos so script-side reads
    // (Hero.pos) reflect tick-driven motion.
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%g %g %g",
                  bot.mTickPos[0], bot.mTickPos[1], bot.mTickPos[2]);
    bot.mPos = StringTable->insert(buf);
}

} // namespace

namespace dts_viewer {

void ai_tick_all(float dt_seconds)
{
    if (dt_seconds <= 0.0f) return;
    // Iterate by index so a script callback that spawns/kills a bot
    // mid-tick can't invalidate the iterator under us.
    const auto& live = all_ai_players();
    for (std::size_t i = 0; i < live.size(); ++i)
        tick_one(*live[i], dt_seconds);
}

} // namespace dts_viewer
