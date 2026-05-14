#include "StylesPanel.hpp"
#include "Canvas.hpp"
#include "SceneNode.hpp"
#include "color/FillStyleInterop.hpp"  // to_paint (FillStyle → Paint) — m4c-3
#include "color/ColorRegion.hpp"        // S85 m4i-cont-3: region_name for hint
#include "color/SwatchLibrary.hpp"      // S85 cont-3: chip swatch resolution
#include "color/Swatch.hpp"             // S85 cont-3: SolidSwatch destructure
#include "style/StyleInterop.hpp"      // materialise_from_style
#include "style/StyleIO.hpp"           // S102 m1: import/export bridge
#include "CurvzLog.hpp"                // S102 m1: LOG_WARN / LOG_INFO in handlers
#include "curvz_utils.hpp"             // s117 m18 v2: apply_motif_class_from_parent
#include "curvz/widgets/Button.hpp"    // s212 m2 — unregistered substrate Button for prompt_text variants
#include "curvz/widgets/DropDown.hpp"  // s212 m2 — registered substrate DropDown (st_cat) + unregistered for new-category prompt
#include "curvz/widgets/Entry.hpp"     // s212 m2 — unregistered substrate Entry for prompt_text variants

#ifdef CURVZ_DIAGNOSTIC
#include "scripting/StylesPanelScriptable.hpp"  // s202 m1: panel-as-Scriptable
#endif

#include <giomm/file.h>                // S102 m1: set_initial_folder for export
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <glibmm/main.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/filedialog.h>          // S102 m1: import/export dialogs
#include <gtkmm/filefilter.h>          // S102 m1: .json filter
#include <gtkmm/gestureclick.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/stringlist.h>        // S83 m4h v12: category dropdown model
#include <gtkmm/window.h>            // S83 m4h+: prompt_text uses Gtk::Window
#include <cairomm/cairomm.h>

#include <algorithm>                  // S83 m4h v12: std::sort for user cats
#include <cmath>
#include <cstdio>                     // S85 m4i-cont-3: snprintf in build_hint_text
#include <filesystem>                 // S102 m1: create_directories for export
#include <set>
#include <sstream>                    // S85 m4i-cont-3: ostringstream in build_hint_text
#include <string>
#include <variant>                    // S85 m4i-cont-3: visit / holds_alternative
#include <unordered_set>

