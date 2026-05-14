#pragma once
//
// ColorPickerPopover — a reusable Gtk::Popover host for CurvzColorPicker.
//
// Lifetime model: one instance per caller widget, held as a member. The
// caller attaches it to a stable parent once (at panel construction),
// then calls open() as many times as the user needs. The popover is
// created lazily on first open and reused thereafter; dismiss is
// popdown(), never release.
//
// This mirrors the S64 handoff rule for Gtk::ColorDialog: "must be
// stored as a member RefPtr, not a local variable". Same principle,
// generalised to popovers.
//
// Dismissal model (S73 rewrite):
//
//   The CurvzColorPicker self-reverts in cancel(): restores m_current
//   to m_original, emits signal_changed (so the host's on_changed
//   callback writes the original colour back through), then emits
//   signal_cancelled. Hosts never need to discriminate cancel-vs-commit
//   at the live-edit level — every signal_changed is the new
//   authoritative live state.
//
//   Esc routing under autohide=true:
//     GTK's autohide intercepts Esc internally and calls popdown()
//     directly — no user-installed CAPTURE key controller on the
//     picker or popover sees the keystroke (verified S72 empirically),
//     and GtkPopover has no public Esc-specific signal. To detect Esc
//     specifically, we install a CAPTURE-phase key controller on the
//     toplevel window for the duration of each open() session. It
//     fires before autohide's internal binding and sets a per-session
//     m_was_cancelled flag, then lets the keystroke continue so
//     autohide still does the popdown. signal_closed reads the flag:
//     true → invoke m_picker->cancel() (which routes the original
//     colour through the host's on_changed) and report committed=false
//     to the ClosedFn; false → outside-click or explicit commit, do
//     not revert, report committed=true (or whatever the
//     signal_committed handler set).
//
//   For sessions that need cancel-vs-commit discrimination at
//   resolution time (e.g. push exactly one undo entry on commit, or
//   remove a freshly-created object on Esc), open() takes an optional
//   ClosedFn that fires once after popdown with a `committed` bool:
//   true if the session ended via Return / pick-recent / outside
//   click, false if it ended via Esc.
//
// Typical caller (s207 m2 singleton form):
//
//     // In a click handler — no member, no attach, just shared():
//     ColorPickerPopover::shared().open(
//         *dot_widget, initial_color, /*with_alpha=*/false,
//         [this, i](const color::Color& c) { apply_to_layer(i, c); });
//
// First call from anywhere in the app self-attaches the singleton to
// the toplevel Gtk::Window containing the anchor widget. Subsequent
// calls re-use the attached popover.
//
// open() returns immediately. The popover dismisses itself on click-
// outside (autohide), Escape (autohide → signal_closed → picker
// self-revert), hex-commit, or pick-recent. Between opens, the popover
// is hidden but all its widgets remain alive — re-opens reuse the same
// Cairo areas and signal wiring, only the callback binding and
// initial state are refreshed.
//

#include "color/Color.hpp"

#include <gtkmm/widget.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gdkmm/rectangle.h>
#include <glibmm/refptr.h>
#include <sigc++/connection.h>
#include <functional>
#include <string>

namespace Curvz {

class CurvzColorPicker;

// S85 cont-2: optional inline name entry above the picker. Off by
// default — every existing caller (toolbar wells, inspector paint
// editor, guide/grid/margin pickers, dialog paint editors) gets
// exactly today's UI. Hosts that ARE creating or editing a named
// entity (SwatchesPanel: new-swatch + edit-colour) opt in by passing
// ColorPickerNameField{enabled=true, initial_name=...} on open().
//
// Behaviour when enabled:
//   * The name entry sits at the top of the popover, above the
//     picker.
//   * Placeholder text is the live colour-region-name derived from
//     the picker's current colour (via color::region_name). Updates
//     on every signal_changed.
//   * `initial_name` populates the entry text on open. Empty means
//     "use the placeholder" — typical create flow.
//   * On Esc, the entry text is restored to `initial_name` (in
//     keeping with the picker's "Esc reverts everything" promise).
//   * On commit (Return / pick-recent / outside-click), the entry
//     text at popdown time is exposed via last_committed_name() so
//     the host can read it inside its ClosedFn. If the entry was
//     left blank, last_committed_name returns the derived name at
//     commit time, NOT an empty string — keeps the system's
//     auto-name-at-birth invariant in one place.
//
// Hosts that don't enable this field can ignore the new accessor;
// last_committed_name returns an empty string for non-name sessions.
//
// Lives at namespace scope (not nested inside ColorPickerPopover) so
// the default `name = {}` argument in open() can default-construct
// it without needing the enclosing class to be complete — nested
// types with default member initialisers can't be brace-defaulted
// in the enclosing class's own member declarations (verified
// empirically in the s85 cont-2 fix2 build break).
struct ColorPickerNameField {
    bool        enabled      = false;
    std::string initial_name = "";
};

class ColorPickerPopover {
public:
    using ChangedFn = std::function<void(const color::Color&)>;

