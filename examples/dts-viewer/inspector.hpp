#pragma once

// Spec 25/06 — Inspector panel. Read-only metadata view of the
// currently-selected asset entry or SimObject. v1 supports asset
// entries from the Asset Browser; SimObject support will land
// alongside the spec 25/07 click-picker.

#include <filesystem>
#include <string>

namespace dts_viewer {

enum class SelectionKind { None, AssetEntry };

struct Selection
{
    SelectionKind         kind = SelectionKind::None;
    std::filesystem::path vol_path;     // for AssetEntry
    std::string           entry_name;
    std::size_t           size = 0;
};

void inspector_set_selection(Selection sel);
void inspector_clear_selection();
void inspector_draw(bool& visible);

} // namespace dts_viewer
