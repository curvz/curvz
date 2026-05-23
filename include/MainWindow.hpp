// ─────────────────────────────────────────────────────────────────────────
// MainWindow — the application's main window class.
//
// Implementation is split across multiple TUs by *category of code*, not by
// domain. The header below is the index — every public and private method
// declared here has its body in one of the categorized files. Find a method
// here, read its trailing `// category: ...` tag, open the matching file.
//
// Vocabulary
// ----------
//   Zones    — named regions of the window the user can point at:
//              headerbar, menubar, toolbar, canvas, status bar, panels,
//              rulers, overlays, tabs, dialogs.
//              Files: MainWindow_zones_*.cpp
//
//   Bindings — what's connected to what. Signals connected to handlers,
//              Gio actions declared and bound to handlers, accelerators
//              registered, key controllers wired. After the s164 split
//              this is still the largest TU because most of its lines
//              are inline lambda slot bodies that travel with the wiring.
//              File:  MainWindow_bindings.cpp
//
//   Handlers — slot bodies. The on_* / apply_* methods that run when a
//              binding fires. Grouped by domain (documents, edit, effects,
//              guides, library, macros).
//              Files: MainWindow_handlers_*.cpp
//
//   Helpers  — reusable pieces called from bindings, handlers, or other
//              helpers. Display sync (refresh_inspector, update_title),
//              action-enable predicates, flow orchestrators (load_project,
//              check_unsaved_then), persistence, the inspector make_section
//              pumps, the SVG-import workhorses, the library write helper.
//              File:  MainWindow_helpers.cpp
//
//   Glue     — what stays in MainWindow.cpp itself: the ctor that
//              orchestrates zone construction and binding setup, plus a
//              few genuinely cross-cutting methods that don't fit any one
//              category (on_tool_changed, on_doc_activated, cycle_doc,
//              rename_doc, setup_project).
//
// Finding things
// --------------
//   "Where does on_save's body live?"     → MainWindow_handlers_documents.cpp
//   "Where is the menubar built?"         → MainWindow_zones_menu.cpp
//   "Where is win.save's accelerator?"    → MainWindow_bindings.cpp
//   "Where is refresh_inspector defined?" → MainWindow_helpers.cpp
//   "Why is the ctor so small?"           → it's just orchestration; the
//                                           real work is in the categorized
//                                           files above.
//
// Why this split exists
// ---------------------
// MainWindow.cpp had grown to ~7000 lines; setup_menu (~830) and
// connect_signals (~2110) alone were 42% of the file. The split is
// structural relief — findability over compile-time. The vocabulary
// is the load-bearing decision; the file layout is its expression.
// Let the code argue with the layout when it doesn't fit; adjust the
// layout, keep the vocabulary.
//
// Per-method tags below: every method declaration carries a trailing
// comment naming its category (and sub-domain where relevant). That tag
// IS the index — without it, the split fragments findability instead
// of improving it.
// ─────────────────────────────────────────────────────────────────────────

#pragma once
#include "BlendPopover.hpp"
#include "Canvas.hpp"
#include "CommandHistory.hpp"
#include "ContextBar.hpp"
#include "CurvzProject.hpp"
#include "CurvzSpinButton.hpp"
#include "DocTabBar.hpp"
#include "DocumentGallery.hpp"
#include "LayersPanel.hpp"
#include "LibraryPanel.hpp"
#include "NewDocumentDialog.hpp"
#include "PreviewPanel.hpp"
#include "PropertiesPanel.hpp"
#include "StatusBar.hpp"
#include "StepRepeatPopover.hpp"
#include "StylesPanel.hpp"
#include "SwatchesPanel.hpp"
#include "ThemesPanel.hpp"
#include "Toolbar.hpp"
#include "WarpPopover.hpp"
#include <chrono> // s165 m3 — chrono trap on rapid undo presses
#include <functional>
#include <map> // s125 m1e: m_last_folders
#include <optional>
#include <unordered_map> // s141: m_sec_apply
#include <vector>
// s146 m3 — WarpDialog removed; warp creation seeds from AppPreferences
// (Application ▸ Warp inspector subsection) and editing happens entirely
// in the Object ▸ Warp inspector section.
#include "ClipboardViewWindow.hpp" // s203 m1 — hide-on-close singleton
#include "GradientDialog.hpp"
#include "HelpWindow.hpp"
#include "ImageInfoDialog.hpp" // s210 m1 — hide-on-close singleton
#include "MacroEditorWindow.hpp"
#include "MacroManagerWindow.hpp"
#include "ManageTemplatesDialog.hpp"
#include "OffsetPathDialog.hpp"
#include "PrintManager.hpp"
#include "ProgressDialog.hpp"        // s277 m1 — long-op progress envelope
#include "RotateFromPointDialog.hpp" // s210 m2 — hide-on-close singleton
#include "Ruler.hpp"
#include "SaveAsTemplateDialog.hpp"
#include "ShortcutsDialog.hpp"
#include "StyleEditorDialog.hpp" // s201 m1 — hide-on-close singleton
#include "ThemeEditDialog.hpp"   // s200 m1 — hide-on-close singleton
#include "TranslateDialog.hpp"   // s205 m4 — pivot-aware transform hub
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/fixed.h>
#include <gtkmm/frame.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/grid.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/overlay.h>
#include <gtkmm/paned.h>
#include <gtkmm/popover.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/revealer.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/togglebutton.h>
#include <memory>

// s208 m5 — forward-declare substrate CheckButton for the guide-review
// dialog's perp toggle (member-pointer here, constructed and dereferenced
// in MainWindow_handlers.cpp).
namespace curvz::widgets {
class CheckButton;
}
namespace curvz::widgets {
class ToggleButton;
} // namespace curvz::widgets

// s216 m1 / s219 m1 — fwd declared rather than full-include so MainWindow.hpp
// doesn't pull scripting headers. The unique_ptr members below are
// incomplete-type friendly only when the dtor is out-of-line; we
// provide that in MainWindow.cpp alongside the construction.
//
// s218 m1 — same shape for GuidesScriptable, the second row-bound model
// Scriptable. Forward-declared in the same block; both members share
// the single out-of-line dtor below.
//
// s219 m1 — these are no longer gated. The scripting TUs always compile.
// ScripterWindow joins the same fwd-decl list because MainWindow now
// owns it as a unique_ptr too (was previously held by Application).
namespace curvz::scripting {
class LayersScriptable;
}
namespace curvz::scripting {
class GuidesScriptable;
}
namespace curvz::scripting {
class SwatchesScriptable;
}
namespace curvz::scripting {
class PalettesScriptable;
} // namespace curvz::scripting
namespace curvz::scripting {
class StylesScriptable;
}
namespace curvz::scripting {
class ThemesScriptable;
}
namespace curvz::scripting {
class ObjectsScriptable;
}
namespace curvz::scripting {
class InspectorScriptable;
}
namespace curvz::scripting {
class ProjScriptable;
} // namespace curvz::scripting
namespace curvz::scripting {
class ExportScriptable;
} // namespace curvz::scripting
namespace curvz::scripting {
class AppScriptable;
} // namespace curvz::scripting
namespace curvz::scripting {
class ActionGroupScriptable;
} // namespace curvz::scripting
namespace curvz::scripting {
class ScripterWindow;
}

namespace Curvz {

class Application;

class MainWindow : public Gtk::ApplicationWindow {
public:
  explicit MainWindow(Application &app);
  // s216 m1 / s219 m1 / s221 m1 / s222 m1 / s222 m2 / s223 m1 / s230 m1 / s243
  // m2 / s246 m1 / s251 m1 / s254 m2 / s263 m2 — out-of-line dtor so
  // unique_ptr<curvz::scripting::LayersScriptable>,
  // unique_ptr<curvz::scripting::GuidesScriptable>,
  // unique_ptr<curvz::scripting::SwatchesScriptable>,
  // unique_ptr<curvz::scripting::PalettesScriptable>,
  // unique_ptr<curvz::scripting::StylesScriptable>,
  // unique_ptr<curvz::scripting::ThemesScriptable>,
  // unique_ptr<curvz::scripting::ObjectsScriptable>,
  // unique_ptr<curvz::scripting::InspectorScriptable>,
  // unique_ptr<curvz::scripting::ProjScriptable>,
  // unique_ptr<curvz::scripting::ExportScriptable>,
  // unique_ptr<curvz::scripting::AppScriptable>, and
  // unique_ptr<curvz::scripting::ActionGroupScriptable> can hold incomplete
  // types at the header level. Implementation lives in MainWindow.cpp
  // alongside the construction. No longer gated as of s219 m1.
  ~MainWindow();

  // s126: last-used folder accessors. Public so non-MainWindow dialogs
  // (ExportDialog and friends) can opt into the same per-purpose
  // memory the built-in pickers use. Storage lives in m_last_folders
  // and persists via save_config.
  std::string get_last_folder(
      const std::string &purpose) const; // category: helper: persistence
  void
  set_last_folder(const std::string &purpose,
                  const std::string &path); // category: helper: persistence

  // s191 m3 / s219 m1 — caption bar driven by the Scripter's `#[sub]` lines.
  // Empty text hides the bar (reveals collapses). Non-empty shows
  // the text and reveals. Replacement is instant (no fade between
  // captions); the reveal animation only plays on show-from-empty
  // and hide-to-empty transitions.
  //
  // Application wires this to the ScriptListener's subtitle
  // callback after both MainWindow and ScripterWindow exist. The
  // surface is always compiled (s219 m1); it only animates when a
  // script with `#[sub]` lines runs, which happens only when the
  // user has the Scripter open and hits Run.
  void set_subtitle(const std::string &text); // category: zone: caption-bar

  // s201 m3 / s219 m1 — panel accessors for the script-driven action-dispatch
  // verb. The Scripter's `do <prefix.action>` verb routes to whichever
  // panel owns the action group with that prefix (StylesPanel inserts
  // "styles", ThemesPanel inserts "themes-io", etc.). Application
  // installs the callback that performs the routing and needs widget
  // handles on each panel to call activate_action() through them —
  // GTK's action-group resolution walks up from the originating
  // widget, so the call must originate on the widget that holds the
  // group. These accessors are the route; they're always available
  // as of s219 m1.
  StylesPanel &styles_panel() { return m_styles; }
  ThemesPanel &themes_panel() { return m_themes; }

  // ── s202 m6 — inspector focus / quick-jump ─────────────────────────
  //
  // Two user-facing affordances that share the m5 focus-move shape:
  //
  //   Ctrl+Shift+Space → collapse every inspector section, top-level
  //                      groups included. Zero state. (Alt+Space was
  //                      the original chord but GNOME captures it
  //                      for the compositor window menu.)
  //   Ctrl+Space       → pop the quick-jump float listing currently
  //                      relevant sections (Document or Object
  //                      sections per selection, plus the always-
  //                      present Content panels). Pick one and the
  //                      inspector focus-moves to it.
  //
  // Both hotkeys fire regardless of text focus per the existing
  // modifier-bypass rule in the keys controller (Ctrl chords always
  // get through; bare Space still routes to text widgets when one
  // has focus).
  //
  // The implementation reaches both collapse mechanisms: MainWindow's
  // m_sec_apply registry (Content group + Layers/Library/Swatches/
  // Styles/Themes/Documents) AND PropertiesPanel's m_section_open
  // map (Document/Object groups + every section inside them, since
  // PropertiesPanel rebuilds its widget tree per selection). Both
  // are treated as one logical state for the purposes of "collapse
  // all" and "focus on this section."
  //
  // Public because the keyboard controller in MainWindow_bindings.cpp
  // and the quick-jump float's button handlers both call these;
  // keeping them at the MainWindow public surface (not hidden behind
  // diagnostic gates) because these are user-facing features, not
  // script-driven ones.
  void collapse_all_inspector_sections();
  void focus_inspector_on(const std::string &section_title);
  void show_quick_jump_popover();

