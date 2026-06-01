// ─────────────────────────────────────────────────────────────────────────────
// StyleBar.cpp — s329 m1 rough-in. See StyleBar.hpp for the design rationale.
//
// This pass: stand the bar up, lay out the eight chips with placeholder faces
// (real icons land as Scott supplies them), and wire the Weight popover's
// B/I/U triggers to the character-formatting backend. Everything else is a
// stub popover so the structure is visible and clickable without crashing.
// ─────────────────────────────────────────────────────────────────────────────
#include "StyleBar.hpp"
#include "curvz_utils.hpp"
#include "CurvzSpinButton.hpp"
#include "CurvzColorPicker.hpp"  // s332 — embedded in the Fill popover
#include "color/Color.hpp"       // s332 — color::Color for the picker round-trip
#include "UnitSystem.hpp"   // s331 — Unit::Pt + px<->pt for the Size chip

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/drawingarea.h>   // s332 — the Fill chip's swatch face
#include <gtkmm/image.h>         // s332 — the Alignment chip's prefix icon
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/searchentry.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/listbox.h>
#include <gtkmm/listboxrow.h>
#include <pango/pango.h>        // PangoAttrType / weight consts
#include <pango/pangocairo.h>   // pango_cairo_font_map_get_default
#include <cairomm/context.h>    // s332 — Cairo::Context for the swatch draw
#include <algorithm>
#include <cmath>                // s331 — std::lround for point<->scale
#include <cstdio>
#include <vector>

namespace Curvz {

// Pango payloads, named here so the call sites read like the s326 hotkey path
// (which passes the real PANGO_* enums). Kept local so the header stays
// pango-free per its contract.
namespace {
constexpr int kAttrWeight = PANGO_ATTR_WEIGHT;
constexpr int kAttrSize   = PANGO_ATTR_SIZE;   // s331 — per-run size (point)
constexpr int kAttrForeground = PANGO_ATTR_FOREGROUND;  // s332 — per-run fill

// s331 — common font-size presets, in points. The spin (steppers + parser)
// covers everything between/around these; the list is the quick-jump.
constexpr double kSizePresets[] = {6, 8, 9, 10, 11, 12, 14, 18, 24, 36, 48, 72};

// Format a point value for the chip face / preset label: one decimal, but
// drop a trailing ".0" so whole sizes read "12" not "12.0".
std::string fmt_pt(double pt) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f", pt);
  std::string s(buf);
  if (s.size() >= 2 && s.compare(s.size() - 2, 2, ".0") == 0)
    s.erase(s.size() - 2);
  return s;
}

// Emphasis (line-decoration) attrs — all live on the span toggle path, with
// strikethrough/overline cases present in build_line_attrs / encode_markup /
// decode_markup_into.
constexpr int  kAttrStyle         = PANGO_ATTR_STYLE;
constexpr int  kAttrUnderline     = PANGO_ATTR_UNDERLINE;
constexpr int  kAttrStrikethrough = PANGO_ATTR_STRIKETHROUGH;
constexpr int  kAttrOverline      = PANGO_ATTR_OVERLINE;
constexpr long kStyleItalic       = 2;   // PANGO_STYLE_ITALIC
constexpr long kUnderlineSingle   = 1;   // PANGO_UNDERLINE_SINGLE
constexpr long kStrikeOn          = 1;   // TRUE
constexpr long kOverlineSingle    = 1;   // PANGO_OVERLINE_SINGLE

// The named Pango weight stops we surface as presets. Pango weight is a
// number 100..900 (variable fonts use the in-betweens, e.g. 450); the stops
// are the common faces. Bold is just 700 here — no separate B toggle.
struct WeightStop { const char* name; long value; };
constexpr WeightStop kWeightStops[] = {
    {"Thin",        100}, {"Extra Light", 200}, {"Light",      300},
    {"Normal",      400}, {"Medium",      500}, {"Semibold",   600},
    {"Bold",        700}, {"Extra Bold",  800}, {"Black",      900},
};

// Map a weight number to its display name; raw number for off-stop values
// (e.g. a variable-font 450 typed into the custom field).
std::string weight_face(long w) {
  for (const auto& s : kWeightStops)
    if (s.value == w) return s.name;
  return std::to_string(w);
}

// s332 — the Fill chip's swatch face. Draws a small rounded square in the
// effective text colour, OR a red diagonal slash on white when the fill is
// None/transparent, OR a two-tone diagonal split (colour over neutral grey)
// when the selection spans more than one colour. When no edit is active /
// unresolved, draws a hollow neutral square so the chip still reads as "fill".
void draw_swatch_face(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h,
                      double r, double g, double b,
                      bool resolved, bool mixed, bool is_none,
                      bool outline = false) {
  const double rad = 3.0;
  const double x0 = 1.5, y0 = 1.5;
  const double x1 = (double)w - 1.5, y1 = (double)h - 1.5;
  auto rounded = [&]() {
    cr->begin_new_path();
    cr->move_to(x0 + rad, y0);
    cr->line_to(x1 - rad, y0);
    cr->arc(x1 - rad, y0 + rad, rad, -M_PI / 2, 0);
    cr->line_to(x1, y1 - rad);
    cr->arc(x1 - rad, y1 - rad, rad, 0, M_PI / 2);
    cr->line_to(x0 + rad, y1);
    cr->arc(x0 + rad, y1 - rad, rad, M_PI / 2, M_PI);
    cr->line_to(x0, y0 + rad);
    cr->arc(x0 + rad, y0 + rad, rad, M_PI, 3 * M_PI / 2);
    cr->close_path();
  };

  if (!resolved) {
    // Hollow neutral square — no active selection to read from.
    rounded();
    cr->set_source_rgba(0.6, 0.6, 0.6, 0.6);
    cr->set_line_width(1.0);
    cr->stroke();
    return;
  }

  if (is_none) {
    // White ground + red diagonal slash (the canonical "no paint").
    rounded();
    cr->set_source_rgb(1.0, 1.0, 1.0);
    cr->fill_preserve();
    cr->set_source_rgba(0.55, 0.55, 0.55, 1.0);
    cr->set_line_width(1.0);
    cr->stroke();
    cr->move_to(x0 + 1.0, y1 - 1.0);
    cr->line_to(x1 - 1.0, y0 + 1.0);
    cr->set_source_rgb(0.85, 0.15, 0.15);
    cr->set_line_width(1.6);
    cr->stroke();
    return;
  }

  if (mixed) {
    // Two-tone diagonal split: the representative colour over neutral grey,
    // signalling the selection holds more than one fill. Reuses the picker's
    // new-over-old split idiom.
    rounded();
    cr->clip();
    cr->move_to(x0, y0); cr->line_to(x1, y0); cr->line_to(x0, y1);
    cr->close_path();
    cr->set_source_rgb(r, g, b);
    cr->fill();
    cr->move_to(x1, y0); cr->line_to(x1, y1); cr->line_to(x0, y1);
    cr->close_path();
    cr->set_source_rgb(0.62, 0.62, 0.62);
    cr->fill();
    cr->reset_clip();
    rounded();
    cr->set_source_rgba(0.45, 0.45, 0.45, 1.0);
    cr->set_line_width(1.0);
    cr->stroke();
    return;
  }

  // Resolved single colour — solid swatch with a subtle border.
  if (outline) {
    // Stroke chip: a hollow square ringed in the colour (a stroke reads as an
    // edge, not a fill). A faint keyline underneath keeps a near-white stroke
    // colour legible against the chip.
    rounded();
    cr->set_source_rgb(r, g, b);
    cr->set_line_width(2.6);
    cr->stroke();
    rounded();
    cr->set_source_rgba(0.45, 0.45, 0.45, 0.5);
    cr->set_line_width(0.75);
    cr->stroke();
    return;
  }
  rounded();
  cr->set_source_rgb(r, g, b);
  cr->fill_preserve();
  double luma = 0.299 * r + 0.587 * g + 0.114 * b;
  cr->set_source_rgba(0.45, 0.45, 0.45, luma > 0.7 ? 1.0 : 0.5);
  cr->set_line_width(1.0);
  cr->stroke();
}
} // namespace

