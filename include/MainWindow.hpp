#pragma once
#include "Canvas.hpp"
#include "Toolbar.hpp"
#include "StatusBar.hpp"
#include "DocumentGallery.hpp"
#include <functional>
#include <map>      // s125 m1e: m_last_folders
#include <optional>
#include <vector>
#include "PropertiesPanel.hpp"
#include "PreviewPanel.hpp"
#include "LayersPanel.hpp"
#include "LibraryPanel.hpp"
#include "SwatchesPanel.hpp"
#include "StylesPanel.hpp"
#include "ContextBar.hpp"
#include "DocTabBar.hpp"
#include "CurvzProject.hpp"
#include "CommandHistory.hpp"
#include "NewDocumentDialog.hpp"
#include "ExportDialog.hpp"
#include "StepRepeatDialog.hpp"
#include "BlendDialog.hpp"
#include "WarpDialog.hpp"
#include "MacroManagerWindow.hpp"
#include "MacroEditorWindow.hpp"
#include "PrintManager.hpp"
#include "OffsetPathDialog.hpp"
#include "GradientDialog.hpp"
#include "SaveAsTemplateDialog.hpp"
#include "ShortcutsDialog.hpp"
#include "HelpWindow.hpp"
#include "ManageTemplatesDialog.hpp"
#include "Ruler.hpp"
#include <gtkmm/applicationwindow.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/box.h>
#include <gtkmm/grid.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/paned.h>
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

namespace Curvz {

class Application;

class MainWindow : public Gtk::ApplicationWindow {
public:
    explicit MainWindow(Application& app);

    // s126: last-used folder accessors. Public so non-MainWindow dialogs
    // (ExportDocsDialog and friends) can opt into the same per-purpose
    // memory the built-in pickers use. Storage lives in m_last_folders
    // and persists via save_config.
    std::string get_last_folder(const std::string& purpose) const;
    void        set_last_folder(const std::string& purpose,
                                const std::string& path);

private:
    void setup_headerbar();
    void setup_layout();
    void setup_project();
    void setup_menu();              // build Gio::Menu + actions + hamburger button
    void connect_signals();

    void load_project(std::unique_ptr<CurvzProject> project);
    void update_all_panels();
    void update_title();
    void update_rulers();
    void refresh_inspector();
    // s132 m2: single funnel for the StatusBar's "N objects · N nodes"
    // readout. Replaces five duplicated "iterate doc.layers, sum
    // children.size()" loops that all hardcoded `nodes=0`. Computes
    // counts via curvz::utils::doc_object_count / doc_anchor_count and
    // pushes them to m_statusbar.set_counts. Safe to call when there is
    // no active document (sets 0 / 0).
    void refresh_status_counts();
    void toggle_rulers(bool visible);  // show/hide ruler chrome

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
    void apply_motif_to_window();

    // S117 m15: apply (or remove) the `curvz-light` CSS class on a single
    // top-level window based on the current project's motif. Used by
    // apply_motif_to_window to walk every existing app window, and by the
    // application's `signal_window_added` hook so newly-opened dialogs
    // immediately wear the right class. Independent dialogs (any
    // Gtk::Window we create — Themes, Export, Macro Manager, etc.) are
    // separate top-level windows from MainWindow, so the class doesn't
    // propagate down naturally; we propagate it explicitly here.
    void sync_motif_class_to(Gtk::Window& w);

    // Shared rename handler used by both the inspector "File" entry and the
    // documents-gallery double-click / context-menu rename. Sanitises the
    // name (spaces → dashes, strips path separators), enforces uniqueness
    // within the project, renames on disk if the project is saved, and
    // updates UI. Caller passes the document explicitly so this works for
    // any doc, not just the active one.
    void rename_doc(CurvzDocument *doc, std::string new_name);

    void on_new();
    void on_new_project();
    void on_close_project();
    void on_open();
    void update_project_sensitive();
    void check_unsaved_then(std::function<void()> then);
    void on_save();
    void on_save_as();
    void on_save_as_template();
    void on_manage_templates();
    void on_show_themes();             // S103 m3 — Project → Themes…
    void on_export_docs();             // S104 m1 — Project → Export Documents…

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
    std::vector<theme::Theme> ndd_available_themes() const;
    void ndd_apply_chosen_theme(CurvzDocument& seed,
                                const std::optional<theme::ThemeId>& id) const;