  // s222 m2 — open a single inspector section by title without
  // collapsing siblings. Lookup is against m_sec_apply (the right-
  // panel registry: Layers, Library, Swatches, Styles, Themes,
  // Documents, Preview, plus the Content group). Unknown titles are
  // silent no-ops. The applier closure handles widget cascade
  // (body visibility + arrow glyph) and project-field persistence,
  // identical to the path the user takes by clicking the header.
  //
  // Limitation: PropertiesPanel-internal sections (Dimensions, Node,
  // Object/Application/Document groups and their inner sections) are
  // NOT reached by this method — the body lookup for those lives in
  // PropertiesPanel's m_section_open map and per-section lambdas,
  // not in m_sec_apply. The keyboard quick-jump's focus_inspector_on
  // method spans BOTH registries (its first phase collapses
  // everything via m_sec_apply + PropertiesPanel's set_section_open_state,
  // then opens the leaf via the appropriate path). The cascading
  // logic isn't lifted here yet — the s222 m2 use case (script
  // demo-prep) targets the right-panel sections, which m_sec_apply
  // covers fully. A future "open with cascade" variant can lift the
  // body of focus_inspector_on once a use case demands it.
  //
  // Public for the same reason collapse_all_inspector_sections and
  // focus_inspector_on are — script-driven and keyboard-driven entry
  // points both sit at the MainWindow public surface.
  void apply_section_open(const std::string &section_title, bool on);
  // ── end s202 m6 ────────────────────────────────────────────────────

  // s203 m1 — View Clipboard mini float (Edit ▸ View Clipboard…).
  // Lazy-builds m_clipboard_view_win on first invocation, then refresh-
  // and-show on subsequent ones. See ClipboardViewWindow.hpp for the
  // design rationale; the user-visible motivation is the Measure tool's
  // structured copy, which the user wants to dissect without a round-
  // trip through an external text editor.
  void show_clipboard_view();

  // s219 m1 — show or hide the Scripter window. Called from the
  // headerbar monkey-button's toggled signal (forward direction) and
  // from apply_scripter_pref() when the pref is being turned off
  // (hide-only on that path; the pref-on path leaves the window's
  // current visibility alone, per the s219 m1 design where the user
  // explicitly clicks to open).
  //
  // The Scripter is a member of MainWindow (m_scripter), constructed
  // in the ctor right after setup_project. The window's lifetime is
  // MainWindow's lifetime; the X-button close hides it via
  // set_hide_on_close(true) in its ctor.
  //
  // `visible=true`  → set_transient_for(*this) then present()
  // `visible=false` → set_visible(false)
  //
  // The asymmetry matches HelpWindow / ShortcutsDialog: every show
  // re-asserts the transient relationship; hide is just a visibility
  // flip.
  void show_scripter(bool visible);

  // ── s277 m2 — long-op progress dialog accessor ────────────────────
  //
  // Public handle on the ProgressDialog member so Canvas (and any
  // other widget child of MainWindow) can wrap its long ops without
  // owning a ProgressDialog itself. Canvas reaches it via
  // dynamic_cast<MainWindow*>(get_root())->progress_dialog().
  //
  // Mirrors the convention used for scripter / save helpers: rather
  // than friend-classing Canvas or routing through signals (which
  // can't return synchronously, and the dialog needs to block its
  // caller until the worker completes), expose a typed reference.
  ProgressDialog &progress_dialog() { return m_progress_dialog; }

  // ── s246 m1 — script-side proj save helpers ───────────────────────
  //
  // ProjScriptable (curvz::scripting::ProjScriptable) needs to invoke
  // the same save path the GUI Save action runs, AND it needs to know
  // the project's current directory path for its `path` / `has_path`
  // queries. Both touch private state (m_project, m_canvas) on this
  // class. Rather than friending the Scriptable or exposing the raw
  // members, two small public helpers package the access here.
  //
  // The split keeps the Scriptable thin (just DSL-facing string
  // formatting on the invoke side) and keeps the decisions about
  // window state inside MainWindow's API surface where they
  // naturally belong. See ProjScriptable.cpp for the enum-to-error
  // mapping on the script side and CANON's "Headless-verb singletons"
  // entry for why save's argument is implicit in the project state
  // rather than supplied by the script.

  // Outcome of a script-driven save attempt. The Scriptable maps
  // each non-Ok value to a structured DSL error so the script
  // author can see why nothing was written.
  //
  //   Ok          — CurvzProject::save() returned true. The helper
  //                 has already called update_title() and emitted a
  //                 LOG_INFO line, mirroring on_save's success path.
  //   NoProject   — m_project is null. Rare edge case; can occur
  //                 transiently between document close and open.
  //   NoPath      — m_project is loaded but its directory is empty
  //                 (the project has never been saved). The GUI's
  //                 on_save falls through to on_save_as in this
  //                 case, opening a picker; a Scriptable can't
  //                 summon a modal, so the helper refuses and the
  //                 Scriptable surfaces the "use save_as <path>"
  //                 hint pointing to s247's future verb.
  //   Dragging    — m_canvas.is_dragging() is true. The GUI handler
  //                 defers via 100ms signal_timeout in this case;
  //                 the Scriptable refuses synchronously rather
  //                 than return ok and fire a deferred save later.
  //   IoFailed    — CurvzProject::save() returned false. Mirrors
  //                 the GUI handler's LOG_ERROR path.
  enum class ScriptSaveResult {
    Ok,
    NoProject,
    NoPath,
    Dragging,
    IoFailed,
  };

  // Run the save flow on behalf of a script caller. Mirrors the
  // body of on_save except the picker-fallthrough (Save As) is
  // converted to a NoPath refusal — scripts can't summon modals,
  // and CANON's Scripter line lists "proj save_as <arbitrary>"
  // as out-of-scope for the in-app DSL playground.
  //
  // Side effects on Ok: project written to disk via
  // CurvzProject::save(), update_title() called, LOG_INFO line
  // emitted. No side effects on any other return value (no
  // partial state).
  ScriptSaveResult script_save_project();

  // Read-only accessor for the project's current directory path.
  // Returns an empty string if no project is loaded or the project
  // has never been saved. Used by ProjScriptable's `path` and
  // `has_path` queries; the empty / non-empty distinction is the
  // only state the queries need to surface.
  std::string script_project_path() const;

  // ── s247 m1 — script-side proj save_as helper ─────────────────────
  //
  // Sibling to script_save_project() above. The Scriptable calls
  // this helper from ProjScriptable's `save_as` verb branch AFTER
  // pre-validating the path through curvz::scripting::path_is_safe.
  // The split is deliberate: path-safety pre-validation lives in the
  // Scriptable (it owns the DSL-facing refusal string), and this
  // helper only runs once the path has cleared that gate. From the
  // helper's point of view, `path` is already vetted to be inside
  // $HOME or $TMPDIR; it only worries about project state.
  //
  // No NoPath case (compare ScriptSaveResult above) — the path IS
  // the argument here. No PathUnsafe case either — that refusal is
  // structurally upstream, the helper never sees an unsafe path.
  // What remains is the same three structural conditions Save flows
  // share: project loaded? mid-drag? did the actual write succeed?
  //
  //   Ok          — Project's directory was set to `path` and
  //                 CurvzProject::save() returned true. The helper
  //                 has already called save_config() and update_title()
  //                 (mirroring do_save_as's success path).
  //   NoProject   — m_project is null. Same edge case as save.
  //   Dragging    — m_canvas.is_dragging() is true. Same refusal
  //                 rationale as save: a synchronous ok-echo while
  //                 a deferred write fires later would lie about
  //                 the result.
  //   IoFailed    — CurvzProject::save() returned false. Same as
  //                 do_save_as's LOG_ERROR path; project state is
  //                 left as-is (directory was already assigned the
  //                 new path before save attempted, matching
  //                 do_save_as's existing posture — a half-written
  //                 file is rarer than a directory-stamp-without-
  //                 successful-write, and the latter is recoverable
  //                 by re-trying the save).
  enum class ScriptSaveAsResult {
    Ok,
    NoProject,
    Dragging,
    IoFailed,
  };

  // Run the save_as flow on behalf of a script caller. The supplied
  // `path` is assumed already vetted by path_is_safe; the helper
  // does not re-check (separation of concerns: path containment is
  // the Scriptable's pre-flight, project-state checks are this
  // helper's). Body is the same three checks the save helper does
  // (skipping NoPath, since `path` IS the path), followed by a
  // delegated call to do_save_as() — the existing on_save_as
  // success-tail helper that updates m_project->directory, saves,
  // updates last-folder config, and refreshes the title.
  //
  // Side effects on Ok: m_project->directory := path, project
  // written to disk via do_save_as → CurvzProject::save(),
  // save_config() and update_title() called. No side effects on
  // NoProject or Dragging. On IoFailed, m_project->directory has
  // been assigned the new path (matching do_save_as's pre-existing
  // behaviour — the assignment happens before the save attempt).
  ScriptSaveAsResult script_save_project_as(const std::string &path);

  // ── s248 m1 — script-side proj close helpers ──────────────────────
  //
  // Sibling to script_save_project / script_save_project_as. The
  // Scriptable calls script_close_project from ProjScriptable's
  // `close` verb branch. The split is the same pattern: state-check
  // and writer/teardown in MainWindow, DSL-facing error formatting
  // in the Scriptable.
  //
  // The close flow has an extra wrinkle relative to save:
  // on_close_project wraps its body in check_unsaved_then(), which
  // summons a Save/Discard/Cancel modal when m_history.can_undo() is
  // true. A Scriptable can't summon modals — same picker-fallthrough
  // problem the s246 m1 NoPath refusal solved for `save`. Solution
  // is symmetric: refuse with Dirty when can_undo() is true, point
  // the script author at `proj save` (or `proj save_as <path>`) as
  // the remedy. The script author who genuinely wants to discard
  // unsaved work waits for a future `force_close` verb or an arg
  // flag on close — out of m1 scope.
  //
  // Outcome of a script-driven close attempt. The Scriptable maps
  // each non-Ok value to a structured DSL error.
  //
  //   Ok          — Project was closed. Teardown ran via
  //                 do_close_project(): panels cleared, command
  //                 history dropped, config-path file removed,
  //                 update_project_sensitive + update_title fired,
  //                 LOG_INFO line emitted.
  //   NoProject   — m_project was already null. There is nothing to
  //                 close. Refuse so the script author sees that
  //                 their assumption ("a project is loaded") was
  //                 wrong; otherwise a no-op close would silently
  //                 succeed and hide the bug.
  //   Dragging    — m_canvas.is_dragging() is true. Same rationale
  //                 as save / save_as: a synchronous ok-echo while a
  //                 deferred teardown fires later would lie about
  //                 the result.
  //   Dirty       — Project has work on the undo stack
  //                 (m_history.can_undo()). The GUI handler prompts
  //                 Save/Discard/Cancel via check_unsaved_then; a
  //                 Scriptable can't summon modals. Refuse instead
  //                 and let the script author make the save/discard
  //                 choice explicit (today: call `proj save` first
  //                 to clear the dirty signal, then re-call `proj
  //                 close`; tomorrow: a future force_close verb
  //                 for the discard branch).
  enum class ScriptCloseResult {
    Ok,
    NoProject,
    Dragging,
    Dirty,
  };

