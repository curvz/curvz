//
// SwatchesPanel.cpp — Phase 4 implementation.
//
// M2: skeleton + empty state.
// M3: functional `+` button (opens CurvzColorPicker), recents strip,
//     click-to-apply-fill / Alt-click-to-apply-stroke.
// M4: palette selector (DropDown + kebab menu), palette grid, implicit
//     "Default" palette auto-created on first add, palette management
//     actions (new/rename/delete/duplicate) via the kebab menu.
//

#include "SwatchesPanel.hpp"
#include "Canvas.hpp"
#include "CommandHistory.hpp"  // s220 m1a: AddSwatchCommand / RemoveSwatchCommand / EditSwatchCommand
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2
#include "curvz/widgets/DropDown.hpp"  // s208 m5 — substrate palette dropdown
#include "curvz/widgets/Button.hpp"    // s211 m2 — unregistered substrate Button for prompt_text dialog
#include "curvz/widgets/Entry.hpp"     // s211 m2 — unregistered substrate Entry for prompt_text dialog

#include <cairomm/context.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <type_traits>
#include <variant>
#include <gdkmm/enums.h>  // Gdk::ModifierType
#include <giomm/file.h>
#include <giomm/liststore.h>
#include <giomm/simpleaction.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/entry.h>
#include <gtkmm/filedialog.h>     // S101 m3: import/export
#include <gtkmm/filefilter.h>     // S101 m3: import/export
#include <gtkmm/gestureclick.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/separator.h>          // S83 m4h v14: recents/palette divider
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>

namespace Curvz {

// ── Chip geometry constants ────────────────────────────────────────────────
static constexpr int    CHIP_SIZE          = 24;
static constexpr double CHIP_CORNER_RADIUS = 3.0;
static constexpr double CHIP_BORDER_GREY   = 0.55;
static constexpr double CHIP_BORDER_WIDTH  = 1.0;
static constexpr double CHIP_BORDER_LUMA_THRESHOLD = 0.85;

// Name used for the auto-created first palette. Not special in the
// library — a user can rename or delete it like any other. The string
// is the only marker; there's no "is-default" flag.
static constexpr const char* DEFAULT_PALETTE_NAME = "Default";

// ── SwatchesPanel ──────────────────────────────────────────────────────────

SwatchesPanel::SwatchesPanel() : Gtk::Box(Gtk::Orientation::VERTICAL) {
    curvz::utils::set_name(*this, "sw", "swatches_panel_root");
    add_css_class("swatches-panel");

    // ── Header ────────────────────────────────────────────────────────────
    m_header.set_margin_start(8);
    m_header.set_margin_end(4);
    m_header.set_margin_top(5);
    m_header.set_margin_bottom(3);

    // S83 m4h v13: title kept as a member but not appended. The
    // make_section wrapper that hosts this panel already labels it
    // ("Swatches"); pre-v13 the inner title was a "  " placeholder
    // that filled the hexpand slot. v13 moves the palette dropdown
    // up here to take the hexpand slot directly, alongside the kebab.
    m_title.set_text("  ");
    m_title.set_xalign(0.0f);
    m_title.set_hexpand(true);
    m_title.add_css_class("panel-title");
    // m_header.append(m_title);  // intentionally not appended in v13

    // S83 m4h v10: kebab (view-more-symbolic) instead of hamburger
    // (open-menu-symbolic) — see StylesPanel for full rationale.
    // GNOME HIG reserves hamburger for app-level primary menus in
    // the header bar; per-panel contextual menus belong on a kebab.
    m_btn_add.set_icon_name("view-more-symbolic");
    m_btn_add.set_has_frame(false);
    m_btn_add.set_tooltip_text("Swatch options");
    m_btn_add.set_focus_on_click(false);
    curvz::utils::set_name(m_btn_add, "sw_kb", "swatches_panel_kebab_btn");
    m_header.append(m_btn_add);

    append(m_header);

    // ── Body ──────────────────────────────────────────────────────────────
    m_body.set_margin_start(8);
    m_body.set_margin_end(8);
    m_body.set_margin_top(2);
    m_body.set_margin_bottom(8);
    m_body.set_spacing(6);
    append(m_body);

    // Labels reused across rebuilds.
    m_empty_state.set_text("No swatches yet. Open the menu to add a colour.");
    m_empty_state.set_xalign(0.0f);
    m_empty_state.set_wrap(true);
    m_empty_state.add_css_class("dim-label");
    m_empty_state.set_margin_top(4);
    m_empty_state.set_margin_bottom(4);

    m_recents_label.set_text("Recents");
    m_recents_label.set_xalign(0.0f);
    m_recents_label.add_css_class("dim-label");
    m_recents_label.add_css_class("caption");

    // S101 m4: Search entry. Filters chips visible in recents +
    // active palette by name / hex / "r,g,b" substring (case-
    // insensitive). Invalidates filter on every keystroke; FlowBox
    // filter_funcs re-run and hide non-matching chips.
    m_search.set_placeholder_text("Search swatches…");
    m_search.set_hexpand(true);
    m_search.set_max_width_chars(0);
    curvz::utils::set_name(m_search, "sw_srch", "swatches_panel_search_entry");
    m_search.signal_search_changed().connect([this]() {
        for (auto* flow : m_active_flows) {
            if (flow) flow->invalidate_filter();
        }
    });

    // ── Palette action group (M4) ─────────────────────────────────────────
    //
    // Actions owned by the panel. Built once; the kebab menu model
    // references them via "swatches.<action>". Wired so they fire
    // regardless of how the kebab is rebuilt during refresh().
    m_palette_actions = Gio::SimpleActionGroup::create();
    // S83 m4h v8: new-swatch was previously a direct button click;
    // moved into the hamburger menu so all panel-level actions
    // share one entry point.
    m_palette_actions->add_action("new-swatch",
        sigc::mem_fun(*this, &SwatchesPanel::on_add_clicked));
    m_palette_actions->add_action("new-palette",
        sigc::mem_fun(*this, &SwatchesPanel::on_new_palette));
    m_palette_actions->add_action("rename-palette",
        sigc::mem_fun(*this, &SwatchesPanel::on_rename_palette));
    m_palette_actions->add_action("delete-palette",
        sigc::mem_fun(*this, &SwatchesPanel::on_delete_palette));
    m_palette_actions->add_action("duplicate-palette",
        sigc::mem_fun(*this, &SwatchesPanel::on_duplicate_palette));

    // S101 m3: palette interchange actions.
    //   load-bundled   — parameterised by palette stem (string), e.g.
    //                    "vivid"; loads from /com/curvz/app/palettes/<stem>.gpl
    //   import-palette — opens a Gtk::FileDialog for a user .gpl
    //   export-palette — opens a save dialog for the active palette
    m_palette_actions->add_action_with_parameter("load-bundled",
        Glib::VariantType("s"),
        [this](const Glib::VariantBase& param) {
            auto str_v = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(param);
            on_load_bundled(str_v.get());
        });
    m_palette_actions->add_action_with_parameter("load-user",
        Glib::VariantType("s"),
        [this](const Glib::VariantBase& param) {
            auto str_v = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(param);
            on_load_user(str_v.get());
        });
    m_palette_actions->add_action("import-palette",
        sigc::mem_fun(*this, &SwatchesPanel::on_import_palette));
    m_palette_actions->add_action("export-palette",
        sigc::mem_fun(*this, &SwatchesPanel::on_export_palette));

    // Chip context-menu actions (M5). Live on the same action group so
    // they're reachable via the same "swatches." prefix.
    m_palette_actions->add_action("ctx-edit",
        sigc::mem_fun(*this, &SwatchesPanel::on_ctx_edit_color));
    m_palette_actions->add_action("ctx-rename",
        sigc::mem_fun(*this, &SwatchesPanel::on_ctx_rename_swatch));
    m_palette_actions->add_action("ctx-duplicate",
        sigc::mem_fun(*this, &SwatchesPanel::on_ctx_duplicate_swatch));
    m_palette_actions->add_action("ctx-delete",
        sigc::mem_fun(*this, &SwatchesPanel::on_ctx_delete_swatch));

    // Parameterised action: "ctx-move" takes a string = target palette id.
    // Bound here so the kebab/context-menu rebuilds don't need to
    // re-register.
    m_palette_actions->add_action_with_parameter("ctx-move",
        Glib::VariantType("s"),
        [this](const Glib::VariantBase& param) {
            auto str_v = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(param);
            std::string target = str_v.get();
            on_ctx_move_to_palette(target);
        });

    insert_action_group("swatches", m_palette_actions);

    // ── Hidden context-menu helper (S72) ──────────────────────────────────
    //
    // See the comment on m_ctx_button in the header. We need this in
    // the widget tree so its popover (set per right-click) inherits
    // the panel's "swatches" action group via the muxer walk, and so
    // popover_present() runs during MenuButton's size_allocate.
    //
    // We keep the button visible (so it gets measured/allocated and
    // therefore has well-defined coordinates for set_pointing_to and
    // translate_coordinates), but render-invisible: zero-sized,
    // opacity 0, no frame, no focus. A 0x0 visible widget contributes
    // nothing to layout in either dimension, so the panel's existing
    // visual layout is unchanged.
    m_ctx_button.set_size_request(0, 0);
    m_ctx_button.set_opacity(0.0);
    m_ctx_button.set_can_focus(false);
    m_ctx_button.set_focus_on_click(false);
    m_ctx_button.set_has_frame(false);
    append(m_ctx_button);

    refresh();
}

void SwatchesPanel::set_library(color::SwatchLibrary* lib) {
    // Drop any previous hooks before swapping libraries.
    // Safe on default-constructed connections.
    m_library_paint_changed_conn.disconnect();
    m_library_swatch_changed_conn.disconnect();
    m_library_swatch_added_conn.disconnect();    // s220 m1a hotfix
    m_library_swatch_removed_conn.disconnect();  // s220 m1a hotfix

    m_library = lib;

    // s207 m1: ColorPickerPopover is now a singleton — no per-panel
    // attach. The earlier lazy-attach-on-set_library block is gone; the
    // shared popover's ensure_attached() runs on first open() instead.

    // Phase 5 M3: hook the library's paint-changed signal. Every time
    // set_paint writes through the choke point, we refresh so the
    // active-paint ring, recents strip, and any indexed-chip indicators
    // stay current. The signal gives us (SceneNode*, slot) — we don't
    // need either parameter here (refresh reads the whole selection
    // state), but future milestones may want the granular info.
    if (m_library) {
        m_library_paint_changed_conn =
            m_library->signal_paint_changed().connect(
                [this](SceneNode* /*obj*/, color::PaintSlot /*slot*/) {
                    refresh();
                });

        // S72: hook signal_swatch_changed so the chip grid refreshes
        // when a swatch's colour changes from any source — popover
        // edits, EditSwatchCommand undo/redo, future scripted edits.
        // Without this, undo would update bound objects via Canvas's
        // own listener but leave the chip showing the stale colour.
        m_library_swatch_changed_conn =
            m_library->signal_swatch_changed().connect(
                [this](const color::SwatchId& /*id*/) {
                    refresh();
                });

        // s220 m1a hotfix — hook signal_swatch_added and
        // signal_swatch_removed so the chip grid refreshes when CRUD
        // happens through any path (panel, command undo/redo, future
        // scripted CRUD). schedule_save also needs to fire because
        // CRUD changes the persisted swatches.json. Mirrors
        // m_sig_library_changed.emit() that the panel's panel-driven
        // sites already do; here we route it through the same channel
        // so MainWindow's schedule_save wiring picks it up.
        m_library_swatch_added_conn =
            m_library->signal_swatch_added().connect(
                [this](const color::SwatchId& /*id*/) {
                    m_sig_library_changed.emit();
                    refresh();
                });
        m_library_swatch_removed_conn =
            m_library->signal_swatch_removed().connect(
                [this](const color::SwatchId& /*id*/) {
                    m_sig_library_changed.emit();
                    m_sig_inspector_refresh_needed.emit();
                    refresh();
                });
    }

    refresh();
}

void SwatchesPanel::set_canvas(Canvas* canvas) {
    // Drop any previous canvas hook. Bare disconnect is safe on a default-
    // constructed sigc::connection (no-op if never connected).
    m_canvas_selection_conn.disconnect();

    m_canvas = canvas;

    if (m_canvas) {
        // Hook selection change → refresh. Keeps the active-paint ring
        // in sync when the user selects a new object, deselects, etc.
        //
        // This handles one class of ring-stale: the selection moved.
        // The other class — the selection's paint changed — is handled
        // by set_library's signal_paint_changed hookup (Phase 5 M3),
        // but ONLY for writes that went through SwatchLibrary::set_paint.
        // Paths that still write FillStyle directly (toolbar, inspector,
        // eyedropper, macro playback) leave the ring stale until we
        // reroute them through set_paint in later milestones.
        m_canvas_selection_conn =
            m_canvas->signal_selection_changed().connect(
                [this](SceneNode* /*primary*/) { refresh(); });
    }
}

// ── Helpers ────────────────────────────────────────────────────────────────

color::PaletteId SwatchesPanel::ensure_active_palette() {
    if (!m_library) return {};

    // Library has at least one palette and an active_palette is set and
    // valid? Nothing to do.
    const auto& active = m_library->active_palette();
    if (!active.empty() && m_library->find_palette(active)) {
        return active;
    }

    // Active is unset or stale. Pick the first palette if any exist.
    auto ids = m_library->all_palette_ids();
    if (!ids.empty()) {
        m_library->set_active_palette(ids.front());
        return ids.front();
    }

    // No palettes at all — create "Default" and make it active.
    color::Palette p;
    p.name   = DEFAULT_PALETTE_NAME;
    p.source = "user";
    auto new_id = m_library->add_palette(std::move(p));
    if (new_id.empty()) {
        LOG_ERROR("SwatchesPanel::ensure_active_palette: add_palette failed");
        return {};
    }
    m_library->set_active_palette(new_id);
    return new_id;
}

Gtk::FlowBox* SwatchesPanel::build_chip_flow(
    const std::vector<color::SwatchId>& ids,
    const color::PaletteId& palette_context) {
    auto* flow = Gtk::make_managed<Gtk::FlowBox>();
    flow->set_orientation(Gtk::Orientation::HORIZONTAL);
    flow->set_homogeneous(true);
    flow->set_min_children_per_line(1);
    flow->set_max_children_per_line(16);
    flow->set_row_spacing(4);
    flow->set_column_spacing(4);
    flow->set_selection_mode(Gtk::SelectionMode::NONE);

    int chip_idx = 0;
    for (const auto& id : ids) {
        auto* chip = make_chip(id, palette_context);
        if (chip) {
            flow->append(*chip);
            // FlowBox auto-wraps the appended widget in a
            // FlowBoxChild. We track the running index ourselves to
            // pull out the just-created child via get_child_at_index
            // and record its swatch id. The filter func reads this
            // map to know which swatch a child represents.
            if (auto* fbc = flow->get_child_at_index(chip_idx)) {
                m_chip_swatch[fbc] = id;
            }
            ++chip_idx;
        }
    }

    // S101 m4: filter func — visible iff the search query is empty,
    // or the swatch's name / hex / rgb-triple contains the query as
    // a case-insensitive substring. The lambda captures `this` so
    // it can read m_search and m_chip_swatch on each invocation;
    // FlowBox calls this whenever invalidate_filter() runs, which
    // is on every search-changed signal.
    flow->set_filter_func([this](Gtk::FlowBoxChild* child) -> bool {
        if (!child) return true;
        std::string q = m_search.get_text();
        if (q.empty()) return true;
        // Case-fold the query in place — std::tolower is fine for
        // ASCII palette names; user-typed Unicode is uncommon here
        // and Glib::ustring lowercase is heavier than we need.
        for (char& c : q) c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));

        auto it = m_chip_swatch.find(child);
        if (it == m_chip_swatch.end()) return true;
        if (!m_library) return true;
        const color::Swatch* s = m_library->find_swatch(it->second);
        if (!s) return true;
        return swatch_matches_query(*s, q);
    });

    m_active_flows.push_back(flow);
    return flow;
}

