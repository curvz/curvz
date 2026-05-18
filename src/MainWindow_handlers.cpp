#include "MainWindow.hpp"
#include "AppPreferences.hpp" // s139 m2 / s143 m1 — boolean-cleanup quality pref + sync
#include "ContextBar.hpp"
#include "CoordSpace.hpp"
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"
#include "DocTabBar.hpp"
#include "ExportDialog.hpp"
#include "PngExporter.hpp"  // s252 m2 — script_export_png helper uses export_png_sized
#include "RecentProjects.hpp" // s144 m3 — Open Recent submenu
#include "Ruler.hpp"
#include "SvgOptimiser.hpp"
#include "SvgParser.hpp"
#include "SvgWriter.hpp"
#include "TemplateLibrary.hpp"
#include "curvz/widgets/Button.hpp"        // s208 m5 — guide-review dialog substrate
#include "curvz/widgets/CheckButton.hpp"   // s208 m5 — guide-review dialog substrate
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
#include <cmath>  // s252 m2: std::round for png aspect-fit calc
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
// MainWindow_handlers.cpp — slot bodies (the on_*/apply_* methods that run when a binding fires).
// ─────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────
// ━━━ documents ━━━ Document and project lifecycle: new/open/save/save-as, import (SVG, SVG-as-icon, image), preview/copy icon, place image, export-docs, quit, close-project.
// ─────────────────────────────────────────────────────────────────────

void MainWindow::on_new_project() {
  check_unsaved_then([this]() {
    // First pick the project location
    auto file_dialog = Gtk::FileDialog::create();
    file_dialog->set_title("New Project — choose location and name");
    file_dialog->set_initial_name("MyIcons");
    file_dialog->save(*this, [this, file_dialog](
                                 Glib::RefPtr<Gio::AsyncResult> &result) {
      try {
        auto file = file_dialog->save_finish(result);
        if (!file)
          return;
        std::string path = file->get_path();
        // Strip extension if user typed one
        auto dot = path.rfind('.');
        auto sep = path.rfind('/');
        if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
          path = path.substr(0, dot);
        std::string dir = path + ".curvz";
        // Create an empty project — the user populates it via Add to Project.
        auto project = CurvzProject::create_empty(dir);
        if (!project) {
          LOG_ERROR("on_new_project: create_empty failed for '{}'", dir);
          return;
        }
        load_project(std::move(project));
      } catch (...) {
      }
    });
  });
}


void MainWindow::on_close_project() {
  check_unsaved_then([this]() {
    do_close_project();
  });
}


// s248 m1 — extracted from on_close_project's check_unsaved_then
// lambda body. The teardown work is unchanged from what the lambda
// used to do; lifting it to a method gives script_close_project a
// single source of truth to call without duplicating the panel-
// surgery boilerplate. Same pump-at-the-seam pattern as do_save_as
// (s247 m1's bool-returning lift).
//
// Preconditions are the caller's responsibility — this method
// unconditionally tears down whatever state exists. on_close_project
// gates via check_unsaved_then (modal Save/Discard/Cancel);
// script_close_project gates via its enum-returning structural
// refusals (NoProject / Dragging / Dirty). Either way, by the time
// do_close_project is called, the caller has decided the teardown
// is the right thing to do.
//
// Side effects: m_project becomes null, m_history reset to empty,
// every panel cleared of its project / document references, the
// idle-queued show_empty() on the properties panel fires after the
// current event loop turn (guarded against m_closing for the
// app-shutdown case), config file removed so next launch starts
// empty, project-sensitive actions disabled, title bar refreshed,
// LOG_INFO line emitted.
void MainWindow::do_close_project() {
  // s249 m1 fix — null-out panel library pointers BEFORE m_project.reset().
  //
  // Same root cause as the update_all_panels fix (see that function's
  // banner): m_canvas.set_document(nullptr) synchronously fires
  // notify_object_selection_changed, which triggers
  // StylesPanel::refresh, which dereferences m_library. If m_project
  // has already been reset by the time the signal fires, m_library
  // still points at the just-freed m_project->styles — crash.
  //
  // Two safe orderings exist:
  //   (A) Drop library pointers (nullptr) BEFORE resetting m_project.
  //       Panels handle nullptr in refresh() (StylesPanel::refresh
  //       short-circuits at line 432 when m_library is null).
  //   (B) Reset m_project AFTER all signal-emitting nullptr setters.
  //
  // Choosing (A): it matches update_all_panels's "Group A then
  // Group B" structure, and a panel sitting with a null library
  // pointer for the brief window between Group A and m_project.reset()
  // is harmless (no signals fire in that window).

  // Clear config so next launch doesn't reopen this project
  std::string cfg = config_path();
  if (std::filesystem::exists(cfg))
    std::filesystem::remove(cfg);

  // ── GROUP A: null out per-panel pointers (no signals fired here) ─
  m_canvas.set_swatch_library(nullptr);
  m_canvas.set_style_library(nullptr);
  m_canvas.set_project(nullptr);
  m_swatches.set_library(nullptr);
  m_styles.set_library(nullptr);
  m_styles.set_swatch_library(nullptr);
  m_themes.set_project(nullptr); // s147 m3
  m_toolbar.set_swatch_library(nullptr);
  m_layers.set_project(nullptr);  // s171 m1 — match project lifecycle

  // Drop the project — safe now that no panel holds a pointer into it.
  m_project.reset();
  m_history = CommandHistory{};

  // ── GROUP B: null document/canvas pointers (signal-emitting) ─
  m_canvas.set_document(nullptr);
  m_canvas.set_history(nullptr);
  m_preview.set_document(nullptr);
  m_layers.set_document(nullptr);
  m_library.set_document(nullptr);
  m_doc_tabs.set_project(nullptr);
  m_doc_tabs.refresh();
  m_gallery.set_project(nullptr);
  m_properties.set_project(nullptr);
  Glib::signal_idle().connect_once([this]() {
    if (!m_closing)
      m_properties.show_empty();
  });

  update_project_sensitive();
  update_title();
  LOG_INFO("Project closed");
}


// File → Add to Project… — adds a new document to the currently loaded
// project. Disabled (via update_project_sensitive) when no project is open.
// Same behavior as the DocTabBar "+" button.
void MainWindow::on_new() {
  if (!m_project)
    return; // guarded by menu sensitivity, belt-and-braces
  // s148 m1: preview colours from active doc (re-promoted to per-doc).
  auto *active = m_project->active_doc();
  double pv_ws_r = 0.09, pv_ws_g = 0.09, pv_ws_b = 0.09;
  double pv_ab_r = 0.157, pv_ab_g = 0.157, pv_ab_b = 0.157;
  if (active) {
    pv_ws_r = active->workspace_bg_r(m_project->motif);
    pv_ws_g = active->workspace_bg_g(m_project->motif);
    pv_ws_b = active->workspace_bg_b(m_project->motif);
    pv_ab_r = active->artboard_bg_r(m_project->motif);
    pv_ab_g = active->artboard_bg_g(m_project->motif);
    pv_ab_b = active->artboard_bg_b(m_project->motif);
  }
  m_new_doc_dialog.show(
      *this, ndd_available_themes(),
      m_project->motif == Motif::Light ? templates::MotifTag::Light
                                       : templates::MotifTag::Dark,
      pv_ws_r, pv_ws_g, pv_ws_b, pv_ab_r, pv_ab_g, pv_ab_b,
      [this](std::unique_ptr<CurvzDocument> seed, std::string name,
             std::optional<theme::ThemeId> theme_id) {
        if (!seed)
          return;
        if (!m_project)
          return; // project may have closed between dialog ops
        ndd_apply_chosen_theme(*seed, theme_id);

        // Sanitise name → filename stem.
        std::string base = curvz::utils::doc_stem_from_name(name);
        std::string fname = base + ".svg";
        // Ensure unique within project
        int suffix = 2;
        while (std::any_of(m_project->documents.begin(),
                           m_project->documents.end(),
                           [&](const auto &d) { return d->filename == fname; }))
          fname = base + std::to_string(suffix++) + ".svg";

        seed->filename = fname;

        m_project->documents.push_back(std::move(seed));
        m_project->active_doc_index = (int)m_project->documents.size() - 1;
        m_project->save();
        update_all_panels();
        LOG_INFO("on_new: added '{}' to project", fname);
      });
}


void MainWindow::on_open() {
  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Open Project (.curvz folder)");
  // s125 m1e: re-open at the last-used parent directory if remembered.
  // For Open Project the picker selects a *.curvz folder, so the value we
  // store is the *parent* of the chosen folder (where the user was browsing),
  // not the .curvz folder itself.
  std::string remembered = get_last_folder("open");
  if (!remembered.empty() && fs::is_directory(remembered)) {
    try {
      dialog->set_initial_folder(Gio::File::create_for_path(remembered));
    } catch (...) {
    }
  }
  dialog->select_folder(
      *this, [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
        try {
          auto file = dialog->select_folder_finish(result);
          if (!file)
            return;
          // Remember the parent directory the user was
          // browsing, not the .curvz folder itself.
          std::string chosen = file->get_path();
          std::string parent = fs::path(chosen).parent_path().string();
          set_last_folder("open", parent);
          auto project = CurvzProject::open(chosen);
          if (!project) {
            LOG_ERROR("on_open: open failed");
            return;
          }
          load_project(std::move(project));
        } catch (...) {
        }
      });
}


void MainWindow::on_save() {
  if (!m_project)
    return;
  if (m_project->directory.empty()) {
    on_save_as();
    return;
  }
  // If a drag is in-flight, defer 100ms and retry — saves mid-drag corrupt node
  // data.
  if (m_canvas.is_dragging()) {
    Glib::signal_timeout().connect_once([this]() { on_save(); }, 100);
    return;
  }
  if (m_project->save()) {
    update_title();
    LOG_INFO("on_save: saved '{}'", m_project->directory);
  } else {
    LOG_ERROR("on_save: failed");
  }
}


