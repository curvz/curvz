//
// ThemesDialog.cpp — apply / manage / import / export themes (S103 m3).
//
// Structure:
//   * ctor → set up window chrome, build panes, wire library signals.
//   * build_apply_pane / build_library_pane / build_footer construct
//     the static widget tree once.
//   * rebuild_apply_pane / rebuild_library_pane re-populate list /
//     dropdown contents on every library change.
//   * Per-action handlers route through theme::theme_io and the
//     command history; library signals trigger rebuild_*.
//
// Pattern parallels for code review:
//   * Self-managed lifecycle      — StyleEditorDialog
//   * Kebab popover for io        — StylesPanel::rebuild_kebab_menu
//   * File dialogs (open / save)  — StylesPanel::on_import_styles /
//                                   on_export_styles
//   * apply_theme_to_doc + redraw — fresh code; mirrors how
//                                   PropertiesPanel's section handlers
//                                   tap doc->...= and emit
//                                   prop_changed → MainWindow handles
//                                   queue_draw / refresh_inspector.
//

#include "ThemesDialog.hpp"
#include "CurvzProject.hpp"
#include "CommandHistory.hpp"
#include "theme/Theme.hpp"
#include "theme/ThemeIO.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2: apply_motif_class_from_parent

#include <gtkmm/alertdialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/popover.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/separator.h>
#include <gtkmm/stringlist.h>
#include <giomm/file.h>
#include <giomm/liststore.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <glibmm/main.h>

#include <filesystem>
#include <cctype>
#include <utility>

namespace Curvz {

// ── ctor ────────────────────────────────────────────────────────────────────

ThemesDialog::ThemesDialog(Gtk::Window&    parent,
                           CurvzProject*   project,
                           CommandHistory* history,
                           OnChangedCb     on_changed)
    : m_project(project)
    , m_history(history)
    , m_on_changed(std::move(on_changed)) {

    if (!m_project) {
        LOG_WARN("ThemesDialog: constructed with null project — nothing to "
                 "apply to. Self-deleting.");
        Glib::signal_idle().connect_once([this]() { delete this; });
        return;
    }

    // Self-managed: GTK keeps the window alive until close. The
    // dialog deletes itself in its close handler.
    m_window = Gtk::make_managed<Gtk::Window>();
    curvz::utils::set_name(m_window, "dlg_th", "themes_dialog_root");
    m_window->set_title("Themes");
    m_window->set_modal(true);
    m_window->set_resizable(true);
    m_window->set_default_size(480, 600);
    m_window->set_transient_for(parent);
    // S117 m18 v2: refactored to curvz::utils::apply_motif_class_from_parent.
    // Same effect (copy curvz-light from parent if present) but the helper
    // is reused across every Curvz dialog so the inheritance pattern is
    // consistent. See curvz_utils.hpp for rationale.
    curvz::utils::apply_motif_class_from_parent(*m_window, parent);

    // SimpleActionGroup hosts the kebab actions. Same idiom as
    // StylesPanel — actions named "themes-io.<verb>" so the menu's
    // detailed-action strings can resolve them.
    m_actions = Gio::SimpleActionGroup::create();
    auto act_import = Gio::SimpleAction::create("import-themes");
    act_import->signal_activate().connect(
        [this](const Glib::VariantBase&) { on_import_themes(); });
    m_actions->add_action(act_import);
    auto act_export = Gio::SimpleAction::create("export-themes");
    act_export->signal_activate().connect(
        [this](const Glib::VariantBase&) { on_export_themes(); });
    m_actions->add_action(act_export);
    m_window->insert_action_group("themes-io", m_actions);

    build();
    connect_library_signals();
    rebuild();

    // Self-delete on window close. Disconnect library signals first
    // — if the project outlives the dialog, library mutations after
    // close would otherwise dispatch to a destroyed `this`. Defer the
    // delete-this via signal_idle (GTK4 widget-mutation idiom; same
    // reasoning as StyleEditorDialog).
    m_window->signal_close_request().connect(
        [this]() -> bool {
            disconnect_library_signals();
            Glib::signal_idle().connect_once([this]() { delete this; });
            return false;  // allow default close to proceed
        }, /*after=*/false);

    m_window->present();
}

// ── Library signal wiring ───────────────────────────────────────────────────

void ThemesDialog::connect_library_signals() {
    if (!m_project) return;
    auto& lib = m_project->themes;
    // Every library mutation triggers a full rebuild — both panes
    // depend on the library list (apply pane's dropdown + library
    // pane's row list). Rebuild is cheap (sub-100 themes); no need
    // for finer-grained partial refresh.
    m_conn_added = lib.signal_theme_added().connect(
        [this](theme::ThemeId) { rebuild(); });
    m_conn_changed = lib.signal_theme_changed().connect(
        [this](theme::ThemeId) { rebuild(); });
    m_conn_removed = lib.signal_theme_removed().connect(
        [this](theme::ThemeId) { rebuild(); });
}

void ThemesDialog::disconnect_library_signals() {
    m_conn_added.disconnect();
    m_conn_changed.disconnect();
    m_conn_removed.disconnect();
}

// ── build (called once from ctor) ───────────────────────────────────────────

void ThemesDialog::build() {
    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    root->set_spacing(8);
    root->set_margin_top(12);
    root->set_margin_bottom(12);
    root->set_margin_start(12);
    root->set_margin_end(12);

    build_apply_pane(*root);

    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

    build_library_pane(*root);

    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

    build_footer(*root);

    m_window->set_child(*root);
}

// ── Apply pane ──────────────────────────────────────────────────────────────

void ThemesDialog::build_apply_pane(Gtk::Box& root) {
    auto* pane = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    pane->set_spacing(6);

    // Section title + description (S103 m3 fix1: dialog needs a one-
    // sentence "what does this do" line under the title — without it,
    // the radio toggles + dropdowns + checkbox list reads as an
    // assemblage of controls without an obvious mental model. The
    // description names the property categories so the user can decide
    // up-front whether this is the dialog they want.
    {
        auto* title = Gtk::make_managed<Gtk::Label>("Apply theme");
        title->set_xalign(0.0f);
        title->add_css_class("inspector-section-title");
        pane->append(*title);

        auto* desc = Gtk::make_managed<Gtk::Label>(
            "Copy non-physical document settings (units, background, "
            "guides, grid, margins, snap) from a saved theme or "
            "another document into the documents you select below. "
            "Canvas size, layers, and content are not affected.");
        desc->set_xalign(0.0f);
        desc->set_wrap(true);
        desc->set_max_width_chars(58);
        desc->add_css_class("dim-label");
        desc->set_margin_bottom(4);
        pane->append(*desc);
    }

    // ── Source picker ─────────────────────────────────────────────────
    {
        auto* src_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        src_box->set_spacing(4);
        src_box->set_margin_start(6);

        // Radio toggles. Using set_group() to make CheckButtons behave
        // as a radio group — the gtkmm idiom for "pick one of two".
        m_radio_saved = Gtk::make_managed<Gtk::CheckButton>("Saved theme");
        m_radio_doc   = Gtk::make_managed<Gtk::CheckButton>("Live document");
        curvz::utils::set_name(m_radio_saved, "dlg_th_rsv", "themes_dialog_radio_saved");
        curvz::utils::set_name(m_radio_doc,   "dlg_th_rdc", "themes_dialog_radio_doc");
        m_radio_doc->set_group(*m_radio_saved);

        auto* src_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        src_row->set_spacing(12);
        src_row->append(*m_radio_saved);
        src_row->append(*m_radio_doc);
        src_box->append(*src_row);

        // Two dropdowns, each appears under the radios. The inactive
        // one is set_sensitive(false). Both populated by
        // rebuild_apply_pane.
        m_drop_saved = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({}));
        curvz::utils::set_name(m_drop_saved, "dlg_th_dsv", "themes_dialog_saved_dd");
        m_drop_saved->set_hexpand(true);

        m_drop_doc = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({}));
        curvz::utils::set_name(m_drop_doc, "dlg_th_ddc", "themes_dialog_doc_dd");
        m_drop_doc->set_hexpand(true);

