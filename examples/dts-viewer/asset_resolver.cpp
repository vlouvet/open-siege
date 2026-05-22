#include "asset_resolver.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#  include <shlobj.h>
#endif

namespace dts_viewer {

namespace fs = std::filesystem;

// Sentinel files that identify a Tribes 1.41 install.
static const char* kSentinels[] = {
    "base/Entities.vol",
    "base/scripts.vol",
    nullptr
};

bool validateTribesDir(const fs::path& dir)
{
    for (int i = 0; kSentinels[i]; ++i) {
        if (!fs::exists(dir / kSentinels[i]))
            return false;
    }
    return true;
}

fs::path configDir()
{
#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / "Library" / "Application Support" / "open-siege";
    return {};
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf) == S_OK)
        return fs::path(buf) / "open-siege";
    return {};
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / "open-siege";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".config" / "open-siege";
    return {};
#endif
}

fs::path configPath()
{
    return configDir() / "config.toml";
}

// --- tiny key=value reader/writer -----------------------------------------
// Format: one "key = value" per line, values may be quoted or bare.

static std::string readKey(const fs::path& file, const std::string& key)
{
    std::ifstream in(file);
    if (!in) return {};
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto k = line.substr(0, eq);
        // trim whitespace from key
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (k != key) continue;
        auto v = line.substr(eq + 1);
        // trim leading whitespace + optional quotes
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
        if (!v.empty() && v.front() == '"') {
            v.erase(0, 1);
            auto close = v.find('"');
            if (close != std::string::npos) v.resize(close);
        }
        return v;
    }
    return {};
}

static void writeKey(const fs::path& file, const std::string& key, const std::string& value)
{
    // Read all lines, replace or append the key.
    std::vector<std::string> lines;
    {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    bool found = false;
    std::string entry = key + " = \"" + value + "\"";
    for (auto& line : lines) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto k = line.substr(0, eq);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (k == key) { line = entry; found = true; break; }
    }
    if (!found) lines.push_back(entry);
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::trunc);
    for (const auto& l : lines) out << l << "\n";
}

// --- CLI override ----------------------------------------------------------

static std::string g_override; // set by stripTribesDir()

void stripTribesDir(int& argc, char** argv)
{
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--tribes-dir") == 0) {
            g_override = argv[i + 1];
            // shift remaining args left by 2
            for (int j = i; j < argc - 2; ++j) argv[j] = argv[j + 2];
            argc -= 2;
            return;
        }
    }
}

// --- resolve ---------------------------------------------------------------

fs::path resolveTribesDir()
{
    // 1. CLI override
    if (!g_override.empty()) {
        fs::path p = g_override;
        if (validateTribesDir(p)) {
            writeKey(configPath(), "tribes_dir", p.string());
            return p;
        }
        std::fprintf(stderr, "[asset-resolver] --tribes-dir '%s' is not a valid Tribes 1.41 directory\n",
                     p.string().c_str());
    }

    // 2. Cached config
    fs::path cfg = configPath();
    if (fs::exists(cfg)) {
        std::string cached = readKey(cfg, "tribes_dir");
        if (!cached.empty()) {
            fs::path p = cached;
            if (validateTribesDir(p))
                return p;
            std::fprintf(stderr, "[asset-resolver] cached path '%s' is missing sentinel files — re-prompting\n",
                         p.string().c_str());
        }
    }

    // 3. Interactive prompt
    std::fprintf(stdout,
        "\n"
        "Open Siege could not find your Tribes 1.41 installation.\n"
        "Please enter the path to your Tribes 1.41 install directory\n"
        "(the folder that contains 'base/Entities.vol'):\n"
        "\n");

    for (int attempt = 0; attempt < 5; ++attempt) {
        std::fprintf(stdout, "Tribes install path: ");
        std::fflush(stdout);

        std::string input;
        if (!std::getline(std::cin, input) || input.empty()) {
            std::fprintf(stdout, "\n[asset-resolver] aborted.\n");
            return {};
        }

        // Strip trailing slash
        while (!input.empty() && (input.back() == '/' || input.back() == '\\'))
            input.pop_back();

        fs::path p = input;
        if (validateTribesDir(p)) {
            writeKey(cfg, "tribes_dir", p.string());
            std::fprintf(stdout, "[asset-resolver] path saved to %s\n", cfg.string().c_str());
            return p;
        }
        std::fprintf(stderr,
            "  '%s' doesn't look like a Tribes 1.41 directory\n"
            "  (expected to find base/Entities.vol and base/scripts.vol inside it).\n"
            "  Please try again.\n\n",
            p.string().c_str());
    }

    std::fprintf(stderr, "[asset-resolver] too many failed attempts — aborting.\n");
    return {};
}

} // namespace dts_viewer
