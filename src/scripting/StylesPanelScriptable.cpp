// scripting/StylesPanelScriptable.cpp ────────────────────────────────────────
//
// s202 m1 + m2 + m4 + m5. See header for design notes; this file is
// the small dispatch table and the per-verb implementation.
//
// m1 verbs: new_style, expand_category, highlight_self.
// m2 verbs: show_kebab, highlight_menuitem, hide_kebab.
// m4 verbs: expand_section, collapse_section (narrow flips on this
//           panel's section only).
// m5 verbs: focus_section (composite — collapse-all-then-open-chain,
//           the headline visual-narration move).
//
// Hosts PanelScriptable::highlight_widget and cancel_highlight too —
// the m-series ships only one PanelScriptable subclass, so the
// base's out-of-line methods ride along in this TU. When a later
// session adds a second PanelScriptable, the definitions graduate
// to a dedicated PanelScriptable.cpp; callers don't change because
// the declarations in the base header are stable.

#include "scripting/StylesPanelScriptable.hpp"
#include "StylesPanel.hpp"   // full panel header — private members reachable
                             // because StylesPanel declares us friend.

#include <chrono>
#include <glibmm/main.h>       // Glib::signal_timeout for the highlight pulse
#include <gtkmm/menubutton.h>  // s202 m2 — kebab popup/popdown
#include <gtkmm/popover.h>     // s202 m2 — popover walk

