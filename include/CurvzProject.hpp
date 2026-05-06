#pragma once
#include "CurvzDocument.hpp"
#include "color/SwatchLibrary.hpp"
#include "style/StyleLibrary.hpp"
#include "theme/ThemeLibrary.hpp"
#include <string>
#include <vector>
#include <memory>

namespace Curvz {

struct DesignTokens {
    double stroke_width  = 2.0;
    double corner_radius = 2.0;
};

struct CurvzProject {
    std::string                                 directory;  // path to .curvz dir
    std::vector<std::unique_ptr<CurvzDocument>> documents;
    DesignTokens                                tokens;
    int                                         active_doc_index = 0;
    double                                      zoom       = 16.0;
    bool                                        snap_pixel = true;
    bool                                        snap_grid  = true;
    double                                      grid_size  = 1.0;

    // Inspector / UI state
    int  pane_position          = 880;  // h_paned divider position
    int  window_w               = 1400; // last window width
    int  window_h               = 860;  // last window height
    bool window_maximized       = false;
    bool sec_preview_open       = false;
    bool sec_layers_open        = false;
    bool sec_library_open       = false;
    bool sec_documents_open     = false;
    bool sec_swatches_open      = false;
    bool sec_styles_open        = false;
    bool sec_themes_open        = false;  // s147 m3 — Themes panel
    bool sec_content_open       = false;  // Content group (Layers/Library/Swatches/Styles/Themes/Documents)

    // s141: per-project list of expanded library category keys. Sparse —
    // only categories the user has explicitly expanded this session are
    // listed; everything else is collapsed. Keys are "sys:<name>" or
    // "usr:<name>" to disambiguate same-named system and user categories.
    // LibraryPanel reads this on set_project() to populate m_expanded
    // and writes it back via signal_request_save when a category toggles.
    std::vector<std::string> library_expanded_categories;

    // S87 — StylesPanel header dropdown selection. Two-field state
    // because the dropdown can sit on either an app-tier category
    // ("Built-in") or a user-tier category ("My Styles", "Brand", …);
    // string alone wouldn't disambiguate. Empty string with
    // is_app_tier=false maps to the (uncategorised) bucket. Defaults
    // (empty + false) match StylesPanel's first-launch behaviour where
    // it picks the first available category itself.
    std::string style_active_category;
    bool        style_active_is_app_tier = false;

    // App-wide snap settings (shared across all documents)
    SnapSettings snap;

    // ── Workspace appearance (project-wide, per-motif) ────────────────────
    // s116 m6: motif and the artboard/workspace background colours moved
    // up from CurvzDocument. They describe the visual environment the
    // user works in for this project.
    //
    // s116 m8: split the bg colours into per-motif pairs. A user who
    // works in Dark mode sets their dark-mode artboard tone; flipping
    // to Light shows the light-mode artboard tone (which the user can
    // independently customise). Without this split, switching motif
    // would leave a dark-grey artboard floating on a light chrome.
    //
    // Defaults are neutral greys in both motifs — Curvz isn't a "paper"
    // editor, so light mode doesn't assume white. Designers get a
    // working surface, not a printed page.
    //
    // Persisted at the project root in project.json's "workspace"
    // block. Old project.json files (pre-m6) store a single bg pair
    // per-doc — load() hoists doc[0]'s values into the *dark* slots
    // (since pre-m6 was dark-only) and seeds the light slots with
    // defaults.
    Motif  motif = Motif::Dark;

    // Dark-motif pair (#282828 artboard on #171717 workspace).
    double artboard_dark_r  = 0.157;
    double artboard_dark_g  = 0.157;
    double artboard_dark_b  = 0.157;
    double workspace_dark_r = 0.09;
    double workspace_dark_g = 0.09;
    double workspace_dark_b = 0.09;