  // Run the close flow on behalf of a script caller. Mirrors
  // on_close_project's state checks (project loaded, canvas not
  // dragging) plus the dirty-prompt that on_close_project handles
  // via check_unsaved_then — except the modal is converted to a
  // Dirty refusal (scripts can't summon modals). On Ok, delegates
  // to do_close_project() for the actual teardown work — the same
  // method on_close_project's lambda body now lives in.
  //
  // Side effects on Ok: m_project becomes null, command history
  // emptied, all panels cleared, config file removed (so next
  // launch starts empty), update_project_sensitive + update_title
  // fired. No side effects on any other return value.
  ScriptCloseResult script_close_project();

  // s248 m1 — script-side accessor for the project's "dirty" signal.
  //
  // Uses the same proxy on_close_project's check_unsaved_then uses:
  // m_history.can_undo(). Curvz does not (yet) have a true project-
  // level dirty bit on CurvzProject (every command push would set
  // it, every save would clear it — real infrastructure work; see
  // backlog). Until that ships, "is the undo stack non-empty?" is
  // the dirty signal the entire GUI uses, end to end — and reusing
  // it for the script-side `dirty` query keeps the script's
  // semantic in lock-step with the GUI's.
  //
  // Returns false if no project is loaded (nothing to be dirty
  // about) or the project's command history is empty. Returns true
  // iff a project is loaded AND the history has at least one
  // undoable command on it. The query is the smoke 46 precondition
  // assertable observable; without it the smoke can only operator-
  // visual its way through the dirty-branch test.
  //
  // When the real project-level dirty bit lands, this helper's
  // body changes to read that bit; the public contract stays
  // identical, so script code keeps working unchanged.
  bool script_project_dirty() const;

  // ── s249 m1 — script-side proj load helper ────────────────────────
  //
  // Sibling to script_save_project_as and script_close_project. The
  // Scriptable calls this from ProjScriptable's `load` verb branch
  // AFTER pre-validating the path through path_is_safe (same split
  // as save_as: path-containment in the Scriptable, project-state +
  // I/O in this helper).
  //
  // `load` is a destructive-replacement verb: success swaps the
  // current project out and the supplied one in. The
  // surface-preservation rule (s248 m1, articulated in
  // ProjScriptable.hpp's `close` block) says any verb that destroys
  // the project state the script reasons about is TestRunner-only.
  // load is the SECOND instance of that rule (close was the first;
  // save_as is sibling-under-the-path-containment-rule rather than
  // surface preservation, even though both rules land at the same
  // TestRunner-only mask).
  //
  //   Ok          — CurvzProject::open(path) succeeded and
  //                 load_project() was called. The orchestrator
  //                 did the panel sync, recent-projects bump,
  //                 update_title, and LOG_INFO line. No NoProject
  //                 case is needed — load is the one project-
  //                 lifecycle verb that's legitimate to call from
  //                 the no-project state (it's how you GET into a
  //                 loaded state).
  //   Dragging    — m_canvas.is_dragging() is true. Same refusal
  //                 rationale as save / save_as / close: a
  //                 synchronous ok-echo while the project swap
  //                 fires later would lie. Note that this is the
  //                 only project-lifecycle helper where the check
  //                 only meaningfully fires when a project is
  //                 ALREADY loaded — there can't be a canvas drag
  //                 against no document. Still wired for symmetry
  //                 with the close / save / save_as flows and for
  //                 the "load while existing project mid-drag"
  //                 case (real and reachable from TestRunner).
  //   Dirty       — m_history.can_undo() is true. Refuse rather
  //                 than silently destroy the user's unsaved work.
  //                 NOTE: this is STRICTER than the GUI's on_open,
  //                 which currently does NOT run through
  //                 check_unsaved_then — the GUI silently replaces
  //                 the project. The script-side stricter posture
  //                 is deliberate: the GUI shows visible feedback
  //                 (title bar changes, panels redraw) that makes
  //                 the destruction observable; a script silently
  //                 dropping work in the middle of an automation
  //                 run would not. The script author who genuinely
  //                 wants to discard waits for a future
  //                 force_load verb or arg flag (s250+); for now,
  //                 refusing rather than silently dropping is the
  //                 safer default. The GUI's missing
  //                 check_unsaved_then is logged as a separate
  //                 backlog item — when the GUI gains the
  //                 check, the script and GUI postures align;
  //                 until then the script is the safer of the two.
  //   OpenFailed  — CurvzProject::open(path) returned null. This
  //                 conflates several disk-side failures (directory
  //                 doesn't exist, isn't a .curvz, parse failure)
  //                 because CurvzProject::open itself doesn't
  //                 distinguish them today — a single OpenFailed
  //                 matches the underlying surface honestly. When
  //                 open() grows distinguishable error returns, the
  //                 enum can split (NoFile / NotProject /
  //                 ParseFailed) without breaking the existing Ok
  //                 path; for now, OpenFailed is the single failure
  //                 bucket.
  enum class ScriptLoadResult {
    Ok,
    Dragging,
    Dirty,
    OpenFailed,
  };

  // Run the load flow on behalf of a script caller. The supplied
  // `path` is assumed already vetted by path_is_safe; the helper
  // does not re-check (separation of concerns: path containment is
  // the Scriptable's pre-flight, project-state + I/O is this
  // helper's).
  //
  // Body: drag check → dirty check → CurvzProject::open(path) →
  // load_project(). No do_load_project lift is needed (compare
  // do_save_as in s247, do_close_project in s248) because
  // load_project() already IS the orchestrator the GUI's on_open
  // delegates to; both callers share that single source of truth
  // already. Same pump-at-the-seam principle, applied at the
  // helper-orchestrator boundary rather than the
  // handler-helper boundary.
  //
  // Side effects on Ok: m_project replaced with the freshly-opened
  // project, m_history reset, every panel re-seeded to the new
  // project (panels, recents bumped, config saved, title
  // refreshed, LOG_INFO line). No side effects on any other return
  // value. On OpenFailed in particular the existing project is
  // left intact — the open() call returns null without mutating
  // any of MainWindow's state.
  ScriptLoadResult script_load_project(const std::string &path);

  // ── s250 m1 — script-side proj new helper ─────────────────────────
  //
  // Sibling to script_load_project (above). The Scriptable calls this
  // from ProjScriptable's `new` verb branch AFTER pre-validating the
  // path through path_is_safe (same split as save_as and load: path-
  // containment in the Scriptable, project-state + I/O in this
  // helper).
  //
  // `new` is a destructive-replacement verb (when a project is
  // already loaded). Sibling to load in shape (one path arg,
  // path_is_safe pre-flight, MainWindow helper); sibling to close
  // and load in posture (TestRunner-only, by the surface-preservation
  // rule). Mechanically distinct from load: load reads an existing
  // .curvz from disk via CurvzProject::open, while new constructs a
  // fresh empty project via CurvzProject::create_empty (which itself
  // calls save() at construction time, writing project.json into the
  // target directory).
  //
  //   Ok            — CurvzProject::create_empty(path) succeeded and
  //                   load_project() was called. The orchestrator did
  //                   the panel sync, recent-projects bump,
  //                   update_title, and LOG_INFO line. No NoProject
  //                   case — new is legitimate from any state (it's
  //                   one of the two project-lifecycle verbs along
  //                   with load that legitimately runs from the
  //                   no-project state).
  //   Dragging      — m_canvas.is_dragging() is true. Same refusal
  //                   posture as every project-lifecycle helper.
  //                   With no project loaded the check is trivially
  //                   false, so this only meaningfully fires when
  //                   new is REPLACING an existing project. Still
  //                   wired for symmetry.
  //   Dirty         — m_history.can_undo() is true. Refuse rather
  //                   than silently destroy the user's unsaved work.
  //                   Stricter than the GUI's on_new_project, which
  //                   routes through check_unsaved_then (a modal the
  //                   script can't summon); the script-side
  //                   structural refusal is the modal's equivalent.
  //                   Same banked-for-when-named posture as load's
  //                   Dirty case: a future force_new verb (s250+)
  //                   gives the discard path; for now refusing is
  //                   the safer default.
  //   TargetExists  — fs::exists(path) is true BEFORE create_empty
  //                   runs. Stricter than the GUI's on_new_project,
  //                   which silently overwrites if the target
  //                   already exists (because create_empty calls
  //                   save() which uses create_directories — no-op
  //                   on existing dir — then writes project.json,
  //                   clobbering whatever was there). The GUI's
  //                   silent-overwrite is arguably another GUI bug
  //                   in the family of "stricter than the GUI"
  //                   examples (alongside on_open's missing
  //                   check_unsaved_then). The script-side stricter
  //                   posture is deliberate by the same rationale
  //                   as load's Dirty refusal: a script silently
  //                   destroying an existing project mid-automation
  //                   is exactly the discipline-instead-of-design
  //                   trap the surface-preservation family of
  //                   rules is here to prevent. A future force_new
  //                   verb would give the overwrite path.
  //
  //                   This check is in the helper rather than the
  //                   Scriptable because it's a project-state-vs-disk
  //                   concern (the same layer that owns the
  //                   Dragging and Dirty checks), not a path-format
  //                   concern (which would belong with
  //                   path_is_safe).
  //   CreateFailed  — CurvzProject::create_empty(path) returned null.
  //                   create_empty's failure modes are: directory
  //                   creation failed (permissions, full disk),
  //                   or the immediate save() inside create_empty
  //                   failed (same causes). Single CreateFailed
  //                   bucket matches CurvzProject's current
  //                   surface; when create_empty grows
  //                   distinguishable error returns, the enum can
  //                   split without breaking the existing Ok path.
  //                   On CreateFailed the existing project is left
  //                   intact (create_empty either succeeds or
  //                   returns null without mutating MainWindow's
  //                   state).
  enum class ScriptNewResult {
    Ok,
    Dragging,
    Dirty,
    TargetExists,
    CreateFailed,
  };

  // Run the new-project flow on behalf of a script caller. The
  // supplied `path` is assumed already vetted by path_is_safe; the
  // helper does not re-check (separation of concerns matches
  // save_as and load exactly: path containment is the Scriptable's
  // pre-flight, project-state + I/O is this helper's).
  //
  // Body: drag check → dirty check → target-exists check →
  // CurvzProject::create_empty(path) → load_project(). No
  // do_new_project lift is needed — load_project() already IS the
  // orchestrator the GUI's on_new_project delegates to (after its
  // own check_unsaved_then + file dialog + extension normalisation),
  // so the same pump-at-the-seam discipline that load applied
  // (s249 m1) applies here too: the seam is the
  // helper-orchestrator boundary, one layer deeper than the do_-
  // lift cases (s247 do_save_as, s248 do_close_project).
  //
  // Side effects on Ok: target directory created on disk with
  // project.json (and empty swatches.json / styles.json /
  // themes.json from seed_library_defaults), m_project replaced
  // with the freshly-created empty project, m_history reset, every
  // panel re-seeded, recents bumped, config saved, title refreshed,
  // LOG_INFO line. No side effects on any other return value — on
  // Dragging / Dirty / TargetExists the existing project is
  // untouched; on CreateFailed any partial disk state from
  // create_empty's aborted run is left for the user/script to
  // inspect, but MainWindow's in-memory state is unchanged.
  ScriptNewResult script_new_project(const std::string &path);