namespace Curvz {

namespace {
// Style-eligible selection (Path + Compound only). Mirrors
// PropertiesPanel.cpp's anonymous-namespace style_selection helper —
// kept local rather than promoted to a shared header because the two
// files are independent and copying ten lines is cheaper than the
// shared-utilities-header overhead in this codebase.
std::vector<SceneNode*> style_eligible_selection(Canvas* canvas) {
    std::vector<SceneNode*> out;
    if (!canvas) return out;
    for (SceneNode* n : canvas->selection()) {
        if (!n) continue;
        if (n->type == SceneNode::Type::Path ||
            n->type == SceneNode::Type::Compound)
            out.push_back(n);
    }
    return out;
}
}  // anonymous namespace

// ── ctor ──────────────────────────────────────────────────────────────────
StylesPanel::StylesPanel()
    : Gtk::Box(Gtk::Orientation::VERTICAL) {

    curvz::utils::set_name(*this, "st", "styles_panel_root");
    set_margin_top(2);
    set_margin_bottom(2);
    set_margin_start(4);
    set_margin_end(4);
    set_spacing(4);

    // ── Action group (S81 m4c-3) ──────────────────────────────────────────
    //
    // One Gio::SimpleActionGroup hosts every menu-driven verb in the
    // panel — both the "+" create flow ("create-empty",
    // "create-from-selection") and the per-row right-click menu
    // ("ctx-rename", "ctx-duplicate", "ctx-delete"). The group is
    // inserted on the panel widget under the "styles" prefix so menu
    // entries reference actions as "styles.create-empty",
    // "styles.ctx-rename", etc. The action muxer walks the widget tree
    // up from the popover to find the group, which is why both the
    // "+" MenuButton and the helper ctx button (which lives inside
    // this panel) resolve correctly without explicit insert_action_group
    // on the popover.
    m_actions = Gio::SimpleActionGroup::create();
    m_actions->add_action("create-empty",
        sigc::mem_fun(*this, &StylesPanel::action_create_empty));
    m_actions->add_action("create-from-selection",
        sigc::mem_fun(*this, &StylesPanel::action_create_from_selection));
    m_actions->add_action("create-category",
        sigc::mem_fun(*this, &StylesPanel::action_create_category));
    m_actions->add_action("ctx-rename",
        sigc::mem_fun(*this, &StylesPanel::action_rename));
    m_actions->add_action("ctx-edit",
        sigc::mem_fun(*this, &StylesPanel::action_edit));
    m_actions->add_action("ctx-edit-copy",
        sigc::mem_fun(*this, &StylesPanel::action_edit_copy));
    m_actions->add_action("ctx-set-category",
        sigc::mem_fun(*this, &StylesPanel::action_set_category));
    m_actions->add_action("ctx-rename-category",
        sigc::mem_fun(*this, &StylesPanel::action_rename_category));
    m_actions->add_action("ctx-delete-category",
        sigc::mem_fun(*this, &StylesPanel::action_delete_category));
    m_actions->add_action("ctx-duplicate",
        sigc::mem_fun(*this, &StylesPanel::action_duplicate));
    m_actions->add_action("ctx-delete",
        sigc::mem_fun(*this, &StylesPanel::action_delete));

    // S102 m1: style interchange. Mirrors S101's palette interchange
    // surface on SwatchesPanel.
    //   load-user      — parameterised by file stem (string), e.g.
    //                    "icons-pack"; reads ~/.config/curvz/styles/<stem>.json
    //   import-styles  — opens a Gtk::FileDialog for any .json
    //   export-styles  — opens a save dialog; defaults to the user-
    //                    styles folder so exports appear in load-user
    m_actions->add_action_with_parameter("load-user",
        Glib::VariantType("s"),
        [this](const Glib::VariantBase& param) {
            auto str_v = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(param);
            on_load_user(str_v.get());
        });
    m_actions->add_action("import-styles",
        sigc::mem_fun(*this, &StylesPanel::on_import_styles));
    m_actions->add_action("export-styles",
        sigc::mem_fun(*this, &StylesPanel::on_export_styles));

    insert_action_group("styles", m_actions);

    // ── Header row ────────────────────────────────────────────────────────
    //
    // Header sits inline (no collapsible wrap here — the wrap comes
    // from MainWindow's make_section caller). Mirrors SwatchesPanel's
    // header shape.
    m_header.set_spacing(6);
    m_header.set_margin_top(2);
    m_header.set_margin_bottom(2);

    // S83 m4h v13: title not appended. The make_section wrapper that
    // hosts this panel already shows "Styles" as the section header;
    // an inner title is redundant. m_title kept as a member for
    // minimal diff and easy re-introduction if needed.
    m_title.set_text("Styles");
    m_title.add_css_class("dim-label");
    m_title.set_xalign(0.0f);
    m_title.set_hexpand(true);
    // m_header.append(m_title);  // intentionally not appended in v13

    // S83 m4h v12: kebab menu items vary based on whether the active
    // category is read-only (app-tier) or user-editable, so the menu
    // is built per-refresh in rebuild_kebab_menu rather than once in
    // the ctor. Pre-v12 the menu was static (3 create items) and lived
    // here. The button itself is configured below; the popover is
    // attached during refresh.
    // S83 m4h v10: kebab (view-more-symbolic) instead of hamburger
    // (open-menu-symbolic). Per GNOME HIG: hamburger is reserved
    // for the app-level primary menu in the header bar; per-panel
    // contextual menus (panel options) belong on a kebab. v8
    // misapplied hamburger here; v10 corrects to the convention.
    m_btn_add.set_icon_name("view-more-symbolic");
    m_btn_add.set_has_frame(false);
    m_btn_add.set_tooltip_text("Style options");
    m_btn_add.set_focus_on_click(false);
    curvz::utils::set_name(m_btn_add, "st_kb", "styles_panel_kebab_btn");
    // S83 m4h v13: kebab appended to m_header now (single-row layout
    // with the dropdown inserted before it during refresh).
    m_header.append(m_btn_add);

    append(m_header);

    // Body — vertical stack of category headers + style rows. Spacing is
    // tight so the list reads as a compact reference, not a form.
    m_body.set_spacing(0);
    m_body.set_margin_start(2);
    m_body.set_margin_end(2);
    append(m_body);

    // ── Hidden context-menu helper (S81 m4c-3, mirrors SwatchesPanel S72) ─
    //
    // Per the SwatchesPanel S72 fix: bare PopoverMenu + set_parent +
    // popup() doesn't wire the action muxer, so item clicks don't
    // dispatch. The fix is to route every right-click PopoverMenu
    // through a hidden, zero-sized MenuButton that lives in the
    // widget tree — its size_allocate path calls gtk_popover_present
    // and the muxer walk resolves "styles.<verb>" actions against
    // the action group inserted on this panel.
    //
    // Visible at zero size + opacity 0 (so it's allocated and has
    // well-defined coordinates for set_pointing_to / translate_
    // coordinates), but contributes nothing to the layout.
    m_ctx_button.set_size_request(0, 0);
    m_ctx_button.set_opacity(0.0);
    m_ctx_button.set_can_focus(false);
    m_ctx_button.set_focus_on_click(false);
    m_ctx_button.set_has_frame(false);
    append(m_ctx_button);

#ifdef CURVZ_DIAGNOSTIC
    // s202 m1 — panel-as-Scriptable foundation. The companion object
    // registers under "pnl_styles" in its ctor (via the Scriptable
    // base) and unregisters in its dtor — which runs when this
    // unique_ptr destroys, i.e. when the panel destroys. No explicit
    // teardown wiring needed.
    //
    // Construction is last in the ctor so every panel member the
    // Scriptable might reach is already constructed. set_library /
    // set_canvas / set_history haven't been called yet — the
    // Scriptable's invoke() guards against null pointers when it can,
    // and the verbs that don't need an attached library (highlight_self,
    // expand_category, collapse_all) work standalone. new_style routes
    // through action_create_empty, which is itself library-safe (it
    // emits the request-style-editor signal that MainWindow handles).
    m_panel_scriptable =
        std::make_unique<curvz::scripting::StylesPanelScriptable>(this);
#endif
}

#ifdef CURVZ_DIAGNOSTIC
// s202 m1 — out-of-line dtor. Required because the header forward-
// declares StylesPanelScriptable; without an out-of-line dtor, every
// translation unit that destroys a StylesPanel would need the full
// type to instantiate unique_ptr's deleter. With the dtor here, the
// header stays slim and only StylesPanel.cpp needs the include.
//
// Destruction order: C++ destroys members in reverse declaration
// order, so m_panel_scriptable (declared first in the private block)
// destroys LAST. That's the right order: the Scriptable's dtor only
// unregisters from the global registry, it never dereferences the
// panel back-pointer, so it's safe for the rest of the panel to
// already be gone. The reverse arrangement would also work — the
// back-pointer isn't touched in dtor — but "construct first, destroy
// last" is the canonical sequence for an object whose lifetime
// brackets the rest.
StylesPanel::~StylesPanel() = default;

// s202 m4 — diagnostic-only section-state hookup. See header.
// Trivial setter: store both handles, no signal wiring needed
// (MainWindow updates the open_flag through its own click handler
// path, the Scriptable reads it on each section_open query).
void StylesPanel::set_section_state(std::shared_ptr<bool> open_flag,
                                    std::function<void(bool)> apply) {
    m_section_open_flag = std::move(open_flag);
    m_section_apply     = std::move(apply);
}

// s202 m5 — composite focus-move hookup. See header for design.
// Two handles stored; the Scriptable composes them at focus_section
// invocation time.
void StylesPanel::set_section_chain(
    std::vector<std::string> chain,
    std::function<void(const std::vector<std::string>&)> focus_closure) {
    m_section_chain = std::move(chain);
    m_section_focus = std::move(focus_closure);
}
#endif

// ── set_library ───────────────────────────────────────────────────────────
void StylesPanel::set_library(style::StyleLibrary* lib) {
    // Drop any previous hooks before swapping libraries. Safe on
    // default-constructed connections (sigc::connection's destructor
    // and disconnect() both no-op when the signal isn't connected).
    m_lib_added_conn.disconnect();
    m_lib_changed_conn.disconnect();
    m_lib_removed_conn.disconnect();

    m_library = lib;

    if (m_library) {
        // All three signals are coarse-grained — refresh rebuilds the
        // whole body either way, so we don't bother filtering by id.
        // Same approach as SwatchesPanel.
        m_lib_added_conn = m_library->signal_style_added().connect(
            [this](style::StyleId /*id*/) { refresh(); });
        m_lib_changed_conn = m_library->signal_style_changed().connect(
            [this](style::StyleId /*id*/) { refresh(); });
        m_lib_removed_conn = m_library->signal_style_removed().connect(
            [this](style::StyleId /*id*/) { refresh(); });
    }

    refresh();
}

// ── set_swatch_library (S85 cont-3) ───────────────────────────────────────
//
// Wire the SwatchLibrary so style row chips can resolve SwatchRef paint
// to the swatch's CURRENT colour and the hover tooltip can print the
// swatch's display name. Without this set, SwatchRef paints render
// with their cached fallback (which can drift from the live swatch
// colour) and tooltips show "<region_name> #rrggbb (swatch)" — the
// pre-cont-3 backlog state.
//
// Hook signal_swatch_changed → refresh so chips repaint live when a
// bound swatch is edited externally (most commonly via SwatchesPanel
// right-click → Edit Colour). Coarse-grained refresh matches the
// style-library hooks above; the row count is small.
void StylesPanel::set_swatch_library(color::SwatchLibrary* lib) {
    m_swatch_lib_changed_conn.disconnect();

    m_swatch_library = lib;

    if (m_swatch_library) {
        m_swatch_lib_changed_conn =
            m_swatch_library->signal_swatch_changed().connect(
                [this](const color::SwatchId& /*id*/) { refresh(); });
    }

    refresh();
}

// ── set_canvas ────────────────────────────────────────────────────────────
void StylesPanel::set_canvas(Canvas* canvas) {
    m_canvas_selection_conn.disconnect();

    m_canvas = canvas;

    if (m_canvas) {
        // Hook selection change → refresh. m4c-1 doesn't visibly use
        // this, but m4c-2's active-binding indicator will need to
        // re-light when the user moves selection between bound and
        // unbound objects. Wiring now keeps the shape stable and
        // saves a follow-on edit when the indicator lights up.
        //
        // Signal carries SceneNode* (the new primary). We don't need
        // it — refresh reads the live selection from the canvas. But
        // the lambda must accept it; sigc can't bind a no-arg lambda
        // to a one-arg signal.
        m_canvas_selection_conn =
            m_canvas->signal_selection_changed().connect(
                [this](SceneNode* /*sel*/) { refresh(); });
    }
}

// ── set_active_category (S87) ─────────────────────────────────────────────
//
// Used by MainWindow on project load to restore the dropdown selection
// from CurvzProject's editor_state. Writes both flags then calls
// refresh() so the panel rebuilds with the new selection — refresh's
// existing logic locates the matching index in m_category_order and
// pre-selects it via the syncing-flag-guarded path.
//
// If the saved category no longer exists in the loaded library (e.g.
// the user deleted the category in another session), refresh's
// fallback picks index 0 and overwrites m_active_category to whatever's
// there, keeping the panel coherent. The persisted (now-stale) value
// is overwritten on the next save, so it's a clean recovery.
void StylesPanel::set_active_category(const std::string& cat,
                                      bool is_app_tier) {
    m_active_category    = cat;
    m_active_is_app_tier = is_app_tier;
    refresh();
}

// ── refresh ───────────────────────────────────────────────────────────────
//
// S83 m4h v12 rewrite. Old structure: render every category as a
// section header followed by its style rows. New structure: build
// a category dropdown chooser, render only the active category's
// style rows in the body. Kebab menu rebuilt per-refresh so its
// items can depend on active-category state (Rename / Delete
// category only available for user-tier non-empty active cat).
void StylesPanel::refresh() {
    // Drop the dropdown signal connection and pointer first — the
    // dropdown is one of the children we're about to tear down.
    // Without disconnecting, the signal handler would fire during
    // teardown and re-enter via set_active_palette. The pointer is
    // nulled because m_category_dropdown becomes dangling after
    // remove. Same idiom as SwatchesPanel.
    m_dropdown_sel_conn.disconnect();
    m_category_dropdown = nullptr;

    // S83 m4h v13: tear down any previous dropdown in m_header.
    // The kebab (m_btn_add) lives in m_header for the panel's
    // lifetime — skip it. Anything else (the previous dropdown or
    // its placeholder) gets removed. Children are managed so GTK
    // owns them after remove.
    //
    // s212 m2: with `st_cat` now a registered substrate widget, the
    // refresh path needs `force_unregister_subtree` BEFORE the
    // `m_header.remove(...)` call — otherwise the next refresh's
    // construction of `st_cat` would collide with the about-to-die
    // predecessor whose registry entry hasn't been cleared yet
    // (GTK4's deferred-destruction model means the destructor that
    // would call force_unregister_subtree in its own dtor hasn't
    // run yet at remove() time). Same discipline s208 m5 added to
    // SwatchesPanel for `sw_pal`. Third canonical instance of the
    // s199 m1 pump being used outside its PropertiesPanel home.
    {
        auto* child = m_header.get_first_child();
        while (child) {
            auto* next = child->get_next_sibling();
            if (child != static_cast<Gtk::Widget*>(&m_btn_add)) {
                curvz::utils::force_unregister_subtree(child);
                m_header.remove(*child);
            }
            child = next;
        }
    }

    // Tear down body. Children are managed (Gtk::make_managed
    // throughout) so ownership returns to GTK on remove. The body
    // contains no substrate Scriptables today (chooser + active body
    // are built from raw GTK widgets), but the force_unregister walk
    // is null-safe and idempotent — adding it keeps the pattern
    // uniform across both m_header and m_body teardowns, and any
    // future substrate migration in the body is covered without
    // further teardown changes (mirror of s208 m5's SwatchesPanel
    // forward-discipline comment).
    for (Gtk::Widget* c = m_body.get_first_child(); c;
         c = c->get_next_sibling()) {
        curvz::utils::force_unregister_subtree(c);
    }
    while (auto* child = m_body.get_first_child())
        m_body.remove(*child);

    if (!m_library) {
        rebuild_kebab_menu();
        return;
    }

    build_chooser();
    build_active_body();
    rebuild_kebab_menu();
}

// ── build_chooser (S83 m4h v12) ───────────────────────────────────────────
//
// Build the category dropdown and append it to m_body. Items in
// dropdown order:
//
//   1. App-tier categories (e.g. "Built-in"), in library insertion
//      order. Pinned at the top — these are read-only and shouldn't
//      mix with user content.
//   2. "(uncategorised)" bucket if any user style has empty category.
//   3. User-tier non-empty categories, sorted alphabetically (case-
//      sensitive, locale-default — same Style.hpp convention as
//      "no normalisation").
//
// Selection is restored from m_active_category / m_active_is_app_tier.
// If the previously-active category no longer exists (e.g. the user
// deleted it), we fall through to the first dropdown item.
//
// m_category_order is populated parallel to the StringList: each
// entry is a (category-string, is_app_tier) pair. The selection-
// changed handler reads from this to translate index → state.
void StylesPanel::build_chooser() {
    // Gather app-tier categories (in insertion order — pinned at top).
    auto app_cats = m_library->app_categories();

    // Gather user-tier categories. user_categories() returns insertion
    // order; we sort alphabetically for the dropdown view. We separate
    // empty-string ("(uncategorised)") from non-empty so the empty
    // bucket can sit between app and user-named categories.
    auto user_cats_raw = m_library->user_categories();
    bool has_uncat = false;
    std::vector<std::string> user_cats_named;
    for (const auto& c : user_cats_raw) {
        if (c.empty()) has_uncat = true;
        else user_cats_named.push_back(c);
    }
    std::sort(user_cats_named.begin(), user_cats_named.end());

    // Build the StringList model + parallel order vector. Each
    // dropdown item maps to one (category, is_app_tier) pair.
    auto string_list = Gtk::StringList::create({});
    m_category_order.clear();

    for (const auto& c : app_cats) {
        // Display "Built-in" as-is. Empty app categories shouldn't
        // exist in current seed data but are passed through if they
        // do (defensive — same conversion as user empty).
        const std::string disp = c.empty() ? std::string("(uncategorised)") : c;
        string_list->append(disp);
        m_category_order.emplace_back(c, /*is_app_tier=*/true);
    }
    if (has_uncat) {
        string_list->append("(uncategorised)");
        m_category_order.emplace_back(std::string{}, /*is_app_tier=*/false);
    }
    for (const auto& c : user_cats_named) {
        string_list->append(c);
        m_category_order.emplace_back(c, /*is_app_tier=*/false);
    }

    // Pathological empty case: no categories at all (no app styles +
    // no user styles). Render a dim placeholder; no dropdown.
    if (m_category_order.empty()) {
        // S83 m4h v13: placeholder lives in m_header alongside the
        // kebab so the row stays populated visually. prepend to put
        // it before the kebab.
        auto* placeholder = Gtk::make_managed<Gtk::Label>("(no categories)");
        placeholder->add_css_class("dim-label");
        placeholder->set_xalign(0.0f);
        placeholder->set_hexpand(true);
        m_header.prepend(*placeholder);
        return;
    }

    auto* drop = Gtk::make_managed<curvz::widgets::DropDown>(
                     "st_cat", string_list);
    curvz::utils::set_name(drop, "st_cat", "styles_panel_category_dd");
    drop->set_hexpand(true);
    // S88: unified inspector dropdown styling. `flat` removes GTK4's
    // always-on dropdown border/background; `prop-dropdown` aligns the
    // font size with the rest of the inspector (11px). Was `flat caption`
    // — caption's GNOME-stylesheet size was visibly larger than the
    // inspector's Units/Mode dropdowns and broke font uniformity.
    drop->add_css_class("flat");
    drop->add_css_class("prop-dropdown");
    m_category_dropdown = drop;

    // Locate the active category in the order. If not found (e.g.
    // category was just deleted), fall through to index 0 and update
    // m_active_category accordingly so kebab-menu state stays in sync.
    int active_idx = -1;
    for (size_t i = 0; i < m_category_order.size(); ++i) {
        if (m_category_order[i].first  == m_active_category &&
            m_category_order[i].second == m_active_is_app_tier) {
            active_idx = static_cast<int>(i);
            break;
        }
    }
    if (active_idx < 0) {
        active_idx = 0;
        m_active_category    = m_category_order[0].first;
        m_active_is_app_tier = m_category_order[0].second;
    }

    // Programmatic selection fires the changed signal; suppress the
    // re-entry into refresh() with the syncing flag.
    m_syncing_dropdown = true;
    drop->set_selected(static_cast<guint>(active_idx));
    m_syncing_dropdown = false;

    m_dropdown_sel_conn = drop->property_selected().signal_changed().connect(
        [this]() {
            if (m_syncing_dropdown || !m_category_dropdown) return;
            guint idx = m_category_dropdown->get_selected();
            if (idx == GTK_INVALID_LIST_POSITION) return;
            if (idx >= m_category_order.size()) return;
            const auto& chosen = m_category_order[idx];
            if (chosen.first  == m_active_category &&
                chosen.second == m_active_is_app_tier) return;  // no-op
            m_active_category    = chosen.first;
            m_active_is_app_tier = chosen.second;
            // S87 — view state changed; MainWindow persists.
            m_sig_view_state_changed.emit();
            refresh();
        });

    // S83 m4h v13: dropdown prepended to m_header so layout reads
    // [dropdown ▾] [⋮]. The kebab is already there; prepend puts
    // the dropdown before it. hexpand makes the dropdown fill the
    // available width, pushing the kebab to the right edge.
    m_header.prepend(*drop);
}

// ── build_active_body (S83 m4h v12) ───────────────────────────────────────
//
// Render the style rows for the currently-active category. Uses the
// app-tier or user-tier accessor depending on m_active_is_app_tier.
// The bound-style indicator logic is identical to the pre-v12
// build_tier (gather selection's bound styles, light up matching
// rows).
void StylesPanel::build_active_body() {
    if (m_category_order.empty()) return;  // placeholder shown by chooser

    // Bound style ids across the current selection — drives the
    // active-binding indicator on each row.
    std::set<style::StyleId> bound_ids;
    if (m_canvas) {
        for (SceneNode* n : style_eligible_selection(m_canvas)) {
            if (n && !n->bound_style.empty())
                bound_ids.insert(n->bound_style);
        }
    }

    auto styles = m_active_is_app_tier
        ? m_library->app_styles_in_category(m_active_category)
        : m_library->user_styles_in_category(m_active_category);

    if (styles.empty()) {
        // Active category is empty (last member was just deleted).
        // Quiet placeholder — explains why the body is bare without
        // an apology. The user can pick another category in the
        // dropdown or use the kebab to add a new style.
        auto* hint = Gtk::make_managed<Gtk::Label>("(no styles in this category)");
        hint->add_css_class("dim-label");
        hint->set_xalign(0.0f);
        hint->set_margin_start(8);
        hint->set_margin_top(4);
        hint->set_margin_bottom(4);
        m_body.append(*hint);
        return;
    }

    // S85 cont-5: chip-grid replaces the per-row list. One FlowBox
    // hosts every chip in the active category. Layout mirrors
    // SwatchesPanel — same chip metrics, same row spacing — so the
    // two panels read as one consistent visual idiom.
    auto* flow = Gtk::make_managed<Gtk::FlowBox>();
    flow->set_orientation(Gtk::Orientation::HORIZONTAL);
    flow->set_homogeneous(true);
    flow->set_min_children_per_line(1);
    flow->set_max_children_per_line(16);
    flow->set_row_spacing(4);
    flow->set_column_spacing(4);
    flow->set_selection_mode(Gtk::SelectionMode::NONE);
    flow->set_margin_start(4);
    flow->set_margin_end(4);
    flow->set_margin_top(2);
    flow->set_margin_bottom(2);

    for (const style::Style* s : styles) {
        if (!s) continue;
        const bool is_bound = bound_ids.count(s->header.id) > 0;
        flow->append(*make_style_chip(*s, is_bound));
    }
    m_body.append(*flow);
}

// ── rebuild_kebab_menu (S83 m4h v12, extended S102 m1) ───────────────────
//
// Build the kebab popover menu fresh on each refresh. Items vary by
// active-category state and library state:
//
//   Always:
//     New style
//     New style from selection
//     New category…
//     ──────────
//     Load styles ▶            (submenu: per-pack stems + Import…)
//
//   When user tier is non-empty:
//     Export user styles…      (sibling of Load)
//
//   When active category is user-tier non-empty:
//     ──────────
//     Rename category…
//     Delete category
//
// Pre-v12 the kebab menu was static (3 create items, built once in
// the ctor). Conditional Rename/Delete category items moved here from
// the old per-header right-click — the dropdown chooser eliminated
// the headers, so the affordance had to land somewhere; the kebab is
// the natural home, mirroring SwatchesPanel's "active palette"
// operations.
//
// S102 m1 added the interchange section (Load styles / Export user
// styles…). Sectioning via append_section so the visual separators
// read cleanly. Load styles is always present — Import… inside the
// submenu is the empty-state escape hatch (the same idiom as
// SwatchesPanel's Load palette ▶ submenu).
void StylesPanel::rebuild_kebab_menu() {
    auto menu = Gio::Menu::create();

    // Section 1: creation primitives. Always present.
    auto sec_create = Gio::Menu::create();
    sec_create->append("New style",                "styles.create-empty");
    sec_create->append("New style from selection", "styles.create-from-selection");
    sec_create->append("New category…",            "styles.create-category");
    menu->append_section(sec_create);

    // Section 2: interchange (load + export). Mirrors SwatchesPanel's
    // section 2 — Load submenu populated from enumerate_user(), with
    // Import… always at the bottom; Export sibling appears only when
    // there's something to export.
    auto sec_interchange = Gio::Menu::create();
    {
        auto load_sub = Gio::Menu::create();

        // User packs from ~/.config/curvz/styles/. Skip the section
        // entirely if no packs exist — an empty separator above
        // Import… reads as a UI bug.
        auto user_stems = style::style_io::enumerate_user();
        if (!user_stems.empty()) {
            auto load_user_sec = Gio::Menu::create();
            for (const auto& stem : user_stems) {
                // Display label: the stem itself. Style packs don't
                // carry a "name" header the way GPL palettes do, so
                // there's nothing richer to show. Users name the
                // file in the export dialog; the stem becomes the
                // user-facing label.
                std::string action =
                    "styles.load-user('" + stem + "')";
                load_user_sec->append(stem, action);
            }
            load_sub->append_section(load_user_sec);
        }

        auto load_import_sec = Gio::Menu::create();
        load_import_sec->append("Import…", "styles.import-styles");
        load_sub->append_section(load_import_sec);

        sec_interchange->append_submenu("Load styles", load_sub);

        // Export — only meaningful when the user tier has at least one
        // style. Library-null safe: the guard short-circuits before
        // touching m_library.
        if (m_library && m_library->user_style_count() > 0) {
            sec_interchange->append("Export user styles…",
                                    "styles.export-styles");
        }
    }
    menu->append_section(sec_interchange);

    // Section 3: category management — Rename / Delete category only
    // on user-tier non-empty active. App-tier ("Built-in") is read-
    // only; "(uncategorised)" is the default state, not a real
    // category to rename or delete.
    const bool can_edit_active =
        !m_active_is_app_tier && !m_active_category.empty();
    if (can_edit_active) {
        auto sec_manage = Gio::Menu::create();
        sec_manage->append("Rename category…", "styles.ctx-rename-category");
        sec_manage->append("Delete category",  "styles.ctx-delete-category");
        menu->append_section(sec_manage);
    }

    auto* popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
    popover->set_has_arrow(false);
    m_btn_add.set_popover(*popover);
}

// ── Style interchange handlers (S102 m1) ─────────────────────────────────
//
// Three entry points wired to the kebab actions:
//   * on_load_user(stem)  — load a previously-exported user pack
//   * on_import_styles()  — open file dialog, load any .json
//   * on_export_styles()  — save dialog, write the user tier
//
// All three route through style::style_io. This panel only handles
// the GTK-side plumbing (file dialogs, library_changed emit, refresh).

void StylesPanel::on_load_user(const std::string& stem) {
    if (!m_library) return;
    if (stem.empty()) return;

    auto loaded = style::style_io::load_user(stem);
    if (!loaded) {
        LOG_WARN("StylesPanel::on_load_user: load failed for '{}'", stem);
        return;
    }

    std::size_t added = style::style_io::import_styles_into_library(
        *m_library, m_history, *loaded);
    if (added == 0) {
        LOG_INFO("StylesPanel::on_load_user: '{}' contained no importable "
                 "styles", stem);
        return;
    }

    m_sig_library_changed.emit();
    m_sig_request_inspector_refresh.emit();
    refresh();
}

void StylesPanel::on_import_styles() {
    if (!m_library) return;

    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Import styles");

    // Filter to .json. All Files as fallback for users with no-
    // extension or .txt-renamed files; the loader will reject
    // anything that isn't shape-compatible.
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto json_filter = Gtk::FileFilter::create();
    json_filter->set_name("Curvz style packs (*.json)");
    json_filter->add_pattern("*.json");
    filters->append(json_filter);
    auto all_filter = Gtk::FileFilter::create();
    all_filter->set_name("All files");
    all_filter->add_pattern("*");
    filters->append(all_filter);
    dialog->set_filters(filters);
    dialog->set_default_filter(json_filter);

    auto* root = dynamic_cast<Gtk::Window*>(get_root());
    if (!root) {
        LOG_WARN("StylesPanel::on_import_styles: no root window");
        return;
    }

    dialog->open(*root,
        [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto file = dialog->open_finish(result);
                if (!file) return;
                if (!m_library) return;
                std::string path = file->get_path();

                auto loaded = style::style_io::load_path(path);
                if (!loaded) {
                    LOG_WARN("StylesPanel::on_import_styles: load failed "
                             "for '{}'", path);
                    return;
                }

                std::size_t added = style::style_io::import_styles_into_library(
                    *m_library, m_history, *loaded);
                if (added == 0) {
                    LOG_INFO("StylesPanel::on_import_styles: '{}' contained "
                             "no importable styles", path);
                    return;
                }

                m_sig_library_changed.emit();
                m_sig_request_inspector_refresh.emit();
                refresh();
            } catch (const Glib::Error&) {
                // User cancelled or dialog error — silent.
            }
        });
}

