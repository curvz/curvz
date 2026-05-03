#include "PreviewPanel.hpp"
#include "CurvzLog.hpp"
#include <gtkmm/separator.h>
#include <gtkmm/grid.h>

namespace Curvz {

PreviewPanel::PreviewPanel() : Gtk::Box(Gtk::Orientation::VERTICAL) {
    add_css_class("preview-panel");

    auto* title = Gtk::make_managed<Gtk::Label>("Preview");
    title->set_xalign(0.0f);
    title->set_margin_start(8);
    title->set_margin_top(6);
    title->set_margin_bottom(4);
    title->add_css_class("panel-title");
    append(*title);

    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    append(*sep);

    // 2×4 grid: sizes across, dark+light rows
    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(4);
    grid->set_column_spacing(6);
    grid->set_margin(8);

    const int sizes[4] = {16, 24, 32, 48};
    for (int i = 0; i < 4; ++i) {
        m_swatches[i].size = sizes[i];

        // Size label
        char buf[8];
        snprintf(buf, sizeof(buf), "%dpx", sizes[i]);
        m_swatches[i].size_label.set_text(buf);
        m_swatches[i].size_label.set_xalign(0.5f);
        m_swatches[i].size_label.add_css_class("caption-label");
        grid->attach(m_swatches[i].size_label, i, 0);

        // Dark swatch
        setup_swatch(m_swatches[i], sizes[i]);
        grid->attach(m_swatches[i].dark_area,  i, 1);
        grid->attach(m_swatches[i].light_area, i, 2);
    }

    append(*grid);
    LOG_DEBUG("PreviewPanel created");
}

void PreviewPanel::setup_swatch(Swatch& s, int size) {
    const int display_size = std::max(size, 24); // minimum display 24px so 16px isn't tiny

    s.dark_area.set_size_request(display_size + 4, display_size + 4);
    s.dark_area.set_draw_func([this, &s](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
        draw_swatch(cr, w, h, true, m_doc, s.size);
    });

    s.light_area.set_size_request(display_size + 4, display_size + 4);
    s.light_area.set_draw_func([this, &s](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
        draw_swatch(cr, w, h, false, m_doc, s.size);
    });
}

void PreviewPanel::draw_swatch(const Cairo::RefPtr<Cairo::Context>& cr,
                                int w, int h, bool dark_bg,
                                CurvzDocument* /*doc*/, int /*px_size*/) {
    // Background
    if (dark_bg)
        cr->set_source_rgb(0.15, 0.15, 0.15);
    else
        cr->set_source_rgb(0.95, 0.95, 0.95);
    cr->paint();

    // Placeholder cross-hatch to show the swatch area
    cr->set_source_rgba(0.5, 0.5, 0.5, 0.3);
    cr->set_line_width(0.5);
    cr->move_to(2, 2); cr->line_to(w-2, h-2);
    cr->move_to(w-2, 2); cr->line_to(2, h-2);
    cr->stroke();
}

void PreviewPanel::set_document(CurvzDocument* doc) {
    m_doc = doc;
    refresh();
}

void PreviewPanel::refresh() {
    for (auto& s : m_swatches) {
        s.dark_area.queue_draw();
        s.light_area.queue_draw();
    }
}

} // namespace Curvz