StyleBar::StyleBar() : Gtk::Box(Gtk::Orientation::HORIZONTAL, 2) {
  curvz::utils::set_name(this, "sty_bar", "style_bar_root");
  add_css_class("curvz-style-bar");

  // Thin strip: no expansion, tight spacing, small margins. The chips keep
  // their natural size; the bar hugs them. (CSS polish — padding-strip to get
  // it truly ruler-thin, the centered cluster — is a follow-up; this is the
  // structural rough-in.)
  set_halign(Gtk::Align::START);
  set_valign(Gtk::Align::START);
  set_margin_start(2);
  set_margin_end(2);
  set_margin_top(1);
  set_margin_bottom(1);

  // ── Character / selection scope ──────────────────────────────────────────
  m_chip_font = add_label_chip("sty_font", "style_bar_font_chip",
                               "Sans", "Font family");
  m_chip_size = add_label_chip("sty_size", "style_bar_size_chip",
                               "12 pt", "Font size (placeholder)");
  // Weight — the one LIVE axis in this pass, now a value picker. Label-faced
  // because the value reads as a name ("Normal" / "Bold" / "450"), not a glyph.
  m_chip_weight = add_label_chip("sty_wt", "style_bar_weight_chip",
                                 "Normal", "Font weight (Thin .. Black)");
  // Emphasis — the additive decoration toggles (italic / underline / strike /
  // overline). Label-faced ("Emphasis", uppercased to EMPHASIS by the chip
  // chrome) like Font / Size / Weight / Style, since the set has a name and
  // "everything is a word" reads cleaner than a glyph here. The popover holds
  // the toggles; the lit/mixed face reflecting the selection is deferred
  // update_state work.
  m_chip_emphasis = add_label_chip(
      "sty_emph", "style_bar_emphasis_chip", "Emphasis",
      "Emphasis: italic / underline / strike / overline");
  // FILL — a named chip like the rest (label "Fill", uppercased to FILL by the
  // chip chrome, plus the dropdown arrow), with a small colour square prefixed
  // in front of the name showing the current fill under the cursor. The square
  // is a Cairo-drawn DrawingArea inside the chip's child box; the name and the
  // arrow stay so FILL reads consistently with FONT / SIZE / WEIGHT.
  m_chip_fill = Gtk::make_managed<Gtk::MenuButton>();
  curvz::utils::set_name(m_chip_fill, "sty_fill", "style_bar_fill_chip");
  m_chip_fill->set_has_frame(false);
  m_chip_fill->set_always_show_arrow(true);  // keep the dropdown arrow (a child
                                             // MenuButton hides it otherwise)
  m_chip_fill->set_tooltip_text("Text fill colour");
  m_chip_fill->add_css_class("curvz-style-chip");
  m_chip_fill->add_css_class("curvz-style-chip-label");
  {
    auto* face = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
    m_fill_face = Gtk::make_managed<Gtk::DrawingArea>();
    m_fill_face->set_content_width(14);
    m_fill_face->set_content_height(14);
    m_fill_face->set_valign(Gtk::Align::CENTER);
    m_fill_face->set_draw_func(
        [this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
          draw_swatch_face(cr, w, h, m_fill_r, m_fill_g, m_fill_b,
                           m_fill_resolved, m_fill_mixed, m_fill_none);
        });
    auto* name = Gtk::make_managed<Gtk::Label>("Fill");
    face->append(*m_fill_face);
    face->append(*name);
    m_chip_fill->set_child(*face);
  }
  append(*m_chip_fill);

  // STROKE — same shape as FILL (named chip + square prefix + arrow), but the
  // square draws as an outline ring (a stroke reads as an edge). Object-level.
  m_chip_stroke = Gtk::make_managed<Gtk::MenuButton>();
  curvz::utils::set_name(m_chip_stroke, "sty_strk", "style_bar_stroke_chip");
  m_chip_stroke->set_has_frame(false);
  m_chip_stroke->set_always_show_arrow(true);
  m_chip_stroke->set_tooltip_text("Text stroke colour + width");
  m_chip_stroke->add_css_class("curvz-style-chip");
  m_chip_stroke->add_css_class("curvz-style-chip-label");
  {
    auto* face = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
    m_stroke_face = Gtk::make_managed<Gtk::DrawingArea>();
    m_stroke_face->set_content_width(14);
    m_stroke_face->set_content_height(14);
    m_stroke_face->set_valign(Gtk::Align::CENTER);
    m_stroke_face->set_draw_func(
        [this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
          draw_swatch_face(cr, w, h, m_stroke_r, m_stroke_g, m_stroke_b,
                           /*resolved=*/true, /*mixed=*/false,
                           /*is_none=*/!m_stroke_has, /*outline=*/true);
        });
    auto* name = Gtk::make_managed<Gtk::Label>("Stroke");
    face->append(*m_stroke_face);
    face->append(*name);
    m_chip_stroke->set_child(*face);
  }
  append(*m_chip_stroke);

  add_sep();  // the character/paragraph scope seam, made visible

  // ── Paragraph scope ──────────────────────────────────────────────────────
  // ALIGNMENT — same shape as FILL / STROKE (prefix + name + arrow). The prefix
  // is the CURRENT paragraph alignment's glyph (curvz-text-*), updated by
  // set_align_face as the caret/selection moves; the name + arrow keep it
  // reading consistently with FILL / STROKE / FONT.
  m_chip_align = Gtk::make_managed<Gtk::MenuButton>();
  curvz::utils::set_name(m_chip_align, "sty_aln", "style_bar_align_chip");
  m_chip_align->set_has_frame(false);
  m_chip_align->set_always_show_arrow(true);  // keep the dropdown arrow
  m_chip_align->set_tooltip_text("Alignment");
  m_chip_align->add_css_class("curvz-style-chip");
  m_chip_align->add_css_class("curvz-style-chip-label");
  {
    auto* face = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
    m_align_icon = Gtk::make_managed<Gtk::Image>();
    m_align_icon->set_from_icon_name("curvz-text-left-symbolic");
    m_align_icon->set_valign(Gtk::Align::CENTER);
    auto* name = Gtk::make_managed<Gtk::Label>("Alignment");
    face->append(*m_align_icon);
    face->append(*name);
    m_chip_align->set_child(*face);
  }
  append(*m_chip_align);
  m_chip_para = add_label_chip("sty_para", "style_bar_linespacing_chip",
                               "Line spacing", "Line spacing");
  m_chip_style = add_label_chip("sty_named", "style_bar_style_chip",
                                "Body", "Named paragraph style (placeholder)");

  // Wire the live popovers after all chips exist.
  build_weight_popover(m_chip_weight);
  build_emphasis_popover(m_chip_emphasis);
  build_font_popover(m_chip_font);
  build_size_popover(m_chip_size);
  build_paragraph_popover(m_chip_para);  build_fill_popover(m_chip_fill);  // s332
  build_stroke_popover(m_chip_stroke);  // s332
  build_align_popover(m_chip_align);    // s332

  // Stub popovers for the chips whose real popups aren't built yet.
  stub_popover(m_chip_style,  "Style chooser + override + verbs");

  // s331 — far-right Reset button. An expanding spacer pushes it to the end
  // of the bar (the chips stay left-clumped per §9; centering them is a
  // separate cosmetic pass). Direct action — no popover. Strips all per-run
  // formatting on the active text back to defaults via the reset callback.
  auto* spacer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  spacer->set_hexpand(true);
  append(*spacer);
  auto* reset = Gtk::make_managed<Gtk::Button>("Reset");
  curvz::utils::set_name(reset, "sty_reset", "style_bar_reset_button");
  reset->set_has_frame(false);
  reset->set_can_focus(false);  // don't steal focus from the active text edit
  reset->set_tooltip_text("Reset text to default formatting");
  reset->add_css_class("curvz-style-chip");
  reset->add_css_class("curvz-style-chip-label");
  reset->signal_clicked().connect([this]() {
    if (m_reset_request) m_reset_request();
  });
  append(*reset);
}

