// Open Siege spec 16/06 — script bindings for Item / Turret / StaticShape
// / Moveable / Generator / Trigger / VehiclePlaceholder / Door /
// MissionMarker / SimLight / Sky / Planet / Sensor.
//
// All follow the same pattern as `class Player` (spec 16/05). Spec 16/10
// extended this with mLive / setLive*State() pointers so script-side
// method calls reach the live POD state structs in entity_renderer.hpp.

#include "entity_bindings.hpp"
#include "entity_renderer.hpp"

#include "console/sim.h"
#include "console/simSet.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

namespace {
// Parse a "x y z" stringtable entry into a vec3-shaped tuple. Missing
// trailing components default to 0.
void parsePos(const char* xyz, float out[3])
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
}

// ---- Item -----------------------------------------------------------------
int Item::sPickupCalls = 0;
int Item::sMountCalls  = 0;

Item::Item()
  : mDataBlock(StringTable->insert(""))
  , mPos      (StringTable->insert("0 0 0"))
  , mRot      (StringTable->insert("0 0 0"))
  , mActive   (true)
  , mRespawnTime(30.0f)
{}

void Item::initPersistFields()
{
    addField("dataBlock",   TypeString, Offset(mDataBlock,   Item));
    addField("pos",         TypeString, Offset(mPos,         Item));
    addField("rot",         TypeString, Offset(mRot,         Item));
    addField("active",      TypeBool,   Offset(mActive,      Item));
    addField("respawnTime", TypeF32,    Offset(mRespawnTime, Item));
    Parent::initPersistFields();
}

void Item::pickup()
{
    ++sPickupCalls;
    mActive = false;
    if (mLive) mLive->active = false;
}

IMPLEMENT_CONOBJECT(Item);
ConsoleDocClass(Item, "@brief Spec 16/06 Item binding (script).");
DefineEngineMethod(Item, pickup, void, (), , "Spec 16/06") { object->pickup(); }
DefineEngineMethod(Item, mount,  void, (), , "Spec 16/06") { object->mount();  }

// ---- Turret ---------------------------------------------------------------
int Turret::sFireCalls  = 0;
int Turret::sAimAtCalls = 0;

Turret::Turret()
  : mDataBlock(StringTable->insert(""))
  , mPos      (StringTable->insert("0 0 0"))
  , mTeam(0), mHealth(150.0f), mHealthMax(150.0f)
  , mScanRange(200.0f), mDestroyed(false)
{}

void Turret::initPersistFields()
{
    addField("dataBlock", TypeString, Offset(mDataBlock, Turret));
    addField("pos",       TypeString, Offset(mPos,       Turret));
    addField("team",      TypeS32,    Offset(mTeam,      Turret));
    addField("health",    TypeF32,    Offset(mHealth,    Turret));
    addField("healthMax", TypeF32,    Offset(mHealthMax, Turret));
    addField("scanRange", TypeF32,    Offset(mScanRange, Turret));
    addField("destroyed", TypeBool,   Offset(mDestroyed, Turret));
    Parent::initPersistFields();
}

void Turret::fire()
{
    ++sFireCalls;
    if (mLive) mLive->script_fire_latch = true;
}

IMPLEMENT_CONOBJECT(Turret);
ConsoleDocClass(Turret, "@brief Spec 16/06 Turret binding (script).");
DefineEngineMethod(Turret, fire,  void, (), , "Spec 16/06") { object->fire(); }
DefineEngineMethod(Turret, aimAt, void, (const char* target), (""),
    "Spec 16/06: aim at the named target.")  { object->aimAt(target); }

// ---- StaticShape ----------------------------------------------------------
StaticShape::StaticShape()
  : mDataBlock(StringTable->insert(""))
  , mPos      (StringTable->insert("0 0 0"))
  , mRot      (StringTable->insert("0 0 0"))
  , mShape    (StringTable->insert(""))
{}

void StaticShape::initPersistFields()
{
    addField("dataBlock", TypeString, Offset(mDataBlock, StaticShape));
    addField("pos",       TypeString, Offset(mPos,       StaticShape));
    addField("rot",       TypeString, Offset(mRot,       StaticShape));
    addField("shape",     TypeString, Offset(mShape,     StaticShape));
    Parent::initPersistFields();
}

IMPLEMENT_CONOBJECT(StaticShape);
ConsoleDocClass(StaticShape, "@brief Spec 16/06 StaticShape binding (script).");

// ---- Moveable -------------------------------------------------------------
Moveable::Moveable()
  : mDataBlock (StringTable->insert(""))
  , mPos       (StringTable->insert("0 0 0"))
  , mEndpointB (StringTable->insert("0 10 0"))
  , mCloseTime (4.0f), mDwellTime(2.0f)
{}

void Moveable::initPersistFields()
{
    addField("dataBlock", TypeString, Offset(mDataBlock, Moveable));
    addField("pos",       TypeString, Offset(mPos,       Moveable));
    addField("endpointB", TypeString, Offset(mEndpointB, Moveable));
    addField("closeTime", TypeF32,    Offset(mCloseTime, Moveable));
    addField("dwellTime", TypeF32,    Offset(mDwellTime, Moveable));
    Parent::initPersistFields();
}

