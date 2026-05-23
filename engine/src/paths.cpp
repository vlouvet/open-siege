#include <osengine/paths.hpp>

#include <cstdlib>
#include <fstream>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#  include <shlobj.h>
#endif

namespace os_paths
{

namespace fs = std::filesystem;

namespace
{

fs::path platform_config_root()
{
#if defined(__APPLE__)
    if (const char* h = std::getenv("HOME"); h && *h)
        return fs::path(h) / "Library" / "Application Support" / "open-siege";
    return {};
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf) == S_OK)
        return fs::path(buf) / "open-siege";
    return {};
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / "open-siege";
    if (const char* h = std::getenv("HOME"); h && *h)
        return fs::path(h) / ".config" / "open-siege";
    return {};
#endif
}

fs::path platform_cache_root()
{
#if defined(__APPLE__)
    if (const char* h = std::getenv("HOME"); h && *h)
        return fs::path(h) / "Library" / "Caches" / "open-siege";
    return {};
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf) == S_OK)
        return fs::path(buf) / "open-siege";
    return {};
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return fs::path(xdg) / "open-siege";
    if (const char* h = std::getenv("HOME"); h && *h)
        return fs::path(h) / ".cache" / "open-siege";
    return {};
#endif
}

} // anonymous namespace

fs::path config_dir(std::string_view binary)
{
    return platform_config_root() / std::string(binary);
}

fs::path cache_dir(std::string_view binary)
{
    return platform_cache_root() / std::string(binary);
}

fs::path assets_dir()
{
    const auto f = config_dir("shared") / "tribes-dir.txt";
    std::ifstream in(f);
    if (!in) return {};
    std::string line;
    std::getline(in, line);
    // Trim trailing whitespace + CR (in case the file came from Windows).
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                             line.back() == ' '  || line.back() == '\t'))
        line.pop_back();
    return line.empty() ? fs::path{} : fs::path(line);
}

bool set_assets_dir(const fs::path& path)
{
    const auto dir = config_dir("shared");
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
    std::ofstream out(dir / "tribes-dir.txt", std::ios::trunc);
    if (!out) return false;
    out << path.string() << '\n';
    return static_cast<bool>(out);
}

} // namespace os_paths