  // ── s293 m4 — script-side proj new_doc helper ────────────────────
  //
  // Sibling to script_new_project (above) but operating one
  // cardinality level down: new_doc ADDS a document to the
  // currently-loaded project (existing project surface preserved),
  // where new REPLACES the entire project (destroys + reseeds).
  // The architectural delta drives the posture differences:
  //
  //   - new_doc is NOT under the surface-preservation rule. It
  //     adds; it doesn't destroy. The script's trace surface
  //     (project, output buffer, host window) is untouched. The
  //     active doc index moves to the new doc, which is the same
  //     class of mutation any other doc-affecting verb performs
  //     (objects.add, palettes.swatch, etc.) — not the
  //     surface-erasing mutation close/load/new perform on the
  //     project as a whole. Mask is Scripter | TestRunner
  //     (sibling to save, sibling to most doc-mutating verbs).
  //
  //   - new_doc does NOT auto-save the project after the add.
  //     The GUI's NewDocumentDialog callback does call save() —
  //     desirable for interactive workflows where the user sees
  //     a new tab and expects it persisted. Script callers may
  //     want to add several docs in a sequence before committing;
  //     calling save() inside the helper would write a half-built
  //     state to disk each iteration. The script caller drives
  //     persistence explicitly via proj.save / proj.save_as.
  //
  //   - new_doc refuses on NO PROJECT loaded but NOT on Dirty.
  //     Adding a doc to a dirty project is a legitimate
  //     in-progress workflow (draw → new_doc → draw → save) —
  //     refusing on dirty would break that. The surface-
  //     preservation Dirty refusals (close/load/new) exist
  //     because those verbs DESTROY the dirty work; new_doc
  //     preserves it untouched.
  //
  //   Ok           — Document constructed, appended to
  //                  m_project->documents, active_doc_index set to
  //                  point at it, update_all_panels() ran. No
  //                  disk I/O happened; the new doc lives in
  //                  memory until the script saves.
  //   NoProject    — m_project is null. Sibling to save's
  //                  NoProject refusal; you need a project to
  //                  add a doc to. The script caller can
  //                  precede with proj.new <path> to ensure a
  //                  project exists. With the typical empty-
  //                  default-project boot path this case is
  //                  rare; wired for symmetry with save.
  //   Dragging     — m_canvas.is_dragging() is true. Same posture
  //                  as every project-lifecycle helper.
  //   BadDimensions — w <= 0 or h <= 0. The Scriptable
  //                  pre-validates the integer parse and the
  //                  >0 check, so this branch is structurally
  //                  unreachable through the wired call path
  //                  (sibling to save_as's path_is_safe
  //                  Scripter-unreachability note). The helper
  //                  re-asserts the invariant for safety because
  //                  CanvasModel::from_pixels with non-positive
  //                  dimensions would produce a degenerate canvas
  //                  (zero or negative width/height in viewBox)
  //                  that downstream code reads as a bug rather
  //                  than a user error.
  enum class ScriptNewDocResult {
    Ok,
    NoProject,
    Dragging,
    BadDimensions,
  };

  // Add a fresh blank document of the supplied pixel dimensions to
  // the currently-loaded project and activate it. Filename is
  // auto-generated as "Untitled.svg" (or "Untitled2.svg",
  // "Untitled3.svg", ... uniqued against existing docs in the
  // project — same shape as the NewDocumentDialog callback's
  // uniquing loop in on_new). Canvas is from_pixels(w, h); the
  // CurvzDocument default ctor already seeds Layer 1 + Guides, so
  // no explicit layer setup needed here.
  //
  // Body — drag check, dimension check, build, append, activate,
  //        refresh panels. No save() call (see header for the
  //        rationale).
  ScriptNewDocResult script_new_doc(int width, int height);

  // ── s251 m1 — script-side export svg helper ──────────────────────
  //
  // Sibling helper to the script_save_project / script_save_project_as
  // / script_close_project / script_load_project / script_new_project
  // family — same shape (enum-naming-the-outcome return; takes
  // already-path-is-safe-vetted path; touches m_project / m_canvas
  // internals on this class's behalf).
  //
  // The helper writes the project's ACTIVE document as a standalone
  // .svg file at `path`, using SvgWriter::write_svg_file() — the
  // simplest of the five export-format writers. No size, units, or
  // fit-side parameters at this layer; the vanilla SVG (no
  // data-curvz-export-* metadata) is the right shape for the
  // singleton-opening milestone. Future m2+ format helpers
  // (script_export_png / script_export_theme / script_export_refpt /
  // script_export_gresource) sit alongside this one as the
  // ExportScriptable's verb surface widens.
  //
  // Path is assumed already vetted by curvz::scripting::path_is_safe;
  // the helper does not re-check (separation of concerns matches
  // every other script_* helper here exactly: path containment is
  // the Scriptable's pre-flight, project-state + I/O is the
  // helper's).
  //
  //   Ok           — Active doc was written to `path` via
  //                  SvgWriter::write_svg_file(); LOG_INFO line
  //                  emitted ("export svg: wrote '<path>'"). No
  //                  side effects on m_project state — export
  //                  produces a side artefact, the loaded project
  //                  is unchanged. No update_title() (the title
  //                  bar reflects project saved-ness, not export
  //                  activity; an export to /tmp shouldn't clear
  //                  the project's dirty indicator).
  //   NoProject    — m_project is null. Same edge case as save's
  //                  NoProject branch; rare but defensive.
  //   NoActiveDoc  — m_project is loaded but active_doc() returns
  //                  nullptr. The post-doc-removal transient
  //                  state: a project can exist with zero
  //                  documents after DocTabBar's close button
  //                  erases the last one. Mirrors what the GUI's
  //                  Export Documents menu item would show as
  //                  insensitive in this state.
  //   IoFailed     — SvgWriter::write_svg_file() returned false.
  //                  Mirrors the writer's LOG_ERROR path (open
  //                  failed, or close/flush failed). No partial
  //                  state — write_svg_file's open-fail returns
  //                  before any bytes go out; its successful-
  //                  bytes-flush-fail path is rare and leaves an
  //                  empty or partial file on disk that the
  //                  script can detect via subsequent inspection
  //                  if needed.
  enum class ScriptExportSvgResult {
    Ok,
    NoProject,
    NoActiveDoc,
    IoFailed,
  };

  // Run the SVG-export flow on behalf of a script caller. The
  // supplied `path` is assumed already vetted by path_is_safe; the
  // helper does not re-check. Active doc is read through
  // m_project->active_doc() at call time (no doc argument — the
  // ACTIVE doc is the export target, matching the GUI's Export
  // Documents dialog's default which targets the active doc).
  //
  // Side effects on Ok: a vanilla .svg file at `path`; LOG_INFO line
  // "export svg: wrote '<path>'". No project state changes — the
  // dirty indicator stays whatever it was, m_project->directory is
  // unchanged, m_history is unchanged. The export is a side artefact;
  // the project remains identifiable by its own .curvz directory.
  //
  // No side effects on any non-Ok return — NoProject / NoActiveDoc
  // are pre-check refusals; IoFailed leaves whatever write_svg_file
  // managed to produce on disk (typically nothing, occasionally an
  // empty/partial file the writer's open-failed path doesn't even
  // reach to create).
  ScriptExportSvgResult script_export_svg(const std::string &path);

  // ── s252 m2 — script-side export png helper ──────────────────────
  //
  // Sibling helper to script_export_svg — same shape (enum-naming-
  // the-outcome return; takes already-path-is-safe-vetted path;
  // touches m_project internals on this class's behalf), plus a
  // size arg (longest-side pixel count) the helper uses to derive
  // width × height from the active doc's aspect ratio.
  //
  // The helper writes the project's ACTIVE document as a PNG file
  // at `path`, using PngExporter::export_png_sized(). Width and
  // height are computed internally from `size` (the longest-side
  // pixel count) plus the active doc's canvas_width() /
  // canvas_height() — same calculation ExportDialog.cpp uses at
  // lines 911-922 for its fit_width / fit_height paths. The
  // longest-side convention matches the GUI's Export Documents
  // dialog (single size field), keeping the script's mental model
  // identical to the user's.
  //
  // Size is assumed already validated > 0 by the Scriptable's
  // argument-shape gate; the helper does not re-check. Same
  // layering posture as path_is_safe: argument-shape in the
  // Scriptable, state-check + I/O in the helper.
  //
  // Path is assumed already vetted by curvz::scripting::path_is_safe;
  // the helper does not re-check (separation of concerns matches
  // every other script_* helper here exactly: path containment is
  // the Scriptable's pre-flight, project-state + I/O is the
  // helper's).
  //
  //   Ok           — Active doc was written to `path` via
  //                  PngExporter::export_png_sized(); LOG_INFO line
  //                  emitted ("export png: wrote '<path>' WxH"). No
  //                  side effects on m_project state — export
  //                  produces a side artefact, the loaded project
  //                  is unchanged. No update_title() (the title
  //                  bar reflects project saved-ness, not export
  //                  activity).
  //   NoProject    — m_project is null. Same edge case as svg's
  //                  NoProject branch.
  //   NoActiveDoc  — m_project is loaded but active_doc() returns
  //                  nullptr. Post-doc-removal transient state;
  //                  mirrors svg.
  //   IoFailed     — export_png_sized() returned false. Mirrors
  //                  the writer's LOG_ERROR paths (invalid
  //                  dimensions — pre-checked here so this is
  //                  rare; or cairo_surface_write_to_png returned
  //                  non-success). The writer itself logs the
  //                  specific cause.
  //
  // No SizeInvalid branch on this enum — size <= 0 is an
  // argument-shape problem refused at the Scriptable layer before
  // the helper is reached. Same posture svg takes for its
  // path.empty() refusal (caught at the Scriptable, no helper
  // branch needed).
  enum class ScriptExportPngResult {
    Ok,
    NoProject,
    NoActiveDoc,
    IoFailed,
  };

  // Run the PNG-export flow on behalf of a script caller. `path` is
  // assumed path_is_safe-vetted; `size` is assumed > 0 (validated
  // at the Scriptable layer). Active doc is read through
  // m_project->active_doc() at call time (no doc argument — the
  // ACTIVE doc is the export target, matching the GUI's Export
  // Documents dialog's default).
  //
  // Size is the LONGEST-SIDE pixel count. Internal calculation:
  //   if (canvas_width >= canvas_height) {
  //       out_w = size;
  //       out_h = max(1, round(size * canvas_height / canvas_width));
  //   } else {
  //       out_h = size;
  //       out_w = max(1, round(size * canvas_width / canvas_height));
  //   }
  // Mirrors ExportDialog.cpp:911-922's fit_width / fit_height
  // arithmetic — the dialog picks fit-side from the user's choice
  // of dimension field, the helper picks fit-side from the
  // doc's aspect ratio (longest side wins). For a square doc
  // (canvas_width == canvas_height) the two branches give the
  // same result (out_w == out_h == size); the >= covers the
  // edge uniformly.
  //
  // Side effects on Ok: a .png file at `path`; LOG_INFO line
  // "export png: wrote '<path>' WxH". No project state changes.
  //
  // No side effects on any non-Ok return — NoProject / NoActiveDoc
  // are pre-check refusals; IoFailed leaves whatever
  // export_png_sized managed to produce on disk (typically nothing
  // if its dimensions check rejected, or a partial/empty file if
  // cairo's write failed mid-output).
  ScriptExportPngResult script_export_png(const std::string &path, int size);

