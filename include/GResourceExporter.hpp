#pragma once
#include "CurvzDocument.hpp"
#include <string>
#include <vector>
#include <functional>

// ── GResourceExporter ─────────────────────────────────────────────────────────
// Orchestrates a full icon theme export to a directory on disk.
//
// Output structure:
//   <output_dir>/
//     scalable/<category>/<name>-symbolic.svg      ← clean SVG (no data-curvz-*)
//     16x16/<category>/<name>-symbolic.png
//     24x24/<category>/<name>-symbolic.png
//     32x32/<category>/<name>-symbolic.png
//     48x48/<category>/<name>-symbolic.png
//     64x64/<category>/<name>-symbolic.png
//     128x128/<category>/<name>-symbolic.png
//     256x256/<category>/<name>-symbolic.png
//     gresource.xml      ← references scalable/ + 16x16/ only (GTK app use)
//     index.theme        ← for system-wide hicolor installation
//     README.md          ← comprehensive usage guide
//
// Icons with empty export_category or empty export_name are skipped.

namespace Curvz {

struct ExportEntry {
    CurvzDocument* doc    = nullptr;
    bool           include = true;   // user toggle in ExportDialog checklist
};

struct ExportResult {
    bool        success       = false;
    int         icons_written = 0;
    int         icons_skipped = 0;   // missing name/category
    std::string error_message;
    std::string output_dir;
};

// Run the full export. Progress callback receives (current, total, icon_name).
// Pass nullptr for progress_cb if not needed.
ExportResult export_theme(
    const std::vector<ExportEntry>& entries,
    const std::string& output_dir,
    const std::string& theme_name,       // e.g. "MyApp"
    const std::string& theme_comment,    // e.g. "Icons for MyApp"
    std::function<void(int,int,const std::string&)> progress_cb = nullptr);

} // namespace Curvz
