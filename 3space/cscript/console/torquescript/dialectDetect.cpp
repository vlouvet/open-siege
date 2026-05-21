// Open Siege spec 15/05 — Tribes-1 CScript dialect detector.
//
// Implementation notes:
// - Pure string analysis; no dependency on the VM, console, or platform
//   layers. Compiles cleanly inside cscript_core even before the math/
//   import unblocks the full VM.
// - The classifier is intentionally conservative: when no strong dialect-B
//   marker is found, dialect A is returned. This matches the engine's
//   load path, which dispatches editor scripts (Ted::*) via an explicit
//   dialect-B context and treats everything else as dialect A by default.

#include "dialectDetect.h"

#include <cstring>

namespace studio::content::cscript::TorqueScript
{

namespace
{

bool startsWith(const char* line, const char* prefix)
{
    while (*prefix)
    {
        if (*line != *prefix)
            return false;
        ++line;
        ++prefix;
    }
    return true;
}

bool isIdentChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

const char* firstSignificantLine(const char* p, const char* end)
{
    while (p < end)
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
        if (p >= end)
            return nullptr;

        if (p + 1 < end && p[0] == '/' && p[1] == '/')
        {
            while (p < end && *p != '\n')
                ++p;
            continue;
        }
        if (*p == '#')
        {
            while (p < end && *p != '\n')
                ++p;
            continue;
        }
        if (p + 1 < end && p[0] == '/' && p[1] == '*')
        {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
                ++p;
            if (p + 1 < end)
                p += 2;
            continue;
        }
        return p;
    }
    return nullptr;
}

// Returns true iff `line` (a leading-significant line) begins with a token
// that is unambiguously a dialect-B opener — `if test`, `endif`, `set X`,
// `alias X`, or one of the editor-script bareword commands shipped in
// ted.vol. These shapes never appear in dialect-A `.cs` / `.mis` files.
bool leadingTokenIsDialectB(const char* line, const char* end)
{
    if (startsWith(line, "if test")
        && line + 7 < end
        && (line[7] == ' ' || line[7] == '\t'))
        return true;

    if (startsWith(line, "endif")
        && (line + 5 >= end || !isIdentChar(line[5])))
        return true;

    if (startsWith(line, "set ")
        && line + 4 < end && isIdentChar(line[4]))
        return true;

    if (startsWith(line, "alias ")
        && line + 6 < end && isIdentChar(line[6]))
        return true;

    // ConsoleWorld editor-script bareword commands observed in ted.vol.
    // None of these names collide with dialect-A keywords or with the
    // canonical Tribes-1 script API.
    // Note: `newActionMap`, `newObject` are deliberately EXCLUDED — those
    // are dialect-A function calls (`newActionMap("foo");`) in scripts.vol
    // and would false-positive here.
    static const char* const kDialectBLeadCommands[] = {
        "newClient", "newCanvas", "newCamera", "newToolWindow",
        "newWindow", "newDialog", "newMenu", "newButton",
        "focusClient", "focusServer", "focusCanvas",
        "loadMission", "loadGame", "loadDef", "loadFile", "loadPalette",
        "edit2Box", "editBox", "editText",
        "addToolButton", "addToolGap", "addStatusBar",
        "setcat", "setToolCommand", "setButtonHelp",
        "addMenuItem", "addMenu",
        "deleteCanvas", "deleteDialog", "deleteMenu",
        "showWindow", "hideWindow",
        "openFile", "saveFile", "closeFile",
        "confirmBox", "messageBox",
        "LSEditor", "ListBox", "ListBoxFile",
        "flushTextureCache",
        "listFiles", "listBlocks", "listMaterials", "listFlags",
        "Ted",   // any line starting with Ted:: (editor namespace) — must
                 // be followed by '::' to win
        "not",   // dialect-B unary `not Ted::selVisible`
        nullptr,
    };
    for (const char* const* w = kDialectBLeadCommands; *w; ++w)
    {
        const unsigned long n = std::strlen(*w);
        if (line + n > end) continue;
        if (std::memcmp(line, *w, n) != 0) continue;

        // Token boundary: must NOT be followed by another identifier char.
        // Exception: `Ted` requires `::` to follow (Ted::flymode style).
        if ((*w)[0] == 'T' && n == 3)
        {
            if (line + 5 < end && line[3] == ':' && line[4] == ':' && isIdentChar(line[5]))
                return true;
            continue;
        }
        if (line + n >= end || !isIdentChar(line[n]))
            return true;
    }

    return false;
}

// Returns true iff source contains a structural `{` outside string literals
// and comments. Dialect-B never uses `{` for blocks — strings sometimes
// contain it, so this scan strips strings and comments first.
bool containsStructuralBrace(const char* p, const char* end)
{
    bool inLineComment = false;
    bool inBlockComment = false;
    bool inString = false;
    char stringQuote = 0;

    while (p < end)
    {
        char c = *p;

        if (inLineComment)
        {
            if (c == '\n') inLineComment = false;
            ++p; continue;
        }
        if (inBlockComment)
        {
            if (c == '*' && p + 1 < end && p[1] == '/') { inBlockComment = false; p += 2; continue; }
            ++p; continue;
        }
        if (inString)
        {
            if (c == '\\' && p + 1 < end) { p += 2; continue; }
            if (c == stringQuote) inString = false;
            ++p; continue;
        }

        if (c == '/' && p + 1 < end && p[1] == '/') { inLineComment = true; p += 2; continue; }
        if (c == '/' && p + 1 < end && p[1] == '*') { inBlockComment = true; p += 2; continue; }
        if (c == '#') { inLineComment = true; ++p; continue; }
        if (c == '"' || c == '\'') { inString = true; stringQuote = c; ++p; continue; }

        if (c == '{') return true;

        ++p;
    }
    return false;
}

} // namespace