// s246 m1 — script-side counterpart to on_save. See the design block
// in MainWindow.hpp at ScriptSaveResult / script_save_project for the
// enum's contract.
//
// The body mirrors on_save's four-case structure exactly, except:
//
//   - The "empty directory" case here returns NoPath instead of
//     falling through to on_save_as. CANON's headless-verb-singletons
//     rule forbids scripts from summoning modals, so the picker
//     fallthrough is converted to a structured refusal that the
//     Scriptable surfaces as "use 'proj save_as <path>' first (s247)".
//
//   - The "drag in flight" case here returns Dragging instead of
//     scheduling a deferred retry. A Scriptable verb returning ok
//     synchronously while a deferred save fires later would lie about
//     its result; let the script re-issue cleanly instead.
//
// The success and IO-failure paths are identical to on_save's — same
// update_title() call, same LOG_INFO / LOG_ERROR lines (with a
// "proj save" prefix so log readers can tell which entry point fired).
MainWindow::ScriptSaveResult MainWindow::script_save_project() {
  if (!m_project) {
    return ScriptSaveResult::NoProject;
  }
  if (m_project->directory.empty()) {
    return ScriptSaveResult::NoPath;
  }
  if (m_canvas.is_dragging()) {
    return ScriptSaveResult::Dragging;
  }
  if (m_project->save()) {
    update_title();
    LOG_INFO("proj save: saved '{}'", m_project->directory);
    return ScriptSaveResult::Ok;
  }
  LOG_ERROR("proj save: failed");
  return ScriptSaveResult::IoFailed;
}


// s246 m1 — script-side accessor for the project's current directory.
// Returns empty string if no project is loaded OR the project has
// never been saved. The empty / non-empty distinction is what
// ProjScriptable's `path` and `has_path` queries surface to the DSL.
std::string MainWindow::script_project_path() const {
  if (!m_project) return std::string();
  return m_project->directory;
}


// s247 m1 — script-side counterpart to on_save_as. See the design
// block in MainWindow.hpp at ScriptSaveAsResult / script_save_project_as
// for the enum's contract.
//
// The `path` argument is assumed pre-validated by the caller through
// curvz::scripting::path_is_safe. This helper does NOT re-check —
// separation of concerns: path containment is the Scriptable's
// pre-flight (it owns the DSL-facing refusal string when the path is
// outside $HOME/$TMPDIR); project-state checks are this helper's.
//
// The body mirrors script_save_project's three structural checks
// (project, drag, write success) — minus NoPath (the path IS the
// argument), minus the NoPath/save-as picker bridge (the script
// supplied the path directly, leapfrogging the picker per CANON's
// headless-verb-singletons rule). Delegates to do_save_as() for the
// actual writer work; do_save_as returns bool as of s247 so this
// helper can map the outcome to Ok or IoFailed honestly.
MainWindow::ScriptSaveAsResult
MainWindow::script_save_project_as(const std::string& path) {
  if (!m_project) {
    return ScriptSaveAsResult::NoProject;
  }
  if (m_canvas.is_dragging()) {
    return ScriptSaveAsResult::Dragging;
  }
  // do_save_as assigns m_project->directory, calls CurvzProject::save(),
  // and on success calls save_config() + update_title(). It returns
  // false iff the save call itself failed (LOG_ERROR was already
  // emitted by do_save_as in that case).
  if (do_save_as(path)) {
    LOG_INFO("proj save_as: saved '{}'", path);
    return ScriptSaveAsResult::Ok;
  }
  // do_save_as already emitted its own LOG_ERROR. We don't log a
  // second time — one entry per failure is enough, and the
  // do_save_as prefix is the canonical one.
  return ScriptSaveAsResult::IoFailed;
}


// s248 m1 — script-side counterpart to on_close_project. See the
// design block in MainWindow.hpp at ScriptCloseResult /
// script_close_project for the enum's contract.
//
// The body mirrors what on_close_project's check_unsaved_then guard
// does (project loaded? dirty?) plus the standard drag check (same
// as save / save_as) — except every refusal becomes a structured
// enum value the Scriptable surfaces as a DSL error, never a modal.
// The teardown work itself lives in do_close_project() so both
// callers share a single source of truth (same pump-at-the-seam
// pattern as do_save_as).
//
// The ordering of refusals matters in only one direction: NoProject
// MUST come before the can_undo() check because m_history is a value
// member of MainWindow whose lifetime is independent of m_project's.
// Although do_close_project resets m_history alongside m_project so
// the normal post-close state is "no project, clean history," nothing
// structurally guarantees the invariant — and NoProject is the
// structurally honest answer whenever m_project is null. The drag
// check after NoProject is order-insensitive (is_dragging only reads
// canvas flags, never touches project state).
MainWindow::ScriptCloseResult
MainWindow::script_close_project() {
  if (!m_project) {
    return ScriptCloseResult::NoProject;
  }
  if (m_canvas.is_dragging()) {
    return ScriptCloseResult::Dragging;
  }
  if (m_history.can_undo()) {
    // The GUI's check_unsaved_then prompts Save/Discard/Cancel here.
    // Scripts can't summon modals; refuse instead and let the script
    // author make the save/discard choice explicit.
    return ScriptCloseResult::Dirty;
  }
  // All preconditions cleared. do_close_project does the teardown
  // (panels, history, config file, title) and logs the standard
  // "Project closed" line. Nothing more to do here — the close
  // is unconditional once preconditions hold.
  do_close_project();
  LOG_INFO("proj close: project closed");
  return ScriptCloseResult::Ok;
}


// s248 m1 — script-side dirty signal. Uses the same proxy
// on_close_project's check_unsaved_then uses (m_history.can_undo()).
// See the design block in MainWindow.hpp at script_project_dirty.
//
// Two-clause body: false when no project (nothing to be dirty
// about), otherwise the undo-stack proxy. When a true project-level
// dirty bit on CurvzProject lands (backlog), this body changes to
// read that bit; the public contract is unchanged.
bool MainWindow::script_project_dirty() const {
  if (!m_project) return false;
  return m_history.can_undo();
}


// s249 m1 — script-side counterpart to on_open. See the design
// block in MainWindow.hpp at ScriptLoadResult / script_load_project
// for the enum's contract.
//
// The `path` argument is assumed pre-validated by the caller through
// curvz::scripting::path_is_safe. This helper does NOT re-check —
// separation of concerns matches save_as exactly: path containment
// is the Scriptable's pre-flight; project-state + I/O is this
// helper's.
//
// Body — drag check, dirty check, open, swap:
//
//   - Drag check first. Same posture as every project-lifecycle
//     helper. Note: with no project loaded the drag check is
//     trivially false (no canvas content to drag), so this only
//     meaningfully fires when load is REPLACING an existing
//     project. Still wired for symmetry; doesn't hurt.
//
//   - Dirty check second. STRICTER than on_open (which does not
//     check_unsaved_then before replacing the project — arguably
//     a GUI bug, logged as separate backlog item). The script-
//     side stricter posture is deliberate: GUI replacement is
//     visually obvious (title bar changes, panels redraw); a
//     script silently destroying mid-automation is not. See
//     ProjScriptable.hpp's verb-surface block at `load` for the
//     full rationale.
//
//   - CurvzProject::open(path) — same call on_open's lambda
//     makes after the file dialog returns. Null means "directory
//     missing, not a .curvz, or parse failed" — open() doesn't
//     distinguish these today. A single OpenFailed bucket matches
//     the underlying surface honestly; when open() grows
//     distinguishable error returns, the enum (and this helper)
//     can split without breaking the existing Ok path.
//
//   - load_project(std::move(project)) — same orchestrator on_open
//     calls. This is the PUMP: panel sync, history reset,
//     recent-projects bump, config save, title refresh, LOG_INFO
//     line. We don't need a do_load_project lift like s247's
//     do_save_as or s248's do_close_project because the pump
//     already exists at the helper-orchestrator boundary — the
//     GUI path also goes through load_project, so the single
//     source of truth is already in place. This is the same
//     pump-at-the-seam discipline; the seam just happens to be
//     one layer deeper than the do_-lift cases.
MainWindow::ScriptLoadResult
MainWindow::script_load_project(const std::string& path) {
  if (m_canvas.is_dragging()) {
    return ScriptLoadResult::Dragging;
  }
  if (m_history.can_undo()) {
    // The script-side stricter posture (see header). on_open
    // silently replaces; we refuse and let the script author make
    // the save/discard choice explicit. A future force_load verb
    // (s250+) gives the discard path.
    return ScriptLoadResult::Dirty;
  }
  auto project = CurvzProject::open(path);
  if (!project) {
    // Mirrors on_open's LOG_ERROR path. Conflates the three
    // disk-side failure causes (see header for the catalogue).
    LOG_ERROR("proj load: open failed for '{}'", path);
    return ScriptLoadResult::OpenFailed;
  }
  load_project(std::move(project));
  // load_project itself emits the canonical "Project loaded: '{}'"
  // LOG_INFO line and the recent-projects bump. No additional log
  // here — one entry per success is enough, and load_project's
  // line is the right one (matches on_open's success path).
  return ScriptLoadResult::Ok;
}


