#include "Toolbar.hpp"
#include "CurvzLog.hpp"
#include "color/SwatchLibrary.hpp"  // S87 m1 — picker section in popovers
#include "color/Swatch.hpp"         // S87 m1 — SolidSwatch resolve
#include "curvz_utils.hpp"          // s118 — curvz::utils::set_name
#include <algorithm>
#include <cairomm/context.h>
#include <cmath>
#include <gdk/gdkkeysyms.h>
#include <glibmm/main.h>
#include <gtkmm/cellrenderer.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/styleprovider.h>
#include <gtkmm/window.h>
#include <iomanip>
#include <iostream>
#include <pango/pangocairo.h>
#include <sstream>

namespace Curvz {

// ── Helper: canvas width/height in display units ──────────────────────────────
// Returns {w, h} in the document's display unit, correctly handling physical
// mode (where the canvas is defined in physical dimensions, not px).
static std::pair<double,double> canvas_display_size(const CurvzDocument* doc) {
    if (!doc) return {1000.0, 1000.0};
    const CanvasModel& cm = doc->canvas;
    if (cm.display_mode == DisplayMode::Physical) {
        // Return physical dimensions directly — popover values are in phys_unit
        return { cm.phys_width, cm.phys_height };
    }
    // Pixel / RatioQuality
    return { UnitSystem::from_px(doc->canvas_width(),  cm.display_unit),
             UnitSystem::from_px(doc->canvas_height(), cm.display_unit) };
}

// ── Hex helpers
// ───────────────────────────────────────────────────────────────
static std::string color_to_hex(double r, double g, double b) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", (int)std::round(r * 255),
           (int)std::round(g * 255), (int)std::round(b * 255));
  return buf;
}

static bool hex_to_color(const std::string &hex, double &r, double &g,
                         double &b) {
  std::string s = hex;
  if (!s.empty() && s[0] == '#')
    s = s.substr(1);
  if (s.size() != 6)
    return false;
  try {
    r = std::stoi(s.substr(0, 2), nullptr, 16) / 255.0;
    g = std::stoi(s.substr(2, 2), nullptr, 16) / 255.0;
    b = std::stoi(s.substr(4, 2), nullptr, 16) / 255.0;
    return true;
  } catch (...) {
    return false;
  }
}

// =============================================================================
// s152 sub-ship 1 — pImpl skeleton
// =============================================================================
//
// `Toolbar` (the public class declared in Toolbar.hpp) is the *contract* —
// signals, mutators, and getters that callers depend on. `Toolbar::Impl`,
// defined here, owns the implementation detail. Sub-ship 1 moved tool
// buttons + radio bookkeeping + snap toggle button + macro button +
// density into Impl; subsequent sub-ships will move:
//
//   - sub-ship 2: the 8 placement popovers (zoom/ref/rect/ellipse/line/
//     text/polygon/spiral) and the measure right-click popover
//   - sub-ship 3: paint editor (fill/stroke popovers, swatch picker,
//     gradient ramp, cap/join, width spinner, defaults well)
//   - sub-ship 4: align popover, snap settings popover; tbproto retired
//
// Until sub-ship 4 lands, the public Toolbar still owns the popover
// members for those areas — they're untouched by sub-ship 1. The
// pImpl pattern admits incremental migration: members can move into
// Impl one feature at a time without breaking callers.

struct Toolbar::Impl {
    Toolbar* self = nullptr;

    // ── Tool-button state ─────────────────────────────────────────────────
    ActiveTool active = ActiveTool::Selection;
    std::vector<Gtk::ToggleButton*> buttons;
    std::vector<ActiveTool>         button_tools;
    Gtk::ToggleButton*              corner_tool_btn = nullptr;

    // ── Density state ─────────────────────────────────────────────────────
    Toolbar::Density density = Toolbar::Density::Standard;
    Gtk::Popover     density_pop;
    bool             density_pop_built = false;

    // ── Snap toggle button ────────────────────────────────────────────────
    // Replaces the prior CurvzSwitch (Cairo-drawn pill). .tool-btn-styled
    // ToggleButton swapping between curvz-switch-on-symbolic and
    // curvz-switch-off-symbolic. .tb-snap-btn class opts out of the
    // .tool-btn:checked active blue (icon carries the state, not bg).
    Gtk::ToggleButton snap_btn;

    // ── s152 sub-ship 2a: zoom popover ───────────────────────────────────
    // Built lazily by build_zoom_popover() during Impl::build(). Public
    // Toolbar::set_zoom() forwards in to update zoom_level + the
    // Adjustment.
    Gtk::Popover                  zoom_pop;
    Glib::RefPtr<Gtk::Adjustment> zoom_adj;
    Gtk::SpinButton               zoom_spin;
    double                        zoom_level = 1.0;

    // ── s153 sub-ship 2b: rect / ellipse / line popovers ─────────────────
    // Three placement popovers that share the setup_rclick_popover helper
    // and the make_pop_row spin-row helper. Open-time logic seeds the
    // adjustments from canvas size and refreshes the unit label. Public
    // Toolbar::set_popup_unit() still drives all popover unit labels and
    // forwards through to these three (plus the four still in the public
    // class until 2c-2e land).
    Gtk::Popover                  rect_pop;
    Glib::RefPtr<Gtk::Adjustment> rect_adj_x, rect_adj_y, rect_adj_w, rect_adj_h;
    Gtk::Label*                   rect_unit_lbl = nullptr;

    Gtk::Popover                  ellipse_pop;
    Glib::RefPtr<Gtk::Adjustment> ellipse_adj_x, ellipse_adj_y,
                                  ellipse_adj_w, ellipse_adj_h;
    Gtk::Label*                   ellipse_unit_lbl = nullptr;

    Gtk::Popover                  line_pop;
    Glib::RefPtr<Gtk::Adjustment> line_adj_x1, line_adj_y1,
                                  line_adj_x2, line_adj_y2;
    Gtk::Label*                   line_unit_lbl = nullptr;

    // ── s153 sub-ship 2c: ref + measure popovers ─────────────────────────
    // Ref: simple X/Y placement popover, mirrors rect/ellipse shape but
    // built directly (no make_pop_row helper) because it predates that
    // helper and uses its own row layout (set_width_chars(3)).
    // Measure: behaviour-at-the-tool surface (s150) — two checkboxes
    // bound to m_doc->measure_*. Open syncs from doc, toggles write
    // back through and emit signal_measure_settings_changed.
    Gtk::Popover                  ref_pop;
    Glib::RefPtr<Gtk::Adjustment> ref_adj_x, ref_adj_y;
    Gtk::Label*                   ref_unit_lbl = nullptr;

    Gtk::Popover                  measure_pop;
    Gtk::CheckButton*             measure_save_chk = nullptr;
    Gtk::CheckButton*             measure_del_chk  = nullptr;
    Gtk::Label*                   measure_del_lbl  = nullptr;

    // ── s153 sub-ship 2d: text popover ────────────────────────────────────
    // Text placement popover. More state than the other 2b/2c popovers:
    // x/y/size adjustments + font family DropDown + bold/italic
    // CheckButtons (held by value as in the original design) + anchor
    // DropDown. All emit signal_place_text on Place click.
    Gtk::Popover                  text_pop;
    Glib::RefPtr<Gtk::Adjustment> text_adj_x, text_adj_y, text_adj_size;
    Gtk::Label*                   text_unit_lbl     = nullptr;
    Gtk::DropDown*                text_family_drop  = nullptr;
    Gtk::CheckButton              text_bold_btn;
    Gtk::CheckButton              text_italic_btn;
    Gtk::DropDown*                text_anchor_drop  = nullptr;

    // ── s153 sub-ship 2e: polygon + spiral popovers ──────────────────────
    // The two heaviest placement popovers — each owns a 160×160 Cairo
    // preview DrawingArea, motion gestures (polygon's inflection handle
    // drag), and scratch state for the live numeric values that
    // Canvas reads via the public polygon_sides() / polygon_inflection() /
    // spiral_turns() / spiral_inner_pct() forwarders.
    //
    // Polygon: 5 adjustments (sides, inflect, cx, cy, r) + drag-state
    // booleans for the inflection handle.
    // Spiral: 5 adjustments (turns, inner, cx, cy, r). S98 defaults
    // tuned for nautilus look — 3 turns, 4% inner radius.
    Gtk::Popover                  poly_pop;
    Glib::RefPtr<Gtk::Adjustment> poly_adj_cx, poly_adj_cy, poly_adj_r;
    Glib::RefPtr<Gtk::Adjustment> poly_adj_sides, poly_adj_inflect;
    Gtk::DrawingArea              poly_preview;
    int                           poly_sides             = 6;
    double                        poly_inflection        = 1.0;
    bool                          poly_hdl_drag          = false;
    double                        poly_hdl_start_inflect = 1.0;
    Gtk::Label*                   poly_unit_lbl          = nullptr;

    Gtk::Popover                  spiral_pop;
    Glib::RefPtr<Gtk::Adjustment> spiral_adj_cx, spiral_adj_cy, spiral_adj_r;
    Glib::RefPtr<Gtk::Adjustment> spiral_adj_turns, spiral_adj_inner;
    Gtk::DrawingArea              spiral_preview;
    double                        spiral_turns           = 3.0;
    double                        spiral_inner           = 4.0;
    Gtk::Label*                   spiral_unit_lbl        = nullptr;

    explicit Impl(Toolbar* owner) : self(owner) {}

    // ── Construction ──────────────────────────────────────────────────────
    void build();

    // ── Tool-button building blocks ──────────────────────────────────────
    void add_tool_button(const char* icon, const char* tooltip, ActiveTool tool);
    void add_tool_button(Gtk::Picture* pic, const char* tooltip, ActiveTool tool);
    // s175 m4: build-but-don't-append overload. Returns the button so the
    // caller can place it manually (e.g. inside build_transforms_section
    // where alphabetical ordering of mixed widget types — toggle buttons
    // and plain buttons — needs the caller to control append order).
    Gtk::ToggleButton* make_tool_button(const char* icon, const char* tooltip,
                                        ActiveTool tool);

    // ── Tool selection (the radio invariant) ─────────────────────────────
    void select_tool(ActiveTool tool);
    void cycle_tool(int dir);
    void set_active_tool_icon(ActiveTool tool, const char* icon_name);

    // ── Density ──────────────────────────────────────────────────────────
    void set_density(Toolbar::Density d);
    void build_density_popover();

    // ── Popovers (sub-ship 2 in progress; one at a time) ────────────────
    void build_zoom_popover(Gtk::ToggleButton* zoom_btn);
    void set_zoom(double zoom_rel);
    void build_rect_popover(Gtk::ToggleButton* btn);
    void build_ellipse_popover(Gtk::ToggleButton* btn);
    void build_line_popover(Gtk::ToggleButton* btn);
    void build_ref_popover(Gtk::ToggleButton* btn);
    void build_measure_popover(Gtk::ToggleButton* btn);
    void build_text_popover(Gtk::ToggleButton* btn);
    void build_polygon_popover(Gtk::ToggleButton* btn);
    void build_spiral_popover(Gtk::ToggleButton* btn);
    // ── s153 sub-ship 3: paint editor ─────────────────────────────────────
    // The defaults well + fill/stroke popovers + swatch picker section +
    // shared colour picker. State previously on the public Toolbar; the
    // public API (default_fill, default_stroke, sync_from_object,
    // sync_from_selection, set_swatch_library) is now forwarded into
    // these members.
    //
    // Naming convention follows sub-ship 2: m_* → unprefixed inside Impl,
    // signals (m_sig_defaults, m_sig_gradient_edit) stay public for
    // accessor stability and Impl reaches them via self-> .

    // Defaults well
    Gtk::DrawingArea              well;

    // Fill popover
    Gtk::Popover                  fill_pop;
    Gtk::ToggleButton             fill_type_solid_btn;
    Gtk::ToggleButton             fill_type_none_btn;
    Gtk::ToggleButton             fill_type_cc_btn;
    Gtk::ToggleButton             fill_type_swatch_btn;     // S87 m1 v2
    Gtk::ToggleButton             fill_type_gradient_btn;   // S91
    CurvzEntry                    fill_hex_entry;
    Gtk::DrawingArea              fill_swatch;
    // S91 fill gradient row: [ramp ─────][Edit…]. Visible only when
    // def_fill.is_gradient(). Member-pointer Box so refresh_fill_popover
    // can drive visibility.
    Gtk::Box*                     fill_gradient_row  = nullptr;
    Gtk::DrawingArea              fill_gradient_ramp;
    Gtk::Button                   fill_gradient_edit_btn;

    // Stroke popover
    Gtk::Popover                  stroke_pop;
    Gtk::ToggleButton             stroke_type_solid_btn;
    Gtk::ToggleButton             stroke_type_none_btn;
    Gtk::ToggleButton             stroke_type_cc_btn;
    Gtk::ToggleButton             stroke_type_swatch_btn;   // S87 m1 v2
    CurvzEntry                    stroke_hex_entry;
    Gtk::DrawingArea              stroke_swatch;
    // S91: stroke does NOT get a gradient toggle (rare in SVG, not
    // worth a separate stroke-side gradient surface). Fill-only.
    Gtk::SpinButton               width_spin;
    Glib::RefPtr<Gtk::Adjustment> width_adj;
    Gtk::Label                    width_unit_lbl;
    Gtk::Label                    width_label;  // "Width (in):" — unit-aware

    // Cap/Join buttons — icon buttons, active state via CSS
    Gtk::Button*                  cap_butt_btn   = nullptr;
    Gtk::Button*                  cap_round_btn  = nullptr;
    Gtk::Button*                  cap_square_btn = nullptr;
    Gtk::Button*                  join_miter_btn = nullptr;
    Gtk::Button*                  join_round_btn = nullptr;
    Gtk::Button*                  join_bevel_btn = nullptr;

    // Defaults state — what the next placed object inherits. m_def_*
    // ↔ def_* under the s153 sub-ship 3 rename.
    FillStyle                     def_fill;
    StrokeStyle                   def_stroke;
    // S58n: true when the current multi-selection has heterogeneous
    // fill / stroke paint. Drives diagonal-stripe rendering and
    // deactivates type toggles in the popovers.
    bool                          fill_mixed   = false;
    bool                          stroke_mixed = false;
    bool                          syncing      = false;

    // ── S87 m1: swatch picker section (copy-only, fill + stroke) ──────
    // Chip-grid picker tucked inside each existing popover. Visible
    // only when swatch_library != nullptr. Click on a chip = "write
    // this swatch's RGB into def_* as Solid" — no binding state.
    // swatch_library is non-owning; MainWindow plumbs it post-load.
    const color::SwatchLibrary*   swatch_library = nullptr;

    // Per-popover picker widgets (managed by host Box; we keep refs
    // for refresh_*_popover and the rebuild path).
    Gtk::Box*                     fill_picker_section   = nullptr;
    Gtk::DropDown*                fill_palette_dd       = nullptr;
    Gtk::FlowBox*                 fill_chip_flow        = nullptr;
    Gtk::Box*                     stroke_picker_section = nullptr;
    Gtk::DropDown*                stroke_palette_dd     = nullptr;
    Gtk::FlowBox*                 stroke_chip_flow      = nullptr;

    // S87 m1 v2: pointers to the colour rows so refresh_*_popover can
    // drive visibility based on active type.
    Gtk::Box*                     fill_color_row        = nullptr;
    Gtk::Box*                     stroke_color_row      = nullptr;

    // S87 m1 v2: per-popover "Swatch tab is showing" flag. Distinct
    // from def_*.type — the type stays Solid/None/CC, but the user
    // chose to see the picker. Reset on Solid/None/currentColor click.
    bool                          fill_picker_open   = false;
    bool                          stroke_picker_open = false;

    // Shadow vectors — dropdown row order so selection-changed maps
    // index → palette id. Includes "__all__" sentinel at row 0.
    std::vector<std::string>      fill_palette_ids;
    std::vector<std::string>      stroke_palette_ids;

    // Connections for dropdown property_selected. Disconnected before
    // programmatic re-selection inside rebuild_palette_dropdown to
    // avoid firing user-flow handlers (PaintEditor idiom).
    sigc::connection              fill_palette_dd_conn;
    sigc::connection              stroke_palette_dd_conn;

    // S87 m1 fix2: tracks the palette id the chip grid currently
    // shows. Guards rebuild against dropdown spurious-fire (handoff
    // gotcha). Skip rebuild_chip_grid when requested id matches
    // what's already drawn.
    std::string                   fill_chips_palette_id;
    std::string                   stroke_chips_palette_id;

    // Shared colour-picker popover for fill + stroke wells. Attached
    // to *self in the ctor (deferred to signal_realize).
    ColorPickerPopover            color_popover;

    // ── Paint editor methods ─────────────────────────────────────────
    void build_defaults_well();
    void build_fill_popover();
    void build_stroke_popover();
    void redraw_well();
    void update_cap_buttons();
    void update_join_buttons();
    void apply_hex_to_fill(const std::string& hex);
    void apply_hex_to_stroke(const std::string& hex);
    void refresh_fill_popover();
    void refresh_stroke_popover();
    void reset_to_defaults();
    void emit_defaults();
    void sync_from_object(const FillStyle& fill, const StrokeStyle& stroke);
    void sync_from_selection(const std::vector<SceneNode*>& sel,
                             SceneNode* primary);

    // S87 m1 helpers — per-slot. is_stroke selects which set of
    // members to operate on.
    void build_swatch_picker_section(Gtk::Box& outer, bool is_stroke);
    void rebuild_swatch_pickers();
    void rebuild_palette_dropdown(bool is_stroke);
    void rebuild_chip_grid(bool is_stroke);
    void apply_swatch_pick_to_fill(const std::string& swatch_id);
    void apply_swatch_pick_to_stroke(const std::string& swatch_id);
    void set_swatch_library(const color::SwatchLibrary* lib);

    // ── s153 sub-ship 4: align + snap + remaining state ───────────────────
    // Naming follows the established convention: paint-editor-side fields
    // are unprefixed inside Impl (def_fill, fill_pop, ...), so align/snap
    // members do the same. Signals lose the m_ prefix too.

    // Current popup display unit (read by tool popovers for label
    // rendering; written via set_popup_unit forwarder).
    Unit popup_unit = Unit::Px;

    // Live document pointer for popover defaults (read by some tool
    // popovers' signal_show handlers; written via set_document
    // forwarder). Non-owning.
    CurvzDocument* doc = nullptr;

    // s117 m5: motif state for Cairo-painted align icons. set_motif
    // (forwarded from MainWindow) writes here and queues redraws.
    Motif motif = Motif::Dark;

    // Align & Distribute
    Gtk::Button       align_btn;
    Gtk::Popover      align_pop;
    Gtk::DrawingArea  align_da[8];
    bool              align_enabled = false;

    void build_align_button();
    void build_align_popover();
    void set_align_enabled(bool enabled);

    // ── Transforms section (s154 m1) ─────────────────────────────────────
    // Cluster of four transform-verb buttons inserted between the top
    // creation tools (Pen..TextOnPath) and the lower tools (Eyedropper
    // onward). Order top-to-bottom: SnR, Blend, Bool, Warp.
    //
    // Faked-disabled pattern (mirrors align): each *_enabled bool gates
    // left-click; the .tool-btn-disabled CSS class drives the visuals;
    // widgets stay sensitive so right-click events keep flowing to the
    // configuration popovers even when the operation itself is gated
    // off by selection state.
    Gtk::Button   snr_btn;
    Gtk::Button   blend_btn;
    Gtk::Button   bool_btn;
    Gtk::Button   warp_btn;
    Gtk::Popover  bool_pop;     // left-click popover on bool_btn
    // s154 m2: snr_cfg_pop retired — SnR right-click now emits
    // sig_step_repeat_configure for MainWindow's StepRepeatPopover.
    // s154 m3: blend_cfg_pop retired — Blend right-click now emits
    // sig_blend_configure for MainWindow's BlendPopover.
    // s154 m4a: warp_cfg_pop retired — Warp right-click now emits
    // sig_warp_configure for MainWindow's WarpPopover. The
    // build_placeholder_cfg_popover helper is gone with its last user.
    bool          snr_enabled   = false;
    bool          blend_enabled = false;
    bool          bool_enabled  = false;
    bool          warp_enabled  = false;
    // s154 m3 follow-up: when true, the next bool-op picker click only
    // updates the toolbar icon — does NOT emit sig_bool_op. Set by the
    // bool_btn right-click gesture (which lets the user pre-select an
    // op before any selection exists, bypassing the bool_enabled gate).
    // Reset to false before every left-click pop so subsequent normal
    // left-click picks fire the op as usual.
    bool          bool_picker_pre_set = false;

    void build_transforms_section();
    void build_bool_popover();
    void set_transforms_enabled(bool snr, bool blend,
                                bool boolop, bool warp);

    // Snap settings popover (separate from the snap-toggle button —
    // that button lives in Impl already since sub-ship 1).
    Gtk::Popover       snap_pop;
    Gtk::CheckButton*  snap_cb_guides  = nullptr;
    Gtk::CheckButton*  snap_cb_grid    = nullptr;
    Gtk::CheckButton*  snap_cb_margins = nullptr;
    Gtk::CheckButton*  snap_cb_nodes   = nullptr;
    Gtk::CheckButton*  snap_cb_edges   = nullptr;
    Gtk::CheckButton*  snap_cb_centers = nullptr;
    bool               snap_loading    = false;

    void build_snap_popover(Gtk::Widget* widget);
    void set_snap_settings(const SnapSettings& s);

    // ── Signals (all moved from public Toolbar in sub-ship 4) ────────────
    // Public Toolbar's signal_*() accessors are now forwarders that
    // return references to these. End-state: header carries no
    // signal storage, only declarations.
    ToolChangedSignal      signal_tool_changed_;
    DensitySignal          sig_density_changed;
    DefaultsSignal         sig_defaults;
    GradientEditSignal     sig_gradient_edit;
    FitSignal              sig_fit;
    ZoomSignal             sig_zoom_step;
    ZoomToSignal           sig_zoom_to;
    PlaceRefSignal         sig_place_ref;
    PlaceRectSignal        sig_place_rect;
    PlaceEllipseSignal     sig_place_ellipse;
    PlaceLineSignal        sig_place_line;
    PlaceTextSignal        sig_place_text;
    PlacePolygonSignal     sig_place_polygon;
    PlaceSpiralSignal      sig_place_spiral;
    CanvasFocusSignal      sig_canvas_focus;
    SnapToggleSignal       sig_snap;
    SnapSettingsSignal     sig_snap_settings;
    SnapPopOpenSignal      sig_snap_pop_open;
    MeasureSettingsSignal  sig_measure_settings;
    AlignSignal            sig_align;
    MacroSignal            sig_macro;
    MacroSignal            sig_macro_run;
    // s154 m1 — transforms section
    StepRepeatSignal       sig_step_repeat;
    BlendSignal            sig_blend;
    WarpSignal             sig_warp;
    BoolOpSignal           sig_bool_op;
    // s154 m2 — SnR right-click → MainWindow opens StepRepeatPopover
    StepRepeatConfigureSignal sig_step_repeat_configure;
    // s154 m3 — Blend right-click → MainWindow opens BlendPopover
    BlendConfigureSignal      sig_blend_configure;
    // s154 m4a — Warp right-click → MainWindow opens WarpPopover
    WarpConfigureSignal       sig_warp_configure;
};

// ── Density class-name helper (used by Impl::set_density) ────────────────
namespace {
const char* density_class_name(Toolbar::Density d) {
    switch (d) {
        case Toolbar::Density::Comfortable: return "";          // base rules
        case Toolbar::Density::Standard:    return "standard";
        case Toolbar::Density::Compact:     return "compact";
        case Toolbar::Density::Tight:       return "tight";
    }
    return "";
}

// ── Right-click claim helper ─────────────────────────────────────────────
//
// The toolbar root carries a button-3 GestureClick that opens the
// density picker on right-click. Several leaf widgets in the toolbar
// (align, defaults well, snap toggle, macro button, the tool buttons
// without per-tool right-click affordances) should NOT trigger density
// when right-clicked — even if the button itself has no meaningful
// right-click action of its own. Right-click on a real interactive
// widget should be a no-op, not "open density."
//
// GTK4 gesture controllers in BUBBLE phase don't auto-claim on press.
// Instead, we attach a CLAIMED-on-press button-3 GestureClick to each
// leaf widget that should swallow right-clicks. The gesture system
// then stops propagating the right-click sequence to the parent
// controller on the toolbar root, leaving density unbothered.
//
// add_rclick_swallow(widget) — drop-in for any widget where a
// background-click semantics would be wrong. The gesture is owned
// by the widget and lives as long as the widget does.
void add_rclick_swallow(Gtk::Widget& w) {
    auto g = Gtk::GestureClick::create();
    g->set_button(3);
    // Capture so we claim before any inner controller runs.
    g->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    auto* g_raw = g.get();
    g->signal_pressed().connect(
        [g_raw](int /*n_press*/, double /*x*/, double /*y*/) {
            g_raw->set_state(Gtk::EventSequenceState::CLAIMED);
        });
    w.add_controller(g);
}
}  // namespace

// ── Public Toolbar — constructor & destructor ────────────────────────────
Toolbar::Toolbar()
    : Gtk::Box(Gtk::Orientation::VERTICAL),
      m_impl(std::make_unique<Impl>(this))
{
    curvz::utils::set_name(*this, "tb", "main_toolbar_root");
    // s152: margins/spacing are governed by CSS now. The root
    // .toolbar-panel rule sets padding; per-density rules override it.
    // We don't set_size_request the column either — CSS min-width owns
    // the column width, and set_density() flips a class to change it
    // without rebuilding the toolbar.
    add_css_class("toolbar-panel");

    // Shared colour-picker popover for fill + stroke wells.
    //
    // IMPORTANT: we attach to the root Gtk::Window, not to `*this`.
    // The Toolbar is a 48px-wide vertical strip on the left edge; GTK
    // constrains popover geometry to its stable-parent's bounds, so
    // parenting to `*this` produced a popover that got squashed or
    // pushed vertically. Attaching to the window gives the popover the
    // whole window as geometry space, while the well remains the visual
    // anchor (via the anchor rect computed in open()).
    //
    // Deferred to signal_realize because get_root() isn't valid during
    // construction.
    signal_realize().connect([this]() {
        if (auto* w = dynamic_cast<Gtk::Window*>(get_root())) {
            m_impl->color_popover.attach(*w);
        }
    });

    m_impl->build();
}

Toolbar::~Toolbar() = default;

// ── Public Toolbar — thin forwarders to Impl ─────────────────────────────
ActiveTool Toolbar::active_tool() const { return m_impl->active; }

void Toolbar::select_tool(ActiveTool tool) { m_impl->select_tool(tool); }

void Toolbar::cycle_tool(int dir) { m_impl->cycle_tool(dir); }

void Toolbar::set_active_tool_icon(ActiveTool tool, const char* icon_name) {
    m_impl->set_active_tool_icon(tool, icon_name);
}

Toolbar::Density Toolbar::density() const { return m_impl->density; }

void Toolbar::set_density(Density d) { m_impl->set_density(d); }

