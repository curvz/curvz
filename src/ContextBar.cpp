#include "ContextBar.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s121 m7: curvz::utils::set_name
#include <gtkmm/gestureclick.h>

namespace Curvz {

// ── Tool metadata ─────────────────────────────────────────────────────────────
// `icon` is a unicode glyph used as a tiny visual marker on the left side of
// the bar. We deliberately don't promote this to a real curvz-*-symbolic
// Gtk::Image — the bar's left column stays compact, and the toolbar already
// shows the proper icon. Keep the glyph + name pair stable; the Toolbar's
// icons are the authoritative tool art.
//
// s132 m1: added Polygon and Spiral entries (previously fell through to "?").
struct ToolMeta { const char* icon; const char* name; };

static ToolMeta tool_meta(ActiveTool t) {
    switch (t) {
        case ActiveTool::Selection:  return {"▷",   "Select"};
        case ActiveTool::Node:       return {"◈",   "Node"};
        case ActiveTool::Pen:        return {"✒",   "Pen"};
        case ActiveTool::Rect:       return {"▭",   "Rectangle"};
        case ActiveTool::Ellipse:    return {"◯",   "Ellipse"};
        case ActiveTool::Line:       return {"╱",   "Line"};
        case ActiveTool::Polygon:    return {"⬠",   "Polygon"};       // s132 m1
        case ActiveTool::Spiral:     return {"◎",   "Spiral"};        // s132 m1
        case ActiveTool::Ref:        return {"⊕",   "Reference Point"};
        case ActiveTool::Text:       return {"T",   "Text"};
        case ActiveTool::TextOnPath: return {"A~",  "Text on Path"};
        case ActiveTool::Corner:     return {"⌐",   "Corner"};
        case ActiveTool::Eyedropper: return {"⊕",   "Eyedropper"};
        case ActiveTool::Zoom:       return {"⊙",   "Zoom"};
        case ActiveTool::Measure:      return {"⊢",   "Measure"};
        default:                     return {"?",   "Unknown"};
    }
}

// s132 m1: tool→manual leaf mapping. Each path is a gresource alias bundled
// at build time under /com/curvz/app/help/. HelpWindow::open_at() walks
// m_topics for the resource_path; if anything here goes stale (file
// renamed, leaf relocated) the right-click action falls back to a normal
// Help open with a log warning.
//
// Tools that have a dedicated leaf get one. Selection/Node share the
// 4.2 sub-section (4-2-1 / 4-2-2). Polygon/Spiral land on the new
// 4.4 shape leaves. Reference Point goes to 4.7. Corner to 4.8.
static const char* tool_help_resource(ActiveTool t) {
    switch (t) {
        case ActiveTool::Selection:  return "/com/curvz/app/help/4-2-1-selection-tool.md";
        case ActiveTool::Node:       return "/com/curvz/app/help/4-2-2-node-tool.md";
        case ActiveTool::Pen:        return "/com/curvz/app/help/4-3-pen.md";
        case ActiveTool::Rect:       return "/com/curvz/app/help/4-4-1-rectangle.md";
        case ActiveTool::Ellipse:    return "/com/curvz/app/help/4-4-2-ellipse.md";
        case ActiveTool::Line:       return "/com/curvz/app/help/4-4-3-line.md";
        case ActiveTool::Polygon:    return "/com/curvz/app/help/4-4-4-polygon.md";
        case ActiveTool::Spiral:     return "/com/curvz/app/help/4-4-5-spiral.md";
        case ActiveTool::Text:       return "/com/curvz/app/help/4-5-1-text.md";
        case ActiveTool::TextOnPath: return "/com/curvz/app/help/4-5-2-text-on-path.md";
        case ActiveTool::Eyedropper: return "/com/curvz/app/help/4-6-1-eyedropper.md";
        case ActiveTool::Zoom:       return "/com/curvz/app/help/4-6-2-zoom.md";
        case ActiveTool::Measure:      return "/com/curvz/app/help/4-6-3-measure.md";
        case ActiveTool::Ref:        return "/com/curvz/app/help/4-7-reference-points.md";
        case ActiveTool::Corner:     return "/com/curvz/app/help/4-8-corner.md";
        default:                     return "/com/curvz/app/help/4-1-tools-overview.md";
    }
}

// ── Constructor ───────────────────────────────────────────────────────────────
ContextBar::ContextBar() : Gtk::Box(Gtk::Orientation::HORIZONTAL) {
    curvz::utils::set_name(*this, "cb", "context_bar_root");
    add_css_class("context-bar");
    set_spacing(0);

    m_left.set_spacing(6);
    m_left.set_margin_start(10);
    m_left.set_margin_end(8);
    m_tool_icon.add_css_class("ctx-tool-icon");
    m_tool_name.add_css_class("ctx-tool-name");
    curvz::utils::set_name(m_tool_icon, "cb_ti", "context_bar_tool_icon_lbl");
    curvz::utils::set_name(m_tool_name, "cb_tn", "context_bar_tool_name_lbl");
    m_left.append(m_tool_icon);
    m_left.append(m_tool_name);
    append(m_left);

    auto* sep_l = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
    sep_l->set_margin_top(5);
    sep_l->set_margin_bottom(5);
    append(*sep_l);

    // s129 m9: m_centre_scroll wraps a horizontal Box of hint widgets. Per
    // tool, the inner Box's natural width is "as wide as all hints stacked
    // end-to-end" — which can exceed the bar width on narrow windows. We
    // want overflow to scroll horizontally, not push the bar wider or clip
    // silently.
    //
    // The fix is set_propagate_natural_width(false). By default GTK4's
    // ScrolledWindow propagates the child's natural width up the layout —
    // so the bar asks for the full content width and no scrolling
    // happens because everything fits. Turning propagation off lets the
    // ScrolledWindow take whatever width its parent grants (driven by
    // hexpand), and content beyond that scrolls.
    //
    // hexpand=true + AUTOMATIC horizontal policy means: scrollbar appears
    // only on overflow. overlay_scrolling=false keeps the scrollbar in
    // its own row of pixels rather than overlaid on hints (which would
    // obscure the last hint when the bar narrows).
    m_centre_scroll.set_hexpand(true);
    m_centre_scroll.set_propagate_natural_width(false);
    m_centre_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::NEVER);
    m_centre_scroll.set_overlay_scrolling(false);  // stable scrollbar row, no overlay
    m_centre.set_spacing(0);
    m_centre.set_margin_start(6);
    m_centre_scroll.set_child(m_centre);
    append(m_centre_scroll);