// ── Refresh ────────────────────────────────────────────────────────────────

void SwatchesPanel::refresh() {
    // Compute the active-paint swatch first so chips drawn this cycle
    // reflect it. Safe with null canvas or library.
    update_active_paint();

    // Disconnect any stale dropdown signal before we tear down — prevents
    // the selection-change from firing on destroy.
    m_dropdown_sel_conn.disconnect();
    m_palette_dropdown = nullptr;
    m_palette_order.clear();

    // S83 m4h v13: tear down any previous dropdown in m_header.
    // The kebab (m_btn_add) lives in m_header for the panel's
    // lifetime — skip it. Anything else (the previous dropdown or
    // its placeholder) gets removed.
    //
    // s208 m5: prior to the unparent below, walk children and
    // force-unregister any Scriptable substrate. This is the s199 m1
    // discipline (lifted from PropertiesPanel::do_clear's s191 m7
    // inline lambda) — necessary now that the palette dropdown is a
    // curvz::widgets::DropDown registered under `sw_pal`. Without
    // this, GTK4's deferred destruction would leave the old dropdown
    // registered when the next refresh() tries to register the new
    // one under the same name, throwing at the registry.
    //
    // m_btn_add is the long-lived kebab; we skip it for unparent but
    // its substrate identity (if any) is not at risk of duplicate
    // registration because it isn't being reconstructed.
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

    // Tear down current body. Force-unregister substrate Scriptables
    // first (s208 m5; today the body has no substrate widgets, but the
    // walk is null-safe and idempotent, and any future migration of
    // chip-row / palette-entry widgets will be covered without further
    // teardown changes).
    for (Gtk::Widget* c = m_body.get_first_child(); c;
         c = c->get_next_sibling()) {
        curvz::utils::force_unregister_subtree(c);
    }
    while (auto* child = m_body.get_first_child()) {
        m_body.remove(*child);
    }

    // S101 m4: clear per-refresh chip tracking. The flows themselves
    // are managed widgets owned by m_body; teardown above already
    // unparented them, but our pointer cache and child→swatch map
    // need explicit clearing so stale entries don't linger into the
    // next pass.
    m_active_flows.clear();
    m_chip_swatch.clear();

    // S101 m3 fix2: build the kebab menu unconditionally before any
    // early-return below. Without this, an empty library leaves the
    // kebab popover-less and GTK auto-disables the button — exactly
    // the case where the empty-state label tells the user "Open the
    // menu to add a colour" and the user finds nothing to open.
    // The bundled-palettes section in particular must be reachable
    // when the library is empty: that's the primary path users take
    // to populate a fresh project.
    rebuild_kebab_menu();

    if (!m_library) {
        m_body.append(m_empty_state);
        return;
    }

    if (m_library->empty()) {
        m_body.append(m_empty_state);
        return;
    }

    // S101 m4: search entry above all chip flows. Only meaningful
    // when at least one chip will render; we conditionally append it
    // below after we've decided whether there's anything to show.
    // (Defer the append until we know recents-or-palette has content.)

    // --- Recents strip -----------------------------------------------------
    //
    // S83 m4h v14: track whether recents was rendered so the separator
    // below can decide whether to draw. Without the flag the separator
    // would dangle when recents is empty.
    const auto& recents = m_library->recents();
    const bool have_recents = !recents.empty();

    // S101 m4: search entry appears at the top of m_body whenever
    // there's at least one chip flow to filter. Cheaper to compute
    // active_will_render up front than to recheck mid-build. If
    // neither recents nor active will render, we skip the search
    // entirely — empty grids don't need a search box.
    color::PaletteId active_for_sep = m_library->active_palette();
    const bool active_will_render = !active_for_sep.empty()
        && m_library->find_palette(active_for_sep) != nullptr
        && !m_library->find_palette(active_for_sep)->swatches.empty();
    if (have_recents || active_will_render) {
        m_body.append(m_search);
    }

    if (have_recents) {
        m_body.append(m_recents_label);
        m_body.append(*build_chip_flow(
            std::vector<color::SwatchId>(recents.begin(), recents.end()),
            /*palette_context=*/""));
    }

    // S83 m4h v14: thin horizontal separator between Recents and the
    // active palette grid. Only drawn when recents has content AND
    // there's an active palette to render below — avoids a dangling
    // separator at the bottom of the panel in degenerate states.
    if (have_recents && active_will_render) {
        auto* sep = Gtk::make_managed<Gtk::Separator>(
            Gtk::Orientation::HORIZONTAL);
        sep->set_margin_top(6);
        sep->set_margin_bottom(4);
        sep->set_margin_start(2);
        sep->set_margin_end(2);
        m_body.append(*sep);
    }

    // --- Palette selector row ---------------------------------------------
    //
    // Row: [DropDown (palette names) ................] [⋯ kebab]
    //
    // If the library has zero palettes, we could either hide the row or
    // show a single-item dropdown with a dim state. The first-add flow
    // creates Default, so zero-palettes-with-swatches is a transient
    // state that only happens if a user somehow deleted every palette.
    // We handle it by showing the kebab (so "New palette…" is reachable)
    // and skipping the DropDown.

    // S83 m4h v13: Selector row is gone. Pre-v13 the dropdown lived
    // inside selector_row at the top of m_body; v13 promotes it into
    // m_header alongside the kebab. The "no palettes" placeholder
    // similarly goes into m_header.
    //
    // Header layout after the prepend below:
    //   [ DropDown (palette names) ............... ] [⋮]

    auto palette_ids = m_library->all_palette_ids();

    if (!palette_ids.empty()) {
        // Build StringList model from palette names, matching order.
        auto string_list = Gtk::StringList::create({});
        m_palette_order = palette_ids;
        for (const auto& pid : palette_ids) {
            const color::Palette* p = m_library->find_palette(pid);
            string_list->append(p ? p->name : pid);
        }

        // s208 m5: substrate. The refresh path teardown above
        // (`force_unregister_subtree` walk on m_header and m_body children)
        // synchronously unregisters this dropdown's substrate identity
        // before the unparent runs, so the next refresh()'s registration
        // under `sw_pal` doesn't collide with the about-to-die predecessor.
        // Same shape as PropertiesPanel::do_clear's s191 m7 discipline,
        // lifted into curvz::utils::force_unregister_subtree in s199 m1.
        auto* drop = Gtk::make_managed<curvz::widgets::DropDown>(
            "sw_pal", string_list);
        curvz::utils::set_name(drop, "sw_pal", "swatches_panel_palette_dd");
        drop->set_hexpand(true);
        // S88: unified inspector dropdown styling. `flat` removes GTK4's
        // always-on dropdown border/background; `prop-dropdown` aligns
        // the font size with the rest of the inspector (11px). Was
        // `flat caption` — caption's GNOME-stylesheet size was visibly
        // larger than the inspector's own dropdowns (Units/Mode etc.).
        drop->add_css_class("flat");
        drop->add_css_class("prop-dropdown");
        m_palette_dropdown = drop;

        // Pre-select the active palette.
        color::PaletteId active = m_library->active_palette();
        if (!active.empty()) {
            auto it = std::find(palette_ids.begin(), palette_ids.end(), active);
            if (it != palette_ids.end()) {
                m_syncing_dropdown = true;
                drop->set_selected(
                    static_cast<guint>(std::distance(palette_ids.begin(), it)));
                m_syncing_dropdown = false;
            }
        }

        m_dropdown_sel_conn = drop->property_selected().signal_changed().connect(
            [this]() {
                if (m_syncing_dropdown || !m_library || !m_palette_dropdown) return;
                guint idx = m_palette_dropdown->get_selected();
                if (idx == GTK_INVALID_LIST_POSITION) return;
                if (idx >= m_palette_order.size()) return;
                color::PaletteId chosen = m_palette_order[idx];
                if (chosen == m_library->active_palette()) return;
                m_library->set_active_palette(chosen);
                m_sig_library_changed.emit();
                refresh();
            });

        // S83 m4h v13: dropdown prepended to m_header so layout reads
        // [dropdown ▾] [⋮]. Pre-v13 it was appended to a body-side
        // selector_row. Kebab-on-right is the Curvz-wide convention.
        m_header.prepend(*drop);
    } else {
        auto* placeholder = Gtk::make_managed<Gtk::Label>("No palettes");
        placeholder->add_css_class("dim-label");
        placeholder->set_hexpand(true);
        placeholder->set_xalign(0.0f);
        m_header.prepend(*placeholder);
    }

    // S83 m4h v8: palette options live in the header kebab
    // (m_btn_add). The actual menu build is in rebuild_kebab_menu(),
    // called near the top of refresh() so it runs unconditionally
    // regardless of library state. See header comment on
    // rebuild_kebab_menu for the full layout.

    // S83 m4h v13: selector_row no longer appended to m_body.
    // (The local was eliminated above.)

    // --- Palette grid ------------------------------------------------------
    //
    // Only render if there's an active palette with members.

    color::PaletteId active = m_library->active_palette();
    if (!active.empty()) {
        const color::Palette* p = m_library->find_palette(active);
        if (p && !p->swatches.empty()) {
            m_body.append(*build_chip_flow(p->swatches, active));
        } else if (p) {
            // Active palette exists but is empty.
            auto* empty_label = Gtk::make_managed<Gtk::Label>(
                "This palette is empty.");
            empty_label->set_xalign(0.0f);
            empty_label->add_css_class("dim-label");
            empty_label->set_margin_top(4);
            m_body.append(*empty_label);
        }
    }
}

