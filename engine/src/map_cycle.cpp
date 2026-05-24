#include <osengine/map_cycle.hpp>

#include <algorithm>
#include <cstdio>

namespace dts_viewer
{

const std::string MapCycle::s_empty;

MapCycle::MapCycle(std::vector<std::string> maps)
    : maps_(std::move(maps))
{
    // An empty cycle is a programmer error but we tolerate it — current()
    // returns "" and advance() is a no-op.
}

const std::string& MapCycle::current() const
{
    if (maps_.empty()) return s_empty;
    return maps_[cursor_ % maps_.size()];
}

const std::string& MapCycle::peek_next() const
{
    if (maps_.empty()) return s_empty;
    return maps_[(cursor_ + 1) % maps_.size()];
}

void MapCycle::advance()
{
    if (maps_.empty()) return;
    cursor_ = (cursor_ + 1) % maps_.size();
}

bool MapCycle::set(const std::string& mission_name)
{
    auto it = std::find(maps_.begin(), maps_.end(), mission_name);
    if (it == maps_.end()) return false;
    cursor_ = static_cast<std::size_t>(std::distance(maps_.begin(), it));
    return true;
}

int MapCycle::selftest()
{
    MapCycle mc({"A", "B", "C"});
    if (mc.current() != "A") {
        std::fprintf(stderr, "[mapcycle-selftest] expected A, got %s\n",
                     mc.current().c_str());
        return 1;
    }
    if (mc.peek_next() != "B") {
        std::fputs("[mapcycle-selftest] peek_next should be B\n", stderr); return 1;
    }
    mc.advance();
    if (mc.current() != "B") { std::fputs("[mapcycle-selftest] after advance != B\n", stderr); return 1; }
    mc.advance();
    if (mc.current() != "C") { std::fputs("[mapcycle-selftest] after 2x advance != C\n", stderr); return 1; }
    mc.advance();
    if (mc.current() != "A") { std::fputs("[mapcycle-selftest] wrap to A failed\n", stderr); return 1; }

    if (!mc.set("C")) { std::fputs("[mapcycle-selftest] set(C) returned false\n", stderr); return 1; }
    if (mc.current() != "C") { std::fputs("[mapcycle-selftest] set(C) didn't update current\n", stderr); return 1; }
    if (mc.set("ZZ")) { std::fputs("[mapcycle-selftest] set(ZZ) should be false\n", stderr); return 1; }
    if (mc.current() != "C") { std::fputs("[mapcycle-selftest] failed set should not change current\n", stderr); return 1; }

    // Empty cycle is a no-op.
    MapCycle empty({});
    if (!empty.current().empty()) { std::fputs("[mapcycle-selftest] empty cycle should return \"\"\n", stderr); return 1; }
    empty.advance();
    if (!empty.current().empty()) { std::fputs("[mapcycle-selftest] empty cycle advance broke\n", stderr); return 1; }

    std::fputs("[mapcycle-selftest] OK — wrap, set, empty\n", stderr);
    return 0;
}

} // namespace dts_viewer
