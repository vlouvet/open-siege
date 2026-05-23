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
#include "console/script.h"
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
    // Spec 18/03 — seed the tick-position from the spawn pos so the
    // pathfollow loop has a starting point.
    parseVec3(pos, bot->mTickPos);
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

// ---------------------------------------------------------------------------
// AI::SetVar(name, varName, value) — alias AI::setVar. Wildcard "*" name
// applies to every live AIPlayer (per ai.cs L56).
// ---------------------------------------------------------------------------
void ai_setvar_cb(SimObject*, S32 argc, ConsoleValue* argv)
{
    if (argc < 4) return;
    const char* name    = argv[1].getString();
    const char* varName = argv[2].getString();
    const char* value   = argv[3].getString();
    if (!varName || !*varName) return;
    if (!value) value = "";

    if (name && std::strcmp(name, "*") == 0) {
        for (auto* bot : dts_viewer::all_ai_players())
            bot->mVars[varName] = value;
        return;
    }
    if (AIPlayer* bot = resolve_ai(argv[1]))
        bot->mVars[varName] = value;
}

// ---------------------------------------------------------------------------
// AI::CallbackPeriodic(name, freqSec, scriptFunc) — schedule `scriptFunc`
// to fire every `freqSec` real seconds. Stored on the AIPlayer and ticked
// by ai_tick_all (spec 18/03).
// ---------------------------------------------------------------------------
void ai_callback_periodic_cb(SimObject*, S32 argc, ConsoleValue* argv)
{
    if (argc < 4) return;
    AIPlayer* bot = resolve_ai(argv[1]);
    if (!bot) return;
    const float freq = static_cast<float>(argv[2].getFloat());
    const char* fn   = argv[3].getString();
    if (!fn) fn = "";
    bot->mPeriodicFn          = fn;
    bot->mPeriodicPeriod      = (freq > 0.0f) ? freq : 0.0f;
    bot->mPeriodicAccumulator = bot->mPeriodicPeriod;
}

// ---------------------------------------------------------------------------
// AI::callWithId(aiId, scriptFunc, ...args) — invoke `scriptFunc` with
// `aiId` prepended. `aiId == "*"` runs once per live bot. ai.cs L54-55,
// L486-488 are the retail call sites.
//
// We emit a literal script statement (`Player::mountItem(2050, blaster, 0);`)
// rather than going through Sim::callMethod so the call site lands in the
// existing Namespace lookup with the right argument types.
// ---------------------------------------------------------------------------
void ai_callwith_id_cb(SimObject*, S32 argc, ConsoleValue* argv)
{
    if (argc < 3) return;
    const char* id_or_star = argv[1].getString();
    const char* fn         = argv[2].getString();
    if (!fn || !*fn) return;

    auto emit_call = [&](int id) {
        std::string cmd;
        cmd.reserve(64);
        cmd.append(fn);
        cmd.push_back('(');
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", id);
        cmd.append(buf);
        for (S32 i = 3; i < argc; ++i) {
            cmd.append(", ");
            const char* a = argv[i].getString();
            // Wrap string args in quotes (numeric-looking args pass through
            // — TorqueScript parses them either way).
            if (a && *a) {
                cmd.push_back('"');
                cmd.append(a);
                cmd.push_back('"');
            } else {
                cmd.append("\"\"");
            }
        }
        cmd.append(");");
        Con::evaluate(cmd.c_str(), false, "AI::callWithId");
    };

    if (std::strcmp(id_or_star, "*") == 0) {
        for (auto* bot : dts_viewer::all_ai_players())
            emit_call(static_cast<int>(bot->getId()));
        return;
    }
    if (AIPlayer* bot = resolve_ai(argv[1]))
        emit_call(static_cast<int>(bot->getId()));
}

// ---------------------------------------------------------------------------
// Graph::AddNode / Graph::buildGraph / Graph::reset — stubs. Retail
// ai.cs::buildGraph calls these for MissionGroup\AIGraph nodes. v1 does
// straight-line waypoint following, so we just log + count.
// ---------------------------------------------------------------------------
namespace { int g_graph_node_count = 0; }

void graph_addnode_cb(SimObject*, S32 argc, ConsoleValue*)
{
    if (argc >= 2) ++g_graph_node_count;
}
void graph_buildgraph_cb(SimObject*, S32, ConsoleValue*)
{
    Con::printf("Graph::buildGraph stub — %d nodes registered (v1 ignores)",
                g_graph_node_count);
}
void graph_reset_cb(SimObject*, S32, ConsoleValue*)
{
    g_graph_node_count = 0;
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

    // Spec 18/03 — variable bag, periodic callbacks, callWithId, Graph stubs.
    Con::addCommand("AI", "SetVar",            ai_setvar_cb,
                    "(name, varName, value) — set a bot variable", 4, 4);
    Con::addCommand("AI", "setVar",            ai_setvar_cb,
                    "(name, varName, value) — lowercase alias", 4, 4);
    Con::addCommand("AI", "CallbackPeriodic",  ai_callback_periodic_cb,
                    "(name, freqSec, scriptFunc)", 4, 4);
    Con::addCommand("AI", "callbackPeriodic",  ai_callback_periodic_cb,
                    "(name, freqSec, scriptFunc) — lowercase alias", 4, 4);
    Con::addCommand("AI", "callWithId",        ai_callwith_id_cb,
                    "(aiId, scriptFunc, ...) — invoke scriptFunc(aiId, ...)",
                    3, 32);
    Con::addCommand("Graph", "AddNode",        graph_addnode_cb,
                    "(pos, name) — stub: nav-graph node (v1 ignores)", 3, 3);
    Con::addCommand("Graph", "buildGraph",     graph_buildgraph_cb,
                    "() — stub: finalise nav graph", 1, 1);
    Con::addCommand("Graph", "reset",          graph_reset_cb,
                    "() — stub: clear nav graph", 1, 1);
}

} // namespace dts_viewer
