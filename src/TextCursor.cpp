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

#include <pango/pangocairo.h>
#include <glib.h>
#include <algorithm>
#include <cmath>

namespace Curvz {

// ── Construction ────────────────────────────────────────────────────────────
TextCursor::TextCursor(Canvas* canvas, SceneNode* text_node)
    : m_canvas(canvas), m_text(text_node) {
    if (m_text) m_byte_index = m_text->text_content.size();
}

// ── Buffer mutation ─────────────────────────────────────────────────────────
bool TextCursor::insert_char(gunichar codepoint) {
    if (!m_text) return false;
    char buf[8];
    int n = g_unichar_to_utf8(codepoint, buf);
    if (n <= 0) return false;
    m_text->text_content.insert(m_byte_index, buf, (size_t)n);
    m_byte_index += (size_t)n;
    return true;
}

bool TextCursor::insert_string(const std::string& utf8) {
    if (!m_text || utf8.empty()) return false;
    if (!g_utf8_validate(utf8.c_str(), (gssize)utf8.size(), nullptr))
        return false;
    m_text->text_content.insert(m_byte_index, utf8);
    m_byte_index += utf8.size();
    return true;
}

bool TextCursor::insert_newline() {
    if (!m_text) return false;
    // Insert a literal line-feed character (U+000A). Pango treats this
    // as a hard paragraph break on next layout. This is the user's
    // explicit "new paragraph" — distinct from word-wrap line breaks
    // which are visual-only and never enter the buffer.
    m_text->text_content.insert(m_byte_index, "\n");
    m_byte_index += 1;
    return true;
}

bool TextCursor::backspace() {
    if (!m_text || m_byte_index == 0) return false;
    const char* base = m_text->text_content.c_str();
    const char* here = base + m_byte_index;
    const char* prev = g_utf8_prev_char(here);
    size_t prev_idx = (size_t)(prev - base);
    m_text->text_content.erase(prev_idx, m_byte_index - prev_idx);
    m_byte_index = prev_idx;
    return true;
}

bool TextCursor::delete_forward() {
    if (!m_text || m_byte_index >= m_text->text_content.size()) return false;
    const char* base = m_text->text_content.c_str();
    const char* here = base + m_byte_index;
    const char* next = g_utf8_next_char(here);
    size_t next_idx = (size_t)(next - base);
    m_text->text_content.erase(m_byte_index, next_idx - m_byte_index);
    return true;
}

// ── Caret navigation ────────────────────────────────────────────────────────
void TextCursor::move_left() {
    if (!m_text || m_byte_index == 0) return;
    const char* base = m_text->text_content.c_str();
    const char* here = base + m_byte_index;
    const char* prev = g_utf8_prev_char(here);
    m_byte_index = (size_t)(prev - base);
}

void TextCursor::move_right() {
    if (!m_text || m_byte_index >= m_text->text_content.size()) return;
    const char* base = m_text->text_content.c_str();
    const char* here = base + m_byte_index;
    const char* next = g_utf8_next_char(here);
    m_byte_index = (size_t)(next - base);
}

void TextCursor::move_line_start() {
    if (!m_text || m_byte_index == 0) return;
    const std::string& s = m_text->text_content;
    size_t i = m_byte_index;
    while (i > 0 && s[i - 1] != '\n') --i;
    m_byte_index = i;
}

void TextCursor::move_line_end() {
    if (!m_text) return;
    const std::string& s = m_text->text_content;
    size_t i = m_byte_index;
    while (i < s.size() && s[i] != '\n') ++i;
    m_byte_index = i;
}

void TextCursor::move_buffer_start() { m_byte_index = 0; }

void TextCursor::move_buffer_end() {
    if (m_text) m_byte_index = m_text->text_content.size();
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
    double ix0 = bx0 + text->text_margin_left;
    double iy0 = by0 + text->text_margin_top;
    double ix1 = bx1 - text->text_margin_right;
    double iy1 = by1 - text->text_margin_bottom;
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
        bl.byte_end = cursor + consumed_on_line;

        out.baselines.push_back(std::move(bl));
        cursor += consumed_on_line;

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
    if (m_text->text_boundary_ids.empty()) return g;

    SceneNode* boundary = m_canvas->find_text_boundary(
        m_text->text_boundary_ids.front());
    if (!boundary) return g;

    TextLayout tl = compute_text_layout(boundary, m_text);
    if (tl.baselines.empty()) return g;

    // Find which baseline holds m_byte_index. The byte ranges are
    // contiguous (with possible \n bytes between them) and ordered, so
    // a linear walk is fine; binary search is overkill for typical line
    // counts.
    const BaselineLayout* target = nullptr;
    for (const auto& bl : tl.baselines) {
        if (m_byte_index >= bl.byte_start && m_byte_index <= bl.byte_end) {
            target = &bl;
            break;
        }
    }
    if (!target) {
        // m_byte_index beyond the last baseline's byte_end → caret past
        // the last visible line. Place it at end of last baseline.
        target = &tl.baselines.back();
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
