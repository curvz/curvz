#pragma once

// ── ProgressDialog (s277 m1) ─────────────────────────────────────────
//
// Long-op progress envelope. Shows a modal dialog with bar /
// message / elapsed / ETA / Cancel.
//
// s277 m3 — dialog presents immediately on run() rather than gating
// on a 300 ms threshold. Threshold gating fired too late on heavy
// ops: the main thread can be busy doing pre-flight work (path
// conversion, intersection enrichment, etc.) BEFORE run() is even
// called, so a threshold timer counting from run() entry would let
// the user see a multi-second freeze before any feedback appeared.
// Trade-off: very fast ops flicker the dialog briefly. In practice
// this reads as "the click did something" — same convention every
// other vector app uses for boolean ops.
//
// The caller passes a work lambda; the dialog runs the lambda on a
// std::thread, blocking the calling site until completion. The lambda
// receives a ProgressHandle that it pokes from the worker thread —
// update() for determinate, pulse() for indeterminate, cancelled()
// to poll for the Cancel button. Cross-thread notification rides on
// Glib::Dispatcher (the established codebase pattern; see
// NewDocumentDialog::m_regen_dispatcher).
//
// ── Modes ────────────────────────────────────────────────────────────
//
//   total == 0  → indeterminate. The bar pulses; update()'s `current`
//                 argument is ignored. Use for ops where the work
//                 happens inside an opaque library call (e.g. the N-way
//                 Union path through Clipper2): the worker emits pulse
//                 from a ticker, the library churns inside the lambda
//                 with no progress hooks, and the bar animates so the
//                 user sees the app is alive.
//   total >  0  → determinate. The bar fills as `current / total`. ETA
//                 is derived from elapsed × (total - current) / current
//                 once current >= 1.
//
// ── Cancel semantics ─────────────────────────────────────────────────
//
// The lambda is responsible for honouring cancelled(). When Cancel is
// clicked, the dialog flips the atomic; it does NOT interrupt the
// worker. For ops with no cooperative cancel inside the library call
// (Clipper2 is one), the established pattern is:
//
//   1. Worker keeps running (orphaned but harmless — pure compute).
//   2. main thread does run.join() before run() returns.
//   3. Caller checks run()'s return value: false → discard the result
//      the worker produced, don't apply any mutation.
//
// "Cancel" from the user's view is therefore "instant dismiss", even
// though the underlying work might churn for a few more seconds before
// the join completes. For v1 this is acceptable; mitigations (detach
// + deep-copy operands, max-one-worker queue, cancel-aware library
// fork) are future work logged in the s277 handoff backlog.
//
// ── Lifecycle ────────────────────────────────────────────────────────
//
// Held as a value member on MainWindow (`m_progress_dialog`). Each
// call to run() re-asserts transient-for, applies the parent's motif
// class, and present()s after the 300 ms threshold elapses. On
// completion (or cancel) the dialog is hidden; the instance is
// reused across calls.

#include "curvz_utils.hpp"  // apply_motif_class_from_parent

#include <glibmm/dispatcher.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/window.h>
#include <sigc++/connection.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace Curvz {

class ProgressDialog;

// ── ProgressHandle ──────────────────────────────────────────────────
//
// Worker-thread-only view onto the dialog's state atomics. All three
// methods are safe to call from the worker thread; they touch only
// std::atomic<> members on the owning ProgressDialog. The UI thread
// reads the same atomics in its dispatcher slot and the timer tick.

class ProgressHandle {
public:
    // Determinate update. Sets the displayed `current` and message;
    // the actual paint happens on the UI thread on the next tick or
    // when the dispatcher fires.
    void update(int current, const std::string& message);

    // Indeterminate pulse. Bumps a pulse generation that the UI tick
    // turns into Gtk::ProgressBar::pulse() calls. Message is updated
    // alongside.
    void pulse(const std::string& message);

    // Has the user clicked Cancel? Worker polls this between chunks of
    // its work; the lambda returns early when true.
    bool cancelled() const;

private:
    friend class ProgressDialog;
    ProgressDialog* m_owner = nullptr;
    explicit ProgressHandle(ProgressDialog* owner) : m_owner(owner) {}
};

// ── ProgressDialog ──────────────────────────────────────────────────

class ProgressDialog : public Gtk::Window {
public:
    using Work = std::function<bool(ProgressHandle&)>;

    ProgressDialog();

    // Run `work` with a progress dialog as its envelope. Returns the
    // lambda's return value (true = completed, false = cancelled or
    // explicit failure). `total == 0` selects indeterminate mode.
    //
    // Blocking semantics: this function spins the GTK main loop on
    // the calling thread until the worker finishes. The calling
    // thread MUST be the UI thread (this is the only thread that may
    // touch the dialog's widgets).
    bool run(Gtk::Window& parent, int total, const std::string& title,
             Work work);

private:
    friend class ProgressHandle;

    // ── Layout ──────────────────────────────────────────────────────
    Gtk::Box          m_root{Gtk::Orientation::VERTICAL};
    Gtk::Label        m_message;
    Gtk::ProgressBar  m_bar;
    Gtk::Box          m_stats_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Label        m_elapsed;
    Gtk::Label        m_eta;
    Gtk::Button       m_btn_cancel{"Cancel"};

    // ── Cross-thread state ──────────────────────────────────────────
    //
    // Atomics are written by the worker (via ProgressHandle) and read
    // by the UI thread (tick + dispatcher slot). No mutex needed —
    // each atomic stands alone; message is the only string and lives
    // behind its own mutex via m_message_mu.
    std::atomic<int>      m_current{0};
    std::atomic<int>      m_total{0};       // 0 = indeterminate
    std::atomic<bool>     m_cancel{false};
    std::atomic<bool>     m_done{false};
    std::atomic<uint64_t> m_pulse_gen{0};   // bumped on each pulse()

    std::mutex            m_message_mu;
    std::string           m_message_text;

    // ── UI-side scratch ─────────────────────────────────────────────
    Glib::Dispatcher      m_dispatcher;     // worker → UI nudge
    bool                  m_dispatcher_connected = false;
    sigc::connection      m_tick_conn;      // periodic UI refresh
    int64_t               m_start_us = 0;   // monotonic, microseconds
    uint64_t              m_last_seen_pulse_gen = 0;
    bool                  m_shown = false;  // dialog has been present()ed

    // ── Helpers ─────────────────────────────────────────────────────
    void refresh_from_atomics();   // copy state → widgets (UI thread)
    void on_dispatcher();          // worker emitted; refresh + check done
    void on_cancel_clicked();
    void reset_for_run(const std::string& title, int total);
};

} // namespace Curvz
