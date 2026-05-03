#pragma once
#include <string>
#include <vector>
#include <functional>

namespace Curvz {

struct SystemIcon {
    std::string theme;      // e.g. "Adwaita"
    std::string category;   // e.g. "actions"
    std::string name;       // stem, e.g. "edit-copy"
    std::string path;       // absolute path to .svg
};

struct IconTheme {
    std::string dir;        // directory name, e.g. "Adwaita"
    std::string display;    // human name from index.theme, e.g. "Adwaita"
};

class SystemIconScanner {
public:
    // Scan all icon search paths synchronously.
    // Call once at startup (or lazily on first System tab open).
    void scan();

    bool is_scanned() const { return m_scanned; }

    // Returns list of themes that contain SVG icons
    const std::vector<IconTheme>& themes() const { return m_themes; }

    // Returns icons filtered by theme dir name, category ("" = all), and
    // name substring filter ("" = all). Case-insensitive name match.
    std::vector<const SystemIcon*> query(const std::string& theme_dir,
                                         const std::string& category,
                                         const std::string& filter) const;

    // All categories present in a given theme ("" = all themes)
    std::vector<std::string> categories(const std::string& theme_dir) const;

private:
    void scan_dir(const std::string& base);
    static std::string read_theme_name(const std::string& theme_dir);

    std::vector<SystemIcon> m_icons;
    std::vector<IconTheme>  m_themes;
    bool                    m_scanned = false;
};

} // namespace Curvz
