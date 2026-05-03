#pragma once
#include "CurvzProject.hpp"
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/popovermenu.h>
#include <giomm/menu.h>
#include <sigc++/sigc++.h>
#include <vector>

namespace Curvz {

// DocTabBar — thin horizontal tab strip that lives inside the headerbar.
// One tab per document in the active project.
// Right-click on a tab → context menu with Remove.
// Right-click on empty area or label → context menu with New Document.
class DocTabBar : public Gtk::Box {
public:
    DocTabBar();

    void set_project(CurvzProject* project);
    void refresh();
    void set_filter(const std::string& query);

    using DocActivatedSignal = sigc::signal<void(int)>;
    using AddDocSignal       = sigc::signal<void()>;
    using RemoveDocSignal    = sigc::signal<void(int)>;

    DocActivatedSignal& signal_doc_activated() { return m_sig_activated; }
    AddDocSignal&       signal_add_doc()       { return m_sig_add; }
    RemoveDocSignal&    signal_remove_doc()    { return m_sig_remove; }

private:
    void rebuild();
    void show_tab_menu(int idx, double x, double y);   // right-click on a tab
    void show_bar_menu(double x, double y);            // right-click on empty area

    // S60 M3: chevron visibility + scroll helpers
    void update_chevron_visibility();
    void scroll_by(double dx);

    Gtk::ScrolledWindow m_scroll;
    Gtk::Box            m_tab_row{Gtk::Orientation::HORIZONTAL};

    // S60 M3: scroll chevrons shown when content overflows the viewport.
    Gtk::Button         m_scroll_left;
    Gtk::Button         m_scroll_right;

    CurvzProject*       m_project = nullptr;
    int                 m_ctx_idx = -1;  // index of tab that was right-clicked
    std::string         m_filter;

    DocActivatedSignal  m_sig_activated;
    AddDocSignal        m_sig_add;
    RemoveDocSignal     m_sig_remove;
};

} // namespace Curvz
