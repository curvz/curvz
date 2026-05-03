#include "ManageTemplatesDialog.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2
#include <algorithm>
#include <cairomm/cairomm.h>
#include <cmath>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/contentprovider.h>
#include <gdkmm/drag.h>
#include <gdkmm/pixbuf.h>
#include <giomm/asyncresult.h>
#include <glibmm/main.h>
#include <glibmm/value.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>

namespace Curvz {

// Row-thumbnail dimensions. Small on purpose — the dialog is for management,
// not selection; the picker in NewDocumentDialog is where you browse visually.
static constexpr int ROW_THUMB_PX = 48;

// ── Helpers ──────────────────────────────────────────────────────────────────

// Build the "W:H quality" spec string that appears next to a template name.
static std::string spec_text_for(const templates::TemplateEntry& e) {
    (void)e;
    // We don't carry canvas info on the TemplateEntry; spec comes from
    // loading the SVG which is expensive for a list view. Return empty for
    // now — M4b ships without per-row specs in the manager to keep scanning
    // fast. If wanted later, cache canvas info in TemplateMeta or scan.
    return {};
}

// Label: "<category>/<slug>" human-friendly form for logs / errors.
static std::string locator(const templates::TemplateEntry& e) {
    return e.meta.category + "/" + e.slug;
}

// ── Constructor ──────────────────────────────────────────────────────────────

ManageTemplatesDialog::ManageTemplatesDialog() {
    curvz::utils::set_name(*this, "dlg_mt", "manage_templates_dialog_root");
    set_title("Manage Templates");
    set_default_size(640, 560);
    set_hide_on_close(true);
    add_css_class("manage-templates-dialog");
    build_layout();
}

// ── build_layout ─────────────────────────────────────────────────────────────

void ManageTemplatesDialog::build_layout() {
    if (m_built) return;
    m_built = true;

    m_root.set_spacing(10);
    m_root.set_margin(12);
    set_child(m_root);

    // ── Header row ────────────────────────────────────────────────────────
    m_header_row.set_spacing(8);
    m_title_label.set_text("Templates");
    m_title_label.set_xalign(0.0f);
    m_title_label.set_hexpand(true);
    m_title_label.add_css_class("section-label");
    m_header_row.append(m_title_label);

    m_btn_new_cat.add_css_class("flat");
    curvz::utils::set_name(m_btn_new_cat, "dlg_mt_nc", "manage_templates_dialog_new_category_btn");
    m_btn_new_cat.signal_clicked().connect(
        sigc::mem_fun(*this, &ManageTemplatesDialog::on_new_category));
    m_header_row.append(m_btn_new_cat);
    m_root.append(m_header_row);

    // ── Toast ─────────────────────────────────────────────────────────────
    m_toast_label.add_css_class("dim-label");
    m_toast_label.set_xalign(0.0f);
    curvz::utils::set_name(m_toast_label, "dlg_mt_tst", "manage_templates_dialog_toast_lbl");
    m_toast_revealer.set_child(m_toast_label);
    // See category revealer comment — use NONE to avoid mid-animation
    // snapshot-without-allocation warnings.
    m_toast_revealer.set_transition_type(Gtk::RevealerTransitionType::NONE);
    m_toast_revealer.set_reveal_child(false);
    m_root.append(m_toast_revealer);

    // ── Scrollable list ───────────────────────────────────────────────────
    m_list.set_spacing(8);
    m_scroll.set_child(m_list);
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_hexpand(true);
    m_scroll.set_vexpand(true);
    m_root.append(m_scroll);

    // ── Footer ────────────────────────────────────────────────────────────
    m_footer.set_halign(Gtk::Align::END);
    curvz::utils::set_name(m_btn_close, "dlg_mt_cls", "manage_templates_dialog_close_btn");
    m_btn_close.signal_clicked().connect([this]() { hide(); });
    m_footer.append(m_btn_close);
    m_root.append(m_footer);
}

// ── show ─────────────────────────────────────────────────────────────────────

void ManageTemplatesDialog::show(Gtk::Window& parent, ChangedCb on_changed) {
    m_on_changed = std::move(on_changed);
    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);  // s117 m18 v2
    set_modal(true);

