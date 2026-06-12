// ─────────────────────────────────────────────────────────────────────────────
// TextCursor + compute_text_layout — implementation.
//
// The universal layout flow (rect, ellipse, S-shape, hand-drawn — all the
// same code path):
//   1. Walk baselines top-to-bottom inside the boundary interior at
//      stride `leading`. For each baseline y, determine its (x_start,
//      x_end) — the segment that lies inside the boundary at that y.
//   2. Build a single-line Pango layout for the remaining buffer chunk.
//      Pango is told NOT to wrap — its line is as long as it wants to
//      be. We then choose the break point ourselves using Pango's
//      built-in line-break iterator (PangoLineBreaks via
//      pango_layout_line_index_to_x and word-boundary measurement).
//   3. The bytes that fit go on the current baseline. The remainder
//      moves to the next baseline. Repeat.
//   4. Stop when baselines exhausted or buffer empty.
//
// 1c implements step (1) for axis-aligned rectangles: every baseline
// shares the same (x_start, x_end) = boundary interior. Arc E will
// generalize step (1) to outline-intersection scanline for arbitrary
// closed paths — the same function signature, the same downstream
// consumers, only the baseline-segment computation changes.
// ─────────────────────────────────────────────────────────────────────────────
#include "TextCursor.hpp"
#include "Canvas.hpp"
#include "SceneNode.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"                  // s331 — kCurvzLeadingAttr (per-para leading)
#include "style/TextStyleLibrary.hpp"        // s340 — per-paragraph style resolve
#include "style/TextStyleInterop.hpp"        // s340 — resolve_paragraph_baseline / box_baseline
#include "color/Color.hpp"                   // s343 — channel_to_u8 (bound-style colour baseline)
#include "color/Paint.hpp"                   // s343 — Solid / SwatchRef variant access
#include "math/TextFlowGeometry.hpp"   // s323 — form-fit reflow geometry pumps
#include "math/BezierPath.hpp"         // s347 m4 — pattern_path_length
#include "math/PathOffset.hpp"         // s347 m4 — pattern_walk_path (offset lines)

#include <pango/pangocairo.h>
#include <glib.h>
#include <variant>                     // s343 — std::get_if (Paint variant)
#include <algorithm>
#include <cmath>
#include <limits>