Gtk::MenuButton* StyleBar::add_label_chip(const std::string& abbrev,
                                          const std::string& long_name,
                                          const std::string& initial_label,
                                          const std::string& tip) {
  auto* chip = Gtk::make_managed<Gtk::MenuButton>();
  curvz::utils::set_name(chip, abbrev.c_str(), long_name.c_str());
  chip->set_label(initial_label);
  chip->set_has_frame(false);
  chip->set_always_show_arrow(false);  // keep it thin; the label IS the face
  chip->set_tooltip_text(tip);
  chip->add_css_class("curvz-style-chip");
  chip->add_css_class("curvz-style-chip-label");
  append(*chip);
  return chip;
}

Gtk::MenuButton* StyleBar::add_icon_chip(const std::string& abbrev,
                                         const std::string& long_name,
                                         const std::string& icon_or_face,
                                         const std::string& tip,
                                         bool face_is_icon) {
  auto* chip = Gtk::make_managed<Gtk::MenuButton>();
  curvz::utils::set_name(chip, abbrev.c_str(), long_name.c_str());
  if (face_is_icon)
    chip->set_icon_name(icon_or_face);
  else
    chip->set_label(icon_or_face);  // glyph-as-text placeholder (e.g. "A", "¶")
  chip->set_has_frame(false);
  chip->set_always_show_arrow(false);
  chip->set_tooltip_text(tip);
  chip->add_css_class("curvz-style-chip");
  chip->add_css_class(face_is_icon ? "curvz-style-chip-icon"
                                    : "curvz-style-chip-glyph");
  append(*chip);
  return chip;
}

