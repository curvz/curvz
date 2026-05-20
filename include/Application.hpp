#pragma once
#include <gtkmm/application.h>
#include <chrono>

// s219 m1 — the Scripter window is no longer owned by Application.
// It lives inside MainWindow now (held as a unique_ptr member), the
// same way HelpWindow / ShortcutsDialog / BlendDialog / MacroEditorWindow
// / MacroManagerWindow do. Application no longer needs to know about
// the Scripter at all.

namespace Curvz {

// s268 m0 — cold-launch timing. main() stamps g_launch_t0 before any
// other code runs; Canvas::on_draw consumes the timestamp the first
// time it fires and logs the delta as INFO. One log line per process
// lifetime, no UI surface, no perf cost beyond the static-bool latch
// inside on_draw.
//
// Flip the constexpr to false to silence. Left as compile-time rather
// than a CLI flag or preference because it's a development-side
// diagnostic that should either be on for everyone or off for
// everyone — not configurable per-user.
inline constexpr bool LAUNCH_TIMING_ENABLED = true;
extern std::chrono::steady_clock::time_point g_launch_t0;

class MainWindow;

class Application : public Gtk::Application {
public:
    static Glib::RefPtr<Application> create();

    // s193 m2 — access the main window for window-stacking control
    // (the Scripter's Auto-lower toggle raises MainWindow above
    // itself at Run start). Held since on_activate constructed it;
    // nullptr only if startup hasn't completed.
    MainWindow* main_window() { return m_main_window; }

protected:
    Application();
    void on_startup() override;
    void on_activate() override;

private:
    MainWindow* m_main_window = nullptr;
};

} // namespace Curvz
