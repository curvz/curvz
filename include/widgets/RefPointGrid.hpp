// widgets/RefPointGrid.hpp ────────────────────────────────────────────
//
// 3x3 anchor-point picker. Pure visual + click widget; no spinners, no
// bbox math, no scriptable wrapper. Draws a 36x36 grid with 9 dots
// (NW N NE / W C E / SW S SE), highlights the selected one, and emits
// signal_preset_changed when the user clicks a different dot.
//
// s290 — extracted from widgets/RefPointPicker so the creator popovers
// (rect, ellipse, ...) can use the grid as a plain anchor input without
// inheriting RefPointPicker's bbox-bound X/Y spinner composite. The
// existing RefPointPicker is refactored to embed a RefPointGrid as its
// visual half; all RefPointPicker public API is preserved.
//
// Drawing follows the GTK CSS color cascade — get_color() picks up the
// theme/motif foreground, with a derived complement for the unselected
// dot fill. The widget exposes the .refpoint-grid CSS class for theme
// overrides.
//
// Not a ScriptableWidget. The grid is a leaf control; scriptable usage
// happens at the composite layer (RefPointPicker registers itself
// scriptably; standalone uses register the containing widget instead).

#pragma once

#include <gtkmm/drawingarea.h>
#include <sigc++/signal.h>

namespace curvz::widgets {

class RefPointGrid : public Gtk::DrawingArea {
public:
  enum class Preset { NW, N, NE, W, C, E, SW, S, SE };

  RefPointGrid();

  // ── Public API ───────────────────────────────────────────────────────

  // Currently selected preset. Default is C (center).
  Preset preset() const { return m_preset; }

  // Programmatic preset change. Emits signal_preset_changed if the
  // value actually changes.
  void set_preset(Preset p);

  // Greyed-out mode — used by composites (e.g. RefPointPicker) to
  // indicate that the preset is currently inactive (e.g. when the user
  // has typed arbitrary X/Y values). Greyed grid still accepts clicks;
  // composite owners can suppress that at the controller level if
  // desired.
  void set_greyed(bool grey);
  bool greyed() const { return m_greyed; }

  // ── Output signal ────────────────────────────────────────────────────

  // Emitted when the preset changes via user click OR set_preset().
  sigc::signal<void(Preset)> &signal_preset_changed() {
    return m_sig_preset_changed;
  }

  // ── Naming helpers (exposed for composites that round-trip via
  //     script verbs / serialization) ─────────────────────────────────
  static const char *name_of(Preset p);
  static bool from_name(std::string_view name, Preset *out);

  // (frac_x, frac_y) in [0,1] for the preset. Useful for anchor math
  // in callers (e.g. "place a bbox with this anchor"):
  //   X_corner = X_anchor - W * frac_x
  //   Y_corner = Y_anchor - H * frac_y
  static void preset_to_fractions(Preset p, double *frac_x, double *frac_y);

private:
  Preset m_preset = Preset::C;
  bool m_greyed = false;
  sigc::signal<void(Preset)> m_sig_preset_changed;

  void on_draw_grid(const Cairo::RefPtr<Cairo::Context> &cr, int w, int h);
  bool pixel_to_preset(double px, double py, Preset *out_p) const;
};

} // namespace curvz::widgets
