#pragma once
//
// PaintEditor — reusable widget that bundles the input paths to editing
// a paint's *colour* and *swatch binding*, plus the read-back surfaces
// that show the current state.
//
// Inputs the user can drive:
//   1. Type toggle    — Solid / None / currentColor / Swatch / Gradient
//                       (changes the paint's kind, not its colour)
//   2. Hex entry      — type a hex value to set the colour directly
//   3. Swatch click   — opens ColorPickerPopover for visual picking
//   4. Swatch picker  — palette dropdown + chip grid below the colour
//                       row, visible only when type==Swatch. Clicking
//                       a chip emits signal_swatch_picked(id).
//   5. Gradient row   — horizontal ramp preview + "Edit…" button,
//                       visible only when paint type is a gradient.
//                       Clicking Edit emits signal_gradient_edit_
//                       requested(current_fill).
//
// Read-backs that stay in sync with the model:
//   5. Swatch chip preview — shows the current colour, or a diagonal-
//                            stripe pattern when uniform=false
//   6. Binding annotation  — "(Vivid Red)" italic when bound, with the
//                            host-supplied display name
//   7. × unbind button     — visible only while a swatch binding is
//                            active for the slot
//
// Ownership / wiring model:
//
//   The host owns the ColorPickerPopover and passes a reference at
//   construction (PropertiesPanel already holds one as a member;
//   StyleEditorDialog holds its own). PaintEditor never opens a picker
//   without going through that shared popover, so picker focus
//   semantics stay consistent across the app.
//
//   The host computes selection-aware uniformity, swatch-binding name
//   resolution, and per-slot type policy, then pushes a RenderState
//   into the widget via set_render_state(). PaintEditor itself has no
//   notion of selection. It does, however, take a non-owning pointer
//   to the SwatchLibrary in RenderState (S85: swatch-pick milestone)
//   to render the palette dropdown + chip grid when type==Swatch. Hosts
//   that don't want the Swatch type — guide / grid / margin colour
//   pickers — leave RenderState.library nullptr and the Swatch button
//   greys out.
//
//   On user action, PaintEditor emits one of five signals. The host is
//   responsible for the actual mutation, the undo command, and the
//   sibling broadcast. After the host has updated its model, it calls
//   set_render_state() again to sync the widget.
//
// This split lets the inspector's per-slot multi-select / coalesced-
// undo wiring stay in PropertiesPanel where the surrounding code lives,
// while the StyleEditorDialog reuses the same widget with trivially
// simpler wiring (one Style, no selection, single UpdateStyleCommand
// on OK).
//

#include "ColorPickerPopover.hpp"
#include "SceneNode.hpp"  // FillStyle
#include "CurvzEntry.hpp"
#include "color/Paint.hpp"    // SwatchId
#include "color/Palette.hpp"  // PaletteId

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/label.h>
#include <gtkmm/togglebutton.h>
#include <sigc++/signal.h>

#include <string>
#include <vector>

namespace Curvz {

// Forward declare to keep SwatchLibrary include out of the header. We
// only hold a pointer; consumers that need to push a library into the
// editor already include the SwatchLibrary header to read it.
namespace color {
class SwatchLibrary;
}

class PaintEditor : public Gtk::Box {
public:
    // RenderState — the full snapshot the host pushes to the widget.
    // Construct, populate, set_render_state(). Calling set_render_state
    // with a fresh RenderState rebuilds nothing structural — only label
    // texts, toggle states, swatch draw funcs, dropdown selection, and
    // visibility get adjusted. The picker section's chip grid IS torn
    // down and rebuilt when the active palette changes (the cells are
    // managed; cheap), but the dropdown widget itself stays put.
    struct RenderState {
        // Current paint state to show. When type != Solid, the colour
        // row hides itself unless `uniform` is false (a mixed selection
        // can include Solids alongside other types — we keep the row
        // visible so the user can click the chip to snap everyone to
        // Solid + the picked colour).
        FillStyle paint{};

        // Multi-select uniformity. false → diagonal-stripe chip + empty
        // hex entry with "mixed" placeholder. The widget treats this as
        // an opaque flag; the host computes it via its own selection
        // helpers.
        bool uniform = true;