    // Fires once per session, after the popover has popped down. The
    // `committed` bool reflects how the session ended:
    //   true  — Return / pick-recent / outside-click autohide
    //   false — Esc (picker self-reverted before this fires)
    // Used by hosts that need to push exactly one undo entry per
    // accepted session, or to remove a freshly-created object on Esc.
    // Hosts that just want live-preview semantics can leave this empty.
    //
    // Note: outside-click counts as committed. The picker's on_changed
    // callback already wrote whatever the live colour is through to
    // the host, and clicking outside expresses "I'm done, accept
    // what's showing" — only explicit Esc reverts.
    using ClosedFn = std::function<void(bool committed)>;

    // Convenience alias so call sites can still write
    // ColorPickerPopover::NameField{...} if they prefer.
    using NameField = ColorPickerNameField;

    ColorPickerPopover();
    ~ColorPickerPopover();

    // Not copyable, not movable — holds raw pointers to GTK widgets that
    // are parented to a specific stable_parent. Copying would duplicate
    // those raw pointers; moving would leave a half-initialised source.
    ColorPickerPopover(const ColorPickerPopover&)            = delete;
    ColorPickerPopover& operator=(const ColorPickerPopover&) = delete;
    ColorPickerPopover(ColorPickerPopover&&)                 = delete;
    ColorPickerPopover& operator=(ColorPickerPopover&&)      = delete;

    // s207 m1 — application-wide singleton instance.
    //
    // The original design held one ColorPickerPopover per host (Toolbar,
    // LayersPanel, PropertiesPanel, SwatchesPanel, plus 3 each in
    // StyleEditorDialog and 6 in ThemeEditDialog — 13 instances total),
    // each with its own attach() to a stable parent. That pattern was
    // pure boilerplate: the popover carries no per-host state between
    // open() calls (everything is per-call argument), and only one
    // popover can be visible at a time anyway. The 13 instances also
    // blocked substrate migration of the popover's internal name Entry
    // (each attach() tried to register the same `pop_cp_nm` abbrev;
    // the second registration would throw — see ledger Open Questions,
    // "Shared widget classes owned per-parent").
    //
    // Collapsing to a single shared instance dissolves both problems.
    // Hosts call shared().open(...) and don't carry a member. The
    // popover's substrate registration happens exactly once at first
    // attach.
    //
    // Heap-allocated, never freed. The single instance leaks at process
    // exit, which is fine — its GTK widgets are children of a Gtk::Window
    // that already gets destroyed during Gtk::Application shutdown. The
    // leak is one struct holding stale pointers; sigc::connection
    // disconnects on those stale pointers are no-ops because the slots
    // were already invalidated by the widget destruction.
    static ColorPickerPopover& shared();

    // Attach to a stable parent widget. Call exactly once, before any
    // open() call. The popover + picker widgets are constructed here
    // and parented to `stable_parent`. The popover lives until
    // `stable_parent` is destroyed.
    //
    // s207 m1: idempotent. Calling attach() after the popover is
    // already attached is a no-op (re-attach to a different parent is
    // not supported — the shared instance lives for the app lifetime
    // and re-parenting would invalidate the popover's coordinate space
    // mid-session).
    void attach(Gtk::Widget& stable_parent);

