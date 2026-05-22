// animation/MotionPump.hpp ───────────────────────────────────────────────────
//
// s288 m1 — OPENS the welcome-demo animation arc. The "final brushstroke"
// of Curvz (see HANDOFF.md s287→s288, "Banked for end-of-Curvz") is a
// scripted construction animation that builds the Scott-bug avatar (or
// any user-supplied SVG) anchor-by-anchor in front of the user on first
// launch. Every visible beat is a real Curvz document mutation through
// the existing model surface; nothing is faked at the render layer.
//
// MotionPump is the load-bearing primitive: a `Glib::Timeout`-driven
// value-interpolator that drives a `void(double t)` callback from t=0
// to t=1 over a fixed duration. The callback mutates whatever model
// state the caller cares about (a BezierNode handle position, an
// anchor count, an opacity, a transform parameter) and triggers the
// canvas refresh cascade. Per-tick: direct mutation, no command
// pushed, no undo entry — the welcome demo is a PRESENTATION, not an
// edit, and shouldn't pollute the user's history stack.
//
// Self-holding lifetime: the pump's start() method captures a
// shared_ptr to itself into the timeout closure. The caller can drop
// its reference immediately after start() returns; the pump stays
// alive until t=1 (or external cancel), then releases its self-ref,
// and the timeout chain terminates naturally. This shape lets a
// transient like ObjectProxy::invoke spin up an animation that
// survives the proxy's own destruction at end-of-statement.
//
// One pump = one animation. Multiple simultaneous pumps coexist
// without coordination — each owns its own timeout and writes its own
// model fields per tick. Last-writer-wins per BezierNode field if two
// pumps target the same field at the same time; the welcome script's
// choreography is sequential by construction so this isn't observable
// in the design case. A future milestone may add a registry for
// cancel-by-iid or block-on-complete if the orchestrator needs it.
//
// ── Tick rate ───────────────────────────────────────────────────────────────
//
// 60 fps target (16ms tick interval). The pump computes the
// fractional t for each tick from the elapsed wall-clock since
// start(), not from a tick counter — so a janky main loop (a long
// canvas redraw on a slow machine) still lands at the correct t when
// the timeout fires next. The visible result on a stutter is a
// momentarily-skipped intermediate frame, not a slowed animation.
// Duration is the contract; smoothness is best-effort.
//
// The pump's last tick is always exactly t=1.0 — a special-case at
// the end of start()'s callback ensures the model lands at its final
// state regardless of timeout granularity. Without this, a 200ms
// duration with 16ms ticks would land at t≈0.992 (12 ticks * 16ms /
// 200ms) and never quite reach the target.
//
// ── Easing ──────────────────────────────────────────────────────────────────
//
// m1 ships linear easing only. The callback receives t as the raw
// elapsed-fraction (0.0 to 1.0). m2+ will add a pluggable easing
// function (probably an std::function<double(double)> passed to
// start() with a linear default), so anchor placement can ease-in
// and handle extension can ease-out without changing the pump's
// shape. The choice to defer easing to m2 is scope discipline — m1
// is "does the timeout/mutate/redraw loop actually work end-to-end?"
// and easing is orthogonal to that question.
//
// ── Cancellation ────────────────────────────────────────────────────────────
//
// m1 does NOT ship cancellation. Once start() is called the pump
// runs to t=1. This is sufficient for the welcome demo's design
// case (sequential animations, no interruption). m2+ may add a
// cancel() method or a CancellationToken pattern when an orchestrator
// needs to stop an animation mid-flight (e.g. the user closes the
// document during the welcome).
//
// ── Why a separate subsystem ────────────────────────────────────────────────
//
// Three reasons to put MotionPump in `animation/` rather than folding
// it into ObjectsScriptable or Canvas:
//
// 1. The primitive is general — it'll be used by every animation
//    beat of the welcome (handle extension, anchor add, segment
//    growth, view-mode crossfade, fade-delete). One subsystem, many
//    consumers. Reuses the same shape every time.
//
// 2. It's testable in isolation — the callback shape is a pure
//    `void(double)`, the pump has no dependency on SceneNode /
//    PathData / Canvas. A future unit-test surface could exercise
//    duration accuracy and t=1 landing without spinning up a project.
//    (No such unit test in m1 scope; the convergent-evidence smoke is
//    the m1 acceptance.)
//
// 3. It's a forward seed — the next subsystem in the welcome arc
//    (the action-array walker + interpreter) will live alongside
//    MotionPump under animation/. Keeping the namespace empty before
//    that surface arrives means there's only one place to look for
//    "the animation engine" once the arc is complete.

#pragma once
#include <functional>
#include <memory>

namespace curvz::animation {

// MotionPump — drives a callback from t=0 to t=1 over a duration.
// Construct, call start() with a duration and tick callback, drop the
// reference. The pump holds itself alive via the timeout closure
// until completion.
class MotionPump : public std::enable_shared_from_this<MotionPump> {
public:
    // Tick callback. Receives the fractional elapsed time (0.0 to 1.0)
    // for this tick. The pump guarantees:
    //   - First tick: t may be > 0 if the first timeout fires after a
    //     delay (typical: t ≈ 0.016 for a 1000ms duration at 60fps).
    //   - Last tick: t == 1.0 exactly. The pump forces a final tick
    //     at t=1.0 regardless of timeout granularity, so the callback
    //     can land the model at its exact final state without
    //     epsilon-checks.
    //   - Monotonic: t is non-decreasing across ticks.
    using Tick = std::function<void(double t)>;

    MotionPump() = default;
    ~MotionPump() = default;

    // Non-copyable / non-movable. The pump's identity is tied to its
    // self-ref in the timeout closure; copying or moving it would
    // sever that reference.
    MotionPump(const MotionPump&) = delete;
    MotionPump& operator=(const MotionPump&) = delete;
    MotionPump(MotionPump&&) = delete;
    MotionPump& operator=(MotionPump&&) = delete;

    // Start the pump. duration_ms must be > 0; a non-positive duration
    // fires one tick at t=1.0 and returns (degenerate case — useful
    // for "instant" beats that share the orchestration shape with
    // timed beats but happen in a single step).
    //
    // tick is called on the GTK main loop thread, once per timeout
    // firing, until t reaches 1.0. The pump self-holds via shared_ptr
    // into the timeout closure, so the caller can drop its reference
    // immediately. The pump must therefore be constructed via
    // std::make_shared (enable_shared_from_this requires it); a
    // stack-allocated or unique_ptr-held MotionPump will misbehave
    // when start() tries shared_from_this().
    //
    // Calling start() more than once on the same pump is undefined
    // behaviour — the second timeout would overlap the first. m1
    // does not check / refuse / queue; the contract is "one pump,
    // one animation."
    void start(double duration_ms, Tick tick);
};

} // namespace curvz::animation