        // Whether the picker should expose the alpha slider when opened
        // from this editor. Object fill/stroke uses false (alpha lives
        // on the stroke separately); guide / grid / margin colours use
        // true. The widget caches this and reuses it on every picker
        // open until the next set_render_state.
        bool has_alpha = false;

        // Binding annotation. When `bound` is false, the annotation
        // shows the unbound region-name fallback derived from the
        // colour itself. The host must always supply that fallback in
        // `unbound_display_name` when paint.type == Solid; the widget
        // will not call into ColorRegion itself (the host might want
        // to use a different naming source for some consumers).
        bool        bound = false;
        bool        bound_mixed = false; // multiple targets bound to different ids
        std::string bound_display_name;  // ignored when bound_mixed
        std::string bound_tooltip;

        // Region-name fallback used when bound is false and paint.type
        // is Solid. Empty hides the annotation.
        std::string unbound_display_name;

        // ── S85 swatch-pick fields ────────────────────────────────────
        //
        // Whether the editor's *active toggle* is the Swatch type. Distinct
        // from `bound` (a display flag): a paint can be `bound` to a swatch
        // and still have its toggle on Solid (the inspector's pre-S85
        // surface); switching the toggle to Swatch is what reveals the
        // picker section below.
        //
        // When uniform=false, the widget ignores this field and shows no
        // toggle as active, same as the existing mixed-paint behaviour.
        bool is_swatch_active = false;

        // The id currently bound to this slot, if any. Used by the picker
        // section to highlight the active chip and pre-select its palette
        // in the dropdown. Empty means no binding (the toggle may still be
        // on Swatch — the user has chosen the type but not yet picked an
        // id, e.g. just switched from Solid).
        std::string binding_id;

        // Non-owning pointer to the SwatchLibrary the picker draws from.
        // Null disables the Swatch toggle button (button is shown but
        // greyed out — discoverability without the picker burden for
        // hosts that don't wire one). Lifetime: the host guarantees the
        // library outlives the widget, or pushes a fresh RenderState
        // with library=nullptr before tearing it down.
        const color::SwatchLibrary* library = nullptr;

        // ── S91 gradient surface ────────────────────────────────────────
        //
        // Gates the Gradient type toggle + gradient row. Inspector and
        // Toolbar set this true; the StyleEditorDialog leaves it false
        // (the color::Paint variant doesn't have a gradient arm yet, so
        // the Style Manager can't round-trip a gradient through its OK
        // button — that's a separate workstream, see S91 follow-on).
        //
        // false → Gradient toggle is greyed out, gradient row never
        //         renders even if paint.is_gradient() somehow. Mirrors
        //         the library==nullptr gating on the Swatch toggle.
        bool gradients_enabled = false;
    };

    // Pass the shared popover by reference. Caller owns it; PaintEditor
    // only borrows. The popover must already be attach()-ed before the
    // widget tries to open() it (caller's responsibility — typically
    // the host attaches in its own ctor).
    explicit PaintEditor(ColorPickerPopover& popover);
    ~PaintEditor() override = default;

    // Push a fresh RenderState. Call after construction (the widget is
    // not visible until the first call) and after every host-side
    // model change. Cheap — no widget tree mutation, just attribute
    // updates on persistent children, plus a chip-grid rebuild when the
    // active palette changes.
    void set_render_state(const RenderState& s);

    // ── Outgoing signals ──────────────────────────────────────────────
    //
    // Each fires on a single user action. The widget does not mutate
    // its own RenderState in response — the host is expected to update
    // its model and call set_render_state() to bring the widget back
    // in sync. This keeps the widget purely a view.

    // User clicked one of the four type toggles. The widget guards
    // against re-entry from set_render_state() programmatic updates,
    // so this fires only for genuine user clicks.
    using TypeChangedSignal = sigc::signal<void(FillStyle::Type)>;
    TypeChangedSignal& signal_type_changed() { return m_sig_type_changed; }

    // User edited the colour, either via hex-entry Return or via the
    // picker (live-drag callbacks AND the on_closed final value
    // arrive through this signal). The host treats every emit as
    // "the colour is now this" and applies it.
    //
    // Currently the alpha channel of the underlying FillStyle is not
    // round-tripped through this signal — fill/stroke object paint
    // ignores alpha (carried separately on stroke), and the guide/
    // grid/margin paths use ColorPickerPopover directly rather than
    // this widget. If a future host wants alpha through PaintEditor
    // we'll widen the signature; for now the simpler RGB triple
    // matches every consumer.
    using ColorChangedSignal = sigc::signal<void(double r, double g, double b)>;
    ColorChangedSignal& signal_color_changed() { return m_sig_color_changed; }

