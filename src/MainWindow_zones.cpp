#include "MainWindow.hpp"
#include "AppPreferences.hpp" // s139 m2 / s143 m1 — boolean-cleanup quality pref + sync
#include "ContextBar.hpp"
#include "CoordSpace.hpp"
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"
#include "DocTabBar.hpp"
#include "RecentProjects.hpp" // s144 m3 — Open Recent submenu
#include "Ruler.hpp"
#include "SvgOptimiser.hpp"
#include "SvgParser.hpp"
#include "SvgWriter.hpp"
#include "TemplateLibrary.hpp"
#include "ThemeEditDialog.hpp" // s183 m2 — Edit-theme dialog wiring
#include "curvz/widgets/ToggleButton.hpp" // s190 m2 / s219 m1 — substrate-routed Scripter toggle
#include "scripting/ScripterWindow.hpp" // s190 m2 / s219 m1 — present/get_visible/property_visible
#include <functional>
#include <giomm/simpleactiongroup.h> // s144 m3 — recents action group
#include <gtkmm/application.h>
#include <gtkmm/separator.h>
// s147 m3: ThemesDialog include removed — surface is now ThemesPanel
// (already pulled in via MainWindow.hpp). The dialog source files
// remain in the tree until Scott deletes them on his end; CMake no
// longer references them in this milestone.
#include "UnitSystem.hpp"
#include "curvz_utils.hpp" // s117 m18 v2: apply_motif_class_from_parent
#include "style/StyleInterop.hpp" // mutate_appearance — inspector-driven appearance edits
#include <algorithm>
#include <cctype> // s136 m4: std::isspace for library item name trim
#include <filesystem>
#include <fstream>
#include <giomm/file.h> // s125 m1a: Gio::File::create_for_path (folder picker)
#include <giomm/liststore.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <glibmm/main.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/image.h>      // s117 m19: custom About dialog hero logo
#include <gtkmm/linkbutton.h> // s136 m1: About dialog outbound links
#include "curvz/widgets/Button.hpp"  // s209 m4: unregistered substrate Button (About dialog)
#include <gtkmm/settings.h>
#include <gtkmm/stack.h> // s117 m19: custom About dialog flip animation
#include <gtkmm/stylecontext.h>
#include <nlohmann/json.hpp>

// GDK key definitions
#include <gdk/gdkkeysyms.h>


namespace Curvz {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────
// MainWindow_zones.cpp — named regions of the window: headerbar, menubar, layout, overlays.
// ─────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────
// ━━━ headerbar ━━━ Headerbar construction.
// ─────────────────────────────────────────────────────────────────────

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
    auto *dlg = new Gtk::Window();
    dlg->set_title("About Curvz");
    dlg->set_modal(true);
    dlg->set_resizable(false);
    dlg->set_default_size(440, 500);
    dlg->set_transient_for(*this);
    curvz::utils::apply_motif_class_from_parent(*dlg, *this);
    // Self-managed lifecycle: deleted via close_request handler so a
    // closed-and-reopened sequence doesn't leak.
    dlg->signal_close_request().connect(
        [dlg]() {
          Glib::signal_idle().connect_once([dlg]() { delete dlg; });
          return false; // allow close to proceed
        },
        false);

    // ── Headerbar (matches other Curvz dialogs)
    auto *hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(true);
    dlg->set_titlebar(*hb);

    // ── Stack: page 1 = About, page 2 = Credits, with slide animation
    auto *stack = Gtk::make_managed<Gtk::Stack>();
    stack->set_transition_type(Gtk::StackTransitionType::SLIDE_UP_DOWN);
    stack->set_transition_duration(220); // matches GTK default-ish
    stack->set_vexpand(true);
    stack->set_hexpand(true);

    // ── Page 1: About ──────────────────────────────────────────────
    auto *about_page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    about_page->set_spacing(8);
    about_page->set_margin_top(24);
    about_page->set_margin_bottom(16);
    about_page->set_margin_start(24);
    about_page->set_margin_end(24);
    about_page->set_halign(Gtk::Align::CENTER);

    // Hero logo (96px, symbolic — follows motif via CSS `color`).
    auto *logo = Gtk::make_managed<Gtk::Image>();
    logo->set_from_icon_name("curvz-logo-symbolic");
    logo->set_pixel_size(96);
    logo->set_margin_bottom(8);
    about_page->append(*logo);

    auto *name_lbl = Gtk::make_managed<Gtk::Label>();
    name_lbl->set_markup("<span size='xx-large' weight='bold'>Curvz</span>");
    about_page->append(*name_lbl);

    auto *version_lbl = Gtk::make_managed<Gtk::Label>("Version 0.9");
    version_lbl->add_css_class("dim-label");
    about_page->append(*version_lbl);

    auto *tagline_lbl = Gtk::make_managed<Gtk::Label>(
        "A professional SVG icon editor for Linux.");
    tagline_lbl->set_margin_top(12);
    tagline_lbl->set_wrap(true);
    tagline_lbl->set_justify(Gtk::Justification::CENTER);
    tagline_lbl->set_max_width_chars(40);
    about_page->append(*tagline_lbl);

    auto *desc_lbl = Gtk::make_managed<Gtk::Label>(
        "Built on GTK4 and Cairo. Produces currentColor-aware SVG "
        "icons that adapt to light and dark themes at any resolution.");
    desc_lbl->set_wrap(true);
    desc_lbl->set_justify(Gtk::Justification::CENTER);
    desc_lbl->set_max_width_chars(48);
    desc_lbl->add_css_class("dim-label");
    desc_lbl->set_margin_top(4);
    about_page->append(*desc_lbl);

    auto *copy_lbl = Gtk::make_managed<Gtk::Label>("© 2026 Scott Combs");
    copy_lbl->set_margin_top(16);
    copy_lbl->add_css_class("dim-label");
    about_page->append(*copy_lbl);

    auto *license_lbl = Gtk::make_managed<Gtk::Label>(
        "Released under the GNU General Public License v3.0 or later");
    license_lbl->add_css_class("dim-label");
    license_lbl->set_wrap(true);
    license_lbl->set_justify(Gtk::Justification::CENTER);
    license_lbl->set_max_width_chars(48);
    about_page->append(*license_lbl);

    // s136 m1: outbound links — repo, issues, contact email. Plain
    // Gtk::LinkButton renders the standard underlined-link affordance and
    // dispatches via Gtk::UriLauncher (https → default browser, mailto →
    // default mail client). Margins keep the row compact under the
    // license line; the row sits above the close-button bar.
    auto *links_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    links_row->set_spacing(4);
    links_row->set_halign(Gtk::Align::CENTER);
    links_row->set_margin_top(12);

    auto *repo_btn = Gtk::make_managed<Gtk::LinkButton>(
        "https://github.com/curvz/curvz", "GitHub");
    auto *issues_btn = Gtk::make_managed<Gtk::LinkButton>(
        "https://github.com/curvz/curvz/issues", "Report a bug");
    auto *mail_btn = Gtk::make_managed<Gtk::LinkButton>(
        "mailto:curvz.app@proton.me", "Contact");

    links_row->append(*repo_btn);
    auto *dot1 = Gtk::make_managed<Gtk::Label>("·");
    dot1->add_css_class("dim-label");
    links_row->append(*dot1);
    links_row->append(*issues_btn);
    auto *dot2 = Gtk::make_managed<Gtk::Label>("·");
    dot2->add_css_class("dim-label");
    links_row->append(*dot2);
    links_row->append(*mail_btn);

    about_page->append(*links_row);

    stack->add(*about_page, "about");

    // ── Page 2: Credits ────────────────────────────────────────────
    auto *credits_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    credits_scroll->set_policy(Gtk::PolicyType::NEVER,
                               Gtk::PolicyType::AUTOMATIC);
    credits_scroll->set_vexpand(true);
    auto *credits_page =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    credits_page->set_spacing(4);
    credits_page->set_margin_top(20);
    credits_page->set_margin_bottom(16);
    credits_page->set_margin_start(28);
    credits_page->set_margin_end(28);

    auto add_section = [&](const char *heading,
                           const std::vector<std::string> &items) {
      auto *h = Gtk::make_managed<Gtk::Label>();
      h->set_markup(Glib::ustring("<b>") + heading + "</b>");
      h->set_halign(Gtk::Align::CENTER);
      h->set_margin_top(12);
      h->set_margin_bottom(4);
      credits_page->append(*h);
      for (const auto &it : items) {
        auto *row = Gtk::make_managed<Gtk::Label>(it);
        row->set_halign(Gtk::Align::CENTER);
        row->set_wrap(true);
        row->set_justify(Gtk::Justification::CENTER);
        credits_page->append(*row);
      }
    };

