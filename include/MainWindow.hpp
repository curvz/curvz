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
#include "Canvas.hpp"
#include "Toolbar.hpp"
#include "StatusBar.hpp"
#include "DocumentGallery.hpp"
#include <functional>
#include <map>      // s125 m1e: m_last_folders
#include <unordered_map>  // s141: m_sec_apply
#include <optional>
#include <vector>
#include <chrono>   // s165 m3 — chrono trap on rapid undo presses
#include "PropertiesPanel.hpp"
#include "PreviewPanel.hpp"
#include "LayersPanel.hpp"
#include "LibraryPanel.hpp"
#include "SwatchesPanel.hpp"
#include "StylesPanel.hpp"
#include "ThemesPanel.hpp"
#include "ContextBar.hpp"
#include "DocTabBar.hpp"
#include "CurvzProject.hpp"
#include "CommandHistory.hpp"
#include "NewDocumentDialog.hpp"
#include "StepRepeatPopover.hpp"
#include "BlendPopover.hpp"
#include "WarpPopover.hpp"
// s146 m3 — WarpDialog removed; warp creation seeds from AppPreferences
// (Application ▸ Warp inspector subsection) and editing happens entirely
// in the Object ▸ Warp inspector section.
#include "MacroManagerWindow.hpp"
#include "MacroEditorWindow.hpp"
#include "PrintManager.hpp"
#include "OffsetPathDialog.hpp"
#include "GradientDialog.hpp"
#include "SaveAsTemplateDialog.hpp"
#include "ShortcutsDialog.hpp"
#include "HelpWindow.hpp"
#include "ManageTemplatesDialog.hpp"
#include "ThemeEditDialog.hpp"  // s200 m1 — hide-on-close singleton
#include "StyleEditorDialog.hpp"  // s201 m1 — hide-on-close singleton
#include "TranslateDialog.hpp"  // s205 m4 — pivot-aware transform hub
#include "ImageInfoDialog.hpp"  // s210 m1 — hide-on-close singleton
#include "RotateFromPointDialog.hpp"  // s210 m2 — hide-on-close singleton
#include "ClipboardViewWindow.hpp"  // s203 m1 — hide-on-close singleton
#include "Ruler.hpp"
#include <gtkmm/applicationwindow.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/box.h>
#include <gtkmm/grid.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/paned.h>
#include <gtkmm/revealer.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/popover.h>
#include <gtkmm/overlay.h>
#include <gtkmm/fixed.h>
#include <gtkmm/frame.h>
#include <gtkmm/label.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/entry.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/button.h>
#include <gtkmm/gesturedrag.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <memory>

// s208 m5 — forward-declare substrate CheckButton for the guide-review
// dialog's perp toggle (member-pointer here, constructed and dereferenced
// in MainWindow_handlers.cpp).
namespace curvz::widgets { class CheckButton; }

namespace Curvz {

class Application;

class MainWindow : public Gtk::ApplicationWindow {
public:
    explicit MainWindow(Application& app);

    // s126: last-used folder accessors. Public so non-MainWindow dialogs
    // (ExportDialog and friends) can opt into the same per-purpose
    // memory the built-in pickers use. Storage lives in m_last_folders
    // and persists via save_config.
    std::string get_last_folder(const std::string& purpose) const;  // category: helper: persistence
    void        set_last_folder(const std::string& purpose,
                                const std::string& path);  // category: helper: persistence

#ifdef CURVZ_DIAGNOSTIC
    // s191 m3 — caption bar driven by the Scripter's `#[sub]` lines.
    // Empty text hides the bar (reveals collapses). Non-empty shows
    // the text and reveals. Replacement is instant (no fade between
    // captions); the reveal animation only plays on show-from-empty
    // and hide-to-empty transitions.
    //
    // Application wires this to the ScriptListener's subtitle
    // callback after both MainWindow and ScripterWindow exist. The
    // diagnostic gate keeps production builds free of the surface.
    void set_subtitle(const std::string& text);  // category: zone: caption-bar

