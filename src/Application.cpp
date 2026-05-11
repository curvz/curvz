#include "Application.hpp"
#include "AppPreferences.hpp"  // s139 m2 — load pref before MainWindow construction
#include "CurvzLog.hpp"
#include "MacroSystem.hpp"
#include "MainWindow.hpp"
#include "RecentProjects.hpp"  // s144 m3 — load recents list at startup
#include "TemplateLibrary.hpp"
#include "color/SwatchLibrary.hpp"  // color::load_app_defaults (s69 M2)
#ifdef CURVZ_DIAGNOSTIC
#include "scripting/ScripterWindow.hpp"  // s186 m2
#include <filesystem>
#include <glibmm/main.h>                 // s186 m2 close-out: warmup
#endif
#include <cstdlib>             // s145 m4 — getenv for early prefs.json lookup
#include <cstring>             // s180 — std::strcmp in warning-filter writer fn
#include <filesystem>
#include <fstream>             // s145 m4 — read prefs.json before singleton loads
#include <nlohmann/json.hpp>   // s145 m4 — parse log_path_override pre-spdlog
extern "C" {
#include "curvz-resources.h"
}
#include <glibmm/miscutils.h>
#include <glib.h>  // s180: g_log_set_handler / g_strstr_len for warning filter
#include <gtkmm/icontheme.h>
#include <gtkmm/settings.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace Curvz {

// s145 m4 — read the log_path_override key directly from preferences.json,
// without going through AppPreferences::instance().load(). Necessary
// because spdlog must be initialised before any LOG_* call can fire,
// and that has to happen in the Application constructor — which runs
// before on_activate, which is where the AppPreferences singleton
// loads. The two cannot coexist on the AppPreferences-singleton path,
// so this helper short-circuits.
//
// Robustness: missing file, missing key, wrong type, and parse errors
// all return empty string. The caller falls through to the default
// log path. No logging from this function (no logger yet); failures
// are silent. The same key is also loaded later by the singleton
// proper, so the inspector-side getter still works.
namespace {
std::string read_log_path_override_from_disk() {
    namespace fs = std::filesystem;
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base = xdg ? xdg
                           : (std::string(std::getenv("HOME")
                                              ? std::getenv("HOME")
                                              : ".") +
                              "/.config");
    const std::string prefs_path = base + "/curvz/preferences.json";
    std::ifstream f(prefs_path);
    if (!f) return {};
    try {
        nlohmann::json j;
        f >> j;
        if (j.contains("log_path_override") &&
            j["log_path_override"].is_string()) {
            return j["log_path_override"].get<std::string>();
        }
    } catch (...) {}
    return {};
}
} // anon

Glib::RefPtr<Application> Application::create() {
  return Glib::make_refptr_for_instance<Application>(new Application());
}