    // s207 m1 — idempotent self-attach helper for the shared singleton.
    //
    // If not already attached, walks `anywhere`'s widget tree to the
    // toplevel Gtk::Window and attaches the popover there. The toplevel
    // is the right parent because color picking is an application-wide
    // affordance and the popover's positioning math (set_pointing_to in
    // the parent's frame) works for any anchor widget anywhere inside
    // the same toplevel.
    //
    // Hosts that use the widget-anchor open() overload don't need to
    // call this — that overload calls ensure_attached() internally with
    // the anchor widget. Hosts that use the raw-rect open() overload
    // (Toolbar's fill/stroke wells, which compute floating-position
    // rects rather than anchoring to a real widget) MUST call
    // ensure_attached() with some live widget in the same toplevel
    // before their first open(), or open() will silently no-op.
    //
    // Safe to call repeatedly — second and subsequent calls are no-ops.
    void ensure_attached(Gtk::Widget& anywhere);

    // S85 m4i-cont-1: unparent the popover widget. For transient
    // hosts (e.g. StyleEditorDialog) whose stable_parent is itself
    // short-lived. Long-lived hosts (Toolbar / PropertiesPanel /
    // LayersPanel) never need this — GTK destroys the popover when
    // their parent dies anyway. But a transient host that closes
    // before its enclosing app does will see "Finalizing GtkWindow,
    // but it still has children left" warnings on the popover unless
    // it detaches first. Idempotent.
    void detach();

    // Open the popover pointing at `anchor_rect` (in the parent's
    // coordinate space). Wires `on_changed` as the live-apply callback,
    // replacing any callback from a previous open. Must be called after
    // attach().
    //
    // s207 m2 — single-visible-instance gate: if the popover is
    // currently visible from a prior open() call, popdown() runs first.
    // signal_closed fires synchronously, the previous session's
    // ClosedFn runs with committed=true (treating the new open() like
    // an outside-click on the old session — see existing autohide-
    // outside-click semantics). The new bindings install on a clean
    // popover. The invariant "at most one ColorPickerPopover session
    // visible at a time" is structurally enforced both by the
    // single-instance design and by this gate; together they make
    // multi-host scripting safe.
    //
    // `on_closed` (optional) fires once after the popover popsdown,
    // with `committed` true for non-Esc dismissals and false for Esc.
    // See ClosedFn doc above for the use cases.
    //
    // If `has_arrow` is false, the popover renders as a clean floating
    // panel with no pointer. Callers that anchor to a real UI element
    // (a swatch, a color dot) want the arrow for affordance; callers
    // that position the popover at an arbitrary canvas location (the
    // Toolbar fill/stroke wells) should pass false since the arrow has
    // nothing meaningful to point at.
    void open(const Gdk::Rectangle& anchor_rect,
              const color::Color& initial,
              bool with_alpha,
              ChangedFn on_changed,
              bool has_arrow = true,
              ClosedFn on_closed = {},
              ColorPickerNameField name = {});

    // Convenience overload. Uses `anchor`'s allocation translated into
    // the stable parent's coordinate space. Safe as long as `anchor`
    // outlives the call itself (we grab the rect synchronously here;
    // once the popover is up it doesn't re-read the anchor). Arrow is
    // always drawn in this overload — the anchor is a real widget the
    // arrow can point at.
    void open(Gtk::Widget& anchor,
              const color::Color& initial,
              bool with_alpha,
              ChangedFn on_changed,
              ClosedFn on_closed = {},
              ColorPickerNameField name = {});

    // S85 cont-2: read the name entry's value at popdown time. Only
    // meaningful when the most recent open() passed a NameField with
    // enabled=true — for non-name sessions, returns an empty string.
    // Intended to be called from inside the host's ClosedFn handler;
    // the value persists until the next open().
    //
    // If the user left the entry blank, this returns the derived
    // colour-region name at commit time (NOT empty), so the host can
    // safely use it directly to populate the swatch's display name.
    // For Esc-cancelled sessions (committed=false in ClosedFn), the
    // value is whatever the entry held immediately before popdown,
    // which the host will typically ignore anyway.
    const std::string& last_committed_name() const {
        return m_last_committed_name;
    }

    // Explicit dismiss. Normally the popover autohides on outside-click
    // or commits; callers rarely need this.
    void close();

    // s207 m2 v7 — am I currently showing? Used by open() as the
    // single state-machine gate: if open, ignore the new request.
    // Reads through to Gtk::Popover::get_visible(), so it's always
    // empirically accurate — no shadow flag to stay in sync with.
    bool is_open() const;

private:
    Gtk::Widget*      m_parent          = nullptr;
    Gtk::Widget*      m_popover_widget  = nullptr;   // really Gtk::Popover*
    CurvzColorPicker* m_picker          = nullptr;

