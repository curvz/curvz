#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// TextCursor — the editing primitive for canvas text.
//
// One cursor instance exists while a text node is being edited; it is owned
// by Canvas and destroyed when the edit commits or cancels. The cursor is
// both a model object (where in the buffer is the insertion point?) and a
// geometry object (where on the canvas does the caret render, at what
// angle, what length?). One class because separating them would mean two
// classes that always change together.
//
// Responsibilities:
//   - Hold a reference to the text node being edited.
//   - Track a single byte offset (UTF-8) into the text node's content.
//   - Insert / delete / navigate operations that mutate both content and
//     offset together so they never desync.
//   - Compute the caret's on-canvas position by consulting:
//       (a) the text node's bound boundary (the layout area), and
//       (b) the text node's line-pattern path if any (the path each line
//           rides). For 1b the line pattern is always the default
//           horizontal one — derived from the boundary's interior.
//     Position returns (x, y, angle) in doc space. For straight horizontal
//     lines the angle is always zero; later arcs (Arc F) will produce
//     non-zero angles when the line pattern curves.
//   - Render itself as a thin caret line, gated by a blink-visibility flag
//     that the owning Canvas toggles via a timer.
//
// Out of scope for 1b (deferred):
//   - Click-to-position-caret (mouse hit-test → byte offset).
//   - Multi-character selection (a separate range, not a single offset).
//   - IME composition (preedit string preview).
//   - Word-granularity navigation (Ctrl+Left/Right).
//
// The class deliberately does NOT own the Cairo context or any GTK widget;
// render() is called with an externally supplied context. Likewise key
// dispatch happens externally — Canvas's keyhandler calls insert_char /
// backspace / etc. The cursor stays a pure model + geometry object.
// ─────────────────────────────────────────────────────────────────────────────

#include <cairomm/cairomm.h>
#include <glib.h>     // gunichar
#include <pango/pangocairo.h>
#include <memory>
#include <string>
#include <cstddef>
#include <vector>

namespace Curvz {

struct SceneNode;
class Canvas;

// ── Layout primitive used by both the renderer and the cursor ───────────────
// A BaselineLayout is one visual line of text: where it sits, how wide it
// is, and what bytes of the buffer ended up on it. The geometry is in doc
// space; the byte range is into the text node's text_content. The Pango
// layout pointer is the single-line Pango layout we built to measure the
// chunk — held here because the cursor needs it later to map byte_index →
// pixel x within the line.
//
// PangoLayout* is a refcounted GObject; we wrap in a custom deleter to
// preserve correct ownership across copies (BaselineLayout is move-only).
struct BaselineLayout {
    double y          = 0.0;   // baseline doc y (Pango layout origin is top-left;
                               // y here is the baseline of the visual line)
    double x_start    = 0.0;   // segment left in doc space
    double x_end      = 0.0;   // segment right in doc space (= x_start + width)
    double angle      = 0.0;   // 0 for straight; reserved for curved line patterns
    double ascent     = 0.0;   // font ascent at this line (doc units)
    double descent    = 0.0;   // s301 m1h — font descent (doc units), separate
                               // from leading. Used by the caret renderer to
                               // size itself to glyph height (ascent + small
                               // tail of descent) rather than full line
                               // height (which includes the line-spacing
                               // gap and is visually too tall for a caret).
    double height     = 0.0;   // full line height including leading (doc units)
    size_t byte_start = 0;     // inclusive, into text_content
    size_t byte_end   = 0;     // exclusive
    struct PangoDeleter { void operator()(PangoLayout* p) const { if (p) g_object_unref(p); } };
    std::unique_ptr<PangoLayout, PangoDeleter> pango;
};

// Result of laying out a buffer into a boundary. Overflow (bytes not
// placed because no baseline accepted them) is everything from
// bytes_consumed to text_content.size(); for 1c it doesn't render and
// the user resizes the bbox to see it. Threading (1d) will pour overflow
// into the next chained boundary.
struct TextLayout {
    std::vector<BaselineLayout> baselines;
    size_t                      bytes_consumed = 0;
};

// THE universal layout function. Given a boundary path and a text node
// (with content, font, margins), produces the baseline list. For 1c the
// boundary is treated as an axis-aligned bbox (the rectangle case); the
// function ignores the actual boundary outline beyond its bbox extents.
// Arc E will replace the body of this function with outline-intersection
// scanline; the signature stays the same and every consumer is unchanged.
//
// "Universal" is deliberate: rect, ellipse, S-shape, hand-drawn polygon
// all flow through this one function. The rect case is just the
// degenerate one where every baseline has the same (x_start, x_end).
TextLayout compute_text_layout(const SceneNode* boundary,
                               const SceneNode* text);

class TextCursor {
public:
    // Construct against a text node. The cursor starts at the end of
    // text_content (byte offset == content.size()) so typing immediately
    // appends — this is what the user expects when entering a fresh bbox.
    // The Canvas pointer gives access to the doc tree for resolving the
    // bound boundary by iid; nothing is captured longer than the cursor
    // lifetime.
    //
    // The optional `boundary` parameter lets the caller pass the
    // boundary directly. This is the path used by TextBox-owned text
    // — the boundary is a structural sibling (parent->children[1]),
    // not an iid-linked peer, so the cursor stores the pointer rather
    // than looking it up via text_boundary_ids each frame. When
    // omitted (legacy paired-sibling text), position_on_canvas falls
    // back to the iid lookup on text_boundary_ids the way it always
    // did. The two paths coexist so files loaded from before the
    // TextBox migration continue to edit correctly.
    TextCursor(Canvas* canvas, SceneNode* text_node,
               SceneNode* boundary = nullptr);

