#pragma once
//
// StylesPanel.hpp — Phase 2 (S81 m4c) skeleton.
//
// Sibling panel to SwatchesPanel. Lives in the right pane between Swatches
// and Documents. Displays the project's style library: app styles first,
// user styles second, grouped by category. m4c-1 is read-only: rebuilds
// on library mutations, no click handlers, no creation, no rename.
//
// m4c-1 scope (this milestone):
//   * Header row: "Styles" label + "+" button stub (no action yet).
//   * Body: vertical list of style names, grouped by category.
//   * Refresh-on-library-signal wiring (added / changed / removed).
//   * Selection-change hookup so a future "active binding" indicator
//     can sit on the row whose id matches the current selection's
//     bound_style. Hookup exists; the indicator itself is m4c-2.
//
// Out of scope for m4c-1, lands in m4c-2:
//   * Click-to-bind (pushes BindStyleCommand).
//   * Create-empty / create-from-selection.
//   * Rename via double-click.
//   * Right-click context menu.
//   * Click-binding-name-in-inspector-jumps-to-this-panel
//     (handoff Q3 — needs a public scroll_to(StyleId) entry point).
//
// Out of scope for m4c entirely, deferred to m5+:
//   * Thumbnail chips (defer per Scott — keep skeleton text-only).
//   * Drag-to-rebind.
//   * Active-binding visual indicator (the dot or ring on the matching
//     row when a selected node carries that binding). Hookups exist
//     in m4c-1; the visual lights up in m4c-2.
//
// Ownership / dependencies:
//   * Non-owning pointer to StyleLibrary (lives on CurvzProject).
//   * Non-owning pointer to Canvas (for selection-change hookup).
//   * No popover, no color picker — styles aren't directly created
//     from a colour the way swatches are. Creation flows route through
//     either "from selection" (which reads the live appearance) or
//     "empty" (which uses the StrokeAppearance defaults). Both wire
//     up in m4c-2.
//
// Refresh model:
//   refresh() tears down and rebuilds the body. Called on every library
//   mutation. Same lazy-rebuild pattern as SwatchesPanel — the list is
//   small (sub-100 entries Phase 1 expectation) so partial-diff tracking
//   isn't worth the complexity yet.
//

#include "style/StyleLibrary.hpp"
#include "CommandHistory.hpp"
#include "StyleEditorDialog.hpp"  // S85 m4i-cont-1: dialog Mode enum + Style

#include <giomm/simpleactiongroup.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/dropdown.h>      // S83 m4h v12: Gtk::DropDown for category chooser
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <sigc++/sigc++.h>
#include <functional>
#include <string>
#include <utility>               // std::pair
#include <vector>

namespace Curvz {

namespace color {
class SwatchLibrary;
}

class Canvas;  // fwd — selection-changed hookup only, no method calls in m4c-1

class StylesPanel : public Gtk::Box {
public:
    StylesPanel();

    // Non-owning. Caller guarantees the library outlives the panel or
    // calls set_library(nullptr) before destroying it. Hooks the
    // library's three mutation signals (added / changed / removed)
    // and triggers a refresh on each.
    void set_library(style::StyleLibrary* lib);

    // S85 cont-3: Non-owning. The panel uses this to (a) resolve
    // SwatchRef paint to the current swatch colour for chip rendering,
    // and (b) print real swatch names in the hover-tooltip summary.
    // Without this set, SwatchRef paints render with their cached
    // fallback colour and tooltip falls back to "(swatch)" tagging.
    // Hooks signal_swatch_changed so chips repaint live when a bound
    // swatch is edited externally (e.g. via SwatchesPanel right-click
    // → Edit Colour).
    void set_swatch_library(color::SwatchLibrary* lib);

    // Non-owning. Hooked for selection-change → refresh so the future
    // active-binding indicator (m4c-2) stays current. m4c-1 doesn't
    // visibly use the canvas, but the hookup is wired now to keep the
    // shape symmetric with SwatchesPanel and avoid a follow-on edit
    // when the indicator lights up.
    void set_canvas(Canvas* canvas);

