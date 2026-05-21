// Parser for Tribes 1 `.mis` ConsoleScript mission files.
//
// Hand-written recursive-descent parser.  The grammar is small enough
// that PEGTL would add complexity without benefit; a state-machine
// tokeniser + recursive object reader is far easier to reason about.
//
// Grammar summary (informal):
//
//   file        := BEGIN_MAGIC ws object ws END_MAGIC ws trailer
//   object      := 'instant' ws classname ws optname ws body_or_semi
//   body_or_semi := '{' ws (property | object)* '}' ';'
//               |  ';'
//   property    := key '[' INT ']' ws '=' ws quoted_str ';'
//               |  key ws '=' ws quoted_str ';'
//   trailer     := trailer_line*
//   trailer_line := exec_line | var_line | comment | empty
//
// Line endings in shipped missions are CRLF; the tokeniser strips `\r`.

#include "content/mission/mis.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>

namespace studio::content::mission
{
  // -------------------------------------------------------------------------
  // Tokeniser
  // -------------------------------------------------------------------------

  namespace
  {
    // Magic sentinels
    constexpr const char* kBeginMagic = "//--- export object begin ---//";
    constexpr const char* kEndMagic   = "//--- export object end ---//";

    // Strip CR so CRLF and LF both work.
    std::string read_whole(std::istream& in)
    {
      std::string s;
      s.reserve(65536);
      char c;
      while (in.get(c))
      {
        if (c != '\r') s.push_back(c);
      }
      return s;
    }

    // A cursor into a string with line-tracking for error messages.
    struct Cursor
    {
      const std::string& src;
      std::size_t pos = 0;
      int line = 1;

      bool eof() const { return pos >= src.size(); }

      char peek() const { return eof() ? '\0' : src[pos]; }

      char get()
      {
        char c = src[pos++];
        if (c == '\n') ++line;
        return c;
      }

      // Skip whitespace (space, tab, newline).
      void skip_ws()
      {
        while (!eof() && std::isspace(static_cast<unsigned char>(src[pos])))
        {
          if (src[pos] == '\n') ++line;
          ++pos;
        }
      }

      // Skip whitespace and //-style comments.
      void skip_ws_and_comments()
      {
        for (;;)
        {
          skip_ws();
          if (pos + 1 < src.size() && src[pos] == '/' && src[pos + 1] == '/')
          {
            // Skip to end of line.
            while (!eof() && src[pos] != '\n') ++pos;
          }
          else
          {
            break;
          }
        }
      }

      // Try to consume a literal string.  Returns true and advances on
      // success, returns false and leaves position unchanged on failure.
      bool consume(const char* literal)
      {
        std::size_t len = std::strlen(literal);
        if (pos + len > src.size()) return false;
        if (src.compare(pos, len, literal) != 0) return false;
        for (std::size_t i = 0; i < len; ++i)
        {
          if (src[pos + i] == '\n') ++line;
        }
        pos += len;
        return true;
      }

      // Read a bare identifier: [A-Za-z_][A-Za-z0-9_:]*
      // Also accepts '$' prefix for $-variables in trailer context.
      std::string read_ident(bool allow_dollar = false)
      {
        std::string out;
        if (eof()) return out;
        char first = src[pos];
        bool ok_start = std::isalpha(static_cast<unsigned char>(first))
                     || first == '_'
                     || (allow_dollar && first == '$');
        if (!ok_start) return out;
        out.push_back(get());
        while (!eof())
        {
          char c = src[pos];
          if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == ':')
          {
            out.push_back(get());
          }
          else
          {
            break;
          }
        }
        return out;
      }

      // Read a double-quoted string.  Returns the content (no outer
      // quotes).  Throws on unclosed string.
      std::string read_quoted()
      {
        if (eof() || src[pos] != '"')
        {
          throw std::runtime_error(
            "expected '\"' at line " + std::to_string(line));
        }
        ++pos;  // consume opening '"'
        std::string out;
        while (!eof())
        {
          char c = get();
          if (c == '"') return out;
          out.push_back(c);
        }
        throw std::runtime_error(
          "unclosed string literal at line " + std::to_string(line));
      }