    // s201 m3 — panel accessors for the script-driven action-dispatch
    // verb. The Scripter's `do <prefix.action>` verb routes to whichever
    // panel owns the action group with that prefix (StylesPanel inserts
    // "styles", ThemesPanel inserts "themes-io", etc.). Application
    // installs the callback that performs the routing and needs widget
    // handles on each panel to call activate_action() through them —
    // GTK's action-group resolution walks up from the originating
    // widget, so the call must originate on the widget that holds the
    // group. These accessors are the route; they exist only in the
    // diagnostic build because nothing in production needs them.
    StylesPanel& styles_panel() { return m_styles; }
    ThemesPanel& themes_panel() { return m_themes; }
#endif

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
    void focus_inspector_on(const std::string& section_title);
    void show_quick_jump_popover();
    // ── end s202 m6 ────────────────────────────────────────────────────

    // s203 m1 — View Clipboard mini float (Edit ▸ View Clipboard…).
    // Lazy-builds m_clipboard_view_win on first invocation, then refresh-
    // and-show on subsequent ones. See ClipboardViewWindow.hpp for the
    // design rationale; the user-visible motivation is the Measure tool's
    // structured copy, which the user wants to dissect without a round-
    // trip through an external text editor.
    void show_clipboard_view();

private:
    void setup_headerbar();  // category: zone: headerbar
    void setup_layout();  // category: zone: layout
    void setup_project();  // category: glue
    void setup_menu();  // category: zone: menu
    void connect_signals();  // category: bindings

    void load_project(std::unique_ptr<CurvzProject> project);  // category: helper: flow orchestrator
    void update_all_panels();  // category: helper: display sync
    void update_title();  // category: helper: display sync
    void update_rulers();  // category: helper: display sync
    void refresh_inspector();  // category: helper: display sync

    // s144 m3 — Open Recent submenu rebuild. Called on every
    // RecentProjects::signal_changed emit (project open, save-as,
    // clear). remove_all() the stored m_recent_menu, then re-append
    // one item per recents path plus a Clear Recent Projects entry.
    // Cheap: typical list is 0..10 entries.
    void rebuild_recents_menu();  // category: helper: menu support
    // s132 m2: single funnel for the StatusBar's "N objects · N nodes"
    // readout. Replaces five duplicated "iterate doc.layers, sum
    // children.size()" loops that all hardcoded `nodes=0`. Computes
    // counts via curvz::utils::doc_object_count / doc_anchor_count and
    // pushes them to m_statusbar.set_counts. Safe to call when there is
    // no active document (sets 0 / 0).
    void refresh_status_counts();  // category: helper: display sync
    void toggle_rulers(bool visible);  // category: helper

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
    void apply_motif_to_window();  // category: helper: window state

    // S117 m15: apply (or remove) the `curvz-light` CSS class on a single
    // top-level window based on the current project's motif. Used by
    // apply_motif_to_window to walk every existing app window, and by the
    // application's `signal_window_added` hook so newly-opened dialogs
    // immediately wear the right class. Independent dialogs (any
    // Gtk::Window we create — Themes, Export, Macro Manager, etc.) are
    // separate top-level windows from MainWindow, so the class doesn't
    // propagate down naturally; we propagate it explicitly here.
    void sync_motif_class_to(Gtk::Window& w);  // category: helper: window state

    // Shared rename handler used by both the inspector "File" entry and the
    // documents-gallery double-click / context-menu rename. Sanitises the
    // name (spaces → dashes, strips path separators), enforces uniqueness
    // within the project, renames on disk if the project is saved, and
    // updates UI. Caller passes the document explicitly so this works for
    // any doc, not just the active one.
    void rename_doc(CurvzDocument *doc, std::string new_name);  // category: glue