  // ── s253 m1 — script-side export theme helper ───────────────────
  //
  // Sibling helper to script_export_svg and script_export_png —
  // same shape (enum-naming-the-outcome return; takes already-
  // path-is-safe-vetted path; touches m_project internals on this
  // class's behalf). The path argument here is a DIRECTORY
  // (output_dir for the freedesktop icon-theme bundle), not a
  // file path. path_is_safe doesn't care about the distinction —
  // it walks up to the nearest existing ancestor and checks
  // containment — but downstream the helper passes <dir> to
  // ensure_dir() inside export_theme(), which creates the
  // directory if it doesn't exist.
  //
  // The helper writes the project's documents as a freedesktop
  // icon-theme bundle at `<path>`, using Curvz::export_theme().
  // Documents are auto-included (every doc in
  // m_project->documents with include=true); theme_name is
  // derived from the project's directory basename; theme_comment
  // is empty. See ExportScriptable's theme verb-surface design
  // block for the defaults rationale.
  //
  // Path is assumed already vetted by curvz::scripting::path_is_safe;
  // the helper does not re-check. Same layering posture as
  // script_export_svg / script_export_png.
  //
  //   Ok            — Bundle written to `path` via
  //                   Curvz::export_theme(); LOG_INFO line emitted
  //                   ("export theme: wrote <N> icons to '<path>'").
  //                   No side effects on m_project state — export
  //                   produces a tree of side artefacts, the loaded
  //                   project is unchanged. No update_title().
  //   NoProject     — m_project is null. Same edge case as svg's
  //                   NoProject branch.
  //   NoProjectPath — m_project is loaded but
  //                   m_project->directory is empty. This branch
  //                   is NEW for theme (not present on svg / png)
  //                   because theme derives theme_name from the
  //                   project directory's basename; without a
  //                   directory there's no usable theme_name.
  //                   Mirrors the "save before exporting a theme"
  //                   condition the GUI enforces by greying out
  //                   the Export action on an unsaved project.
  //                   Structured refusal points the script author
  //                   at proj save_as. NOTE: NoActiveDoc is
  //                   DELIBERATELY ABSENT from this enum — theme
  //                   exports EVERY document, not the active one;
  //                   a project with zero documents fails inside
  //                   the helper's "no valid entries" check and
  //                   surfaces as IoFailed, with the GUI's
  //                   familiar "No icons to export" message
  //                   captured as the error_message.
  //   IoFailed      — export_theme() returned with success=false.
  //                   Mirrors the helper's failure paths: cannot
  //                   create output_dir (ensure_dir failed); no
  //                   valid icons after filtering by export_name
  //                   / export_category; every per-document SVG/
  //                   PNG write failed. The helper's
  //                   error_message is passed through to the
  //                   Scriptable for inclusion in the throw
  //                   message so the script author sees the same
  //                   diagnostic the GUI user would.
  enum class ScriptExportThemeResult {
    Ok,
    NoProject,
    NoProjectPath,
    IoFailed,
  };

  // Run the freedesktop icon-theme bundle export flow on behalf
  // of a script caller. `path` is assumed path_is_safe-vetted and
  // names the OUTPUT DIRECTORY (the helper creates the directory
  // if it doesn't exist). Entries are auto-derived from
  // m_project->documents (every doc with include=true);
  // theme_name from the project directory's basename (fallback
  // "MyIcons"); theme_comment empty.
  //
  // Side effects on Ok: a tree of files at `path` (scalable/
  // <category>/<name>-symbolic.svg + per-size <NxN>/<category>/
  // <name>-symbolic.png across 16/24/32/48/64/128/256 px +
  // gresource.xml + index.theme + README.md). LOG_INFO line
  // "export theme: wrote N icons to '<path>'". No project state
  // changes — the dirty indicator stays whatever it was,
  // m_project->directory is unchanged, m_history is unchanged.
  // The export is a side artefact; the project remains
  // identifiable by its own .curvz directory.
  //
  // On Ok, error_message_out is left unchanged. On IoFailed,
  // error_message_out is set to the helper's error_message
  // (one of: "Cannot create output directory: <path>", "No
  // icons to export. ...", "All icons failed to write. ...")
  // so the Scriptable can surface the precise reason. The
  // pointer-out signature mirrors path_is_safe's reason_out
  // — single OUT parameter, optional (callers that don't need
  // the string pass nullptr).
  //
  // No side effects on any non-Ok return — NoProject /
  // NoProjectPath are pre-check refusals; IoFailed leaves
  // whatever export_theme managed to produce on disk
  // (typically nothing on the ensure_dir-failed branch; a
  // partial tree on the per-document-write-failed branch).
  ScriptExportThemeResult script_export_theme(const std::string &path,
                                              std::string *error_message_out);

private:
  void setup_headerbar(); // category: zone: headerbar
  void setup_layout();    // category: zone: layout
  void setup_project();   // category: glue
  void setup_menu();      // category: zone: menu
  void connect_signals(); // category: bindings

  // s219 m1 — re-sync every surface tied to scripter_enabled. Called
  // once at the end of construction (after every relevant widget,
  // action, and pref subscription is in place) and again whenever
  // AppPreferences::signal_changed fires. Idempotent — flipping a
  // control to its current state is a no-op everywhere.
  //
  // Surfaces synced:
  //   - the headerbar Scripter toggle (m_scripter_btn): visible/hidden
  //   - the Developer ▸ Scripting menu action state (checkmark)
  //   - the Scripter window: present()/set_visible(false)
  // The inspector switch syncs itself when PropertiesPanel rebuilds
  // (the standard m_loading-guarded pattern); it reads scripter_enabled()
  // at construction time of each row.
  void apply_scripter_pref(); // category: glue: scripter integration

  void load_project(std::unique_ptr<CurvzProject>
                        project); // category: helper: flow orchestrator
  void update_all_panels();       // category: helper: display sync
  void update_title();            // category: helper: display sync
  void update_rulers();           // category: helper: display sync
  void refresh_inspector();       // category: helper: display sync

  // s144 m3 — Open Recent submenu rebuild. Called on every
  // RecentProjects::signal_changed emit (project open, save-as,
  // clear). remove_all() the stored m_recent_menu, then re-append
  // one item per recents path plus a Clear Recent Projects entry.
  // Cheap: typical list is 0..10 entries.
  void rebuild_recents_menu(); // category: helper: menu support
  // s132 m2: single funnel for the StatusBar's "N objects · N nodes"
  // readout. Replaces five duplicated "iterate doc.layers, sum
  // children.size()" loops that all hardcoded `nodes=0`. Computes
  // counts via curvz::utils::doc_object_count / doc_anchor_count and
  // pushes them to m_statusbar.set_counts. Safe to call when there is
  // no active document (sets 0 / 0).
  void refresh_status_counts();     // category: helper: display sync
  void toggle_rulers(bool visible); // category: helper

  // Apply the active project's motif to the main window — adds or
  // removes the `curvz-light` CSS class based on `m_project->motif`.
  // Reading CSS tokens defined in css.hpp re-resolves automatically
  // when the class changes, so the entire stylesheet flips. Idempotent:
  // calling when state already matches is a no-op (CSS class membership
  // is a set). Called from on_doc_activated, update_all_panels, and the
  // prop_changed handler so any path that changes the project's motif
  // keeps the window in sync.
  //
  // Motif is project-scope (s116 m6) — switching tabs within the same
  // project never changes the app theme. No-op when no project is
  // loaded.
  void apply_motif_to_window(); // category: helper: window state

  // S117 m15: apply (or remove) the `curvz-light` CSS class on a single
  // top-level window based on the current project's motif. Used by
  // apply_motif_to_window to walk every existing app window, and by the
  // application's `signal_window_added` hook so newly-opened dialogs
  // immediately wear the right class. Independent dialogs (any
  // Gtk::Window we create — Themes, Export, Macro Manager, etc.) are
  // separate top-level windows from MainWindow, so the class doesn't
  // propagate down naturally; we propagate it explicitly here.
  void sync_motif_class_to(Gtk::Window &w); // category: helper: window state

  // Shared rename handler used by both the inspector "File" entry and the
  // documents-gallery double-click / context-menu rename. Sanitises the
  // name (spaces → dashes, strips path separators), enforces uniqueness
  // within the project, renames on disk if the project is saved, and
  // updates UI. Caller passes the document explicitly so this works for
  // any doc, not just the active one.
  void rename_doc(CurvzDocument *doc, std::string new_name); // category: glue

  void on_new();                   // category: handler: documents
  void on_new_project();           // category: handler: documents
  void on_close_project();         // category: handler: documents
  void on_open();                  // category: handler: documents
  void update_project_sensitive(); // category: helper: action predicate
  void check_unsaved_then(
      std::function<void()> then); // category: helper: flow orchestrator
  void on_save();                  // category: handler: documents
  void on_save_as();               // category: handler: documents
  void on_save_as_template();      // category: handler: library
  void on_manage_templates();      // category: handler: library
  // s147 m3: on_show_themes removed — ThemesPanel in Content is the
  // canonical surface, no menu entry / dialog version remains.
  void on_export(); // category: handler: documents — File ▸ Export… (unified)

  // S104 m1 follow-on — NewDocumentDialog "Theme" dropdown helpers.
  //
  // ndd_available_themes() flattens the user-tier of the project's
  // theme library into a vector<Theme> in the order ThemesDialog
  // displays them (categories × themes-in-category). Returns empty
  // when there's no project. Called by every NDD show() site to
  // populate the dropdown.
  //
  // ndd_apply_chosen_theme() looks up the theme by id and writes its
  // settings into the seed via apply_theme_to_doc(). No-op when id
  // is nullopt or the theme is not found (defensive — if the theme
  // was deleted between NDD opening and Create being clicked,
  // silently fall through to the un-themed seed).
  std::vector<theme::Theme>
  ndd_available_themes() const; // category: helper: NDD support
  void ndd_apply_chosen_theme(CurvzDocument &seed,
                              const std::optional<theme::ThemeId> &id)
      const; // category: helper: NDD support

