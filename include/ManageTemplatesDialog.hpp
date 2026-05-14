#pragma once
#include "TemplateLibrary.hpp"
#include <functional>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/revealer.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/window.h>
#include <string>
#include <vector>

namespace curvz::widgets { class Button; }

namespace Curvz {

// ── ManageTemplatesDialog ─────────────────────────────────────────────────────
// Brave-bookmark-manager-style template browser. Single scrollable pane,
// one collapsible section per category, templates as rows under each.
//
// Per-template interactions:
//   Double-click name  — inline rename (user bundles only). Enter commits,
//                        Escape / focus-leave cancels.
//   Drag row           — move the template to another category. Drop on a
//                        category header or the body of another section to
//                        move it there. Same-category drops are a no-op.
//                        System bundles are not draggable.
//   ⭐ Set as Default  — writes the user default pointer
//   🗑  Delete          — AlertDialog confirm, then TemplateLibrary::remove
//
// Per-category actions (next to each category header):
//   ✎  Rename           — inline text entry swap on the category label
//   🗑  Delete           — AlertDialog confirm (worded to warn about contained
//                         templates if any), then TemplateLibrary::delete_category
//
// Global:
//   [+ New Category]    — creates an empty user category with a placeholder
//                         name; user edits in place.
//
// System templates appear in the list with 🗑 greyed out and are not
// draggable; ⭐ is still enabled (a system template can be set as the user
// default). System-seeded category names (see templates::system_categories())
// have their 🗑 disabled and are not renameable, but they are valid drop
// targets for moving user templates into.
//
// On any successful mutation the dialog refreshes from disk via rebuild()
// to reflect the new state. A signal is emitted so the parent window can
// update NewDocumentDialog's template picker without re-scanning itself.

class ManageTemplatesDialog : public Gtk::Window {
public:
    // Called after any successful mutation. Caller should re-scan templates
    // and update any picker UIs they own.
    using ChangedCb = std::function<void()>;

    ManageTemplatesDialog();

    // Show modal, attached to parent. on_changed fires on any successful
    // template/category mutation.
    //
    // motif tells the dialog which of each bundle's two PNG thumbnails to
    // display. If the requested motif's PNG doesn't exist yet (legacy bundle
    // pre-m4, or new bundle whose other-motif variant hasn't regen'd), the
    // dialog falls back to the available variant. The ManageTemplates dialog
    // doesn't need to lazy-regenerate — it's an admin tool, not a picker —
    // and a slightly mismatched thumb here is acceptable. (NewDocumentDialog
    // is where regen happens, and that path keeps the cache fresh on its own.)
    void show(Gtk::Window& parent, templates::MotifTag motif,
              ChangedCb on_changed);

private:
    // ── Layout ────────────────────────────────────────────────────────────
    void build_layout();
    void rebuild();               // clear + redraw from fresh scan

    // ── Category rendering ────────────────────────────────────────────────
    // A "category section" is:
    //   [header row: chevron | name label/entry | spacer | ✎ | 🗑]
    //   [revealer: vbox of template rows]
    void append_category_section(const std::string& category,
                                 const std::vector<templates::TemplateEntry>& entries,
                                 bool is_system_seeded);

    // ── Template rendering ────────────────────────────────────────────────
    void append_template_row(Gtk::Box* parent_box,
                             const templates::TemplateEntry& e,
                             bool is_user_default);

    // ── Actions ───────────────────────────────────────────────────────────
    void confirm_delete_template(const templates::TemplateEntry& e);
    void confirm_delete_category(const std::string& name,
                                 int template_count);
    void begin_rename_template(const templates::TemplateEntry& e,
                               Gtk::Label* label);
    void begin_rename_category(const std::string& old_name,
                               Gtk::Label* label);
    void on_new_category();
    void set_default(const templates::TemplateEntry& e);
    void clear_default();

    // ── Drag-and-drop ─────────────────────────────────────────────────────
    // A template row is a drag source. Payload is "<category>/<slug>" as a
    // string; the drop handler looks the entry up via a fresh scan() rather
    // than carrying a pointer across a rebuild.
    //
    // A category section (its whole top-level box) is a drop target for
    // template moves. Dropping on the same category is a no-op (the backend
    // handles this case).
    void setup_row_drag_source(Gtk::Widget* row,
                               const templates::TemplateEntry& e);
    void setup_category_drop_target(Gtk::Widget* section_widget,
                                    const std::string& category);

    // Resolve a "<category>/<slug>" payload string back to a live
    // TemplateEntry via fresh scan(). Returns false if the slug is gone
    // (e.g. deleted out from under us) or if it refers to a system bundle
    // (moves are user-only).
    bool resolve_user_entry(const std::string& payload,
                            templates::TemplateEntry& out) const;

    // Notify parent that something changed so it can refresh its template
    // picker. Local UI always also rebuilds itself.
    void notify_changed();

    // Transient toast: briefly pops up a Revealer with a message.
    // Used for "This is a protected system template" feedback.
    void show_toast(const std::string& text);

    // ── Widgets ───────────────────────────────────────────────────────────
    Gtk::Box            m_root{Gtk::Orientation::VERTICAL};
    Gtk::Box            m_header_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Label          m_title_label;
    // s214: substrate Button. Constructed in build_layout(); make_managed
    // gives Gtk ownership, dialog never deletes it explicitly.
    curvz::widgets::Button* m_btn_new_cat = nullptr;
    Gtk::ScrolledWindow m_scroll;
    Gtk::Box            m_list{Gtk::Orientation::VERTICAL};
    Gtk::Box            m_footer{Gtk::Orientation::HORIZONTAL};
    // s214: substrate Button. Same ownership shape as m_btn_new_cat.
    curvz::widgets::Button* m_btn_close = nullptr;

    // Toast
    Gtk::Revealer       m_toast_revealer;
    Gtk::Label          m_toast_label;

    // ── State ─────────────────────────────────────────────────────────────
    bool      m_built = false;
    // True while an inline rename entry is live somewhere in the list.
    // Guards against starting a second rename (e.g. double-clicking a
    // different row) while one is already open — two entries can't coexist
    // cleanly because the restore-UI step assumes a 1:1 entry/label pair
    // per rename session.
    bool      m_rename_in_progress = false;
    // Motif under which to display thumbnails. Set by show(). Used by
    // append_template_row() to pick between thumb_path_dark/light.
    templates::MotifTag m_motif = templates::MotifTag::Dark;
    ChangedCb m_on_changed;
};

} // namespace Curvz