    void on_new();  // category: handler: documents
    void on_new_project();  // category: handler: documents
    void on_close_project();  // category: handler: documents
    void on_open();  // category: handler: documents
    void update_project_sensitive();  // category: helper: action predicate
    void check_unsaved_then(std::function<void()> then);  // category: helper: flow orchestrator
    void on_save();  // category: handler: documents
    void on_save_as();  // category: handler: documents
    void on_save_as_template();  // category: handler: library
    void on_manage_templates();  // category: handler: library
    // s147 m3: on_show_themes removed — ThemesPanel in Content is the
    // canonical surface, no menu entry / dialog version remains.
    void on_export();  // category: handler: documents — File ▸ Export… (unified)

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
    std::vector<theme::Theme> ndd_available_themes() const;  // category: helper: NDD support
    void ndd_apply_chosen_theme(CurvzDocument& seed,
                                const std::optional<theme::ThemeId>& id) const;  // category: helper: NDD support

    void on_import_svg();  // category: handler: documents
    void on_import_svg_as_icon();  // category: handler: documents
    // Shared impl.
    //   force_currentcolor=true → convert Solid fill/stroke to CurrentColor
    //                             (icon workflow).
    //   normalize_to_1000=true  → rescale coords so long axis = 1000 units
    //                             (icon workflow). When false, preserve the
    //                             SVG's authored geometry verbatim.
    void import_svg_impl(const std::string& path,
                         bool force_currentcolor,
                         bool normalize_to_1000);  // category: helper: import shared
    void on_place_image();  // category: handler: documents
    // s125 m1d (was m1c on_place_image_as_doc, renamed): routes to
    // import_image_to_canvas with fit_canvas_to_image=true. Wired to
    // win.open-image and the File → Open Image menu entry. Sibling of
    // on_open in user model — both create a new doc.
    void on_open_image();  // category: handler: documents
    void on_save_selection_to_library(const std::string& dest_dir);  // category: handler: library
    // s125 m1a: opens a folder picker (mirrors LibraryPanel::on_add_clicked)
    // and routes the chosen directory to on_save_selection_to_library.
    // Wired to Canvas::signal_request_save_to_library, fired from the
    // canvas right-click "Save to Library…" entry.
    void on_request_save_selection_to_library();  // category: handler: library
    // s136 m4: actual library file write. Pure helper — takes a destination
    // directory and a base name (without extension), writes
    // `<dest_dir>/<base_name>.svg`. Returns true on success. The orchestrator
    // (on_save_selection_to_library) handles the name prompt and any
    // collision resolution; this helper assumes base_name is final.
    bool write_library_item(const std::string& dest_dir,
                            const std::string& base_name);  // category: helper: library
    void on_step_repeat();  // category: handler: effects
    // s154 m2: toolbar SnR left-click path — applies the popover's
    // persisted last values without showing UI. on_step_repeat() is
    // the menu-item / right-click path; this one is the left-click.
    void apply_step_repeat_with_last();  // category: handler: effects
    // Blend orchestrator — validates selection, reads A/B node counts
    // and stroke widths, shows BlendPopover (s154 m3), forwards Result
    // to Canvas::make_blend on OK. If selection is invalid on action-fire
    // (shouldn't happen thanks to the sensitivity hook, but belt-and-
    // braces) shows a user-visible message and returns.
    void on_blend();  // category: handler: effects
    // s154 m3: toolbar Blend left-click path. Applies popover-last-values
    // when node counts already match; falls back to opening the popover
    // when validation requires the warning banner / Equalize button.
    void apply_blend_with_last();  // category: handler: effects
    void on_warp_make();  // category: handler: effects
    void on_warp_edit();  // category: handler: effects
    void on_warp_release();  // category: handler: effects
    void on_warp_flatten();  // category: handler: effects
    // Guide construct (M3) — open the review dialog after the user clicks p2.
    void open_guide_review_dialog();  // category: handler: guides
    void close_guide_review_dialog();  // category: handler: guides
    void on_macro_manager();  // category: handler: macros
    void on_run_macro(const std::string& macro_id);  // category: handler: macros
    void on_quit();  // category: handler: documents
    void do_save_as(const std::string& dir);  // category: helper: save flow
    void on_tool_changed(ActiveTool tool);  // category: glue
    void on_doc_activated(int index);  // category: glue
    void cycle_doc(int delta);  // category: glue
    void on_undo();  // category: handler: edit
    void on_redo();  // category: handler: edit

