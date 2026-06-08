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
#include <glibmm/main.h>         // s337 m2e — Glib::signal_idle (deferred row select)
#include "CurvzColorPicker.hpp"  // s332 — embedded in the Fill popover
#include "color/Color.hpp"       // s332 — color::Color for the picker round-trip
#include "UnitSystem.hpp"   // s331 — Unit::Pt + px<->pt for the Size chip
#include "DocUnits.hpp"     // s338 — doc-px -> display-unit pump for row labels
#include "widgets/DropDown.hpp"          // s339 — Type/Leader detail dropdowns
#include <gtkmm/stringlist.h>            // s339 — dropdown models
#include <gtkmm/eventcontrollerkey.h>    // s339 — Esc dismissal (popover off autohide)
#include "CurvzDocument.hpp" // s338 — CanvasModel (display_unit) behind set_doc_model

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/adjustment.h>     // s334 — tracking/rise spin adjustments
#include <gtkmm/scale.h>          // s334 — justify knobs (moved into align popover)
#include <gtkmm/spinbutton.h>     // s334 — tracking + rise spins
#include <gtkmm/drawingarea.h>   // s332 — the Fill chip's swatch face
#include <gtkmm/notebook.h>      // s343 — Fill/Stroke tabs in the Color popover
#include <gtkmm/image.h>         // s332 — the Alignment chip's prefix icon
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/searchentry.h>
#include <gtkmm/entry.h>          // s341 — new-style name field in the Style popover
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
constexpr int kAttrLetterSpacing = PANGO_ATTR_LETTER_SPACING;  // s334 — tracking
constexpr int kAttrRise          = PANGO_ATTR_RISE;            // s334 — baseline rise

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

// s334 — super/subscript. Pango 1.50+ font-scale does the shrink-and-shift
// together, font-metric aware (not a rise+size fake). One attribute, three
// values, so super and sub are mutually exclusive for free: setting one
// replaces the other over the range. NONE clears.
constexpr int  kAttrFontScale     = PANGO_ATTR_FONT_SCALE;
constexpr long kFontScaleNone     = PANGO_FONT_SCALE_NONE;        // 0
constexpr long kFontScaleSuper    = PANGO_FONT_SCALE_SUPERSCRIPT; // super
constexpr long kFontScaleSub      = PANGO_FONT_SCALE_SUBSCRIPT;   // sub

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

StyleBar::StyleBar() : Gtk::Box(Gtk::Orientation::HORIZONTAL, 1) {
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
  // TYPE — s343: Weight and Emphasis merged into one chip. The label follows
  // the resolved weight ("Normal" / "Bold" / "450"), matching the bar's idiom
  // (a chip shows its value) and the old Weight chip's behaviour; the popover
  // splits Weight / Emphasis into two tabs.
  m_chip_type = add_label_chip("sty_type", "style_bar_type_chip",
                               "Normal", "Type: weight + emphasis");
  // SPACING — s343: Tracking (character axis: letter-spacing + baseline rise)
  // and Line spacing (paragraph axis: leading) merged. Static "Spacing" label;
  // the popover stacks a Character group above a Line group.
  m_chip_spacing = add_label_chip("sty_spacing", "style_bar_spacing_chip",
                                  "Spacing", "Spacing: tracking + line spacing");
  // COLOR — s343: Fill and Stroke merged into one chip to reclaim a slot. The
  // face carries both glances: a filled swatch (fill) and an outline ring
  // (stroke), then the "Color" name. The popover (build_color_popover) splits
  // the two into Fill / Stroke tabs. Both DrawingAreas remain live, so the
  // existing set_fill_face / set_stroke_face live-read sync is unchanged.
  m_chip_color = Gtk::make_managed<Gtk::MenuButton>();
  curvz::utils::set_name(m_chip_color, "sty_color", "style_bar_color_chip");
  m_chip_color->set_has_frame(false);
  m_chip_color->set_always_show_arrow(true);  // keep the dropdown arrow (a child
                                              // MenuButton hides it otherwise)
  m_chip_color->set_tooltip_text("Text fill + stroke colour");
  m_chip_color->add_css_class("curvz-style-chip");
  m_chip_color->add_css_class("curvz-style-chip-label");
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
    auto* name = Gtk::make_managed<Gtk::Label>("Color");
    face->append(*m_fill_face);
    face->append(*m_stroke_face);
    face->append(*name);
    m_chip_color->set_child(*face);
  }
  append(*m_chip_color);


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
  // INDENTS — paragraph scope: left/right (link-locked) + first-line. Sits
  // between Alignment and Line-spacing, where paragraph geometry lives.
  m_chip_insets = add_label_chip("sty_insets", "style_bar_insets_chip",
                                 "Insets", "Box margins + paragraph indents");
  // s335 — TABS chip: own dropdown (sibling to Indents), holds the per-paragraph
  // tab-stop list editor. Paragraph geometry neighbourhood, right of Indents.
  m_chip_tabs = add_label_chip("sty_tabs", "style_bar_tabs_chip",
                               "Tabs", "Tab stops (per paragraph)");
  m_chip_style = add_label_chip("sty_named", "style_bar_style_chip",
                                "Style", "Named paragraph style");

  // Wire the live popovers after all chips exist.
  build_type_popover(m_chip_type);          // s343 — Weight + Emphasis tabs
  build_spacing_popover(m_chip_spacing);    // s343 — Tracking + Line spacing
  build_font_popover(m_chip_font);
  build_size_popover(m_chip_size);
  build_color_popover(m_chip_color);         // s343

  build_align_popover(m_chip_align);    // s332 (+ s334 justify knobs)
  build_insets_popover(m_chip_insets);  // s343 — Box margins + Paragraph indents
  build_tabs_popover(m_chip_tabs);      // s335
  build_style_popover(m_chip_style);    // s341 — named paragraph styles

  // (no stubbed chips remain — every chip carries a live popover)

  // s331 — far-right Reset button. An expanding spacer pushes it to the end
  // of the bar (the chips stay left-clumped per §9; centering them is a
  // separate cosmetic pass). Direct action — no popover. Strips all per-run
  // formatting on the active text back to defaults via the reset callback.
  // s338 — Show invisibles: a view toggle that draws tab / space / paragraph
  // markers in the edited text, like a word processor's "show formatting." It
  // sits with the chips (left of the right-pushed Reset). Diagnostic first --
  // hidden tabs were confounding tab-stop testing -- but it's a real editor
  // feature, so it lives on the bar, not buried. Fires a request callback;
  // Canvas owns the m_show_invisibles draw flag. set_can_focus(false) keeps it
  // out of the text edit's focus chain (the s338 cross-root-focus lesson).
  m_chip_invis = Gtk::make_managed<Gtk::ToggleButton>("\xC2\xB6");  // pilcrow label
  curvz::utils::set_name(m_chip_invis, "sty_invis", "style_bar_invisibles_toggle");
  m_chip_invis->set_has_frame(false);
  m_chip_invis->set_can_focus(false);
  m_chip_invis->set_tooltip_text("Show invisibles (tabs, spaces, paragraph marks)");
  m_chip_invis->add_css_class("curvz-style-chip");
  m_chip_invis->add_css_class("curvz-style-trigger");
  m_chip_invis->signal_toggled().connect([this]() {
    if (m_show_invisibles_request)
      m_show_invisibles_request(m_chip_invis->get_active());
  });
  append(*m_chip_invis);

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
  if (!m_chip_type) return;
  Glib::ustring next = mixed ? "\u2014"
                             : (resolved ? Glib::ustring(weight_face(weight))
                                         : "Type");
  if (m_chip_type->get_label() == next) return;  // unchanged -> no action
  m_chip_type->set_label(next);
}

