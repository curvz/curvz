// src/TranslateDialog.cpp ──────────────────────────────────────────────────
//
// s205 m4 — Translate hub dialog implementation.
// See header banner for the design rationale.

#include "TranslateDialog.hpp"

#include "Canvas.hpp"
#include "CommandHistory.hpp"
#include "CurvzDocument.hpp"  // CanvasModel
#include "CurvzProject.hpp"
#include "SceneNode.hpp"      // SceneNode, PathData, BezierNode
#include "curvz_utils.hpp"

#include <gtkmm/stringlist.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Curvz {

// ── Local helpers ────────────────────────────────────────────────────────────
namespace {

// Collect path leaves from a SceneNode subtree. Same shape as
// PropertiesPanel's inline collect_leaves — descends into children,
// clip_shape, blend sources/cache, warp source/glyph_cache/cache. Paths
// with non-empty nodes are the terminals.
void collect_leaves(SceneNode* n, std::vector<SceneNode*>& out) {
    if (!n) return;
    if (n->type == SceneNode::Type::Path && n->path && !n->path->nodes.empty()) {
        out.push_back(n);
        return;
    }
    for (auto& child : n->children)
        collect_leaves(child.get(), out);
    if (n->type == SceneNode::Type::ClipGroup && n->clip_shape)
        collect_leaves(n->clip_shape.get(), out);
    if (n->type == SceneNode::Type::Blend) {
        collect_leaves(n->blend_source_a.get(), out);
        collect_leaves(n->blend_source_b.get(), out);
        for (auto& step : n->blend_cache)
            collect_leaves(step.get(), out);
    }
    if (n->type == SceneNode::Type::Warp) {
        collect_leaves(n->warp_source.get(), out);
        collect_leaves(n->warp_glyph_cache.get(), out);
        collect_leaves(n->warp_cache.get(), out);
    }
}

// Forward-decl: defined inline in PropertiesPanel.cpp; we replicate the
// minimal subset here. The full version handles ellipses with cubic-
// curve sampling for accurate ovals; the simple node-extents version
// is good enough for the picker's bbox (it'll over-estimate slightly on
// curves but that's fine for placing presets visually). Match the
// PropertiesPanel version more exactly to keep behaviour identical?
// The expand_bbox_for_path in PropertiesPanel is `static` — file-local
// — so we can't link to it. We mirror the simpler node-only behaviour
// here. For Move/Scale/Rotate/Skew the pivot is a doc-space point, so
// any bbox approximation only affects preset accuracy, not transform
// correctness.
void expand_bbox_simple(const PathData& p,
                        double& minx, double& maxx,
                        double& miny, double& maxy) {
    for (const auto& n : p.nodes) {
        minx = std::min(minx, n.x);
        maxx = std::max(maxx, n.x);
        miny = std::min(miny, n.y);
        maxy = std::max(maxy, n.y);
        // Include control points so curvy shapes don't have presets
        // landing inside their visible silhouette.
        minx = std::min({minx, n.cx1, n.cx2});
        maxx = std::max({maxx, n.cx1, n.cx2});
        miny = std::min({miny, n.cy1, n.cy2});
        maxy = std::max({maxy, n.cy1, n.cy2});
    }
}

// Apply an affine pivoted transform fn(dx, dy) -> (dx', dy') to every
// node + handle of every leaf. Captures before-snapshots, then pushes
// one ScaleObjectsCommand for the batch. Mirrors PropertiesPanel's
// apply_xf, except we don't need the panel's coalescing inspector
// command (one Apply == one undo command in the dialog).
void apply_pivoted(const std::vector<SceneNode*>& leaves,
                   double px, double py,
                   std::function<std::pair<double,double>(double,double)> fn,
                   CurvzProject* project,
                   CommandHistory* history,
                   const std::string& desc) {
    if (leaves.empty() || !project || !history) return;

    std::vector<PathData> before;
    before.reserve(leaves.size());
    for (auto* l : leaves) before.push_back(*l->path);

    for (auto* l : leaves) {
        for (auto& n : l->path->nodes) {
            auto [nx,  ny ] = fn(n.x   - px, n.y   - py);
            auto [nx1, ny1] = fn(n.cx1 - px, n.cy1 - py);
            auto [nx2, ny2] = fn(n.cx2 - px, n.cy2 - py);
            n.x   = px + nx;   n.y   = py + ny;
            n.cx1 = px + nx1;  n.cy1 = py + ny1;
            n.cx2 = px + nx2;  n.cy2 = py + ny2;
        }
    }

    std::vector<ScaleObjectsCommand::LeafSnap> snaps;
    snaps.reserve(leaves.size());
    for (size_t i = 0; i < leaves.size(); ++i) {
        snaps.push_back({leaves[i]->internal_id, before[i], *leaves[i]->path});
    }
    history->push(std::make_unique<ScaleObjectsCommand>(
        project, std::move(snaps), desc));
}

} // namespace