namespace Curvz {

// ── s333 — justify spill knobs (TEMP: live-tunable via the StyleBar slider) ──
// Comfort = extra width (in em) a word-space may gain under justify before the
// overflow spills into letter-spacing. Track = ceiling (em) on that letter-
// spacing so it never reads "spacey." These are file-scope statics, not
// compile-time consts, so the temp tuning slider can sweep them live without a
// recompile (the slider writes them via curvz_set_justify_knobs and forces a
// canvas redraw; compute_text_layout re-runs on draw and reads the new values).
// When the values are dialed in, fold the winners back to consts and delete the
// slider + these globals.
static double g_justify_comfort_em = 0.18;
static double g_justify_track_em   = 0.05;
void curvz_set_justify_knobs(double comfort_em, double track_em) {
    g_justify_comfort_em = comfort_em;
    g_justify_track_em   = track_em;
}
void curvz_get_justify_knobs(double& comfort_em, double& track_em) {
    comfort_em = g_justify_comfort_em;
    track_em   = g_justify_track_em;
}

// s305 m4f — Find the last baseline with content (byte_start < byte_end).
//   compute_text_layout emits empty baselines below the content to show
//   the user the textbox's full line capacity; end-of-window fallbacks
//   should target the LAST CONTENT baseline, not the bottom empty one.
//   (s345: the pointer variant last_content_baseline was deleted along
//   with the per-reader predicate stacks — the index variant below is
//   the resolver's only fallback now.)
static size_t last_content_index(const TextLayout& tl) {
    if (tl.baselines.empty()) return 0;
    for (size_t i = tl.baselines.size(); i-- > 0; ) {
        if (tl.baselines[i].byte_start < tl.baselines[i].byte_end)
            return i;
    }
    return 0;
}

// s306 m6 / s307 — HISTORY. This function used to re-derive ownership
//   with the in_range / empty_owns(crossed_newline) / hard_end predicate
//   stack, hand-mirrored across five readers. The worked examples
//   ("first" B=5 -> line 0; "first\n" B=6 -> the empty line below;
//   "first\nsecond" B=12 -> line 1) still describe the INTENDED
//   behavior — they are now pinned by the fit-time own ranges and the
//   sandbox property tests rather than re-implemented per reader.
// s345 — THE byte->baseline resolver. Ownership is a stored-range lookup
//   against the own_start/own_end fields the fitter assigned at push time
//   (see compute_text_layout's own_cursor) — never inferred from buffer
//   content. The s307 crossed_newline, s338/s344 hard_end, and s305 m4d/
//   m4f conventions are all encoded in those ranges; this function and
//   every caller stay in register with the renderer BY CONSTRUCTION
//   because there is exactly one reader and one writer of the fact.
//   The buf parameter is gone: a resolver that peeks at the buffer is a
//   resolver that can disagree with the fitter.
//
//   Fallback: positions outside the tiled window (overflow bytes past
//   bytes_consumed — the region chain's territory) resolve to the last
//   content baseline, same end-of-buffer degradation as the old m4f
//   fallback. INSIDE the window the fallback firing would mean a tiling
//   violation (the s345 geometry diag confirmed zero occurrences before
//   it was stripped).
static size_t baseline_index_for(const TextLayout& tl, size_t byte_index) {
    for (size_t i = 0; i < tl.baselines.size(); ++i) {
        const BaselineLayout& bl = tl.baselines[i];
        if (byte_index >= bl.own_start && byte_index < bl.own_end) {
            return i;
        }
    }
    // Past the tiled window / unmatched: fall back to the last content
    // baseline (caret renders at end of content, not on a bottom
    // capacity hairline).
    return tl.baselines.empty() ? size_t(-1) : last_content_index(tl);
}

// s345 — Vertical-step companions to the resolver. A baseline that owns
//   no caret positions (own_start == own_end: s323 neck artifacts,
//   capacity lines past the first) is not a place the caret can live, so
//   Up/Down must step OVER it to the nearest OWNING baseline. Stepping
//   by raw index ±1 onto a non-owning line put the caret at a byte the
//   resolver assigns to the line BEYOND it — Down skipped a row, and Up
//   could never climb back past a neck (resolve always landed below it;
//   Up no-opped forever). Indents make this hot: an indent that eats a
//   line's span emits a fall-through neck right where the user is
//   editing.
static size_t prev_owning_index(const TextLayout& tl, size_t from) {
    for (size_t i = from; i-- > 0; ) {
        const BaselineLayout& bl = tl.baselines[i];
        if (bl.own_start < bl.own_end) return i;
    }
    return size_t(-1);
}
static size_t next_owning_index(const TextLayout& tl, size_t from) {
    for (size_t i = from + 1; i < tl.baselines.size(); ++i) {
        const BaselineLayout& bl = tl.baselines[i];
        if (bl.own_start < bl.own_end) return i;
    }
    return size_t(-1);
}

// ── Construction ────────────────────────────────────────────────────────────
// s345 — see the declaration's comment: the one layout constructor for
//   cursor operations, carrying the canvas's text-style library so caret
//   geometry, navigation, and selection all read the renderer's layout.
TextLayout TextCursor::layout_for(const SceneNode* boundary) const {
    return compute_text_layout(
        boundary, m_text, m_byte_start,
        m_canvas ? m_canvas->text_style_library() : nullptr);
}

TextCursor::TextCursor(Canvas* canvas, SceneNode* text_node,
                       SceneNode* boundary, size_t byte_start)
    : m_canvas(canvas), m_text(text_node), m_boundary(boundary),
      m_byte_start(byte_start) {
    if (!m_text) return;
    // s305 m1 — Caret persistence. Restore the byte the last edit left
    //   off at; fall back to end-of-buffer when no saved position
    //   exists (text_caret_byte == 0 covers both "never edited" and
    //   "explicitly at the start", but the only natural place a fresh
    //   cursor wants is the end — and saving caret_byte == 0 from an
    //   edit that did happen to land at the start is harmless because
    //   the ctor's behaviour is identical to "no saved position" in
    //   that one degenerate case). The clamp protects against stale
    //   indices on a text whose content shrank between sessions
    //   (paste-then-truncate, undo-after-delete, etc.).
    size_t saved = (m_text->text_caret_byte > 0)
                       ? (size_t)m_text->text_caret_byte
                       : m_text->text_content.size();
    if (saved > m_text->text_content.size())
        saved = m_text->text_content.size();
    m_byte_index = saved;
    // s305 m2 — Anchor starts at the same byte as the caret so the
    //   default state is "no selection." Drag/shift-click in later
    //   milestones moves caret without touching anchor.
    m_anchor_byte = m_byte_index;
}

// ── Buffer mutation ─────────────────────────────────────────────────────────
bool TextCursor::insert_char(gunichar codepoint) {
    if (!m_text) return false;
    // s307 m5+ — Defensive clamp; see backspace for the rationale.
    if (m_byte_index > m_text->text_content.size()) {
        m_byte_index = m_text->text_content.size();
        m_anchor_byte = m_byte_index;
    }
    char buf[8];
    int n = g_unichar_to_utf8(codepoint, buf);
    if (n <= 0) return false;
    m_text->text_content.insert(m_byte_index, buf, (size_t)n);
    curvz::utils::shift_spans_on_insert(
        m_text->text_attr_spans, (unsigned)m_byte_index, (unsigned)n);
    m_byte_index += (size_t)n;
    on_horizontal_motion();  // s306 m6 — collapse anchor + drop preferred_x
    return true;
}

bool TextCursor::insert_string(const std::string& utf8) {
    if (!m_text || utf8.empty()) return false;
    if (!g_utf8_validate(utf8.c_str(), (gssize)utf8.size(), nullptr))
        return false;
    // s307 m5+ — Defensive clamp.
    if (m_byte_index > m_text->text_content.size()) {
        m_byte_index = m_text->text_content.size();
        m_anchor_byte = m_byte_index;
    }
    m_text->text_content.insert(m_byte_index, utf8);
    curvz::utils::shift_spans_on_insert(
        m_text->text_attr_spans, (unsigned)m_byte_index, (unsigned)utf8.size());
    m_byte_index += utf8.size();
    on_horizontal_motion();  // s306 m6 — collapse anchor + drop preferred_x
    return true;
}

bool TextCursor::insert_newline() {
    if (!m_text) return false;
    // Insert a literal line-feed character (U+000A). Pango treats this
    // as a hard paragraph break on next layout. This is the user's
    // explicit "new paragraph" — distinct from word-wrap line breaks
    // which are visual-only and never enter the buffer.
    // s307 m5+ — Defensive clamp.
    if (m_byte_index > m_text->text_content.size()) {
        m_byte_index = m_text->text_content.size();
        m_anchor_byte = m_byte_index;
    }
    m_text->text_content.insert(m_byte_index, "\n");
    curvz::utils::shift_spans_on_insert(
        m_text->text_attr_spans, (unsigned)m_byte_index, 1u);
    m_byte_index += 1;
    on_horizontal_motion();  // s306 m6 — collapse anchor + drop preferred_x
    return true;
}

bool TextCursor::backspace() {
    if (!m_text || m_byte_index == 0) return false;
    // s307 m5+ — Defensive clamp. m_byte_index can lag the buffer when
    //   the buffer is mutated from outside the cursor's view: a global
    //   Ctrl+Z mid-edit applies TextEditCommand::before_content to the
    //   buffer (shrinking it), and the cursor's caret index is now
    //   past the end. Without this clamp, g_utf8_prev_char walks off
    //   the end of the heap-allocated string and the subsequent
    //   string::erase throws std::out_of_range. delete_selection
    //   already had this guard; backspace and delete_forward didn't.
    //   Proper fix lives in m6 (mid-edit Ctrl+Z intercept restores
    //   caret position from the command's before_caret); this is
    //   belt-and-braces so a stale-index crash can't surface.
    if (m_byte_index > m_text->text_content.size()) {
        m_byte_index = m_text->text_content.size();
        m_anchor_byte = m_byte_index;
        if (m_byte_index == 0) return false;
    }
    const char* base = m_text->text_content.c_str();
    const char* here = base + m_byte_index;
    const char* prev = g_utf8_prev_char(here);
    size_t prev_idx = (size_t)(prev - base);
    m_text->text_content.erase(prev_idx, m_byte_index - prev_idx);
    curvz::utils::shift_spans_on_delete(
        m_text->text_attr_spans, (unsigned)prev_idx,
        (unsigned)(m_byte_index - prev_idx));
    m_byte_index = prev_idx;
    on_horizontal_motion();  // s306 m6 — collapse anchor + drop preferred_x
    return true;
}

bool TextCursor::delete_forward() {
    if (!m_text || m_byte_index >= m_text->text_content.size()) return false;
    // s307 m5+ — Same defensive clamp as backspace above. The guard at
    //   the top already early-returns when caret >= size, so a stale
    //   caret PAST size makes the comparison hit the >= branch and
    //   we return false — no crash. This belt-and-braces clamp brings
    //   the post-state into a sane condition before the rest of the
    //   function runs, matching delete_selection's defensive shape.
    if (m_byte_index > m_text->text_content.size()) {
        m_byte_index = m_text->text_content.size();
        m_anchor_byte = m_byte_index;
        return false;  // post-clamp, nothing forward to delete
    }
    const char* base = m_text->text_content.c_str();
    const char* here = base + m_byte_index;
    const char* next = g_utf8_next_char(here);
    size_t next_idx = (size_t)(next - base);
    m_text->text_content.erase(m_byte_index, next_idx - m_byte_index);
    curvz::utils::shift_spans_on_delete(
        m_text->text_attr_spans, (unsigned)m_byte_index,
        (unsigned)(next_idx - m_byte_index));
    on_horizontal_motion();  // s306 m6 — collapse anchor + drop preferred_x
    return true;
}

// ── Caret navigation ────────────────────────────────────────────────────────
void TextCursor::move_left() {
    if (!m_text || m_byte_index == 0) return;
    const char* base = m_text->text_content.c_str();
    const char* here = base + m_byte_index;
    const char* prev = g_utf8_prev_char(here);
    m_byte_index = (size_t)(prev - base);
    on_horizontal_motion();  // s306 m6 — collapse anchor + drop preferred_x
}

void TextCursor::move_right() {
    if (!m_text || m_byte_index >= m_text->text_content.size()) return;
    const char* base = m_text->text_content.c_str();
    const char* here = base + m_byte_index;
    const char* next = g_utf8_next_char(here);
    m_byte_index = (size_t)(next - base);
    on_horizontal_motion();  // s306 m6 — collapse anchor + drop preferred_x
}

void TextCursor::move_line_start() {
    if (!m_text) return;
    // s305 m4c — Visual-line Home. The buffer is a flat byte string; \n
    //   characters mark hard paragraph breaks but NOT soft wraps. A
    //   paragraph that flows across three visual rows has no \n bytes
    //   between rows; walking backward for \n would jump to the
    //   paragraph start, which on a single-paragraph textbox means
    //   byte 0 (same as Ctrl+Home — not what the user wants).
    //
    //   The visual-line truth lives on the TextLayout: each baseline
    //   knows the byte range it holds. Find the baseline that owns
    //   m_byte_index and set caret to its byte_start.
    //
    //   Falls back to the old \n-walk when the layout can't be built
    //   (no boundary bound, malformed textbox, etc.) so legacy bare
    //   text without a boundary still has a sensible Home behaviour.
    SceneNode* boundary = m_boundary;
    if (!boundary && m_canvas && !m_text->text_boundary_ids.empty()) {
        boundary = m_canvas->find_text_boundary(
            m_text->text_boundary_ids.front());
    }
    if (boundary) {
        TextLayout tl = layout_for(boundary);  // s345 — styled layout, see layout_for
        if (!tl.baselines.empty()) {
            // s345 — resolve through THE resolver. This reader's inline
            //   copy of the predicate stack was missing the s338/s344
            //   hard_end rule: Home with the caret at a paragraph break
            //   fell through to the last content line and jumped the
            //   caret to the bottom of the flow. Stored-range lookup
            //   can't drift out of register with the renderer.
            size_t idx = baseline_index_for(tl, m_byte_index);
            if (idx != size_t(-1)) {
                m_byte_index = tl.baselines[idx].byte_start;
                on_horizontal_motion();  // s306 m6
            }
            return;
        }
    }
    // Legacy / fallback: \n-walk paragraph start.
    if (m_byte_index == 0) return;
    const std::string& s = m_text->text_content;
    size_t i = m_byte_index;
    while (i > 0 && s[i - 1] != '\n') --i;
    m_byte_index = i;
    on_horizontal_motion();  // s306 m6
}

void TextCursor::move_line_end() {
    if (!m_text) return;
    // s305 m4c — Visual-line End. Mirror of move_line_start; see that
    //   function for the rationale on layout vs \n-walk.
    SceneNode* boundary = m_boundary;
    if (!boundary && m_canvas && !m_text->text_boundary_ids.empty()) {
        boundary = m_canvas->find_text_boundary(
            m_text->text_boundary_ids.front());
    }
    if (boundary) {
        TextLayout tl = layout_for(boundary);  // s345 — styled layout, see layout_for
        if (!tl.baselines.empty()) {
            // s345 — resolve through THE resolver (this copy was also
            //   missing hard_end; see move_line_start).
            size_t idx = baseline_index_for(tl, m_byte_index);
            if (idx == size_t(-1)) return;
            const BaselineLayout* target = &tl.baselines[idx];

            // s305 m4e — byte_end may include whitespace absorbed by
            //   the wrap (see compute_text_layout's m4e block). For
            //   End-of-line we want the caret to land at the visible
            //   end of the line — just after the last printed glyph,
            //   before the absorbed whitespace. Walk backward from
            //   byte_end past any ' ' or '\t' bytes to find it.
            size_t target_byte = target->byte_end;
            const std::string& s = m_text->text_content;
            while (target_byte > target->byte_start &&
                   target_byte <= s.size() &&
                   (s[target_byte - 1] == ' ' || s[target_byte - 1] == '\t')) {
                --target_byte;
            }
            m_byte_index = target_byte;
            on_horizontal_motion();  // s306 m6
            return;
        }
    }
    // Legacy / fallback: \n-walk paragraph end.
    const std::string& s = m_text->text_content;
    size_t i = m_byte_index;
    while (i < s.size() && s[i] != '\n') ++i;
    m_byte_index = i;
    on_horizontal_motion();  // s306 m6
}

void TextCursor::move_buffer_start() {
    m_byte_index = 0;
    on_horizontal_motion();  // s306 m6 — collapse anchor + drop preferred_x
}

void TextCursor::move_buffer_end() {
    if (m_text) m_byte_index = m_text->text_content.size();
    on_horizontal_motion();  // s306 m6 — collapse anchor + drop preferred_x
}

// ── s306 m6 — Vertical caret navigation ─────────────────────────────────────
// move_up / move_down share most of their logic; only the baseline-index
// step differs (-1 vs +1) and the bounds check (top-of-list vs last-
// content-baseline). Internal helper move_vertical(delta) does the work.
//
// Algorithm:
//   1. Resolve boundary (same priority as position_on_canvas /
//      byte_index_at: m_boundary first, then iid lookup).
//   2. compute_text_layout once.
//   3. Find the caret's current baseline index (strict-< + m4f
//      fallback via baseline_index_for).
//   4. Bounds:
//        delta < 0 and current_idx == 0 → no-op.
//        delta > 0 and current_idx >= last_content_index → no-op.
//          (Content baselines are a contiguous prefix in normal
//          layouts; the last_content_index check correctly stops
//          Down at the last navigable row regardless of how many
//          capacity empties trail it.)
//   5. Snapshot preferred_x lazily: if m_preferred_caret_x < 0,
//      compute the current caret's doc-x via the current baseline's
//      Pango layout (same calculation as position_on_canvas line
//      658-662). Store it. Do NOT update it on Up/Down — only
//      horizontal actions clear it.
//   6. Pick target baseline by index (current_idx + delta).
//   7. Convert preferred_x to local x for the target baseline:
//      local_x = preferred_x - target.x_start, clamped to
//      [0, target.x_end - target.x_start].
//   8. Pango xy_to_index on target. Trailing-flag advance via
//      g_utf8_next_char (same as byte_index_at m1).
//   9. Apply the m4e trailing-whitespace rewind when the clamp
//      pushed us to byte_end — preferred_x past visible line width
//      should land at the visible line end, not after absorbed
//      whitespace.
//   10. Caret moves. Anchor collapses (Canvas-layer Shift+Up/Down
//       handles extend via the snapshot-restore pattern). preferred_x
//       stays set so a chain of Up/Down presses preserves the column.
//   Returns true when the caret actually moved.
bool TextCursor::move_up() {
    if (!m_text || !m_canvas) return false;

    SceneNode* boundary = m_boundary;
    if (!boundary) {
        if (m_text->text_boundary_ids.empty()) return false;
        boundary = m_canvas->find_text_boundary(
            m_text->text_boundary_ids.front());
        if (!boundary) return false;
    }

    TextLayout tl = layout_for(boundary);  // s345 — styled layout, see layout_for
    if (tl.baselines.empty()) return false;

    size_t cur_idx = baseline_index_for(tl, m_byte_index);
    if (cur_idx == size_t(-1)) return false;
    // s345 — step to the nearest OWNING baseline above (skips neck
    //   artifacts and other zero-ownership rows; see prev_owning_index).
    size_t tgt_idx = prev_owning_index(tl, cur_idx);
    if (tgt_idx == size_t(-1)) return false;  // nothing navigable above

    const BaselineLayout* current = &tl.baselines[cur_idx];
    const BaselineLayout* target  = &tl.baselines[tgt_idx];

    // Snapshot preferred_x lazily.
    if (m_preferred_caret_x < 0.0) {
        int rel = (int)(m_byte_index - current->byte_start);
        if (rel < 0) rel = 0;
        int max_rel = (int)(current->byte_end - current->byte_start);
        if (rel > max_rel) rel = max_rel;
        PangoRectangle pos;
        pango_layout_index_to_pos(current->pango.get(), rel, &pos);
        m_preferred_caret_x = current->x_start + pos.x / (double)PANGO_SCALE;
    }

    // Resolve target byte from preferred_x.
    double line_width = target->x_end - target->x_start;
    if (line_width < 0.0) line_width = 0.0;
    double x_in_line = m_preferred_caret_x - target->x_start;
    bool clamped_right = false;
    if (x_in_line < 0.0) x_in_line = 0.0;
    if (x_in_line > line_width) { x_in_line = line_width; clamped_right = true; }

    int px_x = (int)std::round(x_in_line * PANGO_SCALE);
    int rel_byte = 0;
    int trailing = 0;
    pango_layout_xy_to_index(target->pango.get(), px_x, 0,
                              &rel_byte, &trailing);
    if (rel_byte < 0) rel_byte = 0;
    int max_rel = (int)(target->byte_end - target->byte_start);
    if (rel_byte > max_rel) rel_byte = max_rel;

    size_t abs_byte = target->byte_start + (size_t)rel_byte;
    if (trailing > 0) {
        const std::string& s = m_text->text_content;
        for (int t = 0; t < trailing && abs_byte < s.size(); ++t) {
            const char* p = s.c_str() + abs_byte;
            const char* n = g_utf8_next_char(p);
            if (!n || n == p) break;
            abs_byte += (size_t)(n - p);
        }
        if (abs_byte > m_text->text_content.size())
            abs_byte = m_text->text_content.size();
    }

    // m4e trailing-whitespace rewind when preferred_x is past the
    // visible line edge. Without this, clamping to line_width lands
    // the caret after absorbed wrap whitespace — a position the user
    // can't see and didn't ask for. Matches move_line_end's behaviour.
    if (clamped_right) {
        const std::string& s = m_text->text_content;
        while (abs_byte > target->byte_start &&
               abs_byte <= s.size() &&
               (s[abs_byte - 1] == ' ' || s[abs_byte - 1] == '\t')) {
            --abs_byte;
        }
    }

    if (abs_byte > m_text->text_content.size())
        abs_byte = m_text->text_content.size();

    if (abs_byte == m_byte_index) return false;

    m_byte_index = abs_byte;
    // s306 m6 — Vertical move: collapse anchor (extend semantics
    //   handled at Canvas layer), but preserve preferred_x so the
    //   next Up/Down keeps the column.
    m_anchor_byte = m_byte_index;
    return true;
}

bool TextCursor::move_down() {
    if (!m_text || !m_canvas) return false;

    SceneNode* boundary = m_boundary;
    if (!boundary) {
        if (m_text->text_boundary_ids.empty()) return false;
        boundary = m_canvas->find_text_boundary(
            m_text->text_boundary_ids.front());
        if (!boundary) return false;
    }

    TextLayout tl = layout_for(boundary);  // s345 — styled layout, see layout_for
    if (tl.baselines.empty()) return false;

    size_t cur_idx = baseline_index_for(tl, m_byte_index);
    if (cur_idx == size_t(-1)) return false;

    // s345 — step to the nearest OWNING baseline below. This replaces the
    //   `cur_idx >= last_content_index` guard, which was WRONG in two
    //   ways: (a) it blocked Down from the last content line onto the
    //   empty line below a trailing '\n' — a legitimate caret home (it
    //   owns the post-newline position) — the literal "Down won't go
    //   further" symptom; (b) raw index+1 could land on a non-owning
    //   neck row (see prev_owning_index's comment).
    size_t tgt_idx = next_owning_index(tl, cur_idx);
    if (tgt_idx == size_t(-1)) return false;  // already on the last owning row

    const BaselineLayout* current = &tl.baselines[cur_idx];
    const BaselineLayout* target  = &tl.baselines[tgt_idx];

    // Snapshot preferred_x lazily (same as move_up).
    if (m_preferred_caret_x < 0.0) {
        int rel = (int)(m_byte_index - current->byte_start);
        if (rel < 0) rel = 0;
        int max_rel = (int)(current->byte_end - current->byte_start);
        if (rel > max_rel) rel = max_rel;
        PangoRectangle pos;
        pango_layout_index_to_pos(current->pango.get(), rel, &pos);
        m_preferred_caret_x = current->x_start + pos.x / (double)PANGO_SCALE;
    }

    double line_width = target->x_end - target->x_start;
    if (line_width < 0.0) line_width = 0.0;
    double x_in_line = m_preferred_caret_x - target->x_start;
    bool clamped_right = false;
    if (x_in_line < 0.0) x_in_line = 0.0;
    if (x_in_line > line_width) { x_in_line = line_width; clamped_right = true; }

    int px_x = (int)std::round(x_in_line * PANGO_SCALE);
    int rel_byte = 0;
    int trailing = 0;
    pango_layout_xy_to_index(target->pango.get(), px_x, 0,
                              &rel_byte, &trailing);
    if (rel_byte < 0) rel_byte = 0;
    int max_rel = (int)(target->byte_end - target->byte_start);
    if (rel_byte > max_rel) rel_byte = max_rel;

    size_t abs_byte = target->byte_start + (size_t)rel_byte;
    if (trailing > 0) {
        const std::string& s = m_text->text_content;
        for (int t = 0; t < trailing && abs_byte < s.size(); ++t) {
            const char* p = s.c_str() + abs_byte;
            const char* n = g_utf8_next_char(p);
            if (!n || n == p) break;
            abs_byte += (size_t)(n - p);
        }
        if (abs_byte > m_text->text_content.size())
            abs_byte = m_text->text_content.size();
    }

    if (clamped_right) {
        const std::string& s = m_text->text_content;
        while (abs_byte > target->byte_start &&
               abs_byte <= s.size() &&
               (s[abs_byte - 1] == ' ' || s[abs_byte - 1] == '\t')) {
            --abs_byte;
        }
    }

    if (abs_byte > m_text->text_content.size())
        abs_byte = m_text->text_content.size();

    if (abs_byte == m_byte_index) return false;

    m_byte_index = abs_byte;
    m_anchor_byte = m_byte_index;
    return true;
}

// ── Helper: build a single-line Pango layout for `text` ─────────────────────
// Pango is told NOT to wrap (width = -1) so the layout is whatever wide
// the text wants to be. We measure against the available width and pick
// the break ourselves.
// ── s325 — per-run apply seam ───────────────────────────────────────────────
// Build a PangoAttrList for ONE laid-out line by slicing the node's flat
// text_attr_spans to the line's byte window [chunk_start, chunk_start+chunk_len)
// and rebasing each surviving span into line-local [0, len) coordinates. This
// is the only place the flat buffer spans meet a Pango layout; it runs for the
// render layout AND the measurement layouts, so a wider run (bold/big) changes
// the measured width and the breaker accounts for it — not just the paint.
// Empty span list -> nullptr -> byte-identical to pre-s325 (scalar font only).
static PangoAttrList* build_line_attrs(const SceneNode* text,
                                       size_t chunk_start, int chunk_len) {
    if (!text || text->text_attr_spans.empty() || chunk_len <= 0)
        return nullptr;
    const size_t lo = chunk_start;
    const size_t hi = chunk_start + (size_t)chunk_len;
    PangoAttrList* list = nullptr;
    for (const AttrSpan& s : text->text_attr_spans) {
        if (s.end_byte <= lo || s.start_byte >= hi) continue;  // no overlap
        guint ls = (guint)(std::max((size_t)s.start_byte, lo) - lo);
        guint le = (guint)(std::min((size_t)s.end_byte,   hi) - lo);
        if (le <= ls) continue;
        PangoAttribute* a = nullptr;
        switch ((PangoAttrType)s.type) {
            case PANGO_ATTR_WEIGHT:
                a = pango_attr_weight_new((PangoWeight)s.ivalue); break;
            case PANGO_ATTR_STYLE:
                a = pango_attr_style_new((PangoStyle)s.ivalue); break;
            case PANGO_ATTR_UNDERLINE:
                a = pango_attr_underline_new((PangoUnderline)s.ivalue); break;
            case PANGO_ATTR_STRIKETHROUGH:
                a = pango_attr_strikethrough_new(s.ivalue != 0); break;
            case PANGO_ATTR_OVERLINE:  // Pango 1.46+
                a = pango_attr_overline_new((PangoOverline)s.ivalue); break;
            case PANGO_ATTR_SIZE:
                // s339 — belt-and-braces: a non-positive size must never reach
                // pango_attr_size_new (it asserts size>=0 and spams CRITICAL).
                // The span pumps are the structural fix; this is the floor under
                // it so a stray malformed span degrades to "inherit base size"
                // instead of crashing the layout.
                if (s.ivalue > 0) a = pango_attr_size_new((int)s.ivalue);
                break;
            case PANGO_ATTR_ABSOLUTE_SIZE:
                if (s.ivalue > 0) a = pango_attr_size_new_absolute((int)s.ivalue);
                break;
            case PANGO_ATTR_LETTER_SPACING:
                a = pango_attr_letter_spacing_new((int)s.ivalue); break;
            case PANGO_ATTR_RISE:
                a = pango_attr_rise_new((int)s.ivalue); break;
            case PANGO_ATTR_FONT_SCALE:
                a = pango_attr_font_scale_new((PangoFontScale)s.ivalue);
                // s339 — font_scale only RESIZES; the vertical lift is a separate
                // attribute (Pango's own <sup>/<sub> markup pairs the two). Super/
                // sub is stored as a single font_scale span (so apply/sweep/query/
                // serialize stay single-attr); we DERIVE the matching baseline
                // shift here at render time. Without this the glyph shrinks but
                // stays on the baseline -- the reported bug, for both super & sub.
                // PangoBaselineShift superscript/subscript: Pango 1.50+ (build 1.54).
                if (s.ivalue == PANGO_FONT_SCALE_SUPERSCRIPT ||
                    s.ivalue == PANGO_FONT_SCALE_SUBSCRIPT) {
                    PangoBaselineShift bs =
                        (s.ivalue == PANGO_FONT_SCALE_SUPERSCRIPT)
                            ? PANGO_BASELINE_SHIFT_SUPERSCRIPT
                            : PANGO_BASELINE_SHIFT_SUBSCRIPT;
                    PangoAttribute* shift = pango_attr_baseline_shift_new(bs);
                    shift->start_index = ls;
                    shift->end_index   = le;
                    if (!list) list = pango_attr_list_new();
                    pango_attr_list_insert(list, shift);  // takes ownership
                }
                break;
            case PANGO_ATTR_FAMILY:
                a = pango_attr_family_new(s.svalue.c_str()); break;
            case PANGO_ATTR_FOREGROUND: {
                // packed 0xRRGGBB (8-bit) -> Pango 16-bit channels (v<<8|v).
                guint16 r = (guint16)(((s.ivalue >> 16) & 0xFF) * 0x101);
                guint16 g = (guint16)(((s.ivalue >>  8) & 0xFF) * 0x101);
                guint16 b = (guint16)(( s.ivalue        & 0xFF) * 0x101);
                a = pango_attr_foreground_new(r, g, b);
                break;
            }
            default: break;  // unhandled-for-m1 type: recorded but not applied
        }
        if (!a) continue;
        a->start_index = ls;
        a->end_index   = le;
        if (!list) list = pango_attr_list_new();
        pango_attr_list_insert(list, a);  // takes ownership of `a`
    }
    return list;
}

// s337 m2 — build a PangoTabArray for one line from the paragraph's canonical
// tab spec. Locations are LAYOUT-LOCAL: a stop's stored pos is doc-px from the
// content-area left edge (the tab bar's origin_doc = box-left + margin-left, the
// same frame the indent handles live in), and the per-line layout is placed at
// the indented x_start, so the layout-local location is pos - indent_offset
// (clamped >= 0; a stop left of the text start collapses onto the origin). This
// makes a glyph after a '\t' land on exactly the doc-x the bar draws the stop's
// drop-line at — the convergent-evidence point (target and actual on one screen).
//
static PangoTabAlign pango_align_for(curvz::utils::TabAlign a) {
  switch (a) {
    case curvz::utils::TabAlign::Right:   return PANGO_TAB_RIGHT;
    case curvz::utils::TabAlign::Center:  return PANGO_TAB_CENTER;
    case curvz::utils::TabAlign::Decimal: return PANGO_TAB_DECIMAL;
    case curvz::utils::TabAlign::Left:
    default:                              return PANGO_TAB_LEFT;
  }
}

// s339 — THE seam. One drop-filter, two consumers: the PangoTabArray
// (build_line_tab_array) and the per-line leader-mark list (compute_text_layout).
// Computing the surviving stops once here guarantees the leader draw matches
// where tabs actually land — if these diverged, a leader would tile to a stop
// the layout doesn't use. Locations are PANGO units, layout-local (pos minus
// the line's indent offset); leader/align ride along.
//
// s337 m2a HARDENING (kept): a PangoTabArray needs strictly-increasing,
// strictly-positive locations or Pango can spin forever in tab expansion
// (inside pango_layout_get_size, past compute_text_layout's safety counter --
// an un-catchable hang). Keep only stops that advance past the previous
// accepted stop by >= 1 pango unit; drop the rest. The happy path (ascending
// positive stops, no indent) keeps every stop.
struct SurvivingTabStop {
  int                     loc_pango;   // layout-local, PANGO units
  curvz::utils::TabAlign  align;
  curvz::utils::TabLeader leader;
};
static std::vector<SurvivingTabStop>
surviving_tab_stops(const char* spec, double indent_offset) {
  std::vector<SurvivingTabStop> out;
  if (!spec || !*spec) return out;
  std::vector<curvz::utils::TabStop> stops = curvz::utils::parse_tab_spec(spec);
  out.reserve(stops.size());
  const int kMinStepPango = 1;  // >= 1 pango unit forward; never 0 or backward
  int prev = 0;
  for (const auto& s : stops) {
    double loc_doc = s.pos - indent_offset;
    int    loc     = (loc_doc > 0.0) ? (int)(loc_doc * PANGO_SCALE + 0.5) : 0;
    if (loc < prev + kMinStepPango) continue;   // can't advance — drop it
    out.push_back({ loc, s.type, s.leader });
    prev = loc;
  }
  return out;
}

// s339 — R/C/D + decimal point. Each surviving stop's TabAlign maps to its
// Pango alignment (LEFT/RIGHT/CENTER/DECIMAL; Pango 1.50+, build is 1.54); a
// DECIMAL stop is pinned to '.' (else it aligns on a locale default). Returns
// nullptr (Pango falls back to its default tab interval) when nothing survives,
// leaving the no-tab path byte-identical.
static PangoTabArray* build_line_tab_array(const char* spec,
                                           double indent_offset) {
  std::vector<SurvivingTabStop> surv = surviving_tab_stops(spec, indent_offset);
  if (surv.empty()) return nullptr;
  PangoTabArray* ta = pango_tab_array_new((gint)surv.size(), FALSE);
  for (gint i = 0; i < (gint)surv.size(); ++i) {
    const PangoTabAlign pa = pango_align_for(surv[(size_t)i].align);
    pango_tab_array_set_tab(ta, i, pa, surv[(size_t)i].loc_pango);
    if (pa == PANGO_TAB_DECIMAL)
      pango_tab_array_set_decimal_point(ta, i, (gunichar)'.');
  }
  return ta;
}

static PangoLayout* make_single_line_layout(const SceneNode* text,
                                             const char* chunk,
                                             int chunk_len,
                                             size_t chunk_byte_start = 0,
                                             const char* tab_spec = nullptr,
                                             double tab_indent_offset = 0.0,
                                             const style::ResolvedTextStyle* base = nullptr) {
    // Tiny temp cairo context so PangoCairo can build a layout.
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t* tmp = cairo_create(surf);
    PangoLayout* layout = pango_cairo_create_layout(tmp);
    cairo_destroy(tmp);
    cairo_surface_destroy(surf);

    PangoFontDescription* desc = pango_font_description_new();
    // s340 — the per-paragraph resolved baseline (Option A) is the font default;
    // per-run spans (build_line_attrs below) still override it per byte-range.
    // When `base` is null (no library context / unbound), fall back to the
    // node's loose scalars — byte-identical to pre-s340 behaviour.
    const std::string& fam = base ? base->family : text->text_font_family;
    const double       sz  = base ? base->size   : text->text_font_size;
    const bool         bd  = base ? base->bold   : text->text_bold;
    const bool         it  = base ? base->italic : text->text_italic;
    pango_font_description_set_family(desc, fam.c_str());
    pango_font_description_set_absolute_size(desc, sz * PANGO_SCALE);
    if (bd)
        pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    if (it)
        pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_width(layout, -1);  // no Pango wrap
    pango_layout_set_text(layout, chunk, chunk_len);
    // s343 — per-run formatting for this line, layered OVER the resolved-style
    // baseline. The font_description above already carries the bound style's
    // family/size/bold/italic (s340), but COLOUR and LETTER-SPACING have no
    // font-description equivalent: a bound style's colour/spacing never reached
    // the glyphs -- only direct FOREGROUND/LETTER_SPACING runs did -- so a
    // colour-only or spacing-only style rendered as a no-op once clear-direct
    // stripped the manual run. Fix: seed a whole-line colour + letter-spacing
    // baseline from `base`, THEN insert the per-run direct spans after it so
    // direct still overrides bound in its byte range (Option A precedence).
    // Solid / Swatch colours resolve to a flat foreground; None / CurrentColor
    // / Gradient fall through to the node fill source, as before.
    PangoAttrList* la = pango_attr_list_new();
    if (base) {
        const color::Color* bc = nullptr;
        if (const auto* s = std::get_if<color::Solid>(&base->colour))
            bc = &s->color;
        else if (const auto* w = std::get_if<color::SwatchRef>(&base->colour))
            bc = &w->fallback;  // cached resolved colour
        if (bc) {
            auto u16 = [](double c) {
                return (guint16)(color::channel_to_u8(c) * 0x101);
            };
            PangoAttribute* fg = pango_attr_foreground_new(u16(bc->r), u16(bc->g), u16(bc->b));
            fg->start_index = 0; fg->end_index = (guint)chunk_len;
            pango_attr_list_insert(la, fg);
            if (bc->a < 1.0) {
                PangoAttribute* fa = pango_attr_foreground_alpha_new(u16(bc->a));
                fa->start_index = 0; fa->end_index = (guint)chunk_len;
                pango_attr_list_insert(la, fa);
            }
        }
        if (base->letter_spacing != 0.0) {
            PangoAttribute* lsp = pango_attr_letter_spacing_new(
                (int)(base->letter_spacing * PANGO_SCALE));
            lsp->start_index = 0; lsp->end_index = (guint)chunk_len;
            pango_attr_list_insert(la, lsp);
        }
    }
    // Per-run direct spans on top (inserted after the baseline, so a direct
    // colour/spacing run wins over the bound style across its byte range).
    if (PangoAttrList* runs = build_line_attrs(text, chunk_byte_start, chunk_len)) {
        GSList* attrs = pango_attr_list_get_attributes(runs);
        for (GSList* n = attrs; n; n = n->next)
            pango_attr_list_insert(la, (PangoAttribute*)n->data);  // takes ownership
        g_slist_free(attrs);
        pango_attr_list_unref(runs);
    }
    pango_layout_set_attributes(layout, la);
    pango_attr_list_unref(la);
    // s337 m2 — per-paragraph tab stops for this line. set_tabs COPIES the
    // array into the layout (independent), so we free it immediately. With the
    // array set, Pango treats '\t' as a stop-advance and the width/position
    // measurements (get_size, index_to_pos) account for it — which is why the
    // MEASUREMENT layouts need it too, not just the render layout.
    if (PangoTabArray* ta = build_line_tab_array(tab_spec, tab_indent_offset)) {
        pango_layout_set_tabs(layout, ta);
        pango_tab_array_free(ta);
    }
    return layout;
}

// ── Helper: find the longest prefix of `chunk` whose pixel width <= `avail`. ─
// Uses Pango's line-break iterator (pango_get_log_attrs) which marks
// every byte with break flags. We walk word boundaries and try each as
// a candidate break point; take the last one that fits.
//
// Returns the byte offset (into chunk) to break at. If even the first
// word doesn't fit, returns either the first character break or 0
// depending on whether `force_at_least_one_char` is set — for very
// narrow lines we don't want to deadlock.
static size_t fit_chunk_to_width(const char* chunk, size_t chunk_len,
                                  PangoLayout* layout, double avail_doc) {
    if (chunk_len == 0) return 0;
    int avail_pango = (int)(avail_doc * PANGO_SCALE);
    if (avail_pango <= 0) return 0;

    // First check: does the whole chunk fit? Common case for short text
    // in a wide bbox.
    int total_w_pango, total_h_pango;
    pango_layout_get_size(layout, &total_w_pango, &total_h_pango);
    if (total_w_pango <= avail_pango) {
        // Whole chunk fits. But if it contains a '\n', the layout was
        // multi-line and we need to break at the first \n anyway.
        const char* nl = (const char*)memchr(chunk, '\n', chunk_len);
        if (nl) return (size_t)(nl - chunk); // hard break at \n
        return chunk_len;
    }

    // Walk word boundaries via Pango log-attrs.
    glong n_chars = g_utf8_strlen(chunk, (gssize)chunk_len);
    if (n_chars <= 0) return 0;
    std::vector<PangoLogAttr> attrs((size_t)n_chars + 1);
    pango_get_log_attrs(chunk, (int)chunk_len, -1, nullptr,
                        attrs.data(), (int)attrs.size());

    // Walk codepoints; remember the latest break-opportunity byte offset
    // whose layout-up-to-here width fits.
    size_t best_break_byte = 0;
    bool   any_word_fit    = false;
    const char* p = chunk;
    for (int ci = 1; ci <= (int)n_chars; ++ci) {
        // ci is the codepoint index AFTER ci codepoints — i.e. the
        // boundary between codepoint ci-1 and codepoint ci. Map back to
        // a byte offset.
        const char* nextp = g_utf8_next_char(p);
        size_t byte_offset = (size_t)(nextp - chunk);

        // Hard newline forces a break here regardless of width.
        if (chunk[byte_offset - 1] == '\n') {
            // Return byte offset of the '\n' itself (so the '\n' belongs
            // to the consumed line and the next line starts after it).
            return byte_offset - 1;
        }

        if (attrs[ci].is_line_break) {
            // Measure layout up to byte_offset.
            // Use a sub-layout: set_text on a fresh layout would re-shape;
            // cheaper to use pango_layout_index_to_pos which gives the
            // x coordinate at any byte offset within the *current* layout.
            PangoRectangle pos;
            pango_layout_index_to_pos(layout, (int)byte_offset, &pos);
            // pos.x is the layout-x in PANGO_SCALE units at this byte.
            // For LTR text this is the right edge of the run ending here.
            if (pos.x <= avail_pango) {
                best_break_byte = byte_offset;
                any_word_fit = true;
            } else {
                // Past available width — no later word boundary can fit
                // either, so stop walking.
                break;
            }
        }
        p = nextp;
    }

    if (any_word_fit) {
        // Strip any trailing whitespace from best_break_byte so the
        // wrapped line doesn't end in a literal space. (Pango's
        // PANGO_WRAP_WORD does this; we mimic it.)
        while (best_break_byte > 0 &&
               (chunk[best_break_byte - 1] == ' ' ||
                chunk[best_break_byte - 1] == '\t')) {
            --best_break_byte;
        }
        return best_break_byte;
    }

    // Nothing fit at a word boundary. Fall back to a character break to
    // avoid deadlock: take at least one codepoint.
    return (size_t)(g_utf8_next_char(chunk) - chunk);
}

// ── s317 — Last word-break opportunity strictly before `len` ──────────
// Returns the byte offset of the latest Pango line-break opportunity that
// is < len (excluding the end). Used to back a line off by one word when
// the chosen break still renders too wide (a belt-and-braces guard over
// fit_chunk_to_width's index_to_pos measurement, which can disagree with
// the actual rendered width by a word at the margin). Returns 0 if there
// is no earlier break (a single unbreakable word).
static size_t last_word_break_before(const char* s, size_t len) {
    glong nc = g_utf8_strlen(s, (gssize)len);
    if (nc <= 1) return 0;
    std::vector<PangoLogAttr> a((size_t)nc + 1);
    pango_get_log_attrs(s, (int)len, -1, nullptr, a.data(), (int)a.size());
    const char* p = s;
    size_t last = 0;
    for (int ci = 1; ci < (int)nc; ++ci) {   // ci == nc would be the end; skip
        const char* np = g_utf8_next_char(p);
        size_t bo = (size_t)(np - s);
        if (bo >= len) break;
        if (a[ci].is_line_break) last = bo;
        p = np;
    }
    return last;
}

// ── s323 — width (doc units) of the first word of `chunk` ─────────────────────
// Used by the form-fit fall-through wrap to decide whether the next word fits
// THIS line's span before committing to a break. The "first word" runs to the
// first Pango line-break opportunity (trailing whitespace stripped). A leading
// hard newline is the caller's special case; here it measures as empty (0).
static double measure_first_word_width(const SceneNode* text,
                                       const char* chunk, size_t chunk_len,
                                       size_t chunk_byte_start = 0,
                                       const style::ResolvedTextStyle* base = nullptr) {
    if (chunk_len == 0) return 0.0;
    glong nc = g_utf8_strlen(chunk, (gssize)chunk_len);
    if (nc <= 0) return 0.0;
    std::vector<PangoLogAttr> attrs((size_t)nc + 1);
    pango_get_log_attrs(chunk, (int)chunk_len, -1, nullptr,
                        attrs.data(), (int)attrs.size());
    size_t word_byte = chunk_len;
    const char* p = chunk;
    for (int ci = 1; ci <= (int)nc; ++ci) {
        const char* np = g_utf8_next_char(p);
        size_t bo = (size_t)(np - chunk);
        if (chunk[bo - 1] == '\n') { word_byte = bo - 1; break; }   // hard break
        if (ci < (int)nc && attrs[ci].is_line_break) { word_byte = bo; break; }
        p = np;
    }
    while (word_byte > 0 &&
           (chunk[word_byte - 1] == ' ' || chunk[word_byte - 1] == '\t'))
        --word_byte;
    if (word_byte == 0) return 0.0;
    PangoLayout* l = make_single_line_layout(text, chunk, (int)word_byte,
                                              chunk_byte_start, nullptr, 0.0, base);
    int w = 0, h = 0;
    pango_layout_get_size(l, &w, &h);
    g_object_unref(l);
    return (double)w / (double)PANGO_SCALE;
}

// ── The universal layout function ───────────────────────────────────────────
// s320 m1 — see header. Angle from first edge, centroid from node mean.
void text_frame_basis(const SceneNode* boundary, const SceneNode* text,
                      double& angle, double& cx, double& cy) {
    angle = 0.0; cx = 0.0; cy = 0.0;
    if (!boundary || !boundary->path) return;
    const PathData& bp = *boundary->path;
    if (bp.nodes.size() < 2) return;

    // Centroid: mean of the path nodes — the pivot the rotation is taken
    // about. (Geometry-derived for now; the baseline-editing UI may later
    // pin the pivot too. With a 0 angle the pivot is moot — the rotation is
    // a no-op — so node-edit drift of this mean does not matter yet.)
    double sx = 0.0, sy = 0.0;
    for (const auto& n : bp.nodes) { sx += n.x; sy += n.y; }
    cx = sx / (double)bp.nodes.size();
    cy = sy / (double)bp.nodes.size();

    // s327 — Angle is the STORED base-baseline direction, read from the TEXT
    // (buffer-owning) node. Pre-s327 the read was off the boundary; the field
    // comment is explicit that direction belongs to the baseline, not the
    // shape, and the buffer node is where the s325 spans and the other text
    // properties (line-height, font) live. The angle was never assigned
    // anywhere before this session, so every existing file reads 0.0 either
    // way — the move is behavior-identical until the compass first writes it.
    // The shape still governs the per-line spans via the form-fit intersect;
    // only the flow direction is decoupled.
    angle = text ? text->text_baseline_angle : 0.0;
}

// s334 — per-paragraph indent (doc units). Same shape as the old leading reader:
// a private indent run covering the line's start byte (which sits in the owning
// paragraph) wins; default 0 = no inset. ivalue is doc-px x PANGO_SCALE.
// s342 — indent_for_byte was retired here; resolved_indent (defined below,
// beside resolved_line_leading) supersedes it with the three-tier style
// fallback. The lone caller (compute_text_layout) now reads through the
// resolver, so the direct-span-only reader had no remaining users.

// s337 m2 — per-paragraph tab spec (canonical "pos,type;..." string). Same
// shape as indent_for_byte / leading_for_byte: the kCurvzTabsAttr run covering
// the line's start byte (which sits in the owning paragraph) wins; empty when
// none, so Pango falls back to its default tab interval. The fitter parses this
// into a per-line PangoTabArray (build_line_tab_array) so a '\t' advances to the
// next stop instead of being a plain break opportunity.
// s346 — tabs_for_byte (tier-1-only tab-spec reader, s337) deleted; the
// fitter and the Tabs popover both read resolved_tabs now (direct run ->
// bound style -> empty), so a bound style's tabs reach the layout AND the
// face from one pump.

// s331 — see header. Mirrors compute_text_layout's base-font metric block
// (lines ~970-1012) so the read-out and the stride agree by construction.
double metric_leading_px(const SceneNode* text) {
    if (!text) return 0.0;
    const double font_size = text->text_font_size > 0.0
                                 ? text->text_font_size : 24.0;
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t* cr = cairo_create(surf);
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, text->text_font_family.c_str());
    pango_font_description_set_absolute_size(desc, font_size * PANGO_SCALE);
    if (text->text_bold)
        pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    if (text->text_italic)
        pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    PangoContext* pctx = pango_layout_get_context(layout);
    PangoFontMetrics* fm = pango_context_get_metrics(pctx, desc, nullptr);
    double ascent  = pango_font_metrics_get_ascent(fm)  / (double)PANGO_SCALE;
    double descent = pango_font_metrics_get_descent(fm) / (double)PANGO_SCALE;
    double leading = (ascent + descent) * 1.2;
    if (leading <= 0.0) leading = font_size * 1.2;
    pango_font_metrics_unref(fm);
    pango_font_description_free(desc);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return leading;
}