        src_box->append(*m_drop_saved);
        src_box->append(*m_drop_doc);

        // Wiring. Mode flip:
        m_radio_saved->signal_toggled().connect([this]() {
            if (m_loading) return;
            const bool saved = m_radio_saved->get_active();
            m_drop_saved->set_sensitive(saved);
            m_drop_doc->set_sensitive(!saved);
            update_targets_for_source();
        });
        // Doc dropdown selection feeds back into the disabled-row
        // logic in the targets list (active source's target row is
        // greyed). Saved-theme dropdown selection doesn't affect
        // targets — saved themes aren't a doc, so all targets are
        // checkable.
        m_drop_doc->property_selected().signal_changed().connect([this]() {
            if (m_loading) return;
            if (m_radio_doc && m_radio_doc->get_active()) {
                update_targets_for_source();
            }
        });

        pane->append(*src_box);
    }

    // ── Targets list ──────────────────────────────────────────────────
    {
        auto* tgt_label = Gtk::make_managed<Gtk::Label>("Apply to:");
        tgt_label->set_xalign(0.0f);
        tgt_label->set_margin_top(6);
        pane->append(*tgt_label);

        m_targets_list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        m_targets_list->set_spacing(2);
        m_targets_list->set_margin_start(6);
        // Wrap in a small ScrolledWindow so projects with many docs
        // don't blow up the dialog height. Min height ~6 rows.
        auto* scr = Gtk::make_managed<Gtk::ScrolledWindow>();
        scr->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        scr->set_min_content_height(140);
        scr->set_child(*m_targets_list);
        pane->append(*scr);

        auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        btn_row->set_spacing(6);
        btn_row->set_margin_start(6);
        auto* btn_all = Gtk::make_managed<Gtk::Button>("Select all");
        auto* btn_none = Gtk::make_managed<Gtk::Button>("Select none");
        btn_all->signal_clicked().connect([this]() { on_select_all_targets(); });
        btn_none->signal_clicked().connect([this]() { on_select_no_targets(); });
        btn_row->append(*btn_all);
        btn_row->append(*btn_none);
        pane->append(*btn_row);
    }

    // ── Hint + apply buttons ──────────────────────────────────────────
    {
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "This action is not undoable.");
        hint->set_xalign(0.0f);
        hint->set_margin_top(8);
        hint->add_css_class("dim-label");
        pane->append(*hint);

        auto* apply_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        apply_row->set_spacing(6);
        apply_row->set_halign(Gtk::Align::END);
        apply_row->set_margin_top(2);

        m_btn_save_cur = Gtk::make_managed<Gtk::Button>("Save current as theme…");
        curvz::utils::set_name(m_btn_save_cur, "dlg_th_sav", "themes_dialog_save_btn");
        m_btn_save_cur->signal_clicked().connect(
            [this]() { on_save_current_as_theme(); });
        apply_row->append(*m_btn_save_cur);

        m_btn_apply = Gtk::make_managed<Gtk::Button>("Apply");
        curvz::utils::set_name(m_btn_apply, "dlg_th_app", "themes_dialog_apply_btn");
        m_btn_apply->add_css_class("suggested-action");
        m_btn_apply->signal_clicked().connect([this]() { on_apply(); });
        apply_row->append(*m_btn_apply);

        pane->append(*apply_row);
    }

    root.append(*pane);
}

// ── Library pane ────────────────────────────────────────────────────────────

