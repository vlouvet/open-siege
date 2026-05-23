// Spec 18/02 — AIPlayer impl + the global name registry the AI::*
// engine bindings use.

#include "ai_player.hpp"

#include <algorithm>
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

} // namespace dts_viewer
