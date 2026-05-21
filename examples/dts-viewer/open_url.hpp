#pragma once

// Spec 25/08 — cross-platform browser launcher. Opens `url`
// (http://, https://, or file://) in the user's default browser.
// Returns true on a best-effort success — the actual browser process
// is fire-and-forget.

#include <string>

namespace dts_viewer {

bool open_url(const std::string& url);

} // namespace dts_viewer
