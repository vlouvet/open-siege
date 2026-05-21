// Spec 16/03 — minimal datablock probe.
//
// Smaller, faster-cycling counterpart to cscript_datablock_test that
// exercises the bare minimum: Torque `datablock` keyword on a single
// SimDataBlock subclass with three addField'd typed fields (F32 / F32
// / TypeString). Regression-guards the FrameAllocator init wired into
// Con::init() — without it, SimObject::setDataField crashes inside the
// FrameTemp<char> scratch buffer.

#include "console/console.h"
#include "console/consoleInternal.h"
#include "console/consoleTypes.h"
#include "console/engineAPI.h"
#include "console/script.h"
#include "console/sim.h"
#include "console/simDatablock.h"
#include "console/torquescript/runtime.h"

#include <cstdio>
#include <cstring>

using namespace studio::content::cscript;

class ProbeData : public SimDataBlock
{
    typedef SimDataBlock Parent;
public:
    F32              mMass;
    F32              mSpeed;
    StringTableEntry mTag;

    ProbeData() : mMass(0), mSpeed(0), mTag(StringTable->insert("")) {}

    static void initPersistFields()
    {
        addField("mass",  TypeF32,    Offset(mMass,  ProbeData));
        addField("speed", TypeF32,    Offset(mSpeed, ProbeData));
        addField("tag",   TypeString, Offset(mTag,   ProbeData));
        Parent::initPersistFields();
    }
    DECLARE_CONOBJECT(ProbeData);
};
IMPLEMENT_CONOBJECT(ProbeData);
ConsoleDocClass(ProbeData, "@brief Spec 16/03 probe datablock.");

static int gPassed = 0, gFailed = 0;
static void check(bool c, const char* d)
{
    if (c) { ++gPassed; std::printf("  pass: %s\n", d); }
    else   { ++gFailed; std::fprintf(stderr, "  FAIL: %s\n", d); }
}
static void sink(unsigned int, const char*) {}

int main()
{
    Con::init();
    Sim::init();
    Con::addConsumer(sink);
    Con::registerRuntime(0, TorqueScript::getRuntime());

    Con::evaluate(
        "datablock ProbeData(MyProbe) {\n"
        "    mass  = 12;\n"
        "    speed = 7.5;\n"
        "    tag   = \"hi\";\n"
        "};\n",
        false, "probe");

    auto* db = dynamic_cast<ProbeData*>(Sim::findObject("MyProbe"));
    check(db != nullptr,                                       "MyProbe constructed");
    check(db && db->mMass == 12.0f,                            "mMass typed read == 12");
    check(db && db->mSpeed == 7.5f,                            "mSpeed typed read == 7.5");
    check(db && db->mTag && std::strcmp(db->mTag, "hi") == 0,  "mTag typed read == hi");

    std::printf("\ndatablock_probe: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