    // Defer rebuild() to avoid the snapshot-without-allocation warning
    // pattern (S48/S60 M1 class of bug).
    Glib::signal_idle().connect_once([this]() { rebuild(); });

    present();
}

// ── rebuild ──────────────────────────────────────────────────────────────────
// Clear the list and re-render from a fresh scan. Called on show() and after
// any successful mutation.

void ManageTemplatesDialog::rebuild() {
    // A rebuild tears down any inline-rename entry that was alive; clear
    // the flag so ✎ works again after a rebuild (e.g. after the user
    // completed the rename via idle-deferred path).
    m_rename_in_progress = false;

    // Clear existing children
    while (auto* child = m_list.get_first_child())
        m_list.remove(*child);

    // Scan + group by category
    auto all = templates::scan();
    auto sys_cats = templates::system_categories();
    std::vector<std::string> user_cats = templates::user_categories();

    // Merge the two so we render every known category, even empty ones.
    std::vector<std::string> all_cats = user_cats;
    for (const auto& s : sys_cats) {
        if (std::find(all_cats.begin(), all_cats.end(), s) == all_cats.end())
            all_cats.push_back(s);
    }
    std::sort(all_cats.begin(), all_cats.end());

    for (const auto& cat : all_cats) {
        std::vector<templates::TemplateEntry> in_cat;
        for (const auto& e : all) {
            if (e.meta.category == cat) in_cat.push_back(e);
        }
        bool is_system = (std::find(sys_cats.begin(), sys_cats.end(), cat)
                          != sys_cats.end());
        append_category_section(cat, in_cat, is_system);
    }
}

// ── append_category_section ──────────────────────────────────────────────────

void ManageTemplatesDialog::append_category_section(
        const std::string& category,
        const std::vector<templates::TemplateEntry>& entries,
        bool is_system_seeded) {
    // Section container
    auto* section = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    section->add_css_class("template-category-section");
    section->set_spacing(2);

    // ── Header row ────────────────────────────────────────────────────────
    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    header->set_spacing(6);
    header->add_css_class("template-category-header");

    // Chevron button (▸ / ▾) — also toggles the revealer
    auto* chevron = Gtk::make_managed<Gtk::Button>();
    chevron->add_css_class("flat");
    chevron->set_label("▾"); // start expanded
    header->append(*chevron);

    // Name label (click-to-edit)
    auto* name_lbl = Gtk::make_managed<Gtk::Label>(category);
    name_lbl->set_xalign(0.0f);
    name_lbl->set_hexpand(true);
    header->append(*name_lbl);

    // Count hint
    std::string count_text = "(" + std::to_string(entries.size()) + ")";
    auto* count_lbl = Gtk::make_managed<Gtk::Label>(count_text);
    count_lbl->add_css_class("dim-label");
    header->append(*count_lbl);

    // Rename button — disabled on system-seeded categories. (We still allow
    // rename on user-created categories even if they share a name with a
    // system category; that rare case is handled by rename_category.)
    auto* btn_rename = Gtk::make_managed<Gtk::Button>("✎");
    btn_rename->add_css_class("flat");
    btn_rename->set_tooltip_text(is_system_seeded
        ? "System categories cannot be renamed"
        : "Rename category");
    btn_rename->set_sensitive(!is_system_seeded);
    std::string cat_copy = category;
    btn_rename->signal_clicked().connect([this, cat_copy, name_lbl]() {
        begin_rename_category(cat_copy, name_lbl);
    });
    header->append(*btn_rename);

    // Delete button — disabled on system-seeded categories.
    auto* btn_delete = Gtk::make_managed<Gtk::Button>("🗑");
    btn_delete->add_css_class("flat");
    btn_delete->set_tooltip_text(is_system_seeded
        ? "System categories cannot be deleted"
        : "Delete category");
    btn_delete->set_sensitive(!is_system_seeded);
    int n_templates = (int)entries.size();
    btn_delete->signal_clicked().connect([this, cat_copy, n_templates]() {
        confirm_delete_category(cat_copy, n_templates);
    });
    header->append(*btn_delete);

    section->append(*header);

    // ── Revealer with template rows ───────────────────────────────────────
    auto* revealer = Gtk::make_managed<Gtk::Revealer>();
    // NONE (no transition animation) — avoids "snapshot without allocation"
    // warnings that fire when a SlideDown transition snapshots the inner
    // Box mid-animation, before its newly-rebuilt contents have been
    // allocated. The collapse / expand still works; it just snaps instead
    // of animating. Same pattern as other dialogs in this codebase that
    // avoid mid-transition snapshot races.
    revealer->set_transition_type(Gtk::RevealerTransitionType::NONE);
    revealer->set_reveal_child(true);
    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    body->set_spacing(4);
    body->set_margin_start(24);
    revealer->set_child(*body);
    section->append(*revealer);

    // Chevron toggle behavior
    chevron->signal_clicked().connect([chevron, revealer]() {
        bool now = !revealer->get_reveal_child();
        revealer->set_reveal_child(now);
        chevron->set_label(now ? "▾" : "▸");
    });

    // Determine current user default so we can flag the right row
    auto cur_default = templates::user_default();

    // Empty-category hint
    if (entries.empty()) {
        auto* hint = Gtk::make_managed<Gtk::Label>("(no templates in this category)");
        hint->add_css_class("dim-label");
        hint->set_xalign(0.0f);
        hint->set_margin_top(2);
        hint->set_margin_bottom(2);
        body->append(*hint);
    }

    for (const auto& e : entries) {
        bool is_default = cur_default
                          && cur_default->meta.category == e.meta.category
                          && cur_default->slug == e.slug;
        append_template_row(body, e, is_default);
    }

    // Install a drop target on the whole section (header + body). Users can
    // drop on either the header strip or anywhere in the expanded body.
    setup_category_drop_target(section, category);

    m_list.append(*section);
}