// s341 — metric leading for an ARBITRARY resolved font (not just the box's).
// Same 1.2x (ascent+descent) derivation as metric_leading_px / the layout's
// base-font block, parameterised so a styled paragraph's larger font yields a
// proportionally larger stride.
static double metric_leading_for(const std::string& family, double size,
                                 bool bold, bool italic) {
    if (size <= 0.0) size = 24.0;
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t* cr = cairo_create(surf);
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, family.c_str());
    pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
    if (bold)   pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    if (italic) pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    PangoContext* pctx = pango_layout_get_context(layout);
    PangoFontMetrics* fm = pango_context_get_metrics(pctx, desc, nullptr);
    double ascent  = pango_font_metrics_get_ascent(fm)  / (double)PANGO_SCALE;
    double descent = pango_font_metrics_get_descent(fm) / (double)PANGO_SCALE;
    double leading = (ascent + descent) * 1.2;
    if (leading <= 0.0) leading = size * 1.2;
    pango_font_metrics_unref(fm);
    pango_font_description_free(desc);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return leading;
}

// s341 — the stride below a line, resolving leading through the three tiers in
// precedence order (the line-spacing twin of resolve_paragraph_baseline for
// font): direct formatting on top, then the bound style, then a metric default.
//   1. explicit per-paragraph leading run (kCurvzLeadingAttr) — direct
//      formatting, wins outright.
//   2. the resolved style's pinned leading (line_base.leading_px > 0).
//   3. metric leading from the line's RESOLVED font, so a styled larger font
//      opens its lines proportionally. When the resolved font equals the box
//      font -- the common unbound case, where line_base IS box_base -- this
//      returns box_leading unchanged, so no Pango metrics call is paid and the
//      stride is byte-identical to pre-s341 behaviour.
// s346 — de-static'd and declared in TextCursor.hpp: the style-bar leading
// read calls this same pump, so face and stride agree by construction.
double resolved_line_leading(const SceneNode* text,
                             const style::ResolvedTextStyle& line_base,
                             const style::ResolvedTextStyle& box_base,
                             double box_leading, size_t line_start) {
    if (text)
        for (const auto& s : text->text_attr_spans)
            if (s.type == curvz::utils::kCurvzLeadingAttr &&
                (size_t)s.start_byte <= line_start &&
                (size_t)s.end_byte > line_start)
                return (double)s.ivalue / (double)PANGO_SCALE;  // tier 1
    if (line_base.leading_px > 0.0) return line_base.leading_px;  // tier 2
    if (line_base.family == box_base.family && line_base.size == box_base.size &&
        line_base.bold == box_base.bold && line_base.italic == box_base.italic)
        return box_leading;                                        // tier 3, same font
    return metric_leading_for(line_base.family, line_base.size,
                              line_base.bold, line_base.italic);    // tier 3, styled font
}

