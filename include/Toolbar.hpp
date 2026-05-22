#pragma once
#include "ColorPickerPopover.hpp"
#include "CurvzDocument.hpp"
#include "CurvzEntry.hpp"
#include "UnitSystem.hpp"
#include "SceneNode.hpp"
#include <functional>
#include <memory>
#include <gtkmm/adjustment.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/label.h>
#include <gtkmm/picture.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/togglebutton.h>
#include <vector>

namespace Curvz {

// Forward decl — toolbar holds a non-owning pointer to the project's
// swatch library to render copy-only swatch picker sections inside the
// fill/stroke popovers (S87 m1: doc-scoped swatch slots, toolbar bite).
namespace color {
class SwatchLibrary;
}

// ── Align / Distribute operations ────────────────────────────────────────────
enum class AlignOp {
  // Align edges / centres to selection bounding box
  AlignLeft,
  AlignCenterH,
  AlignRight,
  AlignTop,
  AlignCenterV,
  AlignBottom,
  // Distribute with equal gaps between objects
  DistributeH,
  DistributeV,
};

// ── Boolean path operations (s154 m1) ────────────────────────────────────────
// Selected via the Boolean toolbar button's left-click popover. MainWindow
// receives signal_bool_op(BoolOp) and dispatches to the same handler the
// Path menu items already use (m_act_bool_union/subtract/intersect).
enum class BoolOp {
  Union,
  Subtract,
  Intersect,
};

enum class ActiveTool {
  Selection,
  Node,
  Pen,
  Rect,
  Ellipse,
  Line,
  Ref,
  Text,
  Eyedropper,
  Zoom,
  Corner,
  Polygon,
  Spiral,
  Measure,   // s150: was "Ruler". The tool measures; the canvas
             // edge strips that show tick marks are still rulers.
             // Industry convention (Illustrator, Affinity, AutoCAD,
             // Inkscape) names this tool "Measure".
  TextOnPath
};

class Toolbar : public Gtk::Box {
public:
  Toolbar();
  ~Toolbar();   // s152 sub-ship 1: defined in .cpp where Impl is complete
                // (required for unique_ptr<Impl> with forward-declared Impl)

  using ToolChangedSignal = sigc::signal<void(ActiveTool)>;
  ToolChangedSignal& signal_tool_changed();

  ActiveTool active_tool() const;   // forwarder; defined in .cpp
  void set_active_tool_icon(ActiveTool tool, const char *icon_name);
  void select_tool(ActiveTool tool);
  void cycle_tool(int dir); // +1 = next, -1 = previous

  // ── s152: Toolbar density ──────────────────────────────────────────────
  //
  // Four sizing presets. The toolbar shrinks/expands by flipping a
  // CSS class on the .toolbar-panel root — no widget rebuild, GTK
  // relayouts on the class change.
  //
  //   Comfortable  48px column, 40×40 buttons   (curvz historical)
  //   Standard     40px column, 32×32 buttons   (curvz default — s152)
  //   Compact      32px column, 28×28 buttons
  //   Tight        28px column, 24×24 buttons
  //
  // Right-click on empty toolbar background opens a small popover
  // letting the user pick density immediately. AppPreferences carries
  // the persisted default; MainWindow applies it on startup and the
  // user's pick saves back through AppPreferences.
  enum class Density { Comfortable, Standard, Compact, Tight };
  void set_density(Density d);
  Density density() const;   // forwarder; defined in .cpp

  // s152 — Emitted when the user picks a density via the right-click
  // popover. MainWindow listens to persist the pick into AppPreferences.
  // set_density() called programmatically (e.g. on startup from the
  // persisted value) does NOT emit this signal — silent path for
  // host-driven sync.
  using DensitySignal = sigc::signal<void(Density)>;
  DensitySignal& signal_density_changed();

