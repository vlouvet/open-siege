// Open Siege spec 16/03 — Datablock end-to-end test.
//
// Exercises three layers:
//   1. T1 dialect-A datablock-syntax transform
//      (`BulletData ChaingunBullet { ... };` ->
//       `datablock BulletData(ChaingunBullet) { ... };`).
//   2. Typed-field reflection via addField (TypeF32, TypeS32, TypeString).
//   3. Real-corpus load — feed Tribes-1 baseProjData.cs through the
//      transform + Con::evaluate, then verify a handful of script-visible
//      values round-trip to typed C++ getters on the datablock instance.

#include "console/console.h"
#include "console/consoleInternal.h"
#include "console/consoleTypes.h"
#include "console/engineAPI.h"
#include "console/script.h"
#include "console/sim.h"
#include "console/simDatablock.h"
#include "console/torquescript/runtime.h"
#include "console/torquescript/dialectATransform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace studio::content::cscript;

// ---------------------------------------------------------------------------
// Typed datablock subclasses — kept intentionally small. The point is to
// prove the binding pattern. Spec 16/03 acceptance asks for ~20 of the 88
// catalogued T1 types, but the work to fully type ~80 fields per class is
// orthogonal to validating the binding chain — that's mechanical follow-up.
//
// The five classes covered here exercise the most common T1 datablocks:
// projectiles (BulletData, RocketData, GrenadeData), the player armor
// archetype (PlayerData), and a generic shape datablock (StaticShapeData).
// ---------------------------------------------------------------------------
#define COMMON_PROJECTILE_FIELDS(_CLS)                                    \
    F32              mMass;                                               \
    F32              mTotalTime;                                          \
    F32              mDamageValue;                                        \
    S32              mDamageType;                                         \
    StringTableEntry mBulletShapeName;                                    \
    _CLS()                                                                 \
      : mMass(0), mTotalTime(0), mDamageValue(0), mDamageType(0),         \
        mBulletShapeName(StringTable->insert("")) {}                      \
    static void initPersistFields()                                       \
    {                                                                     \
        addField("mass",            TypeF32,    Offset(mMass,            _CLS)); \
        addField("totalTime",       TypeF32,    Offset(mTotalTime,       _CLS)); \
        addField("damageValue",     TypeF32,    Offset(mDamageValue,     _CLS)); \
        addField("damageType",      TypeS32,    Offset(mDamageType,      _CLS)); \
        addField("bulletShapeName", TypeString, Offset(mBulletShapeName, _CLS)); \
        Parent::initPersistFields();                                      \
    }

class BulletData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    F32              mMuzzleVelocity;
    COMMON_PROJECTILE_FIELDS(BulletData)
    DECLARE_CONOBJECT(BulletData);
};
#undef COMMON_PROJECTILE_FIELDS