void Toolbar::set_snap_enabled(bool enabled) {
    // Host-driven sync. set_active() fires signal_toggled, which already
    // swaps the icon and emits m_sig_snap — so a no-op guard keeps us
    // from echoing back to the host. Setting the icon defensively here
    // covers the edge case where the toggle's active state was already
    // correct but the icon wasn't.
    if (m_impl->snap_btn.get_active() != enabled)
        m_impl->snap_btn.set_active(enabled);
    m_impl->snap_btn.set_icon_name(enabled ? "curvz-switch-on-symbolic"
                                           : "curvz-switch-off-symbolic");
}

Gtk::ToggleButton* Toolbar::get_corner_btn() { return m_impl->corner_tool_btn; }

// ── s153 sub-ship 3: paint editor forwarders ────────────────────────────
// State (def_fill, def_stroke, swatch_library) and method bodies live
// in Impl. Public surface stays as documented in Toolbar.hpp.
const FillStyle&   Toolbar::default_fill()   const { return m_impl->def_fill; }
const StrokeStyle& Toolbar::default_stroke() const { return m_impl->def_stroke; }

void Toolbar::sync_from_object(const FillStyle& fill, const StrokeStyle& stroke) {
    m_impl->sync_from_object(fill, stroke);
}

void Toolbar::sync_from_selection(const std::vector<SceneNode*>& sel,
                                   SceneNode* primary) {
    m_impl->sync_from_selection(sel, primary);
}

void Toolbar::set_swatch_library(const color::SwatchLibrary* lib) {
    m_impl->set_swatch_library(lib);
}

// ── s153 sub-ship 2e: polygon / spiral state accessors ──────────────────
// Canvas reads these during drag to honour the popover's current
// settings. Storage moved into Impl alongside the popovers.
int    Toolbar::polygon_sides()      const { return m_impl->poly_sides; }
double Toolbar::polygon_inflection() const { return m_impl->poly_inflection; }
double Toolbar::spiral_turns()       const { return m_impl->spiral_turns; }
double Toolbar::spiral_inner_pct()   const { return m_impl->spiral_inner; }

// ── s153 sub-ship 4: signal accessor forwarders ─────────────────────────
// All 22 signals now live in Impl. Accessors return references to those
// members; same call signature as before, only the storage location
// changed. Header dependents pay only for declarations.
Toolbar::ToolChangedSignal&
  Toolbar::signal_tool_changed()              { return m_impl->signal_tool_changed_; }
Toolbar::DensitySignal&
  Toolbar::signal_density_changed()           { return m_impl->sig_density_changed; }
Toolbar::DefaultsSignal&
  Toolbar::signal_defaults_changed()          { return m_impl->sig_defaults; }
Toolbar::GradientEditSignal&
  Toolbar::signal_gradient_edit_requested()   { return m_impl->sig_gradient_edit; }
Toolbar::SnapToggleSignal&
  Toolbar::signal_snap_toggled()              { return m_impl->sig_snap; }
Toolbar::SnapSettingsSignal&
  Toolbar::signal_snap_settings_changed()     { return m_impl->sig_snap_settings; }
Toolbar::SnapPopOpenSignal&
  Toolbar::signal_snap_pop_open()             { return m_impl->sig_snap_pop_open; }
Toolbar::MeasureSettingsSignal&
  Toolbar::signal_measure_settings_changed()  { return m_impl->sig_measure_settings; }
Toolbar::AlignSignal&
  Toolbar::signal_align_requested()           { return m_impl->sig_align; }
Toolbar::MacroSignal&
  Toolbar::signal_macro_manager()             { return m_impl->sig_macro; }
Toolbar::MacroSignal&
  Toolbar::signal_macro_run()                 { return m_impl->sig_macro_run; }
// s154 m1 — transforms section
Toolbar::StepRepeatSignal&
  Toolbar::signal_step_repeat()               { return m_impl->sig_step_repeat; }
Toolbar::BlendSignal&
  Toolbar::signal_blend()                     { return m_impl->sig_blend; }
Toolbar::WarpSignal&
  Toolbar::signal_warp()                      { return m_impl->sig_warp; }
Toolbar::BoolOpSignal&
  Toolbar::signal_bool_op()                   { return m_impl->sig_bool_op; }
// s154 m2 — SnR right-click configure
Toolbar::StepRepeatConfigureSignal&
  Toolbar::signal_step_repeat_configure()     { return m_impl->sig_step_repeat_configure; }
Gtk::Widget& Toolbar::step_repeat_button()    { return m_impl->snr_btn; }
// s154 m3 — Blend right-click configure
Toolbar::BlendConfigureSignal&
  Toolbar::signal_blend_configure()           { return m_impl->sig_blend_configure; }
Gtk::Widget& Toolbar::blend_button()          { return m_impl->blend_btn; }
// s154 m4a — Warp right-click configure
Toolbar::WarpConfigureSignal&
  Toolbar::signal_warp_configure()            { return m_impl->sig_warp_configure; }
Gtk::Widget& Toolbar::warp_button()           { return m_impl->warp_btn; }
Toolbar::FitSignal&
  Toolbar::signal_fit_requested()             { return m_impl->sig_fit; }
Toolbar::ZoomSignal&
  Toolbar::signal_zoom_step()                 { return m_impl->sig_zoom_step; }
Toolbar::ZoomToSignal&
  Toolbar::signal_zoom_to()                   { return m_impl->sig_zoom_to; }
Toolbar::PlaceRefSignal&
  Toolbar::signal_place_ref()                 { return m_impl->sig_place_ref; }
Toolbar::PlaceRectSignal&
  Toolbar::signal_place_rect()                { return m_impl->sig_place_rect; }
Toolbar::PlaceEllipseSignal&
  Toolbar::signal_place_ellipse()             { return m_impl->sig_place_ellipse; }
Toolbar::PlaceLineSignal&
  Toolbar::signal_place_line()                { return m_impl->sig_place_line; }
Toolbar::PlaceTextSignal&
  Toolbar::signal_place_text()                { return m_impl->sig_place_text; }
Toolbar::PlacePolygonSignal&
  Toolbar::signal_place_polygon()             { return m_impl->sig_place_polygon; }
Toolbar::PlaceSpiralSignal&
  Toolbar::signal_place_spiral()              { return m_impl->sig_place_spiral; }
Toolbar::CanvasFocusSignal&
  Toolbar::signal_request_canvas_focus()      { return m_impl->sig_canvas_focus; }

// ── s153 sub-ship 4: state-setter forwarders ─────────────────────────────
void Toolbar::set_motif(Motif m) {
    if (m_impl->motif == m) return;
    m_impl->motif = m;
    m_impl->align_btn.queue_draw();
    for (auto& da : m_impl->align_da) da.queue_draw();
}

void Toolbar::set_align_enabled(bool enabled) {
    m_impl->set_align_enabled(enabled);
}

// s154 m1 — transforms section enable forwarder. Mirrors set_align_enabled's
// faked-disabled pattern: the widgets stay sensitive (so right-click to
// configure stays reachable even when the action itself is gated off),
// flags drive the .tool-btn-disabled visual class and gate left-click.
void Toolbar::set_transforms_enabled(bool snr, bool blend,
                                     bool boolop, bool warp) {
    m_impl->set_transforms_enabled(snr, blend, boolop, warp);
}

void Toolbar::set_snap_settings(const SnapSettings& s) {
    m_impl->set_snap_settings(s);
}

// =============================================================================
// Impl method definitions
// =============================================================================

// ── Impl::build — toolbar structure (the old ctor body) ──────────────────
//
// Preserves the exact widget order, naming, and behaviour of the prior
// imperative ctor — this is a structural move, not a redesign.
//
// As of s153 sub-ship 4 the entire toolbar lives in Impl: tool buttons,
// 9 placement popovers, paint editor, defaults well, swatch picker,
// align section, snap settings popover. The public Toolbar is a thin
// wrapper — its only remaining job is to be a Gtk::Box host and to
// expose the public method/signal-accessor surface as forwarders.
// `self->` is reserved for genuinely Gtk::Widget-side calls
// (`self->append(...)`, `self->get_root()`, `self->get_width()`).
void Toolbar::Impl::build() {
    add_tool_button("curvz-select-symbolic", "Select (S)", ActiveTool::Selection);
    curvz::utils::set_name(buttons.back(), "tb_sel", "main_toolbar_select_tool_btn");
    add_tool_button("curvz-node-symbolic", "Nodes (N)", ActiveTool::Node);
    curvz::utils::set_name(buttons.back(), "tb_nod", "main_toolbar_node_tool_btn");
    // Density picker only opens on Selection or empty space — see comment
    // above the right-click controller at the end of build(). Swallow
    // right-click here so density doesn't leak through.
    add_rclick_swallow(*buttons.back());

    {
        auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep->set_margin_top(4);
        sep->set_margin_bottom(4);
        self->append(*sep);
    }

    add_tool_button("curvz-pen-symbolic", "Pen (P)", ActiveTool::Pen);
    curvz::utils::set_name(buttons.back(), "tb_pen", "main_toolbar_pen_tool_btn");
    add_rclick_swallow(*buttons.back());
    add_tool_button("curvz-rect-symbolic",
                    "Rectangle (R)  —  Right-click to place precisely",
                    ActiveTool::Rect);
    Gtk::ToggleButton* rect_tool_btn = buttons.back();
    curvz::utils::set_name(rect_tool_btn, "tb_rec", "main_toolbar_rect_tool_btn");
    add_tool_button("curvz-oval-symbolic",
                    "Ellipse (E)  —  Right-click to place precisely",
                    ActiveTool::Ellipse);
    Gtk::ToggleButton* ellipse_tool_btn = buttons.back();
    curvz::utils::set_name(ellipse_tool_btn, "tb_ova", "main_toolbar_ellipse_tool_btn");
    add_tool_button("curvz-line-symbolic",
                    "Line (L)  —  Right-click to place precisely",
                    ActiveTool::Line);
    Gtk::ToggleButton* line_tool_btn = buttons.back();
    curvz::utils::set_name(line_tool_btn, "tb_lin", "main_toolbar_line_tool_btn");
    add_tool_button("curvz-ref-symbolic",
                    "Reference Point (F)  —  Right-click to place precisely",
                    ActiveTool::Ref);
    Gtk::ToggleButton* ref_tool_btn = buttons.back();
    curvz::utils::set_name(ref_tool_btn, "tb_ref", "main_toolbar_ref_tool_btn");
    add_tool_button("curvz-text-symbolic", "Text (T)  —  Right-click for options",
                    ActiveTool::Text);
    Gtk::ToggleButton* text_tool_btn = buttons.back();
    curvz::utils::set_name(text_tool_btn, "tb_txt", "main_toolbar_text_tool_btn");
    add_tool_button("curvz-text-on-path-symbolic",
                    "Text on Path (U)  —  Click text then path to link",
                    ActiveTool::TextOnPath);
    curvz::utils::set_name(buttons.back(), "tb_top", "main_toolbar_text_on_path_tool_btn");
    add_rclick_swallow(*buttons.back());
    // s175 m4: Polygon and Spiral moved here from below — they're
    // shape-creation tools, they belong in the Creation section. Both
    // pop a configuration popover on right-click; popover wiring happens
    // alongside the rest of the placement popovers further down.
    add_tool_button("curvz-polygon-symbolic",
                    "Polygon / Star (G)  —  Right-click to configure",
                    ActiveTool::Polygon);
    Gtk::ToggleButton* polygon_tool_btn = buttons.back();
    curvz::utils::set_name(polygon_tool_btn, "tb_pol", "main_toolbar_polygon_tool_btn");
    add_tool_button("curvz-spiral-symbolic",
                    "Spiral (W)  —  Right-click to configure", ActiveTool::Spiral);
    Gtk::ToggleButton* spiral_tool_btn = buttons.back();
    curvz::utils::set_name(spiral_tool_btn, "tb_spi", "main_toolbar_spiral_tool_btn");

    {
        auto* sep2 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep2->set_margin_top(4);
        sep2->set_margin_bottom(4);
        self->append(*sep2);
    }

    // s154 m1 / s175 m4: transforms section — Align, Blend, Bool,
    // Corner, SnR, Warp (alphabetical). Emits its own closing separator,
    // so the utility cluster (Eyedropper/Measure/Zoom) begins cleanly
    // under it. Original s154 m1 build had only SnR, Blend, Bool, Warp;
    // s175 m4 folded in Corner and Align and sorted alphabetically.
    build_transforms_section();

    // s175 m4: Utility section — Eyedropper, Measure, Zoom. Selection-
    // and navigation-only tools; no geometry effect. Polygon, Spiral,
    // and Corner used to live here mixed in; they moved to Creation
    // and Transforms respectively, leaving the section vocabulary clean.
    add_tool_button("curvz-eyedropper-symbolic", "Eyedropper (I)",
                    ActiveTool::Eyedropper);
    curvz::utils::set_name(buttons.back(), "tb_eyd", "main_toolbar_eyedropper_tool_btn");
    add_rclick_swallow(*buttons.back());
    add_tool_button("curvz-ruler-symbolic",
                    "Measure (M)  —  Measure between two nodes; "
                    "right-click to configure",
                    ActiveTool::Measure);
    Gtk::ToggleButton* measure_tool_btn = buttons.back();
    curvz::utils::set_name(measure_tool_btn, "tb_meas", "main_toolbar_measure_tool_btn");
    add_tool_button("curvz-zoom-symbolic", "Zoom (Z)  —  Right-click to set level",
                    ActiveTool::Zoom);
    Gtk::ToggleButton* zoom_tool_btn = buttons.back();
    curvz::utils::set_name(zoom_tool_btn, "tb_zom", "main_toolbar_zoom_tool_btn");

    // Spacer — everything below floats to the bottom
    {
        auto* spacer = Gtk::make_managed<Gtk::Box>();
        spacer->set_vexpand(true);
        self->append(*spacer);
    }

    // ── Bottom section: well + supporting widgets ─────────────────────────
    {
        auto* sep3 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep3->set_margin_top(4);
        sep3->set_margin_bottom(4);
        self->append(*sep3);
    }

    // Placement popovers — all 9 internalised into Impl across sub-ship 2
    // (s152 m2a → s153 m2e). Calls are unqualified now; the bodies live
    // as Toolbar::Impl::build_<name>_popover further down this file.
    build_zoom_popover(zoom_tool_btn);              // sub-ship 2a — done
    build_ref_popover(ref_tool_btn);                // sub-ship 2c — done
    build_rect_popover(rect_tool_btn);              // sub-ship 2b — done
    build_ellipse_popover(ellipse_tool_btn);        // sub-ship 2b — done
    build_line_popover(line_tool_btn);              // sub-ship 2b — done
    build_text_popover(text_tool_btn);              // sub-ship 2d — done
    build_polygon_popover(polygon_tool_btn);        // sub-ship 2e — done
    build_spiral_popover(spiral_tool_btn);          // sub-ship 2e — done
    build_measure_popover(measure_tool_btn);        // sub-ship 2c — done

    if (!buttons.empty())
        buttons[0]->set_active(true);

    // ── Snap toggle button ────────────────────────────────────────────────
    {
        curvz::utils::set_name(snap_btn, "tb_ss", "main_toolbar_snap_btn");
        snap_btn.set_icon_name("curvz-switch-on-symbolic");
        snap_btn.set_tooltip_text(
            "Toggle Snap (Q)  —  right-click for snap targets");
        snap_btn.set_has_frame(false);
        snap_btn.add_css_class("tool-btn");
        // .tb-snap-btn opts out of .tool-btn:checked active blue (the
        // icon swap conveys state, no bg colour change wanted).
        snap_btn.add_css_class("tb-snap-btn");
        snap_btn.set_active(true);     // snap on by default
        snap_btn.set_hexpand(false);
        snap_btn.set_vexpand(false);
        snap_btn.signal_toggled().connect([this]() {
            const bool on = snap_btn.get_active();
            snap_btn.set_icon_name(on ? "curvz-switch-on-symbolic"
                                      : "curvz-switch-off-symbolic");
            sig_snap.emit(on);
        });
        self->append(snap_btn);
        // s153 sub-ship 4: build_snap_popover is now an Impl method —
        // call directly. The snap settings popover state (6 checkboxes,
        // m_snap_pop, m_snap_loading) all live in Impl too.
        build_snap_popover(&snap_btn);
    }

    // s175 m4: build_align_button() call removed. Align & Distribute
    // moved up into build_transforms_section() (alphabetical position 1)
    // since align is conceptually a transform, not a bottom-of-toolbar
    // utility. The widget construction itself still lives in
    // build_align_button(); transforms section calls it.

    // ── Macro manager button ──────────────────────────────────────────────
    {
        auto* macro_sep = Gtk::make_managed<Gtk::Separator>(
            Gtk::Orientation::HORIZONTAL);
        macro_sep->set_margin_top(4);
        macro_sep->set_margin_bottom(4);
        self->append(*macro_sep);

        auto* macro_btn = Gtk::make_managed<Gtk::Button>();
        curvz::utils::set_name(macro_btn, "tb_mb", "main_toolbar_macro_btn");
        macro_btn->set_icon_name("media-record-symbolic");
        macro_btn->set_tooltip_text(
            "Run current macro  (Ctrl+M)\n"
            "Right-click: Macro Manager  (Ctrl+Shift+M)");
        macro_btn->set_has_frame(false);
        macro_btn->set_halign(Gtk::Align::CENTER);
        macro_btn->add_css_class("tool-btn");
        macro_btn->signal_clicked().connect(
            [this]() { sig_macro_run.emit(); });

        auto macro_rclick = Gtk::GestureClick::create();
        macro_rclick->set_button(3);
        // s152 sub-ship 1 fix: claim on press so the right-click doesn't
        // bubble up to the toolbar root's density picker. Without the
        // claim, both Macro Manager AND density would open on a single
        // right-click.
        auto* macro_rclick_raw = macro_rclick.get();
        macro_rclick->signal_pressed().connect(
            [this, macro_rclick_raw](int, double, double) {
                macro_rclick_raw->set_state(Gtk::EventSequenceState::CLAIMED);
                sig_macro.emit();
            });
        macro_btn->add_controller(macro_rclick);

        self->append(*macro_btn);
    }

    {
        auto* sep4 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep4->set_margin_top(4);
        sep4->set_margin_bottom(4);
        self->append(*sep4);
    }

    build_defaults_well();   // fill/stroke well — sub-ship 3 — done

    // ── Right-click on toolbar background or Selection → density picker ──
    //
    // The rule (s152 sub-ship 2 design call): density picker opens on
    //   - empty toolbar surface (separator gaps, spacer)
    //   - the Selection tool button (the "default tool" — same status
    //     as background for right-click purposes)
    //
    // Every other toolbar widget either claims the right-click for its
    // own affordance (macro → manager; rect/ellipse/line/ref/text/
    // polygon/spiral/zoom/measure → placement popover; snap → settings)
    // or installs add_rclick_swallow to absorb the press silently
    // (align, well, plus the affordance-less tool buttons Node, Pen,
    // Eyedropper, Corner, TextOnPath).
    //
    // Why Selection: it's the toolbar's home tool. Right-click on it
    // carries the same "configure the toolbar" semantics as background.
    // Other tools may grow per-tool right-click affordances (ARC #6+),
    // and locking density to background+Selection now means new
    // affordances can land without displacing density.
    {
        auto density_rclick = Gtk::GestureClick::create();
        density_rclick->set_button(3);
        density_rclick->signal_pressed().connect(
            [this](int /*n_press*/, double x, double y) {
                if (!density_pop_built) build_density_popover();
                Gdk::Rectangle rect(static_cast<int>(x), static_cast<int>(y),
                                    1, 1);
                density_pop.set_pointing_to(rect);
                density_pop.popup();
            });
        self->add_controller(density_rclick);
    }

    LOG_DEBUG("Toolbar created with {} tools", buttons.size());
    select_tool(ActiveTool::Selection);  // apply :checked CSS on startup
}

// ── Impl::add_tool_button (icon-name overload) ───────────────────────────
void Toolbar::Impl::add_tool_button(const char* icon, const char* tooltip,
                                     ActiveTool tool) {
    auto* btn = Gtk::make_managed<Gtk::ToggleButton>();
    btn->set_icon_name(icon);
    btn->set_tooltip_text(tooltip);
    btn->set_has_frame(false);
    btn->add_css_class("tool-btn");
    btn->set_halign(Gtk::Align::FILL);
    btn->set_hexpand(false);
    // s152: size is governed by CSS .tool-btn min-width/min-height,
    // which scales with the density class on the toolbar root.
    btn->signal_clicked().connect([this, btn, tool]() {
        if (btn->get_active()) {
            select_tool(tool);
        } else if (active == tool) {
            btn->set_active(true);
            select_tool(tool);   // re-apply tool-active class
        }
    });
    self->append(*btn);
    buttons.push_back(btn);
    button_tools.push_back(tool);
}

// ── Impl::add_tool_button (Picture overload) ─────────────────────────────
void Toolbar::Impl::add_tool_button(Gtk::Picture* pic, const char* tooltip,
                                     ActiveTool tool) {
    auto* btn = Gtk::make_managed<Gtk::ToggleButton>();
    btn->set_child(*pic);
    btn->set_tooltip_text(tooltip);
    btn->set_has_frame(true);
    btn->add_css_class("tool-btn");
    btn->set_halign(Gtk::Align::FILL);
    btn->set_hexpand(false);
    btn->signal_clicked().connect([this, btn, tool]() {
        if (btn->get_active())
            select_tool(tool);
        else if (active == tool)
            btn->set_active(true);
    });
    self->append(*btn);
    buttons.push_back(btn);
    button_tools.push_back(tool);
}

// ── Impl::make_tool_button (s175 m4) ─────────────────────────────────────
// Same body as the icon-name add_tool_button overload above, but does NOT
// call self->append(). Caller appends explicitly. Returns the button so
// the caller can capture the pointer (for popover wiring or member
// storage) and place it where the section's append order requires.
//
// Used by build_transforms_section to fold Corner (a toggle tool button)
// into the alphabetical ordering of the section's six widgets, where the
// other five are plain Gtk::Button (Align, Blend, Bool, SnR, Warp) and
// can't be added via add_tool_button — the section function appends all
// six explicitly in alphabetical order.
Gtk::ToggleButton* Toolbar::Impl::make_tool_button(const char* icon,
                                                   const char* tooltip,
                                                   ActiveTool tool) {
    auto* btn = Gtk::make_managed<Gtk::ToggleButton>();
    btn->set_icon_name(icon);
    btn->set_tooltip_text(tooltip);
    btn->set_has_frame(false);
    btn->add_css_class("tool-btn");
    btn->set_halign(Gtk::Align::FILL);
    btn->set_hexpand(false);
    btn->signal_clicked().connect([this, btn, tool]() {
        if (btn->get_active()) {
            select_tool(tool);
        } else if (active == tool) {
            btn->set_active(true);
            select_tool(tool);
        }
    });
    buttons.push_back(btn);
    button_tools.push_back(tool);
    return btn;
}

// ── Impl::set_active_tool_icon ───────────────────────────────────────────
void Toolbar::Impl::set_active_tool_icon(ActiveTool tool, const char* icon_name) {
    for (size_t i = 0; i < button_tools.size(); ++i) {
        if (button_tools[i] == tool) {
            buttons[i]->set_icon_name(icon_name);
            return;
        }
    }
}

// ── Impl::select_tool ────────────────────────────────────────────────────
//
// All active-state styling lives in the stylesheet (.tool-btn:checked
// in css.hpp). Setting the toggle's checked state is the entire visual
// update — no per-call CssProvider allocation, no inline hex colours
// overriding Adwaita.
//
// The .tool-active class adds/removes are kept harmless: the legacy
// `togglebutton.tool-active` rule is now a no-op (see css.hpp). External
// code that still adds/removes the class keeps working.
void Toolbar::Impl::select_tool(ActiveTool tool) {
    active = tool;
    for (size_t i = 0; i < buttons.size(); ++i) {
        bool is_active = button_tools[i] == tool;
        buttons[i]->set_active(is_active);
        if (is_active)
            buttons[i]->add_css_class("tool-active");
        else
            buttons[i]->remove_css_class("tool-active");
    }
    signal_tool_changed_.emit(tool);
    LOG_DEBUG("Tool selected: {}", static_cast<int>(tool));
}

// ── Impl::cycle_tool ─────────────────────────────────────────────────────
void Toolbar::Impl::cycle_tool(int dir) {
    if (button_tools.empty()) return;
    int n = static_cast<int>(button_tools.size());
    int cur = 0;
    for (int i = 0; i < n; ++i) {
        if (button_tools[i] == active) { cur = i; break; }
    }
    int next = (cur + dir + n) % n;
    select_tool(button_tools[next]);
}

// ── Impl::set_density ────────────────────────────────────────────────────
//
// CSS class flip on the toolbar root. Comfortable is the bare class
// (no extra modifier); Standard, Compact, Tight each carry their own
// modifier class. set_density() removes any previously-applied modifier
// and adds the new one — GTK4 re-resolves the CSS rules and relayouts.
void Toolbar::Impl::set_density(Toolbar::Density d) {
    if (density == d && self->has_css_class(density_class_name(d))) {
        return;   // no-op (already applied)
    }
    for (const char* cls : {"standard", "compact", "tight"}) {
        if (self->has_css_class(cls)) self->remove_css_class(cls);
    }
    const char* cls = density_class_name(d);
    if (cls && *cls) self->add_css_class(cls);
    density = d;
    LOG_DEBUG("Toolbar density set to {}", static_cast<int>(d));
}

// ── Impl::build_density_popover ──────────────────────────────────────────
//
// Built lazily on first right-click. Lives as an Impl member so it
// survives popdown — popovers parented to a widget are not auto-
// destroyed; they can be popup'd repeatedly without rebuild.
//
// "In your face" rule (ARC.md design rule 4): instead of burying density
// in an AppPreferences dialog, the right-click on the toolbar background
// reveals it where the user is when they want to change it.
// AppPreferences still persists the default — MainWindow applies it on
// startup, and the radio handler here writes the user's pick back via
// signal_density_changed.
//
// Programmatic set_density() (the startup path) does NOT emit
// signal_density_changed — only user-driven changes from this popover do.
void Toolbar::Impl::build_density_popover() {
    if (density_pop_built) return;
    curvz::utils::set_name(density_pop, "pop_tb_density",
                           "popover_toolbar_density_root");
    auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    outer->set_spacing(4);
    outer->set_margin_top(8);
    outer->set_margin_bottom(8);
    outer->set_margin_start(12);
    outer->set_margin_end(12);

    auto* lbl = Gtk::make_managed<Gtk::Label>("Toolbar density");
    lbl->set_halign(Gtk::Align::START);
    lbl->add_css_class("dim-label");
    lbl->set_margin_bottom(4);
    outer->append(*lbl);

    auto* group_master = Gtk::make_managed<Gtk::CheckButton>("Comfortable");
    auto* opt_std      = Gtk::make_managed<Gtk::CheckButton>("Standard");
    auto* opt_compact  = Gtk::make_managed<Gtk::CheckButton>("Compact");
    auto* opt_tight    = Gtk::make_managed<Gtk::CheckButton>("Tight");

    opt_std->set_group(*group_master);
    opt_compact->set_group(*group_master);
    opt_tight->set_group(*group_master);

    switch (density) {
        case Toolbar::Density::Comfortable: group_master->set_active(true); break;
        case Toolbar::Density::Standard:    opt_std->set_active(true);      break;
        case Toolbar::Density::Compact:     opt_compact->set_active(true);  break;
        case Toolbar::Density::Tight:       opt_tight->set_active(true);    break;
    }

    auto wire_radio = [this](Gtk::CheckButton* btn, Toolbar::Density d) {
        btn->signal_toggled().connect([this, btn, d]() {
            if (!btn->get_active()) return;
            set_density(d);
            sig_density_changed.emit(d);
        });
    };
    wire_radio(group_master, Toolbar::Density::Comfortable);
    wire_radio(opt_std,      Toolbar::Density::Standard);
    wire_radio(opt_compact,  Toolbar::Density::Compact);
    wire_radio(opt_tight,    Toolbar::Density::Tight);

    outer->append(*group_master);
    outer->append(*opt_std);
    outer->append(*opt_compact);
    outer->append(*opt_tight);

    density_pop.set_child(*outer);
    density_pop.set_parent(*self);
    density_pop_built = true;
}