// ── ctor ─────────────────────────────────────────────────────────────────────
TranslateDialog::TranslateDialog() {
    curvz::utils::set_name(*this, "dlg_xlt", "translate_dialog_root");
    set_title("Translate");
    set_resizable(false);
    set_default_size(360, -1);
    // Non-modal — the dialog is a workbench; user benefits from being
    // able to interact with canvas (drag pivot, move objects) while
    // applying repeated transforms.
    set_modal(false);
    // X-button closes by hiding (default would destroy, but this is a
    // MainWindow-owned member instance). Matches StyleEditorDialog's
    // s201 m1 pattern.
    set_hide_on_close(true);

    build_root();
}

// ── present() ────────────────────────────────────────────────────────────────
void TranslateDialog::present(Gtk::Window& parent,
                              Canvas* canvas,
                              CommandHistory* history,
                              CurvzProject* project,
                              const CanvasModel* model,
                              double ruler_origin_x,
                              double ruler_origin_y) {
    m_canvas  = canvas;
    m_history = history;
    m_project = project;

    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);  // s117 m18 idiom

    // Rebuild picker + Move row spinners against the new (possibly
    // different) CanvasModel so units & ruler origin reflect the active
    // doc. Cheap; replaces the row's children in place.
    rebuild_picker(model, ruler_origin_x, ruler_origin_y);
    m_active_model = model;

    // Refresh picker bbox + seed pivot from current canvas state.
    refresh_picker_bbox();

    Gtk::Window::present();
}

// ── build_root ───────────────────────────────────────────────────────────────
void TranslateDialog::build_root() {
    m_root.set_margin(16);
    m_root.set_spacing(8);

    build_picker_row();
    build_action_row();
    build_param_stack();
    build_button_row();

    m_root.append(m_picker_row);
    m_picker_sep.set_margin_top(4);
    m_picker_sep.set_margin_bottom(4);
    m_root.append(m_picker_sep);
    m_root.append(m_action_row);
    m_root.append(m_param_stack);
    m_btn_sep.set_margin_top(8);
    m_root.append(m_btn_sep);
    m_root.append(m_btn_row);

    set_child(m_root);
}

void TranslateDialog::build_picker_row() {
    m_picker_row.set_hexpand(true);
    m_picker_row.set_spacing(8);
    // Children populated per-present() via rebuild_picker.
}

void TranslateDialog::build_action_row() {
    m_action_row.set_hexpand(true);
    m_action_row.set_spacing(8);

    auto* lbl = Gtk::make_managed<Gtk::Label>("Action:");
    lbl->add_css_class("prop-lbl");
    lbl->set_xalign(0.0f);
    m_action_row.append(*lbl);

    auto verbs = Gtk::StringList::create({"Move", "Scale", "Rotate", "Skew"});
    m_verb_drop.set_model(verbs);
    m_verb_drop.set_selected(0);  // Move default
    m_verb_drop.set_hexpand(true);
    curvz::utils::set_name(m_verb_drop, "dlg_xlt_verb",
                           "translate_dialog_verb_dd");
    m_verb_drop.property_selected().signal_changed().connect([this]() {
        // Switch stack page to match the dropdown selection. Page names
        // are set in build_param_stack().
        auto sel = m_verb_drop.get_selected();
        const char* page = "move";
        switch (sel) {
            case 0: page = "move";   break;
            case 1: page = "scale";  break;
            case 2: page = "rotate"; break;
            case 3: page = "skew";   break;
            default: break;
        }
        m_param_stack.set_visible_child(page);
    });
    m_action_row.append(m_verb_drop);
}