    add_section("Authors", {"Scott Combs — Engineer / Designer",
                            "Claude (Anthropic) — AI Pair Programmer"});
    add_section("Libraries",
                {"nlohmann/json — Niels Lohmann", "spdlog — Gabi Melman",
                 "Cairo / cairomm — Carl Worth et al.",
                 "GTK / gtkmm — The GNOME Project",
                 "FreeType2 — The FreeType Project",
                 "Pango / PangoCairo — The GNOME Project",
                 "Clipper2 — Angus Johnson (Boost Software License 1.0)",
                 "cmark — John MacFarlane (MIT)"});

    credits_scroll->set_child(*credits_page);
    stack->add(*credits_scroll, "credits");

    // ── Bottom bar: Credits/About toggle + Close ───────────────────
    auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    btn_row->set_spacing(8);
    btn_row->set_margin_top(8);
    btn_row->set_margin_bottom(12);
    btn_row->set_margin_start(12);
    btn_row->set_margin_end(12);

    // s209 m4 — substrate Button with the unregistered tag for both
    // About-dialog footer buttons. The dialog is heap-allocated and
    // self-deleting via close-request → idle-delete; open / close /
    // reopen would otherwise put two same-named instances live at
    // once (the prior instance's idle delete hasn't fired when the
    // new construction begins). Pre-substrate these were raw
    // Gtk::Button with no registry interaction; the migration keeps
    // that runtime behaviour exactly while bringing the type into
    // the substrate family. Completes the third costume of the s208
    // m5 audit (per-show heap-allocated dialog), alongside m1-m3's
    // per-loop and per-click coverage.
    auto *btn_flip = Gtk::make_managed<curvz::widgets::Button>(
                          curvz::scripting::unregistered, "Credits");
    auto *btn_close = Gtk::make_managed<curvz::widgets::Button>(
                           curvz::scripting::unregistered, "Close");
    btn_close->add_css_class("suggested-action");

    // Flip button toggles the stack page and updates its own label.
    btn_flip->signal_clicked().connect([stack, btn_flip]() {
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
    auto *spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    btn_row->append(*spacer);
    btn_row->append(*btn_close);

    auto *root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    root->append(*stack);
    root->append(*btn_row);
    dlg->set_child(*root);

    dlg->present();
  });
  m_headerbar.pack_start(m_logo_btn);

  // s190 m2 / s219 m1 — Scripter quick-open toggle ("the monkey button").
  // Sits immediately after the app logo (left edge of the headerbar)
  // so the scripting entry point is the first thing visible alongside
  // the application's primary brand mark. Symbolic icon — face-monkey
  // is stock Adwaita and reads as "diagnostic / playful operator
  // surface" without conflicting with any tool-button icon in the
  // toolbar.
  //
  // s219 m1 — visibility is governed by AppPreferences::scripter_enabled.
  // The button is always constructed and packed into the headerbar so
  // its slot is reserved and its substrate registration is stable, but
  // it's HIDDEN when the pref is off. The initial visibility is left
  // false here and the canonical apply_scripter_pref() call at the end
  // of MainWindow's ctor flips it to the correct state — single source
  // of truth, no risk of an out-of-sync default.
  //
  // Bidirectional sync (s190 m2-rev2 design, unchanged in s219 m1):
  //   toggle.signal_toggled  →  show/hide the Scripter (forward)
  //   scripter.property_visible.signal_changed  →  flip the toggle
  //                                                 (back-channel,
  //                                                 catches X-button)
  // A shared bool flag (`syncing`) suppresses re-entry during
  // programmatic state changes — the back-channel listener flips
  // the toggle, which would re-fire signal_toggled and chase its
  // own tail without the guard.
  //
  // Substrate-routed (curvz::widgets::ToggleButton) so the scriptable
  // registry sees it under the abbrev "mw_scripter". Toggling from a
  // script via `mw_scripter toggle` works the same as a real click.
  //
  // s219 m1 — toggling the button does NOT change the pref; it only
  // shows/hides the window. This is intentional: the headerbar button
  // is a quick-access window toggle (like minimise), not a feature
  // enable/disable. The pref is changed via the menu item or the
  // inspector switch — both of which DO own the pref's state. If the
  // user disables the feature via the menu/inspector, the button
  // disappears from the headerbar entirely (apply_scripter_pref hides
  // it). If they only want to hide the window briefly, they click the
  // button or its X.
  auto* scripter_btn = Gtk::make_managed<curvz::widgets::ToggleButton>(
      "mw_scripter");
  scripter_btn->set_icon_name("face-monkey-symbolic");
  scripter_btn->set_tooltip_text("Scripter (toggle visibility)");
  scripter_btn->set_has_frame(false);
  scripter_btn->add_css_class("mw-scripter-btn");  // for :checked accent
  scripter_btn->set_visible(false);  // canonical state set later by apply_scripter_pref
  curvz::utils::set_name(*scripter_btn, "mw_scripter",
                         "main_window_scripter_open_btn");
  m_scripter_btn = scripter_btn;  // s219 m1 — held for apply_scripter_pref

  // Seed the toggle's checked state from the Scripter's current
  // visibility. Construction order: Application::on_activate creates
  // MainWindow first, then the Scripter — so at the time this lambda
  // runs (during MainWindow construction), the Scripter doesn't exist
  // yet. We defer the initial sync via signal_idle so application
  // setup can finish first.
  //
  // s190 m2-rev4 lesson — both sides of the toggle initially flipped the
  // same property symmetrically (set_visible true/false) to eliminate
  // a mousedown-hide / mouseup-show flicker observed in --diagnostic
  // builds. s219 m1 update: that's still right for the HIDE side, but
  // the SHOW side now needs present() instead of set_visible(true).
  //
  // The reason: hide_on_close() drops the window's window-manager
  // registration when the X button is pressed. The next show has to
  // re-register it — present() does that (it's "realize + show + raise +
  // focus"), set_visible(true) does not. Without present(), the window
  // surface comes back but its titlebar decorations (min/max/close) come
  // up inert because the WM hasn't been told this is a top-level again.
  //
  // The asymmetric pairing also matches apply_scripter_pref()'s shape
  // — that helper has always used present() for show (it's the pref-
  // change path, which is the canonical "make this exist properly"
  // moment). The monkey button is just one more way to reach the same
  // show, so they should flow the same way.
  //
  // If the s190 m2-rev4 flicker comes back as a result of this
  // asymmetry, we instrument before guessing — log mode rather than
  // re-toggling between set_visible and present chasing the right
  // answer.
  // s219 m1 — toggle handler. The monkey button drives the Scripter
  // window's visibility; show_scripter() is the canonical entry point
  // on MainWindow that does the right thing on each side. Show path
  // calls into m_scripter->show(*this) which sets transient_for and
  // applies the motif class on every show — matching the HelpWindow /
  // ShortcutsDialog pattern that's known to behave well on GNOME.
  //
  // The s190 m2-rev4 mousedown-hide/mouseup-show flicker that the
  // earlier symmetric-set_visible code was guarding against doesn't
  // recur with the show()-based approach because show() is idempotent
  // (re-asserting transient_for and presenting an already-visible
  // window is a no-op at the user-perceivable level).
  auto syncing = std::make_shared<bool>(false);
  scripter_btn->signal_toggled().connect([this, scripter_btn, syncing]() {
    if (*syncing) return;  // change came from back-channel; don't recurse
    show_scripter(scripter_btn->get_active());
  });

  // Back-channel: keep the toggle in sync with the actual window
  // visibility. property_visible fires on any visibility change
  // regardless of cause (X-button close, set_visible(false), present
  // from cold), which is the correct signal for "stay in sync."
  //
  // Deferred via signal_idle so MainWindow's ctor finishes (and
  // therefore m_scripter is alive) before we subscribe.
  Glib::signal_idle().connect_once(
      [this, scripter_btn, syncing]() {
        if (!m_scripter) return;
        auto* sw = m_scripter.get();
        // Initial seed.
        *syncing = true;
        scripter_btn->set_active(sw->get_visible());
        *syncing = false;
        // Live sync.
        sw->property_visible().signal_changed().connect(
            [scripter_btn, sw, syncing]() {
              *syncing = true;
              scripter_btn->set_active(sw->get_visible());
              *syncing = false;
            });
      });

  m_headerbar.pack_start(*scripter_btn);

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
  doc_label->set_hexpand(true);
  doc_label->set_halign(Gtk::Align::END);
  doc_label->set_margin_start(8);
  doc_label->set_margin_end(6);
  m_headerbar.pack_start(*doc_label);

  auto *doc_sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
  doc_sep->set_margin_top(8);
  doc_sep->set_margin_bottom(8);
  doc_sep->set_margin_start(0);
  doc_sep->set_margin_end(0);
  m_headerbar.pack_start(*doc_sep);

