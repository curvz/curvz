#include "MainWindow.hpp"
#include <functional>
#include <gtkmm/application.h>
#include <gtkmm/separator.h>
#include "ContextBar.hpp"
#include "CoordSpace.hpp"
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"
#include "DocTabBar.hpp"
#include "ExportDialog.hpp"
#include "Ruler.hpp"
#include "SvgOptimiser.hpp"
#include "SvgParser.hpp"
#include "SvgWriter.hpp"
#include "TemplateLibrary.hpp"
#include "ThemesDialog.hpp"
#include "ExportDocsDialog.hpp"
#include "UnitSystem.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2: apply_motif_class_from_parent
#include "style/StyleInterop.hpp"  // mutate_appearance — inspector-driven appearance edits
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <giomm/file.h>      // s125 m1a: Gio::File::create_for_path (folder picker)
#include <giomm/liststore.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <glibmm/main.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/image.h>      // s117 m19: custom About dialog hero logo
#include <gtkmm/settings.h>
#include <gtkmm/stack.h>      // s117 m19: custom About dialog flip animation
#include <gtkmm/stylecontext.h>
#include <nlohmann/json.hpp>

// GDK key definitions
#include <gdk/gdkkeysyms.h>

namespace Curvz {

namespace fs = std::filesystem;

// Turn a display name like "Untitled - Default" into a clean SVG filename
// stem like "Untitled-Default" (preserving case). Runs of non-alphanumeric
// characters collapse to a single '-'; leading/trailing dashes are stripped.
// Returns "icon" if the result would be empty.
static std::string doc_stem_from_name(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  bool last_dash = true;  // suppresses leading dashes
  for (unsigned char ch : raw) {
    if (std::isalnum(ch)) {
      out.push_back((char)ch);
      last_dash = false;
    } else if (!last_dash) {
      out.push_back('-');
      last_dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  if (out.empty()) out = "icon";
  return out;
}

#include "css.hpp"

MainWindow::MainWindow(Application & /*app*/) {
  curvz::utils::set_name(*this, "mw", "main_window_root");
  set_default_size(1400, 860);

  // ── CSS loading ────────────────────────────────────────────────────────
  // Two-stage load with GTK priority-based cascade:
  //   1. Built-in defaults  → priority APPLICATION  (ships with the binary)
  //   2. User stylesheet    → priority USER         (user-editable overrides)
  // GTK's cascade handles merging: user rules win where they match, defaults
  // cover everything else.  If the user deletes their file the app still
  // renders correctly from defaults alone.
  auto css = Gtk::CssProvider::create();
  css->load_from_data(CURVZ_CSS);
  Gtk::StyleContext::add_provider_for_display(
      get_display(), css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  // User stylesheet — ~/.config/curvz/styles.css
  fs::path user_css_dir  = fs::path(Glib::get_user_config_dir()) / "curvz";
  fs::path user_css_path = user_css_dir / "styles.css";
  std::error_code ec;
  fs::create_directories(user_css_dir, ec);
  if (ec) {
    LOG_WARN("User CSS: cannot create '{}': {}",
             user_css_dir.string(), ec.message());
  } else if (!fs::exists(user_css_path)) {
    // First-run seed — commented stub that explains usage.
    static const char *USER_CSS_STUB =
      "/* Curvz user stylesheet\n"
      " *\n"
      " * Loaded after Curvz's built-in CSS.  Rules here override defaults;\n"
      " * properties you don't set fall through to the built-in styles.\n"
      " *\n"
      " * To discover class names on any widget, press Ctrl+Shift+D to open\n"
      " * the GTK Inspector, then use its 'Pick widget' button (crosshair)\n"
      " * to select a widget and read its CSS classes.\n"
      " *\n"
      " * To restore defaults: empty this file or delete its contents.\n"
      " * Deleting the file entirely will cause Curvz to re-seed this stub\n"
      " * on next launch.\n"
      " *\n"
      " * Changes take effect on Curvz restart.\n"
      " *\n"
      " * Examples:\n"
      " *\n"
      " *   .inspector-group-title { color: #ffcc99; }\n"
      " *   .inspector-section-header { padding: 4px 10px; }\n"
      " *   .doc-tab-active          { background-color: #2a2a2a; }\n"
      " */\n";
    std::ofstream f(user_css_path);
    if (f) {
      f << USER_CSS_STUB;
      LOG_INFO("User CSS: seeded '{}'", user_css_path.string());
    } else {
      LOG_WARN("User CSS: cannot write '{}'", user_css_path.string());
    }
  }

  if (fs::exists(user_css_path)) {
    auto user_css = Gtk::CssProvider::create();
    user_css->signal_parsing_error().connect(
        [user_css_path](const Glib::RefPtr<const Gtk::CssSection> &section,
                        const Glib::Error &error) {
          LOG_WARN("User CSS parse error in '{}': {}",
                   user_css_path.string(), error.what());
          (void)section;
        });
    try {
      user_css->load_from_path(user_css_path.string());
      Gtk::StyleContext::add_provider_for_display(
          get_display(), user_css, GTK_STYLE_PROVIDER_PRIORITY_USER);
      LOG_INFO("User CSS: loaded '{}'", user_css_path.string());
    } catch (const Glib::Error &e) {
      LOG_WARN("User CSS: failed to load '{}': {}",
               user_css_path.string(), e.what());
    }
  }

  // S117 m2: setup_project must run BEFORE setup_headerbar.
  //
  // GTK4's CSD (client-side decoration) window inserts a `box.vertical`
  // wrapper between the window and its content, including the headerbar.
  // We added a CSS rule for that wrapper (window.csd > box.vertical)
  // — the rule resolves correctly at the cascade level. But on a freshly
  // launched light-motif project, the wrapper still painted black.
  //
  // Cause: GTK4 resolves and caches the style of the CSD wrapper +
  // headerbar at window-construction time. Adding the `curvz-light`
  // class to the window AFTER that point (which is what setup_project()
  // does, via apply_motif_to_window) does not trigger those cached
  // surfaces to re-walk the cascade. They keep painting whatever they
  // resolved on first pass — the dark token values.
  //
  // Symptom: app boots dark on light-motif projects. Opening GTK
  // Inspector forces a global style invalidation that finally re-walks
  // the CSD wrapper — and the headerbar/wrapper turn light. Closing
  // inspector reverts (style cache restored).
  //
  // Fix: load the project (which sets m_project->motif) and apply the
  // motif class to `this` BEFORE setup_headerbar(). The class is on
  // the window when the CSD wrapper + headerbar resolve their style
  // the first time, so they pick up the light tokens directly.
  setup_project();
  setup_headerbar();
  setup_menu();
  setup_layout();
  connect_signals();

  LOG_INFO("MainWindow created");
}

// ── Debounced save ───────────────────────────────────────────────────────────
// Coalesces rapid changes (spinbox scroll, pane drag) into one write
// by resetting a 400ms timer on every call.
void MainWindow::schedule_save() {
  if (!m_project || m_project->directory.empty())
    return;
  m_save_timer.disconnect();
  m_save_timer = Glib::signal_timeout().connect(
      [this]() -> bool {
        if (m_project && !m_project->directory.empty()) {
          // Defer if a drag is in-flight — retry in 100ms
          if (m_canvas.is_dragging()) {
            schedule_save();
            return false;
          }
          m_project->save();
        }
        return false; // one-shot
      },
      400);
}

// s113 m2: gated outline toggle.
//
// In preview/normal rendering mode, drop-shadow rendering allocates a
// Cairo buffer sized to the doc bbox in *device pixels* (doc_size ×
// zoom). At extreme zoom the allocation is unbounded and crashes the
// app. The hazard is concentrated on the outline→preview transition —
// a cliff, not a ramp — because zooming up *within* preview is gradual
// and self-warning (lag builds visibly), but flipping to preview while
// already deep-zoomed in outline mode is an instant cliff.
//
// This helper:
//   • Always allows preview→outline (the safe direction).
//   • Always allows outline→preview when zoom is below the threshold.
//   • Refuses outline→preview when zoom is above the threshold and shows
//     an AlertDialog telling the user to zoom out first.
//
// Returns true if the toggle happened, false if it was refused. Caller
// is responsible for syncing action state and statusbar after a true.
bool MainWindow::try_toggle_outline_safely() {
  if (!m_canvas.preview_safe_at_current_zoom()) {
    curvz::utils::show_alert(
        *this, "Zoom too high for preview mode",
        "Switching to preview mode at this zoom level may crash the app "
        "because of how preview rendering allocates memory at high zoom. "
        "\n\n"
        "Zoom out first, then switch to preview. Outline view stays safe "
        "at any zoom.");
    return false;
  }
  m_canvas.toggle_outline_mode();
  return true;
}

// ── Setup
// ─────────────────────────────────────────────────────────────────────

void MainWindow::setup_headerbar() {
  curvz::utils::set_name(m_headerbar, "mw_hb", "main_window_header_bar");
  m_headerbar.set_show_title_buttons(true);

  // S60 M3: Title widget is set below to the DocTabBar, which fills the
  // full centre region between pack_start and pack_end. No empty-box
  // placeholder needed.

  // App logo button — far left, opens About dialog.
  // S117 m16 v2: was a two-asset swap (curvz-logo-light.svg /
  // curvz-logo-dark.svg) selected in apply_motif_to_window. Logo is
  // single-colour, so symbolic is the right shape — set_icon_name
  // uses the existing GTK icon machinery, gets recoloured by CSS
  // `color` (which already follows motif via --fg-primary), no
  // motif-aware code needed. Same lesson as the layer-row eye fix in
  // m12. Asset is curvz-logo-symbolic.svg (currentColor fill).
  m_logo_btn.set_icon_name("curvz-logo-symbolic");
  m_logo_btn.set_has_frame(false);
  m_logo_btn.set_tooltip_text("About Curvz");
  curvz::utils::set_name(m_logo_btn, "mw_logo", "main_window_logo_btn");
  m_logo_btn.signal_clicked().connect([this]() {
    // S117 m19: custom About dialog. Replaces Gtk::AboutDialog (stock,
    // GNOME-styled, can't take our curvz-light class). Uses Gtk::Stack
    // with SLIDE_UP_DOWN transition for the About ↔ Credits flip,
    // mirroring the stock dialog's animation. Inherits motif via the
    // standard helper. Logo uses curvz-logo-symbolic at 96px so the
    // last asset-pair consumer is gone — Path A resources can be
    // retired in a follow-up cleanup.
    auto* dlg = new Gtk::Window();
    dlg->set_title("About Curvz");
    dlg->set_modal(true);
    dlg->set_resizable(false);
    dlg->set_default_size(440, 460);
    dlg->set_transient_for(*this);
    curvz::utils::apply_motif_class_from_parent(*dlg, *this);
    // Self-managed lifecycle: deleted via close_request handler so a
    // closed-and-reopened sequence doesn't leak.
    dlg->signal_close_request().connect(
        [dlg]() {
          Glib::signal_idle().connect_once([dlg]() { delete dlg; });
          return false;  // allow close to proceed
        }, false);

    // ── Headerbar (matches other Curvz dialogs)
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(true);
    dlg->set_titlebar(*hb);

    // ── Stack: page 1 = About, page 2 = Credits, with slide animation
    auto* stack = Gtk::make_managed<Gtk::Stack>();
    stack->set_transition_type(Gtk::StackTransitionType::SLIDE_UP_DOWN);
    stack->set_transition_duration(220);  // matches GTK default-ish
    stack->set_vexpand(true);
    stack->set_hexpand(true);

    // ── Page 1: About ──────────────────────────────────────────────
    auto* about_page = Gtk::make_managed<Gtk::Box>(
        Gtk::Orientation::VERTICAL);
    about_page->set_spacing(8);
    about_page->set_margin_top(24);
    about_page->set_margin_bottom(16);
    about_page->set_margin_start(24);
    about_page->set_margin_end(24);
    about_page->set_halign(Gtk::Align::CENTER);

    // Hero logo (96px, symbolic — follows motif via CSS `color`).
    auto* logo = Gtk::make_managed<Gtk::Image>();
    logo->set_from_icon_name("curvz-logo-symbolic");
    logo->set_pixel_size(96);
    logo->set_margin_bottom(8);
    about_page->append(*logo);

    auto* name_lbl = Gtk::make_managed<Gtk::Label>();
    name_lbl->set_markup("<span size='xx-large' weight='bold'>Curvz</span>");
    about_page->append(*name_lbl);

    auto* version_lbl = Gtk::make_managed<Gtk::Label>("Version 0.1");
    version_lbl->add_css_class("dim-label");
    about_page->append(*version_lbl);

    auto* tagline_lbl = Gtk::make_managed<Gtk::Label>(
        "A professional SVG icon editor for Linux.");
    tagline_lbl->set_margin_top(12);
    tagline_lbl->set_wrap(true);
    tagline_lbl->set_justify(Gtk::Justification::CENTER);
    tagline_lbl->set_max_width_chars(40);
    about_page->append(*tagline_lbl);

    auto* desc_lbl = Gtk::make_managed<Gtk::Label>(
        "Built on GTK4 and Cairo. Produces currentColor-aware SVG "
        "icons that adapt to light and dark themes at any resolution.");
    desc_lbl->set_wrap(true);
    desc_lbl->set_justify(Gtk::Justification::CENTER);
    desc_lbl->set_max_width_chars(48);
    desc_lbl->add_css_class("dim-label");
    desc_lbl->set_margin_top(4);
    about_page->append(*desc_lbl);

    auto* copy_lbl = Gtk::make_managed<Gtk::Label>("© 2026 Scott Combs");
    copy_lbl->set_margin_top(16);
    copy_lbl->add_css_class("dim-label");
    about_page->append(*copy_lbl);

    auto* license_lbl = Gtk::make_managed<Gtk::Label>(
        "Released under the MIT License");
    license_lbl->add_css_class("dim-label");
    about_page->append(*license_lbl);

    stack->add(*about_page, "about");

    // ── Page 2: Credits ────────────────────────────────────────────
    auto* credits_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    credits_scroll->set_policy(Gtk::PolicyType::NEVER,
                                Gtk::PolicyType::AUTOMATIC);
    credits_scroll->set_vexpand(true);
    auto* credits_page = Gtk::make_managed<Gtk::Box>(
        Gtk::Orientation::VERTICAL);
    credits_page->set_spacing(4);
    credits_page->set_margin_top(20);
    credits_page->set_margin_bottom(16);
    credits_page->set_margin_start(28);
    credits_page->set_margin_end(28);

    auto add_section = [&](const char* heading,
                            const std::vector<std::string>& items) {
      auto* h = Gtk::make_managed<Gtk::Label>();
      h->set_markup(Glib::ustring("<b>") + heading + "</b>");
      h->set_halign(Gtk::Align::CENTER);
      h->set_margin_top(12);
      h->set_margin_bottom(4);
      credits_page->append(*h);
      for (const auto& it : items) {
        auto* row = Gtk::make_managed<Gtk::Label>(it);
        row->set_halign(Gtk::Align::CENTER);
        row->set_wrap(true);
        row->set_justify(Gtk::Justification::CENTER);
        credits_page->append(*row);
      }
    };

    add_section("Authors",
                {"Scott Combs — Engineer / Designer",
                 "Claude (Anthropic) — AI Pair Programmer"});
    add_section("Libraries",
                {"nlohmann/json — Niels Lohmann",
                 "spdlog — Gabi Melman",
                 "Cairo / cairomm — Carl Worth et al.",
                 "GTK / gtkmm — The GNOME Project",
                 "FreeType2 — The FreeType Project",
                 "Pango / PangoCairo — The GNOME Project",
                 "Clipper2 — Angus Johnson (Boost Software License 1.0)",
                 "cmark — John MacFarlane (MIT)"});

    credits_scroll->set_child(*credits_page);
    stack->add(*credits_scroll, "credits");

    // ── Bottom bar: Credits/About toggle + Close ───────────────────
    auto* btn_row = Gtk::make_managed<Gtk::Box>(
        Gtk::Orientation::HORIZONTAL);
    btn_row->set_spacing(8);
    btn_row->set_margin_top(8);
    btn_row->set_margin_bottom(12);
    btn_row->set_margin_start(12);
    btn_row->set_margin_end(12);

    auto* btn_flip = Gtk::make_managed<Gtk::Button>("Credits");
    auto* btn_close = Gtk::make_managed<Gtk::Button>("Close");
    btn_close->add_css_class("suggested-action");

    // Flip button toggles the stack page and updates its own label.
    btn_flip->signal_clicked().connect(
        [stack, btn_flip]() {
          Glib::ustring current = stack->get_visible_child_name();
          if (current == "about") {
            stack->set_visible_child("credits");
            btn_flip->set_label("About");
          } else {
            stack->set_visible_child("about");
            btn_flip->set_label("Credits");
          }
        });
    btn_close->signal_clicked().connect([dlg]() { dlg->close(); });

    // Layout: flip button hugs the left, close button hugs the right.
    btn_row->append(*btn_flip);
    auto* spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    btn_row->append(*spacer);
    btn_row->append(*btn_close);

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    root->append(*stack);
    root->append(*btn_row);
    dlg->set_child(*root);

    dlg->present();
  });
  m_headerbar.pack_start(m_logo_btn);

  // S60 M3: "Documents" label + vertical separator packed into
  // pack_start immediately after the logo so they hug the left edge.
  // Previously these lived inside DocTabBar itself, but when the
  // DocTabBar is the title widget the CenterBox layout creates a
  // ~169px gap between pack_start and the center slot, so the label
  // visibly drifted right of the logo. Moving label + separator into
  // pack_start puts them in the start-slot's Box, immediately after
  // the logo, where they belong visually.
  auto *doc_label = Gtk::make_managed<Gtk::Label>("Documents");
  doc_label->add_css_class("doc-tab-bar-label");
  doc_label->set_margin_start(8);
  doc_label->set_margin_end(6);
  m_headerbar.pack_start(*doc_label);

  auto *doc_sep =
      Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
  doc_sep->set_margin_top(8);
  doc_sep->set_margin_bottom(8);
  doc_sep->set_margin_end(4);
  m_headerbar.pack_start(*doc_sep);

  // Right-click on the "Documents" label → "New Document" context menu.
  // Wired directly against the label so clicking it still opens the
  // bar menu even though the label no longer lives inside the
  // DocTabBar widget tree.
  auto bar_ctx = Gtk::GestureClick::create();
  bar_ctx->set_button(3);
  bar_ctx->signal_pressed().connect(
      [this](int, double /*x*/, double /*y*/) { m_doc_tabs.signal_add_doc().emit(); });
  doc_label->add_controller(bar_ctx);

  // S60 M3: DocTabBar as the HeaderBar title widget so the scroll +
  // chevrons fill all space between pack_start and pack_end.
  //
  // Deferred via signal_idle for the same reason the statusbar is in M1:
  // synchronous attachment at this point causes GTK's frame_clock_paint_idle
  // to race the size-allocate pass and emit a "snapshot without allocation"
  // warning for the title-widget Box. The idle tick lets layout settle
  // first.
  m_doc_tabs.set_hexpand(true);
  m_doc_tabs.set_halign(Gtk::Align::FILL);
  Glib::signal_idle().connect_once([this]() {
    m_headerbar.set_title_widget(m_doc_tabs);
  });

  // Hamburger menu — right side of headerbar
  m_hamburger.set_icon_name("open-menu-symbolic");
  m_hamburger.set_tooltip_text("Menu");
  m_hamburger.set_has_frame(false);
  curvz::utils::set_name(m_hamburger, "mw_ham", "main_window_hamburger_btn");
  m_headerbar.pack_end(m_hamburger);

  set_titlebar(m_headerbar);
}

void MainWindow::setup_menu() {
  // ── Build Gio::Menu ────────────────────────────────────────────────────
  auto menu = Gio::Menu::create();

  // ── File submenu ────────────────────────────────────────────────────────
  auto file_menu = Gio::Menu::create();
  file_menu->append("New Project…", "win.new-project");
  file_menu->append("Add to Project…", "win.new");
  file_menu->append("Open…", "win.open");
  // s125 m1d: "Open Image…" sits next to "Open…" because the user model
  // is "open this image as a document" — both verbs create a new doc.
  // Earlier m1c put this in the I/O section as "Place Image as Document…",
  // grouped with Place/Import/Export, but that obscured the relationship
  // with Open. Verb-by-shape > verb-by-data-type for menu grouping.
  file_menu->append("Open Image…", "win.open-image");
  file_menu->append("Close Project", "win.close-project");
  file_menu->append("Save", "win.save");
  file_menu->append("Save As…", "win.save-as");
  file_menu->append("Save as Template…", "win.save-as-template");
  file_menu->append("Manage Templates…", "win.manage-templates");
  auto file_io = Gio::Menu::create();
  file_io->append("Import SVG…", "win.import-svg");
  file_io->append("Import as Icon…", "win.import-svg-icon");
  file_io->append("Place Image…", "win.place-image");
  file_io->append("Export Icon Theme…", "win.export-theme");
  file_io->append("Print…", "win.print");
  file_menu->append_section("", file_io);
  auto file_section = Gio::Menu::create();
  file_section->append_submenu("File", file_menu);
  menu->append_section("", file_section);

  // ── Edit submenu ────────────────────────────────────────────────────────
  auto edit_menu = Gio::Menu::create();
  edit_menu->append("Undo", "win.undo");
  edit_menu->append("Redo", "win.redo");
  auto edit_sep = Gio::Menu::create();
  edit_sep->append("Cut", "win.cut");
  edit_sep->append("Copy", "win.copy");
  edit_sep->append("Paste", "win.paste");
  edit_sep->append("Duplicate", "win.duplicate");
  edit_sep->append("Clone", "win.clone");
  edit_sep->append("Step and Repeat…", "win.step-repeat");
  edit_menu->append_section("", edit_sep);
  auto edit_section = Gio::Menu::create();
  edit_section->append_submenu("Edit", edit_menu);
  menu->append_section("", edit_section);

  // ── Arrange submenu ─────────────────────────────────────────────────────
  auto arrange_menu = Gio::Menu::create();
  arrange_menu->append("Bring to Front", "win.arrange-bring-front");
  arrange_menu->append("Bring Forward", "win.arrange-bring-forward");
  arrange_menu->append("Send Backward", "win.arrange-send-backward");
  arrange_menu->append("Send to Back", "win.arrange-send-back");
  arrange_menu->append("Flip Horizontal", "win.flip-horizontal");
  arrange_menu->append("Flip Vertical", "win.flip-vertical");
  auto arrange_section = Gio::Menu::create();
  arrange_section->append_submenu("Arrange", arrange_menu);
  menu->append_section("", arrange_section);

  // ── Path submenu ────────────────────────────────────────────────────────
  auto path_menu = Gio::Menu::create();
  path_menu->append("Union", "win.bool-union");
  path_menu->append("Subtract", "win.bool-subtract");
  path_menu->append("Intersect", "win.bool-intersect");
  auto path_compound = Gio::Menu::create();
  path_compound->append("Make Compound Path", "win.make-compound");
  path_compound->append("Split Compound Path", "win.split-compound");
  path_menu->append_section("", path_compound);
  auto path_ops = Gio::Menu::create();
  path_ops->append("Offset Path…", "win.offset-path");
  path_ops->append("Expand Stroke", "win.expand-stroke");
  path_ops->append("Convert Text to Path", "win.text-to-path");
  path_menu->append_section("", path_ops);
  // Clip section — "Clip" arms pick mode (next click on a path defines
  // the clip shape). "Release Clip" dissolves the selected ClipGroup
  // back into its components. Enable/disable is context-driven via
  // SimpleAction::set_enabled() on selection changes (not wired Stage 2).
  auto path_clip = Gio::Menu::create();
  path_clip->append("Clip", "win.clip-make");
  path_clip->append("Release Clip", "win.clip-release");
  path_menu->append_section("", path_clip);
  // Blend section — enabled only when exactly 2 Path nodes are selected.
  // The dialog (M3) and release/expand actions come in later milestones.
  auto path_blend = Gio::Menu::create();
  path_blend->append("Blend", "win.blend-make");
  path_blend->append("Release Blend", "win.blend-release");
  path_menu->append_section("", path_blend);
  // Warp section — Make enabled with 1 Path/Compound/Group selected.
  // Release and Flatten enabled when a Warp is the primary selection.
  // M3a: Make creates with identity envelope (no dialog yet). M3b adds
  // the Make/Edit Warp dialog with envelope presets and quality slider.
  auto path_warp = Gio::Menu::create();
  path_warp->append("Warp",          "win.warp-make");
  path_warp->append("Edit Warp",     "win.warp-edit");
  path_warp->append("Release Warp",  "win.warp-release");
  path_warp->append("Flatten Warp",  "win.warp-flatten");
  path_menu->append_section("", path_warp);
  auto path_section = Gio::Menu::create();
  path_section->append_submenu("Path", path_menu);
  menu->append_section("", path_section);

  // ── View submenu ────────────────────────────────────────────────────────
  auto view_menu = Gio::Menu::create();
  view_menu->append("Rulers", "win.toggle-rulers");
  view_menu->append("Outline Mode", "win.toggle-outline");
  // S93 m7: engine-selector menu section removed. Boolean ops are now
  // permanently routed through Clipper2; the hand-rolled SN engine is
  // retained on disk for reference but no longer reachable from the UI.
  auto view_section = Gio::Menu::create();

  // ── Zoom submenu ──────────────────────────────────────────────────────
  auto zoom_menu = Gio::Menu::create();
  zoom_menu->append("Zoom In", "win.zoom-in");
  zoom_menu->append("Zoom Out", "win.zoom-out");
  zoom_menu->append("Zoom to 100%", "win.zoom-100");
  zoom_menu->append("Zoom to 200%", "win.zoom-200");
  zoom_menu->append("Zoom to Selection", "win.zoom-selection");
  zoom_menu->append("Fit to Window", "win.zoom-fit");
  auto zoom_section = Gio::Menu::create();
  zoom_section->append_submenu("Zoom", zoom_menu);
  view_menu->append_section("", zoom_section);

  view_section->append_submenu("View", view_menu);
  menu->append_section("", view_section);

  // ── Project submenu (S103 m3) ──────────────────────────────────────────
  // Currently houses the Themes… item. New top-level menu — project-
  // scoped utilities that aren't file-IO and aren't editing belong
  // here. Future tenants might include "Project Settings…" or
  // "Migrate project…" if we ever need them.
  auto project_menu = Gio::Menu::create();
  project_menu->append("Themes…", "win.show-themes");
  project_menu->append("Export Documents…", "win.export-docs");
  auto project_section = Gio::Menu::create();
  project_section->append_submenu("Project", project_menu);
  menu->append_section("", project_section);

  // ── Navigate submenu (s108 m7) ─────────────────────────────────────────
  // Document navigation. Menu placement also forces GTK to register the
  // accels via the menu-item path, which has different precedence than
  // bare action accels — required for Ctrl+Tab to reach the action
  // before GTK4's built-in focus-traversal default consumes it.
  auto navigate_menu = Gio::Menu::create();
  navigate_menu->append("Next Document",     "win.doc-next");
  navigate_menu->append("Previous Document", "win.doc-prev");
  auto navigate_section = Gio::Menu::create();
  navigate_section->append_submenu("Navigate", navigate_menu);
  menu->append_section("", navigate_section);

  // ── App items ───────────────────────────────────────────────────────────
  auto app_section = Gio::Menu::create();
  app_section->append("Help",                "win.show-help");
  app_section->append("Keyboard Shortcuts", "win.show-shortcuts");
  app_section->append("Quit", "win.quit");
  menu->append_section("", app_section);

  // ── Wire Gio::SimpleActions ────────────────────────────────────────────
  // File
  auto act_new = Gio::SimpleAction::create("new");
  act_new->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_new(); });
  add_action(act_new);

  auto act_open = Gio::SimpleAction::create("open");
  act_open->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_open(); });
  add_action(act_open);

