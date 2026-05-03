#pragma once
#include <gtkmm/application.h>

namespace Curvz {

class Application : public Gtk::Application {
public:
    static Glib::RefPtr<Application> create();

protected:
    Application();
    void on_activate() override;
};

} // namespace Curvz