// s342 — three-tier per-paragraph indent (the indent twin of
// resolved_line_leading). Precedence, mirroring leading exactly:
//   1. a direct indent run (kCurvzIndent*Attr) covering the byte — direct
//      formatting wins outright, BY PRESENCE, so an explicit 0 beats a styled
//      non-zero value (a direct run is read with a presence test, not a >0
//      test, same as the leading pump).
//   2. the bound style's resolved indent for the axis (line_base.indent_*_px).
//   3. the 0 floor.
// Pre-s342 the call sites used indent_for_byte, which read tier 1 only and
// returned 0 otherwise -- so a bound style's indents never applied. The gap was
// invisible while direct indent spans coexisted with the binding, but a clean-
// swap apply (s342) strips the direct indent spans, leaving the style as the
// only source -- which the old reader ignored. `attr` is one of the three
// kCurvzIndent*Attr.
// s346 — de-static'd and declared in TextCursor.hpp (see resolved_line_leading).
double resolved_indent(const SceneNode* text, int attr,
                       const style::ResolvedTextStyle& line_base,
                       size_t byte) {
    if (text)
        for (const auto& s : text->text_attr_spans)
            if (s.type == attr &&
                (size_t)s.start_byte <= byte && (size_t)s.end_byte > byte)
                return (double)s.ivalue / (double)PANGO_SCALE;     // tier 1
    if (attr == curvz::utils::kCurvzIndentLeftAttr)  return line_base.indent_left_px;
    if (attr == curvz::utils::kCurvzIndentRightAttr) return line_base.indent_right_px;
    if (attr == curvz::utils::kCurvzIndentFirstAttr) return line_base.indent_first_px;
    return 0.0;                                                     // tier 3
}

// s346 — paragraph alignment through the same two tiers the fitter's align
// post-pass applied inline since s342 (now extracted HERE so the style-bar
// read shares it): a direct kCurvzAlignAttr run wins; else a bound style's
// resolved align; else 0 (left). The box text_align scalar is deliberately
// NOT a tier (s342's scoping decision, preserved): an unbound paragraph
// keeps align-span-only behaviour.
int resolved_align(const SceneNode* text,
                   const style::TextStyleLibrary* lib, size_t byte) {
    if (!text) return 0;
    for (const auto& s : text->text_attr_spans)
        if (s.type == curvz::utils::kCurvzAlignAttr &&
            (size_t)s.start_byte <= byte && (size_t)s.end_byte > byte)
            return (int)s.ivalue;                                   // tier 1
    if (lib) {
        const std::string sid = curvz::utils::paragraph_attr_svalue_for_byte(
            text->text_attr_spans, curvz::utils::kCurvzStyleAttr, byte);
        if (!sid.empty())
            return style::para_align_to_ivalue(lib->resolve(sid).align);  // tier 2
    }
    return 0;                                                       // tier 3
}

// s346 — the paragraph's tab spec through the indent/leading tiering. Tier 2
// (line_base.tabs) is NEW for the layout too: pre-s346 tabs_for_byte read
// direct runs only, so a bound style's tabs moved nothing. Both the fitter
// and the Tabs popover read through here now — they go style-aware together.
std::string resolved_tabs(const SceneNode* text,
                          const style::ResolvedTextStyle& line_base,
                          size_t byte) {
    if (text)
        for (const auto& s : text->text_attr_spans)
            if (s.type == curvz::utils::kCurvzTabsAttr &&
                (size_t)s.start_byte <= byte && (size_t)s.end_byte > byte)
                return s.svalue;                                    // tier 1
    return line_base.tabs;                                          // tier 2 / empty floor
}

// ── s347 m4 — walk-path + length pumps (see TextCursor.hpp) ─────────────────
PathData pattern_walk_path(const PathData& guide, double offset) {
    if (offset <= 0.0)
        return guide;
    if (!guide.closed) {
        // s348 m6 — open guide: a TRUE parallel on the descender side
        // (+distance is the left-perp of travel — see offset_open_path).
        // The open-path Return stack rides this exactly as closed rides
        // the inward offset.
        PathData w = offset_open_path(guide, offset);
        if (w.nodes.size() < 2)
            return PathData{};   // degenerate offset -> no line here
        return w;
    }
    std::vector<PathData> in = offset_path(guide, offset, OffsetSide::Inside);
    if (in.empty() || in.front().nodes.size() < 2)
        return PathData{};   // offset consumed the shape -> no line here
    return in.front();
}

double pattern_path_length(const PathData& path) {
    if (path.nodes.size() < 2) return 0.0;
    BezierPath bp = BezierPath::from_path_data(path);
    double total = 0.0;
    const int n = bp.segment_count();
    for (int i = 0; i < n; ++i)
        total += bp.segment(i).length(32);
    return total;
}

// ── s347 — path-text v2 m2: the PATTERN FITTER ──────────────────────────────
// (docs/text_on_path_v2.md, "Architecture: riding the TBM substrate.")
// Lays out path-attached text into BaselineLayouts carrying the pattern
// block. The byte/style side rides the SAME machinery as the box fitter —
// resolve_paragraph_baseline for the bound style, make_single_line_layout
// for the chunk (per-run direct spans, bound colour/letter-spacing baseline,
// all baked into the pango layout exactly as for a box line) — so the
// StyleBar relay, spans, and ownership tiling carry over by construction.
//
// The fitter is GEOMETRY-FREE: it never touches the guide's path data. It
// needs only the line's measured width and the node's (anchor, alignment)
// to place arc_start; wrapping a closed guide's arc / clamping an open
// guide's is the renderer's (and m3 caret's) job. That keeps TextCursor.cpp
// free of Canvas's arc machinery and the byte domain cleanly separated from
// the arc domain.
//
// m2 scope: the FIRST hard line only. L/C/R alignment maps the line's
// extent onto the arc relative to text_guide_anchor (left grows forward
// from the anchor, center straddles it, right ends at it); justify has no
// edge-to-edge meaning on an unbounded arc and degrades to left. Content
// past the first '\n' is unconsumed overflow until the m4 Return
// derivation stacks lines (closed: antipode + flip; open: PathOffset).
//
// Ownership: the single line is hard-ended by definition (a '\n' or the
// buffer end follows), so it owns [byte_start, byte_end + 1) — the caret
// position AT its end included — per the s345 fit-time assignment rules.
// An empty buffer emits one empty baseline owning position 0, so the m3
// caret has a home in a freshly attached text.
static TextLayout compute_pattern_layout(const SceneNode* guide,
                                         const SceneNode* text,
                                         size_t byte_start,
                                         const style::TextStyleLibrary* lib) {
    TextLayout out;
    if (!guide || !guide->path || !text) return out;

    const std::string& buf = text->text_content;
    if (byte_start > buf.size()) byte_start = buf.size();

    // Box-level baseline from the node's loose scalars — identical
    // derivation to the box fitter's prelude (s340 Option A).
    const style::ResolvedTextStyle box_base = style::box_baseline(
        text->text_font_family, text->text_font_size,
        text->text_bold, text->text_italic, text->text_letter_spacing,
        text->text_line_height,
        style::para_align_from_str(text->text_align));

    // ── s347 m4 — multi-line layout (Return semantics) ───────────────────
    // CLOSED guides: hard '\n' lines stack per the v2 derivation: line 0
    // rides the guide at the anchor; line 1 takes the complementary arc —
    // antipode anchor with flip (reverse traversal + π) so it reads upright
    // on the bottom; lines 2+ stack INWARD, each riding a true parallel
    // offset of the guide (pattern_walk_path) at the accumulated leading.
    // s348 m6 — OPEN guides: no antipode, no flip; lines 1+ stack BELOW the
    // guide on true open parallels (offset_open_path via the same pump) at
    // the accumulated leading. The anchor maps onto each offset line
    // PROPORTIONALLY (same fraction of that line's own length — exact for
    // circles and straight lines, reasonable everywhere), and arc_start
    // lives in the derived path's own arc space.
    //
    // Ownership tiles exactly as the box fitter's hard lines: every line —
    // including a '\n\n' empty — owns [its start, byte_end + 1), so the
    // caret position AT each newline has exactly one home.
    const bool guide_closed = guide->path && guide->path->closed;
    const double guide_total =
        guide->path ? pattern_path_length(*guide->path) : 0.0;
    // s348 m3 — the flipped family's anchor image = shared anchor + the
    // independent flip delta (Ctrl+drag), THEN the antipode half-turn.
    // Normalize after fmod: the delta may drive the sum negative.
    double anchor_frac = 0.0;
    if (guide_closed && guide_total > 0.001) {
        anchor_frac = std::fmod(
            (text->text_guide_anchor + text->text_guide_anchor_flip_delta) /
                    guide_total + 0.5,
            1.0);
        if (anchor_frac < 0.0)
            anchor_frac += 1.0;
    }

    size_t cursor = byte_start;
    size_t own_cursor = byte_start;
    int line_idx = 0;
    double stack_offset = 0.0;   // accumulated inward distance for lines 2+
    int safety = 0;
    while (cursor <= buf.size() && safety++ < 10000) {
        size_t line_end = buf.find('\n', cursor);
        if (line_end == std::string::npos) line_end = buf.size();

        const style::ResolvedTextStyle line_base =
            lib ? style::resolve_paragraph_baseline(
                      text->text_attr_spans, (unsigned)cursor, *lib, box_base)
                : box_base;

        // Metrics for the line's resolved font (same derivation as the box
        // prelude, per line so a styled paragraph sizes its own cell).
        double ascent = 0.0, descent = 0.0, metric_lead = 0.0;
        {
            cairo_surface_t* msurf =
                cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
            cairo_t* mcr = cairo_create(msurf);
            PangoLayout* mlayout = pango_cairo_create_layout(mcr);
            PangoFontDescription* mdesc = pango_font_description_new();
            pango_font_description_set_family(mdesc, line_base.family.c_str());
            const double fsize = line_base.size > 0.0 ? line_base.size : 24.0;
            pango_font_description_set_absolute_size(mdesc,
                                                     fsize * PANGO_SCALE);
            if (line_base.bold)
                pango_font_description_set_weight(mdesc, PANGO_WEIGHT_BOLD);
            if (line_base.italic)
                pango_font_description_set_style(mdesc, PANGO_STYLE_ITALIC);
            PangoContext* pctx = pango_layout_get_context(mlayout);
            PangoFontMetrics* fm =
                pango_context_get_metrics(pctx, mdesc, nullptr);
            ascent  = pango_font_metrics_get_ascent(fm)  / (double)PANGO_SCALE;
            descent = pango_font_metrics_get_descent(fm) / (double)PANGO_SCALE;
            pango_font_metrics_unref(fm);
            pango_font_description_free(mdesc);
            g_object_unref(mlayout);
            cairo_destroy(mcr);
            cairo_surface_destroy(msurf);
            metric_lead = (ascent + descent) * 1.2;
            if (metric_lead <= 0.0) metric_lead = fsize * 1.2;
            if (text->text_line_height > 0.0)
                metric_lead = text->text_line_height;
        }
        const double line_lead = resolved_line_leading(
            text, line_base, box_base, metric_lead, cursor);

        // The line's walk path: closed — guide for lines 0/1 (anchor +
        // antipode), inward offsets for 2+; open (s348 m6) — guide for
        // line 0, below-parallels for 1+ (no antipode to spend line 1 on).
        const double off =
            ((guide_closed ? line_idx >= 2 : line_idx >= 1) ? stack_offset
                                                            : 0.0);
        PathData walk = pattern_walk_path(*guide->path, off);
        const double walk_total = pattern_path_length(walk);
        const bool line_has_path = walk.nodes.size() >= 2 && walk_total > 0.001;

        PangoLayout* layout = make_single_line_layout(
            text, buf.data() + cursor, (int)(line_end - cursor),
            cursor, nullptr, 0.0, &line_base);
        int lw = 0;
        pango_layout_get_size(layout, &lw, nullptr);
        const double width = lw / (double)PANGO_SCALE;

        const int align = resolved_align(text, lib, cursor);
        const double align_off =
            (align == 1) ? width * 0.5 : (align == 2) ? width : 0.0;

        const bool flip = guide_closed && line_idx >= 1;
        double arc_start = 0.0;
        if (!flip) {
            // s348 m6 — proportional anchor mapping for unflipped lines:
            // the anchor's FRACTION of the raw guide, on THIS line's own
            // length. At offset 0 (walk_total == guide_total) this is
            // byte-identical to the raw anchor, so closed line 0 and the
            // m2 single-line case are unchanged. Offset lines (the OPEN
            // stack) are the open guide's SECOND FAMILY and add the
            // independent slide delta — the same one closed guides spend
            // on the flipped family — so Ctrl+drag works on both
            // topologies through the one node field.
            const double eff_anchor =
                (off > 0.0) ? text->text_guide_anchor +
                                  text->text_guide_anchor_flip_delta
                            : text->text_guide_anchor;
            const double A = (guide_total > 0.001)
                ? (eff_anchor / guide_total) * walk_total
                : 0.0;
            arc_start = A - align_off;
        } else {
            // Antipode mapped proportionally onto THIS line's path, then
            // into flipped pattern space (lookup = total - pattern_arc).
            const double A = anchor_frac * walk_total;
            arc_start = (walk_total - A) - align_off;
        }

        BaselineLayout bl;
        bl.y       = 0.0;
        bl.x_start = 0.0;
        bl.x_end   = width;
        bl.ascent  = ascent;
        bl.descent = descent;
        bl.height  = line_lead;
        bl.byte_start = cursor;
        bl.byte_end   = line_end;
        bl.ended_by_wrap = false;
        bl.own_start = own_cursor;
        bl.own_end   = line_end + 1;
        own_cursor   = bl.own_end;
        bl.pango.reset(layout);
        if (line_has_path) {
            bl.pattern = BaselineLayout::PatternBlock{
                text->text_guide_id, arc_start, flip, off};
        }
        // No walk path (offset consumed the shape): the baseline still
        // exists for byte/ownership (the caret can live there; nothing
        // renders), pattern stays nullopt and the renderer skips it.
        out.baselines.push_back(std::move(bl));

        if (line_end < buf.size()) {
            cursor = line_end + 1;          // consume the '\\n'
            out.bytes_consumed = cursor;
        } else {
            out.bytes_consumed = buf.size();
            break;
        }

        // Stride: closed — lines 0->1 share the guide (top/bottom), so the
        // inward stack starts accumulating from line 1; open (s348 m6) —
        // every line is the predecessor of an offset line, so each Return
        // adds the CURRENT line's leading from line 0 on.
        if (guide_closed ? line_idx >= 1 : true)
            stack_offset += line_lead;
        ++line_idx;
    }
    return out;
}