  auto act_new_project = Gio::SimpleAction::create("new-project");
  act_new_project->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_new_project(); });
  add_action(act_new_project);

  auto act_close_project = Gio::SimpleAction::create("close-project");
  act_close_project->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_close_project(); });
  add_action(act_close_project);

  auto act_save = Gio::SimpleAction::create("save");
  act_save->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_save(); });
  add_action(act_save);

  auto act_save_as = Gio::SimpleAction::create("save-as");
  act_save_as->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_save_as(); });
  add_action(act_save_as);

  auto act_save_as_template = Gio::SimpleAction::create("save-as-template");
  act_save_as_template->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_save_as_template(); });
  add_action(act_save_as_template);

  auto act_manage_templates = Gio::SimpleAction::create("manage-templates");
  act_manage_templates->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_manage_templates(); });
  add_action(act_manage_templates);

  // S103 m3 — Project → Themes…
  auto act_show_themes = Gio::SimpleAction::create("show-themes");
  act_show_themes->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_show_themes(); });
  add_action(act_show_themes);

  // S104 m1 — Project → Export Documents…
  auto act_export_docs = Gio::SimpleAction::create("export-docs");
  act_export_docs->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_export_docs(); });
  add_action(act_export_docs);

  auto act_import_svg = Gio::SimpleAction::create("import-svg");
  act_import_svg->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_import_svg(); });
  add_action(act_import_svg);

  auto act_import_svg_icon = Gio::SimpleAction::create("import-svg-icon");
  act_import_svg_icon->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_import_svg_as_icon(); });
  add_action(act_import_svg_icon);

  auto act_place_image = Gio::SimpleAction::create("place-image");
  act_place_image->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_place_image(); });
  add_action(act_place_image);

  // s125 m1d: Open Image — picks a raster file and creates a new
  // canvas-sized-to-image doc with the image at (0, 0) at 1:1. Sibling of
  // win.open in user model. (m1c originally named this place-image-as-doc
  // and grouped it with Place; renamed for clarity.)
  auto act_open_image = Gio::SimpleAction::create("open-image");
  act_open_image->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_open_image(); });
  add_action(act_open_image);

  auto act_export_theme = Gio::SimpleAction::create("export-theme");
  act_export_theme->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_export_theme(); });
  add_action(act_export_theme);

  auto act_print = Gio::SimpleAction::create("print");
  act_print->signal_activate().connect([this](const Glib::VariantBase &) {
    if (m_project)
      m_print_manager.show(*this, *m_project);
  });
  add_action(act_print);

  // Edit
  auto act_undo = Gio::SimpleAction::create("undo");
  act_undo->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_undo(); });
  add_action(act_undo);

  auto act_redo = Gio::SimpleAction::create("redo");
  act_redo->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_redo(); });
  add_action(act_redo);

  auto act_cut = Gio::SimpleAction::create("cut");
  act_cut->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.cut_selected(); });
  add_action(act_cut);

  auto act_copy = Gio::SimpleAction::create("copy");
  act_copy->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.copy_selected(); });
  add_action(act_copy);

  auto act_paste = Gio::SimpleAction::create("paste");
  act_paste->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.paste_clipboard(); });
  add_action(act_paste);

  auto act_duplicate = Gio::SimpleAction::create("duplicate");
  act_duplicate->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.duplicate_selected(); });
  add_action(act_duplicate);

  auto act_clone = Gio::SimpleAction::create("clone");
  act_clone->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.clone_selected(); });
  add_action(act_clone);

  auto act_step_repeat = Gio::SimpleAction::create("step-repeat");
  act_step_repeat->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_step_repeat(); });
  add_action(act_step_repeat);

  // Flip
  auto act_flip_h = Gio::SimpleAction::create("flip-horizontal");
  act_flip_h->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.flip_selection(true); });
  add_action(act_flip_h);

  auto act_flip_v = Gio::SimpleAction::create("flip-vertical");
  act_flip_v->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.flip_selection(false); });
  add_action(act_flip_v);

  // Arrange
  auto act_bring_front = Gio::SimpleAction::create("arrange-bring-front");
  act_bring_front->signal_activate().connect([this](const Glib::VariantBase &) {
    m_canvas.arrange(Canvas::ArrangeOp::BringToFront);
  });
  add_action(act_bring_front);

  auto act_bring_fwd = Gio::SimpleAction::create("arrange-bring-forward");
  act_bring_fwd->signal_activate().connect([this](const Glib::VariantBase &) {
    m_canvas.arrange(Canvas::ArrangeOp::BringForward);
  });
  add_action(act_bring_fwd);

  auto act_send_bwd = Gio::SimpleAction::create("arrange-send-backward");
  act_send_bwd->signal_activate().connect([this](const Glib::VariantBase &) {
    m_canvas.arrange(Canvas::ArrangeOp::SendBackward);
  });
  add_action(act_send_bwd);

  auto act_send_back = Gio::SimpleAction::create("arrange-send-back");
  act_send_back->signal_activate().connect([this](const Glib::VariantBase &) {
    m_canvas.arrange(Canvas::ArrangeOp::SendToBack);
  });
  add_action(act_send_back);

  // Boolean path operations
  // s122 m2: stored as members so update_bool_actions_sensitive() can
  // toggle enabled state — exactly-2 gate keeps the not-yet-stable N>=3
  // iterative fold unreachable from the UI. Default-disabled (no
  // selection on startup); the sensitivity pump turns them on when
  // exactly 2 closed paths are selected.
  m_act_bool_union = Gio::SimpleAction::create("bool-union");
  m_act_bool_union->signal_activate().connect([this](const Glib::VariantBase &) {
    m_canvas.boolean_op(BooleanOpType::Union);
  });
  m_act_bool_union->set_enabled(false);
  add_action(m_act_bool_union);

  m_act_bool_subtract = Gio::SimpleAction::create("bool-subtract");
  m_act_bool_subtract->signal_activate().connect([this](const Glib::VariantBase &) {
    m_canvas.boolean_op(BooleanOpType::Subtract);
  });
  m_act_bool_subtract->set_enabled(false);
  add_action(m_act_bool_subtract);

  m_act_bool_intersect = Gio::SimpleAction::create("bool-intersect");
  m_act_bool_intersect->signal_activate().connect([this](const Glib::VariantBase &) {
    m_canvas.boolean_op(BooleanOpType::Intersect);
  });
  m_act_bool_intersect->set_enabled(false);
  add_action(m_act_bool_intersect);

  auto act_offset_path = Gio::SimpleAction::create("offset-path");
  act_offset_path->signal_activate().connect([this](const Glib::VariantBase &) {
    auto *doc = m_project ? m_project->active_doc() : nullptr;
    const CanvasModel *cm = doc ? &doc->canvas : nullptr;
    m_offset_path_dialog.show(*this, cm,
        [this](OffsetPathDialog::Options opts) {
      m_canvas.offset_path_op(opts.distance, opts.side, opts.keep_original);
    });
  });
  add_action(act_offset_path);

  auto act_expand_stroke = Gio::SimpleAction::create("expand-stroke");
  act_expand_stroke->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.expand_stroke_op(); });
  add_action(act_expand_stroke);

  auto act_make_compound = Gio::SimpleAction::create("make-compound");
  act_make_compound->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.make_compound_path(); });
  add_action(act_make_compound);

  auto act_split_compound = Gio::SimpleAction::create("split-compound");
  act_split_compound->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.split_compound_path(); });
  add_action(act_split_compound);

  // Clip / Release Clip. Context-awareness for menu-item enable/disable
  // is deferred — the Canvas methods themselves are defensive (no-op on
  // invalid state, with LOG_INFO). Adding set_enabled() on a selection
  // signal is the natural next pass once UX confirms the flow.
  m_act_clip_make = Gio::SimpleAction::create("clip-make");
  m_act_clip_make->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.make_clip_group(); });
  m_act_clip_make->set_enabled(false);
  add_action(m_act_clip_make);

  m_act_clip_release = Gio::SimpleAction::create("clip-release");
  m_act_clip_release->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.release_clip_group(); });
  m_act_clip_release->set_enabled(false);
  add_action(m_act_clip_release);

  // Blend — exactly 2 selected. Canvas::make_blend does final validation
  // and emits a user-visible error if preconditions aren't met; the
  // sensitivity hook below prevents the common "wrong count" case from
  // even hitting that path.
  m_act_blend_make = Gio::SimpleAction::create("blend-make");
  m_act_blend_make->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_blend(); });
  m_act_blend_make->set_enabled(false);
  add_action(m_act_blend_make);

  m_act_blend_release = Gio::SimpleAction::create("blend-release");
  m_act_blend_release->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.release_blend(); });
  m_act_blend_release->set_enabled(false);
  add_action(m_act_blend_release);

  // Warp — three actions: make, release, flatten. All default-disabled;
  // update_warp_action_sensitive turns them on based on selection. Make
  // goes through on_warp_make so M3b's dialog can intercept; release
  // and flatten are direct canvas-method dispatches (no UI to show).
  m_act_warp_make = Gio::SimpleAction::create("warp-make");
  m_act_warp_make->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_warp_make(); });
  m_act_warp_make->set_enabled(false);
  add_action(m_act_warp_make);

  m_act_warp_edit = Gio::SimpleAction::create("warp-edit");
  m_act_warp_edit->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_warp_edit(); });
  m_act_warp_edit->set_enabled(false);
  add_action(m_act_warp_edit);

  m_act_warp_release = Gio::SimpleAction::create("warp-release");
  m_act_warp_release->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_warp_release(); });
  m_act_warp_release->set_enabled(false);
  add_action(m_act_warp_release);

  m_act_warp_flatten = Gio::SimpleAction::create("warp-flatten");
  m_act_warp_flatten->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_warp_flatten(); });
  m_act_warp_flatten->set_enabled(false);
  add_action(m_act_warp_flatten);

  auto act_text_to_path = Gio::SimpleAction::create("text-to-path");
  act_text_to_path->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.text_to_paths_op(); });
  add_action(act_text_to_path);

  auto act_shortcuts = Gio::SimpleAction::create("show-shortcuts");
  act_shortcuts->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_shortcuts_dialog.show(*this); });
  add_action(act_shortcuts);

  // S100 m1 — in-app manual
  auto act_help = Gio::SimpleAction::create("show-help");
  act_help->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_help_window.show(*this); });
  add_action(act_help);

  // View — rulers toggle (stateful boolean action — shows checkmark)
  auto act_rulers =
      Gio::SimpleAction::create_bool("toggle-rulers", m_rulers_visible);
  act_rulers->signal_activate().connect(
      [this, act_rulers](const Glib::VariantBase &) {
        m_rulers_visible = !m_rulers_visible;
        act_rulers->set_state(Glib::Variant<bool>::create(m_rulers_visible));
        toggle_rulers(m_rulers_visible);
      });
  add_action(act_rulers);

  // View — outline mode toggle (stateful)
  // s113 m2: gate the outline→preview transition when current zoom would
  // make preview rendering crash the app (drop-shadow Cairo buffer scales
  // with device-pixel coverage; unbounded at high zoom). Outline mode is
  // safe at any zoom; only the transition INTO preview is dangerous.
  // s113 m3: action/statusbar sync now lives in the
  // signal_outline_mode_changed handler; this site just triggers the
  // toggle.
  auto act_outline = Gio::SimpleAction::create_bool("toggle-outline", false);
  act_outline->signal_activate().connect(
      [this](const Glib::VariantBase &) { try_toggle_outline_safely(); });
  add_action(act_outline);

  // S93 m7: toggle-clipper2 action removed. Boolean ops now run
  // exclusively through Clipper2; hand-rolled engine is retained on
  // disk for reference but no longer wired to the action system.

  auto act_zoom_in = Gio::SimpleAction::create("zoom-in");
  act_zoom_in->signal_activate().connect([this](const Glib::VariantBase &) {
    m_toolbar.signal_zoom_step().emit(+1.0);
  });
  add_action(act_zoom_in);

  auto act_zoom_out = Gio::SimpleAction::create("zoom-out");
  act_zoom_out->signal_activate().connect([this](const Glib::VariantBase &) {
    m_toolbar.signal_zoom_step().emit(-1.0);
  });
  add_action(act_zoom_out);

  auto act_zoom_fit = Gio::SimpleAction::create("zoom-fit");
  act_zoom_fit->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.zoom_to_all_objects(); });
  add_action(act_zoom_fit);

  auto act_zoom_actual = Gio::SimpleAction::create("zoom-actual");
  act_zoom_actual->signal_activate().connect([this](const Glib::VariantBase &) {
    // 1× = fit-zoom (artboard fills viewport with margin)
    double cx = m_canvas.get_width() / 2.0;
    double cy = m_canvas.get_height() / 2.0;
    double target = m_canvas.fit_zoom_value();
    m_canvas.zoom_toward(cx, cy, target / m_canvas.zoom());
  });
  add_action(act_zoom_actual);

  auto act_zoom_200 = Gio::SimpleAction::create("zoom-200");
  act_zoom_200->signal_activate().connect([this](const Glib::VariantBase &) {
    // 1× = fit-zoom (artboard fills viewport with margin)
    double cx = m_canvas.get_width() / 2.0;
    double cy = m_canvas.get_height() / 2.0;
    double target = m_canvas.fit_zoom_value() * 2.0;
    m_canvas.zoom_toward(cx, cy, target / m_canvas.zoom());
  });
  add_action(act_zoom_200);

  auto act_zoom_sel = Gio::SimpleAction::create("zoom-selection");
  act_zoom_sel->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.zoom_to_selection(); });
  add_action(act_zoom_sel);

  // ── Document navigation (s108 m7) ───────────────────────────────────────
  // Cycles through the project's documents — wraparound at both ends.
  // Funnels through on_doc_activated so the canvas/inspector/layers/
  // gallery/tabs all sync via the single canonical seam.
  auto act_doc_next = Gio::SimpleAction::create("doc-next");
  act_doc_next->signal_activate().connect(
      [this](const Glib::VariantBase &) { cycle_doc(+1); });
  add_action(act_doc_next);

  auto act_doc_prev = Gio::SimpleAction::create("doc-prev");
  act_doc_prev->signal_activate().connect(
      [this](const Glib::VariantBase &) { cycle_doc(-1); });
  add_action(act_doc_prev);

  // Quit
  auto act_quit = Gio::SimpleAction::create("quit");
  act_quit->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_quit(); });
  add_action(act_quit);

  // ── Attach popover to hamburger button ────────────────────────────────
  auto popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
  m_hamburger.set_popover(*popover);

  // ── Register keyboard accelerators with the application (S60 M2) ──────
  // Two purposes:
  //   1. GTK4 auto-displays the accel on the right side of each menu item
  //      (e.g. "Save    Ctrl+S"), removing the need for manual labeling.
  //   2. Menu items that previously had no keyboard access get one.
  //
  // Behavior note: existing shortcuts in signal_key_pressed (CAPTURE phase,
  // in connect_signals) still fire and consume the key before the accel
  // system can see it — so no double-dispatch for the shortcuts that have
  // a keyval handler. The new (gap) accels are only wired here and run
  // through the accel system exclusively.
  //
  // Accel string syntax: "<Control>s", "<Control><Shift>s",
  // "<Control><Alt>v", "<Alt>d". Multiple accels per action are allowed
  // (e.g. Redo = Ctrl+Shift+Z and Ctrl+Y).
  auto app = get_application();
  if (app) {
    // S117 m15: when a new top-level window is registered (e.g. a
    // dialog opens), sync the motif class so it inherits the current
    // project's appearance. Combined with the walk in
    // apply_motif_to_window, this covers both "dialog open at toggle
    // time" and "dialog opens after toggle".
    // S117 m17 diag: log so we can verify whether this signal actually
    // fires for transient dialogs created via set_transient_for. If
    // the log shows nothing when a dialog opens, the dialog never
    // registered with the application and we rely on the
    // gtk_window_get_toplevels walk in apply_motif_to_window instead.
    app->signal_window_added().connect(
        [this](Gtk::Window* w) {
          LOG_INFO("signal_window_added fired: w={}", (void*)w);
          if (w && w != this) sync_motif_class_to(*w);
        });

    auto bind = [&app](const char *action,
                       std::vector<Glib::ustring> accels) {
      app->set_accels_for_action(action, accels);
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
    bind("win.export-theme",      {"<Control><Shift>t"});
    bind("win.print",             {"<Control>p"});

    // Edit
    bind("win.undo",            {"<Control>z"});
    bind("win.redo",            {"<Control><Shift>z", "<Control>y"});
    bind("win.cut",             {"<Control>x"});
    bind("win.copy",            {"<Control>c"});
    bind("win.paste",           {"<Control>v"});
    bind("win.duplicate",       {"<Control>d"});
    bind("win.clone",           {"<Alt>d"});
    bind("win.step-repeat",     {"<Control><Alt>d"});

    // Arrange
    bind("win.arrange-bring-front",    {"<Control><Shift>Up"});
    bind("win.arrange-bring-forward",  {"<Control>Up"});
    bind("win.arrange-send-backward",  {"<Control>Down"});
    bind("win.arrange-send-back",      {"<Control><Shift>Down"});
    bind("win.flip-horizontal",        {"<Control><Shift>h"});
    bind("win.flip-vertical",          {"<Control><Alt>v"});

    // Path
    bind("win.bool-union",      {"<Control><Shift>u"});
    bind("win.bool-subtract",   {"<Control><Shift>e"});
    bind("win.bool-intersect",  {"<Control><Shift>i"});
    bind("win.make-compound",   {"<Control>8"});
    bind("win.split-compound",  {"<Control><Shift>8"});
    bind("win.offset-path",     {"<Control><Shift>o"});
    bind("win.expand-stroke",   {"<Control><Shift>x"});
    bind("win.text-to-path",    {"<Control><Alt>t"});
    bind("win.clip-make",       {"<Control>7"});
    bind("win.clip-release",    {"<Control><Alt>7"});
    bind("win.blend-make",      {"<Control>b"});
    bind("win.blend-release",   {"<Control><Shift>b"});
    bind("win.warp-make",       {"<Control><Shift>y"});
    bind("win.warp-edit",       {"<Control><Alt>y"});
    // warp-release intentionally menu-only (4-key combo not muscle memory)
    bind("win.warp-flatten",    {"<Control><Alt>f"});

    // View
    bind("win.toggle-rulers",   {"<Control>r"});
    bind("win.toggle-outline",  {"<Control>e"});
    bind("win.zoom-in",         {"plus", "equal", "KP_Add"});
    bind("win.zoom-out",        {"minus", "KP_Subtract"});
    bind("win.zoom-100",        {"1", "KP_1"});
    bind("win.zoom-200",        {"2", "KP_2"});
    bind("win.zoom-selection",  {"3", "KP_3", "<Control>3"});
    bind("win.zoom-fit",        {"0", "KP_0", "<Control>0"});

    // Document navigation — both Tab-style (browser/IDE muscle memory)
    // and Page-Up/Down (keyboards without dedicated PgUp/PgDn keys still
    // get a working pair via Tab; full-keyboard users get the
    // conventional PageUp/PageDown).
    bind("win.doc-next",        {"<Control>Tab",       "<Control>Page_Down"});
    bind("win.doc-prev",        {"<Control><Shift>Tab", "<Control>Page_Up"});

    // App
    bind("win.show-help",       {"F1", "<Alt>question"});
    bind("win.show-shortcuts",  {"question", "slash"});
    bind("win.quit",            {"<Control>q", "<Control>w"});
  }
}

void MainWindow::toggle_rulers(bool visible) {
  m_hruler.set_visible(visible);
  m_vruler.set_visible(visible);
  m_corner.set_visible(visible);
  LOG_DEBUG("Rulers {}", visible ? "shown" : "hidden");
}

void MainWindow::setup_project() {
  // Try to reopen last project
  std::string last = load_last_project_path();
  if (!last.empty() && fs::exists(last)) {
    auto project = CurvzProject::open(last);
    if (project) {
      m_project = std::move(project);
      // Propagate project-wide snap to all docs so inspector reads correct
      // values
      for (auto &doc : m_project->documents)
        doc->snap = m_project->snap;
      update_title();
      // s116 fix2: apply motif now (before setup_layout creates widgets)
      // so the CSS class is on the window when children are first styled.
      // Without this, the project's light-motif setting is read into
      // m_project->motif but the curvz-light class is never added until
      // a later trigger (active-doc switch, inspector edit) — meaning
      // the app boots dark even when the project says Light.
      apply_motif_to_window();
      return;
    }
  }
  // Fall back to blank project
  m_project = std::make_unique<CurvzProject>();
  auto doc = std::make_unique<CurvzDocument>();
  doc->canvas = CanvasModel::from_pixels(24, 24);
  doc->filename = "new-icon.svg";
  m_project->documents.push_back(std::move(doc));
  m_project->active_doc_index = 0;
  update_title();
  // Fresh blank project defaults to Dark, but apply for symmetry — keeps
  // class state consistent regardless of which project path executed.
  apply_motif_to_window();
}

Gtk::Box *MainWindow::make_section(const char *title, Gtk::Widget &child,
                                   bool expanded, bool vexpand_child,
                                   std::shared_ptr<bool> *out_flag) {
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->add_css_class("inspector-section");

  // ── Slim header row — a plain Box with GestureClick, not a Button ──────
  // This removes the full-width "pressable button" visual and replaces it
  // with a tight label row that has a small arrow on the left.
  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->add_css_class("inspector-section-header");
  hdr->set_spacing(5);

  auto *arrow = Gtk::make_managed<Gtk::Label>(expanded ? "▾" : "▸");
  arrow->add_css_class("inspector-arrow");

  auto *lbl = Gtk::make_managed<Gtk::Label>(title);
  lbl->set_xalign(0.0f);
  lbl->set_hexpand(true);
  lbl->add_css_class("inspector-section-title");

  hdr->append(*arrow);
  hdr->append(*lbl);
  outer->append(*hdr);

  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  outer->append(*sep);

  auto *body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  body->set_visible(expanded);
  if (vexpand_child)
    body->set_vexpand(true);
  body->append(child);
  outer->append(*body);

  // Track open state so we can save it
  // shared_ptr bool — owned by the closure, no raw new/delete needed
  auto open_flag = std::make_shared<bool>(expanded);

  // Click anywhere on the header row to toggle
  std::string sec_title = title;
  auto gesture = Gtk::GestureClick::create();
  gesture->signal_pressed().connect(
      [this, arrow, body, sec_title, open_flag](int, double, double) {
        bool on = !(*open_flag);
        *open_flag = on;
        body->set_visible(on);
        arrow->set_text(on ? "▾" : "▸");
        if (sec_title == "Preview")
          m_project->sec_preview_open = on;
        else if (sec_title == "Layers")
          m_project->sec_layers_open = on;
        else if (sec_title == "Library")
          m_project->sec_library_open = on;
        else if (sec_title == "Swatches")
          m_project->sec_swatches_open = on;
        else if (sec_title == "Documents")
          m_project->sec_documents_open = on;
        schedule_save();
      });
  hdr->add_controller(gesture);

  // Return the open_flag so the caller can drive open/close state
  if (out_flag)
    *out_flag = open_flag;
  return outer;
}

// Build a collapsible group header (Document/Object/Content style).
// No inline child widget — instead returns a container box for the
// caller to populate with make_section(...) children.  The container
// carries the inspector-group-container CSS class so its descendants
// are indented to indicate hierarchy.
MainWindow::GroupSection
MainWindow::make_group_section(const char *title, bool expanded,
                               std::shared_ptr<bool> *out_flag) {
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->add_css_class("inspector-section");

  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->add_css_class("inspector-section-header");
  hdr->set_spacing(5);

  auto *arrow = Gtk::make_managed<Gtk::Label>(expanded ? "▾" : "▸");
  arrow->add_css_class("inspector-arrow");

  auto *lbl = Gtk::make_managed<Gtk::Label>(title);
  lbl->set_xalign(0.0f);
  lbl->set_hexpand(true);
  lbl->add_css_class("inspector-section-title");
  lbl->add_css_class("inspector-group-title");

  hdr->append(*arrow);
  hdr->append(*lbl);
  outer->append(*hdr);

  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  outer->append(*sep);

  // Container — children go here; carries group-container class for indent.
  auto *container = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  container->add_css_class("inspector-group-container");
  container->set_visible(expanded);
  outer->append(*container);

  auto open_flag = std::make_shared<bool>(expanded);

  std::string sec_title = title;
  auto gesture = Gtk::GestureClick::create();
  gesture->signal_pressed().connect(
      [this, arrow, container, sec_title, open_flag](int, double, double) {
        bool on = !(*open_flag);
        *open_flag = on;
        container->set_visible(on);
        arrow->set_text(on ? "▾" : "▸");
        if (sec_title == "Content")
          m_project->sec_content_open = on;
        schedule_save();
      });
  hdr->add_controller(gesture);

  if (out_flag)
    *out_flag = open_flag;
  return {outer, container};
}

