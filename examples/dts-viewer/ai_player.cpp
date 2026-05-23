// Spec 18/02 — AIPlayer impl + the global name registry the AI::*
// engine bindings use.

#include "ai_player.hpp"
#include "bot_paths.hpp"

#include "console/console.h"
#include "console/script.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace {

// The registry lives in this TU so spec 18/03's tick loop can iterate
// it via dts_viewer::all_ai_players().
std::unordered_map<std::string, AIPlayer*>& name_index()
{
    static std::unordered_map<std::string, AIPlayer*> m;
    return m;
}

std::vector<AIPlayer*>& live_list()
{
    static std::vector<AIPlayer*> v;
    return v;
}

} // namespace

AIPlayer::AIPlayer()
{
    // Default Player ctor sets pos/vel/health/etc.
}

void AIPlayer::initPersistFields()
{
    // No new persistent fields for spec 18/02 — directive queue + forced
    // target are not script-readable as fields. Spec 18/03 may add
    // `pathType`, `iq`, etc. as proper TypeS32 fields if scripts read
    // them back via `bot.iq`.
    Parent::initPersistFields();
}

IMPLEMENT_CONOBJECT(AIPlayer);

namespace dts_viewer {

AIPlayer* find_ai_player_by_name(const std::string& name)
{
    auto it = name_index().find(name);
    return it == name_index().end() ? nullptr : it->second;
}

void register_ai_player(AIPlayer* p)
{
    if (!p) return;
    name_index()[p->mAIName] = p;
    live_list().push_back(p);
}

void unregister_ai_player(const std::string& name)
{
    auto it = name_index().find(name);
    if (it == name_index().end()) return;
    AIPlayer* p = it->second;
    name_index().erase(it);
    auto& v = live_list();
    v.erase(std::remove(v.begin(), v.end(), p), v.end());
}

const std::vector<AIPlayer*>& all_ai_players()
{
    return live_list();
}

void anchorAIPlayerClass()
{
    static auto* anchor = AIPlayer::getStaticClassRep();
    (void)anchor;
}

// Spec 18/05 — mirror of AI::spawn for engine-side mission setup.
// Skip the script roundtrip so this works even when ai.cs hasn't been
// sourced (the retail script paths for setupAI differ between multiplayer
// and training; engine-side spawn covers both conventions extracted by
// load_bot_paths).
int spawn_bots_from_paths(const std::vector<BotPath>& paths)
{
    int spawned = 0;
    for (const auto& bp : paths)
    {
        if (find_ai_player_by_name(bp.drone_name)) continue;

        AIPlayer* bot   = new AIPlayer;
        bot->mAIName    = bp.drone_name;
        bot->mArmorKey  = "larmor";    // ai.cs default $AI::defaultArmorType
        bot->mTeam      = bp.team;
        bot->mTickPos[0] = bp.spawn_pos[0];
        bot->mTickPos[1] = bp.spawn_pos[1];
        bot->mTickPos[2] = bp.spawn_pos[2];

        char buf[96];
        std::snprintf(buf, sizeof(buf), "%g %g %g",
                      bp.spawn_pos[0], bp.spawn_pos[1], bp.spawn_pos[2]);
        bot->mPos       = StringTable->insert(buf);
        bot->mDataBlock = StringTable->insert("larmor");

        // Queue waypoints in declared order (createAI uses orderNumber
        // starting at 100, +100 per marker — same effect here).
        for (std::size_t i = 0; i < bp.waypoints.size(); ++i) {
            AIPlayer::Waypoint wp;
            wp.pos   = bp.waypoints[i];
            wp.order = 100 + static_cast<int>(i) * 100;
            bot->mWaypoints.push_back(wp);
        }
        bot->mVars["pathType"] = "2";  // $AI::defaultPathType
        bot->mVars["iq"]       = "60";
        bot->mVars["attackMode"] = "1";

        if (!bot->registerObject(bp.drone_name.c_str())) {
            Con::errorf("spawn_bots_from_paths: registerObject(\"%s\") failed",
                        bp.drone_name.c_str());
            delete bot;
            continue;
        }
        register_ai_player(bot);
        ++spawned;
    }
    return spawned;
}

void notify_drone_killed(AIPlayer& bot)
{
    // Fire script-side AI::onDroneKilled callback. Retail ai.cs:L386
    // schedules AI::setupAI(name, team) after 8s for respawn.
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "AI::onDroneKilled(\"%s\");",
                  bot.mAIName.c_str());
    Con::evaluate(cmd, false, "AI::death");

    // Engine-side cleanup — remove from the live list so the tick loop
    // skips it. The SimObject is left in Sim's registry so script-side
    // lookups by id still return something until the respawn handler
    // cleans up.
    unregister_ai_player(bot.mAIName);
    // Stop any pending periodic callback.
    bot.mPeriodicFn.clear();
    bot.mWaypoints.clear();
}

} // namespace dts_viewer