void StylesPanel::on_export_styles() {
    if (!m_library) return;

    // Snapshot the user tier up front. The user may take a long time
    // picking a destination, and the library could mutate while the
    // dialog is open; capturing the value at click time is what they
    // intended to export.
    auto snapshot = style::style_io::snapshot_user_tier(*m_library);
    if (snapshot.empty()) {
        LOG_INFO("StylesPanel::on_export_styles: user tier is empty, "
                 "nothing to export");
        return;
    }

    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Export user styles");
    dialog->set_initial_name("styles.json");

    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto json_filter = Gtk::FileFilter::create();
    json_filter->set_name("Curvz style packs (*.json)");
    json_filter->add_pattern("*.json");
    filters->append(json_filter);
    dialog->set_filters(filters);
    dialog->set_default_filter(json_filter);

    // Default to ~/.config/curvz/styles/ so user exports land where
    // the kebab's "Load styles" submenu looks for them. Create the
    // directory if missing — first export bootstraps it. Fail-soft:
    // if mkdir fails (permissions, disk full, …) skip the initial-
    // folder hint and let the dialog open wherever GTK defaults to.
    namespace fs = std::filesystem;
    const std::string user_dir = style::style_io::user_styles_dir();
    std::error_code mkdir_ec;
    fs::create_directories(user_dir, mkdir_ec);
    if (!mkdir_ec) {
        dialog->set_initial_folder(Gio::File::create_for_path(user_dir));
    } else {
        LOG_WARN("StylesPanel::on_export_styles: cannot create '{}': "
                 "{} (skipping initial-folder hint)",
                 user_dir, mkdir_ec.message());
    }

    auto* root = dynamic_cast<Gtk::Window*>(get_root());
    if (!root) {
        LOG_WARN("StylesPanel::on_export_styles: no root window");
        return;
    }

    dialog->save(*root,
        [this, dialog, snapshot](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto file = dialog->save_finish(result);
                if (!file) return;
                std::string path = file->get_path();
                // Append .json if the user didn't.
                if (path.size() < 5 ||
                    path.compare(path.size() - 5, 5, ".json") != 0) {
                    path += ".json";
                }
                if (!style::style_io::write_path(path, snapshot)) {
                    LOG_WARN("StylesPanel::on_export_styles: write failed "
                             "for '{}'", path);
                    return;
                }
                // Rebuild so a fresh export lands in the Load styles
                // submenu without waiting for the next refresh.
                refresh();
            } catch (const Glib::Error&) {
                // User cancelled or dialog error — silent.
            }
        });
}