    // Build a collapsible inspector section
    Gtk::Box* make_section(const char* title, Gtk::Widget& child,
                           bool expanded = true, bool vexpand_child = false,
                           std::shared_ptr<bool>* out_flag = nullptr);  // category: helper: inspector pump

    // Build a collapsible GROUP header (Document/Object/Content style).
    // Returns {outer, container}: append `outer` where you want the group
    // to live; append child sections into `container`.  `container` carries
    // the inspector-group-container CSS class for indentation.
    struct GroupSection { Gtk::Box* outer; Gtk::Box* container; };
    GroupSection make_group_section(const char* title, bool expanded,
                                    std::shared_ptr<bool>* out_flag = nullptr);  // category: helper: inspector pump

    // ── State ─────────────────────────────────────────────────────────────
    std::unique_ptr<CurvzProject> m_project;
    CommandHistory                m_history;
    NewDocumentDialog             m_new_doc_dialog;
    StepRepeatPopover             m_step_repeat_popover;
    BlendPopover                  m_blend_popover;
    WarpPopover                   m_warp_popover;
    MacroManagerWindow            m_macro_manager;
    MacroEditorWindow             m_macro_editor;
    PrintManager                  m_print_manager;
    OffsetPathDialog              m_offset_path_dialog;
    TranslateDialog               m_translate_dialog;  // s205 m4
    GradientDialog                m_gradient_dialog;
    SaveAsTemplateDialog          m_save_as_template_dialog;
    ManageTemplatesDialog         m_manage_templates_dialog;
    ThemeEditDialog               m_theme_edit_dialog;  // s200 m1
    StyleEditorDialog             m_style_editor_dialog;  // s201 m1
    ImageInfoDialog               m_image_info_dialog;  // s210 m1
    RotateFromPointDialog         m_rotate_from_point_dialog;  // s210 m2
    ShortcutsDialog               m_shortcuts_dialog;
    HelpWindow                    m_help_window;

    // Guide-construct review dialog (M3) — created lazily.  A tiny non-modal
    // window with Perpendicular checkbox + OK + Cancel.
    // s208 m5: substrate CheckButton. Forward-declared below; full include
    // in MainWindow_handlers.cpp at the construction site.
    std::unique_ptr<Gtk::Window>           m_guide_review_win;
    curvz::widgets::CheckButton*           m_guide_review_perp_chk = nullptr;

    // s202 m6 — quick-jump float, created lazily on first Ctrl+Space.
    // Held as unique_ptr<Gtk::Window> so the type doesn't bloat
    // MainWindow.hpp; the impl lives in MainWindow_helpers.cpp
    // alongside the focus_inspector_on cascade it triggers.
    std::unique_ptr<Gtk::Window>  m_quick_jump_win;

