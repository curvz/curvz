// curvz_utils.hpp ─────────────────────────────────────────────────────────
//
// General-purpose static pump methods. One header, one .cpp, one
// namespace — utilities accumulate here rather than in one-off helper
// files. Compile is cheap; reuse is the win.
//
// Style: small composable units. Each function does one thing. Larger
// jobs compose these primitives rather than inlining the work.
//
// ── Current contents ─────────────────────────────────────────────────
//   sanitise_for_xml_id    name → safe XML-id token (no spaces/specials)
//   short_iid              first 8 chars of an iid for compact id use
//   encode_svg_id          (name, iid) → SVG id attribute value
//   count_anchors          recursive BezierNode count under a SceneNode
//   doc_anchor_count       sum of count_anchors across non-guide layers
//   doc_object_count       count of top-level visible objects in a doc
//   box_blur_argb32        in-place 3-pass box blur on ARGB32 pixels
//                          (Cairo premultiplied, ≈ Gaussian)
//   build_gradient_pattern FillStyle + bbox → Cairo::Pattern (linear/radial)
//   render_drop_shadow_under blur+composite a host_pat shadow under cr
//                            (offscreen surface + box blur + masked tint)
//
// ── SVG id contract ──────────────────────────────────────────────────
//
// The SVG `id` attribute is a derived, write-only handle from Curvz's
// perspective. Curvz's source of truth for a node is:
//
//   data-curvz-name = the human-readable name (round-trips verbatim)
//   data-curvz-iid  = the internal UUID (round-trips verbatim)
//
// The SVG `id` is constructed from those two so external consumers
// (browsers, foreign editors, <use> references, CSS selectors) get a
// readable, unique handle. Curvz never decodes it back — on load,
// SvgParser reads name from data-curvz-name and iid from data-curvz-iid
// directly. If those attributes are missing (foreign or legacy SVG),
// the parser generates a fresh name+iid; the foreign id is irrelevant.
//
// Encoded form: "<sanitised_name>¶<short_iid>"
//   - separator: U+00B6 PILCROW SIGN (UTF-8 0xC2 0xB6) — a key trap.
//                Stripped from names on encode, so it never appears in
//                the name half. Unique by construction.
//   - short_iid: first 8 hex chars of the full uuid. Compact and
//                effectively unique inside one SVG document; the full
//                iid lives in data-curvz-iid for actual identity.
// ─────────────────────────────────────────────────────────────────────

#pragma once

#include <cairomm/cairomm.h>
#include <functional>  // s125 m2a — std::function for callbacks
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <map>         // s125 m2a — result map for show_form
#include <string>
#include <variant>     // s125 m2a v2 — FormFieldSpec / FormFieldValue::Value
#include <vector>      // s125 m2a — buttons + fields lists

// Forward-declare to keep SceneNode.hpp out of this header. The pump
// signatures only need a const reference, so declarations alone suffice.
namespace Curvz {
struct FillStyle;
struct SceneNode;       // s132 m2 — count_anchors / count_objects pumps
struct CurvzDocument;   // s132 m2 — document-level wrappers
}

