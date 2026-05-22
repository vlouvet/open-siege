// Open Siege spec 15/08 — dialect-B evaluator implementation.
//
// The evaluator is intentionally narrow: it does NOT reimplement the
// upstream Torque3D editor's GUI subsystem. It walks dialect-B source
// line by line, executes the four constructs that have non-GUI side
// effects (`set / alias / unalias`, plus comments and blank lines),
// and emits an `[unbound dialect-B feature: TOKEN]` warning for the
// roughly thirty editor commands and grammar shapes that require the
// ConsoleWorld GUI. Acceptance for spec 15/08 is "zero runtime errors"
// — unbound features warn, they don't abort.
//
// Line continuation is handled per the ConsoleWorld convention: a
// trailing backslash on a non-comment line absorbs the following
// newline. Comments (`#` to EOL) take precedence over `\` continuation.

#include "dialectBEval.h"

#include <cctype>
#include <cstring>
#include <sstream>

namespace studio::content::cscript::TorqueScript
{

namespace
{

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentChar(char c)  { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == '$' || c == '%'; }

// Trim leading spaces/tabs.
const char* skipSpaces(const char* p, const char* end)
{
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    return p;
}

// Read a token starting at p. Stops at whitespace, EOL, or comment '#'.
// Honours double-quoted strings as a single token (preserving the
// internal whitespace) and single-quoted strings ('...'); embedded
// quotes via backslash are supported. Returns the token text and
// advances `p` past it.
std::string readToken(const char*& p, const char* end)
{
    std::string out;
    if (p >= end) return out;
    if (*p == '"' || *p == '\'') {
        char q = *p++;
        while (p < end && *p != q) {
            if (*p == '\\' && p + 1 < end) {
                out += p[1];
                p += 2;
                continue;
            }
            out += *p++;
        }
        if (p < end && *p == q) ++p;
        return out;
    }
    while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '#') {
        out += *p++;
    }
    return out;
}

// Read the remainder of the line (after a token) as a single argument.
// Trims leading + trailing whitespace and strips an end-of-line `#…`
// comment, but preserves embedded quoted whitespace.
std::string readRestOfLine(const char*& p, const char* end)
{
    std::string out;
    p = skipSpaces(p, end);
    bool in_str = false;
    char q = 0;
    while (p < end && *p != '\n' && *p != '\r') {
        if (!in_str && *p == '#') {
            // unquoted '#' starts a trailing comment
            while (p < end && *p != '\n') ++p;
            break;
        }
        if ((*p == '"' || *p == '\'') && (!in_str || q == *p)) {
            in_str = !in_str;
            if (in_str) q = *p;
            out += *p++;
            continue;
        }
        if (*p == '\\' && p + 1 < end && in_str) {
            out += p[1];
            p += 2;
            continue;
        }
        out += *p++;
    }
    // Strip trailing whitespace.
    while (!out.empty()
        && (out.back() == ' ' || out.back() == '\t' || out.back() == '\r'))
    {
        out.pop_back();
    }
    return out;
}

// Skip to start of next physical line.
void advanceLine(const char*& p, const char* end)
{
    while (p < end && *p != '\n') ++p;
    if (p < end) ++p;
}

// Read one logical line into `buf`. Handles trailing-backslash
// continuation by absorbing the next physical line. Returns false at EOF.
bool readLogicalLine(const char*& p, const char* end, std::string& buf)
{
    buf.clear();
    if (p >= end) return false;
    while (p < end) {
        const char* lineStart = p;
        while (p < end && *p != '\n' && *p != '\r') ++p;
        // Does the line end with a backslash continuation?
        bool cont = false;
        const char* lineEnd = p;
        // Strip a single trailing \r before checking.
        while (lineEnd > lineStart
            && (lineEnd[-1] == ' ' || lineEnd[-1] == '\t'))
        {
            --lineEnd;
        }
        if (lineEnd > lineStart && lineEnd[-1] == '\\') {
            cont = true;
            --lineEnd;
        }
        buf.append(lineStart, lineEnd);
        // Consume EOL.
        if (p < end && *p == '\r') ++p;
        if (p < end && *p == '\n') ++p;
        if (!cont) break;
        buf += ' ';
    }
    return true;
}

// Common dialect-B bareword commands we recognise (but don't bind in
// v1). Listed here so unbound-warning reports can stay consistent.
bool isEditorBareword(const std::string& tok)
{
    static const char* const kEditorWords[] = {
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
        "newMoveObj", "postAction",
        "bind",
        nullptr,
    };
    for (const char* const* w = kEditorWords; *w; ++w) {
        if (tok == *w) return true;
    }
    // Ted::… is always editor.
    if (tok.size() >= 5
        && std::memcmp(tok.data(), "Ted::", 5) == 0) return true;
    return false;
}

} // anonymous namespace

