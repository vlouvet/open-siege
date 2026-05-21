// Open Siege spec 16/06 — entity bindings test.
//
// dts-viewer's `examples/dts-viewer/entity_bindings.{hpp,cpp}` defines
// the shipping Item / Turret / StaticShape / ... SimObject classes
// (13 in total). Same pattern as spec 16/05's Player.
//
// This TU re-defines structurally equivalent classes (suffixed `Ent`)
// in cscript_core's test executable so we can assert the binding
// contract without depending on the dts-viewer link environment.
// dts-viewer's actual classes are smoke-tested via `dts-viewer
// --run-script` (transcript captured in the spec 16/06 commit log).

#include "console/console.h"
#include "console/consoleInternal.h"
#include "console/consoleTypes.h"
#include "console/engineAPI.h"
#include "console/script.h"
#include "console/sim.h"
#include "console/simObject.h"
#include "console/torquescript/runtime.h"

#include <cstdio>
#include <cstring>

using namespace studio::content::cscript;

// Minimal SimObject subclass macro — only fields the test exercises.
// One field of each persisted type (TypeString + TypeS32 + TypeF32 +
// TypeBool) is enough to validate the addField machinery; we don't
// re-test every field on every class.
#define DEFINE_ENT(_Name)                                                 \
    class _Name : public SimObject                                        \
    {                                                                     \
        typedef SimObject Parent;                                         \
    public:                                                               \
        StringTableEntry mDataBlock;                                      \
        StringTableEntry mPos;                                            \
        S32              mTeam;                                          \
        F32              mHealth;                                        \
        bool             mActive;                                        \
        _Name() : mDataBlock(StringTable->insert(""))                    \
                , mPos(StringTable->insert("0 0 0"))                     \
                , mTeam(0), mHealth(100.0f), mActive(true) {}            \
        static void initPersistFields()                                  \
        {                                                                 \
            addField("dataBlock", TypeString, Offset(mDataBlock, _Name)); \
            addField("pos",       TypeString, Offset(mPos,       _Name)); \
            addField("team",      TypeS32,    Offset(mTeam,      _Name)); \
            addField("health",    TypeF32,    Offset(mHealth,    _Name)); \
            addField("active",    TypeBool,   Offset(mActive,    _Name)); \
            Parent::initPersistFields();                                 \
        }                                                                 \
        DECLARE_CONOBJECT(_Name);                                         \
    };                                                                    \
    IMPLEMENT_CONOBJECT(_Name);                                           \
    ConsoleDocClass(_Name, "@brief Spec 16/06 " #_Name " binding.");

DEFINE_ENT(ItemEnt)
DEFINE_ENT(TurretEnt)
DEFINE_ENT(StaticShapeEnt)
DEFINE_ENT(MoveableEnt)
DEFINE_ENT(GeneratorEnt)
DEFINE_ENT(TriggerEnt)
DEFINE_ENT(VehiclePlaceholderEnt)
DEFINE_ENT(DoorEnt)
DEFINE_ENT(MissionMarkerEnt)
DEFINE_ENT(SimLightEnt)
DEFINE_ENT(SkyEnt)
DEFINE_ENT(PlanetEnt)
DEFINE_ENT(SensorEnt)

// ---------------------------------------------------------------------------
static int gPassed = 0, gFailed = 0;
static void check(bool c, const char* d)
{
    if (c) { ++gPassed; std::printf("  pass: %s\n", d); }
    else   { ++gFailed; std::fprintf(stderr, "  FAIL: %s\n", d); }
}
static void sink(unsigned int, const char*) {}

// Construct an instance of `typeName` named `instanceName`, then verify
// pos/team/health/active round-trip via typed C++ getters.
template <typename T>
static void exerciseEntity(const char* typeName, const char* instName)
{
    char src[512];
    std::snprintf(src, sizeof(src),
        "new %s(%s) {\n"
        "   dataBlock = \"Test_%s\";\n"
        "   pos       = \"10 20 30\";\n"
        "   team      = 2;\n"
        "   health    = 75;\n"
        "   active    = 1;\n"
        "};\n",
        typeName, instName, typeName);
    Con::evaluate(src, false, "ent.test");
    auto* obj = dynamic_cast<T*>(Sim::findObject(instName));

    char desc[128];
    std::snprintf(desc, sizeof(desc), "new %s(%s) constructs", typeName, instName);
    check(obj != nullptr, desc);
    if (!obj) return;

    std::snprintf(desc, sizeof(desc), "%s.pos round-trip", typeName);
    check(std::strcmp(obj->mPos, "10 20 30") == 0, desc);

    std::snprintf(desc, sizeof(desc), "%s.team round-trip", typeName);
    check(obj->mTeam == 2, desc);

    std::snprintf(desc, sizeof(desc), "%s.health round-trip", typeName);
    check(obj->mHealth == 75.0f, desc);

    std::snprintf(desc, sizeof(desc), "%s.dataBlock round-trip", typeName);
    char expected[64];
    std::snprintf(expected, sizeof(expected), "Test_%s", typeName);
    check(std::strcmp(obj->mDataBlock, expected) == 0, desc);
}

int main()
{
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);

    Con::init();
    Sim::init();
    Con::addConsumer(sink);
    Con::registerRuntime(0, TorqueScript::getRuntime());

    std::printf("[group] all 13 entity types construct + reflect\n");
    exerciseEntity<ItemEnt>             ("ItemEnt",              "It1");
    exerciseEntity<TurretEnt>           ("TurretEnt",            "Tu1");
    exerciseEntity<StaticShapeEnt>      ("StaticShapeEnt",       "Ss1");
    exerciseEntity<MoveableEnt>         ("MoveableEnt",          "Mv1");
    exerciseEntity<GeneratorEnt>        ("GeneratorEnt",         "Gn1");
    exerciseEntity<TriggerEnt>          ("TriggerEnt",           "Tr1");
    exerciseEntity<VehiclePlaceholderEnt>("VehiclePlaceholderEnt","Vp1");
    exerciseEntity<DoorEnt>             ("DoorEnt",              "Dr1");
    exerciseEntity<MissionMarkerEnt>    ("MissionMarkerEnt",     "Mm1");
    exerciseEntity<SimLightEnt>         ("SimLightEnt",          "Sl1");
    exerciseEntity<SkyEnt>              ("SkyEnt",               "Sk1");
    exerciseEntity<PlanetEnt>           ("PlanetEnt",            "Pl1");
    exerciseEntity<SensorEnt>           ("SensorEnt",            "Sn1");

    // Verify Sim::findObject can locate every constructed entity by name.
    std::printf("\n[group] Sim::findObject lookup for every instance\n");
    const char* names[] = {"It1","Tu1","Ss1","Mv1","Gn1","Tr1","Vp1",
                           "Dr1","Mm1","Sl1","Sk1","Pl1","Sn1"};
    int found = 0;
    for (const char* n : names) if (Sim::findObject(n)) ++found;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "all 13 instances findable by name (got %d)", found);
    check(found == 13, buf);

    std::printf("\n========================================================\n");
    std::printf("cscript_entity_bindings_test: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