class RocketData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    F32              mMass;
    F32              mDamageValue;
    F32              mDamageRadius;
    StringTableEntry mProjectileShapeName;
    RocketData()
      : mMass(0), mDamageValue(0), mDamageRadius(0),
        mProjectileShapeName(StringTable->insert("")) {}
    static void initPersistFields()
    {
        addField("mass",                 TypeF32,    Offset(mMass,                 RocketData));
        addField("damageValue",          TypeF32,    Offset(mDamageValue,          RocketData));
        addField("damageRadius",         TypeF32,    Offset(mDamageRadius,         RocketData));
        addField("projectileShapeName",  TypeString, Offset(mProjectileShapeName,  RocketData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(RocketData);
};

class GrenadeData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    F32              mMass;
    F32              mDamageValue;
    StringTableEntry mGrenadeShapeName;
    GrenadeData()
      : mMass(0), mDamageValue(0),
        mGrenadeShapeName(StringTable->insert("")) {}
    static void initPersistFields()
    {
        addField("mass",             TypeF32,    Offset(mMass,             GrenadeData));
        addField("damageValue",      TypeF32,    Offset(mDamageValue,      GrenadeData));
        addField("grenadeShapeName", TypeString, Offset(mGrenadeShapeName, GrenadeData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(GrenadeData);
};

class PlayerData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    F32              mMaxJetForwardVelocity;
    F32              mJetForce;
    F32              mMaxForwardSpeed;
    F32              mMaxDamage;
    StringTableEntry mShapeFile;
    StringTableEntry mClassName;
    PlayerData()
      : mMaxJetForwardVelocity(0), mJetForce(0), mMaxForwardSpeed(0),
        mMaxDamage(0),
        mShapeFile(StringTable->insert("")),
        mClassName(StringTable->insert("")) {}
    static void initPersistFields()
    {
        addField("maxJetForwardVelocity", TypeF32,    Offset(mMaxJetForwardVelocity, PlayerData));
        addField("jetForce",              TypeF32,    Offset(mJetForce,              PlayerData));
        addField("maxForwardSpeed",       TypeF32,    Offset(mMaxForwardSpeed,       PlayerData));
        addField("maxDamage",             TypeF32,    Offset(mMaxDamage,             PlayerData));
        addField("shapeFile",             TypeString, Offset(mShapeFile,             PlayerData));
        addField("className",             TypeString, Offset(mClassName,             PlayerData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(PlayerData);
};

class StaticShapeData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mShapeFile;
    F32              mMass;
    StaticShapeData()
      : mShapeFile(StringTable->insert("")), mMass(0) {}
    static void initPersistFields()
    {
        addField("shapeFile", TypeString, Offset(mShapeFile, StaticShapeData));
        addField("mass",      TypeF32,    Offset(mMass,      StaticShapeData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(StaticShapeData);
};

// ---------------------------------------------------------------------------
// Spec 16/08 — 15 additional typed subclasses.
//
// Field surface for each was chosen by sampling the T1 corpus
// (/tmp/scripts/*.cs); each subclass binds 3–6 fields covering the
// script-visible essence. Generic-SimDataBlock fallback continues to
// catch the rest, so this expansion is about C++-side ergonomics
// rather than script-side functionality.
// ---------------------------------------------------------------------------

// ItemData — Tribes 1 covers weapons / ammo / vehicles / armor under one
// type, discriminated by `className`. Fields cover the visible essence.
class ItemData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mDescription;
    StringTableEntry mClassName;
    StringTableEntry mShapeFile;
    StringTableEntry mHudIcon;
    StringTableEntry mHeading;
    S32              mPrice;
    bool             mShowWeaponBar;
    ItemData()
      : mDescription(StringTable->insert("")),
        mClassName(StringTable->insert("")),
        mShapeFile(StringTable->insert("")),
        mHudIcon(StringTable->insert("")),
        mHeading(StringTable->insert("")),
        mPrice(0), mShowWeaponBar(false) {}
    static void initPersistFields()
    {
        addField("description",   TypeString, Offset(mDescription,   ItemData));
        addField("className",     TypeString, Offset(mClassName,     ItemData));
        addField("shapeFile",     TypeString, Offset(mShapeFile,     ItemData));
        addField("hudIcon",       TypeString, Offset(mHudIcon,       ItemData));
        addField("heading",       TypeString, Offset(mHeading,       ItemData));
        addField("price",         TypeS32,    Offset(mPrice,         ItemData));
        addField("showWeaponBar", TypeBool,   Offset(mShowWeaponBar, ItemData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(ItemData);
};

// ItemImageData — per-weapon visuals and firing tunables. Bound as the
// closest analogue to the spec's WeaponData (no `WeaponData` ships in T1).
class ItemImageData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mShapeFile;
    S32              mMountPoint;
    S32              mWeaponType;
    F32              mReloadTime;
    F32              mFireTime;
    bool             mAccuFire;
    ItemImageData()
      : mShapeFile(StringTable->insert("")),
        mMountPoint(0), mWeaponType(0),
        mReloadTime(0), mFireTime(0),
        mAccuFire(false) {}
    static void initPersistFields()
    {
        addField("shapeFile",  TypeString, Offset(mShapeFile,  ItemImageData));
        addField("mountPoint", TypeS32,    Offset(mMountPoint, ItemImageData));
        addField("weaponType", TypeS32,    Offset(mWeaponType, ItemImageData));
        addField("reloadTime", TypeF32,    Offset(mReloadTime, ItemImageData));
        addField("fireTime",   TypeF32,    Offset(mFireTime,   ItemImageData));
        addField("accuFire",   TypeBool,   Offset(mAccuFire,   ItemImageData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(ItemImageData);
};

class TurretData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mShapeFile;
    F32              mMaxDamage;
    F32              mScanRange;
    F32              mFireInterval;
    TurretData()
      : mShapeFile(StringTable->insert("")),
        mMaxDamage(0), mScanRange(0), mFireInterval(0) {}
    static void initPersistFields()
    {
        addField("shapeFile",    TypeString, Offset(mShapeFile,    TurretData));
        addField("maxDamage",    TypeF32,    Offset(mMaxDamage,    TurretData));
        addField("scanRange",    TypeF32,    Offset(mScanRange,    TurretData));
        addField("fireInterval", TypeF32,    Offset(mFireInterval, TurretData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(TurretData);
};

class SensorData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mShapeFile;
    F32              mDetectRadius;
    F32              mPingInterval;
    SensorData()
      : mShapeFile(StringTable->insert("")),
        mDetectRadius(0), mPingInterval(0) {}
    static void initPersistFields()
    {
        addField("shapeFile",    TypeString, Offset(mShapeFile,    SensorData));
        addField("detectRadius", TypeF32,    Offset(mDetectRadius, SensorData));
        addField("pingInterval", TypeF32,    Offset(mPingInterval, SensorData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(SensorData);
};

class ExplosionData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mShapeName;
    StringTableEntry mSoundId;
    F32              mTimeScale;
    F32              mLightRange;
    F32              mTimeZero;
    F32              mTimeOne;
    bool             mHasLight;
    ExplosionData()
      : mShapeName(StringTable->insert("")),
        mSoundId(StringTable->insert("")),
        mTimeScale(1.0f), mLightRange(0), mTimeZero(0), mTimeOne(0),
        mHasLight(false) {}
    static void initPersistFields()
    {
        addField("shapeName",  TypeString, Offset(mShapeName,  ExplosionData));
        addField("soundId",    TypeString, Offset(mSoundId,    ExplosionData));
        addField("timeScale",  TypeF32,    Offset(mTimeScale,  ExplosionData));
        addField("lightRange", TypeF32,    Offset(mLightRange, ExplosionData));
        addField("timeZero",   TypeF32,    Offset(mTimeZero,   ExplosionData));
        addField("timeOne",    TypeF32,    Offset(mTimeOne,    ExplosionData));
        addField("hasLight",   TypeBool,   Offset(mHasLight,   ExplosionData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(ExplosionData);
};

class DebrisData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    F32 mMass;
    F32 mElasticity;
    F32 mFriction;
    S32 mType;
    S32 mImageType;
    DebrisData() : mMass(0), mElasticity(0), mFriction(0),
                   mType(0), mImageType(0) {}
    static void initPersistFields()
    {
        addField("mass",       TypeF32, Offset(mMass,       DebrisData));
        addField("elasticity", TypeF32, Offset(mElasticity, DebrisData));
        addField("friction",   TypeF32, Offset(mFriction,   DebrisData));
        addField("type",       TypeS32, Offset(mType,       DebrisData));
        addField("imageType",  TypeS32, Offset(mImageType,  DebrisData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(DebrisData);
};

class DamageSkinData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mTextureName;
    F32              mDamageThreshold;
    DamageSkinData()
      : mTextureName(StringTable->insert("")), mDamageThreshold(0) {}
    static void initPersistFields()
    {
        addField("textureName",     TypeString, Offset(mTextureName,     DamageSkinData));
        addField("damageThreshold", TypeF32,    Offset(mDamageThreshold, DamageSkinData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(DamageSkinData);
};

class MarkerData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mShapeFile;
    S32              mMarkerType;
    MarkerData()
      : mShapeFile(StringTable->insert("")), mMarkerType(0) {}
    static void initPersistFields()
    {
        addField("shapeFile",  TypeString, Offset(mShapeFile,  MarkerData));
        addField("markerType", TypeS32,    Offset(mMarkerType, MarkerData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(MarkerData);
};

class MineData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mShapeFile;
    F32              mExplosionRadius;
    F32              mDamageValue;
    F32              mTriggerRadius;
    F32              mKickBackStrength;
    MineData()
      : mShapeFile(StringTable->insert("")),
        mExplosionRadius(0), mDamageValue(0),
        mTriggerRadius(0), mKickBackStrength(0) {}
    static void initPersistFields()
    {
        addField("shapeFile",        TypeString, Offset(mShapeFile,        MineData));
        addField("explosionRadius",  TypeF32,    Offset(mExplosionRadius,  MineData));
        addField("damageValue",      TypeF32,    Offset(mDamageValue,      MineData));
        addField("triggerRadius",    TypeF32,    Offset(mTriggerRadius,    MineData));
        addField("kickBackStrength", TypeF32,    Offset(mKickBackStrength, MineData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(MineData);
};

class SoundData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mFileName;
    F32              mVolume;
    SoundData() : mFileName(StringTable->insert("")), mVolume(1.0f) {}
    static void initPersistFields()
    {
        addField("fileName", TypeString, Offset(mFileName, SoundData));
        addField("volume",   TypeF32,    Offset(mVolume,   SoundData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(SoundData);
};

class SoundProfileData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    F32  mBaseVolume;
    F32  mRefDistance;
    F32  mMaxDistance;
    bool mIs3D;
    bool mLooping;
    SoundProfileData()
      : mBaseVolume(1.0f), mRefDistance(1.0f), mMaxDistance(0),
        mIs3D(false), mLooping(false) {}
    static void initPersistFields()
    {
        addField("baseVolume",  TypeF32,  Offset(mBaseVolume,  SoundProfileData));
        addField("refDistance", TypeF32,  Offset(mRefDistance, SoundProfileData));
        addField("maxDistance", TypeF32,  Offset(mMaxDistance, SoundProfileData));
        addField("is3D",        TypeBool, Offset(mIs3D,        SoundProfileData));
        addField("looping",     TypeBool, Offset(mLooping,     SoundProfileData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(SoundProfileData);
};

class FlierData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mShapeFile;
    F32              mMass;
    F32              mMaxThrust;
    F32              mMaxSpeed;
    FlierData()
      : mShapeFile(StringTable->insert("")),
        mMass(0), mMaxThrust(0), mMaxSpeed(0) {}
    static void initPersistFields()
    {
        addField("shapeFile", TypeString, Offset(mShapeFile, FlierData));
        addField("mass",      TypeF32,    Offset(mMass,      FlierData));
        addField("maxThrust", TypeF32,    Offset(mMaxThrust, FlierData));
        addField("maxSpeed",  TypeF32,    Offset(mMaxSpeed,  FlierData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(FlierData);
};

class MoveableData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mShapeFile;
    F32              mCloseTime;
    F32              mDwellTime;
    MoveableData()
      : mShapeFile(StringTable->insert("")),
        mCloseTime(0), mDwellTime(0) {}
    static void initPersistFields()
    {
        addField("shapeFile", TypeString, Offset(mShapeFile, MoveableData));
        addField("closeTime", TypeF32,    Offset(mCloseTime, MoveableData));
        addField("dwellTime", TypeF32,    Offset(mDwellTime, MoveableData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(MoveableData);
};

class LightningData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mBitmapName;
    F32              mBoltLength;
    F32              mConeAngle;
    F32              mDamagePerSec;
    F32              mEnergyDrainPerSec;
    LightningData()
      : mBitmapName(StringTable->insert("")),
        mBoltLength(0), mConeAngle(0),
        mDamagePerSec(0), mEnergyDrainPerSec(0) {}
    static void initPersistFields()
    {
        addField("bitmapName",        TypeString, Offset(mBitmapName,        LightningData));
        addField("boltLength",        TypeF32,    Offset(mBoltLength,        LightningData));
        addField("coneAngle",         TypeF32,    Offset(mConeAngle,         LightningData));
        addField("damagePerSec",      TypeF32,    Offset(mDamagePerSec,      LightningData));
        addField("energyDrainPerSec", TypeF32,    Offset(mEnergyDrainPerSec, LightningData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(LightningData);
};

class LaserData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    StringTableEntry mLaserBitmapName;
    F32              mDamageConversion;
    F32              mBeamTime;
    F32              mLightRange;
    LaserData()
      : mLaserBitmapName(StringTable->insert("")),
        mDamageConversion(0), mBeamTime(0), mLightRange(0) {}
    static void initPersistFields()
    {
        addField("laserBitmapName",  TypeString, Offset(mLaserBitmapName,  LaserData));
        addField("damageConversion", TypeF32,    Offset(mDamageConversion, LaserData));
        addField("beamTime",         TypeF32,    Offset(mBeamTime,         LaserData));
        addField("lightRange",       TypeF32,    Offset(mLightRange,       LaserData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(LaserData);
};

IMPLEMENT_CONOBJECT(BulletData);
IMPLEMENT_CONOBJECT(RocketData);
IMPLEMENT_CONOBJECT(GrenadeData);
IMPLEMENT_CONOBJECT(PlayerData);
IMPLEMENT_CONOBJECT(StaticShapeData);
IMPLEMENT_CONOBJECT(ItemData);
IMPLEMENT_CONOBJECT(ItemImageData);
IMPLEMENT_CONOBJECT(TurretData);
IMPLEMENT_CONOBJECT(SensorData);
IMPLEMENT_CONOBJECT(ExplosionData);
IMPLEMENT_CONOBJECT(DebrisData);
IMPLEMENT_CONOBJECT(DamageSkinData);
IMPLEMENT_CONOBJECT(MarkerData);
IMPLEMENT_CONOBJECT(MineData);
IMPLEMENT_CONOBJECT(SoundData);
IMPLEMENT_CONOBJECT(SoundProfileData);
IMPLEMENT_CONOBJECT(FlierData);
IMPLEMENT_CONOBJECT(MoveableData);
IMPLEMENT_CONOBJECT(LightningData);
IMPLEMENT_CONOBJECT(LaserData);

ConsoleDocClass(BulletData,      "@brief T1 BulletData binding (spec 16/03).");
ConsoleDocClass(RocketData,      "@brief T1 RocketData binding (spec 16/03).");
ConsoleDocClass(GrenadeData,     "@brief T1 GrenadeData binding (spec 16/03).");
ConsoleDocClass(PlayerData,      "@brief T1 PlayerData binding (spec 16/03).");
ConsoleDocClass(StaticShapeData, "@brief T1 StaticShapeData binding (spec 16/03).");
ConsoleDocClass(ItemData,        "@brief T1 ItemData binding (spec 16/08).");
ConsoleDocClass(ItemImageData,   "@brief T1 ItemImageData binding (spec 16/08).");
ConsoleDocClass(TurretData,      "@brief T1 TurretData binding (spec 16/08).");
ConsoleDocClass(SensorData,      "@brief T1 SensorData binding (spec 16/08).");
ConsoleDocClass(ExplosionData,   "@brief T1 ExplosionData binding (spec 16/08).");
ConsoleDocClass(DebrisData,      "@brief T1 DebrisData binding (spec 16/08).");
ConsoleDocClass(DamageSkinData,  "@brief T1 DamageSkinData binding (spec 16/08).");
ConsoleDocClass(MarkerData,      "@brief T1 MarkerData binding (spec 16/08).");
ConsoleDocClass(MineData,        "@brief T1 MineData binding (spec 16/08).");
ConsoleDocClass(SoundData,       "@brief T1 SoundData binding (spec 16/08).");
ConsoleDocClass(SoundProfileData,"@brief T1 SoundProfileData binding (spec 16/08).");
ConsoleDocClass(FlierData,       "@brief T1 FlierData binding (spec 16/08).");
ConsoleDocClass(MoveableData,    "@brief T1 MoveableData binding (spec 16/08).");
ConsoleDocClass(LightningData,   "@brief T1 LightningData binding (spec 16/08).");
ConsoleDocClass(LaserData,       "@brief T1 LaserData binding (spec 16/08).");

// ---------------------------------------------------------------------------
static int gPassed = 0;
static int gFailed = 0;
static std::vector<std::string> gFailures;

static void check(bool cond, const char* desc)
{
    if (cond) { ++gPassed; std::printf("  pass: %s\n", desc); }
    else { ++gFailed; gFailures.push_back(desc);
           std::fprintf(stderr, "  FAIL: %s\n", desc); }
}

static void sink(unsigned int lvl, const char* line)
{
    if (getenv("VERBOSE")) std::printf("    [vm %u] %s\n", lvl, line);
}

template <typename T>
static T* lookup(const char* name)
{
    return dynamic_cast<T*>(Sim::findObject(name));
}

static std::string readFile(const char* path)
{
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return {};
    std::fseek(fp, 0, SEEK_END);
    long len = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    std::string out(len, '\0');
    std::fread(&out[0], 1, len, fp);
    std::fclose(fp);
    return out;
}

// ---------------------------------------------------------------------------
static void testTransformUnit()
{
    std::printf("\n[group] dialectA-transform unit\n");

    // Same-line form.
    {
        const std::string before = "MarkerData M { foo = 1; };";
        const std::string after  = transformTribes1Datablocks(before);
        check(after.find("datablock MarkerData(M)") != std::string::npos,
              "same-line: 'MarkerData M {' rewritten");
    }

    // Split-line form (typename + instance on one line, '{' on the next).
    {
        const std::string before = "BulletData ChaingunBullet\n{\n  mass = 0.05;\n};";
        const std::string after  = transformTribes1Datablocks(before);
        check(after.find("datablock BulletData(ChaingunBullet)") != std::string::npos,
              "split-line: 'BulletData X\\n{' rewritten");
    }

    // Already-Torque shape — must be unchanged.
    {
        const std::string before = "datablock BulletData(X) { mass = 1; };";
        const std::string after  = transformTribes1Datablocks(before);
        check(after == before,
              "idempotent on Torque-shaped input");
    }

    // No false positive on identifier with similar-looking name inside a
    // function call: `getItemData(123)` must not become `datablock getItemData(123)`.
    {
        const std::string before = "$x = getItemData(123);";
        const std::string after  = transformTribes1Datablocks(before);
        check(after.find("datablock") == std::string::npos,
              "no false-positive on function call");
    }

    // String literals are untouched.
    {
        const std::string before = "$msg = \"BulletData X { y = 1; };\";";
        const std::string after  = transformTribes1Datablocks(before);
        check(after.find("datablock") == std::string::npos,
              "string literal content not transformed");
    }

    // Two declarations back-to-back.
    {
        const std::string before =
            "BulletData A { mass = 1; };\n"
            "BulletData B { mass = 2; };\n";
        const std::string after = transformTribes1Datablocks(before);
        bool hasA = after.find("datablock BulletData(A)") != std::string::npos;
        bool hasB = after.find("datablock BulletData(B)") != std::string::npos;
        check(hasA && hasB, "two back-to-back declarations both rewrite");
    }
}

// ---------------------------------------------------------------------------
static void testTypedDatablock()
{
    std::printf("\n[group] typed datablock via 'datablock' keyword\n");
    Con::evaluate(
        "datablock BulletData(MyBullet) {\n"
        "    mass            = 0.05;\n"
        "    totalTime       = 1.5;\n"
        "    damageValue     = 0.11;\n"
        "    damageType      = 1;\n"
        "    bulletShapeName = \"bullet.dts\";\n"
        "};\n",
        false, "datablock.test");

    BulletData* b = lookup<BulletData>("MyBullet");
    check(b != nullptr, "Sim::findObject(\"MyBullet\") -> BulletData*");
    if (b)
    {
        check(b->mMass == 0.05f,                              "BulletData::mMass = 0.05");
        check(b->mTotalTime == 1.5f,                          "BulletData::mTotalTime = 1.5");
        check(b->mDamageValue == 0.11f,                       "BulletData::mDamageValue = 0.11");
        check(b->mDamageType == 1,                            "BulletData::mDamageType = 1");
        check(std::strcmp(b->mBulletShapeName, "bullet.dts") == 0,
                                                              "BulletData::mBulletShapeName = bullet.dts");
    }
}

// ---------------------------------------------------------------------------
static void testT1Transform()
{
    std::printf("\n[group] dialect-A round-trip (transform + evaluate)\n");
    const char* src =
        "BulletData FusionBolt\n"
        "{\n"
        "   bulletShapeName    = \"fusionbolt.dts\";\n"
        "   mass               = 0.05;\n"
        "   damageValue        = 0.25;\n"
        "   damageType         = 2;\n"
        "   totalTime          = 1.5;\n"
        "};\n";

    std::string t = transformTribes1Datablocks(src);
    Con::evaluate(t.c_str(), false, "FusionBolt.cs");

    BulletData* b = lookup<BulletData>("FusionBolt");
    check(b != nullptr, "FusionBolt loaded via T1 syntax + transform");
    if (b)
    {
        check(b->mDamageValue == 0.25f,
              "FusionBolt.damageValue == 0.25 (T1 form, typed read)");
        check(std::strcmp(b->mBulletShapeName, "fusionbolt.dts") == 0,
              "FusionBolt.bulletShapeName == fusionbolt.dts");
    }
}

// ---------------------------------------------------------------------------
static void testRealCorpus()
{
    std::printf("\n[group] real-corpus baseProjData.cs\n");
    std::string raw = readFile("/tmp/scripts/baseProjData.cs");
    if (raw.empty())
    {
        std::printf("  (skip: /tmp/scripts/baseProjData.cs not present)\n");
        return;
    }
    std::string t = transformTribes1Datablocks(raw);

    // Diagnostic: dump the first failing zone so we can see what the
    // transform produced.
    if (const char* dbg = getenv("DUMP_TRANSFORM"))
    {
        (void)dbg;
        std::printf("=== transform output (first 4KB) ===\n");
        std::fwrite(t.data(), 1, std::min<std::size_t>(t.size(), 4096), stdout);
        std::printf("\n=== end ===\n");
    }

    // Count rewrites — every "datablock <Word>Data(" we count.
    int rewrites = 0;
    for (std::size_t p = 0; ; )
    {
        std::size_t hit = t.find("datablock ", p);
        if (hit == std::string::npos) break;
        ++rewrites; p = hit + 1;
    }
    std::printf("  baseProjData.cs: %d datablock declarations rewritten\n", rewrites);
    check(rewrites >= 10, "baseProjData.cs produced >=10 rewrites");

    Con::evaluate(t.c_str(), false, "baseProjData.cs");

    BulletData* chain = lookup<BulletData>("ChaingunBullet");
    check(chain != nullptr, "ChaingunBullet datablock loaded");
    if (chain)
    {
        check(chain->mMass == 0.05f,
              "ChaingunBullet.mass == 0.05 (real corpus, typed read)");
        check(std::strcmp(chain->mBulletShapeName, "bullet.dts") == 0,
              "ChaingunBullet.bulletShapeName == bullet.dts (real corpus)");
    }
    BulletData* fusion = lookup<BulletData>("FusionBolt");
    check(fusion != nullptr, "FusionBolt datablock loaded from corpus");
    BulletData* mini   = lookup<BulletData>("MiniFusionBolt");
    BulletData* blast  = lookup<BulletData>("BlasterBolt");
    BulletData* plasma = lookup<BulletData>("PlasmaBolt");
    check(mini != nullptr,   "MiniFusionBolt loaded");
    check(blast != nullptr,  "BlasterBolt loaded");
    check(plasma != nullptr, "PlasmaBolt loaded");
    RocketData* disc = lookup<RocketData>("DiscShell");
    check(disc != nullptr, "DiscShell (RocketData) loaded from corpus");
    GrenadeData* grenade = lookup<GrenadeData>("GrenadeShell");
    check(grenade != nullptr, "GrenadeShell (GrenadeData) loaded from corpus");
}

// ---------------------------------------------------------------------------
// Spec 16/08 — one round-trip per new subclass. Each test constructs an
// instance via the `datablock` keyword (typed slot path) and verifies a
// representative field reads back through the typed C++ getter.
// ---------------------------------------------------------------------------
static void testExpansionRoundTrips()
{
    std::printf("\n[group] spec 16/08 expansion round-trips\n");

    auto run = [](const char* tag, const char* src) {
        Con::evaluate(src, false, tag);
    };

    run("ItemData", "datablock ItemData(MortarTest) {\n"
        "  description = \"Mortar\";\n"
        "  className = \"Weapon\";\n"
        "  price = 375;\n"
        "  showWeaponBar = true;\n"
        "};\n");
    if (auto* d = lookup<ItemData>("MortarTest")) {
        check(std::strcmp(d->mClassName, "Weapon") == 0, "ItemData.className=Weapon");
        check(d->mPrice == 375,                          "ItemData.price=375");
        check(d->mShowWeaponBar == true,                 "ItemData.showWeaponBar=true");
    } else check(false, "ItemData round-trip lookup");

    run("ItemImageData", "datablock ItemImageData(MortarImageTest) {\n"
        "  shapeFile = \"mortargun\";\n"
        "  reloadTime = 0.5;\n"
        "  fireTime = 2.0;\n"
        "  accuFire = false;\n"
        "};\n");
    if (auto* d = lookup<ItemImageData>("MortarImageTest")) {
        check(d->mFireTime == 2.0f, "ItemImageData.fireTime=2.0");
        check(d->mReloadTime == 0.5f, "ItemImageData.reloadTime=0.5");
    } else check(false, "ItemImageData round-trip lookup");

    run("TurretData", "datablock TurretData(AAT) {\n"
        "  shapeFile = \"aaturret\";\n"
        "  maxDamage = 200;\n"
        "  scanRange = 250;\n"
        "};\n");
    if (auto* d = lookup<TurretData>("AAT")) {
        check(d->mMaxDamage == 200.0f, "TurretData.maxDamage=200");
        check(d->mScanRange == 250.0f, "TurretData.scanRange=250");
    } else check(false, "TurretData round-trip lookup");

    run("SensorData", "datablock SensorData(SensorPulse) {\n"
        "  detectRadius = 150;\n"
        "  pingInterval = 1.5;\n"
        "};\n");
    if (auto* d = lookup<SensorData>("SensorPulse")) {
        check(d->mDetectRadius == 150.0f, "SensorData.detectRadius=150");
        check(d->mPingInterval == 1.5f,   "SensorData.pingInterval=1.5");
    } else check(false, "SensorData round-trip lookup");

    run("ExplosionData", "datablock ExplosionData(rocketExpT) {\n"
        "  shapeName = \"bluex.dts\";\n"
        "  timeScale = 1.5;\n"
        "  hasLight = true;\n"
        "  lightRange = 8.0;\n"
        "};\n");
    if (auto* d = lookup<ExplosionData>("rocketExpT")) {
        check(d->mTimeScale == 1.5f,   "ExplosionData.timeScale=1.5");
        check(d->mLightRange == 8.0f,  "ExplosionData.lightRange=8.0");
        check(d->mHasLight == true,    "ExplosionData.hasLight=true");
    } else check(false, "ExplosionData round-trip lookup");

    run("DebrisData", "datablock DebrisData(debrisSmallT) {\n"
        "  mass = 100;\n"
        "  elasticity = 0.25;\n"
        "  friction = 0.5;\n"
        "};\n");
    if (auto* d = lookup<DebrisData>("debrisSmallT")) {
        check(d->mMass == 100.0f, "DebrisData.mass=100");
        check(d->mElasticity == 0.25f, "DebrisData.elasticity=0.25");
    } else check(false, "DebrisData round-trip lookup");

    run("DamageSkinData", "datablock DamageSkinData(armorSkinT) {\n"
        "  textureName = \"armorhurt.bmp\";\n"
        "  damageThreshold = 0.5;\n"
        "};\n");
    if (auto* d = lookup<DamageSkinData>("armorSkinT")) {
        check(d->mDamageThreshold == 0.5f, "DamageSkinData.damageThreshold=0.5");
    } else check(false, "DamageSkinData round-trip lookup");

    run("MarkerData", "datablock MarkerData(wpT) {\n"
        "  shapeFile = \"wp\";\n"
        "  markerType = 3;\n"
        "};\n");
    if (auto* d = lookup<MarkerData>("wpT")) {
        check(d->mMarkerType == 3, "MarkerData.markerType=3");
    } else check(false, "MarkerData round-trip lookup");

    run("MineData", "datablock MineData(APMineT) {\n"
        "  shapeFile = \"mine\";\n"
        "  explosionRadius = 10;\n"
        "  damageValue = 0.65;\n"
        "  triggerRadius = 2.5;\n"
        "};\n");
    if (auto* d = lookup<MineData>("APMineT")) {
        check(d->mExplosionRadius == 10.0f, "MineData.explosionRadius=10");
        check(d->mTriggerRadius == 2.5f,    "MineData.triggerRadius=2.5");
    } else check(false, "MineData round-trip lookup");

    run("SoundData", "datablock SoundData(footstepT) {\n"
        "  fileName = \"footstep.wav\";\n"
        "  volume = 0.8;\n"
        "};\n");
    if (auto* d = lookup<SoundData>("footstepT")) {
        check(d->mVolume == 0.8f, "SoundData.volume=0.8");
    } else check(false, "SoundData round-trip lookup");

    run("SoundProfileData", "datablock SoundProfileData(profile3dT) {\n"
        "  baseVolume = 0.5;\n"
        "  refDistance = 10;\n"
        "  maxDistance = 100;\n"
        "  is3D = true;\n"
        "};\n");
    if (auto* d = lookup<SoundProfileData>("profile3dT")) {
        check(d->mBaseVolume == 0.5f,   "SoundProfileData.baseVolume=0.5");
        check(d->mRefDistance == 10.0f, "SoundProfileData.refDistance=10");
        check(d->mIs3D == true,         "SoundProfileData.is3D=true");
    } else check(false, "SoundProfileData round-trip lookup");

    run("FlierData", "datablock FlierData(wildcatT) {\n"
        "  shapeFile = \"wildcat\";\n"
        "  mass = 500;\n"
        "  maxThrust = 40;\n"
        "  maxSpeed = 60;\n"
        "};\n");
    if (auto* d = lookup<FlierData>("wildcatT")) {
        check(d->mMass == 500.0f, "FlierData.mass=500");
        check(d->mMaxSpeed == 60.0f, "FlierData.maxSpeed=60");
    } else check(false, "FlierData round-trip lookup");

    run("MoveableData", "datablock MoveableData(elev4x4T) {\n"
        "  shapeFile = \"elevator_4x4\";\n"
        "  closeTime = 4.0;\n"
        "  dwellTime = 2.0;\n"
        "};\n");
    if (auto* d = lookup<MoveableData>("elev4x4T")) {
        check(d->mCloseTime == 4.0f, "MoveableData.closeTime=4.0");
        check(d->mDwellTime == 2.0f, "MoveableData.dwellTime=2.0");
    } else check(false, "MoveableData round-trip lookup");

    run("LightningData", "datablock LightningData(lightningChargeT) {\n"
        "  bitmapName = \"lightning.bmp\";\n"
        "  boltLength = 40;\n"
        "  coneAngle = 35;\n"
        "  damagePerSec = 0.06;\n"
        "  energyDrainPerSec = 60;\n"
        "};\n");
    if (auto* d = lookup<LightningData>("lightningChargeT")) {
        check(d->mBoltLength == 40.0f,         "LightningData.boltLength=40");
        check(d->mEnergyDrainPerSec == 60.0f,  "LightningData.energyDrainPerSec=60");
    } else check(false, "LightningData round-trip lookup");

    run("LaserData", "datablock LaserData(sniperLaserT) {\n"
        "  laserBitmapName = \"laserPulse.bmp\";\n"
        "  damageConversion = 0.007;\n"
        "  beamTime = 0.5;\n"
        "  lightRange = 2.0;\n"
        "};\n");
    if (auto* d = lookup<LaserData>("sniperLaserT")) {
        check(d->mBeamTime == 0.5f,    "LaserData.beamTime=0.5");
        check(d->mLightRange == 2.0f,  "LaserData.lightRange=2.0");
    } else check(false, "LaserData round-trip lookup");
}

// ---------------------------------------------------------------------------
int main()
{
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
    std::printf("cscript_datablock_test: starting up\n");

    Con::init();
    Sim::init();
    Con::addConsumer(sink);
    Con::registerRuntime(0, TorqueScript::getRuntime());
    std::printf("cscript_datablock_test: VM ready\n");

    testTransformUnit();
    testTypedDatablock();
    testT1Transform();
    testExpansionRoundTrips();
    testRealCorpus();

    std::printf("\n========================================================\n");
    std::printf("cscript_datablock_test: %d passed, %d failed\n", gPassed, gFailed);
    if (!gFailures.empty())
    {
        std::printf("failures:\n");
        for (const auto& s : gFailures) std::printf("  - %s\n", s.c_str());
    }
    return gFailed == 0 ? 0 : 1;
}
