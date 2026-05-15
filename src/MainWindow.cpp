#include "MainWindow.hpp"
#include "AppPreferences.hpp" // s139 m2 / s143 m1 — boolean-cleanup quality pref + sync
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
#ifdef CURVZ_DIAGNOSTIC
#include "scripting/LayersScriptable.hpp"  // s216 m1 — model Scriptable pilot
#include "scripting/GuidesScriptable.hpp"  // s218 m1 — second model Scriptable
#endif
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

MainWindow::MainWindow(Application & /*app*/) {
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

#ifdef CURVZ_DIAGNOSTIC
  // s216 m1 — construct the `layers` model Scriptable now that
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
  m_layers_scriptable =
      std::make_unique<curvz::scripting::LayersScriptable>(
          [this]() -> CurvzProject* { return m_project.get(); },
          &m_history);

  // s218 m1 — `guides` collection Scriptable. Same construction shape
  // as the layers Scriptable above: same project-getter (resolves
  // m_project.get() fresh on every verb call so close/open survives),
  // same m_history pointer (captured for shape-symmetry — guides
  // aren't undoable today, but the wiring is in place for whenever
  // guide-flavored field-edit commands land).
  m_guides_scriptable =
      std::make_unique<curvz::scripting::GuidesScriptable>(
          [this]() -> CurvzProject* { return m_project.get(); },
          &m_history);
#endif

  LOG_INFO("MainWindow created");
}

#ifdef CURVZ_DIAGNOSTIC
// s216 m1 — out-of-line dtor. The header forward-declares
// curvz::scripting::LayersScriptable; the unique_ptr in the header
// needs the complete type at MainWindow destruction time, and that's
// only visible in this TU (where we included the full header above).
// Defaulted body is the right answer — every member has its own
// destructor; we just need the complete type visible at the dtor's
// point-of-emission.
//
// s218 m1 — the same out-of-line dtor also covers
// curvz::scripting::GuidesScriptable (forward-declared in the header,
// fully included above). One dtor, every fwd-declared scripting
// member destroyed correctly. Future row-bound model Scriptables
// (swatches, styles, themes, objects) ride on this same dtor without
// further changes.
MainWindow::~MainWindow() = default;
#endif

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