// s250 m1 — script-side counterpart to on_new_project. See the design
// block in MainWindow.hpp at ScriptNewResult / script_new_project for
// the enum's contract.
//
// The `path` argument is assumed pre-validated by the caller through
// curvz::scripting::path_is_safe. This helper does NOT re-check —
// separation of concerns matches save_as and load exactly: path
// containment is the Scriptable's pre-flight; project-state + disk
// state + I/O is this helper's.
//
// Body — drag check, dirty check, target-exists check, create, swap:
//
//   - Drag check first. Same posture as every project-lifecycle
//     helper. As with load, this only meaningfully fires when new
//     is REPLACING an existing project (no project loaded means no
//     canvas drag is possible). Still wired for symmetry.
//
//   - Dirty check second. STRICTER than on_new_project (which
//     routes through check_unsaved_then — a modal the script can't
//     summon). The script-side structural refusal is the modal's
//     equivalent for non-interactive callers. Same posture as the
//     load Dirty refusal; see ProjScriptable.hpp's verb-surface
//     block at `new` for the full rationale.
//
//   - Target-exists check third. STRICTER than on_new_project,
//     which silently overwrites an existing target (create_empty
//     calls save() which uses create_directories — a no-op on
//     existing dir — then writes project.json over whatever's
//     there). This is the NEW refusal not present in load
//     (load needs the target to EXIST; new needs it NOT to). See
//     header for the rationale and the future force_new opt-in
//     path. fs::exists is the natural primitive here; we don't
//     try to distinguish "exists as a directory" vs "exists as a
//     file" because either case is a target-already-exists refusal
//     from the script's perspective.
//
//   - CurvzProject::create_empty(path) — same call on_new_project's
//     lambda makes. Null means "directory creation failed or the
//     immediate save() inside create_empty failed" — create_empty
//     itself doesn't distinguish these today. A single CreateFailed
//     bucket matches the underlying surface honestly; when
//     create_empty grows distinguishable error returns, the enum
//     (and this helper) can split without breaking the existing Ok
//     path.
//
//   - load_project(std::move(project)) — same orchestrator
//     on_new_project calls. This is the PUMP: panel sync, history
//     reset, recent-projects bump, config save, title refresh,
//     LOG_INFO line. No do_new_project lift needed — the pump
//     already exists at the helper-orchestrator boundary, same as
//     load (s249 m1). Pump-at-the-seam discipline, applied one
//     layer deeper than the s247/s248 do_-lift cases.
MainWindow::ScriptNewResult
MainWindow::script_new_project(const std::string& path) {
  if (m_canvas.is_dragging()) {
    return ScriptNewResult::Dragging;
  }
  if (m_history.can_undo()) {
    // STRICTER than on_new_project's check_unsaved_then. See header.
    return ScriptNewResult::Dirty;
  }
  if (fs::exists(path)) {
    // STRICTER than on_new_project's silent overwrite. The GUI calls
    // create_empty against whatever the picker returns, clobbering
    // any existing project.json / swatches.json / styles.json /
    // themes.json. Script side refuses; a future force_new verb
    // (s250+) gives the overwrite path. See header for the
    // rationale.
    return ScriptNewResult::TargetExists;
  }
  auto project = CurvzProject::create_empty(path);
  if (!project) {
    // Mirrors on_new_project's LOG_ERROR path. Conflates directory-
    // creation failure and the immediate save()-inside-create_empty
    // failure (see header for the catalogue).
    LOG_ERROR("proj new: create_empty failed for '{}'", path);
    return ScriptNewResult::CreateFailed;
  }
  load_project(std::move(project));
  // load_project emits the canonical "Project loaded: '{}'"
  // LOG_INFO line and the recent-projects bump. No additional log
  // here — one entry per success is enough, and load_project's
  // line is the right one (matches on_new_project's success path,
  // which also relies on load_project's logging without adding its
  // own).
  return ScriptNewResult::Ok;
}


// s251 m1 — script_export_svg helper. Sibling shape to the other
// script_* helpers in this file: takes an already-path-is-safe-vetted
// path, returns an enum naming the outcome. Body wraps
// SvgWriter::write_svg_file() with the same project-state pre-checks
// the GUI's Export Documents dialog applies before its own writer call.
//
// Three pre-check refusal branches before the I/O:
//   1. m_project is null         -> ScriptExportSvgResult::NoProject
//   2. active_doc()  is nullptr  -> ScriptExportSvgResult::NoActiveDoc
//   3. write_svg_file returns false -> ScriptExportSvgResult::IoFailed
//
// Happy path: write succeeded; LOG_INFO line emitted matching the
// pattern other writer call sites use. No project-state mutation —
// export produces a side artefact, m_project is unchanged. No
// update_title() (the title bar reflects project saved-ness, not
// export activity; an export to /tmp shouldn't change the dirty
// indicator). No save_config() (the export doesn't change the
// reopen-this-project-next-launch pointer).
//
// See MainWindow.hpp's ScriptExportSvgResult block for the enum
// contract; see ExportScriptable.cpp's invoke() for the
// Scriptable-side enum-to-error mapping.
MainWindow::ScriptExportSvgResult
MainWindow::script_export_svg(const std::string& path) {
  if (!m_project) {
    return ScriptExportSvgResult::NoProject;
  }
  auto* doc = m_project->active_doc();
  if (!doc) {
    return ScriptExportSvgResult::NoActiveDoc;
  }
  // SvgWriter::write_svg_file logs its own LOG_ERROR on open-fail
  // (cannot open '<path>' for writing); the success-side log line
  // here is our own — distinct LOG_INFO so script-driven exports are
  // identifiable in the log apart from GUI Export Documents writes
  // (which log through ExportDialog's own info lines).
  if (!write_svg_file(*doc, path)) {
    return ScriptExportSvgResult::IoFailed;
  }
  LOG_INFO("export svg: wrote '{}'", path);
  return ScriptExportSvgResult::Ok;
}


// s252 m2 — script_export_png helper. Sibling shape to
// script_export_svg: takes a path that's been path_is_safe-vetted by
// the Scriptable, plus the longest-side size (assumed > 0 by the
// Scriptable's argument-shape gate). Returns an enum naming the
// outcome.
//
// Three pre-check refusal branches before the I/O:
//   1. m_project is null         -> ScriptExportPngResult::NoProject
//   2. active_doc()  is nullptr  -> ScriptExportPngResult::NoActiveDoc
//   3. export_png_sized returns false -> ScriptExportPngResult::IoFailed
//
// Width × height calculation from longest-side size. The active
// doc's canvas dimensions tell us which side is "longest":
//   - If canvas_width >= canvas_height: width is the dominant axis,
//     out_w = size, out_h scaled by height/width ratio.
//   - Otherwise: height is dominant, out_h = size, out_w scaled.
// Square documents land in the >= branch (out_w == out_h == size).
//
// Mirrors ExportDialog.cpp:911-922's fit_width / fit_height
// arithmetic. The DIFFERENCE: the GUI dialog picks fit-side from
// the user's choice of dimension field (width-locked or
// height-locked); the script picks fit-side from the doc's aspect
// ratio (longest side always wins). Reason: the script's mental
// model is "I want an N-pixel image"; the GUI's mental model is
// "I want an N-pixel WIDTH" or "N-pixel HEIGHT". The dialog needs
// the user's choice because both produce different aspect-
// preserved outcomes when the doc isn't square; the script's
// single-size convention picks the longest-side because that's the
// "fit in an N×N box" answer everyone means by "give me a 256-pixel
// image".
//
// Defensive against pathological canvas_height / canvas_width zero
// values: CurvzDocument guarantees positive canvas dimensions by
// construction, but if either turns up as zero (e.g. from a corrupt
// document somehow loaded), the std::max(1, ...) floor prevents the
// arithmetic from producing a 0-pixel dimension that
// export_png_sized would reject with IoFailed. The floor never
// changes a non-degenerate case (a 1-pixel result from a
// nondegenerate doc means the user asked for a size so small the
// scaled dimension truncated to under one pixel, which is also
// fine — they asked for tiny, they get tiny).
//
// Happy path: write succeeded; LOG_INFO line emitted matching
// svg's pattern but with the resolved WxH dimensions for easy
// log-side audit. No project-state mutation — export is a side
// artefact. No update_title(). No save_config(). All deliberately
// absent for the same reasons script_export_svg lists.
//
// See MainWindow.hpp's ScriptExportPngResult block for the enum
// contract; see ExportScriptable.cpp's invoke() for the
// Scriptable-side enum-to-error mapping.
MainWindow::ScriptExportPngResult
MainWindow::script_export_png(const std::string& path, int size) {
  if (!m_project) {
    return ScriptExportPngResult::NoProject;
  }
  auto* doc = m_project->active_doc();
  if (!doc) {
    return ScriptExportPngResult::NoActiveDoc;
  }

  // Compute output dimensions from longest-side size + doc aspect
  // ratio. Mirrors ExportDialog.cpp's fit-side arithmetic with the
  // fit-side picked from the doc's aspect ratio rather than user
  // choice.
  const int cw = doc->canvas_width();
  const int ch = doc->canvas_height();
  int out_w = 1, out_h = 1;
  if (cw >= ch) {
    // Width is the longest (or equal) side.
    out_w = size;
    out_h = std::max(1, (int)std::round(
        size * (double)ch / (double)cw));
  } else {
    // Height is the longest side.
    out_h = size;
    out_w = std::max(1, (int)std::round(
        size * (double)cw / (double)ch));
  }

  // export_png_sized logs its own LOG_DEBUG on success and
  // LOG_ERROR on failure (cairo write failed, or invalid
  // dimensions which we've pre-filtered above with the max(1,...)
  // floor). The success-side LOG_INFO here is distinct so
  // script-driven exports are identifiable in the log apart from
  // GUI Export Documents writes (which log through ExportDialog's
  // own lines) and from the writer's own debug-level line.
  if (!export_png_sized(*doc, path, out_w, out_h)) {
    return ScriptExportPngResult::IoFailed;
  }
  LOG_INFO("export png: wrote '{}' {}x{}", path, out_w, out_h);
  return ScriptExportPngResult::Ok;
}