    SceneNode* text_node() const { return m_text; }
    size_t     byte_index() const { return m_byte_index; }

    // ── Buffer mutation. Returns true when the operation actually
    //    changed the buffer (caller may then queue_draw + emit
    //    doc_changed). For insert_char the parameter is one UTF-8
    //    codepoint serialized as up to 4 bytes; insert_string handles
    //    multi-codepoint paste. Backspace removes the codepoint before
    //    the caret (grapheme-aware via g_utf8_*); Delete removes the
    //    codepoint after; insert_newline inserts a literal '\n' so
    //    Pango treats it as a paragraph break on next layout.
    bool insert_char(gunichar codepoint);
    bool insert_string(const std::string& utf8);
    bool insert_newline();
    bool backspace();
    bool delete_forward();

    // ── Caret navigation. Move by codepoints (UTF-8 aware). Vertical
    //    navigation (Up/Down) requires Pango layout introspection — it
    //    walks lines based on the current Pango layout against the
    //    boundary interior. For 1b the vertical navigation is a stub
    //    that no-ops; horizontal navigation works fully.
    void move_left();
    void move_right();
    void move_line_start();
    void move_line_end();
    void move_buffer_start();
    void move_buffer_end();

    // ── Geometry. Compute the caret's (x, y, angle) in doc space.
    //    For 1b the calculation is: build a Pango layout for
    //    text_content into the boundary interior width, ask Pango for
    //    the (x, y) of the caret position at byte_index, translate
    //    into doc space using the boundary's top-left + margins.
    //    Returns valid=false when geometry can't be resolved (no
    //    boundary bound, empty layout, etc.).
    //
    //    `height` is the caret's render length in doc units — matches
    //    the Pango line height at byte_index.
    struct Geometry {
        double x = 0.0;
        double y = 0.0;          // top of caret line in doc space
        double height = 0.0;     // caret render length in doc units
        double angle = 0.0;      // radians; 0 = vertical, line pattern
                                 // tangent rotation adds to this in Arc F
        bool   valid = false;
    };
    Geometry position_on_canvas() const;

    // ── Blink visibility. Canvas owns the timer; cursor only stores
    //    state and obeys it during render. Defaults to visible on
    //    construction so the cursor appears immediately when the user
    //    starts editing.
    bool visible() const { return m_visible; }
    void set_visible(bool v) { m_visible = v; }
    void toggle_visible() { m_visible = !m_visible; }

    // ── Render the caret. Expects to be called inside the doc-space
    //    Cairo transform (translate(ox,oy) + scale(zoom,zoom) already
    //    applied) so the caret renders at doc-unit coordinates. Skips
    //    rendering when invisible (blink off) or geometry invalid.
    void render(const Cairo::RefPtr<Cairo::Context>& cr) const;

private:
    Canvas*    m_canvas = nullptr;
    SceneNode* m_text   = nullptr;
    // Optional direct boundary pointer — used when the cursor is
    // operating on TextBox-owned text. nullptr means "fall back to
    // iid lookup on m_text->text_boundary_ids," the legacy path.
    SceneNode* m_boundary = nullptr;
    size_t     m_byte_index = 0;
    bool       m_visible = true;
};

} // namespace Curvz