// ── append_template_row ──────────────────────────────────────────────────────

void ManageTemplatesDialog::append_template_row(
        Gtk::Box* parent_box,
        const templates::TemplateEntry& e,
        bool is_user_default) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(8);
    row->add_css_class("template-manager-row");

    // ── Thumbnail ─────────────────────────────────────────────────────────
    auto* thumb = Gtk::make_managed<Gtk::DrawingArea>();
    thumb->set_size_request(ROW_THUMB_PX, ROW_THUMB_PX);
    if (!e.thumb_path.empty()) {
        Glib::RefPtr<Gdk::Pixbuf> pb;
        try {
            pb = Gdk::Pixbuf::create_from_file(e.thumb_path);
        } catch (const Glib::Error&) {}
        thumb->set_draw_func(
            [pb](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                if (!pb) {
                    cr->set_source_rgba(0.5, 0.5, 0.5, 0.3);
                    cr->rectangle(1, 1, w - 2, h - 2);
                    cr->fill();
                    return;
                }
                int pw = pb->get_width();
                int ph = pb->get_height();
                if (pw <= 0 || ph <= 0) return;
                double scale = std::min((double)w / pw, (double)h / ph);
                double dw = pw * scale, dh = ph * scale;
                double ox = (w - dw) * 0.5, oy = (h - dh) * 0.5;
                cr->save();
                cr->translate(ox, oy);
                cr->scale(scale, scale);
                // s135 m2: pumped — replaces deprecated gdk_cairo_set_source_pixbuf.
                curvz::utils::cairo_set_source_pixbuf(cr, pb, 0, 0);
                cr->paint();
                cr->restore();
            });
    } else {
        thumb->set_draw_func(
            [](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                cr->set_source_rgba(0.5, 0.5, 0.5, 0.3);
                cr->rectangle(1, 1, w - 2, h - 2);
                cr->fill();
            });
    }
    row->append(*thumb);

    // ── Name + source + specs ─────────────────────────────────────────────
    auto* text_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    text_box->set_hexpand(true);
    text_box->set_valign(Gtk::Align::CENTER);

    std::string display_name = e.meta.name;
    auto* name_lbl = Gtk::make_managed<Gtk::Label>(display_name);
    name_lbl->set_xalign(0.0f);
    text_box->append(*name_lbl);

    std::string sub = e.is_user ? "user" : "system";
    std::string spec = spec_text_for(e);
    if (!spec.empty()) sub += " — " + spec;
    if (!e.meta.description.empty()) {
        sub += " — " + e.meta.description;
    }
    auto* sub_lbl = Gtk::make_managed<Gtk::Label>(sub);
    sub_lbl->add_css_class("dim-label");
    sub_lbl->set_xalign(0.0f);
    sub_lbl->set_ellipsize(Pango::EllipsizeMode::END);
    text_box->append(*sub_lbl);

    row->append(*text_box);

    // Double-click name to rename (user bundles only). Replaces the old row
    // ✎ button. System templates don't wire the gesture — their names are
    // read-only and silent double-clicks are fine.
    if (e.is_user) {
        auto click = Gtk::GestureClick::create();
        click->set_button(1);
        templates::TemplateEntry e_rn = e;
        click->signal_pressed().connect(
            [this, e_rn, name_lbl](int n_press, double, double) {
                if (n_press >= 2) {
                    begin_rename_template(e_rn, name_lbl);
                }
            });
        name_lbl->add_controller(click);
    }

    // ── Actions ───────────────────────────────────────────────────────────
    // (Row-level ✎ Rename removed in favor of double-click on the name.)
    //
    // Default indicator — filled blue dot on THE default template (there is
    // exactly one at a time, or none if the user hasn't picked one). Any
    // other row shows a hollow dim dot as an affordance that clicking it
    // will make it the default. Clicking the active dot clears the default
    // pointer, after which the app falls back to its built-in Default.
    // (Earlier versions used ⭐/★ which read as "favorite" — misleading
    // since only one entry can hold the default at a time.)
    auto* btn_default = Gtk::make_managed<Gtk::Button>(
        is_user_default ? "\u25CF" : "\u25CB");
    btn_default->add_css_class("flat");
    btn_default->add_css_class(is_user_default
        ? "default-dot-active"
        : "default-dot-inactive");
    btn_default->set_tooltip_text(is_user_default
        ? "This is the default template (click to clear)"
        : "Set as default");
    if (is_user_default) {
        btn_default->signal_clicked().connect([this]() { clear_default(); });
    } else {
        templates::TemplateEntry e_d = e;
        btn_default->signal_clicked().connect([this, e_d]() {
            set_default(e_d);
        });
    }
    row->append(*btn_default);

    // Delete — disabled on system bundles
    auto* btn_delete = Gtk::make_managed<Gtk::Button>("🗑");
    btn_delete->add_css_class("flat");
    btn_delete->set_tooltip_text(e.is_user
        ? "Delete template"
        : "System templates cannot be deleted");
    btn_delete->set_sensitive(e.is_user);
    templates::TemplateEntry e_del = e;
    btn_delete->signal_clicked().connect([this, e_del]() {
        confirm_delete_template(e_del);
    });
    row->append(*btn_delete);

    parent_box->append(*row);

    // Wire the row as a drag source for user bundles. Dragged payload is
    // "<category>/<slug>"; the drop target resolves via fresh scan().
    // System rows are intentionally not draggable.
    if (e.is_user) {
        setup_row_drag_source(row, e);
    }
}