void MainWindow::setup_layout() {
  set_child(m_root);

  m_doc_tabs.set_project(m_project.get());
  m_gallery.set_project(m_project.get());

  m_root.append(m_middle);
  m_middle.set_vexpand(true);
  m_middle.append(m_toolbar);
  m_canvas_col.set_hexpand(true);
  m_canvas_col.set_vexpand(true);
  m_canvas_col.append(m_context_bar);
  m_canvas_col.append(m_h_paned);
  m_middle.append(m_canvas_col);
  m_h_paned.set_hexpand(true);
  m_h_paned.set_vexpand(true);

  // s132 m1: bridge ContextBar's right-click "Open Help" affordance to
  // the HelpWindow. ContextBar has no direct dependency on HelpWindow —
  // the callback type is std::function<void(string)> taking a gresource
  // path, so the bar can be tested in isolation and the wiring lives
  // here next to other tool/UI plumbing.
  m_context_bar.set_help_callback(
      [this](const std::string& resource_path) {
        m_help_window.open_at(*this, resource_path);
      });

  m_canvas_grid.set_hexpand(true);
  m_canvas_grid.set_vexpand(true);
  m_canvas_grid.set_row_spacing(0);
  m_canvas_grid.set_column_spacing(0);
  curvz::utils::set_name(m_corner, "mw_crn", "main_window_corner_square");
  curvz::utils::set_name(m_hruler, "mw_hr", "main_window_h_ruler");
  curvz::utils::set_name(m_vruler, "mw_vr", "main_window_v_ruler");
  m_canvas_grid.attach(m_corner, 0, 0, 1, 1);
  m_canvas_grid.attach(m_hruler, 1, 0, 1, 1);
  m_canvas_grid.attach(m_vruler, 0, 1, 1, 1);
  m_canvas_grid.attach(m_canvas, 1, 1, 1, 1);

  m_canvas.set_document(m_project->active_doc());
  m_canvas.set_zoom(m_project->zoom);
  m_canvas.set_history(&m_history);
  m_canvas.set_swatch_library(&m_project->swatches);  // Phase 5 M3
  m_canvas.set_style_library(&m_project->styles);     // S78 m3d
  m_canvas.set_project(m_project.get());              // s116 m6 — workspace appearance reads
  m_toolbar.set_document(m_project->active_doc());
  // S91: wire the swatch library to the toolbar so the Swatch toggle
  // in the fill/stroke popovers becomes sensitive once the project
  // loads. Pre-S91, this call was missing — the toolbar's swatch
  // section and toggle stayed greyed out for the entire session even
  // when the project had swatches. Same library reference the canvas
  // and styles panel use; non-owning pointer.
  m_toolbar.set_swatch_library(&m_project->swatches);

  // Wrap the canvas grid in an overlay so the text-tool entry can float above
  // it.
  m_canvas_overlay.set_child(m_canvas_grid);
  m_text_fixed.set_can_target(false); // pass through mouse events to canvas
  m_canvas_overlay.add_overlay(m_text_fixed);
  m_canvas.set_text_overlay(&m_text_fixed);

  // Build corner treatment popover (anchored to toolbar corner button)
  build_corner_panel();

  m_h_paned.set_start_child(m_canvas_overlay);
  m_h_paned.set_resize_start_child(true);

  m_h_paned.set_end_child(m_right_scroll);
  m_h_paned.set_resize_end_child(false);
  // Defer set_position until after the paned is allocated — GTK4 ignores
  // set_position() called before the widget has a real size.
  auto *conn = new sigc::connection();
  *conn = m_h_paned.signal_map().connect([this, conn]() {
    conn->disconnect();
    delete conn;
    Glib::signal_idle().connect_once([this]() {
      int pos = m_project ? m_project->pane_position : -1;
      // S94 m1 — clamp pos so the right pane retains its minimum width
      // (m_right_scroll's set_size_request floor of 280px). Without this
      // clamp, a saved pos taken on a wide window can be larger than the
      // current paned width minus 280, which leaves the right pane too
      // narrow for its content — rightmost widgets (LayersPanel +/− header
      // buttons, swatch chips' right edges, etc.) get clipped by the
      // window. Cause was state, not layout.
      const int RIGHT_PANE_MIN = 280;
      int paned_w = m_h_paned.get_width();
      if (pos > 0 && paned_w > 0 && pos > paned_w - RIGHT_PANE_MIN) {
        int clamped = std::max(100, paned_w - RIGHT_PANE_MIN);
        LOG_INFO("Pane position {} exceeds available width {}; "
                 "clamping to {} so right pane keeps {}px minimum",
                 pos, paned_w, clamped, RIGHT_PANE_MIN);
        pos = clamped;
      }
      LOG_INFO("Pane restore on map: setting position to {}", pos);
      if (m_project)
        m_h_paned.set_position(pos);
      LOG_INFO("Pane position after set: {}", m_h_paned.get_position());
      m_pane_ready = true;
      m_pane_settled_at =
          g_get_monotonic_time() + 500000; // 500ms settle window
    });
  });
  m_right_scroll.set_child(m_right_panels);
  m_right_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  m_right_scroll.set_size_request(280, -1);
  m_right_panels.set_vexpand(true);

  m_properties.set_project(m_project.get());
  m_properties.set_history(&m_history);
  m_properties.set_canvas_widget(&m_canvas); // S58f: multi-select broadcast for Appearance
  m_right_panels.append(m_properties);

  // Content group — wraps Layers, Library, Documents so they appear as a
  // single top-level collapsible (matching the Document/Object groups
  // inside the inspector).
  auto content = make_group_section("Content", false, &m_sec_open_content);
  content.outer->set_vexpand(true);      // let children (Layers) stretch
  content.container->set_vexpand(true);
  m_right_panels.append(*content.outer);

  // Preview panel intentionally not shown — code retained for future use
  // m_preview.set_document(m_project->active_doc());
  // content.container->append(*make_section("Preview", m_preview,
  //                                     m_project->sec_preview_open, false,
  //                                     &m_sec_open_preview));
  m_layers.set_document(m_project->active_doc());
  m_layers.set_vexpand(true);
  content.container->append(
      *make_section("Layers", m_layers, false, true, &m_sec_open_layers));

  // Layer selection → update active_layer_index (already set by panel,
  // but we also need to refresh inspector and save)
  m_layers.signal_layer_selected().connect([this](int) { schedule_save(); });

  m_layers.signal_layer_changed().connect([this]() {
    m_canvas.queue_draw();
    schedule_save();
    // Re-sync selection highlight after DnD reorder
    Glib::signal_idle().connect_once(
        [this]() { m_layers.set_canvas_selection(m_canvas.selection()); });
  });

  m_layers.signal_object_selected().connect([this](SceneNode *obj) {
    m_canvas.select_object(obj);
    // Refresh inspector with the newly selected object
    auto *doc = m_project->active_doc();
    if (doc) {
      Glib::signal_idle().connect_once([this, obj]() {
        if (m_closing)
          return;
        m_properties.show_object_props(obj);
      });
    }
  });

  m_layers.signal_multi_selected().connect(
      [this](std::vector<SceneNode *> sel) {
        m_canvas.set_multi_selection(sel);
      });
  m_layers.signal_run_macro().connect(
      sigc::mem_fun(*this, &MainWindow::on_run_macro));
  // Right-click Rebuild from LayersPanel → Canvas manual refresh.
  m_layers.signal_rebuild_blend().connect(
      [this](SceneNode *b) { m_canvas.rebuild_blend(b); });
  m_library.set_document(m_project->active_doc());
  m_library.set_project(m_project.get());  // s117 m14 v2 — thumb motif/artboard
  m_library.signal_place_item().connect(
      [this](const std::string &path) { m_canvas.import_svg_to_canvas(path); });
  m_library.signal_add_to_library().connect(
      [this](const std::string &dest_dir) {
        on_save_selection_to_library(dest_dir);
      });
  content.container->append(
      *make_section("Library", m_library, false, false, &m_sec_open_library));

  // Swatches section — Phase 4. Shows project-scoped swatch library.
  // Placed between Library and Documents so the Content group reads as
  // Layers (primary) → reference collections (Library, Swatches) →
  // Documents (project-level).
  m_swatches.set_library(&m_project->swatches);
  m_swatches.set_canvas(&m_canvas);
  m_swatches.signal_library_changed().connect([this]() {
    schedule_save();
    // S91: re-call set_swatch_library so the toolbar re-runs its
    // sensitivity check + popover refresh. Adding the first swatch
    // (or removing the last) needs to flip the Swatch toggle's
    // sensitive state; the library pointer hasn't changed, but the
    // count has.
    if (m_project) m_toolbar.set_swatch_library(&m_project->swatches);
  });
  // S83 m4h v3 → v4: panel emits this whenever a library mutation
  // could leave the inspector stale — bind-click, rename, recolour,
  // delete. Without it, the inspector's Appearance widgets and
  // binding annotation go stale because Canvas's
  // signal_document_changed only triggers sync_selection (bbox
  // spinners), which doesn't rebuild Appearance widgets. Mirrors
  // StylesPanel's signal_request_inspector_refresh wiring below;
  // deferred via signal_idle per the GTK4 widget-mutation deferral
  // idiom.
  //
  // Renamed from v3's signal_paint_applied: the original name only
  // covered bind-click; the v4 sites are library mutations broadly.
  m_swatches.signal_inspector_refresh_needed().connect([this]() {
      Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
  });
  content.container->append(
      *make_section("Swatches", m_swatches, false, false, &m_sec_open_swatches));

  // Styles section — S81 m4c skeleton. Sibling to Swatches, peer panel
  // (not nested inside Swatches). Project-scoped style library: app
  // tier (hardcoded stubs) + user tier (per-project, persisted via
  // m3b's JSON round-trip). m4c-1 is read-only; click-to-bind and
  // create flows land in m4c-2.
  m_styles.set_library(&m_project->styles);
  m_styles.set_swatch_library(&m_project->swatches);  // S85 cont-3
  // S87 — restore persisted dropdown selection. Must come AFTER
  // set_library() so the panel has a live library to look up the
  // category in; refresh() inside set_active_category will rebuild
  // with the right pre-selected index.
  m_styles.set_active_category(m_project->style_active_category,
                               m_project->style_active_is_app_tier);
  m_styles.set_canvas(&m_canvas);
  m_styles.set_history(&m_history);  // S80 m4c-2: undo for click-to-bind
  m_styles.signal_library_changed().connect([this]() { schedule_save(); });
  // S87 — view state changed (dropdown selection). Pull current state
  // into the project fields and trigger a save. Same debounced
  // schedule_save path as content mutations.
  m_styles.signal_view_state_changed().connect([this]() {
    if (!m_project) return;
    m_project->style_active_category    = m_styles.active_category();
    m_project->style_active_is_app_tier = m_styles.active_is_app_tier();
    schedule_save();
  });
  m_styles.set_canvas(&m_canvas);
  m_styles.set_history(&m_history);  // S80 m4c-2: undo for click-to-bind
  m_styles.signal_library_changed().connect([this]() { schedule_save(); });
  // S80 m4c-2: panel emits this after a click-to-bind so the inspector's
  // "Bound: <n>" indicator updates without StylesPanel reaching into
  // MainWindow internals. Deferred via signal_idle to match every other
  // refresh_inspector site (GTK4 widget-mutation deferral idiom).
  m_styles.signal_request_inspector_refresh().connect([this]() {
      Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
  });
  // S85 m4i-cont-1: Style Editor dialog request. Panel emits this
  // for "+ New style" / right-click "Edit…" / right-click "Edit a
  // copy…". MainWindow owns the dialog construction (it has the
  // transient parent and the project handle); the panel-supplied
  // on_committed closure carries the command-push logic so this
  // handler stays mechanical.
  //
  // Lifetime: the dialog is heap-allocated and self-deletes via
  // signal_close_request (see StyleEditorDialog ctor). One dialog
  // can be live at a time but we don't enforce that here — opening
  // a second while the first is still up is rare (modal-on-modal
  // is platform-discouraged) and the panel actions are only
  // reachable through the panel UI which is itself blocked by the
  // first modal's set_modal(true).
  m_styles.signal_request_style_editor().connect(
      [this](StyleEditorDialog::Mode mode,
             style::Style initial,
             std::function<void(style::Style)> on_committed) {
          if (!m_project) return;
          // CanvasModel lives on the active CurvzDocument (each doc
          // has its own canvas dimensions). Pass nullptr if no doc
          // is active — the dialog's spinbuttons fall back to a
          // unit-conversion-less mode in that case (defensive; a
          // dialog open with no active doc shouldn't be reachable
          // through normal UI).
          CurvzDocument* doc = m_project->active_doc();
          CanvasModel* cm = doc ? &doc->canvas : nullptr;
          auto cats = m_project->styles.user_categories();
          new StyleEditorDialog(*this, m_project->swatches, cm,
                                cats, mode, std::move(initial),
                                std::move(on_committed));
      });
  content.container->append(
      *make_section("Styles", m_styles, false, false, &m_sec_open_styles));

  content.container->append(*make_section("Documents", m_gallery, false, false,
                                          &m_sec_open_documents));

  // Save pane position when user drags divider — only after initial restore and
  // settle
  m_h_paned.property_position().signal_changed().connect([this]() {
    if (m_project && m_pane_ready &&
        g_get_monotonic_time() >= m_pane_settled_at) {
      int pos = m_h_paned.get_position();
      LOG_INFO("Pane position saved: {}", pos);
      m_project->pane_position = pos;
      schedule_save();
    } else {
      LOG_INFO("Pane position changed but not ready (ignored): {}",
               m_h_paned.get_position());
    }
  });

  // Status bar — deferred append to avoid snapshot-before-allocation
  // warning on startup (S60). If appended synchronously at the end of
  // setup_layout, GTK's frame_clock_paint_idle can tick between our
  // append and its next size-allocate pass, attempting to snapshot an
  // unallocated widget. Deferring via signal_idle lets the current
  // frame cycle complete first. Same pattern as S48 DocTabBar fix.
  Glib::signal_idle().connect_once([this]() {
    m_root.append(m_statusbar);
    m_statusbar.set_zoom(m_project->zoom * 100.0);
    refresh_status_counts();  // s132 m2 — was set_counts(0, 0)
  });

  // Initial ruler state (will also be refreshed when canvas draws first frame)
  update_rulers();

  // Populate inspector from restored project state once widget is realized
  Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
}

