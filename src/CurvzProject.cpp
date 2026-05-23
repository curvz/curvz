#include "CurvzProject.hpp"
#include "SvgWriter.hpp"
#include "SvgParser.hpp"
#include "CurvzLog.hpp"
#include "style/StyleInterop.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

namespace Curvz {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string basename(const std::string& path) {
    auto pos = path.rfind('/');
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

// ── Post-load style re-materialisation walk (S76 m3b, refactored S78 m3d) ────
//
// After load() parses every SVG document, every SceneNode with a non-empty
// bound_style needs its fill / stroke re-projected from the referenced
// style. The SVG is the last persisted truth for the rendered appearance,
// but the binding itself lives outside the SVG before m3c — and even with
// data-curvz-style round-trip, the post-load pass is what reconciles the
// in-memory cache against the live library state (catches the case where
// a style was edited externally or in another tool between save and reopen).
//
// If materialise_from_style returns false (id unknown — style deleted
// elsewhere, foreign project), clear bound_style: the SVG appearance
// stands as-is. Keeping a dangling binding would silently misrepresent
// the node's relationship to the library.
//
// Pre-m3d this was a separate static walk duplicating the structural
// shape of live_recolour_walk in Canvas.cpp. m3d lifted the recursion
// into style::walk_style_bindings (the cross-doc propagation walk in
// Canvas needs the same shape); the per-node action collapses to a
// 4-line lambda below.
//
// Project-level orchestration lives in CurvzProject::load rather than
// in StyleInterop because StyleInterop shouldn't know about project-
// load policy (drop-on-miss vs log-and-keep is a load-time decision).

// Per-node action: re-materialise from the live library. If the id is
// unknown (style deleted elsewhere, foreign project, externally-edited
// project.json), clear bound_style — the SVG-cached fill/stroke stands
// as-is and the node degrades to "unbound but visually correct".
static void materialise_or_drop_binding(SceneNode& n,
                                        const style::StyleLibrary& lib) {
    if (!style::materialise_from_style(n, lib)) {
        LOG_INFO("CurvzProject::load: node iid='{}' dropping unknown "
                 "bound_style '{}'",
                 n.internal_id, n.bound_style);
        n.bound_style.clear();
    }
}

// ── project.json schema ───────────────────────────────────────────────────────
//
// {
//   "curvz_version": 1,
//   "documents": [ { "file": "icon.svg", "canvas_w": 24, "canvas_h": 24 } ],
//   "tokens": { "stroke_width": 2.0, "corner_radius": 2.0 },
//   "editor_state": {
//     "active_doc": "icon.svg",
//     "zoom": 16.0,
//     "snap_to_pixel": true,
//     "snap_to_grid": true,
//     "grid_size": 1.0
//   },
//   "color_system": {
//     "schema_version": 1,
//     "swatches": [ { "id":..., "kind":"solid", "name":..., "color":"#..." } ],
//     "palettes": [ { "id":..., "name":..., "swatches":[...] } ],
//     "recents": [...],
//     "active_palette": "..."
//   }
// }

// ── save ─────────────────────────────────────────────────────────────────────

bool CurvzProject::save() const {
    if (directory.empty()) {
        LOG_ERROR("CurvzProject::save: no directory set");
        return false;
    }

    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        LOG_ERROR("CurvzProject::save: cannot create dir '{}': {}", directory, ec.message());
        return false;
    }