// ── make_style_chip (S85 cont-5) ──────────────────────────────────────────
//
// Chip-grid rewrite of the style row. Returns a 24×24 DrawingArea
// rendering the style's fill + stroke + active-binding ring. Replaces
// the pre-cont-5 [chip][name][dot] row layout. Information that used
// to live in the name label and binding-indicator dot is now:
//
//   * Style name → hover tooltip (build_hint_text already produces
//     a multi-line summary including name as line 1).
//   * Active-binding indicator → the white-inner / black-outer ring
//     drawn inside the chip when is_bound (mirror of SwatchesPanel's
//     "active paint matches selection" idiom).
//
// Click semantics:
//   * Primary single-click — no-op (matches pre-cont-5 row behaviour).
//   * Primary double-click — bind current selection to this style.
//   * Secondary click — context menu (Edit / Rename / Duplicate /
//     Delete for user tier; Edit a copy / Duplicate to user for app
//     tier). Same menu items as the pre-cont-5 row.
//
// Inline rename is gone — the name label that hosted the Entry no
// longer exists. Right-click → Rename now opens a modal prompt_text
// dialog (same pattern SwatchesPanel uses for swatch rename), routed
// through commit_rename(id, new_name) which carries the Update
// StyleCommand push logic that commit_inline_rename used.
//
// CSS class: `swatch-chip` to inherit hover styling from SwatchesPanel.
Gtk::Widget* StylesPanel::make_style_chip(const style::Style& s, bool is_bound) {
    const style::StyleId id = s.header.id;
    const bool is_app = style::is_built_in(id);

    auto* area = Gtk::make_managed<Gtk::DrawingArea>();
    constexpr int kChipPx = 24;
    area->set_content_width(kChipPx);
    area->set_content_height(kChipPx);
    area->set_valign(Gtk::Align::CENTER);
    area->set_halign(Gtk::Align::CENTER);
    area->add_css_class("swatch-chip");
    area->set_cursor(Gdk::Cursor::create("pointer"));

    // Capture the Style by value: SwatchRef in fill / stroke needs a
    // current resolve through m_swatch_library on every draw, which
    // means the closure has to hold the binding ids. Style is small;
    // copying a few doubles is cheap, and it sidesteps any "captured-
    // pointer dangles after refresh" risk.
    style::Style style_copy = s;
    area->set_draw_func(
        [this, style_copy, is_bound]
        (const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            draw_style_chip(cr, w, h, style_copy, is_bound);
        });

    // Hover tooltip — full multi-line summary. build_hint_text already
    // prepends the style name as line 1, so this surface gives the user
    // everything the pre-cont-5 row text + extras carried.
    area->set_tooltip_text(build_hint_text(s));
    area->set_has_tooltip(true);

    // ── Primary click: double-click binds ───────────────────────────────
    auto click = Gtk::GestureClick::create();
    click->set_button(GDK_BUTTON_PRIMARY);
    click->signal_released().connect(
        [this, id](int n_press, double /*x*/, double /*y*/) {
            if (n_press >= 2) {
                bind_selection_to(id);
            }
            // n_press == 1: no-op. Hover tooltip is the data-preview.
        });
    area->add_controller(click);

    // ── Right-click: context menu ─────────────────────────────────────────
    auto rc = Gtk::GestureClick::create();
    rc->set_button(GDK_BUTTON_SECONDARY);
    rc->signal_released().connect(
        [this, id, is_app, area]
        (int /*n_press*/, double x, double y) {
            m_ctx_style_id = id;

            auto menu = Gio::Menu::create();
            if (is_app) {
                menu->append("Edit a copy…",          "styles.ctx-edit-copy");
                menu->append("Duplicate to user tier", "styles.ctx-duplicate");
            } else {
                menu->append("Edit…",       "styles.ctx-edit");
                menu->append("Rename…",     "styles.ctx-rename");
                menu->append("Category…",   "styles.ctx-set-category");
                menu->append("Duplicate",   "styles.ctx-duplicate");
                menu->append("Delete",      "styles.ctx-delete");
            }

            auto* pop = Gtk::make_managed<Gtk::PopoverMenu>(menu);
            pop->set_has_arrow(false);
            m_ctx_button.set_popover(*pop);

            double bx = x, by = y;
            if (!area->translate_coordinates(m_ctx_button, x, y, bx, by)) {
                bx = 0; by = 0;
            }
            Gdk::Rectangle rect(static_cast<int>(bx),
                                static_cast<int>(by), 1, 1);
            pop->set_pointing_to(rect);

            m_ctx_button.popup();
        });
    area->add_controller(rc);

    return area;
}


// ── bind_selection_to (S80 m4c-2) ─────────────────────────────────────────
//
// Click-to-bind handler. Walks the Path/Compound selection, builds a
// per-target snapshot, mutates each target (sets bound_style + materialises
// fill/stroke from the live style), pushes one atomic BindStyleCommand
// for undo, then refreshes the panel + canvas + inspector.
//
// Pattern: mutate state directly first, THEN push the command (push
// records before+after for undo, doesn't execute). Same convention as
// EditObjectCommand callers in PropertiesPanel.cpp and the m3a test
// harness BindStyleCommand wiring (S80 m4b).
//
// Per-target miss recovery: when materialise_from_style fails on a
// target (style id unknown — defensive only, app stubs are hardcoded),
// the pre-bind cache is restored from the snapshot we just took, and
// that target is excluded from the snap list passed to the command.
// Avoids "phantom undo of nothing" if a heterogeneous selection has
// some targets that fail to bind.
void StylesPanel::bind_selection_to(const style::StyleId& id) {
    if (!m_library || !m_history || !m_canvas) return;

    auto targets = style_eligible_selection(m_canvas);
    if (targets.empty()) {
        // No selection — silently no-op. The row consumed the click
        // (cursor went down on it) but nothing to bind. Future m5
        // could surface a status-bar hint here; for now the absence
        // of any visual feedback IS the signal.
        return;
    }

    // Snapshot pre-bind state for ALL targets first.
    std::vector<BindStyleCommand::TargetSnap> snaps;
    snaps.reserve(targets.size());
    for (SceneNode* n : targets) {
        BindStyleCommand::TargetSnap ts;
        ts.obj                     = n;
        ts.bound_style_before      = n->bound_style;
        ts.fill_before             = n->fill;
        ts.fill_swatch_id_before   = n->fill_swatch_id;
        ts.stroke_before           = n->stroke;
        ts.stroke_swatch_id_before = n->stroke_swatch_id;
        snaps.push_back(std::move(ts));
    }

    int ok_count = 0;
    for (auto& s : snaps) {
        s.obj->bound_style = id;
        if (style::materialise_from_style(*s.obj, *m_library)) {
            ++ok_count;
        } else {
            // Lookup miss — restore pre-bind state on this target.
            s.obj->bound_style       = s.bound_style_before;
            s.obj->fill              = s.fill_before;
            s.obj->fill_swatch_id    = s.fill_swatch_id_before;
            s.obj->stroke            = s.stroke_before;
            s.obj->stroke_swatch_id  = s.stroke_swatch_id_before;
        }
    }

    if (ok_count == 0) {
        // Nothing landed — don't push a no-op command.
        return;
    }

    // Filter snaps to only successfully-bound targets before push.
    std::vector<BindStyleCommand::TargetSnap> kept;
    kept.reserve(ok_count);
    for (auto& s : snaps) {
        if (s.obj->bound_style == id)
            kept.push_back(std::move(s));
    }

    m_history->push(std::make_unique<BindStyleCommand>(
        m_library,
        std::string(id),
        std::move(kept),
        kept.size() > 1 ? std::string("Bind style (multiple)")
                        : std::string("Bind style")));

    m_canvas->queue_draw();
    refresh();  // active-binding indicator shifts to this row
    m_sig_request_inspector_refresh.emit();  // inspector "Bound: <n>" updates
}

// ── next_style_name (S81 m4c-3) ───────────────────────────────────────────
//
// "Style 1", "Style 2"… — lowest free integer against the current user
// tier names. Same idiom as Affinity Designer's auto-naming: gaps in
// the sequence are filled before the next-highest is taken (so deleting
// "Style 2" and creating again gives "Style 2", not "Style 4").
//
// Linear scan, hash-set membership: O(N) per call, fine for sub-100
// user styles. Future Phase 2 may want a more principled name policy
// (per-category counters, etc.) but the simple lowest-free is good
// enough for m4c-3.
std::string StylesPanel::next_style_name() const {
    if (!m_library) return "Style 1";

    std::unordered_set<std::string> taken;
    auto cats = m_library->user_categories();
    for (const auto& cat : cats) {
        for (const style::Style* s : m_library->user_styles_in_category(cat)) {
            if (s) taken.insert(s->header.name);
        }
    }

    for (int i = 1; i < 10000; ++i) {
        std::string candidate = "Style " + std::to_string(i);
        if (!taken.count(candidate)) return candidate;
    }
    // Pathological fallback — should never trigger in practice.
    return "Style";
}

// ── action_create_empty (S81 m4c-3, dialogified S85 m4i-cont-1) ───────────
//
// "+" → "New style". Opens the Style Editor dialog seeded with a
// default-appearance Style (Paint::None fill, default StrokeAppearance,
// auto-name, current active category). OK pushes AddStyleCommand.
//
// Pre-S85 this was a silent-create path that pushed AddStyleCommand
// immediately with the defaults. Per session protocol decision the
// dialog opens here so the user can dial in name / category / colours
// up front, rather than create-then-edit. The "Create from selection"
// fast path remains silent — there's nothing to dial in there.
void StylesPanel::action_create_empty() {
    if (!m_library || !m_history) return;

    style::Style seed;
    seed.header.name     = next_style_name();
    // Pre-fill the active category so the dialog opens with it
    // selected — matches "create lands where the user is currently
    // looking" behaviour from the kebab.
    seed.header.category = m_active_is_app_tier ? "" : m_active_category;
    // seed.fill / seed.stroke take Paint{None{}} / default
    // StrokeAppearance from the data-model defaults — exactly what
    // an "empty" style should look like.

    // Capture by-value so the closure is self-contained — the panel
    // pointer is captured raw because the panel outlives the dialog
    // (MainWindow owns both, and the dialog closes synchronously
    // before the panel could be destroyed).
    auto on_committed = [this](style::Style result) {
        if (!m_library || !m_history) return;

        auto cmd = std::make_unique<AddStyleCommand>(
            m_library, std::move(result), std::string("Create style"));
        cmd->execute();
        m_history->push(std::move(cmd));

        m_sig_library_changed.emit();
        // Library's signal_style_added → panel refresh handles the
        // visual update; no explicit refresh() needed here.
    };

    m_sig_request_style_editor.emit(StyleEditorDialog::Mode::New,
                                    std::move(seed),
                                    std::move(on_committed));
}

