// Open Siege spec 16/05 — Player SimObject impl. See header for rationale.

#include "player_simobject.hpp"

#include "console/sim.h"
#include "console/script.h"

int Player::sFireCalls    = 0;
int Player::sKillCalls    = 0;
int Player::sRespawnCalls = 0;

Player::Player()
    : mPos(StringTable->insert("0 0 0"))
    , mVel(StringTable->insert("0 0 0"))
    , mHealth(100.0f)
    , mHealthMax(100.0f)
    , mEnergy(100.0f)
    , mEnergyMax(100.0f)
    , mTeam(0)
    , mDataBlock(StringTable->insert(""))
    , mMountedItems(0)
{
}

void Player::initPersistFields()
{
    addField("pos",          TypeString, Offset(mPos,          Player));
    addField("vel",          TypeString, Offset(mVel,          Player));
    addField("health",       TypeF32,    Offset(mHealth,       Player));
    addField("healthMax",    TypeF32,    Offset(mHealthMax,    Player));
    addField("energy",       TypeF32,    Offset(mEnergy,       Player));
    addField("energyMax",    TypeF32,    Offset(mEnergyMax,    Player));
    addField("team",         TypeS32,    Offset(mTeam,         Player));
    addField("dataBlock",    TypeString, Offset(mDataBlock,    Player));
    addField("mountedItems", TypeS32,    Offset(mMountedItems, Player));
    Parent::initPersistFields();
}

void Player::fire()    { ++sFireCalls; }
void Player::kill()    { ++sKillCalls;    mHealth = 0.0f; }
void Player::respawn() { ++sRespawnCalls; mHealth = mHealthMax; mEnergy = mEnergyMax; }

void Player::setPos(const char* xyz)
{
    mPos = StringTable->insert(xyz ? xyz : "0 0 0");
}

void Player::setVelocity(const char* xyz)
{
    mVel = StringTable->insert(xyz ? xyz : "0 0 0");
}

IMPLEMENT_CONOBJECT(Player);

ConsoleDocClass(Player,
    "@brief Spec 16/05 Player binding — script-side view of the player "
    "controller. Mutations propagate to PlayerState in a follow-up spec.");

DefineEngineMethod(Player, fire, void, (), ,
    "Spec 16/05: trigger the equipped weapon.")
{
    object->fire();
}

DefineEngineMethod(Player, kill, void, (), ,
    "Spec 16/05: zero health and run death state machine.")
{
    object->kill();
}

DefineEngineMethod(Player, respawn, void, (), ,
    "Spec 16/05: restore to full health + energy.")
{
    object->respawn();
}

DefineEngineMethod(Player, setPos, void, (const char* xyz), ("0 0 0"),
    "Spec 16/05: teleport to position.")
{
    object->setPos(xyz);
}

DefineEngineMethod(Player, getPos, const char*, (), ,
    "Spec 16/05: return position as \"x y z\".")
{
    return object->mPos;
}

DefineEngineMethod(Player, setVelocity, void, (const char* xyz), ("0 0 0"),
    "Spec 16/05: assign world-space velocity.")
{
    object->setVelocity(xyz);
}

DefineEngineMethod(Player, getVelocity, const char*, (), ,
    "Spec 16/05: return velocity as \"x y z\".")
{
    return object->mVel;
}

DefineEngineMethod(Player, getMountedItemCount, S32, (), ,
    "Spec 16/05: number of items currently mounted in inventory slots.")
{
    return object->getMountedItemCount();
}

DefineEngineMethod(Player, getControllingClient, S32, (), ,
    "Spec 16/05 stub: ID of the client controlling this player. Returns 0 "
    "(single-player; multi-client support is track 20+).")
{
    (void)object;
    return 0;
}

namespace dts_viewer {

void anchorPlayerClass()
{
    // Use the class rep so the linker preserves the IMPLEMENT_CONOBJECT
    // initialiser and all DefineEngineMethod ctors above it.
    static auto* anchor = Player::getStaticClassRep();
    (void)anchor;
}

} // namespace dts_viewer
