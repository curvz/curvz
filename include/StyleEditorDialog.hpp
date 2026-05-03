#pragma once
//
// StyleEditorDialog — modal multi-field editor for a style::Style.
//
// S85 m4i-cont-1. The reusable Style Editor that pops over MainWindow
// when the user invokes:
//   * StylesPanel kebab → "New style"            (Mode::New)
//   * StylesPanel right-click on user row → "Edit…"        (Mode::Edit)
//   * StylesPanel right-click on app   row → "Edit a copy…" (Mode::Duplicate)
//
// The dialog edits a single Style in isolation — no selection awareness,
// no broadcast, no coalesced undo. OK pushes one atomic command (Add or
// Update); Cancel discards. The host wires the command and the library-
// changed signal at the call site (see StylesPanel where the dialog is
// instantiated).
//
// Field roster (all bound to a working std::optional<style::Style> on OK):
//
//   ── Identity ──
//   * name           — CurvzEntry. Empty-on-OK falls back to the style
//                      library's auto-name machinery (NOT the dialog's
//                      job — the library's add path handles it).
//   * category       — Gtk::DropDown of existing user categories +
//                      "(uncategorised)" + "New category…" sentinel.
//                      Selecting the sentinel reveals an inline
//                      CurvzEntry next to the dropdown for free text.
//                      Mirrors the StylesPanel kebab "New category…"
//                      flow but inline rather than as a separate
//                      prompt; lets a single OK cover identity+category
//                      change without a second dialog.
//
//   ── Fill ──
//   * fill paint     — PaintEditor. The dialog owns its own
//                      ColorPickerPopover for this slot.
//
//   ── Stroke ──
//   * stroke paint   — PaintEditor. Second ColorPickerPopover member
//                      (one-instance-per-caller-widget rule).
//   * stroke width   — CurvzSpinButton (SpinType::Width).
//   * cap            — Row of three Gtk::ToggleButton (Butt / Round /
//                      Square), set_group()-tied as a radio. Icons:
//                      curvz-cap-{butt,round,square}-symbolic.
//   * join           — Row of three Gtk::ToggleButton (Miter / Round /
//                      Bevel), same idiom. Icons:
//                      curvz-join-{miter,round,bevel}-symbolic.
//
//   ── Shadow (S98) ──
//   * enable         — Gtk::CheckButton. Drives the sensitive() state
//                      of every other shadow widget — disabled section
//                      stays visible-but-greyed so a user can iterate.
//   * dx, dy         — Two CurvzSpinButton (SpinType::Distance) for
//                      offset in document units. Y-down convention
//                      (positive dy = below). Unit labels rendered
//                      next to each spin.
//   * blur           — CurvzSpinButton (SpinType::Width, non-negative)
//                      for Gaussian stddev in doc units.
//   * colour         — Click-to-pick swatch DrawingArea + colour-alpha
//                      Gtk::Scale slider on the same row. Mirrors the
//                      inspector's idiom; uses m_shadow_popover for
//                      the picker session (with_alpha=false — alpha is
//                      the slider's job).
//   * opacity        — Gtk::Scale 0..100 mapped to 0..1. Final shadow
//                      strength multiplier; multiplies with colour
//                      alpha at render.
//
// Removed in S87 m1:
//   * miter limit input — field still lives on StrokeAppearance (and
//                         is honoured by the renderer at its default
//                         value of 4.0); just no UI to edit it.
//   * dash entry / dash offset / align dropdown — fields removed
//                         entirely from StrokeAppearance + JSON.
//                         They had been specced for a future phase
//                         but never reached the renderer.
//
// What's NOT here in v1:
//
//   * SwatchRef binding UI inside the dialog. PaintEditor in this
//     codebase emits FillStyle (None/CurrentColor/Solid only); a
//     SwatchRef on the incoming style is preserved across the dialog
//     IFF the user doesn't touch the fill paint. Any colour-change /
//     type-toggle clears the binding (break-on-override, mirroring
//     the inspector's S83 m4h v2 rule). The "(Vivid Red)" annotation
//     on PaintEditor still renders as a read-only readout while the
//     binding survives. Setting a binding from inside the dialog
//     (drag from Swatches, or a "From swatch…" subcontrol) is post-
//     S85.
//
// Idempotency guards on every CurvzEntry::on_commit per the s84 m4i
// fix6 lesson — focus-leave during dialog destruction or programmatic
// refresh fires on_commit, and an unguarded handler will write back
// the displayed value even when the user didn't touch it. Each
// entry's commit handler compares parsed value against the dialog's
// own "last known" cache before mutating.
//