// ── action_edit (S85 m4i-cont-1) ──────────────────────────────────────────
//
// Right-click on a user-tier row → "Edit…". Opens the Style Editor
// dialog seeded with the row's existing Style. OK pushes
// UpdateStyleCommand carrying full before/after snapshots.
//
// Defensive against the row's id being app-tier (menu hides Edit on
// app rows; this guards the action-route anyway since muxer
// dispatch in GTK4 doesn't honour menu-time visibility).
void StylesPanel::action_edit() {
    if (!m_library || !m_history)        return;
    if (m_ctx_style_id.empty())          return;
    if (style::is_built_in(m_ctx_style_id)) return;

    const style::Style* current = m_library->find_style(m_ctx_style_id);
    if (!current) return;

    style::Style before = *current;     // snapshot for undo
    style::StyleId id   = m_ctx_style_id;

    // Closure captures id + before so the OK callback can build the
    // command without re-reading m_ctx_style_id (which may have
    // shifted to another row by the time the user hits OK — a
    // dialog session is a long-running thing relative to right-click).
    auto on_committed =
        [this, id, before](style::Style after) {
            if (!m_library || !m_history) return;

            // Skip pushing a Style that round-trips identically through
            // the dialog. S98: replaced the field-by-field predicate
            // with a single Style operator== — the field-by-field form
            // had been bitten twice (S85 fix added Paint variants after
            // colour-only edits got dropped; S98 m2 added shadow after
            // shadow-only edits got dropped). The structural fix is to
            // make Style equality the seam: Style.hpp's operator== walks
            // every field, and adding a new field to Style forces an
            // update there in lockstep — one place, visible breakage if
            // you forget. See Style.hpp "Equality" comment block.
            if (before == after) return;

            // The dialog preserved the id through OK in Edit mode;
            // re-set defensively in case some future dialog code path
            // clears it.
            after.header.id = id;

            auto cmd = std::make_unique<UpdateStyleCommand>(
                m_library, id, before, std::move(after),
                std::string("Edit style"));
            cmd->execute();
            m_history->push(std::move(cmd));

            m_sig_library_changed.emit();
        };

    m_sig_request_style_editor.emit(StyleEditorDialog::Mode::Edit,
                                    before,
                                    std::move(on_committed));
}

// ── action_edit_copy (S85 m4i-cont-1) ─────────────────────────────────────
//
// Right-click on an app-tier row → "Edit a copy…". Opens the dialog
// seeded with the app style's appearance and a " copy" suffix on the
// name; OK pushes AddStyleCommand for a fresh user-tier style.
//
// This sits alongside the existing "Duplicate to user tier" entry in
// the same menu — both produce a user-tier copy, but this one lets the
// user customise before the copy lands. Useful when the user knows up
// front they want to adapt the colours rather than duplicate-then-edit.
void StylesPanel::action_edit_copy() {
    if (!m_library || !m_history) return;
    if (m_ctx_style_id.empty())   return;

    const style::Style* src = m_library->find_style(m_ctx_style_id);
    if (!src) return;

    // Seed: app-style appearance, fresh-empty id (library mints on
    // add), name with " copy" suffix per the duplicate convention,
    // category preserved. App-tier styles live under "Built-in" which
    // is read-only as a category — the duplicate moves to
    // "(uncategorised)" so the user picks a real home in the dialog.
    style::Style seed = *src;
    seed.header.id.clear();
    seed.header.name = src->header.name + " copy";
    seed.header.category = "";

    auto on_committed = [this](style::Style result) {
        if (!m_library || !m_history) return;

        auto cmd = std::make_unique<AddStyleCommand>(
            m_library, std::move(result),
            std::string("Duplicate style"));
        cmd->execute();
        m_history->push(std::move(cmd));

        m_sig_library_changed.emit();
    };

    m_sig_request_style_editor.emit(StyleEditorDialog::Mode::Duplicate,
                                    std::move(seed),
                                    std::move(on_committed));
}

// ── action_create_from_selection (S81 m4c-3) ──────────────────────────────
//
// "+" → "Create from selection". Reads the primary selection's
// fill / stroke and projects them onto a new Style. Path / Compound
// only (same eligibility rule as bind). No-op when no eligible
// selection exists.
//
// Naming: "Style from <object name>" if the source has a non-empty
// name; otherwise the lowest-free "Style N". The captured name uses
// the source's friendly name (SceneNode::name), not its SVG id.
//
// Note: this is the first place that exercises m3d's cross-doc walk
// for "create"-style mutations — but only via the no-binding path,
// because no objects are bound to the freshly-minted id yet. The
// real cross-doc exercise lands in m4d when update_style fires on
// a style that has live bindings.
void StylesPanel::action_create_from_selection() {
    if (!m_library || !m_history || !m_canvas) return;

    auto targets = style_eligible_selection(m_canvas);
    if (targets.empty()) {
        // No selection — silently no-op. The "+" menu item could be
        // greyed out instead, but that requires a per-frame re-evaluate
        // hook; defer to m5 polish.
        return;
    }

    SceneNode* src = nullptr;
    SceneNode* primary = m_canvas->primary_selection();
    if (primary) {
        for (SceneNode* n : targets) {
            if (n == primary) { src = n; break; }
        }
    }
    if (!src) src = targets.front();
    if (!src) return;

    style::Style s;

    // Name. Use the source object's name if available, otherwise the
    // lowest-free auto-name.
    if (!src->name.empty()) {
        s.header.name = "Style from " + src->name;
    } else {
        s.header.name = next_style_name();
    }
    // S93 m9: lands in the panel's currently-active category, mirroring
    // action_create_empty's seed (line 792). Pre-S93 this hardcoded ""
    // (Uncategorized), which surprised users who'd switched into a real
    // category and expected "from selection" to land where they were
    // looking. App-tier active means the user is browsing built-in
    // styles; new styles can only go to user tier, where "" is the
    // sensible neutral default.
    s.header.category = m_active_is_app_tier ? "" : m_active_category;

    // Fill: project SceneNode FillStyle to Paint losslessly.
    s.fill = color::to_paint(src->fill);

    // Stroke: project StrokeStyle (paint + width + cap + join + miter)
    // onto StrokeAppearance. opacity has no Style-level home in
    // Phase 1 (handled at the renderer).
    s.stroke.paint       = color::to_paint(src->stroke.paint);
    s.stroke.width       = src->stroke.width;
    s.stroke.cap         = src->stroke.cap;
    s.stroke.join        = src->stroke.join;
    s.stroke.miter_limit = src->stroke.miter;

    auto cmd = std::make_unique<AddStyleCommand>(
        m_library, std::move(s), std::string("Create style from selection"));
    cmd->execute();
    m_history->push(std::move(cmd));

    m_sig_library_changed.emit();
}

// ── action_create_category (S83 m4h v9) ──────────────────────────────────
//
// Hamburger → "New category…". Prompts for a category name, then
// creates a default empty style with that category set. Per design
// (b), categories aren't first-class persisted entities — they exist
// only as long as a style references them — so birthing a category
// means birthing a member.
//
// Empty input = cancel. Same-name-as-existing is fine (just adds
// another style to that category). Whitespace-only is treated as
// empty after trim.
//
// Auto-name: uses the same lowest-free "Style N" pattern as the
// other create actions. The user can rename inline afterwards.
void StylesPanel::action_create_category() {
    if (!m_library || !m_history) return;

    prompt_text("New category", "",
        [this](const std::string& cat_raw) {
            if (!m_library || !m_history) return;

            // Trim whitespace — treat all-whitespace as empty/cancel.
            std::string cat = cat_raw;
            while (!cat.empty() && (cat.front() == ' '  ||
                                    cat.front() == '\t' ||
                                    cat.front() == '\n' ||
                                    cat.front() == '\r')) {
                cat.erase(cat.begin());
            }
            while (!cat.empty() && (cat.back() == ' '  ||
                                    cat.back() == '\t' ||
                                    cat.back() == '\n' ||
                                    cat.back() == '\r')) {
                cat.pop_back();
            }
            if (cat.empty()) return;  // cancel

            style::Style s;
            s.header.name     = next_style_name();
            s.header.category = cat;

            auto cmd = std::make_unique<AddStyleCommand>(
                m_library, std::move(s),
                std::string("Create category"));
            cmd->execute();
            m_history->push(std::move(cmd));

            // S83 m4h v12: jump the dropdown to the newly-created
            // category so the user lands on it immediately. Without
            // this, the new category would exist but the dropdown
            // would still show whatever was selected before.
            m_active_category    = cat;
            m_active_is_app_tier = false;

            m_sig_library_changed.emit();
        });
}

// ── action_rename (S81 m4c-3, rewritten S85 cont-5) ──────────────────────
//
// Right-click → Rename… Opens a modal text prompt seeded with the
// current name. OK pushes UpdateStyleCommand carrying just the name
// change. Pre-cont-5 this began an inline rename on the row's name
// label; the chip-grid rewrite has no name label to host an inline
// Entry, so the rename surface is now a small modal — same pattern
// SwatchesPanel uses for swatch rename.
//
// Empty input cancels (no-op refresh, no command pushed). No-op on
// unchanged name. No-op on app-tier ids (the menu hides Rename for
// those; this is a defensive guard).
void StylesPanel::action_rename() {
    if (m_ctx_style_id.empty()) return;
    if (!m_library || !m_history) return;
    if (style::is_built_in(m_ctx_style_id)) {
        return;
    }

    const style::Style* current = m_library->find_style(m_ctx_style_id);
    if (!current) return;

    style::StyleId id      = m_ctx_style_id;
    std::string    initial = current->header.name;

    prompt_text("Rename style", initial,
        [this, id](const std::string& typed) {
            if (typed.empty()) return;            // cancel-as-empty
            if (!m_library || !m_history) return;
            const style::Style* now = m_library->find_style(id);
            if (!now) return;

            // Trim trailing whitespace (matches commit_inline_rename's
            // pre-cont-5 behaviour).
            std::string nm = typed;
            while (!nm.empty() && (nm.back() == ' '  || nm.back() == '\t' ||
                                    nm.back() == '\n' || nm.back() == '\r')) {
                nm.pop_back();
            }
            if (nm.empty()) return;
            if (now->header.name == nm) return;   // no-op

            style::Style before = *now;
            style::Style after  = before;
            after.header.name   = nm;

            auto cmd = std::make_unique<UpdateStyleCommand>(
                m_library, id, std::move(before), std::move(after),
                std::string("Rename style"));
            cmd->execute();
            m_history->push(std::move(cmd));

            m_sig_library_changed.emit();
        });
}

// ── action_duplicate (S81 m4c-3) ──────────────────────────────────────────
//
// Right-click → Duplicate. Works on both tiers: app rows duplicate
// to the user tier ("Duplicate to user tier" verb in the menu); user
// rows duplicate-in-place ("Duplicate"). Both go through
// duplicate_to_user, which takes a copy of the source and assigns a
// fresh stl_<uuid>.
//
// We don't use library->duplicate_to_user directly — that would mutate
// the library outside the command system, breaking undo. Instead we
// build the duplicate Style ourselves (copy + rename + clear id) and
// push AddStyleCommand. The command's execute() calls library->
// add_style(), which mints a new id for us.
//
// Naming: "<Original name> copy" — same disambiguation suffix the
// library's duplicate_to_user uses.
void StylesPanel::action_duplicate() {
    if (m_ctx_style_id.empty()) return;
    if (!m_library || !m_history) return;

    const style::Style* src = m_library->find_style(m_ctx_style_id);
    if (!src) return;  // race against removal between right-click and menu

    style::Style copy = *src;
    copy.header.id = {};   // empty → library mints a fresh stl_<uuid>
    copy.header.name = src->header.name + " copy";

    auto cmd = std::make_unique<AddStyleCommand>(
        m_library, std::move(copy), std::string("Duplicate style"));
    cmd->execute();
    m_history->push(std::move(cmd));

    m_sig_library_changed.emit();
}

