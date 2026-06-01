#pragma once
#include "CurvzDocument.hpp"
#include "UnitSystem.hpp"
#include "scripting/ScriptableWidget.hpp" // s295 m1 — substrate inheritance
#include <functional>
#include <gtkmm.h>
#include <sigc++/sigc++.h>

namespace Curvz {

// ── SpinType
// ──────────────────────────────────────────────────────────────────
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

// ── CurvzSpinButton
// ─────────────────────────────────────────────────────────── A CurvzSpinButton
// that owns its Adjustment and handles unit conversion internally.  Call sites
// work exclusively in internal (doc-unit) values.
//
// s295 m1 — inherits from ScriptableWidget<Gtk::SpinButton>, so every
// CurvzSpinButton is registered in the script registry under the abbrev
// passed to the ctor. Same vocabulary the curvz::widgets::SpinButton
// family already exposes:
//
//   set <number>     — set_internal_value (clamped, fires
//                      signal_internal_changed). Numeric back door.
//   step <int>       — spin(STEP_FORWARD/BACKWARD, |n|) per direction.
//   parse <string>   — route through try_commit_text — the rich
//                      domain-aware parser (Length / Angle / Percentage
//                      / Dimensionless). Returns Bool: true on commit,
//                      false on refusal. Same path a typing user takes.
//
// Queries:
//   value            — the INTERNAL (doc-unit) value, not the display
//                      value. Scripts work in doc units; the inspector
//                      live-tracking pattern emits internal values too.
//   min / max        — adjustment bounds in DISPLAY units (they're the
//                      adjustment's raw range; CurvzSpinButton scales
//                      between display and internal at i/o time).
//
// Emits:
//   value_changed <number>   on signal_internal_changed (real OR
//                            script-driven). The emitted payload is
//                            the internal value, matching `value`.
//
// Fluent construction (the abbrev is now mandatory):
//
//   // Distance / Width:
//   auto *sp = Gtk::make_managed<CurvzSpinButton>(
//                  "ins_obj_w", SpinType::Width, m_canvas)
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
//                  "ins_obj_px", SpinType::PositionX, m_canvas, m_ruler_ox)
//       ->with_value(obj->x)
//       ->on_changed([this, obj](double v) {
//           if (m_loading) return;
//           obj->x = v;           // v is in doc units
//           push_inspector_command(obj);
//       });
//
//   // Per-show transient (popover field, per-loop helper) — use
//   // the unregistered ctor to skip the registry. The widget is
//   // still IS-A substrate type with set/step/parse and value/min/max,
//   // it just can't be addressed by name from scripts. Required when
//   // multiple instances would otherwise collide on a shared abbrev.
//   auto *sp = Gtk::make_managed<CurvzSpinButton>(
//                  curvz::scripting::unregistered,
//                  SpinType::Width, m_canvas);

class CurvzSpinButton
    : public curvz::scripting::ScriptableWidget<Gtk::SpinButton> {
public:
  // General constructor — for Distance, Width, Angle, Percentage, Integer.
  CurvzSpinButton(std::string_view name, SpinType type,
                  const CanvasModel *model = nullptr);

  // Position constructor — for PositionX / PositionY.
  // ruler_origin is m_ruler_ox (for X) or m_ruler_oy (for Y).
  CurvzSpinButton(std::string_view name, SpinType type,
                  const CanvasModel *model, double ruler_origin);

  // s295 m1 — unregistered substrate CurvzSpinButton. Mirrors the
  // s211 m2 ctor on curvz::widgets::SpinButton: IS-A substrate type
  // (same universal verbs, same SpinType surface, same lifecycle),
  // but skips the script registry. Use at call sites where multiple
  // instances would otherwise collide on a shared abbrev — per-show
  // popovers that rebuild their spins on every open, per-loop helpers,
  // etc. The (general / position) shape mirrors the two registered
  // ctors above.
  CurvzSpinButton(curvz::scripting::unregistered_t, SpinType type,
                  const CanvasModel *model = nullptr);
  CurvzSpinButton(curvz::scripting::unregistered_t, SpinType type,
                  const CanvasModel *model, double ruler_origin);

  // ── Fluent builder ────────────────────────────────────────────────────────

  CurvzSpinButton *with_value(double internal);
  CurvzSpinButton *with_tooltip(const char *tip);
  CurvzSpinButton *with_css(const char *css_class);
  CurvzSpinButton *with_width_chars(int n);
  CurvzSpinButton *on_changed(std::function<void(double)> cb);

  // s331 — unit override. By default a distance/width spinner converts its
  // internal (doc-px) value against the DOCUMENT's display unit, routed
  // through DocUnits' intent-aware scaling. Some fields are type-domain: font
  // size is conventionally points regardless of the doc unit. Setting an
  // override makes THIS spinner own its unit — display, label, bare-number
  // default, and step/page/digits all use the pinned unit, and conversion
  // bypasses DocUnits for the pure UnitSystem 96/72-style math (so it doesn't
  // need a CanvasModel at all). The default path is untouched: unset = exactly
  // the prior behaviour. Only meaningful for Length-domain types (Distance /
  // Width / Position); ignored elsewhere. Reusable by leading later.
  CurvzSpinButton *with_unit_override(Unit u);
  void set_unit_override(Unit u);
  void clear_unit_override();
  bool has_unit_override() const { return m_has_unit_override; }

  // ── Model ─────────────────────────────────────────────────────────────────

  void set_model(const CanvasModel *model);

  // Update ruler origin (e.g. after user moves ruler zero point).
  // Only meaningful for PositionX / PositionY types.
  void set_ruler_origin(double origin);

  // ── Value accessors ───────────────────────────────────────────────────────

  void set_internal_value(double internal);
  double get_internal_value() const { return m_internal; }

  // ── Unit label ────────────────────────────────────────────────────────────

  // Companion label — append to row box after spinbutton.
  // Shows "px", "in", "mm", "pt" for Distance/Width/Position types.
  // Blank for other types.
  Gtk::Label *get_unit_label() { return m_unit_label; }

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

  sigc::signal<void(double)> &signal_internal_changed() {
    return m_signal_internal_changed;
  }

  // ── Scriptable substrate (s295 m1) ───────────────────────────────────────
  //
  // ScriptableWidget routes the universal hide/show/visible surface
  // through final-sealed invoke()/query(); leaves implement *_leaf()
  // with their own verb/property table. See ScriptableWidget.hpp for
  // the dispatch contract.

  curvz::scripting::ScriptValue
  invoke_leaf(std::string_view verb,
              const curvz::scripting::ScriptArgs &args) override;
  curvz::scripting::ScriptValue
  query_leaf(std::string_view property) const override;
  std::vector<std::string> leaf_verbs() const override;
  std::vector<std::string> leaf_properties() const override;

protected:
  void bind_canonical() override;

private:
  SpinType m_type;
  const CanvasModel *m_model;
  double m_internal = 0.0;
  double m_ruler_origin = 0.0; // PositionX/Y only
  bool m_updating = false;

  // s331 — type-domain unit override (see with_unit_override). When set, this
  // spinner pins to m_unit_override instead of the doc display unit.
  bool m_has_unit_override = false;
  Unit m_unit_override = Unit::Pt;

  Glib::RefPtr<Gtk::Adjustment> m_adj;
  Gtk::Label *m_unit_label = nullptr;

  sigc::signal<void(double)> m_signal_internal_changed;

  // Error popover — lazily constructed. Held as Glib::RefPtr so GTK
  // manages lifetime across transient dismissal / reshow.
  Gtk::Popover *m_err_popover = nullptr;
  Gtk::Label *m_err_label = nullptr;
  sigc::connection m_err_hide_timer;

  void init();
  void apply_unit_params();
  void update_unit_label();
  bool is_distance_type() const;
  double to_internal(double display) const;
  void on_value_changed();

  // Expression parsing
  bool type_allows_units() const;
  Unit type_default_unit() const;
  // s263 m5 — returns the parser Domain matching this spinner's SpinType.
  // Used by both the parser (for legal-suffix sets) and the keystroke
  // filter (for which letter keys reach the entry buffer). Replaces the
  // older binary `type_allows_units` discriminator at the use sites;
  // `type_allows_units` is retained as a thin wrapper for source-compat
  // and returns true iff domain is Length.
  Domain type_domain() const;
  bool try_commit_text(const std::string &txt); // returns true on success
  void show_error_popover(const std::string &bad_input, const std::string &msg);
  void hide_error_popover();
  // s263 m5 — keystroke filter is now domain-aware. The legacy bool
  // overload routes through Length/Dimensionless for source-compat; new
  // call sites pass Domain directly.
  static bool is_char_allowed(gunichar ch, Domain domain);
  static bool is_char_allowed(gunichar ch, bool units_allowed);
};

} // namespace Curvz