#include "CurvzEntry.hpp"
#include "CurvzSpinButton.hpp"
#include "ColorPickerPopover.hpp"
#include "GradientDialog.hpp"
#include "PaintEditor.hpp"
#include "style/Style.hpp"

#include <gtkmm/adjustment.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <functional>
#include <string>
#include <vector>

namespace Curvz {

// Forward declarations — keep header lean.
struct CanvasModel;
namespace color { class SwatchLibrary; }

class StyleEditorDialog {
public:
    enum class Mode {
        // Edit an existing user-tier style. OK returns the modified
        // Style with its original id intact; caller pushes
        // UpdateStyleCommand.
        Edit,

        // Create a new style from scratch. Initial Style passed by the
        // caller carries appearance defaults but its header.id is
        // ignored (StyleLibrary mints one on add). OK returns the
        // styled-up value with empty header.id; caller pushes
        // AddStyleCommand.
        New,

        // Edit a copy of an app-tier style. Pre-fills the dialog with
        // the app style's appearance + " copy" suffix on the name.
        // Identical OK path to New (caller pushes AddStyleCommand);
        // discriminant exists for the title-bar text and for any
        // future copy-flow polish (e.g. category choice carry-over).
        Duplicate,
    };

    using CommittedFn = std::function<void(style::Style result)>;

    // Construct + show.
    //
    // The dialog owns itself: present() is called at the end of the
    // ctor, and the dialog destroys itself on close (Cancel button,
    // window close, or after on_committed fires from OK).
    //
    // Parameters:
    //   parent        — transient-for. Required; the dialog refuses to
    //                   present without it.
    //   sw_lib        — for resolving SwatchRef bindings into display
    //                   names on the PaintEditor binding annotation.
    //                   Borrowed; must outlive the dialog.
    //   canvas_model  — for the stroke-width / dash-offset spinbuttons'
    //                   unit conversion. Optional (nullptr = px-only,
    //                   no unit conversion); defensive against being
    //                   called before a document is open. Borrowed.
    //   user_categories — pre-computed list of existing user-tier
    //                   category strings (StyleLibrary::user_categories).
    //                   Empty string for "(uncategorised)" is added by
    //                   the dialog itself; callers don't need to pass
    //                   it. Borrowed for the lifetime of the ctor only.
    //   mode          — see enum.
    //   initial       — the starting Style. For Edit, the existing
    //                   style; for New, a default-constructed Style
    //                   with the caller's preferred initial category
    //                   (typically the panel's m_active_category) and
    //                   an auto-name; for Duplicate, the source style
    //                   with " copy" already appended to the name.
    //   on_committed  — fires once on OK with the result Style.
    //                   Caller pushes the appropriate Add/Update
    //                   command and emits library-changed.
    //
    // No on_cancelled callback — Cancel just closes the dialog. Hosts
    // that need to know about cancel can bind on the window's
    // signal_close_request or rely on "no commit fired".
    StyleEditorDialog(Gtk::Window& parent,
                      const color::SwatchLibrary& sw_lib,
                      CanvasModel* canvas_model,
                      const std::vector<std::string>& user_categories,
                      Mode mode,
                      style::Style initial,
                      CommittedFn on_committed);

    // The dialog manages its own lifetime (self-deleting on close
    // via the standard Gtk::Window managed pattern). Public API is
    // construct-and-forget; no further interaction needed.

private:
    // ── Build helpers (called once from ctor) ─────────────────────────
    void build();
    void build_identity_section(Gtk::Box& root);
    void build_fill_section(Gtk::Box& root);
    void build_stroke_section(Gtk::Box& root);
    void build_shadow_section(Gtk::Box& root);  // S98
    void build_button_row(Gtk::Box& root);

    // ── PaintEditor wiring helpers ────────────────────────────────────
    //
    // Both the fill and stroke editors share the same handler shape:
    // type-change, colour-change, unbind. Each operates on a different
    // slot in m_working. Factored to a single helper that takes a
    // FillStyle& and a SwatchRef binding-id reference.
    void wire_paint_editor(PaintEditor& editor,
                           color::Paint& target_paint,
                           bool is_stroke);
    PaintEditor::RenderState compute_render_state(const color::Paint& p) const;

    // ── Action handlers ───────────────────────────────────────────────
    void on_ok();
    void on_cancel();

    // Pull all field values into m_working. Called from on_ok before
    // emitting on_committed. Field handlers also call this incrementally,
    // but the OK path is defensive — focus-leave on a partly-edited
    // entry might not have committed yet.
    void harvest_into_working();

