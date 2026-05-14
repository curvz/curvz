// ThemesPanel.cpp — s147 m3.
//
// Implementation lifts the substance from ThemesDialog (the dialog we're
// replacing) and reshapes it as an always-visible Content panel. The
// library mutation handlers (rename / dup / del / save / import / export)
// are near-identical in behaviour; the apply flow drops the dual-source
// radio mode and adds a confirmation step. See ThemesPanel.hpp for the
// design rationale.

#include "ThemesPanel.hpp"

#include "CommandHistory.hpp"
#include "CurvzDocument.hpp"
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"
#include "curvz/widgets/Button.hpp"
#include "curvz/widgets/CheckButton.hpp"
#include "curvz/widgets/Entry.hpp"
#include "curvz/widgets/ToggleButton.hpp"
#include "curvz_utils.hpp"
#include "theme/Theme.hpp"
#include "theme/ThemeIO.hpp"
#include "theme/ThemeLibrary.hpp"

#include <giomm/asyncresult.h>
#include <giomm/file.h>
#include <giomm/liststore.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/window.h>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <system_error>
#include <utility>

namespace Curvz {

ThemesPanel::ThemesPanel()
    : Gtk::Box(Gtk::Orientation::VERTICAL) {
  set_spacing(8);
  set_margin_start(6);
  set_margin_end(6);
  set_margin_top(4);
  set_margin_bottom(6);

  // SimpleActionGroup for kebab menu actions. Inserted into the panel
  // itself so the popover's detailed-action strings ("themes-io.<verb>")
  // resolve against this group regardless of where the panel sits in
  // the widget tree.
  m_actions = Gio::SimpleActionGroup::create();
  auto act_import = Gio::SimpleAction::create("import-themes");
  act_import->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_import_themes(); });
  m_actions->add_action(act_import);
  auto act_export = Gio::SimpleAction::create("export-themes");
  act_export->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_export_themes(); });
  m_actions->add_action(act_export);
  insert_action_group("themes-io", m_actions);

  build_ui();
}

ThemesPanel::~ThemesPanel() {
  disconnect_library_signals();
}

// ── Project wiring ───────────────────────────────────────────────────────────

void ThemesPanel::set_project(CurvzProject *project) {
  if (m_project == project) {
    refresh();
    return;
  }
  disconnect_library_signals();
  m_project = project;
  if (m_project) {
    connect_library_signals();
  }
  // Selection is per-project — clear when the project changes so a
  // stale id from the previous project doesn't appear "selected" in
  // the new one.
  m_selected_id.clear();
  refresh();
}

void ThemesPanel::connect_library_signals() {
  if (!m_project) return;
  m_conn_added = m_project->themes.signal_theme_added().connect(
      [this](const theme::ThemeId &) { refresh(); });
  m_conn_changed = m_project->themes.signal_theme_changed().connect(
      [this](const theme::ThemeId &) { refresh(); });
  m_conn_removed = m_project->themes.signal_theme_removed().connect(
      [this](const theme::ThemeId &) { refresh(); });
}

void ThemesPanel::disconnect_library_signals() {
  if (m_conn_added.connected()) m_conn_added.disconnect();
  if (m_conn_changed.connected()) m_conn_changed.disconnect();
  if (m_conn_removed.connected()) m_conn_removed.disconnect();
}

