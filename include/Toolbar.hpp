#pragma once
#include "ColorPickerPopover.hpp"
#include "CurvzDocument.hpp"
#include "CurvzEntry.hpp"
#include "CurvzSwitch.hpp"
#include "UnitSystem.hpp"
#include "SceneNode.hpp"
#include <functional>
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
  Ruler,
  TextOnPath
};

class Toolbar : public Gtk::Box {
public:
  Toolbar();

  using ToolChangedSignal = sigc::signal<void(ActiveTool)>;
  ToolChangedSignal &signal_tool_changed() { return m_signal_tool_changed; }

  ActiveTool active_tool() const { return m_active; }
  void set_active_tool_icon(ActiveTool tool, const char *icon_name);
  void select_tool(ActiveTool tool);
  void cycle_tool(int dir); // +1 = next, -1 = previous

  // ── Defaults well ────────────────────────────────────────────────────
  const FillStyle &default_fill() const { return m_def_fill; }
  const StrokeStyle &default_stroke() const { return m_def_stroke; }
  void sync_from_object(const FillStyle &fill, const StrokeStyle &stroke);
  // S58n: Multi-select-aware sync. Checks selection uniformity on fill and
  // stroke; when mixed, the well and popover swatches paint diagonal
  // stripes matching the Inspector Appearance convention. Falls back to
  // sync_from_object behaviour when selection has <= 1 target.
  void sync_from_selection(const std::vector<SceneNode*>& sel,
                           SceneNode* primary);

  using DefaultsSignal = sigc::signal<void(FillStyle, StrokeStyle)>;
  DefaultsSignal &signal_defaults_changed() { return m_sig_defaults; }

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
  GradientEditSignal &signal_gradient_edit_requested() {
      return m_sig_gradient_edit;
  }

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
  SnapToggleSignal &signal_snap_toggled() { return m_sig_snap; }
  void set_snap_enabled(bool enabled);

  // Snap settings popover
  using SnapSettingsSignal = sigc::signal<void(SnapSettings)>;
  SnapSettingsSignal &signal_snap_settings_changed() { return m_sig_snap_settings; }
  void set_snap_settings(const SnapSettings &s);

  using SnapPopOpenSignal = sigc::signal<void()>;
  SnapPopOpenSignal &signal_snap_pop_open() { return m_sig_snap_pop_open; }

  // ── Align & Distribute ───────────────────────────────────────────────
  using AlignSignal = sigc::signal<void(AlignOp)>;
  AlignSignal &signal_align_requested() { return m_sig_align; }

  using MacroSignal = sigc::signal<void()>;
  MacroSignal &signal_macro_manager() { return m_sig_macro; }
  // Toolbar macro button: left-click runs the current macro (Ctrl+M
  // semantics), right-click opens the manager (Ctrl+Shift+M semantics).
  MacroSignal &signal_macro_run() { return m_sig_macro_run; }
  // Enable/disable the align button (call when selection changes)
  void set_align_enabled(bool enabled);

  // ── Zoom controls ────────────────────────────────────────────────────
  void set_zoom(double zoom);       // update the readout field
  void set_zoom_alt(bool alt_down); // swap zoom-in/out icon
  void set_popup_unit(Unit u);      // update "Units: x" label in all shape popovers (legacy, kept for direct calls)
  void set_document(CurvzDocument* doc); // live doc pointer — used by popovers for unit + canvas size
  Gtk::ToggleButton* get_corner_btn() { return m_corner_tool_btn; }