  // Right-click on the "Documents" label → "New Document" context menu.
  // Wired directly against the label so clicking it still opens the
  // bar menu even though the label no longer lives inside the
  // DocTabBar widget tree.
  auto bar_ctx = Gtk::GestureClick::create();
  bar_ctx->set_button(3);
  bar_ctx->signal_pressed().connect([this](int, double /*x*/, double /*y*/) {
    m_doc_tabs.signal_add_doc().emit();
  });
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
  Glib::signal_idle().connect_once(
      [this]() { m_headerbar.set_title_widget(m_doc_tabs); });

  // Hamburger menu — right side of headerbar
  m_hamburger.set_icon_name("open-menu-symbolic");
  m_hamburger.set_tooltip_text("Menu");
  m_hamburger.set_has_frame(false);
  curvz::utils::set_name(m_hamburger, "mw_ham", "main_window_hamburger_btn");
  m_headerbar.pack_end(m_hamburger);

  set_titlebar(m_headerbar);
}

// ─────────────────────────────────────────────────────────────────────
// ━━━ menu ━━━ Menubar construction (Gio::Menu structure, action declarations, accelerators, hamburger button).
// ─────────────────────────────────────────────────────────────────────

void MainWindow::setup_menu() {
  // s145 m1 — seed ruler visibility from the user pref before the
  // toggle-rulers action below picks up its initial state. AppPreferences
  // is loaded by Application::on_activate before MainWindow is
  // constructed, so the value is available here. Per-session Ctrl+R
  // toggling flips m_rulers_visible without writing back to the pref;
  // the pref controls only the boot state. The actual ruler-widget
  // visibility is applied after setup_layout() runs (see constructor).
  m_rulers_visible = AppPreferences::instance().show_rulers_by_default();

  // ── Build Gio::Menu ────────────────────────────────────────────────────
  auto menu = Gio::Menu::create();

  // ── File submenu ────────────────────────────────────────────────────────
  auto file_menu = Gio::Menu::create();
  file_menu->append("New Project…", "win.new-project");
  file_menu->append("Add to Project…", "win.new");
  file_menu->append("Open…", "win.open");
  // s144 m3 — Open Recent submenu. Sits right after Open… to match
  // Photoshop / Illustrator / VS Code conventions. Built empty here;
  // populated by rebuild_recents_menu() which runs at construction
  // time below and on every RecentProjects::signal_changed emit.
  m_recents_menu = Gio::Menu::create();
  file_menu->append_submenu("Open Recent", m_recents_menu);
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
  // s179 m1: unified Export dialog. Tabbed parent for Documents +
  // Icon Theme; replaces the previous File ▸ Export Icon Theme… and
  // Project ▸ Export Documents… split entries.
  file_io->append("Export…", "win.export");
  file_io->append("Print…", "win.print");
  file_menu->append_section("", file_io);
  auto file_section = Gio::Menu::create();
  file_section->append_submenu("File", file_menu);
  menu->append_section("", file_section);

  // ── Edit submenu ────────────────────────────────────────────────────────
  auto edit_menu = Gio::Menu::create();
  edit_menu->append("Undo", "win.undo");
  edit_menu->append("Redo", "win.redo");
  // s136 m5: selection section — Select All + Deselect All sit between
  // history (Undo/Redo) and clipboard (Cut/Copy/Paste). Order matches
  // GIMP / Inkscape / Affinity Edit-menu convention.
  auto edit_select = Gio::Menu::create();
  edit_select->append("Select All", "win.select-all");
  edit_select->append("Deselect All", "win.deselect-all");
  edit_menu->append_section("", edit_select);
  auto edit_sep = Gio::Menu::create();
  edit_sep->append("Cut", "win.cut");
  edit_sep->append("Copy", "win.copy");
  edit_sep->append("Paste", "win.paste");
  edit_sep->append("Duplicate", "win.duplicate");
  edit_sep->append("Duplicate in Place", "win.duplicate-in-place");
  // s146 m1: Step and Repeat moved to Path submenu. It's a destructive
  // transform (selection + params → N new objects, no persistent
  // re-editable container, atomic undo) — same shape as Boolean ops, so
  // it now sits with them in Path. Action name (win.step-repeat) and
  // hotkey (Ctrl+Alt+D) are unchanged; only the menu position moved.
  edit_menu->append_section("", edit_sep);
  // s203 m1: View Clipboard — small floating window that shows the
  // current system clipboard contents (Type + URL header, body for any
  // text representation). Lets the user select a sub-piece of a copied
  // block and recopy with Ctrl+C, avoiding the TextEdit round-trip.
  // Motivated by the Measure tool's structured-block copy, but works on
  // anything textual on the clipboard.
  auto edit_clip_view = Gio::Menu::create();
  edit_clip_view->append("View Clipboard…", "win.view-clipboard");
  edit_menu->append_section("", edit_clip_view);
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

  // ── Align submenu ───────────────────────────────────────────────────────
  // s135 m1: Align & Distribute had been toolbar-only since introduction;
  // adding the menu mirror brings it into line with Arrange/Path which both
  // expose toolbar AND menu paths. Enable/disable is driven by the same
  // update_align_btn predicate that gates the toolbar button (selection >= 2
  // && Selection tool active). Hotkeys are bound for the six align ops;
  // distribute is menu-only by design (used less often, less muscle-memory
  // pressure to reserve a combo).
  auto align_menu = Gio::Menu::create();
  align_menu->append("Align Left", "win.align-left");
  align_menu->append("Align Center H", "win.align-center-h");
  align_menu->append("Align Right", "win.align-right");
  auto align_v_section = Gio::Menu::create();
  align_v_section->append("Align Top", "win.align-top");
  align_v_section->append("Align Center V", "win.align-center-v");
  align_v_section->append("Align Bottom", "win.align-bottom");
  align_menu->append_section("", align_v_section);
  auto distribute_section = Gio::Menu::create();
  distribute_section->append("Distribute Horizontally", "win.distribute-h");
  distribute_section->append("Distribute Vertically", "win.distribute-v");
  align_menu->append_section("", distribute_section);
  auto align_section = Gio::Menu::create();
  align_section->append_submenu("Align", align_menu);
  menu->append_section("", align_section);

  // ── Path submenu ────────────────────────────────────────────────────────
  auto path_menu = Gio::Menu::create();
  path_menu->append("Union", "win.bool-union");
  path_menu->append("Subtract", "win.bool-subtract");
  path_menu->append("Intersect", "win.bool-intersect");
  // s146 m1: Step and Repeat lives with the Booleans because it has the
  // same shape — destructive transform on the selection that produces N
  // new objects, no persistent re-editable container, single atomic undo.
  // Was in Edit menu's clipboard section through s145; honest home is here.
  path_menu->append("Step and Repeat…", "win.step-repeat");
  // s143 m1 — "Clean Boolean Output" menu toggle (s139 m2) removed.
  // The boolean cleanup post-pass is now controlled by a quality slider
  // in the inspector's Application group (build_app_section): slider at
  // 0 disables cleanup, 1..10 scales faithfulness. No menu surface for
  // this anymore — the inspector is the single point of control.
  auto path_compound = Gio::Menu::create();
  path_compound->append("Make Compound Path", "win.make-compound");
  path_compound->append("Split Compound Path", "win.split-compound");
  path_menu->append_section("", path_compound);
  // Group section (s138) — Group enabled with >=2 nodes selected; Ungroup
  // enabled when the primary selection is a Group. Conventionally sits
  // beside Compound because both operations wrap a multi-selection in a
  // SceneNode container; the difference is structural (Group preserves
  // the children's identities for editing later; Compound merges them
  // into one boolean unit).
  auto path_group = Gio::Menu::create();
  path_group->append("Group", "win.group-make");
  path_group->append("Ungroup", "win.group-release");
  path_menu->append_section("", path_group);
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
  path_warp->append("Warp", "win.warp-make");
  path_warp->append("Edit Warp", "win.warp-edit");
  path_warp->append("Release Warp", "win.warp-release");
  path_warp->append("Flatten Warp", "win.warp-flatten");
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

  // s179 m1: Project submenu dropped. Originally (S103 m3) it housed
  // Themes… and Export Documents…; s147 m3 removed Themes… (replaced
  // by ThemesPanel in the Content pane), and s179 m1 unified Export
  // Documents… into File ▸ Export…. The submenu itself had no other
  // inhabitants, so removing it leaves the menu structure cleaner.

  // ── Navigate submenu (s108 m7) ─────────────────────────────────────────
  // Document navigation. Menu placement also forces GTK to register the
  // accels via the menu-item path, which has different precedence than
  // bare action accels — required for Ctrl+Tab to reach the action
  // before GTK4's built-in focus-traversal default consumes it.
  auto navigate_menu = Gio::Menu::create();
  navigate_menu->append("Next Document", "win.doc-next");
  navigate_menu->append("Previous Document", "win.doc-prev");
  auto navigate_section = Gio::Menu::create();
  navigate_section->append_submenu("Navigate", navigate_menu);
  menu->append_section("", navigate_section);

  // ── Developer submenu (s219 m1) ────────────────────────────────────────
  // Top-level submenu for power-user / contributor surfaces. First (and
  // currently only) inhabitant: Scripting — the master toggle for
  // Curvz's script-driven test harness (the Scripter window).
  //
  // The menu item is a stateful boolean GAction so a checkmark
  // mirrors the live AppPreferences::scripter_enabled value. When the
  // user activates it, the action flips the pref; the pref's
  // signal_changed re-runs apply_scripter_pref() which feeds the new
  // state back into the action via set_state(). The two paths can
  // re-enter each other safely because:
  //   - set_state on the action is silent (doesn't re-fire activate)
  //   - set_scripter_enabled is no-op-if-equal
  // so the loop terminates after one round-trip regardless of who
  // started it.
  //
  // The matching inspector switch (Application ▸ Developer ▸ Scripting)
  // mirrors this surface — both write to the same pref. Future
  // Developer-menu inhabitants land here as a sibling section.
  auto developer_menu = Gio::Menu::create();
  developer_menu->append("Scripting", "win.toggle-scripting");
  auto developer_section = Gio::Menu::create();
  developer_section->append_submenu("Developer", developer_menu);
  menu->append_section("", developer_section);

  // ── App items ───────────────────────────────────────────────────────────
  auto app_section = Gio::Menu::create();
  app_section->append("Help", "win.show-help");
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

  // s147 m3: show-themes action removed alongside the menu entry.
  // ThemesPanel in Content is the canonical surface.

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

  // s179 m1: unified Export dialog (File ▸ Export…). Tabbed parent
  // for Documents + Icon Theme; one action and one menu entry replace
  // the previous win.export-theme + win.export-docs split.
  auto act_export = Gio::SimpleAction::create("export");
  act_export->signal_activate().connect(
      [this](const Glib::VariantBase &) { on_export(); });
  add_action(act_export);

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

  // s162 m3: delete-selected exposed as a Gio action so the right-click
  // context menu can invoke it. Today's Delete/Backspace handling lives
  // in the CAPTURE-phase key controller (MainWindow.cpp:~3675 calling
  // m_canvas.delete_selected()) — that path is unchanged. This action
  // exists so menu items have something to invoke; menu/keyboard parity
  // for menus that drive operations on the current selection.
  auto act_delete_selected = Gio::SimpleAction::create("delete-selected");
  act_delete_selected->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.delete_selected(); });
  add_action(act_delete_selected);

  // s181: was "clone" / clone_selected. Renamed because the operation
  // never had source/instance semantics — it's a zero-offset duplicate.
  auto act_duplicate_in_place = Gio::SimpleAction::create("duplicate-in-place");
  act_duplicate_in_place->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.duplicate_in_place_selected(); });
  add_action(act_duplicate_in_place);

  // s136 m5: select-all and deselect-all promoted to actions so the Edit
  // menu can reach them. The hotkeys (Ctrl+A and Ctrl+Shift+A) are still
  // dispatched through the CAPTURE-phase controller in MainWindow.cpp's
  // key handler — set_accels_for_action is cosmetic in this codebase
  // (per the hotkey-dispatch convention). These actions exist so the
  // menu entries have something to invoke.
  auto act_select_all = Gio::SimpleAction::create("select-all");
  act_select_all->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.select_all(); });
  add_action(act_select_all);

  auto act_deselect_all = Gio::SimpleAction::create("deselect-all");
  act_deselect_all->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.clear_selection(); });
  add_action(act_deselect_all);

  // s203 m1: View Clipboard… — Edit menu entry. Opens a mini floating
  // window that shows the current system clipboard contents so the user
  // can select a piece and Ctrl+C it (use case: pull one value out of a
  // multi-line Measure-tool copy block without the TextEdit round-trip).
  // The window lives on MainWindow as a lazy-built unique_ptr; this
  // action just triggers refresh + present.
  auto act_view_clipboard = Gio::SimpleAction::create("view-clipboard");
  act_view_clipboard->signal_activate().connect(
      [this](const Glib::VariantBase &) { show_clipboard_view(); });
  add_action(act_view_clipboard);

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
  m_act_bool_union->signal_activate().connect(
      [this](const Glib::VariantBase &) {
        m_canvas.boolean_op(BooleanOpType::Union);
      });
  m_act_bool_union->set_enabled(false);
  add_action(m_act_bool_union);

  m_act_bool_subtract = Gio::SimpleAction::create("bool-subtract");
  m_act_bool_subtract->signal_activate().connect(
      [this](const Glib::VariantBase &) {
        m_canvas.boolean_op(BooleanOpType::Subtract);
      });
  m_act_bool_subtract->set_enabled(false);
  add_action(m_act_bool_subtract);

  m_act_bool_intersect = Gio::SimpleAction::create("bool-intersect");
  m_act_bool_intersect->signal_activate().connect(
      [this](const Glib::VariantBase &) {
        m_canvas.boolean_op(BooleanOpType::Intersect);
      });
  m_act_bool_intersect->set_enabled(false);
  add_action(m_act_bool_intersect);

  // s135 m1: Align & Distribute actions. Same wire-up pattern as boolean ops:
  // stored on MainWindow so the update_align_btn predicate (extended below)
  // can toggle enabled state. Activate handler delegates to Canvas, exactly
  // mirroring what the toolbar popover already does via signal_align_requested.
  m_act_align_left = Gio::SimpleAction::create("align-left");
  m_act_align_left->signal_activate().connect(
      [this](const Glib::VariantBase &) {
        m_canvas.align_selection(AlignOp::AlignLeft);
      });
  m_act_align_left->set_enabled(false);
  add_action(m_act_align_left);

  m_act_align_center_h = Gio::SimpleAction::create("align-center-h");
  m_act_align_center_h->signal_activate().connect(
      [this](const Glib::VariantBase &) {
        m_canvas.align_selection(AlignOp::AlignCenterH);
      });
  m_act_align_center_h->set_enabled(false);
  add_action(m_act_align_center_h);

  m_act_align_right = Gio::SimpleAction::create("align-right");
  m_act_align_right->signal_activate().connect(
      [this](const Glib::VariantBase &) {
        m_canvas.align_selection(AlignOp::AlignRight);
      });
  m_act_align_right->set_enabled(false);
  add_action(m_act_align_right);

  m_act_align_top = Gio::SimpleAction::create("align-top");
  m_act_align_top->signal_activate().connect([this](const Glib::VariantBase &) {
    m_canvas.align_selection(AlignOp::AlignTop);
  });
  m_act_align_top->set_enabled(false);
  add_action(m_act_align_top);

  m_act_align_center_v = Gio::SimpleAction::create("align-center-v");
  m_act_align_center_v->signal_activate().connect(
      [this](const Glib::VariantBase &) {
        m_canvas.align_selection(AlignOp::AlignCenterV);
      });
  m_act_align_center_v->set_enabled(false);
  add_action(m_act_align_center_v);

  m_act_align_bottom = Gio::SimpleAction::create("align-bottom");
  m_act_align_bottom->signal_activate().connect(
      [this](const Glib::VariantBase &) {
        m_canvas.align_selection(AlignOp::AlignBottom);
      });
  m_act_align_bottom->set_enabled(false);
  add_action(m_act_align_bottom);

  m_act_distribute_h = Gio::SimpleAction::create("distribute-h");
  m_act_distribute_h->signal_activate().connect(
      [this](const Glib::VariantBase &) {
        m_canvas.align_selection(AlignOp::DistributeH);
      });
  m_act_distribute_h->set_enabled(false);
  add_action(m_act_distribute_h);

  m_act_distribute_v = Gio::SimpleAction::create("distribute-v");
  m_act_distribute_v->signal_activate().connect(
      [this](const Glib::VariantBase &) {
        m_canvas.align_selection(AlignOp::DistributeV);
      });
  m_act_distribute_v->set_enabled(false);
  add_action(m_act_distribute_v);

  auto act_offset_path = Gio::SimpleAction::create("offset-path");
  act_offset_path->signal_activate().connect([this](const Glib::VariantBase &) {
    auto *doc = m_project ? m_project->active_doc() : nullptr;
    const CanvasModel *cm = doc ? &doc->canvas : nullptr;
    m_offset_path_dialog.show(
        *this, cm, [this](OffsetPathDialog::Options opts) {
          m_canvas.offset_path_op(opts.distance, opts.side, opts.keep_original);
        });
  });
  add_action(act_offset_path);

  // s205 m4 — Translate hub dialog. Right-click menu's "Translate…" entry
  // (Canvas.cpp::build_object_context_menu) and any future menu/hotkey
  // exposure both activate this action. Selection-aware via the dialog's
  // internal apply-time re-collect — no enable/disable gating needed at
  // the action level (the dialog no-ops on empty selection).
  auto act_translate = Gio::SimpleAction::create("translate-dialog");
  act_translate->signal_activate().connect([this](const Glib::VariantBase &) {
    auto *doc = m_project ? m_project->active_doc() : nullptr;
    const CanvasModel *cm = doc ? &doc->canvas : nullptr;
    const double rox = doc ? doc->ruler_origin_x : 0.0;
    const double roy = doc ? doc->ruler_origin_y : 0.0;
    m_translate_dialog.present(*this, &m_canvas, &m_history,
                               m_project.get(), cm, rox, roy);
  });
  add_action(act_translate);

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

  // Group / Ungroup (s138). Both wrap pre-existing Canvas methods that
  // were never reachable from the UI. Default-disabled; sensitivity is
  // set per selection by update_group_actions_sensitive(), called from
  // refresh_inspector alongside the other action sensitivity helpers.
  m_act_group_make = Gio::SimpleAction::create("group-make");
  m_act_group_make->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.group_selection(); });
  m_act_group_make->set_enabled(false);
  add_action(m_act_group_make);

  m_act_group_release = Gio::SimpleAction::create("group-release");
  m_act_group_release->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_canvas.ungroup_selection(); });
  m_act_group_release->set_enabled(false);
  add_action(m_act_group_release);

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

  // Developer — Scripting toggle (s219 m1, stateful boolean action).
  // The action's state mirrors AppPreferences::scripter_enabled.
  //
  // Activation flow:
  //   1. User clicks the menu item → Gio fires activate() with no
  //      parameter (stateful bool actions invert their current state).
  //   2. We read the action's current state, compute the new value,
  //      and write the pref. set_scripter_enabled fires signal_changed.
  //   3. apply_scripter_pref() (subscribed to signal_changed in
  //      MainWindow's ctor) updates the action state, the headerbar
  //      button visibility, and the Scripter window's present/hide.
  //
  // The action is created with its initial state matching the loaded
  // pref so the menu's checkmark is correct from first paint. The
  // member ref m_act_toggle_scripting lets apply_scripter_pref()
  // update the state without an action-group lookup round-trip
  // (same pattern as m_recents_clear_action above).
  m_act_toggle_scripting = Gio::SimpleAction::create_bool(
      "toggle-scripting",
      AppPreferences::instance().scripter_enabled());
  m_act_toggle_scripting->signal_activate().connect(
      [this](const Glib::VariantBase &) {
        bool current = false;
        m_act_toggle_scripting->get_state(current);
        AppPreferences::instance().set_scripter_enabled(!current);
        // No direct apply_scripter_pref() call here — the pref's
        // signal_changed will fire it for us, keeping a single
        // re-sync path regardless of who triggered the change.
      });
  add_action(m_act_toggle_scripting);

  // S93 m7: toggle-clipper2 action removed. Boolean ops now run
  // exclusively through Clipper2; hand-rolled engine is retained on
  // disk for reference but no longer wired to the action system.

  // Path — boolean cleanup quality (s143 m1, replaces s139 m2 toggle).
  // The user-facing control is a slider in the inspector's Application
  // group (PropertiesPanel::build_app_section). This block does two
  // things:
  //   1. Seeds the Canvas's quality field from the loaded AppPreferences
  //      value at startup (so the first boolean op uses the pref).
  //   2. Connects to AppPreferences::signal_changed so any later slider
  //      tug updates the Canvas before the next boolean op.
  // The signal is parameterless and re-fires for every pref change
  // (including ones that aren't quality), so we just re-read.
  // No GAction, no menu item — the slider is the only surface.
  {
    auto &prefs = AppPreferences::instance();
    m_canvas.set_boolean_cleanup_quality(prefs.boolean_cleanup_quality());
    prefs.signal_changed().connect([this]() {
      m_canvas.set_boolean_cleanup_quality(
          AppPreferences::instance().boolean_cleanup_quality());
    });
  }

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

  // s138 fix: action name is "zoom-100" to match the three call sites
  // (menu item, accel binding, update_project_sensitive enable list).
  // Was "zoom-actual" historically; the callers were renamed to "zoom-100"
  // to match the user-facing label "Zoom to 100%" but the action itself
  // was missed in the rename. Result: lookup_action("zoom-100") returned
  // null everywhere, the menu item rendered disabled, and the keybind
  // dispatched into the void. Caught when s138 m2 made the popover render
  // accels honestly — the disabled state was always there; the empty
  // accel column just hid it.
  auto act_zoom_100 = Gio::SimpleAction::create("zoom-100");
  act_zoom_100->signal_activate().connect([this](const Glib::VariantBase &) {
    // 1× = fit-zoom (artboard fills viewport with margin)
    double cx = m_canvas.get_width() / 2.0;
    double cy = m_canvas.get_height() / 2.0;
    double target = m_canvas.fit_zoom_value();
    m_canvas.zoom_toward(cx, cy, target / m_canvas.zoom());
  });
  add_action(act_zoom_100);

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

  // ── s144 m3: Open Recent action group ─────────────────────────────────
  // Recents items invoke recents.open(<path>) — a parameterised string
  // action. Using an action group (rather than win.open-recent) mirrors
  // SwatchesPanel's load-bundled / load-user pattern, which is the only
  // proven parameterised-action shape in this codebase.
  //
  // The path is the .curvz directory (matches CurvzProject::open).
  // Missing-on-disk pruning happens at RecentProjects::load (boot) and
  // at the action handler (defensive — file could be deleted while the
  // menu is open). Either way, a clean log line, no crash.
  auto recents_group = Gio::SimpleActionGroup::create();
  recents_group->add_action_with_parameter(
      "open", Glib::VariantType("s"), [this](const Glib::VariantBase &param) {
        auto str_v =
            Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(
                param);
        std::string path = str_v.get();
        if (path.empty())
          return;
        if (!fs::exists(path)) {
          LOG_WARN("recents.open: '{}' no longer exists — pruning", path);
          RecentProjects::instance().remove(path);
          return;
        }
        auto project = CurvzProject::open(path);
        if (!project) {
          LOG_ERROR("recents.open: failed to open '{}'", path);
          return;
        }
        load_project(std::move(project));
      });
  // Build clear as a SimpleAction directly so we can hold a ref and
  // toggle enabled state from rebuild_recents_menu without touching
  // group-lookup machinery.
  m_recents_clear_action = Gio::SimpleAction::create("clear");
  m_recents_clear_action->signal_activate().connect(
      [](const Glib::VariantBase &) { RecentProjects::instance().clear(); });
  recents_group->add_action(m_recents_clear_action);
  insert_action_group("recents", recents_group);

  // Subscribe to recents-list churn so the submenu stays in sync. The
  // signal is parameterless; we just rebuild from the current paths().
  RecentProjects::instance().signal_changed().connect(
      [this]() { rebuild_recents_menu(); });

  // Initial population — list was loaded at Application::on_activate, so
  // the in-memory state is already correct; this just paints it.
  rebuild_recents_menu();

  // ── Attach popover to hamburger button ────────────────────────────────
  auto popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
  m_hamburger.set_popover(*popover);

  // ── Sync motif class to newly-added windows (S117 m15) ────────────────
  // When a new top-level window is registered (e.g. a dialog opens), sync
  // the motif class so it inherits the current project's appearance.
  // Combined with the walk in apply_motif_to_window, this covers both
  // "dialog open at toggle time" and "dialog opens after toggle".
  //
  // Note: keyboard accelerator registration moved to Application::on_startup
  // in s138. get_application() returns null during MainWindow construction
  // (the window joins the app *after* construction completes), so any code
  // here that depends on the application reference must guard against that
  // null-or-not-yet-attached window.
  if (auto app = get_application()) {
    app->signal_window_added().connect([this](Gtk::Window *w) {
      LOG_INFO("signal_window_added fired: w={}", (void *)w);
      if (w && w != this)
        sync_motif_class_to(*w);
    });
  }
}

