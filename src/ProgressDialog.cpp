#include "ProgressDialog.hpp"

#include <glibmm/main.h>
#include <gtkmm/separator.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

namespace Curvz {

// ── ProgressHandle ──────────────────────────────────────────────────
//
// All three methods are called from the worker thread. They write
// std::atomic<> members on the owner and (for the message string)
// take a short mutex. The UI thread reads the same atomics in its
// tick and dispatcher slot — see refresh_from_atomics().

void ProgressHandle::update(int current, const std::string& message) {
    if (!m_owner) return;
    m_owner->m_current.store(current, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_owner->m_message_mu);
        m_owner->m_message_text = message;
    }
    m_owner->m_dispatcher.emit();
}

void ProgressHandle::pulse(const std::string& message) {
    if (!m_owner) return;
    m_owner->m_pulse_gen.fetch_add(1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_owner->m_message_mu);
        m_owner->m_message_text = message;
    }
    m_owner->m_dispatcher.emit();
}

bool ProgressHandle::cancelled() const {
    return m_owner && m_owner->m_cancel.load(std::memory_order_acquire);
}

// ── ProgressDialog ──────────────────────────────────────────────────

ProgressDialog::ProgressDialog() {
    curvz::utils::set_name(*this, "dlg_pr", "progress_dialog_root");
    set_modal(true);
    set_resizable(false);
    set_deletable(false);   // X-button disabled; cancel via the button
    set_default_size(360, -1);

    // ── Layout ──────────────────────────────────────────────────────
    m_root.set_spacing(8);
    m_root.set_margin(16);

    m_message.set_halign(Gtk::Align::START);
    m_message.set_hexpand(true);
    m_message.set_ellipsize(Pango::EllipsizeMode::END);
    m_message.set_max_width_chars(50);
    curvz::utils::set_name(m_message, "dlg_pr_msg",
                           "progress_dialog_message_label");

    m_bar.set_hexpand(true);
    m_bar.set_show_text(false);
    curvz::utils::set_name(m_bar, "dlg_pr_bar",
                           "progress_dialog_progress_bar");

    m_stats_row.set_spacing(0);
    m_stats_row.set_hexpand(true);

    m_elapsed.set_halign(Gtk::Align::START);
    m_elapsed.set_hexpand(true);
    m_elapsed.add_css_class("dim-label");
    curvz::utils::set_name(m_elapsed, "dlg_pr_el",
                           "progress_dialog_elapsed_label");

    m_eta.set_halign(Gtk::Align::END);
    m_eta.set_hexpand(true);
    m_eta.add_css_class("dim-label");
    curvz::utils::set_name(m_eta, "dlg_pr_eta",
                           "progress_dialog_eta_label");

    m_stats_row.append(m_elapsed);
    m_stats_row.append(m_eta);

    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    sep->set_margin_top(4);

    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    btn_row->set_halign(Gtk::Align::END);
    btn_row->set_spacing(8);
    curvz::utils::set_name(m_btn_cancel, "dlg_pr_cnc",
                           "progress_dialog_cancel_btn");
    btn_row->append(m_btn_cancel);

    m_btn_cancel.signal_clicked().connect(
        sigc::mem_fun(*this, &ProgressDialog::on_cancel_clicked));

    m_root.append(m_message);
    m_root.append(m_bar);
    m_root.append(m_stats_row);
    m_root.append(*sep);
    m_root.append(*btn_row);
    set_child(m_root);
}

// ── Helpers ─────────────────────────────────────────────────────────

