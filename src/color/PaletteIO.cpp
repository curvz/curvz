//
// PaletteIO.cpp — implementation of the GplFormat ↔ SwatchLibrary
// bridge plus bundled-palette gresource enumeration.
//

#include "color/PaletteIO.hpp"
#include "color/Color.hpp"
#include "color/Swatch.hpp"
#include "CurvzLog.hpp"

#include <giomm/resource.h>
#include <glibmm/bytes.h>
#include <glibmm/miscutils.h>  // Glib::get_user_config_dir

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace Curvz {
namespace color {
namespace palette_io {

namespace {

// Where the bundled .gpl files live in the gresource tree. Keep in
// sync with resources/resources.xml's <gresource prefix=...>.
constexpr const char* kBundledResourcePath = "/com/curvz/app/palettes";

// Strip a trailing ".gpl" extension. Returns the input unchanged if
// the suffix isn't present.
std::string strip_gpl_ext(const std::string& s) {
    constexpr const char* ext = ".gpl";
    constexpr std::size_t ext_len = 4;
    if (s.size() > ext_len &&
        s.compare(s.size() - ext_len, ext_len, ext) == 0) {
        return s.substr(0, s.size() - ext_len);
    }
    return s;
}

// Add " 2", " 3", … to `base` until the result doesn't collide with
// any existing palette name in `lib`. Returns `base` unchanged if no
// collision.
std::string disambiguate_palette_name(const SwatchLibrary& lib,
                                      const std::string&   base) {
    auto exists = [&](const std::string& candidate) {
        for (const auto& pid : lib.all_palette_ids()) {
            if (const auto* p = lib.find_palette(pid)) {
                if (p->name == candidate) return true;
            }
        }
        return false;
    };
    if (!exists(base)) return base;
    for (int n = 2; n < 1000; ++n) {
        std::string candidate = base + " " + std::to_string(n);
        if (!exists(candidate)) return candidate;
    }
    // Pathological — give up and append the numeric suffix anyway.
    LOG_WARN("PaletteIO: name disambiguation exhausted for '{}'", base);
    return base + " *";
}

} // anonymous namespace

// ── Bundled enumeration ──────────────────────────────────────────────

std::vector<std::string> enumerate_bundled() {
    std::vector<std::string> out;
    try {
        auto names = Gio::Resource::enumerate_children_global(
            kBundledResourcePath, Gio::Resource::LookupFlags::NONE);
        out.reserve(names.size());
        for (const auto& n : names) {
            // Filter to .gpl just in case other files ever land in
            // this gresource folder.
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".gpl") == 0) {
                out.push_back(strip_gpl_ext(n));
            }
        }
    } catch (const Glib::Error& e) {
        LOG_WARN("PaletteIO::enumerate_bundled: gresource error: {}",
                 e.what());
    }
    // glib-compile-resources orders alphabetically; we keep that.
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<GplPalette> load_bundled(const std::string& stem) {
    const std::string path =
        std::string(kBundledResourcePath) + "/" + stem + ".gpl";
    try {
        Glib::RefPtr<const Glib::Bytes> bytes =
            Gio::Resource::lookup_data_global(
                path, Gio::Resource::LookupFlags::NONE);
        if (!bytes) {
            LOG_WARN("PaletteIO::load_bundled: '{}' not found", path);
            return std::nullopt;
        }
        gsize sz = 0;
        const char* data = static_cast<const char*>(bytes->get_data(sz));
        std::string text(data, data + sz);
        auto parsed = parse_gpl(text);
        if (!parsed) {
            LOG_WARN("PaletteIO::load_bundled: parse failed for '{}'",
                     path);
        }
        return parsed;
    } catch (const Glib::Error& e) {
        LOG_WARN("PaletteIO::load_bundled: gresource error '{}': {}",
                 path, e.what());
        return std::nullopt;
    }
}

// ── User palette enumeration ─────────────────────────────────────────

std::string user_palettes_dir() {
    namespace fs = std::filesystem;
    return (fs::path(Glib::get_user_config_dir()) / "curvz" /
            "palettes").string();
}

std::vector<std::string> enumerate_user() {
    namespace fs = std::filesystem;
    std::vector<std::string> out;

    const std::string dir = user_palettes_dir();
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        // Not an error — the directory only exists after the user
        // exports a palette there (or creates it manually). Empty
        // list is the correct answer.
        return out;
    }

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto& path = entry.path();
        if (path.extension() != ".gpl") continue;
        out.push_back(path.stem().string());
    }
    if (ec) {
        LOG_WARN("PaletteIO::enumerate_user: directory read error in "
                 "'{}': {}", dir, ec.message());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<GplPalette> load_user(const std::string& stem) {
    namespace fs = std::filesystem;
    if (stem.empty()) return std::nullopt;
    const std::string path =
        (fs::path(user_palettes_dir()) / (stem + ".gpl")).string();

    GplPalette out;
    if (!read_gpl_file(path, out)) {
        // read_gpl_file already logged the specific cause.
        return std::nullopt;
    }
    return out;
}

// ── Import ───────────────────────────────────────────────────────────

PaletteId import_gpl_into_library(SwatchLibrary& lib,
                                  const GplPalette&  src,
                                  const std::string& palette_name) {
    if (palette_name.empty()) {
        LOG_WARN("PaletteIO::import: refusing empty palette name");
        return {};
    }

    // Step 1 — add each entry as a new SolidSwatch in the custom tier.
    // Auto-name empties with the hex string. We collect the resulting
    // ids in entry-order so the new palette preserves the file's order.
    std::vector<SwatchId> added_ids;
    added_ids.reserve(src.entries.size());

    for (const auto& e : src.entries) {
        SolidSwatch sw;
        sw.color = Color{
            channel_from_u8(e.r),
            channel_from_u8(e.g),
            channel_from_u8(e.b),
            1.0
        };
        sw.header.name = e.name.empty() ? to_hex(sw.color) : e.name;
        // Leave header.id empty so add_swatch generates a fresh one.
        SwatchId id = lib.add_swatch(Swatch{sw});
        if (id.empty()) {
            LOG_WARN("PaletteIO::import: add_swatch failed for entry '{}'",
                     sw.header.name);
            continue;
        }
        added_ids.push_back(id);
    }

    // Step 2 — create the palette with these swatches in order.
    Palette p;
    p.name     = disambiguate_palette_name(lib, palette_name);
    p.source   = "imported";  // distinguishes from "user" (created in app)
    p.swatches = std::move(added_ids);
    PaletteId pid = lib.add_palette(std::move(p));
    if (pid.empty()) {
        LOG_WARN("PaletteIO::import: add_palette failed for '{}'",
                 palette_name);
    }
    return pid;
}

// ── Export ───────────────────────────────────────────────────────────

std::optional<GplPalette> export_palette_as_gpl(const SwatchLibrary& lib,
                                                const PaletteId&     pid) {
    const Palette* p = lib.find_palette(pid);
    if (!p) {
        LOG_WARN("PaletteIO::export: palette '{}' not found", pid);
        return std::nullopt;
    }

    GplPalette out;
    out.name    = p->name;
    out.columns = 8;  // sensible default; library has no column hint

    for (const auto& sid : p->swatches) {
        const Swatch* s = lib.find_swatch(sid);
        if (!s) {
            LOG_WARN("PaletteIO::export: dangling swatch ref '{}' in "
                     "palette '{}', skipping", sid, pid);
            continue;
        }
        // Visit pulls out the kind. GPL only carries solids; gradient
        // / future kinds are skipped with a log.
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, SolidSwatch>) {
                GplEntry e;
                e.r = channel_to_u8(v.color.r);
                e.g = channel_to_u8(v.color.g);
                e.b = channel_to_u8(v.color.b);
                e.name = v.header.name;
                out.entries.push_back(std::move(e));
            } else {
                LOG_WARN("PaletteIO::export: skipping non-solid swatch "
                         "'{}' in palette '{}' (GPL is RGB-only)",
                         v.header.id, pid);
            }
        }, *s);
    }

    if (out.entries.empty()) {
        LOG_WARN("PaletteIO::export: palette '{}' has no exportable "
                 "entries", pid);
        return std::nullopt;
    }
    return out;
}

} // namespace palette_io
} // namespace color
} // namespace Curvz