    // s132 m1: right-click anywhere on the bar opens the "Open Help"
    // popover for the active tool. Whole-bar coverage (rather than just
    // the tool icon) is intentional — a fresh user shouldn't have to
    // discover *which* corner of the bar carries the affordance.
    auto rc = Gtk::GestureClick::create();
    rc->set_button(GDK_BUTTON_SECONDARY);
    rc->signal_pressed().connect(
        [this](int /*n*/, double x, double y) { show_context_menu(x, y); });
    add_controller(rc);

    rebuild(ActiveTool::Selection);
    LOG_DEBUG("ContextBar created");
}

// ── Dynamic section helpers ───────────────────────────────────────────────────
void ContextBar::clear_dynamic() {
    for (auto* w : m_dynamic) m_centre.remove(*w);
    m_dynamic.clear();
}

Gtk::Label* ContextBar::add_hint(const std::string& text, const std::string& css) {
    auto* lbl = Gtk::make_managed<Gtk::Label>(text);
    lbl->add_css_class(css.c_str());
    lbl->set_margin_start(4);
    lbl->set_margin_end(2);
    m_centre.append(*lbl);
    m_dynamic.push_back(lbl);
    return lbl;
}

Gtk::Label* ContextBar::add_key(const std::string& key) {
    auto* lbl = Gtk::make_managed<Gtk::Label>(key);
    lbl->add_css_class("ctx-key");
    lbl->set_margin_start(3);
    lbl->set_margin_end(3);
    m_centre.append(*lbl);
    m_dynamic.push_back(lbl);
    return lbl;
}