namespace curvz::scripting {

// ── PanelScriptable::highlight_widget ────────────────────────────────────────

void PanelScriptable::highlight_widget(Gtk::Widget* target,
                                       std::chrono::milliseconds ms) {
    if (!target) return;

    // Cancel any in-flight removal. If we were highlighting a
    // different widget, also clear that one's class first — otherwise
    // a rapid sequence of highlight_widget calls on different targets
    // would leave the older targets stuck with the class.
    if (m_highlight_timeout.connected()) {
        m_highlight_timeout.disconnect();
        if (m_highlight_target && m_highlight_target != target) {
            m_highlight_target->remove_css_class("demo-highlight");
        }
    }

    m_highlight_target = target;
    target->add_css_class("demo-highlight");

    m_highlight_timeout = Glib::signal_timeout().connect(
        [this]() {
            if (m_highlight_target) {
                m_highlight_target->remove_css_class("demo-highlight");
                m_highlight_target = nullptr;
            }
            return false;  // one-shot
        },
        static_cast<unsigned int>(ms.count()));
}

// s202 m2 — proactive cancel for the popover-teardown case. See header.
void PanelScriptable::cancel_highlight(bool target_still_alive) {
    if (!m_highlight_timeout.connected()) {
        // Nothing in flight, but null the target just in case a
        // previous timeout fired and we lost track. Cheap.
        m_highlight_target = nullptr;
        return;
    }
    m_highlight_timeout.disconnect();
    if (target_still_alive && m_highlight_target) {
        m_highlight_target->remove_css_class("demo-highlight");
    }
    m_highlight_target = nullptr;
}

// ── StylesPanelScriptable ────────────────────────────────────────────────────

StylesPanelScriptable::StylesPanelScriptable(Curvz::StylesPanel* panel)
    : PanelScriptable("pnl_styles"),
      m_panel(panel) {
    // Registry registration happens in the Scriptable base ctor.
    // Nothing else to wire — invoke() routes verbs directly through
    // m_panel via the friend declaration.
}

// Helper: pull an int from a ScriptValue, accepting Int and (defensively)
// Double. Used by highlight_self's optional duration arg.
static long long arg_as_int(const ScriptValue& v, long long fallback) {
    switch (v.kind) {
        case ValueKind::Int:    return v.i;
        case ValueKind::Double: return static_cast<long long>(v.d);
        default:                return fallback;
    }
}

// s202 m2 — popover tree walker.
//
// Gtk::PopoverMenu builds its widget tree from a Gio::MenuModel each
// time it pops up. The leaf widgets are GtkModelButton instances (a
// GTK-private widget class with no gtkmm binding) with their
// "action-name" property set to the menu item's action string. To
// find a specific menu item we recurse the widget tree looking for a
// model button whose action-name matches the target.
//
// The walk uses Gtk::Widget::get_first_child / get_next_sibling
// rather than the GObject-tree API — same shape the rest of the
// codebase uses for child traversal (see StylesPanel::refresh's
// header-tear-down loop). action-name is read via g_object_get on
// the raw GObject; gtkmm doesn't expose it on Gtk::Widget because
// it's a model-button property, not a generic widget property.
//
// Returns the first matching widget, or nullptr if none found.
static Gtk::Widget* find_model_button_by_action(Gtk::Widget* root,
                                                const std::string& want) {
    if (!root) return nullptr;

    // Check this widget. g_object_get on a widget that doesn't have
    // an action-name property is harmless — GObject sets *out = NULL
    // and emits a runtime warning. To stay quiet, only probe widgets
    // whose GType name suggests they could carry one (GtkModelButton,
    // GtkButton). For simplicity m2 probes everything and accepts the
    // occasional warning in the log when the property is absent;
    // an unconditional g_object_class_find_property pre-check would
    // be cleaner if the warning noise turns out to bother in practice.
    gchar* action = nullptr;
    GObjectClass* klass = G_OBJECT_GET_CLASS(root->gobj());
    if (g_object_class_find_property(klass, "action-name")) {
        g_object_get(root->gobj(), "action-name", &action, nullptr);
        if (action) {
            const bool match = (want == action);
            g_free(action);
            if (match) return root;
        }
    }

    // Recurse into children.
    for (auto* c = root->get_first_child(); c; c = c->get_next_sibling()) {
        if (auto* hit = find_model_button_by_action(c, want)) return hit;
    }
    return nullptr;
}

ScriptValue StylesPanelScriptable::invoke(std::string_view verb,
                                          const ScriptArgs& args) {
    if (!m_panel) return ScriptValue::null();

    if (verb == "new_style") {
        // The kebab's "+ New style" outcome. Calls the same private
        // method the Gio::SimpleAction's mem_fun bound to — friend
        // reach is what makes this clean: the script side never sees
        // the action group, the menu, or the popover.
        m_panel->action_create_empty();
        return ScriptValue::null();
    }

    if (verb == "expand_category") {
        if (args.empty()) return ScriptValue::null();
        if (args[0].kind != ValueKind::String) return ScriptValue::null();
        const std::string& name = args[0].s;
        // Walk the panel's category-order vector to resolve the tier.
        // m_category_order is built fresh on each refresh and holds
        // every (name, is_app_tier) pair the dropdown shows. On a hit,
        // call the setter with the matching tier — same path the
        // dropdown's user-driven change handler takes. On a miss,
        // no-op: the script's intent is "go there if it exists" and
        // set_active_category's stale-state fallback would land us
        // somewhere unrelated (typically "Built-in") which is worse
        // than silence.
        for (const auto& entry : m_panel->m_category_order) {
            if (entry.first == name) {
                m_panel->set_active_category(entry.first, entry.second);
                break;
            }
        }
        return ScriptValue::null();
    }

    if (verb == "highlight_self") {
        // Optional first arg: duration in milliseconds. Default 600
        // — long enough for a viewer to register the flash, short
        // enough to keep narrated scripts moving. Negative or zero
        // falls back to the default; a hostile script writer asking
        // for highlight_self -1 gets a sensible flash, not a never-
        // ending CSS class.
        long long ms = 600;
        if (!args.empty()) {
            long long supplied = arg_as_int(args[0], 600);
            if (supplied > 0) ms = supplied;
        }
        highlight_widget(m_panel, std::chrono::milliseconds(ms));
        return ScriptValue::null();
    }

    // ── s202 m2 — kebab + menuitem demonstration verbs ──────────────────

    if (verb == "show_kebab") {
        // Pop up the kebab MenuButton's popover. Idempotent: GTK's
        // popup() is safe to call when already popped (no-op on
        // second call). The popover's model-button children
        // construct after popup() returns and the main loop has a
        // chance to lay them out — highlight_menuitem handles that
        // timing via Glib::signal_idle.
        m_panel->m_btn_add.popup();
        return ScriptValue::null();
    }

    if (verb == "highlight_menuitem") {
        if (args.empty()) return ScriptValue::null();
        if (args[0].kind != ValueKind::String) return ScriptValue::null();
        const std::string suffix = args[0].s;

        auto* popover = m_panel->m_btn_add.get_popover();
        if (!popover) return ScriptValue::null();

        // Pre-pend the panel's action-group prefix so the script can
        // pass just the suffix ("create-empty") rather than the
        // fully-qualified name ("styles.create-empty"). The model
        // buttons carry the qualified form per Gio::Menu's append
        // contract (see StylesPanel::rebuild_kebab_menu).
        const std::string full = "styles." + suffix;

        // The popover's children may not have been laid out yet —
        // popup() queues the construction. Yield to the idle loop
        // once before walking so GTK has constructed the model
        // buttons. The walk + class-add happen in the idle callback;
        // the script's natural narration sleep (sleep 400 after
        // show_kebab in the canonical prologue) usually covers this
        // already, but the explicit idle dance makes the verb
        // robust even when the script omits the sleep.
        Glib::signal_idle().connect_once([this, popover, full]() {
            Gtk::Widget* hit = find_model_button_by_action(popover, full);
            if (!hit) return;
            // Pulse for 600ms — same default as highlight_self. The
            // lifetime defense: if hide_kebab fires before this
            // timeout completes, hide_kebab calls cancel_highlight
            // which nulls the target and disconnects the timeout
            // BEFORE the popover destroys the model button. The
            // raw pointer never gets dereferenced post-destruction.
            highlight_widget(hit, std::chrono::milliseconds(600));
        });
        return ScriptValue::null();
    }

    if (verb == "hide_kebab") {
        // Cancel any in-flight highlight first — the popover is
        // about to destroy its model-button children, and the
        // highlight timeout's lambda would dereference the dead
        // widget when it fires. cancel_highlight(false) disconnects
        // the timeout and nulls the target without touching the
        // (possibly already dying) widget. Calling cancel when
        // nothing's in flight is a cheap no-op.
        cancel_highlight(/*target_still_alive=*/false);
        m_panel->m_btn_add.popdown();
        return ScriptValue::null();
    }

    // ── s202 m4 — section state verbs ───────────────────────────────────

    if (verb == "expand_section") {
        // Invoke the apply-state closure MainWindow handed us via
        // set_section_state. Safe-no-op if the closure is empty
        // (the panel ran outside an inspector section, or
        // MainWindow forgot the wiring) — narrated scripts then
        // just don't get the pre-expand benefit, they don't crash.
        if (m_panel->m_section_apply) {
            m_panel->m_section_apply(true);
        }
        return ScriptValue::null();
    }

    if (verb == "collapse_section") {
        if (m_panel->m_section_apply) {
            m_panel->m_section_apply(false);
        }
        return ScriptValue::null();
    }

    // ── s202 m5 — composite focus verb ──────────────────────────────────

    if (verb == "focus_section") {
        // Hand the chain to the focus closure MainWindow installed
        // via set_section_chain. The closure handles the composite:
        // collapse every registered section, then walk the chain
        // open. Safe-no-op if the closure is empty (same convention
        // as expand_section / collapse_section above — narrated
        // scripts that run on a build without the wiring just don't
        // get the focus move; they don't crash).
        if (m_panel->m_section_focus) {
            m_panel->m_section_focus(m_panel->m_section_chain);
        }
        return ScriptValue::null();
    }

    return ScriptValue::null();
}

ScriptValue StylesPanelScriptable::query(std::string_view property) const {
    if (!m_panel) return ScriptValue::null();

    if (property == "active_category") {
        return ScriptValue::text(m_panel->active_category());
    }
    if (property == "active_is_app_tier") {
        return ScriptValue::boolean(m_panel->active_is_app_tier());
    }
    if (property == "kebab_visible") {
        // True if the kebab popover is currently popped up. Reads
        // through the MenuButton's get_active() — GTK ties the
        // button's :active property to the popover's visibility,
        // so this is the canonical "is the popover open" query
        // without reaching into the popover itself.
        return ScriptValue::boolean(m_panel->m_btn_add.get_active());
    }
    if (property == "section_open") {
        // s202 m4 — read the shared_ptr<bool> MainWindow handed us
        // via set_section_state. The flag is kept current by
        // make_section's click handler (user clicks the section
        // header → apply_state writes the flag) AND by our own
        // expand_section / collapse_section verbs (which invoke
        // the same apply_state through m_section_apply). False
        // when the flag pointer is null (unwired panel — narration
        // can still read the query, it just always answers false).
        if (m_panel->m_section_open_flag) {
            return ScriptValue::boolean(*m_panel->m_section_open_flag);
        }
        return ScriptValue::boolean(false);
    }
    // m1 queries are minimal — the verbs are the headline. Future
    // milestones add more (style count, currently-bound style, etc.)
    // as test scripts surface needs.
    //
    // No has_category query: expand_category is idempotent (no-op on
    // miss), so the test verifies presence indirectly via the post-
    // condition assert "after expand X, active_category == X". On a
    // miss the assert fails, which is the right test outcome anyway.
    // If a future use case needs "is this category in the list" as
    // its own observable, query() needs an args parameter — defer
    // until the second arg-bearing query surfaces.
    return ScriptValue::null();
}

std::vector<std::string> StylesPanelScriptable::verbs() const {
    return {
        "new_style",
        "expand_category",
        "highlight_self",
        // s202 m2
        "show_kebab",
        "highlight_menuitem",
        "hide_kebab",
        // s202 m4
        "expand_section",
        "collapse_section",
        // s202 m5
        "focus_section",
    };
}

std::vector<std::string> StylesPanelScriptable::properties() const {
    return {
        "active_category",
        "active_is_app_tier",
        "kebab_visible",   // s202 m2
        "section_open",    // s202 m4
    };
}

} // namespace curvz::scripting