  // ── Defaults well ────────────────────────────────────────────────────
  // s153 sub-ship 3: state moved to Impl. Accessors are forwarders
  // defined in Toolbar.cpp where Impl is complete.
  const FillStyle& default_fill() const;
  const StrokeStyle& default_stroke() const;
  void sync_from_object(const FillStyle &fill, const StrokeStyle &stroke);
  // S58n: Multi-select-aware sync. Checks selection uniformity on fill and
  // stroke; when mixed, the well and popover swatches paint diagonal
  // stripes matching the Inspector Appearance convention. Falls back to
  // sync_from_object behaviour when selection has <= 1 target.
  void sync_from_selection(const std::vector<SceneNode*>& sel,
                           SceneNode* primary);

  using DefaultsSignal = sigc::signal<void(FillStyle, StrokeStyle)>;
  DefaultsSignal& signal_defaults_changed();

  // S91: User clicked Edit gradient inside the Fill or Stroke popover.
  // The toolbar packages "what to do on Apply" as a callback that writes
  // the edited FillStyle into m_def_fill / m_def_stroke.paint and
  // re-emits defaults_changed. MainWindow owns the GradientDialog
  // instance (same one the inspector uses) and routes show() through
  // this signal.
  //
  // No undo plumbing here — toolbar defaults are session-only state,
  // matching the Solid-edit pathway for the toolbar (m_def_fill writes
  // never push an EditAppearanceCommand). The dialog's Apply just
  // updates the live defaults; the next placement uses them.
  using GradientEditSignal =
      sigc::signal<void(FillStyle /*current*/,
                        std::function<void(FillStyle /*edited*/)> /*apply_cb*/)>;
  GradientEditSignal& signal_gradient_edit_requested();

  // S87 m1 (doc-scoped slot swatches — toolbar bite): wire the project's
  // swatch library so the fill/stroke popovers can show a copy-only
  // chip-grid picker section. Pass nullptr to hide the section (e.g.
  // before a project is loaded, or on project teardown). The pointer is
  // non-owning; the host (MainWindow) guarantees the library outlives
  // the toolbar.
  //
  // Copy-only semantics: clicking a chip writes the swatch's RGB into
  // m_def_fill / m_def_stroke as a Solid paint and emits defaults_changed.
  // No binding is created — toolbar defaults are session-only state, so
  // a binding wouldn't survive next session anyway. For per-document
  // colour slots (guides / grid / margins) the s87 plan calls for full
  // binding via PaintEditor; that's the next milestone.
  void set_swatch_library(const color::SwatchLibrary* lib);

  using SnapToggleSignal = sigc::signal<void(bool)>;
  SnapToggleSignal& signal_snap_toggled();
  void set_snap_enabled(bool enabled);

  // Snap settings popover
  using SnapSettingsSignal = sigc::signal<void(SnapSettings)>;
  SnapSettingsSignal& signal_snap_settings_changed();
  void set_snap_settings(const SnapSettings &s);

  using SnapPopOpenSignal = sigc::signal<void()>;
  SnapPopOpenSignal& signal_snap_pop_open();

  // s150 — Ruler / Measure settings signal. Fires when the user toggles
  // either of the two measure prefs in the Ruler tool's right-click
  // popover. Writes already happened directly on m_doc; this signal
  // exists so MainWindow can schedule_save() and perform any cross-doc
  // mirroring (currently none — measure prefs are per-doc only).
  using MeasureSettingsSignal = sigc::signal<void()>;
  MeasureSettingsSignal& signal_measure_settings_changed();

  // ── Align & Distribute ───────────────────────────────────────────────
  using AlignSignal = sigc::signal<void(AlignOp)>;
  AlignSignal& signal_align_requested();