    // S80 m4c-2: non-owning. Required for click-to-bind to push a
    // BindStyleCommand onto the undo stack. Without this set, click
    // still mutates state but no undo entry is recorded — silent
    // correctness regression. Wired from MainWindow at startup.
    void set_history(CommandHistory* history) { m_history = history; }

    // S87 — view state accessors for project-level persistence. The
    // category dropdown selection is per-project state; CurvzProject
    // saves it in editor_state. MainWindow reads/writes via these.
    //
    // set_active_category writes both fields and triggers a refresh()
    // so the dropdown re-syncs to the new state. Safe to call before
    // set_library() / refresh() — the values just sit until the panel
    // first builds. After build, calling it forces a rebuild.
    //
    // active_category() returns the current category id ("" maps to
    // (uncategorised) in user tier, or the empty-named "Built-in"
    // bucket in app tier — disambiguated by active_is_app_tier()).
    void set_active_category(const std::string& cat, bool is_app_tier);
    const std::string& active_category() const { return m_active_category; }
    bool active_is_app_tier() const { return m_active_is_app_tier; }

    // Emitted when the user changes the dropdown selection. Distinct
    // from signal_library_changed because the *library* hasn't mutated
    // — only the panel's view of it. MainWindow wires this to
    // schedule_save() so the choice persists across launches.
    using ViewStateChangedSignal = sigc::signal<void()>;
    ViewStateChangedSignal& signal_view_state_changed() {
        return m_sig_view_state_changed;
    }

    // Rebuild the panel body from scratch.
    void refresh();

    // Emitted whenever the library mutates in a way that needs persisting
    // (add / update / remove). MainWindow wires this to schedule_save().
    // Phase 1 m1 has no JSON round-trip on the library, but the signal
    // exists for symmetry with SwatchesPanel — m3b's persistence work
    // means saves DO matter once user styles can be created (m4c-2).
    using LibraryChangedSignal = sigc::signal<void()>;
    LibraryChangedSignal& signal_library_changed() { return m_sig_library_changed; }

    // S80 m4c-2: Emitted after a click-to-bind so MainWindow can refresh
    // the inspector's "Bound: <n>" indicator without StylesPanel reaching
    // into MainWindow internals. Same idiom as
    // PropertiesPanel::signal_request_canvas_focus — panels emit
    // intent, MainWindow routes to the right method.
    using RequestInspectorRefreshSignal = sigc::signal<void()>;
    RequestInspectorRefreshSignal& signal_request_inspector_refresh() {
        return m_sig_request_inspector_refresh;
    }

    // S85 m4i-cont-1: Emitted when the panel wants to open the Style
    // Editor dialog. MainWindow listens, constructs the dialog with
    // itself as transient parent, and wires the OK callback to invoke
    // the supplied `on_committed` closure. The closure carries the
    // panel-side command-push logic (Add vs Update, library, history),
    // so MainWindow doesn't need to reach into StylesPanel internals
    // beyond presenting the dialog.
    //
    // initial: seed Style for the dialog. id stays empty for New/
    // Duplicate; for Edit it carries the existing id which the dialog
    // preserves through OK.
    using RequestStyleEditorSignal =
        sigc::signal<void(StyleEditorDialog::Mode mode,
                          style::Style initial,
                          std::function<void(style::Style)> on_committed)>;
    RequestStyleEditorSignal& signal_request_style_editor() {
        return m_sig_request_style_editor;
    }

private:
    style::StyleLibrary* m_library = nullptr;
    color::SwatchLibrary* m_swatch_library = nullptr;  // S85 cont-3
    Canvas*              m_canvas  = nullptr;
    CommandHistory*      m_history = nullptr;  // S80 m4c-2: undo for bind

    // S85 cont-3: signal connection for the swatch library's
    // signal_swatch_changed. Disconnected on set_swatch_library(nullptr)
    // and before the panel is destroyed. When a bound swatch is edited
    // externally (SwatchesPanel right-click → Edit Colour), this fires
    // refresh() so chip preview and tooltip stay current.
    sigc::connection m_swatch_lib_changed_conn;