void MainWindow::connect_signals() {
  // Window close button (X) — GTK4 uses signal_close_request, not delete-event
  signal_close_request().connect(
      [this]() -> bool {
        if (!m_closing)
          on_quit();
        return false; // allow GTK to proceed with closing
      },
      false);

  m_toolbar.signal_tool_changed().connect(
      sigc::mem_fun(*this, &MainWindow::on_tool_changed));

  // DocTabBar signals — primary navigation
  m_doc_tabs.signal_doc_activated().connect(
      sigc::mem_fun(*this, &MainWindow::on_doc_activated));

  m_doc_tabs.signal_add_doc().connect([this]() {
    if (!m_project)
      return;
    m_new_doc_dialog.show(*this, ndd_available_themes(),
        [this](std::unique_ptr<CurvzDocument> seed,
               std::string name,
               std::optional<theme::ThemeId> theme_id) {
      if (!seed) return;
      // Apply chosen theme (if any) BEFORE assigning filename / pushing to
      // project. apply_theme_to_doc may add layers (grid, margin) that
      // weren't in the seed, and we want all that wiring done before the
      // doc is observable by the rest of the UI.
      ndd_apply_chosen_theme(*seed, theme_id);

      // Sanitise name → filename stem (see doc_stem_from_name comment).
      std::string base = doc_stem_from_name(name);
      std::string fname = base + ".svg";
      // Ensure unique within project
      int suffix = 2;
      while (std::any_of(m_project->documents.begin(),
                         m_project->documents.end(),
                         [&](const auto &d) { return d->filename == fname; }))
        fname = base + std::to_string(suffix++) + ".svg";

      seed->filename = fname;

      m_project->documents.push_back(std::move(seed));
      m_project->active_doc_index = (int)m_project->documents.size() - 1;
      m_project->save();
      update_all_panels();
      LOG_INFO("Added document '{}' to project", fname);
    });
  });

  m_doc_tabs.signal_remove_doc().connect([this](int idx) {
    if (!m_project) return;
    if (idx < 0 || idx >= (int)m_project->documents.size())
      return;
    m_project->documents.erase(m_project->documents.begin() + idx);
    // Clamp active index, but allow empty gallery (-> -1 is fine; active_doc()
    // returns nullptr in that case, and update_all_panels handles it).
    if (m_project->documents.empty()) {
      m_project->active_doc_index = 0;  // harmless; active_doc() guards empty
    } else {
      m_project->active_doc_index =
          std::min(idx, (int)m_project->documents.size() - 1);
    }
    m_project->save();
    // Defer panel rebuild — we're inside a click handler on the doc-tab's
    // close button. update_all_panels() tears down and rebuilds the tab bar
    // and gallery subtrees, which invalidates the button that just fired
    // this handler. Running it synchronously produces
    //   "Trying to snapshot gtkmm__GtkBox ... without a current allocation"
    // warnings on the next frame. Same S48 / S60 / S61 class of bug.
    Glib::signal_idle().connect_once([this]() {
      if (m_closing) return;
      update_all_panels();
    });
    LOG_INFO("Removed document at index {}", idx);
  });

  // Legacy gallery signals (gallery widget kept for thumbnail rendering only;
  // its add/remove/clear signals are no longer the primary path)
  m_gallery.signal_doc_activated().connect(
      sigc::mem_fun(*this, &MainWindow::on_doc_activated));

  m_gallery.signal_add_doc().connect([this]() {
    if (!m_project)
      return;
    // Show canvas settings dialog then add new doc to project
    m_new_doc_dialog.show(*this, ndd_available_themes(),
        [this](std::unique_ptr<CurvzDocument> seed,
               std::string name,
               std::optional<theme::ThemeId> theme_id) {
      if (!seed) return;
      ndd_apply_chosen_theme(*seed, theme_id);

      // Sanitise name → filename stem.
      std::string base = doc_stem_from_name(name);
      std::string fname = base + ".svg";
      int suffix = 2;
      while (std::any_of(m_project->documents.begin(),
                         m_project->documents.end(),
                         [&](const auto &d) { return d->filename == fname; }))
        fname = base + std::to_string(suffix++) + ".svg";

      seed->filename = fname;

      m_project->documents.push_back(std::move(seed));
      m_project->active_doc_index = (int)m_project->documents.size() - 1;
      m_project->save();
      update_all_panels();
      LOG_INFO("Added document '{}' to project", fname);
    });
  });

  // Duplicate the active doc. Uses SvgWriter→SvgParser round-trip as the
  // clone seam: every field the writer emits round-trips for free, so we
  // can't forget one (the same lesson as S98 m2 fix1, applied to docs).
  // After parse we mint fresh internal_ids on every node and remap
  // text_path_id references through the old→new map so text-on-path
  // survives. Non-SVG state (canvas model, ruler origin, bg/guide colours,
  // export metadata, measure flags) is copied directly from the source —
  // the SVG round-trip doesn't carry those.
  m_gallery.signal_dup_doc().connect([this](int idx) {
    if (!m_project) return;
    if (idx < 0 || idx >= (int)m_project->documents.size()) return;

    const CurvzDocument& src = *m_project->documents[idx];

    // ── 1. Round-trip the scene tree ──────────────────────────────────
    std::string svg_str = write_svg(src);
    auto dup = parse_svg(svg_str);
    if (!dup) {
      LOG_ERROR("dup_doc: parse_svg failed on round-trip of '{}'", src.filename);
      return;
    }

    // ── 2. Regenerate iids; build old→new map for reference remap ─────
    std::unordered_map<std::string, std::string> iid_map;
    std::function<void(SceneNode*)> rewrite_iids = [&](SceneNode* n) {
      if (!n->internal_id.empty()) {
        std::string fresh = generate_internal_id();
        iid_map[n->internal_id] = fresh;
        n->internal_id = fresh;
      } else {
        n->internal_id = generate_internal_id();
      }
      for (auto& ch : n->children) rewrite_iids(ch.get());
    };
    for (auto& layer : dup->layers) rewrite_iids(layer.get());

    // ── 3. Remap iid-keyed cross-references inside the clone ──────────
    // text_path_id is the only cross-reference Curvz currently keeps as
    // an iid. If more iid-keyed refs land later, extend this walker.
    std::function<void(SceneNode*)> remap_refs = [&](SceneNode* n) {
      if (n->is_text() && !n->text_path_id.empty()) {
        auto it = iid_map.find(n->text_path_id);
        if (it != iid_map.end()) n->text_path_id = it->second;
        // If not in map, the original ref was already broken — leave it
        // for the parser's stale-ref rescue logic to handle on next load.
      }
      for (auto& ch : n->children) remap_refs(ch.get());
    };
    for (auto& layer : dup->layers) remap_refs(layer.get());

    // ── 4. Copy non-SVG state from the source ─────────────────────────
    dup->canvas                      = src.canvas;
    dup->ruler_origin_x              = src.ruler_origin_x;
    dup->ruler_origin_y              = src.ruler_origin_y;
    dup->active_layer_index          = src.active_layer_index;
    dup->snap                        = src.snap;
    dup->guide_color_r               = src.guide_color_r;
    dup->guide_color_g               = src.guide_color_g;
    dup->guide_color_b               = src.guide_color_b;
    // s116 m6: artboard_bg / workspace_bg / motif are project-scope now;
    // the duplicated doc's legacy fields stay at struct defaults — they
    // are write-on-load-only and not consulted by any paint or theming
    // code post-m6.
    dup->measure_save_to_layer       = src.measure_save_to_layer;
    dup->measure_destruct_after_copy = src.measure_destruct_after_copy;
    dup->export_name                 = src.export_name;
    dup->export_category             = src.export_category;

    // ── 4b. Carry grid + margin layers from source ────────────────────
    // SvgWriter deliberately skips Grid/MarginLayer (they live in
    // project.json, not in the SVG), so the round-tripped dup only has
    // whatever defaults parse_svg / ensure_*_layer produce. Mirror them
    // by replacing each special layer in dup with a clone of the
    // source's. Uses clone_node so the field set is centralised — any
    // future grid_/margin_ field added to SceneNode flows through here
    // for free.
    auto replace_layer = [](CurvzDocument& doc,
                            const SceneNode& src_layer) {
      // Find the existing layer of the same type in `doc` and replace
      // the unique_ptr slot. Keeps doc->layers ordering intact (matters
      // for active_layer_index and the layer panel). SceneNode is
      // move-only (it owns unique_ptrs), so we substitute the whole slot
      // rather than copy-assigning fields.
      for (auto& l : doc.layers) {
        if (l->type == src_layer.type) {
          l = clone_node(src_layer);
          return;
        }
      }
      // No existing layer of that type — append a clone.
      doc.layers.push_back(clone_node(src_layer));
    };
    if (const SceneNode* src_grid = src.grid_layer())
      replace_layer(*dup, *src_grid);
    if (const SceneNode* src_margin = src.margin_layer())
      replace_layer(*dup, *src_margin);
    // Regenerate iids on the freshly-cloned grid/margin layer nodes so
    // they don't collide with the source. They typically have no
    // children, so a single-level mint is enough; if that ever changes,
    // route through rewrite_iids() instead.
    for (auto& l : dup->layers) {
      if (l->is_grid_layer() || l->is_margin_layer())
        l->internal_id = generate_internal_id();
    }

    // ── 5. Pick a unique filename ─────────────────────────────────────
    // Source "foo.svg" → "foo copy.svg" → "foo copy 2.svg" → ...
    auto strip_ext = [](const std::string& s) -> std::string {
      if (s.size() > 4 && s.substr(s.size() - 4) == ".svg")
        return s.substr(0, s.size() - 4);
      return s;
    };
    std::string src_stem = strip_ext(src.filename.empty() ? "untitled"
                                                          : src.filename);
    auto exists = [&](const std::string& fname) {
      return std::any_of(m_project->documents.begin(),
                         m_project->documents.end(),
                         [&](const auto& d) { return d->filename == fname; });
    };
    std::string fname = src_stem + " copy.svg";
    int suffix = 2;
    while (exists(fname))
      fname = src_stem + " copy " + std::to_string(suffix++) + ".svg";
    dup->filename = fname;

    // ── 6. Insert immediately after the source, set active, save ──────
    int insert_at = idx + 1;
    m_project->documents.insert(m_project->documents.begin() + insert_at,
                                std::move(dup));
    m_project->active_doc_index = insert_at;
    m_project->save();
    update_all_panels();
    LOG_INFO("Duplicated document '{}' → '{}'", src.filename, fname);
  });

  m_gallery.signal_remove_doc().connect([this](int idx) {
    if (!m_project) return;
    if (idx < 0 || idx >= (int)m_project->documents.size())
      return;
    // Delete the SVG file from disk before erasing from the document list.
    if (!m_project->directory.empty()) {
      std::string fname = m_project->documents[idx]->filename;
      if (!fname.empty()) {
        auto svg_path =
            fs::path(m_project->directory) / fs::path(fname).filename();
        std::error_code ec;
        fs::remove(svg_path, ec);
        if (ec)
          LOG_WARN("remove_doc: could not delete '{}': {}", svg_path.string(),
                   ec.message());
        else
          LOG_INFO("remove_doc: deleted '{}'", svg_path.string());
      }
    }
    m_project->documents.erase(m_project->documents.begin() + idx);
    if (m_project->documents.empty()) {
      m_project->active_doc_index = 0;
    } else {
      m_project->active_doc_index =
          std::min(idx, (int)m_project->documents.size() - 1);
    }
    m_project->save();
    // Same deferral as the doc-tab remove path above — we're inside a click
    // handler on a gallery thumbnail's delete button; rebuilding the gallery
    // synchronously yanks that button out from under the event.
    Glib::signal_idle().connect_once([this]() {
      if (m_closing) return;
      update_all_panels();
    });
    LOG_INFO("Removed document at index {}", idx);
  });

  m_gallery.signal_clear_all().connect([this]() {
    if (!m_project)
      return;
    // Delete all SVG files from disk before clearing the document list.
    if (!m_project->directory.empty()) {
      for (const auto &doc : m_project->documents) {
        if (!doc->filename.empty()) {
          auto svg_path = fs::path(m_project->directory) /
                          fs::path(doc->filename).filename();
          std::error_code ec;
          fs::remove(svg_path, ec);
          if (ec)
            LOG_WARN("clear_all: could not delete '{}': {}", svg_path.string(),
                     ec.message());
          else
            LOG_INFO("clear_all: deleted '{}'", svg_path.string());
        }
      }
    }
    m_project->documents.clear();
    m_project->active_doc_index = 0;
    m_project->save();
    update_all_panels();
    LOG_INFO("Cleared all documents from project");
  });

  m_gallery.signal_filter_changed().connect([this](std::string query) {
    m_doc_tabs.set_filter(query);
  });

  m_gallery.signal_preview_icon().connect([this](std::string path) {
    on_preview_icon(path);
  });

  m_gallery.signal_copy_icon().connect([this](std::string path) {
    on_copy_icon(path);
  });

  m_properties.signal_prop_changed().connect([this]() {
    LOG_DEBUG("signal_prop_changed: fired — calling queue_draw");
    // Any inspector edit may affect a Blend's A or B — invalidate caches
    // so the next draw regenerates intermediates.
    m_canvas.mark_all_blends_dirty();
    m_canvas.queue_draw();
    m_layers.refresh();
    // s116 m5: motif may have been the edited prop (Motif panel switch);
    // re-apply so the CSS class on the window reflects current state.
    // Idempotent — if motif didn't change, this is a pair of no-op
    // class membership checks.
    apply_motif_to_window();
    // S80 m4c-2: keep the Styles panel's active-binding indicator dot
    // in sync after inspector edits that change bound_style — namely
    // the Style section's Unbind button. Selection is unchanged so the
    // panel's selection-changed hook won't fire. Cheap rebuild.
    m_styles.refresh();
    schedule_save();
    // Keep toolbar well in sync with the selection's appearance. After an
    // inspector edit on a multi-select, run the full uniformity check
    // instead of just syncing from primary — otherwise the toolbar would
    // not clear its mixed-stripe display when a commit unifies everyone.
    m_toolbar.sync_from_selection(m_canvas.selection(),
                                  m_canvas.selected_object());
    LOG_DEBUG("signal_prop_changed: queue_draw done — scheduling gallery idle");
    Glib::signal_idle().connect_once([this]() {
      LOG_DEBUG("signal_prop_changed: gallery idle firing");
      m_gallery.refresh();
      LOG_DEBUG("signal_prop_changed: gallery idle done");
    });
    LOG_DEBUG("signal_prop_changed: handler done");
  });

  // Blend section's Release button → Canvas::release_blend. Primary
  // selection is guaranteed to be a Blend at this point (the button only
  // exists when build_blend_section ran against a Blend).
  m_properties.signal_request_release_blend().connect(
      [this]() { m_canvas.release_blend(); });

  // S91 Inspector → Edit gradient → open the modal GradientDialog.
  // The inspector packages "what to do on Apply" as the apply_cb
  // closure (writes back via mutate_appearance + EditAppearanceCommand
  // + sibling broadcast); we just open the dialog and forward the
  // edited FillStyle to the callback. m_gradient_dialog is a single
  // long-lived window member; show() reseeds it each call.
  m_properties.signal_request_gradient_edit().connect(
      [this](FillStyle current,
             std::function<void(FillStyle)> apply_cb) {
        m_gradient_dialog.show(*this, current,
            [apply_cb](FillStyle edited) {
              if (apply_cb) apply_cb(std::move(edited));
            });
      });

  // Inspector node coordinate/handle edits — route through canvas so it owns
  // all writes to obj->path. Values arrive in display space; convert to doc
  // space.
  m_properties.signal_request_node_edit().connect(
      [this](int node_idx, double ax, double ay, double ix, double iy,
             double ox, double oy) {
        auto *doc = m_project ? m_project->active_doc() : nullptr;
        if (!doc)
          return;
        const CanvasModel &cm = doc->canvas;
        double rox = doc->ruler_origin_x;
        double roy = doc->ruler_origin_y;
        int q = std::max(1, cm.quality);
        double ch = (double)cm.canvas_height();

        // Display → doc space (inverse of node_doc_to_display_x/y)
        auto to_dx = [&](double disp) -> double {
          double user = disp;
          if (cm.display_mode == DisplayMode::RatioQuality)
            user = disp / 100.0 * q;
          else if (cm.display_mode == DisplayMode::Physical) {
            double sp = std::min(cm.phys_width, cm.phys_height);
            user = sp > 0 ? disp / sp * q : disp;
          }
          return user + rox;
        };
        auto to_dy = [&](double disp) -> double {
          double user = disp;
          if (cm.display_mode == DisplayMode::RatioQuality)
            user = disp / 100.0 * q;
          else if (cm.display_mode == DisplayMode::Physical) {
            double sp = std::min(cm.phys_width, cm.phys_height);
            user = sp > 0 ? disp / sp * q : disp;
          }
          // Y-up display → Y-down doc
          return ch - (user + roy);
        };

        m_canvas.apply_node_edit(node_idx, to_dx(ax), to_dy(ay), to_dx(ix),
                                 to_dy(iy), to_dx(ox), to_dy(oy));
      });

  // Canvas property changes (quality, ratio) from inspector
  m_properties.signal_request_reverse().connect([this]() {
    m_properties.reset_undo_coalesce();
    m_canvas.reverse_selected_path();
    // Inspector needs a refresh so the direction indicator flips
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
  });

  m_properties.signal_request_open_at_node().connect([this]() {
    m_properties.reset_undo_coalesce();
    m_canvas.open_selected_at_node();
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
  });

  m_properties.signal_request_split().connect([this]() {
    m_properties.reset_undo_coalesce();
    m_canvas.split_selected_at_node();
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
  });

  m_properties.signal_request_node_type_change().connect(
      [this](BezierNode::Type type) {
        m_canvas.set_selected_nodes_type(type);
        Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
      });

  m_properties.signal_request_flip().connect(
      [this](bool horizontal) { m_canvas.flip_selection(horizontal); });

  m_properties.signal_request_detach_text().connect([this](SceneNode *node) {
    if (!node)
      return;
    // Ensure the node is in the canvas selection so release_text_from_path
    // finds it
    m_canvas.set_selection_single(node);
    m_canvas.release_text_from_path();
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
  });

  m_properties.signal_canvas_changed().connect([this](CanvasModel cm) {
    auto *doc = m_project->active_doc();
    if (!doc)
      return;
    doc->canvas = cm;
    m_canvas.zoom_fit();
    m_canvas.queue_draw();
    schedule_save();
    LOG_INFO("Canvas changed: {}×{} quality={} ratio={:.4g}:{:.4g}",
             cm.canvas_width(), cm.canvas_height(), cm.quality, cm.ratio_w,
             cm.ratio_h);
  });

  m_properties.signal_doc_renamed().connect([this](std::string new_name) {
    if (!m_project)
      return;
    auto *doc = m_project->active_doc();
    if (!doc)
      return;
    rename_doc(doc, std::move(new_name));
  });

  // Gallery: double-click a tile (or right-click → Rename) to rename. The
  // gallery passes the doc index explicitly so any doc can be renamed
  // without first activating it.
  m_gallery.signal_rename_doc().connect(
      [this](int idx, std::string new_name) {
        if (!m_project)
          return;
        if (idx < 0 || idx >= (int)m_project->documents.size())
          return;
        rename_doc(m_project->documents[idx].get(), std::move(new_name));
      });

  m_properties.signal_request_canvas_focus().connect(
      [this]() { m_canvas.grab_focus(); });

  m_canvas.signal_zoom_changed().connect([this](double zoom) {
    m_statusbar.set_zoom(zoom * 100.0);
    m_toolbar.set_zoom(zoom);
    update_rulers();
  });

  // Zoom tool Alt feedback — swap toolbar icon and update context bar
  m_canvas.signal_zoom_alt_changed().connect([this](bool alt_down) {
    m_toolbar.set_active_tool_icon(ActiveTool::Zoom,
                                   alt_down ? "zoom-out-symbolic"
                                            : "system-search-symbolic");
    m_toolbar.set_zoom_alt(alt_down);
  });

  m_canvas.signal_cursor_moved().connect([this](double x, double y) {
    auto *doc = m_project->active_doc();
    if (!doc)
      return;
    CoordSpace cs{(double)doc->canvas_height()};
    // Apply ruler origin offset to status bar display
    double ux = cs.to_user_x(x) - doc->ruler_origin_x;
    double uy = cs.to_user_y(y) - doc->ruler_origin_y;
    m_statusbar.set_cursor_pos(ux, uy);
    // Update cursor marker in rulers (cheap — just update cursor fields)
    update_rulers();
    // Continuously sync selection panel position during drag
    if (m_canvas.is_dragging() && m_canvas.selected_object() &&
        (m_active_tool == ActiveTool::Selection ||
         m_active_tool == ActiveTool::Node)) {
      m_properties.sync_selection(m_canvas.selected_object());
    }
  });

  // Corner square — set new ruler origin
  m_corner.signal_origin_changed().connect([this](double ux, double uy) {
    auto *doc = m_project->active_doc();
    if (!doc)
      return;

    // Snap origin to existing guide positions (8px screen threshold)
    const double zoom = m_canvas.zoom();
    const double snap_thresh = 8.0 / zoom; // doc-space tolerance
    const double ch = (double)doc->canvas_height();

    if (auto *gl = doc->guide_layer()) {
      for (auto &g : gl->children) {
        if (!g->is_guide())
          continue;
        if (g->guide_is_horizontal()) {
          double guide_uy = ch - g->guide_y;
          if (std::abs(uy - guide_uy) <= snap_thresh)
            uy = guide_uy;
        } else if (g->guide_is_vertical()) {
          double guide_ux = g->guide_x;
          if (std::abs(ux - guide_ux) <= snap_thresh)
            ux = guide_ux;
        }
        // Angled guides: no ruler-origin snap in M1.
      }
    }

    doc->ruler_origin_x = ux;
    doc->ruler_origin_y = uy;
    update_rulers();
    schedule_save();
    m_canvas.grab_focus();
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
    LOG_INFO("Ruler origin set to ({:.2f}, {:.2f}) user space", ux, uy);
  });

  // Live drag preview — show dashed lines on rulers
  m_corner.signal_origin_preview().connect([this](double ux, double uy) {
    auto *doc = m_project ? m_project->active_doc() : nullptr;
    if (!doc)
      return;

    // Apply same guide snap so preview lines jump visually
    const double zoom = m_canvas.zoom();
    const double snap_thresh = 8.0 / zoom;
    const double ch = (double)doc->canvas_height();
    if (auto *gl = doc->guide_layer()) {
      for (auto &g : gl->children) {
        if (!g->is_guide())
          continue;
        if (g->guide_is_horizontal()) {
          double guide_uy = ch - g->guide_y;
          if (std::abs(uy - guide_uy) <= snap_thresh)
            uy = guide_uy;
        } else if (g->guide_is_vertical()) {
          double guide_ux = g->guide_x;
          if (std::abs(ux - guide_ux) <= snap_thresh)
            ux = guide_ux;
        }
        // Angled guides: no ruler-origin snap in M1.
      }
    }

    RulerState rs;
    rs.zoom = m_canvas.zoom();
    rs.pan_x = m_canvas.pan_x();
    rs.pan_y = m_canvas.pan_y();
    rs.widget_w = (double)m_canvas.get_width();
    rs.widget_h = (double)m_canvas.get_height();
    rs.cursor_doc_x = m_canvas.cursor_doc_x();
    rs.cursor_doc_y = m_canvas.cursor_doc_y();
    rs.canvas_w = (double)doc->canvas_width();
    rs.canvas_h = (double)doc->canvas_height();
    rs.quality = doc->canvas.quality;
    rs.display_mode = doc->canvas.display_mode;
    rs.ruler_origin_x = doc->ruler_origin_x;
    rs.ruler_origin_y = doc->ruler_origin_y;
    rs.phys_short = std::min(doc->canvas.phys_width, doc->canvas.phys_height);
    rs.phys_unit = doc->canvas.phys_unit;
    rs.display_unit = doc->canvas.display_unit;
    rs.preview_active = true;
    rs.preview_origin_x = ux;
    rs.preview_origin_y = uy;
    m_hruler.set_state(rs);
    m_vruler.set_state(rs);
    m_canvas.set_origin_preview(ux, uy);
  });

  // Drag ended — clear preview
  m_corner.signal_preview_stop().connect([this]() {
    m_canvas.clear_origin_preview();
    update_rulers();
  });

  // Corner square double-click — reset origin to 0,0 (artboard bottom-left)
  m_corner.signal_origin_reset().connect([this]() {
    auto *doc = m_project->active_doc();
    if (!doc)
      return;
    doc->ruler_origin_x = 0.0;
    doc->ruler_origin_y = 0.0;
    update_rulers();
    schedule_save();
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
    LOG_INFO("Ruler origin reset to 0,0");
  });

  // HRuler — drag downward creates horizontal guide (constant Y)
  m_hruler.signal_guide_dragging().connect([this](double doc_y) {
    if (!m_canvas.guide_drag_active())
      m_canvas.begin_guide_drag(doc_y, true);
    else
      m_canvas.update_guide_drag(doc_y);
  });
  m_hruler.signal_guide_created().connect([this](double doc_y) {
    if (!m_canvas.guide_drag_active())
      m_canvas.begin_guide_drag(doc_y, true);
    m_canvas.end_guide_drag(doc_y);
    schedule_save();
  });
  m_hruler.signal_guide_drag_cancel().connect(
      [this]() { m_canvas.cancel_guide_drag(); });

  // VRuler — drag rightward creates vertical guide (constant X)
  m_vruler.signal_guide_dragging().connect([this](double doc_x) {
    if (!m_canvas.guide_drag_active())
      m_canvas.begin_guide_drag(doc_x, false);
    else
      m_canvas.update_guide_drag(doc_x);
  });
  m_vruler.signal_guide_created().connect([this](double doc_x) {
    if (!m_canvas.guide_drag_active())
      m_canvas.begin_guide_drag(doc_x, false);
    m_canvas.end_guide_drag(doc_x);
    schedule_save();
  });
  m_vruler.signal_guide_drag_cancel().connect(
      [this]() { m_canvas.cancel_guide_drag(); });

  // Unit change from right-click on either ruler
  auto on_unit_changed = [this](Unit u) {
    auto *doc = m_project->active_doc();
    if (!doc)
      return;
    doc->canvas.display_unit = u;
    schedule_save();
    update_rulers();
    m_properties.refresh(&doc->canvas, nullptr);
    m_toolbar.set_document(doc);
    m_corner.set_unit(u);
    {
      Unit cu = (doc->canvas.display_mode == DisplayMode::Physical)
                ? UnitSystem::parse_unit(doc->canvas.phys_unit)
                : u;
      std::string unit_str = UnitSystem::label(cu);
      m_corner_unit_label.set_text("Units: " + unit_str);
    }
  };
  m_hruler.signal_unit_changed().connect(on_unit_changed);
  m_vruler.signal_unit_changed().connect(on_unit_changed);

  // Context bar actions
  m_toolbar.signal_fit_requested().connect([this]() { m_canvas.zoom_fit(); });

  // Zoom-to-exact-value from the right-click popover Apply button.
  // target is a raw zoom factor (1.0 = 100% = 1:1 pixels).
  m_toolbar.signal_zoom_to().connect([this](double target) {
    double cx = m_canvas.get_width() / 2.0;
    double cy = m_canvas.get_height() / 2.0;
    double current = m_canvas.zoom();
    if (current > 0.0)
      m_canvas.zoom_toward(cx, cy, target / current);
  });
  m_toolbar.signal_request_canvas_focus().connect(
      [this]() { m_canvas.grab_focus(); });
  m_toolbar.signal_snap_toggled().connect([this](bool enabled) {
    if (!m_project)
      return;
    m_project->snap.enabled = enabled;
    // Apply to all documents
    for (auto &doc : m_project->documents)
      doc->snap.enabled = enabled;
    schedule_save();
  });

  m_toolbar.signal_snap_pop_open().connect([this]() {
    if (m_project)
      m_toolbar.set_snap_settings(m_project->snap);
  });

  m_toolbar.signal_snap_settings_changed().connect([this](SnapSettings s) {
    if (!m_project)
      return;
    m_project->snap = s;
    // Apply to all documents so Canvas always reads correct values
    for (auto &doc : m_project->documents)
      doc->snap = s;
    m_canvas.queue_draw();
    schedule_save();
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
  });

  // ── Align & Distribute ───────────────────────────────────────────────────
  m_toolbar.signal_align_requested().connect(
      [this](AlignOp op) { m_canvas.align_selection(op); });

  m_toolbar.signal_macro_manager().connect(
      sigc::mem_fun(*this, &MainWindow::on_macro_manager));

  // Toolbar macro button left-click: run the current macro. Mirrors
  // the Ctrl+M keyboard path — on_run_macro() opens the manager itself
  // if there's no current macro or the current macro is empty.
  m_toolbar.signal_macro_run().connect([this]() {
    Macro *cur = MacroManager::instance().current_macro();
    on_run_macro(cur ? cur->internal_id : std::string(""));
  });

  // Enable the align button whenever the selection tool is active
  // and 2+ objects are selected.
  auto update_align_btn = [this]() {
    bool ok = (m_active_tool == ActiveTool::Selection) &&
              (m_canvas.selection().size() >= 2);
    m_toolbar.set_align_enabled(ok);
  };

  m_canvas.signal_selection_changed().connect(
      [this, update_align_btn](SceneNode *) { update_align_btn(); });

  // Also re-check on tool change (handled in on_tool_changed, but we store
  // the lambda so we can call it there too).
  // Store it in a shared_ptr so on_tool_changed can invoke it.
  m_update_align_btn = update_align_btn;

  m_toolbar.signal_zoom_step().connect([this](double dir) {
    // Zoom ladder expressed as fit-zoom multipliers.
    // 1.0 = artboard fills the viewport (Ctrl+0 / zoom_fit).
    // Values below 1.0 zoom out; above 1.0 zoom in.
    // Upper end extends to 128× for high-quality documents
    // (quality=40000 billboard → you still want to see individual units).
    static const double steps[] = {
        0.0125, 0.025, 0.05, 0.10, 0.15, 0.20, 0.25, 0.33,  0.50, 0.67, 0.75,
        1.0,    1.25,  1.5,  2.0,  3.0,  4.0,  5.0,  6.0,   8.0,  10.0, 12.0,
        16.0,   20.0,  24.0, 32.0, 48.0, 64.0, 96.0, 128.0, 192.0};
    static constexpr int N = sizeof(steps) / sizeof(steps[0]);

    // Current zoom expressed as fit-relative multiplier — same units as the
    // ladder.
    double fit = m_canvas.fit_zoom_value();
    double cur = (fit > 0.0) ? (m_canvas.zoom() / fit) : 1.0;

    int idx = 0;
    if (dir > 0) {
      for (int i = 0; i < N; ++i)
        if (steps[i] > cur * 1.02) {
          idx = i;
          goto found;
        }
      idx = N - 1;
    } else {
      for (int i = N - 1; i >= 0; --i)
        if (steps[i] < cur * 0.98) {
          idx = i;
          goto found;
        }
      idx = 0;
    }
  found:
    // Convert the target ladder step back to a raw zoom factor for zoom_toward.
    double target_raw = steps[idx] * fit;
    double cx = m_canvas.get_width() / 2.0;
    double cy = m_canvas.get_height() / 2.0;
    m_canvas.zoom_toward(cx, cy, target_raw / m_canvas.zoom());
  });

  // Helper: convert a popover value from the document's display unit to
  // doc-units (quality pixels). place_*_precise expects doc-units.
  // In physical mode, popover values are in phys_unit (e.g. inches at 300dpi),
  // and 1 doc-unit = phys_short / quality physical units.
  m_toolbar.signal_place_ref().connect(
      [this](double ux, double uy) { m_canvas.place_ref_at_display(ux, uy); });
  m_toolbar.signal_place_rect().connect(
      [this](double x, double y, double w, double h) {
        m_canvas.place_rect_precise(pop_to_px(x), pop_to_px(y), pop_to_px(w),
                                    pop_to_px(h));
      });
  m_toolbar.signal_place_ellipse().connect(
      [this](double cx, double cy, double rx, double ry) {
        m_canvas.place_ellipse_precise(pop_to_px(cx), pop_to_px(cy),
                                       pop_to_px(rx), pop_to_px(ry));
      });
  m_toolbar.signal_place_polygon().connect(
      [this](double cx, double cy, double radius, int sides,
                        double inflection, double angle_rad) {
        m_canvas.set_polygon_settings(sides, inflection);
        m_canvas.place_polygon_precise(pop_to_px(cx), pop_to_px(cy),
                                       pop_to_px(radius), sides, inflection,
                                       angle_rad);
      });
  m_toolbar.signal_place_spiral().connect(
      [this](double cx, double cy, double outer_r, double inner_r,
                        double turns, double angle_rad) {
        m_canvas.set_spiral_settings(
            turns, (outer_r > 0.0) ? (inner_r / outer_r * 100.0) : 0.0);
        m_canvas.place_spiral_precise(pop_to_px(cx), pop_to_px(cy),
                                      pop_to_px(outer_r), pop_to_px(inner_r),
                                      turns, angle_rad);
      });
  m_toolbar.signal_place_line().connect(
      [this](double x1, double y1, double x2, double y2) {
        m_canvas.place_line_precise(pop_to_px(x1), pop_to_px(y1), pop_to_px(x2),
                                    pop_to_px(y2));
      });
  m_toolbar.signal_place_text().connect(
      [this](double x, double y, std::string family, double size_pt,
                        bool bold, bool italic, std::string anchor,
                        std::string align) {
        // x, y: display units → doc px via pop_to_px
        // size: always in points → convert pt→doc px
        auto *doc = m_project ? m_project->active_doc() : nullptr;
        double size_doc = size_pt / 72.0 * 96.0; // fallback: screen dpi
        if (doc) {
          const CanvasModel& cm = doc->canvas;
          if (cm.display_mode == DisplayMode::Physical && cm.quality > 0) {
            double phys_short = std::min(cm.phys_width, cm.phys_height);
            if (phys_short > 0)
              size_doc = size_pt / 72.0 / phys_short * cm.quality;
          } else {
            size_doc = size_pt / 72.0 * 96.0; // pt → screen px (96dpi)
          }
        }
        m_canvas.place_text_precise(pop_to_px(x), pop_to_px(y), family,
                                    size_doc, bold, italic, anchor, align);
      });

  m_toolbar.signal_defaults_changed().connect([this](FillStyle fill,
                                                     StrokeStyle stroke) {
    m_canvas.set_default_style(fill, stroke);

    // Apply to the full selection (multi-select aware).
    // For groups and composites, recurse into all path descendants.
    const auto &sel = m_canvas.selection();
    SceneNode  *primary = m_canvas.selected_object();

    // Gather every leaf paint target that should receive the new appearance.
    // S58g: Compound is treated as a TERMINAL target (its own fill/stroke
    // are the canonical paint per the S58d rule, and the Canvas renderer
    // reads them directly). Group remains a pass-through container —
    // setting fill/stroke on a Group broadcasts to its descendants.
    std::vector<SceneNode *> targets;
    std::function<void(SceneNode *)> collect = [&](SceneNode *node) {
      if (!node)
        return;
      if (node->is_path() || node->is_compound() ||
          node->type == SceneNode::Type::Text) {
        targets.push_back(node);
      } else if (node->is_group()) {
        for (auto &child : node->children)
          collect(child.get());
      }
    };

    if (!sel.empty()) {
      for (SceneNode *obj : sel)
        collect(obj);
    } else if (primary) {
      collect(primary);
    }

    if (targets.empty())
      return;

    auto composite = std::make_unique<CompositeCommand>("Edit appearance");
    for (SceneNode *node : targets) {
      // S82 m4f: capture pre-edit swatch ids before the funnel clears
      // them; capture post-edit ids after the funnel runs. Reordered
      // from push-then-mutate to mutate-then-push so the after snapshot
      // reflects the funnel's break-on-override result. Same pattern as
      // PropertiesPanel broadcast sites.
      // S92 m3: same shape for bound_style — funnel clears it on every
      // direct mutate_appearance call.
      FillStyle   fb = node->fill;
      StrokeStyle sb = node->stroke;
      std::string fsib = node->fill_swatch_id;
      std::string ssib = node->stroke_swatch_id;
      std::string bsb  = node->bound_style;
      // Inspector-driven appearance edit is a user override — break any
      // Style binding on the target (S74 m2).
      Curvz::style::mutate_appearance(*node, [&](SceneNode& n) {
        n.fill   = fill;
        n.stroke = stroke;
      });
      std::string fsia = node->fill_swatch_id;
      std::string ssia = node->stroke_swatch_id;
      std::string bsa  = node->bound_style;
      composite->add(std::make_unique<EditAppearanceCommand>(
          node, std::move(fb), std::move(sb), fill, stroke,
          std::move(fsib), std::move(ssib),
          std::move(fsia), std::move(ssia),
          std::move(bsb), std::move(bsa)));
    }
    m_history.push(std::move(composite));
    m_canvas.queue_draw();
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
  });

  // S91 Toolbar → Edit gradient → open the modal GradientDialog (same
  // instance the inspector uses). The toolbar packages "what to do on
  // Apply" as the apply_cb closure (writes back to m_def_fill / m_def_
  // stroke.paint and re-emits defaults_changed); this side just opens
  // the dialog and forwards the result.
  m_toolbar.signal_gradient_edit_requested().connect(
      [this](FillStyle current,
             std::function<void(FillStyle)> apply_cb) {
        m_gradient_dialog.show(*this, current,
            [apply_cb](FillStyle edited) {
              if (apply_cb) apply_cb(std::move(edited));
            });
      });

  m_canvas.signal_selection_changed().connect([this](GlyphObject *obj) {
    // Deduplicate — skip if the inspector already shows this object for the
    // same tool.  A tool switch (e.g. Selection → TextOnPath) requires a
    // rebuild even when the selected object pointer hasn't changed.
    // TextOnPath is never deduplicated: phase transitions (0→1→2) keep the
    // same selected object pointer but require an inspector rebuild each time.
    //
    // S58m: the selection *size* is also part of dedup identity. Shift-click
    // add/remove keeps the primary pointer stable but changes the set —
    // without the size check, Appearance's mixed/uniform display stayed
    // stale until the user reselected everything.
    size_t sel_size_now = m_canvas.selection().size();
    bool same = (m_active_tool != ActiveTool::TextOnPath) &&
                (m_properties.current_object() == obj) &&
                (m_inspector_tool == m_active_tool) &&
                (sel_size_now == m_prev_selection_size);
    if (same) {
      LOG_DEBUG("selection_changed: same object={} tool={} size={}, skip rebuild",
                (void *)obj, (int)m_active_tool, sel_size_now);
      return;
    }
    m_prev_selection_size = sel_size_now;
    LOG_DEBUG("selection_changed: new obj={} tool={} size={}, scheduling idle rebuild",
              (void *)obj, (int)m_active_tool, sel_size_now);
    Glib::signal_idle().connect_once([this, obj]() {
      if (m_closing)
        return;
      size_t sel_size_now2 = m_canvas.selection().size();
      bool same2 = (m_active_tool != ActiveTool::TextOnPath) &&
                   (m_properties.current_object() == obj) &&
                   (m_inspector_tool == m_active_tool) &&
                   (sel_size_now2 == m_prev_selection_size);
      if (same2) {
        LOG_DEBUG(
            "selection_changed idle: already showing obj={} tool={} size={}, skip",
            (void *)obj, (int)m_active_tool, sel_size_now2);
        return;
      }
      LOG_DEBUG("selection_changed idle: rebuilding for obj={} tool={} size={}",
                (void *)obj, (int)m_active_tool, sel_size_now2);
      refresh_inspector();
      // S58n: give the toolbar the whole selection so it can detect mixed
      // fill/stroke and render diagonal-stripe swatches. Previously passed
      // only the primary's paint, which left the toolbar unable to tell a
      // uniform selection from a mixed one.
      m_toolbar.sync_from_selection(m_canvas.selection(), obj);
      // Sync layers panel highlight to canvas selection
      m_layers.set_canvas_selection(m_canvas.selection());
    });
  });

  m_canvas.signal_node_changed().connect([this](SceneNode *obj, int node_idx) {
    if (m_closing)
      return;
    auto *doc = m_project ? m_project->active_doc() : nullptr;
    CanvasModel *cm = doc ? &doc->canvas : nullptr;
    // Fast-path is safe to call synchronously — it only calls set_value() on
    // existing adjustments, no widget tree changes.
    // Full rebuild (new obj, new node, OR type changed) MUST be deferred via
    // idle — GTK4 must not rebuild the widget tree inside a live signal.
    bool same_obj =
        (obj && node_idx >= 0 && obj == m_properties.current_object() &&
         node_idx == m_properties.current_node());
    bool type_same =
        same_obj && m_properties.current_node_type_matches(obj, node_idx);
    if (same_obj && type_same) {
      m_properties.refresh_node(cm, obj,
                                node_idx); // fast path — no idle needed
    } else {
      Glib::signal_idle().connect_once([this, cm, obj, node_idx]() {
        if (m_closing)
          return;
        m_properties.refresh_node(cm, obj, node_idx);
      });
    }
  });

  m_canvas.signal_document_changed().connect([this]() {
    // Canvas-driven mutations (drag-move, node drags, transform handles,
    // structural edits) come through doc_changed — not prop_changed.
    // Invalidate blend caches here too so live regen works for A/B
    // edited directly on canvas, not just via the inspector.
    m_canvas.mark_all_blends_dirty();
    refresh_status_counts();  // s132 m2 — replaces hand-rolled loop
    m_gallery.refresh();
    m_doc_tabs.refresh();
    // Refresh layers panel so new object appears
    m_layers.refresh();
    // Persist structural changes (e.g. text-to-path link UUID) to disk
    schedule_save();
    // Sync inspector selection position/size when object moves
    if (m_canvas.selected_object() &&
        (m_active_tool == ActiveTool::Selection ||
         m_active_tool == ActiveTool::Node)) {
      m_properties.sync_selection(m_canvas.selected_object());
    }
  });

  // ── Guide selection — three-way sync: canvas ↔ layers ↔ inspector ──────────
  // Canvas → layers + inspector
  m_canvas.signal_guide_selection_changed().connect(
      [this](std::vector<SceneNode *> sel) {
        m_layers.set_guide_selection(sel);
        m_properties.set_guide_selection(sel);
        Glib::signal_idle().connect_once([this]() {
          if (m_closing)
            return;
          refresh_inspector();
        });
      });

  // LayersPanel → canvas + inspector
  m_layers.signal_guide_selection_changed().connect(
      [this](std::vector<SceneNode *> sel) {
        LOG_DEBUG("MainWindow: layers signal_guide_selection_changed size={}",
                  sel.size());
        m_canvas.set_guide_selection(sel);
        m_properties.set_guide_selection(sel);
        Glib::signal_idle().connect_once([this]() {
          if (m_closing)
            return;
          LOG_DEBUG("MainWindow: guide selection idle refresh_inspector");
          refresh_inspector();
        });
      });

  // Inspector delete emits selection-changed with empty set — canvas + layers
  // update
  m_properties.signal_guide_selection_changed().connect(
      [this](std::vector<SceneNode *> sel) {
        m_canvas.set_guide_selection(sel);
        m_layers.set_guide_selection(sel);
        m_canvas.queue_draw();
      });

  // Guide added/deleted from inspector — refresh layers panel and canvas.
  // Also sync guide selection state: PropertiesPanel already cleared its own
  // m_guide_selection; tell canvas and layers to match.
  m_properties.signal_guide_layer_changed().connect([this]() {
    m_canvas.set_guide_selection(m_properties.guide_selection());
    m_layers.set_guide_selection(m_properties.guide_selection());
    m_layers.refresh();
    m_canvas.queue_draw();
  });

  // Guide construct from inspector "From 2 points…" button.  Begins the
  // canvas-side mode and arranges for the review dialog to open after the
  // user clicks the second point.
  m_properties.signal_guide_construct_requested().connect([this]() {
    m_canvas.set_guide_construct_review_callback(
        [this]() { open_guide_review_dialog(); });
    m_canvas.begin_guide_construct();
    m_canvas.grab_focus();
  });

  m_canvas.signal_request_tool().connect([this](ActiveTool tool) {
    m_toolbar.select_tool(tool);
    on_tool_changed(tool);
  });

  // s125 m1a: canvas right-click → "Save to Library…" emits this. We open
  // the same folder picker that LibraryPanel's + button uses, then route
  // through on_save_selection_to_library. Two callers (the panel's + and
  // this) currently duplicate the picker setup; if a third caller appears,
  // extract — for now duplication is cheaper than the abstraction.
  m_canvas.signal_request_save_to_library().connect(
      [this]() { on_request_save_selection_to_library(); });

  // Non-blocking info/warning messages emitted by Canvas (e.g. boolean op
  // skips)
  m_canvas.signal_show_message().connect(
      [this](std::string title, std::string body) {
        curvz::utils::show_alert(*this, title, body);
      });

  // s113 m3: sync UI state when outline mode flips for any reason —
  // manual toggle, keyboard E, settings restore, or the m3 zoom-safety
  // auto-flip. Single sync seam means future outline-mode change paths
  // pick up the sync automatically.
  m_canvas.signal_outline_mode_changed().connect([this]() {
    const bool om = m_canvas.is_outline_mode();
    m_statusbar.set_mode(om ? "Outline" : "Preview");
    if (auto act = lookup_action("toggle-outline")) {
      auto sa = std::dynamic_pointer_cast<Gio::SimpleAction>(act);
      if (sa)
        sa->set_state(Glib::Variant<bool>::create(om));
    }
  });

  // After an eyedropper pick, sync the toolbar well so the sampled colour
  // becomes the new fill or stroke default shown in the well.
  m_canvas.signal_eyedropper_pick().connect(
      [this](FillStyle color, bool to_stroke) {
        FillStyle new_fill = to_stroke ? m_toolbar.default_fill() : color;
        StrokeStyle new_stroke = m_toolbar.default_stroke();
        if (to_stroke)
          new_stroke.paint = color;
        m_toolbar.sync_from_object(new_fill, new_stroke);
      });

  m_canvas.signal_corner_sel_changed().connect(
      [this](int count) { show_corner_panel(count > 0); });

  // Keyboard shortcuts — attached to MainWindow so they work regardless of
  // which child widget has focus.
  auto keys = Gtk::EventControllerKey::create();

  keys->signal_key_pressed().connect(
      [this](guint kv, guint, Gdk::ModifierType mod) -> bool {
        bool ctrl =
            (mod & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
        bool shift =
            (mod & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
        bool alt = (mod & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};

        // ── Alt press — forward to canvas for zoom cursor/icon ─────────────
        // The Canvas key controller only fires when the canvas has focus, which
        // is unreliable.  Handle Alt here at the window level instead.
        if (kv == GDK_KEY_Alt_L || kv == GDK_KEY_Alt_R) {
          m_canvas.notify_alt_pressed();
          return false; // never consume Alt — other handlers may need it
        }

        // ── Text-input focus guard ──────────────────────────────────────────
        // In CAPTURE phase we see all keypresses. Skip non-Ctrl shortcuts when
        // a text-entry widget (SpinButton, Entry) has keyboard focus so normal
        // typing/editing is not disrupted.
        auto *focused = get_focus();
        bool text_focused =
            focused &&
            (dynamic_cast<Gtk::SpinButton *>(focused) ||
             dynamic_cast<Gtk::Entry *>(focused) ||
             (focused &&
              GTK_IS_TEXT(focused->gobj()))); // GTK4 Entry internal text widget

        // Ctrl shortcuts always fire regardless of focus
        if (!ctrl) {
          if (text_focused)
            return false;
        }

        // ── Space press — forward to canvas for Space+drag pan ──────────────
        // In Ruler tool, space clears the current measurement for a fresh pick.
        if (kv == GDK_KEY_space) {
          if (m_canvas.active_tool() == ActiveTool::Ruler) {
            m_canvas.ruler_clear();
            return true;
          }
          m_canvas.notify_space_pressed();
          return true;
        }

        // ── Shortcuts dialog ─────────────────────────────────────────────
        if (!ctrl && !shift && !alt &&
            (kv == GDK_KEY_question || kv == GDK_KEY_slash)) {
          m_shortcuts_dialog.show(*this);
          return true;
        }

        // ── Help manual ──────────────────────────────────────────────────
        // F1 is the universal "help" key on every desktop platform. On
        // Apple Silicon Macs (e.g. Asahi Fedora on M-series) the function
        // row defaults to system controls and F1 is unreliable without
        // fn-modifier gymnastics, so Alt+? is wired as a parallel that
        // works on any keyboard. ? is also the Shortcuts dialog (handler
        // immediately above) without a modifier — Alt+? is distinct and
        // not in conflict.
        // win.show-help action exists (see act_help around line 928); the
        // bind() entry at MainWindow.cpp:1134 is cosmetic-only per s123 m4
        // findings, so the action is wired here in CAPTURE explicitly.
        if (!ctrl && !shift && !alt && kv == GDK_KEY_F1) {
          m_help_window.show(*this);
          return true;
        }
        if (alt && !ctrl &&
            (kv == GDK_KEY_question || kv == GDK_KEY_slash)) {
          m_help_window.show(*this);
          return true;
        }

        // ── Document navigation (s108 m7) ────────────────────────────────
        // Window-level CAPTURE-phase dispatch. Action accels alone proved
        // unreliable for Ctrl+Tab because GTK4's built-in focus-traversal
        // default consumes the key for Tab-trapping widgets before the
        // accel system gets a turn. Catching it here (CAPTURE phase, ahead
        // of any focus dispatch) is the same pattern this file uses for
        // Ctrl+Z, Ctrl+Shift+M, etc.
        //
        // Two key pairs supported: Tab (browser/IDE muscle memory, works
        // on keyboards without dedicated PgUp/PgDn) and Page_Down/Up
        // (full-keyboard convention). Both funnel through the same
        // activate_action call so the menu items show one accel each.
        if (ctrl && !alt &&
            (kv == GDK_KEY_Tab || kv == GDK_KEY_ISO_Left_Tab ||
             kv == GDK_KEY_Page_Down || kv == GDK_KEY_Page_Up ||
             kv == GDK_KEY_KP_Page_Down || kv == GDK_KEY_KP_Page_Up)) {
          const bool prev =
              shift || kv == GDK_KEY_ISO_Left_Tab ||
              kv == GDK_KEY_Page_Up || kv == GDK_KEY_KP_Page_Up;
          cycle_doc(prev ? -1 : +1);
          return true;
        }

        // S128 — S75 m3a Ctrl+Shift+M test harness removed. The harness
        // claimed Ctrl+Shift+M to exercise BindStyleCommand /
        // materialise_from_style during Phase 1 development, returning
        // true unconditionally and shadowing the Macro Manager handler
        // farther down in the Ctrl block (~line 3390). Phase 2 long
        // since shipped real binding affordances (Styles panel +
        // inspector "Style" row + Ctrl+Shift+B unbind), so the harness
        // was dead code — and its shadow of the macro hotkey was a
        // user-facing bug. Removed; Ctrl+Shift+M now reaches
        // on_macro_manager() as documented.

        // S91 — Ctrl+Alt+9 hotkey removed. Gradient editor is now reachable
        // from the inspector's Appearance section (Gradient toggle → Edit…)
        // and from the Toolbar fill / stroke popovers (Gradient toggle →
        // Edit…). MainWindow connects both signal_request_gradient_edit
        // (PropertiesPanel) and signal_gradient_edit_requested (Toolbar)
        // to m_gradient_dialog.show — the dialog itself is unchanged.

        // ── S80 m4c-2: Ctrl+Shift+B — Unbind selection from style ────────
        // Power-user accelerator. The canonical Unbind path is the
        // inspector's "Style" binding row at the top of Appearance
        // (S83 m4h); this hotkey provides a keyboard-only alternative
        // for users who keep both hands on the keyboard during work.
        //
        // Behaviour: walks the Path/Compound selection, builds an
        // UnbindStyleCommand for currently-bound nodes only (skips
        // already-unbound), pushes one atomic command, refreshes
        // canvas + inspector. Mirrors the inspector handler in
        // PropertiesPanel.cpp's add_fill_stroke_section binding-row
        // block (the "Style" row's Unbind button) exactly.
        //
        // Unlike the m3a/m3b/m3d test harnesses above and below,
        // this one is permanent — survives the m4c-3+ cleanup pass.
        if (ctrl && shift && !alt &&
            (kv == GDK_KEY_B || kv == GDK_KEY_b)) {
          if (m_canvas.selection().empty()) {
            LOG_INFO("Ctrl+Shift+B: no selection");
            return true;
          }
          // Style-eligible selection (Path + Compound).
          std::vector<SceneNode*> targets;
          for (SceneNode* n : m_canvas.selection()) {
            if (!n) continue;
            if (n->type == SceneNode::Type::Path ||
                n->type == SceneNode::Type::Compound)
              targets.push_back(n);
          }
          if (targets.empty()) return true;

          // Mutate-then-push pattern (CommandHistory::push doesn't
          // execute — see CommandHistory.cpp ~line 7). Skip already-
          // unbound nodes so undo only restores ones that actually
          // changed; matches the inspector handler's approach.
          std::vector<UnbindStyleCommand::TargetSnap> snaps;
          snaps.reserve(targets.size());
          for (SceneNode* n : targets) {
            if (n->bound_style.empty()) continue;
            UnbindStyleCommand::TargetSnap ts;
            ts.obj                     = n;
            ts.bound_style_before      = n->bound_style;
            ts.fill_before             = n->fill;
            ts.fill_swatch_id_before   = n->fill_swatch_id;
            ts.stroke_before           = n->stroke;
            ts.stroke_swatch_id_before = n->stroke_swatch_id;
            snaps.push_back(std::move(ts));
            n->bound_style.clear();
            // fill / stroke deliberately untouched (break-on-override
            // v1 — pre-unbind cache IS the post-unbind appearance).
          }
          if (snaps.empty()) {
            LOG_INFO("Ctrl+Shift+B: nothing in selection was bound");
            return true;
          }

          const bool plural = snaps.size() > 1;
          m_history.push(std::make_unique<UnbindStyleCommand>(
              std::move(snaps),
              plural ? std::string("Unbind style (multiple)")
                     : std::string("Unbind style")));
          m_canvas.queue_draw();
          // Refresh the Styles panel so the active-binding indicator
          // dot clears off the row(s) that just unbound. Selection
          // didn't change, so signal_selection_changed won't fire on
          // its own.
          m_styles.refresh();
          Glib::signal_idle().connect_once(
              [this]() { refresh_inspector(); });
          return true;
        }

        // ── M4c-2e: Escape clears Warp envelope pick set ─────────────────
        // Must run BEFORE the pen-cancel Escape handler below so the key
        // isn't consumed. Only intercepts when primary is a Warp and
        // the pick set is non-empty; otherwise falls through to the
        // general Escape cascade.
        if (kv == GDK_KEY_Escape && !ctrl && !shift && !alt) {
          SceneNode *sel = m_canvas.primary_selection();
          if (sel && sel->is_warp() &&
              m_canvas.active_tool() == ActiveTool::Selection &&
              !m_canvas.warp_env_picks().empty()) {
            m_canvas.warp_env_picks_clear();
            return true;
          }
        }

        // ── Pen tool commit/cancel ──────────────────────────────────────────
        if (kv == GDK_KEY_Escape) {
          if (m_canvas.guide_construct_active()) {
            m_canvas.cancel_guide_construct();
            close_guide_review_dialog();
            return true;
          }
          m_canvas.cancel_pen_path();
          m_canvas.cancel_line_path();
          m_canvas.cancel_text_edit();
          return true;
        }
        if (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) {
          if (text_focused)
            return false;
          m_canvas.commit_pen_path();
          m_canvas.commit_line_path();
          m_canvas.commit_text_edit();
          if (m_canvas.active_tool() == ActiveTool::Ruler)
            m_canvas.ruler_place_measurement();
          return true;
        }

        // ── M4c-2e: Warp envelope pick-set keyboard shortcuts ─────────────
        // Only when Selection tool is active AND primary selection is a
        // Warp. Gates on no-Ctrl so Ctrl+T/L/R etc. still reach other
        // handlers. Shift-A is handled explicitly since it picks a
        // different set than A alone.
        {
          SceneNode *sel = m_canvas.primary_selection();
          if (!ctrl &&
              m_canvas.active_tool() == ActiveTool::Selection &&
              sel && sel->is_warp()) {
            // Escape clears the pick set.
            if (!shift && !alt && kv == GDK_KEY_Escape) {
              m_canvas.warp_env_picks_clear();
              return true;
            }
            // T — all top anchors
            if (!shift && !alt && (kv == GDK_KEY_t || kv == GDK_KEY_T)) {
              m_canvas.warp_env_picks_select_all_top_anchors();
              return true;
            }
            // B — all bottom anchors
            if (!shift && !alt && (kv == GDK_KEY_b || kv == GDK_KEY_B)) {
              m_canvas.warp_env_picks_select_all_bottom_anchors();
              return true;
            }
            // L — leftmost of top + leftmost of bottom
            if (!shift && !alt && (kv == GDK_KEY_l || kv == GDK_KEY_L)) {
              m_canvas.warp_env_picks_select_leftmost_pair();
              return true;
            }
            // R — rightmost of top + rightmost of bottom
            if (!shift && !alt && (kv == GDK_KEY_r || kv == GDK_KEY_R)) {
              m_canvas.warp_env_picks_select_rightmost_pair();
              return true;
            }
            // C — interior anchors on both envelopes
            if (!shift && !alt && (kv == GDK_KEY_c || kv == GDK_KEY_C)) {
              m_canvas.warp_env_picks_select_interior_anchors();
              return true;
            }
            // A — select all: every anchor + every visible handle on
            // both envelopes.
            if (!shift && !alt && (kv == GDK_KEY_a || kv == GDK_KEY_A)) {
              m_canvas.warp_env_picks_select_all();
              return true;
            }
          }
        }

        // ── Delete selected object or guides ──────────────────────────────
        if ((kv == GDK_KEY_Delete || kv == GDK_KEY_BackSpace) && !ctrl) {
          // Guide selection takes priority — delete guides if any are selected
          if (!m_canvas.guide_selection().empty()) {
            m_canvas.delete_selected_guides();
            // Sync layers and inspector
            m_layers.set_guide_selection({});
            m_properties.set_guide_selection({});
            m_layers.refresh();
            Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
            return true;
          }
          if (m_canvas.delete_selected())
            return true;
        }

        // ── Node tool keys ──────────────────────────────────────────────────
        if (m_canvas.node_tool_key(kv, shift, ctrl, alt))
          return true;

        // ── Selection tool keys ─────────────────────────────────────────────
        if (m_canvas.selection_tool_key(kv, shift, ctrl, alt))
          return true;

        // ── Shift+U: release text from path ──────────────────────────────
        if (!ctrl && shift && !alt && (kv == GDK_KEY_u || kv == GDK_KEY_U)) {
          m_canvas.release_text_from_path();
          return true;
        }

        // ── No-modifier tool hotkeys ────────────────────────────────────────
        if (!ctrl && !shift && !alt) {
          // Up/Down arrows cycle through toolbar tools — only when nothing
          // selected
          if (m_canvas.selection().empty()) {
            if (kv == GDK_KEY_Up) {
              m_toolbar.cycle_tool(-1);
              return true;
            }
            if (kv == GDK_KEY_Down) {
              m_toolbar.cycle_tool(+1);
              return true;
            }
          }
          auto switch_tool = [&](ActiveTool t) {
            m_toolbar.select_tool(t);
            // select_tool emits signal_tool_changed which calls on_tool_changed
          };
          switch (kv) {
          case GDK_KEY_s:
          case GDK_KEY_S:
            switch_tool(ActiveTool::Selection);
            return true;
          case GDK_KEY_n:
          case GDK_KEY_N:
            switch_tool(ActiveTool::Node);
            return true;
          case GDK_KEY_p:
          case GDK_KEY_P:
            switch_tool(ActiveTool::Pen);
            return true;
          case GDK_KEY_r:
          case GDK_KEY_R:
            // If Selection tool is active with objects selected, R activates
            // pivot placement mode rather than switching to the Rect tool.
            if (m_canvas.active_tool() == ActiveTool::Selection &&
                !m_canvas.selection().empty()) {
              m_canvas.notify_r_pressed();
              return true;
            }
            switch_tool(ActiveTool::Rect);
            return true;
          case GDK_KEY_e:
          case GDK_KEY_E:
            switch_tool(ActiveTool::Ellipse);
            return true;
          case GDK_KEY_l:
          case GDK_KEY_L:
            switch_tool(ActiveTool::Line);
            return true;
          case GDK_KEY_f:
          case GDK_KEY_F:
            switch_tool(ActiveTool::Ref);
            return true;
          case GDK_KEY_t:
          case GDK_KEY_T:
            switch_tool(ActiveTool::Text);
            return true;
          case GDK_KEY_i:
          case GDK_KEY_I:
            switch_tool(ActiveTool::Eyedropper);
            return true;
          case GDK_KEY_k:
          case GDK_KEY_K:
            switch_tool(ActiveTool::Corner);
            return true;
          case GDK_KEY_g:
          case GDK_KEY_G:
            switch_tool(ActiveTool::Polygon);
            return true;
          case GDK_KEY_w:
          case GDK_KEY_W:
            switch_tool(ActiveTool::Spiral);
            return true;
          case GDK_KEY_z:
          case GDK_KEY_Z:
            switch_tool(ActiveTool::Zoom);
            return true;
          case GDK_KEY_m:
          case GDK_KEY_M:
            switch_tool(ActiveTool::Ruler);
            return true;
          case GDK_KEY_u:
          case GDK_KEY_U:
            switch_tool(ActiveTool::TextOnPath);
            return true;
          case GDK_KEY_q:
          case GDK_KEY_Q: {
            // Toggle snap
            auto *doc = m_project ? m_project->active_doc() : nullptr;
            if (doc) {
              doc->snap.enabled = !doc->snap.enabled;
              m_project->snap.enabled = doc->snap.enabled;
              m_toolbar.set_snap_enabled(doc->snap.enabled);
              LOG_DEBUG("Snap toggled: {}", doc->snap.enabled);
            }
            return true;
          }
          case GDK_KEY_question: {
            m_shortcuts_dialog.show(*this);
            return true;
          }
          default:
            break;
          }
          // +/- zoom
          if (kv == GDK_KEY_plus || kv == GDK_KEY_equal ||
              kv == GDK_KEY_KP_Add) {
            m_toolbar.signal_zoom_step().emit(+1.0);
            return true;
          }
          if (kv == GDK_KEY_minus || kv == GDK_KEY_KP_Subtract) {
            m_toolbar.signal_zoom_step().emit(-1.0);
            return true;
          }
          if (kv == GDK_KEY_0 || kv == GDK_KEY_KP_0) {
            m_canvas.zoom_fit();
            return true;
          }
          if (kv == GDK_KEY_1 || kv == GDK_KEY_KP_1) {
            double cx = m_canvas.get_width() / 2.0;
            double cy = m_canvas.get_height() / 2.0;
            double target = m_canvas.fit_zoom_value();
            m_canvas.zoom_toward(cx, cy, target / m_canvas.zoom());
            return true;
          }
          if (kv == GDK_KEY_2 || kv == GDK_KEY_KP_2) {
            double cx = m_canvas.get_width() / 2.0;
            double cy = m_canvas.get_height() / 2.0;
            double target = m_canvas.fit_zoom_value() * 2.0;
            m_canvas.zoom_toward(cx, cy, target / m_canvas.zoom());
            return true;
          }
          if (kv == GDK_KEY_3 || kv == GDK_KEY_KP_3) {
            m_canvas.zoom_to_selection();
            return true;
          }
        }

        // ── Alt shortcuts ───────────────────────────────────────────────────
        if (alt && !ctrl && !shift) {
          if (kv == GDK_KEY_d || kv == GDK_KEY_D) {
            m_canvas.clone_selected();
            return true;
          }
        }
        // Ctrl+Alt+D → Step and Repeat (moved from Ctrl+Shift+D so the
        // latter can open the GTK Inspector).
        if (ctrl && alt && !shift &&
            (kv == GDK_KEY_d || kv == GDK_KEY_D)) {
          on_step_repeat();
          return true;
        }

        // ── Ctrl shortcuts ──────────────────────────────────────────────────
        if (!ctrl)
          return false;

        if (!shift && (kv == GDK_KEY_z || kv == GDK_KEY_Z)) {
          on_undo();
          return true;
        }
        if (shift && (kv == GDK_KEY_z || kv == GDK_KEY_Z)) {
          on_redo();
          return true;
        }
        if (!shift && (kv == GDK_KEY_y || kv == GDK_KEY_Y)) {
          on_redo();
          return true;
        }
        // Clipboard
        if (!shift && (kv == GDK_KEY_a || kv == GDK_KEY_A)) {
          // M4c-2e: Suppress Ctrl+A when primary is a Warp — global
          // object-select-all would disrupt envelope editing by
          // changing primary selection away from the Warp.
          SceneNode *sel = m_canvas.primary_selection();
          if (sel && sel->is_warp() &&
              m_canvas.active_tool() == ActiveTool::Selection) {
            return true;
          }
          m_canvas.select_all();
          return true;
        }
        if (!shift && (kv == GDK_KEY_c || kv == GDK_KEY_C)) {
          m_canvas.copy_selected();
          return true;
        }
        if (!shift && (kv == GDK_KEY_x || kv == GDK_KEY_X)) {
          m_canvas.cut_selected();
          return true;
        }
        if (!shift && (kv == GDK_KEY_v || kv == GDK_KEY_V)) {
          m_canvas.paste_clipboard();
          return true;
        }
        if (!shift && (kv == GDK_KEY_d || kv == GDK_KEY_D)) {
          m_canvas.duplicate_selected();
          return true;
        }
        if (!shift && (kv == GDK_KEY_m || kv == GDK_KEY_M)) {
          Macro *cur = MacroManager::instance().current_macro();
          LOG_INFO("Ctrl+M: current_macro={}", cur ? cur->name : "(none)");
          on_run_macro(cur ? cur->internal_id : std::string(""));
          return true;
        }
        if (shift && (kv == GDK_KEY_m || kv == GDK_KEY_M)) {
          on_macro_manager();
          return true;
        }
        if (!shift && (kv == GDK_KEY_g || kv == GDK_KEY_G)) {
          m_canvas.group_selection();
          return true;
        }
        if (shift && (kv == GDK_KEY_g || kv == GDK_KEY_G)) {
          m_canvas.ungroup_selection();
          return true;
        }
        // Clipping — Ctrl+7 makes a clip group (arms pick mode),
        // Ctrl+Alt+7 releases a selected ClipGroup. '7' has no Shift
        // sibling ('&') so we accept both keysyms — some keyboards
        // map Ctrl+Shift+7 as the '/' key in certain layouts but
        // distinguishing that isn't worth the complexity here; we
        // require !shift to keep Ctrl+Shift+7 free.
        if (!shift && !alt && kv == GDK_KEY_7) {
          m_canvas.make_clip_group();
          return true;
        }
        if (!shift && alt && kv == GDK_KEY_7) {
          m_canvas.release_clip_group();
          return true;
        }
        // Arrange z-order
        if (!shift && !alt && kv == GDK_KEY_Up) {
          m_canvas.arrange(Canvas::ArrangeOp::BringForward);
          return true;
        }
        if (!shift && !alt && kv == GDK_KEY_Down) {
          m_canvas.arrange(Canvas::ArrangeOp::SendBackward);
          return true;
        }
        if (shift && !alt && kv == GDK_KEY_Up) {
          m_canvas.arrange(Canvas::ArrangeOp::BringToFront);
          return true;
        }
        if (shift && !alt && kv == GDK_KEY_Down) {
          m_canvas.arrange(Canvas::ArrangeOp::SendToBack);
          return true;
        }
        // Boolean path ops: wired directly in this CAPTURE controller
        // because set_accels_for_action() accelerator dispatch loses to
        // the controller (CAPTURE handler claims the event before GTK's
        // accelerator machinery sees it). Every other "bound" shortcut
        // in this file follows the same explicit pattern. We still
        // consult the action's enabled state so the sensitivity gate
        // (set by update_bool_actions_sensitive) is respected — same
        // protection s122 m2 was reaching for.
        if (shift && (kv == GDK_KEY_u || kv == GDK_KEY_U)) {
          if (m_act_bool_union && m_act_bool_union->get_enabled())
            m_canvas.boolean_op(BooleanOpType::Union);
          return true;
        }
        if (shift && (kv == GDK_KEY_e || kv == GDK_KEY_E)) {
          if (m_act_bool_subtract && m_act_bool_subtract->get_enabled())
            m_canvas.boolean_op(BooleanOpType::Subtract);
          return true;
        }
        if (shift && (kv == GDK_KEY_i || kv == GDK_KEY_I)) {
          if (m_act_bool_intersect && m_act_bool_intersect->get_enabled())
            m_canvas.boolean_op(BooleanOpType::Intersect);
          return true;
        }
        if (shift && (kv == GDK_KEY_o || kv == GDK_KEY_O)) {
          auto *doc = m_project ? m_project->active_doc() : nullptr;
          const CanvasModel *cm = doc ? &doc->canvas : nullptr;
          m_offset_path_dialog.show(
              *this, cm, [this](OffsetPathDialog::Options opts) {
                m_canvas.offset_path_op(opts.distance, opts.side,
                                        opts.keep_original);
              });
          return true;
        }
        if (shift && (kv == GDK_KEY_x || kv == GDK_KEY_X)) {
          m_canvas.expand_stroke_op();
          return true;
        }
        if (!shift && kv == GDK_KEY_8) {
          m_canvas.make_compound_path();
          return true;
        }
        if (shift && (kv == GDK_KEY_8 || kv == GDK_KEY_asterisk)) {
          m_canvas.split_compound_path();
          return true;
        }
        if (!shift && (kv == GDK_KEY_n || kv == GDK_KEY_N)) {
          on_new();
          return true;
        }
        if (!shift && (kv == GDK_KEY_o || kv == GDK_KEY_O)) {
          on_open();
          return true;
        }
        if (!shift && (kv == GDK_KEY_s || kv == GDK_KEY_S)) {
          on_save();
          return true;
        }
        if (shift && (kv == GDK_KEY_s || kv == GDK_KEY_S)) {
          on_save_as();
          return true;
        }
        if (!shift && (kv == GDK_KEY_p || kv == GDK_KEY_P)) {
          if (m_project)
            m_print_manager.show(*this, *m_project);
          return true;
        }
        if (!shift && (kv == GDK_KEY_q || kv == GDK_KEY_Q)) {
          on_quit();
          return true;
        }
        if (!shift && (kv == GDK_KEY_w || kv == GDK_KEY_W)) {
          on_quit();
          return true;
        }
        if (!shift && (kv == GDK_KEY_question || kv == GDK_KEY_slash)) {
          m_shortcuts_dialog.show(*this);
          return true;
        }
        if (!shift && (kv == GDK_KEY_e || kv == GDK_KEY_E)) {
          // s113 m2: same gate as the action — outline→preview at extreme
          // zoom would crash the app via drop-shadow buffer allocation.
          // s113 m3: action/statusbar sync handled by
          // signal_outline_mode_changed connection.
          try_toggle_outline_safely();
          return true;
        }
        if (!shift && (kv == GDK_KEY_r || kv == GDK_KEY_R)) {
          m_rulers_visible = !m_rulers_visible;
          toggle_rulers(m_rulers_visible);
          // Sync menu checkmark via action state
          if (auto act = lookup_action("toggle-rulers")) {
            auto sa = std::dynamic_pointer_cast<Gio::SimpleAction>(act);
            if (sa)
              sa->set_state(Glib::Variant<bool>::create(m_rulers_visible));
          }
          return true;
        }

        // Ctrl+0/1/2 zoom presets
        // All expressed relative to fit-zoom (1× = fits screen, 2× = double,
        // etc.)
        if (!shift && (kv == GDK_KEY_0 || kv == GDK_KEY_KP_0)) {
          m_canvas.zoom_fit(); // Ctrl+0 — fit artboard
          return true;
        } else if (kv == GDK_KEY_0 || kv == GDK_KEY_KP_0) {
          m_canvas.zoom_to_all_objects(); // Ctrl+Shift+0 — fit all objects
                                          // incl. off-canvas
          return true;
        }

        if (kv == GDK_KEY_0 || kv == GDK_KEY_KP_0) {
          if (shift)
            m_canvas.zoom_to_all_objects(); // Ctrl+Shift+0 — fit all objects
                                            // incl. off-canvas
          else
            m_canvas.zoom_fit(); // Ctrl+0 — fit artboard
          return true;
        }

        if (!shift && (kv == GDK_KEY_1 || kv == GDK_KEY_KP_1)) {
          // 1× = fit-zoom (artboard fills viewport with margin)
          double cx = m_canvas.get_width() / 2.0;
          double cy = m_canvas.get_height() / 2.0;
          double target = m_canvas.fit_zoom_value();
          m_canvas.zoom_toward(cx, cy, target / m_canvas.zoom());
          return true;
        }

        if (!shift && (kv == GDK_KEY_2 || kv == GDK_KEY_KP_2)) {
          // 2× = double fit-zoom
          double cx = m_canvas.get_width() / 2.0;
          double cy = m_canvas.get_height() / 2.0;
          double target = m_canvas.fit_zoom_value() * 2.0;
          m_canvas.zoom_toward(cx, cy, target / m_canvas.zoom());
          return true;
        }

        if (!shift && (kv == GDK_KEY_3 || kv == GDK_KEY_KP_3)) {
          m_canvas.zoom_to_selection(); // Ctrl+3 — zoom to fit selection
          return true;
        }

        return false;
      },
      false);

  // Alt release — forward to canvas at window level for same reason
  keys->signal_key_released().connect(
      [this](guint kv, guint, Gdk::ModifierType) {
        if (kv == GDK_KEY_Alt_L || kv == GDK_KEY_Alt_R)
          m_canvas.notify_alt_released();
        if (kv == GDK_KEY_space)
          m_canvas.notify_space_released();
        if (kv == GDK_KEY_r || kv == GDK_KEY_R)
          m_canvas.notify_r_released();
      });

  keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
  add_controller(keys);
}

// ── Slots
// ─────────────────────────────────────────────────────────────────────

void MainWindow::on_tool_changed(ActiveTool tool) {
  LOG_DEBUG("Active tool changed: {}", (int)tool);
  m_active_tool = tool;
  // Cancel any in-progress line when leaving the Line tool
  if (tool != ActiveTool::Line)
    m_canvas.cancel_line_path();
  m_canvas.set_active_tool(tool);
  m_context_bar.set_tool(tool);
  refresh_inspector();
  if (m_update_align_btn)
    m_update_align_btn();
}

void MainWindow::update_rulers() {
  auto *doc = m_project ? m_project->active_doc() : nullptr;

  RulerState rs;
  rs.zoom = m_canvas.zoom();
  rs.pan_x = m_canvas.pan_x();
  rs.pan_y = m_canvas.pan_y();
  rs.widget_w = (double)m_canvas.get_width();
  rs.widget_h = (double)m_canvas.get_height();
  rs.cursor_doc_x = m_canvas.cursor_doc_x();
  rs.cursor_doc_y = m_canvas.cursor_doc_y();

  if (doc) {
    rs.canvas_w = (double)doc->canvas_width();
    rs.canvas_h = (double)doc->canvas_height();
    rs.quality = doc->canvas.quality;
    rs.display_mode = doc->canvas.display_mode;
    rs.ruler_origin_x = doc->ruler_origin_x;
    rs.ruler_origin_y = doc->ruler_origin_y;
    rs.phys_short = std::min(doc->canvas.phys_width, doc->canvas.phys_height);
    rs.phys_unit = doc->canvas.phys_unit;
    rs.display_unit = doc->canvas.display_unit;
  }

  m_hruler.set_state(rs);
  m_vruler.set_state(rs);
  m_corner.set_state(rs);
}

void MainWindow::update_clip_actions_sensitive() {
  // Clip: any non-empty canvas selection is a valid starting point; the
  //   stricter precondition (no ref, no guide, etc.) is inside
  //   Canvas::make_clip_group — if violated, the action no-ops and
  //   LOG_INFOs. Enabling the menu item just means "there is something
  //   to clip".
  // Release Clip: primary selection must be a ClipGroup.
  bool have_sel = !m_canvas.selection().empty();
  SceneNode *primary = m_canvas.selected_object();
  bool is_cg = (primary && primary->is_clip_group());
  if (m_act_clip_make)    m_act_clip_make->set_enabled(have_sel);
  if (m_act_clip_release) m_act_clip_release->set_enabled(is_cg);
}

void MainWindow::update_blend_action_sensitive() {
  // Blend: exactly 2 selected, both Paths. Deeper preconditions (same
  // parent, equal node counts, equal closed flag) are validated inside
  // Canvas::make_blend with a user-visible error message — not surfaced
  // as greyed state here because the user needs feedback about what's
  // wrong, not just a silent disabled menu.
  // Release Blend: primary selection is a Blend.
  const auto &sel = m_canvas.selection();
  bool make_ok = (sel.size() == 2) &&
                 sel[0] && sel[0]->type == SceneNode::Type::Path &&
                 sel[1] && sel[1]->type == SceneNode::Type::Path;
  if (m_act_blend_make) m_act_blend_make->set_enabled(make_ok);

  SceneNode *primary = m_canvas.selected_object();
  bool release_ok = (primary && primary->is_blend());
  if (m_act_blend_release) m_act_blend_release->set_enabled(release_ok);
}

void MainWindow::update_warp_action_sensitive() {
  // Warp Make: exactly 1 selected, type ∈ {Path, Compound, Group}. No
  // "already inside a Warp" check here — the user can legitimately
  // warp a warped result after Flatten, or nest Warps. Canvas::make_warp
  // validates and reports user-visible errors for edge cases.
  // Release Warp / Flatten Warp: primary selection is a Warp.
  const auto &sel = m_canvas.selection();
  bool make_ok = (sel.size() == 1) && sel[0] &&
                 (sel[0]->type == SceneNode::Type::Path ||
                  sel[0]->type == SceneNode::Type::Compound ||
                  sel[0]->type == SceneNode::Type::Group);
  if (m_act_warp_make) m_act_warp_make->set_enabled(make_ok);

  SceneNode *primary = m_canvas.selected_object();
  bool warp_ok = (primary && primary->is_warp());
  if (m_act_warp_edit) m_act_warp_edit->set_enabled(warp_ok);
  if (m_act_warp_release) m_act_warp_release->set_enabled(warp_ok);
  if (m_act_warp_flatten) m_act_warp_flatten->set_enabled(warp_ok);
}

void MainWindow::update_bool_actions_sensitive() {
  // s122 m3: Union / Subtract / Intersect — at least 2 selected, each
  // either a closed Path or a Compound with all-closed children. Same-
  // parent check stays inside Canvas::boolean_op (with user-visible
  // error) — the user benefits from a specific message when that fails,
  // not a silently-disabled menu.
  // (s122 m2 originally hard-gated at exactly-2 to keep the unstable
  // iterative-fold path unreachable; m3 replaces the fold with Clipper2's
  // native N-way Union and an associative Intersect iteration, so the
  // hazard is gone and N>=2 is fully supported.)
  const auto &sel = m_canvas.selection();
  bool ok = (sel.size() >= 2);
  if (ok) {
    for (SceneNode *n : sel) {
      if (!n) { ok = false; break; }
      const bool is_path = (n->path != nullptr);
      const bool is_compound =
          (n->type == SceneNode::Type::Compound && !n->children.empty());
      if (!is_path && !is_compound) { ok = false; break; }
      // Closedness check, same shape as Canvas::boolean_op
      if (is_path) {
        if (!n->path->closed) { ok = false; break; }
      } else {
        for (auto &ch : n->children) {
          if (!ch || !ch->path || !ch->path->closed) { ok = false; break; }
        }
        if (!ok) break;
      }
    }
  }
  if (m_act_bool_union)     m_act_bool_union->set_enabled(ok);
  if (m_act_bool_subtract)  m_act_bool_subtract->set_enabled(ok);
  if (m_act_bool_intersect) m_act_bool_intersect->set_enabled(ok);
}

void MainWindow::refresh_inspector() {
  if (m_closing)
    return;
  // Action enable/disable follows the selection — refresh these up-front
  // (cheap, no widget rebuild) before the deferred panel refresh.
  update_clip_actions_sensitive();
  update_blend_action_sensitive();
  update_warp_action_sensitive();
  update_bool_actions_sensitive();  // s122 m2
  Glib::signal_idle().connect_once([this]() {
    if (m_closing)
      return;
    auto *doc = m_project ? m_project->active_doc() : nullptr;
    SceneNode *sel = m_canvas.selected_object();
    CanvasModel *cm = doc ? &doc->canvas : nullptr;

    if (doc)
      m_properties.set_ruler_origin(doc->ruler_origin_x, doc->ruler_origin_y);
    else
      m_properties.set_ruler_origin(0.0, 0.0);

    int tool = (int)m_active_tool;
    int node = m_canvas.selected_node();
    LOG_DEBUG("refresh_inspector: tool={} sel={} node={}", tool, (void *)sel,
              node);

    if (sel && m_active_tool == ActiveTool::Node) {
      LOG_DEBUG("refresh_inspector → refresh_node(node={})", node);
      m_properties.refresh_node(cm, sel, node);
    } else if (sel && (m_active_tool == ActiveTool::Selection ||
                       m_active_tool == ActiveTool::TextOnPath)) {
      LOG_DEBUG("refresh_inspector → refresh(sel)");
      m_properties.refresh(cm, sel);
    } else if (cm) {
      LOG_DEBUG("refresh_inspector → refresh(no sel)");
      m_properties.refresh(cm, nullptr);
    } else {
      m_properties.show_empty();
    }
    m_inspector_tool = m_active_tool;
  });
}

// ── refresh_status_counts ─────────────────────────────────────────────────
//
// s132 m2: replaces five duplicated "iterate doc.layers and sum
// children.size()" loops that all hardcoded `nodes=0` for the
// StatusBar's node counter — that counter has read 0 since the bar
// shipped because nobody had a recursive walk handy. Pumps live in
// curvz::utils so the bug class stops at one place.
//
// Safe to call in any state. No active project → 0/0. The pumps
// internally skip overlay layers (guides, grid, margins, refs,
// measurements) so the user-visible counts mirror what the Layers
// panel shows.
void MainWindow::refresh_status_counts() {
  if (m_closing) return;
  int objects = 0;
  int nodes   = 0;
  if (m_project) {
    if (auto *doc = m_project->active_doc()) {
      objects = curvz::utils::doc_object_count(*doc);
      nodes   = curvz::utils::doc_anchor_count(*doc);
    }
  }
  m_statusbar.set_counts(objects, nodes);
}

void MainWindow::on_doc_activated(int index) {
  if (!m_project || index < 0 || index >= (int)m_project->documents.size())
    return;
  // Clicking a project doc exits any active system icon preview
  if (m_preview_active) {
    m_preview_active = false;
    m_preview_doc.reset();
    m_statusbar.set_mode(m_canvas.is_outline_mode() ? "Outline" : "Preview");
  }
  m_project->active_doc_index = index;
  auto *doc = m_project->active_doc();
  m_canvas.set_document(doc);
  m_toolbar.set_document(doc);
  m_preview.set_document(doc);
  m_layers.set_document(doc);
  m_library.set_document(doc);
  if (doc) {
    CanvasModel *cm = &doc->canvas;
    Glib::signal_idle().connect_once([this, cm]() {
      if (m_closing)
        return;
      m_properties.show_document_props(cm);
    });
  }
  m_gallery.refresh();
  m_doc_tabs.refresh();
  update_rulers();
  update_title();
  apply_motif_to_window();
  LOG_INFO("Doc activated: index {}", index);
}

// s108 m7: cycle through the project's documents with wraparound. Called
// from the doc-next / doc-prev actions AND from the CAPTURE-phase
// keyboard handler (Ctrl+Tab / Ctrl+Page_Down can't reliably reach the
// action accel system because GTK4's focus-traversal default consumes
// Tab keys). Funnels through on_doc_activated so all panels stay in
// sync via the single canonical seam.
void MainWindow::cycle_doc(int delta) {
  if (!m_project) return;
  const int n = (int)m_project->documents.size();
  if (n <= 1) return;
  int cur = m_project->active_doc_index;
  if (cur < 0 || cur >= n) cur = 0;
  int next = ((cur + delta) % n + n) % n;
  on_doc_activated(next);
}

void MainWindow::on_undo() {
  if (!m_history.can_undo())
    return;
  LOG_INFO("Undo: {}", m_history.undo_description());
  if (!m_history.peek_undo()->preserves_selection())
    m_canvas.select_object(nullptr); // clear before nodes are destroyed
  m_history.undo();
  m_properties.reset_undo_coalesce();
  refresh_status_counts();  // s132 m2 — replaces hand-rolled loop
  m_canvas.queue_draw();
  m_gallery.refresh();
  m_styles.refresh();   // S80 m4c-2: bind/unbind undo updates indicator dot
  refresh_inspector();
}

void MainWindow::on_redo() {
  if (!m_history.can_redo())
    return;
  LOG_INFO("Redo: {}", m_history.redo_description());
  if (!m_history.peek_redo()->preserves_selection())
    m_canvas.select_object(nullptr); // clear before nodes are destroyed
  m_history.redo();
  m_properties.reset_undo_coalesce();
  refresh_status_counts();  // s132 m2 — replaces hand-rolled loop
  m_canvas.queue_draw();
  m_gallery.refresh();
  m_styles.refresh();   // S80 m4c-2: bind/unbind redo updates indicator dot
  refresh_inspector();
}

// ── Project management
// ────────────────────────────────────────────────────────

void MainWindow::load_project(std::unique_ptr<CurvzProject> project) {
  m_project = std::move(project);
  m_history = CommandHistory{};

  // Sync inspector section open-state flags from the loaded project.
  // The sections were built once in setup_layout; we update the shared flags
  // so any subsequent toggle gesture sees the correct starting state.
  auto sync_flag = [](const std::shared_ptr<bool> &flag, bool on) {
    if (flag)
      *flag = on;
  };
  sync_flag(m_sec_open_preview, m_project->sec_preview_open);
  sync_flag(m_sec_open_layers, m_project->sec_layers_open);
  sync_flag(m_sec_open_library, m_project->sec_library_open);
  sync_flag(m_sec_open_documents, m_project->sec_documents_open);
  sync_flag(m_sec_open_swatches, m_project->sec_swatches_open);
  sync_flag(m_sec_open_styles,   m_project->sec_styles_open);
  sync_flag(m_sec_open_content, m_project->sec_content_open);

  // Restore pane position — defer until after allocation
  Glib::signal_idle().connect_once([this]() {
    m_pane_ready = false;
    int pos = m_project ? m_project->pane_position : -1;
    // S94 m1 — same clamp as on first map (see ctor for rationale).
    const int RIGHT_PANE_MIN = 280;
    int paned_w = m_h_paned.get_width();
    if (pos > 0 && paned_w > 0 && pos > paned_w - RIGHT_PANE_MIN) {
      int clamped = std::max(100, paned_w - RIGHT_PANE_MIN);
      LOG_INFO("Pane position {} exceeds available width {}; "
               "clamping to {} so right pane keeps {}px minimum",
               pos, paned_w, clamped, RIGHT_PANE_MIN);
      pos = clamped;
    }
    LOG_INFO("Pane restore on project load: setting position to {}", pos);
    if (m_project)
      m_h_paned.set_position(pos);
    LOG_INFO("Pane position after set: {}", m_h_paned.get_position());
    m_pane_ready = true;
    m_pane_settled_at = g_get_monotonic_time() + 500000; // 500ms settle window
  });

  m_properties.set_project(m_project.get());

  // Propagate project-wide snap to all docs on load
  for (auto &doc : m_project->documents)
    doc->snap = m_project->snap;

  update_all_panels();
  update_project_sensitive();
  update_title();
  save_config();
  LOG_INFO("Project loaded: '{}'", m_project->directory);
}

// ── apply_motif_to_window ────────────────────────────────────────────────
// Read the project's motif and add/remove the `curvz-light` CSS class
// on the main window. The class is the only mechanical state — CSS tokens
// defined in css.hpp under `window.curvz-light {}` outrank the dark
// defaults under `window {}` and var() references throughout the
// stylesheet re-resolve automatically.
//
// Motif is project-scope (s116 m6). Switching tabs within the same
// project never changes the app theme — every doc shares the project's
// appearance. Switching projects *does* re-apply.
//
// Idempotent: GTK's CSS-class API is set-membership (add/remove are no-ops
// when state already matches), so callers don't need to track previous
// motif. Called from update_all_panels (project load / project switch),
// on_doc_activated (tab click — defensive; no-op within a project), and
// the prop_changed handler (Project panel toggle).
void MainWindow::sync_motif_class_to(Gtk::Window& w) {
  if (!m_project) return;
  if (m_project->motif == Motif::Light) {
    w.add_css_class("curvz-light");
  } else {
    w.remove_css_class("curvz-light");
  }
}

void MainWindow::apply_motif_to_window() {
  if (!m_project) return;
  // S117 m15 v2: tell GTK to load its dark/light theme variant to
  // match our motif. Adding `curvz-light` to a window tells our CSS
  // which token block to use, but GTK's *system* theme (Adwaita,
  // including the CSD chrome) loads its dark or light variant based
  // on the `gtk-application-prefer-dark-theme` setting. If the system
  // setting says "dark" but our motif is light, dialogs resolve their
  // base style against system-dark and our overrides only patch the
  // specific selectors we author — leaving dialog chrome dark.
  // Setting this property pulls the system-theme variant in line, so
  // dialogs paint light on first present.
  if (auto settings = Gtk::Settings::get_default()) {
    settings->property_gtk_application_prefer_dark_theme().set_value(
        m_project->motif == Motif::Dark);
  }
  // S117 m15 v3: walk every top-level GTK window — INCLUDING transient
  // dialogs created via make_managed<Gtk::Window> + set_transient_for
  // that never go through Gtk::Application::add_window(). Inspector
  // confirmed (m17 diag) that ThemesDialog's window node had only the
  // 'background csd' classes — no curvz-light — so our v1 get_windows()
  // walk was missing it entirely. gtk_window_get_toplevels() returns
  // every GtkWindow GTK knows about, regardless of app registration.
  sync_motif_class_to(*this);
  GListModel* tops = gtk_window_get_toplevels();
  guint n = tops ? g_list_model_get_n_items(tops) : 0;
  LOG_INFO("apply_motif: walking {} top-level windows", (int)n);
  for (guint i = 0; i < n; ++i) {
    GObject* obj = G_OBJECT(g_list_model_get_item(tops, i));
    if (!obj) continue;
    auto wrapped = Glib::wrap(GTK_WINDOW(obj));
    if (wrapped && wrapped != this) {
      LOG_INFO("apply_motif:   -> styling window {}",
               (void*)wrapped);
      sync_motif_class_to(*wrapped);
    }
    g_object_unref(obj);  // get_item gives us a ref
  }
  // S117 m3: rulers + corner are Cairo-painted (not CSS), so they need
  // explicit motif notification. Each setter is a no-op if the motif
  // didn't change, so calling unconditionally here is cheap and means
  // every motif-application path (boot, project switch, motif toggle)
  // automatically refreshes the ruler chrome.
  m_hruler.set_motif(m_project->motif);
  m_vruler.set_motif(m_project->motif);
  m_corner.set_motif(m_project->motif);
  // S117 m5: same shape for the toolbar's Cairo-painted align icon.
  m_toolbar.set_motif(m_project->motif);
  // S117 m14: thumbnails are Cairo-painted into ImageSurfaces inside
  // DocumentGallery::render_thumb, so they need a refresh() to redraw
  // with the new motif's artboard colour and currentColor sample.
  // S117 m14 v2: library thumbnails (LibraryPanel::render_thumb) follow
  // the same pattern — refresh both on motif change.
  m_gallery.refresh();
  m_library.refresh();
  // BACKLOG: GTK4 caches the rendered pixels of the CSD chrome wrapper
  // (`window.csd > box.vertical`) independently of style. Class change
  // invalidates the style — var() references re-resolve correctly —
  // but the painted surface of the CSD wrapper is a cached frame that
  // queue_draw / queue_resize / class re-add / inspector-toggle hack
  // (tested in v7) do NOT reach. Result: headerbar in light motif
  // paints the dark token's colour until the user manually opens GTK
  // Inspector. No clean fix found in s117. Tracked for future session.
}

void MainWindow::update_all_panels() {
  auto *doc = m_project->active_doc();
  m_canvas.set_document(doc);
  m_canvas.set_zoom(m_project->zoom);
  m_canvas.set_history(&m_history);
  m_canvas.set_swatch_library(&m_project->swatches);  // Phase 5 M3
  m_canvas.set_style_library(&m_project->styles);     // S78 m3d
  m_canvas.set_project(m_project.get());              // s116 m6 — workspace appearance reads
  m_preview.set_document(doc);
  m_layers.set_document(doc);
  m_library.set_document(doc);
  m_swatches.set_library(&m_project->swatches);
  m_styles.set_library(&m_project->styles);   // S80 m4c
  m_styles.set_swatch_library(&m_project->swatches);  // S85 cont-3
  m_toolbar.set_swatch_library(&m_project->swatches);  // S91
  // S87 — restore the dropdown selection on project switch.
  m_styles.set_active_category(m_project->style_active_category,
                               m_project->style_active_is_app_tier);
  m_gallery.set_project(m_project.get());
  Glib::signal_idle().connect_once([this]() {
    if (m_closing)
      return;
    m_gallery.refresh();
  });
  m_doc_tabs.set_project(m_project.get());
  m_doc_tabs.refresh();
  if (doc) {
    // Propagate project-wide snap before building inspector so it reads correct
    // values
    doc->snap = m_project->snap;
    m_toolbar.set_snap_enabled(doc->snap.enabled);
    m_toolbar.set_snap_settings(doc->snap);
    // Keep popover unit labels in sync with document display unit
    m_toolbar.set_document(doc);
    m_corner.set_unit(doc->canvas.display_unit);
    // Corner panel unit label
    {
      Unit u = (doc->canvas.display_mode == DisplayMode::Physical)
               ? UnitSystem::parse_unit(doc->canvas.phys_unit)
               : doc->canvas.display_unit;
      std::string unit_str = UnitSystem::label(u);
      m_corner_unit_label.set_text("Units: " + unit_str);
      // Set a sensible default radius in current units (0.1 of current unit)
      m_corner_radius_spin.set_value(0.1);
    }
  }
  if (doc) {
    CanvasModel *cm = &doc->canvas;
    Glib::signal_idle().connect_once([this, cm]() {
      if (m_closing)
        return;
      m_properties.show_document_props(cm);
    });
  } else {
    Glib::signal_idle().connect_once([this]() {
      if (m_closing)
        return;
      m_properties.show_empty();
    });
  }
  refresh_status_counts();  // s132 m2 — replaces hand-rolled loop
  m_statusbar.set_zoom(m_project->zoom * 100.0);
  update_rulers();
  apply_motif_to_window();
}

// ── rename_doc ────────────────────────────────────────────────────────────
// Rename a document within the current project. Sanitises the name (spaces
// → dashes, path separators → underscore), enforces uniqueness against
// other documents in the project, performs an `fs::rename` on disk if the
// project is saved, and updates all UI panels.
//
// Reused by:
//   • Inspector "File" entry (always operates on active_doc)
//   • DocumentGallery double-click / right-click → Rename (any doc by index)
void MainWindow::rename_doc(CurvzDocument *doc, std::string new_name) {
  if (!m_project || !doc)
    return;

  // Sanitise: spaces → dashes, strip path separators
  for (char &c : new_name) {
    if (c == ' ')
      c = '-';
    if (c == '/' || c == '\\')
      c = '_';
  }
  if (new_name.empty())
    return;

  std::string new_fname = new_name + ".svg";

  // No change?
  if (new_fname == fs::path(doc->filename).filename().string())
    return;

  // Ensure unique within project
  int suffix = 2;
  std::string base = new_name;
  while (std::any_of(m_project->documents.begin(), m_project->documents.end(),
                     [&](const auto &d) {
                       return d.get() != doc &&
                              fs::path(d->filename).filename().string() ==
                                  new_fname;
                     })) {
    new_fname = base + std::to_string(suffix++) + ".svg";
  }

  // Rename on disk if project is saved
  if (!m_project->directory.empty()) {
    fs::path old_path =
        fs::path(m_project->directory) / fs::path(doc->filename).filename();
    fs::path new_path = fs::path(m_project->directory) / new_fname;
    std::error_code ec;
    if (fs::exists(old_path, ec))
      fs::rename(old_path, new_path, ec);
    if (ec)
      LOG_WARN("rename_doc: rename failed: {}", ec.message());
    else
      LOG_INFO("rename_doc: '{}' → '{}'", old_path.filename().string(),
               new_fname);
  }

  doc->filename = new_fname;
  m_project->save();
  update_all_panels();
}

void MainWindow::update_title() {
  // No window title shown — project name lives in the status bar footer.
  if (!m_project || m_project->directory.empty()) {
    m_statusbar.set_project_name("unsaved");
  } else {
    std::string name = fs::path(m_project->directory).filename().string();
    m_statusbar.set_project_name(name + "  ✓");
  }
  // Active document name in status bar footer
  auto *doc = m_project ? m_project->active_doc() : nullptr;
  if (doc && !doc->filename.empty()) {
    std::string dname = fs::path(doc->filename).filename().string();
    m_statusbar.set_doc_name(dname);
  } else {
    m_statusbar.set_doc_name("untitled");
  }
}

// ── File operations (GTK4 async FileDialog)

// Helper: enable/disable project-dependent actions based on whether a
// project is currently open.
void MainWindow::update_project_sensitive() {
  bool open = (m_project != nullptr);
  for (const char *name : {"new", "save", "save-as", "save-as-template",
                            "close-project",
                            "import-svg", "place-image", "export-theme",
                            "print", "step-repeat", "undo", "redo",
                            "zoom-fit", "zoom-in", "zoom-out",
                            "zoom-100", "zoom-200", "zoom-selection"}) {
    if (auto act = lookup_action(name)) {
      if (auto sa = std::dynamic_pointer_cast<Gio::SimpleAction>(act))
        sa->set_enabled(open);
    }
  }
}

// Helper: if there are unsaved changes, show a Save/Discard/Cancel dialog.
// Calls `then` only when the user chooses Save (after saving) or Discard.
void MainWindow::check_unsaved_then(std::function<void()> then) {
  if (!m_project || !m_history.can_undo()) {
    then();
    return;
  }
  curvz::utils::show_confirm(
      *this, "Save changes to this project?",
      "Your changes will be lost if you don't save them.",
      {"Cancel", "Discard", "Save"},
      /*default_button=*/2, /*cancel_button=*/0,
      [this, then](int btn) {
        if (btn == 0 || btn == -1) return;       // Cancel / dismissed
        if (btn == 1) { then(); return; }        // Discard
        if (btn == 2) { on_save(); then(); }     // Save then proceed
      });
}

void MainWindow::on_new_project() {
  check_unsaved_then([this]() {
    // First pick the project location
    auto file_dialog = Gtk::FileDialog::create();
    file_dialog->set_title("New Project — choose location and name");
    file_dialog->set_initial_name("MyIcons");
    file_dialog->save(*this, [this, file_dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
      try {
        auto file = file_dialog->save_finish(result);
        if (!file) return;
        std::string path = file->get_path();
        // Strip extension if user typed one
        auto dot = path.rfind('.');
        auto sep = path.rfind('/');
        if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
          path = path.substr(0, dot);
        std::string dir = path + ".curvz";
        // Create an empty project — the user populates it via Add to Project.
        auto project = CurvzProject::create_empty(dir);
        if (!project) {
          LOG_ERROR("on_new_project: create_empty failed for '{}'", dir);
          return;
        }
        load_project(std::move(project));
      } catch (...) {}
    });
  });
}

void MainWindow::on_close_project() {
  check_unsaved_then([this]() {
    // Clear config so next launch doesn't reopen this project
    std::string cfg = config_path();
    if (std::filesystem::exists(cfg))
      std::filesystem::remove(cfg);

    // Drop the project — leave the app in an empty waiting state
    m_project.reset();
    m_history = CommandHistory{};

    // Clear all panels
    m_canvas.set_document(nullptr);
    m_canvas.set_history(nullptr);
    m_preview.set_document(nullptr);
    m_layers.set_document(nullptr);
    m_library.set_document(nullptr);
    m_doc_tabs.set_project(nullptr);
    m_doc_tabs.refresh();
    m_gallery.set_project(nullptr);
    m_properties.set_project(nullptr);
    Glib::signal_idle().connect_once([this]() {
      if (!m_closing)
        m_properties.show_empty();
    });

    update_project_sensitive();
    update_title();
    LOG_INFO("Project closed");
  });
}

// File → Add to Project… — adds a new document to the currently loaded
// project. Disabled (via update_project_sensitive) when no project is open.
// Same behavior as the DocTabBar "+" button.
void MainWindow::on_new() {
  if (!m_project) return;  // guarded by menu sensitivity, belt-and-braces
  m_new_doc_dialog.show(*this, ndd_available_themes(),
      [this](std::unique_ptr<CurvzDocument> seed,
             std::string name,
             std::optional<theme::ThemeId> theme_id) {
    if (!seed) return;
    if (!m_project) return;  // project may have closed between dialog ops
    ndd_apply_chosen_theme(*seed, theme_id);

    // Sanitise name → filename stem.
    std::string base = doc_stem_from_name(name);
    std::string fname = base + ".svg";
    // Ensure unique within project
    int suffix = 2;
    while (std::any_of(m_project->documents.begin(),
                       m_project->documents.end(),
                       [&](const auto &d) { return d->filename == fname; }))
      fname = base + std::to_string(suffix++) + ".svg";

    seed->filename = fname;

    m_project->documents.push_back(std::move(seed));
    m_project->active_doc_index = (int)m_project->documents.size() - 1;
    m_project->save();
    update_all_panels();
    LOG_INFO("on_new: added '{}' to project", fname);
  });
}

void MainWindow::on_open() {
  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Open Project (.curvz folder)");
  // s125 m1e: re-open at the last-used parent directory if remembered.
  // For Open Project the picker selects a *.curvz folder, so the value we
  // store is the *parent* of the chosen folder (where the user was browsing),
  // not the .curvz folder itself.
  std::string remembered = get_last_folder("open");
  if (!remembered.empty() && fs::is_directory(remembered)) {
    try {
      dialog->set_initial_folder(Gio::File::create_for_path(remembered));
    } catch (...) {}
  }
  dialog->select_folder(*this,
                        [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
                          try {
                            auto file = dialog->select_folder_finish(result);
                            if (!file)
                              return;
                            // Remember the parent directory the user was
                            // browsing, not the .curvz folder itself.
                            std::string chosen = file->get_path();
                            std::string parent =
                                fs::path(chosen).parent_path().string();
                            set_last_folder("open", parent);
                            auto project = CurvzProject::open(chosen);
                            if (!project) {
                              LOG_ERROR("on_open: open failed");
                              return;
                            }
                            load_project(std::move(project));
                          } catch (...) {
                          }
                        });
}

void MainWindow::on_save() {
  if (!m_project)
    return;
  if (m_project->directory.empty()) {
    on_save_as();
    return;
  }
  // If a drag is in-flight, defer 100ms and retry — saves mid-drag corrupt node
  // data.
  if (m_canvas.is_dragging()) {
    Glib::signal_timeout().connect_once([this]() { on_save(); }, 100);
    return;
  }
  if (m_project->save()) {
    update_title();
    LOG_INFO("on_save: saved '{}'", m_project->directory);
  } else {
    LOG_ERROR("on_save: failed");
  }
}

void MainWindow::on_save_as() {
  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Save Project As — enter project name");
  // Pre-fill with current project name if we have one
  if (!m_project->directory.empty()) {
    std::string cur = fs::path(m_project->directory).stem().string();
    dialog->set_initial_name(cur);
  } else {
    dialog->set_initial_name("MyIcons");
  }
  dialog->save(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
    try {
      auto file = dialog->save_finish(result);
      if (!file)
        return;
      std::string path = file->get_path();
      // Strip extension, append .curvz
      auto dot = path.rfind('.');
      auto sep = path.rfind('/');
      if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
        path = path.substr(0, dot);
      std::string final_path = path + ".curvz";

      // Confirm overwrite if file exists
      if (fs::exists(final_path)) {
        curvz::utils::show_confirm(
            *this,
            "\"" + fs::path(final_path).filename().string() +
                "\" already exists.\nDo you want to replace it?",
            "The existing file will be replaced.",
            {"Cancel", "Replace"},
            /*default_button=*/0, /*cancel_button=*/0,
            [this, final_path](int btn) {
              if (btn == 1) do_save_as(final_path);
            });
      } else {
        do_save_as(final_path);
      }
    } catch (...) {
    }
  });
}

// Save active document as a user-global template. Opens SaveAsTemplateDialog;
// on accept checks for an existing bundle with the same (category, slug) and
// prompts Replace before calling templates::save().
void MainWindow::on_save_as_template() {
  if (!m_project) return;
  CurvzDocument *active = m_project->active_doc();
  if (!active) {
    LOG_WARN("on_save_as_template: no active document");
    return;
  }

  std::string suggested;
  if (!active->filename.empty()) {
    suggested = fs::path(active->filename).stem().string();
    for (char &c : suggested)
      if (c == '-' || c == '_') c = ' ';
  }

  m_save_as_template_dialog.show(*this, suggested,
      [this](templates::TemplateMeta meta) {
    auto do_save = [this, meta]() {
      if (!m_project) return;
      CurvzDocument *doc = m_project->active_doc();
      if (!doc) {
        LOG_WARN("on_save_as_template: no active document at save time");
        return;
      }
      std::string bundle;
      if (templates::save(*doc, meta, &bundle)) {
        LOG_INFO("on_save_as_template: saved '{}'", bundle);
      } else {
        LOG_ERROR("on_save_as_template: save failed");
      }
    };

    if (templates::user_bundle_exists(meta.category, meta.name)) {
      curvz::utils::show_confirm(
          *this,
          "A template named \"" + meta.name +
              "\" already exists in \"" + meta.category +
              "\".\nDo you want to replace it?",
          "The existing template will be overwritten.",
          {"Cancel", "Replace"},
          /*default_button=*/0, /*cancel_button=*/0,
          [do_save](int btn) {
            if (btn == 1) do_save();
          });
    } else {
      do_save();
    }
  });
}

// File → Manage Templates — opens the template browser. Enabled regardless
// of project state (templates are user-global).
void MainWindow::on_manage_templates() {
  m_manage_templates_dialog.show(*this, [this]() {
    // No-op for now: NewDocumentDialog rescans templates on each show(),
    // so changes from the manager are picked up the next time the user
    // opens Add to Project or New Project. If a persistent picker UI ever
    // lives outside the dialog, wire its refresh here.
    LOG_INFO("on_manage_templates: templates changed");
  });
}

// S103 m3 — Project → Themes… The dialog manages its own lifetime
// (self-deleting on close, same pattern as StyleEditorDialog). Apply
// is non-undoable per design; library mutations push commands onto
// our m_history. The on_changed callback wires canvas redraw +
// inspector refresh after any successful apply or library mutation
// (renames bubble through too — they don't visually change anything
// on canvas, but the inspector's snap section reads doc->snap which
// the apply path may have rewritten, so a refresh keeps the panel
// aligned).
void MainWindow::on_show_themes() {
  if (!m_project) {
    LOG_INFO("on_show_themes: no project, ignoring");
    return;
  }
  new ThemesDialog(*this, m_project.get(), &m_history,
                   [this]() {
                     m_canvas.queue_draw();
                     refresh_inspector();
                     schedule_save();
                   });
}

void MainWindow::on_export_docs() {
  if (!m_project) {
    LOG_INFO("on_export_docs: no project, ignoring");
    return;
  }
  if (m_project->documents.empty()) {
    LOG_INFO("on_export_docs: project has no documents, ignoring");
    return;
  }
  // Self-managed dialog. The dialog deletes itself on close; the
  // done_cb is just for logging — the dialog handles its own success
  // confirmation via an AlertDialog after a successful export.
  new ExportDocsDialog(*this, *m_project,
                       [](bool success, std::string dir) {
                         if (success)
                           LOG_INFO("on_export_docs: export complete → '{}'", dir);
                         else
                           LOG_INFO("on_export_docs: export cancelled or failed");
                       });
}

// S104 m1 follow-on — NDD theme dropdown helpers ─────────────────────────
//
// Snapshot the user-tier of the project's theme library as a flat
// vector<Theme>, in the same display order ThemesDialog uses. Ordered
// pass: each user category in insertion order, then each theme within
// the category. Returns empty when there's no project.
std::vector<theme::Theme> MainWindow::ndd_available_themes() const {
  std::vector<theme::Theme> out;
  if (!m_project) return out;
  for (const std::string& cat : m_project->themes.user_categories()) {
    for (const theme::Theme* t :
             m_project->themes.user_themes_in_category(cat)) {
      if (t) out.push_back(*t);  // copy by value — sub-100 themes, cheap
    }
  }
  return out;
}

// Apply the user's theme dropdown choice (if any) to the freshly-built
// seed. Looks up the theme by id; if found, copies its settings into the
// seed via the same apply_theme_to_doc() pump used by ThemesDialog.
//
// Defensive: id may reference a theme that was deleted between NDD
// opening and Create being clicked — in that case find_theme returns
// null and we silently produce an un-themed doc. The user gets what
// they would have gotten with no theme; nothing breaks.
void MainWindow::ndd_apply_chosen_theme(
    CurvzDocument& seed,
    const std::optional<theme::ThemeId>& id) const {
  if (!id || id->empty()) return;
  if (!m_project) return;
  const theme::Theme* t = m_project->themes.find_theme(*id);
  if (!t) {
    LOG_INFO("ndd_apply_chosen_theme: theme id '{}' no longer exists, "
             "creating doc without theme", *id);
    return;
  }
  theme::apply_theme_to_doc(*t, seed);
  LOG_INFO("ndd_apply_chosen_theme: applied '{}' to new doc", t->header.name);
}

// Import SVG — preserves original fill/stroke colors.
void MainWindow::on_import_svg() {
  if (!m_project) return;

  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Import SVG");

  auto filter = Gtk::FileFilter::create();
  filter->set_name("SVG files");
  filter->add_mime_type("image/svg+xml");
  filter->add_pattern("*.svg");
  auto filters = Gio::ListStore<Gtk::FileFilter>::create();
  filters->append(filter);
  dialog->set_filters(filters);

  dialog->open(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
    try {
      auto file = dialog->open_finish(result);
      if (!file) return;
      import_svg_impl(file->get_path(),
                      /*force_currentcolor=*/false,
                      /*normalize_to_1000=*/false);
    } catch (...) {
      LOG_ERROR("on_import_svg: exception during import");
    }
  });
}

