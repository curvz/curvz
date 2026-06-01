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
#include <gtkmm/drawingarea.h>
#include <gtkmm/image.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/togglebutton.h>
#include <functional>
#include <string>

namespace Curvz {

class CurvzSpinButton;  // s331 — held by the Size popover (fwd decl, pt-locked)
class CurvzColorPicker; // s332 — embedded in the Fill popover (fwd decl)

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

  // s331 — far-right Reset button. Strips all per-run formatting on the
  // active text and returns it to defaults (wired to Canvas::
  // reset_text_to_default). Lives on the bar because the bar IS the text-
  // formatting surface, and unlike the right-click path it's always visible
  // while editing and needs no action-group lookup.
  using ResetRequest = std::function<void()>;
  void set_reset_request(ResetRequest cb) { m_reset_request = std::move(cb); }

  // s331 — Leading (line height). Buffer-global, so it's a request, not a span
  // toggle/set: the arg is points, with pt <= 0 meaning auto (metric-derived).
  // Wired to Canvas::set_text_leading.
  using LeadingRequest = std::function<void(double pt)>;
  void set_leading_request(LeadingRequest cb) { m_leading_request = std::move(cb); }

  // s332 — Alignment (per-paragraph): 0=left, 1=centre, 2=right. Wired to
  // Canvas::set_text_alignment.
  using AlignRequest = std::function<void(int align)>;
  void set_align_request(AlignRequest cb) { m_align_request = std::move(cb); }

  // s330 — live face setters, driven by Canvas's text-style-changed signal
  // (relayed by MainWindow). Family: the name, the mixed marker if the
  // selection spans more than one family, or the axis name "Font" when
  // unresolved (empty). Weight: the named stop, the mixed marker, or "Weight"
  // when unresolved. The faces are read-only summaries; the popovers write.
  void set_font_face(const Glib::ustring& family, bool mixed);
  void set_weight_face(long weight, bool resolved, bool mixed);

  // s331 — Size face + popover spin sync. pt is the chip's unit (the spin is
  // pt-locked via CurvzSpinButton's unit override, independent of the doc
  // unit). Face reads "12 pt" / "—" (mixed) / "Size" (unresolved). When
  // resolved, the popover spin is set to match (no apply re-fired).
  void set_size_face(double pt, bool resolved, bool mixed);

  // s332 — Fill face + popover sync. The chip is named ("Fill") like the rest,
  // with a small colour square prefixed in front of the name. The square shows
  // the effective text colour under the cursor: a solid fill, a red diagonal
  // slash when the fill is None/transparent, or a two-tone diagonal split when
  // the selection spans more than one colour. rgb is packed 0xRRGGBB. When
  // resolved & single, the embedded picker is synced to match (no apply
  // re-fired — set_initial does not emit signal_changed).
  void set_fill_face(unsigned long rgb, bool resolved, bool mixed, bool is_none);

  // s332 — Stroke face + popover sync. Like the Fill chip: named ("Stroke")
  // with a small square in front, but the square is drawn as an outline ring
  // in the stroke colour (a stroke reads as an edge, not a fill), or the red
  // none-slash when there's no visible stroke. rgb is packed 0xRRGGBB;
  // has_color=false draws the slash. The width arg syncs the popover spin
  // (points). When has_color, the embedded picker is synced to match.
  void set_stroke_face(unsigned long rgb, bool has_color);
  void set_stroke_width(double pt);

  // s331 — sync the Paragraph popover's Leading spin to the current effective
  // leading (points). is_auto only annotates the tooltip; the spin shows the
  // value either way. No apply re-fired (set_internal_value is guarded).
  void set_leading(double pt, bool is_auto);
  // s332 — reflect the caret paragraph's alignment on the chip face.
  void set_align_face(int align);

  // s331 — push the selection's per-decoration lit state into the emphasis
  // popover toggles. Each arg is a tri-state: 0 = off everywhere,
  // 1 = on everywhere, 2 = mixed (rendered with GTK's inconsistent look).
  // Order: italic / underline / strikethrough / overline. Guarded internally
  // so the programmatic set doesn't re-fire the toggle handlers (which would
  // re-apply the format). The popover toggles are the bar's first chip that
  // both reads and writes; the EMPHASIS chip face stays the static word.
  void set_emphasis_state(int italic, int underline, int strike, int overline);

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

  // s331 — Size: a pt-locked editable combo. A CurvzSpinButton with the unit
  // override pinned to points (steppers nudge, math/units parser accepts
  // "2in" -> 144 pt and commits) over a column of common presets (6..72).
  // Picking or committing SETS PANGO_ATTR_SIZE in point units (round-trips
  // through the markup `size` wire form) via the value-set path. The face
  // and the spin both follow the selection on the live-read.
  void build_size_popover(Gtk::MenuButton* chip);

  // s332 — Fill: a chip whose face is a colour swatch and whose popover hosts
  // the CurvzColorPicker. Per-run text fill rides PANGO_ATTR_FOREGROUND (a
  // solid colour — Pango foreground has no gradient/swatch form), so the
  // colour-only picker is the right surface; picking SETS the foreground over
  // the selection via the value-set path. The swatch face follows the
  // selection on the live-read (set_fill_face). Built once like the others.
  void build_fill_popover(Gtk::MenuButton* chip);