void StyleBar::build_type_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop  = Gtk::make_managed<Gtk::Popover>();
  auto* book = Gtk::make_managed<Gtk::Notebook>();
  book->set_show_border(false);

  // ── Weight tab ─────────────────────────────────────────────────────────────
  {
    auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    col->set_margin(6);

    // Apply a weight: SET (not toggle) over the selection, update the chip face
    // to the new value's name (the bar's value-showing idiom), and close like a
    // menu pick.
    auto apply_weight = [this, chip, pop](long w) {
      if (m_format_set) m_format_set(kAttrWeight, w, "");
      chip->set_label(weight_face(w));  // "last look" face follows the pick
      pop->popdown();
    };

    // Preset stops — exclusive by nature: each SET replaces the last (the live
    // read of the selection's current weight is deferred update_state work).
    for (const auto& s : kWeightStops) {
      auto* b = Gtk::make_managed<Gtk::Button>(s.name);
      b->set_has_frame(false);
      b->set_can_focus(false);  // don't steal focus from the active text edit
      b->add_css_class("curvz-style-weight-stop");
      if (auto* lbl = dynamic_cast<Gtk::Label*>(b->get_child()))
        lbl->set_xalign(0.0f);
      long w = s.value;
      b->signal_clicked().connect([apply_weight, w]() { apply_weight(w); });
      col->append(*b);
    }

    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    sep->set_margin_top(4);
    sep->set_margin_bottom(4);
    col->append(*sep);

    // Custom numeric override — variable-font in-betweens (450, 575). Applied
    // on commit (Enter / activate) only, so stepping doesn't spray undo steps.
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

    book->append_page(*col, "Weight");
  }

  // ── Emphasis tab ───────────────────────────────────────────────────────────
  // Written-out rows (not single letters) matching the Weight stop list. Each
  // is a full-width left-aligned ToggleButton; the live-read pushes lit/off/
  // mixed via set_emphasis_state and m_suppress_emphasis stops that programmatic
  // set from re-firing the click handler.
  {
    auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
    col->set_margin(4);

    auto make_row = [&](const std::string& label) -> Gtk::ToggleButton* {
      auto* b = Gtk::make_managed<Gtk::ToggleButton>(label);
      b->set_has_frame(false);
      b->set_can_focus(false);  // don't steal focus from the active text edit
      b->add_css_class("curvz-style-trigger");
      b->add_css_class("curvz-style-emph-row");
      if (auto* l = dynamic_cast<Gtk::Label*>(b->get_child())) l->set_xalign(0.0);
      col->append(*b);
      return b;
    };

    // Independent decorations (additive set) — the toggle backend flips presence.
    auto add_decoration = [&](const std::string& label, const std::string& tip,
                              int attr, long value) -> Gtk::ToggleButton* {
      auto* b = make_row(label);
      b->set_tooltip_text(tip);
      b->signal_toggled().connect([this, attr, value]() {
        if (m_suppress_emphasis) return;  // programmatic state push, not a click
        if (m_format_toggle) m_format_toggle(attr, value, "");
      });
      return b;
    };

    m_emph_italic    = add_decoration("Italic", "Italic (Ctrl+I)",
                                      kAttrStyle,         kStyleItalic);
    m_emph_underline = add_decoration("Underline", "Underline (Ctrl+U)",
                                      kAttrUnderline,     kUnderlineSingle);
    m_emph_strike    = add_decoration("Strikethrough", "Strikethrough",
                                      kAttrStrikethrough, kStrikeOn);
    m_emph_overline  = add_decoration("Overline", "Overline",
                                      kAttrOverline,      kOverlineSingle);

    // Position pair — exclusive, font-metric shrink+shift; a thin rule sets them
    // apart. Each uses the value-SET path: on -> its scale, off -> NONE.
    {
      auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
      sep->set_margin_top(2);
      sep->set_margin_bottom(2);
      col->append(*sep);
    }
    auto add_scale = [&](const std::string& label, const std::string& tip,
                         long scale) -> Gtk::ToggleButton* {
      auto* b = make_row(label);
      b->set_tooltip_text(tip);
      b->signal_toggled().connect([this, b, scale]() {
        if (m_suppress_emphasis) return;
        if (m_format_set)
          m_format_set(kAttrFontScale,
                       b->get_active() ? scale : kFontScaleNone, "");
      });
      return b;
    };
    m_emph_super = add_scale("Superscript", "Superscript", kFontScaleSuper);
    m_emph_sub   = add_scale("Subscript",   "Subscript",   kFontScaleSub);

    book->append_page(*col, "Emphasis");
  }

  pop->set_child(*book);
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
  // s334 — keep the tracking-em -> letter-spacing-units conversion honest:
  // remember the last resolved single size so em scales against the text the
  // user is actually looking at.
  if (resolved && !mixed && pt > 0.0)
    m_size_ref_pt = pt;
}