// ── confirm_delete_template ──────────────────────────────────────────────────

void ManageTemplatesDialog::confirm_delete_template(
        const templates::TemplateEntry& e) {
    if (!e.is_user) {
        show_toast("This is a protected system template.");
        return;
    }
    auto dlg = Gtk::AlertDialog::create(
        "Delete template \"" + e.meta.name + "\"?");
    dlg->set_detail("This cannot be undone.");
    dlg->set_buttons({"Cancel", "Delete"});
    dlg->set_default_button(0);
    dlg->set_cancel_button(0);
    templates::TemplateEntry e_copy = e;
    dlg->choose(*this, [this, dlg, e_copy](Glib::RefPtr<Gio::AsyncResult>& r) {
        try {
            int btn = dlg->choose_finish(r);
            if (btn != 1) return;
            if (templates::remove(e_copy)) {
                notify_changed();
            } else {
                show_toast("Delete failed.");
            }
        } catch (...) {}
    });
}

// ── confirm_delete_category ──────────────────────────────────────────────────

void ManageTemplatesDialog::confirm_delete_category(const std::string& name,
                                                     int template_count) {
    std::string title = "Delete category \"" + name + "\"?";
    std::string detail;
    if (template_count > 0) {
        detail = "This will permanently delete " +
                 std::to_string(template_count) +
                 (template_count == 1 ? " template" : " templates") +
                 " in this category. Move them to another category first if "
                 "you want to keep them.";
    } else {
        detail = "This category is empty.";
    }
    auto dlg = Gtk::AlertDialog::create(title);
    dlg->set_detail(detail);
    dlg->set_buttons({"Cancel",
                      template_count > 0 ? "Delete All" : "Delete"});
    dlg->set_default_button(0);
    dlg->set_cancel_button(0);
    std::string name_copy = name;
    dlg->choose(*this, [this, dlg, name_copy](Glib::RefPtr<Gio::AsyncResult>& r) {
        try {
            int btn = dlg->choose_finish(r);
            if (btn != 1) return;
            if (templates::delete_category(name_copy)) {
                notify_changed();
            } else {
                show_toast("Delete category failed.");
            }
        } catch (...) {}
    });
}