  // ── Transforms section (s154 m1) ─────────────────────────────────────
  // New cluster between the top creation tools and the lower tools:
  // Step-and-Repeat, Blend, Boolean Ops, Warp. Left-click on the first
  // three fires the matching signal (M1: equivalent to the existing
  // menu-item handler). Boolean is a single button whose left-click
  // pops a small horizontal popover with three icon buttons (union /
  // subtract / intersect); selection emits signal_bool_op(BoolOp).
  // Right-click on SnR/Blend/Warp pops a placeholder configuration
  // popover (real config content lands in M2/M3/M4 when the existing
  // dialogs convert into popovers).
  using StepRepeatSignal = sigc::signal<void()>;
  using BlendSignal      = sigc::signal<void()>;
  using WarpSignal       = sigc::signal<void()>;
  using BoolOpSignal     = sigc::signal<void(BoolOp)>;
  StepRepeatSignal& signal_step_repeat();
  BlendSignal&      signal_blend();
  WarpSignal&       signal_warp();
  BoolOpSignal&     signal_bool_op();

  // s154 m2: Right-click on the SnR toolbar button. Replaces the M1
  // placeholder cfg popover for SnR. MainWindow handles by calling
  // StepRepeatPopover::show(button, ...). Blend/Warp follow the same
  // pattern when their popovers land in M3/M4; for now they keep the
  // placeholder right-click popover.
  using StepRepeatConfigureSignal = sigc::signal<void()>;
  StepRepeatConfigureSignal& signal_step_repeat_configure();

  // s154 m3: Right-click on the Blend toolbar button. Same shape as
  // signal_step_repeat_configure — MainWindow calls
  // BlendPopover::show(button, ...). Replaces the M1 placeholder for
  // Blend; the placeholder pattern remains for Warp until M4.
  using BlendConfigureSignal = sigc::signal<void()>;
  BlendConfigureSignal& signal_blend_configure();

  // s154 m4a: Right-click on the Warp toolbar button. MainWindow opens
  // WarpPopover (a defaults-editor mirroring AppPreferences fields, in
  // parallel with the inspector's Application ▸ Warp subsection for a
  // comparison trial — last write wins, no live sync).
  using WarpConfigureSignal = sigc::signal<void()>;
  WarpConfigureSignal& signal_warp_configure();

  // s154 m2: SnR toolbar button accessor — MainWindow's StepRepeatPopover
  // anchors to this widget. Mirrors get_corner_btn()'s pattern.
  Gtk::Widget& step_repeat_button();

  // s154 m3: Blend toolbar button accessor.
  Gtk::Widget& blend_button();

  // s154 m4a: Warp toolbar button accessor.
  Gtk::Widget& warp_button();

  // s154 m1: Mirrors set_align_enabled's faked-disabled pattern. Each
  // flag drives a single button's .tool-btn-disabled CSS class and
  // gates the left-click action; right-click stays live so the
  // configuration popover is reachable even when the action is
  // disabled by selection state. MainWindow calls this from the same
  // selection-change handlers that update m_act_bool_*/blend/warp/
  // step_repeat sensitivity (Path menu mirror).
  void set_transforms_enabled(bool snr, bool blend,
                              bool boolop, bool warp);

  using MacroSignal = sigc::signal<void()>;
  MacroSignal& signal_macro_manager();
  // Toolbar macro button: left-click runs the current macro (Ctrl+M
  // semantics), right-click opens the manager (Ctrl+Shift+M semantics).
  MacroSignal& signal_macro_run();
  // Enable/disable the align button (call when selection changes)
  void set_align_enabled(bool enabled);

  // ── Zoom controls ────────────────────────────────────────────────────
  void set_zoom(double zoom);       // update the readout field
  void set_zoom_alt(bool alt_down); // swap zoom-in/out icon
  void set_popup_unit(Unit u);      // update "Units: x" label in all shape popovers (legacy, kept for direct calls)
  void set_document(CurvzDocument* doc); // live doc pointer — used by popovers for unit + canvas size
  Gtk::ToggleButton* get_corner_btn();   // forwarder; defined in .cpp