// Import as Icon — converts all Solid fills/strokes to currentColor.
void MainWindow::on_import_svg_as_icon() {
  if (!m_project) return;

  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Import as Icon");

  auto filter = Gtk::FileFilter::create();
  filter->set_name("SVG files");
  filter->add_mime_type("image/svg+xml");
  filter->add_pattern("*.svg");
  auto filters = Gio::ListStore<Gtk::FileFilter>::create();
  filters->append(filter);
  dialog->set_filters(filters);

  dialog->open(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
    try {
      auto file = dialog->open_finish(result);
      if (!file) return;
      import_svg_impl(file->get_path(),
                      /*force_currentcolor=*/true,
                      /*normalize_to_1000=*/true);
    } catch (...) {
      LOG_ERROR("on_import_svg_as_icon: exception during import");
    }
  });
}

// Shared import body.
//   force_currentcolor: convert Solid fills/strokes to CurrentColor.
//   normalize_to_1000:  rescale coords so long axis = 1000 (icon workflow).
//                       When false, use SVG's own dimensions verbatim.
void MainWindow::import_svg_impl(const std::string& path,
                                 bool force_currentcolor,
                                 bool normalize_to_1000) {
  if (!m_project) return;

  try {
      auto imported = Curvz::parse_svg_file(path);
      if (!imported) {
        LOG_ERROR("import_svg_impl: failed to parse '{}'", path);
        return;
      }

      double src_w = (double)imported->canvas_width();
      double src_h = (double)imported->canvas_height();
      if (src_w <= 0) src_w = 1000.0;
      if (src_h <= 0) src_h = 1000.0;

      double scale;
      int dst_w, dst_h;
      if (normalize_to_1000) {
        // Icon workflow — rescale so the longest axis is 1000 units.
        constexpr double QUALITY = 1000.0;
        scale = QUALITY / std::max(src_w, src_h);
        dst_w = (int)std::round(src_w * scale);
        dst_h = (int)std::round(src_h * scale);
      } else {
        // Generic SVG import — preserve authored geometry verbatim.
        scale = 1.0;
        dst_w = (int)std::round(src_w);
        dst_h = (int)std::round(src_h);
      }

      LOG_INFO("import_svg_impl: src={}x{} → dst={}x{} scale={:.6f} "
               "(normalize={}, force_cc={})",
               (int)src_w, (int)src_h, dst_w, dst_h, scale,
               normalize_to_1000, force_currentcolor);

      // Scale every node in the imported tree (no-op when scale == 1.0)
      std::function<void(SceneNode &)> scale_node = [&](SceneNode &n) {
        if (scale != 1.0 && n.path) {
          for (auto &nd : n.path->nodes) {
            nd.x = nd.x * scale;
            nd.y = nd.y * scale;
            nd.cx1 = nd.cx1 * scale;
            nd.cy1 = nd.cy1 * scale;
            nd.cx2 = nd.cx2 * scale;
            nd.cy2 = nd.cy2 * scale;
          }
        }
        if (scale != 1.0 && n.type == SceneNode::Type::Text) {
          n.text_x *= scale;
          n.text_y *= scale;
          n.text_font_size *= scale;
        }
        for (auto &child : n.children)
          scale_node(*child);
      };

      // ── Build a new CurvzDocument for this import ───────────────────────
      auto new_doc = std::make_unique<CurvzDocument>();

      if (normalize_to_1000) {
        // Icon workflow — canonical pixel canvas at normalized dimensions.
        new_doc->canvas = CanvasModel::from_pixels(dst_w, dst_h);
      } else {
        // Generic import — inherit the parsed canvas model verbatim so any
        // physical-mode settings, DPI, or display unit in the source SVG
        // survive the import.
        new_doc->canvas = imported->canvas;
      }

      // Filename derived from the SVG filename stem
      std::string stem = fs::path(path).stem().string();
      std::string fname = stem + ".svg";
      // Ensure unique within project
      int suffix = 2;
      while (std::any_of(m_project->documents.begin(),
                         m_project->documents.end(),
                         [&](const auto &d) { return d->filename == fname; }))
        fname = stem + std::to_string(suffix++) + ".svg";
      new_doc->filename = fname;

      // Ensure the new doc has a default layer
      new_doc->ensure_guide_layer();
      auto layer = std::make_unique<SceneNode>();
      layer->type = SceneNode::Type::Layer;
      layer->internal_id = generate_internal_id();
      layer->name = new_doc->next_default_name(CurvzDocument::NameKind::Layer);
      SceneNode *target_layer = layer.get();
      new_doc->layers.insert(new_doc->layers.begin(), std::move(layer));
      new_doc->active_layer_index = 0;

      // Move scaled objects into the new doc's layer
      int imported_count = 0;
      static int s_import_counter = 1;

      // Helper: fix fill/stroke — convert hardcoded colors to currentColor
      std::function<void(SceneNode &)> fix_style = [&](SceneNode &n) {
        if (n.fill.type == FillStyle::Type::Solid)
          n.fill.type = FillStyle::Type::CurrentColor;
        if (n.stroke.paint.type == FillStyle::Type::Solid)
          n.stroke.paint.type = FillStyle::Type::CurrentColor;
        for (auto &child : n.children)
          fix_style(*child);
      };

      for (auto &imp_layer : imported->layers) {
        LOG_INFO("import_svg_impl: layer '{}' type={} is_layer={} children={}",
                 imp_layer->name, (int)imp_layer->type,
                 imp_layer->is_layer(), imp_layer->children.size());
        if (!imp_layer->is_layer())
          continue;
        for (auto &child : imp_layer->children) {
          LOG_INFO("import_svg_impl: scaling child type={} has_path={}",
                   (int)child->type, child->path != nullptr);
          scale_node(*child);
          if (force_currentcolor)
            fix_style(*child);
          child->id = "imp" + std::to_string(s_import_counter++);
          target_layer->children.push_back(std::move(child));
          ++imported_count;
        }
      }

      if (imported_count == 0) {
        LOG_INFO("import_svg_impl: no objects found in '{}'", path);
        return;
      }

      // Add new doc to project and activate it
      m_project->documents.push_back(std::move(new_doc));
      m_project->active_doc_index = (int)m_project->documents.size() - 1;
      m_project->save();
      update_all_panels();

      LOG_INFO("import_svg_impl: imported {} objects from '{}' as '{}' "
               "(force_currentcolor={})",
               imported_count, path, fname, force_currentcolor);

    } catch (...) {
      LOG_ERROR("import_svg_impl: exception during import");
    }
}