void StyleBar::build_color_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop  = Gtk::make_managed<Gtk::Popover>();
  auto* book = Gtk::make_managed<Gtk::Notebook>();
  book->set_show_border(false);

  // ── Fill tab ─────────────────────────────────────────────────────────────
  // Per-run text fill rides PANGO_ATTR_FOREGROUND (a solid colour — Pango
  // foreground has no gradient/swatch form), so the colour-only picker is the
  // right surface. Every interactive edit SETS the foreground over the
  // selection; the set-path emits text_style_changed, so the swatch face
  // refreshes via the live-read (we don't write the face here). set_initial
  // (used by the live-read sync) does not emit signal_changed, so there is no
  // feedback loop; m_suppress_fill guards it anyway as belt-and-braces.
  {
    auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    col->set_margin(6);
    m_fill_picker = Gtk::make_managed<CurvzColorPicker>();
    m_fill_picker->set_with_alpha(false);  // Pango foreground is opaque RGB
    m_fill_picker->set_initial(color::Color(0.0, 0.0, 0.0, 1.0));
    m_fill_picker->signal_changed().connect([this](color::Color c) {
      if (m_suppress_fill) return;
      auto ch = [](double v) {
        return (long)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
      };
      long packed = (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
      if (m_format_set) m_format_set(kAttrForeground, packed, "");
    });
    col->append(*m_fill_picker);
    book->append_page(*col, "Fill");
  }

  // ── Stroke tab ─────────────────────────────────────────────────────────────
  // Object-level (Pango has no stroke attr). Colour picker SETS the per-run
  // stroke colour (kCurvzStrokeColorAttr); the set re-emits text_style_changed,
  // so the face/spin refresh via the live-read. set_initial doesn't emit, so
  // syncing from the live-read can't loop (m_suppress_stroke guards anyway).
  {
    auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    col->set_margin(6);

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
    // per-run width span (doc-px x PANGO_SCALE).
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

    // None — explicit no-stroke span over the selection (face shows the red-
    // slash; also suppresses the object stroke on those glyphs).
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

    book->append_page(*col, "Stroke");
  }

  pop->set_child(*book);
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

void StyleBar::set_leading(double pt, bool is_auto) {
  if (!m_leading_spin) return;
  m_leading_spin->set_tooltip_text(
      is_auto ? "Line spacing — Auto (from font metrics); type a value to pin it"
              : "Line spacing in points (type 2in, 10mm, 14pt, ...)");
  m_leading_spin->set_internal_value(UnitSystem::to_px(pt, Unit::Pt));  // no emit
}

// s332 — Alignment popover: Left / Centre / Right / Justify as a small icon
// row. Each requests Canvas::set_text_alignment(0/1/2/3), which paragraph-snaps
// and writes the kCurvzAlignAttr run. Justify (3) lands as the Pango justify
// post-pass in compute_text_layout (s333). The chip face mirrors the caret
// paragraph's alignment.
void StyleBar::build_align_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  col->set_margin(6);

  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 2);

  struct Opt { const char* icon; const char* tip; int val; };
  const Opt opts[] = {
      {"curvz-text-left-symbolic",      "Align left",   0},
      {"curvz-text-center-symbolic",    "Align centre", 1},
      {"curvz-text-right-symbolic",     "Align right",  2},
      {"curvz-text-justified-symbolic", "Justify",      3},
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
      // Justify keeps the popover open so its tuning knobs (below) are
      // reachable; L/C/R dismiss as before.
      if (val != 3) pop->popdown();
    });
    row->append(*b);
  }
  col->append(*row);

  // ── s334 — Justify tuning knobs, revealed only when Justify is active ──────
  // Migrated from the s333 temp bar row into the alignment popover (controls
  // live where the feature lives). Same two scales, same callback to MainWindow
  // (still the live g_justify_* globals; persisted node fields are a separate
  // step). The whole box is shown/hidden by alignment == 3.
  m_justify_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  {
    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    m_justify_box->append(*sep);

    auto mk_row = [&](const char* label, Gtk::Scale*& out_scale,
                      double lo, double hi, double step, double page,
                      int digits, double init, const char* tip) {
      auto* r = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
      auto* l = Gtk::make_managed<Gtk::Label>(label);
      l->set_xalign(0.0);
      l->set_size_request(46, -1);
      auto* sc = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
      sc->set_range(lo, hi);
      sc->set_increments(step, page);
      sc->set_value(init);
      sc->set_digits(digits);
      sc->set_draw_value(true);
      sc->set_size_request(120, -1);
      sc->set_tooltip_text(tip);
      r->append(*l);
      r->append(*sc);
      m_justify_box->append(*r);
      out_scale = sc;
    };

    Gtk::Scale* comfort = nullptr;
    Gtk::Scale* track   = nullptr;
    mk_row("Comfort", comfort, 0.0, 0.60, 0.01, 0.05, 2, 0.18,
           "Justify: extra space a word-gap may gain before letters help (em)");
    mk_row("Track",   track,   0.0, 0.20, 0.005, 0.02, 3, 0.05,
           "Justify: ceiling on letter-spacing (em)");

    auto push = [this, comfort, track]() {
      if (m_justify_knob_request)
        m_justify_knob_request(comfort->get_value(), track->get_value());
    };
    comfort->signal_value_changed().connect(push);
    track->signal_value_changed().connect(push);
  }
  m_justify_box->set_visible(false);
  col->append(*m_justify_box);

  // Re-evaluate the reveal each time the popover opens (alignment may have
  // changed via caret movement since it was last shown).
  pop->signal_show().connect([this]() {
    if (m_justify_box) m_justify_box->set_visible(m_cur_align == 3);
  });

  pop->set_child(*col);
  chip->set_popover(*pop);
}

void StyleBar::build_spacing_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  col->set_margin(8);

  // ── Character group: tracking (em) + rise (pt) ─────────────────────────────
  {
    auto* hdr = Gtk::make_managed<Gtk::Label>("Character");
    hdr->set_xalign(0.0f);
    hdr->add_css_class("dim-label");
    col->append(*hdr);
  }

  auto mk_row = [&](const char* label, Gtk::SpinButton*& out_spin,
                    double lo, double hi, double step, double page,
                    int digits, const char* tip) {
    auto* r = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto* l = Gtk::make_managed<Gtk::Label>(label);
    l->set_xalign(0.0);
    l->set_size_request(74, -1);
    auto adj = Gtk::Adjustment::create(0.0, lo, hi, step, page);
    auto* sp = Gtk::make_managed<Gtk::SpinButton>(adj, step, digits);
    sp->set_tooltip_text(tip);
    sp->set_width_chars(7);
    r->append(*l);
    r->append(*sp);
    col->append(*r);
    out_spin = sp;
  };

  // Tracking in em. Wide range on purpose (display work treats tracking as
  // placement). Apply: em -> PANGO_ATTR_LETTER_SPACING units against the
  // selection's resolved point size.
  Gtk::SpinButton* track_spin = nullptr;
  mk_row("Tracking (em)", track_spin, -50.0, 50.0, 0.01, 0.1, 2,
         "Letter-spacing across the selection, in em (scales with font size). "
         "Negative tightens/overlaps; positive spreads.");
  track_spin->signal_value_changed().connect([this, track_spin]() {
    if (!m_format_set) return;
    double em = track_spin->get_value();
    long units = std::lround(em * m_size_ref_pt * (double)PANGO_SCALE);
    m_format_set(kAttrLetterSpacing, units, "");
  });

  // Rise in pt (baseline displacement; positive = up). Apply: pt -> RISE units.
  Gtk::SpinButton* rise_spin = nullptr;
  mk_row("Rise (pt)", rise_spin, -500.0, 500.0, 0.5, 5.0, 1,
         "Baseline shift across the selection, in points. Positive raises.");
  rise_spin->signal_value_changed().connect([this, rise_spin]() {
    if (!m_format_set) return;
    double pt = rise_spin->get_value();
    long units = std::lround(pt * (double)PANGO_SCALE);
    m_format_set(kAttrRise, units, "");
  });

  {
    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    sep->set_margin_top(4);
    sep->set_margin_bottom(4);
    col->append(*sep);
  }

  // ── Line group: line spacing (pt) + Auto ───────────────────────────────────
  {
    auto* hdr = Gtk::make_managed<Gtk::Label>("Line");
    hdr->set_xalign(0.0f);
    hdr->add_css_class("dim-label");
    col->append(*hdr);
  }

  // Leading: buffer-global, so a change requests m_leading_request (not a span
  // op). The spin reuses the size chip's unit override (owns pt regardless of
  // doc unit).
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

  // Auto: hand leading back to the metric-derived default (signalled as pt = 0).
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