    // Light-motif pair (#e0e0e0 artboard on #c0c0c0 workspace).
    // Neutral medium-light greys — designed working surface, not a
    // paper-white sheet. User re-picks freely via the Project → Motif
    // panel; defaults just have to be sensible-looking on first launch.
    double artboard_light_r  = 0.878;   // #e0e0e0
    double artboard_light_g  = 0.878;
    double artboard_light_b  = 0.878;
    double workspace_light_r = 0.753;   // #c0c0c0
    double workspace_light_g = 0.753;
    double workspace_light_b = 0.753;

    // ── Creation colour (s137 m5) ─────────────────────────────────────
    // Used everywhere the user is *creating* something but hasn't yet
    // committed it: rect/ellipse/line/polygon/spiral construction
    // outlines and fills, pen tool segments and rubber-band, pen handle
    // levers (lightened-derived from this value).
    //
    // Selection-time UI (selected-object outline, marquee, resize handles)
    // is a sibling concept and uses its own hardcoded blue today; a future
    // "Selection colour" setting may split it out further.
    //
    // Defaults are calibrated for contrast against each motif's artboard:
    //   Dark motif  → light blue (reads bright against #282828 artboard)
    //   Light motif → dark navy (reads dark against #e0e0e0 artboard)
    // Both are within the same blue family for visual consistency with
    // Curvz's accent vocabulary, but luminance flips with motif.
    //
    // r/g/b only — alphas at each draw site stay hardcoded because they
    // encode role within creation (fill = dim, outline = bold, rubber-
    // band = tentative). Letting users override alphas would turn every
    // tool's preview into a designer's job.
    double creation_dark_r  = 0.30;   // current cyan-blue, reads on dark
    double creation_dark_g  = 0.60;
    double creation_dark_b  = 1.00;
    double creation_light_r = 0.05;   // deep navy, reads on light
    double creation_light_g = 0.25;
    double creation_light_b = 0.55;

    // ── Active-motif accessors ────────────────────────────────────────
    // Return the rgb triple matching the project's current motif.
    // Callers that paint or read the bg should use these instead of
    // selecting a pair manually — central seam means the motif-pair
    // mapping is in one place.
    double artboard_r() const { return motif == Motif::Light ? artboard_light_r : artboard_dark_r; }
    double artboard_g() const { return motif == Motif::Light ? artboard_light_g : artboard_dark_g; }
    double artboard_b() const { return motif == Motif::Light ? artboard_light_b : artboard_dark_b; }
    double workspace_r() const { return motif == Motif::Light ? workspace_light_r : workspace_dark_r; }
    double workspace_g() const { return motif == Motif::Light ? workspace_light_g : workspace_dark_g; }
    double workspace_b() const { return motif == Motif::Light ? workspace_light_b : workspace_dark_b; }
    double creation_r()  const { return motif == Motif::Light ? creation_light_r  : creation_dark_r;  }
    double creation_g()  const { return motif == Motif::Light ? creation_light_g  : creation_dark_g;  }
    double creation_b()  const { return motif == Motif::Light ? creation_light_b  : creation_dark_b;  }

    // Project-wide color system (Phase 1 / M4). See ARCHITECTURE.md
    // "Color System". Empty in v1 — types exist, no UI wired yet, not
    // serialized. Later phases (4–9) grow CRUD, reverse usage index,
    // save/load, and SVG round-trip.
    color::SwatchLibrary swatches;

    // Project-wide style system (Style System Phase 1). See
    // ARCHITECTURE.md "Style System". Sibling of `swatches` above. The
    // library is constructed with the hardcoded app-style stub list
    // pre-populated; user styles persist in project.json (m3b). No UI
    // wired yet — the StylesPanel lands in Phase 2 (S76+).
    style::StyleLibrary styles;

    // Project-wide theme system (S103 m1). Sibling of `styles`. A theme
    // is a named bundle of doc-level "non-physical" settings (units,
    // background, guides, grid, margins, snap) that the user can apply
    // across docs in this project, save as a JSON pack, and reuse in
    // other projects. v1 has no built-in (app:) themes — only user
    // themes, prefixed thm_<uuid>. Persisted inline in project.json
    // alongside the styles array.
    theme::ThemeLibrary themes;

