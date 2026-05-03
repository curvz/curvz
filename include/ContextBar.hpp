#pragma once
#include "Toolbar.hpp"
#include "SceneNode.hpp"
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/button.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/scrolledwindow.h>
#include <functional>
#include <string>
#include <vector>

namespace Curvz {

class ContextBar : public Gtk::Box {
public:
    ContextBar();

    void set_tool(ActiveTool tool);

    // s132 m1: right-click anywhere on the bar to open the manual at the
    // page for the active tool. MainWindow wires this to HelpWindow::open_at
    // at construction; the resource path is built from a static tool→leaf
    // table in ContextBar.cpp. Quick-guide hints in the centre strip stay
    // the first line of help; this is the "what if I want more?" affordance
    // so users don't have to hunt the outline.
    using HelpCallback = std::function<void(const std::string& resource_path)>;
    void set_help_callback(HelpCallback cb) { m_help_cb = std::move(cb); }

private:
    void rebuild(ActiveTool tool);
    void clear_dynamic();

    // s132 m1: build the right-click popover for the current tool.
    void show_context_menu(double x, double y);

    Gtk::Label*        add_hint  (const std::string& text, const std::string& css = "ctx-hint");
    Gtk::Label*        add_key   (const std::string& key);
    Gtk::Separator*    add_sep   ();
    Gtk::Button*       add_btn   (const std::string& icon, const std::string& tip,
                                  sigc::slot<void()> slot);
    Gtk::ToggleButton* add_toggle(const std::string& icon, const std::string& tip,
                                  bool initial, sigc::slot<void(bool)> slot);

    // Left side: tool icon + name
    Gtk::Box    m_left{Gtk::Orientation::HORIZONTAL};
    Gtk::Label  m_tool_icon;
    Gtk::Label  m_tool_name;

    // Centre: scrollable hint strip
    Gtk::ScrolledWindow m_centre_scroll;
    Gtk::Box            m_centre{Gtk::Orientation::HORIZONTAL};

    std::vector<Gtk::Widget*> m_dynamic;

    ActiveTool   m_tool = ActiveTool::Selection;
    HelpCallback m_help_cb;        // s132 m1
};

} // namespace Curvz