// ── Chip construction ──────────────────────────────────────────────────────

Gtk::Widget* SwatchesPanel::make_chip(const color::SwatchId& id,
                                     const color::PaletteId& palette_context) {
    if (!m_library) return nullptr;
    const color::Swatch* sw = m_library->find_swatch(id);
    if (!sw) return nullptr;

    const auto* solid = std::get_if<color::SolidSwatch>(sw);
    if (!solid) return nullptr;

    const color::Color color = solid->color;
    const std::string  name  = solid->header.name;
    const bool is_active_paint = (!m_active_paint_swatch_id.empty() &&
                                   m_active_paint_swatch_id == id);

    auto* area = Gtk::make_managed<Gtk::DrawingArea>();
    area->set_content_width(CHIP_SIZE);
    area->set_content_height(CHIP_SIZE);
    area->set_draw_func(
        [this, color, is_active_paint]
        (const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            draw_chip(cr, w, h, color, is_active_paint);
        });

    // Gesture handling note (M5b fix):
    //   Previously we wrapped the DrawingArea in a Gtk::Button and added
    //   GestureClick controllers to the button. That failed to deliver
    //   left-click events — Gtk::Button has its own internal click
    //   gesture, and attaching additional GestureClicks in parallel
    //   causes sequence-claim conflicts that eat events silently.
    //
    //   Solution: use a plain DrawingArea (no built-in gestures) and
    //   attach both left-click and right-click GestureClicks directly.
    //   The DrawingArea itself does the painting. We style it with the
    //   `swatch-chip` CSS class so users can theme the hover state.
    area->add_css_class("swatch-chip");
    area->set_cursor(Gdk::Cursor::create("pointer"));

    std::string hex = color::to_hex(color);
    std::string tip = name.empty() ? hex : (name + " · " + hex);
    area->set_tooltip_text(tip);

    // --- Left-click: apply ------------------------------------------------
    auto gesture = Gtk::GestureClick::create();
    gesture->set_button(1);
    gesture->signal_released().connect(
        [this, id, gesture](int /*n_press*/, double /*x*/, double /*y*/) {
            unsigned mods = static_cast<unsigned>(gesture->get_current_event_state());
            apply_swatch(id, mods);
        });
    area->add_controller(gesture);

    // --- Right-click: context menu (M5, S72 dispatch fix) -----------------
    //
    // Pre-S72 we did:
    //   pop = make_managed<PopoverMenu>(menu);
    //   pop->set_parent(*this);
    //   pop->popup();
    //
    // The menu rendered fine but item clicks didn't dispatch. Per
    // GTK maintainer guidance and the s72_m1/m2 repros, popovers
    // parented to plain widgets need gtk_popover_present() called
    // during the parent's size_allocate to fully wire the action
    // muxer. MenuButton does this in its own size_allocate; bare
    // set_parent + popup() does not. So instead we route the
    // popover through a hidden MenuButton (m_ctx_button) that lives
    // permanently in the panel's widget tree.
    //
    // Per right-click we build a fresh PopoverMenu, set it on the
    // helper MenuButton (which reparents and triggers the layout
    // path that wires dispatch), set_pointing_to in MenuButton-local
    // coordinates so the popover anchors at the click point, and
    // call MenuButton::popup() programmatically.
    //
    // No need to manage popover lifetime by hand — set_popover()
    // dissociates and (effectively) replaces the previous popover,
    // and the make_managed instances will be cleaned up by their
    // ownership chains in the normal GTK lifecycle.
    auto rc_gesture = Gtk::GestureClick::create();
    rc_gesture->set_button(3);
    rc_gesture->signal_released().connect(
        [this, id, palette_context, area]
        (int /*n_press*/, double x, double y) {
            m_ctx_swatch_id  = id;
            m_ctx_palette_id = palette_context;

            auto menu = Gio::Menu::create();
            menu->append("Edit Colour…", "swatches.ctx-edit");
            menu->append("Rename…",      "swatches.ctx-rename");
            menu->append("Duplicate",    "swatches.ctx-duplicate");

            // Move / Add to palette submenu — lists all palettes other
            // than the current context (for palette chips) or all
            // palettes (for recents-only chips).
            auto palette_ids = m_library ? m_library->all_palette_ids()
                                         : std::vector<color::PaletteId>{};
            if (!palette_ids.empty()) {
                auto sub = Gio::Menu::create();
                for (std::size_t i = 0; i < palette_ids.size(); ++i) {
                    const auto& pid = palette_ids[i];
                    if (pid == palette_context) continue;
                    const color::Palette* p = m_library->find_palette(pid);
                    if (!p) continue;
                    // Use a parameterised action so one action handles
                    // all palette targets. The string parameter is the
                    // target palette id.
                    sub->append(p->name,
                        Glib::ustring("swatches.ctx-move('") + pid + "')");
                }
                // Only attach the submenu if it has entries.
                if (sub->get_n_items() > 0) {
                    const char* label = palette_context.empty()
                        ? "Add to Palette" : "Move to Palette";
                    menu->append_submenu(label, sub);
                }
            }

            menu->append("Delete Swatch", "swatches.ctx-delete");

            // Wrap in a fresh PopoverMenu and route through the helper
            // MenuButton. set_popover replaces any previous popover.
            auto* pop = Gtk::make_managed<Gtk::PopoverMenu>(menu);
            pop->set_has_arrow(false);
            m_ctx_button.set_popover(*pop);

            // Position the popover at the click point. set_pointing_to
            // takes a rect in the popover's parent's coord space —
            // here, m_ctx_button's. Translate chip → m_ctx_button so
            // the rect is meaningful regardless of where the helper
            // ends up sitting in the box. Since m_ctx_button is
            // visible (zero-sized + opacity 0), it's allocated and
            // translate_coordinates produces a valid frame.
            double bx = x, by = y;
            if (!area->translate_coordinates(m_ctx_button, x, y, bx, by)) {
                // Defensive fallback: anchor at chip-local 0,0 (which
                // should never trigger in practice).
                bx = 0; by = 0;
            }
            Gdk::Rectangle rect(static_cast<int>(bx),
                                static_cast<int>(by), 1, 1);
            pop->set_pointing_to(rect);

            m_ctx_button.popup();
        });
    area->add_controller(rc_gesture);

    return area;
}

