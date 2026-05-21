#include "ContextBar.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s121 m7: curvz::utils::set_name
#include "widgets/Button.hpp"  // s209 m1: unregistered substrate Button
#include "widgets/ToggleButton.hpp"  // s209 m2: unregistered substrate ToggleButton
#include <gtkmm/gestureclick.h>
#include <gtkmm/popovermenu.h>  // s283 m5: PopoverMenu for the Open Other submenu
#include <giomm/simpleaction.h>      // s283 m5
#include <giomm/simpleactiongroup.h> // s283 m5
#include <giomm/menu.h>              // s283 m5
#include <glibmm/main.h>             // s283 m5: Glib::signal_idle for popover unparent
#include <iterator>                  // s283 m5: std::size for the entry array

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
    // s209 m1 — substrate Button with the unregistered tag.
    //
    // add_btn is called N times per tool-set rebuild; the per-tool
    // metadata in rebuild() declares its button affordances and this
    // helper instantiates them. A registered substrate Button here
    // would need an abbrev (`ctx_<tool>_<role>` or similar) AND the
    // registry would have to tolerate the old instance still being
    // alive at GTK4-deferred-destruction time when the new one
    // constructs — same shape as the s199 m1 PropertiesPanel
    // rebuild-collision problem.
    //
    // These buttons aren't useful as per-instance script targets (the
    // tool itself is addressable; the bar's affordances mirror the
    // tool's own verbs). Skipping registration is faithful to the
    // UX — the substrate type stays uniform with the rest of the
    // codebase, the script registry stays clean.
    //
    // Pilot site for the s209 unregistered_t pattern. The other two
    // raw sites in this file (add_toggle and show_context_menu's
    // help_btn) follow in s209 m2 once the pattern is banked.
    auto* btn = Gtk::make_managed<curvz::widgets::Button>(
                    curvz::scripting::unregistered);
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
    // s209 m2 — substrate ToggleButton with the unregistered tag.
    // Same rationale as add_btn's m1 migration: helper-multiplier
    // called N times per tool-set rebuild; per-instance script
    // addressability would force shared-abbrev collisions on tool
    // change. The tool itself is the addressable surface; the bar's
    // toggle affordances mirror the tool's verbs. Sibling of the
    // s209 m1 Button site above — three sites in this file now sit
    // on the same pattern.
    auto* btn = Gtk::make_managed<curvz::widgets::ToggleButton>(
                    curvz::scripting::unregistered);
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

// ── Right-click context menu (s132 m1, s283 m5) ───────────────────────────────
//
// Two-level menu. Outer level: "Open Help: <Tool>" for the active tool, plus
// an "Open Other" submenu containing every tool and major operation surface
// in Curvz (23 entries, alphabetised). Each leaf opens the corresponding
// manual page via m_help_cb.
//
// s283 m5: rationale for Open Other. Some tools are disabled in the
// active-tool Context bar (Pen, Ref, Corner, Eyedropper — tools whose
// Context bar shape was never written) and even the wired tools' bars
// don't always expose every affordance the tool offers. Right-clicking
// the Context bar gives a universal, always-available path to the
// manual page for any tool or workflow operation, regardless of whether
// its own Context bar exists or covers the verb the user wants.
// Sidesteps the disabled-Context-bar problem without touching the
// Context bar surface itself.
//
// Implementation idiom: Gtk::PopoverMenu driven by a Gio::Menu model,
// with an "open-other-<index>" SimpleAction per entry. Same shape as
// Canvas's right-click context menu (see build_object_context_menu in
// Canvas.cpp): Gio::Menu::create() → append_submenu for nesting → bind
// actions to a SimpleActionGroup inserted under a local scope.
//
// Lifetime: the popover is parented via set_parent(*this) which doesn't
// participate in managed-widget cleanup. Every right-click would leak a
// PopoverMenu against the long-lived ContextBar otherwise. Canvas's
// pattern: hook signal_closed and unparent from an idle, breaking the
// popdown→destroy ordering race. Same trick applied here — also closes
// the pre-existing leak from the s132 m1 implementation.
namespace {

// The combined tool + operation list, alphabetised by display label.
// Used by the "Open Other" submenu. Each entry pairs the label scripts
// see in the menu with the gresource path the help window opens. Keep
// alphabetised: if a future entry is added, slot it in the right spot
// rather than appending at the end.
struct HelpEntry { const char* label; const char* path; };

constexpr HelpEntry kOpenOtherEntries[] = {
    {"Align & Distribute",     "/com/curvz/app/help/7-4-align-distribute.md"},
    {"Blend",                  "/com/curvz/app/help/7-3-blends.md"},
    {"Boolean Operations",     "/com/curvz/app/help/8-2-boolean-operations.md"},
    {"Corner",                 "/com/curvz/app/help/4-8-corner.md"},
    {"Ellipse",                "/com/curvz/app/help/4-4-2-ellipse.md"},
    {"Eyedropper",             "/com/curvz/app/help/4-6-1-eyedropper.md"},
    {"Fill & Stroke",          "/com/curvz/app/help/5-4-5-appearance.md"},
    {"Line",                   "/com/curvz/app/help/4-4-3-line.md"},
    {"Macros",                 "/com/curvz/app/help/11-1-macros.md"},
    {"Measure",                "/com/curvz/app/help/4-6-3-measure.md"},
    {"Node",                   "/com/curvz/app/help/4-2-2-node-tool.md"},
    {"Pen",                    "/com/curvz/app/help/4-3-pen.md"},
    {"Polygon",                "/com/curvz/app/help/4-4-4-polygon.md"},
    {"Rectangle",              "/com/curvz/app/help/4-4-1-rectangle.md"},
    {"Reference Point",        "/com/curvz/app/help/4-7-reference-points.md"},
    {"Select",                 "/com/curvz/app/help/4-2-1-selection-tool.md"},
    {"Snap",                   "/com/curvz/app/help/5-3-6-snap.md"},
    {"Spiral",                 "/com/curvz/app/help/4-4-5-spiral.md"},
    {"Step and Repeat",        "/com/curvz/app/help/7-1-step-and-repeat.md"},
    {"Text",                   "/com/curvz/app/help/4-5-1-text.md"},
    {"Text on Path",           "/com/curvz/app/help/4-5-2-text-on-path.md"},
    {"Warp",                   "/com/curvz/app/help/7-6-warp.md"},
    {"Zoom",                   "/com/curvz/app/help/4-6-2-zoom.md"},
};

} // anon namespace