void TranslateDialog::build_param_stack() {
    m_param_stack.set_hexpand(true);
    m_param_stack.set_transition_type(Gtk::StackTransitionType::NONE);

    m_move_row   = build_move_row();
    m_scale_row  = build_scale_row();
    m_rotate_row = build_rotate_row();
    m_skew_row   = build_skew_row();

    m_param_stack.add(*m_move_row,   "move");
    m_param_stack.add(*m_scale_row,  "scale");
    m_param_stack.add(*m_rotate_row, "rotate");
    m_param_stack.add(*m_skew_row,   "skew");
    m_param_stack.set_visible_child("move");
}

// ── Param rows ───────────────────────────────────────────────────────────────
Gtk::Box* TranslateDialog::build_move_row() {
    // Empty placeholder row — rebuild_picker populates X/Y labels +
    // spinners per-present() because the spinners are model-bound. We
    // don't seed any children here to avoid orphaning them when
    // rebuild_picker tears the row down.
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_hexpand(true);
    return row;
}

Gtk::Box* TranslateDialog::build_scale_row() {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    row->set_hexpand(true);

    auto* lbl_x = Gtk::make_managed<Gtk::Label>("SX:");
    lbl_x->add_css_class("prop-lbl");
    row->append(*lbl_x);

    auto adj_sx = Gtk::Adjustment::create(100.0, 0.1, 10000.0, 0.1, 10.0);
    m_scale_sx_spin = Gtk::make_managed<Gtk::SpinButton>(adj_sx, 0.1, 1);
    m_scale_sx_spin->set_width_chars(8);
    m_scale_sx_spin->add_css_class("prop-width-entry");
    m_scale_sx_spin->add_css_class("node-spin");
    curvz::utils::set_name(m_scale_sx_spin, "dlg_xlt_sx",
                           "translate_dialog_scale_x_spn");
    row->append(*m_scale_sx_spin);

    auto* unit_x = Gtk::make_managed<Gtk::Label>("%");
    unit_x->add_css_class("prop-width-unit");
    row->append(*unit_x);

    // Link toggle — same monochrome dim/bright style as the inspector
    m_scale_link_btn = Gtk::make_managed<Gtk::ToggleButton>();
    m_scale_link_btn->set_active(m_scale_linked);
    m_scale_link_btn->set_tooltip_text("Link X/Y scale");
    m_scale_link_btn->add_css_class("flat");
    m_scale_link_btn->set_has_frame(false);
    m_scale_link_btn->set_icon_name("curvz-link-symbolic");
    m_scale_link_btn->set_opacity(m_scale_linked ? 1.0 : 0.3);
    m_scale_link_btn->signal_toggled().connect([this]() {
        m_scale_linked = m_scale_link_btn->get_active();
        m_scale_link_btn->set_opacity(m_scale_linked ? 1.0 : 0.3);
    });
    curvz::utils::set_name(m_scale_link_btn, "dlg_xlt_sln",
                           "translate_dialog_scale_link_toggle");
    row->append(*m_scale_link_btn);

    auto* lbl_y = Gtk::make_managed<Gtk::Label>("SY:");
    lbl_y->add_css_class("prop-lbl");
    row->append(*lbl_y);

    auto adj_sy = Gtk::Adjustment::create(100.0, 0.1, 10000.0, 0.1, 10.0);
    m_scale_sy_spin = Gtk::make_managed<Gtk::SpinButton>(adj_sy, 0.1, 1);
    m_scale_sy_spin->set_width_chars(8);
    m_scale_sy_spin->add_css_class("prop-width-entry");
    m_scale_sy_spin->add_css_class("node-spin");
    curvz::utils::set_name(m_scale_sy_spin, "dlg_xlt_sy",
                           "translate_dialog_scale_y_spn");
    row->append(*m_scale_sy_spin);

    auto* unit_y = Gtk::make_managed<Gtk::Label>("%");
    unit_y->add_css_class("prop-width-unit");
    row->append(*unit_y);

    // Linked X/Y cross-update — same idiom as inspector Scale row.
    adj_sx->signal_value_changed().connect([this, adj_sx, adj_sy]() {
        if (!m_scale_linked) return;
        if (std::abs(adj_sy->get_value() - adj_sx->get_value()) < 1e-9) return;
        adj_sy->set_value(adj_sx->get_value());
    });
    adj_sy->signal_value_changed().connect([this, adj_sx, adj_sy]() {
        if (!m_scale_linked) return;
        if (std::abs(adj_sx->get_value() - adj_sy->get_value()) < 1e-9) return;
        adj_sx->set_value(adj_sy->get_value());
    });

    return row;
}