  void on_import_svg();         // category: handler: documents
  void on_import_svg_as_icon(); // category: handler: documents
  // Shared impl.
  //   force_currentcolor=true → convert Solid fill/stroke to CurrentColor
  //                             (icon workflow).
  //   normalize_to_1000=true  → rescale coords so long axis = 1000 units
  //                             (icon workflow). When false, preserve the
  //                             SVG's authored geometry verbatim.
  void
  import_svg_impl(const std::string &path, bool force_currentcolor,
                  bool normalize_to_1000); // category: helper: import shared
  void on_place_image();                   // category: handler: documents
  // s125 m1d (was m1c on_place_image_as_doc, renamed): routes to
  // import_image_to_canvas with fit_canvas_to_image=true. Wired to
  // win.open-image and the File → Open Image menu entry. Sibling of
  // on_open in user model — both create a new doc.
  void on_open_image(); // category: handler: documents
  void on_save_selection_to_library(
      const std::string &dest_dir); // category: handler: library
  // s125 m1a: opens a folder picker (mirrors LibraryPanel::on_add_clicked)
  // and routes the chosen directory to on_save_selection_to_library.
  // Wired to Canvas::signal_request_save_to_library, fired from the
  // canvas right-click "Save to Library…" entry.
  void on_request_save_selection_to_library(); // category: handler: library
  // s136 m4: actual library file write. Pure helper — takes a destination
  // directory and a base name (without extension), writes
  // `<dest_dir>/<base_name>.svg`. Returns true on success. The orchestrator
  // (on_save_selection_to_library) handles the name prompt and any
  // collision resolution; this helper assumes base_name is final.
  bool
  write_library_item(const std::string &dest_dir,
                     const std::string &base_name); // category: helper: library
  void on_step_repeat(); // category: handler: effects
  // s154 m2: toolbar SnR left-click path — applies the popover's
  // persisted last values without showing UI. on_step_repeat() is
  // the menu-item / right-click path; this one is the left-click.
  void apply_step_repeat_with_last(); // category: handler: effects
  // Blend orchestrator — validates selection, reads A/B node counts
  // and stroke widths, shows BlendPopover (s154 m3), forwards Result
  // to Canvas::make_blend on OK. If selection is invalid on action-fire
  // (shouldn't happen thanks to the sensitivity hook, but belt-and-
  // braces) shows a user-visible message and returns.
  void on_blend(); // category: handler: effects
  // s154 m3: toolbar Blend left-click path. Applies popover-last-values
  // when node counts already match; falls back to opening the popover
  // when validation requires the warning banner / Equalize button.
  void apply_blend_with_last(); // category: handler: effects
  void on_warp_make();          // category: handler: effects
  void on_warp_edit();          // category: handler: effects
  void on_warp_release();       // category: handler: effects
  void on_warp_flatten();       // category: handler: effects
  // Guide construct (M3) — open the review dialog after the user clicks p2.
  void open_guide_review_dialog();                // category: handler: guides
  void close_guide_review_dialog();               // category: handler: guides
  void on_macro_manager();                        // category: handler: macros
  void on_run_macro(const std::string &macro_id); // category: handler: macros
  void on_quit(); // category: handler: documents
  // s247 m1 — return type bool (was void). True iff
  // CurvzProject::save() returned true and save_config /
  // update_title ran. Existing GUI call sites in on_save_as discard
  // the return; the script-side caller (script_save_project_as)
  // uses it to surface IoFailed honestly to the DSL. The semantics
  // for both callers are unchanged: on failure, the project's
  // directory has already been mutated (assignment happens before
  // the save attempt) and a LOG_ERROR line has been emitted.
  bool do_save_as(const std::string &dir); // category: helper: save flow
  // s248 m1 — extracted from on_close_project's check_unsaved_then
  // lambda body. The teardown work (drop project, clear command
  // history, clear every panel, remove config file, refresh title
  // and project-sensitive actions, log) lives here as a single
  // method so on_close_project and script_close_project can both
  // call it from a single source of truth. Same pump-at-the-seam
  // pattern as do_save_as (s247 m1). No args, no return — the
  // teardown is unconditionally successful once preconditions
  // (project loaded, not dragging, not dirty) have been verified by
  // the caller.
  void do_close_project();               // category: helper: project lifecycle
  void on_tool_changed(ActiveTool tool); // category: glue
  void on_doc_activated(int index);      // category: glue
  void cycle_doc(int delta);             // category: glue
  void on_undo();                        // category: handler: edit
  void on_redo();                        // category: handler: edit

  // Build a collapsible inspector section
  Gtk::Box *make_section(const char *title, Gtk::Widget &child,
                         bool expanded = true, bool vexpand_child = false,
                         std::shared_ptr<bool> *out_flag =
                             nullptr); // category: helper: inspector pump

  // Build a collapsible GROUP header (Document/Object/Content style).
  // Returns {outer, container}: append `outer` where you want the group
  // to live; append child sections into `container`.  `container` carries
  // the inspector-group-container CSS class for indentation.
  struct GroupSection {
    Gtk::Box *outer;
    Gtk::Box *container;
  };
  GroupSection
  make_group_section(const char *title, bool expanded,
                     std::shared_ptr<bool> *out_flag =
                         nullptr); // category: helper: inspector pump

  // ── State ─────────────────────────────────────────────────────────────
  std::unique_ptr<CurvzProject> m_project;
  CommandHistory m_history;
  NewDocumentDialog m_new_doc_dialog;
  StepRepeatPopover m_step_repeat_popover;
  BlendPopover m_blend_popover;
  WarpPopover m_warp_popover;
  MacroManagerWindow m_macro_manager;
  MacroEditorWindow m_macro_editor;
  PrintManager m_print_manager;
  OffsetPathDialog m_offset_path_dialog;
  TranslateDialog m_translate_dialog; // s205 m4
  GradientDialog m_gradient_dialog;
  SaveAsTemplateDialog m_save_as_template_dialog;
  ManageTemplatesDialog m_manage_templates_dialog;
  ThemeEditDialog m_theme_edit_dialog;              // s200 m1
  StyleEditorDialog m_style_editor_dialog;          // s201 m1
  ImageInfoDialog m_image_info_dialog;              // s210 m1
  RotateFromPointDialog m_rotate_from_point_dialog; // s210 m2
  ShortcutsDialog m_shortcuts_dialog;
  HelpWindow m_help_window;
  ProgressDialog m_progress_dialog; // s277 m1

  // Guide-construct review dialog (M3) — created lazily.  A tiny non-modal
  // window with Perpendicular checkbox + OK + Cancel.
  // s208 m5: substrate CheckButton. Forward-declared below; full include
  // in MainWindow_handlers.cpp at the construction site.
  std::unique_ptr<Gtk::Window> m_guide_review_win;
  curvz::widgets::CheckButton *m_guide_review_perp_chk = nullptr;

  // s202 m6 — quick-jump float, created lazily on first Ctrl+Space.
  // Held as unique_ptr<Gtk::Window> so the type doesn't bloat
  // MainWindow.hpp; the impl lives in MainWindow_helpers.cpp
  // alongside the focus_inspector_on cascade it triggers.
  std::unique_ptr<Gtk::Window> m_quick_jump_win;

  // s203 m1 — View Clipboard mini float. Owned here so the action
  // handler can lazily build, refresh, and re-show. ClipboardViewWindow
  // is a real subclass (not a bare Gtk::Window) because it has its own
  // refresh() method and async-read state; the type is small enough
  // that including the header in MainWindow.hpp is fine.
  std::unique_ptr<ClipboardViewWindow> m_clipboard_view_win;
  ActiveTool m_active_tool = ActiveTool::Selection;
  ActiveTool m_inspector_tool = ActiveTool::Selection;
  // s268 — track last applied motif so apply_motif_to_window can short-
  // circuit when the motif hasn't actually changed. signal_prop_changed
  // calls apply_motif_to_window on every inspector edit ("just in case
  // motif was the edited prop"); without this gate every node-coord
  // spinner click triggered a full motif walk + inspector rebuild,
  // destroying the spinner mid-click and killing GTK4's auto-repeat.
  // std::optional so the first real call always runs.
  //
  // s277 m1 — ported from stray src/MainWindow.hpp during the
  // ProgressDialog header sweep. The s268 edit had landed in the
  // wrong file (CMake's include path resolves to include/, so the
  // src/ copy was off-path); MainWindow_helpers.cpp:766 references
  // this field, so the canonical header needs it.
  std::optional<Motif> m_last_applied_motif;
  // S58m: remember previous selection size so shift-click add/remove
  // (which keeps the primary pointer but changes the set) triggers an
  // inspector rebuild. Without this the Appearance panel stays showing
  // the old mixed/uniform state.
  size_t m_prev_selection_size = 0;
  bool m_closing = false;
  bool m_pane_ready = false;
  gint64 m_pane_settled_at = 0; // microseconds, from g_get_monotonic_time()
  bool m_rulers_visible = true; // toggled by View → Rulers / Ctrl+R
  // s165 m3 — re-entrancy guard for on_undo / on_redo. Set true at entry,
  // cleared at exit. Suppresses re-entry when a Ctrl+Z keypress arrives
  // while a previous undo is still mid-flight (e.g. partway through
  // m_history.undo() or one of the post-undo refreshes). Without this,
  // rapid Ctrl+Z spam can re-enter on_undo on a partially-mutated tree
  // and crash. Belt-and-braces — also protects against signal handlers
  // queued during refresh that fire back into on_undo before we exit.
  bool m_undo_in_progress = false;
  // s165 m3 — chrono trap on rapid undo/redo presses. The undo system has
  // a structural issue where queue eviction (cap=500 by default) destroys
  // command storage that other queued commands reference via raw pointers
  // — rapid Ctrl+Z that traverses many history entries can hit one of
  // those dangling references and crash. Until commands are reworked to
  // capture by id rather than raw pointer, throttle the input rate so
  // each undo has time to settle (refresh, scrub, paint) before the next
  // is accepted. Threshold: 80ms — fast enough that deliberate presses
  // never get dropped, slow enough to absorb keyboard auto-repeat.
  std::chrono::steady_clock::time_point m_last_undo_redo_at = {};

  // ── Widgets ───────────────────────────────────────────────────────────
  Gtk::HeaderBar m_headerbar;
  Gtk::MenuButton m_hamburger; // ☰ — opens the app menu popover
  Gtk::Button m_logo_btn;      // App logo — opens About dialog

  // s144 m3 — Open Recent submenu. Held as a member so
  // rebuild_recents_menu() can remove_all() and re-append on every
  // RecentProjects::signal_changed emit. Same Gio::Menu instance for
  // the lifetime of the window; only its contents churn.
  Glib::RefPtr<Gio::Menu> m_recents_menu;
  // Captured at action-creation so rebuild_recents_menu() can toggle
  // the Clear Recent Projects entry's enabled state without a
  // lookup_action_group() round-trip — that API has no precedent in
  // this codebase, the direct ref is simpler and verifiable.
  Glib::RefPtr<Gio::SimpleAction> m_recents_clear_action;
  Gtk::Box m_root{Gtk::Orientation::VERTICAL};
  DocumentGallery m_gallery; // kept for thumbnail rendering only
  DocTabBar m_doc_tabs;
  Gtk::Box m_middle{Gtk::Orientation::HORIZONTAL};
  Toolbar m_toolbar;
  Gtk::Box m_canvas_col{Gtk::Orientation::VERTICAL}; // context bar + paned
  ContextBar m_context_bar;
  Gtk::Paned m_h_paned{Gtk::Orientation::HORIZONTAL};
  // Canvas area: corner + rulers in a Grid, all wrapped in an Overlay
  // so the floating text-tool entry can sit above the canvas.
  Gtk::Overlay m_canvas_overlay;
  Gtk::Fixed m_text_fixed; // overlay layer for text entry widget
  Gtk::Grid m_canvas_grid;

  // ── Corner Treatment panel (Popover on corner tool button) ────────────────
  Gtk::Popover m_corner_panel;
  bool m_corner_panel_visible = false; // s194_m1: edge-guard tracker
  Gtk::Box m_corner_panel_vbox{Gtk::Orientation::VERTICAL};
  Gtk::Box m_corner_type_row{Gtk::Orientation::HORIZONTAL};
  Gtk::ToggleButton m_corner_btn_round;
  Gtk::ToggleButton m_corner_btn_chamfer;
  Gtk::ToggleButton m_corner_btn_inverse;
  Gtk::Box m_corner_radius_row{Gtk::Orientation::HORIZONTAL};
  Gtk::Label m_corner_radius_label;
  CurvzSpinButton m_corner_radius_spin;
  Glib::RefPtr<Gtk::Adjustment> m_corner_radius_adj;
  Gtk::Label m_corner_unit_label;
  Gtk::Button m_corner_apply_btn;

  void build_corner_panel();           // category: zone: overlays
  void show_corner_panel(bool show);   // category: zone: overlays
  void update_corner_panel_position(); // category: zone: overlays

