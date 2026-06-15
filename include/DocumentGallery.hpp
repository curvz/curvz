#pragma once
#include "CurvzProject.hpp"
#include "SystemIconScanner.hpp"
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/notebook.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/listbox.h>
#include <gtkmm/searchentry.h>
#include <gtkmm/label.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/drawingarea.h>
#include <cairomm/cairomm.h>

// s208 m5 — forward-declare substrate DropDown so DocumentGallery can hold
// pointers without pulling ScriptableWidget into every TU that includes
// this header. Full include in DocumentGallery.cpp.
namespace curvz::widgets { class DropDown; }

namespace Curvz {

class DocumentGallery : public Gtk::Box {
public:
    DocumentGallery();

    void set_project(CurvzProject* project);
    void refresh();

    using DocActivatedSignal  = sigc::signal<void(int)>;
    using AddDocSignal        = sigc::signal<void()>;
    using DupDocSignal        = sigc::signal<void(int)>;
    using RemoveDocSignal     = sigc::signal<void(int)>;
    using RenameDocSignal     = sigc::signal<void(int, std::string)>;
    using ClearAllSignal      = sigc::signal<void()>;
    using FilterChangedSignal = sigc::signal<void(std::string)>;
    using PreviewIconSignal   = sigc::signal<void(std::string)>;  // path
    using CopyIconSignal      = sigc::signal<void(std::string)>;  // path
    // s360 — fired when the gallery switches back to the Project tab. MainWindow
    // uses it to leave icon-preview mode (which swapped the canvas to a
    // throwaway preview doc) and restore + refresh the real project view.
    using ShowProjectTabSignal = sigc::signal<void()>;

    DocActivatedSignal&  signal_doc_activated()  { return m_signal_doc_activated; }
    AddDocSignal&        signal_add_doc()         { return m_signal_add_doc; }
    DupDocSignal&        signal_dup_doc()         { return m_signal_dup_doc; }
    RemoveDocSignal&     signal_remove_doc()      { return m_signal_remove_doc; }
    RenameDocSignal&     signal_rename_doc()      { return m_signal_rename_doc; }
    ClearAllSignal&      signal_clear_all()       { return m_signal_clear_all; }
    FilterChangedSignal& signal_filter_changed()  { return m_signal_filter_changed; }
    PreviewIconSignal&   signal_preview_icon()    { return m_signal_preview_icon; }
    CopyIconSignal&      signal_copy_icon()       { return m_signal_copy_icon; }
    ShowProjectTabSignal& signal_show_project_tab() { return m_signal_show_project_tab; }

private:
    enum class ViewMode { Thumbnail, List };

    void rebuild_project_tab();
    void rebuild_system_tab();
    void apply_filter();
    // s360 — show/hide the no-matches empty-state and the (now-empty) scrolls
    // after a filter pass. Shared by apply_filter (live typing) and
    // rebuild_project_tab (full rebuild) so both paths stay consistent.
    void update_filter_empty_state();

    Cairo::RefPtr<Cairo::ImageSurface> render_thumb(CurvzDocument* doc, int size);
    Cairo::RefPtr<Cairo::ImageSurface> render_svg_thumb(const std::string& path, int size);

    CurvzProject*     m_project   = nullptr;
    ViewMode          m_view_mode = ViewMode::Thumbnail;
    std::string       m_filter;

    // System icon scanner (lazy — scanned on first System tab open)
    SystemIconScanner m_scanner;
    bool              m_system_built = false;
    std::string       m_sys_theme;    // currently selected theme dir
    std::string       m_sys_category; // currently selected category ("" = all)

    // Header toolbar
    Gtk::Box          m_header{Gtk::Orientation::HORIZONTAL};
    Gtk::SearchEntry  m_search;
    Gtk::ToggleButton m_btn_view;
    Gtk::Button       m_btn_add;
    Gtk::Button       m_btn_dup;
    Gtk::Button       m_btn_remove;
    Gtk::Button       m_btn_clear;

    // Notebook (Project / System tabs)
    Gtk::Notebook     m_notebook;

    // Project — thumbnail view
    Gtk::ScrolledWindow m_thumb_scroll;
    Gtk::FlowBox        m_project_flow;

    // Project — list view
    Gtk::ScrolledWindow m_list_scroll;
    Gtk::ListBox        m_list_box;

    // System tab
    Gtk::Box            m_sys_box{Gtk::Orientation::VERTICAL};
    Gtk::Box            m_sys_controls{Gtk::Orientation::HORIZONTAL};
    // s208 m5: substrate. Forward-declared above to keep this header light;
    // full include lives in DocumentGallery.cpp.
    curvz::widgets::DropDown*  m_sys_theme_drop   = nullptr;
    curvz::widgets::DropDown*  m_sys_cat_drop     = nullptr;
    Gtk::ScrolledWindow m_sys_scroll;
    Gtk::FlowBox        m_sys_flow;

    // System tab placeholder (shown while not yet scanned)
    Gtk::Label          m_system_placeholder;
    // s360 — empty-state shown in the Project tab when a search filter is
    // active but matches no documents, so a filtered-to-nothing gallery reads
    // as "no matches" rather than looking like the documents vanished.
    Gtk::Label          m_project_empty;

    DocActivatedSignal  m_signal_doc_activated;
    AddDocSignal        m_signal_add_doc;
    DupDocSignal        m_signal_dup_doc;
    RemoveDocSignal     m_signal_remove_doc;
    RenameDocSignal     m_signal_rename_doc;
    ClearAllSignal      m_signal_clear_all;
    FilterChangedSignal m_signal_filter_changed;
    PreviewIconSignal   m_signal_preview_icon;
    CopyIconSignal      m_signal_copy_icon;
    ShowProjectTabSignal m_signal_show_project_tab;
};

} // namespace Curvz
