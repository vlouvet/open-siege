// Open Siege spec 17/01 — script path resolver impl.

#include "script_resolver.h"

#include "console/console.h"
#include "console/script.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace studio { namespace content { namespace cscript { namespace ScriptResolver {

namespace {

std::vector<std::string>& searchPaths()
{
    static std::vector<std::string> sg;
    return sg;
}

std::vector<std::string>& dirStack()
{
    static std::vector<std::string> sg;
    return sg;
}

bool fileExists(const std::string& path)
{
    return std::filesystem::is_regular_file(path);
}

std::string parentDir(const std::string& path)
{
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    return path.substr(0, slash);
}

std::string joinPath(const std::string& dir, const std::string& rel)
{
    if (dir.empty() || dir == ".") return rel;
    if (rel.empty()) return dir;
    if (rel.front() == '/') return rel;
    if (dir.back() == '/') return dir + rel;
    return dir + "/" + rel;
}

// Strip a leading "./" if present.
std::string stripDotSlash(const std::string& s)
{
    if (s.size() >= 2 && s[0] == '.' && s[1] == '/') return s.substr(2);
    return s;
}

} // namespace

void addSearchPath(const std::string& dir)
{
    auto& sp = searchPaths();
    for (const auto& existing : sp)
        if (existing == dir) return;
    sp.push_back(dir);
}

void clearSearchPaths()
{
    searchPaths().clear();
}

std::string resolve(const std::string& scriptPath)
{
    if (scriptPath.empty()) return {};

    auto tryWithExt = [](const std::string& p) -> std::string {
        if (fileExists(p)) return p;
        if (p.size() < 3 || p.substr(p.size() - 3) != ".cs")
        {
            std::string withExt = p + ".cs";
            if (fileExists(withExt)) return withExt;
        }
        return {};
    };

    // 1. Absolute path.
    if (!scriptPath.empty() && scriptPath.front() == '/')
    {
        std::string r = tryWithExt(scriptPath);
        if (!r.empty()) return r;
        return {};
    }

    // 2. "./..." or anything when we have a current script dir.
    if (scriptPath.size() >= 2 && scriptPath[0] == '.' && scriptPath[1] == '/'
        && !dirStack().empty())
    {
        std::string rel = stripDotSlash(scriptPath);
        std::string candidate = joinPath(dirStack().back(), rel);
        std::string r = tryWithExt(candidate);
        if (!r.empty()) return r;
        // Fall through to search paths as a fallback.
    }

    // Also try relative-to-current-script-dir even without the explicit
    // "./" prefix — Tribes scripts mix both styles.
    if (!dirStack().empty())
    {
        std::string candidate = joinPath(dirStack().back(), scriptPath);
        std::string r = tryWithExt(candidate);
        if (!r.empty()) return r;
    }

    // 3. Search paths.
    for (const auto& sp : searchPaths())
    {
        std::string candidate = joinPath(sp, scriptPath);
        std::string r = tryWithExt(candidate);
        if (!r.empty()) return r;
    }

    return {};
}

bool runScriptFile(const char* scriptPath)
{
    if (!scriptPath || !*scriptPath) return false;

    std::string resolved = resolve(scriptPath);
    if (resolved.empty())
    {
        Con::warnf("exec: could not resolve script \"%s\"", scriptPath);
        return false;
    }

    std::ifstream f(resolved, std::ios::binary);
    if (!f)
    {
        Con::warnf("exec: failed to open \"%s\"", resolved.c_str());
        return false;
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    std::string body = buf.str();

    // Push the resolved file's parent dir so nested exec("./...") calls
    // resolve relative to THIS script.
    dirStack().push_back(parentDir(resolved));
    Con::evaluate(body.c_str(), false, resolved.c_str());
    dirStack().pop_back();
    return true;
}

std::vector<std::string> currentSearchPaths() { return searchPaths(); }
int currentStackDepth() { return (int)dirStack().size(); }

}}}} // namespace studio::content::cscript::ScriptResolver