void ThemesDialog::build_library_pane(Gtk::Box& root) {
    auto* pane = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    pane->set_spacing(6);

    // Section header row: title + kebab
    {
        auto* hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        hdr->set_spacing(6);

        auto* title = Gtk::make_managed<Gtk::Label>("Saved themes");
        title->set_xalign(0.0f);
        title->set_hexpand(true);
        title->add_css_class("inspector-section-title");
        hdr->append(*title);

        // Kebab — Import / Export. Same shape as StylesPanel's IO
        // section (sans the per-pack Load submenu — that's a
        // post-export discoverability nicety we can add later if
        // user-pack proliferation justifies it).
        m_btn_io_kebab = Gtk::make_managed<Gtk::MenuButton>();
        curvz::utils::set_name(m_btn_io_kebab, "dlg_th_iok", "themes_dialog_io_kebab_btn");
        m_btn_io_kebab->set_icon_name("view-more-symbolic");
        m_btn_io_kebab->set_tooltip_text("Import / Export themes");

        auto menu = Gio::Menu::create();
        menu->append("Import…",          "themes-io.import-themes");
        menu->append("Export user themes…", "themes-io.export-themes");
        auto* popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
        popover->set_has_arrow(false);
        m_btn_io_kebab->set_popover(*popover);

        hdr->append(*m_btn_io_kebab);
        pane->append(*hdr);
    }

    // List body. Each row is built by rebuild_library_pane.
    {
        m_library_list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        m_library_list->set_spacing(2);
        m_library_list->set_margin_start(6);

        m_library_empty = Gtk::make_managed<Gtk::Label>(
            "No saved themes yet. "
            "Use \"Save current as theme…\" or Import to add one.");
        m_library_empty->set_xalign(0.0f);
        m_library_empty->set_wrap(true);
        m_library_empty->add_css_class("dim-label");
        m_library_list->append(*m_library_empty);

        auto* scr = Gtk::make_managed<Gtk::ScrolledWindow>();
        scr->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        scr->set_min_content_height(140);
        scr->set_child(*m_library_list);
        scr->set_vexpand(true);
        pane->append(*scr);
    }

    root.append(*pane);
}

// ── Footer ──────────────────────────────────────────────────────────────────

void ThemesDialog::build_footer(Gtk::Box& root) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_halign(Gtk::Align::END);
    auto* btn_close = Gtk::make_managed<Gtk::Button>("Close");
    curvz::utils::set_name(btn_close, "dlg_th_cls", "themes_dialog_close_btn");
    btn_close->signal_clicked().connect([this]() {
        if (m_window) m_window->close();
    });
    row->append(*btn_close);
    root.append(*row);
}

// ── rebuild ─────────────────────────────────────────────────────────────────

void ThemesDialog::rebuild() {
    m_loading = true;
    rebuild_apply_pane();
    rebuild_library_pane();
    m_loading = false;

    // After rebuild, fix targets-list enabled state based on the
    // current source mode + selection (rebuild_apply_pane sets
    // initial selections; targets state depends on them).
    update_targets_for_source();
}

void ThemesDialog::rebuild_apply_pane() {
    if (!m_project) return;

    // ── Saved-theme dropdown ──────────────────────────────────────────
    {
        // Replace the model with a freshly-built StringList. gtkmm
        // doesn't expose a clean remove_all on StringList; building a
        // new one is cheap and avoids leaking the old.
        auto list = Gtk::StringList::create({});
        m_drop_saved_ids.clear();

        // Walk user categories first so the order matches what users
        // see in styles. v1 has no app themes anyway.
        for (const std::string& cat : m_project->themes.user_categories()) {
            for (const theme::Theme* t :
                 m_project->themes.user_themes_in_category(cat)) {
                if (!t) continue;
                // Display label: name first, with category in
                // parens if non-empty. Matches the established
                // convention in templates etc.
                std::string label = t->header.name;
                if (label.empty()) {
                    // Defensive: empty name shouldn't reach here in
                    // v1 (UX rule auto-names every theme), but if it
                    // does we show "(unnamed)" rather than the UUID.
                    label = "(unnamed)";
                }
                if (!t->header.category.empty()) {
                    label += "  —  " + t->header.category;
                }
                list->append(label);
                m_drop_saved_ids.push_back(t->header.id);
            }
        }
        m_drop_saved->set_model(list);
        if (!m_drop_saved_ids.empty()) {
            m_drop_saved->set_selected(0);
        }
    }

    // ── Document dropdown ──────────────────────────────────────────────
    {
        auto list = Gtk::StringList::create({});
        m_drop_doc_indices.clear();

        const auto& docs = m_project->documents;
        const int active = active_doc_index();
        for (std::size_t i = 0; i < docs.size(); ++i) {
            if (!docs[i]) continue;
            std::string label = doc_display_name(*docs[i], i);
            if (static_cast<int>(i) == active) {
                label += "  (active)";
            }
            list->append(label);
            m_drop_doc_indices.push_back(static_cast<int>(i));
        }
        m_drop_doc->set_model(list);
        // Default the doc-source dropdown to the active doc — that's
        // the most common "I want THIS doc's settings on the others"
        // workflow.
        if (!m_drop_doc_indices.empty()) {
            int sel = 0;
            for (std::size_t i = 0; i < m_drop_doc_indices.size(); ++i) {
                if (m_drop_doc_indices[i] == active) {
                    sel = static_cast<int>(i);
                    break;
                }
            }
            m_drop_doc->set_selected(static_cast<guint>(sel));
        }
    }

    // ── Default mode selection ────────────────────────────────────────
    //
    // First-time logic (rebuild from a freshly-built dialog):
    //   - If saved themes exist, default to Saved-theme mode.
    //   - Else, default to Live-document mode.
    //
    // Subsequent rebuilds (after a library mutation) preserve the
    // user's current mode selection — calling set_active() would
    // fire signal_toggled and yank the user's mode out from under
    // them. We only force-set on first build.
    if (m_radio_saved && m_radio_doc &&
        !m_radio_saved->get_active() && !m_radio_doc->get_active()) {
        if (!m_drop_saved_ids.empty()) {
            m_radio_saved->set_active(true);
        } else {
            m_radio_doc->set_active(true);
        }
    }

    // Sensitive state — drives off the radio.
    const bool saved_mode = m_radio_saved && m_radio_saved->get_active();
    m_drop_saved->set_sensitive(saved_mode && !m_drop_saved_ids.empty());
    m_drop_doc->set_sensitive(!saved_mode && !m_drop_doc_indices.empty());

    // Apply button's enabled state — disabled when there's nothing
    // to apply (no saved themes in saved mode, no docs in doc mode)
    // or no targets.
    if (m_btn_apply) {
        const bool has_source =
            (saved_mode && !m_drop_saved_ids.empty()) ||
            (!saved_mode && !m_drop_doc_indices.empty());
        m_btn_apply->set_sensitive(has_source);
    }

    // Save-current button — disabled when no docs exist (nothing
    // to capture from).
    if (m_btn_save_cur) {
        m_btn_save_cur->set_sensitive(!m_project->documents.empty());
    }

    // ── Targets list ──────────────────────────────────────────────────
    //
    // One CheckButton per document. Re-built fresh each rebuild —
    // simpler than diffing, and cheap at sub-20-doc scale.
    if (m_targets_list) {
        // Clear existing children.
        Gtk::Widget* child = m_targets_list->get_first_child();
        while (child) {
            Gtk::Widget* next = child->get_next_sibling();
            m_targets_list->remove(*child);
            child = next;
        }
        m_target_checks.clear();

        const auto& docs = m_project->documents;
        const int active = active_doc_index();
        for (std::size_t i = 0; i < docs.size(); ++i) {
            if (!docs[i]) {
                m_target_checks.push_back(nullptr);
                continue;
            }
            std::string label = doc_display_name(*docs[i], i);
            if (static_cast<int>(i) == active) {
                label += "  (active)";
            }
            auto* cb = Gtk::make_managed<Gtk::CheckButton>(label);
            cb->set_active(true);  // checked by default; user untoggles
            m_targets_list->append(*cb);
            m_target_checks.push_back(cb);
        }
    }
}

