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

#include <pango/pangocairo.h>
#include <glib.h>
#include <algorithm>
#include <cmath>

namespace Curvz {

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
                       SceneNode* boundary)
    : m_canvas(canvas), m_text(text_node), m_boundary(boundary) {
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
        TextLayout tl = compute_text_layout(boundary, m_text);
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
        TextLayout tl = compute_text_layout(boundary, m_text);
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

    TextLayout tl = compute_text_layout(boundary, m_text);
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

    TextLayout tl = compute_text_layout(boundary, m_text);
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
static PangoLayout* make_single_line_layout(const SceneNode* text,
                                             const char* chunk,
                                             int chunk_len) {
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

// ── The universal layout function ───────────────────────────────────────────
TextLayout compute_text_layout(const SceneNode* boundary,
                               const SceneNode* text) {
    TextLayout out;
    if (!boundary || !boundary->path || !text) return out;
    const PathData& bp = *boundary->path;
    if (bp.nodes.size() < 3) return out;

    // Boundary bbox in doc space (1c: rectangle case).
    double bx0 = bp.nodes[0].x, by0 = bp.nodes[0].y;
    double bx1 = bx0, by1 = by0;
    for (const auto& n : bp.nodes) {
        if (n.x < bx0) bx0 = n.x; if (n.x > bx1) bx1 = n.x;
        if (n.y < by0) by0 = n.y; if (n.y > by1) by1 = n.y;
    }
    // Margins live on the boundary for TextBox-owned text; fall back
    // to the text node for legacy paired-sibling files. See
    // effective_text_margins doc in SceneNode.hpp.
    auto m = effective_text_margins(text, boundary);
    double ix0 = bx0 + m.left;
    double iy0 = by0 + m.top;
    double ix1 = bx1 - m.right;
    double iy1 = by1 - m.bottom;
    if (ix1 <= ix0 || iy1 <= iy0) return out;

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
    // to match the prior look. The user can override later via a
    // text_line_height field if needed.
    double leading = (ascent + descent) * 1.2;
    if (leading <= 0.0) leading = font_size * 1.2;  // defensive fallback
    pango_font_metrics_unref(fmetrics);
    pango_font_description_free(metrics_desc);
    g_object_unref(metrics_layout);
    cairo_destroy(metrics_cr);
    cairo_surface_destroy(metrics_surf);

    // First baseline y. If even the first baseline doesn't fit (interior
    // too short), no baselines are emitted and bytes_consumed stays 0.
    double baseline_y = iy0 + ascent;

    size_t cursor = 0;
    const std::string& buf = text->text_content;
    int safety = 0;
    while (baseline_y <= iy1 && cursor <= buf.size() && safety++ < 10000) {
        // Rectangle case: every baseline has the full interior width.
        // Arc E replaces this with outline-intersection scanline.
        double x_start = ix0;
        double x_end   = ix1;
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
            baseline_y += leading;
            continue;
        }

        PangoLayout* layout = make_single_line_layout(text,
                                                       remaining,
                                                       (int)remaining_n);
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
            baseline_y += leading;
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
                                                            (int)consumed_on_line);
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
        out.baselines.push_back(std::move(bl));

        // If we broke at a '\n', consume that newline byte too (it
        // shouldn't appear on the next line).
        if (cursor < buf.size() && buf[cursor] == '\n') {
            cursor += 1;
        }
        out.bytes_consumed = cursor;

        baseline_y += leading;
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

    TextLayout tl = compute_text_layout(boundary, m_text);
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
    g.angle = target->angle;
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

    TextLayout tl = compute_text_layout(boundary, m_text);
    if (tl.baselines.empty()) return std::nullopt;

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
        if (doc_y >= top && doc_y <= bottom) {
            target = &bl;
            break;
        }
        // Click above this band — if it's the first baseline, snap
        // to it; otherwise let the previous baseline have already
        // claimed it via its bottom-edge band.
        if (doc_y < top) {
            if (i == 0) {
                target = &bl;
                break;
            }
            // Gap between previous and current — snap to whichever
            // band edge is closer.
            const BaselineLayout& prev = tl.baselines[i - 1];
            double prev_bottom = prev.y + prev.descent;
            double mid = 0.5 * (prev_bottom + top);
            target = (doc_y < mid) ? &prev : &bl;
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
    double x_in_line = doc_x - target->x_start;
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
        // Reserved for curved line patterns (Arc F).
        double dx = std::sin(g.angle) * g.height;
        double dy = std::cos(g.angle) * g.height;
        cr->move_to(g.x, g.y);
        cr->line_to(g.x + dx, g.y + dy);
    }
    cr->stroke();
    cr->restore();
}

} // namespace Curvz