    // ── Write each SVG ────────────────────────────────────────────────────
    json doc_list = json::array();
    for (const auto& doc : documents) {
        std::string fname = basename(doc->filename);
        if (fname.empty()) fname = "untitled.svg";

        std::string svg_path = (fs::path(directory) / fname).string();
        if (!write_svg_file(*doc, svg_path)) return false;

        const CanvasModel& cm = doc->canvas;
        json canvas_j = {
            {"ratio_w",        cm.ratio_w},
            {"ratio_h",        cm.ratio_h},
            {"quality",        cm.quality},
            {"display_mode",   (int)cm.display_mode},
            {"phys_width",     cm.phys_width},
            {"phys_height",    cm.phys_height},
            {"phys_unit",      cm.phys_unit},
            {"dpi",            cm.dpi},
            {"px_width",       cm.px_width},
            {"px_height",      cm.px_height},
            {"display_unit",   UnitSystem::label(cm.display_unit)},
            // s292 m1: render-intent fields (s264 m1 / s265 m2). Must
            // persist on the JSON side too — the load path rebuilds the
            // CanvasModel from JSON and reassigns doc->canvas, wiping any
            // intent the SVG parser set. Default-zero on read so legacy
            // pre-s292 project files load with intent unset and the
            // doc->canvas SVG-side intent (if any) is preserved via the
            // fallback copy in the load path below.
            {"intended_w",     cm.intended_w},
            {"intended_h",     cm.intended_h},
            {"intended_unit",  cm.intended_unit},
            {"ruler_origin_x", doc->ruler_origin_x},
            {"ruler_origin_y", doc->ruler_origin_y}
        };
        doc_list.push_back({
            {"file",           fname},
            {"canvas",         canvas_j},
            {"export_name",     doc->export_name},
            {"export_category", doc->export_category},
            // s148 m1: artboard / workspace / creation re-promoted to
            // per-doc (single slot per doc; was project-level per-motif
            // in s116 m6). Each doc carries its own editor-presentation
            // tone — flipping app appearance no longer alters the canvas.
            //
            // The project-root "workspace" block continues to be written
            // (with motif + per-motif slots) during m1 as a downgrade-
            // safety net. m2 will trim those once doc-scope is settled.
            // s183 m5a — dual-pair motif storage. Both dark and light
            // triples written so the doc renders identically in either
            // mode without re-applying a theme. Old single-slot keys
            // (artboard_bg_*, workspace_bg_*, creation_color_*) are no
            // longer written; old files lose their custom colours and
            // fall to defaults (Scott: "they can just use a default").
            {"dark_artboard_bg_r",  doc->dark_artboard_bg_r},
            {"dark_artboard_bg_g",  doc->dark_artboard_bg_g},
            {"dark_artboard_bg_b",  doc->dark_artboard_bg_b},
            {"dark_workspace_bg_r", doc->dark_workspace_bg_r},
            {"dark_workspace_bg_g", doc->dark_workspace_bg_g},
            {"dark_workspace_bg_b", doc->dark_workspace_bg_b},
            {"dark_creation_color_r", doc->dark_creation_color_r},
            {"dark_creation_color_g", doc->dark_creation_color_g},
            {"dark_creation_color_b", doc->dark_creation_color_b},
            {"light_artboard_bg_r",  doc->light_artboard_bg_r},
            {"light_artboard_bg_g",  doc->light_artboard_bg_g},
            {"light_artboard_bg_b",  doc->light_artboard_bg_b},
            {"light_workspace_bg_r", doc->light_workspace_bg_r},
            {"light_workspace_bg_g", doc->light_workspace_bg_g},
            {"light_workspace_bg_b", doc->light_workspace_bg_b},
            {"light_creation_color_r", doc->light_creation_color_r},
            {"light_creation_color_g", doc->light_creation_color_g},
            {"light_creation_color_b", doc->light_creation_color_b},
            // S98: persist guide colour. Pre-S98 it was a per-session-
            // only field — defaults seeded from CurvzDocument, lost on
            // every project close. Per-doc since guides are doc-scope
            // (each doc has its own GuideLayer).
            {"guide_color_r",  doc->guide_color_r},
            {"guide_color_g",  doc->guide_color_g},
            {"guide_color_b",  doc->guide_color_b},
            {"grid",   [&]() -> json {
                const SceneNode* gl = doc->grid_layer();
                if (!gl) return nullptr;
                return json{
                    {"visible",    gl->visible},
                    {"locked",     gl->locked},
                    {"spacing_x",  gl->grid_spacing_x},
                    {"spacing_y",  gl->grid_spacing_y},
                    {"offset_x",   gl->grid_offset_x},
                    {"offset_y",   gl->grid_offset_y},
                    {"color_r",    gl->grid_color_r},
                    {"color_g",    gl->grid_color_g},
                    {"color_b",    gl->grid_color_b},
                    {"color_a",    gl->grid_color_a},
                    {"dots",       gl->grid_dots},
                    {"snap",       gl->grid_snap}
                };
            }()},
            {"margins", [&]() -> json {
                const SceneNode* ml = doc->margin_layer();
                if (!ml) return nullptr;
                return json{
                    {"visible",   ml->visible},
                    {"locked",    ml->locked},
                    {"top",       ml->margin_top},
                    {"bottom",    ml->margin_bottom},
                    {"left",      ml->margin_left},
                    {"right",     ml->margin_right},
                    {"columns",   ml->margin_columns},
                    {"col_gap",   ml->margin_col_gap},
                    {"rows",      ml->margin_rows},
                    {"row_gap",   ml->margin_row_gap},
                    {"color_r",   ml->margin_color_r},
                    {"color_g",   ml->margin_color_g},
                    {"color_b",   ml->margin_color_b},
                    {"color_a",   ml->margin_color_a}
                };
            }()}
        });
    }

    // ── Active doc filename ───────────────────────────────────────────────
    std::string active_file;
    if (active_doc_index >= 0 && active_doc_index < (int)documents.size())
        active_file = basename(documents[active_doc_index]->filename);