void ThemesDialog::rebuild_library_pane() {
    if (!m_project) return;
    if (!m_library_list) return;

    // Clear existing rows.
    Gtk::Widget* child = m_library_list->get_first_child();
    while (child) {
        Gtk::Widget* next = child->get_next_sibling();
        m_library_list->remove(*child);
        child = next;
    }

    // Empty-state placeholder when there are no user themes.
    if (m_project->themes.user_theme_count() == 0) {
        m_library_empty = Gtk::make_managed<Gtk::Label>(
            "No saved themes yet. "
            "Use \"Save current as theme…\" or Import to add one.");
        m_library_empty->set_xalign(0.0f);
        m_library_empty->set_wrap(true);
        m_library_empty->add_css_class("dim-label");
        m_library_empty->set_margin_top(4);
        m_library_list->append(*m_library_empty);
        return;
    }
    m_library_empty = nullptr;

    // One row per user theme. Walk user categories so display order
    // matches the apply-pane dropdown.
    for (const std::string& cat : m_project->themes.user_categories()) {
        for (const theme::Theme* t :
             m_project->themes.user_themes_in_category(cat)) {
            if (!t) continue;

            const theme::ThemeId id = t->header.id;  // copy for lambda capture

            auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
            row->set_spacing(4);

            std::string label = t->header.name;
            if (label.empty()) label = "(unnamed)";
            if (!t->header.category.empty()) {
                label += "  —  " + t->header.category;
            }
            auto* lbl = Gtk::make_managed<Gtk::Label>(label);
            lbl->set_xalign(0.0f);
            lbl->set_hexpand(true);
            lbl->set_ellipsize(Pango::EllipsizeMode::END);
            row->append(*lbl);

            // Per-row icon buttons. Icons:
            //   document-edit-symbolic     — rename
            //   edit-copy-symbolic         — duplicate
            //   user-trash-symbolic        — delete
            // Match the freedesktop names used elsewhere in the codebase.
            auto make_icon_btn =
                [](const char* icon_name, const char* tooltip) {
                    auto* b = Gtk::make_managed<Gtk::Button>();
                    b->set_icon_name(icon_name);
                    b->set_tooltip_text(tooltip);
                    b->add_css_class("flat");
                    return b;
                };

            auto* btn_rename =
                make_icon_btn("document-edit-symbolic", "Rename");
            btn_rename->signal_clicked().connect(
                [this, id]() { on_rename_theme(id); });
            row->append(*btn_rename);

            auto* btn_dup =
                make_icon_btn("edit-copy-symbolic", "Duplicate");
            btn_dup->signal_clicked().connect(
                [this, id]() { on_duplicate_theme(id); });
            row->append(*btn_dup);

            auto* btn_del =
                make_icon_btn("user-trash-symbolic", "Delete");
            btn_del->signal_clicked().connect(
                [this, id]() { on_delete_theme(id); });
            row->append(*btn_del);

            m_library_list->append(*row);
        }
    }
}

// ── Targets state ───────────────────────────────────────────────────────────

void ThemesDialog::update_targets_for_source() {
    if (m_target_checks.empty()) return;

    // The active source's target row gets disabled (greyed) — can't
    // sync a doc to itself. Saved-theme mode has no implicated doc;
    // every target is enabled.
    int disabled_idx = -1;
    if (m_radio_doc && m_radio_doc->get_active()) {
        const guint sel = m_drop_doc->get_selected();
        if (sel != GTK_INVALID_LIST_POSITION &&
            sel < m_drop_doc_indices.size()) {
            disabled_idx = m_drop_doc_indices[sel];
        }
    }

    for (std::size_t i = 0; i < m_target_checks.size(); ++i) {
        Gtk::CheckButton* cb = m_target_checks[i];
        if (!cb) continue;
        const bool is_disabled = (static_cast<int>(i) == disabled_idx);
        cb->set_sensitive(!is_disabled);
        // When disabled, also untick — keeps the "applied to N docs"
        // count honest if the user ignores the disabled row.
        if (is_disabled) cb->set_active(false);
    }
}

