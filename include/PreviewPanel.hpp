#pragma once
#include "CurvzDocument.hpp"
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/drawingarea.h>

namespace Curvz {

class PreviewPanel : public Gtk::Box {
public:
    PreviewPanel();

    void set_document(CurvzDocument* doc);
    void refresh();

private:
    CurvzDocument* m_doc = nullptr;

    // Four mini DrawingAreas: 16, 24, 32, 48 px, dark + light each
    struct Swatch {
        Gtk::DrawingArea dark_area;
        Gtk::DrawingArea light_area;
        Gtk::Label       size_label;
        int              size = 16;
    };
    std::array<Swatch, 4> m_swatches;

    void setup_swatch(Swatch& s, int size);
    void draw_swatch(const Cairo::RefPtr<Cairo::Context>& cr,
                     int w, int h, bool dark_bg, CurvzDocument* doc, int px_size);
};

} // namespace Curvz