    // ── Build project.json ────────────────────────────────────────────────
    //
    // S69 M2 file-layout change:
    //   * Swatches + palettes moved out of project.json into a sibling
    //     "swatches.json" (custom tier only — defaults are app-global).
    //   * active_palette + recents (per-project working state) moved into
    //     editor_state. They used to live inside the color_system block
    //     alongside the swatches/palettes pool.
    //
    // Older projects that still have a color_system block keep working via
    // load()'s legacy-migration path; on the next save they're rewritten
    // in the new layout and the block disappears.
    json j = {
        {"curvz_version", 1},
        {"documents",     doc_list},
        {"tokens", {
            {"stroke_width",   tokens.stroke_width},
            {"corner_radius",  tokens.corner_radius}
        }},
        // Project-wide workspace appearance (s116 m6). The motif drives
        // which CSS fork the app paints (Dark/Light), and the bg colours
        // describe the artboard tone and surrounding workspace shade —
        // every doc/tab in this project shares them. Old project.json
        // files store these per-doc; load() hoists doc[0]'s values to
        // here when this block is absent. After the first save in this
        // version, the doc-level keys stop being written.
        {"workspace", {
            {"motif",          motif == Motif::Light ? "light" : "dark"},
            {"artboard_dark_r",  artboard_dark_r},
            {"artboard_dark_g",  artboard_dark_g},
            {"artboard_dark_b",  artboard_dark_b},
            {"workspace_dark_r", workspace_dark_r},
            {"workspace_dark_g", workspace_dark_g},
            {"workspace_dark_b", workspace_dark_b},
            {"artboard_light_r",  artboard_light_r},
            {"artboard_light_g",  artboard_light_g},
            {"artboard_light_b",  artboard_light_b},
            {"workspace_light_r", workspace_light_r},
            {"workspace_light_g", workspace_light_g},
            {"workspace_light_b", workspace_light_b},
            // s137 m5: Creation colour pair (per-motif). Missing in
            // pre-m5 project.json files — load() falls back to the
            // struct defaults defined in CurvzProject.hpp.
            {"creation_dark_r",  creation_dark_r},
            {"creation_dark_g",  creation_dark_g},
            {"creation_dark_b",  creation_dark_b},
            {"creation_light_r", creation_light_r},
            {"creation_light_g", creation_light_g},
            {"creation_light_b", creation_light_b}
        }},
        {"editor_state", {
            {"active_doc",          active_file},
            {"zoom",                zoom},
            {"snap_to_pixel",       snap_pixel},
            {"snap_to_grid",        snap_grid},
            {"grid_size",           grid_size},
            {"pane_position",       pane_position},
            {"window_w",            window_w},
            {"window_h",            window_h},
            {"window_maximized",    window_maximized},
            {"sec_preview_open",    sec_preview_open},
            {"sec_layers_open",     sec_layers_open},
            {"sec_library_open",    sec_library_open},
            {"sec_documents_open",  sec_documents_open},
            {"sec_swatches_open",   sec_swatches_open},
            {"sec_styles_open",     sec_styles_open},
            {"sec_themes_open",     sec_themes_open},
            {"sec_content_open",    sec_content_open},
            {"snap_enabled",        snap.enabled},
            {"snap_guides",         snap.snap_guides},
            {"snap_grid",           snap.snap_grid},
            {"snap_margins",        snap.snap_margins},
            {"snap_nodes",          snap.snap_nodes},
            {"snap_edges",          snap.snap_edges},
            {"snap_centers",        snap.snap_centers},
            {"active_palette",      swatches.active_palette()},
            {"recents",             swatches.recents()},
            {"style_active_category",     style_active_category},
            {"style_active_is_app_tier",  style_active_is_app_tier},
            {"library_expanded_categories", library_expanded_categories}
        }}
    };

    // ── Write sibling swatches.json (custom tier only) ────────────────────
    //
    // Defaults are app-global (~/.config/curvz/swatches.json) and never
    // written here. Per-project working state (active_palette, recents)
    // lives in project.json above — this file holds the pure data pool.
    {
        json sw;
        swatches.to_custom_json(sw);
        std::string sw_path = (fs::path(directory) / "swatches.json").string();
        std::ofstream sf(sw_path);
        if (!sf) {
            LOG_ERROR("CurvzProject::save: cannot write '{}'", sw_path);
            return false;
        }
        sf << sw.dump(2) << "\n";
    }

    // ── Styles (S76 m3b — inline in project.json) ─────────────────────────
    //
    // User-tier only. App styles are hardcoded at library construction and
    // never serialised. If the user tier is empty, the 'styles' key is
    // omitted entirely — keeps project.json diffs clean for projects that
    // never used styles. to_user_json always writes a 'styles' array; we
    // lift that out into the top-level object so the schema reads as
    // `{"styles": [...]}` rather than `{"styles": {"styles": [...]}}`.
    if (styles.user_style_count() > 0) {
        json sj;
        styles.to_user_json(sj);
        j["styles"] = std::move(sj["styles"]);
    }

    // ── Themes (S103 m1 — inline in project.json) ─────────────────────────
    //
    // Same shape as styles above: user-tier only (v1 has no app themes
    // anyway), key omitted entirely when the tier is empty so
    // theme-less projects keep clean diffs. to_user_json writes a
    // top-level "themes" array; we lift it out to a top-level "themes"
    // key on j so the load side wraps it back up symmetrically.
    if (themes.user_theme_count() > 0) {
        json tj;
        themes.to_user_json(tj);
        j["themes"] = std::move(tj["themes"]);
    }