// ── Static layout ────────────────────────────────────────────────────────────
//
// Built once from the constructor. All dynamic content (the library
// rows, the targets rows) lives inside Gtk::Box children that get
// torn down + repopulated in refresh().
void ThemesPanel::build_ui() {
  // ── Library section ───────────────────────────────────────────────
  // Header: "Saved themes" + [+] save-as button + [⋮] kebab.
  {
    auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    hdr->set_spacing(6);

    auto *title = Gtk::make_managed<Gtk::Label>("Saved themes");
    title->set_xalign(0.0f);
    title->set_hexpand(true);
    title->add_css_class("inspector-section-title");
    hdr->append(*title);

    // [+] — save active doc as a new theme. Same shape as StylesPanel's
    // create button. Tooltip distinguishes it from generic "+" in
    // sibling panels (Library has its own +, Swatches has its own +).
    //
    // s213 m1 — substrate Button. Abbrev folded into the ctor; the
    // explicit set_name call below stays so widget_names_sync's regex
    // on curvz::utils::set_name keeps harvesting the long-name
    // annotation. (Same pattern as TranslateDialog's s212 m3 sweep.)
    m_btn_save = Gtk::make_managed<curvz::widgets::Button>("th_save");
    curvz::utils::set_name(m_btn_save, "th_save", "themes_panel_save_btn");
    m_btn_save->set_icon_name("list-add-symbolic");
    m_btn_save->set_tooltip_text("Save active document as a new theme…");
    // s147 m3: use set_has_frame(false) — canonical idiom in
    // LibraryPanel/StylesPanel headers for icon-only buttons. The
    // "flat" CSS class works in GTK theming but isn't the in-codebase
    // pattern.
    m_btn_save->set_has_frame(false);
    m_btn_save->signal_clicked().connect(
        [this]() { on_save_current_as_theme(); });
    hdr->append(*m_btn_save);

    // [⋮] — Import / Export popover. Lifted from ThemesDialog verbatim.
    m_btn_io_kebab = Gtk::make_managed<Gtk::MenuButton>();
    curvz::utils::set_name(m_btn_io_kebab, "th_iok",
                           "themes_panel_io_kebab_btn");
    m_btn_io_kebab->set_icon_name("view-more-symbolic");
    m_btn_io_kebab->set_tooltip_text("Import / Export themes");
    m_btn_io_kebab->add_css_class("flat");

    auto menu = Gio::Menu::create();
    menu->append("Import…", "themes-io.import-themes");
    menu->append("Export user themes…", "themes-io.export-themes");
    auto *popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
    popover->set_has_arrow(false);
    m_btn_io_kebab->set_popover(*popover);
    hdr->append(*m_btn_io_kebab);

    append(*hdr);
  }

  // Library list body — rows built by rebuild_library_list().
  {
    m_library_list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    m_library_list->set_spacing(2);
    m_library_list->set_margin_start(2);

    auto *scr = Gtk::make_managed<Gtk::ScrolledWindow>();
    // Vertical scrolling only — hint propagation off so the parent
    // doesn't need to grant infinite width to the rows. (s129 m9
    // gotcha: panel sits inside the right-pane ScrolledWindow which
    // already manages width; nested h-scroll is wrong here.)
    scr->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scr->set_propagate_natural_height(true);
    scr->set_min_content_height(80);
    scr->set_max_content_height(220);
    scr->set_child(*m_library_list);
    append(*scr);
  }

  // Subtle separator between library and targets — visual signal that
  // the two sections do different jobs.
  {
    auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    sep->set_margin_top(2);
    sep->set_margin_bottom(2);
    append(*sep);
  }

  // ── Targets section ───────────────────────────────────────────────
  // Header: "Apply to:" + select-all/none mini-buttons.
  {
    auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    hdr->set_spacing(6);

    auto *title = Gtk::make_managed<Gtk::Label>("Apply to:");
    title->set_xalign(0.0f);
    title->set_hexpand(true);
    title->add_css_class("inspector-section-title");
    hdr->append(*title);

    // s213 m1 — substrate Buttons. The targets-section select-all /
    // select-none mini-buttons are persistent build-once panel children
    // and worth registering for script-driven test flows that need to
    // batch-tick / untick the targets list. Picked fresh abbrevs in
    // the panel's `th_*` neighbourhood: `th_tall` (targets-all) and
    // `th_tnone` (targets-none).
    auto *btn_all = Gtk::make_managed<curvz::widgets::Button>("th_tall", "All");
    curvz::utils::set_name(btn_all, "th_tall", "themes_panel_targets_all_btn");
    btn_all->add_css_class("flat");
    btn_all->set_tooltip_text("Tick all documents");
    btn_all->signal_clicked().connect(
        [this]() { on_select_all_targets(); });
    hdr->append(*btn_all);

    auto *btn_none = Gtk::make_managed<curvz::widgets::Button>("th_tnone", "None");
    curvz::utils::set_name(btn_none, "th_tnone", "themes_panel_targets_none_btn");
    btn_none->add_css_class("flat");
    btn_none->set_tooltip_text("Untick all documents");
    btn_none->signal_clicked().connect(
        [this]() { on_select_no_targets(); });
    hdr->append(*btn_none);

    append(*hdr);
  }

  // Targets list — rows built by rebuild_targets_list().
  {
    m_targets_list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    m_targets_list->set_spacing(2);
    m_targets_list->set_margin_start(6);
    append(*m_targets_list);
  }

  // ── Footer ────────────────────────────────────────────────────────
  // Hint label + Apply button.
  {
    m_hint_label = Gtk::make_managed<Gtk::Label>(
        "Applying a theme is not undoable.");
    m_hint_label->set_xalign(0.0f);
    m_hint_label->add_css_class("dim-label");
    m_hint_label->set_margin_top(4);
    append(*m_hint_label);

    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_halign(Gtk::Align::END);
    row->set_margin_top(2);

    m_btn_apply = Gtk::make_managed<curvz::widgets::Button>("th_apply", "Apply");
    curvz::utils::set_name(m_btn_apply, "th_apply",
                           "themes_panel_apply_btn");
    m_btn_apply->add_css_class("suggested-action");
    m_btn_apply->set_sensitive(false);  // no source selected yet
    m_btn_apply->signal_clicked().connect(
        [this]() { on_apply_clicked(); });
    row->append(*m_btn_apply);

    append(*row);
  }
}

// ── Refresh ─────────────────────────────────────────────────────────────────

void ThemesPanel::refresh() {
  rebuild_library_list();
  rebuild_targets_list();
  update_apply_button_state();
}