void StyleBar::build_insets_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop  = Gtk::make_managed<Gtk::Popover>();
  auto* book = Gtk::make_managed<Gtk::Notebook>();
  book->set_show_border(false);

  // ── Box tab: the four box margins (top/bottom/left/right) ──────────────────
  // Box-wide, on the boundary node. Doc-unit Distance spins (units parser);
  // internal value is doc-px, which is exactly what set_text_margin wants.
  {
    auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    col->set_margin(8);

    auto mk_mrg = [&](const char* name, CurvzSpinButton*& out) {
      auto* sp = Gtk::make_managed<CurvzSpinButton>(name, SpinType::Distance);
      sp->with_value(9.0)  // TEXT_DEFAULT_MARGIN look; the live-read corrects it
          ->with_width_chars(6)
          ->with_tooltip("Box margin — follows the document unit (type 12pt, 1in, 10mm, …)");
      sp->set_can_focus(true);
      out = sp;
    };
    mk_mrg("sty_mrg_t", m_mrg_top);
    mk_mrg("sty_mrg_b", m_mrg_bottom);
    mk_mrg("sty_mrg_l", m_mrg_left);
    mk_mrg("sty_mrg_r", m_mrg_right);

    // on_changed delivers doc-px; set_internal_value (live-read) is guarded
    // against emitting, so syncing never re-fires the apply.
    m_mrg_top->on_changed(
        [this](double px) { if (m_margin_request) m_margin_request(0, px); });
    m_mrg_bottom->on_changed(
        [this](double px) { if (m_margin_request) m_margin_request(1, px); });
    m_mrg_left->on_changed(
        [this](double px) { if (m_margin_request) m_margin_request(2, px); });
    m_mrg_right->on_changed(
        [this](double px) { if (m_margin_request) m_margin_request(3, px); });

    auto mrow = [&](const char* la, CurvzSpinButton* sa,
                    const char* lb, CurvzSpinButton* sb) {
      auto* r = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
      auto* la_l = Gtk::make_managed<Gtk::Label>(la);
      la_l->set_xalign(0.0); la_l->set_size_request(48, -1);
      auto* lb_l = Gtk::make_managed<Gtk::Label>(lb);
      lb_l->set_xalign(0.0); lb_l->set_size_request(48, -1);
      r->append(*la_l); r->append(*sa); r->append(*sa->get_unit_label());
      r->append(*lb_l); r->append(*sb); r->append(*sb->get_unit_label());
      col->append(*r);
    };
    mrow("Top",  m_mrg_top,  "Bottom", m_mrg_bottom);
    mrow("Left", m_mrg_left, "Right",  m_mrg_right);

    book->append_page(*col, "Box");
  }

  // ── Paragraph tab: per-paragraph indents (left / right / first-line) ───────
  {
  auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  col->set_margin(8);

  auto mk_spin = [&](const char* name) -> CurvzSpinButton* {
    // Indents are layout-domain, so they follow the DOCUMENT unit and accept
    // unit-tagged input via the math/units parser ("12pt", "1in", "10mm").
    // Distance = signed doc units (negative = hanging / leftward). Internal
    // value is doc-px, which is exactly what set_text_indent wants.
    auto* sp = Gtk::make_managed<CurvzSpinButton>(name, SpinType::Distance);
    sp->with_value(0.0)
        ->with_width_chars(6)
        ->with_tooltip("Indent — follows the document unit (type 12pt, 1in, 10mm, …)");
    sp->set_can_focus(true);
    return sp;
  };

  // Row 1: Left  [link]  Right — the link is the inspector's W/H lock idiom.
  auto* r1 = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* ll = Gtk::make_managed<Gtk::Label>("Left");
  ll->set_xalign(0.0); ll->set_size_request(34, -1);
  m_ind_left  = mk_spin("sty_ind_l");
  auto* lock  = Gtk::make_managed<Gtk::ToggleButton>();
  lock->set_icon_name("curvz-link-symbolic");
  lock->add_css_class("flat");
  lock->set_has_frame(false);
  lock->set_can_focus(false);
  lock->set_active(m_indent_locked);
  lock->set_opacity(m_indent_locked ? 1.0 : 0.3);
  lock->set_tooltip_text("Link left and right indent (keep them equal)");
  auto* rl = Gtk::make_managed<Gtk::Label>("Right");
  rl->set_xalign(0.0); rl->set_size_request(38, -1);
  m_ind_right = mk_spin("sty_ind_r");
  r1->append(*ll); r1->append(*m_ind_left); r1->append(*m_ind_left->get_unit_label());
  r1->append(*lock);
  r1->append(*rl); r1->append(*m_ind_right); r1->append(*m_ind_right->get_unit_label());
  col->append(*r1);

  // Row 2: First line + Clear (zeros all three indents).
  auto* r2 = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* fl = Gtk::make_managed<Gtk::Label>("First line");
  fl->set_xalign(0.0); fl->set_size_request(64, -1);
  m_ind_first = mk_spin("sty_ind_f");
  auto* clear = Gtk::make_managed<Gtk::Button>("Clear");
  clear->set_hexpand(true); clear->set_halign(Gtk::Align::END);
  clear->set_tooltip_text("Clear all indents on this paragraph");
  r2->append(*fl); r2->append(*m_ind_first); r2->append(*m_ind_first->get_unit_label());
  r2->append(*clear);
  col->append(*r2);

  lock->signal_toggled().connect([this, lock]() {
    m_indent_locked = lock->get_active();
    lock->set_opacity(m_indent_locked ? 1.0 : 0.3);
  });

  // on_changed delivers the value in doc-px. set_internal_value (used for the
  // locked mirror and the live-read) is guarded against emitting, so no
  // recursion guard is needed — but it also won't apply the sibling, so we fire
  // its request by hand when locked.
  m_ind_left->on_changed([this](double px) {
    if (m_indent_request) m_indent_request(0, px);
    if (m_indent_locked && m_ind_right) {
      m_ind_right->set_internal_value(px);
      if (m_indent_request) m_indent_request(1, px);
    }
  });
  m_ind_right->on_changed([this](double px) {
    if (m_indent_request) m_indent_request(1, px);
    if (m_indent_locked && m_ind_left) {
      m_ind_left->set_internal_value(px);
      if (m_indent_request) m_indent_request(0, px);
    }
  });
  m_ind_first->on_changed([this](double px) {
    if (m_indent_request) m_indent_request(2, px);
  });

  // Clear: zero all three (set_text_indent(_,0) clears each run), and reflect
  // the zeros in the spins without re-firing (set_internal_value is guarded).
  clear->signal_clicked().connect([this]() {
    if (m_ind_left)  m_ind_left->set_internal_value(0.0);
    if (m_ind_right) m_ind_right->set_internal_value(0.0);
    if (m_ind_first) m_ind_first->set_internal_value(0.0);
    if (m_indent_request) {
      m_indent_request(0, 0.0);
      m_indent_request(1, 0.0);
      m_indent_request(2, 0.0);
    }
  });

  book->append_page(*col, "Paragraph");
  }

  pop->set_child(*book);
  chip->set_popover(*pop);
}

