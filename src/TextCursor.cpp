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
#include "math/TextFlowGeometry.hpp"   // s323 — form-fit reflow geometry pumps

#include <pango/pangocairo.h>
#include <glib.h>
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
//   the user the textbox's full line capacity. End-of-buffer caret /
//   line nav fallbacks should target the LAST CONTENT baseline, not
//   the bottom empty one. Returns nullptr only when the baseline list
//   is empty (caller should have early-returned in that case).
static const BaselineLayout* last_content_baseline(const TextLayout& tl) {
    for (auto it = tl.baselines.rbegin(); it != tl.baselines.rend(); ++it) {
        if (it->byte_start < it->byte_end) return &(*it);
    }
    // All empty (no text typed) — return the first one. The caret will
    // render at the very top of the textbox, where the first character
    // would go.
    return tl.baselines.empty() ? nullptr : &tl.baselines.front();
}

// s306 m6 — Index variant of last_content_baseline, for Up/Down's
//   bounds check ("Down at last-content-baseline does nothing").
//   Returns the index into tl.baselines of the last baseline with
//   content, or 0 when there is no content baseline at all (the
//   "empty buffer / all-empty layout" case — Up/Down both no-op
//   because there is exactly one navigable position).
static size_t last_content_index(const TextLayout& tl) {
    if (tl.baselines.empty()) return 0;
    for (size_t i = tl.baselines.size(); i-- > 0; ) {
        if (tl.baselines[i].byte_start < tl.baselines[i].byte_end)
            return i;
    }
    return 0;
}

// s306 m6 — Find the baseline index currently holding the caret.
//   Same lookup convention as position_on_canvas / move_line_*:
//   strict `<` on the upper bound (m4d's "later-baseline-wins"
//   rule), with last_content_index as the end-of-buffer fallback
//   (m4f). Returns size_t(-1) only when the baseline list is
//   empty; callers should have early-returned in that case.
// s307 (newline fix v2) — The caret tracks the insertion point.
//   After typing N chars the caret is at byte N; after typing a
//   newline the caret crosses ONTO the next baseline (the empty
//   one immediately after the consumed \n). The "did the caret
//   cross a newline" discriminator is buf[B-1] == '\n':
//     - "first" + B=5 → buf[4]='t', no \n. Caret on baseline 0
//       (end of "first" content), not on the empty hairline below.
//     - "first\n" + B=6 → buf[5]='\n'. Caret on baseline 1 (the
//       empty baseline that owns byte 6, post-newline).
//     - "first\nsecond" + B=12 → buf[11]='d'. Caret on baseline 1
//       at end of "second" content.
//   Empty-baseline ownership applies ONLY when the byte was
//   reached by crossing a \n; otherwise the caret stays on the
//   content baseline that ends at B (resolved via the m4f
//   last_content_baseline fallback).
static size_t baseline_index_for(const TextLayout& tl, size_t byte_index,
                                  const std::string& buf) {
    bool crossed_newline = (byte_index > 0 && byte_index <= buf.size() &&
                            buf[byte_index - 1] == '\n');
    bool buffer_empty = buf.empty();
    for (size_t i = 0; i < tl.baselines.size(); ++i) {
        const BaselineLayout& bl = tl.baselines[i];
        bool in_range = (byte_index >= bl.byte_start && byte_index < bl.byte_end);
        bool empty_owns = (bl.byte_start == bl.byte_end &&
                           byte_index == bl.byte_start &&
                           (crossed_newline || buffer_empty));
        if (in_range || empty_owns) {
            return i;
        }
    }
    // End-of-buffer / unmatched: fall back to the last content
    // baseline (caret renders at end of content, not on a bottom
    // capacity hairline).
    return tl.baselines.empty() ? size_t(-1) : last_content_index(tl);
}