Gtk::Box* TranslateDialog::build_rotate_row() {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    row->set_hexpand(true);

    auto* lbl = Gtk::make_managed<Gtk::Label>("Angle:");
    lbl->add_css_class("prop-lbl");
    row->append(*lbl);

    auto adj_a = Gtk::Adjustment::create(0.0, -360.0, 360.0, 0.1, 10.0);
    m_rotate_a_spin = Gtk::make_managed<Gtk::SpinButton>(adj_a, 0.1, 3);
    m_rotate_a_spin->set_width_chars(10);
    m_rotate_a_spin->add_css_class("prop-width-entry");
    m_rotate_a_spin->add_css_class("node-spin");
    curvz::utils::set_name(m_rotate_a_spin, "dlg_xlt_ra",
                           "translate_dialog_rotate_a_spn");
    row->append(*m_rotate_a_spin);

    auto* unit = Gtk::make_managed<Gtk::Label>("°");
    unit->add_css_class("prop-width-unit");
    row->append(*unit);

    return row;
}

Gtk::Box* TranslateDialog::build_skew_row() {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    row->set_hexpand(true);

    auto* lbl = Gtk::make_managed<Gtk::Label>("Axis:");
    lbl->add_css_class("prop-lbl");
    row->append(*lbl);

    // Grouped toggle pair: H / V. Default H. Mutually exclusive — at
    // least one must remain active. The `prop-toggle` CSS class
    // (css.hpp lines 540-564) is the compact-pill style with a clear
    // :checked rule painting accent-bg + accent-hover text, so the
    // active toggle visibly stands out. min-height 17px and font-size
    // 10px keep the row tight; min-width 0 lets the buttons size to
    // their 1-char content rather than GTK's default button width.
    m_skew_h_btn = Gtk::make_managed<Gtk::ToggleButton>("H");
    m_skew_v_btn = Gtk::make_managed<Gtk::ToggleButton>("V");
    m_skew_h_btn->set_active(true);
    m_skew_h_btn->add_css_class("prop-toggle");
    m_skew_v_btn->add_css_class("prop-toggle");
    curvz::utils::set_name(m_skew_h_btn, "dlg_xlt_kh",
                           "translate_dialog_skew_h_toggle");
    curvz::utils::set_name(m_skew_v_btn, "dlg_xlt_kv",
                           "translate_dialog_skew_v_toggle");

    // Mutual exclusion: when one goes on, the other goes off. At least
    // one must remain active — clicking the active one is a no-op.
    m_skew_h_btn->signal_toggled().connect([this]() {
        if (m_skew_h_btn->get_active()) {
            if (m_skew_v_btn->get_active()) m_skew_v_btn->set_active(false);
        } else if (!m_skew_v_btn->get_active()) {
            m_skew_h_btn->set_active(true);  // re-assert (don't allow none)
        }
    });
    m_skew_v_btn->signal_toggled().connect([this]() {
        if (m_skew_v_btn->get_active()) {
            if (m_skew_h_btn->get_active()) m_skew_h_btn->set_active(false);
        } else if (!m_skew_h_btn->get_active()) {
            m_skew_v_btn->set_active(true);
        }
    });

    row->append(*m_skew_h_btn);
    row->append(*m_skew_v_btn);

    auto* lbl_a = Gtk::make_managed<Gtk::Label>("Angle:");
    lbl_a->add_css_class("prop-lbl");
    lbl_a->set_margin_start(12);
    row->append(*lbl_a);

    auto adj_a = Gtk::Adjustment::create(0.0, -89.0, 89.0, 0.1, 5.0);
    m_skew_a_spin = Gtk::make_managed<Gtk::SpinButton>(adj_a, 0.1, 3);
    m_skew_a_spin->set_width_chars(10);
    m_skew_a_spin->add_css_class("prop-width-entry");
    m_skew_a_spin->add_css_class("node-spin");
    curvz::utils::set_name(m_skew_a_spin, "dlg_xlt_ka",
                           "translate_dialog_skew_a_spn");
    row->append(*m_skew_a_spin);

    auto* unit = Gtk::make_managed<Gtk::Label>("°");
    unit->add_css_class("prop-width-unit");
    row->append(*unit);

    return row;
}

