// Spec 18/02 — AI::* engine bindings the retail ai.cs + Training_AI.cs
// scripts depend on.
//
//   AI::spawn(name, armor, pos, rot, displayName, modelKey) -> "true"/"false"
//   AI::getId(name) -> int (object id, -1 if not found)
//   AI::DirectiveWaypoint(name, pos, orderNumber) -> void
//   AI::DirectiveTarget(name, targetClientId) -> void
//   AI::DirectiveTargetLaser(aiId, targetClientId) -> void
//
// Pattern follows hud_bindings.cpp: free functions registered via the
// console addCommand surface with the "AI" namespace.

#include "ai_bindings.hpp"
#include "ai_player.hpp"

#include "console/console.h"
#include "console/consoleInternal.h"
#include "console/sim.h"
#include "console/simObject.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void parseVec3(const char* xyz, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (!xyz) return;
    const char* p = xyz;
    for (int i = 0; i < 3; ++i) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        out[i] = static_cast<float>(std::strtod(p, const_cast<char**>(&p)));
    }
}

// ---------------------------------------------------------------------------
// AI::spawn(name, armor, pos, rot, displayName, modelKey)
// Returns "true" on success, "false" on failure (mirrors retail behaviour
// — ai.cs L49 tests against the literal "false" string).
// ---------------------------------------------------------------------------
const char* ai_spawn_cb(SimObject*, S32 argc, ConsoleValue* argv)
{
    if (argc < 5) {
        Con::errorf("AI::spawn: need at least 4 args (name, armor, pos, rot)");
        return "false";
    }
    const char* name        = argv[1].getString();
    const char* armor       = argv[2].getString();
    const char* pos         = argv[3].getString();
    const char* rot         = argv[4].getString();
    // argv[5] = displayName (optional, ignored for v1)
    // argv[6] = modelKey    (optional, ignored for v1)

    if (!name || !*name) {
        Con::errorf("AI::spawn: empty name");
        return "false";
    }

    // Defensive: refuse to clobber a live bot with the same name. ai.cs
    // respawn path always calls createAI AFTER onDroneKilled clears the
    // entry, so collision means caller is broken.
    if (dts_viewer::find_ai_player_by_name(name)) {
        Con::warnf("AI::spawn: \"%s\" already exists — refusing to clobber", name);
        return "false";
    }

    AIPlayer* bot = new AIPlayer;
    bot->mAIName   = name;
    bot->mArmorKey = (armor && *armor) ? armor : "larmor";
    bot->mPos      = StringTable->insert(pos ? pos : "0 0 0");
    bot->mVel      = StringTable->insert("0 0 0");
    bot->mDataBlock = StringTable->insert(bot->mArmorKey.c_str());
    (void)rot;  // v1: rotation kept on the spawn marker; physics owns the
                // live rotation. Spec 18/03 hooks it into mLive when set.

    if (!bot->registerObject(name)) {
        Con::errorf("AI::spawn: registerObject(\"%s\") failed", name);
        delete bot;
        return "false";
    }
    dts_viewer::register_ai_player(bot);
    return "true";
}

// ---------------------------------------------------------------------------
// AI::getId(name) — returns the SimObject id (positive int) or -1.
// ---------------------------------------------------------------------------
S32 ai_getid_cb(SimObject*, S32 argc, ConsoleValue* argv)
{
    if (argc < 2) return -1;
    const char* name = argv[1].getString();
    if (!name || !*name) return -1;
    AIPlayer* bot = dts_viewer::find_ai_player_by_name(name);
    return bot ? S32(bot->getId()) : -1;
}

// ---------------------------------------------------------------------------
// Helper — find an AIPlayer either by name (string arg) or by SimObject id
// (numeric arg). Used by DirectiveTargetLaser which takes a numeric id.
// ---------------------------------------------------------------------------
AIPlayer* resolve_ai(const ConsoleValue& v)
{
    const char* s = v.getString();
    if (s && *s && !(s[0] >= '0' && s[0] <= '9'))
        return dts_viewer::find_ai_player_by_name(s);
    SimObject* obj = Sim::findObject(v.getInt());
    return dynamic_cast<AIPlayer*>(obj);
}

// ---------------------------------------------------------------------------
// AI::DirectiveWaypoint(name, pos, order) — append + re-sort by order.
// ---------------------------------------------------------------------------
void ai_directive_waypoint_cb(SimObject*, S32 argc, ConsoleValue* argv)
{
    if (argc < 4) {
        Con::errorf("AI::DirectiveWaypoint: need (name, pos, orderNumber)");
        return;
    }
    AIPlayer* bot = resolve_ai(argv[1]);
    if (!bot) {
        Con::warnf("AI::DirectiveWaypoint: no AI named \"%s\"",
                   argv[1].getString());
        return;
    }
    AIPlayer::Waypoint wp;
    parseVec3(argv[2].getString(), wp.pos.data());
    wp.order = static_cast<int>(argv[3].getInt());
    bot->mWaypoints.push_back(wp);
    std::stable_sort(bot->mWaypoints.begin(), bot->mWaypoints.end(),
        [](const AIPlayer::Waypoint& a, const AIPlayer::Waypoint& b) {
            return a.order < b.order;
        });
}

// ---------------------------------------------------------------------------
// AI::DirectiveTarget(name, clientId) — sets/clears forced target.
// AI::DirectiveTargetLaser(aiId, clientId) — same but takes numeric aiId.
// ---------------------------------------------------------------------------
void ai_directive_target_cb(SimObject*, S32 argc, ConsoleValue* argv)
{
    if (argc < 3) return;
    AIPlayer* bot = resolve_ai(argv[1]);
    if (!bot) return;
    bot->mForcedTargetClientId = static_cast<int>(argv[2].getInt());
}

void ai_directive_targetlaser_cb(SimObject*, S32 argc, ConsoleValue* argv)
{
    // Identical behaviour to DirectiveTarget for v1 — the laser-vs-target
    // distinction is a script-side concern (ai.cs uses DirectiveTargetLaser
    // for laser-painting / mortar guidance).
    ai_directive_target_cb(nullptr, argc, argv);
}

} // namespace

namespace dts_viewer {

void anchorAIBindings()
{
    // Idempotent: addCommand replaces existing registrations under the
    // same (namespace, name), so re-anchoring is safe.
    static bool once = false;
    if (once) return;
    once = true;

    Con::addCommand("AI", "spawn",                 ai_spawn_cb,
                    "(name, armor, pos, rot, displayName, modelKey) — spawn a bot",
                    5, 7);
    Con::addCommand("AI", "getId",                 ai_getid_cb,
                    "(name) — return the bot's object id or -1",
                    2, 2);
    Con::addCommand("AI", "DirectiveWaypoint",     ai_directive_waypoint_cb,
                    "(name, pos, orderNumber) — queue a waypoint",
                    4, 4);
    Con::addCommand("AI", "DirectiveTarget",       ai_directive_target_cb,
                    "(name, clientId) — force target",
                    3, 3);
    Con::addCommand("AI", "DirectiveTargetLaser",  ai_directive_targetlaser_cb,
                    "(aiId, clientId) — force target (laser-painted)",
                    3, 3);
}

} // namespace dts_viewer