// ── Defaults well — only widget in toolbar column
// ─────────────────────────────
void Toolbar::Impl::build_defaults_well() {
  // 30×26 drawing area, centred in the 40px toolbar column.
  // Fill square 14×14 at (0,0), stroke square 14×14 at (8,8).
  // Click top-left region → fill popover; click bottom-right → stroke popover.
  curvz::utils::set_name(well, "tb_well", "main_toolbar_defaults_well");
  well.set_size_request(30, 26);
  well.set_can_target(true);
  well.set_halign(Gtk::Align::CENTER);
  well.set_tooltip_text("Click fill or stroke square to edit");
  well.add_css_class("tb-well");
  redraw_well();

  auto well_click = Gtk::GestureClick::create();
  well_click->set_button(1);
  well_click->signal_pressed().connect(
      [this, &well_click = *well_click](int, double x, double y) {
        // Ctrl+click anywhere → reset to defaults
        Gdk::ModifierType mods = well_click.get_current_event_state();
        if ((mods & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{}) {
          reset_to_defaults();
          return;
        }
        // Top-left region → fill popover; bottom-right → stroke popover
        bool stroke_region = (x >= 16 || y >= 16);
        if (stroke_region)
          stroke_pop.popup();
        else
          fill_pop.popup();
      });
  well.add_controller(well_click);
  // s152 sub-ship 1 fix: swallow right-clicks so they don't bubble up
  // to the toolbar root's density picker. The well has no meaningful
  // right-click action of its own (left-click split is fill vs stroke
  // by region); right-click should just be a no-op here.
  add_rclick_swallow(well);

  build_fill_popover();
  build_stroke_popover();

  fill_pop.set_parent(well);
  stroke_pop.set_parent(well);

  // S87 m1 fix5: refresh on every popover open. Replaces the prior
  // child-realize-hooked refresh which fired re-entrantly when the
  // colour row went from hidden→visible during a type-button click.
  // signal_show fires once per popup cycle, before the popover is
  // interactive — refresh lands cleanly with no event in flight.
  // S87 m1 fix5: refresh on every popover open. Replaces the prior
  // child-realize-hooked refresh which fired re-entrantly when the
  // colour row went from hidden→visible during a type-button click.
  // signal_show fires once per popup cycle, before the popover is
  // interactive — refresh lands cleanly with no event in flight.
  fill_pop.signal_show().connect([this]() {
    refresh_fill_popover();
  });
  stroke_pop.signal_show().connect([this]() {
    refresh_stroke_popover();
    update_cap_buttons();
    update_join_buttons();
  });

  self->append(well);
}

// ── Fill popover
// ──────────────────────────────────────────────────────────────
void Toolbar::Impl::build_fill_popover() {
  curvz::utils::set_name(fill_pop, "pop_tb_fill", "popover_toolbar_fill_root");
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(8);
  outer->set_margin_top(10);
  outer->set_margin_bottom(10);
  outer->set_margin_start(10);
  outer->set_margin_end(10);

  // Title
  auto *title = Gtk::make_managed<Gtk::Label>("Fill");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // Type row: Solid / None / currentColor / Swatch / Gradient
  // (S87 m1 v2 added Swatch as 4th; S91 added Gradient as 5th)
  auto *type_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  type_row->set_spacing(4);

  curvz::utils::set_name(fill_type_solid_btn, "pop_tb_fill_sol", "popover_toolbar_fill_type_solid_toggle");
  fill_type_solid_btn.set_label("Solid");
  fill_type_solid_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(fill_type_none_btn, "pop_tb_fill_non", "popover_toolbar_fill_type_none_toggle");
  fill_type_none_btn.set_label("None");
  fill_type_none_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(fill_type_cc_btn, "pop_tb_fill_cc", "popover_toolbar_fill_type_currentcolor_toggle");
  fill_type_cc_btn.set_label("currentColor");
  fill_type_cc_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(fill_type_swatch_btn, "pop_tb_fill_sw", "popover_toolbar_fill_type_swatch_toggle");
  fill_type_swatch_btn.set_label("Swatch");
  fill_type_swatch_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(fill_type_gradient_btn, "pop_tb_fill_grd", "popover_toolbar_fill_type_gradient_toggle");
  fill_type_gradient_btn.set_label("Gradient");
  fill_type_gradient_btn.add_css_class("tb-type-btn");

  // S87 m1 fix6: ToggleButton + set_group, mirroring PaintEditor's idiom.
  // The previous Gtk::Button approach hit a GTK4 autohide trap: a click
  // on a plain Button inside an autohide popover bubbles through to the
  // popover's outside-click-dismiss handler and triggers popdown ~3-5 ms
  // later. ToggleButton consumes the click via its toggled-state change
  // in a way GTK doesn't interpret as outside-click. set_group ties the
  // five into a radio: only one is active at a time, GTK auto-deactivates
  // the previous one when a new one activates.
  fill_type_none_btn.set_group(fill_type_solid_btn);
  fill_type_cc_btn.set_group(fill_type_solid_btn);
  fill_type_swatch_btn.set_group(fill_type_solid_btn);
  fill_type_gradient_btn.set_group(fill_type_solid_btn);

  // signal_toggled fires twice on group changes (deactivate + activate).
  // Guard with get_active() so each handler only runs on the activation
  // edge, not the deactivation. syncing guards against re-entry from
  // refresh_fill_popover's programmatic set_active calls. Same idiom
  // as PaintEditor.
  //
  // s153: Definitive vs picker-bearing rule.
  //
  //   None / currentColor are *definitive* — they're complete on click
  //   (no value to choose). They commit immediately: write type, refresh,
  //   broadcast.
  //
  //   Solid / Swatch / Gradient are *picker-bearing* — they don't carry a
  //   commitable value on the toggle alone. The toggle reveals the
  //   appropriate picker; the broadcast happens on value-confirm
  //   (picker confirm / chip click / gradient Apply). Cancel from the
  //   picker reverts the toggle group to the prior committed type via
  //   the next refresh_fill_popover call — since we never wrote
  //   def_fill.type, the radio re-aligns to truth automatically.
  //
  //   Solid is the canonical case. Clicking Solid opens the colour
  //   picker (same path as the well-swatch click). Confirm writes
  //   type=Solid + colour and broadcasts. Cancel refreshes the popover
  //   so the radio snaps back to whatever def_fill.type still holds.
  fill_type_solid_btn.signal_toggled().connect([this]() {
    if (syncing || !fill_type_solid_btn.get_active()) return;
    // Initial colour for the picker. If the prior committed type was
    // already Solid, use its RGB. For any non-Solid prior type the
    // RGB fields hold whatever stale value was last there (often
    // zeros) — that's fine; the picker shows it as the starting point
    // and the user picks something else on the first interaction.
    color::Color initial(def_fill.r, def_fill.g, def_fill.b, 1.0);
    fill_pop.popdown();

    // Anchor the picker over the canvas, same idiom as the well-
    // swatch click.
    auto* root = dynamic_cast<Gtk::Window*>(self->get_root());
    if (!root) return;
    const int win_h = root->get_height();
    const int tb_w  = self->get_width();
    Gdk::Rectangle anchor(tb_w, std::max(0, win_h - 8), 1, 1);

    color_popover.open(
        anchor, initial, /*with_alpha=*/false,
        // on_changed: live updates as user drags inside the picker.
        // Per the design rule we DON'T broadcast here — broadcast is
        // on commit only. But we update def_fill so the well preview
        // tracks the live colour.
        [this](const color::Color& c) {
          def_fill.type = FillStyle::Type::Solid;
          def_fill.r = c.r;
          def_fill.g = c.g;
          def_fill.b = c.b;
          redraw_well();
        },
        /*has_arrow=*/false,
        // on_closed(committed): true on Return / outside-click /
        // pick-recent; false on Esc. On commit, broadcast the final
        // def_fill (already written by on_changed). On cancel, def_fill
        // may have been mutated by intermediate on_changed calls — the
        // CurvzColorPicker's self-revert routes the original colour
        // back through on_changed, so def_fill ends up restored to
        // its pre-open state. Either way, refresh_fill_popover so the
        // radio reflects truth.
        [this](bool committed) {
          refresh_fill_popover();
          if (committed) {
            emit_defaults();
          }
        });
  });
  fill_type_none_btn.signal_toggled().connect([this]() {
    if (syncing || !fill_type_none_btn.get_active()) return;
    def_fill.type = FillStyle::Type::None;
    fill_picker_open = false;
    refresh_fill_popover();
    redraw_well();
    emit_defaults();
  });
  fill_type_cc_btn.signal_toggled().connect([this]() {
    if (syncing || !fill_type_cc_btn.get_active()) return;
    def_fill.type = FillStyle::Type::CurrentColor;
    fill_picker_open = false;
    refresh_fill_popover();
    redraw_well();
    emit_defaults();
  });
  // S87 m1 v2: Swatch toggle reveals the chip picker. Doesn't change
  // def_fill.type — the toolbar has no Swatch FillStyle case (it's
  // session-only state, and binding wouldn't survive next session
  // anyway, per the copy-only design). When the user clicks a chip,
  // apply_swatch_pick_to_fill snaps the type back to Solid + the
  // chip's RGB, equivalent to a hex paste.
  fill_type_swatch_btn.signal_toggled().connect([this]() {
    if (syncing || !fill_type_swatch_btn.get_active()) return;
    fill_picker_open = true;
    refresh_fill_popover();
  });

  // S91 Gradient toggle. Snaps def_fill into a 2-stop linear gradient
  // when promoting from a non-gradient state; preserves existing stops
  // on a programmatic re-activate. The Edit… button below is what
  // actually opens the editor; toggling Gradient just switches the
  // *kind* of paint that gets used on next placement, with sensible
  // black→white defaults so the well and ramp preview render
  // immediately.
  //
  // No re-emit guard for "already a gradient" — the early-return below
  // covers it. Symmetric with the inspector's signal_type_changed
  // handler that seeds defaults on a fresh promotion.
  fill_type_gradient_btn.signal_toggled().connect([this]() {
    if (syncing || !fill_type_gradient_btn.get_active()) return;
    fill_picker_open = false;
    if (!def_fill.is_gradient()) {
      def_fill.type = FillStyle::Type::LinearGradient;
      if (def_fill.stops.empty()) {
        GradientStop s0; s0.offset = 0.0;
        s0.r = 0; s0.g = 0; s0.b = 0; s0.a = 1;
        GradientStop s1; s1.offset = 1.0;
        s1.r = 1; s1.g = 1; s1.b = 1; s1.a = 1;
        def_fill.stops = { s0, s1 };
      }
      def_fill.g_x1 = 0.0; def_fill.g_y1 = 0.5;
      def_fill.g_x2 = 1.0; def_fill.g_y2 = 0.5;
      def_fill.g_r  = 0.5;
    }
    refresh_fill_popover();
    redraw_well();
    emit_defaults();
  });

  // S91 Edit gradient button. Bubbles up via signal_gradient_edit_
  // requested with an apply_cb that writes the dialog's edit result
  // back into def_fill. MainWindow connects the signal to its
  // m_gradient_dialog.show — same dialog the inspector uses.
  //
  // Popdown the fill popover before opening the dialog: GTK4 autohide
  // popovers and modal windows interact poorly otherwise (the popover's
  // grab can fight the dialog's transient parent).
  fill_gradient_edit_btn.signal_clicked().connect([this]() {
    if (syncing) return;
    FillStyle current = def_fill;
    fill_pop.popdown();
    auto apply_cb = [this](FillStyle edited) {
      def_fill = edited;
      refresh_fill_popover();
      redraw_well();
      emit_defaults();
    };
    sig_gradient_edit.emit(current, apply_cb);
  });

  type_row->append(fill_type_solid_btn);
  type_row->append(fill_type_none_btn);
  type_row->append(fill_type_cc_btn);
  type_row->append(fill_type_swatch_btn);
  type_row->append(fill_type_gradient_btn);
  outer->append(*type_row);

  // Swatch + hex row — promoted to a member pointer so refresh_fill_
  // popover can drive its visibility.
  fill_color_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  fill_color_row->set_spacing(6);

  curvz::utils::set_name(fill_swatch, "pop_tb_fill_ch", "popover_toolbar_fill_color_swatch_da");
  fill_swatch.set_size_request(24, 24);
  fill_swatch.set_can_target(true);
  fill_swatch.add_css_class("tb-swatch");
  fill_color_row->append(fill_swatch);

  curvz::utils::set_name(fill_hex_entry, "pop_tb_fill_hex", "popover_toolbar_fill_hex_entry");
  fill_hex_entry.set_max_length(7);
  fill_hex_entry.set_width_chars(8);
  fill_hex_entry.set_placeholder_text("#RRGGBB");
  fill_hex_entry.add_css_class("tb-hex-entry");
  fill_hex_entry.on_commit(
      [this]() { apply_hex_to_fill(fill_hex_entry.get_text()); });
  fill_color_row->append(fill_hex_entry);
  outer->append(*fill_color_row);

  // S91 Gradient row: [ramp ─────][Edit…]. Visible only when def_fill
  // is a gradient. Same idiom as PaintEditor's gradient row — the ramp
  // is non-interactive; the button is the affordance for opening the
  // editor.
  fill_gradient_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  fill_gradient_row->set_spacing(6);
  curvz::utils::set_name(fill_gradient_ramp, "pop_tb_fill_grm", "popover_toolbar_fill_gradient_ramp_da");
  fill_gradient_ramp.set_size_request(120, 22);
  fill_gradient_ramp.set_hexpand(true);
  fill_gradient_ramp.add_css_class("tb-swatch");
  fill_gradient_ramp.set_valign(Gtk::Align::CENTER);
  curvz::utils::set_name(fill_gradient_edit_btn, "pop_tb_fill_ged", "popover_toolbar_fill_gradient_edit_btn");
  fill_gradient_edit_btn.set_label("Edit…");
  fill_gradient_edit_btn.set_tooltip_text(
      "Edit gradient stops, type, and angle…");
  fill_gradient_row->append(fill_gradient_ramp);
  fill_gradient_row->append(fill_gradient_edit_btn);
  outer->append(*fill_gradient_row);
  fill_gradient_row->set_visible(false);  // refresh_fill_popover gates

  // Click swatch → open colour picker popover (Solid mode only).
  //
  // The popover is parented to the main window, not to the Toolbar (the
  // Toolbar is a 48px-wide strip with no room to host a picker). We
  // position the popover's arrow at a point in the canvas's lower-left
  // area so it floats over the canvas rather than squashing against the
  // toolbar. That matches the pre-Phase-2 experience where the system
  // Gtk::ColorDialog was a floating window.
  auto swatch_click = Gtk::GestureClick::create();
  swatch_click->set_button(1);
  swatch_click->signal_pressed().connect([this](int, double, double) {
    if (def_fill.type != FillStyle::Type::Solid)
      return;
    color::Color initial(def_fill.r, def_fill.g, def_fill.b, 1.0);
    fill_pop.popdown();

    // Position the picker with its left edge at the toolbar's right
    // edge, and its bottom near the window's bottom. We use a 1x1
    // anchor at exactly that corner and let GTK expand the popover
    // body up-and-right from there. This reliably places the picker
    // in the canvas's lower-left area without the arrow ambiguity
    // that was making it drift to x=0.
    auto* root = dynamic_cast<Gtk::Window*>(self->get_root());
    if (!root) return;
    const int win_h = root->get_height();
    const int tb_w  = self->get_width();
    Gdk::Rectangle anchor(tb_w, std::max(0, win_h - 8), 1, 1);

    color_popover.open(
        anchor, initial, /*with_alpha=*/false,
        [this](const color::Color& c) {
          def_fill.type = FillStyle::Type::Solid;
          def_fill.r = c.r;
          def_fill.g = c.g;
          def_fill.b = c.b;
          refresh_fill_popover();
          redraw_well();
          emit_defaults();
        },
        /*has_arrow=*/false);
  });
  fill_swatch.add_controller(swatch_click);

  // S87 m1: copy-only swatch picker section, appended after the colour
  // row. Section is hidden until set_swatch_library() is called.
  build_swatch_picker_section(*outer, /*is_stroke=*/false);

  // Swatch draw func
  fill_swatch.set_draw_func(
      [this](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
        if (fill_mixed) {
          // S58n: diagonal-stripe swatch for mixed multi-selection.
          cr->set_source_rgb(1.0, 1.0, 1.0);
          cr->rectangle(0, 0, w, h);
          cr->fill();
          cr->save();
          cr->rectangle(0, 0, w, h);
          cr->clip();
          cr->set_source_rgb(0.55, 0.55, 0.55);
          cr->set_line_width(2.0);
          const double span = (double)(w + h) * 1.5;
          const double step = 5.0;
          cr->translate(w * 0.5, h * 0.5);
          cr->rotate(M_PI / 4.0);
          cr->translate(-span * 0.5, -span * 0.5);
          for (double yy = 0; yy < span; yy += step) {
            cr->move_to(0, yy);
            cr->line_to(span, yy);
          }
          cr->stroke();
          cr->restore();
        } else if (def_fill.type == FillStyle::Type::Solid) {
          cr->set_source_rgb(def_fill.r, def_fill.g, def_fill.b);
          cr->rectangle(0, 0, w, h);
          cr->fill();
        } else if (def_fill.type == FillStyle::Type::None) {
          // White background + red slash — clearly visible
          cr->set_source_rgb(0.92, 0.92, 0.92);
          cr->rectangle(0, 0, w, h);
          cr->fill();
          cr->set_source_rgb(0.85, 0.18, 0.18);
          cr->set_line_width(2.0);
          cr->move_to(2, 2);
          cr->line_to(w - 2, h - 2);
          cr->stroke();
        } else {
          // currentColor — checkerboard (white + mid-gray), no frame.
          // Fill swatch has no border for currentColor — stroke swatch
          // is the one that gets a visible border.
          const int cs = 2;  // 2px squares
          for (int row = 0; row < h; row += cs)
            for (int col = 0; col < w; col += cs) {
              bool light = ((row / cs + col / cs) % 2 == 0);
              if (light)
                cr->set_source_rgb(1.0, 1.0, 1.0);
              else
                cr->set_source_rgb(0.55, 0.55, 0.55);
              cr->rectangle(col, row, cs, cs);
              cr->fill();
            }
        }
        cr->set_source_rgba(0, 0, 0, 0.45);
        cr->set_line_width(1.0);
        cr->rectangle(0.5, 0.5, w - 1, h - 1);
        cr->stroke();
      });

  fill_pop.set_child(*outer);
  fill_pop.set_has_arrow(true);
  fill_pop.set_position(Gtk::PositionType::RIGHT);

  // S87 m1 fix5: previously the initial refresh was hooked to
  // fill_swatch.signal_realize(). That worked in v1 (when the swatch
  // was always visible from popover creation) but broke in v2 — the
  // colour row is now hidden when type != Solid, so the swatch doesn't
  // realize until the user picks Solid; the realize handler then fires
  // a re-entrant refresh during the click event, which destabilises
  // GTK's autohide popover grab and causes the popover to dismiss in
  // the next idle cycle (~3 ms later — confirmed via diag1 logs).
  // The refresh is now driven by the popover's own signal_show hook
  // (wired further up in the ctor next to the diag log connections),
  // which fires once per popup with no re-entrancy and no realize-
  // dependent timing.
}

void Toolbar::Impl::refresh_fill_popover() {
  // S87 m1 fix6: programmatic ToggleButton state changes run inside
  // syncing so the signal_toggled handlers bail (they re-check
  // syncing at the top). Pattern lifted from PaintEditor.
  syncing = true;

  // Update type button styles AND active state. The CSS class drives
  // the visual highlight (with the .tb-type-btn-active CSS rule from
  // fix4); set_active drives the GTK ToggleButton state which the
  // radio group uses to enforce one-active-at-a-time. Both must be
  // kept in sync.
  auto set_active = [](Gtk::ToggleButton &btn, bool on) {
    if (on)
      btn.add_css_class("tb-type-btn-active");
    else
      btn.remove_css_class("tb-type-btn-active");
    btn.set_active(on);
  };

  // Type-button active states.
  if (fill_mixed) {
    set_active(fill_type_solid_btn,    false);
    set_active(fill_type_none_btn,     false);
    set_active(fill_type_cc_btn,       false);
    set_active(fill_type_swatch_btn,   false);
    set_active(fill_type_gradient_btn, false);
  } else if (fill_picker_open) {
    set_active(fill_type_solid_btn,    false);
    set_active(fill_type_none_btn,     false);
    set_active(fill_type_cc_btn,       false);
    set_active(fill_type_swatch_btn,   true);
    set_active(fill_type_gradient_btn, false);
  } else {
    set_active(fill_type_solid_btn,
               def_fill.type == FillStyle::Type::Solid);
    set_active(fill_type_none_btn,
               def_fill.type == FillStyle::Type::None);
    set_active(fill_type_cc_btn,
               def_fill.type == FillStyle::Type::CurrentColor);
    set_active(fill_type_swatch_btn,   false);
    set_active(fill_type_gradient_btn, def_fill.is_gradient());
  }

  // Colour row visibility — hidden for gradients (gradient row owns
  // the chip-area real estate, same as PaintEditor).
  const bool show_color_row =
      fill_mixed
      || (!fill_picker_open
          && def_fill.type == FillStyle::Type::Solid);
  if (fill_color_row) fill_color_row->set_visible(show_color_row);

  // S91 Gradient row visibility — visible iff paint is a gradient and
  // the popover isn't displaying the swatch picker. Mixed-selection
  // suppresses it (the user clicks Gradient on the type row to snap
  // everyone to a default gradient first).
  if (fill_gradient_row) {
    const bool show_gradient_row =
        !fill_mixed
        && !fill_picker_open
        && def_fill.is_gradient();
    fill_gradient_row->set_visible(show_gradient_row);
    if (show_gradient_row) {
      // Repaint the ramp from current stops. Capture-by-value matches
      // PaintEditor::apply_gradient_row.
      std::vector<GradientStop> stops = def_fill.stops;
      std::sort(stops.begin(), stops.end(),
                [](const GradientStop &a, const GradientStop &b) {
                    return a.offset < b.offset;
                });
      fill_gradient_ramp.set_draw_func(
          [stops](const Cairo::RefPtr<Cairo::Context> &cr,
                  int w, int h) {
            if (stops.empty()) {
              cr->set_source_rgb(0.5, 0.5, 0.5);
              cr->rectangle(0, 0, w, h);
              cr->fill();
            } else if (stops.size() == 1) {
              const auto &s0 = stops.front();
              cr->set_source_rgba(s0.r, s0.g, s0.b, s0.a);
              cr->rectangle(0, 0, w, h);
              cr->fill();
            } else {
              auto pat = Cairo::LinearGradient::create(0, 0, w, 0);
              for (const auto &st : stops) {
                pat->add_color_stop_rgba(
                    std::clamp(st.offset, 0.0, 1.0),
                    st.r, st.g, st.b, st.a);
              }
              cr->set_source(pat);
              cr->rectangle(0, 0, w, h);
              cr->fill();
            }
            cr->set_source_rgba(0, 0, 0, 0.4);
            cr->set_line_width(1.0);
            cr->rectangle(0.5, 0.5, w - 1, h - 1);
            cr->stroke();
          });
      fill_gradient_ramp.queue_draw();
    }
  }

  // Hex entry contents.
  if (fill_mixed) {
    fill_hex_entry.set_text("");
    fill_hex_entry.set_placeholder_text("mixed");
    fill_hex_entry.set_sensitive(true);
  } else {
    fill_hex_entry.set_placeholder_text("#RRGGBB");
    if (def_fill.type == FillStyle::Type::Solid)
      fill_hex_entry.set_text(
          color_to_hex(def_fill.r, def_fill.g, def_fill.b));
    else
      fill_hex_entry.set_text("");
    fill_hex_entry.set_sensitive(def_fill.type == FillStyle::Type::Solid);
  }

  // Picker section visibility.
  if (fill_picker_section) {
    const bool show_picker =
        fill_picker_open
        && (swatch_library != nullptr)
        && (swatch_library->swatch_count() > 0)
        && !fill_mixed;
    fill_picker_section->set_visible(show_picker);
  }

  // Swatch button sensitivity.
  const bool swatch_enabled =
      (swatch_library != nullptr)
      && (swatch_library->swatch_count() > 0);
  fill_type_swatch_btn.set_sensitive(swatch_enabled);

  if (fill_swatch.get_realized())
    fill_swatch.queue_draw();

  syncing = false;
}

void Toolbar::Impl::apply_hex_to_fill(const std::string &hex) {
  // S93 m4: defensive syncing guard — same shape as the width_adj
  // handler. Hex entries commit on focus-leave, which can fire during
  // a refresh that replaces the entry's text via set_text(). Pre-S93
  // a stray commit during refresh would have leaked an emit_defaults
  // call. Belt-and-suspenders since the L1074 fix already plugs the
  // observed width-spin leak.
  if (syncing) return;
  double r, g, b;
  if (hex_to_color(hex, r, g, b)) {
    def_fill.type = FillStyle::Type::Solid;
    def_fill.r = r;
    def_fill.g = g;
    def_fill.b = b;
    refresh_fill_popover();
    redraw_well();
    emit_defaults();
  }
}

// ── Stroke popover
// ────────────────────────────────────────────────────────────
void Toolbar::Impl::build_stroke_popover() {
  curvz::utils::set_name(stroke_pop, "pop_tb_strk", "popover_toolbar_stroke_root");
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(8);
  outer->set_margin_top(10);
  outer->set_margin_bottom(10);
  outer->set_margin_start(10);
  outer->set_margin_end(10);

  // Title
  auto *title = Gtk::make_managed<Gtk::Label>("Stroke");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // Type row
  auto *type_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  type_row->set_spacing(4);

  curvz::utils::set_name(stroke_type_solid_btn, "pop_tb_strk_sol", "popover_toolbar_stroke_type_solid_toggle");
  stroke_type_solid_btn.set_label("Solid");
  stroke_type_solid_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(stroke_type_none_btn, "pop_tb_strk_non", "popover_toolbar_stroke_type_none_toggle");
  stroke_type_none_btn.set_label("None");
  stroke_type_none_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(stroke_type_cc_btn, "pop_tb_strk_cc", "popover_toolbar_stroke_type_currentcolor_toggle");
  stroke_type_cc_btn.set_label("currentColor");
  stroke_type_cc_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(stroke_type_swatch_btn, "pop_tb_strk_sw", "popover_toolbar_stroke_type_swatch_toggle");
  stroke_type_swatch_btn.set_label("Swatch");
  stroke_type_swatch_btn.add_css_class("tb-type-btn");

  // S87 m1 fix6: ToggleButton radio group, mirroring the fill side and
  // PaintEditor's idiom. Plain Gtk::Button inside an autohide popover
  // dismissed the popover on click — see fill-side comment for details.
  // S91: stroke deliberately has no Gradient toggle (fill-only surface).
  stroke_type_none_btn.set_group(stroke_type_solid_btn);
  stroke_type_cc_btn.set_group(stroke_type_solid_btn);
  stroke_type_swatch_btn.set_group(stroke_type_solid_btn);

  // s153: Definitive vs picker-bearing rule (mirrors fill side).
  // None / currentColor commit immediately; Solid opens the colour
  // picker and broadcasts only on confirm.
  stroke_type_solid_btn.signal_toggled().connect([this]() {
    if (syncing || !stroke_type_solid_btn.get_active()) return;
    color::Color initial(def_stroke.paint.r, def_stroke.paint.g,
                         def_stroke.paint.b, 1.0);
    stroke_pop.popdown();

    auto* root = dynamic_cast<Gtk::Window*>(self->get_root());
    if (!root) return;
    const int win_h = root->get_height();
    const int tb_w  = self->get_width();
    Gdk::Rectangle anchor(tb_w, std::max(0, win_h - 8), 1, 1);

    color_popover.open(
        anchor, initial, /*with_alpha=*/false,
        [this](const color::Color& c) {
          def_stroke.paint.type = FillStyle::Type::Solid;
          def_stroke.paint.r = c.r;
          def_stroke.paint.g = c.g;
          def_stroke.paint.b = c.b;
          redraw_well();
        },
        /*has_arrow=*/false,
        [this](bool committed) {
          refresh_stroke_popover();
          if (committed) {
            emit_defaults();
          }
        });
  });
  stroke_type_none_btn.signal_toggled().connect([this]() {
    if (syncing || !stroke_type_none_btn.get_active()) return;
    def_stroke.paint.type = FillStyle::Type::None;
    stroke_picker_open = false;
    refresh_stroke_popover();
    redraw_well();
    emit_defaults();
  });
  stroke_type_cc_btn.signal_toggled().connect([this]() {
    if (syncing || !stroke_type_cc_btn.get_active()) return;
    def_stroke.paint.type = FillStyle::Type::CurrentColor;
    stroke_picker_open = false;
    refresh_stroke_popover();
    redraw_well();
    emit_defaults();
  });
  stroke_type_swatch_btn.signal_toggled().connect([this]() {
    if (syncing || !stroke_type_swatch_btn.get_active()) return;
    stroke_picker_open = true;
    refresh_stroke_popover();
  });

  type_row->append(stroke_type_solid_btn);
  type_row->append(stroke_type_none_btn);
  type_row->append(stroke_type_cc_btn);
  type_row->append(stroke_type_swatch_btn);
  outer->append(*type_row);

  // Swatch + hex — promoted to a member pointer so refresh_stroke_
  // popover can drive its visibility.
  stroke_color_row =
      Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  stroke_color_row->set_spacing(6);

  curvz::utils::set_name(stroke_swatch, "pop_tb_strk_ch", "popover_toolbar_stroke_color_swatch_da");
  stroke_swatch.set_size_request(24, 24);
  stroke_swatch.set_can_target(true);
  stroke_swatch.add_css_class("tb-swatch");
  stroke_color_row->append(stroke_swatch);

  curvz::utils::set_name(stroke_hex_entry, "pop_tb_strk_hex", "popover_toolbar_stroke_hex_entry");
  stroke_hex_entry.set_max_length(7);
  stroke_hex_entry.set_width_chars(8);
  stroke_hex_entry.set_placeholder_text("#RRGGBB");
  stroke_hex_entry.add_css_class("tb-hex-entry");
  stroke_hex_entry.on_commit(
      [this]() { apply_hex_to_stroke(stroke_hex_entry.get_text()); });
  stroke_color_row->append(stroke_hex_entry);
  outer->append(*stroke_color_row);

  // S91: stroke does NOT get a gradient row (fill-only surface).

  // Click swatch → open colour picker popover (Solid mode only). Same
  // canvas-lower-left positioning as the fill swatch — see that handler
  // for the reasoning.
  auto swatch_click = Gtk::GestureClick::create();
  swatch_click->set_button(1);
  swatch_click->signal_pressed().connect([this](int, double, double) {
    if (def_stroke.paint.type != FillStyle::Type::Solid)
      return;
    color::Color initial(def_stroke.paint.r, def_stroke.paint.g,
                         def_stroke.paint.b, 1.0);
    stroke_pop.popdown();

    auto* root = dynamic_cast<Gtk::Window*>(self->get_root());
    if (!root) return;
    const int win_h = root->get_height();
    const int tb_w  = self->get_width();
    Gdk::Rectangle anchor(tb_w, std::max(0, win_h - 8), 1, 1);

    color_popover.open(
        anchor, initial, /*with_alpha=*/false,
        [this](const color::Color& c) {
          def_stroke.paint.type = FillStyle::Type::Solid;
          def_stroke.paint.r = c.r;
          def_stroke.paint.g = c.g;
          def_stroke.paint.b = c.b;
          refresh_stroke_popover();
          redraw_well();
          emit_defaults();
        },
        /*has_arrow=*/false);
  });
  stroke_swatch.add_controller(swatch_click);

  stroke_swatch.set_draw_func(
      [this](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
        const auto &p = def_stroke.paint;
        if (stroke_mixed) {
          // S58n: diagonal-stripe swatch for mixed multi-selection, plus a
          // dark inset frame to preserve the "stroke" visual signature.
          cr->set_source_rgb(1.0, 1.0, 1.0);
          cr->rectangle(0, 0, w, h);
          cr->fill();
          cr->save();
          cr->rectangle(0, 0, w, h);
          cr->clip();
          cr->set_source_rgb(0.55, 0.55, 0.55);
          cr->set_line_width(2.0);
          const double span = (double)(w + h) * 1.5;
          const double step = 5.0;
          cr->translate(w * 0.5, h * 0.5);
          cr->rotate(M_PI / 4.0);
          cr->translate(-span * 0.5, -span * 0.5);
          for (double yy = 0; yy < span; yy += step) {
            cr->move_to(0, yy);
            cr->line_to(span, yy);
          }
          cr->stroke();
          cr->restore();
          cr->set_source_rgb(0.102, 0.102, 0.102);
          cr->set_line_width(2.0);
          cr->rectangle(1.0, 1.0, w - 2, h - 2);
          cr->stroke();
        } else if (p.type == FillStyle::Type::Solid) {
          cr->set_source_rgb(p.r, p.g, p.b);
          cr->rectangle(0, 0, w, h);
          cr->fill();
        } else if (p.type == FillStyle::Type::None) {
          // White background + red slash
          cr->set_source_rgb(0.92, 0.92, 0.92);
          cr->rectangle(0, 0, w, h);
          cr->fill();
          cr->set_source_rgb(0.85, 0.18, 0.18);
          cr->set_line_width(2.0);
          cr->move_to(2, 2);
          cr->line_to(w - 2, h - 2);
          cr->stroke();
        } else {
          // currentColor — checkerboard interior + visible near-black
          // border (2px, inset).  The border is the "stroke" signal;
          // the checkerboard is the "currentColor" signal.
          const int cs = 2;  // 2px squares
          for (int row = 0; row < h; row += cs)
            for (int col = 0; col < w; col += cs) {
              bool light = ((row / cs + col / cs) % 2 == 0);
              if (light)
                cr->set_source_rgb(1.0, 1.0, 1.0);
              else
                cr->set_source_rgb(0.55, 0.55, 0.55);
              cr->rectangle(col, row, cs, cs);
              cr->fill();
            }
          // Near-black stroke-ness frame (#1a1a1a)
          cr->set_source_rgb(0.102, 0.102, 0.102);
          cr->set_line_width(2.0);
          cr->rectangle(1.0, 1.0, w - 2, h - 2);
          cr->stroke();
        }
        cr->set_source_rgba(0, 0, 0, 0.45);
        cr->set_line_width(1.0);
        cr->rectangle(0.5, 0.5, w - 1, h - 1);
        cr->stroke();
      });

  // S87 m1: copy-only swatch picker section, between the colour row and
  // the thickness/cap/join area. Section is hidden until
  // set_swatch_library() is called.
  build_swatch_picker_section(*outer, /*is_stroke=*/true);

  // Separator
  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  outer->append(*sep);

  // Thickness row
  auto *width_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  width_row->set_spacing(6);
  width_label.set_text("Thickness (px):");
  width_label.set_xalign(0.0f);
  width_label.add_css_class("tb-pop-label");
  width_label.set_hexpand(true);
  width_row->append(width_label);

  width_adj =
      Gtk::Adjustment::create(def_stroke.width, 0.0, 9999.0, 0.5, 5.0);
  curvz::utils::set_name(width_spin, "pop_tb_strk_w", "popover_toolbar_stroke_width_spn");
  width_spin.set_adjustment(width_adj);
  width_spin.set_digits(1);
  width_spin.set_width_chars(5);
  width_spin.add_css_class("tb-well-spin");
  width_adj->signal_value_changed().connect([this]() {
    // S93 m4: defensive syncing guard. Programmatic set_value
    // calls during a refresh fire this signal synchronously; without
    // the guard, a sync_from_selection (which calls refresh_stroke_
    // popover, which set_value's the adjustment) leaked an
    // emit_defaults call back to the host. That in turn pushed a
    // spurious 'Edit appearance' undo entry on every selection
    // change and — in multi-select — bled the primary's fill/stroke
    // onto siblings via MainWindow's defaults_changed handler. The
    // refresh function was also fixed to keep syncing true through
    // its width-update section; this guard is defence-in-depth.
    if (syncing) return;
    const CanvasModel* cm = doc ? &doc->canvas : nullptr;
    double display = width_adj->get_value();
    if (cm && cm->display_mode == DisplayMode::Physical) {
      double short_phys = std::min(cm->phys_width, cm->phys_height);
      int q = std::max(1, cm->quality);
      def_stroke.width = short_phys > 0 ? (display / short_phys) * q : display;
    } else if (cm && cm->display_mode == DisplayMode::RatioQuality) {
      int q = std::max(1, cm->quality);
      def_stroke.width = (display / 100.0) * q;
    } else {
      def_stroke.width = display;
    }
    emit_defaults();
  });
  width_spin.signal_activate().connect([this]() {
    width_spin.update();
    stroke_pop.popdown();
    sig_canvas_focus.emit();
  });
  // Key controller belt-and-suspenders for SpinButton inside popover
  {
    auto kc = Gtk::EventControllerKey::create();
    kc->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    kc->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) -> bool {
          if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            width_spin.update();
            stroke_pop.popdown();
            sig_canvas_focus.emit();
            return false;
          }
          return false;
        },
        false);
    width_spin.add_controller(kc);
  }
  width_row->append(width_spin);
  width_unit_lbl.set_text("px");
    width_label.set_text("Thickness (px):");
  width_unit_lbl.add_css_class("prop-width-unit");
  width_row->append(width_unit_lbl);
  outer->append(*width_row);

  // Cap row
  auto make_section = [&](const std::string &lbl_text) -> Gtk::Box * {
    auto *vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    vbox->set_spacing(3);
    auto *lbl = Gtk::make_managed<Gtk::Label>(lbl_text);
    lbl->set_xalign(0.0f);
    lbl->add_css_class("tb-pop-label");
    vbox->append(*lbl);
    auto *hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    hbox->set_spacing(8);
    vbox->append(*hbox);
    outer->append(*vbox);
    return hbox;
  };

  // Helper: create an icon button for cap/join
  auto make_icon_btn = [](const char* icon, const char* tip) -> Gtk::Button* {
    auto *btn = Gtk::make_managed<Gtk::Button>();
    btn->set_icon_name(icon);
    btn->set_has_frame(false);
    btn->add_css_class("tb-icon-btn");
    btn->set_tooltip_text(tip);
    return btn;
  };

  auto *cap_row = make_section("Cap");
  cap_butt_btn   = make_icon_btn("curvz-cap-butt-symbolic",   "Butt");
  curvz::utils::set_name(cap_butt_btn, "pop_tb_strk_cb", "popover_toolbar_stroke_cap_butt_btn");
  cap_round_btn  = make_icon_btn("curvz-cap-round-symbolic",  "Round");
  curvz::utils::set_name(cap_round_btn, "pop_tb_strk_cr", "popover_toolbar_stroke_cap_round_btn");
  cap_square_btn = make_icon_btn("curvz-cap-square-symbolic", "Square");
  curvz::utils::set_name(cap_square_btn, "pop_tb_strk_cs", "popover_toolbar_stroke_cap_square_btn");
  cap_butt_btn->signal_clicked().connect([this]() {
    def_stroke.cap = LineCap::Butt;
    update_cap_buttons();
    emit_defaults();
  });
  cap_round_btn->signal_clicked().connect([this]() {
    def_stroke.cap = LineCap::Round;
    update_cap_buttons();
    emit_defaults();
  });
  cap_square_btn->signal_clicked().connect([this]() {
    def_stroke.cap = LineCap::Square;
    update_cap_buttons();
    emit_defaults();
  });
  cap_row->append(*cap_butt_btn);
  cap_row->append(*cap_round_btn);
  cap_row->append(*cap_square_btn);

  auto *join_row = make_section("Join");
  join_miter_btn = make_icon_btn("curvz-join-miter-symbolic", "Miter");
  curvz::utils::set_name(join_miter_btn, "pop_tb_strk_jm", "popover_toolbar_stroke_join_miter_btn");
  join_round_btn = make_icon_btn("curvz-join-round-symbolic", "Round");
  curvz::utils::set_name(join_round_btn, "pop_tb_strk_jr", "popover_toolbar_stroke_join_round_btn");
  join_bevel_btn = make_icon_btn("curvz-join-bevel-symbolic", "Bevel");
  curvz::utils::set_name(join_bevel_btn, "pop_tb_strk_jb", "popover_toolbar_stroke_join_bevel_btn");
  join_miter_btn->signal_clicked().connect([this]() {
    def_stroke.join = LineJoin::Miter;
    update_join_buttons();
    emit_defaults();
  });
  join_round_btn->signal_clicked().connect([this]() {
    def_stroke.join = LineJoin::Round;
    update_join_buttons();
    emit_defaults();
  });
  join_bevel_btn->signal_clicked().connect([this]() {
    def_stroke.join = LineJoin::Bevel;
    update_join_buttons();
    emit_defaults();
  });
  join_row->append(*join_miter_btn);
  join_row->append(*join_round_btn);
  join_row->append(*join_bevel_btn);

  stroke_pop.set_child(*outer);
  stroke_pop.set_has_arrow(true);
  stroke_pop.set_position(Gtk::PositionType::RIGHT);

  // S87 m1 fix5: see fill_pop equivalent comment. Refresh is driven by
  // the popover's signal_show hook, not a child realize hook — moves
  // the refresh out of the click event's idle window and avoids the
  // re-entrant autohide-dismiss bug.
}