// s343 — live-read sync of the Box-tab margin spins to the edited box (doc-px).
// Guarded by set_internal_value (no emit), mirroring set_indent_values.
void StyleBar::set_margin_values(double top_px, double bottom_px,
                                 double left_px, double right_px) {
  if (m_mrg_top)    m_mrg_top->set_internal_value(top_px);
  if (m_mrg_bottom) m_mrg_bottom->set_internal_value(bottom_px);
  if (m_mrg_left)   m_mrg_left->set_internal_value(left_px);
  if (m_mrg_right)  m_mrg_right->set_internal_value(right_px);
}

// s335 — live-read sync of the Indents spins to the caret paragraph (doc-px).
// Guarded by set_internal_value (no emit), so pushing values never re-fires the
// apply. Called from the text-style-changed relay alongside the other faces.
void StyleBar::set_indent_values(double left_px, double right_px, double first_px) {
  if (m_ind_left)  m_ind_left->set_internal_value(left_px);
  if (m_ind_right) m_ind_right->set_internal_value(right_px);
  if (m_ind_first) m_ind_first->set_internal_value(first_px);
}

// ── s335 — Tabs popover (master-detail) ───────────────────────────────────────
// Master: a scrolled ListBox of the paragraph's stops (sorted, "Type  pos").
// Detail: a position spin (doc units) + a four-way L/R/C/D type chooser that
// edit the selected row. Add / Remove / Remove-all mutate the list. Every
// mutation canonicalises m_tabs_spec and fires m_tabs_request. The list is
// (re)built from the cached spec on popover-show (set_tabs_state fills the
// cache from the caret paragraph on the text-style-changed relay).
void StyleBar::build_tabs_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  col->set_margin(8);

  // Master — the stop list.
  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_propagate_natural_width(false);  // s334 gotcha — scroll fires
  scroller->set_min_content_height(120);  // s339 — was 84; taller resting list
  scroller->set_max_content_height(300);  // s339 — was 168 (~6 rows); ~10 rows
                                          // now show before scroll kicks in
  scroller->set_size_request(248, -1);  // s339 — widened with the position spin
  scroller->set_hexpand(true);          // s339 — the type+leader dropdown row is
                                        // now the widest child; let the list fill
                                        // the popover width rather than sit at 248
  m_tabs_list = Gtk::make_managed<Gtk::ListBox>();
  m_tabs_list->set_selection_mode(Gtk::SelectionMode::SINGLE);
  // s338 — belt-and-braces with the per-row opt-out below: the ListBox is in
  // the popover's root, so it must not pull focus out of the main window's
  // text editor either. Selection is gesture-driven, not focus-driven.
  m_tabs_list->set_can_focus(false);
  scroller->set_child(*m_tabs_list);
  // (Appended at the BOTTOM of the column — controls sit on top, list below.)

  // Detail — position spin (follows the document unit, like the indent spins).
  auto* det = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* pl = Gtk::make_managed<Gtk::Label>("Position");
  pl->set_xalign(0.0); pl->set_size_request(56, -1);
  m_tabs_pos = Gtk::make_managed<CurvzSpinButton>("sty_tab_pos", SpinType::Distance);
  m_tabs_pos->with_value(48.0)
      ->with_width_chars(10)   // s339 — inches render at 6 decimals; 6 chars
                               // clipped values like 12.345678. 10 shows it whole.
      ->with_tooltip("Tab-stop position — follows the document unit (12pt, 1in, 10mm, …)");
  det->append(*pl); det->append(*m_tabs_pos); det->append(*m_tabs_pos->get_unit_label());
  col->append(*det);

  // Detail — type + leader, unified as matching dropdowns on one row. Both act
  // on the selected stop (master-detail), or stage the next Add when nothing is
  // selected. DropDowns (not the old toggle group) so the two read alike; this
  // is why the popover runs non-autohide (see the autohide note at attach) --
  // a DropDown's own popup breaks an autohide popover's grab (s155 house lesson).
  auto* tr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* tl = Gtk::make_managed<Gtk::Label>("Type");
  tl->set_xalign(0.0); tl->set_size_request(56, -1);
  auto type_model = Gtk::StringList::create({ "Left", "Right", "Centre", "Decimal" });
  m_tabs_type = Gtk::make_managed<curvz::widgets::DropDown>("sty_tab_type", type_model);
  m_tabs_type->set_can_focus(false);  // s338/s339 — stay out of the cross-root focus walk
  m_tabs_type->set_focusable(false);
  m_tabs_type->set_tooltip_text("Tab-stop alignment");

  auto* ll = Gtk::make_managed<Gtk::Label>("Leaders");
  ll->set_xalign(0.0); ll->set_size_request(56, -1); ll->set_margin_start(6);
  auto lead_model = Gtk::StringList::create({ "None", "Tight", "Normal", "Loose" });
  m_tabs_leader = Gtk::make_managed<curvz::widgets::DropDown>("sty_tab_lead", lead_model);
  m_tabs_leader->set_can_focus(false);
  m_tabs_leader->set_focusable(false);
  m_tabs_leader->set_tooltip_text("Dot leader filling the tab's gap (None = no leader)");

  // Seed Left / None so the controls compose a usable stop from the first open.
  m_tabs_loading = true;
  m_tabs_type->set_selected(0);
  m_tabs_leader->set_selected(0);
  m_tabs_loading = false;
  tr->append(*tl); tr->append(*m_tabs_type);
  tr->append(*ll); tr->append(*m_tabs_leader);
  col->append(*tr);

  // Buttons.
  auto* br = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* add = Gtk::make_managed<Gtk::Button>("Add");
  auto* rem = Gtk::make_managed<Gtk::Button>("Remove");
  auto* all = Gtk::make_managed<Gtk::Button>("Remove all");
  add->set_hexpand(true); rem->set_hexpand(true);
  br->append(*add); br->append(*rem); br->append(*all);
  col->append(*br);

  // Master — the stop list, below the controls.
  col->append(*scroller);

  // ── Wiring ────────────────────────────────────────────────────────────────
  // Rebuild from the cached spec each time the popover is shown. signal_map
  // fires on every popup (more reliable than signal_show for a menu-button
  // popover), so the list always reflects the caret paragraph's current stops.
  pop->signal_map().connect([this]() {
    m_tabs_sel = -1;
    rebuild_tabs_list();
    load_tabs_editor(-1);
  });

  m_tabs_list->signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
    if (m_tabs_loading) return;
    m_tabs_sel = row ? row->get_index() : -1;
    load_tabs_editor(m_tabs_sel);
  });

  // Position commit: update the selected stop, re-canonicalise (which re-sorts),
  // re-find the moved stop, fire, rebuild (restoring the selection).
  m_tabs_pos->on_changed([this](double px) {
    if (m_tabs_loading || m_tabs_sel < 0) return;
    auto stops = curvz::utils::parse_tab_spec(m_tabs_spec);
    if (m_tabs_sel >= (int)stops.size()) return;
    stops[(size_t)m_tabs_sel].pos = px;
    m_tabs_spec = curvz::utils::format_tab_spec(stops);
    m_tabs_sel  = reselect_after_sort(px);
    commit_tabs();
    schedule_tabs_refresh();   // s338 — defer teardown off the gesture dispatch
  });

  // Type dropdown — apply alignment to the selected stop.
  m_tabs_type->property_selected().signal_changed().connect([this]() {
    if (m_tabs_loading || m_tabs_sel < 0) return;
    auto stops = curvz::utils::parse_tab_spec(m_tabs_spec);
    if (m_tabs_sel >= (int)stops.size()) return;
    stops[(size_t)m_tabs_sel].type =
        (curvz::utils::TabAlign)(int)m_tabs_type->get_selected();
    m_tabs_spec = curvz::utils::format_tab_spec(stops);
    commit_tabs();
    schedule_tabs_refresh();   // s338 — defer teardown off the gesture dispatch
  });

  // Leader dropdown — apply dot-leader spacing to the selected stop.
  m_tabs_leader->property_selected().signal_changed().connect([this]() {
    if (m_tabs_loading || m_tabs_sel < 0) return;
    auto stops = curvz::utils::parse_tab_spec(m_tabs_spec);
    if (m_tabs_sel >= (int)stops.size()) return;
    stops[(size_t)m_tabs_sel].leader =
        (curvz::utils::TabLeader)(int)m_tabs_leader->get_selected();
    m_tabs_spec = curvz::utils::format_tab_spec(stops);
    commit_tabs();
    schedule_tabs_refresh();
  });

  add->signal_clicked().connect([this]() {
    auto stops = curvz::utils::parse_tab_spec(m_tabs_spec);
    // Compose the new stop from the live controls (position spin + type + leader).
    double pos = m_tabs_pos ? m_tabs_pos->get_internal_value() : 48.0;
    int ty = m_tabs_type   ? (int)m_tabs_type->get_selected()   : 0;
    int ld = m_tabs_leader ? (int)m_tabs_leader->get_selected() : 0;
    stops.push_back({ pos, (curvz::utils::TabAlign)ty, (curvz::utils::TabLeader)ld });
    m_tabs_spec = curvz::utils::format_tab_spec(stops);
    m_tabs_sel  = reselect_after_sort(pos);
    commit_tabs();
    schedule_tabs_refresh();   // s338 — defer teardown off the gesture dispatch
  });

  rem->signal_clicked().connect([this]() {
    if (m_tabs_sel < 0) return;
    auto stops = curvz::utils::parse_tab_spec(m_tabs_spec);
    if (m_tabs_sel >= (int)stops.size()) return;
    stops.erase(stops.begin() + m_tabs_sel);
    m_tabs_spec = curvz::utils::format_tab_spec(stops);
    if (m_tabs_sel >= (int)stops.size()) m_tabs_sel = (int)stops.size() - 1;
    commit_tabs();
    schedule_tabs_refresh();   // s338 — defer teardown off the gesture dispatch
  });

  all->signal_clicked().connect([this]() {
    m_tabs_spec.clear();
    m_tabs_sel = -1;
    commit_tabs();
    schedule_tabs_refresh();   // s338 — defer teardown off the gesture dispatch
  });

  pop->set_child(*col);
  // s339 — autohide OFF. The detail row now embeds Gtk::DropDowns, and a
  // dropdown's own popup closing breaks an autohide popover's grab (s155, hit
  // in WarpPopover; the house fix is non-autohide + explicit Esc, mirrored by
  // StepRepeatPopover). The MenuButton chip still toggles this popover open/
  // shut on click; Esc closes it too. Cost: an outside click no longer
  // dismisses it (unlike the autohide Indents / Line-spacing chips beside it).
  pop->set_autohide(false);
  auto key = Gtk::EventControllerKey::create();
  key->signal_key_pressed().connect(
      [pop](guint keyval, guint, Gdk::ModifierType) -> bool {
        if (keyval == GDK_KEY_Escape) { pop->popdown(); return true; }
        return false;
      }, false);
  pop->add_controller(key);
  chip->set_popover(*pop);
}

