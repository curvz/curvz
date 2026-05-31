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
#include <vector>

namespace Curvz {

// Pango payloads, named here so the call sites read like the s326 hotkey path
// (which passes the real PANGO_* enums). Kept local so the header stays
// pango-free per its contract.
namespace {
constexpr int kAttrWeight = PANGO_ATTR_WEIGHT;

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
  m_chip_para = add_icon_chip("sty_para", "style_bar_paragraph_chip",
                              "\u00B6", "Paragraph: leading / spacing (placeholder)",
                              /*face_is_icon=*/false);
  m_chip_style = add_label_chip("sty_named", "style_bar_style_chip",
                                "Body", "Named paragraph style (placeholder)");

  // Wire the live popovers after all chips exist.
  build_weight_popover(m_chip_weight);
  build_emphasis_popover(m_chip_emphasis);
  build_font_popover(m_chip_font);

  // Stub popovers for the chips whose real popups aren't built yet.
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

void StyleBar::build_emphasis_popover(Gtk::MenuButton* chip) {
  if (!chip) return;
  auto* pop = Gtk::make_managed<Gtk::Popover>();
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
  box->set_margin(6);

  // Each decoration is an independent toggle over the selection (additive set,
  // not exclusive) routed through the s326 toggle backend. Momentary triggers
  // for now: the void callback can't report on/off, so lit/mixed face state is
  // the deferred update_state work — better no lit state than a lying one.
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
      b->set_sensitive(false);  // gated until its span consumers learn the case
    }
    box->append(*b);
  };

  add_trigger("I", "Italic (Ctrl+I)",       kAttrStyle,         kStyleItalic,     true);
  add_trigger("U", "Underline (Ctrl+U)",    kAttrUnderline,     kUnderlineSingle, true);
  add_trigger("S", "Strikethrough",          kAttrStrikethrough, kStrikeOn,        true);
  add_trigger("O", "Overline",               kAttrOverline,      kOverlineSingle,  true);

  pop->set_child(*box);
  chip->set_popover(*pop);
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