void Toolbar::Impl::refresh_stroke_popover() {
  // S87 m1 fix6: syncing wraps programmatic ToggleButton state to
  // avoid signal_toggled re-entry. See refresh_fill_popover for the
  // matching pattern.
  syncing = true;

  auto set_active = [](Gtk::ToggleButton &btn, bool on) {
    if (on)
      btn.add_css_class("tb-type-btn-active");
    else
      btn.remove_css_class("tb-type-btn-active");
    btn.set_active(on);
  };
  const auto &p = def_stroke.paint;

  // S87 m1 v2: visibility model — see refresh_fill_popover for the
  // matrix. The thickness / cap / join sub-area is independent and
  // always visible (those properties belong to the stroke regardless
  // of whether the picker tab is open).
  if (stroke_mixed) {
    set_active(stroke_type_solid_btn,  false);
    set_active(stroke_type_none_btn,   false);
    set_active(stroke_type_cc_btn,     false);
    set_active(stroke_type_swatch_btn, false);
  } else if (stroke_picker_open) {
    set_active(stroke_type_solid_btn,  false);
    set_active(stroke_type_none_btn,   false);
    set_active(stroke_type_cc_btn,     false);
    set_active(stroke_type_swatch_btn, true);
  } else {
    set_active(stroke_type_solid_btn,  p.type == FillStyle::Type::Solid);
    set_active(stroke_type_none_btn,   p.type == FillStyle::Type::None);
    set_active(stroke_type_cc_btn,     p.type == FillStyle::Type::CurrentColor);
    set_active(stroke_type_swatch_btn, false);
  }

  // Colour row visibility.
  const bool show_color_row =
      stroke_mixed
      || (!stroke_picker_open && p.type == FillStyle::Type::Solid);
  if (stroke_color_row) stroke_color_row->set_visible(show_color_row);

  // S91: no stroke gradient row to manage (fill-only gradient surface).

  // Hex entry contents.
  if (stroke_mixed) {
    stroke_hex_entry.set_text("");
    stroke_hex_entry.set_placeholder_text("mixed");
    stroke_hex_entry.set_sensitive(true);
  } else {
    stroke_hex_entry.set_placeholder_text("#RRGGBB");
    if (p.type == FillStyle::Type::Solid)
      stroke_hex_entry.set_text(color_to_hex(p.r, p.g, p.b));
    else
      stroke_hex_entry.set_text("");
    stroke_hex_entry.set_sensitive(p.type == FillStyle::Type::Solid);
  }

  // Picker section visibility.
  if (stroke_picker_section) {
    const bool show_picker =
        stroke_picker_open
        && (swatch_library != nullptr)
        && (swatch_library->swatch_count() > 0)
        && !stroke_mixed;
    stroke_picker_section->set_visible(show_picker);
  }

  // Greyed-out Swatch button when no library wired or empty.
  const bool swatch_enabled =
      (swatch_library != nullptr)
      && (swatch_library->swatch_count() > 0);
  stroke_type_swatch_btn.set_sensitive(swatch_enabled);

  if (stroke_swatch.get_realized())
    stroke_swatch.queue_draw();

  // S93 m4: syncing must remain true through the rest of the
  // function. Pre-S93 the flag flipped to false here, BEFORE the
  // width_adj->set_value() calls below — those set_value calls
  // fire signal_value_changed synchronously, the handler at the
  // adjustment definition (currently L1059) had no syncing check,
  // and so every selection-change leaked an emit_defaults() call
  // ~30ms later. That signal in turn triggered MainWindow's
  // defaults_changed handler, which broadcasts the toolbar's
  // primary-fed fill/stroke onto every selected target — bleeding
  // a Compound's fill onto its multi-select sibling, stripping the
  // Compound's bound_style, and pushing a spurious 'Edit appearance'
  // command to the undo stack on every selection change.
  //
  // The flag is now flipped at the genuine end of the function,
  // wrapping the thickness / cap / join updates too. The
  // width_adj signal_value_changed handler also got a defensive
  // syncing guard (see comment at the connect site below).

  // Thickness — convert from doc units to display units
  const CanvasModel* cm = doc ? &doc->canvas : nullptr;
  if (cm && cm->display_mode == DisplayMode::Physical) {
    double short_phys = std::min(cm->phys_width, cm->phys_height);
    int q = std::max(1, cm->quality);
    double display = short_phys > 0 ? (def_stroke.width / q) * short_phys : def_stroke.width;
    double step = (cm->phys_unit == "in") ? 0.001 : 0.01;
    width_adj->set_step_increment(step);
    width_adj->set_page_increment(step * 10.0);
    width_adj->set_value(display);
    width_spin.set_digits(3);
    width_unit_lbl.set_text(cm->phys_unit);
    width_label.set_text("Thickness (" + cm->phys_unit + "):");
  } else if (cm && cm->display_mode == DisplayMode::RatioQuality) {
    int q = std::max(1, cm->quality);
    width_adj->set_step_increment(0.05);
    width_adj->set_page_increment(0.5);
    width_adj->set_value((def_stroke.width / q) * 100.0);
    width_spin.set_digits(2);
    width_unit_lbl.set_text("%");
    width_label.set_text("Thickness (%):");
  } else {
    width_adj->set_step_increment(0.5);
    width_adj->set_page_increment(5.0);
    width_adj->set_value(def_stroke.width);
    width_spin.set_digits(1);
    width_unit_lbl.set_text("px");
    width_label.set_text("Thickness (px):");
  }
  update_cap_buttons();
  update_join_buttons();

  syncing = false;
}

void Toolbar::Impl::apply_hex_to_stroke(const std::string &hex) {
  // S93 m4: defensive syncing guard. See apply_hex_to_fill.
  if (syncing) return;
  double r, g, b;
  if (hex_to_color(hex, r, g, b)) {
    def_stroke.paint.type = FillStyle::Type::Solid;
    def_stroke.paint.r = r;
    def_stroke.paint.g = g;
    def_stroke.paint.b = b;
    refresh_stroke_popover();
    redraw_well();
    emit_defaults();
  }
}

// ── Cap / Join icon draw funcs
// ─────────────────────────────────────────────────

void Toolbar::Impl::update_cap_buttons() {
  auto set_active = [](Gtk::Button* btn, bool active) {
    if (!btn) return;
    if (active) btn->add_css_class("tb-icon-btn-active");
    else        btn->remove_css_class("tb-icon-btn-active");
  };
  set_active(cap_butt_btn,   def_stroke.cap == LineCap::Butt);
  set_active(cap_round_btn,  def_stroke.cap == LineCap::Round);
  set_active(cap_square_btn, def_stroke.cap == LineCap::Square);
}

void Toolbar::Impl::update_join_buttons() {
  auto set_active = [](Gtk::Button* btn, bool active) {
    if (!btn) return;
    if (active) btn->add_css_class("tb-icon-btn-active");
    else        btn->remove_css_class("tb-icon-btn-active");
  };
  set_active(join_miter_btn, def_stroke.join == LineJoin::Miter);
  set_active(join_round_btn, def_stroke.join == LineJoin::Round);
  set_active(join_bevel_btn, def_stroke.join == LineJoin::Bevel);
}

// ── Well draw
// ─────────────────────────────────────────────────────────────────
void Toolbar::Impl::redraw_well() {
  FillStyle fill = def_fill;
  StrokeStyle stroke = def_stroke;
  bool fmixed = fill_mixed;
  bool smixed = stroke_mixed;

  well.set_draw_func([fill, stroke, fmixed, smixed]
                       (const Cairo::RefPtr<Cairo::Context> &cr,
                        int /*w*/, int /*h*/) {
    const double sq = 14.0, off = 8.0;

    // Draw a checkerboard (white + mid-gray) inside [x, y] of size sz.
    auto draw_checker = [&](double x, double y, double sz) {
      const int cs = 2;  // 2px squares
      for (int r = 0; r < (int)sz; r += cs)
        for (int c = 0; c < (int)sz; c += cs) {
          bool light = ((r / cs + c / cs) % 2 == 0);
          if (light)
            cr->set_source_rgb(1.0, 1.0, 1.0);
          else
            cr->set_source_rgb(0.55, 0.55, 0.55);
          cr->rectangle(x + c, y + r, cs, cs);
          cr->fill();
        }
    };

    // S58n: Diagonal-stripe swatch to indicate mixed paint across a
    // multi-selection. Matches the Inspector Appearance mixed renderer.
    auto draw_mixed = [&](double x, double y, double sz) {
      cr->set_source_rgb(1.0, 1.0, 1.0);
      cr->rectangle(x, y, sz, sz);
      cr->fill();
      cr->save();
      cr->rectangle(x, y, sz, sz);
      cr->clip();
      cr->set_source_rgb(0.55, 0.55, 0.55);
      cr->set_line_width(1.5);
      const double span = sz * 2.0;
      const double step = 3.0;
      cr->translate(x + sz * 0.5, y + sz * 0.5);
      cr->rotate(M_PI / 4.0);
      cr->translate(-span * 0.5, -span * 0.5);
      for (double yy = 0; yy < span; yy += step) {
        cr->move_to(0, yy);
        cr->line_to(span, yy);
      }
      cr->stroke();
      cr->restore();
    };

    // S91 helper: render a small horizontal gradient ramp inside [x,y,sz].
    // Used by both fill and stroke when the corresponding paint is a
    // linear or radial gradient. We always paint a 1D ramp here even
    // for radial — the well preview is too small to show 2D structure
    // meaningfully; stop ordering is the useful signal.
    auto draw_gradient = [&](double x, double y, double sz,
                             const FillStyle &fs) {
      // Sort defensively so reversed lists still render in canonical
      // order (mirrors PaintEditor::apply_gradient_row).
      std::vector<GradientStop> sorted = fs.stops;
      std::sort(sorted.begin(), sorted.end(),
                [](const GradientStop &a, const GradientStop &b) {
                    return a.offset < b.offset;
                });
      if (sorted.empty()) {
        cr->set_source_rgb(0.5, 0.5, 0.5);
        cr->rectangle(x, y, sz, sz);
        cr->fill();
      } else if (sorted.size() == 1) {
        const auto &s0 = sorted.front();
        cr->set_source_rgba(s0.r, s0.g, s0.b, s0.a);
        cr->rectangle(x, y, sz, sz);
        cr->fill();
      } else {
        auto pat = Cairo::LinearGradient::create(x, 0, x + sz, 0);
        for (const auto &st : sorted) {
          pat->add_color_stop_rgba(
              std::clamp(st.offset, 0.0, 1.0),
              st.r, st.g, st.b, st.a);
        }
        cr->set_source(pat);
        cr->rectangle(x, y, sz, sz);
        cr->fill();
      }
    };

    // Fill-style square at (x,y) size sz.
    auto draw_fill_square = [&](double x, double y, double sz,
                                const FillStyle &fs, bool mixed) {
      if (mixed) {
        draw_mixed(x, y, sz);
      } else if (fs.type == FillStyle::Type::CurrentColor) {
        draw_checker(x, y, sz);
      } else if (fs.type == FillStyle::Type::None) {
        cr->set_source_rgb(0.92, 0.92, 0.92);
        cr->rectangle(x, y, sz, sz);
        cr->fill();
      } else if (fs.is_gradient()) {
        draw_gradient(x, y, sz, fs);
      } else {
        cr->set_source_rgb(fs.r, fs.g, fs.b);
        cr->rectangle(x, y, sz, sz);
        cr->fill();
      }
    };

    auto draw_none_slash = [&](double x, double y, double sz) {
      cr->set_source_rgb(0.85, 0.18, 0.18);
      cr->set_line_width(1.5);
      cr->move_to(x + 2, y + 2);
      cr->line_to(x + sz - 2, y + sz - 2);
      cr->stroke();
    };

    // Stroke square — drawn first (behind fill).
    if (smixed) {
      // Mixed stroke: diagonal stripes with the dark interior flavour.
      draw_mixed(off, off, sq);
      cr->set_source_rgb(0.18, 0.18, 0.18);
      cr->set_line_width(2.0);
      cr->rectangle(off + 0.5, off + 0.5, sq - 1, sq - 1);
      cr->stroke();
    } else if (stroke.paint.type == FillStyle::Type::Solid) {
      cr->set_source_rgb(0.18, 0.18, 0.18);
      cr->rectangle(off, off, sq, sq);
      cr->fill();
      cr->set_source_rgb(stroke.paint.r, stroke.paint.g, stroke.paint.b);
      cr->set_line_width(2.0);
      cr->rectangle(off + 0.5, off + 0.5, sq - 1, sq - 1);
      cr->stroke();
    } else if (stroke.paint.type == FillStyle::Type::None) {
      cr->set_source_rgb(0.92, 0.92, 0.92);
      cr->rectangle(off, off, sq, sq);
      cr->fill();
      draw_none_slash(off, off, sq);
    } else if (stroke.paint.is_gradient()) {
      // S91 gradient stroke preview. Fill the square with a ramp the
      // same way fill does, then draw the dark inner background to
      // emulate the "stroke around fill" frame visual. The stroke
      // version of the well overlay is just a colour signal — using
      // the gradient as the colour is the right read.
      cr->set_source_rgb(0.18, 0.18, 0.18);
      cr->rectangle(off, off, sq, sq);
      cr->fill();
      // Draw the gradient as a 2px-thick frame: render the gradient
      // into a slightly-inset square, then punch out the inner. We
      // approximate this with a fill + an inset cover-up so the visual
      // matches the Solid case's stroke-frame idiom.
      draw_gradient(off, off, sq, stroke.paint);
      cr->set_source_rgb(0.18, 0.18, 0.18);
      cr->rectangle(off + 2, off + 2, sq - 4, sq - 4);
      cr->fill();
    } else {
      // currentColor: checkerboard + near-black stroke-signal frame.
      draw_checker(off, off, sq);
      cr->set_source_rgb(0.102, 0.102, 0.102);
      cr->set_line_width(1.5);
      cr->rectangle(off + 0.75, off + 0.75, sq - 1.5, sq - 1.5);
      cr->stroke();
    }

    // Fill square — drawn on top.
    draw_fill_square(0, 0, sq, fill, fmixed);
    if (!fmixed && fill.type == FillStyle::Type::None)
      draw_none_slash(0, 0, sq);
    cr->set_source_rgba(0, 0, 0, 0.55);
    cr->set_line_width(1.0);
    cr->rectangle(0.5, 0.5, sq - 1, sq - 1);
    cr->stroke();
  });
  if (well.get_realized())
    well.queue_draw();
}

