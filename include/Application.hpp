#pragma once
#include <gtkmm/application.h>

// s219 m1 — the Scripter window is no longer owned by Application.
// It lives inside MainWindow now (held as a unique_ptr member), the
// same way HelpWindow / ShortcutsDialog / BlendDialog / MacroEditorWindow
// / MacroManagerWindow do. Application no longer needs to know about
// the Scripter at all.

namespace Curvz {

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
