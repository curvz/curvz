#include "TemplateLibrary.hpp"
#include "CurvzLog.hpp"
#include "PngExporter.hpp"
#include "SvgParser.hpp"
#include "SvgWriter.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <glibmm/miscutils.h>
#include <nlohmann/json.hpp>

namespace Curvz {
namespace templates {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr const char* k_bundle_ext     = ".curvztpl";
static constexpr const char* k_svg_name       = "template.svg";
static constexpr const char* k_meta_name      = "template.json";
// Legacy single-motif filename. Treated as the dark variant when scan() finds
// it without motif-suffixed siblings; never written by current code.
static constexpr const char* k_thumb_name     = "thumbnail.png";
// Motif-suffixed names — current write format. read_bundle() prefers these
// over the legacy name when both are present.
static constexpr const char* k_thumb_name_dark  = "thumbnail-dark.png";
static constexpr const char* k_thumb_name_light = "thumbnail-light.png";
static constexpr const char* k_defaults_name  = "defaults.json";
static constexpr int         k_thumb_size_px  = 256;
static constexpr int         k_schema_version = 1;

static const char* k_default_categories[] = {
    "icons", "print", "web", "social", nullptr
};

// ── Paths ─────────────────────────────────────────────────────────────────────

std::string user_dir() {
    return Glib::get_user_config_dir() + std::string("/curvz/templates");
}

std::string system_dir() {
    return "/usr/share/curvz/templates";
}

// ── Slugify ───────────────────────────────────────────────────────────────────

std::string slugify(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    bool last_dash = true;
    for (unsigned char ch : name) {
        if (std::isalnum(ch)) {
            out.push_back((char)std::tolower(ch));
            last_dash = false;
        } else {
            if (!last_dash) {
                out.push_back('-');
                last_dash = true;
            }
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "template";
    return out;
}

// ── ISO-8601 UTC timestamp ────────────────────────────────────────────────────

static std::string now_utc_iso8601() {
    using namespace std::chrono;
    auto now      = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm     tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

// ── Cheap viewBox peek ────────────────────────────────────────────────────────
// Reads up to the first 2KB of an SVG file looking for the root `viewBox`
// attribute and returns width/height as an aspect ratio (w/h). Avoids the full
// SvgParser cost — scan() runs every dialog open, so this stays light. Returns
// 0.0 on any failure (file not readable, no viewBox, malformed numbers); the
// dialog falls back to a 1:1 placeholder when the cached ratio is zero.
static double peek_svg_aspect(const std::string& svg_path) {
    std::ifstream f(svg_path);
    if (!f) return 0.0;
    char buf[2048];
    f.read(buf, sizeof(buf) - 1);
    std::streamsize n = f.gcount();
    if (n <= 0) return 0.0;
    buf[n] = '\0';
    std::string head(buf);

    // Find viewBox attribute, accept either case (SVGs are typically lowercase
    // but tolerate the camelCase form too).
    auto pos = head.find("viewBox");
    if (pos == std::string::npos) pos = head.find("viewbox");
    if (pos == std::string::npos) return 0.0;

    // Skip ahead to the opening quote.
    pos = head.find_first_of("\"'", pos);
    if (pos == std::string::npos) return 0.0;
    char quote = head[pos];
    auto end = head.find(quote, pos + 1);
    if (end == std::string::npos) return 0.0;

    // viewBox = "min-x min-y width height" — four numbers, whitespace or
    // comma separated. We only need width and height (the 3rd and 4th).
    std::string vb = head.substr(pos + 1, end - pos - 1);
    double a, b, w, h;
    if (std::sscanf(vb.c_str(), "%lf %lf %lf %lf", &a, &b, &w, &h) != 4) {
        // Try comma-separated.
        for (char& c : vb) if (c == ',') c = ' ';
        if (std::sscanf(vb.c_str(), "%lf %lf %lf %lf", &a, &b, &w, &h) != 4) {
            return 0.0;
        }
    }
    if (h <= 0.0 || w <= 0.0) return 0.0;
    return w / h;
}

// ── Built-in entry helpers ────────────────────────────────────────────────────

bool is_builtin(const TemplateEntry& e) {
    return e.meta.category == k_builtin_category &&
           (e.slug == k_builtin_blank_slug || e.slug == k_builtin_default_slug);
}

BuiltinKind builtin_kind(const TemplateEntry& e) {
    if (e.meta.category != k_builtin_category) return BuiltinKind::None;
    if (e.slug == k_builtin_blank_slug)   return BuiltinKind::Blank;
    if (e.slug == k_builtin_default_slug) return BuiltinKind::Default;
    return BuiltinKind::None;
}

// ── Metadata JSON round-trip ──────────────────────────────────────────────────

static bool read_meta(const std::string& path, TemplateMeta& out) {
    std::ifstream f(path);
    if (!f) return false;
    json j;
    try { f >> j; }
    catch (const json::exception& e) {
        LOG_WARN("TemplateLibrary: parse error in '{}': {}", path, e.what());
        return false;
    }
    out.name        = j.value("name",        std::string{});
    out.category    = j.value("category",    std::string{});
    out.description = j.value("description", std::string{});
    out.author      = j.value("author",      std::string{});
    out.created_utc = j.value("created_utc", std::string{});
    // Proportional rules (S63). Missing fields default to 0 = "do not apply",
    // which matches pre-S63 template behavior.
    out.grid_divisions    = j.value("grid_divisions",    0);
    out.margin_ratio      = j.value("margin_ratio",      0.0);
    out.grid_offset_ratio = j.value("grid_offset_ratio", 0.0);
    return true;
}

static bool write_meta(const std::string& path, const TemplateMeta& meta) {
    json j = {
        {"curvz_template_version", k_schema_version},
        {"name",                   meta.name},
        {"category",               meta.category},
        {"description",            meta.description},
        {"author",                 meta.author},
        {"created_utc",            meta.created_utc},
        {"grid_divisions",         meta.grid_divisions},
        {"margin_ratio",           meta.margin_ratio},
        {"grid_offset_ratio",      meta.grid_offset_ratio}
    };
    std::ofstream f(path);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return f.good();
}

// ── Bundle path helpers ───────────────────────────────────────────────────────

static std::string bundle_path_for(const std::string& root,
                                   const std::string& category,
                                   const std::string& slug) {
    return (fs::path(root) / category / (slug + k_bundle_ext)).string();
}

static bool read_bundle(const std::string& bundle, bool is_user,
                        TemplateEntry& out) {
    fs::path base(bundle);
    std::string dir_name = base.filename().string();
    std::string slug = dir_name;
    std::string ext  = k_bundle_ext;
    if (slug.size() > ext.size() &&
        slug.compare(slug.size() - ext.size(), ext.size(), ext) == 0) {
        slug.resize(slug.size() - ext.size());
    }

    std::string svg          = (base / k_svg_name).string();
    std::string meta         = (base / k_meta_name).string();
    std::string thumb_legacy = (base / k_thumb_name).string();
    std::string thumb_dark   = (base / k_thumb_name_dark).string();
    std::string thumb_light  = (base / k_thumb_name_light).string();

    std::error_code ec;
    if (!fs::is_regular_file(svg, ec))  return false;
    if (!fs::is_regular_file(meta, ec)) return false;

    TemplateMeta m;
    if (!read_meta(meta, m)) return false;
    std::string dir_category = base.parent_path().filename().string();
    if (!dir_category.empty()) m.category = dir_category;
    if (m.name.empty()) m.name = slug;

    // Thumb path resolution: prefer motif-suffixed files, fall back to the
    // legacy single-thumb name (treated as the dark variant). When only the
    // legacy file exists, the light slot stays empty and the regen path
    // produces it on first view in light motif. The legacy file is left in
    // place — it remains valid as the dark variant.
    out.bundle_path = bundle;
    out.svg_path    = svg;
    if (fs::is_regular_file(thumb_dark, ec)) {
        out.thumb_path_dark = thumb_dark;
    } else if (fs::is_regular_file(thumb_legacy, ec)) {
        out.thumb_path_dark = thumb_legacy;
    }
    if (fs::is_regular_file(thumb_light, ec)) {
        out.thumb_path_light = thumb_light;
    }
    out.aspect_ratio = peek_svg_aspect(svg);  // 0.0 on parse failure
    out.slug         = slug;
    out.meta         = std::move(m);
    out.is_user      = is_user;
    return true;
}

// ── Ensure user root exists with default categories ───────────────────────────

static void ensure_user_root() {
    std::error_code ec;
    std::string root = user_dir();
    fs::create_directories(root, ec);
    if (ec) {
        LOG_WARN("TemplateLibrary: cannot create user root '{}': {}",
                 root, ec.message());
        return;
    }
    for (int i = 0; k_default_categories[i]; ++i) {
        fs::create_directories(fs::path(root) / k_default_categories[i], ec);
        if (ec) {
            LOG_WARN("TemplateLibrary: cannot create category '{}/{}': {}",
                     root, k_default_categories[i], ec.message());
            ec.clear();
        }
    }

    // Opportunistic cleanup: prior versions seeded a "blank" category
    // directory. Blank is now a built-in template (synthesized by scan()),
    // not a category. Remove the stale directory only if it's empty — never
    // touch user files. If the user has somehow stashed bundles in there,
    // it renders as a regular user category they can rename or empty.
    fs::path stale_blank = fs::path(root) / "blank";
    if (fs::is_directory(stale_blank, ec) && fs::is_empty(stale_blank, ec)) {
        fs::remove(stale_blank, ec);
        if (!ec) {
            LOG_INFO("TemplateLibrary: removed stale empty 'blank/' category dir");
        } else {
            ec.clear();
        }
    }
}

// Reusable "sanity check: path lives under user_dir()" guard for mutating ops.
// Prevents any mutating API from touching the system root or paths with
// traversal sequences.
static bool path_under_user_root(const std::string& abs_path,
                                 std::string* canon_out = nullptr) {
    std::error_code ec;
    fs::path p(abs_path);
    fs::path root(user_dir());
    auto p_c    = fs::weakly_canonical(p, ec);    if (ec) return false;
    auto root_c = fs::weakly_canonical(root, ec); if (ec) return false;
    auto p_str  = p_c.string();
    auto r_str  = root_c.string();
    bool under  = (p_str.rfind(r_str, 0) == 0);
    if (under && canon_out) *canon_out = p_str;
    return under;
}

// ── Scan ──────────────────────────────────────────────────────────────────────

std::vector<TemplateEntry> scan() {
    ensure_user_root();

    std::vector<TemplateEntry> all;

    auto walk_root = [&](const std::string& root, bool is_user) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) return;
        for (const auto& cat_entry : fs::directory_iterator(root, ec)) {
            if (!cat_entry.is_directory()) continue;
            std::string cat_name = cat_entry.path().filename().string();
            if (cat_name.empty() || cat_name[0] == '.') continue;

            std::error_code ec2;
            for (const auto& bundle : fs::directory_iterator(cat_entry.path(), ec2)) {
                if (!bundle.is_directory()) continue;
                std::string bname = bundle.path().filename().string();
                std::string ext = k_bundle_ext;
                if (bname.size() <= ext.size()) continue;
                if (bname.compare(bname.size() - ext.size(), ext.size(), ext) != 0)
                    continue;

                TemplateEntry e;
                if (read_bundle(bundle.path().string(), is_user, e))
                    all.push_back(std::move(e));
            }
        }
    };

    walk_root(system_dir(), /*is_user=*/false);
    walk_root(user_dir(),   /*is_user=*/true);

    std::stable_sort(all.begin(), all.end(),
        [](const TemplateEntry& a, const TemplateEntry& b) {
            if (a.meta.category != b.meta.category) return a.meta.category < b.meta.category;
            if (a.slug != b.slug) return a.slug < b.slug;
            return !a.is_user && b.is_user;
        });

    std::vector<TemplateEntry> deduped;
    deduped.reserve(all.size());
    for (auto& e : all) {
        if (!deduped.empty()
            && deduped.back().meta.category == e.meta.category
            && deduped.back().slug == e.slug) {
            deduped.back() = std::move(e);
        } else {
            deduped.push_back(std::move(e));
        }
    }

    // Append synthetic built-in entries (Blank + Default). These live in the
    // protected "builtin" category. No SVG on disk — load is handled by the
    // caller via is_builtin()/builtin_kind(). They are_user=false (not
    // deletable, not renameable via the manager) but the category ITSELF is
    // a valid drop target for user templates.
    //
    // Placed before the final sort so they interleave normally with any user
    // bundles the user has moved into the builtin category. Slugs "blank" and
    // "default" are reserved; the move-collision logic elsewhere bumps
    // incoming user templates to "-2" suffix if they collide here.
    {
        TemplateEntry blank;
        blank.slug            = k_builtin_blank_slug;
        blank.is_user         = false;
        blank.meta.name       = "Blank";
        blank.meta.category   = k_builtin_category;
        blank.meta.description= "Empty canvas, no grid";
        deduped.push_back(std::move(blank));

        TemplateEntry deflt;
        deflt.slug            = k_builtin_default_slug;
        deflt.is_user         = false;
        deflt.meta.name       = "Default";
        deflt.meta.category   = k_builtin_category;
        deflt.meta.description= "Default canvas with grid";
        deduped.push_back(std::move(deflt));
    }

    std::stable_sort(deduped.begin(), deduped.end(),
        [](const TemplateEntry& a, const TemplateEntry& b) {
            if (a.is_user != b.is_user) return !a.is_user;
            if (a.meta.category != b.meta.category) return a.meta.category < b.meta.category;
            return a.meta.name < b.meta.name;
        });

    LOG_INFO("TemplateLibrary: scanned {} templates", deduped.size());
    return deduped;
}

// ── user_categories / system_categories ───────────────────────────────────────

std::vector<std::string> user_categories() {
    ensure_user_root();
    std::vector<std::string> out;
    std::error_code ec;
    std::string root = user_dir();
    if (!fs::is_directory(root, ec)) return out;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory()) continue;
        std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        out.push_back(std::move(name));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> system_categories() {
    std::vector<std::string> out;
    std::error_code ec;
    std::string root = system_dir();
    if (fs::is_directory(root, ec)) {
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (!e.is_directory()) continue;
            std::string name = e.path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            out.push_back(std::move(name));
        }
    }
    // Also include the seeded defaults — these are "system-protected" even
    // if no system bundle lives there (the scan always recreates them).
    for (int i = 0; k_default_categories[i]; ++i) {
        std::string name = k_default_categories[i];
        if (std::find(out.begin(), out.end(), name) == out.end())
            out.push_back(name);
    }
    // And the synthetic "builtin" category — it has no disk presence in the
    // system root, but hosts the Blank and Default built-in templates (and
    // is a valid drop target for user templates the user wants to keep
    // alongside them). Locked against rename/delete by the manager.
    if (std::find(out.begin(), out.end(), std::string(k_builtin_category)) == out.end())
        out.push_back(k_builtin_category);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ── Dimensions label for thumbnails (S63 M2) ─────────────────────────────────
// Produces a human-readable canvas spec matching the doc's display mode.
// Physical:  "8.5 × 11 in"
// Pixel:     "1920 × 1080 px"
// Ratio:     "16:9 · 1000q"

static std::string dimensions_label_for(const CurvzDocument& doc) {
    char buf[96];
    switch (doc.canvas.display_mode) {
    case DisplayMode::Physical: {
        // Trim trailing zeros on integers (e.g. "11" not "11.0")
        auto fmt = [](double v) -> std::string {
            char b[32];
            if (v == std::floor(v)) snprintf(b, sizeof(b), "%.0f", v);
            else                    snprintf(b, sizeof(b), "%g",   v);
            return b;
        };
        snprintf(buf, sizeof(buf), "%s \xc3\x97 %s %s",
                 fmt(doc.canvas.phys_width).c_str(),
                 fmt(doc.canvas.phys_height).c_str(),
                 doc.canvas.phys_unit.c_str());
        return buf;
    }
    case DisplayMode::Pixel:
        snprintf(buf, sizeof(buf), "%d \xc3\x97 %d px",
                 doc.canvas_width(), doc.canvas_height());
        return buf;
    case DisplayMode::RatioQuality: {
        // Try to express as a clean integer ratio if close; otherwise decimal.
        double rw = doc.canvas.ratio_w;
        double rh = doc.canvas.ratio_h;
        auto near_int = [](double v) {
            return std::fabs(v - std::round(v)) < 1e-3;
        };
        if (near_int(rw) && near_int(rh)) {
            snprintf(buf, sizeof(buf), "%d:%d \xc2\xb7 %dq",
                     (int)std::round(rw), (int)std::round(rh),
                     doc.canvas.quality);
        } else {
            snprintf(buf, sizeof(buf), "%.3g:%.3g \xc2\xb7 %dq",
                     rw, rh, doc.canvas.quality);
        }
        return buf;
    }
    }
    return {};
}

// ── Save ──────────────────────────────────────────────────────────────────────

bool save(const CurvzDocument& doc, TemplateMeta meta,
          double dark_workspace_r,  double dark_workspace_g,  double dark_workspace_b,
          double dark_artboard_r,   double dark_artboard_g,   double dark_artboard_b,
          double light_workspace_r, double light_workspace_g, double light_workspace_b,
          double light_artboard_r,  double light_artboard_g,  double light_artboard_b,
          std::string* out_bundle_path) {
    if (meta.name.empty()) {
        LOG_ERROR("TemplateLibrary::save: empty name");
        return false;
    }
    if (meta.category.empty()) {
        LOG_ERROR("TemplateLibrary::save: empty category");
        return false;
    }
    if (meta.created_utc.empty()) meta.created_utc = now_utc_iso8601();

    std::string slug   = slugify(meta.name);
    std::string root   = user_dir();
    std::string bundle = bundle_path_for(root, meta.category, slug);

    std::error_code ec;
    fs::create_directories(bundle, ec);
    if (ec) {
        LOG_ERROR("TemplateLibrary::save: cannot create '{}': {}",
                  bundle, ec.message());
        return false;
    }

    std::string svg_path         = (fs::path(bundle) / k_svg_name).string();
    std::string meta_path        = (fs::path(bundle) / k_meta_name).string();
    std::string thumb_dark_path  = (fs::path(bundle) / k_thumb_name_dark).string();
    std::string thumb_light_path = (fs::path(bundle) / k_thumb_name_light).string();

    if (!write_svg_file(doc, svg_path)) {
        LOG_ERROR("TemplateLibrary::save: write_svg_file failed for '{}'", svg_path);
        return false;
    }
    // m4: write BOTH motif thumbs eagerly. Save is already a slow user-
    // initiated operation, so doubling thumbnail render cost is invisible
    // and means the dialog never has to lazy-regen for newly-saved templates.
    if (!export_template_thumbnail(doc, thumb_dark_path, k_thumb_size_px,
                                   meta.name,
                                   dimensions_label_for(doc),
                                   meta.grid_divisions,
                                   meta.margin_ratio,
                                   meta.grid_offset_ratio,
                                   dark_workspace_r, dark_workspace_g, dark_workspace_b,
                                   dark_artboard_r,  dark_artboard_g,  dark_artboard_b)) {
        LOG_WARN("TemplateLibrary::save: dark thumbnail render failed for '{}'",
                 thumb_dark_path);
    }
    if (!export_template_thumbnail(doc, thumb_light_path, k_thumb_size_px,
                                   meta.name,
                                   dimensions_label_for(doc),
                                   meta.grid_divisions,
                                   meta.margin_ratio,
                                   meta.grid_offset_ratio,
                                   light_workspace_r, light_workspace_g, light_workspace_b,
                                   light_artboard_r,  light_artboard_g,  light_artboard_b)) {
        LOG_WARN("TemplateLibrary::save: light thumbnail render failed for '{}'",
                 thumb_light_path);
    }
    if (!write_meta(meta_path, meta)) {
        LOG_ERROR("TemplateLibrary::save: write_meta failed for '{}'", meta_path);
        return false;
    }

    if (out_bundle_path) *out_bundle_path = bundle;
    LOG_INFO("TemplateLibrary::save: wrote '{}' (category='{}', name='{}')",
             bundle, meta.category, meta.name);
    return true;
}

bool user_bundle_exists(const std::string& category, const std::string& name) {
    std::string slug   = slugify(name);
    std::string bundle = bundle_path_for(user_dir(), category, slug);
    std::error_code ec;
    return fs::is_directory(bundle, ec);
}

// ── Lazy thumbnail regen ─────────────────────────────────────────────────────
// Cache-or-regen: if the bundle already has a PNG for the requested motif,
// returns its path immediately. Otherwise loads the SVG, renders the thumbnail
// at the bundle's per-motif filename, returns that path. Returns empty on any
// failure so the caller falls back to a procedural placeholder.
//
// Safe to call from a worker thread — no GTK widget access, only filesystem
// + Cairo + the SvgParser used by load_document. The PNG cache write is
// last-write-wins; if two threads race on the same template+motif (unlikely
// in practice — the dialog stages its kickoffs), the second write just
// replaces the first and both threads return the same path.

std::string ensure_thumb_for_motif(const TemplateEntry& entry,
                                   MotifTag motif,
                                   double workspace_r, double workspace_g, double workspace_b,
                                   double artboard_r,  double artboard_g,  double artboard_b) {
    if (is_builtin(entry)) {
        // Builtins have no bundle directory; the dialog draws them
        // procedurally via NewDocumentDialog::draw_builtin_thumb.
        return {};
    }
    if (entry.bundle_path.empty()) {
        LOG_WARN("ensure_thumb_for_motif: empty bundle_path for '{}'", entry.slug);
        return {};
    }

    const char* fname = (motif == MotifTag::Light) ? k_thumb_name_light
                                                    : k_thumb_name_dark;
    std::string out_path = (fs::path(entry.bundle_path) / fname).string();

    // Cache hit — file already on disk.
    std::error_code ec;
    if (fs::is_regular_file(out_path, ec)) {
        return out_path;
    }

    // Cache miss — render. load_document does the SVG parse + applies the
    // S63 proportional rules from meta; the rendered thumb sees the full
    // grid/margin layers as configured by the template.
    auto doc = load_document(entry);
    if (!doc) {
        LOG_ERROR("ensure_thumb_for_motif: load_document failed for '{}'",
                  entry.svg_path);
        return {};
    }
    if (!export_template_thumbnail(*doc, out_path, k_thumb_size_px,
                                   entry.meta.name,
                                   dimensions_label_for(*doc),
                                   entry.meta.grid_divisions,
                                   entry.meta.margin_ratio,
                                   entry.meta.grid_offset_ratio,
                                   workspace_r, workspace_g, workspace_b,
                                   artboard_r,  artboard_g,  artboard_b)) {
        LOG_ERROR("ensure_thumb_for_motif: render failed for '{}'", out_path);
        return {};
    }
    LOG_INFO("ensure_thumb_for_motif: regenerated '{}'", out_path);
    return out_path;
}

// ── Proportional rules (S63) ──────────────────────────────────────────────────

void apply_template_proportions(CurvzDocument& doc, const TemplateMeta& meta) {
    if (meta.grid_divisions <= 0
        && meta.margin_ratio == 0.0
        && meta.grid_offset_ratio == 0.0) {
        // All fields disabled — leave doc untouched.
        return;
    }

    // Grid + margin are keyed to the short axis of the canvas. This keeps the
    // visual cell size consistent regardless of whether the canvas is square,
    // landscape, or portrait: e.g. a 1618×1000 icon and a 1000×1618 icon both
    // get 100-unit grid cells at 10 divisions.
    double short_axis = (double)std::min(doc.canvas_width(),
                                         doc.canvas_height());

    double spacing = 0.0;
    if (meta.grid_divisions > 0) {
        spacing = short_axis / (double)meta.grid_divisions;
    }

    if (spacing > 0.0) {
        SceneNode* gl = doc.ensure_grid_layer();
        gl->grid_spacing_x = spacing;
        gl->grid_spacing_y = spacing;
        double off = spacing * meta.grid_offset_ratio;
        gl->grid_offset_x = off;
        gl->grid_offset_y = off;
        gl->visible = true;
    }

    if (meta.margin_ratio > 0.0 && spacing > 0.0) {
        double m = spacing * meta.margin_ratio;
        SceneNode* ml = doc.ensure_margin_layer();
        ml->margin_top    = m;
        ml->margin_bottom = m;
        ml->margin_left   = m;
        ml->margin_right  = m;
        ml->visible       = true;
    }
}

// ── load_document ─────────────────────────────────────────────────────────────

std::unique_ptr<CurvzDocument> load_document(const TemplateEntry& entry) {
    if (is_builtin(entry)) {
        // Builtin templates have no SVG on disk. The caller is responsible
        // for routing these to its own seed builders via builtin_kind().
        LOG_WARN("TemplateLibrary::load_document: refusing builtin entry '{}/{}' — caller must use builtin_kind()",
                 entry.meta.category, entry.slug);
        return nullptr;
    }
    if (entry.svg_path.empty()) {
        LOG_ERROR("TemplateLibrary::load_document: empty svg_path");
        return nullptr;
    }
    auto doc = parse_svg_file(entry.svg_path);
    if (!doc) {
        LOG_ERROR("TemplateLibrary::load_document: parse failed for '{}'",
                  entry.svg_path);
        return nullptr;
    }
    doc->filename.clear();
    // S63: apply any proportional grid/margin rules carried in the template's
    // metadata. No-op for templates without the rule fields (pre-S63 bundles).
    apply_template_proportions(*doc, entry.meta);
    return doc;
}

// ── Unique-bundle-slug helper for rename/move collision handling ──────────────
// Given a target category under user root and a desired slug, returns a slug
// that doesn't collide with an existing bundle there. Appends -2, -3, ... as
// needed. If `current_bundle_path` is non-empty, the same bundle at that path
// is ignored for collision purposes (so renaming to the same name is a no-op
// rather than a suffix).
static std::string unique_slug_in(const std::string& category,
                                  const std::string& desired_slug,
                                  const std::string& current_bundle_path = "") {
    std::string root  = user_dir();
    std::string base  = desired_slug;
    std::error_code ec;
    int suffix = 1;
    std::string slug = base;
    // "blank" and "default" are reserved inside the builtin category —
    // they're owned by the synthesized built-in entries. User templates
    // moved or renamed into builtin must step aside to avoid scan-time
    // collision.
    auto is_reserved = [&category](const std::string& s) -> bool {
        if (category != k_builtin_category) return false;
        return s == k_builtin_blank_slug || s == k_builtin_default_slug;
    };
    while (true) {
        std::string cand = bundle_path_for(root, category, slug);
        if (!is_reserved(slug) &&
            (!fs::exists(cand, ec) ||
             (!current_bundle_path.empty() && cand == current_bundle_path))) {
            return slug;
        }
        ++suffix;
        slug = base + "-" + std::to_string(suffix);
    }
}

// ── User default pointer (read/write defaults.json) ───────────────────────────

// The file lives at ~/.config/curvz/templates/defaults.json with shape:
//   { "default_bundle": "icons/my-template" }
// Value is "<category>/<slug>" — no file extension, no prefix. Resolution
// happens through scan() so system templates can also serve as the default.

static std::string defaults_file_path() {
    return (fs::path(user_dir()) / k_defaults_name).string();
}

static std::optional<std::string> read_default_pointer() {
    std::ifstream f(defaults_file_path());
    if (!f) return std::nullopt;
    json j;
    try { f >> j; }
    catch (const json::exception& e) {
        LOG_WARN("TemplateLibrary: defaults.json parse error: {}", e.what());
        return std::nullopt;
    }
    std::string p = j.value("default_bundle", std::string{});
    if (p.empty()) return std::nullopt;
    return p;
}

static bool write_default_pointer(const std::string& cat_slash_slug) {
    ensure_user_root();
    json j = {{"default_bundle", cat_slash_slug}};
    std::ofstream f(defaults_file_path());
    if (!f) return false;
    f << j.dump(2) << "\n";
    return f.good();
}

std::optional<TemplateEntry> user_default() {
    auto ptr = read_default_pointer();
    if (!ptr) return std::nullopt;
    // Parse "category/slug"
    auto slash = ptr->find('/');
    if (slash == std::string::npos) return std::nullopt;
    std::string cat  = ptr->substr(0, slash);
    std::string slug = ptr->substr(slash + 1);
    if (cat.empty() || slug.empty()) return std::nullopt;

    // Resolve through scan() rather than direct filesystem access, so we use
    // the same precedence rules (user shadows system).
    auto all = scan();
    for (const auto& e : all) {
        if (e.meta.category == cat && e.slug == slug) return e;
    }
    // Dangling pointer — reference exists but bundle is gone. Treat as unset.
    LOG_INFO("TemplateLibrary::user_default: pointer '{}' does not resolve — treating as unset",
             *ptr);
    return std::nullopt;
}

bool set_user_default(const TemplateEntry& entry) {
    if (entry.meta.category.empty() || entry.slug.empty()) {
        LOG_ERROR("TemplateLibrary::set_user_default: invalid entry");
        return false;
    }
    std::string val = entry.meta.category + "/" + entry.slug;
    if (!write_default_pointer(val)) {
        LOG_ERROR("TemplateLibrary::set_user_default: write failed");
        return false;
    }
    LOG_INFO("TemplateLibrary::set_user_default: '{}'", val);
    return true;
}

bool clear_user_default() {
    std::error_code ec;
    fs::remove(defaults_file_path(), ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        LOG_WARN("TemplateLibrary::clear_user_default: remove error: {}", ec.message());
        return false;
    }
    LOG_INFO("TemplateLibrary::clear_user_default");
    return true;
}

// ── remove ────────────────────────────────────────────────────────────────────

bool remove(const TemplateEntry& entry) {
    if (!entry.is_user) {
        LOG_WARN("TemplateLibrary::remove: refusing to remove system bundle '{}'",
                 entry.bundle_path);
        return false;
    }
    if (entry.bundle_path.empty()) return false;

    std::string canon;
    if (!path_under_user_root(entry.bundle_path, &canon)) {
        LOG_ERROR("TemplateLibrary::remove: '{}' not under user root",
                  entry.bundle_path);
        return false;
    }

    // If this bundle is the current default, clear the pointer first.
    auto def = user_default();
    if (def && def->meta.category == entry.meta.category && def->slug == entry.slug)
        clear_user_default();

    std::error_code ec;
    auto n = fs::remove_all(entry.bundle_path, ec);
    if (ec) {
        LOG_ERROR("TemplateLibrary::remove: remove_all failed for '{}': {}",
                  entry.bundle_path, ec.message());
        return false;
    }
    LOG_INFO("TemplateLibrary::remove: deleted '{}' ({} entries)",
             entry.bundle_path, (unsigned long long)n);
    return true;
}

// ── rename_template ───────────────────────────────────────────────────────────

bool rename_template(const TemplateEntry& entry, const std::string& new_name,
                     std::string* out_bundle_path) {
    if (!entry.is_user) {
        LOG_WARN("TemplateLibrary::rename_template: refusing system bundle '{}'",
                 entry.bundle_path);
        return false;
    }
    if (new_name.empty()) {
        LOG_ERROR("TemplateLibrary::rename_template: empty new_name");
        return false;
    }
    if (!path_under_user_root(entry.bundle_path)) {
        LOG_ERROR("TemplateLibrary::rename_template: '{}' not under user root",
                  entry.bundle_path);
        return false;
    }

    std::string new_slug_base = slugify(new_name);
    std::string new_slug = unique_slug_in(entry.meta.category, new_slug_base,
                                          entry.bundle_path);
    std::string new_bundle = bundle_path_for(user_dir(), entry.meta.category,
                                              new_slug);

    // If rename produces the same path (same name), just update the json.
    if (new_bundle == entry.bundle_path) {
        // Just rewrite metadata with the new display name.
        TemplateMeta m = entry.meta;
        m.name = new_name;
        std::string meta_path = (fs::path(entry.bundle_path) / k_meta_name).string();
        if (!write_meta(meta_path, m)) {
            LOG_ERROR("TemplateLibrary::rename_template: meta rewrite failed");
            return false;
        }
        if (out_bundle_path) *out_bundle_path = entry.bundle_path;
        LOG_INFO("TemplateLibrary::rename_template: in-place meta update for '{}'",
                 entry.bundle_path);
        return true;
    }

    // Move the directory on disk.
    std::error_code ec;
    fs::rename(entry.bundle_path, new_bundle, ec);
    if (ec) {
        LOG_ERROR("TemplateLibrary::rename_template: fs::rename '{}' -> '{}' failed: {}",
                  entry.bundle_path, new_bundle, ec.message());
        return false;
    }

    // Update template.json inside the new path.
    TemplateMeta m = entry.meta;
    m.name = new_name;
    std::string meta_path = (fs::path(new_bundle) / k_meta_name).string();
    if (!write_meta(meta_path, m)) {
        LOG_WARN("TemplateLibrary::rename_template: meta rewrite failed after rename");
        // Don't fail overall — the rename succeeded even if metadata is stale
    }

    // If this bundle was the default, update the pointer to the new slug.
    auto def = user_default();
    if (def && def->meta.category == entry.meta.category && def->slug == entry.slug) {
        TemplateEntry new_entry = entry;
        new_entry.slug = new_slug;
        set_user_default(new_entry);
    }

    if (out_bundle_path) *out_bundle_path = new_bundle;
    LOG_INFO("TemplateLibrary::rename_template: '{}' -> '{}' (slug='{}')",
             entry.bundle_path, new_bundle, new_slug);
    return true;
}

// ── move_template ─────────────────────────────────────────────────────────────

bool move_template(const TemplateEntry& entry, const std::string& new_category,
                   std::string* out_bundle_path) {
    if (!entry.is_user) {
        LOG_WARN("TemplateLibrary::move_template: refusing system bundle '{}'",
                 entry.bundle_path);
        return false;
    }
    if (new_category.empty()) {
        LOG_ERROR("TemplateLibrary::move_template: empty new_category");
        return false;
    }
    if (!path_under_user_root(entry.bundle_path)) {
        LOG_ERROR("TemplateLibrary::move_template: '{}' not under user root",
                  entry.bundle_path);
        return false;
    }
    if (new_category == entry.meta.category) {
        // No-op — same category. Just report success.
        if (out_bundle_path) *out_bundle_path = entry.bundle_path;
        return true;
    }

    // Ensure the target category directory exists.
    std::string target_cat_dir = (fs::path(user_dir()) / new_category).string();
    std::error_code ec;
    fs::create_directories(target_cat_dir, ec);
    if (ec) {
        LOG_ERROR("TemplateLibrary::move_template: cannot create category dir '{}': {}",
                  target_cat_dir, ec.message());
        return false;
    }

    // Handle name collision at the destination.
    std::string new_slug = unique_slug_in(new_category, entry.slug);
    std::string new_bundle = bundle_path_for(user_dir(), new_category, new_slug);

    fs::rename(entry.bundle_path, new_bundle, ec);
    if (ec) {
        LOG_ERROR("TemplateLibrary::move_template: fs::rename '{}' -> '{}' failed: {}",
                  entry.bundle_path, new_bundle, ec.message());
        return false;
    }

    // Update template.json's category field (directory wins at scan-time, but
    // keep json in sync so out-of-band readers see the correct category).
    TemplateMeta m = entry.meta;
    m.category = new_category;
    std::string meta_path = (fs::path(new_bundle) / k_meta_name).string();
    if (!write_meta(meta_path, m)) {
        LOG_WARN("TemplateLibrary::move_template: meta rewrite failed after move");
    }

    // Update default pointer if it referenced this bundle.
    auto def = user_default();
    if (def && def->meta.category == entry.meta.category && def->slug == entry.slug) {
        TemplateEntry new_entry = entry;
        new_entry.meta.category = new_category;
        new_entry.slug          = new_slug;
        set_user_default(new_entry);
    }

    if (out_bundle_path) *out_bundle_path = new_bundle;
    LOG_INFO("TemplateLibrary::move_template: '{}' -> '{}'",
             entry.bundle_path, new_bundle);
    return true;
}

// ── create_category ───────────────────────────────────────────────────────────

std::string create_category(const std::string& raw_name) {
    ensure_user_root();
    std::string slug = slugify(raw_name.empty() ? "new-category" : raw_name);
    if (slug.empty()) slug = "new-category";

    // Collision: append -2, -3, ...
    std::string root = user_dir();
    std::error_code ec;
    std::string final_name = slug;
    int suffix = 1;
    while (fs::exists(fs::path(root) / final_name, ec)) {
        ++suffix;
        final_name = slug + "-" + std::to_string(suffix);
    }

    fs::create_directories(fs::path(root) / final_name, ec);
    if (ec) {
        LOG_ERROR("TemplateLibrary::create_category: mkdir failed for '{}': {}",
                  final_name, ec.message());
        return {};
    }
    LOG_INFO("TemplateLibrary::create_category: '{}'", final_name);
    return final_name;
}

// ── rename_category ───────────────────────────────────────────────────────────

bool rename_category(const std::string& old_name, const std::string& new_name_raw) {
    if (old_name.empty() || new_name_raw.empty()) return false;
    if (old_name == new_name_raw) return true; // no-op

    std::string new_name = slugify(new_name_raw);
    if (new_name.empty()) return false;

    std::string root = user_dir();
    std::string old_path = (fs::path(root) / old_name).string();
    std::string new_path = (fs::path(root) / new_name).string();

    std::error_code ec;
    if (!fs::is_directory(old_path, ec)) {
        LOG_ERROR("TemplateLibrary::rename_category: '{}' is not a directory", old_path);
        return false;
    }
    if (fs::exists(new_path, ec)) {
        LOG_ERROR("TemplateLibrary::rename_category: '{}' already exists", new_path);
        return false;
    }
    if (!path_under_user_root(old_path) || !path_under_user_root(new_path)) {
        LOG_ERROR("TemplateLibrary::rename_category: path escape check failed");
        return false;
    }

    fs::rename(old_path, new_path, ec);
    if (ec) {
        LOG_ERROR("TemplateLibrary::rename_category: rename failed: {}", ec.message());
        return false;
    }

    // Rewrite category field in each bundle's template.json so the json
    // stays consistent with the directory.
    for (const auto& sub : fs::directory_iterator(new_path, ec)) {
        if (!sub.is_directory()) continue;
        std::string meta_file = (sub.path() / k_meta_name).string();
        TemplateMeta m;
        if (read_meta(meta_file, m)) {
            m.category = new_name;
            write_meta(meta_file, m);
        }
    }

    // Update default pointer if it lived under the old category.
    auto def_raw = read_default_pointer();
    if (def_raw) {
        auto slash = def_raw->find('/');
        if (slash != std::string::npos &&
            def_raw->substr(0, slash) == old_name) {
            std::string slug = def_raw->substr(slash + 1);
            write_default_pointer(new_name + "/" + slug);
        }
    }

    LOG_INFO("TemplateLibrary::rename_category: '{}' -> '{}'", old_name, new_name);
    return true;
}

// ── delete_category ───────────────────────────────────────────────────────────

bool delete_category(const std::string& name) {
    if (name.empty()) return false;
    std::string cat_path = (fs::path(user_dir()) / name).string();
    if (!path_under_user_root(cat_path)) {
        LOG_ERROR("TemplateLibrary::delete_category: '{}' not under user root", cat_path);
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(cat_path, ec)) {
        LOG_WARN("TemplateLibrary::delete_category: '{}' is not a directory", cat_path);
        return false;
    }

    // If the current default lives under this category, clear it first.
    auto def_raw = read_default_pointer();
    if (def_raw) {
        auto slash = def_raw->find('/');
        if (slash != std::string::npos &&
            def_raw->substr(0, slash) == name) {
            clear_user_default();
        }
    }

    auto n = fs::remove_all(cat_path, ec);
    if (ec) {
        LOG_ERROR("TemplateLibrary::delete_category: remove_all failed for '{}': {}",
                  cat_path, ec.message());
        return false;
    }
    LOG_INFO("TemplateLibrary::delete_category: '{}' ({} entries)",
             cat_path, (unsigned long long)n);
    return true;
}

} // namespace templates
} // namespace Curvz