// ── action_delete (S81 m4c-3) ─────────────────────────────────────────────
//
// Right-click → Delete. user-tier only (the menu hides Delete on app
// rows; defensive guard inside).
//
// m4c-3 deletes silently — no usage-check warning dialog. Bound objects
// keep their cached fill/stroke and a dangling bound_style id; the
// post-load walk in CurvzProject::load auto-clears danglers on the
// next document open. Usage-check dialog lands in m4c-4 / m5.
void StylesPanel::action_delete() {
    if (m_ctx_style_id.empty()) return;
    if (!m_library || !m_history) return;
    if (style::is_built_in(m_ctx_style_id)) return;

    const style::Style* src = m_library->find_style(m_ctx_style_id);
    if (!src) return;  // race against external removal

    style::Style snapshot = *src;  // full pre-remove value for undo

    auto cmd = std::make_unique<RemoveStyleCommand>(
        m_library, std::move(snapshot), std::string("Delete style"));
    cmd->execute();
    m_history->push(std::move(cmd));

    m_sig_library_changed.emit();
}

// ── action_set_category (S83 m4h+) ───────────────────────────────────────
//
// Right-click → Category…. Pops a small text prompt prefilled with the
// current category. On OK:
//   * Empty input  → clears category. Style returns to the
//                    "(uncategorised)" group in the panel rendering.
//                    This is intentional — categories are a panel
//                    grouping convention, and the empty-string slot
//                    is part of that convention (see StyleHeader doc).
//   * New input    → updates category. Library's signal_style_changed
//                    drives the panel refresh; the moved style appears
//                    under the new category header (creating the header
//                    if it didn't exist yet).
//   * Cancel / no change → silent no-op.
//
// Same UpdateStyleCommand path as inline rename — both are header-
// only edits and use the same before/after snapshot shape. Cross-doc
// propagation: m3d's walk listens on signal_style_changed but only
// re-materialises bindings when the appearance changes; a category-
// only edit is a no-op for the live walk, which is correct.
//
// Defensive guards: app-tier rows shouldn't reach this (menu hides
// the entry on built-ins), but we re-check is_built_in inside in
// case the action gets dispatched from an unexpected source.
void StylesPanel::action_set_category() {
    if (m_ctx_style_id.empty()) return;
    if (!m_library || !m_history) return;
    if (style::is_built_in(m_ctx_style_id)) return;

    const style::Style* current = m_library->find_style(m_ctx_style_id);
    if (!current) return;  // race against external removal

    // Capture id by value; current may be invalidated by the time
    // the prompt callback fires (a sibling refresh would tear the
    // pointer out from under us).
    style::StyleId id = m_ctx_style_id;
    std::string current_cat = current->header.category;
    auto existing = m_library->user_categories();

    prompt_category_picker("Set category", current_cat, existing,
        [this, id, current_cat](const std::string& new_cat) {
            if (!m_library || !m_history) return;

            // No-op if unchanged. Avoids a degenerate UpdateStyle
            // entry on the undo stack for a non-edit confirm.
            if (new_cat == current_cat) return;

            const style::Style* live = m_library->find_style(id);
            if (!live) return;  // race against external removal

            style::Style before = *live;
            style::Style after  = before;
            after.header.category = new_cat;

            auto cmd = std::make_unique<UpdateStyleCommand>(
                m_library, id, std::move(before), std::move(after),
                std::string("Set style category"));
            cmd->execute();
            m_history->push(std::move(cmd));

            m_sig_library_changed.emit();
            // Library's signal_style_changed → panel refresh handles
            // the visual update; no explicit refresh() needed.
        });
}

// ── action_rename_category (S83 m4h+) ────────────────────────────────────
//
// Right-click on a user-tier category header → Rename. Renames the
// category for every user-tier style currently in it. Atomic via
// CompositeCommand wrapping one UpdateStyleCommand per member —
// Ctrl+Z restores the original category for all members in one step.
//
// Empty new name = no-op. Treating empty as "clear category for all
// members" would silently move every style in the category back to
// "(uncategorised)" with no visual hint that's what was intended;
// the per-row "Set category…" path is the explicit way to do that.
//
// Same name = no-op. No degenerate undo entry.
//
// Race-safety: each UpdateStyleCommand is built from a fresh
// find_style() inside the loop, so a concurrent removal of one
// member doesn't poison the cascade — that member is silently
// skipped. (In practice the panel is single-threaded; this is
// defensive only.)
void StylesPanel::action_rename_category() {
    // S83 m4h v12: read active-category state set by the dropdown
    // chooser. Pre-v12 this came from m_ctx_category captured at
    // header right-click time; the headers are gone and the kebab
    // operates on the active dropdown selection. Read-only / empty
    // active is gated out by rebuild_kebab_menu hiding the item,
    // but defensive guards remain.
    if (m_active_category.empty()) return;     // (uncategorised) bucket
    if (m_active_is_app_tier)      return;     // read-only Built-in
    if (!m_library || !m_history)  return;

    std::string old_cat = m_active_category;

    prompt_text("Rename category", old_cat,
        [this, old_cat](const std::string& new_cat) {
            if (!m_library || !m_history) return;
            if (new_cat.empty()) return;        // ignore — see header
            if (new_cat == old_cat) return;     // no-op

            // Snapshot member ids first so the loop below isn't
            // walking a vector being mutated by signal-driven
            // refreshes from each UpdateStyleCommand::execute.
            std::vector<style::StyleId> member_ids;
            for (const style::Style* s :
                     m_library->user_styles_in_category(old_cat)) {
                if (s) member_ids.push_back(s->header.id);
            }
            if (member_ids.empty()) return;     // category vanished

            auto composite = std::make_unique<CompositeCommand>(
                std::string("Rename category"));
            for (const auto& id : member_ids) {
                const style::Style* live = m_library->find_style(id);
                if (!live) continue;            // race-skipped
                style::Style before = *live;
                style::Style after  = before;
                after.header.category = new_cat;
                composite->add(std::make_unique<UpdateStyleCommand>(
                    m_library, id,
                    std::move(before), std::move(after),
                    std::string("Rename category (member)")));
            }
            composite->execute();
            m_history->push(std::move(composite));

            // Active-category bookkeeping: the rename moved every
            // member from old_cat to new_cat. Move our active state
            // along with them so the dropdown lands on the renamed
            // category after refresh, not on whatever's at index 0.
            m_active_category = new_cat;

            m_sig_library_changed.emit();
            // Each UpdateStyleCommand fires signal_style_changed
            // which the panel listens to; refresh happens
            // transitively. No explicit refresh() needed.
        });
}

// ── action_delete_category (S83 m4h+) ────────────────────────────────────
//
// Right-click on a user-tier category header → Delete. Deletes every
// user-tier style in the category. Atomic via CompositeCommand
// wrapping one RemoveStyleCommand per member — Ctrl+Z restores
// the entire category in one step (all member styles re-added with
// their original ids and full Style values).
//
// No confirmation dialog (per Scott's call): undo is the safety net.
// The dangling-binding behaviour matches per-row delete — bound
// objects keep their cached fill/stroke and a dangling bound_style
// id; the post-load walk auto-clears danglers on next document open.
//
// App-tier "Built-in" header is gated out at the menu builder, but
// re-checked inside as a defensive guard.
void StylesPanel::action_delete_category() {
    // S83 m4h v12: read active-category state. Same semantics as
    // pre-v12 m_ctx_category capture, but driven by dropdown
    // selection instead of header right-click.
    if (m_active_category.empty()) return;     // (uncategorised) bucket
    if (m_active_is_app_tier)      return;     // read-only Built-in
    if (!m_library || !m_history)  return;

    // Snapshot member ids + their Style values for the cascade.
    // Capturing values up front (rather than re-fetching inside the
    // composite's execute) keeps the snapshot stable against any
    // concurrent panel refresh.
    std::vector<style::Style> snapshots;
    for (const style::Style* s :
             m_library->user_styles_in_category(m_active_category)) {
        if (s) snapshots.push_back(*s);
    }
    if (snapshots.empty()) return;

    auto composite = std::make_unique<CompositeCommand>(
        std::string("Delete category"));
    for (auto& snap : snapshots) {
        composite->add(std::make_unique<RemoveStyleCommand>(
            m_library, std::move(snap),
            std::string("Delete category (member)")));
    }
    composite->execute();
    m_history->push(std::move(composite));

    // The category we were viewing just lost all members and ceases
    // to exist (categories are emergent from style references).
    // Reset the active state so the next refresh picks the first
    // available — typically the first user category alphabetically,
    // or Built-in if no user categories remain.
    m_active_category.clear();
    m_active_is_app_tier = false;

    m_sig_library_changed.emit();
}

// ── prompt_text (S83 m4h+) ───────────────────────────────────────────────
//
// Small modal text prompt — same shape as SwatchesPanel::prompt_text.
// Empty input is passed through to on_ok unchanged; each caller decides
// what empty means. action_set_category treats empty as "clear the
// category" (intentional); future callers that need empty-as-cancel
// should guard at the top of their on_ok callback.
//
// Self-managed transient window. GTK cleans it up on close — no
// manual lifetime management needed.
void StylesPanel::prompt_text(const std::string& title,
                              const std::string& initial,
                              std::function<void(const std::string&)> on_ok) {
    auto* win = Gtk::make_managed<Gtk::Window>();
    win->set_title(title);
    win->set_modal(true);
    win->set_resizable(false);
    win->set_default_size(280, -1);

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    root->set_margin(12);
    win->set_child(*root);

    // s212 m2 — unregistered substrate Entry + 2 Buttons. Variant 1
    // of StylesPanel's two prompt_text shapes; identical shape to
    // SwatchesPanel::prompt_text closed in s211 m2. Per-prompt
    // transient with no script-addressability requirement.
    auto* entry = Gtk::make_managed<curvz::widgets::Entry>(
                       curvz::scripting::unregistered);
    entry->set_text(initial);
    entry->set_activates_default(true);
    root->append(*entry);

    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    btn_row->set_halign(Gtk::Align::END);
    auto* btn_cancel = Gtk::make_managed<curvz::widgets::Button>(
                            curvz::scripting::unregistered, "Cancel");
    auto* btn_ok     = Gtk::make_managed<curvz::widgets::Button>(
                            curvz::scripting::unregistered, "OK");
    btn_ok->add_css_class("suggested-action");
    btn_ok->set_receives_default(true);
    btn_row->append(*btn_cancel);
    btn_row->append(*btn_ok);
    root->append(*btn_row);

    if (auto* r = dynamic_cast<Gtk::Window*>(get_root())) {
        win->set_transient_for(*r);
        curvz::utils::apply_motif_class_from_parent(*win, *r);  // s117 m18 v2
    }

    auto do_ok = [win, entry, on_ok]() {
        // Empty passes through — caller decides semantics. See
        // SwatchesPanel v5 for the same idiom and rationale.
        std::string text = entry->get_text();
        on_ok(text);
        win->close();
    };

    btn_ok->signal_clicked().connect(do_ok);
    entry->signal_activate().connect(do_ok);
    btn_cancel->signal_clicked().connect([win]() { win->close(); });

    win->present();
    entry->grab_focus();
}