    // The caller's current callbacks. Refreshed on every open(). Stored
    // here so we can disconnect prior bindings cleanly.
    ChangedFn         m_on_changed;
    ClosedFn          m_on_closed;
    sigc::connection  m_conn_changed;

    // Per-session "esc was pressed" flag. Reset to false at every
    // open(). Set to true by the window-level CAPTURE-phase key
    // controller (m_esc_controller, installed on the toplevel window
    // in open() and removed in signal_closed) when it sees an Esc
    // keystroke while the popover is showing. signal_closed reads
    // the flag: true means we should invoke m_picker->cancel() to
    // route the original colour through the host's on_changed, then
    // fire ClosedFn(committed=false). False means a non-Esc dismissal
    // (signal_committed via Return / pick-recent, or outside-click
    // autohide) — leave the live colour in place, fire ClosedFn(true).
    bool m_was_cancelled = false;

    // Window-level CAPTURE-phase key controller. Installed on the
    // toplevel window at open() time and removed in signal_closed.
    // Gives us a chance to see Esc before GTK's autohide-internal
    // binding swallows it (verified empirically in S72: picker-level
    // and popover-level CAPTURE controllers do NOT see Esc under
    // autohide=true; window-level CAPTURE does). The controller
    // handler sets m_was_cancelled=true on Esc and returns false so
    // autohide still proceeds with the popdown.
    Glib::RefPtr<Gtk::EventControllerKey> m_esc_controller;
    Gtk::Widget*                          m_esc_controller_target = nullptr;

    // S85 cont-2: name field state.
    //
    // Lazy-built widgets — null until the first open() call with a
    // NameField{enabled=true}. Both are children of m_picker_wrapper
    // (an internal VBox); m_picker_wrapper is the popover's set_child
    // target, with m_name_entry on top and the picker widget below.
    // When name is disabled for a given session, m_name_entry's
    // visibility is just toggled off; the wrapper still hosts both.
    //
    // Stored as raw Gtk::Widget* to avoid pulling Gtk::Entry / Gtk::Box
    // includes into this header; .cpp reinterpret_casts back. Same
    // type-erasure trick we use for m_popover_widget.
    Gtk::Widget* m_picker_wrapper       = nullptr; // really Gtk::Box*
    Gtk::Widget* m_name_entry           = nullptr; // really Gtk::Entry*

    // Per-session: name field config from the most recent open().
    // initial_name is restored on Esc (in keeping with the popover's
    // "Esc = revert everything" model). enabled drives visibility.
    bool        m_name_enabled       = false;
    std::string m_name_initial;

    // Per-session: tracks whether the user has typed in the name entry.
    // Set to true by signal_changed on the entry; reset to false at
    // every open(). When false, signal_changed from the picker keeps
    // updating the entry's placeholder live; when true, the user has
    // taken ownership of the field and we leave their text alone.
    //
    // Note: distinct from "entry text is non-empty". A blank entry that
    // the user has never touched still shows a live-updating placeholder
    // ("Vivid Red", "Cobalt Blue", etc.); a blank entry the user
    // explicitly cleared is treated as "user wants empty" and on commit
    // we fall back to the derived name (per the auto-name-at-birth
    // invariant — see header docs on NameField).
    bool m_name_user_typed = false;

    // Guard for the entry's signal_changed handler so that programmatic
    // set_text() calls (in open(), and on Esc-restore in signal_closed)
    // don't get treated as user typing. Set true around the relevant
    // set_text(), then false. The handler bails when this is true.
    bool m_name_setting_programmatically = false;

    // Set in signal_closed when committed. Holds either the user's
    // typed name OR (for blank-untouched commits) the derived
    // colour-region name at commit time. Cleared (empty) for non-name
    // sessions and at every open(). Read by hosts via the public
    // last_committed_name() accessor.
    std::string m_last_committed_name;

    // Connection for the picker's signal_changed → live-update of the
    // name entry's placeholder. Disconnected on signal_closed alongside
    // m_conn_changed. Distinct from m_conn_changed (host's on_changed)
    // because the placeholder hook needs to fire even when no host
    // callback is installed.
    sigc::connection m_conn_picker_changed_for_name;
};

} // namespace Curvz