// ── Chip drawing ───────────────────────────────────────────────────────────

void SwatchesPanel::draw_chip(const Cairo::RefPtr<Cairo::Context>& cr,
                              int w, int h, const color::Color& color,
                              bool is_active_paint) {
    const double r = CHIP_CORNER_RADIUS;
    const double x = 0.5;
    const double y = 0.5;
    const double ww = static_cast<double>(w) - 1.0;
    const double hh = static_cast<double>(h) - 1.0;

    auto rounded_rect = [&](double x0, double y0, double x1, double y1, double rad) {
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

    rounded_rect(x, y, x + ww, y + hh, r);
    cr->set_source_rgba(color.r, color.g, color.b, color.a);
    cr->fill_preserve();

    double luma = 0.299 * color.r + 0.587 * color.g + 0.114 * color.b;
    if (luma > CHIP_BORDER_LUMA_THRESHOLD || color.a < 0.95) {
        cr->set_source_rgba(CHIP_BORDER_GREY, CHIP_BORDER_GREY,
                            CHIP_BORDER_GREY, 1.0);
        cr->set_line_width(CHIP_BORDER_WIDTH);
        cr->stroke();
    } else {
        cr->begin_new_path();
    }

    if (is_active_paint) {
        rounded_rect(x + 1.5, y + 1.5, x + ww - 1.5, y + hh - 1.5, r - 1);
        cr->set_source_rgba(1.0, 1.0, 1.0, 0.9);
        cr->set_line_width(1.2);
        cr->stroke();
        rounded_rect(x + 0.5, y + 0.5, x + ww - 0.5, y + hh - 0.5, r);
        cr->set_source_rgba(0.0, 0.0, 0.0, 0.9);
        cr->set_line_width(1.0);
        cr->stroke();
    }
}

// ── Handlers ───────────────────────────────────────────────────────────────

void SwatchesPanel::on_add_clicked() {
    if (!m_library) {
        LOG_WARN("SwatchesPanel::on_add_clicked: library unattached");
        return;
    }

    m_editing_swatch_id.clear();

    // S73: capture session state. Create flow — first on_changed
    // creates the swatch and stores its id in m_session_created_id;
    // Esc (committed=false in on_closed) removes it.
    m_session_created_id.clear();

    // s207 m1: ColorPickerPopover is the app-wide singleton; route
    // through shared(). The widget-anchor overload auto-attaches on
    // first call. last_committed_name() reads from the same instance.
    auto& popover = ColorPickerPopover::shared();
    popover.open(
        m_btn_add,
        color::Color::black(),
        /*with_alpha=*/false,
        // on_changed — first invocation creates the swatch + adds to
        // active palette + touches recents. Subsequent invocations
        // update in place. On Esc, the picker self-reverts and
        // re-emits this signal at black; for an already-created
        // swatch that's an in-place update to black, then on_closed
        // removes the swatch. (For never-dragged Esc, this lambda
        // creates a black swatch which on_closed promptly removes.)
        [this](const color::Color& c) {
            if (!m_library) return;

            if (m_editing_swatch_id.empty()) {
                // First invocation — create the swatch and add to active
                // palette. Ensure a palette exists (implicit Default).
                color::PaletteId pid = ensure_active_palette();

                color::SolidSwatch s;
                s.header.id.clear();
                s.header.name.clear();
                s.color = c;
                color::SwatchId new_id = m_library->add_swatch(color::Swatch{s});
                if (new_id.empty()) {
                    LOG_ERROR("SwatchesPanel::on_add_clicked: add_swatch failed");
                    return;
                }
                m_editing_swatch_id  = new_id;
                m_session_created_id = new_id;  // remember for Esc-cleanup
                m_library->touch_recent(new_id);
                if (!pid.empty()) {
                    m_library->add_to_palette(pid, new_id);
                }
            } else {
                const color::Swatch* existing =
                    m_library->find_swatch(m_editing_swatch_id);
                if (!existing) {
                    m_editing_swatch_id.clear();
                    return;
                }
                color::SolidSwatch updated =
                    std::get<color::SolidSwatch>(*existing);
                updated.color = c;
                m_library->update_swatch(m_editing_swatch_id,
                                         color::Swatch{updated});
            }
            m_sig_library_changed.emit();
            // S83 m4h v4: live recolour propagates via library's
            // signal_swatch_changed → live_recolour_walk (Canvas hookup),
            // which updates bound objects' caches. But the inspector's
            // bound annotation shows the derived region_name fallback,
            // and that fallback depends on the colour — so it goes
            // stale on recolour. Trigger an inspector refresh.
            m_sig_inspector_refresh_needed.emit();
            refresh();
        },
        // on_closed — fires once after popdown.
        //   committed=false → Esc. Remove the just-created swatch (if
        //                     one was created during the session).
        //                     Don't push undo: the session is being
        //                     cancelled, there's nothing to undo.
        //   committed=true  → Return / pick-recent / outside-click.
        //                     Leave the swatch in place. s220 m1a:
        //                     push an "already-added" AddSwatchCommand
        //                     so Ctrl-Z removes the commit-create. The
        //                     swatch is already in the library (the
        //                     mid-session create did the add directly,
        //                     because the popover live-previews each
        //                     colour change); we use the already_added
        //                     factory so push() doesn't double-add and
        //                     undo correctly removes by the live id.
        //
        // S85 cont-2: on commit, also stamp the popover's last_
        // committed_name onto the just-created swatch via rename_swatch.
        // The popover's auto-name-at-birth fallback (returns derived
        // region name when entry blank) means we always have a non-
        // empty name to set, satisfying the no-empty-names invariant.
        [this](bool committed) {
            if (!committed && m_library && !m_session_created_id.empty()) {
                m_library->remove_swatch(m_session_created_id);
                m_sig_library_changed.emit();
                refresh();
            } else if (committed && m_library &&
                       !m_session_created_id.empty()) {
                const std::string& nm =
                    ColorPickerPopover::shared().last_committed_name();
                if (!nm.empty()) {
                    m_library->rename_swatch(m_session_created_id, nm);
                }
                // s220 m1a — push the AddSwatchCommand so the commit
                // is undoable. Snapshot the live swatch (post-rename)
                // and use the already_added factory: the swatch is
                // already in the library from the mid-session direct
                // add, so first execute() is a no-op. Undo removes by
                // the captured id; redo re-adds.
                if (m_history) {
                    const color::Swatch* live =
                        m_library->find_swatch(m_session_created_id);
                    if (live) {
                        auto cmd = AddSwatchCommand::already_added(
                            m_library, *live, "Create swatch");
                        cmd->execute();   // no-op for already_added
                        m_history->push(std::move(cmd));
                    }
                }
                m_sig_library_changed.emit();
                refresh();
            }
            m_editing_swatch_id.clear();
            m_session_created_id.clear();
        },
        // S85 cont-2: enable the inline name field. Empty initial_name
        // means the entry shows only the derived-name placeholder until
        // the user types — typical create flow.
        ColorPickerPopover::NameField{ /*enabled=*/true,
                                       /*initial_name=*/"" });
}

void SwatchesPanel::apply_swatch(const color::SwatchId& id,
                                 unsigned gtk_modifiers) {
    if (!m_library) return;

    const color::Swatch* sw = m_library->find_swatch(id);
    if (!sw) return;

    const auto* solid = std::get_if<color::SolidSwatch>(sw);
    if (!solid) return;

    const auto mods = static_cast<Gdk::ModifierType>(gtk_modifiers);
    const bool alt = (mods & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};

    // Phase 5 M3: route through Canvas::apply_swatch_to_selection so the
    // write lands as a SwatchRef in the library's reverse usage index.
    // The earlier raw-hex path (apply_fill_to_selection / apply_stroke_to_
    // selection) bypassed the index — it's still available on Canvas for
    // other call sites (toolbar, macro replay) but the swatch panel
    // should never use it again: a click here is, by definition, a
    // binding, not a one-shot colour change.
    if (m_canvas) {
        color::PaintSlot slot = alt ? color::PaintSlot::Stroke
                                    : color::PaintSlot::Fill;
        m_canvas->apply_swatch_to_selection(id, slot);
        // S83 m4h v4: signal_inspector_refresh_needed (renamed from v3's
        // signal_paint_applied; same semantics, broadened consumer set
        // — see header). Canvas's signal_document_changed only triggers
        // sync_selection (bbox spinners only), so the inspector needs
        // its own refresh path.
        m_sig_inspector_refresh_needed.emit();
    }

    m_library->touch_recent(id);
    m_sig_library_changed.emit();
    refresh();
}

// ── Palette actions (M4) ──────────────────────────────────────────────────

void SwatchesPanel::prompt_text(const std::string& title,
                                const std::string& initial,
                                std::function<void(const std::string&)> on_ok) {
    // Self-managed transient window. Created per-prompt — the interaction
    // is short-lived and GTK cleans it up when closed. Not a modal in the
    // native sense; we just make it transient-for the panel's root so it
    // floats above the main window.
    auto* win = Gtk::make_managed<Gtk::Window>();
    win->set_title(title);
    win->set_modal(true);
    win->set_resizable(false);
    win->set_default_size(280, -1);

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    root->set_margin(12);
    win->set_child(*root);

    // s211 m2 — unregistered substrate Entry + 2 Buttons. `prompt_text`
    // is called per-prompt (new-palette name, swatch-name edit, rename
    // palette) — each invocation builds a fresh transient Gtk::Window
    // with three widgets inside. Shared abbrevs (`pop_pt_ent`,
    // `pop_pt_cnc`, `pop_pt_ok`) would collide across simultaneous
    // prompts. The interaction surface is fully local (Enter or OK
    // commits, Cancel closes, focus-loss does nothing) — no script
    // addressability needed. The transient window itself stays as a
    // raw Gtk::Window (substrate doesn't cover windows; not in scope).
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

    // Parent the transient to the root window so it positions correctly.
    if (auto* r = dynamic_cast<Gtk::Window*>(get_root())) {
        win->set_transient_for(*r);
        curvz::utils::apply_motif_class_from_parent(*win, *r);  // s117 m18 v2
    }

    auto do_ok = [win, entry, on_ok]() {
        // S83 m4h v5: prompt_text now ALWAYS fires on_ok when the user
        // confirms (Enter or OK button). Each caller decides what
        // empty-input means — "cancel" for palette names (rejected),
        // "clear user name, use auto-derived" for swatch names. The
        // previous "if (!name.empty())" gate baked one policy in here
        // and silently swallowed empty-confirms, which broke the
        // swatch UX rule "every swatch always has a non-empty name —
        // user-supplied OR auto-derived" (the user couldn't get back
        // to the auto-derived name once they'd named a swatch).
        std::string name = entry->get_text();
        on_ok(name);
        win->close();
    };

    btn_ok->signal_clicked().connect(do_ok);
    entry->signal_activate().connect(do_ok);
    btn_cancel->signal_clicked().connect([win]() { win->close(); });

    win->present();
    entry->grab_focus();
}

void SwatchesPanel::on_new_palette() {
    if (!m_library) return;

    prompt_text("New palette", "Untitled", [this](const std::string& name) {
        // S83 m4h v5: prompt_text now passes empty input through —
        // each caller validates. A blank palette name is a cancel
        // (palettes need names; there's no derived fallback for them).
        if (name.empty()) return;
        color::Palette p;
        p.name   = name;
        p.source = "user";
        auto new_id = m_library->add_palette(std::move(p));
        if (new_id.empty()) {
            LOG_ERROR("SwatchesPanel::on_new_palette: add_palette failed");
            return;
        }
        m_library->set_active_palette(new_id);
        m_sig_library_changed.emit();
        refresh();
    });
}

void SwatchesPanel::on_rename_palette() {
    if (!m_library) return;
    color::PaletteId active = m_library->active_palette();
    if (active.empty()) return;
    const color::Palette* p = m_library->find_palette(active);
    if (!p) return;

    std::string current = p->name;
    prompt_text("Rename palette", current,
        [this, active](const std::string& name) {
            // S83 m4h v5: blank palette rename is a cancel — see
            // on_new_palette comment for rationale.
            if (name.empty()) return;
            if (!m_library) return;
            m_library->rename_palette(active, name);
            m_sig_library_changed.emit();
            refresh();
        });
}

void SwatchesPanel::on_delete_palette() {
    if (!m_library) return;
    color::PaletteId active = m_library->active_palette();
    if (active.empty()) return;

    // Straight delete — no confirmation. Swatches stay in the library;
    // only the palette (view) is removed. Restoring = create a new
    // palette and drag swatches back in (M5 territory).
    m_library->remove_palette(active);
    m_sig_library_changed.emit();
    refresh();
}

void SwatchesPanel::on_duplicate_palette() {
    if (!m_library) return;
    color::PaletteId active = m_library->active_palette();
    if (active.empty()) return;

    auto new_id = m_library->duplicate_palette(active, /*new_name=*/"");
    if (new_id.empty()) {
        LOG_ERROR("SwatchesPanel::on_duplicate_palette: duplicate failed");
        return;
    }
    m_library->set_active_palette(new_id);
    m_sig_library_changed.emit();
    refresh();
}

// ── S101 m3: palette interchange ───────────────────────────────────────────
//
// All three handlers route format work through color::palette_io and
// keep the SwatchesPanel logic to: dialog plumbing, library-change
// signal, refresh, set-active. The format module never sees a GTK
// type; this layer never sees a GplPalette field.

// ── S101 m4: search predicate ─────────────────────────────────────────────
//
// The contract documented in the header: case-insensitive substring
// match against name, hex ("#rrggbb[aa]"), and "r,g,b" decimal triple.
// Empty query short-circuits to true and never reaches here — caller
// guards. The query argument is already lowercased by the caller.
//
// Why all three axes are matched: users searching "ff" expect to find
// swatches with bright reds in their hex; users searching "255" expect
// to find anything saturated; users searching "red" expect named hits.
// One predicate satisfies all three intents without mode-switching.

bool SwatchesPanel::swatch_matches_query(const color::Swatch& s,
                                         const std::string& q) const {
    auto contains_lc = [](const std::string& haystack,
                          const std::string& needle) -> bool {
        if (needle.empty()) return true;
        // Lowercase haystack on the fly into a temporary; q is
        // already lowered by the caller. Avoids allocating per
        // chip-render in the common no-search path.
        std::string h;
        h.reserve(haystack.size());
        for (char c : haystack) h.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
        return h.find(needle) != std::string::npos;
    };

    const color::SwatchHeader& hdr = color::swatch_header(s);

    // Always test the name. Cheap.
    if (contains_lc(hdr.name, q)) return true;

    // Solid-only paths: derive hex + RGB triple. Future variant kinds
    // (gradients) skip this section — name-only match for them is
    // intentional; gradient swatches don't have a single hex to test.
    return std::visit([&](const auto& v) -> bool {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, color::SolidSwatch>) {
            // Hex: lowercase by construction (color::to_hex emits
            // lowercase). Substring match catches "ff", "#ff8000",
            // and the prefix-fragments users type.
            if (contains_lc(color::to_hex(v.color), q)) return true;
            // Decimal triple "r,g,b" — eight-bit channels. Matches
            // "255" → any 255-channel; "0,0,0" → black. No spaces:
            // users typing "255 0 0" with spaces won't match, but
            // that's an acceptable simplification for v1.
            char rgb[24];
            std::snprintf(rgb, sizeof(rgb), "%u,%u,%u",
                          static_cast<unsigned>(color::channel_to_u8(v.color.r)),
                          static_cast<unsigned>(color::channel_to_u8(v.color.g)),
                          static_cast<unsigned>(color::channel_to_u8(v.color.b)));
            if (contains_lc(rgb, q)) return true;
        }
        return false;
    }, s);
}

