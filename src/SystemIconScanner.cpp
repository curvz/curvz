#include "SystemIconScanner.hpp"
#include "CurvzLog.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <set>

namespace Curvz {
namespace fs = std::filesystem;

// Read "Name=" from index.theme; fall back to directory name on failure
std::string SystemIconScanner::read_theme_name(const std::string& theme_dir) {
    std::string index = theme_dir + "/index.theme";
    std::ifstream f(index);
    if (!f.is_open()) return fs::path(theme_dir).filename().string();
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Name=", 0) == 0)
            return line.substr(5);
    }
    return fs::path(theme_dir).filename().string();
}

// s203 m5 — Filter out SVGs that use features Curvz doesn't model.
//
// The System icons grid renders each entry as a thumbnail via
// parse_svg_file + render_thumb. After m3/m4 hardening, foreign SVGs
// that use <mask>, <filter>, <use>, or foreign (non-Curvz-marked)
// <image> elements load without crashing — but they render with wrong
// colours because we ignore the offending elements. A grid of half-
// correct thumbnails reads worse than a slightly shorter grid of
// correct ones, so we filter at scan time and pretend the funky files
// were never there.
//
// Detection strategy: read a small head of the file and string-scan
// for the four problem tag names. They all live in <defs> near the
// top of the document, so an 8KB head is enough to catch all cases
// we've seen on real icon themes. If the file is smaller than 8KB we
// read it all. Tag-name match is loose ("<mask " or "<mask>") so we
// don't false-positive on substrings inside attributes or comments.
//
// Curvz-authored files (which carry data-curvz-* markers somewhere)
// are NEVER flagged as funky — Curvz's own writer emits <filter> for
// drop-shadow, and the parser handles its own output. The rule is
// "foreign AND uses a feature we don't render." In practice, no
// system icon is Curvz-authored, so this matters mainly as future-
// proofing if Curvz output ever lands in an icon search path.
//
// Note: this is the SYSTEM ICONS thumbnail filter only. File ▸ Open
// on a funky SVG still loads it (with the visible paths showing and
// the funky parts missing) — that's a useful "extract paths from a
// foreign icon" workflow.  parse_svg_file itself is unfiltered.
static bool is_funky_svg(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;  // can't read → not funky, let it try
    char buf[8192];
    f.read(buf, sizeof(buf));
    std::streamsize n = f.gcount();
    if (n <= 0) return false;
    std::string head(buf, (size_t)n);

    // Curvz-authored escape hatch: any data-curvz-* marker means we
    // trust the writer's element choices. Cheapest possible check.
    if (head.find("data-curvz-") != std::string::npos) return false;

    // Substring match on the tag open. The trailing char filters out
    // accidental matches inside id attributes ("mask0" etc.) since a
    // real tag is followed by space, '>', or '/'.
    auto has_tag = [&head](const char* open) -> bool {
        std::string needle = open;
        size_t pos = 0;
        while ((pos = head.find(needle, pos)) != std::string::npos) {
            size_t after = pos + needle.size();
            if (after < head.size()) {
                char c = head[after];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                    c == '>' || c == '/') {
                    return true;
                }
            }
            pos = after;
        }
        return false;
    };
    if (has_tag("<mask"))   return true;
    if (has_tag("<filter")) return true;
    if (has_tag("<use"))    return true;
    if (has_tag("<image"))  return true;  // foreign image; Curvz-marked
                                          // images were excluded above
                                          // by the data-curvz- early-out.
    return false;
}

void SystemIconScanner::scan_dir(const std::string& base) {
    if (!fs::is_directory(base)) return;

    for (const auto& theme_entry : fs::directory_iterator(base)) {
        if (!theme_entry.is_directory()) continue;
        std::string theme_dir  = theme_entry.path().string();
        std::string theme_name = theme_entry.path().filename().string();

        // Collect SVG files recursively; derive category from parent dir name
        bool found_svg = false;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(theme_dir,
                fs::directory_options::skip_permission_denied, ec)) {
            if (!e.is_regular_file()) continue;
            if (e.path().extension() != ".svg") continue;

            // s203 m5 — Filter foreign-SVG-feature files BEFORE adding to
            // m_icons. See is_funky_svg above for the rationale. Skipped
            // files don't count toward found_svg either, so a theme that
            // consists entirely of funky icons (rare but possible) won't
            // appear in the Theme dropdown at all.
            if (is_funky_svg(e.path().string())) continue;

            // Category = the directory immediately under the theme root
            // e.g. /usr/share/icons/Adwaita/symbolic/actions/foo.svg → "actions"
            fs::path rel = fs::relative(e.path(), theme_dir, ec);
            if (ec) continue;
            std::string category;
            if (rel.has_parent_path()) {
                // Take the last component of the parent path
                category = rel.parent_path().filename().string();
                // Skip non-SVG subdirs like "symbolic-up-to-32" duplicates
                // by keeping only the leaf category name
            }

            SystemIcon icon;
            icon.theme    = theme_name;
            icon.category = category;
            icon.name     = e.path().stem().string();
            icon.path     = e.path().string();
            m_icons.push_back(std::move(icon));
            found_svg = true;
        }

        if (found_svg) {
            // Only add theme once (scan_dir may be called for multiple base dirs)
            bool already = std::any_of(m_themes.begin(), m_themes.end(),
                [&](const IconTheme& t){ return t.dir == theme_name; });
            if (!already) {
                IconTheme t;
                t.dir     = theme_name;
                t.display = read_theme_name(theme_dir);
                m_themes.push_back(std::move(t));
            }
        }
    }
}

void SystemIconScanner::scan() {
    m_icons.clear();
    m_themes.clear();

    // Standard search paths
    scan_dir("/usr/share/icons");
    scan_dir("/usr/local/share/icons");

    const char* home = std::getenv("HOME");
    if (home)
        scan_dir(std::string(home) + "/.local/share/icons");

    // Sort themes alphabetically by display name
    std::sort(m_themes.begin(), m_themes.end(),
        [](const IconTheme& a, const IconTheme& b){ return a.display < b.display; });

    m_scanned = true;
    LOG_INFO("SystemIconScanner: scanned {} SVG icons across {} themes",
             m_icons.size(), m_themes.size());
}

std::vector<std::string>
SystemIconScanner::categories(const std::string& theme_dir) const {
    std::set<std::string> seen;
    for (const auto& ic : m_icons) {
        if (theme_dir.empty() || ic.theme == theme_dir)
            if (!ic.category.empty())
                seen.insert(ic.category);
    }
    return std::vector<std::string>(seen.begin(), seen.end());
}

std::vector<const SystemIcon*>
SystemIconScanner::query(const std::string& theme_dir,
                         const std::string& category,
                         const std::string& filter) const {
    std::vector<const SystemIcon*> result;
    for (const auto& ic : m_icons) {
        if (!theme_dir.empty() && ic.theme != theme_dir) continue;
        if (!category.empty()  && ic.category != category) continue;
        if (!filter.empty()) {
            // Case-insensitive substring match on name
            auto it = std::search(ic.name.begin(), ic.name.end(),
                                  filter.begin(), filter.end(),
                                  [](char a, char b){
                                      return std::tolower(a) == std::tolower(b);
                                  });
            if (it == ic.name.end()) continue;
        }
        result.push_back(&ic);
    }
    // Sort by name within results
    std::sort(result.begin(), result.end(),
        [](const SystemIcon* a, const SystemIcon* b){ return a->name < b->name; });
    return result;
}

} // namespace Curvz