CScriptDialect detectDialect(const char* source, unsigned long length)
{
    if (!source)
        return CScriptDialect::A;

    const char* end = (length > 0) ? (source + length) : (source + std::strlen(source));
    const char* line = firstSignificantLine(source, end);
    if (!line)
        return CScriptDialect::A;

    // 1. Strong dialect-A first-line markers.
    static const char* const kDialectALeadKeywords[] = {
        "function ", "datablock ",
        "singleton ", "package ", "switch ",
        nullptr,
    };
    for (const char* const* w = kDialectALeadKeywords; *w; ++w)
    {
        const unsigned long n = std::strlen(*w);
        if (line + n <= end && std::memcmp(line, *w, n) == 0)
            return CScriptDialect::A;
    }
    if (*line == '$' || *line == '%')
        return CScriptDialect::A;

    // `instant ` and `new ` are ambiguous against editor-script `newClient`,
    // `newCanvas`, etc. Require a capital letter following — dialect-A object
    // declarations always name a Type next.
    auto leadIsInstantOrNew = [&](const char* kw, unsigned long n) {
        if (line + n + 1 > end) return false;
        if (std::memcmp(line, kw, n) != 0) return false;
        if (line[n] != ' ' && line[n] != '\t') return false;
        const char* nc = line + n;
        while (nc < end && (*nc == ' ' || *nc == '\t')) ++nc;
        return nc < end && *nc >= 'A' && *nc <= 'Z';
    };
    if (leadIsInstantOrNew("instant", 7)) return CScriptDialect::A;
    if (leadIsInstantOrNew("new", 3))     return CScriptDialect::A;

    // 2. Strong dialect-B first-line markers (incl. editor commands).
    if (leadingTokenIsDialectB(line, end))
        return CScriptDialect::B;

    // 3. Structural-brace scan: dialect-B never uses `{` for blocks.
    //    Any structural `{` (outside strings/comments) proves dialect-A.
    if (containsStructuralBrace(source, end))
        return CScriptDialect::A;

    // 4. Default — dialect-A is the engine's default load context.
    return CScriptDialect::A;
}

} // namespace studio::content::cscript::TorqueScript