void SwatchesPanel::rebuild_kebab_menu() {
    // S101 m3 layout (top-to-bottom):
    //   New swatch
    //   New palette…
    //   ──────────
    //   Load palette ▶  (submenu with bundled palettes + Import…)
    //   Export current palette…   (only when active palette has swatches)
    //   ──────────
    //   Rename / Duplicate / Delete palette  (only when palette exists)
    //
    // Called unconditionally at the top of refresh() — runs even for
    // null / empty libraries so the bundled-palettes path stays
    // reachable as the empty-state escape hatch.

    auto menu = Gio::Menu::create();

    // Section 1: creation primitives. Always present.
    auto sec_create = Gio::Menu::create();
    sec_create->append("New swatch",   "swatches.new-swatch");
    sec_create->append("New palette…", "swatches.new-palette");
    menu->append_section(sec_create);

    // Section 2: interchange (load + export). Bundled palettes live
    // in a submenu populated from the gresource; Import… sits at the
    // bottom of the same submenu so it reads as "load from somewhere".
    auto sec_interchange = Gio::Menu::create();
    {
        auto load_sub = Gio::Menu::create();
        auto load_bundled_sec = Gio::Menu::create();
        for (const auto& stem : color::palette_io::enumerate_bundled()) {
            // Display name: capitalise the first letter of the stem.
            // Bundled stems are short ASCII tokens (greyscale, vivid,
            // …) so this is sufficient — anything fancier (i18n,
            // multi-word names) belongs in metadata, not in stems.
            std::string label = stem;
            if (!label.empty()) label[0] = static_cast<char>(
                std::toupper(static_cast<unsigned char>(label[0])));
            // Action with string parameter: GAction format string is
            //   "swatches.load-bundled('<stem>')"
            std::string action =
                "swatches.load-bundled('" + stem + "')";
            load_bundled_sec->append(label, action);
        }
        load_sub->append_section(load_bundled_sec);

        // S101 m5: user palettes from ~/.config/curvz/palettes/. Only
        // append the section if at least one user palette exists; an
        // empty user folder shouldn't add a blank separator. User
        // stems can collide with bundled (a user "vivid" overriding
        // the shipped Vivid) — that's intentional, the menu shows
        // both rows and the user can pick whichever they want.
        // Display labels for user palettes use the GplPalette name
        // when present, else the file stem; the kebab is rebuilt on
        // every refresh so renames pick up next time.
        auto user_stems = color::palette_io::enumerate_user();
        if (!user_stems.empty()) {
            auto load_user_sec = Gio::Menu::create();
            for (const auto& stem : user_stems) {
                // Read the file just to peek at its display name.
                // Cheap — small files, infrequent menu rebuilds —
                // and avoids stem-vs-name confusion in the UI. If
                // the file fails to parse, fall back to the stem.
                std::string label = stem;
                if (auto p = color::palette_io::load_user(stem)) {
                    if (!p->name.empty()) label = p->name;
                }
                std::string action =
                    "swatches.load-user('" + stem + "')";
                load_user_sec->append(label, action);
            }
            load_sub->append_section(load_user_sec);
        }

        auto load_import_sec = Gio::Menu::create();
        load_import_sec->append("Import…", "swatches.import-palette");
        load_sub->append_section(load_import_sec);

        sec_interchange->append_submenu("Load palette", load_sub);

        // Export entry — only meaningful when there's a non-empty
        // active palette. We can't easily disable a Gio::Menu item
        // (PopoverMenu reads the action's enabled state), so just
        // omit the entry when it would be a no-op. Library-null safe:
        // the guard short-circuits before touching m_library.
        if (m_library) {
            const color::PaletteId active_pid = m_library->active_palette();
            if (!active_pid.empty()) {
                const color::Palette* ap = m_library->find_palette(active_pid);
                if (ap && !ap->swatches.empty()) {
                    sec_interchange->append("Export current palette…",
                                            "swatches.export-palette");
                }
            }
        }
    }
    menu->append_section(sec_interchange);

    // Section 3: palette management (rename / duplicate / delete).
    // Only relevant when at least one palette exists in the library.
    if (m_library && !m_library->all_palette_ids().empty()) {
        auto sec_manage = Gio::Menu::create();
        sec_manage->append("Rename palette…",   "swatches.rename-palette");
        sec_manage->append("Duplicate palette", "swatches.duplicate-palette");
        sec_manage->append("Delete palette",    "swatches.delete-palette");
        menu->append_section(sec_manage);
    }

    auto* popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
    m_btn_add.set_popover(*popover);
}