    // User clicked the × button. The host clears its binding and
    // pushes the appropriate UnbindSwatchCommand. The × is shown
    // only when RenderState.bound is true.
    using UnbindClickedSignal = sigc::signal<void()>;
    UnbindClickedSignal& signal_unbind_clicked() { return m_sig_unbind_clicked; }

    // S85: user clicked a swatch chip in the picker section. The host
    // treats every emit as "bind this slot to this id" — for the
    // inspector that means routing through Canvas::apply_swatch_to_
    // selection (which builds a BindSwatchCommand for the whole
    // selection); for the StyleEditorDialog it means stashing the id
    // in m_*_binding_id and updating the working Paint to a SwatchRef.
    //
    // The widget does NOT change its own toggle state in response —
    // the host calls set_render_state() with is_swatch_active=true
    // and binding_id=picked, which both refreshes the chip ring and
    // keeps the type toggle on Swatch.
    using SwatchPickedSignal = sigc::signal<void(color::SwatchId)>;
    SwatchPickedSignal& signal_swatch_picked() { return m_sig_swatch_picked; }

    // Picker session ended after popdown. committed=true on Return /
    // pick-recent / outside-click; false on Esc. Most hosts can
    // ignore this signal; the SwatchesPanel-style create-on-Esc-
    // remove flow uses it.
    using PickerClosedSignal = sigc::signal<void(bool committed)>;
    PickerClosedSignal& signal_picker_closed() { return m_sig_picker_closed; }

    // S91: user clicked the "Edit…" button in the gradient row. The host
    // opens its GradientDialog seeded with the current fill, and on
    // Apply writes the edited FillStyle back through its commit path
    // (mutate_appearance for the inspector, m_def_fill = … for the
    // Toolbar). The widget itself does not open a dialog — keeping it
    // a pure view, mirroring how it doesn't open ColorPickerPopover by
    // itself either (the host wires the popover at construction).
    //
    // The emitted FillStyle is m_last.paint — the same one the host
    // pushed via set_render_state. Hosts pass it forward to
    // GradientDialog::show as the seed.
    using GradientEditRequestedSignal = sigc::signal<void(FillStyle)>;
    GradientEditRequestedSignal& signal_gradient_edit_requested() {
        return m_sig_gradient_edit_requested;
    }

private:
    // Helpers to wall off programmatic state-set vs user-driven
    // signals, and to keep the colour row + binding annotation
    // re-renders adjacent.
    void apply_type_active(FillStyle::Type t, bool is_swatch);
    void apply_color_row(const RenderState& s);
    void apply_binding_annotation(const RenderState& s);
    void apply_picker_section(const RenderState& s);
    // S91: refresh the gradient ramp + Edit button visibility / contents.
    // Visible iff gradients_enabled && uniform && paint.is_gradient().
    // Repaints m_gradient_ramp using the current stops.
    void apply_gradient_row(const RenderState& s);

    // S85: chip-grid build for the active palette. Tears down the prior
    // grid (managed children) and rebuilds. Lifted in spirit from
    // SwatchesPanel::build_chip_flow + make_chip but with simpler
    // semantics (left-click only, no right-click menu, no apply-to-
    // selection — just emit signal_swatch_picked).
    void rebuild_chip_grid(const color::SwatchLibrary& lib,
                           const std::vector<color::SwatchId>& ids,
                           const std::string& active_binding_id);

    // S85: rebuild palette dropdown's StringList model from the library.
    // Picks the palette containing `binding_id` if any, else the
    // library's active_palette(), else the first palette. m_picker_
    // palette_ids tracks the dropdown's order so selection-changed
    // resolves index → id without a name lookup.
    void rebuild_palette_dropdown(const color::SwatchLibrary& lib,
                                  const std::string& binding_id);

    // Hex parse / format — pure helpers, no member access.
    static bool parse_hex(const std::string& hex,
                          double& r, double& g, double& b);
    static std::string format_hex(double r, double g, double b);