void StyleBar::rebuild_tabs_list() {
  if (!m_tabs_list) return;
  m_tabs_loading = true;
  // s337 m2b — BOUNDED clear. The old `while (get_first_child) remove` spun
  // forever on the popup Remove path: removing the selected/focused row while
  // the popover is mapped left get_first_child() returning a child that
  // remove() refused to detach (the gtk_widget_get_parent GTK-CRITICAL), so the
  // loop never made progress -- an un-catchable hang. Snapshot the children via
  // the sibling walk FIRST, then remove each in one forward pass: the loop is
  // over a fixed list, so it terminates regardless of whether any single
  // remove() no-ops, and we never re-query get_first_child mid-removal (which is
  // what the selection-change reorder confused).
  {
    std::vector<Gtk::Widget*> kids;
    for (Gtk::Widget* c = m_tabs_list->get_first_child(); c;
         c = c->get_next_sibling())
      kids.push_back(c);
    for (Gtk::Widget* c : kids)
      m_tabs_list->remove(*c);
    // s339 — s337 m2b clear-survivor diagnostic stripped; the popover hang it
    // was armed for is confirmed closed (s338). The snapshot-then-remove loop
    // above stays (it's the correct teardown, not just instrumentation).
  }
  auto stops = curvz::utils::parse_tab_spec(m_tabs_spec);
  const char* names[4] = { "Left", "Right", "Centre", "Decimal" };
  for (const auto& s : stops) {
    auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
    // s338 — keep rows OUT of the window's focus chain. GDB proved the hang:
    // clicking a row moves focus from the main window's GtkText (canvas editor)
    // into this row, which lives in the popover -- a SEPARATE GtkRoot. GTK's
    // synthesize_focus_change_events then walks UP from the row looking for a
    // common ancestor with the old focus (gtkwindow.c:5258); there is none
    // across two roots, so the walk runs gtk_widget_get_parent off the top of
    // the popover tree and trips GTK_IS_WIDGET. A click-to-select menu row has
    // no business taking keyboard focus -- opting it out removes the crossing
    // entirely. signal_row_selected still fires on click (selection != focus).
    row->set_focusable(false);
    row->set_can_focus(false);
    auto* hb  = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    hb->set_margin_start(6); hb->set_margin_end(6);
    hb->set_margin_top(2);   hb->set_margin_bottom(2);
    auto* ty = Gtk::make_managed<Gtk::Label>(names[(int)s.type]);
    ty->set_xalign(0.0); ty->set_size_request(56, -1);
    char buf[48];
    // s338 — honor the doc's display unit. Value via DocUnits' X length pump
    // (ruler_origin=0 -> pure scale, all display modes), which is exactly what
    // CurvzSpinButton::to_display(Distance) uses -- but called directly here so
    // we don't trip that method's per-call LOG_INFO once per row. The unit
    // suffix is read off the position spin's OWN unit label, so the row and the
    // editor spin can never show different units. Falls back to raw doc-px when
    // no model is wired yet (pre-first-doc-activate).
    double disp = m_doc_model
                      ? DocUnits::doc_to_display_x(s.pos, m_doc_model, 0.0)
                      : s.pos;
    std::string usuf = (m_tabs_pos && m_tabs_pos->get_unit_label())
                           ? std::string(m_tabs_pos->get_unit_label()->get_text())
                           : std::string();
    if (usuf.empty())
      std::snprintf(buf, sizeof(buf), "%.4g", disp);
    else
      std::snprintf(buf, sizeof(buf), "%.4g %s", disp, usuf.c_str());
    auto* po = Gtk::make_managed<Gtk::Label>(buf);
    po->set_xalign(1.0); po->set_hexpand(true);
    hb->append(*ty); hb->append(*po);
    row->set_child(*hb);
    m_tabs_list->append(*row);
  }
  m_tabs_loading = false;
  // s337 m2e — DEFER the row re-selection. Breadcrumbs proved rebuild_tabs_list
  // runs to END every time; the hang follows it ONLY when sel>=0, i.e. only when
  // select_row() is called -- and it runs synchronously inside the spin/button
  // signal dispatch that drove this rebuild, sending GTK into a layout/selection
  // loop with no re-entry into our code. Posting the selection to an idle runs it
  // on a clean stack after GTK settles the current dispatch, which breaks the
  // re-entrancy. Index captured by value; re-validated on fire (the list/spec may
  // have churned). m_tabs_loading guards the row-selected handler during the set.
  if (m_tabs_sel >= 0 && m_tabs_sel < (int)stops.size()) {
    const int want = m_tabs_sel;
    Glib::signal_idle().connect_once([this, want]() {
      if (!m_tabs_list) return;
      if (auto* r = m_tabs_list->get_row_at_index(want)) {
        m_tabs_loading = true;
        m_tabs_list->select_row(*r);
        m_tabs_loading = false;
      }
    });
  }
}