void ContextBar::show_context_menu(double x, double y) {
    auto meta = tool_meta(m_tool);
    const std::string active_path = tool_help_resource(m_tool);

    // ── Build the action group ────────────────────────────────────────────────
    //
    // Two action sources feed one SimpleActionGroup, inserted under the local
    // "ctxhelp" scope on the ContextBar widget. The menu items below reference
    // these by "ctxhelp.<name>".
    //
    //   1. "open-active" — opens the active tool's help page (same outcome
    //      the original s132 m1 button had).
    //   2. "open-other-N" (N = 0..22) — opens kOpenOtherEntries[N].path.
    auto ag = Gio::SimpleActionGroup::create();

    {
        auto act = Gio::SimpleAction::create("open-active");
        act->signal_activate().connect(
            [this, active_path](const Glib::VariantBase&) {
                if (m_help_cb) {
                    m_help_cb(active_path);
                } else {
                    LOG_WARN("ContextBar: help callback not wired; "
                             "cannot open '{}'", active_path);
                }
            });
        ag->add_action(act);
    }

    for (size_t i = 0; i < std::size(kOpenOtherEntries); ++i) {
        std::string name = "open-other-" + std::to_string(i);
        const char* path = kOpenOtherEntries[i].path;
        auto act = Gio::SimpleAction::create(name);
        act->signal_activate().connect(
            [this, path](const Glib::VariantBase&) {
                if (m_help_cb) {
                    m_help_cb(path);
                } else {
                    LOG_WARN("ContextBar: help callback not wired; "
                             "cannot open '{}'", path);
                }
            });
        ag->add_action(act);
    }

    insert_action_group("ctxhelp", ag);

    // ── Build the menu model ──────────────────────────────────────────────────
    //
    // Outer menu has two sections: the active-tool quick-access at the top,
    // and the "Open Other" submenu below. Sections separated by
    // append_section produce a visual divider in PopoverMenu.
    auto menu = Gio::Menu::create();

    // Section 1: active tool quick-access.
    {
        auto sec = Gio::Menu::create();
        sec->append(std::string("Open Help: ") + meta.name,
                    "ctxhelp.open-active");
        menu->append_section("", sec);
    }

    // Section 2: Open Other → submenu of all tools/operations.
    {
        auto sub = Gio::Menu::create();
        for (size_t i = 0; i < std::size(kOpenOtherEntries); ++i) {
            std::string action = "ctxhelp.open-other-" + std::to_string(i);
            sub->append(kOpenOtherEntries[i].label, action);
        }
        auto sec = Gio::Menu::create();
        sec->append_submenu("Open Other", sub);
        menu->append_section("", sec);
    }

    // ── Pop up the menu ───────────────────────────────────────────────────────
    auto* popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
    popover->set_parent(*this);
    popover->set_has_arrow(true);
    Gdk::Rectangle rect((int)x, (int)y, 1, 1);
    popover->set_pointing_to(rect);

    // Lifetime: set_parent() popovers don't participate in managed-widget
    // cleanup, so each right-click would leak its own PopoverMenu against
    // the long-lived ContextBar. Canvas.cpp's pattern (s125 m1a era):
    // hook signal_closed, unparent from an idle to break the popdown→
    // destroy ordering race. The popover survives until the idle runs
    // (Glib::signal_idle keeps managed widgets alive across the callback).
    popover->signal_closed().connect([popover]() {
        Glib::signal_idle().connect_once([popover]() { popover->unparent(); });
    });

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