    ColorPickerPopover& m_popover;

    // Type toggle row (Solid / None / currentColor / Swatch / Gradient).
    // Persistent members so set_render_state can re-set their active
    // state and visibility without rebuilding the tree.
    Gtk::Box           m_type_row{Gtk::Orientation::HORIZONTAL};
    Gtk::ToggleButton  m_btn_solid{"Solid"};
    Gtk::ToggleButton  m_btn_none{"None"};
    Gtk::ToggleButton  m_btn_cc{"currentColor"};
    Gtk::ToggleButton  m_btn_swatch{"Swatch"};
    Gtk::ToggleButton  m_btn_gradient{"Gradient"};

    // Colour row: [swatch][hex entry][binding label][×]
    Gtk::Box           m_color_row{Gtk::Orientation::HORIZONTAL};
    Gtk::DrawingArea   m_swatch;
    CurvzEntry         m_hex_entry;
    Gtk::Label         m_bind_lbl;
    Gtk::Button        m_unbind_btn;

    // S91 Gradient row: [ramp preview ─────][Edit…]
    //
    // Visible only when paint type is a gradient AND gradients_enabled is
    // true. The ramp preview is a plain horizontal strip painted with
    // the working stops via Cairo::LinearGradient — even when the type
    // is RadialGradient, we paint a 1D ramp here because the read is
    // "ordering and colour transitions of stops"; the actual radial
    // shape lives on-canvas. Same call as GradientDialog's draw_track.
    //
    // The ramp is non-interactive (no click handlers); Edit is the
    // affordance for opening the editor. Click-on-ramp-opens-editor is
    // a possible future polish but adds discoverability ambiguity ("is
    // it a swatch I can drag, or a button I can click?").
    Gtk::Box           m_gradient_row{Gtk::Orientation::HORIZONTAL};
    Gtk::DrawingArea   m_gradient_ramp;
    Gtk::Button        m_btn_edit_gradient{"Edit…"};

    // S85 Picker section (palette dropdown + chip grid). Visible only
    // when the Swatch toggle is active AND the host wired a library.
    Gtk::Box       m_picker_section{Gtk::Orientation::VERTICAL};
    Gtk::DropDown* m_palette_dd = nullptr;       // owned by m_picker_section
    Gtk::FlowBox*  m_chip_flow  = nullptr;       // owned by m_picker_section
    Gtk::Label     m_picker_empty;               // shown when library has no swatches

    // Shadow vector tracking the dropdown's row order so selection-
    // changed maps row index → palette id without a name lookup. Plus
    // a sentinel value for the synthetic "All" pseudo-palette that
    // walks every swatch in the library — handy when the binding is
    // to a swatch that lives outside any palette.
    //
    // We use the literal id "__all__" as the sentinel value; PaletteId
    // is std::string and SwatchLibrary's add_palette generates UUID-
    // style ids, so collision-safe. Definition lives in PaintEditor.cpp
    // anonymous namespace as kAllPaletteId.
    std::vector<color::PaletteId> m_picker_palette_ids;

    // Connection for the dropdown's property_selected signal. Disconnected
    // before rebuild so programmatic re-selection in rebuild_palette_
    // dropdown doesn't fire user-flow handlers.
    sigc::connection m_palette_dd_conn;

    // Cached RenderState for re-rendering after picker callbacks (the
    // picker delivers RGB but doesn't know about binding annotation,
    // so we need the most recent state to restore the annotation
    // after a picker close even if the host hasn't called
    // set_render_state yet). Set every set_render_state call.
    RenderState m_last;
    bool        m_have_state = false;

    // Re-entry guard for programmatic toggle/entry updates inside
    // set_render_state. Mirrors PropertiesPanel's m_loading idiom but
    // scoped to this widget — the widget can't see the host's flag,
    // and a host-only flag wouldn't catch picker-callback re-entry.
    bool        m_syncing = false;

    TypeChangedSignal   m_sig_type_changed;
    ColorChangedSignal  m_sig_color_changed;
    UnbindClickedSignal m_sig_unbind_clicked;
    SwatchPickedSignal  m_sig_swatch_picked;
    PickerClosedSignal  m_sig_picker_closed;
    GradientEditRequestedSignal m_sig_gradient_edit_requested;
};

} // namespace Curvz
