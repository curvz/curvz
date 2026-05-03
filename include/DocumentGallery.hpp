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

    DocActivatedSignal&  signal_doc_activated()  { return m_signal_doc_activated; }
    AddDocSignal&        signal_add_doc()         { return m_signal_add_doc; }
    DupDocSignal&        signal_dup_doc()         { return m_signal_dup_doc; }
    RemoveDocSignal&     signal_remove_doc()      { return m_signal_remove_doc; }
    RenameDocSignal&     signal_rename_doc()      { return m_signal_rename_doc; }
    ClearAllSignal&      signal_clear_all()       { return m_signal_clear_all; }
    FilterChangedSignal& signal_filter_changed()  { return m_signal_filter_changed; }
    PreviewIconSignal&   signal_preview_icon()    { return m_signal_preview_icon; }
    CopyIconSignal&      signal_copy_icon()       { return m_signal_copy_icon; }

private:
    enum class ViewMode { Thumbnail, List };

    void rebuild_project_tab();
    void rebuild_system_tab();
    void apply_filter();

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
    Gtk::DropDown*      m_sys_theme_drop   = nullptr;
    Gtk::DropDown*      m_sys_cat_drop     = nullptr;
    Gtk::ScrolledWindow m_sys_scroll;
    Gtk::FlowBox        m_sys_flow;

    // System tab placeholder (shown while not yet scanned)
    Gtk::Label          m_system_placeholder;

    DocActivatedSignal  m_signal_doc_activated;
    AddDocSignal        m_signal_add_doc;
    DupDocSignal        m_signal_dup_doc;
    RemoveDocSignal     m_signal_remove_doc;
    RenameDocSignal     m_signal_rename_doc;
    ClearAllSignal      m_signal_clear_all;
    FilterChangedSignal m_signal_filter_changed;
    PreviewIconSignal   m_signal_preview_icon;
    CopyIconSignal      m_signal_copy_icon;
};

} // namespace Curvz