    void on_import_svg();
    void on_import_svg_as_icon();
    // Shared impl.
    //   force_currentcolor=true → convert Solid fill/stroke to CurrentColor
    //                             (icon workflow).
    //   normalize_to_1000=true  → rescale coords so long axis = 1000 units
    //                             (icon workflow). When false, preserve the
    //                             SVG's authored geometry verbatim.
    void import_svg_impl(const std::string& path,
                         bool force_currentcolor,
                         bool normalize_to_1000);
    void on_place_image();
    // s125 m1d (was m1c on_place_image_as_doc, renamed): routes to
    // import_image_to_canvas with fit_canvas_to_image=true. Wired to
    // win.open-image and the File → Open Image menu entry. Sibling of
    // on_open in user model — both create a new doc.
    void on_open_image();
    void on_save_selection_to_library(const std::string& dest_dir);
    // s125 m1a: opens a folder picker (mirrors LibraryPanel::on_add_clicked)
    // and routes the chosen directory to on_save_selection_to_library.
    // Wired to Canvas::signal_request_save_to_library, fired from the
    // canvas right-click "Save to Library…" entry.
    void on_request_save_selection_to_library();
    // s136 m4: actual library file write. Pure helper — takes a destination
    // directory and a base name (without extension), writes
    // `<dest_dir>/<base_name>.svg`. Returns true on success. The orchestrator
    // (on_save_selection_to_library) handles the name prompt and any
    // collision resolution; this helper assumes base_name is final.
    bool write_library_item(const std::string& dest_dir,
                            const std::string& base_name);
    void on_step_repeat();
    // Blend orchestrator (M3) — validates selection, reads A/B node counts
    // and stroke widths, shows BlendDialog, forwards dialog Result to
    // Canvas::make_blend on OK. If selection is invalid on action-fire
    // (shouldn't happen thanks to the sensitivity hook, but belt-and-
    // braces) shows a user-visible message and returns.
    void on_blend();
    void on_warp_make();
    void on_warp_edit();
    void on_warp_release();
    void on_warp_flatten();
    // Guide construct (M3) — open the review dialog after the user clicks p2.
    void open_guide_review_dialog();
    void close_guide_review_dialog();
    void on_macro_manager();
    void on_run_macro(const std::string& macro_id);
    void on_export_theme();
    void on_quit();
    void do_save_as(const std::string& dir);
    void on_tool_changed(ActiveTool tool);
    void on_doc_activated(int index);
    void cycle_doc(int delta);   // s108 m7: doc-next / doc-prev seam
    void on_undo();
    void on_redo();

    // Build a collapsible inspector section
    Gtk::Box* make_section(const char* title, Gtk::Widget& child,
                           bool expanded = true, bool vexpand_child = false,
                           std::shared_ptr<bool>* out_flag = nullptr);

    // Build a collapsible GROUP header (Document/Object/Content style).
    // Returns {outer, container}: append `outer` where you want the group
    // to live; append child sections into `container`.  `container` carries
    // the inspector-group-container CSS class for indentation.
    struct GroupSection { Gtk::Box* outer; Gtk::Box* container; };
    GroupSection make_group_section(const char* title, bool expanded,
                                    std::shared_ptr<bool>* out_flag = nullptr);

    // ── State ─────────────────────────────────────────────────────────────
    std::unique_ptr<CurvzProject> m_project;
    CommandHistory                m_history;
    NewDocumentDialog             m_new_doc_dialog;
    ExportDialog                  m_export_dialog;
    StepRepeatDialog              m_step_repeat_dialog;
    BlendDialog                   m_blend_dialog;
    WarpDialog                    m_warp_dialog;
    MacroManagerWindow            m_macro_manager;
    MacroEditorWindow             m_macro_editor;
    PrintManager                  m_print_manager;
    OffsetPathDialog              m_offset_path_dialog;
    GradientDialog                m_gradient_dialog;
    SaveAsTemplateDialog          m_save_as_template_dialog;
    ManageTemplatesDialog         m_manage_templates_dialog;
    ShortcutsDialog               m_shortcuts_dialog;
    HelpWindow                    m_help_window;

    // Guide-construct review dialog (M3) — created lazily.  A tiny non-modal
    // window with Perpendicular checkbox + OK + Cancel.
    std::unique_ptr<Gtk::Window>  m_guide_review_win;
    Gtk::CheckButton*             m_guide_review_perp_chk = nullptr;
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

    // ── Widgets ───────────────────────────────────────────────────────────
    Gtk::HeaderBar      m_headerbar;
    Gtk::MenuButton     m_hamburger;   // ☰ — opens the app menu popover
    Gtk::Button         m_logo_btn;    // App logo — opens About dialog
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

    void build_corner_panel();
    void show_corner_panel(bool show);
    void update_corner_panel_position();