void ThemesPanel::on_documents_changed() {
  // Targets list is the only section affected by doc-list changes.
  rebuild_targets_list();
}

// Build the library rows. Walks user themes only (built-ins are not
// shown — they live at the file-format level for round-trip and are
// not user-editable). Each row is name + 3 icon buttons; a click
// anywhere outside the icons selects that theme as the apply source.
void ThemesPanel::rebuild_library_list() {
  if (!m_library_list) return;

  // Clear existing rows.
  Gtk::Widget *child = m_library_list->get_first_child();
  while (child) {
    Gtk::Widget *next = child->get_next_sibling();
    m_library_list->remove(*child);
    child = next;
  }
  m_library_empty = nullptr;

  if (!m_project || m_project->themes.user_theme_count() == 0) {
    m_library_empty = Gtk::make_managed<Gtk::Label>(
        "No saved themes yet. Use [+] or Import to add one.");
    m_library_empty->set_xalign(0.0f);
    m_library_empty->set_wrap(true);
    m_library_empty->add_css_class("dim-label");
    m_library_empty->set_margin_top(4);
    m_library_list->append(*m_library_empty);
    return;
  }

  // One row per user theme, walking categories so display order
  // matches what the dialog used to show.
  for (const std::string &cat : m_project->themes.user_categories()) {
    for (const theme::Theme *t :
         m_project->themes.user_themes_in_category(cat)) {
      if (!t) continue;
      const theme::ThemeId id = t->header.id;  // copy for lambda capture

      // Row container. s147 m3 fix4: switched to a ToggleButton wrapping
      // the label as the click target — plain Button with a Label child
      // wasn't dispatching clicks in this layout (diagnostic in fix3
      // confirmed set_selected_source was never reached). ToggleButton
      // is the canonical GTK4 idiom for "selectable row" — it has
      // built-in active/inactive visual states (no custom CSS for
      // selection styling needed) and toggle semantics that match the
      // discovery-not-commit click model. "Guest in GTK's home"
      // principle: when GTK doesn't dispatch our event, the answer is
      // usually that we're using the wrong widget for the gesture.
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->set_spacing(4);
      row->add_css_class("themes-row");
      // s147 m3 fix7: explicit accent-tint via themes-row-selected.
      // ToggleButton's built-in active visual is too subtle in this
      // theme to read as "this is the apply source." A background
      // tint on the row is unambiguous.
      if (id == m_selected_id) {
        row->add_css_class("themes-row-selected");
      }

      std::string label = t->header.name;
      if (label.empty()) label = "(unnamed)";
      if (!t->header.category.empty()) {
        label += "  —  " + t->header.category;
      }
      auto *lbl = Gtk::make_managed<Gtk::Label>(label);
      lbl->set_xalign(0.0f);
      lbl->set_hexpand(true);
      lbl->set_ellipsize(Pango::EllipsizeMode::END);
      lbl->set_margin_start(4);

      // ToggleButton with the label as child. signal_toggled fires on
      // every state change; we guard against feedback loops via the
      // m_loading flag (set during programmatic set_active calls below).
      //
      // s213 m1 — unregistered substrate ToggleButton. This is the
      // **last raw `Gtk::ToggleButton` site in the codebase** — the
      // substrate sweep takes the ToggleButton type to 0 raw (second
      // type after Scale to hit zero). The per-theme-row build loop
      // has no per-row abbrev (every row would collide on a shared
      // name), which is the canonical first-costume use case for the
      // `unregistered_t{}` tag banked in s212 m1's CANON entry.
      auto *click_btn = Gtk::make_managed<curvz::widgets::ToggleButton>(
          curvz::scripting::unregistered_t{});
      click_btn->set_child(*lbl);
      click_btn->set_hexpand(true);
      click_btn->set_has_frame(false);
      // Pre-set the toggle state to match selection BEFORE connecting
      // signal_toggled, so the initial set_active doesn't fire the
      // handler and we don't get a spurious set_selected_source call
      // on every rebuild.
      bool is_selected = (id == m_selected_id);
      click_btn->set_active(is_selected);
      click_btn->signal_toggled().connect([this, id, click_btn]() {
        // ToggleButton fires on every state flip. We translate flips to
        // set_selected_source(id) — set_selected_source itself toggles
        // selection (clicking the active row deselects). The two layers
        // of toggling cancel cleanly: clicking row A selects A; clicking
        // row A again toggles ToggleButton off, which calls
        // set_selected_source(A), which sees m_selected_id==A and clears.
        // Then rebuild_library_list rebuilds with all toggles inactive.
        // Clicking row B with A active: ToggleButton B goes active (calls
        // set_selected_source(B), m_selected_id was A, set to B, rebuild
        // — A's toggle reappears inactive, B's appears active).
        //
        // Edge case: rebuild may set_active(false) on this same
        // ToggleButton, which would re-fire signal_toggled. Guard via
        // checking that the toggle's current active state actually
        // disagrees with our notion of selection — if they match,
        // somebody (probably us) just set it programmatically and we
        // should ignore.
        bool now_active = click_btn->get_active();
        bool we_think_selected = (id == m_selected_id);
        if (now_active == we_think_selected) return;  // programmatic
        set_selected_source(id);
      });
      row->append(*click_btn);

      // Per-row icon buttons. s183 m2: edit / dup / del.
      //
      // s213 m1 — unregistered substrate Button. The lambda mints
      // three icon buttons per theme row × N theme rows; no per-row
      // abbrev is available (every row would collide). Canonical
      // first-costume use case for the `unregistered_t{}` tag — same
      // shape as s211 m2's NewDocumentDialog preset-button loops and
      // s209 m1's ContextBar `add_btn` helper.
      auto make_icon_btn = [](const char *icon_name, const char *tooltip) {
        auto *b = Gtk::make_managed<curvz::widgets::Button>(
            curvz::scripting::unregistered_t{});
        b->set_icon_name(icon_name);
        b->set_tooltip_text(tooltip);
        b->add_css_class("flat");
        return b;
      };
      // Per-row icon buttons (edit / dup / del). s183 m2: the
      // "Rename" inline-prompt button became "Edit" — clicking it
      // emits signal_request_theme_editor and the host opens the
      // full ThemeEditDialog. The dialog covers name editing plus
      // the property surfaces, so the dedicated rename mini-dialog
      // is redundant. on_rename_theme is retained as the
      // implementation behind a name-only undo path; nothing on the
      // panel itself drives it any longer.
      auto *btn_edit = make_icon_btn("document-edit-symbolic", "Edit theme");
      btn_edit->signal_clicked().connect(
          [this, id]() { on_edit_theme(id); });
      row->append(*btn_edit);
      auto *btn_dup = make_icon_btn("edit-copy-symbolic", "Duplicate");
      btn_dup->signal_clicked().connect(
          [this, id]() { on_duplicate_theme(id); });
      row->append(*btn_dup);
      auto *btn_del = make_icon_btn("user-trash-symbolic", "Delete");
      btn_del->signal_clicked().connect(
          [this, id]() { on_delete_theme(id); });
      row->append(*btn_del);

      m_library_list->append(*row);
    }
  }

  // If the previously-selected id is no longer in the library (e.g.
  // it was just deleted), clear selection so the apply button doesn't
  // dangle on a stale id.
  if (!m_selected_id.empty() && !m_project->themes.find_theme(m_selected_id)) {
    m_selected_id.clear();
  }
}