namespace curvz::utils {

// ── Dialog motif inheritance ────────────────────────────────────────
// Dialogs are separate top-level GTK windows from MainWindow, so the
// motif class on MainWindow doesn't propagate. Each dialog calls this
// after `set_transient_for(parent)` and before `present()`. It checks
// whether the parent window already wears the `curvz-light` class
// (true iff the project motif is Light), and if so, copies the class
// onto the dialog. Setting the class before first present means the
// dialog body resolves the light token block on its very first style
// pass — no dark-then-flip flash.
//
// Inheriting via the parent window (rather than a project pointer)
// means sub-dialogs spawned from another dialog also work: their
// parent is the parent dialog (which already wears the class from
// its own apply call), so the class chains through correctly.
//
// Idempotent: if the parent doesn't have the class, nothing happens.
// Calling on an already-classed dialog is a no-op (add_css_class is
// set-membership). S117 m18 v2.
inline void apply_motif_class_from_parent(Gtk::Window& dlg, Gtk::Window& parent) {
    if (parent.has_css_class("curvz-light"))
        dlg.add_css_class("curvz-light");
}

// ── set_name ─────────────────────────────────────────────────────────
// Tag a widget with a short abbrev (visible in GTK Inspector's Name
// column) and a long descriptive name (compile-time documentation).
//
// Only the abbrev is stored on the widget — the long name lives in
// the call site as a self-documenting argument and is harvested by
// `widget_names_sync` to regenerate `widget_names.db`. Source code is
// the single source of truth: the DB is a generated artefact.
//
// Calling this rather than Gtk::Widget::set_name() directly is the
// trap-character convention that lets the sync script grep for
// curvz::utils::set_name unambiguously, with no risk of confusion
// with Gtk::FileFilter::set_name() and other unrelated set_name calls.
//
// Usage:
//   curvz::utils::set_name(*this, "mw", "main_window_root");
//   curvz::utils::set_name(btn,   "tb_mb", "main_toolbar_macro_btn");
//
// Both reference and pointer overloads exist so call sites stay clean
// regardless of whether the widget is an automatic, *this, or a raw
// pointer from make_managed.
inline void set_name(Gtk::Widget& w, const char* abbrev,
                     const char* /*long_name — registry annotation*/) {
    w.set_name(abbrev);
}

inline void set_name(Gtk::Widget* w, const char* abbrev,
                     const char* /*long_name — registry annotation*/) {
    if (w) w->set_name(abbrev);
}

// Sanitise a human name into an XML-id-legal token.
//
//   - Allow ASCII letters, digits, '-', '_', '.'
//   - Replace whitespace runs with single '_'
//   - Drop the pilcrow separator (key trap)
//   - Drop everything else (specials, Unicode letters)
//   - If result is empty or starts with a digit/'-'/'.', prepend '_'
//
// Lossy on purpose. The original name is preserved in data-curvz-name;
// this token only needs to be stable, unique-ish, and XML-id-legal.
std::string sanitise_for_xml_id(const std::string& name);

// Compact form of an iid for use in an SVG id. Returns the first 8
// characters of iid, or the full iid if shorter than 8. Empty in →
// empty out.
std::string short_iid(const std::string& iid);

// Compose an SVG id from a human name and an internal iid.
//
//   ("Layer 1", "12345678-1234-...") → "Layer_1¶12345678"
//   ("",        "12345678-1234-...") → "12345678"
//   ("Layer 1", "")                  → "Layer_1"
//   ("",        "")                  → ""
std::string encode_svg_id(const std::string& name,
                          const std::string& iid);

// ── Document counting pumps (s132 m2) ────────────────────────────────
// Single source of truth for the StatusBar's "N objects · N nodes"
// readout. MainWindow had five copies of "iterate doc.layers and sum
// children.size()" with `nodes=0` hardcoded — same hand-rolled walk
// duplicated across selection, undo, redo, doc-active-changed, and
// doc-changed handlers. These pumps replace all five with a recursive
// walk that handles Compound, Group, ClipGroup, Blend, Warp containers
// and ignores Guide / GridLayer / MarginLayer / RefLayer / MeasureLayer.
//
// "Anchor" = BezierNode entry on a Path. Compound / Group / ClipGroup
// recurse into children. Text / Image / Ref / Guide / Measurement do
// not contribute — they are not paths in the editing-surface sense
// the StatusBar means by "nodes". Matches what node-count means in
// every vector editor (anchor points, not glyph runs or pixel corners).
//
// "Object" = a top-level direct child of a non-guide layer. Same
// definition the existing duplicated loops used; now centralised.
// Groups / compounds count as a single object regardless of children
// (a 100-path group is still "one object" to the user).

// Recursively count BezierNode anchors under any SceneNode.
//   • Path                   → path->nodes.size() if path is non-null
//   • Compound, Group,
//     ClipGroup, Blend, Warp → recurse over children
//   • Anything else          → 0
int count_anchors(const Curvz::SceneNode& n);

// Sum of count_anchors across every non-guide / non-overlay layer in
// a document. Skips GuideLayer, GridLayer, MarginLayer, RefLayer,
// MeasureLayer — those are scaffolding, not user content.
int doc_anchor_count(const Curvz::CurvzDocument& doc);

// Number of top-level user-visible objects across non-overlay layers.
// "User-visible" here means "what shows in the Layers panel as a row" —
// direct children of regular Layer nodes, regardless of node count.
int doc_object_count(const Curvz::CurvzDocument& doc);

// ── box_blur_argb32 ──────────────────────────────────────────────────
// In-place blur of an ARGB32 (Cairo premultiplied, BGRA byte order on
// little-endian) image buffer. Approximates a Gaussian blur via three
// passes of a separable box filter — Wells (1986); also what most
// browser <feGaussianBlur> implementations do internally. The visual
// difference from a true Gaussian is below threshold for the radii
// used in drop shadows.
//
// Parameters:
//   data        — pointer to the ImageSurface's pixel buffer (from
//                 cairo_image_surface_get_data).
//   stride      — row stride in bytes (from get_stride). Cairo guarantees
//                 stride >= width * 4 and 4-byte alignment.
//   width, height — surface dimensions in pixels.
//   radius      — box-pass radius in pixels. 0 = no-op. Each pass averages
//                 (2*radius + 1) samples in each direction; three passes
//                 of radius r approximate a Gaussian with stddev sigma
//                 ≈ r * sqrt(3/4) ≈ 0.866*r. To target a desired sigma,
//                 caller passes radius = round(sigma / 0.866). For SVG
//                 stdDeviation parity, radius = round(stdDeviation).
//
// Premultiplied alpha invariant is preserved: averaging premultiplied
// channels yields the correct premultiplied result, no un/re-mul needed.
//
// The caller is responsible for calling Cairo::ImageSurface::flush()
// before this function (so any pending Cairo writes are visible in the
// buffer) and ::mark_dirty() after (so subsequent Cairo reads see the
// blurred bytes).
void box_blur_argb32(unsigned char* data, int stride,
                     int width, int height, int radius);

// ── build_gradient_pattern ────────────────────────────────────────────
// Build a Cairo gradient pattern from a FillStyle of type LinearGradient
// or RadialGradient, with endpoints lerped from objectBoundingBox-fraction
// space (0..1 of the shape's bbox) into doc-space using the supplied
// bbox. Mirrors Canvas::apply_fill(cr, fill, bbox)'s gradient branch so
// printers, exporters, or any other Cairo consumer can render a node's
// gradient identically to the canvas.
//
// Parameters:
//   fill            — must be LinearGradient or RadialGradient. Other
//                     types produce an empty RefPtr (caller checks).
//   bbox_x, bbox_y  — top-left of the shape's bbox in the same coordinate
//                     space as cr's current user-space.
//   bbox_w, bbox_h  — width/height of the shape's bbox in user-space.
//
// Returns: a refcounted pattern with stops added, or an empty RefPtr
// when fill isn't a gradient or has no stops. Caller is responsible for
// cr->set_source(pat) and the fill/stroke call that consumes it.
//
// Empty stops produces a fully transparent pattern (matches Canvas).
Cairo::RefPtr<Cairo::Pattern> build_gradient_pattern(
    const Curvz::FillStyle& fill,
    double bbox_x, double bbox_y, double bbox_w, double bbox_h);

// ── render_drop_shadow_under ──────────────────────────────────────────
// Paint a tinted, blurred, offset shadow of host_pat onto cr, in the
// region described by host_bbox (doc-space). Reads CTM scale via
// user_to_device so it works in any Cairo context — canvas widget,
// print page, SVG export — without knowing the zoom factor up front.
//
// Pipeline (matches Canvas::render_shadow_under):
//   1. Pad host_bbox by blur reach + |dx|, |dy| + safety constant.
//   2. Map padded rect through cr's CTM → device pixels.
//   3. Allocate ARGB32 ImageSurface at those pixel dimensions.
//   4. Paint host_pat into surface with cr's CTM offset by surface origin
//      (matrix-correct so source pixels land at correct surface pixels).
//   5. Three-pass box blur in place (radius = round(blur_doc * ctm_scale)).
//   6. On cr at identity CTM, mask shadow colour through blurred alpha
//      at (surface_origin + shadow_offset_device).
//
// Caller paints the unblurred host on top after this returns.
//
// No-op when host_pat is empty or surface dimensions degenerate.
//
// Note: this function omits the canvas-viewport intersect that
// Canvas::render_shadow_under uses. The intersect is a zoom-bounded
// optimisation for live editing; print/export contexts have CTM scales
// bounded by the output device (paper points, SVG units), so the
// surface size is naturally bounded. Skipping it keeps the function
// general-purpose without sacrificing print performance.
void render_drop_shadow_under(
    const Cairo::RefPtr<Cairo::Context>& cr,
    const Cairo::RefPtr<Cairo::Pattern>& host_pat,
    double host_bbox_x, double host_bbox_y,
    double host_bbox_w, double host_bbox_h,
    double blur_doc, double dx_doc, double dy_doc,
    double color_r, double color_g, double color_b, double color_a,
    double opacity);

// ── Curvz dialog factory (s125 m2a) ────────────────────────────────────
//
// A small toolkit for showing modal dialogs that look and behave like
// part of the app. Three entry points: show_alert (one-button info),
// show_confirm (multi-button confirm), show_form (input fields).
//
// ── Why this exists ─────────────────────────────────────────────────
//
// GTK4's Gtk::AlertDialog is a system-level alert primitive. It paints
// itself using the OS theme — not the app's — so a lightmode Curvz
// running on a dark desktop produces a dark popup. Its body is a
// non-selectable Pango string, so users can't copy paths or error
// messages out. And because it's the system's dialog, we have no
// hooks to extend or theme it.
//
// This factory builds plain Gtk::Window dialogs that live INSIDE the
// app's CSS scope. They inherit motif from the parent window via
// apply_motif_class_from_parent — light app, light dialog. They use
// stable CSS classes so visual treatment is a stylesheet edit, not a
// code change. And they're regular widgets, so they extend naturally
// when we want a new feature (selectable text, structured fields,
// custom buttons) instead of wrestling with the OS dialog API.
//
// ── Why three functions instead of one ──────────────────────────────
//
// show_alert and show_confirm are conveniences over the same primitive
// — show_alert is "show_confirm with one button and no result." But
// "click OK on a warning" is the most common dialog by an order of
// magnitude in this app, and forcing every callsite to construct a
// button list, a callback, and an index just to say `show_alert("Save
// failed", path)` would be friction nobody wanted.
//
// show_form is a different beast: it accepts typed input fields, and
// the result is a map keyed by field id. It's the form-builder of
// the trio. See its own banner below for the design concepts that
// drive it.
//
// ── CSS hooks ───────────────────────────────────────────────────────
//
// Visual styling lives in css.hpp under `Curvz alert / confirm / form
// dialog`. Stable hooks the factory always emits:
//
//   .curvz-alert            window root
//   .curvz-alert-title      headline label
//   .curvz-alert-detail     detail entry (read-only, selectable)
//   .curvz-alert-fields     form field grid (show_form only)
//   .curvz-alert-buttons    button row
//   .curvz-alert-default    suggested-action button
//   .curvz-alert-cancel     destructive/cancel button
//
// Don't add layout-equivalent properties (margins, colors, font sizes)
// inline in the factory. Only structural decisions live in code.

// ── show_alert ──────────────────────────────────────────────────────
//
// Fire-and-forget info / warning. One OK button, no callback. Use this
// when you have something to TELL the user and don't need a decision
// back. The user can dismiss with OK, Esc, or X — all equivalent.
//
// Most common use: error messages, permission denials, "nothing
// selected" feedback. If you find yourself wanting a callback after the
// user dismisses, you probably want show_confirm with one button
// instead.
void show_alert(Gtk::Window& parent,
                const std::string& title,
                const std::string& detail = "");

// ── show_confirm ────────────────────────────────────────────────────
//
// Multi-button decision. Callback fires with the index of whichever
// button the user picked, OR with `cancel_button`'s index if they
// dismissed via Esc / X. Pass cancel_button=-1 if there's nothing
// cancel-equivalent in your button list (callback gets -1 on dismiss).
//
// `default_button` is the suggested-action button — gets the accent
// styling and activates on Enter.
// `cancel_button` is the destructive/escape button — gets neutral
// styling (loud destructive override is a per-callsite CSS class) and
// activates on Esc.
//
// Buttons are 0-indexed in the vector you pass. By convention put the
// destructive option first ("Cancel", "Discard") and the affirmative
// last ("Save", "Replace") — that's the order callers tend to
// remember when reading the callback's index.
void show_confirm(Gtk::Window& parent,
                  const std::string& title,
                  const std::string& detail,
                  const std::vector<std::string>& buttons,
                  int default_button,
                  int cancel_button,
                  std::function<void(int)> callback);

// ── show_form ───────────────────────────────────────────────────────
//
// A spec-driven input dialog. Borland-inspired.
//
// ── Concept 1: the dialog is data, not code ─────────────────────────
//
// Most dialog APIs are imperative: you instantiate a dialog object,
// call .add_field(), .add_button(), wire signals, present, wait. By
// the time the dialog runs, "what dialog is this" is spread across
// dozens of stateful method calls.
//
// show_form inverts that. The caller describes the dialog as a value
// — a vector<FormField> plus a few scalars — and hands it over. The
// factory translates the value into widgets. The data IS the
// contract; widgets are the implementation.
//
// Consequences:
//   • You can build the spec inline, deserialise it from JSON, generate
//     it from runtime state (e.g. a crash dialog reflecting on app
//     state), or fetch it from a server. The factory doesn't care.
//   • Two callsites with different specs share zero code. Adding a
//     dialog never requires editing the factory.
//   • The spec is inspectable: log it, diff it, hash it, store it.
//
// This is why FormField is a plain struct with no pointers, callbacks,
// or references. Pure data. JSON-shaped.
//
// ── Concept 2: open type set, closed operations ─────────────────────
//
// We need to support different kinds of input (text, numbers, checks,
// combos) and we need to know we'll add more later (date, multiline,
// radio group, file picker). The natural OOP answer is a base class
// with virtual build/extract methods — open for extension, but the
// type set leaks into every consumer that needs to allocate one.
//
// Variant + visitor inverts the trade-off. The TYPE SET is open: add
// a new struct, add it to FormFieldSpec, done. The OPERATIONS are
// closed: build_field_widget and extract_value enumerate every arm,
// and the compiler enforces it via static_assert in std::visit. So
// "what kinds of fields exist" is a one-line edit, but "what we do
// with each field" is rigorously checked.
//
// You cannot ship a half-supported field type. Either the build pass
// compiles (every arm handled) or it doesn't. This is the property
// that makes the factory maintainable as it grows.
//
// ── Concept 3: identity by name, not position ───────────────────────
//
// Fields are declared with stable string ids; results come back as a
// map keyed by id. Field ORDER in the spec is purely visual — does
// not affect callsite code. This means:
//
//   • Reordering fields in the spec doesn't break consumers.
//   • Adding a new field doesn't break consumers (they ignore the new
//     key in the result map).
//   • Conditionally including a field (e.g. show "category" only if
//     advanced=true) doesn't break consumers — they just don't see
//     that key when it wasn't shown.
//
// This is why FormField has an `id` string member that's required and
// must be unique. The id is the API contract; the position isn't.
//
// ── Concept 4: cleanup is automatic ─────────────────────────────────
//
// The dialog window self-deletes on hide. The callback is fired exactly
// once, on dismissal, with whatever button the user picked (or the
// cancel button index if they hit Esc / X). Caller does not manage
// window lifetime, signal connections, or modal state.
//
// This is what makes show_form safe to call from anywhere — including
// places where holding a dialog object across an async boundary would
// be awkward (signal handlers, idle callbacks, error paths).
//
// ── Extending: adding a new field type ──────────────────────────────
//
// Concretely, given the three concepts above:
//
//   1. Define `struct FooField { … config … };` next to the others —
//      this is your "spec for a Foo." Pure data, no widgets.
//   2. Add it to the FormFieldSpec variant — now the spec layer
//      accepts FooField values.
//   3. Add a `[](const FooField&)` arm in build_field_widget (in
//      curvz_utils.cpp) — this is what the factory does with one.
//   4. Add an arm in extract_value — this is how its widget's state
//      becomes a FormFieldValue.
//
// If you forget step 3 or 4 the build fails with a static_assert. If
// you forget steps 1 or 2, you can't even construct a spec. The
// concepts steer you — there's no fifth place to update because the
// design has only those four moving parts.
//
// ── Styling ─────────────────────────────────────────────────────────
//
// Visual appearance is entirely in css.hpp under `Curvz alert /
// confirm / form dialog`. Don't add CSS-equivalent properties (margins,
// colors, font sizes) inline in build_dialog. The only inline styling
// is structural: layout direction, widget existence, alignment.
//
// Field type configs follow.

struct TextField {
    std::string default_text;
    std::string placeholder;  // greyed hint when empty
};

struct NumberField {
    double default_value = 0.0;
    double min      = 0.0;
    double max      = 100.0;
    double step     = 1.0;
    int    decimals = 0;
};

struct CheckboxField {
    bool default_value = false;
};

struct ComboField {
    std::vector<std::string> items;
    int default_index = 0;
};

// FormFieldSpec — discriminated union of field-type configs. Add new
// field types here. std::variant gives us compile-time exhaustiveness
// for the visitor in build_dialog.
using FormFieldSpec = std::variant<TextField, NumberField,
                                   CheckboxField, ComboField>;

struct FormField {
    std::string   id;     // key in the result map; required, must be unique
    std::string   label;  // human-readable label shown left of the widget
    FormFieldSpec spec;   // the per-type config
};

// FormFieldValue — the result for one field. Accessors throw if the
// wrong type is requested; callers should know what type they asked for
// (or use std::visit on .value for fully type-safe handling).
struct FormFieldValue {
    using Value = std::variant<std::string,  // Text
                               double,       // Number
                               bool,         // Checkbox
                               int>;         // Combo (selected index)
    Value       value;
    std::string combo_item;   // Combo only — the text of the selected
                              // item, since callers usually want the
                              // string not the index. Empty for other types.

    // Convenience accessors. `text()` works for both Text fields (the
    // entered string) and Combo fields (the selected item text — uses
    // combo_item rather than reaching into the variant).
    const std::string& text() const {
        if (std::holds_alternative<std::string>(value))
            return std::get<std::string>(value);
        return combo_item;  // Combo case
    }
    double num()   const { return std::get<double>(value); }
    bool   flag()  const { return std::get<bool>(value); }
    int    index() const { return std::get<int>(value); }
};

void show_form(Gtk::Window& parent,
               const std::string& title,
               const std::string& detail,  // "" to omit the detail row
               const std::vector<FormField>& fields,
               const std::vector<std::string>& buttons,
               int default_button,
               int cancel_button,
               std::function<void(int button_index,
                                  const std::map<std::string, FormFieldValue>& values)>
                   callback);

} // namespace curvz::utils