      void expect(char c)
      {
        if (eof() || src[pos] != c)
        {
          throw std::runtime_error(
            std::string("expected '") + c + "' at line " + std::to_string(line)
            + " but got '"
            + (eof() ? std::string("EOF") : std::string(1, src[pos])) + "'");
        }
        get();
      }
    };

    // -----------------------------------------------------------------------
    // Recursive object parser
    // -----------------------------------------------------------------------

    mis_object parse_object(Cursor& cur);

    // Parse the body `{ prop* obj* '}' ';'` or just `;` (empty body).
    void parse_body(Cursor& cur, mis_object& obj)
    {
      cur.skip_ws_and_comments();
      if (cur.peek() == ';')
      {
        // Empty body: `instant Foo "Bar";`
        cur.get();
        return;
      }
      cur.expect('{');
      for (;;)
      {
        cur.skip_ws_and_comments();
        if (cur.peek() == '}') break;

        // Peek ahead: is this a child object (`instant`) or a property?
        if (cur.src.compare(cur.pos, 7, "instant") == 0
            && (cur.pos + 7 >= cur.src.size()
                || !std::isalnum(static_cast<unsigned char>(cur.src[cur.pos + 7]))))
        {
          obj.children.push_back(parse_object(cur));
        }
        else
        {
          // Property: key[N] = "value"; or key = "value";
          mis_property prop;

          // Read the key name (may include alphanumerics and underscores).
          while (!cur.eof())
          {
            char c = cur.src[cur.pos];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            {
              prop.key.push_back(cur.get());
            }
            else
            {
              break;
            }
          }

          if (prop.key.empty())
          {
            throw std::runtime_error(
              "expected property key at line " + std::to_string(cur.line));
          }

          // Optional array subscript.
          if (!cur.eof() && cur.peek() == '[')
          {
            cur.get();  // consume '['
            std::string idx_str;
            while (!cur.eof() && cur.peek() != ']')
            {
              idx_str.push_back(cur.get());
            }
            cur.expect(']');
            std::int32_t idx = 0;
            auto [ptr, ec] = std::from_chars(
              idx_str.data(), idx_str.data() + idx_str.size(), idx);
            if (ec == std::errc{})
            {
              prop.array_index = idx;
            }
          }

          cur.skip_ws();
          cur.expect('=');
          cur.skip_ws();
          prop.value = cur.read_quoted();
          cur.skip_ws();
          cur.expect(';');

          obj.properties.push_back(std::move(prop));
        }
      }
      cur.expect('}');
      cur.skip_ws();
      cur.expect(';');
    }

    mis_object parse_object(Cursor& cur)
    {
      cur.skip_ws_and_comments();

      // Consume `instant` keyword.
      if (!cur.consume("instant"))
      {
        throw std::runtime_error(
          "expected 'instant' at line " + std::to_string(cur.line));
      }

      cur.skip_ws();

      // Class name: may include any alphanumeric + underscore characters.
      // The file ships one `simGroup` in lower-case; store verbatim,
      // comparisons should be case-insensitive at the call site.
      mis_object obj;
      while (!cur.eof())
      {
        char c = cur.src[cur.pos];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
        {
          obj.class_name.push_back(cur.get());
        }
        else
        {
          break;
        }
      }

      if (obj.class_name.empty())
      {
        throw std::runtime_error(
          "expected class name at line " + std::to_string(cur.line));
      }

      cur.skip_ws();

      // Optional instance name (double-quoted string).
      if (!cur.eof() && cur.peek() == '"')
      {
        obj.instance_name = cur.read_quoted();
        cur.skip_ws();
      }

      parse_body(cur, obj);
      return obj;
    }