Gtk::Separator* ContextBar::add_sep() {
    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
    sep->set_margin_start(8);
    sep->set_margin_end(8);
    sep->set_margin_top(5);
    sep->set_margin_bottom(5);
    m_centre.append(*sep);
    m_dynamic.push_back(sep);
    return sep;
}

Gtk::Button* ContextBar::add_btn(const std::string& icon, const std::string& tip,
                                  sigc::slot<void()> slot) {
    auto* btn = Gtk::make_managed<Gtk::Button>();
    btn->set_icon_name(icon.c_str());
    btn->set_tooltip_text(tip.c_str());
    btn->set_has_frame(false);
    btn->add_css_class("ctx-action-btn");
    btn->signal_clicked().connect(slot);
    m_centre.append(*btn);
    m_dynamic.push_back(btn);
    return btn;
}

Gtk::ToggleButton* ContextBar::add_toggle(const std::string& icon, const std::string& tip,
                                           bool initial, sigc::slot<void(bool)> slot) {
    auto* btn = Gtk::make_managed<Gtk::ToggleButton>();
    btn->set_icon_name(icon.c_str());
    btn->set_tooltip_text(tip.c_str());
    btn->set_has_frame(false);
    btn->set_active(initial);
    btn->add_css_class("ctx-action-btn");
    btn->signal_toggled().connect([btn, slot]() { slot(btn->get_active()); });
    m_centre.append(*btn);
    m_dynamic.push_back(btn);
    return btn;
}

// ── Right-click context menu (s132 m1) ────────────────────────────────────────
//
// One-item popover today — "Open Help: <Tool>" — but the structure (Box of
// flat Buttons inside a Popover) leaves room for future tool-scoped verbs
// without rewriting the controller. Mirrors the Canvas right-click popover
// idiom rather than the PopoverMenu+action-group style; matches the
// codebase convention for these compact context menus.
void ContextBar::show_context_menu(double x, double y) {
    auto meta = tool_meta(m_tool);
    auto* popover = Gtk::make_managed<Gtk::Popover>();
    popover->set_parent(*this);
    popover->set_has_arrow(true);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    box->set_spacing(2);
    box->set_margin_start(4);
    box->set_margin_end(4);
    box->set_margin_top(4);
    box->set_margin_bottom(4);

    auto* help_btn = Gtk::make_managed<Gtk::Button>();
    help_btn->set_has_frame(false);
    help_btn->set_halign(Gtk::Align::FILL);
    auto* help_lbl = Gtk::make_managed<Gtk::Label>(
        std::string("Open Help: ") + meta.name);
    help_lbl->set_xalign(0.0f);
    help_lbl->set_hexpand(true);
    help_btn->set_child(*help_lbl);

    const std::string path = tool_help_resource(m_tool);
    help_btn->signal_clicked().connect([this, popover, path]() {
        popover->popdown();
        if (m_help_cb) {
            m_help_cb(path);
        } else {
            LOG_WARN("ContextBar: help callback not wired; cannot open '{}'",
                      path);
        }
    });
    box->append(*help_btn);

    popover->set_child(*box);
    Gdk::Rectangle rect((int)x, (int)y, 1, 1);
    popover->set_pointing_to(rect);
    popover->popup();
}

// ── set_tool ──────────────────────────────────────────────────────────────────
void ContextBar::set_tool(ActiveTool tool) {
    m_tool = tool;
    rebuild(tool);
}