void SwatchesPanel::on_load_bundled(const std::string& stem) {
    if (!m_library) return;
    if (stem.empty()) return;

    auto src = color::palette_io::load_bundled(stem);
    if (!src) {
        LOG_WARN("SwatchesPanel::on_load_bundled: '{}' load failed", stem);
        return;
    }

    // Use the GplPalette's display name; fall back to a Title-Cased
    // stem if the file forgot to set one (shouldn't happen for
    // bundled, but defensive).
    std::string display = src->name;
    if (display.empty()) {
        display = stem;
        if (!display.empty()) display[0] = static_cast<char>(
            std::toupper(static_cast<unsigned char>(display[0])));
    }

    color::PaletteId new_pid =
        color::palette_io::import_gpl_into_library(*m_library, *src, display);
    if (new_pid.empty()) {
        LOG_WARN("SwatchesPanel::on_load_bundled: import failed for '{}'",
                 stem);
        return;
    }
    // Make the loaded palette the active one — matches "user clicks
    // Vivid → Vivid is what they see in the grid".
    m_library->set_active_palette(new_pid);
    m_sig_library_changed.emit();
    m_sig_inspector_refresh_needed.emit();
    refresh();
}

void SwatchesPanel::on_load_user(const std::string& stem) {
    if (!m_library) return;
    if (stem.empty()) return;

    auto src = color::palette_io::load_user(stem);
    if (!src) {
        LOG_WARN("SwatchesPanel::on_load_user: '{}' load failed", stem);
        return;
    }

    // Display name: GplPalette::name when the file has one, else the
    // stem. Same fallback chain as bundled — keeps user-named files
    // self-describing while still working for files that omit the
    // Name: header.
    std::string display = src->name;
    if (display.empty()) display = stem;

    color::PaletteId new_pid =
        color::palette_io::import_gpl_into_library(*m_library, *src, display);
    if (new_pid.empty()) {
        LOG_WARN("SwatchesPanel::on_load_user: import failed for '{}'",
                 stem);
        return;
    }
    m_library->set_active_palette(new_pid);
    m_sig_library_changed.emit();
    m_sig_inspector_refresh_needed.emit();
    refresh();
}