  using FitSignal = sigc::signal<void()>;
  using ZoomSignal = sigc::signal<void(double)>;
  using ZoomToSignal = sigc::signal<void(double /*raw zoom factor*/)>;
  using CanvasFocusSignal = sigc::signal<void()>;
  using PlaceRefSignal = sigc::signal<void(double /*x*/, double /*y*/)>;
  using PlaceRectSignal = sigc::signal<void(double /*x*/, double /*y*/,
                                            double /*w*/, double /*h*/)>;
  using PlaceEllipseSignal = sigc::signal<void(double /*x*/, double /*y*/,
                                               double /*w*/, double /*h*/)>;
  using PlaceLineSignal = sigc::signal<void(double /*x1*/, double /*y1*/,
                                            double /*x2*/, double /*y2*/)>;
  using PlaceTextSignal =
      sigc::signal<void(double /*x*/, double /*y*/, std::string /*family*/,
                        double /*size*/, bool /*bold*/, bool /*italic*/,
                        std::string /*anchor*/, std::string /*align*/)>;
  using PlacePolygonSignal = sigc::signal<void(
      double /*cx*/, double /*cy*/, double /*radius*/, int /*sides*/,
      double /*inflection*/, double /*angle_rad*/)>;
  using PlaceSpiralSignal = sigc::signal<void(
      double /*cx*/, double /*cy*/, double /*outer_r*/, double /*inner_r*/,
      double /*turns*/, double /*angle_rad*/)>;
  FitSignal& signal_fit_requested();
  ZoomSignal& signal_zoom_step();
  ZoomToSignal& signal_zoom_to();
  PlaceRefSignal& signal_place_ref();
  PlaceRectSignal& signal_place_rect();
  PlaceEllipseSignal& signal_place_ellipse();
  PlaceLineSignal& signal_place_line();
  PlaceTextSignal& signal_place_text();
  PlacePolygonSignal& signal_place_polygon();
  PlaceSpiralSignal& signal_place_spiral();
  CanvasFocusSignal& signal_request_canvas_focus();

  // ── Polygon tool settings (read by Canvas during drag) ───────────────
  // s153 sub-ship 2e: state moved to Impl. These are now forwarders
  // defined in Toolbar.cpp where Impl is complete.
  int polygon_sides() const;
  double polygon_inflection() const;
  double spiral_turns() const;
  double spiral_inner_pct() const;

  // S117 m5: align icon is Cairo-painted, so it can't read motif tokens
  // directly. Forwarder; body in Toolbar.cpp where Impl is complete.
  void set_motif(Motif m);

private:
  // ── pImpl ──────────────────────────────────────────────────────────────
  //
  // The public surface above is the contract callers depend on.
  // Implementation detail — every state member, every popover builder,
  // every signal storage slot — lives behind `std::unique_ptr<Impl>`,
  // defined in Toolbar.cpp.
  //
  // Migration arc:
  //   s152 sub-ship 1 — tool buttons + radio bookkeeping + snap toggle
  //                     button + macro button + density into Impl.
  //   s153 sub-ship 2 — all 9 placement popovers (zoom, rect, ellipse,
  //                     line, ref, measure, text, polygon, spiral).
  //   s153 sub-ship 3 — paint editor (defaults well, fill/stroke
  //                     popovers, swatch picker section, cap/join,
  //                     width spinner, gradient row, colour picker).
  //   s153 sub-ship 4 — align popover + snap settings popover, plus
  //                     ALL signal storage (Impl owns the signals
  //                     directly; public accessors forward), m_doc,
  //                     m_popup_unit, m_motif. tbproto/ retired.
  //
  // After sub-ship 4 the public Toolbar is a thin wrapper: ctor +
  // dtor + public method declarations + signal accessor declarations
  // + the unique_ptr<Impl>. Header dependents pay only for the public
  // contract; signal type instantiations and widget storage no longer
  // bleed through.
  struct Impl;
  std::unique_ptr<Impl> m_impl;

  friend struct Impl;
};

} // namespace Curvz