void MainWindow::on_save_as() {
  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Save Project As — enter project name");
  // Pre-fill with current project name if we have one
  if (!m_project->directory.empty()) {
    std::string cur = fs::path(m_project->directory).stem().string();
    dialog->set_initial_name(cur);
  } else {
    dialog->set_initial_name("MyIcons");
  }
  dialog->save(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
    try {
      auto file = dialog->save_finish(result);
      if (!file)
        return;
      std::string path = file->get_path();
      // Strip extension, append .curvz
      auto dot = path.rfind('.');
      auto sep = path.rfind('/');
      if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
        path = path.substr(0, dot);
      std::string final_path = path + ".curvz";

      // Confirm overwrite if file exists
      if (fs::exists(final_path)) {
        curvz::utils::show_confirm(
            *this,
            "\"" + fs::path(final_path).filename().string() +
                "\" already exists.\nDo you want to replace it?",
            "The existing file will be replaced.", {"Cancel", "Replace"},
            /*default_button=*/0, /*cancel_button=*/0,
            [this, final_path](int btn) {
              if (btn == 1)
                do_save_as(final_path);
            });
      } else {
        do_save_as(final_path);
      }
    } catch (...) {
    }
  });
}


// s147 m3 — on_show_themes removed. ThemesPanel in the right pane's
// Content group is the canonical surface; there is no dialog version
// any longer. Library mutations push commands onto m_history via the
// panel; apply is non-undoable per design (same as the dialog used
// to be).

// File ▸ Export… handler. Self-managed dialog: constructed with
// `new`, deletes itself via signal_close_request → idle delete-this.
// The done_cb is just for logging; the dialog handles its own
// success confirmation internally.
void MainWindow::on_export() {
  if (!m_project) {
    LOG_INFO("on_export: no project, ignoring");
    return;
  }
  if (m_project->documents.empty()) {
    LOG_INFO("on_export: project has no documents, ignoring");
    return;
  }
  new ExportDialog(*this, *m_project,
      [](bool success, std::string dir) {
        if (success)
          LOG_INFO("on_export: export complete → '{}'", dir);
        else
          LOG_INFO("on_export: dialog closed without export");
      });
}


// Import SVG — preserves original fill/stroke colors.
void MainWindow::on_import_svg() {
  if (!m_project)
    return;

  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Import SVG");

  auto filter = Gtk::FileFilter::create();
  filter->set_name("SVG files");
  filter->add_mime_type("image/svg+xml");
  filter->add_pattern("*.svg");
  auto filters = Gio::ListStore<Gtk::FileFilter>::create();
  filters->append(filter);
  dialog->set_filters(filters);

  dialog->open(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
    try {
      auto file = dialog->open_finish(result);
      if (!file)
        return;
      import_svg_impl(file->get_path(),
                      /*force_currentcolor=*/false,
                      /*normalize_to_1000=*/false);
    } catch (...) {
      LOG_ERROR("on_import_svg: exception during import");
    }
  });
}


// Import as Icon — converts all Solid fills/strokes to currentColor.
void MainWindow::on_import_svg_as_icon() {
  if (!m_project)
    return;

  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Import as Icon");

  auto filter = Gtk::FileFilter::create();
  filter->set_name("SVG files");
  filter->add_mime_type("image/svg+xml");
  filter->add_pattern("*.svg");
  auto filters = Gio::ListStore<Gtk::FileFilter>::create();
  filters->append(filter);
  dialog->set_filters(filters);

  dialog->open(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
    try {
      auto file = dialog->open_finish(result);
      if (!file)
        return;
      import_svg_impl(file->get_path(),
                      /*force_currentcolor=*/true,
                      /*normalize_to_1000=*/true);
    } catch (...) {
      LOG_ERROR("on_import_svg_as_icon: exception during import");
    }
  });
}


void MainWindow::on_preview_icon(const std::string &path) {
  if (!m_project)
    return;

  if (!m_preview_active) {
    m_preview_saved_index = m_project->active_doc_index;
    m_preview_active = true;
  }

  auto doc = Curvz::parse_svg_file(path);
  if (!doc) {
    LOG_WARN("on_preview_icon: failed to parse '{}'", path);
    return;
  }

  m_preview_doc = std::move(doc);
  m_canvas.set_document(m_preview_doc.get());
  m_canvas.queue_draw();

  std::string name = fs::path(path).stem().string();
  m_statusbar.set_mode("Previewing: " + name + "  (double-click to copy)");
}


void MainWindow::on_copy_icon(const std::string &path) {
  if (!m_project)
    return;

  std::string name = fs::path(path).stem().string();
  curvz::utils::show_confirm(
      *this, "Copy icon to project?",
      "Add '" + name + "' as a new document in this project.",
      {"Copy", "Cancel"},
      /*default_button=*/0, /*cancel_button=*/1, [this, path](int response) {
        if (response == 0) {
          exit_preview_mode();
          import_svg_as_doc(path);
        }
      });
}


void MainWindow::on_quit() {
  m_closing = true;
  // Save project (when one exists with a directory) and always flush
  // app-level config so last_folders / outline_mode / window state
  // persist across runs regardless of project state. (s125 m1e — the
  // unconditional save_config() landed when save_config itself was
  // hardened against the no-project case.)
  if (m_project && !m_project->directory.empty())
    m_project->save();
  save_config();
  // Close the window — the application will quit when all windows are closed
  close();
}


// ── on_place_image
// ────────────────────────────────────────────────────────────
void MainWindow::on_place_image() {
  if (!m_project)
    return;

  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Place Image");

  auto filter = Gtk::FileFilter::create();
  filter->set_name("Image files");
  filter->add_mime_type("image/png");
  filter->add_mime_type("image/jpeg");
  filter->add_mime_type("image/gif");
  filter->add_mime_type("image/webp");
  filter->add_pattern("*.png");
  filter->add_pattern("*.jpg");
  filter->add_pattern("*.jpeg");
  filter->add_pattern("*.gif");
  filter->add_pattern("*.webp");
  auto filters = Gio::ListStore<Gtk::FileFilter>::create();
  filters->append(filter);
  dialog->set_filters(filters);

  // s125 m1e: re-open at the last-used folder for this purpose.
  std::string remembered = get_last_folder("place-image");
  if (!remembered.empty() && fs::is_directory(remembered)) {
    try {
      dialog->set_initial_folder(Gio::File::create_for_path(remembered));
    } catch (...) {
    }
  }

  dialog->open(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
    try {
      auto file = dialog->open_finish(result);
      if (!file)
        return;
      std::string chosen = file->get_path();
      set_last_folder("place-image", fs::path(chosen).parent_path().string());
      m_canvas.import_image_to_canvas(chosen,
                                      /*fit_canvas_to_image=*/false);
    } catch (...) {
    }
  });
}


// ── on_open_image
// ──────────────────────────────────────────────────── s125 m1d (was m1c
// on_place_image_as_doc, renamed). Variant of on_place_image that resizes
// the document canvas to the image's pixel dimensions and places it at
// (0, 0) at 1:1. Picker config is duplicated from on_place_image; if a
// third image-picker caller appears, lift the filter setup into a helper.
void MainWindow::on_open_image() {
  if (!m_project)
    return;

  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Open Image");

  auto filter = Gtk::FileFilter::create();
  filter->set_name("Image files");
  filter->add_mime_type("image/png");
  filter->add_mime_type("image/jpeg");
  filter->add_mime_type("image/gif");
  filter->add_mime_type("image/webp");
  filter->add_pattern("*.png");
  filter->add_pattern("*.jpg");
  filter->add_pattern("*.jpeg");
  filter->add_pattern("*.gif");
  filter->add_pattern("*.webp");
  auto filters = Gio::ListStore<Gtk::FileFilter>::create();
  filters->append(filter);
  dialog->set_filters(filters);

  // s125 m1e: re-open at the last-used folder for this purpose. Note this
  // is keyed separately from "place-image" — Open Image and Place Image
  // are typically used from different directories (screenshots vs. asset
  // library), so cross-pollution isn't desirable.
  std::string remembered = get_last_folder("open-image");
  if (!remembered.empty() && fs::is_directory(remembered)) {
    try {
      dialog->set_initial_folder(Gio::File::create_for_path(remembered));
    } catch (...) {
    }
  }

  dialog->open(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
    try {
      auto file = dialog->open_finish(result);
      if (!file)
        return;
      std::string chosen = file->get_path();
      set_last_folder("open-image", fs::path(chosen).parent_path().string());

      // s125 m1f: Open Image creates a new document in the project named
      // after the image's filename stem, then routes the import into it.
      // Earlier versions imported into the active doc, which conflated
      // "drop reference into existing doc" (Place) with "open as document"
      // (Open). Pattern mirrors import_svg_as_doc / signal_add_doc:
      //   1. Build minimal CurvzDocument (filename + guide layer +
      //      default user layer)
      //   2. Append + flip active_doc_index
      //   3. update_all_panels() so m_canvas.m_doc points at the new doc
      //   4. import_image_to_canvas(fit_canvas=true) does the canvas
      //      resize + image placement on the now-active new doc
      auto new_doc = std::make_unique<CurvzDocument>();

      std::string stem = fs::path(chosen).stem().string();
      if (stem.empty())
        stem = "Image";
      // Collision-resistant filename — matches import_svg_as_doc,
      // signal_add_doc, etc. The .svg extension is the project's
      // canonical doc format, regardless of the source raster type.
      std::string fname = stem + ".svg";
      int suffix = 2;
      while (std::any_of(m_project->documents.begin(),
                         m_project->documents.end(),
                         [&](const auto &d) { return d->filename == fname; }))
        fname = stem + std::to_string(suffix++) + ".svg";
      new_doc->filename = fname;

      new_doc->ensure_guide_layer();
      auto layer = std::make_unique<SceneNode>();
      layer->type = SceneNode::Type::Layer;
      layer->internal_id = generate_internal_id();
      layer->name = new_doc->next_default_name(CurvzDocument::NameKind::Layer);
      layer->visible = true;
      new_doc->layers.insert(new_doc->layers.begin(), std::move(layer));
      new_doc->active_layer_index = 0;

      m_project->documents.push_back(std::move(new_doc));
      m_project->active_doc_index = (int)m_project->documents.size() - 1;

      // Switch m_canvas.m_doc to the new doc before import — import_
      // image_to_canvas mutates m_canvas's current doc, so the order
      // here is load-bearing.
      update_all_panels();

      // Now do the canvas resize + image placement on the new doc.
      m_canvas.import_image_to_canvas(chosen,
                                      /*fit_canvas_to_image=*/true);

      // Persist. import_image_to_canvas emits signal_doc_changed which
      // schedules a save, but a direct save here is faster and matches
      // the import_svg_as_doc / signal_add_doc pattern.
      m_project->save();
      LOG_INFO("on_open_image: opened '{}' as new doc '{}'", chosen, fname);
    } catch (...) {
    }
  });
}