  // System icon preview/copy
  // force_currentcolor=true → convert all Solid fills/strokes to CurrentColor
  // (default true because this is the icon-gallery copy flow).
  bool import_svg_as_doc(
      const std::string &path,
      bool force_currentcolor = true); // category: helper: import shared
  void on_preview_icon(const std::string &path); // category: handler: documents
  void on_copy_icon(const std::string &path);    // category: handler: documents
  void exit_preview_mode();                      // category: helper
  double pop_to_px(double v) const;              // category: helper

  bool m_preview_active = false;
  int m_preview_saved_index = 0;
  std::unique_ptr<CurvzDocument> m_preview_doc; // temp doc while previewing
  CornerSquare m_corner;
  HRuler m_hruler;
  VRuler m_vruler;
  Canvas m_canvas;
  Gtk::Box m_right_panels{Gtk::Orientation::VERTICAL};
  Gtk::ScrolledWindow m_right_scroll;

  PropertiesPanel m_properties;
  PreviewPanel m_preview;
  LayersPanel m_layers;
  LibraryPanel m_library;
  SwatchesPanel m_swatches;
  StylesPanel m_styles;
  ThemesPanel m_themes;

  // s191 m3 / s219 m1 — caption bar. Sits as an overlay on the canvas
  // (added in setup_layout); revealed by set_subtitle(). See the
  // public method's comment for the wiring story. Always compiled
  // as of s219 m1; only animates when a script runs.
  Gtk::Revealer m_caption_revealer;
  Gtk::Label m_caption_label;

  // s216 m1 / s219 m1 — `layers` collection Scriptable. One registry entry per
  // app session; transient per-instance `layer.<iid>` proxies route
  // through this object's `proxy_for`. Holds a project-getter lambda
  // that resolves `m_project.get()` on every verb call, so the
  // Scriptable survives File→Open / File→Close without re-registering.
  // Held as unique_ptr because LayersScriptable is forward-declared
  // (see fwd-decl above the namespace) — only the .cpp side sees the
  // complete type. Constructed in MainWindow's ctor, destroyed in
  // the out-of-line dtor. Always present as of s219 m1.
  std::unique_ptr<curvz::scripting::LayersScriptable> m_layers_scriptable;
  // s218 m1 / s219 m1 — `guides` collection Scriptable, second row-bound model
  // Scriptable. Same lifetime / construction / destruction shape as
  // m_layers_scriptable; transient per-instance `guides.<iid>` proxies
  // route through this object's `proxy_for`. Held as unique_ptr for
  // the same forward-decl reason.
  std::unique_ptr<curvz::scripting::GuidesScriptable> m_guides_scriptable;
  // s221 m1 — `swatches` collection Scriptable, third row-bound model
  // Scriptable. First library-collection Scriptable (layers/guides
  // wrap SceneNode collections; this one wraps the project's
  // SwatchLibrary). Same lifetime / construction / destruction shape
  // as the two above; transient per-instance `swatches.<id>` proxies
  // route through this object's `proxy_for`. Unlike GuidesScriptable's
  // captured-but-unused history pointer, this one IS dereferenced —
  // s220 m1a made swatch CRUD undoable and the Scriptable rides that
  // plumbing.
  std::unique_ptr<curvz::scripting::SwatchesScriptable> m_swatches_scriptable;
  // s243 m2 — `palettes` collection Scriptable, eighth row-bound model
  // Scriptable. Sibling of m_swatches_scriptable — both wrap the same
  // SwatchLibrary, just opposite ends of it (the library holds two
  // parallel two-tier pools, swatches and palettes). Closes the s243
  // arc on top of the s243 m1 palette-CRUD command quintet
  // (AddPaletteCommand / RemovePaletteCommand / RenamePaletteCommand /
  // PaletteMembershipCommand). Same lifetime / construction /
  // destruction shape as m_swatches_scriptable; transient per-instance
  // `palettes.<iid>` proxies route through this object's `proxy_for`.
  //
  // history pointer is DEREFERENCED on every mutating CRUD verb
  // (new / delete / duplicate / rename) — s243 m1 made every palette
  // CRUD undoable and the Scriptable rides exactly that plumbing.
  // The `activate` verb is the one mutator that doesn't push a
  // command — set_active_palette is transient working state,
  // deliberately outside undo (matches the panel's dropdown click).
  //
  // No PanelGetter argument — SwatchesPanel listens on the s243 m1
  // palette signal trio (signal_palette_added / _removed / _changed)
  // and rebuilds the dropdown + grid wholesale on any fire. Scripted
  // palette mutations show up automatically. Fourth application of
  // the visibility canon entry (s221 swatches: library-side fix;
  // s222 styles: panel-side fix; s223 themes: no fix needed because
  // the panel doesn't filter; s243 palettes: same as themes — the
  // panel's library-signal listeners are sufficient).
  std::unique_ptr<curvz::scripting::PalettesScriptable> m_palettes_scriptable;
  // s222 m1 — `styles` collection Scriptable, fourth row-bound model
  // Scriptable. Second library-collection Scriptable (sibling of
  // m_swatches_scriptable; both wrap project-scoped libraries rather
  // than per-doc SceneNode collections). Same lifetime / construction
  // / destruction shape as the three above; transient per-instance
  // `styles.<id>` proxies route through this object's `proxy_for`.
  // history pointer is DEREFERENCED on every mutating verb — the
  // S81 m4c-3 commands (AddStyleCommand / UpdateStyleCommand /
  // RemoveStyleCommand) cover every CRUD verb, exactly like the
  // swatch Scriptable rides the s220 m1a swatch commands.
  //
  // s222 m1 fix-1: this Scriptable also carries a StylesPanel
  // getter (constructed in MainWindow.cpp pointing at m_styles)
  // for after-mutation navigation on `new` and `duplicate`. The
  // mechanism differs from the swatches fix-1 (library-side
  // add_to_palette) because the styles panel filters by panel
  // state (m_active_category), not by library state.
  std::unique_ptr<curvz::scripting::StylesScriptable> m_styles_scriptable;
  // s223 m1 — `themes` collection Scriptable, fifth row-bound model
  // Scriptable. Third library-collection variant (sibling of
  // m_swatches_scriptable and m_styles_scriptable; all three wrap
  // project-scoped libraries rather than per-doc SceneNode
  // collections). Same lifetime / construction / destruction shape
  // as the four above; transient per-instance `themes.<id>` proxies
  // route through this object's `proxy_for`. history pointer is
  // DEREFERENCED on every mutating verb — the S103 m2 commands
  // (AddThemeCommand / UpdateThemeCommand / RemoveThemeCommand) cover
  // every CRUD verb, exactly like the styles Scriptable rides the
  // S81 m4c-3 style commands.
  //
  // No PanelGetter argument — see ThemesScriptable.hpp's "Panel
  // visibility" block. ThemesPanel renders the full user-tier theme
  // list as a flat vertical box and rebuilds on signal_theme_added /
  // _changed / _removed; library-side mutation is sufficient for new
  // rows to appear. Third application of the visibility canon entry
  // (s221 swatches: library-side fix; s222 styles: panel-side fix;
  // s223 themes: no fix needed because the panel doesn't filter).
  // Construction surface is correspondingly two-arg (matches
  // m_swatches_scriptable; differs from m_styles_scriptable's three).
  std::unique_ptr<curvz::scripting::ThemesScriptable> m_themes_scriptable;
  // s230 m1 — `objects` collection Scriptable, sixth row-bound model
  // Scriptable. OPENS the multi-session `objects` arc — the remaining
  // row-bound Tier 3 type for the SceneNode tree at document level.
  // Wraps every "real scene content" SceneNode (Path / Group /
  // Compound / ClipGroup / Blend / Warp / Text / Image / Ref /
  // Measurement) across all layers in the active document via a
  // recursive tree walk. Layers / guides / special-layer types are
  // explicitly out of scope (they have their own Scriptables); see
  // ObjectsScriptable.hpp's scope discussion.
  //
  // Same lifetime / construction / destruction shape as the five
  // above; transient per-instance `objects.<iid>` proxies route
  // through this object's `proxy_for`. m1 ships READ-SIDE only —
  // collection queries (count, all_iids) plus invoke-shaped reads
  // (find_by_name, find_by_type), per-instance proxy queries (name,
  // type, visible, locked, parent_iid, child_count, iid). No
  // mutating verbs anywhere yet; those land in m3+ riding the
  // EditObjectCommand surfaces that already exist for the inspector
  // and canvas drivers.
  //
  // history pointer is captured but UNUSED in m1 (no mutating verb
  // pushes a command). Construction is two-arg, matching
  // m_swatches_scriptable / m_themes_scriptable. The history wiring
  // is in place for m3+ — no ctor signature churn when verbs land.
  std::unique_ptr<curvz::scripting::ObjectsScriptable> m_objects_scriptable;
  // s222 m2 — `inspector` Scriptable, wraps the inspector-area
  // section open/close orchestration (collapse_all + open). NOT a
  // model-collection Scriptable (no proxy_for surface, no per-row
  // addressing); flat verb surface that delegates to MainWindow's
  // existing collapse_all_inspector_sections / apply_section_open
  // methods. Same lifetime / dtor story as the model Scriptables
  // above; the dtor stays out-of-line so the unique_ptr can hold an
  // incomplete type. See InspectorScriptable.hpp for the design
  // block (in particular, why this isn't a verb on pnl_styles or a
  // PropertiesPanel-only Scriptable).
  std::unique_ptr<curvz::scripting::InspectorScriptable> m_inspector_scriptable;

  // s246 m1 — `proj` Scriptable, first headless-verb singleton from
  // ARC m5b (Tier 4). Sibling of InspectorScriptable in shape — flat
  // verb surface, no proxy routing, MainWindow-pointer constructor —
  // but wraps a different concern (the project-level save outcome
  // rather than the inspector-area section orchestration). m1 ships
  // one verb (`save`) and two queries (`path`, `has_path`); save_as /
  // load / new / close land in s247+. Same lifetime / dtor story as
  // the Scriptables above; the dtor stays out-of-line so the
  // unique_ptr can hold an incomplete type. See ProjScriptable.hpp
  // for the verb surface, the three-branch refusal contract on
  // `save`, and the RunContext mask rationale (Scripter | TestRunner;
  // Macro is OUT per CANON's RunContext pseudocode). Also the
  // second consumer of Scriptable::context_mask() — when a third
  // consumer earns the refactor, the virtual-method-per-Scriptable
  // shape promotes to a central register_verb(name, mask) table.
  std::unique_ptr<curvz::scripting::ProjScriptable> m_proj_scriptable;

  // s251 m1 — `export` Scriptable, second headless-verb singleton from
  // ARC m5b (Tier 4 row ticks 1/~4-5 → 2/~4-5). Sibling of
  // ProjScriptable in shape — flat verb surface, no proxy routing,
  // MainWindow-pointer constructor — but wraps a different concern
  // (the export-format-bearing dialog's per-format writers, not the
  // project lifecycle). m1 ships one verb (`svg <path>`) and two
  // queries (`last_path`, `last_ok`); the remaining four format-
  // verbs (png / theme / refpt / gresource) land in m2+ as the
  // surface widens. Same lifetime / dtor story as the Scriptables
  // above; the dtor stays out-of-line so the unique_ptr can hold
  // an incomplete type. See ExportScriptable.hpp for the verb
  // surface, the five-branch refusal contract on `svg`, and the
  // RunContext mask rationale (Scripter | TestRunner — same as
  // proj save, not proj save_as; export produces a side artefact
  // that does not rewrite the project's identity). Seventh
  // consumer of Scriptable::context_mask() — registry-promotion
  // clock at 7/n, still held by design.
  std::unique_ptr<curvz::scripting::ExportScriptable> m_export_scriptable;