void TranslateDialog::build_button_row() {
    m_btn_row.set_spacing(8);
    m_btn_row.set_halign(Gtk::Align::END);
    curvz::utils::set_name(m_btn_close, "dlg_xlt_cls",
                           "translate_dialog_close_btn");
    curvz::utils::set_name(m_btn_apply, "dlg_xlt_app",
                           "translate_dialog_apply_btn");
    m_btn_apply.add_css_class("suggested-action");
    m_btn_row.append(m_btn_close);
    m_btn_row.append(m_btn_apply);

    m_btn_close.signal_clicked().connect([this]() { hide(); });
    m_btn_apply.signal_clicked().connect(
        [this]() { apply_current_verb(); });
}

// ── rebuild_picker ───────────────────────────────────────────────────────────
// Replace the picker (and the Move-row spinners that need a CanvasModel*)
// against the new active model. Same idiom as OffsetPathDialog's
// build_distance_spin — tear down children of the picker row in place
// rather than maintain a parallel state machine across doc switches.
//
// Wires the picker's signal_point_changed listener every time so the
// new picker actually feeds the canvas pivot. The previous picker
// (managed widget) goes away with its connections when the row's
// children are removed.
void TranslateDialog::rebuild_picker(const CanvasModel* model,
                                     double ruler_origin_x,
                                     double ruler_origin_y) {
    // s199 m1 idiom — synchronously unregister Scriptable children
    // before remove(). The old picker registered as
    // "refpoint_picker.translate"; without this synchronous cleanup,
    // GTK4's idle-priority destruction would leave the registry entry
    // alive past the moment the new picker tries to register, and the
    // registry's throw-on-duplicate enforcement would crash on the
    // second present(). force_unregister_subtree is null-tolerant and
    // recursive, so calling it on the row itself covers the picker
    // (a Scriptable composite) plus any future Scriptable children.
    curvz::utils::force_unregister_subtree(&m_picker_row);

    // Tear down picker row's children
    while (auto* c = m_picker_row.get_first_child()) {
        m_picker_row.remove(*c);
    }
    m_picker = nullptr;

    auto* pivot_lbl = Gtk::make_managed<Gtk::Label>("Pivot:");
    pivot_lbl->add_css_class("prop-lbl");
    pivot_lbl->set_valign(Gtk::Align::CENTER);
    m_picker_row.append(*pivot_lbl);

    const double rox = ruler_origin_x;
    const double roy = ruler_origin_y;

    m_picker = Gtk::make_managed<curvz::widgets::RefPointPicker>(
        "refpoint_picker.translate", model, rox, roy);
    m_picker->set_hexpand(true);

    // Listener: every picker emit writes through to Canvas's pivot
    // state, which in turn emits signal_pivot_changed (which the
    // inspector picker hears and which we also hear, see present-time
    // sync below). The hide() guard avoids spurious writes when the
    // dialog is closed but the picker still emits during teardown.
    m_picker->signal_point_changed().connect(
        [this](double px, double py) {
            if (!is_visible()) return;
            if (!m_canvas) return;
            m_canvas->set_custom_pivot(px, py);
        });

    m_picker_row.append(*m_picker);

    // Rebuild Move row's X/Y spinners too — they need the same model
    // for unit display and ruler origins. Tear down existing Move row
    // children EXCEPT the static labels (those are managed inside the
    // row Box; we identify "labels we built" by leaving them in place
    // and rebuilding spinners between them).
    //
    // Simpler approach: clear the row entirely and rebuild from scratch.
    if (m_move_row) {
        while (auto* c = m_move_row->get_first_child()) {
            m_move_row->remove(*c);
        }
        auto* lbl_x = Gtk::make_managed<Gtk::Label>("X:");
        lbl_x->add_css_class("prop-lbl");
        m_move_row->append(*lbl_x);

        m_move_dx_spin = Gtk::make_managed<CurvzSpinButton>(
            SpinType::PositionX, model, rox);
        m_move_dx_spin->with_width_chars(8);
        m_move_dx_spin->add_css_class("prop-width-entry");
        m_move_dx_spin->add_css_class("node-spin");
        curvz::utils::set_name(m_move_dx_spin, "dlg_xlt_mx",
                               "translate_dialog_move_x_spn");
        m_move_row->append(*m_move_dx_spin);
        if (auto* u = m_move_dx_spin->get_unit_label()) m_move_row->append(*u);

        auto* lbl_y = Gtk::make_managed<Gtk::Label>("Y:");
        lbl_y->add_css_class("prop-lbl");
        lbl_y->set_margin_start(12);
        m_move_row->append(*lbl_y);

        m_move_dy_spin = Gtk::make_managed<CurvzSpinButton>(
            SpinType::PositionY, model, roy);
        m_move_dy_spin->with_width_chars(8);
        m_move_dy_spin->add_css_class("prop-width-entry");
        m_move_dy_spin->add_css_class("node-spin");
        curvz::utils::set_name(m_move_dy_spin, "dlg_xlt_my",
                               "translate_dialog_move_y_spn");
        m_move_row->append(*m_move_dy_spin);
        if (auto* u = m_move_dy_spin->get_unit_label()) m_move_row->append(*u);
    }
}

