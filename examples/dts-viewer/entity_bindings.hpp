#ifndef DTS_VIEWER_ENTITY_BINDINGS_HPP
#define DTS_VIEWER_ENTITY_BINDINGS_HPP

// Open Siege spec 16/06 — script bindings for the remaining Track 14
// entity types. Same pattern as `class Player` (spec 16/05): each is a
// global-namespace SimObject subclass with addField-reflected
// properties + a handful of DefineEngineMethod hooks where the
// behaviour is meaningful (placement-only entities like StaticShape /
// MissionMarker / SimLight / Sky have no methods beyond the
// SimObject base).
//
// Each class is intentionally minimal:
//   - dataBlock (StringTableEntry)
//   - pos       (StringTableEntry, "x y z")
//   - rot       (StringTableEntry, "x y z w" or Euler)
//   - team      (S32) where it matters
// plus per-type semantic fields (health, active, etc.).
//
// 22 native MIS-corpus types broken down:
//   - Bound (this file): Item, Turret, StaticShape, Moveable, Generator,
//     Trigger, VehiclePlaceholder, Door, MissionMarker, SimLight, Sky,
//     Planet, Sensor.
//   - Already bound elsewhere: Player (spec 16/05).
//   - Internal-only / not script-constructable: DataBlocks (spec 16/03),
//     SimGroup/SimSet (spec 16/02), the various render-state datablock
//     types that exist purely as SimDataBlock subclasses.
//
// 13 bindings + Player = 14 distinct script-callable classes — covers
// every entity-type identifier present in the shipped T1 `.mis` corpus.

#include "console/console.h"
#include "console/consoleInternal.h"
#include "console/consoleTypes.h"
#include "console/engineAPI.h"
#include "console/simObject.h"

// Forward decls of the live POD state structs. Defined in
// entity_renderer.hpp; included only by callers that need them.
namespace dts_viewer {
struct ItemState;
struct TurretState;
struct GeneratorState;
struct TriggerState;
}

// All entity SimObject subclasses live at global scope. See
// player_simobject.hpp for the macro-token-pasting reason.

class Item : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mDataBlock;
    StringTableEntry mPos;
    StringTableEntry mRot;
    bool             mActive;
    F32              mRespawnTime;
    static int       sPickupCalls;
    static int       sMountCalls;

    // Spec 16/10 — live state link. pickup() flips mLive->active.
    dts_viewer::ItemState* mLive = nullptr;
    void setLiveItemState(dts_viewer::ItemState* p) { mLive = p; }

    Item();
    static void initPersistFields();
    void pickup();
    void mount()  { ++sMountCalls; }
    DECLARE_CONOBJECT(Item);
};

class Turret : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mDataBlock;
    StringTableEntry mPos;
    S32              mTeam;
    F32              mHealth;
    F32              mHealthMax;
    F32              mScanRange;
    bool             mDestroyed;
    static int       sFireCalls;
    static int       sAimAtCalls;

    // Spec 16/10 — live state link. fire() flips a latch consumed by
    // tick_turrets; aimAt() is still a stub pending target tracking.
    dts_viewer::TurretState* mLive = nullptr;
    void setLiveTurretState(dts_viewer::TurretState* p) { mLive = p; }

    Turret();
    static void initPersistFields();
    void fire();
    void aimAt(const char* /*target*/) { ++sAimAtCalls; }
    DECLARE_CONOBJECT(Turret);
};

class StaticShape : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mDataBlock;
    StringTableEntry mPos;
    StringTableEntry mRot;
    StringTableEntry mShape;

    StaticShape();
    static void initPersistFields();
    DECLARE_CONOBJECT(StaticShape);
};

class Moveable : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mDataBlock;
    StringTableEntry mPos;
    StringTableEntry mEndpointB;
    F32              mCloseTime;
    F32              mDwellTime;

    Moveable();
    static void initPersistFields();
    DECLARE_CONOBJECT(Moveable);
};

class Generator : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mDataBlock;
    StringTableEntry mPos;
    S32              mTeam;
    F32              mHealth;
    F32              mHealthMax;
    bool             mDestroyed;
    bool             mIsPortable;

    // Spec 16/10 — live state link. Reads of health / destroyed
    // reflect the live POD when bound.
    dts_viewer::GeneratorState* mLive = nullptr;
    void setLiveGeneratorState(dts_viewer::GeneratorState* p);

    Generator();
    static void initPersistFields();
    DECLARE_CONOBJECT(Generator);
};

class Trigger : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mDataBlock;
    StringTableEntry mPos;
    StringTableEntry mBounds;     // "x_min y_min z_min x_max y_max z_max"
    bool             mIsSphere;
    bool             mActive;

    // Spec 16/10 — live state link. tick_triggers fires `onEnter` on
    // this SimObject via Con::executef when the player crosses in.
    dts_viewer::TriggerState* mLive = nullptr;
    void setLiveTriggerState(dts_viewer::TriggerState* p) { mLive = p; }

    Trigger();
    static void initPersistFields();
    DECLARE_CONOBJECT(Trigger);
};

class VehiclePlaceholder : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mDataBlock;
    StringTableEntry mPos;
    StringTableEntry mVehicleDts;
    bool             mVisible;

    VehiclePlaceholder();
    static void initPersistFields();
    DECLARE_CONOBJECT(VehiclePlaceholder);
};

class Door : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mDataBlock;
    StringTableEntry mPos;
    bool             mOpen;

    Door();
    static void initPersistFields();
    DECLARE_CONOBJECT(Door);
};

class MissionMarker : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mDataBlock;
    StringTableEntry mPos;
    StringTableEntry mRot;

    MissionMarker();
    static void initPersistFields();
    DECLARE_CONOBJECT(MissionMarker);
};

class SimLight : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mPos;
    StringTableEntry mColor;       // "r g b a"
    F32              mRange;

    SimLight();
    static void initPersistFields();
    DECLARE_CONOBJECT(SimLight);
};

class Sky : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mMaterial;
    F32              mWindVelocity;

    Sky();
    static void initPersistFields();
    DECLARE_CONOBJECT(Sky);
};

class Planet : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mMaterial;
    StringTableEntry mPos;

    Planet();
    static void initPersistFields();
    DECLARE_CONOBJECT(Planet);
};

class Sensor : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mPos;
    S32              mTeam;
    F32              mRange;
    static int       sScanCalls;

    Sensor();
    static void initPersistFields();
    // Spec 16/10 — walks the MissionGroup and returns a
    // space-separated list of SimObject IDs whose pos field is
    // within `mRange` metres of this sensor. Replaces the spec
    // 16/06 stub that always returned 0.
    const char* scan();
    DECLARE_CONOBJECT(Sensor);
};

// Force-link anchor — invoked from cscript_host so the static
// IMPLEMENT_CONOBJECT registrations survive static-archive link.
namespace dts_viewer {
    void anchorEntityClasses();
}

#endif // DTS_VIEWER_ENTITY_BINDINGS_HPP
