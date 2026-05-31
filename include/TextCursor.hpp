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
#include <optional>
#include <string>
#include <cstddef>
#include <utility>    // std::pair (s305 m2 — selection_range return type)
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

    // s320 m1 — Frame rotation. Baselines are laid out in the boundary's
    // *upright* frame (x_start/x_end/y are upright-frame doc coords), and
    // this frame transform maps them into doc space. frame_angle is the
    // boundary's rotation (radians) derived from its first edge; (frame_cx,
    // frame_cy) is the boundary centroid the rotation pivots about. A
    // consumer that wants a baseline point in doc space rotates the upright
    // point about (frame_cx, frame_cy) by frame_angle. frame_angle == 0 is
    // the common (axis-aligned) case: the transform is identity and every
    // consumer behaves exactly as before. Only rotation lives here for now;
    // skew/curvy outlines are deferred (the baseline still asks "what is my
    // allowed span", which this milestone answers with the upright bbox).
    double frame_angle = 0.0;
    double frame_cx    = 0.0;
    double frame_cy    = 0.0;
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
                               const SceneNode* text,
                               size_t byte_start = 0);

// s320 m1 — Frame basis for a text boundary. Derives the boundary's
// orientation (radians) from its first edge (node[0] -> node[1], which for
// a rect-built TextBox boundary is the text-flow axis) and its centroid
// (mean of the path nodes, the pivot the rotation is taken about). Both
// compute_text_layout and the renderer call this so they agree on one frame
// — the pump pattern: one place defines the basis, two sides consume it.
// Returns angle == 0 for a degenerate or missing boundary (axis-aligned
// fallback). This is the rect-only, derive-from-geometry stage; stored
// angle/skew memory replaces it when skew and non-rect frames arrive, where
// geometry can no longer reconstruct the transform unambiguously.
void text_frame_basis(const SceneNode* boundary,
                      double& angle, double& cx, double& cy);

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
               SceneNode* boundary = nullptr, size_t byte_start = 0);

    SceneNode* text_node() const { return m_text; }
    size_t     byte_index() const { return m_byte_index; }

    // s317 — Multi-region support. The cursor lays out [m_byte_start, …]
    //   into m_boundary; for a Mgr with several member boxes, each box is
    //   a region with its own boundary + byte_start. Canvas drives crossing
    //   between regions by re-anchoring via set_region. The cursor itself
    //   stays single-region; absolute byte offsets in the layout (see
    //   compute_text_layout) mean m_byte_index matching is unchanged.
    SceneNode* boundary() const { return m_boundary; }
    size_t     region_byte_start() const { return m_byte_start; }
    void       set_region(SceneNode* boundary, size_t byte_start) {
        m_boundary = boundary;
        m_byte_start = byte_start;
        m_preferred_caret_x = -1.0;   // re-anchoring resets column intent
    }

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

    // s306 m6 — Vertical caret navigation.
    //
    //   Up/Down move the caret to the previous/next visual baseline,
    //   trying to preserve the caret's horizontal position (doc-x)
    //   across multiple presses. The preferred-x snapshot lets the
    //   caret thread through short lines without "collapsing" inward:
    //     ┌────────────────────────────────────────────────────────┐
    //     │ A long first line that goes most of the way across     │
    //     │ short                                                  │
    //     │ another long line resumed                              │
    //     └────────────────────────────────────────────────────────┘
    //   Down from the end of line 1 lands at the end of "short"
    //   (clamped by available width); a second Down lands BACK at
    //   the preserved column on line 3, not at the collapsed end-of-
    //   "short" column. The preferred_x is captured the first time
    //   Up/Down is pressed (sentinel < 0 = unset) and cleared by
    //   every horizontal action (move_left/right/line_start/line_end/
    //   buffer_start/buffer_end, every insert/delete, place_caret_at)
    //   so the next Up/Down rediscovers it from the caret's new x.
    //
    //   Bounds:
    //     - Up at baseline index 0 → no-op (returns false).
    //     - Down at the last CONTENT baseline → no-op. Empty
    //       baselines below content (m4f's capacity-display
    //       hairlines) are visual-only; Down does not walk into
    //       them. Clicks into them remain explicit and route
    //       naturally through byte_index_at.
    //
    //   Selection semantics match the other movers: plain Up/Down
    //   collapse anchor=caret; the Canvas-layer Shift-extend
    //   snapshot-and-restore pattern (handle_text_edit_key's
    //   extend_with lambda) handles Shift+Up/Down by saving the
    //   anchor before the call and restoring it after, leaving the
    //   selection growing from the original press point.
    //
    //   Returns true when the caret actually moved (so callers can
    //   gate queue_draw).
    bool move_up();
    bool move_down();

    // s305 m1 — Click-to-position. Map a doc-space (x, y) to a byte
    //   offset inside the current buffer using the same TextLayout
    //   that position_on_canvas inverts the other direction. The
    //   mapping algorithm:
    //     1. Find the baseline whose vertical band [y - ascent,
    //        y + descent] contains doc_y. Above the first → snap to
    //        first; below the last → snap to last. Outside the
    //        boundary's horizontal extent on a chosen line clamps to
    //        the line's [byte_start, byte_end] range.
    //     2. Within that baseline, compute the local x in doc units,
    //        ask Pango's xy_to_index for the byte (with trailing flag
    //        for caret-affinity: trailing past the glyph midpoint
    //        snaps to the NEXT byte, matching every text editor).
    //     3. Return the absolute byte = baseline.byte_start + relative.
    //   Returns nullopt when the layout can't resolve (no boundary,
    //   no baselines, empty buffer). For an empty buffer the only
    //   sensible caret position is 0, so an empty buffer returns 0
    //   rather than nullopt.
    std::optional<size_t> byte_index_at(double doc_x, double doc_y) const;

    // Move the caret to (doc_x, doc_y). Thin wrapper around
    // byte_index_at — assigns m_byte_index when resolution succeeds.
    // Returns true when the index actually changed (caller may
    // queue_draw); returns false when geometry didn't resolve OR
    // the click landed exactly where the caret already was.
    bool place_caret_at(double doc_x, double doc_y);

    // ── s305 m2 — Selection range model.
    //
    //   The selection is the byte span between an "anchor" and the
    //   caret (m_byte_index). The anchor is the OTHER end of the
    //   selection: a click sets anchor = caret (collapsed); a
    //   shift+click or a drag moves the caret but leaves the anchor;
    //   typing while a selection is active replaces the range. When
    //   anchor == caret there is no selection — just an insertion
    //   point. selection_range() returns the sorted [start, end)
    //   pair so callers never have to know which end is which.
    //
    //   For m2 the model is added pure-additively: every existing
    //   mutation (insert_*, backspace, delete_forward, move_*,
    //   place_caret_at) collapses the anchor to the caret after it
    //   runs, preserving "no selection ever active" behaviour from
    //   m1 and earlier. m3 wires drag-to-select (which calls
    //   set_byte_index without collapsing); m4 flips the existing
    //   mutations to extend/collapse/replace based on whether shift
    //   is held; m5 hooks copy/cut/paste through selection_text and
    //   delete_selection. The split keeps m2 a no-op for users while
    //   making the model available for the next milestones.

    size_t anchor_byte() const { return m_anchor_byte; }

    // True when anchor != caret, i.e. some bytes are selected.
    bool has_selection() const { return m_anchor_byte != m_byte_index; }

    // Sorted [start, end) byte span. When there is no selection,
    // returns (caret, caret) — both halves equal — so callers that
    // unconditionally call this still behave sensibly (the empty
    // range produces an empty slice, no rendered highlight).
    std::pair<size_t, size_t> selection_range() const;

    // UTF-8 slice of the buffer between anchor and caret. Empty
    // string when has_selection() is false. Used by clipboard
    // copy/cut in m5.
    std::string selection_text() const;

    // Set the caret WITHOUT touching the anchor. Used by drag-update
    // in m3 — the press already set anchor = caret at the click, and
    // motion-while-dragging extends the selection by moving only the
    // caret end. Clamped against text_content.size().
    void set_byte_index(size_t b);

    // Set the anchor WITHOUT touching the caret. Used by select-all
    // (anchor = 0, then caret = size via set_byte_index) and by any
    // future "Shift+click to extend" gesture (move caret to click,
    // leave anchor alone — but the anchor needs to have been seeded
    // somewhere). Clamped against text_content.size().
    void set_anchor_byte(size_t b);

    // Collapse the selection to the caret. After this, has_selection()
    // is false. Idempotent.
    void collapse_selection();

    // Anchor = 0, caret = text_content.size(). Triggered by Ctrl+A.
    void select_all();

    // s326 m2c — Multi-click granularity selection around a byte offset
    //   (from byte_index_at at the click point). Double-click -> word,
    //   triple -> visual line (the baseline's byte window), quadruple ->
    //   paragraph (between hard '\n' breaks). Each sets anchor + caret to
    //   the span; no buffer mutation. select_line_at resolves the layout
    //   the same way byte_index_at does.
    void select_word_at(size_t byte);
    void select_line_at(size_t byte);
    void select_paragraph_at(size_t byte);

    // Erase [start, end) if non-empty, set caret = start, collapse
    // anchor. Returns true when the buffer actually changed. Used
    // by m4 "typing-while-selected replaces" and m5 "Ctrl+X" /
    // "Delete with selection."
    bool delete_selection();

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
    // s317 — absolute byte where this region's layout begins (0 for a
    //   single-box edit; the running flow offset for member k>0).
    size_t     m_byte_start = 0;
    // s305 m2 — Selection anchor. The OTHER end of the selection;
    //   the caret (m_byte_index) is one end and m_anchor_byte is the
    //   other. Initialised equal to m_byte_index (in the ctor, after
    //   the caret's restore-from-text_caret_byte logic runs) so the
    //   default state is "no selection." Existing mutations sync
    //   m_anchor_byte = m_byte_index at the end of their run to
    //   keep the m1-and-earlier behaviour of "no selection ever
    //   active" until m3/m4 wire the extend semantics.
    size_t     m_anchor_byte = 0;
    // s306 m6 — Vertical-nav column anchor. Sentinel < 0 means "unset
    //   — the next Up/Down should snapshot it from the caret's current
    //   doc-x." Every horizontal action (move_left, move_right,
    //   move_line_start, move_line_end, move_buffer_start,
    //   move_buffer_end, place_caret_at, insert_char, insert_string,
    //   insert_newline, backspace, delete_forward, delete_selection)
    //   resets it; Up/Down read it (snapshotting if unset) but never
    //   reset it. Result: a chain of Up/Down presses preserves the
    //   horizontal column across short intermediate lines; any
    //   horizontal motion drops the anchor and the next Up/Down
    //   re-snapshots.
    double     m_preferred_caret_x = -1.0;
    bool       m_visible = true;

    // s306 m6 — Horizontal-action bookkeeping. Every method that
    //   moves the caret horizontally or mutates the buffer at the
    //   caret position calls this at the end: collapse anchor to
    //   caret (m2 default) AND drop the preferred-x column anchor
    //   so the next Up/Down rediscovers it from the caret's new
    //   doc-x. The two effects always travel together — separating
    //   them invites a forgotten-reset bug where typing-then-Down
    //   uses a stale column from before the typing. Inline so the
    //   compiler can fold it into existing hot paths.
    void on_horizontal_motion() {
        m_anchor_byte = m_byte_index;
        m_preferred_caret_x = -1.0;
    }
};

} // namespace Curvz