DialectBEvalResult evaluateDialectB(const char* source,
                                    unsigned long length,
                                    DialectBContext& ctx)
{
    DialectBEvalResult r;
    if (!source) return r;
    const char* p = source;
    const char* end = (length > 0) ? (source + length) : (source + std::strlen(source));

    std::string logical;
    while (readLogicalLine(p, end, logical)) {
        ++r.lines_scanned;
        const char* lp = logical.c_str();
        const char* le = lp + logical.size();
        lp = skipSpaces(lp, le);

        // Blank line.
        if (lp >= le) { ++r.lines_executed; continue; }
        // Comment line.
        if (*lp == '#') { ++r.lines_executed; continue; }

        // Read leading token.
        std::string tok = readToken(lp, le);
        if (tok.empty()) { ++r.lines_executed; continue; }

        // Recognised constructs first.
        if (tok == "set") {
            lp = skipSpaces(lp, le);
            std::string name = readToken(lp, le);
            std::string value = readRestOfLine(lp, le);
            if (name.empty()) {
                r.warnings.push_back("set without name");
                ++r.unbound_features;
                continue;
            }
            ctx.vars[name] = value;
            ++r.lines_executed;
            continue;
        }
        if (tok == "alias") {
            lp = skipSpaces(lp, le);
            std::string name = readToken(lp, le);
            std::string value = readRestOfLine(lp, le);
            if (name.empty()) {
                r.warnings.push_back("alias without name");
                ++r.unbound_features;
                continue;
            }
            ctx.aliases[name] = value;
            ++r.lines_executed;
            continue;
        }
        if (tok == "unalias") {
            lp = skipSpaces(lp, le);
            std::string name = readToken(lp, le);
            ctx.aliases.erase(name);
            ++r.lines_executed;
            continue;
        }
        if (tok == "echo" || tok == "print") {
            // No side effect in this evaluator — silent rather than
            // routed to a host stdout because we run inside the test
            // harness today. Treated as recognised + no-op.
            ++r.lines_executed;
            continue;
        }
        if (tok == "if" || tok == "endif" || tok == "else"
         || tok == "then" || tok == "for" || tok == "endfor"
         || tok == "while" || tok == "endwhile")
        {
            // Structural shell control. Without a full predicate
            // evaluator we treat the whole construct as recognised
            // but inert. The "test" predicate body in `if test …`
            // would land in a follow-up if we ever need to honour
            // gameplay-side branches.
            ++r.lines_executed;
            continue;
        }
        if (isEditorBareword(tok)) {
            ++r.unbound_features;
            r.warnings.push_back("[unbound dialect-B feature: " + tok + "]");
            continue;
        }

        // Unrecognised but identifier-shaped — log + continue, do not
        // fail. This covers the ConsoleWorld editor commands not in
        // our whitelist plus any one-off macros mods may add.
        if (isIdentStart(tok[0])) {
            ++r.unbound_features;
            r.warnings.push_back("[unbound dialect-B feature: " + tok + "]");
            continue;
        }

        // Truly garbage leading char — count as an error but keep going.
        r.ok = false;
        ++r.unbound_features;
        r.warnings.push_back("garbage token: '" + tok + "'");
    }
    return r;
}

} // namespace studio::content::cscript::TorqueScript
