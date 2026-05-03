#pragma once
#include <gtkmm.h>
#include <sigc++/sigc++.h>
#include <functional>
#include "UnitSystem.hpp"
#include "CurvzDocument.hpp"

namespace Curvz {

// ── SpinType ──────────────────────────────────────────────────────────────────
//
//   Distance   — signed distance in doc units; converts via UnitSystem.
//                Use for offsets, gap fields where origin doesn't matter.
//
//   Width      — unsigned distance in doc units; converts via UnitSystem.
//                Use for W, H, stroke width, spacing, gap fields.
//
//   PositionX  — signed X coordinate in doc units.
//                Applies ruler origin offset. No Y-flip.
//                Pass ruler_ox at construction.
//
//   PositionY  — signed Y coordinate in doc units.
//                Applies ruler origin offset + Y-flip (Y-up user space).
//                Pass ruler_oy at construction.
//
//   Angle      — degrees, no conversion. Range –360..360, step 0.1.
//
//   Percentage — 0..100, no conversion. Step 0.1, 1 decimal.
//
//   Integer    — whole numbers, no conversion. Step 1, 0 decimals.

enum class SpinType {
    Distance,
    Width,
    PositionX,
    PositionY,
    Angle,
    Percentage,
    Integer,
};

// ── CurvzSpinButton ───────────────────────────────────────────────────────────
// A Gtk::SpinButton that owns its Adjustment and handles unit conversion
// internally.  Call sites work exclusively in internal (doc-unit) values.
//
// Fluent construction:
//
//   // Distance / Width:
//   auto *sp = Gtk::make_managed<CurvzSpinButton>(SpinType::Width, m_canvas)
//       ->with_value(obj->stroke.width)
//       ->with_tooltip("Stroke thickness")
//       ->with_css("tb-well-spin")
//       ->on_changed([this, obj](double v) {
//           if (m_loading) return;
//           obj->stroke.width = v;
//           push_inspector_command(obj);
//       });
//   row->append(*sp);
//   row->append(*sp->get_unit_label());
//
//   // Position:
//   auto *sp = Gtk::make_managed<CurvzSpinButton>(
//                  SpinType::PositionX, m_canvas, m_ruler_ox)
//       ->with_value(obj->x)
//       ->on_changed([this, obj](double v) {
//           if (m_loading) return;
//           obj->x = v;           // v is in doc units
//           push_inspector_command(obj);
//       });

class CurvzSpinButton : public Gtk::SpinButton {
public:
    // General constructor — for Distance, Width, Angle, Percentage, Integer.
    explicit CurvzSpinButton(SpinType type,
                              const CanvasModel* model = nullptr);

    // Position constructor — for PositionX / PositionY.
    // ruler_origin is m_ruler_ox (for X) or m_ruler_oy (for Y).
    CurvzSpinButton(SpinType type,
                    const CanvasModel* model,
                    double ruler_origin);

    // ── Fluent builder ────────────────────────────────────────────────────────

    CurvzSpinButton* with_value(double internal);
    CurvzSpinButton* with_tooltip(const char* tip);
    CurvzSpinButton* with_css(const char* css_class);
    CurvzSpinButton* with_width_chars(int n);
    CurvzSpinButton* on_changed(std::function<void(double)> cb);

    // ── Model ─────────────────────────────────────────────────────────────────

    void set_model(const CanvasModel* model);

    // Update ruler origin (e.g. after user moves ruler zero point).
    // Only meaningful for PositionX / PositionY types.
    void set_ruler_origin(double origin);

    // ── Value accessors ───────────────────────────────────────────────────────

    void   set_internal_value(double internal);
    double get_internal_value() const { return m_internal; }

    // ── Unit label ────────────────────────────────────────────────────────────

    // Companion label — append to row box after spinbutton.
    // Shows "px", "in", "mm", "pt" for Distance/Width/Position types.
    // Blank for other types.
    Gtk::Label* get_unit_label() { return m_unit_label; }

    // ── Unit refresh ─────────────────────────────────────────────────────────

    // Call when CanvasModel::display_unit changes.
    // No-op for non-distance types.
    void refresh_units();

    // ── Unit conversion (read-only) ──────────────────────────────────────────
    //
    // Convert a stored "internal" doc-space value to the user-facing display
    // value (current display unit, ruler origin applied, Y-flipped for
    // PositionY). Mirrors the conversion done internally on every
    // adjustment update. Public so panels with read-only readouts (visual
    // size labels, status bars, etc.) can format doc-space values the same
    // way the editable spinners do.
    double to_display(double internal) const;

    // ── Signal ───────────────────────────────────────────────────────────────

    sigc::signal<void(double)>& signal_internal_changed() {
        return m_signal_internal_changed;
    }

private:
    SpinType             m_type;
    const CanvasModel*   m_model;
    double               m_internal      = 0.0;
    double               m_ruler_origin  = 0.0;  // PositionX/Y only
    bool                 m_updating      = false;

    Glib::RefPtr<Gtk::Adjustment> m_adj;
    Gtk::Label*                   m_unit_label = nullptr;

    sigc::signal<void(double)> m_signal_internal_changed;

    // Error popover — lazily constructed. Held as Glib::RefPtr so GTK
    // manages lifetime across transient dismissal / reshow.
    Gtk::Popover* m_err_popover = nullptr;
    Gtk::Label*   m_err_label   = nullptr;
    sigc::connection m_err_hide_timer;

    void   init();
    void   apply_unit_params();
    void   update_unit_label();
    bool   is_distance_type() const;
    double to_internal(double display)  const;
    void   on_value_changed();

    // Expression parsing
    bool   type_allows_units() const;
    Unit   type_default_unit() const;
    bool   try_commit_text(const std::string& txt);  // returns true on success
    void   show_error_popover(const std::string& bad_input,
                              const std::string& msg);
    void   hide_error_popover();
    static bool is_char_allowed(gunichar ch, bool units_allowed);
};

} // namespace Curvz