// Targets list — one CheckButton per document in the project. All
// rows are sensitive (no source-mode gating). Active doc is visually
// marked with "(active)" suffix so the user knows which doc is the
// canvas-current one. All checked by default; the user can untick
// before clicking Apply.
void ThemesPanel::rebuild_targets_list() {
  if (!m_targets_list) return;

  // Capture which checks the user had ticked, so a doc-list rebuild
  // (e.g. doc rename) doesn't reset the user's selection. Indexed by
  // doc index. Defaults to true for newly-added docs.
  std::vector<bool> prev_checked;
  prev_checked.resize(m_target_checks.size(), true);
  for (std::size_t i = 0; i < m_target_checks.size(); ++i) {
    if (m_target_checks[i]) prev_checked[i] = m_target_checks[i]->get_active();
  }

  // Clear existing rows.
  Gtk::Widget *child = m_targets_list->get_first_child();
  while (child) {
    Gtk::Widget *next = child->get_next_sibling();
    m_targets_list->remove(*child);
    child = next;
  }
  m_target_checks.clear();

  if (!m_project || m_project->documents.empty()) {
    auto *empty = Gtk::make_managed<Gtk::Label>("No documents.");
    empty->set_xalign(0.0f);
    empty->add_css_class("dim-label");
    m_targets_list->append(*empty);
    update_apply_button_state();
    return;
  }

  const int active_idx = active_doc_index();
  m_target_checks.reserve(m_project->documents.size());

  for (std::size_t i = 0; i < m_project->documents.size(); ++i) {
    auto &doc_ptr = m_project->documents[i];
    if (!doc_ptr) {
      m_target_checks.push_back(nullptr);
      continue;
    }

    std::string label = doc_display_name(*doc_ptr, i);
    if (static_cast<int>(i) == active_idx) {
      label += "  (active)";
    }
    // s213 m1 — unregistered substrate CheckButton. Per-doc rebuild
    // loop — every CheckButton would collide on a shared abbrev. The
    // substrate gains a parallel `CheckButton(unregistered_t, label)`
    // ctor for this site (one decl line in CheckButton.hpp, one ctor
    // body in CheckButton.cpp forwarding to ScriptableWidget's tagged
    // ctor — same shape as the prior five additions). CheckButton is
    // the sixth substrate type to gain the tag, after Button (s209
    // m1), ToggleButton (s209 m2), Entry (s211 m1), SpinButton (s211
    // m2), and DropDown (s212 m2).
    auto *cb = Gtk::make_managed<curvz::widgets::CheckButton>(
        curvz::scripting::unregistered_t{}, label);
    bool initial = (i < prev_checked.size()) ? prev_checked[i] : true;
    cb->set_active(initial);
    cb->signal_toggled().connect([this]() { update_apply_button_state(); });
    m_targets_list->append(*cb);
    m_target_checks.push_back(cb);
  }

  update_apply_button_state();
}

