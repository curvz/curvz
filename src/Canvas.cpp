#include "Canvas.hpp"
#include "CommandHistory.hpp"
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"  // s116 m6 — m_project field reads workspace appearance
#include "CurvzSpinButton.hpp"
#include "MacroSystem.hpp"
#include "SvgParser.hpp"
#include "curvz_utils.hpp"  // S97 m2 — box_blur_argb32 for drop-shadow render
#include "color/SwatchLibrary.hpp"  // set_swatch_library + apply_swatch_to_selection
#include "color/FillStyleInterop.hpp"  // to_fillstyle — live-recolour walk (s70 M3)
#include "style/StyleInterop.hpp"  // mutate_appearance funnel for user-driven fill/stroke writes
#include "style/StyleLibrary.hpp"  // set_style_library + signal_style_changed (S78 m3d)
#include <filesystem>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/clipboard.h>
#include <gdkmm/contentprovider.h>
#include <gdkmm/pixbuf.h>
#include <gdkmm/rectangle.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/grid.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/window.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-font.h>
#include <sstream>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>  // s125 m1c — uint8_t for PNG IHDR parser
#include <cstring>  // s125 m1c — std::memcmp for PNG signature check
#include <ctime>    // s125 m1c — strftime/localtime for mtime in info dialog
#include <fstream>  // s125 m1c — std::ifstream for IHDR parser
#include <functional>
#include <glib.h> // g_uuid_string_random via generate_internal_id()
#include <glibmm/main.h>
#include <gtkmm/gestureclick.h>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Curvz {

// ── static id counter
// ─────────────────────────────────────────────────────────
static int s_next_id = 1;
static int s_next_id_pen = 100; // separate range for pen paths
static std::string s_last_iid;  // set by next_id(), grabbed by caller
static std::string next_id() {
  s_last_iid = generate_internal_id();
  return "obj" + std::to_string(s_next_id++);
}
static const std::string &last_iid() { return s_last_iid; }

// ── Layer colour helper
// ─────────────────────────────────────────────────────── Parses "#rrggbb" into
// [0,1] doubles. Returns false and leaves r/g/b unchanged if the string is
// empty or malformed.
static bool parse_layer_color(const std::string &hex, double &r, double &g,
                              double &b) {
  if (hex.size() != 7 || hex[0] != '#')
    return false;
  auto h = [&](int i) { return std::stoi(hex.substr(i, 2), nullptr, 16); };
  r = h(1) / 255.0;
  g = h(3) / 255.0;
  b = h(5) / 255.0;
  return true;
}

// ── Macro recording helper
// ──────────────────────────────────────────────────── Call at every undoable
// commit point. No-op when not recording.
static void record_step_if_recording(MacroStep step) {
  if (MacroManager::instance().is_recording())
    MacroManager::instance().record_step(std::move(step));
}

// ── Image metadata helper (s125 m1c)
// ───────────────────────────────────────────── Reads pixel dimensions, colour
// depth, alpha presence, and format from an image file on disk. Used by both
// import_image_to_canvas (to size the canvas to natural dims when fitting)
// and the right-click Image Info dialog. PNG colour-depth comes from a
// minimal IHDR parse (libpng dependency avoided — IHDR is at a fixed offset
// and we only need bit-depth + colour-type bytes); other formats fall back to
// Gdk::Pixbuf metadata which normalises to 8-bit RGB(A) on load. The Pixbuf
// path is therefore correct for "what got loaded" but may understate the
// source file's true depth for non-PNGs. Acceptable for an info dialog.
struct ImageMeta {
  int width = 0;        // pixels
  int height = 0;       // pixels
  std::string depth;    // human-readable colour depth, e.g. "8-bit RGBA",
                        // "16-bit RGB", "8-bit Indexed". Empty when unknown.
  std::string format;   // "PNG", "JPEG", "GIF", "WebP", or "" if unrecognised
  uint64_t file_size = 0;  // bytes, 0 if unreadable
  bool valid = false;   // true iff width/height were read successfully
};

// PNG IHDR colour-type byte → human-readable channel layout.
// PNG spec table 11.1: 0=Gray, 2=RGB, 3=Indexed, 4=Gray+A, 6=RGBA.
static std::string png_color_type_str(uint8_t ct) {
  switch (ct) {
    case 0: return "Gray";
    case 2: return "RGB";
    case 3: return "Indexed";
    case 4: return "Gray+A";
    case 6: return "RGBA";
    default: return "?";
  }
}

static ImageMeta read_image_meta(const std::string &path) {
  ImageMeta m;

  // File size — cheap, do this regardless of format.
  std::error_code ec;
  auto fsz = std::filesystem::file_size(path, ec);
  if (!ec)
    m.file_size = static_cast<uint64_t>(fsz);

  // Format from extension. Used for both the format string and to gate the
  // PNG IHDR fast-path.
  std::string ext;
  auto dot = path.rfind('.');
  if (dot != std::string::npos) {
    ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  }
  if (ext == "png") m.format = "PNG";
  else if (ext == "jpg" || ext == "jpeg") m.format = "JPEG";
  else if (ext == "gif") m.format = "GIF";
  else if (ext == "webp") m.format = "WebP";

  // PNG fast-path: read IHDR for accurate colour depth even for 16-bit /
  // indexed / grayscale variants that Pixbuf would normalise away.
  if (ext == "png") {
    std::ifstream f(path, std::ios::binary);
    if (f) {
      // PNG signature (8 bytes) + IHDR length (4) + "IHDR" (4) + width (4)
      // + height (4) + bit depth (1) + colour type (1) = 26 bytes minimum.
      uint8_t buf[26];
      f.read(reinterpret_cast<char *>(buf), sizeof(buf));
      static const uint8_t png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
      if (f.gcount() == sizeof(buf) &&
          std::memcmp(buf, png_sig, 8) == 0 &&
          buf[12] == 'I' && buf[13] == 'H' &&
          buf[14] == 'D' && buf[15] == 'R') {
        // Big-endian uint32. Cast each byte to uint32_t before shifting so
        // we don't promote to int and risk shifting into the sign bit on
        // pathologically-large (≥2^30 px) PNGs.
        auto be32 = [&](int off) -> uint32_t {
          return (uint32_t(buf[off]) << 24) | (uint32_t(buf[off + 1]) << 16) |
                 (uint32_t(buf[off + 2]) << 8) | uint32_t(buf[off + 3]);
        };
        m.width = static_cast<int>(be32(16));
        m.height = static_cast<int>(be32(20));
        uint8_t bit_depth = buf[24];
        uint8_t color_type = buf[25];
        m.depth = std::to_string(bit_depth) + "-bit " +
                  png_color_type_str(color_type);
        m.valid = (m.width > 0 && m.height > 0);
        return m;
      }
      // Header malformed — fall through to Pixbuf.
    }
  }

  // Generic path: Gdk::Pixbuf reads everything we care about. bits_per_sample
  // × n_channels gives a reasonable depth string post-normalisation.
  try {
    auto pb = Gdk::Pixbuf::create_from_file(path);
    if (pb) {
      m.width = pb->get_width();
      m.height = pb->get_height();
      int bps = pb->get_bits_per_sample();
      int nch = pb->get_n_channels();
      bool has_alpha = pb->get_has_alpha();
      // Build a label like "8-bit RGB" / "8-bit RGBA" / "8-bit Gray".
      // Pixbuf reports bits per *sample* (per channel), not total. nch=3 RGB
      // sans alpha, nch=4 RGB with alpha, nch=1 grayscale, nch=2 gray+alpha.
      const char *layout = "?";
      if (nch == 1) layout = "Gray";
      else if (nch == 2) layout = "Gray+A";
      else if (nch == 3) layout = "RGB";
      else if (nch == 4) layout = has_alpha ? "RGBA" : "RGBX";
      m.depth = std::to_string(bps) + "-bit " + layout;
      m.valid = (m.width > 0 && m.height > 0);
    }
  } catch (...) {
    // m.valid stays false
  }

  return m;
}

// Pretty-print bytes — same scale used in the Image Info dialog.
static std::string format_file_size(uint64_t bytes) {
  if (bytes == 0) return "unknown";
  if (bytes >= 1024 * 1024)
    return std::to_string(bytes / (1024 * 1024)) + " MB";
  if (bytes >= 1024)
    return std::to_string(bytes / 1024) + " KB";
  return std::to_string(bytes) + " B";
}

// ── Constructor
// ───────────────────────────────────────────────────────────────
Canvas::Canvas() {
  curvz::utils::set_name(*this, "cv", "canvas_drawing_area");
  set_expand(true);
  set_draw_func(sigc::mem_fun(*this, &Canvas::on_draw));
  set_focusable(true);

  // Scroll → pan (both axes); Ctrl+scroll → zoom
  m_scroll_ctrl = Gtk::EventControllerScroll::create();
  m_scroll_ctrl->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES |
                           Gtk::EventControllerScroll::Flags::KINETIC);
  m_scroll_ctrl->signal_scroll().connect(
      sigc::mem_fun(*this, &Canvas::on_scroll), false);
  add_controller(m_scroll_ctrl);

  // Pinch → zoom
  auto zoom_gest = Gtk::GestureZoom::create();
  zoom_gest->signal_scale_changed().connect([this, zoom_gest](double scale) {
    // scale is cumulative from gesture begin — compute delta from last frame
    double delta = scale / m_pinch_last_scale;
    m_pinch_last_scale = scale;
    // Zoom toward gesture centroid
    double cx = 0.0, cy = 0.0;
    zoom_gest->get_bounding_box_center(cx, cy);
    zoom_toward(cx, cy, delta);
  });
  zoom_gest->signal_begin().connect(
      [this](Gdk::EventSequence *) { m_pinch_last_scale = 1.0; });
  add_controller(zoom_gest);

  // Middle-drag → pan
  auto pan_drag = Gtk::GestureDrag::create();
  pan_drag->set_button(2);
  pan_drag->signal_drag_begin().connect(
      sigc::mem_fun(*this, &Canvas::on_pan_begin));
  pan_drag->signal_drag_update().connect(
      sigc::mem_fun(*this, &Canvas::on_pan_update));
  pan_drag->signal_drag_end().connect(
      sigc::mem_fun(*this, &Canvas::on_pan_end));
  add_controller(pan_drag);

  // Left-drag → draw shapes / move selection
  auto draw_drag = Gtk::GestureDrag::create();
  draw_drag->set_button(1);
  draw_drag->signal_drag_begin().connect([this, draw_drag](double x, double y) {
    // Read modifier state from the gesture's current event
    auto evt = draw_drag->get_current_event();
    if (evt) {
      auto state = evt->get_modifier_state();
      m_mod_alt = (state & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
      m_mod_shift =
          (state & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
      m_mod_ctrl =
          (state & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
    }
    on_draw_begin(x, y);
  });
  draw_drag->signal_drag_update().connect([this, draw_drag](double dx,
                                                            double dy) {
    auto evt = draw_drag->get_current_event();
    if (evt) {
      auto state = evt->get_modifier_state();
      m_mod_alt = (state & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
      m_mod_shift =
          (state & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
      m_mod_ctrl =
          (state & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
    }
    on_draw_update(dx, dy);
  });
  draw_drag->signal_drag_end().connect([this, draw_drag](double dx, double dy) {
    // Do NOT re-read modifier state from the drag-end event.
    // Button-release events on most platforms clear Alt before the event
    // is delivered, so get_modifier_state() here returns 0 even when the
    // user is holding Alt.  The correct value was already captured by the
    // last drag_update or on_motion call — leave m_mod_alt as-is.
    on_draw_end(dx, dy);
  });
  add_controller(draw_drag);

  // Double-click detector — re-enters text edit when a Text node is
  // double-clicked while the Selection tool is active.
  auto dbl_click = Gtk::GestureClick::create();
  dbl_click->set_button(1);
  dbl_click->signal_pressed().connect([this, dbl_click](int n_press, double x,
                                                        double y) {
    if (n_press != 2)
      return;
    if (m_tool != ActiveTool::Selection)
      return;
    double dx, dy;
    screen_to_doc(x, y, dx, dy);
    SceneNode *hit = hit_test(dx, dy);
    if (hit && hit->is_text()) {
      m_sig_request_tool.emit(ActiveTool::Text);
      // Defer on_text_begin until after tool switch has processed.
      Glib::signal_idle().connect_once([this, x, y]() { on_text_begin(x, y); });
    }
  });
  add_controller(dbl_click);

  // Right-click context menu. Three branches by hit type:
  //   • Blend (or one of its A/B/cache children) → "Rebuild Blend Steps"
  //     popover (the original branch).
  //   • Image → modal "Image Info" dialog with file/pixel/size details
  //     (the s124-era branch).
  //   • Anything else hit-testable (path, text, group, compound, ref) →
  //     general object context menu, currently one entry: "Save to
  //     Library…" (s125 m1a). Future entries (Group, Cut/Copy/Paste,
  //     Bring Forward, …) append in that branch.
  // Empty-canvas right-click is intentionally a no-op (no document-level
  // verbs wired yet).
  auto rclick = Gtk::GestureClick::create();
  rclick->set_button(3);
  rclick->signal_pressed().connect([this](int, double x, double y) {
    if (!m_doc)
      return;
    // TextOnPath tool intercepts right-click for detach
    if (m_tool == ActiveTool::TextOnPath) {
      on_top_rclick(x, y);
      return;
    }
    double dx, dy;
    screen_to_doc(x, y, dx, dy);
    SceneNode *hit = hit_test(dx, dy);
    // Blend right-click: show a small popover with "Rebuild steps".
    // Owns the Blend whose A or B was hit, or the Blend itself when
    // hit_test returns it directly (unlikely — Blend isn't hit-testable
    // as a whole, just its A/B/cache children). We walk up via
    // find_blend_owner — if the hit is A or B, that returns the Blend.
    // If the hit is a cached step (which find_blend_owner doesn't cover),
    // we fall back to scanning all Blends for one whose cache contains
    // the hit pointer.
    if (hit) {
      SceneNode *blend = nullptr;
      if (hit->is_blend()) {
        blend = hit;
      } else if (auto *owner = find_blend_owner(hit)) {
        blend = owner;
      } else {
        // Could be a cache step — walk doc looking for a Blend whose
        // cache owns `hit`. Rare path; cheap walk.
        std::function<SceneNode *(SceneNode *)> find =
            [&](SceneNode *n) -> SceneNode * {
          if (!n)
            return nullptr;
          if (n->is_blend()) {
            for (auto &s : n->blend_cache)
              if (s.get() == hit)
                return n;
          }
          for (auto &c : n->children)
            if (auto *r = find(c.get()))
              return r;
          if (n->is_blend()) {
            if (auto *r = find(n->blend_source_a.get()))
              return r;
            if (auto *r = find(n->blend_source_b.get()))
              return r;
          }
          if (n->is_warp()) {
            if (auto *r = find(n->warp_source.get()))
              return r;
            if (auto *r = find(n->warp_glyph_cache.get()))
              return r;
            if (auto *r = find(n->warp_cache.get()))
              return r;
          }
          return nullptr;
        };
        if (m_doc)
          for (auto &layer : m_doc->layers)
            if (auto *r = find(layer.get())) {
              blend = r;
              break;
            }
      }
      if (blend) {
        auto *popover = Gtk::make_managed<Gtk::Popover>();
        popover->set_parent(*this);
        popover->set_has_arrow(true);
        auto *box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        box->set_spacing(2);
        box->set_margin_start(4);
        box->set_margin_end(4);
        box->set_margin_top(4);
        box->set_margin_bottom(4);
        auto *btn = Gtk::make_managed<Gtk::Button>("Rebuild Blend Steps");
        btn->set_has_frame(false);
        btn->signal_clicked().connect([this, blend, popover]() {
          rebuild_blend(blend);
          popover->popdown();
        });
        box->append(*btn);
        popover->set_child(*box);
        Gdk::Rectangle rect((int)x, (int)y, 1, 1);
        popover->set_pointing_to(rect);
        popover->popup();
        return;
      }
    }
    if (!hit) {
      // Right-click on empty canvas — no menu. Affinity/Illustrator convention:
      // an empty-canvas context menu would only carry document-level verbs
      // (paste, etc.), and we don't have those wired yet. Add later if needed.
      return;
    }
    if (!hit->is_image()) {
      // s125 m1a: general object context menu — paths, text, groups,
      // compounds, refs. Single entry today: "Save to Library…". Future
      // entries (Group, Cut/Copy/Paste, Bring Forward, …) append here.
      // Image is excluded because it has its own info-dialog branch below;
      // Blend is excluded because the Blend branch above already returned.
      //
      // If the hit isn't already in the selection, select-then-show — matches
      // Illustrator/Affinity. Otherwise leave the selection as-is so a
      // multi-selection can be saved as one library item.
      if (!is_selected(hit)) {
        set_selection_single(hit);
        m_sig_selection.emit(nullptr);  // notify inspector / sidebar
        queue_draw();
      }

      auto *popover = Gtk::make_managed<Gtk::Popover>();
      popover->set_parent(*this);
      popover->set_has_arrow(true);
      auto *box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
      box->set_spacing(2);
      box->set_margin_start(4);
      box->set_margin_end(4);
      box->set_margin_top(4);
      box->set_margin_bottom(4);

      auto *btn_save = Gtk::make_managed<Gtk::Button>("Save to Library…");
      btn_save->set_has_frame(false);
      // Left-align the label so it reads as a menu entry, not a centred
      // button. The Blend popover above uses a single centred button (one
      // entry, button-like) — this menu is a list of entries even though
      // it has just one today, so left-align is the right idiom.
      if (auto *child = btn_save->get_child()) {
        if (auto *lbl = dynamic_cast<Gtk::Label *>(child)) {
          lbl->set_xalign(0.0f);
          lbl->set_hexpand(true);
        }
      }
      btn_save->signal_clicked().connect([this, popover]() {
        popover->popdown();
        // Defer the signal emission so the popover finishes dismissing
        // before MainWindow opens the folder-picker dialog. Without this,
        // the popover popdown and dialog modal-show race in a way that
        // sometimes leaves the popover frame painted under the dialog.
        Glib::signal_idle().connect_once([this]() {
          m_sig_request_save_to_library.emit();
        });
      });
      box->append(*btn_save);

      popover->set_child(*box);
      Gdk::Rectangle rect((int)x, (int)y, 1, 1);
      popover->set_pointing_to(rect);
      popover->popup();
      return;
    }

    // Image branch — info dialog. Reaching here means hit is non-null and
    // is_image() is true (gated above).
    //
    // s125 m1g: replaced Gtk::AlertDialog with a Curvz-themed Gtk::Window.
    // AlertDialog is a system-level alert primitive — it paints in the OS
    // theme (so dark when the app is in lightmode), and its body text is
    // a non-selectable Pango string. Both wrong for an info dialog: the
    // user wants to read it in the app's chosen theme, and to copy the
    // path/filename out into other tools. This Gtk::Window inherits the
    // app motif via apply_motif_class_from_parent, and each value is a
    // Gtk::Label with set_selectable(true).

    // Filename (last path component)
    std::string full_path = hit->image_path;
    std::string fname = full_path;
    auto slash = full_path.rfind('/');
    if (slash != std::string::npos)
      fname = full_path.substr(slash + 1);

    // s125 m1c: read pixel dimensions, colour depth, format, file size via
    // the shared image_meta helper. Same source-of-truth as the importer,
    // so what the user sees in the info dialog matches what was loaded.
    ImageMeta meta = read_image_meta(full_path);
    std::string size_str = format_file_size(meta.file_size);

    // Modified time — best-effort; missing or unreadable file gets "unknown".
    std::string mtime_str = "unknown";
    {
      std::error_code mec;
      auto ftime = std::filesystem::last_write_time(full_path, mec);
      if (!mec) {
        // file_time_type → system_clock isn't portable in C++17 the strict way,
        // but std::chrono::clock_cast lands in C++20. The widely-supported
        // hack is to subtract clocks' "now" deltas. We just want a date string
        // for a human; second-resolution drift between mounts is fine.
        auto sys_time = std::chrono::system_clock::now() +
                        (ftime - std::filesystem::file_time_type::clock::now());
        std::time_t tt = std::chrono::system_clock::to_time_t(sys_time);
        std::tm tm_local{};
#ifdef _WIN32
        localtime_s(&tm_local, &tt);
#else
        localtime_r(&tt, &tm_local);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_local);
        mtime_str = buf;
      }
    }

    // Canvas (placed) size in doc units — what the image actually occupies
    // in the document, distinct from its source pixel dimensions.
    char canvas_size[64];
    snprintf(canvas_size, sizeof(canvas_size), "%.1f × %.1f", hit->image_w,
             hit->image_h);

    // Find parent window — needed for transient_for + motif inheritance.
    Gtk::Window *win = dynamic_cast<Gtk::Window *>(get_root());
    if (!win)
      return;

    auto *dlg = new Gtk::Window();
    curvz::utils::set_name(dlg, "dlg_imginfo", "image_info_dialog_root");
    dlg->set_title("Image Info");
    dlg->set_transient_for(*win);
    curvz::utils::apply_motif_class_from_parent(*dlg, *win);
    dlg->set_modal(true);
    dlg->set_resizable(false);
    // s125 m1j: with read-only Entry values (no wrap), the measure pass is
    // stable — no width-for-height feedback loop. The earlier (m1g) attempt
    // at set_default_size + set_resizable(false) + wrapping labels caused
    // continuous "needs at least N" warnings on hover. This is fine now.
    dlg->set_default_size(480, -1);

    auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    auto *grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(12);
    grid->set_margin(16);
    grid->set_margin_bottom(8);

    // s125 m1j: value column is a read-only Gtk::Entry, not a selectable
    // Gtk::Label. The Label approach (m1g/m1h) produced a "Trying to
    // measure dlg_imginfo for height of N, but it needs at least M"
    // warning storm on hover — selectable+wrapping labels in a non-fixed
    // container have an unstable measure cycle, and tooltip hover-timeout
    // re-measurements hit it on every hover. Entries don't wrap, scroll
    // horizontally for long values, and have full keyboard selection
    // (double-click word, triple-click all, Ctrl+A, Ctrl+C). They also
    // visually signal "this is data you can interact with" better than
    // a static label, which is what we want here.
    int row = 0;
    auto add_row = [&](const char *name, const std::string &value) {
      auto *lbl_name = Gtk::make_managed<Gtk::Label>(name);
      lbl_name->set_halign(Gtk::Align::END);
      lbl_name->set_valign(Gtk::Align::CENTER);
      lbl_name->add_css_class("dim-label");  // GTK4 standard dim style

      auto *ent_val = Gtk::make_managed<Gtk::Entry>();
      ent_val->set_text(value);
      ent_val->set_editable(false);
      ent_val->set_can_focus(true);  // need focus for keyboard selection
      ent_val->set_hexpand(true);
      // Frameless visual treatment — looks more like a value display
      // than a text-entry field, while keeping all the selection
      // affordances. The .flat CSS class is GTK4 standard for
      // "show me as transparent / no border".
      ent_val->add_css_class("flat");
      // Width hint — typical fields fit comfortably; long paths
      // scroll horizontally rather than blowing out the dialog.
      ent_val->set_width_chars(48);

      grid->attach(*lbl_name, 0, row, 1, 1);
      grid->attach(*ent_val, 1, row, 1, 1);
      ++row;
    };

    add_row("Name", fname);
    add_row("Path", full_path);
    add_row("Pixels",
            meta.valid ? std::to_string(meta.width) + " × " +
                             std::to_string(meta.height)
                       : "unknown");
    if (!meta.format.empty())
      add_row("Format", meta.format);
    if (!meta.depth.empty())
      add_row("Depth", meta.depth);
    add_row("File size", size_str);
    add_row("Modified", mtime_str);
    add_row("Placed", std::string(canvas_size) + " doc units");
    add_row("Linkage", "External file (not embedded)");

    outer->append(*grid);

    // Close button row — right-aligned, takes initial focus so the
    // selectable labels don't get a focus ring on open.
    auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    btn_row->set_margin_start(16);
    btn_row->set_margin_end(16);
    btn_row->set_margin_bottom(12);
    btn_row->set_halign(Gtk::Align::END);
    auto *btn_close = Gtk::make_managed<Gtk::Button>("Close");
    curvz::utils::set_name(btn_close, "dlg_imginfo_close",
                           "image_info_dialog_close_btn");
    btn_close->signal_clicked().connect([dlg]() { dlg->close(); });
    btn_row->append(*btn_close);
    outer->append(*btn_row);

    dlg->set_child(*outer);

    // Manage lifetime: delete on hide. Matches the Rotate-from-Point dialog
    // pattern earlier in this file — heap-allocated, self-deletes on hide.
    dlg->signal_hide().connect([dlg]() { delete dlg; });

    // Enter activates Close (default widget). GTK4 doesn't auto-bind Esc
    // for transient windows; the X button + Close button cover dismissal.
    btn_close->set_receives_default(true);
    dlg->set_default_widget(*btn_close);

    dlg->present();
    // s125 m1h: grab focus AFTER present(), and defer through signal_idle so
    // we run after GTK4's initial focus-traversal walk. Without the defer,
    // selectable labels claim focus during the first arrange pass and our
    // grab_focus() is silently overridden — the dialog opens with a
    // selection cursor on the first value field instead of the Close
    // button. Calling set_focus on the window plus a deferred grab_focus
    // covers both the window-level focus and the widget-level focus state.
    // The idle fires on the next event-loop iteration, before any user
    // input can reach the dialog, so btn_close is guaranteed alive.
    dlg->set_focus(*btn_close);
    Glib::signal_idle().connect_once(
        [btn_close]() { btn_close->grab_focus(); });
  });
  add_controller(rclick);

  // Motion → cursor pos + rubber band
  auto motion = Gtk::EventControllerMotion::create();
  motion->signal_motion().connect(sigc::mem_fun(*this, &Canvas::on_motion));
  add_controller(motion);

  // Alt is forwarded by MainWindow::notify_alt_pressed/released — no local
  // key controller needed (it would require canvas focus which is unreliable).

  LOG_DEBUG("Canvas created");
}

// ── Public setters
// ────────────────────────────────────────────────────────────
void Canvas::set_document(CurvzDocument *doc) {
  m_doc = doc;
  m_selected = nullptr;
  m_selection.clear();
  m_node_selection.clear();
  m_selected_node = -1;
  m_corner_selection.clear();
  m_ref_hovered = nullptr;
  m_alt_drag_dup = false;
  m_pan_x = 0.0;
  m_pan_y = 0.0;
  m_fit_pending = true; // defer fit until first draw when widget is sized
  queue_draw();
}

void Canvas::set_zoom(double zoom) {
  double mn = fit_zoom() * 0.01;
  mn = std::max(mn, MIN_ZOOM);
  m_zoom = std::clamp(zoom, mn, MAX_ZOOM);
  clamp_pan();
  m_sig_zoom.emit(m_zoom);
  queue_draw();
}

void Canvas::set_history(CommandHistory *history) { m_history = history; }

// ── Live-recolour walker (s70 M3) ────────────────────────────────────────────
//
// Descends the whole scene under `node` and, for every SceneNode whose
// fill_swatch_id / stroke_swatch_id matches `changed_id`, refreshes its
// cached FillStyle from the library's current swatch colour. Binding
// itself is unchanged — M1 already guaranteed id preservation; M3 only
// refreshes the render cache so the screen reflects the edit without a
// save/reload.
//
// Walk shape mirrors collect_paths (Canvas.cpp) and the scene-walker
// family more generally — descend into children, clip_shape, blend
// sources + cache, and warp_source + warp_glyph_cache + warp_cache.
// Touching caches keeps this frame consistent; any subsequent dirty-
// flag-driven regen will recompute them cleanly from the (now
// refreshed) sources.
static void live_recolour_walk(SceneNode* node,
                               const color::SwatchId& changed_id,
                               const color::SwatchLibrary& lib) {
  if (!node) return;

  // Refresh this node's cached FillStyle(s) if bound to the changed id.
  // Re-resolution goes through to_fillstyle with the library — same path
  // set_paint uses — so the Paint-with-swatch case lands on find_swatch
  // and writes a concrete Solid. Dead-ref degrades to the old fallback,
  // which is acceptable here (an edit to a deleted swatch is nonsense
  // anyway; M4 will prevent it).
  if (node->fill_swatch_id == changed_id) {
    color::Paint p = color::SwatchRef{ changed_id,
                                       color::Color{ node->fill.r, node->fill.g,
                                                     node->fill.b, node->fill.a } };
    node->fill = color::to_fillstyle(p, lib);
    // S106 m1 — fill colour just changed via swatch live-recolour.
    // No-op if not a gradient fill, so safe on solid swatch binds.
    mark_gradient_cache_dirty(*node);
  }
  if (node->stroke_swatch_id == changed_id) {
    color::Paint p = color::SwatchRef{ changed_id,
                                       color::Color{ node->stroke.paint.r,
                                                     node->stroke.paint.g,
                                                     node->stroke.paint.b,
                                                     node->stroke.paint.a } };
    node->stroke.paint = color::to_fillstyle(p, lib);
  }

  // Descend into every structural slot that can hold stylable nodes.
  for (auto& child : node->children)
    live_recolour_walk(child.get(), changed_id, lib);
  if (node->clip_shape)
    live_recolour_walk(node->clip_shape.get(), changed_id, lib);
  if (node->blend_source_a)
    live_recolour_walk(node->blend_source_a.get(), changed_id, lib);
  if (node->blend_source_b)
    live_recolour_walk(node->blend_source_b.get(), changed_id, lib);
  for (auto& step : node->blend_cache)
    live_recolour_walk(step.get(), changed_id, lib);
  if (node->warp_source)
    live_recolour_walk(node->warp_source.get(), changed_id, lib);
  if (node->warp_glyph_cache)
    live_recolour_walk(node->warp_glyph_cache.get(), changed_id, lib);
  if (node->warp_cache)
    live_recolour_walk(node->warp_cache.get(), changed_id, lib);
}

// ── unbind_swatch_walk (S83 m4h v4) ──────────────────────────────────────────
// Sibling to live_recolour_walk — same shape, different action. Walks the
// scene tree clearing fill_swatch_id / stroke_swatch_id on every node bound
// to the given id. The cached FillStyle (object's actual colour) is
// deliberately NOT touched — break-on-override v1 says the moment-of-unbind
// appearance is what the user keeps. This mirrors UnbindSwatchCommand's
// execute() shape and the manual ×-button unbind path in PropertiesPanel.
//
// Used by Canvas::unbind_swatch_from_doc, called from SwatchesPanel's
// delete-swatch path BEFORE remove_swatch runs. Without it,
// remove_swatch leaves dangling fill_swatch_id / stroke_swatch_id values
// on objects (the comment in remove_swatch acknowledges this and defers
// cleanup to a "delete-with-usage-check flow" — m4h v4 implements the
// no-confirm version of that flow as part of the inspector unification
// arc).
//
// Slot list MUST stay in sync with live_recolour_walk above. Any new
// structural slot added there needs adding here too.
static void unbind_swatch_walk(SceneNode* node,
                               const color::SwatchId& removed_id) {
  if (!node) return;

  if (node->fill_swatch_id == removed_id)   node->fill_swatch_id.clear();
  if (node->stroke_swatch_id == removed_id) node->stroke_swatch_id.clear();

  for (auto& child : node->children)
    unbind_swatch_walk(child.get(), removed_id);
  if (node->clip_shape)
    unbind_swatch_walk(node->clip_shape.get(), removed_id);
  if (node->blend_source_a)
    unbind_swatch_walk(node->blend_source_a.get(), removed_id);
  if (node->blend_source_b)
    unbind_swatch_walk(node->blend_source_b.get(), removed_id);
  for (auto& step : node->blend_cache)
    unbind_swatch_walk(step.get(), removed_id);
  if (node->warp_source)
    unbind_swatch_walk(node->warp_source.get(), removed_id);
  if (node->warp_glyph_cache)
    unbind_swatch_walk(node->warp_glyph_cache.get(), removed_id);
  if (node->warp_cache)
    unbind_swatch_walk(node->warp_cache.get(), removed_id);
}

void Canvas::set_swatch_library(color::SwatchLibrary *lib) {
  // Drop any live-recolour connection tied to the previous library. Bare
  // disconnect is safe on a default-constructed sigc::connection and a
  // no-op when already disconnected. Called again on project switch —
  // MainWindow invokes set_swatch_library both at boot and when a new
  // doc becomes active.
  m_library_swatch_changed_conn.disconnect();

  m_swatch_library = lib;

  if (!m_swatch_library) return;

  // Hook live-recolour. On swatch edit, walk the whole doc tree, refresh
  // every SceneNode whose binding id matches, and queue a redraw. We
  // capture `this` (Canvas outlives any individual library handoff —
  // set_swatch_library is only called while Canvas is alive) and the
  // handler re-reads m_doc / m_swatch_library on each fire so project
  // switches between connect and signal are safe.
  m_library_swatch_changed_conn =
      m_swatch_library->signal_swatch_changed().connect(
          [this](const color::SwatchId& id) {
            if (!m_doc || !m_swatch_library) return;
            for (auto& L : m_doc->layers)
              live_recolour_walk(L.get(), id, *m_swatch_library);
            queue_draw();
          });
}

// ── Canvas::set_style_library (S78 m3d) ──────────────────────────────────────
// Cross-document live-propagation wiring for styles. Symmetric to
// set_swatch_library above — same lifecycle (boot + project switch via
// MainWindow::update_all_panels), same disconnect-then-reconnect dance.
//
// On signal_style_changed(id), every SceneNode in this canvas's document
// whose bound_style == id needs its fill/stroke cache refreshed from the
// (now-mutated) library entry. Without this walk, an edit to a user
// style would only be visible after close-and-reopen.
//
// Override model: break-on-override v1. Any direct fill/stroke edit
// clears bound_style via mutate_appearance, so any node still bound at
// signal time has not been overridden — overwriting fill/stroke
// unconditionally is correct under v1. If/when sticky-override tracking
// lands (StyleOverrides per StyleInterop.hpp), the per-node lambda
// here grows to honour the override flags; the walk shape doesn't.
//
// Drop-on-miss policy: this walk does NOT clear bound_style when
// materialise_from_style returns false. The load-time walk in
// CurvzProject does that (last-persisted-truth resolves; anything
// dangling is genuinely unresolvable). The live walk is purely a cache
// refresh — auto-clearing here would race with BindStyleCommand undo
// and the m4 delete-with-usage-check dialog. A WARN log is enough; the
// only path that actually fires this with an unknown id is a stale
// signal from a library that's mid-rename, which materialise itself
// will already log.
void Canvas::set_style_library(style::StyleLibrary *lib) {
  // Drop the previous library's connection. Default-constructed
  // sigc::connection on first call is a harmless no-op disconnect.
  m_library_style_changed_conn.disconnect();

  m_style_library = lib;

  if (!m_style_library) return;

  // Hook live propagation. Same capture-this discipline as the swatch
  // counterpart — Canvas outlives any individual library handoff, and
  // the handler re-reads m_doc / m_style_library on each fire so a
  // project switch between connect and signal-fire is safe.
  m_library_style_changed_conn =
      m_style_library->signal_style_changed().connect(
          [this](const style::StyleId& changed_id) {
            if (!m_doc || !m_style_library) return;
            for (auto& L : m_doc->layers) {
              style::walk_style_bindings(L.get(),
                  [this, &changed_id](SceneNode& n) {
                    if (n.bound_style != changed_id) return;
                    style::materialise_from_style(n, *m_style_library);
                    // Dropped binding (return false) is logged inside
                    // materialise_from_style; we deliberately do NOT
                    // clear bound_style here — see policy comment above.
                  });
            }
            queue_draw();
          });
}

// ── Canvas::mint_id / last_minted_iid ────────────────────────────────────────
// Thin public wrappers over the file-local next_id() / last_iid() helpers
// so callers outside this TU can produce fresh ids without duplicating
// the generate_internal_id() / s_last_iid book-keeping.
std::string Canvas::mint_id() { return next_id(); }
std::string Canvas::last_minted_iid() { return last_iid(); }

bool Canvas::object_bbox_query(const SceneNode &obj, double &x, double &y,
                               double &w, double &h,
                               bool include_stroke) const {
  auto bb = object_bbox(obj, include_stroke);
  if (!bb)
    return false;
  x = bb->x;
  y = bb->y;
  w = bb->w;
  h = bb->h;
  return true;
}

void Canvas::set_active_tool(ActiveTool tool) {
  // Leaving pen tool — commit if enough nodes, otherwise cancel
  if (m_tool == ActiveTool::Pen && tool != ActiveTool::Pen) {
    if (m_pen_tool.has_wip && m_pen_tool.wip.nodes.size() >= 2) {
      commit_pen_path();
    } else {
      m_pen_tool.cancel();
    }
  }
  m_prev_tool = m_tool; // old tool, before we overwrite

  // Leaving Zoom — always reset toolbar icon before switching away
  if (m_tool == ActiveTool::Zoom && tool != ActiveTool::Zoom) {
    m_zoom_alt_prev = false;
    m_sig_zoom_alt.emit(false);
  }

  // Leaving Eyedropper — clear hover state so pointer doesn't dangle
  if (m_tool == ActiveTool::Eyedropper && tool != ActiveTool::Eyedropper) {
    m_eyedropper_hovered = nullptr;
  }

  // Leaving Text tool — commit any in-progress edit
  if (m_tool == ActiveTool::Text && tool != ActiveTool::Text) {
    if (m_text_editing)
      commit_text_edit();
  }

  m_tool = tool; // now set the new tool

  // Disarm any pending clip-pick — one arm = one selection-tool press.
  if (m_clip_pick_armed) {
    m_clip_pick_armed = false;
    m_clip_pick_selection.clear();
    LOG_INFO("Canvas: clip-pick disarmed on tool switch");
  }

  // Selection state management
  // Always clear secondary node selection on any tool switch
  m_selected2 = nullptr;
  m_selected_node2 = -1;
  // Clear multi-selection on tool switch — but NOT for Eyedropper (S66
  // Phase 3): the eyedropper's whole job is applying a sampled colour to
  // the current selection. Clearing it on entry makes every pick a no-op
  // against objects and only updates the Toolbar wells.
  if (tool != ActiveTool::Selection && tool != ActiveTool::Eyedropper) {
    m_selection.clear();
    m_move_snaps.clear();
    m_warp_env_move_snaps.clear();
  }

  if (tool == ActiveTool::Node && m_prev_tool != ActiveTool::Pen) {
    // Carry over a Path selection from Selection tool so the user doesn't
    // have to re-click after switching tools (S→N workflow).
    bool carried = m_selected && m_selected->type == SceneNode::Type::Path &&
                   m_selected->path && !m_selected->path->nodes.empty();
    if (carried) {
      m_selected_node = 0;
      m_node_selection.clear();
      m_node_selection.push_back({m_selected, 0});
      m_sig_selection.emit(m_selected);
      m_sig_node_changed.emit(m_selected, m_selected_node);
    } else {
      m_selected = nullptr;
      m_selected_node = -1;
      m_sig_selection.emit(nullptr);
      m_sig_node_changed.emit(nullptr, -1);
    }
  } else if (tool == ActiveTool::Selection) {
    // Entering Selection tool — clear node index, keep object selection.
    // Always emit so the inspector rebuilds from Node→Selection layout.
    m_selected_node = -1;
    // Sync m_selection with m_selected
    if (m_selected && m_selection.empty())
      m_selection.push_back(m_selected);
    else if (!m_selected)
      m_selection.clear();
    m_sig_selection.emit(m_selected);
  } else if (tool == ActiveTool::Eyedropper) {
    // S66 Phase 3 — preserve both primary and multi-selection. Node index
    // is meaningless for the eyedropper (it works on whole-object paint),
    // so clear that but leave m_selected alone. Emit so panels refresh
    // without losing the highlight.
    m_selected_node = -1;
    // Auto-sync m_selection ← m_selected when coming from a tool that
    // didn't maintain the multi-selection (e.g. Node, TextOnPath). The
    // commit loop iterates m_selection, so without this sync a click from
    // Eyedropper-after-Node would only update the Toolbar wells.
    if (m_selected && m_selection.empty())
      m_selection.push_back(m_selected);
    else if (!m_selected)
      m_selection.clear();
    m_sig_selection.emit(m_selected);
  } else if (tool != ActiveTool::Node && tool != ActiveTool::TextOnPath) {
    m_selected = nullptr;
    m_selected_node = -1;
    m_sig_selection.emit(nullptr);
    m_sig_node_changed.emit(nullptr, -1);
  }

  // Clear corner tool state when leaving it
  if (m_prev_tool == ActiveTool::Corner && tool != ActiveTool::Corner) {
    m_corner_selection.clear();
    m_corner_rubber_active = false;
    m_sig_corner_sel_changed.emit(0);
    queue_draw();
  }

  // Cursor + zoom-alt sync for the newly active tool
  switch (tool) {
  case ActiveTool::Zoom:
    set_cursor(m_mod_alt ? "zoom-out" : "zoom-in");
    // Always emit so toolbar icon is set correctly on tool entry,
    // regardless of whether alt state changed.
    m_zoom_alt_prev = m_mod_alt;
    m_sig_zoom_alt.emit(m_mod_alt);
    break;
  case ActiveTool::Eyedropper:
    set_cursor("crosshair");
    break;
  case ActiveTool::Text:
    set_cursor("text");
    break;
  case ActiveTool::Pen:
  case ActiveTool::Rect:
  case ActiveTool::Ellipse:
  case ActiveTool::Line:
    set_cursor("crosshair");
    break;
  case ActiveTool::Ruler:
    set_cursor("crosshair");
    break;
  case ActiveTool::TextOnPath:
    set_cursor("crosshair");
    break;
  case ActiveTool::Selection:
  case ActiveTool::Node:
  default:
    set_cursor("default");
    break;
  }

  // Entering Ruler tool — try to inherit a 2-node selection from Node tool
  if (tool == ActiveTool::Ruler) {
    ruler_try_inherit_node_selection();
  }

  // Leaving Ruler tool — clear ruler node refs (pointers may dangle)
  if (m_prev_tool == ActiveTool::Ruler && tool != ActiveTool::Ruler) {
    m_ruler_node_a_obj = nullptr;
    m_ruler_node_a_idx = -1;
    m_ruler_node_b_obj = nullptr;
    m_ruler_node_b_idx = -1;
    m_ruler_labels.clear();
    if (m_ruler_toast_conn.connected())
      m_ruler_toast_conn.disconnect();
    m_ruler_toast_ms = 0;
  }

  // Entering / leaving TextOnPath tool
  if (tool == ActiveTool::TextOnPath) {
    m_top_phase = 0;
    m_top_text = nullptr;
    m_top_path_node = nullptr;
    m_top_dragging = false;
    // If there's already a linked text node selected, restore phase 2
    // so the user can drag the start point immediately without re-clicking.
    if (m_selected && m_selected->is_text() &&
        !m_selected->text_path_id.empty()) {
      SceneNode *guide = top_find_path_by_id(m_selected->text_path_id);
      if (guide) {
        m_top_text = m_selected;
        m_top_path_node = guide;
        m_top_phase = 2;
        LOG_DEBUG("set_active_tool(TOP): restored phase 2 for linked text '{}'",
                  m_selected->text_path_id);
      }
    }
  }
  if (m_prev_tool == ActiveTool::TextOnPath && tool != ActiveTool::TextOnPath) {
    m_top_phase = 0;
    m_top_text = nullptr;
    m_top_path_node = nullptr;
    m_top_dragging = false;
  }

  queue_draw();
}

// ── Coordinate helpers
// ────────────────────────────────────────────────────────
double Canvas::doc_origin_x() const {
  if (!m_doc)
    return 0.0;
  return (get_width() - m_doc->canvas_width() * m_zoom) * 0.5 + m_pan_x;
}
double Canvas::doc_origin_y() const {
  if (!m_doc)
    return 0.0;
  return (get_height() - m_doc->canvas_height() * m_zoom) * 0.5 + m_pan_y;
}
void Canvas::screen_to_doc(double sx, double sy, double &dx, double &dy) const {
  dx = (sx - doc_origin_x()) / m_zoom;
  dy = (sy - doc_origin_y()) / m_zoom;
}
void Canvas::doc_to_screen(double dx, double dy, double &sx, double &sy) const {
  sx = dx * m_zoom + doc_origin_x();
  sy = dy * m_zoom + doc_origin_y();
}

double Canvas::snap(double v) const {
  // TODO: snap to document grid when grid snap system is built.
  (void)v;
  return v;
}

double Canvas::snap_x(double doc_x, double tolerance_px) const {
  if (!m_doc || !m_doc->snap.enabled)
    return doc_x;
  double best = doc_x;
  double best_d = tolerance_px;

  if (m_doc->snap.snap_guides) {
    const SceneNode *gl = m_doc->guide_layer();
    if (gl && gl->visible && !gl->locked) {
      for (const auto &child : gl->children) {
        if (!child->is_guide() || !child->guide_is_vertical())
          continue;
        double sx_guide, sy_dummy;
        doc_to_screen(child->guide_x, 0, sx_guide, sy_dummy);
        double sx_cur, sy_dummy2;
        doc_to_screen(doc_x, 0, sx_cur, sy_dummy2);
        double d = std::abs(sx_cur - sx_guide);
        if (d < best_d) {
          best_d = d;
          best = child->guide_x;
        }
      }
    }
    // Ref points snap X
    const SceneNode *rl = m_doc->ref_layer();
    if (rl && rl->visible && !rl->locked) {
      for (const auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        double sx_ref, sy_dummy;
        doc_to_screen(child->ref_x, 0, sx_ref, sy_dummy);
        double sx_cur, sy_dummy2;
        doc_to_screen(doc_x, 0, sx_cur, sy_dummy2);
        double d = std::abs(sx_cur - sx_ref);
        if (d < best_d) {
          best_d = d;
          best = child->ref_x;
        }
      }
    }
  }

  // ── Grid X ─────────────────────────────────────────────────────────────
  // Closest vertical gridline to doc_x. Arithmetic — O(1), no enumeration.
  if (m_doc->snap.snap_grid) {
    const SceneNode *grid = m_doc->grid_layer();
    if (grid && grid->visible && !grid->locked &&
        grid->grid_spacing_x >= 0.5) {
      double sx = grid->grid_spacing_x;
      double ox = grid->grid_offset_x;
      double cand = ox + std::round((doc_x - ox) / sx) * sx;
      double scx, scy_dummy;
      doc_to_screen(cand, 0, scx, scy_dummy);
      double sx_cur, sy_dummy2;
      doc_to_screen(doc_x, 0, sx_cur, sy_dummy2);
      double d = std::abs(sx_cur - scx);
      if (d < best_d) {
        best_d = d;
        best = cand;
      }
    }
  }

  // ── Margin X ───────────────────────────────────────────────────────────
  // Left edge, right edge, and column gutter dividers (if any).
  if (m_doc->snap.snap_margins) {
    const SceneNode *ml = m_doc->margin_layer();
    if (ml && ml->visible && !ml->locked) {
      const double cw = (double)m_doc->canvas_width();
      const double left  = ml->margin_left;
      const double right = cw - ml->margin_right;
      auto try_cand = [&](double cand) {
        double scx, scy_dummy;
        doc_to_screen(cand, 0, scx, scy_dummy);
        double sx_cur, sy_dummy2;
        doc_to_screen(doc_x, 0, sx_cur, sy_dummy2);
        double d = std::abs(sx_cur - scx);
        if (d < best_d) { best_d = d; best = cand; }
      };
      try_cand(left);
      try_cand(right);
      // Column gutter dividers — only if columns subdivide the inner area
      const int cols = std::max(1, ml->margin_columns);
      const double iw = right - left;
      const double gap_x = ml->margin_col_gap;
      if (cols > 1 && iw > 0) {
        const double col_w = (iw - gap_x * (cols - 1)) / cols;
        for (int c = 1; c < cols; ++c) {
          double gx_left = left + c * col_w + (c - 1) * gap_x;
          double gx_right = gx_left + gap_x;
          try_cand(gx_left);
          try_cand(gx_right);
        }
      }
    }
  }

  return best;
}

double Canvas::snap_y(double doc_y, double tolerance_px) const {
  if (!m_doc || !m_doc->snap.enabled)
    return doc_y;
  double best = doc_y;
  double best_d = tolerance_px;

  if (m_doc->snap.snap_guides) {
    const SceneNode *gl = m_doc->guide_layer();
    if (gl && gl->visible && !gl->locked) {
      for (const auto &child : gl->children) {
        if (!child->is_guide() || !child->guide_is_horizontal())
          continue;
        double sx_dummy, sy_guide;
        doc_to_screen(0, child->guide_y, sx_dummy, sy_guide);
        double sx_dummy2, sy_cur;
        doc_to_screen(0, doc_y, sx_dummy2, sy_cur);
        double d = std::abs(sy_cur - sy_guide);
        if (d < best_d) {
          best_d = d;
          best = child->guide_y;
        }
      }
    }
    // Ref points snap Y
    const SceneNode *rl = m_doc->ref_layer();
    if (rl && rl->visible && !rl->locked) {
      for (const auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        double sx_dummy, sy_ref;
        doc_to_screen(0, child->ref_y, sx_dummy, sy_ref);
        double sx_dummy2, sy_cur;
        doc_to_screen(0, doc_y, sx_dummy2, sy_cur);
        double d = std::abs(sy_cur - sy_ref);
        if (d < best_d) {
          best_d = d;
          best = child->ref_y;
        }
      }
    }
  }

  // ── Grid Y ─────────────────────────────────────────────────────────────
  if (m_doc->snap.snap_grid) {
    const SceneNode *grid = m_doc->grid_layer();
    if (grid && grid->visible && !grid->locked &&
        grid->grid_spacing_y >= 0.5) {
      double sy = grid->grid_spacing_y;
      double oy = grid->grid_offset_y;
      double cand = oy + std::round((doc_y - oy) / sy) * sy;
      double sx_dummy, scy;
      doc_to_screen(0, cand, sx_dummy, scy);
      double sx_dummy2, sy_cur;
      doc_to_screen(0, doc_y, sx_dummy2, sy_cur);
      double d = std::abs(sy_cur - scy);
      if (d < best_d) {
        best_d = d;
        best = cand;
      }
    }
  }

  // ── Margin Y ───────────────────────────────────────────────────────────
  if (m_doc->snap.snap_margins) {
    const SceneNode *ml = m_doc->margin_layer();
    if (ml && ml->visible && !ml->locked) {
      const double ch = (double)m_doc->canvas_height();
      const double top    = ml->margin_top;
      const double bottom = ch - ml->margin_bottom;
      auto try_cand = [&](double cand) {
        double sx_dummy, scy;
        doc_to_screen(0, cand, sx_dummy, scy);
        double sx_dummy2, sy_cur;
        doc_to_screen(0, doc_y, sx_dummy2, sy_cur);
        double d = std::abs(sy_cur - scy);
        if (d < best_d) { best_d = d; best = cand; }
      };
      try_cand(top);
      try_cand(bottom);
      const int rows = std::max(1, ml->margin_rows);
      const double ih = bottom - top;
      const double gap_y = ml->margin_row_gap;
      if (rows > 1 && ih > 0) {
        const double row_h = (ih - gap_y * (rows - 1)) / rows;
        for (int r = 1; r < rows; ++r) {
          double gy_top = top + r * row_h + (r - 1) * gap_y;
          double gy_bottom = gy_top + gap_y;
          try_cand(gy_top);
          try_cand(gy_bottom);
        }
      }
    }
  }

  return best;
}

// Snap a moving selection to guides and ref points.
// BBX from snapshot positions — stable across frames.
// Hysteresis: engages at ENGAGE_PX, releases at RELEASE_PX.
std::pair<double, double> Canvas::snap_move(double raw_dx, double raw_dy) {
  if (!m_doc || !m_doc->snap.enabled)
    return {raw_dx, raw_dy};
  // Any of the four snap classes (guides/refs/grid/margins) can drive a snap.
  // Short-circuit only if they're all inactive.
  if (!m_doc->snap.snap_guides && !m_doc->snap.snap_grid &&
      !m_doc->snap.snap_margins)
    return {raw_dx, raw_dy};

  const SceneNode *gl = m_doc->guide_layer();
  bool guides_active = m_doc->snap.snap_guides &&
                       (gl && gl->visible && !gl->locked);
  const SceneNode *rl = m_doc->ref_layer();
  bool refs_active = m_doc->snap.snap_guides &&
                     (rl && rl->visible && !rl->locked);
  const SceneNode *grid_l = m_doc->grid_layer();
  bool grid_active = m_doc->snap.snap_grid &&
                     (grid_l && grid_l->visible && !grid_l->locked &&
                      grid_l->grid_spacing_x >= 0.5 &&
                      grid_l->grid_spacing_y >= 0.5);
  const SceneNode *ml = m_doc->margin_layer();
  bool margins_active = m_doc->snap.snap_margins &&
                        (ml && ml->visible && !ml->locked);
  if (!guides_active && !refs_active && !grid_active && !margins_active)
    return {raw_dx, raw_dy};

  static constexpr double ENGAGE_PX = 12.0;
  static constexpr double RELEASE_PX = 20.0;

  bool found = false;
  double bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
  for (auto &snap : m_move_snaps) {
    if (!snap.obj->path)
      continue;
    auto saved = snap.obj->path->nodes;
    snap.obj->path->nodes = snap.orig_nodes;
    for (auto &nd : snap.obj->path->nodes) {
      nd.x += raw_dx;
      nd.cx1 += raw_dx;
      nd.cx2 += raw_dx;
      nd.y += raw_dy;
      nd.cy1 += raw_dy;
      nd.cy2 += raw_dy;
    }
    auto bb = object_bbox(*snap.obj, /*include_stroke=*/false);
    snap.obj->path->nodes = saved;
    if (!bb)
      continue;
    if (!found) {
      bx1 = bb->x;
      by1 = bb->y;
      bx2 = bb->x + bb->w;
      by2 = bb->y + bb->h;
      found = true;
    } else {
      bx1 = std::min(bx1, bb->x);
      by1 = std::min(by1, bb->y);
      bx2 = std::max(bx2, bb->x + bb->w);
      by2 = std::max(by2, bb->y + bb->h);
    }
  }
  // Also include text/image nodes — temporarily apply the raw delta to measure
  // snap candidates.
  for (auto &tsnap : m_text_move_snaps) {
    // Temporarily shift the node to its would-be position.
    double saved_x =
        tsnap.obj->is_image() ? tsnap.obj->image_x : tsnap.obj->text_x;
    double saved_y =
        tsnap.obj->is_image() ? tsnap.obj->image_y : tsnap.obj->text_y;
    if (tsnap.obj->is_image()) {
      tsnap.obj->image_x = tsnap.orig_x + raw_dx;
      tsnap.obj->image_y = tsnap.orig_y + raw_dy;
    } else {
      tsnap.obj->text_x = tsnap.orig_x + raw_dx;
      tsnap.obj->text_y = tsnap.orig_y + raw_dy;
    }
    auto bb = object_bbox(*tsnap.obj, false);
    if (tsnap.obj->is_image()) {
      tsnap.obj->image_x = saved_x;
      tsnap.obj->image_y = saved_y;
    } else {
      tsnap.obj->text_x = saved_x;
      tsnap.obj->text_y = saved_y;
    }
    if (!bb)
      continue;
    if (!found) {
      bx1 = bb->x;
      by1 = bb->y;
      bx2 = bb->x + bb->w;
      by2 = bb->y + bb->h;
      found = true;
    } else {
      bx1 = std::min(bx1, bb->x);
      by1 = std::min(by1, bb->y);
      bx2 = std::max(bx2, bb->x + bb->w);
      by2 = std::max(by2, bb->y + bb->h);
    }
  }
  // Refpts — treat each as a degenerate bbox at its translated position.
  // Multi-refpt selection unions to the bounding rect of all refpts, so
  // {bx1, cx, bx2} candidates correspond to leftmost / centre / rightmost.
  for (auto &rsnap : m_ref_move_snaps) {
    if (!rsnap.obj || !rsnap.obj->is_ref())
      continue;
    double px = rsnap.orig_x + raw_dx;
    double py = rsnap.orig_y + raw_dy;
    if (!found) {
      bx1 = bx2 = px;
      by1 = by2 = py;
      found = true;
    } else {
      bx1 = std::min(bx1, px);
      by1 = std::min(by1, py);
      bx2 = std::max(bx2, px);
      by2 = std::max(by2, py);
    }
  }
  if (!found)
    return {raw_dx, raw_dy};
  double cx = (bx1 + bx2) * 0.5, cy = (by1 + by2) * 0.5;

  double adj_dx = raw_dx, adj_dy = raw_dy;

  // ── X snap (vertical guides + ref X) ─────────────────────────────────
  if (m_snap_x_locked) {
    double gsx, gsy;
    doc_to_screen(m_snap_locked_x, 0, gsx, gsy);
    double best_cand = bx1, best_d = 1e9;
    for (double c : {bx1, cx, bx2}) {
      double sx, sy;
      doc_to_screen(c, 0, sx, sy);
      double d = std::abs(sx - gsx);
      if (d < best_d) {
        best_d = d;
        best_cand = c;
      }
    }
    if (best_d < RELEASE_PX)
      adj_dx = raw_dx + (m_snap_locked_x - best_cand);
    else
      m_snap_x_locked = false;
  }
  if (!m_snap_x_locked) {
    double best_d = ENGAGE_PX;
    // Guide X
    if (guides_active) {
      for (const auto &child : gl->children) {
        if (!child->is_guide() || !child->guide_is_vertical())
          continue;
        double gsx, gsy;
        doc_to_screen(child->guide_x, 0, gsx, gsy);
        for (double c : {bx1, cx, bx2}) {
          double sx, sy;
          doc_to_screen(c, 0, sx, sy);
          double d = std::abs(sx - gsx);
          if (d < best_d) {
            best_d = d;
            m_snap_x_locked = true;
            m_snap_locked_x = child->guide_x;
            adj_dx = raw_dx + (child->guide_x - c);
          }
        }
      }
    }
    // Ref X
    if (refs_active) {
      for (const auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        // Skip self when dragging refpts — a refpt in the move set
        // would otherwise snap to its own current position.
        bool is_self = false;
        for (auto &rsnap : m_ref_move_snaps)
          if (rsnap.obj == child.get()) { is_self = true; break; }
        if (is_self) continue;
        double gsx, gsy;
        doc_to_screen(child->ref_x, 0, gsx, gsy);
        for (double c : {bx1, cx, bx2}) {
          double sx, sy;
          doc_to_screen(c, 0, sx, sy);
          double d = std::abs(sx - gsx);
          if (d < best_d) {
            best_d = d;
            m_snap_x_locked = true;
            m_snap_locked_x = child->ref_x;
            adj_dx = raw_dx + (child->ref_x - c);
          }
        }
      }
    }
    // Grid X — closest vertical gridline to each candidate (arithmetic)
    if (grid_active) {
      const double sx_grid = grid_l->grid_spacing_x;
      const double ox_grid = grid_l->grid_offset_x;
      for (double c : {bx1, cx, bx2}) {
        double cand = ox_grid + std::round((c - ox_grid) / sx_grid) * sx_grid;
        double gsx, gsy;
        doc_to_screen(cand, 0, gsx, gsy);
        double sx, sy;
        doc_to_screen(c, 0, sx, sy);
        double d = std::abs(sx - gsx);
        if (d < best_d) {
          best_d = d;
          m_snap_x_locked = true;
          m_snap_locked_x = cand;
          adj_dx = raw_dx + (cand - c);
        }
      }
    }
    // Margin X — left, right, column dividers
    if (margins_active) {
      const double cw = (double)m_doc->canvas_width();
      const double left  = ml->margin_left;
      const double right = cw - ml->margin_right;
      std::vector<double> mcands{left, right};
      const int cols = std::max(1, ml->margin_columns);
      const double iw = right - left;
      const double gap_x = ml->margin_col_gap;
      if (cols > 1 && iw > 0) {
        const double col_w = (iw - gap_x * (cols - 1)) / cols;
        for (int ci = 1; ci < cols; ++ci) {
          double gx_left = left + ci * col_w + (ci - 1) * gap_x;
          mcands.push_back(gx_left);
          mcands.push_back(gx_left + gap_x);
        }
      }
      for (double cand : mcands) {
        double gsx, gsy;
        doc_to_screen(cand, 0, gsx, gsy);
        for (double c : {bx1, cx, bx2}) {
          double sx, sy;
          doc_to_screen(c, 0, sx, sy);
          double d = std::abs(sx - gsx);
          if (d < best_d) {
            best_d = d;
            m_snap_x_locked = true;
            m_snap_locked_x = cand;
            adj_dx = raw_dx + (cand - c);
          }
        }
      }
    }
  }

  // ── Y snap (horizontal guides + ref Y) ───────────────────────────────
  if (m_snap_y_locked) {
    double gsx, gsy;
    doc_to_screen(0, m_snap_locked_y, gsx, gsy);
    double best_cand = by1, best_d = 1e9;
    for (double c : {by1, cy, by2}) {
      double sx, sy;
      doc_to_screen(0, c, sx, sy);
      double d = std::abs(sy - gsy);
      if (d < best_d) {
        best_d = d;
        best_cand = c;
      }
    }
    if (best_d < RELEASE_PX)
      adj_dy = raw_dy + (m_snap_locked_y - best_cand);
    else
      m_snap_y_locked = false;
  }
  if (!m_snap_y_locked) {
    double best_d = ENGAGE_PX;
    // Guide Y
    if (guides_active) {
      for (const auto &child : gl->children) {
        if (!child->is_guide() || !child->guide_is_horizontal())
          continue;
        double gsx, gsy;
        doc_to_screen(0, child->guide_y, gsx, gsy);
        for (double c : {by1, cy, by2}) {
          double sx, sy;
          doc_to_screen(0, c, sx, sy);
          double d = std::abs(sy - gsy);
          if (d < best_d) {
            best_d = d;
            m_snap_y_locked = true;
            m_snap_locked_y = child->guide_y;
            adj_dy = raw_dy + (child->guide_y - c);
          }
        }
      }
    }
    // Ref Y
    if (refs_active) {
      for (const auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        // Skip self — see X-acquire branch.
        bool is_self = false;
        for (auto &rsnap : m_ref_move_snaps)
          if (rsnap.obj == child.get()) { is_self = true; break; }
        if (is_self) continue;
        double gsx, gsy;
        doc_to_screen(0, child->ref_y, gsx, gsy);
        for (double c : {by1, cy, by2}) {
          double sx, sy;
          doc_to_screen(0, c, sx, sy);
          double d = std::abs(sy - gsy);
          if (d < best_d) {
            best_d = d;
            m_snap_y_locked = true;
            m_snap_locked_y = child->ref_y;
            adj_dy = raw_dy + (child->ref_y - c);
          }
        }
      }
    }
    // Grid Y — closest horizontal gridline (arithmetic)
    if (grid_active) {
      const double sy_grid = grid_l->grid_spacing_y;
      const double oy_grid = grid_l->grid_offset_y;
      for (double c : {by1, cy, by2}) {
        double cand = oy_grid + std::round((c - oy_grid) / sy_grid) * sy_grid;
        double gsx, gsy;
        doc_to_screen(0, cand, gsx, gsy);
        double sx, sy;
        doc_to_screen(0, c, sx, sy);
        double d = std::abs(sy - gsy);
        if (d < best_d) {
          best_d = d;
          m_snap_y_locked = true;
          m_snap_locked_y = cand;
          adj_dy = raw_dy + (cand - c);
        }
      }
    }
    // Margin Y — top, bottom, row dividers
    if (margins_active) {
      const double ch = (double)m_doc->canvas_height();
      const double top    = ml->margin_top;
      const double bottom = ch - ml->margin_bottom;
      std::vector<double> mcands{top, bottom};
      const int rows = std::max(1, ml->margin_rows);
      const double ih = bottom - top;
      const double gap_y = ml->margin_row_gap;
      if (rows > 1 && ih > 0) {
        const double row_h = (ih - gap_y * (rows - 1)) / rows;
        for (int ri = 1; ri < rows; ++ri) {
          double gy_top = top + ri * row_h + (ri - 1) * gap_y;
          mcands.push_back(gy_top);
          mcands.push_back(gy_top + gap_y);
        }
      }
      for (double cand : mcands) {
        double gsx, gsy;
        doc_to_screen(0, cand, gsx, gsy);
        for (double c : {by1, cy, by2}) {
          double sx, sy;
          doc_to_screen(0, c, sx, sy);
          double d = std::abs(sy - gsy);
          if (d < best_d) {
            best_d = d;
            m_snap_y_locked = true;
            m_snap_locked_y = cand;
            adj_dy = raw_dy + (cand - c);
          }
        }
      }
    }
  }

  return {adj_dx, adj_dy};
}

// ── Zoom ─────────────────────────────────────────────────────────────────────
void Canvas::zoom_toward(double wx, double wy, double factor) {
  const double old_zoom = m_zoom;
  const double min_z = std::max(fit_zoom() * 0.01, MIN_ZOOM);
  const double new_zoom = std::clamp(m_zoom * factor, min_z, MAX_ZOOM);
  if (new_zoom == old_zoom)
    return;

  // s113 m3: in preview mode, a zoom-in past the device-pixel safety
  // ceiling would crash the app on the next draw (drop-shadow Cairo
  // buffer scales with device pixels). Auto-flip to outline so the zoom
  // proceeds safely. Same hazard the m2 toggle gate prevents on the
  // outline→preview cliff; here we cover the in-preview-zoom-in path.
  if (!m_outline_mode && m_doc && new_zoom > old_zoom) {
    const double dw =
        static_cast<double>(m_doc->canvas_width()) * new_zoom;
    const double dh =
        static_cast<double>(m_doc->canvas_height()) * new_zoom;
    if (std::max(dw, dh) > PREVIEW_SAFE_DEVICE_PIXELS) {
      m_outline_mode = true;
      m_sig_outline_mode_changed.emit();
      m_sig_show_message.emit(
          "Switched to outline view",
          "Zoom is too high to render preview mode safely. The view has "
          "been switched to outline so the zoom can continue. Zoom out "
          "and toggle preview again when you're ready.");
    }
  }

  const double ox = doc_origin_x();
  const double oy = doc_origin_y();
  const double doc_x = (wx - ox) / old_zoom;
  const double doc_y = (wy - oy) / old_zoom;

  m_zoom = new_zoom;

  const double new_cw = m_doc ? m_doc->canvas_width() * m_zoom : 0;
  const double new_ch = m_doc ? m_doc->canvas_height() * m_zoom : 0;
  const double centre_x = (get_width() - new_cw) * 0.5;
  const double centre_y = (get_height() - new_ch) * 0.5;
  m_pan_x = wx - doc_x * m_zoom - centre_x;
  m_pan_y = wy - doc_y * m_zoom - centre_y;

  clamp_pan();
  m_sig_zoom.emit(m_zoom);
  queue_draw();
}

// Compute the zoom level that fits the entire canvas in the widget with margin
double Canvas::fit_zoom() const {
  if (!m_doc)
    return 1.0;
  int w = get_width();
  int h = get_height();
  if (w <= 0 || h <= 0)
    return 1.0;
  constexpr double MARGIN = 0.85; // use 85% of widget — leaves visible border
  double zw = (double)w / m_doc->canvas_width() * MARGIN;
  double zh = (double)h / m_doc->canvas_height() * MARGIN;
  return std::max(std::min(zw, zh), MIN_ZOOM);
}

// Fit canvas to window, centered, with margin
void Canvas::zoom_fit() {
  if (!m_doc)
    return;
  m_zoom = fit_zoom();
  m_pan_x = 0.0;
  m_pan_y = 0.0;
  m_sig_zoom.emit(m_zoom);
  queue_draw();
}

// Zoom to fit the screen-space rectangle (sx1,sy1)-(sx2,sy2) into the viewport.
void Canvas::zoom_to_rect(double sx1, double sy1, double sx2, double sy2) {
  if (!m_doc)
    return;
  double left = std::min(sx1, sx2);
  double top = std::min(sy1, sy2);
  double right = std::max(sx1, sx2);
  double bottom = std::max(sy1, sy2);
  if ((right - left) < 4 || (bottom - top) < 4)
    return;

  double dx1, dy1, dx2, dy2;
  screen_to_doc(left, top, dx1, dy1);
  screen_to_doc(right, bottom, dx2, dy2);
  double dw = dx2 - dx1;
  double dh = dy2 - dy1;
  if (dw < 1.0 || dh < 1.0)
    return;

  const double new_zoom =
      std::clamp(std::min(get_width() / dw, get_height() / dh) * 0.90,
                 std::max(fit_zoom() * 0.01, MIN_ZOOM), MAX_ZOOM);

  // s113 m3: same safety auto-switch as zoom_toward — marquee zoom can
  // land on an unsafe zoom in one shot (small marquee → large factor).
  // Only fires when zooming IN past the threshold; zoom-out and zoom-out
  // marquees are unaffected.
  if (!m_outline_mode && new_zoom > m_zoom) {
    const double pdw =
        static_cast<double>(m_doc->canvas_width()) * new_zoom;
    const double pdh =
        static_cast<double>(m_doc->canvas_height()) * new_zoom;
    if (std::max(pdw, pdh) > PREVIEW_SAFE_DEVICE_PIXELS) {
      m_outline_mode = true;
      m_sig_outline_mode_changed.emit();
      m_sig_show_message.emit(
          "Switched to outline view",
          "Zoom is too high to render preview mode safely. The view has "
          "been switched to outline so the zoom can continue. Zoom out "
          "and toggle preview again when you're ready.");
    }
  }

  double cx = (dx1 + dx2) * 0.5;
  double cy = (dy1 + dy2) * 0.5;
  m_zoom = new_zoom;
  // Re-centre the zoomed rect in the viewport
  m_pan_x = get_width() * 0.5 - cx * m_zoom -
            (get_width() - m_doc->canvas_width() * m_zoom) * 0.5;
  m_pan_y = get_height() * 0.5 - cy * m_zoom -
            (get_height() - m_doc->canvas_height() * m_zoom) * 0.5;

  m_sig_zoom.emit(m_zoom);
  queue_draw();
}

// Zoom to fit the active selection, centred in the viewport.
// Falls back to zoom_fit() if nothing is selected.
//
// In Node tool mode:
//   - Multiple nodes selected: zoom to tight bbox over all selected anchors.
//   - Single node selected: compute a comfortable view radius using the
//     distances to the 2 nearest other nodes in the same path, then pad
//     by 1.5× so the target node sits comfortably centred with neighbours
//     visible. Falls back to whole-object bbox if path has ≤ 2 nodes.
void Canvas::zoom_to_selection() {
  // ── Node tool branch ────────────────────────────────────────────────────
  if (m_tool == ActiveTool::Node && m_selected &&
      m_selected->type == SceneNode::Type::Path && m_selected->path) {

    const auto &nodes = m_selected->path->nodes;
    int n = (int)nodes.size();

    // Collect which node indices are selected
    std::vector<int> sel_indices;
    if (m_selected_node >= 0 && m_selected_node < n)
      sel_indices.push_back(m_selected_node);
    for (const auto &ns : m_node_selection) {
      if (ns.obj == m_selected && ns.node_idx >= 0 && ns.node_idx < n) {
        if (std::find(sel_indices.begin(), sel_indices.end(), ns.node_idx) ==
            sel_indices.end())
          sel_indices.push_back(ns.node_idx);
      }
    }

    if (sel_indices.empty()) {
      // Node tool active but no node selected — fall through to object bbox
      goto object_bbox_fallback;
    }

    if (sel_indices.size() == 1 && n >= 3) {
      // ── Single node: find 2 nearest neighbours by anchor distance ──────
      int si = sel_indices[0];
      double ax = nodes[si].x;
      double ay = nodes[si].y;

      // Collect distances from selected node to all other nodes
      std::vector<std::pair<double, int>> dists;
      dists.reserve(n - 1);
      for (int i = 0; i < n; ++i) {
        if (i == si)
          continue;
        double dx = nodes[i].x - ax;
        double dy = nodes[i].y - ay;
        dists.push_back({std::sqrt(dx * dx + dy * dy), i});
      }
      std::sort(dists.begin(), dists.end());

      // Take up to 2 nearest neighbours; measure their axis distances from
      // the selected node to compute a comfortable view half-extent.
      int k = std::min((int)dists.size(), 2);
      double max_dx = 0, max_dy = 0;
      for (int i = 0; i < k; ++i) {
        int ni = dists[i].second;
        max_dx = std::max(max_dx, std::abs(nodes[ni].x - ax));
        max_dy = std::max(max_dy, std::abs(nodes[ni].y - ay));
      }

      // Half-extent = neighbour distance * 1.5 — neighbours land inside
      // the view while the selected node stays exactly centred.
      double half_extent_x = max_dx * 1.5;
      double half_extent_y = max_dy * 1.5;
      // Minimum floor: 3% of canvas width so isolated nodes get a sensible view
      double min_half = m_doc ? m_doc->canvas_width() * 0.03 : 10.0;
      half_extent_x = std::max(half_extent_x, min_half);
      half_extent_y = std::max(half_extent_y, min_half);

      // Build rect centred on the selected node anchor
      double vx1 = ax - half_extent_x;
      double vy1 = ay - half_extent_y;
      double vx2 = ax + half_extent_x;
      double vy2 = ay + half_extent_y;

      double sx1, sy1, sx2, sy2;
      doc_to_screen(vx1, vy1, sx1, sy1);
      doc_to_screen(vx2, vy2, sx2, sy2);
      zoom_to_rect(sx1, sy1, sx2, sy2);
      return;

    } else {
      // ── Multiple nodes (or path too small for neighbour calc): ──────────
      // zoom to tight bbox of all selected anchors with a light padding.
      double minx = 0, miny = 0, maxx = 0, maxy = 0;
      bool first = true;
      for (int si : sel_indices) {
        double ax = nodes[si].x;
        double ay = nodes[si].y;
        if (first) {
          minx = maxx = ax;
          miny = maxy = ay;
          first = false;
        } else {
          minx = std::min(minx, ax);
          miny = std::min(miny, ay);
          maxx = std::max(maxx, ax);
          maxy = std::max(maxy, ay);
        }
      }

      // Pad by 20% each axis so nodes aren't right at the viewport edge
      double pw = (maxx - minx) * 0.20;
      double ph = (maxy - miny) * 0.20;
      double min_pad = m_doc ? m_doc->canvas_width() * 0.02 : 8.0;
      pw = std::max(pw, min_pad);
      ph = std::max(ph, min_pad);

      double sx1, sy1, sx2, sy2;
      doc_to_screen(minx - pw, miny - ph, sx1, sy1);
      doc_to_screen(maxx + pw, maxy + ph, sx2, sy2);
      zoom_to_rect(sx1, sy1, sx2, sy2);
      return;
    }
  }

  // ── Object selection fallback (Selection tool or no node selected) ───────
object_bbox_fallback:
  if (!m_doc || m_selection.empty()) {
    zoom_fit();
    return;
  }

  {
    bool found = false;
    double minx = 0, miny = 0, maxx = 0, maxy = 0;

    for (const SceneNode *obj : m_selection) {
      auto bb = object_bbox(*obj, /*include_stroke=*/false);
      if (!bb)
        continue;
      if (!found) {
        minx = bb->x;
        miny = bb->y;
        maxx = bb->x + bb->w;
        maxy = bb->y + bb->h;
        found = true;
      } else {
        minx = std::min(minx, bb->x);
        miny = std::min(miny, bb->y);
        maxx = std::max(maxx, bb->x + bb->w);
        maxy = std::max(maxy, bb->y + bb->h);
      }
    }

    if (!found) {
      zoom_fit();
      return;
    }

    double sx1, sy1, sx2, sy2;
    doc_to_screen(minx, miny, sx1, sy1);
    doc_to_screen(maxx, maxy, sx2, sy2);
    zoom_to_rect(sx1, sy1, sx2, sy2);
  }
}

// the artboard. Primary recovery tool for stranded off-canvas objects.
// Ctrl+Shift+0 per the HANDOFF spec.
void Canvas::zoom_to_all_objects() {
  if (!m_doc)
    return;

  // Collect union of all object bboxes in document space
  bool found = false;
  double minx, miny, maxx, maxy;
  minx = miny = maxx = maxy = 0.0;

  for (const auto &layer : m_doc->layers) {
    if (!layer->visible || layer->is_special_layer())
      continue;
    for (const auto &obj_ptr : layer->children) {
      auto bb = object_bbox(*obj_ptr);
      if (!bb)
        continue;
      if (!found) {
        minx = bb->x;
        miny = bb->y;
        maxx = bb->x + bb->w;
        maxy = bb->y + bb->h;
        found = true;
      } else {
        minx = std::min(minx, bb->x);
        miny = std::min(miny, bb->y);
        maxx = std::max(maxx, bb->x + bb->w);
        maxy = std::max(maxy, bb->y + bb->h);
      }
    }
  }

  if (!found) {
    // No objects — fall back to fit-artboard
    zoom_fit();
    return;
  }

  // Also include the artboard itself so it stays visible
  minx = std::min(minx, 0.0);
  miny = std::min(miny, 0.0);
  maxx = std::max(maxx, (double)m_doc->canvas_width());
  maxy = std::max(maxy, (double)m_doc->canvas_height());

  // Convert doc-space bbox to screen space at current zoom for zoom_to_rect
  double sx1, sy1, sx2, sy2;
  doc_to_screen(minx, miny, sx1, sy1);
  doc_to_screen(maxx, maxy, sx2, sy2);
  zoom_to_rect(sx1, sy1, sx2, sy2);
}

// ── Alt forwarding from MainWindow (more reliable than canvas-local key ctrl)
// ─
void Canvas::notify_alt_pressed() {
  if (m_mod_alt)
    return; // already set
  m_mod_alt = true;
  if (m_tool == ActiveTool::Zoom) {
    set_cursor("zoom-out");
    if (!m_zoom_alt_prev) {
      m_zoom_alt_prev = true;
      m_sig_zoom_alt.emit(true);
    }
  }
}

void Canvas::notify_alt_released() {
  if (!m_mod_alt)
    return; // already clear
  m_mod_alt = false;
  if (m_tool == ActiveTool::Zoom) {
    set_cursor("zoom-in");
    if (m_zoom_alt_prev) {
      m_zoom_alt_prev = false;
      m_sig_zoom_alt.emit(false);
    }
  }
}

void Canvas::notify_space_pressed() {
  if (m_space_held)
    return;
  m_space_held = true;
  // Show a grab cursor whenever Space is down so the user knows pan is ready.
  // Don't change cursor if the Zoom tool is active — it owns its own cursor.
  if (m_tool != ActiveTool::Zoom)
    set_cursor("grab");
}

void Canvas::notify_space_released() {
  if (!m_space_held)
    return;
  m_space_held = false;
  m_space_panning = false;
  // Restore normal cursor for the active tool.
  if (m_tool == ActiveTool::Zoom)
    set_cursor(m_mod_alt ? "zoom-out" : "zoom-in");
  else if (m_tool == ActiveTool::Pen || m_tool == ActiveTool::Rect ||
           m_tool == ActiveTool::Ellipse || m_tool == ActiveTool::Line ||
           m_tool == ActiveTool::Eyedropper || m_tool == ActiveTool::Polygon ||
           m_tool == ActiveTool::Spiral)
    set_cursor("crosshair");
  else
    set_cursor("default");
}

void Canvas::notify_r_pressed() {
  // R toggles pivot mode on/off
  if (m_tool != ActiveTool::Selection || m_selection.empty())
    return;

  if (m_r_held) {
    // Second R press — exit pivot mode
    m_r_held = false;
    m_pivot_dragging = false;
    set_cursor("default");
    queue_draw();
    return;
  }

  m_r_held = true;
  // Default pivot to BBX center if not already set
  if (!m_has_custom_pivot) {
    bool found = false;
    double bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
    for (SceneNode *obj : m_selection) {
      auto bb = object_bbox(*obj);
      if (!bb)
        continue;
      if (!found) {
        bx1 = bb->x;
        by1 = bb->y;
        bx2 = bb->x + bb->w;
        by2 = bb->y + bb->h;
        found = true;
      } else {
        bx1 = std::min(bx1, bb->x);
        by1 = std::min(by1, bb->y);
        bx2 = std::max(bx2, bb->x + bb->w);
        by2 = std::max(by2, bb->y + bb->h);
      }
    }
    if (found) {
      m_custom_pivot_x = (bx1 + bx2) * 0.5;
      m_custom_pivot_y = (by1 + by2) * 0.5;
    }
  }
  set_cursor("crosshair");
  queue_draw();
}

void Canvas::notify_r_released() {
  // R is now a toggle — key release does nothing
  (void)0;
}

void Canvas::on_pivot_dialog(double doc_x, double doc_y) {
  m_custom_pivot_x = snap(doc_x);
  m_custom_pivot_y = snap(doc_y);
  m_has_custom_pivot = true;
  queue_draw();

  auto *win = dynamic_cast<Gtk::Window *>(get_root());
  if (!win)
    return;

  auto *dlg = new Gtk::Window();
  curvz::utils::set_name(dlg, "dlg_rfp", "rotate_from_point_dialog_root");
  dlg->set_title("Rotate from Point");
  dlg->set_transient_for(*win);
  curvz::utils::apply_motif_class_from_parent(*dlg, *win);  // s117 m18 v2
  dlg->set_modal(true);
  dlg->set_resizable(false);
  dlg->set_default_size(260, -1);

  auto *vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto *grid = Gtk::make_managed<Gtk::Grid>();
  grid->set_row_spacing(8);
  grid->set_column_spacing(12);
  grid->set_margin(16);
  grid->set_margin_bottom(8);

  auto make_lbl = [](const char *t) {
    auto *l = Gtk::make_managed<Gtk::Label>(t);
    l->set_halign(Gtk::Align::START);
    return l;
  };

  // csb handles unit display + Y-flip (PositionY stores doc-Y-down internally
  // and displays Y-up user space). Ruler origin = 0 so pivot coordinates are
  // shown in raw canvas space, matching prior behavior.
  const CanvasModel *cm = m_doc ? &m_doc->canvas : nullptr;
  auto *spin_x =
      Gtk::make_managed<CurvzSpinButton>(SpinType::PositionX, cm, 0.0);
  curvz::utils::set_name(spin_x, "dlg_rfp_x", "rotate_from_point_dialog_x_spn");
  spin_x->with_value(m_custom_pivot_x);
  spin_x->set_hexpand(true);

  auto *spin_y =
      Gtk::make_managed<CurvzSpinButton>(SpinType::PositionY, cm, 0.0);
  curvz::utils::set_name(spin_y, "dlg_rfp_y", "rotate_from_point_dialog_y_spn");
  spin_y->with_value(m_custom_pivot_y); // doc-Y-down; csb flips for display
  spin_y->set_hexpand(true);

  auto *spin_a = Gtk::make_managed<CurvzSpinButton>(SpinType::Angle, cm);
  curvz::utils::set_name(spin_a, "dlg_rfp_ang", "rotate_from_point_dialog_angle_spn");
  spin_a->with_value(0.0);
  spin_a->set_hexpand(true);

  grid->attach(*make_lbl("Pivot X"), 0, 0);
  grid->attach(*spin_x, 1, 0);
  if (auto *ul = spin_x->get_unit_label())
    grid->attach(*ul, 2, 0);
  grid->attach(*make_lbl("Pivot Y"), 0, 1);
  grid->attach(*spin_y, 1, 1);
  if (auto *ul = spin_y->get_unit_label())
    grid->attach(*ul, 2, 1);
  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  sep->set_margin_top(4);
  sep->set_margin_bottom(4);
  grid->attach(*sep, 0, 2, 3, 1);
  grid->attach(*make_lbl("Rotate °"), 0, 3);
  grid->attach(*spin_a, 1, 3);

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  btn_row->set_halign(Gtk::Align::END);
  btn_row->set_margin_start(16);
  btn_row->set_margin_end(16);
  btn_row->set_margin_bottom(16);
  auto *btn_cancel = Gtk::make_managed<Gtk::Button>("Cancel");
  auto *btn_apply = Gtk::make_managed<Gtk::Button>("Apply");
  curvz::utils::set_name(btn_cancel, "dlg_rfp_cnc", "rotate_from_point_dialog_cancel_btn");
  curvz::utils::set_name(btn_apply, "dlg_rfp_app", "rotate_from_point_dialog_apply_btn");
  btn_apply->add_css_class("suggested-action");
  btn_row->append(*btn_cancel);
  btn_row->append(*btn_apply);

  vbox->append(*grid);
  vbox->append(*btn_row);
  dlg->set_child(*vbox);
  spin_x->grab_focus();

  btn_cancel->signal_clicked().connect([dlg]() { dlg->close(); });

  btn_apply->signal_clicked().connect([this, dlg, spin_x, spin_y, spin_a]() {
    // csb internal = doc-Y-down coords already; no manual flip needed.
    m_custom_pivot_x = spin_x->get_internal_value();
    m_custom_pivot_y = spin_y->get_internal_value();
    m_has_custom_pivot = true;

    double angle_deg = spin_a->get_internal_value();
    if (m_doc && std::abs(angle_deg) > 0.0001) {
      double angle_rad = angle_deg * M_PI / 180.0;
      double px = m_custom_pivot_x, py = m_custom_pivot_y;
      double cos_a = std::cos(angle_rad), sin_a = std::sin(angle_rad);

      std::vector<SceneNode *> leaves, direct;
      for (SceneNode *obj : m_selection) {
        if (obj->is_path()) {
          std::vector<SceneNode *> tmp;
          // inline collect: paths only
          std::function<void(SceneNode *)> gather = [&](SceneNode *n) {
            if (n->is_path() && n->path)
              leaves.push_back(n);
            for (auto &ch : n->children)
              gather(ch.get());
          };
          gather(obj);
        } else if (obj->is_text() || obj->is_image()) {
          direct.push_back(obj);
        } else {
          std::function<void(SceneNode *)> gather = [&](SceneNode *n) {
            if (n->is_path() && n->path)
              leaves.push_back(n);
            for (auto &ch : n->children)
              gather(ch.get());
          };
          gather(obj);
        }
      }

      struct PSnap {
        SceneNode *obj;
        PathData before;
      };
      std::vector<PSnap> psnaps;
      for (SceneNode *leaf : leaves)
        if (leaf->path)
          psnaps.push_back({leaf, *leaf->path});
      struct TSnap {
        SceneNode *obj;
        double bx, by;
      };
      std::vector<TSnap> tsnaps;
      for (SceneNode *obj : direct)
        tsnaps.push_back({obj, obj->is_text() ? obj->text_x : obj->image_x,
                          obj->is_text() ? obj->text_y : obj->image_y});

      auto rot = [&](double &x, double &y) {
        double dx = x - px, dy = y - py;
        x = px + dx * cos_a - dy * sin_a;
        y = py + dx * sin_a + dy * cos_a;
      };

      for (SceneNode *leaf : leaves)
        if (leaf->path)
          for (auto &n : leaf->path->nodes) {
            rot(n.x, n.y);
            rot(n.cx1, n.cy1);
            rot(n.cx2, n.cy2);
          }
      for (SceneNode *obj : direct) {
        if (obj->is_text()) {
          rot(obj->text_x, obj->text_y);
        } else {
          double cx = obj->image_x + obj->image_w * 0.5;
          double cy = obj->image_y + obj->image_h * 0.5;
          rot(cx, cy);
          obj->image_x = cx - obj->image_w * 0.5;
          obj->image_y = cy - obj->image_h * 0.5;
        }
      }

      if (m_history) {
        auto comp = std::make_unique<CompositeCommand>("Rotate from point");
        for (auto &s : psnaps)
          comp->add(std::make_unique<EditPathCommand>(
              s.obj, s.before, *s.obj->path, "Rotate from point"));
        for (auto &s : tsnaps) {
          double ax = s.obj->is_text() ? s.obj->text_x : s.obj->image_x;
          double ay = s.obj->is_text() ? s.obj->text_y : s.obj->image_y;
          comp->add(
              std::make_unique<MoveObjectCommand>(s.obj, s.bx, s.by, ax, ay));
        }
        if (!comp->steps.empty())
          m_history->push(std::move(comp));
      }
      m_sig_doc_changed.emit();
      queue_draw();
    }
    queue_draw();
    dlg->close();
  });

  dlg->signal_hide().connect([dlg]() { delete dlg; });
  dlg->show();
}

// Forward declaration — collect_paths is a static free function defined later
// in this TU.
static void collect_paths(SceneNode *obj, std::vector<SceneNode *> &out);

// Forward declaration — find_parent is a static free function defined later
// in this TU (near the boolean-ops / make_compound_path block). delete_selected
// uses it to route both layer-level and Compound-child deletions through the
// same lookup that descends one level into Group/Compound.
static SceneNode *find_parent(CurvzDocument *doc, SceneNode *target,
                              int *out_idx);

// ── Delete selected object(s) (Selection tool) ───────────────────────────────
bool Canvas::delete_selected() {
  if (m_tool == ActiveTool::Node)
    return false;
  if (m_selection.empty() || !m_doc)
    return false;

  bool deleted_any = false;
  // If a Compound dissolves during this delete, its surviving child is
  // tracked here so the post-loop block can promote it as the new
  // selection. Reset to nullptr if a subsequent iteration deletes the
  // survivor itself (multi-select of all Compound children).
  SceneNode *promoted_survivor = nullptr;
  const SceneNode *rl = m_doc->ref_layer();
  bool ref_layer_locked = (rl && rl->locked);

  // Helper — find the ClipGroup whose clip_shape slot holds `node`, or
  //   nullptr if none. We walk the whole tree; ClipGroups are rare
  //   enough that this is cheap. Returns the ClipGroup ancestor, not
  //   the immediate parent of the clip_shape (clip_shape has no
  //   parent->children slot).
  std::function<SceneNode *(SceneNode *, SceneNode *)> find_clip_owner =
      [&](SceneNode *root, SceneNode *target) -> SceneNode * {
    if (!root)
      return nullptr;
    if (root->is_clip_group() && root->clip_shape &&
        root->clip_shape.get() == target)
      return root;
    for (auto &c : root->children) {
      if (auto *r = find_clip_owner(c.get(), target))
        return r;
    }
    if (root->clip_shape) {
      if (auto *r = find_clip_owner(root->clip_shape.get(), target))
        return r;
    }
    return nullptr;
  };

  // Snapshot the selection vector before iterating. Several branches
  // below (ClipGroup release, Blend release, Compound dissolve)
  // reassign m_selection mid-flight; iterating the live vector with a
  // range-for would walk into UB after the reassignment. The snapshot
  // is cheap (vector of pointers) and decouples iteration from the
  // selection state's evolution.
  std::vector<SceneNode *> to_delete = m_selection;

  for (SceneNode *obj : to_delete) {
    // Refs not deletable when ref layer is locked
    if (obj->is_ref() && ref_layer_locked)
      continue;

    // ── Clip-shape special case ──────────────────────────────────────
    // If `obj` is the clip_shape of some ClipGroup, the user's intent
    // is "stop clipping" — dissolve the ClipGroup. We route through
    // release_clip_group which handles the proper unwind (clip_shape
    // and children return as siblings in parent, clip_shape on top).
    // We don't individually "delete" the clip_shape: the release flow
    // already restores it as a normal Path in the parent, matching the
    // spec Scott confirmed back at spec time ("if deleted it breaks
    // the clip and restores the items clipped").
    SceneNode *clip_owner = nullptr;
    for (auto &layer : m_doc->layers)
      if ((clip_owner = find_clip_owner(layer.get(), obj)))
        break;
    if (clip_owner) {
      LOG_INFO("Canvas: delete of clip_shape → dissolving ClipGroup '{}'",
               clip_owner->name);
      SceneNode *prev_selected = m_selected;
      m_selected = clip_owner;
      m_selection = {clip_owner};
      release_clip_group();
      (void)prev_selected; // selection is replaced by release_clip_group
      deleted_any = true;
      continue;
    }

    // ── Blend-source special case ────────────────────────────────────
    // If `obj` is blend_source_a or blend_source_b of some Blend, the
    // user's intent is "break the blend" — dissolve it. Matches the
    // ClipGroup precedent and keeps A/B + Steps available as siblings.
    // Both sources are restored; if the user truly wants the source
    // gone, they delete again after release.
    if (SceneNode *blend_owner = find_blend_owner(obj)) {
      LOG_INFO("Canvas: delete of Blend source → dissolving Blend '{}'",
               blend_owner->name);
      m_selected = blend_owner;
      m_selection = {blend_owner};
      release_blend();
      deleted_any = true;
      continue;
    }

    // Locate the parent of `obj`. find_parent descends one level into
    // Group/Compound, so this handles top-level layer children AND
    // children of a Group or Compound. Returns nullptr if not found
    // (e.g. obj was already removed by a prior iteration of the outer
    // loop, or it lives in a slot we don't walk like ClipGroup
    // clip_shape — that case is routed above).
    int idx = -1;
    SceneNode *parent = find_parent(m_doc, obj, &idx);
    if (!parent)
      continue;

    // Snapshot the doomed child for undo BEFORE mutation.
    auto child_snap = clone_node(*parent->children[idx]);

    // ── Compound subpath delete: extra invariants ──
    // A Compound exists only when it has ≥2 children. Removing a child
    // may push it to 1 (auto-dissolve into the survivor) or 0 (delete
    // the empty Compound). Each case uses a single command — not a
    // composite — chosen to capture the structural change atomically:
    //   - ≥2 children remaining → DeleteObjectCommand on the Compound.
    //   - 1 remaining → ReplaceNodeCommand on the outer parent
    //     (Compound → survivor); the deleted child rides along inside
    //     the `before` snapshot for undo.
    //   - 0 remaining → DeleteObjectCommand on the OUTER parent with
    //     the full pre-delete Compound subtree as snapshot.
    if (parent->is_compound()) {
      // Locate the Compound itself within ITS parent (the layer or an
      // outer container). Required for both the dissolve (replace
      // Compound with survivor) and the empty-collapse (delete
      // Compound) paths.
      int comp_idx = -1;
      SceneNode *comp_parent = find_parent(m_doc, parent, &comp_idx);
      if (!comp_parent) {
        // Compound not findable — should not happen for a well-formed
        // doc, but bail safely rather than corrupt the tree.
        LOG_WARN("delete_selected: Compound subpath delete found no "
                 "Compound parent — skipping");
        continue;
      }

      const int new_child_count = (int)parent->children.size() - 1;

      if (new_child_count >= 2) {
        // ── Plain subpath delete, Compound stays. Same shape as a
        //    layer-level delete, just one level down.
        if (m_history)
          m_history->push(std::make_unique<DeleteObjectCommand>(
              parent, std::move(child_snap), idx));
        parent->children.erase(parent->children.begin() + idx);
      } else if (new_child_count == 1) {
        // ── Dissolve: Compound + 1 surviving child → just the child
        //    promoted into the Compound's slot. Survivor keeps its own
        //    name (per spec: Compound's name was a label for the
        //    combination, not the surviving piece).
        //
        // The transformation is naturally a single
        // ReplaceNodeCommand: swap the original (2-child) Compound for
        // the surviving child at the same slot in comp_parent. The
        // doomed child rides along inside compound_before_snap (the
        // command's `before`); on undo the whole pre-mutation Compound
        // is restored, including both children.
        //
        // No composite is needed here. An earlier draft used
        // CompositeCommand of {DeleteObjectCommand on Compound,
        // ReplaceNodeCommand}, but DeleteObjectCommand's `parent`
        // pointer would dangle after ReplaceNodeCommand destroyed the
        // original Compound — undo would write to freed memory.
        const int survivor_idx = (idx == 0) ? 1 : 0;
        (void)child_snap; // doomed child is captured inside compound_before_snap.

        // Snapshots:
        //   - compound_before_snap: full pre-mutation Compound (both
        //     children present). Restored on undo.
        //   - survivor_snap: surviving child standing alone. Re-applied
        //     on redo.
        auto compound_before_snap = clone_node(*parent);
        auto survivor_snap = clone_node(*parent->children[survivor_idx]);

        // Mutate the live tree. The unique_ptr assignment at
        // comp_parent->children[comp_idx] destroys the original
        // Compound (and with it the doomed child plus the moved-from
        // null at survivor_idx). After this line, `parent` is a
        // dangling pointer — do NOT use it again in this branch.
        auto survivor_owned =
            std::move(parent->children[survivor_idx]);
        SceneNode *survivor_ptr_in_tree = survivor_owned.get();
        comp_parent->children[comp_idx] = std::move(survivor_owned);

        if (m_history)
          m_history->push(std::make_unique<ReplaceNodeCommand>(
              comp_parent, comp_idx, std::move(compound_before_snap),
              std::move(survivor_snap)));

        // Promote the survivor as the new selection — useful affordance
        // matching split_compound_path's "select the result of the
        // structural change" idiom. Recorded for the post-loop block;
        // cleared if a later iteration deletes the survivor itself.
        promoted_survivor = survivor_ptr_in_tree;
        LOG_INFO("Canvas: subpath delete → Compound dissolved, survivor "
                 "'{}' promoted",
                 survivor_ptr_in_tree->name.empty()
                     ? survivor_ptr_in_tree->id
                     : survivor_ptr_in_tree->name);
        deleted_any = true;
        continue;
      } else {
        // ── new_child_count == 0: Compound becomes empty → delete it.
        //    Only reachable from batch-delete that removes both children
        //    of a 2-child Compound in one operation; single-step
        //    delete dissolves at 1 first.
        //
        // Undo is one command, not a composite: snapshot the WHOLE
        // pre-delete Compound (with both children) and restore it via
        // DeleteObjectCommand on the OUTER parent. Forward erases the
        // Compound from comp_parent; undo re-inserts the snapshot
        // (with both children intact). The doomed child restoration is
        // implicit in the subtree snapshot.
        //
        // Note: child_snap is unused on this branch — the whole-
        // Compound snapshot already contains it. clone_node is cheap
        // enough that we don't bother avoiding the redundant clone.
        (void)child_snap;
        auto compound_before_snap = clone_node(*parent);

        // Mutate: erase the Compound from its outer parent. This
        // destroys the Compound and its surviving (doomed) child in
        // one shot.
        comp_parent->children.erase(comp_parent->children.begin() +
                                    comp_idx);

        if (m_history)
          m_history->push(std::make_unique<DeleteObjectCommand>(
              comp_parent, std::move(compound_before_snap), comp_idx));

        LOG_INFO("Canvas: subpath delete → Compound emptied, removed");
      }

      deleted_any = true;
      continue;
    }

    // ── Non-Compound parent (Layer or Group): plain delete.
    if (m_history)
      m_history->push(std::make_unique<DeleteObjectCommand>(
          parent, std::move(child_snap), idx));
    parent->children.erase(parent->children.begin() + idx);
    // If this delete consumed an earlier-dissolve survivor, drop the
    // promotion record — the post-loop block must then fall through to
    // the plain "clear selection" path.
    if (promoted_survivor == obj)
      promoted_survivor = nullptr;
    deleted_any = true;
  }
  if (deleted_any) {
    // Record macro step
    {
      MacroStep s;
      s.op = MacroStep::Op::Delete;
      record_step_if_recording(s);
    }
    if (promoted_survivor) {
      m_selection = {promoted_survivor};
      m_selected = promoted_survivor;
    } else {
      m_selected = nullptr;
      m_selection.clear();
    }
    m_selected_node = -1;
    m_sig_selection.emit(m_selected);
    m_sig_doc_changed.emit();
    queue_draw();
    LOG_INFO("Canvas: deleted {} selected objects", m_selection.size());
  }
  return deleted_any;
}

// ── Clipboard helpers
// ─────────────────────────────────────────────────────────

// Forward declaration — defined below near boolean ops
static void collect_paths(SceneNode *obj, std::vector<SceneNode *> &out);

// Collect all nodes in doc order (walk layers top-to-bottom, children
// top-to-bottom). Returns {parent*, node*} pairs for every node in m_selection.
struct SelectionEntry {
  SceneNode *parent;
  SceneNode *node;
  int index;
};
static std::vector<SelectionEntry>
collect_selection_entries(CurvzDocument *doc,
                          const std::vector<SceneNode *> &selection) {
  std::vector<SelectionEntry> result;
  for (auto &layer : doc->layers) {
    for (int i = 0; i < (int)layer->children.size(); ++i) {
      SceneNode *child = layer->children[i].get();
      for (SceneNode *sel : selection) {
        if (sel == child)
          result.push_back({layer.get(), child, i});
      }
    }
  }
  return result;
}

// Strip any existing " (N)" suffix, then find the lowest N≥2 that is unique
// across all ids and names in the document.
static void collect_all_names(const SceneNode *node,
                              std::set<std::string> &out) {
  out.insert(node->name);
  for (const auto &child : node->children)
    collect_all_names(child.get(), out);
}

static std::string generate_unique_name(const std::string &base_name,
                                        CurvzDocument *doc) {
  // Strip existing " (N)" suffix
  std::string base = base_name;
  auto paren = base.rfind(" (");
  if (paren != std::string::npos) {
    std::string suffix = base.substr(paren + 2);
    if (!suffix.empty() && suffix.back() == ')') {
      suffix.pop_back();
      bool is_num = !suffix.empty() &&
                    std::all_of(suffix.begin(), suffix.end(), ::isdigit);
      if (is_num)
        base = base.substr(0, paren);
    }
  }

  // Collect all existing names
  std::set<std::string> names;
  for (auto &layer : doc->layers)
    for (auto &child : layer->children)
      collect_all_names(child.get(), names);

  // Try base (2), (3), ... until unique
  for (int n = 2; n < 10000; ++n) {
    std::string candidate = base + " (" + std::to_string(n) + ")";
    if (names.find(candidate) == names.end())
      return candidate;
  }
  return base + " (copy)";
}

// Recursively assign fresh ids and unique names to a cloned subtree.
static void freshen_ids(SceneNode *node, CurvzDocument *doc, int &counter) {
  node->id = "obj" + std::to_string(counter++);
  node->name = generate_unique_name(node->name, doc);
  for (auto &child : node->children)
    freshen_ids(child.get(), doc, counter);
}

// ── copy_selected
// ─────────────────────────────────────────────────────────────
void Canvas::select_all() {
  if (!m_doc)
    return;

  m_selection.clear();
  m_selected = nullptr;

  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->locked)
      continue;
    if (layer->is_special_layer())
      continue;
    for (auto &child : layer->children) {
      if (!child->visible)
        continue;
      m_selection.push_back(child.get());
    }
  }

  if (!m_selection.empty())
    m_selected = m_selection[0];

  m_sig_selection.emit(m_selected);
  queue_draw();
  LOG_INFO("Canvas: select_all — {} objects", m_selection.size());
}

// s136 m5: counterpart to select_all. Wired to Ctrl+Shift+A. Cheap to call
// when nothing is selected (no signal storm, no redraw) — guards on the
// already-empty case so accidental double-presses don't churn.
void Canvas::clear_selection() {
  if (m_selection.empty() && m_selected == nullptr)
    return;

  m_selection.clear();
  m_selected = nullptr;

  m_sig_selection.emit(nullptr);
  queue_draw();
  LOG_INFO("Canvas: clear_selection");
}

void Canvas::copy_selected() {
  if (m_selection.empty() || !m_doc)
    return;
  m_clipboard.clear();
  m_clipboard_was_cut = false;

  auto entries = collect_selection_entries(m_doc, m_selection);
  for (auto &e : entries)
    m_clipboard.push_back(clone_node(*e.node));

  LOG_INFO("Canvas: copied {} object(s) to clipboard", m_clipboard.size());
}

// ── cut_selected
// ──────────────────────────────────────────────────────────────
void Canvas::cut_selected() {
  if (m_selection.empty() || !m_doc)
    return;

  auto entries = collect_selection_entries(m_doc, m_selection);
  if (entries.empty())
    return;

  // Snapshot for clipboard (keep original ids — cut reuses them on paste)
  m_clipboard.clear();
  m_clipboard_was_cut = true;
  for (auto &e : entries)
    m_clipboard.push_back(clone_node(*e.node));

  // Build CutCommand entries (descending index order for safe removal)
  std::vector<CutCommand::Entry> cmd_entries;
  for (auto &e : entries)
    cmd_entries.push_back({e.parent, clone_node(*e.node), e.index});

  // Remove from document (high index first)
  std::vector<int> order(entries.size());
  for (int i = 0; i < (int)entries.size(); ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](int a, int b) { return entries[a].index > entries[b].index; });
  for (int i : order) {
    auto &ch = entries[i].parent->children;
    for (int j = (int)ch.size() - 1; j >= 0; --j)
      if (ch[j].get() == entries[i].node) {
        ch.erase(ch.begin() + j);
        break;
      }
  }

  if (m_history)
    m_history->push(std::make_unique<CutCommand>(std::move(cmd_entries)));

  m_selected = nullptr;
  m_selection.clear();
  m_selected_node = -1;
  m_sig_selection.emit(nullptr);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: cut {} object(s)", m_clipboard.size());
}

// ── paste_clipboard
// ───────────────────────────────────────────────────────────
void Canvas::paste_clipboard() {
  if (m_clipboard.empty() || !m_doc)
    return;

  SceneNode *target_layer = m_doc->active_layer();
  if (!target_layer)
    return;

  // Build pasted nodes: copy from clipboard, freshen ids/names unless cut
  std::vector<std::unique_ptr<SceneNode>> pasted;
  int id_counter = s_next_id;

  for (auto &cb : m_clipboard) {
    auto node = clone_node(*cb);
    if (!m_clipboard_was_cut) {
      freshen_ids(node.get(), m_doc, id_counter);
    }
    pasted.push_back(std::move(node));
  }
  if (!m_clipboard_was_cut)
    s_next_id = id_counter;

  // Compute viewport centre in doc space — paste centres objects there
  double vp_cx, vp_cy;
  screen_to_doc(get_width() * 0.5, get_height() * 0.5, vp_cx, vp_cy);

  // Compute bounding box of pasted objects to centre them
  double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
  for (auto &node : pasted) {
    auto bb = object_bbox(*node, false);
    if (!bb)
      continue;
    bx1 = std::min(bx1, bb->x);
    by1 = std::min(by1, bb->y);
    bx2 = std::max(bx2, bb->x + bb->w);
    by2 = std::max(by2, bb->y + bb->h);
  }
  double paste_cx = (bx1 + bx2) * 0.5;
  double paste_cy = (by1 + by2) * 0.5;
  double dx = vp_cx - paste_cx;
  double dy = vp_cy - paste_cy;

  // Translate all pasted nodes to viewport centre
  if (std::abs(dx) > 0.01 || std::abs(dy) > 0.01) {
    for (auto &node : pasted) {
      std::vector<SceneNode *> paths;
      collect_paths(node.get(), paths);
      for (SceneNode *p : paths) {
        if (!p->path)
          continue;
        for (auto &n : p->path->nodes) {
          n.x += dx;
          n.y += dy;
          n.cx1 += dx;
          n.cy1 += dy;
          n.cx2 += dx;
          n.cy2 += dy;
        }
      }
    }
  }

  // Build command snapshots before moving ownership
  std::vector<std::unique_ptr<SceneNode>> cmd_snaps;
  for (auto &node : pasted)
    cmd_snaps.push_back(clone_node(*node));

  // Insert into target layer (front = top of layer)
  std::vector<SceneNode *> new_selection;
  for (int i = (int)pasted.size() - 1; i >= 0; --i) {
    new_selection.push_back(pasted[i].get());
    target_layer->children.insert(target_layer->children.begin(),
                                  std::move(pasted[i]));
  }

  if (m_history)
    m_history->push(
        std::make_unique<PasteCommand>(target_layer, std::move(cmd_snaps)));

  // After a cut-paste, the clipboard contents are consumed (can't paste again
  // with same ids). Clear the cut flag so a re-paste would freshen ids.
  if (m_clipboard_was_cut)
    m_clipboard_was_cut = false;

  // Select the pasted objects
  m_selection = new_selection;
  m_selected = new_selection.empty() ? nullptr : new_selection[0];
  m_selected_node = -1;
  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: pasted {} object(s)", new_selection.size());
}

// ── duplicate_selected
// ────────────────────────────────────────────────────────
void Canvas::duplicate_selected() {
  if (m_selection.empty() || !m_doc)
    return;

  auto entries = collect_selection_entries(m_doc, m_selection);
  if (entries.empty())
    return;

  constexpr double OFFSET = 10.0; // doc-space nudge so duplicate is visible

  std::vector<DuplicateCommand::Entry> cmd_entries;
  std::vector<SceneNode *> new_selection;
  int id_counter = s_next_id;

  // Process in ascending index order so insertions don't shift later indices
  // We insert each duplicate immediately above its original (index + offset).
  // We accumulate an offset_shift to account for already-inserted duplicates.
  int shift = 0;
  for (auto &e : entries) {
    auto dup = clone_node(*e.node);
    freshen_ids(dup.get(), m_doc, id_counter);

    // Translate path nodes by OFFSET
    std::vector<SceneNode *> paths;
    collect_paths(dup.get(), paths);
    for (SceneNode *p : paths) {
      if (!p->path)
        continue;
      for (auto &n : p->path->nodes) {
        n.x += OFFSET;
        n.y -= OFFSET; // y-up: subtract moves up visually
        n.cx1 += OFFSET;
        n.cy1 -= OFFSET;
        n.cx2 += OFFSET;
        n.cy2 -= OFFSET;
      }
    }

    int ins = e.index +
              shift; // insert above original (lower index = higher in layer)
    auto snap = clone_node(*dup);
    new_selection.push_back(dup.get());
    e.parent->children.insert(e.parent->children.begin() + ins, std::move(dup));
    cmd_entries.push_back({e.parent, std::move(snap), ins});
    ++shift;
  }
  s_next_id = id_counter;

  if (m_history)
    m_history->push(std::make_unique<DuplicateCommand>(std::move(cmd_entries)));

  // Record macro step
  {
    MacroStep s;
    s.op = MacroStep::Op::Duplicate;
    s.dx = OFFSET;
    s.dy = -OFFSET; // y-up: negative moves up visually
    record_step_if_recording(s);
  }

  m_selection = new_selection;
  m_selected = new_selection.empty() ? nullptr : new_selection[0];
  m_selected_node = -1;
  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: duplicated {} object(s)", new_selection.size());
}

// ── clone_selected
// ───────────────────────────────────────────────────────────── Duplicate
// in-place (zero offset) — clone lands exactly on top of original. Recordable
// as MacroStep::Op::Clone.
void Canvas::clone_selected() {
  if (m_selection.empty() || !m_doc)
    return;

  auto entries = collect_selection_entries(m_doc, m_selection);
  if (entries.empty())
    return;

  std::vector<DuplicateCommand::Entry> cmd_entries;
  std::vector<SceneNode *> new_selection;
  int id_counter = s_next_id;

  int shift = 0;
  for (auto &e : entries) {
    auto dup = clone_node(*e.node);
    freshen_ids(dup.get(), m_doc, id_counter);
    // No position offset — clone lands exactly on top
    int ins = e.index + shift;
    auto snap = clone_node(*dup);
    new_selection.push_back(dup.get());
    e.parent->children.insert(e.parent->children.begin() + ins, std::move(dup));
    cmd_entries.push_back({e.parent, std::move(snap), ins});
    ++shift;
  }
  s_next_id = id_counter;

  if (m_history)
    m_history->push(std::make_unique<DuplicateCommand>(std::move(cmd_entries)));

  // Record macro step
  {
    MacroStep s;
    s.op = MacroStep::Op::Clone;
    record_step_if_recording(s);
  }

  m_selection = new_selection;
  m_selected = new_selection.empty() ? nullptr : new_selection[0];
  m_selected_node = -1;
  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: cloned {} object(s)", new_selection.size());
}

void Canvas::select_object(SceneNode *obj) {
  // Selecting an object clears guide selection
  if (!m_guide_selection.empty()) {
    m_guide_selection.clear();
    m_sig_guide_selection_changed.emit(m_guide_selection);
  }
  m_selected = obj;
  m_selection.clear();
  if (obj)
    m_selection.push_back(obj);
  m_selected_node = -1;
  m_sig_selection.emit(m_selected);
  queue_draw();
}

void Canvas::set_multi_selection(const std::vector<SceneNode *> &sel) {
  if (!sel.empty() && !m_guide_selection.empty()) {
    m_guide_selection.clear();
    m_sig_guide_selection_changed.emit(m_guide_selection);
  }
  m_selection = sel;
  m_selected = sel.empty() ? nullptr : sel.front();
  m_selected_node = -1;
  m_sig_selection.emit(m_selected);
  queue_draw();
}

// ── Helper: find the layer and index of a SceneNode
// ─────────────────────────── ── collect_paths
// ───────────────────────────────────────────────────────────── Recursively
// collect all leaf Path nodes from obj (handles Group, Compound, ClipGroup).
//
// ClipGroup: descend into both `children` (the clipped objects) AND
// `clip_shape` (the path defining the clip region). Transforms driven
// off the returned list — nudge, drag, scale, rotate, skew — then
// mutate the clip shape in lock-step with its children, which is the
// correct rigid-transform behaviour: move/scale/rotate the ClipGroup
// and the clipped content stays clipped at its new location.
static void collect_paths(SceneNode *obj, std::vector<SceneNode *> &out) {
  if (!obj)
    return;
  if (obj->type == SceneNode::Type::Path && obj->path) {
    out.push_back(obj);
    return;
  }
  if (obj->type == SceneNode::Type::Group ||
      obj->type == SceneNode::Type::Compound) {
    for (auto &child : obj->children)
      collect_paths(child.get(), out);
    return;
  }
  if (obj->type == SceneNode::Type::ClipGroup) {
    for (auto &child : obj->children)
      collect_paths(child.get(), out);
    if (obj->clip_shape)
      collect_paths(obj->clip_shape.get(), out);
    return;
  }
  // Blend: A and B live in dedicated slots (not in `children`), and
  // blend_cache holds the generated intermediates. Transforms driven
  // off the returned list (drag, nudge, scale, rotate, skew) then
  // mutate sources AND cache in lock-step. Because the cache is
  // regenerated from A/B on every dirty rebuild, mutating cache
  // entries here is harmless — they'd get overwritten next frame
  // anyway — but including them keeps the bounding-box / transform
  // math correct *this* frame before the rebuild lands.
  if (obj->type == SceneNode::Type::Blend) {
    if (obj->blend_source_a)
      collect_paths(obj->blend_source_a.get(), out);
    if (obj->blend_source_b)
      collect_paths(obj->blend_source_b.get(), out);
    for (auto &step : obj->blend_cache)
      collect_paths(step.get(), out);
    return;
  }
  // Warp: only descend into warp_source (stable). The derived caches
  // (warp_glyph_cache, warp_cache) get regenerated on every envelope
  // edit — any pointer we hold into them becomes dangling as soon as
  // rebuild_warp_caches runs. Since callers use the returned list for
  // drag-begin snapshots consumed at drag-end, dangling pointers cause
  // use-after-free crashes (reproduced when selecting a Warp and
  // releasing without dragging — the overlay rebuild ran between press
  // and release). The caches will regenerate naturally from moved
  // source + moved envelope on the next draw, so we don't need them
  // in the drag-translate path.
  if (obj->type == SceneNode::Type::Warp) {
    if (obj->warp_source)
      collect_paths(obj->warp_source.get(), out);
    return;
  }
}

// ── collect_text_image_leaves
// ─────────────────────────────────────────────────────
// Complement to collect_paths: walks a SceneNode subtree and pushes every
// Text and Image node into `out`. Needed because the drag-move and nudge
// paths snapshot position fields (text_x/text_y, image_x/image_y) for
// those types separately — collect_paths only returns Path leaves.
//
// Containers recursed: Group, Compound, ClipGroup (including clip_shape,
// even though clip_shape can currently only be Path/Compound — kept
// symmetric with collect_paths for forward-compat).
// Text / Image themselves terminate recursion.
static void collect_text_image_leaves(SceneNode *obj,
                                      std::vector<SceneNode *> &out) {
  if (!obj)
    return;
  if (obj->type == SceneNode::Type::Text ||
      obj->type == SceneNode::Type::Image) {
    out.push_back(obj);
    return;
  }
  if (obj->type == SceneNode::Type::Group ||
      obj->type == SceneNode::Type::Compound ||
      obj->type == SceneNode::Type::ClipGroup) {
    for (auto &child : obj->children)
      collect_text_image_leaves(child.get(), out);
    if (obj->type == SceneNode::Type::ClipGroup && obj->clip_shape)
      collect_text_image_leaves(obj->clip_shape.get(), out);
  }
  // Blend: descend into A, B, and cache. In v1 sources are required to
  // be Paths so this always produces empty out, but we descend
  // symmetrically with collect_paths so future expansion (Text blend,
  // Image blend) doesn't need a separate pass here.
  if (obj->type == SceneNode::Type::Blend) {
    if (obj->blend_source_a)
      collect_text_image_leaves(obj->blend_source_a.get(), out);
    if (obj->blend_source_b)
      collect_text_image_leaves(obj->blend_source_b.get(), out);
    for (auto &step : obj->blend_cache)
      collect_text_image_leaves(step.get(), out);
  }
  // Warp: descend into source + both caches. In M1–M6 sources are
  // Path/Compound/Group so this produces empty `out`, but descending
  // symmetrically with collect_paths means the future text-source Warp
  // milestone won't need a separate pass here.
  if (obj->type == SceneNode::Type::Warp) {
    if (obj->warp_source)
      collect_text_image_leaves(obj->warp_source.get(), out);
    if (obj->warp_glyph_cache)
      collect_text_image_leaves(obj->warp_glyph_cache.get(), out);
    if (obj->warp_cache)
      collect_text_image_leaves(obj->warp_cache.get(), out);
  }
}

// ── rebuild_blend_cache
// ─────────────────────────────────────────────────────
// Regenerate obj->blend_cache from blend_source_a/b for a Blend node.
// Preconditions (enforced by make_blend at construction, locked
// thereafter): A and B are Paths with equal node counts and equal
// closed flag. If preconditions don't hold at call time, we clear the
// cache and LOG_WARN — render will paint A and B only, which is the
// least-surprising fallback.
//
// Interpolation math (linear, all fields):
//   For N = blend_steps intermediate paths, step i in 1..N uses
//     t = i / (N + 1)   // strictly between A and B, exclusive
//   For each node index k, lerp anchor, both handles, and keep A's
//   type (node type doesn't meaningfully interpolate — Smooth vs Cusp
//   is a discrete property).
//   closed = A.closed (== B.closed by precondition).
//
// Style interpolation (step gets own fill/stroke/opacity):
//   fill.type:   take A's if both are Solid/CurrentColor, otherwise A's
//                (None stays None). Colour interpolated in sRGB.
//   stroke.paint: same rule.
//   stroke.width: lerp(A, B) unless blend_stroke_w_user_set, in which
//                 case lerp(blend_stroke_w_start, blend_stroke_w_end).
//   opacity:     lerp(A.opacity, B.opacity).
//
// Cache entries are full SceneNodes of Type::Path, minted with fresh
// ids (each frame, that's fine — ids are render-only for cache; they
// never hit SVG since the cache isn't persisted as structure).
static void rebuild_blend_cache(SceneNode *obj) {
  if (!obj || obj->type != SceneNode::Type::Blend)
    return;
  obj->blend_cache.clear();
  obj->blend_cache_dirty = false;

  SceneNode *A = obj->blend_source_a.get();
  SceneNode *B = obj->blend_source_b.get();
  if (!A || !B || A->type != SceneNode::Type::Path ||
      B->type != SceneNode::Type::Path || !A->path || !B->path) {
    LOG_WARN("Canvas::rebuild_blend_cache: A or B missing/not-Path — "
             "cache cleared");
    return;
  }
  const auto &na = A->path->nodes;
  const auto &nb = B->path->nodes;
  if (na.size() != nb.size() || A->path->closed != B->path->closed) {
    LOG_WARN("Canvas::rebuild_blend_cache: node-count or closed mismatch "
             "(A={} B={} closedA={} closedB={}) — cache cleared",
             na.size(), nb.size(), A->path->closed, B->path->closed);
    return;
  }

  int N = std::clamp(obj->blend_steps, 1, 50);
  auto lerp = [](double x, double y, double t) { return x + (y - x) * t; };

  for (int i = 1; i <= N; ++i) {
    double t = double(i) / double(N + 1);

    auto step = std::make_unique<SceneNode>();
    step->type = SceneNode::Type::Path;
    step->path = std::make_unique<PathData>();
    step->path->closed = A->path->closed;
    step->path->nodes.reserve(na.size());
    for (size_t k = 0; k < na.size(); ++k) {
      BezierNode n;
      n.x = lerp(na[k].x, nb[k].x, t);
      n.y = lerp(na[k].y, nb[k].y, t);
      n.cx1 = lerp(na[k].cx1, nb[k].cx1, t);
      n.cy1 = lerp(na[k].cy1, nb[k].cy1, t);
      n.cx2 = lerp(na[k].cx2, nb[k].cx2, t);
      n.cy2 = lerp(na[k].cy2, nb[k].cy2, t);
      n.type = na[k].type;
      step->path->nodes.push_back(n);
    }

    // Fill: inherit A's Type (None/Solid/CurrentColor). Only interpolate
    // rgba when BOTH ends are Solid — CurrentColor ignores rgba at paint
    // time (apply_fill hardcodes gray), so lerping into CurrentColor's
    // default-zero rgba produces invisible steps. When a user hasn't
    // touched the paint, both sides are CurrentColor; intermediates
    // inherit that — same-colored gradient, visually consistent.
    step->fill.type = A->fill.type;
    if (A->fill.type == FillStyle::Type::Solid &&
        B->fill.type == FillStyle::Type::Solid) {
      step->fill.r = lerp(A->fill.r, B->fill.r, t);
      step->fill.g = lerp(A->fill.g, B->fill.g, t);
      step->fill.b = lerp(A->fill.b, B->fill.b, t);
      step->fill.a = lerp(A->fill.a, B->fill.a, t);
    } else {
      step->fill = A->fill;
    }

    // Stroke paint: same rule. Lerp only when BOTH sides are Solid.
    step->stroke.paint.type = A->stroke.paint.type;
    if (A->stroke.paint.type == FillStyle::Type::Solid &&
        B->stroke.paint.type == FillStyle::Type::Solid) {
      step->stroke.paint.r = lerp(A->stroke.paint.r, B->stroke.paint.r, t);
      step->stroke.paint.g = lerp(A->stroke.paint.g, B->stroke.paint.g, t);
      step->stroke.paint.b = lerp(A->stroke.paint.b, B->stroke.paint.b, t);
      step->stroke.paint.a = lerp(A->stroke.paint.a, B->stroke.paint.a, t);
    } else {
      step->stroke.paint = A->stroke.paint;
    }
    // Stroke width — user-override wins if set.
    if (obj->blend_stroke_w_user_set) {
      step->stroke.width =
          lerp(obj->blend_stroke_w_start, obj->blend_stroke_w_end, t);
    } else {
      step->stroke.width = lerp(A->stroke.width, B->stroke.width, t);
    }
    // Stroke adornments not interpolated in v1 — take A's.
    step->stroke.cap = A->stroke.cap;
    step->stroke.join = A->stroke.join;
    step->stroke.miter = A->stroke.miter;
    step->stroke.opacity = lerp(A->stroke.opacity, B->stroke.opacity, t);

    step->opacity = lerp(A->opacity, B->opacity, t);
    step->visible = true;
    step->locked = true; // cache is non-editable
    step->name = "blend-step-" + std::to_string(i);

    obj->blend_cache.push_back(std::move(step));
  }
}

static SceneNode *find_parent(CurvzDocument *doc, SceneNode *target,
                              int *out_idx) {
  if (!doc)
    return nullptr;
  // Search layers
  for (auto &layer : doc->layers) {
    for (int i = 0; i < (int)layer->children.size(); ++i) {
      if (layer->children[i].get() == target) {
        if (out_idx)
          *out_idx = i;
        return layer.get();
      }
    }
    // Search groups and compounds within layers (one level)
    for (auto &child : layer->children) {
      if (child->type != SceneNode::Type::Group &&
          child->type != SceneNode::Type::Compound)
        continue;
      for (int i = 0; i < (int)child->children.size(); ++i) {
        if (child->children[i].get() == target) {
          if (out_idx)
            *out_idx = i;
          return child.get();
        }
      }
    }
  }
  return nullptr;
}

void Canvas::make_compound_path() {
  if (!m_doc || m_selection.size() < 2)
    return;

  // All selected objects must be paths or compounds
  for (SceneNode *obj : m_selection)
    if (obj->type != SceneNode::Type::Path &&
        obj->type != SceneNode::Type::Compound)
      return;

  // Find parent — all selected must share the same parent
  int insert_idx = 0;
  SceneNode *parent = find_parent(m_doc, m_selection.front(), &insert_idx);
  if (!parent)
    return;

  // Verify all selected share the same parent
  for (SceneNode *obj : m_selection) {
    int idx = 0;
    SceneNode *p = find_parent(m_doc, obj, &idx);
    if (p != parent)
      return;
  }

  // ── Phase 1: expand any Compound members in-place ─────────────────────
  // For each selected Compound, pull its children out into the parent at
  // the compound's position and remove the compound. Collect all resulting
  // path pointers into an expanded selection in parent z-order.
  std::set<SceneNode *> sel_set(m_selection.begin(), m_selection.end());
  for (auto it = parent->children.begin(); it != parent->children.end();) {
    SceneNode *obj = it->get();
    if (!sel_set.count(obj) || obj->type != SceneNode::Type::Compound) {
      ++it;
      continue;
    }
    // Replace the compound with its children at the same position
    int pos = (int)(it - parent->children.begin());
    // Remove compound from sel_set — its children will be selected instead
    sel_set.erase(obj);
    std::vector<std::unique_ptr<SceneNode>> released;
    for (auto &child : obj->children)
      released.push_back(std::move(child));
    it = parent->children.erase(it); // removes compound, it now points to next
    // Insert children at pos (in order)
    for (int ci = 0; ci < (int)released.size(); ++ci) {
      sel_set.insert(released[ci].get());
      parent->children.insert(parent->children.begin() + pos + ci,
                              std::move(released[ci]));
    }
    // Advance past inserted children
    it = parent->children.begin() + pos + (int)released.size();
  }

  // ── Phase 2: find topmost position among expanded selection ───────────
  insert_idx = (int)parent->children.size();
  for (int i = 0; i < (int)parent->children.size(); ++i) {
    if (sel_set.count(parent->children[i].get()))
      insert_idx = std::min(insert_idx, i);
  }

  // ── Phase 3: build new Compound, moving expanded paths in z-order ─────
  auto compound = std::make_unique<SceneNode>();
  compound->type = SceneNode::Type::Compound;
  compound->internal_id = generate_internal_id();
  compound->name = m_doc->next_default_name(CurvzDocument::NameKind::Compound);

  size_t src_count = sel_set.size();
  for (auto it = parent->children.begin(); it != parent->children.end();) {
    if (sel_set.count(it->get())) {
      compound->children.push_back(std::move(*it));
      it = parent->children.erase(it);
    } else {
      ++it;
    }
  }

  // S58g: Seed the Compound's own fill/stroke/opacity from the first moved
  // child so the S58d/S58g "Compound owns its paint" rule doesn't silently
  // drop the user's colour at make-compound time. Without this, a Compound
  // created from two red paths would render as the default CurrentColor
  // because compound->fill was never initialised.
  if (!compound->children.empty() && compound->children.front()) {
    const SceneNode &first = *compound->children.front();
    compound->fill = first.fill;
    compound->stroke = first.stroke;
    compound->opacity = first.opacity;
  }

  insert_idx = std::min(insert_idx, (int)parent->children.size());
  SceneNode *cptr = compound.get();
  parent->children.insert(parent->children.begin() + insert_idx,
                          std::move(compound));

  m_selected = cptr;
  m_selection = {cptr};
  m_selected_node = -1;
  m_node_selection.clear();

  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: made compound from {} paths", src_count);
}

void Canvas::split_compound_path() {
  // Splits every Compound in the selection, not just the focused one.
  // Pre-s127 behaviour matched the singular menu language ("Split Compound
  // Path") but multi-selecting three compounds and hitting split would
  // only release the first — surprising on a multi-selection.
  //
  // Non-compound members of the selection are ignored (a mixed selection
  // is allowed; compounds split, paths stay put). After split, the new
  // selection is the union of all released children. No-op if the
  // selection contains no compounds.
  //
  // Note: like make_compound_path, this does not push to m_history. Both
  // ops are pending undo support — parity preserved here so a future fix
  // wires them together.
  if (!m_doc || m_selection.empty()) return;

  // Collect the compounds to split. Working off a stable copy because
  // we'll be mutating m_selection as the splits commit.
  std::vector<SceneNode *> targets;
  targets.reserve(m_selection.size());
  for (SceneNode *obj : m_selection) {
    if (obj && obj->type == SceneNode::Type::Compound && !obj->children.empty())
      targets.push_back(obj);
  }
  if (targets.empty()) return;

  std::vector<SceneNode *> new_sel;
  size_t total_released = 0;
  size_t compounds_split = 0;

  for (SceneNode *target : targets) {
    int insert_idx = 0;
    SceneNode *parent = find_parent(m_doc, target, &insert_idx);
    if (!parent) continue;

    // Pull children out of the compound, then erase the compound from
    // parent. Insert children at the compound's z-position, top-to-bottom
    // order preserved (matches the within-layer descending convention —
    // children[0] = top).
    std::vector<std::unique_ptr<SceneNode>> released;
    released.reserve(target->children.size());
    for (auto &child : target->children)
      released.push_back(std::move(child));

    parent->children.erase(parent->children.begin() + insert_idx);

    int idx = insert_idx;
    for (auto &child : released) {
      SceneNode *ptr = child.get();
      parent->children.insert(parent->children.begin() + idx, std::move(child));
      new_sel.push_back(ptr);
      ++idx;
    }
    total_released += released.size();
    ++compounds_split;
  }

  // Replace selection with the released children. If nothing was actually
  // split (e.g. all targets had unfindable parents — shouldn't happen in
  // practice), bail without mutating selection.
  if (new_sel.empty()) return;

  m_selection      = new_sel;
  m_selected       = new_sel.front();
  m_selected_node  = -1;
  m_node_selection.clear();

  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: split {} compound{} → {} paths",
           compounds_split, compounds_split == 1 ? "" : "s", total_released);
}

// ── Clipping paths
// ────────────────────────────────────────────────────────────
//
// Workflow:
//   1. User selects N objects of any type.
//   2. Object ▸ Clip menu calls make_clip_group(): arms pick mode,
//      snapshots the selection. The selection itself stays visible so
//      the user can see what they're about to clip.
//   3. User clicks on a Path or Compound in the canvas.
//      on_select_begin detects m_clip_pick_armed at its top and calls
//      finish_clip_pick(clicked_shape). Clicking anything else (empty
//      canvas, non-path, group, etc.) cancels and disarms.
//   4. finish_clip_pick extracts the stashed selection + the clicked
//      shape from their parent(s), wraps them in a new ClipGroup, and
//      inserts it at the topmost stashed-selection z-position of the
//      common parent.
//
// Constraints enforced:
//   - All stashed selection members must share one parent. The clip
//     shape must live in that same parent. (Simplest contract; keeps
//     the extraction unambiguous. Matches make_compound_path rules.)
//   - Clip shape must be a Path or Compound (not a Group, not a Text,
//     not another ClipGroup).
//   - Clip shape cannot itself be in the stashed selection.
//
// release_clip_group does the inverse: dissolves the ClipGroup, returns
// clip_shape and children as normal siblings in parent z-order
// (clip_shape on top — matches the Layer-panel convention where the
// clip shape is rendered above its children in outline mode).
void Canvas::make_clip_group() {
  if (!m_doc || m_selection.empty()) {
    LOG_INFO("Canvas: make_clip_group ignored — no selection");
    return;
  }
  // Reject if the selection contains anything that can't sensibly live
  // inside a ClipGroup. We allow Path, Compound, Group, Text, Image.
  // Refs/guides/measures/layers — no.
  for (SceneNode *obj : m_selection) {
    if (obj->is_ref() || obj->is_guide() || obj->is_measurement() ||
        obj->is_layer() || obj->is_special_layer()) {
      LOG_INFO("Canvas: make_clip_group rejected — selection contains a "
               "non-clippable node");
      return;
    }
  }
  m_clip_pick_armed = true;
  m_clip_pick_selection = m_selection;
  LOG_INFO("Canvas: clip-pick armed, selection={}",
           m_clip_pick_selection.size());
  // Status bar messaging could hook off is_clip_pick_armed() — not wired
  // here to keep Stage 2 diff small. StatusBar poll happens on cursor
  // move anyway.
  queue_draw();
}

// Called from on_select_begin when m_clip_pick_armed is true.
// Returns true if the click was consumed (armed state always cleared
// either way on return).
bool Canvas::finish_clip_pick(SceneNode *clicked) {
  // Disarm no matter what — a single arm = a single click opportunity.
  bool armed_was_true = m_clip_pick_armed;
  m_clip_pick_armed = false;
  std::vector<SceneNode *> stash;
  stash.swap(m_clip_pick_selection);

  if (!armed_was_true)
    return false;

  // Clicked nothing, or clicked an invalid shape → cancel silently.
  if (!clicked) {
    LOG_INFO("Canvas: clip-pick cancelled — empty click");
    queue_draw();
    return true; // still consume the click so selection doesn't reset
  }
  if (clicked->type != SceneNode::Type::Path &&
      clicked->type != SceneNode::Type::Compound) {
    LOG_INFO("Canvas: clip-pick cancelled — clicked node is not a Path or "
             "Compound");
    queue_draw();
    return true;
  }
  // Clip shape can't be in the stashed selection.
  if (std::find(stash.begin(), stash.end(), clicked) != stash.end()) {
    LOG_INFO("Canvas: clip-pick cancelled — clicked shape is part of the "
             "selection");
    queue_draw();
    return true;
  }

  // Find common parent of stashed selection.
  int dummy = 0;
  SceneNode *parent = find_parent(m_doc, stash.front(), &dummy);
  if (!parent) {
    LOG_INFO("Canvas: clip-pick aborted — selection has no parent");
    queue_draw();
    return true;
  }
  for (SceneNode *obj : stash) {
    int d = 0;
    if (find_parent(m_doc, obj, &d) != parent) {
      LOG_INFO("Canvas: clip-pick aborted — selection members across "
               "different parents");
      queue_draw();
      return true;
    }
  }
  // Clip shape must be in the same parent.
  int clip_idx_in_parent = 0;
  if (find_parent(m_doc, clicked, &clip_idx_in_parent) != parent) {
    LOG_INFO("Canvas: clip-pick aborted — clip shape has a different parent "
             "than the selection");
    queue_draw();
    return true;
  }

  // Compute insert index = topmost z-position among stash + clicked in parent.
  std::set<SceneNode *> extract_set(stash.begin(), stash.end());
  extract_set.insert(clicked);
  int insert_idx = (int)parent->children.size();
  for (int i = 0; i < (int)parent->children.size(); ++i) {
    if (extract_set.count(parent->children[i].get()))
      insert_idx = std::min(insert_idx, i);
  }

  // Build new ClipGroup.
  auto cg = std::make_unique<SceneNode>();
  cg->type = SceneNode::Type::ClipGroup;
  cg->internal_id = generate_internal_id();
  cg->name = m_doc->next_default_name(CurvzDocument::NameKind::Clip);
  cg->clip_id = "cp_" + cg->internal_id; // SVG defs id, stable

  // Extract the clip shape first, then the stashed children, preserving
  // their original z-order. Iterate once through parent->children.
  std::unique_ptr<SceneNode> clip_shape_owned;
  std::vector<std::unique_ptr<SceneNode>> children_owned;
  for (auto it = parent->children.begin(); it != parent->children.end();) {
    SceneNode *n = it->get();
    if (n == clicked) {
      clip_shape_owned = std::move(*it);
      it = parent->children.erase(it);
    } else if (extract_set.count(n)) {
      children_owned.push_back(std::move(*it));
      it = parent->children.erase(it);
    } else {
      ++it;
    }
  }

  cg->clip_shape = std::move(clip_shape_owned);
  cg->children = std::move(children_owned);

  insert_idx = std::min(insert_idx, (int)parent->children.size());
  SceneNode *cg_ptr = cg.get();
  parent->children.insert(parent->children.begin() + insert_idx, std::move(cg));

  m_selected = cg_ptr;
  m_selection = {cg_ptr};
  m_selected_node = -1;
  m_node_selection.clear();

  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: clip-pick done → ClipGroup '{}' with {} children",
           cg_ptr->name, (int)cg_ptr->children.size());
  return true;
}

void Canvas::release_clip_group() {
  if (!m_doc || !m_selected || !m_selected->is_clip_group()) {
    LOG_INFO("Canvas: release_clip_group ignored — selection is not a "
             "ClipGroup");
    return;
  }

  int cg_idx = 0;
  SceneNode *parent = find_parent(m_doc, m_selected, &cg_idx);
  if (!parent)
    return;

  SceneNode *cg = m_selected;

  // Collect releasable children: clip_shape first (goes on top), then
  // the clipped children in their existing order.
  std::vector<std::unique_ptr<SceneNode>> released;
  if (cg->clip_shape)
    released.push_back(std::move(cg->clip_shape));
  for (auto &ch : cg->children)
    released.push_back(std::move(ch));
  cg->children.clear();

  // Remove ClipGroup from parent.
  parent->children.erase(parent->children.begin() + cg_idx);

  // Insert released nodes at cg_idx in order (first = top = cg_idx).
  std::vector<SceneNode *> new_sel;
  int idx = cg_idx;
  for (auto &n : released) {
    SceneNode *ptr = n.get();
    parent->children.insert(parent->children.begin() + idx, std::move(n));
    new_sel.push_back(ptr);
    ++idx;
  }

  m_selection = new_sel;
  m_selected = new_sel.empty() ? nullptr : new_sel.front();
  m_selected_node = -1;
  m_node_selection.clear();

  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: released ClipGroup → {} objects", (int)new_sel.size());
}

// ── Canvas::make_blend ───────────────────────────────────────────────────────
// M1 scope: exactly two Path nodes selected, sharing a common parent,
// with equal node counts and equal closed-flag. Violations are
// rejected with LOG_WARN + a user-visible message; M3 dialog will
// cover the unequal-node-count path with interactive equalization,
// M4 will insert nodes via De Casteljau midpoint splits.
//
// Operation:
//   1. Validate selection (exactly 2, both Path, same parent, equal
//      node counts, equal closed flag).
//   2. Snapshot both originals for undo (with their indices).
//   3. Build the Blend node: type=Blend, name="Blend", deep-clone A
//      and B into blend_source_a/b, blend_steps=4, dirty=true. First
//      draw will call rebuild_blend_cache.
//   4. Remove both originals from parent (descending index order).
//   5. Insert the Blend at the lower of the two original indices.
//   6. Push MakeBlendCommand (atomic undo).
//   7. Select the new Blend, emit signals.
void Canvas::make_blend(int steps, bool reverse, bool stroke_w_override,
                        double stroke_w_start, double stroke_w_end) {
  if (!m_doc)
    return;

  if (m_selection.size() != 2) {
    LOG_WARN("Canvas::make_blend: need exactly 2 selected, got {}",
             m_selection.size());
    m_sig_show_message.emit("Blend",
                            "Blend requires exactly two selected paths.");
    return;
  }
  SceneNode *a = m_selection[0];
  SceneNode *b = m_selection[1];
  if (reverse)
    std::swap(a, b);
  if (!a || !b || a->type != SceneNode::Type::Path ||
      b->type != SceneNode::Type::Path || !a->path || !b->path) {
    LOG_WARN("Canvas::make_blend: both selections must be Paths");
    m_sig_show_message.emit(
        "Blend",
        "Blend requires both selected objects to be paths. Compound and "
        "other types are not supported in this version.");
    return;
  }

  int idx_a = -1, idx_b = -1;
  SceneNode *pa = find_parent(m_doc, a, &idx_a);
  SceneNode *pb = find_parent(m_doc, b, &idx_b);
  if (!pa || !pb || pa != pb) {
    LOG_WARN("Canvas::make_blend: sources must share the same parent");
    m_sig_show_message.emit(
        "Blend", "Blend requires both paths to be in the same group or layer.");
    return;
  }
  SceneNode *parent = pa;

  // M1: reject unequal node counts / mismatched closed flag. M4 will
  // offer interactive equalization via De Casteljau midpoint inserts.
  if (a->path->nodes.size() != b->path->nodes.size() ||
      a->path->closed != b->path->closed) {
    LOG_WARN("Canvas::make_blend: node-count/closed mismatch (A={} B={} "
             "closedA={} closedB={})",
             a->path->nodes.size(), b->path->nodes.size(), a->path->closed,
             b->path->closed);
    m_sig_show_message.emit(
        "Blend", "The two paths must have the same number of nodes and both be "
                 "open or both be closed. Automatic node-count equalization is "
                 "not yet implemented.");
    return;
  }

  // Order by z-index: "lower" (earlier in children) = bottom. Blend
  // takes the lower slot so it occupies the combined z-range without
  // jumping to the top.
  int lo_idx = std::min(idx_a, idx_b);
  int hi_idx = std::max(idx_a, idx_b);

  // Snapshot originals in ascending index order (matches MakeBlendCommand
  // insertion order on undo).
  std::vector<MakeBlendCommand::Original> originals;
  originals.push_back({clone_node(*parent->children[lo_idx]), lo_idx});
  originals.push_back({clone_node(*parent->children[hi_idx]), hi_idx});

  // Build Blend node. Deep-clone A and B into slots so the Blend is
  // self-contained — sources can be mutated via Layers panel without
  // aliasing into freed memory after we remove them from `children`.
  auto blend = std::make_unique<SceneNode>();
  blend->type = SceneNode::Type::Blend;
  blend->id = next_id();
  blend->internal_id = last_iid();
  blend->name = m_doc->next_default_name(CurvzDocument::NameKind::Blend);
  blend->visible = true;
  blend->locked = false;
  blend->opacity = 1.0;
  blend->blend_source_a = clone_node(*a);
  blend->blend_source_b = clone_node(*b);
  blend->blend_steps = std::clamp(steps, 1, 50);
  blend->blend_stroke_w_user_set = stroke_w_override;
  blend->blend_stroke_w_start = stroke_w_start;
  blend->blend_stroke_w_end = stroke_w_end;
  blend->blend_cache_dirty = true;

  // Remove both originals — descending index order so lo_idx stays valid.
  // After this point, `a` and `b` are dangling pointers.
  parent->children.erase(parent->children.begin() + hi_idx);
  parent->children.erase(parent->children.begin() + lo_idx);

  int ins = std::clamp(lo_idx, 0, (int)parent->children.size());
  SceneNode *blend_ptr = blend.get();

  // Snapshot for undo before we move the unique_ptr.
  auto blend_snap = clone_node(*blend);

  parent->children.insert(parent->children.begin() + ins, std::move(blend));

  if (m_history)
    m_history->push(std::make_unique<MakeBlendCommand>(
        parent, std::move(originals), std::move(blend_snap), ins));

  m_selected = blend_ptr;
  m_selection = {blend_ptr};
  m_selected_node = -1;
  m_node_selection.clear();

  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: made Blend '{}' with {} source nodes each, steps={}",
           blend_ptr->name, blend_ptr->blend_source_a->path->nodes.size(),
           blend_ptr->blend_steps);
}

// ── Canvas::find_blend_owner ─────────────────────────────────────────────────
// Returns the Blend SceneNode whose blend_source_a or blend_source_b slot
// holds `target`, or nullptr if none. Walks the full tree; Blends are
// rare enough that this is cheap. Used by delete_selected to route
// deletion of A/B through release_blend (matches ClipGroup semantics:
// deleting the clip_shape dissolves, restoring the clipped children).
SceneNode *Canvas::find_blend_owner(SceneNode *target) {
  if (!m_doc || !target)
    return nullptr;
  std::function<SceneNode *(SceneNode *)> walk =
      [&](SceneNode *root) -> SceneNode * {
    if (!root)
      return nullptr;
    if (root->is_blend()) {
      if (root->blend_source_a.get() == target ||
          root->blend_source_b.get() == target)
        return root;
      // Descend into Blend's slots as well, in case of future nested Blends.
      if (auto *r = walk(root->blend_source_a.get()))
        return r;
      if (auto *r = walk(root->blend_source_b.get()))
        return r;
    }
    for (auto &c : root->children)
      if (auto *r = walk(c.get()))
        return r;
    if (root->clip_shape)
      if (auto *r = walk(root->clip_shape.get()))
        return r;
    return nullptr;
  };
  for (auto &layer : m_doc->layers)
    if (auto *r = walk(layer.get()))
      return r;
  return nullptr;
}

// ── Canvas::mark_all_blends_dirty ────────────────────────────────────────────
// Walks every SceneNode in the doc and sets blend_cache_dirty=true on any
// Blend encountered. Called from the prop_changed signal pathway so that
// downstream inspector edits (fill, stroke, opacity, node anchors/handles)
// invalidate cached intermediates. Next draw's lazy rebuild picks it up.
// Cheap — just a bool flip — and correct even when the edited node wasn't
// a Blend source (false positive just triggers one redundant rebuild).
void Canvas::mark_all_blends_dirty() {
  if (!m_doc)
    return;
  std::function<void(SceneNode *)> walk = [&](SceneNode *n) {
    if (!n)
      return;
    if (n->is_blend())
      n->blend_cache_dirty = true;
    for (auto &c : n->children)
      walk(c.get());
    if (n->clip_shape)
      walk(n->clip_shape.get());
    if (n->is_blend()) {
      walk(n->blend_source_a.get());
      walk(n->blend_source_b.get());
    }
    if (n->is_warp()) {
      walk(n->warp_source.get());
      walk(n->warp_glyph_cache.get());
      walk(n->warp_cache.get());
    }
  };
  for (auto &layer : m_doc->layers)
    walk(layer.get());
}

// ── Canvas::find_warp_owner ──────────────────────────────────────────────────
// Returns the Warp SceneNode whose warp_source slot transitively holds
// `target`, or nullptr if none. Walks the full tree. A hit inside the
// source's own subtree (e.g. a Path inside a Group that is warp_source)
// also counts — matches ClipGroup/Blend semantics where touching the
// payload routes through the wrapper.
SceneNode *Canvas::find_warp_owner(SceneNode *target) {
  if (!m_doc || !target)
    return nullptr;
  // Subtree containment check — does `root` contain `target` somewhere?
  std::function<bool(SceneNode *)> contains = [&](SceneNode *root) -> bool {
    if (!root)
      return false;
    if (root == target)
      return true;
    for (auto &c : root->children)
      if (contains(c.get()))
        return true;
    if (root->clip_shape && contains(root->clip_shape.get()))
      return true;
    if (root->is_blend()) {
      if (contains(root->blend_source_a.get()))
        return true;
      if (contains(root->blend_source_b.get()))
        return true;
      for (auto &s : root->blend_cache)
        if (contains(s.get()))
          return true;
    }
    if (root->is_warp()) {
      if (contains(root->warp_source.get()))
        return true;
      if (contains(root->warp_glyph_cache.get()))
        return true;
      if (contains(root->warp_cache.get()))
        return true;
    }
    return false;
  };
  std::function<SceneNode *(SceneNode *)> walk =
      [&](SceneNode *root) -> SceneNode * {
    if (!root)
      return nullptr;
    if (root->is_warp()) {
      if (contains(root->warp_source.get()))
        return root;
      // Descend into Warp slots in case of future nested Warps.
      if (auto *r = walk(root->warp_source.get()))
        return r;
      if (auto *r = walk(root->warp_glyph_cache.get()))
        return r;
      if (auto *r = walk(root->warp_cache.get()))
        return r;
    }
    for (auto &c : root->children)
      if (auto *r = walk(c.get()))
        return r;
    if (root->clip_shape)
      if (auto *r = walk(root->clip_shape.get()))
        return r;
    if (root->is_blend()) {
      if (auto *r = walk(root->blend_source_a.get()))
        return r;
      if (auto *r = walk(root->blend_source_b.get()))
        return r;
    }
    return nullptr;
  };
  for (auto &layer : m_doc->layers)
    if (auto *r = walk(layer.get()))
      return r;
  return nullptr;
}

// ── Canvas::mark_all_warps_dirty ─────────────────────────────────────────────
// Walks every SceneNode and sets both warp_glyph_cache_dirty and
// warp_cache_dirty on any Warp encountered. Called from prop_changed
// signal pathway so downstream inspector edits to source geometry
// invalidate cached outlines/warped geometry on next draw. Coarse but
// cheap — one pair of bool flips per Warp, zero cost when no Warps exist.
void Canvas::mark_all_warps_dirty() {
  if (!m_doc)
    return;
  std::function<void(SceneNode *)> walk = [&](SceneNode *n) {
    if (!n)
      return;
    if (n->is_warp()) {
      n->warp_glyph_cache_dirty = true;
      n->warp_cache_dirty = true;
    }
    for (auto &c : n->children)
      walk(c.get());
    if (n->clip_shape)
      walk(n->clip_shape.get());
    if (n->is_blend()) {
      walk(n->blend_source_a.get());
      walk(n->blend_source_b.get());
    }
    if (n->is_warp()) {
      walk(n->warp_source.get());
      walk(n->warp_glyph_cache.get());
      walk(n->warp_cache.get());
    }
  };
  for (auto &layer : m_doc->layers)
    walk(layer.get());
}

// ─────────────────────────────────────────────────────────────────────────────
// Warp math (M2)
// ─────────────────────────────────────────────────────────────────────────────
//
// Model: ruled surface between two Bézier boundary curves.
//
//   P(u, v) = (1 - v) · C_bottom(u) + v · C_top(u)      for u,v ∈ [0,1]
//
// where:
//   u ∈ [0,1] is horizontal position, normalized across the glyph_cache bbox
//   v ∈ [0,1] is vertical position, normalized across the glyph_cache bbox
//   C_top(u), C_bottom(u) are the top and bottom envelope curves, each a
//     PathData with 1..N cubic segments (N = nodes.size()-1 for an open
//     polybezier).
//
// Left and right sides are implicit straight lines between the top/bottom
// corner endpoints — trapezoidal perspective warps fall out naturally when
// the top is narrower than the bottom, no special-case needed.
//
// u parametrization: segment-uniform. A 3-segment envelope treats u as
// [0, 1/3] = segment 0, [1/3, 2/3] = segment 1, [2/3, 1] = segment 2.
// This is simpler than arclength-accurate and matches how users draw
// multi-anchor envelopes (each segment controls roughly the same amount
// of output). Arclength-accurate can be a later refinement.
//
// Subdivision: warp_quality (1..16) controls how many equal t-slices each
// source cubic segment is split into before warping. Each slice produces
// one output cubic fit via the "implicit-handle through 4 samples"
// technique — given warped p(0), p(1/3), p(2/3), p(1), solve for the
// cubic's two handle control points exactly. Cheap, accurate for cubics,
// no least-squares needed.

// Evaluate a single cubic segment at parameter t ∈ [0,1].
// p0/p3 are the anchor endpoints, p1/p2 the bezier handles (absolute coords).
static void eval_cubic(double t, double p0x, double p0y, double p1x, double p1y,
                       double p2x, double p2y, double p3x, double p3y,
                       double &ox, double &oy) {
  double mt = 1.0 - t;
  double mt2 = mt * mt;
  double t2 = t * t;
  double a = mt2 * mt;
  double b = 3.0 * mt2 * t;
  double c = 3.0 * mt * t2;
  double d = t2 * t;
  ox = a * p0x + b * p1x + c * p2x + d * p3x;
  oy = a * p0y + b * p1y + c * p2y + d * p3y;
}

// Evaluate an envelope curve (a PathData with 1..N cubic segments) at the
// global parameter u ∈ [0,1]. Uses segment-uniform parametrization: each
// segment gets an equal slice of u.
//
// Degenerate fallbacks:
//   empty nodes           → (0,0), caller handles
//   single node           → that anchor's position
//   single cubic (2 nodes)→ eval_cubic on the one segment
static void eval_envelope(const PathData &env, double u, double &ox,
                          double &oy) {
  const auto &ns = env.nodes;
  if (ns.empty()) {
    ox = 0.0;
    oy = 0.0;
    return;
  }
  if (ns.size() == 1) {
    ox = ns[0].x;
    oy = ns[0].y;
    return;
  }
  int seg_count = (int)ns.size() - 1; // open polybezier: N nodes = N-1 segs
  if (seg_count <= 0) {
    ox = ns[0].x;
    oy = ns[0].y;
    return;
  }
  // Clamp u to [0,1]
  if (u < 0.0)
    u = 0.0;
  if (u > 1.0)
    u = 1.0;
  // Which segment? Segment i owns [i/seg_count, (i+1)/seg_count].
  double scaled = u * seg_count;
  int seg = (int)scaled;
  if (seg >= seg_count)
    seg = seg_count - 1; // u == 1.0 edge case
  double local_t = scaled - (double)seg;
  const BezierNode &a = ns[seg];
  const BezierNode &b = ns[seg + 1];
  // Outgoing handle of a is a.cx2/cy2; incoming handle of b is b.cx1/cy1
  eval_cubic(local_t, a.x, a.y, a.cx2, a.cy2, b.cx1, b.cy1, b.x, b.y, ox, oy);
}

// Normalize a doc-space point (px, py) into (u, v) ∈ [0,1]² given a
// glyph_cache bbox. No clamping — points outside the bbox extrapolate,
// which is fine for degenerate inputs (extrapolation just lands outside
// the envelope, which is where they'd land anyway).
static void normalize_uv(double px, double py, double bbx, double bby,
                         double bbw, double bbh, double &u, double &v) {
  u = (bbw > 1e-12) ? (px - bbx) / bbw : 0.0;
  v = (bbh > 1e-12) ? (py - bby) / bbh : 0.0;
}

// Map a doc-space point through the warp envelope. Returns the warped
// position. envelope_top is at v=1 (top of source bbox maps to top curve);
// envelope_bottom is at v=0 (bottom of source bbox maps to bottom curve).
//
// Y convention: PathData is stored in doc space Y-down (canonical Cairo
// convention used throughout Curvz). Source bbox y=bby is the top edge
// in Y-down, y=bby+bbh is the bottom. We want the point with v=0 (small
// y) to map to the TOP envelope and v=1 (large y) to map to the BOTTOM
// envelope. So warp_env_top corresponds to v=0 and warp_env_bottom to
// v=1 in our blend.
static void warp_point(double px, double py, double bbx, double bby, double bbw,
                       double bbh, const PathData &env_top,
                       const PathData &env_bottom, double &ox, double &oy) {
  double u, v;
  normalize_uv(px, py, bbx, bby, bbw, bbh, u, v);
  double tx, ty, bx, by;
  eval_envelope(env_top, u, tx, ty);
  eval_envelope(env_bottom, u, bx, by);
  // v=0 → top envelope, v=1 → bottom envelope (Y-down doc space)
  double mv = 1.0 - v;
  ox = mv * tx + v * bx;
  oy = mv * ty + v * by;
}

// Fit a cubic Bézier through four samples at t = 0, 1/3, 2/3, 1.
// Given: q0 = p(0), q1 = p(1/3), q2 = p(2/3), q3 = p(1)
// Cubic: p(t) = (1-t)³·c0 + 3(1-t)²t·c1 + 3(1-t)t²·c2 + t³·c3
// c0 = q0 and c3 = q3 (endpoints match).
// At t=1/3: q1 = 8/27·c0 + 12/27·c1 + 6/27·c2 + 1/27·c3
// At t=2/3: q2 = 1/27·c0 + 6/27·c1 + 12/27·c2 + 8/27·c3
// Solving this 2×2 system for c1, c2:
//   c1 = ( -5·q0 + 18·q1 -  9·q2 + 2·q3) / 6
//   c2 = (  2·q0 -  9·q1 + 18·q2 - 5·q3) / 6
static void fit_cubic_4samples(double q0x, double q0y, double q1x, double q1y,
                               double q2x, double q2y, double q3x, double q3y,
                               double &c1x, double &c1y, double &c2x,
                               double &c2y) {
  c1x = (-5.0 * q0x + 18.0 * q1x - 9.0 * q2x + 2.0 * q3x) / 6.0;
  c1y = (-5.0 * q0y + 18.0 * q1y - 9.0 * q2y + 2.0 * q3y) / 6.0;
  c2x = (2.0 * q0x - 9.0 * q1x + 18.0 * q2x - 5.0 * q3x) / 6.0;
  c2y = (2.0 * q0y - 9.0 * q1y + 18.0 * q2y - 5.0 * q3y) / 6.0;
}

// Axis-aligned bbox of a PathData (anchors only — sufficient for
// normalization purposes; handles that poke outside the anchor bbox
// just extrapolate slightly, which is harmless). Returns false for
// empty paths. Matches canonical Y-down doc space.
static bool path_anchor_bbox(const PathData &pd, double &bx, double &by,
                             double &bw, double &bh) {
  if (pd.nodes.empty())
    return false;
  double minx = pd.nodes[0].x, maxx = pd.nodes[0].x;
  double miny = pd.nodes[0].y, maxy = pd.nodes[0].y;
  for (const auto &n : pd.nodes) {
    minx = std::min(minx, n.x);
    maxx = std::max(maxx, n.x);
    miny = std::min(miny, n.y);
    maxy = std::max(maxy, n.y);
  }
  bx = minx;
  by = miny;
  bw = maxx - minx;
  bh = maxy - miny;
  // Guard against degenerate zero-width or zero-height paths
  if (bw < 1e-9)
    bw = 1e-9;
  if (bh < 1e-9)
    bh = 1e-9;
  return true;
}

// Bbox across the union of leaf paths in a subtree — walks into Group /
// Compound children to find the overall extent of the source payload.
// Used to compute the (u,v) normalization frame for the whole Warp.
static bool subtree_path_bbox(const SceneNode *n, double &bx, double &by,
                              double &bw, double &bh) {
  if (!n)
    return false;
  bool found = false;
  double minx = 0, miny = 0, maxx = 0, maxy = 0;
  std::function<void(const SceneNode *)> walk = [&](const SceneNode *nd) {
    if (!nd)
      return;
    if (nd->type == SceneNode::Type::Path && nd->path &&
        !nd->path->nodes.empty()) {
      double lx, ly, lw, lh;
      if (!path_anchor_bbox(*nd->path, lx, ly, lw, lh))
        return;
      if (!found) {
        minx = lx;
        miny = ly;
        maxx = lx + lw;
        maxy = ly + lh;
        found = true;
      } else {
        minx = std::min(minx, lx);
        miny = std::min(miny, ly);
        maxx = std::max(maxx, lx + lw);
        maxy = std::max(maxy, ly + lh);
      }
      return;
    }
    // Recurse into containers
    for (const auto &c : nd->children)
      walk(c.get());
  };
  walk(n);
  if (!found)
    return false;
  bx = minx;
  by = miny;
  bw = std::max(maxx - minx, 1e-9);
  bh = std::max(maxy - miny, 1e-9);
  return true;
}

// Seed a Warp's envelope curves as straight 2-anchor lines across the
// top and bottom of the glyph_cache bbox. Identity warp — the output
// will look exactly like the input. Called when rebuild_warp_cache is
// asked to work on a Warp whose envelopes are empty (hand-built in
// test code, or a freshly-parsed M1-stub file that had no envelope
// encoding). M3's MakeWarpCommand will also call this at construction.
//
// Handles are colinear with the endpoints (classic "straight Bézier"
// with handles at 1/3 and 2/3 along the line), so the curve IS a line.
static void default_envelope_from_bbox(PathData &env_top, PathData &env_bottom,
                                       double bx, double by, double bw,
                                       double bh) {
  env_top.nodes.clear();
  env_top.closed = false;
  env_bottom.nodes.clear();
  env_bottom.closed = false;
  // Top envelope — two anchors at top corners. y = by (top in Y-down).
  double x0 = bx, x1 = bx + bw;
  double yt = by, yb = by + bh;
  auto mk = [](double ax, double ay) {
    BezierNode n;
    n.x = ax;
    n.y = ay;
    n.cx1 = ax;
    n.cy1 = ay;
    n.cx2 = ax;
    n.cy2 = ay;
    return n;
  };
  // Position handles at 1/3 along the straight segment so the cubic
  // degenerates to the line (tangent direction matches the chord).
  double dx = (x1 - x0) / 3.0;
  BezierNode t0 = mk(x0, yt);
  t0.cx2 = x0 + dx;
  t0.cy2 = yt;
  BezierNode t1 = mk(x1, yt);
  t1.cx1 = x1 - dx;
  t1.cy1 = yt;
  env_top.nodes.push_back(t0);
  env_top.nodes.push_back(t1);
  BezierNode b0 = mk(x0, yb);
  b0.cx2 = x0 + dx;
  b0.cy2 = yb;
  BezierNode b1 = mk(x1, yb);
  b1.cx1 = x1 - dx;
  b1.cy1 = yb;
  env_bottom.nodes.push_back(b0);
  env_bottom.nodes.push_back(b1);
}

// Warp a single PathData through the envelope, producing a new PathData.
// Each source cubic segment between nodes[i] and nodes[i+1] is split
// into `quality` equal t-slices, each slice warped and fit as a single
// cubic via the 4-sample method. Output has (source_seg_count * quality)
// cubic segments, plus the original closed-flag and one more leading
// anchor (for N+1 anchors describing N segments).
//
// Handles of the first and last output anchors (cx1 of [0], cx2 of
// [last]) inherit from the colinear fit for continuity with neighbours
// on closed paths. For open paths they can be left equal to the anchor
// (effectively untangented at the endpoints).
static PathData warp_path_data(const PathData &src, const PathData &env_top,
                               const PathData &env_bottom, double bx, double by,
                               double bw, double bh, int quality) {
  PathData out;
  out.closed = src.closed;
  if (src.nodes.size() < 2) {
    // Single-anchor or empty path — warp the lone anchor (if any) and
    // return a degenerate path.
    for (const auto &n : src.nodes) {
      double wx, wy;
      warp_point(n.x, n.y, bx, by, bw, bh, env_top, env_bottom, wx, wy);
      BezierNode wn;
      wn.x = wx;
      wn.y = wy;
      wn.cx1 = wx;
      wn.cy1 = wy;
      wn.cx2 = wx;
      wn.cy2 = wy;
      out.nodes.push_back(wn);
    }
    return out;
  }
  if (quality < 1)
    quality = 1;
  if (quality > 16)
    quality = 16;
  // For a closed path the "segment" from nodes[last] back to nodes[0]
  // also warps. Open paths stop at nodes[last]. We iterate segments
  // accordingly.
  int n_nodes = (int)src.nodes.size();
  int n_segs = src.closed ? n_nodes : (n_nodes - 1);
  // Preallocate first anchor (gets filled below with first slice's p0).
  // Each segment emits `quality` output segments; for the very first
  // segment of an open path we also need to push the leading anchor
  // before the first slice.
  bool first_anchor_emitted = false;
  for (int si = 0; si < n_segs; ++si) {
    const BezierNode &a = src.nodes[si];
    const BezierNode &b = src.nodes[(si + 1) % n_nodes];
    for (int q = 0; q < quality; ++q) {
      double t0 = (double)q / quality;
      double t1 = (double)(q + 1) / quality;
      double t13 = t0 + (t1 - t0) / 3.0;
      double t23 = t0 + 2.0 * (t1 - t0) / 3.0;
      // Sample the source cubic at t0, 1/3, 2/3, t1 and warp each sample
      double s0x, s0y, s1x, s1y, s2x, s2y, s3x, s3y;
      eval_cubic(t0, a.x, a.y, a.cx2, a.cy2, b.cx1, b.cy1, b.x, b.y, s0x, s0y);
      eval_cubic(t13, a.x, a.y, a.cx2, a.cy2, b.cx1, b.cy1, b.x, b.y, s1x, s1y);
      eval_cubic(t23, a.x, a.y, a.cx2, a.cy2, b.cx1, b.cy1, b.x, b.y, s2x, s2y);
      eval_cubic(t1, a.x, a.y, a.cx2, a.cy2, b.cx1, b.cy1, b.x, b.y, s3x, s3y);
      double w0x, w0y, w1x, w1y, w2x, w2y, w3x, w3y;
      warp_point(s0x, s0y, bx, by, bw, bh, env_top, env_bottom, w0x, w0y);
      warp_point(s1x, s1y, bx, by, bw, bh, env_top, env_bottom, w1x, w1y);
      warp_point(s2x, s2y, bx, by, bw, bh, env_top, env_bottom, w2x, w2y);
      warp_point(s3x, s3y, bx, by, bw, bh, env_top, env_bottom, w3x, w3y);
      // Fit the warped cubic: control points c1, c2 from the 4 samples
      double c1x, c1y, c2x, c2y;
      fit_cubic_4samples(w0x, w0y, w1x, w1y, w2x, w2y, w3x, w3y, c1x, c1y, c2x,
                         c2y);
      // Emit anchor w0 on the first slice (or when starting a new
      // segment's first slice on an open path's very first iteration)
      if (!first_anchor_emitted) {
        BezierNode wn;
        wn.x = w0x;
        wn.y = w0y;
        wn.cx1 = w0x;
        wn.cy1 = w0y; // patched below if prev slice exists
        wn.cx2 = c1x;
        wn.cy2 = c1y;
        out.nodes.push_back(wn);
        first_anchor_emitted = true;
      } else {
        // Patch the previous anchor's outgoing handle to match THIS
        // slice's c1. (It was provisionally set from the PREVIOUS
        // slice's fit; overwriting with the continuation maintains
        // C0 continuity and locks in per-slice tangents.)
        BezierNode &prev = out.nodes.back();
        prev.cx2 = c1x;
        prev.cy2 = c1y;
      }
      // Emit anchor w3 with incoming handle c2 and outgoing provisional
      // (set equal to w3 for now; next slice will overwrite, or if this
      // is the last slice on a closed path, the first-anchor patch
      // below finishes the loop).
      BezierNode wn;
      wn.x = w3x;
      wn.y = w3y;
      wn.cx1 = c2x;
      wn.cy1 = c2y;
      wn.cx2 = w3x;
      wn.cy2 = w3y;
      out.nodes.push_back(wn);
    }
  }
  // Closed path: the last emitted anchor is actually a duplicate of the
  // first (we walked N segments and emitted N+1 anchors, but on a closed
  // path nodes[0] == nodes[last] logically). Merge the duplicate:
  // take the last anchor's cx1 and move it to the first anchor's cx1,
  // then drop the last anchor.
  if (src.closed && out.nodes.size() > 1) {
    BezierNode &last = out.nodes.back();
    BezierNode &first = out.nodes.front();
    first.cx1 = last.cx1;
    first.cy1 = last.cy1;
    out.nodes.pop_back();
  }
  return out;
}

// Walk src recursively, producing a structurally-parallel tree in dst
// where every Path has its PathData replaced by the warped version.
// Group/Compound containers are cloned preserving type, children are
// recursed into. Non-path leaves (Text, Image, Ref — which don't appear
// in the M1-M6 source types but future text Warp will hit) are cloned
// as-is for forward-compat; they render un-warped, which is the
// reasonable fallback until text-warp math lands.
static std::unique_ptr<SceneNode> warp_subtree(const SceneNode &src,
                                               const PathData &env_top,
                                               const PathData &env_bottom,
                                               double bx, double by, double bw,
                                               double bh, int quality) {
  auto dst = std::make_unique<SceneNode>();
  dst->type = src.type;
  dst->id = src.id;
  dst->internal_id = src.internal_id;
  dst->name = src.name;
  dst->visible = src.visible;
  dst->locked = src.locked;
  dst->opacity = src.opacity;
  dst->color = src.color;
  dst->transform = src.transform;
  dst->fill = src.fill;
  dst->stroke = src.stroke;
  dst->fill_swatch_id = src.fill_swatch_id;
  dst->stroke_swatch_id = src.stroke_swatch_id;
  if (src.type == SceneNode::Type::Path && src.path) {
    dst->path = std::make_unique<PathData>(warp_path_data(
        *src.path, env_top, env_bottom, bx, by, bw, bh, quality));
  }
  for (const auto &c : src.children) {
    dst->children.push_back(
        warp_subtree(*c, env_top, env_bottom, bx, by, bw, bh, quality));
  }
  return dst;
}

// ── Canvas::rebuild_warp_caches ──────────────────────────────────────────────
// Brings both caches in sync with the source + envelope state. Three-phase:
//
//   1. Glyph cache: deep-clone source → glyph_cache. This is a no-op for
//      today's Path/Compound/Group sources (glyph_cache is literally a
//      copy). Future text-source Warp replaces this with render-string-
//      to-outlines. Triggered by warp_glyph_cache_dirty.
//
//   2. Envelope defaults: if env_top or env_bottom is empty, seed them
//      from the glyph_cache bbox as straight 2-anchor lines. Identity
//      warp. Lets hand-constructed and freshly-parsed M1-stub Warps
//      render without blowing up.
//
//   3. Warp cache: walk glyph_cache, warp each path through the
//      envelope using warp_quality subdivision, emit into warp_cache.
//      Triggered by warp_cache_dirty.
//
// Safe on nodes that aren't Warps (no-op). Safe on Warps with null
// source (clears caches, logs warn). Idempotent — calling twice with
// no state change does nothing after the first.
void Canvas::rebuild_warp_caches(SceneNode *w) {
  if (!w || !w->is_warp())
    return;
  if (!w->warp_source) {
    LOG_WARN("Canvas::rebuild_warp_caches: Warp '{}' has null source — "
             "clearing caches",
             w->name);
    w->warp_glyph_cache.reset();
    w->warp_cache.reset();
    w->warp_glyph_cache_dirty = false;
    w->warp_cache_dirty = false;
    return;
  }
  // Phase 1: glyph cache
  if (w->warp_glyph_cache_dirty || !w->warp_glyph_cache) {
    w->warp_glyph_cache = clone_node(*w->warp_source);
    w->warp_glyph_cache_dirty = false;
    // Changing glyph cache implies warp cache must also rebuild
    w->warp_cache_dirty = true;
  }
  // Phase 2: compute bbox across glyph_cache, seed default envelopes
  double bx = 0, by = 0, bw = 0, bh = 0;
  if (!subtree_path_bbox(w->warp_glyph_cache.get(), bx, by, bw, bh)) {
    LOG_WARN("Canvas::rebuild_warp_caches: Warp '{}' glyph_cache has no "
             "paths — cannot compute bbox, clearing warp_cache",
             w->name);
    w->warp_cache.reset();
    w->warp_cache_dirty = false;
    return;
  }
  if (w->warp_env_top.nodes.empty() || w->warp_env_bottom.nodes.empty()) {
    default_envelope_from_bbox(w->warp_env_top, w->warp_env_bottom, bx, by, bw,
                               bh);
    // Envelope changed implicitly; ensure warp_cache rebuild runs
    w->warp_cache_dirty = true;
  }
  // Phase 3: warp cache
  if (w->warp_cache_dirty || !w->warp_cache) {
    int q = std::clamp(w->warp_quality, 1, 16);
    w->warp_cache = warp_subtree(*w->warp_glyph_cache, w->warp_env_top,
                                 w->warp_env_bottom, bx, by, bw, bh, q);
    w->warp_cache_dirty = false;
  }
}

// ── Canvas::hit_test_warp_envelope ───────────────────────────────────────────
// Screen-space hit test against the envelope anchors + handles of the
// given Warp. Hit threshold matches the overlay's visual sizes: 5 px
// for anchors (8px filled square), 5 px for handle dots (6px ring with
// some tolerance).
//
// Handles are checked FIRST so that in degenerate cases where a handle
// overlaps its anchor (straight envelope with colinear handle at
// anchor distance zero) the drag still works — though in practice the
// overlay skips drawing such handles, so they shouldn't be clickable.
// We still skip hit-testing them since the user can't see them to aim.
//
// Returns true on hit; kind/is_top/idx describe what was picked.
bool Canvas::hit_test_warp_envelope(double x_screen, double y_screen,
                                    const SceneNode &warp, WarpDragKind &kind,
                                    bool &is_top, int &idx) const {
  if (warp.type != SceneNode::Type::Warp)
    return false;
  const double hit_r = 6.0; // screen-space hit radius
  const double hit_r2 = hit_r * hit_r;

  auto scan_envelope = [&](const PathData &env, bool top_flag) -> bool {
    // Pass 1: handles first (preferred over anchors on overlap).
    for (int i = 0; i < (int)env.nodes.size(); ++i) {
      const BezierNode &n = env.nodes[i];
      double asx, asy;
      doc_to_screen(n.x, n.y, asx, asy);
      double h1sx, h1sy, h2sx, h2sy;
      doc_to_screen(n.cx1, n.cy1, h1sx, h1sy);
      doc_to_screen(n.cx2, n.cy2, h2sx, h2sy);
      // Skip handles coincident with their anchor — overlay doesn't
      // draw them, so they can't be aimed at.
      if (std::hypot(h1sx - asx, h1sy - asy) > 0.5) {
        double dx = x_screen - h1sx, dy = y_screen - h1sy;
        if (dx * dx + dy * dy <= hit_r2) {
          kind = WarpDragKind::HandleIn;
          is_top = top_flag;
          idx = i;
          return true;
        }
      }
      if (std::hypot(h2sx - asx, h2sy - asy) > 0.5) {
        double dx = x_screen - h2sx, dy = y_screen - h2sy;
        if (dx * dx + dy * dy <= hit_r2) {
          kind = WarpDragKind::HandleOut;
          is_top = top_flag;
          idx = i;
          return true;
        }
      }
    }
    // Pass 2: anchors.
    for (int i = 0; i < (int)env.nodes.size(); ++i) {
      const BezierNode &n = env.nodes[i];
      double asx, asy;
      doc_to_screen(n.x, n.y, asx, asy);
      double dx = x_screen - asx, dy = y_screen - asy;
      if (dx * dx + dy * dy <= hit_r2) {
        kind = WarpDragKind::Anchor;
        is_top = top_flag;
        idx = i;
        return true;
      }
    }
    return false;
  };

  if (scan_envelope(warp.warp_env_top, true))
    return true;
  if (scan_envelope(warp.warp_env_bottom, false))
    return true;
  return false;
}

// ── Canvas::sync_warp_env_picks_to_selection ────────────────────────────────
// Lazy invalidation: if the recorded pick-set owner no longer matches the
// current primary selection (or that primary isn't a Warp anymore), clear
// the pick set. Called at every read site that consults the pick set.
void Canvas::sync_warp_env_picks_to_selection() {
  const SceneNode *cur =
      (m_selected && m_selected->is_warp()) ? m_selected : nullptr;
  if (m_warp_env_picks_owner != cur) {
    m_warp_env_picks.clear();
    m_warp_env_picks_owner = cur;
  }
}

// ── Canvas::warp_env_picks_clear ────────────────────────────────────────────
void Canvas::warp_env_picks_clear() {
  if (m_warp_env_picks.empty())
    return;
  m_warp_env_picks.clear();
  queue_draw();
}

// ── Canvas::warp_env_picks_select_all_top_anchors ───────────────────────────
// Picks every anchor AND every visible handle on the top envelope.
// Coincident handles are skipped since they're not visually aimable.
void Canvas::warp_env_picks_select_all_top_anchors() {
  if (!m_selected || !m_selected->is_warp())
    return;
  m_warp_env_picks.clear();
  m_warp_env_picks_owner = m_selected;
  const PathData &env = m_selected->warp_env_top;
  for (int i = 0; i < (int)env.nodes.size(); ++i) {
    const BezierNode &n = env.nodes[i];
    m_warp_env_picks.push_back({true, i, EnvelopePart::Anchor});
    if (std::hypot(n.cx1 - n.x, n.cy1 - n.y) > 1e-6)
      m_warp_env_picks.push_back({true, i, EnvelopePart::HandleIn});
    if (std::hypot(n.cx2 - n.x, n.cy2 - n.y) > 1e-6)
      m_warp_env_picks.push_back({true, i, EnvelopePart::HandleOut});
  }
  queue_draw();
}

// ── Canvas::warp_env_picks_select_all_bottom_anchors ────────────────────────
// Picks every anchor AND every visible handle on the bottom envelope.
void Canvas::warp_env_picks_select_all_bottom_anchors() {
  if (!m_selected || !m_selected->is_warp())
    return;
  m_warp_env_picks.clear();
  m_warp_env_picks_owner = m_selected;
  const PathData &env = m_selected->warp_env_bottom;
  for (int i = 0; i < (int)env.nodes.size(); ++i) {
    const BezierNode &n = env.nodes[i];
    m_warp_env_picks.push_back({false, i, EnvelopePart::Anchor});
    if (std::hypot(n.cx1 - n.x, n.cy1 - n.y) > 1e-6)
      m_warp_env_picks.push_back({false, i, EnvelopePart::HandleIn});
    if (std::hypot(n.cx2 - n.x, n.cy2 - n.y) > 1e-6)
      m_warp_env_picks.push_back({false, i, EnvelopePart::HandleOut});
  }
  queue_draw();
}

// ── Canvas::warp_env_picks_select_leftmost_pair ─────────────────────────────
// Picks the leftmost anchor (min x) of top + leftmost of bottom, along
// with each picked anchor's visible handles so the anchor+handles move
// as a unit when dragged.
void Canvas::warp_env_picks_select_leftmost_pair() {
  if (!m_selected || !m_selected->is_warp())
    return;
  m_warp_env_picks.clear();
  m_warp_env_picks_owner = m_selected;
  auto pick_leftmost = [&](const PathData &env, bool is_top) {
    if (env.nodes.empty())
      return;
    int best = 0;
    double best_x = env.nodes[0].x;
    for (int i = 1; i < (int)env.nodes.size(); ++i) {
      if (env.nodes[i].x < best_x) {
        best = i;
        best_x = env.nodes[i].x;
      }
    }
    const BezierNode &n = env.nodes[best];
    m_warp_env_picks.push_back({is_top, best, EnvelopePart::Anchor});
    if (std::hypot(n.cx1 - n.x, n.cy1 - n.y) > 1e-6)
      m_warp_env_picks.push_back({is_top, best, EnvelopePart::HandleIn});
    if (std::hypot(n.cx2 - n.x, n.cy2 - n.y) > 1e-6)
      m_warp_env_picks.push_back({is_top, best, EnvelopePart::HandleOut});
  };
  pick_leftmost(m_selected->warp_env_top, true);
  pick_leftmost(m_selected->warp_env_bottom, false);
  queue_draw();
}

// ── Canvas::warp_env_picks_select_rightmost_pair ────────────────────────────
// Picks the rightmost anchor (max x) of top + rightmost of bottom,
// with each picked anchor's visible handles so they travel together.
void Canvas::warp_env_picks_select_rightmost_pair() {
  if (!m_selected || !m_selected->is_warp())
    return;
  m_warp_env_picks.clear();
  m_warp_env_picks_owner = m_selected;
  auto pick_rightmost = [&](const PathData &env, bool is_top) {
    if (env.nodes.empty())
      return;
    int best = 0;
    double best_x = env.nodes[0].x;
    for (int i = 1; i < (int)env.nodes.size(); ++i) {
      if (env.nodes[i].x > best_x) {
        best = i;
        best_x = env.nodes[i].x;
      }
    }
    const BezierNode &n = env.nodes[best];
    m_warp_env_picks.push_back({is_top, best, EnvelopePart::Anchor});
    if (std::hypot(n.cx1 - n.x, n.cy1 - n.y) > 1e-6)
      m_warp_env_picks.push_back({is_top, best, EnvelopePart::HandleIn});
    if (std::hypot(n.cx2 - n.x, n.cy2 - n.y) > 1e-6)
      m_warp_env_picks.push_back({is_top, best, EnvelopePart::HandleOut});
  };
  pick_rightmost(m_selected->warp_env_top, true);
  pick_rightmost(m_selected->warp_env_bottom, false);
  queue_draw();
}

// ── Canvas::warp_env_picks_select_interior_anchors ──────────────────────────
// All anchors except the leftmost and rightmost (by x) on each envelope.
// For a 2-anchor envelope this yields nothing; for 3-anchor it's the
// middle; for 4-anchor it's the two middles.
void Canvas::warp_env_picks_select_interior_anchors() {
  if (!m_selected || !m_selected->is_warp())
    return;
  // Build into a local list first — only swap into m_warp_env_picks if
  // something was found. Keeps C non-destructive on 2-anchor envelopes
  // where there are no interior anchors.
  std::vector<EnvelopePick> picks;
  auto pick_interior = [&](const PathData &env, bool is_top) {
    int n = (int)env.nodes.size();
    if (n <= 2)
      return;
    // Find leftmost and rightmost indices.
    int lm = 0, rm = 0;
    double lx = env.nodes[0].x, rx = env.nodes[0].x;
    for (int i = 1; i < n; ++i) {
      if (env.nodes[i].x < lx) {
        lx = env.nodes[i].x;
        lm = i;
      }
      if (env.nodes[i].x > rx) {
        rx = env.nodes[i].x;
        rm = i;
      }
    }
    for (int i = 0; i < n; ++i) {
      if (i == lm || i == rm)
        continue;
      picks.push_back({is_top, i, EnvelopePart::Anchor});
    }
  };
  pick_interior(m_selected->warp_env_top, true);
  pick_interior(m_selected->warp_env_bottom, false);
  if (picks.empty())
    return; // No-op on 2-anchor envelopes.
  m_warp_env_picks = std::move(picks);
  m_warp_env_picks_owner = m_selected;
  queue_draw();
}

// ── Canvas::warp_env_picks_select_all_anchors ───────────────────────────────
void Canvas::warp_env_picks_select_all_anchors() {
  if (!m_selected || !m_selected->is_warp())
    return;
  m_warp_env_picks.clear();
  m_warp_env_picks_owner = m_selected;
  for (int i = 0; i < (int)m_selected->warp_env_top.nodes.size(); ++i)
    m_warp_env_picks.push_back({true, i, EnvelopePart::Anchor});
  for (int i = 0; i < (int)m_selected->warp_env_bottom.nodes.size(); ++i)
    m_warp_env_picks.push_back({false, i, EnvelopePart::Anchor});
  queue_draw();
}

// ── Canvas::warp_env_picks_select_all ───────────────────────────────────────
// Every anchor + every handle on both envelopes. Skips coincident handles
// (those that overlap their anchor and aren't visually distinct).
void Canvas::warp_env_picks_select_all() {
  if (!m_selected || !m_selected->is_warp())
    return;
  m_warp_env_picks.clear();
  m_warp_env_picks_owner = m_selected;
  auto add_env = [&](const PathData &env, bool is_top) {
    for (int i = 0; i < (int)env.nodes.size(); ++i) {
      const BezierNode &n = env.nodes[i];
      m_warp_env_picks.push_back({is_top, i, EnvelopePart::Anchor});
      // Handle dots are only drawn when offset from the anchor — include
      // them in the pick only when visually distinct to avoid confusing
      // "I picked something invisible" cases.
      if (std::hypot(n.cx1 - n.x, n.cy1 - n.y) > 1e-6)
        m_warp_env_picks.push_back({is_top, i, EnvelopePart::HandleIn});
      if (std::hypot(n.cx2 - n.x, n.cy2 - n.y) > 1e-6)
        m_warp_env_picks.push_back({is_top, i, EnvelopePart::HandleOut});
    }
  };
  add_env(m_selected->warp_env_top, true);
  add_env(m_selected->warp_env_bottom, false);
  queue_draw();
}

// ── Canvas::make_warp ────────────────────────────────────────────────────────
// Wraps the single selected Path / Compound / Group in a Warp container
// at the same position in the parent's children. Envelope is left empty
// — rebuild_warp_caches seeds a default identity envelope from the
// glyph_cache bbox on first draw. M3a ships with this identity default
// (user sees an unchanged-looking result after Make); M3b's dialog lets
// the user shape the envelope. Undoable via MakeWarpCommand.
//
// Preconditions (mirror Blend for consistency):
//   exactly 1 selected node, not already inside a Warp/Blend/ClipGroup
//   wrapper at the same parent level, type ∈ {Path, Compound, Group}.
// Deeper preconditions get a user-visible error message; the menu-
// sensitivity gate in MainWindow only checks count and type.
void Canvas::make_warp() {
  if (!m_doc)
    return;

  if (m_selection.size() != 1) {
    LOG_WARN("Canvas::make_warp: need exactly 1 selected, got {}",
             m_selection.size());
    m_sig_show_message.emit(
        "Warp", "Warp requires exactly one selected path, compound, or group.");
    return;
  }
  SceneNode *src = m_selection[0];
  if (!src || (src->type != SceneNode::Type::Path &&
               src->type != SceneNode::Type::Compound &&
               src->type != SceneNode::Type::Group)) {
    LOG_WARN("Canvas::make_warp: selection must be Path, Compound, or Group");
    m_sig_show_message.emit(
        "Warp",
        "Warp requires the selection to be a path, compound, or group.");
    return;
  }
  int src_idx = -1;
  SceneNode *parent = find_parent(m_doc, src, &src_idx);
  if (!parent) {
    LOG_WARN("Canvas::make_warp: could not find parent of source");
    return;
  }
  // Snapshot the original for undo BEFORE any mutation.
  auto source_snap = clone_node(*src);

  // Build the Warp node. Source is deep-cloned into the slot so the
  // Warp is self-contained — the original's unique_ptr will be erased
  // from parent->children below.
  auto warp = std::make_unique<SceneNode>();
  warp->type = SceneNode::Type::Warp;
  warp->id = next_id();
  warp->internal_id = last_iid();
  warp->name = m_doc->next_default_name(CurvzDocument::NameKind::Warp);
  warp->visible = true;
  warp->locked = false;
  warp->opacity = 1.0;
  warp->warp_source = clone_node(*src);
  warp->warp_glyph_cache_dirty = true;
  warp->warp_cache_dirty = true;
  // Leave warp_env_top / warp_env_bottom empty — rebuild_warp_caches
  // seeds identity from the glyph_cache bbox on first draw.
  warp->warp_quality = 4;

  // Remove the original and insert the Warp at the same position.
  parent->children.erase(parent->children.begin() + src_idx);
  int ins = std::clamp(src_idx, 0, (int)parent->children.size());
  SceneNode *warp_ptr = warp.get();

  // Snapshot the built Warp for undo BEFORE moving the unique_ptr.
  auto warp_snap = clone_node(*warp);

  parent->children.insert(parent->children.begin() + ins, std::move(warp));

  if (m_history)
    m_history->push(std::make_unique<MakeWarpCommand>(
        parent, std::move(source_snap), src_idx, std::move(warp_snap), ins));

  m_selected = warp_ptr;
  m_selection = {warp_ptr};
  m_selected_node = -1;
  m_node_selection.clear();

  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: made Warp '{}' around source type={}", warp_ptr->name,
           (int)warp_ptr->warp_source->type);
}

// ── Canvas::release_warp ─────────────────────────────────────────────────────
// Undoes a Warp back to its source node. The Warp's envelope is
// discarded — use Flatten Warp if the visible warped geometry should
// be preserved. Result takes the Warp's slot position and is selected.
void Canvas::release_warp() {
  if (!m_doc || !m_selected || !m_selected->is_warp()) {
    LOG_INFO("Canvas::release_warp: ignored — selection is not a Warp");
    return;
  }
  if (!m_selected->warp_source) {
    LOG_WARN("Canvas::release_warp: Warp has null source — cannot release");
    m_sig_show_message.emit(
        "Release Warp",
        "This Warp has no source to release — it appears to be corrupted "
        "or was loaded from a malformed file. Use Delete instead.");
    return;
  }
  int warp_idx = 0;
  SceneNode *parent = find_parent(m_doc, m_selected, &warp_idx);
  if (!parent) {
    LOG_WARN("Canvas::release_warp: could not find parent of Warp");
    return;
  }
  SceneNode *warp = m_selected;

  // Snapshot the Warp (with envelope + caches) for undo.
  auto warp_snap = clone_node(*warp);

  // Build the source-out node: clone warp_source, mint fresh id/iid
  // so it doesn't alias the slot clone held by warp_snap if the Warp
  // is later restored via undo.
  auto source_out = clone_node(*warp->warp_source);
  source_out->id = next_id();
  source_out->internal_id = last_iid();

  // Snapshot the source-out for the command before moving it in.
  auto source_snap = clone_node(*source_out);

  // Mutate the live tree: remove Warp, insert source at same index.
  parent->children.erase(parent->children.begin() + warp_idx);
  int ins = std::clamp(warp_idx, 0, (int)parent->children.size());
  parent->children.insert(parent->children.begin() + ins,
                          std::move(source_out));
  SceneNode *new_sel = parent->children[ins].get();

  if (m_history)
    m_history->push(std::make_unique<ReleaseWarpCommand>(
        parent, std::move(warp_snap), std::move(source_snap), warp_idx));

  m_selected = new_sel;
  m_selection = {new_sel};
  m_selected_node = -1;
  m_node_selection.clear();

  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: released Warp → source");
}

// ── Canvas::flatten_warp ─────────────────────────────────────────────────────
// Replaces the selected Warp with a plain clone of its warp_cache —
// the baked warped geometry. Forces a cache rebuild first so the
// flatten reflects the current envelope state. Envelope is lost in
// the process; undo restores the Warp intact.
//
// For Path sources the flattened result is a Path; for Compound a
// Compound; for Group a Group. The Warp's visible/locked/opacity/
// transform do NOT transfer to the flattened result (the warp_cache
// clones already carry the source's own styling) — any "on the Warp"
// styling the user may have set via the Warp node itself will be
// lost, which matches how Flatten destructive-commits work.
void Canvas::flatten_warp() {
  if (!m_doc || !m_selected || !m_selected->is_warp()) {
    LOG_INFO("Canvas::flatten_warp: ignored — selection is not a Warp");
    return;
  }
  // Force a fresh rebuild so the flatten reflects the current envelope.
  rebuild_warp_caches(m_selected);

  if (!m_selected->warp_cache) {
    LOG_WARN("Canvas::flatten_warp: warp_cache is null — nothing to flatten");
    m_sig_show_message.emit(
        "Flatten Warp",
        "This Warp has no baked geometry to flatten. Try Release Warp "
        "instead.");
    return;
  }
  int warp_idx = 0;
  SceneNode *parent = find_parent(m_doc, m_selected, &warp_idx);
  if (!parent) {
    LOG_WARN("Canvas::flatten_warp: could not find parent of Warp");
    return;
  }
  SceneNode *warp = m_selected;

  // Snapshot the Warp for undo.
  auto warp_snap = clone_node(*warp);

  // Clone warp_cache, mint fresh id/iid so it doesn't alias the Warp's
  // internal cache clone held by warp_snap if the Warp is later
  // restored via undo.
  auto flat = clone_node(*warp->warp_cache);
  flat->id = next_id();
  flat->internal_id = last_iid();

  auto flat_snap = clone_node(*flat);

  parent->children.erase(parent->children.begin() + warp_idx);
  int ins = std::clamp(warp_idx, 0, (int)parent->children.size());
  parent->children.insert(parent->children.begin() + ins, std::move(flat));
  SceneNode *new_sel = parent->children[ins].get();

  if (m_history)
    m_history->push(std::make_unique<FlattenWarpCommand>(
        parent, std::move(warp_snap), std::move(flat_snap), warp_idx));

  m_selected = new_sel;
  m_selection = {new_sel};
  m_selected_node = -1;
  m_node_selection.clear();

  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: flattened Warp");
}

// Brings A and B to the same node count by inserting nodes into whichever
// has fewer, via greedy longest-segment-midpoint subdivision. De Casteljau
// split at t=0.5 is shape-preserving (the visual curve doesn't change) so
// equalization is safe to apply before the user even confirms the Blend.
//
// Greedy selection — each iteration picks the segment with the largest
// current arc length and splits it. This spreads inserted nodes evenly
// across the path regardless of its topology: a path with one very long
// segment and several short ones gets all its new nodes in the long one,
// while a roughly-uniform path gets nodes sprinkled around. Either way
// no two inserted nodes are "stacked" unless the starting geometry is
// degenerate.
//
// Atomic undo: one EditPathCommand per equalized side (typically just
// the shorter one; if counts already match, no command is pushed).
bool Canvas::equalize_blend_sources(SceneNode *a, SceneNode *b) {
  if (!a || !b || !a->path || !b->path || a->type != SceneNode::Type::Path ||
      b->type != SceneNode::Type::Path) {
    LOG_WARN("Canvas::equalize_blend_sources: invalid inputs");
    return false;
  }
  if (a->path->closed != b->path->closed) {
    LOG_WARN("Canvas::equalize_blend_sources: closed flag mismatch — "
             "equalization cannot resolve this");
    return false;
  }
  int na = (int)a->path->nodes.size();
  int nb = (int)b->path->nodes.size();
  if (na == nb)
    return true; // already equal

  SceneNode *target = (na < nb) ? a : b;
  int need = std::abs(na - nb);

  PathData before_pd = *target->path;
  BezierPath bp = BezierPath::from_path_data(*target->path);

  for (int iter = 0; iter < need; ++iter) {
    int segs = bp.segment_count();
    if (segs <= 0) {
      LOG_WARN("Canvas::equalize_blend_sources: zero-segment path — "
               "cannot split further");
      return false;
    }
    // Find segment with longest arc length.
    int best_i = 0;
    double best_len = -1.0;
    for (int i = 0; i < segs; ++i) {
      double len = bp.segment(i).length();
      if (len > best_len) {
        best_len = len;
        best_i = i;
      }
    }
    bp.insert_node_at(best_i, 0.5);
  }

  PathData after_pd = bp.to_path_data();
  *target->path = after_pd;

  if (m_history)
    m_history->push(std::make_unique<EditPathCommand>(
        target, std::move(before_pd), std::move(after_pd),
        "Equalize nodes for Blend"));

  LOG_INFO("Canvas: equalized path '{}' — inserted {} node(s), now {} nodes "
           "(match with other at {})",
           target->id, need, (int)target->path->nodes.size(),
           (na < nb) ? nb : na);
  return true;
}

// ── Canvas::rebuild_blend ────────────────────────────────────────────────────
// User-facing manual refresh. Sets blend_cache_dirty on the given Blend
// and requests a redraw. The existing lazy-rebuild in draw_object picks
// it up on the next frame. Safe to call with a null or non-Blend arg
// (logs warning, no-op) so context-menu handlers don't have to validate.
void Canvas::rebuild_blend(SceneNode *b) {
  if (!b)
    return;
  if (!b->is_blend()) {
    LOG_WARN("Canvas::rebuild_blend: node is not a Blend — ignoring");
    return;
  }
  b->blend_cache_dirty = true;
  queue_draw();
  LOG_INFO("Canvas::rebuild_blend: forced refresh on Blend '{}'", b->name);
}

// ── Canvas::release_blend ────────────────────────────────────────────────────
// Dissolves the currently-selected Blend into three siblings in its
// parent, z-order bottom→top:
//   [blend_idx + 0] = A (clone of blend_source_a)
//   [blend_idx + 1] = Group "Steps" containing clones of blend_cache
//   [blend_idx + 2] = B (clone of blend_source_b)
// The Blend node itself is removed. Selection becomes the three new
// siblings. Atomic undo via ReleaseBlendCommand.
//
// If blend_cache is empty (e.g. the Blend has never been rendered),
// the Steps Group is emitted empty — benign, user can delete it.
// Alternative would be to force a rebuild here, but that would require
// exposing rebuild_blend_cache (currently file-static). For M2 we keep
// the flow simple; M3 may add a guaranteed-fresh guarantee on release.
void Canvas::release_blend() {
  if (!m_doc || !m_selected || !m_selected->is_blend()) {
    LOG_INFO("Canvas::release_blend: ignored — selection is not a Blend");
    return;
  }

  int blend_idx = 0;
  SceneNode *parent = find_parent(m_doc, m_selected, &blend_idx);
  if (!parent) {
    LOG_WARN("Canvas::release_blend: could not find parent of Blend");
    return;
  }

  SceneNode *blend = m_selected;

  // Snapshot the Blend for undo BEFORE any mutation. Clone deep so the
  // snapshot carries A, B, and cache independently of the live node.
  auto blend_snap = clone_node(*blend);

  // Build the three result nodes in ascending z-order.
  std::vector<std::unique_ptr<SceneNode>> results;

  // A (bottom). Deep clone with a fresh id+iid so it doesn't alias the
  // internal slot clone if the Blend is ever restored via undo.
  auto a_out = clone_node(*blend->blend_source_a);
  a_out->id = next_id();
  a_out->internal_id = last_iid();
  results.push_back(std::move(a_out));

  // Steps Group — always emitted, even if cache is empty. Contains
  // clones of each cache entry, in panel [0]=top convention so the
  // topmost step is children[0]. Since cache is stored in render
  // order (bottom→top, matching A→B progression) we reverse on push
  // to land at [0]=top.
  auto steps_grp = std::make_unique<SceneNode>();
  steps_grp->type = SceneNode::Type::Group;
  steps_grp->id = next_id();
  steps_grp->internal_id = last_iid();
  steps_grp->name = m_doc->next_default_name(CurvzDocument::NameKind::Steps);
  steps_grp->visible = true;
  steps_grp->locked = false;
  steps_grp->opacity = 1.0;
  for (int i = (int)blend->blend_cache.size() - 1; i >= 0; --i) {
    auto step = clone_node(*blend->blend_cache[i]);
    step->id = next_id();
    step->internal_id = last_iid();
    step->locked = false; // steps are now independent, editable
    steps_grp->children.push_back(std::move(step));
  }
  results.push_back(std::move(steps_grp));

  // B (top).
  auto b_out = clone_node(*blend->blend_source_b);
  b_out->id = next_id();
  b_out->internal_id = last_iid();
  results.push_back(std::move(b_out));

  // Mutate the live tree: remove Blend, insert results in ascending
  // z-order at blend_idx. Keep pointers to the inserted nodes for
  // the new selection.
  parent->children.erase(parent->children.begin() + blend_idx);
  std::vector<SceneNode *> new_sel;
  for (int i = 0; i < (int)results.size(); ++i) {
    int ins = blend_idx + i;
    ins = std::clamp(ins, 0, (int)parent->children.size());
    auto *ptr = results[i].get();
    parent->children.insert(parent->children.begin() + ins,
                            clone_node(*results[i]));
    // After insert, the live node is at ins; results[i] itself is still
    // the source-of-truth copy we'll hand to the command.
    new_sel.push_back(parent->children[ins].get());
    (void)ptr;
  }

  if (m_history)
    m_history->push(std::make_unique<ReleaseBlendCommand>(
        parent, std::move(blend_snap), std::move(results), blend_idx));

  m_selection = new_sel;
  m_selected = new_sel.empty() ? nullptr : new_sel.front();
  m_selected_node = -1;
  m_node_selection.clear();

  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: released Blend → A + Steps + B (3 siblings)");
}

void Canvas::group_selection() {
  if (!m_doc || m_selection.size() < 2)
    return;

  // Find parent layer of first selected object
  int dummy;
  SceneNode *parent = find_parent(m_doc, m_selection.front(), &dummy);
  if (!parent)
    return;

  // Generate group name via the document funnel (gap-fill, document-wide
  // unique). The SVG `id` is now derived from name + iid by SvgWriter, so
  // we no longer need to set group->id explicitly here.
  std::string gname = m_doc->next_default_name(CurvzDocument::NameKind::Group);

  // Create the group
  auto group = std::make_unique<SceneNode>();
  group->type = SceneNode::Type::Group;
  group->internal_id = generate_internal_id();
  group->name = gname;

  // Find the highest z-position (lowest index) of any selected object in parent
  int insert_idx = (int)parent->children.size();
  for (SceneNode *obj : m_selection) {
    for (int i = 0; i < (int)parent->children.size(); ++i) {
      if (parent->children[i].get() == obj)
        insert_idx = std::min(insert_idx, i);
    }
  }

  // Move selected objects into group in PARENT Z-ORDER, not selection order.
  //
  // Pre-fix: the loop iterated m_selection, which is the order the user
  // clicked. Selecting a back object first then a top object produced a
  // group whose children were [back, top] — visually fine, but if the user
  // had clicked the top first then the back, the group came out [top, back],
  // reversing the layer's stacking. Same input geometry, different group
  // because of click order. Comment said "in original order" but the
  // author meant "in user's click order" which is not what feels original
  // to the user.
  //
  // Fix: walk parent->children once and pluck anything that's in the
  // selection. This preserves the layer's existing z-relationship inside
  // the group regardless of how the user assembled the selection. Same
  // pattern make_compound_path already uses (Canvas.cpp ~3357 — Phase 3
  // builds the compound in parent z-order).
  std::set<SceneNode *> sel_set(m_selection.begin(), m_selection.end());
  for (auto it = parent->children.begin(); it != parent->children.end();) {
    if (sel_set.count(it->get())) {
      group->children.push_back(std::move(*it));
      it = parent->children.erase(it);
    } else {
      ++it;
    }
  }

  // Insert group at the position of the topmost selected object
  insert_idx = std::min(insert_idx, (int)parent->children.size());
  SceneNode *gptr = group.get();
  parent->children.insert(parent->children.begin() + insert_idx,
                          std::move(group));

  // Select the new group
  m_selected = gptr;
  m_selection = {gptr};
  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  {
    MacroStep s;
    s.op = MacroStep::Op::Group;
    record_step_if_recording(s);
  }
  LOG_INFO("Canvas: grouped {} objects → '{}'", m_selection.size(), gname);
}

void Canvas::ungroup_selection() {
  if (!m_doc || !m_selected || m_selected->type != SceneNode::Type::Group)
    return;

  int group_idx;
  SceneNode *parent = find_parent(m_doc, m_selected, &group_idx);
  if (!parent)
    return;

  // Collect children to move out
  std::vector<std::unique_ptr<SceneNode>> children;
  for (auto &child : m_selected->children)
    children.push_back(std::move(child));

  // Remove the group from parent
  parent->children.erase(parent->children.begin() + group_idx);

  // Insert children at the group's position
  std::vector<SceneNode *> new_selection;
  for (int i = (int)children.size() - 1; i >= 0; --i) {
    SceneNode *ptr = children[i].get();
    new_selection.push_back(ptr);
    parent->children.insert(parent->children.begin() + group_idx,
                            std::move(children[i]));
  }

  // Select the ungrouped objects
  m_selection = new_selection;
  m_selected = m_selection.empty() ? nullptr : m_selection.front();
  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  {
    MacroStep s;
    s.op = MacroStep::Op::Ungroup;
    record_step_if_recording(s);
  }
  LOG_INFO("Canvas: ungrouped → {} objects", new_selection.size());
}

// ── align_anchor (validator-on-read)
// ─────────────────────────────────────────────
// Self-cleaning gate for m_align_anchor. The anchor is meaningful only
// while the marked object is still part of the current selection; the
// moment it leaves (Esc, replace, delete, Shift-toggle, marquee replace,
// undo redo, etc.) the stored pointer is stale. Rather than instrument
// every selection-mutation site to clear the anchor (that's the ~40+
// m_selection writes scattered across this file — the procedural fix),
// gate every read through this function (the structural fix). All
// consumers — align_selection, the canvas glyph render, future toolbar
// status — call align_anchor() instead of touching the member, and the
// invariant "anchor is valid or null" is preserved automatically.
SceneNode *Canvas::align_anchor() {
  if (!m_align_anchor)
    return nullptr;
  if (std::find(m_selection.begin(), m_selection.end(), m_align_anchor) ==
      m_selection.end()) {
    m_align_anchor = nullptr;
    return nullptr;
  }
  return m_align_anchor;
}

// ── align_selection
// ───────────────────────────────────────────────────────────
void Canvas::align_selection(AlignOp op) {
  if (m_selection.size() < 2)
    return;

  // ── 1. Separate ref points from regular objects ───────────────────────
  // If ref points are in the selection they act as immovable alignment anchors.
  // The target bbox comes from ref points alone; non-ref objects align to it.
  // If no ref points, fall through to normal union-bbox behavior.
  struct ObjBB {
    SceneNode *obj;
    BBox bb;
  };
  std::vector<ObjBB> items;     // non-ref objects to move
  std::vector<ObjBB> ref_items; // ref points (anchor, never moved)
  items.reserve(m_selection.size());

  for (SceneNode *obj : m_selection) {
    if (obj->is_ref()) {
      // Ref point — treat as a zero-size bbox at its position
      BBox bb{obj->ref_x, obj->ref_y, 0.0, 0.0};
      ref_items.push_back({obj, bb});
      continue;
    }
    auto bb = object_bbox(*obj);
    if (!bb)
      continue;
    items.push_back({obj, *bb});
  }
  if (items.empty())
    return;

  // ── 1.5. Locate align anchor (selection-time key object) ──────────────
  // Validator-on-read: align_anchor() returns the anchor SceneNode* only
  // if it's still in m_selection, otherwise self-clears. Anchor is
  // considered only for Align ops; Distribute ignores it entirely
  // (matches Affinity — distribute geometry has no "anchor" semantics).
  // Anchor wins over ref-points (selection-time user intent overrides
  // persistent ref-point geometry for the duration of one click).
  // Find the anchor's index in items so its dx/dy can be pinned to zero.
  SceneNode *anchor = nullptr;
  size_t anchor_idx = items.size(); // sentinel: "not found"
  bool is_distribute =
      (op == AlignOp::DistributeH || op == AlignOp::DistributeV);
  if (!is_distribute) {
    if (SceneNode *a = align_anchor()) {
      for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].obj == a) {
          anchor = a;
          anchor_idx = i;
          break;
        }
      }
      // Anchor present in selection but is itself a ref-point (zero-size
      // bbox stored in ref_items, not items): fall through to the normal
      // ref-points-as-target path. The visible behaviour is identical
      // (ref point already pins the target) so no special handling needed.
    }
  }

  // ── 2. Compute alignment target bbox ─────────────────────────────────
  // Priority for align ops: anchor > ref points > union bbox.
  // Distribute always uses the union span.
  double ux1, uy1, ux2, uy2;

  if (anchor) {
    // Target = anchor's bbox alone. Anchor stays put; others align to it.
    const BBox &abb = items[anchor_idx].bb;
    ux1 = abb.x;
    uy1 = abb.y;
    ux2 = abb.x + abb.w;
    uy2 = abb.y + abb.h;
  } else if (!ref_items.empty()) {
    // Target from ref points
    ux1 = ref_items[0].bb.x;
    uy1 = ref_items[0].bb.y;
    ux2 = ux1;
    uy2 = uy1;
    for (auto &r : ref_items) {
      ux1 = std::min(ux1, r.bb.x);
      uy1 = std::min(uy1, r.bb.y);
      ux2 = std::max(ux2, r.bb.x);
      uy2 = std::max(uy2, r.bb.y);
    }
    // Include the movable items in union only for distribute ops
    if (op == AlignOp::DistributeH || op == AlignOp::DistributeV) {
      for (auto &it : items) {
        ux1 = std::min(ux1, it.bb.x);
        uy1 = std::min(uy1, it.bb.y);
        ux2 = std::max(ux2, it.bb.x + it.bb.w);
        uy2 = std::max(uy2, it.bb.y + it.bb.h);
      }
    }
  } else {
    // Normal: union of all items
    if (items.size() < 2)
      return;
    ux1 = items[0].bb.x;
    uy1 = items[0].bb.y;
    ux2 = items[0].bb.x + items[0].bb.w;
    uy2 = items[0].bb.y + items[0].bb.h;
    for (auto &it : items) {
      ux1 = std::min(ux1, it.bb.x);
      uy1 = std::min(uy1, it.bb.y);
      ux2 = std::max(ux2, it.bb.x + it.bb.w);
      uy2 = std::max(uy2, it.bb.y + it.bb.h);
    }
  }
  double ucx = (ux1 + ux2) * 0.5;
  double ucy = (uy1 + uy2) * 0.5;

  // ── 3. Collect all leaf paths before mutation (for undo snapshots) ────
  std::vector<AlignObjectsCommand::LeafSnap> snaps;

  // Distribute: needs objects sorted; compute once.
  std::vector<size_t> sorted_idx(items.size());
  std::iota(sorted_idx.begin(), sorted_idx.end(), 0);

  if (op == AlignOp::DistributeH || op == AlignOp::DistributeV) {
    std::sort(sorted_idx.begin(), sorted_idx.end(), [&](size_t a, size_t b) {
      return (op == AlignOp::DistributeH) ? items[a].bb.x < items[b].bb.x
                                          : items[a].bb.y < items[b].bb.y;
    });
  }

  std::vector<double> dx(items.size(), 0.0), dy(items.size(), 0.0);

  if (op == AlignOp::DistributeH || op == AlignOp::DistributeV) {
    int N = (int)sorted_idx.size();
    if (op == AlignOp::DistributeH) {
      size_t fi = sorted_idx.front(), li = sorted_idx.back();
      double span = (items[li].bb.x + items[li].bb.w) - items[fi].bb.x;
      double total_w = 0;
      for (size_t i : sorted_idx)
        total_w += items[i].bb.w;
      double total_gap = span - total_w;
      double gap = (N > 1) ? total_gap / (N - 1) : 0.0;
      double cursor = items[fi].bb.x + items[fi].bb.w + gap;
      for (int k = 1; k < N - 1; ++k) {
        size_t idx = sorted_idx[k];
        dx[idx] = cursor - items[idx].bb.x;
        cursor += items[idx].bb.w + gap;
      }
    } else {
      size_t fi = sorted_idx.front(), li = sorted_idx.back();
      double span = (items[li].bb.y + items[li].bb.h) - items[fi].bb.y;
      double total_h = 0;
      for (size_t i : sorted_idx)
        total_h += items[i].bb.h;
      double total_gap = span - total_h;
      double gap = (N > 1) ? total_gap / (N - 1) : 0.0;
      double cursor = items[fi].bb.y + items[fi].bb.h + gap;
      for (int k = 1; k < N - 1; ++k) {
        size_t idx = sorted_idx[k];
        dy[idx] = cursor - items[idx].bb.y;
        cursor += items[idx].bb.h + gap;
      }
    }
  } else {
    for (size_t i = 0; i < items.size(); ++i) {
      // Anchor is the alignment target — its bbox IS ux1..ux2, uy1..uy2.
      // Computing dx/dy on it yields zero in the perfect-arithmetic case,
      // but we skip explicitly so floating-point dust never nudges the
      // anchor. Matches the ref-points-don't-move guarantee in shape.
      if (i == anchor_idx)
        continue;
      const BBox &bb = items[i].bb;
      switch (op) {
      case AlignOp::AlignLeft:
        dx[i] = ux1 - bb.x;
        break;
      case AlignOp::AlignCenterH:
        dx[i] = ucx - (bb.x + bb.w * 0.5);
        break;
      case AlignOp::AlignRight:
        dx[i] = ux2 - (bb.x + bb.w);
        break;
      case AlignOp::AlignTop:
        dy[i] = uy1 - bb.y;
        break;
      case AlignOp::AlignCenterV:
        dy[i] = ucy - (bb.y + bb.h * 0.5);
        break;
      case AlignOp::AlignBottom:
        dy[i] = uy2 - (bb.y + bb.h);
        break;
      default:
        break;
      }
    }
  }

  // ── 4. Snapshot leaves, apply translation, snapshot after ─────────────
  // Ref points are never moved.
  for (size_t i = 0; i < items.size(); ++i) {
    double ddx = dx[i], ddy = dy[i];
    if (std::abs(ddx) < 1e-9 && std::abs(ddy) < 1e-9)
      continue;

    std::vector<SceneNode *> leaves;
    collect_paths(items[i].obj, leaves);
    for (SceneNode *leaf : leaves) {
      if (!leaf->path)
        continue;
      PathData before = *leaf->path;
      for (auto &n : leaf->path->nodes) {
        n.x += ddx;
        n.y += ddy;
        n.cx1 += ddx;
        n.cy1 += ddy;
        n.cx2 += ddx;
        n.cy2 += ddy;
      }
      snaps.push_back({leaf, std::move(before), *leaf->path});
    }
  }

  if (snaps.empty())
    return;

  // ── 4. Push undo + signal redraw ──────────────────────────────────────
  static const char *op_names[] = {
      "Align left",     "Align center H", "Align right",  "Align top",
      "Align center V", "Align bottom",   "Distribute H", "Distribute V"};
  std::string desc = op_names[static_cast<int>(op)];

  if (m_history)
    m_history->push(
        std::make_unique<AlignObjectsCommand>(std::move(snaps), desc));

  // ── Record macro step ─────────────────────────────────────────────────
  {
    MacroStep s;
    switch (op) {
    case AlignOp::AlignLeft:
      s.op = MacroStep::Op::AlignLeft;
      break;
    case AlignOp::AlignCenterH:
      s.op = MacroStep::Op::AlignCenterH;
      break;
    case AlignOp::AlignRight:
      s.op = MacroStep::Op::AlignRight;
      break;
    case AlignOp::AlignTop:
      s.op = MacroStep::Op::AlignTop;
      break;
    case AlignOp::AlignCenterV:
      s.op = MacroStep::Op::AlignMiddleV;
      break;
    case AlignOp::AlignBottom:
      s.op = MacroStep::Op::AlignBottom;
      break;
    case AlignOp::DistributeH:
      s.op = MacroStep::Op::DistributeH;
      break;
    case AlignOp::DistributeV:
      s.op = MacroStep::Op::DistributeV;
      break;
    }
    record_step_if_recording(s);
  }

  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: {}", desc);
}

// ── boolean_op
// ────────────────────────────────────────────────────────────────
void Canvas::boolean_op(BooleanOpType op) {
  if (!m_doc)
    return;

  const char *op_name = (op == BooleanOpType::Union)      ? "Union"
                        : (op == BooleanOpType::Subtract) ? "Subtract"
                                                          : "Intersect";

  // s122 m3: boolean ops require AT LEAST 2 selected objects. The N-way
  // path goes through Clipper2 in a single call (or, for Intersect, an
  // associative iteration that monotonically shrinks). The S58b "exactly
  // 2" gate is gone — Clipper2's native N-way Union replaces the previous
  // hand-rolled fold that corrupted on disjoint intermediate results.
  // Subtract follows the Affinity / Illustrator convention: bottommost
  // (lowest-Z) operand minus the union of the rest.
  if (m_selection.size() < 2) {
    std::string msg = "Boolean " + std::string(op_name) +
                      " requires at least 2 selected objects.\n\n"
                      "Select 2 or more closed paths or compounds to combine.";
    m_sig_show_message.emit("Boolean Operation Failed", msg);
    LOG_WARN("Canvas: boolean_op requires >=2 selected objects (got {})",
             m_selection.size());
    return;
  }

  // ── Validate: collect closed path objects sharing one parent ─────────────
  // Each candidate must be a Path (or Compound of paths) in the same
  // parent. Open paths are skipped with a warning; if fewer than 2 valid
  // candidates remain after filtering, abort.

  struct Candidate {
    SceneNode *node;
    SceneNode *parent;
    int index;
  };
  std::vector<Candidate> candidates;
  int skipped_open = 0;
  int skipped_nopath = 0;

  SceneNode *common_parent = nullptr;

  for (SceneNode *obj : m_selection) {
    // S58b Ship 2: Compound operands are handled by the Clipper2 engine.
    const bool is_path = obj->path != nullptr;
    const bool is_compound =
        obj->type == SceneNode::Type::Compound && !obj->children.empty();
    if (!is_path && !is_compound) {
      ++skipped_nopath;
      continue;
    }

    int idx = -1;
    SceneNode *par = find_parent(m_doc, obj, &idx);
    if (!par)
      continue;

    if (!common_parent)
      common_parent = par;
    if (par != common_parent) {
      LOG_WARN("Canvas: boolean_op — objects must be in the same layer/group");
      m_sig_show_message.emit(
          "Boolean Operation Failed",
          "All selected objects must be in the same layer or group.");
      return;
    }

    // Closedness check:
    //  - Path: obj->path->closed.
    //  - Compound: require every child path to be closed.
    bool all_closed = true;
    if (is_path) {
      all_closed = obj->path->closed;
    } else {
      for (auto &ch : obj->children) {
        if (!ch->path || !ch->path->closed) {
          all_closed = false;
          break;
        }
      }
    }
    if (!all_closed) {
      ++skipped_open;
      continue;
    }

    candidates.push_back({obj, par, idx});
  }

  if (candidates.size() < 2) {
    std::string msg =
        "Boolean " + std::string(op_name) + " requires at least 2 closed paths.";
    if (skipped_open > 0)
      msg +=
          "\n" + std::to_string(skipped_open) + " open path(s) were skipped.";
    if (skipped_nopath > 0)
      msg += "\n" + std::to_string(skipped_nopath) +
             " non-path object(s) were skipped.";
    m_sig_show_message.emit("Boolean Operation Failed", msg);
    return;
  }

  // ── Sort candidates by layer index ascending (bottom-up = render order) ──
  std::sort(
      candidates.begin(), candidates.end(),
      [](const Candidate &a, const Candidate &b) { return a.index < b.index; });

  SceneNode *parent = common_parent;
  int insert_idx = candidates[0].index; // result goes at lowest position

  // ── Snapshot all originals for undo before any mutation ──────────────────
  std::vector<BooleanOpCommand::Original> originals;
  for (auto &c : candidates)
    originals.push_back({clone_node(*c.node), c.index});

  // ── Build per-operand subpath vectors ────────────────────────────────────
  // s122 m3: walk all candidates (sorted bottom-up), produce one subpath
  // vector per operand. Operand[0] is the lowest-Z; for Subtract that's
  // the kept subject (Affinity convention).
  // A simple Path → 1 subpath. A Compound → N subpaths, one per child.
  auto collect_subpaths = [](SceneNode *n) -> std::vector<BezierPath> {
    std::vector<BezierPath> out;
    if (n->path) {
      out.push_back(BezierPath::from_path_data(*n->path));
    } else if (n->type == SceneNode::Type::Compound) {
      for (auto &ch : n->children) {
        if (ch && ch->path) {
          out.push_back(BezierPath::from_path_data(*ch->path));
        }
      }
    }
    return out;
  };

  // Style source: lowest-Z candidate for Path; lowest-Z candidate's topmost
  // child for Compound (Compound's own fill/stroke are ignored by convention
  // in the rendering code — see SceneNode comment).
  // s122 m5: capture the style as VALUES here, before the originals are
  // erased below. Holding `style_source` as a raw pointer through the
  // erase would dangle — for Compound candidates, style_source pointed
  // at an interior child whose owner unique_ptr is destroyed when the
  // candidate is erased from parent->children. Reading fill/stroke off
  // that pointer in the result-build branches segfaulted on
  // compound+primitive Union (heap reuse made it data-dependent).
  // Same precedent: expand_stroke_op already does this pattern.
  SceneNode *style_source = candidates[0].node;
  if (style_source->type == SceneNode::Type::Compound &&
      !style_source->children.empty() && style_source->children.back()) {
    style_source = style_source->children.back().get();
  }
  const FillStyle   saved_fill    = style_source->fill;
  const StrokeStyle saved_stroke  = style_source->stroke;
  const double      saved_opacity = style_source->opacity;
  // style_source pointer is unused after this point — use the saved_*
  // values in the result-build branches below.

  std::vector<std::vector<BezierPath>> operands;
  operands.reserve(candidates.size());
  for (auto &c : candidates) {
    auto sub = collect_subpaths(c.node);
    if (sub.empty()) {
      LOG_WARN("Canvas: boolean {} — candidate produced 0 subpaths", op_name);
      m_sig_show_message.emit(
          "Boolean Operation Failed",
          "One of the selected objects produced no usable geometry.");
      return;
    }
    operands.push_back(std::move(sub));
  }

  // ── Math call ─────────────────────────────────────────────────────────────
  // s122 m3: N-operand call. For Union: Clipper2 fuses every subpath in a
  // single pass (no iterative fold, no corruption hazard). For Subtract:
  // operands[0] minus union of operands[1..]. For Intersect: associative
  // iteration, monotonically shrinking.
  std::vector<PathData> final_loops;
  std::size_t total_subs = 0;
  for (const auto &o : operands) total_subs += o.size();
  LOG_INFO("Canvas: boolean {} — engine=Clipper2, operands={}, total subpaths={}",
           op_name, operands.size(), total_subs);

  // ── s140 m3 — ENRICH + CLIPPER2 + CLEANUP ───────────────────────────────
  // When cleanup is on, run enrichment on the input operands (s140 m1
  // validated), hand to Clipper2 (s140 m2 validated), then run
  // cleanup_loop on each result.
  //
  // Cleanup walk (s140 m3 — single pass, no pass 2):
  //   - Compute keeper set from ORIGINAL (un-enriched) operand geometry.
  //     This contains originals + intersections only. Synthetic guards
  //     are NOT keepers in the cleanup phase — they served their purpose
  //     at Clipper2 time (carrying curve shape into the boolean output
  //     via on-curve smooth tangent samples) and are now interpolant-
  //     equivalent.
  //   - Per keeper, find its slot in the output polyline and tag it.
  //     Originals restore byte-for-byte from KeeperPoint::source (the
  //     authored handles + type — corner stays corner, smooth stays
  //     smooth). Intersections retract to Corner.
  //   - Delete every untagged node.
  //
  // Toggle off → no enrichment, no cleanup, raw Clipper2 path
  // (pre-s139 behaviour).
  std::vector<std::vector<BezierPath>> enriched_operands;
  Curvz::refit::KeeperSet synthetic_guards;
  const auto* operands_for_clipper = &operands;
  if (m_boolean_cleanup_enabled) {
    Curvz::refit::enrich_operands(operands, enriched_operands, synthetic_guards);
    operands_for_clipper = &enriched_operands;
    LOG_INFO("Canvas: boolean {} — s140 m3: enrichment ON, {} synthetic "
             "guards across {} operands; handing enriched operands to Clipper2.",
             op_name, synthetic_guards.size(), enriched_operands.size());
  }

  final_loops = Curvz::boolean_op_clipper(*operands_for_clipper, op);

  if (m_boolean_cleanup_enabled && !final_loops.empty()) {
    // Keepers from the ORIGINAL operand geometry (pre-enrichment).
    // compute_keeper_set returns originals + intersections only — the
    // SyntheticGuard category is not produced here. Guards are
    // intentionally absent: they're transport, not destination.
    auto keepers = Curvz::refit::compute_keeper_set(operands, op);
    LOG_INFO("Canvas: boolean {} — s140 m3 cleanup: {} keepers across "
             "{} loops",
             op_name, keepers.size(), final_loops.size());
    for (auto &loop : final_loops) {
      loop = Curvz::refit::cleanup_loop(std::move(loop), keepers);
    }
  }

  if (final_loops.empty()) {
    LOG_WARN("Canvas: boolean {} returned empty result", op_name);
    m_sig_show_message.emit(
        "Boolean Operation Failed",
        std::string(op_name) +
            " produced an empty result. No changes were made.");
    return;
  }

  // ── Remove all original nodes from parent (high index first) ─────────────
  // Collect indices in descending order to avoid shifting
  std::vector<int> indices;
  for (auto &c : candidates)
    indices.push_back(c.index);
  std::sort(indices.rbegin(), indices.rend());
  for (int idx : indices)
    parent->children.erase(parent->children.begin() + idx);

  // ── Insert result node at insert_idx ─────────────────────────────────────
  // Single-loop result: emit a Path with the loop's geometry.
  // Multi-loop result:  emit a Compound whose children are Paths, one per
  //                     loop. The Compound's even/odd fill renders holes
  //                     (Subtract B-in-A) and disjoint islands (disjoint
  //                     Union) correctly.
  SceneNode *first_result = nullptr;
  int ins = std::clamp(insert_idx, 0, (int)parent->children.size());

  if (final_loops.size() == 1) {
    auto result_node = std::make_unique<SceneNode>();
    result_node->type = SceneNode::Type::Path;
    result_node->id = next_id();
    result_node->internal_id = last_iid();
    result_node->name = std::string(op_name);
    result_node->fill = saved_fill;          // s122 m5: by-value, not via dangling style_source
    result_node->stroke = saved_stroke;
    result_node->opacity = saved_opacity;
    result_node->visible = true;
    result_node->locked = false;
    result_node->path = std::make_unique<PathData>(std::move(final_loops[0]));

    first_result = result_node.get();
    parent->children.insert(parent->children.begin() + ins,
                            std::move(result_node));
  } else {
    // Multi-loop → Compound. Fill/stroke live on the Compound parent so
    // the even/odd fill paints the whole thing as a single shape.
    auto compound = std::make_unique<SceneNode>();
    compound->type = SceneNode::Type::Compound;
    compound->id = next_id();
    compound->internal_id = last_iid();
    compound->name = std::string(op_name);
    compound->fill = saved_fill;             // s122 m5: by-value
    compound->stroke = saved_stroke;
    compound->opacity = saved_opacity;
    compound->visible = true;
    compound->locked = false;
    for (auto &loop : final_loops) {
      auto child = std::make_unique<SceneNode>();
      child->type = SceneNode::Type::Path;
      child->id = next_id();
      child->internal_id = last_iid();
      // Compound renders fill from the topmost child; stroke per-child.
      // Mirror the style source on each child so fill/stroke render
      // regardless of Z-order decisions later. (See draw_object's Compound
      // branch for the rendering convention.)
      child->fill = saved_fill;              // s122 m5: by-value
      child->stroke = saved_stroke;
      child->opacity = saved_opacity;
      child->visible = true;
      child->locked = false;
      child->path = std::make_unique<PathData>(std::move(loop));
      compound->children.push_back(std::move(child));
    }
    LOG_INFO("Canvas: boolean {} → Compound with {} subpaths", op_name,
             compound->children.size());

    first_result = compound.get();
    parent->children.insert(parent->children.begin() + ins,
                            std::move(compound));
  }

  // ── Build result snapshot for undo ───────────────────────────────────────
  std::vector<std::unique_ptr<SceneNode>> result_snaps;
  result_snaps.push_back(clone_node(*parent->children[ins]));

  // ── Push single atomic undo command ──────────────────────────────────────
  if (m_history)
    m_history->push(std::make_unique<BooleanOpCommand>(
        parent, std::move(originals), std::move(result_snaps), insert_idx,
        std::string(op_name)));

  // ── Notify if any objects were skipped ───────────────────────────────────
  if (skipped_open > 0 || skipped_nopath > 0) {
    std::string msg;
    if (skipped_open > 0)
      msg += std::to_string(skipped_open) + " open path(s) were skipped.\n";
    if (skipped_nopath > 0)
      msg += std::to_string(skipped_nopath) +
             " non-path object(s) were skipped.\n";
    msg += "The operation was performed on the remaining closed paths.";
    m_sig_show_message.emit("Boolean " + std::string(op_name), msg);
  }

  // ── Update selection ─────────────────────────────────────────────────────
  m_selected = first_result;
  m_selection.clear();
  m_selection.push_back(first_result);
  m_selected_node = -1;
  m_node_selection.clear();

  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  {
    MacroStep s;
    switch (op) {
    case BooleanOpType::Union:
      s.op = MacroStep::Op::BooleanUnion;
      break;
    case BooleanOpType::Subtract:
      s.op = MacroStep::Op::BooleanSubtract;
      break;
    case BooleanOpType::Intersect:
      s.op = MacroStep::Op::BooleanIntersect;
      break;
    }
    record_step_if_recording(s);
  }
  LOG_INFO("Canvas: boolean {} complete — {} originals → 1 result", op_name,
           candidates.size());
}

// ── offset_path_op
// ────────────────────────────────────────────────────────────
void Canvas::offset_path_op(double distance, OffsetSide side,
                            bool keep_original) {
  if (!m_doc || m_selection.empty()) {
    LOG_WARN("Canvas: offset_path_op — nothing selected");
    return;
  }

  const char *side_name = (side == OffsetSide::Outside)  ? "Outside"
                          : (side == OffsetSide::Inside) ? "Inside"
                                                         : "Both";

  int applied = 0;
  int skipped = 0;

  for (SceneNode *obj : m_selection) {
    if (!obj->path || !obj->path->closed) {
      ++skipped;
      continue;
    }

    int idx = -1;
    SceneNode *parent = find_parent(m_doc, obj, &idx);
    if (!parent) {
      ++skipped;
      continue;
    }

    std::vector<PathData> results =
        Curvz::offset_path(*obj->path, distance, side);
    if (results.empty()) {
      ++skipped;
      continue;
    }

    // Capture style before any mutation (obj may dangle after erase)
    FillStyle saved_fill = obj->fill;
    StrokeStyle saved_stroke = obj->stroke;
    double saved_opacity = obj->opacity;

    if (keep_original) {
      // ── Keep original: insert result above original, undo via AddNodeCommand
      for (int ri = 0; ri < (int)results.size(); ++ri) {
        auto rnode = std::make_unique<SceneNode>();
        rnode->type = SceneNode::Type::Path;
        rnode->id = next_id();
        rnode->internal_id = last_iid();
        rnode->name = m_doc->uniquify_name(std::string("Offset ") + side_name);
        rnode->fill = saved_fill;
        rnode->stroke = saved_stroke;
        rnode->opacity = saved_opacity;
        rnode->visible = true;
        rnode->locked = false;
        rnode->path = std::make_unique<PathData>(results[ri]);

        // Insert above (before) the original
        int insert_at = std::clamp(idx + ri, 0, (int)parent->children.size());
        parent->children.insert(parent->children.begin() + insert_at,
                                std::move(rnode));

        if (m_history)
          m_history->push(std::make_unique<AddNodeCommand>(
              parent, clone_node(*parent->children[insert_at])));
      }
    } else {
      // ── Consume original: replace with result, undo via BooleanOpCommand
      std::vector<BooleanOpCommand::Original> originals;
      originals.push_back({clone_node(*obj), idx});

      std::vector<std::unique_ptr<SceneNode>> result_snaps;
      int ins = idx;

      parent->children.erase(parent->children.begin() + idx);

      for (int ri = 0; ri < (int)results.size(); ++ri) {
        auto rnode = std::make_unique<SceneNode>();
        rnode->type = SceneNode::Type::Path;
        rnode->id = next_id();
        rnode->name = m_doc->uniquify_name(std::string("Offset ") + side_name);
        rnode->fill = saved_fill;
        rnode->stroke = saved_stroke;
        rnode->opacity = saved_opacity;
        rnode->visible = true;
        rnode->locked = false;
        rnode->path = std::make_unique<PathData>(results[ri]);

        int insert_at = std::clamp(ins + ri, 0, (int)parent->children.size());
        parent->children.insert(parent->children.begin() + insert_at,
                                std::move(rnode));
        result_snaps.push_back(clone_node(*parent->children[insert_at]));
      }

      if (m_history)
        m_history->push(std::make_unique<BooleanOpCommand>(
            parent, std::move(originals), std::move(result_snaps), ins,
            "Offset Path"));
    }

    ++applied;
  }

  if (skipped > 0) {
    std::string msg =
        std::to_string(skipped) + " object(s) skipped (must be closed paths).";
    m_sig_show_message.emit("Offset Path", msg);
  }

  if (applied > 0) {
    m_selected = nullptr;
    m_selection.clear();
    m_selected_node = -1;
    m_node_selection.clear();
    m_sig_selection.emit(nullptr);
    m_sig_doc_changed.emit();
    queue_draw();
    {
      MacroStep s;
      s.op = MacroStep::Op::OffsetPath;
      s.value = distance;
      record_step_if_recording(s);
    }
    LOG_INFO("Canvas: offset_path_op({}, {}, keep={}) — applied to {} path(s)",
             distance, side_name, keep_original, applied);
  }
}

// ── expand_stroke_op
// ────────────────────────────────────────────────────────── Converts each
// selected stroked path into a filled outline shape. Open paths  → single
// closed filled Path (stroke outline joined at ends). ── Text tool
// ─────────────────────────────────────────────────────────────────

void Canvas::set_text_overlay(Gtk::Fixed *fixed) {
  m_text_fixed = fixed;

  // Create the floating entry once; hide it until a text edit begins.
  auto *entry = Gtk::make_managed<Gtk::Entry>();
  m_text_entry = entry;
  m_text_entry->set_visible(false);
  m_text_entry->add_css_class("text-tool-entry");
  // Give it a transparent background so it blends with the canvas.
  m_text_fixed->put(*m_text_entry, 0, 0);
}

void Canvas::on_text_begin(double sx, double sy) {
  if (!m_doc)
    return;

  // Convert screen → doc space.
  double dx, dy;
  screen_to_doc(sx, sy, dx, dy);

  // Hit-test for an existing Text node first.
  SceneNode *hit = nullptr;
  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->locked)
      continue;
    for (int i = (int)layer->children.size() - 1; i >= 0; --i) {
      SceneNode *obj = layer->children[i].get();
      if (!obj->is_text())
        continue;
      // Simple bbox hit: use font_size as approximate height, width heuristic.
      double approx_w = obj->text_content.size() * obj->text_font_size * 0.6;
      double approx_h = obj->text_font_size * 1.4;
      double ox = obj->text_x;
      double oy = obj->text_y - approx_h;
      if (obj->text_anchor == "middle")
        ox -= approx_w * 0.5;
      if (obj->text_anchor == "end")
        ox -= approx_w;
      if (dx >= ox && dx <= ox + approx_w && dy >= oy && dy <= oy + approx_h) {
        hit = obj;
        break;
      }
    }
    if (hit)
      break;
  }

  if (hit) {
    // Edit existing text node — snapshot before-state for undo.
    m_text_editing = hit;
    m_text_is_new = false;
    m_text_snapshot = TextEditCommand::snapshot_before(hit);
    m_text_has_snapshot = true;
    m_selected = hit;
    m_sig_selection.emit(hit);
  } else {
    // Create a new text node at click position.
    auto obj = std::make_unique<SceneNode>();
    obj->type = SceneNode::Type::Text;
    obj->internal_id = generate_internal_id();
    obj->name = m_doc->next_default_name(CurvzDocument::NameKind::Text);
    // Defaults-application routes through the Style funnel per S74 m2.
    // For new objects the clear-bound_style step is a no-op, but going
    // through the funnel keeps the invariant "every writer outside
    // style-propagation takes this path" uniform.
    style::mutate_appearance(*obj, [this](SceneNode& n) {
      n.fill = m_def_fill;
      n.stroke = m_def_stroke;
      n.stroke.paint.type = FillStyle::Type::None;  // default: no stroke on text
    });
    obj->text_x = dx;
    obj->text_y = dy;
    obj->text_font_family = "Sans";
    obj->text_font_size = 24.0;
    obj->text_anchor = "start";
    obj->text_align = "left";

    SceneNode *layer = m_doc->active_layer();
    if (!layer)
      layer = m_doc->layers[0].get();
    layer->children.insert(layer->children.begin(), std::move(obj));
    m_text_editing = layer->children.front().get();
    m_text_is_new = true;
    m_text_has_snapshot = false;
    m_selected = m_text_editing;
    m_sig_selection.emit(m_selected);
  }

  // Show and populate the entry.
  if (m_text_entry) {
    m_text_entry_conn_activate.disconnect();
    m_text_entry_conn_changed.disconnect();

    m_text_entry->set_text(m_text_editing->text_content);
    m_text_entry->set_visible(true);
    m_text_entry->grab_focus();
    m_text_entry->select_region(0, -1);

    position_text_entry();

    m_text_entry_conn_changed =
        m_text_entry->signal_changed().connect([this]() {
          if (m_text_editing) {
            m_text_editing->text_content = m_text_entry->get_text();
            queue_draw();
          }
        });
    m_text_entry_conn_activate = m_text_entry->signal_activate().connect(
        [this]() { commit_text_edit(); });
  }

  queue_draw();
}

void Canvas::position_text_entry() {
  if (!m_text_editing || !m_text_entry || !m_text_fixed)
    return;

  double sx, sy;
  doc_to_screen(m_text_editing->text_x, m_text_editing->text_y, sx, sy);

  // m_text_fixed covers the whole overlay. The canvas (this) sits inside
  // a grid inside the overlay, offset by the ruler widgets.
  // get_allocation() on `this` gives position relative to its parent (the
  // grid). We need position relative to the overlay root — walk the parent
  // chain.
  int cx = 0, cy = 0;
  Gtk::Widget *w = this;
  while (w) {
    Gtk::Widget *parent = w->get_parent();
    if (!parent || parent == m_text_fixed->get_parent())
      break;
    auto alloc = w->get_allocation();
    cx += alloc.get_x();
    cy += alloc.get_y();
    w = parent;
  }

  double ex = cx + sx;
  double ey = cy + sy - m_text_editing->text_font_size * m_zoom;

  // Clamp so entry stays inside the overlay.
  ex = std::max(0.0, ex);
  ey = std::max(0.0, ey);

  // Size entry to current text width or a minimum.
  int entry_w =
      std::max(120, (int)(m_text_editing->text_content.size() *
                          m_text_editing->text_font_size * 0.65 * m_zoom) +
                        24);
  m_text_entry->set_size_request(entry_w, -1);
  m_text_fixed->move(*m_text_entry, (int)ex, (int)ey);
}

void Canvas::commit_text_edit() {
  if (!m_text_editing)
    return;

  std::string content = m_text_entry ? m_text_entry->get_text() : "";

  if (content.empty() && m_text_is_new) {
    // Nothing typed — delete the placeholder node.
    cancel_text_edit();
    return;
  }

  m_text_editing->text_content = content;

  // Push undo command via TextEditCommand (full before/after snapshot).
  if (m_history) {
    if (!m_text_is_new && m_text_has_snapshot) {
      // Existing node edit.
      m_text_snapshot.record_after(m_text_editing);
      m_history->push(
          std::make_unique<TextEditCommand>(std::move(m_text_snapshot)));
      m_text_has_snapshot = false;
    } else if (m_text_is_new) {
      // New node — AddNodeCommand so Ctrl+Z deletes it.
      for (auto &layer : m_doc->layers) {
        for (auto &child : layer->children) {
          if (child.get() == m_text_editing) {
            m_history->push(std::make_unique<AddNodeCommand>(
                layer.get(), clone_node(*m_text_editing)));
            break;
          }
        }
      }
    }
  }

  if (m_text_entry)
    m_text_entry->set_visible(false);
  m_text_entry_conn_activate.disconnect();
  m_text_entry_conn_changed.disconnect();

  // Select the committed node so it's immediately moveable.
  SceneNode *committed = m_text_editing;
  m_selected = committed;
  m_selection = {committed};

  m_sig_doc_changed.emit();
  m_sig_selection.emit(committed);
  m_text_editing = nullptr;
  m_text_is_new = false;
  // Stay in Text tool so user can click to add another text node,
  // but also switch to Selection so the committed node can be moved right away.
  // Emitting request_tool(Selection) mimics what place_shape_node does.
  m_sig_request_tool.emit(ActiveTool::Selection);
  queue_draw();
}

void Canvas::cancel_text_edit() {
  if (!m_text_editing)
    return;

  if (m_text_is_new) {
    // Remove the freshly-created node.
    for (auto &layer : m_doc->layers) {
      auto &ch = layer->children;
      auto it = std::find_if(ch.begin(), ch.end(),
                             [this](const std::unique_ptr<SceneNode> &n) {
                               return n.get() == m_text_editing;
                             });
      if (it != ch.end()) {
        ch.erase(it);
        break;
      }
    }
    m_selected = nullptr;
    m_sig_selection.emit(nullptr);
  }

  if (m_text_entry)
    m_text_entry->set_visible(false);
  m_text_entry_conn_activate.disconnect();
  m_text_entry_conn_changed.disconnect();

  m_text_editing = nullptr;
  m_text_is_new = false;
  m_sig_doc_changed.emit();
  queue_draw();
}

// ── draw_text_on_path
// ───────────────────────────────────────────────────────── Places each glyph
// individually along the guide path using the arc-length table. Each glyph is
// translated to its path position and rotated to match the path tangent at that
// point.
//
// Strategy:
//   1. Build a Pango layout for the full text to extract per-glyph advance
//      widths (using pango_glyph_string_get_logical_widths).
//   2. Walk the path arc-length table, placing each glyph's centre at
//      offset + advance/2 from the previous glyph's end.
//   3. For each glyph: save CTM, translate to path point, rotate by tangent
//      angle (± flip), render via pango_cairo_show_glyph_string, restore.
//
// We're called from inside the doc-space transform (translate+scale applied).
// ─────────────────────────────────────────────────────────────────────────────
void Canvas::draw_text_on_path(const Cairo::RefPtr<Cairo::Context> &cr,
                               const SceneNode &obj, const SceneNode &guide) {
  if (!guide.path)
    return;

  // Build arc-length table in doc units
  BezierPath bp = BezierPath::from_path_data(*guide.path);
  std::vector<double> arc_table;
  double total_len = build_arc_table(bp, arc_table);
  LOG_DEBUG(
      "draw_text_on_path: text='{}' path_id='{}' total_len={:.1f} zoom={:.2f}",
      obj.text_content, obj.text_path_id, total_len, m_zoom);
  if (total_len < 0.001) {
    LOG_DEBUG("draw_text_on_path: ABORT total_len<0.001");
    return;
  }

  // Use only first line of text
  std::string text = obj.text_content;
  auto nl = text.find('\n');
  if (nl != std::string::npos)
    text = text.substr(0, nl);
  if (text.empty()) {
    LOG_DEBUG("draw_text_on_path: ABORT text empty after trim");
    return;
  }

  // Build Pango layout — same as draw_text_node
  PangoLayout *layout = pango_cairo_create_layout(cr->cobj());
  PangoFontDescription *desc = pango_font_description_new();
  pango_font_description_set_family(desc, obj.text_font_family.c_str());
  pango_font_description_set_absolute_size(desc,
                                           obj.text_font_size * PANGO_SCALE);
  if (obj.text_bold)
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  if (obj.text_italic)
    pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
  pango_layout_set_font_description(layout, desc);
  pango_font_description_free(desc);
  pango_layout_set_text(layout, text.c_str(), -1);

  // Baseline: distance from layout top to baseline in pixels
  double baseline_px = pango_layout_get_baseline(layout) / (double)PANGO_SCALE;

  // Total layout width for alignment anchor
  PangoRectangle logical;
  pango_layout_get_pixel_extents(layout, nullptr, &logical);
  double total_text_w = logical.width;

  // Alignment: offset is anchor point on path
  // left=start, center=middle, right=end
  double anchor_arc = obj.text_path_offset;
  if (obj.text_anchor == "middle")
    anchor_arc -= total_text_w * 0.5;
  else if (obj.text_anchor == "end")
    anchor_arc -= total_text_w;

  // Perpendicular offset from path:
  // flip=false → baseline on path, glyphs extend upward (negative Y in rotated
  // frame). flip=true  → reversed traversal + π rotation puts glyphs on the
  // outside of the
  //              bottom arc, readable. Perp is same sign — glyphs still extend
  //              "up" in their own rotated frame, which is outward from the
  //              circle bottom.
  double perp_offset = -obj.text_baseline_shift;

  LOG_DEBUG(
      "draw_text_on_path: text='{}' total_text_w={:.1f} total_path={:.1f} "
      "baseline_px={:.1f} perp={:.1f} anchor_arc={:.1f} flip={}",
      text, total_text_w, total_len, baseline_px, perp_offset, anchor_arc,
      obj.text_path_flip);

  // Apply fill colour once (all glyphs share it)
  apply_fill(cr, obj.fill);

  // ── Iterate runs and place each glyph individually ─────────────────────
  // Use the proven pattern from text_to_paths_op:
  // pango_layout_iter_get_run_extents gives us the run's x position in
  // layout coordinates — critical for correct multi-glyph placement.
  PangoLayoutIter *iter = pango_layout_get_iter(layout);
  do {
    PangoLayoutRun *run = pango_layout_iter_get_run(iter);
    if (!run)
      continue;

    PangoGlyphString *gs = run->glyphs;
    PangoFont *pfont = run->item->analysis.font;

    // Run origin in layout space (pixels)
    PangoRectangle run_ext;
    pango_layout_iter_get_run_extents(iter, nullptr, &run_ext);
    double run_x_px = run_ext.x / (double)PANGO_SCALE;

    // Walk glyphs within this run
    double glyph_x_px = run_x_px; // x position of this glyph within layout

    for (int gi = 0; gi < gs->num_glyphs; ++gi) {
      PangoGlyphInfo &gi_info = gs->glyphs[gi];

      // Skip empty/space glyphs
      if (gi_info.glyph == PANGO_GLYPH_EMPTY ||
          (gi_info.glyph & PANGO_GLYPH_UNKNOWN_FLAG)) {
        glyph_x_px += gi_info.geometry.width / (double)PANGO_SCALE;
        continue;
      }

      double adv_px = gi_info.geometry.width / (double)PANGO_SCALE +
                      obj.text_letter_spacing;

      // Centre of this glyph on the path arc.
      // flip=true: traverse path in reverse so text reads correctly on
      // the bottom of a circle — mirror the arc position to the far end.
      double glyph_centre_arc = anchor_arc + glyph_x_px + adv_px * 0.5;
      double lookup_arc =
          obj.text_path_flip ? total_len - glyph_centre_arc : glyph_centre_arc;

      // Skip glyphs outside path bounds
      if (lookup_arc < 0.0 || lookup_arc > total_len) {
        glyph_x_px += adv_px;
        continue;
      }

      Vec2 pos;
      double angle;
      if (!path_point_at(bp, arc_table, total_len, lookup_arc, pos, angle)) {
        LOG_DEBUG("draw_text_on_path: path_point_at FAILED arc={:.1f}",
                  lookup_arc);
        break;
      }

      // flip=true: add π so glyph faces the opposite tangent direction,
      // making it readable when traversing the path in reverse.
      double effective_angle = obj.text_path_flip ? angle + M_PI : angle;

      if (gi == 0) {
        LOG_DEBUG("draw_text_on_path: glyph 0 adv={:.1f} centre_arc={:.1f} "
                  "lookup={:.1f} pos=({:.1f},{:.1f}) angle={:.3f} perp={:.1f}",
                  adv_px, glyph_centre_arc, lookup_arc, pos.x, pos.y,
                  effective_angle, perp_offset);
      }

      // Place glyph: translate to path point, rotate to (effective) tangent.
      cr->save();
      cr->translate(pos.x, pos.y);
      cr->rotate(effective_angle);

      // Draw single glyph via pango_cairo_show_glyph_string
      PangoGlyphString single;
      int log_cluster = 0;
      single.num_glyphs = 1;
      single.glyphs = &gi_info;
      single.log_clusters = &log_cluster;

      // Horizontal: centre glyph on its advance width.
      // Vertical: baseline_shift pushes away from the path (always negative
      // in the rotated frame — after the π flip, "away" is still -Y).
      double gx = -adv_px * 0.5;
      double gy = perp_offset;

      cr->move_to(gx, gy);
      pango_cairo_show_glyph_string(cr->cobj(), pfont, &single);

      // Optional stroke
      if (obj.stroke.paint.type != FillStyle::Type::None) {
        cr->move_to(gx, gy);
        pango_cairo_glyph_string_path(cr->cobj(), pfont, &single);
        apply_stroke_style(cr, obj.stroke);
        cr->stroke();
        // Restore fill for next glyph
        apply_fill(cr, obj.fill);
      }

      cr->restore();

      glyph_x_px += adv_px;
    }

  } while (pango_layout_iter_next_run(iter));

  pango_layout_iter_free(iter);
  g_object_unref(layout);
}

void Canvas::draw_text_node(const Cairo::RefPtr<Cairo::Context> &cr,
                            const SceneNode &obj) {
  if (obj.text_content.empty()) {
    // Draw a placeholder cursor line so the user sees where text will appear.
    // We're inside the doc-space transform (translate+scale already applied).
    cr->save();
    cr->set_source_rgba(0.4, 0.4, 0.4, 0.7);
    cr->set_line_width(1.5 / m_zoom);
    double h = obj.text_font_size;
    cr->move_to(obj.text_x, obj.text_y);
    cr->line_to(obj.text_x, obj.text_y - h);
    cr->stroke();
    cr->restore();
    return;
  }

  // ── Text-on-path branch ───────────────────────────────────────────────
  if (!obj.text_path_id.empty()) {
    LOG_DEBUG("draw_text_node: text_path_id='{}' looking up guide",
              obj.text_path_id);
    SceneNode *guide = top_find_path_by_id(obj.text_path_id);
    if (guide && guide->path) {
      LOG_DEBUG("draw_text_node: guide found id='{}' nodes={}", guide->id,
                guide->path->nodes.size());
      draw_text_on_path(cr, obj, *guide);
      return;
    }
    LOG_DEBUG("draw_text_node: guide NOT found for id='{}' — falling through",
              obj.text_path_id);
    // Guide path not found — fall through to normal rendering
  }

  // Build a Pango layout via the C API (PangoCairo) since pangomm's
  // create_layout requires a Cairo::Context and we have one).
  cr->save();

  // Y-up: text_y is the baseline in doc space. Cairo's Y is down,
  // but we're already inside the translate(ox,oy)+scale(zoom,zoom)
  // transform where Y increases downward (doc_y = canvas_h - user_y).
  // text_y is already in doc (Y-down) space at this point.
  cr->translate(obj.text_x, obj.text_y);

  // Apply fill.
  apply_fill(cr, obj.fill);

  // Use PangoCairo C API directly — reliable, no pangomm wrapping issues.
  PangoLayout *layout = pango_cairo_create_layout(cr->cobj());

  PangoFontDescription *desc = pango_font_description_new();
  pango_font_description_set_family(desc, obj.text_font_family.c_str());
  // Pango size is in 1/PANGO_SCALE points; we treat font_size as pixels at
  // zoom=1.
  pango_font_description_set_absolute_size(desc,
                                           obj.text_font_size * PANGO_SCALE);
  if (obj.text_bold)
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  if (obj.text_italic)
    pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
  pango_layout_set_font_description(layout, desc);
  pango_font_description_free(desc);

  pango_layout_set_text(layout, obj.text_content.c_str(), -1);

  // Letter spacing — applied as a Pango attribute so layout metrics
  // (width, glyph positions) are correct for anchor offset calculation.
  if (obj.text_letter_spacing != 0.0) {
    PangoAttrList *attrs = pango_attr_list_new();
    // Pango letter_spacing is in Pango units (1/PANGO_SCALE pixels)
    pango_attr_list_insert(attrs,
                           pango_attr_letter_spacing_new(
                               (int)(obj.text_letter_spacing * PANGO_SCALE)));
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);
  }

  // Apply paragraph alignment
  if (obj.text_align == "center")
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
  else if (obj.text_align == "right")
    pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
  else
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
  pango_layout_set_justify(layout, obj.text_align == "justify");

  // Horizontal anchor.
  PangoRectangle ink, logical;
  pango_layout_get_pixel_extents(layout, &ink, &logical);
  double off_x = 0.0;
  if (obj.text_anchor == "middle")
    off_x = -logical.width * 0.5;
  if (obj.text_anchor == "end")
    off_x = -logical.width;

  // Move to baseline: Pango draws from top, we want the baseline at y=0.
  PangoLayoutLine *line = pango_layout_get_line_readonly(layout, 0);
  int baseline = pango_layout_get_baseline(layout);
  double base_px = baseline / (double)PANGO_SCALE;

  cr->move_to(off_x, -base_px - obj.text_baseline_shift);
  pango_cairo_show_layout(cr->cobj(), layout);

  // Optional stroke.
  if (obj.stroke.paint.type != FillStyle::Type::None) {
    cr->move_to(off_x, -base_px - obj.text_baseline_shift);
    pango_cairo_layout_path(cr->cobj(), layout);
    apply_stroke_style(cr, obj.stroke);
    cr->stroke();
  }

  g_object_unref(layout);
  cr->restore();
}

// ── Convert Text to Path outlines (FreeType2) ────────────────────────────────
// Strategy:
//   1. Use PangoCairo to lay out the text and iterate glyph runs,
//      getting each glyph's index and x/y position within the layout.
//   2. Use PangoFcFont to retrieve the underlying FcPattern and from it
//      the font file path + face index, then open it with FreeType.
//   3. Call FT_Load_Glyph → FT_Outline_Decompose with callbacks that
//      build PathData contours.  Each FreeType contour → one closed Path
//      child of a Compound node.
//   4. Quadratic (conic) control points are elevated to cubics so they fit
//      our BezierNode format.
//   5. The assembled Compound replaces the original Text node in-place.
//      Undo is a single AddNodeCommand recording the new Compound.
// ─────────────────────────────────────────────────────────────────────────────

// FreeType outline decomposition callbacks — build PathData contours.
struct FTOutlineCtx {
  // Each call to move_to starts a new contour.
  std::vector<PathData> contours;
  PathData *cur = nullptr;
  double scale = 1.0; // FT units → doc units (1/64 px)

  // Current point (needed for quadratic elevation)
  double cx = 0, cy = 0;

  static int move_to_cb(const FT_Vector *to, void *user) {
    auto *c = static_cast<FTOutlineCtx *>(user);
    c->contours.emplace_back();
    c->cur = &c->contours.back();
    c->cur->closed = true;
    double x = to->x * c->scale;
    double y = to->y * c->scale;
    BezierNode n;
    n.x = x;
    n.y = y;
    n.cx1 = x;
    n.cy1 = y;
    n.cx2 = x;
    n.cy2 = y;
    n.type = BezierNode::Type::Corner;
    c->cur->nodes.push_back(n);
    c->cx = x;
    c->cy = y;
    return 0;
  }
  static int line_to_cb(const FT_Vector *to, void *user) {
    auto *c = static_cast<FTOutlineCtx *>(user);
    if (!c->cur)
      return 0;
    double x = to->x * c->scale;
    double y = to->y * c->scale;
    BezierNode n;
    n.x = x;
    n.y = y;
    n.cx1 = x;
    n.cy1 = y;
    n.cx2 = x;
    n.cy2 = y;
    n.type = BezierNode::Type::Corner;
    c->cur->nodes.push_back(n);
    c->cx = x;
    c->cy = y;
    return 0;
  }
  // Quadratic (conic) control point — elevate to cubic.
  // Two successive conic points imply an implicit on-curve midpoint.
  static int conic_to_cb(const FT_Vector *ctrl, const FT_Vector *to,
                         void *user) {
    auto *c = static_cast<FTOutlineCtx *>(user);
    if (!c->cur || c->cur->nodes.empty())
      return 0;
    double qx0 = c->cx, qy0 = c->cy;
    double qx1 = ctrl->x * c->scale, qy1 = ctrl->y * c->scale;
    double qx2 = to->x * c->scale, qy2 = to->y * c->scale;
    // Cubic control points: p1 = p0 + 2/3*(q1-p0), p2 = p2 + 2/3*(q1-p2)
    double cx1 = qx0 + 2.0 / 3.0 * (qx1 - qx0);
    double cy1 = qy0 + 2.0 / 3.0 * (qy1 - qy0);
    double cx2 = qx2 + 2.0 / 3.0 * (qx1 - qx2);
    double cy2 = qy2 + 2.0 / 3.0 * (qy1 - qy2);
    // Set outgoing handle of previous node
    c->cur->nodes.back().cx2 = cx1;
    c->cur->nodes.back().cy2 = cy1;
    // New on-curve node with incoming handle
    BezierNode n;
    n.x = qx2;
    n.y = qy2;
    n.cx1 = cx2;
    n.cy1 = cy2;
    n.cx2 = qx2;
    n.cy2 = qy2;
    n.type = BezierNode::Type::Corner;
    c->cur->nodes.push_back(n);
    c->cx = qx2;
    c->cy = qy2;
    return 0;
  }
  static int cubic_to_cb(const FT_Vector *c1, const FT_Vector *c2,
                         const FT_Vector *to, void *user) {
    auto *c = static_cast<FTOutlineCtx *>(user);
    if (!c->cur || c->cur->nodes.empty())
      return 0;
    double cx1 = c1->x * c->scale, cy1 = c1->y * c->scale;
    double cx2 = c2->x * c->scale, cy2 = c2->y * c->scale;
    double tx = to->x * c->scale, ty = to->y * c->scale;
    // Set outgoing handle of previous node
    c->cur->nodes.back().cx2 = cx1;
    c->cur->nodes.back().cy2 = cy1;
    // New on-curve node with incoming handle
    BezierNode n;
    n.x = tx;
    n.y = ty;
    n.cx1 = cx2;
    n.cy1 = cy2;
    n.cx2 = tx;
    n.cy2 = ty;
    n.type = BezierNode::Type::Corner;
    c->cur->nodes.push_back(n);
    c->cx = tx;
    c->cy = ty;
    return 0;
  }
};

static FT_Outline_Funcs s_ft_callbacks = {
    FTOutlineCtx::move_to_cb,
    FTOutlineCtx::line_to_cb,
    FTOutlineCtx::conic_to_cb,
    FTOutlineCtx::cubic_to_cb,
    0, // shift
    0  // delta
};

void Canvas::text_to_paths_op() {
  if (!m_doc || m_selection.empty()) {
    LOG_WARN("Canvas: text_to_paths_op — nothing selected");
    return;
  }

  std::vector<SceneNode *> text_nodes;
  for (SceneNode *obj : m_selection)
    if (obj->is_text())
      text_nodes.push_back(obj);

  if (text_nodes.empty()) {
    m_sig_show_message.emit("Convert Text to Path",
                            "Select one or more text objects first.");
    return;
  }

  // Initialise FreeType library once per call.
  FT_Library ft_lib = nullptr;
  if (FT_Init_FreeType(&ft_lib) != 0) {
    m_sig_show_message.emit("Convert Text to Path",
                            "FreeType initialisation failed.");
    return;
  }

  for (SceneNode *obj : text_nodes) {
    if (obj->text_content.empty())
      continue;

    // ── Detect text-on-path ───────────────────────────────────────────
    bool is_top = !obj->text_path_id.empty();
    SceneNode *guide_node =
        is_top ? top_find_path_by_id(obj->text_path_id) : nullptr;
    if (is_top && (!guide_node || !guide_node->path)) {
      LOG_WARN("text_to_paths_op: PTT guide not found for '{}', falling back "
               "to normal",
               obj->text_content);
      is_top = false;
    }

    // Build arc table once if PTT
    BezierPath top_bp;
    std::vector<double> top_arc_table;
    double top_total = 0.0;
    if (is_top) {
      top_bp = BezierPath::from_path_data(*guide_node->path);
      top_total = build_arc_table(top_bp, top_arc_table);
    }

    // ── 1. Build Pango layout to resolve font + glyph positions ──────
    // Use a 1×1 scratch Cairo surface — we only need layout metrics, not
    // rendering.
    auto surf =
        Cairo::ImageSurface::create(Cairo::ImageSurface::Format::ARGB32, 1, 1);
    auto cr = Cairo::Context::create(surf);

    PangoLayout *layout = pango_cairo_create_layout(cr->cobj());
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, obj->text_font_family.c_str());
    // Use absolute size in Pango units (= points * PANGO_SCALE).
    // We want doc-unit pixels → treat as points at 72dpi (1pt = 1px at 72dpi).
    pango_font_description_set_absolute_size(desc,
                                             obj->text_font_size * PANGO_SCALE);
    if (obj->text_bold)
      pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    if (obj->text_italic)
      pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, obj->text_content.c_str(), -1);

    // Apply letter spacing so glyph positions match rendering exactly.
    if (obj->text_letter_spacing != 0.0) {
      PangoAttrList *attrs = pango_attr_list_new();
      pango_attr_list_insert(
          attrs, pango_attr_letter_spacing_new(
                     (int)(obj->text_letter_spacing * PANGO_SCALE)));
      pango_layout_set_attributes(layout, attrs);
      pango_attr_list_unref(attrs);
    }

    // Baseline offset for Y positioning (same as draw_text_node).
    int baseline_pango = pango_layout_get_baseline(layout);
    double baseline_px = baseline_pango / (double)PANGO_SCALE;

    // Anchor horizontal offset.
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, nullptr, &logical);
    double anchor_off_x = 0.0;
    if (obj->text_anchor == "middle")
      anchor_off_x = -logical.width * 0.5;
    if (obj->text_anchor == "end")
      anchor_off_x = -logical.width;

    // PTT: compute anchor arc position and perp offset (mirrors
    // draw_text_on_path)
    double top_anchor_arc = obj->text_path_offset;
    double top_perp_offset = -obj->text_baseline_shift;
    if (is_top) {
      if (obj->text_anchor == "middle")
        top_anchor_arc -= logical.width * 0.5;
      else if (obj->text_anchor == "end")
        top_anchor_arc -= logical.width;
    }

    // ── 2. Iterate glyph runs ─────────────────────────────────────────
    // We'll collect all contours across all glyphs into one Compound.
    std::vector<PathData> all_contours;

    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    do {
      PangoLayoutRun *run = pango_layout_iter_get_run(iter);
      if (!run)
        continue;

      PangoFont *pfont = run->item->analysis.font;
      PangoGlyphString *gs = run->glyphs;

      // Get run origin in Pango units (layout coordinates).
      PangoRectangle run_ext;
      pango_layout_iter_get_run_extents(iter, nullptr, &run_ext);
      double run_x_px = run_ext.x / (double)PANGO_SCALE;

      // ── 3. Resolve font file via PangoFc ─────────────────────────
      // PangoFont → PangoFcFont → FcPattern → FC_FILE
      const char *font_file = nullptr;
      int face_idx = 0;

#if defined(PANGO_VERSION_CHECK) && PANGO_VERSION_CHECK(1, 18, 0)
      // pango_fc_font_get_pattern is available when PangoCairo uses fontconfig.
      PangoFcFont *fc_font = PANGO_FC_FONT(pfont);
      if (fc_font) {
        FcPattern *pat = pango_fc_font_get_pattern(fc_font);
        FcPatternGetString(pat, FC_FILE, 0, (FcChar8 **)&font_file);
        FcPatternGetInteger(pat, FC_INDEX, 0, &face_idx);
      }
#endif
      if (!font_file) {
        LOG_WARN(
            "text_to_paths_op: could not resolve font file for run, skipping");
        continue;
      }

      // ── 4. Open FreeType face ─────────────────────────────────────
      FT_Face ft_face = nullptr;
      if (FT_New_Face(ft_lib, font_file, face_idx, &ft_face) != 0) {
        LOG_WARN("text_to_paths_op: FT_New_Face failed for {}", font_file);
        continue;
      }
      // Set size to match Pango's absolute size.
      // FT size is in 26.6 fixed-point (1/64 pt), at 72dpi so 1pt = 1px.
      FT_Set_Char_Size(ft_face, 0, (FT_F26Dot6)(obj->text_font_size * 64.0), 72,
                       72);

      // Scale from FT 26.6 units to doc pixels: 1 FT unit = 1/64 px.
      double ft_scale = 1.0 / 64.0;

      // ── 5. Per-glyph outline extraction ───────────────────────────
      double glyph_x_px = run_x_px;
      for (int gi = 0; gi < gs->num_glyphs; ++gi) {
        PangoGlyphInfo &gi_info = gs->glyphs[gi];
        PangoGlyph glyph_id = gi_info.glyph;

        // Skip invalid/space glyphs.
        if (glyph_id == PANGO_GLYPH_EMPTY ||
            (glyph_id & PANGO_GLYPH_UNKNOWN_FLAG)) {
          glyph_x_px += gi_info.geometry.width / (double)PANGO_SCALE;
          continue;
        }

        double adv_px = gi_info.geometry.width / (double)PANGO_SCALE +
                        obj->text_letter_spacing;

        // Glyph x offset within the run (geometry.x_offset in Pango units).
        double gx =
            glyph_x_px + gi_info.geometry.x_offset / (double)PANGO_SCALE;
        double gy = -gi_info.geometry.y_offset / (double)PANGO_SCALE;

        if (FT_Load_Glyph(ft_face, glyph_id, FT_LOAD_NO_BITMAP) == 0 &&
            ft_face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {

          FTOutlineCtx ctx;
          ctx.scale = ft_scale;

          FT_Outline_Decompose(&ft_face->glyph->outline, &s_ft_callbacks, &ctx);

          if (is_top) {
            // ── PTT placement: rotate + translate to path point ──
            // Mirror draw_text_on_path exactly.
            double glyph_centre_arc =
                top_anchor_arc + glyph_x_px + adv_px * 0.5;
            double lookup_arc = obj->text_path_flip
                                    ? top_total - glyph_centre_arc
                                    : glyph_centre_arc;
            lookup_arc = std::max(0.0, std::min(lookup_arc, top_total));

            Vec2 pos;
            double angle;
            if (!path_point_at(top_bp, top_arc_table, top_total, lookup_arc,
                               pos, angle)) {
              glyph_x_px += adv_px;
              continue;
            }
            double eff_angle = obj->text_path_flip ? angle + M_PI : angle;

            // Local glyph origin in rotated frame:
            // glyph draw point = (-adv/2, perp_offset) in rotated coords.
            // FT point (ft_x, ft_y) in Y-up glyph space maps to:
            //   local_x = ft_x - adv/2
            //   local_y = -ft_y + perp_offset  (Y-flip + perp shift)
            // Then rotate by eff_angle and translate to pos.
            double cos_a = std::cos(eff_angle);
            double sin_a = std::sin(eff_angle);
            double half_adv = adv_px * 0.5;

            auto xform = [&](double &nx, double &ny) {
              double lx = nx - half_adv;
              double ly = -ny + top_perp_offset;
              nx = pos.x + lx * cos_a - ly * sin_a;
              ny = pos.y + lx * sin_a + ly * cos_a;
            };

            for (auto &pd : ctx.contours) {
              for (auto &n : pd.nodes) {
                xform(n.x, n.y);
                xform(n.cx1, n.cy1);
                xform(n.cx2, n.cy2);
              }
              if (!pd.nodes.empty())
                all_contours.push_back(std::move(pd));
            }
          } else {
            // ── Normal text placement: translate + Y-flip ────────
            double tx = obj->text_x + anchor_off_x + gx;
            double ty = obj->text_y - baseline_px + gy;

            for (auto &pd : ctx.contours) {
              for (auto &n : pd.nodes) {
                n.x = n.x + tx;
                n.y = ty - n.y;
                n.cx1 = n.cx1 + tx;
                n.cy1 = ty - n.cy1;
                n.cx2 = n.cx2 + tx;
                n.cy2 = ty - n.cy2;
              }
              if (!pd.nodes.empty())
                all_contours.push_back(std::move(pd));
            }
          }
        }

        glyph_x_px += adv_px;
      }

      FT_Done_Face(ft_face);

    } while (pango_layout_iter_next_run(iter));
    pango_layout_iter_free(iter);
    g_object_unref(layout);

    if (all_contours.empty())
      continue;

    // DIAG S55: surface contour details so we can diagnose text-to-path
    // rendering failures empirically. Remove once issue is resolved.
    LOG_INFO("text_to_paths: collected {} contour(s) for '{}'",
             all_contours.size(), obj->text_content);
    for (size_t ci = 0; ci < all_contours.size(); ++ci) {
      const auto &pd = all_contours[ci];
      if (pd.nodes.empty()) {
        LOG_INFO("  contour[{}]: EMPTY (0 nodes), closed={}", ci, pd.closed);
      } else {
        LOG_INFO("  contour[{}]: {} nodes, closed={}, first=({:.3f},{:.3f}), "
                 "last=({:.3f},{:.3f})",
                 ci, pd.nodes.size(), pd.closed, pd.nodes.front().x,
                 pd.nodes.front().y, pd.nodes.back().x, pd.nodes.back().y);
      }
    }

    // ── 6. Build a Compound node — one Path child per contour ────────
    auto compound = std::make_unique<SceneNode>();
    compound->type = SceneNode::Type::Compound;
    compound->name = m_doc->uniquify_name(obj->name + " (outline)");
    compound->fill = obj->fill;
    compound->stroke = obj->stroke;
    compound->opacity = obj->opacity;

    for (auto &pd : all_contours) {
      auto path_child = std::make_unique<SceneNode>();
      path_child->type = SceneNode::Type::Path;
      path_child->fill = obj->fill;
      // Stroke must sit on each child — Compound's draw reads stroke
      // per-child, never from the Compound itself (see draw_object's
      // Compound branch). Setting this to None would silently discard
      // any stroke the user had on the original text, requiring them to
      // release the compound first before a stroke could render.
      path_child->stroke = obj->stroke;
      path_child->path = std::make_unique<PathData>(std::move(pd));
      compound->children.push_back(std::move(path_child));
    }

    // ── 7. Replace Text node with Compound in-place ───────────────────
    SceneNode *owner_layer = nullptr;
    int insert_idx = 0;
    for (auto &layer : m_doc->layers) {
      for (int i = 0; i < (int)layer->children.size(); ++i) {
        if (layer->children[i].get() == obj) {
          owner_layer = layer.get();
          insert_idx = i;
          break;
        }
      }
      if (owner_layer)
        break;
    }
    if (!owner_layer)
      continue;

    SceneNode *raw_compound = compound.get();

    // Clone the original text node before replacing it — needed for undo.
    auto before_snap = clone_node(*obj);

    owner_layer->children.insert(owner_layer->children.begin() + insert_idx,
                                 std::move(compound));
    owner_layer->children.erase(owner_layer->children.begin() + insert_idx + 1);

    // ── 8. Remove guide path (PTT only) ───────────────────────────────
    // The guide path is now an invisible orphan — remove it.
    // Find it before pushing undo so we can store its index.
    SceneNode *guide_parent = nullptr;
    int guide_idx = -1;
    std::unique_ptr<SceneNode> guide_snap;
    if (is_top && guide_node) {
      for (auto &layer : m_doc->layers) {
        for (int i = 0; i < (int)layer->children.size(); ++i) {
          if (layer->children[i].get() == guide_node) {
            guide_parent = layer.get();
            guide_idx = i;
            break;
          }
        }
        if (guide_parent)
          break;
      }
      if (guide_parent) {
        guide_snap = clone_node(*guide_node);
        guide_parent->children.erase(guide_parent->children.begin() +
                                     guide_idx);
      }
    }

    if (m_history) {
      auto composite =
          std::make_unique<CompositeCommand>("Convert text to path");
      composite->add(std::make_unique<ReplaceNodeCommand>(
          owner_layer, insert_idx, std::move(before_snap),
          clone_node(*raw_compound)));
      if (guide_parent && guide_snap) {
        composite->add(std::make_unique<DeleteObjectCommand>(
            guide_parent, std::move(guide_snap), guide_idx));
      }
      m_history->push(std::move(composite));
    }

    LOG_INFO("text_to_paths_op: '{}' → {} contour(s){}", obj->text_content,
             all_contours.size(), is_top ? " (PTT, guide removed)" : "");
  }

  FT_Done_FreeType(ft_lib);

  m_selection.clear();
  m_selected = nullptr;
  m_sig_selection.emit(nullptr);
  m_sig_doc_changed.emit();
  queue_draw();
}

// Closed paths → Compound with outer + inner (even-odd, stroke colour as fill).
// Operation is undoable via BooleanOpCommand.
void Canvas::expand_stroke_op() {
  if (!m_doc || m_selection.empty()) {
    LOG_WARN("Canvas: expand_stroke_op — nothing selected");
    return;
  }

  // S96 m3: name uniqueness comes from the document funnel; SVG `id`
  // is derived from name + internal_id at write time. The previous
  // static counter is no longer needed.
  int applied = 0;
  int skipped = 0;

  for (SceneNode *obj : m_selection) {
    if (!obj->path) {
      ++skipped;
      continue;
    }
    if (obj->stroke.paint.type == FillStyle::Type::None ||
        obj->stroke.width <= 0.0) {
      ++skipped;
      continue;
    }

    int idx = -1;
    SceneNode *parent = find_parent(m_doc, obj, &idx);
    if (!parent) {
      ++skipped;
      continue;
    }

    double half = obj->stroke.width * 0.5;
    bool is_open = !obj->path->closed;

    std::vector<PathData> outline_pd;
    std::vector<PathData> outer_pd, inner_pd;

    if (is_open) {
      outline_pd = Curvz::offset_path(*obj->path, half, OffsetSide::Both);
      if (outline_pd.empty()) {
        ++skipped;
        continue;
      }
    } else {
      outer_pd = Curvz::offset_path(*obj->path, half, OffsetSide::Outside);
      inner_pd = Curvz::offset_path(*obj->path, half, OffsetSide::Inside);
      if (outer_pd.empty() || inner_pd.empty()) {
        ++skipped;
        continue;
      }
    }

    FillStyle result_fill = obj->stroke.paint;
    double saved_opacity = obj->opacity;

    std::vector<BooleanOpCommand::Original> originals;
    originals.push_back({clone_node(*obj), idx});
    parent->children.erase(parent->children.begin() + idx);

    int insert_at = std::clamp(idx, 0, (int)parent->children.size());

    if (is_open) {
      // Open path -> single closed filled Path
      auto result_node = std::make_unique<SceneNode>();
      result_node->type = SceneNode::Type::Path;
      result_node->id = next_id();
      result_node->internal_id = last_iid();
      result_node->name =
          m_doc->next_default_name(CurvzDocument::NameKind::ExpandedStroke);
      result_node->fill = result_fill;
      result_node->stroke.paint.type = FillStyle::Type::None;
      result_node->opacity = saved_opacity;
      result_node->visible = true;
      result_node->locked = false;
      result_node->path = std::make_unique<PathData>(outline_pd[0]);
      parent->children.insert(parent->children.begin() + insert_at,
                              std::move(result_node));
    } else {
      // Closed path -> compound (outer + inner, even-odd)
      auto outer_node = std::make_unique<SceneNode>();
      outer_node->type = SceneNode::Type::Path;
      outer_node->id = next_id();
      outer_node->internal_id = last_iid();
      outer_node->name = "Outer";
      outer_node->fill = result_fill;
      outer_node->stroke.paint.type = FillStyle::Type::None;
      outer_node->opacity = 1.0;
      outer_node->visible = true;
      outer_node->locked = false;
      outer_node->path = std::make_unique<PathData>(outer_pd[0]);

      auto inner_node = std::make_unique<SceneNode>();
      inner_node->type = SceneNode::Type::Path;
      inner_node->id = next_id();
      inner_node->internal_id = last_iid();
      inner_node->name = "Inner";
      inner_node->fill = result_fill;
      inner_node->stroke.paint.type = FillStyle::Type::None;
      inner_node->opacity = 1.0;
      inner_node->visible = true;
      inner_node->locked = false;
      inner_node->path = std::make_unique<PathData>(inner_pd[0]);

      auto compound = std::make_unique<SceneNode>();
      compound->type = SceneNode::Type::Compound;
      compound->internal_id = generate_internal_id();
      compound->name = m_doc->next_default_name(CurvzDocument::NameKind::ExpandedStroke);
      // S58g: Compound owns its paint per the S58d rule — set the expanded
      // stroke's target colour on the Compound itself so the canvas renderer
      // (which reads Compound.fill) picks it up. The per-child fills we
      // also set above are now redundant for rendering but kept intact so
      // that if the Compound is ever split back into plain paths, the
      // children carry the right colour.
      compound->fill = result_fill;
      compound->stroke.paint.type = FillStyle::Type::None;
      compound->opacity = saved_opacity;
      compound->visible = true;
      compound->locked = false;
      compound->children.push_back(std::move(outer_node));
      compound->children.push_back(std::move(inner_node));
      parent->children.insert(parent->children.begin() + insert_at,
                              std::move(compound));
    }

    std::vector<std::unique_ptr<SceneNode>> result_snaps;
    result_snaps.push_back(clone_node(*parent->children[insert_at]));

    if (m_history)
      m_history->push(std::make_unique<BooleanOpCommand>(
          parent, std::move(originals), std::move(result_snaps), idx,
          "Expand Stroke"));

    ++applied;
  }

  if (skipped > 0) {
    std::string msg =
        std::to_string(skipped) +
        " object(s) skipped (must have a stroke with non-zero width).";
    m_sig_show_message.emit("Expand Stroke", msg);
  }

  if (applied > 0) {
    m_selected = nullptr;
    m_selection.clear();
    m_selected_node = -1;
    m_node_selection.clear();
    m_sig_selection.emit(nullptr);
    m_sig_doc_changed.emit();
    queue_draw();
    LOG_INFO("Canvas: expand_stroke_op — applied to {} path(s)", applied);
  }
}

void Canvas::arrange(ArrangeOp op) {
  if (!m_doc || !m_selected)
    return;

  // ── 1. Collect selected objects and verify they share one parent ───────
  // All objects in m_selection must be in the same parent for a coherent
  // block move. Find the parent of m_selected; skip any selection member
  // that lives elsewhere (e.g. objects in different layers).
  int primary_idx = -1;
  SceneNode *parent = find_parent(m_doc, m_selected, &primary_idx);
  if (!parent || primary_idx < 0)
    return;

  // Build sorted list of (index, object*) for every selected object that
  // shares this parent.
  std::vector<std::pair<int, SceneNode *>> sel_items; // (index, ptr) ascending
  for (SceneNode *obj : m_selection) {
    int idx = -1;
    SceneNode *p = find_parent(m_doc, obj, &idx);
    if (p == parent && idx >= 0)
      sel_items.push_back({idx, obj});
  }
  if (sel_items.empty())
    return;
  std::sort(sel_items.begin(), sel_items.end());

  int n = (int)parent->children.size();
  if (n < 2)
    return;

  const char *desc = "";

  // ── 2. Compute new positions ──────────────────────────────────────────
  // Strategy: extract the selected block, find the insertion point, reinsert.
  // We work on a temporary index array to figure out before/after pairs
  // without actually mutating yet.

  // Build a vector of all child indices, marking which are selected.
  std::vector<int> order(
      n); // order[i] = original index of child now at position i
  std::iota(order.begin(), order.end(), 0);

  std::unordered_set<int> sel_set;
  for (auto &[idx, _] : sel_items)
    sel_set.insert(idx);

  // Separate selected and non-selected in their current relative order
  std::vector<int> non_sel, sel_indices;
  for (int i = 0; i < n; ++i) {
    if (sel_set.count(i))
      sel_indices.push_back(i);
    else
      non_sel.push_back(i);
  }

  // Determine insertion position in the non-selected list
  // then interleave: selected block inserts before/after a non-sel neighbour.
  int lowest = sel_indices.front();
  int highest = sel_indices.back();

  // Count non-selected objects strictly below and above the selected block
  int non_sel_below = 0, non_sel_above = 0;
  for (int i : non_sel) {
    if (i < lowest)
      ++non_sel_below;
    if (i > highest)
      ++non_sel_above;
  }

  switch (op) {
  case ArrangeOp::BringToFront:
    desc = "Bring to front";
    break;
  case ArrangeOp::BringForward:
    desc = "Bring forward";
    break;
  case ArrangeOp::SendBackward:
    desc = "Send backward";
    break;
  case ArrangeOp::SendToBack:
    desc = "Send to back";
    break;
  }

  // Build the new child order as a permutation of [0..n-1]
  // new_order[i] = which original child ends up at position i
  std::vector<int> new_order;
  new_order.reserve(n);

  if (op == ArrangeOp::BringToFront) {
    // index 0 = visually on top → selected goes at FRONT of new_order
    if (non_sel_below == 0)
      return; // already at top (index 0)
    for (int i : sel_indices)
      new_order.push_back(i);
    for (int i : non_sel)
      new_order.push_back(i);
  } else if (op == ArrangeOp::SendToBack) {
    // selected goes at BACK of new_order (highest index = bottom)
    if (non_sel_above == 0)
      return; // already at bottom
    for (int i : non_sel)
      new_order.push_back(i);
    for (int i : sel_indices)
      new_order.push_back(i);
  } else if (op == ArrangeOp::BringForward) {
    // Move toward lower index (toward index 0 = top)
    if (non_sel_below == 0)
      return; // already at top
    // Find the last non-sel index below lowest (the one to jump over)
    int pivot = -1;
    for (int i = (int)non_sel.size() - 1; i >= 0; --i) {
      if (non_sel[i] < lowest) {
        pivot = non_sel[i];
        break;
      }
    }
    for (int i : non_sel) {
      if (i < pivot)
        new_order.push_back(i);
    }
    for (int i : sel_indices)
      new_order.push_back(i);
    new_order.push_back(pivot);
    for (int i : non_sel) {
      if (i > highest)
        new_order.push_back(i);
    }
  } else { // SendBackward — move toward higher index (toward bottom)
    if (non_sel_above == 0)
      return; // already at bottom
    int pivot = -1;
    for (int i : non_sel) {
      if (i > highest) {
        pivot = i;
        break;
      }
    }
    for (int i : non_sel) {
      if (i < lowest)
        new_order.push_back(i);
    }
    new_order.push_back(pivot);
    for (int i : sel_indices)
      new_order.push_back(i);
    for (int i : non_sel) {
      if (i > pivot)
        new_order.push_back(i);
    }
  }

  if (new_order.size() != (size_t)n)
    return; // sanity

  // ── 3. Apply permutation to children vector ───────────────────────────
  // Snapshot before order
  std::vector<std::string> before_order, after_order;
  before_order.reserve(n);
  for (auto &c : parent->children)
    before_order.push_back(c->id);

  // Build inverse map: original_index → new_position (for undo records)
  std::vector<int> orig_to_new(n);
  for (int pos = 0; pos < n; ++pos)
    orig_to_new[new_order[pos]] = pos;

  // Collect undo entries: for each selected object, record before/after
  std::vector<ZOrderCommand::Entry> entries;
  for (auto &[orig_idx, obj] : sel_items)
    entries.push_back({obj->id, orig_idx, orig_to_new[orig_idx]});

  // Reorder children in-place using the permutation
  {
    auto &ch = parent->children;
    std::vector<std::unique_ptr<SceneNode>> reordered;
    reordered.reserve(n);
    for (int orig : new_order)
      reordered.push_back(std::move(ch[orig]));
    ch = std::move(reordered);
  }

  // Snapshot after order
  after_order.reserve(n);
  for (auto &c : parent->children)
    after_order.push_back(c->id);

  // ── 4. Push undo + signal ─────────────────────────────────────────────
  if (m_history)
    m_history->push(std::make_unique<ZOrderCommand>(
        parent, std::move(entries), std::move(before_order),
        std::move(after_order), desc));

  m_sig_doc_changed.emit();
  queue_draw();
  {
    MacroStep s;
    switch (op) {
    case ArrangeOp::BringToFront:
      s.op = MacroStep::Op::BringToFront;
      break;
    case ArrangeOp::BringForward:
      s.op = MacroStep::Op::BringForward;
      break;
    case ArrangeOp::SendBackward:
      s.op = MacroStep::Op::SendBackward;
      break;
    case ArrangeOp::SendToBack:
      s.op = MacroStep::Op::SendToBack;
      break;
    }
    record_step_if_recording(s);
  }
  LOG_INFO("Canvas: {} ({} objects)", desc, sel_items.size());
}

void Canvas::reverse_selected_path() {
  if (!m_selected || !m_selected->path)
    return;
  if (m_selected->path->nodes.size() < 2)
    return;

  BezierPath bp = BezierPath::from_path_data(*m_selected->path);
  PathData before = *m_selected->path;
  bp.reverse();
  // Keep selected node index mapped to its reversed position
  if (m_selected_node >= 0) {
    int n = (int)bp.nodes.size();
    m_selected_node = n - 1 - m_selected_node;
  }
  *m_selected->path = bp.to_path_data();
  if (m_history)
    m_history->push(std::make_unique<EditPathCommand>(
        m_selected, std::move(before), *m_selected->path, "Reverse path"));

  // ── Record macro step ─────────────────────────────────────────────────
  {
    MacroStep s;
    s.op = MacroStep::Op::ReversePath;
    record_step_if_recording(s);
  }

  m_sig_doc_changed.emit();
  if (m_selected_node >= 0)
    m_sig_node_changed.emit(m_selected, m_selected_node);
  queue_draw();
  LOG_INFO("Canvas: reversed path '{}'", m_selected->id);
}

// ── open_selected_at_node
// ─────────────────────────────────────────────────────
void Canvas::open_selected_at_node() {
  if (!m_selected || !m_selected->path) {
    return;
  }
  if (!m_selected->path->closed) {
    // B on an open path is a no-op — caller should have routed to
    // split_selected_at_node instead. Silent here; the dispatch lives
    // at the keypress site.
    return;
  }
  if (m_selected_node < 0) {
    return;
  }
  if ((int)m_selected->path->nodes.size() < 2) {
    return;
  }

  BezierPath bp = BezierPath::from_path_data(*m_selected->path);
  PathData before = *m_selected->path;
  bp.open_at_node(m_selected_node);

  // Nudge the new tail (duplicate of the break node) by a small amount
  // so the user can see the path has separated — ~4 screen pixels in doc space.
  if (!bp.nodes.empty()) {
    double nudge = 4.0 / m_zoom;
    bp.nodes.back().x += nudge;
    bp.nodes.back().y += nudge;
    bp.nodes.back().cx1 += nudge;
    bp.nodes.back().cy1 += nudge;
    bp.nodes.back().cx2 += nudge;
    bp.nodes.back().cy2 += nudge;
  }
  *m_selected->path = bp.to_path_data();

  if (m_history)
    m_history->push(std::make_unique<EditPathCommand>(
        m_selected, std::move(before), *m_selected->path, "Open path at node"));

  // Select the new tail (last node) — the cut point
  m_selected_node = (int)m_selected->path->nodes.size() - 1;
  m_sig_doc_changed.emit();
  m_sig_node_changed.emit(m_selected, m_selected_node);
  queue_draw();
  LOG_INFO("Canvas: opened path '{}' at node", m_selected->id);
}

// ── split_selected_at_node
// ────────────────────────────────────────────────────
void Canvas::split_selected_at_node() {
  if (!m_selected || !m_selected->path) {
    return;
  }
  if (m_selected->path->closed) {
    // Split only applies to open paths; closed paths use Open here.
    // Caller should have routed; silent here.
    return;
  }
  if (m_selected_node <= 0) {
    return;
  }
  int n = (int)m_selected->path->nodes.size();
  if (m_selected_node >= n - 1) {
    return;
  }
  if (n < 3) {
    return;
  }
  if (!m_doc) {
    return;
  }

  // Find parent layer
  SceneNode *parent_layer = nullptr;
  int orig_index = -1;
  for (auto &layer : m_doc->layers) {
    for (int i = 0; i < (int)layer->children.size(); ++i) {
      if (layer->children[i].get() == m_selected) {
        parent_layer = layer.get();
        orig_index = i;
        break;
      }
    }
    if (parent_layer)
      break;
  }
  if (!parent_layer || orig_index < 0) {
    // Path is nested in a Group/Compound — split currently only
    // supports top-level layer children. Kept as INFO so a user who
    // hits this gets a hint in the log; the hotkey otherwise just
    // appears to do nothing.
    LOG_INFO("Canvas: split_at_node — selected path is nested in a "
             "Group/Compound; not currently supported");
    return;
  }

  BezierPath bp = BezierPath::from_path_data(*m_selected->path);
  auto [left_pd, right_pd] = bp.split_at_node(m_selected_node);
  if (left_pd.nodes.empty() || right_pd.nodes.empty()) {
    LOG_WARN("Canvas: split_at_node({}) produced empty halves "
             "(left={}, right={}) — math layer disagrees with caller's guards",
             m_selected_node, (int)left_pd.nodes.size(),
             (int)right_pd.nodes.size());
    return;
  }

  // Nudge the right path's head (shared break node) so the split is visible
  {
    double nudge = 4.0 / m_zoom;
    auto &nd = right_pd.nodes.front();
    nd.x += nudge;
    nd.y += nudge;
    nd.cx1 += nudge;
    nd.cy1 += nudge;
    nd.cx2 += nudge;
    nd.cy2 += nudge;
  }

  // Build two new SceneNodes inheriting appearance from the original
  auto left_node = std::make_unique<SceneNode>();
  left_node->type = SceneNode::Type::Path;
  left_node->id = m_selected->id + "_L";
  left_node->name = m_selected->name;
  left_node->fill = m_selected->fill;
  left_node->stroke = m_selected->stroke;
  left_node->path = std::make_unique<PathData>(left_pd);

  auto right_node = std::make_unique<SceneNode>();
  right_node->type = SceneNode::Type::Path;
  right_node->id = m_selected->id + "_R";
  right_node->name = m_selected->name;
  right_node->fill = m_selected->fill;
  right_node->stroke = m_selected->stroke;
  right_node->path = std::make_unique<PathData>(right_pd);

  // Push undo before mutating
  if (m_history) {
    auto orig_snap = clone_node(*m_selected);
    auto left_snap = clone_node(*left_node);
    auto right_snap = clone_node(*right_node);
    m_history->push(std::make_unique<SplitPathCommand>(
        parent_layer, std::move(orig_snap), orig_index, std::move(left_snap),
        std::move(right_snap)));
  }

  // Replace original with the two halves
  SceneNode *right_raw = right_node.get();
  // Danger zone: between the erase and `m_selected = right_raw` below,
  // the original SceneNode is freed but `m_selected`, `m_selection`,
  // `m_node_selection` (and any cached pointers in PropertiesPanel)
  // still reference it. Anything that derefs those during this window
  // is undefined behaviour — keep this region tight, no signal emits.
  parent_layer->children.erase(parent_layer->children.begin() + orig_index);
  // After this point m_selected is dangling until the assignment below.
  parent_layer->children.insert(parent_layer->children.begin() + orig_index,
                                std::move(right_node));
  parent_layer->children.insert(parent_layer->children.begin() + orig_index,
                                std::move(left_node));

  // Select the right piece, head node (the cut point). m_selection /
  // m_node_selection still hold dangling pointers to the destroyed
  // original at this point; the signal_selection emit below propagates
  // the new m_selected to listeners which refresh their state.
  m_selected = right_raw;
  m_selected_node = 0;
  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: split path at node {}", m_selected_node);
}

void Canvas::clamp_pan() {
  if (!m_doc)
    return;
  const double cw = m_doc->canvas_width() * m_zoom;
  const double ch = m_doc->canvas_height() * m_zoom;
  // Allow panning until only a small margin strip of the artboard remains
  // visible. margin = max(40px, 10% of canvas screen size) — scales with zoom
  // so you can't lose the artboard entirely at any zoom level.
  const double margin_x = std::max(40.0, cw * 0.10);
  const double margin_y = std::max(40.0, ch * 0.10);
  const double cx = (get_width() - cw) * 0.5;
  const double cy = (get_height() - ch) * 0.5;
  m_pan_x =
      std::clamp(m_pan_x, -cx - cw + margin_x, -cx + get_width() - margin_x);
  m_pan_y =
      std::clamp(m_pan_y, -cy - ch + margin_y, -cy + get_height() - margin_y);
}

// ── Bounding box ─────────────────────────────────────────────────────────────
std::optional<Canvas::BBox> Canvas::object_bbox(const SceneNode &obj,
                                                bool include_stroke) const {
  // Ref points have no geometric bbox
  if (obj.type == SceneNode::Type::Ref)
    return std::nullopt;

  // ── Image: axis-aligned bbox from stored position/size ────────────────
  if (obj.type == SceneNode::Type::Image) {
    if (obj.image_w <= 0 || obj.image_h <= 0)
      return std::nullopt;
    return BBox{obj.image_x, obj.image_y, obj.image_w, obj.image_h};
  }

  // ── Group / Compound: union of all children's bboxes ─────────────────
  if (obj.type == SceneNode::Type::Group ||
      obj.type == SceneNode::Type::Compound) {
    std::optional<BBox> result;
    for (const auto &child : obj.children) {
      auto bb = object_bbox(*child, include_stroke);
      if (!bb)
        continue;
      if (!result) {
        result = bb;
        continue;
      }
      double x1 = std::min(result->x, bb->x);
      double y1 = std::min(result->y, bb->y);
      double x2 = std::max(result->x + result->w, bb->x + bb->w);
      double y2 = std::max(result->y + result->h, bb->y + bb->h);
      result = BBox{x1, y1, x2 - x1, y2 - y1};
    }
    return result;
  }

  // ── ClipGroup: bbox is the clip shape's own physical extent.
  //   The clip shape defines the region; clipped children that lie
  //   outside it aren't visible, so the ClipGroup's selection bbox
  //   shouldn't shrink around their visible portions. If there is no
  //   clip shape yet (degenerate) fall back to the children union.
  if (obj.type == SceneNode::Type::ClipGroup) {
    if (obj.clip_shape) {
      auto clip_bb = object_bbox(*obj.clip_shape, include_stroke);
      if (clip_bb)
        return clip_bb;
    }
    std::optional<BBox> kids;
    for (const auto &child : obj.children) {
      auto bb = object_bbox(*child, include_stroke);
      if (!bb)
        continue;
      if (!kids) {
        kids = bb;
        continue;
      }
      double x1 = std::min(kids->x, bb->x);
      double y1 = std::min(kids->y, bb->y);
      double x2 = std::max(kids->x + kids->w, bb->x + bb->w);
      double y2 = std::max(kids->y + kids->h, bb->y + bb->h);
      kids = BBox{x1, y1, x2 - x1, y2 - y1};
    }
    return kids;
  }

  // ── Blend: union of A, B, and cache bboxes. Because every cache
  //   entry's geometry lies strictly between A and B (linear lerp of
  //   anchors/handles), the A∪B bbox actually bounds everything in v1.
  //   We still union the cache for symmetry and forward-compat (future
  //   non-linear spines, easing curves, etc).
  if (obj.type == SceneNode::Type::Blend) {
    std::optional<BBox> result;
    auto accumulate = [&](const SceneNode *n) {
      if (!n)
        return;
      auto bb = object_bbox(*n, include_stroke);
      if (!bb)
        return;
      if (!result) {
        result = bb;
        return;
      }
      double x1 = std::min(result->x, bb->x);
      double y1 = std::min(result->y, bb->y);
      double x2 = std::max(result->x + result->w, bb->x + bb->w);
      double y2 = std::max(result->y + result->h, bb->y + bb->h);
      result = BBox{x1, y1, x2 - x1, y2 - y1};
    };
    accumulate(obj.blend_source_a.get());
    accumulate(obj.blend_source_b.get());
    for (const auto &step : obj.blend_cache)
      accumulate(step.get());
    return result;
  }

  // ── Warp: bbox is the union of source + both caches. In the steady
  //   state warp_cache is what's actually visible, and its bbox bounds
  //   the rendered warped geometry. Including warp_source keeps the
  //   bbox sensible in transient states where the caches are empty
  //   (just-constructed Warp before first rebuild) or being rebuilt.
  //   warp_glyph_cache is included symmetrically — for path sources
  //   its bbox equals the source's, but this future-proofs when text
  //   sources make glyph_cache diverge from source.
  if (obj.type == SceneNode::Type::Warp) {
    std::optional<BBox> result;
    auto accumulate = [&](const SceneNode *n) {
      if (!n)
        return;
      auto bb = object_bbox(*n, include_stroke);
      if (!bb)
        return;
      if (!result) {
        result = bb;
        return;
      }
      double x1 = std::min(result->x, bb->x);
      double y1 = std::min(result->y, bb->y);
      double x2 = std::max(result->x + result->w, bb->x + bb->w);
      double y2 = std::max(result->y + result->h, bb->y + bb->h);
      result = BBox{x1, y1, x2 - x1, y2 - y1};
    };
    accumulate(obj.warp_source.get());
    accumulate(obj.warp_glyph_cache.get());
    accumulate(obj.warp_cache.get());
    return result;
  }

  // ── Text: measure actual layout extents via Pango ──────────────────────
  if (obj.type == SceneNode::Type::Text) {
    // Linked text-on-path: use the guide path's bbox so the selection box
    // reflects where the text actually renders, not the creation point.
    if (!obj.text_path_id.empty()) {
      SceneNode *guide = top_find_path_by_id(obj.text_path_id);
      if (guide)
        return object_bbox(*guide, include_stroke);
    }
    // Use a scratch Cairo surface — we only need the layout metrics.
    auto surf =
        Cairo::ImageSurface::create(Cairo::ImageSurface::Format::ARGB32, 1, 1);
    auto cr = Cairo::Context::create(surf);

    PangoLayout *layout = pango_cairo_create_layout(cr->cobj());
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, obj.text_font_family.c_str());
    pango_font_description_set_absolute_size(desc,
                                             obj.text_font_size * PANGO_SCALE);
    if (obj.text_bold)
      pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    if (obj.text_italic)
      pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, obj.text_content.c_str(), -1);

    // Match draw_text_node: apply letter spacing so width is accurate.
    if (obj.text_letter_spacing != 0.0) {
      PangoAttrList *attrs = pango_attr_list_new();
      pango_attr_list_insert(attrs,
                             pango_attr_letter_spacing_new(
                                 (int)(obj.text_letter_spacing * PANGO_SCALE)));
      pango_layout_set_attributes(layout, attrs);
      pango_attr_list_unref(attrs);
    }

    PangoRectangle ink, logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);

    int baseline = pango_layout_get_baseline(layout);
    double base_px = baseline / (double)PANGO_SCALE;

    g_object_unref(layout);

    // Use ink extents for tightest bbox (logical includes descender space).
    double w = ink.width > 0 ? ink.width : logical.width;
    double h = ink.height > 0 ? ink.height : logical.height;

    // Anchor offset — same logic as draw_text_node.
    double off_x = 0.0;
    if (obj.text_anchor == "middle")
      off_x = -logical.width * 0.5;
    if (obj.text_anchor == "end")
      off_x = -logical.width;

    // ink.x/y are offsets from the layout origin (which is top-left).
    // text_y is the baseline in doc-space Y-down.
    // Baseline shift offsets the whole block upward in doc space (Y-down =
    // subtract).
    double bx = obj.text_x + off_x + ink.x;
    double by = obj.text_y - base_px + ink.y - obj.text_baseline_shift;

    double outset = include_stroke ? obj.stroke.width * 0.5 : 0.0;
    return BBox{bx - outset, by - outset, w + outset * 2, h + outset * 2};
  }

  if (obj.type == SceneNode::Type::Path && obj.path &&
      !obj.path->nodes.empty()) {
    const auto &nodes = obj.path->nodes;
    double minx = nodes[0].x, maxx = minx;
    double miny = nodes[0].y, maxy = miny;

    // Compute tight curve bounds by sampling each cubic segment
    // Segments: node[i] → node[i+1], using out-handle of i and in-handle of i+1
    auto expand_cubic = [&](double p0x, double p0y, double p1x, double p1y,
                            double p2x, double p2y, double p3x, double p3y) {
      // Sample 32 points along the cubic — sufficient for icon/glyph scale
      for (int s = 0; s <= 32; ++s) {
        double t = s / 32.0;
        double mt = 1.0 - t;
        double x = mt * mt * mt * p0x + 3 * mt * mt * t * p1x +
                   3 * mt * t * t * p2x + t * t * t * p3x;
        double y = mt * mt * mt * p0y + 3 * mt * mt * t * p1y +
                   3 * mt * t * t * p2y + t * t * t * p3y;
        minx = std::min(minx, x);
        maxx = std::max(maxx, x);
        miny = std::min(miny, y);
        maxy = std::max(maxy, y);
      }
    };

    int n = (int)nodes.size();
    for (int i = 0; i < n - 1; ++i) {
      expand_cubic(nodes[i].x, nodes[i].y, nodes[i].cx2, nodes[i].cy2,
                   nodes[i + 1].cx1, nodes[i + 1].cy1, nodes[i + 1].x,
                   nodes[i + 1].y);
    }
    // Closing segment if path is closed
    if (obj.path->closed && n > 1) {
      expand_cubic(nodes[n - 1].x, nodes[n - 1].y, nodes[n - 1].cx2,
                   nodes[n - 1].cy2, nodes[0].cx1, nodes[0].cy1, nodes[0].x,
                   nodes[0].y);
    }

    // ── Stroke bbox expansion ─────────────────────────────────────────
    //
    // The stroke contributes width/2 perpendicular to the path along
    // every segment — that's covered by uniform AABB outset and is
    // correct for closed paths and for round/square caps on open paths.
    //
    // Cap type only affects the LONGITUDINAL extension at OPEN-PATH
    // ENDPOINTS. Round and Square caps extend width/2 past the endpoint
    // along the tangent. Butt does NOT — it ends flat at the endpoint.
    //
    // The pre-S104 code padded uniformly by width/2 on all four sides
    // regardless of cap, which was wrong for Butt: drawing a horizontal
    // or vertical line with butt cap would show a bbox extending past
    // the endpoints by width/2 in the tangent direction, when the
    // actual ink stops at the endpoint.
    //
    // Fix: pad uniformly first (covers perpendicular bulge + joins +
    // round/square caps). Then, for open paths with Butt cap, carve
    // back the bbox by `width/2 × |tangent_axis_component|` at each
    // endpoint, but ONLY on the side(s) where the endpoint is the
    // current AABB extreme. The "owner" check is critical: if some
    // interior cubic bulge is what drove maxy, we mustn't shrink maxy
    // just because the start endpoint's tangent points down.
    double outset = include_stroke ? obj.stroke.width * 0.5 : 0.0;
    double bx = minx - outset;
    double by = miny - outset;
    double bw = (maxx - minx) + outset * 2;
    double bh = (maxy - miny) + outset * 2;

    if (include_stroke && outset > 0.0 && !obj.path->closed && n >= 2 &&
        obj.stroke.cap == LineCap::Butt) {
      // Compute outward tangents at the two open endpoints. Outward =
      // pointing away from the path interior.
      //
      // Start endpoint (node 0): outward = node[0] - out-handle of node[0].
      //   Fallback for degenerate handle: node[0] - node[1] (straight seg).
      // End endpoint   (node n-1): outward = node[n-1] - in-handle of n-1.
      //   Fallback: node[n-1] - node[n-2].
      auto outward_tangent = [](double nx, double ny,
                                double hx, double hy,
                                double anx, double any,
                                double& tx, double& ty) {
        double dx = nx - hx;
        double dy = ny - hy;
        double len2 = dx * dx + dy * dy;
        if (len2 < 1e-12) {
          dx = nx - anx;
          dy = ny - any;
          len2 = dx * dx + dy * dy;
          if (len2 < 1e-12) { tx = 0.0; ty = 0.0; return; }
        }
        double inv = 1.0 / std::sqrt(len2);
        tx = dx * inv;
        ty = dy * inv;
      };

      double s_tx = 0.0, s_ty = 0.0;
      double e_tx = 0.0, e_ty = 0.0;
      outward_tangent(nodes[0].x, nodes[0].y,
                      nodes[0].cx2, nodes[0].cy2,
                      nodes[1].x, nodes[1].y,
                      s_tx, s_ty);
      outward_tangent(nodes[n - 1].x, nodes[n - 1].y,
                      nodes[n - 1].cx1, nodes[n - 1].cy1,
                      nodes[n - 2].x, nodes[n - 2].y,
                      e_tx, e_ty);

      // For each endpoint, on each axis, if the endpoint's coordinate
      // is the current bbox extreme on the side its tangent points to,
      // carve back the over-pad by the correct amount.
      //
      // The correct cap-AABB contribution for a butt-capped endpoint
      // with outward tangent (tx, ty) is endpoint ± (h·|ty|, h·|tx|).
      // (The cap segment is perpendicular to tangent, length width;
      // its AABB-x is h × |perp_x| = h × |ty|, AABB-y is h × |tx|.)
      //
      // The uniform pad outset h on the side the endpoint owns was
      // therefore an over-pad by:
      //   x: h × (1 - |ty|)   when endpoint is the x-extreme
      //   y: h × (1 - |tx|)   when endpoint is the y-extreme
      //
      // Sanity: horizontal line (tx=±1, ty=0) → x carve = h, y carve = 0.
      // Bbox shrinks by h past each endpoint in x (correct — butt cap
      // is flush at endpoint), no carve in y (correct — perpendicular
      // is full h). Vertical line: symmetric. Diagonal: partial carve
      // both axes, matching the rotated cap's AABB exactly.
      //
      // Equality check uses a small epsilon: floating-point identity
      // between sampled cubic extreme and node coordinate isn't
      // guaranteed in the curved-segment case, but at the endpoint
      // (t=0 / t=1) the sample IS the node, so equality holds modulo
      // arithmetic.
      auto carve = [&](double ex, double ey, double tx, double ty) {
        const double eps = 1e-9;
        double abs_tx = std::fabs(tx);
        double abs_ty = std::fabs(ty);
        // X axis — carve amount is outset × (1 - |ty|).
        double x_carve = outset * (1.0 - abs_ty);
        if (tx > 0.0 && std::fabs(ex - maxx) < eps) {
          bw -= x_carve;
        } else if (tx < 0.0 && std::fabs(ex - minx) < eps) {
          bx += x_carve;
          bw -= x_carve;
        }
        // Y axis — carve amount is outset × (1 - |tx|).
        double y_carve = outset * (1.0 - abs_tx);
        if (ty > 0.0 && std::fabs(ey - maxy) < eps) {
          bh -= y_carve;
        } else if (ty < 0.0 && std::fabs(ey - miny) < eps) {
          by += y_carve;
          bh -= y_carve;
        }
      };

      carve(nodes[0].x, nodes[0].y, s_tx, s_ty);
      carve(nodes[n - 1].x, nodes[n - 1].y, e_tx, e_ty);
    }

    return BBox{bx, by, bw, bh};
  }
  return std::nullopt;
}

// ── Hit test ─────────────────────────────────────────────────────────────────
SceneNode *Canvas::hit_test(double doc_x, double doc_y) {
  if (!m_doc)
    return nullptr;

  // Recursive helper — tests a list of children, returns first hit
  // Returns the child itself, OR the group if hit is inside a group.
  //
  // s112 m4: when parent_is_compound is true, the children are subpaths
  // of a Compound. By rendering convention the Compound parent owns the
  // fill and child Paths have fill.type==None — so the normal "filled
  // path interior counts as a hit" test never fires for them, and a
  // click in the Compound's filled interior would miss. We patch that
  // here by treating a closed-path interior-bbox hit as a fill hit
  // when recursing inside a Compound. (Cycle-through Ctrl+click relies
  // on hit_test/hit_test_next correctly returning the Compound for
  // interior clicks.)
  std::function<SceneNode *(std::vector<std::unique_ptr<SceneNode>> &, bool)>
      test_children;
  test_children =
      [&](std::vector<std::unique_ptr<SceneNode>> &children,
          bool parent_is_compound) -> SceneNode * {
    // Index 0 is drawn last = visually on top. Iterate 0→N to hit front objects
    // first.
    for (int oi = 0; oi < (int)children.size(); ++oi) {
      SceneNode &obj = *children[oi];
      if (!obj.visible)
        continue;

      if (obj.type == SceneNode::Type::Group ||
          obj.type == SceneNode::Type::Compound) {
        // Test bbox first
        auto bb = object_bbox(obj);
        if (!bb)
          continue;
        if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
            doc_y > bb->y + bb->h)
          continue;
        // Hit inside bbox — test children. Compound children inherit the
        // parent's fill semantics for hit purposes (see helper banner).
        bool nested_is_compound = (obj.type == SceneNode::Type::Compound);
        SceneNode *child_hit = test_children(obj.children, nested_is_compound);
        if (child_hit)
          return &obj; // return the GROUP/COMPOUND, not the child
        continue;
      }

      // ── ClipGroup: like Group, but the click must additionally fall
      //   within the clip shape's bbox — otherwise the pixel the user
      //   clicked is masked out and shouldn't count as a hit.
      //   We return the ClipGroup itself (not a child), matching how
      //   Group hits return the group.
      if (obj.type == SceneNode::Type::ClipGroup) {
        auto bb = object_bbox(obj);
        if (!bb)
          continue;
        if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
            doc_y > bb->y + bb->h)
          continue;
        // Extra gate on the clip shape bbox (if present).
        if (obj.clip_shape) {
          auto cbb = object_bbox(*obj.clip_shape);
          if (cbb) {
            if (doc_x < cbb->x || doc_x > cbb->x + cbb->w || doc_y < cbb->y ||
                doc_y > cbb->y + cbb->h)
              continue;
          }
        }
        SceneNode *child_hit = test_children(obj.children, false);
        if (child_hit)
          return &obj;
        continue;
      }

      // ── Blend: bbox gate on A/B/cache. A/B live in slots, not
      //   children, so we build a temporary children-like vector of
      //   pointers. Returning the Blend itself (not A/B/a step) is
      //   the right answer: the Blend is the user-visible object,
      //   and selection of internal A/B will come via LayersPanel in M2.
      //   Any hit inside the Blend's rendered region selects the Blend.
      if (obj.type == SceneNode::Type::Blend) {
        auto bb = object_bbox(obj);
        if (!bb)
          continue;
        if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
            doc_y > bb->y + bb->h)
          continue;
        // Test the actual geometry — walk A/B/cache with bbox + path
        // check via object_bbox on each. Cheap and correct: if any
        // sub-element's bbox contains the point, claim the Blend.
        auto inside = [&](const SceneNode *n) -> bool {
          if (!n)
            return false;
          auto sb = object_bbox(*n);
          return sb && doc_x >= sb->x && doc_x <= sb->x + sb->w &&
                 doc_y >= sb->y && doc_y <= sb->y + sb->h;
        };
        if (inside(obj.blend_source_a.get()) ||
            inside(obj.blend_source_b.get())) {
          return &obj;
        }
        bool cache_hit = false;
        for (const auto &step : obj.blend_cache) {
          if (inside(step.get())) {
            cache_hit = true;
            break;
          }
        }
        if (cache_hit)
          return &obj;
        continue;
      }

      // ── Warp: same pattern as Blend — bbox gate, then per-slot
      //   containment against source + caches. Returning the Warp
      //   itself is the right answer: the Warp is the user-visible
      //   object; source/cache selection comes via LayersPanel.
      if (obj.type == SceneNode::Type::Warp) {
        auto bb = object_bbox(obj);
        if (!bb)
          continue;
        if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
            doc_y > bb->y + bb->h)
          continue;
        auto inside = [&](const SceneNode *n) -> bool {
          if (!n)
            return false;
          auto sb = object_bbox(*n);
          return sb && doc_x >= sb->x && doc_x <= sb->x + sb->w &&
                 doc_y >= sb->y && doc_y <= sb->y + sb->h;
        };
        if (inside(obj.warp_source.get()) ||
            inside(obj.warp_glyph_cache.get()) ||
            inside(obj.warp_cache.get())) {
          return &obj;
        }
        continue;
      }

      // ── Text / Image: use object_bbox for hit testing ─────────────
      if (obj.type == SceneNode::Type::Text ||
          obj.type == SceneNode::Type::Image) {
        auto bb = object_bbox(obj, false);
        if (bb && doc_x >= bb->x && doc_x <= bb->x + bb->w && doc_y >= bb->y &&
            doc_y <= bb->y + bb->h)
          return &obj;
        continue;
      }

      if (obj.type != SceneNode::Type::Path || !obj.path)
        continue;
      const auto &nodes = obj.path->nodes;
      if (nodes.empty())
        continue;

      auto bb = object_bbox(obj);
      if (!bb)
        continue;
      if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
          doc_y > bb->y + bb->h)
        continue;

      // s112 m4: compound children carry no fill of their own (parent
      // owns it by convention), so a click inside the compound's filled
      // interior would otherwise miss. Treat closed-path interior bbox
      // hits as fill hits when recursing inside a Compound. The parent
      // re-tests bbox-containment via the outer Compound branch, so
      // this is sound: we only get here for closed children of a
      // Compound whose own bbox already contains the click.
      bool has_fill = (obj.fill.type != FillStyle::Type::None) ||
                      (parent_is_compound && obj.path->closed);
      if (has_fill)
        return &obj;

      double threshold = std::max(obj.stroke.width * 0.5, 2.0);
      double thresh2 = threshold * threshold;

      auto point_on_cubic = [&](double p0x, double p0y, double p1x, double p1y,
                                double p2x, double p2y, double p3x,
                                double p3y) -> bool {
        for (int s = 0; s <= 32; ++s) {
          double t = s / 32.0;
          double mt = 1.0 - t;
          double x = mt * mt * mt * p0x + 3 * mt * mt * t * p1x +
                     3 * mt * t * t * p2x + t * t * t * p3x;
          double y = mt * mt * mt * p0y + 3 * mt * mt * t * p1y +
                     3 * mt * t * t * p2y + t * t * t * p3y;
          double dx = x - doc_x, dy = y - doc_y;
          if (dx * dx + dy * dy <= thresh2)
            return true;
        }
        return false;
      };

      int n = (int)nodes.size();
      bool hit = false;
      for (int i = 0; i < n - 1 && !hit; ++i) {
        hit = point_on_cubic(nodes[i].x, nodes[i].y, nodes[i].cx2, nodes[i].cy2,
                             nodes[i + 1].cx1, nodes[i + 1].cy1, nodes[i + 1].x,
                             nodes[i + 1].y);
      }
      if (!hit && obj.path->closed && n > 1) {
        hit = point_on_cubic(nodes[n - 1].x, nodes[n - 1].y, nodes[n - 1].cx2,
                             nodes[n - 1].cy2, nodes[0].cx1, nodes[0].cy1,
                             nodes[0].x, nodes[0].y);
      }
      if (hit)
        return &obj;
    }
    return nullptr;
  };

  // Iterate layers top-to-bottom (after the z-order fix, index n-1 = visually
  // on top, so walk highest-index first and pick the first hit).
  for (int li = (int)m_doc->layers.size() - 1; li >= 0; --li) {
    auto &layer = m_doc->layers[li];
    if (!layer->visible || layer->locked || layer->is_special_layer())
      continue;
    SceneNode *hit = test_children(layer->children, false);
    if (hit)
      return hit;
  }
  return nullptr;
}

// ── hit_test_next
// ───────────────────────────────────────────────────────────── Like hit_test
// but skips `skip` — returns the next object underneath it. Used for Ctrl+click
// select-through (Illustrator convention).
SceneNode *Canvas::hit_test_next(double doc_x, double doc_y, SceneNode *skip) {
  if (!m_doc || !skip)
    return hit_test(doc_x, doc_y);

  bool found_skip = false;

  // s113: non-skip-aware "does this subtree contain the click" probe.
  // Used by the Group/Compound/ClipGroup branches below to decide
  // "is this composite under the click" independent of where `skip`
  // lives. The original code piggy-backed on the recursive test_children
  // truthiness as the "is hit" signal, but when skip IS the composite
  // itself, the recursion returns null (no child matches skip, found_skip
  // never flips inside) — the caller then bails silently and Ctrl+click
  // gets stuck on the group instead of advancing past it. This helper
  // gives a clean boolean answer with no skip-state coupling.
  std::function<bool(SceneNode &, bool)> subtree_hit;
  subtree_hit = [&](SceneNode &obj, bool parent_is_compound) -> bool {
    if (!obj.visible)
      return false;
    auto bb = object_bbox(obj);
    if (!bb)
      return false;
    if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
        doc_y > bb->y + bb->h)
      return false;

    if (obj.type == SceneNode::Type::Group ||
        obj.type == SceneNode::Type::Compound) {
      bool nested_is_compound = (obj.type == SceneNode::Type::Compound);
      for (auto &ch : obj.children)
        if (subtree_hit(*ch, nested_is_compound))
          return true;
      return false;
    }
    if (obj.type == SceneNode::Type::ClipGroup) {
      if (obj.clip_shape) {
        auto cbb = object_bbox(*obj.clip_shape);
        if (cbb && (doc_x < cbb->x || doc_x > cbb->x + cbb->w ||
                    doc_y < cbb->y || doc_y > cbb->y + cbb->h))
          return false;
      }
      for (auto &ch : obj.children)
        if (subtree_hit(*ch, false))
          return true;
      return false;
    }
    if (obj.type == SceneNode::Type::Text ||
        obj.type == SceneNode::Type::Image) {
      // bbox already passed above
      return true;
    }
    if (obj.type == SceneNode::Type::Blend ||
        obj.type == SceneNode::Type::Warp) {
      // Mirror hit_test's per-slot containment for these — just bbox
      // is sufficient here since the outer bbox already passed; a
      // tighter check matches hit_test's behaviour.
      auto inside = [&](const SceneNode *n) -> bool {
        if (!n)
          return false;
        auto sb = object_bbox(*n);
        return sb && doc_x >= sb->x && doc_x <= sb->x + sb->w &&
               doc_y >= sb->y && doc_y <= sb->y + sb->h;
      };
      if (obj.type == SceneNode::Type::Blend) {
        if (inside(obj.blend_source_a.get()) ||
            inside(obj.blend_source_b.get()))
          return true;
        for (const auto &step : obj.blend_cache)
          if (inside(step.get()))
            return true;
        return false;
      } else { // Warp
        return inside(obj.warp_source.get()) ||
               inside(obj.warp_glyph_cache.get()) ||
               inside(obj.warp_cache.get());
      }
    }
    if (obj.type != SceneNode::Type::Path || !obj.path)
      return false;
    const auto &nodes = obj.path->nodes;
    if (nodes.empty())
      return false;
    bool has_fill = (obj.fill.type != FillStyle::Type::None) ||
                    (parent_is_compound && obj.path->closed);
    if (has_fill)
      return true;
    double threshold = std::max(obj.stroke.width * 0.5, 2.0);
    double thresh2 = threshold * threshold;
    auto point_on_cubic = [&](double p0x, double p0y, double p1x, double p1y,
                              double p2x, double p2y, double p3x,
                              double p3y) -> bool {
      for (int s = 0; s <= 32; ++s) {
        double t = s / 32.0;
        double mt = 1.0 - t;
        double x = mt * mt * mt * p0x + 3 * mt * mt * t * p1x +
                   3 * mt * t * t * p2x + t * t * t * p3x;
        double y = mt * mt * mt * p0y + 3 * mt * mt * t * p1y +
                   3 * mt * t * t * p2y + t * t * t * p3y;
        double ddx = x - doc_x, ddy = y - doc_y;
        if (ddx * ddx + ddy * ddy <= thresh2)
          return true;
      }
      return false;
    };
    int n = (int)nodes.size();
    for (int i = 0; i < n - 1; ++i)
      if (point_on_cubic(nodes[i].x, nodes[i].y, nodes[i].cx2, nodes[i].cy2,
                         nodes[i + 1].cx1, nodes[i + 1].cy1, nodes[i + 1].x,
                         nodes[i + 1].y))
        return true;
    if (obj.path->closed && n > 1) {
      if (point_on_cubic(nodes[n - 1].x, nodes[n - 1].y, nodes[n - 1].cx2,
                         nodes[n - 1].cy2, nodes[0].cx1, nodes[0].cy1,
                         nodes[0].x, nodes[0].y))
        return true;
    }
    return false;
  };

  // s112 m4: same parent_is_compound thread as in hit_test — closed
  // children of a Compound count as filled for hit purposes since the
  // parent owns the fill by convention.
  std::function<SceneNode *(std::vector<std::unique_ptr<SceneNode>> &, bool)>
      test_children;
  test_children =
      [&](std::vector<std::unique_ptr<SceneNode>> &children,
          bool parent_is_compound) -> SceneNode * {
    // Index 0 is drawn last = visually on top. Iterate 0→N to hit front objects
    // first.
    for (int oi = 0; oi < (int)children.size(); ++oi) {
      SceneNode &obj = *children[oi];
      if (!obj.visible)
        continue;

      if (obj.type == SceneNode::Type::Group ||
          obj.type == SceneNode::Type::Compound) {
        // s113: use the non-skip-aware subtree_hit probe so the "is this
        // group hit" decision is independent of where `skip` lives. The
        // old code used the recursive test_children's truthiness as the
        // hit signal, which silently failed when skip == &obj (the group
        // itself): the recursion never matches skip in its own children,
        // returns null, and the caller falls through without setting
        // found_skip — Ctrl+click then gets stuck on the group instead
        // of advancing to the next layer-level sibling.
        bool nested_is_compound = (obj.type == SceneNode::Type::Compound);
        bool group_hit = subtree_hit(obj, parent_is_compound);
        if (!group_hit) {
          continue;
        }
        if (&obj == skip) {
          found_skip = true;
          continue;
        }
        if (found_skip) {
          return &obj;
        }
        // Hit but pre-skip — recurse so that found_skip flips if skip
        // lives inside this group's subtree. After the recursion, if
        // found_skip is now true, this group itself is the "next" hit
        // (cycle-through escapes the group containing skip).
        (void)test_children(obj.children, nested_is_compound);
        if (found_skip) {
          return &obj;
        }
        continue;
      }

      // ── ClipGroup parallel to the hit_test branch above. ─────────────
      // s113: same fix shape as the Group/Compound branch — use
      // subtree_hit for "is this hit" so skip == &obj (the ClipGroup)
      // is handled correctly.
      if (obj.type == SceneNode::Type::ClipGroup) {
        bool group_hit = subtree_hit(obj, false);
        if (!group_hit) {
          continue;
        }
        if (&obj == skip) {
          found_skip = true;
          continue;
        }
        if (found_skip) {
          return &obj;
        }
        // Pre-skip — recurse to let found_skip flip if skip is inside.
        (void)test_children(obj.children, false);
        if (found_skip) {
          return &obj;
        }
        continue;
      }

      if (obj.type == SceneNode::Type::Text ||
          obj.type == SceneNode::Type::Image) {
        auto bb = object_bbox(obj, false);
        if (bb && doc_x >= bb->x && doc_x <= bb->x + bb->w && doc_y >= bb->y &&
            doc_y <= bb->y + bb->h) {
          if (&obj == skip) {
            found_skip = true;
            continue;
          }
          if (found_skip)
            return &obj;
        }
        continue;
      }

      if (obj.type != SceneNode::Type::Path || !obj.path)
        continue;
      const auto &nodes = obj.path->nodes;
      if (nodes.empty())
        continue;

      auto bb = object_bbox(obj);
      if (!bb)
        continue;
      if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
          doc_y > bb->y + bb->h)
        continue;

      // s112 m4: see hit_test for the parent_is_compound rationale.
      bool has_fill = (obj.fill.type != FillStyle::Type::None) ||
                      (parent_is_compound && obj.path->closed);
      bool hit = has_fill;

      if (!hit) {
        double threshold = std::max(obj.stroke.width * 0.5, 2.0);
        double thresh2 = threshold * threshold;
        auto point_on_cubic = [&](double p0x, double p0y, double p1x,
                                  double p1y, double p2x, double p2y,
                                  double p3x, double p3y) -> bool {
          for (int s = 0; s <= 32; ++s) {
            double t = s / 32.0;
            double mt = 1.0 - t;
            double x = mt * mt * mt * p0x + 3 * mt * mt * t * p1x +
                       3 * mt * t * t * p2x + t * t * t * p3x;
            double y = mt * mt * mt * p0y + 3 * mt * mt * t * p1y +
                       3 * mt * t * t * p2y + t * t * t * p3y;
            double ddx = x - doc_x, ddy = y - doc_y;
            if (ddx * ddx + ddy * ddy <= thresh2)
              return true;
          }
          return false;
        };
        int n = (int)nodes.size();
        for (int i = 0; i < n - 1 && !hit; ++i)
          hit = point_on_cubic(nodes[i].x, nodes[i].y, nodes[i].cx2,
                               nodes[i].cy2, nodes[i + 1].cx1, nodes[i + 1].cy1,
                               nodes[i + 1].x, nodes[i + 1].y);
        if (!hit && obj.path->closed && n > 1)
          hit = point_on_cubic(nodes[n - 1].x, nodes[n - 1].y, nodes[n - 1].cx2,
                               nodes[n - 1].cy2, nodes[0].cx1, nodes[0].cy1,
                               nodes[0].x, nodes[0].y);
      }

      if (hit) {
        if (&obj == skip) {
          found_skip = true;
          continue;
        }
        if (found_skip)
          return &obj;
      }
    }
    return nullptr;
  };

  // Iterate layers top-to-bottom (after the z-order fix, index n-1 = visually
  // on top, so walk highest-index first and pick the first hit).
  for (int li = (int)m_doc->layers.size() - 1; li >= 0; --li) {
    auto &layer = m_doc->layers[li];
    if (!layer->visible || layer->locked || layer->is_special_layer())
      continue;
    SceneNode *hit = test_children(layer->children, false);
    if (hit)
      return hit;
  }

  // Wrapped around — nothing else under the cursor, return nullptr
  return nullptr;
}

// ── Input: scroll
// ─────────────────────────────────────────────────────────────
bool Canvas::on_scroll(double dx, double dy) {
  if (!m_doc)
    return false;
  // Read ctrl from the scroll event itself — reliable regardless of canvas
  // focus
  bool ctrl = m_mod_ctrl;
  if (m_scroll_ctrl) {
    auto ev = m_scroll_ctrl->get_current_event();
    if (ev) {
      auto state = ev->get_modifier_state();
      ctrl = (state & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
    }
  }
  // Ctrl+scroll → zoom toward cursor, always
  if (ctrl) {
    zoom_toward(m_mouse_x, m_mouse_y, dy < 0 ? 1.1 : (1.0 / 1.1));
    return true;
  }
  // Scroll → pan (two-finger trackpad or scroll wheel)
  // dy is lines; multiply by a comfortable pixel step scaled by zoom
  constexpr double PAN_SPEED = 40.0;
  m_pan_x -= dx * PAN_SPEED;
  m_pan_y -= dy * PAN_SPEED;
  clamp_pan();
  queue_draw();
  return true;
}

// ── Input: middle-drag pan
// ────────────────────────────────────────────────────
void Canvas::on_pan_begin(double /*x*/, double /*y*/) {
  m_pan_drag_start_x = m_pan_x;
  m_pan_drag_start_y = m_pan_y;
}
void Canvas::on_pan_update(double dx, double dy) {
  m_pan_x = m_pan_drag_start_x + dx;
  m_pan_y = m_pan_drag_start_y + dy;
  clamp_pan();
  queue_draw();
}
void Canvas::on_pan_end(double /*dx*/, double /*dy*/) {}

// ── Input: left-drag — route by tool ─────────────────────────────────────────
void Canvas::on_draw_begin(double x, double y) {
  if (!m_doc)
    return;

  // Space+left-drag → pan regardless of active tool.
  // Capture pan origin and early-return — no tool action.
  if (m_space_held) {
    m_space_panning = true;
    m_space_pan_start_x = m_pan_x;
    m_space_pan_start_y = m_pan_y;
    // Reuse middle-drag fields so update/end arithmetic is identical.
    m_pan_drag_start_x = x;
    m_pan_drag_start_y = y;
    set_cursor("grabbing");
    return;
  }

  // ── Warp envelope drag (M4b) / pick (M4c-2) ─────────────────────────
  // Selection tool only. If primary selection is a Warp and the click
  // lands on an envelope anchor/handle, handle pick-set update then
  // snapshot pre-state and capture drag kind. Further routing (marquee,
  // object hit, etc.) is skipped for this press → release cycle.
  if (m_tool == ActiveTool::Selection && m_selected && m_selected->is_warp()) {
    sync_warp_env_picks_to_selection();
    WarpDragKind kind;
    bool is_top;
    int idx;
    if (hit_test_warp_envelope(x, y, *m_selected, kind, is_top, idx)) {
      // Map drag kind → pick-set part enum.
      EnvelopePart part =
          (kind == WarpDragKind::HandleIn)    ? EnvelopePart::HandleIn
          : (kind == WarpDragKind::HandleOut) ? EnvelopePart::HandleOut
                                              : EnvelopePart::Anchor;
      EnvelopePick pick{is_top, idx, part};
      // M4c-2: Shift+click → toggle pick in/out of set. No drag starts.
      if (m_mod_shift) {
        auto it =
            std::find(m_warp_env_picks.begin(), m_warp_env_picks.end(), pick);
        if (it != m_warp_env_picks.end())
          m_warp_env_picks.erase(it);
        else
          m_warp_env_picks.push_back(pick);
        m_warp_env_picks_owner = m_selected;
        queue_draw();
        return;
      }
      // M4c-2: Plain click. Two paths:
      //   (a) Hit is already in pick set with set size > 1 → start MULTI-
      //       drag. Pick set preserved, press point captured for delta.
      //   (b) Otherwise → replace pick set with just this hit, fall
      //       through to M4b single-drag setup.
      bool hit_in_set =
          (std::find(m_warp_env_picks.begin(), m_warp_env_picks.end(), pick) !=
           m_warp_env_picks.end());
      if (hit_in_set && m_warp_env_picks.size() > 1) {
        // Multi-drag path.
        m_warp_drag_is_multi = true;
        m_warp_drag_kind = kind; // anchor flag; not used for mutation
        m_warp_drag_is_top = is_top;
        m_warp_drag_idx = idx;
        m_warp_drag_pre_top = m_selected->warp_env_top;
        m_warp_drag_pre_bottom = m_selected->warp_env_bottom;
        m_warp_drag_pre_quality = m_selected->warp_quality;
        screen_to_doc(x, y, m_warp_drag_press_doc_x, m_warp_drag_press_doc_y);
        m_warp_drag_click_offset_x = 0.0;
        m_warp_drag_click_offset_y = 0.0;
        queue_draw();
        return;
      }
      // Single-drag path: replace set with just this element.
      m_warp_env_picks.clear();
      m_warp_env_picks.push_back(pick);
      m_warp_env_picks_owner = m_selected;
      m_warp_drag_is_multi = false;
      m_warp_drag_kind = kind;
      m_warp_drag_is_top = is_top;
      m_warp_drag_idx = idx;
      m_warp_drag_pre_top = m_selected->warp_env_top;
      m_warp_drag_pre_bottom = m_selected->warp_env_bottom;
      m_warp_drag_pre_quality = m_selected->warp_quality;
      // Compute click offset from the hit's doc-space center so first
      // motion doesn't jump. For anchors the offset is click-minus-
      // anchor; for handles it's click-minus-handle.
      double dx_doc, dy_doc;
      screen_to_doc(x, y, dx_doc, dy_doc);
      const PathData &env =
          is_top ? m_selected->warp_env_top : m_selected->warp_env_bottom;
      if (idx >= 0 && idx < (int)env.nodes.size()) {
        const BezierNode &n = env.nodes[idx];
        double target_x = n.x, target_y = n.y;
        if (kind == WarpDragKind::HandleIn) {
          target_x = n.cx1;
          target_y = n.cy1;
        }
        if (kind == WarpDragKind::HandleOut) {
          target_x = n.cx2;
          target_y = n.cy2;
        }
        m_warp_drag_click_offset_x = dx_doc - target_x;
        m_warp_drag_click_offset_y = dy_doc - target_y;
      }
      queue_draw();
      return;
    }
  }

  // SnR dialog pivot drag — pre-empts all tool logic while dialog is open.
  // Any click moves the refpt to that location and begins a drag; release
  // commits wherever the cursor is.  Works under any active tool (unlike
  // rotate-from-point which gates on Selection).
  if (m_sr_preview_active) {
    double dx, dy;
    screen_to_doc(x, y, dx, dy);
    m_sr_preview_x = dx;
    m_sr_preview_y = dy;
    m_sr_pivot_dragging = true;
    if (m_sr_pivot_change_cb)
      m_sr_pivot_change_cb(dx, dy);
    queue_draw();
    return;
  }

  // Guide construct mode — pre-empts tool logic.
  //   Phase 0: capture p1 (node-snap if within tolerance), advance to phase 1.
  //   Phase 1: capture p2 (node-snap), advance to phase 2, fire review cb.
  //   Phase 2: ignore clicks (dialog handles commit/cancel).
  if (m_guide_construct_active) {
    if (m_guide_construct_phase >= 2)
      return;
    double dx, dy;
    screen_to_doc(x, y, dx, dy);

    // Node snap — use the same helper the Ruler tool uses.
    const double tol = 8.0 / m_zoom;
    std::vector<std::pair<SceneNode *, int>> all_nodes;
    ruler_collect_all_path_nodes(all_nodes);
    double best_d = tol;
    for (auto &[obj, ni] : all_nodes) {
      const BezierNode &n = obj->path->nodes[ni];
      double d = std::hypot(n.x - dx, n.y - dy);
      if (d < best_d) {
        best_d = d;
        dx = n.x;
        dy = n.y;
      }
    }

    if (m_guide_construct_phase == 0) {
      m_guide_construct_p1_x = dx;
      m_guide_construct_p1_y = dy;
      m_guide_construct_preview_x = dx;
      m_guide_construct_preview_y = dy;
      m_guide_construct_phase = 1;
      queue_draw();
      return;
    }
    // Phase 1 → 2: capture p2 and hand control to the review dialog.
    m_guide_construct_p2_x = dx;
    m_guide_construct_p2_y = dy;
    m_guide_construct_phase = 2;
    queue_draw();
    if (m_guide_construct_review_cb)
      m_guide_construct_review_cb();
    return;
  }

  // R pivot mode + Selection tool: intercept clicks near the pivot crosshair
  // to move it. Clicks elsewhere fall through to normal handle/selection logic
  // so the rotate handles work as usual.
  if (m_r_held && m_tool == ActiveTool::Selection && !m_selection.empty()) {
    if (m_mod_ctrl) {
      // Ctrl+click anywhere → exact position dialog
      double dx, dy;
      screen_to_doc(x, y, dx, dy);
      on_pivot_dialog(dx, dy);
      return;
    }
    if (m_has_custom_pivot) {
      // Only grab the pivot if click is within ~12px of the crosshair
      double pvsx, pvsy;
      doc_to_screen(m_custom_pivot_x, m_custom_pivot_y, pvsx, pvsy);
      double dist = std::hypot(x - pvsx, y - pvsy);
      if (dist <= 12.0) {
        // Start pivot drag
        m_pivot_dragging = true;
        queue_draw();
        return;
      }
      // Not near pivot — fall through to normal handle/selection logic
    } else {
      // No pivot set yet — click sets it
      double dx, dy;
      screen_to_doc(x, y, dx, dy);
      m_custom_pivot_x = snap(dx);
      m_custom_pivot_y = snap(dy);
      m_has_custom_pivot = true;
      m_pivot_dragging = true;
      queue_draw();
      return;
    }
  }

  double dx, dy;
  screen_to_doc(x, y, dx, dy);

  if (m_tool == ActiveTool::Rect || m_tool == ActiveTool::Ellipse ||
      m_tool == ActiveTool::Polygon || m_tool == ActiveTool::Spiral) {
    m_drawing = true;
    m_draw_start_dx = snap(dx);
    m_draw_start_dy = snap(dy);
    m_draw_cur_dx = m_draw_start_dx;
    m_draw_cur_dy = m_draw_start_dy;
    m_draw_start_effective_dx = m_draw_start_dx;
    m_draw_start_effective_dy = m_draw_start_dy;
    m_poly_drag_angle = -M_PI * 0.5; // point-up default
    LOG_DEBUG("Draw begin at ({:.2f},{:.2f})", m_draw_start_dx,
              m_draw_start_dy);
  } else if (m_tool == ActiveTool::Line) {
    double px = snap(dx), py = snap(dy);
    // Apply 15° angle restriction when Shift held.
    // Compute fresh from the raw position so this works even if the motion
    // handler hasn't run between clicks (rapid clicking).
    if (m_mod_shift && m_line_tool.active()) {
      auto [lx, ly] = m_line_tool.points.back();
      double dw = px - lx, dh = py - ly;
      double len = std::hypot(dw, dh);
      if (len > 0.001) {
        double angle = std::atan2(dh, dw);
        double snapped = std::round(angle / (M_PI / 12.0)) * (M_PI / 12.0);
        px = lx + len * std::cos(snapped);
        py = ly + len * std::sin(snapped);
      }
    }
    // Close snap: if near start, snap to it
    if (m_line_tool.active()) {
      double tol = 8.0 / m_zoom;
      auto [sx, sy] = m_line_tool.points[0];
      if (std::hypot(px - sx, py - sy) <= tol) {
        px = sx;
        py = sy;
        m_line_tool.close_snap = true;
      }
    }
    if (m_line_tool.close_snap && m_line_tool.points.size() >= 2) {
      // Closing click — commit closed path
      commit_line_path();
    } else {
      m_line_tool.points.push_back({px, py});
      m_line_tool.live_x = px;
      m_line_tool.live_y = py;
      LOG_DEBUG("LineTool: placed point ({:.2f},{:.2f}), total={}", px, py,
                m_line_tool.points.size());
    }
    queue_draw();
    return;
  } else if (m_tool == ActiveTool::Ref) {
    double px = snap(dx), py = snap(dy);
    // Hit test existing ref points in the RefLayer
    double tol = 8.0 / m_zoom;
    m_ref_selected = nullptr;
    SceneNode *rl = m_doc->ref_layer();
    if (rl && !rl->locked) {
      for (auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        if (std::hypot(px - child->ref_x, py - child->ref_y) <= tol) {
          m_ref_selected = child.get();
          m_ref_drag_ox = px - child->ref_x;
          m_ref_drag_oy = py - child->ref_y;
          break;
        }
      }
    }
    m_drawing = true;
    m_draw_start_dx = px;
    m_draw_start_dy = py;
    m_draw_cur_dx = px;
    m_draw_cur_dy = py;
    queue_draw();
    return;
  } else if (m_tool == ActiveTool::Node) {
    on_node_begin(x, y);
  } else if (m_tool == ActiveTool::Ruler) {
    on_ruler_begin(x, y);
  } else if (m_tool == ActiveTool::TextOnPath) {
    on_top_begin(x, y);
  } else if (m_tool == ActiveTool::Corner) {
    on_corner_begin(x, y);
  } else if (m_tool == ActiveTool::Pen) {
    on_pen_begin(x, y);
  } else if (m_tool == ActiveTool::Selection) {
    on_select_begin(x, y);
  } else if (m_tool == ActiveTool::Zoom) {
    // Store screen-space anchor for marquee or click zoom
    m_zoom_anchor_x = x;
    m_zoom_anchor_y = y;
    m_drawing = true;  // reuse drawing flag to track drag
    m_draw_cur_dx = x; // repurpose as screen-space current end (not snapped)
    m_draw_cur_dy = y;
  } else if (m_tool == ActiveTool::Eyedropper) {
    // S66 — Phase 3 always-zoom loupe. Every click commits the centre
    // pixel of the magnified buffer sample (the pixel under the crosshair
    // in the loupe). Alt still controls the destination channel:
    //   Alt    -> apply to stroke.
    //   No Alt -> apply to fill.
    // Hit-test fallback retained only for the edge case where the buffer
    // isn't fresh yet (first activation with no hover tick).
    bool sampled_ok = false;
    FillStyle sampled;
    bool to_stroke = m_mod_alt;
    if (m_loupe_buffer_valid) {
      sampled.type = FillStyle::Type::Solid;
      sampled.r = m_loupe_buffer_r;
      sampled.g = m_loupe_buffer_g;
      sampled.b = m_loupe_buffer_b;
      sampled.a = m_loupe_buffer_a;
      sampled_ok = true;
    } else {
      SceneNode *hit = hit_test(dx, dy);
      if (hit) {
        sampled = m_mod_alt ? hit->stroke.paint : hit->fill;
        sampled_ok = true;
      }
    }

    if (sampled_ok) {
      // Apply to every selected object (undoable).
      for (SceneNode *obj : m_selection) {
        FillStyle fb = obj->fill;
        StrokeStyle sb = obj->stroke;
        FillStyle fa = fb;
        StrokeStyle sa = sb;
        if (to_stroke)
          sa.paint = sampled;
        else
          fa = sampled;
        // S82 m4f: capture pre-edit swatch ids; mutate via funnel which
        // clears them as a break-on-override side effect; then read post-
        // mutation ids for the after snapshot. Reordered from push-then-
        // mutate to mutate-then-push so the after snapshot is read from
        // the actual post-funnel state (matches the PropertiesPanel
        // broadcast pattern).
        // S92 m3: same shape for bound_style.
        std::string fsib = obj->fill_swatch_id;
        std::string ssib = obj->stroke_swatch_id;
        std::string bsb  = obj->bound_style;
        // Eyedropper is a user override — route through the funnel so any
        // Style/Swatch binding on this object is broken per addendum
        // invariant.
        style::mutate_appearance(*obj, [&](SceneNode& n) {
          n.fill = fa;
          n.stroke = sa;
        });
        std::string fsia = obj->fill_swatch_id;
        std::string ssia = obj->stroke_swatch_id;
        std::string bsa  = obj->bound_style;
        if (m_history)
          m_history->push(std::make_unique<EditAppearanceCommand>(
              obj, fb, sb, fa, sa,
              std::move(fsib), std::move(ssib),
              std::move(fsia), std::move(ssia),
              std::move(bsb), std::move(bsa),
              "Eyedropper"));
      }

      // Notify MainWindow so it can sync the toolbar well.
      m_sig_eyedropper_pick.emit(sampled, to_stroke);

      if (!m_selection.empty()) {
        m_sig_doc_changed.emit();
        queue_draw();
      }
    }
    // Restore the previous tool (like Illustrator — one-shot pick).
    m_sig_request_tool.emit(m_prev_tool);
  } else if (m_tool == ActiveTool::Text) {
    on_text_begin(x, y);
  }
}

void Canvas::on_draw_update(double delta_x, double delta_y) {
  if (!m_doc)
    return;

  // Space pan — intercept before tool routing.
  if (m_space_panning) {
    m_pan_x = m_space_pan_start_x + (m_mouse_x - m_pan_drag_start_x);
    m_pan_y = m_space_pan_start_y + (m_mouse_y - m_pan_drag_start_y);
    clamp_pan();
    queue_draw();
    return;
  }

  // ── Warp envelope MULTI-drag (M4c-2c) ──────────────────────────────
  // Translate every element in m_warp_env_picks by cursor delta from
  // press point. Anchor picks carry both handles; standalone HandleIn/
  // HandleOut picks translate only the corresponding handle. If an
  // anchor's Anchor part is picked AND its handles are separately
  // picked, the handles are carried by the anchor — skip them to avoid
  // double-translation. Writes from snapshot + delta each motion event
  // to prevent accumulation.
  if (m_warp_drag_is_multi && m_selected && m_selected->is_warp()) {
    double mx_doc, my_doc;
    screen_to_doc(m_mouse_x, m_mouse_y, mx_doc, my_doc);
    double dx = mx_doc - m_warp_drag_press_doc_x;
    double dy = my_doc - m_warp_drag_press_doc_y;
    // Start from pre-drag snapshot — prevents cumulative error.
    m_selected->warp_env_top = m_warp_drag_pre_top;
    m_selected->warp_env_bottom = m_warp_drag_pre_bottom;
    // Flat independence: each picked element translates by the same
    // delta. Anchors translate x/y only (do NOT carry handles —
    // handles are independent unless separately picked). Handles
    // translate their own component.
    for (const auto &p : m_warp_env_picks) {
      PathData &env =
          p.is_top ? m_selected->warp_env_top : m_selected->warp_env_bottom;
      if (p.idx < 0 || p.idx >= (int)env.nodes.size())
        continue;
      BezierNode &n = env.nodes[p.idx];
      if (p.part == EnvelopePart::Anchor) {
        n.x += dx;
        n.y += dy;
        // Carry along any handle that was coincident with the anchor
        // in the pre-snapshot. A coincident handle has no independent
        // visual identity — stranding it at the old anchor position
        // produces visible "ghost dots" after the drag. Separated
        // handles are left alone (flat independence). Threshold
        // matches the one used by select_all for visibility gating.
        const PathData &pre =
            p.is_top ? m_warp_drag_pre_top : m_warp_drag_pre_bottom;
        if (p.idx < (int)pre.nodes.size()) {
          const BezierNode &prn = pre.nodes[p.idx];
          if (std::hypot(prn.cx1 - prn.x, prn.cy1 - prn.y) <= 1e-6) {
            n.cx1 += dx;
            n.cy1 += dy;
          }
          if (std::hypot(prn.cx2 - prn.x, prn.cy2 - prn.y) <= 1e-6) {
            n.cx2 += dx;
            n.cy2 += dy;
          }
        }
      } else if (p.part == EnvelopePart::HandleIn) {
        n.cx1 += dx;
        n.cy1 += dy;
      } else if (p.part == EnvelopePart::HandleOut) {
        n.cx2 += dx;
        n.cy2 += dy;
      }
    }
    m_selected->warp_cache_dirty = true;
    queue_draw();
    return;
  }

  // ── Warp envelope drag (M4b) ────────────────────────────────────────
  // Live writethrough to the envelope PathData. Anchor drags move the
  // anchor AND translate both handles by the same delta (so the tangent
  // shape is preserved). Handle drags move one handle independently by
  // default (M4c-2 revision: handles are independent from anchors and
  // from each other unless explicitly locked). Shift-drag re-enables
  // mirror-across-anchor for Smooth tangent continuity.
  if (m_warp_drag_kind != WarpDragKind::None && m_selected &&
      m_selected->is_warp()) {
    PathData &env = m_warp_drag_is_top ? m_selected->warp_env_top
                                       : m_selected->warp_env_bottom;
    if (m_warp_drag_idx < 0 || m_warp_drag_idx >= (int)env.nodes.size()) {
      // Drag target went away (undo mid-drag? shouldn't happen) — abort.
      m_warp_drag_kind = WarpDragKind::None;
      return;
    }
    BezierNode &n = env.nodes[m_warp_drag_idx];
    double mx_doc, my_doc;
    screen_to_doc(m_mouse_x, m_mouse_y, mx_doc, my_doc);
    double target_x = mx_doc - m_warp_drag_click_offset_x;
    double target_y = my_doc - m_warp_drag_click_offset_y;
    switch (m_warp_drag_kind) {
    case WarpDragKind::Anchor: {
      double dx = target_x - n.x;
      double dy = target_y - n.y;
      n.x += dx;
      n.y += dy;
      // Separated handles stay independent (flat independence). Coincident
      // handles (invisibly hiding under the anchor) ride along so they
      // don't get stranded when the anchor moves. Detected from pre-press
      // snapshot so jitter on live drag doesn't falsely separate them.
      const PathData &pre =
          m_warp_drag_is_top ? m_warp_drag_pre_top : m_warp_drag_pre_bottom;
      if (m_warp_drag_idx < (int)pre.nodes.size()) {
        const BezierNode &prn = pre.nodes[m_warp_drag_idx];
        if (std::hypot(prn.cx1 - prn.x, prn.cy1 - prn.y) <= 1e-6) {
          n.cx1 += dx;
          n.cy1 += dy;
        }
        if (std::hypot(prn.cx2 - prn.x, prn.cy2 - prn.y) <= 1e-6) {
          n.cx2 += dx;
          n.cy2 += dy;
        }
      }
      break;
    }
    case WarpDragKind::HandleIn: {
      n.cx1 = target_x;
      n.cy1 = target_y;
      if (m_mod_shift) {
        // Shift-drag: mirror cx2/cy2 across the anchor. Length matches
        // original cx2 distance, direction mirrors cx1 vector — classic
        // Smooth tangent continuity, opt-in.
        double vx = n.x - n.cx1;
        double vy = n.y - n.cy1;
        double v_len = std::hypot(vx, vy);
        double orig_cx2_dx = n.cx2 - n.x;
        double orig_cx2_dy = n.cy2 - n.y;
        double orig_len = std::hypot(orig_cx2_dx, orig_cx2_dy);
        if (v_len > 1e-9) {
          double scale = orig_len / v_len;
          n.cx2 = n.x + vx * scale;
          n.cy2 = n.y + vy * scale;
        }
      }
      break;
    }
    case WarpDragKind::HandleOut: {
      n.cx2 = target_x;
      n.cy2 = target_y;
      if (m_mod_shift) {
        double vx = n.x - n.cx2;
        double vy = n.y - n.cy2;
        double v_len = std::hypot(vx, vy);
        double orig_cx1_dx = n.cx1 - n.x;
        double orig_cx1_dy = n.cy1 - n.y;
        double orig_len = std::hypot(orig_cx1_dx, orig_cx1_dy);
        if (v_len > 1e-9) {
          double scale = orig_len / v_len;
          n.cx1 = n.x + vx * scale;
          n.cy1 = n.y + vy * scale;
        }
      }
      break;
    }
    case WarpDragKind::None:
      break;
    }
    m_selected->warp_cache_dirty = true;
    queue_draw();
    return;
  }

  // Pivot drag — update custom pivot position.
  if (m_pivot_dragging) {
    double dx, dy;
    screen_to_doc(m_mouse_x, m_mouse_y, dx, dy);
    m_custom_pivot_x = snap(dx);
    m_custom_pivot_y = snap(dy);
    queue_draw();
    return;
  }

  // SnR pivot drag — update SR preview pivot + fire callback to dialog.
  if (m_sr_pivot_dragging) {
    double dx, dy;
    screen_to_doc(m_mouse_x, m_mouse_y, dx, dy);
    m_sr_preview_x = dx;
    m_sr_preview_y = dy;
    if (m_sr_pivot_change_cb)
      m_sr_pivot_change_cb(dx, dy);
    queue_draw();
    return;
  }

  if (m_drawing && m_tool == ActiveTool::Zoom) {
    // Track current mouse in screen space for marquee preview
    m_draw_cur_dx = m_mouse_x;
    m_draw_cur_dy = m_mouse_y;
    queue_draw();
  } else if (m_drawing) {
    double ex, ey;
    screen_to_doc(m_mouse_x, m_mouse_y, ex, ey);
    m_draw_cur_dx = snap(ex);
    m_draw_cur_dy = snap(ey);
    // Shift/Alt constrain for Rect and Ellipse:
    //   Shift      — square (equal W and H)
    //   Alt        — draw from center (start point is center)
    //   Shift+Alt  — square from center
    if (m_tool == ActiveTool::Rect || m_tool == ActiveTool::Ellipse) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      if (m_mod_shift) {
        double sz = std::max(std::abs(dw), std::abs(dh));
        dw = std::copysign(sz, dw);
        dh = std::copysign(sz, dh);
        m_draw_cur_dx = m_draw_start_dx + dw;
        m_draw_cur_dy = m_draw_start_dy + dh;
      }
      if (m_mod_alt) {
        // Mirror cur through start — start becomes center
        m_draw_start_effective_dx =
            m_draw_start_dx - (m_draw_cur_dx - m_draw_start_dx);
        m_draw_start_effective_dy =
            m_draw_start_dy - (m_draw_cur_dy - m_draw_start_dy);
      } else {
        m_draw_start_effective_dx = m_draw_start_dx;
        m_draw_start_effective_dy = m_draw_start_dy;
      }
    }

    // ── Polygon update ─────────────────────────────────────────────────
    // Alt  = draw from center (start is center, radius = distance to cur)
    // Shift = snap rotation to nearest 15°
    if (m_tool == ActiveTool::Polygon) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      // Compute angle from start to current for rotation
      double angle = std::atan2(dh, dw);
      if (m_mod_shift) {
        // Snap to nearest 15°
        double step = M_PI / 12.0;
        angle = std::round(angle / step) * step;
      }
      m_poly_drag_angle = angle - M_PI * 0.5; // offset so point is up at 0°
      if (m_mod_alt) {
        // Center = start, radius = distance to cur
        m_draw_start_effective_dx = m_draw_start_dx;
        m_draw_start_effective_dy = m_draw_start_dy;
      } else {
        // Center = midpoint of drag
        m_draw_start_effective_dx = m_draw_start_dx;
        m_draw_start_effective_dy = m_draw_start_dy;
      }
    }
    // ── Spiral update ──────────────────────────────────────────────────
    // Same drag convention as Polygon: drag distance = outer radius,
    // drag direction = rotation angle of outer tip.
    // Shift = snap rotation to 15°
    if (m_tool == ActiveTool::Spiral) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      double angle = std::atan2(dh, dw);
      if (m_mod_shift) {
        double step = M_PI / 12.0;
        angle = std::round(angle / step) * step;
      }
      m_spiral_drag_angle = angle - M_PI * 0.5; // tip points up at 0°
    }

    // Shift constrain for Line: snap angle to nearest 45°
    if (m_tool == ActiveTool::Line && m_mod_shift) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      double len = std::hypot(dw, dh);
      if (len > 0.001) {
        double angle = std::atan2(dh, dw);
        double snapped = std::round(angle / (M_PI / 12.0)) * (M_PI / 12.0);
        m_draw_cur_dx = m_draw_start_dx + len * std::cos(snapped);
        m_draw_cur_dy = m_draw_start_dy + len * std::sin(snapped);
      }
    }
    // Ref tool: drag moves an existing ref point
    if (m_tool == ActiveTool::Ref && m_ref_selected) {
      double ex, ey;
      screen_to_doc(m_mouse_x, m_mouse_y, ex, ey);
      m_ref_selected->ref_x = snap(ex) - m_ref_drag_ox;
      m_ref_selected->ref_y = snap(ey) - m_ref_drag_oy;
      char name_buf[64];
      snprintf(name_buf, sizeof(name_buf), "%.6f_%.6f", m_ref_selected->ref_x,
               m_ref_selected->ref_y);
      m_ref_selected->name = name_buf;
      m_sig_doc_changed.emit();
    }
    // Corner tool: update rubber-band endpoint
    if (m_tool == ActiveTool::Corner && m_corner_rubber_active) {
      m_corner_rubber_x1 = m_mouse_x;
      m_corner_rubber_y1 = m_mouse_y;
    }
    queue_draw();
  } else if (m_tool == ActiveTool::Pen) {
    double ddx, ddy;
    screen_to_doc(m_mouse_x, m_mouse_y, ddx, ddy);
    m_pen_tool.mods.alt = m_mod_alt;
    m_pen_tool.mods.shift = m_mod_shift;
    m_pen_tool.on_drag({ddx, ddy});
    queue_draw();
  } else if (m_tool == ActiveTool::Line && m_line_tool.active()) {
    double ex, ey;
    screen_to_doc(m_mouse_x, m_mouse_y, ex, ey);
    ex = snap(ex);
    ey = snap(ey);
    // 15° angle snap when Shift held (24 increments around the circle)
    if (m_mod_shift) {
      auto [lx, ly] = m_line_tool.points.back();
      double dw = ex - lx, dh = ey - ly;
      double len = std::hypot(dw, dh);
      if (len > 0.001) {
        double angle = std::atan2(dh, dw);
        double snapped = std::round(angle / (M_PI / 12.0)) * (M_PI / 12.0);
        ex = lx + len * std::cos(snapped);
        ey = ly + len * std::sin(snapped);
      }
    }
    // Close snap check
    double tol = 8.0 / m_zoom;
    auto [sx, sy] = m_line_tool.points[0];
    m_line_tool.close_snap = (std::hypot(ex - sx, ey - sy) <= tol);
    if (m_line_tool.close_snap) {
      ex = sx;
      ey = sy;
    }
    m_line_tool.live_x = ex;
    m_line_tool.live_y = ey;
    queue_draw();
  } else if (m_tool == ActiveTool::Node) {
    on_node_update(delta_x, delta_y);
  } else if (m_tool == ActiveTool::Ruler) {
    on_ruler_motion(m_draw_cur_dx, m_draw_cur_dy);
  } else if (m_tool == ActiveTool::TextOnPath) {
    // TOP drag does not set m_drawing, so m_draw_cur_dx/dy are never
    // updated in the m_drawing branch above. Convert mouse position fresh.
    double tdx, tdy;
    screen_to_doc(m_mouse_x, m_mouse_y, tdx, tdy);
    on_top_motion(tdx, tdy);
  } else if (m_tool == ActiveTool::Corner) {
    on_corner_motion(m_draw_cur_dx, m_draw_cur_dy);
  } else if (m_tool == ActiveTool::Selection) {
    on_select_update(delta_x, delta_y);
  }
}

void Canvas::on_draw_end(double delta_x, double delta_y) {
  if (!m_doc)
    return;

  // Space pan — finish without any tool action.
  if (m_space_panning) {
    m_space_panning = false;
    // Restore grab cursor (space still held) or the tool default.
    set_cursor(m_space_held ? "grab" : "default");
    return;
  }

  // ── Warp envelope drag commit (M4b single / M4c-2c multi) ──────────
  // Push EditWarpCommand with the captured pre-state and current
  // post-state. One command per drag — successive drags are distinct
  // undo steps. No time-coalescing; could add later.
  //
  // Multi-drag edge case: if the user clicked on a pick-set member but
  // didn't actually drag, the envelope is unchanged. Don't push a no-op
  // undo; instead collapse the pick set to just that element (matches
  // Illustrator/Affinity muscle memory: click-without-drag on a selected
  // item replaces selection with just that one).
  if (m_warp_drag_kind != WarpDragKind::None && m_selected &&
      m_selected->is_warp()) {
    // Detect no-motion by comparing envelopes to pre-snapshot.
    auto env_equal = [](const PathData &a, const PathData &b) {
      if (a.nodes.size() != b.nodes.size())
        return false;
      for (size_t i = 0; i < a.nodes.size(); ++i) {
        const auto &n1 = a.nodes[i];
        const auto &n2 = b.nodes[i];
        if (n1.x != n2.x || n1.y != n2.y || n1.cx1 != n2.cx1 ||
            n1.cy1 != n2.cy1 || n1.cx2 != n2.cx2 || n1.cy2 != n2.cy2)
          return false;
      }
      return true;
    };
    bool no_motion =
        env_equal(m_selected->warp_env_top, m_warp_drag_pre_top) &&
        env_equal(m_selected->warp_env_bottom, m_warp_drag_pre_bottom);
    if (m_warp_drag_is_multi && no_motion) {
      // Click-without-drag on a pick-set member → collapse set to just this.
      EnvelopePart part = (m_warp_drag_kind == WarpDragKind::HandleIn)
                              ? EnvelopePart::HandleIn
                          : (m_warp_drag_kind == WarpDragKind::HandleOut)
                              ? EnvelopePart::HandleOut
                              : EnvelopePart::Anchor;
      m_warp_env_picks.clear();
      m_warp_env_picks.push_back({m_warp_drag_is_top, m_warp_drag_idx, part});
      m_warp_env_picks_owner = m_selected;
    } else if (!no_motion) {
      if (m_history) {
        m_history->push(std::make_unique<EditWarpCommand>(
            m_selected, m_warp_drag_pre_top, m_warp_drag_pre_bottom,
            m_warp_drag_pre_quality, m_selected->warp_env_top,
            m_selected->warp_env_bottom, m_selected->warp_quality));
      }
    }
    m_warp_drag_kind = WarpDragKind::None;
    m_warp_drag_idx = -1;
    m_warp_drag_is_multi = false;
    m_warp_drag_press_doc_x = 0.0;
    m_warp_drag_press_doc_y = 0.0;
    queue_draw();
    return;
  }

  // Pivot drag — finish; pivot is already set in m_custom_pivot_x/y.
  if (m_pivot_dragging) {
    m_pivot_dragging = false;
    set_cursor("crosshair"); // R still held
    queue_draw();
    return;
  }

  // SnR pivot drag — finish; SR preview already live.
  if (m_sr_pivot_dragging) {
    m_sr_pivot_dragging = false;
    queue_draw();
    return;
  }

  if (m_drawing && m_tool == ActiveTool::Zoom) {
    m_drawing = false;
    double ex = m_draw_cur_dx; // screen-space end (stored in draw_cur)
    double ey = m_draw_cur_dy;
    double dist = std::hypot(ex - m_zoom_anchor_x, ey - m_zoom_anchor_y);
    if (dist < 5.0) {
      // Ctrl+click — fit canvas to window
      if (m_mod_ctrl) {
        zoom_fit();
      } else {
        // Click — zoom in 2× (or out with Alt)
        double factor = m_mod_alt ? 0.5 : 2.0;
        zoom_toward(m_zoom_anchor_x, m_zoom_anchor_y, factor);
      }
    } else {
      // Marquee drag
      if (m_mod_alt) {
        // Alt+drag — zoom OUT: the marquee represents the area of the
        // screen into which the current canvas view should be shrunk.
        // Equivalent to zooming out by the ratio of viewport to marquee.
        double rw = std::abs(ex - m_zoom_anchor_x);
        double rh = std::abs(ey - m_zoom_anchor_y);
        double vw = static_cast<double>(get_width());
        double vh = static_cast<double>(get_height());
        if (rw > 4 && rh > 4) {
          double factor = std::min(rw / vw, rh / vh);
          double cx = (m_zoom_anchor_x + ex) * 0.5;
          double cy = (m_zoom_anchor_y + ey) * 0.5;
          zoom_toward(cx, cy, factor);
        }
      } else {
        // Normal drag — zoom IN to the marquee rect
        zoom_to_rect(m_zoom_anchor_x, m_zoom_anchor_y, ex, ey);
      }
    }
    queue_draw();
    return;
  }

  if (m_drawing && m_tool == ActiveTool::Line) {
    // Line tool no longer uses m_drawing — this branch is dead but kept for
    // safety
    m_drawing = false;
    queue_draw();
    return;
  }

  // Corner tool — end rubber-band or click, never commits a shape
  if (m_drawing && m_tool == ActiveTool::Corner) {
    m_drawing = false;
    on_corner_end(m_draw_cur_dx, m_draw_cur_dy);
    return;
  }

  // Ref tool: mouse-up — place new point or finish drag
  if (m_drawing && m_tool == ActiveTool::Ref) {
    m_drawing = false;
    double dx2, dy2;
    screen_to_doc(m_mouse_x, m_mouse_y, dx2, dy2);
    double px = snap(dx2), py = snap(dy2);

    if (m_ref_selected) {
      // Finished dragging existing ref point
      m_ref_selected = nullptr;
      m_sig_doc_changed.emit();
    } else {
      // No existing ref hit — place new ref point on mouse-up
      double tol = 3.0 / m_zoom;
      bool is_click =
          (std::hypot(px - m_draw_start_dx, py - m_draw_start_dy) < tol);
      if (is_click && m_doc) {
        SceneNode *rl = m_doc->ensure_ref_layer();

        double rx = m_draw_start_dx, ry = m_draw_start_dy;
        char name_buf[64];
        snprintf(name_buf, sizeof(name_buf), "%.6f_%.6f", rx, ry);

        auto ref = std::make_unique<SceneNode>();
        ref->type = SceneNode::Type::Ref;
        ref->id = next_id();
        ref->name = name_buf;
        ref->ref_x = rx;
        ref->ref_y = ry;

        rl->children.insert(rl->children.begin(), clone_node(*ref));
        m_selected = rl->children.front().get();

        if (m_history)
          m_history->push(std::make_unique<AddNodeCommand>(
              rl, clone_node(*rl->children.front())));

        m_sig_selection.emit(m_selected);
        m_sig_doc_changed.emit();
        LOG_INFO("Ref point placed at ({:.4f},{:.4f})", rx, ry);
      }
    }
    queue_draw();
    return;
  }

  // Line tool double-click detection: on_draw_begin already placed a new point,
  // so back() is the just-placed point. A true double-click means the cursor is
  // near the PREVIOUS point (second-to-last), i.e. the user clicked the same
  // spot twice. We pop the duplicate and commit.
  if (m_tool == ActiveTool::Line && m_line_tool.points.size() >= 3) {
    double dx2, dy2;
    screen_to_doc(m_mouse_x, m_mouse_y, dx2, dy2);
    // Compare against second-to-last (the point placed on the previous click)
    auto [lx, ly] = m_line_tool.points[m_line_tool.points.size() - 2];
    double tol = 6.0 / m_zoom;
    if (std::hypot(dx2 - lx, dy2 - ly) <= tol) {
      // Double-click — remove the duplicate last point and commit open
      m_line_tool.points.pop_back();
      commit_line_path();
      return;
    }
  }

  if (m_drawing) {
    m_drawing = false;

    // Re-apply Shift/Alt constrains at commit (mirrors on_draw_update)
    if (m_tool == ActiveTool::Rect || m_tool == ActiveTool::Ellipse) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      if (m_mod_shift) {
        double sz = std::max(std::abs(dw), std::abs(dh));
        dw = std::copysign(sz, dw);
        dh = std::copysign(sz, dh);
        m_draw_cur_dx = m_draw_start_dx + dw;
        m_draw_cur_dy = m_draw_start_dy + dh;
      }
      if (m_mod_alt) {
        m_draw_start_effective_dx =
            m_draw_start_dx - (m_draw_cur_dx - m_draw_start_dx);
        m_draw_start_effective_dy =
            m_draw_start_dy - (m_draw_cur_dy - m_draw_start_dy);
      } else {
        m_draw_start_effective_dx = m_draw_start_dx;
        m_draw_start_effective_dy = m_draw_start_dy;
      }
    }

    // ── Polygon commit ─────────────────────────────────────────────────
    if (m_tool == ActiveTool::Polygon) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      double radius = std::hypot(dw, dh);
      if (radius < 2.0) {
        queue_draw();
        return;
      }

      // Center: Alt = start is center, else start is edge of bounding circle
      double cx, cy;
      if (m_mod_alt) {
        cx = m_draw_start_dx;
        cy = m_draw_start_dy;
      } else {
        // Center is midpoint — drag defines radius and direction
        cx = m_draw_start_dx;
        cy = m_draw_start_dy;
      }

      int sides = m_poly_sides;
      double inflection = m_poly_inflection;

      // Snap inflection to perfect star ratio cos(π/sides)
      double perfect_star =
          (sides >= 5) ? std::cos(2.0 * M_PI / sides) / std::cos(M_PI / sides)
                       : -1.0;
      if (perfect_star > 0.0 && std::abs(inflection - perfect_star) < 0.04)
        inflection = perfect_star;
      // Snap to full polygon
      if (inflection > 0.985)
        inflection = 1.0;

      SceneNode obj;
      obj.id = next_id();
      obj.internal_id = last_iid();
      obj.name = (inflection >= 1.0)
                     ? m_doc->next_default_name(CurvzDocument::NameKind::Polygon)
                     : m_doc->next_default_name(CurvzDocument::NameKind::Star);
      obj.type = SceneNode::Type::Path;
      obj.path = std::make_unique<PathData>(polygon_to_path(
          cx, cy, radius, sides, inflection, m_poly_drag_angle));
      style::mutate_appearance(obj, [this](SceneNode& n) {
        n.fill = m_def_fill;
        n.stroke = m_def_stroke;
      });

      if (!m_doc->layers.empty()) {
        SceneNode *layer = m_doc->active_layer();
        if (!layer)
          layer = m_doc->layers[0].get();
        layer->children.insert(layer->children.begin(), clone_node(obj));
        m_selected = layer->children.front().get();
        if (m_history) {
          auto cmd = std::make_unique<AddNodeCommand>(layer, clone_node(obj));
          m_history->push(std::move(cmd));
        }
      }
      m_selection.clear();
      if (m_selected)
        m_selection.push_back(m_selected);
      m_sig_selection.emit(m_selected);
      m_sig_request_tool.emit(ActiveTool::Selection);
      m_sig_doc_changed.emit();
      queue_draw();
      LOG_INFO("Polygon placed: cx={:.1f} cy={:.1f} r={:.1f} sides={} "
               "inflect={:.3f}",
               cx, cy, radius, sides, inflection);
      return;
    }

    // ── Spiral commit ──────────────────────────────────────────────────
    if (m_tool == ActiveTool::Spiral) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      double outer_r = std::hypot(dw, dh);
      if (outer_r < 2.0) {
        queue_draw();
        return;
      }

      double cx = m_draw_start_dx;
      double cy = m_draw_start_dy;
      double inner_r = outer_r * (m_spiral_inner / 100.0);

      SceneNode obj;
      obj.id = next_id();
      obj.internal_id = last_iid();
      obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Spiral);
      obj.type = SceneNode::Type::Path;
      obj.path = std::make_unique<PathData>(
          spiral_to_path(cx, cy, outer_r, inner_r, m_spiral_turns,
                         m_spiral_drag_angle));
      style::mutate_appearance(obj, [this](SceneNode& n) {
        n.fill = m_def_fill;
        n.stroke = m_def_stroke;
      });

      SceneNode *layer = m_doc->active_layer();
      if (!layer)
        layer = m_doc->layers[0].get();
      layer->children.insert(layer->children.begin(), clone_node(obj));
      m_selected = layer->children.front().get();
      if (m_history)
        m_history->push(
            std::make_unique<AddNodeCommand>(layer, clone_node(obj)));

      m_selection.clear();
      if (m_selected)
        m_selection.push_back(m_selected);
      m_sig_selection.emit(m_selected);
      m_sig_request_tool.emit(ActiveTool::Selection);
      m_sig_doc_changed.emit();
      queue_draw();
      LOG_INFO(
          "Spiral placed: cx={:.1f} cy={:.1f} r={:.1f} turns={:.1f}",
          cx, cy, outer_r, m_spiral_turns);
      return;
    }

    double x1 = std::min(m_draw_start_effective_dx, m_draw_cur_dx);
    double y1 = std::min(m_draw_start_effective_dy, m_draw_cur_dy);
    double x2 = std::max(m_draw_start_effective_dx, m_draw_cur_dx);
    double y2 = std::max(m_draw_start_effective_dy, m_draw_cur_dy);
    double w = x2 - x1;
    double h = y2 - y1;

    // Minimum 1px in document space
    if (w < 0.5 || h < 0.5) {
      queue_draw();
      return;
    }

    SceneNode obj;
    obj.id = next_id();
    obj.internal_id = last_iid();

    obj.type = SceneNode::Type::Path;
    if (m_tool == ActiveTool::Rect) {
      obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Rectangle);
      obj.path = std::make_unique<PathData>(rect_to_path(x1, y1, w, h));
      LOG_INFO("Rect placed: ({:.1f},{:.1f}) {:.1f}x{:.1f}", x1, y1, w, h);
    } else {
      obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Ellipse);
      double ecx = x1 + w * 0.5, ecy = y1 + h * 0.5;
      obj.path = std::make_unique<PathData>(
          ellipse_to_path(ecx, ecy, w * 0.5, h * 0.5));
      LOG_INFO("Ellipse placed: cx={:.1f} cy={:.1f} rx={:.1f} ry={:.1f}", ecx,
               ecy, w * 0.5, h * 0.5);
    }

    // Apply project-wide defaults from ContextBar
    style::mutate_appearance(obj, [this](SceneNode& n) {
      n.fill = m_def_fill;
      n.stroke = m_def_stroke;
    });

    // Add to active layer via command
    if (!m_doc->layers.empty()) {
      SceneNode *layer = m_doc->active_layer();
      if (!layer)
        layer = m_doc->layers[0].get();
      // Pre-insert at front so new object appears at top of layer list
      layer->children.insert(layer->children.begin(), clone_node(obj));
      m_selected = layer->children.front().get();

      if (m_history) {
        auto cmd = std::make_unique<AddNodeCommand>(layer, clone_node(obj));
        // Don't call execute() — object already inserted above
        m_history->push(std::move(cmd));
      }
    }

    m_sig_selection.emit(m_selected);
    m_sig_request_tool.emit(ActiveTool::Selection);
    m_sig_doc_changed.emit();
    queue_draw();
  } else if (m_tool == ActiveTool::Pen) {
    double ddx, ddy;
    screen_to_doc(m_mouse_x, m_mouse_y, ddx, ddy);
    m_pen_tool.on_release({ddx, ddy});
    if (m_pen_closing) {
      m_pen_closing = false;
      commit_pen_path();
    }
    queue_draw();
  } else if (m_tool == ActiveTool::Node) {
    on_node_end();
  } else if (m_tool == ActiveTool::Ruler) {
    on_ruler_end(m_draw_cur_dx, m_draw_cur_dy);
  } else if (m_tool == ActiveTool::TextOnPath) {
    on_top_end(m_draw_cur_dx, m_draw_cur_dy);
  } else if (m_tool == ActiveTool::Selection) {
    on_select_end(0, 0);
  }
}

// ── Input: selection tool
// ─────────────────────────────────────────────────────
void Canvas::on_select_begin(double x, double y) {
  double dx, dy;
  screen_to_doc(x, y, dx, dy);

  // ── Clip-pick mode? Intercept before everything else ────────────────
  // When armed, any click is destined for clip-shape selection: either
  // we consume a Path/Compound and build the ClipGroup, or we cancel.
  // Either way we must NOT fall through to normal selection logic —
  // that would mutate m_selection before finish_clip_pick consults the
  // stashed snapshot.
  if (m_clip_pick_armed) {
    SceneNode *hit = hit_test(dx, dy);
    finish_clip_pick(hit);
    return;
  }

  // ── Refpt drag? Check before guide/handle/object — refpts aren't
  //   found by hit_test (skipped via is_special_layer in the regular
  //   layer scan), so without an explicit branch a click on a refpt
  //   falls through to "click on empty" and clears the selection.
  //   Tolerance 8 screen px matches the Ref tool's own hit-test.
  if (m_doc) {
    SceneNode *rl = m_doc->ref_layer();
    if (rl && rl->visible && !rl->locked) {
      SceneNode *hit_ref = nullptr;
      for (auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        double sx, sy;
        doc_to_screen(child->ref_x, child->ref_y, sx, sy);
        if (std::hypot(x - sx, y - sy) <= 8.0) {
          hit_ref = child.get();
          break;
        }
      }
      if (hit_ref) {
        // Selection logic: if the hit refpt is already in m_selection
        // (marquee-then-drag), keep the whole selection so the drag
        // moves all selected refpts. Otherwise replace with single.
        bool already_in_sel = std::find(m_selection.begin(),
                                        m_selection.end(),
                                        hit_ref) != m_selection.end();
        if (!already_in_sel) {
          m_selection.clear();
          m_selection.push_back(hit_ref);
        }
        m_selected = hit_ref;
        // Clear guide selection — selecting a refpt is a foreground
        // pick, mirrors path-object selection's clear at line 8843.
        if (!m_guide_selection.empty()) {
          m_guide_selection.clear();
          m_sig_guide_selection_changed.emit(m_guide_selection);
        }
        // Snapshot refpts in selection for multi-refpt drag. Path /
        // text / image / warp snap lists stay empty — the
        // on_select_update branch checks m_ref_move_snaps separately.
        m_move_snaps.clear();
        m_text_move_snaps.clear();
        m_warp_env_move_snaps.clear();
        m_ref_move_snaps.clear();
        for (SceneNode *o : m_selection) {
          if (o && o->is_ref())
            m_ref_move_snaps.push_back({o, o->ref_x, o->ref_y});
        }
        m_moving = true;
        m_move_start_dx = dx;
        m_move_start_dy = dy;
        m_snap_bias_x = 0.0;
        m_snap_bias_y = 0.0;
        m_snap_x_locked = false;
        m_snap_y_locked = false;
        m_sig_selection.emit(m_selected);
        queue_draw();
        return;
      }
    }
  }

  // ── Guide drag? Check before handle/object hit test ──────────────────
  if (m_guide_hovered && !m_guide_drag_active) {
    // Record press position — drag starts in on_select_update once
    // the mouse moves beyond GUIDE_DRAG_THRESHOLD_PX.
    m_guide_drag_node = m_guide_hovered;
    m_guide_press_x = x;
    m_guide_press_y = y;

    SceneNode *g = m_guide_hovered;
    bool already = std::find(m_guide_selection.begin(), m_guide_selection.end(),
                             g) != m_guide_selection.end();
    if (m_mod_ctrl) {
      // Ctrl+click: toggle membership
      if (already)
        m_guide_selection.erase(
            std::remove(m_guide_selection.begin(), m_guide_selection.end(), g),
            m_guide_selection.end());
      else
        m_guide_selection.push_back(g);
    } else {
      // Plain click: clear objects + replace guide selection
      if (!m_selection.empty()) {
        m_selection.clear();
        m_selected = nullptr;
        m_sig_selection.emit(nullptr);
      }
      m_guide_selection = {g};
    }
    m_sig_guide_selection_changed.emit(m_guide_selection);
    queue_draw();
    return;
  }

  // ── Handle drag? Check before object hit test ────────────────────────
  // Only try handles when something is already selected and no shift.
  // M4c-1: Also suppressed when primary selection is a Warp — envelope
  // handles are the manipulation UI; bbox handles gone. Click falls
  // through to hit_test() for object-drag-translate.
  if (!m_selection.empty() && !m_mod_shift &&
      !(m_selected && m_selected->is_warp())) {
    HandleKind hk = handle_hit_test(x, y);
    // In pivot mode, corner scale handles become rotate handles
    if (m_r_held) {
      if (hk == HandleKind::NW)
        hk = HandleKind::RotateNW;
      else if (hk == HandleKind::NE)
        hk = HandleKind::RotateNE;
      else if (hk == HandleKind::SE)
        hk = HandleKind::RotateSE;
      else if (hk == HandleKind::SW)
        hk = HandleKind::RotateSW;
      // Edge-mid handles ignored in pivot mode
      else if (hk == HandleKind::N || hk == HandleKind::S ||
               hk == HandleKind::E || hk == HandleKind::W)
        hk = HandleKind::None;
    }
    if (hk != HandleKind::None) {
      // Compute union BBX of selection
      bool found = false;
      double bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
      for (SceneNode *obj : m_selection) {
        auto bb = object_bbox(*obj);
        if (!bb)
          continue;
        if (!found) {
          bx1 = bb->x;
          by1 = bb->y;
          bx2 = bb->x + bb->w;
          by2 = bb->y + bb->h;
          found = true;
        } else {
          bx1 = std::min(bx1, bb->x);
          by1 = std::min(by1, bb->y);
          bx2 = std::max(bx2, bb->x + bb->w);
          by2 = std::max(by2, bb->y + bb->h);
        }
      }
      if (found) {
        m_handle_drag = hk;
        m_handle_start_bb = {bx1, by1, bx2 - bx1, by2 - by1};

        // Pivot = opposite corner (Alt = center)
        if (m_mod_alt) {
          m_handle_pivot_x = bx1 + (bx2 - bx1) * 0.5;
          m_handle_pivot_y = by1 + (by2 - by1) * 0.5;
        } else {
          switch (hk) {
          case HandleKind::NW:
            m_handle_pivot_x = bx2;
            m_handle_pivot_y = by2;
            break;
          case HandleKind::NE:
            m_handle_pivot_x = bx1;
            m_handle_pivot_y = by2;
            break;
          case HandleKind::SE:
            m_handle_pivot_x = bx1;
            m_handle_pivot_y = by1;
            break;
          case HandleKind::SW:
            m_handle_pivot_x = bx2;
            m_handle_pivot_y = by1;
            break;
          default:
            m_handle_pivot_x = (bx1 + bx2) * 0.5;
            m_handle_pivot_y = (by1 + by2) * 0.5;
            break;
          }
        }

        // Snapshot all leaves for undo
        m_scale_snaps.clear();
        m_image_transform_snaps.clear();
        for (SceneNode *obj : m_selection) {
          if (obj->is_image()) {
            m_image_transform_snaps.push_back({obj, obj->image_x, obj->image_y,
                                               obj->image_w, obj->image_h,
                                               obj->transform});
            continue;
          }
          std::vector<SceneNode *> leaves;
          collect_paths(obj, leaves);
          for (SceneNode *leaf : leaves) {
            if (leaf->path && !leaf->path->nodes.empty())
              m_scale_snaps.push_back({leaf, leaf->path->nodes, *leaf->path});
          }
        }

        // For edge-mid handles (N/S/E/W) record drag-start doc pos so we
        // can resolve scale-vs-skew intent once the cursor moves enough.
        bool is_edge_mid = (hk == HandleKind::N || hk == HandleKind::S ||
                            hk == HandleKind::E || hk == HandleKind::W);
        if (is_edge_mid) {
          m_skew_intent_locked = false;
          m_skew_is_skew = false;
          m_skew_start_dx = dx;
          m_skew_start_dy = dy;
        }

        // For rotate kinds, record the starting angle from pivot to cursor
        if (hk == HandleKind::RotateNW || hk == HandleKind::RotateNE ||
            hk == HandleKind::RotateSE || hk == HandleKind::RotateSW) {
          // Use custom pivot if set (rotate-from-point), otherwise
          // BBX center (or Alt = opposite corner).
          double rpx, rpy;
          if (m_has_custom_pivot) {
            rpx = m_custom_pivot_x;
            rpy = m_custom_pivot_y;
          } else if (m_mod_alt) {
            switch (hk) {
            case HandleKind::RotateNW:
              rpx = bx2;
              rpy = by2;
              break;
            case HandleKind::RotateNE:
              rpx = bx1;
              rpy = by2;
              break;
            case HandleKind::RotateSE:
              rpx = bx1;
              rpy = by1;
              break;
            case HandleKind::RotateSW:
              rpx = bx2;
              rpy = by1;
              break;
            default:
              rpx = (bx1 + bx2) * 0.5;
              rpy = (by1 + by2) * 0.5;
              break;
            }
          } else {
            rpx = (bx1 + bx2) * 0.5;
            rpy = (by1 + by2) * 0.5;
          }
          m_handle_pivot_x = rpx;
          m_handle_pivot_y = rpy;
          m_rotate_start_dx = dx;
          m_rotate_start_dy = dy;
        }

        m_moving = false;
        queue_draw();
        return;
      }
    }
  }

  SceneNode *hit = hit_test(dx, dy);

  // ── Ctrl+click: select-behind, wrap-around cycle within one layer ────
  // Build a cycle of all objects at the click point in m_selected's
  // parent layer, starting from the topmost (whatever hit_test returned)
  // and descending. Rotate one position forward from m_selected's slot
  // in the cycle — wrapping to the top if m_selected is at the bottom.
  //
  // Gate does NOT require hit == m_selected. After a previous Ctrl+click
  // rotation, m_selected may be the middle or bottom rect, but hit_test
  // still returns the topmost. Requiring equality would break repeated
  // Ctrl+clicks into a back-and-forth between top and second-from-top.
  //
  // Plain click on a selected object falls through to start a move.
  // Shift+click still does additive selection.
  // Alt is excluded because Ctrl+Alt+click is the align-anchor toggle
  // (handled below in the if(hit) block) — without this guard the cycle
  // would consume the click first when only one object is selected.
  if (m_mod_ctrl && !m_mod_shift && !m_mod_alt && hit && m_selected &&
      m_selection.size() == 1) {
    // Find m_selected's parent layer.
    SceneNode *sel_layer = nullptr;
    for (auto &layer : m_doc->layers) {
      for (auto &ch : layer->children) {
        if (ch.get() == m_selected) {
          sel_layer = layer.get();
          break;
        }
      }
      if (sel_layer)
        break;
    }
    if (sel_layer) {
      // Helper: is node a direct child of sel_layer?
      auto in_sel_layer = [&](SceneNode *n) -> bool {
        for (auto &ch : sel_layer->children)
          if (ch.get() == n)
            return true;
        return false;
      };

      // Start the cycle from the topmost hit. If that hit is outside
      // sel_layer, the cycle isn't meaningful for our selection — skip.
      std::vector<SceneNode *> cycle;
      if (in_sel_layer(hit)) {
        cycle.push_back(hit);
        SceneNode *prev = hit;
        while (SceneNode *n = hit_test_next(dx, dy, prev)) {
          if (!in_sel_layer(n))
            break; // layer boundary
          if (std::find(cycle.begin(), cycle.end(), n) != cycle.end())
            break; // cycle closed
          cycle.push_back(n);
          prev = n;
        }
      }

      // Rotate forward from m_selected's position (wrap to front if past
      // the end). If m_selected isn't in the cycle, fall back to the top.
      if (cycle.size() > 1) {
        auto it = std::find(cycle.begin(), cycle.end(), m_selected);
        if (it == cycle.end()) {
          hit = cycle.front();
        } else {
          auto next_it = std::next(it);
          if (next_it == cycle.end())
            next_it = cycle.begin();
          hit = *next_it;
        }
      }
      // If cycle.size() <= 1, nothing else to cycle to — hit stays.
    }
  }

  if (hit) {
    // ── Ctrl+Alt+click: toggle align anchor on hit object ──────────
    // Selection-time mark for the next Align op. Validator-on-read
    // (align_anchor()) clears it the moment the marked object leaves
    // m_selection, so we only need to set it here. Selects the object
    // first if it isn't already selected, so a single Ctrl+Alt+click
    // on an unselected object both selects AND anchors. Toggling: a
    // second Ctrl+Alt+click on the same anchored object clears the
    // anchor (object stays selected). Distribute ops ignore the anchor.
    //
    // Why not plain Alt+click: that's already taken by the Alt+drag
    // duplicate-in-place idiom (see m_alt_drag_dup below). Ctrl+Alt
    // pairs cleanly because the Ctrl+click select-behind cycle gates
    // on m_selection.size() == 1 and is naturally orthogonal here.
    if (m_mod_alt && m_mod_ctrl && !m_mod_shift) {
      // Add to selection if not already there.
      if (!is_selected(hit)) {
        m_selection.push_back(hit);
        m_selected = hit;
      }
      // Toggle anchor: if hit is already the anchor, clear; else set.
      // Read through the validator so a stale pointer (e.g. anchor
      // pointed to a deleted object) is normalised before compare.
      SceneNode *cur = align_anchor();
      m_align_anchor = (cur == hit) ? nullptr : hit;
      m_moving = false;
      m_move_snaps.clear();
      m_sig_selection.emit(m_selected);
      queue_draw();
      return;
    }

    if (m_mod_shift) {
      // ── Shift+click: toggle object in/out of selection ────────────
      auto it = std::find(m_selection.begin(), m_selection.end(), hit);
      if (it != m_selection.end()) {
        m_selection.erase(it);
        m_selected = m_selection.empty() ? nullptr : m_selection.front();
      } else {
        m_selection.push_back(hit);
        m_selected = hit;
      }
      m_moving = false;
      m_move_snaps.clear();
      m_sig_selection.emit(m_selected);
      queue_draw();
      return;
    }

    // ── Plain click: select this object ──────────────────────────────
    // Selecting an object clears guide selection
    if (!m_guide_selection.empty()) {
      m_guide_selection.clear();
      m_sig_guide_selection_changed.emit(m_guide_selection);
    }
    // If clicking an already-selected object in a multi-selection, keep
    // the selection and start moving; otherwise replace selection.
    bool already_selected = is_selected(hit);
    if (!already_selected) {
      m_selection.clear();
      m_selection.push_back(hit);
      // PTT coupled selection: also add the pair partner so both move together
      SceneNode *partner = top_pair_partner(hit);
      if (partner && !is_selected(partner))
        m_selection.push_back(partner);
      // New selection → reset custom pivot
      m_has_custom_pivot = false;
      m_pivot_dragging = false;
    }
    m_selected = hit;
    m_moving = true;
    m_move_start_dx = dx;
    m_move_start_dy = dy;
    m_snap_bias_x = 0.0;
    m_snap_bias_y = 0.0;
    m_snap_x_locked = false;
    m_snap_y_locked = false;

    // ── Alt+drag: duplicate in-place, then move the clones ───────────
    // The originals stay where they are; we immediately insert clones
    // at the same position and redirect the move to the clones.
    m_alt_drag_dup = false;
    if (m_mod_alt && !m_selection.empty()) {
      auto entries = collect_selection_entries(m_doc, m_selection);
      if (!entries.empty()) {
        std::vector<DuplicateCommand::Entry> cmd_entries;
        std::vector<SceneNode *> clone_sel;
        int id_counter = s_next_id;
        int shift = 0;
        for (auto &e : entries) {
          auto dup = clone_node(*e.node);
          freshen_ids(dup.get(), m_doc, id_counter);
          // No position offset — clone is placed exactly on top
          int ins = e.index + shift;
          auto snap = clone_node(*dup);
          clone_sel.push_back(dup.get());
          e.parent->children.insert(e.parent->children.begin() + ins,
                                    std::move(dup));
          cmd_entries.push_back({e.parent, std::move(snap), ins});
          ++shift;
        }
        s_next_id = id_counter;
        if (m_history)
          m_history->push(
              std::make_unique<DuplicateCommand>(std::move(cmd_entries)));
        // Redirect selection to clones — the move will operate on them
        m_selection = clone_sel;
        m_selected = clone_sel.empty() ? nullptr : clone_sel[0];
        m_alt_drag_dup = true;
      }
    }

    // Snapshot all selected objects for multi-move
    m_move_snaps.clear();
    m_text_move_snaps.clear();
    m_warp_env_move_snaps.clear();
    // Helper — recursively find Warps in a subtree so nested Warps
    // (inside a selected Group/Compound) also have their envelopes
    // snapshotted for drag-translate.
    std::function<void(SceneNode *)> collect_warps = [&](SceneNode *n) {
      if (!n)
        return;
      if (n->is_warp()) {
        m_warp_env_move_snaps.push_back(
            {n, n->warp_env_top, n->warp_env_bottom});
      }
      for (auto &c : n->children)
        collect_warps(c.get());
      if (n->is_warp()) {
        // Don't recurse into warp_source/cache — they aren't Warps,
        // they're the source/derived geometry. (Nested Warps
        // are not supported at this time, but children-walk above
        // would catch them if/when they are.)
      }
    };
    for (SceneNode *obj : m_selection) {
      if (obj->is_text()) {
        m_text_move_snaps.push_back({obj, obj->text_x, obj->text_y});
        continue;
      }
      if (obj->is_image()) {
        m_text_move_snaps.push_back({obj, obj->image_x, obj->image_y});
        continue;
      }
      collect_warps(obj);
      std::vector<SceneNode *> leaves;
      collect_paths(obj, leaves);
      for (SceneNode *leaf : leaves) {
        if (!leaf->path->nodes.empty())
          m_move_snaps.push_back({leaf, leaf->path->nodes, *leaf->path});
      }
      // Nested Text/Image inside containers (Group/Compound/ClipGroup) —
      // collect_paths skips them by design (no Path geometry), so collect
      // them separately and snapshot their position fields for drag-move.
      if (obj->type == SceneNode::Type::Group ||
          obj->type == SceneNode::Type::Compound ||
          obj->type == SceneNode::Type::ClipGroup) {
        std::vector<SceneNode *> ti_leaves;
        collect_text_image_leaves(obj, ti_leaves);
        for (SceneNode *leaf : ti_leaves) {
          if (leaf->is_text())
            m_text_move_snaps.push_back({leaf, leaf->text_x, leaf->text_y});
          else if (leaf->is_image())
            m_text_move_snaps.push_back({leaf, leaf->image_x, leaf->image_y});
        }
      }
    }
    // Legacy single-object fields (used by undo in on_select_end)
    if (hit->type == SceneNode::Type::Path && hit->path &&
        !hit->path->nodes.empty()) {
      m_move_orig_nodes = hit->path->nodes;
      m_move_before_path = *hit->path;
    } else {
      m_move_orig_nodes.clear();
    }

    m_sig_selection.emit(m_selected);
    LOG_DEBUG("Selected object {} (total selected: {})", hit->id,
              m_selection.size());

  } else {
    // M4c-2d: When primary is a Warp, click/Shift+click on empty starts
    // an envelope marquee (rubber-band anchor pick). Release handler
    // picks anchors inside. Plain → replaces set; Shift → additive.
    // Must run before the general "Shift+click-empty does nothing" rule
    // so Shift+marquee can start.
    if (m_selected && m_selected->is_warp()) {
      m_marquee_active = true;
      m_marquee_start_dx = dx;
      m_marquee_start_dy = dy;
      m_marquee_cur_dx = dx;
      m_marquee_cur_dy = dy;
      m_warp_env_marquee_additive = m_mod_shift;
      queue_draw();
      return;
    }

    if (m_mod_shift) {
      // Shift+click on empty — keep existing selection, do nothing
      queue_draw();
      return;
    }

    // ── Click on empty: clear selection and guide selection, start marquee
    if (!m_guide_selection.empty()) {
      m_guide_selection.clear();
      m_sig_guide_selection_changed.emit(m_guide_selection);
    }
    m_selected = nullptr;
    m_selection.clear();
    m_moving = false;
    m_move_snaps.clear();
    m_warp_env_move_snaps.clear();
    m_move_orig_nodes.clear();
    // Deselect → reset custom pivot
    m_has_custom_pivot = false;
    m_pivot_dragging = false;
    m_marquee_active = true;
    m_marquee_start_dx = dx;
    m_marquee_start_dy = dy;
    m_marquee_cur_dx = dx;
    m_marquee_cur_dy = dy;
    m_sig_selection.emit(nullptr);
  }
  queue_draw();
}

void Canvas::on_select_update(double /*dx*/, double /*dy*/) {
  // ── Guide drag ────────────────────────────────────────────────────────
  // If a guide node is armed (press recorded) but drag not yet active,
  // start the drag once the mouse moves beyond the threshold.
  if (!m_guide_drag_active && m_guide_drag_node) {
    double dist =
        std::hypot(m_mouse_x - m_guide_press_x, m_mouse_y - m_guide_press_y);
    if (dist >= GUIDE_DRAG_THRESHOLD_PX)
      m_guide_drag_active = true;
  }
  if (m_guide_drag_active && m_guide_drag_node) {
    if (m_guide_drag_node->locked) {
      // Guide is locked — swallow the drag with no position change.
      queue_draw();
      return;
    }
    double cur_dx, cur_dy;
    screen_to_doc(m_mouse_x, m_mouse_y, cur_dx, cur_dy);
    // Axis-aligned guides: slide along the varying axis only so the
    // perpendicular position is preserved.  Angled guides: move the whole
    // anchor to follow the cursor (anchor slides along the perpendicular).
    if (m_guide_drag_node->guide_is_horizontal()) {
      m_guide_drag_node->guide_y = cur_dy;
    } else if (m_guide_drag_node->guide_is_vertical()) {
      m_guide_drag_node->guide_x = cur_dx;
    } else {
      m_guide_drag_node->guide_x = cur_dx;
      m_guide_drag_node->guide_y = cur_dy;
    }
    queue_draw();
    return;
  }

  // ── Handle drag ───────────────────────────────────────────────────────
  if (m_handle_drag != HandleKind::None) {
    double cur_dx, cur_dy;
    screen_to_doc(m_mouse_x, m_mouse_y, cur_dx, cur_dy);

    // ── Rotate drag ───────────────────────────────────────────────────
    bool is_rotate = (m_handle_drag == HandleKind::RotateNW ||
                      m_handle_drag == HandleKind::RotateNE ||
                      m_handle_drag == HandleKind::RotateSE ||
                      m_handle_drag == HandleKind::RotateSW);
    if (is_rotate) {
      // Pivot is locked at drag-start (m_handle_pivot_x/y set in
      // on_select_begin). Recomputing live would cause a jump if Alt is toggled
      // mid-drag.
      const double px = m_handle_pivot_x;
      const double py = m_handle_pivot_y;

      double start_angle =
          std::atan2(m_rotate_start_dy - py, m_rotate_start_dx - px);
      double cur_angle = std::atan2(cur_dy - py, cur_dx - px);
      double delta = cur_angle - start_angle;

      // Shift = snap to 15° increments
      if (m_mod_shift) {
        constexpr double SNAP = M_PI / 12.0; // 15°
        delta = std::round(delta / SNAP) * SNAP;
      }

      m_last_rotate_angle_deg = delta * (180.0 / M_PI);

      double cosA = std::cos(delta);
      double sinA = std::sin(delta);

      for (auto &snap : m_scale_snaps) {
        if (!snap.obj->path)
          continue;
        auto &nodes = snap.obj->path->nodes;
        const auto &orig = snap.orig_nodes;
        if (nodes.size() != orig.size())
          continue;
        for (size_t i = 0; i < nodes.size(); ++i) {
          auto rot = [&](double ox, double oy, double &rx, double &ry) {
            double rx0 = ox - px, ry0 = oy - py;
            rx = px + rx0 * cosA - ry0 * sinA;
            ry = py + rx0 * sinA + ry0 * cosA;
          };
          rot(orig[i].x, orig[i].y, nodes[i].x, nodes[i].y);
          rot(orig[i].cx1, orig[i].cy1, nodes[i].cx1, nodes[i].cy1);
          rot(orig[i].cx2, orig[i].cy2, nodes[i].cx2, nodes[i].cy2);
        }
      }
      // Rotate image nodes — rotate centre point, update transform matrix
      for (auto &isnap : m_image_transform_snaps) {
        auto &obj = *isnap.obj;
        // Image centre in doc space
        double icx = isnap.orig_x + isnap.orig_w * 0.5;
        double icy = isnap.orig_y + isnap.orig_h * 0.5;
        // Rotate centre around pivot
        double rx0 = icx - px, ry0 = icy - py;
        double new_cx = px + rx0 * cosA - ry0 * sinA;
        double new_cy = py + rx0 * sinA + ry0 * cosA;
        obj.image_x = new_cx - isnap.orig_w * 0.5;
        obj.image_y = new_cy - isnap.orig_h * 0.5;
        obj.image_w = isnap.orig_w;
        obj.image_h = isnap.orig_h;
        // Accumulate rotation in transform: R_new = R(delta) * R_orig
        // Store as rotation angle in transform
        // (a=cos,b=sin,c=-sin,d=cos,e=0,f=0) relative to image centre (applied
        // during draw around image centre)
        double orig_angle =
            std::atan2(isnap.orig_transform.b, isnap.orig_transform.a);
        double new_angle = orig_angle + delta;
        obj.transform.a = std::cos(new_angle);
        obj.transform.b = std::sin(new_angle);
        obj.transform.c = -std::sin(new_angle);
        obj.transform.d = std::cos(new_angle);
        obj.transform.e = 0.0;
        obj.transform.f = 0.0;
      }
      queue_draw();
      return;
    }

    // ── Scale / Skew drag (edge-mid handles N/S/E/W) ─────────────────
    bool is_edge_mid =
        (m_handle_drag == HandleKind::N || m_handle_drag == HandleKind::S ||
         m_handle_drag == HandleKind::E || m_handle_drag == HandleKind::W);

    if (is_edge_mid && !m_skew_intent_locked) {
      // Resolve intent once the drag exceeds 12px from start (screen space).
      // Require a 2:1 dominance ratio before committing — pure jitter near
      // the axis boundary stays ambiguous and keeps waiting.
      double dsx, dsy, ssx, ssy;
      doc_to_screen(cur_dx, cur_dy, dsx, dsy);
      doc_to_screen(m_skew_start_dx, m_skew_start_dy, ssx, ssy);
      double adx = std::abs(dsx - ssx);
      double ady = std::abs(dsy - ssy);
      double total = adx + ady;
      if (total > 12.0) {
        // E/W: ady dominant → skew, adx dominant → scale
        // N/S: adx dominant → skew, ady dominant → scale
        bool vert_dominant = (ady > adx * 1.5);
        bool horiz_dominant = (adx > ady * 1.5);
        bool resolved =
            (m_handle_drag == HandleKind::E || m_handle_drag == HandleKind::W)
                ? (vert_dominant || horiz_dominant)
                : (horiz_dominant || vert_dominant);
        if (resolved) {
          m_skew_intent_locked = true;
          if (m_handle_drag == HandleKind::E || m_handle_drag == HandleKind::W)
            m_skew_is_skew = vert_dominant;
          else
            m_skew_is_skew = horiz_dominant;
        }
      }
    }

    // ── Skew drag ─────────────────────────────────────────────────────
    if (is_edge_mid && m_skew_intent_locked && m_skew_is_skew) {
      const BBoxF &sb = m_handle_start_bb;
      double shear = 0.0;
      bool horiz_shear =
          (m_handle_drag == HandleKind::N || m_handle_drag == HandleKind::S);

      if (horiz_shear) {
        // N/S horizontal drag → horizontal shear: x' = x + (y - anchor_y) * k
        // Default: opposite edge is anchor (stays fixed).
        // Alt: BBX center is anchor (symmetric shear from middle).
        double anchor_y;
        double span;
        if (m_mod_alt) {
          anchor_y = sb.y + sb.h * 0.5;
          span = sb.h * 0.5; // half-height: each edge moves half as much
        } else {
          // N dragged: bottom edge locked (sb.y + sb.h)
          // S dragged: top edge locked (sb.y)
          anchor_y = (m_handle_drag == HandleKind::N) ? sb.y + sb.h : sb.y;
          span = sb.h;
        }
        if (std::abs(span) > 1e-6)
          shear = -(cur_dx - m_skew_start_dx) / span;
        for (auto &snap : m_scale_snaps) {
          if (!snap.obj->path)
            continue;
          auto &nodes = snap.obj->path->nodes;
          const auto &orig = snap.orig_nodes;
          if (nodes.size() != orig.size())
            continue;
          for (size_t i = 0; i < nodes.size(); ++i) {
            nodes[i].x = orig[i].x + (orig[i].y - anchor_y) * shear;
            nodes[i].y = orig[i].y;
            nodes[i].cx1 = orig[i].cx1 + (orig[i].cy1 - anchor_y) * shear;
            nodes[i].cy1 = orig[i].cy1;
            nodes[i].cx2 = orig[i].cx2 + (orig[i].cy2 - anchor_y) * shear;
            nodes[i].cy2 = orig[i].cy2;
          }
        }
      } else {
        // E/W vertical drag → vertical shear: y' = y + (x - anchor_x) * k
        // Default: opposite edge is anchor (stays fixed).
        // Alt: BBX center is anchor (symmetric shear from middle).
        double anchor_x;
        double span;
        if (m_mod_alt) {
          anchor_x = sb.x + sb.w * 0.5;
          span = sb.w * 0.5;
        } else {
          // E dragged: left edge locked (sb.x)
          // W dragged: right edge locked (sb.x + sb.w)
          anchor_x = (m_handle_drag == HandleKind::E) ? sb.x : sb.x + sb.w;
          span = sb.w;
        }
        if (std::abs(span) > 1e-6)
          shear = (cur_dy - m_skew_start_dy) / span;
        for (auto &snap : m_scale_snaps) {
          if (!snap.obj->path)
            continue;
          auto &nodes = snap.obj->path->nodes;
          const auto &orig = snap.orig_nodes;
          if (nodes.size() != orig.size())
            continue;
          for (size_t i = 0; i < nodes.size(); ++i) {
            nodes[i].x = orig[i].x;
            nodes[i].y = orig[i].y + (orig[i].x - anchor_x) * shear;
            nodes[i].cx1 = orig[i].cx1;
            nodes[i].cy1 = orig[i].cy1 + (orig[i].cx1 - anchor_x) * shear;
            nodes[i].cx2 = orig[i].cx2;
            nodes[i].cy2 = orig[i].cy2 + (orig[i].cx2 - anchor_x) * shear;
          }
        }
      }
      // Skew image nodes via transform matrix shear
      for (auto &isnap : m_image_transform_snaps) {
        auto &obj = *isnap.obj;
        obj.image_x = isnap.orig_x;
        obj.image_y = isnap.orig_y;
        obj.image_w = isnap.orig_w;
        obj.image_h = isnap.orig_h;
        // Preserve any existing rotation, add shear
        double ca = isnap.orig_transform.a, sa = isnap.orig_transform.b;
        if (std::abs(ca) < 1e-10 && std::abs(sa) < 1e-10) {
          ca = 1.0;
          sa = 0.0;
        }
        if (horiz_shear) {
          // Horizontal shear: x' = x + k*y
          obj.transform.a = ca;
          obj.transform.b = sa;
          obj.transform.c = -sa + shear * ca;
          obj.transform.d = ca + shear * sa;
        } else {
          // Vertical shear: y' = y + k*x
          obj.transform.a = ca + shear * (-sa);
          obj.transform.b = sa + shear * ca;
          obj.transform.c = -sa;
          obj.transform.d = ca;
        }
        obj.transform.e = 0.0;
        obj.transform.f = 0.0;
      }
      queue_draw();
      return;
    }

    // ── Scale drag ────────────────────────────────────────────────────
    const BBoxF &sb = m_handle_start_bb;

    // Avoid degenerate BBX
    if (sb.w < 1e-6 || sb.h < 1e-6)
      return;

    // Recompute pivot live so Alt can be toggled mid-drag.
    // Alt = scale from center; default = opposite edge/corner.
    double px, py;
    if (m_mod_alt) {
      px = sb.x + sb.w * 0.5;
      py = sb.y + sb.h * 0.5;
    } else {
      switch (m_handle_drag) {
      case HandleKind::NW:
        px = sb.x + sb.w;
        py = sb.y + sb.h;
        break;
      case HandleKind::NE:
        px = sb.x;
        py = sb.y + sb.h;
        break;
      case HandleKind::SE:
        px = sb.x;
        py = sb.y;
        break;
      case HandleKind::SW:
        px = sb.x + sb.w;
        py = sb.y;
        break;
      // Edge mids: pivot is opposite edge midpoint
      case HandleKind::N:
        px = sb.x + sb.w * 0.5;
        py = sb.y + sb.h;
        break;
      case HandleKind::S:
        px = sb.x + sb.w * 0.5;
        py = sb.y;
        break;
      case HandleKind::E:
        px = sb.x;
        py = sb.y + sb.h * 0.5;
        break;
      case HandleKind::W:
        px = sb.x + sb.w;
        py = sb.y + sb.h * 0.5;
        break;
      default:
        px = sb.x + sb.w * 0.5;
        py = sb.y + sb.h * 0.5;
        break;
      }
    }

    // Original handle position and constrained scale axes.
    // Edge mids scale one axis only (sx or sy = 1.0 on the fixed axis).
    double orig_hx = 0, orig_hy = 0;
    bool scale_x = true, scale_y = true;
    switch (m_handle_drag) {
    case HandleKind::NW:
      orig_hx = sb.x;
      orig_hy = sb.y;
      break;
    case HandleKind::NE:
      orig_hx = sb.x + sb.w;
      orig_hy = sb.y;
      break;
    case HandleKind::SE:
      orig_hx = sb.x + sb.w;
      orig_hy = sb.y + sb.h;
      break;
    case HandleKind::SW:
      orig_hx = sb.x;
      orig_hy = sb.y + sb.h;
      break;
    case HandleKind::N:
      orig_hx = sb.x + sb.w * 0.5;
      orig_hy = sb.y;
      scale_x = false;
      break;
    case HandleKind::S:
      orig_hx = sb.x + sb.w * 0.5;
      orig_hy = sb.y + sb.h;
      scale_x = false;
      break;
    case HandleKind::E:
      orig_hx = sb.x + sb.w;
      orig_hy = sb.y + sb.h * 0.5;
      scale_y = false;
      break;
    case HandleKind::W:
      orig_hx = sb.x;
      orig_hy = sb.y + sb.h * 0.5;
      scale_y = false;
      break;
    default:
      return;
    }

    double dx_from_pivot = orig_hx - px;
    double dy_from_pivot = orig_hy - py;

    // ── Snap the dragged handle (S98) ────────────────────────────────────
    // The handle's destination in doc space is (cur_dx, cur_dy). We snap
    // that point to nearest grid/guide/margin/ref via snap_x / snap_y
    // BEFORE deriving sx / sy, so the snap target ends up exactly on
    // the dragged edge. Pivot (px, py) is unaffected — it's the anchor.
    //
    // Per-axis: edge-mid handles (N/S/E/W) only scale one axis; only
    // snap the axis that's actually moving. Shift (uniform scale) is
    // handled below — we snap each axis independently here, then collapse
    // to a single scale factor under Shift.
    //
    // Tolerance matches the single-point snap idiom in snap_x / snap_y
    // (12 px); snap_move's engage/release hysteresis isn't a fit because
    // there's no equivalent of a "moving away from snapped" continuous
    // delta in scale — each frame is a fresh snap candidate.
    double snap_dx = cur_dx;
    double snap_dy = cur_dy;
    if (scale_x) snap_dx = snap_x(cur_dx, /*tolerance_px=*/12.0);
    if (scale_y) snap_dy = snap_y(cur_dy, /*tolerance_px=*/12.0);

    // Compute raw scale on each active axis, from the snapped destination.
    double sx = 1.0, sy = 1.0;
    if (scale_x && std::abs(dx_from_pivot) > 1e-6)
      sx = (snap_dx - px) / dx_from_pivot;
    if (scale_y && std::abs(dy_from_pivot) > 1e-6)
      sy = (snap_dy - py) / dy_from_pivot;

    // Shift on corner handles = uniform scale (largest delta wins).
    // After snap each axis may have its own target; "largest delta wins"
    // picks one, which is the right behaviour — the user gets a snap on
    // the dominant axis, with the other axis matching that scale.
    if (m_mod_shift && scale_x && scale_y) {
      double s = (std::abs(sx - 1.0) >= std::abs(sy - 1.0)) ? sx : sy;
      sx = sy = s;
    }

    // Apply to all leaves from their original snapshots
    for (auto &snap : m_scale_snaps) {
      if (!snap.obj->path)
        continue;
      auto &nodes = snap.obj->path->nodes;
      const auto &orig = snap.orig_nodes;
      if (nodes.size() != orig.size())
        continue;
      for (size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].x = px + (orig[i].x - px) * sx;
        nodes[i].y = py + (orig[i].y - py) * sy;
        nodes[i].cx1 = px + (orig[i].cx1 - px) * sx;
        nodes[i].cy1 = py + (orig[i].cy1 - py) * sy;
        nodes[i].cx2 = px + (orig[i].cx2 - px) * sx;
        nodes[i].cy2 = py + (orig[i].cy2 - py) * sy;
      }
    }
    // Scale image nodes — move top-left and resize
    for (auto &isnap : m_image_transform_snaps) {
      auto &obj = *isnap.obj;
      // Original corners
      double x1 = isnap.orig_x, y1 = isnap.orig_y;
      double x2 = x1 + isnap.orig_w, y2 = y1 + isnap.orig_h;
      // Scale corners from pivot
      double nx1 = px + (x1 - px) * sx;
      double ny1 = py + (y1 - py) * sy;
      double nx2 = px + (x2 - px) * sx;
      double ny2 = py + (y2 - py) * sy;
      obj.image_x = std::min(nx1, nx2);
      obj.image_y = std::min(ny1, ny2);
      obj.image_w = std::abs(nx2 - nx1);
      obj.image_h = std::abs(ny2 - ny1);
      // Preserve existing transform rotation/shear — only position/size change
      obj.transform = isnap.orig_transform;
    }
    queue_draw();
    return;
  }

  // ── Marquee update ────────────────────────────────────────────────────
  if (m_marquee_active) {
    double cx, cy;
    screen_to_doc(m_mouse_x, m_mouse_y, cx, cy);
    m_marquee_cur_dx = cx;
    m_marquee_cur_dy = cy;
    queue_draw();
    return;
  }

  if (!m_moving || m_selection.empty())
    return;

  double cur_dx, cur_dy;
  screen_to_doc(m_mouse_x, m_mouse_y, cur_dx, cur_dy);

  double raw_dx = cur_dx - m_move_start_dx;
  double raw_dy = cur_dy - m_move_start_dy;
  auto [delta_x, delta_y] = snap_move(raw_dx, raw_dy);

  // Move all selected objects by the same delta
  for (auto &snap_data : m_move_snaps) {
    SceneNode *obj = snap_data.obj;
    if (obj->type != SceneNode::Type::Path || !obj->path)
      continue;
    if (snap_data.orig_nodes.size() != obj->path->nodes.size())
      continue;
    for (size_t i = 0; i < obj->path->nodes.size(); ++i) {
      auto &nd = obj->path->nodes[i];
      const auto &orig = snap_data.orig_nodes[i];
      nd.x = orig.x + delta_x;
      nd.y = orig.y + delta_y;
      nd.cx1 = orig.cx1 + delta_x;
      nd.cy1 = orig.cy1 + delta_y;
      nd.cx2 = orig.cx2 + delta_x;
      nd.cy2 = orig.cy2 + delta_y;
    }
  }
  // Move Text and Image nodes
  for (auto &tsnap : m_text_move_snaps) {
    if (tsnap.obj->is_image()) {
      tsnap.obj->image_x = tsnap.orig_x + delta_x;
      tsnap.obj->image_y = tsnap.orig_y + delta_y;
    } else {
      tsnap.obj->text_x = tsnap.orig_x + delta_x;
      tsnap.obj->text_y = tsnap.orig_y + delta_y;
    }
  }
  // Move ref points — same per-point translation as text/image.
  for (auto &rsnap : m_ref_move_snaps) {
    if (rsnap.obj && rsnap.obj->is_ref()) {
      rsnap.obj->ref_x = rsnap.orig_x + delta_x;
      rsnap.obj->ref_y = rsnap.orig_y + delta_y;
    }
  }
  // Translate Warp envelopes alongside source geometry so the whole
  // Warp moves as a unit (envelope + source + derived caches).
  // C++17: structured bindings can't be captured by lambdas, so copy
  // delta_x/delta_y into plain locals for the inner translate lambda.
  const double dxl = delta_x;
  const double dyl = delta_y;
  for (auto &wsnap : m_warp_env_move_snaps) {
    SceneNode *w = wsnap.warp;
    if (!w || !w->is_warp())
      continue;
    w->warp_env_top = wsnap.orig_env_top;
    w->warp_env_bottom = wsnap.orig_env_bottom;
    auto translate_env = [dxl, dyl](PathData &env) {
      for (auto &n : env.nodes) {
        n.x += dxl;
        n.y += dyl;
        n.cx1 += dxl;
        n.cy1 += dyl;
        n.cx2 += dxl;
        n.cy2 += dyl;
      }
    };
    translate_env(w->warp_env_top);
    translate_env(w->warp_env_bottom);
    w->warp_cache_dirty = true;
  }
  queue_draw();
}

void Canvas::on_select_end(double /*dx*/, double /*dy*/) {
  // ── Guide drag end ────────────────────────────────────────────────────
  if (m_guide_drag_node) {
    if (m_guide_drag_active && !m_guide_drag_node->locked) {
      // Actual drag — commit position (new-model only).
      double cur_dx, cur_dy;
      screen_to_doc(m_mouse_x, m_mouse_y, cur_dx, cur_dy);
      if (m_guide_drag_node->guide_is_horizontal()) {
        m_guide_drag_node->guide_y = cur_dy;
      } else if (m_guide_drag_node->guide_is_vertical()) {
        m_guide_drag_node->guide_x = cur_dx;
      } else {
        m_guide_drag_node->guide_x = cur_dx;
        m_guide_drag_node->guide_y = cur_dy;
      }
      m_sig_doc_changed.emit();
    }
    // Either way, clear armed state
    m_guide_drag_node = nullptr;
    m_guide_drag_active = false;
    queue_draw();
    return;
  }

  // ── Handle scale drag end — push undo ─────────────────────────────────
  if (m_handle_drag != HandleKind::None) {
    bool was_rotate = (m_handle_drag == HandleKind::RotateNW ||
                       m_handle_drag == HandleKind::RotateNE ||
                       m_handle_drag == HandleKind::RotateSE ||
                       m_handle_drag == HandleKind::RotateSW);
    bool was_skew = m_skew_intent_locked && m_skew_is_skew;
    m_handle_drag = HandleKind::None;
    m_skew_intent_locked = false;
    m_skew_is_skew = false;
    if (m_history) {
      std::vector<ScaleObjectsCommand::LeafSnap> snaps;
      for (auto &snap : m_scale_snaps) {
        SceneNode *obj = snap.obj;
        if (!obj->path)
          continue;
        bool changed = false;
        for (size_t i = 0;
             i < obj->path->nodes.size() && i < snap.orig_nodes.size(); ++i) {
          if (std::abs(obj->path->nodes[i].x - snap.orig_nodes[i].x) > 1e-6 ||
              std::abs(obj->path->nodes[i].y - snap.orig_nodes[i].y) > 1e-6) {
            changed = true;
            break;
          }
        }
        if (changed)
          snaps.push_back({obj, snap.before_path, *obj->path});
      }
      std::string desc = was_rotate
                             ? "Rotate object"
                             : (was_skew ? "Skew object" : "Scale object");
      if (!snaps.empty())
        m_history->push(std::make_unique<ScaleObjectsCommand>(std::move(snaps),
                                                              std::move(desc)));
      // Image transform undo
      std::vector<ScaleImageCommand::Snap> img_snaps;
      for (auto &isnap : m_image_transform_snaps) {
        auto &obj = *isnap.obj;
        bool changed =
            std::abs(obj.image_x - isnap.orig_x) > 1e-6 ||
            std::abs(obj.image_y - isnap.orig_y) > 1e-6 ||
            std::abs(obj.image_w - isnap.orig_w) > 1e-6 ||
            std::abs(obj.image_h - isnap.orig_h) > 1e-6 ||
            std::abs(obj.transform.a - isnap.orig_transform.a) > 1e-6 ||
            std::abs(obj.transform.b - isnap.orig_transform.b) > 1e-6;
        if (changed)
          img_snaps.push_back({&obj, isnap.orig_x, isnap.orig_y, isnap.orig_w,
                               isnap.orig_h, isnap.orig_transform, obj.image_x,
                               obj.image_y, obj.image_w, obj.image_h,
                               obj.transform});
      }
      if (!img_snaps.empty())
        m_history->push(
            std::make_unique<ScaleImageCommand>(std::move(img_snaps), desc));
    }
    // ── Capture scale factor before clearing snaps ────────────────────
    double rec_scale_x = 1.0, rec_scale_y = 1.0;
    if (!was_rotate && !was_skew && !m_scale_snaps.empty()) {
      // Compute from first path snap: ratio of after-bbox to before-bbox
      double bx1b = 1e9, by1b = 1e9, bx2b = -1e9, by2b = -1e9;
      double bx1a = 1e9, by1a = 1e9, bx2a = -1e9, by2a = -1e9;
      for (auto &sn : m_scale_snaps) {
        for (auto &nd : sn.before_path.nodes) {
          bx1b = std::min(bx1b, nd.x);
          by1b = std::min(by1b, nd.y);
          bx2b = std::max(bx2b, nd.x);
          by2b = std::max(by2b, nd.y);
        }
        if (sn.obj->path)
          for (auto &nd : sn.obj->path->nodes) {
            bx1a = std::min(bx1a, nd.x);
            by1a = std::min(by1a, nd.y);
            bx2a = std::max(bx2a, nd.x);
            by2a = std::max(by2a, nd.y);
          }
      }
      double wb = bx2b - bx1b, hb = by2b - by1b;
      double wa = bx2a - bx1a, ha = by2a - by1a;
      if (wb > 1e-6)
        rec_scale_x = wa / wb;
      if (hb > 1e-6)
        rec_scale_y = ha / hb;
    }

    m_scale_snaps.clear();
    m_image_transform_snaps.clear();
    m_sig_doc_changed.emit();
    m_sig_selection.emit(m_selected);
    queue_draw();
    // ── Record macro step ─────────────────────────────────────────────
    if (was_rotate) {
      if (std::abs(m_last_rotate_angle_deg) > 0.001) {
        MacroStep s;
        s.op = MacroStep::Op::Rotate;
        s.angle_deg = m_last_rotate_angle_deg;
        s.pivot_x = m_handle_pivot_x;
        s.pivot_y = m_handle_pivot_y;
        s.pivot_is_explicit = true;
        record_step_if_recording(s);
      }
    } else if (!was_skew) {
      if (std::abs(rec_scale_x - 1.0) > 0.001 ||
          std::abs(rec_scale_y - 1.0) > 0.001) {
        MacroStep s;
        s.op = MacroStep::Op::Scale;
        s.scale_x = rec_scale_x;
        s.scale_y = rec_scale_y;
        record_step_if_recording(s);
      }
    }
    return;
  }

  // ── Marquee end — select all intersecting objects ─────────────────────
  if (m_marquee_active) {
    m_marquee_active = false;
    double x1 = std::min(m_marquee_start_dx, m_marquee_cur_dx);
    double y1 = std::min(m_marquee_start_dy, m_marquee_cur_dy);
    double x2 = std::max(m_marquee_start_dx, m_marquee_cur_dx);
    double y2 = std::max(m_marquee_start_dy, m_marquee_cur_dy);

    // M4c-2d: Warp-primary marquee — pick envelope anchors inside rect.
    // Handles NOT picked by marquee (per spec — must be Shift-clicked).
    if (m_selected && m_selected->is_warp()) {
      bool additive = m_warp_env_marquee_additive;
      m_warp_env_marquee_additive = false;
      if (x2 - x1 > 1.0 && y2 - y1 > 1.0) {
        // Meaningful drag → pick anchors inside rect.
        if (!additive) {
          m_warp_env_picks.clear();
        }
        m_warp_env_picks_owner = m_selected;
        auto scan_env = [&](const PathData &env, bool is_top) {
          auto try_add = [&](EnvelopePick pk) {
            if (std::find(m_warp_env_picks.begin(), m_warp_env_picks.end(),
                          pk) == m_warp_env_picks.end())
              m_warp_env_picks.push_back(pk);
          };
          for (int i = 0; i < (int)env.nodes.size(); ++i) {
            const BezierNode &n = env.nodes[i];
            // Anchor
            if (n.x >= x1 && n.x <= x2 && n.y >= y1 && n.y <= y2)
              try_add({is_top, i, EnvelopePart::Anchor});
            // HandleIn — only pick if visually distinct from anchor
            // (coincident handles are not drawn and can't be aimed at).
            if (std::hypot(n.cx1 - n.x, n.cy1 - n.y) > 1e-6 && n.cx1 >= x1 &&
                n.cx1 <= x2 && n.cy1 >= y1 && n.cy1 <= y2)
              try_add({is_top, i, EnvelopePart::HandleIn});
            // HandleOut — same rule
            if (std::hypot(n.cx2 - n.x, n.cy2 - n.y) > 1e-6 && n.cx2 >= x1 &&
                n.cx2 <= x2 && n.cy2 >= y1 && n.cy2 <= y2)
              try_add({is_top, i, EnvelopePart::HandleOut});
          }
        };
        scan_env(m_selected->warp_env_top, true);
        scan_env(m_selected->warp_env_bottom, false);
      } else {
        // Click-without-drag. Plain click clears picks; Shift click
        // leaves picks alone (matches spec: click-empty clears set,
        // Shift+click-empty is a no-op).
        if (!additive && !m_warp_env_picks.empty()) {
          m_warp_env_picks.clear();
        }
      }
      queue_draw();
      return;
    }

    // Only select if marquee has meaningful size
    if (x2 - x1 > 1.0 && y2 - y1 > 1.0 && m_doc) {
      m_selection.clear();
      // Scan regular layers
      for (auto &layer : m_doc->layers) {
        if (!layer->visible || layer->locked || layer->is_special_layer())
          continue;
        for (auto &obj_uptr : layer->children) {
          SceneNode &obj = *obj_uptr;
          // s125 m1b: the marquee was previously gated to Path-only, which
          // silently filtered out Compounds, Groups, Text, Images, Blends,
          // Warps, ClipGroups — every other user-visible node type. The
          // bug surfaced as "marquee can't select compound objects" but
          // affected all non-Path types. Mirror hit_test's selectable set:
          // these are the node types that count as a single user-visible
          // object (the parent — not its inner children — is what the
          // marquee adds to selection, matching how hit_test returns the
          // Compound/Group/Blend/Warp itself, never its inner nodes).
          // Locked objects (per-object, not per-layer) are still selectable
          // — locked means "no move/delete," not "invisible to selection,"
          // matching the existing RefLayer pass below and how Illustrator
          // and Affinity behave.
          const bool selectable =
              obj.type == SceneNode::Type::Path     ||
              obj.type == SceneNode::Type::Compound ||
              obj.type == SceneNode::Type::Group    ||
              obj.type == SceneNode::Type::ClipGroup||
              obj.type == SceneNode::Type::Text     ||
              obj.type == SceneNode::Type::Image    ||
              obj.type == SceneNode::Type::Blend    ||
              obj.type == SceneNode::Type::Warp;
          if (!selectable)
            continue;
          if (obj.type == SceneNode::Type::Path && !obj.path)
            continue;  // malformed path — nothing to bbox
          if (!obj.visible)
            continue;  // respect per-object visibility (matches hit_test)
          auto bb = object_bbox(obj);
          if (!bb)
            continue;
          if (bb->x < x2 && bb->x + bb->w > x1 && bb->y < y2 &&
              bb->y + bb->h > y1)
            m_selection.push_back(&obj);
        }
      }
      // Also scan RefLayer for ref points inside marquee —
      // selectable even when locked (locked = no move/delete, not no select)
      SceneNode *rl = m_doc->ref_layer();
      if (rl && rl->visible) {
        for (auto &child : rl->children) {
          if (!child->is_ref())
            continue;
          if (child->ref_x >= x1 && child->ref_x <= x2 && child->ref_y >= y1 &&
              child->ref_y <= y2)
            m_selection.push_back(child.get());
        }
      }
      m_selected = m_selection.empty() ? nullptr : m_selection.front();
      // PTT coupled selection: ensure both members of any PTT pair are
      // included if either was caught by the marquee.
      std::vector<SceneNode *> partners;
      for (SceneNode *obj : m_selection) {
        SceneNode *p = top_pair_partner(obj);
        if (p && !is_selected(p) &&
            std::find(partners.begin(), partners.end(), p) == partners.end())
          partners.push_back(p);
      }
      for (SceneNode *p : partners)
        m_selection.push_back(p);
      m_sig_selection.emit(m_selected);
      LOG_DEBUG("Marquee selected {} objects", m_selection.size());
    }
    queue_draw();
    return;
  }

  if (m_moving) {
    m_moving = false;
    m_snap_bias_x = 0.0;
    m_snap_bias_y = 0.0;
    m_snap_x_locked = false;
    m_snap_y_locked = false;

    // Push undo command for each moved object
    if (m_history) {
      for (auto &snap_data : m_move_snaps) {
        SceneNode *obj = snap_data.obj;
        if (!obj->path || snap_data.orig_nodes.empty())
          continue;
        bool moved = false;
        for (size_t i = 0;
             i < obj->path->nodes.size() && i < snap_data.orig_nodes.size();
             ++i) {
          if (std::abs(obj->path->nodes[i].x - snap_data.orig_nodes[i].x) >
                  0.001 ||
              std::abs(obj->path->nodes[i].y - snap_data.orig_nodes[i].y) >
                  0.001) {
            moved = true;
            break;
          }
        }
        if (moved) {
          PathData after = *obj->path;
          m_history->push(std::make_unique<EditPathCommand>(
              obj, snap_data.before_path, std::move(after), "Move object"));
        }
      }
      // Text and Image node moves
      for (auto &tsnap : m_text_move_snaps) {
        double cur_x =
            tsnap.obj->is_image() ? tsnap.obj->image_x : tsnap.obj->text_x;
        double cur_y =
            tsnap.obj->is_image() ? tsnap.obj->image_y : tsnap.obj->text_y;
        if (std::abs(cur_x - tsnap.orig_x) > 0.001 ||
            std::abs(cur_y - tsnap.orig_y) > 0.001) {
          m_history->push(std::make_unique<MoveObjectCommand>(
              tsnap.obj, tsnap.orig_x, tsnap.orig_y, cur_x, cur_y));
        }
      }
      // Warp envelope moves — push EditWarpCommand per moved Warp.
      for (auto &wsnap : m_warp_env_move_snaps) {
        SceneNode *w = wsnap.warp;
        if (!w || !w->is_warp())
          continue;
        bool moved = false;
        if (!w->warp_env_top.nodes.empty() &&
            !wsnap.orig_env_top.nodes.empty()) {
          moved = std::abs(w->warp_env_top.nodes[0].x -
                           wsnap.orig_env_top.nodes[0].x) > 0.001 ||
                  std::abs(w->warp_env_top.nodes[0].y -
                           wsnap.orig_env_top.nodes[0].y) > 0.001;
        }
        if (moved) {
          m_history->push(std::make_unique<EditWarpCommand>(
              w, wsnap.orig_env_top, wsnap.orig_env_bottom, w->warp_quality,
              w->warp_env_top, w->warp_env_bottom, w->warp_quality));
        }
      }
    }
    // ── Record macro step ─────────────────────────────────────────────
    {
      // Compute delta from first moved path snap (or text/image snap)
      double rdx = 0.0, rdy = 0.0;
      bool found = false;
      for (auto &snap : m_move_snaps) {
        if (!snap.obj->path || snap.orig_nodes.empty() ||
            snap.obj->path->nodes.empty())
          continue;
        rdx = snap.obj->path->nodes[0].x - snap.orig_nodes[0].x;
        rdy = snap.obj->path->nodes[0].y - snap.orig_nodes[0].y;
        found = true;
        break;
      }
      if (!found) {
        for (auto &tsnap : m_text_move_snaps) {
          double cx =
              tsnap.obj->is_image() ? tsnap.obj->image_x : tsnap.obj->text_x;
          double cy =
              tsnap.obj->is_image() ? tsnap.obj->image_y : tsnap.obj->text_y;
          rdx = cx - tsnap.orig_x;
          rdy = cy - tsnap.orig_y;
          found = true;
          break;
        }
      }
      if (!found) {
        for (auto &rsnap : m_ref_move_snaps) {
          if (!rsnap.obj || !rsnap.obj->is_ref())
            continue;
          rdx = rsnap.obj->ref_x - rsnap.orig_x;
          rdy = rsnap.obj->ref_y - rsnap.orig_y;
          found = true;
          break;
        }
      }
      if (found && (std::abs(rdx) > 0.001 || std::abs(rdy) > 0.001)) {
        MacroStep step;
        step.op = MacroStep::Op::Move;
        step.dx = rdx;
        step.dy = rdy;
        record_step_if_recording(step);
      }
    }
    m_move_snaps.clear();
    m_text_move_snaps.clear();
    m_warp_env_move_snaps.clear();
    m_ref_move_snaps.clear();
    m_move_orig_nodes.clear();
    m_sig_doc_changed.emit();
    m_sig_selection.emit(m_selected);
    LOG_DEBUG("Move end ({} objects)", m_selection.size());
  }
}

// ── Input: motion
// ─────────────────────────────────────────────────────────────
void Canvas::on_motion(double x, double y) {
  m_mouse_x = x;
  m_mouse_y = y;
  double doc_x, doc_y;
  screen_to_doc(x, y, doc_x, doc_y);
  m_cursor_doc_x = doc_x;
  m_cursor_doc_y = doc_y;
  m_sig_cursor.emit(doc_x, doc_y);

  // Guide construct live preview — snap-aware.  Phase 1 follows mouse from
  // the captured p1.  Phase 2 is locked (dialog is open).  Phase 0 just
  // shows the cursor.
  if (m_guide_construct_active && m_guide_construct_phase == 1) {
    double sx = doc_x, sy = doc_y;
    const double tol = 8.0 / m_zoom;
    std::vector<std::pair<SceneNode *, int>> all_nodes;
    ruler_collect_all_path_nodes(all_nodes);
    double best_d = tol;
    for (auto &[obj, ni] : all_nodes) {
      const BezierNode &n = obj->path->nodes[ni];
      double d = std::hypot(n.x - doc_x, n.y - doc_y);
      if (d < best_d) {
        best_d = d;
        sx = n.x;
        sy = n.y;
      }
    }
    m_guide_construct_preview_x = sx;
    m_guide_construct_preview_y = sy;
    queue_draw();
  }

  // ── Guide hover hit-test — runs for all tools ─────────────────────────
  // Test within 5px screen tolerance. Update m_guide_hovered and cursor.
  // Only active when the guide layer is visually above (lower index = on top)
  // the current active layer — if guides are below other layers they are
  // obscured and should not grab the cursor.
  //
  // S66 — Phase 3: skip entirely for Eyedropper. Eyedropper doesn't
  // interact with guides, and the guide-hovered path early-returns out
  // of on_motion, which freezes the loupe buffer and the crosshair cursor
  // whenever the mouse passes within 5px of a guide — the "eyedropper
  // snaps to guide" bug.
  if (m_doc && !m_guide_drag_active && m_tool != ActiveTool::Eyedropper) {
    const SceneNode *gl = m_doc->guide_layer();
    SceneNode *prev_hovered = m_guide_hovered;
    m_guide_hovered = nullptr;

    // Find guide layer index (lower index = drawn on top)
    int guide_idx = -1;
    for (int i = 0; i < (int)m_doc->layers.size(); ++i)
      if (m_doc->layers[i]->is_guide_layer()) {
        guide_idx = i;
        break;
      }

    // Only hover-test when guide layer is on top of (index ≤) active layer
    bool guide_on_top =
        (guide_idx >= 0 && guide_idx <= m_doc->active_layer_index);

    if (gl && gl->visible && !gl->locked && guide_on_top) {
      static constexpr double GUIDE_HIT_PX = 5.0;
      const double ox = doc_origin_x();
      const double oy = doc_origin_y();
      for (auto &child : gl->children) {
        if (!child->is_guide())
          continue;
        double dist;
        if (child->guide_is_horizontal()) {
          double sy = child->guide_y * m_zoom + oy;
          dist = std::abs(y - sy);
        } else if (child->guide_is_vertical()) {
          double sx = child->guide_x * m_zoom + ox;
          dist = std::abs(x - sx);
        } else {
          // Angled guide — perpendicular distance from mouse point to the
          // infinite line through (guide_x, guide_y) at angle guide_angle.
          double ax_s = child->guide_x * m_zoom + ox;
          double ay_s = child->guide_y * m_zoom + oy;
          double a = child->guide_angle * M_PI / 180.0;
          double dxu = std::cos(a);
          double dyu = std::sin(a);
          // Perp distance = |(p - a) × dir| where dir is unit.
          double vx = x - ax_s;
          double vy = y - ay_s;
          dist = std::abs(vx * dyu - vy * dxu);
        }
        if (dist <= GUIDE_HIT_PX) {
          m_guide_hovered = child.get();
          break;
        }
      }
    }
    if (m_guide_hovered != prev_hovered)
      queue_draw();
    if (m_guide_hovered) {
      // Cursor hint: row/col-resize for axis-aligned, default "move" for
      // angled (no single axis meaningful).
      if (m_guide_hovered->guide_is_horizontal())
        set_cursor("row-resize");
      else if (m_guide_hovered->guide_is_vertical())
        set_cursor("col-resize");
      else
        set_cursor("move");
      return; // suppress other cursor logic
    }
    // In node mode, no guide hovered — ensure cursor stays default
    if (m_tool == ActiveTool::Node && prev_hovered && !m_guide_hovered)
      set_cursor("default");
  }

  // ── Eyedropper hover tracking
  // ─────────────────────────────────────────────────────
  if (m_tool == ActiveTool::Eyedropper) {
    // S66 — Phase 3: re-assert the crosshair cursor each tick so nothing
    // upstream (e.g. a future guide/handle/hit-test cursor hint) can leave
    // the cursor stuck on a non-crosshair icon while the eyedropper is
    // active.
    set_cursor("crosshair");
    SceneNode *prev_eye = m_eyedropper_hovered;
    m_eyedropper_hovered = hit_test(doc_x, doc_y);
    if (m_eyedropper_hovered) {
      m_eyedropper_hovered_color = m_mod_alt
                                       ? m_eyedropper_hovered->stroke.paint
                                       : m_eyedropper_hovered->fill;
    }
    // S66 — Phase 3: loupe is always-zoom. Refresh the buffer sample
    // every hover tick so the magnified view follows the cursor.
    refresh_loupe_buffer();
    if (m_eyedropper_hovered != prev_eye)
      queue_draw();
    queue_draw(); // always redraw so loupe follows cursor smoothly
    return;
  }

  // ── Selection tool: cursor changes on handle proximity ────────────────
  if (m_tool == ActiveTool::Selection) {
    // M4c-1: When primary selection is a Warp, bbox handles are gone —
    // don't run the bbox handle cursor resolver (it would otherwise hand
    // out stale nw-resize/grab cursors where bbox handles used to be).
    // Envelope handles draw themselves; cursor stays default over them.
    if (m_selected && m_selected->is_warp()) {
      set_cursor("default");
    } else if (m_handle_drag != HandleKind::None) {
      // During an active edge-mid drag: swap to grab once skew is locked
      bool is_edge_mid =
          (m_handle_drag == HandleKind::N || m_handle_drag == HandleKind::S ||
           m_handle_drag == HandleKind::E || m_handle_drag == HandleKind::W);
      if (is_edge_mid && m_skew_intent_locked && m_skew_is_skew)
        set_cursor("grab");
    } else {
      HandleKind hk = handle_hit_test(x, y);
      if (m_r_held) {
        // Pivot mode cursors:
        // Near pivot crosshair → move cursor (can drag pivot)
        // Near corner handles → grab cursor (can rotate)
        // Elsewhere → crosshair (click sets new pivot)
        bool near_pivot = false;
        if (m_has_custom_pivot) {
          double pvsx, pvsy;
          doc_to_screen(m_custom_pivot_x, m_custom_pivot_y, pvsx, pvsy);
          near_pivot = std::hypot(x - pvsx, y - pvsy) <= 12.0;
        }
        bool is_corner =
            (hk == HandleKind::NW || hk == HandleKind::NE ||
             hk == HandleKind::SE || hk == HandleKind::SW ||
             hk == HandleKind::RotateNW || hk == HandleKind::RotateNE ||
             hk == HandleKind::RotateSE || hk == HandleKind::RotateSW);
        if (near_pivot)
          set_cursor("move");
        else if (is_corner)
          set_cursor("grab");
        else
          set_cursor("crosshair");
      } else {
        switch (hk) {
        case HandleKind::NW:
        case HandleKind::SE:
          set_cursor("nw-resize");
          break;
        case HandleKind::NE:
        case HandleKind::SW:
          set_cursor("ne-resize");
          break;
        case HandleKind::N:
        case HandleKind::S:
          set_cursor("n-resize");
          break;
        case HandleKind::E:
        case HandleKind::W:
          set_cursor("e-resize");
          break;
        case HandleKind::RotateNW:
        case HandleKind::RotateNE:
        case HandleKind::RotateSE:
        case HandleKind::RotateSW:
          set_cursor("grab");
          break;
        default:
          set_cursor("default");
          break;
        }
      }
    }
  }
  if (m_tool == ActiveTool::Pen) {
    PenModifiers mods;
    mods.alt = m_mod_alt;
    mods.shift = m_mod_shift;
    // Snap motion to nearest node on any existing path (not WIP)
    // Only snap when starting a new stroke (no WIP) to avoid
    // the rubber-band locking to the path being drawn.
    double pen_snap_x = doc_x, pen_snap_y = doc_y;
    if (!m_pen_tool.has_wip && m_doc) {
      static constexpr double NODE_SNAP_PX = 6.0;
      double best_d2 = NODE_SNAP_PX * NODE_SNAP_PX;
      for (auto &layer : m_doc->layers) {
        if (!layer->visible || layer->locked || layer->is_special_layer())
          continue;
        for (auto &obj_uptr : layer->children) {
          SceneNode &obj = *obj_uptr;
          if (obj.type != SceneNode::Type::Path || !obj.path)
            continue;
          for (const auto &nd : obj.path->nodes) {
            double sx, sy;
            doc_to_screen(nd.x, nd.y, sx, sy);
            double ddx = x - sx, ddy = y - sy;
            double d2 = ddx * ddx + ddy * ddy;
            if (d2 < best_d2) {
              best_d2 = d2;
              pen_snap_x = nd.x;
              pen_snap_y = nd.y;
            }
          }
        }
      }
      // Guide snap — only if node snap didn't already lock
      if (pen_snap_x == doc_x)
        pen_snap_x = snap_x(doc_x);
      if (pen_snap_y == doc_y)
        pen_snap_y = snap_y(doc_y);
    }
    m_pen_tool.on_motion({pen_snap_x, pen_snap_y}, mods, m_zoom);
    queue_draw();
  } else if (m_tool == ActiveTool::Line && m_line_tool.active()) {
    double ex = snap(doc_x), ey = snap(doc_y);
    // 15° angle snap when Shift held
    if (m_mod_shift && !m_line_tool.points.empty()) {
      auto [lx, ly] = m_line_tool.points.back();
      double dw = ex - lx, dh = ey - ly;
      double len = std::hypot(dw, dh);
      if (len > 0.001) {
        double angle = std::atan2(dh, dw);
        double snapped = std::round(angle / (M_PI / 12.0)) * (M_PI / 12.0);
        ex = lx + len * std::cos(snapped);
        ey = ly + len * std::sin(snapped);
      }
    }
    // Close-snap check
    double tol = 8.0 / m_zoom;
    auto [sx, sy] = m_line_tool.points[0];
    m_line_tool.close_snap = (std::hypot(ex - sx, ey - sy) <= tol);
    if (m_line_tool.close_snap) {
      ex = sx;
      ey = sy;
    }
    m_line_tool.live_x = ex;
    m_line_tool.live_y = ey;
    queue_draw();
  } else if (m_tool == ActiveTool::Zoom) {
    // Flip cursor and notify toolbar when Alt state changes
    set_cursor(m_mod_alt ? "zoom-out" : "zoom-in");
    if (m_mod_alt != m_zoom_alt_prev) {
      m_zoom_alt_prev = m_mod_alt;
      m_sig_zoom_alt.emit(m_mod_alt);
    }
  } else if (m_tool == ActiveTool::Ref && m_doc) {
    // Hover hit-test for ref points in the RefLayer
    SceneNode *prev_hovered = m_ref_hovered;
    m_ref_hovered = nullptr;
    SceneNode *rl = m_doc->ref_layer();
    if (rl && !rl->locked && rl->visible) {
      for (auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        double sx, sy;
        doc_to_screen(child->ref_x, child->ref_y, sx, sy);
        if (std::hypot(x - sx, y - sy) <= 8.0) {
          m_ref_hovered = child.get();
          break;
        }
      }
    }
    set_cursor(m_ref_hovered ? "move" : "crosshair");
    queue_draw();
  }
}

// ── Pen tool click (via drag with tiny delta treated as click)
// ────────────────
void Canvas::on_pen_begin(double x, double y) {
  double dx, dy;
  screen_to_doc(x, y, dx, dy);
  m_draw_start_dx = dx;
  m_draw_start_dy = dy;
  // Update modifier state before press
  m_pen_tool.mods.alt = m_mod_alt;
  m_pen_tool.mods.shift = m_mod_shift;

  // ── Continue path ─────────────────────────────────────────────────────
  // If no WIP is active and the click lands on the tail or head of an
  // existing open path, load that path and continue drawing from that end.
  if (!m_pen_tool.has_wip && m_doc) {
    double tol = PenTool::CLOSE_RADIUS_PX / m_zoom;
    Vec2 click{dx, dy};
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        SceneNode &obj = *obj_uptr;
        if (obj.type != SceneNode::Type::Path || !obj.path)
          continue;
        if (obj.path->closed)
          continue;
        if (obj.path->nodes.size() < 2)
          continue;

        BezierNode &head = obj.path->nodes.front();
        BezierNode &tail = obj.path->nodes.back();
        Vec2 head_pos{head.x, head.y};
        Vec2 tail_pos{tail.x, tail.y};

        bool near_tail = click.dist(tail_pos) <= tol;
        bool near_head = click.dist(head_pos) <= tol;

        if (!near_tail && !near_head)
          continue;

        // Save before-state for undo (we'll replace the object's path)
        PathData before = *obj.path;
        m_continue_target = &obj;
        m_continue_before = before;

        // Load path into WIP
        BezierPath bp = BezierPath::from_path_data(*obj.path);

        if (near_head && !near_tail) {
          // Continuing from head — reverse so tail is the open end
          bp.reverse();
        }

        m_pen_tool.wip = bp;
        m_pen_tool.has_wip = true;
        m_pen_tool.state = PenTool::State::Placing;
        m_pen_tool.drag_node_idx = (int)bp.nodes.size() - 1;
        m_pen_tool.live_in_valid = false;

        // Remove the object from the document — it will be re-committed
        // with the extra nodes when the pen path is finished
        for (auto &l2 : m_doc->layers) {
          auto it = std::find_if(l2->children.begin(), l2->children.end(),
                                 [&obj](const std::unique_ptr<SceneNode> &o) {
                                   return o.get() == &obj;
                                 });
          if (it != l2->children.end()) {
            l2->children.erase(it);
            break;
          }
        }
        m_selected = nullptr;
        m_sig_selection.emit(nullptr);
        m_sig_doc_changed.emit();
        queue_draw();
        LOG_INFO("PenTool: continuing path from {}",
                 near_tail ? "tail" : "head");
        return; // don't fall through to on_press
      }
    }
  }

  // ── Pen node snap ─────────────────────────────────────────────────────
  // Snap click to nearest node within NODE_SNAP_PX screen pixels.
  // Comparison in screen space so tolerance is zoom-independent.
  if (!m_pen_tool.has_wip && m_doc) {
    static constexpr double NODE_SNAP_PX = 6.0;
    double best_d2 = NODE_SNAP_PX * NODE_SNAP_PX;
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        SceneNode &obj = *obj_uptr;
        if (obj.type != SceneNode::Type::Path || !obj.path)
          continue;
        for (const auto &nd : obj.path->nodes) {
          double sx, sy;
          doc_to_screen(nd.x, nd.y, sx, sy);
          double ddx = x - sx, ddy = y - sy;
          double d2 = ddx * ddx + ddy * ddy;
          if (d2 < best_d2) {
            best_d2 = d2;
            dx = nd.x;
            dy = nd.y;
          }
        }
      }
    }
  }

  bool done = m_pen_tool.on_press({dx, dy}, m_zoom);
  if (done) {
    // Close detected — don't commit yet, allow drag to set node 0 in-handle
    m_pen_closing = true;
  }
  queue_draw();
}

void Canvas::place_ref_at_display(double ux, double uy) {
  if (!m_doc)
    return;
  double doc_x = ux + m_doc->ruler_origin_x;
  double doc_y = m_doc->canvas_height() - (uy + m_doc->ruler_origin_y);
  double rx = snap(doc_x), ry = snap(doc_y);

  SceneNode *rl = m_doc->ensure_ref_layer();

  char name_buf[64];
  snprintf(name_buf, sizeof(name_buf), "%.6f_%.6f", rx, ry);

  auto ref = std::make_unique<SceneNode>();
  ref->type = SceneNode::Type::Ref;
  ref->id = next_id();
  ref->name = name_buf;
  ref->ref_x = rx;
  ref->ref_y = ry;

  rl->children.insert(rl->children.begin(), clone_node(*ref));
  m_selected = rl->children.front().get();

  if (m_history)
    m_history->push(std::make_unique<AddNodeCommand>(
        rl, clone_node(*rl->children.front())));

  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Ref placed via popover at doc ({:.4f},{:.4f})", rx, ry);
}

// ── Precise placement helpers (Ctrl+click popovers)
// ─────────────────────────── All coordinates arrive in display space (Y-up,
// ruler-origin-relative). Convert to doc space (Y-down, absolute) before
// placing.

void Canvas::place_rect_precise(double ux, double uy, double w, double h) {
  if (!m_doc || w < 0.001 || h < 0.001)
    return;
  // ux,uy = top-left corner in display space (Y-up)
  // Doc space: x = ux + ruler_ox, y_bottom = canvas_h - (uy + ruler_oy)
  // y_top (doc, Y-down) = y_bottom - h
  double doc_x = ux + m_doc->ruler_origin_x;
  double doc_yb = m_doc->canvas_height() - (uy + m_doc->ruler_origin_y);
  double doc_y = doc_yb - h; // top-left corner in Y-down space

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Rectangle);
  obj.type = SceneNode::Type::Path;
  obj.path = std::make_unique<PathData>(rect_to_path(doc_x, doc_y, w, h));
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke = m_def_stroke;
  });

  place_shape_node(std::move(obj));
  LOG_INFO("Rect placed via popover: x={:.2f} y={:.2f} w={:.2f} h={:.2f}",
           doc_x, doc_y, w, h);
}

void Canvas::place_ellipse_precise(double ucx, double ucy, double rx,
                                   double ry) {
  if (!m_doc || rx < 0.001 || ry < 0.001)
    return;
  double doc_cx = ucx + m_doc->ruler_origin_x;
  double doc_cy = m_doc->canvas_height() - (ucy + m_doc->ruler_origin_y);

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Ellipse);
  obj.type = SceneNode::Type::Path;
  obj.path =
      std::make_unique<PathData>(ellipse_to_path(doc_cx, doc_cy, rx, ry));
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke = m_def_stroke;
  });

  place_shape_node(std::move(obj));
  LOG_INFO(
      "Ellipse placed via popover: cx={:.2f} cy={:.2f} rx={:.2f} ry={:.2f}",
      doc_cx, doc_cy, rx, ry);
}

void Canvas::place_polygon_precise(double cx, double cy, double radius,
                                   int sides, double inflection,
                                   double angle_rad) {
  if (!m_doc || radius < 0.001 || sides < 3)
    return;
  double doc_cx = cx + m_doc->ruler_origin_x;
  double doc_cy = m_doc->canvas_height() - (cy + m_doc->ruler_origin_y);

  // Snap inflection
  double perfect_star =
      (sides >= 5) ? std::cos(2.0 * M_PI / sides) / std::cos(M_PI / sides)
                   : -1.0;
  if (perfect_star > 0.0 && std::abs(inflection - perfect_star) < 0.04)
    inflection = perfect_star;
  if (inflection > 0.985)
    inflection = 1.0;

  m_poly_sides = sides;
  m_poly_inflection = inflection;

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = (inflection >= 1.0)
                 ? m_doc->next_default_name(CurvzDocument::NameKind::Polygon)
                 : m_doc->next_default_name(CurvzDocument::NameKind::Star);
  obj.type = SceneNode::Type::Path;
  obj.path = std::make_unique<PathData>(
      polygon_to_path(doc_cx, doc_cy, radius, sides, inflection, angle_rad));
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke = m_def_stroke;
  });

  place_shape_node(std::move(obj));
  LOG_INFO("Polygon placed via popover: cx={:.2f} cy={:.2f} r={:.2f} sides={} "
           "inflect={:.3f}",
           doc_cx, doc_cy, radius, sides, inflection);
}

void Canvas::place_spiral_precise(double cx, double cy, double outer_r,
                                  double inner_r, double turns,
                                  double angle_rad) {
  if (!m_doc || outer_r < 0.001)
    return;
  double doc_cx = cx + m_doc->ruler_origin_x;
  double doc_cy = m_doc->canvas_height() - (cy + m_doc->ruler_origin_y);

  m_spiral_turns = turns;
  m_spiral_inner = (outer_r > 0.0) ? (inner_r / outer_r * 100.0) : 0.0;

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Spiral);
  obj.type = SceneNode::Type::Path;
  obj.path = std::make_unique<PathData>(spiral_to_path(
      doc_cx, doc_cy, outer_r, inner_r, turns, angle_rad));
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke = m_def_stroke;
  });

  place_shape_node(std::move(obj));
  LOG_INFO("Spiral placed via popover: cx={:.2f} cy={:.2f} r={:.2f} "
           "turns={:.1f}",
           doc_cx, doc_cy, outer_r, turns);
}

void Canvas::place_line_precise(double ux1, double uy1, double ux2,
                                double uy2) {
  if (!m_doc)
    return;
  double doc_x1 = ux1 + m_doc->ruler_origin_x;
  double doc_y1 = m_doc->canvas_height() - (uy1 + m_doc->ruler_origin_y);
  double doc_x2 = ux2 + m_doc->ruler_origin_x;
  double doc_y2 = m_doc->canvas_height() - (uy2 + m_doc->ruler_origin_y);

  PathData pd;
  pd.closed = false;
  for (auto [px, py] : std::initializer_list<std::pair<double, double>>{
           {doc_x1, doc_y1}, {doc_x2, doc_y2}}) {
    BezierNode n;
    n.x = px;
    n.y = py;
    n.cx1 = px;
    n.cy1 = py;
    n.cx2 = px;
    n.cy2 = py;
    n.type = BezierNode::Type::Corner;
    pd.nodes.push_back(n);
  }

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Line);
  obj.type = SceneNode::Type::Path;
  obj.path = std::make_unique<PathData>(std::move(pd));
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke = m_def_stroke;
  });

  place_shape_node(std::move(obj));
  LOG_INFO("Line placed via popover: ({:.2f},{:.2f})→({:.2f},{:.2f})", doc_x1,
           doc_y1, doc_x2, doc_y2);
}

void Canvas::place_text_precise(double ux, double uy, const std::string &family,
                                double size, bool bold, bool italic,
                                const std::string &anchor,
                                const std::string &align) {
  if (!m_doc)
    return;
  // Convert display (user) space → doc space (Y-down Cairo coords)
  double doc_x = ux + m_doc->ruler_origin_x;
  double doc_y = m_doc->canvas_height() - (uy + m_doc->ruler_origin_y);

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = generate_internal_id();
  obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Text);
  obj.type = SceneNode::Type::Text;
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke.paint.type = FillStyle::Type::None;
  });
  obj.text_x = doc_x;
  obj.text_y = doc_y;
  obj.text_font_family = family.empty() ? "Sans" : family;
  obj.text_font_size = size > 0 ? size : 24.0;
  obj.text_bold = bold;
  obj.text_italic = italic;
  obj.text_anchor = anchor.empty() ? "start" : anchor;
  obj.text_align = align.empty() ? "left" : align;
  obj.text_content = ""; // user types content via the floating entry

  // Insert into scene first so on_text_begin can find and edit it
  SceneNode *layer = m_doc->active_layer();
  if (!layer)
    layer = m_doc->layers[0].get();
  layer->children.insert(layer->children.begin(),
                         std::make_unique<SceneNode>(std::move(obj)));
  SceneNode *inserted = layer->children.front().get();
  m_selected = inserted;
  m_selection = {inserted};

  // Switch to Text tool and open the inline editor on the new node
  m_sig_request_tool.emit(ActiveTool::Text);
  Glib::signal_idle().connect_once([this, inserted]() {
    m_text_editing = inserted;
    m_text_is_new = true;
    if (m_text_entry) {
      m_text_entry_conn_activate.disconnect();
      m_text_entry_conn_changed.disconnect();
      m_text_entry->set_text("");
      m_text_entry->set_visible(true);
      m_text_entry->grab_focus();
      position_text_entry();
      m_text_entry_conn_changed =
          m_text_entry->signal_changed().connect([this]() {
            if (m_text_editing) {
              m_text_editing->text_content = m_text_entry->get_text();
              queue_draw();
            }
          });
      m_text_entry_conn_activate = m_text_entry->signal_activate().connect(
          [this]() { commit_text_edit(); });
    }
    m_sig_selection.emit(inserted);
    m_sig_doc_changed.emit();
    queue_draw();
  });
  LOG_INFO("Text placed via popover at ({:.2f},{:.2f}) family={} size={:.1f}",
           doc_x, doc_y, family, size);
}

// Shared: insert a new shape node at the front of the active layer with undo
void Canvas::place_shape_node(SceneNode obj) {
  if (!m_doc)
    return;
  SceneNode *layer = m_doc->active_layer();
  if (!layer)
    layer = m_doc->layers[0].get();
  layer->children.insert(layer->children.begin(), clone_node(obj));
  m_selected = layer->children.front().get();
  if (m_history)
    m_history->push(
        std::make_unique<AddNodeCommand>(layer, clone_node(*m_selected)));
  m_sig_selection.emit(m_selected);
  m_sig_request_tool.emit(ActiveTool::Selection);
  m_sig_doc_changed.emit();
  queue_draw();
}

// ── import_svg_to_canvas
// ─────────────────────────────────────────────────────── Loads an SVG from
// disk, collects all visible objects, centres them on the current viewport, and
// inserts them into the active layer with undo support.
void Canvas::import_svg_to_canvas(const std::string &path) {
  if (!m_doc)
    return;

  auto src_doc = parse_svg_file(path);
  if (!src_doc) {
    LOG_ERROR("import_svg_to_canvas: failed to parse '{}'", path);
    return;
  }

  SceneNode *target_layer = m_doc->active_layer();
  if (!target_layer)
    target_layer = m_doc->layers[0].get();

  // Collect all visible, non-guide, non-ref objects from the imported doc
  std::vector<std::unique_ptr<SceneNode>> imported;
  for (auto &layer_uptr : src_doc->layers) {
    if (!layer_uptr->visible)
      continue;
    if (layer_uptr->is_special_layer())
      continue;
    for (auto &child : layer_uptr->children) {
      if (!child->visible)
        continue;
      imported.push_back(clone_node(*child));
    }
  }

  if (imported.empty()) {
    LOG_WARN("import_svg_to_canvas: no visible objects in '{}'", path);
    return;
  }

  // Freshen IDs so imported nodes don't collide with existing scene
  int id_counter = s_next_id;
  for (auto &node : imported)
    freshen_ids(node.get(), m_doc, id_counter);
  s_next_id = id_counter;

  // Compute bounding box of all imported objects
  double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
  for (auto &node : imported) {
    auto bb = object_bbox(*node, false);
    if (!bb)
      continue;
    bx1 = std::min(bx1, bb->x);
    by1 = std::min(by1, bb->y);
    bx2 = std::max(bx2, bb->x + bb->w);
    by2 = std::max(by2, bb->y + bb->h);
  }

  // Centre on current viewport
  double vp_cx, vp_cy;
  screen_to_doc(get_width() * 0.5, get_height() * 0.5, vp_cx, vp_cy);

  double src_cx = (bx1 + bx2) * 0.5;
  double src_cy = (by1 + by2) * 0.5;
  double dx = vp_cx - src_cx;
  double dy = vp_cy - src_cy;

  if (std::abs(dx) > 0.01 || std::abs(dy) > 0.01) {
    for (auto &node : imported) {
      std::vector<SceneNode *> paths;
      collect_paths(node.get(), paths);
      for (SceneNode *p : paths) {
        if (!p->path)
          continue;
        for (auto &n : p->path->nodes) {
          n.x += dx;
          n.y += dy;
          n.cx1 += dx;
          n.cy1 += dy;
          n.cx2 += dx;
          n.cy2 += dy;
        }
      }
    }
  }

  // Build undo command snapshots and insert into target layer
  std::vector<SceneNode *> new_sel;
  for (int i = (int)imported.size() - 1; i >= 0; --i) {
    auto snap = clone_node(*imported[i]);
    target_layer->children.insert(target_layer->children.begin(),
                                  std::move(imported[i]));
    SceneNode *inserted = target_layer->children.front().get();
    new_sel.push_back(inserted);
    if (m_history)
      m_history->push(
          std::make_unique<AddNodeCommand>(target_layer, std::move(snap)));
  }

  // Update selection
  m_selection.clear();
  for (SceneNode *n : new_sel)
    m_selection.push_back(n);
  m_selected = m_selection.empty() ? nullptr : m_selection.front();

  m_sig_selection.emit(m_selected);
  m_sig_request_tool.emit(ActiveTool::Selection);
  m_sig_doc_changed.emit();
  queue_draw();

  LOG_INFO("import_svg_to_canvas: placed {} object(s) from '{}'",
           new_sel.size(), path);
}

// ── import_image_to_canvas
// ──────────────────────────────────────────────────── Places a raster image
// (PNG/JPG/GIF/WebP) as an Image node.
//
// fit_canvas_to_image=false (default): scales to ≤80% of the current canvas
//   preserving aspect ratio, centres in viewport, leaves the canvas size
//   unchanged. The original behaviour, used for "drop a reference into an
//   existing doc" workflows.
//
// fit_canvas_to_image=true (s125 m1c): resizes the document's canvas to
//   match the image's natural pixel dimensions, places the image at (0, 0)
//   at 1:1 pixel mapping. Used by File → Place Image as Document for the
//   manual-screenshot annotation workflow. The canvas resize is NOT
//   currently undoable — matches the rest of the canvas-resize paths in
//   the app (PropertiesPanel canvas inspector, NewDocument flow). Backlog
//   item to wire a CanvasResizeCommand and route all three through it.
void Canvas::import_image_to_canvas(const std::string &path,
                                    bool fit_canvas_to_image) {
  if (!m_doc)
    return;

  // Determine natural pixel dimensions via the shared image_meta helper —
  // single seam for both this path and the right-click Image Info dialog.
  ImageMeta meta = read_image_meta(path);
  if (!meta.valid) {
    LOG_ERROR("import_image_to_canvas: could not read dimensions of '{}'",
              path);
    return;
  }
  const int img_w = meta.width;
  const int img_h = meta.height;

  double doc_w = 0.0, doc_h = 0.0;
  double doc_x = 0.0, doc_y = 0.0;

  if (fit_canvas_to_image) {
    // Resize the document canvas to match the image's pixel dimensions
    // exactly. Image goes at (0, 0) at 1:1.
    m_doc->canvas = CanvasModel::from_pixels(img_w, img_h);
    doc_w = static_cast<double>(img_w);
    doc_h = static_cast<double>(img_h);
    doc_x = 0.0;
    doc_y = 0.0;
  } else {
    // Existing behaviour: scale to ≤80% of canvas, centre.
    int cw = m_doc->canvas_width();
    int ch = m_doc->canvas_height();
    double max_w = cw * 0.8;
    double max_h = ch * 0.8;
    double scale = std::min(max_w / img_w, max_h / img_h);
    if (scale > 1.0)
      scale = 1.0; // don't upscale beyond natural size
    doc_w = img_w * scale;
    doc_h = img_h * scale;
    // Centre in doc space (Y-down)
    doc_x = (cw - doc_w) * 0.5;
    doc_y = (ch - doc_h) * 0.5;
  }

  SceneNode *layer = m_doc->active_layer();
  if (!layer)
    layer = m_doc->layers[0].get();

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = std::filesystem::path(path).stem().string();
  obj.type = SceneNode::Type::Image;
  obj.image_path = path;
  obj.image_x = doc_x;
  obj.image_y = doc_y;
  obj.image_w = doc_w;
  obj.image_h = doc_h;

  layer->children.insert(layer->children.begin(), clone_node(obj));
  m_selected = layer->children.front().get();
  if (m_history)
    m_history->push(
        std::make_unique<AddNodeCommand>(layer, clone_node(*m_selected)));

  m_selection = {m_selected};
  m_sig_selection.emit(m_selected);
  m_sig_request_tool.emit(ActiveTool::Selection);

  // Re-fit viewport when the canvas itself changed size, so the new
  // image-sized canvas lands centred and visible. Matches the
  // PropertiesPanel canvas-inspector handler at MainWindow.cpp:2076.
  if (fit_canvas_to_image)
    zoom_fit();

  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO(
      "import_image_to_canvas: '{}' {}x{} → {:.1f}x{:.1f} at ({:.1f},{:.1f}) "
      "fit_canvas={}",
      path, img_w, img_h, doc_w, doc_h, doc_x, doc_y, fit_canvas_to_image);
}

// ── flip_selection
// ──────────────────────────────────────────────────────────── Flips all
// selected objects horizontally or vertically around the union BBX centre.
// Paths have their node coordinates negated; images get a scale(-1,1) or
// scale(1,-1) composed into their transform.
void Canvas::flip_selection(bool horizontal) {
  if (m_selection.empty() || !m_doc)
    return;

  // Compute union bounding box of selection
  double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
  for (SceneNode *obj : m_selection) {
    auto bb = object_bbox(*obj, false);
    if (!bb)
      continue;
    bx1 = std::min(bx1, bb->x);
    by1 = std::min(by1, bb->y);
    bx2 = std::max(bx2, bb->x + bb->w);
    by2 = std::max(by2, bb->y + bb->h);
  }
  if (bx1 > bx2)
    return;
  double cx = (bx1 + bx2) * 0.5;
  double cy = (by1 + by2) * 0.5;

  std::vector<ScaleObjectsCommand::LeafSnap> path_snaps;
  std::vector<ScaleImageCommand::Snap> img_snaps;

  for (SceneNode *obj : m_selection) {
    if (obj->is_image()) {
      // Save before state
      Transform bef = obj->transform;
      double bef_x = obj->image_x, bef_y = obj->image_y;
      double bef_w = obj->image_w, bef_h = obj->image_h;

      // Image centre
      double icx = obj->image_x + obj->image_w * 0.5;
      double icy = obj->image_y + obj->image_h * 0.5;

      if (horizontal) {
        // Flip image centre across vertical axis through cx
        obj->image_x = 2.0 * cx - icx - obj->image_w * 0.5;
        // Compose scale(-1,1) into transform: negate column 0
        obj->transform.a = -obj->transform.a;
        obj->transform.b = -obj->transform.b;
      } else {
        // Flip image centre across horizontal axis through cy
        obj->image_y = 2.0 * cy - icy - obj->image_h * 0.5;
        // Compose scale(1,-1) into transform: negate column 1
        obj->transform.c = -obj->transform.c;
        obj->transform.d = -obj->transform.d;
      }

      img_snaps.push_back({obj, bef_x, bef_y, bef_w, bef_h, bef, obj->image_x,
                           obj->image_y, obj->image_w, obj->image_h,
                           obj->transform});
      continue;
    }

    // Text — reflect anchor point around centre
    if (obj->is_text()) {
      double bef_x = obj->text_x, bef_y = obj->text_y;
      if (horizontal)
        obj->text_x = 2.0 * cx - obj->text_x;
      else
        obj->text_y = 2.0 * cy - obj->text_y;
      if (m_history)
        m_history->push(std::make_unique<MoveObjectCommand>(
            obj, bef_x, bef_y, obj->text_x, obj->text_y));
      continue;
    }

    // Path / group / compound — collect leaves
    std::vector<SceneNode *> leaves;
    collect_paths(obj, leaves);
    for (SceneNode *leaf : leaves) {
      if (!leaf->path || leaf->path->nodes.empty())
        continue;
      PathData before = *leaf->path;
      for (auto &n : leaf->path->nodes) {
        if (horizontal) {
          n.x = 2.0 * cx - n.x;
          n.cx1 = 2.0 * cx - n.cx1;
          n.cx2 = 2.0 * cx - n.cx2;
        } else {
          n.y = 2.0 * cy - n.y;
          n.cy1 = 2.0 * cy - n.cy1;
          n.cy2 = 2.0 * cy - n.cy2;
        }
      }
      path_snaps.push_back({leaf, before, *leaf->path});
    }
  }

  if (m_history) {
    std::string desc = horizontal ? "Flip horizontal" : "Flip vertical";
    if (!path_snaps.empty())
      m_history->push(
          std::make_unique<ScaleObjectsCommand>(std::move(path_snaps), desc));
    if (!img_snaps.empty())
      m_history->push(
          std::make_unique<ScaleImageCommand>(std::move(img_snaps), desc));
  }

  m_sig_doc_changed.emit();
  queue_draw();
  {
    MacroStep s;
    s.op = horizontal ? MacroStep::Op::FlipH : MacroStep::Op::FlipV;
    record_step_if_recording(s);
  }
  LOG_INFO("Canvas: flip_{}", horizontal ? "horizontal" : "vertical");
}

// ── Macro playback helpers
// ────────────────────────────────────────────────────

// Parse "#RRGGBB" → r,g,b in [0,1].  Returns false if unparseable.
static bool parse_hex_color(const std::string &hex, double &r, double &g,
                            double &b) {
  if (hex.size() != 7 || hex[0] != '#')
    return false;
  auto h2 = [&](int pos) {
    auto c = [](char ch) -> int {
      if (ch >= '0' && ch <= '9')
        return ch - '0';
      if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
      if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
      return 0;
    };
    return c(hex[pos]) * 16 + c(hex[pos + 1]);
  };
  r = h2(1) / 255.0;
  g = h2(3) / 255.0;
  b = h2(5) / 255.0;
  return true;
}

void Canvas::apply_fill_to_selection(const std::string &hex) {
  if (m_selection.empty() || !m_doc)
    return;
  for (SceneNode *obj : m_selection) {
    FillStyle fs;
    double r, g, b;
    if (hex.empty()) {
      fs.type = FillStyle::Type::CurrentColor;
    } else if (parse_hex_color(hex, r, g, b)) {
      fs.type = FillStyle::Type::Solid;
      fs.r = r;
      fs.g = g;
      fs.b = b;
    }
    style::mutate_appearance(*obj, [&](SceneNode& n) {
      n.fill = fs;
    });
  }
  m_sig_doc_changed.emit();
  queue_draw();
}

void Canvas::apply_stroke_to_selection(const std::string &hex) {
  if (m_selection.empty() || !m_doc)
    return;
  for (SceneNode *obj : m_selection) {
    double r, g, b;
    style::mutate_appearance(*obj, [&](SceneNode& n) {
      if (hex.empty()) {
        n.stroke.paint.type = FillStyle::Type::None;
      } else if (parse_hex_color(hex, r, g, b)) {
        n.stroke.paint.type = FillStyle::Type::Solid;
        n.stroke.paint.r = r;
        n.stroke.paint.g = g;
        n.stroke.paint.b = b;
      }
    });
  }
  m_sig_doc_changed.emit();
  queue_draw();
}

// S90 Stage 1 — debug-only. Builds a 3-stop linear gradient (red → yellow
// → blue, horizontal in objectBoundingBox space) and assigns it to every
// selected shape's fill. Bypasses style::mutate_appearance because there's
// no undo or swatch tracking yet — Stage 1 is purely "prove the render
// path." Removed once the gradient editor lands.
void Canvas::debug_apply_test_gradient() {
  if (m_selection.empty() || !m_doc) {
    LOG_WARN("Canvas: debug_apply_test_gradient — nothing selected");
    return;
  }
  FillStyle g;
  g.type = FillStyle::Type::LinearGradient;
  g.g_x1 = 0.0; g.g_y1 = 0.5;
  g.g_x2 = 1.0; g.g_y2 = 0.5;
  g.stops = {
    { 0.0,  1.0, 0.10, 0.10, 1.0 },  // red
    { 0.5,  1.0, 0.85, 0.10, 1.0 },  // yellow
    { 1.0,  0.10, 0.30, 1.00, 1.0 }, // blue
  };
  for (SceneNode *obj : m_selection) {
    obj->fill = g;
  }
  LOG_INFO("Canvas: debug test gradient applied to {} object(s)",
           m_selection.size());
  m_sig_doc_changed.emit();
  queue_draw();
}

// ── Swatch apply (Phase 5 M3) ────────────────────────────────────────────────
// Routes through library->set_paint for each selected object. Today this
// writes the resolved colour into the object's FillStyle and fires the
// library's paint-changed signal so the SwatchesPanel's active-paint ring
// refreshes. An upcoming milestone adds fill_swatch_id / stroke_swatch_id
// fields to SceneNode; set_paint will also write those, at which point
// this path carries full binding identity through to save/load.
//
// Until then, this method behaves identically to apply_fill_to_selection /
// apply_stroke_to_selection from the user's perspective — the difference
// is plumbing-only. We keep the separate call site because the future
// binding work piggybacks on it without needing to rewire callers again.
//
// Behaviour if the library isn't wired: log a warning and no-op. We don't
// silently fall back to a raw colour apply — a missing library is an
// integration bug and we'd rather surface it.
void Canvas::apply_swatch_to_selection(const color::SwatchId &swatch_id,
                                       color::PaintSlot slot) {
  if (m_selection.empty() || !m_doc)
    return;

  if (!m_swatch_library) {
    LOG_WARN("Canvas::apply_swatch_to_selection: no swatch library wired; "
             "falling back to raw colour apply (binding will be lost)");
    const color::Swatch *sw = nullptr;
    // Can't look it up — no library. Nothing to fall back to. Give up.
    (void)sw;
    return;
  }

  const color::Swatch *sw = m_swatch_library->find_swatch(swatch_id);
  if (!sw) {
    LOG_WARN("Canvas::apply_swatch_to_selection: swatch '{}' not found",
             swatch_id);
    return;
  }

  // Fallback colour for the SwatchRef. The library will overwrite this on
  // its set_paint call (resolving through the library), but we pre-seed
  // with the current resolved colour so the ref is well-formed even if
  // some edge case skipped the library's own refresh.
  color::Color fallback{};
  if (const auto *solid = std::get_if<color::SolidSwatch>(sw)) {
    fallback = solid->color;
  }
  // Future gradient swatches would resolve to a representative colour
  // here — Phase 4.5 territory. For M3 only solid swatches exist.

  color::Paint ref = color::SwatchRef{swatch_id, fallback};

  // S82 m4g: push BindSwatchCommand for proper undo. Mutate-then-push
  // pattern (matches every other history-pushing site in this file):
  // capture pre-bind state per target, route the actual mutation
  // through set_paint (so cache + signal emission match production
  // semantics), then read post-bind state for the after snapshot.
  // ONE atomic command across the whole selection — Ctrl+Z restores
  // every target in a single step.
  std::vector<BindSwatchCommand::TargetSnap> snaps;
  snaps.reserve(m_selection.size());
  for (SceneNode *obj : m_selection) {
    if (!obj)
      continue;
    BindSwatchCommand::TargetSnap ts;
    ts.obj                     = obj;
    ts.bound_style_before      = obj->bound_style;
    ts.fill_before             = obj->fill;
    ts.fill_swatch_id_before   = obj->fill_swatch_id;
    ts.stroke_before           = obj->stroke;
    ts.stroke_swatch_id_before = obj->stroke_swatch_id;
    // set_paint funnels through mutate_appearance which clears
    // bound_style + both swatch ids, then writes the new id for
    // the chosen slot. signal_paint_changed fires for the swatches
    // panel's active-paint ring.
    m_swatch_library->set_paint(*obj, slot, ref);
    // Read post-mutation cache for the after snapshot. BindSwatch
    // Command::execute replays this directly on redo (cached
    // after-state policy — see BindSwatchCommand header for why
    // we don't re-resolve through the library on redo).
    ts.fill_after   = obj->fill;
    ts.stroke_after = obj->stroke;
    snaps.push_back(std::move(ts));
  }

  if (m_history && !snaps.empty()) {
    const bool multi = snaps.size() > 1;
    m_history->push(std::make_unique<BindSwatchCommand>(
        m_swatch_library,
        slot,
        swatch_id,
        std::move(snaps),
        multi ? std::string("Bind swatch (multi)")
              : std::string("Bind swatch")));
  }

  m_sig_doc_changed.emit();
  queue_draw();
}

// ── Canvas::unbind_swatch_from_doc (S83 m4h v4) ──────────────────────────────
//
// Walk every layer in the active document and clear fill_swatch_id /
// stroke_swatch_id on any node whose binding matches `id`. Cached fill /
// stroke (the actual rendered colours) are deliberately untouched —
// break-on-override v1 says the moment-of-unbind appearance is what the
// user keeps.
//
// Caller: SwatchesPanel::on_ctx_delete_swatch, BEFORE
// SwatchLibrary::remove_swatch runs. The library's remove path itself
// has a comment acknowledging it deliberately leaves dangling ids and
// defers cleanup to a "delete-with-usage-check flow" — this method is
// the no-confirm version of that flow. (The full confirm-with-usage-
// counts dialog is still on the backlog.)
//
// Not undoable in v1 — matches the existing un-undoable swatch-create
// path; both are tracked as backlog items (DeleteSwatchCommand +
// AddSwatchCommand). Until those land, the user gets the visual /
// inspector outcome they expect (binding gone, colour preserved) but
// Ctrl+Z won't bring the swatch back. Acceptable for v4; the
// alternative is making delete itself undoable, which is a larger
// scope than m4h.
void Canvas::unbind_swatch_from_doc(const color::SwatchId& id) {
  if (id.empty() || !m_doc) return;
  for (auto& L : m_doc->layers)
    unbind_swatch_walk(L.get(), id);
  // No m_sig_doc_changed.emit() — the visual hasn't changed (cache
  // preserved), and the caller will fire its own signal_inspector_
  // refresh_needed for the panel-side update. Keeping this method
  // narrow — it does the structural unbind and nothing else.
}

void Canvas::apply_stroke_width_to_selection(double width) {
  if (m_selection.empty() || !m_doc)
    return;
  for (SceneNode *obj : m_selection) {
    style::mutate_appearance(*obj, [width](SceneNode& n) {
      n.stroke.width = width;
    });
  }
  m_sig_doc_changed.emit();
  queue_draw();
}

void Canvas::apply_opacity_to_selection(double opacity) {
  if (m_selection.empty() || !m_doc)
    return;
  const double v = std::clamp(opacity, 0.0, 1.0);
  // Group-flatten rule (s108 m2): when the user sets opacity on a
  // group-like container, the visual intent is "this group as a unit
  // looks V transparent" — not "compound V on top of whatever member
  // opacities already have." Flatten direct members to 1.0 so the
  // group's own opacity carries the full effect. The structural change
  // is triggered only by an explicit user/macro action; load/parse
  // doesn't reach here, so externally-authored compounded group
  // opacity round-trips intact when untouched.
  auto is_grouplike = [](const SceneNode *n) {
    return n && (n->type == SceneNode::Type::Group ||
                 n->type == SceneNode::Type::Compound ||
                 n->type == SceneNode::Type::ClipGroup ||
                 n->type == SceneNode::Type::Blend ||
                 n->type == SceneNode::Type::Warp);
  };
  // Capture pre-edit snapshots BEFORE writing — group + every flattened
  // child gets a Snap so undo restores the full pre-edit state. Track
  // already-snapped nodes by pointer to avoid double-recording when a
  // group and one of its members are both selected (rare but possible
  // via shift-click).
  std::vector<SetOpacityCommand::Snap> snaps;
  std::unordered_set<SceneNode*> seen;
  auto snap_and_write = [&](SceneNode *n, double after) {
    if (!n || !seen.insert(n).second) return;
    snaps.push_back({n, n->opacity, after});
    n->opacity = after;
  };
  for (SceneNode *obj : m_selection) {
    if (!obj) continue;
    snap_and_write(obj, v);
    if (is_grouplike(obj)) {
      for (auto &child : obj->children)
        if (child) snap_and_write(child.get(), 1.0);
    }
  }
  if (snaps.empty()) return;
  if (m_history)
    m_history->push(std::make_unique<SetOpacityCommand>(std::move(snaps),
                                                        "Set opacity"));
  m_sig_doc_changed.emit();
  queue_draw();
  // Macro recording (s108 m3): record the SetOpacity step at the same
  // seam that does the work. Replay re-enters this function but
  // is_recording is false during replay, so record_step_if_recording
  // no-ops — no double-record. Mirrors the pattern at every other
  // canvas-side commit site (flip, transform, etc).
  {
    MacroStep s;
    s.op    = MacroStep::Op::SetOpacity;
    s.value = v;
    record_step_if_recording(s);
  }
}

void Canvas::rotate_selection_by(double angle_deg, double pivot_x,
                                 double pivot_y, bool pivot_explicit) {
  if (m_selection.empty() || !m_doc)
    return;

  // Compute pivot from selection bbox if not explicit
  if (!pivot_explicit) {
    double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
    for (SceneNode *obj : m_selection) {
      auto bb = object_bbox(*obj, false);
      if (!bb)
        continue;
      bx1 = std::min(bx1, bb->x);
      by1 = std::min(by1, bb->y);
      bx2 = std::max(bx2, bb->x + bb->w);
      by2 = std::max(by2, bb->y + bb->h);
    }
    pivot_x = (bx1 + bx2) * 0.5;
    pivot_y = (by1 + by2) * 0.5;
  }

  double rad = angle_deg * M_PI / 180.0;
  double cosA = std::cos(rad), sinA = std::sin(rad);

  std::vector<ScaleObjectsCommand::LeafSnap> snaps;
  for (SceneNode *obj : m_selection) {
    std::vector<SceneNode *> leaves;
    collect_paths(obj, leaves);
    for (SceneNode *leaf : leaves) {
      if (!leaf->path)
        continue;
      PathData before = *leaf->path;
      for (auto &nd : leaf->path->nodes) {
        auto rot = [&](double ox, double oy, double &rx, double &ry) {
          double rx0 = ox - pivot_x, ry0 = oy - pivot_y;
          rx = pivot_x + rx0 * cosA - ry0 * sinA;
          ry = pivot_y + rx0 * sinA + ry0 * cosA;
        };
        rot(nd.x, nd.y, nd.x, nd.y);
        rot(nd.cx1, nd.cy1, nd.cx1, nd.cy1);
        rot(nd.cx2, nd.cy2, nd.cx2, nd.cy2);
      }
      snaps.push_back({leaf, before, *leaf->path});
    }
  }
  if (m_history && !snaps.empty())
    m_history->push(std::make_unique<ScaleObjectsCommand>(std::move(snaps),
                                                          "Rotate object"));
  m_sig_doc_changed.emit();
  queue_draw();
}

void Canvas::scale_selection_by(double sx, double sy) {
  if (m_selection.empty() || !m_doc)
    return;

  // Scale around bbox centre
  double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
  for (SceneNode *obj : m_selection) {
    auto bb = object_bbox(*obj, false);
    if (!bb)
      continue;
    bx1 = std::min(bx1, bb->x);
    by1 = std::min(by1, bb->y);
    bx2 = std::max(bx2, bb->x + bb->w);
    by2 = std::max(by2, bb->y + bb->h);
  }
  double cx = (bx1 + bx2) * 0.5, cy = (by1 + by2) * 0.5;

  std::vector<ScaleObjectsCommand::LeafSnap> snaps;
  for (SceneNode *obj : m_selection) {
    std::vector<SceneNode *> leaves;
    collect_paths(obj, leaves);
    for (SceneNode *leaf : leaves) {
      if (!leaf->path)
        continue;
      PathData before = *leaf->path;
      for (auto &nd : leaf->path->nodes) {
        auto sc = [&](double ox, double oy, double &rx, double &ry) {
          rx = cx + (ox - cx) * sx;
          ry = cy + (oy - cy) * sy;
        };
        sc(nd.x, nd.y, nd.x, nd.y);
        sc(nd.cx1, nd.cy1, nd.cx1, nd.cy1);
        sc(nd.cx2, nd.cy2, nd.cx2, nd.cy2);
      }
      snaps.push_back({leaf, before, *leaf->path});
    }
  }
  if (m_history && !snaps.empty())
    m_history->push(std::make_unique<ScaleObjectsCommand>(std::move(snaps),
                                                          "Scale object"));
  m_sig_doc_changed.emit();
  queue_draw();
}

// ── run_macro ────────────────────────────────────────────────────────────────
// Plays back a macro on the current selection.
// If selection is ungrouped, runs once per selected object.
// If selection is a single group or compound, runs once on the whole selection.
void Canvas::run_macro(const std::string &macro_id, int from_step) {
  Macro *macro = MacroManager::instance().find_macro(macro_id);
  if (!macro || macro->steps.empty())
    return;
  if (m_selection.empty() || !m_doc)
    return;

  // Determine if we iterate per-object or treat selection as a unit
  bool iterate_per_object = false;
  for (SceneNode *obj : m_selection) {
    if (obj->type == SceneNode::Type::Path ||
        obj->type == SceneNode::Type::Text) {
      iterate_per_object = true;
      break;
    }
  }

  auto run_steps = [&](const std::vector<SceneNode *> &sel) {
    // Temporarily set selection to this object set
    auto saved_sel = m_selection;
    auto saved_selected = m_selected;
    m_selection = sel;
    m_selected = sel.empty() ? nullptr : sel[0];

    int end = (int)macro->steps.size();
    for (int i = std::max(0, from_step); i < end; ++i) {
      const MacroStep &s = macro->steps[i];
      switch (s.op) {
      case MacroStep::Op::Clone:
        clone_selected();
        break;
      case MacroStep::Op::Duplicate:
        duplicate_selected();
        break;
      case MacroStep::Op::Delete:
        delete_selected();
        break;
      case MacroStep::Op::Group:
        group_selection();
        break;
      case MacroStep::Op::Ungroup:
        ungroup_selection();
        break;

      case MacroStep::Op::Move: {
        // Translate all path nodes
        std::vector<ScaleObjectsCommand::LeafSnap> snaps;
        for (SceneNode *obj : m_selection) {
          std::vector<SceneNode *> leaves;
          collect_paths(obj, leaves);
          for (SceneNode *leaf : leaves) {
            if (!leaf->path)
              continue;
            PathData before = *leaf->path;
            for (auto &nd : leaf->path->nodes) {
              nd.x += s.dx;
              nd.y += s.dy;
              nd.cx1 += s.dx;
              nd.cy1 += s.dy;
              nd.cx2 += s.dx;
              nd.cy2 += s.dy;
            }
            snaps.push_back({leaf, before, *leaf->path});
          }
          if (obj->is_text()) {
            obj->text_x += s.dx;
            obj->text_y += s.dy;
          }
          if (obj->is_image()) {
            obj->image_x += s.dx;
            obj->image_y += s.dy;
          }
        }
        if (m_history && !snaps.empty())
          m_history->push(std::make_unique<ScaleObjectsCommand>(
              std::move(snaps), "Move object"));
        m_sig_doc_changed.emit();
        queue_draw();
        break;
      }

      case MacroStep::Op::Scale:
        scale_selection_by(s.scale_x, s.scale_y);
        break;

      case MacroStep::Op::Rotate:
        rotate_selection_by(s.angle_deg, s.pivot_x, s.pivot_y,
                            s.pivot_is_explicit);
        break;

      case MacroStep::Op::FlipH:
        flip_selection(true);
        break;
      case MacroStep::Op::FlipV:
        flip_selection(false);
        break;

      case MacroStep::Op::BringToFront:
        arrange(ArrangeOp::BringToFront);
        break;
      case MacroStep::Op::BringForward:
        arrange(ArrangeOp::BringForward);
        break;
      case MacroStep::Op::SendBackward:
        arrange(ArrangeOp::SendBackward);
        break;
      case MacroStep::Op::SendToBack:
        arrange(ArrangeOp::SendToBack);
        break;

      case MacroStep::Op::BooleanUnion:
        boolean_op(BooleanOpType::Union);
        break;
      case MacroStep::Op::BooleanSubtract:
        boolean_op(BooleanOpType::Subtract);
        break;
      case MacroStep::Op::BooleanIntersect:
        boolean_op(BooleanOpType::Intersect);
        break;

      case MacroStep::Op::OffsetPath:
        offset_path_op(s.value, OffsetSide::Both, false);
        break;

      case MacroStep::Op::SetFill:
        apply_fill_to_selection(s.color_hex);
        break;
      case MacroStep::Op::SetStroke:
        apply_stroke_to_selection(s.color_hex);
        break;
      case MacroStep::Op::SetStrokeWidth:
        apply_stroke_width_to_selection(s.value);
        break;
      case MacroStep::Op::SetOpacity:
        apply_opacity_to_selection(s.value);
        break;

      // Alignment ops — run on whole selection regardless of iterate mode
      case MacroStep::Op::AlignLeft:
        align_selection(AlignOp::AlignLeft);
        break;
      case MacroStep::Op::AlignCenterH:
        align_selection(AlignOp::AlignCenterH);
        break;
      case MacroStep::Op::AlignRight:
        align_selection(AlignOp::AlignRight);
        break;
      case MacroStep::Op::AlignTop:
        align_selection(AlignOp::AlignTop);
        break;
      case MacroStep::Op::AlignMiddleV:
        align_selection(AlignOp::AlignCenterV);
        break;
      case MacroStep::Op::AlignBottom:
        align_selection(AlignOp::AlignBottom);
        break;
      case MacroStep::Op::DistributeH:
        align_selection(AlignOp::DistributeH);
        break;
      case MacroStep::Op::DistributeV:
        align_selection(AlignOp::DistributeV);
        break;

      case MacroStep::Op::ReversePath:
        reverse_selected_path();
        break;
      }
    }

    // Restore selection
    m_selection = saved_sel;
    m_selected = saved_selected;
  };

  if (iterate_per_object) {
    // Run once per top-level selected object
    for (SceneNode *obj : m_selection)
      run_steps({obj});
    // Restore after iteration
    m_sig_selection.emit(m_selected);
  } else {
    // Run on whole selection as a unit (group / compound)
    run_steps(m_selection);
    m_sig_selection.emit(m_selected);
  }

  LOG_INFO("Canvas: run_macro '{}' from_step={} on {} object(s)", macro->name,
           from_step, m_selection.size());
}

// ── step_repeat (legacy 3-arg)
// ──────────────────────────────────────────────────────── Duplicates the
// current selection `copies` times, each copy offset by (dx*i, dy*i) relative
// to the original.  All copies are one atomic undo step.  Translation-only;
// delegates to the extended overload with rotate_enabled=false.
void Canvas::step_repeat(int copies, double dx, double dy) {
  step_repeat(copies, dx, dy, /*rotate_enabled=*/false,
              /*angle_deg=*/0.0, /*pivot_x=*/0.0, /*pivot_y=*/0.0);
}

// ── step_repeat (extended with rotate-around-pivot)
// ──────────────────────────────────────── Two modes:
//
// 1. rotate_enabled = false  →  translation only.  Each copy i is offset by
//    (dx*i, dy*i).  Old behaviour preserved.
//
// 2. rotate_enabled = true   →  orbit-and-spin.  dx/dy ignored.  Let C0 =
//    group bbox centre, r = distance(refpt, C0), θ0 = atan2(C0 - refpt).
//    For each copy i:
//       θi = θ0 + angle_deg*i
//       Ci = refpt + r·(cos θi, sin θi)      (orbit position)
//       Translate clone by (Ci - C0), then rotate in place by angle_deg*i
//       around Ci.
//    A multi-object selection is treated as a rigid group: same Ci and
//    same per-step spin applied to every object in the selection.
//
// All copies form a single atomic undo step.  pivot is in doc coords,
// Y-down.  Angle sign matches Canvas::rotate_selection_by (positive = CW
// in doc-Y-down space, which reads as CW visually).
void Canvas::step_repeat(int copies, double dx, double dy, bool rotate_enabled,
                         double angle_deg, double pivot_x, double pivot_y) {
  if (m_selection.empty() || !m_doc || copies < 1)
    return;

  auto entries = collect_selection_entries(m_doc, m_selection);
  if (entries.empty())
    return;

  // ── Orbit-mode precomputation ──────────────────────────────────────────
  // C0: group bbox centre of the CURRENT selection (original, pre-copy).
  // r, theta0: polar coords of C0 relative to refpt.
  double C0x = 0.0, C0y = 0.0, r = 0.0, theta0 = 0.0;
  if (rotate_enabled) {
    double bx = 0.0, by = 0.0, bw = 0.0, bh = 0.0;
    if (!selection_bbox(bx, by, bw, bh)) {
      // Can't compute centre → fall back to translation-only (dx/dy ignored
      // since UI has them dimmed; copies will stack on original).
      rotate_enabled = false;
    } else {
      C0x = bx + bw * 0.5;
      C0y = by + bh * 0.5;
      double dxC = C0x - pivot_x;
      double dyC = C0y - pivot_y;
      r = std::hypot(dxC, dyC);
      theta0 = std::atan2(dyC, dxC);
    }
  }

  std::vector<StepRepeatCommand::Entry> cmd_entries;
  std::vector<SceneNode *> new_selection;
  int id_counter = s_next_id;

  // Insert above the originals; track shift as we accumulate insertions.
  int shift = 0;
  for (int step = 1; step <= copies; ++step) {
    // ── Per-step transform parameters ────────────────────────────────
    // Translation-only mode uses (dx*step, dy*step).
    // Orbit mode computes a translation (Ci - C0) plus a rotation by
    // step_angle around Ci.
    double tx = 0.0, ty = 0.0;
    double cosA = 1.0, sinA = 0.0;
    double Cix = 0.0, Ciy = 0.0;

    if (rotate_enabled) {
      double step_angle = angle_deg * (double)step * M_PI / 180.0;
      double theta_i = theta0 + step_angle;
      Cix = pivot_x + r * std::cos(theta_i);
      Ciy = pivot_y + r * std::sin(theta_i);
      tx = Cix - C0x;
      ty = Ciy - C0y;
      cosA = std::cos(step_angle);
      sinA = std::sin(step_angle);
    } else {
      tx = dx * step;
      ty = dy * step;
    }

    auto xform = [&](double &x, double &y) {
      if (!rotate_enabled) {
        x += tx;
        y += ty;
        return;
      }
      // Orbit mode: translate by (Ci - C0) so the point that used to be at
      // C0 is now at Ci, then rotate around Ci in place.
      double tx_pt = x + tx;
      double ty_pt = y + ty;
      double rx = tx_pt - Cix;
      double ry = ty_pt - Ciy;
      x = Cix + rx * cosA - ry * sinA;
      y = Ciy + rx * sinA + ry * cosA;
    };

    for (auto &e : entries) {
      auto dup = clone_node(*e.node);
      freshen_ids(dup.get(), m_doc, id_counter);

      std::vector<SceneNode *> paths;
      collect_paths(dup.get(), paths);
      for (SceneNode *p : paths) {
        if (!p->path)
          continue;
        for (auto &n : p->path->nodes) {
          xform(n.x, n.y);
          xform(n.cx1, n.cy1);
          xform(n.cx2, n.cy2);
        }
      }
      if (dup->type == SceneNode::Type::Text) {
        xform(dup->text_x, dup->text_y);
      }

      int ins = e.index + shift;
      auto snap = clone_node(*dup);
      new_selection.push_back(dup.get());
      e.parent->children.insert(e.parent->children.begin() + ins,
                                std::move(dup));
      cmd_entries.push_back({e.parent, std::move(snap), ins});
      ++shift;
    }
  }
  s_next_id = id_counter;

  if (m_history)
    m_history->push(
        std::make_unique<StepRepeatCommand>(std::move(cmd_entries)));

  // Select all new copies
  m_selection = new_selection;
  m_selected = new_selection.empty() ? nullptr : new_selection[0];
  m_selected_node = -1;
  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: step_repeat copies={} dx={:.2f} dy={:.2f} "
           "rotate={} angle={:.2f} refpt=({:.2f},{:.2f}) total={}",
           copies, dx, dy, rotate_enabled ? 1 : 0, angle_deg, pivot_x, pivot_y,
           new_selection.size());
}

// ── selection_bbox / selection_bbox_center
// ──────────────────────────── Aggregate bbox across the current selection
// (doc coords, Y-down).  Pattern lifted from rotate_selection_by.
bool Canvas::selection_bbox(double &x, double &y, double &w, double &h) const {
  if (m_selection.empty())
    return false;
  double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
  bool any = false;
  for (SceneNode *obj : m_selection) {
    auto bb = object_bbox(*obj, /*include_stroke=*/false);
    if (!bb)
      continue;
    bx1 = std::min(bx1, bb->x);
    by1 = std::min(by1, bb->y);
    bx2 = std::max(bx2, bb->x + bb->w);
    by2 = std::max(by2, bb->y + bb->h);
    any = true;
  }
  if (!any)
    return false;
  x = bx1;
  y = by1;
  w = bx2 - bx1;
  h = by2 - by1;
  return true;
}

bool Canvas::selection_bbox_center(double &cx, double &cy) const {
  double x, y, w, h;
  if (!selection_bbox(x, y, w, h))
    return false;
  cx = x + w * 0.5;
  cy = y + h * 0.5;
  return true;
}

// ── set_step_repeat_preview
// ─────────────────────────────────────────── Dialog-owned crosshair
// overlay, separate from rotate-from-point's m_custom_pivot_*.
void Canvas::set_step_repeat_preview(bool active, double px, double py) {
  m_sr_preview_active = active;
  m_sr_preview_x = px;
  m_sr_preview_y = py;
  if (!active) {
    m_sr_pivot_dragging = false;
  }
  queue_draw();
}

void Canvas::set_step_repeat_pivot_callback(
    std::function<void(double, double)> cb) {
  m_sr_pivot_change_cb = std::move(cb);
}

// ── Guide construct (two-point guide creation) ─────────────────────────────
void Canvas::begin_guide_construct() {
  m_guide_construct_active = true;
  m_guide_construct_phase = 0;
  m_guide_construct_perpendicular = false;
  queue_draw();
}

void Canvas::cancel_guide_construct() {
  m_guide_construct_active = false;
  m_guide_construct_phase = 0;
  queue_draw();
}

void Canvas::set_guide_construct_perpendicular(bool on) {
  if (!m_guide_construct_active)
    return;
  m_guide_construct_perpendicular = on;
  queue_draw();
}

void Canvas::set_guide_construct_review_callback(GuideConstructReviewCb cb) {
  m_guide_construct_review_cb = std::move(cb);
}

bool Canvas::commit_guide_construct() {
  if (!m_guide_construct_active || m_guide_construct_phase < 2 || !m_doc)
    return false;

  SceneNode *gl = m_doc->ensure_guide_layer();
  if (!gl || gl->locked) {
    m_guide_construct_active = false;
    m_guide_construct_phase = 0;
    queue_draw();
    return false;
  }
  if (!gl->visible)
    gl->visible = true;

  // Anchor = midpoint of p1/p2.  Angle = atan2(Δy, Δx); +90 for perpendicular.
  const double cx = (m_guide_construct_p1_x + m_guide_construct_p2_x) * 0.5;
  const double cy = (m_guide_construct_p1_y + m_guide_construct_p2_y) * 0.5;
  const double dx = m_guide_construct_p2_x - m_guide_construct_p1_x;
  const double dy = m_guide_construct_p2_y - m_guide_construct_p1_y;
  double angle_deg = std::atan2(dy, dx) * 180.0 / M_PI;
  if (m_guide_construct_perpendicular)
    angle_deg += 90.0;
  // Normalize into (-180, 180] for cleanliness.
  while (angle_deg > 180.0)
    angle_deg -= 360.0;
  while (angle_deg <= -180.0)
    angle_deg += 360.0;

  auto g = std::make_unique<SceneNode>();
  g->type = SceneNode::Type::Guide;
  g->guide_x = cx;
  g->guide_y = cy;
  g->guide_angle = angle_deg;
  gl->children.push_back(std::move(g));

  m_guide_construct_active = false;
  m_guide_construct_phase = 0;
  m_sig_doc_changed.emit();
  queue_draw();
  return true;
}

void Canvas::commit_line_path() {
  if (m_line_tool.points.size() < 2) {
    m_line_tool.reset();
    queue_draw();
    return;
  }

  PathData pd;
  pd.closed = m_line_tool.close_snap; // closed if snapped to start

  for (auto [px, py] : m_line_tool.points) {
    BezierNode n;
    n.x = px;
    n.y = py;
    n.cx1 = px;
    n.cy1 = py;
    n.cx2 = px;
    n.cy2 = py;
    n.type = BezierNode::Type::Corner;
    pd.nodes.push_back(n);
  }

  // If closing, the last point equals first — don't duplicate it
  if (pd.closed && pd.nodes.size() >= 2) {
    auto &first = pd.nodes.front();
    auto &last = pd.nodes.back();
    if (std::hypot(last.x - first.x, last.y - first.y) < 0.001)
      pd.nodes.pop_back();
  }

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Line);
  obj.type = SceneNode::Type::Path;
  obj.path = std::make_unique<PathData>(std::move(pd));
  FillStyle no_fill;
  no_fill.type = FillStyle::Type::None;
  style::mutate_appearance(obj, [this, &no_fill](SceneNode& n) {
    n.fill = no_fill;
    n.stroke = m_def_stroke;
  });

  if (!m_doc || m_doc->layers.empty()) {
    m_line_tool.reset();
    return;
  }
  SceneNode *layer = m_doc->active_layer();
  if (!layer)
    layer = m_doc->layers[0].get();
  layer->children.insert(layer->children.begin(), clone_node(obj));
  m_selected = layer->children.front().get();

  if (m_history)
    m_history->push(std::make_unique<AddNodeCommand>(layer, clone_node(obj)));

  m_line_tool.reset();
  m_sig_selection.emit(m_selected);
  m_sig_request_tool.emit(ActiveTool::Selection);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("LineTool: committed {} points, closed={}", obj.path->nodes.size(),
           obj.path->closed);
}

void Canvas::cancel_line_path() {
  if (!m_line_tool.active())
    return;
  m_line_tool.reset();
  m_sig_selection.emit(nullptr);
  queue_draw();
  LOG_INFO("LineTool: cancelled");
}

void Canvas::commit_pen_path() {
  auto pd = m_pen_tool.finish();
  if (!pd)
    return;

  SceneNode obj;
  obj.type = SceneNode::Type::Path;
  obj.id = "obj" + std::to_string(++s_next_id_pen);
  obj.internal_id = generate_internal_id();
  obj.path = std::make_unique<PathData>(std::move(*pd));
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke = m_def_stroke;
  });

  if (!m_doc || m_doc->layers.empty())
    return;
  SceneNode *layer = m_doc->active_layer();
  if (!layer)
    layer = m_doc->layers[0].get();
  layer->children.insert(layer->children.begin(), clone_node(obj));
  m_selected = layer->children.front().get();

  if (m_history) {
    auto cmd = std::make_unique<AddNodeCommand>(layer, clone_node(obj));
    m_history->push(std::move(cmd));
  }

  // Clear continue-path state
  m_continue_target = nullptr;

  m_sig_selection.emit(m_selected);
  m_sig_doc_changed.emit();
  LOG_INFO("PenTool: committed path '{}'", m_selected->id);
}

void Canvas::cancel_pen_path() {
  if (!m_pen_tool.has_wip)
    return;

  // If we were continuing an existing path, restore it
  if (m_continue_target && m_doc) {
    for (auto &layer : m_doc->layers) {
      // The target was erased — put it back
      auto restored = clone_node(*m_continue_target);
      *restored->path = m_continue_before;
      layer->children.insert(layer->children.begin(), std::move(restored));
      m_selected = layer->children.front().get();
      break;
    }
    m_continue_target = nullptr;
    m_sig_selection.emit(m_selected);
    m_sig_doc_changed.emit();
  }

  m_pen_tool.cancel();
  m_pen_closing = false;
  if (!m_continue_target)
    m_sig_selection.emit(nullptr);
  queue_draw();
  LOG_INFO("PenTool: cancelled WIP path");
}

bool Canvas::selection_tool_key(guint keyval, bool shift, bool ctrl, bool alt) {
  if (m_tool != ActiveTool::Selection)
    return false;
  if (!m_selected)
    return false;

  // ── M4c-2c: Warp envelope pick-set nudge ─────────────────────────────
  // When a Warp is primary and the pick set is non-empty, arrow keys
  // translate every pick by a step (1 px / Shift=8 / Alt=32). Matches
  // object nudge conventions. Ctrl reserved for arrange. One Edit-
  // WarpCommand per keypress → distinct undo steps. Skip handle picks
  // whose parent anchor is also picked (anchor carries handles).
  if (m_selected->is_warp() && !m_warp_env_picks.empty() && !ctrl) {
    double ndx = 0, ndy = 0;
    switch (keyval) {
    case GDK_KEY_Left:
      ndx = -1;
      break;
    case GDK_KEY_Right:
      ndx = 1;
      break;
    case GDK_KEY_Up:
      ndy = -1;
      break;
    case GDK_KEY_Down:
      ndy = 1;
      break;
    default:
      return false;
    }
    double step_px = alt ? 32.0 : (shift ? 8.0 : 1.0);
    double step = step_px / m_zoom;
    ndx *= step;
    ndy *= step;
    // Snapshot pre-state for undo.
    PathData pre_top = m_selected->warp_env_top;
    PathData pre_bottom = m_selected->warp_env_bottom;
    int pre_quality = m_selected->warp_quality;
    // Flat independence: anchors translate x/y only (do NOT carry
    // handles); handles translate their own component. Each picked
    // element moves by the same delta.
    for (const auto &p : m_warp_env_picks) {
      PathData &env =
          p.is_top ? m_selected->warp_env_top : m_selected->warp_env_bottom;
      if (p.idx < 0 || p.idx >= (int)env.nodes.size())
        continue;
      BezierNode &n = env.nodes[p.idx];
      if (p.part == EnvelopePart::Anchor) {
        n.x += ndx;
        n.y += ndy;
        // Carry coincident handles — see multi-drag rationale.
        const PathData &pre = p.is_top ? pre_top : pre_bottom;
        if (p.idx < (int)pre.nodes.size()) {
          const BezierNode &prn = pre.nodes[p.idx];
          if (std::hypot(prn.cx1 - prn.x, prn.cy1 - prn.y) <= 1e-6) {
            n.cx1 += ndx;
            n.cy1 += ndy;
          }
          if (std::hypot(prn.cx2 - prn.x, prn.cy2 - prn.y) <= 1e-6) {
            n.cx2 += ndx;
            n.cy2 += ndy;
          }
        }
      } else if (p.part == EnvelopePart::HandleIn) {
        n.cx1 += ndx;
        n.cy1 += ndy;
      } else if (p.part == EnvelopePart::HandleOut) {
        n.cx2 += ndx;
        n.cy2 += ndy;
      }
    }
    m_selected->warp_cache_dirty = true;
    if (m_history) {
      m_history->push(std::make_unique<EditWarpCommand>(
          m_selected, pre_top, pre_bottom, pre_quality,
          m_selected->warp_env_top, m_selected->warp_env_bottom,
          m_selected->warp_quality));
    }
    queue_draw();
    return true;
  }

  // Collect all leaf paths — works for Path, Group, and Compound
  std::vector<SceneNode *> leaves;
  for (SceneNode *obj : m_selection)
    collect_paths(obj, leaves);

  // Text/Image live outside the path-leaf tree — they have their own
  // position fields (text_x/text_y, image_x/image_y) rather than
  // path->nodes. Gather them separately so keyboard nudge works for
  // mixed and pure-text/image selections, and for containers
  // (Group/Compound/ClipGroup) that nest Text or Image children.
  std::vector<SceneNode *> text_nodes;
  std::vector<SceneNode *> image_nodes;
  std::vector<SceneNode *> ref_nodes;
  for (SceneNode *obj : m_selection) {
    if (!obj)
      continue;
    if (obj->is_text()) {
      text_nodes.push_back(obj);
      continue;
    }
    if (obj->is_image()) {
      image_nodes.push_back(obj);
      continue;
    }
    if (obj->is_ref()) {
      ref_nodes.push_back(obj);
      continue;
    }
    if (obj->type == SceneNode::Type::Group ||
        obj->type == SceneNode::Type::Compound ||
        obj->type == SceneNode::Type::ClipGroup) {
      std::vector<SceneNode *> ti_leaves;
      collect_text_image_leaves(obj, ti_leaves);
      for (SceneNode *leaf : ti_leaves) {
        if (leaf->is_text())
          text_nodes.push_back(leaf);
        else if (leaf->is_image())
          image_nodes.push_back(leaf);
      }
    }
  }

  if (leaves.empty() && text_nodes.empty() && image_nodes.empty() &&
      ref_nodes.empty())
    return false;

  // ── Arrow-key nudge — moves all nodes by the same delta ───────────────
  double ndx = 0, ndy = 0;
  switch (keyval) {
  case GDK_KEY_Left:
    ndx = -1;
    break;
  case GDK_KEY_Right:
    ndx = 1;
    break;
  case GDK_KEY_Up:
    ndy = -1;
    break;
  case GDK_KEY_Down:
    ndy = 1;
    break;
  default:
    return false;
  }

  double screen_px = alt ? 32.0 : (shift ? 8.0 : 2.0);
  if (ctrl)
    return false; // Ctrl+arrow reserved for arrange
  double step = screen_px / m_zoom;
  ndx *= step;
  ndy *= step;

  // One-shot snap — populate m_move_snaps + m_ref_move_snaps from current
  // positions, run snap_move, then immediately clear lock state. Snaps to
  // any active snap class (guides / grid / margins). Path leaves and refpts
  // contribute their positions; text/image are not snapped via nudge today
  // (m_text_move_snaps stays empty — separate work if needed).
  if (m_doc && m_doc->snap.enabled &&
      (m_doc->snap.snap_guides || m_doc->snap.snap_grid ||
       m_doc->snap.snap_margins)) {
    m_move_snaps.clear();
    m_ref_move_snaps.clear();
    for (SceneNode *leaf : leaves)
      if (leaf->path && !leaf->path->nodes.empty())
        m_move_snaps.push_back({leaf, leaf->path->nodes, *leaf->path});
    for (SceneNode *r : ref_nodes)
      m_ref_move_snaps.push_back({r, r->ref_x, r->ref_y});
    m_snap_x_locked = false;
    m_snap_y_locked = false;
    auto [snapped_dx, snapped_dy] = snap_move(ndx, ndy);
    ndx = snapped_dx;
    ndy = snapped_dy;
    m_move_snaps.clear();
    m_ref_move_snaps.clear();
    m_snap_x_locked = false;
    m_snap_y_locked = false;
  }

  std::vector<PathData> befores, afters;
  for (SceneNode *leaf : leaves)
    befores.push_back(*leaf->path);

  for (SceneNode *leaf : leaves) {
    for (auto &nd : leaf->path->nodes) {
      nd.x += ndx;
      nd.y += ndy;
      nd.cx1 += ndx;
      nd.cy1 += ndy;
      nd.cx2 += ndx;
      nd.cy2 += ndy;
    }
  }
  for (SceneNode *leaf : leaves)
    afters.push_back(*leaf->path);

  // Text nudge — match the drag-move convention in on_select_update
  // (see m_text_move_snaps loop near line ~6670): text_x/text_y are
  // updated with the same signs as path node deltas, regardless of what
  // the "Y-up" comment on the field declaration suggests. If down-arrow
  // moves text upward after this lands, invert ndy for text.
  for (SceneNode *t : text_nodes) {
    t->text_x += ndx;
    t->text_y += ndy;
  }
  // Image: stored Y-down (doc space) like paths — add directly.
  for (SceneNode *im : image_nodes) {
    im->image_x += ndx;
    im->image_y += ndy;
  }
  // Refpt: same per-point translation as image. No undo command — matches
  // the Ref-tool drag and Selection-tool refpt drag, neither of which
  // push undo. Could be added later via a RefMoveCommand if needed.
  for (SceneNode *r : ref_nodes) {
    r->ref_x += ndx;
    r->ref_y += ndy;
  }

  if (m_history) {
    using clock = std::chrono::steady_clock;
    auto now = clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - m_nudge_last_time)
                       .count();
    bool same_window = (m_nudge_last_obj == m_selected) && (elapsed < 1500);
    m_nudge_last_obj = m_selected;
    m_nudge_last_time = now;

    if (!same_window) {
      for (int i = 0; i < (int)leaves.size(); ++i)
        m_history->push(std::make_unique<EditPathCommand>(
            leaves[i], std::move(befores[i]), std::move(afters[i]),
            "Nudge object"));
    } else {
      for (SceneNode *leaf : leaves)
        if (auto *cmd =
                dynamic_cast<EditPathCommand *>(m_history->last_command()))
          if (cmd->obj == leaf)
            cmd->after = *leaf->path;
    }
  }

  m_sig_doc_changed.emit();
  queue_draw();
  // Record nudge as a Move step. Consecutive nudges within the coalesce
  // window will be folded by the macro editor later.
  if (std::abs(ndx) > 0.001 || std::abs(ndy) > 0.001) {
    MacroStep ms;
    ms.op = MacroStep::Op::Move;
    ms.dx = ndx;
    ms.dy = ndy;
    record_step_if_recording(ms);
  }
  return true;
}

bool Canvas::node_tool_key(guint keyval, bool shift, bool ctrl, bool alt) {
  if (m_tool != ActiveTool::Node)
    return false;
  if (!m_selected || m_selected->type != SceneNode::Type::Path ||
      !m_selected->path)
    return false;

  BezierPath bp = BezierPath::from_path_data(*m_selected->path);
  int n = (int)bp.nodes.size();
  if (n == 0)
    return false;

  // ── Delete selected node ──────────────────────────────────────────────
  if (keyval == GDK_KEY_Delete || keyval == GDK_KEY_BackSpace) {
    if (m_selected_node < 0 || m_selected_node >= n)
      return false;
    // Node-count lock: the selected path can't shrink if it's A or B of
    // a Blend — the Blend's math requires matching node counts on A and B.
    // M3 rejects with a user-visible message; M4 could offer to release
    // the Blend first, but for now the user must Release explicitly.
    if (find_blend_owner(m_selected)) {
      LOG_INFO("NodeTool: delete-node rejected — path is a Blend source "
               "(node count is locked)");
      m_sig_show_message.emit(
          "Blend", "This path is part of a Blend and its node count is locked. "
                   "Release the Blend first to edit node counts.");
      return true;
    }
    if (n == 1) {
      // Last node — delete the whole object
      if (!m_doc)
        return false;
      for (auto &layer : m_doc->layers) {
        auto it = std::find_if(layer->children.begin(), layer->children.end(),
                               [this](const std::unique_ptr<SceneNode> &o) {
                                 return o.get() == m_selected;
                               });
        if (it != layer->children.end()) {
          int idx = (int)(it - layer->children.begin());
          if (m_history)
            m_history->push(std::make_unique<DeleteObjectCommand>(
                layer.get(), clone_node(**it), idx));
          layer->children.erase(it);
          m_selected = nullptr;
          m_selected_node = -1;
          m_sig_selection.emit(nullptr);
          m_sig_doc_changed.emit();
          queue_draw();
          LOG_INFO("NodeTool: deleted last node — removed object");
          return true;
        }
      }
    }
    PathData before_pd = *m_selected->path;
    bp.delete_node(m_selected_node);
    // Keep selection in bounds
    m_selected_node = std::min(m_selected_node, (int)bp.nodes.size() - 1);
    *m_selected->path = bp.to_path_data();
    if (m_history)
      m_history->push(std::make_unique<EditPathCommand>(
          m_selected, std::move(before_pd), *m_selected->path, "Delete node"));
    m_sig_doc_changed.emit();
    m_sig_node_changed.emit(m_selected, m_selected_node);
    queue_draw();
    LOG_INFO("NodeTool: deleted node, {} remaining", bp.nodes.size());
    return true;
  }

  // ── Cycle selection with Tab / Shift+Tab ──────────────────────────────
  // Ctrl-gated: Ctrl+Tab / Ctrl+Shift+Tab are reserved for document
  // navigation at the window level (s108 m7). Bare Tab / Shift+Tab
  // remains the node-cycle binding when the Node tool is active.
  if ((keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) && !ctrl) {
    if (m_selected_node < 0) {
      m_selected_node = 0;
    } else if (shift) {
      m_selected_node = (m_selected_node - 1 + n) % n;
    } else {
      m_selected_node = (m_selected_node + 1) % n;
    }
    queue_draw();
    LOG_DEBUG("NodeTool: selected node {}/{}", m_selected_node, n);
    m_sig_node_changed.emit(m_selected, m_selected_node);
    return true;
  }

  // ── Context-aware Join / Close (J) ───────────────────────────────────
  // Priority:
  //   1. m_selected2 set → cross-path join of two open paths
  //   2. Two endpoints of the same open path in m_node_selection → close it
  //   3. Single open path (any endpoint selected, or none) → close/open toggle
  //   4. Anything else → show a helpful error dialog
  if (keyval == GDK_KEY_j || keyval == GDK_KEY_J) {
    // Node-count lock: J closes/opens single paths (no count change) OR
    // joins two paths (count change). Either way, the closed-flag and
    // node-count of A/B must stay matched. Reject if the primary path
    // is a Blend source.
    if (find_blend_owner(m_selected)) {
      LOG_INFO("NodeTool: J rejected — path is a Blend source");
      m_sig_show_message.emit(
          "Blend",
          "This path is part of a Blend; its node count and open/closed "
          "state are locked. Release the Blend first.");
      return true;
    }

    // ── Pre-flight: try to resolve m_selected2 from current state ─────
    // If m_selected2 is not already set, check if the user has indicated
    // a second path via m_node_selection (Shift+clicked a node on another
    // path), or if there is exactly one other open path whose endpoint is
    // closest to the active endpoint of m_selected.
    //
    // This lets the user:
    //   a) Click endpoint on path A → Shift+click endpoint on path B → J
    //   b) Click endpoint on path A → J  (auto-picks nearest endpoint)
    if (!m_selected->path->closed && (!m_selected2 || !m_selected2->path)) {
      // Determine which endpoint of m_selected is "active"
      int n1 = (int)m_selected->path->nodes.size();
      int active_ep = -1; // 0=head, n1-1=tail
      if (m_selected_node == 0 || m_selected_node == n1 - 1)
        active_ep = m_selected_node;

      // ── a) Shift+click gave us a node on another path ────────────────
      if (!m_selected2) {
        for (const auto &ns : m_node_selection) {
          if (ns.obj == m_selected || !ns.obj->path || ns.obj->path->closed)
            continue;
          int nn = (int)ns.obj->path->nodes.size();
          if (ns.node_idx == 0 || ns.node_idx == nn - 1) {
            m_selected2 = ns.obj;
            m_selected_node2 = ns.node_idx;
            // If primary has no endpoint active, pick the nearer end of
            // m_selected
            if (active_ep < 0 && n1 >= 1) {
              const BezierNode &ep2 = ns.obj->path->nodes[ns.node_idx];
              double d0 = Vec2{m_selected->path->nodes[0].x - ep2.x,
                               m_selected->path->nodes[0].y - ep2.y}
                              .length();
              double d1 = Vec2{m_selected->path->nodes[n1 - 1].x - ep2.x,
                               m_selected->path->nodes[n1 - 1].y - ep2.y}
                              .length();
              active_ep = (d0 <= d1) ? 0 : n1 - 1;
              m_selected_node = active_ep;
            }
            break;
          }
        }
      }

      // ── b) Auto-scan: find nearest endpoint on any other open path ───
      if (!m_selected2 && m_doc && active_ep >= 0) {
        const BezierNode &my_ep = m_selected->path->nodes[active_ep];
        double best_d2 = 1e18;
        SceneNode *best_obj = nullptr;
        int best_node = -1;

        for (auto &layer : m_doc->layers) {
          if (!layer->visible || layer->locked || layer->is_special_layer())
            continue;
          for (auto &uptr : layer->children) {
            SceneNode *other = uptr.get();
            if (other == m_selected || !other->path || other->path->closed ||
                other->path->nodes.empty())
              continue;
            int nn = (int)other->path->nodes.size();
            for (int ei = 0; ei < 2; ++ei) {
              int ni = (ei == 0) ? 0 : nn - 1;
              const BezierNode &ep = other->path->nodes[ni];
              double d2 = Vec2{ep.x - my_ep.x, ep.y - my_ep.y}.length();
              d2 = d2 * d2;
              if (d2 < best_d2) {
                best_d2 = d2;
                best_obj = other;
                best_node = ni;
              }
            }
          }
        }

        if (best_obj) {
          m_selected2 = best_obj;
          m_selected_node2 = best_node;
        }
      }
    }

    // ── 1. Cross-path join: secondary path queued ─────────────────────
    if (m_selected2 && m_selected2->path && m_selected_node2 >= 0) {

      // Validate m_selected2 is still live in the document
      SceneNode *layer2_ptr = nullptr;
      std::unique_ptr<SceneNode> *obj2_uptr = nullptr;
      if (m_doc) {
        for (auto &layer : m_doc->layers) {
          for (auto &uptr : layer->children) {
            if (uptr.get() == m_selected2) {
              layer2_ptr = layer.get();
              obj2_uptr = &uptr;
              break;
            }
          }
          if (layer2_ptr)
            break;
        }
      }
      if (!layer2_ptr) {
        // m_selected2 is dangling — clear and fall through to single-path logic
        m_selected2 = nullptr;
        m_selected_node2 = -1;
        LOG_INFO(
            "NodeTool: join — secondary path no longer in document, cleared");
        // fall through: skip the rest of this block, handled below
      } else {

        // Both paths must be open
        if (m_selected->path->closed || m_selected2->path->closed) {
          std::string reason;
          if (m_selected->path->closed && m_selected2->path->closed)
            reason = "Both paths are already closed.\n\nJoin only works "
                     "between two open paths.";
          else
            reason = "One of the selected paths is already closed.\n\nJoin "
                     "only works between two open paths.";
          m_sig_show_message.emit("Cannot Join Paths", reason);
          LOG_INFO("NodeTool: join — one or both paths are closed");
          return true;
        }

        int n1 = (int)m_selected->path->nodes.size();
        int n2 = (int)m_selected2->path->nodes.size();

        // Validate endpoint selection — each selected node must be an endpoint
        bool s1_is_head = (m_selected_node == 0);
        bool s1_is_tail = (m_selected_node == n1 - 1);
        bool s2_is_head = (m_selected_node2 == 0);
        bool s2_is_tail = (m_selected_node2 == n2 - 1);

        if (!(s1_is_head || s1_is_tail) || !(s2_is_head || s2_is_tail)) {
          m_sig_show_message.emit(
              "Cannot Join Paths",
              "Select an endpoint node on each path before joining.\n\n"
              "Only the first or last node of an open path can be joined.");
          LOG_INFO("NodeTool: join — selected nodes are not endpoints");
          return true;
        }

        BezierPath a = BezierPath::from_path_data(*m_selected->path);
        BezierPath b = BezierPath::from_path_data(*m_selected2->path);

        // Orient so we join a.tail → b.head
        if (s1_is_head)
          a.reverse();
        if (s2_is_tail)
          b.reverse();

        // a.back() joins to b.front()
        double tol = 6.0 / m_zoom;
        double d = Vec2{a.nodes.back().x - b.nodes.front().x,
                        a.nodes.back().y - b.nodes.front().y}
                       .length();

        PathData before_a = *m_selected->path;

        if (d <= tol) {
          // Coincident — drop b_head, weld cleanly into one node
          a.nodes.pop_back();
        } else {
          // Not coincident — bridge with a straight segment
          a.nodes.back().cx2 = a.nodes.back().x;
          a.nodes.back().cy2 = a.nodes.back().y;
          b.nodes.front().cx1 = b.nodes.front().x;
          b.nodes.front().cy1 = b.nodes.front().y;
        }

        for (auto &nd : b.nodes)
          a.nodes.push_back(nd);
        a.closed = false;

        *m_selected->path = a.to_path_data();

        // Clear secondary BEFORE erasing from document to avoid dangling ptr
        SceneNode *joined_obj = m_selected;
        m_selected2 = nullptr;
        m_selected_node2 = -1;
        m_selected_node = (int)joined_obj->path->nodes.size() - 1;

        // Find index of path B in its layer before erasing (needed for undo)
        int b_idx = 0;
        for (int i = 0; i < (int)layer2_ptr->children.size(); ++i) {
          if (&layer2_ptr->children[i] == obj2_uptr) {
            b_idx = i;
            break;
          }
        }

        // Snapshot path B for undo BEFORE erasing it
        auto b_snap = clone_node(**obj2_uptr);

        // Erase path B
        layer2_ptr->children.erase(std::find_if(
            layer2_ptr->children.begin(), layer2_ptr->children.end(),
            [obj2_uptr](const std::unique_ptr<SceneNode> &o) {
              return &o == obj2_uptr;
            }));

        // Composite undo: restores path A's data AND re-inserts path B
        if (m_history) {
          auto composite = std::make_unique<CompositeCommand>("Join paths");
          composite->add(std::make_unique<EditPathCommand>(
              joined_obj, std::move(before_a), *joined_obj->path,
              "Join paths"));
          composite->add(std::make_unique<DeleteObjectCommand>(
              layer2_ptr, std::move(b_snap), b_idx));
          m_history->push(std::move(composite));
        }

        m_sig_selection.emit(joined_obj);
        m_sig_node_changed.emit(joined_obj, m_selected_node);
        m_sig_doc_changed.emit();
        queue_draw();
        LOG_INFO("NodeTool: joined paths — {} nodes",
                 joined_obj->path->nodes.size());
        return true;

      } // end else (layer2_ptr valid)
    } // end if (m_selected2)

    // ── 2. Same-path two-endpoint join ───────────────────────────────
    // If exactly two endpoints of the *same* open path are selected via
    // m_node_selection (head=0 and tail=n-1), close the path directly.
    {
      // Collect endpoint indices selected on this path
      bool has_head = false, has_tail = false;
      int last_idx = n - 1;
      for (const auto &ns : m_node_selection) {
        if (ns.obj == m_selected) {
          if (ns.node_idx == 0)
            has_head = true;
          if (ns.node_idx == last_idx)
            has_tail = true;
        }
      }
      // Also count m_selected_node itself
      if (m_selected_node == 0)
        has_head = true;
      if (m_selected_node == last_idx)
        has_tail = true;

      if (has_head && has_tail && !bp.closed && n >= 2) {
        PathData before_pd = *m_selected->path;
        const BezierNode &head = bp.nodes[0];
        const BezierNode &tail = bp.nodes.back();
        double tol_doc = 6.0 / m_zoom;
        double d = Vec2{tail.x - head.x, tail.y - head.y}.length();
        if (d <= tol_doc) {
          // Coincident — weld tail into head, close cleanly
          bp.nodes.pop_back();
        } else {
          // Distant — bridge with straight segment
          int last = (int)bp.nodes.size() - 1;
          bp.nodes.back().cx2 = bp.nodes.back().x;
          bp.nodes.back().cy2 = bp.nodes.back().y;
          bp.nodes.front().cx1 = bp.nodes.front().x;
          bp.nodes.front().cy1 = bp.nodes.front().y;
          bp.recompute_join_handles(0);
          bp.recompute_join_handles(last);
        }
        bp.closed = true;
        *m_selected->path = bp.to_path_data();
        m_selected_node = -1;
        m_node_selection.clear();
        if (m_history)
          m_history->push(std::make_unique<EditPathCommand>(
              m_selected, std::move(before_pd), *m_selected->path,
              "Close path"));
        m_sig_selection.emit(m_selected);
        m_sig_doc_changed.emit();
        queue_draw();
        LOG_INFO("NodeTool: closed path via two-endpoint selection");
        return true;
      }
    }

    // ── 3. Single path: close / open toggle ──────────────────────────
    // Works on any open or closed path regardless of which node is selected.
    if (n < 2 && !bp.closed) {
      m_sig_show_message.emit(
          "Cannot Close Path",
          "A path needs at least 2 nodes before it can be closed.");
      return true;
    }

    PathData before_pd = *m_selected->path;
    bool was_closed = bp.closed;

    if (bp.closed) {
      // Opening: duplicate node 0 and append as new tail so the closing
      // segment becomes explicit and no geometry is lost.
      BezierNode tail = bp.nodes[0];
      tail.cx2 = tail.x;
      tail.cy2 = tail.y;
      LOG_INFO("J open: node0=({:.2f},{:.2f}) tail=({:.2f},{:.2f})",
               bp.nodes[0].x, bp.nodes[0].y, tail.x, tail.y);
      bp.nodes.push_back(tail);
      bp.closed = false;
    } else {
      // Closing: weld if endpoints are coincident, else bridge.
      const BezierNode &head = bp.nodes[0];
      const BezierNode &tail = bp.nodes.back();
      double tol_doc = 6.0 / m_zoom;
      double d = Vec2{tail.x - head.x, tail.y - head.y}.length();
      if (d <= tol_doc) {
        bp.nodes.pop_back();
      } else {
        int last = (int)bp.nodes.size() - 1;
        bp.nodes.back().cx2 = bp.nodes.back().x;
        bp.nodes.back().cy2 = bp.nodes.back().y;
        bp.nodes.front().cx1 = bp.nodes.front().x;
        bp.nodes.front().cy1 = bp.nodes.front().y;
        bp.recompute_join_handles(0);
        bp.recompute_join_handles(last);
      }
      bp.closed = true;
    }
    *m_selected->path = bp.to_path_data();
    m_selected_node = -1;
    if (m_history)
      m_history->push(std::make_unique<EditPathCommand>(
          m_selected, std::move(before_pd), *m_selected->path,
          was_closed ? "Open path" : "Close path"));
    m_sig_selection.emit(m_selected);
    m_sig_doc_changed.emit();
    queue_draw();
    LOG_INFO("NodeTool: path {} → {}", m_selected->id,
             bp.closed ? "closed" : "open");
    return true;
  }

  // ── Break path at selected node (B) ──────────────────────────────────
  // Routing is path-state aware:
  //   • Closed path → open the loop at the selected node (one path remains)
  //   • Open path  → split into two paths at the selected node
  // Pre-S100 m3 the handler always called open_selected_at_node, which is
  // the closed-path opener; on open paths it returned silently and the user
  // saw nothing happen. Symptom name: "B sometimes does nothing." Fixed by
  // dispatching on bp.closed at the call site (the handler already has bp).
  if (!ctrl && !shift && !alt && (keyval == GDK_KEY_b || keyval == GDK_KEY_B)) {
    if (m_selected_node < 0 || m_selected_node >= n) {
      m_sig_show_message.emit(
          "Cannot Break Path",
          "Select a node first, then press B to break the path at that node.");
      return true;
    }
    if (find_blend_owner(m_selected)) {
      LOG_INFO("NodeTool: break-path rejected — path is a Blend source");
      m_sig_show_message.emit(
          "Blend", "This path is part of a Blend; its structure is locked. "
                   "Release the Blend first.");
      return true;
    }
    if (bp.closed) {
      open_selected_at_node();
    } else {
      split_selected_at_node();
    }
    return true;
  }

  // ── Node type hotkeys ─────────────────────────────────────────────────
  // A = Symmetric, M = sMooth, C = Cusp, K = corner (Kink)
  // One key per type — aliases (S for Symmetric, Q for Corner) were removed
  // because they shadowed global shortcuts (S → Selection tool,
  // Q → toggle snap) making those global keys unreachable when a node
  // was selected. Node-tool keys only act when no modifier is held.
  if (!ctrl && !shift && m_selected_node >= 0 && m_selected_node < n) {
    BezierNode::Type new_type;
    bool type_key = true;
    switch (keyval) {
    case GDK_KEY_a:
    case GDK_KEY_A:
      new_type = BezierNode::Type::Symmetric;
      break;
    case GDK_KEY_m:
    case GDK_KEY_M:
      new_type = BezierNode::Type::Smooth;
      break;
    case GDK_KEY_c:
    case GDK_KEY_C:
      new_type = BezierNode::Type::Cusp;
      break;
    case GDK_KEY_k:
    case GDK_KEY_K:
      new_type = BezierNode::Type::Corner;
      break;
    default:
      type_key = false;
      break;
    }
    if (type_key) {
      PathData before = *m_selected->path;
      // Apply to all nodes in m_node_selection (or just m_selected_node)
      if (!m_node_selection.empty()) {
        for (const auto &ns : m_node_selection) {
          if (ns.obj == m_selected && ns.node_idx >= 0 &&
              ns.node_idx < (int)bp.nodes.size())
            bp.set_node_type(ns.node_idx, new_type);
        }
      } else {
        bp.set_node_type(m_selected_node, new_type);
      }
      *m_selected->path = bp.to_path_data();
      if (m_history)
        m_history->push(std::make_unique<EditPathCommand>(
            m_selected, std::move(before), *m_selected->path, "Set node type"));
      LOG_INFO("NodeTool: node {} type → {}", m_selected_node, (int)new_type);
      m_sig_doc_changed.emit();
      m_sig_node_changed.emit(m_selected, m_selected_node);
      queue_draw();
      return true;
    }
  }

  // ── Reverse path direction (R) ────────────────────────────────────────
  if (keyval == GDK_KEY_r || keyval == GDK_KEY_R) {
    PathData before = *m_selected->path;
    bp.reverse();
    // After reversal the selected node index maps to (n-1-idx)
    if (m_selected_node >= 0)
      m_selected_node = n - 1 - m_selected_node;
    *m_selected->path = bp.to_path_data();
    PathData after = *m_selected->path;
    if (m_history)
      m_history->push(std::make_unique<EditPathCommand>(
          m_selected, std::move(before), std::move(after), "Reverse path"));
    m_sig_doc_changed.emit();
    queue_draw();
    LOG_INFO("NodeTool: reversed path '{}'", m_selected->id);
    return true;
  }

  // ── Arrow-key nudge ───────────────────────────────────────────────────
  // Zoom-relative: small=2px, medium=8px, large=32px screen pixels → doc units
  {
    double ndx = 0, ndy = 0;
    switch (keyval) {
    case GDK_KEY_Left:
      ndx = -1;
      break;
    case GDK_KEY_Right:
      ndx = 1;
      break;
    case GDK_KEY_Up:
      ndy = -1;
      break;
    case GDK_KEY_Down:
      ndy = 1;
      break;
    default:
      break;
    }
    if (ndx != 0.0 || ndy != 0.0) {
      double screen_px = alt ? 32.0 : (shift ? 8.0 : 2.0);
      if (ctrl)
        return false; // Ctrl+arrow reserved for arrange
      double step = screen_px / m_zoom;
      ndx *= step;
      ndy *= step;

      PathData before = *m_selected->path;

      if (m_selected_node >= 0 && m_selected_node < n) {
        // Move primary node
        BezierNode &nd = bp.nodes[m_selected_node];
        nd.x += ndx;
        nd.y += ndy;
        nd.cx1 += ndx;
        nd.cy1 += ndy;
        nd.cx2 += ndx;
        nd.cy2 += ndy;
        if (m_selected_node > 0 || bp.closed)
          bp.recompute_cusp_handles((m_selected_node - 1 + n) % n);
        bp.recompute_cusp_handles(m_selected_node);
        if (m_selected_node < n - 1 || bp.closed)
          bp.recompute_cusp_handles((m_selected_node + 1) % n);
      }
      *m_selected->path = bp.to_path_data();

      // Move all other nodes in m_node_selection
      for (const auto &ns : m_node_selection) {
        if (!ns.obj || !ns.obj->path)
          continue;
        if (ns.obj == m_selected && ns.node_idx == m_selected_node)
          continue;
        if (ns.node_idx < 0 || ns.node_idx >= (int)ns.obj->path->nodes.size())
          continue;
        BezierNode &nd = ns.obj->path->nodes[ns.node_idx];
        nd.x += ndx;
        nd.y += ndy;
        nd.cx1 += ndx;
        nd.cy1 += ndy;
        nd.cx2 += ndx;
        nd.cy2 += ndy;
      }
      PathData after = *m_selected->path;
      if (m_history) {
        using clock = std::chrono::steady_clock;
        auto now = clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - m_nudge_last_time)
                           .count();
        bool same_window = (m_nudge_last_obj == m_selected) && (elapsed < 1500);
        m_nudge_last_obj = m_selected;
        m_nudge_last_time = now;

        if (!same_window) {
          m_history->push(std::make_unique<EditPathCommand>(
              m_selected, std::move(before), std::move(after), "Nudge node"));
        } else {
          if (auto *cmd =
                  dynamic_cast<EditPathCommand *>(m_history->last_command()))
            if (cmd->obj == m_selected)
              cmd->after = after;
        }
      }
      m_sig_doc_changed.emit();
      queue_draw();
      LOG_DEBUG("NodeTool: nudge node {} by ({:.2f},{:.2f})", m_selected_node,
                ndx, ndy);
      return true;
    }
  }

  return false;
}

// ── Node editor
// ───────────────────────────────────────────────────────────────
void Canvas::on_node_begin(double x, double y) {
  if (!m_doc)
    return;
  // Record press position for dead-zone; drag hasn't started yet
  m_node_press_x = x;
  m_node_press_y = y;
  m_node_drag_started = false;

  double dx, dy;
  screen_to_doc(x, y, dx, dy);
  Vec2 doc_pos{dx, dy};

  // ── Shift+click: toggle any node on any path in/out of multi-selection ─
  if (m_mod_shift && m_selected) {
    double tol_doc = 8.0 / m_zoom;

    std::function<bool(SceneNode *)> shift_scan = [&](SceneNode *node) -> bool {
      if (node->type == SceneNode::Type::Path && node->path) {
        for (int i = 0; i < (int)node->path->nodes.size(); ++i) {
          Vec2 np{node->path->nodes[i].x, node->path->nodes[i].y};
          if (np.dist(doc_pos) <= tol_doc) {
            auto it =
                std::find_if(m_node_selection.begin(), m_node_selection.end(),
                             [&](const NodeSel &ns) {
                               return ns.obj == node && ns.node_idx == i;
                             });
            if (it != m_node_selection.end()) {
              m_node_selection.erase(it);
              // If we just deselected the primary node, point to another
              if (m_selected_node == i && node == m_selected) {
                if (!m_node_selection.empty()) {
                  for (const auto &ns2 : m_node_selection) {
                    if (ns2.obj == m_selected) {
                      m_selected_node = ns2.node_idx;
                      break;
                    }
                  }
                }
              }
            } else {
              m_node_selection.push_back({node, i});
              if (node != m_selected) {
                m_selected = node;
                m_selected_node = i;
                m_node_drag_kind = HitResult::Kind::Node;
                m_sig_selection.emit(m_selected);
              } else {
                m_selected_node = i;
                m_node_drag_kind = HitResult::Kind::Node;
              }
            }
            m_sig_node_changed.emit(m_selected, m_selected_node);
            queue_draw();
            return true;
          }
        }
      } else if (node->type == SceneNode::Type::Group ||
                 node->type == SceneNode::Type::Compound) {
        for (auto &child : node->children)
          if (shift_scan(child.get()))
            return true;
      }
      return false;
    };

    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children)
        if (shift_scan(obj_uptr.get()))
          return;
    }
    // Shift+click missed all nodes — fall through to path body hit
  }

  // 1. Try to hit the currently selected path first
  if (m_selected && m_selected->type == SceneNode::Type::Path &&
      m_selected->path) {
    BezierPath bp = BezierPath::from_path_data(*m_selected->path);

    if (m_mod_shift) {
      // Already handled above — this branch only reached if no node was hit
      // Fall through to step 2 for path body shift+click
    }

    HitResult hit = bp.hit_test(doc_pos, m_zoom);
    if (hit.kind != HitResult::Kind::None) {
      m_node_drag_kind = hit.kind;
      m_selected_node = hit.node_index;
      m_cycle_last_pos = doc_pos;
      m_cycle_index = 0;

      // Only clear multi-selection if clicking a node NOT already in it
      bool already_in_selection = std::any_of(
          m_node_selection.begin(), m_node_selection.end(),
          [&](const NodeSel &ns) {
            return ns.obj == m_selected && ns.node_idx == hit.node_index;
          });
      if (!already_in_selection) {
        m_node_selection.clear();
        if (hit.kind == HitResult::Kind::Node)
          m_node_selection.push_back({m_selected, hit.node_index});
      }

      if (hit.kind == HitResult::Kind::OnCurve) {
        // Node-count lock: same reasoning as delete — A and B of a Blend
        // must have matching node counts.
        if (find_blend_owner(m_selected)) {
          LOG_INFO("NodeTool: insert-node rejected — path is a Blend source");
          m_sig_show_message.emit(
              "Blend",
              "This path is part of a Blend and its node count is locked. "
              "Release the Blend first to edit node counts.");
          queue_draw();
          return;
        }
        m_node_selection.clear(); // inserting always clears
        PathData before = *m_selected->path;
        bp.insert_node_at(hit.segment_index, hit.t);
        PathData after = bp.to_path_data();
        *m_selected->path = after;
        m_selected_node = hit.segment_index + 1;
        if (m_history)
          m_history->push(std::make_unique<EditPathCommand>(
              m_selected, std::move(before), std::move(after), "Insert node"));
        m_sig_doc_changed.emit();
      }
      m_sig_node_changed.emit(m_selected, m_selected_node);
      queue_draw();
      return;
    }
  }

  // 2. No hit on current selection — scan all OTHER path objects for the
  // closest hit Descends into Group and Compound children to find leaf paths.
  SceneNode *best_obj = nullptr;
  HitResult best_hit;
  best_hit.distance = 1e9;

  std::function<void(SceneNode *)> scan_node = [&](SceneNode *node) {
    if (node->type == SceneNode::Type::Path && node->path) {
      if (node == m_selected)
        return; // already checked in step 1
      BezierPath bp = BezierPath::from_path_data(*node->path);
      HitResult hit = bp.hit_test(doc_pos, m_zoom);
      if (hit.kind != HitResult::Kind::None &&
          hit.distance < best_hit.distance) {
        best_hit = hit;
        best_obj = node;
      }
    } else if (node->type == SceneNode::Type::Group ||
               node->type == SceneNode::Type::Compound) {
      for (auto &child : node->children)
        scan_node(child.get());
    }
  };

  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->locked || layer->is_special_layer())
      continue;
    for (auto &obj_uptr : layer->children)
      scan_node(obj_uptr.get());
  }

  if (best_obj) {
    if (m_mod_shift && m_selected && best_obj != m_selected) {
      // ── Shift+click on different path body: add all its nodes ────────
      bool already_added = std::any_of(
          m_node_selection.begin(), m_node_selection.end(),
          [best_obj](const NodeSel &ns) { return ns.obj == best_obj; });
      if (already_added) {
        // Remove all nodes of this path
        m_node_selection.erase(std::remove_if(m_node_selection.begin(),
                                              m_node_selection.end(),
                                              [best_obj](const NodeSel &ns) {
                                                return ns.obj == best_obj;
                                              }),
                               m_node_selection.end());
      } else {
        // Add all nodes of this path
        for (int i = 0; i < (int)best_obj->path->nodes.size(); ++i)
          m_node_selection.push_back({best_obj, i});
      }
      LOG_DEBUG("NodeTool: shift+path '{}' → {} nodes in selection",
                best_obj->id, m_node_selection.size());
      queue_draw();
      return;
    }

    // ── Plain click on unselected path — select it ────────────────────
    m_node_selection.clear();
    m_selected = best_obj;
    m_node_drag_kind = best_hit.kind == HitResult::Kind::OnCurve
                           ? HitResult::Kind::None
                           : best_hit.kind;
    m_selected_node =
        best_hit.kind == HitResult::Kind::OnCurve ? -1 : best_hit.node_index;
    LOG_DEBUG("NodeTool: new obj='{}' node={} kind={}", m_selected->id,
              m_selected_node, (int)m_node_drag_kind);
    m_sig_selection.emit(m_selected);
    m_sig_node_changed.emit(m_selected, m_selected_node);
  } else {
    LOG_DEBUG("NodeTool: clicked empty space — start marquee");
    m_selected = nullptr;
    m_selected2 = nullptr;
    m_selected_node = -1;
    m_selected_node2 = -1;
    m_node_selection.clear();
    m_node_drag_kind = HitResult::Kind::None;
    // Start node marquee
    m_marquee_active = true;
    m_marquee_start_dx = doc_pos.x;
    m_marquee_start_dy = doc_pos.y;
    m_marquee_cur_dx = doc_pos.x;
    m_marquee_cur_dy = doc_pos.y;
    m_sig_selection.emit(nullptr);
    m_sig_node_changed.emit(nullptr, -1);
  }
  queue_draw();
}

void Canvas::on_node_update(double /*delta_x*/, double /*delta_y*/) {
  // ── Node marquee update ───────────────────────────────────────────────
  if (m_marquee_active) {
    double cx, cy;
    screen_to_doc(m_mouse_x, m_mouse_y, cx, cy);
    m_marquee_cur_dx = cx;
    m_marquee_cur_dy = cy;
    queue_draw();
    return;
  }

  if (!m_selected || m_selected->type != SceneNode::Type::Path ||
      !m_selected->path)
    return;
  if (m_selected_node < 0)
    return;
  if (m_node_drag_kind == HitResult::Kind::None)
    return;

  // Dead-zone: suppress movement until cursor has travelled enough from press
  if (!m_node_drag_started) {
    double dist =
        std::hypot(m_mouse_x - m_node_press_x, m_mouse_y - m_node_press_y);
    if (dist < NODE_DRAG_THRESHOLD_PX)
      return;
    m_node_drag_started = true;
    // Snapshot before-state for undo
    m_node_drag_before = *m_selected->path;
    m_node_drag_before_multi.clear();
    for (const auto &ns : m_node_selection) {
      if (!ns.obj || !ns.obj->path || ns.obj == m_selected)
        continue;
      bool already = std::any_of(
          m_node_drag_before_multi.begin(), m_node_drag_before_multi.end(),
          [&](const auto &p) { return p.first == ns.obj; });
      if (!already)
        m_node_drag_before_multi.push_back({ns.obj, *ns.obj->path});
    }
  }

  double dx, dy;
  screen_to_doc(m_mouse_x, m_mouse_y, dx, dy);
  Vec2 doc_pos{dx, dy};

  BezierPath bp = BezierPath::from_path_data(*m_selected->path);
  if (m_selected_node >= (int)bp.nodes.size())
    return;

  // Snapshot before move to detect actual change
  const BezierNode &nb = bp.nodes[m_selected_node];
  double pre_x = nb.x, pre_y = nb.y;
  double pre_ix = nb.cx1, pre_iy = nb.cy1;
  double pre_ox = nb.cx2, pre_oy = nb.cy2;

  // ── Endpoint snap ─────────────────────────────────────────────────────
  // When dragging an endpoint of an open path, snap to nearby endpoints
  // of other open paths. This sets up the merge point for J.
  m_snap_target_obj = nullptr;
  m_snap_target_end = -1;

  bool is_endpoint =
      !m_selected->path->closed && m_node_drag_kind == HitResult::Kind::Node &&
      (m_selected_node == 0 ||
       m_selected_node == (int)m_selected->path->nodes.size() - 1);

  if (is_endpoint && m_doc) {
    static constexpr double ENDPOINT_SNAP_PX = 10.0; // screen pixels
    double best_d2 = ENDPOINT_SNAP_PX * ENDPOINT_SNAP_PX;

    // ── Same path: snap tail→head or head→tail (closing gesture) ─────
    {
      int n_nodes = (int)m_selected->path->nodes.size();
      int other_end_idx = (m_selected_node == 0) ? n_nodes - 1 : 0;
      if (n_nodes >= 2) {
        const BezierNode &other_ep = m_selected->path->nodes[other_end_idx];
        double sx, sy;
        doc_to_screen(other_ep.x, other_ep.y, sx, sy);
        double ddx = m_mouse_x - sx, ddy = m_mouse_y - sy;
        double d2 = ddx * ddx + ddy * ddy;
        if (d2 < best_d2) {
          best_d2 = d2;
          m_snap_target_obj = m_selected;
          m_snap_target_end = (other_end_idx == 0) ? 0 : 1;
          doc_pos = Vec2{other_ep.x, other_ep.y};
        }
      }
    }

    // ── Other paths: snap to their endpoints ──────────────────────────
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        SceneNode &other = *obj_uptr;
        if (&other == m_selected)
          continue;
        if (other.type != SceneNode::Type::Path || !other.path)
          continue;
        if (other.path->closed)
          continue;
        if (other.path->nodes.empty())
          continue;

        for (int end = 0; end < 2; ++end) {
          const BezierNode &ep =
              end == 0 ? other.path->nodes.front() : other.path->nodes.back();
          double sx, sy;
          doc_to_screen(ep.x, ep.y, sx, sy);
          double ddx = m_mouse_x - sx, ddy = m_mouse_y - sy;
          double d2 = ddx * ddx + ddy * ddy;
          if (d2 < best_d2) {
            best_d2 = d2;
            m_snap_target_obj = &other;
            m_snap_target_end = end;
            doc_pos = Vec2{ep.x, ep.y};
          }
        }
      }
    }
  }

  // ── Shift-axis constraint (Node + Handle drags) ───────────────────────
  // Anchor drag: lock to dominant axis from the pre-drag anchor position
  // (Affinity convention — shift = "I want a straight line").
  // Handle drag: lock to dominant axis from the ANCHOR. The anchor is the
  // natural reference because a handle's role is to point a direction
  // outward from its anchor; H/V-locking that direction is the predictable
  // analogue of the anchor's axis-lock. (Affinity uses 45° increments for
  // handles instead — flagged as a follow-up if we want that fidelity.)
  // Applied AFTER endpoint-snap so shift overrides snap.
  //
  // Snap on the LOCKED axis is bypassed below for the Node case — a
  // horizontal-lock with a horizontal guide near the start Y would
  // otherwise snap-pull the lock off-axis. Handles don't go through
  // snap_x/y in the existing code, so no snap-bypass plumbing for them.
  bool shift_lock_x = false; // true ⇒ X is locked, snap_x bypassed
  bool shift_lock_y = false; // true ⇒ Y is locked, snap_y bypassed
  if (m_mod_shift &&
      m_selected_node >= 0 &&
      m_selected_node < (int)m_node_drag_before.nodes.size() &&
      (m_node_drag_kind == HitResult::Kind::Node ||
       m_node_drag_kind == HitResult::Kind::HandleIn ||
       m_node_drag_kind == HitResult::Kind::HandleOut)) {
    const BezierNode &orig = m_node_drag_before.nodes[m_selected_node];
    // Reference is the anchor position for ALL three drag kinds. For
    // anchor drag this is the pre-drag anchor; for handle drag this is
    // the live anchor (handles never alter their own anchor, so the
    // pre-drag value is still the current value).
    double cdx = doc_pos.x - orig.x;
    double cdy = doc_pos.y - orig.y;
    if (std::abs(cdx) >= std::abs(cdy)) {
      doc_pos.y = orig.y;        // horizontal lock
      shift_lock_y = true;
    } else {
      doc_pos.x = orig.x;        // vertical lock
      shift_lock_x = true;
    }
  }

  switch (m_node_drag_kind) {
  case HitResult::Kind::Node:
    bp.move_node(m_selected_node,
                 {shift_lock_x ? doc_pos.x : snap_x(doc_pos.x),
                  shift_lock_y ? doc_pos.y : snap_y(doc_pos.y)});
    break;
  case HitResult::Kind::HandleIn:
    bp.move_handle_in(m_selected_node, doc_pos);
    break;
  case HitResult::Kind::HandleOut:
    bp.move_handle_out(m_selected_node, doc_pos);
    break;
  default:
    break;
  }
  *m_selected->path = bp.to_path_data();

  // ── Apply same delta to all other nodes in m_node_selection ──────────
  if (m_node_drag_kind == HitResult::Kind::Node && !m_node_selection.empty()) {
    const BezierNode &moved = m_selected->path->nodes[m_selected_node];
    double delta_x = moved.x - pre_x;
    double delta_y = moved.y - pre_y;
    for (const auto &ns : m_node_selection) {
      if (!ns.obj || !ns.obj->path)
        continue;
      if (ns.obj == m_selected && ns.node_idx == m_selected_node)
        continue;
      if (ns.node_idx < 0 || ns.node_idx >= (int)ns.obj->path->nodes.size())
        continue;
      BezierNode &nd = ns.obj->path->nodes[ns.node_idx];
      nd.x += delta_x;
      nd.y += delta_y;
      nd.cx1 += delta_x;
      nd.cy1 += delta_y;
      nd.cx2 += delta_x;
      nd.cy2 += delta_y;
    }
  }

  // Only emit if any coordinate actually changed
  const BezierNode &na = bp.nodes[m_selected_node];
  bool changed =
      (std::abs(na.x - pre_x) > 1e-9 || std::abs(na.y - pre_y) > 1e-9 ||
       std::abs(na.cx1 - pre_ix) > 1e-9 || std::abs(na.cy1 - pre_iy) > 1e-9 ||
       std::abs(na.cx2 - pre_ox) > 1e-9 || std::abs(na.cy2 - pre_oy) > 1e-9);
  if (changed)
    m_sig_node_changed.emit(m_selected, m_selected_node);
  queue_draw();
}

void Canvas::on_node_end() {
  // ── Node marquee end — select all nodes within rect ───────────────────
  if (m_marquee_active) {
    m_marquee_active = false;
    double x1 = std::min(m_marquee_start_dx, m_marquee_cur_dx);
    double y1 = std::min(m_marquee_start_dy, m_marquee_cur_dy);
    double x2 = std::max(m_marquee_start_dx, m_marquee_cur_dx);
    double y2 = std::max(m_marquee_start_dy, m_marquee_cur_dy);

    if (x2 - x1 > 1.0 && y2 - y1 > 1.0 && m_doc) {
      m_node_selection.clear();
      m_selected = nullptr;
      m_selected_node = -1;

      for (auto &layer : m_doc->layers) {
        if (!layer->visible || layer->locked || layer->is_special_layer())
          continue;
        for (auto &obj_uptr : layer->children) {
          SceneNode &obj = *obj_uptr;
          if (obj.type != SceneNode::Type::Path || !obj.path)
            continue;

          auto bb = object_bbox(obj);
          if (!bb)
            continue;
          bool path_hit = (bb->x < x2 && bb->x + bb->w > x1 && bb->y < y2 &&
                           bb->y + bb->h > y1);
          if (!path_hit)
            continue;

          if (!m_selected)
            m_selected = &obj;

          for (int i = 0; i < (int)obj.path->nodes.size(); ++i) {
            const BezierNode &nd = obj.path->nodes[i];
            if (nd.x >= x1 && nd.x <= x2 && nd.y >= y1 && nd.y <= y2) {
              m_node_selection.push_back({&obj, i});
              if (m_selected_node < 0 && &obj == m_selected)
                m_selected_node = i;
            }
          }
        }
      }

      if (m_selected) {
        m_sig_selection.emit(m_selected);
        m_sig_node_changed.emit(m_selected, m_selected_node);
      }
    }
    queue_draw();
    return;
  }

  if (m_node_drag_kind != HitResult::Kind::None && m_node_drag_started) {
    // Push undo for the drag
    if (m_history && m_selected && m_selected->path) {
      if (m_node_drag_before_multi.empty()) {
        // Single path drag
        m_history->push(std::make_unique<EditPathCommand>(
            m_selected, m_node_drag_before, *m_selected->path, "Move node"));
      } else {
        // Multi-path drag — use ScaleObjectsCommand to bundle all snaps
        std::vector<ScaleObjectsCommand::LeafSnap> snaps;
        snaps.push_back({m_selected, m_node_drag_before, *m_selected->path});
        for (auto &[obj, before] : m_node_drag_before_multi)
          if (obj->path)
            snaps.push_back({obj, before, *obj->path});
        m_history->push(std::make_unique<ScaleObjectsCommand>(std::move(snaps),
                                                              "Move nodes"));
      }
    }
    m_sig_doc_changed.emit();
    m_sig_node_changed.emit(m_selected, m_selected_node);

    // ── Auto-set secondary selection when endpoint snapped ────────────
    if (m_snap_target_obj && m_snap_target_obj->path &&
        m_snap_target_obj != m_selected) {
      m_selected2 = m_snap_target_obj;
      m_selected_node2 = (m_snap_target_end == 0)
                             ? 0
                             : (int)m_snap_target_obj->path->nodes.size() - 1;
      LOG_DEBUG("NodeTool: snap ended — secondary set to obj='{}' node={}",
                m_selected2->id, m_selected_node2);
    }
  }
  m_node_drag_kind = HitResult::Kind::None;
  m_node_drag_started = false;
}

void Canvas::apply_node_edit(int node_idx, double x, double y, double cx1,
                             double cy1, double cx2, double cy2) {
  if (!m_selected || !m_selected->path)
    return;
  if (node_idx < 0 || node_idx >= (int)m_selected->path->nodes.size())
    return;

  // Never overwrite path data during an active drag — on_node_update owns
  // the path during a drag and reads from *m_selected->path at the start of
  // every frame. A write here would corrupt the next frame's starting state.
  if (m_node_drag_started)
    return;

  const BezierNode &prev = m_selected->path->nodes[node_idx];
  LOG_DEBUG(
      "apply_node_edit: node={} type={} x={:.2f}→{:.2f} y={:.2f}→{:.2f} "
      "cx1={:.2f}→{:.2f} cy1={:.2f}→{:.2f} cx2={:.2f}→{:.2f} cy2={:.2f}→{:.2f}",
      node_idx, (int)prev.type, prev.x, x, prev.y, y, prev.cx1, cx1, prev.cy1,
      cy1, prev.cx2, cx2, prev.cy2, cy2);

  BezierNode &n = m_selected->path->nodes[node_idx];
  n.x = x;
  n.y = y;
  n.cx1 = cx1;
  n.cy1 = cy1;
  n.cx2 = cx2;
  n.cy2 = cy2;

  queue_draw();
}

void Canvas::set_selected_nodes_type(BezierNode::Type type) {
  if (m_node_selection.empty() || !m_selected || !m_selected->path)
    return;
  if (m_node_drag_started)
    return;

  // Snapshot before state for undo
  PathData before = *m_selected->path;

  BezierPath bp = BezierPath::from_path_data(*m_selected->path);
  for (const auto &ns : m_node_selection) {
    if (ns.obj == m_selected && ns.node_idx >= 0 &&
        ns.node_idx < (int)bp.nodes.size())
      bp.set_node_type(ns.node_idx, type);
  }
  *m_selected->path = bp.to_path_data();

  if (m_history)
    m_history->push(std::make_unique<EditPathCommand>(
        m_selected, std::move(before), *m_selected->path, "Set node type"));

  m_sig_doc_changed.emit();
  queue_draw();
}
void Canvas::on_draw(const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
  // ── Outer background — workspace colour around the artboard ─────────
  // Project-wide field (s116 m6) — every doc/tab in this project shares
  // the same workspace tone. Falls back to the active doc's legacy
  // bg field if the project pointer hasn't been wired (early boot),
  // and finally to the historical #171717 grey if neither is set.
  if (m_project) {
    cr->set_source_rgb(m_project->workspace_r(),
                       m_project->workspace_g(),
                       m_project->workspace_b());
  } else if (m_doc) {
    cr->set_source_rgb(m_doc->workspace_bg_r,
                       m_doc->workspace_bg_g,
                       m_doc->workspace_bg_b);
  } else {
    cr->set_source_rgb(0.09, 0.09, 0.09);
  }
  cr->paint();
  if (!m_doc)
    return;

  // Deferred fit — runs on first draw after set_document, when widget has real
  // size
  if (m_fit_pending && w > 0 && h > 0) {
    m_fit_pending = false;
    zoom_fit();
  }

  const double cw = m_doc->canvas_width() * m_zoom;
  const double ch = m_doc->canvas_height() * m_zoom;
  const double ox = doc_origin_x();
  const double oy = doc_origin_y();

  // Drop shadow — offset + blur-approximated with layered rects
  cr->save();
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.18);
  cr->rectangle(ox + 6, oy + 6, cw, ch);
  cr->fill();
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.25);
  cr->rectangle(ox + 3, oy + 3, cw, ch);
  cr->fill();
  cr->restore();

  cr->save();
  cr->translate(ox, oy);

  // ── Artboard surface — project-wide editor presentation (s116 m6) ────
  // Outline mode previously kept the same grey; that branch was a no-op
  // even before S98 (both arms set the same colour). Keep the gate in
  // case outline mode wants a different treatment in future, but read
  // the same artboard_bg_* either way for now.
  //
  // s116 m6: read from project (project-wide) instead of doc. Falls back
  // to legacy doc field if project pointer is unwired.
  if (m_project) {
    cr->set_source_rgb(m_project->artboard_r(),
                       m_project->artboard_g(),
                       m_project->artboard_b());
  } else {
    cr->set_source_rgb(m_doc->artboard_bg_r,
                       m_doc->artboard_bg_g,
                       m_doc->artboard_bg_b);
  }
  cr->rectangle(0, 0, cw, ch);
  cr->fill();

  draw_grid(cr, (int)cw, (int)ch);

  draw_objects(cr);

  cr->restore();

  // ── Artboard border — crisp 1px, clearly visible ──────────────────────
  cr->save();
  cr->translate(ox, oy);
  // Outer glow — subtle halo so edge reads against dark bg
  cr->set_source_rgba(0.6, 0.6, 0.6, 0.15);
  cr->set_line_width(3.0);
  cr->rectangle(0, 0, cw, ch);
  cr->stroke();
  // Crisp inner border
  cr->set_source_rgba(0.55, 0.55, 0.55, 1.0);
  cr->set_line_width(1.0);
  cr->rectangle(0.5, 0.5, cw - 1, ch - 1);
  cr->stroke();
  cr->restore();

  // ── Rubber band / marquee — pure widget space, no translate active ────
  draw_rubber_band(cr);
  draw_marquee(cr);

  // ── Origin drag preview — full-screen dashed crosshair ────────────────
  if (m_origin_preview && m_doc) {
    // Convert user coords to screen
    double doc_x = m_origin_preview_ux;
    double doc_y = m_doc->canvas_height() - m_origin_preview_uy;
    double sx = doc_x * m_zoom + ox;
    double sy = doc_y * m_zoom + oy;

    std::vector<double> dash = {6.0, 4.0};
    cr->save();
    cr->set_source_rgba(1.0, 0.65, 0.0, 0.85);
    cr->set_line_width(1.0);
    cr->set_dash(dash, 0);
    // Vertical line
    cr->move_to(sx + 0.5, 0);
    cr->line_to(sx + 0.5, h);
    cr->stroke();
    // Horizontal line
    cr->move_to(0, sy + 0.5);
    cr->line_to(w, sy + 0.5);
    cr->stroke();
    cr->restore();
  }

  // ── Selection handles — pure screen space, no translate active ────────
  auto obj_layer_visible = [this](SceneNode *obj) -> bool {
    if (!obj || !m_doc)
      return false;
    for (const auto &layer : m_doc->layers)
      for (const auto &child : layer->children)
        if (child.get() == obj)
          return layer->visible;
    return true;
  };

  // Draw outline for all selected objects
  // S66 Phase 3 — Eyedropper also shows the outline + dashed bbox (but
  // without draggable handles, via the bbox_only flag on
  // draw_selection_handles below). Gives visual confirmation of what
  // the eyedropper will apply to.
  if (m_tool == ActiveTool::Selection || m_tool == ActiveTool::Eyedropper) {
    cr->save();
    cr->translate(ox, oy);
    cr->scale(m_zoom, m_zoom);
    // S58g: Compounds now participate in the selection halo. A Compound is
    // one visual object (S58d) — selecting it should light up its full
    // outline, including the inner boundaries of any hole. We collect the
    // list of path-bearing SceneNodes for each selected object (Path → 1;
    // Compound → N children) and stroke them all together so the outer
    // contour and any inner rings both get the blue halo.
    for (SceneNode *obj : m_selection) {
      if (!obj_layer_visible(obj))
        continue;

      // Gather the leaf Path subpaths for this selection entry.
      std::vector<const SceneNode *> subpaths;
      if (obj->type == SceneNode::Type::Path && obj->path) {
        subpaths.push_back(obj);
      } else if (obj->type == SceneNode::Type::Compound) {
        for (const auto &ch : obj->children) {
          if (ch && ch->type == SceneNode::Type::Path && ch->path)
            subpaths.push_back(ch.get());
        }
      }
      if (subpaths.empty())
        continue;

      // Apply every subpath to Cairo, then stroke the whole batch twice —
      // once for the dark underlay, once for the blue halo.
      for (const SceneNode *sp : subpaths) {
        BezierPath bp = BezierPath::from_path_data(*sp->path);
        bp.apply_to_cairo(cr);
      }
      cr->set_source_rgba(0.0, 0.0, 0.0, 0.45);
      cr->set_line_width(4.0 / m_zoom);
      std::vector<double> no_dash = {};
      cr->set_dash(no_dash, 0);
      cr->stroke_preserve();
      // Primary object gets full blue, others get dimmer
      bool is_primary = (obj == m_selected);
      cr->set_source_rgba(0.3, 0.6, 1.0, is_primary ? 0.85 : 0.55);
      cr->set_line_width(2.0 / m_zoom);
      cr->stroke();
    }
    cr->restore();

    // ── Align anchor glyph (selection-time key-object marker) ────────
    // Drawn in screen space (translate already restored above) so the
    // glyph stays a constant size at any zoom — same convention as
    // selection handles. Validator-on-read: align_anchor() returns
    // null if the marked object has left the selection. Tool gate is
    // Selection only — Eyedropper shows selection outlines but anchor
    // is meaningless there. Glyph: curvz-anchor-symbolic.svg, loaded
    // once into m_anchor_glyph_pixbuf at 2x size for HiDPI crispness;
    // Cairo downscales for paint via curvz::utils::cairo_set_source_pixbuf,
    // the same pump NewDocumentDialog and DocumentGallery use (s135 m2).
    if (m_tool == ActiveTool::Selection) {
      if (SceneNode *a = align_anchor()) {
        if (obj_layer_visible(a)) {
          if (auto bb = object_bbox(*a)) {
            // Doc-space centre → screen
            double cx = bb->x + bb->w * 0.5;
            double cy = bb->y + bb->h * 0.5;
            double sx, sy;
            doc_to_screen(cx, cy, sx, sy);

            // Lazy-load the anchor pixbuf on first use. The SVG declares
            // width/height of 48 px so the pixbuf rasterises at 48×48 —
            // ample oversampling against the 22-px target. Cairo
            // bilinear-downscales for paint. If load fails (build missed
            // registering the resource), silently no-op and the anchor
            // still works, just without the glyph.
            if (!m_anchor_glyph_pixbuf) {
              try {
                m_anchor_glyph_pixbuf = Gdk::Pixbuf::create_from_resource(
                    "/com/curvz/app/icons/scalable/apps/"
                    "curvz-anchor-symbolic.svg");
              } catch (const Glib::Error &e) {
                LOG_WARN("Canvas: failed to load anchor glyph: {}", e.what());
              }
            }

            // Backdrop disc — keeps the glyph readable against any fill
            // and gives the anchor a visual frame consistent with how
            // selection handles read against busy artwork.
            cr->save();
            cr->arc(sx, sy, 14.0, 0.0, 2.0 * M_PI);
            cr->set_source_rgba(1.0, 1.0, 1.0, 0.85);
            cr->fill_preserve();
            cr->set_source_rgba(0.3, 0.6, 1.0, 0.95);
            cr->set_line_width(1.5);
            cr->stroke();

            if (m_anchor_glyph_pixbuf) {
              // Paint the SVG centred on (sx, sy) at 22 px (slight
              // padding inside the 28 px disc). Cairo composites the
              // 56 px source down to 22 px via bilinear; perfectly
              // crisp on 1x and 2x displays.
              const double kGlyphPx = 22.0;
              double pw = m_anchor_glyph_pixbuf->get_width();
              double ph = m_anchor_glyph_pixbuf->get_height();
              double scale = kGlyphPx / std::max(pw, ph);
              cr->translate(sx, sy);
              cr->scale(scale, scale);
              // s135 m2: pumped — gdk_cairo_set_source_pixbuf was deprecated
              // in GTK 4.20. The pump does a proper RGBA→ARGB32 conversion,
              // sidesteps the deprecation, and matches the per-call cost of
              // the old function.
              curvz::utils::cairo_set_source_pixbuf(cr,
                                                    m_anchor_glyph_pixbuf,
                                                    -pw * 0.5, -ph * 0.5);
              cr->paint();
            }
            cr->restore();
          }
        }
      }
    }

    // Draw one union-BBX handle set for the whole selection.
    // M4c-1: Suppressed when primary selection is a Warp — envelope handles
    // ARE the manipulation UI for Warps; bbox handles get in the way and
    // don't track envelope edits. User flattens first for scale/rotate/skew.
    // S66 Phase 3: Eyedropper shows the same handle UI as Selection for
    // visual consistency; handles are inert under the eyedropper (click
    // commits the sample rather than grabbing a handle).
    if (!m_selection.empty() && !(m_selected && m_selected->is_warp()))
      draw_selection_handles(cr);
  } else if (m_tool == ActiveTool::Node && m_selected &&
             obj_layer_visible(m_selected)) {
    // Node tool — outline primary selected path only.
    // Compounds aren't directly editable in the Node tool (children must be
    // reached via split-compound first), so we keep this branch Path-only.
    if (m_selected->type == SceneNode::Type::Path && m_selected->path) {
      cr->save();
      cr->translate(ox, oy);
      cr->scale(m_zoom, m_zoom);
      BezierPath bp = BezierPath::from_path_data(*m_selected->path);
      bp.apply_to_cairo(cr);
      cr->set_source_rgba(0.0, 0.0, 0.0, 0.45);
      cr->set_line_width(4.0 / m_zoom);
      std::vector<double> no_dash = {};
      cr->set_dash(no_dash, 0);
      cr->stroke_preserve();
      cr->set_source_rgba(0.3, 0.6, 1.0, 0.85);
      cr->set_line_width(2.0 / m_zoom);
      cr->stroke();
      cr->restore();
    }
  }

  // ── Pen tool WIP — document space ─────────────────────────────────────
  if (m_tool == ActiveTool::Pen && m_pen_tool.has_wip) {
    cr->save();
    cr->translate(ox, oy);
    cr->scale(m_zoom, m_zoom);
    // s137 m5: pass project's motif-resolved Creation colour through.
    // PenTool stays project-agnostic; receives values, doesn't reach
    // back into the project.
    m_pen_tool.draw_preview(cr, m_zoom,
                            m_project->creation_r(),
                            m_project->creation_g(),
                            m_project->creation_b());
    cr->restore();
  }

  // ── Continue-path hover indicator ─────────────────────────────────────
  if (m_tool == ActiveTool::Pen && !m_pen_tool.has_wip && m_doc) {
    double doc_x, doc_y;
    screen_to_doc(m_mouse_x, m_mouse_y, doc_x, doc_y);
    double tol = PenTool::CLOSE_RADIUS_PX / m_zoom;
    Vec2 mouse{doc_x, doc_y};
    cr->save();
    cr->translate(ox, oy);
    cr->scale(m_zoom, m_zoom);
    for (const auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (const auto &obj_ptr : layer->children) {
        const SceneNode &obj = *obj_ptr;
        if (obj.type != SceneNode::Type::Path || !obj.path)
          continue;

        // ── Endpoint continue indicator (amber ring) ──────────────
        if (!obj.path->closed && obj.path->nodes.size() >= 2) {
          for (int ei = 0; ei < 2; ++ei) {
            const BezierNode &ep =
                ei == 0 ? obj.path->nodes.front() : obj.path->nodes.back();
            Vec2 ep_pos{ep.x, ep.y};
            if (mouse.dist(ep_pos) <= tol) {
              cr->arc(ep.x, ep.y, PenTool::CLOSE_RADIUS_PX / m_zoom, 0,
                      2 * M_PI);
              cr->set_source_rgba(1.0, 0.7, 0.0, 0.9);
              cr->set_line_width(1.5 / m_zoom);
              cr->stroke();
            }
          }
        }

        // ── Node snap indicator (cyan ring) ───────────────────────
        for (const auto &nd : obj.path->nodes) {
          double sx, sy;
          doc_to_screen(nd.x, nd.y, sx, sy);
          double ddx = m_mouse_x - sx, ddy = m_mouse_y - sy;
          double d2 = ddx * ddx + ddy * ddy;
          static constexpr double NODE_SNAP_PX = 6.0;
          if (d2 < NODE_SNAP_PX * NODE_SNAP_PX) {
            cr->arc(nd.x, nd.y, NODE_SNAP_PX / m_zoom, 0, 2 * M_PI);
            cr->set_source_rgba(0.0, 0.85, 1.0, 0.9); // cyan
            cr->set_line_width(1.5 / m_zoom);
            cr->stroke();
          }
        }
      }
    }
    cr->restore();
  }

  // ── Node editor overlay — document space ──────────────────────────────
  if (m_tool == ActiveTool::Node && m_selected &&
      m_selected->type == SceneNode::Type::Path && m_selected->path) {
    cr->save();
    cr->translate(ox, oy);
    cr->scale(m_zoom, m_zoom);

    // ── Background paths — all non-active paths in layer colour ──────────
    // Drawn before the node overlay so they sit behind handles/nodes.
    // m_selected and any path in m_node_selection are skipped (they get
    // the blue overlay treatment below).
    {
      std::set<SceneNode *> active;
      active.insert(m_selected);
      for (const auto &ns : m_node_selection)
        if (ns.obj)
          active.insert(ns.obj);

      for (int li = 0; li < (int)m_doc->layers.size(); ++li) {
        const auto &layer = m_doc->layers[li];
        if (!layer->visible || layer->is_special_layer())
          continue;
        double lr = 0.88, lg = 0.88, lb = 0.88;
        parse_layer_color(layer->color, lr, lg, lb);
        for (const auto &obj_ptr : layer->children) {
          const SceneNode &obj = *obj_ptr;
          if (obj.type != SceneNode::Type::Path || !obj.path)
            continue;
          if (active.count(const_cast<SceneNode *>(&obj)))
            continue;
          BezierPath bp = BezierPath::from_path_data(*obj.path);
          bp.apply_to_cairo(cr);
          cr->set_source_rgba(lr, lg, lb, 0.40);
          cr->set_line_width(1.0 / m_zoom);
          cr->stroke();
        }
      }
    }

    // Draw overlays for extra paths in m_node_selection first (dimmer)
    std::set<SceneNode *> drawn;
    drawn.insert(m_selected);
    for (const auto &ns : m_node_selection) {
      if (!ns.obj || !ns.obj->path)
        continue;
      if (drawn.count(ns.obj))
        continue;
      drawn.insert(ns.obj);
      BezierPath extra = BezierPath::from_path_data(*ns.obj->path);
      // Draw path outline
      extra.apply_to_cairo(cr);
      cr->set_source_rgba(0.3, 0.6, 1.0, 0.55);
      cr->set_line_width(1.5 / m_zoom);
      cr->stroke();
      // Build selected set for this extra path
      std::unordered_set<int> extra_sel;
      for (const auto &ns2 : m_node_selection)
        if (ns2.obj == ns.obj && ns2.node_idx >= 0)
          extra_sel.insert(ns2.node_idx);
      extra.draw_editor_overlay(cr, m_zoom, extra_sel, extra.closed);
    }

    // Draw primary path overlay on top
    BezierPath bp = BezierPath::from_path_data(*m_selected->path);

    // Build set of selected node indices on the primary path
    std::unordered_set<int> sel_indices;
    if (m_selected_node >= 0)
      sel_indices.insert(m_selected_node);
    for (const auto &ns : m_node_selection) {
      if (ns.obj == m_selected && ns.node_idx >= 0)
        sel_indices.insert(ns.node_idx);
    }
    if (sel_indices.size() > 1)
      bp.draw_editor_overlay(cr, m_zoom, sel_indices, bp.closed);
    else
      bp.draw_editor_overlay(cr, m_zoom, m_selected_node, bp.closed);

    // ── Secondary selection — highlight node on second path ────────────
    if (m_selected2 && m_selected2->path && m_selected_node2 >= 0 &&
        m_selected_node2 < (int)m_selected2->path->nodes.size()) {
      const BezierNode &n2 = m_selected2->path->nodes[m_selected_node2];
      double r = 5.0 / m_zoom;
      cr->arc(n2.x, n2.y, r, 0, 2 * M_PI);
      cr->set_source_rgba(1.0, 0.6, 0.1, 0.9);
      cr->set_line_width(2.0 / m_zoom);
      cr->stroke();
    }

    // ── Multi-node selection on EXTRA paths — drawn by extra path loop above
    // ── (nodes on m_selected are handled by the set-based draw_editor_overlay)

    // ── Endpoint snap indicator — green ring around target endpoint ───
    if (m_snap_target_obj && m_snap_target_obj->path) {
      const auto &nodes = m_snap_target_obj->path->nodes;
      const BezierNode &ep =
          m_snap_target_end == 0 ? nodes.front() : nodes.back();
      double r = 6.0 / m_zoom;
      cr->arc(ep.x, ep.y, r, 0, 2 * M_PI);
      cr->set_source_rgba(0.2, 1.0, 0.4, 0.9);
      cr->set_line_width(1.5 / m_zoom);
      cr->stroke();
    }
    cr->restore();
  }

  // ── Ref tool coordinate overlay — only when Alt or Shift held ────────────
  if (m_tool == ActiveTool::Ref && (m_mod_alt || m_mod_shift))
    draw_ref_coord_overlay(cr);

  // ── Eyedropper colour swatch overlay ──────────────────────────────
  if (m_tool == ActiveTool::Eyedropper)
    draw_eyedropper_overlay(cr);

  // ── Corner tool overlay ───────────────────────────────────────────
  if (m_tool == ActiveTool::Corner)
    draw_corner_tool_overlay(cr);

  // ── Ruler tool overlay ────────────────────────────────────────────────
  if (m_tool == ActiveTool::Ruler)
    draw_ruler_overlay(cr, w, h);

  // ── Text-on-Path tool overlay ─────────────────────────────────────────
  if (m_tool == ActiveTool::TextOnPath)
    draw_top_overlay(cr);

  // ── Guide-construct overlay (any tool, pre-empts visual focus) ────────
  if (m_guide_construct_active)
    draw_guide_construct_overlay(cr);

  // ── Persistent measurement overlays (drawn in all tools) ──────────────
  // S89: rendered with the same shared draw_measurement_annotations helper
  // that the live ruler triangle uses, so saved entries look identical to
  // the live one — full structured labels (x,y at A and B / Δx / Δy /
  // distance / α / β) plus triangle + endpoints. Click-to-copy is wired
  // when the ruler tool is active by passing push_labels=true so the
  // labels register hit-test rects in m_ruler_labels.
  if (m_doc) {
    SceneNode *ml = m_doc->measure_layer();
    if (ml && ml->visible) {
      bool ruler_active = (m_tool == ActiveTool::Ruler);
      for (auto &ch : ml->children) {
        if (!ch->is_measurement())
          continue;
        // Per-entry visibility — inspector and layers-panel eye toggles
        // flip ch->visible. Whole-layer visibility is the outer ml->visible
        // gate above; this skips individual entries that the user has hidden.
        if (!ch->visible)
          continue;
        // measure_* are user-space (Y-up) — pass straight through.
        draw_measurement_annotations(cr,
                                     ch->measure_x1, ch->measure_y1,
                                     ch->measure_x2, ch->measure_y2,
                                     /*push_labels=*/ruler_active);
      }
    }
  }

  // ── Warp envelope overlay (M4a — display-only) ────────────────────────
  // When a Warp is the primary selection, paint its top/bottom envelope
  // anchors + handles + leashes on top of everything. Screen-space sizes
  // (8px anchors, 6px handle dots) so they stay constant under zoom.
  // Top envelope = orange, Bottom envelope = cyan — different colors so
  // the user can always tell them apart. M4b adds hit-test + drag.
  // M4c-2: elements in the pick set render in bright yellow over the
  // base color so the selection is obvious.
  if (m_selected && m_selected->is_warp()) {
    sync_warp_env_picks_to_selection();
    const SceneNode &wp = *m_selected;
    // Pick-set membership check. Linear over picks — always small.
    auto is_picked = [&](bool is_top, int idx, EnvelopePart part) -> bool {
      for (const auto &p : m_warp_env_picks)
        if (p.is_top == is_top && p.idx == idx && p.part == part)
          return true;
      return false;
    };
    auto draw_envelope = [&](const PathData &env, bool is_top, double r,
                             double g, double b) {
      if (env.nodes.empty())
        return;
      cr->save();
      // Leashes: anchor→cx1 (incoming) and anchor→cx2 (outgoing).
      // Thin, low-alpha version of the envelope color.
      cr->set_source_rgba(r, g, b, 0.55);
      cr->set_line_width(1.0);
      std::vector<double> no_dash;
      cr->set_dash(no_dash, 0);
      for (const auto &n : env.nodes) {
        double asx, asy, h1sx, h1sy, h2sx, h2sy;
        doc_to_screen(n.x, n.y, asx, asy);
        doc_to_screen(n.cx1, n.cy1, h1sx, h1sy);
        doc_to_screen(n.cx2, n.cy2, h2sx, h2sy);
        // Only draw the leash if the handle isn't coincident with anchor
        // (avoids drawing zero-length segments for degenerate cases).
        if (std::hypot(h1sx - asx, h1sy - asy) > 0.5) {
          cr->move_to(asx, asy);
          cr->line_to(h1sx, h1sy);
          cr->stroke();
        }
        if (std::hypot(h2sx - asx, h2sy - asy) > 0.5) {
          cr->move_to(asx, asy);
          cr->line_to(h2sx, h2sy);
          cr->stroke();
        }
      }
      // Handle dots: 6px. Picked → yellow fill + bold black ring.
      // Unpicked → white fill + colored ring.
      for (int i = 0; i < (int)env.nodes.size(); ++i) {
        const BezierNode &n = env.nodes[i];
        double h1sx, h1sy, h2sx, h2sy, asx, asy;
        doc_to_screen(n.x, n.y, asx, asy);
        doc_to_screen(n.cx1, n.cy1, h1sx, h1sy);
        doc_to_screen(n.cx2, n.cy2, h2sx, h2sy);
        auto draw_handle = [&](double sx, double sy, bool picked) {
          if (std::hypot(sx - asx, sy - asy) < 0.5)
            return; // coincident
          if (picked) {
            cr->set_source_rgba(1.0, 0.93, 0.20, 1.0); // yellow
            cr->arc(sx, sy, 3.5, 0, 2 * M_PI);
            cr->fill();
            cr->set_source_rgba(0.0, 0.0, 0.0, 1.0);
            cr->set_line_width(1.5);
            cr->arc(sx, sy, 3.5, 0, 2 * M_PI);
            cr->stroke();
          } else {
            cr->set_source_rgba(1.0, 1.0, 1.0, 0.95);
            cr->arc(sx, sy, 3.0, 0, 2 * M_PI);
            cr->fill();
            cr->set_source_rgba(r, g, b, 1.0);
            cr->set_line_width(1.5);
            cr->arc(sx, sy, 3.0, 0, 2 * M_PI);
            cr->stroke();
          }
        };
        draw_handle(h1sx, h1sy, is_picked(is_top, i, EnvelopePart::HandleIn));
        draw_handle(h2sx, h2sy, is_picked(is_top, i, EnvelopePart::HandleOut));
      }
      // Anchor squares: 8px. Picked → yellow fill + bolder black outline.
      // Unpicked → envelope-color fill + thin black outline.
      for (int i = 0; i < (int)env.nodes.size(); ++i) {
        const BezierNode &n = env.nodes[i];
        double asx, asy;
        doc_to_screen(n.x, n.y, asx, asy);
        bool picked = is_picked(is_top, i, EnvelopePart::Anchor);
        if (picked) {
          cr->set_source_rgba(1.0, 0.93, 0.20, 1.0); // yellow
          cr->rectangle(asx - 5.0, asy - 5.0, 10.0, 10.0);
          cr->fill_preserve();
          cr->set_source_rgba(0.0, 0.0, 0.0, 1.0);
          cr->set_line_width(1.5);
          cr->stroke();
        } else {
          cr->set_source_rgba(r, g, b, 1.0);
          cr->rectangle(asx - 4.0, asy - 4.0, 8.0, 8.0);
          cr->fill_preserve();
          cr->set_source_rgba(0.0, 0.0, 0.0, 0.9);
          cr->set_line_width(1.0);
          cr->stroke();
        }
      }
      cr->restore();
    };
    // Top envelope = orange. Bottom envelope = cyan.
    draw_envelope(wp.warp_env_top, true, 1.0, 0.55, 0.10);
    draw_envelope(wp.warp_env_bottom, false, 0.15, 0.70, 0.95);
  }
}

// ── Corner Treatment Tool
// ─────────────────────────────────────────────────────

bool Canvas::corner_sel_contains(SceneNode *obj, int idx) const {
  for (auto &cs : m_corner_selection)
    if (cs.obj == obj && cs.node_idx == idx)
      return true;
  return false;
}

static void foreach_corner_node(CurvzDocument *doc,
                                std::function<bool(SceneNode *, int)> fn) {
  if (!doc)
    return;
  std::function<void(SceneNode *)> walk = [&](SceneNode *node) {
    if (node->type == SceneNode::Type::Path && node->path) {
      for (int i = 0; i < (int)node->path->nodes.size(); ++i) {
        auto t = node->path->nodes[i].type;
        if (t == BezierNode::Type::Corner || t == BezierNode::Type::Cusp)
          if (fn(node, i))
            return;
      }
    } else if (node->type == SceneNode::Type::Group ||
               node->type == SceneNode::Type::Compound) {
      for (auto &ch : node->children)
        walk(ch.get());
    }
  };
  for (auto &layer : doc->layers) {
    if (!layer->visible || layer->locked || layer->is_special_layer())
      continue;
    for (auto &obj : layer->children)
      walk(obj.get());
  }
}

void Canvas::on_corner_begin(double x, double y) {
  if (!m_doc)
    return;

  double dx, dy;
  screen_to_doc(x, y, dx, dy);
  Vec2 doc_pos{dx, dy};
  double tol = 8.0 / m_zoom;

  // Try to hit a Corner/Cusp node
  SceneNode *hit_obj = nullptr;
  int hit_idx = -1;
  double hit_d2 = tol * tol;

  foreach_corner_node(m_doc, [&](SceneNode *obj, int i) {
    Vec2 np{obj->path->nodes[i].x, obj->path->nodes[i].y};
    double d2 = np.dist_sq(doc_pos);
    if (d2 < hit_d2) {
      hit_d2 = d2;
      hit_obj = obj;
      hit_idx = i;
    }
    return false; // keep scanning for nearest
  });

  if (hit_obj) {
    if (m_mod_shift) {
      // Shift+click: toggle
      auto it =
          std::find_if(m_corner_selection.begin(), m_corner_selection.end(),
                       [&](const CornerSel &cs) {
                         return cs.obj == hit_obj && cs.node_idx == hit_idx;
                       });
      if (it != m_corner_selection.end())
        m_corner_selection.erase(it);
      else
        m_corner_selection.push_back({hit_obj, hit_idx});
    } else {
      // Plain click: select only this node (unless it was already selected)
      bool already = corner_sel_contains(hit_obj, hit_idx);
      if (!already) {
        m_corner_selection.clear();
        m_corner_selection.push_back({hit_obj, hit_idx});
      }
    }
    m_corner_rubber_active = false;
  } else {
    // Missed all nodes — start rubber-band (clears selection unless Shift)
    if (!m_mod_shift)
      m_corner_selection.clear();
    m_corner_rubber_active = true;
    m_corner_rubber_x0 = x;
    m_corner_rubber_y0 = y;
    m_corner_rubber_x1 = x;
    m_corner_rubber_y1 = y;
  }

  m_drawing = true; // enable drag gesture routing
  m_sig_corner_sel_changed.emit((int)m_corner_selection.size());
  queue_draw();
}

void Canvas::on_corner_motion(double /*x*/, double /*y*/) {
  if (!m_doc)
    return;
  if (m_corner_rubber_active) {
    m_corner_rubber_x1 = m_mouse_x;
    m_corner_rubber_y1 = m_mouse_y;
    queue_draw();
  }
}

void Canvas::on_corner_end(double /*x*/, double /*y*/) {
  if (!m_doc)
    return;

  if (m_corner_rubber_active) {
    // Finalise rubber-band: select all Corner/Cusp nodes inside the rect
    m_corner_rubber_active = false;

    double sx0 = std::min(m_corner_rubber_x0, m_mouse_x);
    double sy0 = std::min(m_corner_rubber_y0, m_mouse_y);
    double sx1 = std::max(m_corner_rubber_x0, m_mouse_x);
    double sy1 = std::max(m_corner_rubber_y0, m_mouse_y);

    foreach_corner_node(m_doc, [&](SceneNode *obj, int i) {
      double sx, sy;
      doc_to_screen(obj->path->nodes[i].x, obj->path->nodes[i].y, sx, sy);
      if (sx >= sx0 && sx <= sx1 && sy >= sy0 && sy <= sy1) {
        if (!corner_sel_contains(obj, i))
          m_corner_selection.push_back({obj, i});
      }
      return false;
    });

    m_sig_corner_sel_changed.emit((int)m_corner_selection.size());
    queue_draw();
  }
}

void Canvas::apply_corner_treatment_op(CornerType type, double radius) {
  if (m_corner_selection.empty() || !m_doc || !m_history)
    return;

  // Group selected nodes by path object
  std::unordered_map<SceneNode *, std::unordered_set<int>> by_obj;
  for (auto &cs : m_corner_selection)
    by_obj[cs.obj].insert(cs.node_idx);

  auto cmd = std::make_unique<CornerTreatmentCommand>();
  bool any = false;

  for (auto &[obj, indices] : by_obj) {
    if (!obj->path)
      continue;
    PathData before = *obj->path;
    PathData after =
        Curvz::apply_corner_treatment(*obj->path, indices, type, radius);
    // Only record if geometry actually changed
    if (after.nodes.size() != before.nodes.size()) {
      cmd->add(obj, std::move(before), after);
      *obj->path = std::move(after);
      any = true;
    }
  }

  if (any) {
    m_history->push(std::move(cmd));
    m_sig_doc_changed.emit();
    queue_draw();
  }

  // Clear selection and notify panel to hide
  m_corner_selection.clear();
  m_sig_corner_sel_changed.emit(0);
  queue_draw();
}

void Canvas::draw_corner_tool_overlay(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_doc)
    return;

  double ox = doc_origin_x();
  double oy = doc_origin_y();

  cr->save();
  cr->translate(ox, oy);
  cr->scale(m_zoom, m_zoom);

  const double node_r = 4.0 / m_zoom;

  // Draw all paths faintly so user can see geometry
  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->is_special_layer())
      continue;
    double lr = 0.78, lg = 0.78, lb = 0.78;
    parse_layer_color(layer->color, lr, lg, lb);
    std::function<void(SceneNode *)> draw_path = [&](SceneNode *node) {
      if (node->type == SceneNode::Type::Path && node->path) {
        BezierPath bp = BezierPath::from_path_data(*node->path);
        bp.apply_to_cairo(cr);
        cr->set_source_rgba(lr, lg, lb, 0.35);
        cr->set_line_width(1.0 / m_zoom);
        cr->stroke();
      } else if (node->type == SceneNode::Type::Group ||
                 node->type == SceneNode::Type::Compound) {
        for (auto &ch : node->children)
          draw_path(ch.get());
      }
    };
    for (auto &obj : layer->children)
      draw_path(obj.get());
  }

  // Draw nodes: Corner/Cusp are orange squares; other types are dimmed grey
  // dots
  foreach_corner_node(m_doc, [&](SceneNode *obj, int i) {
    const BezierNode &n = obj->path->nodes[i];
    bool selected = corner_sel_contains(obj, i);

    if (selected) {
      // Filled blue square
      cr->set_source_rgba(0.2, 0.5, 1.0, 1.0);
      cr->rectangle(n.x - node_r, n.y - node_r, node_r * 2, node_r * 2);
      cr->fill_preserve();
      cr->set_source_rgba(1.0, 1.0, 1.0, 1.0);
      cr->set_line_width(1.0 / m_zoom);
      cr->stroke();
    } else {
      // Orange hollow square — selectable corner/cusp
      cr->set_source_rgba(1.0, 0.55, 0.1, 0.9);
      cr->rectangle(n.x - node_r, n.y - node_r, node_r * 2, node_r * 2);
      cr->set_line_width(1.0 / m_zoom);
      cr->stroke();
    }
    return false;
  });

  // Dim non-corner nodes with small grey circles so user can see them but
  // knows they are not selectable
  auto draw_inert = [&](SceneNode *node) {
    std::function<void(SceneNode *)> walk = [&](SceneNode *n) {
      if (n->type == SceneNode::Type::Path && n->path) {
        for (auto &bn : n->path->nodes) {
          auto t = bn.type;
          if (t == BezierNode::Type::Corner || t == BezierNode::Type::Cusp)
            continue; // already drawn above
          cr->arc(bn.x, bn.y, node_r * 0.7, 0, 2 * M_PI);
          cr->set_source_rgba(0.6, 0.6, 0.6, 0.4);
          cr->fill();
        }
      } else if (n->type == SceneNode::Type::Group ||
                 n->type == SceneNode::Type::Compound) {
        for (auto &ch : n->children)
          walk(ch.get());
      }
    };
    walk(node);
  };
  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->is_special_layer())
      continue;
    for (auto &obj : layer->children)
      draw_inert(obj.get());
  }

  cr->restore();

  // Rubber-band rect (screen space, outside translate/scale)
  if (m_corner_rubber_active) {
    double x0 = std::min(m_corner_rubber_x0, m_corner_rubber_x1);
    double y0 = std::min(m_corner_rubber_y0, m_corner_rubber_y1);
    double x1 = std::max(m_corner_rubber_x0, m_corner_rubber_x1);
    double y1 = std::max(m_corner_rubber_y0, m_corner_rubber_y1);
    cr->save();
    cr->rectangle(x0, y0, x1 - x0, y1 - y0);
    cr->set_source_rgba(0.3, 0.6, 1.0, 0.12);
    cr->fill_preserve();
    cr->set_source_rgba(0.3, 0.6, 1.0, 0.8);
    cr->set_line_width(1.0);
    std::vector<double> dash = {4.0, 3.0};
    cr->set_dash(dash, 0);
    cr->stroke();
    cr->restore();
  }
}

// ── draw_guide_construct_overlay ───────────────────────────────────────────
// Renders the two-point guide construction preview:
//   Phase 0: only cursor hint
//   Phase 1: marker at p1, dashed preview segment p1 → mouse
//   Phase 2: markers at p1 + p2, infinite-line preview at committed vector
//            (orange).  If perpendicular, draw the perpendicular through the
//            midpoint instead (dashed style to signal "alternate mode").
//
// Additionally, in phases 0 and 1 (when the user still has nodes to click),
// overlay node markers on every visible path so the user sees every legal
// snap target, with the currently-nearest-within-tolerance node highlighted.
void Canvas::draw_guide_construct_overlay(
    const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_guide_construct_active || !m_doc)
    return;

  // ── Node markers on every visible non-special layer (snap targets) ────
  // Only while the user still has nodes to click (phases 0 and 1).  Same
  // visual idiom as the Ruler tool overlay.
  if (m_guide_construct_phase < 2) {
    // Determine which node (if any) is currently the snap candidate, so we
    // can highlight it.  Snap tolerance mirrors the press / motion code:
    // 8 px / zoom = 8 doc units when zoom == 1.
    const double tol = 8.0 / m_zoom;
    SceneNode *hot_obj = nullptr;
    int hot_idx = -1;
    double best_d = tol;
    std::vector<std::pair<SceneNode *, int>> all_nodes;
    ruler_collect_all_path_nodes(all_nodes);
    for (auto &[obj, ni] : all_nodes) {
      const BezierNode &n = obj->path->nodes[ni];
      double d = std::hypot(n.x - m_cursor_doc_x, n.y - m_cursor_doc_y);
      if (d < best_d) {
        best_d = d;
        hot_obj = obj;
        hot_idx = ni;
      }
    }

    cr->save();
    const double ox = doc_origin_x();
    const double oy = doc_origin_y();
    cr->translate(ox, oy);
    cr->scale(m_zoom, m_zoom);
    std::vector<double> no_dash;
    cr->set_dash(no_dash, 0);

    const double ns = 3.5 / m_zoom; // half-size in doc units
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        const SceneNode &obj = *obj_uptr;
        if (obj.type != SceneNode::Type::Path || !obj.path)
          continue;
        for (int ni = 0; ni < (int)obj.path->nodes.size(); ++ni) {
          const BezierNode &nd = obj.path->nodes[ni];
          bool is_hot = (&obj == hot_obj && ni == hot_idx);
          if (is_hot) {
            // Snap candidate — filled orange, larger.
            const double hs = ns * 1.4;
            cr->set_source_rgba(1.0, 0.55, 0.0, 1.0);
            cr->rectangle(nd.x - hs, nd.y - hs, hs * 2, hs * 2);
            cr->fill();
          } else {
            cr->set_source_rgba(0.85, 0.85, 0.85, 0.9);
            cr->rectangle(nd.x - ns, nd.y - ns, ns * 2, ns * 2);
            cr->fill();
            cr->set_source_rgba(0.4, 0.4, 0.4, 0.9);
            cr->set_line_width(0.75 / m_zoom);
            cr->rectangle(nd.x - ns, nd.y - ns, ns * 2, ns * 2);
            cr->stroke();
          }
        }
      }
    }
    cr->restore();
  }

  auto draw_dot = [&](double dx, double dy, double r, double g, double b) {
    double sx, sy;
    doc_to_screen(dx, dy, sx, sy);
    cr->save();
    cr->set_source_rgba(0.0, 0.0, 0.0, 0.6);
    cr->arc(sx, sy, 5.0, 0, 2 * M_PI);
    cr->fill();
    cr->set_source_rgba(r, g, b, 1.0);
    cr->arc(sx, sy, 4.0, 0, 2 * M_PI);
    cr->fill();
    cr->restore();
  };

  auto draw_preview_line = [&](double ax, double ay, double bx, double by,
                               bool dashed, bool infinite) {
    double asx, asy, bsx, bsy;
    doc_to_screen(ax, ay, asx, asy);
    doc_to_screen(bx, by, bsx, bsy);
    cr->save();
    cr->set_source_rgba(1.0, 0.55, 0.0, 0.9);
    cr->set_line_width(1.5);
    if (dashed) {
      std::vector<double> dash = {6.0, 4.0};
      cr->set_dash(dash, 0);
    }
    if (infinite) {
      // Extend the line past the widget bounds in both directions.
      double w = get_width();
      double h = get_height();
      double span = std::hypot(w, h) * 2.0 + 1000.0;
      double dx = bsx - asx;
      double dy = bsy - asy;
      double len = std::hypot(dx, dy);
      if (len < 1e-6) {
        cr->restore();
        return;
      }
      double ux = dx / len;
      double uy = dy / len;
      double mx = (asx + bsx) * 0.5;
      double my = (asy + bsy) * 0.5;
      cr->move_to(mx - ux * span, my - uy * span);
      cr->line_to(mx + ux * span, my + uy * span);
    } else {
      cr->move_to(asx, asy);
      cr->line_to(bsx, bsy);
    }
    cr->stroke();
    cr->restore();
  };

  if (m_guide_construct_phase == 1) {
    // p1 captured, previewing to current mouse.
    draw_preview_line(m_guide_construct_p1_x, m_guide_construct_p1_y,
                      m_guide_construct_preview_x, m_guide_construct_preview_y,
                      /*dashed=*/true, /*infinite=*/false);
    draw_dot(m_guide_construct_p1_x, m_guide_construct_p1_y, 1.0, 0.55, 0.0);
    draw_dot(m_guide_construct_preview_x, m_guide_construct_preview_y, 1.0, 0.8,
             0.2);
  } else if (m_guide_construct_phase >= 2) {
    // Locked preview: show vector from p1→p2, then the infinite proposal line
    // (dashed if perpendicular is active — hints that the proposal line is
    // NOT along the clicked vector).
    draw_preview_line(m_guide_construct_p1_x, m_guide_construct_p1_y,
                      m_guide_construct_p2_x, m_guide_construct_p2_y,
                      /*dashed=*/true, /*infinite=*/false);
    // Compute midpoint + direction for the infinite proposal line.
    const double mx = (m_guide_construct_p1_x + m_guide_construct_p2_x) * 0.5;
    const double my = (m_guide_construct_p1_y + m_guide_construct_p2_y) * 0.5;
    double dx = m_guide_construct_p2_x - m_guide_construct_p1_x;
    double dy = m_guide_construct_p2_y - m_guide_construct_p1_y;
    if (m_guide_construct_perpendicular) {
      // Rotate 90° in doc-Y-down space.
      double tmp = dx;
      dx = -dy;
      dy = tmp;
    }
    draw_preview_line(mx - dx, my - dy, mx + dx, my + dy,
                      /*dashed=*/false, /*infinite=*/true);
    draw_dot(m_guide_construct_p1_x, m_guide_construct_p1_y, 1.0, 0.55, 0.0);
    draw_dot(m_guide_construct_p2_x, m_guide_construct_p2_y, 1.0, 0.55, 0.0);
    // Midpoint marker — smaller, cyan-ish, to show anchor of the commit.
    draw_dot(mx, my, 0.3, 0.85, 1.0);
  }
}

void Canvas::draw_eyedropper_overlay(const Cairo::RefPtr<Cairo::Context> &cr) {
  // S66 — Phase 3. The old solid-swatch overlay was replaced by the loupe.
  // Keep this entry point stable (on_draw still calls it) and dispatch.
  draw_eyedropper_loupe(cr);
}

void Canvas::refresh_loupe_buffer() {
  // Render a small screen-space region centred on the cursor into a scratch
  // ImageSurface. Reads back the centre pixel into m_loupe_buffer_{rgba}.
  // The surface is kept for draw_eyedropper_loupe's magnified display.
  //
  // Source-pixel footprint: fixed 15×15 source pixels around the cursor.
  // That's 7 on each side. At 10× magnification the drawn loupe body is
  // 150×150 — a touch larger than the 120-circle diameter so the circle
  // crops nicely and the centre pixel sits dead-on the cursor.
  if (!m_doc) {
    m_loupe_buffer_valid = false;
    return;
  }
  const int W = get_allocated_width();
  const int H = get_allocated_height();
  if (W <= 0 || H <= 0) {
    m_loupe_buffer_valid = false;
    return;
  }

  const int SAMPLE = 15; // odd, so there's a single centre pixel
  const int HALF = SAMPLE / 2;

  m_loupe_sample_w = SAMPLE;
  m_loupe_sample_h = SAMPLE;

  const int cx = (int)std::lround(m_mouse_x);
  const int cy = (int)std::lround(m_mouse_y);
  const int sx0 = cx - HALF;
  const int sy0 = cy - HALF;

  // (Re)allocate the scratch surface on first use / size change.
  if (!m_loupe_surface ||
      m_loupe_surface->get_width() != SAMPLE ||
      m_loupe_surface->get_height() != SAMPLE) {
    m_loupe_surface = Cairo::ImageSurface::create(
        Cairo::Surface::Format::ARGB32, SAMPLE, SAMPLE);
  }

  auto scr = Cairo::Context::create(m_loupe_surface);
  // Workspace background under everything — on_draw paints this first.
  scr->set_source_rgb(0.09, 0.09, 0.09);
  scr->paint();

  // Translate so the main-window screen region (sx0, sy0)..(sx0+SAMPLE,
  // sy0+SAMPLE) lands at the scratch surface's origin, then replay the
  // artboard origin + artboard fill + draw_objects pipeline. draw_objects
  // itself applies m_zoom per-layer, so we only need the translation.
  const double ox = doc_origin_x();
  const double oy = doc_origin_y();
  const double cw = m_doc->canvas_width() * m_zoom;
  const double ch = m_doc->canvas_height() * m_zoom;

  scr->save();
  scr->translate(ox - sx0, oy - sy0);
  // Artboard fill (matches on_draw's artboard rect).
  scr->set_source_rgb(0.157, 0.157, 0.157);
  scr->rectangle(0, 0, cw, ch);
  scr->fill();
  draw_grid(scr, (int)cw, (int)ch);
  draw_objects(scr);
  scr->restore();

  m_loupe_surface->flush();

  // Read back the centre pixel (ARGB32 little-endian: B G R A).
  unsigned char *data = m_loupe_surface->get_data();
  int stride = m_loupe_surface->get_stride();
  unsigned char *px = data + HALF * stride + HALF * 4;
  unsigned int b = px[0];
  unsigned int g = px[1];
  unsigned int r = px[2];
  unsigned int a = px[3];
  // ARGB32 stores premultiplied alpha. Un-premultiply for a FillStyle
  // value that will be applied to a fresh solid paint.
  if (a > 0 && a < 255) {
    r = std::min(255u, (r * 255u + a / 2) / a);
    g = std::min(255u, (g * 255u + a / 2) / a);
    b = std::min(255u, (b * 255u + a / 2) / a);
  }
  m_loupe_buffer_r = r / 255.0;
  m_loupe_buffer_g = g / 255.0;
  m_loupe_buffer_b = b / 255.0;
  m_loupe_buffer_a = a / 255.0;
  m_loupe_buffer_valid = true;
}

void Canvas::draw_eyedropper_loupe(const Cairo::RefPtr<Cairo::Context> &cr) {
  // S66 — Phase 3 always-zoom loupe.
  //
  // 120px circular magnifier that follows the cursor. Always shows the
  // rendered-buffer sample at 10×, with a light-grey pixel grid and a
  // crosshair (black + white halo) marking the centre pixel — the pixel
  // that clicking commits. Hex readout in a pill below the circle.
  //
  // Offset is applied edge-to-cursor (not bounding-box-corner-to-cursor),
  // so the near edge of the circle sits OFFSET pixels from the cursor.
  // Preferred direction is up-right; flips per-axis when near an edge of
  // the widget, with a final clamp so the loupe stays fully visible.
  if (!m_loupe_buffer_valid || !m_loupe_surface)
    return;

  const double cursor_x = m_mouse_x;
  const double cursor_y = m_mouse_y;
  const double RADIUS = 60.0;          // 120px circle
  const double DIAMETER = RADIUS * 2.0;
  const double EDGE_OFFSET = 8.0;      // cursor → nearest circle edge
  const double READOUT_H = 22.0;
  const double READOUT_PAD_Y = 6.0;
  const double TOTAL_H = DIAMETER + READOUT_PAD_Y + READOUT_H;
  const int W = get_allocated_width();
  const int H = get_allocated_height();

  // Bounding box top-left (box_l, box_t) for the loupe (circle + readout).
  // try_dir sets them so the near CIRCLE edge is EDGE_OFFSET from cursor.
  double box_l = 0.0, box_t = 0.0;
  auto try_dir = [&](int dx, int dy) {
    // dx: +1 = right of cursor, -1 = left. dy: -1 = above, +1 = below.
    box_l = (dx > 0) ? (cursor_x + EDGE_OFFSET)
                     : (cursor_x - EDGE_OFFSET - DIAMETER);
    box_t = (dy < 0) ? (cursor_y - EDGE_OFFSET - TOTAL_H)
                     : (cursor_y + EDGE_OFFSET);
  };
  // Preferred: up-right.
  try_dir(+1, -1);
  bool right_ok = box_l + DIAMETER <= W;
  bool up_ok = box_t >= 0;
  if (!(right_ok && up_ok))
    try_dir(right_ok ? +1 : -1, up_ok ? -1 : +1);
  // Final clamp into widget bounds.
  if (box_l < 4.0) box_l = 4.0;
  if (box_t < 4.0) box_t = 4.0;
  if (box_l + DIAMETER > W - 4.0) box_l = W - 4.0 - DIAMETER;
  if (box_t + TOTAL_H > H - 4.0) box_t = H - 4.0 - TOTAL_H;

  const double cx = box_l + RADIUS;
  const double cy = box_t + RADIUS;

  // ── Circle body ──────────────────────────────────────────────────────
  cr->save();

  // Drop shadow — soft, reads against any canvas content.
  cr->arc(cx + 1.5, cy + 2.0, RADIUS + 0.5, 0, 2 * M_PI);
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.35);
  cr->fill();

  // Clip to the circle for the magnified-pixel fill + overlays.
  cr->arc(cx, cy, RADIUS, 0, 2 * M_PI);
  cr->clip_preserve();

  // Magnified pixels: 10× NEAREST scale, centred over (cx, cy).
  const double MAG = 10.0;
  const double draw_w = m_loupe_sample_w * MAG;
  const double draw_h = m_loupe_sample_h * MAG;
  const double grid_left = cx - draw_w * 0.5;
  const double grid_top = cy - draw_h * 0.5;

  cr->save();
  cr->translate(grid_left, grid_top);
  cr->scale(MAG, MAG);
  auto pat = Cairo::SurfacePattern::create(m_loupe_surface);
  pat->set_filter(Cairo::SurfacePattern::Filter::NEAREST);
  cr->set_source(pat);
  cr->paint();
  cr->restore();

  // Pixel grid — light grey, always visible. Fainter than on a solid
  // workspace background but still readable over any sampled pixel.
  cr->save();
  cr->set_source_rgba(0.78, 0.78, 0.78, 0.55);
  cr->set_line_width(1.0);
  for (int i = 1; i < m_loupe_sample_w; ++i) {
    double x = grid_left + i * MAG;
    cr->move_to(x + 0.5, grid_top);
    cr->line_to(x + 0.5, grid_top + draw_h);
  }
  for (int j = 1; j < m_loupe_sample_h; ++j) {
    double y = grid_top + j * MAG;
    cr->move_to(grid_left, y + 0.5);
    cr->line_to(grid_left + draw_w, y + 0.5);
  }
  cr->stroke();
  cr->restore();

  // Centre-pixel crosshair — black + with a white halo so it reads on
  // any background colour. Drawn through the centre pixel, width = 1 pixel
  // cell so the halo completely surrounds the black line.
  const int HALF = m_loupe_sample_w / 2;
  const double centre_left = grid_left + HALF * MAG;
  const double centre_top = grid_top + HALF * MAG;
  const double centre_x = centre_left + MAG * 0.5;
  const double centre_y = centre_top + MAG * 0.5;
  const double ARM = MAG * 1.4; // crosshair extends slightly past the pixel
  cr->save();
  // White halo — drawn first, wider line.
  cr->set_source_rgba(1.0, 1.0, 1.0, 0.95);
  cr->set_line_width(3.0);
  cr->set_line_cap(Cairo::Context::LineCap::ROUND);
  cr->move_to(centre_x - ARM, centre_y);
  cr->line_to(centre_x + ARM, centre_y);
  cr->move_to(centre_x, centre_y - ARM);
  cr->line_to(centre_x, centre_y + ARM);
  cr->stroke();
  // Black crosshair — drawn on top, thinner.
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.95);
  cr->set_line_width(1.2);
  cr->move_to(centre_x - ARM, centre_y);
  cr->line_to(centre_x + ARM, centre_y);
  cr->move_to(centre_x, centre_y - ARM);
  cr->line_to(centre_x, centre_y + ARM);
  cr->stroke();
  cr->restore();

  cr->reset_clip();

  // Outer ring — dark for contrast.
  cr->arc(cx, cy, RADIUS, 0, 2 * M_PI);
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.85);
  cr->set_line_width(2.0);
  cr->stroke();

  // Inner highlight ring — thin white, so the body separates from the
  // dark outer ring.
  cr->arc(cx, cy, RADIUS - 1.5, 0, 2 * M_PI);
  cr->set_source_rgba(1.0, 1.0, 1.0, 0.35);
  cr->set_line_width(1.0);
  cr->stroke();

  // ── Readout strip ────────────────────────────────────────────────────
  // Hex + alpha% of the centre pixel.
  char readout[64] = {0};
  unsigned r = (unsigned)std::lround(m_loupe_buffer_r * 255.0);
  unsigned g = (unsigned)std::lround(m_loupe_buffer_g * 255.0);
  unsigned b = (unsigned)std::lround(m_loupe_buffer_b * 255.0);
  unsigned a = (unsigned)std::lround(m_loupe_buffer_a * 255.0);
  if (a < 255)
    std::snprintf(readout, sizeof(readout), "#%02X%02X%02X  %u%%", r, g, b,
                  (unsigned)std::lround(m_loupe_buffer_a * 100.0));
  else
    std::snprintf(readout, sizeof(readout), "#%02X%02X%02X", r, g, b);

  const double strip_y = box_t + DIAMETER + READOUT_PAD_Y;
  const double strip_x = box_l;

  // Rounded pill behind the text.
  cr->save();
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.75);
  double rr = READOUT_H * 0.5;
  cr->move_to(strip_x + rr, strip_y);
  cr->arc(strip_x + DIAMETER - rr, strip_y + rr, rr, -M_PI / 2, M_PI / 2);
  cr->line_to(strip_x + rr, strip_y + READOUT_H);
  cr->arc(strip_x + rr, strip_y + rr, rr, M_PI / 2, 3 * M_PI / 2);
  cr->close_path();
  cr->fill();
  cr->restore();

  // Text — monospace, centred in the strip.
  cr->save();
  cr->select_font_face("Monospace", Cairo::ToyFontFace::Slant::NORMAL,
                       Cairo::ToyFontFace::Weight::NORMAL);
  cr->set_font_size(12.0);
  Cairo::TextExtents ext;
  cr->get_text_extents(readout, ext);
  double tx = strip_x + (DIAMETER - ext.width) * 0.5 - ext.x_bearing;
  double ty = strip_y + (READOUT_H - ext.height) * 0.5 - ext.y_bearing;
  cr->set_source_rgba(1.0, 1.0, 1.0, 0.95);
  cr->move_to(tx, ty);
  cr->show_text(readout);
  cr->restore();

  cr->restore();
}

void Canvas::draw_ref_coord_overlay(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_doc)
    return;
  // Snap to whole units for display
  double doc_x = std::round(snap(m_cursor_doc_x));
  double doc_y = std::round(snap(m_cursor_doc_y));

  // Format in display/user-space coords (ruler origin convention)
  double ux = doc_x - m_doc->ruler_origin_x;
  double uy = (m_doc->canvas_height() - doc_y) - m_doc->ruler_origin_y;

  char buf[64];
  snprintf(buf, sizeof(buf), "X: %.0f  Y: %.0f", ux, uy);

  double sx, sy;
  doc_to_screen(doc_x, doc_y, sx, sy);
  double lx = sx + 14.0;
  double ly = sy - 6.0;

  cr->save();
  const double PAD_H = 6.0, PAD_V = 4.0;
  cr->select_font_face("monospace", Cairo::ToyFontFace::Slant::NORMAL,
                       Cairo::ToyFontFace::Weight::NORMAL);
  cr->set_font_size(11.0);
  Cairo::TextExtents te;
  cr->get_text_extents(buf, te);
  double bw = te.width + PAD_H * 2;
  double bh = te.height + PAD_V * 2;
  double bx = lx, by = ly - bh;
  cr->set_source_rgba(0.1, 0.1, 0.1, 0.82);
  cr->rectangle(bx, by, bw, bh);
  cr->fill();
  cr->set_source_rgba(1.0, 1.0, 1.0, 0.95);
  cr->move_to(bx + PAD_H, by + PAD_V + te.height);
  cr->show_text(buf);
  cr->restore();
}

void Canvas::draw_grid(const Cairo::RefPtr<Cairo::Context> &cr, int cw,
                       int ch) {
  // Grid is drawn in screen space (already translated to artboard origin,
  // but NOT yet scaled — cw/ch are screen pixels of the artboard).
  //
  // Show grid when individual document units are at least 8 screen pixels
  // apart — below that the lines are too dense to be useful.
  // This replaces the old `m_zoom < 4.0` pixel check which was meaningless
  // for ratio-based documents (a 1000-unit doc at fit-zoom has m_zoom~0.5).
  if (m_zoom < 8.0)
    return;

  cr->save();
  cr->set_source_rgba(0.7, 0.8, 1.0, 0.18);
  cr->set_line_width(0.5);
  // step = m_zoom means one grid line per document unit (1:1 with doc grid).
  // This is correct: at m_zoom==8 each unit is 8px wide — fine grain visible.
  const double step = m_zoom;
  for (double x = step; x < cw; x += step) {
    cr->move_to(x, 0);
    cr->line_to(x, ch);
  }
  for (double y = step; y < ch; y += step) {
    cr->move_to(0, y);
    cr->line_to(cw, y);
  }
  cr->stroke();
  cr->restore();
}

void Canvas::apply_fill(const Cairo::RefPtr<Cairo::Context> &cr,
                        const FillStyle &fill) {
  // Bbox-less form. For solid/None/CurrentColor this is fine; for gradients
  // we don't know the shape's bbox so we degrade to the first stop's flat
  // colour (still produces *something* visible rather than crashing). The
  // bbox-aware overload is what real renders go through.
  if (fill.is_gradient()) {
    if (!fill.stops.empty()) {
      const auto &s = fill.stops.front();
      cr->set_source_rgba(s.r, s.g, s.b, s.a);
    } else {
      cr->set_source_rgba(0, 0, 0, 0);
    }
    return;
  }
  switch (fill.type) {
  case FillStyle::Type::None:
    break;
  case FillStyle::Type::CurrentColor:
    cr->set_source_rgb(0.88, 0.88,
                       0.88); // preview as near-white on dark artboard
    break;
  case FillStyle::Type::Solid:
    cr->set_source_rgba(fill.r, fill.g, fill.b, fill.a);
    break;
  case FillStyle::Type::LinearGradient:
  case FillStyle::Type::RadialGradient:
    // unreachable — handled above by is_gradient() branch.
    break;
  }
}

// Bbox-aware overload — required for gradient fills. Stops are stored in
// objectBoundingBox-fraction space (0..1) and lerped into doc-pixel space
// using the shape's bbox at render time. Solid/None/CurrentColor fall back
// to the bbox-less path; bbox is ignored in those cases.
void Canvas::apply_fill(const Cairo::RefPtr<Cairo::Context> &cr,
                        const FillStyle &fill,
                        const BBox &bbox) {
  if (!fill.is_gradient()) {
    apply_fill(cr, fill);
    return;
  }
  if (fill.stops.empty()) {
    // No stops — nothing to render. Source set to fully transparent so a
    // subsequent fill() draws nothing rather than whatever was last set.
    cr->set_source_rgba(0, 0, 0, 0);
    return;
  }

  // Lerp endpoints from 0..1 fractions into doc coordinates.
  // (g_x1, g_y1) → (bbox.x + g_x1 * bbox.w, bbox.y + g_y1 * bbox.h).
  const double x1 = bbox.x + fill.g_x1 * bbox.w;
  const double y1 = bbox.y + fill.g_y1 * bbox.h;
  const double x2 = bbox.x + fill.g_x2 * bbox.w;
  const double y2 = bbox.y + fill.g_y2 * bbox.h;

  Cairo::RefPtr<Cairo::Gradient> pat;
  if (fill.type == FillStyle::Type::LinearGradient) {
    pat = Cairo::LinearGradient::create(x1, y1, x2, y2);
  } else {
    // Radial: focal at (g_x1,g_y1), centre at (g_x2,g_y2), radius is a
    // fraction of the bbox's larger dimension (objectBoundingBox semantics
    // for radial gradients in SVG use sqrt((w²+h²)/2) but the larger-dim
    // approximation is close enough for Stage 1 and matches user intuition).
    const double R = fill.g_r * std::max(bbox.w, bbox.h);
    pat = Cairo::RadialGradient::create(x1, y1, 0.0, x2, y2, R);
  }

  // Stops. Sort defensively in case the data wasn't pre-sorted.
  std::vector<GradientStop> sorted = fill.stops;
  std::sort(sorted.begin(), sorted.end(),
            [](const GradientStop &a, const GradientStop &b) {
              return a.offset < b.offset;
            });
  for (const auto &s : sorted) {
    pat->add_color_stop_rgba(s.offset, s.r, s.g, s.b, s.a);
  }

  cr->set_source(pat);
}

// ── Cache-aware apply_fill (S106 m1) ────────────────────────────────────────
//
// Routes gradient fills on a SceneNode through obj.gradient_cache. The
// cache is rasterised at doc resolution (cache surface dimensions =
// ceil(bbox.w) × ceil(bbox.h) pixels), filled with the gradient pattern,
// and reused on subsequent frames. Cairo bilinear-upscales at higher
// zooms.
//
// Coherence checks (any "yes" → rebuild):
//   1. gradient_cache_dirty — fill content changed (stops, type, colour).
//   2. gradient_cache surface is null — first paint of this node.
//   3. cached bbox doesn't match current bbox — geometry changed (path
//      mutation, scale). Move alone preserves the bbox shape; only the
//      caller's set_source x,y offset moves.
//
// Why doc-resolution caching: gradients are smooth fields. A gradient
// rasterised at doc-unit (zoom=1) resolution and bilinear-upscaled at
// zoom 4× looks essentially identical to one rasterised at zoom 4×.
// Per-zoom-level caching would invalidate on every zoom click and pay
// the rasterisation cost again — exactly the s105 failure mode. Doc-
// resolution caching survives every zoom change without rebuild.
//
// Surface size cap: extreme-aspect or large-bbox shapes could in
// principle demand huge surfaces. SCENE_NODE_GRADIENT_CACHE_MAX_PX caps
// the long axis at 4096 px (16 MB ARGB32 — 2× the budget we ever spend
// per node in practice). On overflow we bypass the cache and rasterise
// directly. Rare in icon-design workloads.
void Canvas::apply_fill(const Cairo::RefPtr<Cairo::Context> &cr,
                        const SceneNode &obj,
                        const BBox &bbox) {
  // Non-gradient fills don't benefit from caching — fall through.
  if (!obj.fill.is_gradient()) {
    apply_fill(cr, obj.fill, bbox);
    return;
  }
  if (obj.fill.stops.empty()) {
    cr->set_source_rgba(0, 0, 0, 0);
    return;
  }

  // Coherence check.
  //
  // Only the bbox SHAPE (w, h) matters for cache validity. The bbox
  // position (x, y) becomes the blit offset in the cr->set_source call
  // below — translating an object preserves the cached pixels and only
  // changes where they land. Comparing position would invalidate on
  // every drag frame (the s106 m1 fix1 bug, caught via instrumentation).
  constexpr double EPS = 1e-6;
  const bool bbox_match =
      obj.gradient_cache_bbox_w > 0.0 &&
      std::abs(obj.gradient_cache_bbox_w - bbox.w) < EPS &&
      std::abs(obj.gradient_cache_bbox_h - bbox.h) < EPS;

  const bool needs_rebuild =
      obj.gradient_cache_dirty || !obj.gradient_cache || !bbox_match;

  if (needs_rebuild) {
    // Allocate / reallocate the offscreen surface at doc resolution.
    constexpr int CACHE_MAX_PX = 4096;
    const int surf_w = std::max(1, (int)std::ceil(bbox.w));
    const int surf_h = std::max(1, (int)std::ceil(bbox.h));
    if (surf_w > CACHE_MAX_PX || surf_h > CACHE_MAX_PX) {
      // Surface too large for the cache budget. Bypass to direct render
      // — slower per frame for this object, but bounded memory.
      LOG_INFO("Gradient cache bypass: surface {}x{} px exceeds cap {} "
               "(direct-render fallback)",
               surf_w, surf_h, CACHE_MAX_PX);
      apply_fill(cr, obj.fill, bbox);
      return;
    }

    auto surf = Cairo::ImageSurface::create(
        Cairo::Surface::Format::ARGB32, surf_w, surf_h);
    // Cairo returns an "error surface" rather than throwing on alloc
    // failure — read back width to verify.
    if (!surf || surf->get_width() != surf_w) {
      LOG_WARN("Gradient cache surface allocation failed for {}x{} px "
               "(direct-render fallback)",
               surf_w, surf_h);
      apply_fill(cr, obj.fill, bbox);
      return;
    }

    // Build the gradient pattern in the surface's coordinate frame.
    // The surface is sized to the bbox in doc units; the pattern's
    // endpoints lerp from objectBoundingBox-fractions into surface-
    // pixel coordinates.
    const double x1 = obj.fill.g_x1 * bbox.w;
    const double y1 = obj.fill.g_y1 * bbox.h;
    const double x2 = obj.fill.g_x2 * bbox.w;
    const double y2 = obj.fill.g_y2 * bbox.h;

    Cairo::RefPtr<Cairo::Gradient> pat;
    if (obj.fill.type == FillStyle::Type::LinearGradient) {
      pat = Cairo::LinearGradient::create(x1, y1, x2, y2);
    } else {
      const double R = obj.fill.g_r * std::max(bbox.w, bbox.h);
      pat = Cairo::RadialGradient::create(x1, y1, 0.0, x2, y2, R);
    }
    std::vector<GradientStop> sorted = obj.fill.stops;
    std::sort(sorted.begin(), sorted.end(),
              [](const GradientStop &a, const GradientStop &b) {
                return a.offset < b.offset;
              });
    for (const auto &s : sorted) {
      pat->add_color_stop_rgba(s.offset, s.r, s.g, s.b, s.a);
    }

    // Paint the gradient onto the entire surface.
    auto sc = Cairo::Context::create(surf);
    sc->set_source(pat);
    sc->paint();

    // Commit to the node.
    obj.gradient_cache = surf;
    obj.gradient_cache_dirty = false;
    obj.gradient_cache_zoom = 1.0;  // we cache at doc resolution
    obj.gradient_cache_bbox_x = bbox.x;
    obj.gradient_cache_bbox_y = bbox.y;
    obj.gradient_cache_bbox_w = bbox.w;
    obj.gradient_cache_bbox_h = bbox.h;
  }

  // Set the cached surface as the fill source. The surface is in doc
  // units; placing it at (bbox.x, bbox.y) in the active doc-space
  // transform makes a subsequent cr->fill() against the path sample
  // the right pixels. Cairo applies the active transform's scale to
  // the source automatically (i.e. at zoom 4× the surface is sampled
  // 4× larger via bilinear upsampling).
  cr->set_source(obj.gradient_cache, bbox.x, bbox.y);
}

void Canvas::apply_stroke_style(const Cairo::RefPtr<Cairo::Context> &cr,
                                const StrokeStyle &stroke) {
  apply_fill(cr, stroke.paint);
  // draw_objects scales by m_zoom before calling us, so stroke.width is already
  // in doc units
  cr->set_line_width(stroke.width);
  switch (stroke.cap) {
  case LineCap::Butt:
    cr->set_line_cap(Cairo::Context::LineCap::BUTT);
    break;
  case LineCap::Round:
    cr->set_line_cap(Cairo::Context::LineCap::ROUND);
    break;
  case LineCap::Square:
    cr->set_line_cap(Cairo::Context::LineCap::SQUARE);
    break;
  }
  switch (stroke.join) {
  case LineJoin::Miter:
    cr->set_line_join(Cairo::Context::LineJoin::MITER);
    break;
  case LineJoin::Round:
    cr->set_line_join(Cairo::Context::LineJoin::ROUND);
    break;
  case LineJoin::Bevel:
    cr->set_line_join(Cairo::Context::LineJoin::BEVEL);
    break;
  }
}

// ── render_shadow_under (S97 m2) ─────────────────────────────────────────
// Paints a tinted, blurred, offset shadow of `host_pat` onto `cr`.
// Called from draw_object's end-of-wrap when obj.shadow_enabled is true.
//
// Pipeline (mirrors the SVG <filter> chain emitted by SvgWriter):
//   1. Compute host doc-space bbox; pad it by max(blur, |dx|, |dy|) so the
//      shadow has room to land without clipping at the offscreen surface
//      edges. Convert to device pixels via the cr's current CTM (which
//      includes draw_objects' scale(m_zoom)).
//   2. Allocate an ImageSurface of those pixel dimensions, transformed so
//      the host's doc-space coordinates land at the correct pixel inside.
//   3. Paint host_pat into the ImageSurface — gets us the host's silhouette
//      and colour at the right pixel positions.
//   4. Three-pass box blur (curvz::utils::box_blur_argb32) approximating
//      Gaussian. Radius in pixels = round(blur_doc * m_zoom).
//   5. On the canvas cr: translate by (dx, dy) in doc-space, set source to
//      the shadow colour, mask through the blurred surface's alpha. This
//      stamps the shadow tint wherever the blurred host had alpha — the
//      blurred surface's RGB is discarded, only its alpha contributes.
//
// Caller paints the unblurred host on top after this returns.
//
// No-op when bbox is unavailable or surface dimensions degenerate. The
// host still renders correctly (caller's set_source(host_pat); paint());
// the shadow just goes missing rather than crashing.
void Canvas::render_shadow_under(const Cairo::RefPtr<Cairo::Context> &cr,
                                 const SceneNode &obj,
                                 const Cairo::RefPtr<Cairo::Pattern> &host_pat) {
  if (!host_pat) return;

  // ── 1. Doc-space bbox + padding ────────────────────────────────────
  // include_stroke=true: shadow follows the stroked silhouette, not just
  // the fill geometry. Bail if bbox is unavailable (degenerate node, no
  // path geometry, etc.). Without a bbox we'd have nowhere to allocate
  // the offscreen surface.
  auto bb = object_bbox(obj, /*include_stroke=*/true);
  if (!bb) return;

  // Pad in doc units. Three sources of padding combine:
  //   * blur radius — the kernel reach of the box-blur passes
  //   * |shadow_dx|, |shadow_dy| — the offset can push shadow off-bbox
  //   * a small safety constant to absorb minor object_bbox under-estimates
  //     and to give the blur smooth ramps at the edges
  const double blur_doc = std::max(0.0, obj.shadow_blur);
  const double pad_doc =
      std::ceil(blur_doc * 2.0)            // 2σ for visual completeness
      + std::abs(obj.shadow_dx)
      + std::abs(obj.shadow_dy)
      + 4.0;                               // safety constant

  double doc_x0 = bb->x       - pad_doc;
  double doc_y0 = bb->y       - pad_doc;
  double doc_x1 = bb->x + bb->w + pad_doc;
  double doc_y1 = bb->y + bb->h + pad_doc;

  // ── 2. Doc rect → device pixel rect ────────────────────────────────
  // cr's CTM is doc → device; user_to_device walks both corners.
  // After this conversion, dev_x0/y0 and dev_x1/y1 are pixel coordinates
  // on the canvas's backing surface.
  double dx0 = doc_x0, dy0 = doc_y0;
  double dx1 = doc_x1, dy1 = doc_y1;
  cr->user_to_device(dx0, dy0);
  cr->user_to_device(dx1, dy1);
  // CTM scale should be positive (we never flip), but defend against
  // arbitrary CTM by min/max-ing.
  if (dx1 < dx0) std::swap(dx0, dx1);
  if (dy1 < dy0) std::swap(dy0, dy1);

  // Snap to pixel grid (outward). Sub-pixel surface offsets cause sample
  // shimmer when the canvas zooms or pans across the host.
  int pix_x = (int)std::floor(dx0);
  int pix_y = (int)std::floor(dy0);
  int pix_w = (int)std::ceil(dx1) - pix_x;
  int pix_h = (int)std::ceil(dy1) - pix_y;
  if (pix_w <= 0 || pix_h <= 0) return;

  // ── Viewport intersect (S107 m1) ────────────────────────────────────
  //
  // Without this clip the offscreen surface scales as host_bbox × zoom².
  // A canvas-sized shadowed object at zoom 12 demands a ~16000² ARGB32
  // buffer (~1 GB) plus host-pattern paint into every pixel plus a
  // box-blur over the whole thing. That was the 12s-frame freeze
  // observed during S107 diagnosis (`us_render_shadow` will confirm).
  //
  // The shadow is only visually meaningful where its blur footprint
  // intersects the visible widget. Off-viewport host bbox plus blur
  // ramp can still bleed into the visible region, so we expand the
  // viewport rect by the same padding the bbox already used (blur
  // reach + shadow offset + safety) before intersecting. A shadowed
  // object whose entire (bbox + blur) sits off-screen produces no
  // visible pixels and we early-out.
  //
  // The intersection rect's origin (pix_x, pix_y) still describes
  // where the offscreen surface sits on the destination device — the
  // matrix-correct paint code below (`m.x0 -= pix_x`) and the final
  // `cr->mask(surf, pix_x + off_dx, pix_y + off_dy)` both consume the
  // post-intersect values correctly because both interpret pix_x/y as
  // "device pixel of surface(0,0)", which is what the intersection
  // rect's origin is.
  {
    const int radius_px_pre = (int)std::round(blur_doc * m_zoom);
    const int pad_px = radius_px_pre * 2
                     + (int)std::ceil(std::abs(obj.shadow_dx) * m_zoom)
                     + (int)std::ceil(std::abs(obj.shadow_dy) * m_zoom)
                     + 4;
    const int vp_x0 = -pad_px;
    const int vp_y0 = -pad_px;
    const int vp_x1 = get_width()  + pad_px;
    const int vp_y1 = get_height() + pad_px;

    const int host_x0 = pix_x;
    const int host_y0 = pix_y;
    const int host_x1 = pix_x + pix_w;
    const int host_y1 = pix_y + pix_h;

    const int new_x0 = std::max(host_x0, vp_x0);
    const int new_y0 = std::max(host_y0, vp_y0);
    const int new_x1 = std::min(host_x1, vp_x1);
    const int new_y1 = std::min(host_y1, vp_y1);

    if (new_x1 <= new_x0 || new_y1 <= new_y0) return;  // off-viewport

    pix_x = new_x0;
    pix_y = new_y0;
    pix_w = new_x1 - new_x0;
    pix_h = new_y1 - new_y0;
  }

  // ── 3. Offscreen ImageSurface, paint host into it ──────────────────
  // ARGB32 = Cairo's premultiplied 32-bit format, the only format on
  // which our box_blur_argb32 operates.
  auto surf = Cairo::ImageSurface::create(
      Cairo::Surface::Format::ARGB32, pix_w, pix_h);
  if (!surf) return;
  auto sc = Cairo::Context::create(surf);
  // ── Critical: matrix-correct paint of host_pat into the offscreen surf.
  //
  // host_pat is a Cairo::SurfacePattern returned by pop_group_to_source.
  // Its internal pattern_matrix was set by Cairo so that, used as a source
  // on the ORIGINAL cr (CTM unchanged from push time), painting reproduces
  // the captured image at its original device-pixel location.
  //
  // To paint host_pat correctly into `sc`, sc's CTM must reproduce the
  // CTM the original cr had at push_group time, with one extra step: a
  // device-space translation by (-pix_x, -pix_y) so that the offscreen
  // surface's pixel (0,0) corresponds to canvas device pixel (pix_x, pix_y).
  //
  // The original cr's CTM hasn't been touched between push_group (in
  // begin_alpha) and now (inside end_alpha → render_shadow_under), so
  // cr->get_matrix() returns exactly that CTM. We grab it, prepend a
  // device-space translation by subtracting from x0/y0 (the device-space
  // origin column of the affine), and apply to sc. From sc's perspective,
  // user-space coordinates now map to its surface's pixel grid in lockstep
  // with the original canvas-pixel grid for that doc-space region.
  //
  // Wrong attempt (initial m2 ship): sc->translate(-pix_x, -pix_y) on the
  // default identity matrix. That only worked if host_pat's source pixels
  // were addressed in canvas device coords — but the pattern matrix maps
  // destination user-space → source pixels, and sc's user-space was
  // identity-doc-pixels, not canvas-device-pixels. Result: paint produced
  // either empty pixels or a tiny smear at the wrong spot.
  Cairo::Matrix m = cr->get_matrix();
  m.x0 -= pix_x;
  m.y0 -= pix_y;
  sc->set_matrix(m);
  sc->set_source(host_pat);
  sc->paint();
  // Drop the cairomm Context before reading bytes (it may have pending
  // ops in its private state). flush() forces them out.
  sc.reset();
  surf->flush();

  // ── 4. Blur in place ───────────────────────────────────────────────
  // Convert blur radius doc → pixels via current zoom. radius=0 skips
  // the blur entirely (curvz::utils handles that as a no-op). For very
  // small radii (<1 px) the result is visually unchanged from the
  // unblurred copy; still cheap, no special-case needed.
  const int radius_px = (int)std::round(blur_doc * m_zoom);
  if (radius_px > 0) {
    curvz::utils::box_blur_argb32(
        surf->get_data(), surf->get_stride(), pix_w, pix_h, radius_px);
    surf->mark_dirty();
  }

  // ── 5. Composite onto cr: shadow colour masked by blurred alpha ────
  // The mask call wants its (x,y) interpreted in CURRENT user space, then
  // multiplied through the CTM to landing pixels. Simplest: switch to a
  // device-space identity CTM, compute the shadow-offset delta in device
  // pixels, do the mask there, then restore. This keeps the math local
  // and avoids juggling user vs device for each component.
  //
  // Shadow offset in device pixels: take (0,0) and (dx,dy) doc-space and
  // run them both through user_to_device; the difference is the shadow
  // delta in pixels. Same for the surface origin (pix_x, pix_y) — those
  // are already device pixels by construction.
  double off_a_x = 0.0, off_a_y = 0.0;
  double off_b_x = obj.shadow_dx, off_b_y = obj.shadow_dy;
  cr->user_to_device(off_a_x, off_a_y);
  cr->user_to_device(off_b_x, off_b_y);
  const double off_dx = off_b_x - off_a_x;
  const double off_dy = off_b_y - off_a_y;

  // Final shadow alpha = colour.a × shadow_opacity (matches SvgWriter's
  // pre-multiplication into flood-opacity). This dials the whole shadow
  // up/down independent of its hue.
  const double final_a =
      std::max(0.0, std::min(1.0, obj.shadow_color_a * obj.shadow_opacity));

  cr->save();
  // Identity CTM — units are now device pixels.
  cr->set_identity_matrix();
  cr->set_source_rgba(obj.shadow_color_r,
                      obj.shadow_color_g,
                      obj.shadow_color_b,
                      final_a);
  // mask(surface, x, y): paints the current source through the surface's
  // alpha mask, with the surface positioned at (x, y) in current user
  // space. We're at identity now, so x, y are device pixels — exactly the
  // shadow's placement on the canvas backing surface.
  cr->mask(surf, pix_x + off_dx, pix_y + off_dy);
  cr->restore();
}

void Canvas::draw_objects(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_doc)
    return;

  // Layer z-order matches the LayersPanel row order:
  //   - LayersPanel iterates rows from `layers[n-1]` down to `layers[0]`
  //     and appends top-to-bottom, so `layers[n-1]` appears at the TOP
  //     of the panel and `layers[0]` at the BOTTOM.
  //   - Cairo paints in call order (last-painted wins), so to match the
  //     panel we must paint `layers[0]` FIRST (bottom of z-order) and
  //     `layers[n-1]` LAST (top of z-order).
  // This means dragging a guide / margin / grid layer to the bottom of
  // the panel actually puts it behind art.
  for (int li = 0; li < (int)m_doc->layers.size(); ++li) {
    const auto &layer = m_doc->layers[li];
    if (!layer->visible)
      continue;

    if (layer->is_guide_layer()) {
      draw_guides_doc_space(cr, layer.get());
      continue;
    }

    if (layer->is_grid_layer()) {
      draw_grid_doc_space(cr, layer.get());
      continue;
    }

    if (layer->is_margin_layer()) {
      draw_margin_doc_space(cr, layer.get());
      continue;
    }

    double rr, rg, rb;
    if (layer->is_ref_layer()) {
      rr = 0.85;
      rg = 0.1;
      rb = 0.75;
      parse_layer_color(layer->color, rr, rg, rb);
    } else {
      // S99: pick a default layer stroke V (HSV value) that contrasts
      // with the artboard background. Pre-S99 was hardcoded 0.88 grey
      // on a dark default artboard — vanishes when the user picks a
      // light artboard via the S98 m3 picker. Especially noticeable in
      // outline / node mode where the only thing on screen IS the stroke.
      //
      // Layers carry an auto-assigned palette colour (LayersPanel mints
      // one on creation) so multiple visible layers stay distinguishable
      // by hue. We preserve hue + saturation and only swap the V axis
      // — so "Layer A is blue, Layer B is green" still reads at a glance,
      // but both stay readable against any artboard.
      //
      // HSV (not HSL) was chosen so bright strokes stay vivid: HSL L→1
      // washes any colour toward white, HSV V→1 keeps the hue at full
      // brightness. The "bright line" cases really are intended to be
      // bright-blue / bright-green, not pale.
      //
      // V curve — single direction flip at bg V 0.60.
      //
      //   bg V 0.0..0.60   → bright line, gentle endpoints.
      //                          bg 0.0  → V 0.40 (gentle bright on black)
      //                          bg 0.60 → V 0.85 (max bright, just before flip)
      //   bg V 0.60..1.0   → dark line, gentle endpoints.
      //                          bg 0.60 → V 0.20 (max dark, just after flip)
      //                          bg 1.0  → V 0.60 (gentle dark on white)
      //
      // Discontinuity at 0.60 is intentional — that's the direction
      // flip. A continuous curve through it would always pass through
      // bg's own V somewhere and vanish there. Bg threshold uses HSV V
      // (max channel) to match the user's intuition about brightness,
      // not perceptual luminance.
      // Read artboard bg via the project accessor — picks dark or
      // light pair based on current motif. Falls back to doc-level
      // legacy field if the project pointer hasn't been wired (early
      // boot, before set_project is called).
      double ab_r, ab_g, ab_b;
      if (m_project) {
          ab_r = m_project->artboard_r();
          ab_g = m_project->artboard_g();
          ab_b = m_project->artboard_b();
      } else {
          ab_r = m_doc->artboard_bg_r;
          ab_g = m_doc->artboard_bg_g;
          ab_b = m_doc->artboard_bg_b;
      }
      double bg_v = std::max({ab_r, ab_g, ab_b});
      double target_v;
      if (bg_v < 0.60) {
        target_v = 0.40 + (bg_v / 0.60) * 0.45;
      } else {
        target_v = 0.20 + ((bg_v - 0.60) / 0.40) * 0.40;
      }

      // Source: layer's palette colour (or 0.88 grey if missing/malformed).
      double src_r = 0.88, src_g = 0.88, src_b = 0.88;
      parse_layer_color(layer->color, src_r, src_g, src_b);

      // RGB → HSV. Standard formulas; we extract H + S, then substitute
      // target_v and convert back. Achromatic source (s=0) just paints
      // a grey of the target V — desaturated layer colours stay grey.
      double mx = std::max({src_r, src_g, src_b});
      double mn = std::min({src_r, src_g, src_b});
      double d  = mx - mn;
      double h  = 0.0;
      double s  = (mx == 0.0) ? 0.0 : d / mx;
      if (d != 0.0) {
        if      (mx == src_r) h = (src_g - src_b) / d + (src_g < src_b ? 6 : 0);
        else if (mx == src_g) h = (src_b - src_r) / d + 2;
        else                  h = (src_r - src_g) / d + 4;
        h /= 6.0;
      }

      // HSV → RGB at (h, s, target_v).
      if (s == 0.0) {
        rr = rg = rb = target_v;  // achromatic
      } else {
        double hh = h * 6.0;
        int    i  = (int)std::floor(hh) % 6;
        if (i < 0) i += 6;
        double f  = hh - std::floor(hh);
        double p  = target_v * (1.0 - s);
        double q  = target_v * (1.0 - s * f);
        double t  = target_v * (1.0 - s * (1.0 - f));
        switch (i) {
          case 0: rr = target_v; rg = t;        rb = p;        break;
          case 1: rr = q;        rg = target_v; rb = p;        break;
          case 2: rr = p;        rg = target_v; rb = t;        break;
          case 3: rr = p;        rg = q;        rb = target_v; break;
          case 4: rr = t;        rg = p;        rb = target_v; break;
          default:rr = target_v; rg = p;        rb = q;        break;
        }
      }

      LOG_INFO("outline_contrast: layer='{}' bg_v={:.3f} target_v={:.3f} "
               "src_rgb=({:.2f},{:.2f},{:.2f}) hsv=(h={:.2f} s={:.2f}) "
               "out_rgb=({:.2f},{:.2f},{:.2f})",
               layer->name, bg_v, target_v, src_r, src_g, src_b,
               h, s, rr, rg, rb);
    }
    cr->save();
    cr->scale(m_zoom, m_zoom);
    for (int oi = (int)layer->children.size() - 1; oi >= 0; --oi)
      draw_object(cr, *layer->children[oi], rr, rg, rb);
    cr->restore();
  }
}

void Canvas::draw_object(const Cairo::RefPtr<Cairo::Context> &cr,
                         const SceneNode &obj, double layer_r, double layer_g,
                         double layer_b) {
  cr->save();

  // ── Per-object opacity + drop shadow (S97 m2) ─────────────────────────
  // When opacity < 1.0, push a cairo group so the object's fill+stroke
  // (and any internal compositing) render as a single isolated layer,
  // then pop and paint the whole thing with alpha. This matches SVG's
  // object-level `opacity` semantics (as opposed to fill-opacity /
  // stroke-opacity which are per-paint). Groups don't paint pixels of
  // their own; their children are each recursed into draw_object and
  // handle their own opacity — which is correct: parent * child opacity
  // compounds multiplicatively, matching SVG.
  //
  // Drop shadow piggy-backs on the same wrap pair (S97 m2). When the host
  // has shadow_enabled, end_alpha runs the offscreen-blur composite under
  // the host BEFORE painting the host on top — see render_shadow_under
  // for the full pipeline. Both effects can apply: shadow first, then
  // opacity wraps the combined shadow+host as a single alpha pass,
  // matching SVG (filter is applied before opacity in the rendering
  // pipeline).
  //
  // Ref points and degenerate (non-path) objects skip this.
  const bool wants_alpha  = (obj.opacity < 0.999);
  // Outline mode renders fills as 1-px strokes for editing — a shadow of
  // a hollow outline is visually surprising and not what Illustrator/
  // Affinity show in their equivalent modes. Skip the shadow pass when
  // outline mode is on; the host still renders normally.
  const bool wants_shadow = obj.shadow_enabled && !m_outline_mode
                            && (obj.shadow_color_a > 0.0)
                            && (obj.shadow_opacity > 0.0);
  bool alpha_pushed = false;
  auto begin_alpha = [&]() {
    if (wants_alpha || wants_shadow) {
      cr->push_group();
      alpha_pushed = true;
    }
  };
  auto end_alpha = [&]() {
    if (!alpha_pushed) return;
    // Pop the host's render off the group stack. From here we hold a
    // pattern (Cairo::SurfacePattern wrapping a recording/backed surface)
    // until we paint it back to cr.
    cr->pop_group_to_source();
    Cairo::RefPtr<Cairo::Pattern> host_pat = cr->get_source();

    if (wants_shadow) {
      render_shadow_under(cr, obj, host_pat);
    }

    // Paint the original host on top, with opacity if requested.
    cr->set_source(host_pat);
    if (wants_alpha) cr->paint_with_alpha(obj.opacity);
    else             cr->paint();

    alpha_pushed = false;
  };

  // ── Group: recurse into children independently ────────────────────────
  if (obj.type == SceneNode::Type::Group) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    // Wrap the recursion so group opacity composites the group as a unit
    // (overlapping children alpha'd once, not additively). Each child's
    // own opacity still applies inside the group — SVG semantics.
    begin_alpha();
    for (int i = (int)obj.children.size() - 1; i >= 0; --i)
      draw_object(cr, *obj.children[i], layer_r, layer_g, layer_b);
    end_alpha();
    cr->restore();
    return;
  }

  // ── ClipGroup: children drawn inside a Cairo clip region ──────────────
  // Normal mode:
  //   - Build a Cairo path from clip_shape's geometry (Path or Compound).
  //   - cr->clip() intersects the current clip region with that path.
  //     We're already inside a cr->save() so cr->restore() at end of the
  //     function undoes the clip. No fill/stroke is drawn for the shape.
  //   - Recurse children top-down (size-1 → 0) same as Group.
  //
  // Outline mode:
  //   - Skip the clip entirely so the user can see children outside the
  //     clipped region for editing. Stroke the clip_shape in layer colour
  //     so it's visible/editable on canvas.
  //
  // Compound clip shape: emit all subpaths before calling clip().
  // Cairo's default fill rule (WINDING) vs SVG clip-path's default
  // (NONZERO, same thing) — match by leaving the rule alone. EVEN_ODD
  // would be needed only if a Compound was authored with even/odd intent;
  // a future refinement.
  if (obj.type == SceneNode::Type::ClipGroup) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    begin_alpha();

    if (m_outline_mode) {
      // Children first (in outline) — no clip applied.
      for (int i = (int)obj.children.size() - 1; i >= 0; --i)
        draw_object(cr, *obj.children[i], layer_r, layer_g, layer_b);

      // Then the clip shape on top as a thin stroke.
      if (obj.clip_shape) {
        const SceneNode &cs = *obj.clip_shape;
        if (cs.type == SceneNode::Type::Path && cs.path) {
          BezierPath bp = BezierPath::from_path_data(*cs.path);
          bp.apply_to_cairo(cr);
        } else if (cs.type == SceneNode::Type::Compound) {
          for (int i = (int)cs.children.size() - 1; i >= 0; --i) {
            const SceneNode &sub = *cs.children[i];
            if (sub.type != SceneNode::Type::Path || !sub.path)
              continue;
            BezierPath bp = BezierPath::from_path_data(*sub.path);
            bp.apply_to_cairo(cr);
          }
        }
        cr->set_source_rgb(layer_r, layer_g, layer_b);
        cr->set_line_width(1.0 / m_zoom);
        cr->stroke();
      }
    } else {
      // Normal mode — apply the clip, then recurse.
      //
      // Fill-rule note: Cairo defaults to WINDING. A Compound clip shape
      // with nested subpaths (e.g. donut = outer ring + inner ring) uses
      // EVEN_ODD to produce the hole — matches how the Compound itself
      // is filled in draw_object. Without this, overlapping subpaths
      // union (no hole), so the clip honors only the outermost ring.
      // cr->save() at the top of this function bounds the fill_rule
      // change to this object's recursion — cr->restore() resets it.
      if (obj.clip_shape) {
        const SceneNode &cs = *obj.clip_shape;
        if (cs.type == SceneNode::Type::Path && cs.path) {
          BezierPath bp = BezierPath::from_path_data(*cs.path);
          bp.apply_to_cairo(cr);
          cr->clip();
        } else if (cs.type == SceneNode::Type::Compound) {
          for (int i = (int)cs.children.size() - 1; i >= 0; --i) {
            const SceneNode &sub = *cs.children[i];
            if (sub.type != SceneNode::Type::Path || !sub.path)
              continue;
            BezierPath bp = BezierPath::from_path_data(*sub.path);
            bp.apply_to_cairo(cr);
          }
          cr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);
          cr->clip();
          cr->set_fill_rule(Cairo::Context::FillRule::WINDING);
        }
      }
      // If clip_shape is missing/degenerate, nothing is clipped —
      // children render normally. Matches "no clip defined yet" state.
      for (int i = (int)obj.children.size() - 1; i >= 0; --i)
        draw_object(cr, *obj.children[i], layer_r, layer_g, layer_b);
    }

    end_alpha();
    cr->restore();
    return;
  }

  // ── Blend: render A, generated intermediates, B ───────────────────────
  // Blend is a container, not a drawable. We recurse into its three
  // logical children in painter order:
  //   1. blend_source_a  (bottom)
  //   2. blend_cache[0..N-1]  (middle — regenerated on dirty)
  //   3. blend_source_b  (top)
  // Each step / source is itself a Path SceneNode, so we re-enter
  // draw_object for each one and inherit the normal Path rendering
  // path (fill, stroke, opacity, transform). The Blend's own opacity
  // composites the whole stack as a unit via begin_alpha/end_alpha —
  // same semantics as Group.
  // ── Blend: render A, generated intermediates, B ───────────────────────
  // Blend is a container, not a drawable. We recurse into its three
  // logical children in painter order:
  //   1. blend_source_a  (bottom)
  //   2. blend_cache[0..N-1]  (middle — regenerated on dirty)
  //   3. blend_source_b  (top)
  // Each step / source is itself a Path SceneNode, so we re-enter
  // draw_object for each one and inherit the normal Path rendering
  // path (fill, stroke, opacity, transform). The Blend's own opacity
  // composites the whole stack as a unit via begin_alpha/end_alpha —
  // same semantics as Group.
  if (obj.type == SceneNode::Type::Blend) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    // Cache rebuild is lazy-on-read. draw_object takes obj by const
    // reference — the constness refers to scene-tree structure, not
    // the derived cache. Matches Cairo's "immediate draw" model: by
    // the time we start painting the Blend, A/B are settled and the
    // cache is guaranteed fresh for this frame.
    SceneNode *mobj = const_cast<SceneNode *>(&obj);
    if (mobj->blend_cache_dirty ||
        (int)mobj->blend_cache.size() != std::clamp(mobj->blend_steps, 1, 50)) {
      rebuild_blend_cache(mobj);
    }

    begin_alpha();
    if (obj.blend_source_a)
      draw_object(cr, *obj.blend_source_a, layer_r, layer_g, layer_b);
    for (auto &step : obj.blend_cache)
      draw_object(cr, *step, layer_r, layer_g, layer_b);
    if (obj.blend_source_b)
      draw_object(cr, *obj.blend_source_b, layer_r, layer_g, layer_b);
    end_alpha();
    cr->restore();
    return;
  }

  // ── Warp ─────────────────────────────────────────────────────────────
  // Lazy cache rebuild on draw — same pattern as Blend. If either dirty
  // flag is set OR the cache is missing (e.g. just-parsed M1-stub file
  // that didn't carry envelope data), rebuild_warp_caches brings both
  // stages in sync. draw_object takes obj by const reference; the
  // constness refers to scene-tree structure, not the derived caches,
  // so const_cast here matches the Blend precedent.
  //
  // Render priority after rebuild: warp_cache if present (normal path),
  // else warp_glyph_cache (rebuilder cleared warp_cache — means no
  // paths in source), else warp_source (rebuilder bailed entirely,
  // degenerate fallback). M1 used to always fall through to source;
  // M2 has real math, so warp_cache is the happy path.
  if (obj.type == SceneNode::Type::Warp) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    SceneNode *mobj = const_cast<SceneNode *>(&obj);
    if (mobj->warp_glyph_cache_dirty || mobj->warp_cache_dirty ||
        !mobj->warp_cache) {
      const_cast<Canvas *>(this)->rebuild_warp_caches(mobj);
    }
    begin_alpha();
    if (obj.warp_cache)
      draw_object(cr, *obj.warp_cache, layer_r, layer_g, layer_b);
    else if (obj.warp_glyph_cache)
      draw_object(cr, *obj.warp_glyph_cache, layer_r, layer_g, layer_b);
    else if (obj.warp_source)
      draw_object(cr, *obj.warp_source, layer_r, layer_g, layer_b);
    end_alpha();
    cr->restore();
    return;
  }

  // ── Compound: all children paths fed to Cairo together, EVEN_ODD fill ─
  if (obj.type == SceneNode::Type::Compound) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    begin_alpha();
    if (m_outline_mode) {
      // Outline mode — stroke each child in layer colour
      for (int i = (int)obj.children.size() - 1; i >= 0; --i) {
        const SceneNode &child = *obj.children[i];
        if (child.type != SceneNode::Type::Path || !child.path)
          continue;
        BezierPath bp = BezierPath::from_path_data(*child.path);
        bp.apply_to_cairo(cr);
        cr->set_source_rgb(layer_r, layer_g, layer_b);
        cr->set_line_width(1.0 / m_zoom);
        cr->stroke();
      }
    } else {
      // Build combined path from all children (bottom-up = last child first)
      for (int i = (int)obj.children.size() - 1; i >= 0; --i) {
        const SceneNode &child = *obj.children[i];
        if (child.type != SceneNode::Type::Path || !child.path)
          continue;
        BezierPath bp = BezierPath::from_path_data(*child.path);
        bp.apply_to_cairo(cr);
      }
      // S58g: Fill with EVEN_ODD using the COMPOUND's own fill style. A
      // Compound is one visual object (S58d rule) — its own fill is the
      // canonical source of truth; child fills are inert. This matches
      // the print-mode Compound renderer and makes inspector edits to a
      // Compound's fill visibly take effect.
      if (obj.fill.type != FillStyle::Type::None) {
        cr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);
        if (obj.fill.is_gradient()) {
          if (auto bb = object_bbox(obj, false)) {
            apply_fill(cr, obj, *bb);  // S106 m1 — cache-aware
          } else {
            apply_fill(cr, obj.fill);
          }
        } else {
          apply_fill(cr, obj.fill);
        }
        cr->fill();
        cr->set_fill_rule(Cairo::Context::FillRule::WINDING);
      } else {
        cr->begin_new_path();
      }
      // S58g: Stroke using the COMPOUND's own stroke style, applied to each
      // child subpath. A Compound's stroke sets the outline style for the
      // whole shape including hole boundaries. If the Compound has no
      // stroke, no strokes are drawn.
      if (obj.stroke.paint.type != FillStyle::Type::None) {
        for (int i = (int)obj.children.size() - 1; i >= 0; --i) {
          const SceneNode &child = *obj.children[i];
          if (child.type != SceneNode::Type::Path || !child.path)
            continue;
          BezierPath bp = BezierPath::from_path_data(*child.path);
          bp.apply_to_cairo(cr);
          apply_stroke_style(cr, obj.stroke);
          cr->stroke();
        }
      }
    }
    end_alpha();
    cr->restore();
    return;
  }

  // ── Ref point: small crosshair in layer color, fixed screen size ─────
  if (obj.type == SceneNode::Type::Ref) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    double sx, sy;
    doc_to_screen(obj.ref_x, obj.ref_y, sx, sy);
    Cairo::Matrix saved;
    cr->get_matrix(saved);
    cr->set_identity_matrix();

    bool sel = (m_selected == &obj) ||
               std::any_of(m_selection.begin(), m_selection.end(),
                           [&obj](SceneNode *s) { return s == &obj; });
    bool hovered = (m_ref_hovered == &obj);

    if (sel) {
      // Selected: white crosshair with colored outline for contrast
      cr->set_source_rgba(1.0, 1.0, 1.0, 1.0);
    } else {
      cr->set_source_rgba(layer_r, layer_g, layer_b, hovered ? 1.0 : 0.75);
    }
    cr->set_line_width(sel ? 1.5 : 1.0);
    cr->set_dash(std::vector<double>{}, 0);

    constexpr double ARM = 6.0;
    constexpr double DOT = 2.0;
    cr->move_to(sx - ARM, sy);
    cr->line_to(sx + ARM, sy);
    cr->move_to(sx, sy - ARM);
    cr->line_to(sx, sy + ARM);
    cr->stroke();
    cr->arc(sx, sy, DOT, 0, 2 * M_PI);
    cr->fill();

    // Selected: draw colored ring around the dot for visibility
    if (sel) {
      cr->set_source_rgba(layer_r, layer_g, layer_b, 1.0);
      cr->set_line_width(1.0);
      cr->arc(sx, sy, ARM - 1.0, 0, 2 * M_PI);
      cr->stroke();
    }

    cr->set_matrix(saved);
    cr->restore();
    return;
  }

  // ── Image node ───────────────────────────────────────────────────────
  if (obj.type == SceneNode::Type::Image) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    begin_alpha();
    // draw_object is called with translate(ox,oy) + scale(zoom,zoom) active.
    // image_x/y/w/h are in doc units — used directly in this transformed space.
    double dx = obj.image_x; // doc x
    double dy = obj.image_y; // doc y (Y-down)
    double dw = obj.image_w; // doc width
    double dh = obj.image_h; // doc height
    if (dw < 0.01 || dh < 0.01) {
      cr->restore();
      return;
    }

    // ── Outline mode: draw bbox wireframe in lieu of pixel content ─────
    // Outline is a structural view — paths show as stroke skeletons,
    // text as bbox outlines, images likewise. Drawing the pixbuf in
    // outline defeats the abstraction (the user is looking AT the
    // structure, not the imagery). Apply the same 2x2 transform
    // (rotation/skew around image centre) we'd apply to the pixbuf so
    // the wireframe tracks rotated images correctly.
    //
    // Rect-plus-X is the universal "image placeholder" idiom (every
    // web browser uses it for broken-image fallbacks). The diagonals
    // distinguish an image bbox from an arbitrary rect path at a glance.
    if (m_outline_mode) {
      cr->save();
      const Transform &t = obj.transform;
      bool has_transform =
          (std::abs(t.a - 1.0) > 1e-6 || std::abs(t.b) > 1e-6 ||
           std::abs(t.c) > 1e-6 || std::abs(t.d - 1.0) > 1e-6);
      if (has_transform) {
        double icx = dx + dw * 0.5;
        double icy = dy + dh * 0.5;
        cr->translate(icx, icy);
        Cairo::Matrix m(t.a, t.b, t.c, t.d, 0, 0);
        cr->transform(m);
        cr->translate(-icx, -icy);
      }
      cr->set_source_rgb(layer_r, layer_g, layer_b);
      cr->set_line_width(1.0 / m_zoom);
      // Bbox rectangle.
      cr->rectangle(dx, dy, dw, dh);
      // X diagonals — image-placeholder idiom.
      cr->move_to(dx,      dy);
      cr->line_to(dx + dw, dy + dh);
      cr->move_to(dx + dw, dy);
      cr->line_to(dx,      dy + dh);
      cr->stroke();
      cr->restore();
      end_alpha();
      cr->restore();
      return;
    }

    Cairo::RefPtr<Cairo::ImageSurface> img_surf;
    try {
      auto ext = obj.image_path.substr(obj.image_path.rfind('.') + 1);
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == "png") {
        img_surf = Cairo::ImageSurface::create_from_png(obj.image_path);
      } else {
        auto pb = Gdk::Pixbuf::create_from_file(obj.image_path);
        if (pb) {
          int pw = pb->get_width(), ph = pb->get_height();
          auto surf2 = Cairo::ImageSurface::create(
              Cairo::Surface::Format::ARGB32, pw, ph);
          auto cr2 = Cairo::Context::create(surf2);
          // s135 m2: pumped — replaces deprecated gdk_cairo_set_source_pixbuf.
          curvz::utils::cairo_set_source_pixbuf(cr2, pb, 0, 0);
          cr2->paint();
          img_surf = surf2;
        }
      }
    } catch (...) {
    }

    if (img_surf) {
      int iw = img_surf->get_width();
      int ih = img_surf->get_height();
      if (iw > 0 && ih > 0) {
        cr->save();
        // Apply obj.transform (rotation/skew) around image centre.
        // transform is stored as a 2x2 matrix (a,b,c,d) with no translation —
        // we apply it centred on the image midpoint in doc space.
        double icx = dx + dw * 0.5;
        double icy = dy + dh * 0.5;
        const Transform &t = obj.transform;
        bool has_transform =
            (std::abs(t.a - 1.0) > 1e-6 || std::abs(t.b) > 1e-6 ||
             std::abs(t.c) > 1e-6 || std::abs(t.d - 1.0) > 1e-6);
        if (has_transform) {
          // Translate to image centre, apply 2x2 matrix, translate back
          cr->translate(icx, icy);
          Cairo::Matrix m(t.a, t.b, t.c, t.d, 0, 0);
          cr->transform(m);
          cr->translate(-icx, -icy);
        }
        cr->translate(dx, dy);
        cr->scale(dw / iw, dh / ih);
        cr->set_source(img_surf, 0, 0);
        // Opacity is applied by the outer begin_alpha/end_alpha wrapper
        cr->paint();
        cr->restore();
      }
    } else {
      // Placeholder — red X when image can't load
      cr->save();
      cr->set_source_rgba(0.8, 0.2, 0.2, 0.5);
      cr->rectangle(dx, dy, dw, dh);
      cr->fill();
      cr->set_source_rgba(0.8, 0.2, 0.2, 0.9);
      cr->set_line_width(1.0 / m_zoom);
      cr->move_to(dx, dy);
      cr->line_to(dx + dw, dy + dh);
      cr->move_to(dx + dw, dy);
      cr->line_to(dx, dy + dh);
      cr->stroke();
      cr->restore();
    }

    end_alpha();
    cr->restore();
    return;
  }

  // ── Text node ─────────────────────────────────────────────────────────
  if (obj.type == SceneNode::Type::Text) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    begin_alpha();
    // draw_text_node expects the doc-space transform (translate+scale) active.
    draw_text_node(cr, obj);
    end_alpha();
    cr->restore();
    return;
  }

  if (obj.type == SceneNode::Type::Path && obj.path) {
    begin_alpha();
    BezierPath bp = BezierPath::from_path_data(*obj.path);
    bp.apply_to_cairo(cr);
  } else {
    cr->restore();
    return;
  }

  if (m_outline_mode) {
    cr->set_source_rgb(layer_r, layer_g, layer_b);
    cr->set_line_width(1.0 / m_zoom);
    cr->stroke();
  } else {
    // If this path is a text-on-path guide, suppress all fill and stroke
    // in normal (preview) mode — it should be invisible.  Exception: when
    // selected, show it as a plain stroke so the user can see its geometry.
    if (is_top_guide_path(obj)) {
      bool selected = std::any_of(m_selection.begin(), m_selection.end(),
                                  [&obj](SceneNode *s) { return s == &obj; });
      if (selected) {
        cr->set_source_rgba(0.3, 0.6, 1.0, 0.7);
        cr->set_line_width(1.0 / m_zoom);
        cr->stroke();
      } else {
        cr->begin_new_path();
      }
      end_alpha();
      cr->restore();
      return;
    }
    if (obj.fill.type != FillStyle::Type::None) {
      if (obj.fill.is_gradient()) {
        if (auto bb = object_bbox(obj, false)) {
          apply_fill(cr, obj, *bb);  // S106 m1 — cache-aware
        } else {
          apply_fill(cr, obj.fill);
        }
      } else {
        apply_fill(cr, obj.fill);
      }
      cr->fill_preserve();
    }
    if (obj.stroke.paint.type != FillStyle::Type::None) {
      apply_stroke_style(cr, obj.stroke);
      cr->stroke();
    } else {
      cr->begin_new_path();
    }
  }

  end_alpha();
  cr->restore();
}

void Canvas::draw_rubber_band(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_drawing && !(m_tool == ActiveTool::Line && m_line_tool.active()))
    return;
  // Ref tool uses a coordinate overlay instead of a rubber-band rect
  if (m_tool == ActiveTool::Ref)
    return;
  // Corner tool draws its own rubber-band in draw_corner_tool_overlay
  if (m_tool == ActiveTool::Corner)
    return;

  double x1, y1, x2, y2;

  if (m_tool == ActiveTool::Zoom) {
    // Zoom marquee uses raw screen-space coords stored in anchor + draw_cur
    x1 = std::min(m_zoom_anchor_x, m_draw_cur_dx);
    y1 = std::min(m_zoom_anchor_y, m_draw_cur_dy);
    x2 = std::max(m_zoom_anchor_x, m_draw_cur_dx);
    y2 = std::max(m_zoom_anchor_y, m_draw_cur_dy);

    double w = x2 - x1, h = y2 - y1;
    if (w < 2 || h < 2)
      return;

    // Offset by doc origin (rubber band is drawn in screen/widget space)
    double ox = doc_origin_x(), oy = doc_origin_y();
    double rx = x1 + ox - ox; // screen coords already absolute
    // Actually draw_rubber_band is called inside on_draw where cr has no
    // zoom transform applied yet — coords are raw widget pixels. Good.
    // Colour: blue = zoom in, amber = zoom out
    double cr_r, cr_g, cr_b;
    if (m_mod_alt) {
      cr_r = 0.93;
      cr_g = 0.60;
      cr_b = 0.13;
    } // amber — zoom out
    else {
      cr_r = 0.21;
      cr_g = 0.52;
      cr_b = 0.89;
    } // blue  — zoom in

    cr->save();
    cr->set_source_rgba(cr_r, cr_g, cr_b, 0.10);
    cr->rectangle(x1, y1, w, h);
    cr->fill();
    cr->set_source_rgba(cr_r, cr_g, cr_b, 1.0);
    cr->set_line_width(1.0);
    std::vector<double> zoom_dash = {5.0, 3.0};
    cr->set_dash(zoom_dash, 0);
    cr->rectangle(x1, y1, w, h);
    cr->stroke();
    // Corner accent marks — like a real marquee zoom
    const double arm = 6.0;
    cr->set_dash(std::vector<double>{}, 0);
    cr->set_line_width(1.5);
    cr->set_source_rgba(cr_r, cr_g, cr_b, 1.0);
    // top-left
    cr->move_to(x1, y1 + arm);
    cr->line_to(x1, y1);
    cr->line_to(x1 + arm, y1);
    // top-right
    cr->move_to(x2 - arm, y1);
    cr->line_to(x2, y1);
    cr->line_to(x2, y1 + arm);
    // bottom-right
    cr->move_to(x2, y2 - arm);
    cr->line_to(x2, y2);
    cr->line_to(x2 - arm, y2);
    // bottom-left
    cr->move_to(x1 + arm, y2);
    cr->line_to(x1, y2);
    cr->line_to(x1, y2 - arm);
    cr->stroke();
    cr->restore();
    return;
  }

  // ── All other tools: doc-space coords → widget space ─────────────────
  // draw_rubber_band now runs outside the cr->translate(ox,oy) block,
  // so we must add the doc origin offset ourselves.

  // Line tool — multi-segment preview
  if (m_tool == ActiveTool::Line && m_line_tool.active()) {
    double ox = doc_origin_x(), oy = doc_origin_y();
    cr->save();
    // s137 m5: creation colour (motif-aware), per-project user setting.
    cr->set_source_rgba(m_project->creation_r(),
                        m_project->creation_g(),
                        m_project->creation_b(), 0.9);
    cr->set_line_width(1.5);
    std::vector<double> dashes = {4.0, 3.0};
    cr->set_dash(dashes, 0);

    // Draw placed segments
    auto [p0x, p0y] = m_line_tool.points[0];
    cr->move_to(p0x * m_zoom + ox, p0y * m_zoom + oy);
    for (size_t i = 1; i < m_line_tool.points.size(); ++i) {
      auto [px, py] = m_line_tool.points[i];
      cr->line_to(px * m_zoom + ox, py * m_zoom + oy);
    }
    // Live rubber-band to cursor
    cr->line_to(m_line_tool.live_x * m_zoom + ox,
                m_line_tool.live_y * m_zoom + oy);
    cr->stroke();

    // Draw placed anchor points
    cr->set_dash(std::vector<double>{}, 0);
    cr->set_source_rgba(m_project->creation_r(),
                        m_project->creation_g(),
                        m_project->creation_b(), 1.0);
    for (auto [px, py] : m_line_tool.points) {
      cr->arc(px * m_zoom + ox, py * m_zoom + oy, 3.0, 0, 2 * M_PI);
      cr->fill();
    }

    // Close-snap indicator: green ring around start point
    if (m_line_tool.close_snap && m_line_tool.points.size() >= 2) {
      cr->set_source_rgba(0.1, 0.85, 0.3, 0.9);
      cr->arc(p0x * m_zoom + ox, p0y * m_zoom + oy, 8.0 / m_zoom * m_zoom, 0,
              2 * M_PI);
      cr->stroke();
    }

    cr->restore();
    return;
  }

  // ── Polygon overlay ───────────────────────────────────────────────────
  if (m_tool == ActiveTool::Polygon && m_drawing) {
    double ox = doc_origin_x(), oy = doc_origin_y();
    double dw = m_draw_cur_dx - m_draw_start_dx;
    double dh = m_draw_cur_dy - m_draw_start_dy;
    double radius = std::hypot(dw, dh);
    if (radius < 1.0)
      return;

    double cx = m_draw_start_dx;
    double cy = m_draw_start_dy;

    int sides = m_poly_sides;
    double inflection = m_poly_inflection;

    // Snap inflection display
    double perfect_star =
        (sides >= 5) ? std::cos(2.0 * M_PI / sides) / std::cos(M_PI / sides)
                     : -1.0;
    if (perfect_star > 0.0 && std::abs(inflection - perfect_star) < 0.04)
      inflection = perfect_star;
    if (inflection > 0.985)
      inflection = 1.0;

    // Build path and draw in screen space
    PathData pd =
        polygon_to_path(cx, cy, radius, sides, inflection, m_poly_drag_angle);

    cr->save();
    // s137 m5: creation colour (motif-aware) for both fill and outline.
    // Fill alpha 0.12 / outline alpha 0.9 are role-coded — fills always
    // dimmer than outlines at every construction site.
    // Fill
    cr->set_source_rgba(m_project->creation_r(),
                        m_project->creation_g(),
                        m_project->creation_b(), 0.12);
    bool first = true;
    for (const auto &n : pd.nodes) {
      double sx = n.x * m_zoom + ox;
      double sy = n.y * m_zoom + oy;
      if (first) {
        cr->move_to(sx, sy);
        first = false;
      } else
        cr->line_to(sx, sy);
    }
    cr->close_path();
    cr->fill();

    // Outline
    cr->set_source_rgba(m_project->creation_r(),
                        m_project->creation_g(),
                        m_project->creation_b(), 0.9);
    cr->set_line_width(1.0);
    std::vector<double> dashes = {4.0, 3.0};
    cr->set_dash(dashes, 0);
    first = true;
    for (const auto &n : pd.nodes) {
      double sx = n.x * m_zoom + ox;
      double sy = n.y * m_zoom + oy;
      if (first) {
        cr->move_to(sx, sy);
        first = false;
      } else
        cr->line_to(sx, sy);
    }
    cr->close_path();
    cr->stroke();
    cr->restore();
    return;
  }

  // ── Spiral overlay ────────────────────────────────────────────────────
  if (m_tool == ActiveTool::Spiral && m_drawing) {
    double ox = doc_origin_x(), oy = doc_origin_y();
    double dw = m_draw_cur_dx - m_draw_start_dx;
    double dh = m_draw_cur_dy - m_draw_start_dy;
    double outer_r = std::hypot(dw, dh);
    if (outer_r < 1.0)
      return;

    double cx = m_draw_start_dx;
    double cy = m_draw_start_dy;
    double inner_r = outer_r * (m_spiral_inner / 100.0);

    PathData pd = spiral_to_path(cx, cy, outer_r, inner_r,
                                 m_spiral_turns, m_spiral_drag_angle);

    cr->save();
    // s137 m5: creation colour (motif-aware).
    cr->set_source_rgba(m_project->creation_r(),
                        m_project->creation_g(),
                        m_project->creation_b(), 0.9);
    cr->set_line_width(1.0);
    std::vector<double> dashes = {4.0, 3.0};
    cr->set_dash(dashes, 0);

    bool first = true;
    for (int i = 0; i < (int)pd.nodes.size(); ++i) {
      const auto &n = pd.nodes[i];
      double sx = n.x * m_zoom + ox;
      double sy = n.y * m_zoom + oy;
      if (first) {
        cr->move_to(sx, sy);
        first = false;
      } else {
        // Use Bézier handles if available
        const auto &prev = pd.nodes[i - 1];
        double cx2s = prev.cx2 * m_zoom + ox;
        double cy2s = prev.cy2 * m_zoom + oy;
        double cx1s = n.cx1 * m_zoom + ox;
        double cy1s = n.cy1 * m_zoom + oy;
        cr->curve_to(cx2s, cy2s, cx1s, cy1s, sx, sy);
      }
    }
    cr->stroke();
    cr->restore();
    return;
  }

  {
    double ox = doc_origin_x(), oy = doc_origin_y();
    x1 = std::min(m_draw_start_effective_dx, m_draw_cur_dx) * m_zoom + ox;
    y1 = std::min(m_draw_start_effective_dy, m_draw_cur_dy) * m_zoom + oy;
    x2 = std::max(m_draw_start_effective_dx, m_draw_cur_dx) * m_zoom + ox;
    y2 = std::max(m_draw_start_effective_dy, m_draw_cur_dy) * m_zoom + oy;
  }
  double w = x2 - x1;
  double h = y2 - y1;
  if (w < 1 || h < 1)
    return;

  cr->save();

  // Fill preview — s137 m5: creation colour (motif-aware), dim alpha for fill.
  cr->set_source_rgba(m_project->creation_r(),
                      m_project->creation_g(),
                      m_project->creation_b(), 0.12);
  if (m_tool == ActiveTool::Ellipse) {
    cr->save();
    cr->translate(x1 + w * 0.5, y1 + h * 0.5);
    cr->scale(w * 0.5, h * 0.5);
    cr->arc(0, 0, 1.0, 0, 2 * M_PI);
    cr->restore();
  } else {
    cr->rectangle(x1, y1, w, h);
  }
  cr->fill();

  // Outline — s137 m5: creation colour (motif-aware), bold alpha for outline.
  cr->set_source_rgba(m_project->creation_r(),
                      m_project->creation_g(),
                      m_project->creation_b(), 0.9);
  cr->set_line_width(1.0);
  std::vector<double> dashes = {4.0, 3.0};
  cr->set_dash(dashes, 0);
  if (m_tool == ActiveTool::Ellipse) {
    cr->save();
    cr->translate(x1 + w * 0.5, y1 + h * 0.5);
    cr->scale(w * 0.5, h * 0.5);
    cr->arc(0, 0, 1.0, 0, 2 * M_PI);
    cr->restore();
  } else {
    cr->rectangle(x1, y1, w, h);
  }
  cr->stroke();

  cr->restore();
}

void Canvas::draw_marquee(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_marquee_active)
    return;

  double ox = doc_origin_x(), oy = doc_origin_y();
  double x1 = std::min(m_marquee_start_dx, m_marquee_cur_dx) * m_zoom + ox;
  double y1 = std::min(m_marquee_start_dy, m_marquee_cur_dy) * m_zoom + oy;
  double x2 = std::max(m_marquee_start_dx, m_marquee_cur_dx) * m_zoom + ox;
  double y2 = std::max(m_marquee_start_dy, m_marquee_cur_dy) * m_zoom + oy;
  double w = x2 - x1, h = y2 - y1;
  if (w < 1 || h < 1)
    return;

  cr->save();
  // Fill
  cr->set_source_rgba(0.3, 0.6, 1.0, 0.08);
  cr->rectangle(x1, y1, w, h);
  cr->fill();
  // Outline — dashed blue
  cr->set_source_rgba(0.3, 0.6, 1.0, 0.85);
  cr->set_line_width(1.0);
  std::vector<double> dashes = {4.0, 3.0};
  cr->set_dash(dashes, 0);
  cr->rectangle(x1, y1, w, h);
  cr->stroke();
  cr->restore();
}

// ── Handle geometry helpers
// ──────────────────────────────────────────────────── Returns the 8 handle
// screen positions for a given BBX. Order: NW, N, NE, E, SE, S, SW, W  (index
// matches HandleKind enum - 1)
static void selection_handle_positions(double sx1, double sy1, double sx2,
                                       double sy2, double hx[8], double hy[8]) {
  double mx = (sx1 + sx2) * 0.5;
  double my = (sy1 + sy2) * 0.5;
  hx[0] = sx1;
  hy[0] = sy1; // NW
  hx[1] = mx;
  hy[1] = sy1; // N
  hx[2] = sx2;
  hy[2] = sy1; // NE
  hx[3] = sx2;
  hy[3] = my; // E
  hx[4] = sx2;
  hy[4] = sy2; // SE
  hx[5] = mx;
  hy[5] = sy2; // S
  hx[6] = sx1;
  hy[6] = sy2; // SW
  hx[7] = sx1;
  hy[7] = my; // W
}

Canvas::HandleKind Canvas::handle_hit_test(double sx, double sy) const {
  if (m_selection.empty())
    return HandleKind::None;

  // Compute union BBX of all selected objects
  bool found = false;
  double bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
  for (SceneNode *obj : m_selection) {
    auto bb = object_bbox(*obj);
    if (!bb)
      continue;
    double s1x, s1y, s2x, s2y;
    doc_to_screen(bb->x, bb->y, s1x, s1y);
    doc_to_screen(bb->x + bb->w, bb->y + bb->h, s2x, s2y);
    if (!found) {
      bx1 = s1x;
      by1 = s1y;
      bx2 = s2x;
      by2 = s2y;
      found = true;
    } else {
      bx1 = std::min(bx1, s1x);
      by1 = std::min(by1, s1y);
      bx2 = std::max(bx2, s2x);
      by2 = std::max(by2, s2y);
    }
  }
  if (!found)
    return HandleKind::None;

  double hx[8], hy[8];
  selection_handle_positions(bx1, by1, bx2, by2, hx, hy);

  // Test corners first (higher priority — they overlap edge mid zones at small
  // sizes) Array index matches HandleKind enum order:
  // NW=1,N=2,NE=3,E=4,SE=5,S=6,SW=7,W=8 positions array:
  // 0=NW,1=N,2=NE,3=E,4=SE,5=S,6=SW,7=W
  const int pos_idx[8] = {0, 2, 4, 6, 1, 3, 5, 7};
  const HandleKind hkind[8] = {
      HandleKind::NW, HandleKind::NE,
      HandleKind::SE, HandleKind::SW, // corners first
      HandleKind::N,  HandleKind::E,
      HandleKind::S,  HandleKind::W // then edges
  };
  for (int i = 0; i < 8; ++i) {
    double ddx = sx - hx[pos_idx[i]];
    double ddy = sy - hy[pos_idx[i]];
    if (std::abs(ddx) <= HANDLE_HIT_PX && std::abs(ddy) <= HANDLE_HIT_PX)
      return hkind[i];
  }

  // Rotate zones — annular ring just outside each corner square.
  // Inner radius = HANDLE_HIT_PX (edge of corner square).
  // Outer radius = HANDLE_HIT_PX + ROTATE_RING_PX.
  static constexpr double ROTATE_RING_PX = 10.0;
  const int corner_pos[4] = {0, 2, 4, 6};
  const HandleKind rotate_kind[4] = {HandleKind::RotateNW, HandleKind::RotateNE,
                                     HandleKind::RotateSE,
                                     HandleKind::RotateSW};
  for (int i = 0; i < 4; ++i) {
    double ddx = sx - hx[corner_pos[i]];
    double ddy = sy - hy[corner_pos[i]];
    double dist = std::hypot(ddx, ddy);
    if (dist >= HANDLE_HIT_PX && dist <= HANDLE_HIT_PX + ROTATE_RING_PX)
      return rotate_kind[i];
  }

  return HandleKind::None;
}

void Canvas::draw_selection_handles(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (m_selection.empty())
    return;

  // During a rotate drag: draw the rotating BBX outline + a pivot crosshair.
  // The normal handle squares are suppressed — they'd dance around confusingly.
  bool is_rotating = (m_handle_drag == HandleKind::RotateNW ||
                      m_handle_drag == HandleKind::RotateNE ||
                      m_handle_drag == HandleKind::RotateSE ||
                      m_handle_drag == HandleKind::RotateSW);

  // Compute union BBX from live node positions (follows the rotating object)
  bool found = false;
  double sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;
  for (SceneNode *obj : m_selection) {
    auto bb = object_bbox(*obj);
    if (!bb)
      continue;
    double a1, b1, a2, b2;
    doc_to_screen(bb->x, bb->y, a1, b1);
    doc_to_screen(bb->x + bb->w, bb->y + bb->h, a2, b2);
    if (!found) {
      sx1 = a1;
      sy1 = b1;
      sx2 = a2;
      sy2 = b2;
      found = true;
    } else {
      sx1 = std::min(sx1, a1);
      sy1 = std::min(sy1, b1);
      sx2 = std::max(sx2, a2);
      sy2 = std::max(sy2, b2);
    }
  }
  if (!found)
    return;

  cr->save();

  // Selection rect — dashed blue
  cr->set_source_rgba(0.3, 0.6, 1.0, 0.8);
  cr->set_line_width(1.0);
  std::vector<double> dashes = {5.0, 3.0};
  cr->set_dash(dashes, 0);
  cr->rectangle(sx1, sy1, sx2 - sx1, sy2 - sy1);
  cr->stroke();
  cr->set_dash(std::vector<double>{}, 0);

  if (is_rotating) {
    // Draw the original start BBX as a ghost
    double gx1, gy1, gx2, gy2;
    doc_to_screen(m_handle_start_bb.x, m_handle_start_bb.y, gx1, gy1);
    doc_to_screen(m_handle_start_bb.x + m_handle_start_bb.w,
                  m_handle_start_bb.y + m_handle_start_bb.h, gx2, gy2);
    cr->set_source_rgba(1.0, 0.5, 0.0, 0.5);
    cr->set_line_width(1.5);
    std::vector<double> ghost_dash = {3.0, 3.0};
    cr->set_dash(ghost_dash, 0);
    cr->rectangle(gx1, gy1, gx2 - gx1, gy2 - gy1);
    cr->stroke();
    cr->set_dash(std::vector<double>{}, 0);

    // Pivot crosshair — bright red
    double pvx, pvy;
    doc_to_screen(m_handle_pivot_x, m_handle_pivot_y, pvx, pvy);
    const double arm = 14.0;
    cr->set_source_rgba(1.0, 0.1, 0.1, 1.0);
    cr->set_line_width(2.0);
    cr->move_to(pvx - arm, pvy);
    cr->line_to(pvx + arm, pvy);
    cr->stroke();
    cr->move_to(pvx, pvy - arm);
    cr->line_to(pvx, pvy + arm);
    cr->stroke();
    cr->arc(pvx, pvy, 5.0, 0, 2 * M_PI);
    cr->fill();
    cr->restore();
    return;
  }

  double hx[8], hy[8];
  selection_handle_positions(sx1, sy1, sx2, sy2, hx, hy);

  const double hs = 5.0; // handle square half-size

  // Draw all 8 handle positions — corners solid, edge mids slightly smaller
  for (int i = 0; i < 8; ++i) {
    bool is_corner = (i % 2 == 0);
    double sz = is_corner ? hs : hs * 0.85;
    cr->set_source_rgb(1.0, 1.0, 1.0);
    cr->rectangle(hx[i] - sz, hy[i] - sz, sz * 2, sz * 2);
    cr->fill();
    cr->set_source_rgba(0.3, 0.6, 1.0, 1.0);
    cr->set_line_width(1.0);
    cr->rectangle(hx[i] - sz, hy[i] - sz, sz * 2, sz * 2);
    cr->stroke();
  }

  // Draw custom pivot point if set (or R held showing default), or if the
  // Step-and-Repeat dialog is showing its pivot preview.
  const bool draw_sr = m_sr_preview_active;
  if (m_has_custom_pivot || m_r_held || draw_sr) {
    double pvx, pvy;
    if (draw_sr)
      doc_to_screen(m_sr_preview_x, m_sr_preview_y, pvx, pvy);
    else
      doc_to_screen(m_custom_pivot_x, m_custom_pivot_y, pvx, pvy);

    const double arm = 10.0;
    const double radius = 4.0;

    // Shadow
    cr->set_source_rgba(0.0, 0.0, 0.0, 0.5);
    cr->set_line_width(3.0);
    cr->move_to(pvx - arm, pvy);
    cr->line_to(pvx + arm, pvy);
    cr->stroke();
    cr->move_to(pvx, pvy - arm);
    cr->line_to(pvx, pvy + arm);
    cr->stroke();

    // Bright orange crosshair (distinct from blue selection handles)
    cr->set_source_rgba(1.0, 0.55, 0.0, 1.0);
    cr->set_line_width(1.5);
    cr->move_to(pvx - arm, pvy);
    cr->line_to(pvx + arm, pvy);
    cr->stroke();
    cr->move_to(pvx, pvy - arm);
    cr->line_to(pvx, pvy + arm);
    cr->stroke();

    // Circle at centre
    cr->set_line_width(1.5);
    cr->arc(pvx, pvy, radius, 0, 2 * M_PI);
    cr->stroke();
  }

  cr->restore();
}

// ── Ruler Tool
// ────────────────────────────────────────────────────────────────

// Called on tool switch from Node tool — inherit exactly 2 selected nodes.
void Canvas::ruler_try_inherit_node_selection() {
  m_ruler_node_a_obj = nullptr;
  m_ruler_node_a_idx = -1;
  m_ruler_node_b_obj = nullptr;
  m_ruler_node_b_idx = -1;
  if (m_node_selection.size() == 2) {
    m_ruler_node_a_obj = m_node_selection[0].obj;
    m_ruler_node_a_idx = m_node_selection[0].node_idx;
    m_ruler_node_b_obj = m_node_selection[1].obj;
    m_ruler_node_b_idx = m_node_selection[1].node_idx;
  }
  queue_draw();
}

// Collect all visible, unlocked PATH NODES across all layers. Used by
// non-ruler callers (guide-construct snap, selection-tool node-snap,
// hover highlight) that explicitly want path-node-only results — they
// read obj->path->nodes[ni] directly. Refpt callers must use
// ruler_collect_all_endpoints.
void Canvas::ruler_collect_all_path_nodes(
    std::vector<std::pair<SceneNode *, int>> &out) const {
  if (!m_doc)
    return;
  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->locked || layer->is_special_layer())
      continue;
    for (auto &obj_uptr : layer->children) {
      SceneNode &obj = *obj_uptr;
      if (obj.type != SceneNode::Type::Path || !obj.path)
        continue;
      if (obj.locked)
        continue;
      for (int i = 0; i < (int)obj.path->nodes.size(); ++i)
        out.push_back({&obj, i});
    }
  }
}

// Collect all visible, unlocked endpoints across all layers — path nodes
// AND refpts. Refpts emitted with idx = -1 (sentinel; see ruler_endpoint_pos).
// The ref layer is a special layer normally skipped by the regular layer
// filter — handled explicitly here so refpts are pickable from the ruler
// tool. Renamed from ruler_collect_all_nodes in S89.
void Canvas::ruler_collect_all_endpoints(
    std::vector<std::pair<SceneNode *, int>> &out) const {
  if (!m_doc)
    return;
  // Path nodes — same set as ruler_collect_all_path_nodes.
  ruler_collect_all_path_nodes(out);
  // Refpts — visible + unlocked entries on the ref layer, idx = -1.
  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->locked)
      continue;
    if (!layer->is_ref_layer())
      continue;
    for (auto &child_uptr : layer->children) {
      SceneNode &child = *child_uptr;
      if (!child.is_ref())
        continue;
      if (child.locked)
        continue;
      out.push_back({&child, -1});
    }
  }
}

// Resolve an endpoint to a doc-space (Y-down) position. Branch on kind:
//   - Ref:  returns (ref_x, ref_y) directly.
//   - Node: returns the BezierNode's (x, y) at the given index.
// Returns false if the SceneNode pointer is null or not a recognised
// endpoint kind, or if a Node idx is out of range.
bool Canvas::ruler_endpoint_pos(SceneNode *obj, int idx,
                                double &x_doc, double &y_doc) const {
  if (!obj)
    return false;
  if (obj->is_ref()) {
    x_doc = obj->ref_x;
    y_doc = obj->ref_y;
    return true;
  }
  if (obj->type == SceneNode::Type::Path && obj->path &&
      idx >= 0 && idx < (int)obj->path->nodes.size()) {
    const BezierNode &n = obj->path->nodes[idx];
    x_doc = n.x;
    y_doc = n.y;
    return true;
  }
  return false;
}

void Canvas::on_ruler_begin(double x, double y) {
  if (!m_doc)
    return;
  double dx, dy;
  screen_to_doc(x, y, dx, dy);
  // dy is in Cairo doc space (Y-down); convert to user space (Y-up)
  double ux = dx;
  double uy = m_doc->canvas_height() - dy;

  double tol = 8.0 / m_zoom;

  // ── Click-to-copy hit test first ───────────────────────────────────────
  // S89: m_ruler_labels carries hit-test rects from BOTH the live ruler
  // triangle (when A/B are picked) AND saved measurements drawn by the
  // persistent overlay (gated on ruler_active in the persistent draw
  // block). So a non-empty labels vector means there's something
  // clickable; we don't need to pre-check live A/B presence.
  if (!m_ruler_labels.empty()) {
    for (auto &lbl : m_ruler_labels) {
      if (x >= lbl.sx && x <= lbl.sx + lbl.sw && y >= lbl.sy &&
          y <= lbl.sy + lbl.sh) {

        // S89: single-click on any label always copies the full
        // structured block (x1,y1 / x2,y2 / Δx,Δy / distance / angles).
        // Alt-modifier no longer distinguishes — both produce the same
        // payload. The label that was clicked is recorded in copy_value
        // by the renderer (set to the structured block uniformly).
        std::string to_copy = lbl.copy_value;
        // Copy to clipboard via Gdk::ContentProvider (GTK4 API)
        auto disp = get_display();
        if (disp) {
          auto clip = disp->get_clipboard();
          if (clip) {
            Glib::Value<Glib::ustring> val;
            val.init(Glib::Value<Glib::ustring>::value_type());
            val.set(Glib::ustring(to_copy));
            clip->set_content(Gdk::ContentProvider::create(val));
          }
        }
        m_ruler_toast_text = "Copied measurement data";
        m_ruler_toast_x = lbl.sx + lbl.sw * 0.5;
        m_ruler_toast_y = lbl.sy - 6;
        m_ruler_toast_ms = 1500;
        if (m_ruler_toast_conn.connected())
          m_ruler_toast_conn.disconnect();
        m_ruler_toast_conn = Glib::signal_timeout().connect(
            [this]() -> bool {
              m_ruler_toast_ms -= 50;
              if (m_ruler_toast_ms <= 0) {
                m_ruler_toast_ms = 0;
              }
              queue_draw();
              return m_ruler_toast_ms > 0;
            },
            50);

        // S89: "Delete on copy" applies only to TRANSIENT measurements —
        // i.e. the live A/B picks shown by the ruler tool overlay. Saved
        // entries on the measure layer are NEVER auto-deleted by copy
        // (permanent until manually × deleted from inspector or layers).
        // When measure_save_to_layer is ON, completion auto-saves and the
        // live A/B are reset at save-time, so by the time the user sees a
        // label to copy, the underlying entry is a saved one — destruct
        // doesn't apply. When OFF, the live measurement is transient and
        // destruct dismisses it from canvas.
        if (m_doc->measure_destruct_after_copy &&
            !m_doc->measure_save_to_layer) {
          // Reset ruler session state so the next click starts a fresh
          // pick — same as hitting Space. Done inline rather than calling
          // ruler_clear() so the "Copied …" toast stays visible.
          m_ruler_node_a_obj = nullptr;
          m_ruler_node_a_idx = -1;
          m_ruler_node_b_obj = nullptr;
          m_ruler_node_b_idx = -1;
          m_ruler_labels.clear();
        }
        queue_draw();
        return;
      }
    }
  }

  // ── Endpoint hit test — pick closest node OR refpt within tolerance ───
  // S89: refpts joined the candidate set. ruler_endpoint_pos resolves
  // both kinds to a doc-space position uniformly.
  std::vector<std::pair<SceneNode *, int>> all_nodes;
  ruler_collect_all_endpoints(all_nodes);

  SceneNode *hit_obj = nullptr;
  int hit_idx = -1;
  double best_d = tol;

  for (auto &[obj, ni] : all_nodes) {
    double ex, ey;
    if (!ruler_endpoint_pos(obj, ni, ex, ey))
      continue;
    double d = std::hypot(ex - dx, ey - dy);
    if (d < best_d) {
      best_d = d;
      hit_obj = obj;
      hit_idx = ni;
    }
  }

  if (!hit_obj) {
    // Clicked empty space — start marquee to box-select nodes
    m_marquee_active = true;
    m_marquee_start_dx = dx;
    m_marquee_start_dy = dy;
    m_marquee_cur_dx = dx;
    m_marquee_cur_dy = dy;
    return;
  }

  // Shift+click — add/remove from the two-node selection
  if (m_mod_shift) {
    // If this node is already A or B, deselect it
    bool was_a =
        (hit_obj == m_ruler_node_a_obj && hit_idx == m_ruler_node_a_idx);
    bool was_b =
        (hit_obj == m_ruler_node_b_obj && hit_idx == m_ruler_node_b_idx);
    if (was_a) {
      m_ruler_node_a_obj = m_ruler_node_b_obj;
      m_ruler_node_a_idx = m_ruler_node_b_idx;
      m_ruler_node_b_obj = nullptr;
      m_ruler_node_b_idx = -1;
    } else if (was_b) {
      m_ruler_node_b_obj = nullptr;
      m_ruler_node_b_idx = -1;
    } else {
      // Promote A→B, set new as A
      m_ruler_node_b_obj = m_ruler_node_a_obj;
      m_ruler_node_b_idx = m_ruler_node_a_idx;
      m_ruler_node_a_obj = hit_obj;
      m_ruler_node_a_idx = hit_idx;
      // S89: shift+click that lands a fresh {A,B} pair is a completion
      // event — auto-save if the doc flag is on. Helper resets A/B on save.
      ruler_try_auto_save();
    }
  } else {
    // Plain click — set as A, clear B
    m_ruler_node_a_obj = hit_obj;
    m_ruler_node_a_idx = hit_idx;
    m_ruler_node_b_obj = nullptr;
    m_ruler_node_b_idx = -1;
  }
  queue_draw();
}

void Canvas::on_ruler_motion(double /*x*/, double /*y*/) {
  // Marquee redraws are handled by on_draw_update writing m_marquee_cur_*
  queue_draw();
}

void Canvas::on_ruler_end(double /*x*/, double /*y*/) {
  if (!m_doc)
    return;
  if (!m_marquee_active)
    return;

  m_marquee_active = false;
  double x1 = std::min(m_marquee_start_dx, m_marquee_cur_dx);
  double y1 = std::min(m_marquee_start_dy, m_marquee_cur_dy);
  double x2 = std::max(m_marquee_start_dx, m_marquee_cur_dx);
  double y2 = std::max(m_marquee_start_dy, m_marquee_cur_dy);

  if (x2 - x1 < 1.0 || y2 - y1 < 1.0) {
    queue_draw();
    return;
  }

  // Collect endpoints inside marquee rect (doc space, Y-down). S89: refpts
  // are picked alongside path nodes — same kind logic as the click hit test.
  std::vector<std::pair<SceneNode *, int>> all_nodes;
  ruler_collect_all_endpoints(all_nodes);
  std::vector<std::pair<SceneNode *, int>> inside;
  for (auto &[obj, ni] : all_nodes) {
    double ex, ey;
    if (!ruler_endpoint_pos(obj, ni, ex, ey))
      continue;
    if (ex >= x1 && ex <= x2 && ey >= y1 && ey <= y2)
      inside.push_back({obj, ni});
  }

  if (inside.size() == 0) {
    queue_draw();
    return;
  }
  if (inside.size() == 1) {
    m_ruler_node_a_obj = inside[0].first;
    m_ruler_node_a_idx = inside[0].second;
    m_ruler_node_b_obj = nullptr;
    m_ruler_node_b_idx = -1;
    queue_draw();
    return;
  }
  if (inside.size() == 2) {
    m_ruler_node_a_obj = inside[0].first;
    m_ruler_node_a_idx = inside[0].second;
    m_ruler_node_b_obj = inside[1].first;
    m_ruler_node_b_idx = inside[1].second;
    // S89: marquee-with-2 is a single user gesture that completes the pair.
    // Auto-save if flag is on. Helper resets A/B on save.
    ruler_try_auto_save();
    queue_draw();
    return;
  }

  // >2 nodes — inform user
  if (auto *win = dynamic_cast<Gtk::Window *>(get_root()))
    curvz::utils::show_alert(
        *win, "Too many nodes selected",
        "Only 2 nodes can be measured at a time. Please marquee "
        "exactly 2 nodes.");
  queue_draw();
}

// Clear ruler state — Space key, fresh pick.
void Canvas::ruler_clear() {
  m_ruler_node_a_obj = nullptr;
  m_ruler_node_a_idx = -1;
  m_ruler_node_b_obj = nullptr;
  m_ruler_node_b_idx = -1;
  m_ruler_labels.clear();
  if (m_ruler_toast_conn.connected())
    m_ruler_toast_conn.disconnect();
  m_ruler_toast_ms = 0;
  m_marquee_active = false;
  queue_draw();
}

// Called when Enter is pressed while Ruler tool is active.
void Canvas::ruler_place_measurement() {
  if (!m_doc)
    return;
  if (!m_ruler_node_a_obj || !m_ruler_node_b_obj)
    return;

  // S89: endpoint position resolves both Node and Ref kinds. If the read
  // fails for either side (e.g. a stale path-node idx after a path edit
  // wiped the underlying nodes), bail rather than write garbage coords.
  double na_x, na_y, nb_x, nb_y;
  if (!ruler_endpoint_pos(m_ruler_node_a_obj, m_ruler_node_a_idx, na_x, na_y))
    return;
  if (!ruler_endpoint_pos(m_ruler_node_b_obj, m_ruler_node_b_idx, nb_x, nb_y))
    return;

  // Convert doc-space Y-down → user-space Y-up
  double ax = na_x, ay = m_doc->canvas_height() - na_y;
  double bx = nb_x, by = m_doc->canvas_height() - nb_y;

  SceneNode *ml = m_doc->ensure_measure_layer();

  // S89: no display name stored — the layers panel and inspector both
  // synthesise a coords-with-unit string at render time so the label is
  // always live with respect to the doc's display unit. internal_id is
  // a fresh UUID giving stable identity across renames/reorders/round-
  // trip, mirroring how every other SceneNode is identified.
  auto mn = std::make_unique<SceneNode>();
  mn->type = SceneNode::Type::Measurement;
  mn->internal_id = generate_internal_id();
  mn->measure_x1 = ax;
  mn->measure_y1 = ay;
  mn->measure_x2 = bx;
  mn->measure_y2 = by;
  ml->children.push_back(std::move(mn));

  // Signal layer panel + inspector to rebuild. Selection signal hits the
  // existing layers-panel refresh chain; measurements_changed is the
  // dedicated S89 signal so MainWindow can refresh the inspector's
  // saved-measurements list (signal_selection_changed dedups against
  // current_object and skips when no object is selected).
  m_sig_selection.emit(nullptr);
  m_sig_measurements_changed.emit();
  queue_draw();
}

// S89: auto-save the live {A,B} measurement when measure_save_to_layer is ON.
// Called from completion paths in on_ruler_begin (shift+click) and on_ruler_end
// (marquee-with-2). On save the live A/B is cleared (acts like an implicit
// Space) — the persistent overlay then re-renders the same triangle + labels
// from the saved entry, so the user gets visual continuity. Returns true on
// save. NOT called from ruler_try_inherit_node_selection — inheritance from
// the Node tool is not a user-initiated completion.
bool Canvas::ruler_try_auto_save() {
  if (!m_doc)
    return false;
  if (!m_doc->measure_save_to_layer)
    return false;
  if (!m_ruler_node_a_obj || !m_ruler_node_b_obj)
    return false;
  ruler_place_measurement();
  // Clear live picks — implicit Space. The persistent overlay (drawn after
  // the live one in on_draw) will render the just-saved entry at the same
  // screen coords with the same triangle + labels, so visual continuity is
  // preserved. m_ruler_labels are rebuilt every frame by draw_ruler_overlay,
  // so clearing here just lets the next frame populate them from the
  // persistent measurement instead of the live picks.
  m_ruler_node_a_obj = nullptr;
  m_ruler_node_a_idx = -1;
  m_ruler_node_b_obj = nullptr;
  m_ruler_node_b_idx = -1;
  m_ruler_labels.clear();
  return true;
}

// S89: structured copy block formatter — takes user-space coords explicitly
// so it can be called for both live ruler picks AND saved measurements.
// Returns the multi-line "x1 = ..., y1 = ...\n..." block, values formatted
// in the doc's display unit. Static so the persistent overlay can call it
// without an instance reference.
std::string Canvas::format_structured_block_for(const CurvzDocument *doc,
                                                double ax, double ay,
                                                double bx, double by) {
  if (!doc)
    return {};
  double dx = std::abs(bx - ax);
  double dy = std::abs(by - ay);
  double dist = std::hypot(bx - ax, by - ay);
  // Angle CCW from +X axis (Y-up), in degrees
  double angle_rad = std::atan2(by - ay, bx - ax);
  double angle_deg = angle_rad * 180.0 / M_PI;
  if (angle_deg < 0)
    angle_deg += 360.0;
  double alpha = std::atan2(dy, dx) * 180.0 / M_PI; // angle at A
  double beta = 90.0 - alpha;                       // angle at B

  Unit u = doc->canvas.display_unit;
  const char *ul = UnitSystem::label(u);
  auto fmt = [&](double v) -> std::string {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4g %s", UnitSystem::from_px(v, u), ul);
    return buf;
  };
  auto fmt_deg = [](double v) -> std::string {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f°", v);
    return buf;
  };

  std::string s;
  s += "x\u2081 = " + fmt(ax) + ",  y\u2081 = " + fmt(ay) + "\n";
  s += "x\u2082 = " + fmt(bx) + ",  y\u2082 = " + fmt(by) + "\n";
  s += "\u0394X = " + fmt(dx) + ",  \u0394Y = " + fmt(dy) + "\n";
  s += "Distance = " + fmt(dist) + "\n";
  s += "Angle = " + fmt_deg(angle_deg) + ",  \u03b1 = " + fmt_deg(alpha) +
       ",  \u03b2 = " + fmt_deg(beta) + "\n";
  return s;
}

// Thin wrapper — resolve live A/B endpoints to user-space coords, then
// defer to format_structured_block_for. Returns empty when picks are
// incomplete or the resolver fails.
std::string Canvas::ruler_structured_block() const {
  if (!m_ruler_node_a_obj || !m_ruler_node_b_obj)
    return {};
  if (!m_doc)
    return {};
  // S89: endpoint position resolves both Node and Ref kinds.
  double na_x, na_y, nb_x, nb_y;
  if (!ruler_endpoint_pos(m_ruler_node_a_obj, m_ruler_node_a_idx, na_x, na_y))
    return {};
  if (!ruler_endpoint_pos(m_ruler_node_b_obj, m_ruler_node_b_idx, nb_x, nb_y))
    return {};
  // User-space Y-up
  double ax = na_x, ay = m_doc->canvas_height() - na_y;
  double bx = nb_x, by = m_doc->canvas_height() - nb_y;
  return format_structured_block_for(m_doc, ax, ay, bx, by);
}

// ── draw_measurement_annotations
// ────────────────────────────────────────────
// Shared annotation render — given user-space (Y-up) endpoint coords,
// draws the triangle (hypotenuse + legs + right-angle box + endpoint
// dots) plus the seven structured labels (distance, Δx, Δy, α, β at
// each endpoint, plus tiny coord tags at A and B). Used by both the
// live ruler overlay and the persistent (saved) measurement overlay
// so they look identical.
//
// push_labels: when true, every label is appended to m_ruler_labels
// with copy_value = the structured block, enabling click-to-copy
// while in ruler mode.
void Canvas::draw_measurement_annotations(
    const Cairo::RefPtr<Cairo::Context> &cr,
    double ax_user, double ay_user, double bx_user, double by_user,
    bool push_labels) {
  if (!m_doc)
    return;

  // ── Convert user-space (Y-up) to screen coords ──────────────────────────
  double doc_ay = m_doc->canvas_height() - ay_user;
  double doc_by = m_doc->canvas_height() - by_user;
  double asx, asy, bsx, bsy;
  doc_to_screen(ax_user, doc_ay, asx, asy);
  doc_to_screen(bx_user, doc_by, bsx, bsy);

  // C is the right-angle corner: same X as B, same Y as A
  double csx = bsx, csy = asy;

  double ddx = std::abs(bx_user - ax_user);
  double ddy = std::abs(by_user - ay_user);
  double dist = std::hypot(bx_user - ax_user, by_user - ay_user);
  double angle_rad = std::atan2(by_user - ay_user, bx_user - ax_user);
  double angle_deg = angle_rad * 180.0 / M_PI;
  if (angle_deg < 0)
    angle_deg += 360.0;
  double alpha = std::atan2(ddy, ddx) * 180.0 / M_PI;
  double beta = 90.0 - alpha;

  Unit u = m_doc->canvas.display_unit;
  const char *ul = UnitSystem::label(u);
  auto fmt_val = [&](double v) -> std::string {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4g", UnitSystem::from_px(v, u));
    return buf;
  };
  auto fmt_lbl = [&](double v) -> std::string {
    return fmt_val(v) + std::string(" ") + ul;
  };
  auto fmt_deg_lbl = [](double v) -> std::string {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f\xc2\xb0", v);
    return buf;
  };

  // S89: the same structured block is the copy_value for every label.
  // Computed once here rather than per-label.
  std::string structured =
      format_structured_block_for(m_doc, ax_user, ay_user, bx_user, by_user);

  // ── Draw triangle lines ───────────────────────────────────────────────
  cr->save();

  // Hypotenuse A→B  (blue)
  cr->set_source_rgba(0.084, 0.325, 0.620, 1.0); // #15539e
  cr->set_line_width(1.5);
  std::vector<double> no_dash;
  cr->set_dash(no_dash, 0);
  cr->move_to(asx, asy);
  cr->line_to(bsx, bsy);
  cr->stroke();

  // Horizontal leg A→C and vertical leg C→B  (green dashed)
  std::vector<double> dash = {6.0, 3.0};
  cr->set_source_rgba(0.133, 0.773, 0.369, 1.0); // #22C55E
  cr->set_line_width(1.0);
  cr->set_dash(dash, 0);
  cr->move_to(asx, asy);
  cr->line_to(csx, csy); // horizontal
  cr->stroke();
  cr->move_to(csx, csy);
  cr->line_to(bsx, bsy); // vertical
  cr->stroke();
  cr->set_dash(no_dash, 0);

  // Right-angle box at C
  double box = 8.0;
  double bx_dir = (bsx >= asx) ? 1.0 : -1.0;
  double by_dir = (bsy >= asy) ? 1.0 : -1.0;
  double hpx = csx - bx_dir * box, hpy = csy;
  double vpx = csx, vpy = csy + by_dir * box;
  double ipx = csx - bx_dir * box, ipy = csy + by_dir * box;
  cr->set_source_rgba(0.133, 0.773, 0.369, 0.85);
  cr->set_line_width(1.0);
  cr->move_to(hpx, hpy);
  cr->line_to(ipx, ipy);
  cr->line_to(vpx, vpy);
  cr->stroke();

  // Endpoint dots
  cr->set_source_rgba(0.084, 0.325, 0.620, 1.0);
  cr->arc(asx, asy, 4.0, 0, 2 * M_PI);
  cr->fill();
  cr->arc(bsx, bsy, 4.0, 0, 2 * M_PI);
  cr->fill();

  cr->restore();

  // ── Draw labels with pill backgrounds ───────────────────────────────────
  auto draw_label = [&](const std::string &text, double tx, double ty,
                        double r_col, double g_col, double b_col) {
    cr->save();
    cr->select_font_face("Sans", Cairo::ToyFontFace::Slant::NORMAL,
                         Cairo::ToyFontFace::Weight::NORMAL);
    cr->set_font_size(11.0);
    Cairo::TextExtents te;
    cr->get_text_extents(text, te);
    double pw = te.width + 8.0;
    double ph = te.height + 6.0;
    double px = tx - pw * 0.5;
    double py = ty - ph * 0.5;

    cr->set_source_rgba(0.10, 0.10, 0.10, 0.82);
    double r = 4.0;
    cr->move_to(px + r, py);
    cr->line_to(px + pw - r, py);
    cr->arc(px + pw - r, py + r, r, -M_PI * 0.5, 0.0);
    cr->line_to(px + pw, py + ph - r);
    cr->arc(px + pw - r, py + ph - r, r, 0.0, M_PI * 0.5);
    cr->line_to(px + r, py + ph);
    cr->arc(px + r, py + ph - r, r, M_PI * 0.5, M_PI);
    cr->line_to(px, py + r);
    cr->arc(px + r, py + r, r, M_PI, M_PI * 1.5);
    cr->close_path();
    cr->fill();

    cr->set_source_rgb(r_col, g_col, b_col);
    cr->move_to(tx - te.width * 0.5 - te.x_bearing,
                ty - te.height * 0.5 - te.y_bearing);
    cr->show_text(text);
    cr->restore();

    if (push_labels) {
      RulerLabel lbl;
      lbl.copy_value = structured;
      lbl.display_text = text;
      lbl.sx = px;
      lbl.sy = py;
      lbl.sw = pw;
      lbl.sh = ph;
      m_ruler_labels.push_back(lbl);
    }
  };

  // Mid-points for label placement
  double hmid_x = (asx + csx) * 0.5;
  double vmid_y = (csy + bsy) * 0.5;
  double hyp_mx = (asx + bsx) * 0.5;
  double hyp_my = (asy + bsy) * 0.5;

  double by_dir2 = (bsy >= asy) ? 1.0 : -1.0;
  double bx_dir2 = (bsx >= asx) ? 1.0 : -1.0;

  // Hypotenuse — distance label
  {
    double dx_n = bsx - asx, dy_n = bsy - asy;
    double len = std::hypot(dx_n, dy_n);
    if (len > 0.01) {
      dx_n /= len;
      dy_n /= len;
    }
    double perp_x = -dy_n, perp_y = dx_n;
    double lx = hyp_mx + perp_x * 16.0;
    double ly = hyp_my + perp_y * 16.0;
    draw_label(fmt_lbl(dist), lx, ly, 0.4, 0.75, 1.0);
  }

  // Horizontal leg — Δx
  if (ddx > 0.5) {
    double lx = hmid_x;
    double ly = asy + by_dir2 * 16.0;
    draw_label(fmt_lbl(ddx), lx, ly, 0.133, 0.773, 0.369);
  }

  // Vertical leg — Δy
  if (ddy > 0.5) {
    double lx = csx - bx_dir2 * 26.0;
    double ly = vmid_y;
    draw_label(fmt_lbl(ddy), lx, ly, 0.133, 0.773, 0.369);
  }

  // Angle α at A
  if (dist > 0.5) {
    double lx = asx + bx_dir2 * 22.0;
    double ly = asy + by_dir2 * 18.0;
    draw_label(fmt_deg_lbl(alpha), lx, ly, 1.0, 0.85, 0.3);
  }

  // Angle β at B
  if (dist > 0.5) {
    double lx = bsx - bx_dir2 * 22.0;
    double ly = bsy - by_dir2 * 18.0;
    draw_label(fmt_deg_lbl(beta), lx, ly, 1.0, 0.85, 0.3);
  }

  // Coord tags at endpoints — small "(x, y)" pills, white-ish
  {
    auto fmt_xy = [&](double x, double y) -> std::string {
      return std::string("(") + fmt_val(x) + ", " + fmt_val(y) + ")";
    };
    // A endpoint — placed just outside the right-angle direction so it
    // doesn't overlap leg labels.
    double ax_lx = asx - bx_dir2 * 28.0;
    double ax_ly = asy - by_dir2 * 14.0;
    draw_label(fmt_xy(ax_user, ay_user), ax_lx, ax_ly, 0.85, 0.85, 0.95);
    double bx_lx = bsx + bx_dir2 * 28.0;
    double bx_ly = bsy + by_dir2 * 14.0;
    draw_label(fmt_xy(bx_user, by_user), bx_lx, bx_ly, 0.85, 0.85, 0.95);
  }
}

// ── draw_ruler_overlay
// ──────────────────────────────────────────────────────── Draws the
// measurement triangle overlay in screen space. Called from on_draw after all
// content.
void Canvas::draw_ruler_overlay(const Cairo::RefPtr<Cairo::Context> &cr,
                                int /*w*/, int /*h*/) {
  if (!m_doc)
    return;

  m_ruler_labels.clear();

  // ── Pick-mode: draw all objects as outlines + all nodes as hollow squares ──
  // This gives the user a full node map to click from, regardless of which
  // tool was active before. Drawn in doc space then restored.
  {
    cr->save();
    const double ox = doc_origin_x();
    const double oy = doc_origin_y();
    cr->translate(ox, oy);
    cr->scale(m_zoom, m_zoom);

    std::vector<double> no_dash;
    cr->set_dash(no_dash, 0);

    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->is_special_layer())
        continue;

      for (auto &obj_uptr : layer->children) {
        const SceneNode &obj = *obj_uptr;
        if (obj.type != SceneNode::Type::Path || !obj.path)
          continue;

        // Draw path as thin outline
        BezierPath bp = BezierPath::from_path_data(*obj.path);
        bp.apply_to_cairo(cr);
        cr->set_source_rgba(0.6, 0.6, 0.6, 0.45);
        cr->set_line_width(1.0 / m_zoom);
        cr->stroke();

        // Draw each node as a hollow square
        double ns = 3.5 / m_zoom; // half-size in doc units
        for (int ni = 0; ni < (int)obj.path->nodes.size(); ++ni) {
          const BezierNode &nd = obj.path->nodes[ni];
          bool is_a = (&obj == m_ruler_node_a_obj && ni == m_ruler_node_a_idx);
          bool is_b = (&obj == m_ruler_node_b_obj && ni == m_ruler_node_b_idx);
          if (is_a || is_b) {
            // Selected nodes — filled blue
            cr->set_source_rgba(0.084, 0.325, 0.620, 1.0);
            cr->rectangle(nd.x - ns, nd.y - ns, ns * 2, ns * 2);
            cr->fill();
          } else {
            // Unselected nodes — hollow white square
            cr->set_source_rgba(0.85, 0.85, 0.85, 0.9);
            cr->rectangle(nd.x - ns, nd.y - ns, ns * 2, ns * 2);
            cr->fill();
            cr->set_source_rgba(0.4, 0.4, 0.4, 0.9);
            cr->set_line_width(0.75 / m_zoom);
            cr->rectangle(nd.x - ns, nd.y - ns, ns * 2, ns * 2);
            cr->stroke();
          }
        }
      }
    }

    // ── S89: refpt pick-map pass ───────────────────────────────────────
    // Refpts are pickable endpoints in ruler mode. Drawn as small filled
    // circles, magenta-tinted to match the ref layer's default colour so
    // the user can tell them apart from the node hollow-squares above.
    // Selected refpts (the live A or B) get the same blue fill as
    // selected nodes for visual consistency.
    {
      double rs = 4.0 / m_zoom; // radius in doc units
      for (auto &layer : m_doc->layers) {
        if (!layer->visible || layer->locked || !layer->is_ref_layer())
          continue;
        for (auto &child_uptr : layer->children) {
          SceneNode &child = *child_uptr;
          if (!child.is_ref() || child.locked)
            continue;
          bool is_a = (&child == m_ruler_node_a_obj);
          bool is_b = (&child == m_ruler_node_b_obj);
          if (is_a || is_b) {
            cr->set_source_rgba(0.084, 0.325, 0.620, 1.0);
            cr->arc(child.ref_x, child.ref_y, rs, 0, 2 * M_PI);
            cr->fill();
          } else {
            // Filled magenta dot with a subtle outline.
            cr->set_source_rgba(0.85, 0.10, 0.75, 0.9);
            cr->arc(child.ref_x, child.ref_y, rs, 0, 2 * M_PI);
            cr->fill();
            cr->set_source_rgba(0.4, 0.4, 0.4, 0.9);
            cr->set_line_width(0.75 / m_zoom);
            cr->arc(child.ref_x, child.ref_y, rs, 0, 2 * M_PI);
            cr->stroke();
          }
        }
      }
    }

    cr->restore();
  }

  // Need at least one endpoint picked to draw the triangle
  if (!m_ruler_node_a_obj)
    return;

  // S89: endpoint position resolves both Node and Ref kinds.
  double na_x, na_y;
  if (!ruler_endpoint_pos(m_ruler_node_a_obj, m_ruler_node_a_idx, na_x, na_y))
    return;

  // If only A is picked, the filled blue dot drawn during the pick-map
  // pass is enough — no triangle yet.
  if (!m_ruler_node_b_obj)
    return;

  double nb_x, nb_y;
  if (!ruler_endpoint_pos(m_ruler_node_b_obj, m_ruler_node_b_idx, nb_x, nb_y))
    return;

  // Convert to user-space (Y-up) and delegate to the shared annotation
  // renderer. push_labels=true so click-to-copy works on the live triangle.
  double ax_user = na_x;
  double ay_user = m_doc->canvas_height() - na_y;
  double bx_user = nb_x;
  double by_user = m_doc->canvas_height() - nb_y;
  draw_measurement_annotations(cr, ax_user, ay_user, bx_user, by_user,
                               /*push_labels=*/true);

  // ── Toast overlay ─────────────────────────────────────────────────────
  if (m_ruler_toast_ms > 0 && !m_ruler_toast_text.empty()) {
    double alpha_t = std::min(1.0, m_ruler_toast_ms / 400.0);
    cr->save();
    cr->select_font_face("Sans", Cairo::ToyFontFace::Slant::NORMAL,
                         Cairo::ToyFontFace::Weight::NORMAL);
    cr->set_font_size(12.0);
    Cairo::TextExtents te;
    cr->get_text_extents(m_ruler_toast_text, te);
    double pw = te.width + 14.0, ph = te.height + 10.0;
    double px = m_ruler_toast_x - pw * 0.5;
    double py = m_ruler_toast_y - ph;
    cr->set_source_rgba(0.08, 0.77, 0.33, 0.92 * alpha_t);
    cr->rectangle(px, py, pw, ph);
    cr->fill();
    cr->set_source_rgba(1.0, 1.0, 1.0, alpha_t);
    cr->move_to(px + 7.0, py + ph - 5.0);
    cr->show_text(m_ruler_toast_text);
    cr->restore();
  }
}

// ── Text-on-Path Tool
// ─────────────────────────────────────────────────────────

// Build cumulative arc-length table. arc_table[i] = total length up to
// the start of segment i. Returns total path length.
double Canvas::build_arc_table(const BezierPath &bp,
                               std::vector<double> &arc_table) const {
  int n = bp.segment_count();
  arc_table.clear();
  arc_table.reserve(n + 1);
  double total = 0.0;
  arc_table.push_back(0.0);
  for (int i = 0; i < n; ++i) {
    double len = bp.segment(i).length(32);
    total += len;
    arc_table.push_back(total);
  }
  LOG_DEBUG("build_arc_table: {} segs total_len={:.1f}", n, total);
  return total;
}

// Given arc offset (clamped/wrapped), return position + tangent angle on path.
bool Canvas::path_point_at(const BezierPath &bp,
                           const std::vector<double> &arc_table,
                           double total_len, double arc_offset, Vec2 &pos,
                           double &angle) const {
  if (total_len < 0.001 || arc_table.empty())
    return false;
  // Clamp to path length
  arc_offset = std::max(0.0, std::min(arc_offset, total_len));

  int n = bp.segment_count();
  for (int i = 0; i < n; ++i) {
    double seg_start = arc_table[i];
    double seg_end = arc_table[i + 1];
    double seg_len = seg_end - seg_start;
    if (arc_offset <= seg_end || i == n - 1) {
      double local =
          (seg_len > 0.001) ? (arc_offset - seg_start) / seg_len : 0.0;
      local = std::max(0.0, std::min(1.0, local));
      CubicSegment seg = bp.segment(i);
      Vec2 pt = seg.at(local);
      Vec2 tan = seg.tangent(local);
      pos = pt;
      angle = std::atan2(tan.y, tan.x);
      return true;
    }
  }
  return false;
}

// Find a path SceneNode in the document by its id string.
SceneNode *Canvas::top_find_path_by_id(const std::string &id) const {
  if (!m_doc || id.empty())
    return nullptr;
  // Search by internal_id (stable UUID) — not SVG id which collides across docs
  std::function<SceneNode *(SceneNode *)> find =
      [&](SceneNode *n) -> SceneNode * {
    if (n->is_path() && n->internal_id == id)
      return n;
    for (auto &ch : n->children) {
      if (auto *r = find(ch.get()))
        return r;
    }
    return nullptr;
  };
  for (auto &layer : m_doc->layers) {
    if (auto *r = find(layer.get())) {
      if (r->path && !r->path->nodes.empty()) {
        const auto &n0 = r->path->nodes.front();
        const auto &n1 = r->path->nodes.back();
        LOG_DEBUG("top_find_path_by_id: found iid='{}' svg_id='{}' nodes={} "
                  "first=({:.1f},{:.1f}) last=({:.1f},{:.1f})",
                  id, r->id, r->path->nodes.size(), n0.x, n0.y, n1.x, n1.y);
      }
      return r;
    }
  }
  // Dump all path internal_ids to diagnose string mismatch
  LOG_DEBUG("top_find_path_by_id: NOT FOUND len={} — searching iid='{}'",
            id.size(), id);
  std::function<void(SceneNode *)> dump_ids = [&](SceneNode *n) {
    if (n->is_path()) {
      LOG_DEBUG("  path svg_id='{}' iid='{}' iid_len={} match={}", n->id,
                n->internal_id, n->internal_id.size(), (n->internal_id == id));
    }
    for (auto &ch : n->children)
      dump_ids(ch.get());
  };
  for (auto &layer : m_doc->layers)
    dump_ids(layer.get());
  return nullptr;
}

void Canvas::on_top_begin(double x, double y) {
  if (!m_doc)
    return;
  double dx, dy;
  screen_to_doc(x, y, dx, dy);
  double tol = 10.0 / m_zoom;
  LOG_DEBUG("on_top_begin: phase={} screen=({:.1f},{:.1f}) doc=({:.1f},{:.1f})",
            m_top_phase, x, y, dx, dy);

  // Phase 2: check if clicking the offset drag handle OR anywhere on the guide
  // path
  if (m_top_phase == 2 && m_top_text && !m_top_text->text_path_id.empty()) {
    SceneNode *guide = top_find_path_by_id(m_top_text->text_path_id);
    if (guide && guide->path) {
      BezierPath bp = BezierPath::from_path_data(*guide->path);
      std::vector<double> arc_table;
      double total = build_arc_table(bp, arc_table);

      // Hit-test the I-beam handle (tight tolerance).
      // flip=true: I-beam is at the mirrored arc position.
      double ibeam_arc = m_top_text->text_path_flip
                             ? total - m_top_text->text_path_offset
                             : m_top_text->text_path_offset;
      Vec2 pos;
      double angle;
      if (path_point_at(bp, arc_table, total, ibeam_arc, pos, angle)) {
        double hsx, hsy;
        doc_to_screen(pos.x, pos.y, hsx, hsy);
        double dist = std::hypot(x - hsx, y - hsy);
        LOG_DEBUG("on_top_begin phase2: ibeam screen=({:.1f},{:.1f}) "
                  "dist={:.1f} tol={:.1f}",
                  hsx, hsy, dist, tol + 6.0);
        if (dist < tol + 6.0) {
          LOG_DEBUG("on_top_begin phase2: HIT ibeam — starting drag");
          m_top_dragging = true;
          m_top_drag_start_off = m_top_text->text_path_offset;
          m_top_drag_start_x = x;
          m_top_drag_start_y = y;
          return;
        }
      }
      // Also hit-test the full path stroke
      HitResult hr = bp.hit_test({dx, dy}, m_zoom, 10.0);
      LOG_DEBUG("on_top_begin phase2: path hit_test kind={}", (int)hr.kind);
      if (hr.kind != HitResult::Kind::None) {
        LOG_DEBUG("on_top_begin phase2: HIT path stroke — starting drag");
        m_top_dragging = true;
        m_top_drag_start_off = m_top_text->text_path_offset;
        m_top_drag_start_x = x;
        m_top_drag_start_y = y;
        return;
      }
    }
    // Phase 2 miss: click hit neither I-beam nor guide path stroke.
    // Stay in phase 2 — a miss is a no-op, not a reason to lose the selection.
    return;
  }

  // Right-click is handled by on_top_rclick — skip here
  // Phase 0: pick a text node
  if (m_top_phase == 0) {
    // Hit-test text nodes
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        SceneNode *obj = obj_uptr.get();
        if (!obj->is_text())
          continue;

        bool hit = false;
        if (!obj->text_path_id.empty()) {
          // Linked text: hit-test the guide path stroke so the user
          // can click anywhere along the path to re-select the link.
          SceneNode *guide = top_find_path_by_id(obj->text_path_id);
          if (guide && guide->path) {
            BezierPath bp = BezierPath::from_path_data(*guide->path);
            HitResult hr = bp.hit_test({dx, dy}, m_zoom, 10.0);
            hit = (hr.kind != HitResult::Kind::None);
          }
        } else {
          // Unlinked text: use approx bounding box
          double approx_w =
              obj->text_content.size() * obj->text_font_size * 0.6;
          double approx_h = obj->text_font_size * 1.4;
          double tx = obj->text_x;
          double ty = obj->text_y;
          if (obj->text_anchor == "middle")
            tx -= approx_w * 0.5;
          if (obj->text_anchor == "end")
            tx -= approx_w;
          hit = (dx >= tx - 4 && dx <= tx + approx_w + 4 &&
                 dy >= ty - approx_h && dy <= ty + 4);
        }

        if (hit) {
          m_top_text = obj;
          // If already linked, jump straight to phase 2
          if (!obj->text_path_id.empty()) {
            m_top_path_node = top_find_path_by_id(obj->text_path_id);
            m_top_phase = 2;
          } else {
            m_top_phase = 1;
            m_top_path_node = nullptr;
          }
          m_selected = obj;
          m_sig_selection.emit(m_selected);
          queue_draw();
          return;
        }
      }
    }
    // Missed — reset
    m_top_text = nullptr;
    m_top_phase = 0;
    queue_draw();
    return;
  }

  // Phase 0: also check if user clicked a bare path (no pre-selected text)
  // → create a new text node linked to that path and enter edit mode.
  if (m_top_phase == 0) {
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        SceneNode *obj = obj_uptr.get();
        if (!obj->is_path() || !obj->path)
          continue;
        BezierPath bp = BezierPath::from_path_data(*obj->path);
        HitResult hr = bp.hit_test({dx, dy}, m_zoom, 8.0);
        if (hr.kind != HitResult::Kind::None) {
          // Ensure path has stable internal_id
          if (obj->internal_id.empty())
            obj->internal_id = generate_internal_id();
          // Find path start in doc space to position text anchor
          Vec2 path_start{obj->path->nodes.front().x,
                          obj->path->nodes.front().y};
          // Create new text node
          auto tn = std::make_unique<SceneNode>();
          tn->type = SceneNode::Type::Text;
          tn->internal_id = generate_internal_id();
          tn->id =
              "text_" + std::to_string(reinterpret_cast<uintptr_t>(tn.get()));
          tn->name = m_doc->next_default_name(CurvzDocument::NameKind::Text);
          tn->text_content = "";
          tn->text_x = path_start.x;
          tn->text_y = path_start.y;
          tn->text_font_family = "Sans";
          tn->text_font_size = 24.0;
          style::mutate_appearance(*tn, [](SceneNode& n) {
            n.fill.type = FillStyle::Type::CurrentColor;
            n.stroke.paint.type = FillStyle::Type::None;
          });
          tn->text_path_id = obj->internal_id;
          tn->text_path_offset = 0.0;
          tn->text_path_flip = false;
          SceneNode *tn_ptr = tn.get();
          // Place in active layer
          SceneNode *al = m_doc->active_layer();
          if (al)
            al->children.push_back(std::move(tn));
          // Enter text editing mode
          m_top_text = tn_ptr;
          m_top_path_node = obj;
          m_top_phase = 2;
          // Enter text editing mode (same as Text tool activation)
          m_text_editing = tn_ptr;
          m_text_is_new = true;
          if (m_text_entry) {
            m_text_entry_conn_activate.disconnect();
            m_text_entry_conn_changed.disconnect();
            m_text_entry->set_text("");
            m_text_entry->set_visible(true);
            m_text_entry->grab_focus();
            position_text_entry();
            m_text_entry_conn_changed =
                m_text_entry->signal_changed().connect([this]() {
                  if (m_text_editing)
                    m_text_editing->text_content = m_text_entry->get_text();
                  queue_draw();
                });
            m_text_entry_conn_activate =
                m_text_entry->signal_activate().connect([this, tn_ptr, obj]() {
                  // Save TOP state before commit_text_edit, which
                  // emits request_tool(Selection) internally and
                  // will cause set_active_tool(TextOnPath) to reset.
                  SceneNode *saved_text = tn_ptr;
                  SceneNode *saved_path_node = obj;
                  commit_text_edit();
                  // Return to TextOnPath tool, then immediately
                  // restore phase 2 so inspector sees the text node.
                  set_active_tool(ActiveTool::TextOnPath);
                  m_top_text = saved_text;
                  m_top_path_node = saved_path_node;
                  m_top_phase = 2;
                  m_selected = saved_text;
                  m_sig_selection.emit(m_selected);
                  queue_draw();
                });
          }
          queue_draw();
          return;
        }
      }
    }
  }

  // Phase 1: pick a path node to link
  if (m_top_phase == 1 && m_top_text) {
    // Hit-test path objects
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        SceneNode *obj = obj_uptr.get();
        if (!obj->is_path() || !obj->path)
          continue;
        if (obj == m_top_text)
          continue;
        BezierPath bp = BezierPath::from_path_data(*obj->path);
        HitResult hr = bp.hit_test({dx, dy}, m_zoom, 12.0);
        if (hr.kind != HitResult::Kind::None) {
          // Check multi-line warning
          const std::string &tc = m_top_text->text_content;
          if (tc.find('\n') != std::string::npos) {
            if (auto *win = dynamic_cast<Gtk::Window *>(get_root()))
              curvz::utils::show_alert(
                  *win, "Text on path supports single-line text only.",
                  "Only the first line of text will be used. "
                  "Please edit the text to a single line.");
          }
          // Ensure path has stable internal_id
          if (obj->internal_id.empty())
            obj->internal_id = generate_internal_id();
          // Push undo command before mutating
          if (m_history) {
            m_history->push(std::make_unique<LinkTextToPathCommand>(
                m_top_text,
                m_top_text->text_path_id, // before (empty or old)
                m_top_text->text_path_offset, m_top_text->text_path_flip,
                m_top_text->text_x, m_top_text->text_y, // before x/y
                obj->internal_id,                       // after
                0.0, false, m_top_text->text_x,
                m_top_text->text_y)); // after x/y (unchanged)
          }
          m_top_text->text_path_id = obj->internal_id;
          m_top_text->text_path_offset = 0.0;
          m_top_text->text_path_flip = false;
          m_top_path_node = obj;
          m_top_phase = 2;
          // Keep text node selected so inspector shows text panel
          m_selected = m_top_text;
          m_sig_selection.emit(m_selected);
          // Persist the link immediately — ensures data-curvz-iid is
          // written to SVG before next load, so text_path_id survives
          // restarts without UUID re-assignment breaking the link.
          m_sig_doc_changed.emit();
          queue_draw();
          return;
        }
      }
    }
  }
}

void Canvas::on_top_motion(double x, double y) {
  if (!m_top_dragging || !m_top_text) {
    queue_draw();
    return;
  }

  // x, y are already doc-space coordinates (m_draw_cur_dx/dy passed by caller).
  SceneNode *guide = top_find_path_by_id(m_top_text->text_path_id);
  if (guide && guide->path) {
    BezierPath bp = BezierPath::from_path_data(*guide->path);
    std::vector<double> arc_table;
    double total = build_arc_table(bp, arc_table);

    // Sample path at fine intervals to find nearest arc position
    const int samples = 256;
    double best_dist2 = 1e18;
    double best_arc = m_top_text->text_path_offset;
    for (int i = 0; i <= samples; ++i) {
      double arc = total * i / (double)samples;
      Vec2 pt;
      double ang;
      if (path_point_at(bp, arc_table, total, arc, pt, ang)) {
        double d2 = (pt.x - x) * (pt.x - x) + (pt.y - y) * (pt.y - y);
        if (d2 < best_dist2) {
          best_dist2 = d2;
          best_arc = arc;
        }
      }
    }
    m_top_text->text_path_offset = std::max(
        0.0, std::min(m_top_text->text_path_flip ? total - best_arc : best_arc,
                      total));
  } else {
    // Fallback: horizontal drag delta
    double ddx = x - m_top_drag_start_x;
    m_top_text->text_path_offset = std::max(0.0, m_top_drag_start_off + ddx);
  }
  queue_draw();
}

void Canvas::on_top_end(double /*x*/, double /*y*/) {
  if (m_top_dragging)
    m_sig_doc_changed.emit(); // persist the new offset
  m_top_dragging = false;
}

void Canvas::on_top_rclick(double x, double y) {
  if (!m_doc)
    return;
  LOG_DEBUG("on_top_rclick: top_text={} top_phase={}", (void *)m_top_text,
            m_top_phase);

  // If we have a currently selected text node, release it via the proper
  // pathway so text_x/text_y is repositioned and undo works correctly.
  if (m_top_text && !m_top_text->text_path_id.empty()) {
    set_selection_single(m_top_text);
    release_text_from_path();
    return;
  }

  // Fallback: hit-test any text node with text_path_id
  double dx, dy;
  screen_to_doc(x, y, dx, dy);
  for (auto &layer : m_doc->layers) {
    if (!layer->visible)
      continue;
    for (auto &obj_uptr : layer->children) {
      SceneNode *obj = obj_uptr.get();
      if (!obj->is_text() || obj->text_path_id.empty())
        continue;
      SceneNode *guide = top_find_path_by_id(obj->text_path_id);
      if (guide && guide->path) {
        BezierPath bp = BezierPath::from_path_data(*guide->path);
        HitResult hr = bp.hit_test({dx, dy}, m_zoom, 20.0);
        if (hr.kind != HitResult::Kind::None) {
          set_selection_single(obj);
          release_text_from_path();
          return;
        }
      }
    }
  }
}

// Computes the doc-space position where a linked text node's anchor sits on
// its guide path.  Used when detaching so text_x/text_y land where the text
// was visually, making the node immediately re-selectable.
// Returns false if the guide path can't be found.
bool Canvas::top_compute_detach_position(const SceneNode &tn, double &out_x,
                                         double &out_y) const {
  if (tn.text_path_id.empty())
    return false;
  SceneNode *guide = top_find_path_by_id(tn.text_path_id);
  if (!guide || !guide->path)
    return false;
  BezierPath bp = BezierPath::from_path_data(*guide->path);
  std::vector<double> arc_table;
  double total = build_arc_table(bp, arc_table);
  double arc =
      tn.text_path_flip ? total - tn.text_path_offset : tn.text_path_offset;
  arc = std::max(0.0, std::min(arc, total));
  Vec2 pos;
  double angle;
  if (!path_point_at(bp, arc_table, total, arc, pos, angle))
    return false;
  out_x = pos.x;
  // Convert doc Y-down back to text_y (which is Y-down baseline)
  out_y = pos.y;
  return true;
}

// Works from any tool — scans m_selection for text nodes with text_path_id set,
// or path nodes whose partner text node is implicitly in the pair.
// Each detach is pushed as an undoable LinkTextToPathCommand
// (after_path_id="").
void Canvas::release_text_from_path() {
  if (!m_doc)
    return;

  // Collect all text nodes to release — from m_selection directly, or via
  // partner lookup if the user selected the guide path side of the pair.
  std::vector<SceneNode *> to_release;
  auto add_if_linked = [&](SceneNode *n) {
    if (!n || n->text_path_id.empty())
      return;
    if (std::find(to_release.begin(), to_release.end(), n) == to_release.end())
      to_release.push_back(n);
  };
  for (SceneNode *obj : m_selection) {
    if (obj->is_text())
      add_if_linked(obj);
    else if (obj->is_path()) {
      SceneNode *partner = top_pair_partner(obj);
      if (partner && partner->is_text())
        add_if_linked(partner);
    }
  }
  // Also check m_top_text for when we're in the TOP tool
  if (m_top_text)
    add_if_linked(m_top_text);

  if (to_release.empty())
    return;

  for (SceneNode *tn : to_release) {
    // Compute where the text anchor sits on the path so the detached
    // node lands where it was visually, making it immediately re-selectable.
    double detach_x = tn->text_x, detach_y = tn->text_y;
    top_compute_detach_position(*tn, detach_x, detach_y);

    if (m_history) {
      m_history->push(std::make_unique<LinkTextToPathCommand>(
          tn, tn->text_path_id, tn->text_path_offset, tn->text_path_flip,
          tn->text_x, tn->text_y,               // before x/y
          "", 0.0, false, detach_x, detach_y)); // after x/y
    }
    tn->text_x = detach_x;
    tn->text_y = detach_y;
    tn->text_path_id = "";
    tn->text_path_offset = 0.0;
    tn->text_path_flip = false;
  }

  // Reset TOP tool state if active
  if (m_tool == ActiveTool::TextOnPath) {
    m_top_text = nullptr;
    m_top_path_node = nullptr;
    m_top_phase = 0;
    m_top_dragging = false;
  }

  m_sig_doc_changed.emit();
  m_sig_selection.emit(m_selected);
  queue_draw();
  LOG_DEBUG("release_text_from_path: released {} text node(s)",
            to_release.size());
}

// node in the document.  Used to suppress fill/stroke on guide paths in
// normal render mode (they should be invisible outside outline mode).
bool Canvas::is_top_guide_path(const SceneNode &node) const {
  if (!m_doc || node.internal_id.empty())
    return false;
  for (const auto &layer : m_doc->layers) {
    for (const auto &child : layer->children) {
      if (child->is_text() && child->text_path_id == node.internal_id)
        return true;
    }
  }
  return false;
}

// Returns the partner of a PTT pair:
//   - If node is a linked text node  → returns its guide path SceneNode
//   - If node is a guide path        → returns the text node that links to it
//   - Otherwise                      → returns nullptr
SceneNode *Canvas::top_pair_partner(SceneNode *node) const {
  if (!m_doc || !node)
    return nullptr;
  // Text node case: look up its guide path
  if (node->is_text() && !node->text_path_id.empty())
    return top_find_path_by_id(node->text_path_id);
  // Path node case: scan for a text node referencing it
  if (node->is_path() && !node->internal_id.empty()) {
    for (const auto &layer : m_doc->layers) {
      for (const auto &child : layer->children) {
        if (child->is_text() && child->text_path_id == node->internal_id)
          return child.get();
      }
    }
  }
  return nullptr;
}

void Canvas::draw_top_overlay(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_doc)
    return;

  const double ox = doc_origin_x();
  const double oy = doc_origin_y();

  // Highlight selected text node (phase 1)
  if (m_top_text && m_top_phase >= 1) {
    double tx, ty;
    double doc_ty = m_doc->canvas_height() - m_top_text->text_y;
    doc_to_screen(m_top_text->text_x, doc_ty, tx, ty);
    cr->save();
    cr->set_source_rgba(0.3, 0.6, 1.0, 0.6);
    cr->set_line_width(1.5);
    double approx_w = m_top_text->text_content.size() *
                      m_top_text->text_font_size * 0.6 * m_zoom;
    double approx_h = m_top_text->text_font_size * m_zoom;
    cr->rectangle(tx - 2, ty - approx_h - 2, approx_w + 4, approx_h + 4);
    cr->stroke();
    cr->restore();
  }

  // Phase 2: draw offset drag handle at text_path_offset position
  if (m_top_phase == 2 && m_top_text && !m_top_text->text_path_id.empty()) {
    SceneNode *guide = top_find_path_by_id(m_top_text->text_path_id);
    if (guide && guide->path) {
      BezierPath bp = BezierPath::from_path_data(*guide->path);
      std::vector<double> arc_table;
      double total = build_arc_table(bp, arc_table);

      // Always draw guide path as dashed green line (not just outline mode)
      cr->save();
      cr->translate(ox, oy);
      cr->scale(m_zoom, m_zoom);
      bp.apply_to_cairo(cr);
      std::vector<double> dash = {6.0 / m_zoom, 3.0 / m_zoom};
      cr->set_dash(dash, 0);
      cr->set_source_rgba(0.133, 0.773, 0.369, 0.6);
      cr->set_line_width(1.0 / m_zoom);
      cr->stroke();
      cr->restore();

      // Draw I-beam cursor at offset position (perpendicular to path tangent).
      // flip=true: mirror to show where text actually starts on the reversed
      // path.
      double ibeam_arc = m_top_text->text_path_flip
                             ? total - m_top_text->text_path_offset
                             : m_top_text->text_path_offset;
      Vec2 pos;
      double angle;
      if (path_point_at(bp, arc_table, total, ibeam_arc, pos, angle)) {
        double hsx, hsy;
        doc_to_screen(pos.x, pos.y, hsx, hsy);

        // I-beam geometry: vertical stroke + top/bottom serifs
        // All drawn perpendicular to the path tangent
        double perp = angle + M_PI * 0.5;
        double px = std::cos(perp); // perpendicular unit vector
        double py = std::sin(perp);
        double tx = std::cos(angle); // tangent unit vector
        double ty = std::sin(angle);

        const double stem = 12.0; // half-height of vertical stroke
        const double serif = 5.0; // half-width of top/bottom serifs
        const double stroke_w = 1.5;

        // Top and bottom serif endpoints
        double top_x = hsx + px * stem;
        double top_y = hsy + py * stem;
        double bot_x = hsx - px * stem;
        double bot_y = hsy - py * stem;

        cr->save();
        // Shadow pass for contrast
        cr->set_source_rgba(0.0, 0.0, 0.0, 0.5);
        cr->set_line_width(stroke_w + 2.0);
        cr->set_line_cap(Cairo::Context::LineCap::ROUND);
        // Vertical stem
        cr->move_to(top_x, top_y);
        cr->line_to(bot_x, bot_y);
        cr->stroke();
        // Top serif
        cr->move_to(top_x - tx * serif, top_y - ty * serif);
        cr->line_to(top_x + tx * serif, top_y + ty * serif);
        cr->stroke();
        // Bottom serif
        cr->move_to(bot_x - tx * serif, bot_y - ty * serif);
        cr->line_to(bot_x + tx * serif, bot_y + ty * serif);
        cr->stroke();

        // Green foreground pass
        cr->set_source_rgba(0.133, 0.773, 0.369, 1.0);
        cr->set_line_width(stroke_w);
        cr->move_to(top_x, top_y);
        cr->line_to(bot_x, bot_y);
        cr->stroke();
        cr->move_to(top_x - tx * serif, top_y - ty * serif);
        cr->line_to(top_x + tx * serif, top_y + ty * serif);
        cr->stroke();
        cr->move_to(bot_x - tx * serif, bot_y - ty * serif);
        cr->line_to(bot_x + tx * serif, bot_y + ty * serif);
        cr->stroke();
        cr->restore();
      }
    }
  }
}

// ── Guide rendering
// ─────────────────────────────────────────────────────────── ──
// draw_guides_doc_space ─────────────────────────────────────────────────────
// Draws guide lines for the given guide layer in doc space.
// Called from draw_objects where cr has translate(ox,oy) active but NO scale.
// Guide lines extend across the full canvas in doc units so they clip naturally
// to the artboard when the artboard clip rect is active.
// This replaces the old identity-matrix approach so guides respect z-order.
void Canvas::draw_guides_doc_space(const Cairo::RefPtr<Cairo::Context> &cr,
                                   const SceneNode *gl) {
  if (!gl || !gl->visible)
    return;
  if (!m_doc)
    return;

  const double cw = m_doc->canvas_width();  // doc units
  const double ch = m_doc->canvas_height(); // doc units

  std::vector<double> dash = {6.0, 4.0};
  std::vector<double> no_dash = {};
  cr->save();
  cr->set_line_width(1.0);

  for (const auto &child : gl->children) {
    if (!child->is_guide())
      continue;
    bool hovered = (child.get() == m_guide_hovered);
    bool selected =
        std::find(m_guide_selection.begin(), m_guide_selection.end(),
                  child.get()) != m_guide_selection.end();
    double r = m_doc->guide_color_r;
    double g = m_doc->guide_color_g;
    double b = m_doc->guide_color_b;

    if (selected) {
      cr->set_dash(no_dash, 0);
      cr->set_source_rgba(r, g, b, 1.0);
    } else {
      cr->set_dash(dash, 0);
      cr->set_source_rgba(r, g, b, hovered ? 1.0 : 0.7);
    }

    // Render from the (guide_x, guide_y, guide_angle) triplet.  angle is
    // in degrees, doc-Y-down space (0°=H, 90°=V, +=CW visually).  We draw
    // an infinite line through the anchor at that angle, extended well
    // past the artboard on both sides; the artboard clip in draw_objects
    // trims it to the visible region.
    const double ax = child->guide_x * m_zoom;
    const double ay = child->guide_y * m_zoom;
    const double a_rad = child->guide_angle * M_PI / 180.0;
    const double dxu = std::cos(a_rad);
    const double dyu = std::sin(a_rad);
    // Choose an extent larger than any reasonable artboard diagonal so
    // the line spans the visible region after clipping.
    const double span = std::hypot(cw, ch) * m_zoom * 2.0 + 1000.0;
    const double x0 = ax - dxu * span;
    const double y0 = ay - dyu * span;
    const double x1 = ax + dxu * span;
    const double y1 = ay + dyu * span;
    cr->move_to(x0, y0);
    cr->line_to(x1, y1);
    cr->stroke();
  }
  cr->restore();
}

void Canvas::draw_guides(const Cairo::RefPtr<Cairo::Context> &cr, int w,
                         int h) {
  if (!m_doc)
    return;
  const SceneNode *gl = m_doc->guide_layer();
  if (!gl || !gl->visible)
    return;

  const double ox = doc_origin_x();
  const double oy = doc_origin_y();

  std::vector<double> dash = {6.0, 4.0};
  std::vector<double> no_dash = {};
  cr->save();
  cr->set_line_width(1.0);

  for (const auto &child : gl->children) {
    if (!child->is_guide())
      continue;
    bool hovered = (child.get() == m_guide_hovered);
    bool selected =
        std::find(m_guide_selection.begin(), m_guide_selection.end(),
                  child.get()) != m_guide_selection.end();
    double r = m_doc->guide_color_r;
    double g = m_doc->guide_color_g;
    double b = m_doc->guide_color_b;

    if (selected) {
      cr->set_dash(no_dash, 0);
      cr->set_source_rgba(r, g, b, 1.0);
    } else {
      cr->set_dash(dash, 0);
      cr->set_source_rgba(r, g, b, hovered ? 1.0 : 0.7);
    }

    // See draw_guides_doc_space for the model rationale.  This legacy
    // entry point appears unused as of S49 but is kept consistent so it
    // doesn't render stale H/V-only geometry if anything calls it.
    const double ax = child->guide_x * m_zoom + ox;
    const double ay = child->guide_y * m_zoom + oy;
    const double a_rad = child->guide_angle * M_PI / 180.0;
    const double dxu = std::cos(a_rad);
    const double dyu = std::sin(a_rad);
    const double span = std::hypot((double)w, (double)h) * 2.0 + 1000.0;
    cr->move_to(ax - dxu * span, ay - dyu * span);
    cr->line_to(ax + dxu * span, ay + dyu * span);
    cr->stroke();
  }
  cr->restore();
}

// ── draw_grid_doc_space
// ──────────────────────────────────────────────────────────
// Draws a regular grid overlay in doc space. cr has translate(ox,oy) active
// but NO scale — lines are drawn in doc units, zoom is applied externally.
void Canvas::draw_grid_doc_space(const Cairo::RefPtr<Cairo::Context> &cr,
                                 const SceneNode *gl) {
  if (!gl || !gl->visible || !m_doc)
    return;

  const double cw = (double)m_doc->canvas_width();
  const double ch = (double)m_doc->canvas_height();
  const double sx = gl->grid_spacing_x;
  const double sy = gl->grid_spacing_y;
  if (sx < 0.5 || sy < 0.5)
    return;

  cr->save();
  cr->scale(m_zoom, m_zoom);
  cr->set_source_rgba(gl->grid_color_r, gl->grid_color_g, gl->grid_color_b,
                      gl->grid_color_a);
  cr->set_line_width(1.0 / m_zoom);

  const double ox = gl->grid_offset_x;
  const double oy = gl->grid_offset_y;

  if (gl->grid_dots) {
    // Dots at every grid intersection
    const double r = 1.5 / m_zoom;
    for (double x = std::fmod(ox, sx); x <= cw; x += sx) {
      for (double y = std::fmod(oy, sy); y <= ch; y += sy) {
        cr->arc(x, y, r, 0, 2 * M_PI);
        cr->fill();
      }
    }
  } else {
    // Vertical lines
    for (double x = std::fmod(ox, sx); x <= cw; x += sx) {
      cr->move_to(x, 0);
      cr->line_to(x, ch);
      cr->stroke();
    }
    // Horizontal lines
    for (double y = std::fmod(oy, sy); y <= ch; y += sy) {
      cr->move_to(0, y);
      cr->line_to(cw, y);
      cr->stroke();
    }
  }
  cr->restore();
}

// ── draw_margin_doc_space
// ────────────────────────────────────────────────────
// Draws margin and column/row guide lines as clean coloured lines.
// No tinted fill areas — objects can snap to the lines cleanly.
void Canvas::draw_margin_doc_space(const Cairo::RefPtr<Cairo::Context> &cr,
                                   const SceneNode *ml) {
  if (!ml || !ml->visible || !m_doc)
    return;

  const double cw = (double)m_doc->canvas_width();
  const double ch = (double)m_doc->canvas_height();

  const double left = ml->margin_left;
  const double right = ml->margin_right;
  const double top = ml->margin_top;
  const double bottom = ml->margin_bottom;

  const double ix = left;
  const double iy = top;
  const double iw = cw - left - right;
  const double ih = ch - top - bottom;
  if (iw <= 0 || ih <= 0)
    return;

  cr->save();
  cr->scale(m_zoom, m_zoom);

  const double r = ml->margin_color_r;
  const double g = ml->margin_color_g;
  const double b = ml->margin_color_b;
  // Lines are drawn at full opacity; alpha from the node colour sets intensity
  const double a = std::min(ml->margin_color_a * 3.0, 1.0);

  cr->set_source_rgba(r, g, b, a);
  cr->set_line_width(1.0 / m_zoom);

  // Margin border rectangle
  cr->rectangle(ix, iy, iw, ih);
  cr->stroke();

  // Column dividers — each gap is split at its centre line
  const int cols = std::max(1, ml->margin_columns);
  const double gap_x = ml->margin_col_gap;
  if (cols > 1 && iw > 0) {
    const double col_w = (iw - gap_x * (cols - 1)) / cols;
    for (int c = 1; c < cols; ++c) {
      // Left edge of gap
      double gx_left = ix + c * col_w + (c - 1) * gap_x;
      // Right edge of gap
      double gx_right = gx_left + gap_x;
      // Draw left and right edges of the gap as lines
      cr->move_to(gx_left, iy);
      cr->line_to(gx_left, iy + ih);
      cr->stroke();
      cr->move_to(gx_right, iy);
      cr->line_to(gx_right, iy + ih);
      cr->stroke();
    }
  }

  // Row dividers
  const int rows = std::max(1, ml->margin_rows);
  const double gap_y = ml->margin_row_gap;
  if (rows > 1 && ih > 0) {
    const double row_h = (ih - gap_y * (rows - 1)) / rows;
    for (int row = 1; row < rows; ++row) {
      double gy_top = iy + row * row_h + (row - 1) * gap_y;
      double gy_bottom = gy_top + gap_y;
      cr->move_to(ix, gy_top);
      cr->line_to(ix + iw, gy_top);
      cr->stroke();
      cr->move_to(ix, gy_bottom);
      cr->line_to(ix + iw, gy_bottom);
      cr->stroke();
    }
  }

  cr->restore();
}

// ── Guide drag from ruler ────────────────────────────────────────────────────
void Canvas::begin_guide_drag(double doc_pos, bool horizontal) {
  if (!m_doc)
    return;
  SceneNode *gl = m_doc->ensure_guide_layer();
  // If guide layer was somehow saved as invisible, make it visible now
  // (creating a guide implies the user wants to see guides)
  if (!gl->visible)
    gl->visible = true;
  if (gl->locked)
    return;
  LOG_DEBUG("Canvas::begin_guide_drag pos={:.2f} horiz={}", doc_pos,
            horizontal);

  // Create guide node and start dragging it.  Anchor = canvas-center-on-line;
  // angle = 0 (H) or 90 (V).
  auto guide = std::make_unique<SceneNode>();
  guide->type = SceneNode::Type::Guide;
  const double cw_doc = m_doc->canvas_width();
  const double ch_doc = m_doc->canvas_height();
  if (horizontal) {
    guide->guide_x = cw_doc * 0.5;
    guide->guide_y = doc_pos;
    guide->guide_angle = 0.0;
  } else {
    guide->guide_x = doc_pos;
    guide->guide_y = ch_doc * 0.5;
    guide->guide_angle = 90.0;
  }

  m_guide_drag_node = guide.get();
  gl->children.push_back(std::move(guide));
  m_guide_drag_active = true;
  queue_draw();
}

void Canvas::update_guide_drag(double doc_pos) {
  if (!m_guide_drag_active || !m_guide_drag_node)
    return;
  // Ruler-originated drag: adjust along the varying axis.
  if (m_guide_drag_node->guide_is_horizontal()) {
    m_guide_drag_node->guide_y = doc_pos;
  } else if (m_guide_drag_node->guide_is_vertical()) {
    m_guide_drag_node->guide_x = doc_pos;
  }
  queue_draw();
}

void Canvas::end_guide_drag(double doc_pos) {
  LOG_DEBUG("Canvas::end_guide_drag pos={:.2f} active={}", doc_pos,
            m_guide_drag_active);
  if (!m_guide_drag_active || !m_guide_drag_node)
    return;
  if (m_guide_drag_node->guide_is_horizontal()) {
    m_guide_drag_node->guide_y = doc_pos;
  } else if (m_guide_drag_node->guide_is_vertical()) {
    m_guide_drag_node->guide_x = doc_pos;
  }
  m_guide_drag_node = nullptr;
  m_guide_drag_active = false;
  m_sig_doc_changed.emit();
  queue_draw();
}

void Canvas::cancel_guide_drag() {
  if (!m_guide_drag_active || !m_guide_drag_node || !m_doc)
    return;
  // Remove the guide that was being created
  SceneNode *gl = m_doc->guide_layer();
  if (gl) {
    auto it = std::find_if(gl->children.begin(), gl->children.end(),
                           [this](const std::unique_ptr<SceneNode> &c) {
                             return c.get() == m_guide_drag_node;
                           });
    if (it != gl->children.end())
      gl->children.erase(it);
  }
  m_guide_drag_node = nullptr;
  m_guide_drag_active = false;
  queue_draw();
}

void Canvas::set_guide_selection(const std::vector<SceneNode *> &sel) {
  m_guide_selection = sel;
  // Selecting guides clears object selection silently — no signal emit
  // to avoid a round-trip that would wipe the guide selection we just set.
  if (!sel.empty() && !m_selection.empty()) {
    m_selection.clear();
    m_selected = nullptr;
    m_selected_node = -1;
  }
  queue_draw();
  // Called from external code (LayersPanel/inspector) — do not re-emit.
}

void Canvas::clear_guide_selection() {
  if (m_guide_selection.empty())
    return;
  m_guide_selection.clear();
  queue_draw();
}

void Canvas::delete_selected_guides() {
  if (!m_doc)
    return;
  SceneNode *gl = m_doc->guide_layer();
  if (!gl || m_guide_selection.empty())
    return;
  for (SceneNode *g : m_guide_selection) {
    if (m_guide_hovered == g)
      m_guide_hovered = nullptr;
    if (m_guide_drag_node == g) {
      m_guide_drag_node = nullptr;
      m_guide_drag_active = false;
    }
    gl->children.erase(std::remove_if(gl->children.begin(), gl->children.end(),
                                      [g](const std::unique_ptr<SceneNode> &c) {
                                        return c.get() == g;
                                      }),
                       gl->children.end());
  }
  m_guide_selection.clear();
  m_sig_guide_selection_changed.emit(m_guide_selection);
  m_sig_doc_changed.emit();
  queue_draw();
}

void Canvas::delete_guide(SceneNode *g) {
  if (!m_doc || !g)
    return;
  SceneNode *gl = m_doc->guide_layer();
  if (!gl)
    return;
  auto it = std::find_if(
      gl->children.begin(), gl->children.end(),
      [g](const std::unique_ptr<SceneNode> &c) { return c.get() == g; });
  if (it == gl->children.end())
    return;
  // Remove from selection set, clear hover/drag if pointing at this guide
  m_guide_selection.erase(
      std::remove(m_guide_selection.begin(), m_guide_selection.end(), g),
      m_guide_selection.end());
  if (m_guide_hovered == g)
    m_guide_hovered = nullptr;
  if (m_guide_drag_node == g) {
    m_guide_drag_node = nullptr;
    m_guide_drag_active = false;
  }
  gl->children.erase(it);
  m_sig_doc_changed.emit();
  queue_draw();
}

} // namespace Curvz