// ─────────────────────────────────────────────────────────────────────
// ━━━ edit ━━━ Edit menu verbs (Undo, Redo).
// ─────────────────────────────────────────────────────────────────────

void MainWindow::on_undo() {
  // s165 m3 — chrono trap. If a previous undo/redo fired within the
  // throttle window, drop this press. Protects against keyboard auto-repeat
  // and frantic Ctrl+Z spam outpacing the post-undo refresh pipeline; also
  // limits exposure to the dangling-pointer-in-queued-commands hazard
  // (queue cap=500 → eviction destroys SceneNodes still referenced by
  // raw pointers in other queued commands). 80ms is roughly the auto-repeat
  // boundary on most systems.
  using namespace std::chrono;
  constexpr auto kUndoThrottle = milliseconds(80);
  auto now = steady_clock::now();
  if (now - m_last_undo_redo_at < kUndoThrottle) {
    return;
  }
  m_last_undo_redo_at = now;
  // s165 m3 — re-entrancy guard. Rapid Ctrl+Z spam can deliver a second
  // undo while the first is still mid-flight (a queued signal or a paint
  // event handler can fire mid-refresh and route back into on_undo). The
  // guard early-returns the second call so only one undo is in flight at
  // a time. Insurance even when each individual undo is well-formed.
  if (m_undo_in_progress) {
    return;
  }
  if (!m_history.can_undo())
    return;
  m_undo_in_progress = true;
  // s165 m3 — bump the inspector's lambda generation BEFORE the destructive
  // mutation. Any signal_value_changed handler that GTK has queued from a
  // prior refresh_node fast-path set_value() will fire eventually; by
  // bumping the gen now we ensure those handlers see their captured `gen`
  // as stale and early-return, instead of dereferencing `obj`/`node_idx`
  // captures that may now point past the post-undo path's nodes.
  m_properties.invalidate_lambdas();
  if (!m_history.peek_undo()->preserves_selection())
    m_canvas.select_object(nullptr); // clear before nodes are destroyed
  m_history.undo();
  m_properties.reset_undo_coalesce();
  refresh_status_counts(); // s132 m2 — replaces hand-rolled loop
  m_canvas.queue_draw();
  m_gallery.refresh();
  m_styles.refresh(); // S80 m4c-2: bind/unbind undo updates indicator dot
  m_layers.refresh(); // s171 m1 — Add/DeleteLayerCommand mutate doc->layers
  refresh_inspector();
  m_undo_in_progress = false;
}


void MainWindow::on_redo() {
  // s165 m3 — chrono trap (shared timestamp with on_undo). See on_undo.
  using namespace std::chrono;
  constexpr auto kUndoThrottle = milliseconds(80);
  auto now = steady_clock::now();
  if (now - m_last_undo_redo_at < kUndoThrottle) {
    return;
  }
  m_last_undo_redo_at = now;
  // s165 m3 — re-entrancy guard. See on_undo for the rationale; same logic
  // here. Re-uses m_undo_in_progress (one flag covers both directions —
  // we never want either to re-enter the other either).
  if (m_undo_in_progress) {
    return;
  }
  if (!m_history.can_redo())
    return;
  m_undo_in_progress = true;
  m_properties.invalidate_lambdas();
  if (!m_history.peek_redo()->preserves_selection())
    m_canvas.select_object(nullptr); // clear before nodes are destroyed
  m_history.redo();
  m_properties.reset_undo_coalesce();
  refresh_status_counts(); // s132 m2 — replaces hand-rolled loop
  m_canvas.queue_draw();
  m_gallery.refresh();
  m_styles.refresh(); // S80 m4c-2: bind/unbind redo updates indicator dot
  m_layers.refresh(); // s171 m1 — Add/DeleteLayerCommand mutate doc->layers
  refresh_inspector();
  m_undo_in_progress = false;
}

// ─────────────────────────────────────────────────────────────────────
// ━━━ library ━━━ Library, Templates, and Themes save/manage/export verbs, plus the Canvas right-click Save-Selection-to-Library funnel.
// ─────────────────────────────────────────────────────────────────────

// Save active document as a user-global template. Opens SaveAsTemplateDialog;
// on accept checks for an existing bundle with the same (category, slug) and
// prompts Replace before calling templates::save().
void MainWindow::on_save_as_template() {
  if (!m_project)
    return;
  CurvzDocument *active = m_project->active_doc();
  if (!active) {
    LOG_WARN("on_save_as_template: no active document");
    return;
  }

  std::string suggested;
  if (!active->filename.empty()) {
    suggested = fs::path(active->filename).stem().string();
    for (char &c : suggested)
      if (c == '-' || c == '_')
        c = ' ';
  }

  m_save_as_template_dialog.show(
      *this, suggested, [this](templates::TemplateMeta meta) {
        auto do_save = [this, meta]() {
          if (!m_project)
            return;
          CurvzDocument *doc = m_project->active_doc();
          if (!doc) {
            LOG_WARN("on_save_as_template: no active document at save time");
            return;
          }
          std::string bundle;
          // m4: save() now writes both motif PNGs eagerly. Pass both motif
          // pairs from the project so the bundle is valid in either motif
          // immediately.
          if (templates::save(
                  *doc, meta, m_project->workspace_dark_r,
                  m_project->workspace_dark_g, m_project->workspace_dark_b,
                  m_project->artboard_dark_r, m_project->artboard_dark_g,
                  m_project->artboard_dark_b, m_project->workspace_light_r,
                  m_project->workspace_light_g, m_project->workspace_light_b,
                  m_project->artboard_light_r, m_project->artboard_light_g,
                  m_project->artboard_light_b, &bundle)) {
            LOG_INFO("on_save_as_template: saved '{}'", bundle);
          } else {
            LOG_ERROR("on_save_as_template: save failed");
          }
        };

        if (templates::user_bundle_exists(meta.category, meta.name)) {
          curvz::utils::show_confirm(
              *this,
              "A template named \"" + meta.name + "\" already exists in \"" +
                  meta.category + "\".\nDo you want to replace it?",
              "The existing template will be overwritten.",
              {"Cancel", "Replace"},
              /*default_button=*/0, /*cancel_button=*/0, [do_save](int btn) {
                if (btn == 1)
                  do_save();
              });
        } else {
          do_save();
        }
      });
}


// File → Manage Templates — opens the template browser. Enabled regardless
// of project state (templates are user-global).
void MainWindow::on_manage_templates() {
  // motif tag for thumbnail picking. When there's no project, we still need
  // to pick a motif somehow — fall back to Dark since the dialog itself
  // applies the motif class from the parent window separately. Templates
  // are user-global so the picked motif is purely cosmetic for thumb display.
  templates::MotifTag motif = templates::MotifTag::Dark;
  if (m_project && m_project->motif == Motif::Light) {
    motif = templates::MotifTag::Light;
  }
  m_manage_templates_dialog.show(*this, motif, [this]() {
    // No-op for now: NewDocumentDialog rescans templates on each show(),
    // so changes from the manager are picked up the next time the user
    // opens Add to Project or New Project. If a persistent picker UI ever
    // lives outside the dialog, wire its refresh here.
    LOG_INFO("on_manage_templates: templates changed");
  });
}


// ── on_request_save_selection_to_library
// s125 m1a: handler for Canvas::signal_request_save_to_library, fired
// from the right-click "Save to Library…" entry on a path/text/group.
// Opens a folder-picker pre-pointed at ~/.config/curvz/library, then
// routes the chosen directory to on_save_selection_to_library — which
// is the same destination the LibraryPanel + button takes via its own
// signal. Two callers, parallel picker setup; if a third caller appears,
// extract a shared helper.

void MainWindow::on_request_save_selection_to_library() {
  // Bail early if there's no selection — the canvas right-click branch
  // only shows the menu when something was hit, but the selection could
  // change via keyboard between popup and click in edge cases. Match
  // on_save_selection_to_library's empty-selection alert for symmetry.
  if (m_canvas.selection().empty()) {
    curvz::utils::show_alert(
        *this, "Nothing selected",
        "Select one or more objects before saving to the library.");
    return;
  }

  // Ensure user library dir exists before opening the picker, so the
  // picker has somewhere to land. Mirrors LibraryPanel::on_add_clicked.
  std::string lib_dir = Glib::get_user_config_dir() + "/curvz/library";
  std::error_code ec;
  fs::create_directories(lib_dir, ec);

  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Choose library category folder");

  try {
    auto initial = Gio::File::create_for_path(lib_dir);
    dialog->set_initial_folder(initial);
  } catch (...) {
  }

  dialog->select_folder(*this,
                        [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
                          try {
                            auto folder = dialog->select_folder_finish(result);
                            if (!folder)
                              return;
                            std::string dest_dir = folder->get_path();
                            on_save_selection_to_library(dest_dir);
                          } catch (...) {
                            // User cancelled — do nothing
                          }
                        });
}