    // s203 m1 — View Clipboard mini float. Owned here so the action
    // handler can lazily build, refresh, and re-show. ClipboardViewWindow
    // is a real subclass (not a bare Gtk::Window) because it has its own
    // refresh() method and async-read state; the type is small enough
    // that including the header in MainWindow.hpp is fine.
    std::unique_ptr<ClipboardViewWindow>  m_clipboard_view_win;
    ActiveTool                    m_active_tool    = ActiveTool::Selection;
    ActiveTool                    m_inspector_tool = ActiveTool::Selection;
    // S58m: remember previous selection size so shift-click add/remove
    // (which keeps the primary pointer but changes the set) triggers an
    // inspector rebuild. Without this the Appearance panel stays showing
    // the old mixed/uniform state.
    size_t                        m_prev_selection_size = 0;
    bool                          m_closing       = false;
    bool                          m_pane_ready    = false;
    gint64                        m_pane_settled_at = 0; // microseconds, from g_get_monotonic_time()
    bool                          m_rulers_visible = true; // toggled by View → Rulers / Ctrl+R
    // s165 m3 — re-entrancy guard for on_undo / on_redo. Set true at entry,
    // cleared at exit. Suppresses re-entry when a Ctrl+Z keypress arrives
    // while a previous undo is still mid-flight (e.g. partway through
    // m_history.undo() or one of the post-undo refreshes). Without this,
    // rapid Ctrl+Z spam can re-enter on_undo on a partially-mutated tree
    // and crash. Belt-and-braces — also protects against signal handlers
    // queued during refresh that fire back into on_undo before we exit.
    bool                          m_undo_in_progress = false;
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
    Gtk::HeaderBar      m_headerbar;
    Gtk::MenuButton     m_hamburger;   // ☰ — opens the app menu popover
    Gtk::Button         m_logo_btn;    // App logo — opens About dialog

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
    Gtk::Box            m_root{Gtk::Orientation::VERTICAL};
    DocumentGallery     m_gallery;   // kept for thumbnail rendering only
    DocTabBar           m_doc_tabs;
    Gtk::Box            m_middle{Gtk::Orientation::HORIZONTAL};
    Toolbar             m_toolbar;
    Gtk::Box            m_canvas_col{Gtk::Orientation::VERTICAL};  // context bar + paned
    ContextBar          m_context_bar;
    Gtk::Paned          m_h_paned{Gtk::Orientation::HORIZONTAL};
    // Canvas area: corner + rulers in a Grid, all wrapped in an Overlay
    // so the floating text-tool entry can sit above the canvas.
    Gtk::Overlay        m_canvas_overlay;
    Gtk::Fixed          m_text_fixed;        // overlay layer for text entry widget
    Gtk::Grid           m_canvas_grid;

    // ── Corner Treatment panel (Popover on corner tool button) ────────────────
    Gtk::Popover        m_corner_panel;
    bool                m_corner_panel_visible = false;  // s194_m1: edge-guard tracker
    Gtk::Box            m_corner_panel_vbox{Gtk::Orientation::VERTICAL};
    Gtk::Box            m_corner_type_row{Gtk::Orientation::HORIZONTAL};
    Gtk::ToggleButton   m_corner_btn_round;
    Gtk::ToggleButton   m_corner_btn_chamfer;
    Gtk::ToggleButton   m_corner_btn_inverse;
    Gtk::Box            m_corner_radius_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Label          m_corner_radius_label;
    Gtk::SpinButton     m_corner_radius_spin;
    Glib::RefPtr<Gtk::Adjustment> m_corner_radius_adj;
    Gtk::Label          m_corner_unit_label;
    Gtk::Button         m_corner_apply_btn;

    void build_corner_panel();  // category: zone: overlays
    void show_corner_panel(bool show);  // category: zone: overlays
    void update_corner_panel_position();  // category: zone: overlays

    // System icon preview/copy
    // force_currentcolor=true → convert all Solid fills/strokes to CurrentColor
    // (default true because this is the icon-gallery copy flow).
    bool import_svg_as_doc(const std::string& path,
                           bool force_currentcolor = true);  // category: helper: import shared
    void on_preview_icon(const std::string& path);  // category: handler: documents
    void on_copy_icon(const std::string& path);  // category: handler: documents
    void exit_preview_mode();  // category: helper
    double pop_to_px(double v) const;  // category: helper

    bool                           m_preview_active     = false;
    int                            m_preview_saved_index = 0;
    std::unique_ptr<CurvzDocument> m_preview_doc;       // temp doc while previewing
    CornerSquare        m_corner;
    HRuler              m_hruler;
    VRuler              m_vruler;
    Canvas              m_canvas;
    Gtk::Box            m_right_panels{Gtk::Orientation::VERTICAL};
    Gtk::ScrolledWindow m_right_scroll;