Gtk::Separator* StyleBar::add_sep() {
  auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
  sep->add_css_class("curvz-style-sep");
  sep->set_margin_start(2);
  sep->set_margin_end(2);
  append(*sep);
  return sep;
}

void StyleBar::stub_popover(Gtk::MenuButton* chip, const std::string& axis_label) {
  if (!chip) return;
  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* lbl = Gtk::make_managed<Gtk::Label>(axis_label);
  lbl->set_margin(8);
  lbl->add_css_class("dim-label");
  pop->set_child(*lbl);
  chip->set_popover(*pop);
}

void StyleBar::set_font_face(const Glib::ustring& family, bool mixed) {
  if (!m_chip_font) return;
  Glib::ustring next = mixed ? "\u2014"                       // em dash = mixed
                             : (family.empty() ? "Font" : family);
  if (m_chip_font->get_label() == next) return;  // unchanged -> no action
  m_chip_font->set_label(next);
}

void StyleBar::set_weight_face(long weight, bool resolved, bool mixed) {
  if (!m_chip_weight) return;
  Glib::ustring next = mixed ? "\u2014"
                             : (resolved ? Glib::ustring(weight_face(weight))
                                         : "Weight");
  if (m_chip_weight->get_label() == next) return;  // unchanged -> no action
  m_chip_weight->set_label(next);
}