void StyleBar::load_tabs_editor(int index) {
  // The controls are ALWAYS usable: when a row is selected they edit it live;
  // with no selection they hold the staged "compose" values that Add reads. So
  // this never disables them — it only loads the selected row's values, or (no
  // selection) leaves the staged values intact, ensuring one type stays active.
  m_tabs_loading = true;
  auto stops = curvz::utils::parse_tab_spec(m_tabs_spec);
  const bool valid = (index >= 0 && index < (int)stops.size());
  if (valid) {
    if (m_tabs_pos) m_tabs_pos->set_internal_value(stops[(size_t)index].pos);
    if (m_tabs_type)
      m_tabs_type->set_selected((guint)(int)stops[(size_t)index].type);
    if (m_tabs_leader)
      m_tabs_leader->set_selected((guint)(int)stops[(size_t)index].leader);
  }
  // No selection: leave the staged type/leader/position as-is (Add reads them);
  // both dropdowns already hold a valid value (seeded Left/None at build).
  m_tabs_loading = false;
}

void StyleBar::commit_tabs() {
  if (m_tabs_request) m_tabs_request(m_tabs_spec);
}

// s338 — Post-commit list refresh, deferred to an idle. The mutating handlers
// (Add / Remove / Remove-all / pos / type) fire from a ListBox row's click
// gesture; rebuilding the list synchronously frees the very row whose gesture
// is still mid-dispatch, and the gesture's RELEASE phase then focus-walks into
// the freed row (GTK_IS_WIDGET assertion in gtk_widget_get_parent -> hang).
// Posting the teardown to an idle runs it on a clean stack, after the gesture
// has settled its focus against the live rows. The pending latch coalesces a
// burst of edits into a single rebuild; m_tabs_sel (a member) is read at
// fire-time, so the latest selection wins. The list may be gone by then (chip
// destroyed), so re-check m_tabs_list.
void StyleBar::schedule_tabs_refresh() {
  if (m_tabs_refresh_pending) return;
  m_tabs_refresh_pending = true;
  Glib::signal_idle().connect_once([this]() {
    m_tabs_refresh_pending = false;
    if (!m_tabs_list) return;
    rebuild_tabs_list();
    load_tabs_editor(m_tabs_sel);
  });
}

int StyleBar::reselect_after_sort(double pos) {
  auto stops = curvz::utils::parse_tab_spec(m_tabs_spec);
  int best = -1; double bestd = 1e18;
  for (size_t i = 0; i < stops.size(); ++i) {
    double d = std::fabs(stops[i].pos - pos);
    if (d < bestd) { bestd = d; best = (int)i; }
  }
  return best;
}

void StyleBar::set_tabs_state(const std::string& spec) {
  // Canonicalise so the cache matches what the apply path stores; the popover
  // reads this on show.
  m_tabs_spec = curvz::utils::format_tab_spec(curvz::utils::parse_tab_spec(spec));
}

// s338 — Borrow the active doc's CanvasModel so the layout-domain Distance
// spins (indents + tab-stop position) and the tab-stop row labels report in the
// doc's display unit. Re-applied on every doc-activate (pointer follows the
// active doc) and on display-unit change (same pointer, unit moved); set_model
// reconverts the displayed value either way. The tab-stop ROW labels are
// rebuilt so visible rows relabel into the new unit. Safe on a clean stack
// (callers are doc-activate / unit-change, never a gesture dispatch).
void StyleBar::set_doc_model(const CanvasModel* model) {
  m_doc_model = model;
  for (CurvzSpinButton* sp : { m_ind_left, m_ind_right, m_ind_first, m_tabs_pos })
    if (sp) sp->set_model(model);
  if (m_tabs_list) rebuild_tabs_list();
}

void StyleBar::set_align_face(int align) {
  m_cur_align = align;  // s334 — drives the justify-knob reveal on popover open
  if (m_justify_box) m_justify_box->set_visible(align == 3);
  if (!m_align_icon) return;
  const char* icon = (align == 1) ? "curvz-text-center-symbolic"
                   : (align == 2) ? "curvz-text-right-symbolic"
                   : (align == 3) ? "curvz-text-justified-symbolic"
                                  : "curvz-text-left-symbolic";
  m_align_icon->set_from_icon_name(icon);
}

