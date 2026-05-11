// main.cpp — scriptproto ─────────────────────────────────────────────────────
//
// Two top-level windows, one process:
//
//   • DemoWindow      — the BASE app. Owns the registered widgets
//                       (tool.node, filename.entry, opacity.slider, …).
//                       Listens for changes the way any normal GTK app
//                       would; emits events for both user clicks and
//                       script-driven invokes.
//
//   • ScripterWindow  — the SCRIPTER. Loads scripts from a library
//                       folder, lets you edit, drives the registry on
//                       Run, streams the output trace into its panel.
//
// Both windows visible from launch. Both share the same in-process
// ScriptableRegistry — the scripter dispatches by name and the base
// reacts via its registered widgets' invoke() implementations.
//
// s186: before the windows open, run_lifetime_test() exercises the
// register/unregister/re-register cycle and the name-validation
// invariants. Loud assertion-style failure if anything is wrong.

#include "ScriptableRegistry.hpp"
#include "ScripterWindow.hpp"

#include "widgets/Button.hpp"
#include "widgets/CheckButton.hpp"
#include "widgets/DropDown.hpp"
#include "widgets/Entry.hpp"
#include "widgets/Scale.hpp"
#include "widgets/SpinButton.hpp"
#include "widgets/ToggleButton.hpp"

#include <gtkmm/application.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace scriptproto;

// Forward declaration — defined in lifetime_test.cpp. No header file;
// it's a single entry point called once at startup. Adding a header for
// one symbol would be over-engineering.
namespace scriptproto { void run_lifetime_test(); }

// ── The base window — one of each scriptable widget ──────────────────────────
class DemoWindow : public Gtk::ApplicationWindow {
public:
    DemoWindow() {
        set_title("scriptproto — base");
        set_default_size(360, 480);

        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        box->set_margin(12);
        set_child(*box);

        auto add_row = [&](const char* caption, Gtk::Widget& w) {
            auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            auto* lbl = Gtk::make_managed<Gtk::Label>(caption);
            lbl->set_xalign(0.0);
            lbl->set_size_request(120, -1);
            row->append(*lbl);
            w.set_hexpand(true);
            row->append(w);
            box->append(*row);
        };

        m_toggle = std::make_unique<ToggleButton>("tool.node", "Node");
        add_row("tool.node", *m_toggle);

        m_button = std::make_unique<Button>("action.bake", "Bake");
        add_row("action.bake", *m_button);

        m_entry = std::make_unique<Entry>("filename.entry");
        m_entry->set_placeholder_text("type something");
        add_row("filename.entry", *m_entry);

        m_spin = std::make_unique<SpinButton>("opacity.spin",
                                                0.0, 1.0, 0.05, 2);
        m_spin->set_value(1.0);
        add_row("opacity.spin", *m_spin);

        m_scale = std::make_unique<Scale>("opacity.slider",
                                            0.0, 1.0, 0.05);
        m_scale->set_value(1.0);
        add_row("opacity.slider", *m_scale);

        std::vector<Glib::ustring> units = { "px", "pt", "mm", "in" };
        m_dropdown = std::make_unique<DropDown>("units.dropdown", units);
        add_row("units.dropdown", *m_dropdown);

        m_check = std::make_unique<CheckButton>("snap.check", "Snap");
        add_row("snap.check", *m_check);
    }

private:
    std::unique_ptr<ToggleButton>  m_toggle;
    std::unique_ptr<Button>        m_button;
    std::unique_ptr<Entry>         m_entry;
    std::unique_ptr<SpinButton>    m_spin;
    std::unique_ptr<Scale>         m_scale;
    std::unique_ptr<DropDown>      m_dropdown;
    std::unique_ptr<CheckButton>   m_check;
};

// ── Find the default script library folder ───────────────────────────────────
static std::string find_default_scripts_dir() {
    namespace fs = std::filesystem;
    for (auto candidate : {
            fs::path("scripts"),
            fs::path("../scripts"),
            fs::path("./scriptproto/scripts"),
        }) {
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            return fs::absolute(candidate).string();
        }
    }
    return "scripts";
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    auto app = Gtk::Application::create("io.curvz.scriptproto");

    app->signal_activate().connect([app]() {
        // Run the lifetime + validation test BEFORE any window opens.
        // If it fails, the program prints to stderr and exits non-zero
        // before the user sees anything — which is exactly the loud
        // failure we want for a foundation invariant.
        scriptproto::run_lifetime_test();

        auto* base = new DemoWindow();
        app->add_window(*base);
        base->present();

        auto* scripter = new ScripterWindow(find_default_scripts_dir());
        app->add_window(*scripter);
        scripter->present();
    });

    return app->run(argc, argv);
}