void StyleBar::build_weight_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  col->set_margin(6);

  // Apply a weight: SET (not toggle) over the selection, update the chip
  // face to the new value's name, and close the popover like a menu pick.
  auto apply_weight = [this, chip, pop](long w) {
    if (m_format_set) m_format_set(kAttrWeight, w, "");
    chip->set_label(weight_face(w));  // "last look" face follows the pick
    pop->popdown();
  };

  // Preset stops — one flat menu-like row per named weight. Exclusive by
  // nature: each SET replaces the last, so no lit-state bookkeeping here
  // (the live read of the selection's current weight is the deferred
  // update_state work).
  for (const auto& s : kWeightStops) {
    auto* b = Gtk::make_managed<Gtk::Button>(s.name);
    b->set_has_frame(false);
    b->set_can_focus(false);  // don't steal focus from the active text edit
    b->add_css_class("curvz-style-weight-stop");
    if (auto* lbl = dynamic_cast<Gtk::Label*>(b->get_child()))
      lbl->set_xalign(0.0f);  // left-align the names into a tidy column
    long w = s.value;
    b->signal_clicked().connect([apply_weight, w]() { apply_weight(w); });
    col->append(*b);
  }

  auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  sep->set_margin_top(4);
  sep->set_margin_bottom(4);
  col->append(*sep);

  // Custom numeric override — for the variable-font in-betweens (450, 575).
  // Pango weight is an integer, so SpinType::Integer. Applied on commit
  // (Enter / activate) only, so stepping the value doesn't spray undo steps;
  // one apply = one undoable command, matching the preset path.
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* row_lbl = Gtk::make_managed<Gtk::Label>("Custom");
  row_lbl->set_xalign(0.0f);
  row_lbl->add_css_class("dim-label");
  auto* spin = Gtk::make_managed<CurvzSpinButton>("sty_wt_num",
                                                  SpinType::Integer);
  spin->with_value(400)->with_width_chars(4)
      ->with_tooltip("Any weight 1-1000 (variable fonts)");
  spin->set_can_focus(true);  // the field DOES take focus to type into
  spin->signal_activate().connect([apply_weight, spin]() {
    apply_weight((long)spin->get_internal_value());
  });
  row->append(*row_lbl);
  row->append(*spin);
  col->append(*row);

  pop->set_child(*col);
  chip->set_popover(*pop);
}

void StyleBar::build_size_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  col->set_margin(6);

  // Apply a size in points over the selection: SET PANGO_ATTR_SIZE (point x
  // PANGO_SCALE). The set-path emits text_style_changed, so the face + spin
  // refresh via the live-read; we don't write the face here.
  auto apply_pt = [this](double pt) {
    if (m_format_set)
      m_format_set(kAttrSize, std::lround(pt * (double)PANGO_SCALE), "");
  };

  // ── The pt-locked editable field (steppers + math/units parser). Width
  // type (non-negative) with the unit override pinned to points, so it owns
  // its unit regardless of the document unit and "2in" commits as 144 pt.
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  m_size_spin = Gtk::make_managed<CurvzSpinButton>("sty_sz_num", SpinType::Width);
  m_size_spin->with_unit_override(Unit::Pt)
      ->with_value(UnitSystem::to_px(12.0, Unit::Pt))  // 12 pt default
      ->with_width_chars(5)
      ->with_tooltip("Font size in points (type 2in, 10mm, 12pt, ...)");
  m_size_spin->set_can_focus(true);  // the field takes focus to type into
  // Steppers nudge and typed-commits apply live (one apply per change).
  m_size_spin->on_changed([this, apply_pt](double internal_px) {
    apply_pt(m_size_spin->to_display(internal_px));  // px -> pt (override)
  });
  row->append(*m_size_spin);
  row->append(*m_size_spin->get_unit_label());
  col->append(*row);

  auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  sep->set_margin_top(2);
  sep->set_margin_bottom(2);
  col->append(*sep);

  // ── Preset jumps — common sizes in a scrolled column, like the weight
  // stops. Each SETS that point size and closes the popover.
  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_min_content_height(180);
  auto* presets = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  for (double pt : kSizePresets) {
    auto* b = Gtk::make_managed<Gtk::Button>(fmt_pt(pt) + " pt");
    b->set_has_frame(false);
    b->set_can_focus(false);  // don't steal focus from the active text edit
    b->add_css_class("curvz-style-weight-stop");  // reuse the menu-row look
    if (auto* lbl = dynamic_cast<Gtk::Label*>(b->get_child()))
      lbl->set_xalign(0.0f);
    b->signal_clicked().connect([apply_pt, pop, pt]() {
      apply_pt(pt);
      pop->popdown();
    });
    presets->append(*b);
  }
  scroller->set_child(*presets);
  col->append(*scroller);

  pop->set_child(*col);
  chip->set_popover(*pop);
}

void StyleBar::set_size_face(double pt, bool resolved, bool mixed) {
  if (!m_chip_size) return;
  Glib::ustring next = mixed ? "\u2014"
                             : (resolved ? Glib::ustring(fmt_pt(pt) + " pt")
                                         : "Size");
  if (m_chip_size->get_label() != next)
    m_chip_size->set_label(next);
  // Sync the popover spin to a resolved single size (no apply re-fired —
  // set_internal_value is guarded against emitting).
  if (resolved && !mixed && m_size_spin)
    m_size_spin->set_internal_value(UnitSystem::to_px(pt, Unit::Pt));
}

void StyleBar::build_fill_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  col->set_margin(6);

  m_fill_picker = Gtk::make_managed<CurvzColorPicker>();
  m_fill_picker->set_with_alpha(false);  // Pango foreground is opaque RGB
  m_fill_picker->set_initial(color::Color(0.0, 0.0, 0.0, 1.0));
  // Live: every interactive edit SETS the foreground over the selection. The
  // set-path emits text_style_changed, so the swatch face refreshes via the
  // live-read; we don't write the face here. set_initial (used by the live-
  // read sync) does not emit signal_changed, so there's no feedback loop;
  // m_suppress_fill guards it anyway as belt-and-braces.
  m_fill_picker->signal_changed().connect([this](color::Color c) {
    if (m_suppress_fill) return;
    auto ch = [](double v) {
      return (long)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
    };
    long packed = (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
    if (m_format_set) m_format_set(kAttrForeground, packed, "");
  });
  col->append(*m_fill_picker);

  pop->set_child(*col);
  chip->set_popover(*pop);
}