// ── Source resolution ───────────────────────────────────────────────────────

std::optional<theme::Theme> ThemesDialog::current_source_theme() const {
    if (!m_project) return std::nullopt;

    const bool saved_mode = m_radio_saved && m_radio_saved->get_active();
    if (saved_mode) {
        if (m_drop_saved_ids.empty()) return std::nullopt;
        const guint sel = m_drop_saved->get_selected();
        if (sel == GTK_INVALID_LIST_POSITION ||
            sel >= m_drop_saved_ids.size()) return std::nullopt;
        const theme::Theme* t =
            m_project->themes.find_theme(m_drop_saved_ids[sel]);
        if (!t) return std::nullopt;
        return *t;
    } else {
        if (m_drop_doc_indices.empty()) return std::nullopt;
        const guint sel = m_drop_doc->get_selected();
        if (sel == GTK_INVALID_LIST_POSITION ||
            sel >= m_drop_doc_indices.size()) return std::nullopt;
        const int doc_idx = m_drop_doc_indices[sel];
        if (doc_idx < 0 ||
            doc_idx >= static_cast<int>(m_project->documents.size()))
            return std::nullopt;
        const auto& doc_ptr = m_project->documents[doc_idx];
        if (!doc_ptr) return std::nullopt;
        return theme::capture_theme_from_doc(*doc_ptr);
    }
}

// ── Apply ───────────────────────────────────────────────────────────────────

void ThemesDialog::on_apply() {
    if (!m_project) return;

    auto src = current_source_theme();
    if (!src) {
        LOG_INFO("ThemesDialog::on_apply: no source resolved, ignoring");
        return;
    }

    // Capture a user-facing source label BEFORE the apply walks the
    // targets — for the post-apply confirmation message. The label
    // depends on which radio mode is active. Saved-theme mode reads
    // src->header.name (the source theme); live-document mode looks
    // up the doc by index.
    std::string source_label;
    {
        const bool saved_mode = m_radio_saved && m_radio_saved->get_active();
        if (saved_mode) {
            source_label = src->header.name;
            if (source_label.empty()) source_label = "(unnamed theme)";
        } else if (!m_drop_doc_indices.empty()) {
            const guint sel = m_drop_doc->get_selected();
            if (sel != GTK_INVALID_LIST_POSITION &&
                sel < m_drop_doc_indices.size()) {
                const int doc_idx = m_drop_doc_indices[sel];
                if (doc_idx >= 0 &&
                    doc_idx < static_cast<int>(m_project->documents.size()) &&
                    m_project->documents[doc_idx]) {
                    source_label = doc_display_name(
                        *m_project->documents[doc_idx],
                        static_cast<std::size_t>(doc_idx));
                }
            }
        }
        if (source_label.empty()) source_label = "(source)";
    }

    // Walk targets, apply to each ticked one. Disabled rows are not
    // ticked (update_targets_for_source enforced this); defensive
    // re-check just in case. Collect names for the confirmation message.
    std::vector<std::string> target_names;
    int active_idx = active_doc_index();
    bool active_was_target = false;

    for (std::size_t i = 0; i < m_target_checks.size(); ++i) {
        Gtk::CheckButton* cb = m_target_checks[i];
        if (!cb) continue;
        if (!cb->get_sensitive()) continue;
        if (!cb->get_active()) continue;
        if (i >= m_project->documents.size()) break;
        auto& doc_ptr = m_project->documents[i];
        if (!doc_ptr) continue;

        theme::apply_theme_to_doc(*src, *doc_ptr);
        target_names.push_back(doc_display_name(*doc_ptr, i));
        if (static_cast<int>(i) == active_idx) active_was_target = true;
    }

    if (target_names.empty()) {
        LOG_INFO("ThemesDialog::on_apply: 0 targets selected, nothing applied");
        // Don't leave the user wondering why the click did nothing. Mirror
        // the post-apply success alert's shape (title + detail) so feedback
        // is consistent across the two outcomes. S110 m3 fix for S103
        // carryover.
        if (m_window) {
            auto dlg = Gtk::AlertDialog::create("No targets selected");
            dlg->set_detail(
                "Tick one or more documents in the Apply to: list, "
                "then click Apply again.");
            dlg->set_buttons({"OK"});
            dlg->set_default_button(0);
            dlg->show(*m_window);
        }
        return;
    }

    // Project-snap mirror — the existing snap-edit paths in
    // PropertiesPanel and MainWindow keep project->snap == active
    // doc's snap so the next save round-trips correctly. If the
    // active doc was one of the apply targets, mirror now.
    if (active_was_target && active_idx >= 0 &&
        active_idx < static_cast<int>(m_project->documents.size()) &&
        m_project->documents[active_idx]) {
        m_project->snap = m_project->documents[active_idx]->snap;
    }

    LOG_INFO("ThemesDialog::on_apply: applied '{}' to {} document(s)",
             source_label, target_names.size());

    // After apply we DON'T rebuild — nothing visible in the dialog
    // changed (library unchanged, doc list unchanged, source unchanged).
    // Rebuild would also reset the targets list to "all checked",
    // wiping the user's selections — surprising if they wanted to
    // tweak and re-apply. Just notify the host so the canvas + inspector
    // refresh.
    if (m_on_changed) m_on_changed();

    // ── Post-apply confirmation (S103 m3 fix1) ────────────────────
    //
    // Show a brief AlertDialog so the user knows the action took
    // effect on which docs. Without this, apply is silent — the
    // canvas redraws but the dialog is still in front, so the user
    // has no signal that anything happened beyond the button click.
    //
    // Message format:
    //   Title:   "Theme applied"
    //   Detail:  "Settings from <source> have been copied to:
    //              • Doc A
    //              • Doc B
    //              • Doc C"
    //
    // Bullet-list rather than comma-separated so 5+ targets still
    // read cleanly. AlertDialog wraps text but doesn't paginate;
    // sub-20 docs is the practical ceiling for any project (matches
    // the dialog's targets list scrollwindow sizing).
    {
        std::string detail = "Settings from \"" + source_label +
                             "\" have been copied to:\n";
        for (const std::string& name : target_names) {
            detail += "  \xE2\x80\xA2 " + name + "\n";
            // \xE2\x80\xA2 = U+2022 BULLET, UTF-8.
        }
        // Trim trailing newline — AlertDialog detail renders with
        // its own bottom padding; an extra blank line looks loose.
        if (!detail.empty() && detail.back() == '\n') detail.pop_back();

        auto dlg = Gtk::AlertDialog::create("Theme applied");
        dlg->set_detail(detail);
        dlg->set_buttons({"OK"});
        dlg->set_default_button(0);
        // Show on the parent (m_window) — modal-on-modal is fine,
        // and the user wants the confirmation tied to the dialog
        // they just acted in.
        if (m_window) dlg->show(*m_window);
    }
}

