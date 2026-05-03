#pragma once
// HelpWindow — non-modal in-app manual viewer.
//
// S100 m1 scaffolding:
//   • Sidebar of topics on the left (collapsible outline as of s129 m7)
//   • Markdown content rendered as a stack of GTK widgets on the right
//   • Topics are bundled .md files compiled into the binary via gresource
//     under /com/curvz/app/help/
//   • Markdown parsing via libcmark (system dep, MIT-licensed)
//   • Block-to-widget renderer: each cmark AST node becomes a Gtk widget
//     (Label / Picture / Frame / Box) so screenshots can render inline,
//     which Pango markup alone cannot do.
//
// s129 m6: sidebar started carrying the full outline skeleton with leaf
//   availability flags so undrafted pages are visible-but-inert.
// s129 m7: sidebar is now a proper outline tree using the same disclosure
//   idiom as PropertiesPanel — Chapter and Group rows are
//   click-to-expand headers (▾/▸ + title), Leaf rows are flat buttons
//   inside the body of their parent. Open/closed state is keyed by row
//   title and persisted across sessions in the app config (alongside
//   inspector section state and last-folder memory). Default is "all
//   collapsed except chapter 1" so opening the manual lands on Welcome.
//
// Open via Help menu item, F1 keybinding, or HelpWindow::show(parent).
// set_hide_on_close(true) so the same instance is reused across opens
// (matches the ShortcutsDialog pattern).

#include <gtkmm/box.h>
#include <gtkmm/label.h>         // s132 m1 — named in m_section_arrow
#include <gtkmm/paned.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/searchentry.h>   // s132 m1 — sidebar search box
#include <gtkmm/window.h>
#include <map>
#include <string>
#include <unordered_map>         // s132 m1 — body-text cache for search
#include <vector>

namespace Curvz {

class HelpWindow : public Gtk::Window {
public:
  HelpWindow();
  void show(Gtk::Window &parent);

  // ── Targeted-open entry point (s132 m1) ─────────────────────────────────
  // Open the manual to a specific leaf identified by its gresource path
  // (e.g. "/com/curvz/app/help/4-2-1-selection-tool.md"). Behaves like
  // show() — first call lazy-builds the sidebar — but instead of landing
  // on Welcome, walks m_topics for a resource_path match and selects that
  // leaf. The leaf's parent chapter (and group, if any) are force-opened
  // first so the highlighted row is visible in the sidebar. Falls back to
  // a normal show() if the path doesn't match any topic.
  //
  // Used by the ContextBar's right-click "Open Help: <tool>" menu — when
  // the quick hints aren't enough, the user can jump straight to the full
  // page for the active tool without hunting through the outline.
  void open_at(Gtk::Window &parent, const std::string &resource_path);

  // ── Persistence hooks (s129 m7) ─────────────────────────────────────────
  // Mirror the PropertiesPanel pattern: MainWindow::save_config snapshots
  // this map into app config under a dedicated key, and load_last_project_*
  // hands the restored map back before the user opens the help window.
  // The map is keyed by row title (e.g. "1 Getting started", "4.2 Selection
  // & Node") and values are the open/collapsed bool for that header. Keys
  // for rows that have never been touched simply aren't present, and the
  // builder falls back to the default-open rules baked into build_sidebar.
  const std::map<std::string, bool>& sidebar_open_state() const {
    return m_section_open;
  }
  void set_sidebar_open_state(const std::map<std::string, bool>& state) {
    m_section_open = state;
  }

private:
  // ── A single sidebar entry ──────────────────────────────────────────────
  // The outline lists every chapter, every group, every leaf in declaration
  // order. `kind` says how the row behaves:
  //   • Chapter — top-level disclosure header (e.g. "1 Getting started")
  //   • Group   — second-level disclosure header used for the sub-groups
  //               under chapters 4 and 5 (e.g. "4.2 Selection & Node")
  //   • Leaf    — actual page row; clickable when `available` is true
  // `indent` is no longer used at runtime (the nested-box layout encodes
  // depth on its own) but is kept on the row for documentation purposes —
  // it's the third number you'd read off the dotted ID.
  // `available` only applies to leaves and decides whether the row is
  // clickable. Non-leaf rows have empty `resource_path` and are inert.
  enum class RowKind { Chapter, Group, Leaf };

  struct Topic {
    RowKind     kind          = RowKind::Leaf;
    int         indent        = 0;       // 0..2; not used at runtime
    bool        available     = false;   // leaves only
    std::string resource_path;           // empty for Chapter / Group
    std::string title;                   // e.g. "1.3 Workspace tour"
  };