    CurvzDocument* active_doc() {
        if (documents.empty()) return nullptr;
        if (active_doc_index < 0 || active_doc_index >= (int)documents.size())
            active_doc_index = 0;
        return documents[active_doc_index].get();
    }

    // ── Workspace appearance helpers ──────────────────────────────────
    // Reset one motif's artboard + workspace colour pair to the struct
    // defaults. Used by the Project → Motif "Reset" button. Scoped to a
    // single motif (the active one, by caller convention) so flipping
    // motifs and resetting one doesn't clobber the other. Idempotent.
    //
    // The default values live in this file's field initialisers — this
    // method is the single source of truth for "what does reset
    // restore to," so changing a default here automatically changes the
    // reset behaviour. Avoids the easy bug of editing one and forgetting
    // the other.
    void reset_motif_bg_to_defaults(Motif which) {
        if (which == Motif::Light) {
            artboard_light_r  = 0.878;  // #e0e0e0
            artboard_light_g  = 0.878;
            artboard_light_b  = 0.878;
            workspace_light_r = 0.753;  // #c0c0c0
            workspace_light_g = 0.753;
            workspace_light_b = 0.753;
            // Creation: deep navy that reads against light artboard.
            creation_light_r  = 0.05;
            creation_light_g  = 0.25;
            creation_light_b  = 0.55;
        } else {
            artboard_dark_r  = 0.157;   // #282828
            artboard_dark_g  = 0.157;
            artboard_dark_b  = 0.157;
            workspace_dark_r = 0.09;    // #171717
            workspace_dark_g = 0.09;
            workspace_dark_b = 0.09;
            // Creation: light blue that reads against dark artboard.
            creation_dark_r  = 0.30;
            creation_dark_g  = 0.60;
            creation_dark_b  = 1.00;
        }
    }

    // ── Persistence ───────────────────────────────────────────────────────
    // Save all SVGs + project.json to directory. Returns true on success.
    bool save() const;

    // Load from a .curvz directory. Returns true on success.
    bool load(const std::string& dir);

    // Seed this project's SwatchLibrary defaults tier from the app-global
    // defaults singleton (color::swatch_defaults). Called by every create_*
    // and open() entry point right after construction, before any save or
    // load touches the library. Idempotent — replaces the defaults pool
    // with whatever's currently in the singleton.
    //
    // Per s69: option B for defaults ownership. Each project's library
    // holds its own copy of the defaults pool; the singleton is the
    // source of truth at load time, but isn't consulted for reads during
    // the project's lifetime. Small pool (~50 entries post-1.0), cheap
    // duplication, no shared-pointer lifetime questions.
    void seed_library_defaults();

    // Create a fresh project at the given directory path.
    // Creates the directory if needed, adds one default document.
    static std::unique_ptr<CurvzProject> create_new(const std::string& dir);
    static std::unique_ptr<CurvzProject> create_new(const std::string& dir, const CanvasModel& canvas);
    static std::unique_ptr<CurvzProject> create_new(const std::string& dir, const CanvasModel& canvas, const std::string& doc_name);
    // Create a fresh project from a caller-supplied seed document (from a
    // template or builder). Takes ownership of the seed. The seed's
    // filename is derived from doc_name.
    static std::unique_ptr<CurvzProject> create_new(const std::string& dir,
                                                    std::unique_ptr<CurvzDocument> seed,
                                                    const std::string& doc_name);

    // Create a fresh project with an empty document gallery. The caller is
    // expected to populate documents via the "Add to Project" UI path.
    // This is the model used by File → New Project.
    static std::unique_ptr<CurvzProject> create_empty(const std::string& dir);

    // Open an existing .curvz directory.
    static std::unique_ptr<CurvzProject> open(const std::string& dir);
};

} // namespace Curvz
