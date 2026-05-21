// Open Siege spec 16/02 — SimSet / SimGroup container proof.
//
// Spec 15/02b vendored simSet.cpp + simObject.cpp + simObjectList.cpp into
// cscript_core; the engine methods (addObject, removeObject, getCount,
// getObject, getGroup, delete) were registered via DefineEngineMethod /
// IMPLEMENT_CONOBJECT and already live in the static library.
//
// This TU exercises them from script and asserts behaviour matches the
// Torque contract:
//
//   1. Nested-construction: `instant SimGroup(G1) { instant SimGroup(G2) { ... }; };`
//      builds the tree and links group-of-group parentage.
//   2. Indexing: `G1.getObject(0)` returns G2; `G2.getObject(0)` returns P.
//   3. Reverse navigation: `P.getGroup()` returns G2 (single-group rule).
//   4. Cascade lifetime: deleting G1 destroys G2 and P with it.
//   5. add / remove / isMember / clear / getCount script bindings work.

#include "console/console.h"
#include "console/consoleInternal.h"
#include "console/engineAPI.h"
#include "console/script.h"
#include "console/sim.h"
#include "console/simObject.h"
#include "console/simSet.h"
#include "console/torquescript/runtime.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace studio::content::cscript;

// ---------------------------------------------------------------------------
// GroupMember — a script-creatable SimObject leaf used inside SimGroups.
// Declared in this TU so its IMPLEMENT_CONOBJECT is in the test binary
// (libcscript_core.a is a static archive; unreferenced TUs get dropped).
// ---------------------------------------------------------------------------
class GroupMember : public SimObject
{
    typedef SimObject Parent;
public:
    static int sLiveCount;
    GroupMember()  { ++sLiveCount; }
    ~GroupMember() { --sLiveCount; }
    DECLARE_CONOBJECT(GroupMember);
};

int GroupMember::sLiveCount = 0;
IMPLEMENT_CONOBJECT(GroupMember);

ConsoleDocClass(GroupMember,
    "@brief Spec 16/02 sim-group-test leaf object. Tracks live-count to verify"
    " SimGroup cascade destruction.");

// ---------------------------------------------------------------------------
static int gPassed = 0;
static int gFailed = 0;

static void check(bool cond, const char* desc)
{
    if (cond) { ++gPassed; std::printf("  pass: %s\n", desc); }
    else      { ++gFailed; std::fprintf(stderr, "  FAIL: %s\n", desc); }
}

static void sink(unsigned int /*level*/, const char* /*line*/) { /* swallow */ }

static SimObject* findByName(const char* name) { return Sim::findObject(name); }

