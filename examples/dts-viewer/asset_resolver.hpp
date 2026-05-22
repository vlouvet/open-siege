#pragma once
#include <filesystem>
#include <string>

namespace dts_viewer {

namespace fs = std::filesystem;

// Returns the validated Tribes 1.41 install directory.
//
// Resolution order:
//   1. --tribes-dir <path> CLI override (if strip_tribes_dir_arg was called)
//   2. Cached path in configPath() / config.toml
//   3. Interactive terminal prompt (first-run)
//
// Returns {} if the user aborts the prompt (empty path).
fs::path resolveTribesDir();

// Checks for sentinel files that identify a Tribes 1.41 install dir.
bool validateTribesDir(const fs::path& dir);

// Platform config directory:
//   Linux:   $XDG_CONFIG_HOME/open-siege  (fallback: ~/.config/open-siege)
//   Windows: %APPDATA%/open-siege
//   macOS:   ~/Library/Application Support/open-siege
fs::path configDir();
fs::path configPath();  // configDir() / "config.toml"

// Call before resolveTribesDir() to handle --tribes-dir in argv.
// Strips the flag + value from argv/argc and caches the value for
// resolveTribesDir() to pick up.
void stripTribesDir(int& argc, char** argv);

} // namespace dts_viewer