void ProgressDialog::reset_for_run(const std::string& title, int total) {
    set_title(title);

    m_current.store(0, std::memory_order_release);
    m_total.store(total, std::memory_order_release);
    m_cancel.store(false, std::memory_order_release);
    m_done.store(false, std::memory_order_release);
    m_pulse_gen.store(0, std::memory_order_release);
    m_last_seen_pulse_gen = 0;

    {
        std::lock_guard<std::mutex> lk(m_message_mu);
        m_message_text.clear();
    }

    m_message.set_text(title);
    m_bar.set_fraction(0.0);
    m_elapsed.set_text("");
    m_eta.set_text("");
    m_btn_cancel.set_sensitive(true);
    m_shown = false;

    m_start_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

static std::string format_hms(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    int h = static_cast<int>(seconds / 3600);
    int m = static_cast<int>((seconds % 3600) / 60);
    int s = static_cast<int>(seconds % 60);
    char buf[32];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

void ProgressDialog::refresh_from_atomics() {
    // ── Message ─────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(m_message_mu);
        if (!m_message_text.empty()) m_message.set_text(m_message_text);
    }

    // ── Bar ─────────────────────────────────────────────────────────
    const int total = m_total.load(std::memory_order_acquire);
    const int current = m_current.load(std::memory_order_acquire);
    const uint64_t pulse_now = m_pulse_gen.load(std::memory_order_acquire);

    if (total > 0) {
        double frac = static_cast<double>(current) /
                      static_cast<double>(total);
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        m_bar.set_fraction(frac);
    } else {
        // Indeterminate. Pulse once per dispatcher hit AND once per
        // tick — the tick keeps the bar animating even when the
        // worker is silently churning between pulse() calls.
        m_bar.pulse();
        m_last_seen_pulse_gen = pulse_now;
    }

    // ── Elapsed / ETA ───────────────────────────────────────────────
    int64_t now_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    int64_t elapsed_us = now_us - m_start_us;
    int64_t elapsed_s = elapsed_us / 1'000'000;
    m_elapsed.set_text("Elapsed " + format_hms(elapsed_s));

    if (total > 0 && current >= 1) {
        double ratio = static_cast<double>(total - current) /
                       static_cast<double>(current);
        int64_t eta_s =
            static_cast<int64_t>(static_cast<double>(elapsed_s) * ratio);
        m_eta.set_text("ETA " + format_hms(eta_s));
    } else {
        m_eta.set_text("");
    }
}

void ProgressDialog::on_dispatcher() {
    // Worker emitted; the message string / current count / pulse gen
    // were just written. Pull them through to the widgets so the user
    // sees progress without waiting for the next 100ms tick.
    //
    // Gate on m_shown: dispatcher emits can race with the main loop's
    // exit from run() — a slot queued just before m_done flips may
    // not run until the NEXT main-loop entry, by which point we're
    // hidden and state is being reset for the next caller. Painting
    // a hidden widget with stale state is harmless but wasted work.
    if (m_shown) refresh_from_atomics();
}

void ProgressDialog::on_cancel_clicked() {
    m_cancel.store(true, std::memory_order_release);
    m_btn_cancel.set_sensitive(false);
    // Worker keeps running; main loop in run() waits for m_done. The
    // worker is expected to honour cancelled() and return promptly,
    // but for ops with no cooperative cancel inside their core call
    // (e.g. Clipper2) the wait may be the full remaining op duration.
    // Caller discards the result on cancel.
}

// ── run() ───────────────────────────────────────────────────────────

bool ProgressDialog::run(Gtk::Window& parent, int total,
                         const std::string& title, Work work) {
    if (!work) return false;

    reset_for_run(title, total);

    // First-time wire-up of the cross-thread dispatcher. Survives
    // across run() calls.
    if (!m_dispatcher_connected) {
        m_dispatcher.connect(
            sigc::mem_fun(*this, &ProgressDialog::on_dispatcher));
        m_dispatcher_connected = true;
    }

    // ── s277 m3 — present immediately ───────────────────────────────
    //
    // m1/m2 used a 300ms threshold timer so under-300ms ops never
    // painted a dialog. In practice that gate fired too late: for
    // genuinely heavy ops, the *main thread* is busy doing pre-flight
    // work (path conversion, intersection enrichment, etc.) BEFORE
    // run() is even called. The user saw a multi-second freeze before
    // the dialog appeared — long enough for GNOME's window-not-
    // responding watchdog to prompt force-quit.
    //
    // m3 trades a tiny flicker on fast ops for an instant dialog on
    // slow ops. Every other vector app (Inkscape, Affinity, Illustrator)
    // pops the modal immediately for boolean ops; we now match that
    // convention. If the work finishes in 50ms, the dialog flickers
    // for 50ms — visually reads as "the click did something."
    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);
    present();
    m_shown = true;

    // ── s277 m4 — pump GTK until the dialog actually paints ─────────
    //
    // present() only QUEUES the realize/paint; the actual draw happens
    // on a subsequent frame-clock tick. If we launch the worker
    // immediately and the worker thread saturates a core, the main
    // thread will sit in iteration(true) but may not get a frame slot
    // before the user-visible "what's happening?" window closes (Scott
    // observed the dialog being delayed for some time before showing).
    //
    // Force the realize + first paint through by pumping the main
    // context a few non-blocking iterations. This is the gtkmm idiom
    // for "make sure the user sees this widget before we proceed."
    // Bounded loop with a hard cap so we never spin pathologically.
    auto pump_ctx = Glib::MainContext::get_default();
    for (int i = 0; i < 20 && pump_ctx->pending(); ++i) {
        pump_ctx->iteration(false);
    }

    // ── 100 ms UI tick ──────────────────────────────────────────────
    //
    // Repaints elapsed/ETA from atomics and pulses the bar in
    // indeterminate mode even when the worker is silent. Cheap; one
    // timer wakes the main loop a few times per second.
    m_tick_conn = Glib::signal_timeout().connect(
        [this]() {
            if (m_done.load(std::memory_order_acquire)) return false;
            if (m_shown) refresh_from_atomics();
            return true;  // re-arm
        },
        100);

    // ── Worker thread ───────────────────────────────────────────────
    //
    // The worker runs the user's lambda off-UI. On completion it
    // stores the result, flips m_done, and emits the dispatcher one
    // last time so the UI loop wakes promptly.
    bool result = false;
    std::thread worker([this, &work, &result]() {
        ProgressHandle handle(this);
        try {
            result = work(handle);
        } catch (...) {
            // Don't let exceptions cross the thread boundary. The
            // contract is the lambda returns true/false.
            result = false;
        }
        m_done.store(true, std::memory_order_release);
        m_dispatcher.emit();
    });

    // ── Spin the main loop until the worker is done ────────────────
    //
    // Glib::MainContext::iteration(true) blocks until at least one
    // event is dispatched, then returns. The dispatcher emit from
    // the worker is one such event; so is the threshold timer and
    // the tick. This is the standard gtkmm pattern for
    // "synchronous-with-UI-pumping" calls.
    auto ctx = Glib::MainContext::get_default();
    while (!m_done.load(std::memory_order_acquire)) {
        ctx->iteration(true);
    }

    worker.join();

    // ── Teardown ────────────────────────────────────────────────────
    if (m_tick_conn.connected()) m_tick_conn.disconnect();
    if (m_shown) {
        hide();
        m_shown = false;
    }

    // Cancel overrides whatever the lambda returned. A lambda that
    // returns true while cancelled() was true is treated as cancelled
    // by the caller's view: callers test run()'s return.
    if (m_cancel.load(std::memory_order_acquire)) return false;
    return result;
}

} // namespace Curvz