void ThemesPanel::update_apply_button_state() {
  if (!m_btn_apply) return;
  // Apply is sensitive only when:
  //   1. A library row is selected as source.
  //   2. At least one target is ticked.
  bool has_source = !m_selected_id.empty();
  bool has_target = false;
  for (Gtk::CheckButton *cb : m_target_checks) {
    if (cb && cb->get_active()) { has_target = true; break; }
  }
  m_btn_apply->set_sensitive(has_source && has_target);
}

void ThemesPanel::set_selected_source(const theme::ThemeId &id) {
  if (m_selected_id == id) {
    // Toggle off when same row clicked twice — explicit deselect.
    m_selected_id.clear();
  } else {
    m_selected_id = id;
  }
  // Cheap full library rebuild to update the active state on the row's
  // ToggleButton. Could be optimised by walking children, but the
  // list is small (sub-100) and rebuild is the established panel idiom
  // (mirrors SwatchesPanel's selection handling).
  rebuild_library_list();
  update_apply_button_state();
}

// ── Apply with confirmation ──────────────────────────────────────────────────

void ThemesPanel::on_apply_clicked() {
  if (!m_project || m_selected_id.empty()) return;

  const theme::Theme *src = m_project->themes.find_theme(m_selected_id);
  if (!src) {
    LOG_WARN("ThemesPanel::on_apply_clicked: selected theme '{}' not found",
             m_selected_id);
    return;
  }

  // Collect target indices + display names BEFORE the confirmation
  // dialog so any doc-list mutation between confirm-show and confirm-
  // accept (impossible while modal, defensive) doesn't move targets
  // out from under us. We re-validate at apply time anyway.
  std::vector<std::size_t> targets;
  std::vector<std::string> target_names;
  for (std::size_t i = 0; i < m_target_checks.size(); ++i) {
    Gtk::CheckButton *cb = m_target_checks[i];
    if (!cb || !cb->get_active()) continue;
    if (i >= m_project->documents.size()) break;
    auto &doc_ptr = m_project->documents[i];
    if (!doc_ptr) continue;
    targets.push_back(i);
    target_names.push_back(doc_display_name(*doc_ptr, i));
  }

  if (targets.empty()) {
    // update_apply_button_state should have prevented this, but
    // defensive — surface a small alert rather than silent.
    auto root = root_window();
    auto dlg = Gtk::AlertDialog::create("No targets selected");
    dlg->set_detail(
        "Tick one or more documents in the Apply to: list, "
        "then click Apply again.");
    dlg->set_buttons({"OK"});
    dlg->set_default_button(0);
    if (root) dlg->show(*root);
    return;
  }

  const std::string source_label =
      src->header.name.empty() ? std::string("(unnamed theme)")
                                : src->header.name;

  // Confirmation modal — lists source + N targets, NOT-undoable warning.
  std::string detail;
  detail = "Theme \"" + source_label + "\" will overwrite settings in:\n";
  for (const std::string &name : target_names) {
    detail += "  \xE2\x80\xA2 " + name + "\n";  // U+2022
  }
  if (!detail.empty() && detail.back() == '\n') detail.pop_back();
  detail += "\n\nThis action is not undoable.";

  auto *root = root_window();
  auto alert = Gtk::AlertDialog::create("Apply theme?");
  alert->set_detail(detail);
  alert->set_buttons({"Cancel", "Apply"});
  alert->set_cancel_button(0);
  alert->set_default_button(1);

  // Snapshot the source theme value into the lambda (defensive against
  // mid-flight library mutations — same pattern as on_delete_theme).
  theme::Theme src_snapshot = *src;

  auto do_apply =
      [this, alert, src_snapshot, targets, target_names, source_label]
      (Glib::RefPtr<Gio::AsyncResult> &result) {
        try {
          int btn = alert->choose_finish(result);
          if (btn != 1) return;  // cancelled
          if (!m_project) return;

          int active_idx = active_doc_index();
          bool active_was_target = false;
          int applied_count = 0;

          // Re-validate targets at apply time — list may have shifted
          // (won't happen while modal, but defensive).
          for (std::size_t idx : targets) {
            if (idx >= m_project->documents.size()) continue;
            auto &doc_ptr = m_project->documents[idx];
            if (!doc_ptr) continue;
            // s149 m1: pass the current motif so apply picks the matching
            // colour pair from the theme's MotifSettings sub-bundle.
            // Storage of the motif itself migrates to AppPreferences in
            // sub-ship 2; for now it lives on m_project->motif.
            theme::apply_theme_to_doc(src_snapshot, *doc_ptr,
                                      m_project->motif);
            ++applied_count;
            if (static_cast<int>(idx) == active_idx) active_was_target = true;
          }

          // Project-snap mirror — see ThemesDialog::on_apply for
          // rationale. If the active doc was one of the targets,
          // sync project->snap so the next save round-trips correctly.
          if (active_was_target && active_idx >= 0 &&
              active_idx <
                  static_cast<int>(m_project->documents.size()) &&
              m_project->documents[active_idx]) {
            m_project->snap = m_project->documents[active_idx]->snap;
          }

          LOG_INFO("ThemesPanel::on_apply_clicked: applied '{}' to {} "
                   "document(s)",
                   source_label, applied_count);

          if (m_on_changed) m_on_changed();
        } catch (const Glib::Error &) {
          // User dismissed via window close or async error — silent.
        }
      };

  if (root) {
    alert->choose(*root, do_apply);
  }
}

