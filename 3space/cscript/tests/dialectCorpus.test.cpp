// Open Siege spec 15/05 — full-corpus dialect detector verification.
//
// Reads every .cs file in a given directory and classifies each. Compares
// against an expected dialect supplied via CLI. Outputs a summary and a
// non-zero exit code if any file misclassifies. Used to drive the
// already-extracted scripts.vol and ted.vol corpora.
//
//   ./cscript_dialect_corpus <directory> A    # expect dialect A
//   ./cscript_dialect_corpus <directory> B    # expect dialect B
//
// Build target: cscript_dialect_corpus (EXCLUDE_FROM_ALL).

#include "console/torquescript/dialectDetect.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using studio::content::cscript::TorqueScript::CScriptDialect;
using studio::content::cscript::TorqueScript::detectDialect;

static const char* dialectName(CScriptDialect d)
{
    return d == CScriptDialect::A ? "A" : "B";
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr,
            "usage: %s <directory> {A|B}\n"
            "  classifies every .cs file in <directory> and compares to expectation\n",
            argv[0]);
        return 2;
    }

    fs::path dir = argv[1];
    if (!fs::is_directory(dir))
    {
        std::fprintf(stderr, "error: %s is not a directory\n", argv[1]);
        return 2;
    }

    CScriptDialect expected =
        (argv[2][0] == 'A' || argv[2][0] == 'a') ? CScriptDialect::A : CScriptDialect::B;

    // scripts.vol is NOT 100% dialect-A: two files (changeMission.cs and
    // loadShow.cs) actually contain dialect-B grammar (focusServer +
    // alias). They're treated as documented exceptions to the
    // "scripts.vol == dialect A" expectation, not detector bugs.
    static const char* const kKnownDialectBInScriptsVol[] = {
        "changeMission.cs", "loadShow.cs", nullptr,
    };
    auto isKnownExceptionAsB = [](const std::string& fname) {
        for (const char* const* p = kKnownDialectBInScriptsVol; *p; ++p)
            if (fname == *p) return true;
        return false;
    };

    int totalScanned = 0;
    int totalMatched = 0;
    int totalMismatched = 0;
    std::string mismatches;

    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (!entry.is_regular_file())
            continue;
        std::string ext = entry.path().extension().string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".cs")
            continue;

        std::ifstream f(entry.path(), std::ios::binary);
        if (!f)
            continue;
        std::ostringstream buf;
        buf << f.rdbuf();
        std::string contents = buf.str();

        CScriptDialect got = detectDialect(contents.c_str(), contents.size());
        ++totalScanned;

        // Treat known-exception files (dialect-B scripts that happen to
        // be packaged in scripts.vol) as matches when the expectation is A
        // and the detector returns B.
        bool isException = (expected == CScriptDialect::A
            && got == CScriptDialect::B
            && isKnownExceptionAsB(entry.path().filename().string()));

        if (got == expected || isException)
        {
            ++totalMatched;
        }
        else
        {
            ++totalMismatched;
            mismatches += "  ";
            mismatches += entry.path().filename().string();
            mismatches += " classified as ";
            mismatches += dialectName(got);
            mismatches += "\n";
        }
    }

    std::printf(
        "cscript_dialect_corpus: %s expectation=%s scanned=%d matched=%d mismatched=%d\n",
        dir.string().c_str(),
        dialectName(expected),
        totalScanned, totalMatched, totalMismatched);

    if (totalMismatched > 0)
        std::printf("mismatches:\n%s", mismatches.c_str());

    return totalMismatched == 0 ? 0 : 1;
}