    // -----------------------------------------------------------------------
    // Trailer parser
    // -----------------------------------------------------------------------

    mis_trailer parse_trailer(const std::string& src, std::size_t start_pos)
    {
      mis_trailer t;

      // Simple line-by-line scan; no PEG needed for these few patterns.
      Cursor cur{src, start_pos, 0};

      while (!cur.eof())
      {
        cur.skip_ws_and_comments();
        if (cur.eof()) break;

        // exec(ident);
        if (cur.src.compare(cur.pos, 4, "exec") == 0)
        {
          cur.pos += 4;
          cur.skip_ws();
          cur.expect('(');
          cur.skip_ws();
          std::string ident;
          // exec arg is either a bare ident or a quoted string
          if (!cur.eof() && cur.peek() == '"')
          {
            ident = cur.read_quoted();
          }
          else
          {
            while (!cur.eof() && cur.peek() != ')' && !std::isspace(static_cast<unsigned char>(cur.peek())))
            {
              ident.push_back(cur.get());
            }
          }
          cur.skip_ws();
          cur.expect(')');
          cur.skip_ws();
          if (!cur.eof() && cur.peek() == ';') cur.get();
          if (!ident.empty()) t.exec_idents.push_back(ident);
          continue;
        }

        // $-variable assignment.
        if (!cur.eof() && cur.peek() == '$')
        {
          // Read $VarName including :: scope separator.
          std::string varname;
          cur.get();  // consume '$'
          while (!cur.eof())
          {
            char c = cur.src[cur.pos];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':')
            {
              varname.push_back(cur.get());
            }
            else
            {
              break;
            }
          }
          cur.skip_ws();
          if (cur.eof() || cur.peek() != '=')
          {
            // Not an assignment; skip rest of line.
            while (!cur.eof() && cur.peek() != '\n') cur.get();
            continue;
          }
          cur.get();  // consume '='
          cur.skip_ws();

          // Read the value: quoted string or bare number.
          std::string value;
          if (!cur.eof() && cur.peek() == '"')
          {
            value = cur.read_quoted();
          }
          else
          {
            while (!cur.eof() && cur.peek() != ';' && cur.peek() != '\n')
            {
              value.push_back(cur.get());
            }
            // Trim trailing whitespace.
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            {
              value.pop_back();
            }
          }
          if (!cur.eof() && cur.peek() == ';') cur.get();

          // Match to known trailer variables (case-insensitive comparison).
          auto lower_eq = [](const std::string& a, const char* b) {
            std::string la(a.size(), '\0');
            std::transform(a.begin(), a.end(), la.begin(), [](unsigned char c){ return std::tolower(c); });
            return la == b;
          };

          if (lower_eq(varname, "game::missiontype"))
          {
            t.game_mission_type = value;
          }
          else if (lower_eq(varname, "teamscorelimit"))
          {
            std::int32_t v = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), v).ec == std::errc{})
              t.team_score_limit = v;
          }
          else if (lower_eq(varname, "dmscorelimit"))
          {
            std::int32_t v = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), v).ec == std::errc{})
              t.dm_score_limit = v;
          }
          else if (lower_eq(varname, "cdtrack"))
          {
            std::int32_t v = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), v).ec == std::errc{})
              t.cd_track = v;
          }
          else if (lower_eq(varname, "cdplaymode"))
          {
            std::int32_t v = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), v).ec == std::errc{})
              t.cd_play_mode = v;
          }
          continue;
        }

        // Skip any other line (e.g. blank lines, unknown statements).
        while (!cur.eof() && cur.peek() != '\n') cur.get();
      }

      return t;
    }

  }  // anonymous namespace

  // -------------------------------------------------------------------------
  // Public API
  // -------------------------------------------------------------------------

  bool is_mis_file(std::istream& in)
  {
    auto saved = in.tellg();
    char buf[64] = {};
    in.read(buf, sizeof(buf) - 1);
    std::size_t n = static_cast<std::size_t>(in.gcount());
    in.clear();
    in.seekg(saved, std::ios::beg);

    // Strip CR from what we read for comparison.
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
      if (buf[i] != '\r') s.push_back(buf[i]);
    }
    return s.compare(0, std::strlen(kBeginMagic), kBeginMagic) == 0;
  }

  mis_file read_mis_file(std::istream& in)
  {
    std::string src = read_whole(in);

    Cursor cur{src, 0, 1};

    // Expect the begin magic on the first line.
    cur.skip_ws();
    if (!cur.consume(kBeginMagic))
    {
      throw std::runtime_error(
        "missing '//--- export object begin ---//' magic at start of file");
    }
    cur.skip_ws_and_comments();

    // Parse the single top-level object (always SimGroup "MissionGroup").
    mis_object root = parse_object(cur);

    cur.skip_ws_and_comments();

    // Locate the end-magic; trailer comes after it.
    std::size_t end_magic_pos = src.find(kEndMagic, cur.pos);
    std::size_t trailer_start = 0;
    if (end_magic_pos != std::string::npos)
    {
      trailer_start = end_magic_pos + std::strlen(kEndMagic);
    }
    else
    {
      // No end magic; treat remaining content as trailer.
      trailer_start = cur.pos;
    }

    mis_trailer trailer = parse_trailer(src, trailer_start);

    return mis_file{std::move(root), std::move(trailer)};
  }

  // -------------------------------------------------------------------------
  // Value helpers
  // -------------------------------------------------------------------------

  std::optional<float> parse_float(std::string_view sv)
  {
    if (sv.empty()) return std::nullopt;

    // Trim leading/trailing whitespace.
    std::size_t start = 0;
    while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start]))) ++start;
    std::size_t end = sv.size();
    while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1]))) --end;
    sv = sv.substr(start, end - start);
    if (sv.empty()) return std::nullopt;

    // strtof handles NaN, Inf, denormals, and scientific notation natively.
    // std::stof throws on subnormal values (e.g. 1.03844e-38), so we use
    // the C function directly.
    std::string tmp(sv);
    char* endp = nullptr;
    float v = std::strtof(tmp.c_str(), &endp);
    if (endp == nullptr || endp == tmp.c_str()) return std::nullopt;
    // Ensure the whole token was consumed (allow trailing whitespace already
    // stripped above).
    if (endp != tmp.c_str() + tmp.size()) return std::nullopt;
    return v;
  }

  std::optional<bool> parse_bool(std::string_view sv)
  {
    if (sv.empty()) return std::nullopt;
    std::string ls(sv.size(), '\0');
    std::transform(sv.begin(), sv.end(), ls.begin(), [](unsigned char c){ return std::tolower(c); });
    if (ls == "true" || ls == "1") return true;
    if (ls == "false" || ls == "0") return false;
    return std::nullopt;
  }

  std::optional<std::array<float, 3>> parse_vec3(std::string_view sv)
  {
    std::array<float, 3> result{};
    std::string s(sv);
    std::istringstream ss(s);
    for (int i = 0; i < 3; ++i)
    {
      std::string token;
      if (!(ss >> token)) return std::nullopt;
      auto v = parse_float(token);
      if (!v) return std::nullopt;
      result[static_cast<std::size_t>(i)] = *v;
    }
    return result;
  }

  std::optional<std::array<float, 6>> parse_vec6(std::string_view sv)
  {
    std::array<float, 6> result{};
    std::string s(sv);
    std::istringstream ss(s);
    for (int i = 0; i < 6; ++i)
    {
      std::string token;
      if (!(ss >> token)) return std::nullopt;
      auto v = parse_float(token);
      if (!v) return std::nullopt;
      result[static_cast<std::size_t>(i)] = *v;
    }
    return result;
  }

}  // namespace studio::content::mission