void StyleBar::set_fill_face(unsigned long rgb, bool resolved, bool mixed,
                             bool is_none) {
  m_fill_resolved = resolved;
  m_fill_mixed    = mixed;
  m_fill_none     = is_none;
  m_fill_r = (double)((rgb >> 16) & 0xFF) / 255.0;
  m_fill_g = (double)((rgb >>  8) & 0xFF) / 255.0;
  m_fill_b = (double)( rgb        & 0xFF) / 255.0;
  if (m_fill_face) m_fill_face->queue_draw();
  // Sync the picker to a resolved single colour so re-opening the popover
  // starts from the current value (no apply re-fired — set_initial does not
  // emit signal_changed; the guard is held anyway for safety).
  if (resolved && !mixed && !is_none && m_fill_picker) {
    m_suppress_fill = true;
    m_fill_picker->set_initial(color::Color(m_fill_r, m_fill_g, m_fill_b, 1.0));
    m_suppress_fill = false;
  }
}

void StyleBar::build_stroke_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  col->set_margin(6);

  // Colour picker — picking SETS the per-run stroke colour over the selection
  // via the generic value-set span path (kCurvzStrokeColorAttr). The set
  // re-emits text_style_changed, so the face/spin refresh via the live-read;
  // set_initial doesn't emit, so syncing from the live-read can't loop
  // (m_suppress_stroke guards anyway).
  m_stroke_picker = Gtk::make_managed<CurvzColorPicker>();
  m_stroke_picker->set_with_alpha(false);  // stroke paint is opaque RGB here
  m_stroke_picker->set_initial(color::Color(0.0, 0.0, 0.0, 1.0));
  m_stroke_picker->signal_changed().connect([this](color::Color c) {
    if (m_suppress_stroke) return;
    auto ch = [](double v) {
      return (long)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
    };
    long packed = (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
    if (m_format_set)
      m_format_set(curvz::utils::kCurvzStrokeColorAttr, packed, "");
  });
  col->append(*m_stroke_picker);

  // Width row — pt-locked spin (reuses the size chip's unit override). SETS a
  // per-run width span (doc-px x PANGO_SCALE). Plain input for now; the finer
  // per-instance step + modifier x10 stepping is noted/parked.
  auto* wrow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* wlbl = Gtk::make_managed<Gtk::Label>("Width");
  wlbl->set_xalign(0.0f);
  wlbl->add_css_class("dim-label");
  m_stroke_width_spin = Gtk::make_managed<CurvzSpinButton>("sty_strk_w",
                                                           SpinType::Width);
  m_stroke_width_spin->with_unit_override(Unit::Pt)
      ->with_value(UnitSystem::to_px(1.0, Unit::Pt))  // 1 pt default
      ->with_width_chars(5)
      ->with_tooltip("Stroke width in points (type 2pt, 1mm, ...)");
  m_stroke_width_spin->set_can_focus(true);
  m_stroke_width_spin->on_changed([this](double internal_px) {
    if (m_format_set)
      m_format_set(curvz::utils::kCurvzStrokeWidthAttr,
                   std::lround(internal_px * (double)PANGO_SCALE), "");
  });
  wrow->append(*wlbl);
  wrow->append(*m_stroke_width_spin);
  wrow->append(*m_stroke_width_spin->get_unit_label());
  col->append(*wrow);

  // None — set an explicit no-stroke span over the selection (face shows the
  // red-slash; also suppresses the object stroke on those glyphs).
  auto* noneb = Gtk::make_managed<Gtk::Button>("None");
  noneb->set_has_frame(false);
  noneb->set_can_focus(false);
  noneb->add_css_class("curvz-style-weight-stop");
  if (auto* nl = dynamic_cast<Gtk::Label*>(noneb->get_child()))
    nl->set_xalign(0.0f);
  noneb->set_tooltip_text("No stroke");
  noneb->signal_clicked().connect([this, pop]() {
    if (m_format_set)
      m_format_set(curvz::utils::kCurvzStrokeColorAttr,
                   curvz::utils::kCurvzStrokeNone, "");
    pop->popdown();
  });
  col->append(*noneb);

  pop->set_child(*col);
  chip->set_popover(*pop);
}

void StyleBar::set_stroke_face(unsigned long rgb, bool has_color) {
  m_stroke_has = has_color;
  m_stroke_r = (double)((rgb >> 16) & 0xFF) / 255.0;
  m_stroke_g = (double)((rgb >>  8) & 0xFF) / 255.0;
  m_stroke_b = (double)( rgb        & 0xFF) / 255.0;
  if (m_stroke_face) m_stroke_face->queue_draw();
  if (has_color && m_stroke_picker) {
    m_suppress_stroke = true;
    m_stroke_picker->set_initial(
        color::Color(m_stroke_r, m_stroke_g, m_stroke_b, 1.0));
    m_suppress_stroke = false;
  }
}

