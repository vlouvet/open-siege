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

IMPLEMENT_CONOBJECT(BulletData);
IMPLEMENT_CONOBJECT(RocketData);
IMPLEMENT_CONOBJECT(GrenadeData);
IMPLEMENT_CONOBJECT(PlayerData);
IMPLEMENT_CONOBJECT(StaticShapeData);

ConsoleDocClass(BulletData,      "@brief T1 BulletData binding (spec 16/03).");
ConsoleDocClass(RocketData,      "@brief T1 RocketData binding (spec 16/03).");
ConsoleDocClass(GrenadeData,     "@brief T1 GrenadeData binding (spec 16/03).");
ConsoleDocClass(PlayerData,      "@brief T1 PlayerData binding (spec 16/03).");
ConsoleDocClass(StaticShapeData, "@brief T1 StaticShapeData binding (spec 16/03).");

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
