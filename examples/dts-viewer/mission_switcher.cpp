#include "mission_switcher.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace fs = std::filesystem;

namespace dts_viewer
{

namespace
{

// Natural sort: split leading integer prefix, compare numerically first,
// then lexicographically.  "1_Welcome" < "2_Weapons" < "10_Phobos".
bool natural_less(const std::string& a, const std::string& b)
{
    auto take_num = [](const std::string& s, std::size_t& off) -> long long {
        long long n = 0;
        bool any = false;
        while (off < s.size() && std::isdigit(static_cast<unsigned char>(s[off]))) {
            n = n * 10 + (s[off] - '0');
            ++off;
            any = true;
        }
        return any ? n : -1;
    };
    std::size_t i = 0, j = 0;
    long long na = take_num(a, i);
    long long nb = take_num(b, j);
    if (na >= 0 && nb >= 0 && na != nb) return na < nb;
    return a < b;
}

bool ext_eq_ci(const fs::path& p, const std::string& ext_lower)
{
    std::string e = p.extension().string();
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == ext_lower;
}

} // anonymous namespace

MissionRegistry scan_missions(const fs::path& missions_dir)
{
    MissionRegistry reg;
    if (!fs::is_directory(missions_dir)) return reg;

    struct Entry { std::string short_name; fs::path ted; };
    std::vector<Entry> entries;
    for (const auto& ent : fs::directory_iterator(missions_dir)) {
        if (!ent.is_regular_file()) continue;
        if (!ext_eq_ci(ent.path(), ".ted")) continue;
        entries.push_back({ ent.path().stem().string(), ent.path() });
    }

    std::sort(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b) {
            return natural_less(a.short_name, b.short_name);
        });

    reg.short_names.reserve(entries.size());
    reg.ted_paths.reserve(entries.size());
    for (auto& e : entries) {
        reg.short_names.push_back(std::move(e.short_name));
        reg.ted_paths.push_back(std::move(e.ted));
    }
    return reg;
}

std::string cycle_mission(
    MissionRegistry& reg,
    int delta,
    std::function<bool(const std::string&, const fs::path&)> load_callback)
{
    if (reg.short_names.empty()) return {};
    const std::size_t n = reg.short_names.size();
    const std::size_t prev = reg.current_index;
    std::size_t next = prev;
    // Wrap-around modulo arithmetic.
    long long signed_idx = static_cast<long long>(prev) + delta;
    while (signed_idx < 0)            signed_idx += n;
    while (signed_idx >= static_cast<long long>(n)) signed_idx -= n;
    next = static_cast<std::size_t>(signed_idx);

    reg.current_index = next;
    if (load_callback &&
        !load_callback(reg.short_names[next], reg.ted_paths[next])) {
        reg.current_index = prev;
        return {};
    }
    return reg.short_names[next];
}

void print_mission_list(const MissionRegistry& reg)
{
    std::fprintf(stderr, "missions (%zu):\n", reg.short_names.size());
    for (std::size_t i = 0; i < reg.short_names.size(); ++i) {
        std::fprintf(stderr, "  %s %s\n",
            (i == reg.current_index ? "*" : " "),
            reg.short_names[i].c_str());
    }
}

} // namespace dts_viewer
