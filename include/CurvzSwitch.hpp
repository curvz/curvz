#pragma once
#include <gtkmm/drawingarea.h>
#include <sigc++/sigc++.h>

namespace Curvz {

// Cairo-drawn 32×18 toggle switch replacing Gtk::Switch.
// Emits signal_toggled(bool) when clicked.
class CurvzSwitch : public Gtk::DrawingArea {
public:
    CurvzSwitch();

    bool get_state() const { return m_state; }
    void set_state(bool on);

    using ToggledSignal = sigc::signal<void(bool)>;
    ToggledSignal& signal_toggled() { return m_sig_toggled; }

private:
    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void on_click(int n, double x, double y);

    bool           m_state  = true;
    ToggledSignal  m_sig_toggled;
};

} // namespace Curvz