// ── begin_rename_template ────────────────────────────────────────────────────
// Swap the name label for a temporary Gtk::Entry. Enter commits, Escape
// cancels, focus-out commits. The dialog rebuilds on success.

void ManageTemplatesDialog::begin_rename_template(
        const templates::TemplateEntry& e, Gtk::Label* label) {
    // Only one rename session at a time. Clicking ✎ on any row while
    // another rename entry is open is a no-op.
    if (m_rename_in_progress) return;
    auto* parent = dynamic_cast<Gtk::Box*>(label->get_parent());
    if (!parent) return;
    m_rename_in_progress = true;

    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(e.meta.name);
    entry->set_hexpand(true);
    entry->select_region(0, -1);

    // Insert the entry before the label, then hide the label.
    parent->insert_child_after(*entry, *label);
    label->set_visible(false);
    entry->grab_focus();

    // State shared with commit/cancel handlers
    auto committed = std::make_shared<bool>(false);
    templates::TemplateEntry e_copy = e;

    auto commit = [this, entry, label, parent, committed, e_copy]() {
        if (*committed) return;
        *committed = true;
        m_rename_in_progress = false;
        std::string new_name = entry->get_text();
        // Trim
        while (!new_name.empty() && std::isspace((unsigned char)new_name.front()))
            new_name.erase(new_name.begin());
        while (!new_name.empty() && std::isspace((unsigned char)new_name.back()))
            new_name.pop_back();

        // No-op paths: restore UI in place, no rebuild.
        if (new_name.empty() || new_name == e_copy.meta.name) {
            parent->remove(*entry);
            label->set_visible(true);
            return;
        }

        // Attempt the rename. On success, schedule rebuild via idle so the
        // widget tree replacement happens AFTER the current key/focus event
        // finishes propagating — touching the parent/entry/label here would
        // dangle when GTK returns through the controller chain.
        if (templates::rename_template(e_copy, new_name)) {
            Glib::signal_idle().connect_once([this]() { notify_changed(); });
        } else {
            // Rename failed — restore UI in place and toast.
            parent->remove(*entry);
            label->set_visible(true);
            show_toast("Rename failed.");
        }
    };
    auto cancel = [this, entry, label, parent, committed]() {
        if (*committed) return;
        *committed = true;
        m_rename_in_progress = false;
        parent->remove(*entry);
        label->set_visible(true);
    };

    // Enter commits; Escape or focus-leave cancels.
    // We deliberately do NOT commit on focus-out: clicking a sibling button
    // (✎, 🗑, ⭐) in the same row would otherwise race the button's own
    // handler against a rebuild triggered by the commit. User must press
    // Enter to confirm the new name.
    auto kc = Gtk::EventControllerKey::create();
    kc->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    kc->signal_key_pressed().connect(
        [commit, cancel](guint keyval, guint, Gdk::ModifierType) -> bool {
            if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
                commit();
                return true;
            }
            if (keyval == GDK_KEY_Escape) {
                cancel();
                return true;
            }
            return false;
        }, false);
    entry->add_controller(kc);

    // Focus-out: revert. See comment above.
    auto fc = Gtk::EventControllerFocus::create();
    fc->signal_leave().connect(cancel);
    entry->add_controller(fc);
}