Application::Application()
    : Gtk::Application("io.github.curvz.Curvz",
                       Gio::Application::Flags::HANDLES_OPEN) {
  try {
    namespace fs = std::filesystem;

    // s145 m4 — consult log_path_override before falling through to the
    // default location. The override is read directly from disk because
    // the AppPreferences singleton hasn't loaded yet at this point; see
    // read_log_path_override_from_disk() above. Empty override = use
    // default. We also create the parent directory either way.
    std::string log_path;
    const std::string override_path = read_log_path_override_from_disk();
    if (!override_path.empty()) {
        log_path = override_path;
        fs::path parent = fs::path(log_path).parent_path();
        if (!parent.empty()) fs::create_directories(parent);
    } else {
        std::string data_dir = Glib::get_user_data_dir();
        std::string log_dir = (fs::path(data_dir) / "curvz").string();
        fs::create_directories(log_dir);
        log_path = (fs::path(log_dir) / "curvz.log").string();
    }

    auto logger = spdlog::basic_logger_mt("curvz", log_path, true);
    logger->set_level(spdlog::level::debug);
    logger->flush_on(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    LOG_INFO("Curvz starting — log: {}{}", log_path,
             override_path.empty() ? "" : " (from log_path_override)");
  } catch (...) {
  }
}

// s138: Register keyboard accelerators at the application level.
//
// Background: this code lived in MainWindow::setup_menu() through s137.
// MainWindow's constructor calls setup_menu() during construction, but the
// window has not yet been added to the application at that point — so
// get_application() returns null and the entire bind block was skipped
// silently. Net effect: accels were never registered, the hamburger menu
// rendered with empty accel columns, and the manual claim that the menu
// shows shortcuts had not been true since whenever the regression landed.
//
// The fix is structural, not procedural. Accels are an application-scope
// concern: GTK4's accel system is owned by Gtk::Application and survives
// any window-construction reshuffle. Registering them in on_startup —
// which fires once before any window exists — removes the latent
// dependency on MainWindow's construction order and makes the call site
// match the concept's natural home.
//
// CAPTURE-controller note: per s125+ project rules, set_accels_for_action
// is COSMETIC in this codebase. Every shortcut that needs to dispatch is
// wired explicitly in MainWindow's CAPTURE-phase key controller switch.
// This block exists solely so the popover menu can render the accel
// strings in its right-aligned column. Do not assume an accel registered
// here will fire — the controller is the source of truth for dispatch.
void Application::on_startup() {
  Gtk::Application::on_startup();

  // ── s180: suppress one specific GTK warning ────────────────────────────────
  //
  // The cosmetic warning
  //
  //   Gtk-WARNING **: Trying to snapshot sb 0x... without a current allocation
  //
  // has been firing intermittently since s35 on doc-list mutations (make doc,
  // delete doc, first switch-into-doc). The widget is the StatusBar root box
  // (named "sb" in src/StatusBar.cpp via curvz::utils::set_name). It has been
  // chased across multiple sessions, including:
  //
  //   - s35: traced to gtkmm__GtkBox at startup; banked as low priority.
  //   - s60: deferred initial append to defeat startup-time race.
  //   - s180: full gdb investigation under G_DEBUG=fatal-warnings. Backtrace
  //     contains zero Curvz frames — the call chain is pure GTK frame-clock
  //     paint into gtk_widget_snapshot_child. The StatusBar has cached
  //     allocation 1496×23, is visible/realized/mapped, but its parent
  //     (m_root) has marked it alloc_needed and the next paint cycle catches
  //     it before the size-allocate phase resolves. m_root has a valid
  //     allocation; only the StatusBar child is dirty.
  //   - s180 m2: detach/reattach pattern wrapping make/delete handlers
  //     (with_statusbar_detached). Did NOT silence the warning, confirming
  //     that the issue is not about the bar being a sibling of the
  //     mutating subtree — GTK's frame-clock can catch the bar with stale
  //     allocation regardless. Renderer-independent (cairo and ngl both).
  //
  // Verdict: this is a GTK4 layout-cycle timing artefact, not a Curvz bug.
  // The app behaves correctly; only the warning text is noise. Suppressing
  // it here keeps the log readable for actual issues. The filter is
  // narrow — it matches only the exact warning text on this exact widget
  // name, so any new "snapshot without allocation" warning on a different
  // widget will still surface.
  //
  // Implementation note: GTK4 uses g_log_structured for warning emission,
  // which bypasses the legacy g_log_set_handler path entirely. The right
  // hook is g_log_set_writer_func, which sees every structured-log call
  // for the whole process. We extract the MESSAGE field from the fields
  // array, match against our specific warning text, and either drop or
  // forward to g_log_writer_default for everything else.
  //
  // To re-enable for diagnosis: comment out g_log_set_writer_func call below.
  g_log_set_writer_func(
      [](GLogLevelFlags log_level, const GLogField *fields, gsize n_fields,
         gpointer user_data) -> GLogWriterOutput {
        // Find the MESSAGE field. Structured-log fields are key-value pairs
        // and order isn't guaranteed; we walk the array to find it.
        const char *msg = nullptr;
        for (gsize i = 0; i < n_fields; ++i) {
          if (fields[i].key && std::strcmp(fields[i].key, "MESSAGE") == 0) {
            msg = static_cast<const char *>(fields[i].value);
            break;
          }
        }
        if (msg && g_strstr_len(msg, -1, "Trying to snapshot sb") != nullptr) {
          // Swallow this specific warning. Treat as handled so GLib doesn't
          // fall through to abort on G_DEBUG=fatal-warnings either.
          return G_LOG_WRITER_HANDLED;
        }
        return g_log_writer_default(log_level, fields, n_fields, user_data);
      },
      nullptr, nullptr);

  auto bind = [this](const char *action,
                     std::vector<Glib::ustring> accels) {
    set_accels_for_action(action, accels);
  };

  // File
  bind("win.new-project",       {"<Control><Shift>n"});
  bind("win.new",               {"<Control>n"});
  bind("win.open",              {"<Control>o"});
  bind("win.close-project",     {"<Control><Shift>w"});
  bind("win.save",              {"<Control>s"});
  bind("win.save-as",           {"<Control><Shift>s"});
  bind("win.save-as-template",  {"<Control><Alt>s"});
  bind("win.manage-templates",  {"<Control><Alt>t"});
  bind("win.import-svg",        {"<Control>i"});
  bind("win.import-svg-icon",   {"<Control><Alt>i"});
  bind("win.place-image",       {"<Control><Shift>p"});
  bind("win.export",            {"<Control><Shift>t"});
  bind("win.print",             {"<Control>p"});

  // Edit
  bind("win.undo",            {"<Control>z"});
  bind("win.redo",            {"<Control><Shift>z", "<Control>y"});
  bind("win.select-all",      {"<Control>a"});
  bind("win.deselect-all",    {"<Control><Shift>a"});
  bind("win.cut",             {"<Control>x"});
  bind("win.copy",            {"<Control>c"});
  bind("win.paste",           {"<Control>v"});
  bind("win.duplicate",       {"<Control>d"});
  bind("win.duplicate-in-place", {"<Alt>d"}); // s181: was win.clone
  bind("win.step-repeat",     {"<Control><Alt>d"});

  // Arrange
  bind("win.arrange-bring-front",    {"<Control><Shift>Up"});
  bind("win.arrange-bring-forward",  {"<Control>Up"});
  bind("win.arrange-send-backward",  {"<Control>Down"});
  bind("win.arrange-send-back",      {"<Control><Shift>Down"});
  bind("win.flip-horizontal",        {"<Control><Shift>h"});
  bind("win.flip-vertical",          {"<Control><Alt>v"});

  // Align & Distribute (s135 m1)
  bind("win.align-left",      {"<Control><Alt>l"});
  bind("win.align-center-h",  {"<Control><Alt>h"});
  bind("win.align-right",     {"<Control><Alt>r"});
  bind("win.align-top",       {"<Control><Alt>p"});
  bind("win.align-center-v",  {"<Control><Alt>m"});
  bind("win.align-bottom",    {"<Control><Alt>b"});

  // Path
  bind("win.bool-union",      {"<Control><Shift>u"});
  bind("win.bool-subtract",   {"<Control><Shift>e"});
  bind("win.bool-intersect",  {"<Control><Shift>i"});
  bind("win.make-compound",   {"<Control>8"});
  bind("win.split-compound",  {"<Control><Shift>8"});
  bind("win.group-make",      {"<Control>g"});
  bind("win.group-release",   {"<Control><Shift>g"});
  bind("win.offset-path",     {"<Control><Shift>o"});
  bind("win.expand-stroke",   {"<Control><Shift>x"});
  bind("win.text-to-path",    {"<Control><Alt>t"});
  bind("win.clip-make",       {"<Control>7"});
  bind("win.clip-release",    {"<Control><Alt>7"});
  bind("win.blend-make",      {"<Control>b"});
  bind("win.blend-release",   {"<Control><Shift>b"});
  bind("win.warp-make",       {"<Control><Shift>y"});
  bind("win.warp-edit",       {"<Control><Alt>y"});
  bind("win.warp-flatten",    {"<Control><Alt>f"});

  // View
  bind("win.toggle-rulers",   {"<Control>r"});
  bind("win.toggle-outline",  {"<Control>e"});
  // Zoom: bare-key accels (0/1/2/3/+/-) are intentionally NOT registered.
  // set_accels_for_action's accelerator dispatch fires AFTER MainWindow's
  // CAPTURE controller returns false (which it does when text input has
  // focus), so a bare-key accel hijacks every digit/symbol typed into a
  // CurvzEntry or CurvzSpinButton. The CAPTURE controller in MainWindow
  // wires bare-key zoom directly when the canvas has focus; the menu's
  // shortcut hint column will show only the modifier-keyed alternatives.
  bind("win.zoom-selection",  {"<Control>3"});
  bind("win.zoom-fit",        {"<Control>0"});

  // Document navigation
  bind("win.doc-next",        {"<Control>Tab",        "<Control>Page_Down"});
  bind("win.doc-prev",        {"<Control><Shift>Tab", "<Control>Page_Up"});

  // App
  bind("win.show-help",       {"F1", "<Alt>question"});
  bind("win.show-shortcuts",  {"question", "slash"});
  bind("win.quit",            {"<Control>q", "<Control>w"});

  LOG_INFO("Application::on_startup — registered menu accels");
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

  // s139 m2 (moved earlier in s145 m1): load app-tier preferences before
  // the GTK settings block so the tooltip-delay pref can be applied below.
  // None of the pre-load callers (icon-theme registration above) consult
  // AppPreferences, so moving this up is safe; the original placement
  // before MainWindow construction is still preserved.
  AppPreferences::instance().load();

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
    // s145 m1: the literal 150 is now the tooltip_delay_ms user pref
    // (default 150 preserves historical behaviour). Range 0..2000 is
    // enforced by the setter; we don't double-clamp here.
    GObjectClass* settings_class =
        G_OBJECT_GET_CLASS(settings->gobj());
    if (g_object_class_find_property(settings_class,
                                     "gtk-tooltip-timeout") != nullptr) {
      const int delay_ms =
          AppPreferences::instance().tooltip_delay_ms();
      settings->set_property<int>("gtk-tooltip-timeout", delay_ms);
      LOG_INFO("Application: tooltip delay set to {} ms (pref)", delay_ms);
    }
  }

  MacroManager::instance().load();

  // s144 m3: load recents list. Order matters — AppPreferences first (now
  // loaded above) so RecentProjects::load can read recent_projects_max_count
  // if it ever grows trim-on-load logic. Missing file is fine; empty list
  // is the first-run state.
  RecentProjects::instance().load();

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

#ifdef CURVZ_DIAGNOSTIC
  // s186 m2: Scripter window — the developer/QA surface for the
  // script-driven substrate. Visible only in diagnostic builds.
  //
  // Script library location: prefer ./tests/scripts (run from build
  // tree during dev) and ../tests/scripts (run from inside build/).
  // Falls back to "tests/scripts" so the picker shows a folder name
  // even if nothing's there yet — user can use the Folder… button
  // to point elsewhere.
  //
  // Theme: this is a regular Gtk::ApplicationWindow added to the
  // application, so MainWindow::apply_motif_to_window() walks every
  // top-level via gtk_window_get_toplevels() and stamps curvz-light
  // on the next project-load / motif-change. First-present may show
  // the system theme briefly — acceptable for a diagnostic window.
  namespace fs = std::filesystem;
  std::string scripts_dir = "tests/scripts";
  for (auto candidate : {
          fs::path("tests/scripts"),
          fs::path("../tests/scripts"),
       }) {
      if (fs::exists(candidate) && fs::is_directory(candidate)) {
          scripts_dir = fs::absolute(candidate).string();
          break;
      }
  }
  auto* scripter = new curvz::scripting::ScripterWindow(scripts_dir);
  add_window(*scripter);
  scripter->present();
  LOG_INFO("Application: CURVZ_DIAGNOSTIC build — Scripter window opened "
           "(scripts dir: {})", scripts_dir);

  // s186 m2 close-out: warm the GLib main-loop dispatch mechanism.
  //
  // Empirical observation (s186 matrix: sessions 1/2/3 with verb
  // permutations): the FIRST script that hits the async activate()
  // path FAILs cold-start, every other path / every subsequent
  // script PASSes. set_active() (synchronous setter) and queries
  // are cold-safe; only activate() — which queues a click event
  // for main-loop dispatch — is cold-bound.
  //
  // The hypothesis under test: GLib's main-context dispatch
  // mechanism (signal_idle / signal_timeout / event queue) pays
  // a first-iteration overhead. The listener's 5ms wait between
  // script lines isn't long enough to cover that first-iteration
  // cost. By pre-firing one idle and one timeout at startup, we
  // pay the warmup cost during launch (where it's invisible
  // against existing startup time) instead of during the user's
  // first test (where it lies about state).
  //
  // If this fixes 01_node_tool_toggle's cold-start FAIL: the
  // mechanism is GLib-side. m3 calibration formalises this as
  // a wait-class system.
  //
  // If this DOESN'T fix it: the cold path is in the click cascade
  // itself (gesture-controller init, signal_realize chain, lazy
  // tool-state lookup). Different fix needed; this five-line
  // change costs nothing if it doesn't work.
  Glib::signal_idle().connect_once([]() {
      LOG_INFO("Application: GLib main-loop idle warmup fired");
  });
  Glib::signal_timeout().connect_once([]() {
      LOG_INFO("Application: GLib main-loop timeout warmup fired");
  }, 0);
#endif
}

} // namespace Curvz