  // ── Construction helpers ─────────────────────────────────────────────────
  void build_topic_list();         // populate m_topics from the bundled list
  void build_sidebar();            // attach rows to m_sidebar_box
  void show_topic(std::size_t i);  // swap content_area to topic i's render

  // Apply visual highlight to one leaf row, clear it from any previously-
  // selected leaf, and call show_topic for index i. Used by both initial-
  // selection and the per-leaf click gesture.
  void select_leaf(std::size_t i);

  // ── Markdown rendering ───────────────────────────────────────────────────
  // Read raw .md from gresource, parse with libcmark, build a vertical box
  // of widgets representing the document. Returns a managed widget owned by
  // the caller's container.
  Gtk::Widget *render_markdown(const std::string &resource_path);

  // ── Search (s132 m1) ────────────────────────────────────────────────────
  // Sidebar search filters the visible outline as the user types. Match is
  // case-insensitive and runs over leaf titles plus their bundled markdown
  // body text. Body text is loaded lazily — first search forces a one-shot
  // gresource sweep — and cached in m_body_search_text indexed by topic
  // index. On empty query, the sidebar restores its saved disclosure state
  // from m_section_open.
  void on_search_changed();
  void apply_search_filter(const std::string &query_lower);
  void load_search_index_if_needed();
  static std::string lower_ascii(const std::string &s);
  // Strip cmark image tags / pure HTML / leading hashes etc. just enough
  // to make a body match meaningful (we don't want "img/icons/curvz-pen-
  // symbolic.svg" to match a search for "pen"). Minimal sanitiser, not a
  // full markdown stripper.
  static std::string sanitise_body_for_search(const std::string &md);

  // ── Widgets ──────────────────────────────────────────────────────────────
  Gtk::Box           m_root{Gtk::Orientation::VERTICAL};
  Gtk::Paned         m_paned{Gtk::Orientation::HORIZONTAL};
  Gtk::Box           m_sidebar_col{Gtk::Orientation::VERTICAL};  // s132 m1
  Gtk::SearchEntry   m_search_entry;                              // s132 m1
  Gtk::ScrolledWindow m_sidebar_scroll;
  // m6 used a Gtk::ListBox here; m7 swapped it for a plain VBox so the
  // disclosure-and-body-with-set_visible idiom works the same way it does
  // in PropertiesPanel. The box holds chapter outers, each of which holds
  // its own header + body containing leaves and (for chapters 4/5) nested
  // group outers.
  Gtk::Box           m_sidebar_box{Gtk::Orientation::VERTICAL};
  Gtk::ScrolledWindow m_content_scroll;
  Gtk::Box           m_content_holder{Gtk::Orientation::VERTICAL};
  // Currently-rendered topic widget; replaced on selection change. Owned by
  // m_content_holder (managed widget).
  Gtk::Widget       *m_current_content = nullptr;

  std::vector<Topic> m_topics;

  // Per-row leaf widget pointers, parallel to m_topics. Non-leaf entries
  // are nullptr. Used to apply / clear the "selected" CSS class on the
  // active row when leaf selection changes.
  std::vector<Gtk::Widget *> m_leaf_widgets;
  std::size_t m_selected_leaf = static_cast<std::size_t>(-1);

  // Open/closed state for Chapter and Group disclosures, keyed by title.
  // Persisted across sessions via save_config / load_last_project_path
  // in MainWindow. See sidebar_open_state() / set_sidebar_open_state().
  std::map<std::string, bool> m_section_open;

  // ── Search state (s132 m1) ──────────────────────────────────────────────
  // Section-row outer widgets, parallel to m_topics — populated only at
  // Chapter and Group indices, nullptr elsewhere. apply_search_filter
  // walks topics and hides outers whose subtree has no match. Body bodies
  // (the contents-of-disclosure boxes) are tracked separately so we can
  // force them open during an active search, then restore on clear.
  std::vector<Gtk::Widget *> m_section_outer;
  std::vector<Gtk::Widget *> m_section_body;
  std::vector<Gtk::Label  *> m_section_arrow;

  // Lazily-loaded markdown body text (post-sanitise, lower-cased) keyed
  // by topic index. Filled on first search via load_search_index_if_needed.
  std::unordered_map<std::size_t, std::string> m_body_search_text;
  bool m_search_index_loaded = false;
  // Lower-cased title cache, parallel to m_topics — built once next to
  // load_search_index_if_needed so per-keystroke matching is just a pair
  // of substring searches.
  std::vector<std::string> m_title_search_text;
};

} // namespace Curvz