// ── Construction ────────────────────────────────────────────────────────────
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
        TextLayout tl = compute_text_layout(boundary, m_text, m_byte_start);
        if (!tl.baselines.empty()) {
            // s305 m4d — Later-baseline-wins at wrap boundary. See
            //   position_on_canvas for the rationale. Strict `<` on
            //   upper bound; fallback to last baseline catches the
            //   end-of-buffer case.
            // s307 (newline fix v2) — Empty-baseline ownership gated
            //   on crossed_newline. See baseline_index_for.
            const std::string& buf = m_text->text_content;
            bool crossed_newline = (m_byte_index > 0 &&
                                    m_byte_index <= buf.size() &&
                                    buf[m_byte_index - 1] == '\n');
            bool buffer_empty = buf.empty();
            for (const auto& bl : tl.baselines) {
                bool in_range = (m_byte_index >= bl.byte_start &&
                                 m_byte_index < bl.byte_end);
                bool empty_owns = (bl.byte_start == bl.byte_end &&
                                   m_byte_index == bl.byte_start &&
                                   (crossed_newline || buffer_empty));
                if (in_range || empty_owns) {
                    m_byte_index = bl.byte_start;
                    on_horizontal_motion();  // s306 m6
                    return;
                }
            }
            // Caret at end-of-buffer or past the last baseline → start
            // of the last content baseline.
            const BaselineLayout* last = last_content_baseline(tl);
            if (last) {
                m_byte_index = last->byte_start;
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
        TextLayout tl = compute_text_layout(boundary, m_text, m_byte_start);
        if (!tl.baselines.empty()) {
            // s305 m4d — Later-baseline-wins (see move_line_start).
            // s307 (newline fix v2) — Empty-baseline ownership gated.
            const std::string& buf = m_text->text_content;
            bool crossed_newline = (m_byte_index > 0 &&
                                    m_byte_index <= buf.size() &&
                                    buf[m_byte_index - 1] == '\n');
            bool buffer_empty = buf.empty();
            const BaselineLayout* target = nullptr;
            for (const auto& bl : tl.baselines) {
                bool in_range = (m_byte_index >= bl.byte_start &&
                                 m_byte_index < bl.byte_end);
                bool empty_owns = (bl.byte_start == bl.byte_end &&
                                   m_byte_index == bl.byte_start &&
                                   (crossed_newline || buffer_empty));
                if (in_range || empty_owns) {
                    target = &bl;
                    break;
                }
            }
            if (!target) target = last_content_baseline(tl);
            if (!target) return;

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

    TextLayout tl = compute_text_layout(boundary, m_text, m_byte_start);
    if (tl.baselines.empty()) return false;

    size_t cur_idx = baseline_index_for(tl, m_byte_index, m_text->text_content);
    if (cur_idx == size_t(-1)) return false;
    if (cur_idx == 0) return false;  // top of list, nothing above

    const BaselineLayout* current = &tl.baselines[cur_idx];
    const BaselineLayout* target  = &tl.baselines[cur_idx - 1];

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

    TextLayout tl = compute_text_layout(boundary, m_text, m_byte_start);
    if (tl.baselines.empty()) return false;

    size_t cur_idx = baseline_index_for(tl, m_byte_index, m_text->text_content);
    if (cur_idx == size_t(-1)) return false;

    size_t last_idx = last_content_index(tl);
    if (cur_idx >= last_idx) return false;  // already on last content row

    const BaselineLayout* current = &tl.baselines[cur_idx];
    const BaselineLayout* target  = &tl.baselines[cur_idx + 1];

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
                a = pango_attr_size_new((int)s.ivalue); break;
            case PANGO_ATTR_ABSOLUTE_SIZE:
                a = pango_attr_size_new_absolute((int)s.ivalue); break;
            case PANGO_ATTR_LETTER_SPACING:
                a = pango_attr_letter_spacing_new((int)s.ivalue); break;
            case PANGO_ATTR_RISE:
                a = pango_attr_rise_new((int)s.ivalue); break;
            case PANGO_ATTR_FONT_SCALE:
                a = pango_attr_font_scale_new((PangoFontScale)s.ivalue); break;
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

static PangoLayout* make_single_line_layout(const SceneNode* text,
                                             const char* chunk,
                                             int chunk_len,
                                             size_t chunk_byte_start = 0) {
    // Tiny temp cairo context so PangoCairo can build a layout.
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t* tmp = cairo_create(surf);
    PangoLayout* layout = pango_cairo_create_layout(tmp);
    cairo_destroy(tmp);
    cairo_surface_destroy(surf);

    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, text->text_font_family.c_str());
    pango_font_description_set_absolute_size(desc,
        text->text_font_size * PANGO_SCALE);
    if (text->text_bold)
        pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    if (text->text_italic)
        pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_width(layout, -1);  // no Pango wrap
    pango_layout_set_text(layout, chunk, chunk_len);
    // s325 — per-run formatting for this line. The scalar font_description
    // above is the baseline; these spans override it per byte-range.
    if (PangoAttrList* la = build_line_attrs(text, chunk_byte_start, chunk_len)) {
        pango_layout_set_attributes(layout, la);
        pango_attr_list_unref(la);
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
                                       size_t chunk_byte_start = 0) {
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
                                              chunk_byte_start);
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

// s331 — per-paragraph leading: the stride below a given line. A leading run
// (kCurvzLeadingAttr) covering `byte` (the line's start byte, which sits in
// the owning paragraph) wins; otherwise the buffer default (metric or legacy
// scalar) applies. ivalue is doc-px x PANGO_SCALE.
static double leading_for_byte(const SceneNode* text, double default_leading,
                               size_t byte) {
  if (!text) return default_leading;
  for (const auto& s : text->text_attr_spans)
    if (s.type == curvz::utils::kCurvzLeadingAttr &&
        (size_t)s.start_byte <= byte && (size_t)s.end_byte > byte)
      return (double)s.ivalue / (double)PANGO_SCALE;
  return default_leading;
}

// s334 — per-paragraph indent (doc units). Same shape as leading_for_byte: a
// private indent run covering the line's start byte (which sits in the owning
// paragraph) wins; default 0 = no inset. ivalue is doc-px x PANGO_SCALE.
static double indent_for_byte(const SceneNode* text, int attr, size_t byte) {
  if (!text) return 0.0;
  double v = 0.0;
  for (const auto& s : text->text_attr_spans)
    if (s.type == attr &&
        (size_t)s.start_byte <= byte && (size_t)s.end_byte > byte)
      v = (double)s.ivalue / (double)PANGO_SCALE;
  return v;
}

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

TextLayout compute_text_layout(const SceneNode* boundary,
                               const SceneNode* text,
                               size_t byte_start) {
    TextLayout out;
    if (!boundary || !boundary->path || !text) return out;
    const PathData& bp = *boundary->path;
    if (bp.nodes.size() < 3) return out;

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
    int safety = 0;
    while (baseline_y <= eroded_bottom && cursor <= buf.size() && safety++ < 10000) {
        // s331 — the byte that starts this line; sits in the owning paragraph,
        // so it resolves the per-paragraph leading for the stride below.
        const size_t line_start = cursor;
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
            baseline_y += leading_for_byte(text, leading, line_start);
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
        {
          const bool para_start =
              (line_start == 0) || (buf[line_start - 1] == '\n');
          const double ind_l = indent_for_byte(
              text, curvz::utils::kCurvzIndentLeftAttr,  line_start);
          const double ind_r = indent_for_byte(
              text, curvz::utils::kCurvzIndentRightAttr, line_start);
          const double ind_f = para_start
              ? indent_for_byte(text, curvz::utils::kCurvzIndentFirstAttr,
                                line_start)
              : 0.0;
          x_start += ind_l + ind_f;
          x_end   -= ind_r;
          if (x_end < x_start) x_end = x_start;
        }
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
        bl.height     = leading;
        bl.byte_start = cursor;

        if (remaining_n == 0) {
            // s301 m1e — Buffer exhausted. Emit this baseline empty and
            // continue striding down to fill the remaining interior with
            // empty baselines, so the user sees the bbox's full line
            // capacity (every baseline that fits gets a hairline). Caret
            // navigation past end-of-buffer can land on these too —
            // they're real baselines, just without content.
            PangoLayout* layout = make_single_line_layout(text, "", 0);
            bl.pango.reset(layout);
            bl.byte_end = cursor;
            out.baselines.push_back(std::move(bl));
            out.bytes_consumed = cursor;
            baseline_y += leading_for_byte(text, leading, line_start);
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
            measure_first_word_width(text, remaining, remaining_n, cursor) > avail) {
            PangoLayout* empty = make_single_line_layout(text, "", 0);
            bl.pango.reset(empty);
            bl.byte_end = cursor;          // empty line; owns no bytes
            out.baselines.push_back(std::move(bl));
            baseline_y += leading_for_byte(text, leading, line_start);  // cursor NOT advanced
            continue;
        }

        PangoLayout* layout = make_single_line_layout(text,
                                                       remaining,
                                                       (int)remaining_n,
                                                       cursor);
        size_t consumed_on_line = fit_chunk_to_width(remaining, remaining_n,
                                                      layout, avail);
        // Special case: a leading '\n' makes fit_chunk_to_width return 0,
        // meaning "this line is empty; the very first character of the
        // remaining buffer is a hard newline." Emit an empty baseline and
        // consume the newline so the next iteration starts after it.
        if (consumed_on_line == 0 && remaining[0] == '\n') {
            g_object_unref(layout);
            PangoLayout* empty = make_single_line_layout(text, "", 0);
            bl.pango.reset(empty);
            bl.byte_end = cursor;
            out.baselines.push_back(std::move(bl));
            cursor += 1;  // consume the '\n'
            out.bytes_consumed = cursor;
            baseline_y += leading_for_byte(text, leading, line_start);
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
                                                            cursor);
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
                                                       cursor);
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
        out.baselines.push_back(std::move(bl));

        // If we broke at a '\n', consume that newline byte too (it
        // shouldn't appear on the next line).
        if (cursor < buf.size() && buf[cursor] == '\n') {
            cursor += 1;
        }
        out.bytes_consumed = cursor;

        baseline_y += leading_for_byte(text, leading, line_start);
    }

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
            int align = 0;
            for (const auto& s : text->text_attr_spans) {
                if (s.type == curvz::utils::kCurvzAlignAttr &&
                    (unsigned)s.start_byte <= bl.byte_start &&
                    (unsigned)s.end_byte > bl.byte_start) {
                    align = (int)s.ivalue;
                    break;
                }
            }
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

    // Boundary resolution — two paths in priority order:
    //   1. m_boundary set: TextBox-owned text passes its sibling
    //      boundary into the ctor. Direct pointer, no lookup.
    //   2. Legacy paired-sibling: read text_boundary_ids.front() and
    //      look the boundary up by iid through Canvas. Works for
    //      files loaded from before the TextBox migration.
    SceneNode* boundary = m_boundary;
    if (!boundary) {
        if (m_text->text_boundary_ids.empty()) return g;
        boundary = m_canvas->find_text_boundary(
            m_text->text_boundary_ids.front());
        if (!boundary) return g;
    }

    TextLayout tl = compute_text_layout(boundary, m_text, m_byte_start);
    if (tl.baselines.empty()) return g;

    // s305 m4d — At a wrap byte boundary, the byte belongs to the
    //   LATER baseline. Two baselines share their boundary byte
    //   (line A's byte_end == line B's byte_start), and the caret
    //   sitting on that byte visually marks the start of line B,
    //   not the end of line A. Strict `<` on the upper bound picks
    //   the later baseline; fallback to last baseline catches
    //   m_byte_index == buffer_size (where the last baseline's
    //   byte_end equals the buffer size and the strict `<` would
    //   miss it). Same convention as move_line_start/end and the
    //   click handler's byte_index_at.
    // s307 (newline fix v2) — Empty-baseline ownership ONLY when
    //   the caret crossed a \n to get here (buf[B-1] == '\n') or
    //   the buffer is completely empty. See baseline_index_for
    //   for the full rationale. Without the gate, end-of-content
    //   carets (e.g. after typing "first") would jump onto the
    //   empty hairline below.
    const std::string& buf = m_text->text_content;
    bool crossed_newline = (m_byte_index > 0 &&
                            m_byte_index <= buf.size() &&
                            buf[m_byte_index - 1] == '\n');
    bool buffer_empty = buf.empty();
    const BaselineLayout* target = nullptr;
    for (const auto& bl : tl.baselines) {
        bool in_range = (m_byte_index >= bl.byte_start &&
                         m_byte_index < bl.byte_end);
        bool empty_owns = (bl.byte_start == bl.byte_end &&
                           m_byte_index == bl.byte_start &&
                           (crossed_newline || buffer_empty));
        if (in_range || empty_owns) {
            target = &bl;
            break;
        }
    }
    if (!target) {
        // s305 m4f — End-of-buffer fallback. compute_text_layout emits
        //   empty baselines (byte_start == byte_end == buf.size())
        //   below the content so the user sees the textbox's full
        //   line capacity. With strict-`<` lookup above, m_byte_index
        //   == buf.size() matches no baseline; picking baselines.back()
        //   would pick the BOTTOM empty baseline — caret would render
        //   at bottom of the textbox even though text ends mid-textbox.
        //   Right answer: last baseline with content (see helper).
        target = last_content_baseline(tl);
        if (!target) return g;
    }

    // Cursor x within the line: pango_layout_index_to_pos at the
    // relative byte offset.
    int rel_byte = (int)(m_byte_index - target->byte_start);
    if (rel_byte < 0) rel_byte = 0;
    int max_byte = (int)(target->byte_end - target->byte_start);
    if (rel_byte > max_byte) rel_byte = max_byte;

    PangoRectangle pos;
    pango_layout_index_to_pos(target->pango.get(), rel_byte, &pos);
    double caret_x_in_line = pos.x / (double)PANGO_SCALE;

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

    // Resolve boundary the same way position_on_canvas does.
    SceneNode* boundary = m_boundary;
    if (!boundary) {
        if (m_text->text_boundary_ids.empty()) return std::nullopt;
        boundary = m_canvas->find_text_boundary(
            m_text->text_boundary_ids.front());
        if (!boundary) return std::nullopt;
    }

    TextLayout tl = compute_text_layout(boundary, m_text, m_byte_start);
    if (tl.baselines.empty()) return std::nullopt;

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
    const BaselineLayout* target = nullptr;
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
    double x_in_line = ux - target->x_start;
    if (x_in_line < 0.0) x_in_line = 0.0;
    if (x_in_line > line_width) x_in_line = line_width;

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
    TextLayout tl = compute_text_layout(boundary, m_text, m_byte_start);
    if (tl.baselines.empty()) return;
    // The visual line is the baseline whose byte window contains `byte`.
    for (const auto& bl : tl.baselines) {
        if (byte >= bl.byte_start && byte < bl.byte_end) {
            m_anchor_byte = bl.byte_start;
            m_byte_index  = bl.byte_end;
            m_preferred_caret_x = -1.0;
            return;
        }
    }
    // Past the last placed byte -> the last baseline.
    const BaselineLayout& last = tl.baselines.back();
    m_anchor_byte = last.byte_start;
    m_byte_index  = last.byte_end;
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