// ── rebuild ───────────────────────────────────────────────────────────────────
//
// s132 m1: hint content reviewed against ShortcutsDialog.cpp (the canonical
// mouse + modifier reference for each tool). Drift fixed:
//
//   • Selection — added clicked-selected→cycle-underneath, R toggle pivot,
//     scale/rotate/skew handle drags, Ctrl+Alt+click align anchor.
//     Nudge values corrected from "1 / 10" to "2 / 8 / 32 px".
//   • Node — added J join, B break, R reverse, Shift+Tab prev. Type-key
//     row kept (A/M/C/K) — those match current source.
//   • Pen — added "click open endpoint — continue existing path".
//   • Rect / Ellipse / Line — added right-click-toolbar precise-place hint.
//   • Polygon / Spiral — entirely new (previously fell through to default).
//   • Text — dropped "Phase 4 feature" leftover; added right-click-toolbar.
//   • Eyedropper — fixed Alt vs Shift modifier semantics: Alt swaps the
//     SAMPLE source (fill→stroke), Shift swaps the TARGET (apply to
//     stroke). Bar previously conflated Alt as the target swap.
//   • Reference Point — added precise-place hint.
//   • Zoom — added right-click-toolbar to set zoom level.
//
// Hints are intentionally terse: this is the quick-glance line, not the
// manual page. Right-click → Open Help drops the user on the full page.
void ContextBar::rebuild(ActiveTool tool) {
    clear_dynamic();
    auto meta = tool_meta(tool);
    m_tool_icon.set_text(meta.icon);
    m_tool_name.set_text(meta.name);

    switch (tool) {
    case ActiveTool::Selection:
        add_hint("Click — select  |  Click again — cycle underneath"); add_sep();
        add_key("Shift"); add_hint("+ click — add/remove from selection"); add_sep();
        add_hint("Drag empty — rubber-band  |  Drag handle — scale/rotate/skew"); add_sep();
        add_key("Alt"); add_hint("+ drag — duplicate & move"); add_sep();
        add_key("Ctrl"); add_key("Alt"); add_hint("+ click — toggle align anchor"); add_sep();
        add_key("R"); add_hint("— toggle pivot mode (with selection)"); add_sep();
        add_key("↑↓←→"); add_hint("nudge 2 px"); add_sep();
        add_key("Shift"); add_hint("+ nudge 8"); add_sep();
        add_key("Alt"); add_hint("+ nudge 32");
        break;

    case ActiveTool::Node:
        add_hint("Click node — select  |  Drag handle — adjust curve"); add_sep();
        add_key("Shift"); add_hint("+ click node — add/remove"); add_sep();
        add_key("Shift"); add_hint("+ click path — add whole path"); add_sep();
        add_key("Alt"); add_hint("+ drag handle — break symmetry"); add_sep();
        add_key("Tab"); add_hint("/"); add_key("Shift"); add_key("Tab");
        add_hint("— next/prev node"); add_sep();
        add_hint("Type:");
        add_key("A"); add_hint("Sym");
        add_key("M"); add_hint("Smooth");
        add_key("C"); add_hint("Cusp");
        add_key("K"); add_hint("Corner"); add_sep();
        add_key("J"); add_hint("join"); add_sep();
        add_key("B"); add_hint("break"); add_sep();
        add_key("R"); add_hint("reverse"); add_sep();
        add_key("↑↓←→"); add_hint("nudge 2 / 8 / 32 px (Shift / Alt)");
        break;

    case ActiveTool::Pen:
        add_hint("Click — anchor (sharp)  |  Click+drag — anchor with handles"); add_sep();
        add_key("Alt"); add_hint("+ drag handle — break symmetry"); add_sep();
        add_hint("Click first node — close path"); add_sep();
        add_hint("Click open endpoint — continue existing path"); add_sep();
        add_key("↵"); add_hint("commit  |  "); add_key("Esc"); add_hint("cancel");
        break;

    case ActiveTool::Rect:
        add_hint("Drag to draw"); add_sep();
        add_key("Shift"); add_hint("+ drag — square"); add_sep();
        add_key("Alt"); add_hint("+ drag — from centre"); add_sep();
        add_hint("Right-click toolbar — place precisely (popover)");
        break;

    case ActiveTool::Ellipse:
        add_hint("Drag to draw"); add_sep();
        add_key("Shift"); add_hint("+ drag — circle"); add_sep();
        add_key("Alt"); add_hint("+ drag — from centre"); add_sep();
        add_hint("Right-click toolbar — place precisely (popover)");
        break;

    case ActiveTool::Line:
        add_hint("Click+drag — line segment"); add_sep();
        add_key("Shift"); add_hint("+ drag — snap to 45° angles"); add_sep();
        add_key("↵"); add_hint("commit  |  "); add_key("Esc"); add_hint("cancel"); add_sep();
        add_hint("Right-click toolbar — place precisely (popover)");
        break;

    case ActiveTool::Polygon:
        // s132 m1: new entry. Drag = centre + radius + rotation; popover for
        // sides / inflection / star ratio.
        add_hint("Drag — centre, radius, rotation"); add_sep();
        add_key("Shift"); add_hint("+ drag — snap rotation to 15°"); add_sep();
        add_hint("Right-click toolbar — sides, inflection, star (popover)");
        break;

    case ActiveTool::Spiral:
        // s132 m1: new entry. Same drag flow as Polygon; turns + inner
        // radius from popover.
        add_hint("Drag — centre, outer radius, rotation"); add_sep();
        add_key("Shift"); add_hint("+ drag — snap rotation to 15°"); add_sep();
        add_hint("Right-click toolbar — turns, inner radius (popover)");
        break;

    case ActiveTool::Ref:
        add_hint("Click — place reference point"); add_sep();
        add_hint("Position precisely via X / Y in Properties"); add_sep();
        add_hint("Right-click toolbar — place precisely (popover)"); add_sep();
        add_hint("Reference points snap and align like anchors");
        break;

    case ActiveTool::Text:
        add_hint("Click — place text  |  Drag — place text box"); add_sep();
        add_key("↵"); add_hint("commit  |  "); add_key("Esc"); add_hint("cancel"); add_sep();
        add_hint("Right-click toolbar — text options (popover)");
        break;

    case ActiveTool::TextOnPath:
        add_hint("Click text node — select text"); add_sep();
        add_hint("Click path — link text to path"); add_sep();
        add_hint("Drag text node — adjust offset"); add_sep();
        add_hint("Right-click text node — detach from path");
        break;

    case ActiveTool::Eyedropper:
        // s132 m1: corrected modifier semantics. Alt swaps the SAMPLE
        // source (sample stroke instead of fill). Shift swaps the TARGET
        // (apply to stroke instead of fill). Combine for stroke→stroke.
        add_hint("Click — sample fill → apply to selection fill"); add_sep();
        add_key("Alt"); add_hint("+ click — sample stroke colour"); add_sep();
        add_key("Shift"); add_hint("+ click — apply sampled colour to stroke"); add_sep();
        add_key("Shift"); add_key("Alt"); add_hint("+ click — sample stroke → apply to stroke");
        break;

    case ActiveTool::Corner:
        add_hint("Click corner/cusp node to select"); add_sep();
        add_key("Shift"); add_hint("+ click — add/remove from selection"); add_sep();
        add_hint("Drag — rubber-band select"); add_sep();
        add_hint("Set type + radius in panel, then Apply");
        break;

    case ActiveTool::Zoom:
        add_hint("Click — zoom in 2×"); add_sep();
        add_key("Alt"); add_hint("+ click — zoom out 2×"); add_sep();
        add_key("Ctrl"); add_hint("+ click — fit to window"); add_sep();
        add_hint("Drag — marquee zoom in"); add_sep();
        add_key("Ctrl"); add_key("0"); add_hint("fit"); add_sep();
        add_key("Ctrl"); add_key("1"); add_hint("100%"); add_sep();
        add_hint("Right-click toolbar — set zoom level (popover)");
        break;

    case ActiveTool::Measure:
        add_hint("Click node — first point"); add_sep();
        add_key("Shift"); add_hint("+ click — second point"); add_sep();
        add_hint("Drag — marquee select 2 nodes"); add_sep();
        add_key("Space"); add_hint("clear  |  "); add_key("↵"); add_hint("place on layer"); add_sep();
        add_hint("Click label — copy  |  "); add_key("Alt"); add_hint("+ click — copy all");
        break;

    default:
        add_hint("No hints for this tool.");
        break;
    }
}

} // namespace Curvz