TextLayout compute_text_layout(const SceneNode* boundary,
                               const SceneNode* text,
                               size_t byte_start,
                               const style::TextStyleLibrary* lib) {
    TextLayout out;
    // s347 — path-text v2 m2: PATTERN MODE. When the text is guide-attached
    // and the caller passed its guide as the "boundary", route to the
    // pattern fitter. Sits ABOVE the nodes<3 rejection below — a 2-node
    // open line is a perfectly good guide, just not a box boundary. Box
    // callers are unaffected (a TBM text never carries text_guide_id).
    if (text && boundary && !text->text_guide_id.empty() &&
        boundary->internal_id == text->text_guide_id)
        return compute_pattern_layout(boundary, text, byte_start, lib);
    if (!boundary || !boundary->path || !text) return out;
    const PathData& bp = *boundary->path;
    if (bp.nodes.size() < 3) return out;

    // s340 — the box-level baseline (Option A): the unbound paragraph's font
    // default, from the node's loose scalars. Per paragraph below,
    // resolve_paragraph_baseline returns this when the paragraph has no style
    // binding, or the resolved style when it does. When `lib` is null (no
    // project context — some internal callers) every paragraph uses this box
    // baseline, i.e. byte-identical to pre-s340 behaviour. (leading/align here
    // are carried for completeness; this milestone only consumes the font half
    // — family/size/bold/italic — at make_single_line_layout. The leading/align/
    // indent defaults under the per-paragraph spans are the next step.)
    const style::ResolvedTextStyle box_base = style::box_baseline(
        text->text_font_family, text->text_font_size,
        text->text_bold, text->text_italic, text->text_letter_spacing,
        text->text_line_height,
        style::para_align_from_str(text->text_align));

    // s320 m1 — Frame rotation. Lay out in the boundary's UPRIGHT frame:
    // rotate every node by -angle about the centroid, bbox THAT, and run the
    // existing axis-aligned layout there. The baselines come out in upright
    // coords; the renderer and caret rotate them back via (frame_angle,
    // frame_cx, frame_cy). angle == 0 reproduces the old doc-space bbox
    // exactly (cos=1, sin=0), so the common rect case is byte-identical.
    double fangle, fcx, fcy;
    text_frame_basis(boundary, text, fangle, fcx, fcy);
    out.frame_angle = fangle;
    out.frame_cx    = fcx;
    out.frame_cy    = fcy;
    const double ca = std::cos(-fangle), sa = std::sin(-fangle);

    // Boundary bbox in the upright frame (rect case; the rotated rect maps
    // back to an axis-aligned rect here).
    auto upright = [&](double x, double y, double& ux, double& uy) {
        double rx = x - fcx, ry = y - fcy;
        ux = fcx + rx * ca - ry * sa;
        uy = fcy + rx * sa + ry * ca;
    };
    double bx0, by0; upright(bp.nodes[0].x, bp.nodes[0].y, bx0, by0);
    double bx1 = bx0, by1 = by0;
    for (const auto& n : bp.nodes) {
        double ux, uy; upright(n.x, n.y, ux, uy);
        if (ux < bx0) bx0 = ux; if (ux > bx1) bx1 = ux;
        if (uy < by0) by0 = uy; if (uy > by1) by1 = uy;
    }
    // s323 (form-fit B) — the text margin is one UNIFORM inward erosion of
    // the whole boundary outline, not four per-edge rect subtractions
    // (per-edge has no meaning on a blob). Map the boundary into the upright
    // frame (anchors + both bezier handles rotate about the centroid) so the
    // eroded region and the baseline ribbons share one frame for the Clipper
    // intersect; the geometry pumps stay frame-agnostic, the caller keeps the
    // frame consistent. effective_text_margins still sources the value; the
    // four margins collapse to a single inset (the minimum, so we never erode
    // past the smallest requested margin — per-edge rect margins become a
    // later needle-frame nicety if wanted). Zero margins -> inset 0 -> erosion
    // is identity, text flows to the boundary, byte-identical to the old rect.
    auto m = effective_text_margins(text, boundary);
    double inset = std::min(std::min(m.left, m.right),
                            std::min(m.top,  m.bottom));
    if (inset < 0.0) inset = 0.0;

    PathData upright_boundary;
    upright_boundary.closed = true;
    upright_boundary.nodes.reserve(bp.nodes.size());
    for (const auto& n : bp.nodes) {
        BezierNode un = n;
        upright(n.x,   n.y,   un.x,   un.y);
        upright(n.cx1, n.cy1, un.cx1, un.cy1);
        upright(n.cx2, n.cy2, un.cx2, un.cy2);
        upright_boundary.nodes.push_back(un);
    }

    std::vector<PathData> eroded = erode_outline(upright_boundary, inset);
    if (eroded.empty()) return out;   // margin consumed the shape -> no text

    // Vertical extent of the eroded region (upright frame). The first baseline
    // floats off the REAL eroded top (the rim), not bbox_top + ascent; the
    // bottom bound replaces the old interior iy1. Read off the eroded vertices
    // — for M1's rect/ellipse/polygon boundaries the offset output is dense
    // enough that vertex extrema match the true rim.
    double eroded_top    =  std::numeric_limits<double>::max();
    double eroded_bottom = -std::numeric_limits<double>::max();
    for (const auto& piece : eroded)
        for (const auto& n : piece.nodes) {
            if (n.y < eroded_top)    eroded_top    = n.y;
            if (n.y > eroded_bottom) eroded_bottom = n.y;
        }
    if (eroded_bottom <= eroded_top) return out;

    const double font_size = text->text_font_size > 0.0
                                 ? text->text_font_size : 24.0;

    // s301 m1d — Real Pango font metrics for ascent/descent/leading.
    // Previously approximated as 0.8 × font_size; with the
    // approximation a hairline at "baseline" cut through the middle
    // of lowercase letters because 0.8 is wrong for most fonts at
    // typical sizes. Pango's font_metrics give the true ascent (top
    // of glyph cell → baseline) for the chosen family + size + style,
    // and the hairline placed at baseline_y = interior.top + ascent
    // sits exactly at the bottom of lowercase letters as expected.
    //
    // Fetch once per layout call (font is constant for the whole text
    // node). Pango caches the metrics internally so this is cheap on
    // repeated calls with the same font description.
    cairo_surface_t* metrics_surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t* metrics_cr = cairo_create(metrics_surf);
    PangoLayout* metrics_layout = pango_cairo_create_layout(metrics_cr);
    PangoFontDescription* metrics_desc = pango_font_description_new();
    pango_font_description_set_family(metrics_desc,
                                       text->text_font_family.c_str());
    pango_font_description_set_absolute_size(metrics_desc,
                                              font_size * PANGO_SCALE);
    if (text->text_bold)
        pango_font_description_set_weight(metrics_desc, PANGO_WEIGHT_BOLD);
    if (text->text_italic)
        pango_font_description_set_style(metrics_desc, PANGO_STYLE_ITALIC);
    PangoContext* pctx = pango_layout_get_context(metrics_layout);
    PangoFontMetrics* fmetrics = pango_context_get_metrics(pctx, metrics_desc,
                                                            nullptr);
    double ascent  = pango_font_metrics_get_ascent(fmetrics)
                     / (double)PANGO_SCALE;
    double descent = pango_font_metrics_get_descent(fmetrics)
                     / (double)PANGO_SCALE;
    // Leading: standard typographic line height is ascent + descent
    // plus a small visual gap. Many fonts produce ascent+descent ≈
    // 1.0 × em-size already; we add 20% as the line-spacing default
    // to match the prior look.
    // s326 m2b — an explicit text_line_height (doc units) overrides the
    //   derivation; 0 (the default) keeps the metric-based 1.2x so existing
    //   boxes are byte-identical to before this field existed.
    double leading = (ascent + descent) * 1.2;
    if (leading <= 0.0) leading = font_size * 1.2;  // defensive fallback
    if (text && text->text_line_height > 0.0)
        leading = text->text_line_height;
    pango_font_metrics_unref(fmetrics);
    pango_font_description_free(metrics_desc);
    g_object_unref(metrics_layout);
    cairo_destroy(metrics_cr);
    cairo_surface_destroy(metrics_surf);

    // s323 — cap-float first baseline: the cap line (baseline - ascent) clears
    // the eroded top exactly at eroded_top, so the first baseline is
    // eroded_top + ascent. For a flat rim this reproduces the old rect result;
    // for a curved rim it floats with the real geometry (the first line sits
    // just under the peak with a narrow span there, and text falls through to
    // the wider lines below). If even the first baseline overshoots the floor,
    // no baselines are emitted and bytes_consumed stays 0.
    double first_baseline_y = eroded_top + ascent;

    // s323 — base baseline (upright frame): a straight horizontal line at the
    // first-baseline y, overhanging the bbox on both ends so every run
    // boundary in the Clipper intersect is a true margin crossing, never a
    // ribbon terminus. intervals_for_baseline translates it DOWN by k*leading
    // for line k; line 0 is the base baseline itself. (Per-view custom/curvy
    // base baselines are deferred — M1 is straight-by-default.)
    const double overhang = std::max(bx1 - bx0, 1.0);
    PathData base_baseline;
    base_baseline.closed = false;
    {
        BezierNode a; a.x = bx0 - overhang; a.y = first_baseline_y;
        a.cx1 = a.cx2 = a.x; a.cy1 = a.cy2 = a.y;
        BezierNode b; b.x = bx1 + overhang; b.y = first_baseline_y;
        b.cx1 = b.cx2 = b.x; b.cy1 = b.cy2 = b.y;
        base_baseline.nodes.push_back(a);
        base_baseline.nodes.push_back(b);
    }

    double baseline_y = first_baseline_y;

    // s317 — flow resolver: a member view lays out its slice of the shared
    // Mgr buffer starting at byte_start (the running offset handed down by
    // the Mgr render loop). Clamp defensively; byte_start == size means the
    // upstream members already consumed everything, so this view emits only
    // empty baselines (the buffer-exhausted branch below). The emitted
    // baselines carry ABSOLUTE byte offsets into text_content, and
    // bytes_consumed is the absolute end offset, so the caller can chain
    // offset = bytes_consumed into the next member with no bookkeeping.
    size_t cursor = std::min(byte_start, text->text_content.size());
    const std::string& buf = text->text_content;
    // s345 — running caret-position ownership cursor. Each push site claims
    // a half-open range of caret positions [own_cursor, own_end) and
    // advances own_cursor; the resulting per-baseline own ranges tile the
    // flow window exactly (no gaps, no overlaps). This is the fit-time
    // assignment behind baseline_index_for's lookup — the fitter KNOWS each
    // line's kind (soft wrap / hard end / \n-empty / neck / capacity), so
    // ownership is decided here once instead of re-guessed from buffer
    // content by every reader.
    size_t own_cursor = cursor;
    int safety = 0;
    while (baseline_y <= eroded_bottom && cursor <= buf.size() && safety++ < 10000) {
        // s331 — the byte that starts this line; sits in the owning paragraph,
        // so it resolves the per-paragraph leading for the stride below.
        const size_t line_start = cursor;
        // s341 — resolve THIS line's paragraph baseline + stride up-front (the
        // baseline resolve used to live below the span block; hoisting it lets
        // the empty-span stride here use the styled leading too). para_start
        // gates the first-line indent in the indent block further down.
        const bool para_start =
            (line_start == 0) || (buf[line_start - 1] == '\n');
        const style::ResolvedTextStyle line_base =
            lib ? style::resolve_paragraph_baseline(
                      text->text_attr_spans, (unsigned)line_start, *lib, box_base)
                : box_base;
        const double line_lead =
            resolved_line_leading(text, line_base, box_base, leading, line_start);
        // s323 (Arc E) — per-baseline span from the outline intersection.
        // M1 takes the single leftmost run (concave multi-span gap-flow is
        // deferred). dy is measured from the base baseline (line 0) so it
        // stays exact as baseline_y accumulates float leading. An empty span
        // means this baseline misses the eroded shape (above the rim, below
        // the floor, or through a pinch gap): stride down without emitting.
        double dy = baseline_y - first_baseline_y;
        std::vector<BaselineSpan> spans =
            intervals_for_baseline(eroded, base_baseline, dy);
        if (spans.empty()) {
            baseline_y += line_lead;
            continue;
        }
        double x_start = spans.front().start.x;
        double x_end   = spans.front().end.x;
        // s334 — per-paragraph indents inset the line's span BEFORE wrap and
        // alignment (so breaking, L/C/R and justify all operate on the inset
        // width). Left/right inset every line of the paragraph; first-line adds
        // to the left inset only on the paragraph's first visual line — a hard
        // boundary (buffer start or just after '\n'); soft-wrap continuations
        // are not paragraph starts, so they keep the plain left inset. Negative
        // first-line is a hanging indent. Clamp so an over-large inset never
        // inverts the span.
        //
        // s337 m2 — ind_l / ind_f are lifted to loop-body scope (was an inner
        // block) because the per-line PangoTabArray needs the indent offset: a
        // stop's pos is doc-px from the content-area left edge, the layout is
        // placed at the indented x_start, so layout-local tab loc = pos
        // - (ind_l [+ ind_f]). See build_line_tab_array.
        const double ind_l = resolved_indent(
            text, curvz::utils::kCurvzIndentLeftAttr,  line_base, line_start);
        const double ind_r = resolved_indent(
            text, curvz::utils::kCurvzIndentRightAttr, line_base, line_start);
        const double ind_f = para_start
            ? resolved_indent(text, curvz::utils::kCurvzIndentFirstAttr,
                              line_base, line_start)
            : 0.0;
        x_start += ind_l + ind_f;
        x_end   -= ind_r;
        if (x_end < x_start) x_end = x_start;
        // s337 m2 — offset from the tab-spec origin (content-area left edge) to
        // this line's layout origin (x_start). ind_f is already 0 on
        // continuation lines, so this is ind_l there and ind_l+ind_f on a
        // paragraph's first line. Resolve the paragraph's tab spec once per line.
        const double tab_indent_offset = ind_l + ind_f;
        const std::string line_tabs = resolved_tabs(text, line_base, line_start);
        const char* line_tabs_c = line_tabs.empty() ? nullptr : line_tabs.c_str();
        double avail   = x_end - x_start;

        // Build a single-line layout for the remaining buffer.
        const char* remaining     = buf.c_str() + cursor;
        size_t       remaining_n  = buf.size() - cursor;

        BaselineLayout bl;
        bl.y          = baseline_y;
        bl.x_start    = x_start;
        bl.x_end      = x_end;
        bl.angle      = 0.0;
        bl.ascent     = ascent;
        bl.descent    = descent;  // s301 m1h: caret height needs descent separately
        bl.height     = line_lead;
        bl.byte_start = cursor;

        // s339 — if any stop on this line carries a leader, record ALL its
        // surviving stops in layout-local doc-px (ascending), each with its
        // leader. The draw needs the full set, not just leadered ones: a tab
        // consumes the first stop PAST its pen (Pango's tab rule), so to know
        // whether THAT stop has a leader we must see the no-leader stops too --
        // otherwise a leadered stop further right would be mis-attributed to a
        // tab that actually landed on an earlier no-leader stop. Same filter the
        // PangoTabArray uses, so the locations match where tabs land. No-leader
        // lines stay empty (cheap skip in the draw pass).
        {
          auto surv = surviving_tab_stops(line_tabs_c, tab_indent_offset);
          bool any_leader = false;
          for (const auto& st : surv)
            if (st.leader != curvz::utils::TabLeader::None) { any_leader = true; break; }
          if (any_leader)
            for (const auto& st : surv)
              bl.tab_locs.push_back(
                  { (double)st.loc_pango / (double)PANGO_SCALE, st.leader });
        }

        if (remaining_n == 0) {
            // s301 m1e — Buffer exhausted. Emit this baseline empty and
            // continue striding down to fill the remaining interior with
            // empty baselines, so the user sees the bbox's full line
            // capacity (every baseline that fits gets a hairline). Caret
            // navigation past end-of-buffer can land on these too —
            // they're real baselines, just without content.
            PangoLayout* layout = make_single_line_layout(text, "", 0, 0, nullptr, 0.0, &line_base);
            bl.pango.reset(layout);
            bl.byte_end = cursor;
            // s345 — ownership: the FIRST capacity line claims the
            // end-of-buffer caret position iff it is still unclaimed
            // (a trailing '\n' hard-ended the last content line at
            // byte_end+1 == buf.size(), leaving own_cursor == buf.size());
            // when the buffer ends in content, that line already claimed
            // byte_end+1 == buf.size()+1 and capacity lines own nothing.
            // This replaces the crossed_newline / m4f-fallback guesswork.
            bl.own_start = own_cursor;
            bl.own_end   = std::max(own_cursor, buf.size() + 1);
            own_cursor   = bl.own_end;
            out.baselines.push_back(std::move(bl));
            out.bytes_consumed = cursor;
            baseline_y += line_lead;
            continue;
        }

        // s323 — fall-through wrap (Scott's rule): if the next word does not
        // fit THIS line's span, don't break the word — emit an empty baseline
        // here (a real but too-narrow line, e.g. the neck of a vase) and fall
        // through to the next baseline's (possibly wider) span WITHOUT
        // advancing the byte cursor. The same bytes retry the next line. A
        // word wider than EVERY line in the shape never places: the loop ends
        // at eroded_bottom and the bytes become overflow (today's contract:
        // bytes_consumed < size, the user resizes the shape). Forcing it onto
        // the widest line and clipping is the deferred floor.
        if (remaining[0] != '\n' &&
            measure_first_word_width(text, remaining, remaining_n, cursor, &line_base) > avail) {
            PangoLayout* empty = make_single_line_layout(text, "", 0, 0, nullptr, 0.0, &line_base);
            bl.pango.reset(empty);
            bl.byte_end = cursor;          // empty line; owns no bytes
            // s345 — ownership: a neck line owns NO caret positions. It is
            // a geometric artifact (the span was too narrow), not a place
            // the caret can live. The old empty_owns heuristic could
            // falsely claim one when a hard '\n' preceded the neck
            // (crossed_newline true, byte_start == position) — the caret
            // landed on the artifact. Fit-time assignment knows better.
            bl.own_start = own_cursor;
            bl.own_end   = own_cursor;
            out.baselines.push_back(std::move(bl));
            baseline_y += line_lead;  // cursor NOT advanced
            continue;
        }

        PangoLayout* layout = make_single_line_layout(text,
                                                       remaining,
                                                       (int)remaining_n,
                                                       cursor,
                                                       line_tabs_c,
                                                       tab_indent_offset,
                                                       &line_base);
        size_t consumed_on_line = fit_chunk_to_width(remaining, remaining_n,
                                                      layout, avail);
        // Special case: a leading '\n' makes fit_chunk_to_width return 0,
        // meaning "this line is empty; the very first character of the
        // remaining buffer is a hard newline." Emit an empty baseline and
        // consume the newline so the next iteration starts after it.
        if (consumed_on_line == 0 && remaining[0] == '\n') {
            g_object_unref(layout);
            PangoLayout* empty = make_single_line_layout(text, "", 0, 0, nullptr, 0.0, &line_base);
            bl.pango.reset(empty);
            bl.byte_end = cursor;
            // s345 — ownership: a hard-ended empty line owns exactly the
            // caret position AT its '\n' (the "between two newlines"
            // position of a \n\n run). This is what the crossed_newline
            // heuristic was reverse-engineering from buf[B-1]; stored
            // here, the empty line legitimately owns its one position.
            bl.own_start = own_cursor;
            bl.own_end   = cursor + 1;
            own_cursor   = bl.own_end;
            out.baselines.push_back(std::move(bl));
            cursor += 1;  // consume the '\n'
            out.bytes_consumed = cursor;
            baseline_y += line_lead;
            continue;
        }
        if (consumed_on_line == 0) {
            // Nothing fit at all (interior too narrow for even one
            // codepoint). Bail to avoid infinite loop; remaining bytes
            // are unrendered overflow.
            g_object_unref(layout);
            out.bytes_consumed = cursor;
            return out;
        }

        // Rebuild the layout with exactly the consumed substring so the
        // glyph painter can render this baseline directly. (The
        // fit_chunk_to_width layout was over the full remaining; we
        // want one scoped to this line for rendering.)
        g_object_unref(layout);
        PangoLayout* line_layout = make_single_line_layout(text,
                                                            remaining,
                                                            (int)consumed_on_line,
                                                            cursor,
                                                            line_tabs_c,
                                                            tab_indent_offset,
                                                            &line_base);
        // s317 — Belt-and-braces: the chosen break is verified against the
        //   ACTUAL rendered width. If the line is still wider than the
        //   interior (the index_to_pos estimate over-reached by a word),
        //   back off to the previous word boundary and rebuild — never let
        //   a word render past the margin and get clipped mid-word. A
        //   single unbreakable word that exceeds the width is left as-is
        //   (the paint clip is the final guard for that rare case).
        {
            const int avail_pango = (int)(avail * PANGO_SCALE);
            // s317 — Tolerance. fit_chunk_to_width measures candidate breaks
            //   with index_to_pos over the FULL single-line layout; this
            //   backoff re-measures with a freshly shaped per-line layout.
            //   The two can disagree by a sub-pixel at the margin, and a
            //   strict `lw > avail_pango` then drops a word that actually
            //   fits — leaving a whole word's worth of trailing gap (the
            //   line was short, so the gap looks large). Only back off when
            //   the overrun exceeds a slack well under any real word width
            //   (~0.3em): sub-pixel noise is absorbed, a genuine word past
            //   the margin (tens of units) still triggers the backoff.
            const int slack_pango = (int)(font_size * 0.3 * PANGO_SCALE);
            int lw = 0, lh = 0;
            pango_layout_get_size(line_layout, &lw, &lh);
            while (lw > avail_pango + slack_pango && consumed_on_line > 1) {
                size_t shorter = last_word_break_before(remaining,
                                                         consumed_on_line);
                while (shorter > 0 &&
                       (remaining[shorter - 1] == ' ' ||
                        remaining[shorter - 1] == '\t')) {
                    --shorter;
                }
                if (shorter == 0 || shorter >= consumed_on_line)
                    break;   // single long word: leave it, clip will bound it
                consumed_on_line = shorter;
                g_object_unref(line_layout);
                line_layout = make_single_line_layout(text, remaining,
                                                       (int)consumed_on_line,
                                                       cursor,
                                                       line_tabs_c,
                                                       tab_indent_offset,
                                                       &line_base);
                pango_layout_get_size(line_layout, &lw, &lh);
            }
        }
        bl.pango.reset(line_layout);

        // s305 m4e — Auto-wrap trailing whitespace handling. fit_chunk_to_width
        //   strips trailing spaces/tabs from consumed_on_line so the rendered
        //   line doesn't end in a visible space. But those stripped bytes
        //   need to "belong" somewhere — otherwise they show as a leading
        //   indent on the NEXT baseline (Pango will render them) and the
        //   caret can't reach them sensibly. Standard text-editor behaviour:
        //   the trailing space is consumed by the wrap. We model that by
        //   extending the previous baseline's byte_end through the run of
        //   whitespace, so the chars are "owned" by line N (caret nav lands
        //   on them, selection includes them) but NOT in line N's Pango
        //   layout (so they don't render). Line N+1 then starts at the
        //   first non-whitespace byte.
        //
        //   Hard newlines are NOT absorbed here — the explicit \n-consume
        //   below still runs. Trailing spaces ahead of a user-typed \n
        //   (e.g. "hello   \n") are user content, not a wrap artifact,
        //   so the absorb stops before any \n.
        size_t line_consumed_end = cursor + consumed_on_line;
        size_t absorb = line_consumed_end;
        while (absorb < buf.size() &&
               (buf[absorb] == ' ' || buf[absorb] == '\t')) {
            ++absorb;
        }
        // Only extend if absorbed run isn't immediately followed by EOF
        // (a buffer-final whitespace run is user content — let the caret
        // see it on the last line) and isn't followed by a hard newline.
        bool is_soft_wrap_absorb = (absorb > line_consumed_end) &&
                                    (absorb < buf.size()) &&
                                    (buf[absorb] != '\n');
        if (is_soft_wrap_absorb) {
            bl.byte_end = absorb;
            cursor = absorb;
        } else {
            bl.byte_end = line_consumed_end;
            cursor = line_consumed_end;
        }
        // s333 — paragraph-end detection for justify. cursor now points at the
        // first byte NOT on this line (the hard-'\n' consume below hasn't run
        // yet). The line ended by a SOFT wrap iff more non-newline content
        // follows: buffer not exhausted AND the next byte isn't a hard '\n'.
        // A pending '\n' (hard paragraph break) or end-of-buffer means this is
        // the paragraph's last line -> stays left under justify.
        bl.ended_by_wrap = (cursor < buf.size()) && (buf[cursor] != '\n');

        // s344 m1 — Manual soft-hyphen rendering (the one mechanism both hyphen
        // sources ride). fit_chunk_to_width breaks at a soft hyphen (U+00AD;
        // Pango's log-attrs set is_line_break there), but U+00AD is zero-width,
        // so a line that broke mid-word shows no dash. Detect that break (the
        // line's last two content bytes are the UTF-8 soft hyphen C2 AD, and
        // content continues -> a real word split) and render a visible dash by
        // APPENDING a '-' to THIS line's render layout, past all content.
        //
        // s344 fix — APPEND, do not substitute. An earlier version swapped the
        // 2-byte soft hyphen for a 1-byte '-', making the layout one byte
        // shorter than the buffer slice; the caret maps byte_index->x via
        // pango_layout_index_to_pos on this layout, so every hyphenated line
        // drifted the mapping and the caret jumped / cycled. Appending keeps the
        // zero-width soft hyphen in place (Pango renders it invisible -- a
        // width=-1 single-line layout never hits its own hyphen path), so every
        // buffer byte maps to the identical layout byte; the extra '-' sits at
        // the very end, beyond byte_end, where no caret position lives. One
        // visible dash, byte-faithful. Baking it into bl.pango still means the
        // renderer, justify, and the outline extractor all show it for free.
        bl.ended_by_hyphen = bl.ended_by_wrap && line_consumed_end >= 2 &&
            (unsigned char)buf[line_consumed_end - 2] == 0xC2 &&
            (unsigned char)buf[line_consumed_end - 1] == 0xAD;
        // s344 — Dash rendering DISABLED for isolation. Baking a dash into the
        // line layout (append or substitute) is the only thing the hyphen work
        // changed about the layout the CARET reads, and a caret regression
        // appeared. Leaving bl.pango as the exact buffer slice here returns the
        // per-line layout to byte-AND-char-identical-to-pre-hyphenation. The
        // ended_by_hyphen flag is still recorded (no consumer yet). If the caret
        // is healthy with this off, the dash returns as a draw-time OVERLAY that
        // never mutates the layout (so it can't perturb caret byte-mapping).
        // Re-enable by restoring the make_single_line_layout swap below.
        //
        // if (bl.ended_by_hyphen) {
        //     std::string line_text(remaining, consumed_on_line);
        //     line_text.push_back('-');
        //     PangoLayout* hy = make_single_line_layout(
        //         text, line_text.c_str(), (int)line_text.size(),
        //         bl.byte_start, line_tabs_c, tab_indent_offset, &line_base);
        //     bl.pango.reset(hy);
        // }
        // s345 — ownership: a soft-wrapped line's boundary position belongs
        // to the NEXT line (s305 m4d later-wins, unchanged); a hard-ended
        // line ('\n' follows, or end of buffer) also owns the position AT
        // its byte_end — the caret "before the newline" / "after the last
        // char". This subsumes the s338/s344 hard_end heuristic AND the
        // s305 m4f end-of-buffer fallback in one stored fact.
        bl.own_start = own_cursor;
        bl.own_end   = bl.ended_by_wrap ? bl.byte_end : bl.byte_end + 1;
        own_cursor   = bl.own_end;
        out.baselines.push_back(std::move(bl));

        // If we broke at a '\n', consume that newline byte too (it
        // shouldn't appear on the next line).
        if (cursor < buf.size() && buf[cursor] == '\n') {
            cursor += 1;
        }
        out.bytes_consumed = cursor;

        baseline_y += line_lead;
    }

    // s339 — the s337 m2a safety-cap diagnostic was stripped here; the popover
    // hang it was armed for is confirmed closed (s338). The `safety` counter on
    // the wrap loop above stays as the geometry-loop guard; only its WARN report
    // is gone.

    // ── s332 — per-paragraph alignment ──────────────────────────────────────
    // Shift each baseline's x_start within its available span [x_start, x_end]
    // by (avail - text_width) * factor, where the factor is the line's
    // paragraph alignment (kCurvzAlignAttr at the line's first byte; 0 = left =
    // no shift, 1 = centre, 2 = right). Done as a post-pass on the finished
    // baselines: every consumer that keys off x_start — glyph draw, caret,
    // selection highlight, click hit-test, the stroke valve — inherits the
    // alignment automatically, so there's exactly one place to get right and
    // the visible text never drifts from the caret/selection geometry. avail
    // varies per line inside a curvy boundary, so each line aligns within its
    // own width (centre text in a vase outline -> each line centres in its
    // own span). Justify (3) is handled here too as of s333: it is NOT an
    // x-shift -- the line stays at its span's left edge and Pango stretches
    // the glyphs to fill the span by widening inter-word gaps.
    if (text) {
        for (auto& bl : out.baselines) {
            if (!bl.pango) continue;
            // s346 — the direct-run -> bound-style tiering this loop carried
            // inline since s342 now lives in resolved_align (TextCursor.hpp),
            // shared with the style-bar's alignment read. Semantics unchanged.
            int align = resolved_align(text, lib, bl.byte_start);
            if (align <= 0) continue;  // left default — no shift

            if (align == 3) {
                // s333 — Justify. Only soft-wrapped lines with content stretch;
                // a paragraph's final line (ended_by_wrap == false) and empty
                // lines stay left-natural. x_start is unchanged (the line still
                // begins at its span's left edge); Pango spreads within `width`.
                //
                // Each baseline is its OWN single-line layout, so this one line
                // is the layout's "last line" — Pango won't justify the last
                // line unless told to. set_justify_last_line(TRUE) (Pango 1.50+,
                // pango 1.54 in the build) forces the stretch; our ended_by_wrap
                // gate above is what enforces the real paragraph-last-line rule.
                if (!bl.ended_by_wrap) continue;
                if (bl.byte_end <= bl.byte_start) continue;  // empty line
                double avail = bl.x_end - bl.x_start;
                if (avail <= 0.0) continue;

                // ── s333 — letter-spacing SPILL (river suppression) ──────────
                // Plain Pango justify widens only the inter-word SPACES, equally
                // (Bresenham-rounded). On short lines with few words that yawns
                // the gaps into "rivers." We cap how far a space may stretch and
                // spill the OVERFLOW into a small, bounded letter-spacing — which
                // Pango is NOT allowed to count as expandable, so it composes
                // cleanly: we widen letters by a fixed amount, Pango then
                // recomputes the (now larger) natural width and distributes only
                // the REMAINING slack across the spaces. Result: spaces carry the
                // load up to a comfort ceiling, letters quietly absorb the rest.
                //
                // CRITICAL — this is NOT a stored property. It is a transient
                // layout output, computed here from THIS line's bytes and THIS
                // line's `avail`, applied to the freshly-built `bl.pango`, and
                // never written back to text_attr_spans. compute_text_layout
                // re-runs on every draw / caret move / box resize / edit, so a
                // tb resize or an added word re-breaks the line and recomputes
                // the spill from scratch. The only durable thing is the INTENT
                // (kCurvzAlignAttr ivalue=3). Do NOT "optimize" by caching the
                // letter-spacing on the node or as a span — that reintroduces the
                // staleness bug (a value computed for a line that no longer
                // exists). The spill must die with the layout object every frame.
                //
                // The two knobs are live-tunable (TEMP slider) — see the file-
                // scope g_justify_* statics. comfort = extra a space may gain
                // before letters help; track = letter-spacing ceiling.
                const double kComfortSpaceEm = g_justify_comfort_em;
                const double kMaxTrackEm     = g_justify_track_em;
                int w0px = 0;
                pango_layout_get_pixel_size(bl.pango.get(), &w0px, nullptr);
                double slack = avail - (double)w0px;
                if (slack > 0.0) {
                    gint n_attrs = 0;
                    const PangoLogAttr* la =
                        pango_layout_get_log_attrs_readonly(bl.pango.get(),
                                                            &n_attrs);
                    int n_spaces = 0;
                    for (int i = 0; i < n_attrs; ++i)
                        if (la[i].is_expandable_space) ++n_spaces;
                    int n_chars = pango_layout_get_character_count(bl.pango.get());
                    int gaps    = std::max(1, n_chars - 1);

                    double comfort = font_size * kComfortSpaceEm;
                    double max_ls  = font_size * kMaxTrackEm;
                    double ls_doc  = 0.0;
                    if (n_spaces > 0) {
                        // Spill only the excess beyond what spaces can carry
                        // comfortably; if spaces alone stay under the ceiling,
                        // leave letters untouched and let plain justify run.
                        double per_space = slack / (double)n_spaces;
                        if (per_space > comfort) {
                            double excess = slack - comfort * (double)n_spaces;
                            ls_doc = excess / (double)gaps;
                        }
                    } else {
                        // Single word, no expandable spaces — letters are the
                        // ONLY stretch path (Pango won't cluster-justify Latin).
                        ls_doc = slack / (double)gaps;
                    }
                    if (ls_doc > max_ls) ls_doc = max_ls;

                    // ── STRUCTURAL anti-rewrap clamp (belt-and-braces) ──────
                    // Pango decides WRAPPING from the line's natural width; we
                    // set the layout width to `avail` below. If letter-spacing
                    // inflates the natural width to >= avail, Pango re-wraps the
                    // single-line layout into TWO lines and the renderer stacks
                    // them at one baseline -> shattered glyphs. So letter-spacing
                    // must never push the natural width up to avail. Pango can
                    // apply spacing to EVERY cluster (incl. edges), so bound the
                    // growth conservatively by n_chars (not interior gaps) and
                    // leave a 2px margin of slack for the word-gaps. Justify then
                    // fills that residual into the spaces. This makes a re-wrap
                    // impossible at ANY knob value -- the knob affects looks, not
                    // correctness.
                    double max_ls_fit =
                        (slack - 2.0) / (double)std::max(1, n_chars);
                    if (max_ls_fit < 0.0) max_ls_fit = 0.0;
                    if (ls_doc > max_ls_fit) ls_doc = max_ls_fit;

                    int ls_pango = (int)(ls_doc * PANGO_SCALE + 0.5);
                    if (ls_pango > 0) {
                        // Add a whole-line letter-spacing attr ON TOP of the
                        // existing per-run attrs; copy + set_attributes forces
                        // Pango to re-shape (mutating the cached list in place
                        // would not invalidate the layout). end_index G_MAXUINT
                        // = "to end of text," so no byte-length bookkeeping.
                        PangoAttrList* base =
                            pango_layout_get_attributes(bl.pango.get());
                        PangoAttrList* nl = base ? pango_attr_list_copy(base)
                                                 : pango_attr_list_new();
                        PangoAttribute* a = pango_attr_letter_spacing_new(ls_pango);
                        a->start_index = 0;
                        a->end_index   = G_MAXUINT;
                        pango_attr_list_insert(nl, a);  // takes ownership of `a`
                        pango_layout_set_attributes(bl.pango.get(), nl);
                        pango_attr_list_unref(nl);
                    }
                }
                // ─────────────────────────────────────────────────────────────

                pango_layout_set_width(bl.pango.get(),
                                       (int)(avail * PANGO_SCALE));
                pango_layout_set_justify(bl.pango.get(), TRUE);
                pango_layout_set_justify_last_line(bl.pango.get(), TRUE);
                continue;
            }

            // centre (1) / right (2) — measure the natural width and shift.
            int wpx = 0;
            pango_layout_get_pixel_size(bl.pango.get(), &wpx, nullptr);
            double slack = (bl.x_end - bl.x_start) - (double)wpx;
            if (slack <= 0.0) continue;  // text fills/overflows the span
            bl.x_start += slack * ((align == 1) ? 0.5 : 1.0);  // centre / right
        }
    }

    return out;
}

