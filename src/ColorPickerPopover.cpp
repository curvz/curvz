//
// ColorPickerPopover.cpp — see header for lifetime discipline notes
// and dismissal-model documentation.
//

#include "ColorPickerPopover.hpp"
#include "CurvzColorPicker.hpp"
#include "color/ColorRegion.hpp"
#include "curvz_utils.hpp"  // s121 m14: curvz::utils::set_name
#include "curvz/widgets/Entry.hpp"  // s208 m4: substrate name entry

#include <gtkmm/popover.h>
#include <gtkmm/window.h>
#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gdk/gdkkeysyms.h>

namespace Curvz {

ColorPickerPopover::ColorPickerPopover() = default;

ColorPickerPopover::~ColorPickerPopover() {
    // We don't own the popover / picker — they're Gtk::make_managed<>
    // children of m_parent. GTK destroys them when the parent dies.
    // Disconnect our signal to avoid the (unlikely) firing into a
    // disappearing ChangedFn during shutdown ordering.
    if (m_conn_changed.connected()) m_conn_changed.disconnect();
    if (m_conn_picker_changed_for_name.connected())
        m_conn_picker_changed_for_name.disconnect();
}

// s207 m1 — singleton accessor. Heap-allocated, never freed. See
// header for the lifetime rationale.
ColorPickerPopover& ColorPickerPopover::shared() {
    static ColorPickerPopover* instance = new ColorPickerPopover();
    return *instance;
}

// s207 m1/v7 — idempotent self-attach helper for the shared singleton.
// Walks the supplied widget's tree to the toplevel Gtk::Window and
// attaches the popover there. If already attached to that toplevel,
// no-op. If attached to a DIFFERENT toplevel (e.g. the user opened a
// floating dialog and the picker needs to position relative to its
// window), re-parents to the new toplevel. The popover's internal
// child tree (wrapper, picker, name entry, signal wiring) survives
// the re-parent unchanged.
void ColorPickerPopover::ensure_attached(Gtk::Widget& anywhere) {
    auto* root = dynamic_cast<Gtk::Window*>(anywhere.get_root());
    if (!root) return;  // widget not yet in a window — caller should retry

    if (!m_popover_widget) {
        attach(*root);
        return;
    }

    if (m_parent == root) return;  // already on the right toplevel

    auto* pop = static_cast<Gtk::Popover*>(m_popover_widget);
    pop->unparent();
    pop->set_parent(*root);
    m_parent = root;
}

// s207 m2 v7 — empirical visibility query. Reads through to GTK; no
// shadow flag.
bool ColorPickerPopover::is_open() const {
    if (!m_popover_widget) return false;
    return static_cast<Gtk::Popover*>(m_popover_widget)->get_visible();
}

void ColorPickerPopover::attach(Gtk::Widget& stable_parent) {
    // Idempotent: callers like Toolbar attach from signal_realize, which
    // can fire again if the widget gets re-realized (hot theme changes,
    // etc.). Second call is a no-op — the existing popover + picker stay
    // parented to their original stable_parent.
    if (m_popover_widget) return;

    m_parent = &stable_parent;

    // Build the popover + picker once. Parent to the stable widget.
    // Mirrors Canvas.cpp / LayersPanel.cpp ad-hoc popover pattern,
    // with the one-instance-per-caller wrinkle.
    //
    // s207 m2 v9 — own a strong ref so re-parenting across toplevels
    // is safe. make_managed gives a floating ref that GTK sinks when
    // set_parent runs; thereafter the popover is owned by its parent.
    // unparent() releases that ownership and would finalize the
    // popover (and our m_popover_widget would dangle) unless we hold
    // our own ref. g_object_ref takes one and we never release it —
    // the popover lives for the app's lifetime, surviving any number
    // of unparent/set_parent cycles in ensure_attached.
    auto* pop = Gtk::make_managed<Gtk::Popover>();
    g_object_ref(G_OBJECT(pop->gobj()));
    curvz::utils::set_name(pop, "pop_cp", "color_picker_popover_root");
    pop->set_has_arrow(true);

    // S73: autohide=true. GTK4's autohide gives us outside-click
    // dismissal and Esc-popdown for free. The picker self-reverts in
    // cancel() (which the signal_closed handler below invokes
    // explicitly when our window-level Esc controller has flagged the
    // session as cancelled). The Esc controller is installed in open()
    // because it lives on the toplevel window, which we don't have
    // until open() time anyway.
    pop->set_autohide(true);
    pop->set_parent(stable_parent);

    // S85 cont-2: wrap the picker in a vertical Box so a name entry can
    // sit above it on opt-in sessions. Pre-cont-2 the popover's child
    // was the picker directly. The wrapper changes nothing for sessions
    // that don't enable the name field — m_name_entry is hidden, the
    // wrapper has zero overhead beyond a single GtkBox.
    auto* wrapper = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    wrapper->set_spacing(6);

    // Name entry, lazy-visible. Always built, visibility per-open.
    //
    // s208 m4: substrate-migrated. Pre-s207, this Entry was per-host (13
    // ColorPickerPopover instances each tried to register `pop_cp_nm` →
    // collision; substrate migration was deferred in s198 m4 for that
    // reason). s207 m2 collapsed the 13 instances into one app-lifetime
    // singleton; this build_once path runs exactly once, so the single
    // substrate Entry registration is unproblematic. Reading C question
    // (per-row / per-instance / shared-class addressability) remains
    // open for the *other* dynamic-widget categories — this site is
    // unblocked because the singleton resolves the per-instance multiplier.
    //
    // set_name kept (separately from the substrate ctor) for the GTK
    // widget-name (CSS hook); ScriptableWidget doesn't set the GTK name.
    auto* entry = Gtk::make_managed<curvz::widgets::Entry>("pop_cp_nm");
    curvz::utils::set_name(entry, "pop_cp_nm", "color_picker_popover_name_entry");
    entry->set_visible(false);
    entry->set_max_length(64);
    entry->add_css_class("curvz-popover-name-entry");
    wrapper->append(*entry);

    m_picker = Gtk::make_managed<CurvzColorPicker>();
    wrapper->append(*m_picker);
    pop->set_child(*wrapper);

    m_picker_wrapper = wrapper;
    m_name_entry     = entry;

    // Wire the name entry: track whether the user has typed so live
    // placeholder updates from the picker stop overwriting their input.
    // m_name_setting_programmatically guards against open()'s set_text
    // firing this handler during programmatic init.
    entry->signal_changed().connect([this]() {
        if (m_name_setting_programmatically) return;
        m_name_user_typed = true;
    });

    // Return-in-name-entry: forward to the picker's commit semantics so
    // the user can hit Return from anywhere in the popover and it means
    // "I'm done". CurvzColorPicker::signal_committed fires with the
    // current colour; the existing wiring in this class popsdown the
    // popover, which triggers signal_closed where we capture the name.
    entry->signal_activate().connect([this]() {
        if (m_picker) m_picker->commit();
    });

    // Picker → popover wiring. Connected once in attach().
    //
    // signal_committed fires from the picker on Return-in-hex or
    // pick-recent. The picker has already updated its m_current to
    // the committed value and emitted signal_changed (so the host's
    // on_changed wrote it through). Our job is to dismiss; the
    // signal_closed handler will see m_was_cancelled=false and fire
    // ClosedFn(true).
    m_picker->signal_committed().connect(
        [pop](const color::Color& /*c*/) {
            pop->popdown();
        });

    // signal_cancelled fires from the picker when CurvzColorPicker::cancel()
    // is called — currently only from the picker's own hex-entry CAPTURE
    // Esc controller (when entry has focus and intercepts Esc before
    // autohide does). The picker has self-reverted and emitted
    // signal_changed at the original colour. Mark the session cancelled
    // and dismiss; signal_closed will skip the redundant cancel() bridge
    // because the work is already done, but it still fires ClosedFn(false).
    m_picker->signal_cancelled().connect([this, pop]() {
        m_was_cancelled = true;
        pop->popdown();
    });

    // signal_closed fires once per popdown — invoked by every dismissal
    // path: explicit cancel/commit above, GTK's autohide (outside
    // click, Esc), or our explicit close(). Single resolution point.
    pop->signal_closed().connect([this]() {
        // Bridge Esc → picker.cancel() when our window-level controller
        // saw Esc but the picker's own Esc path didn't run (the common
        // case under autohide=true: hex entry doesn't have focus, so
        // the picker's CAPTURE controller never fires; the window-level
        // controller below caught it). Calling cancel() here writes
        // the original colour through the host's on_changed callback
        // before we fire ClosedFn(false), satisfying the architectural
        // promise that "Esc = swatch reverts".
        //
        // If the picker already self-reverted (signal_cancelled handler
        // above set m_was_cancelled=true), calling cancel() again is
        // idempotent: m_current is already m_original, so signal_changed
        // re-emits the original (host gets a redundant write of the same
        // value), signal_cancelled re-fires (handler is a no-op since
        // we're inside signal_closed and the popover is already down).
        if (m_was_cancelled && m_picker) {
            m_picker->cancel();
        }

        // S85 cont-2: capture the final name. For commit, prefer the
        // user's typed text; if blank, fall back to the derived name
        // at the picker's current colour (auto-name-at-birth invariant).
        // For Esc, set last_committed_name to empty — the host will
        // see committed=false and ignore it anyway. This also lets us
        // restore the entry text to m_name_initial so the next open()
        // starts clean.
        if (m_name_enabled && m_name_entry) {
            auto* entry = static_cast<Gtk::Entry*>(m_name_entry);
            if (m_was_cancelled) {
                m_last_committed_name.clear();
                // Defensive: restore the entry text in case user typed
                // before Esc. The next open() will set it again, but
                // restoring here keeps the widget in a clean state for
                // any debug inspection between sessions. Guarded so the
                // restore doesn't trip the "user typed" flag.
                m_name_setting_programmatically = true;
                entry->set_text(m_name_initial);
                m_name_setting_programmatically = false;
            } else {
                std::string typed = entry->get_text();
                if (!typed.empty()) {
                    m_last_committed_name = std::move(typed);
                } else {
                    // Blank-on-commit → derive from current colour.
                    color::Color c = m_picker ? m_picker->current()
                                              : color::Color::black();
                    m_last_committed_name = color::region_name(c);
                }
            }
        } else {
            m_last_committed_name.clear();
        }

        // Defensive: disconnect the on_changed binding. With picker-self-
        // revert, late on_changed fires would re-emit the original colour
        // and be no-ops, but a clean disconnect prevents subtle state
        // leaks if the picker is re-opened on an unrelated host while a
        // stale signal is pending.
        if (m_conn_changed.connected()) m_conn_changed.disconnect();
        if (m_conn_picker_changed_for_name.connected())
            m_conn_picker_changed_for_name.disconnect();

        // Remove the window-level Esc controller. add_controller takes
        // shared ownership; remove releases that share so a fresh
        // controller can be installed at the next open() without
        // duplicates.
        if (m_esc_controller && m_esc_controller_target) {
            m_esc_controller_target->remove_controller(m_esc_controller);
        }
        m_esc_controller.reset();
        m_esc_controller_target = nullptr;

        // Fire the closure callback exactly once, then clear so a
        // subsequent stray popdown can't fire it twice.
        if (m_on_closed) {
            ClosedFn fn = std::move(m_on_closed);
            m_on_closed = {};
            fn(/*committed=*/!m_was_cancelled);
        }
    });

    m_popover_widget = pop;
}

void ColorPickerPopover::open(const Gdk::Rectangle& anchor_rect,
                              const color::Color& initial,
                              bool with_alpha,
                              ChangedFn on_changed,
                              bool has_arrow,
                              ClosedFn on_closed,
                              ColorPickerNameField name) {
    if (!m_popover_widget || !m_picker) return;   // attach() not called

    if (is_open()) return;

    // Swap the live-apply callback. Disconnect the prior binding first
    // so the old caller (who may have owned a different layer index or
    // a dead document pointer) doesn't get re-fired.
    if (m_conn_changed.connected()) m_conn_changed.disconnect();
    m_on_changed = std::move(on_changed);
    m_conn_changed = m_picker->signal_changed().connect(
        [this](const color::Color& c) {
            if (m_on_changed) m_on_changed(c);
        });

    // Per-session state. Reset before installing controllers so any
    // stale value from a prior session can't leak in.
    m_on_closed = std::move(on_closed);
    m_was_cancelled = false;

    // ── S85 cont-2: Name field per-session config ──────────────────────────
    //
    // Set visibility, populate initial text, hook the live-placeholder
    // update from picker→entry, reset user-typed flag. When name field
    // is disabled, the entry is hidden and the placeholder hook is not
    // installed — non-name sessions see no overhead beyond the wrapper
    // VBox built once in attach().
    if (m_conn_picker_changed_for_name.connected())
        m_conn_picker_changed_for_name.disconnect();

    m_name_enabled         = name.enabled;
    m_name_initial         = std::move(name.initial_name);
    m_name_user_typed      = false;
    m_last_committed_name.clear();

    if (m_name_entry) {
        auto* entry = static_cast<Gtk::Entry*>(m_name_entry);
        entry->set_visible(m_name_enabled);

        if (m_name_enabled) {
            // Set initial text under the programmatic guard so the
            // signal_changed handler doesn't flip m_name_user_typed.
            m_name_setting_programmatically = true;
            entry->set_text(m_name_initial);
            m_name_setting_programmatically = false;

            // Populate placeholder from the initial colour. The
            // picker's signal_changed will keep this fresh as the
            // user drags.
            entry->set_placeholder_text(color::region_name(initial));

            // Live-update placeholder while user hasn't typed. Hooks
            // the picker's signal_changed; m_conn_changed (host's
            // on_changed binding) is independent and fires the host
            // callback in parallel. Both are disconnected on close.
            m_conn_picker_changed_for_name = m_picker->signal_changed()
                .connect([this](const color::Color& c) {
                    if (!m_name_enabled || m_name_user_typed) return;
                    if (!m_name_entry) return;
                    auto* e = static_cast<Gtk::Entry*>(m_name_entry);
                    e->set_placeholder_text(color::region_name(c));
                });
        }
    }

    m_picker->set_with_alpha(with_alpha);
    m_picker->set_initial(initial);

    auto* pop = static_cast<Gtk::Popover*>(m_popover_widget);
    // Arrow is per-open so the same shared popover can swap between
    // "anchored to a swatch" and "floating over the canvas" behaviour.
    pop->set_has_arrow(has_arrow);
    pop->set_pointing_to(anchor_rect);

    // Install the window-level CAPTURE-phase Esc controller for this
    // session. See header notes: autohide swallows Esc internally
    // before any picker-level or popover-level CAPTURE controller
    // sees it; the toplevel window is the only place we can intercept
    // it in time. The handler sets m_was_cancelled and returns false
    // so autohide's internal binding still proceeds with popdown.
    auto* root = dynamic_cast<Gtk::Window*>(
        m_parent ? m_parent->get_root() : nullptr);
    if (root) {
        m_esc_controller = Gtk::EventControllerKey::create();
        m_esc_controller->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        m_esc_controller->signal_key_pressed().connect(
            [this, pop](guint keyval, guint /*keycode*/, Gdk::ModifierType) -> bool {
                if (!pop->get_visible()) return false;
                if (keyval == GDK_KEY_Escape) {
                    m_was_cancelled = true;
                    // Return false: let autohide's internal Esc binding
                    // still fire and pop the popover down. We just want
                    // to flag the dismissal type, not consume the key.
                    return false;
                }
                return false;
            }, /*after=*/false);
        root->add_controller(m_esc_controller);
        m_esc_controller_target = root;
    }

    pop->popup();
}

void ColorPickerPopover::open(Gtk::Widget& anchor,
                              const color::Color& initial,
                              bool with_alpha,
                              ChangedFn on_changed,
                              ClosedFn on_closed,
                              ColorPickerNameField name) {
    if (is_open()) return;

    // Auto-attach (or re-parent if a different toplevel) on first use
    // from this anchor. Subsequent open()s on the same toplevel no-op
    // in ensure_attached.
    ensure_attached(anchor);

    if (!m_parent) return;

    Gdk::Rectangle rect(0, 0, 1, 1);
    double x = 0.0, y = 0.0;
    if (anchor.translate_coordinates(*m_parent, 0.0, 0.0, x, y)) {
        int w = anchor.get_width();
        int h = anchor.get_height();
        rect = Gdk::Rectangle(static_cast<int>(x), static_cast<int>(y),
                              w > 0 ? w : 1, h > 0 ? h : 1);
    }
    open(rect, initial, with_alpha,
         std::move(on_changed),
         /*has_arrow=*/true,
         std::move(on_closed),
         std::move(name));
}

void ColorPickerPopover::close() {
    if (!m_popover_widget) return;
    static_cast<Gtk::Popover*>(m_popover_widget)->popdown();
}

// S85 m4i-cont-1: idempotent unparent. After detach(), the popover
// widget is gone — open() would fail (m_popover_widget is null).
// In practice, this is called from a transient host's close path
// where no further open() is expected. The host destroys after.
void ColorPickerPopover::detach() {
    if (!m_popover_widget) return;
    auto* pop = static_cast<Gtk::Popover*>(m_popover_widget);
    // Disconnect first to avoid late-firing into a half-destroyed
    // ChangedFn during the unparent's destruction sequence. Same
    // defensive disconnect the dtor does.
    if (m_conn_changed.connected()) m_conn_changed.disconnect();
    if (m_conn_picker_changed_for_name.connected())
        m_conn_picker_changed_for_name.disconnect();
    pop->unparent();
    m_popover_widget = nullptr;
    m_picker         = nullptr;
    m_picker_wrapper = nullptr;
    m_name_entry     = nullptr;
    m_parent         = nullptr;
}

} // namespace Curvz
