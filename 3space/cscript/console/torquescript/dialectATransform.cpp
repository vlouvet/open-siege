// Open Siege spec 16/03 — Tribes-1 dialect-A datablock transform impl.
//
// See dialectATransform.h for the rationale. The implementation is a
// hand-rolled character scanner (no std::regex — std::regex would have
// to handle multiline quoted strings + comments correctly, and the
// performance is poor on 100KB+ scripts).
//
// Scanner state machine:
//
//   NORMAL              — copying source verbatim, watching for tokens
//   IN_LINE_COMMENT     — copy until newline
//   IN_BLOCK_COMMENT    — copy until closing `*/`
//   IN_STRING           — copy until closing `"` (handles `\"`)
//
// On NORMAL at a statement-start boundary (BOF, `;`, `}`, or just after
// a newline whose preceding non-blank is one of those), we look ahead
// for `Identifier Identifier ... {`. If the first identifier ends with
// the suffix "Data" / "Body" / "Image", we emit the rewritten form.

#include "console/torquescript/dialectATransform.h"

#include <cctype>
#include <cstring>

namespace studio { namespace content { namespace cscript {

namespace {

bool isIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool isIdentBody (char c) { return std::isalnum((unsigned char)c) || c == '_'; }

bool endsWithDatablockSuffix(const char* p, std::size_t len)
{
    auto endsWith = [&](const char* suf) {
        std::size_t slen = std::strlen(suf);
        return len > slen && std::memcmp(p + len - slen, suf, slen) == 0;
    };
    return endsWith("Data") || endsWith("Body") || endsWith("Image");
}

/// Skip whitespace + comments starting at `i`. Advances `i` past them.
/// Returns true if at least one whitespace/comment character was consumed.
void skipWsAndComments(const std::string& s, std::size_t& i)
{
    while (i < s.size())
    {
        char c = s[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (c == '/' && i + 1 < s.size())
        {
            if (s[i+1] == '/')
            {
                i += 2;
                while (i < s.size() && s[i] != '\n') ++i;
                continue;
            }
            if (s[i+1] == '*')
            {
                i += 2;
                while (i + 1 < s.size() && !(s[i] == '*' && s[i+1] == '/')) ++i;
                if (i + 1 < s.size()) i += 2;
                continue;
            }
        }
        break;
    }
}

/// Parse an identifier starting at s[i]. On success advances `i` past it
/// and returns the [start, end) byte range. On failure returns (i,i).
std::pair<std::size_t, std::size_t> matchIdent(const std::string& s, std::size_t& i)
{
    if (i >= s.size() || !isIdentStart(s[i])) return {i, i};
    std::size_t start = i;
    ++i;
    while (i < s.size() && isIdentBody(s[i])) ++i;
    return {start, i};
}

} // namespace

std::string transformTribes1Datablocks(const std::string& source)
{
    std::string out;
    out.reserve(source.size() + 64);

    enum State { NORMAL, LINE_COMMENT, BLOCK_COMMENT, STRING };
    State st = NORMAL;
    char stringQuote = '"';

    // True when the next NORMAL token would be at a statement-start
    // position. Initially true (start of file). Set true after we emit
    // a `;` or `}`. Stays true through whitespace and comments — they
    // don't end a statement boundary. Reset by anything else.
    bool atStmtStart = true;

    // True when the last non-whitespace/comment token we emitted was
    // `=`, meaning the next NORMAL token starts an expression value.
    // Used to recognise `slot = { a, b, c };` T1 vector literals.
    bool afterEquals = false;

    std::size_t i = 0;
    while (i < source.size())
    {
        char c = source[i];

        // Handle non-NORMAL states first (verbatim copy).
        if (st == LINE_COMMENT)
        {
            out.push_back(c);
            if (c == '\n') st = NORMAL;
            ++i; continue;
        }
        if (st == BLOCK_COMMENT)
        {
            out.push_back(c);
            if (c == '*' && i + 1 < source.size() && source[i+1] == '/')
            {
                out.push_back('/'); i += 2;
                st = NORMAL; continue;
            }
            ++i; continue;
        }
        if (st == STRING)
        {
            out.push_back(c);
            if (c == '\\' && i + 1 < source.size())
            {
                out.push_back(source[i+1]); i += 2; continue;
            }
            if (c == stringQuote) st = NORMAL;
            ++i; continue;
        }

        // NORMAL — look for state transitions first. Comments are NOT
        // a statement-boundary reset; strings ARE (they're an expression
        // value that must precede `;`).
        if (c == '/' && i + 1 < source.size() && source[i+1] == '/')
        { out.push_back(c); out.push_back('/'); i += 2; st = LINE_COMMENT; continue; }
        if (c == '/' && i + 1 < source.size() && source[i+1] == '*')
        { out.push_back(c); out.push_back('*'); i += 2; st = BLOCK_COMMENT; continue; }
        // T1 quirk: a few corpus scripts (baseProjData.cs, sound.cs,
        // trees.cs, loadShow.cs) use `#` as a line comment. The
        // Torque scanner rejects it. Rewrite to `//` so downstream
        // parsing succeeds. Only at statement-start positions to
        // avoid eating `#` inside identifiers or strings.
        if (c == '#' && atStmtStart)
        { out.push_back('/'); out.push_back('/'); ++i; st = LINE_COMMENT; continue; }
        if (c == '"' || c == '\'')
        { out.push_back(c); ++i; st = STRING; stringQuote = c;
          atStmtStart = false; afterEquals = false; continue; }

        // T1 vector/color literal: `= { a, b, c };` after an `=`. Flatten
        // to a quoted space-separated string so the Torque grammar (which
        // doesn't understand brace-init expressions) parses it as a string.
        if (c == '{' && afterEquals)
        {
            std::size_t j = i + 1;
            std::string tokens;
            std::string acc;
            int depth = 1;
            bool ok = true;
            auto flush = [&](){
                // trim
                std::size_t a = 0;
                while (a < acc.size() && std::isspace((unsigned char)acc[a])) ++a;
                std::size_t b = acc.size();
                while (b > a && std::isspace((unsigned char)acc[b-1])) --b;
                if (b > a)
                {
                    if (!tokens.empty()) tokens.push_back(' ');
                    tokens.append(acc, a, b - a);
                }
                acc.clear();
            };
            while (j < source.size() && depth > 0)
            {
                char d = source[j];
                if (d == '{') { ++depth; acc.push_back(d); ++j; continue; }
                if (d == '}') { --depth; if (depth == 0) { flush(); ++j; break; }
                                acc.push_back(d); ++j; continue; }
                if (d == ',') { flush(); ++j; continue; }
                if (d == '"' || d == '\'')
                { // bail on strings inside the brace list
                  ok = false; break; }
                acc.push_back(d);
                ++j;
            }
            if (ok && depth == 0)
            {
                out.push_back('"');
                out.append(tokens);
                out.push_back('"');
                i = j;
                afterEquals = false;
                atStmtStart = false;
                continue;
            }
            // fall through to normal handling if we bailed
        }

        // Whitespace doesn't change statement-start status.
        if (std::isspace((unsigned char)c))
        { out.push_back(c); ++i; continue; }

        // Possible datablock declaration: at statement boundary AND
        // current char starts an identifier ending in Data/Body/Image.
        if (isIdentStart(c) && atStmtStart)
        {
            std::size_t look = i;
            auto t1 = matchIdent(source, look);
            std::size_t tNameStart = t1.first;
            std::size_t tNameEnd   = t1.second;
            if (tNameEnd > tNameStart
                && endsWithDatablockSuffix(source.data() + tNameStart, tNameEnd - tNameStart))
            {
                std::size_t afterType = look;
                skipWsAndComments(source, look);
                auto t2 = matchIdent(source, look);
                std::size_t iNameStart = t2.first;
                std::size_t iNameEnd   = t2.second;
                if (iNameEnd > iNameStart)
                {
                    std::size_t afterInst = look;
                    skipWsAndComments(source, look);
                    if (look < source.size() && source[look] == '{')
                    {
                        // Match! Emit rewritten form.
                        // Already in `out` is everything up to `i`. Append:
                        //     datablock TYPE(INST)<ws-between-type-and-instance-preserved-as-spaces>
                        // and then continue scanning from afterInst (so the
                        // whitespace between instance and `{` is preserved).
                        out.append("datablock ");
                        out.append(source.data() + tNameStart, tNameEnd - tNameStart);
                        out.push_back('(');
                        out.append(source.data() + iNameStart, iNameEnd - iNameStart);
                        out.push_back(')');
                        // Skip over the type-instance pair entirely; resume
                        // from `afterInst` to preserve interior whitespace
                        // (newlines, comments) up to the `{`.
                        (void)afterType;
                        i = afterInst;
                        atStmtStart = false; // inside the new datablock decl
                        continue;
                    }
                }
            }
        }

        // Any non-whitespace token here means we're no longer at a
        // statement boundary. Update the flag based on the char we're
        // about to emit, then emit it.
        if (c == ';' || c == '}')
        { atStmtStart = true; afterEquals = false; }
        else
            atStmtStart = false;

        // Track a single `=` for the brace-list heuristic. Skip multi-char
        // operators like `==`, `<=`, `>=`, `!=`, `+=` etc.
        if (c == '=')
        {
            bool isComposite =
                (i + 1 < source.size() && source[i+1] == '=') ||  // ==
                (!out.empty() && (out.back() == '!' || out.back() == '<' ||
                                  out.back() == '>' || out.back() == '+' ||
                                  out.back() == '-' || out.back() == '*' ||
                                  out.back() == '/' || out.back() == '%'));
            afterEquals = !isComposite;
        }
        else if (!std::isspace((unsigned char)c) && c != '{')
        {
            afterEquals = false;
        }

        out.push_back(c);
        ++i;
    }

    return out;
}

}}} // namespace studio::content::cscript
