#pragma once
#include <string>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>

namespace Curvz {

class StatusBar : public Gtk::Box {
public:
    StatusBar();

    void set_cursor_pos(double x, double y);
    void set_zoom(double pct);
    void set_counts(int objects, int nodes);
    void set_project_name(const std::string& name);
    void set_doc_name(const std::string& name);  // active document filename
    void set_mode(const std::string& mode);

private:
    Gtk::Label m_pos_label;
    Gtk::Label m_zoom_label;
    Gtk::Label m_count_label;
    Gtk::Label m_project_label;
    Gtk::Label m_doc_label;    // active document name — right-aligned
    Gtk::Label m_mode_label;
};

} // namespace Curvz