// ── Geometry — consumes compute_text_layout ─────────────────────────────────
// Given the buffer's current layout, find which baseline contains
// m_byte_index and ask its Pango layout for the cursor x within the
// line. Translate back into doc space.
TextCursor::Geometry TextCursor::position_on_canvas() const {
    Geometry g;
    if (!m_text || !m_canvas) return g;

    // Boundary resolution — three paths in priority order:
    //   1. m_boundary set: TextBox-owned text passes its sibling
    //      boundary into the ctor (and the ToP edit entry passes the
    //      GUIDE here — s347 m3); direct pointer, no lookup.
    //   2. s347 — guide-attached text: resolve the guide by iid so a
    //      cursor constructed without the pointer (re-entry paths)
    //      still lays out through the pattern fitter.
    //   3. Legacy paired-sibling: read text_boundary_ids.front() and
    //      look the boundary up by iid through Canvas. Works for
    //      files loaded from before the TextBox migration.
    SceneNode* boundary = m_boundary;
    if (!boundary && !m_text->text_guide_id.empty())
        boundary = m_canvas->find_guide_path(m_text->text_guide_id);
    if (!boundary) {
        if (m_text->text_boundary_ids.empty()) return g;
        boundary = m_canvas->find_text_boundary(
            m_text->text_boundary_ids.front());
        if (!boundary) return g;
    }

    TextLayout tl = layout_for(boundary);  // s345 — styled layout, see layout_for
    if (tl.baselines.empty()) return g;

    // s345 — resolve through THE resolver (stored own ranges), the same
    //   lookup move_up/down, Home/End, and select_line_at use, so the
    //   renderer and every stepper agree BY CONSTRUCTION. The s305 m4d
    //   later-wins, s307 crossed-newline, s338 hard_end, and s305 m4f
    //   end-of-buffer conventions are all encoded in the fitter's
    //   ownership assignment; nothing is re-derived here.
    const std::string& buf = m_text->text_content;
    const BaselineLayout* target = nullptr;
    {
        size_t idx = baseline_index_for(tl, m_byte_index);
        if (idx == size_t(-1)) return g;
        target = &tl.baselines[idx];
    }
    // (The old s305 m4f fallback block lived here; the resolver now
    //  carries that behaviour internally — last_content_index on a miss.)

    // Cursor x within the line: pango_layout_index_to_pos at the
    // relative byte offset.
    int rel_byte = (int)(m_byte_index - target->byte_start);
    if (rel_byte < 0) rel_byte = 0;
    int max_byte = (int)(target->byte_end - target->byte_start);
    if (rel_byte > max_byte) rel_byte = max_byte;

    PangoRectangle pos;
    pango_layout_index_to_pos(target->pango.get(), rel_byte, &pos);
    double caret_x_in_line = pos.x / (double)PANGO_SCALE;

    // ── s347 — path-text v2 m3: pattern caret ───────────────────────────
    // The baseline rides a guide; map the line-local caret x through the
    // arc (arc = arc_start + x, the PatternBlock contract) and stand the
    // caret perpendicular to the tangent. Cap-top is the glyph frame's
    // local (0, -ascent) rotated into doc space; Geometry.angle carries
    // the tangent so render()'s existing rotated branch draws the caret
    // down the glyph's local +y — the field was reserved for exactly
    // this ("line pattern tangent rotation adds to this in Arc F").
    if (target->pattern) {
        const double arc = target->pattern->arc_start + caret_x_in_line;
        SceneNode* gnode =
            m_canvas->find_guide_path(target->pattern->guide_id);
        Vec2 gp; double ga = 0.0;
        if (!gnode ||
            !m_canvas->guide_arc_point(gnode, arc, target->pattern->flip,
                                       gp, ga, target->pattern->offset))
            return g;   // dangling guide: no caret (buffer ops still work)
        // s347 — attachment convention: unflipped caret tops at -ascent
        // (cap height above the path); flipped lines hang from the
        // ascender, so the caret tops ON the path. Local (0, t) maps to
        // gp + (-t*sin, t*cos).
        const double t_top = target->pattern->flip ? 0.0 : -target->ascent;
        g.x = gp.x - t_top * std::sin(ga);
        g.y = gp.y + t_top * std::cos(ga);
        g.height = target->ascent + target->descent * 0.25;
        g.angle = ga;
        g.valid = true;
        return g;
    }

    g.x = target->x_start + caret_x_in_line;
    // s301 m1h — Caret geometry sized to glyph height, not line height.
    //
    // Previously: caret top = baseline - ascent, caret bottom = baseline
    //             - ascent + leading. That spans the line-spacing gap on
    //             both sides of the glyphs, which looks intimidating —
    //             the caret is taller than the visible text.
    //
    // Now: caret top = baseline - ascent (top of capitals), caret
    //      bottom = baseline + 0.25 * descent (just a smidge below the
    //      baseline, hinting at the descender zone without spanning the
    //      whole leading gap). Total height = ascent + 0.25 * descent —
    //      roughly matches how every text editor draws its caret.
    g.y = target->y - target->ascent;
    g.height = target->ascent + target->descent * 0.25;
    // s320 m1 — baselines are laid out in the boundary's upright frame; map
    // the caret origin back into doc space so it sits on the rotated text.
    // angle == 0 leaves it untouched (the common rect case).
    if (tl.frame_angle != 0.0) {
        const double ca = std::cos(tl.frame_angle), sa = std::sin(tl.frame_angle);
        const double rx = g.x - tl.frame_cx, ry = g.y - tl.frame_cy;
        g.x = tl.frame_cx + rx * ca - ry * sa;
        g.y = tl.frame_cy + rx * sa + ry * ca;
    }
    g.angle = tl.frame_angle;
    g.valid = true;
    return g;
}