// ── on_save_selection_to_library ─────────────────────────────────────────────
//
// s136 m4: orchestrator. Prompts the user for an item name, then routes to
// write_library_item to do the actual file write. Smart default for the prompt:
//
//   • Single object selected → pre-fill with the object's `name` (which may be
//     a designer-set string like "arrow" or an auto-generated default like
//     "Rectangle 3"). Either way, the user gets a meaningful starting point.
//   • Multi-selection → pre-fill empty (deliberate friction — saving a group
//     should be a deliberate naming act). If the user submits empty anyway,
//     a `group_<n>` fallback is generated, collision-aware.
//
// Collision handling: a typed name that clashes with an existing file in
// dest_dir is reported via show_confirm with Replace / Cancel. The fallback
// path (`item_<n>` / `group_<n>`) is the only place where automatic numeric
// suffixing happens.
//
// Why this is the chokepoint: both entry points (LibraryPanel + button and
// Canvas right-click → Save to Library…) funnel through here. The folder
// picker happens before this; the name prompt and write happen here.

void MainWindow::on_save_selection_to_library(const std::string &dest_dir) {
  if (!m_project)
    return;
  auto *doc = m_project->active_doc();
  if (!doc)
    return;

  const auto &sel = m_canvas.selection();
  if (sel.empty()) {
    // Inform user — no selection
    curvz::utils::show_alert(
        *this, "Nothing selected",
        "Select one or more objects before adding to the library.");
    return;
  }

  // ── Build the prompt's default value ───────────────────────────────────
  // Single selection → object name (may be empty / default-generated).
  // Multi-selection → empty, forcing the user to type or accept group_<n>.
  std::string default_name;
  if (sel.size() == 1 && sel[0]) {
    default_name = sel[0]->name;
  }

  // Prompt for the library item name. Spec-driven via curvz::utils::show_form.
  // The async callback fires once on dismissal with the user's input (or the
  // cancel-button index if dismissed). On Save, we either accept the typed
  // name, generate a fallback if it's empty, or surface a collision alert.
  std::vector<curvz::utils::FormField> fields = {
      {"name", "Name",
       curvz::utils::TextField{default_name, "Library item name"}}};

  std::vector<std::string> buttons = {"Cancel", "Save"};

  curvz::utils::show_form(
      *this, "Save to library",
      "Name this item — it will appear in the Library panel and on disk as "
      "<name>.svg.",
      fields, buttons,
      /*default_button=*/1, /*cancel_button=*/0,
      [this, dest_dir, multi = (sel.size() > 1)](
          int button_index,
          const std::map<std::string, curvz::utils::FormFieldValue> &values) {
        if (button_index != 1)
          return; // Cancel

        // Fetch typed name; trim whitespace.
        std::string name;
        auto it = values.find("name");
        if (it != values.end())
          name = it->second.text();
        // Trim
        auto ltrim = [](std::string s) {
          size_t i = 0;
          while (i < s.size() && std::isspace((unsigned char)s[i]))
            ++i;
          return s.substr(i);
        };
        auto rtrim = [](std::string s) {
          size_t i = s.size();
          while (i > 0 && std::isspace((unsigned char)s[i - 1]))
            --i;
          return s.substr(0, i);
        };
        name = rtrim(ltrim(name));

        namespace fs = std::filesystem;

        // Empty name → fallback to item_<n> (single) or group_<n> (multi).
        // Walk numeric suffixes until a free slot is found.
        if (name.empty()) {
          std::error_code ec;
          fs::create_directories(dest_dir, ec);
          const std::string prefix = multi ? "group_" : "item_";
          int n = 1;
          while (true) {
            std::string candidate = prefix + std::to_string(n);
            std::string path = dest_dir + "/" + candidate + ".svg";
            if (!fs::exists(path, ec)) {
              name = candidate;
              break;
            }
            ++n;
          }
          (void)write_library_item(dest_dir, name);
          return;
        }

        // Typed name → check collision; if present, ask Replace / Cancel.
        std::error_code ec;
        std::string candidate = dest_dir + "/" + name + ".svg";
        if (fs::exists(candidate, ec)) {
          curvz::utils::show_confirm(
              *this, "Item already exists",
              "An item named \"" + name +
                  "\" already exists in this category.\n\n"
                  "Replace it, or cancel and choose a different name?",
              {"Cancel", "Replace"}, /*default_button=*/0,
              /*cancel_button=*/0, [this, dest_dir, name](int btn) {
                if (btn != 1)
                  return; // Cancel
                std::error_code ec;
                fs::remove(dest_dir + "/" + name + ".svg", ec);
                (void)write_library_item(dest_dir, name);
              });
          return;
        }

        (void)write_library_item(dest_dir, name);
      });
}

// ─────────────────────────────────────────────────────────────────────
// ━━━ effects ━━━ Step-and-Repeat, Blend, and Warp (make/edit/release/flatten + apply-with-last popover paths).
// ─────────────────────────────────────────────────────────────────────

// ── on_step_repeat
// ────────────────────────────────────────────────────────────
// s154 m2: Was a Gtk::Window dialog; now a Gtk::Popover anchored to
// the SnR toolbar button. Three entry points share this method:
//   - Path → "Step and Repeat…" menu item
//   - Toolbar SnR right-click (signal_step_repeat_configure)
//   - Programmatic (e.g. Ctrl+Alt+something — no current binding)
// All open the popover for configuration. Toolbar SnR left-click takes
// a separate path through apply_step_repeat_with_last(), which uses
// the popover's persisted last values without opening UI.
void MainWindow::on_step_repeat() {
  if (!m_project)
    return;
  if (m_canvas.selection().empty())
    return;
  auto *doc = m_project->active_doc();
  const CanvasModel *cm = doc ? &doc->canvas : nullptr;

  // Seed pivot = selection bbox center. Used by the popover for the default
  // pivot AND for the mini preview's bbox rectangle scale.
  double bx = 0.0, by = 0.0, bw = 0.0, bh = 0.0;
  double cx = 0.0, cy = 0.0;
  if (!m_canvas.selection_bbox(bx, by, bw, bh)) {
    // Fallback: no usable bbox. Use doc origin as pivot, 100x100 preview.
    cx = 0.0;
    cy = 0.0;
    bw = 100.0;
    bh = 100.0;
  } else {
    cx = bx + bw * 0.5;
    cy = by + bh * 0.5;
  }

  // Pivot change: popover → canvas.  Live crosshair while popover is open.
  // NOTE: do NOT null the canvas→popover callback here on visible=false.
  // visible can flip false/true mid-session (e.g. user toggles rotate off
  // then on).  The callback is cleared in apply_cb and in signal_closed,
  // which fire only when the popover actually closes.
  auto pivot_cb = [this](double px, double py, bool visible) {
    m_canvas.set_step_repeat_preview(visible, px, py);
  };

  // Pivot change: canvas → popover.  User drags the crosshair on canvas →
  // popover spins/preview track live.
  m_canvas.set_step_repeat_pivot_callback([this](double px, double py) {
    m_step_repeat_popover.set_pivot_from_canvas(px, py);
  });

  // Apply: invoke extended step_repeat. Y-flip on offset matches the
  // existing sign convention (popover Y screen-down → canvas Y-up).
  // Pivot Y is passed as-is because PositionY csb already returns doc-Y-down
  // internal, and that is exactly what the new step_repeat overload expects.
  auto apply_cb = [this](StepRepeatPopover::Result r) {
    m_canvas.set_step_repeat_preview(false, 0.0, 0.0);
    m_canvas.set_step_repeat_pivot_callback(nullptr);
    m_canvas.step_repeat(r.copies, r.dx, -r.dy, r.rotate_enabled, r.angle_deg,
                         r.pivot_x, r.pivot_y);
  };

  // s154 m2: anchor to the SnR toolbar button. The popover repoints
  // its parent on each show() if needed (idempotent for the common
  // case where it's always the same button).
  m_step_repeat_popover.show(m_toolbar.step_repeat_button(),
                             cm, cx, cy, bw, bh,
                             std::move(pivot_cb), std::move(apply_cb));
}


// ── apply_step_repeat_with_last
// ────────────────────────────────────────────
// s154 m2: Toolbar SnR left-click path. No UI: applies the popover's
// persisted last values (or hardcoded defaults if the popover has
// never been opened this session) directly to the current selection.
// The user must have used the popover at least once for the values
// to be meaningful — but the constructor defaults (3 copies, 20px
// right, no rotate) produce a sensible result even on first click.
void MainWindow::apply_step_repeat_with_last() {
  if (!m_project) return;
  if (m_canvas.selection().empty()) return;
  auto* doc = m_project->active_doc();
  const CanvasModel* cm = doc ? &doc->canvas : nullptr;

  // Compute bbox centre — used as the pivot fallback for rotate mode
  // (matches on_step_repeat's seeding).
  double bx = 0.0, by = 0.0, bw = 0.0, bh = 0.0;
  double cx = 0.0, cy = 0.0;
  if (m_canvas.selection_bbox(bx, by, bw, bh)) {
    cx = bx + bw * 0.5;
    cy = by + bh * 0.5;
  }

  auto apply_cb = [this](StepRepeatPopover::Result r) {
    // No preview/pivot-callback teardown needed — they were never
    // installed for this entry point.
    m_canvas.step_repeat(r.copies, r.dx, -r.dy, r.rotate_enabled,
                         r.angle_deg, r.pivot_x, r.pivot_y);
  };

  m_step_repeat_popover.apply_with_last(cm, cx, cy, std::move(apply_cb));
}