// ── prompt_category_picker (S93 m10) ──────────────────────────────────────
//
// Discoverable category picker shared by action_set_category and
// action_create_category. Pre-m10 both flowed through a free-text
// prompt, which made it impossible to know what categories already
// existed without dismissing the dialog and inspecting the panel.
//
// Layout:
//   ┌──────────────────────────────┐
//   │ Category: [▾ Uncategorised  ] │   ← dropdown
//   │           [ New name…        ] │   ← entry, hidden unless "+ New" picked
//   │                  [Cancel] [OK] │
//   └──────────────────────────────┘
//
// Dropdown contents (in order):
//   • "(uncategorised)"          → maps to ""
//   • each existing category      → maps to itself
//   • "+ New category…"           → reveals the entry; entry text is the result
//
// The initial argument selects the matching dropdown row. If `initial`
// doesn't match any existing category and isn't empty, it pre-populates
// the new-category entry with `initial` selected — so the user can
// see "this is a typo of X — pick X" or just edit it. (This case is
// rare in practice; categories are entered through this picker
// post-m10, but old free-text data is still possible.)
//
// "(uncategorised)" is shown as the first row even when no styles
// currently use that bucket, matching the panel's category dropdown
// (where the empty-string slot always exists). This is intentional:
// "no category" is a valid choice the user might want to make
// (move-out-of-category), not an absence.
void StylesPanel::prompt_category_picker(
        const std::string& title,
        const std::string& initial,
        const std::vector<std::string>& existing,
        std::function<void(const std::string&)> on_ok) {

    auto* win = Gtk::make_managed<Gtk::Window>();
    win->set_title(title);
    win->set_modal(true);
    win->set_resizable(false);
    win->set_default_size(300, -1);

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    root->set_margin(12);
    win->set_child(*root);

    // ── Build StringList model ────────────────────────────────────────────
    // Row 0: "(uncategorised)" → ""
    // Row 1..N: each existing category → itself
    // Row N+1: "+ New category…" → reveals entry
    auto strings = Gtk::StringList::create();
    strings->append("(uncategorised)");
    for (const auto& c : existing) {
        if (c.empty()) continue;  // defensive — uncategorised already in row 0
        strings->append(c);
    }
    const guint new_row_index = strings->get_n_items();
    strings->append("+ New category…");

    // s212 m2 — unregistered substrate DropDown. Variant 2's
    // category-picker dropdown rebuilt per-prompt with the running
    // list of categories; the StringList is fresh each invocation,
    // so there's no shared abbrev to register at. The DropDown
    // substrate gained an unregistered ctor for this milestone
    // (mirrors the s211 m2 SpinButton work — first-use site is the
    // StringList-model form, basic-options form doesn't get a tag
    // yet).
    auto* dropdown = Gtk::make_managed<curvz::widgets::DropDown>(
                          curvz::scripting::unregistered, strings);
    dropdown->set_hexpand(true);
    root->append(*dropdown);

    // Pre-select based on `initial`.
    guint initial_index = 0;  // uncategorised
    if (!initial.empty()) {
        Glib::ustring initial_u(initial);
        for (guint i = 1; i < new_row_index; ++i) {
            if (strings->get_string(i) == initial_u) {
                initial_index = i;
                break;
            }
        }
    }
    dropdown->set_selected(initial_index);

    // ── New-category entry (hidden unless "+ New" selected) ───────────────
    // s212 m2 — unregistered substrate Entry. Per-prompt transient,
    // visibility-driven by the dropdown selection. No script-
    // addressability requirement.
    auto* entry = Gtk::make_managed<curvz::widgets::Entry>(
                       curvz::scripting::unregistered);
    entry->set_placeholder_text("New category name");
    entry->set_activates_default(true);
    entry->set_visible(false);
    root->append(*entry);

    auto sync_entry_visibility = [dropdown, entry, new_row_index]() {
        const bool is_new = (dropdown->get_selected() == new_row_index);
        entry->set_visible(is_new);
        if (is_new) entry->grab_focus();
    };
    dropdown->property_selected().signal_changed().connect(sync_entry_visibility);

    // ── Buttons ──────────────────────────────────────────────────────────
    // s212 m2 — unregistered substrate Cancel/OK Buttons.
    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    btn_row->set_halign(Gtk::Align::END);
    auto* btn_cancel = Gtk::make_managed<curvz::widgets::Button>(
                            curvz::scripting::unregistered, "Cancel");
    auto* btn_ok     = Gtk::make_managed<curvz::widgets::Button>(
                            curvz::scripting::unregistered, "OK");
    btn_ok->add_css_class("suggested-action");
    btn_ok->set_receives_default(true);
    btn_row->append(*btn_cancel);
    btn_row->append(*btn_ok);
    root->append(*btn_row);

    if (auto* r = dynamic_cast<Gtk::Window*>(get_root())) {
        win->set_transient_for(*r);
        curvz::utils::apply_motif_class_from_parent(*win, *r);  // s117 m18 v2
    }

    auto do_ok = [win, dropdown, entry, on_ok, new_row_index]() {
        const guint sel = dropdown->get_selected();
        std::string result;
        if (sel == 0) {
            // (uncategorised)
            result = "";
        } else if (sel == new_row_index) {
            // + New category… → use entry text. Trim leading/trailing
            // whitespace; all-whitespace becomes empty (uncategorised).
            std::string raw = entry->get_text();
            auto is_ws = [](char c) {
                return c == ' ' || c == '\t' || c == '\n' || c == '\r';
            };
            size_t b = 0, e = raw.size();
            while (b < e && is_ws(raw[b])) ++b;
            while (e > b && is_ws(raw[e - 1])) --e;
            result = raw.substr(b, e - b);
        } else {
            // Existing category — read the model row directly so what
            // the user sees in the dropdown is exactly what they get.
            // (DropDown's get_selected_item returns a Glib::ObjectBase
            // that we'd need to downcast; reading from the model by
            // index is simpler and equivalent.)
            if (auto* sl = dynamic_cast<Gtk::StringList*>(
                    dropdown->get_model().get())) {
                result = sl->get_string(sel);
            }
        }
        on_ok(result);
        win->close();
    };

    btn_ok->signal_clicked().connect(do_ok);
    entry->signal_activate().connect(do_ok);
    btn_cancel->signal_clicked().connect([win]() { win->close(); });

    win->present();
}

// ── build_hint_text (S85 m4i-cont-3) ──────────────────────────────────────
//
// Compose the multi-line summary string shown in the single-click hint
// popover. Format:
//
//   <name>
//   <tier> · <category>
//   Fill:   <description>
//   Stroke: <description>
//
// Pure helper — only reads the supplied Style (and the panel's
// SwatchLibrary for swatch-name resolution). No widget access.
//
// "Description" formatting:
//   None         → "(none)"
//   CurrentColor → "(currentColor)"
//   Solid        → "#rrggbb (Region Name)"
//   SwatchRef    → "<Swatch Name> (#rrggbb)"  using the swatch's
//                  display name, falling back to the cached fallback
//                  colour's region name if the binding is dangling.
std::string StylesPanel::build_hint_text(const style::Style& s) const {
    auto color_to_hex = [](double r, double g, double b) {
        auto clamp = [](double v) {
            return v < 0.0 ? 0 : (v > 1.0 ? 255 : static_cast<int>(v * 255.0 + 0.5));
        };
        char buf[8];
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                      clamp(r), clamp(g), clamp(b));
        return std::string(buf);
    };

    auto describe_paint = [&](const color::Paint& p) -> std::string {
        return std::visit(color::Overloaded{
            [](const color::None&) -> std::string {
                return "(none)";
            },
            [](const color::CurrentColor&) -> std::string {
                return "(currentColor)";
            },
            [&](const color::Solid& sd) -> std::string {
                return color_to_hex(sd.color.r, sd.color.g, sd.color.b) +
                       " (" + color::region_name(
                           sd.color.r, sd.color.g, sd.color.b) + ")";
            },
            [&](const color::SwatchRef& sr) -> std::string {
                // S85 cont-3: resolve through m_swatch_library when
                // wired (always, post-cont-3) for the swatch's actual
                // current colour and display name. Falls back to the
                // SwatchRef's cached fallback colour + region-name
                // tagging when the lib isn't wired or the binding is
                // dangling — same shape pre-cont-3 always took.
                std::string name;
                color::Color resolved = sr.fallback;

                if (m_swatch_library) {
                    if (const color::Swatch* sw =
                            m_swatch_library->find_swatch(sr.id)) {
                        if (const auto* solid =
                                std::get_if<color::SolidSwatch>(sw)) {
                            resolved = solid->color;
                            name     = solid->header.name;
                        }
                    }
                }

                std::string hex = color_to_hex(
                    resolved.r, resolved.g, resolved.b);
                if (!name.empty()) {
                    return name + " (" + hex + ")";
                }
                // Lib-less or dangling — keep the pre-cont-3 fallback
                // shape so users at least see the colour.
                std::string region = color::region_name(
                    resolved.r, resolved.g, resolved.b);
                return region + " " + hex + " (swatch)";
            },
            [&](const color::Gradient& g) -> std::string {
                // S93 m1: tooltip describes the gradient. Format:
                // "Linear gradient (3 stops)" / "Radial gradient (2 stops)".
                // Could grow into a stop-by-stop dump if the tooltip
                // gets the room, but multi-line is already crowded.
                const char* kind = (g.kind == color::Gradient::Kind::Radial)
                                       ? "Radial gradient"
                                       : "Linear gradient";
                return std::string(kind) + " (" +
                       std::to_string(g.stops.size()) + " stops)";
            },
        }, p);
    };

    std::ostringstream os;

    // Line 1: name (or fallback)
    os << (s.header.name.empty() ? std::string("(unnamed)") : s.header.name);

    // Line 2: tier · category
    const bool app = style::is_built_in(s.header.id);
    os << "\n" << (app ? "Built-in" : "User");
    if (!s.header.category.empty()) {
        os << " · " << s.header.category;
    }

    // Line 3: fill
    os << "\nFill: " << describe_paint(s.fill);

    // Line 4: stroke (paint, then width / cap / join inline)
    os << "\nStroke: " << describe_paint(s.stroke.paint);
    if (!std::holds_alternative<color::None>(s.stroke.paint)) {
        // Width and cap/join only meaningful when stroke isn't None.
        os << ", " << s.stroke.width << "px";
        const char* cap_str =
            (s.stroke.cap == LineCap::Round)  ? "round" :
            (s.stroke.cap == LineCap::Square) ? "square" : "butt";
        const char* join_str =
            (s.stroke.join == LineJoin::Round) ? "round" :
            (s.stroke.join == LineJoin::Bevel) ? "bevel" : "miter";
        os << ", " << cap_str << " cap, " << join_str << " join";
    }

    return os.str();
}