    // ── Header (S83 m4h v13) ──────────────────────────────────────────────
    //
    // Single-row header:
    //   [ category dropdown ▾ ............................ [⋮]  ]
    //
    // Pre-v13 the kebab had its own row with a "Styles" title label,
    // and the dropdown lived in m_body. v13 drops the inner title
    // (the make_section wrapper above already labels the panel) and
    // moves the dropdown up to share the row with the kebab. Mirrors
    // SwatchesPanel which has used an empty-ish inner title since
    // construction (the section wrapper does the labeling).
    //
    // m_title is retained as a member (not removed) to minimise diff
    // and preserve the option to re-introduce a label without
    // re-plumbing the header. It's not appended in v13.
    Gtk::Box        m_header { Gtk::Orientation::HORIZONTAL };
    Gtk::Label      m_title;
    Gtk::MenuButton m_btn_add;

    // ── Body container ────────────────────────────────────────────────────
    //
    // Torn down and rebuilt by refresh(). Layout when populated (v13+):
    //   [ Style rows for the active category only ]
    //
    // The category dropdown chooser lives in m_header above (v13).
    // Pre-v13 the dropdown was the first body child; v13 promoted
    // it to share the header row with the kebab.
    Gtk::Box m_body { Gtk::Orientation::VERTICAL };

    // ── Right-click context-menu plumbing (S81 m4c-3) ─────────────────────
    //
    // Per the SwatchesPanel S72 dispatch fix: bare PopoverMenu::set_parent
    // + popup() doesn't wire the action muxer (item clicks don't dispatch).
    // Solution: route the popover through a hidden, zero-sized MenuButton
    // that lives permanently in the panel's widget tree. MenuButton's own
    // size_allocate calls gtk_popover_present, which finishes the wiring.
    //
    // We rebuild a fresh PopoverMenu per right-click and call set_popover
    // on the helper, which dissociates the previous popover (no manual
    // lifetime management needed — make_managed instances are owned by
    // their parent chain).
    Gtk::MenuButton m_ctx_button;

    // The id of the row most recently right-clicked. Read by the action
    // handlers (rename / duplicate / delete) to decide which row to
    // operate on. Captured at right-click time, consumed by the next
    // action invocation.
    style::StyleId m_ctx_style_id;

    // S83 m4h+: The category most recently right-clicked on a
    // category header. Read by action_rename_category /
    // action_delete_category to decide which category to operate
    // on. Captured at right-click time, consumed by the next
    // action invocation. Same pattern as m_ctx_style_id.
    //
    // S83 m4h v12: per-header right-click is gone (categories now
    // collapse into a dropdown chooser, no header to right-click).
    // The two action handlers now read m_active_category /
    // m_active_is_app_tier instead. This member is retained for
    // potential future use but is no longer set by anything.
    std::string m_ctx_category;

    // ── Category chooser (S83 m4h v12) ────────────────────────────────────
    //
    // Replaces the old all-categories-visible rendering. One category
    // shows at a time, selected via the dropdown. Mirrors the
    // SwatchesPanel palette dropdown idiom exactly.
    //
    // m_active_category — string ID of the selected category. "" is
    //   the "(uncategorised)" bucket. Initialised to "" (uncat) at
    //   ctor time; updated on dropdown selection-changed and on
    //   create flows that birth a new category. Persists across
    //   refreshes.
    //
    // m_active_is_app_tier — true if the selected category is an
    //   app-tier category (e.g. "Built-in"). Disambiguates from a
    //   hypothetical user category with the same name. Used by the
    //   action handlers to gate read-only operations.
    //
    // m_category_dropdown — non-owning pointer to the live DropDown
    //   widget. Nulled at the start of each refresh because the
    //   dropdown is rebuilt every refresh (children are managed,
    //   the old pointer becomes dangling). Re-assigned during
    //   refresh.
    //
    // m_category_order — parallel to dropdown items: pairs of
    //   (category, is_app_tier) in dropdown index order. Read by
    //   the selection-changed handler to translate index → state.
    //
    // m_syncing_dropdown — re-entrant guard. set_selected() fires
    //   the selection-changed signal even for programmatic calls;
    //   without this guard the handler would fire during refresh
    //   and re-enter refresh, causing churn.
    std::string                       m_active_category;
    bool                              m_active_is_app_tier = false;
    Gtk::DropDown*                    m_category_dropdown  = nullptr;
    std::vector<std::pair<std::string,bool>> m_category_order;
    bool                              m_syncing_dropdown   = false;
    sigc::connection                  m_dropdown_sel_conn;

