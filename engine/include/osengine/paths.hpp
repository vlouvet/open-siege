#ifndef OSENGINE_PATHS_HPP
#define OSENGINE_PATHS_HPP

// Per-binary config + cache + assets dirs — spec 26/08.
//
// All three binaries (dts-viewer, open-siege-t1-client, t1-server) share
// a single assets root (the Tribes 1.41 install) and their own
// per-binary config/cache subdirectory.
//
// Layout (macOS):
//   ~/Library/Application Support/open-siege/<binary>/
//   ~/Library/Caches/open-siege/<binary>/
//   ~/Library/Application Support/open-siege/shared/tribes-dir.txt
//
// Layout (Linux):
//   $XDG_CONFIG_HOME/open-siege/<binary>/  (fallback ~/.config/...)
//   $XDG_CACHE_HOME/open-siege/<binary>/   (fallback ~/.cache/...)
//   $XDG_CONFIG_HOME/open-siege/shared/tribes-dir.txt
//
// Layout (Windows):
//   %APPDATA%/open-siege/<binary>/
//   %LOCALAPPDATA%/open-siege/<binary>/
//   %APPDATA%/open-siege/shared/tribes-dir.txt

#include <filesystem>
#include <string_view>

namespace os_paths
{

// Per-binary config root. Dir is NOT created here — callers create it
// the first time they write something into it.
std::filesystem::path config_dir(std::string_view binary);

// Per-binary cache root. Same lazy-create rule.
std::filesystem::path cache_dir(std::string_view binary);

// The shared Tribes 1.41 install path, read from
// config_dir("shared")/tribes-dir.txt (single line, plain text).
// Returns empty path if the file doesn't exist; UI binaries can prompt
// the user, headless binaries (server) should error out.
std::filesystem::path assets_dir();

// Write `path` to shared tribes-dir.txt. Creates the shared config dir
// if needed. Returns true on success.
bool set_assets_dir(const std::filesystem::path& path);

} // namespace os_paths

#endif // OSENGINE_PATHS_HPP