// ── s305 m1 — Click-to-position: map doc-space (x, y) → byte ────────────────
// The inverse of position_on_canvas. Same TextLayout, same baseline list,
// same per-baseline Pango layouts — but we ask Pango for byte-given-x
// rather than x-given-byte.
//
// Vertical resolution is forgiving: a click above the first baseline
// returns byte 0; a click below the last returns the last baseline's
// byte_end. Inside the band of a baseline (between y - ascent and
// y + descent) we route to that baseline's Pango layout. This is what
// every text editor does — clicking in the gap above a line still
// places the caret on that line, because the visual gap belongs to
// the next line down. We pick the baseline whose [top, bottom] band
// contains doc_y, choosing the band that runs from the previous
// baseline's bottom (or boundary top for the first) down to this
// baseline's bottom.
//
// Horizontal resolution per-baseline:
//   - x_in_line = doc_x - baseline.x_start, clamped to [0, line width]
//   - pango_layout_xy_to_index gives (byte_relative, trailing). Caret
//     affinity rule: trailing == 1 means click landed past the glyph
//     midpoint → caret goes AFTER that glyph. Add the next grapheme's
//     byte advance via g_utf8_next_char.
//   - Add baseline.byte_start to get the absolute buffer byte.
std::optional<size_t> TextCursor::byte_index_at(
    double doc_x, double doc_y) const {

    if (!m_text || !m_canvas) return std::nullopt;

    // Empty buffer: only one sensible caret position.
    if (m_text->text_content.empty()) return size_t{0};

    // Resolve boundary the same way position_on_canvas does (including
    // the s347 guide tier).
    SceneNode* boundary = m_boundary;
    if (!boundary && !m_text->text_guide_id.empty())
        boundary = m_canvas->find_guide_path(m_text->text_guide_id);
    if (!boundary) {
        if (m_text->text_boundary_ids.empty()) return std::nullopt;
        boundary = m_canvas->find_text_boundary(
            m_text->text_boundary_ids.front());
        if (!boundary) return std::nullopt;
    }

    TextLayout tl = layout_for(boundary);  // s345 — styled layout, see layout_for
    if (tl.baselines.empty()) return std::nullopt;

    // ── s347 — path-text v2 m3: pattern click-to-byte ───────────────────
    // Project the click onto the guide, un-map the arc to line-local x
    // (the inverse of the PatternBlock contract), and ask the line's
    // pango layout for the byte — the same xy_to_index + trailing logic
    // as the box path below, sharing its tail. One baseline exists until
    // m4 stacks lines; the band test below has no meaning on a curve, so
    // this branch fully replaces target selection for pattern layouts.
    // Distance is NOT gated here: callers (ToP entry / caret reposition)
    // have already decided the click belongs to this text, and clamping
    // a far click to the line's nearest end mirrors the box behaviour.
    const BaselineLayout* target = nullptr;
    double x_in_line = 0.0;
    if (!m_text->text_guide_id.empty()) {
        // s347 m4 — several stacked lines, each on its OWN walk path
        // (line 0/1 the guide, 2+ inward offsets). Project the click onto
        // every line's path and keep the nearest; ties resolve to the
        // earlier line. Baselines without a pattern block (offset consumed
        // the shape) can't be clicked — skipped; if NO line has a path the
        // click falls back to the first baseline at x 0.
        double best_d = std::numeric_limits<double>::max();
        const BaselineLayout* best_bl = nullptr;
        double best_x = 0.0;
        for (const auto& bl : tl.baselines) {
            if (!bl.pattern) continue;
            SceneNode* gnode =
                m_canvas->find_guide_path(bl.pattern->guide_id);
            double arc = 0.0, dist = 0.0, total = 0.0;
            if (!gnode ||
                !m_canvas->guide_project_point(gnode, {doc_x, doc_y}, arc,
                                               dist, &total,
                                               bl.pattern->offset))
                continue;
            double x = (bl.pattern->flip ? total - arc : arc)
                       - bl.pattern->arc_start;
            const double w = std::max(bl.x_end - bl.x_start, 0.0);
            auto pen = [&](double c) {
                return c < 0.0 ? -c : (c > w ? c - w : 0.0);
            };
            if (gnode->path && gnode->path->closed && total > 0.001) {
                // The line may straddle the seam (arc_start negative or
                // past total): of x, x±total, keep the candidate least
                // outside [0, w].
                for (double c : {x - total, x + total})
                    if (pen(c) < pen(x)) x = c;
            }
            // Rank by perpendicular distance to the path PLUS how far the
            // click overshoots the line's arc extent, so a click past a
            // short top line but right on a long bottom line picks the
            // bottom line, not the nearer-path top.
            const double score = dist + pen(x);
            if (score < best_d) {
                best_d = score;
                best_bl = &bl;
                best_x = std::min(std::max(x, 0.0), w);
            }
        }
        if (!best_bl) {
            target = &tl.baselines.front();
            x_in_line = 0.0;
        } else {
            target = best_bl;
            x_in_line = best_x;
        }
    } else {

    // s327 m3 — The baselines in `tl` live in the UPRIGHT frame: the layout
    // rotated the boundary by -frame_angle about the centroid before laying
    // text out. The incoming click is in doc space. Map it INTO the upright
    // frame with the SAME rotation the layout used, so the band/x tests below
    // (all written for upright coords) compare like with like. Caret OUTPUT is
    // rotated back to doc space in position_on_canvas; this is the missing
    // input side. frame_angle 0 -> ux/uy == doc_x/doc_y exactly (identity).
    double ux = doc_x, uy = doc_y;
    if (tl.frame_angle != 0.0) {
        const double ca = std::cos(-tl.frame_angle), sa = std::sin(-tl.frame_angle);
        const double rx = doc_x - tl.frame_cx, ry = doc_y - tl.frame_cy;
        ux = tl.frame_cx + rx * ca - ry * sa;
        uy = tl.frame_cy + rx * sa + ry * ca;
    }

    // Choose baseline by vertical band. A baseline's visual band runs
    // from "halfway up to the previous baseline" down to "halfway
    // down to the next baseline" — same convention as how clicking in
    // line spacing routes to the closer line. We approximate by using
    // baseline.y - baseline.ascent as the top edge and
    // baseline.y + baseline.descent as the bottom edge; a click in
    // the gap snaps to the nearest band.
    for (size_t i = 0; i < tl.baselines.size(); ++i) {
        const BaselineLayout& bl = tl.baselines[i];
        double top    = bl.y - bl.ascent;
        double bottom = bl.y + bl.descent;
        if (uy >= top && uy <= bottom) {
            target = &bl;
            break;
        }
        // Click above this band — if it's the first baseline, snap
        // to it; otherwise let the previous baseline have already
        // claimed it via its bottom-edge band.
        if (uy < top) {
            if (i == 0) {
                target = &bl;
                break;
            }
            // Gap between previous and current — snap to whichever
            // band edge is closer.
            const BaselineLayout& prev = tl.baselines[i - 1];
            double prev_bottom = prev.y + prev.descent;
            double mid = 0.5 * (prev_bottom + top);
            target = (uy < mid) ? &prev : &bl;
            break;
        }
    }
    if (!target) {
        // Click below the last baseline's band — snap to the last.
        target = &tl.baselines.back();
    }

    // Local x within the baseline's segment. Clamp to [0, width] so
    // a click far left of the line lands at byte_start and far right
    // at byte_end.
    double line_width = target->x_end - target->x_start;
    if (line_width < 0.0) line_width = 0.0;
    x_in_line = ux - target->x_start;
    if (x_in_line < 0.0) x_in_line = 0.0;
    if (x_in_line > line_width) x_in_line = line_width;

    }  // s347 — end of the box-frame else (pattern branch above shares the
       // xy_to_index + trailing tail below)

    // Pango wants pixel coords scaled by PANGO_SCALE. The layouts were
    // built with the same unit convention compute_text_layout uses
    // throughout (doc units == pixels for the single-line Pango layout).
    int px_x = (int)std::round(x_in_line * PANGO_SCALE);
    int rel_byte = 0;
    int trailing = 0;
    gboolean inside = pango_layout_xy_to_index(
        target->pango.get(), px_x, 0, &rel_byte, &trailing);
    (void)inside;  // false at extremes is fine — clamp already handled

    if (rel_byte < 0) rel_byte = 0;
    int max_rel = (int)(target->byte_end - target->byte_start);
    if (rel_byte > max_rel) rel_byte = max_rel;

    // Trailing flag: caret-affinity. Pango reports the byte at the
    // start of the glyph the click landed in, plus trailing=1 if
    // past the midpoint. Advance by trailing graphemes to land the
    // caret AFTER the clicked character when appropriate.
    size_t abs_byte = target->byte_start + (size_t)rel_byte;
    if (trailing > 0) {
        const std::string& s = m_text->text_content;
        for (int t = 0; t < trailing && abs_byte < s.size(); ++t) {
            const char* p = s.c_str() + abs_byte;
            const char* n = g_utf8_next_char(p);
            if (!n || n == p) break;
            abs_byte += (size_t)(n - p);
            if (abs_byte > s.size()) {
                abs_byte = s.size();
                break;
            }
        }
    }

    // Final clamp: never past the buffer.
    if (abs_byte > m_text->text_content.size())
        abs_byte = m_text->text_content.size();

    return abs_byte;
}