  // s263 m2 — `app` Scriptable, third headless-verb singleton from
  // ARC m5b (Tier 4 row ticks 2/~4-5 → 3/~4-5). Sibling of
  // ProjScriptable and ExportScriptable in shape — flat verb
  // surface, no proxy routing, MainWindow-pointer constructor — but
  // wraps a different concern (process-scope identity: build
  // version baked at compile time, runtime gtkmm version linked at
  // process start). m2 ships one verb (`version`); m3 adds
  // `gtk_version` as the second pure-read verb on the same
  // singleton. Both verbs are pure-read with the Scripter |
  // TestRunner mask — Macro is OUT (recorded-macro replay on a
  // different build / system means the recorded value is stale at
  // replay time, violating the recorded-macro mental model). Tenth
  // and eleventh consumers of Scriptable::context_mask() across
  // the corpus — registry-promotion clock at 10/n, still held by
  // design (catalogue uniformising further; no novel mask shape).
  // Same lifetime / dtor story as the Scriptables above; the dtor
  // stays out-of-line so the unique_ptr can hold an incomplete
  // type. MainWindow pointer at construction is unused in m2 / m3
  // (both verbs read process-scope state) but stashed for symmetry
  // with proj / export and for future verbs (`app.quit`, etc.).
  // See AppScriptable.hpp for the verb surface, the no-refusal
  // contract on both verbs, and the trust-profile rationale.
  std::unique_ptr<curvz::scripting::AppScriptable> m_app_scriptable;

  // s254 m2 — `win` ActionGroupScriptable, first move on Tier 2 action
  // wrappers. Holds the wrap-now subset of MainWindow's win.* actions
  // (the 49 actions classified as wrap-now in s254 m1's
  // tier2_action_audit.md), exposing them as Scriptable verbs so
  // scripts can address `win.undo`, `win.zoom-fit`, etc. through the
  // dispatcher. m2 ships the wrapper plumbing plus the first five
  // wrap-now actions migrated (`undo`, `redo`, `select-all`,
  // `deselect-all`, `zoom-fit`) as proof; subsequent milestones
  // sweep the remaining 44 by menu domain. Same lifetime / dtor
  // story as the Scriptables above; the dtor stays out-of-line so
  // the unique_ptr can hold an incomplete type. Constructed BEFORE
  // create_actions runs (in MainWindow's ctor) so the wrap-now
  // sites in zones can register against a live group_scriptable.
  // See scripting/Action.hpp for the wrapper-class contract, the
  // one-Scriptable-per-action-group mapping rationale, and the
  // helper-replaces-three-line-pattern motivation. The
  // ActionGroupScriptable adds an eighth consumer of context_mask()
  // — registry-promotion clock now at 8+/n (the per-verb mask
  // catalogue inside this Scriptable's m_actions map is the larger
  // contributor; each wrapped action declares its own mask). Still
  // held by design — see the audit doc's Forks section for the
  // mask-shape posture (default Scripter | TestRunner for wrap-now
  // actions; the unsafe bucket C entries are decided-not-deferred
  // and stay GUI-only without appearing in the wrapper's verb
  // surface at all).
  std::unique_ptr<curvz::scripting::ActionGroupScriptable>
      m_action_group_scriptable;

  // s219 m1 — Scripter window. Owned by MainWindow as a unique_ptr
  // member, matching the pattern of every other persistent floating
  // dialog (HelpWindow, ShortcutsDialog, BlendDialog, MacroEditorWindow,
  // MacroManagerWindow are value members; this one is unique_ptr
  // because ScripterWindow is forward-declared to keep MainWindow.hpp
  // free of scripting includes).
  //
  // Constructed at the end of MainWindow's ctor, destroyed by the
  // out-of-line dtor. The X-button close hides via set_hide_on_close
  // (the window's ctor sets that). MainWindow's show_scripter()
  // method is the canonical entry for showing or hiding it.
  //
  // Previously the Scripter was owned by Application and add_window'd
  // to the Gtk::Application directly. That arrangement made mutter
  // treat the Scripter as a peer top-level rather than a secondary
  // of MainWindow, with the visible consequence that hide/show
  // cycles left the titlebar decorations unresponsive. Moving
  // ownership down into MainWindow puts the Scripter on the same
  // window-relationship footing as every other dialog in the app.
  std::unique_ptr<curvz::scripting::ScripterWindow> m_scripter;

  // s219 m1 — headerbar Scripter toggle (the "monkey button"). Held
  // as a managed pointer so MainWindow can hide/show it in response
  // to AppPreferences::scripter_enabled changes. Constructed in
  // setup_headerbar() and packed into m_headerbar; visibility is
  // driven by apply_scripter_pref(). Type forward-declared above.
  curvz::widgets::ToggleButton *m_scripter_btn = nullptr;

  // s219 m1 — stateful menu action for Developer ▸ Scripting. Held
  // as a member so apply_scripter_pref() can update its state when
  // the pref changes from any other surface (the inspector switch,
  // or a future scripted toggle). The action lives in the window's
  // "win" action group; this ref is just for fast state updates.
  Glib::RefPtr<Gio::SimpleAction> m_act_toggle_scripting;

  StatusBar m_statusbar;

  // Inspector section open-state flags — set by make_section, used by
  // load_project
  std::shared_ptr<bool> m_sec_open_preview;
  std::shared_ptr<bool> m_sec_open_layers;
  std::shared_ptr<bool> m_sec_open_library;
  std::shared_ptr<bool> m_sec_open_documents;
  std::shared_ptr<bool> m_sec_open_swatches;
  std::shared_ptr<bool> m_sec_open_styles;
  std::shared_ptr<bool> m_sec_open_themes;
  std::shared_ptr<bool> m_sec_open_content;

  // s141: per-section "apply visual state" setters keyed by section title.
  // make_section / make_group_section register a closure here that flips
  // body->set_visible + arrow text in lock-step with the open_flag.
  // load_project calls each setter after sync_flag so the widgets match
  // the just-loaded project's saved state. Without this, sync_flag only
  // updates the in-memory bool — the widget tree stays in whatever state
  // setup_layout built it in (collapsed by default).
  std::unordered_map<std::string, std::function<void(bool)>> m_sec_apply;

  // App-level config (last opened project path)
  std::string config_path() const;      // category: helper: persistence
  void save_config() const;             // category: helper: persistence
  std::string load_last_project_path(); // category: helper: persistence

  // s125 m1e: per-purpose "last folder used" memory for file pickers.
  // Keyed by a stable purpose string (typically the action name —
  // "open-image", "place-image", "save-as", etc.). Persisted in
  // settings.json alongside the rest of app-level config; flushed on
  // project load, save-as, and quit (matches existing save_config call
  // sites). Folder, not file: pickers re-open at the same directory,
  // not pre-select the same file.
  //
  // Accessors are public (above) — see s126 last-folder wiring.
  std::map<std::string, std::string> m_last_folders;

  // Clip / Release Clip — held as members so we can toggle enabled
  // state on selection changes. Clip enabled iff selection non-empty;
  // Release Clip enabled iff primary selection is a ClipGroup.
  Glib::RefPtr<Gio::SimpleAction> m_act_clip_make;
  Glib::RefPtr<Gio::SimpleAction> m_act_clip_release;
  void update_clip_actions_sensitive(); // category: helper: action predicate

  // Blend — enabled iff exactly 2 Path nodes are selected (stricter
  // preconditions — same parent, equal node counts — enforced inside
  // Canvas::make_blend with user-visible error message on violation).
  // Release Blend — enabled iff primary selection is a Blend.
  Glib::RefPtr<Gio::SimpleAction> m_act_blend_make;
  Glib::RefPtr<Gio::SimpleAction> m_act_blend_release;
  void update_blend_action_sensitive(); // category: helper: action predicate

  // Warp — Make enabled iff exactly 1 Path/Compound/Group is selected.
  // Release / Flatten enabled iff primary selection is a Warp.
  // Deeper preconditions (if any) enforced inside Canvas::make_warp.
  Glib::RefPtr<Gio::SimpleAction> m_act_warp_make;
  Glib::RefPtr<Gio::SimpleAction> m_act_warp_edit;
  Glib::RefPtr<Gio::SimpleAction> m_act_warp_release;
  Glib::RefPtr<Gio::SimpleAction> m_act_warp_flatten;
  void update_warp_action_sensitive(); // category: helper: action predicate

  // Group / Ungroup (s138) — Make enabled iff >=2 nodes are selected;
  // Release enabled iff the primary selection is a Group. Wraps the
  // Canvas::group_selection / ungroup_selection methods that have
  // existed in the engine for some time but were never reachable from
  // the UI (no menu item, no action, no keybind). Surfaced when the
  // s138 m2 menu-accel fix made the Path submenu's gaps visible.
  Glib::RefPtr<Gio::SimpleAction> m_act_group_make;
  Glib::RefPtr<Gio::SimpleAction> m_act_group_release;
  void update_group_actions_sensitive(); // category: helper: action predicate

  // Boolean path ops (s122 m2) — Union/Subtract/Intersect enabled iff
  // exactly 2 Path or Compound nodes are selected. Deeper preconditions
  // (closed paths, common parent) are enforced inside Canvas::boolean_op
  // with user-visible error messages on violation. Hard-gating at 2
  // prevents triggering the not-yet-stable N>=3 iterative fold path.
  Glib::RefPtr<Gio::SimpleAction> m_act_bool_union;
  Glib::RefPtr<Gio::SimpleAction> m_act_bool_subtract;
  Glib::RefPtr<Gio::SimpleAction> m_act_bool_intersect;
  void update_bool_actions_sensitive(); // category: helper: action predicate

  // s135 m1: Align & Distribute actions. Mirror the Boolean ops pattern —
  // stored as members so the existing update_align_btn() predicate can flip
  // their enabled state alongside the toolbar button. Same gate
  // (selection >= 2 && tool == Selection). Distribute ignores the
  // align-anchor; align ops honour it (validator-on-read clears stale
  // anchors automatically).
  Glib::RefPtr<Gio::SimpleAction> m_act_align_left;
  Glib::RefPtr<Gio::SimpleAction> m_act_align_center_h;
  Glib::RefPtr<Gio::SimpleAction> m_act_align_right;
  Glib::RefPtr<Gio::SimpleAction> m_act_align_top;
  Glib::RefPtr<Gio::SimpleAction> m_act_align_center_v;
  Glib::RefPtr<Gio::SimpleAction> m_act_align_bottom;
  Glib::RefPtr<Gio::SimpleAction> m_act_distribute_h;
  Glib::RefPtr<Gio::SimpleAction> m_act_distribute_v;

  // Debounced auto-save — coalesces rapid changes into one write
  void schedule_save();          // category: helper
  sigc::connection m_save_timer; // pending debounce timer

  // s113 m2: gated outline-mode toggle. Refuses outline→preview when
  // current zoom would crash the app via drop-shadow buffer allocation;
  // shows an AlertDialog explaining how to proceed safely. Returns
  // true if the toggle happened (caller should sync action state +
  // statusbar), false if the toggle was refused.
  bool try_toggle_outline_safely(); // category: helper

  // Updates align button sensitivity; set in connect_signals, called from
  // on_tool_changed
  std::function<void()> m_update_align_btn;
};

} // namespace Curvz