// ── on_blend ───────────────────────────────────────────────────────────────
// s154 m3: Was BlendDialog (Gtk::Window); now BlendPopover anchored to the
// Blend toolbar button. Validates selection (exactly 2 Path nodes), reads
// node counts + current stroke widths, opens popover with those as seed
// values, forwards Result to Canvas::make_blend on OK. Deep preconditions
// (same parent, matching closed flag) are checked by Canvas::make_blend
// itself — if they fail it emits a user-visible error via m_sig_show_message.
//
// Three entry points share this method:
//   - Path → "Blend" menu item
//   - Toolbar Blend right-click (signal_blend_configure)
//   - apply_blend_with_last() falls through here when node counts mismatch
// All open the popover. Toolbar Blend left-click takes apply_blend_with_last
// directly, which only opens the popover on count-mismatch.
void MainWindow::on_blend() {
  if (!m_project)
    return;
  const auto &sel = m_canvas.selection();
  if (sel.size() != 2 || !sel[0] || sel[0]->type != SceneNode::Type::Path ||
      !sel[0]->path || !sel[1] || sel[1]->type != SceneNode::Type::Path ||
      !sel[1]->path) {
    // Action sensitivity should have prevented this, but be defensive.
    LOG_WARN("MainWindow::on_blend: selection not 2 Paths — ignoring");
    return;
  }

  auto *doc = m_project->active_doc();
  const CanvasModel *cm = doc ? &doc->canvas : nullptr;

  int a_count = (int)sel[0]->path->nodes.size();
  int b_count = (int)sel[1]->path->nodes.size();
  double a_w = sel[0]->stroke.width;
  double b_w = sel[1]->stroke.width;

  auto apply_cb = [this](BlendPopover::Result r) {
    m_canvas.make_blend(r.steps, r.reverse, r.stroke_w_override,
                        r.stroke_w_start, r.stroke_w_end);
  };

  // Equalize callback — runs on the current 2-path selection. Returns the
  // new matched node count on success, 0 on failure (which leaves the
  // popover's warning banner up). After successful equalization the
  // popover clears its warning state and enables OK.
  auto equalize_cb = [this]() -> int {
    const auto &s = m_canvas.selection();
    if (s.size() != 2 || !s[0] || !s[1] || !s[0]->path || !s[1]->path)
      return 0;
    if (!m_canvas.equalize_blend_sources(s[0], s[1]))
      return 0;
    // Both sides are now the same count; either side will do.
    int n = (int)s[0]->path->nodes.size();
    // Redraw so the new nodes appear on canvas immediately.
    m_canvas.queue_draw();
    m_layers.refresh();
    return n;
  };

  m_blend_popover.show(m_toolbar.blend_button(),
                       cm, a_count, b_count, a_w, b_w,
                       std::move(apply_cb), std::move(equalize_cb));
}


// ── apply_blend_with_last
// ──────────────────────────────────────────────────
// s154 m3: Toolbar Blend left-click path. If node counts match, fires
// the persisted last-values directly through Canvas::make_blend.
// Otherwise the popover takes over (so the user sees the warning banner
// and Equalize button). Either way, all the validation + callback
// orchestration runs through BlendPopover::apply_with_last, which decides
// internally whether to short-circuit or fall through to show().
void MainWindow::apply_blend_with_last() {
  if (!m_project) return;
  const auto& sel = m_canvas.selection();
  if (sel.size() != 2 || !sel[0] || sel[0]->type != SceneNode::Type::Path ||
      !sel[0]->path || !sel[1] || sel[1]->type != SceneNode::Type::Path ||
      !sel[1]->path) {
    LOG_WARN("MainWindow::apply_blend_with_last: selection not 2 Paths — ignoring");
    return;
  }

  auto* doc = m_project->active_doc();
  const CanvasModel* cm = doc ? &doc->canvas : nullptr;

  int a_count = (int)sel[0]->path->nodes.size();
  int b_count = (int)sel[1]->path->nodes.size();
  double a_w = sel[0]->stroke.width;
  double b_w = sel[1]->stroke.width;

  auto apply_cb = [this](BlendPopover::Result r) {
    m_canvas.make_blend(r.steps, r.reverse, r.stroke_w_override,
                        r.stroke_w_start, r.stroke_w_end);
  };

  auto equalize_cb = [this]() -> int {
    const auto& s = m_canvas.selection();
    if (s.size() != 2 || !s[0] || !s[1] || !s[0]->path || !s[1]->path)
      return 0;
    if (!m_canvas.equalize_blend_sources(s[0], s[1]))
      return 0;
    int n = (int)s[0]->path->nodes.size();
    m_canvas.queue_draw();
    m_layers.refresh();
    return n;
  };

  m_blend_popover.apply_with_last(m_toolbar.blend_button(),
                                  cm, a_count, b_count, a_w, b_w,
                                  std::move(apply_cb),
                                  std::move(equalize_cb));
}


// ── on_warp_make ────────────────────────────────────────────────────────────
// s146 m3: dialog-free flow. The user's preferred warp shape lives in the
// inspector's Application ▸ Warp subsection (AppPreferences). When they
// invoke Path ▸ Warp, we read those defaults, generate the envelope from
// the chosen preset against the source's bbox, build a Warp node, and
// push MakeWarpCommand. The Warp is selected on success; the inspector's
// Object ▸ Warp section then takes over for fine-tuning.
//
// No scratch+restore dance: there's nothing to "cancel" because there's
// no dialog session. If the user doesn't like the result, Ctrl+Z undoes
// the MakeWarpCommand atomically and restores the original source.
void MainWindow::on_warp_make() {
  if (!m_project || !m_project->active_doc())
    return;
  CurvzDocument *doc = m_project->active_doc();

  const auto &sel = m_canvas.selection();
  if (sel.size() != 1 || !sel[0]) {
    LOG_WARN("MainWindow::on_warp_make: selection not size-1, ignoring");
    return;
  }
  SceneNode *src = sel[0];
  if (src->type != SceneNode::Type::Path &&
      src->type != SceneNode::Type::Compound &&
      src->type != SceneNode::Type::Group) {
    LOG_WARN("MainWindow::on_warp_make: selection type not warpable");
    return;
  }

  // Find the source's parent and its index. Same walk as before — the
  // tree-walk logic is unchanged, only the post-walk action differs.
  int src_idx = -1;
  SceneNode *parent = nullptr;
  {
    std::function<bool(SceneNode *)> walk = [&](SceneNode *n) -> bool {
      if (!n)
        return false;
      for (int i = 0; i < (int)n->children.size(); ++i) {
        if (n->children[i].get() == src) {
          parent = n;
          src_idx = i;
          return true;
        }
        if (walk(n->children[i].get()))
          return true;
      }
      if (n->is_blend()) {
        if (walk(n->blend_source_a.get()))
          return true;
        if (walk(n->blend_source_b.get()))
          return true;
      }
      if (n->is_warp()) {
        if (walk(n->warp_source.get()))
          return true;
        if (walk(n->warp_glyph_cache.get()))
          return true;
        if (walk(n->warp_cache.get()))
          return true;
      }
      if (n->clip_shape && walk(n->clip_shape.get()))
        return true;
      return false;
    };
    for (auto &layer : doc->layers)
      if (walk(layer.get()))
        break;
  }
  if (!parent || src_idx < 0) {
    LOG_WARN("MainWindow::on_warp_make: could not locate source's parent");
    return;
  }

  // Read AppPreferences defaults (these are the inspector ▸ Application
  // ▸ Warp subsection's controls). Wave preset auto-bumps anchor counts
  // to >=3 so the wave is actually visible — same logic the dialog
  // used to enforce, applied here at the seam between defaults and
  // create.
  const auto &prefs = AppPreferences::instance();
  int top_n = std::clamp(prefs.warp_default_top_count(), 2, 4);
  int bot_n = std::clamp(prefs.warp_default_bot_count(), 2, 4);
  int preset = std::clamp(prefs.warp_default_preset(), 0, 7);
  int qual = std::clamp(prefs.warp_default_quality(), 1, 16);
  if (curvz::utils::warp_presets::requires_three_anchors(preset)) {
    if (top_n < 3)
      top_n = 3;
    if (bot_n < 3)
      bot_n = 3;
  }

  // Snapshot the original for the eventual undo restoration. This is
  // the "source_snap" that MakeWarpCommand stores so undo can put the
  // path back exactly as it was. Done BEFORE we mutate the tree.
  auto source_snap = clone_node(*src);
  int source_index = src_idx;

  // Build the Warp directly. Same setup the old scratch path used —
  // type, ids, name, source clone, dirty flags. The difference is we
  // populate envelope and quality up-front from the defaults, so the
  // first draw pass produces the user's chosen shape.
  auto warp = std::make_unique<SceneNode>();
  warp->type = SceneNode::Type::Warp;
  warp->id = m_canvas.mint_id();
  warp->internal_id = m_canvas.last_minted_iid();
  warp->name = doc->next_default_name(CurvzDocument::NameKind::Warp);
  warp->visible = true;
  warp->locked = false;
  warp->opacity = 1.0;
  warp->warp_source = clone_node(*src);
  warp->warp_glyph_cache_dirty = true;
  warp->warp_cache_dirty = true;
  warp->warp_quality = qual;
  // s147 m2: stamp preset provenance on the new warp so the inspector
  // dropdown shows the right name immediately, and so save/load
  // round-trips correctly. preset_idx, top/bot counts come from the
  // AppPreferences defaults; auto-bump for Wave already happened
  // upstream when we read top_n/bot_n from prefs.
  warp->warp_preset_idx = preset;
  warp->warp_top_count = top_n;
  warp->warp_bot_count = bot_n;

  // Replace source with warp temporarily to compute the bbox the
  // preset envelope is generated against. We need a real glyph_cache
  // for object_bbox_query, so build it now. Insert-then-bbox-then-
  // generate is the order — the envelope is sized to the source's
  // bbox, not the empty-warp's.
  parent->children.erase(parent->children.begin() + src_idx);
  parent->children.insert(parent->children.begin() + src_idx, std::move(warp));
  SceneNode *wp = parent->children[src_idx].get();

  m_canvas.rebuild_warp_caches(wp);
  double gbx = 0, gby = 0, gbw = 1, gbh = 1;
  m_canvas.object_bbox_query(*wp, gbx, gby, gbw, gbh, false);
  if (gbw < 1e-9)
    gbw = 1.0;
  if (gbh < 1e-9)
    gbh = 1.0;

  // Generate envelope from preset+counts+bbox using the lifted preset
  // pump (same code path the inspector's Object ▸ Warp section uses
  // when the user picks a preset).
  curvz::utils::generate_warp_preset(preset, gbx, gby, gbw, gbh, top_n, bot_n,
                                     wp->warp_env_top, wp->warp_env_bottom);
  wp->warp_cache_dirty = true;

  // Push MakeWarpCommand against the post-state. The command's execute()
  // re-runs cleanly on redo because warp_snap is a clone, not the live
  // pointer. source_snap restores the original on undo.
  auto warp_snap = clone_node(*wp);
  if (auto *hist = m_canvas.history()) {
    hist->push(std::make_unique<MakeWarpCommand>(
        m_canvas.project(), parent->internal_id,
        std::move(source_snap), source_index, std::move(warp_snap),
        src_idx));
  }

  // Select the new Warp so the inspector's Object ▸ Warp section binds
  // to it for fine-tuning. queue_draw kicks the canvas to refresh with
  // the new envelope baked in.
  m_canvas.set_selection_single(wp);
  m_canvas.queue_draw();
  refresh_inspector();
}