// ── refresh_picker_bbox ──────────────────────────────────────────────────────
// Pull the current selection's union bbox and push into the picker so
// preset points re-evaluate. Also seed the picker's display from the
// canvas pivot state — if canvas has a custom pivot, the picker shows
// it (silently, no mode flip, matching sync_selected_pivot's m2 fix).
void TranslateDialog::refresh_picker_bbox() {
    if (!m_picker || !m_canvas) return;

    std::vector<SceneNode*> leaves;
    for (auto* obj : m_canvas->selection())
        collect_leaves(obj, leaves);
    if (leaves.empty()) return;

    double minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18;
    for (auto* l : leaves) {
        if (l->path) expand_bbox_simple(*l->path, minx, maxx, miny, maxy);
    }
    if (minx > 1e17) return;

    m_picker->set_bbox(minx, miny, maxx - minx, maxy - miny);

    // Seed picker display from canvas pivot, mirroring the inspector
    // picker's m2 behaviour. Mode is left untouched — user gets to
    // pick Preset vs Arbitrary, the dialog just keeps the numbers
    // fresh underneath.
    if (m_canvas->has_custom_pivot()) {
        m_picker->update_arbitrary_xy_silent(
            m_canvas->custom_pivot_x(),
            m_canvas->custom_pivot_y());
    }

    // Refresh Move row's destination spinners to current refpt position
    // so the initial state is "no move" — matching the inspector idiom
    // of "fields show identity, edit to change". current_pivot() gives
    // us the bbox-centre fallback when no custom pivot is active.
    double px, py;
    if (current_pivot(px, py)) {
        if (m_move_dx_spin) m_move_dx_spin->set_internal_value(px);
        if (m_move_dy_spin) m_move_dy_spin->set_internal_value(py);
    }
}

