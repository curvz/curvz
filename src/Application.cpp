#include "Application.hpp"
#include "CurvzLog.hpp"
#include "MacroSystem.hpp"
#include "MainWindow.hpp"
#include "TemplateLibrary.hpp"
#include "color/SwatchLibrary.hpp"  // color::load_app_defaults (s69 M2)
#include <filesystem>
extern "C" {
#include "curvz-resources.h"
}
#include <glibmm/miscutils.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/settings.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace Curvz {

Glib::RefPtr<Application> Application::create() {
  return Glib::make_refptr_for_instance<Application>(new Application());
}

Application::Application()
    : Gtk::Application("com.example.curvz",
                       Gio::Application::Flags::HANDLES_OPEN) {
  try {
    namespace fs = std::filesystem;
    std::string data_dir = Glib::get_user_data_dir();
    std::string log_dir = (fs::path(data_dir) / "curvz").string();
    fs::create_directories(log_dir);
    std::string log_path = (fs::path(log_dir) / "curvz.log").string();

    auto logger = spdlog::basic_logger_mt("curvz", log_path, true);
    logger->set_level(spdlog::level::debug);
    logger->flush_on(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    LOG_INFO("Curvz starting — log: {}", log_path);
  } catch (...) {
  }
}

void Application::on_activate() {
  // Register compiled GLib resources (icons etc.) into the process resource
  // set.
  g_resources_register(resources_get_resource());

  // Register our gresource icon prefix with the default icon theme. GTK
  // resolves set_icon_name("curvz-foo-symbolic") by walking
  // /com/curvz/app/icons/scalable/apps/curvz-foo-symbolic.svg directly out
  // of the compiled-in resource bundle — no disk extraction, no allow-list
  // to maintain. Adding a new icon = drop the .svg in resources/icons/ and
  // list it in resources.xml under the Path B block.
  //
  // Replaces an earlier workaround that extracted each icon to
  // ~/.local/share/icons/hicolor/scalable/apps/ on first run and registered
  // that path. The extraction loop required a hand-maintained subset list
  // in C++; anything not in the list silently failed to resolve unless the
  // user installed it manually. add_resource_path predates the workaround
  // (GTK 3.14+) and avoids both problems.
  auto icon_theme =
      Gtk::IconTheme::get_for_display(Gdk::Display::get_default());
  icon_theme->add_resource_path("/com/curvz/app/icons");
  LOG_INFO("Icon theme has curvz-select-symbolic: {}",
           icon_theme->has_icon("curvz-select-symbolic"));

  // Request dark color scheme — affects headerbar CSD and all GTK widgets.
  // This uses the system's dark variant if available (Adwaita-dark on GNOME).
  // User can override via GNOME Settings → Appearance later (Phase 4).
  auto settings = Gtk::Settings::get_default();
  if (settings) {
    settings->property_gtk_application_prefer_dark_theme() = true;
    // S85 cont-6: drop the tooltip delay from 500ms (default) to 150ms.
    // The "gtk-tooltip-timeout" property is deprecated since GTK 3.10
    // and gtkmm-4.0 has dropped the property_gtk_tooltip_timeout()
    // accessor. S86 fix6: current GTK has fully removed the underlying
    // GObject property as well, so an unconditional set_property() emits
    // a GLib-GObject-CRITICAL on startup. Probe the GObject class first
    // and only call set_property() when the property still exists; on
    // builds that have dropped it, we silently fall back to the default
    // tooltip timeout.
    GObjectClass* settings_class =
        G_OBJECT_GET_CLASS(settings->gobj());
    if (g_object_class_find_property(settings_class,
                                     "gtk-tooltip-timeout") != nullptr) {
      settings->set_property<int>("gtk-tooltip-timeout", 150);
    }
  }

  MacroManager::instance().load();

  // S69 M2: load app-global swatch defaults once at startup. Each project
  // that subsequently opens or is created gets these seeded into its
  // library's defaults tier via CurvzProject (see seed_defaults_from).
  // Missing file is fine — defaults tier stays empty and the two-tier
  // machinery falls through to custom-only behaviour.
  color::load_app_defaults();

  // S63: first-run seeding of starter templates (print / icons / web / social).
  // Gated by a marker file at ~/.config/curvz/templates/.seeds_v1 — runs once
  // per user, respects subsequent deletions.
  templates::seed_defaults_if_needed();

  auto *win = new MainWindow(*this);
  add_window(*win);
  win->present();
}

} // namespace Curvz