void Toolbar::set_zoom(double zoom_rel) { m_impl->set_zoom(zoom_rel); }

void Toolbar::set_zoom_alt(bool) {
  // Zoom +/- buttons removed — nothing to update
}

void Toolbar::set_popup_unit(Unit u) {
  m_impl->popup_unit = u;
  std::string txt = std::string("Units: ") + UnitSystem::label(u);
  // s153 sub-ship 2: all eight placement popovers live in Impl now.
  if (m_impl->rect_unit_lbl)    m_impl->rect_unit_lbl->set_text(txt);
  if (m_impl->ellipse_unit_lbl) m_impl->ellipse_unit_lbl->set_text(txt);
  if (m_impl->line_unit_lbl)    m_impl->line_unit_lbl->set_text(txt);
  if (m_impl->ref_unit_lbl)     m_impl->ref_unit_lbl->set_text(txt);
  if (m_impl->text_unit_lbl)    m_impl->text_unit_lbl->set_text(txt);
  if (m_impl->poly_unit_lbl)    m_impl->poly_unit_lbl->set_text(txt);
  if (m_impl->spiral_unit_lbl)  m_impl->spiral_unit_lbl->set_text(txt);
}

void Toolbar::set_document(CurvzDocument* doc) {
  m_impl->doc = doc;
  if (doc) {
    // In physical mode the effective unit for popovers is phys_unit
    if (doc->canvas.display_mode == DisplayMode::Physical) {
      m_impl->popup_unit = UnitSystem::parse_unit(doc->canvas.phys_unit);
    } else {
      m_impl->popup_unit = doc->canvas.display_unit;
    }
    set_popup_unit(m_impl->popup_unit);
  }
}

void Toolbar::Impl::reset_to_defaults() {
  def_fill.type = FillStyle::Type::CurrentColor;
  def_stroke.paint.type = FillStyle::Type::None;
  def_stroke.width = 1.0;
  def_stroke.cap = LineCap::Butt;
  def_stroke.join = LineJoin::Miter;
  redraw_well();
  refresh_fill_popover();
  refresh_stroke_popover();
  update_cap_buttons();
  update_join_buttons();
  emit_defaults();
}

void Toolbar::Impl::emit_defaults() {
  if (syncing)
    return;
  sig_defaults.emit(def_fill, def_stroke);
}

void Toolbar::Impl::sync_from_object(const FillStyle &fill,
                                     const StrokeStyle &stroke) {
  syncing = true;
  def_fill = fill;
  def_stroke = stroke;
  fill_mixed = false;
  stroke_mixed = false;
  redraw_well();
  refresh_fill_popover();
  refresh_stroke_popover();
  update_cap_buttons();
  update_join_buttons();
  syncing = false;
}

// ══════════════════════════════════════════════════════════════════════════════
// S58n: multi-select aware sync. Uniformity check mirrors the Inspector's
// rule (S58i): compare FillStyle at 8-bit hex granularity so two paint-paths
// that display the same hex are equal. Paint targets are Paths + Compounds +
// Text (matches the toolbar broadcast target set in MainWindow). Groups are
// pass-through containers per user rule and are flattened via recursion.
// ══════════════════════════════════════════════════════════════════════════════
namespace {

bool tb_fills_equal(const FillStyle& a, const FillStyle& b) {
    if (a.type != b.type) return false;
    if (a.type == FillStyle::Type::Solid) {
        auto q = [](double v) {
            return (int)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
        };
        return q(a.r) == q(b.r) && q(a.g) == q(b.g) && q(a.b) == q(b.b);
    }
    // S91: gradient comparison — same-typed gradients are equal iff they
    // have matching stops (offset + RGBA at 8-bit granularity) and
    // matching geometry. This isn't ideal for mixed-but-similar
    // gradients (e.g. same colours, slightly different angles —
    // "mixed" still feels right) but matches the spirit of the Solid
    // 8-bit-hex bucketing.
    if (a.is_gradient()) {
        if (a.stops.size() != b.stops.size()) return false;
        auto q = [](double v) {
            return (int)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
        };
        for (std::size_t i = 0; i < a.stops.size(); ++i) {
            const auto& sa = a.stops[i];
            const auto& sb = b.stops[i];
            if (q(sa.offset) != q(sb.offset)) return false;
            if (q(sa.r) != q(sb.r)) return false;
            if (q(sa.g) != q(sb.g)) return false;
            if (q(sa.b) != q(sb.b)) return false;
            if (q(sa.a) != q(sb.a)) return false;
        }
        // Geometry compared at the same 8-bit granularity as colours
        // (objectBoundingBox space is 0..1, so the bucket count is the
        // same). Equality on all five fields, which covers both linear
        // (uses x1/y1/x2/y2) and radial (uses x1/y1 fx,fy / x2/y2 cx,cy
        // / r).
        if (q(a.g_x1) != q(b.g_x1)) return false;
        if (q(a.g_y1) != q(b.g_y1)) return false;
        if (q(a.g_x2) != q(b.g_x2)) return false;
        if (q(a.g_y2) != q(b.g_y2)) return false;
        if (q(a.g_r)  != q(b.g_r))  return false;
    }
    return true;
}

// Walk a selection and gather its paint targets (Path / Compound / Text).
// Groups are recursed into. Mirrors the collect() lambda in MainWindow's
// signal_defaults_changed handler (S58h) so the display and the broadcast
// see the same object set.
void tb_collect_paint_targets(SceneNode* node,
                              std::vector<SceneNode*>& out) {
    if (!node) return;
    if (node->is_path() || node->is_compound() ||
        node->type == SceneNode::Type::Text) {
        out.push_back(node);
    } else if (node->is_group()) {
        for (auto &child : node->children)
            tb_collect_paint_targets(child.get(), out);
    }
}

// ── S87 m1 chip helpers ──────────────────────────────────────────────────────
//
// Mirrors PaintEditor's anonymous-namespace helpers (paint_chip,
// resolve_solid, kAllPaletteId). Duplicated rather than promoted to a
// shared header because (a) it's small, (b) promoting would leak Cairo
// out of a widget header that's currently GTK-only, and (c) the chip
// metrics are intentionally tunable per host (PaintEditor uses 18px;
// here we match StylesPanel's 24px to match the more recent visual
// convention).
//
// If a future milestone extracts a shared SwatchChipGrid widget, both
// PaintEditor and Toolbar can switch to it and these can go.

constexpr const char* kAllPaletteId = "__all__";

constexpr int    TB_CHIP_SIZE                  = 24;
constexpr double TB_CHIP_CORNER_RADIUS         = 3.0;
constexpr double TB_CHIP_BORDER_GREY           = 0.55;
constexpr double TB_CHIP_BORDER_WIDTH          = 1.0;
constexpr double TB_CHIP_BORDER_LUMA_THRESHOLD = 0.85;

void tb_paint_chip(const Cairo::RefPtr<Cairo::Context>& cr,
                   int w, int h,
                   const color::Color& c) {
    const double r = TB_CHIP_CORNER_RADIUS;
    const double x = 0.5;
    const double y = 0.5;
    const double ww = static_cast<double>(w) - 1.0;
    const double hh = static_cast<double>(h) - 1.0;

    auto rounded_rect = [&](double x0, double y0,
                            double x1, double y1, double rad) {
        cr->move_to(x0 + rad, y0);
        cr->line_to(x1 - rad, y0);
        cr->arc(x1 - rad, y0 + rad, rad, -M_PI / 2, 0);
        cr->line_to(x1, y1 - rad);
        cr->arc(x1 - rad, y1 - rad, rad, 0, M_PI / 2);
        cr->line_to(x0 + rad, y1);
        cr->arc(x0 + rad, y1 - rad, rad, M_PI / 2, M_PI);
        cr->line_to(x0, y0 + rad);
        cr->arc(x0 + rad, y0 + rad, rad, M_PI, 3 * M_PI / 2);
        cr->close_path();
    };

    rounded_rect(x, y, x + ww, y + hh, r);
    cr->set_source_rgba(c.r, c.g, c.b, c.a);
    cr->fill_preserve();

    double luma = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    if (luma > TB_CHIP_BORDER_LUMA_THRESHOLD || c.a < 0.95) {
        cr->set_source_rgba(TB_CHIP_BORDER_GREY, TB_CHIP_BORDER_GREY,
                            TB_CHIP_BORDER_GREY, 1.0);
        cr->set_line_width(TB_CHIP_BORDER_WIDTH);
        cr->stroke();
    } else {
        cr->begin_new_path();
    }
    // No active-binding ring on copy-only chips — toolbar has no
    // notion of "the bound swatch". Click is a copy, full stop.
}

// Resolve a swatch id to its display name + colour. Returns false
// (no fields written) if missing or non-solid. Future gradient
// swatches need their own visit case.
bool tb_resolve_solid(const color::SwatchLibrary& lib,
                      const std::string& id,
                      std::string& name_out,
                      color::Color& color_out) {
    const color::Swatch* sw = lib.find_swatch(id);
    if (!sw) return false;
    const auto* solid = std::get_if<color::SolidSwatch>(sw);
    if (!solid) return false;
    name_out  = solid->header.name;
    color_out = solid->color;
    return true;
}

} // anonymous namespace

void Toolbar::Impl::sync_from_selection(const std::vector<SceneNode*>& sel,
                                        SceneNode* primary) {
    if (!primary) {
        // Nothing selected — don't change well state (keep whatever the
        // defaults currently are) but clear the mixed flags so a stale
        // stripe render doesn't persist.
        fill_mixed = false;
        stroke_mixed = false;
        redraw_well();
        refresh_fill_popover();
        refresh_stroke_popover();
        return;
    }

    // Expand the selection to paint targets (Groups flatten; Paths /
    // Compounds / Text stay terminal).
    std::vector<SceneNode*> targets;
    for (SceneNode* n : sel) tb_collect_paint_targets(n, targets);

    // Display reads from primary (Path → itself; Compound → itself per
    // S58d rule; a Group primary → first leaf in its tree).
    SceneNode* disp = primary;
    if (primary->is_group()) {
        std::vector<SceneNode*> flat;
        tb_collect_paint_targets(primary, flat);
        if (!flat.empty()) disp = flat.front();
    }

    syncing = true;
    def_fill   = disp->fill;
    def_stroke = disp->stroke;

    // Uniformity check. One target or none → not mixed.
    fill_mixed = false;
    stroke_mixed = false;
    if (targets.size() >= 2) {
        const FillStyle& first_fill   = targets[0]->fill;
        const FillStyle& first_stroke = targets[0]->stroke.paint;
        for (size_t i = 1; i < targets.size(); ++i) {
            if (!fill_mixed &&
                !tb_fills_equal(first_fill, targets[i]->fill))
                fill_mixed = true;
            if (!stroke_mixed &&
                !tb_fills_equal(first_stroke, targets[i]->stroke.paint))
                stroke_mixed = true;
            if (fill_mixed && stroke_mixed) break; // both decided
        }
    }

    redraw_well();
    refresh_fill_popover();
    refresh_stroke_popover();
    update_cap_buttons();
    update_join_buttons();
    syncing = false;
}

// ── Align & Distribute
// ────────────────────────────────────────────────────────

// Draw a single align/distribute icon into a 32×32 area.
// Uses the same Cairo-icon pattern as cap/join buttons.
// S117 m5: align icon was hardcoded for dark motif (light icon on dark
// surface). On light motif (icon on #dddddd toolbar) the same #e0e0e0
// rects vanished. Same problem class as the ruler chrome — Cairo paint
// can't read CSS tokens, so we keep two palettes and pick at draw time.
// Mirrors RulerPalette in src/Ruler.cpp.
struct AlignIconPalette {
  double fg_enabled;   // rect fill when button is enabled
  double fg_disabled;  // rect fill when button is disabled (faded)
  double ref;          // reference line / tick colour
};
static const AlignIconPalette kAlignDark  = {0.88, 0.50, 0.38};
static const AlignIconPalette kAlignLight = {0.20, 0.55, 0.40};
static const AlignIconPalette &align_palette_for(Motif m) {
  return m == Motif::Light ? kAlignLight : kAlignDark;
}

static void draw_align_icon(const Cairo::RefPtr<Cairo::Context> &cr, int w,
                            int h, AlignOp op, bool enabled, Motif motif) {
  const AlignIconPalette &pal = align_palette_for(motif);
  double fg = enabled ? pal.fg_enabled : pal.fg_disabled;
  double ref_col = pal.ref;

  cr->set_antialias(Cairo::Antialias::ANTIALIAS_DEFAULT);

  // Helper: filled rect
  auto rect = [&](double x, double y, double rw, double rh) {
    cr->rectangle(x, y, rw, rh);
    cr->fill();
  };

  switch (op) {
  case AlignOp::AlignLeft: {
    // Vertical reference line on left; two rects of different widths flush left
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.5);
    cr->move_to(5.5, 4);
    cr->line_to(5.5, h - 4);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    rect(6, 8, 16, 6);
    rect(6, 18, 10, 6);
    break;
  }
  case AlignOp::AlignCenterH: {
    // Vertical reference line in centre; rects centred on it
    double cx = w * 0.5;
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.5);
    cr->move_to(cx, 4);
    cr->line_to(cx, h - 4);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    rect(cx - 8, 8, 16, 6);
    rect(cx - 5, 18, 10, 6);
    break;
  }
  case AlignOp::AlignRight: {
    double rx = w - 5.5;
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.5);
    cr->move_to(rx, 4);
    cr->line_to(rx, h - 4);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    rect(rx - 16, 8, 16, 6);
    rect(rx - 10, 18, 10, 6);
    break;
  }
  case AlignOp::AlignTop: {
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.5);
    cr->move_to(4, 5.5);
    cr->line_to(w - 4, 5.5);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    rect(8, 6, 6, 14);
    rect(18, 6, 6, 9);
    break;
  }
  case AlignOp::AlignCenterV: {
    double cy = h * 0.5;
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.5);
    cr->move_to(4, cy);
    cr->line_to(w - 4, cy);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    rect(8, cy - 7, 6, 14);
    rect(18, cy - 4, 6, 9);
    break;
  }
  case AlignOp::AlignBottom: {
    double by = h - 5.5;
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.5);
    cr->move_to(4, by);
    cr->line_to(w - 4, by);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    rect(8, by - 14, 6, 14);
    rect(18, by - 9, 6, 9);
    break;
  }
  case AlignOp::DistributeH: {
    // Three vertical bars with equal gaps indicated by arrows/bars
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.0);
    // Left + right edge ticks
    cr->move_to(4.5, 6);
    cr->line_to(4.5, h - 6);
    cr->stroke();
    cr->move_to(w - 4.5, 6);
    cr->line_to(w - 4.5, h - 6);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    // Three equal-width rects spaced evenly
    double bw = 5, gap = (w - 8 - 3 * bw) / 2.0;
    for (int i = 0; i < 3; ++i)
      rect(4 + i * (bw + gap), 8, bw, h - 16);
    break;
  }
  case AlignOp::DistributeV: {
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.0);
    cr->move_to(6, 4.5);
    cr->line_to(w - 6, 4.5);
    cr->stroke();
    cr->move_to(6, h - 4.5);
    cr->line_to(w - 6, h - 4.5);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    double bh = 5, gap = (h - 8 - 3 * bh) / 2.0;
    for (int i = 0; i < 3; ++i)
      rect(8, 4 + i * (bh + gap), w - 16, bh);
    break;
  }
  }
}

void Toolbar::Impl::build_align_button() {
  // s175 m4: opening separator removed and trailing self->append removed.
  // Align is now part of the transforms section (alphabetical position 1);
  // build_transforms_section appends the widget at the right position
  // among its six buttons. This function still owns the widget's
  // construction, drawing, popover wiring, and signal hookup — only the
  // placement-in-the-toolbar concern moved to the caller.

  // The trigger button — s183 m1: replaced the cairo-drawn AlignCenterH
  // stand-in with the dedicated curvz-align-symbolic icon. CSS class
  // .tool-btn-disabled already drives the dim look when align_enabled
  // is false (set by set_align_enabled below), so the cairo `en`
  // parameter that previously dimmed the drawing isn't needed —
  // currentColor + class opacity handles it. set_align_enabled's
  // dynamic_cast<Gtk::DrawingArea*> on get_child() now returns null
  // and the queue_draw branch becomes a clean no-op; left in place
  // as defensive plumbing (no harm, no allocation).
  Gtk::Image *icon_img = Gtk::make_managed<Gtk::Image>();
  icon_img->set_from_icon_name("curvz-align-symbolic");
  icon_img->set_pixel_size(22);
  curvz::utils::set_name(icon_img, "tb_ab_icon", "main_toolbar_align_btn_icon");
  icon_img->set_can_target(false);
  align_btn.set_child(*icon_img);
  curvz::utils::set_name(align_btn, "tb_ab", "main_toolbar_align_btn");
  // S117 m5: was set_has_frame(true). Every other toolbar button uses
  // frame=false (logo, hamburger, zoom, fit, macro, etc.); this one
  // alone wore the system button frame chrome, which paints a grey
  // background-image that sits visibly over the toolbar band in light
  // motif. Source-side fix is cleaner than a CSS background-image
  // override for one specific class — drops the inconsistency and the
  // visual bug together.
  align_btn.set_has_frame(false);
  align_btn.set_halign(Gtk::Align::CENTER);
  // s152: CSS owns sizing — see add_tool_button comment. .tool-btn
  // class brings hover/disabled styling in line with tool buttons.
  // .tb-align-btn retained for any align-specific overrides
  // (e.g. Cairo-painted icon contrast tweaks).
  align_btn.add_css_class("tool-btn");
  align_btn.set_tooltip_text("Align & Distribute");
  // s152 sub-ship 1 fix: faked disabled. Widget stays sensitive so it
  // continues to receive right-click events (the swallow runs); the
  // .tool-btn-disabled class supplies the visual look (0.4 opacity,
  // hover suppressed). align_enabled gates the left-click action.
  // MainWindow flips both via set_align_enabled() when selection
  // changes (≥2 objects → enabled).
  align_btn.add_css_class("tool-btn-disabled");
  align_btn.add_css_class("tb-align-btn");

  build_align_popover();
  align_pop.set_parent(align_btn);

  align_btn.signal_clicked().connect([this]() {
    // s152 sub-ship 1 fix: faked disabled — gate the action here. The
    // widget stays sensitive (so right-click events reach the swallow)
    // but left-click does nothing until MainWindow has flipped the
    // bool via set_align_enabled().
    if (!align_enabled) return;
    align_pop.popup();
  });
  // s152 sub-ship 1 fix: swallow right-clicks. Align has no per-tool
  // right-click affordance; preventing density from opening here keeps
  // right-click semantics consistent across all real toolbar widgets.
  add_rclick_swallow(align_btn);
}

void Toolbar::Impl::build_align_popover() {
  curvz::utils::set_name(align_pop, "pop_tb_align", "popover_toolbar_align_root");
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(8);
  outer->set_margin_top(10);
  outer->set_margin_bottom(10);
  outer->set_margin_start(10);
  outer->set_margin_end(10);

  // ── Title ─────────────────────────────────────────────────────────────
  auto *title = Gtk::make_managed<Gtk::Label>("Align & Distribute");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // Helper: make a labelled section with a row of icon buttons
  auto make_section = [&](const std::string &label_text) -> Gtk::Box * {
    auto *vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    vbox->set_spacing(3);
    auto *lbl = Gtk::make_managed<Gtk::Label>(label_text);
    lbl->set_xalign(0.0f);
    lbl->add_css_class("tb-pop-label");
    vbox->append(*lbl);
    auto *hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    hbox->set_spacing(3);
    vbox->append(*hbox);
    outer->append(*vbox);
    return hbox;
  };

  // Helper: wrap a DrawingArea in a button that emits an AlignOp
  auto make_btn = [&](Gtk::DrawingArea &da, AlignOp op,
                      const std::string &tip) -> Gtk::Button * {
    da.set_size_request(32, 32);
    da.set_can_target(false);
    da.set_draw_func([this, op](const Cairo::RefPtr<Cairo::Context> &cr, int w,
                                int h) { draw_align_icon(cr, w, h, op, true, motif); });
    auto *btn = Gtk::make_managed<Gtk::Button>();
    btn->set_child(da);
    btn->set_has_frame(false);
    btn->add_css_class("tb-icon-btn");
    btn->set_tooltip_text(tip);
    btn->signal_clicked().connect([this, op]() {
      align_pop.popdown();
      sig_align.emit(op);
    });
    return btn;
  };

  // ── Align row (6 buttons: L CH R / T CV B) ────────────────────────────
  // We lay them out as two sub-rows for clarity
  auto *align_h_row = make_section("Align");
  Gtk::Button *al_btn;
  al_btn = make_btn(align_da[0], AlignOp::AlignLeft, "Align left edges");
  curvz::utils::set_name(al_btn, "pop_tb_align_l", "popover_toolbar_align_left_btn");
  align_h_row->append(*al_btn);
  al_btn = make_btn(align_da[1], AlignOp::AlignCenterH, "Center on vertical axis");
  curvz::utils::set_name(al_btn, "pop_tb_align_ch", "popover_toolbar_align_center_h_btn");
  align_h_row->append(*al_btn);
  al_btn = make_btn(align_da[2], AlignOp::AlignRight, "Align right edges");
  curvz::utils::set_name(al_btn, "pop_tb_align_r", "popover_toolbar_align_right_btn");
  align_h_row->append(*al_btn);
  al_btn = make_btn(align_da[3], AlignOp::AlignTop, "Align top edges");
  curvz::utils::set_name(al_btn, "pop_tb_align_t", "popover_toolbar_align_top_btn");
  align_h_row->append(*al_btn);
  al_btn = make_btn(align_da[4], AlignOp::AlignCenterV, "Center on horizontal axis");
  curvz::utils::set_name(al_btn, "pop_tb_align_cv", "popover_toolbar_align_center_v_btn");
  align_h_row->append(*al_btn);
  al_btn = make_btn(align_da[5], AlignOp::AlignBottom, "Align bottom edges");
  curvz::utils::set_name(al_btn, "pop_tb_align_b", "popover_toolbar_align_bottom_btn");
  align_h_row->append(*al_btn);

  // ── Distribute row ─────────────────────────────────────────────────────
  auto *dist_row = make_section("Distribute");
  al_btn = make_btn(align_da[6], AlignOp::DistributeH, "Distribute horizontally (equal gaps)");
  curvz::utils::set_name(al_btn, "pop_tb_align_dh", "popover_toolbar_distribute_h_btn");
  dist_row->append(*al_btn);
  al_btn = make_btn(align_da[7], AlignOp::DistributeV, "Distribute vertically (equal gaps)");
  curvz::utils::set_name(al_btn, "pop_tb_align_dv", "popover_toolbar_distribute_v_btn");
  dist_row->append(*al_btn);

  align_pop.set_child(*outer);
  align_pop.set_has_arrow(true);
  align_pop.set_position(Gtk::PositionType::RIGHT);
}

void Toolbar::Impl::set_align_enabled(bool enabled) {
  // s152 sub-ship 1 fix: faked-disabled. The widget stays sensitive
  // so right-click events keep flowing (the swallow runs); the
  // .tool-btn-disabled class drives the visual look; align_enabled
  // gates the left-click action.
  align_enabled = enabled;
  if (enabled) {
    align_btn.remove_css_class("tool-btn-disabled");
  } else {
    align_btn.add_css_class("tool-btn-disabled");
  }
  // Redraw the trigger icon so it reflects the enabled state
  if (align_btn.get_child()) {
    if (auto *da = dynamic_cast<Gtk::DrawingArea *>(align_btn.get_child()))
      da->queue_draw();
  }
}