double MainWindow::pop_to_px(double v) const {
  auto *doc = m_project ? m_project->active_doc() : nullptr;
  if (!doc) return v;
  const CanvasModel& cm = doc->canvas;
  if (cm.display_mode == DisplayMode::Physical && cm.quality > 0) {
    double phys_short = std::min(cm.phys_width, cm.phys_height);
    if (phys_short <= 0) return v;
    return v / phys_short * cm.quality;
  }
  return UnitSystem::to_px(v, cm.display_unit);
}

bool MainWindow::import_svg_as_doc(const std::string& path,
                                   bool force_currentcolor) {
  if (!m_project) return false;

  auto imported = Curvz::parse_svg_file(path);
  if (!imported) {
    LOG_ERROR("import_svg_as_doc: failed to parse '{}'", path);
    return false;
  }

  double src_w = (double)imported->canvas_width();
  double src_h = (double)imported->canvas_height();
  if (src_w <= 0) src_w = 1000.0;
  if (src_h <= 0) src_h = 1000.0;

  constexpr double QUALITY = 1000.0;
  double scale = QUALITY / std::max(src_w, src_h);
  int dst_w = (int)std::round(src_w * scale);
  int dst_h = (int)std::round(src_h * scale);

  LOG_INFO("import_svg_as_doc: src={}x{} -> dst={}x{} scale={:.6f}",
           (int)src_w, (int)src_h, dst_w, dst_h, scale);

  std::function<void(SceneNode &)> scale_node = [&](SceneNode &n) {
    if (n.path) {
      for (auto &nd : n.path->nodes) {
        nd.x   *= scale; nd.y   *= scale;
        nd.cx1 *= scale; nd.cy1 *= scale;
        nd.cx2 *= scale; nd.cy2 *= scale;
      }
    }
    if (n.type == SceneNode::Type::Text) {
      n.text_x *= scale;
      n.text_y *= scale;
      n.text_font_size *= scale;
    }
    for (auto &child : n.children) scale_node(*child);
  };

  std::function<void(SceneNode &)> fix_style = [&](SceneNode &n) {
    if (n.fill.type == FillStyle::Type::Solid)
      n.fill.type = FillStyle::Type::CurrentColor;
    if (n.stroke.paint.type == FillStyle::Type::Solid)
      n.stroke.paint.type = FillStyle::Type::CurrentColor;
    for (auto &child : n.children) fix_style(*child);
  };

  auto new_doc = std::make_unique<CurvzDocument>();
  new_doc->canvas = CanvasModel::from_pixels(dst_w, dst_h);

  std::string stem  = fs::path(path).stem().string();
  std::string fname = stem + ".svg";
  int suffix = 2;
  while (std::any_of(m_project->documents.begin(), m_project->documents.end(),
                     [&](const auto &d){ return d->filename == fname; }))
    fname = stem + std::to_string(suffix++) + ".svg";
  new_doc->filename = fname;

  new_doc->ensure_guide_layer();
  auto layer = std::make_unique<SceneNode>();
  layer->type = SceneNode::Type::Layer;
  layer->internal_id = generate_internal_id();
  layer->name = new_doc->next_default_name(CurvzDocument::NameKind::Layer);
  SceneNode *target_layer = layer.get();
  new_doc->layers.insert(new_doc->layers.begin(), std::move(layer));
  new_doc->active_layer_index = 0;

  int imported_count = 0;
  static int s_import_counter = 1;
  for (auto &imp_layer : imported->layers) {
    if (!imp_layer->is_layer()) continue;
    for (auto &child : imp_layer->children) {
      scale_node(*child);
      if (force_currentcolor)
        fix_style(*child);
      child->id = "imp" + std::to_string(s_import_counter++);
      target_layer->children.push_back(std::move(child));
      ++imported_count;
    }
  }

  if (imported_count == 0) {
    LOG_INFO("import_svg_as_doc: no objects found in '{}'", path);
    return false;
  }

  m_project->documents.push_back(std::move(new_doc));
  m_project->active_doc_index = (int)m_project->documents.size() - 1;
  m_project->save();
  update_all_panels();
  LOG_INFO("import_svg_as_doc: imported {} objects from '{}' as '{}'",
           imported_count, path, fname);
  return true;
}

