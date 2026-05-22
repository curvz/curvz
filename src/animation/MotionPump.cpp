// animation/MotionPump.cpp ───────────────────────────────────────────────────
//
// s288 m1 — see animation/MotionPump.hpp for the arc header. Implementation
// is small by design: a Glib::Timeout firing at ~60fps, a wall-clock-derived
// fractional t, and a final t=1.0 force-tick at completion.

#include "animation/MotionPump.hpp"

#include <glibmm/main.h>
#include <chrono>

namespace curvz::animation {

namespace {
// 60fps target. Glib::Timeout granularity is millisecond-level; 16ms is
// close enough to 1/60s that the visible cadence is smooth. The pump
// computes t from wall-clock elapsed, not tick count, so a missed tick
// (long redraw, GC pause) lands at the correct t when the next tick
// fires — duration is preserved, smoothness is best-effort.
constexpr int kTickIntervalMs = 16;
} // namespace

void MotionPump::start(double duration_ms, Tick tick) {
    if (!tick) return;

    // Degenerate case: non-positive duration. Fire once at t=1.0 and
    // return — useful for "instant" beats that share the orchestration
    // shape with timed beats but want to land in one step. No timeout
    // installed; no self-hold needed.
    if (duration_ms <= 0.0) {
        tick(1.0);
        return;
    }

    // Self-hold: capture a shared_ptr to ourselves into the timeout
    // closure. The caller can drop their reference immediately after
    // start() returns; we stay alive until the timeout chain
    // terminates (when t reaches 1.0 and we return false from the
    // tick handler).
    auto self = shared_from_this();

    // Wall-clock start. Using steady_clock so we're immune to system
    // clock adjustments (NTP nudges, daylight-saving rollovers, the
    // user manually changing their clock during a 30-second welcome).
    const auto t_start = std::chrono::steady_clock::now();

    // Per-tick state: whether we've already fired the final t=1.0
    // tick. Without this flag, a slow main loop could schedule one
    // more tick after we've completed and re-fire the final state.
    // The flag is captured by value into the closure but mutated via
    // shared_ptr so it survives across timeout invocations.
    auto done = std::make_shared<bool>(false);

    Glib::signal_timeout().connect(
        [self, tick, t_start, duration_ms, done]() -> bool {
            if (*done) return false;  // belt-and-braces; shouldn't fire

            const auto now = std::chrono::steady_clock::now();
            const double elapsed_ms =
                std::chrono::duration<double, std::milli>(now - t_start)
                    .count();

            double t = elapsed_ms / duration_ms;

            // Have we passed the duration? Fire one final tick at
            // exactly t=1.0 and tear down. The model lands at its
            // exact final state regardless of timeout granularity.
            if (t >= 1.0) {
                *done = true;
                tick(1.0);
                return false;  // stop the timeout; self-ref drops
            }

            tick(t);
            return true;  // continue
        },
        kTickIntervalMs);

    // self goes out of scope here; the only remaining reference is
    // inside the timeout closure, which keeps the pump alive until
    // the timeout returns false.
}

} // namespace curvz::animation