    // Action group hosting the right-click menu actions and the "+"
    // create-flow actions. Inserted on the panel widget under the
    // "styles" prefix. Action names match the menu's "styles.<verb>"
    // strings used in the popover menu definitions.
    Glib::RefPtr<Gio::SimpleActionGroup> m_actions;

    // ── Inline-rename state (S81 m4c-3) ───────────────────────────────────
    //
    // S85 cont-5: m_rename_in_flight_id member retired alongside the
    // inline-rename functions. The rename target id flows through the
    // prompt_text callback closure now.

    // ── Signal connections ────────────────────────────────────────────────
    sigc::connection m_lib_added_conn;
    sigc::connection m_lib_changed_conn;
    sigc::connection m_lib_removed_conn;
    sigc::connection m_canvas_selection_conn;

    LibraryChangedSignal m_sig_library_changed;
    RequestInspectorRefreshSignal m_sig_request_inspector_refresh;
    RequestStyleEditorSignal m_sig_request_style_editor;  // S85 m4i-cont-1
    ViewStateChangedSignal m_sig_view_state_changed;       // S87

    // ── Build helpers ─────────────────────────────────────────────────────
    //
    // Each "category section" is a Box: a small dim-label header followed
    // by one row per style in that category. "Tier separator" is a thin
    // horizontal rule (Gtk::Separator) between app and user blocks. Both
    // are added directly to m_body.
    // S83 m4h v12: replaced build_tier / make_category_header /
    // add_tier_separator. The new structure:
    //
    //   build_chooser()      — populates m_category_order, builds the
    //                          DropDown, restores active selection,
    //                          appends to m_body.
    //   build_active_body()  — appends style rows for the currently
    //                          active category.
    //
    // The previous all-categories-visible rendering (build_tier +
    // make_category_header + add_tier_separator) is gone with v12;
    // category management moved to the kebab menu.
    void build_chooser();
    void build_active_body();
    // S85 cont-5: chip-grid rewrite. Returns a 24×24 DrawingArea
    // representing the style's appearance — fill in the centre, stroke
    // as a constant-thickness ring, active-paint ring overlay when
    // is_bound is true. Hover tooltip carries the full data summary
    // (name + tier + fill / stroke / dash). Primary double-click binds
    // the current selection; secondary click opens the context menu.
    // Pre-cont-5 this returned a Gtk::Box row with [chip][name][dot];
    // the rewrite folds the binding-indicator dot into the chip ring
    // and drops the name label entirely (all info is on hover).
    Gtk::Widget* make_style_chip(const style::Style& style, bool is_bound);

    // S83 m4h v12: build the kebab menu fresh on each refresh.
    // Items vary based on whether the active category is read-only
    // (app-tier) or user-editable. Built per-refresh because palette-
    // /category-state changes between refreshes — simpler than tracking
    // and toggling action-enabled states.
    void rebuild_kebab_menu();

    // ── Style interchange (S102 m1) ───────────────────────────────────────
    //
    // Three new entry points on the kebab, mirroring the S101 palette
    // interchange surface on SwatchesPanel:
    //
    //   on_load_user(stem)  — load a previously-exported pack from
    //                         ~/.config/curvz/styles/<stem>.json. The
    //                         kebab populates this submenu dynamically
    //                         from style::style_io::enumerate_user(),
    //                         so user packs appear automatically once
    //                         exported.
    //   on_import_styles()  — open a file dialog, parse the chosen
    //                         .json, append the styles into the user
    //                         tier with name disambiguation. Atomic
    //                         undo via CompositeCommand wrapped around
    //                         one AddStyleCommand per imported style.
    //   on_export_styles()  — serialise the entire user tier to a
    //                         .json file via a save dialog. Defaults to
    //                         the user-styles directory so the kebab
    //                         submenu picks up the export immediately.
    //
    // None of these touch app-tier styles (app styles are code, never
    // user data). All three route through style::style_io for the
    // format ↔ library translation; this class only handles file
    // dialog plumbing and emitting library_changed afterwards.
    void on_load_user(const std::string& stem);
    void on_import_styles();
    void on_export_styles();

