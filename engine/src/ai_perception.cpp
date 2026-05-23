// Spec 18/04 — perception engine + LOS-callback dispatch.

#include "ai_perception.hpp"
#include "ai_player.hpp"

#include "console/console.h"
#include "console/script.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace dts_viewer {

namespace {

float dist3d(const float a[3], const float b[3])
{
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void fire_script_cb(const char* fn, const std::string& name, int idNum)
{
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "%s(\"%s\", %d);", fn, name.c_str(), idNum);
    Con::evaluate(cmd, false, "AI::perception");
}

} // namespace

bool has_los(const AIPlayer& bot, const AIPlayer& target, float max_dist)
{
    // v1: distance gate only. Terrain ray-cast hooks here in a follow-up.
    const float d = dist3d(bot.mTickPos, target.mTickPos);
    return d <= max_dist;
}

AIPlayer* find_best_target(const AIPlayer& bot)
{
    AIPlayer* best = nullptr;
    float best_dist = std::numeric_limits<float>::infinity();
    for (auto* candidate : all_ai_players())
    {
        if (candidate == &bot) continue;
        // Hostile = different team. Treat team 255 (observer) as neutral.
        if (candidate->mTeam == bot.mTeam) continue;
        if (candidate->mTeam == 255 || bot.mTeam == 255) continue;
        if (!has_los(bot, *candidate)) continue;
        const float d = dist3d(bot.mTickPos, candidate->mTickPos);
        if (d < best_dist) { best_dist = d; best = candidate; }
    }
    return best;
}

void run_perception_tick(AIPlayer& bot)
{
    if (!bot.mAutoTargets) return;

    // A forced target (from AI::DirectiveTarget) overrides auto-targeting.
    if (bot.mForcedTargetClientId >= 0) {
        bot.mCurrentTargetClientId = bot.mForcedTargetClientId;
        bot.mCurrentTargetLOSHeld  = true;
        return;
    }

    AIPlayer* new_target = find_best_target(bot);
    const int new_id     = new_target ? static_cast<int>(new_target->getId())
                                      : -1;
    const bool new_los   = (new_target != nullptr);

    if (new_id != bot.mCurrentTargetClientId)
    {
        // Target changed. Was the old one alive? (We don't have a real
        // death event yet; for v1 a target whose AIPlayer disappears
        // from all_ai_players() is treated as dead.)
        const int old = bot.mCurrentTargetClientId;
        if (old >= 0)
        {
            // If the old SimObject still exists, fire LOSLost; otherwise
            // fire onTargetDied.
            SimObject* old_obj = Sim::findObject(old);
            if (old_obj) {
                fire_script_cb("AI::onTargetLOSLost", bot.mAIName, old);
                bot.mLastLostTargetClientId = old;
            } else {
                fire_script_cb("AI::onTargetDied", bot.mAIName, old);
                bot.mLastLostTargetClientId = -1;
            }
        }

        if (new_target)
        {
            const char* cb = (new_id == bot.mLastLostTargetClientId)
                ? "AI::onTargetLOSRegained"
                : "AI::onTargetLOSAcquired";
            fire_script_cb(cb, bot.mAIName, new_id);
            if (new_id == bot.mLastLostTargetClientId)
                bot.mLastLostTargetClientId = -1;
        }

        bot.mCurrentTargetClientId = new_id;
        bot.mCurrentTargetLOSHeld  = new_los;
        return;
    }

    // Same target id, LOS state changed.
    if (new_los != bot.mCurrentTargetLOSHeld)
    {
        if (new_los) {
            fire_script_cb("AI::onTargetLOSRegained", bot.mAIName, new_id);
        } else {
            fire_script_cb("AI::onTargetLOSLost", bot.mAIName, new_id);
            bot.mLastLostTargetClientId = new_id;
        }
        bot.mCurrentTargetLOSHeld = new_los;
    }
}

} // namespace dts_viewer