// ── Save-current-as-theme ───────────────────────────────────────────────────
//
// Pops a small inline name-prompt dialog. Captures the ACTIVE doc's
// current settings via capture_theme_from_doc and adds the result to
// the library under the user-supplied name.
//
// "Current" here means "the active doc's live settings", not "whatever
// the source dropdown is pointing at". The reasoning: in the typical
// flow the user has just edited the active doc and wants to preserve
// those settings as a reusable bundle. Tying this to the source
// dropdown would make it surprising when the user has the source set
// to "Saved theme: Print Setup" — clicking save would create a copy
// of Print Setup, which is what the Duplicate row button is for.
//
// Empty-name fallback: auto-generated "Theme 1", "Theme 2", etc.,
// walking up from 1 against the existing library names — matches the
// project-wide "no UUIDs in user-facing UI" rule.

void ThemesDialog::on_save_current_as_theme() {
    if (!m_project) return;
    CurvzDocument* doc = m_project->active_doc();
    if (!doc) {
        LOG_INFO("ThemesDialog::on_save_current_as_theme: no active doc");
        return;
    }

    // Capture now — values are taken at click time, not at name-
    // commit time, so the user can't sneak edits in between.
    theme::Theme captured = theme::capture_theme_from_doc(*doc);

    // Inline name prompt. AlertDialog with a custom child entry.
    // gtkmm's AlertDialog doesn't support custom widgets directly,
    // so we use a small modal Gtk::Window for the prompt — same
    // trick StylesPanel uses for "New category…".
    auto* prompt = Gtk::make_managed<Gtk::Window>();
    prompt->set_title("Save theme");
    prompt->set_modal(true);
    prompt->set_resizable(false);
    prompt->set_transient_for(*m_window);
    curvz::utils::apply_motif_class_from_parent(*prompt, *m_window);  // s117 m18 v2
    prompt->set_default_size(360, -1);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    box->set_spacing(8);
    box->set_margin_top(12);
    box->set_margin_bottom(12);
    box->set_margin_start(12);
    box->set_margin_end(12);

    auto* lbl = Gtk::make_managed<Gtk::Label>("Theme name:");
    lbl->set_xalign(0.0f);
    box->append(*lbl);

    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_hexpand(true);
    // Suggest a fresh default name walked against existing names.
    {
        std::string proposed = "Theme 1";
        for (int n = 1; n < 10000; ++n) {
            std::string candidate = "Theme " + std::to_string(n);
            if (!m_project->themes.has_user_name(candidate)) {
                proposed = candidate;
                break;
            }
        }
        entry->set_text(proposed);
    }
    entry->select_region(0, -1);
    box->append(*entry);

    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    btn_row->set_spacing(6);
    btn_row->set_halign(Gtk::Align::END);
    auto* btn_cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    auto* btn_ok     = Gtk::make_managed<Gtk::Button>("Save");
    btn_ok->add_css_class("suggested-action");
    btn_row->append(*btn_cancel);
    btn_row->append(*btn_ok);
    box->append(*btn_row);

    prompt->set_child(*box);

    auto commit = [this, prompt, entry, captured]() mutable {
        std::string name = entry->get_text();
        // Trim trailing whitespace; empty after trim falls back to
        // an auto-generated unique name.
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
            name.pop_back();
        }
        // Leading whitespace too.
        std::size_t lead = 0;
        while (lead < name.size() &&
               std::isspace(static_cast<unsigned char>(name[lead]))) {
            ++lead;
        }
        if (lead > 0) name.erase(0, lead);

        if (name.empty()) {
            for (int n = 1; n < 10000; ++n) {
                std::string candidate = "Theme " + std::to_string(n);
                if (!m_project->themes.has_user_name(candidate)) {
                    name = candidate;
                    break;
                }
            }
            if (name.empty()) name = "Theme";  // ultimate fallback
        } else if (m_project->themes.has_user_name(name)) {
            // Name collision — disambiguate with a numeric suffix.
            std::string base = name;
            for (int n = 2; n < 10000; ++n) {
                std::string candidate = base + " " + std::to_string(n);
                if (!m_project->themes.has_user_name(candidate)) {
                    name = candidate;
                    break;
                }
            }
        }

        theme::Theme to_add = captured;
        to_add.header.id   = "";  // library mints a fresh thm_<uuid>
        to_add.header.name = name;
        // category left empty in v1; categorisation UX deferred.

        if (m_history) {
            auto cmd = std::make_unique<AddThemeCommand>(
                &m_project->themes, std::move(to_add), "Save theme");
            cmd->execute();
            m_history->push(std::move(cmd));
        } else {
            m_project->themes.add_theme(std::move(to_add));
        }
        notify_changed();
        prompt->close();
    };

    btn_cancel->signal_clicked().connect([prompt]() { prompt->close(); });
    btn_ok->signal_clicked().connect(commit);
    // Enter in the entry commits.
    entry->signal_activate().connect(commit);

    prompt->present();
    entry->grab_focus();
}