void MainWindow::on_preview_icon(const std::string& path) {
  if (!m_project) return;

  if (!m_preview_active) {
    m_preview_saved_index = m_project->active_doc_index;
    m_preview_active = true;
  }

  auto doc = Curvz::parse_svg_file(path);
  if (!doc) {
    LOG_WARN("on_preview_icon: failed to parse '{}'", path);
    return;
  }

  m_preview_doc = std::move(doc);
  m_canvas.set_document(m_preview_doc.get());
  m_canvas.queue_draw();

  std::string name = fs::path(path).stem().string();
  m_statusbar.set_mode("Previewing: " + name + "  (double-click to copy)");
}

void MainWindow::exit_preview_mode() {
  if (!m_preview_active) return;
  m_preview_active = false;
  m_preview_doc.reset();
  m_project->active_doc_index = m_preview_saved_index;
  update_all_panels();
  m_statusbar.set_mode(m_canvas.is_outline_mode() ? "Outline" : "Preview");
}

void MainWindow::on_copy_icon(const std::string& path) {
  if (!m_project) return;

  std::string name = fs::path(path).stem().string();
  curvz::utils::show_confirm(
      *this, "Copy icon to project?",
      "Add '" + name + "' as a new document in this project.",
      {"Copy", "Cancel"},
      /*default_button=*/0, /*cancel_button=*/1,
      [this, path](int response) {
        if (response == 0) {
          exit_preview_mode();
          import_svg_as_doc(path);
        }
      });
}

void MainWindow::on_export_theme() {
  if (!m_project)
    return;
  m_export_dialog.show(*this, *m_project, [](bool success, std::string dir) {
    if (success)
      LOG_INFO("on_export_theme: export complete → '{}'", dir);
    else
      LOG_INFO("on_export_theme: export cancelled or failed");
  });
}

void MainWindow::on_quit() {
  m_closing = true;
  // Save project (when one exists with a directory) and always flush
  // app-level config so last_folders / outline_mode / window state
  // persist across runs regardless of project state. (s125 m1e — the
  // unconditional save_config() landed when save_config itself was
  // hardened against the no-project case.)
  if (m_project && !m_project->directory.empty())
    m_project->save();
  save_config();
  // Close the window — the application will quit when all windows are closed
  close();
}

void MainWindow::do_save_as(const std::string &dir) {
  m_project->directory = dir;
  if (!m_project->save()) {
    LOG_ERROR("do_save_as: failed");
    return;
  }
  save_config();
  update_title();
}

// ── App config (last opened project)

std::string MainWindow::config_path() const {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  std::string base =
      xdg ? xdg
          : (std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.config");
  return base + "/curvz/settings.json";
}

// s125 m1e: per-purpose last-folder accessors. See MainWindow.hpp banner.
std::string MainWindow::get_last_folder(const std::string &purpose) const {
  auto it = m_last_folders.find(purpose);
  return (it == m_last_folders.end()) ? std::string{} : it->second;
}

void MainWindow::set_last_folder(const std::string &purpose,
                                 const std::string &path) {
  if (path.empty())
    return;
  m_last_folders[purpose] = path;
  // Don't write settings.json synchronously here — that would mean a disk
  // hit per file picker close. Existing save_config call sites (project
  // load, save-as, quit) flush to disk; that's enough for "remembered
  // across runs" semantics. Worst case after a crash: forget the most
  // recent folder choice. Acceptable.
}

void MainWindow::save_config() const {
  // s125 m1e: relaxed the "no project → no save" early-return that used to
  // sit here. App-level state (window_maximized, outline_mode, inspector
  // sections, last_folders) shouldn't be lost when there's no project
  // open. last_project is written as "" in that case.
  std::string path = config_path();
  fs::create_directories(fs::path(path).parent_path());
  bool outline = m_canvas.is_outline_mode();

  // Serialize inspector section open/close state
  nlohmann::json sections = nlohmann::json::object();
  for (auto &[k, v] : m_properties.section_open_state())
    sections[k] = v;

  // s129 m7: help-window sidebar open/close state. Same shape as
  // inspector_sections — a flat object keyed by row title. Drives the
  // collapsibles in HelpWindow's outline tree.
  nlohmann::json help_sections = nlohmann::json::object();
  for (auto &[k, v] : m_help_window.sidebar_open_state())
    help_sections[k] = v;

  // s125 m1e: serialise last-folders.
  nlohmann::json folders = nlohmann::json::object();
  for (auto &[purpose, dir] : m_last_folders)
    folders[purpose] = dir;

  std::string last_project =
      (m_project && !m_project->directory.empty()) ? m_project->directory : "";

  nlohmann::json j = {{"last_project", last_project},
                      {"outline_mode", outline},
                      {"window_maximized", is_maximized()},
                      {"inspector_sections", sections},
                      {"help_sidebar_sections", help_sections},
                      {"last_folders", folders}};
  std::ofstream f(path);
  if (f) {
    f << j.dump(2) << "\n";
  }
}

std::string MainWindow::load_last_project_path() {
  std::ifstream f(config_path());
  if (!f)
    return {};
  try {
    nlohmann::json j;
    f >> j;

    // Restore outline mode. NOTE: this runs from setup_project() during
    // construction, before connect_signals() wires up
    // signal_outline_mode_changed — so the inline sync below is required
    // here. Other call sites rely on the signal handler.
    bool outline = j.value("outline_mode", false);
    if (outline != m_canvas.is_outline_mode()) {
      m_canvas.toggle_outline_mode();
      m_statusbar.set_mode(outline ? "Outline" : "Preview");
      if (auto act = lookup_action("toggle-outline")) {
        auto sa = std::dynamic_pointer_cast<Gio::SimpleAction>(act);
        if (sa)
          sa->set_state(Glib::Variant<bool>::create(outline));
      }
    }

    // Restore window maximized state
    if (j.value("window_maximized", false))
      maximize();

    // Restore inspector section open/close state
    if (j.contains("inspector_sections") &&
        j["inspector_sections"].is_object()) {
      std::map<std::string, bool> state;
      for (auto &[k, v] : j["inspector_sections"].items())
        if (v.is_boolean())
          state[k] = v.get<bool>();
      m_properties.set_section_open_state(state);
    }

    // s129 m7: restore help-window sidebar open/close state.
    if (j.contains("help_sidebar_sections") &&
        j["help_sidebar_sections"].is_object()) {
      std::map<std::string, bool> state;
      for (auto &[k, v] : j["help_sidebar_sections"].items())
        if (v.is_boolean())
          state[k] = v.get<bool>();
      m_help_window.set_sidebar_open_state(state);
    }

    // s125 m1e: restore per-purpose last-folder map.
    if (j.contains("last_folders") && j["last_folders"].is_object()) {
      m_last_folders.clear();
      for (auto &[k, v] : j["last_folders"].items())
        if (v.is_string())
          m_last_folders[k] = v.get<std::string>();
    }

    return j.value("last_project", std::string{});
  } catch (...) {
    return {};
  }
}

// ── Corner Treatment Panel
// ────────────────────────────────────────────────────

void MainWindow::build_corner_panel() {
  curvz::utils::set_name(m_corner_panel, "pop_corner", "corner_panel_popover_root");
  // Radius entry — accepts expressions like "8px", "0.125in", "3px + 2mm"
  m_corner_radius_adj = Gtk::Adjustment::create(0.1, 0.0, 9999.0, 0.01, 0.1);
  m_corner_radius_spin.set_adjustment(m_corner_radius_adj);
  m_corner_radius_spin.set_digits(4);
  m_corner_radius_spin.set_numeric(false);
  m_corner_radius_spin.set_width_chars(10);
  m_corner_radius_spin.set_value(0.1);
  curvz::utils::set_name(m_corner_radius_spin, "pop_corner_rad", "corner_panel_radius_spn");
  
  

  // Helper: parse entry text → doc px radius
  auto get_radius = [this]() -> double {
    std::string txt = m_corner_radius_spin.get_text();
    auto *doc = m_project ? m_project->active_doc() : nullptr;
    Unit default_unit = Unit::Px;
    if (doc) {
      if (doc->canvas.display_mode == DisplayMode::Physical)
        default_unit = UnitSystem::parse_unit(doc->canvas.phys_unit);
      else
        default_unit = doc->canvas.display_unit;
    }
    // parse_expr returns screen px (96dpi basis)
    double screen_px = 8.0;
    if (!UnitSystem::parse_expr(txt, default_unit, screen_px)) {
      try { screen_px = UnitSystem::to_px(std::stod(txt), default_unit); }
      catch (...) { screen_px = 8.0; }
    }
    // Convert screen px → doc units
    if (doc && doc->canvas.display_mode == DisplayMode::Physical) {
      double phys_short = std::min(doc->canvas.phys_width, doc->canvas.phys_height);
      if (phys_short > 0 && doc->canvas.quality > 0)
        return (screen_px / 96.0) / phys_short * doc->canvas.quality;
    }
    return screen_px; // Pixel mode: screen px == doc px
  };

  // Enter key in entry → apply
  auto kc = Gtk::EventControllerKey::create();
  kc->signal_key_pressed().connect([this, get_radius](guint keyval, guint, Gdk::ModifierType) -> bool {
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
      CornerType type = CornerType::Round;
      if (m_corner_btn_chamfer.get_active()) type = CornerType::Chamfer;
      if (m_corner_btn_inverse.get_active()) type = CornerType::InverseRound;
      m_canvas.apply_corner_treatment_op(type, get_radius());
      return true;
    }
    return false;
  }, false);
  m_corner_radius_spin.add_controller(kc);

  // Type buttons — radio group via set_group
  m_corner_btn_round.set_label("Round");
  m_corner_btn_chamfer.set_label("Chamfer");
  m_corner_btn_inverse.set_label("Inverse");
  curvz::utils::set_name(m_corner_btn_round, "pop_corner_round", "corner_panel_round_toggle");
  curvz::utils::set_name(m_corner_btn_chamfer, "pop_corner_chamfer", "corner_panel_chamfer_toggle");
  curvz::utils::set_name(m_corner_btn_inverse, "pop_corner_inverse", "corner_panel_inverse_toggle");
  m_corner_btn_chamfer.set_group(m_corner_btn_round);
  m_corner_btn_inverse.set_group(m_corner_btn_round);
  m_corner_btn_round.set_active(true);

  // Style each button; CSS :checked handles active highlight
  for (auto* btn : {&m_corner_btn_round, &m_corner_btn_chamfer, &m_corner_btn_inverse})
    (void)btn; // no custom CSS — use GTK default ToggleButton appearance

  // Update unit label whenever popover opens (picks up current doc unit live)
  m_corner_panel.signal_show().connect([this]() {
    auto *doc = m_project ? m_project->active_doc() : nullptr;
    if (doc) {
      Unit u = (doc->canvas.display_mode == DisplayMode::Physical)
               ? UnitSystem::parse_unit(doc->canvas.phys_unit)
               : doc->canvas.display_unit;
      m_corner_unit_label.set_text(std::string("Units: ") + UnitSystem::label(u));
      m_corner_radius_spin.set_value(0.1);
    }
  });

  m_corner_type_row.set_spacing(6);
  m_corner_type_row.set_homogeneous(true);
  m_corner_btn_round.set_hexpand(true);
  m_corner_btn_chamfer.set_hexpand(true);
  m_corner_btn_inverse.set_hexpand(true);
  m_corner_type_row.append(m_corner_btn_round);
  m_corner_type_row.append(m_corner_btn_chamfer);
  m_corner_type_row.append(m_corner_btn_inverse);

  m_corner_radius_label.set_text("Radius:");
  m_corner_radius_label.set_margin_end(4);
  m_corner_radius_row.set_spacing(4);
  m_corner_radius_row.append(m_corner_radius_label);
  m_corner_radius_row.append(m_corner_radius_spin);

  m_corner_apply_btn.set_label("Apply");
  curvz::utils::set_name(m_corner_apply_btn, "pop_corner_app", "corner_panel_apply_btn");
  m_corner_apply_btn.signal_clicked().connect([this, get_radius]() {
    CornerType type = CornerType::Round;
    if (m_corner_btn_chamfer.get_active())
      type = CornerType::Chamfer;
    if (m_corner_btn_inverse.get_active())
      type = CornerType::InverseRound;
    m_canvas.apply_corner_treatment_op(type, get_radius());
  });

  m_corner_unit_label.set_text("Units: —");
  m_corner_unit_label.set_xalign(0.0f);
  m_corner_unit_label.add_css_class("pop-unit-label");
  curvz::utils::set_name(m_corner_unit_label, "pop_corner_unit", "corner_panel_unit_lbl");

  m_corner_panel_vbox.set_spacing(6);
  m_corner_panel_vbox.set_margin_top(8);
  m_corner_panel_vbox.set_margin_bottom(8);
  m_corner_panel_vbox.set_margin_start(8);
  m_corner_panel_vbox.set_margin_end(8);
  m_corner_panel_vbox.append(m_corner_type_row);
  m_corner_panel_vbox.append(m_corner_radius_row);
  m_corner_panel_vbox.append(m_corner_unit_label);
  m_corner_panel_vbox.append(m_corner_apply_btn);

  // Anchor popover to the corner tool button in the toolbar
  m_corner_panel.set_child(m_corner_panel_vbox);
  m_corner_panel.set_position(Gtk::PositionType::RIGHT);
  m_corner_panel.set_has_arrow(true);
  if (auto* btn = m_toolbar.get_corner_btn())
    m_corner_panel.set_parent(*btn);
}

void MainWindow::show_corner_panel(bool show) {
  if (show)
    m_corner_panel.popup();
  else
    m_corner_panel.popdown();
}

void MainWindow::update_corner_panel_position() {
  // No-op for now — position managed by alignment + margins
}

// ── on_request_save_selection_to_library
// s125 m1a: handler for Canvas::signal_request_save_to_library, fired
// from the right-click "Save to Library…" entry on a path/text/group.
// Opens a folder-picker pre-pointed at ~/.config/curvz/library, then
// routes the chosen directory to on_save_selection_to_library — which
// is the same destination the LibraryPanel + button takes via its own
// signal. Two callers, parallel picker setup; if a third caller appears,
// extract a shared helper.

void MainWindow::on_request_save_selection_to_library() {
  // Bail early if there's no selection — the canvas right-click branch
  // only shows the menu when something was hit, but the selection could
  // change via keyboard between popup and click in edge cases. Match
  // on_save_selection_to_library's empty-selection alert for symmetry.
  if (m_canvas.selection().empty()) {
    curvz::utils::show_alert(
        *this, "Nothing selected",
        "Select one or more objects before saving to the library.");
    return;
  }

  // Ensure user library dir exists before opening the picker, so the
  // picker has somewhere to land. Mirrors LibraryPanel::on_add_clicked.
  std::string lib_dir = Glib::get_user_config_dir() + "/curvz/library";
  std::error_code ec;
  fs::create_directories(lib_dir, ec);

  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Choose library category folder");

  try {
    auto initial = Gio::File::create_for_path(lib_dir);
    dialog->set_initial_folder(initial);
  } catch (...) {
  }

  dialog->select_folder(
      *this, [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
        try {
          auto folder = dialog->select_folder_finish(result);
          if (!folder) return;
          std::string dest_dir = folder->get_path();
          on_save_selection_to_library(dest_dir);
        } catch (...) {
          // User cancelled — do nothing
        }
      });
}

// ── on_save_selection_to_library
// Builds a scratch CurvzDocument from the current selection, serialises it via
// optimise_svg, and writes to dest_dir/{name}.svg.  The filename comes from
// the document filename stem; a numeric suffix is added if it already exists.

