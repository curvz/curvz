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
    void set_project(CurvzProject* project) { m_project = project; }
    void refresh();

    using PlaceItemSignal      = sigc::signal<void(const std::string&)>;
    using AddToLibrarySignal   = sigc::signal<void(const std::string&)>;

    PlaceItemSignal&    signal_place_item()     { return m_sig_place_item; }
    AddToLibrarySignal& signal_add_to_library() { return m_sig_add_to_library; }

private:
    struct Category {
        std::string              name;
        bool                     is_user;
        std::vector<LibraryItem> items;
    };

    CurvzDocument*        m_doc = nullptr;
    CurvzProject*         m_project = nullptr;  // s117 m14 v2: thumb motif/artboard
    std::vector<Category> m_categories;

    PlaceItemSignal    m_sig_place_item;
    AddToLibrarySignal m_sig_add_to_library;

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

    Cairo::RefPtr<Cairo::ImageSurface>
    render_thumb(const std::string& svg_path, int size);

    std::string user_library_dir() const;
    std::string system_library_dir() const;
    void        on_add_clicked();
};

} // namespace Curvz