void StyleBar::set_stroke_width(double pt) {
  if (!m_stroke_width_spin) return;
  m_stroke_width_spin->set_internal_value(UnitSystem::to_px(pt, Unit::Pt));  // no emit
}

void StyleBar::build_paragraph_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  col->set_margin(6);

  // ── Leading row: label + pt-locked spin + unit label. Buffer-global, so a
  // change requests Canvas::set_text_leading (not a span op). The spin reuses
  // the size chip's unit override (owns pt regardless of doc unit).
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* lbl = Gtk::make_managed<Gtk::Label>("Line spacing");
  lbl->set_xalign(0.0f);
  lbl->add_css_class("dim-label");
  m_leading_spin = Gtk::make_managed<CurvzSpinButton>("sty_lead_num",
                                                      SpinType::Width);
  m_leading_spin->with_unit_override(Unit::Pt)
      ->with_value(UnitSystem::to_px(14.0, Unit::Pt))  // 14 pt default look
      ->with_width_chars(5)
      ->with_tooltip("Line spacing in points (type 2in, 10mm, 14pt, ...)");
  m_leading_spin->set_can_focus(true);
  m_leading_spin->on_changed([this](double internal_px) {
    if (m_leading_request)
      m_leading_request(m_leading_spin->to_display(internal_px));  // px -> pt
  });
  row->append(*lbl);
  row->append(*m_leading_spin);
  row->append(*m_leading_spin->get_unit_label());
  col->append(*row);

  // Auto: hand leading back to the metric-derived default (line_height = 0).
  // Signalled as pt = 0, which Canvas reads as auto.
  auto* autob = Gtk::make_managed<Gtk::Button>("Auto");
  autob->set_has_frame(false);
  autob->set_can_focus(false);
  autob->add_css_class("curvz-style-weight-stop");  // reuse the menu-row look
  if (auto* al = dynamic_cast<Gtk::Label*>(autob->get_child()))
    al->set_xalign(0.0f);
  autob->set_tooltip_text("Auto line spacing (derive from the font metrics)");
  autob->signal_clicked().connect([this, pop]() {
    if (m_leading_request) m_leading_request(0.0);  // <= 0 = auto
    pop->popdown();
  });
  col->append(*autob);

  pop->set_child(*col);
  chip->set_popover(*pop);
}

void StyleBar::set_leading(double pt, bool is_auto) {
  if (!m_leading_spin) return;
  m_leading_spin->set_tooltip_text(
      is_auto ? "Line spacing — Auto (from font metrics); type a value to pin it"
              : "Line spacing in points (type 2in, 10mm, 14pt, ...)");
  m_leading_spin->set_internal_value(UnitSystem::to_px(pt, Unit::Pt));  // no emit
}

// s332 — Alignment popover: Left / Centre / Right as a small icon row. Each
// requests Canvas::set_text_alignment(0/1/2), which paragraph-snaps and writes
// the kCurvzAlignAttr run. Justify (3) is the follow-up (no icon yet, needs the
// Pango justify path). The chip face mirrors the caret paragraph's alignment.
void StyleBar::build_align_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 2);
  row->set_margin(6);

  struct Opt { const char* icon; const char* tip; int val; };
  const Opt opts[] = {
      {"curvz-text-left-symbolic",   "Align left",   0},
      {"curvz-text-center-symbolic", "Align centre", 1},
      {"curvz-text-right-symbolic",  "Align right",  2},
  };
  for (const auto& o : opts) {
    auto* b = Gtk::make_managed<Gtk::Button>();
    b->set_icon_name(o.icon);
    b->set_has_frame(false);
    b->set_can_focus(false);
    b->set_tooltip_text(o.tip);
    b->add_css_class("curvz-style-chip-icon");
    int val = o.val;
    b->signal_clicked().connect([this, pop, val]() {
      if (m_align_request) m_align_request(val);
      set_align_face(val);  // last-look face follows the pick (like Weight)
      pop->popdown();
    });
    row->append(*b);
  }
  pop->set_child(*row);
  chip->set_popover(*pop);
}

void StyleBar::set_align_face(int align) {
  if (!m_align_icon) return;
  const char* icon = (align == 1) ? "curvz-text-center-symbolic"
                   : (align == 2) ? "curvz-text-right-symbolic"
                                  : "curvz-text-left-symbolic";
  m_align_icon->set_from_icon_name(icon);
}