// ── Library mutation handlers ────────────────────────────────────────────────
//
// Lifted from ThemesDialog with minor adjustments: parent window is
// resolved via root_window() instead of a held m_window, and selection
// state (m_selected_id) is maintained across mutations where it makes
// sense (delete clears selection if the deleted theme was the source).

void ThemesPanel::on_edit_theme(const theme::ThemeId &id) {
  // s183 m2 — emit signal_request_theme_editor so the host can open
  // the dialog. The closure carries the captured before-snapshot and
  // the per-OK callback that pushes UpdateThemeCommand. Mirrors
  // StylesPanel::open_style_editor's signal-emit shape.
  if (!m_project) return;
  const theme::Theme *current = m_project->themes.find_theme(id);
  if (!current) {
    LOG_WARN("ThemesPanel::on_edit_theme: theme '{}' not found", id);
    return;
  }
  if (m_project->themes.is_built_in(id)) {
    LOG_WARN("ThemesPanel::on_edit_theme: '{}' is built-in, refusing", id);
    return;
  }

  theme::Theme before = *current;
  // Capture id by value into the closure — m_project->themes can
  // reorder under us between the emit and the OK callback firing.
  // before is captured by value too so the command's snapshot is
  // stable independent of subsequent panel state.
  m_sig_request_theme_editor.emit(
      before,
      [this, id, before](theme::Theme after) {
        if (!m_project) return;
        // Defensive: theme might have been deleted while the dialog
        // was open. Skip the command push on a stale id rather than
        // crash inside UpdateThemeCommand::apply.
        if (!m_project->themes.find_theme(id)) {
          LOG_WARN("ThemesPanel::on_edit_theme commit: theme '{}' "
                   "no longer in library, dropping edit", id);
          return;
        }
        // No-change short-circuit. Theme's operator== covers every
        // editable field by construction (see Theme.hpp comment) —
        // a bare-OK with no edits should not pollute the undo stack.
        if (after == before) {
          LOG_DEBUG("ThemesPanel::on_edit_theme commit: no changes");
          return;
        }
        if (m_history) {
          auto cmd = std::make_unique<UpdateThemeCommand>(
              &m_project->themes, id, before, after, "Edit theme");
          cmd->execute();
          m_history->push(std::move(cmd));
        } else {
          theme::Theme copy = after;
          copy.header.id = id;
          m_project->themes.update_theme(id, std::move(copy));
        }
        notify_changed();
      });
}

