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

  // The one live axis in m1: weight & decoration. B/I/U wired to the backend;
  // strike / overline / smallcaps / case are present but disabled placeholders.
  void build_weight_popover(Gtk::MenuButton* chip);

  FormatToggle m_format_toggle;

  // Persistent chips — built once, never rebuilt.
  Gtk::MenuButton* m_chip_font   = nullptr;  // character
  Gtk::MenuButton* m_chip_size   = nullptr;
  Gtk::MenuButton* m_chip_weight = nullptr;
  Gtk::MenuButton* m_chip_fill   = nullptr;
  Gtk::MenuButton* m_chip_stroke = nullptr;
  Gtk::MenuButton* m_chip_align  = nullptr;  // paragraph
  Gtk::MenuButton* m_chip_para   = nullptr;
  Gtk::MenuButton* m_chip_style  = nullptr;
};

} // namespace Curvz