IMPLEMENT_CONOBJECT(Moveable);
ConsoleDocClass(Moveable, "@brief Spec 16/06 Moveable binding (script).");

// ---- Generator ------------------------------------------------------------
Generator::Generator()
  : mDataBlock (StringTable->insert(""))
  , mPos       (StringTable->insert("0 0 0"))
  , mTeam(0), mHealth(250.0f), mHealthMax(250.0f)
  , mDestroyed(false), mIsPortable(false)
{}

void Generator::initPersistFields()
{
    addField("dataBlock",  TypeString, Offset(mDataBlock,  Generator));
    addField("pos",        TypeString, Offset(mPos,        Generator));
    addField("team",       TypeS32,    Offset(mTeam,       Generator));
    addField("health",     TypeF32,    Offset(mHealth,     Generator));
    addField("healthMax",  TypeF32,    Offset(mHealthMax,  Generator));
    addField("destroyed",  TypeBool,   Offset(mDestroyed,  Generator));
    addField("isPortable", TypeBool,   Offset(mIsPortable, Generator));
    Parent::initPersistFields();
}

void Generator::setLiveGeneratorState(dts_viewer::GeneratorState* p)
{
    mLive = p;
    if (mLive) {
        mHealth    = mLive->health;
        mHealthMax = mLive->health_max;
        mTeam      = mLive->team;
        mDestroyed = mLive->destroyed;
    }
}

IMPLEMENT_CONOBJECT(Generator);
ConsoleDocClass(Generator, "@brief Spec 16/06 Generator binding (script).");

// ---- Trigger --------------------------------------------------------------
Trigger::Trigger()
  : mDataBlock(StringTable->insert(""))
  , mPos      (StringTable->insert("0 0 0"))
  , mBounds   (StringTable->insert("-1 -1 -1 1 1 1"))
  , mIsSphere (false), mActive(true)
{}

void Trigger::initPersistFields()
{
    addField("dataBlock", TypeString, Offset(mDataBlock, Trigger));
    addField("pos",       TypeString, Offset(mPos,       Trigger));
    addField("bounds",    TypeString, Offset(mBounds,    Trigger));
    addField("isSphere",  TypeBool,   Offset(mIsSphere,  Trigger));
    addField("active",    TypeBool,   Offset(mActive,    Trigger));
    Parent::initPersistFields();
}

IMPLEMENT_CONOBJECT(Trigger);
ConsoleDocClass(Trigger, "@brief Spec 16/06 Trigger binding (script).");

// ---- VehiclePlaceholder ---------------------------------------------------
VehiclePlaceholder::VehiclePlaceholder()
  : mDataBlock (StringTable->insert(""))
  , mPos       (StringTable->insert("0 0 0"))
  , mVehicleDts(StringTable->insert(""))
  , mVisible(true)
{}

void VehiclePlaceholder::initPersistFields()
{
    addField("dataBlock",  TypeString, Offset(mDataBlock,  VehiclePlaceholder));
    addField("pos",        TypeString, Offset(mPos,        VehiclePlaceholder));
    addField("vehicleDts", TypeString, Offset(mVehicleDts, VehiclePlaceholder));
    addField("visible",    TypeBool,   Offset(mVisible,    VehiclePlaceholder));
    Parent::initPersistFields();
}

IMPLEMENT_CONOBJECT(VehiclePlaceholder);
ConsoleDocClass(VehiclePlaceholder, "@brief Spec 16/06 VehiclePlaceholder binding.");

// ---- Door -----------------------------------------------------------------
Door::Door()
  : mDataBlock(StringTable->insert(""))
  , mPos      (StringTable->insert("0 0 0"))
  , mOpen     (false)
{}

void Door::initPersistFields()
{
    addField("dataBlock", TypeString, Offset(mDataBlock, Door));
    addField("pos",       TypeString, Offset(mPos,       Door));
    addField("open",      TypeBool,   Offset(mOpen,      Door));
    Parent::initPersistFields();
}

IMPLEMENT_CONOBJECT(Door);
ConsoleDocClass(Door, "@brief Spec 16/06 Door binding (script).");

// ---- MissionMarker --------------------------------------------------------
MissionMarker::MissionMarker()
  : mDataBlock(StringTable->insert(""))
  , mPos      (StringTable->insert("0 0 0"))
  , mRot      (StringTable->insert("0 0 0"))
{}

void MissionMarker::initPersistFields()
{
    addField("dataBlock", TypeString, Offset(mDataBlock, MissionMarker));
    addField("pos",       TypeString, Offset(mPos,       MissionMarker));
    addField("rot",       TypeString, Offset(mRot,       MissionMarker));
    Parent::initPersistFields();
}

