#ifndef OSENGINE_MAP_CYCLE_HPP
#define OSENGINE_MAP_CYCLE_HPP

// Spec 28/11 — server map cycle.
//
// MapCycle holds an ordered list of mission short-names (e.g.
// {"1_Welcome", "5_CTF_Bedlam"}) and a current index. advance()
// wraps modulo the list. set() forces the next current to the named
// mission (returns false if name not in the list).
//
// The actual mission load/unload is performed by main.cpp using the
// existing mission_loader; this class only owns the rotation state.

#include <cstdint>
#include <string>
#include <vector>

namespace dts_viewer
{

class MapCycle
{
public:
    explicit MapCycle(std::vector<std::string> maps);

    const std::string& current()   const;
    const std::string& peek_next() const;
    void               advance();
    bool               set(const std::string& mission_name);

    std::size_t size()  const noexcept { return maps_.size(); }
    std::size_t index() const noexcept { return cursor_; }

    static int selftest();

private:
    std::vector<std::string> maps_;
    std::size_t              cursor_ = 0;
    static const std::string s_empty;
};

} // namespace dts_viewer

#endif // OSENGINE_MAP_CYCLE_HPP