// ── on_warp_edit ────────────────────────────────────────────────────────────
// s146 m3: dialog removed. Editing a Warp now happens entirely in the
// inspector's Object ▸ Warp section, which is always shown when a Warp
// is selected. The menu entry "Path ▸ Edit Warp" stays as a verb (per
// s146 design discussion) but does no real work — it just makes sure
// the selection is a Warp and refreshes the inspector. If a non-Warp
// is selected, it warns and returns. If a Warp is already selected,
// refresh_inspector is the affordance: the user's attention follows
// to the now-visible Object ▸ Warp section.
void MainWindow::on_warp_edit() {
  SceneNode *warp = m_canvas.selected_object();
  if (!warp || !warp->is_warp()) {
    LOG_WARN("MainWindow::on_warp_edit: selection is not a Warp");
    return;
  }
  // Ensure glyph_cache is current — selection-driven inspector refresh
  // reads bbox-derived state, so we want it accurate. Also a defensive
  // queue_draw in case the caller arrived here from a state where the
  // canvas hadn't redrawn since the last envelope mutation.
  m_canvas.rebuild_warp_caches(warp);
  m_canvas.queue_draw();
  refresh_inspector();
}


// ── on_warp_release ─────────────────────────────────────────────────────────
// Delegates to Canvas::release_warp. No dialog needed — the action is
// a straightforward tree mutation with atomic undo.
void MainWindow::on_warp_release() { m_canvas.release_warp(); }


// ── on_warp_flatten ─────────────────────────────────────────────────────────
// Delegates to Canvas::flatten_warp. Replaces the Warp with its baked
// warped geometry; envelope is lost. Atomic undo.
void MainWindow::on_warp_flatten() { m_canvas.flatten_warp(); }

// ─────────────────────────────────────────────────────────────────────
// ━━━ guides ━━━ Guide-construct review dialog (open + close).
// ─────────────────────────────────────────────────────────────────────

// ── open_guide_review_dialog ───────────────────────────────────────────────
// Tiny non-modal window: Perpendicular checkbox + Cancel + OK.  Created
// lazily and reused across multiple guide-construct sessions.  P-key on the
// dialog toggles the checkbox; Enter commits; Esc cancels.
void MainWindow::open_guide_review_dialog() {
  if (!m_guide_review_win) {
    m_guide_review_win = std::make_unique<Gtk::Window>();
    m_guide_review_win->set_title("New Guide");
    m_guide_review_win->set_resizable(false);
    m_guide_review_win->set_hide_on_close(true);
    m_guide_review_win->set_default_size(260, -1);
    m_guide_review_win->set_transient_for(*this);
    curvz::utils::apply_motif_class_from_parent(*m_guide_review_win,
                                                *this); // s117 m18 v2

    auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    outer->set_spacing(0);

    auto *body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    body->set_spacing(8);
    body->set_margin(14);

    auto *info = Gtk::make_managed<Gtk::Label>(
        "Click two points on the canvas.\nP flips to perpendicular.");
    info->set_xalign(0.0f);
    info->add_css_class("dim-label");
    body->append(*info);

    // s208 m5: substrate. open_guide_review_dialog uses the lazy-once
    // pattern `if (!m_guide_review_win) { ... build ... }`, so the
    // construction below runs exactly once per MainWindow lifetime.
    // Substrate registrations are unproblematic.
    m_guide_review_perp_chk =
        Gtk::make_managed<curvz::widgets::CheckButton>(
            "dlg_gr_perp", "Perpendicular (through midpoint)");
    curvz::utils::set_name(m_guide_review_perp_chk, "dlg_gr_perp",
                           "guide_review_dialog_perpendicular_chk");
    m_guide_review_perp_chk->signal_toggled().connect([this]() {
      m_canvas.set_guide_construct_perpendicular(
          m_guide_review_perp_chk->get_active());
    });
    body->append(*m_guide_review_perp_chk);

    outer->append(*body);

    auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    outer->append(*sep);

    auto *btn_bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    btn_bar->set_spacing(8);
    btn_bar->set_margin(10);
    btn_bar->set_halign(Gtk::Align::END);

    auto *cancel_btn = Gtk::make_managed<curvz::widgets::Button>(
        "dlg_gr_cnc", "Cancel");
    curvz::utils::set_name(cancel_btn, "dlg_gr_cnc",
                           "guide_review_dialog_cancel_btn");
    cancel_btn->signal_clicked().connect([this]() {
      m_canvas.cancel_guide_construct();
      close_guide_review_dialog();
    });
    btn_bar->append(*cancel_btn);

    auto *ok_btn = Gtk::make_managed<curvz::widgets::Button>(
        "dlg_gr_ok", "Create");
    curvz::utils::set_name(ok_btn, "dlg_gr_ok",
                           "guide_review_dialog_create_btn");
    ok_btn->add_css_class("suggested-action");
    ok_btn->signal_clicked().connect([this]() {
      m_canvas.commit_guide_construct();
      close_guide_review_dialog();
    });
    btn_bar->append(*ok_btn);

    outer->append(*btn_bar);
    m_guide_review_win->set_child(*outer);

    // Keyboard: Enter → OK, Esc → cancel, P → toggle perpendicular.
    auto key = Gtk::EventControllerKey::create();
    key->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) -> bool {
          if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            m_canvas.commit_guide_construct();
            close_guide_review_dialog();
            return true;
          }
          if (keyval == GDK_KEY_Escape) {
            m_canvas.cancel_guide_construct();
            close_guide_review_dialog();
            return true;
          }
          if (keyval == GDK_KEY_p || keyval == GDK_KEY_P) {
            if (m_guide_review_perp_chk)
              m_guide_review_perp_chk->set_active(
                  !m_guide_review_perp_chk->get_active());
            return true;
          }
          return false;
        },
        true);
    m_guide_review_win->add_controller(key);

    // WM close button → cancel (matches Esc).
    m_guide_review_win->signal_close_request().connect(
        [this]() -> bool {
          if (m_canvas.guide_construct_active())
            m_canvas.cancel_guide_construct();
          return false; // allow default hide
        },
        false);
  }

  // Reset widgets per-show.
  if (m_guide_review_perp_chk)
    m_guide_review_perp_chk->set_active(false);

  m_guide_review_win->set_modal(false);
  m_guide_review_win->present();
}


void MainWindow::close_guide_review_dialog() {
  if (m_guide_review_win)
    m_guide_review_win->hide();
}

// ─────────────────────────────────────────────────────────────────────
// ━━━ macros ━━━ Macro manager and run-macro entry points.
// ─────────────────────────────────────────────────────────────────────

// ── on_macro_manager
// ──────────────────────────────────────────────────────────
void MainWindow::on_macro_manager() {
  // Wire signals on first show (idempotent — signals are connected once)
  static bool signals_connected = false;
  if (!signals_connected) {
    m_macro_manager.signal_run_macro().connect(
        sigc::mem_fun(*this, &MainWindow::on_run_macro));
    m_macro_manager.signal_edit_macro().connect(
        [this](const std::string &macro_id) {
          m_macro_editor.load_macro(macro_id);
          m_macro_editor.show(*this);
        });
    m_macro_editor.signal_run_from().connect([this](const std::string &macro_id,
                                                    int step_idx) {
      LOG_INFO("MainWindow: run macro '{}' from step {}", macro_id, step_idx);
      m_canvas.run_macro(macro_id, step_idx);
    });
    signals_connected = true;
  }
  m_macro_manager.show(*this);
}


// ── on_run_macro
// ──────────────────────────────────────────────────────────────
void MainWindow::on_run_macro(const std::string &macro_id) {
  if (macro_id.empty()) {
    on_macro_manager();
    return;
  }
  Macro *m = MacroManager::instance().find_macro(macro_id);
  if (!m) {
    LOG_WARN("MainWindow: macro {} not found", macro_id);
    return;
  }
  if (m->steps.empty()) {
    LOG_INFO("MainWindow: macro '{}' has no steps — opening manager", m->name);
    on_macro_manager();
    return;
  }
  LOG_INFO("MainWindow: run macro '{}' ({} steps)", m->name, m->steps.size());
  m_canvas.run_macro(macro_id);
}

} // namespace Curvz
