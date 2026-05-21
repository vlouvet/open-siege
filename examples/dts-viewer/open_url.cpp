// Spec 25/08 — system-default browser launcher.

#include "open_url.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#  include <shellapi.h>
#endif

namespace dts_viewer {

namespace {

// Defensive shell quoting for POSIX `open` / `xdg-open`. The URL is
// constrained to ASCII URL characters in normal use, but we still
// refuse to launch if a single-quote sneaks in — safer than
// expanding the escape rules.
bool safe_for_single_quote(const std::string& s)
{
    for (char c : s) if (c == '\'' || c == '\n' || c == '\r') return false;
    return true;
}

} // namespace

bool open_url(const std::string& url)
{
    if (url.empty()) return false;
    std::fprintf(stdout, "[help] open_url: %s\n", url.c_str());

#if defined(__APPLE__)
    if (!safe_for_single_quote(url)) return false;
    std::string cmd = "open '";
    cmd += url;
    cmd += "' >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
#elif defined(_WIN32)
    HINSTANCE r = ShellExecuteA(nullptr, "open", url.c_str(),
                                nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
#else
    if (!safe_for_single_quote(url)) return false;
    std::string cmd = "xdg-open '";
    cmd += url;
    cmd += "' >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) == 0) return true;
    if (const char* br = std::getenv("BROWSER"); br && *br) {
        std::string cmd2 = br;
        cmd2 += " '";
        cmd2 += url;
        cmd2 += "' >/dev/null 2>&1 &";
        return std::system(cmd2.c_str()) == 0;
    }
    return false;
#endif
}

} // namespace dts_viewer