  using FitSignal = sigc::signal<void()>;
  using ZoomSignal = sigc::signal<void(double)>;
  using ZoomToSignal = sigc::signal<void(double /*raw zoom factor*/)>;
  using CanvasFocusSignal = sigc::signal<void()>;
  using PlaceRefSignal = sigc::signal<void(double /*x*/, double /*y*/)>;
  using PlaceRectSignal = sigc::signal<void(double /*x*/, double /*y*/,
                                            double /*w*/, double /*h*/)>;
  using PlaceEllipseSignal = sigc::signal<void(double /*cx*/, double /*cy*/,
                                               double /*rx*/, double /*ry*/)>;
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
  FitSignal &signal_fit_requested() { return m_sig_fit; }
  ZoomSignal &signal_zoom_step() { return m_sig_zoom_step; }
  ZoomToSignal &signal_zoom_to() { return m_sig_zoom_to; }
  PlaceRefSignal &signal_place_ref() { return m_sig_place_ref; }
  PlaceRectSignal &signal_place_rect() { return m_sig_place_rect; }
  PlaceEllipseSignal &signal_place_ellipse() { return m_sig_place_ellipse; }
  PlaceLineSignal &signal_place_line() { return m_sig_place_line; }
  PlaceTextSignal &signal_place_text() { return m_sig_place_text; }
  PlacePolygonSignal &signal_place_polygon() { return m_sig_place_polygon; }
  PlaceSpiralSignal &signal_place_spiral() { return m_sig_place_spiral; }
  CanvasFocusSignal &signal_request_canvas_focus() {
    return m_sig_canvas_focus;
  }

  // ── Polygon tool settings (read by Canvas during drag) ───────────────
  int polygon_sides() const { return m_poly_sides; }
  double polygon_inflection() const { return m_poly_inflection; }
  double spiral_turns() const { return m_spiral_turns; }
  double spiral_inner_pct() const { return m_spiral_inner; }

private:
  void add_tool_button(const char *icon, const char *tooltip, ActiveTool tool);
  void add_tool_button(Gtk::Picture *pic, const char *tooltip, ActiveTool tool);

  // Well
  void build_defaults_well();
  void build_fill_popover();
  void build_stroke_popover();
  void redraw_well();
  void update_cap_buttons();
  void update_join_buttons();
  // Cap/Join buttons — icon buttons, active state via CSS
  Gtk::Button* m_cap_butt_btn   = nullptr;
  Gtk::Button* m_cap_round_btn  = nullptr;
  Gtk::Button* m_cap_square_btn = nullptr;
  Gtk::Button* m_join_miter_btn = nullptr;
  Gtk::Button* m_join_round_btn = nullptr;
  Gtk::Button* m_join_bevel_btn = nullptr;
  void apply_hex_to_fill(const std::string &hex);
  void apply_hex_to_stroke(const std::string &hex);
  void refresh_fill_popover();
  void refresh_stroke_popover();
  void reset_to_defaults();
  void emit_defaults();

  // Tool buttons
  ActiveTool m_active = ActiveTool::Selection;
  ToolChangedSignal m_signal_tool_changed;
  std::vector<Gtk::ToggleButton *> m_buttons;
  Gtk::ToggleButton* m_corner_tool_btn = nullptr;
  std::vector<ActiveTool> m_button_tools;

  double m_zoom = 1.0;

  // Zoom popover (Ctrl+click zoom tool)
  Gtk::Popover m_zoom_pop;
  Glib::RefPtr<Gtk::Adjustment> m_zoom_adj;
  Gtk::SpinButton m_zoom_spin;

  // Ref tool placement popover (Ctrl+click)
  Gtk::Popover m_ref_pop;
  Glib::RefPtr<Gtk::Adjustment> m_ref_adj_x;
  Glib::RefPtr<Gtk::Adjustment> m_ref_adj_y;
  Gtk::Label* m_ref_unit_lbl = nullptr;
  void build_zoom_popover(Gtk::ToggleButton *zoom_btn);
  void build_ref_popover(Gtk::ToggleButton *ref_btn);

  // Rect placement popover (Ctrl+click)
  Gtk::Popover m_rect_pop;
  Glib::RefPtr<Gtk::Adjustment> m_rect_adj_x, m_rect_adj_y, m_rect_adj_w,
      m_rect_adj_h;
  Gtk::Label* m_rect_unit_lbl = nullptr;
  void build_rect_popover(Gtk::ToggleButton *btn);

  // Ellipse placement popover (Ctrl+click)
  Gtk::Popover m_ellipse_pop;
  Glib::RefPtr<Gtk::Adjustment> m_ellipse_adj_x, m_ellipse_adj_y,
      m_ellipse_adj_w, m_ellipse_adj_h;
  Gtk::Label* m_ellipse_unit_lbl = nullptr;
  void build_ellipse_popover(Gtk::ToggleButton *btn);

