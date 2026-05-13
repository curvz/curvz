// scripting/PanelScriptable.hpp ──────────────────────────────────────────────
//
// s202 m1 — panel-as-Scriptable foundation.
//
// PanelScriptable is the abstract base for companion Scriptables that
// wrap a panel (StylesPanel, ThemesPanel, future SwatchesPanel /
// LibraryPanel / DocumentsPanel). Each concrete subclass holds a non-
// owning back-pointer to its panel; the panel constructs and owns the
// Scriptable as a member, so lifetime is tied automatically.
//
// Why a separate base from ScriptableWidget:
//
//   ScriptableWidget<GtkBase> templates over a single gtkmm widget and
//   binds that widget's canonical signal (signal_toggled, signal_changed)
//   to emit. A panel isn't a single widget; it's a container with many
//   internal widgets, and there's no single canonical signal that
//   represents "the panel did something." The Scriptable mixin's
//   contract is the right one — invoke / query / verbs / properties /
//   emit — but the canonical-signal binding doesn't apply.
//
//   So this class inherits Scriptable directly. Concrete subclasses
//   override invoke / query / verbs / properties; they call emit()
//   themselves at outcome points if they want subscribers to see panel
//   events (m1 doesn't emit; m2 might once kebab show/hide becomes
//   observable).
//
// "The script has no fingers" (CANON):
//
//   Each panel's Scriptable is part of the panel's interior. The panel
//   declares it as `friend` so the Scriptable can reach private state
//   (m_active_category, refresh(), action_create_empty()) directly —
//   the Scriptable is the panel's deliberate publication of outcomes,
//   not an outside consumer. Compare with the s201 m3 path: there, the
//   scripting layer reached past panel privacy via diagnostic
//   accessors on MainWindow. That's the anti-pattern this class
//   supplants — the consumer is now interior, registered by the panel
//   itself, addressing the panel's outcomes from inside the panel's
//   cohesion boundary.
//
// .demo-highlight CSS class:
//
//   The base provides highlight_widget(widget, ms) — adds the
//   .demo-highlight CSS class to the given widget and schedules a
//   Glib::signal_timeout to remove it after `ms` milliseconds.
//   Idempotent: calling twice in flight cancels the first timeout
//   and starts a fresh one (the most recent call wins). Used by every
//   subclass's `highlight_self` verb and, in m2, by per-element
//   highlight verbs like StylesPanel's `highlight_menuitem`.
//
//   The CSS class itself is defined in include/css.hpp's CURVZ_CSS
//   bundle alongside every other built-in rule.
//
// Diagnostic-only:
//
//   This header is included only from .cpp files compiled in the
//   CURVZ_DIAGNOSTIC build; production builds never see it. The
//   panel's member is held behind a #ifdef CURVZ_DIAGNOSTIC guard so
//   the panel header stays clean in production.

#pragma once
#include "scripting/Scriptable.hpp"

#include <chrono>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>

namespace curvz::scripting {

class PanelScriptable : public Scriptable {
public:
    using Scriptable::Scriptable;  // forward the name-required ctor
    ~PanelScriptable() override = default;

    // Note: the base Scriptable ctor doesn't validate the name string
    // (whitespace / quotes / reserved-keyword check); that lives in
    // ScriptableWidget::validated_script_name, which only widget
    // wrappers inherit. PanelScriptable subclasses today use literal
    // hardcoded names ("pnl_styles", future "pnl_themes" etc.) so the
    // gap is harmless. If a future subclass needs a dynamic name,
    // promote that validator to a free function in Scriptable.hpp and
    // call it explicitly from the subclass ctor.

    // Universal verbs shared by every panel-Scriptable subclass land
    // here over time. m1 ships only the highlight helper as a
    // protected utility (the `highlight_self` verb is per-subclass
    // because each chooses which widget to flash — the panel root,
    // the kebab, a section header). hide/show/visible would belong
    // here if we wanted them; m1 doesn't, because making a whole
    // panel disappear mid-script is more confusing than useful.

protected:
    // Add the .demo-highlight CSS class to `target`, then schedule its
    // removal after `ms` milliseconds. Calling again while a removal
    // is pending cancels the pending removal and starts fresh; the
    // class is removed once when the latest timeout fires.
    //
    // `target` must outlive the timeout. In practice every caller
    // passes a widget owned by the panel — and the panel owns the
    // Scriptable — so the lifetime hierarchy guarantees this. If a
    // future caller needs to highlight a transient widget (a popover
    // about to close, for instance), the call must complete before
    // the widget destructs; the timeout's lambda holds the raw
    // pointer and would dangle otherwise.
    //
    // Returns immediately; the highlight runs asynchronously on the
    // main loop.
    void highlight_widget(Gtk::Widget* target,
                          std::chrono::milliseconds ms);

    // s202 m2 — proactively cancel any in-flight highlight. Used when
    // the highlight target is about to be destroyed (e.g. a popover
    // hide that takes its model-button children with it). Disconnects
    // the pending timeout, removes the .demo-highlight class from the
    // target if the target is still alive, nulls the target pointer.
    // After this call the helper is in a clean state — a subsequent
    // highlight_widget on any target starts fresh.
    //
    // Safe to call when no highlight is in flight (cheap no-op:
    // checks connected() before doing anything).
    //
    // The `target_still_alive` parameter is the caller's promise that
    // m_highlight_target is safe to dereference. The defensive caller
    // (hide_kebab) passes false because by the time it knows the
    // popover is about to hide, the model buttons may already be
    // mid-teardown — touching them is unsafe. A "graceful" caller
    // that knows the target is still good can pass true and get the
    // CSS class cleanly removed; in practice "false" is the common
    // case.
    void cancel_highlight(bool target_still_alive = false);

private:
    sigc::connection m_highlight_timeout;
    Gtk::Widget*     m_highlight_target = nullptr;
};

} // namespace curvz::scripting