    PropertiesPanel m_properties;
    PreviewPanel    m_preview;
    LayersPanel     m_layers;
    LibraryPanel    m_library;
    SwatchesPanel   m_swatches;
    StylesPanel     m_styles;
    ThemesPanel     m_themes;

#ifdef CURVZ_DIAGNOSTIC
    // s191 m3 — caption bar. Sits between m_middle and m_statusbar
    // in the root vertical box; revealed by set_subtitle(). See the
    // public method's comment for the wiring story.
    Gtk::Revealer       m_caption_revealer;
    Gtk::Label          m_caption_label;
#endif

    StatusBar       m_statusbar;

    // Inspector section open-state flags — set by make_section, used by load_project
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
    std::string config_path() const;  // category: helper: persistence
    void        save_config() const;  // category: helper: persistence
    std::string load_last_project_path();  // category: helper: persistence

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
    void update_clip_actions_sensitive();  // category: helper: action predicate

    // Blend — enabled iff exactly 2 Path nodes are selected (stricter
    // preconditions — same parent, equal node counts — enforced inside
    // Canvas::make_blend with user-visible error message on violation).
    // Release Blend — enabled iff primary selection is a Blend.
    Glib::RefPtr<Gio::SimpleAction> m_act_blend_make;
    Glib::RefPtr<Gio::SimpleAction> m_act_blend_release;
    void update_blend_action_sensitive();  // category: helper: action predicate

    // Warp — Make enabled iff exactly 1 Path/Compound/Group is selected.
    // Release / Flatten enabled iff primary selection is a Warp.
    // Deeper preconditions (if any) enforced inside Canvas::make_warp.
    Glib::RefPtr<Gio::SimpleAction> m_act_warp_make;
    Glib::RefPtr<Gio::SimpleAction> m_act_warp_edit;
    Glib::RefPtr<Gio::SimpleAction> m_act_warp_release;
    Glib::RefPtr<Gio::SimpleAction> m_act_warp_flatten;
    void update_warp_action_sensitive();  // category: helper: action predicate

    // Group / Ungroup (s138) — Make enabled iff >=2 nodes are selected;
    // Release enabled iff the primary selection is a Group. Wraps the
    // Canvas::group_selection / ungroup_selection methods that have
    // existed in the engine for some time but were never reachable from
    // the UI (no menu item, no action, no keybind). Surfaced when the
    // s138 m2 menu-accel fix made the Path submenu's gaps visible.
    Glib::RefPtr<Gio::SimpleAction> m_act_group_make;
    Glib::RefPtr<Gio::SimpleAction> m_act_group_release;
    void update_group_actions_sensitive();  // category: helper: action predicate

    // Boolean path ops (s122 m2) — Union/Subtract/Intersect enabled iff
    // exactly 2 Path or Compound nodes are selected. Deeper preconditions
    // (closed paths, common parent) are enforced inside Canvas::boolean_op
    // with user-visible error messages on violation. Hard-gating at 2
    // prevents triggering the not-yet-stable N>=3 iterative fold path.
    Glib::RefPtr<Gio::SimpleAction> m_act_bool_union;
    Glib::RefPtr<Gio::SimpleAction> m_act_bool_subtract;
    Glib::RefPtr<Gio::SimpleAction> m_act_bool_intersect;
    void update_bool_actions_sensitive();  // category: helper: action predicate

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
    void schedule_save();  // category: helper
    sigc::connection m_save_timer;  // pending debounce timer

    // s113 m2: gated outline-mode toggle. Refuses outline→preview when
    // current zoom would crash the app via drop-shadow buffer allocation;
    // shows an AlertDialog explaining how to proceed safely. Returns
    // true if the toggle happened (caller should sync action state +
    // statusbar), false if the toggle was refused.
    bool try_toggle_outline_safely();  // category: helper

    // Updates align button sensitivity; set in connect_signals, called from on_tool_changed
    std::function<void()> m_update_align_btn;
};

} // namespace Curvz
