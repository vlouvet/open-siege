#ifndef DTS_VIEWER_PLAYER_SIMOBJECT_HPP
#define DTS_VIEWER_PLAYER_SIMOBJECT_HPP

// Open Siege spec 16/05 — Player as scriptable SimObject.
//
// Subclass of SimObject so a script can:
//   - Construct: `new Player(Hero) { dataBlock = "LightArmor"; pos = "0 0 100"; };`
//   - Read/write reflected fields: `Hero.pos`, `Hero.health`, `Hero.energy`, ...
//   - Call C++ methods: `Hero.fire(); Hero.kill(); Hero.respawn();`
//
// Why at global namespace? Torque's IMPLEMENT_CONOBJECT and
// DefineEngineMethod macros token-paste the class name into symbol
// identifiers (`_fnPlayerfireimpl`, `Player::dynClassRep`, ...). The
// preprocessor can't paste `dts_viewer::Player` so the convention is
// to keep SimObject subclasses at global scope.
//
// Storage model (deliberate v1 trade-off):
//
//   Player owns its own state (mPos, mVel, mHealth, mEnergy, mTeam,
//   mDataBlock, mMountedItems). It does NOT yet drive the
//   `dts_viewer::PlayerState` POD that the camera + physics use. A
//   follow-up spec wires a live PlayerState pointer so script-side
//   mutations propagate to gameplay. For now, the binding surface is
//   independently testable and gameplay-agnostic.

#include "console/console.h"
#include "console/consoleInternal.h"
#include "console/consoleTypes.h"
#include "console/engineAPI.h"
#include "console/simObject.h"

namespace dts_viewer { struct PlayerState; }

class Player : public SimObject
{
    typedef SimObject Parent;

public:
    // Reflected fields (writable from script via Hero.fieldName = value).
    // When mLive is bound (see setLivePlayerState), these become caches —
    // the protected setters/getters defined in initPersistFields() forward
    // reads/writes to mLive->… so script edits drive real gameplay.
    StringTableEntry mPos;       // serialised as "x y z" — Tribes idiom
    StringTableEntry mVel;       // serialised as "x y z"
    F32              mHealth;
    F32              mHealthMax;
    F32              mEnergy;
    F32              mEnergyMax;
    S32              mTeam;
    StringTableEntry mDataBlock; // datablock identifier (e.g. "LightArmor")
    S32              mMountedItems;

    // Spec 16/09 — live gameplay state pointer. nullptr means no bridge
    // is wired (script-only path, used by the spec 16/05 unit test).
    dts_viewer::PlayerState* mLive = nullptr;

    // Engine-side counters for script-callable methods.
    static int sFireCalls;
    static int sKillCalls;
    static int sRespawnCalls;

    Player();

    static void initPersistFields();

    // Engine methods callable from script.
    void fire();
    void kill();
    void respawn();
    void setPos     (const char* xyz);
    void setVelocity(const char* xyz);
    S32  getMountedItemCount() const { return mMountedItems; }

    // Spec 16/09 — bind/unbind the gameplay-side PlayerState pointer.
    // The bridge is opt-in; callers that just want a script-side
    // SimObject (e.g. spec 16/05's unit test) leave it nullptr.
    void setLivePlayerState(dts_viewer::PlayerState* live);

    // Sync helpers exposed for the protected-field callbacks.
    void syncFromLive();   // mLive -> cached mPos/mVel/mHealth/mEnergy
    void writePosToLive(const char* xyz);
    void writeVelToLive(const char* xyz);
    void writeHealthToLive(float v);
    void writeEnergyToLive(float v);

    DECLARE_CONOBJECT(Player);
};

// Force-link anchor — invoked from cscript_host so the static
// IMPLEMENT_CONOBJECT(Player) registration survives static-archive
// link. Lives in dts_viewer:: because the host calls it through
// that namespace; the Player class itself is global.
namespace dts_viewer {
    void anchorPlayerClass();
}

#endif // DTS_VIEWER_PLAYER_SIMOBJECT_HPP
