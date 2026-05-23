#include "MainWindow.hpp"
#include "AppPreferences.hpp" // s139 m2 / s143 m1 — boolean-cleanup quality pref + sync
#include "Application.hpp" // s219 m1 — Curvz::Application (main_window only)
#include "ContextBar.hpp"
#include "CoordSpace.hpp"
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"
#include "DocTabBar.hpp"
#include "RecentProjects.hpp" // s144 m3 — Open Recent submenu
#include "Ruler.hpp"
#include "SvgOptimiser.hpp"
#include "SvgParser.hpp"
#include "SvgWriter.hpp"
#include "TemplateLibrary.hpp"
#include "scripting/Action.hpp"        // s254 m2 — Tier 2 action wrappers
#include "scripting/AppScriptable.hpp" // s263 m2 — third headless-verb singleton
#include "scripting/ExportScriptable.hpp" // s251 m1 — second headless-verb singleton
#include "scripting/GuidesScriptable.hpp" // s218 m1 — second model Scriptable
#include "scripting/InspectorScriptable.hpp" // s222 m2 — inspector area Scriptable
#include "scripting/LayersScriptable.hpp"    // s216 m1 — model Scriptable pilot
#include "scripting/ObjectsScriptable.hpp"  // s230 m1 — sixth model Scriptable
#include "scripting/PalettesScriptable.hpp" // s243 m2 — eighth model Scriptable
#include "scripting/ProjScriptable.hpp" // s246 m1 — first headless-verb singleton
#include "scripting/ScripterWindow.hpp" // s219 m1 — apply_scripter_pref present/hide
#include "scripting/StylesScriptable.hpp"   // s222 m1 — fourth model Scriptable
#include "scripting/SwatchesScriptable.hpp" // s221 m1 — third model Scriptable
#include "scripting/ThemesScriptable.hpp"   // s223 m1 — fifth model Scriptable
#include "widgets/ToggleButton.hpp" // s219 m1 — m_scripter_btn visibility flip
#include <functional>
#include <giomm/simpleactiongroup.h> // s144 m3 — recents action group
#include <gtkmm/application.h>
#include <gtkmm/separator.h>
// s147 m3: ThemesDialog include removed — surface is now ThemesPanel
// (already pulled in via MainWindow.hpp). The dialog source files
// remain in the tree until Scott deletes them on his end; CMake no
// longer references them in this milestone.
#include "UnitSystem.hpp"
#include "curvz_utils.hpp" // s117 m18 v2: apply_motif_class_from_parent
#include "style/StyleInterop.hpp" // mutate_appearance — inspector-driven appearance edits
#include <algorithm>
#include <cctype> // s136 m4: std::isspace for library item name trim
#include <filesystem>
#include <fstream>
#include <giomm/file.h> // s125 m1a: Gio::File::create_for_path (folder picker)
#include <giomm/liststore.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <glibmm/main.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/image.h>      // s117 m19: custom About dialog hero logo
#include <gtkmm/linkbutton.h> // s136 m1: About dialog outbound links
#include <gtkmm/settings.h>
#include <gtkmm/stack.h> // s117 m19: custom About dialog flip animation
#include <gtkmm/stylecontext.h>
#include <nlohmann/json.hpp>

// GDK key definitions
#include <gdk/gdkkeysyms.h>

namespace Curvz {

namespace fs = std::filesystem;

// ── Scripts paths (s267 m1) ────────────────────────────────────────────
// User-scripts directory: AppPreferences::scripts_path_override when
// set, else ~/.config/curvz/scripts (mirror of TemplateLibrary's user_dir
// shape — same convention as templates and macros). The dir is created
// on first read so the Scripter never has to deal with a missing
// folder; the rescan path tolerates "exists but empty" cleanly already.
//
// System-scripts directory: /usr/share/curvz/scripts. Hard-coded to
// match templates' system_dir convention. CMake's install() rule
// (s267 m2) populates this from resources/scripts/. Flatpak inherits
// via the same /app prefix bind as templates does.
//
// Lives in MainWindow.cpp rather than its own TU because (a) it's
// two ten-line functions and (b) MainWindow is the only call site —
// the Scripter receives the resolved paths via its constructor, not
// by calling these helpers itself.
std::string scripts_user_dir() {
  const std::string &override_path =
      AppPreferences::instance().scripts_path_override();
  std::string dir =
      !override_path.empty()
          ? override_path
          : (std::string(Glib::get_user_config_dir()) + "/curvz/scripts");
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    LOG_WARN("scripts_user_dir: cannot create '{}': {}", dir, ec.message());
  }
  return dir;
}

std::string scripts_system_dir() { return "/usr/share/curvz/scripts"; }

// ─────────────────────────────────────────────────────────────────────
// MainWindow.cpp — the glue.
//
// MainWindow's implementation is split across multiple TUs by category:
// see the top of MainWindow.hpp for the index. This file holds:
//   • the constructor (orchestrates zone/binding setup)
//   • setup_project (project-state initialisation; precedes zone setup)
//   • on_tool_changed, on_doc_activated, cycle_doc — cross-cutting
//     orchestrators that touch zones, panels, canvas, and actions in
//     ways that don't fit any one domain handler file
//   • rename_doc — called from inspector and gallery, sits at the
//     document seam but isn't a pure UI handler
//   • the file-static doc_stem_from_name HAS BEEN HOISTED into
//     curvz::utils (used from connect_signals + on_new + binding
//     slot bodies).
// ─────────────────────────────────────────────────────────────────────

