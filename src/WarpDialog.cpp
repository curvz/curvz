#include "WarpDialog.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2
#include <cmath>
#include <gtkmm/adjustment.h>
#include <vector>

namespace Curvz {

// ─────────────────────────────────────────────────────────────────────────────
// Preset shape generators
// ─────────────────────────────────────────────────────────────────────────────
//
// Each preset produces two PathData objects — top and bottom envelope
// curves — given:
//   - the glyph-cache bbox (bx, by, bw, bh) in doc space Y-down
//   - an anchor count for each side (2..4)
//
// Anchor-count-3 curves place the extra anchor at the midpoint; count-4
// places two anchors at 1/3 and 2/3 along the span. Handles position
// 1/3 of segment length along the tangent direction, which makes
// straight segments render truly straight (colinear handles) and curved
// segments curve as expected.
//
// Y convention: top of bbox = by (small y in Y-down). Bottom of bbox =
// by + bh. When a preset "arcs up", the top curve's mid-anchor pushes
// to smaller y (visually upward).

namespace {

// Helper to build a single anchor with colinear handles at distance dx
// left and right. Produces a straight-segment cubic.
BezierNode mk_straight(double x, double y, double dx_in, double dx_out) {
    BezierNode n;
    n.x = x; n.y = y;
    n.cx1 = x - dx_in;  n.cy1 = y;
    n.cx2 = x + dx_out; n.cy2 = y;
    n.type = BezierNode::Type::Smooth;
    return n;
}

// Build a flat (straight horizontal) envelope at y=fixed_y across
// [bx, bx+bw] with `count` evenly-spaced anchors.
PathData build_flat(double bx, double bw, double fixed_y, int count) {
    PathData pd;
    pd.closed = false;
    if (count < 2) count = 2;
    if (count > 4) count = 4;
    // Segment length for handle distance — 1/3 of segment span.
    double seg_span = bw / (count - 1);
    double h = seg_span / 3.0;
    for (int i = 0; i < count; ++i) {
        double x = bx + (bw * i) / (count - 1);
        double dx_in  = (i == 0)         ? 0.0 : h;
        double dx_out = (i == count - 1) ? 0.0 : h;
        pd.nodes.push_back(mk_straight(x, fixed_y, dx_in, dx_out));
    }
    return pd;
}

// Build an arced envelope: `count` anchors along the base y, with
// the interior (non-endpoint) anchors displaced vertically by
// `amplitude` (+ = down in Y-down, - = up).
//
// 2 anchors → no arc possible (just a line), return flat.
// 3 anchors → midpoint lifts by amplitude.
// 4 anchors → both interior anchors lift by amplitude, making a
//   smoother/broader arc.
//
// Handles are positioned horizontally 1/3 of segment span with
// vertical offset zero on endpoints (to keep the endpoints' tangents
// horizontal) and scaled toward the arc direction on interior anchors
// (to smooth the peak).
PathData build_arc(double bx, double bw, double base_y,
                   double amplitude, int count) {
    if (count <= 2)
        return build_flat(bx, bw, base_y, count);
    PathData pd;
    pd.closed = false;
    if (count > 4) count = 4;
    double seg_span = bw / (count - 1);
    double h = seg_span / 3.0;
    for (int i = 0; i < count; ++i) {
        double x = bx + (bw * i) / (count - 1);
        bool interior = (i != 0 && i != count - 1);
        double y = interior ? (base_y + amplitude) : base_y;
        BezierNode n;
        n.x = x; n.y = y;
        n.cx1 = (i == 0)         ? x : (x - h);
        n.cy1 = y;
        n.cx2 = (i == count - 1) ? x : (x + h);
        n.cy2 = y;
        n.type = BezierNode::Type::Smooth;
        pd.nodes.push_back(n);
    }
    return pd;
}

// Build a perspective-trapezoidal envelope: straight-line top/bottom
// with non-uniform spacing. For perspective near (base wider than
// opposite), the output curve is a narrower horizontal line at the
// same y as the opposite side would be — we implement this by
// shrinking the x-span of one boundary. The envelope itself remains
// straight; the perspective effect comes from one side being narrower
// than the other.
//
// inset_factor: 0.0 = no inset (flat), 0.5 = narrow to half width.
PathData build_trapezoid_side(double bx, double bw, double fixed_y,
                              double inset_factor, int count) {
    double inset = bw * inset_factor * 0.5;
    return build_flat(bx + inset, bw - 2.0 * inset, fixed_y, count);
}

// Build a wavy envelope using sinusoidal displacement. Only meaningful
// for count >= 3 (single-segment envelope can't express a wave). For
// count == 2 falls back to flat.
PathData build_wave(double bx, double bw, double base_y,
                    double amplitude, int count) {
    if (count <= 2)
        return build_flat(bx, bw, base_y, count);
    PathData pd;
    pd.closed = false;
    if (count > 4) count = 4;
    double seg_span = bw / (count - 1);
    double h = seg_span / 3.0;
    for (int i = 0; i < count; ++i) {
        double x = bx + (bw * i) / (count - 1);
        // Alternate interior anchors +amp / -amp. Endpoints stay on base.
        bool interior = (i != 0 && i != count - 1);
        double y = base_y;
        if (interior) {
            // For count=3: one interior anchor, just use +amp.
            // For count=4: two interior anchors, alternate +amp, -amp.
            int interior_idx = i - 1;
            y = base_y + ((interior_idx % 2 == 0) ? amplitude : -amplitude);
        }
        BezierNode n;
        n.x = x; n.y = y;
        n.cx1 = (i == 0)         ? x : (x - h);
        n.cy1 = y;
        n.cx2 = (i == count - 1) ? x : (x + h);
        n.cy2 = y;
        n.type = BezierNode::Type::Smooth;
        pd.nodes.push_back(n);
    }
    return pd;
}

// Preset index constants — match the order of strings added to m_preset_model.
enum PresetIndex {
    PRESET_FLAT             = 0,
    PRESET_ARC_UP           = 1,
    PRESET_ARC_DOWN         = 2,
    PRESET_BULGE            = 3,
    PRESET_SQUEEZE          = 4,
    PRESET_PERSPECTIVE_NEAR = 5,
    PRESET_PERSPECTIVE_FAR  = 6,
    PRESET_WAVE             = 7
};

// Generate both envelopes for a given preset + bbox + anchor counts.
// top_env and bottom_env are output params.
void generate_preset(int preset_idx,
                     double bx, double by, double bw, double bh,
                     int top_count, int bot_count,
                     PathData &top_env, PathData &bot_env) {
    double y_top    = by;             // smaller y (up in Y-down)
    double y_bottom = by + bh;        // larger y (down in Y-down)
    double amp      = bh * 0.25;      // 25% bbox height as arc amplitude
    double insetF   = 0.30;           // 30% side-inset for perspective

    switch (preset_idx) {
    case PRESET_FLAT:
    default:
        top_env = build_flat(bx, bw, y_top,    top_count);
        bot_env = build_flat(bx, bw, y_bottom, bot_count);
        break;
    case PRESET_ARC_UP:
        // Top curves up (smaller y), bottom straight
        top_env = build_arc(bx, bw, y_top, -amp, top_count);
        bot_env = build_flat(bx, bw, y_bottom, bot_count);
        break;
    case PRESET_ARC_DOWN:
        // Top straight, bottom curves down (larger y)
        top_env = build_flat(bx, bw, y_top, top_count);
        bot_env = build_arc(bx, bw, y_bottom, +amp, bot_count);
        break;
    case PRESET_BULGE:
        // Top curves up, bottom curves down → balloon
        top_env = build_arc(bx, bw, y_top,    -amp, top_count);
        bot_env = build_arc(bx, bw, y_bottom, +amp, bot_count);
        break;
    case PRESET_SQUEEZE:
        // Top curves down, bottom curves up → pinch
        top_env = build_arc(bx, bw, y_top,    +amp, top_count);
        bot_env = build_arc(bx, bw, y_bottom, -amp, bot_count);
        break;
    case PRESET_PERSPECTIVE_NEAR:
        // Top narrower than bottom (wide base, vanishing top)
        top_env = build_trapezoid_side(bx, bw, y_top, insetF, top_count);
        bot_env = build_flat(bx, bw, y_bottom, bot_count);
        break;
    case PRESET_PERSPECTIVE_FAR:
        // Bottom narrower than top
        top_env = build_flat(bx, bw, y_top, top_count);
        bot_env = build_trapezoid_side(bx, bw, y_bottom, insetF, bot_count);
        break;
    case PRESET_WAVE:
        // Both curves wave — requires count >= 3 to look like anything
        top_env = build_wave(bx, bw, y_top,    -amp * 0.6, top_count);
        bot_env = build_wave(bx, bw, y_bottom, +amp * 0.6, bot_count);
        break;
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// WarpDialog
// ─────────────────────────────────────────────────────────────────────────────

WarpDialog::WarpDialog()
    : m_top_count(Gtk::Adjustment::create(2, 2, 4, 1, 1), 0.0, 0)
    , m_bot_count(Gtk::Adjustment::create(2, 2, 4, 1, 1), 0.0, 0)
    , m_quality(Gtk::Adjustment::create(4, 1, 16, 1, 1),
                Gtk::Orientation::HORIZONTAL)
{
    curvz::utils::set_name(*this, "dlg_wrp", "warp_dialog_root");
    set_title("Warp");
    set_resizable(false);
    set_hide_on_close(true);
    set_default_size(360, -1);

    m_outer.set_spacing(0);

    // ── Form grid ────────────────────────────────────────────────────────
    m_grid.set_row_spacing(8);
    m_grid.set_column_spacing(12);
    m_grid.set_margin(16);

    auto make_label = [](const char* text) {
        auto* lbl = Gtk::make_managed<Gtk::Label>(text);
        lbl->set_xalign(1.0f);
        lbl->set_hexpand(false);
        return lbl;
    };

    int row = 0;

    // Preset dropdown
    m_grid.attach(*make_label("Preset:"), 0, row);
    m_preset_model = Gtk::StringList::create({
        "Flat",
        "Arc Up",
        "Arc Down",
        "Bulge",
        "Squeeze",
        "Perspective Near",
        "Perspective Far",
        "Wave"
    });
    m_preset.set_model(m_preset_model);
    m_preset.set_selected(0);
    m_preset.set_hexpand(true);
    m_preset.set_tooltip_text(
        "Starting envelope shape. Changing this regenerates top and bottom "
        "curves from the preset formula scaled to your source's bbox.");
    curvz::utils::set_name(m_preset, "dlg_wrp_pre", "warp_dialog_preset_dd");
    m_grid.attach(m_preset, 1, row);
    ++row;

    // Anchor counts
    m_grid.attach(*make_label("Top anchors:"), 0, row);
    m_top_count.set_hexpand(true);
    m_top_count.set_width_chars(4);
    m_top_count.set_tooltip_text(
        "Number of anchors on the top envelope curve. 2 = single cubic "
        "(arc/line). 3 = one cusp in the middle. 4 = two cusps.");
    curvz::utils::set_name(m_top_count, "dlg_wrp_top", "warp_dialog_top_count_spn");
    m_grid.attach(m_top_count, 1, row);
    ++row;

    m_grid.attach(*make_label("Bottom anchors:"), 0, row);
    m_bot_count.set_hexpand(true);
    m_bot_count.set_width_chars(4);
    m_bot_count.set_tooltip_text(
        "Number of anchors on the bottom envelope curve. Independent of "
        "top count — they don't have to match.");
    curvz::utils::set_name(m_bot_count, "dlg_wrp_bot", "warp_dialog_bot_count_spn");
    m_grid.attach(m_bot_count, 1, row);
    ++row;

    // Separator
    {
        auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep->set_margin_top(4);
        sep->set_margin_bottom(4);
        m_grid.attach(*sep, 0, row, 2, 1);
        ++row;
    }

    // Quality slider
    m_grid.attach(*make_label("Quality:"), 0, row);
    m_quality.set_hexpand(true);
    m_quality.set_draw_value(true);
    m_quality.set_value_pos(Gtk::PositionType::RIGHT);
    m_quality.set_digits(0);
    // Tick marks at 1, 4, 8, 16 for orientation
    m_quality.add_mark(1.0,  Gtk::PositionType::BOTTOM, "");
    m_quality.add_mark(4.0,  Gtk::PositionType::BOTTOM, "");
    m_quality.add_mark(8.0,  Gtk::PositionType::BOTTOM, "");
    m_quality.add_mark(16.0, Gtk::PositionType::BOTTOM, "");
    m_quality.set_tooltip_text(
        "Subdivision density. Each source cubic segment splits into this "
        "many warped cubics. Higher = smoother curves, slower rebuild. "
        "1 = fast preview, 4 = default, 16 = print quality.");
    curvz::utils::set_name(m_quality, "dlg_wrp_qty", "warp_dialog_quality_slider");
    m_grid.attach(m_quality, 1, row);
    ++row;

    m_outer.append(m_grid);

    // ── Button row ───────────────────────────────────────────────────────
    m_btn_row.set_spacing(8);
    m_btn_row.set_margin(12);
    m_btn_row.set_margin_top(0);
    m_btn_row.set_halign(Gtk::Align::END);
    m_btn_cancel.signal_clicked().connect([this]() { on_cancel(); });
    m_btn_ok.signal_clicked().connect([this]() { on_ok(); });
    m_btn_ok.add_css_class("suggested-action");
    curvz::utils::set_name(m_btn_cancel, "dlg_wrp_cnc", "warp_dialog_cancel_btn");
    curvz::utils::set_name(m_btn_ok, "dlg_wrp_ok", "warp_dialog_ok_btn");
    m_btn_row.append(m_btn_cancel);
    m_btn_row.append(m_btn_ok);
    m_outer.append(m_btn_row);

    set_child(m_outer);

    // Live updates — each control change pushes a Result through the
    // update callback so the canvas can retint in real time. All
    // handlers guard on m_loading to suppress firing during show() setup.
    m_preset.property_selected().signal_changed().connect([this]() {
        if (m_loading) return;
        emit_update();
    });
    m_top_count.signal_value_changed().connect([this]() {
        if (m_loading) return;
        emit_update();
    });
    m_bot_count.signal_value_changed().connect([this]() {
        if (m_loading) return;
        emit_update();
    });
    m_quality.signal_value_changed().connect([this]() {
        if (m_loading) return;
        emit_update();
    });

    // X button on window treated as Cancel (rather than silent hide)
    signal_close_request().connect([this]() {
        on_close_request_handled();
        return false;  // allow default close behaviour
    }, false);
}

void WarpDialog::show(Gtk::Window& parent,
                      const SceneNode* warp,
                      double glyph_bx, double glyph_by,
                      double glyph_bw, double glyph_bh,
                      UpdateCb update_cb,
                      ApplyCb  apply_cb,
                      CancelCb cancel_cb)
{
    m_update_cb = std::move(update_cb);
    m_apply_cb  = std::move(apply_cb);
    m_cancel_cb = std::move(cancel_cb);
    m_committed = false;

    m_bx = glyph_bx; m_by = glyph_by;
    m_bw = (glyph_bw > 1e-9) ? glyph_bw : 1.0;
    m_bh = (glyph_bh > 1e-9) ? glyph_bh : 1.0;

    m_loading = true;

    // Seed control values. If the Warp already has quality set, use it;
    // otherwise use the sticky last value. Anchor counts are inferred
    // from the existing envelope node counts (clamped to 2..4).
    int q = warp ? std::clamp(warp->warp_quality, 1, 16) : m_last_quality;
    m_quality.set_value((double)q);

    int top_n = 2, bot_n = 2;
    if (warp) {
        top_n = std::clamp((int)warp->warp_env_top.nodes.size(), 2, 4);
        bot_n = std::clamp((int)warp->warp_env_bottom.nodes.size(), 2, 4);
        // Empty envelopes (fresh Make) → default counts
        if (warp->warp_env_top.nodes.empty())    top_n = m_last_top_count;
        if (warp->warp_env_bottom.nodes.empty()) bot_n = m_last_bot_count;
    } else {
        top_n = m_last_top_count;
        bot_n = m_last_bot_count;
    }
    m_top_count.set_value((double)top_n);
    m_bot_count.set_value((double)bot_n);

    // Preset selection — always restore sticky value. Inferring preset
    // from an existing envelope's shape isn't reliably possible, so on
    // Edit Warp we default to the last-used preset. The moment the user
    // picks one in the dropdown we overwrite with its fresh shape,
    // which is what they'd want.
    m_preset.set_selected((guint)m_last_preset);

    m_loading = false;

    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);  // s117 m18 v2
    set_modal(false);  // canvas stays interactive for live preview
    present();

    // Immediately emit one update so Apply-without-touching-controls
    // produces the expected preset envelope (rather than whatever
    // default the Warp came in with).
    emit_update();
}

void WarpDialog::emit_update() {
    Result r;
    int top_n = std::clamp((int)m_top_count.get_value(), 2, 4);
    int bot_n = std::clamp((int)m_bot_count.get_value(), 2, 4);
    int preset_idx = (int)m_preset.get_selected();
    int quality = std::clamp((int)m_quality.get_value(), 1, 16);

    // Special case: Wave needs count >= 3 to actually wave. If user
    // picks Wave with count 2, auto-bump to 3 (matching the UX note
    // from the design). Guard against loading re-entry.
    if (preset_idx == PRESET_WAVE) {
        bool changed = false;
        if (top_n < 3) { top_n = 3; changed = true; }
        if (bot_n < 3) { bot_n = 3; changed = true; }
        if (changed) {
            bool was_loading = m_loading;
            m_loading = true;
            m_top_count.set_value((double)top_n);
            m_bot_count.set_value((double)bot_n);
            m_loading = was_loading;
        }
    }

    generate_preset(preset_idx, m_bx, m_by, m_bw, m_bh,
                    top_n, bot_n, r.env_top, r.env_bottom);
    r.quality = quality;

    // Remember for next show()
    m_last_top_count = top_n;
    m_last_bot_count = bot_n;
    m_last_preset    = preset_idx;
    m_last_quality   = quality;

    if (m_update_cb) m_update_cb(r);
}

void WarpDialog::on_ok() {
    // Build the final Result the same way emit_update does, then hand it
    // to the apply callback (which commits the EditWarpCommand /
    // MakeWarpCommand).
    Result r;
    int top_n = std::clamp((int)m_top_count.get_value(), 2, 4);
    int bot_n = std::clamp((int)m_bot_count.get_value(), 2, 4);
    int preset_idx = (int)m_preset.get_selected();
    int quality    = std::clamp((int)m_quality.get_value(), 1, 16);
    if (preset_idx == PRESET_WAVE) {
        if (top_n < 3) top_n = 3;
        if (bot_n < 3) bot_n = 3;
    }
    generate_preset(preset_idx, m_bx, m_by, m_bw, m_bh,
                    top_n, bot_n, r.env_top, r.env_bottom);
    r.quality = quality;

    m_last_top_count = top_n;
    m_last_bot_count = bot_n;
    m_last_preset    = preset_idx;
    m_last_quality   = quality;

    m_committed = true;
    hide();
    if (m_apply_cb) m_apply_cb(r);
}

void WarpDialog::on_cancel() {
    m_committed = false;
    hide();
    if (m_cancel_cb) m_cancel_cb();
}

void WarpDialog::on_close_request_handled() {
    // Window X button — treat as cancel iff not already committed by
    // on_ok's hide(). signal_close_request fires even when hide() is
    // called programmatically, so the committed flag prevents a spurious
    // cancel firing after apply.
    if (m_committed) return;
    if (m_cancel_cb) {
        auto cb = std::move(m_cancel_cb);
        m_cancel_cb = nullptr;  // prevent re-fire if show() reopens
        cb();
    }
}

} // namespace Curvz