void SwatchesPanel::on_import_palette() {
    if (!m_library) return;

    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Import palette");

    // Filter to .gpl. We allow All Files as a fallback because users
    // sometimes have palettes named with no extension or .pal — let
    // them pick the file and let parse_gpl decide.
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto gpl_filter = Gtk::FileFilter::create();
    gpl_filter->set_name("GIMP palettes (*.gpl)");
    gpl_filter->add_pattern("*.gpl");
    filters->append(gpl_filter);
    auto all_filter = Gtk::FileFilter::create();
    all_filter->set_name("All files");
    all_filter->add_pattern("*");
    filters->append(all_filter);
    dialog->set_filters(filters);
    dialog->set_default_filter(gpl_filter);

    auto* root = dynamic_cast<Gtk::Window*>(get_root());
    if (!root) {
        LOG_WARN("SwatchesPanel::on_import_palette: no root window");
        return;
    }

    dialog->open(*root,
        [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto file = dialog->open_finish(result);
                if (!file) return;
                if (!m_library) return;
                std::string path = file->get_path();

                color::GplPalette parsed;
                if (!color::read_gpl_file(path, parsed)) {
                    LOG_WARN("Import: read_gpl_file failed for '{}'", path);
                    return;
                }

                // Display name: GplPalette::name if present, else file
                // basename minus .gpl. We never end up with a nameless
                // palette — UX rule.
                std::string display = parsed.name;
                if (display.empty()) {
                    auto basename = file->get_basename();
                    display = basename.empty() ? std::string("Imported")
                                               : basename;
                    auto dot = display.rfind('.');
                    if (dot != std::string::npos) display.erase(dot);
                    if (display.empty()) display = "Imported";
                }

                color::PaletteId new_pid =
                    color::palette_io::import_gpl_into_library(
                        *m_library, parsed, display);
                if (new_pid.empty()) {
                    LOG_WARN("Import: import_gpl_into_library failed for '{}'",
                             path);
                    return;
                }
                m_library->set_active_palette(new_pid);
                m_sig_library_changed.emit();
                m_sig_inspector_refresh_needed.emit();
                refresh();
            } catch (const Glib::Error&) {
                // User cancelled or dialog error — silent.
            }
        });
}

void SwatchesPanel::on_export_palette() {
    if (!m_library) return;
    color::PaletteId active = m_library->active_palette();
    if (active.empty()) return;
    const color::Palette* ap = m_library->find_palette(active);
    if (!ap || ap->swatches.empty()) return;

    auto built = color::palette_io::export_palette_as_gpl(*m_library, active);
    if (!built) {
        LOG_WARN("Export: export_palette_as_gpl produced nothing");
        return;
    }
    // Snapshot the GplPalette into the lambda — the user may take a
    // long time picking the destination, and the library could mutate
    // while the dialog is open. Capturing the value frozen at click
    // time is what they intended to export.
    color::GplPalette snapshot = std::move(*built);

    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Export palette");
    // Suggest a sensible filename: <palette-name>.gpl, with spaces
    // replaced by dashes for a cleaner default.
    std::string suggested = ap->name;
    for (char& c : suggested) if (c == ' ') c = '-';
    if (suggested.empty()) suggested = "palette";
    dialog->set_initial_name(suggested + ".gpl");

    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto gpl_filter = Gtk::FileFilter::create();
    gpl_filter->set_name("GIMP palettes (*.gpl)");
    gpl_filter->add_pattern("*.gpl");
    filters->append(gpl_filter);
    dialog->set_filters(filters);
    dialog->set_default_filter(gpl_filter);

    // S101 m5: default to ~/.config/curvz/palettes/ so user exports
    // land where the kebab's "Load palette → User palettes" section
    // looks for them. Create the directory if missing — first export
    // bootstraps it. Fail-soft: if mkdir fails (permissions, disk
    // full, …) we skip the initial-folder hint and the dialog opens
    // wherever GTK defaults to.
    namespace fs = std::filesystem;
    const std::string user_dir = color::palette_io::user_palettes_dir();
    std::error_code mkdir_ec;
    fs::create_directories(user_dir, mkdir_ec);
    if (!mkdir_ec) {
        dialog->set_initial_folder(Gio::File::create_for_path(user_dir));
    } else {
        LOG_WARN("SwatchesPanel::on_export_palette: cannot create '{}': "
                 "{} (skipping initial-folder hint)",
                 user_dir, mkdir_ec.message());
    }

    auto* root = dynamic_cast<Gtk::Window*>(get_root());
    if (!root) {
        LOG_WARN("SwatchesPanel::on_export_palette: no root window");
        return;
    }

    dialog->save(*root,
        [this, dialog, snapshot](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto file = dialog->save_finish(result);
                if (!file) return;
                std::string path = file->get_path();
                // Append .gpl if the user didn't.
                if (path.size() < 4 ||
                    path.compare(path.size() - 4, 4, ".gpl") != 0) {
                    path += ".gpl";
                }
                if (!color::write_gpl_file(path, snapshot)) {
                    LOG_WARN("Export: write_gpl_file failed for '{}'", path);
                    return;
                }
                // S101 m5: rebuild the kebab so a fresh export to the
                // user palettes folder appears in the Load palette
                // submenu without waiting for the next refresh.
                // refresh() also re-renders chips, but that's a no-op
                // for the library state (which export doesn't mutate).
                refresh();
            } catch (const Glib::Error&) {
                // User cancelled or dialog error — silent.
            }
        });
}

// ── Active-paint computation (M5) ──────────────────────────────────────────

void SwatchesPanel::update_active_paint() {
    m_active_paint_swatch_id.clear();

    if (!m_library || !m_canvas) return;

    const auto& sel = m_canvas->selection();
    if (sel.empty()) return;

    // Use the first selected node as the reference. Multi-select with
    // mixed fills would ideally show no ring (ambiguous), but consulting
    // just the primary is simpler and matches the "inspector target"
    // semantics already used elsewhere in Curvz.
    SceneNode* primary = sel.front();
    if (!primary) return;

    if (primary->fill.type != FillStyle::Type::Solid) return;

    color::Color c{primary->fill.r, primary->fill.g,
                   primary->fill.b, primary->fill.a};
    m_active_paint_swatch_id = m_library->find_solid_by_color(c);
}

// ── Edit popover (reused by + button and ctx-edit) ─────────────────────────

void SwatchesPanel::open_edit_popover_for(const color::SwatchId& id,
                                          Gtk::Widget& anchor) {
    if (!m_library) return;

    const color::Swatch* sw = m_library->find_swatch(id);
    if (!sw) return;
    const auto* solid = std::get_if<color::SolidSwatch>(sw);
    if (!solid) return;

    // Set the editing-target so every callback updates this swatch in
    // place. Distinct from the "new swatch" mode which starts with an
    // empty m_editing_swatch_id.
    m_editing_swatch_id = id;

    // S73: capture session state for create-flow Esc cleanup. This
    // path is the existing-edit flow (not create), so we just clear
    // m_session_created_id to prevent any leakage from a previous
    // session. The original colour for undo is captured via the
    // ClosedFn lambda's by-value capture below.
    m_session_created_id.clear();

    // s207 m1: app-wide singleton — see on_add_clicked for the rationale.
    ColorPickerPopover::shared().open(
        anchor,
        solid->color,
        /*with_alpha=*/false,
        // on_changed — live drag, writes through every callback.
        // On Esc, the picker self-reverts (m_current = m_original) and
        // re-emits this signal at the original colour — that is the
        // mechanism by which the swatch returns to its original colour.
        // No separate revert path is needed.
        [this, id](const color::Color& c) {
            if (!m_library) return;
            const color::Swatch* existing = m_library->find_swatch(id);
            if (!existing) {
                m_editing_swatch_id.clear();
                return;
            }
            color::SolidSwatch updated =
                std::get<color::SolidSwatch>(*existing);
            updated.color = c;
            m_library->update_swatch(id, color::Swatch{updated});
            m_sig_library_changed.emit();
            // S83 m4h v4: see inline-edit path above for rationale.
            m_sig_inspector_refresh_needed.emit();
            refresh();
        },
        // on_closed — fires once after popdown.
        //   committed=true  → Return / pick-recent / outside-click. The
        //                     swatch's current colour is whatever the
        //                     last on_changed wrote. Push an
        //                     EditSwatchCommand if it actually differs
        //                     from the original; no-op otherwise.
        //   committed=false → Esc. Picker self-reverted via on_changed
        //                     above; the swatch's colour is back to
        //                     original. Nothing to record on the undo
        //                     stack.
        //
        // S85 cont-2: on commit, also stamp the popover's last_
        // committed_name onto the swatch and fold the name change
        // (if any) into the same EditSwatchCommand. The full ctor
        // takes name_before/name_after; if the user didn't change
        // the name, before==after and undo's a no-op for that field.
        [this, id, original_color = solid->color,
         original_name = solid->header.name](bool committed) {
            m_editing_swatch_id.clear();
            if (!committed) return;
            if (!m_canvas || !m_canvas->history() || !m_library) return;
            const color::Swatch* current = m_library->find_swatch(id);
            if (!current) return;
            const auto* current_solid = std::get_if<color::SolidSwatch>(current);
            if (!current_solid) return;

            // Apply the typed name before pushing the command, so the
            // before/after snapshot reflects what the user actually
            // sees post-commit. Empty last_committed_name shouldn't
            // happen (the popover falls back to derived name on
            // commit), but defensively skip the rename if it does.
            std::string final_name =
                ColorPickerPopover::shared().last_committed_name();
            if (final_name.empty()) final_name = original_name;
            if (final_name != current_solid->header.name) {
                color::SolidSwatch renamed = *current_solid;
                renamed.header.name = final_name;
                m_library->update_swatch(id, color::Swatch{renamed});
                m_sig_library_changed.emit();
                refresh();
                current = m_library->find_swatch(id);
                current_solid = current ?
                    std::get_if<color::SolidSwatch>(current) : nullptr;
                if (!current_solid) return;
            }

            // No-op suppression: skip the command if neither colour
            // nor name actually changed.
            if (original_color == current_solid->color &&
                original_name == current_solid->header.name) return;

            m_canvas->history()->push(
                std::make_unique<EditSwatchCommand>(
                    m_library, id,
                    original_color, current_solid->color,
                    original_name,  current_solid->header.name));
        },
        // S85 cont-2: enable the inline name field, pre-filled with
        // the swatch's existing name. User can accept or type over.
        ColorPickerPopover::NameField{ /*enabled=*/true,
                                       solid->header.name });
}