    // S80 m4c-2: bind the current canvas selection to the given style id.
    // Walks Path/Compound selection, builds per-target snapshots, mutates
    // bound_style + materialises, pushes one atomic BindStyleCommand,
    // queues canvas redraw, refreshes panel + emits inspector-refresh.
    // No-op when selection is empty, no library, or no history.
    void bind_selection_to(const style::StyleId& id);

    // ── m4c-3 CRUD action handlers ────────────────────────────────────────
    //
    // Each helper is invoked from a Gio::SimpleAction installed in
    // m_actions. The actions wire to popover menu items (the "+" flow
    // and the per-row right-click menu). All four push a corresponding
    // command (Add / Update / Remove) and let the library signals drive
    // the panel refresh.

    // S85 m4i-cont-1: "+" → "New style" now opens the Style Editor
    // dialog instead of silently creating a default-appearance style.
    // The dialog seeds with sensible defaults (auto-name, current
    // active category, default StrokeAppearance); OK pushes
    // AddStyleCommand via the request-signal callback.
    void action_create_empty();

    // S85 m4i-cont-1: Right-click on a user-tier row → "Edit…".
    // Opens the Style Editor seeded with the row's existing style.
    // OK pushes UpdateStyleCommand via the request-signal callback.
    // m_ctx_style_id drives target.
    void action_edit();

    // S85 m4i-cont-1: Right-click on an app-tier row → "Edit a copy…".
    // Opens the dialog seeded with the app style's appearance and a
    // " copy" suffix on the name. OK pushes AddStyleCommand under a
    // fresh stl_<uuid> id (the bare-fact-of-duplication command path
    // is action_duplicate; this is the duplicate-and-edit-in-one-step
    // variant for users who know they want to customise).
    void action_edit_copy();

    // "Create from selection" — reads the primary selection's appearance
    // (Path or Compound) and builds a Style around it. Auto-name uses
    // the object's name if present ("Style from <name>"), otherwise the
    // "Style N" lowest-free pattern. No-op when selection is empty or
    // has no eligible target.
    void action_create_from_selection();

    // S83 m4h v9: "New category…" — prompts for a category name, then
    // creates a default empty style under that category. Categories
    // aren't a first-class persisted entity; an empty one would
    // disappear on next reload. Birthing a category by birthing a
    // member is the simpler semantic: the user types a name, gets
    // an empty style placed there, can rename / edit / move it later
    // like any other style. Pushes AddStyleCommand for undo.
    void action_create_category();

    // Right-click → Rename. Switches the matching row's name Label to
    // an Entry (not a separate dialog — inline edit per the handoff).
    // For app-tier rows the action is hidden by the menu builder so this
    // never fires on read-only entries. m_ctx_style_id drives target.
    void action_rename();

    // Right-click → Duplicate. user-tier: duplicate_to_user(src) under
    // a fresh stl_<uuid>. app-tier: same call (the "Duplicate to user"
    // verb on app rows). Both push AddStyleCommand with the duplicated
    // Style value + the new id.
    void action_duplicate();

    // Right-click → Delete. user-tier only (menu hidden on app rows).
    // Pushes RemoveStyleCommand. m4c-3 deletes silently; usage-check
    // dialog is m4c-4 / m5.
    void action_delete();

    // S83 m4h+: Right-click → Category…. user-tier only (menu hidden
    // on app rows; built-in styles share the read-only "Built-in"
    // category set at seed time). Pops a small text prompt prefilled
    // with the current category. Empty input clears the category
    // (the style returns to "(uncategorised)" in the panel grouping).
    // Pushes UpdateStyleCommand for undo, mirroring the rename path
    // — both are header-only mutations and use the same command.
    void action_set_category();