IMPLEMENT_CONOBJECT(MissionMarker);
ConsoleDocClass(MissionMarker, "@brief Spec 16/06 MissionMarker binding (script).");

// ---- SimLight -------------------------------------------------------------
SimLight::SimLight()
  : mPos  (StringTable->insert("0 0 0"))
  , mColor(StringTable->insert("1 1 1 1"))
  , mRange(20.0f)
{}

void SimLight::initPersistFields()
{
    addField("pos",   TypeString, Offset(mPos,   SimLight));
    addField("color", TypeString, Offset(mColor, SimLight));
    addField("range", TypeF32,    Offset(mRange, SimLight));
    Parent::initPersistFields();
}

IMPLEMENT_CONOBJECT(SimLight);
ConsoleDocClass(SimLight, "@brief Spec 16/06 SimLight binding (script).");

// ---- Sky ------------------------------------------------------------------
Sky::Sky()
  : mMaterial(StringTable->insert(""))
  , mWindVelocity(0.0f)
{}

void Sky::initPersistFields()
{
    addField("material",     TypeString, Offset(mMaterial,     Sky));
    addField("windVelocity", TypeF32,    Offset(mWindVelocity, Sky));
    Parent::initPersistFields();
}

IMPLEMENT_CONOBJECT(Sky);
ConsoleDocClass(Sky, "@brief Spec 16/06 Sky binding (script).");

// ---- Planet ---------------------------------------------------------------
Planet::Planet()
  : mMaterial(StringTable->insert(""))
  , mPos     (StringTable->insert("0 0 0"))
{}

void Planet::initPersistFields()
{
    addField("material", TypeString, Offset(mMaterial, Planet));
    addField("pos",      TypeString, Offset(mPos,      Planet));
    Parent::initPersistFields();
}

IMPLEMENT_CONOBJECT(Planet);
ConsoleDocClass(Planet, "@brief Spec 16/06 Planet binding (script).");

// ---- Sensor ---------------------------------------------------------------
int Sensor::sScanCalls = 0;

Sensor::Sensor()
  : mPos  (StringTable->insert("0 0 0"))
  , mTeam (0)
  , mRange(200.0f)
{}

void Sensor::initPersistFields()
{
    addField("pos",   TypeString, Offset(mPos,   Sensor));
    addField("team",  TypeS32,    Offset(mTeam,  Sensor));
    addField("range", TypeF32,    Offset(mRange, Sensor));
    Parent::initPersistFields();
}

const char* Sensor::scan()
{
    ++sScanCalls;
    SimObject* mg = Sim::findObject("MissionGroup");
    if (!mg) return StringTable->insert("");
    auto* group = dynamic_cast<SimGroup*>(mg);
    if (!group) return StringTable->insert("");

    float center[3]; parsePos(mPos, center);
    const float range_sq = mRange * mRange;

    std::string out;
    char buf[32];

    std::function<void(SimGroup*)> walk = [&](SimGroup* g) {
        for (auto it = g->begin(); it != g->end(); ++it) {
            SimObject* obj = *it;
            // Recurse into nested groups (the MissionGroup tree is flat in
            // practice but defensive coding is cheap).
            if (auto* sub = dynamic_cast<SimGroup*>(obj)) {
                walk(sub);
                continue;
            }
            // Skip self.
            if (obj == this) continue;
            // Find a `pos` data-field. Per spec 16/06 every reflected
            // entity carries `pos = "x y z"`.
            const char* posField = obj->getDataField(
                StringTable->insert("pos"), nullptr);
            if (!posField || !*posField) continue;
            float p[3]; parsePos(posField, p);
            const float dx = p[0] - center[0];
            const float dy = p[1] - center[1];
            const float dz = p[2] - center[2];
            const float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 > range_sq) continue;
            if (!out.empty()) out += ' ';
            std::snprintf(buf, sizeof(buf), "%u", obj->getId());
            out += buf;
        }
    };
    walk(group);
    return StringTable->insert(out.c_str());
}

IMPLEMENT_CONOBJECT(Sensor);
ConsoleDocClass(Sensor, "@brief Spec 16/06/10 Sensor — scan() walks MissionGroup.");
DefineEngineMethod(Sensor, scan, const char*, (), ,
    "Spec 16/10: return space-separated SimObject IDs within mRange.")
{
    return object->scan();
}

namespace dts_viewer {

void anchorEntityClasses()
{
    static AbstractClassRep* anchors[] = {
        Item::getStaticClassRep(),
        Turret::getStaticClassRep(),
        StaticShape::getStaticClassRep(),
        Moveable::getStaticClassRep(),
        Generator::getStaticClassRep(),
        Trigger::getStaticClassRep(),
        VehiclePlaceholder::getStaticClassRep(),
        Door::getStaticClassRep(),
        MissionMarker::getStaticClassRep(),
        SimLight::getStaticClassRep(),
        Sky::getStaticClassRep(),
        Planet::getStaticClassRep(),
        Sensor::getStaticClassRep(),
    };
    (void)anchors;
}

} // namespace dts_viewer