// ── Chip context actions (M5) ──────────────────────────────────────────────

void SwatchesPanel::on_ctx_edit_color() {
    if (!m_library || m_ctx_swatch_id.empty()) return;
    // No per-chip anchor available here — the menu was closed already,
    // the chip widget may even have been rebuilt by refresh(). Anchor
    // the edit popover to the panel's add-button instead. Visually
    // acceptable and avoids chasing a widget pointer.
    open_edit_popover_for(m_ctx_swatch_id, m_btn_add);
}

void SwatchesPanel::on_ctx_rename_swatch() {
    if (!m_library || m_ctx_swatch_id.empty()) return;
    const color::Swatch* sw = m_library->find_swatch(m_ctx_swatch_id);
    if (!sw) return;
    std::string current = color::swatch_header(*sw).name;
    color::SwatchId id = m_ctx_swatch_id;  // capture by value

    // s220 m1a: snapshot the colour pre-rename so we can build an
    // EditSwatchCommand carrying name_before/name_after (and colour
    // unchanged, before == after). The existing EditSwatchCommand
    // shape already supports rename-only edits (S85 cont-2) — the
    // description() resolves to "Rename swatch" when only the name
    // changed.
    color::Color color_snapshot = color::Color::black();
    if (const auto* solid = std::get_if<color::SolidSwatch>(sw)) {
        color_snapshot = solid->color;
    }

    prompt_text("Rename swatch", current,
                [this, id, current, color_snapshot]
                (const std::string& name) {
        if (!m_library) return;
        // S83 m4h v5: empty input is meaningful here — it means
        // "clear the user-supplied name; use the auto-derived
        // region name in UI." rename_swatch accepts empty; the
        // inspector's annotation block falls back to
        // color::region_name when swatch_header.name is empty.
        // This implements the user-facing rule "every swatch has
        // a non-empty display name — user-set OR auto-derived"
        // (memory rule from session start).
        //
        // s220 m1a: push EditSwatchCommand through the history if
        // available (panel was given a history pointer at startup).
        // The command's execute() calls update_swatch which fires
        // signal_swatch_changed → live recolour walk; same path the
        // direct call took, plus undo coverage. Skip the push if name
        // didn't change (Esc / OK-with-no-edit no-op).
        if (name == current) return;
        if (m_history) {
            auto cmd = std::make_unique<EditSwatchCommand>(
                m_library, id, color_snapshot, color_snapshot,
                current, name);
            cmd->execute();
            m_history->push(std::move(cmd));
        } else {
            m_library->rename_swatch(id, name);
        }
        m_sig_library_changed.emit();
        // Bound objects' inspector annotations show the (now-stale)
        // old name until the panel triggers a rebuild. Inspector
        // reads swatch_header(*sw).name live each rebuild, so a
        // refresh fixes it.
        m_sig_inspector_refresh_needed.emit();
        refresh();
    });
}

void SwatchesPanel::on_ctx_duplicate_swatch() {
    if (!m_library || m_ctx_swatch_id.empty()) return;
    const color::Swatch* sw = m_library->find_swatch(m_ctx_swatch_id);
    if (!sw) return;
    const auto* solid = std::get_if<color::SolidSwatch>(sw);
    if (!solid) return;

    // Deep-copy the source, clear the id so the library generates a new
    // one, append " copy" to the name. Empty name stays empty.
    color::SolidSwatch copy = *solid;
    copy.header.id.clear();
    if (!copy.header.name.empty()) copy.header.name += " copy";

    // s220 m1a: push AddSwatchCommand instead of calling add_swatch
    // directly. The command mints a fresh id on execute(); we read it
    // back from m_assigned_id (mirrored into swatch_value.header by
    // execute()) so the touch_recent / add_to_palette follow-ups
    // address the right swatch.
    //
    // The follow-up operations (touch_recent, add_to_palette) are NOT
    // covered by undo. Recents is a transient working list; palette
    // membership for a freshly-duplicated swatch is best-effort — if
    // the user undoes the duplicate, RemoveSwatchCommand's scrub on
    // the eventual remove would clean up anyway, but here the AddSwatch
    // undo just removes the swatch, and remove_swatch's own palette
    // scrubbing strips the membership transitively.
    color::SwatchId new_id;
    if (m_history) {
        auto cmd = std::make_unique<AddSwatchCommand>(
            m_library, color::Swatch{copy}, "Duplicate swatch");
        cmd->execute();
        new_id = cmd->m_assigned_id;
        m_history->push(std::move(cmd));
    } else {
        new_id = m_library->add_swatch(color::Swatch{copy});
    }
    if (new_id.empty()) return;
    m_library->touch_recent(new_id);

    // Put the duplicate into the same palette context where the original
    // was right-clicked (if any). Falls back to the active palette for
    // recents-chip duplicates.
    color::PaletteId target = m_ctx_palette_id;
    if (target.empty()) target = ensure_active_palette();
    if (!target.empty()) {
        m_library->add_to_palette(target, new_id);
    }

    m_sig_library_changed.emit();
    refresh();
}

void SwatchesPanel::on_ctx_delete_swatch() {
    if (!m_library || m_ctx_swatch_id.empty()) return;
    // S83 m4h v4: clear bindings on bound objects BEFORE removing
    // the swatch from the library. Without this, remove_swatch
    // (per its own header comment) deliberately leaves dangling
    // fill_swatch_id / stroke_swatch_id values on bound objects —
    // pending a "delete-with-usage-check flow" that's still backlog.
    // m4h v4 implements the no-confirm version: bound objects
    // unbind cleanly, the cache (the actual rendered colour) is
    // preserved per break-on-override v1, the inspector falls back
    // to the derived region name.
    //
    // s220 m1a note: this unbind step is NOT yet covered by undo.
    // The RemoveSwatchCommand below covers the library-side remove
    // (re-adds the swatch + restores palette memberships on undo),
    // but the per-node binding-clear is a scene-tree mutation that
    // would need its own command snapshot. Tracked as a follow-up;
    // the un-undoable unbind has been the behaviour since m4h v4
    // and isn't a new regression.
    if (m_canvas) {
        m_canvas->unbind_swatch_from_doc(m_ctx_swatch_id);
    }
    // s220 m1a: push RemoveSwatchCommand. The ctor snapshots the
    // swatch value + palette memberships BEFORE execute() runs, then
    // execute() calls remove_swatch (cleaning palettes + recents).
    // Undo re-adds via add_swatch with the captured id and replays
    // the palette memberships.
    if (m_history) {
        auto cmd = std::make_unique<RemoveSwatchCommand>(
            m_library, m_ctx_swatch_id);
        cmd->execute();
        m_history->push(std::move(cmd));
    } else {
        // remove_swatch cleans up references in all palettes + recents.
        m_library->remove_swatch(m_ctx_swatch_id);
    }
    m_sig_library_changed.emit();
    m_sig_inspector_refresh_needed.emit();
    refresh();
}

void SwatchesPanel::on_ctx_move_to_palette(const color::PaletteId& target) {
    if (!m_library || m_ctx_swatch_id.empty() || target.empty()) return;

    // If the user right-clicked from a palette grid (non-empty context),
    // this is a move: remove from source, add to target. For recents-chip
    // context (empty), it's just an add.
    if (!m_ctx_palette_id.empty() && m_ctx_palette_id != target) {
        m_library->remove_from_palette(m_ctx_palette_id, m_ctx_swatch_id);
    }
    m_library->add_to_palette(target, m_ctx_swatch_id);

    m_sig_library_changed.emit();
    refresh();
}

} // namespace Curvz
