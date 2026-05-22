#ifndef DTS_VIEWER_SCREENSHOT_HPP
#define DTS_VIEWER_SCREENSHOT_HPP

// Screenshot capture — Spec 04 (08-walkable-viewer track).
//
// PNG would require a zlib dependency we don't currently link; v1 writes
// binary PPM (P6) — the format Preview.app and most image viewers open
// natively.  Filename: <out_dir>/YYYY-MM-DD_HHMMSS_<mission>.ppm.
//
// glReadPixels returns rows bottom-up; this helper flips them top-down
// before writing.

#define GL_SILENCE_DEPRECATION
#include "gl_includes.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dts_viewer
{

// Returns the absolute output path on success, empty path on failure.
std::filesystem::path capture_screenshot(
    int viewport_width,
    int viewport_height,
    const std::filesystem::path& out_dir,
    const std::string& mission_name);

// Low-level: write `rgba_top_left_origin` (W * H * 4 bytes) to disk as PPM.
bool write_ppm(
    const std::filesystem::path& path,
    int width,
    int height,
    const std::uint8_t* rgba_top_left_origin);

} // namespace dts_viewer

#endif // DTS_VIEWER_SCREENSHOT_HPP
