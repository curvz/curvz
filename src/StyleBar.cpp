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
#include "UnitSystem.hpp"   // s331 — Unit::Pt + px<->pt for the Size chip

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/searchentry.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/listbox.h>
#include <gtkmm/listboxrow.h>
#include <pango/pango.h>        // PangoAttrType / weight consts
#include <pango/pangocairo.h>   // pango_cairo_font_map_get_default
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
  m_chip_para = add_label_chip("sty_para", "style_bar_linespacing_chip",
                               "Line spacing", "Line spacing");
  m_chip_style = add_label_chip("sty_named", "style_bar_style_chip",
                                "Body", "Named paragraph style (placeholder)");

  // Wire the live popovers after all chips exist.
  build_weight_popover(m_chip_weight);
  build_emphasis_popover(m_chip_emphasis);
  build_font_popover(m_chip_font);
  build_size_popover(m_chip_size);
  build_paragraph_popover(m_chip_para);

  // Stub popovers for the chips whose real popups aren't built yet.
  stub_popover(m_chip_fill,   "Fill colour picker");
  stub_popover(m_chip_stroke, "Stroke colour + width");
  stub_popover(m_chip_align,  "Left / Center / Right / Justified");
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