// ─────────────────────────────────────────────────────────────────────
// ━━━ layout ━━━ Main paned layout, ruler placement, panels, canvas area, overlay wrapper, status bar.
// ─────────────────────────────────────────────────────────────────────

void MainWindow::setup_layout() {
  set_child(m_root);

  m_doc_tabs.set_project(m_project.get());
  m_gallery.set_project(m_project.get());

  m_root.append(m_middle);
  m_middle.set_vexpand(true);
  m_middle.append(m_toolbar);

  // s152 — apply persisted toolbar density. AppPreferences stores it
  // as int 0..3 mapping to Toolbar::Density enum order. Default 1
  // (Standard) — the curvz-going-forward default after the s152
  // toolbar refactor. Programmatic set_density() does NOT emit
  // signal_density_changed, so this startup apply is silent (no
  // re-save loop).
  {
    int td = AppPreferences::instance().toolbar_density();
    auto d =
        static_cast<Toolbar::Density>(td); // already clamped 0..3 in setter
    m_toolbar.set_density(d);
  }

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
  m_context_bar.set_help_callback([this](const std::string &resource_path) {
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
  m_canvas.set_swatch_library(&m_project->swatches); // Phase 5 M3
  m_canvas.set_style_library(&m_project->styles);    // S78 m3d
  m_canvas.set_project(m_project.get()); // s116 m6 — workspace appearance reads
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
  m_properties.set_canvas_widget(
      &m_canvas); // S58f: multi-select broadcast for Appearance
  m_right_panels.append(m_properties);

  // Content group — wraps Layers, Library, Documents so they appear as a
  // single top-level collapsible (matching the Document/Object groups
  // inside the inspector).
  auto content = make_group_section("Content", false, &m_sec_open_content);
  content.outer->set_vexpand(true); // let children (Layers) stretch
  content.container->set_vexpand(true);
  m_right_panels.append(*content.outer);

  // Preview panel intentionally not shown — code retained for future use
  // m_preview.set_document(m_project->active_doc());
  // content.container->append(*make_section("Preview", m_preview,
  //                                     m_project->sec_preview_open, false,
  //                                     &m_sec_open_preview));
  m_layers.set_document(m_project->active_doc());
  // s171 m1 — wire history + project so structural mutations push
  // commands instead of mutating the doc directly. Order: set_document
  // first (doc ptr in place), then history+project (so any signal-
  // driven push site has full context).
  m_layers.set_history(&m_history);
  m_layers.set_project(m_project.get());
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
  m_library.set_project(m_project.get()); // s117 m14 v2 — thumb motif/artboard
  m_library.signal_place_item().connect(
      [this](const std::string &path) { m_canvas.import_svg_to_canvas(path); });
  m_library.signal_add_to_library().connect(
      [this](const std::string &dest_dir) {
        on_save_selection_to_library(dest_dir);
      });
  // s141: per-project library category expansion state. LibraryPanel
  // writes m_project->library_expanded_categories on each toggle and
  // emits this signal; we route to schedule_save through the existing
  // debounce so multiple rapid toggles coalesce into one save.
  m_library.signal_request_save().connect([this]() { schedule_save(); });
  content.container->append(
      *make_section("Library", m_library, false, false, &m_sec_open_library));

  // Swatches section — Phase 4. Shows project-scoped swatch library.
  // Placed between Library and Documents so the Content group reads as
  // Layers (primary) → reference collections (Library, Swatches) →
  // Documents (project-level).
  m_swatches.set_library(&m_project->swatches);
  m_swatches.set_canvas(&m_canvas);
  m_swatches.set_history(&m_history); // s220 m1a: undo for swatch CRUD
  m_swatches.signal_library_changed().connect([this]() {
    schedule_save();
    // S91: re-call set_swatch_library so the toolbar re-runs its
    // sensitivity check + popover refresh. Adding the first swatch
    // (or removing the last) needs to flip the Swatch toggle's
    // sensitive state; the library pointer hasn't changed, but the
    // count has.
    if (m_project)
      m_toolbar.set_swatch_library(&m_project->swatches);
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
  content.container->append(*make_section("Swatches", m_swatches, false, false,
                                          &m_sec_open_swatches));

  // Styles section — S81 m4c skeleton. Sibling to Swatches, peer panel
  // (not nested inside Swatches). Project-scoped style library: app
  // tier (hardcoded stubs) + user tier (per-project, persisted via
  // m3b's JSON round-trip). m4c-1 is read-only; click-to-bind and
  // create flows land in m4c-2.
  m_styles.set_library(&m_project->styles);
  m_styles.set_swatch_library(&m_project->swatches); // S85 cont-3
  // S87 — restore persisted dropdown selection. Must come AFTER
  // set_library() so the panel has a live library to look up the
  // category in; refresh() inside set_active_category will rebuild
  // with the right pre-selected index.
  m_styles.set_active_category(m_project->style_active_category,
                               m_project->style_active_is_app_tier);
  m_styles.set_canvas(&m_canvas);
  m_styles.set_history(&m_history); // S80 m4c-2: undo for click-to-bind
  m_styles.signal_library_changed().connect([this]() { schedule_save(); });
  // S87 — view state changed (dropdown selection). Pull current state
  // into the project fields and trigger a save. Same debounced
  // schedule_save path as content mutations.
  m_styles.signal_view_state_changed().connect([this]() {
    if (!m_project)
      return;
    m_project->style_active_category = m_styles.active_category();
    m_project->style_active_is_app_tier = m_styles.active_is_app_tier();
    schedule_save();
  });
  m_styles.set_canvas(&m_canvas);
  m_styles.set_history(&m_history); // S80 m4c-2: undo for click-to-bind
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
  // s201 m1 — StyleEditorDialog is now a MainWindow member (hide-on-
  // close singleton). show() prepares the editing buffer + callback
  // and presents; close() hides. on_committed pushes the appropriate
  // Add/Update command against m_history (the panel-side closure
  // handles the push; we just open the dialog).
  m_styles.signal_request_style_editor().connect(
      [this](StyleEditorDialog::Mode mode, style::Style initial,
             std::function<void(style::Style)> on_committed) {
        if (!m_project)
          return;
        // CanvasModel lives on the active CurvzDocument (each doc
        // has its own canvas dimensions). Pass nullptr if no doc
        // is active — the dialog's spinbuttons fall back to a
        // unit-conversion-less mode in that case (defensive; a
        // dialog open with no active doc shouldn't be reachable
        // through normal UI).
        CurvzDocument *doc = m_project->active_doc();
        CanvasModel *cm = doc ? &doc->canvas : nullptr;
        auto cats = m_project->styles.user_categories();
        m_style_editor_dialog.show(*this, m_project->swatches, cm, cats,
                                    mode, std::move(initial),
                                    std::move(on_committed));
      });
  content.container->append(
      *make_section("Styles", m_styles, false, false, &m_sec_open_styles));

  // s202 m4 / s219 m1 — give StylesPanel's panel-as-Scriptable a hookup to drive
  // the parent inspector section's expand/collapse state. The kebab
  // popover positions relative to its button's on-screen allocation;
  // when the section is collapsed the kebab has no allocation, so
  // popup() falls back to a default location (upper-right of the app).
  // The Scriptable's expand_section verb pre-opens the section so
  // narrated scripts can rely on the kebab appearing where a user
  // would expect it.
  //
  // The apply closure resolves "Styles" through m_sec_apply (the
  // registry make_section populated above). That preserves the full
  // open/close cascade — body visibility, arrow glyph, project field
  // update, schedule_save — without the Scriptable needing to know
  // any of that machinery.
  //
  // s219 m1 — always wired now. The panel-Scriptable is always built
  // (CMake change in s219 m1); the hookup is harmless when no script
  // ever runs, since the closure only fires when a script invokes
  // expand_section / focus_section on pnl_styles.
  m_styles.set_section_state(m_sec_open_styles,
      [this](bool on) {
        auto it = m_sec_apply.find("Styles");
        if (it != m_sec_apply.end()) it->second(on);
      });

  // s202 m5 / s219 m1 — composite focus-move hookup. See set_section_state
  // comment above for the gating rationale; same shape, same always-wired
  // story.
  m_styles.set_section_chain(
      {"Content", "Styles"},
      [this](const std::vector<std::string>& chain) {
        // Phase 1: collapse every registered section. m_sec_apply
        // holds entries for every title make_section and
        // make_group_section registered, so this hits the inspector
        // top-to-bottom without us enumerating them.
        for (auto& kv : m_sec_apply) {
          kv.second(false);
        }
        // Phase 2: walk the chain open, outermost first. Order
        // matters here: opening "Styles" before "Content" works
        // (both flags flip and the project state updates), but the
        // visual cascade reads more naturally when ancestors open
        // first.
        for (const auto& title : chain) {
          auto it = m_sec_apply.find(title);
          if (it != m_sec_apply.end()) it->second(true);
        }
      });

  // Themes panel — s147 m3 replacement for ThemesDialog. Project-scoped
  // library + apply-to-targets flow, always-visible. on_changed routes
  // canvas redraw + inspector refresh + save schedule, same as the
  // dialog's callback used to.
  //
  // s147 m3 fix1: set_project must run HERE, at setup_layout time —
  // sister panels call set_library on their respective project members
  // here too. Without this, m_project stays null until update_all_panels
  // fires (which only happens on doc add/delete or project switch — not
  // initial startup), so the [+] button silently early-returns. Setup
  // order matters: history first (stable address), project last (which
  // also runs the initial refresh via set_project's body).
  m_themes.set_history(&m_history);

  // s227 m1 — Factor the on-themes-changed body into a named local
  // so it can be installed in two places: (a) the existing
  // m_themes.set_on_changed slot (called by ThemesPanel when its
  // library mutations or apply-clicked complete), and (b) the new
  // ThemeLibrary::signal_theme_applied signal (fired by the scripted
  // apply verb in ThemesScriptable). Both code paths need the same
  // canvas refresh cascade — the panel goes through its callback, the
  // script goes through the library signal because it has no panel
  // pointer.
  //
  // s147 m3 fix8 (preserved): themes can mutate scene structure
  // (grid/margin layers added or removed by apply_theme_to_doc) and
  // shift fields the canvas reads live (guide colors, snap settings).
  // queue_draw alone repaints, but doesn't run the structural cascade
  // — the layers panel, status counts, ruler ticks, etc. need
  // notify_document_changed to refresh. Without it, the active doc's
  // canvas keeps showing pre-apply state until the user switches docs
  // or makes an unrelated edit.
  auto themes_refresh_cascade = [this]() {
    m_canvas.queue_draw();
    m_canvas.notify_document_changed();
    refresh_inspector();
    schedule_save();
  };
  m_themes.set_on_changed(themes_refresh_cascade);
  m_themes.set_project(m_project.get());

  // s227 m1 — wire scripted apply into the same refresh cascade. The
  // ThemesScriptable's apply verb fires signal_theme_applied after
  // calling apply_theme_to_doc + project->snap mirror; we connect it
  // to the exact same callback the panel's set_on_changed uses, so
  // canvas refresh + inspector + schedule_save run identically
  // whether apply was driven by the panel button or by a script
  // line. The signal carries the theme id; the refresh cascade
  // doesn't need it (refresh is broad, not theme-specific), so the
  // wrapper ignores the arg.
  //
  // Connection must run AFTER set_project (which is where ThemeLibrary
  // becomes addressable through m_project->themes). If the project
  // changes mid-session (close + reopen), the new project's
  // ThemeLibrary needs reconnecting; the current architecture
  // doesn't formally support project swap with signal re-wiring
  // (sister panels have the same property), so this connection is
  // effectively for-the-lifetime-of-the-window with the project's
  // identity stable across the use. Same lifetime story as the
  // theme-editor signal_request_theme_editor wiring below.
  if (m_project) {
    m_project->themes.signal_theme_applied().connect(
        [themes_refresh_cascade](theme::ThemeId /*id*/) {
          themes_refresh_cascade();
        });
  }

  // s183 m2 — Edit-theme dialog wiring. Mirrors the StyleEditorDialog
  // request handler above. The panel emits when its row Edit (✎)
  // button is clicked.
  //
  // s200 m1 — ThemeEditDialog is now a MainWindow member (hide-on-
  // close singleton). show() prepares the editing buffer + callback
  // and presents; close() hides. on_committed pushes UpdateThemeCommand
  // against m_history (the panel-side closure handles the push; we
  // just open the dialog). notify_changed() inside the panel's commit
  // closure runs the refresh + on_changed cascade (canvas redraw +
  // inspector + schedule_save), so the visual result follows
  // immediately.
  m_themes.signal_request_theme_editor().connect(
      [this](theme::Theme initial,
             std::function<void(theme::Theme)> on_committed) {
        // s183 m3: pass the project's current motif so the dialog's
        // thumbnail opens previewing the mode the user is actually
        // working in. The toggle inside the dialog flips the
        // thumbnail; never the chrome.
        Motif initial_mode =
            (m_project ? m_project->motif : Motif::Dark);
        m_theme_edit_dialog.show(*this, std::move(initial),
                                  initial_mode, std::move(on_committed));
      });

  content.container->append(
      *make_section("Themes", m_themes, false, false, &m_sec_open_themes));

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

  // s191 m3 + s192 m4 / s219 m1 — caption bar. Floats above the canvas as an
  // overlay rather than taking a row in m_root's layout flow. Anchored
  // to the bottom edge of the canvas overlay, full width, with the
  // SLIDE_UP transition revealing it from below the canvas edge into
  // view. The 30%-alpha background (s191 m3-followup) lets the canvas
  // read through; this only became visually honest once the bar moved
  // onto the canvas (m4) — before that it was alpha-blending against
  // the window chrome between m_middle and m_statusbar, which looked
  // muddy. m_can_target(false) routes pointer events through to the
  // canvas underneath so the caption never steals clicks.
  //
  // Substrate routing: m_caption_label is a plain Gtk::Label (not
  // a substrate wrapper) because it's display-only — no script
  // verbs to address it through. The revealer is also plain;
  // visibility is driven by ScriptListener via Application's
  // SubtitleCallback bridge, not by user scripts.
  //
  // s219 m1 — always compiled. The revealer's reveal-state starts
  // false and only flips when a running script emits `#[sub]` lines,
  // which can't happen unless the user has scripting enabled and is
  // running something. Cost when unused: a hidden Gtk::Revealer in
  // the overlay tree.
  m_caption_label.set_text("");
  m_caption_label.set_halign(Gtk::Align::CENTER);
  m_caption_label.set_hexpand(true);
  m_caption_label.set_ellipsize(Pango::EllipsizeMode::END);
  m_caption_label.set_single_line_mode(true);
  m_caption_label.add_css_class("mw-caption-label");
  m_caption_revealer.set_child(m_caption_label);
  m_caption_revealer.set_transition_type(
      Gtk::RevealerTransitionType::SLIDE_UP);
  m_caption_revealer.set_transition_duration(180);
  m_caption_revealer.set_reveal_child(false);
  m_caption_revealer.add_css_class("mw-caption-bar");
  // Overlay positioning: full canvas width, bottom-anchored.
  m_caption_revealer.set_halign(Gtk::Align::FILL);
  m_caption_revealer.set_valign(Gtk::Align::END);
  m_caption_revealer.set_hexpand(true);
  // Pointer events fall through to the canvas underneath. Same idiom
  // used on m_text_fixed (the text-tool overlay above).
  m_caption_revealer.set_can_target(false);
  m_canvas_overlay.add_overlay(m_caption_revealer);

  // Status bar — deferred append to avoid snapshot-before-allocation
  // warning on startup (S60). If appended synchronously at the end of
  // setup_layout, GTK's frame_clock_paint_idle can tick between our
  // append and its next size-allocate pass, attempting to snapshot an
  // unallocated widget. Deferring via signal_idle lets the current
  // frame cycle complete first. Same pattern as S48 DocTabBar fix.
  //
  // s223 m2: do NOT call m_statusbar.set_zoom(m_project->zoom * 100.0)
  // here. m_project->zoom defaults to 16.0 (interpreted as scale factor,
  // displays as "1600%"), and at idle time it has NOT been updated to
  // reflect the deferred zoom-to-fit that Canvas runs on its first
  // draw — zoom_fit only writes Canvas::m_zoom, never back to
  // m_project->zoom. The idle callback therefore overwrote the
  // correct fit value (which the signal-driven path had just
  // delivered, or was about to deliver) with the stale 1600%. The
  // signal-driven path (Canvas::m_sig_zoom → connect_signals listener
  // → m_statusbar.set_zoom) is the authoritative way to populate
  // this label; it fires on every zoom mutation including the
  // first-draw zoom_fit. The placeholder in StatusBar::StatusBar is
  // now "--" so any window-of-time between widget construction and
  // first emit shows a benign dash rather than a wrong-looking
  // percentage. See the s217 backlog item for the original bug
  // report.
  Glib::signal_idle().connect_once([this]() {
    m_root.append(m_statusbar);
    refresh_status_counts(); // s132 m2 — was set_counts(0, 0)
  });

  // Initial ruler state (will also be refreshed when canvas draws first frame)
  update_rulers();

  // s141: apply saved section open-state to the section widgets. The
  // sections were just built with hardcoded expanded=false; without this
  // pass, a returning user always sees their inspector sections collapsed
  // regardless of how they left them. setup_project ran before
  // setup_layout, so m_project carries the loaded values; m_sec_apply
  // was registered as each section was built above.
  if (m_project) {
    auto apply_sec = [this](const std::string &title, bool on) {
      auto it = m_sec_apply.find(title);
      if (it != m_sec_apply.end())
        it->second(on);
    };
    apply_sec("Content", m_project->sec_content_open);
    apply_sec("Layers", m_project->sec_layers_open);
    apply_sec("Library", m_project->sec_library_open);
    apply_sec("Swatches", m_project->sec_swatches_open);
    apply_sec("Styles", m_project->sec_styles_open);
    apply_sec("Themes", m_project->sec_themes_open);
    apply_sec("Documents", m_project->sec_documents_open);
    apply_sec("Preview", m_project->sec_preview_open);
  }

  // Populate inspector from restored project state once widget is realized
  Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
}

// ─────────────────────────────────────────────────────────────────────
// ━━━ overlays ━━━ Corner-treatment popover (build, show/hide, position).
// ─────────────────────────────────────────────────────────────────────

// ── Corner Treatment Panel
// ────────────────────────────────────────────────────

void MainWindow::build_corner_panel() {
  curvz::utils::set_name(m_corner_panel, "pop_corner",
                         "corner_panel_popover_root");
  // Radius entry — accepts expressions like "8px", "0.125in", "3px + 2mm"
  m_corner_radius_adj = Gtk::Adjustment::create(0.1, 0.0, 9999.0, 0.01, 0.1);
  m_corner_radius_spin.set_adjustment(m_corner_radius_adj);
  m_corner_radius_spin.set_digits(4);
  m_corner_radius_spin.set_numeric(false);
  m_corner_radius_spin.set_width_chars(10);
  m_corner_radius_spin.set_value(0.1);
  curvz::utils::set_name(m_corner_radius_spin, "pop_corner_rad",
                         "corner_panel_radius_spn");

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
      try {
        screen_px = UnitSystem::to_px(std::stod(txt), default_unit);
      } catch (...) {
        screen_px = 8.0;
      }
    }
    // Convert screen px → doc units
    if (doc && doc->canvas.display_mode == DisplayMode::Physical) {
      double phys_short =
          std::min(doc->canvas.phys_width, doc->canvas.phys_height);
      if (phys_short > 0 && doc->canvas.quality > 0)
        return (screen_px / 96.0) / phys_short * doc->canvas.quality;
    }
    return screen_px; // Pixel mode: screen px == doc px
  };

  // Enter key in entry → apply
  auto kc = Gtk::EventControllerKey::create();
  kc->signal_key_pressed().connect(
      [this, get_radius](guint keyval, guint, Gdk::ModifierType) -> bool {
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
          CornerType type = CornerType::Round;
          if (m_corner_btn_chamfer.get_active())
            type = CornerType::Chamfer;
          if (m_corner_btn_inverse.get_active())
            type = CornerType::InverseRound;
          m_canvas.apply_corner_treatment_op(type, get_radius());
          m_corner_panel.popdown();  // s194_m1 — match Apply button behaviour
          return true;
        }
        return false;
      },
      false);
  m_corner_radius_spin.add_controller(kc);

  // Type buttons — radio group via set_group
  m_corner_btn_round.set_label("Round");
  m_corner_btn_chamfer.set_label("Chamfer");
  m_corner_btn_inverse.set_label("Inverse");
  curvz::utils::set_name(m_corner_btn_round, "pop_corner_round",
                         "corner_panel_round_toggle");
  curvz::utils::set_name(m_corner_btn_chamfer, "pop_corner_chamfer",
                         "corner_panel_chamfer_toggle");
  curvz::utils::set_name(m_corner_btn_inverse, "pop_corner_inverse",
                         "corner_panel_inverse_toggle");
  m_corner_btn_chamfer.set_group(m_corner_btn_round);
  m_corner_btn_inverse.set_group(m_corner_btn_round);
  m_corner_btn_round.set_active(true);

  // Style each button; CSS :checked handles active highlight
  for (auto *btn :
       {&m_corner_btn_round, &m_corner_btn_chamfer, &m_corner_btn_inverse})
    (void)btn; // no custom CSS — use GTK default ToggleButton appearance

  // Update unit label whenever popover opens (picks up current doc unit live)
  m_corner_panel.signal_show().connect([this]() {
    auto *doc = m_project ? m_project->active_doc() : nullptr;
    if (doc) {
      Unit u = (doc->canvas.display_mode == DisplayMode::Physical)
                   ? UnitSystem::parse_unit(doc->canvas.phys_unit)
                   : doc->canvas.display_unit;
      m_corner_unit_label.set_text(std::string("Units: ") +
                                   UnitSystem::label(u));
      m_corner_radius_spin.set_value(0.1);
    }
  });

  // s194_m1: keep m_corner_panel_visible honest when the popover is
  // dismissed via GTK's own mechanisms (Escape, click-outside, etc.).  The
  // sticky-visibility listener in MainWindow_bindings.cpp checks this flag
  // to decide whether to re-show on the next non-empty selection.
  m_corner_panel.signal_closed().connect([this]() {
    LOG_INFO("corner_panel: signal_closed fired, tracker -> false");
    m_corner_panel_visible = false;
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
  curvz::utils::set_name(m_corner_apply_btn, "pop_corner_app",
                         "corner_panel_apply_btn");
  m_corner_apply_btn.signal_clicked().connect([this, get_radius]() {
    CornerType type = CornerType::Round;
    if (m_corner_btn_chamfer.get_active())
      type = CornerType::Chamfer;
    if (m_corner_btn_inverse.get_active())
      type = CornerType::InverseRound;
    m_canvas.apply_corner_treatment_op(type, get_radius());
    // s194_m1: dismiss the panel on Apply — the op is complete; leaving
    // the panel up over the now-changed canvas is more clutter than
    // useful.  signal_closed flips m_corner_panel_visible to false so
    // the next non-empty selection re-shows the panel.
    m_corner_panel.popdown();
  });

  m_corner_unit_label.set_text("Units: —");
  m_corner_unit_label.set_xalign(0.0f);
  m_corner_unit_label.add_css_class("pop-unit-label");
  curvz::utils::set_name(m_corner_unit_label, "pop_corner_unit",
                         "corner_panel_unit_lbl");

  m_corner_panel_vbox.set_spacing(6);
  m_corner_panel_vbox.set_margin_top(8);
  m_corner_panel_vbox.set_margin_bottom(8);
  m_corner_panel_vbox.set_margin_start(8);
  m_corner_panel_vbox.set_margin_end(8);
  m_corner_panel_vbox.append(m_corner_type_row);
  m_corner_panel_vbox.append(m_corner_radius_row);
  m_corner_panel_vbox.append(m_corner_unit_label);
  m_corner_panel_vbox.append(m_corner_apply_btn);

  // Anchor popover to the corner tool button in the toolbar.
  // s194_m1: disable autohide.  Without this, every click outside the
  // popover (i.e. every click on the canvas — *every node selection*)
  // dismisses the popover via GTK's autohide mechanism.  The user has to
  // re-open it by clicking the toolbar button between each selection,
  // which is unusable.  Sticky-visibility is owned by the
  // signal_corner_sel_changed listener in MainWindow_bindings.cpp;
  // Escape and explicit popdown() still close the panel.
  m_corner_panel.set_child(m_corner_panel_vbox);
  m_corner_panel.set_position(Gtk::PositionType::RIGHT);
  m_corner_panel.set_has_arrow(true);
  m_corner_panel.set_autohide(false);
  if (auto *btn = m_toolbar.get_corner_btn())
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

// ─────────────────────────────────────────────────────────────────────
// ━━━ caption bar ━━━ Subtitle-driven caption row above the status bar.
// s219 m1: always compiled. Only fires when a script with `#[sub]`
// lines runs (which requires the user to have enabled scripting and
// hit Run); otherwise the revealer sits hidden and untouched.
// ─────────────────────────────────────────────────────────────────────

void MainWindow::set_subtitle(const std::string& text) {
  // Empty text hides the caption. Non-empty shows the bar with the
  // new text. Replacing text while already visible is instant — we
  // don't toggle reveal off-and-on for replacements because the
  // SLIDE_UP transition is 180ms and would visibly flicker between
  // captions during a paced script run. Only the show-from-empty
  // and hide-to-empty transitions animate.
  if (text.empty()) {
    m_caption_revealer.set_reveal_child(false);
    // Defer the text clear by one idle so the slide-down animation
    // doesn't fight visibly with an empty-string mid-fade. Keeps
    // the caption visible during the slide.
    Glib::signal_idle().connect_once([this]() {
      m_caption_label.set_text("");
    });
    return;
  }
  m_caption_label.set_text(text);
  m_caption_revealer.set_reveal_child(true);
}

} // namespace Curvz