// ── begin_rename_category ────────────────────────────────────────────────────

void ManageTemplatesDialog::begin_rename_category(const std::string& old_name,
                                                   Gtk::Label* label) {
    if (m_rename_in_progress) return;
    auto* parent = dynamic_cast<Gtk::Box*>(label->get_parent());
    if (!parent) return;
    m_rename_in_progress = true;

    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(old_name);
    entry->set_hexpand(true);
    entry->select_region(0, -1);

    parent->insert_child_after(*entry, *label);
    label->set_visible(false);
    entry->grab_focus();

    auto committed = std::make_shared<bool>(false);

    auto commit = [this, entry, label, parent, committed, old_name]() {
        if (*committed) return;
        *committed = true;
        m_rename_in_progress = false;
        std::string new_name = entry->get_text();
        while (!new_name.empty() && std::isspace((unsigned char)new_name.front()))
            new_name.erase(new_name.begin());
        while (!new_name.empty() && std::isspace((unsigned char)new_name.back()))
            new_name.pop_back();

        if (new_name.empty() || new_name == old_name) {
            parent->remove(*entry);
            label->set_visible(true);
            return;
        }
        if (templates::rename_category(old_name, new_name)) {
            Glib::signal_idle().connect_once([this]() { notify_changed(); });
        } else {
            parent->remove(*entry);
            label->set_visible(true);
            show_toast("Category rename failed (name may already exist).");
        }
    };
    auto cancel = [this, entry, label, parent, committed]() {
        if (*committed) return;
        *committed = true;
        m_rename_in_progress = false;
        parent->remove(*entry);
        label->set_visible(true);
    };

    auto kc = Gtk::EventControllerKey::create();
    kc->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    kc->signal_key_pressed().connect(
        [commit, cancel](guint keyval, guint, Gdk::ModifierType) -> bool {
            if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
                commit();
                return true;
            }
            if (keyval == GDK_KEY_Escape) {
                cancel();
                return true;
            }
            return false;
        }, false);
    entry->add_controller(kc);

    auto fc = Gtk::EventControllerFocus::create();
    fc->signal_leave().connect(cancel);
    entry->add_controller(fc);
}

// ── on_new_category ──────────────────────────────────────────────────────────

void ManageTemplatesDialog::on_new_category() {
    std::string created = templates::create_category("new-category");
    if (created.empty()) {
        show_toast("Could not create category.");
        return;
    }
    notify_changed();
    // The rebuild happens synchronously inside notify_changed; the new
    // category is now present in the list. We don't currently auto-enter
    // inline-edit mode on it (that'd require locating the newly rendered
    // label after rebuild — a future polish).
    show_toast("Created \"" + created + "\". Click ✎ next to it to rename.");
}

// ── set_default / clear_default ──────────────────────────────────────────────

void ManageTemplatesDialog::set_default(const templates::TemplateEntry& e) {
    if (templates::set_user_default(e)) {
        notify_changed();
    } else {
        show_toast("Could not set default.");
    }
}

void ManageTemplatesDialog::clear_default() {
    if (templates::clear_user_default()) {
        notify_changed();
    } else {
        show_toast("Could not clear default.");
    }
}

// ── notify_changed ───────────────────────────────────────────────────────────
// Rebuilds the list from disk and fires the parent callback. The rebuild is
// ALWAYS deferred via signal_idle because callers reach us from click /
// key-press handlers that live inside the widget tree we're about to tear
// down. Running rebuild() synchronously would invalidate the calling
// widget mid-event and crash GTK when it returns through the controller
// chain.
void ManageTemplatesDialog::notify_changed() {
    Glib::signal_idle().connect_once([this]() {
        rebuild();
        if (m_on_changed) m_on_changed();
    });
}

// ── show_toast ───────────────────────────────────────────────────────────────

