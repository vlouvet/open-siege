#pragma once

// Spec 25/08 — Help menu actions: docs URL resolver, About modal,
// online URL constants.

#include <string>

namespace dts_viewer {

// Online mirror — same URL for all binary versions in v1; future
// per-tag versioning is a packaging concern.
extern const char* kOnlineDocsUrl;
extern const char* kReportIssueUrl;

// Resolve a docs page URL. Walks upward from the executable directory
// looking for `share/open-siege/docs/<page>` (installer layout) or
// `docs/user/_site/<page>` (developer layout). Falls back to the
// online mirror with a stderr warning.
//
// `page` examples: "index.html", "controls.html", "scripting.html"
std::string docs_url_for(const char* page);

// About modal — version, build SHA, vendored-licence list.
void about_modal_open();
void about_modal_draw();

} // namespace dts_viewer
