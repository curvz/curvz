#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// StyleBar — s329 — the floating character/paragraph formatting bar.
//
// A thin horizontal strip that floats above the textbox being edited (in the
// canvas text overlay, the same Gtk::Fixed that hosts the text-entry). It is
// constructed ONCE and lives for the process — chips and their popovers are
// built in the constructor and never rebuilt; only visibility (set_visible)
// and, later, chip face state change. This is deliberate: GTK is slow to
// reclaim torn-down widget trees, and a strip whose STRUCTURE is fixed (always
// the same chips) and whose STATE varies (lit / off / mixed as the selection
// moves) wants to be built-once-update-state, not rebuilt. That is the
// difference from ContextBar, which legitimately rebuilds because its
// structure varies per tool.
//
// Organization (left→right), grouped by scope with a separator at the seam:
//
//   CHARACTER / SELECTION          ‖   PARAGRAPH
//   Font  Size  Weight  Fill  Stroke ‖  Align  Paragraph  Style
//
// Each chip is ONE button whose popover holds that axis's options; the chip's
// face shows the axis's current value (its "last look"). Font / Size / Style
// are label-faced (a name or number can only read as text); the rest are
// glyph/swatch-faced. The popup pattern is what keeps the bar thin: the breadth
// lives in the popovers, the bar holds one collapsed button per axis.
//
// s329 m1 (this rough-in): the bar stands up, floats above the box, shows on
// edit / hides otherwise. The Weight popover is wired LIVE to the character-
// formatting backend (s326 apply_text_format_toggle) to prove B/I/U end-to-end.
// Every other chip is a placeholder face + stub popover; icons and real popups
// land as the design fans out. Indent + tabs are intentionally absent — they
// live on the on-canvas tab bar and in the named styles, not here.
//
// Units note (carried from the design conversation): type-domain values
// (size, leading) default to pt/pica; layout-domain values follow the document
// unit; every spinner runs the math parser so any unit is typeable. Not yet
// realised in m1 (placeholder faces), but the Size chip face will read with its
// unit ("12 pt"), never a bare number, because the bar mixes regimes.
// ─────────────────────────────────────────────────────────────────────────────

#include <gtkmm/box.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/togglebutton.h>
#include <functional>
#include <string>

namespace Curvz {

class StyleBar : public Gtk::Box {
public:
  StyleBar();

  // Canvas wires this at overlay-setup. The bar invokes it when a character-
  // axis toggle fires. attr_type is a PangoAttrType cast to int (so this
  // header stays pango-free); ivalue / svalue are the attr payload. The
  // backend is selection-only (no-op when nothing is selected) — matches the
  // s326 Ctrl-B/I/U behaviour; the caret-pending-format case is deferred.
  using FormatToggle = std::function<void(int attr_type, long ivalue,
                                          const std::string& svalue)>;
  void set_format_toggle(FormatToggle cb) { m_format_toggle = std::move(cb); }

  // s330 — the value-set counterpart. Weight (and later size / family / fill)
  // are value axes: picking a stop SETS the attribute, it doesn't toggle it.
  // Canvas wires this to apply_text_format_set. Same signature as the toggle;
  // separate so the call sites read in the model's own terms (set vs toggle)
  // and route to the right backend.
  using FormatSet = std::function<void(int attr_type, long ivalue,
                                       const std::string& svalue)>;
  void set_format_set(FormatSet cb) { m_format_set = std::move(cb); }

  // s330 — live face setters, driven by Canvas's text-style-changed signal
  // (relayed by MainWindow). Family: the name, the mixed marker if the
  // selection spans more than one family, or the axis name "Font" when
  // unresolved (empty). Weight: the named stop, the mixed marker, or "Weight"
  // when unresolved. The faces are read-only summaries; the popovers write.
  void set_font_face(const Glib::ustring& family, bool mixed);
  void set_weight_face(long weight, bool resolved, bool mixed);

  // s329 m1 placeholder. Later: push the effective format of the current
  // selection into the chip faces (lit / off / mixed) WITHOUT rebuilding —
  // re-read range_has_attr per axis and restyle the live chips. Stubbed now.
  // void update_state(const std::vector<AttrSpan>& spans, unsigned a, unsigned b);

private:
  // One chip = one MenuButton (button + attached popover). Label-faced for
  // text values, icon-faced for glyphs. The popover is built once here.
  Gtk::MenuButton* add_label_chip(const std::string& abbrev,
                                  const std::string& long_name,
                                  const std::string& initial_label,
                                  const std::string& tip);
  Gtk::MenuButton* add_icon_chip(const std::string& abbrev,
                                 const std::string& long_name,
                                 const std::string& icon_or_face,
                                 const std::string& tip,
                                 bool face_is_icon);
  Gtk::Separator* add_sep();

  // Attach a minimal placeholder popover so the chip opens something (the
  // "this axis lives here" structure is visible even before the real popup).
  void stub_popover(Gtk::MenuButton* chip, const std::string& axis_label);

  // s330 — the Weight axis as a value picker (the first "attribute-shape is
  // tool-shape" instance): Pango weight is a 100..900 number, so the popover
  // is preset stops (Thin..Black) + a numeric override, exclusive-set via
  // apply_text_format_set. Bold is simply the 700 stop — not a separate B
  // toggle. Italic / underline / strike / overline are NOT weight; they move
  // to the Emphasis chip in the next pass (Ctrl-I/U keep working meanwhile).
  void build_weight_popover(Gtk::MenuButton* chip);

  // s330 — Emphasis: the line-decoration toggles that are NOT weight. Pango
  // treats these as independent attributes that combine (an additive set, vs
  // weight's exclusive scale), so they're toggles, not a picker. Italic
  // (STYLE), underline (UNDERLINE), strikethrough (STRIKETHROUGH) and overline
  // (OVERLINE) are all live on the span toggle path, with strikethrough/
  // overline cases now present in all three consumers (build_line_attrs
  // render / encode_markup save / decode_markup_into parse). Lit/mixed face
  // state is the deferred update_state work — the toggles are momentary
  // triggers for now, since the void m_format_toggle can't report the
  // resulting on/off and an optimistic lit state would lie on mixed runs.
  void build_emphasis_popover(Gtk::MenuButton* chip);

  // s330 — Font family: a chip whose popover holds a search box + a scrolled
  // list of every family the Pango font map reports, sorted. Picking a row
  // SETS family (PANGO_ATTR_FAMILY via the value-set path; already wired through
  // build_line_attrs / encode_markup / decode_markup_into) and writes the name
  // onto the chip face. Per-row typeface previews and live face-follows-
  // selection are deferred enhancements. Built once like the other popovers.
  void build_font_popover(Gtk::MenuButton* chip);

  FormatToggle m_format_toggle;
  FormatSet    m_format_set;

  // Persistent chips — built once, never rebuilt.
  Gtk::MenuButton* m_chip_font   = nullptr;  // character
  Gtk::MenuButton* m_chip_size   = nullptr;
  Gtk::MenuButton* m_chip_weight = nullptr;
  Gtk::MenuButton* m_chip_emphasis = nullptr;  // italic/underline/strike/overline
  Gtk::MenuButton* m_chip_fill   = nullptr;
  Gtk::MenuButton* m_chip_stroke = nullptr;
  Gtk::MenuButton* m_chip_align  = nullptr;  // paragraph
  Gtk::MenuButton* m_chip_para   = nullptr;
  Gtk::MenuButton* m_chip_style  = nullptr;
};

} // namespace Curvz