int main()
{
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);

    std::printf("cscript_simgroup_test: starting up\n");
    Con::init();
    Sim::init();
    Con::addConsumer(sink);
    Con::registerRuntime(0, TorqueScript::getRuntime());
    std::printf("cscript_simgroup_test: VM + Sim initialized\n\n");

    // -----------------------------------------------------------------------
    // 1. Build the nested tree in one evaluate call.
    //    G1
    //    └── G2
    //        └── P (GroupMember)
    // -----------------------------------------------------------------------
    std::printf("[group] nested construction\n");
    const int beforeMembers = GroupMember::sLiveCount;
    Con::evaluate(
        "instant SimGroup(G1) {\n"
        "    instant SimGroup(G2) {\n"
        "        instant GroupMember(P) { };\n"
        "    };\n"
        "};\n",
        false, "simgroup.test");

    SimObject* g1 = findByName("G1");
    SimObject* g2 = findByName("G2");
    SimObject* p  = findByName("P");
    check(g1 != nullptr, "Sim::findObject(\"G1\") returns the outer group");
    check(g2 != nullptr, "Sim::findObject(\"G2\") returns the inner group");
    check(p  != nullptr, "Sim::findObject(\"P\") returns the leaf member");

    auto* g1Grp = dynamic_cast<SimGroup*>(g1);
    auto* g2Grp = dynamic_cast<SimGroup*>(g2);
    check(g1Grp != nullptr, "G1 is a SimGroup");
    check(g2Grp != nullptr, "G2 is a SimGroup");
    check(GroupMember::sLiveCount == beforeMembers + 1,
          "GroupMember live count incremented by 1 after construction");

    // -----------------------------------------------------------------------
    // 2. Indexing via script — G1.getObject(0) == G2; G2.getObject(0) == P.
    //    SimGroup::getObject returns the SimObject's id as a string.
    // -----------------------------------------------------------------------
    std::printf("\n[group] script indexing\n");
    Con::evaluate("$gc1 = G1.getCount(); $gc2 = G2.getCount();",
                  false, "simgroup.test");
    const char* gc1 = Con::getVariable("gc1");
    const char* gc2 = Con::getVariable("gc2");
    check(gc1 && std::strcmp(gc1, "1") == 0, "G1.getCount() == 1");
    check(gc2 && std::strcmp(gc2, "1") == 0, "G2.getCount() == 1");

    Con::evaluate("$o1 = G1.getObject(0); $o2 = G2.getObject(0);",
                  false, "simgroup.test");
    const char* o1 = Con::getVariable("o1");
    const char* o2 = Con::getVariable("o2");

    // The script return is a numeric id; compare to the C++ ids.
    char idG2[32], idP[32];
    std::snprintf(idG2, sizeof(idG2), "%u", g2->getId());
    std::snprintf(idP,  sizeof(idP),  "%u", p->getId());
    check(o1 && std::strcmp(o1, idG2) == 0, "G1.getObject(0).id == G2.id");
    check(o2 && std::strcmp(o2, idP)  == 0, "G2.getObject(0).id == P.id");

    // -----------------------------------------------------------------------
    // 3. Reverse navigation: P.getGroup() returns G2 (single-group rule).
    // -----------------------------------------------------------------------
    std::printf("\n[group] reverse navigation\n");
    check(p->getGroup() == g2, "P.getGroup() (C++ side) == G2");

    Con::evaluate("$pg = P.getGroup();", false, "simgroup.test");
    const char* pg = Con::getVariable("pg");
    check(pg && std::strcmp(pg, idG2) == 0, "P.getGroup().id == G2.id (script)");

    Con::evaluate("$g2g = G2.getGroup();", false, "simgroup.test");
    const char* g2g = Con::getVariable("g2g");
    char idG1[32];
    std::snprintf(idG1, sizeof(idG1), "%u", g1->getId());
    check(g2g && std::strcmp(g2g, idG1) == 0, "G2.getGroup().id == G1.id (script)");

    // -----------------------------------------------------------------------
    // 4. Script-driven add/remove/isMember on an empty SimSet.
    //    Use a separate container (SimSet, not SimGroup) so add/remove
    //    semantics don't conflict with group-membership rules.
    // -----------------------------------------------------------------------
    std::printf("\n[group] add / remove / isMember\n");
    Con::evaluate(
        "instant SimSet(MySet) { };\n"
        "instant GroupMember(M1) { };\n"
        "instant GroupMember(M2) { };\n"
        "MySet.add(M1);\n"
        "MySet.add(M2);\n"
        "$cnt = MySet.getCount();\n"
        "$mem1 = MySet.isMember(M1);\n",
        false, "simgroup.test");
    const char* cnt = Con::getVariable("cnt");
    const char* mem1 = Con::getVariable("mem1");
    check(cnt && std::strcmp(cnt, "2") == 0, "MySet.getCount() == 2 after two adds");
    check(mem1 && std::strcmp(mem1, "1") == 0, "MySet.isMember(M1) == true");

    Con::evaluate("MySet.remove(M1); $cnt2 = MySet.getCount();"
                  " $mem1b = MySet.isMember(M1);",
                  false, "simgroup.test");
    const char* cnt2 = Con::getVariable("cnt2");
    const char* mem1b = Con::getVariable("mem1b");
    check(cnt2 && std::strcmp(cnt2, "1") == 0, "MySet.getCount() == 1 after remove");
    check(mem1b && std::strcmp(mem1b, "0") == 0, "MySet.isMember(M1) == false after remove");

    Con::evaluate("MySet.clear(); $cnt3 = MySet.getCount();",
                  false, "simgroup.test");
    const char* cnt3 = Con::getVariable("cnt3");
    check(cnt3 && std::strcmp(cnt3, "0") == 0, "MySet.clear() empties the set");

    // -----------------------------------------------------------------------
    // 5. Cascade delete — deleting G1 destroys G2 and P with it.
    //    Track GroupMember::sLiveCount to verify the destructor ran.
    // -----------------------------------------------------------------------
    std::printf("\n[group] cascade lifetime\n");
    const int liveBefore = GroupMember::sLiveCount;
    check(liveBefore >= 3, "(precondition) live count >= 3 before delete");

    Con::evaluate("G1.delete();", false, "simgroup.test");

    check(findByName("G1") == nullptr, "G1 destroyed (findObject returns null)");
    check(findByName("G2") == nullptr, "G2 destroyed cascading from G1");
    check(findByName("P")  == nullptr, "P destroyed cascading from G2");
    check(GroupMember::sLiveCount == liveBefore - 1,
          "exactly one GroupMember (P) was destructed by G1.delete()");

    // Cleanup the still-existing M1/M2/MySet so the leak detector is happy.
    Con::evaluate("MySet.delete(); M1.delete(); M2.delete();",
                  false, "simgroup.test");

    // -----------------------------------------------------------------------
    std::printf("\n========================================================\n");
    std::printf("cscript_simgroup_test: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
