#pragma once
#include "CurvzDocument.hpp"
#include "CoordSpace.hpp"
#include "UnitSystem.hpp"
#include <gtkmm/drawingarea.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/popover.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/button.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/adjustment.h>
#include <sigc++/sigc++.h>

namespace Curvz {

static constexpr int RULER_SIZE = 16;

struct RulerState {
    double zoom          = 1.0;
    double pan_x         = 0.0;
    double pan_y         = 0.0;
    double canvas_w      = 24.0;
    double canvas_h      = 24.0;
    double widget_w      = 800.0;
    double widget_h      = 600.0;
    double ruler_origin_x = 0.0;
    double ruler_origin_y = 0.0;
    int    quality        = 24;
    DisplayMode display_mode = DisplayMode::Pixel;
    double phys_short    = 1.0;
    std::string phys_unit = "in";
    Unit   display_unit  = Unit::Px;   // governs tick labels and intervals
    double cursor_doc_x  = 0.0;
    double cursor_doc_y  = 0.0;
    // Preview origin for drag feedback (active only during drag)
    bool   preview_active  = false;
    double preview_origin_x = 0.0;  // user space
    double preview_origin_y = 0.0;
};

// ── HRuler ────────────────────────────────────────────────────────────────────
class HRuler : public Gtk::DrawingArea {
public:
    HRuler();
    void set_state(const RulerState& s);

    // S117 m3: motif controls which ruler colour palette is used at draw
    // time. See Ruler.cpp for the palette definitions. Setter triggers a
    // queue_draw() so the change is visible immediately.
    void set_motif(Motif m) {
        if (m_motif == m) return;
        m_motif = m;
        queue_draw();
    }

    // Emitted when user drags from ruler onto canvas — pos is doc-space X
    using GuideSignal = sigc::signal<void(double /*doc_pos*/)>;
    GuideSignal& signal_guide_dragging() { return m_sig_guide_dragging; }
    GuideSignal& signal_guide_created()  { return m_sig_guide_created;  }
    sigc::signal<void()>& signal_guide_drag_cancel() { return m_sig_guide_cancel; }

    using UnitSignal = sigc::signal<void(Unit)>;
    UnitSignal& signal_unit_changed() { return m_sig_unit; }

private:
    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void build_unit_popover();
    RulerState m_state;
    Motif      m_motif      = Motif::Dark;
    bool   m_dragging    = false;
    double m_drag_doc_x  = 0.0;
    Gtk::Popover         m_pop;
    GuideSignal          m_sig_guide_dragging;
    GuideSignal          m_sig_guide_created;
    sigc::signal<void()> m_sig_guide_cancel;
    UnitSignal           m_sig_unit;
};

// ── VRuler ────────────────────────────────────────────────────────────────────
class VRuler : public Gtk::DrawingArea {
public:
    VRuler();
    void set_state(const RulerState& s);

    // S117 m3: see HRuler::set_motif.
    void set_motif(Motif m) {
        if (m_motif == m) return;
        m_motif = m;
        queue_draw();
    }

    // Emitted when user drags from ruler onto canvas — pos is doc-space Y
    using GuideSignal = sigc::signal<void(double /*doc_pos*/)>;
    GuideSignal& signal_guide_dragging() { return m_sig_guide_dragging; }
    GuideSignal& signal_guide_created()  { return m_sig_guide_created;  }
    sigc::signal<void()>& signal_guide_drag_cancel() { return m_sig_guide_cancel; }

    using UnitSignal = sigc::signal<void(Unit)>;
    UnitSignal& signal_unit_changed() { return m_sig_unit; }

private:
    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void build_unit_popover();
    RulerState m_state;
    Motif      m_motif      = Motif::Dark;
    bool   m_dragging    = false;
    double m_drag_doc_y  = 0.0;
    Gtk::Popover         m_pop;
    GuideSignal          m_sig_guide_dragging;
    GuideSignal          m_sig_guide_created;
    sigc::signal<void()> m_sig_guide_cancel;
    UnitSignal           m_sig_unit;
};

// ── CornerSquare ──────────────────────────────────────────────────────────────
class CornerSquare : public Gtk::DrawingArea {
public:
    CornerSquare();

    using OriginSignal  = sigc::signal<void(double, double)>;
    using ResetSignal   = sigc::signal<void()>;
    using PreviewSignal = sigc::signal<void(double, double)>;  // live drag preview
    using StopPreviewSignal = sigc::signal<void()>;            // drag ended/cancelled

    OriginSignal&      signal_origin_changed()  { return m_sig_origin;       }
    ResetSignal&       signal_origin_reset()    { return m_sig_reset;        }
    PreviewSignal&     signal_origin_preview()  { return m_sig_preview;      }
    StopPreviewSignal& signal_preview_stop()    { return m_sig_preview_stop; }

    void set_state(const RulerState& s) { m_state = s; }
    void set_unit(Unit u) {
        if (m_unit_lbl)
            m_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    }

    // S117 m3: see HRuler::set_motif.
    void set_motif(Motif m) {
        if (m_motif == m) return;
        m_motif = m;
        queue_draw();
    }

private:
    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void build_origin_popover();
    std::pair<double,double> screen_to_user(double sx, double sy) const;
    std::pair<double,double> drag_screen_to_user(double offset_x, double offset_y) const;

    RulerState   m_state;
    Motif        m_motif    = Motif::Dark;
    bool         m_dragging = false;

    // Alt-click exact origin popover
    Gtk::Popover     m_pop;
    Gtk::SpinButton  m_x_spin;
    Gtk::SpinButton  m_y_spin;
    Glib::RefPtr<Gtk::Adjustment> m_x_adj;
    Glib::RefPtr<Gtk::Adjustment> m_y_adj;
    Gtk::Label*      m_unit_lbl = nullptr;

    OriginSignal      m_sig_origin;
    ResetSignal       m_sig_reset;
    PreviewSignal     m_sig_preview;
    StopPreviewSignal m_sig_preview_stop;
};

} // namespace Curvz