void ManageTemplatesDialog::show_toast(const std::string& text) {
    m_toast_label.set_text(text);
    m_toast_revealer.set_reveal_child(true);
    // Auto-hide after 3 seconds
    Glib::signal_timeout().connect_once(
        [this]() { m_toast_revealer.set_reveal_child(false); }, 3000);
}

// ── Drag-and-drop ────────────────────────────────────────────────────────────
// The payload is a plain "<category>/<slug>" string. We deliberately do NOT
// stash a TemplateEntry pointer or reference in the payload: rebuild() is the
// source of truth, and a rebuild between drag-begin and drop would dangle any
// such pointer. On drop we re-scan to resolve the live entry.

bool ManageTemplatesDialog::resolve_user_entry(const std::string& payload,
                                               templates::TemplateEntry& out) const {
    auto slash = payload.find('/');
    if (slash == std::string::npos) return false;
    std::string cat  = payload.substr(0, slash);
    std::string slug = payload.substr(slash + 1);
    if (cat.empty() || slug.empty()) return false;

    auto all = templates::scan();
    for (const auto& e : all) {
        if (e.is_user && e.meta.category == cat && e.slug == slug) {
            out = e;
            return true;
        }
    }
    return false;
}

void ManageTemplatesDialog::setup_row_drag_source(
        Gtk::Widget* row, const templates::TemplateEntry& e) {
    auto src = Gtk::DragSource::create();
    src->set_actions(Gdk::DragAction::MOVE);

    // Snapshot the locator into the source — never read from `e` in the
    // prepare handler since it'd dangle across rebuilds.
    std::string payload = e.meta.category + "/" + e.slug;

    src->signal_prepare().connect(
        [payload](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
            Glib::Value<Glib::ustring> val;
            val.init(Glib::Value<Glib::ustring>::value_type());
            val.set(Glib::ustring(payload));
            return Gdk::ContentProvider::create(val);
        },
        false);

    src->signal_drag_begin().connect([row](const Glib::RefPtr<Gdk::Drag>&) {
        row->add_css_class("template-dragging");
    });
    src->signal_drag_end().connect(
        [row](const Glib::RefPtr<Gdk::Drag>&, bool) {
            row->remove_css_class("template-dragging");
        });

    row->add_controller(src);
}

void ManageTemplatesDialog::setup_category_drop_target(
        Gtk::Widget* section_widget, const std::string& category) {
    auto tgt = Gtk::DropTarget::create(G_TYPE_STRING, Gdk::DragAction::MOVE);

    tgt->signal_enter().connect(
        [section_widget](double, double) {
            section_widget->add_css_class("template-category-drop");
            return Gdk::DragAction::MOVE;
        },
        false);

    tgt->signal_leave().connect([section_widget]() {
        section_widget->remove_css_class("template-category-drop");
    });

    // Capture category by value — the lambda outlives this function call
    // but the string can't reference a local.
    std::string target_cat = category;
    tgt->signal_drop().connect(
        [this, section_widget, target_cat](const Glib::ValueBase& val,
                                           double, double) -> bool {
            section_widget->remove_css_class("template-category-drop");

            // The GValue holds a string via GObject's boxed-string plumbing.
            if (!G_VALUE_HOLDS_STRING(val.gobj())) return false;
            const char* raw = g_value_get_string(val.gobj());
            if (!raw) return false;
            std::string payload(raw);

            templates::TemplateEntry live;
            if (!resolve_user_entry(payload, live)) {
                show_toast("Could not find that template (was it deleted?).");
                return false;
            }

            // Same-category is handled as a no-op by the backend, but we
            // short-circuit here so we don't trigger a pointless rebuild.
            if (live.meta.category == target_cat) return false;

            if (templates::move_template(live, target_cat)) {
                // Rebuild is deferred via idle inside notify_changed() — the
                // drop handler is mid-controller-chain; tearing down our own
                // subtree synchronously would crash (same S48 / S60 class of
                // GTK bug). notify_changed() already schedules the rebuild
                // safely.
                notify_changed();
                return true;
            }
            show_toast("Move failed.");
            return false;
        },
        false);

    section_widget->add_controller(tgt);
}

} // namespace Curvz