    // System icon preview/copy
    // force_currentcolor=true → convert all Solid fills/strokes to CurrentColor
    // (default true because this is the icon-gallery copy flow).
    bool import_svg_as_doc(const std::string& path,
                           bool force_currentcolor = true);
    void on_preview_icon(const std::string& path);
    void on_copy_icon(const std::string& path);
    void exit_preview_mode();
    double pop_to_px(double v) const;   // convert display-unit value to doc units

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
    StatusBar       m_statusbar;

    // Inspector section open-state flags — set by make_section, used by load_project
    std::shared_ptr<bool> m_sec_open_preview;
    std::shared_ptr<bool> m_sec_open_layers;
    std::shared_ptr<bool> m_sec_open_library;
    std::shared_ptr<bool> m_sec_open_documents;
    std::shared_ptr<bool> m_sec_open_swatches;
    std::shared_ptr<bool> m_sec_open_styles;
    std::shared_ptr<bool> m_sec_open_content;

    // App-level config (last opened project path)
    std::string config_path() const;
    void        save_config() const;
    std::string load_last_project_path();

    // s125 m1e: per-purpose "last folder used" memory for file pickers.
    // Keyed by a stable purpose string (typically the action name —
    // "open-image", "place-image", "save-as", etc.). Persisted in
    // settings.json alongside the rest of app-level config; flushed on
    // project load, save-as, and quit (matches existing save_config call
    // sites). Folder, not file: pickers re-open at the same directory,
    // not pre-select the same file.
    //
    // Accessors are public (above) — see s126 ExportDocsDialog wiring.
    std::map<std::string, std::string> m_last_folders;

    // Clip / Release Clip — held as members so we can toggle enabled
    // state on selection changes. Clip enabled iff selection non-empty;
    // Release Clip enabled iff primary selection is a ClipGroup.
    Glib::RefPtr<Gio::SimpleAction> m_act_clip_make;
    Glib::RefPtr<Gio::SimpleAction> m_act_clip_release;
    void update_clip_actions_sensitive();

    // Blend — enabled iff exactly 2 Path nodes are selected (stricter
    // preconditions — same parent, equal node counts — enforced inside
    // Canvas::make_blend with user-visible error message on violation).
    // Release Blend — enabled iff primary selection is a Blend.
    Glib::RefPtr<Gio::SimpleAction> m_act_blend_make;
    Glib::RefPtr<Gio::SimpleAction> m_act_blend_release;
    void update_blend_action_sensitive();

    // Warp — Make enabled iff exactly 1 Path/Compound/Group is selected.
    // Release / Flatten enabled iff primary selection is a Warp.
    // Deeper preconditions (if any) enforced inside Canvas::make_warp.
    Glib::RefPtr<Gio::SimpleAction> m_act_warp_make;
    Glib::RefPtr<Gio::SimpleAction> m_act_warp_edit;
    Glib::RefPtr<Gio::SimpleAction> m_act_warp_release;
    Glib::RefPtr<Gio::SimpleAction> m_act_warp_flatten;
    void update_warp_action_sensitive();

    // Group / Ungroup (s138) — Make enabled iff >=2 nodes are selected;
    // Release enabled iff the primary selection is a Group. Wraps the
    // Canvas::group_selection / ungroup_selection methods that have
    // existed in the engine for some time but were never reachable from
    // the UI (no menu item, no action, no keybind). Surfaced when the
    // s138 m2 menu-accel fix made the Path submenu's gaps visible.
    Glib::RefPtr<Gio::SimpleAction> m_act_group_make;
    Glib::RefPtr<Gio::SimpleAction> m_act_group_release;
    void update_group_actions_sensitive();

    // Boolean path ops (s122 m2) — Union/Subtract/Intersect enabled iff
    // exactly 2 Path or Compound nodes are selected. Deeper preconditions
    // (closed paths, common parent) are enforced inside Canvas::boolean_op
    // with user-visible error messages on violation. Hard-gating at 2
    // prevents triggering the not-yet-stable N>=3 iterative fold path.
    Glib::RefPtr<Gio::SimpleAction> m_act_bool_union;
    Glib::RefPtr<Gio::SimpleAction> m_act_bool_subtract;
    Glib::RefPtr<Gio::SimpleAction> m_act_bool_intersect;
    void update_bool_actions_sensitive();

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
    void schedule_save();
    sigc::connection m_save_timer;  // pending debounce timer

    // s113 m2: gated outline-mode toggle. Refuses outline→preview when
    // current zoom would crash the app via drop-shadow buffer allocation;
    // shows an AlertDialog explaining how to proceed safely. Returns
    // true if the toggle happened (caller should sync action state +
    // statusbar), false if the toggle was refused.
    bool try_toggle_outline_safely();

    // Updates align button sensitivity; set in connect_signals, called from on_tool_changed
    std::function<void()> m_update_align_btn;
};

} // namespace Curvz