// ── s154 m1: Transforms section ─────────────────────────────────────────
//
// Layout (top to bottom): SnR, Blend, Bool, Warp, then a closing
// separator. The opening separator (sep2) is already emitted by
// Impl::build() right above the call to build_transforms_section.
//
// Buttons are styled as plain icon buttons matching the tool-button
// idiom: .tool-btn class, frame=false, .tool-btn-disabled when their
// per-button enabled flag is false. Left-click runs the operation
// (M1: just emits the corresponding signal — MainWindow routes it
// to the existing Path-menu handler). Right-click pops a small
// configuration popover; in M1 these are placeholders, M2/M3/M4 will
// fill them with the migrated dialog content per op.
//
// Bool button is the odd one out: left-click pops a horizontal popover
// of three icon buttons (Union/Subtract/Intersect, tooltip-only). No
// right-click affordance — the left-click popover is the configuration
// surface.
//
// Faked-disabled mirrors align: the widgets stay sensitive so the
// right-click gestures keep firing even when the operation itself is
// gated off; the *_enabled bool gates the actual left-click action.
void Toolbar::Impl::build_transforms_section() {
  // Helper: construct one transform-verb button with shared chrome.
  auto make_xform_btn = [&](Gtk::Button& btn, const char* icon_name,
                            const char* tooltip,
                            const char* name_short,
                            const char* name_long) {
    btn.set_icon_name(icon_name);
    btn.set_has_frame(false);
    btn.set_halign(Gtk::Align::CENTER);
    btn.set_tooltip_text(tooltip);
    btn.add_css_class("tool-btn");
    // Default state is disabled; MainWindow flips via
    // set_transforms_enabled() once selection state is known.
    btn.add_css_class("tool-btn-disabled");
    curvz::utils::set_name(btn, name_short, name_long);
  };

  make_xform_btn(snr_btn, "curvz-step-repeat-symbolic",
                 "Step and Repeat — Right-click to configure",
                 "tb_snr", "main_toolbar_step_repeat_btn");
  make_xform_btn(blend_btn, "curvz-blend-symbolic",
                 "Blend — Right-click to configure",
                 "tb_bln", "main_toolbar_blend_btn");
  make_xform_btn(bool_btn, "curvz-union-symbolic",
                 "Boolean operators: Union, Subtraction, and Intersection",
                 "tb_bop", "main_toolbar_bool_btn");
  make_xform_btn(warp_btn, "curvz-warp-symbolic",
                 "Warp — Right-click to configure",
                 "tb_wrp", "main_toolbar_warp_btn");

  // s154 m4a: all four transform-verb buttons now route their
  // configuration popovers through MainWindow signals (no Toolbar-owned
  // placeholders left). The build_placeholder_cfg_popover helper has
  // been retired with its last user.

  // ── Bool button: build the left-click op popover (3 icon buttons)
  build_bool_popover();
  bool_pop.set_parent(bool_btn);

  // ── Click wiring ─────────────────────────────────────────────────────
  // Left-click: gate on per-button enabled flag, then fire the signal.
  snr_btn.signal_clicked().connect([this]() {
    if (!snr_enabled) return;
    sig_step_repeat.emit();
  });
  blend_btn.signal_clicked().connect([this]() {
    if (!blend_enabled) return;
    sig_blend.emit();
  });
  warp_btn.signal_clicked().connect([this]() {
    if (!warp_enabled) return;
    sig_warp.emit();
  });
  // Bool: left-click always pops the popover when enabled. The popover
  // itself emits sig_bool_op when the user picks an op inside it.
  // s154 m3 follow-up: clear bool_picker_pre_set on left-click so a
  // subsequent picker pick fires the op normally — even if a previous
  // right-click left the flag set without consuming it (e.g. user
  // dismissed the popover via outside-click).
  bool_btn.signal_clicked().connect([this]() {
    if (!bool_enabled) return;
    bool_picker_pre_set = false;
    bool_pop.popup();
  });

  // Right-click: opens the configuration popover. Always live, even
  // when the per-button enabled flag is false — configuration is
  // legitimate pre-selection.
  // s154 m2: SnR right-click emits sig_step_repeat_configure so
  //   MainWindow can summon the real StepRepeatPopover (which needs
  //   canvas wiring — pivot crosshair, preview state).
  // s154 m3: Blend right-click emits sig_blend_configure so MainWindow
  //   can summon BlendPopover (which needs the current selection's
  //   node counts + stroke widths to seed the form correctly).
  // Warp keeps the placeholder pattern until M4.
  // Bool's left-click popover already serves as its config surface, so
  // its right-click is swallowed to keep right-click semantics
  // consistent.
  {
    auto rc = Gtk::GestureClick::create();
    rc->set_button(3);
    auto* rc_raw = rc.get();
    rc->signal_pressed().connect(
        [this, rc_raw](int, double, double) {
          rc_raw->set_state(Gtk::EventSequenceState::CLAIMED);
          sig_step_repeat_configure.emit();
        });
    snr_btn.add_controller(rc);
  }
  {
    auto rc = Gtk::GestureClick::create();
    rc->set_button(3);
    auto* rc_raw = rc.get();
    rc->signal_pressed().connect(
        [this, rc_raw](int, double, double) {
          rc_raw->set_state(Gtk::EventSequenceState::CLAIMED);
          sig_blend_configure.emit();
        });
    blend_btn.add_controller(rc);
  }
  // s154 m4a: warp right-click — emits sig_warp_configure so MainWindow
  // can summon WarpPopover. Bypasses warp_enabled (configure access is
  // legitimate pre-selection — same shape as SnR/Blend right-click).
  {
    auto rc = Gtk::GestureClick::create();
    rc->set_button(3);
    auto* rc_raw = rc.get();
    rc->signal_pressed().connect(
        [this, rc_raw](int, double, double) {
          rc_raw->set_state(Gtk::EventSequenceState::CLAIMED);
          sig_warp_configure.emit();
        });
    warp_btn.add_controller(rc);
  }
  // s154 m3 follow-up: bool_btn right-click pops the SAME picker the
  // left-click does, but sets bool_picker_pre_set so the picker click
  // only updates the toolbar icon (no sig_bool_op emission). Lets the
  // user pre-select a bool op before a valid selection exists. Bypasses
  // bool_enabled — same shape as SnR/Blend right-click bypassing their
  // *_enabled flags for configure access.
  {
    auto rc = Gtk::GestureClick::create();
    rc->set_button(3);
    auto* rc_raw = rc.get();
    rc->signal_pressed().connect(
        [this, rc_raw](int, double, double) {
          rc_raw->set_state(Gtk::EventSequenceState::CLAIMED);
          bool_picker_pre_set = true;
          bool_pop.popup();
        });
    bool_btn.add_controller(rc);
  }

  // ── Corner (s175 m4) — folded into the Transforms section ──────────
  // Corner is a tool toggle (it switches the active tool to Corner
  // mode for rubber-band-selecting nodes to corner-treat), unlike the
  // other transforms-section buttons which are operation triggers.
  // Visually it lives here because conceptually it modifies geometry
  // in place — same family as Warp. Structural unification (making
  // Corner an operation trigger like SnR/Blend/Bool/Warp/Align) is its
  // own future ship; for now it lives in this section as a tool toggle.
  //
  // make_tool_button registers Corner in the tool-buttons vector
  // (so cycle_tool and select_tool work) but does NOT append — this
  // function controls the section's append order alphabetically.
  corner_tool_btn = make_tool_button("curvz-corner-symbolic",
                                     "Corner Treatment (K)",
                                     ActiveTool::Corner);
  curvz::utils::set_name(corner_tool_btn, "tb_cor",
                         "main_toolbar_corner_tool_btn");
  add_rclick_swallow(*corner_tool_btn);

  // ── Align (s175 m4) — folded into the Transforms section ───────────
  // build_align_button() constructs the widget, the Cairo-drawn icon,
  // the popover, and the click wiring — but no longer appends. This
  // function appends align_btn at alphabetical position 1.
  build_align_button();

  // ── Append in alphabetical order ───────────────────────────────────
  // s175 m4: was SnR, Blend, Bool, Warp. Now Align, Blend, Bool,
  // Corner, SnR, Warp — alphabetical sort of the section's six
  // members. Mixed widget types (toggle for Corner, plain Button for
  // the other five) — append order is what determines visual order in
  // the box, so this list IS the user-visible toolbar order.
  self->append(align_btn);
  self->append(blend_btn);
  self->append(bool_btn);
  self->append(*corner_tool_btn);
  self->append(snr_btn);
  self->append(warp_btn);

  // Closing separator — visually closes the section before the
  // utility cluster (Eyedropper, Measure, Zoom) begins.
  auto* sep_close = Gtk::make_managed<Gtk::Separator>(
      Gtk::Orientation::HORIZONTAL);
  sep_close->set_margin_top(4);
  sep_close->set_margin_bottom(4);
  self->append(*sep_close);
}

void Toolbar::Impl::build_bool_popover() {
  curvz::utils::set_name(bool_pop, "pop_tb_bop",
                         "popover_toolbar_bool_root");
  bool_pop.set_position(Gtk::PositionType::RIGHT);

  auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(6);
  outer->set_margin_top(8);
  outer->set_margin_bottom(8);
  outer->set_margin_start(8);
  outer->set_margin_end(8);

  auto* title = Gtk::make_managed<Gtk::Label>("Boolean Operations");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // Horizontal row of three icon buttons. Tooltip carries the op name;
  // selection emits sig_bool_op(BoolOp), dismisses the popover so the
  // op fires immediately on canvas, AND updates the toolbar button's
  // icon to match the picked op (s154 m3 follow-up). The visible
  // bool_btn icon thus reflects "last picked op" — pure visual cue,
  // does NOT change left-click behaviour (left-click still pops the
  // picker; users can change op by re-picking).
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  row->set_spacing(4);

  auto add_op = [&](const char* icon_name, const char* tip,
                    BoolOp op, const char* name_short,
                    const char* name_long) {
    auto* b = Gtk::make_managed<Gtk::Button>();
    b->set_icon_name(icon_name);
    b->set_has_frame(false);
    b->add_css_class("tool-btn");
    b->set_tooltip_text(tip);
    curvz::utils::set_name(b, name_short, name_long);
    b->signal_clicked().connect([this, op, icon_name]() {
      bool_pop.popdown();
      bool_btn.set_icon_name(icon_name);
      // s154 m3 follow-up: right-click pre-set path skips emission so
      // the user can pick an op before a valid selection exists. The
      // flag is consumed here and reset for the next interaction.
      const bool pre_set = bool_picker_pre_set;
      bool_picker_pre_set = false;
      if (!pre_set) sig_bool_op.emit(op);
    });
    row->append(*b);
  };

  add_op("curvz-union-symbolic",     "Union",     BoolOp::Union,
         "tb_bop_un", "popover_bool_union_btn");
  add_op("curvz-subtract-symbolic",  "Subtract",  BoolOp::Subtract,
         "tb_bop_sb", "popover_bool_subtract_btn");
  add_op("curvz-intersect-symbolic", "Intersect", BoolOp::Intersect,
         "tb_bop_in", "popover_bool_intersect_btn");

  outer->append(*row);
  bool_pop.set_child(*outer);
}

void Toolbar::Impl::set_transforms_enabled(bool snr, bool blend,
                                           bool boolop, bool warp) {
  // Faked-disabled mirrors set_align_enabled. Widgets stay sensitive
  // so right-click events keep reaching the configuration popovers
  // even when the operation is gated off; the per-button bool gates
  // left-click, and the .tool-btn-disabled class drives the visuals.
  auto apply = [](Gtk::Button& btn, bool& flag, bool en) {
    flag = en;
    if (en) btn.remove_css_class("tool-btn-disabled");
    else    btn.add_css_class("tool-btn-disabled");
  };
  apply(snr_btn,   snr_enabled,   snr);
  apply(blend_btn, blend_enabled, blend);
  apply(bool_btn,  bool_enabled,  boolop);
  apply(warp_btn,  warp_enabled,  warp);
}

void Toolbar::Impl::build_zoom_popover(Gtk::ToggleButton *zoom_btn) {
  // ── Right-click on zoom tool button opens popover ─────────────────────
  // S89: was Ctrl+left-click; now plain right-click for world consistency.
  // s152 sub-ship 1: claim on press so right-click doesn't bubble to
  // the toolbar root's density picker.
  auto rclick = Gtk::GestureClick::create();
  rclick->set_button(3);
  auto* rclick_raw = rclick.get();
  rclick->signal_pressed().connect([this, rclick_raw](int, double, double) {
    rclick_raw->set_state(Gtk::EventSequenceState::CLAIMED);
    if (zoom_adj)
      zoom_adj->set_value(zoom_level * 100.0);
    zoom_pop.popup();
  });
  zoom_btn->add_controller(rclick);

  // ── Popover content ───────────────────────────────────────────────────
  curvz::utils::set_name(zoom_pop, "pop_tb_zom", "popover_toolbar_zoom_root");
  zoom_pop.set_parent(*zoom_btn);
  zoom_pop.set_position(Gtk::PositionType::RIGHT);

  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(10);
  outer->set_margin_top(12);
  outer->set_margin_bottom(12);
  outer->set_margin_start(14);
  outer->set_margin_end(14);

  // Title
  auto *title = Gtk::make_managed<Gtk::Label>("Zoom Level");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // Spin button row
  auto *spin_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  spin_row->set_spacing(8);
  auto *spin_lbl = Gtk::make_managed<Gtk::Label>("Zoom:");
  spin_lbl->add_css_class("tb-pop-label");
  spin_lbl->set_xalign(0.0f);

  zoom_adj = Gtk::Adjustment::create(100.0, 1.0, 9500.0, 1.0, 10.0);
  curvz::utils::set_name(zoom_spin, "pop_tb_zom_spn", "popover_toolbar_zoom_pct_spn");
  zoom_spin.set_adjustment(zoom_adj);
  zoom_spin.set_digits(0);
  zoom_spin.set_numeric(true);
  zoom_spin.set_hexpand(true);
  zoom_spin.set_width_chars(6);

  auto *pct_lbl = Gtk::make_managed<Gtk::Label>("%");
  pct_lbl->add_css_class("tb-pop-label");

  spin_row->append(*spin_lbl);
  spin_row->append(zoom_spin);
  spin_row->append(*pct_lbl);
  outer->append(*spin_row);

  // Preset buttons
  auto *preset_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  preset_row->set_spacing(4);
  preset_row->set_homogeneous(true);

  // Five preset buttons — unrolled from the previous loop so each call
  // to set_name carries literal string args, which is what
  // widget_names_sync's harvester regex requires (it captures quoted
  // string literals, not constructed std::string c_str pointers). One
  // local helper keeps the body compact without re-introducing the
  // dynamic-string problem.
  auto add_zoom_preset = [&](int pct, Gtk::Button* btn) {
    btn->add_css_class("tb-type-btn");
    btn->signal_clicked().connect([this, pct]() {
      if (zoom_adj)
        zoom_adj->set_value(pct);
    });
    preset_row->append(*btn);
  };
  {
    auto *btn = Gtk::make_managed<Gtk::Button>("25%");
    curvz::utils::set_name(btn, "pop_tb_zom_p25", "popover_toolbar_zoom_preset_25pct_btn");
    add_zoom_preset(25, btn);
  }
  {
    auto *btn = Gtk::make_managed<Gtk::Button>("50%");
    curvz::utils::set_name(btn, "pop_tb_zom_p50", "popover_toolbar_zoom_preset_50pct_btn");
    add_zoom_preset(50, btn);
  }
  {
    auto *btn = Gtk::make_managed<Gtk::Button>("100%");
    curvz::utils::set_name(btn, "pop_tb_zom_p100", "popover_toolbar_zoom_preset_100pct_btn");
    add_zoom_preset(100, btn);
  }
  {
    auto *btn = Gtk::make_managed<Gtk::Button>("200%");
    curvz::utils::set_name(btn, "pop_tb_zom_p200", "popover_toolbar_zoom_preset_200pct_btn");
    add_zoom_preset(200, btn);
  }
  {
    auto *btn = Gtk::make_managed<Gtk::Button>("400%");
    curvz::utils::set_name(btn, "pop_tb_zom_p400", "popover_toolbar_zoom_preset_400pct_btn");
    add_zoom_preset(400, btn);
  }
  outer->append(*preset_row);

  // Separator
  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  sep->set_margin_top(2);
  outer->append(*sep);

  // Button row: Fit | Apply
  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_spacing(8);
  btn_row->set_halign(Gtk::Align::END);

  auto *fit_btn = Gtk::make_managed<Gtk::Button>("Fit");
  curvz::utils::set_name(fit_btn, "pop_tb_zom_fit", "popover_toolbar_zoom_fit_btn");
  fit_btn->signal_clicked().connect([this]() {
    zoom_pop.popdown();
    sig_fit.emit();
    sig_canvas_focus.emit();
  });

  auto *apply_btn = Gtk::make_managed<Gtk::Button>("Apply");
  curvz::utils::set_name(apply_btn, "pop_tb_zom_apl", "popover_toolbar_zoom_apply_btn");
  apply_btn->add_css_class("suggested-action");
  apply_btn->signal_clicked().connect([this]() {
    double pct = zoom_adj ? zoom_adj->get_value() : 100.0;
    double target = pct / 100.0; // raw zoom factor: 100% → 1.0, 50% → 0.5
    if (target > 0.0)
      sig_zoom_to.emit(
          target); // MainWindow calls zoom_toward with exact factor
    zoom_pop.popdown();
    sig_canvas_focus.emit();
  });

  btn_row->append(*fit_btn);
  btn_row->append(*apply_btn);
  outer->append(*btn_row);

  zoom_pop.set_child(*outer);
}

// ── Impl::set_zoom — host-driven zoom level sync ──────────────────────────
void Toolbar::Impl::set_zoom(double zoom_rel) {
  zoom_level = zoom_rel;
  // Keep popover spin in sync so it shows current value when opened
  if (zoom_adj)
    zoom_adj->set_value(std::round(zoom_rel * 100.0));
}

// ── s150 — Measure tool right-click popover ─────────────────────────────
// Houses the two measure-behaviour settings (save_to_layer,
// destruct_after_copy) that used to live in the Inspector ▸ Document ▸
// Measure section. The section was deleted in s150 per ARC.md design
// rule 1: behaviour at the tool, not in the inspector.
//
// State source: m_doc (set via set_document). On popover open, checkbox
// states are synced from doc->measure_*. On toggle, writes go directly
// to doc->measure_*; signal_measure_settings_changed fires so MainWindow
// can schedule_save. No project-level mirror — measure prefs are per-doc.
//
// Mirrors the snap popover shape: open syncs from doc, toggles write
// back through. Mirrors the deleted Inspector Measure section's
// interaction: the two checkboxes have a sensitivity coupling
// (delete-on-copy is greyed out when save-to-layer is on).
//
// s153 sub-ship 2c: moved into Impl. m_doc and the signal still live
// on the public Toolbar; reach via self->.
void Toolbar::Impl::build_measure_popover(Gtk::ToggleButton *measure_btn) {
  // ── Right-click on Measure tool button opens popover ──────────────────
  // s152 sub-ship 1: claim on press so the right-click doesn't bubble
  // to the toolbar root's density picker.
  auto rclick = Gtk::GestureClick::create();
  rclick->set_button(3);
  auto* rclick_raw = rclick.get();
  rclick->signal_pressed().connect([this, rclick_raw](int, double, double) {
    rclick_raw->set_state(Gtk::EventSequenceState::CLAIMED);
    // Sync checkboxes from current doc state on every open. Same pattern
    // as the snap popover (signal_snap_pop_open in the inspector path).
    if (doc && measure_save_chk && measure_del_chk && measure_del_lbl) {
      measure_save_chk->set_active(doc->measure_save_to_layer);
      measure_del_chk->set_active(doc->measure_destruct_after_copy);
      measure_del_chk->set_sensitive(!doc->measure_save_to_layer);
      if (doc->measure_save_to_layer)
        measure_del_lbl->add_css_class("dim-label");
      else
        measure_del_lbl->remove_css_class("dim-label");
    }
    measure_pop.popup();
  });
  measure_btn->add_controller(rclick);

  // ── Popover content ───────────────────────────────────────────────────
  curvz::utils::set_name(measure_pop, "pop_tb_meas", "popover_toolbar_measure_root");
  measure_pop.set_parent(*measure_btn);
  measure_pop.set_position(Gtk::PositionType::RIGHT);

  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(8);
  outer->set_margin_top(12);
  outer->set_margin_bottom(12);
  outer->set_margin_start(14);
  outer->set_margin_end(14);

  // Title
  auto *title = Gtk::make_managed<Gtk::Label>("Measure");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // Helper — make a [checkbox][label] row with consistent spacing.
  // Mirrors the deleted build_measure_section's row helper.
  auto make_chk_row = [](Gtk::CheckButton *chk, Gtk::Label *lbl) -> Gtk::Box * {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(6);
    row->set_margin_top(2);
    row->set_margin_bottom(2);
    row->append(*chk);
    row->append(*lbl);
    return row;
  };

  // ── "Save measurements" checkbox row ────────────────────────────────────
  measure_save_chk = Gtk::make_managed<Gtk::CheckButton>();
  curvz::utils::set_name(measure_save_chk, "pop_tb_meas_sv",
                         "popover_toolbar_measure_save_check");
  measure_save_chk->set_tooltip_text(
      "When enabled, every completed measurement is appended to the "
      "Measurements layer and persists in the document.");
  auto *save_lbl = Gtk::make_managed<Gtk::Label>("Save measurements");
  save_lbl->set_xalign(0.0f);
  save_lbl->set_hexpand(true);
  save_lbl->add_css_class("tb-pop-label");
  outer->append(*make_chk_row(measure_save_chk, save_lbl));

  // ── "Delete on copy" checkbox row ───────────────────────────────────────
  measure_del_chk = Gtk::make_managed<Gtk::CheckButton>();
  curvz::utils::set_name(measure_del_chk, "pop_tb_meas_dc",
                         "popover_toolbar_measure_destruct_check");
  measure_del_chk->set_tooltip_text(
      "When enabled, copying a transient measurement label dismisses it "
      "from the canvas. Only applies when 'Save measurements' is off — "
      "saved entries are permanent.");
  measure_del_lbl = Gtk::make_managed<Gtk::Label>("Delete on copy");
  measure_del_lbl->set_xalign(0.0f);
  measure_del_lbl->set_hexpand(true);
  measure_del_lbl->add_css_class("tb-pop-label");
  outer->append(*make_chk_row(measure_del_chk, measure_del_lbl));

  // ── Save toggle handler ─────────────────────────────────────────────────
  // Writes through to doc, updates Delete-on-copy sensitivity inline,
  // emits signal_measure_settings_changed for MainWindow to schedule_save.
  measure_save_chk->signal_toggled().connect([this]() {
    if (!doc) return;
    bool on = measure_save_chk->get_active();
    doc->measure_save_to_layer = on;
    if (measure_del_chk) measure_del_chk->set_sensitive(!on);
    if (measure_del_lbl) {
      if (on) measure_del_lbl->add_css_class("dim-label");
      else    measure_del_lbl->remove_css_class("dim-label");
    }
    sig_measure_settings.emit();
  });

  measure_del_chk->signal_toggled().connect([this]() {
    if (!doc) return;
    doc->measure_destruct_after_copy = measure_del_chk->get_active();
    sig_measure_settings.emit();
  });

  measure_pop.set_child(*outer);
}

// s153 sub-ship 2c: moved into Impl. m_doc, m_popup_unit, and the
// place-ref signal still live on the public Toolbar; reach via self->.
void Toolbar::Impl::build_ref_popover(Gtk::ToggleButton *ref_btn) {
  // ── Right-click on ref tool button opens placement popover ────────────
  // S89: was Ctrl+left-click; converted to plain right-click.
  // s152 sub-ship 1: claim on press so the right-click doesn't bubble
  // to the toolbar root's density picker.
  auto rclick = Gtk::GestureClick::create();
  rclick->set_button(3);
  auto* rclick_raw = rclick.get();
  rclick->signal_pressed().connect(
      [this, rclick_raw](int, double, double) {
        rclick_raw->set_state(Gtk::EventSequenceState::CLAIMED);
        Unit u = doc ? (doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(doc->canvas.phys_unit) : doc->canvas.display_unit) : popup_unit;
        auto [cw, ch] = canvas_display_size(doc);
        if (ref_unit_lbl)
          ref_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
        if (ref_adj_x)
          ref_adj_x->set_value(cw * 0.5);
        if (ref_adj_y)
          ref_adj_y->set_value(ch * 0.5);
        ref_pop.popup();
      });
  ref_btn->add_controller(rclick);

  curvz::utils::set_name(ref_pop, "pop_tb_ref", "popover_toolbar_ref_root");
  ref_pop.set_parent(*ref_btn);
  ref_pop.set_position(Gtk::PositionType::RIGHT);

  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(10);
  outer->set_margin_top(12);
  outer->set_margin_bottom(12);
  outer->set_margin_start(14);
  outer->set_margin_end(14);

  // Title
  auto *title = Gtk::make_managed<Gtk::Label>("Place Reference Point");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // X row
  auto *x_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  x_row->set_spacing(8);
  auto *x_lbl = Gtk::make_managed<Gtk::Label>("X:");
  x_lbl->add_css_class("tb-pop-label");
  x_lbl->set_xalign(0.0f);
  x_lbl->set_width_chars(3);

  ref_adj_x = Gtk::Adjustment::create(0.0, -1e6, 1e6, 0.000001, 10.0);
  auto *x_spin = Gtk::make_managed<Gtk::SpinButton>(ref_adj_x, 0.000001, 6);
  curvz::utils::set_name(x_spin, "pop_tb_ref_x", "popover_toolbar_ref_x_spn");
  x_spin->set_hexpand(true);
  x_spin->set_width_chars(12);
  x_spin->set_numeric(true);

  x_row->append(*x_lbl);
  x_row->append(*x_spin);
  outer->append(*x_row);

  // Y row
  auto *y_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  y_row->set_spacing(8);
  auto *y_lbl = Gtk::make_managed<Gtk::Label>("Y:");
  y_lbl->add_css_class("tb-pop-label");
  y_lbl->set_xalign(0.0f);
  y_lbl->set_width_chars(3);

  ref_adj_y = Gtk::Adjustment::create(0.0, -1e6, 1e6, 0.000001, 10.0);
  auto *y_spin = Gtk::make_managed<Gtk::SpinButton>(ref_adj_y, 0.000001, 6);
  curvz::utils::set_name(y_spin, "pop_tb_ref_y", "popover_toolbar_ref_y_spn");
  y_spin->set_hexpand(true);
  y_spin->set_width_chars(12);
  y_spin->set_numeric(true);

  y_row->append(*y_lbl);
  y_row->append(*y_spin);
  outer->append(*y_row);

  ref_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  ref_unit_lbl->set_xalign(0.0f);
  ref_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*ref_unit_lbl);

  // Place button
  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_spacing(6);
  btn_row->set_halign(Gtk::Align::END);

  auto *place_btn = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place_btn, "pop_tb_ref_pl", "popover_toolbar_ref_place_btn");
  place_btn->add_css_class("suggested-action");
  place_btn->signal_clicked().connect([this]() {
    if (ref_adj_x && ref_adj_y)
      sig_place_ref.emit(ref_adj_x->get_value(), ref_adj_y->get_value());
    ref_pop.popdown();
    sig_canvas_focus.emit();
  });

  btn_row->append(*place_btn);
  outer->append(*btn_row);

  ref_pop.set_child(*outer);
}

// ── Shared helper: build a right-click popover with N spin rows + Place button
// S89: was Ctrl+left-click, converted to plain right-click to match world UI
// convention (right-click for context/configuration menus). Tool buttons
// remain ToggleButtons whose left-click toggles the active tool — right-click
// is a separate gesture stream so there's no conflict.
//
// s152 sub-ship 1: claim on press so the right-click doesn't bubble to
// the toolbar root's density picker. Without the claim, both this
// per-tool popover AND density would open on a single right-click.
// Single fix here covers all six placement popovers (rect, ellipse,
// line, text, polygon, spiral) that route through this helper.
static void setup_rclick_popover(Gtk::ToggleButton *btn, Gtk::Popover &pop,
                                 std::function<void()> on_open) {
  auto rclick = Gtk::GestureClick::create();
  rclick->set_button(3); // right mouse button
  auto* rclick_raw = rclick.get();
  rclick->signal_pressed().connect(
      [&pop, on_open, rclick_raw](int, double, double) {
        rclick_raw->set_state(Gtk::EventSequenceState::CLAIMED);
        if (on_open)
          on_open();
        pop.popup();
      });
  btn->add_controller(rclick);
  pop.set_parent(*btn);
  pop.set_position(Gtk::PositionType::RIGHT);
}

