#include "StatusBar.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s121 m6: curvz::utils::set_name
#include <gtkmm/separator.h>
#include <string>

namespace Curvz {

StatusBar::StatusBar() : Gtk::Box(Gtk::Orientation::HORIZONTAL) {
    curvz::utils::set_name(*this, "sb", "status_bar_root");
    set_spacing(0);
    set_margin_start(8);
    set_margin_end(8);
    set_margin_top(2);
    set_margin_bottom(2);
    add_css_class("statusbar");

    m_pos_label.set_text("x: 0.00  y: 0.00");
    m_pos_label.set_width_chars(20);
    m_pos_label.set_xalign(0.0f);
    m_pos_label.add_css_class("statusbar-label");
    curvz::utils::set_name(m_pos_label, "sb_pos", "status_bar_pos_lbl");

    auto* sep1 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
    sep1->set_margin_start(8);
    sep1->set_margin_end(8);

    m_zoom_label.set_text("1600%");
    m_zoom_label.set_width_chars(8);
    m_zoom_label.set_xalign(0.0f);
    m_zoom_label.add_css_class("statusbar-label");
    curvz::utils::set_name(m_zoom_label, "sb_zm", "status_bar_zoom_lbl");

    auto* sep2 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
    sep2->set_margin_start(8);
    sep2->set_margin_end(8);

    m_count_label.set_text("0 objects  0 nodes");
    m_count_label.set_xalign(0.0f);
    m_count_label.add_css_class("statusbar-label");
    curvz::utils::set_name(m_count_label, "sb_cnt", "status_bar_count_lbl");

    append(m_pos_label);
    append(*sep1);
    append(m_zoom_label);
    append(*sep2);
    append(m_count_label);

    auto* sep3 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
    sep3->set_margin_start(8);
    sep3->set_margin_end(8);
    append(*sep3);

    m_project_label.set_text("unsaved");
    m_project_label.set_xalign(0.0f);
    m_project_label.add_css_class("statusbar-label");
    curvz::utils::set_name(m_project_label, "sb_prj", "status_bar_project_lbl");
    append(m_project_label);

    auto* sep4 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
    sep4->set_margin_start(8);
    sep4->set_margin_end(8);
    append(*sep4);

    m_mode_label.set_text("Preview");
    m_mode_label.set_xalign(0.0f);
    m_mode_label.add_css_class("statusbar-label");
    curvz::utils::set_name(m_mode_label, "sb_mod", "status_bar_mode_lbl");
    append(m_mode_label);

    // Active document name — pushed to the right edge
    auto* sep5 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
    sep5->set_margin_start(8);
    sep5->set_margin_end(8);

    m_doc_label.set_text("");
    m_doc_label.set_xalign(1.0f);  // right-align text
    m_doc_label.set_hexpand(true); // push to far right
    m_doc_label.add_css_class("statusbar-label");
    m_doc_label.add_css_class("statusbar-doc-label");
    curvz::utils::set_name(m_doc_label, "sb_doc", "status_bar_doc_lbl");
    append(*sep5);
    append(m_doc_label);
}

void StatusBar::set_cursor_pos(double x, double y) {
    char buf[64];
    snprintf(buf, sizeof(buf), "x: %.2f  y: %.2f", x, y);
    m_pos_label.set_text(buf);
}

void StatusBar::set_zoom(double pct) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f%%", pct);
    m_zoom_label.set_text(buf);
}

void StatusBar::set_counts(int objects, int nodes) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d object%s  %d node%s",
             objects, objects == 1 ? "" : "s",
             nodes,   nodes   == 1 ? "" : "s");
    m_count_label.set_text(buf);
}

void StatusBar::set_project_name(const std::string& name) {
    m_project_label.set_text(name);
}

void StatusBar::set_doc_name(const std::string& name) {
    m_doc_label.set_text(name);
}

void StatusBar::set_mode(const std::string& mode) {
    m_mode_label.set_text(mode);
}

} // namespace Curvz