void MainWindow::on_save_selection_to_library(const std::string &dest_dir) {
  if (!m_project)
    return;
  auto *doc = m_project->active_doc();
  if (!doc)
    return;

  const auto &sel = m_canvas.selection();
  if (sel.empty()) {
    // Inform user — no selection
    curvz::utils::show_alert(
        *this, "Nothing selected",
        "Select one or more objects before adding to the library.");
    return;
  }

  // ── Build scratch document ────────────────────────────────────────────
  CurvzDocument scratch;
  scratch.canvas = doc->canvas; // preserve viewBox dimensions

  auto layer = std::make_unique<SceneNode>();
  layer->type = SceneNode::Type::Layer;
  layer->internal_id = generate_internal_id();
  layer->name = scratch.next_default_name(CurvzDocument::NameKind::Layer);
  layer->visible = true;
  SceneNode *tgt = layer.get();
  scratch.layers.push_back(std::move(layer));
  scratch.active_layer_index = 0;

  // Compute bounding box of selection to re-centre in a tight canvas
  double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
  for (SceneNode *obj : sel) {
    // Walk the node to find path bboxes
    std::function<void(const SceneNode &)> collect_bb =
        [&](const SceneNode &n) {
          if (n.path) {
            for (const auto &nd : n.path->nodes) {
              bx1 = std::min(bx1, nd.x);
              bx2 = std::max(bx2, nd.x);
              by1 = std::min(by1, nd.y);
              by2 = std::max(by2, nd.y);
            }
          }
          for (const auto &c : n.children)
            collect_bb(*c);
        };
    collect_bb(*obj);
  }
  // Clamp if no geometry found
  if (bx1 > bx2) {
    bx1 = 0;
    by1 = 0;
    bx2 = 48;
    by2 = 48;
  }

  // Pad slightly and set canvas to tight bbox
  double pad = 2.0;
  double cw = (bx2 - bx1) + pad * 2.0;
  double ch = (by2 - by1) + pad * 2.0;
  scratch.canvas = CanvasModel::from_pixels(std::max(1, (int)std::round(cw)),
                                            std::max(1, (int)std::round(ch)));

  // Clone selected objects, translate so bbox is at (pad, pad) in doc space
  double tx = pad - bx1;
  double ty = pad - by1;
  for (SceneNode *obj : sel) {
    auto node = clone_node(*obj);
    // Translate all path nodes
    std::function<void(SceneNode &)> translate = [&](SceneNode &n) {
      if (n.path) {
        for (auto &nd : n.path->nodes) {
          nd.x += tx;
          nd.y += ty;
          nd.cx1 += tx;
          nd.cy1 += ty;
          nd.cx2 += tx;
          nd.cy2 += ty;
        }
      }
      if (n.type == SceneNode::Type::Text) {
        n.text_x += tx;
        n.text_y += ty;
      }
      for (auto &c : n.children)
        translate(*c);
    };
    translate(*node);
    tgt->children.push_back(std::move(node));
  }

  // ── Serialise via optimise_svg ────────────────────────────────────────
  std::string svg = optimise_svg(scratch);

  // ── Derive filename ───────────────────────────────────────────────────
  namespace fs = std::filesystem;
  std::string stem = fs::path(doc->filename).stem().string();
  if (stem.empty())
    stem = "shape";

  // Ensure dest_dir exists
  std::error_code ec;
  fs::create_directories(dest_dir, ec);

  // Avoid clobbering existing files — append numeric suffix
  std::string dest = dest_dir + "/" + stem + ".svg";
  int suffix = 2;
  while (fs::exists(dest, ec))
    dest = dest_dir + "/" + stem + std::to_string(suffix++) + ".svg";

  // Write
  std::ofstream f(dest);
  if (!f) {
    LOG_ERROR("on_save_selection_to_library: cannot write '{}'", dest);
    curvz::utils::show_alert(*this, "Save failed",
                             "Could not write to:\n" + dest);
    return;
  }
  f << svg;
  f.close();

  LOG_INFO("on_save_selection_to_library: saved {} object(s) to '{}'",
           sel.size(), dest);

  // Refresh panel so new item appears immediately
  m_library.refresh();
}

// ── on_step_repeat
// ────────────────────────────────────────────────────────────
void MainWindow::on_step_repeat() {
  if (!m_project)
    return;
  if (m_canvas.selection().empty())
    return;
  auto *doc = m_project->active_doc();
  const CanvasModel *cm = doc ? &doc->canvas : nullptr;

  // Seed pivot = selection bbox center. Used by the dialog for the default
  // pivot AND for the mini preview's bbox rectangle scale.
  double bx = 0.0, by = 0.0, bw = 0.0, bh = 0.0;
  double cx = 0.0, cy = 0.0;
  if (!m_canvas.selection_bbox(bx, by, bw, bh)) {
    // Fallback: no usable bbox. Use doc origin as pivot, 100x100 preview.
    cx = 0.0; cy = 0.0; bw = 100.0; bh = 100.0;
  } else {
    cx = bx + bw * 0.5;
    cy = by + bh * 0.5;
  }

  // Pivot change: dialog → canvas.  Live crosshair while dialog is open.
  // NOTE: do NOT null the canvas→dialog callback here on visible=false.
  // visible can flip false/true mid-session (e.g. user toggles rotate off
  // then on).  The callback is cleared in apply_cb and in set_modal(false)
  // → signal_hide, which fire only when the dialog actually closes.
  auto pivot_cb = [this](double px, double py, bool visible) {
    m_canvas.set_step_repeat_preview(visible, px, py);
  };

  // Pivot change: canvas → dialog.  User drags the crosshair on canvas →
  // dialog spins/preview track live.
  m_canvas.set_step_repeat_pivot_callback(
      [this](double px, double py) {
          m_step_repeat_dialog.set_pivot_from_canvas(px, py);
      });

  // Apply: invoke extended step_repeat. Y-flip on offset matches the
  // existing sign convention (handoff: dialog Y screen-down → canvas Y-up).
  // Pivot Y is passed as-is because PositionY csb already returns doc-Y-down
  // internal, and that is exactly what the new step_repeat overload expects.
  auto apply_cb = [this](StepRepeatDialog::Result r) {
    m_canvas.set_step_repeat_preview(false, 0.0, 0.0);
    m_canvas.set_step_repeat_pivot_callback(nullptr);
    m_canvas.step_repeat(r.copies, r.dx, -r.dy,
                         r.rotate_enabled, r.angle_deg,
                         r.pivot_x, r.pivot_y);
  };

  m_step_repeat_dialog.show(*this, cm, cx, cy, bw, bh,
                            std::move(pivot_cb), std::move(apply_cb));
}

// ── on_blend ───────────────────────────────────────────────────────────────
// M3 Blend orchestrator. Validates the selection (exactly 2 Path nodes),
// reads node counts + current stroke widths, opens BlendDialog with those
// as seed values, and on OK forwards the dialog Result to Canvas::make_blend.
// Deep preconditions (same parent, matching closed flag) are checked by
// Canvas::make_blend itself — if they fail it emits a user-visible error
// via m_sig_show_message.
void MainWindow::on_blend() {
  if (!m_project) return;
  const auto &sel = m_canvas.selection();
  if (sel.size() != 2 ||
      !sel[0] || sel[0]->type != SceneNode::Type::Path || !sel[0]->path ||
      !sel[1] || sel[1]->type != SceneNode::Type::Path || !sel[1]->path) {
    // Action sensitivity should have prevented this, but be defensive.
    LOG_WARN("MainWindow::on_blend: selection not 2 Paths — ignoring");
    return;
  }

  auto *doc = m_project->active_doc();
  const CanvasModel *cm = doc ? &doc->canvas : nullptr;

  int a_count = (int)sel[0]->path->nodes.size();
  int b_count = (int)sel[1]->path->nodes.size();
  double a_w = sel[0]->stroke.width;
  double b_w = sel[1]->stroke.width;

  auto apply_cb = [this](BlendDialog::Result r) {
    m_canvas.make_blend(r.steps, r.reverse, r.stroke_w_override,
                        r.stroke_w_start, r.stroke_w_end);
  };

  // Equalize callback — runs on the current 2-path selection. Returns the
  // new matched node count on success, 0 on failure (which leaves the
  // dialog's warning banner up). After successful equalization the dialog
  // clears its warning state and enables OK.
  auto equalize_cb = [this]() -> int {
    const auto &s = m_canvas.selection();
    if (s.size() != 2 || !s[0] || !s[1] ||
        !s[0]->path || !s[1]->path)
      return 0;
    if (!m_canvas.equalize_blend_sources(s[0], s[1]))
      return 0;
    // Both sides are now the same count; either side will do.
    int n = (int)s[0]->path->nodes.size();
    // Redraw so the new nodes appear on canvas immediately.
    m_canvas.queue_draw();
    m_layers.refresh();
    return n;
  };

  m_blend_dialog.show(*this, cm, a_count, b_count, a_w, b_w,
                      std::move(apply_cb), std::move(equalize_cb));
}

// ── on_warp_make ────────────────────────────────────────────────────────────
// Orchestrates: single-selection validation → build a scratch Warp in
// the tree without pushing a command → show WarpDialog with live
// preview callbacks → on Apply, push MakeWarpCommand to make the
// mutation undoable → on Cancel, rip the scratch out and restore the
// original source.
//
// The scratch Warp is a real node in the tree during the dialog
// session, so the canvas shows live warp updates as the user tweaks
// controls. Without this the user would stare at an unchanged canvas
// until Apply.
void MainWindow::on_warp_make() {
  if (!m_project || !m_project->active_doc()) return;
  CurvzDocument *doc = m_project->active_doc();

  const auto &sel = m_canvas.selection();
  if (sel.size() != 1 || !sel[0]) {
    LOG_WARN("MainWindow::on_warp_make: selection not size-1, ignoring");
    return;
  }
  SceneNode *src = sel[0];
  if (src->type != SceneNode::Type::Path &&
      src->type != SceneNode::Type::Compound &&
      src->type != SceneNode::Type::Group) {
    LOG_WARN("MainWindow::on_warp_make: selection type not warpable");
    return;
  }

  // Find the source's parent and its index. We'll need both for the
  // scratch tree mutation and for the eventual MakeWarpCommand.
  int src_idx = -1;
  SceneNode *parent = nullptr;
  {
    // Walk the doc to find src's parent. Same logic find_parent uses in
    // Canvas — but MainWindow doesn't have direct access, so we mirror
    // it inline. Could factor to a helper later.
    std::function<bool(SceneNode*)> walk = [&](SceneNode *n) -> bool {
      if (!n) return false;
      for (int i = 0; i < (int)n->children.size(); ++i) {
        if (n->children[i].get() == src) {
          parent = n; src_idx = i; return true;
        }
        if (walk(n->children[i].get())) return true;
      }
      if (n->is_blend()) {
        if (walk(n->blend_source_a.get())) return true;
        if (walk(n->blend_source_b.get())) return true;
      }
      if (n->is_warp()) {
        if (walk(n->warp_source.get())) return true;
        if (walk(n->warp_glyph_cache.get())) return true;
        if (walk(n->warp_cache.get())) return true;
      }
      if (n->clip_shape && walk(n->clip_shape.get())) return true;
      return false;
    };
    for (auto &layer : doc->layers)
      if (walk(layer.get())) break;
  }
  if (!parent || src_idx < 0) {
    LOG_WARN("MainWindow::on_warp_make: could not locate source's parent");
    return;
  }

  // Snapshot the original for the eventual undo restoration.
  auto source_snap = clone_node(*src);
  int source_index = src_idx;

  // Build the scratch Warp. Identical setup to Canvas::make_warp but
  // WITHOUT the m_history->push — dialog commit/cancel controls that.
  auto warp = std::make_unique<SceneNode>();
  warp->type = SceneNode::Type::Warp;
  warp->id = m_canvas.mint_id();
  warp->internal_id = m_canvas.last_minted_iid();
  warp->name = doc->next_default_name(CurvzDocument::NameKind::Warp);
  warp->visible = true;
  warp->locked  = false;
  warp->opacity = 1.0;
  warp->warp_source = clone_node(*src);
  warp->warp_glyph_cache_dirty = true;
  warp->warp_cache_dirty = true;
  warp->warp_quality = 4;

  // Remove original, insert scratch at same index.
  parent->children.erase(parent->children.begin() + src_idx);
  parent->children.insert(parent->children.begin() + src_idx,
                          std::move(warp));
  SceneNode *scratch = parent->children[src_idx].get();

  // Select the scratch so inspector/bbox reflect it.
  m_canvas.set_selection_single(scratch);
  m_canvas.queue_draw();

  // Compute the glyph_cache bbox for the dialog's preset shape math.
  // At this point warp_glyph_cache is still null — rebuild_warp_caches
  // runs lazy-on-draw. Force a rebuild now so the bbox is available.
  m_canvas.rebuild_warp_caches(scratch);

  double gbx = 0, gby = 0, gbw = 1, gbh = 1;
  m_canvas.object_bbox_query(*scratch, gbx, gby, gbw, gbh, false);
  if (gbw < 1e-9) gbw = 1.0;
  if (gbh < 1e-9) gbh = 1.0;

  // Capture parent/scratch/source_snap into the callbacks by value.
  SceneNode *parent_cap = parent;
  SceneNode *scratch_ptr = scratch;
  // Move source_snap into a shared_ptr wrapper so both callbacks can
  // reference it (only one fires per show, but the lambda types need
  // matching captures).
  auto src_snap_shared =
      std::make_shared<std::unique_ptr<SceneNode>>(std::move(source_snap));

  // Update callback: write dialog's envelope/quality to live scratch
  // and redraw.
  auto update_cb = [this, scratch_ptr](WarpDialog::Result r) {
    if (!scratch_ptr) return;
    scratch_ptr->warp_env_top    = std::move(r.env_top);
    scratch_ptr->warp_env_bottom = std::move(r.env_bottom);
    scratch_ptr->warp_quality    = r.quality;
    scratch_ptr->warp_cache_dirty = true;
    m_canvas.queue_draw();
  };

  // Apply callback: scratch is already in final state (last update_cb
  // has been applied). Push MakeWarpCommand with warp_snap cloned from
  // the current scratch state, source_snap from the pre-mutation
  // snapshot.
  auto apply_cb = [this, parent_cap, scratch_ptr, src_snap_shared,
                   source_index](WarpDialog::Result r) {
    if (!scratch_ptr || !parent_cap) return;
    // Write final state once more in case update_cb didn't fire.
    scratch_ptr->warp_env_top    = std::move(r.env_top);
    scratch_ptr->warp_env_bottom = std::move(r.env_bottom);
    scratch_ptr->warp_quality    = r.quality;
    scratch_ptr->warp_cache_dirty = true;
    // Find the scratch's current index (should still be source_index
    // but may have shifted if anything weird happened).
    int ins = -1;
    for (int i = 0; i < (int)parent_cap->children.size(); ++i) {
      if (parent_cap->children[i].get() == scratch_ptr) { ins = i; break; }
    }
    if (ins < 0) {
      LOG_WARN("MainWindow::on_warp_make apply: scratch not in parent");
      return;
    }
    auto warp_snap = clone_node(*scratch_ptr);
    if (auto *hist = m_canvas.history()) {
      hist->push(std::make_unique<MakeWarpCommand>(
          parent_cap, clone_node(**src_snap_shared), source_index,
          std::move(warp_snap), ins));
    }
    m_canvas.queue_draw();
    refresh_inspector();
  };

  // Cancel callback: tear scratch out, restore original source at its
  // original index. No command pushed — the whole session is as if it
  // never happened.
  auto cancel_cb = [this, parent_cap, scratch_ptr, src_snap_shared,
                    source_index]() {
    if (!parent_cap) return;
    int idx = -1;
    for (int i = 0; i < (int)parent_cap->children.size(); ++i) {
      if (parent_cap->children[i].get() == scratch_ptr) { idx = i; break; }
    }
    if (idx >= 0) {
      parent_cap->children.erase(parent_cap->children.begin() + idx);
    }
    int ins = std::clamp(source_index, 0,
                         (int)parent_cap->children.size());
    parent_cap->children.insert(parent_cap->children.begin() + ins,
                                clone_node(**src_snap_shared));
    SceneNode *restored = parent_cap->children[ins].get();
    m_canvas.set_selection_single(restored);
    m_canvas.queue_draw();
    refresh_inspector();
  };

  m_warp_dialog.show(*this, scratch, gbx, gby, gbw, gbh,
                     std::move(update_cb),
                     std::move(apply_cb),
                     std::move(cancel_cb));
}

// ── on_warp_edit ────────────────────────────────────────────────────────────
// Opens the dialog against an already-committed Warp. Snapshots the
// pre-edit envelope + quality so Cancel can restore; on Apply, pushes
// EditWarpCommand carrying pre/post snapshots for atomic undo.
void MainWindow::on_warp_edit() {
  SceneNode *warp = m_canvas.selected_object();
  if (!warp || !warp->is_warp()) {
    LOG_WARN("MainWindow::on_warp_edit: selection is not a Warp");
    return;
  }

  // Ensure glyph_cache is current so bbox math is meaningful.
  m_canvas.rebuild_warp_caches(warp);

  double gbx = 0, gby = 0, gbw = 1, gbh = 1;
  m_canvas.object_bbox_query(*warp, gbx, gby, gbw, gbh, false);
  if (gbw < 1e-9) gbw = 1.0;
  if (gbh < 1e-9) gbh = 1.0;

  // Snapshot pre-state for undo and for cancel-revert.
  auto pre_top    = std::make_shared<PathData>(warp->warp_env_top);
  auto pre_bottom = std::make_shared<PathData>(warp->warp_env_bottom);
  int pre_quality = warp->warp_quality;
  SceneNode *warp_ptr = warp;

  auto update_cb = [this, warp_ptr](WarpDialog::Result r) {
    if (!warp_ptr) return;
    warp_ptr->warp_env_top    = std::move(r.env_top);
    warp_ptr->warp_env_bottom = std::move(r.env_bottom);
    warp_ptr->warp_quality    = r.quality;
    warp_ptr->warp_cache_dirty = true;
    m_canvas.queue_draw();
  };

  auto apply_cb = [this, warp_ptr, pre_top, pre_bottom,
                   pre_quality](WarpDialog::Result r) {
    if (!warp_ptr) return;
    // Live state already reflects r via the last update_cb. Push edit
    // command with pre/post snapshots.
    warp_ptr->warp_env_top    = std::move(r.env_top);
    warp_ptr->warp_env_bottom = std::move(r.env_bottom);
    warp_ptr->warp_quality    = r.quality;
    warp_ptr->warp_cache_dirty = true;
    if (auto *hist = m_canvas.history()) {
      hist->push(std::make_unique<EditWarpCommand>(
          warp_ptr,
          *pre_top, *pre_bottom, pre_quality,
          warp_ptr->warp_env_top,
          warp_ptr->warp_env_bottom,
          warp_ptr->warp_quality));
    }
    m_canvas.queue_draw();
    refresh_inspector();
  };

  auto cancel_cb = [this, warp_ptr, pre_top, pre_bottom, pre_quality]() {
    if (!warp_ptr) return;
    warp_ptr->warp_env_top    = *pre_top;
    warp_ptr->warp_env_bottom = *pre_bottom;
    warp_ptr->warp_quality    = pre_quality;
    warp_ptr->warp_cache_dirty = true;
    m_canvas.queue_draw();
    refresh_inspector();
  };

  m_warp_dialog.show(*this, warp, gbx, gby, gbw, gbh,
                     std::move(update_cb),
                     std::move(apply_cb),
                     std::move(cancel_cb));
}

// ── on_warp_release ─────────────────────────────────────────────────────────
// Delegates to Canvas::release_warp. No dialog needed — the action is
// a straightforward tree mutation with atomic undo.
void MainWindow::on_warp_release() {
  m_canvas.release_warp();
}

// ── on_warp_flatten ─────────────────────────────────────────────────────────
// Delegates to Canvas::flatten_warp. Replaces the Warp with its baked
// warped geometry; envelope is lost. Atomic undo.
void MainWindow::on_warp_flatten() {
  m_canvas.flatten_warp();
}

// ── open_guide_review_dialog ───────────────────────────────────────────────
// Tiny non-modal window: Perpendicular checkbox + Cancel + OK.  Created
// lazily and reused across multiple guide-construct sessions.  P-key on the
// dialog toggles the checkbox; Enter commits; Esc cancels.
void MainWindow::open_guide_review_dialog() {
  if (!m_guide_review_win) {
    m_guide_review_win = std::make_unique<Gtk::Window>();
    m_guide_review_win->set_title("New Guide");
    m_guide_review_win->set_resizable(false);
    m_guide_review_win->set_hide_on_close(true);
    m_guide_review_win->set_default_size(260, -1);
    m_guide_review_win->set_transient_for(*this);
    curvz::utils::apply_motif_class_from_parent(*m_guide_review_win, *this);  // s117 m18 v2

    auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    outer->set_spacing(0);

    auto *body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    body->set_spacing(8);
    body->set_margin(14);

    auto *info = Gtk::make_managed<Gtk::Label>(
        "Click two points on the canvas.\nP flips to perpendicular.");
    info->set_xalign(0.0f);
    info->add_css_class("dim-label");
    body->append(*info);

    m_guide_review_perp_chk =
        Gtk::make_managed<Gtk::CheckButton>("Perpendicular (through midpoint)");
    m_guide_review_perp_chk->signal_toggled().connect([this]() {
      m_canvas.set_guide_construct_perpendicular(
          m_guide_review_perp_chk->get_active());
    });
    body->append(*m_guide_review_perp_chk);

    outer->append(*body);

    auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    outer->append(*sep);

    auto *btn_bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    btn_bar->set_spacing(8);
    btn_bar->set_margin(10);
    btn_bar->set_halign(Gtk::Align::END);

    auto *cancel_btn = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel_btn->signal_clicked().connect([this]() {
      m_canvas.cancel_guide_construct();
      close_guide_review_dialog();
    });
    btn_bar->append(*cancel_btn);

    auto *ok_btn = Gtk::make_managed<Gtk::Button>("Create");
    ok_btn->add_css_class("suggested-action");
    ok_btn->signal_clicked().connect([this]() {
      m_canvas.commit_guide_construct();
      close_guide_review_dialog();
    });
    btn_bar->append(*ok_btn);

    outer->append(*btn_bar);
    m_guide_review_win->set_child(*outer);

    // Keyboard: Enter → OK, Esc → cancel, P → toggle perpendicular.
    auto key = Gtk::EventControllerKey::create();
    key->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) -> bool {
          if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            m_canvas.commit_guide_construct();
            close_guide_review_dialog();
            return true;
          }
          if (keyval == GDK_KEY_Escape) {
            m_canvas.cancel_guide_construct();
            close_guide_review_dialog();
            return true;
          }
          if (keyval == GDK_KEY_p || keyval == GDK_KEY_P) {
            if (m_guide_review_perp_chk)
              m_guide_review_perp_chk->set_active(
                  !m_guide_review_perp_chk->get_active());
            return true;
          }
          return false;
        },
        true);
    m_guide_review_win->add_controller(key);

    // WM close button → cancel (matches Esc).
    m_guide_review_win->signal_close_request().connect(
        [this]() -> bool {
          if (m_canvas.guide_construct_active())
            m_canvas.cancel_guide_construct();
          return false; // allow default hide
        },
        false);
  }

  // Reset widgets per-show.
  if (m_guide_review_perp_chk)
    m_guide_review_perp_chk->set_active(false);

  m_guide_review_win->set_modal(false);
  m_guide_review_win->present();
}

void MainWindow::close_guide_review_dialog() {
  if (m_guide_review_win) m_guide_review_win->hide();
}

// ── on_place_image
// ────────────────────────────────────────────────────────────
void MainWindow::on_place_image() {
  if (!m_project)
    return;

  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Place Image");

  auto filter = Gtk::FileFilter::create();
  filter->set_name("Image files");
  filter->add_mime_type("image/png");
  filter->add_mime_type("image/jpeg");
  filter->add_mime_type("image/gif");
  filter->add_mime_type("image/webp");
  filter->add_pattern("*.png");
  filter->add_pattern("*.jpg");
  filter->add_pattern("*.jpeg");
  filter->add_pattern("*.gif");
  filter->add_pattern("*.webp");
  auto filters = Gio::ListStore<Gtk::FileFilter>::create();
  filters->append(filter);
  dialog->set_filters(filters);

  // s125 m1e: re-open at the last-used folder for this purpose.
  std::string remembered = get_last_folder("place-image");
  if (!remembered.empty() && fs::is_directory(remembered)) {
    try {
      dialog->set_initial_folder(Gio::File::create_for_path(remembered));
    } catch (...) {}
  }

  dialog->open(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
    try {
      auto file = dialog->open_finish(result);
      if (!file)
        return;
      std::string chosen = file->get_path();
      set_last_folder("place-image",
                      fs::path(chosen).parent_path().string());
      m_canvas.import_image_to_canvas(chosen,
                                      /*fit_canvas_to_image=*/false);
    } catch (...) {
    }
  });
}

// ── on_open_image
// ──────────────────────────────────────────────────── s125 m1d (was m1c
// on_place_image_as_doc, renamed). Variant of on_place_image that resizes
// the document canvas to the image's pixel dimensions and places it at
// (0, 0) at 1:1. Picker config is duplicated from on_place_image; if a
// third image-picker caller appears, lift the filter setup into a helper.
void MainWindow::on_open_image() {
  if (!m_project)
    return;

  auto dialog = Gtk::FileDialog::create();
  dialog->set_title("Open Image");

  auto filter = Gtk::FileFilter::create();
  filter->set_name("Image files");
  filter->add_mime_type("image/png");
  filter->add_mime_type("image/jpeg");
  filter->add_mime_type("image/gif");
  filter->add_mime_type("image/webp");
  filter->add_pattern("*.png");
  filter->add_pattern("*.jpg");
  filter->add_pattern("*.jpeg");
  filter->add_pattern("*.gif");
  filter->add_pattern("*.webp");
  auto filters = Gio::ListStore<Gtk::FileFilter>::create();
  filters->append(filter);
  dialog->set_filters(filters);

  // s125 m1e: re-open at the last-used folder for this purpose. Note this
  // is keyed separately from "place-image" — Open Image and Place Image
  // are typically used from different directories (screenshots vs. asset
  // library), so cross-pollution isn't desirable.
  std::string remembered = get_last_folder("open-image");
  if (!remembered.empty() && fs::is_directory(remembered)) {
    try {
      dialog->set_initial_folder(Gio::File::create_for_path(remembered));
    } catch (...) {}
  }

  dialog->open(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult> &result) {
    try {
      auto file = dialog->open_finish(result);
      if (!file)
        return;
      std::string chosen = file->get_path();
      set_last_folder("open-image",
                      fs::path(chosen).parent_path().string());

      // s125 m1f: Open Image creates a new document in the project named
      // after the image's filename stem, then routes the import into it.
      // Earlier versions imported into the active doc, which conflated
      // "drop reference into existing doc" (Place) with "open as document"
      // (Open). Pattern mirrors import_svg_as_doc / signal_add_doc:
      //   1. Build minimal CurvzDocument (filename + guide layer +
      //      default user layer)
      //   2. Append + flip active_doc_index
      //   3. update_all_panels() so m_canvas.m_doc points at the new doc
      //   4. import_image_to_canvas(fit_canvas=true) does the canvas
      //      resize + image placement on the now-active new doc
      auto new_doc = std::make_unique<CurvzDocument>();

      std::string stem = fs::path(chosen).stem().string();
      if (stem.empty())
        stem = "Image";
      // Collision-resistant filename — matches import_svg_as_doc,
      // signal_add_doc, etc. The .svg extension is the project's
      // canonical doc format, regardless of the source raster type.
      std::string fname = stem + ".svg";
      int suffix = 2;
      while (std::any_of(
          m_project->documents.begin(), m_project->documents.end(),
          [&](const auto &d) { return d->filename == fname; }))
        fname = stem + std::to_string(suffix++) + ".svg";
      new_doc->filename = fname;

      new_doc->ensure_guide_layer();
      auto layer = std::make_unique<SceneNode>();
      layer->type = SceneNode::Type::Layer;
      layer->internal_id = generate_internal_id();
      layer->name =
          new_doc->next_default_name(CurvzDocument::NameKind::Layer);
      layer->visible = true;
      new_doc->layers.insert(new_doc->layers.begin(), std::move(layer));
      new_doc->active_layer_index = 0;

      m_project->documents.push_back(std::move(new_doc));
      m_project->active_doc_index =
          (int)m_project->documents.size() - 1;

      // Switch m_canvas.m_doc to the new doc before import — import_
      // image_to_canvas mutates m_canvas's current doc, so the order
      // here is load-bearing.
      update_all_panels();

      // Now do the canvas resize + image placement on the new doc.
      m_canvas.import_image_to_canvas(chosen,
                                      /*fit_canvas_to_image=*/true);

      // Persist. import_image_to_canvas emits signal_doc_changed which
      // schedules a save, but a direct save here is faster and matches
      // the import_svg_as_doc / signal_add_doc pattern.
      m_project->save();
      LOG_INFO("on_open_image: opened '{}' as new doc '{}'", chosen, fname);
    } catch (...) {
    }
  });
}

// ── on_macro_manager
// ──────────────────────────────────────────────────────────
void MainWindow::on_macro_manager() {
  // Wire signals on first show (idempotent — signals are connected once)
  static bool signals_connected = false;
  if (!signals_connected) {
    m_macro_manager.signal_run_macro().connect(
        sigc::mem_fun(*this, &MainWindow::on_run_macro));
    m_macro_manager.signal_edit_macro().connect(
        [this](const std::string &macro_id) {
          m_macro_editor.load_macro(macro_id);
          m_macro_editor.show(*this);
        });
    m_macro_editor.signal_run_from().connect([this](const std::string &macro_id,
                                                    int step_idx) {
      LOG_INFO("MainWindow: run macro '{}' from step {}", macro_id, step_idx);
      m_canvas.run_macro(macro_id, step_idx);
    });
    signals_connected = true;
  }
  m_macro_manager.show(*this);
}

// ── on_run_macro
// ──────────────────────────────────────────────────────────────
void MainWindow::on_run_macro(const std::string &macro_id) {
  if (macro_id.empty()) {
    on_macro_manager();
    return;
  }
  Macro *m = MacroManager::instance().find_macro(macro_id);
  if (!m) {
    LOG_WARN("MainWindow: macro {} not found", macro_id);
    return;
  }
  if (m->steps.empty()) {
    LOG_INFO("MainWindow: macro '{}' has no steps — opening manager", m->name);
    on_macro_manager();
    return;
  }
  LOG_INFO("MainWindow: run macro '{}' ({} steps)", m->name, m->steps.size());
  m_canvas.run_macro(macro_id);
}

} // namespace Curvz