static Gtk::Box *make_pop_outer(const char *title) {
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(8);
  outer->set_margin_top(12);
  outer->set_margin_bottom(12);
  outer->set_margin_start(14);
  outer->set_margin_end(14);
  auto *lbl = Gtk::make_managed<Gtk::Label>(title);
  lbl->add_css_class("tb-pop-title");
  lbl->set_xalign(0.0f);
  outer->append(*lbl);
  return outer;
}

// Helper: build a "[Label:] [SpinButton]" row inside a popover and append
// it to outer. Returns the Adjustment AND writes the SpinButton pointer
// out via the optional out_spin parameter — callers that want to
// curvz::utils::set_name() the spin pass &spin_ptr; pure-Adjustment
// callers pass nullptr.
//
// s119: the out-pointer pattern lets every call site name its spin with
// literal-string args (which is what widget_names_sync's harvester regex
// requires). An earlier attempt threaded abbrev/long_name through the
// helper and called set_name inside, but that hides the literals from
// the harvester — the set_name body sees only variable names, not the
// caller's strings.
static Glib::RefPtr<Gtk::Adjustment> make_pop_row(Gtk::Box *outer,
                                                  const char *label,
                                                  Gtk::SpinButton **out_spin = nullptr) {
  auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  row->set_spacing(8);
  auto *lbl = Gtk::make_managed<Gtk::Label>(label);
  lbl->add_css_class("tb-pop-label");
  lbl->set_xalign(0.0f);
  lbl->set_width_chars(4);
  row->append(*lbl);
  auto adj = Gtk::Adjustment::create(0.0, -1e6, 1e6, 0.000001, 10.0);
  auto *spin = Gtk::make_managed<Gtk::SpinButton>(adj, 0.000001, 6);
  spin->set_hexpand(true);
  spin->set_width_chars(10);
  spin->set_numeric(true);
  row->append(*spin);
  outer->append(*row);
  if (out_spin) *out_spin = spin;
  return adj;
}

// ── Rect popover
// ──────────────────────────────────────────────────────────────
// s153 sub-ship 2b: moved into Impl. State (rect_pop, rect_adj_*,
// rect_unit_lbl) is unprefixed and lives on Impl. Signal emits and
// m_doc / m_popup_unit reach back via self-> until 2e cleanup.
void Toolbar::Impl::build_rect_popover(Gtk::ToggleButton *btn) {
  curvz::utils::set_name(rect_pop, "pop_tb_rec", "popover_toolbar_rect_root");
  setup_rclick_popover(btn, rect_pop, [this]() {
    Unit u = doc ? (doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(doc->canvas.phys_unit) : doc->canvas.display_unit) : popup_unit;
    auto [cw, ch] = canvas_display_size(doc);
    if (rect_unit_lbl)
      rect_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    if (rect_adj_x) {
      double sz_w = cw * 0.2;
      double sz_h = ch * 0.2;
      rect_adj_x->set_value(cw * 0.5 - sz_w * 0.5);
      rect_adj_y->set_value(ch * 0.5 - sz_h * 0.5);
      rect_adj_w->set_value(sz_w);
      rect_adj_h->set_value(sz_h);
    }
  });
  auto *outer = make_pop_outer("Place Rectangle");
  Gtk::SpinButton *rec_x_spin = nullptr, *rec_y_spin = nullptr,
                  *rec_w_spin = nullptr, *rec_h_spin = nullptr;
  rect_adj_x = make_pop_row(outer, "X:", &rec_x_spin);
  curvz::utils::set_name(rec_x_spin, "pop_tb_rec_x", "popover_toolbar_rect_x_spn");
  rect_adj_y = make_pop_row(outer, "Y:", &rec_y_spin);
  curvz::utils::set_name(rec_y_spin, "pop_tb_rec_y", "popover_toolbar_rect_y_spn");
  rect_adj_w = make_pop_row(outer, "W:", &rec_w_spin);
  curvz::utils::set_name(rec_w_spin, "pop_tb_rec_w", "popover_toolbar_rect_w_spn");
  rect_adj_h = make_pop_row(outer, "H:", &rec_h_spin);
  curvz::utils::set_name(rec_h_spin, "pop_tb_rec_h", "popover_toolbar_rect_h_spn");
  rect_adj_w->set_lower(0.000001);
  rect_adj_h->set_lower(0.000001);
  rect_adj_w->set_value(100);
  rect_adj_h->set_value(100);

  rect_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  rect_unit_lbl->set_xalign(0.0f);
  rect_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*rect_unit_lbl);

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_halign(Gtk::Align::END);
  auto *place = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place, "pop_tb_rec_pl", "popover_toolbar_rect_place_btn");
  place->add_css_class("suggested-action");
  place->signal_clicked().connect([this]() {
    if (rect_adj_x)
      sig_place_rect.emit(
          rect_adj_x->get_value(), rect_adj_y->get_value(),
          rect_adj_w->get_value(), rect_adj_h->get_value());
    rect_pop.popdown();
    sig_canvas_focus.emit();
  });
  btn_row->append(*place);
  outer->append(*btn_row);
  rect_pop.set_child(*outer);
}

// ── Ellipse popover
// ───────────────────────────────────────────────────────────
// s153 sub-ship 2b: moved into Impl alongside rect.
void Toolbar::Impl::build_ellipse_popover(Gtk::ToggleButton *btn) {
  curvz::utils::set_name(ellipse_pop, "pop_tb_ova", "popover_toolbar_ellipse_root");
  setup_rclick_popover(btn, ellipse_pop, [this]() {
    Unit u = doc ? (doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(doc->canvas.phys_unit) : doc->canvas.display_unit) : popup_unit;
    auto [cw, ch] = canvas_display_size(doc);
    if (ellipse_unit_lbl)
      ellipse_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    if (ellipse_adj_x) {
      double sz_w = cw * 0.2;
      double sz_h = ch * 0.2;
      ellipse_adj_x->set_value(cw * 0.5 - sz_w * 0.5);
      ellipse_adj_y->set_value(ch * 0.5 - sz_h * 0.5);
      ellipse_adj_w->set_value(sz_w);
      ellipse_adj_h->set_value(sz_h);
    }
  });
  auto *outer = make_pop_outer("Place Ellipse");
  Gtk::SpinButton *ova_x_spin = nullptr, *ova_y_spin = nullptr,
                  *ova_w_spin = nullptr, *ova_h_spin = nullptr;
  ellipse_adj_x = make_pop_row(outer, "X:", &ova_x_spin);
  curvz::utils::set_name(ova_x_spin, "pop_tb_ova_x", "popover_toolbar_ellipse_x_spn");
  ellipse_adj_y = make_pop_row(outer, "Y:", &ova_y_spin);
  curvz::utils::set_name(ova_y_spin, "pop_tb_ova_y", "popover_toolbar_ellipse_y_spn");
  ellipse_adj_w = make_pop_row(outer, "W:", &ova_w_spin);
  curvz::utils::set_name(ova_w_spin, "pop_tb_ova_w", "popover_toolbar_ellipse_w_spn");
  ellipse_adj_h = make_pop_row(outer, "H:", &ova_h_spin);
  curvz::utils::set_name(ova_h_spin, "pop_tb_ova_h", "popover_toolbar_ellipse_h_spn");
  ellipse_adj_w->set_lower(0.000001);
  ellipse_adj_h->set_lower(0.000001);
  ellipse_adj_w->set_value(100);
  ellipse_adj_h->set_value(100);

  ellipse_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  ellipse_unit_lbl->set_xalign(0.0f);
  ellipse_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*ellipse_unit_lbl);

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_halign(Gtk::Align::END);
  auto *place = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place, "pop_tb_ova_pl", "popover_toolbar_ellipse_place_btn");
  place->add_css_class("suggested-action");
  place->signal_clicked().connect([this]() {
    if (ellipse_adj_x) {
      double x = ellipse_adj_x->get_value();
      double y = ellipse_adj_y->get_value();
      double w = ellipse_adj_w->get_value();
      double h = ellipse_adj_h->get_value();
      double cx = x + w * 0.5;
      double cy = y + h * 0.5;
      double rx = w * 0.5;
      double ry = h * 0.5;
      sig_place_ellipse.emit(cx, cy, rx, ry);
    }
    ellipse_pop.popdown();
    sig_canvas_focus.emit();
  });
  btn_row->append(*place);
  outer->append(*btn_row);
  ellipse_pop.set_child(*outer);
}

// ── Line popover
// ──────────────────────────────────────────────────────────────
// s153 sub-ship 2b: moved into Impl alongside rect/ellipse.
void Toolbar::Impl::build_line_popover(Gtk::ToggleButton *btn) {
  curvz::utils::set_name(line_pop, "pop_tb_lin", "popover_toolbar_line_root");
  setup_rclick_popover(btn, line_pop, [this]() {
    Unit u = doc ? (doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(doc->canvas.phys_unit) : doc->canvas.display_unit) : popup_unit;
    auto [cw, ch] = canvas_display_size(doc);
    if (line_unit_lbl)
      line_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    if (line_adj_x1) {
      line_adj_x1->set_value(cw * 0.25);
      line_adj_y1->set_value(ch * 0.5);
      line_adj_x2->set_value(cw * 0.75);
      line_adj_y2->set_value(ch * 0.5);
    }
  });
  auto *outer = make_pop_outer("Place Line");
  Gtk::SpinButton *lin_x1_spin = nullptr, *lin_y1_spin = nullptr,
                  *lin_x2_spin = nullptr, *lin_y2_spin = nullptr;
  line_adj_x1 = make_pop_row(outer, "X1:", &lin_x1_spin);
  curvz::utils::set_name(lin_x1_spin, "pop_tb_lin_x1", "popover_toolbar_line_x1_spn");
  line_adj_y1 = make_pop_row(outer, "Y1:", &lin_y1_spin);
  curvz::utils::set_name(lin_y1_spin, "pop_tb_lin_y1", "popover_toolbar_line_y1_spn");
  line_adj_x2 = make_pop_row(outer, "X2:", &lin_x2_spin);
  curvz::utils::set_name(lin_x2_spin, "pop_tb_lin_x2", "popover_toolbar_line_x2_spn");
  line_adj_y2 = make_pop_row(outer, "Y2:", &lin_y2_spin);
  curvz::utils::set_name(lin_y2_spin, "pop_tb_lin_y2", "popover_toolbar_line_y2_spn");
  line_adj_x1->set_value(100);
  line_adj_y1->set_value(500);
  line_adj_x2->set_value(900);
  line_adj_y2->set_value(500);

  line_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  line_unit_lbl->set_xalign(0.0f);
  line_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*line_unit_lbl);

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_halign(Gtk::Align::END);
  auto *place = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place, "pop_tb_lin_pl", "popover_toolbar_line_place_btn");
  place->add_css_class("suggested-action");
  place->signal_clicked().connect([this]() {
    if (line_adj_x1)
      sig_place_line.emit(
          line_adj_x1->get_value(), line_adj_y1->get_value(),
          line_adj_x2->get_value(), line_adj_y2->get_value());
    line_pop.popdown();
    sig_canvas_focus.emit();
  });
  btn_row->append(*place);
  outer->append(*btn_row);
  line_pop.set_child(*outer);
}

// ── Text placement popover (right-click)
// ───────────────────────────────────────
// s153 sub-ship 2d: moved into Impl. m_doc, m_popup_unit, and the
// place-text signal still live on the public Toolbar; reach via self->.
void Toolbar::Impl::build_text_popover(Gtk::ToggleButton *btn) {
  curvz::utils::set_name(text_pop, "pop_tb_txt", "popover_toolbar_text_root");
  setup_rclick_popover(btn, text_pop, [this]() {
    Unit u = doc ? (doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(doc->canvas.phys_unit) : doc->canvas.display_unit) : popup_unit;
    auto [cw, ch] = canvas_display_size(doc);
    if (text_unit_lbl)
      text_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    if (text_adj_x)
      text_adj_x->set_value(cw * 0.1);
    if (text_adj_y)
      text_adj_y->set_value(ch * 0.75);
    if (text_adj_size)
      text_adj_size->set_value(72.0);  // always points
    text_bold_btn.set_active(false);
    text_italic_btn.set_active(false);
    if (text_anchor_drop)
      text_anchor_drop->set_selected(0);
  });

  auto *outer = make_pop_outer("Place Text");

  // X / Y position
  Gtk::SpinButton *txt_x_spin = nullptr, *txt_y_spin = nullptr,
                  *txt_sz_spin = nullptr;
  text_adj_x = make_pop_row(outer, "X:", &txt_x_spin);
  curvz::utils::set_name(txt_x_spin, "pop_tb_txt_x", "popover_toolbar_text_x_spn");
  text_adj_y = make_pop_row(outer, "Y:", &txt_y_spin);
  curvz::utils::set_name(txt_y_spin, "pop_tb_txt_y", "popover_toolbar_text_y_spn");
  text_adj_x->set_value(100);
  text_adj_y->set_value(500);

  // Font size — always in points
  text_adj_size = make_pop_row(outer, "Size (pt):", &txt_sz_spin);
  curvz::utils::set_name(txt_sz_spin, "pop_tb_txt_sz", "popover_toolbar_text_size_spn");
  text_adj_size->set_lower(1.0);
  text_adj_size->set_upper(2000.0);
  text_adj_size->set_step_increment(1.0);
  text_adj_size->set_value(72);

  // Font family
  // Font family dropdown — populated from system Pango font map
  {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(8);
    auto *lbl = Gtk::make_managed<Gtk::Label>("Font:");
    lbl->add_css_class("tb-pop-label");
    lbl->set_xalign(0.0f);
    lbl->set_width_chars(4);
    row->append(*lbl);

    // Enumerate system fonts via Pango
    PangoFontMap *fmap = pango_cairo_font_map_get_default();
    PangoFontFamily **families = nullptr;
    int n_families = 0;
    pango_font_map_list_families(fmap, &families, &n_families);
    std::vector<std::string> fnames;
    fnames.reserve(n_families);
    for (int i = 0; i < n_families; ++i)
      fnames.push_back(pango_font_family_get_name(families[i]));
    g_free(families);
    std::sort(fnames.begin(), fnames.end());

    auto slist = Gtk::StringList::create({});
    guint sans_idx = 0;
    for (guint i = 0; i < (guint)fnames.size(); ++i) {
      slist->append(fnames[i]);
      if (fnames[i] == "Sans")
        sans_idx = i;
    }

    text_family_drop = Gtk::make_managed<Gtk::DropDown>(slist);
    curvz::utils::set_name(text_family_drop, "pop_tb_txt_fam", "popover_toolbar_text_font_family_dd");
    text_family_drop->set_enable_search(true);
    text_family_drop->set_selected(sans_idx);
    text_family_drop->set_hexpand(true);
    row->append(*text_family_drop);
    outer->append(*row);
  }

  // Bold / Italic toggles
  {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(12);
    text_bold_btn.set_label("Bold");
    curvz::utils::set_name(text_bold_btn, "pop_tb_txt_b", "popover_toolbar_text_bold_check");
    text_italic_btn.set_label("Italic");
    curvz::utils::set_name(text_italic_btn, "pop_tb_txt_i", "popover_toolbar_text_italic_check");
    row->append(text_bold_btn);
    row->append(text_italic_btn);
    outer->append(*row);
  }

  // Anchor dropdown
  {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(8);
    auto *lbl = Gtk::make_managed<Gtk::Label>("Align:");
    lbl->add_css_class("tb-pop-label");
    lbl->set_xalign(0.0f);
    lbl->set_width_chars(4);
    row->append(*lbl);
    auto anchor_items =
        Gtk::StringList::create({"Left", "Centre", "Right", "Justify"});
    text_anchor_drop = Gtk::make_managed<Gtk::DropDown>(anchor_items);
    curvz::utils::set_name(text_anchor_drop, "pop_tb_txt_an", "popover_toolbar_text_anchor_dd");
    text_anchor_drop->set_selected(0);
    text_anchor_drop->set_hexpand(true);
    row->append(*text_anchor_drop);
    outer->append(*row);
  }

  // Place button
  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_halign(Gtk::Align::END);
  auto *place = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place, "pop_tb_txt_pl", "popover_toolbar_text_place_btn");
  place->add_css_class("suggested-action");
  place->signal_clicked().connect([this]() {
    if (!text_adj_x)
      return;
    double x = text_adj_x->get_value();
    double y = text_adj_y->get_value();
    double size = text_adj_size->get_value();
    // Read selected font family from the dropdown
    std::string family = "Sans";
    if (text_family_drop) {
      auto *sl = dynamic_cast<Gtk::StringList *>(
          text_family_drop->get_model().get());
      auto sel = text_family_drop->get_selected();
      if (sl && sel != GTK_INVALID_LIST_POSITION)
        family = sl->get_string(sel);
    }
    bool bold = text_bold_btn.get_active();
    bool italic = text_italic_btn.get_active();
    std::string anchor = "start";
    std::string align = "left";
    if (text_anchor_drop) {
      auto sel = text_anchor_drop->get_selected();
      if (sel == 1) {
        anchor = "middle";
        align = "center";
      } else if (sel == 2) {
        anchor = "end";
        align = "right";
      } else if (sel == 3) {
        anchor = "start";
        align = "justify";
      }
    }
    sig_place_text.emit(x, y, family, size, bold, italic, anchor, align);
    text_pop.popdown();
    sig_canvas_focus.emit();
  });
  btn_row->append(*place);

  text_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  text_unit_lbl->set_xalign(0.0f);
  text_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*text_unit_lbl);

  outer->append(*btn_row);
  text_pop.set_child(*outer);
}

// ── Polygon / Star popover
// ────────────────────────────────────────────────────
// s153 sub-ship 2e: moved into Impl. m_doc, m_popup_unit, and
// m_sig_place_polygon / m_sig_canvas_focus still live on public
// Toolbar; reach via self->.
void Toolbar::Impl::build_polygon_popover(Gtk::ToggleButton *btn) {
  curvz::utils::set_name(poly_pop, "pop_tb_pol", "popover_toolbar_polygon_root");
  setup_rclick_popover(btn, poly_pop, [this]() {
    Unit u = doc ? (doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(doc->canvas.phys_unit) : doc->canvas.display_unit) : popup_unit;
    auto [cw, ch] = canvas_display_size(doc);
    if (poly_unit_lbl)
      poly_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    if (poly_adj_sides) {
      poly_adj_sides->set_value(poly_sides);
      poly_adj_inflect->set_value(poly_inflection * 100.0);
      poly_adj_cx->set_value(cw * 0.5);
      poly_adj_cy->set_value(ch * 0.5);
      poly_adj_r->set_value(std::min(cw, ch) * 0.2);
    }
    poly_preview.queue_draw();
  });

  auto *outer = make_pop_outer("Place Polygon / Star");

  // ── Interactive preview (160×160) ────────────────────────────────────
  curvz::utils::set_name(poly_preview, "pop_tb_pol_pv", "popover_toolbar_polygon_preview_da");
  poly_preview.set_size_request(160, 160);
  poly_preview.set_halign(Gtk::Align::CENTER);
  poly_preview.set_margin_bottom(6);

  poly_preview.set_draw_func(
      [this](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
        int sides = poly_adj_sides ? (int)poly_adj_sides->get_value()
                                   : poly_sides;
        double inflect = poly_adj_inflect
                             ? poly_adj_inflect->get_value() / 100.0
                             : poly_inflection;

        double perfect_star =
            (sides >= 5) ? std::cos(2.0 * M_PI / sides) / std::cos(M_PI / sides)
                         : -1.0; // no valid star for N<5
        bool snapped_star =
            (perfect_star > 0.0 && std::abs(inflect - perfect_star) < 0.001);
        bool snapped_poly = (inflect > 0.9990);

        double cx = w * 0.5;
        double cy = h * 0.5;
        double radius = std::min(w, h) * 0.40;
        double inner_r = radius * inflect;
        bool is_star = (inflect < 0.9999);
        int total = is_star ? sides * 2 : sides;
        double base_angle = -M_PI * 0.5; // point up

        // Dark background
        cr->set_source_rgb(0.13, 0.13, 0.13);
        cr->rectangle(0, 0, w, h);
        cr->fill();

        // Shape path
        cr->begin_new_path();
        for (int i = 0; i < total; ++i) {
          double angle =
              base_angle + i * (is_star ? M_PI / sides : 2.0 * M_PI / sides);
          double r = (!is_star || i % 2 == 0) ? radius : inner_r;
          double px = cx + r * std::cos(angle);
          double py = cy + r * std::sin(angle);
          if (i == 0)
            cr->move_to(px, py);
          else
            cr->line_to(px, py);
        }
        cr->close_path();

        // Fill
        cr->set_source_rgba(0.3, 0.6, 1.0, 0.20);
        cr->fill_preserve();

        // Stroke — green when snapped, blue otherwise
        if (snapped_star || snapped_poly)
          cr->set_source_rgba(0.3, 0.9, 0.4, 1.0);
        else
          cr->set_source_rgba(0.4, 0.7, 1.0, 0.95);
        cr->set_line_width(1.5);
        cr->set_line_join(Cairo::Context::LineJoin::MITER);
        cr->stroke();

        // ── Inflection handle ─────────────────────────────────────────────
        // Handle sits on the bisector closest to 0° (rightmost inner vertex).
        // For a star: bisector angle = base_angle + (M_PI / sides)
        // That's the angle of the first inner vertex (index 1).
        // For a regular polygon we draw it at outer radius on the same
        // bisector.
        double hdl_angle = base_angle + M_PI / sides; // first bisector
        double hdl_r = is_star ? inner_r : radius;
        double hdl_x = cx + hdl_r * std::cos(hdl_angle);
        double hdl_y = cy + hdl_r * std::sin(hdl_angle);

        // Dashed radial guide line from center to outer radius on bisector
        cr->set_source_rgba(0.5, 0.5, 0.5, 0.5);
        cr->set_line_width(0.75);
        std::vector<double> dash = {3.0, 3.0};
        cr->set_dash(dash, 0);
        cr->move_to(cx, cy);
        cr->line_to(cx + radius * std::cos(hdl_angle),
                    cy + radius * std::sin(hdl_angle));
        cr->stroke();
        cr->set_dash(std::vector<double>{}, 0);

        // Handle circle
        const double HDL_R = 5.0;
        cr->arc(hdl_x, hdl_y, HDL_R, 0, 2 * M_PI);
        if (snapped_star || snapped_poly) {
          cr->set_source_rgba(0.3, 0.9, 0.4, 1.0);
          cr->fill_preserve();
          cr->set_source_rgba(1.0, 1.0, 1.0, 0.9);
        } else {
          cr->set_source_rgba(0.25, 0.55, 1.0, 0.9);
          cr->fill_preserve();
          cr->set_source_rgba(1.0, 1.0, 1.0, 0.8);
        }
        cr->set_line_width(1.5);
        cr->stroke();

        // Snap label
        cr->set_source_rgba(0.5, 0.5, 0.5, 0.8);
        cr->set_font_size(9);
        if (snapped_poly) {
          cr->set_source_rgba(0.3, 0.9, 0.4, 0.9);
          cr->move_to(4, h - 4);
          cr->show_text("polygon");
        } else if (snapped_star) {
          cr->set_source_rgba(0.3, 0.9, 0.4, 0.9);
          cr->move_to(4, h - 4);
          cr->show_text("perfect star");
        }
      });

  // ── Inflection handle drag ────────────────────────────────────────────
  // Dragging the inner handle along the bisector radius sets inflection.
  // All positions are in preview widget coordinates.
  auto drag = Gtk::GestureDrag::create();
  drag->set_button(1);

  drag->signal_drag_begin().connect([this](double x, double y) {
    if (!poly_adj_sides || !poly_adj_inflect)
      return;
    int sides = (int)poly_adj_sides->get_value();
    double inflect = poly_adj_inflect->get_value() / 100.0;

    int w = poly_preview.get_width();
    int h = poly_preview.get_height();
    double cx = w * 0.5;
    double cy = h * 0.5;
    double radius = std::min(w, h) * 0.40;
    double inner_r = radius * inflect;
    bool is_star = (inflect < 0.9999);
    double base_angle = -M_PI * 0.5;
    double hdl_angle = base_angle + M_PI / sides;
    double hdl_r = is_star ? inner_r : radius;
    double hdl_x = cx + hdl_r * std::cos(hdl_angle);
    double hdl_y = cy + hdl_r * std::sin(hdl_angle);

    // Hit test: within 10px of handle
    if (std::hypot(x - hdl_x, y - hdl_y) <= 10.0) {
      poly_hdl_drag = true;
      poly_hdl_start_inflect = inflect;
    }
  });

  drag->signal_drag_update().connect([this](double dx, double dy) {
    if (!poly_hdl_drag || !poly_adj_sides || !poly_adj_inflect)
      return;
    int sides = (int)poly_adj_sides->get_value();
    int w = poly_preview.get_width();
    int h = poly_preview.get_height();
    double radius = std::min(w, h) * 0.40;
    double base_angle = -M_PI * 0.5;
    double hdl_angle = base_angle + M_PI / sides;

    // Project drag delta onto the bisector direction
    double dir_x = std::cos(hdl_angle);
    double dir_y = std::sin(hdl_angle);
    double proj = dx * dir_x + dy * dir_y;

    // New inner radius = start inner radius + projection
    double start_inner = radius * poly_hdl_start_inflect;
    double new_inner = std::clamp(start_inner + proj, 0.0, radius);
    double new_inflect = new_inner / radius;

    // Snap at perfect star and at 1.0 (polygon)
    // Perfect star snap point: cos(2π/N)/cos(π/N)
    double perfect_star =
        (sides >= 5) ? std::cos(2.0 * M_PI / sides) / std::cos(M_PI / sides)
                     : -1.0;
    double snap_thresh = 0.08;
    if (perfect_star > 0.0 &&
        std::abs(new_inflect - perfect_star) < snap_thresh)
      new_inflect = perfect_star;
    else if (new_inflect > 1.0 - snap_thresh)
      new_inflect = 1.0;

    new_inflect = std::clamp(new_inflect, 0.01, 1.0);
    poly_adj_inflect->set_value(new_inflect * 100.0);
    // queue_draw fires automatically via signal_value_changed
  });

  drag->signal_drag_end().connect(
      [this](double, double) { poly_hdl_drag = false; });

  poly_preview.add_controller(drag);
  outer->append(poly_preview);

  // ── Sides spinner ─────────────────────────────────────────────────────
  auto *sides_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  sides_row->set_spacing(8);
  auto *sides_lbl = Gtk::make_managed<Gtk::Label>("Sides:");
  sides_lbl->add_css_class("tb-pop-label");
  sides_lbl->set_xalign(0.0f);
  sides_lbl->set_width_chars(8);
  sides_row->append(*sides_lbl);
  poly_adj_sides = Gtk::Adjustment::create(poly_sides, 3, 64, 1, 5);
  auto *sides_spin = Gtk::make_managed<Gtk::SpinButton>(poly_adj_sides, 1, 0);
  curvz::utils::set_name(sides_spin, "pop_tb_pol_sd", "popover_toolbar_polygon_sides_spn");
  sides_spin->set_hexpand(true);
  sides_row->append(*sides_spin);
  outer->append(*sides_row);

  // ── Inflection spinner ────────────────────────────────────────────────
  auto *inf_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  inf_row->set_spacing(8);
  auto *inf_lbl = Gtk::make_managed<Gtk::Label>("Inflect %:");
  inf_lbl->add_css_class("tb-pop-label");
  inf_lbl->set_xalign(0.0f);
  inf_lbl->set_width_chars(8);
  inf_row->append(*inf_lbl);
  poly_adj_inflect =
      Gtk::Adjustment::create(poly_inflection * 100.0, 1.0, 100.0, 0.5, 5.0);
  auto *inf_spin =
      Gtk::make_managed<Gtk::SpinButton>(poly_adj_inflect, 0.5, 1);
  curvz::utils::set_name(inf_spin, "pop_tb_pol_in", "popover_toolbar_polygon_inflect_spn");
  inf_spin->set_hexpand(true);
  inf_row->append(*inf_spin);
  outer->append(*inf_row);

  // Redraw preview when values change
  poly_adj_sides->signal_value_changed().connect(
      [this]() { poly_preview.queue_draw(); });
  poly_adj_inflect->signal_value_changed().connect(
      [this]() { poly_preview.queue_draw(); });

  // ── Placement fields ──────────────────────────────────────────────────
  Gtk::SpinButton *pol_cx_spin = nullptr, *pol_cy_spin = nullptr,
                  *pol_r_spin  = nullptr;
  poly_adj_cx = make_pop_row(outer, "CX:", &pol_cx_spin);
  curvz::utils::set_name(pol_cx_spin, "pop_tb_pol_cx", "popover_toolbar_polygon_cx_spn");
  poly_adj_cy = make_pop_row(outer, "CY:", &pol_cy_spin);
  curvz::utils::set_name(pol_cy_spin, "pop_tb_pol_cy", "popover_toolbar_polygon_cy_spn");
  poly_adj_r = make_pop_row(outer, "R:", &pol_r_spin);
  curvz::utils::set_name(pol_r_spin, "pop_tb_pol_r", "popover_toolbar_polygon_r_spn");
  poly_adj_cx->set_value(500);
  poly_adj_cy->set_value(500);
  poly_adj_r->set_value(200);
  poly_adj_r->set_lower(1.0);

  poly_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  poly_unit_lbl->set_xalign(0.0f);
  poly_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*poly_unit_lbl);

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_halign(Gtk::Align::END);
  auto *place = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place, "pop_tb_pol_pl", "popover_toolbar_polygon_place_btn");
  place->add_css_class("suggested-action");
  place->signal_clicked().connect([this]() {
    if (!poly_adj_cx)
      return;
    poly_sides = (int)poly_adj_sides->get_value();
    poly_inflection = poly_adj_inflect->get_value() / 100.0;
    sig_place_polygon.emit(poly_adj_cx->get_value(),
                                   poly_adj_cy->get_value(),
                                   poly_adj_r->get_value(), poly_sides,
                                   poly_inflection, -M_PI * 0.5);
    poly_pop.popdown();
    sig_canvas_focus.emit();
  });
  btn_row->append(*place);
  outer->append(*btn_row);
  poly_pop.set_child(*outer);
}