// ── current_pivot ────────────────────────────────────────────────────────────
bool TranslateDialog::current_pivot(double& out_px, double& out_py) {
    if (!m_canvas) return false;
    if (m_canvas->has_custom_pivot()) {
        out_px = m_canvas->custom_pivot_x();
        out_py = m_canvas->custom_pivot_y();
        return true;
    }
    // No custom pivot — fall back to bbox centre of current selection.
    std::vector<SceneNode*> leaves;
    for (auto* obj : m_canvas->selection())
        collect_leaves(obj, leaves);
    if (leaves.empty()) return false;

    double minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18;
    for (auto* l : leaves) {
        if (l->path) expand_bbox_simple(*l->path, minx, maxx, miny, maxy);
    }
    if (minx > 1e17) return false;
    out_px = (minx + maxx) * 0.5;
    out_py = (miny + maxy) * 0.5;
    return true;
}

// ── apply_current_verb ───────────────────────────────────────────────────────
// Dispatch to the verb-specific do_* based on dropdown selection.
void TranslateDialog::apply_current_verb() {
    if (!m_canvas || !m_history || !m_project) return;

    auto sel = m_verb_drop.get_selected();
    switch (sel) {
        case 0: do_move();   break;
        case 1: do_scale();  break;
        case 2: do_rotate(); break;
        case 3: do_skew();   break;
        default: return;
    }

    // After applying, re-collect leaves & refresh picker bbox so the
    // next Apply operates against the geometry-after-transform. The
    // pivot itself doesn't change here (Scale/Rotate/Skew leave pivot
    // alone; Move shifts geometry but pivot stays put in doc-space
    // unless the user re-clicks a preset).
    refresh_picker_bbox();

    // Notify canvas of the doc-tree mutation so layers/gallery/blends
    // refresh, mirroring what the inspector's emit_prop_changed does.
    m_canvas->notify_document_changed();
}

// ── do_move ──────────────────────────────────────────────────────────────────
// Move semantics: anchor (= current refpt) moves to destination (= user
// dx/dy entries). Translation vector = destination - anchor. Applied
// uniformly to every path-node + handle.
void TranslateDialog::do_move() {
    double px, py;
    if (!current_pivot(px, py)) return;
    if (!m_move_dx_spin || !m_move_dy_spin) return;

    const double dest_x = m_move_dx_spin->get_internal_value();
    const double dest_y = m_move_dy_spin->get_internal_value();
    const double tx = dest_x - px;
    const double ty = dest_y - py;
    if (std::abs(tx) < 1e-9 && std::abs(ty) < 1e-9) return;

    std::vector<SceneNode*> leaves;
    for (auto* obj : m_canvas->selection())
        collect_leaves(obj, leaves);
    if (leaves.empty()) return;

    // Translate is independent of pivot — fn ignores its dx/dy frame.
    // Use apply_pivoted with px=py=0 and a fn that returns (x+tx, y+ty)?
    // That breaks the pivot-relative contract. Simpler: a direct inline
    // translation loop here, avoiding the apply_pivoted helper which is
    // specifically for pivoted ops.
    std::vector<PathData> before;
    before.reserve(leaves.size());
    for (auto* l : leaves) before.push_back(*l->path);

    for (auto* l : leaves) {
        for (auto& n : l->path->nodes) {
            n.x   += tx;  n.y   += ty;
            n.cx1 += tx;  n.cy1 += ty;
            n.cx2 += tx;  n.cy2 += ty;
        }
    }

    std::vector<ScaleObjectsCommand::LeafSnap> snaps;
    snaps.reserve(leaves.size());
    for (size_t i = 0; i < leaves.size(); ++i) {
        snaps.push_back({leaves[i]->internal_id, before[i], *leaves[i]->path});
    }
    m_history->push(std::make_unique<ScaleObjectsCommand>(
        m_project, std::move(snaps), "Move object"));

    // Update canvas pivot too — the anchor has now followed the geometry
    // to (dest_x, dest_y), so the picker's idea of "where the anchor is"
    // should match. set_custom_pivot will fire pivot_changed which
    // propagates to inspector picker via the existing m1 sync wiring.
    m_canvas->set_custom_pivot(dest_x, dest_y);
}

