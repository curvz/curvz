#pragma once
#include "CurvzDocument.hpp"
#include "CurvzProject.hpp"
#include "SceneNode.hpp"
#include <cairomm/cairomm.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/label.h>
#include <gtkmm/revealer.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <sigc++/sigc++.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace Curvz {

// ── LibraryItem ───────────────────────────────────────────────────────────────
struct LibraryItem {
    std::string path;      // Full path to .svg file
    std::string name;      // Display name (filename without .svg)
    std::string category;  // Category (parent folder name)
    bool        is_user;   // true = ~/.config/curvz/library
};

// ── LibraryPanel ─────────────────────────────────────────────────────────────
class LibraryPanel : public Gtk::Box {
public:
    LibraryPanel();

    void set_document(CurvzDocument* doc) { m_doc = doc; }
    // S117 m14 v2: project access lets render_thumb honour the project's
    // motif and per-motif artboard colour, matching the DocumentGallery
    // thumbnail fix. MainWindow wires this alongside set_document.
    //
    // s141: also seeds m_expanded from project->library_expanded_categories
    // so per-project expansion state survives across launches. The setter
    // is defined out-of-line in the .cpp so the inline definition stays
    // mechanical (no implementation hidden in the header).
    void set_project(CurvzProject* project);
    void refresh();

    using PlaceItemSignal      = sigc::signal<void(const std::string&)>;
    using AddToLibrarySignal   = sigc::signal<void(const std::string&)>;
    using RequestSaveSignal    = sigc::signal<void()>;

    PlaceItemSignal&    signal_place_item()     { return m_sig_place_item; }
    AddToLibrarySignal& signal_add_to_library() { return m_sig_add_to_library; }
    // s141: emitted when the user expands or collapses a library
    // category. MainWindow connects this to schedule_save so per-project
    // expansion state lands in the .curvz file. Mirrors StylesPanel's
    // signal_view_state_changed pattern.
    RequestSaveSignal&  signal_request_save()   { return m_sig_request_save; }

private:
    struct Category {
        std::string              name;
        bool                     is_user;
        std::vector<LibraryItem> items;
    };

    CurvzDocument*        m_doc = nullptr;
    CurvzProject*         m_project = nullptr;  // s117 m14 v2: thumb motif/artboard
    std::vector<Category> m_categories;

    // s141: per-category expanded flag, keyed by "sys:<name>" / "usr:<name>"
    // to disambiguate same-named system and user categories. Categories
    // default to collapsed on first show (matches Affinity / Sketch / Figma
    // asset-library convention — expanding a giant icon grid by default
    // overwhelms the panel). Map lookup of a missing key returns false
    // (= collapsed). User opens a category → entry flips true.
    //
    // Persistence: seeded from m_project->library_expanded_categories
    // when set_project() runs. On every toggle, sync_expanded_to_project
    // writes the current set of true-keyed entries back to the project
    // and emits signal_request_save so MainWindow triggers a save.
    std::unordered_map<std::string, bool> m_expanded;

    PlaceItemSignal    m_sig_place_item;
    AddToLibrarySignal m_sig_add_to_library;
    RequestSaveSignal  m_sig_request_save;

    Gtk::Box            m_header  { Gtk::Orientation::HORIZONTAL };
    Gtk::Label          m_title;
    Gtk::Button         m_btn_add;
    Gtk::Button         m_btn_refresh;
    Gtk::ScrolledWindow m_scroll;
    Gtk::Box            m_content { Gtk::Orientation::VERTICAL };

    void         scan_library();
    void         rebuild_ui();
    Gtk::Widget* make_category_section(const Category& cat);
    Gtk::Widget* make_item_card(const LibraryItem& item);

    // s141: write the current m_expanded state back into
    // m_project->library_expanded_categories. Only true-keyed entries
    // are emitted (sparse representation). Called from the category-
    // header click handler after each toggle. No-op if m_project is
    // null.
    void         sync_expanded_to_project();

    Cairo::RefPtr<Cairo::ImageSurface>
    render_thumb(const std::string& svg_path, int size);

    std::string user_library_dir() const;
    std::string system_library_dir() const;
    void        on_add_clicked();
};

} // namespace Curvz