// ── Snap popover (right-click on snap toggle button)
// ─────────────────────────────────────────
void Toolbar::Impl::build_snap_popover(Gtk::Widget *widget) {
  curvz::utils::set_name(snap_pop, "pop_tb_snap", "popover_toolbar_snap_settings_root");
  // Attach a right-click gesture directly to the widget. Works for any
  // widget — the prior CurvzSwitch implementation needed this approach
  // because CurvzSwitch is a DrawingArea (not a ToggleButton); s152's
  // Gtk::ToggleButton replacement uses the same pattern unchanged.
  // S89: was Ctrl+left-click; now plain right-click for world consistency.
  auto rclick = Gtk::GestureClick::create();
  rclick->set_button(3);
  // s152 sub-ship 1 fix: claim on press so the right-click doesn't
  // bubble up to the toolbar root's density picker.
  auto* rclick_raw = rclick.get();
  rclick->signal_pressed().connect(
      [this, rclick_raw](int, double, double) {
        rclick_raw->set_state(Gtk::EventSequenceState::CLAIMED);
        sig_snap_pop_open.emit(); // let MainWindow push current settings first
        snap_pop.popup();
      });
  widget->add_controller(rclick);
  snap_pop.set_parent(*widget);
  snap_pop.set_position(Gtk::PositionType::RIGHT);

  auto *outer = make_pop_outer("Snap Settings");

  // Each checkbutton tagged inline with a literal-string set_name so
  // widget_names_sync's harvester picks up all six rows. Loop body would
  // hide the literals from the harvester (variable args don't match the
  // regex) — explicit unroll, six near-identical blocks, is the version
  // that actually populates the DB.
  auto add_snap_cb = [&](const char *label, Gtk::CheckButton **slot,
                         Gtk::CheckButton *cb) {
    cb->add_css_class("tb-pop-label");
    *slot = cb;
    cb->signal_toggled().connect([this]() {
      if (snap_loading || !snap_cb_guides) return;
      SnapSettings s;
      s.snap_guides  = snap_cb_guides->get_active();
      s.snap_grid    = snap_cb_grid->get_active();
      s.snap_margins = snap_cb_margins->get_active();
      s.snap_nodes   = snap_cb_nodes->get_active();
      s.snap_edges   = snap_cb_edges->get_active();
      s.snap_centers = snap_cb_centers->get_active();
      sig_snap_settings.emit(s);
    });
    outer->append(*cb);
    (void)label;  // label was used at construction, kept for clarity
  };

  {
    auto *cb = Gtk::make_managed<Gtk::CheckButton>("Snap to guides");
    curvz::utils::set_name(cb, "pop_tb_snap_g", "popover_toolbar_snap_guides_check");
    add_snap_cb("Snap to guides", &snap_cb_guides, cb);
  }
  {
    auto *cb = Gtk::make_managed<Gtk::CheckButton>("Snap to grid");
    curvz::utils::set_name(cb, "pop_tb_snap_gr", "popover_toolbar_snap_grid_check");
    add_snap_cb("Snap to grid", &snap_cb_grid, cb);
  }
  {
    auto *cb = Gtk::make_managed<Gtk::CheckButton>("Snap to margins");
    curvz::utils::set_name(cb, "pop_tb_snap_m", "popover_toolbar_snap_margins_check");
    add_snap_cb("Snap to margins", &snap_cb_margins, cb);
  }
  {
    auto *cb = Gtk::make_managed<Gtk::CheckButton>("Snap to nodes");
    curvz::utils::set_name(cb, "pop_tb_snap_n", "popover_toolbar_snap_nodes_check");
    add_snap_cb("Snap to nodes", &snap_cb_nodes, cb);
  }
  {
    auto *cb = Gtk::make_managed<Gtk::CheckButton>("Snap to edges");
    curvz::utils::set_name(cb, "pop_tb_snap_e", "popover_toolbar_snap_edges_check");
    add_snap_cb("Snap to edges", &snap_cb_edges, cb);
  }
  {
    auto *cb = Gtk::make_managed<Gtk::CheckButton>("Snap to centers");
    curvz::utils::set_name(cb, "pop_tb_snap_c", "popover_toolbar_snap_centers_check");
    add_snap_cb("Snap to centers", &snap_cb_centers, cb);
  }

  snap_pop.set_child(*outer);
}

void Toolbar::Impl::set_snap_settings(const SnapSettings &s) {
  snap_loading = true;
  if (snap_cb_guides)  snap_cb_guides->set_active(s.snap_guides);
  if (snap_cb_grid)    snap_cb_grid->set_active(s.snap_grid);
  if (snap_cb_margins) snap_cb_margins->set_active(s.snap_margins);
  if (snap_cb_nodes)   snap_cb_nodes->set_active(s.snap_nodes);
  if (snap_cb_edges)   snap_cb_edges->set_active(s.snap_edges);
  if (snap_cb_centers) snap_cb_centers->set_active(s.snap_centers);
  snap_loading = false;
}

// ── Spiral popover
// ────────────────────────────────────────────────────────────
// s153 sub-ship 2e: moved into Impl. m_doc, m_popup_unit, and
// m_sig_place_spiral / m_sig_canvas_focus still live on public Toolbar;
// reach via self->.
void Toolbar::Impl::build_spiral_popover(Gtk::ToggleButton *btn) {
  curvz::utils::set_name(spiral_pop, "pop_tb_spi", "popover_toolbar_spiral_root");
  setup_rclick_popover(btn, spiral_pop, [this]() {
    Unit u = doc ? (doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(doc->canvas.phys_unit) : doc->canvas.display_unit) : popup_unit;
    auto [cw, ch] = canvas_display_size(doc);
    if (spiral_unit_lbl)
      spiral_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    if (spiral_adj_turns) {
      spiral_adj_turns->set_value(spiral_turns);
      spiral_adj_inner->set_value(spiral_inner);
      spiral_adj_cx->set_value(cw * 0.5);
      spiral_adj_cy->set_value(ch * 0.5);
      spiral_adj_r->set_value(std::min(cw, ch) * 0.2);
    }
    spiral_preview.queue_draw();
  });

  auto *outer = make_pop_outer("Place Spiral");

  // ── Interactive preview (160×160) ────────────────────────────────────
  curvz::utils::set_name(spiral_preview, "pop_tb_spi_pv", "popover_toolbar_spiral_preview_da");
  spiral_preview.set_size_request(160, 160);
  spiral_preview.set_halign(Gtk::Align::CENTER);
  spiral_preview.set_margin_bottom(6);

  spiral_preview.set_draw_func([this](const Cairo::RefPtr<Cairo::Context> &cr,
                                      int w, int h) {
    double turns =
        spiral_adj_turns ? spiral_adj_turns->get_value() : spiral_turns;
    double inner_p =
        spiral_adj_inner ? spiral_adj_inner->get_value() : spiral_inner;

    double cx = w * 0.5;
    double cy = h * 0.5;
    double outer_r = std::min(w, h) * 0.43;
    double inner_r = outer_r * (inner_p / 100.0);

    // Dark background
    cr->set_source_rgb(0.13, 0.13, 0.13);
    cr->rectangle(0, 0, w, h);
    cr->fill();

    // S98: render via the same spiral_to_path the canvas uses, so the
    // preview always matches what gets placed. No inline reimplementation.
    PathData pd = spiral_to_path(cx, cy, outer_r, inner_r, turns,
                                 -M_PI * 0.5);

    cr->begin_new_path();
    for (size_t i = 0; i < pd.nodes.size(); ++i) {
      const auto &n = pd.nodes[i];
      if (i == 0) {
        cr->move_to(n.x, n.y);
      } else {
        const auto &p = pd.nodes[i - 1];
        cr->curve_to(p.cx2, p.cy2, n.cx1, n.cy1, n.x, n.y);
      }
    }

    cr->set_source_rgba(0.4, 0.7, 1.0, 0.95);
    cr->set_line_width(1.5);
    cr->set_line_cap(Cairo::Context::LineCap::ROUND);
    cr->stroke();

    // Small dot at centre
    cr->arc(cx, cy, 2.5, 0, 2 * M_PI);
    cr->set_source_rgba(0.5, 0.5, 0.5, 0.7);
    cr->fill();
  });

  outer->append(spiral_preview);

  // ── Turns spinner ─────────────────────────────────────────────────────
  auto *turns_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  turns_row->set_spacing(8);
  auto *turns_lbl = Gtk::make_managed<Gtk::Label>("Turns:");
  turns_lbl->add_css_class("tb-pop-label");
  turns_lbl->set_xalign(0.0f);
  turns_lbl->set_width_chars(10);
  turns_row->append(*turns_lbl);
  spiral_adj_turns =
      Gtk::Adjustment::create(spiral_turns, 0.25, 20.0, 0.25, 1.0);
  auto *turns_spin =
      Gtk::make_managed<Gtk::SpinButton>(spiral_adj_turns, 0.25, 2);
  curvz::utils::set_name(turns_spin, "pop_tb_spi_tn", "popover_toolbar_spiral_turns_spn");
  turns_spin->set_hexpand(true);
  turns_row->append(*turns_spin);
  outer->append(*turns_row);

  // ── Inner radius % spinner ────────────────────────────────────────────
  auto *inner_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  inner_row->set_spacing(8);
  auto *inner_lbl = Gtk::make_managed<Gtk::Label>("Inner r %:");
  inner_lbl->add_css_class("tb-pop-label");
  inner_lbl->set_xalign(0.0f);
  inner_lbl->set_width_chars(10);
  inner_row->append(*inner_lbl);
  spiral_adj_inner =
      Gtk::Adjustment::create(spiral_inner, 0.0, 95.0, 1.0, 5.0);
  auto *inner_spin =
      Gtk::make_managed<Gtk::SpinButton>(spiral_adj_inner, 1.0, 1);
  curvz::utils::set_name(inner_spin, "pop_tb_spi_ir", "popover_toolbar_spiral_inner_r_spn");
  inner_spin->set_hexpand(true);
  inner_row->append(*inner_spin);
  outer->append(*inner_row);

  // Redraw preview when values change
  spiral_adj_turns->signal_value_changed().connect(
      [this]() { spiral_preview.queue_draw(); });
  spiral_adj_inner->signal_value_changed().connect(
      [this]() { spiral_preview.queue_draw(); });

  // ── Placement fields ──────────────────────────────────────────────────
  Gtk::SpinButton *spi_cx_spin = nullptr, *spi_cy_spin = nullptr,
                  *spi_r_spin  = nullptr;
  spiral_adj_cx = make_pop_row(outer, "CX:", &spi_cx_spin);
  curvz::utils::set_name(spi_cx_spin, "pop_tb_spi_cx", "popover_toolbar_spiral_cx_spn");
  spiral_adj_cy = make_pop_row(outer, "CY:", &spi_cy_spin);
  curvz::utils::set_name(spi_cy_spin, "pop_tb_spi_cy", "popover_toolbar_spiral_cy_spn");
  spiral_adj_r = make_pop_row(outer, "R:", &spi_r_spin);
  curvz::utils::set_name(spi_r_spin, "pop_tb_spi_r", "popover_toolbar_spiral_r_spn");
  spiral_adj_cx->set_value(500);
  spiral_adj_cy->set_value(500);
  spiral_adj_r->set_value(200);
  spiral_adj_r->set_lower(1.0);

  spiral_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  spiral_unit_lbl->set_xalign(0.0f);
  spiral_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*spiral_unit_lbl);

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_halign(Gtk::Align::END);
  auto *place = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place, "pop_tb_spi_pl", "popover_toolbar_spiral_place_btn");
  place->add_css_class("suggested-action");
  place->signal_clicked().connect([this]() {
    if (!spiral_adj_cx)
      return;
    spiral_turns = spiral_adj_turns->get_value();
    spiral_inner = spiral_adj_inner->get_value();
    double r = spiral_adj_r->get_value();
    double inner_r = r * (spiral_inner / 100.0);
    sig_place_spiral.emit(spiral_adj_cx->get_value(),
                                  spiral_adj_cy->get_value(), r, inner_r,
                                  spiral_turns, -M_PI * 0.5);
    spiral_pop.popdown();
    sig_canvas_focus.emit();
  });
  btn_row->append(*place);
  outer->append(*btn_row);
  spiral_pop.set_child(*outer);
}

// ── S87 m1: swatch picker section ─────────────────────────────────────────────
//
// Per-popover layout:
//
//   ┌──────────────────────────────────────┐
//   │ Swatches      [palette dropdown    ▾] │
//   │ ┌──┬──┬──┬──┬──┬──┬──┐                │
//   │ │  │  │  │  │  │  │  │  ← FlowBox     │
//   │ └──┴──┴──┴──┴──┴──┴──┘                │
//   └──────────────────────────────────────┘
//
// Click a chip → m_def_*.{r,g,b} = swatch.color, type=Solid, refresh
// + emit_defaults. No binding state on the toolbar — copy-only.
//
// Visibility gate: section appended to outer at build time as a hidden
// Box. set_swatch_library() flips visibility when a non-null library
// arrives, and rebuilds the dropdown + chips.

void Toolbar::Impl::build_swatch_picker_section(Gtk::Box& outer, bool is_stroke) {
    auto* section = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    section->set_spacing(6);
    section->set_margin_top(6);

    // Header: "Swatches" label + palette dropdown (right-aligned).
    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    header->set_spacing(6);
    auto* title = Gtk::make_managed<Gtk::Label>("Swatches");
    title->add_css_class("tb-pop-label");
    title->set_xalign(0.0f);
    title->set_hexpand(true);
    header->append(*title);

    auto* dd = Gtk::make_managed<Gtk::DropDown>();
    dd->add_css_class("tb-palette-dd");
    // Initial empty model — rebuild_palette_dropdown swaps real entries
    // in once a library is wired.
    dd->set_model(Gtk::StringList::create({}));
    header->append(*dd);
    section->append(*header);

    // Chip flow. Constraints chosen to roughly match the StylesPanel
    // chip-grid idiom (S85 cont-3 fix5): 24px chips, max 8 per row in
    // the toolbar popover (which is narrower than the styles panel).
    auto* flow = Gtk::make_managed<Gtk::FlowBox>();
    flow->set_selection_mode(Gtk::SelectionMode::NONE);
    flow->set_max_children_per_line(8);
    flow->set_min_children_per_line(1);
    flow->set_homogeneous(true);
    flow->set_row_spacing(4);
    flow->set_column_spacing(4);
    flow->add_css_class("tb-chip-flow");
    section->append(*flow);

    // Stash pointers per slot so refresh / set_swatch_library can find
    // the right widgets without conditionals scattered everywhere.
    // s119: also set_name the dd/flow per slot so GTK Inspector sees
    // distinct names for fill vs stroke. Branch is the natural seam —
    // each call site provides literal abbrev/long_name strings, which
    // is what widget_names_sync's harvester regex requires.
    if (is_stroke) {
        stroke_picker_section = section;
        stroke_palette_dd     = dd;
        stroke_chip_flow      = flow;
        curvz::utils::set_name(dd, "pop_tb_strk_pdd", "popover_toolbar_stroke_palette_dd");
        curvz::utils::set_name(flow, "pop_tb_strk_cf", "popover_toolbar_stroke_chip_flow");
    } else {
        fill_picker_section   = section;
        fill_palette_dd       = dd;
        fill_chip_flow        = flow;
        curvz::utils::set_name(dd, "pop_tb_fill_pdd", "popover_toolbar_fill_palette_dd");
        curvz::utils::set_name(flow, "pop_tb_fill_cf", "popover_toolbar_fill_chip_flow");
    }

    // Hidden by default — flipped on by set_swatch_library() when a
    // library is wired and has at least one swatch.
    section->set_visible(false);

    outer.append(*section);
}

void Toolbar::Impl::set_swatch_library(const color::SwatchLibrary* lib) {
    swatch_library = lib;
    rebuild_swatch_pickers();
}

void Toolbar::Impl::rebuild_swatch_pickers() {
    // Both pickers might not exist yet (set_swatch_library could be
    // called before build_*_popover; not the case in current MainWindow
    // wiring, but the guard makes the method idempotent).
    if (!fill_picker_section || !stroke_picker_section) return;

    const bool has_lib = (swatch_library != nullptr) &&
                         (swatch_library->swatch_count() > 0);
    if (has_lib) {
        rebuild_palette_dropdown(/*is_stroke=*/false);
        rebuild_palette_dropdown(/*is_stroke=*/true);
        rebuild_chip_grid(/*is_stroke=*/false);
        rebuild_chip_grid(/*is_stroke=*/true);
    }

    // S87 m1 v2: actual visibility is now driven by refresh_*_popover
    // (which respects m_*_picker_open and the type-toggle state). Force
    // a refresh so the Swatch button sensitivity, picker visibility,
    // and colour-row visibility all reflect current state. If the
    // popovers haven't been built yet (set_swatch_library called too
    // early), refresh is harmless — set_active and set_visible on
    // freshly-constructed widgets just no-op.
    refresh_fill_popover();
    refresh_stroke_popover();
}

void Toolbar::Impl::rebuild_palette_dropdown(bool is_stroke) {
    if (!swatch_library) return;
    Gtk::DropDown* dd = is_stroke ? stroke_palette_dd : fill_palette_dd;
    auto& palette_ids = is_stroke ? stroke_palette_ids : fill_palette_ids;
    auto& conn        = is_stroke ? stroke_palette_dd_conn
                                  : fill_palette_dd_conn;
    if (!dd) return;

    // Disconnect prior selection handler so programmatic refill doesn't
    // fire a user-flow chip rebuild before the IDs vector is populated.
    if (conn.connected()) conn.disconnect();

    palette_ids.clear();
    palette_ids.push_back(kAllPaletteId);

    auto sl = Gtk::StringList::create({});
    sl->append("All");

    for (const auto& pid : swatch_library->all_palette_ids()) {
        const color::Palette* p = swatch_library->find_palette(pid);
        if (!p) continue;
        palette_ids.push_back(pid);
        sl->append(p->name.empty() ? std::string("(unnamed)") : p->name);
    }

    dd->set_model(sl);
    dd->set_selected(0);  // default to "All"

    // Reconnect: row index → palette id; rebuild chip grid for the
    // active slot only. The "other" slot keeps its independent state.
    conn = dd->property_selected().signal_changed().connect(
        [this, is_stroke]() {
            rebuild_chip_grid(is_stroke);
        });
}

void Toolbar::Impl::rebuild_chip_grid(bool is_stroke) {
    if (!swatch_library) return;
    Gtk::FlowBox* flow = is_stroke ? stroke_chip_flow : fill_chip_flow;
    Gtk::DropDown* dd  = is_stroke ? stroke_palette_dd : fill_palette_dd;
    const auto& palette_ids = is_stroke ? stroke_palette_ids
                                        : fill_palette_ids;
    auto& shown_pid = is_stroke ? stroke_chips_palette_id
                                : fill_chips_palette_id;
    if (!flow || !dd || palette_ids.empty()) return;

    // Resolve the requested palette id from the dropdown selection.
    const auto sel = dd->get_selected();
    if (sel == GTK_INVALID_LIST_POSITION ||
        sel >= palette_ids.size()) return;
    const std::string& requested_pid = palette_ids[sel];

    // S87 m1 fix2: short-circuit when the requested palette is already
    // rendered. The dropdown's property_selected can fire spuriously
    // (panel rebuild gotcha from the handoff). Without this guard,
    // every spurious fire rebuilt the FlowBox children, tearing down
    // the chips' GestureClick controllers — causing the asymmetric
    // chip-click-then-popdown bug where only the first click after a
    // (spurious) rebuild dismissed the popover cleanly. Comparing the
    // requested vs already-shown palette id makes the rebuild idempotent.
    if (requested_pid == shown_pid && flow->get_first_child() != nullptr) {
        return;
    }

    // Tear down current children. FlowBox::remove takes a child widget;
    // walk first_child until the box is empty (GTK4 idiom, same pattern
    // as PaintEditor::rebuild_chip_grid).
    while (auto* child = flow->get_first_child()) {
        flow->remove(*child);
    }

    std::vector<std::string> ids;
    if (requested_pid == kAllPaletteId) {
        ids = swatch_library->all_swatch_ids();
    } else {
        const color::Palette* p =
            swatch_library->find_palette(requested_pid);
        if (p) ids = p->swatches;
    }

    for (const auto& id : ids) {
        std::string name;
        color::Color c;
        if (!tb_resolve_solid(*swatch_library, id, name, c)) continue;

        auto* area = Gtk::make_managed<Gtk::DrawingArea>();
        area->set_content_width(TB_CHIP_SIZE);
        area->set_content_height(TB_CHIP_SIZE);
        // Capture by value — chip flow gets rebuilt whenever the palette
        // changes, so closure lifetime is bounded by the next rebuild.
        area->set_draw_func(
            [c](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                tb_paint_chip(cr, w, h, c);
            });
        area->add_css_class("swatch-chip");
        area->set_cursor(Gdk::Cursor::create("pointer"));

        std::string hex = color::to_hex(c);
        std::string tip = name.empty() ? hex : (name + "  " + hex);
        area->set_tooltip_text(tip);

        // Left-click → copy swatch's RGB into m_def_* as Solid. Routes
        // through apply_swatch_pick_to_* so the popover refresh + emit
        // path matches the hex-paste flow.
        //
        // Important: signal_pressed (not signal_released). The toolbar's
        // fill/stroke popovers are autohide; a GestureClick connected to
        // signal_released claims the press-release sequence, which can
        // interfere with the popover's outside-click dismissal.
        //
        // S87 m1 fix4: popdown is deferred to idle. Calling popdown()
        // synchronously inside the gesture event handler races with
        // GTK's grab management — when the chip click is a no-op
        // (picked colour matches current colour, no broadcast chain
        // forces focus elsewhere), the synchronous popdown gets
        // cancelled and the popover stays open until the app loses
        // focus. By queuing popdown for idle dispatch, the gesture
        // event finishes first; idle then runs after the press is
        // fully processed and popdown completes cleanly regardless of
        // whether the pick caused a state change.
        auto gesture = Gtk::GestureClick::create();
        gesture->set_button(1);
        const std::string captured_id = id;
        const bool stroke_local = is_stroke;
        gesture->signal_pressed().connect(
            [this, captured_id, stroke_local](int, double, double) {
                if (stroke_local)
                    apply_swatch_pick_to_stroke(captured_id);
                else
                    apply_swatch_pick_to_fill(captured_id);
                Glib::signal_idle().connect_once(
                    [this, stroke_local]() {
                        if (stroke_local) stroke_pop.popdown();
                        else              fill_pop.popdown();
                    });
            });
        area->add_controller(gesture);

        flow->append(*area);
    }

    // S87 m1 fix2: record the palette this rebuild rendered, so a
    // subsequent spurious dropdown signal_changed for the same id
    // short-circuits at the guard above.
    shown_pid = requested_pid;
}

void Toolbar::Impl::apply_swatch_pick_to_fill(const std::string& swatch_id) {
    // S93 m4: defensive syncing guard. See apply_hex_to_fill.
    if (syncing) return;
    if (!swatch_library) return;
    std::string name;
    color::Color c;
    if (!tb_resolve_solid(*swatch_library, swatch_id, name, c)) return;
    def_fill.type = FillStyle::Type::Solid;
    def_fill.r = c.r;
    def_fill.g = c.g;
    def_fill.b = c.b;
    refresh_fill_popover();
    redraw_well();
    emit_defaults();
}

void Toolbar::Impl::apply_swatch_pick_to_stroke(const std::string& swatch_id) {
    // S93 m4: defensive syncing guard. See apply_hex_to_fill.
    if (syncing) return;
    if (!swatch_library) return;
    std::string name;
    color::Color c;
    if (!tb_resolve_solid(*swatch_library, swatch_id, name, c)) return;
    def_stroke.paint.type = FillStyle::Type::Solid;
    def_stroke.paint.r = c.r;
    def_stroke.paint.g = c.g;
    def_stroke.paint.b = c.b;
    refresh_stroke_popover();
    redraw_well();
    emit_defaults();
}

} // namespace Curvz
