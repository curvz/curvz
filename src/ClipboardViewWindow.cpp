// ── ClipboardViewWindow.cpp ─────────────────────────────────────────────────
//
// See ClipboardViewWindow.hpp for the design rationale. This file is the
// concrete UI: a mini chromeless float that snapshots the system clipboard
// on open and lets the user select a piece to recopy with Ctrl+C.

#include "ClipboardViewWindow.hpp"

namespace Curvz {

ClipboardViewWindow::ClipboardViewWindow()
    : m_root(Gtk::Orientation::VERTICAL),
      m_sep(Gtk::Orientation::HORIZONTAL),
      m_btn_row(Gtk::Orientation::HORIZONTAL),
      m_refresh_btn("Refresh") {
  // ── Window chrome ────────────────────────────────────────────────────────
  // Standard window decorations — the user gets a small titlebar with a
  // native × close button. This is a working tool the user dips in and
  // out of across context switches; a titlebar earns its space here in a
  // way it didn't for the quick-jump float (one-shot, closes on pick).
  // Not modal — workflow is "click away to paste, come back for another
  // piece."
  set_title("Clipboard");
  set_resizable(true);
  set_hide_on_close(true);
  set_default_size(360, 240);
  add_css_class("clip-view-win");

  // Escape closes. Focus-out does NOT close — that would break the
  // come-back-for-another-piece workflow this window exists for.
  auto kc = Gtk::EventControllerKey::create();
  kc->signal_key_pressed().connect(
      [this](guint kv, guint, Gdk::ModifierType) -> bool {
        if (kv == GDK_KEY_Escape) {
          set_visible(false);
          return true;
        }
        return false;
      },
      /*after=*/false);
  add_controller(kc);

  // ── Root layout ──────────────────────────────────────────────────────────
  m_root.set_margin_top(4);
  m_root.set_margin_bottom(4);
  m_root.set_margin_start(4);
  m_root.set_margin_end(4);
  m_root.set_spacing(3);
  set_child(m_root);

  // ── Type header strip ────────────────────────────────────────────────────
  // Dim, small. Shows the MIME-type formats currently on the clipboard plus
  // the URL line (always present for format consistency, "(none)" when there
  // is no URL on the clipboard — see refresh() for the policy).
  m_type_lbl.set_xalign(0.0f);
  m_type_lbl.set_wrap(true);
  m_type_lbl.set_wrap_mode(Pango::WrapMode::WORD_CHAR);
  m_type_lbl.add_css_class("clip-view-header");
  m_type_lbl.add_css_class("dim-label");
  m_root.append(m_type_lbl);

  m_root.append(m_sep);

  // ── Body ─────────────────────────────────────────────────────────────────
  // Read-only-feeling: editable() false so the user can't accidentally type
  // into it, but they CAN select and Ctrl+C — that's the whole point. Body
  // gets a monospace class so structured measure blocks (x₁ = ...) line up
  // neatly when the user scans for a value.
  m_body.set_editable(false);
  m_body.set_cursor_visible(true);  // shows where Ctrl+A / click puts caret
  m_body.set_monospace(true);
  m_body.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
  m_body.add_css_class("clip-view-body");

  m_scroll.set_child(m_body);
  m_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
  m_scroll.set_hexpand(true);
  m_scroll.set_vexpand(true);
  m_root.append(m_scroll);

  // ── Button row ───────────────────────────────────────────────────────────
  // Only Refresh — the titlebar's × handles closing, and Escape closes
  // too (key controller above). A second Close button would be clutter.
  m_btn_row.set_spacing(4);
  m_btn_row.set_halign(Gtk::Align::END);

  m_refresh_btn.add_css_class("clip-view-btn");
  m_refresh_btn.signal_clicked().connect([this]() { refresh(); });
  m_btn_row.append(m_refresh_btn);

  m_root.append(m_btn_row);
}

void ClipboardViewWindow::refresh() {
  // Bump the generation BEFORE issuing the async read. Any in-flight
  // callback from a prior refresh will see gen != m_gen and skip its
  // populate — last-refresh-wins.
  const unsigned gen = ++m_gen;

  auto disp = get_display();
  if (!disp) {
    m_type_lbl.set_text("(no display)");
    m_body.get_buffer()->set_text("");
    return;
  }
  auto clip = disp->get_clipboard();
  if (!clip) {
    m_type_lbl.set_text("(no clipboard)");
    m_body.get_buffer()->set_text("");
    return;
  }

  // ── Synchronous: enumerate the MIME formats now ─────────────────────────
  // Gdk::Clipboard::get_formats() returns a ContentFormats describing every
  // representation on the clipboard right now. We use:
  //   - to_string() for the human-readable header strip (canonical
  //     whitespace-separated list of GTypes and mime types)
  //   - contain_mime_type(...) probes to decide whether to attempt the
  //     async text read
  // Avoiding get_mime_types() here: the C-side binding returns a NULL-
  // terminated char** with an out-param size, and the gtkmm wrapper shape
  // for that is awkward to use directly. to_string() + a couple of probes
  // is more idiomatic and matches what the user wants to see anyway.
  auto fmts = clip->get_formats();

  bool has_text = false;
  bool has_uri_list = false;
  Glib::ustring fmts_str;

  if (fmts) {
    fmts_str = fmts->to_string();
    has_text = fmts->contain_mime_type("text/plain") ||
               fmts->contain_mime_type("text/plain;charset=utf-8") ||
               fmts->contain_mime_type("UTF8_STRING") ||
               fmts->contain_mime_type("STRING");
    has_uri_list = fmts->contain_mime_type("text/uri-list");
  }

  // Build the header text. Type line, then URL line.
  // The URL line is always present for consistency — "(none)" when the
  // clipboard doesn't expose a uri-list representation. (When it does, the
  // URL itself shows up in the body via text/uri-list, which reads as
  // newline-separated URIs.)
  Glib::ustring header;
  if (fmts_str.empty()) {
    header = "Type: (clipboard empty)\nURL: (none)";
  } else {
    header = "Type: " + fmts_str + "\nURL: ";
    header += has_uri_list ? "(see body — text/uri-list)" : "(none)";
  }
  m_type_lbl.set_text(header);

  // ── Body: text-or-bust ──────────────────────────────────────────────────
  // If the clipboard has any text-shaped representation, async-read it and
  // fill the body when the callback fires. Otherwise the body stays empty
  // — the header tells the user what's there, but we're explicit that we
  // don't render non-text content.
  if (!has_text && !has_uri_list) {
    m_body.get_buffer()->set_text("(non-text clipboard — header above shows "
                                  "the MIME types)");
    return;
  }

  // Placeholder while the async read is in flight. For tiny clips this
  // flashes briefly; for large ones the user sees it long enough to know
  // something's happening.
  m_body.get_buffer()->set_text("(reading…)");

  // Async read. Capture `this` and `gen` — the gen check inside the
  // callback guards against:
  //   - the window being hidden before the read completes (gen still
  //     matches, we populate; harmless — user's next show will refresh
  //     anyway, and populating a hidden buffer is fine)
  //   - a *newer* refresh having been issued (gen no longer matches,
  //     skip)
  // We do NOT use a weak-ref because the window is owned by MainWindow's
  // unique_ptr and outlives any plausible async read. If MainWindow tears
  // down mid-read the entire app is going down anyway.
  clip->read_text_async(
      [this, gen, clip](Glib::RefPtr<Gio::AsyncResult>& res) {
        if (gen != m_gen) {
          // Superseded by a newer refresh. Nothing to do.
          return;
        }
        try {
          Glib::ustring text = clip->read_text_finish(res);
          if (text.empty()) {
            m_body.get_buffer()->set_text("(empty)");
          } else {
            m_body.get_buffer()->set_text(text);
          }
        } catch (const Glib::Error& e) {
          // read_text_finish throws on cancellation or transport error.
          // Surface it in the body so the user can tell the difference
          // between "empty clipboard" and "read failed".
          m_body.get_buffer()->set_text(
              Glib::ustring("(read failed: ") + e.what() + ")");
        }
      });
}

} // namespace Curvz