// ── Select all / none ───────────────────────────────────────────────────────

void ThemesDialog::on_select_all_targets() {
    for (Gtk::CheckButton* cb : m_target_checks) {
        if (cb && cb->get_sensitive()) cb->set_active(true);
    }
}

void ThemesDialog::on_select_no_targets() {
    for (Gtk::CheckButton* cb : m_target_checks) {
        if (cb) cb->set_active(false);
    }
}

// ── Library row actions ─────────────────────────────────────────────────────

void ThemesDialog::on_rename_theme(const theme::ThemeId& id) {
    if (!m_project) return;
    const theme::Theme* current = m_project->themes.find_theme(id);
    if (!current) {
        LOG_WARN("ThemesDialog::on_rename_theme: theme '{}' not found", id);
        return;
    }
    if (m_project->themes.is_built_in(id)) {
        LOG_WARN("ThemesDialog::on_rename_theme: '{}' is built-in", id);
        return;
    }

    // Inline rename via the same prompt-window pattern. Pre-fill with
    // the current name; commit pushes UpdateThemeCommand.
    auto* prompt = Gtk::make_managed<Gtk::Window>();
    prompt->set_title("Rename theme");
    prompt->set_modal(true);
    prompt->set_resizable(false);
    prompt->set_transient_for(*m_window);
    curvz::utils::apply_motif_class_from_parent(*prompt, *m_window);  // s117 m18 v2
    prompt->set_default_size(360, -1);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    box->set_spacing(8);
    box->set_margin_top(12);
    box->set_margin_bottom(12);
    box->set_margin_start(12);
    box->set_margin_end(12);

    auto* lbl = Gtk::make_managed<Gtk::Label>("New name:");
    lbl->set_xalign(0.0f);
    box->append(*lbl);

    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(current->header.name);
    entry->select_region(0, -1);
    box->append(*entry);

    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    btn_row->set_spacing(6);
    btn_row->set_halign(Gtk::Align::END);
    auto* btn_cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    auto* btn_ok     = Gtk::make_managed<Gtk::Button>("Rename");
    btn_ok->add_css_class("suggested-action");
    btn_row->append(*btn_cancel);
    btn_row->append(*btn_ok);
    box->append(*btn_row);

    prompt->set_child(*box);

    // Snapshot the original Theme value for UpdateThemeCommand's
    // `before` field — taken now so the command always reverts to
    // the right state even if the library mutates between dialog
    // open and commit (in practice impossible — modal — but defensive).
    theme::Theme before = *current;

    auto commit = [this, prompt, entry, id, before]() mutable {
        if (!m_project) { prompt->close(); return; }
        const theme::Theme* live = m_project->themes.find_theme(id);
        if (!live) { prompt->close(); return; }

        std::string name = entry->get_text();
        // Trim. Same rules as save-as.
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
            name.pop_back();
        }
        std::size_t lead = 0;
        while (lead < name.size() &&
               std::isspace(static_cast<unsigned char>(name[lead]))) {
            ++lead;
        }
        if (lead > 0) name.erase(0, lead);

        if (name.empty() || name == live->header.name) {
            // No-op — empty name is rejected (would violate the
            // "always non-empty" UX rule), and same-as-current is
            // a no-op rename.
            prompt->close();
            return;
        }

        if (m_project->themes.has_user_name(name)) {
            // Disambiguate.
            std::string base = name;
            for (int n = 2; n < 10000; ++n) {
                std::string candidate = base + " " + std::to_string(n);
                if (!m_project->themes.has_user_name(candidate)) {
                    name = candidate;
                    break;
                }
            }
        }

        theme::Theme after = before;
        after.header.name = name;

        if (m_history) {
            auto cmd = std::make_unique<UpdateThemeCommand>(
                &m_project->themes, id, before, after, "Rename theme");
            cmd->execute();
            m_history->push(std::move(cmd));
        } else {
            m_project->themes.update_theme(id, std::move(after));
        }
        notify_changed();
        prompt->close();
    };

    btn_cancel->signal_clicked().connect([prompt]() { prompt->close(); });
    btn_ok->signal_clicked().connect(commit);
    entry->signal_activate().connect(commit);

    prompt->present();
    entry->grab_focus();
}

void ThemesDialog::on_duplicate_theme(const theme::ThemeId& id) {
    if (!m_project) return;
    const theme::Theme* src = m_project->themes.find_theme(id);
    if (!src) {
        LOG_WARN("ThemesDialog::on_duplicate_theme: theme '{}' not found", id);
        return;
    }

    // duplicate_to_user already appends " copy" to the name, mints a
    // fresh id, and inserts. We re-implement here to route through
    // AddThemeCommand instead of the library's direct insert — same
    // pattern as the styles import flow. That keeps the duplicate
    // undoable.
    theme::Theme copy = *src;
    copy.header.id = "";  // library mints
    if (!copy.header.name.empty()) {
        copy.header.name += " copy";
    } else {
        copy.header.name = "Theme copy";
    }
    if (m_project->themes.has_user_name(copy.header.name)) {
        std::string base = copy.header.name;
        for (int n = 2; n < 10000; ++n) {
            std::string candidate = base + " " + std::to_string(n);
            if (!m_project->themes.has_user_name(candidate)) {
                copy.header.name = candidate;
                break;
            }
        }
    }

    if (m_history) {
        auto cmd = std::make_unique<AddThemeCommand>(
            &m_project->themes, std::move(copy), "Duplicate theme");
        cmd->execute();
        m_history->push(std::move(cmd));
    } else {
        m_project->themes.add_theme(std::move(copy));
    }
    notify_changed();
}

