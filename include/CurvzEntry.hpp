#pragma once
#include <gtkmm/entry.h>
#include <gtkmm/popover.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gdk/gdkkeysyms.h>
#include <functional>
#include <vector>

namespace Curvz {

// CurvzEntry — Gtk::Entry that treats Return as Tab (advance focus + commit).
//
// Problem: GTK4 Popovers intercept Enter before the entry sees it.
//          gtkmm4 doesn't wrap GtkEntry::activate as a sigc++ signal.
//
// Solution:
//   1. CAPTURE-phase key controller intercepts Return/KP_Enter.
//   2. Fires all on_commit() callbacks (value accepted).
//   3. Calls child_focus(TAB_FORWARD) — identical to Tab.
//   4. Pops down any enclosing Gtk::Popover.
//   5. Returns true to consume the event.
//
// Register commit actions with: entry.on_commit([](){ ... });
// Drop-in replacement for Gtk::Entry everywhere.

class CurvzEntry : public Gtk::Entry {
public:
    CurvzEntry() {
        // Disable GtkText's internal undo stack. GtkText (the underlying
        // text widget GtkEntry wraps) maintains a per-entry undo history
        // for typed input — Ctrl+Z while the entry has keyboard focus
        // pops a character off that history instead of propagating to
        // the window-level accelerator. CurvzEntry is for one-line
        // values that get committed atomically (Return/Tab/focus-out);
        // there's nothing to "undo" inside the entry itself, and
        // intercepting Ctrl+Z here would silently eat the user's first
        // application-level undo press after a commit. Disabling the
        // internal stack makes Ctrl+Z fall through to the window every
        // time, regardless of where focus currently sits.
        set_enable_undo(false);

        auto kc = Gtk::EventControllerKey::create();
        kc->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        kc->signal_key_pressed().connect(
            [this](guint keyval, guint, Gdk::ModifierType) -> bool {
                if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
                    // 1. Fire commit callbacks
                    for (auto& cb : m_callbacks) cb();
                    // 2. Move focus forward — same as Tab
                    child_focus(Gtk::DirectionType::TAB_FORWARD);
                    // 3. Dismiss any enclosing popover
                    if (auto* pop = dynamic_cast<Gtk::Popover*>(
                            get_ancestor(GTK_TYPE_POPOVER)))
                        pop->popdown();
                    return true; // consume event
                }
                if (keyval == GDK_KEY_Escape) {
                    m_cancelled = true;
                    child_focus(Gtk::DirectionType::TAB_FORWARD);
                    return true;
                }
                return false;
            }, false);
        add_controller(kc);

        // Focus-out (Tab, click elsewhere) also commits
        auto fc = Gtk::EventControllerFocus::create();
        fc->signal_leave().connect([this]() {
            if (!m_cancelled) {
                for (auto& cb : m_callbacks) cb();
            }
            m_cancelled = false;
        });
        add_controller(fc);
    }

    void on_commit(std::function<void()> cb) {
        m_callbacks.push_back(std::move(cb));
    }

private:
    std::vector<std::function<void()>> m_callbacks;
    bool m_cancelled = false;
};

} // namespace Curvz