    std::string json_path = (fs::path(directory) / "project.json").string();
    std::ofstream f(json_path);
    if (!f) {
        LOG_ERROR("CurvzProject::save: cannot write '{}'", json_path);
        return false;
    }
    f << j.dump(2) << "\n";
    LOG_INFO("CurvzProject::save: saved to '{}' ({} docs)", directory, documents.size());
    return true;
}

// ── seed_library_defaults ────────────────────────────────────────────────────
//
// S69 M2. Called by every construction entry point (create_new, create_empty,
// open) right after `new CurvzProject` so the library's defaults tier is
// populated before any save or load runs. Source of truth is the app-global
// singleton loaded once at startup by Application::on_activate via
// color::load_app_defaults. Copy-by-value per-project (option B).

void CurvzProject::seed_library_defaults() {
    swatches.seed_defaults_from(color::swatch_defaults());
}

// ── load ─────────────────────────────────────────────────────────────────────

bool CurvzProject::load(const std::string& dir) {
    directory = dir;
    documents.clear();
    active_doc_index = 0;

    std::string json_path = (fs::path(dir) / "project.json").string();
    std::ifstream f(json_path);
    if (!f) {
        LOG_ERROR("CurvzProject::load: cannot open '{}'", json_path);
        return false;
    }

    json j;
    try { f >> j; }
    catch (const json::exception& e) {
        LOG_ERROR("CurvzProject::load: JSON parse error: {}", e.what());
        return false;
    }

    // ── Tokens ────────────────────────────────────────────────────────────
    if (j.contains("tokens")) {
        auto& t = j["tokens"];
        tokens.stroke_width  = t.value("stroke_width",  2.0);
        tokens.corner_radius = t.value("corner_radius", 2.0);
    }

    // ── Workspace appearance (s116 m6, expanded m8) ──────────────────────
    // Read project-level workspace block if present. The block carries
    // motif + 12 colour fields (per-motif pairs). Older formats:
    //
    //   Pre-m6: bg lives per-doc, no project workspace block. Per-doc
    //     loop below reads doc[0]'s legacy bg into the dark pair (since
    //     pre-m6 was dark-only) and leaves the light pair at defaults.
    //
    //   m6 (single pair, brief window): the workspace block had merged
    //     artboard_bg_*/workspace_bg_* keys without per-motif split. We
    //     read those keys into the *dark* pair on load and leave light
    //     at defaults — same migration shape as pre-m6.
    //
    // After this version's first save, only the m8 keys are written.
    bool had_project_workspace = false;
    if (j.contains("workspace") && j["workspace"].is_object()) {
        had_project_workspace = true;
        auto& w = j["workspace"];

        // Motif first — sets which pair we'll show on first paint.
        std::string motif_str = w.value("motif", std::string("dark"));
        motif = (motif_str == "light") ? Motif::Light : Motif::Dark;

        // m8 per-motif pairs.
        artboard_dark_r   = w.value("artboard_dark_r",   artboard_dark_r);
        artboard_dark_g   = w.value("artboard_dark_g",   artboard_dark_g);
        artboard_dark_b   = w.value("artboard_dark_b",   artboard_dark_b);
        workspace_dark_r  = w.value("workspace_dark_r",  workspace_dark_r);
        workspace_dark_g  = w.value("workspace_dark_g",  workspace_dark_g);
        workspace_dark_b  = w.value("workspace_dark_b",  workspace_dark_b);
        artboard_light_r  = w.value("artboard_light_r",  artboard_light_r);
        artboard_light_g  = w.value("artboard_light_g",  artboard_light_g);
        artboard_light_b  = w.value("artboard_light_b",  artboard_light_b);
        workspace_light_r = w.value("workspace_light_r", workspace_light_r);
        workspace_light_g = w.value("workspace_light_g", workspace_light_g);
        workspace_light_b = w.value("workspace_light_b", workspace_light_b);

        // s137 m5: Creation colour pair. Pre-m5 project.json files don't
        // carry these — value() falls through to the struct defaults
        // defined in CurvzProject.hpp.
        creation_dark_r   = w.value("creation_dark_r",   creation_dark_r);
        creation_dark_g   = w.value("creation_dark_g",   creation_dark_g);
        creation_dark_b   = w.value("creation_dark_b",   creation_dark_b);
        creation_light_r  = w.value("creation_light_r",  creation_light_r);
        creation_light_g  = w.value("creation_light_g",  creation_light_g);
        creation_light_b  = w.value("creation_light_b",  creation_light_b);

        // m6-format single-pair migration: if the dark pair didn't show
        // up by name but the legacy artboard_bg_*/workspace_bg_* keys
        // are present, hoist them into the dark slots.
        if (!w.contains("artboard_dark_r") && w.contains("artboard_bg_r")) {
            artboard_dark_r  = w.value("artboard_bg_r",  artboard_dark_r);
            artboard_dark_g  = w.value("artboard_bg_g",  artboard_dark_g);
            artboard_dark_b  = w.value("artboard_bg_b",  artboard_dark_b);
            workspace_dark_r = w.value("workspace_bg_r", workspace_dark_r);
            workspace_dark_g = w.value("workspace_bg_g", workspace_dark_g);
            workspace_dark_b = w.value("workspace_bg_b", workspace_dark_b);
        }
    }

    // ── Editor state ──────────────────────────────────────────────────────
    std::string active_file;
    // Per-project swatch working state (active palette + recents). We
    // remember whether these came from editor_state so we know whether
    // the legacy migration path in the color-system section below should
    // overwrite them. A new-format project always writes them here; a
    // legacy project never did, so seeing them missing means either
    // (a) a legacy project — legacy loader will fill them, or (b) a
    // new project that simply hasn't used swatches yet — they stay empty.
    bool editor_state_had_swatch_working_state = false;
    std::string editor_state_active_palette;
    std::vector<std::string> editor_state_recents;
    if (j.contains("editor_state")) {
        auto& es = j["editor_state"];
        zoom        = es.value("zoom",          16.0);
        snap_pixel  = es.value("snap_to_pixel", true);
        snap_grid   = es.value("snap_to_grid",  true);
        grid_size   = es.value("grid_size",     1.0);
        active_file = es.value("active_doc",    std::string{});
        pane_position        = es.value("pane_position",        880);
        window_w             = es.value("window_w",             1400);
        window_h             = es.value("window_h",             860);
        window_maximized     = es.value("window_maximized",     false);
        sec_preview_open     = es.value("sec_preview_open",     false);
        sec_layers_open      = es.value("sec_layers_open",      false);
        sec_library_open     = es.value("sec_library_open",     false);
        sec_documents_open   = es.value("sec_documents_open",   false);
        sec_swatches_open    = es.value("sec_swatches_open",    false);
        sec_styles_open      = es.value("sec_styles_open",      false);
        sec_themes_open      = es.value("sec_themes_open",      false);
        sec_content_open     = es.value("sec_content_open",     false);
        snap.enabled         = es.value("snap_enabled",         true);
        snap.snap_guides     = es.value("snap_guides",          true);
        snap.snap_grid       = es.value("snap_grid",            false);
        snap.snap_margins    = es.value("snap_margins",         false);
        snap.snap_nodes      = es.value("snap_nodes",           true);
        snap.snap_edges      = es.value("snap_edges",           false);
        snap.snap_centers    = es.value("snap_centers",         false);
        // S87: StylesPanel category dropdown state. Empty + false on a
        // legacy project — StylesPanel falls back to picking the first
        // available category, matching the pre-S87 behaviour.
        style_active_category    = es.value("style_active_category",
                                            std::string{});
        style_active_is_app_tier = es.value("style_active_is_app_tier", false);

        // s141: per-project library category expansion list. Absent on
        // legacy projects → empty vector → all categories collapsed,
        // matching the in-memory default behaviour of LibraryPanel.
        if (es.contains("library_expanded_categories") &&
            es["library_expanded_categories"].is_array()) {
            library_expanded_categories.clear();
            for (const auto& v : es["library_expanded_categories"]) {
                if (v.is_string())
                    library_expanded_categories.push_back(v.get<std::string>());
            }
        }
        // S69 M2: per-project swatch working state moved out of the old
        // color_system block into editor_state. Absence = legacy project
        // (or a new one that never touched swatches).
        if (es.contains("active_palette") || es.contains("recents")) {
            editor_state_had_swatch_working_state = true;
            editor_state_active_palette = es.value("active_palette", std::string{});
            if (es.contains("recents") && es["recents"].is_array()) {
                for (const auto& sid : es["recents"]) {
                    if (sid.is_string()) {
                        editor_state_recents.push_back(sid.get<std::string>());
                    }
                }
            }
        }
    }

    // ── Swatches (S69 M2 two-tier layout) ────────────────────────────────
    //
    // Precedence:
    //   1. Sibling swatches.json exists → from_custom_json; working state
    //      comes from editor_state (already captured above).
    //   2. Legacy color_system block in project.json → load_legacy_color_system
    //      which populates BOTH the custom tier pool and the working state
    //      fields on the library directly. On next save this project
    //      migrates to the new layout.
    //   3. Neither → library keeps whatever defaults were seeded by the
    //      caller via seed_library_defaults() and is otherwise empty.
    //
    // The defaults tier should already be populated by the caller (open/
    // create_*) before load() runs — we don't touch it here.
    std::string sw_path = (fs::path(dir) / "swatches.json").string();
    std::error_code sw_ec;
    if (fs::exists(sw_path, sw_ec)) {
        std::ifstream sf(sw_path);
        if (sf) {
            try {
                json sj;
                sf >> sj;
                swatches.from_custom_json(sj);
            } catch (const json::exception& e) {
                LOG_WARN("CurvzProject::load: swatches.json parse error: {}",
                         e.what());
            }
        } else {
            LOG_WARN("CurvzProject::load: cannot open '{}'", sw_path);
        }
        // Apply working state from editor_state. Filter to ids that
        // actually resolve in either tier of the library — matches the
        // defensive behaviour of legacy load.
        if (editor_state_had_swatch_working_state) {
            // touch_recent inserts at the FRONT (MRU semantics), so to
            // reconstruct editor_state's order we push in reverse — the
            // first id in the saved array ends up at the front of the
            // queue again.
            swatches.clear_recents();
            for (auto it = editor_state_recents.rbegin();
                 it != editor_state_recents.rend(); ++it) {
                swatches.touch_recent(*it);  // no-ops for unknown ids
            }
            swatches.set_active_palette(editor_state_active_palette);
        }
    } else if (j.contains("color_system")) {
        LOG_INFO("CurvzProject::load: legacy color_system block present — "
                 "migrating to swatches.json on next save");
        swatches.load_legacy_color_system(j["color_system"]);
    }

    // ── Styles (S76 m3b — inline in project.json) ────────────────────────
    //
    // User tier only. App styles are re-seeded every launch by the
    // StyleLibrary constructor; we never touch the app tier here. The
    // save side lifts the array up to a top-level "styles" key for a
    // natural project.json shape; we wrap it back up before calling
    // from_user_json (which expects {"styles": [...]}).
    if (j.contains("styles") && j["styles"].is_array()) {
        json wrapped;
        wrapped["styles"] = j["styles"];
        styles.from_user_json(wrapped);
    }

    // ── Themes (S103 m1 — inline in project.json) ────────────────────────
    //
    // Same shape as styles above. v1 has no app themes; user tier is the
    // only thing in play. Older project.json files written before S103
    // simply lack the "themes" key — additive schema change, no
    // migration. from_user_json clears the user tier first, so reloading
    // a project after deleting all themes correctly empties the library.
    if (j.contains("themes") && j["themes"].is_array()) {
        json wrapped;
        wrapped["themes"] = j["themes"];
        themes.from_user_json(wrapped);
    }

    // ── Documents ─────────────────────────────────────────────────────────
    if (!j.contains("documents")) {
        LOG_ERROR("CurvzProject::load: no documents array");
        return false;
    }

    int idx = 0;
    // s116 m6: when had_project_workspace==false, we hoist doc[0]'s legacy
    // bg fields and (if present) its m4-only "motif" key to the project on
    // first save. legacy_doc0_motif captures doc[0]'s motif string so the
    // hoist sees it — bg fields are read into doc->artboard_bg_* directly
    // and read off doc[0] after the loop.
    std::string legacy_doc0_motif;
    // s148 m1: parallel to documents[], records whether each doc's JSON
    // entry carried per-doc artboard_bg_* keys at load time. Used by the
    // s116→s148 post-loop seed step to fill doc fields from the
    // project's active-motif slot for files that pre-date the doc-scope
    // promotion. Reserved up front to keep parallel indexing reliable.
    std::vector<bool> migration_doc_had_bg;
    migration_doc_had_bg.reserve(j["documents"].size());
    for (auto& entry : j["documents"]) {
        std::string fname = entry.value("file", "");
        if (fname.empty()) continue;

        std::string svg_path = (fs::path(dir) / fname).string();
        auto doc = parse_svg_file(svg_path);
        if (!doc) {
            // File missing — create a blank document stub
            LOG_WARN("CurvzProject::load: '{}' missing, creating blank", svg_path);
            doc = std::make_unique<CurvzDocument>();
        }
        doc->filename = fname;
        // New canvas model
        if (entry.contains("canvas")) {
            const auto& c = entry["canvas"];
            CanvasModel cm;
            cm.ratio_w      = c.value("ratio_w",      1.0);
            cm.ratio_h      = c.value("ratio_h",      1.0);
            cm.quality      = c.value("quality",      24);
            cm.display_mode = (DisplayMode)c.value("display_mode", (int)DisplayMode::Pixel);
            cm.phys_width   = c.value("phys_width",   1.0);
            cm.phys_height  = c.value("phys_height",  1.0);
            cm.phys_unit    = c.value("phys_unit",    std::string("in"));
            cm.dpi          = c.value("dpi",          300);
            cm.px_width     = c.value("px_width",     24);
            cm.px_height    = c.value("px_height",    24);
            cm.display_unit = UnitSystem::parse_unit(c.value("display_unit", std::string{"px"}));
            // s292 m1: render-intent fields. Two paths to populate:
            //   1. JSON has them (post-s292 project files) -- read direct.
            //   2. JSON doesn't (pre-s292 legacy) -- fall back to whatever
            //      the SVG parser set on doc->canvas before we got here.
            //      The SVG file itself carries data-curvz-intended-* attrs
            //      since s264, so a legacy .curvz wrapping a post-s264 SVG
            //      still round-trips correctly.
            // Defaults to the doc->canvas (parser-set) value so the fallback
            // is the default behavior when JSON keys are absent.
            cm.intended_w    = c.value("intended_w",    doc->canvas.intended_w);
            cm.intended_h    = c.value("intended_h",    doc->canvas.intended_h);
            cm.intended_unit = c.value("intended_unit", doc->canvas.intended_unit);
            // Migrate: older builds saved pixel docs with display_mode=RatioQuality
            // and left px_width/height at the default (24). Detect by ratio==1:1
            // (pixel docs always normalise to 1:1) and fix px_width/height from quality.
            if (cm.display_mode == DisplayMode::RatioQuality &&
                std::abs(cm.ratio_w - 1.0) < 1e-6 &&
                std::abs(cm.ratio_h - 1.0) < 1e-6) {
                cm.px_width     = cm.quality;
                cm.px_height    = cm.quality;
                cm.display_mode = DisplayMode::Pixel;
                LOG_INFO("CurvzProject: migrated '{}' RatioQuality→Pixel ({}px)", fname, cm.quality);
            }
            doc->canvas     = cm;
            doc->ruler_origin_x = c.value("ruler_origin_x", 0.0);
            doc->ruler_origin_y = c.value("ruler_origin_y", 0.0);
            // Migrate: if origin_y equals canvas_height it was set by a bad earlier build — reset to 0
            if (doc->ruler_origin_y == (double)doc->canvas_height())
                doc->ruler_origin_y = 0.0;
        } else {
            // Backwards compat: legacy canvas_w / canvas_h integers
            int cw = entry.value("canvas_w", 24);
            int ch = entry.value("canvas_h", 24);
            doc->canvas = CanvasModel::from_legacy(cw, ch);
            doc->ruler_origin_y = 0.0;
        }

        doc->export_name     = entry.value("export_name",     std::string{});
        doc->export_category = entry.value("export_category", std::string{});

        // ── Per-doc editor-presentation tone load ─────────────────────
        //
        // s148 m1 promotes artboard/workspace/creation back to per-doc
        // (single slot). Three input scenarios:
        //
        //   • Pre-s116 file: per-doc keys present, no project workspace
        //     block — keys read directly into doc fields (line below).
        //     creation_color_* absent → falls through to struct defaults.
        //
        //   • s116-s147 file: doc-level keys absent, project root carries
        //     per-motif slots. We detect the absence here (had_doc_bg=
        //     false), then post-loop seed doc fields from project's
        //     active-motif slot.
        //
        //   • s148+ file: per-doc keys present (including creation_color)
        //     → read directly. Project root may also carry the legacy
        //     per-motif block as a downgrade-safety net but those slots
        //     are no longer canonical for paint.
        // s183 m5a — dual-pair motif storage. Old single-slot keys
        // (artboard_bg_*, workspace_bg_*, creation_color_*) are not
        // read; old files load with default mode pairs and the user
        // re-applies a theme to recover their custom colours.
        const bool had_doc_bg =
            entry.contains("dark_artboard_bg_r") ||
            entry.contains("light_artboard_bg_r");
        doc->dark_artboard_bg_r  = entry.value("dark_artboard_bg_r",  doc->dark_artboard_bg_r);
        doc->dark_artboard_bg_g  = entry.value("dark_artboard_bg_g",  doc->dark_artboard_bg_g);
        doc->dark_artboard_bg_b  = entry.value("dark_artboard_bg_b",  doc->dark_artboard_bg_b);
        doc->dark_workspace_bg_r = entry.value("dark_workspace_bg_r", doc->dark_workspace_bg_r);
        doc->dark_workspace_bg_g = entry.value("dark_workspace_bg_g", doc->dark_workspace_bg_g);
        doc->dark_workspace_bg_b = entry.value("dark_workspace_bg_b", doc->dark_workspace_bg_b);
        doc->dark_creation_color_r = entry.value("dark_creation_color_r", doc->dark_creation_color_r);
        doc->dark_creation_color_g = entry.value("dark_creation_color_g", doc->dark_creation_color_g);
        doc->dark_creation_color_b = entry.value("dark_creation_color_b", doc->dark_creation_color_b);
        doc->light_artboard_bg_r  = entry.value("light_artboard_bg_r",  doc->light_artboard_bg_r);
        doc->light_artboard_bg_g  = entry.value("light_artboard_bg_g",  doc->light_artboard_bg_g);
        doc->light_artboard_bg_b  = entry.value("light_artboard_bg_b",  doc->light_artboard_bg_b);
        doc->light_workspace_bg_r = entry.value("light_workspace_bg_r", doc->light_workspace_bg_r);
        doc->light_workspace_bg_g = entry.value("light_workspace_bg_g", doc->light_workspace_bg_g);
        doc->light_workspace_bg_b = entry.value("light_workspace_bg_b", doc->light_workspace_bg_b);
        doc->light_creation_color_r = entry.value("light_creation_color_r", doc->light_creation_color_r);
        doc->light_creation_color_g = entry.value("light_creation_color_g", doc->light_creation_color_g);
        doc->light_creation_color_b = entry.value("light_creation_color_b", doc->light_creation_color_b);
        // Track per-doc whether the bg key set was present, for the
        // post-loop s116→s148 seed step. Stored on the bool vector
        // declared just above the doc loop (see migration_doc_had_bg).
        migration_doc_had_bg.push_back(had_doc_bg);
        // Pre-m6 m4-only files briefly stored a per-doc "motif" key. If
        // the project root has no "workspace" block but a doc carried
        // a motif key, hoist that one too (handled below post-loop).
        if (idx == 0)
            legacy_doc0_motif = entry.value("motif", std::string());

        // S98: guide colour. Defaults to CurvzDocument's cyan when the
        // key is absent — pre-S98 projects open visually identical.
        doc->guide_color_r  = entry.value("guide_color_r",  doc->guide_color_r);
        doc->guide_color_g  = entry.value("guide_color_g",  doc->guide_color_g);
        doc->guide_color_b  = entry.value("guide_color_b",  doc->guide_color_b);

        // Load grid layer if saved
        if (entry.contains("grid") && !entry["grid"].is_null()) {
            const auto& g = entry["grid"];
            SceneNode* gl = doc->ensure_grid_layer();
            gl->visible        = g.value("visible",   true);
            gl->locked         = g.value("locked",    false);
            gl->grid_spacing_x = g.value("spacing_x", 100.0);
            gl->grid_spacing_y = g.value("spacing_y", 100.0);
            gl->grid_offset_x  = g.value("offset_x",  0.0);
            gl->grid_offset_y  = g.value("offset_y",  0.0);
            gl->grid_color_r   = g.value("color_r",   0.5);
            gl->grid_color_g   = g.value("color_g",   0.5);
            gl->grid_color_b   = g.value("color_b",   0.8);
            gl->grid_color_a   = g.value("color_a",   0.35);
            gl->grid_dots      = g.value("dots",       false);
            gl->grid_snap      = g.value("snap",       false);
        }

        // Load margin layer if saved
        if (entry.contains("margins") && !entry["margins"].is_null()) {
            const auto& m = entry["margins"];
            SceneNode* ml = doc->ensure_margin_layer();
            ml->visible        = m.value("visible",  true);
            ml->locked         = m.value("locked",   false);
            ml->margin_top     = m.value("top",      0.0);
            ml->margin_bottom  = m.value("bottom",   0.0);
            ml->margin_left    = m.value("left",     0.0);
            ml->margin_right   = m.value("right",    0.0);
            ml->margin_columns = m.value("columns",  1);
            ml->margin_col_gap = m.value("col_gap",  0.0);
            ml->margin_rows    = m.value("rows",     1);
            ml->margin_row_gap = m.value("row_gap",  0.0);
            ml->margin_color_r = m.value("color_r",  0.8);
            ml->margin_color_g = m.value("color_g",  0.3);
            ml->margin_color_b = m.value("color_b",  0.3);
            ml->margin_color_a = m.value("color_a",  0.15);
        }

        if (!active_file.empty() && fname == active_file)
            active_doc_index = idx;

        documents.push_back(std::move(doc));
        ++idx;
    }

    if (documents.empty())
        LOG_INFO("CurvzProject::load: loaded '{}' with empty gallery", dir);

    // ── s183 m5a: prior-format migration removed ──────────────────────
    //
    // Pre-s116 hoist (doc[0]'s single-slot bg → project dark workspace)
    // and the s148 m1 seed step (project per-motif → per-doc single
    // slot) were both stripped here. Both wrote to the old per-doc
    // single-slot fields (artboard_bg_r/_g/_b etc.) which no longer
    // exist — docs now carry dual pairs. Old project.json files
    // missing the new dual-pair keys load with factory-default colour
    // pairs in both modes; per Scott "they can just use a default."
    // The user re-applies a theme (or hand-picks colours on the
    // Document ▸ Canvas section) to recover their preferred look.
    (void)had_project_workspace;
    (void)legacy_doc0_motif;
    (void)migration_doc_had_bg;

    // ── Post-load style re-materialisation (S76 m3b) ─────────────────────
    //
    // Every SceneNode with a non-empty bound_style gets its fill/stroke
    // re-projected from the referenced style. bound_style itself is not
    // yet persisted on the SceneNode (that's m3c — SVG round-trip of
    // data-curvz-style), so on a cold load there are typically no
    // bindings to restore yet; this walk costs effectively nothing until
    // m3c lands. Wiring it in now means m3c doesn't have to revisit the
    // load sequence.
    for (auto& doc : documents) {
        for (auto& layer : doc->layers) {
            style::walk_style_bindings(layer.get(),
                [this](SceneNode& n) {
                    materialise_or_drop_binding(n, styles);
                });
        }
    }

    LOG_INFO("CurvzProject::load: loaded '{}' — {} docs, active={}",
             dir, documents.size(), active_doc_index);
    return true;
}

// ── create_new ───────────────────────────────────────────────────────────────

std::unique_ptr<CurvzProject> CurvzProject::create_new(const std::string& dir) {
    return create_new(dir, CanvasModel::from_pixels(24, 24));
}

std::unique_ptr<CurvzProject> CurvzProject::create_new(const std::string& dir,
                                                        const CanvasModel& canvas) {
    return create_new(dir, canvas, "icon");
}

std::unique_ptr<CurvzProject> CurvzProject::create_new(const std::string& dir,
                                                        const CanvasModel& canvas,
                                                        const std::string& doc_name) {
    auto p = std::make_unique<CurvzProject>();
    p->seed_library_defaults();  // S69 M2 — before save writes swatches.json
    p->directory = dir;

    // Sanitise doc_name → safe filename
    std::string base = doc_name.empty() ? "icon" : doc_name;
    for (char& c : base) if (c == ' ') c = '-';

    auto doc = std::make_unique<CurvzDocument>();
    doc->canvas   = canvas;
    doc->filename = base + ".svg";
    p->documents.push_back(std::move(doc));
    p->active_doc_index = 0;

    if (!p->save()) return nullptr;
    LOG_INFO("CurvzProject::create_new: '{}' canvas {}x{} doc '{}'",
             dir, canvas.canvas_width(), canvas.canvas_height(), base + ".svg");
    return p;
}

// Seed-doc overload: the caller (typically NewDocumentDialog) hands over a
// fully-formed CurvzDocument (from a template or a synthetic built-in).
// We take ownership, sanitise the filename from doc_name, and save.
std::unique_ptr<CurvzProject> CurvzProject::create_new(const std::string& dir,
                                                        std::unique_ptr<CurvzDocument> seed,
                                                        const std::string& doc_name) {    if (!seed) {
        LOG_ERROR("CurvzProject::create_new(seed): null seed");
        return nullptr;
    }
    auto p = std::make_unique<CurvzProject>();
    p->seed_library_defaults();  // S69 M2 — before save writes swatches.json
    p->directory = dir;

    std::string base = doc_name.empty() ? "icon" : doc_name;
    for (char& c : base) if (c == ' ') c = '-';

    seed->filename = base + ".svg";
    p->documents.push_back(std::move(seed));
    p->active_doc_index = 0;

    if (!p->save()) return nullptr;
    LOG_INFO("CurvzProject::create_new(seed): '{}' doc '{}'", dir, base + ".svg");
    return p;
}

// Empty-gallery overload: produces a project with no documents. The caller
// (File → New Project) is responsible for populating documents through the
// "Add to Project" flow afterwards.
std::unique_ptr<CurvzProject> CurvzProject::create_empty(const std::string& dir) {
    auto p = std::make_unique<CurvzProject>();
    p->seed_library_defaults();  // S69 M2 — before save writes swatches.json
    p->directory = dir;
    p->active_doc_index = 0;
    if (!p->save()) return nullptr;
    LOG_INFO("CurvzProject::create_empty: '{}' (no documents)", dir);
    return p;
}

// ── open ─────────────────────────────────────────────────────────────────────

std::unique_ptr<CurvzProject> CurvzProject::open(const std::string& dir) {
    auto p = std::make_unique<CurvzProject>();
    p->seed_library_defaults();  // S69 M2 — before load reads swatches.json
    if (!p->load(dir)) return nullptr;
    return p;
}

} // namespace Curvz