void ThemesDialog::on_delete_theme(const theme::ThemeId& id) {
    if (!m_project) return;
    const theme::Theme* current = m_project->themes.find_theme(id);
    if (!current) return;
    if (m_project->themes.is_built_in(id)) return;

    // Confirm via AlertDialog. Single-step destructive action; no
    // bindings to dangle, but the user still loses the saved bundle.
    auto alert = Gtk::AlertDialog::create(
        "Delete theme \"" + current->header.name + "\"?");
    alert->set_detail("This cannot be undone via the menu, "
                      "but Ctrl+Z will restore it.");
    alert->set_buttons({"Cancel", "Delete"});
    alert->set_cancel_button(0);
    alert->set_default_button(1);

    // Snapshot the value for RemoveThemeCommand. Library mutates
    // out from under the AlertDialog if signals fire — defensive
    // copy now.
    theme::Theme snapshot = *current;

    alert->choose(*m_window,
        [this, alert, id, snapshot](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                int btn = alert->choose_finish(result);
                if (btn != 1) return;  // cancelled or X
                if (!m_project) return;

                if (m_history) {
                    auto cmd = std::make_unique<RemoveThemeCommand>(
                        &m_project->themes, snapshot, "Delete theme");
                    cmd->execute();
                    m_history->push(std::move(cmd));
                } else {
                    m_project->themes.remove_theme(id);
                }
                notify_changed();
            } catch (const Glib::Error&) {
                // User dismissed via window close — silent.
            }
        });
}

// ── Import / Export ─────────────────────────────────────────────────────────
//
// Direct mirror of StylesPanel::on_import_styles / on_export_styles.

void ThemesDialog::on_import_themes() {
    if (!m_project) return;

    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Import themes");

    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto json_filter = Gtk::FileFilter::create();
    json_filter->set_name("Curvz theme packs (*.json)");
    json_filter->add_pattern("*.json");
    filters->append(json_filter);
    auto all_filter = Gtk::FileFilter::create();
    all_filter->set_name("All files");
    all_filter->add_pattern("*");
    filters->append(all_filter);
    dialog->set_filters(filters);
    dialog->set_default_filter(json_filter);

    dialog->open(*m_window,
        [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto file = dialog->open_finish(result);
                if (!file) return;
                if (!m_project) return;
                std::string path = file->get_path();

                auto loaded = theme::theme_io::load_path(path);
                if (!loaded) {
                    LOG_WARN("ThemesDialog::on_import_themes: load failed "
                             "for '{}'", path);
                    return;
                }

                std::size_t added = theme::theme_io::import_themes_into_library(
                    m_project->themes, m_history, *loaded);
                if (added == 0) {
                    LOG_INFO("ThemesDialog::on_import_themes: '{}' contained "
                             "no importable themes", path);
                    return;
                }

                notify_changed();
            } catch (const Glib::Error&) {
                // User cancelled or dialog error — silent.
            }
        });
}

void ThemesDialog::on_export_themes() {
    if (!m_project) return;

    // Snapshot the user tier up front (user might be slow picking a
    // destination; library could mutate while the dialog is open).
    auto snapshot = theme::theme_io::snapshot_user_tier(m_project->themes);
    if (snapshot.empty()) {
        LOG_INFO("ThemesDialog::on_export_themes: user tier is empty, "
                 "nothing to export");
        return;
    }

    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Export user themes");
    dialog->set_initial_name("themes.json");

    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto json_filter = Gtk::FileFilter::create();
    json_filter->set_name("Curvz theme packs (*.json)");
    json_filter->add_pattern("*.json");
    filters->append(json_filter);
    dialog->set_filters(filters);
    dialog->set_default_filter(json_filter);

    // Default to ~/.config/curvz/themes/ (mirrors styles export).
    namespace fs = std::filesystem;
    const std::string user_dir = theme::theme_io::user_themes_dir();
    std::error_code mkdir_ec;
    fs::create_directories(user_dir, mkdir_ec);
    if (!mkdir_ec) {
        dialog->set_initial_folder(Gio::File::create_for_path(user_dir));
    } else {
        LOG_WARN("ThemesDialog::on_export_themes: cannot create '{}': "
                 "{} (skipping initial-folder hint)",
                 user_dir, mkdir_ec.message());
    }

    dialog->save(*m_window,
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
                if (!theme::theme_io::write_path(path, snapshot)) {
                    LOG_WARN("ThemesDialog::on_export_themes: write failed "
                             "for '{}'", path);
                }
                // No library state changed — no notify_changed needed.
            } catch (const Glib::Error&) {
                // User cancelled or dialog error — silent.
            }
        });
}

// ── Helpers ─────────────────────────────────────────────────────────────────

std::string ThemesDialog::doc_display_name(const CurvzDocument& doc,
                                           std::size_t fallback_index) {
    if (!doc.filename.empty()) return doc.filename;
    return "Document " + std::to_string(fallback_index + 1);
}

int ThemesDialog::active_doc_index() const {
    if (!m_project) return -1;
    int idx = m_project->active_doc_index;
    if (idx < 0 || idx >= static_cast<int>(m_project->documents.size())) {
        return -1;
    }
    return idx;
}

void ThemesDialog::notify_changed() {
    // Local refresh covers library mutations; apply notifications use
    // the same hook because the active doc's settings may have shifted.
    rebuild();
    if (m_on_changed) m_on_changed();
}

} // namespace Curvz