  // Polygon placement popover (Ctrl+click)
  Gtk::Popover m_poly_pop;
  Glib::RefPtr<Gtk::Adjustment> m_poly_adj_cx, m_poly_adj_cy, m_poly_adj_r;
  Glib::RefPtr<Gtk::Adjustment> m_poly_adj_sides, m_poly_adj_inflect;
  Gtk::DrawingArea m_poly_preview;
  int m_poly_sides = 6;
  double m_poly_inflection = 1.0;
  bool m_poly_hdl_drag = false;
  double m_poly_hdl_start_inflect = 1.0;
  Gtk::Label* m_poly_unit_lbl = nullptr;

  // ── Spiral popover state ──────────────────────────────────────────────
  Gtk::Popover m_spiral_pop;
  Glib::RefPtr<Gtk::Adjustment> m_spiral_adj_cx, m_spiral_adj_cy,
      m_spiral_adj_r;
  Glib::RefPtr<Gtk::Adjustment> m_spiral_adj_turns, m_spiral_adj_inner;
  Gtk::DrawingArea m_spiral_preview;
  // S98: defaults tuned for nautilus look — 3 turns, 4% inner radius
  // gives a growth ratio of (1/0.04)^(1/3) ≈ 2.92× per turn, close to
  // a real nautilus.
  double m_spiral_turns = 3.0;
  double m_spiral_inner = 4.0;
  Gtk::Label* m_spiral_unit_lbl = nullptr;
  void build_polygon_popover(Gtk::ToggleButton *btn);
  void build_spiral_popover(Gtk::ToggleButton *btn);

  // Line placement popover (Ctrl+click)
  Gtk::Popover m_line_pop;
  Glib::RefPtr<Gtk::Adjustment> m_line_adj_x1, m_line_adj_y1, m_line_adj_x2,
      m_line_adj_y2;
  Gtk::Label* m_line_unit_lbl = nullptr;
  void build_line_popover(Gtk::ToggleButton *btn);

  // Current popup display unit
  Unit m_popup_unit = Unit::Px;
  CurvzDocument* m_doc = nullptr;  // live doc pointer for popover defaults

  // Text placement popover (Ctrl+click)
  Gtk::Popover m_text_pop;
  Glib::RefPtr<Gtk::Adjustment> m_text_adj_x, m_text_adj_y, m_text_adj_size;
  Gtk::Label* m_text_unit_lbl = nullptr;
  Gtk::DropDown *m_text_family_drop = nullptr; // font family selector
  Gtk::CheckButton m_text_bold_btn;
  Gtk::CheckButton m_text_italic_btn;
  Gtk::DropDown *m_text_anchor_drop = nullptr;
  void build_text_popover(Gtk::ToggleButton *btn);

  // The only toolbar-column well widget
  Gtk::DrawingArea m_well;

  // Fill popover
  Gtk::Popover m_fill_pop;
  Gtk::ToggleButton m_fill_type_solid_btn;
  Gtk::ToggleButton m_fill_type_none_btn;
  Gtk::ToggleButton m_fill_type_cc_btn;
  Gtk::ToggleButton m_fill_type_swatch_btn;  // S87 m1 v2 — 4th type toggle
  Gtk::ToggleButton m_fill_type_gradient_btn;  // S91 — 5th type toggle
  CurvzEntry m_fill_hex_entry;
  Gtk::DrawingArea m_fill_swatch;
  // S91 fill gradient row: [ramp ─────][Edit…]. Visible only when
  // m_def_fill.is_gradient(). Same idiom as the colour row above —
  // member-pointer Box so refresh_fill_popover can drive visibility.
  Gtk::Box*        m_fill_gradient_row  = nullptr;
  Gtk::DrawingArea m_fill_gradient_ramp;
  Gtk::Button      m_fill_gradient_edit_btn;