#include "css.hpp"

MainWindow::MainWindow(Application & /*app*/)
    : m_corner_radius_spin(SpinType::Distance) {
  curvz::utils::set_name(*this, "mw", "main_window_root");
  set_default_size(1400, 860);

  // ── CSS loading ────────────────────────────────────────────────────────
  // Two-stage load with GTK priority-based cascade:
  //   1. Built-in defaults  → priority APPLICATION  (ships with the binary)
  //   2. User stylesheet    → priority USER         (user-editable overrides)
  // GTK's cascade handles merging: user rules win where they match, defaults
  // cover everything else.  If the user deletes their file the app still
  // renders correctly from defaults alone.
  auto css = Gtk::CssProvider::create();
  css->load_from_data(CURVZ_CSS);
  Gtk::StyleContext::add_provider_for_display(
      get_display(), css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  // User stylesheet — default ~/.config/curvz/styles.css, or the
  // user's custom_css_path_override if set (s145 m4). When the
  // override is in effect we skip the dir-create and stub-seed
  // steps: the user has explicitly pointed at a file they own,
  // so we read it if present and silently fall through if not.
  const std::string &css_override =
      AppPreferences::instance().custom_css_path_override();
  const bool css_is_override = !css_override.empty();

  fs::path user_css_dir = fs::path(Glib::get_user_config_dir()) / "curvz";
  fs::path user_css_path =
      css_is_override ? fs::path(css_override) : (user_css_dir / "styles.css");
  std::error_code ec;
  if (!css_is_override) {
    fs::create_directories(user_css_dir, ec);
  }
  if (!css_is_override && ec) {
    LOG_WARN("User CSS: cannot create '{}': {}", user_css_dir.string(),
             ec.message());
  } else if (!css_is_override && !fs::exists(user_css_path)) {
    // First-run seed — commented stub that explains usage.
    static const char *USER_CSS_STUB =
        "/* Curvz user stylesheet\n"
        " *\n"
        " * Loaded after Curvz's built-in CSS.  Rules here override defaults;\n"
        " * properties you don't set fall through to the built-in styles.\n"
        " *\n"
        " * To discover class names on any widget, press Ctrl+Shift+D to open\n"
        " * the GTK Inspector, then use its 'Pick widget' button (crosshair)\n"
        " * to select a widget and read its CSS classes.\n"
        " *\n"
        " * To restore defaults: empty this file or delete its contents.\n"
        " * Deleting the file entirely will cause Curvz to re-seed this stub\n"
        " * on next launch.\n"
        " *\n"
        " * Changes take effect on Curvz restart.\n"
        " *\n"
        " * Examples:\n"
        " *\n"
        " *   .inspector-group-title { color: #ffcc99; }\n"
        " *   .inspector-section-header { padding: 4px 10px; }\n"
        " *   .doc-tab-active          { background-color: #2a2a2a; }\n"
        " */\n";
    std::ofstream f(user_css_path);
    if (f) {
      f << USER_CSS_STUB;
      LOG_INFO("User CSS: seeded '{}'", user_css_path.string());
    } else {
      LOG_WARN("User CSS: cannot write '{}'", user_css_path.string());
    }
  }
  if (css_is_override) {
    LOG_INFO("User CSS: using override path '{}'", user_css_path.string());
  }

  if (fs::exists(user_css_path)) {
    auto user_css = Gtk::CssProvider::create();
    user_css->signal_parsing_error().connect(
        [user_css_path](const Glib::RefPtr<const Gtk::CssSection> &section,
                        const Glib::Error &error) {
          LOG_WARN("User CSS parse error in '{}': {}", user_css_path.string(),
                   error.what());
          (void)section;
        });
    try {
      user_css->load_from_path(user_css_path.string());
      Gtk::StyleContext::add_provider_for_display(
          get_display(), user_css, GTK_STYLE_PROVIDER_PRIORITY_USER);
      LOG_INFO("User CSS: loaded '{}'", user_css_path.string());
    } catch (const Glib::Error &e) {
      LOG_WARN("User CSS: failed to load '{}': {}", user_css_path.string(),
               e.what());
    }
  }

  // S117 m2: setup_project must run BEFORE setup_headerbar.
  //
  // GTK4's CSD (client-side decoration) window inserts a `box.vertical`
  // wrapper between the window and its content, including the headerbar.
  // We added a CSS rule for that wrapper (window.csd > box.vertical)
  // — the rule resolves correctly at the cascade level. But on a freshly
  // launched light-motif project, the wrapper still painted black.
  //
  // Cause: GTK4 resolves and caches the style of the CSD wrapper +
  // headerbar at window-construction time. Adding the `curvz-light`
  // class to the window AFTER that point (which is what setup_project()
  // does, via apply_motif_to_window) does not trigger those cached
  // surfaces to re-walk the cascade. They keep painting whatever they
  // resolved on first pass — the dark token values.
  //
  // Symptom: app boots dark on light-motif projects. Opening GTK
  // Inspector forces a global style invalidation that finally re-walks
  // the CSD wrapper — and the headerbar/wrapper turn light. Closing
  // inspector reverts (style cache restored).
  //
  // Fix: load the project (which sets m_project->motif) and apply the
  // motif class to `this` BEFORE setup_headerbar(). The class is on
  // the window when the CSD wrapper + headerbar resolve their style
  // the first time, so they pick up the light tokens directly.
  setup_project();

  // s254 m2 — Construct the `win` ActionGroupScriptable BEFORE
  // setup_menu() runs, because setup_menu()'s action declarations
  // call add_scripted_action() for the wrap-now subset, and that
  // helper registers each wrapped action against this Scriptable.
  // The Scriptable's registry entry IS the script-side `win`
  // handle. See scripting/Action.hpp for the wrapper-class design
  // and the helper-replaces-three-line-pattern motivation; see
  // tier2_action_audit.md for the wrap-now classification of all
  // 74 win.* actions.
  m_action_group_scriptable =
      std::make_unique<curvz::scripting::ActionGroupScriptable>("win");

  setup_headerbar();
  setup_menu();
  setup_layout();
  connect_signals();

  // s145 m1 — apply boot-time ruler visibility to the actual widgets.
  // setup_menu() seeded m_rulers_visible from AppPreferences, and
  // setup_layout() built the ruler widgets (which default visible).
  // Push the seeded value through to the widgets so a pref of `false`
  // boots with rulers hidden. The action's state was already set
  // correctly when it was created in setup_menu, so the menu checkmark
  // matches.
  toggle_rulers(m_rulers_visible);

  // s216 m1 / s219 m1 — construct the `layers` model Scriptable now that
  // setup_project has run (m_project is initialised). The getter
  // captures `this` so it resolves the CURRENT m_project pointer on
  // every verb invocation — the unique_ptr can be reset / reassigned
  // (close-project, load-project) without invalidating the Scriptable.
  // The history pointer is stable (m_history is a value member at a
  // fixed address); pass by raw pointer.
  //
  // Pilot scope: one collection registered. Per-instance addresses
  // (`layer.<iid>`) come from the router hooks; no per-instance
  // registry entry. Registry list shows `layers` once for the
  // lifetime of the window, regardless of how many layers exist or
  // how many proxies are materialised across script runs.
  //
  // s219 m1 — always constructed. The Scriptable is cheap (one
  // unique_ptr + one registry entry); whether the SCRIPTER WINDOW is
  // visible is a separate concern governed by scripter_enabled.
  //
  // s237 m2 — third ctor arg: a PanelGetter lambda for after-mutation
  // panel rebuild. Same shape as the s222 m1 fix-1 hook on
  // StylesScriptable. Closes the "ghost row stays after script delete"
  // gap Scott reported at s237 close — script-side `layers delete`
  // mutates the doc but doesn't reach the panel, so the row keeps
  // rendering until the user clicks +/- (which forces a rebuild
  // via on_add_layer / on_delete_layer's tail). The lambda is
  // identical to StylesScriptable's: returns &m_layers, the panel
  // member that lives at MainWindow scope.
  m_layers_scriptable = std::make_unique<curvz::scripting::LayersScriptable>(
      [this]() -> CurvzProject * { return m_project.get(); }, &m_history,
      [this]() -> LayersPanel * { return &m_layers; });

  // s218 m1 / s219 m1 — `guides` collection Scriptable. Same construction shape
  // as the layers Scriptable above: same project-getter (resolves
  // m_project.get() fresh on every verb call so close/open survives),
  // same m_history pointer (captured for shape-symmetry — guides
  // aren't undoable today, but the wiring is in place for whenever
  // guide-flavored field-edit commands land).
  m_guides_scriptable = std::make_unique<curvz::scripting::GuidesScriptable>(
      [this]() -> CurvzProject * { return m_project.get(); }, &m_history);

  // s221 m1 — `swatches` collection Scriptable. First library-
  // collection Scriptable (the previous two wrap SceneNode collections;
  // this one wraps the project's SwatchLibrary). Same construction
  // shape: project-getter resolves m_project.get() fresh on every verb
  // call, history pointer to the doc's CommandHistory.
  //
  // Unlike the guides Scriptable's captured-but-unused history pointer,
  // this one is DEREFERENCED on every mutating verb — s220 m1a made
  // swatch CRUD undoable (AddSwatchCommand / RemoveSwatchCommand /
  // EditSwatchCommand) and the Scriptable rides that plumbing.
  m_swatches_scriptable =
      std::make_unique<curvz::scripting::SwatchesScriptable>(
          [this]() -> CurvzProject * { return m_project.get(); }, &m_history);

  // s243 m2 — `palettes` collection Scriptable. Sibling of
  // m_swatches_scriptable — both wrap CurvzProject::swatches (the
  // SwatchLibrary holds two parallel pools, swatches and palettes;
  // each gets its own collection Scriptable). Same construction
  // shape: project-getter resolves m_project.get() fresh on every
  // verb call, history pointer to the doc's CommandHistory.
  //
  // history is DEREFERENCED on every mutating CRUD verb — s243 m1
  // made every palette CRUD undoable (AddPaletteCommand /
  // RemovePaletteCommand / RenamePaletteCommand) and the Scriptable
  // rides exactly that plumbing. The `activate` verb is the one
  // mutator that doesn't push a command (transient working state,
  // matching the panel's posture).
  //
  // No PanelGetter — SwatchesPanel listens on the s243 m1 palette
  // signal trio and rebuilds wholesale on any fire. Fourth
  // application of the panel-visibility canon entry, same shape as
  // m_themes_scriptable (no panel-side step needed).
  m_palettes_scriptable =
      std::make_unique<curvz::scripting::PalettesScriptable>(
          [this]() -> CurvzProject * { return m_project.get(); }, &m_history);

  // s222 m1 — `styles` collection Scriptable. Second library-collection
  // Scriptable (sibling of m_swatches_scriptable; both wrap
  // project-scoped libraries — the swatches Scriptable wraps
  // CurvzProject::swatches, this one wraps CurvzProject::styles). Same
  // construction shape: project-getter resolves m_project.get() fresh
  // on every verb call, history pointer to the doc's CommandHistory.
  //
  // history is DEREFERENCED on every mutating verb — the S81 m4c-3
  // commands (AddStyleCommand / UpdateStyleCommand / RemoveStyleCommand)
  // make every style CRUD undoable from the panel, and the Scriptable
  // rides exactly that plumbing.
  //
  // s222 m1 fix-1: panel-getter for after-mutation navigation. Without
  // this, scripted `new`/`duplicate` leave the panel sitting on
  // whatever category was selected before the script ran — library
  // is populated, dropdown has the new category as an option, but the
  // user sees nothing change. With this, the Scriptable navigates the
  // panel to the new style's category right after the command pushes.
  // See StylesScriptable.hpp's "Panel visibility" block for the full
  // rationale and the comparison with SwatchesScriptable's library-
  // side fix-1.
  m_styles_scriptable = std::make_unique<curvz::scripting::StylesScriptable>(
      [this]() -> CurvzProject * { return m_project.get(); }, &m_history,
      [this]() -> StylesPanel * { return &m_styles; });

  // s223 m1 — `themes` collection Scriptable. Third library-collection
  // variant (sibling of m_swatches_scriptable and m_styles_scriptable;
  // all three wrap project-scoped libraries — swatches wraps
  // CurvzProject::swatches, styles wraps CurvzProject::styles, this
  // one wraps CurvzProject::themes). Same construction shape:
  // project-getter resolves m_project.get() fresh on every verb call,
  // history pointer to the doc's CommandHistory.
  //
  // history is DEREFERENCED on every mutating verb — the S103 m2
  // commands (AddThemeCommand / UpdateThemeCommand / RemoveThemeCommand)
  // make every theme CRUD undoable from the panel, and the Scriptable
  // rides exactly that plumbing.
  //
  // No PanelGetter — see ThemesScriptable.hpp's "Panel visibility"
  // block. ThemesPanel renders the full user-tier theme list flat and
  // rebuilds on signal_theme_added / _changed / _removed; the new row
  // appears automatically after a scripted `new` or `duplicate`. Third
  // application of the visibility canon entry; this time the answer is
  // "no panel-side step needed because the panel doesn't filter."
  // Construction is two-arg, matching m_swatches_scriptable.
  m_themes_scriptable = std::make_unique<curvz::scripting::ThemesScriptable>(
      [this]() -> CurvzProject * { return m_project.get(); }, &m_history);

  // s230 m1 — `objects` collection Scriptable. Sixth row-bound model
  // Scriptable, OPENS the multi-session `objects` arc. Wraps every
  // "real scene content" SceneNode (Path / Group / Compound /
  // ClipGroup / Blend / Warp / Text / Image / Ref / Measurement)
  // across all layers in the active document via a recursive tree
  // walk. Layer / Guide / special-layer types are explicitly out of
  // scope — they have their own Scriptables (`layers`, `guides`).
  //
  // Same construction shape as the five model Scriptables above:
  // project-getter resolves m_project.get() fresh on every verb call,
  // history pointer to the doc's CommandHistory.
  //
  // m1 ships READ-SIDE only — collection queries, invoke-shaped
  // reads (find_by_name / find_by_type), per-instance proxy queries.
  // No mutating verbs anywhere yet. m3+ adds toggle_visible /
  // set_visible / set_locked / rename riding EditObjectCommand;
  // m4+ adds structural verbs (move / reparent / delete / duplicate);
  // m5+ adds collection-level structural verbs (new / group /
  // ungroup). history is CAPTURED-BUT-UNUSED in m1 — wiring is in
  // place for whenever m3+ lands so no ctor signature churn.
  //
  // See ObjectsScriptable.hpp for the full verb/query surface, the
  // scope-choice discussion (real scene contents only — option (ii)
  // from the s230 handoff's design fork), and why selected_iids is
  // NOT on `objects` (selection is canvas-side state; it gets its
  // own future Scriptable).
  //
  // s235 m1 — third ctor arg: a NotifyDocChanged callback that fires
  // the canvas-refresh cascade. ObjectsScriptable and its
  // ObjectProxy children invoke this at the end of every successful
  // mutating verb (collection `new` / `delete`; element toggle_visible
  // / set_visible / toggle_locked / set_locked / rename / move /
  // reparent / duplicate), AFTER the command push. Two calls are the
  // canonical shape for external mutators: queue_draw repaints the
  // canvas, notify_document_changed runs the structural cascade
  // (layers panel rebuild, status counts, gallery thumb, blend
  // cache invalidate, schedule_save). Mirrors s227 m1's
  // themes_refresh_cascade lambda — same two-call pattern for the
  // ThemesScriptable apply verb. Closes the gap Scott caught at the
  // end of s234: script-path mutations updated the doc model and the
  // iid index but never told Canvas anything changed, so the smoke
  // ran clean while the canvas kept rendering its previous frame.
  m_objects_scriptable = std::make_unique<curvz::scripting::ObjectsScriptable>(
      [this]() -> CurvzProject * { return m_project.get(); }, &m_history,
      [this]() {
        m_canvas.queue_draw();
        m_canvas.notify_document_changed();
      });

  // s222 m2 — `inspector` Scriptable. Flat verb surface (no proxy
  // routing) that delegates to MainWindow's existing collapse-all
  // and apply-section-open methods. Constructed last among the
  // Scriptables because it's a meta-Scriptable — it talks to
  // MainWindow itself rather than to a project sub-system, and only
  // makes sense once MainWindow's section state is initialised
  // (which happens elsewhere in the ctor before this point). Same
  // unique_ptr / out-of-line dtor pattern as the five above.
  m_inspector_scriptable =
      std::make_unique<curvz::scripting::InspectorScriptable>(this);

  // s246 m1 — `proj` Scriptable. First headless-verb singleton from
  // ARC m5b. Same singleton-host pattern as inspector: flat verb
  // surface, no proxy routing, MainWindow-pointer constructor. The
  // one verb that ships in m1 (`save`) goes through
  // script_save_project() — the same code on_save runs minus the
  // picker-fallthrough branch (which becomes a structured refusal
  // since scripts can't summon modals). See ProjScriptable.hpp for
  // the design block and the RunContext mask rationale.
  m_proj_scriptable = std::make_unique<curvz::scripting::ProjScriptable>(this);

  // s251 m1 — `export` Scriptable. Second headless-verb singleton from
  // ARC m5b. Sibling singleton-host pattern as proj: flat verb surface,
  // no proxy routing, MainWindow-pointer constructor. The one verb that
  // ships in m1 (`svg <path>`) goes through script_export_svg() — the
  // helper that wraps SvgWriter::write_svg_file with the same project-
  // state pre-checks (NoProject / NoActiveDoc / IoFailed). path_is_safe
  // pre-flight lives in the Scriptable, mirroring save_as / load / new.
  // See ExportScriptable.hpp for the design block and the RunContext
  // mask rationale (Scripter | TestRunner — same as proj save; the
  // path-containment gate covers the system-integrity layer and the
  // Macro exclusion covers the recorded-automation surface).
  m_export_scriptable =
      std::make_unique<curvz::scripting::ExportScriptable>(this);

  // s263 m2 — `app` Scriptable. Third headless-verb singleton from
  // ARC m5b. Sibling singleton-host pattern as proj / export: flat
  // verb surface, no proxy routing, MainWindow-pointer constructor.
  // The two pure-read verbs in m2 / m3 (`version` reading the
  // CURVZ_VERSION macro plumbed via target_compile_definitions, and
  // `gtk_version` reading gtk_get_*_version()) both declare the
  // Scripter | TestRunner mask. No disk writes, no project state
  // mutation; the singleton's lifetime is its registration window.
  // See AppScriptable.hpp for the design block and the RunContext
  // mask rationale (Scripter | TestRunner — same as proj.save and
  // export.svg; Macro is OUT because recorded-macro replay on a
  // different build/system means the recorded value is stale at
  // replay time, violating the recorded-macro mental model).
  //
  // s288 m2 — second ctor arg: EnactPenPath callback. Forwards to
  // Canvas's thin entry point which forwards to the SvgPerformer.
  // The animation runs asynchronously via Glib::Timeout owned by the
  // performer; this lambda returns immediately.
  // s288 m3 — third ctor arg: AnimateSvgFile callback. Sibling routing
  // for the SVG-file entry point.
  // s291 m2 — renamed Canvas-side methods (welcome_* → perform_*); verb
  // names on the AppScriptable surface stay unchanged (enact_pen_path,
  // animate_svg) so scripts continue to work without edits.
  m_app_scriptable = std::make_unique<curvz::scripting::AppScriptable>(
      this,
      [this](const std::string &d_string, double speed) {
        m_canvas.perform_pen_path(d_string, speed);
      },
      [this](const std::string &svg_path, double speed) {
        m_canvas.perform_svg_file(svg_path, speed);
      });

  // s219 m1 — Scripter window construction. Previously lived in
  // Application::on_activate; moved here so MainWindow owns the
  // window the way it owns HelpWindow, ShortcutsDialog, and the
  // other persistent floating dialogs.
  //
  // s267 m1 — scripts directory resolution lifted to scripts_user_dir()
  // and scripts_system_dir() free functions (defined at the top of
  // this TU). User dir follows AppPreferences::scripts_path_override with
  // fallback to
  // ~/.config/curvz/scripts; system dir is the install-time
  // /usr/share/curvz/scripts. The historical tests/scripts build-tree
  // probe is gone — that was test-runner ergonomics leaking into a
  // runtime app. Developers pointing the Scripter at tests/scripts
  // now do it once via the picker (or Paths preferences), and the
  // pref persists. The picker's accept handler funnels back through
  // AppPreferences::set_scripts_path_override via the callback wired
  // a few lines below.
  {
    const std::string scripts_dir = scripts_user_dir();
    const std::string sys_scripts = scripts_system_dir();
    m_scripter = std::make_unique<curvz::scripting::ScripterWindow>(
        scripts_dir, sys_scripts);
    LOG_INFO("MainWindow: Scripter constructed "
             "(user scripts: {}, system scripts: {})",
             scripts_dir, sys_scripts);

    // s267 m1 — picker-persistence bridge. The Scripter's folder
    // picker (statusbar at the window bottom) writes back to
    // scripts_path_override here, so the choice persists across
    // launches. Kept as a callback rather than reaching into
    // AppPreferences from inside curvz::scripting — keeps the
    // scripting namespace independent of preferences plumbing,
    // symmetric with how Application bridges the listener's
    // SubtitleCallback to MainWindow's caption bar.
    m_scripter->set_user_folder_changed_callback([](const std::string &path) {
      AppPreferences::instance().set_scripts_path_override(path);
      LOG_INFO("MainWindow: scripts_path_override -> '{}' "
               "(from Scripter folder picker)",
               path);
    });

    // s191 m3 / s219 m1 — bridge `#[sub]` lines from the Scripter's
    // listener to MainWindow's caption bar. Previously wired in
    // Application; moved here with the Scripter. The listener
    // emits its flanked-caption line to the Scripter's output pane
    // (driven by the OutputCallback wired inside ScripterWindow)
    // and ALSO hands the body text to set_subtitle() via this
    // bridge — where the user's eyes are while watching Curvz drive
    // itself is where the caption lands.
    if (auto *lst = m_scripter->listener()) {
      lst->set_subtitle_callback(
          [this](const std::string &text) { set_subtitle(text); });

      // s201 m3 / s219 m1 — `do <action.name>` dispatch. The
      // script's user-driven verbs reach substrate widgets directly,
      // but menu items and inline action-driven buttons fire Gio
      // actions instead. This callback bridges that gap so scripts
      // can do `do styles.create-empty` to open the style editor.
      //
      // Routing by prefix because GTK's activate_action walks UP
      // from the originating widget looking for the action group —
      // we have to call activate_action() on the widget that owns
      // the group (the panel that inserted it), not on MainWindow.
      //
      // The prefix list grows as new action-driven UI surfaces want
      // to be script-addressable. Today's roster:
      //   styles.* — StylesPanel kebab + context menu actions
      lst->set_action_callback([this](const std::string &action_name) -> bool {
        const auto dot = action_name.find('.');
        if (dot == std::string::npos)
          return false;
        const std::string prefix = action_name.substr(0, dot);
        if (prefix == "styles") {
          return m_styles.activate_action(action_name);
        }
        return false;
      });
    }
  }

  // s219 m1 — wire the live pref subscription. AppPreferences fires
  // signal_changed() for every preference write (it's a single
  // parameterless signal across the whole prefs surface), so we just
  // re-run apply_scripter_pref(). The function is idempotent and
  // cheap (a visibility flip + an action-state set + a present-or-
  // hide), so firing it on unrelated pref writes is harmless.
  //
  // The initial call right after subscription pulls every surface
  // (headerbar button visibility, menu action state, scripter window
  // present/hide) into agreement with the pref's current value —
  // necessary because setup_headerbar() and setup_menu() built their
  // controls in their default-off state without consulting the pref.
  AppPreferences::instance().signal_changed().connect(
      [this]() { apply_scripter_pref(); });
  apply_scripter_pref();

  LOG_INFO("MainWindow created");
}

// s216 m1 / s218 m1 / s219 m1 / s221 m1 / s222 m1 / s222 m2 / s223 m1 / s230 m1
// / s246 m1 / s263 m2 — out-of-line dtor. The header forward-declares
// curvz::scripting::LayersScriptable, curvz::scripting::GuidesScriptable,
// curvz::scripting::SwatchesScriptable, curvz::scripting::StylesScriptable,
// curvz::scripting::ThemesScriptable, curvz::scripting::ObjectsScriptable,
// curvz::scripting::InspectorScriptable, curvz::scripting::ProjScriptable,
// curvz::scripting::ExportScriptable, and curvz::scripting::AppScriptable;
// the unique_ptrs in the header need the complete types at MainWindow
// destruction time, and they're only visible in this TU (where we
// included the full headers above). Defaulted body is the right
// answer — every member has its own destructor; we just need the
// complete types visible at the dtor's point-of-emission. No longer
// gated as of s219 m1.
MainWindow::~MainWindow() = default;

// s219 m1 — apply scripter_enabled to every dependent surface.
//
// Called once at the end of MainWindow's ctor (after setup_headerbar
// and setup_menu have built their controls) and again on every
// AppPreferences::signal_changed emit. Idempotent.
//
// Surfaces:
//   1. Headerbar Scripter toggle (m_scripter_btn): visible iff
//      scripter_enabled. The button itself is always packed into
//      the headerbar at construction (so its slot is reserved); we
//      flip visibility only.
//   2. Developer ▸ Scripting menu action (m_act_toggle_scripting):
//      state mirrors the pref so the checkmark stays honest if the
//      pref was changed via the inspector switch (or any future
//      surface).
//   3. The Scripter window itself: present() when enabling, hide
//      when disabling. The Scripter is a MainWindow member
//      (m_scripter, unique_ptr); it just hides instead of destroying
//      on the X-button (set_hide_on_close(true) in its ctor).
//
// Defensive on every step — apply_scripter_pref() can be called at
// any point in MainWindow's lifetime, including from a pref-changed
// signal that arrives before construction has fully finished. Guards
// against null m_scripter_btn (not yet packed), null
// m_act_toggle_scripting (not yet created), and null m_scripter
// (ctor not yet at the construction site).
void MainWindow::apply_scripter_pref() {
  const bool on = AppPreferences::instance().scripter_enabled();

  // Headerbar button visibility. m_scripter_btn is a
  // curvz::widgets::ToggleButton which IS-A Gtk::Widget, so the
  // implicit upcast for set_visible() is fine now that the header
  // is included in this TU.
  if (m_scripter_btn != nullptr) {
    m_scripter_btn->set_visible(on);
  }

  // Menu action state. Gio::SimpleAction's set_state takes a Variant;
  // we wrap the bool and push it. The menu picks the new state up on
  // its next render cycle.
  if (m_act_toggle_scripting) {
    m_act_toggle_scripting->set_state(Glib::Variant<bool>::create(on));
  }

  // Scripter window — hide if the pref is being turned off, otherwise
  // do NOTHING (leave the window's current visibility alone).
  //
  // s219 m1 design rule: pref-on means "scripting is available" —
  // the monkey button is visible and ready, and the Scripter window
  // is reachable via that button. Pref-on does NOT mean "show the
  // Scripter window right now." The user clicks the monkey to open
  // the Scripter, the same way they'd click any window-open button.
  //
  // This separates two concerns the earlier (s190 / s219 m1 initial)
  // design conflated:
  //   - feature enablement (pref) — set by menu / inspector
  //   - window visibility — controlled by monkey button + X
  //
  // So the show side here is empty by design. The hide side fires
  // when the user disables scripting altogether: without hiding the
  // window we'd leave it open with no way to dismiss it (the monkey
  // button just disappeared along with the feature).
  if (!on) {
    show_scripter(false);
  }
}

// s219 m1 — public entry for showing or hiding the Scripter window.
// Called by:
//   - the headerbar monkey-button's signal_toggled (in MainWindow_zones)
//   - apply_scripter_pref() above (hide-only on pref-off transitions)
//
// The show path matches HelpWindow::show / ShortcutsDialog::show:
// re-assert transient_for on every show so mutter keeps the window
// relationship fresh, apply the motif class so theming sticks, then
// present. ScripterWindow::show() encapsulates all three steps.
//
// The hide path is a plain set_visible(false). hide_on_close is
// already set in ScripterWindow's ctor, so the X-button does the
// same thing automatically.
void MainWindow::show_scripter(bool visible) {
  if (!m_scripter)
    return; // defensive: ctor not finished yet
  if (visible) {
    m_scripter->show(*this);
  } else {
    m_scripter->set_visible(false);
  }
}

void MainWindow::setup_project() {
  // s144 m2 — Reopen-last-project pref. When false, skip the
  // last-project lookup entirely and fall through to the blank-project
  // branch. The pref defaults to true so existing users (and anyone
  // without preferences.json yet) keep the historical behaviour.
  bool reopen = AppPreferences::instance().reopen_last_project();
  // Try to reopen last project
  std::string last = reopen ? load_last_project_path() : std::string{};
  if (!last.empty() && fs::exists(last)) {
    auto project = CurvzProject::open(last);
    if (project) {
      m_project = std::move(project);
      // Propagate project-wide snap to all docs so inspector reads correct
      // values
      for (auto &doc : m_project->documents)
        doc->snap = m_project->snap;
      update_title();
      // s116 fix2: apply motif now (before setup_layout creates widgets)
      // so the CSS class is on the window when children are first styled.
      // Without this, the project's light-motif setting is read into
      // m_project->motif but the curvz-light class is never added until
      // a later trigger (active-doc switch, inspector edit) — meaning
      // the app boots dark even when the project says Light.
      apply_motif_to_window();
      // s144 m3 — startup auto-reopen routes around load_project(), so
      // record-recent here too. add() promotes the entry to front (it's
      // probably already at index 0 from last session, but make-it-so).
      if (!m_project->directory.empty())
        RecentProjects::instance().add(m_project->directory);
      return;
    }
  }
  // Fall back to blank project
  m_project = std::make_unique<CurvzProject>();
  auto doc = std::make_unique<CurvzDocument>();
  doc->canvas = CanvasModel::from_pixels(24, 24);
  doc->filename = "new-icon.svg";
  m_project->documents.push_back(std::move(doc));
  m_project->active_doc_index = 0;
  update_title();
  // Fresh blank project defaults to Dark, but apply for symmetry — keeps
  // class state consistent regardless of which project path executed.
  apply_motif_to_window();
}

// ── Slots
// ─────────────────────────────────────────────────────────────────────

void MainWindow::on_tool_changed(ActiveTool tool) {
  LOG_DEBUG("Active tool changed: {}", (int)tool);
  m_active_tool = tool;
  // Cancel any in-progress line when leaving the Line tool
  if (tool != ActiveTool::Line)
    m_canvas.cancel_line_path();
  m_canvas.set_active_tool(tool);
  m_context_bar.set_tool(tool);
  refresh_inspector();
  if (m_update_align_btn)
    m_update_align_btn();
}

void MainWindow::on_doc_activated(int index) {
  if (!m_project || index < 0 || index >= (int)m_project->documents.size())
    return;
  // Clicking a project doc exits any active system icon preview
  if (m_preview_active) {
    m_preview_active = false;
    m_preview_doc.reset();
    m_statusbar.set_mode(m_canvas.is_outline_mode() ? "Outline" : "Preview");
  }
  m_project->active_doc_index = index;
  auto *doc = m_project->active_doc();
  m_canvas.set_document(doc);
  m_toolbar.set_document(doc);
  m_preview.set_document(doc);
  m_layers.set_document(doc);
  m_library.set_document(doc);
  if (doc) {
    CanvasModel *cm = &doc->canvas;
    Glib::signal_idle().connect_once([this, cm]() {
      if (m_closing)
        return;
      m_properties.show_document_props(cm);
    });
  }
  m_gallery.refresh();
  m_doc_tabs.refresh();
  m_themes.on_documents_changed(); // s147 m3 — active marker + targets list
  update_rulers();
  update_title();
  apply_motif_to_window();
  LOG_INFO("Doc activated: index {}", index);
}

// s108 m7: cycle through the project's documents with wraparound. Called
// from the doc-next / doc-prev actions AND from the CAPTURE-phase
// keyboard handler (Ctrl+Tab / Ctrl+Page_Down can't reliably reach the
// action accel system because GTK4's focus-traversal default consumes
// Tab keys). Funnels through on_doc_activated so all panels stay in
// sync via the single canonical seam.
void MainWindow::cycle_doc(int delta) {
  if (!m_project)
    return;
  const int n = (int)m_project->documents.size();
  if (n <= 1)
    return;
  int cur = m_project->active_doc_index;
  if (cur < 0 || cur >= n)
    cur = 0;
  int next = ((cur + delta) % n + n) % n;
  on_doc_activated(next);
}

// ── rename_doc ────────────────────────────────────────────────────────────
// Rename a document within the current project. Sanitises the name (spaces
// → dashes, path separators → underscore), enforces uniqueness against
// other documents in the project, performs an `fs::rename` on disk if the
// project is saved, and updates all UI panels.
//
// Reused by:
//   • Inspector "File" entry (always operates on active_doc)
//   • DocumentGallery double-click / right-click → Rename (any doc by index)
void MainWindow::rename_doc(CurvzDocument *doc, std::string new_name) {
  if (!m_project || !doc)
    return;

  // Sanitise: spaces → dashes, strip path separators
  for (char &c : new_name) {
    if (c == ' ')
      c = '-';
    if (c == '/' || c == '\\')
      c = '_';
  }
  if (new_name.empty())
    return;

  std::string new_fname = new_name + ".svg";

  // No change?
  if (new_fname == fs::path(doc->filename).filename().string())
    return;

  // Ensure unique within project
  int suffix = 2;
  std::string base = new_name;
  while (std::any_of(m_project->documents.begin(), m_project->documents.end(),
                     [&](const auto &d) {
                       return d.get() != doc &&
                              fs::path(d->filename).filename().string() ==
                                  new_fname;
                     })) {
    new_fname = base + std::to_string(suffix++) + ".svg";
  }

  // Rename on disk if project is saved
  if (!m_project->directory.empty()) {
    fs::path old_path =
        fs::path(m_project->directory) / fs::path(doc->filename).filename();
    fs::path new_path = fs::path(m_project->directory) / new_fname;
    std::error_code ec;
    if (fs::exists(old_path, ec))
      fs::rename(old_path, new_path, ec);
    if (ec)
      LOG_WARN("rename_doc: rename failed: {}", ec.message());
    else
      LOG_INFO("rename_doc: '{}' → '{}'", old_path.filename().string(),
               new_fname);
  }

  doc->filename = new_fname;
  m_project->save();
  update_all_panels();
}

} // namespace Curvz