// ── draw_style_chip (S85 cont-3) ──────────────────────────────────────────
//
// Single-chip preview of a style's appearance — rendered as the row's
// indicator widget in build_row. Layout:
//
//   * Outer ring of CONSTANT thickness (regardless of s.stroke.width):
//     the stroke colour. Stroke = None → no ring (chip is full inner).
//   * Inner area: fill colour. Fill = None → diagonal stripes.
//   * CurrentColor: small "C" glyph in the relevant area.
//   * SwatchRef: resolved through m_swatch_library to the swatch's
//     CURRENT colour, so the chip reflects swatch edits live.
//
// Chip metrics: 16px square with a 3px stroke ring and 1px outer
// border for definition on light themes. The ring sits inset from
// the outer border so very light strokes still read against a white
// chip background.
void StylesPanel::draw_style_chip(const Cairo::RefPtr<Cairo::Context>& cr,
                                   int w, int h,
                                   const style::Style& s,
                                   bool is_bound) const {
    // Resolve a Paint to (kind, color, optional gradient) for chip rendering.
    // Kind enum is local to this helper — outside-callers don't need it.
    //
    // S93 m8: gradient kinds added. The chip now renders gradient styles as
    // an actual gradient (linear ramp or radial dot) instead of a first-stop
    // solid placeholder. The grad_out pointer aliases into the Style being
    // drawn — safe because the lambda below holds style_copy by value for
    // the lifetime of the draw call.
    enum class Kind { Solid, None, CurrentColor, LinearGradient, RadialGradient };
    auto resolve = [this](const color::Paint& p,
                          Kind& kind_out,
                          color::Color& color_out,
                          const color::Gradient*& grad_out) {
        grad_out = nullptr;
        std::visit(color::Overloaded{
            [&](const color::None&) {
                kind_out  = Kind::None;
                color_out = color::Color::black();  // unused
            },
            [&](const color::CurrentColor&) {
                kind_out  = Kind::CurrentColor;
                color_out = color::Color::black();  // unused
            },
            [&](const color::Solid& sd) {
                kind_out  = Kind::Solid;
                color_out = sd.color;
            },
            [&](const color::SwatchRef& sr) {
                kind_out  = Kind::Solid;
                color_out = sr.fallback;
                if (m_swatch_library) {
                    if (const color::Swatch* sw =
                            m_swatch_library->find_swatch(sr.id)) {
                        if (const auto* solid =
                                std::get_if<color::SolidSwatch>(sw)) {
                            color_out = solid->color;
                        }
                    }
                }
            },
            [&](const color::Gradient& g) {
                // S93 m8: gradient is now a first-class chip kind. Capture
                // the gradient ptr so draw_gradient_in_path can build a
                // matching Cairo pattern. Falls back to Solid+black if the
                // gradient is malformed (no stops) — cheap defensive default.
                if (g.stops.empty()) {
                    kind_out  = Kind::Solid;
                    color_out = color::Color::black();
                    return;
                }
                kind_out  = (g.kind == color::Gradient::Kind::Radial)
                                ? Kind::RadialGradient
                                : Kind::LinearGradient;
                color_out = g.stops.front().color;  // unused for gradient kinds
                grad_out  = &g;
            },
        }, p);
    };

    Kind                      fill_kind = Kind::Solid;
    color::Color              fill_col;
    const color::Gradient*    fill_grad = nullptr;
    resolve(s.fill, fill_kind, fill_col, fill_grad);

    Kind                      stroke_kind = Kind::None;
    color::Color              stroke_col;
    const color::Gradient*    stroke_grad = nullptr;
    resolve(s.stroke.paint, stroke_kind, stroke_col, stroke_grad);

    // Chip layout. Constants in pixels — caller passes 24x24 today, but
    // the layout scales with whatever w/h come in.
    const double ring_thick = 3.0;   // constant; not s.stroke.width
    const double border     = 0.5;   // half-px outer line for crisp 1px

    // Outer-rounded square path.
    auto rounded = [&](double x0, double y0,
                       double x1, double y1, double rad) {
        cr->move_to(x0 + rad, y0);
        cr->line_to(x1 - rad, y0);
        cr->arc(x1 - rad, y0 + rad, rad, -M_PI / 2, 0);
        cr->line_to(x1, y1 - rad);
        cr->arc(x1 - rad, y1 - rad, rad, 0, M_PI / 2);
        cr->line_to(x0 + rad, y1);
        cr->arc(x0 + rad, y1 - rad, rad, M_PI / 2, M_PI);
        cr->line_to(x0, y0 + rad);
        cr->arc(x0 + rad, y0 + rad, rad, M_PI, 3 * M_PI / 2);
        cr->close_path();
    };

    const double r_outer = 2.5;
    const double r_inner = std::max(0.0, r_outer - ring_thick);

    // Diagonal-stripe pattern helper for None paints.
    auto stripe_in_path = [&](double x0, double y0, double x1, double y1) {
        cr->save();
        cr->clip();
        // White ground.
        cr->set_source_rgb(1.0, 1.0, 1.0);
        cr->rectangle(x0, y0, x1 - x0, y1 - y0);
        cr->fill();
        // Diagonal red (Affinity-style "no paint").
        cr->set_source_rgba(0.85, 0.15, 0.15, 0.85);
        cr->set_line_width(1.5);
        const double cx = (x0 + x1) * 0.5;
        const double cy = (y0 + y1) * 0.5;
        const double r  = std::max(x1 - x0, y1 - y0) * 0.55;
        cr->move_to(cx - r * 0.7, cy + r * 0.7);
        cr->line_to(cx + r * 0.7, cy - r * 0.7);
        cr->stroke();
        cr->restore();
    };

    // CurrentColor: tiny "C" glyph centred in the supplied rect.
    auto draw_C = [&](double x0, double y0, double x1, double y1) {
        cr->save();
        cr->clip();
        cr->set_source_rgb(0.92, 0.92, 0.92);
        cr->rectangle(x0, y0, x1 - x0, y1 - y0);
        cr->fill();
        cr->set_source_rgba(0.25, 0.25, 0.25, 0.9);
        // Use whatever default sans the toolkit picked; Cairo default
        // toy font is fine for a single glyph at this size.
        cr->set_font_size(std::max(6.0, (y1 - y0) * 0.85));
        cr->move_to(x0 + (x1 - x0) * 0.18, y0 + (y1 - y0) * 0.82);
        cr->show_text("C");
        cr->restore();
    };

    // S93 m8: Gradient fill helper. Cairo pattern from the Gradient's
    // stops + geometry, mapped from bbox-fraction space (the gradient's
    // native coordinate space) into the chip rect provided. The current
    // path has already been built by the caller; we clip to it, fill with
    // the pattern, then restore. Mirrors stripe_in_path's clip-and-fill
    // shape so call sites can swap helpers based on kind without other
    // changes.
    auto gradient_in_path = [&](double x0, double y0, double x1, double y1,
                                const color::Gradient* g) {
        if (!g || g->stops.empty()) return;
        cr->save();
        cr->clip();
        const double rw = x1 - x0;
        const double rh = y1 - y0;
        Cairo::RefPtr<Cairo::Gradient> pat;
        if (g->kind == color::Gradient::Kind::Radial) {
            // Radial: g_x1/y1 is focal, g_x2/y2 is centre, g_r is outer
            // radius (all in bbox-fraction space).
            const double fx = x0 + g->g_x1 * rw;
            const double fy = y0 + g->g_y1 * rh;
            const double cx = x0 + g->g_x2 * rw;
            const double cy = y0 + g->g_y2 * rh;
            // Radius is fraction of the chip's shorter dimension — a
            // reasonable visual proxy in a square-ish chip. Cairo's
            // create_radial takes inner radius (focal) and outer radius;
            // we use 0 for inner so the centre is a point source.
            const double rad = g->g_r * std::min(rw, rh);
            pat = Cairo::RadialGradient::create(fx, fy, 0.0, cx, cy, rad);
        } else {
            // Linear: start=(g_x1,g_y1), end=(g_x2,g_y2) in bbox-fraction.
            const double sx = x0 + g->g_x1 * rw;
            const double sy = y0 + g->g_y1 * rh;
            const double ex = x0 + g->g_x2 * rw;
            const double ey = y0 + g->g_y2 * rh;
            pat = Cairo::LinearGradient::create(sx, sy, ex, ey);
        }
        for (const auto& stop : g->stops) {
            pat->add_color_stop_rgba(stop.offset,
                                     stop.color.r, stop.color.g,
                                     stop.color.b, stop.color.a);
        }
        cr->set_source(pat);
        cr->rectangle(x0, y0, rw, rh);
        cr->fill();
        cr->restore();
    };

    // ── Stroke ring ───────────────────────────────────────────────────────
    if (stroke_kind == Kind::None) {
        // No ring; inner area is the full chip. Skip drawing a ring,
        // and the "inner area" rect below uses the outer bounds.
    } else {
        // Outer rounded path filled with stroke colour / gradient / glyph.
        rounded(border, border,
                w - border, h - border, r_outer);
        if (stroke_kind == Kind::CurrentColor) {
            draw_C(border, border, w - border, h - border);
        } else if (stroke_kind == Kind::LinearGradient ||
                   stroke_kind == Kind::RadialGradient) {
            // S93 m8: stroke gradient renders into the ring zone, same as
            // a solid would. The inner-fill stage will overdraw the centre.
            gradient_in_path(border, border, w - border, h - border,
                             stroke_grad);
        } else {
            cr->set_source_rgba(stroke_col.r, stroke_col.g,
                                stroke_col.b, stroke_col.a);
            cr->fill();
        }
    }

    // ── Inner fill area ───────────────────────────────────────────────────
    double ix0 = border, iy0 = border;
    double ix1 = w - border, iy1 = h - border;
    if (stroke_kind != Kind::None) {
        ix0 += ring_thick;
        iy0 += ring_thick;
        ix1 -= ring_thick;
        iy1 -= ring_thick;
    }

    rounded(ix0, iy0, ix1, iy1,
            stroke_kind == Kind::None ? r_outer : std::max(0.5, r_inner));

    if (fill_kind == Kind::Solid) {
        cr->set_source_rgba(fill_col.r, fill_col.g, fill_col.b, fill_col.a);
        cr->fill();
    } else if (fill_kind == Kind::None) {
        stripe_in_path(ix0, iy0, ix1, iy1);
    } else if (fill_kind == Kind::LinearGradient ||
               fill_kind == Kind::RadialGradient) {
        // S93 m8: inner-area gradient fill.
        gradient_in_path(ix0, iy0, ix1, iy1, fill_grad);
    } else { // CurrentColor
        draw_C(ix0, iy0, ix1, iy1);
    }

    // ── Outer 1px border for crisp definition ─────────────────────────────
    rounded(border, border, w - border, h - border, r_outer);
    cr->set_source_rgba(0.55, 0.55, 0.55, 0.9);
    cr->set_line_width(1.0);
    cr->stroke();

    // ── Active-binding ring (S85 cont-5) ──────────────────────────────────
    //
    // is_bound true → at least one node in current selection is bound
    // to this style. Draw a white-inner / black-outer double ring just
    // inside the outer border, mirroring SwatchesPanel's active-paint
    // ring. Sits ON TOP of fill / stroke so it's visible regardless of
    // chip colour.
    if (is_bound) {
        // White inner ring at 1.5px in from outer border.
        rounded(border + 1.5, border + 1.5,
                w - border - 1.5, h - border - 1.5,
                std::max(0.5, r_outer - 1.0));
        cr->set_source_rgba(1.0, 1.0, 1.0, 0.9);
        cr->set_line_width(1.2);
        cr->stroke();
        // Black outer ring overlapping the outer border.
        rounded(border + 0.5, border + 0.5,
                w - border - 0.5, h - border - 0.5, r_outer);
        cr->set_source_rgba(0.0, 0.0, 0.0, 0.9);
        cr->set_line_width(1.0);
        cr->stroke();
    }
}

// ── show_hint_for_row — RETIRED in S85 m4i-cont-4 ─────────────────────────
//
// Pre-cont-4 this opened a Gtk::Popover anchored to the clicked row to
// show the data-preview summary. The popover ate the double-click
// event on GTK4 (the popup activated before the second press
// registered), so cont-4 replaced the click-popover with a hover-
// tooltip set on the row itself via row->set_tooltip_text. The tooltip
// uses the same build_hint_text helper. The function is gone; call
// sites are gone.

}  // namespace Curvz
