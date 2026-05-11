#pragma once
#include <gtkmm/application.h>

#ifdef CURVZ_DIAGNOSTIC
namespace curvz::scripting { class ScripterWindow; }
#endif

namespace Curvz {

class Application : public Gtk::Application {
public:
    static Glib::RefPtr<Application> create();

#ifdef CURVZ_DIAGNOSTIC
    // s190 m2 — access the diagnostic Scripter window. Built once at
    // on_activate and held by Application; X-button close hides it
    // rather than destroying (set_hide_on_close(true) in the ctor).
    //
    // Returns nullptr if construction failed (shouldn't occur in
    // diagnostic builds, but defensive). MainWindow's headerbar
    // toggle drives present()/set_visible(false) directly and
    // subscribes to property_visible() to keep its checked state in
    // sync when the X-button hides the window.
    curvz::scripting::ScripterWindow* scripter() { return m_scripter; }
#endif

protected:
    Application();
    void on_startup() override;
    void on_activate() override;

#ifdef CURVZ_DIAGNOSTIC
private:
    curvz::scripting::ScripterWindow* m_scripter = nullptr;
#endif
};

} // namespace Curvz