// ── do_scale ─────────────────────────────────────────────────────────────────
// Percent scale around pivot. SX/SY in percent, 100 = identity.
void TranslateDialog::do_scale() {
    double px, py;
    if (!current_pivot(px, py)) return;
    if (!m_scale_sx_spin || !m_scale_sy_spin) return;

    const double sx = m_scale_sx_spin->get_value() / 100.0;
    const double sy = m_scale_sy_spin->get_value() / 100.0;
    if (sx < 0.001 || sy < 0.001) return;
    if (std::abs(sx - 1.0) < 1e-9 && std::abs(sy - 1.0) < 1e-9) return;

    std::vector<SceneNode*> leaves;
    for (auto* obj : m_canvas->selection())
        collect_leaves(obj, leaves);

    apply_pivoted(leaves, px, py,
                  [sx, sy](double dx, double dy) {
                      return std::make_pair(dx * sx, dy * sy);
                  },
                  m_project, m_history, "Scale object");

    // Reset SX/SY to identity so next Apply starts neutral.
    m_scale_sx_spin->set_value(100.0);
    m_scale_sy_spin->set_value(100.0);
}

// ── do_rotate ────────────────────────────────────────────────────────────────
// Rotate around pivot by signed degrees (Y-down convention matches the
// inspector's apply_rotate, m3).
void TranslateDialog::do_rotate() {
    double px, py;
    if (!current_pivot(px, py)) return;
    if (!m_rotate_a_spin) return;

    const double angle_deg = m_rotate_a_spin->get_value();
    if (std::abs(angle_deg) < 1e-9) return;

    const double rad = -angle_deg * M_PI / 180.0;
    const double c = std::cos(rad), s = std::sin(rad);

    std::vector<SceneNode*> leaves;
    for (auto* obj : m_canvas->selection())
        collect_leaves(obj, leaves);

    apply_pivoted(leaves, px, py,
                  [c, s](double dx, double dy) {
                      return std::make_pair(dx * c - dy * s, dx * s + dy * c);
                  },
                  m_project, m_history, "Rotate object");

    m_rotate_a_spin->set_value(0.0);
}

// ── do_skew ──────────────────────────────────────────────────────────────────
// Horizontal or vertical shear around pivot. Tan(angle_deg) is the shear
// factor — same parametrisation the inspector Skew row uses.
void TranslateDialog::do_skew() {
    double px, py;
    if (!current_pivot(px, py)) return;
    if (!m_skew_a_spin || !m_skew_h_btn) return;

    const double angle_deg = m_skew_a_spin->get_value();
    if (std::abs(angle_deg) < 1e-9) return;
    const bool horizontal = m_skew_h_btn->get_active();
    const double k = std::tan(angle_deg * M_PI / 180.0);

    std::vector<SceneNode*> leaves;
    for (auto* obj : m_canvas->selection())
        collect_leaves(obj, leaves);

    apply_pivoted(leaves, px, py,
                  [k, horizontal](double dx, double dy) {
                      if (horizontal)
                          return std::make_pair(dx + k * dy, dy);
                      return std::make_pair(dx, dy + k * dx);
                  },
                  m_project, m_history,
                  horizontal ? "Skew H" : "Skew V");

    m_skew_a_spin->set_value(0.0);
}

} // namespace Curvz