  // s332 — Stroke: object-level (Pango has no stroke attr). The popover hosts
  // the CurvzColorPicker for the stroke colour, a pt-locked width spin, and a
  // None button to clear the stroke. Picking a colour / changing the width
  // routes to the object-stroke requests; the square face follows the edited
  // node on the live-read. Built once like the others.
  void build_stroke_popover(Gtk::MenuButton* chip);

  // s331 — Paragraph popover (the ¶ chip). First control: Leading — a pt-locked
  // CurvzSpinButton (steppers + units parser, reusing the size chip's unit
  // override) plus an Auto button that hands leading back to the metric default.
  // Space-before/after and the rest of the paragraph set come later.
  void build_paragraph_popover(Gtk::MenuButton* chip);
  void build_align_popover(Gtk::MenuButton* chip);  // s332

  // s330 — Emphasis: the line-decoration toggles that are NOT weight. Pango
  // treats these as independent attributes that combine (an additive set, vs
  // weight's exclusive scale), so they're toggles, not a picker. Italic
  // (STYLE), underline (UNDERLINE), strikethrough (STRIKETHROUGH) and overline
  // (OVERLINE) are all live on the span toggle path, with strikethrough/
  // overline cases now present in all three consumers (build_line_attrs
  // render / encode_markup save / decode_markup_into parse). s331 — the
  // triggers became ToggleButtons that READ the selection too: the live-read
  // pushes per-decoration lit / off / mixed state via set_emphasis_state, and
  // a click toggles. The format-apply re-emits text_style_changed, so the
  // resulting state flows straight back onto the toggle (the click's own
  // active value isn't authoritative — the re-read corrects it).
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
  ResetRequest m_reset_request;  // s331 — far-right Reset button callback
  LeadingRequest m_leading_request;  // s331 — Paragraph Leading callback
  AlignRequest   m_align_request;    // s332 — Alignment callback

  // Persistent chips — built once, never rebuilt.
  Gtk::MenuButton* m_chip_font   = nullptr;  // character
  Gtk::MenuButton* m_chip_size   = nullptr;  Gtk::MenuButton* m_chip_weight = nullptr;
  Gtk::MenuButton* m_chip_emphasis = nullptr;  // italic/underline/strike/overline
  Gtk::MenuButton* m_chip_fill   = nullptr;
  Gtk::MenuButton* m_chip_stroke = nullptr;
  Gtk::MenuButton* m_chip_align  = nullptr;  // paragraph
  Gtk::Image*      m_align_icon   = nullptr;  // s332 — chip prefix = current alignment
  Gtk::MenuButton* m_chip_para   = nullptr;
  Gtk::MenuButton* m_chip_style  = nullptr;

  // s331 — the four emphasis popover toggles, held so the live-read can push
  // their lit/off/mixed state in. m_suppress_emphasis guards the programmatic
  // set so it doesn't re-fire signal_toggled and re-apply the format.
  Gtk::ToggleButton* m_emph_italic    = nullptr;
  Gtk::ToggleButton* m_emph_underline = nullptr;
  Gtk::ToggleButton* m_emph_strike    = nullptr;
  Gtk::ToggleButton* m_emph_overline  = nullptr;
  bool m_suppress_emphasis = false;

  // s331 — the Size popover's pt-locked spin, held so the live-read can sync
  // it to the selection without re-firing the apply.
  CurvzSpinButton* m_size_spin = nullptr;
  // s331 — the Paragraph popover's pt-locked Leading spin (held for live-sync).
  CurvzSpinButton* m_leading_spin = nullptr;

  // s332 — Fill chip. The chip is named ("Fill") like the rest; a small
  // colour square (DrawingArea) is prefixed in front of the name in the chip's
  // child box, and its draw func reads the cached state below. m_fill_picker is
  // the embedded colour picker, held so the live-read can sync its colour.
  // m_suppress_fill guards that programmatic sync from re-firing the picker's
  // changed handler.
  Gtk::DrawingArea* m_fill_face   = nullptr;
  CurvzColorPicker* m_fill_picker = nullptr;
  double m_fill_r = 0.0, m_fill_g = 0.0, m_fill_b = 0.0;  // resolved colour
  bool   m_fill_resolved = false;  // an edit is active and a colour resolved
  bool   m_fill_mixed    = false;  // selection spans more than one colour
  bool   m_fill_none     = false;  // effective fill is None/transparent
  bool   m_suppress_fill = false;

  // s332 — Stroke chip. Same shape as Fill (named chip + square prefix), but
  // the square draws as an outline ring (a stroke reads as an edge). Per-run
  // (selection-scoped) stroke set rides the generic m_format_set span path
  // under the Curvz-private stroke attrs. Picker + width spin held for the
  // live-read sync; m_suppress_stroke guards the programmatic picker sync.
  Gtk::DrawingArea* m_stroke_face       = nullptr;
  CurvzColorPicker* m_stroke_picker     = nullptr;
  CurvzSpinButton*  m_stroke_width_spin = nullptr;
  double m_stroke_r = 0.0, m_stroke_g = 0.0, m_stroke_b = 0.0;
  bool   m_stroke_has   = false;  // a visible stroke colour is present
  bool   m_suppress_stroke = false;
};

} // namespace Curvz
