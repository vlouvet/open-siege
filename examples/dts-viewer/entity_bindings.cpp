// Open Siege spec 16/06 — script bindings for Item / Turret / StaticShape
// / Moveable / Generator / Trigger / VehiclePlaceholder / Door /
// MissionMarker / SimLight / Sky / Planet / Sensor.
//
// All follow the same pattern as `class Player` (spec 16/05). Storage
// is the SimObject's own member fields; gameplay-side wiring is a
// follow-up spec.

#include "entity_bindings.hpp"

#include "console/sim.h"

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

IMPLEMENT_CONOBJECT(Sensor);
ConsoleDocClass(Sensor, "@brief Spec 16/06 Sensor binding (script). scan() is a stub.");
DefineEngineMethod(Sensor, scan, S32, (), ,
    "Spec 16/06 stub: scan for nearby objects. Returns 0 for now.")
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