void ThemesPanel::on_rename_theme(const theme::ThemeId &id) {
  if (!m_project) return;
  const theme::Theme *current = m_project->themes.find_theme(id);
  if (!current) {
    LOG_WARN("ThemesPanel::on_rename_theme: theme '{}' not found", id);
    return;
  }
  if (m_project->themes.is_built_in(id)) {
    LOG_WARN("ThemesPanel::on_rename_theme: '{}' is built-in", id);
    return;
  }

  Gtk::Window *root = root_window();
  if (!root) return;

  auto *prompt = Gtk::make_managed<Gtk::Window>();
  prompt->set_title("Rename theme");
  prompt->set_modal(true);
  prompt->set_resizable(false);
  prompt->set_transient_for(*root);
  curvz::utils::apply_motif_class_from_parent(*prompt, *root);
  prompt->set_default_size(360, -1);

  auto *box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  box->set_spacing(8);
  box->set_margin_top(12);
  box->set_margin_bottom(12);
  box->set_margin_start(12);
  box->set_margin_end(12);

  auto *lbl = Gtk::make_managed<Gtk::Label>("New name:");
  lbl->set_xalign(0.0f);
  box->append(*lbl);

  // s213 m1 — unregistered substrate widgets for the per-show
  // transient rename prompt. Same costume as SwatchesPanel's
  // s211 m2 prompt_text + StylesPanel's s212 m2 prompt_text builders:
  // the Gtk::Window itself isn't substrate-scope, but the inner
  // Entry + Cancel/OK Buttons gain the tag to ride the universal
  // verb / lifecycle machinery without claiming shared abbrevs that
  // would collide across repeated prompts.
  auto *entry = Gtk::make_managed<curvz::widgets::Entry>(
      curvz::scripting::unregistered_t{});
  entry->set_text(current->header.name);
  entry->select_region(0, -1);
  box->append(*entry);

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_spacing(6);
  btn_row->set_halign(Gtk::Align::END);
  auto *btn_cancel = Gtk::make_managed<curvz::widgets::Button>(
      curvz::scripting::unregistered_t{}, "Cancel");
  auto *btn_ok = Gtk::make_managed<curvz::widgets::Button>(
      curvz::scripting::unregistered_t{}, "Rename");
  btn_ok->add_css_class("suggested-action");
  btn_row->append(*btn_cancel);
  btn_row->append(*btn_ok);
  box->append(*btn_row);

  prompt->set_child(*box);

  theme::Theme before = *current;

  auto commit = [this, prompt, entry, id, before]() mutable {
    if (!m_project) { prompt->close(); return; }
    const theme::Theme *live = m_project->themes.find_theme(id);
    if (!live) { prompt->close(); return; }

    std::string name = entry->get_text();
    while (!name.empty() &&
           std::isspace(static_cast<unsigned char>(name.back()))) {
      name.pop_back();
    }
    std::size_t lead = 0;
    while (lead < name.size() &&
           std::isspace(static_cast<unsigned char>(name[lead]))) {
      ++lead;
    }
    if (lead > 0) name.erase(0, lead);

    if (name.empty() || name == live->header.name) {
      prompt->close();
      return;
    }
    if (m_project->themes.has_user_name(name)) {
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

void ThemesPanel::on_duplicate_theme(const theme::ThemeId &id) {
  if (!m_project) return;
  const theme::Theme *src = m_project->themes.find_theme(id);
  if (!src) {
    LOG_WARN("ThemesPanel::on_duplicate_theme: theme '{}' not found", id);
    return;
  }

  theme::Theme copy = *src;
  copy.header.id = "";  // library mints fresh
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

void ThemesPanel::on_delete_theme(const theme::ThemeId &id) {
  if (!m_project) return;
  const theme::Theme *current = m_project->themes.find_theme(id);
  if (!current) return;
  if (m_project->themes.is_built_in(id)) return;

  Gtk::Window *root = root_window();
  if (!root) return;

  auto alert = Gtk::AlertDialog::create(
      "Delete theme \"" + current->header.name + "\"?");
  alert->set_detail("This cannot be undone via the menu, "
                    "but Ctrl+Z will restore it.");
  alert->set_buttons({"Cancel", "Delete"});
  alert->set_cancel_button(0);
  alert->set_default_button(1);

  theme::Theme snapshot = *current;
  theme::ThemeId id_copy = id;

  alert->choose(*root,
      [this, alert, id_copy, snapshot]
      (Glib::RefPtr<Gio::AsyncResult> &result) {
        try {
          int btn = alert->choose_finish(result);
          if (btn != 1) return;
          if (!m_project) return;

          // Clear selection if the deleted theme was the apply source.
          if (m_selected_id == id_copy) m_selected_id.clear();

          if (m_history) {
            auto cmd = std::make_unique<RemoveThemeCommand>(
                &m_project->themes, snapshot, "Delete theme");
            cmd->execute();
            m_history->push(std::move(cmd));
          } else {
            m_project->themes.remove_theme(id_copy);
          }
          notify_changed();
        } catch (const Glib::Error &) {
          // User dismissed via window close — silent.
        }
      });
}

void ThemesPanel::on_save_current_as_theme() {
  if (!m_project) return;
  CurvzDocument *doc = m_project->active_doc();
  if (!doc) {
    LOG_INFO("ThemesPanel::on_save_current_as_theme: no active doc");
    return;
  }

  // Capture at click time so the user can't sneak edits in between
  // click and name-commit. s149 m1: pass current motif so the doc's
  // colours go into the matching slot of the theme's MotifSettings;
  // the off-mode slot gets factory defaults.
  theme::Theme captured =
      theme::capture_theme_from_doc(*doc, m_project->motif);

  Gtk::Window *root = root_window();
  if (!root) return;

  auto *prompt = Gtk::make_managed<Gtk::Window>();
  prompt->set_title("Save theme");
  prompt->set_modal(true);
  prompt->set_resizable(false);
  prompt->set_transient_for(*root);
  curvz::utils::apply_motif_class_from_parent(*prompt, *root);
  prompt->set_default_size(360, -1);

  auto *box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  box->set_spacing(8);
  box->set_margin_top(12);
  box->set_margin_bottom(12);
  box->set_margin_start(12);
  box->set_margin_end(12);

  auto *lbl = Gtk::make_managed<Gtk::Label>("Theme name:");
  lbl->set_xalign(0.0f);
  box->append(*lbl);

  // s213 m1 — unregistered substrate widgets for the per-show
  // transient save-as prompt. Sibling of on_rename_theme above.
  auto *entry = Gtk::make_managed<curvz::widgets::Entry>(
      curvz::scripting::unregistered_t{});
  entry->set_hexpand(true);
  // Suggest a fresh default name. Walks "Theme N" against existing
  // user names — same as the dialog used to do.
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

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_spacing(6);
  btn_row->set_halign(Gtk::Align::END);
  auto *btn_cancel = Gtk::make_managed<curvz::widgets::Button>(
      curvz::scripting::unregistered_t{}, "Cancel");
  auto *btn_ok = Gtk::make_managed<curvz::widgets::Button>(
      curvz::scripting::unregistered_t{}, "Save");
  btn_ok->add_css_class("suggested-action");
  btn_row->append(*btn_cancel);
  btn_row->append(*btn_ok);
  box->append(*btn_row);
  prompt->set_child(*box);

  auto commit = [this, prompt, entry, captured]() mutable {
    if (!m_project) { prompt->close(); return; }
    std::string name = entry->get_text();
    while (!name.empty() &&
           std::isspace(static_cast<unsigned char>(name.back()))) {
      name.pop_back();
    }
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
      if (name.empty()) name = "Theme";
    } else if (m_project->themes.has_user_name(name)) {
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
    to_add.header.id = "";  // library mints
    to_add.header.name = name;

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
  entry->signal_activate().connect(commit);

  prompt->present();
  entry->grab_focus();
}

void ThemesPanel::on_import_themes() {
  if (!m_project) return;
  Gtk::Window *root = root_window();
  if (!root) return;

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

  dialog->open(*root,
      [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
        try {
          auto file = dialog->open_finish(result);
          if (!file) return;
          if (!m_project) return;
          std::string path = file->get_path();

          auto loaded = theme::theme_io::load_path(path);
          if (!loaded) {
            LOG_WARN("ThemesPanel::on_import_themes: load failed for '{}'",
                     path);
            return;
          }

          std::size_t added = theme::theme_io::import_themes_into_library(
              m_project->themes, m_history, *loaded);
          if (added == 0) {
            LOG_INFO("ThemesPanel::on_import_themes: '{}' contained no "
                     "importable themes",
                     path);
            return;
          }

          notify_changed();
        } catch (const Glib::Error &) {
          // User cancelled — silent.
        }
      });
}

void ThemesPanel::on_export_themes() {
  if (!m_project) return;
  Gtk::Window *root = root_window();
  if (!root) return;

  auto snapshot = theme::theme_io::snapshot_user_tier(m_project->themes);
  if (snapshot.empty()) {
    LOG_INFO("ThemesPanel::on_export_themes: user tier is empty, "
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

  namespace fs = std::filesystem;
  const std::string user_dir = theme::theme_io::user_themes_dir();
  std::error_code mkdir_ec;
  fs::create_directories(user_dir, mkdir_ec);
  if (!mkdir_ec) {
    dialog->set_initial_folder(Gio::File::create_for_path(user_dir));
  } else {
    LOG_WARN("ThemesPanel::on_export_themes: cannot create '{}': {} "
             "(skipping initial-folder hint)",
             user_dir, mkdir_ec.message());
  }

  dialog->save(*root,
      [this, dialog, snapshot](Glib::RefPtr<Gio::AsyncResult> &result) {
        try {
          auto file = dialog->save_finish(result);
          if (!file) return;
          std::string path = file->get_path();
          if (path.size() < 5 ||
              path.compare(path.size() - 5, 5, ".json") != 0) {
            path += ".json";
          }
          if (!theme::theme_io::write_path(path, snapshot)) {
            LOG_WARN("ThemesPanel::on_export_themes: write failed for '{}'",
                     path);
          }
          // No library state changed — no notify_changed needed.
        } catch (const Glib::Error &) {
          // User cancelled — silent.
        }
      });
}

// ── Targets convenience ─────────────────────────────────────────────────────

void ThemesPanel::on_select_all_targets() {
  for (Gtk::CheckButton *cb : m_target_checks) {
    if (cb) cb->set_active(true);
  }
  update_apply_button_state();
}

void ThemesPanel::on_select_no_targets() {
  for (Gtk::CheckButton *cb : m_target_checks) {
    if (cb) cb->set_active(false);
  }
  update_apply_button_state();
}

// ── Helpers ─────────────────────────────────────────────────────────────────

Gtk::Window *ThemesPanel::root_window() {
  return dynamic_cast<Gtk::Window *>(get_root());
}

std::string ThemesPanel::doc_display_name(const CurvzDocument &doc,
                                          std::size_t fallback_index) {
  if (!doc.filename.empty()) return doc.filename;
  return "Untitled " + std::to_string(fallback_index + 1);
}

int ThemesPanel::active_doc_index() const {
  if (!m_project) return -1;
  return m_project->active_doc_index;
}

void ThemesPanel::notify_changed() {
  // Refresh first so the UI catches up immediately even if the host
  // callback is null.
  refresh();
  if (m_on_changed) m_on_changed();
}

} // namespace Curvz