// s333 TEMP — justify spill tuning row. Two horizontal scales (Comfort, Track)
// that drive the live g_justify_* knobs in TextCursor.cpp. Drag to taste; the
// canvas re-justifies on every value change. Init positions match the static
// defaults (0.18 / 0.05). Delete this whole method + its ctor call when the
// values are dialed in.
void StyleBar::set_emphasis_state(int italic, int underline, int strike,
                                  int overline, int superscript, int subscript) {
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
  apply(m_emph_super,     superscript);
  apply(m_emph_sub,       subscript);
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

// s341 — Style popover: the named-paragraph-style surface AND its manager. The
// list is rebuilt on each show from the provider (so library CRUD appears). A
// row activate binds; user rows carry a redefine (edit-from-paragraph) and a
// delete button; below, "Clear formatting" and a "New from paragraph" name
// field. Every CRUD action popdowns rather than rebuilding the ListBox in a
// click handler -- that synchronous row teardown is the s338 gesture-vs-freed-
// row hang; popdown sidesteps it and the next open shows the fresh list.
void StyleBar::build_style_popover(Gtk::MenuButton* chip) {
  if (!chip) return;

  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  box->set_margin(6);

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_min_content_height(260);
  scroller->set_min_content_width(230);

  auto* list = Gtk::make_managed<Gtk::ListBox>();
  list->set_selection_mode(Gtk::SelectionMode::NONE);  // activate, don't select
  list->add_css_class("curvz-style-list");
  scroller->set_child(*list);
  box->append(*scroller);

  box->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

  auto* clear_btn = Gtk::make_managed<Gtk::Button>("Clear formatting");
  clear_btn->set_tooltip_text(
      "Strip manual formatting so the applied style shows");
  box->append(*clear_btn);

  // s342 — New style… opens the blank field editor (the dialog path), beside
  // the from-paragraph capture below. Two New affordances on purpose: the
  // dialog builds a style field-by-field; the capture names the current
  // paragraph's look (Word/Pages "from selection").
  auto* new_dialog_btn = Gtk::make_managed<Gtk::Button>("New style…");
  new_dialog_btn->set_tooltip_text("Create a new style in the field editor");
  new_dialog_btn->signal_clicked().connect([this, pop]() {
    if (m_style_new_request) m_style_new_request();
    pop->popdown();
  });
  box->append(*new_dialog_btn);

  // New-from-paragraph: a name field + button. Capturing the current
  // paragraph's formatting is the "editor" -- no separate field dialog.
  auto* new_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
  auto* name_entry = Gtk::make_managed<Gtk::Entry>();
  name_entry->set_placeholder_text("New style from this ¶");
  name_entry->set_hexpand(true);
  auto* new_btn = Gtk::make_managed<Gtk::Button>("Capture");
  new_btn->set_tooltip_text("Create a style from this paragraph's formatting");
  new_row->append(*name_entry);
  new_row->append(*new_btn);
  box->append(*new_row);

  // Rows hold their entry; rebuilt on show, so a shared vector keeps the
  // row<->entry index map in sync without a header member.
  auto rows = std::make_shared<std::vector<StyleEntry>>();

  auto rebuild = [this, list, rows, pop]() {
    while (Gtk::Widget* c = list->get_first_child())
      list->remove(*c);
    rows->clear();

    std::vector<StyleEntry> entries;
    std::string current;
    if (m_style_list_provider) m_style_list_provider(entries, current);

    auto add_row = [&](const StyleEntry& e) {
      rows->push_back(e);
      auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
      auto* check = Gtk::make_managed<Gtk::Label>(e.id == current ? "\u2713" : "");
      check->set_width_chars(2);
      check->set_xalign(0.0f);
      auto* lbl = Gtk::make_managed<Gtk::Label>(e.name);
      lbl->set_xalign(0.0f);
      lbl->set_hexpand(true);
      row_box->append(*check);
      row_box->append(*lbl);
      // s342 — per-row actions. User rows: Edit… (dialog) / Update-to-match-
      // paragraph (the s341 capture convenience) / Delete. App rows: Edit a
      // copy… (duplicate-to-user then edit in the dialog). The Edit / Edit-a-
      // copy buttons popdown the popover so the dialog opens cleanly over it.
      if (!e.id.empty()) {
        if (e.app) {
          auto* editc = Gtk::make_managed<Gtk::Button>();
          editc->set_icon_name("edit-copy-symbolic");
          editc->set_has_frame(false);
          editc->set_tooltip_text("Edit a copy of this style…");
          const std::string cid = e.id;
          editc->signal_clicked().connect([this, cid, pop]() {
            if (m_style_edit_copy_request) m_style_edit_copy_request(cid);
            pop->popdown();
          });
          row_box->append(*editc);
        } else {
          auto* edit = Gtk::make_managed<Gtk::Button>();
          edit->set_icon_name("document-edit-symbolic");
          edit->set_has_frame(false);
          edit->set_tooltip_text("Edit this style…");
          const std::string eid = e.id;
          edit->signal_clicked().connect([this, eid, pop]() {
            if (m_style_edit_request) m_style_edit_request(eid);
            pop->popdown();
          });
          auto* redef = Gtk::make_managed<Gtk::Button>();
          redef->set_icon_name("view-refresh-symbolic");
          redef->set_has_frame(false);
          redef->set_tooltip_text("Update this style to match this paragraph");
          const std::string rid = e.id;
          redef->signal_clicked().connect([this, rid]() {
            if (m_style_redefine_request) m_style_redefine_request(rid);
          });
          auto* del = Gtk::make_managed<Gtk::Button>();
          del->set_icon_name("user-trash-symbolic");
          del->set_has_frame(false);
          del->set_tooltip_text("Delete this style");
          const std::string did = e.id;
          del->signal_clicked().connect([this, did, pop]() {
            if (m_style_delete_request) m_style_delete_request(did);
            pop->popdown();  // list changed; reopen rebuilds (no in-handler teardown)
          });
          row_box->append(*edit);
          row_box->append(*redef);
          row_box->append(*del);
        }
      }
      row_box->set_margin_start(6);
      row_box->set_margin_end(6);
      row_box->set_margin_top(2);
      row_box->set_margin_bottom(2);
      list->append(*row_box);
    };

    add_row({"", "Unstyled", true});       // unbind row (treated read-only)
    for (const auto& e : entries) add_row(e);
  };

  pop->signal_show().connect(rebuild);

  list->signal_row_activated().connect(
      [this, chip, pop, rows](Gtk::ListBoxRow* row) {
        int i = row->get_index();
        if (i < 0 || i >= (int)rows->size()) return;
        const StyleEntry& e = (*rows)[i];
        if (m_style_apply_request) m_style_apply_request(e.id);
        chip->set_label(e.id.empty() ? "Style" : Glib::ustring(e.name));
        pop->popdown();
      });

  clear_btn->signal_clicked().connect([this, pop]() {
    if (m_style_clear_request) m_style_clear_request();
    pop->popdown();
  });

  auto do_create = [this, pop, name_entry]() {
    if (m_style_create_request)
      m_style_create_request(name_entry->get_text().raw());
    name_entry->set_text("");
    pop->popdown();
  };
  new_btn->signal_clicked().connect(do_create);
  name_entry->signal_activate().connect(do_create);  // Enter in the field

  pop->set_child(*box);
  chip->set_popover(*pop);
}

void StyleBar::set_style_face(const Glib::ustring& name, bool resolved) {
  if (!m_chip_style) return;
  Glib::ustring next = (resolved && !name.empty()) ? name : "Style";
  if (m_chip_style->get_label() == next) return;  // unchanged -> no action
  m_chip_style->set_label(next);
}

} // namespace Curvz