    // ── State ─────────────────────────────────────────────────────────
    Gtk::Window*                     m_window      = nullptr;  // self-managed
    const color::SwatchLibrary&      m_sw_lib;
    CanvasModel*                     m_canvas_model = nullptr;
    Mode                             m_mode;
    style::Style                     m_working;
    CommittedFn                      m_on_committed;

    // Carried alongside m_working.fill / m_working.stroke.paint —
    // the SwatchRef ids preserved across edits as long as the user
    // doesn't touch the paint. These mirror SceneNode's separate
    // fill_swatch_id / stroke_swatch_id strings; on commit, if a slot
    // still has a non-empty id, the corresponding paint is rebuilt
    // as Paint{SwatchRef{id, fallback=current colour}}.
    std::string m_fill_binding_id;
    std::string m_stroke_binding_id;

    // ── Widgets (persistent for re-render) ────────────────────────────
    CurvzEntry*       m_name_entry        = nullptr;
    Gtk::DropDown*    m_category_dd       = nullptr;
    CurvzEntry*       m_category_new_entry = nullptr;  // visible iff
                                                       // dropdown sits on the
                                                       // "New category…"
                                                       // sentinel
    std::vector<std::string> m_category_order;        // parallel to dd

    PaintEditor*      m_fill_editor       = nullptr;
    PaintEditor*      m_stroke_editor     = nullptr;

    CurvzSpinButton*  m_stroke_width_sp   = nullptr;

    // S87 m1: cap/join migrated from Gtk::DropDown to a row of three
    // Gtk::ToggleButton instances each, set_group()-tied as a radio.
    // Mirrors the toolbar's Stroke popover idiom (`m_cap_butt_btn` etc.)
    // and the Properties Panel inspector. Symbolic icons (curvz-cap-*-
    // symbolic, curvz-join-*-symbolic) provided by the GResource bundle.
    Gtk::ToggleButton* m_cap_butt_btn     = nullptr;
    Gtk::ToggleButton* m_cap_round_btn    = nullptr;
    Gtk::ToggleButton* m_cap_square_btn   = nullptr;
    Gtk::ToggleButton* m_join_miter_btn   = nullptr;
    Gtk::ToggleButton* m_join_round_btn   = nullptr;
    Gtk::ToggleButton* m_join_bevel_btn   = nullptr;

    Gtk::Button*      m_btn_ok            = nullptr;

    // ── Shadow widgets (S98) ──────────────────────────────────────────
    // build_shadow_section caches the widgets it needs to address from
    // harvest_into_working and from the enable-checkbox sensitivity-slave
    // lambda. Same pattern as the stroke spin pointer above. Colour itself
    // (rgb) is written back to m_working.shadow.color_* directly inside
    // the popover's apply callback; only the slider widgets and the
    // swatch DrawingArea are tracked here.
    Gtk::CheckButton* m_shadow_enable_chk  = nullptr;
    CurvzSpinButton*  m_shadow_dx_sp       = nullptr;
    CurvzSpinButton*  m_shadow_dy_sp       = nullptr;
    CurvzSpinButton*  m_shadow_blur_sp     = nullptr;
    Gtk::DrawingArea* m_shadow_swatch      = nullptr;
    Gtk::Scale*       m_shadow_color_a_sl  = nullptr;
    Gtk::Scale*       m_shadow_opacity_sl  = nullptr;
    Glib::RefPtr<Gtk::Adjustment> m_shadow_color_a_adj;
    Glib::RefPtr<Gtk::Adjustment> m_shadow_opacity_adj;

    // The dialog owns its three popovers so each colour-picker caller has
    // its own session. Held by value, attach()-ed in build(). S98: the
    // shadow popover is the third — the inspector's pattern.
    ColorPickerPopover m_fill_popover;
    ColorPickerPopover m_stroke_popover;
    ColorPickerPopover m_shadow_popover;

    // S93 m3: GradientDialog instance for in-dialog gradient editing.
    // Long-lived member; show() reseeds per click. Mirrors MainWindow's
    // m_gradient_dialog pattern. Hosted locally (rather than reusing
    // MainWindow's) so the dialog stays self-contained — no signal has
    // to bubble up through StylesPanel to reach the MainWindow instance.
    // Transient-for is set to *m_window on each open, so the modal stack
    // becomes MainWindow → StyleEditorDialog → GradientDialog.
    GradientDialog m_gradient_dialog;

    // Re-entry guard for programmatic widget updates. Mirrors PaintEditor's
    // m_syncing — set true while build() is wiring initial values so the
    // commit handlers don't fire writebacks during construction.
    bool              m_syncing = false;
};

} // namespace Curvz