  // Stroke popover
  Gtk::Popover m_stroke_pop;
  Gtk::ToggleButton m_stroke_type_solid_btn;
  Gtk::ToggleButton m_stroke_type_none_btn;
  Gtk::ToggleButton m_stroke_type_cc_btn;
  Gtk::ToggleButton m_stroke_type_swatch_btn;  // S87 m1 v2 — 4th type toggle
  CurvzEntry m_stroke_hex_entry;
  Gtk::DrawingArea m_stroke_swatch;
  // S91: stroke does NOT get a gradient toggle. Gradient strokes are
  // valid SVG but rare, and the cost of a separate stroke-side gradient
  // surface (toggle handler, popover row, well frame rendering) isn't
  // justified. Fill-only is the shipped surface.
  Gtk::SpinButton m_width_spin;
  Glib::RefPtr<Gtk::Adjustment> m_width_adj;
  Gtk::Label      m_width_unit_lbl;
  Gtk::Label      m_width_label;  // "Width (in):" label — updated with unit
  // (cap/join buttons stored as Gtk::Button* above)

  FillStyle m_def_fill;
  StrokeStyle m_def_stroke;
  // S58n: true when the current multi-selection has heterogeneous fill /
  // stroke paint. Drives diagonal-stripe rendering in the well and popover
  // swatches, and deactivates all type toggles in the popovers.
  bool m_fill_mixed = false;
  bool m_stroke_mixed = false;
  bool m_syncing = false;

  // ── S87 m1: swatch picker section (copy-only, fill + stroke) ─────────
  //
  // Chip-grid picker tucked inside each existing popover, below the
  // type/colour rows and before the popover-specific footer (stroke has
  // width + cap/join below; fill ends at the colour row). Visible only
  // when m_swatch_library != nullptr. Click on a chip = "write this
  // swatch's RGB into m_def_* as a Solid paint" — i.e. equivalent to
  // pasting a hex into the entry. No binding state on the toolbar.
  //
  // m_swatch_library is non-owning; MainWindow plumbs it after project
  // load via set_swatch_library() and again on project switch. Same
  // pattern as StylesPanel::set_swatch_library (S85 cont-3 fix4).
  const color::SwatchLibrary* m_swatch_library = nullptr;

  // Per-popover picker widgets. Stored as pointers because they're
  // owned (managed) by their host Box — we keep references for
  // refresh_*_popover and the rebuild path. Set to non-null in the
  // build_*_popover methods, then live for the toolbar's lifetime.
  Gtk::Box*      m_fill_picker_section   = nullptr;
  Gtk::DropDown* m_fill_palette_dd       = nullptr;
  Gtk::FlowBox*  m_fill_chip_flow        = nullptr;
  Gtk::Box*      m_stroke_picker_section = nullptr;
  Gtk::DropDown* m_stroke_palette_dd     = nullptr;
  Gtk::FlowBox*  m_stroke_chip_flow      = nullptr;

  // S87 m1 v2: pointers to the colour rows (chip + hex entry) so the
  // refresh handler can show/hide them based on the active type. The
  // rows themselves are Boxes built in build_*_popover and appended to
  // outer; we promoted the locals to members to drive visibility.
  Gtk::Box*      m_fill_color_row        = nullptr;
  Gtk::Box*      m_stroke_color_row      = nullptr;

  // S87 m1 v2: per-popover "Swatch tab is showing" flag. Distinct from
  // m_def_*.type — the type stays Solid/None/CC, but the user has
  // chosen to see the picker. Mirrors PaintEditor's `is_swatch_active`
  // RenderState flag, kept locally because the Toolbar is its own host.
  // Reset to false whenever the user clicks Solid / None / currentColor.
  bool m_fill_picker_open   = false;
  bool m_stroke_picker_open = false;

  // Shadow vectors tracking each dropdown's row order so selection-
  // changed maps row index → palette id without a name lookup. Mirrors
  // PaintEditor::m_picker_palette_ids. Includes the "__all__" sentinel
  // at row 0 (synthetic "All" pseudo-palette walking every swatch).
  std::vector<std::string> m_fill_palette_ids;
  std::vector<std::string> m_stroke_palette_ids;

  // Connections for dropdown property_selected. Disconnected before
  // programmatic re-selection inside rebuild_palette_dropdown to avoid
  // firing user-flow handlers — same idiom as PaintEditor.
  sigc::connection m_fill_palette_dd_conn;
  sigc::connection m_stroke_palette_dd_conn;

