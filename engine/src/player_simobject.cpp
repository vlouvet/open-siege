// Open Siege spec 16/05 — Player SimObject impl. See header for rationale.

#include "player_simobject.hpp"
#include "player_controller.hpp"

#include "console/sim.h"
#include "console/script.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

// ---- Spec 16/09 bridge helpers --------------------------------------------

namespace {

// Parse "x y z" into a 3-tuple. Missing trailing components default to 0.
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

const char* formatVec3(float x, float y, float z)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%g %g %g", x, y, z);
    return StringTable->insert(buf);
}

// Protected-field thunks. Signature: bool fn(void*, const char*, const char*).
// Returning false skips the default field-write — we redirect into mLive.
bool setPosField(void* obj, const char* /*idx*/, const char* data)
{
    static_cast<Player*>(obj)->writePosToLive(data);
    // Update the cached mPos so subsequent reads still echo the value
    // even if mLive is unbound.
    static_cast<Player*>(obj)->mPos = StringTable->insert(data ? data : "0 0 0");
    return false;
}
const char* getPosField(void* obj, const char* /*data*/)
{
    Player* p = static_cast<Player*>(obj);
    if (p->mLive) {
        p->syncFromLive();
    }
    return p->mPos;
}
bool setVelField(void* obj, const char* /*idx*/, const char* data)
{
    static_cast<Player*>(obj)->writeVelToLive(data);
    static_cast<Player*>(obj)->mVel = StringTable->insert(data ? data : "0 0 0");
    return false;
}
const char* getVelField(void* obj, const char* /*data*/)
{
    Player* p = static_cast<Player*>(obj);
    if (p->mLive) p->syncFromLive();
    return p->mVel;
}
bool setHealthField(void* obj, const char* /*idx*/, const char* data)
{
    Player* p = static_cast<Player*>(obj);
    float v = data ? static_cast<float>(std::atof(data)) : 0.0f;
    p->writeHealthToLive(v);
    p->mHealth = v;
    return false;
}
const char* getHealthField(void* obj, const char* /*data*/)
{
    Player* p = static_cast<Player*>(obj);
    if (p->mLive) p->syncFromLive();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", p->mHealth);
    return StringTable->insert(buf);
}
bool setEnergyField(void* obj, const char* /*idx*/, const char* data)
{
    Player* p = static_cast<Player*>(obj);
    float v = data ? static_cast<float>(std::atof(data)) : 0.0f;
    p->writeEnergyToLive(v);
    p->mEnergy = v;
    return false;
}
const char* getEnergyField(void* obj, const char* /*data*/)
{
    Player* p = static_cast<Player*>(obj);
    if (p->mLive) p->syncFromLive();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", p->mEnergy);
    return StringTable->insert(buf);
}

} // anonymous namespace

void Player::initPersistFields()
{
    // Spec 16/09 — protected get/set so when mLive is bound, reads return
    // the live gameplay value and writes propagate into PlayerState.
    // Trailing "" docs disambiguates the 4-overload addProtectedField group.
    addProtectedField("pos",       TypeString,
        Offset(mPos,    Player), &setPosField,    &getPosField,    "");
    addProtectedField("vel",       TypeString,
        Offset(mVel,    Player), &setVelField,    &getVelField,    "");
    addProtectedField("health",    TypeF32,
        Offset(mHealth, Player), &setHealthField, &getHealthField, "");
    addProtectedField("energy",    TypeF32,
        Offset(mEnergy, Player), &setEnergyField, &getEnergyField, "");
    // Non-proxied fields keep the simple addField path.
    addField("healthMax",    TypeF32,    Offset(mHealthMax,    Player));
    addField("energyMax",    TypeF32,    Offset(mEnergyMax,    Player));
    addField("team",         TypeS32,    Offset(mTeam,         Player));
    addField("dataBlock",    TypeString, Offset(mDataBlock,    Player));
    addField("mountedItems", TypeS32,    Offset(mMountedItems, Player));
    Parent::initPersistFields();
}

void Player::fire()
{
    ++sFireCalls;
    if (mLive) mLive->script_fire_latch = true;
}

void Player::kill()
{
    ++sKillCalls;
    mHealth = 0.0f;
    if (mLive) mLive->health = 0.0f;
}

void Player::respawn()
{
    ++sRespawnCalls;
    mHealth = mHealthMax;
    mEnergy = mEnergyMax;
    if (mLive) {
        mLive->health   = mLive->health_max;
        mLive->jet_fuel = 100.0f;  // PlayerTuning::jet_fuel_max — default-aligned
    }
}

void Player::setPos(const char* xyz)
{
    mPos = StringTable->insert(xyz ? xyz : "0 0 0");
    writePosToLive(xyz);
}

void Player::setVelocity(const char* xyz)
{
    mVel = StringTable->insert(xyz ? xyz : "0 0 0");
    writeVelToLive(xyz);
}

void Player::setLivePlayerState(dts_viewer::PlayerState* live)
{
    mLive = live;
    if (mLive) {
        // Initial sync: prime the cached fields from the live state so
        // first-read-after-bind reflects real gameplay values.
        syncFromLive();
        mHealthMax = mLive->health_max;
    }
}

void Player::syncFromLive()
{
    if (!mLive) return;
    mPos    = formatVec3(mLive->pos.x, mLive->pos.y, mLive->pos.z);
    mVel    = formatVec3(mLive->vel.x, mLive->vel.y, mLive->vel.z);
    mHealth = mLive->health;
    mEnergy = mLive->jet_fuel;
}

void Player::writePosToLive(const char* xyz)
{
    if (!mLive) return;
    float v[3]; parseVec3(xyz, v);
    mLive->pos.x = v[0];
    mLive->pos.y = v[1];
    mLive->pos.z = v[2];
    // Stop momentum on teleport so the player doesn't get pulled back
    // toward the old position by inertia.
    mLive->vel = glm::vec3(0.0f);
    mLive->on_ground = false;
}

void Player::writeVelToLive(const char* xyz)
{
    if (!mLive) return;
    float v[3]; parseVec3(xyz, v);
    mLive->vel.x = v[0];
    mLive->vel.y = v[1];
    mLive->vel.z = v[2];
}

void Player::writeHealthToLive(float v)
{
    if (mLive) mLive->health = v;
}

void Player::writeEnergyToLive(float v)
{
    if (mLive) mLive->jet_fuel = v;
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