bool TextCursor::place_caret_at(double doc_x, double doc_y) {
    auto byte = byte_index_at(doc_x, doc_y);
    if (!byte) return false;
    if (*byte == m_byte_index && m_anchor_byte == m_byte_index) return false;
    m_byte_index = *byte;
    on_horizontal_motion();  // s306 m6 — collapse anchor + drop preferred_x
    return true;
}

// ── s305 m2 — Selection range model ─────────────────────────────────────────
// Helpers that operate on the (anchor, caret) pair. The model itself is
// the two size_t members; these methods are just convenience and sanity-
// clamping so callers don't have to reach into the internals.

std::pair<size_t, size_t> TextCursor::selection_range() const {
    if (m_anchor_byte <= m_byte_index)
        return {m_anchor_byte, m_byte_index};
    return {m_byte_index, m_anchor_byte};
}

std::string TextCursor::selection_text() const {
    if (!m_text || !has_selection()) return {};
    auto [s, e] = selection_range();
    // Clamp defensively — m_text content could have shrunk via an
    // external mutation between the anchor being set and this call.
    if (s > m_text->text_content.size()) s = m_text->text_content.size();
    if (e > m_text->text_content.size()) e = m_text->text_content.size();
    if (e <= s) return {};
    return m_text->text_content.substr(s, e - s);
}

void TextCursor::set_byte_index(size_t b) {
    if (!m_text) return;
    if (b > m_text->text_content.size()) b = m_text->text_content.size();
    m_byte_index = b;
    // Note: deliberately does NOT touch m_anchor_byte. Drag-update in
    // m3 calls this between press (which collapsed anchor=caret) and
    // release; the moving caret end grows the selection from the
    // press point.
    //
    // s306 m6 — Drag IS horizontal motion (the caret end moves), so
    //   drop preferred_x. on_horizontal_motion() can't be used here
    //   because it also collapses the anchor, which is exactly what
    //   set_byte_index must NOT do.
    m_preferred_caret_x = -1.0;
}

void TextCursor::set_anchor_byte(size_t b) {
    if (!m_text) return;
    if (b > m_text->text_content.size()) b = m_text->text_content.size();
    m_anchor_byte = b;
}

void TextCursor::collapse_selection() {
    m_anchor_byte = m_byte_index;
}

void TextCursor::select_all() {
    if (!m_text) return;
    m_anchor_byte = 0;
    m_byte_index = m_text->text_content.size();
    // s306 m6 — Caret jumped to buffer end; drop preferred_x.
    m_preferred_caret_x = -1.0;
}

// ── s326 m2c — Multi-click granularity selection ────────────────────────────
// Double / triple / quadruple click in edit mode select word / visual line /
// paragraph around a byte offset (resolved by byte_index_at at the click).
// Each sets anchor + caret to the span bounds; no buffer mutation. byte is
// absolute into text_content (the cursor's own coordinate system).

void TextCursor::select_word_at(size_t byte) {
    if (!m_text) return;
    const std::string& s = m_text->text_content;
    const size_t n = s.size();
    if (n == 0) { m_anchor_byte = m_byte_index = 0; return; }
    if (byte > n) byte = n;

    // Character class for word runs: 0 = word (alnum + '_'), 1 = whitespace,
    // 2 = other (punctuation/symbol). Double-click selects the maximal run of
    // the class under the click — word on a word, the space run on a space.
    auto cls = [](gunichar c) -> int {
        if (g_unichar_isspace(c)) return 1;
        if (g_unichar_isalnum(c) || c == (gunichar)'_') return 0;
        return 2;
    };

    const char* base = s.c_str();
    // Reference char: the one starting at `byte`; at end-of-buffer step back
    // so a caret past the last glyph still selects the final word.
    size_t ref = byte;
    if (ref >= n) ref = (size_t)(g_utf8_prev_char(base + n) - base);
    gunichar rc = g_utf8_get_char(base + ref);
    const int target = cls(rc);

    // Extend left over same-class chars.
    size_t start = ref;
    while (start > 0) {
        const char* prev = g_utf8_prev_char(base + start);
        if (cls(g_utf8_get_char(prev)) != target) break;
        start = (size_t)(prev - base);
    }
    // Extend right over same-class chars.
    size_t end = (size_t)(g_utf8_next_char(base + ref) - base);
    while (end < n) {
        if (cls(g_utf8_get_char(base + end)) != target) break;
        end = (size_t)(g_utf8_next_char(base + end) - base);
    }
    m_anchor_byte = start;
    m_byte_index  = end;
    m_preferred_caret_x = -1.0;
}

void TextCursor::select_line_at(size_t byte) {
    if (!m_text || !m_canvas) return;
    SceneNode* boundary = m_boundary;
    if (!boundary) {
        if (m_text->text_boundary_ids.empty()) return;
        boundary = m_canvas->find_text_boundary(
            m_text->text_boundary_ids.front());
        if (!boundary) return;
    }
    TextLayout tl = layout_for(boundary);  // s345 — styled layout, see layout_for
    if (tl.baselines.empty()) return;
    // s345 — resolve the owning line through THE resolver (this reader's
    // strict-`<` scan also missed hard_end: triple-click at a paragraph
    // break selected the wrong line). Selection range stays the line's
    // CONTENT bytes [byte_start, byte_end) — ownership answers "which
    // line", content answers "which bytes".
    size_t idx = baseline_index_for(tl, byte);
    if (idx == size_t(-1)) return;
    const BaselineLayout& bl = tl.baselines[idx];
    m_anchor_byte = bl.byte_start;
    m_byte_index  = bl.byte_end;
    m_preferred_caret_x = -1.0;
}

void TextCursor::select_paragraph_at(size_t byte) {
    if (!m_text) return;
    const std::string& s = m_text->text_content;
    const size_t n = s.size();
    if (n == 0) { m_anchor_byte = m_byte_index = 0; return; }
    if (byte > n) byte = n;
    // Paragraph = the run between hard '\n' breaks. '\n' is a single byte and
    // cannot occur inside a UTF-8 multibyte sequence, so byte scanning is safe.
    size_t start = byte;
    while (start > 0 && s[start - 1] != '\n') --start;
    size_t end = byte;
    while (end < n && s[end] != '\n') ++end;   // up to, not including, the '\n'
    m_anchor_byte = start;
    m_byte_index  = end;
    m_preferred_caret_x = -1.0;
}

bool TextCursor::delete_selection() {
    if (!m_text || !has_selection()) return false;
    auto [s, e] = selection_range();
    // Defensive clamp — buffer may have shrunk underneath us.
    if (s > m_text->text_content.size()) s = m_text->text_content.size();
    if (e > m_text->text_content.size()) e = m_text->text_content.size();
    if (e <= s) {
        // Anchor and caret straddle a zero-width range (e.g. stale
        // anchor past end after content shrink). Collapse and signal
        // "no change."
        m_anchor_byte = m_byte_index = s;
        return false;
    }
    m_text->text_content.erase(s, e - s);
    curvz::utils::shift_spans_on_delete(
        m_text->text_attr_spans, (unsigned)s, (unsigned)(e - s));
    m_byte_index = s;
    m_anchor_byte = s;
    // s306 m6 — Buffer mutated, caret moved; drop preferred_x so the
    //   next Up/Down rediscovers it at the collapsed caret's new x.
    m_preferred_caret_x = -1.0;
    return true;
}


// ── Render — vertical hairline at the cursor position ───────────────────────
// Expects the doc-space transform to be applied (translate(ox,oy) +
// scale(zoom,zoom)). Line width in doc units.
//
// s301 m1g — Color is CurrentColor-style contrast against the canvas
// background, NOT the text fill. The caret marks where the next typed
// character will go; making it match the text fill seems intuitive but
// fails when the fill matches the background (white text on white
// background → invisible caret). The convention every text editor
// converges on: black on light, white on dark. We sample the artboard
// background via Canvas::caret_contrast_color and use whichever
// produces visible contrast.
void TextCursor::render(const Cairo::RefPtr<Cairo::Context>& cr) const {
    if (!m_visible) return;
    Geometry g = position_on_canvas();
    if (!g.valid) return;

    double cr_r = 1.0, cr_g = 1.0, cr_b = 1.0;
    if (m_canvas) m_canvas->caret_contrast_color(cr_r, cr_g, cr_b);

    cr->save();
    cr->set_source_rgba(cr_r, cr_g, cr_b, 1.0);

    double zoom = m_canvas ? m_canvas->zoom() : 1.0;
    if (zoom <= 0.0) zoom = 1.0;
    cr->set_line_width(1.5 / zoom);

    if (g.angle == 0.0) {
        cr->move_to(g.x, g.y);
        cr->line_to(g.x, g.y + g.height);
    } else {
        // s320 m1 — caret runs down the frame's local +y (down) axis,
        // which for a frame rotated by `angle` is (-sin, cos) in doc space.
        double dx = -std::sin(g.angle) * g.height;
        double dy =  std::cos(g.angle) * g.height;
        cr->move_to(g.x, g.y);
        cr->line_to(g.x + dx, g.y + dy);
    }
    cr->stroke();
    cr->restore();
}

} // namespace Curvz