  // S87 m1 fix2: tracks the palette id the chip grid currently shows.
  // The dropdown's property_selected can spurious-fire (handoff gotcha:
  // "Spurious signal_changed fires on dropdowns after panel rebuilds").
  // Without this guard, every spurious fire rebuilt the chip grid,
  // tearing down + recreating gesture controllers — producing an
  // asymmetric chip-click-then-popdown bug where the first click after
  // a rebuild dismissed cleanly but subsequent clicks didn't. Guard
  // pattern: skip rebuild_chip_grid when the requested palette id
  // matches what's already drawn.
  std::string m_fill_chips_palette_id;
  std::string m_stroke_chips_palette_id;

  // S87 m1 helpers — per-slot. is_stroke selects which set of members
  // to operate on. Keeps the build / rebuild code from duplicating.
  void build_swatch_picker_section(Gtk::Box& outer, bool is_stroke);
  void rebuild_swatch_pickers();         // re-runs both dropdown + chips
  void rebuild_palette_dropdown(bool is_stroke);
  void rebuild_chip_grid(bool is_stroke);
  void apply_swatch_pick_to_fill(const std::string& swatch_id);
  void apply_swatch_pick_to_stroke(const std::string& swatch_id);

  // Shared colour-picker popover for fill + stroke wells. One instance,
  // reused for both: set_initial() + the per-open callback determines
  // which target is active. Attached to `*this` (the Toolbar) in the
  // ctor. Anchored to m_well at open time.
  //
  // The old design dismissed m_fill_pop / m_stroke_pop before opening a
  // modal Gtk::ColorDialog. We keep that dismissal (the user loses the
  // mode-buttons context while picking), because nesting popovers gets
  // hairy with autohide and Phase 2 chose conservative over clever.
  // A later phase can embed CurvzColorPicker directly inside m_fill_pop
  // if desired.
  ColorPickerPopover m_color_popover;

  DefaultsSignal m_sig_defaults;
  GradientEditSignal m_sig_gradient_edit;
  FitSignal m_sig_fit;
  ZoomSignal m_sig_zoom_step;
  ZoomToSignal m_sig_zoom_to;
  PlaceRefSignal m_sig_place_ref;
  PlaceRectSignal m_sig_place_rect;
  PlaceEllipseSignal m_sig_place_ellipse;
  PlaceLineSignal m_sig_place_line;
  PlaceTextSignal m_sig_place_text;
  PlacePolygonSignal m_sig_place_polygon;
  PlaceSpiralSignal m_sig_place_spiral;
  CanvasFocusSignal m_sig_canvas_focus;
  SnapToggleSignal m_sig_snap;
  SnapSettingsSignal m_sig_snap_settings;
  SnapPopOpenSignal m_sig_snap_pop_open;
  CurvzSwitch m_snap_switch;

  // Snap settings popover
  Gtk::Popover m_snap_pop;
  Gtk::CheckButton *m_snap_cb_guides  = nullptr;
  Gtk::CheckButton *m_snap_cb_grid    = nullptr;
  Gtk::CheckButton *m_snap_cb_margins = nullptr;
  Gtk::CheckButton *m_snap_cb_nodes   = nullptr;
  Gtk::CheckButton *m_snap_cb_edges   = nullptr;
  Gtk::CheckButton *m_snap_cb_centers = nullptr;
  bool m_snap_loading = false;
  void build_snap_popover(Gtk::Widget *widget);

  // Align & Distribute
  void build_align_button();
  void build_align_popover();
  Gtk::Button m_align_btn;
  Gtk::Popover m_align_pop;
  AlignSignal m_sig_align;
  MacroSignal m_sig_macro;
  MacroSignal m_sig_macro_run;
  // 8 icon drawing areas for the popover
  Gtk::DrawingArea m_align_da[8];

  // S117 m5: align icon is Cairo-painted (not CSS), so it can't read
  // motif tokens directly. We mirror the ruler approach: a per-instance
  // motif member, set_motif() forwarded from MainWindow's
  // apply_motif_to_window(), and a small palette struct in Toolbar.cpp
  // that picks colours based on motif. queue_draw on the main button
  // and all 8 popover DAs forces an immediate repaint.
  Motif m_motif = Motif::Dark;
public:
  void set_motif(Motif m) {
    if (m_motif == m) return;
    m_motif = m;
    m_align_btn.queue_draw();
    for (auto &da : m_align_da) da.queue_draw();
  }
};

} // namespace Curvz
