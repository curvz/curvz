#include "CurvzSwitch.hpp"
#include <gtkmm/gestureclick.h>

namespace Curvz {

static constexpr int SW_W = 32;
static constexpr int SW_H = 18;
static constexpr int SW_R =  9;  // track corner radius = half height

CurvzSwitch::CurvzSwitch() {
    set_size_request(SW_W, SW_H);
    set_halign(Gtk::Align::CENTER);
    set_valign(Gtk::Align::CENTER);
    set_tooltip_text("Toggle Snap (Q)");
    set_cursor("pointer");

    set_draw_func(sigc::mem_fun(*this, &CurvzSwitch::on_draw));

    auto click = Gtk::GestureClick::create();
    click->signal_pressed().connect(sigc::mem_fun(*this, &CurvzSwitch::on_click));
    add_controller(click);
}

void CurvzSwitch::set_state(bool on) {
    if (m_state == on) return;
    m_state = on;
    queue_draw();
}

// Draw a rounded-rect track + circle thumb.
// On:  track #15539e, thumb white
// Off: track #4a4a4a, thumb #9a9a9a
void CurvzSwitch::on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int /*w*/, int /*h*/) {
    const double tx = 0.5;   // track x origin (0.5 for crisp AA)
    const double ty = 0.5;
    const double tw = SW_W - 1.0;
    const double th = SW_H - 1.0;
    const double r  = th / 2.0;

    // --- Track ---
    cr->move_to(tx + r, ty);
    cr->arc(tx + tw - r, ty + r, r, -M_PI / 2.0,  M_PI / 2.0);
    cr->arc(tx + r,      ty + r, r,  M_PI / 2.0,  3.0 * M_PI / 2.0);
    cr->close_path();

    if (m_state)
        cr->set_source_rgb(0x15 / 255.0, 0x53 / 255.0, 0x9e / 255.0);
    else
        cr->set_source_rgb(0x4a / 255.0, 0x4a / 255.0, 0x4a / 255.0);
    cr->fill();

    // --- Thumb ---
    const double thumb_r  = r - 2.0;
    const double thumb_cx = m_state ? (tx + tw - r) : (tx + r);
    const double thumb_cy = ty + r;

    cr->arc(thumb_cx, thumb_cy, thumb_r, 0.0, 2.0 * M_PI);

    if (m_state)
        cr->set_source_rgb(1.0, 1.0, 1.0);
    else
        cr->set_source_rgb(0x9a / 255.0, 0x9a / 255.0, 0x9a / 255.0);
    cr->fill();
}

void CurvzSwitch::on_click(int /*n*/, double /*x*/, double /*y*/) {
    m_state = !m_state;
    queue_draw();
    m_sig_toggled.emit(m_state);
}

} // namespace Curvz
