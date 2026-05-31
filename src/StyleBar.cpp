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

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <pango/pango.h>   // PangoAttrType / weight / style / underline consts

namespace Curvz {

// Pango payloads, named here so the call sites read like the s326 hotkey path
// (which passes the real PANGO_* enums). Kept local so the header stays
// pango-free per its contract.
namespace {
constexpr int kAttrWeight    = PANGO_ATTR_WEIGHT;
constexpr int kAttrStyle     = PANGO_ATTR_STYLE;
constexpr int kAttrUnderline = PANGO_ATTR_UNDERLINE;
constexpr long kWeightBold     = 700;                 // PANGO_WEIGHT_BOLD
constexpr long kStyleItalic    = 2;                   // PANGO_STYLE_ITALIC
constexpr long kUnderlineSingle = 1;                  // PANGO_UNDERLINE_SINGLE
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
                               "Sans", "Font family (placeholder)");
  m_chip_size = add_label_chip("sty_size", "style_bar_size_chip",
                               "12 pt", "Font size (placeholder)");
  // Weight & decoration — the one LIVE axis in m1. Face is a placeholder "A".
  m_chip_weight = add_icon_chip("sty_wt", "style_bar_weight_chip",
                                "A", "Weight & decoration (B/I/U live)",
                                /*face_is_icon=*/false);
  m_chip_fill = add_label_chip("sty_fill", "style_bar_fill_chip",
                               "Fill", "Text fill colour (placeholder)");
  m_chip_stroke = add_label_chip("sty_strk", "style_bar_stroke_chip",
                                 "Stroke", "Text stroke + width (placeholder)");

  add_sep();  // the character/paragraph scope seam, made visible

  // ── Paragraph scope ──────────────────────────────────────────────────────
  // Alignment has a real glyph already in the icon set.
  m_chip_align = add_icon_chip("sty_aln", "style_bar_align_chip",
                               "curvz-align-left-symbolic",
                               "Justification (placeholder)",
                               /*face_is_icon=*/true);
  m_chip_para = add_icon_chip("sty_para", "style_bar_paragraph_chip",
                              "\u00B6", "Paragraph: leading / spacing (placeholder)",
                              /*face_is_icon=*/false);
  m_chip_style = add_label_chip("sty_named", "style_bar_style_chip",
                                "Body", "Named paragraph style (placeholder)");

  // Wire the one live popover after all chips exist.
  build_weight_popover(m_chip_weight);

  // Stub popovers for every placeholder chip so they open *something*.
  stub_popover(m_chip_font,   "Font family chooser");
  stub_popover(m_chip_size,   "Size stepper + presets");
  stub_popover(m_chip_fill,   "Fill colour picker");
  stub_popover(m_chip_stroke, "Stroke colour + width");
  stub_popover(m_chip_align,  "Left / Center / Right / Justified");
  stub_popover(m_chip_para,   "Leading / space before-after / anchor / hyphenation");
  stub_popover(m_chip_style,  "Style chooser + override + verbs");
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

void StyleBar::build_weight_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
  box->set_margin(6);

  // Pure-trigger buttons (split-button dropped, per the settled design): each
  // click toggles that attribute over the current selection via the backend.
  // The backend decides on/off from the selection state, so these stay
  // momentary triggers, not state mirrors — the lit/mixed face is the deferred
  // update_state work.
  auto add_trigger = [&](const std::string& face, const std::string& tip,
                         int attr, long value, bool live) {
    auto* b = Gtk::make_managed<Gtk::Button>(face);
    b->set_tooltip_text(tip);
    b->set_has_frame(false);
    b->set_can_focus(false);  // don't steal focus from the active text edit
    b->add_css_class("curvz-style-trigger");
    if (live) {
      b->signal_clicked().connect([this, attr, value]() {
        if (m_format_toggle) m_format_toggle(attr, value, "");
      });
    } else {
      b->set_sensitive(false);  // placeholder for a not-yet-wired decoration
    }
    box->append(*b);
  };

  add_trigger("B",  "Bold (Ctrl+B)",        kAttrWeight,    kWeightBold,      true);
  add_trigger("I",  "Italic (Ctrl+I)",      kAttrStyle,     kStyleItalic,     true);
  add_trigger("U",  "Underline (Ctrl+U)",   kAttrUnderline, kUnderlineSingle, true);
  add_trigger("S",  "Strikethrough (soon)", 0, 0, false);
  add_trigger("O",  "Overline (soon)",      0, 0, false);
  add_trigger("ab", "Small caps (soon)",    0, 0, false);
  add_trigger("Aa", "Case (soon)",          0, 0, false);

  pop->set_child(*box);
  chip->set_popover(*pop);
}

} // namespace Curvz