void StyleBar::build_emphasis_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
  box->set_margin(6);

  // s331 — each decoration is an independent ToggleButton over the selection
  // (additive set, not exclusive) routed through the s326 toggle backend.
  // The toggle now READS as well as writes: the live-read pushes its lit/off/
  // mixed state via set_emphasis_state on each signal_text_style_changed, and
  // a click toggles the attribute. The format-apply emits text_style_changed,
  // so the resulting state flows straight back onto the toggle — the click's
  // own active value isn't authoritative, the re-read corrects it. The
  // m_suppress_emphasis guard stops that programmatic set from re-firing here.
  auto add_toggle = [&](const std::string& face, const std::string& tip,
                        int attr, long value) -> Gtk::ToggleButton* {
    auto* b = Gtk::make_managed<Gtk::ToggleButton>(face);
    b->set_tooltip_text(tip);
    b->set_has_frame(false);
    b->set_can_focus(false);  // don't steal focus from the active text edit
    b->add_css_class("curvz-style-trigger");
    b->signal_toggled().connect([this, attr, value]() {
      if (m_suppress_emphasis) return;  // programmatic state push, not a click
      if (m_format_toggle) m_format_toggle(attr, value, "");
    });
    box->append(*b);
    return b;
  };

  m_emph_italic    = add_toggle("I", "Italic (Ctrl+I)",
                                kAttrStyle,         kStyleItalic);
  m_emph_underline = add_toggle("U", "Underline (Ctrl+U)",
                                kAttrUnderline,     kUnderlineSingle);
  m_emph_strike    = add_toggle("S", "Strikethrough",
                                kAttrStrikethrough, kStrikeOn);
  m_emph_overline  = add_toggle("O", "Overline",
                                kAttrOverline,      kOverlineSingle);

  pop->set_child(*box);
  chip->set_popover(*pop);
}

void StyleBar::set_emphasis_state(int italic, int underline, int strike,
                                  int overline) {
  // Apply one tri-state to one toggle: 0 = off, 1 = on, 2 = mixed. GTK4's
  // Gtk::ToggleButton has no set_inconsistent() (that's CheckButton), so the
  // mixed look rides a CSS class (.curvz-trigger-mixed) instead; on/off ride
  // the :checked state via set_active. Guarded so set_active() doesn't re-fire
  // the click handler when the value actually changes.
  m_suppress_emphasis = true;
  auto apply = [](Gtk::ToggleButton* t, int state) {
    if (!t) return;
    if (state == 2) t->add_css_class("curvz-trigger-mixed");
    else            t->remove_css_class("curvz-trigger-mixed");
    t->set_active(state == 1);
  };
  apply(m_emph_italic,    italic);
  apply(m_emph_underline, underline);
  apply(m_emph_strike,    strike);
  apply(m_emph_overline,  overline);
  m_suppress_emphasis = false;
}

void StyleBar::build_font_popover(Gtk::MenuButton* chip) {
  if (!chip) return;

  // Enumerate families from the default Pango font map (stable for the
  // session, so a build-once list is fine) and sort case-insensitively.
  std::vector<std::string> families;
  {
    PangoFontMap* fm = pango_cairo_font_map_get_default();
    PangoFontFamily** fams = nullptr;
    int n = 0;
    pango_font_map_list_families(fm, &fams, &n);
    families.reserve(n);
    for (int i = 0; i < n; ++i)
      families.emplace_back(pango_font_family_get_name(fams[i]));
    g_free(fams);
    std::sort(families.begin(), families.end(),
              [](const std::string& a, const std::string& b) {
                return g_ascii_strcasecmp(a.c_str(), b.c_str()) < 0;
              });
  }

  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  box->set_margin(6);

  auto* search = Gtk::make_managed<Gtk::SearchEntry>();
  box->append(*search);

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_min_content_height(320);
  scroller->set_min_content_width(220);

  auto* list = Gtk::make_managed<Gtk::ListBox>();
  list->set_selection_mode(Gtk::SelectionMode::NONE);  // activate, don't select
  list->add_css_class("curvz-font-list");
  for (const auto& fam : families) {
    auto* lbl = Gtk::make_managed<Gtk::Label>(fam);
    lbl->set_xalign(0.0f);
    lbl->set_margin_start(6);
    lbl->set_margin_end(6);
    lbl->set_margin_top(2);
    lbl->set_margin_bottom(2);
    list->append(*lbl);  // ListBox wraps each child in a ListBoxRow
  }
  scroller->set_child(*list);
  box->append(*scroller);

  // Case-insensitive substring filter, re-run as the search text changes.
  list->set_filter_func([search](Gtk::ListBoxRow* row) -> bool {
    Glib::ustring needle = search->get_text();
    if (needle.empty()) return true;
    auto* lbl = dynamic_cast<Gtk::Label*>(row->get_child());
    if (!lbl) return true;
    return lbl->get_text().lowercase().find(needle.lowercase())
           != Glib::ustring::npos;
  });
  search->signal_search_changed().connect(
      [list]() { list->invalidate_filter(); });

  // Pick: SET family over the selection, write the name onto the face, close.
  list->signal_row_activated().connect(
      [this, chip, pop](Gtk::ListBoxRow* row) {
        auto* lbl = dynamic_cast<Gtk::Label*>(row->get_child());
        if (!lbl) return;
        Glib::ustring fam = lbl->get_text();
        if (m_format_set) m_format_set(PANGO_ATTR_FAMILY, 0, fam.raw());
        chip->set_label(fam);
        pop->popdown();
      });

  pop->set_child(*box);
  chip->set_popover(*pop);
}

} // namespace Curvz