    // S83 m4h+: Right-click on a category header → Rename. Renames
    // the category for every user-tier style currently in it. Atomic
    // via CompositeCommand wrapping one UpdateStyleCommand per
    // member. App-tier "Built-in" header is NOT renameable (read-
    // only; menu omits the entry on app-tier headers). Empty new
    // name = no-op (treated as cancel — empty would clobber every
    // style in the category to uncategorised, which is the explicit
    // job of "Set category…" on individual rows).
    void action_rename_category();

    // S83 m4h+: Right-click on a category header → Delete. Deletes
    // every user-tier style in the category. Atomic via Composite-
    // Command wrapping one RemoveStyleCommand per member — Ctrl+Z
    // restores the entire category in one step. Per Scott's call:
    // no warning dialog (undo handles the "oops" case). App-tier
    // "Built-in" header is NOT deletable.
    void action_delete_category();

    // Auto-name helper for create flows. Returns the lowest-free
    // "Style N" name not currently used by any user-tier style.
    std::string next_style_name() const;

    // S85 cont-5: inline-rename infrastructure (begin_inline_rename /
    // commit_inline_rename / cancel_inline_rename plus the
    // m_rename_in_flight_id member) is gone. The chip-grid rewrite
    // has no name label to host an inline Entry; rename now goes
    // through prompt_text directly inside action_rename, mirroring
    // SwatchesPanel's rename flow.

    // ── Hover tooltip helper (S85 m4i-cont-4) ─────────────────────────────
    //
    // Compose the multi-line summary string applied as the row's
    // hover tooltip — fill / stroke / width / dash etc. Pure helper;
    // no widget access. Pre-cont-4 this fed a single-click popover;
    // popover interfered with double-click-to-bind on GTK4 so the
    // surface moved to a tooltip. The helper signature is unchanged.
    std::string build_hint_text(const style::Style& s) const;

    // ── Style chip render (S85 cont-3) ────────────────────────────────────
    //
    // Single-chip preview of a style's appearance. Layout:
    //   * Outer ring of CONSTANT thickness (~3px on a 24px chip) renders
    //     the stroke colour. Independent of s.stroke.width — chip is a
    //     colour signal, not a thickness preview. Stroke = None → no
    //     ring (chip is full inner).
    //   * Inner area renders the fill colour.
    //   * For SwatchRef paints, resolve through m_swatch_library to the
    //     swatch's CURRENT colour (so chip stays in sync with swatch
    //     edits).
    //   * None → diagonal stripes in the relevant area.
    //   * CurrentColor → small "C" glyph in the relevant area.
    //   * is_bound=true → active-paint ring (white-inner / black-outer)
    //     overlaid on the chip, mirroring SwatchesPanel's
    //     "active swatch matches selection" idiom.
    //
    // Pure draw helper; doesn't access widget state beyond the
    // m_swatch_library member it needs for SwatchRef resolution.
    void draw_style_chip(const Cairo::RefPtr<Cairo::Context>& cr,
                         int w, int h,
                         const style::Style& s,
                         bool is_bound) const;

    // S83 m4h+: small modal text prompt used by action_set_category
    // (and any future header-edit action that doesn't fit inline).
    // Mirrors SwatchesPanel::prompt_text exactly: title in window
    // title bar, Entry prefilled with `initial`, OK / Cancel buttons,
    // Enter activates, Esc cancels. Empty input is passed through
    // to on_ok — each caller decides what empty means (cancel vs.
    // clear-the-field). Self-managed transient window; GTK cleans
    // it up on close.
    void prompt_text(const std::string& title,
                     const std::string& initial,
                     std::function<void(const std::string&)> on_ok);

    // S93 m10: category picker. A dropdown populated with the supplied
    // existing categories — caller passes m_library->user_categories()
    // — plus an "(uncategorised)" entry mapping to "" and a "+ New
    // category…" entry that reveals an inline text field. on_ok fires
    // with the chosen string (existing name, "" for uncategorised, or
    // user-entered new name). Cancel calls on_ok never fires. Used by
    // both action_set_category and action_create_category for a
    // consistent, discoverable category-pick experience.
    void prompt_category_picker(
            const std::string& title,
            const std::string& initial,
            const std::vector<std::string>& existing,
            std::function<void(const std::string&)> on_ok);
};

} // namespace Curvz
