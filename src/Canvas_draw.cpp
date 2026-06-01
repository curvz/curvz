#include "Canvas.hpp"
#include "Application.hpp"  // s268 m0 — g_launch_t0 + LAUNCH_TIMING_ENABLED
#include "CommandHistory.hpp"
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"  // s116 m6 — m_project field reads workspace appearance
#include "CurvzSpinButton.hpp"
#include "MacroSystem.hpp"
#include "SvgParser.hpp"
#include "TextCursor.hpp"   // s301 m1c — compute_text_layout + caret render
#include "math/TextFlowGeometry.hpp"  // s322 — form-fit reflow debug overlay
#include "curvz_utils.hpp"  // S97 m2 — box_blur_argb32 for drop-shadow render
#include "color/SwatchLibrary.hpp"  // set_swatch_library + apply_swatch_to_selection
#include "color/FillStyleInterop.hpp"  // to_fillstyle — live-recolour walk (s70 M3)
#include "style/StyleInterop.hpp"  // mutate_appearance funnel for user-driven fill/stroke writes
#include "style/StyleLibrary.hpp"  // set_style_library + signal_style_changed (S78 m3d)
#include <filesystem>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/clipboard.h>
#include <gdkmm/contentprovider.h>
#include <gdkmm/pixbuf.h>
#include <gdkmm/rectangle.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/grid.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/window.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-font.h>
#include <sstream>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>  // s125 m1c — uint8_t for PNG IHDR parser
#include <cstring>  // s125 m1c — std::memcmp for PNG signature check
#include <ctime>    // s125 m1c — strftime/localtime for mtime in info dialog
#include <fstream>  // s125 m1c — std::ifstream for IHDR parser
#include <functional>
#include <glib.h> // g_uuid_string_random via generate_internal_id()
#include <glibmm/main.h>
#include <gtkmm/gestureclick.h>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Canvas_internal.hpp"

namespace Curvz {


// s183 m5a — Canvas::doc_motif(). See Canvas.hpp for the rationale.
Motif Canvas::doc_motif() const {
  return m_project ? m_project->motif : Motif::Dark;
}


// ── Layer colour helper
// ─────────────────────────────────────────────────────── Parses "#rrggbb" into
// [0,1] doubles. Returns false and leaves r/g/b unchanged if the string is
// empty or malformed.
static bool parse_layer_color(const std::string &hex, double &r, double &g,
                              double &b) {
  if (hex.size() != 7 || hex[0] != '#')
    return false;
  auto h = [&](int i) { return std::stoi(hex.substr(i, 2), nullptr, 16); };
  r = h(1) / 255.0;
  g = h(3) / 255.0;
  b = h(5) / 255.0;
  return true;
}

// ── draw_text_on_path
// ───────────────────────────────────────────────────────── Places each glyph
// individually along the guide path using the arc-length table. Each glyph is
// translated to its path position and rotated to match the path tangent at that
// point.
//
// Strategy:
//   1. Build a Pango layout for the full text to extract per-glyph advance
//      widths (using pango_glyph_string_get_logical_widths).
//   2. Walk the path arc-length table, placing each glyph's centre at
//      offset + advance/2 from the previous glyph's end.
//   3. For each glyph: save CTM, translate to path point, rotate by tangent
//      angle (± flip), render via pango_cairo_show_glyph_string, restore.
//
// We're called from inside the doc-space transform (translate+scale applied).
// ─────────────────────────────────────────────────────────────────────────────
void Canvas::draw_text_on_path(const Cairo::RefPtr<Cairo::Context> &cr,
                               const SceneNode &obj, const SceneNode &guide) {
  if (!guide.path)
    return;

  // Build arc-length table in doc units
  BezierPath bp = BezierPath::from_path_data(*guide.path);
  std::vector<double> arc_table;
  double total_len = build_arc_table(bp, arc_table);
  LOG_DEBUG(
      "draw_text_on_path: text='{}' path_id='{}' total_len={:.1f} zoom={:.2f}",
      obj.text_content, obj.text_path_id, total_len, m_zoom);
  if (total_len < 0.001) {
    LOG_DEBUG("draw_text_on_path: ABORT total_len<0.001");
    return;
  }

  // Use only first line of text
  std::string text = obj.text_content;
  auto nl = text.find('\n');
  if (nl != std::string::npos)
    text = text.substr(0, nl);
  if (text.empty()) {
    LOG_DEBUG("draw_text_on_path: ABORT text empty after trim");
    return;
  }

  // Build Pango layout — same as draw_text_node
  PangoLayout *layout = pango_cairo_create_layout(cr->cobj());
  PangoFontDescription *desc = pango_font_description_new();
  pango_font_description_set_family(desc, obj.text_font_family.c_str());
  pango_font_description_set_absolute_size(desc,
                                           obj.text_font_size * PANGO_SCALE);
  if (obj.text_bold)
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  if (obj.text_italic)
    pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
  pango_layout_set_font_description(layout, desc);
  pango_font_description_free(desc);
  pango_layout_set_text(layout, text.c_str(), -1);

  // Baseline: distance from layout top to baseline in pixels
  double baseline_px = pango_layout_get_baseline(layout) / (double)PANGO_SCALE;

  // Total layout width for alignment anchor
  PangoRectangle logical;
  pango_layout_get_pixel_extents(layout, nullptr, &logical);
  double total_text_w = logical.width;

  // Alignment: offset is anchor point on path
  // left=start, center=middle, right=end
  double anchor_arc = obj.text_path_offset;
  if (obj.text_anchor == "middle")
    anchor_arc -= total_text_w * 0.5;
  else if (obj.text_anchor == "end")
    anchor_arc -= total_text_w;

  // Perpendicular offset from path:
  // flip=false → baseline on path, glyphs extend upward (negative Y in rotated
  // frame). flip=true  → reversed traversal + π rotation puts glyphs on the
  // outside of the
  //              bottom arc, readable. Perp is same sign — glyphs still extend
  //              "up" in their own rotated frame, which is outward from the
  //              circle bottom.
  double perp_offset = -obj.text_baseline_shift;

  LOG_DEBUG(
      "draw_text_on_path: text='{}' total_text_w={:.1f} total_path={:.1f} "
      "baseline_px={:.1f} perp={:.1f} anchor_arc={:.1f} flip={}",
      text, total_text_w, total_len, baseline_px, perp_offset, anchor_arc,
      obj.text_path_flip);

  // Apply fill colour once (all glyphs share it)
  apply_fill(cr, obj.fill);

  // ── Iterate runs and place each glyph individually ─────────────────────
  // Use the proven pattern from text_to_paths_op:
  // pango_layout_iter_get_run_extents gives us the run's x position in
  // layout coordinates — critical for correct multi-glyph placement.
  PangoLayoutIter *iter = pango_layout_get_iter(layout);
  do {
    PangoLayoutRun *run = pango_layout_iter_get_run(iter);
    if (!run)
      continue;

    PangoGlyphString *gs = run->glyphs;
    PangoFont *pfont = run->item->analysis.font;

    // Run origin in layout space (pixels)
    PangoRectangle run_ext;
    pango_layout_iter_get_run_extents(iter, nullptr, &run_ext);
    double run_x_px = run_ext.x / (double)PANGO_SCALE;

    // Walk glyphs within this run
    double glyph_x_px = run_x_px; // x position of this glyph within layout

    for (int gi = 0; gi < gs->num_glyphs; ++gi) {
      PangoGlyphInfo &gi_info = gs->glyphs[gi];

      // Skip empty/space glyphs
      if (gi_info.glyph == PANGO_GLYPH_EMPTY ||
          (gi_info.glyph & PANGO_GLYPH_UNKNOWN_FLAG)) {
        glyph_x_px += gi_info.geometry.width / (double)PANGO_SCALE;
        continue;
      }

      double adv_px = gi_info.geometry.width / (double)PANGO_SCALE +
                      obj.text_letter_spacing;

      // Centre of this glyph on the path arc.
      // flip=true: traverse path in reverse so text reads correctly on
      // the bottom of a circle — mirror the arc position to the far end.
      double glyph_centre_arc = anchor_arc + glyph_x_px + adv_px * 0.5;
      double lookup_arc =
          obj.text_path_flip ? total_len - glyph_centre_arc : glyph_centre_arc;

      // Skip glyphs outside path bounds
      if (lookup_arc < 0.0 || lookup_arc > total_len) {
        glyph_x_px += adv_px;
        continue;
      }

      Vec2 pos;
      double angle;
      if (!path_point_at(bp, arc_table, total_len, lookup_arc, pos, angle)) {
        LOG_DEBUG("draw_text_on_path: path_point_at FAILED arc={:.1f}",
                  lookup_arc);
        break;
      }

      // flip=true: add π so glyph faces the opposite tangent direction,
      // making it readable when traversing the path in reverse.
      double effective_angle = obj.text_path_flip ? angle + M_PI : angle;

      if (gi == 0) {
        LOG_DEBUG("draw_text_on_path: glyph 0 adv={:.1f} centre_arc={:.1f} "
                  "lookup={:.1f} pos=({:.1f},{:.1f}) angle={:.3f} perp={:.1f}",
                  adv_px, glyph_centre_arc, lookup_arc, pos.x, pos.y,
                  effective_angle, perp_offset);
      }

      // Place glyph: translate to path point, rotate to (effective) tangent.
      cr->save();
      cr->translate(pos.x, pos.y);
      cr->rotate(effective_angle);

      // Draw single glyph via pango_cairo_show_glyph_string
      PangoGlyphString single;
      int log_cluster = 0;
      single.num_glyphs = 1;
      single.glyphs = &gi_info;
      single.log_clusters = &log_cluster;

      // Horizontal: centre glyph on its advance width.
      // Vertical: baseline_shift pushes away from the path (always negative
      // in the rotated frame — after the π flip, "away" is still -Y).
      double gx = -adv_px * 0.5;
      double gy = perp_offset;

      cr->move_to(gx, gy);
      pango_cairo_show_glyph_string(cr->cobj(), pfont, &single);

      // Optional stroke
      if (obj.stroke.paint.type != FillStyle::Type::None) {
        cr->move_to(gx, gy);
        pango_cairo_glyph_string_path(cr->cobj(), pfont, &single);
        apply_stroke_style(cr, obj.stroke);
        cr->stroke();
        // Restore fill for next glyph
        apply_fill(cr, obj.fill);
      }

      cr->restore();

      glyph_x_px += adv_px;
    }

  } while (pango_layout_iter_next_run(iter));

  pango_layout_iter_free(iter);
  g_object_unref(layout);
}

// ── s301 m1c — Glyph render for bound text ──────────────────────────────────
// Consumes compute_text_layout. For each baseline, builds a single-line
// Pango layout of the baseline's byte range (already done inside the
// layout function, but re-paints from scratch here because the layout's
// PangoLayout* pointers are good for sizing+caret lookup, not necessarily
// for pango_cairo_show_layout on this specific cairo context).
//
// Actually — Pango layouts ARE cairo-context-independent for rendering
// purposes; pango_cairo_show_layout draws them into whatever context is
// active. We re-use the pango layouts from BaselineLayout.
//
// Each baseline draws its chunk starting at (x_start, y - ascent) so the
// Pango layout's top-left aligns with the visual top of the line and the
// text's baseline lands on bl.y as intended.
size_t Canvas::draw_text_in_boundary(const Cairo::RefPtr<Cairo::Context>& cr,
                                    const SceneNode& text_obj,
                                    const SceneNode& boundary,
                                    size_t byte_start,
                                    bool draw_overflow_indicator) {
  TextLayout tl = compute_text_layout(&boundary, &text_obj, byte_start);

  // ── s308 m1 — Overflow indicator (drawn before baselines-empty
  // early-return) ───────────────────────────────────────────────────────
  // When the bbox is too short to hold ANY baseline (interior height
  // < ascent), compute_text_layout produces zero baselines and
  // bytes_consumed stays 0 — i.e. 100% of the buffer overflows. The
  // earlier version put this check after the early-return, so a
  // user who shrank the bbox to nothing saw no glyphs AND no overflow
  // marker, leaving them confused about what happened to their text.
  //
  // Drawing this first (regardless of baselines) is the correct fix:
  // the indicator exists to signal "your content isn't all visible",
  // and the most extreme version of that is "none of it is visible".
  //
  // Geometry: 16 screen px glyph, centered 14 screen px inside the
  // bottom-RIGHT corner of the tail's bbox (s317 — the overflow `!`
  // belongs to the tail member; lower-right keeps it clear of the
  // member's leading text and reads as the spill affordance).
  // Sizer-pattern affordance — Canvas paints it, Canvas hit-tests it
  // (check_overflow_hit), Canvas dispatches the click (GestureClick
  // wired in Canvas.cpp). Constants here MUST match check_overflow_hit.
  if (draw_overflow_indicator &&
      tl.bytes_consumed < text_obj.text_content.size() &&
      boundary.path && boundary.path->nodes.size() >= 3) {
    const auto& bp = boundary.path->nodes;
    double bx0 = bp[0].x, by0 = bp[0].y;
    double bx1 = bx0, by1 = by0;
    for (const auto& n : bp) {
      if (n.x < bx0) bx0 = n.x; if (n.x > bx1) bx1 = n.x;
      if (n.y < by0) by0 = n.y; if (n.y > by1) by1 = n.y;
    }
    constexpr double INDICATOR_SIZE_PX = 16.0;   // matches Canvas_input.cpp hit-test
    constexpr double INDICATOR_INSET_PX = 14.0;  // ditto
    double size_doc = INDICATOR_SIZE_PX / std::max(m_zoom, 0.001);
    double inset_doc = INDICATOR_INSET_PX / std::max(m_zoom, 0.001);
    double cx = bx1 - inset_doc;   // s317 — lower-RIGHT corner (tail's spill)
    double cy = by1 - inset_doc;

    // Lazy-load the pixbuf on first use, recolored from currentColor
    // to --badge-error red at load time (Cairo doesn't read CSS the
    // way symbolic icons do).
    ensure_overflow_glyph_pixbuf();

    if (m_overflow_glyph_pixbuf) {
      cr->save();
      // Center the pixbuf on (cx, cy) in doc coords, scaled to size_doc.
      double pw = m_overflow_glyph_pixbuf->get_width();
      double ph = m_overflow_glyph_pixbuf->get_height();
      double scale = size_doc / std::max(pw, ph);
      cr->translate(cx, cy);
      cr->scale(scale, scale);
      curvz::utils::cairo_set_source_pixbuf(cr,
                                            m_overflow_glyph_pixbuf,
                                            -pw * 0.5, -ph * 0.5);
      cr->paint();
      cr->restore();
    } else {
      // Pixbuf load failed (resource missing) — fallback to a solid
      // red square so the user still sees something. Won't be clickable
      // because check_overflow_hit independently computes the same rect.
      cr->save();
      cr->set_source_rgba(0.91, 0.31, 0.31, 1.0);  // --badge-error dark
      cr->rectangle(cx - size_doc * 0.5, cy - size_doc * 0.5,
                    size_doc, size_doc);
      cr->fill();
      cr->restore();
    }
  }

  if (tl.baselines.empty()) return tl.bytes_consumed;

  // ── s305 m3 — Selection highlight ────────────────────────────────────────
  // When this text node is the active edit and the cursor has a
  // selection, paint a translucent rectangle on each baseline whose
  // byte range overlaps the selection. Drawn BEFORE glyphs so text
  // reads cleanly through the highlight.
  //
  // Per-baseline geometry:
  //   - Clip the selection to the baseline's [byte_start, byte_end)
  //     range. If the clipped range is empty, skip the baseline.
  //   - pango_layout_index_to_pos at each clipped byte (relative to
  //     byte_start) gives the doc-x of that position within the
  //     baseline's Pango layout.
  //   - Rect spans (x_left .. x_right) horizontally and
  //     (y - ascent .. y + descent) vertically — full glyph cell
  //     height so wrapped lines show a connected highlight column.
  //
  // Special case: when the selection ends exactly at byte_end of a
  // baseline (i.e. the selection spans a wrap or newline), extend
  // the rect to the baseline's x_end so the highlight visually
  // includes the line break (matches every text editor's behavior).
  if (&text_obj == m_text_editing && m_text_cursor &&
      m_text_cursor->has_selection()) {
    auto [sel_start, sel_end] = m_text_cursor->selection_range();
    cr->save();
    // Translucent blue; same family as selection chrome elsewhere
    // in the app. Alpha low enough that glyphs remain legible.
    cr->set_source_rgba(0.30, 0.55, 0.95, 0.30);
    for (const auto& bl : tl.baselines) {
      if (!bl.pango) continue;
      // Clip selection to this baseline's byte range.
      size_t bs = bl.byte_start;
      size_t be = bl.byte_end;
      size_t ov_s = std::max(sel_start, bs);
      size_t ov_e = std::min(sel_end,   be);
      if (ov_e <= ov_s && !(sel_start <= bs && sel_end > be)) {
        // No overlap on this line — unless the selection fully spans
        // it (in which case ov_s == ov_e == bs and we still want the
        // line highlighted because the wrap/newline is part of the
        // selection). The compound test catches the "fully contained"
        // case where the selection straddles this whole baseline.
        if (!(sel_start <= bs && sel_end >= be)) continue;
        ov_s = bs;
        ov_e = be;
      }

      // Local (relative-to-byte_start) byte offsets for Pango.
      int rel_s = (int)(ov_s - bs);
      int rel_e = (int)(ov_e - bs);
      // pango_layout_index_to_pos for the start and end byte. For an
      // empty span on this line (ov_s == ov_e), pos.width is 0; we
      // skip in that case unless the selection visually extends past
      // the line's end (extend-to-x_end case).
      PangoRectangle p_s, p_e;
      pango_layout_index_to_pos(bl.pango.get(), rel_s, &p_s);
      pango_layout_index_to_pos(bl.pango.get(), rel_e, &p_e);
      double x_left  = bl.x_start + (double)p_s.x / (double)PANGO_SCALE;
      double x_right = bl.x_start + (double)p_e.x / (double)PANGO_SCALE;

      // If the selection extends past this baseline's last byte
      // (wrap-or-newline continues into the next line), extend the
      // highlight to the baseline's right edge so the line-break
      // gap is included.
      if (sel_end > be) {
        x_right = bl.x_end;
      }

      // Sanity: nothing to draw if zero width and no extend case.
      if (x_right <= x_left) continue;

      double y_top = bl.y - bl.ascent;
      double y_bot = bl.y + bl.descent;
      cr->rectangle(x_left, y_top, x_right - x_left, y_bot - y_top);
    }
    cr->fill();
    cr->restore();
  }

  cr->save();
  // s320 m1 — Frame rotation. Baselines are in the boundary's upright frame
  // (see compute_text_layout); rotate the canvas into that frame so the
  // glyphs land on the rotated box. angle == 0 is a no-op (common rect).
  if (tl.frame_angle != 0.0) {
    cr->translate(tl.frame_cx, tl.frame_cy);
    cr->rotate(tl.frame_angle);
    cr->translate(-tl.frame_cx, -tl.frame_cy);
  }
  // s317 — Clip glyphs to the interior (margin) rect. compute_text_layout
  //   wraps to this width, but a word that measures as fitting can render a
  //   hair wider; clipping guarantees text never spills past the margins,
  //   which is the contract ("text is bound to be inside the margins").
  // s320 m1 — clip in the UPRIGHT frame (the rotation transform above turns
  //   it into the rotated interior rect). For angle == 0 this is identical
  //   to the previous doc-space bbox.
  if (boundary.path && boundary.path->nodes.size() >= 2) {
    const double ca = std::cos(-tl.frame_angle), sa = std::sin(-tl.frame_angle);
    auto upright = [&](double x, double y, double& ux, double& uy) {
      double rx = x - tl.frame_cx, ry = y - tl.frame_cy;
      ux = tl.frame_cx + rx * ca - ry * sa;
      uy = tl.frame_cy + rx * sa + ry * ca;
    };
    const auto& cbn = boundary.path->nodes;
    double cbx0, cby0; upright(cbn[0].x, cbn[0].y, cbx0, cby0);
    double cbx1 = cbx0, cby1 = cby0;
    for (const auto& pn : cbn) {
      double ux, uy; upright(pn.x, pn.y, ux, uy);
      if (ux < cbx0) cbx0 = ux; if (ux > cbx1) cbx1 = ux;
      if (uy < cby0) cby0 = uy; if (uy > cby1) cby1 = uy;
    }
    auto cm = effective_text_margins(&text_obj, &boundary);
    const double clx = cbx0 + cm.left, cty = cby0 + cm.top;
    const double crx = cbx1 - cm.right, cby = cby1 - cm.bottom;
    if (crx > clx && cby > cty) {
      cr->rectangle(clx, cty, crx - clx, cby - cty);
      cr->clip();
    }
  }
  apply_fill(cr, text_obj.fill);

  for (const auto& bl : tl.baselines) {
    if (!bl.pango) continue;
    cr->save();
    // s331 — anchor by the line's ACTUAL baseline, not the node's uniform
    // ascent. With per-run sizes a line's real ascent is its tallest run, so
    // bl.ascent (uniform) would drop the whole line below its fixed leading-
    // stride baseline. pango_layout_get_baseline gives the true baseline; the
    // tall run then overlaps the line above while every baseline stays put.
    // For a uniform line this equals bl.ascent, so unchanged.
    double base_px = pango_layout_get_baseline(bl.pango.get())
                     / (double)PANGO_SCALE;
    cr->move_to(bl.x_start, bl.y - base_px);
    pango_cairo_show_layout(cr->cobj(), bl.pango.get());
    cr->restore();
  }

  // s332 — text stroke render valve. Pango has no stroke attr, so text stroke
  // rides Curvz-private spans (kCurvzStrokeColorAttr: packed 0xRRGGBB or
  // kCurvzStrokeNone; kCurvzStrokeWidthAttr: width doc-px x PANGO_SCALE) and is
  // painted here, per-run. Text stroke is the StyleBar's domain ONLY -- the
  // node's object stroke (node->stroke, the inspector's "box" stroke) is NOT
  // applied to text glyphs, so Reset / None fully clear it and a stale object
  // stroke never paints text it shouldn't. Walk each line's runs+glyphs and
  // stroke only glyphs carrying a real colour span. Runs don't split on the
  // private attrs, so the boundary is resolved PER GLYPH via log_clusters.
  // Reuses the proven glyph-path stroke from the text-on-path painter.
  {
    const auto& spans = text_obj.text_attr_spans;
    bool any_stroke_span = false;
    for (const auto& s : spans)
      if (s.type == curvz::utils::kCurvzStrokeColorAttr) { any_stroke_span = true; break; }
    if (any_stroke_span) {
      const double def_w = UnitSystem::to_px(1.0, Unit::Pt);
      for (const auto& bl : tl.baselines) {
        if (!bl.pango) continue;
        const double baseline_y = bl.y;  // show_layout put the baseline here
        PangoLayoutIter* it = pango_layout_get_iter(bl.pango.get());
        do {
          PangoLayoutRun* run = pango_layout_iter_get_run(it);
          if (!run) continue;
          PangoGlyphString* gs = run->glyphs;
          PangoFont* pf = run->item->analysis.font;
          PangoRectangle rext;
          pango_layout_iter_get_run_extents(it, nullptr, &rext);
          double gx = bl.x_start + rext.x / (double)PANGO_SCALE;
          for (int gi = 0; gi < gs->num_glyphs; ++gi) {
            PangoGlyphInfo& info = gs->glyphs[gi];
            double adv = info.geometry.width / (double)PANGO_SCALE;
            size_t abyte = bl.byte_start + (size_t)run->item->offset +
                           (size_t)gs->log_clusters[gi];
            long color = -2, wscaled = -1;  // -2 = no colour span on this glyph
            for (const auto& s : spans) {
              if ((unsigned)s.start_byte <= abyte && (unsigned)s.end_byte > abyte) {
                if      (s.type == curvz::utils::kCurvzStrokeColorAttr) color   = s.ivalue;
                else if (s.type == curvz::utils::kCurvzStrokeWidthAttr) wscaled = s.ivalue;
              }
            }
            // Only a real colour span strokes; kCurvzStrokeNone (-1) and "no
            // span" (-2) both mean no stroke (no object-stroke fallback).
            if (color >= 0 && info.glyph != PANGO_GLYPH_EMPTY &&
                !(info.glyph & PANGO_GLYPH_UNKNOWN_FLAG)) {
              double sr = (double)((color >> 16) & 0xFF) / 255.0;
              double sg = (double)((color >>  8) & 0xFF) / 255.0;
              double sb = (double)( color        & 0xFF) / 255.0;
              double sw = (wscaled >= 0) ? (double)wscaled / (double)PANGO_SCALE
                                         : def_w;
              cr->save();
              cr->move_to(gx, baseline_y);
              PangoGlyphString single; int lc = 0;
              single.num_glyphs = 1; single.glyphs = &info; single.log_clusters = &lc;
              pango_cairo_glyph_string_path(cr->cobj(), pf, &single);
              cr->set_source_rgb(sr, sg, sb);
              cr->set_line_width(sw);
              cr->stroke();
              cr->restore();
            }
            gx += adv;
          }
        } while (pango_layout_iter_next_run(it));
        pango_layout_iter_free(it);
      }
    }
  }

  cr->restore();
  return tl.bytes_consumed;
}

void Canvas::draw_text_node(const Cairo::RefPtr<Cairo::Context> &cr,
                            const SceneNode &obj) {
  // ── s301 m1c — Bound text routes to draw_text_in_boundary ─────────────
  // Text bound to one or more boundaries lays out via compute_text_layout
  // (the universal flow) and renders one Pango chunk per baseline. If
  // the first boundary iid can't be resolved (deleted, dangling), fall
  // through to the legacy text_x/text_y path so the text remains visible
  // — better degraded behavior than disappearing.
  if (!obj.text_boundary_ids.empty()) {
    SceneNode *boundary = find_text_boundary(obj.text_boundary_ids.front());
    if (boundary && boundary->path) {
      draw_text_in_boundary(cr, obj, *boundary);
      return;
    }
    LOG_DEBUG("draw_text_node: bound but boundary iid='{}' not resolved",
              obj.text_boundary_ids.front());
    // Fall through to legacy rendering.
  }

  if (obj.text_content.empty()) {
    // Draw a placeholder cursor line so the user sees where text will appear.
    // We're inside the doc-space transform (translate+scale already applied).
    cr->save();
    cr->set_source_rgba(0.4, 0.4, 0.4, 0.7);
    cr->set_line_width(1.5 / m_zoom);
    double h = obj.text_font_size;
    cr->move_to(obj.text_x, obj.text_y);
    cr->line_to(obj.text_x, obj.text_y - h);
    cr->stroke();
    cr->restore();
    return;
  }

  // ── Text-on-path branch ───────────────────────────────────────────────
  if (!obj.text_path_id.empty()) {
    LOG_DEBUG("draw_text_node: text_path_id='{}' looking up guide",
              obj.text_path_id);
    SceneNode *guide = top_find_path_by_id(obj.text_path_id);
    if (guide && guide->path) {
      LOG_DEBUG("draw_text_node: guide found id='{}' nodes={}", guide->id,
                guide->path->nodes.size());
      draw_text_on_path(cr, obj, *guide);
      return;
    }
    LOG_DEBUG("draw_text_node: guide NOT found for id='{}' — falling through",
              obj.text_path_id);
    // Guide path not found — fall through to normal rendering
  }

  // Build a Pango layout via the C API (PangoCairo) since pangomm's
  // create_layout requires a Cairo::Context and we have one).
  cr->save();

  // Y-up: text_y is the baseline in doc space. Cairo's Y is down,
  // but we're already inside the translate(ox,oy)+scale(zoom,zoom)
  // transform where Y increases downward (doc_y = canvas_h - user_y).
  // text_y is already in doc (Y-down) space at this point.
  cr->translate(obj.text_x, obj.text_y);

  // Apply fill.
  apply_fill(cr, obj.fill);

  // Use PangoCairo C API directly — reliable, no pangomm wrapping issues.
  PangoLayout *layout = pango_cairo_create_layout(cr->cobj());

  PangoFontDescription *desc = pango_font_description_new();
  pango_font_description_set_family(desc, obj.text_font_family.c_str());
  // Pango size is in 1/PANGO_SCALE points; we treat font_size as pixels at
  // zoom=1.
  pango_font_description_set_absolute_size(desc,
                                           obj.text_font_size * PANGO_SCALE);
  if (obj.text_bold)
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  if (obj.text_italic)
    pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
  pango_layout_set_font_description(layout, desc);
  pango_font_description_free(desc);

  pango_layout_set_text(layout, obj.text_content.c_str(), -1);

  // Letter spacing — applied as a Pango attribute so layout metrics
  // (width, glyph positions) are correct for anchor offset calculation.
  if (obj.text_letter_spacing != 0.0) {
    PangoAttrList *attrs = pango_attr_list_new();
    // Pango letter_spacing is in Pango units (1/PANGO_SCALE pixels)
    pango_attr_list_insert(attrs,
                           pango_attr_letter_spacing_new(
                               (int)(obj.text_letter_spacing * PANGO_SCALE)));
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);
  }

  // Apply paragraph alignment
  if (obj.text_align == "center")
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
  else if (obj.text_align == "right")
    pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
  else
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
  pango_layout_set_justify(layout, obj.text_align == "justify");

  // Horizontal anchor.
  PangoRectangle ink, logical;
  pango_layout_get_pixel_extents(layout, &ink, &logical);
  double off_x = 0.0;
  if (obj.text_anchor == "middle")
    off_x = -logical.width * 0.5;
  if (obj.text_anchor == "end")
    off_x = -logical.width;

  // Move to baseline: Pango draws from top, we want the baseline at y=0.
  PangoLayoutLine *line = pango_layout_get_line_readonly(layout, 0);
  int baseline = pango_layout_get_baseline(layout);
  double base_px = baseline / (double)PANGO_SCALE;

  cr->move_to(off_x, -base_px - obj.text_baseline_shift);
  pango_cairo_show_layout(cr->cobj(), layout);

  // Optional stroke.
  if (obj.stroke.paint.type != FillStyle::Type::None) {
    cr->move_to(off_x, -base_px - obj.text_baseline_shift);
    pango_cairo_layout_path(cr->cobj(), layout);
    apply_stroke_style(cr, obj.stroke);
    cr->stroke();
  }

  g_object_unref(layout);
  cr->restore();
}
void Canvas::on_draw(const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
  // s268 m0 — cold-launch timing. Fires exactly once per process via
  // the static-bool latch. main() stamped g_launch_t0 before any
  // other code ran; the delta to first-paint is what the user
  // perceives as "launch time." One INFO line in the log; no UI
  // effect, no further cost on subsequent draws (the branch
  // mispredict on the latch is negligible at draw frequency).
  if constexpr (LAUNCH_TIMING_ENABLED) {
    static bool first_paint_logged = false;
    if (!first_paint_logged) {
      first_paint_logged = true;
      auto now = std::chrono::steady_clock::now();
      auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - g_launch_t0).count();
      LOG_INFO("cold launch: {} ms (exec -> first paint)", ms);
    }
  }

  // ── Outer background — workspace colour around the artboard ─────────
  // Doc-level field (s148 m1 — re-promoted from project per-motif).
  // Each document carries its own workspace surround tone; switching
  // tabs intentionally re-paints the surround. Falls back to the
  // historical #171717 grey only if no doc is wired (early boot).
  if (m_doc) {
    const Motif mo = doc_motif();
    cr->set_source_rgb(m_doc->workspace_bg_r(mo),
                       m_doc->workspace_bg_g(mo),
                       m_doc->workspace_bg_b(mo));
  } else {
    cr->set_source_rgb(0.09, 0.09, 0.09);
  }
  cr->paint();
  if (!m_doc)
    return;

  // Deferred fit — runs on first draw after set_document, when widget has real
  // size
  if (m_fit_pending && w > 0 && h > 0) {
    m_fit_pending = false;
    zoom_fit();
  }

  const double cw = m_doc->canvas_width() * m_zoom;
  const double ch = m_doc->canvas_height() * m_zoom;
  const double ox = doc_origin_x();
  const double oy = doc_origin_y();

  // Drop shadow — offset + blur-approximated with layered rects
  cr->save();
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.18);
  cr->rectangle(ox + 6, oy + 6, cw, ch);
  cr->fill();
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.25);
  cr->rectangle(ox + 3, oy + 3, cw, ch);
  cr->fill();
  cr->restore();

  cr->save();
  cr->translate(ox, oy);

  // ── Artboard surface — doc-level editor presentation (s148 m1) ───────
  // Outline mode previously kept the same grey; that branch was a no-op
  // even before S98 (both arms set the same colour). Keep the gate in
  // case outline mode wants a different treatment in future, but read
  // the same artboard_bg_* either way for now.
  //
  // s148 m1: read from doc (re-promoted from project per-motif). Each
  // doc carries its own artboard tone — flipping app appearance no
  // longer alters the canvas, the doc owns its colour.
  cr->set_source_rgb(m_doc->artboard_bg_r(doc_motif()),
                     m_doc->artboard_bg_g(doc_motif()),
                     m_doc->artboard_bg_b(doc_motif()));
  cr->rectangle(0, 0, cw, ch);
  cr->fill();

  draw_grid(cr, (int)cw, (int)ch);

  draw_objects(cr);

  cr->restore();


  // ── Artboard border — crisp 1px, clearly visible ──────────────────────
  cr->save();
  cr->translate(ox, oy);
  // Outer glow — subtle halo so edge reads against dark bg
  cr->set_source_rgba(0.6, 0.6, 0.6, 0.15);
  cr->set_line_width(3.0);
  cr->rectangle(0, 0, cw, ch);
  cr->stroke();
  // Crisp inner border
  cr->set_source_rgba(0.55, 0.55, 0.55, 1.0);
  cr->set_line_width(1.0);
  cr->rectangle(0.5, 0.5, cw - 1, ch - 1);
  cr->stroke();
  cr->restore();


  // ── Rubber band / marquee — pure widget space, no translate active ────
  draw_rubber_band(cr);
  draw_marquee(cr);

  // ── Origin drag preview — full-screen dashed crosshair ────────────────
  if (m_origin_preview && m_doc) {
    // Convert user coords to screen
    double doc_x = m_origin_preview_ux;
    double doc_y = m_doc->canvas_height() - m_origin_preview_uy;
    double sx = doc_x * m_zoom + ox;
    double sy = doc_y * m_zoom + oy;

    std::vector<double> dash = {6.0, 4.0};
    cr->save();
    cr->set_source_rgba(1.0, 0.65, 0.0, 0.85);
    cr->set_line_width(1.0);
    cr->set_dash(dash, 0);
    // Vertical line
    cr->move_to(sx + 0.5, 0);
    cr->line_to(sx + 0.5, h);
    cr->stroke();
    // Horizontal line
    cr->move_to(0, sy + 0.5);
    cr->line_to(w, sy + 0.5);
    cr->stroke();
    cr->restore();
  }

  // ── Selection handles — pure screen space, no translate active ────────
  auto obj_layer_visible = [this](SceneNode *obj) -> bool {
    if (!obj || !m_doc)
      return false;
    for (const auto &layer : m_doc->layers)
      for (const auto &child : layer->children)
        if (child.get() == obj)
          return layer->visible;
    return true;
  };

  // Draw outline for all selected objects
  // S66 Phase 3 — Eyedropper also shows the outline + dashed bbox (but
  // without draggable handles, via the bbox_only flag on
  // draw_selection_handles below). Gives visual confirmation of what
  // the eyedropper will apply to.
  //
  // s301 m1i — Also fires when a bound-text edit is active, regardless
  // of tool. The bbox is the user-facing primitive in the unified text
  // container model; the user expects to see its handles whenever they
  // are arranging or editing it. Without this gate the marquee flow
  // (which leaves tool as Text) renders the bbox glyphs and baselines
  // but no handles — confusing because the user can't see what they
  // can grab. Adding `m_text_boundary_editing` to the gate makes the
  // handles follow the conceptual state of the bbox rather than the
  // tool choice.
  if (m_tool == ActiveTool::Selection || m_tool == ActiveTool::Eyedropper ||
      m_text_boundary_editing) {
    cr->save();
    cr->translate(ox, oy);
    cr->scale(m_zoom, m_zoom);
    // S58g: Compounds now participate in the selection halo. A Compound is
    // one visual object (S58d) — selecting it should light up its full
    // outline, including the inner boundaries of any hole. We collect the
    // list of path-bearing SceneNodes for each selected object (Path → 1;
    // Compound → N children) and stroke them all together so the outer
    // contour and any inner rings both get the blue halo.
    for (SceneNode *obj : m_selection) {
      if (!obj_layer_visible(obj))
        continue;

      // Gather the leaf Path subpaths for this selection entry.
      std::vector<const SceneNode *> subpaths;
      if (obj->type == SceneNode::Type::Path && obj->path) {
        subpaths.push_back(obj);
      } else if (obj->type == SceneNode::Type::Compound) {
        for (const auto &ch : obj->children) {
          if (ch && ch->type == SceneNode::Type::Path && ch->path)
            subpaths.push_back(ch.get());
        }
      }
      if (subpaths.empty())
        continue;

      // Apply every subpath to Cairo, then stroke the whole batch twice —
      // once for the dark underlay, once for the blue halo.
      for (const SceneNode *sp : subpaths) {
        BezierPath bp = BezierPath::from_path_data(*sp->path);
        bp.apply_to_cairo(cr);
      }
      cr->set_source_rgba(0.0, 0.0, 0.0, 0.45);
      cr->set_line_width(4.0 / m_zoom);
      std::vector<double> no_dash = {};
      cr->set_dash(no_dash, 0);
      cr->stroke_preserve();
      // Primary object gets full blue, others get dimmer
      bool is_primary = (obj == m_selected);
      cr->set_source_rgba(0.3, 0.6, 1.0, is_primary ? 0.85 : 0.55);
      cr->set_line_width(2.0 / m_zoom);
      cr->stroke();
    }
    cr->restore();

    // ── Align anchor glyph (selection-time key-object marker) ────────
    // Drawn in screen space (translate already restored above) so the
    // glyph stays a constant size at any zoom — same convention as
    // selection handles. Validator-on-read: align_anchor() returns
    // null if the marked object has left the selection. Tool gate is
    // Selection only — Eyedropper shows selection outlines but anchor
    // is meaningless there. Glyph: curvz-anchor-symbolic.svg, loaded
    // once into m_anchor_glyph_pixbuf at 2x size for HiDPI crispness;
    // Cairo downscales for paint via curvz::utils::cairo_set_source_pixbuf,
    // the same pump NewDocumentDialog and DocumentGallery use (s135 m2).
    if (m_tool == ActiveTool::Selection) {
      if (SceneNode *a = align_anchor()) {
        if (obj_layer_visible(a)) {
          if (auto bb = object_bbox(*a)) {
            // Doc-space centre → screen
            double cx = bb->x + bb->w * 0.5;
            double cy = bb->y + bb->h * 0.5;
            double sx, sy;
            doc_to_screen(cx, cy, sx, sy);

            // Lazy-load the anchor pixbuf on first use. The SVG declares
            // width/height of 48 px so the pixbuf rasterises at 48×48 —
            // ample oversampling against the 22-px target. Cairo
            // bilinear-downscales for paint. If load fails (build missed
            // registering the resource), silently no-op and the anchor
            // still works, just without the glyph.
            if (!m_anchor_glyph_pixbuf) {
              try {
                m_anchor_glyph_pixbuf = Gdk::Pixbuf::create_from_resource(
                    "/com/curvz/app/icons/scalable/apps/"
                    "curvz-anchor-symbolic.svg");
              } catch (const Glib::Error &e) {
                LOG_WARN("Canvas: failed to load anchor glyph: {}", e.what());
              }
            }

            // Backdrop disc — keeps the glyph readable against any fill
            // and gives the anchor a visual frame consistent with how
            // selection handles read against busy artwork.
            cr->save();
            cr->arc(sx, sy, 14.0, 0.0, 2.0 * M_PI);
            cr->set_source_rgba(1.0, 1.0, 1.0, 0.85);
            cr->fill_preserve();
            cr->set_source_rgba(0.3, 0.6, 1.0, 0.95);
            cr->set_line_width(1.5);
            cr->stroke();

            if (m_anchor_glyph_pixbuf) {
              // Paint the SVG centred on (sx, sy) at 22 px (slight
              // padding inside the 28 px disc). Cairo composites the
              // 56 px source down to 22 px via bilinear; perfectly
              // crisp on 1x and 2x displays.
              const double kGlyphPx = 22.0;
              double pw = m_anchor_glyph_pixbuf->get_width();
              double ph = m_anchor_glyph_pixbuf->get_height();
              double scale = kGlyphPx / std::max(pw, ph);
              cr->translate(sx, sy);
              cr->scale(scale, scale);
              // s135 m2: pumped — gdk_cairo_set_source_pixbuf was deprecated
              // in GTK 4.20. The pump does a proper RGBA→ARGB32 conversion,
              // sidesteps the deprecation, and matches the per-call cost of
              // the old function.
              curvz::utils::cairo_set_source_pixbuf(cr,
                                                    m_anchor_glyph_pixbuf,
                                                    -pw * 0.5, -ph * 0.5);
              cr->paint();
            }
            cr->restore();
          }
        }
      }
    }

    // Draw one union-BBX handle set for the whole selection.
    // M4c-1: Suppressed when primary selection is a Warp — envelope handles
    // ARE the manipulation UI for Warps; bbox handles get in the way and
    // don't track envelope edits. User flattens first for scale/rotate/skew.
    // S66 Phase 3: Eyedropper shows the same handle UI as Selection for
    // visual consistency; handles are inert under the eyedropper (click
    // commits the sample rather than grabbing a handle).
    if (!m_selection.empty() && !(m_selected && m_selected->is_warp()))
      draw_selection_handles(cr);
  } else if (m_tool == ActiveTool::Node && m_selected &&
             obj_layer_visible(m_selected)) {
    // Node tool — outline primary selected path only.
    // Compounds aren't directly editable in the Node tool (children must be
    // reached via split-compound first), so we keep this branch Path-only.
    if (m_selected->type == SceneNode::Type::Path && m_selected->path) {
      cr->save();
      cr->translate(ox, oy);
      cr->scale(m_zoom, m_zoom);
      BezierPath bp = BezierPath::from_path_data(*m_selected->path);
      bp.apply_to_cairo(cr);
      cr->set_source_rgba(0.0, 0.0, 0.0, 0.45);
      cr->set_line_width(4.0 / m_zoom);
      std::vector<double> no_dash = {};
      cr->set_dash(no_dash, 0);
      cr->stroke_preserve();
      cr->set_source_rgba(0.3, 0.6, 1.0, 0.85);
      cr->set_line_width(2.0 / m_zoom);
      cr->stroke();
      cr->restore();
    }
  }

  // ── Pen tool WIP — document space ─────────────────────────────────────
  if (m_tool == ActiveTool::Pen && m_pen_tool.has_wip) {
    cr->save();
    cr->translate(ox, oy);
    cr->scale(m_zoom, m_zoom);
    // s148 m1: pass doc's Creation colour through (re-promoted from
    // project per-motif). PenTool stays document-agnostic; receives
    // values, doesn't reach back into the doc.
    m_pen_tool.draw_preview(cr, m_zoom,
                            m_doc->creation_color_r(doc_motif()),
                            m_doc->creation_color_g(doc_motif()),
                            m_doc->creation_color_b(doc_motif()));
    cr->restore();
  }

  // ── SVG-performer phantom overlay — document space (s288 m2, s291 m2) ─
  // When the SvgPerformer is mid-performance, it owns a list of phantom
  // shapes (anchor squares, handle lever lines, partial Bezier segments)
  // representing the in-flight construction. The actual model doesn't
  // change during the performance; we paint phantoms on top of whatever's
  // there. At commit time the performer mints a real Path SceneNode and
  // clears its phantoms; subsequent on_draw calls see is_playing() == false
  // and skip this branch.
  //
  // Independent of m_tool — the performance plays regardless of which
  // toolbar tool is "active" (the performance is its own production; the
  // ambient tool doesn't matter to it).
  if (m_svg_performer.is_playing() && m_doc) {
    cr->save();
    cr->translate(ox, oy);
    cr->scale(m_zoom, m_zoom);
    m_svg_performer.draw_overlay(cr, m_zoom,
                                 m_doc->creation_color_r(doc_motif()),
                                 m_doc->creation_color_g(doc_motif()),
                                 m_doc->creation_color_b(doc_motif()));
    cr->restore();
  }

  // ── Continue-path hover indicator ─────────────────────────────────────
  if (m_tool == ActiveTool::Pen && !m_pen_tool.has_wip && m_doc) {
    double doc_x, doc_y;
    screen_to_doc(m_mouse_x, m_mouse_y, doc_x, doc_y);
    double tol = PenTool::CLOSE_RADIUS_PX / m_zoom;
    Vec2 mouse{doc_x, doc_y};
    cr->save();
    cr->translate(ox, oy);
    cr->scale(m_zoom, m_zoom);
    for (const auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (const auto &obj_ptr : layer->children) {
        const SceneNode &obj = *obj_ptr;
        if (obj.type != SceneNode::Type::Path || !obj.path)
          continue;

        // ── Endpoint continue indicator (amber ring) ──────────────
        if (!obj.path->closed && obj.path->nodes.size() >= 2) {
          for (int ei = 0; ei < 2; ++ei) {
            const BezierNode &ep =
                ei == 0 ? obj.path->nodes.front() : obj.path->nodes.back();
            Vec2 ep_pos{ep.x, ep.y};
            if (mouse.dist(ep_pos) <= tol) {
              cr->arc(ep.x, ep.y, PenTool::CLOSE_RADIUS_PX / m_zoom, 0,
                      2 * M_PI);
              cr->set_source_rgba(1.0, 0.7, 0.0, 0.9);
              cr->set_line_width(1.5 / m_zoom);
              cr->stroke();
            }
          }
        }

        // ── Node snap indicator (cyan ring) ───────────────────────
        for (const auto &nd : obj.path->nodes) {
          double sx, sy;
          doc_to_screen(nd.x, nd.y, sx, sy);
          double ddx = m_mouse_x - sx, ddy = m_mouse_y - sy;
          double d2 = ddx * ddx + ddy * ddy;
          static constexpr double NODE_SNAP_PX = 6.0;
          if (d2 < NODE_SNAP_PX * NODE_SNAP_PX) {
            cr->arc(nd.x, nd.y, NODE_SNAP_PX / m_zoom, 0, 2 * M_PI);
            cr->set_source_rgba(0.0, 0.85, 1.0, 0.9); // cyan
            cr->set_line_width(1.5 / m_zoom);
            cr->stroke();
          }
        }
      }
    }
    cr->restore();
  }

  // ── Node editor overlay — document space ──────────────────────────────
  if (m_tool == ActiveTool::Node && m_selected &&
      m_selected->type == SceneNode::Type::Path && m_selected->path) {
    cr->save();
    cr->translate(ox, oy);
    cr->scale(m_zoom, m_zoom);

    // ── Background paths — all non-active paths in layer colour ──────────
    // Drawn before the node overlay so they sit behind handles/nodes.
    // m_selected and any path in m_node_selection are skipped (they get
    // the blue overlay treatment below).
    {
      std::set<SceneNode *> active;
      active.insert(m_selected);
      for (const auto &ns : m_node_selection)
        if (ns.obj)
          active.insert(ns.obj);

      for (int li = 0; li < (int)m_doc->layers.size(); ++li) {
        const auto &layer = m_doc->layers[li];
        if (!layer->visible || layer->is_special_layer())
          continue;
        double lr = 0.88, lg = 0.88, lb = 0.88;
        parse_layer_color(layer->color, lr, lg, lb);
        for (const auto &obj_ptr : layer->children) {
          const SceneNode &obj = *obj_ptr;
          if (obj.type != SceneNode::Type::Path || !obj.path)
            continue;
          if (active.count(const_cast<SceneNode *>(&obj)))
            continue;
          BezierPath bp = BezierPath::from_path_data(*obj.path);
          bp.apply_to_cairo(cr);
          cr->set_source_rgba(lr, lg, lb, 0.40);
          cr->set_line_width(1.0 / m_zoom);
          cr->stroke();
        }
      }
    }

    // Draw overlays for extra paths in m_node_selection first (dimmer)
    std::set<SceneNode *> drawn;
    drawn.insert(m_selected);
    for (const auto &ns : m_node_selection) {
      if (!ns.obj || !ns.obj->path)
        continue;
      if (drawn.count(ns.obj))
        continue;
      drawn.insert(ns.obj);
      BezierPath extra = BezierPath::from_path_data(*ns.obj->path);
      // Draw path outline
      extra.apply_to_cairo(cr);
      cr->set_source_rgba(0.3, 0.6, 1.0, 0.55);
      cr->set_line_width(1.5 / m_zoom);
      cr->stroke();
      // Build selected set for this extra path
      std::unordered_set<int> extra_sel;
      for (const auto &ns2 : m_node_selection)
        if (ns2.obj == ns.obj && ns2.node_idx >= 0)
          extra_sel.insert(ns2.node_idx);
      extra.draw_editor_overlay(cr, m_zoom, extra_sel, extra.closed);
    }

    // Draw primary path overlay on top
    BezierPath bp = BezierPath::from_path_data(*m_selected->path);

    // Build set of selected node indices on the primary path
    std::unordered_set<int> sel_indices;
    if (m_selected_node >= 0)
      sel_indices.insert(m_selected_node);
    for (const auto &ns : m_node_selection) {
      if (ns.obj == m_selected && ns.node_idx >= 0)
        sel_indices.insert(ns.node_idx);
    }
    if (sel_indices.size() > 1)
      bp.draw_editor_overlay(cr, m_zoom, sel_indices, bp.closed);
    else
      bp.draw_editor_overlay(cr, m_zoom, m_selected_node, bp.closed);

    // ── Secondary selection — highlight node on second path ────────────
    if (m_selected2 && m_selected2->path && m_selected_node2 >= 0 &&
        m_selected_node2 < (int)m_selected2->path->nodes.size()) {
      const BezierNode &n2 = m_selected2->path->nodes[m_selected_node2];
      double r = 5.0 / m_zoom;
      cr->arc(n2.x, n2.y, r, 0, 2 * M_PI);
      cr->set_source_rgba(1.0, 0.6, 0.1, 0.9);
      cr->set_line_width(2.0 / m_zoom);
      cr->stroke();
    }

    // ── Multi-node selection on EXTRA paths — drawn by extra path loop above
    // ── (nodes on m_selected are handled by the set-based draw_editor_overlay)

    // ── Endpoint snap indicator — green ring around target endpoint ───
    if (m_snap_target_obj && m_snap_target_obj->path) {
      const auto &nodes = m_snap_target_obj->path->nodes;
      const BezierNode &ep =
          m_snap_target_end == 0 ? nodes.front() : nodes.back();
      double r = 6.0 / m_zoom;
      cr->arc(ep.x, ep.y, r, 0, 2 * M_PI);
      cr->set_source_rgba(0.2, 1.0, 0.4, 0.9);
      cr->set_line_width(1.5 / m_zoom);
      cr->stroke();
    }
    cr->restore();
  }

  // ── Ref tool coordinate overlay — only when Alt or Shift held ────────────
  if (m_tool == ActiveTool::Ref && (m_mod_alt || m_mod_shift))
    draw_ref_coord_overlay(cr);

  // ── Eyedropper colour swatch overlay ──────────────────────────────
  if (m_tool == ActiveTool::Eyedropper)
    draw_eyedropper_overlay(cr);

  // ── Corner tool overlay ───────────────────────────────────────────
  if (m_tool == ActiveTool::Corner)
    draw_corner_tool_overlay(cr);

  // ── Ruler tool overlay ────────────────────────────────────────────────
  if (m_tool == ActiveTool::Measure)
    draw_ruler_overlay(cr, w, h);

  // ── Text-on-Path tool overlay ─────────────────────────────────────────
  if (m_tool == ActiveTool::TextOnPath)
    draw_top_overlay(cr);

  // ── s301 m1b/m1c — Text baseline guides ───────────────────────────────
  // Draw whenever (a) a text edit is active, OR (b) a bound text or its
  // boundary is selected. The helper itself decides which case applies
  // and which text/boundary pair to use.
  draw_text_baseline_guides(cr);

  // ── s328 m4a — text compass (text-angle readout / live rotate guide) ──
  draw_text_compass(cr);

  // ── s322 — Form-fit reflow debug overlay (PARKED s326) ────────────────
  // Pure geometry probe: eroded margin + float-found baselines on a selected
  // closed Path in Node mode. PARKED — it fired on ANY selected closed path,
  // textbox or not (an ellipse selected in node mode lit up with baseline-
  // looking spans), which read as "all closed paths draw baselines." The
  // dispatch is disabled; the body is kept as the validation harness for the
  // layer-2 form-fit work (the "eroded-outline margin frame" backlog item).
  //
  // It also surfaced a real layer-2 finding worth keeping: on a curved
  // boundary the eroded margin comes back as a POLYGON, not an inset curve
  // (an ellipse insets to a diamond) — erode_outline -> offset_path feeds
  // Clipper2 the raw node polygon without flattening the beziers. Fix when
  // layer-2 actually consumes the eroded outline (rect boxes are unaffected:
  // they take offset_path's square-corner fast path).
  //
  // Re-arm by uncommenting the call below.
  // draw_formfit_debug(cr);


  // ── s301 m1c/m1f — Caret render. Inside doc-space transform; the
  //    cursor manages its own line width and color (zoom-aware width,
  //    color from text fill).
  if (m_text_cursor) {
    const double ox_dc = doc_origin_x();
    const double oy_dc = doc_origin_y();
    cr->save();
    cr->translate(ox_dc, oy_dc);
    cr->scale(m_zoom, m_zoom);
    // s319 — when the caret is in the OA, window it like the OA text: clip
    //   to the visible text rect and translate by the scroll offset (derived
    //   in the OA draw, which ran earlier in this paint), so the caret
    //   scrolls with the content and never draws outside the panel.
    bool oa_windowed = false;
    if (caret_in_overflow() && m_text_editing) {
      MgrOverlayLayout L = compute_mgr_overlay_layout(m_text_editing);
      if (L.valid && L.has_overlay) {
        OverflowBubbleLayout B = compute_overflow_bubble_layout(L);
        if (B.valid && B.text_w > 0.0 && B.text_h > 0.0) {
          cr->save();
          cr->rectangle(B.text_x, B.text_y, B.text_w, B.text_h);
          cr->clip();
          cr->translate(0.0, -m_overflow_scroll_y);
          oa_windowed = true;
        }
      }
    }
    m_text_cursor->render(cr);
    if (oa_windowed) cr->restore();
    cr->restore();
  }

  // ── Guide-construct overlay (any tool, pre-empts visual focus) ────────
  if (m_guide_construct_active)
    draw_guide_construct_overlay(cr);

  // ── Persistent measurement overlays (drawn in all tools) ──────────────
  // S89: rendered with the same shared draw_measurement_annotations helper
  // that the live ruler triangle uses, so saved entries look identical to
  // the live one — full structured labels (x,y at A and B / Δx / Δy /
  // distance / α / β) plus triangle + endpoints. Click-to-copy is wired
  // when the ruler tool is active by passing push_labels=true so the
  // labels register hit-test rects in m_ruler_labels.
  if (m_doc) {
    SceneNode *ml = m_doc->measure_layer();
    if (ml && ml->visible) {
      bool ruler_active = (m_tool == ActiveTool::Measure);
      for (auto &ch : ml->children) {
        if (!ch->is_measurement())
          continue;
        // Per-entry visibility — inspector and layers-panel eye toggles
        // flip ch->visible. Whole-layer visibility is the outer ml->visible
        // gate above; this skips individual entries that the user has hidden.
        if (!ch->visible)
          continue;
        // measure_* are user-space (Y-up) — pass straight through.
        draw_measurement_annotations(cr,
                                     ch->measure_x1, ch->measure_y1,
                                     ch->measure_x2, ch->measure_y2,
                                     /*push_labels=*/ruler_active);
      }
    }
  }

  // ── Warp envelope overlay (M4a — display-only) ────────────────────────
  // When a Warp is the primary selection, paint its top/bottom envelope
  // anchors + handles + leashes on top of everything. Screen-space sizes
  // (8px anchors, 6px handle dots) so they stay constant under zoom.
  // Top envelope = orange, Bottom envelope = cyan — different colors so
  // the user can always tell them apart. M4b adds hit-test + drag.
  // M4c-2: elements in the pick set render in bright yellow over the
  // base color so the selection is obvious.
  if (m_selected && m_selected->is_warp()) {
    sync_warp_env_picks_to_selection();
    const SceneNode &wp = *m_selected;
    // Pick-set membership check. Linear over picks — always small.
    auto is_picked = [&](bool is_top, int idx, EnvelopePart part) -> bool {
      for (const auto &p : m_warp_env_picks)
        if (p.is_top == is_top && p.idx == idx && p.part == part)
          return true;
      return false;
    };
    auto draw_envelope = [&](const PathData &env, bool is_top, double r,
                             double g, double b) {
      if (env.nodes.empty())
        return;
      cr->save();
      // Leashes: anchor→cx1 (incoming) and anchor→cx2 (outgoing).
      // Thin, low-alpha version of the envelope color.
      cr->set_source_rgba(r, g, b, 0.55);
      cr->set_line_width(1.0);
      std::vector<double> no_dash;
      cr->set_dash(no_dash, 0);
      for (const auto &n : env.nodes) {
        double asx, asy, h1sx, h1sy, h2sx, h2sy;
        doc_to_screen(n.x, n.y, asx, asy);
        doc_to_screen(n.cx1, n.cy1, h1sx, h1sy);
        doc_to_screen(n.cx2, n.cy2, h2sx, h2sy);
        // Only draw the leash if the handle isn't coincident with anchor
        // (avoids drawing zero-length segments for degenerate cases).
        if (std::hypot(h1sx - asx, h1sy - asy) > 0.5) {
          cr->move_to(asx, asy);
          cr->line_to(h1sx, h1sy);
          cr->stroke();
        }
        if (std::hypot(h2sx - asx, h2sy - asy) > 0.5) {
          cr->move_to(asx, asy);
          cr->line_to(h2sx, h2sy);
          cr->stroke();
        }
      }
      // Handle dots: 6px. Picked → yellow fill + bold black ring.
      // Unpicked → white fill + colored ring.
      for (int i = 0; i < (int)env.nodes.size(); ++i) {
        const BezierNode &n = env.nodes[i];
        double h1sx, h1sy, h2sx, h2sy, asx, asy;
        doc_to_screen(n.x, n.y, asx, asy);
        doc_to_screen(n.cx1, n.cy1, h1sx, h1sy);
        doc_to_screen(n.cx2, n.cy2, h2sx, h2sy);
        auto draw_handle = [&](double sx, double sy, bool picked) {
          if (std::hypot(sx - asx, sy - asy) < 0.5)
            return; // coincident
          if (picked) {
            cr->set_source_rgba(1.0, 0.93, 0.20, 1.0); // yellow
            cr->arc(sx, sy, 3.5, 0, 2 * M_PI);
            cr->fill();
            cr->set_source_rgba(0.0, 0.0, 0.0, 1.0);
            cr->set_line_width(1.5);
            cr->arc(sx, sy, 3.5, 0, 2 * M_PI);
            cr->stroke();
          } else {
            cr->set_source_rgba(1.0, 1.0, 1.0, 0.95);
            cr->arc(sx, sy, 3.0, 0, 2 * M_PI);
            cr->fill();
            cr->set_source_rgba(r, g, b, 1.0);
            cr->set_line_width(1.5);
            cr->arc(sx, sy, 3.0, 0, 2 * M_PI);
            cr->stroke();
          }
        };
        draw_handle(h1sx, h1sy, is_picked(is_top, i, EnvelopePart::HandleIn));
        draw_handle(h2sx, h2sy, is_picked(is_top, i, EnvelopePart::HandleOut));
      }
      // Anchor squares: 8px. Picked → yellow fill + bolder black outline.
      // Unpicked → envelope-color fill + thin black outline.
      for (int i = 0; i < (int)env.nodes.size(); ++i) {
        const BezierNode &n = env.nodes[i];
        double asx, asy;
        doc_to_screen(n.x, n.y, asx, asy);
        bool picked = is_picked(is_top, i, EnvelopePart::Anchor);
        if (picked) {
          cr->set_source_rgba(1.0, 0.93, 0.20, 1.0); // yellow
          cr->rectangle(asx - 5.0, asy - 5.0, 10.0, 10.0);
          cr->fill_preserve();
          cr->set_source_rgba(0.0, 0.0, 0.0, 1.0);
          cr->set_line_width(1.5);
          cr->stroke();
        } else {
          cr->set_source_rgba(r, g, b, 1.0);
          cr->rectangle(asx - 4.0, asy - 4.0, 8.0, 8.0);
          cr->fill_preserve();
          cr->set_source_rgba(0.0, 0.0, 0.0, 0.9);
          cr->set_line_width(1.0);
          cr->stroke();
        }
      }
      cr->restore();
    };
    // Top envelope = orange. Bottom envelope = cyan.
    draw_envelope(wp.warp_env_top, true, 1.0, 0.55, 0.10);
    draw_envelope(wp.warp_env_bottom, false, 0.15, 0.70, 0.95);
  }
}

void foreach_corner_node(CurvzDocument *doc,
                                std::function<bool(SceneNode *, int)> fn) {
  if (!doc)
    return;
  std::function<void(SceneNode *)> walk = [&](SceneNode *node) {
    if (node->type == SceneNode::Type::Path && node->path) {
      for (int i = 0; i < (int)node->path->nodes.size(); ++i) {
        auto t = node->path->nodes[i].type;
        if (t == BezierNode::Type::Corner || t == BezierNode::Type::Cusp)
          if (fn(node, i))
            return;
      }
    } else if (node->type == SceneNode::Type::Group ||
               node->type == SceneNode::Type::Compound) {
      for (auto &ch : node->children)
        walk(ch.get());
    }
  };
  for (auto &layer : doc->layers) {
    if (!layer->visible || layer->locked || layer->is_special_layer())
      continue;
    for (auto &obj : layer->children)
      walk(obj.get());
  }
}

void Canvas::draw_corner_tool_overlay(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_doc)
    return;

  double ox = doc_origin_x();
  double oy = doc_origin_y();

  cr->save();
  cr->translate(ox, oy);
  cr->scale(m_zoom, m_zoom);

  const double node_r = 4.0 / m_zoom;

  // Draw all paths faintly so user can see geometry
  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->is_special_layer())
      continue;
    double lr = 0.78, lg = 0.78, lb = 0.78;
    parse_layer_color(layer->color, lr, lg, lb);
    std::function<void(SceneNode *)> draw_path = [&](SceneNode *node) {
      if (node->type == SceneNode::Type::Path && node->path) {
        BezierPath bp = BezierPath::from_path_data(*node->path);
        bp.apply_to_cairo(cr);
        cr->set_source_rgba(lr, lg, lb, 0.35);
        cr->set_line_width(1.0 / m_zoom);
        cr->stroke();
      } else if (node->type == SceneNode::Type::Group ||
                 node->type == SceneNode::Type::Compound) {
        for (auto &ch : node->children)
          draw_path(ch.get());
      }
    };
    for (auto &obj : layer->children)
      draw_path(obj.get());
  }

  // Draw nodes: Corner/Cusp are orange squares; other types are dimmed grey
  // dots
  foreach_corner_node(m_doc, [&](SceneNode *obj, int i) {
    const BezierNode &n = obj->path->nodes[i];
    bool selected = corner_sel_contains(obj, i);

    if (selected) {
      // Filled blue square
      cr->set_source_rgba(0.2, 0.5, 1.0, 1.0);
      cr->rectangle(n.x - node_r, n.y - node_r, node_r * 2, node_r * 2);
      cr->fill_preserve();
      cr->set_source_rgba(1.0, 1.0, 1.0, 1.0);
      cr->set_line_width(1.0 / m_zoom);
      cr->stroke();
    } else {
      // Orange hollow square — selectable corner/cusp
      cr->set_source_rgba(1.0, 0.55, 0.1, 0.9);
      cr->rectangle(n.x - node_r, n.y - node_r, node_r * 2, node_r * 2);
      cr->set_line_width(1.0 / m_zoom);
      cr->stroke();
    }
    return false;
  });

  // Dim non-corner nodes with small grey circles so user can see them but
  // knows they are not selectable
  auto draw_inert = [&](SceneNode *node) {
    std::function<void(SceneNode *)> walk = [&](SceneNode *n) {
      if (n->type == SceneNode::Type::Path && n->path) {
        for (auto &bn : n->path->nodes) {
          auto t = bn.type;
          if (t == BezierNode::Type::Corner || t == BezierNode::Type::Cusp)
            continue; // already drawn above
          cr->arc(bn.x, bn.y, node_r * 0.7, 0, 2 * M_PI);
          cr->set_source_rgba(0.6, 0.6, 0.6, 0.4);
          cr->fill();
        }
      } else if (n->type == SceneNode::Type::Group ||
                 n->type == SceneNode::Type::Compound) {
        for (auto &ch : n->children)
          walk(ch.get());
      }
    };
    walk(node);
  };
  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->is_special_layer())
      continue;
    for (auto &obj : layer->children)
      draw_inert(obj.get());
  }

  cr->restore();

  // Rubber-band rect (screen space, outside translate/scale)
  if (m_corner_rubber_active) {
    double x0 = std::min(m_corner_rubber_x0, m_corner_rubber_x1);
    double y0 = std::min(m_corner_rubber_y0, m_corner_rubber_y1);
    double x1 = std::max(m_corner_rubber_x0, m_corner_rubber_x1);
    double y1 = std::max(m_corner_rubber_y0, m_corner_rubber_y1);
    cr->save();
    cr->rectangle(x0, y0, x1 - x0, y1 - y0);
    cr->set_source_rgba(0.3, 0.6, 1.0, 0.12);
    cr->fill_preserve();
    cr->set_source_rgba(0.3, 0.6, 1.0, 0.8);
    cr->set_line_width(1.0);
    std::vector<double> dash = {4.0, 3.0};
    cr->set_dash(dash, 0);
    cr->stroke();
    cr->restore();
  }
}

// ── draw_guide_construct_overlay ───────────────────────────────────────────
// Renders the two-point guide construction preview:
//   Phase 0: only cursor hint
//   Phase 1: marker at p1, dashed preview segment p1 → mouse
//   Phase 2: markers at p1 + p2, infinite-line preview at committed vector
//            (orange).  If perpendicular, draw the perpendicular through the
//            midpoint instead (dashed style to signal "alternate mode").
//
// Additionally, in phases 0 and 1 (when the user still has nodes to click),
// overlay node markers on every visible path so the user sees every legal
// snap target, with the currently-nearest-within-tolerance node highlighted.
void Canvas::draw_guide_construct_overlay(
    const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_guide_construct_active || !m_doc)
    return;

  // ── Node markers on every visible non-special layer (snap targets) ────
  // Only while the user still has nodes to click (phases 0 and 1).  Same
  // visual idiom as the Ruler tool overlay.
  if (m_guide_construct_phase < 2) {
    // Determine which node (if any) is currently the snap candidate, so we
    // can highlight it.  Snap tolerance mirrors the press / motion code:
    // 8 px / zoom = 8 doc units when zoom == 1.
    const double tol = 8.0 / m_zoom;
    SceneNode *hot_obj = nullptr;
    int hot_idx = -1;
    double best_d = tol;
    std::vector<std::pair<SceneNode *, int>> all_nodes;
    ruler_collect_all_path_nodes(all_nodes);
    for (auto &[obj, ni] : all_nodes) {
      const BezierNode &n = obj->path->nodes[ni];
      double d = std::hypot(n.x - m_cursor_doc_x, n.y - m_cursor_doc_y);
      if (d < best_d) {
        best_d = d;
        hot_obj = obj;
        hot_idx = ni;
      }
    }

    cr->save();
    const double ox = doc_origin_x();
    const double oy = doc_origin_y();
    cr->translate(ox, oy);
    cr->scale(m_zoom, m_zoom);
    std::vector<double> no_dash;
    cr->set_dash(no_dash, 0);

    const double ns = 3.5 / m_zoom; // half-size in doc units
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        const SceneNode &obj = *obj_uptr;
        if (obj.type != SceneNode::Type::Path || !obj.path)
          continue;
        for (int ni = 0; ni < (int)obj.path->nodes.size(); ++ni) {
          const BezierNode &nd = obj.path->nodes[ni];
          bool is_hot = (&obj == hot_obj && ni == hot_idx);
          if (is_hot) {
            // Snap candidate — filled orange, larger.
            const double hs = ns * 1.4;
            cr->set_source_rgba(1.0, 0.55, 0.0, 1.0);
            cr->rectangle(nd.x - hs, nd.y - hs, hs * 2, hs * 2);
            cr->fill();
          } else {
            cr->set_source_rgba(0.85, 0.85, 0.85, 0.9);
            cr->rectangle(nd.x - ns, nd.y - ns, ns * 2, ns * 2);
            cr->fill();
            cr->set_source_rgba(0.4, 0.4, 0.4, 0.9);
            cr->set_line_width(0.75 / m_zoom);
            cr->rectangle(nd.x - ns, nd.y - ns, ns * 2, ns * 2);
            cr->stroke();
          }
        }
      }
    }
    cr->restore();
  }

  auto draw_dot = [&](double dx, double dy, double r, double g, double b) {
    double sx, sy;
    doc_to_screen(dx, dy, sx, sy);
    cr->save();
    cr->set_source_rgba(0.0, 0.0, 0.0, 0.6);
    cr->arc(sx, sy, 5.0, 0, 2 * M_PI);
    cr->fill();
    cr->set_source_rgba(r, g, b, 1.0);
    cr->arc(sx, sy, 4.0, 0, 2 * M_PI);
    cr->fill();
    cr->restore();
  };

  auto draw_preview_line = [&](double ax, double ay, double bx, double by,
                               bool dashed, bool infinite) {
    double asx, asy, bsx, bsy;
    doc_to_screen(ax, ay, asx, asy);
    doc_to_screen(bx, by, bsx, bsy);
    cr->save();
    cr->set_source_rgba(1.0, 0.55, 0.0, 0.9);
    cr->set_line_width(1.5);
    if (dashed) {
      std::vector<double> dash = {6.0, 4.0};
      cr->set_dash(dash, 0);
    }
    if (infinite) {
      // Extend the line past the widget bounds in both directions.
      double w = get_width();
      double h = get_height();
      double span = std::hypot(w, h) * 2.0 + 1000.0;
      double dx = bsx - asx;
      double dy = bsy - asy;
      double len = std::hypot(dx, dy);
      if (len < 1e-6) {
        cr->restore();
        return;
      }
      double ux = dx / len;
      double uy = dy / len;
      double mx = (asx + bsx) * 0.5;
      double my = (asy + bsy) * 0.5;
      cr->move_to(mx - ux * span, my - uy * span);
      cr->line_to(mx + ux * span, my + uy * span);
    } else {
      cr->move_to(asx, asy);
      cr->line_to(bsx, bsy);
    }
    cr->stroke();
    cr->restore();
  };

  if (m_guide_construct_phase == 1) {
    // p1 captured, previewing to current mouse.
    draw_preview_line(m_guide_construct_p1_x, m_guide_construct_p1_y,
                      m_guide_construct_preview_x, m_guide_construct_preview_y,
                      /*dashed=*/true, /*infinite=*/false);
    draw_dot(m_guide_construct_p1_x, m_guide_construct_p1_y, 1.0, 0.55, 0.0);
    draw_dot(m_guide_construct_preview_x, m_guide_construct_preview_y, 1.0, 0.8,
             0.2);
  } else if (m_guide_construct_phase >= 2) {
    // Locked preview: show vector from p1→p2, then the infinite proposal line
    // (dashed if perpendicular is active — hints that the proposal line is
    // NOT along the clicked vector).
    draw_preview_line(m_guide_construct_p1_x, m_guide_construct_p1_y,
                      m_guide_construct_p2_x, m_guide_construct_p2_y,
                      /*dashed=*/true, /*infinite=*/false);
    // Compute midpoint + direction for the infinite proposal line.
    const double mx = (m_guide_construct_p1_x + m_guide_construct_p2_x) * 0.5;
    const double my = (m_guide_construct_p1_y + m_guide_construct_p2_y) * 0.5;
    double dx = m_guide_construct_p2_x - m_guide_construct_p1_x;
    double dy = m_guide_construct_p2_y - m_guide_construct_p1_y;
    if (m_guide_construct_perpendicular) {
      // Rotate 90° in doc-Y-down space.
      double tmp = dx;
      dx = -dy;
      dy = tmp;
    }
    draw_preview_line(mx - dx, my - dy, mx + dx, my + dy,
                      /*dashed=*/false, /*infinite=*/true);
    draw_dot(m_guide_construct_p1_x, m_guide_construct_p1_y, 1.0, 0.55, 0.0);
    draw_dot(m_guide_construct_p2_x, m_guide_construct_p2_y, 1.0, 0.55, 0.0);
    // Midpoint marker — smaller, cyan-ish, to show anchor of the commit.
    draw_dot(mx, my, 0.3, 0.85, 1.0);
  }
}

void Canvas::draw_eyedropper_overlay(const Cairo::RefPtr<Cairo::Context> &cr) {
  // S66 — Phase 3. The old solid-swatch overlay was replaced by the loupe.
  // Keep this entry point stable (on_draw still calls it) and dispatch.
  draw_eyedropper_loupe(cr);
}

void Canvas::refresh_loupe_buffer() {
  // Render a small screen-space region centred on the cursor into a scratch
  // ImageSurface. Reads back the centre pixel into m_loupe_buffer_{rgba}.
  // The surface is kept for draw_eyedropper_loupe's magnified display.
  //
  // Source-pixel footprint: fixed 15×15 source pixels around the cursor.
  // That's 7 on each side. At 10× magnification the drawn loupe body is
  // 150×150 — a touch larger than the 120-circle diameter so the circle
  // crops nicely and the centre pixel sits dead-on the cursor.
  if (!m_doc) {
    m_loupe_buffer_valid = false;
    return;
  }
  const int W = get_allocated_width();
  const int H = get_allocated_height();
  if (W <= 0 || H <= 0) {
    m_loupe_buffer_valid = false;
    return;
  }

  const int SAMPLE = 15; // odd, so there's a single centre pixel
  const int HALF = SAMPLE / 2;

  m_loupe_sample_w = SAMPLE;
  m_loupe_sample_h = SAMPLE;

  const int cx = (int)std::lround(m_mouse_x);
  const int cy = (int)std::lround(m_mouse_y);
  const int sx0 = cx - HALF;
  const int sy0 = cy - HALF;

  // (Re)allocate the scratch surface on first use / size change.
  if (!m_loupe_surface ||
      m_loupe_surface->get_width() != SAMPLE ||
      m_loupe_surface->get_height() != SAMPLE) {
    m_loupe_surface = Cairo::ImageSurface::create(
        Cairo::Surface::Format::ARGB32, SAMPLE, SAMPLE);
  }

  auto scr = Cairo::Context::create(m_loupe_surface);
  // Workspace background under everything — on_draw paints this first.
  scr->set_source_rgb(0.09, 0.09, 0.09);
  scr->paint();

  // Translate so the main-window screen region (sx0, sy0)..(sx0+SAMPLE,
  // sy0+SAMPLE) lands at the scratch surface's origin, then replay the
  // artboard origin + artboard fill + draw_objects pipeline. draw_objects
  // itself applies m_zoom per-layer, so we only need the translation.
  const double ox = doc_origin_x();
  const double oy = doc_origin_y();
  const double cw = m_doc->canvas_width() * m_zoom;
  const double ch = m_doc->canvas_height() * m_zoom;

  scr->save();
  scr->translate(ox - sx0, oy - sy0);
  // Artboard fill (matches on_draw's artboard rect).
  scr->set_source_rgb(0.157, 0.157, 0.157);
  scr->rectangle(0, 0, cw, ch);
  scr->fill();
  draw_grid(scr, (int)cw, (int)ch);
  draw_objects(scr);
  scr->restore();

  m_loupe_surface->flush();

  // Read back the centre pixel (ARGB32 little-endian: B G R A).
  unsigned char *data = m_loupe_surface->get_data();
  int stride = m_loupe_surface->get_stride();
  unsigned char *px = data + HALF * stride + HALF * 4;
  unsigned int b = px[0];
  unsigned int g = px[1];
  unsigned int r = px[2];
  unsigned int a = px[3];
  // ARGB32 stores premultiplied alpha. Un-premultiply for a FillStyle
  // value that will be applied to a fresh solid paint.
  if (a > 0 && a < 255) {
    r = std::min(255u, (r * 255u + a / 2) / a);
    g = std::min(255u, (g * 255u + a / 2) / a);
    b = std::min(255u, (b * 255u + a / 2) / a);
  }
  m_loupe_buffer_r = r / 255.0;
  m_loupe_buffer_g = g / 255.0;
  m_loupe_buffer_b = b / 255.0;
  m_loupe_buffer_a = a / 255.0;
  m_loupe_buffer_valid = true;
}

void Canvas::draw_eyedropper_loupe(const Cairo::RefPtr<Cairo::Context> &cr) {
  // S66 — Phase 3 always-zoom loupe.
  //
  // 120px circular magnifier that follows the cursor. Always shows the
  // rendered-buffer sample at 10×, with a light-grey pixel grid and a
  // crosshair (black + white halo) marking the centre pixel — the pixel
  // that clicking commits. Hex readout in a pill below the circle.
  //
  // Offset is applied edge-to-cursor (not bounding-box-corner-to-cursor),
  // so the near edge of the circle sits OFFSET pixels from the cursor.
  // Preferred direction is up-right; flips per-axis when near an edge of
  // the widget, with a final clamp so the loupe stays fully visible.
  if (!m_loupe_buffer_valid || !m_loupe_surface)
    return;

  const double cursor_x = m_mouse_x;
  const double cursor_y = m_mouse_y;
  const double RADIUS = 60.0;          // 120px circle
  const double DIAMETER = RADIUS * 2.0;
  const double EDGE_OFFSET = 8.0;      // cursor → nearest circle edge
  const double READOUT_H = 22.0;
  const double READOUT_PAD_Y = 6.0;
  const double TOTAL_H = DIAMETER + READOUT_PAD_Y + READOUT_H;
  const int W = get_allocated_width();
  const int H = get_allocated_height();

  // Bounding box top-left (box_l, box_t) for the loupe (circle + readout).
  // try_dir sets them so the near CIRCLE edge is EDGE_OFFSET from cursor.
  double box_l = 0.0, box_t = 0.0;
  auto try_dir = [&](int dx, int dy) {
    // dx: +1 = right of cursor, -1 = left. dy: -1 = above, +1 = below.
    box_l = (dx > 0) ? (cursor_x + EDGE_OFFSET)
                     : (cursor_x - EDGE_OFFSET - DIAMETER);
    box_t = (dy < 0) ? (cursor_y - EDGE_OFFSET - TOTAL_H)
                     : (cursor_y + EDGE_OFFSET);
  };
  // Preferred: up-right.
  try_dir(+1, -1);
  bool right_ok = box_l + DIAMETER <= W;
  bool up_ok = box_t >= 0;
  if (!(right_ok && up_ok))
    try_dir(right_ok ? +1 : -1, up_ok ? -1 : +1);
  // Final clamp into widget bounds.
  if (box_l < 4.0) box_l = 4.0;
  if (box_t < 4.0) box_t = 4.0;
  if (box_l + DIAMETER > W - 4.0) box_l = W - 4.0 - DIAMETER;
  if (box_t + TOTAL_H > H - 4.0) box_t = H - 4.0 - TOTAL_H;

  const double cx = box_l + RADIUS;
  const double cy = box_t + RADIUS;

  // ── Circle body ──────────────────────────────────────────────────────
  cr->save();

  // Drop shadow — soft, reads against any canvas content.
  cr->arc(cx + 1.5, cy + 2.0, RADIUS + 0.5, 0, 2 * M_PI);
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.35);
  cr->fill();

  // Clip to the circle for the magnified-pixel fill + overlays.
  cr->arc(cx, cy, RADIUS, 0, 2 * M_PI);
  cr->clip_preserve();

  // Magnified pixels: 10× NEAREST scale, centred over (cx, cy).
  const double MAG = 10.0;
  const double draw_w = m_loupe_sample_w * MAG;
  const double draw_h = m_loupe_sample_h * MAG;
  const double grid_left = cx - draw_w * 0.5;
  const double grid_top = cy - draw_h * 0.5;

  cr->save();
  cr->translate(grid_left, grid_top);
  cr->scale(MAG, MAG);
  auto pat = Cairo::SurfacePattern::create(m_loupe_surface);
  pat->set_filter(Cairo::SurfacePattern::Filter::NEAREST);
  cr->set_source(pat);
  cr->paint();
  cr->restore();

  // Pixel grid — light grey, always visible. Fainter than on a solid
  // workspace background but still readable over any sampled pixel.
  cr->save();
  cr->set_source_rgba(0.78, 0.78, 0.78, 0.55);
  cr->set_line_width(1.0);
  for (int i = 1; i < m_loupe_sample_w; ++i) {
    double x = grid_left + i * MAG;
    cr->move_to(x + 0.5, grid_top);
    cr->line_to(x + 0.5, grid_top + draw_h);
  }
  for (int j = 1; j < m_loupe_sample_h; ++j) {
    double y = grid_top + j * MAG;
    cr->move_to(grid_left, y + 0.5);
    cr->line_to(grid_left + draw_w, y + 0.5);
  }
  cr->stroke();
  cr->restore();

  // Centre-pixel crosshair — black + with a white halo so it reads on
  // any background colour. Drawn through the centre pixel, width = 1 pixel
  // cell so the halo completely surrounds the black line.
  const int HALF = m_loupe_sample_w / 2;
  const double centre_left = grid_left + HALF * MAG;
  const double centre_top = grid_top + HALF * MAG;
  const double centre_x = centre_left + MAG * 0.5;
  const double centre_y = centre_top + MAG * 0.5;
  const double ARM = MAG * 1.4; // crosshair extends slightly past the pixel
  cr->save();
  // White halo — drawn first, wider line.
  cr->set_source_rgba(1.0, 1.0, 1.0, 0.95);
  cr->set_line_width(3.0);
  cr->set_line_cap(Cairo::Context::LineCap::ROUND);
  cr->move_to(centre_x - ARM, centre_y);
  cr->line_to(centre_x + ARM, centre_y);
  cr->move_to(centre_x, centre_y - ARM);
  cr->line_to(centre_x, centre_y + ARM);
  cr->stroke();
  // Black crosshair — drawn on top, thinner.
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.95);
  cr->set_line_width(1.2);
  cr->move_to(centre_x - ARM, centre_y);
  cr->line_to(centre_x + ARM, centre_y);
  cr->move_to(centre_x, centre_y - ARM);
  cr->line_to(centre_x, centre_y + ARM);
  cr->stroke();
  cr->restore();

  cr->reset_clip();

  // Outer ring — dark for contrast.
  cr->arc(cx, cy, RADIUS, 0, 2 * M_PI);
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.85);
  cr->set_line_width(2.0);
  cr->stroke();

  // Inner highlight ring — thin white, so the body separates from the
  // dark outer ring.
  cr->arc(cx, cy, RADIUS - 1.5, 0, 2 * M_PI);
  cr->set_source_rgba(1.0, 1.0, 1.0, 0.35);
  cr->set_line_width(1.0);
  cr->stroke();

  // ── Readout strip ────────────────────────────────────────────────────
  // Hex + alpha% of the centre pixel.
  char readout[64] = {0};
  unsigned r = (unsigned)std::lround(m_loupe_buffer_r * 255.0);
  unsigned g = (unsigned)std::lround(m_loupe_buffer_g * 255.0);
  unsigned b = (unsigned)std::lround(m_loupe_buffer_b * 255.0);
  unsigned a = (unsigned)std::lround(m_loupe_buffer_a * 255.0);
  if (a < 255)
    std::snprintf(readout, sizeof(readout), "#%02X%02X%02X  %u%%", r, g, b,
                  (unsigned)std::lround(m_loupe_buffer_a * 100.0));
  else
    std::snprintf(readout, sizeof(readout), "#%02X%02X%02X", r, g, b);

  const double strip_y = box_t + DIAMETER + READOUT_PAD_Y;
  const double strip_x = box_l;

  // Rounded pill behind the text.
  cr->save();
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.75);
  double rr = READOUT_H * 0.5;
  cr->move_to(strip_x + rr, strip_y);
  cr->arc(strip_x + DIAMETER - rr, strip_y + rr, rr, -M_PI / 2, M_PI / 2);
  cr->line_to(strip_x + rr, strip_y + READOUT_H);
  cr->arc(strip_x + rr, strip_y + rr, rr, M_PI / 2, 3 * M_PI / 2);
  cr->close_path();
  cr->fill();
  cr->restore();

  // Text — monospace, centred in the strip.
  cr->save();
  cr->select_font_face("Monospace", Cairo::ToyFontFace::Slant::NORMAL,
                       Cairo::ToyFontFace::Weight::NORMAL);
  cr->set_font_size(12.0);
  Cairo::TextExtents ext;
  cr->get_text_extents(readout, ext);
  double tx = strip_x + (DIAMETER - ext.width) * 0.5 - ext.x_bearing;
  double ty = strip_y + (READOUT_H - ext.height) * 0.5 - ext.y_bearing;
  cr->set_source_rgba(1.0, 1.0, 1.0, 0.95);
  cr->move_to(tx, ty);
  cr->show_text(readout);
  cr->restore();

  cr->restore();
}

void Canvas::draw_ref_coord_overlay(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_doc)
    return;
  // Snap to whole units for display
  double doc_x = std::round(snap_x(m_cursor_doc_x));
  double doc_y = std::round(snap_y(m_cursor_doc_y));

  // Format in display/user-space coords (ruler origin convention)
  double ux = doc_x - m_doc->ruler_origin_x;
  double uy = (m_doc->canvas_height() - doc_y) - m_doc->ruler_origin_y;

  char buf[64];
  snprintf(buf, sizeof(buf), "X: %.0f  Y: %.0f", ux, uy);

  double sx, sy;
  doc_to_screen(doc_x, doc_y, sx, sy);
  double lx = sx + 14.0;
  double ly = sy - 6.0;

  cr->save();
  const double PAD_H = 6.0, PAD_V = 4.0;
  cr->select_font_face("monospace", Cairo::ToyFontFace::Slant::NORMAL,
                       Cairo::ToyFontFace::Weight::NORMAL);
  cr->set_font_size(11.0);
  Cairo::TextExtents te;
  cr->get_text_extents(buf, te);
  double bw = te.width + PAD_H * 2;
  double bh = te.height + PAD_V * 2;
  double bx = lx, by = ly - bh;
  cr->set_source_rgba(0.1, 0.1, 0.1, 0.82);
  cr->rectangle(bx, by, bw, bh);
  cr->fill();
  cr->set_source_rgba(1.0, 1.0, 1.0, 0.95);
  cr->move_to(bx + PAD_H, by + PAD_V + te.height);
  cr->show_text(buf);
  cr->restore();
}

void Canvas::draw_grid(const Cairo::RefPtr<Cairo::Context> &cr, int cw,
                       int ch) {
  // Grid is drawn in screen space (already translated to artboard origin,
  // but NOT yet scaled — cw/ch are screen pixels of the artboard).
  //
  // Show grid when individual document units are at least 8 screen pixels
  // apart — below that the lines are too dense to be useful.
  // This replaces the old `m_zoom < 4.0` pixel check which was meaningless
  // for ratio-based documents (a 1000-unit doc at fit-zoom has m_zoom~0.5).
  if (m_zoom < 8.0)
    return;

  cr->save();
  cr->set_source_rgba(0.7, 0.8, 1.0, 0.18);
  cr->set_line_width(0.5);
  // step = m_zoom means one grid line per document unit (1:1 with doc grid).
  // This is correct: at m_zoom==8 each unit is 8px wide — fine grain visible.
  const double step = m_zoom;
  for (double x = step; x < cw; x += step) {
    cr->move_to(x, 0);
    cr->line_to(x, ch);
  }
  for (double y = step; y < ch; y += step) {
    cr->move_to(0, y);
    cr->line_to(cw, y);
  }
  cr->stroke();
  cr->restore();
}

void Canvas::apply_fill(const Cairo::RefPtr<Cairo::Context> &cr,
                        const FillStyle &fill) {
  // Bbox-less form. For solid/None/CurrentColor this is fine; for gradients
  // we don't know the shape's bbox so we degrade to the first stop's flat
  // colour (still produces *something* visible rather than crashing). The
  // bbox-aware overload is what real renders go through.
  if (fill.is_gradient()) {
    if (!fill.stops.empty()) {
      const auto &s = fill.stops.front();
      cr->set_source_rgba(s.r, s.g, s.b, s.a);
    } else {
      cr->set_source_rgba(0, 0, 0, 0);
    }
    return;
  }
  switch (fill.type) {
  case FillStyle::Type::None:
    break;
  case FillStyle::Type::CurrentColor:
    cr->set_source_rgb(0.88, 0.88,
                       0.88); // preview as near-white on dark artboard
    break;
  case FillStyle::Type::Solid:
    cr->set_source_rgba(fill.r, fill.g, fill.b, fill.a);
    break;
  case FillStyle::Type::LinearGradient:
  case FillStyle::Type::RadialGradient:
    // unreachable — handled above by is_gradient() branch.
    break;
  }
}

// Bbox-aware overload — required for gradient fills. Stops are stored in
// objectBoundingBox-fraction space (0..1) and lerped into doc-pixel space
// using the shape's bbox at render time. Solid/None/CurrentColor fall back
// to the bbox-less path; bbox is ignored in those cases.
void Canvas::apply_fill(const Cairo::RefPtr<Cairo::Context> &cr,
                        const FillStyle &fill,
                        const BBox &bbox) {
  if (!fill.is_gradient()) {
    apply_fill(cr, fill);
    return;
  }
  if (fill.stops.empty()) {
    // No stops — nothing to render. Source set to fully transparent so a
    // subsequent fill() draws nothing rather than whatever was last set.
    cr->set_source_rgba(0, 0, 0, 0);
    return;
  }

  // Lerp endpoints from 0..1 fractions into doc coordinates.
  // (g_x1, g_y1) → (bbox.x + g_x1 * bbox.w, bbox.y + g_y1 * bbox.h).
  const double x1 = bbox.x + fill.g_x1 * bbox.w;
  const double y1 = bbox.y + fill.g_y1 * bbox.h;
  const double x2 = bbox.x + fill.g_x2 * bbox.w;
  const double y2 = bbox.y + fill.g_y2 * bbox.h;

  Cairo::RefPtr<Cairo::Gradient> pat;
  if (fill.type == FillStyle::Type::LinearGradient) {
    pat = Cairo::LinearGradient::create(x1, y1, x2, y2);
  } else {
    // Radial: focal at (g_x1,g_y1), centre at (g_x2,g_y2), radius is a
    // fraction of the bbox's larger dimension (objectBoundingBox semantics
    // for radial gradients in SVG use sqrt((w²+h²)/2) but the larger-dim
    // approximation is close enough for Stage 1 and matches user intuition).
    const double R = fill.g_r * std::max(bbox.w, bbox.h);
    pat = Cairo::RadialGradient::create(x1, y1, 0.0, x2, y2, R);
  }

  // Stops. Sort defensively in case the data wasn't pre-sorted.
  std::vector<GradientStop> sorted = fill.stops;
  std::sort(sorted.begin(), sorted.end(),
            [](const GradientStop &a, const GradientStop &b) {
              return a.offset < b.offset;
            });
  for (const auto &s : sorted) {
    pat->add_color_stop_rgba(s.offset, s.r, s.g, s.b, s.a);
  }

  cr->set_source(pat);
}

// ── Cache-aware apply_fill (S106 m1) ────────────────────────────────────────
//
// Routes gradient fills on a SceneNode through obj.gradient_cache. The
// cache is rasterised at doc resolution (cache surface dimensions =
// ceil(bbox.w) × ceil(bbox.h) pixels), filled with the gradient pattern,
// and reused on subsequent frames. Cairo bilinear-upscales at higher
// zooms.
//
// Coherence checks (any "yes" → rebuild):
//   1. gradient_cache_dirty — fill content changed (stops, type, colour).
//   2. gradient_cache surface is null — first paint of this node.
//   3. cached bbox doesn't match current bbox — geometry changed (path
//      mutation, scale). Move alone preserves the bbox shape; only the
//      caller's set_source x,y offset moves.
//
// Why doc-resolution caching: gradients are smooth fields. A gradient
// rasterised at doc-unit (zoom=1) resolution and bilinear-upscaled at
// zoom 4× looks essentially identical to one rasterised at zoom 4×.
// Per-zoom-level caching would invalidate on every zoom click and pay
// the rasterisation cost again — exactly the s105 failure mode. Doc-
// resolution caching survives every zoom change without rebuild.
//
// Surface size cap: extreme-aspect or large-bbox shapes could in
// principle demand huge surfaces. SCENE_NODE_GRADIENT_CACHE_MAX_PX caps
// the long axis at 4096 px (16 MB ARGB32 — 2× the budget we ever spend
// per node in practice). On overflow we bypass the cache and rasterise
// directly. Rare in icon-design workloads.
void Canvas::apply_fill(const Cairo::RefPtr<Cairo::Context> &cr,
                        const SceneNode &obj,
                        const BBox &bbox) {
  // Non-gradient fills don't benefit from caching — fall through.
  if (!obj.fill.is_gradient()) {
    apply_fill(cr, obj.fill, bbox);
    return;
  }
  if (obj.fill.stops.empty()) {
    cr->set_source_rgba(0, 0, 0, 0);
    return;
  }

  // Coherence check.
  //
  // Only the bbox SHAPE (w, h) matters for cache validity. The bbox
  // position (x, y) becomes the blit offset in the cr->set_source call
  // below — translating an object preserves the cached pixels and only
  // changes where they land. Comparing position would invalidate on
  // every drag frame (the s106 m1 fix1 bug, caught via instrumentation).
  constexpr double EPS = 1e-6;
  const bool bbox_match =
      obj.gradient_cache_bbox_w > 0.0 &&
      std::abs(obj.gradient_cache_bbox_w - bbox.w) < EPS &&
      std::abs(obj.gradient_cache_bbox_h - bbox.h) < EPS;

  const bool needs_rebuild =
      obj.gradient_cache_dirty || !obj.gradient_cache || !bbox_match;

  if (needs_rebuild) {
    // Allocate / reallocate the offscreen surface at doc resolution.
    constexpr int CACHE_MAX_PX = 4096;
    const int surf_w = std::max(1, (int)std::ceil(bbox.w));
    const int surf_h = std::max(1, (int)std::ceil(bbox.h));
    if (surf_w > CACHE_MAX_PX || surf_h > CACHE_MAX_PX) {
      // Surface too large for the cache budget. Bypass to direct render
      // — slower per frame for this object, but bounded memory.
      LOG_INFO("Gradient cache bypass: surface {}x{} px exceeds cap {} "
               "(direct-render fallback)",
               surf_w, surf_h, CACHE_MAX_PX);
      apply_fill(cr, obj.fill, bbox);
      return;
    }

    auto surf = Cairo::ImageSurface::create(
        Cairo::Surface::Format::ARGB32, surf_w, surf_h);
    // Cairo returns an "error surface" rather than throwing on alloc
    // failure — read back width to verify.
    if (!surf || surf->get_width() != surf_w) {
      LOG_WARN("Gradient cache surface allocation failed for {}x{} px "
               "(direct-render fallback)",
               surf_w, surf_h);
      apply_fill(cr, obj.fill, bbox);
      return;
    }

    // Build the gradient pattern in the surface's coordinate frame.
    // The surface is sized to the bbox in doc units; the pattern's
    // endpoints lerp from objectBoundingBox-fractions into surface-
    // pixel coordinates.
    const double x1 = obj.fill.g_x1 * bbox.w;
    const double y1 = obj.fill.g_y1 * bbox.h;
    const double x2 = obj.fill.g_x2 * bbox.w;
    const double y2 = obj.fill.g_y2 * bbox.h;

    Cairo::RefPtr<Cairo::Gradient> pat;
    if (obj.fill.type == FillStyle::Type::LinearGradient) {
      pat = Cairo::LinearGradient::create(x1, y1, x2, y2);
    } else {
      const double R = obj.fill.g_r * std::max(bbox.w, bbox.h);
      pat = Cairo::RadialGradient::create(x1, y1, 0.0, x2, y2, R);
    }
    std::vector<GradientStop> sorted = obj.fill.stops;
    std::sort(sorted.begin(), sorted.end(),
              [](const GradientStop &a, const GradientStop &b) {
                return a.offset < b.offset;
              });
    for (const auto &s : sorted) {
      pat->add_color_stop_rgba(s.offset, s.r, s.g, s.b, s.a);
    }

    // Paint the gradient onto the entire surface.
    auto sc = Cairo::Context::create(surf);
    sc->set_source(pat);
    sc->paint();

    // Commit to the node.
    obj.gradient_cache = surf;
    obj.gradient_cache_dirty = false;
    obj.gradient_cache_zoom = 1.0;  // we cache at doc resolution
    obj.gradient_cache_bbox_x = bbox.x;
    obj.gradient_cache_bbox_y = bbox.y;
    obj.gradient_cache_bbox_w = bbox.w;
    obj.gradient_cache_bbox_h = bbox.h;
  }

  // Set the cached surface as the fill source. The surface is in doc
  // units; placing it at (bbox.x, bbox.y) in the active doc-space
  // transform makes a subsequent cr->fill() against the path sample
  // the right pixels. Cairo applies the active transform's scale to
  // the source automatically (i.e. at zoom 4× the surface is sampled
  // 4× larger via bilinear upsampling).
  cr->set_source(obj.gradient_cache, bbox.x, bbox.y);
}

void Canvas::apply_stroke_style(const Cairo::RefPtr<Cairo::Context> &cr,
                                const StrokeStyle &stroke) {
  apply_fill(cr, stroke.paint);
  // draw_objects scales by m_zoom before calling us, so stroke.width is already
  // in doc units
  cr->set_line_width(stroke.width);
  switch (stroke.cap) {
  case LineCap::Butt:
    cr->set_line_cap(Cairo::Context::LineCap::BUTT);
    break;
  case LineCap::Round:
    cr->set_line_cap(Cairo::Context::LineCap::ROUND);
    break;
  case LineCap::Square:
    cr->set_line_cap(Cairo::Context::LineCap::SQUARE);
    break;
  }
  switch (stroke.join) {
  case LineJoin::Miter:
    cr->set_line_join(Cairo::Context::LineJoin::MITER);
    break;
  case LineJoin::Round:
    cr->set_line_join(Cairo::Context::LineJoin::ROUND);
    break;
  case LineJoin::Bevel:
    cr->set_line_join(Cairo::Context::LineJoin::BEVEL);
    break;
  }
}

// ── render_shadow_under (S97 m2) ─────────────────────────────────────────
// Paints a tinted, blurred, offset shadow of `host_pat` onto `cr`.
// Called from draw_object's end-of-wrap when obj.shadow_enabled is true.
//
// Pipeline (mirrors the SVG <filter> chain emitted by SvgWriter):
//   1. Compute host doc-space bbox; pad it by max(blur, |dx|, |dy|) so the
//      shadow has room to land without clipping at the offscreen surface
//      edges. Convert to device pixels via the cr's current CTM (which
//      includes draw_objects' scale(m_zoom)).
//   2. Allocate an ImageSurface of those pixel dimensions, transformed so
//      the host's doc-space coordinates land at the correct pixel inside.
//   3. Paint host_pat into the ImageSurface — gets us the host's silhouette
//      and colour at the right pixel positions.
//   4. Three-pass box blur (curvz::utils::box_blur_argb32) approximating
//      Gaussian. Radius in pixels = round(blur_doc * m_zoom).
//   5. On the canvas cr: translate by (dx, dy) in doc-space, set source to
//      the shadow colour, mask through the blurred surface's alpha. This
//      stamps the shadow tint wherever the blurred host had alpha — the
//      blurred surface's RGB is discarded, only its alpha contributes.
//
// Caller paints the unblurred host on top after this returns.
//
// No-op when bbox is unavailable or surface dimensions degenerate. The
// host still renders correctly (caller's set_source(host_pat); paint());
// the shadow just goes missing rather than crashing.
void Canvas::render_shadow_under(const Cairo::RefPtr<Cairo::Context> &cr,
                                 const SceneNode &obj,
                                 const Cairo::RefPtr<Cairo::Pattern> &host_pat) {
  if (!host_pat) return;

  // ── 1. Doc-space bbox + padding ────────────────────────────────────
  // include_stroke=true: shadow follows the stroked silhouette, not just
  // the fill geometry. Bail if bbox is unavailable (degenerate node, no
  // path geometry, etc.). Without a bbox we'd have nowhere to allocate
  // the offscreen surface.
  auto bb = object_bbox(obj, /*include_stroke=*/true);
  if (!bb) return;

  // Pad in doc units. Three sources of padding combine:
  //   * blur radius — the kernel reach of the box-blur passes
  //   * |shadow_dx|, |shadow_dy| — the offset can push shadow off-bbox
  //   * a small safety constant to absorb minor object_bbox under-estimates
  //     and to give the blur smooth ramps at the edges
  const double blur_doc = std::max(0.0, obj.shadow_blur);
  const double pad_doc =
      std::ceil(blur_doc * 2.0)            // 2σ for visual completeness
      + std::abs(obj.shadow_dx)
      + std::abs(obj.shadow_dy)
      + 4.0;                               // safety constant

  double doc_x0 = bb->x       - pad_doc;
  double doc_y0 = bb->y       - pad_doc;
  double doc_x1 = bb->x + bb->w + pad_doc;
  double doc_y1 = bb->y + bb->h + pad_doc;

  // ── 2. Doc rect → device pixel rect ────────────────────────────────
  // cr's CTM is doc → device; user_to_device walks both corners.
  // After this conversion, dev_x0/y0 and dev_x1/y1 are pixel coordinates
  // on the canvas's backing surface.
  double dx0 = doc_x0, dy0 = doc_y0;
  double dx1 = doc_x1, dy1 = doc_y1;
  cr->user_to_device(dx0, dy0);
  cr->user_to_device(dx1, dy1);
  // CTM scale should be positive (we never flip), but defend against
  // arbitrary CTM by min/max-ing.
  if (dx1 < dx0) std::swap(dx0, dx1);
  if (dy1 < dy0) std::swap(dy0, dy1);

  // Snap to pixel grid (outward). Sub-pixel surface offsets cause sample
  // shimmer when the canvas zooms or pans across the host.
  int pix_x = (int)std::floor(dx0);
  int pix_y = (int)std::floor(dy0);
  int pix_w = (int)std::ceil(dx1) - pix_x;
  int pix_h = (int)std::ceil(dy1) - pix_y;
  if (pix_w <= 0 || pix_h <= 0) return;

  // ── Viewport intersect (S107 m1) ────────────────────────────────────
  //
  // Without this clip the offscreen surface scales as host_bbox × zoom².
  // A canvas-sized shadowed object at zoom 12 demands a ~16000² ARGB32
  // buffer (~1 GB) plus host-pattern paint into every pixel plus a
  // box-blur over the whole thing. That was the 12s-frame freeze
  // observed during S107 diagnosis (`us_render_shadow` will confirm).
  //
  // The shadow is only visually meaningful where its blur footprint
  // intersects the visible widget. Off-viewport host bbox plus blur
  // ramp can still bleed into the visible region, so we expand the
  // viewport rect by the same padding the bbox already used (blur
  // reach + shadow offset + safety) before intersecting. A shadowed
  // object whose entire (bbox + blur) sits off-screen produces no
  // visible pixels and we early-out.
  //
  // The intersection rect's origin (pix_x, pix_y) still describes
  // where the offscreen surface sits on the destination device — the
  // matrix-correct paint code below (`m.x0 -= pix_x`) and the final
  // `cr->mask(surf, pix_x + off_dx, pix_y + off_dy)` both consume the
  // post-intersect values correctly because both interpret pix_x/y as
  // "device pixel of surface(0,0)", which is what the intersection
  // rect's origin is.
  {
    const int radius_px_pre = (int)std::round(blur_doc * m_zoom);
    const int pad_px = radius_px_pre * 2
                     + (int)std::ceil(std::abs(obj.shadow_dx) * m_zoom)
                     + (int)std::ceil(std::abs(obj.shadow_dy) * m_zoom)
                     + 4;
    const int vp_x0 = -pad_px;
    const int vp_y0 = -pad_px;
    const int vp_x1 = get_width()  + pad_px;
    const int vp_y1 = get_height() + pad_px;

    const int host_x0 = pix_x;
    const int host_y0 = pix_y;
    const int host_x1 = pix_x + pix_w;
    const int host_y1 = pix_y + pix_h;

    const int new_x0 = std::max(host_x0, vp_x0);
    const int new_y0 = std::max(host_y0, vp_y0);
    const int new_x1 = std::min(host_x1, vp_x1);
    const int new_y1 = std::min(host_y1, vp_y1);

    if (new_x1 <= new_x0 || new_y1 <= new_y0) return;  // off-viewport

    pix_x = new_x0;
    pix_y = new_y0;
    pix_w = new_x1 - new_x0;
    pix_h = new_y1 - new_y0;
  }

  // ── 3. Offscreen ImageSurface, paint host into it ──────────────────
  // ARGB32 = Cairo's premultiplied 32-bit format, the only format on
  // which our box_blur_argb32 operates.
  auto surf = Cairo::ImageSurface::create(
      Cairo::Surface::Format::ARGB32, pix_w, pix_h);
  if (!surf) return;
  auto sc = Cairo::Context::create(surf);
  // ── Critical: matrix-correct paint of host_pat into the offscreen surf.
  //
  // host_pat is a Cairo::SurfacePattern returned by pop_group_to_source.
  // Its internal pattern_matrix was set by Cairo so that, used as a source
  // on the ORIGINAL cr (CTM unchanged from push time), painting reproduces
  // the captured image at its original device-pixel location.
  //
  // To paint host_pat correctly into `sc`, sc's CTM must reproduce the
  // CTM the original cr had at push_group time, with one extra step: a
  // device-space translation by (-pix_x, -pix_y) so that the offscreen
  // surface's pixel (0,0) corresponds to canvas device pixel (pix_x, pix_y).
  //
  // The original cr's CTM hasn't been touched between push_group (in
  // begin_alpha) and now (inside end_alpha → render_shadow_under), so
  // cr->get_matrix() returns exactly that CTM. We grab it, prepend a
  // device-space translation by subtracting from x0/y0 (the device-space
  // origin column of the affine), and apply to sc. From sc's perspective,
  // user-space coordinates now map to its surface's pixel grid in lockstep
  // with the original canvas-pixel grid for that doc-space region.
  //
  // Wrong attempt (initial m2 ship): sc->translate(-pix_x, -pix_y) on the
  // default identity matrix. That only worked if host_pat's source pixels
  // were addressed in canvas device coords — but the pattern matrix maps
  // destination user-space → source pixels, and sc's user-space was
  // identity-doc-pixels, not canvas-device-pixels. Result: paint produced
  // either empty pixels or a tiny smear at the wrong spot.
  Cairo::Matrix m = cr->get_matrix();
  m.x0 -= pix_x;
  m.y0 -= pix_y;
  sc->set_matrix(m);
  sc->set_source(host_pat);
  sc->paint();
  // Drop the cairomm Context before reading bytes (it may have pending
  // ops in its private state). flush() forces them out.
  sc.reset();
  surf->flush();

  // ── 4. Blur in place ───────────────────────────────────────────────
  // Convert blur radius doc → pixels via current zoom. radius=0 skips
  // the blur entirely (curvz::utils handles that as a no-op). For very
  // small radii (<1 px) the result is visually unchanged from the
  // unblurred copy; still cheap, no special-case needed.
  const int radius_px = (int)std::round(blur_doc * m_zoom);
  if (radius_px > 0) {
    curvz::utils::box_blur_argb32(
        surf->get_data(), surf->get_stride(), pix_w, pix_h, radius_px);
    surf->mark_dirty();
  }

  // ── 5. Composite onto cr: shadow colour masked by blurred alpha ────
  // The mask call wants its (x,y) interpreted in CURRENT user space, then
  // multiplied through the CTM to landing pixels. Simplest: switch to a
  // device-space identity CTM, compute the shadow-offset delta in device
  // pixels, do the mask there, then restore. This keeps the math local
  // and avoids juggling user vs device for each component.
  //
  // Shadow offset in device pixels: take (0,0) and (dx,dy) doc-space and
  // run them both through user_to_device; the difference is the shadow
  // delta in pixels. Same for the surface origin (pix_x, pix_y) — those
  // are already device pixels by construction.
  double off_a_x = 0.0, off_a_y = 0.0;
  double off_b_x = obj.shadow_dx, off_b_y = obj.shadow_dy;
  cr->user_to_device(off_a_x, off_a_y);
  cr->user_to_device(off_b_x, off_b_y);
  const double off_dx = off_b_x - off_a_x;
  const double off_dy = off_b_y - off_a_y;

  // Final shadow alpha = colour.a × shadow_opacity (matches SvgWriter's
  // pre-multiplication into flood-opacity). This dials the whole shadow
  // up/down independent of its hue.
  const double final_a =
      std::max(0.0, std::min(1.0, obj.shadow_color_a * obj.shadow_opacity));

  cr->save();
  // Identity CTM — units are now device pixels.
  cr->set_identity_matrix();
  cr->set_source_rgba(obj.shadow_color_r,
                      obj.shadow_color_g,
                      obj.shadow_color_b,
                      final_a);
  // mask(surface, x, y): paints the current source through the surface's
  // alpha mask, with the surface positioned at (x, y) in current user
  // space. We're at identity now, so x, y are device pixels — exactly the
  // shadow's placement on the canvas backing surface.
  cr->mask(surf, pix_x + off_dx, pix_y + off_dy);
  cr->restore();
}

void Canvas::draw_objects(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_doc)
    return;

  // Layer z-order matches the LayersPanel row order:
  //   - LayersPanel iterates rows from `layers[n-1]` down to `layers[0]`
  //     and appends top-to-bottom, so `layers[n-1]` appears at the TOP
  //     of the panel and `layers[0]` at the BOTTOM.
  //   - Cairo paints in call order (last-painted wins), so to match the
  //     panel we must paint `layers[0]` FIRST (bottom of z-order) and
  //     `layers[n-1]` LAST (top of z-order).
  // This means dragging a guide / margin / grid layer to the bottom of
  // the panel actually puts it behind art.
  for (int li = 0; li < (int)m_doc->layers.size(); ++li) {
    const auto &layer = m_doc->layers[li];
    if (!layer->visible)
      continue;

    if (layer->is_guide_layer()) {
      draw_guides_doc_space(cr, layer.get());
      continue;
    }

    if (layer->is_grid_layer()) {
      draw_grid_doc_space(cr, layer.get());
      continue;
    }

    if (layer->is_margin_layer()) {
      draw_margin_doc_space(cr, layer.get());
      continue;
    }

    double rr, rg, rb;
    if (layer->is_ref_layer()) {
      rr = 0.85;
      rg = 0.1;
      rb = 0.75;
      parse_layer_color(layer->color, rr, rg, rb);
    } else {
      // S99: pick a default layer stroke V (HSV value) that contrasts
      // with the artboard background. Pre-S99 was hardcoded 0.88 grey
      // on a dark default artboard — vanishes when the user picks a
      // light artboard via the S98 m3 picker. Especially noticeable in
      // outline / node mode where the only thing on screen IS the stroke.
      //
      // Layers carry an auto-assigned palette colour (LayersPanel mints
      // one on creation) so multiple visible layers stay distinguishable
      // by hue. We preserve hue + saturation and only swap the V axis
      // — so "Layer A is blue, Layer B is green" still reads at a glance,
      // but both stay readable against any artboard.
      //
      // HSV (not HSL) was chosen so bright strokes stay vivid: HSL L→1
      // washes any colour toward white, HSV V→1 keeps the hue at full
      // brightness. The "bright line" cases really are intended to be
      // bright-blue / bright-green, not pale.
      //
      // V curve — single direction flip at bg V 0.60.
      //
      //   bg V 0.0..0.60   → bright line, gentle endpoints.
      //                          bg 0.0  → V 0.40 (gentle bright on black)
      //                          bg 0.60 → V 0.85 (max bright, just before flip)
      //   bg V 0.60..1.0   → dark line, gentle endpoints.
      //                          bg 0.60 → V 0.20 (max dark, just after flip)
      //                          bg 1.0  → V 0.60 (gentle dark on white)
      //
      // Discontinuity at 0.60 is intentional — that's the direction
      // flip. A continuous curve through it would always pass through
      // bg's own V somewhere and vanish there. Bg threshold uses HSV V
      // (max channel) to match the user's intuition about brightness,
      // not perceptual luminance.
      // s148 m1: read artboard bg from doc (re-promoted from project
      // per-motif). Each doc owns its tone; the V-flip threshold below
      // adapts to whichever colour the author chose for THIS doc.
      double ab_r = m_doc->artboard_bg_r(doc_motif());
      double ab_g = m_doc->artboard_bg_g(doc_motif());
      double ab_b = m_doc->artboard_bg_b(doc_motif());
      double bg_v = std::max({ab_r, ab_g, ab_b});
      double target_v;
      if (bg_v < 0.60) {
        target_v = 0.40 + (bg_v / 0.60) * 0.45;
      } else {
        target_v = 0.20 + ((bg_v - 0.60) / 0.40) * 0.40;
      }

      // Source: layer's palette colour (or 0.88 grey if missing/malformed).
      double src_r = 0.88, src_g = 0.88, src_b = 0.88;
      parse_layer_color(layer->color, src_r, src_g, src_b);

      // RGB → HSV. Standard formulas; we extract H + S, then substitute
      // target_v and convert back. Achromatic source (s=0) just paints
      // a grey of the target V — desaturated layer colours stay grey.
      double mx = std::max({src_r, src_g, src_b});
      double mn = std::min({src_r, src_g, src_b});
      double d  = mx - mn;
      double h  = 0.0;
      double s  = (mx == 0.0) ? 0.0 : d / mx;
      if (d != 0.0) {
        if      (mx == src_r) h = (src_g - src_b) / d + (src_g < src_b ? 6 : 0);
        else if (mx == src_g) h = (src_b - src_r) / d + 2;
        else                  h = (src_r - src_g) / d + 4;
        h /= 6.0;
      }

      // HSV → RGB at (h, s, target_v).
      if (s == 0.0) {
        rr = rg = rb = target_v;  // achromatic
      } else {
        double hh = h * 6.0;
        int    i  = (int)std::floor(hh) % 6;
        if (i < 0) i += 6;
        double f  = hh - std::floor(hh);
        double p  = target_v * (1.0 - s);
        double q  = target_v * (1.0 - s * f);
        double t  = target_v * (1.0 - s * (1.0 - f));
        switch (i) {
          case 0: rr = target_v; rg = t;        rb = p;        break;
          case 1: rr = q;        rg = target_v; rb = p;        break;
          case 2: rr = p;        rg = target_v; rb = t;        break;
          case 3: rr = p;        rg = q;        rb = target_v; break;
          case 4: rr = t;        rg = p;        rb = target_v; break;
          default:rr = target_v; rg = p;        rb = q;        break;
        }
      }

      LOG_INFO("outline_contrast: layer='{}' bg_v={:.3f} target_v={:.3f} "
               "src_rgb=({:.2f},{:.2f},{:.2f}) hsv=(h={:.2f} s={:.2f}) "
               "out_rgb=({:.2f},{:.2f},{:.2f})",
               layer->name, bg_v, target_v, src_r, src_g, src_b,
               h, s, rr, rg, rb);
    }
    cr->save();
    cr->scale(m_zoom, m_zoom);
    for (int oi = (int)layer->children.size() - 1; oi >= 0; --oi)
      draw_object(cr, *layer->children[oi], rr, rg, rb);
    cr->restore();
  }
}

void Canvas::draw_object(const Cairo::RefPtr<Cairo::Context> &cr,
                         const SceneNode &obj, double layer_r, double layer_g,
                         double layer_b) {
  cr->save();

  // ── Per-object opacity + drop shadow (S97 m2) ─────────────────────────
  // When opacity < 1.0, push a cairo group so the object's fill+stroke
  // (and any internal compositing) render as a single isolated layer,
  // then pop and paint the whole thing with alpha. This matches SVG's
  // object-level `opacity` semantics (as opposed to fill-opacity /
  // stroke-opacity which are per-paint). Groups don't paint pixels of
  // their own; their children are each recursed into draw_object and
  // handle their own opacity — which is correct: parent * child opacity
  // compounds multiplicatively, matching SVG.
  //
  // Drop shadow piggy-backs on the same wrap pair (S97 m2). When the host
  // has shadow_enabled, end_alpha runs the offscreen-blur composite under
  // the host BEFORE painting the host on top — see render_shadow_under
  // for the full pipeline. Both effects can apply: shadow first, then
  // opacity wraps the combined shadow+host as a single alpha pass,
  // matching SVG (filter is applied before opacity in the rendering
  // pipeline).
  //
  // Ref points and degenerate (non-path) objects skip this.
  const bool wants_alpha  = (obj.opacity < 0.999);
  // Outline mode renders fills as 1-px strokes for editing — a shadow of
  // a hollow outline is visually surprising and not what Illustrator/
  // Affinity show in their equivalent modes. Skip the shadow pass when
  // outline mode is on; the host still renders normally.
  const bool wants_shadow = obj.shadow_enabled && !m_outline_mode
                            && (obj.shadow_color_a > 0.0)
                            && (obj.shadow_opacity > 0.0);
  bool alpha_pushed = false;
  auto begin_alpha = [&]() {
    if (wants_alpha || wants_shadow) {
      cr->push_group();
      alpha_pushed = true;
    }
  };
  auto end_alpha = [&]() {
    if (!alpha_pushed) return;
    // Pop the host's render off the group stack. From here we hold a
    // pattern (Cairo::SurfacePattern wrapping a recording/backed surface)
    // until we paint it back to cr.
    cr->pop_group_to_source();
    Cairo::RefPtr<Cairo::Pattern> host_pat = cr->get_source();

    if (wants_shadow) {
      render_shadow_under(cr, obj, host_pat);
    }

    // Paint the original host on top, with opacity if requested.
    cr->set_source(host_pat);
    if (wants_alpha) cr->paint_with_alpha(obj.opacity);
    else             cr->paint();

    alpha_pushed = false;
  };

  // ── Group: recurse into children independently ────────────────────────
  if (obj.type == SceneNode::Type::Group) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    // Wrap the recursion so group opacity composites the group as a unit
    // (overlapping children alpha'd once, not additively). Each child's
    // own opacity still applies inside the group — SVG semantics.
    begin_alpha();
    for (int i = (int)obj.children.size() - 1; i >= 0; --i)
      draw_object(cr, *obj.children[i], layer_r, layer_g, layer_b);
    end_alpha();
    cr->restore();
    return;
  }

  // ── TextBox: a typed container that renders a text frame as one
  //   user-visible atom. Owns its parts as `children` with a fixed
  //   role-to-index mapping:
  //
  //     children[0]  text node — what the user reads, drawn over
  //                  the boundary.
  //     children[1]  boundary path — the frame, fill/stroke like any
  //                  other Path. Default style is transparent, but
  //                  the user can set fill or stroke and they render
  //                  through the boundary's normal Path branch.
  //
  //   Render order is bottom-up: boundary first (so its fill/stroke
  //   land underneath), then text on top. We don't iterate via the
  //   generic children loop because the text child needs to lay out
  //   against its sibling boundary — calling draw_object on the text
  //   directly would route through draw_text_node's legacy
  //   text_boundary_ids lookup, which is empty for TextBox-owned text
  //   (the link is structural now, not field-based). draw_text_in_-
  //   boundary takes the boundary by reference and does the right
  //   layout — same function the legacy paired-sibling path uses,
  //   just called with the boundary in hand rather than looked up by
  //   iid.
  //
  //   The TextBox has no paint of its own — it's a container. Opacity
  //   composites the whole textbox as a unit (begin_alpha/end_alpha
  //   wrap the two paints), so children's individual opacities don't
  //   compound against the parent's — matching Group / ClipGroup /
  //   Blend / Warp SVG semantics.
  //
  //   Falls back gracefully if the container is malformed: missing
  //   text or boundary child → render nothing for that role rather
  //   than crash. A reader of the live code can see the invariant
  //   in the size/type checks.
  // s310 m1bc — TextBoxMgr render. The Mgr is the text-bearing node
  // (carries text_content / font defaults / fill / stroke / caret).
  // Walk children: each Canvas TextBoxView holds a Path child as its
  // boundary; paint that boundary, then paint text laid out against
  // it. Popover views have no Path child and don't render in m1
  // (m2 will wire the Gtk popover widget).
  //
  // Falls back gracefully if the Mgr is malformed: a view with no
  // valid boundary skips silently rather than crashing. A reader of
  // the live code can see the invariant in the size/type checks.
  if (obj.is_text_box_mgr()) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    begin_alpha();
    // s317 — Flow resolver (render-time). The shared buffer pours through
    //   the members in child order: member 0 lays out from byte 0, consumes
    //   what fits, and hands its end offset to member 1, and so on. Each
    //   member renders only its slice (draw_text_in_boundary takes a
    //   byte_start and returns the absolute bytes_consumed, which we chain).
    //   This replaces the s316 "first view only" stub — text now visibly
    //   flows from a full box into the next linked box. The `!` overflow
    //   indicator is gated to the TAIL member (the last canvas view): only
    //   the tail can have text that still has nowhere to go.
    //
    //   Paint is const — it does NOT write the view_byte_start /
    //   view_bytes_consumed caches (those belong to a recompute_views seam,
    //   not the render path). The chained local offset is all the render
    //   needs; the slice boundaries are re-derived here every paint.
    //
    //   First find the tail (last canvas view) so we can flag it.
    const SceneNode* tail_view = nullptr;
    for (const auto& vp : obj.children)
      if (vp && vp->is_canvas_view()) tail_view = vp.get();

    // s317 — collect each member's rect + boundary as we flow, so the
    //   post-pass can draw the unadorned member outlines and the flow
    //   connectors (arrowed lines proving 0->N order) without recomputing.
    struct MemberRect { double x, y, w, h; const SceneNode* boundary; };
    std::vector<MemberRect> member_rects;

    // s318 — Active-edit pad bookkeeping. If this Mgr is the one being
    //   edited, remember which member holds the caret and where its text
    //   slice begins, so a post-loop pass can mask its interior and redraw
    //   its text cleanly on top of any overlapping members.
    const bool editing_this_mgr =
        (m_text_editing == &obj) && (m_text_boundary_editing != nullptr);
    const SceneNode* edit_boundary = nullptr;
    size_t edit_byte_start = 0;
    bool   edit_is_tail = false;

    size_t flow_offset = 0;
    for (const auto& view_ptr : obj.children) {
      if (!view_ptr) continue;
      const SceneNode& view = *view_ptr;
      if (!view.is_canvas_view()) continue;
      if (!view.visible) continue;
      if (view.children.empty() || !view.children[0]) continue;
      const SceneNode& boundary = *view.children[0];
      if (boundary.type != SceneNode::Type::Path) continue;
      // Boundary first (underneath). Recurses via draw_object so the
      // boundary's fill/stroke/opacity/transform all apply via the
      // normal Path branch.
      draw_object(cr, boundary, layer_r, layer_g, layer_b);
      // Member rect (bbox of the boundary path) for the viz post-pass.
      if (boundary.path && boundary.path->nodes.size() >= 2) {
        const auto& bn = boundary.path->nodes;
        double rx0 = bn[0].x, ry0 = bn[0].y, rx1 = rx0, ry1 = ry0;
        for (const auto& pn : bn) {
          if (pn.x < rx0) rx0 = pn.x; if (pn.x > rx1) rx1 = pn.x;
          if (pn.y < ry0) ry0 = pn.y; if (pn.y > ry1) ry1 = pn.y;
        }
        member_rects.push_back({rx0, ry0, rx1 - rx0, ry1 - ry0, &boundary});
      }
      // Text slice for this member, starting where the previous member
      // left off. Indicator only on the tail.
      const bool is_tail = (&view == tail_view);
      // s318 — record the editing member's slice start before drawing it,
      //   so the post-loop pad can redraw exactly this member's text.
      if (editing_this_mgr && &boundary == m_text_boundary_editing) {
        edit_boundary   = &boundary;
        edit_byte_start = flow_offset;
        edit_is_tail    = is_tail;
      }
      flow_offset =
          draw_text_in_boundary(cr, obj, boundary, flow_offset, is_tail);
    }

    // s318 — Active-edit pad. In edit mode, mask the editing member's
    //   interior with a 65%-opaque white rect inset to its text margins,
    //   then re-draw that member's text on top. Stacked no-fill boxes show
    //   each other's text through on the canvas; this gives the box being
    //   edited a clean field. It is purely an edit-mode overlay — nothing
    //   persists, so the moment edit ends the boxes revert to their plain
    //   (possibly overlapping) appearance, per the design note. Drawn AFTER
    //   the flow loop so it sits above every other member regardless of
    //   child order.
    if (editing_this_mgr && edit_boundary && edit_boundary->path &&
        edit_boundary->path->nodes.size() >= 2) {
      const auto& bn = edit_boundary->path->nodes;
      double px0 = bn[0].x, py0 = bn[0].y, px1 = px0, py1 = py0;
      for (const auto& pn : bn) {
        if (pn.x < px0) px0 = pn.x; if (pn.x > px1) px1 = pn.x;
        if (pn.y < py0) py0 = pn.y; if (pn.y > py1) py1 = pn.y;
      }
      const double ml = edit_boundary->text_margin_left;
      const double mr = edit_boundary->text_margin_right;
      const double mt = edit_boundary->text_margin_top;
      const double mb = edit_boundary->text_margin_bottom;
      const double rx = px0 + ml, ry = py0 + mt;
      const double rw = (px1 - px0) - (ml + mr);
      const double rh = (py1 - py0) - (mt + mb);
      if (rw > 0.0 && rh > 0.0) {
        // s318 — Pad color is derived from the artboard background (so it
        //   tracks motif + the user's artboard color) but shifted AWAY from
        //   it so the panel is actually visible: painting the bg colour over
        //   the bg is a no-op. On a dark artboard we lighten (an "elevated
        //   card"); on a light artboard we darken. The panel lands between
        //   the text colour and the background, so text stays legible while
        //   other boxes' bleed-through dims under the 65% fill. `delta` is
        //   the visibility knob; the 0.65 alpha is the masking-strength knob.
        double br = 0.157, bg = 0.157, bb = 0.157;   // fallback ~dark artboard
        if (m_doc) {
          br = m_doc->artboard_bg_r(doc_motif());
          bg = m_doc->artboard_bg_g(doc_motif());
          bb = m_doc->artboard_bg_b(doc_motif());
        }
        const double luma = 0.299 * br + 0.587 * bg + 0.114 * bb;
        const double delta = 0.18;
        const double s = (luma > 0.5) ? -delta : +delta;  // light bg darkens
        const double pad_r = std::clamp(br + s, 0.0, 1.0);
        const double pad_g = std::clamp(bg + s, 0.0, 1.0);
        const double pad_b = std::clamp(bb + s, 0.0, 1.0);
        cr->save();
        cr->set_source_rgba(pad_r, pad_g, pad_b, 0.65);
        cr->rectangle(rx, ry, rw, rh);
        cr->fill();
        cr->restore();
        draw_text_in_boundary(cr, obj, *edit_boundary, edit_byte_start,
                              edit_is_tail);
      }
    }

    // s317 — is there any text past the last member? (Used to gate the
    //   overflow area: an empty overflow draws nothing.)
    const bool has_overflow_text = (flow_offset < obj.text_content.size());

    // s314 m1 — Canvas-region depot (sighting shot). Paint the depot
    //   region anchored at (textbox.right, textbox.bottom) using the
    //   overlay_w / overlay_h stored on the Popover view. Just a
    //   stroked rectangle for the prototype — no text inside yet.
    //   When the layout reads right and the grip drag feels natural,
    //   we tear out the widget-based depot machinery and wire the
    //   text-flow rendering into this region.
    //
    //   The mask corners (UR above depot, LL below textbox) paint
    //   nothing — they're literally just absent from the depot paint
    //   call, so the canvas background shows through.
    auto L = compute_mgr_overlay_layout(&obj);
    const bool mgr_is_selected = (m_selected == &obj);

    // s326 — Gate the structure-proof viz (container rect, flow connectors,
    //   member outlines, margin guides) to selection. Unselected, a TextBoxMgr
    //   reads as plain text on the page; selecting the Mgr (or any member box)
    //   reveals the ownership structure. The overflow `!` indicator and the
    //   user-toggled overflow bubble are NOT gated here — they are functional
    //   affordances, not structural chrome. A member is "selected" when it is
    //   the active single selection or part of the multi-selection set.
    bool mgr_or_member_selected = mgr_is_selected || (m_text_editing == &obj);
    if (!mgr_or_member_selected) {
      for (const auto& mr : member_rects) {
        if (mr.boundary == m_selected ||
            std::find(m_selection.begin(), m_selection.end(), mr.boundary) !=
                m_selection.end()) {
          mgr_or_member_selected = true;
          break;
        }
      }
    }

    // s317 — Container rect: visible proof of ownership. When a Mgr has
    //   more than one member, draw a thin frame around the union of the
    //   member boxes so the user can SEE the boxes are one structure (the
    //   mock's container_bbox). It is a DERIVED visualization, never a hit
    //   target (member_hit_test only claims member rects) — the enclosing
    //   rect is the picture of the structure, not a UI object. A single-
    //   member Mgr draws nothing here: the lone box outline already is the
    //   whole structure, so a second frame would just be noise.
    if (mgr_or_member_selected && L.valid && L.member_count >= 2) {
      const double iz = 1.0 / std::max(m_zoom, 0.001);
      // s317 — the container frame proves ownership of everything the Mgr
      //   owns: the member boxes AND the overflow panel (when presented).
      //   Start from the member union and extend over the panel's actual
      //   drawn rect (it's shifted left of the depot anchor for the
      //   vertical pointer, so union the bubble body, not L.bbox).
      double ux0 = L.members_x, uy0 = L.members_y;
      double ux1 = L.members_x + L.members_w, uy1 = L.members_y + L.members_h;
      const bool ov_shown = (m_overflow_shown_iid == obj.internal_id) &&
                            has_overflow_text && L.has_overlay;
      if (ov_shown) {
        OverflowBubbleLayout ob = compute_overflow_bubble_layout(L);
        if (ob.valid) {
          ux0 = std::min(ux0, ob.body_x);
          uy0 = std::min(uy0, ob.body_y);
          ux1 = std::max(ux1, ob.body_x + ob.body_w);
          uy1 = std::max(uy1, ob.body_y + ob.body_h);
        }
      }
      cr->save();
      cr->rectangle(ux0, uy0, ux1 - ux0, uy1 - uy0);
      cr->set_source_rgba(0.2, 0.4, 0.8, 0.7);
      cr->set_line_width(1.0 * iz);
      cr->set_dash(std::vector<double>{}, 0.0);
      cr->stroke();
      cr->restore();
    }

    // s317 — Per-member viz + flow connectors. Two parts, both only when
    //   there's more than one member (a single box is its own structure):
    //
    //   (a) Each member that is NOT the active selection draws an
    //       unadorned rect outline, so every box reads as a box. The
    //       ACTIVE member is skipped here — its 8 resizers + dashed
    //       selection rect (draw_selection_handles) already mark it, and
    //       a second outline would just double up.
    //
    //   (b) A flow connector between each consecutive pair: a dashed line
    //       from member i's bottom-right to member i+1's top-left with a
    //       midpoint arrowhead pointing in flow direction (0 -> N). This
    //       is the picture of the text route — port of the mock's flow
    //       connectors. Drawn UNDER the member rects so the boxes sit on
    //       top of the threads.
    if (mgr_or_member_selected && member_rects.size() >= 2) {
      const double iz = 1.0 / std::max(m_zoom, 0.001);
      const double hair = 1.0 * iz;

      auto is_selected_boundary = [&](const SceneNode* b) -> bool {
        if (m_selected == b) return true;
        return std::find(m_selection.begin(), m_selection.end(), b) !=
               m_selection.end();
      };

      // (b) Flow connectors first (underneath the rects).
      cr->save();
      cr->set_source_rgba(0.55, 0.3, 0.7, 0.85);
      const double arrow_doc = 9.0 * iz;
      std::vector<double> fdash = { 5.0 * iz, 4.0 * iz };
      for (std::size_t i = 0; i + 1 < member_rects.size(); ++i) {
        const MemberRect& a = member_rects[i];
        const MemberRect& b = member_rects[i + 1];
        const double ax = a.x + a.w, ay = a.y + a.h;   // i's BR
        const double bx = b.x,        by = b.y;        // i+1's TL

        cr->set_line_width(hair * 1.3);
        cr->set_dash(fdash, 0.0);
        cr->move_to(ax, ay);
        cr->line_to(bx, by);
        cr->stroke();
        cr->set_dash(std::vector<double>{}, 0.0);

        // Midpoint arrowhead, tip at the midpoint, barbs trailing back
        // along the travel direction (so it reads as "flowing toward i+1").
        const double mx = (ax + bx) * 0.5, my = (ay + by) * 0.5;
        const double ang = std::atan2(by - ay, bx - ax);
        const double spread = 0.5;  // radians half-spread
        cr->move_to(mx, my);
        cr->line_to(mx - arrow_doc * std::cos(ang - spread),
                    my - arrow_doc * std::sin(ang - spread));
        cr->move_to(mx, my);
        cr->line_to(mx - arrow_doc * std::cos(ang + spread),
                    my - arrow_doc * std::sin(ang + spread));
        cr->stroke();
      }
      cr->restore();

      // (a) Unadorned rect for every non-active member.
      cr->save();
      cr->set_source_rgba(0.55, 0.58, 0.62, 0.8);
      cr->set_line_width(hair);
      cr->set_dash(std::vector<double>{}, 0.0);
      for (const auto& mr : member_rects) {
        if (is_selected_boundary(mr.boundary)) continue;  // active gets handles
        cr->rectangle(mr.x, mr.y, mr.w, mr.h);
        cr->stroke();
      }
      cr->restore();

      // s317 — Margin guides: a dashed rect inset by each member's margins,
      //   so the user can see where text is bound. Drawn for every member
      //   (active included — the margins matter most on the box you're
      //   editing). Faint dashes so they read as a guide, not a border.
      cr->save();
      cr->set_source_rgba(0.5, 0.55, 0.6, 0.55);
      cr->set_line_width(hair);
      std::vector<double> mdash = { 3.0 * iz, 3.0 * iz };
      cr->set_dash(mdash, 0.0);
      for (const auto& mr : member_rects) {
        auto mg = effective_text_margins(&obj, mr.boundary);
        const double ilx = mr.x + mg.left;
        const double ity = mr.y + mg.top;
        const double iw  = mr.w - mg.left - mg.right;
        const double ih  = mr.h - mg.top  - mg.bottom;
        if (iw > 0.0 && ih > 0.0) {
          cr->rectangle(ilx, ity, iw, ih);
          cr->stroke();
        }
      }
      cr->set_dash(std::vector<double>{}, 0.0);
      cr->restore();
    }

    // s316 m3 — The custom overflow region is a popup-style affordance
    //   presented by the `!` button, not a permanent fixture. Draw it
    //   only when the user has toggled it on for THIS Mgr. The region's
    //   size persists on the popover view (overlay_w/overlay_h); its
    //   visibility is the transient m_overflow_shown_iid toggle.
    const bool overflow_presented = (m_overflow_shown_iid == obj.internal_id);
    // s317 — an overflow area with nothing spilling into it draws nothing,
    //   even if toggled on (e.g. a box was resized so the text now fits).
    if (overflow_presented && has_overflow_text && L.valid && L.has_overlay) {
      OverflowBubbleLayout B = compute_overflow_bubble_layout(L);
      if (B.valid) {
        const double iz = 1.0 / std::max(m_zoom, 0.001);
        const double hair = 1.0 * iz;

        // ── Bubble outline ────────────────────────────────────────────
        // Speech-bubble: three SOFT corners (TL, TR, BL) + one SHARP
        // corner (BR, where the grip lives) + a vertical point on the top
        // edge under the `!`. The soft edges read as "floating area"; the
        // sharp grip corner reads as "drag to resize." One closed path,
        // factored into a lambda so the drop shadow can reuse the exact
        // silhouette offset down-right.
        const double r = B.corner_r;
        const double x0 = B.body_x, y0 = B.body_y;
        const double x1 = B.body_x + B.body_w, y1 = B.body_y + B.body_h;
        auto build_bubble_path = [&](double ox, double oy) {
          cr->begin_new_path();
          cr->move_to(x0 + r + ox, y0 + oy);
          cr->line_to(B.point_base_l + ox, y0 + oy);
          cr->line_to(B.point_tip_x + ox, B.point_tip_y + oy);
          cr->line_to(B.point_base_r + ox, y0 + oy);
          cr->line_to(x1 - r + ox, y0 + oy);
          cr->arc(x1 - r + ox, y0 + r + oy, r, -M_PI / 2, 0);
          cr->line_to(x1 + ox, y1 + oy);
          cr->line_to(x0 + r + ox, y1 + oy);
          cr->arc(x0 + r + ox, y1 - r + oy, r, M_PI / 2, M_PI);
          cr->line_to(x0 + ox, y0 + r + oy);
          cr->arc(x0 + r + ox, y0 + r + oy, r, M_PI, 3 * M_PI / 2);
          cr->close_path();
        };

        // Drop shadow — the depth cue that makes the panel read as floating
        //   ABOVE the canvas. A floating surface casts a soft, offset
        //   shadow: we approximate the blur cheaply by stacking a few
        //   copies of the silhouette, each offset further down-right with
        //   lower alpha, so the edge fades. (A true Gaussian via
        //   box_blur_argb32 exists for object shadows, but the stacked-
        //   silhouette trick is enough for a small popup and avoids an
        //   offscreen surface per paint.)
        cr->save();
        const double sh_step = 2.0 * iz;
        for (int s = 4; s >= 1; --s) {
          build_bubble_path(sh_step * s, sh_step * s);
          cr->set_source_rgba(0.0, 0.0, 0.0, 0.10);
          cr->fill();
        }
        cr->restore();

        // s317 — Light/dark-aware panel. Sample the artboard luma to pick a
        //   surface that reads as a panel against the current motif: an
        //   "elevated dark" in dark mode, a near-white in light mode. The
        //   overflow text reuses the Mgr's own fill (already tuned for the
        //   motif on the canvas), so contrast carries over for free; chrome
        //   (info text, link, grip) flips to match.
        double panel_r, panel_g, panel_b, chrome_r, chrome_g, chrome_b;
        std::string chrome_hex;
        {
          const double abr = m_doc->artboard_bg_r(doc_motif());
          const double abg = m_doc->artboard_bg_g(doc_motif());
          const double abb = m_doc->artboard_bg_b(doc_motif());
          const double luma = 0.2126 * abr + 0.7152 * abg + 0.0722 * abb;
          const bool dark = (luma < 0.5);
          if (dark) {
            panel_r = 0.17; panel_g = 0.17; panel_b = 0.19;     // elevated dark
            chrome_r = 0.80; chrome_g = 0.83; chrome_b = 0.88;  // light chrome
            chrome_hex = "#ccd4e0";
          } else {
            panel_r = 0.96; panel_g = 0.96; panel_b = 0.97;     // light surface
            chrome_r = 0.22; chrome_g = 0.24; chrome_b = 0.28;  // dark chrome
            chrome_hex = "#383d47";
          }
        }

        // Body fill — NO border (s317). The shape reads as a panel from the
        //   fill + the outer drop shadow + the inner perimeter glow below.
        cr->save();
        build_bubble_path(0.0, 0.0);
        cr->set_source_rgba(panel_r, panel_g, panel_b, 0.98);
        cr->fill();
        cr->restore();

        // Inner perimeter glow — darken the inside edges so the panel reads
        //   as a recessed floating area without a hard outline. Clip to the
        //   silhouette, then stroke the path several times with a dark,
        //   semi-transparent ink at decreasing width: the outer half of each
        //   stroke is clipped away, leaving a soft dark band hugging the
        //   exact perimeter (rounded corners + point included).
        cr->save();
        build_bubble_path(0.0, 0.0);
        cr->clip();
        build_bubble_path(0.0, 0.0);
        cr->set_source_rgba(0.0, 0.0, 0.0, 0.18);
        cr->set_line_width(10.0 * iz);
        cr->stroke();
        build_bubble_path(0.0, 0.0);
        cr->set_source_rgba(0.0, 0.0, 0.0, 0.16);
        cr->set_line_width(5.0 * iz);
        cr->stroke();
        cr->restore();

        // ── Info bar — TEXT, not an outlined region: total word count and
        //    how many words spilled into the overflow. (s317 — the old
        //    outlined rects were just region markers; the real content is
        //    the text.)
        {
          auto count_words = [](const std::string& s, size_t from) -> int {
            int n = 0; bool in_word = false;
            for (size_t i = from; i < s.size(); ++i) {
              const char c = s[i];
              const bool ws = (c == ' ' || c == '\t' || c == '\n' ||
                               c == '\r' || c == '\f' || c == '\v');
              if (!ws && !in_word) { ++n; in_word = true; }
              else if (ws) in_word = false;
            }
            return n;
          };
          const int total_words = count_words(obj.text_content, 0);
          const int overflow_words = count_words(obj.text_content, flow_offset);
          const std::string info_str =
              std::to_string(total_words) + " words \xC2\xB7 " +
              std::to_string(overflow_words) + " in overflow";
          cr->save();
          // Clip to the info column so it can never run into the link button.
          cr->rectangle(B.info_x, B.info_y, B.info_w, B.info_h);
          cr->clip();
          cr->set_source_rgba(chrome_r, chrome_g, chrome_b, 0.9);
          cr->set_font_size(B.info_h * 0.5);   // smaller (s317)
          cr->move_to(B.info_x, B.info_y + B.info_h * 0.66);
          cr->show_text(info_str);
          cr->restore();
        }

        // ── Link button — the SOLE create-next-tb verb. s317: the
        //    curvz-link-to-symbolic.svg icon, recolored to chrome, NO
        //    border. Wired to link_textbox_member via hit-test.
        {
          ensure_link_glyph_pixbuf(chrome_hex);
          const double lx = B.link_x, ly = B.link_y;
          const double lw = B.link_w, lh = B.link_h;
          const double bcx = lx + lw * 0.5;
          const double bcy = ly + lh * 0.5;
          if (m_link_glyph_pixbuf) {
            const double pw = m_link_glyph_pixbuf->get_width();
            const double ph = m_link_glyph_pixbuf->get_height();
            const double fit = std::min(lw, lh) * 0.95;
            const double scale = fit / std::max(pw, ph);
            cr->save();
            cr->translate(bcx, bcy);
            cr->scale(scale, scale);
            curvz::utils::cairo_set_source_pixbuf(cr, m_link_glyph_pixbuf,
                                                  -pw * 0.5, -ph * 0.5);
            cr->paint();
            cr->restore();
          } else {
            // Fallback: a small chrome "+" if the icon failed to load.
            cr->save();
            cr->set_source_rgba(chrome_r, chrome_g, chrome_b, 0.9);
            cr->set_line_width(hair * 1.4);
            const double hg = std::min(lw, lh) * 0.25;
            cr->move_to(bcx - hg, bcy); cr->line_to(bcx + hg, bcy); cr->stroke();
            cr->move_to(bcx, bcy - hg); cr->line_to(bcx, bcy + hg); cr->stroke();
            cr->restore();
          }
        }

        // ── Text area: the overflow text, rendered here (s317; s319 scroll)
        //   Everything past the last member (byte_start = flow_offset) is
        //   poured into the panel. s319 — the region is laid out to its FULL
        //   remainder height (tall boundary, same as the cursor anchors to)
        //   and the panel WINDOWS it: clip to the visible text rect and
        //   translate by m_overflow_scroll_y, which follows the caret.
        if (B.text_w > 0.0 && B.text_h > 0.0) {
          const double pad_doc = 4.0 * iz;
          // s319 — content-sized region (matches sync_overflow_region_boundary)
          //   so layout doesn't flood with capacity baselines.
          const double region_h = overflow_region_height(
              &obj, B.text_x, B.text_y, B.text_w, B.text_h, pad_doc, flow_offset);
          SceneNode area_boundary;
          area_boundary.type = SceneNode::Type::Path;
          area_boundary.path = std::make_unique<PathData>(
              rect_to_path(B.text_x, B.text_y, B.text_w, region_h));
          area_boundary.text_margin_top    = pad_doc;
          area_boundary.text_margin_bottom = pad_doc;
          area_boundary.text_margin_left   = pad_doc;
          area_boundary.text_margin_right  = pad_doc;

          // s319 — scroll-follows-caret. When the caret is editing in THIS
          //   OA, derive the scroll so the caret line stays inside the
          //   visible window. Stored for the caret render (overlay pass) to
          //   window identically. Pure transient view state.
          const bool caret_here =
              (m_overflow_shown_iid == obj.internal_id) && caret_in_overflow();
          if (caret_here && m_text_cursor) {
            // s319 — Keep the cursor's OA region boundary (the popover's
            //   children[0]) sized to the CURRENT content: typing in the OA
            //   may have grown/shrunk the remainder since cross-in, and the
            //   caret can only reach lines its boundary lays out. The popover
            //   boundary is our own cursor scaffolding (not user geometry —
            //   object_bbox/hit-test exclude the popover), so resizing it to
            //   the freshly-computed region_h here keeps caret math valid
            //   without per-keystroke wiring. Done before position_on_canvas
            //   so the caret geometry reflects the resize.
            SceneNode& mobj = const_cast<SceneNode&>(obj);
            for (auto& c : mobj.children) {
              if (c && c->is_popover_view() && !c->children.empty() &&
                  c->children[0] && c->children[0]->is_path()) {
                c->children[0]->path = std::make_unique<PathData>(
                    rect_to_path(B.text_x, B.text_y, B.text_w, region_h));
                break;
              }
            }
            TextCursor::Geometry g = m_text_cursor->position_on_canvas();
            if (g.valid) {
              const double vis_top = B.text_y;
              const double vis_bot = B.text_y + B.text_h;
              double sy = m_overflow_scroll_y;
              if (g.y + g.height - sy > vis_bot) sy = g.y + g.height - vis_bot;
              if (g.y - sy < vis_top)            sy = g.y - vis_top;
              if (sy < 0.0) sy = 0.0;
              m_overflow_scroll_y = sy;
            }
          } else {
            m_overflow_scroll_y = 0.0;   // not editing here → pinned at top
          }

          cr->save();
          cr->rectangle(B.text_x, B.text_y, B.text_w, B.text_h);
          cr->clip();
          cr->translate(0.0, -m_overflow_scroll_y);
          draw_text_in_boundary(cr, obj, area_boundary, flow_offset,
                                /*draw_overflow_indicator=*/false);
          cr->restore();

          // ── Scroll thumb (s319) — sized to window/content and positioned
          //    by the scroll offset. panel_end is now the full remainder
          //    (tall boundary), so overflow is detected from content HEIGHT
          //    vs the visible window, not a byte count.
          TextLayout tl_full =
              compute_text_layout(&area_boundary, &obj, flow_offset);
          double content_h = 0.0;
          for (const auto& b : tl_full.baselines)
            if (b.byte_end > b.byte_start)        // s319 — skip capacity empties
              content_h = (b.y + b.descent) - B.text_y + pad_doc;
          const double window_h = B.text_h;
          if (content_h > window_h + 0.5) {
            const double max_scroll = content_h - window_h;
            double frac = window_h / content_h;
            if (frac < 0.08) frac = 0.08;
            if (frac > 1.0)  frac = 1.0;
            const double track_h = B.sbar_h;
            const double thumb_h = track_h * frac;
            const double thumb_w = B.sbar_w * 0.6;
            const double thumb_x = B.sbar_x + (B.sbar_w - thumb_w) * 0.5;
            double pos_frac =
                (max_scroll > 0.0) ? (m_overflow_scroll_y / max_scroll) : 0.0;
            if (pos_frac < 0.0) pos_frac = 0.0;
            if (pos_frac > 1.0) pos_frac = 1.0;
            const double thumb_y = B.sbar_y + (track_h - thumb_h) * pos_frac;
            cr->save();
            cr->set_source_rgba(chrome_r, chrome_g, chrome_b, 0.55);
            const double rr = thumb_w * 0.5;
            cr->begin_new_path();
            cr->arc(thumb_x + rr, thumb_y + rr, rr, M_PI, 1.5 * M_PI);
            cr->arc(thumb_x + thumb_w - rr, thumb_y + rr, rr, 1.5 * M_PI, 2 * M_PI);
            cr->arc(thumb_x + thumb_w - rr, thumb_y + thumb_h - rr, rr, 0, 0.5 * M_PI);
            cr->arc(thumb_x + rr, thumb_y + thumb_h - rr, rr, 0.5 * M_PI, M_PI);
            cr->close_path();
            cr->fill();
            cr->restore();
          }
        }

        // ── Scrollbar strip: omitted (no region lines — s317). Scroll
        //    wiring comes when the overflow exceeds the panel height.

        // ── Grip: hatch marks in the sharp BR corner ──────────────────
        cr->save();
        cr->set_source_rgba(chrome_r, chrome_g, chrome_b, 0.85);
        cr->set_line_width(hair * 1.2);
        for (int k = 1; k <= 3; ++k) {
          const double off = (B.grip_w) * (k / 4.0);
          cr->move_to(x1 - off, y1);
          cr->line_to(x1, y1 - off);
          cr->stroke();
        }
        cr->restore();

        // s314 m1 — when the Mgr is selected, dashed-outline the tail tb
        //   so tb + bubble read as one object.
        if (mgr_is_selected) {
          cr->save();
          cr->rectangle(L.textbox_x, L.textbox_y, L.textbox_w, L.textbox_h);
          cr->set_source_rgba(0.35, 0.35, 0.35, 0.5);
          cr->set_line_width(hair);
          std::vector<double> dashes_tb = { 4.0 * iz, 3.0 * iz };
          cr->set_dash(dashes_tb, 0.0);
          cr->stroke();
          cr->restore();
        }
      }
    }
    // s316 m3 — The old "materialize a depot by dragging a grip at the tb
    //   BR" affordance (s314 else-branch) is removed: the `!` button now
    //   owns the tb bottom-right and is the way the overflow region is
    //   summoned. No grip is drawn when the region is hidden.

    end_alpha();
    cr->restore();
    return;
  }

  // ── ClipGroup: children drawn inside a Cairo clip region ──────────────
  // Normal mode:
  //   - Build a Cairo path from clip_shape's geometry (Path or Compound).
  //   - cr->clip() intersects the current clip region with that path.
  //     We're already inside a cr->save() so cr->restore() at end of the
  //     function undoes the clip. No fill/stroke is drawn for the shape.
  //   - Recurse children top-down (size-1 → 0) same as Group.
  //
  // Outline mode:
  //   - Skip the clip entirely so the user can see children outside the
  //     clipped region for editing. Stroke the clip_shape in layer colour
  //     so it's visible/editable on canvas.
  //
  // Compound clip shape: emit all subpaths before calling clip().
  // Cairo's default fill rule (WINDING) vs SVG clip-path's default
  // (NONZERO, same thing) — match by leaving the rule alone. EVEN_ODD
  // would be needed only if a Compound was authored with even/odd intent;
  // a future refinement.
  if (obj.type == SceneNode::Type::ClipGroup) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    begin_alpha();

    if (m_outline_mode) {
      // Children first (in outline) — no clip applied.
      for (int i = (int)obj.children.size() - 1; i >= 0; --i)
        draw_object(cr, *obj.children[i], layer_r, layer_g, layer_b);

      // Then the clip shape on top as a thin stroke.
      if (obj.clip_shape) {
        const SceneNode &cs = *obj.clip_shape;
        if (cs.type == SceneNode::Type::Path && cs.path) {
          BezierPath bp = BezierPath::from_path_data(*cs.path);
          bp.apply_to_cairo(cr);
        } else if (cs.type == SceneNode::Type::Compound) {
          for (int i = (int)cs.children.size() - 1; i >= 0; --i) {
            const SceneNode &sub = *cs.children[i];
            if (sub.type != SceneNode::Type::Path || !sub.path)
              continue;
            BezierPath bp = BezierPath::from_path_data(*sub.path);
            bp.apply_to_cairo(cr);
          }
        }
        cr->set_source_rgb(layer_r, layer_g, layer_b);
        cr->set_line_width(1.0 / m_zoom);
        cr->stroke();
      }
    } else {
      // Normal mode — apply the clip, then recurse.
      //
      // Fill-rule note: Cairo defaults to WINDING. A Compound clip shape
      // with nested subpaths (e.g. donut = outer ring + inner ring) uses
      // EVEN_ODD to produce the hole — matches how the Compound itself
      // is filled in draw_object. Without this, overlapping subpaths
      // union (no hole), so the clip honors only the outermost ring.
      // cr->save() at the top of this function bounds the fill_rule
      // change to this object's recursion — cr->restore() resets it.
      if (obj.clip_shape) {
        const SceneNode &cs = *obj.clip_shape;
        if (cs.type == SceneNode::Type::Path && cs.path) {
          BezierPath bp = BezierPath::from_path_data(*cs.path);
          bp.apply_to_cairo(cr);
          cr->clip();
        } else if (cs.type == SceneNode::Type::Compound) {
          for (int i = (int)cs.children.size() - 1; i >= 0; --i) {
            const SceneNode &sub = *cs.children[i];
            if (sub.type != SceneNode::Type::Path || !sub.path)
              continue;
            BezierPath bp = BezierPath::from_path_data(*sub.path);
            bp.apply_to_cairo(cr);
          }
          cr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);
          cr->clip();
          cr->set_fill_rule(Cairo::Context::FillRule::WINDING);
        }
      }
      // If clip_shape is missing/degenerate, nothing is clipped —
      // children render normally. Matches "no clip defined yet" state.
      for (int i = (int)obj.children.size() - 1; i >= 0; --i)
        draw_object(cr, *obj.children[i], layer_r, layer_g, layer_b);
    }

    end_alpha();
    cr->restore();
    return;
  }

  // ── Blend: render A, generated intermediates, B ───────────────────────
  // Blend is a container, not a drawable. We recurse into its three
  // logical children in painter order:
  //   1. blend_source_a  (bottom)
  //   2. blend_cache[0..N-1]  (middle — regenerated on dirty)
  //   3. blend_source_b  (top)
  // Each step / source is itself a Path SceneNode, so we re-enter
  // draw_object for each one and inherit the normal Path rendering
  // path (fill, stroke, opacity, transform). The Blend's own opacity
  // composites the whole stack as a unit via begin_alpha/end_alpha —
  // same semantics as Group.
  // ── Blend: render A, generated intermediates, B ───────────────────────
  // Blend is a container, not a drawable. We recurse into its three
  // logical children in painter order:
  //   1. blend_source_a  (bottom)
  //   2. blend_cache[0..N-1]  (middle — regenerated on dirty)
  //   3. blend_source_b  (top)
  // Each step / source is itself a Path SceneNode, so we re-enter
  // draw_object for each one and inherit the normal Path rendering
  // path (fill, stroke, opacity, transform). The Blend's own opacity
  // composites the whole stack as a unit via begin_alpha/end_alpha —
  // same semantics as Group.
  if (obj.type == SceneNode::Type::Blend) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    // Cache rebuild is lazy-on-read. draw_object takes obj by const
    // reference — the constness refers to scene-tree structure, not
    // the derived cache. Matches Cairo's "immediate draw" model: by
    // the time we start painting the Blend, A/B are settled and the
    // cache is guaranteed fresh for this frame.
    SceneNode *mobj = const_cast<SceneNode *>(&obj);
    if (mobj->blend_cache_dirty ||
        (int)mobj->blend_cache.size() != std::clamp(mobj->blend_steps, 1, 50)) {
      rebuild_blend_cache(mobj);
    }

    begin_alpha();
    if (obj.blend_source_a)
      draw_object(cr, *obj.blend_source_a, layer_r, layer_g, layer_b);
    for (auto &step : obj.blend_cache)
      draw_object(cr, *step, layer_r, layer_g, layer_b);
    if (obj.blend_source_b)
      draw_object(cr, *obj.blend_source_b, layer_r, layer_g, layer_b);
    end_alpha();
    cr->restore();
    return;
  }

  // ── Warp ─────────────────────────────────────────────────────────────
  // Lazy cache rebuild on draw — same pattern as Blend. If either dirty
  // flag is set OR the cache is missing (e.g. just-parsed M1-stub file
  // that didn't carry envelope data), rebuild_warp_caches brings both
  // stages in sync. draw_object takes obj by const reference; the
  // constness refers to scene-tree structure, not the derived caches,
  // so const_cast here matches the Blend precedent.
  //
  // Render priority after rebuild: warp_cache if present (normal path),
  // else warp_glyph_cache (rebuilder cleared warp_cache — means no
  // paths in source), else warp_source (rebuilder bailed entirely,
  // degenerate fallback). M1 used to always fall through to source;
  // M2 has real math, so warp_cache is the happy path.
  if (obj.type == SceneNode::Type::Warp) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    SceneNode *mobj = const_cast<SceneNode *>(&obj);
    if (mobj->warp_glyph_cache_dirty || mobj->warp_cache_dirty ||
        !mobj->warp_cache) {
      const_cast<Canvas *>(this)->rebuild_warp_caches(mobj);
    }
    begin_alpha();
    if (obj.warp_cache)
      draw_object(cr, *obj.warp_cache, layer_r, layer_g, layer_b);
    else if (obj.warp_glyph_cache)
      draw_object(cr, *obj.warp_glyph_cache, layer_r, layer_g, layer_b);
    else if (obj.warp_source)
      draw_object(cr, *obj.warp_source, layer_r, layer_g, layer_b);
    end_alpha();
    cr->restore();
    return;
  }

  // ── Compound: all children paths fed to Cairo together, EVEN_ODD fill ─
  if (obj.type == SceneNode::Type::Compound) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    begin_alpha();
    if (m_outline_mode) {
      // Outline mode — stroke each child in layer colour
      for (int i = (int)obj.children.size() - 1; i >= 0; --i) {
        const SceneNode &child = *obj.children[i];
        if (child.type != SceneNode::Type::Path || !child.path)
          continue;
        BezierPath bp = BezierPath::from_path_data(*child.path);
        bp.apply_to_cairo(cr);
        cr->set_source_rgb(layer_r, layer_g, layer_b);
        cr->set_line_width(1.0 / m_zoom);
        cr->stroke();
      }
    } else {
      // Build combined path from all children (bottom-up = last child first)
      for (int i = (int)obj.children.size() - 1; i >= 0; --i) {
        const SceneNode &child = *obj.children[i];
        if (child.type != SceneNode::Type::Path || !child.path)
          continue;
        BezierPath bp = BezierPath::from_path_data(*child.path);
        bp.apply_to_cairo(cr);
      }
      // S58g: Fill with EVEN_ODD using the COMPOUND's own fill style. A
      // Compound is one visual object (S58d rule) — its own fill is the
      // canonical source of truth; child fills are inert. This matches
      // the print-mode Compound renderer and makes inspector edits to a
      // Compound's fill visibly take effect.
      if (obj.fill.type != FillStyle::Type::None) {
        cr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);
        if (obj.fill.is_gradient()) {
          if (auto bb = object_bbox(obj, false)) {
            apply_fill(cr, obj, *bb);  // S106 m1 — cache-aware
          } else {
            apply_fill(cr, obj.fill);
          }
        } else {
          apply_fill(cr, obj.fill);
        }
        cr->fill();
        cr->set_fill_rule(Cairo::Context::FillRule::WINDING);
      } else {
        cr->begin_new_path();
      }
      // S58g: Stroke using the COMPOUND's own stroke style, applied to each
      // child subpath. A Compound's stroke sets the outline style for the
      // whole shape including hole boundaries. If the Compound has no
      // stroke, no strokes are drawn.
      if (obj.stroke.paint.type != FillStyle::Type::None) {
        for (int i = (int)obj.children.size() - 1; i >= 0; --i) {
          const SceneNode &child = *obj.children[i];
          if (child.type != SceneNode::Type::Path || !child.path)
            continue;
          BezierPath bp = BezierPath::from_path_data(*child.path);
          bp.apply_to_cairo(cr);
          apply_stroke_style(cr, obj.stroke);
          cr->stroke();
        }
      }
    }
    end_alpha();
    cr->restore();
    return;
  }

  // ── Ref point: small crosshair in layer color, fixed screen size ─────
  if (obj.type == SceneNode::Type::Ref) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    double sx, sy;
    doc_to_screen(obj.ref_x, obj.ref_y, sx, sy);
    Cairo::Matrix saved;
    cr->get_matrix(saved);
    cr->set_identity_matrix();

    bool sel = (m_selected == &obj) ||
               std::any_of(m_selection.begin(), m_selection.end(),
                           [&obj](SceneNode *s) { return s == &obj; });
    bool hovered = (m_ref_hovered == &obj);

    if (sel) {
      // Selected: white crosshair with colored outline for contrast
      cr->set_source_rgba(1.0, 1.0, 1.0, 1.0);
    } else {
      cr->set_source_rgba(layer_r, layer_g, layer_b, hovered ? 1.0 : 0.75);
    }
    cr->set_line_width(sel ? 1.5 : 1.0);
    cr->set_dash(std::vector<double>{}, 0);

    constexpr double ARM = 6.0;
    constexpr double DOT = 2.0;
    cr->move_to(sx - ARM, sy);
    cr->line_to(sx + ARM, sy);
    cr->move_to(sx, sy - ARM);
    cr->line_to(sx, sy + ARM);
    cr->stroke();
    cr->arc(sx, sy, DOT, 0, 2 * M_PI);
    cr->fill();

    // Selected: draw colored ring around the dot for visibility
    if (sel) {
      cr->set_source_rgba(layer_r, layer_g, layer_b, 1.0);
      cr->set_line_width(1.0);
      cr->arc(sx, sy, ARM - 1.0, 0, 2 * M_PI);
      cr->stroke();
    }

    cr->set_matrix(saved);
    cr->restore();
    return;
  }

  // ── Image node ───────────────────────────────────────────────────────
  if (obj.type == SceneNode::Type::Image) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    begin_alpha();
    // draw_object is called with translate(ox,oy) + scale(zoom,zoom) active.
    // image_x/y/w/h are in doc units — used directly in this transformed space.
    double dx = obj.image_x; // doc x
    double dy = obj.image_y; // doc y (Y-down)
    double dw = obj.image_w; // doc width
    double dh = obj.image_h; // doc height
    if (dw < 0.01 || dh < 0.01) {
      cr->restore();
      return;
    }

    // ── Outline mode: draw bbox wireframe in lieu of pixel content ─────
    // Outline is a structural view — paths show as stroke skeletons,
    // text as bbox outlines, images likewise. Drawing the pixbuf in
    // outline defeats the abstraction (the user is looking AT the
    // structure, not the imagery). Apply the same 2x2 transform
    // (rotation/skew around image centre) we'd apply to the pixbuf so
    // the wireframe tracks rotated images correctly.
    //
    // Rect-plus-X is the universal "image placeholder" idiom (every
    // web browser uses it for broken-image fallbacks). The diagonals
    // distinguish an image bbox from an arbitrary rect path at a glance.
    if (m_outline_mode) {
      cr->save();
      const Transform &t = obj.transform;
      bool has_transform =
          (std::abs(t.a - 1.0) > 1e-6 || std::abs(t.b) > 1e-6 ||
           std::abs(t.c) > 1e-6 || std::abs(t.d - 1.0) > 1e-6);
      if (has_transform) {
        double icx = dx + dw * 0.5;
        double icy = dy + dh * 0.5;
        cr->translate(icx, icy);
        Cairo::Matrix m(t.a, t.b, t.c, t.d, 0, 0);
        cr->transform(m);
        cr->translate(-icx, -icy);
      }
      cr->set_source_rgb(layer_r, layer_g, layer_b);
      cr->set_line_width(1.0 / m_zoom);
      // Bbox rectangle.
      cr->rectangle(dx, dy, dw, dh);
      // X diagonals — image-placeholder idiom.
      cr->move_to(dx,      dy);
      cr->line_to(dx + dw, dy + dh);
      cr->move_to(dx + dw, dy);
      cr->line_to(dx,      dy + dh);
      cr->stroke();
      cr->restore();
      end_alpha();
      cr->restore();
      return;
    }

    Cairo::RefPtr<Cairo::ImageSurface> img_surf;
    try {
      auto ext = obj.image_path.substr(obj.image_path.rfind('.') + 1);
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == "png") {
        img_surf = Cairo::ImageSurface::create_from_png(obj.image_path);
      } else {
        auto pb = Gdk::Pixbuf::create_from_file(obj.image_path);
        if (pb) {
          int pw = pb->get_width(), ph = pb->get_height();
          auto surf2 = Cairo::ImageSurface::create(
              Cairo::Surface::Format::ARGB32, pw, ph);
          auto cr2 = Cairo::Context::create(surf2);
          // s135 m2: pumped — replaces deprecated gdk_cairo_set_source_pixbuf.
          curvz::utils::cairo_set_source_pixbuf(cr2, pb, 0, 0);
          cr2->paint();
          img_surf = surf2;
        }
      }
    } catch (...) {
    }

    if (img_surf) {
      int iw = img_surf->get_width();
      int ih = img_surf->get_height();
      if (iw > 0 && ih > 0) {
        cr->save();
        // Apply obj.transform (rotation/skew) around image centre.
        // transform is stored as a 2x2 matrix (a,b,c,d) with no translation —
        // we apply it centred on the image midpoint in doc space.
        double icx = dx + dw * 0.5;
        double icy = dy + dh * 0.5;
        const Transform &t = obj.transform;
        bool has_transform =
            (std::abs(t.a - 1.0) > 1e-6 || std::abs(t.b) > 1e-6 ||
             std::abs(t.c) > 1e-6 || std::abs(t.d - 1.0) > 1e-6);
        if (has_transform) {
          // Translate to image centre, apply 2x2 matrix, translate back
          cr->translate(icx, icy);
          Cairo::Matrix m(t.a, t.b, t.c, t.d, 0, 0);
          cr->transform(m);
          cr->translate(-icx, -icy);
        }
        cr->translate(dx, dy);
        cr->scale(dw / iw, dh / ih);
        cr->set_source(img_surf, 0, 0);
        // Opacity is applied by the outer begin_alpha/end_alpha wrapper
        cr->paint();
        cr->restore();
      }
    } else {
      // Placeholder — red X when image can't load
      cr->save();
      cr->set_source_rgba(0.8, 0.2, 0.2, 0.5);
      cr->rectangle(dx, dy, dw, dh);
      cr->fill();
      cr->set_source_rgba(0.8, 0.2, 0.2, 0.9);
      cr->set_line_width(1.0 / m_zoom);
      cr->move_to(dx, dy);
      cr->line_to(dx + dw, dy + dh);
      cr->move_to(dx + dw, dy);
      cr->line_to(dx, dy + dh);
      cr->stroke();
      cr->restore();
    }

    end_alpha();
    cr->restore();
    return;
  }

  // ── Text node ─────────────────────────────────────────────────────────
  if (obj.type == SceneNode::Type::Text) {
    if (!obj.visible) {
      cr->restore();
      return;
    }
    begin_alpha();
    // draw_text_node expects the doc-space transform (translate+scale) active.
    draw_text_node(cr, obj);
    end_alpha();
    cr->restore();
    return;
  }

  if (obj.type == SceneNode::Type::Path && obj.path) {
    begin_alpha();
    BezierPath bp = BezierPath::from_path_data(*obj.path);
    bp.apply_to_cairo(cr);
  } else {
    cr->restore();
    return;
  }

  if (m_outline_mode) {
    cr->set_source_rgb(layer_r, layer_g, layer_b);
    cr->set_line_width(1.0 / m_zoom);
    cr->stroke();
  } else {
    // If this path is a text-on-path guide, suppress all fill and stroke
    // in normal (preview) mode — it should be invisible.  Exception: when
    // selected, show it as a plain stroke so the user can see its geometry.
    if (is_top_guide_path(obj)) {
      bool selected = std::any_of(m_selection.begin(), m_selection.end(),
                                  [&obj](SceneNode *s) { return s == &obj; });
      if (selected) {
        cr->set_source_rgba(0.3, 0.6, 1.0, 0.7);
        cr->set_line_width(1.0 / m_zoom);
        cr->stroke();
      } else {
        cr->begin_new_path();
      }
      end_alpha();
      cr->restore();
      return;
    }
    if (obj.fill.type != FillStyle::Type::None) {
      if (obj.fill.is_gradient()) {
        if (auto bb = object_bbox(obj, false)) {
          apply_fill(cr, obj, *bb);  // S106 m1 — cache-aware
        } else {
          apply_fill(cr, obj.fill);
        }
      } else {
        apply_fill(cr, obj.fill);
      }
      cr->fill_preserve();
    }
    if (obj.stroke.paint.type != FillStyle::Type::None) {
      apply_stroke_style(cr, obj.stroke);
      cr->stroke();
    } else {
      cr->begin_new_path();
    }
  }

  end_alpha();
  cr->restore();
}

void Canvas::draw_rubber_band(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_drawing && !(m_tool == ActiveTool::Line && m_line_tool.active()))
    return;
  // Ref tool uses a coordinate overlay instead of a rubber-band rect
  if (m_tool == ActiveTool::Ref)
    return;
  // Corner tool draws its own rubber-band in draw_corner_tool_overlay
  if (m_tool == ActiveTool::Corner)
    return;

  double x1, y1, x2, y2;

  if (m_tool == ActiveTool::Zoom) {
    // Zoom marquee uses raw screen-space coords stored in anchor + draw_cur
    x1 = std::min(m_zoom_anchor_x, m_draw_cur_dx);
    y1 = std::min(m_zoom_anchor_y, m_draw_cur_dy);
    x2 = std::max(m_zoom_anchor_x, m_draw_cur_dx);
    y2 = std::max(m_zoom_anchor_y, m_draw_cur_dy);

    double w = x2 - x1, h = y2 - y1;
    if (w < 2 || h < 2)
      return;

    // Offset by doc origin (rubber band is drawn in screen/widget space)
    double ox = doc_origin_x(), oy = doc_origin_y();
    double rx = x1 + ox - ox; // screen coords already absolute
    // Actually draw_rubber_band is called inside on_draw where cr has no
    // zoom transform applied yet — coords are raw widget pixels. Good.
    // Colour: blue = zoom in, amber = zoom out
    double cr_r, cr_g, cr_b;
    if (m_mod_alt) {
      cr_r = 0.93;
      cr_g = 0.60;
      cr_b = 0.13;
    } // amber — zoom out
    else {
      cr_r = 0.21;
      cr_g = 0.52;
      cr_b = 0.89;
    } // blue  — zoom in

    cr->save();
    cr->set_source_rgba(cr_r, cr_g, cr_b, 0.10);
    cr->rectangle(x1, y1, w, h);
    cr->fill();
    cr->set_source_rgba(cr_r, cr_g, cr_b, 1.0);
    cr->set_line_width(1.0);
    std::vector<double> zoom_dash = {5.0, 3.0};
    cr->set_dash(zoom_dash, 0);
    cr->rectangle(x1, y1, w, h);
    cr->stroke();
    // Corner accent marks — like a real marquee zoom
    const double arm = 6.0;
    cr->set_dash(std::vector<double>{}, 0);
    cr->set_line_width(1.5);
    cr->set_source_rgba(cr_r, cr_g, cr_b, 1.0);
    // top-left
    cr->move_to(x1, y1 + arm);
    cr->line_to(x1, y1);
    cr->line_to(x1 + arm, y1);
    // top-right
    cr->move_to(x2 - arm, y1);
    cr->line_to(x2, y1);
    cr->line_to(x2, y1 + arm);
    // bottom-right
    cr->move_to(x2, y2 - arm);
    cr->line_to(x2, y2);
    cr->line_to(x2 - arm, y2);
    // bottom-left
    cr->move_to(x1 + arm, y2);
    cr->line_to(x1, y2);
    cr->line_to(x1, y2 - arm);
    cr->stroke();
    cr->restore();
    return;
  }

  // ── All other tools: doc-space coords → widget space ─────────────────
  // draw_rubber_band now runs outside the cr->translate(ox,oy) block,
  // so we must add the doc origin offset ourselves.

  // Line tool — multi-segment preview
  if (m_tool == ActiveTool::Line && m_line_tool.active()) {
    double ox = doc_origin_x(), oy = doc_origin_y();
    cr->save();
    // s148 m1: creation colour (per-doc, re-promoted from project per-motif).
    cr->set_source_rgba(m_doc->creation_color_r(doc_motif()),
                        m_doc->creation_color_g(doc_motif()),
                        m_doc->creation_color_b(doc_motif()), 0.9);
    cr->set_line_width(1.5);
    std::vector<double> dashes = {4.0, 3.0};
    cr->set_dash(dashes, 0);

    // Draw placed segments
    auto [p0x, p0y] = m_line_tool.points[0];
    cr->move_to(p0x * m_zoom + ox, p0y * m_zoom + oy);
    for (size_t i = 1; i < m_line_tool.points.size(); ++i) {
      auto [px, py] = m_line_tool.points[i];
      cr->line_to(px * m_zoom + ox, py * m_zoom + oy);
    }
    // Live rubber-band to cursor
    cr->line_to(m_line_tool.live_x * m_zoom + ox,
                m_line_tool.live_y * m_zoom + oy);
    cr->stroke();

    // Draw placed anchor points
    cr->set_dash(std::vector<double>{}, 0);
    cr->set_source_rgba(m_doc->creation_color_r(doc_motif()),
                        m_doc->creation_color_g(doc_motif()),
                        m_doc->creation_color_b(doc_motif()), 1.0);
    for (auto [px, py] : m_line_tool.points) {
      cr->arc(px * m_zoom + ox, py * m_zoom + oy, 3.0, 0, 2 * M_PI);
      cr->fill();
    }

    // Close-snap indicator: green ring around start point
    if (m_line_tool.close_snap && m_line_tool.points.size() >= 2) {
      cr->set_source_rgba(0.1, 0.85, 0.3, 0.9);
      cr->arc(p0x * m_zoom + ox, p0y * m_zoom + oy, 8.0 / m_zoom * m_zoom, 0,
              2 * M_PI);
      cr->stroke();
    }

    cr->restore();
    return;
  }

  // ── Polygon overlay ───────────────────────────────────────────────────
  if (m_tool == ActiveTool::Polygon && m_drawing) {
    double ox = doc_origin_x(), oy = doc_origin_y();
    double dw = m_draw_cur_dx - m_draw_start_dx;
    double dh = m_draw_cur_dy - m_draw_start_dy;
    double radius = std::hypot(dw, dh);
    if (radius < 1.0)
      return;

    double cx = m_draw_start_dx;
    double cy = m_draw_start_dy;

    int sides = m_poly_sides;
    double inflection = m_poly_inflection;

    // Snap inflection display
    double perfect_star =
        (sides >= 5) ? std::cos(2.0 * M_PI / sides) / std::cos(M_PI / sides)
                     : -1.0;
    if (perfect_star > 0.0 && std::abs(inflection - perfect_star) < 0.04)
      inflection = perfect_star;
    if (inflection > 0.985)
      inflection = 1.0;

    // Build path and draw in screen space
    PathData pd =
        polygon_to_path(cx, cy, radius, sides, inflection, m_poly_drag_angle);

    cr->save();
    // s148 m1: creation colour (per-doc) for both fill and outline.
    // Fill alpha 0.12 / outline alpha 0.9 are role-coded — fills always
    // dimmer than outlines at every construction site.
    // Fill
    cr->set_source_rgba(m_doc->creation_color_r(doc_motif()),
                        m_doc->creation_color_g(doc_motif()),
                        m_doc->creation_color_b(doc_motif()), 0.12);
    bool first = true;
    for (const auto &n : pd.nodes) {
      double sx = n.x * m_zoom + ox;
      double sy = n.y * m_zoom + oy;
      if (first) {
        cr->move_to(sx, sy);
        first = false;
      } else
        cr->line_to(sx, sy);
    }
    cr->close_path();
    cr->fill();

    // Outline
    cr->set_source_rgba(m_doc->creation_color_r(doc_motif()),
                        m_doc->creation_color_g(doc_motif()),
                        m_doc->creation_color_b(doc_motif()), 0.9);
    cr->set_line_width(1.0);
    std::vector<double> dashes = {4.0, 3.0};
    cr->set_dash(dashes, 0);
    first = true;
    for (const auto &n : pd.nodes) {
      double sx = n.x * m_zoom + ox;
      double sy = n.y * m_zoom + oy;
      if (first) {
        cr->move_to(sx, sy);
        first = false;
      } else
        cr->line_to(sx, sy);
    }
    cr->close_path();
    cr->stroke();
    cr->restore();
    return;
  }

  // ── Spiral overlay ────────────────────────────────────────────────────
  if (m_tool == ActiveTool::Spiral && m_drawing) {
    double ox = doc_origin_x(), oy = doc_origin_y();
    double dw = m_draw_cur_dx - m_draw_start_dx;
    double dh = m_draw_cur_dy - m_draw_start_dy;
    double outer_r = std::hypot(dw, dh);
    if (outer_r < 1.0)
      return;

    double cx = m_draw_start_dx;
    double cy = m_draw_start_dy;
    double inner_r = outer_r * (m_spiral_inner / 100.0);

    PathData pd = spiral_to_path(cx, cy, outer_r, inner_r,
                                 m_spiral_turns, m_spiral_drag_angle);

    cr->save();
    // s148 m1: creation colour (per-doc).
    cr->set_source_rgba(m_doc->creation_color_r(doc_motif()),
                        m_doc->creation_color_g(doc_motif()),
                        m_doc->creation_color_b(doc_motif()), 0.9);
    cr->set_line_width(1.0);
    std::vector<double> dashes = {4.0, 3.0};
    cr->set_dash(dashes, 0);

    bool first = true;
    for (int i = 0; i < (int)pd.nodes.size(); ++i) {
      const auto &n = pd.nodes[i];
      double sx = n.x * m_zoom + ox;
      double sy = n.y * m_zoom + oy;
      if (first) {
        cr->move_to(sx, sy);
        first = false;
      } else {
        // Use Bézier handles if available
        const auto &prev = pd.nodes[i - 1];
        double cx2s = prev.cx2 * m_zoom + ox;
        double cy2s = prev.cy2 * m_zoom + oy;
        double cx1s = n.cx1 * m_zoom + ox;
        double cy1s = n.cy1 * m_zoom + oy;
        cr->curve_to(cx2s, cy2s, cx1s, cy1s, sx, sy);
      }
    }
    cr->stroke();
    cr->restore();
    return;
  }

  {
    double ox = doc_origin_x(), oy = doc_origin_y();
    x1 = std::min(m_draw_start_effective_dx, m_draw_cur_dx) * m_zoom + ox;
    y1 = std::min(m_draw_start_effective_dy, m_draw_cur_dy) * m_zoom + oy;
    x2 = std::max(m_draw_start_effective_dx, m_draw_cur_dx) * m_zoom + ox;
    y2 = std::max(m_draw_start_effective_dy, m_draw_cur_dy) * m_zoom + oy;
  }
  double w = x2 - x1;
  double h = y2 - y1;
  if (w < 1 || h < 1)
    return;

  cr->save();

  // Fill preview — s148 m1: creation colour (per-doc), dim alpha for fill.
  cr->set_source_rgba(m_doc->creation_color_r(doc_motif()),
                      m_doc->creation_color_g(doc_motif()),
                      m_doc->creation_color_b(doc_motif()), 0.12);
  if (m_tool == ActiveTool::Ellipse) {
    cr->save();
    cr->translate(x1 + w * 0.5, y1 + h * 0.5);
    cr->scale(w * 0.5, h * 0.5);
    cr->arc(0, 0, 1.0, 0, 2 * M_PI);
    cr->restore();
  } else {
    cr->rectangle(x1, y1, w, h);
  }
  cr->fill();

  // Outline — s148 m1: creation colour (per-doc), bold alpha for outline.
  cr->set_source_rgba(m_doc->creation_color_r(doc_motif()),
                      m_doc->creation_color_g(doc_motif()),
                      m_doc->creation_color_b(doc_motif()), 0.9);
  cr->set_line_width(1.0);
  std::vector<double> dashes = {4.0, 3.0};
  cr->set_dash(dashes, 0);
  if (m_tool == ActiveTool::Ellipse) {
    cr->save();
    cr->translate(x1 + w * 0.5, y1 + h * 0.5);
    cr->scale(w * 0.5, h * 0.5);
    cr->arc(0, 0, 1.0, 0, 2 * M_PI);
    cr->restore();
  } else {
    cr->rectangle(x1, y1, w, h);
  }
  cr->stroke();

  cr->restore();
}

void Canvas::draw_marquee(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_marquee_active)
    return;

  double ox = doc_origin_x(), oy = doc_origin_y();
  double x1 = std::min(m_marquee_start_dx, m_marquee_cur_dx) * m_zoom + ox;
  double y1 = std::min(m_marquee_start_dy, m_marquee_cur_dy) * m_zoom + oy;
  double x2 = std::max(m_marquee_start_dx, m_marquee_cur_dx) * m_zoom + ox;
  double y2 = std::max(m_marquee_start_dy, m_marquee_cur_dy) * m_zoom + oy;
  double w = x2 - x1, h = y2 - y1;
  if (w < 1 || h < 1)
    return;

  cr->save();
  // Fill
  cr->set_source_rgba(0.3, 0.6, 1.0, 0.08);
  cr->rectangle(x1, y1, w, h);
  cr->fill();
  // Outline — dashed blue
  cr->set_source_rgba(0.3, 0.6, 1.0, 0.85);
  cr->set_line_width(1.0);
  std::vector<double> dashes = {4.0, 3.0};
  cr->set_dash(dashes, 0);
  cr->rectangle(x1, y1, w, h);
  cr->stroke();
  cr->restore();
}

void Canvas::draw_selection_handles(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (m_selection.empty())
    return;

  // During a rotate drag: draw the rotating BBX outline + a pivot crosshair.
  // The normal handle squares are suppressed — they'd dance around confusingly.
  bool is_rotating = (m_handle_drag == HandleKind::RotateNW ||
                      m_handle_drag == HandleKind::RotateNE ||
                      m_handle_drag == HandleKind::RotateSE ||
                      m_handle_drag == HandleKind::RotateSW);

  // Compute union BBX from live node positions (follows the rotating object)
  bool found = false;
  double sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;
  for (SceneNode *obj : m_selection) {
    auto bb = object_bbox(*obj);
    if (!bb)
      continue;
    double a1, b1, a2, b2;
    doc_to_screen(bb->x, bb->y, a1, b1);
    doc_to_screen(bb->x + bb->w, bb->y + bb->h, a2, b2);
    if (!found) {
      sx1 = a1;
      sy1 = b1;
      sx2 = a2;
      sy2 = b2;
      found = true;
    } else {
      sx1 = std::min(sx1, a1);
      sy1 = std::min(sy1, b1);
      sx2 = std::max(sx2, a2);
      sy2 = std::max(sy2, b2);
    }
  }
  if (!found)
    return;

  cr->save();

  // Selection rect — dashed blue
  cr->set_source_rgba(0.3, 0.6, 1.0, 0.8);
  cr->set_line_width(1.0);
  std::vector<double> dashes = {5.0, 3.0};
  cr->set_dash(dashes, 0);
  cr->rectangle(sx1, sy1, sx2 - sx1, sy2 - sy1);
  cr->stroke();
  cr->set_dash(std::vector<double>{}, 0);

  if (is_rotating) {
    // Draw the original start BBX as a ghost
    double gx1, gy1, gx2, gy2;
    doc_to_screen(m_handle_start_bb.x, m_handle_start_bb.y, gx1, gy1);
    doc_to_screen(m_handle_start_bb.x + m_handle_start_bb.w,
                  m_handle_start_bb.y + m_handle_start_bb.h, gx2, gy2);
    cr->set_source_rgba(1.0, 0.5, 0.0, 0.5);
    cr->set_line_width(1.5);
    std::vector<double> ghost_dash = {3.0, 3.0};
    cr->set_dash(ghost_dash, 0);
    cr->rectangle(gx1, gy1, gx2 - gx1, gy2 - gy1);
    cr->stroke();
    cr->set_dash(std::vector<double>{}, 0);

    // Pivot crosshair — bright red
    double pvx, pvy;
    doc_to_screen(m_handle_pivot_x, m_handle_pivot_y, pvx, pvy);
    const double arm = 14.0;
    cr->set_source_rgba(1.0, 0.1, 0.1, 1.0);
    cr->set_line_width(2.0);
    cr->move_to(pvx - arm, pvy);
    cr->line_to(pvx + arm, pvy);
    cr->stroke();
    cr->move_to(pvx, pvy - arm);
    cr->line_to(pvx, pvy + arm);
    cr->stroke();
    cr->arc(pvx, pvy, 5.0, 0, 2 * M_PI);
    cr->fill();
    cr->restore();
    return;
  }

  double hx[8], hy[8];
  selection_handle_positions(sx1, sy1, sx2, sy2, hx, hy);

  const double hs = 5.0; // handle square half-size

  // Draw all 8 handle positions — corners solid, edge mids slightly smaller
  for (int i = 0; i < 8; ++i) {
    bool is_corner = (i % 2 == 0);
    double sz = is_corner ? hs : hs * 0.85;
    cr->set_source_rgb(1.0, 1.0, 1.0);
    cr->rectangle(hx[i] - sz, hy[i] - sz, sz * 2, sz * 2);
    cr->fill();
    cr->set_source_rgba(0.3, 0.6, 1.0, 1.0);
    cr->set_line_width(1.0);
    cr->rectangle(hx[i] - sz, hy[i] - sz, sz * 2, sz * 2);
    cr->stroke();
  }

  // s259 — Weighted ("true") centre marker. Drawn whenever a selection
  // exists, no custom pivot is active, and no drag is in progress (the
  // marker would jitter mid-drag as the geometry mutates). The custom-
  // pivot crosshair below takes precedence when set.
  //
  // The marker indicates the no-wobble pivot — the centre of the
  // selection's minimum enclosing circle. For regular shapes (rect,
  // ellipse, regular polygon / star) it sits at the bbox centre. For
  // irregular shapes it sits at the visual middle, distinct from the
  // bbox centre.
  //
  // Style: small white dot (3 px radius) with a black halo (1 px outer
  // ring). Smaller and quieter than the orange custom-pivot crosshair —
  // a passive indicator, not an interaction target.
  const bool draw_sr_preview = m_sr_preview_active;
  const bool dragging        = m_moving || (m_handle_drag != HandleKind::None);
  if (!m_has_custom_pivot && !m_r_held && !draw_sr_preview && !dragging) {
    double tcx, tcy;
    if (selection_true_center(tcx, tcy)) {
      double tsx, tsy;
      doc_to_screen(tcx, tcy, tsx, tsy);
      const double dot_r = 3.0;
      const double halo  = 1.0;
      // Halo (black ring)
      cr->set_source_rgba(0.0, 0.0, 0.0, 0.65);
      cr->set_line_width(halo * 2.0);
      cr->arc(tsx, tsy, dot_r + halo * 0.5, 0, 2 * M_PI);
      cr->stroke();
      // Fill (white)
      cr->set_source_rgba(1.0, 1.0, 1.0, 1.0);
      cr->arc(tsx, tsy, dot_r, 0, 2 * M_PI);
      cr->fill();
    }
  }

  // Draw custom pivot point if set (or R held showing default), or if the
  // Step-and-Repeat dialog is showing its pivot preview.
  const bool draw_sr = m_sr_preview_active;
  if (m_has_custom_pivot || m_r_held || draw_sr) {
    double pvx, pvy;
    if (draw_sr)
      doc_to_screen(m_sr_preview_x, m_sr_preview_y, pvx, pvy);
    else
      doc_to_screen(m_custom_pivot_x, m_custom_pivot_y, pvx, pvy);

    const double arm = 10.0;
    const double radius = 4.0;

    // Shadow
    cr->set_source_rgba(0.0, 0.0, 0.0, 0.5);
    cr->set_line_width(3.0);
    cr->move_to(pvx - arm, pvy);
    cr->line_to(pvx + arm, pvy);
    cr->stroke();
    cr->move_to(pvx, pvy - arm);
    cr->line_to(pvx, pvy + arm);
    cr->stroke();

    // Bright orange crosshair (distinct from blue selection handles)
    cr->set_source_rgba(1.0, 0.55, 0.0, 1.0);
    cr->set_line_width(1.5);
    cr->move_to(pvx - arm, pvy);
    cr->line_to(pvx + arm, pvy);
    cr->stroke();
    cr->move_to(pvx, pvy - arm);
    cr->line_to(pvx, pvy + arm);
    cr->stroke();

    // Circle at centre
    cr->set_line_width(1.5);
    cr->arc(pvx, pvy, radius, 0, 2 * M_PI);
    cr->stroke();
  }

  cr->restore();
}

// ── draw_measurement_annotations
// ────────────────────────────────────────────
// Shared annotation render — given user-space (Y-up) endpoint coords,
// draws the triangle (hypotenuse + legs + right-angle box + endpoint
// dots) plus the seven structured labels (distance, Δx, Δy, α, β at
// each endpoint, plus tiny coord tags at A and B). Used by both the
// live ruler overlay and the persistent (saved) measurement overlay
// so they look identical.
//
// push_labels: when true, every label is appended to m_ruler_labels
// with copy_value = the structured block, enabling click-to-copy
// while in ruler mode.
void Canvas::draw_measurement_annotations(
    const Cairo::RefPtr<Cairo::Context> &cr,
    double ax_user, double ay_user, double bx_user, double by_user,
    bool push_labels) {
  if (!m_doc)
    return;

  // ── Convert user-space (Y-up) to screen coords ──────────────────────────
  double doc_ay = m_doc->canvas_height() - ay_user;
  double doc_by = m_doc->canvas_height() - by_user;
  double asx, asy, bsx, bsy;
  doc_to_screen(ax_user, doc_ay, asx, asy);
  doc_to_screen(bx_user, doc_by, bsx, bsy);

  // C is the right-angle corner: same X as B, same Y as A
  double csx = bsx, csy = asy;

  double ddx = std::abs(bx_user - ax_user);
  double ddy = std::abs(by_user - ay_user);
  double dist = std::hypot(bx_user - ax_user, by_user - ay_user);
  double angle_rad = std::atan2(by_user - ay_user, bx_user - ax_user);
  double angle_deg = angle_rad * 180.0 / M_PI;
  if (angle_deg < 0)
    angle_deg += 360.0;
  double alpha = std::atan2(ddy, ddx) * 180.0 / M_PI;
  double beta = 90.0 - alpha;

  Unit u = m_doc->canvas.display_unit;
  const char *ul = UnitSystem::label(u);
  auto fmt_val = [&](double v) -> std::string {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4g", UnitSystem::from_px(v, u));
    return buf;
  };
  auto fmt_lbl = [&](double v) -> std::string {
    return fmt_val(v) + std::string(" ") + ul;
  };
  auto fmt_deg_lbl = [](double v) -> std::string {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f\xc2\xb0", v);
    return buf;
  };

  // S89: the same structured block is the copy_value for every label.
  // Computed once here rather than per-label.
  std::string structured =
      format_structured_block_for(m_doc, ax_user, ay_user, bx_user, by_user);

  // ── Draw triangle lines ───────────────────────────────────────────────
  cr->save();

  // Hypotenuse A→B  (blue)
  cr->set_source_rgba(0.084, 0.325, 0.620, 1.0); // #15539e
  cr->set_line_width(1.5);
  std::vector<double> no_dash;
  cr->set_dash(no_dash, 0);
  cr->move_to(asx, asy);
  cr->line_to(bsx, bsy);
  cr->stroke();

  // Horizontal leg A→C and vertical leg C→B  (green dashed)
  std::vector<double> dash = {6.0, 3.0};
  cr->set_source_rgba(0.133, 0.773, 0.369, 1.0); // #22C55E
  cr->set_line_width(1.0);
  cr->set_dash(dash, 0);
  cr->move_to(asx, asy);
  cr->line_to(csx, csy); // horizontal
  cr->stroke();
  cr->move_to(csx, csy);
  cr->line_to(bsx, bsy); // vertical
  cr->stroke();
  cr->set_dash(no_dash, 0);

  // Right-angle box at C
  double box = 8.0;
  double bx_dir = (bsx >= asx) ? 1.0 : -1.0;
  double by_dir = (bsy >= asy) ? 1.0 : -1.0;
  double hpx = csx - bx_dir * box, hpy = csy;
  double vpx = csx, vpy = csy + by_dir * box;
  double ipx = csx - bx_dir * box, ipy = csy + by_dir * box;
  cr->set_source_rgba(0.133, 0.773, 0.369, 0.85);
  cr->set_line_width(1.0);
  cr->move_to(hpx, hpy);
  cr->line_to(ipx, ipy);
  cr->line_to(vpx, vpy);
  cr->stroke();

  // Endpoint dots
  cr->set_source_rgba(0.084, 0.325, 0.620, 1.0);
  cr->arc(asx, asy, 4.0, 0, 2 * M_PI);
  cr->fill();
  cr->arc(bsx, bsy, 4.0, 0, 2 * M_PI);
  cr->fill();

  cr->restore();

  // ── Draw labels with pill backgrounds ───────────────────────────────────
  auto draw_label = [&](const std::string &text, double tx, double ty,
                        double r_col, double g_col, double b_col) {
    cr->save();
    cr->select_font_face("Sans", Cairo::ToyFontFace::Slant::NORMAL,
                         Cairo::ToyFontFace::Weight::NORMAL);
    cr->set_font_size(11.0);
    Cairo::TextExtents te;
    cr->get_text_extents(text, te);
    double pw = te.width + 8.0;
    double ph = te.height + 6.0;
    double px = tx - pw * 0.5;
    double py = ty - ph * 0.5;

    cr->set_source_rgba(0.10, 0.10, 0.10, 0.82);
    double r = 4.0;
    cr->move_to(px + r, py);
    cr->line_to(px + pw - r, py);
    cr->arc(px + pw - r, py + r, r, -M_PI * 0.5, 0.0);
    cr->line_to(px + pw, py + ph - r);
    cr->arc(px + pw - r, py + ph - r, r, 0.0, M_PI * 0.5);
    cr->line_to(px + r, py + ph);
    cr->arc(px + r, py + ph - r, r, M_PI * 0.5, M_PI);
    cr->line_to(px, py + r);
    cr->arc(px + r, py + r, r, M_PI, M_PI * 1.5);
    cr->close_path();
    cr->fill();

    cr->set_source_rgb(r_col, g_col, b_col);
    cr->move_to(tx - te.width * 0.5 - te.x_bearing,
                ty - te.height * 0.5 - te.y_bearing);
    cr->show_text(text);
    cr->restore();

    if (push_labels) {
      RulerLabel lbl;
      lbl.copy_value = structured;
      lbl.display_text = text;
      lbl.sx = px;
      lbl.sy = py;
      lbl.sw = pw;
      lbl.sh = ph;
      m_ruler_labels.push_back(lbl);
    }
  };

  // Mid-points for label placement
  double hmid_x = (asx + csx) * 0.5;
  double vmid_y = (csy + bsy) * 0.5;
  double hyp_mx = (asx + bsx) * 0.5;
  double hyp_my = (asy + bsy) * 0.5;

  double by_dir2 = (bsy >= asy) ? 1.0 : -1.0;
  double bx_dir2 = (bsx >= asx) ? 1.0 : -1.0;

  // Hypotenuse — distance label
  {
    double dx_n = bsx - asx, dy_n = bsy - asy;
    double len = std::hypot(dx_n, dy_n);
    if (len > 0.01) {
      dx_n /= len;
      dy_n /= len;
    }
    double perp_x = -dy_n, perp_y = dx_n;
    double lx = hyp_mx + perp_x * 16.0;
    double ly = hyp_my + perp_y * 16.0;
    draw_label(fmt_lbl(dist), lx, ly, 0.4, 0.75, 1.0);
  }

  // Horizontal leg — Δx
  if (ddx > 0.5) {
    double lx = hmid_x;
    double ly = asy + by_dir2 * 16.0;
    draw_label(fmt_lbl(ddx), lx, ly, 0.133, 0.773, 0.369);
  }

  // Vertical leg — Δy
  if (ddy > 0.5) {
    double lx = csx - bx_dir2 * 26.0;
    double ly = vmid_y;
    draw_label(fmt_lbl(ddy), lx, ly, 0.133, 0.773, 0.369);
  }

  // Angle α at A
  if (dist > 0.5) {
    double lx = asx + bx_dir2 * 22.0;
    double ly = asy + by_dir2 * 18.0;
    draw_label(fmt_deg_lbl(alpha), lx, ly, 1.0, 0.85, 0.3);
  }

  // Angle β at B
  if (dist > 0.5) {
    double lx = bsx - bx_dir2 * 22.0;
    double ly = bsy - by_dir2 * 18.0;
    draw_label(fmt_deg_lbl(beta), lx, ly, 1.0, 0.85, 0.3);
  }

  // Coord tags at endpoints — small "(x, y)" pills, white-ish
  {
    auto fmt_xy = [&](double x, double y) -> std::string {
      return std::string("(") + fmt_val(x) + ", " + fmt_val(y) + ")";
    };
    // A endpoint — placed just outside the right-angle direction so it
    // doesn't overlap leg labels.
    double ax_lx = asx - bx_dir2 * 28.0;
    double ax_ly = asy - by_dir2 * 14.0;
    draw_label(fmt_xy(ax_user, ay_user), ax_lx, ax_ly, 0.85, 0.85, 0.95);
    double bx_lx = bsx + bx_dir2 * 28.0;
    double bx_ly = bsy + by_dir2 * 14.0;
    draw_label(fmt_xy(bx_user, by_user), bx_lx, bx_ly, 0.85, 0.85, 0.95);
  }
}

// ── draw_ruler_overlay
// ──────────────────────────────────────────────────────── Draws the
// measurement triangle overlay in screen space. Called from on_draw after all
// content.
void Canvas::draw_ruler_overlay(const Cairo::RefPtr<Cairo::Context> &cr,
                                int /*w*/, int /*h*/) {
  if (!m_doc)
    return;

  m_ruler_labels.clear();

  // ── Pick-mode: draw all objects as outlines + all nodes as hollow squares ──
  // This gives the user a full node map to click from, regardless of which
  // tool was active before. Drawn in doc space then restored.
  {
    cr->save();
    const double ox = doc_origin_x();
    const double oy = doc_origin_y();
    cr->translate(ox, oy);
    cr->scale(m_zoom, m_zoom);

    std::vector<double> no_dash;
    cr->set_dash(no_dash, 0);

    // s182: pick-map pass driven by ruler_collect_all_endpoints, which
    // is path nodes (recursive, descending into Compound and Group) plus
    // refpts (idx=-1 sentinel). Pre-s182 the path-only pass was a flat
    // layer-children walk identical to the pre-s182 collector, so
    // compound/group child path nodes were skipped — they neither lit
    // up nor became pickable. Pre-s182 refpts had a separate magenta-dot
    // pass with its own A/B blue-fill code, which made them visually
    // different from path-node candidates. s182 unifies the two: every
    // pickable point draws as the same hollow white square (filled blue
    // when it's A or B), regardless of whether it sits on a path or is a
    // refpt. Refpt identity is still signalled by the layer-coloured
    // crosshair drawn underneath in the regular scene-draw pass.
    std::vector<std::pair<SceneNode *, int>> pick_nodes;
    ruler_collect_all_endpoints(pick_nodes);

    // Outline pass — one trace per distinct path object. Refpts have no
    // outline geometry (they're points), so skip them by checking for
    // a real path. Distinct via "different pointer than previous" — the
    // collector emits all (obj, ni) entries for one path consecutively.
    std::vector<SceneNode *> outline_objs;
    {
      SceneNode *prev = nullptr;
      for (auto &[obj, ni] : pick_nodes) {
        if (obj == prev)
          continue;
        if (!obj || !obj->path)
          continue;  // refpts skipped here (path is null)
        outline_objs.push_back(obj);
        prev = obj;
      }
    }
    for (SceneNode *obj : outline_objs) {
      BezierPath bp = BezierPath::from_path_data(*obj->path);
      bp.apply_to_cairo(cr);
      cr->set_source_rgba(0.6, 0.6, 0.6, 0.45);
      cr->set_line_width(1.0 / m_zoom);
      cr->stroke();
    }

    // Per-endpoint hollow-square pass — direct iteration of the
    // collector. Position resolved via ruler_endpoint_pos so the same
    // code handles Path nodes (idx>=0) and refpts (idx=-1) uniformly.
    {
      double ns = 3.5 / m_zoom; // half-size in doc units
      for (auto &[obj, ni] : pick_nodes) {
        double nx, ny;
        if (!ruler_endpoint_pos(obj, ni, nx, ny))
          continue;
        bool is_a = (obj == m_ruler_node_a_obj && ni == m_ruler_node_a_idx);
        bool is_b = (obj == m_ruler_node_b_obj && ni == m_ruler_node_b_idx);
        if (is_a || is_b) {
          // Selected endpoints — filled blue
          cr->set_source_rgba(0.084, 0.325, 0.620, 1.0);
          cr->rectangle(nx - ns, ny - ns, ns * 2, ns * 2);
          cr->fill();
        } else {
          // Unselected endpoints — hollow white square
          cr->set_source_rgba(0.85, 0.85, 0.85, 0.9);
          cr->rectangle(nx - ns, ny - ns, ns * 2, ns * 2);
          cr->fill();
          cr->set_source_rgba(0.4, 0.4, 0.4, 0.9);
          cr->set_line_width(0.75 / m_zoom);
          cr->rectangle(nx - ns, ny - ns, ns * 2, ns * 2);
          cr->stroke();
        }
      }
    }

    cr->restore();
  }

  // ── Toast overlay ─────────────────────────────────────────────────────
  // s182 m4 — hoisted above the A-null early-return so toasts paint
  // regardless of A/B state. Pre-s182 the toast lived after the triangle
  // block, so a "no measurement point at this location" toast fired at
  // a fresh-state click could never appear. Toasts are independent of
  // ruler state — they're a screen-space message about the most recent
  // event.
  if (m_ruler_toast_ms > 0 && !m_ruler_toast_text.empty()) {
    double alpha_t = std::min(1.0, m_ruler_toast_ms / 400.0);
    cr->save();
    cr->select_font_face("Sans", Cairo::ToyFontFace::Slant::NORMAL,
                         Cairo::ToyFontFace::Weight::NORMAL);
    cr->set_font_size(12.0);
    Cairo::TextExtents te;
    cr->get_text_extents(m_ruler_toast_text, te);
    double pw = te.width + 14.0, ph = te.height + 10.0;
    double px = m_ruler_toast_x - pw * 0.5;
    double py = m_ruler_toast_y - ph;
    cr->set_source_rgba(0.08, 0.77, 0.33, 0.92 * alpha_t);
    cr->rectangle(px, py, pw, ph);
    cr->fill();
    cr->set_source_rgba(1.0, 1.0, 1.0, alpha_t);
    cr->move_to(px + 7.0, py + ph - 5.0);
    cr->show_text(m_ruler_toast_text);
    cr->restore();
  }

  // Need at least one endpoint picked to draw the triangle
  if (!m_ruler_node_a_obj)
    return;

  // S89: endpoint position resolves both Node and Ref kinds.
  double na_x, na_y;
  if (!ruler_endpoint_pos(m_ruler_node_a_obj, m_ruler_node_a_idx, na_x, na_y))
    return;

  // s182 m3 — A-only state: draw a dashed track line from A to the
  // current cursor position so the user sees that the tool is in flight
  // and tracking. Drawn in doc space with the same translate/scale as
  // the pick-map pass above (which has already restored). The line is
  // thin and dashed; the filled blue square at A (drawn during the
  // pick-map pass) anchors the start.
  if (!m_ruler_node_b_obj) {
    cr->save();
    const double ox2 = doc_origin_x();
    const double oy2 = doc_origin_y();
    cr->translate(ox2, oy2);
    cr->scale(m_zoom, m_zoom);

    cr->set_source_rgba(0.084, 0.325, 0.620, 0.85);
    cr->set_line_width(1.5 / m_zoom);
    std::vector<double> dash = {4.0 / m_zoom, 3.0 / m_zoom};
    cr->set_dash(dash, 0);
    cr->move_to(na_x, na_y);
    cr->line_to(m_cursor_doc_x, m_cursor_doc_y);
    cr->stroke();
    cr->set_dash(std::vector<double>{}, 0);

    cr->restore();
    return;
  }

  double nb_x, nb_y;
  if (!ruler_endpoint_pos(m_ruler_node_b_obj, m_ruler_node_b_idx, nb_x, nb_y))
    return;

  // Convert to user-space (Y-up) and delegate to the shared annotation
  // renderer. push_labels=true so click-to-copy works on the live triangle.
  double ax_user = na_x;
  double ay_user = m_doc->canvas_height() - na_y;
  double bx_user = nb_x;
  double by_user = m_doc->canvas_height() - nb_y;
  draw_measurement_annotations(cr, ax_user, ay_user, bx_user, by_user,
                               /*push_labels=*/true);
}

// ── s301 m1c — Baseline guides consume the universal layout function ────────
// Reads the baseline list produced by compute_text_layout (the same one
// the glyph renderer and the cursor use) and paints each baseline as a
// dashed hairline from its (x_start, y) to (x_end, y). For 1c's
// rectangular case every baseline is the full interior width; the
// segment-level call is identical to the prior stride-loop, but the
// data source is now the layout function — so when Arc E generalizes
// compute_text_layout to outline-intersection scanline, the guides
// follow the boundary shape automatically with zero change here.
void Canvas::draw_text_baseline_guides(
    const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_doc) return;

  // ── The per-box drawing ─────────────────────────────────────────────────
  // Margin frame + baseline hairlines for one (text, boundary) pair. Factored
  // out so both the node-mode "show every box" walk and the single-selection
  // path below share one source of truth. Recomputes the layout from live
  // geometry every call, so the guides always match the current shape.
  auto draw_pair = [&](SceneNode* text, SceneNode* boundary,
                       size_t byte_start) {
    if (!text || !boundary || !boundary->path ||
        boundary->path->nodes.size() < 3)
      return;
    TextLayout tl = compute_text_layout(boundary, text, byte_start);
    if (tl.baselines.empty()) return;

    cr->save();
    cr->set_source_rgba(0.30, 0.55, 0.85, 0.45);
    cr->set_line_width(1.0);
    std::vector<double> dash = {3.0, 3.0};
    cr->set_dash(dash, 0);

    // Margin rect — faint dashed frame at the interior extents (bbox minus
    // margins). The "this is your text frame" cue. (Rect-era overlay; on a
    // skewed/curved boundary it traces the bbox, not the eroded shape — the
    // eroded-outline replacement is a separate follow-up.)
    if (boundary->path->nodes.size() >= 3) {
      const auto& bp_nodes = boundary->path->nodes;
      double bx0 = bp_nodes[0].x, by0 = bp_nodes[0].y;
      double bx1 = bx0, by1 = by0;
      for (const auto& n : bp_nodes) {
        if (n.x < bx0) bx0 = n.x; if (n.x > bx1) bx1 = n.x;
        if (n.y < by0) by0 = n.y; if (n.y > by1) by1 = n.y;
      }
      auto m = effective_text_margins(text, boundary);
      double ix0 = bx0 + m.left,  iy0 = by0 + m.top;
      double ix1 = bx1 - m.right, iy1 = by1 - m.bottom;
      if (ix1 > ix0 && iy1 > iy0) {
        double sx0, sy0, sx1, sy1;
        doc_to_screen(ix0, iy0, sx0, sy0);
        doc_to_screen(ix1, iy1, sx1, sy1);
        cr->rectangle(std::floor(sx0) + 0.5, std::floor(sy0) + 0.5,
                      std::floor(sx1 - sx0), std::floor(sy1 - sy0));
        cr->stroke();
      }
    }

    // s327 — Each baseline's (x_start, x_end) at bl.y are the form-fit
    // intersection endpoints: where this line's ribbon crosses the eroded
    // margin, computed in the UPRIGHT frame. The line IS those two points.
    // Send them home through the same frame the text renders in (rotate by
    // +frame_angle about the centroid — the inverse of compute_text_layout's
    // upright-ing) and the guide both rotates with the text and clips to the
    // margin, because the endpoints were born on the eroded boundary. At
    // frame_angle 0 this is byte-identical to the old horizontal segment.
    const double gca = std::cos(tl.frame_angle), gsa = std::sin(tl.frame_angle);
    auto upright_to_doc = [&](double ux, double uy, double& dx, double& dy) {
      double rx = ux - tl.frame_cx, ry = uy - tl.frame_cy;
      dx = tl.frame_cx + rx * gca - ry * gsa;
      dy = tl.frame_cy + rx * gsa + ry * gca;
    };
    for (const auto& bl : tl.baselines) {
      double d0x, d0y, d1x, d1y;
      upright_to_doc(bl.x_start, bl.y, d0x, d0y);
      upright_to_doc(bl.x_end,   bl.y, d1x, d1y);
      double sx0, sy0, sx1, sy1;
      doc_to_screen(d0x, d0y, sx0, sy0);
      doc_to_screen(d1x, d1y, sx1, sy1);
      if (tl.frame_angle == 0.0) {
        // Horizontal: keep the half-pixel snap for a crisp hairline.
        cr->move_to(sx0, std::floor(sy0) + 0.5);
        cr->line_to(sx1, std::floor(sy0) + 0.5);
      } else {
        cr->move_to(sx0, sy0);
        cr->line_to(sx1, sy1);
      }
      cr->stroke();
    }
    cr->restore();
  };

  // ── s323 — Node mode is the "outline mode" for text ──────────────────────
  // Every text box in every TextBoxMgr shows its baselines while the Node tool
  // is active, independent of what's selected. This deletes the whole class of
  // single-target-resolver bugs: no m_selected dependency, no resolution gap,
  // no sync-on-edit gating. draw_pair recomputes from live geometry and node
  // edits already repaint, so the guides are always current. byte_start is 0
  // per member — the hairline GRID is geometry-driven (capacity fills the
  // shape regardless of which bytes land where), so the flow offset doesn't
  // affect where the guides sit. (Cost: one layout per visible box per paint;
  // fine for typical docs, revisit with a cache if a dense doc lags.)
  if (m_tool == ActiveTool::Node) {
    std::function<void(SceneNode*)> walk = [&](SceneNode* n) {
      if (!n) return;
      if (n->is_text_box_mgr()) {
        for (auto& v : n->children) {
          if (v && v->is_canvas_view() && !v->children.empty() &&
              v->children[0] && v->children[0]->is_path())
            draw_pair(n, v->children[0].get(), 0);   // canvas members only
        }
        return;                                       // don't descend further
      }
      if (n->type == SceneNode::Type::Group ||
          n->type == SceneNode::Type::Compound ||
          n->type == SceneNode::Type::ClipGroup)
        for (auto& c : n->children) walk(c.get());
    };
    for (auto& layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto& c : layer->children) walk(c.get());
    }
    return;
  }

  // ── Non-node mode: single resolved (text, boundary) pair ─────────────────
  // Active text edit, or a selected box/boundary/legacy-bound-text.
  SceneNode* text_for_guides = nullptr;
  SceneNode* boundary_for_guides = nullptr;

  if (m_text_editing && m_text_boundary_editing) {
    text_for_guides = m_text_editing;
    boundary_for_guides = m_text_boundary_editing;
  } else if (m_selected) {
    if (m_selected->is_text_box_mgr()) {
      SceneNode* canvas_view = nullptr;
      for (auto& v : m_selected->children) {
        if (v && v->is_canvas_view()) { canvas_view = v.get(); break; }
      }
      if (canvas_view && !canvas_view->children.empty() &&
          canvas_view->children[0] &&
          canvas_view->children[0]->is_path()) {
        text_for_guides     = m_selected;
        boundary_for_guides = canvas_view->children[0].get();
      }
    } else if (m_selected->is_text() && !m_selected->text_boundary_ids.empty()) {
      text_for_guides = m_selected;
      boundary_for_guides = find_text_boundary(
          m_selected->text_boundary_ids.front());
    } else if (m_selected->is_path()) {
      SceneNode* owner_mgr = nullptr;
      if (find_textbox_member(m_selected, &owner_mgr, nullptr) && owner_mgr) {
        text_for_guides     = owner_mgr;
        boundary_for_guides = m_selected;
      } else {
        for (auto& layer : m_doc->layers) {
          for (auto& c : layer->children) {
            if (c->is_text() && !c->text_boundary_ids.empty() &&
                c->text_boundary_ids.front() == m_selected->internal_id) {
              text_for_guides = c.get();
              boundary_for_guides = m_selected;
              break;
            }
          }
          if (text_for_guides) break;
        }
      }
    }
  }

  if (!text_for_guides || !boundary_for_guides) return;

  // Suppress guides for the OA/overflow popover region (self-contained chrome).
  if (text_for_guides->is_text_box_mgr()) {
    for (const auto& v : text_for_guides->children) {
      if (v && v->is_popover_view() && !v->children.empty() &&
          v->children[0] && v->children[0].get() == boundary_for_guides) {
        return;
      }
    }
  }

  draw_pair(text_for_guides, boundary_for_guides, 0);
}

// ── s328 m4a — text compass ──────────────────────────────────────────────────
// The text-angle indicator. NOT tied to the bbox (it just lives inside it):
// drawn at the text centre (the boundary centroid, the same point
// compute_text_layout pivots the reflow about), oriented to the live
// text_baseline_angle. A long arm marks the reading direction (with an
// arrowhead), a short arm crosses it perpendicular — a compass needle that
// visualises the angle the drag is tracking. Shown in compass mode (T) so
// the user sees the affordance the moment the box is "about to rotate," and
// kept live during the drag (m_text_compass_live_angle). It is a readout, not
// an interaction target — the gesture is a press anywhere in the text body
// (see on_draw_begin), with the press point as the angle vertex.
void Canvas::draw_text_compass(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_doc || m_tool != ActiveTool::Selection) return;
  if (!(m_text_compass_mode || m_text_compass_dragging)) return;
  if (m_text_editing) return;
  if (m_selection.size() != 1) return;

  SceneNode *boundary = m_selection[0];
  SceneNode *mgr = nullptr;
  if (!find_textbox_member(boundary, &mgr, nullptr) || !mgr) return;
  if (!boundary || !boundary->path || boundary->path->nodes.empty()) return;

  // Centre = boundary centroid (matches text_frame_basis cx/cy).
  double sx = 0.0, sy = 0.0;
  for (const auto &n : boundary->path->nodes) { sx += n.x; sy += n.y; }
  const double n = (double)boundary->path->nodes.size();
  double ccx = sx / n, ccy = sy / n;

  // Live angle while this box is being dragged; otherwise the stored angle.
  double angle = (m_text_compass_dragging && m_text_compass_text == mgr)
                     ? m_text_compass_live_angle
                     : mgr->text_baseline_angle;

  double cx, cy;
  doc_to_screen(ccx, ccy, cx, cy);
  const double ca = std::cos(angle), sa = std::sin(angle);

  // Screen-space arm lengths (constant regardless of zoom).
  const double arm   = 46.0; // reading-direction half-length
  const double perp  = 16.0; // perpendicular half-length
  const double ring  = 6.0;  // hub radius
  const double headl = 10.0; // arrowhead length
  const double headw = 6.0;  // arrowhead half-width

  // Endpoints (screen y grows downward — sin term added, not subtracted).
  auto axis = [&](double t_along, double t_perp, double &px, double &py) {
    px = cx + t_along * ca - t_perp * sa;
    py = cy + t_along * sa + t_perp * ca;
  };
  double ax0, ay0, ax1, ay1, px0, py0, px1, py1;
  axis(-arm, 0, ax0, ay0);
  axis( arm, 0, ax1, ay1);
  axis(0, -perp, px0, py0);
  axis(0,  perp, px1, py1);

  cr->save();
  cr->set_line_cap(Cairo::Context::LineCap::ROUND);

  // Dark halo underneath for contrast on any background.
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.55);
  cr->set_line_width(3.5);
  cr->move_to(ax0, ay0); cr->line_to(ax1, ay1); cr->stroke();
  cr->move_to(px0, py0); cr->line_to(px1, py1); cr->stroke();

  // Violet needle — distinct from the orange pivot and blue handles.
  cr->set_source_rgba(0.62, 0.22, 0.86, 0.95);
  cr->set_line_width(1.6);
  cr->move_to(ax0, ay0); cr->line_to(ax1, ay1); cr->stroke();
  cr->move_to(px0, py0); cr->line_to(px1, py1); cr->stroke();

  // Arrowhead at the reading-direction (+) end.
  double hbx, hby, hlx, hly, hrx, hry;
  axis(arm, 0, hbx, hby);
  axis(arm - headl, -headw, hlx, hly);
  axis(arm - headl,  headw, hrx, hry);
  cr->set_source_rgba(0.62, 0.22, 0.86, 0.95);
  cr->move_to(hbx, hby);
  cr->line_to(hlx, hly);
  cr->line_to(hrx, hry);
  cr->close_path();
  cr->fill();

  // Hub ring.
  cr->set_source_rgba(0.0, 0.0, 0.0, 0.55);
  cr->set_line_width(3.0);
  cr->arc(cx, cy, ring, 0, 2 * M_PI);
  cr->stroke();
  cr->set_source_rgba(1.0, 1.0, 1.0, 1.0);
  cr->set_line_width(1.6);
  cr->arc(cx, cy, ring, 0, 2 * M_PI);
  cr->stroke();

  cr->restore();
}

// ── s322 — Form-fit reflow debug overlay (TEMPORARY) ─────────────────────────
// Pure geometry probe: on a selected closed Path with the Node tool active,
// draw the eroded margin (orange), the float-found first baseline (green), and
// the per-line spans striding down by leading (blue). Exercises the whole
// math chain — erode_outline -> baseline_ribbon -> intersect_regions_polylines
// -> intervals_for_baseline — with no dependency on real text layout, so the
// geometry can be validated on a shape (and watched reflow live as the shape
// is node-edited) before wiring into compute_text_layout. Debug constants here
// stand in for the text node's margin + font metrics.
void Canvas::draw_formfit_debug(const Cairo::RefPtr<Cairo::Context>& cr) {
  if (!m_doc) return;
  if (m_tool != ActiveTool::Node) return;
  if (!m_selected || !m_selected->is_path() || !m_selected->path) return;
  const PathData& shape = *m_selected->path;
  if (!shape.closed || shape.nodes.size() < 3) return;

  // Stand-ins for the text node's margin + font metrics (overlay only).
  const double inset    = 12.0;   // uniform margin (erosion distance)
  const double ascent   = 19.0;   // ~0.8 * 24pt — band above baseline
  const double leading  = 29.0;   // ~1.2 * 24pt — line stride
  const double ribbon_t = 2.0;    // baseline ribbon thickness

  std::vector<PathData> eroded = Curvz::erode_outline(shape, inset);
  if (eroded.empty()) return;

  // bbox of the eroded region (float bounds + baseline overhang).
  double ex0 = 1e18, ey0 = 1e18, ex1 = -1e18, ey1 = -1e18;
  for (const auto& pd : eroded)
    for (const auto& n : pd.nodes) {
      ex0 = std::min(ex0, n.x); ex1 = std::max(ex1, n.x);
      ey0 = std::min(ey0, n.y); ey1 = std::max(ey1, n.y);
    }
  if (ex1 <= ex0 || ey1 <= ey0) return;

  // Straight base baseline at y=0, overhanging the bbox on both ends so every
  // run boundary in the intersect is a true margin crossing.
  const double over = (ex1 - ex0) * 0.5 + inset;
  auto mk = [](double x, double y) {
    BezierNode n; n.x = x; n.y = y; n.cx1 = x; n.cy1 = y; n.cx2 = x; n.cy2 = y;
    n.type = BezierNode::Type::Corner; return n;
  };
  PathData base;
  base.nodes.push_back(mk(ex0 - over, 0.0));
  base.nodes.push_back(mk(ex1 + over, 0.0));
  base.closed = false;

  cr->save();

  // Eroded margin outline(s) — orange.
  cr->set_line_width(1.0);
  cr->set_source_rgba(0.95, 0.55, 0.15, 0.9);
  for (const auto& pd : eroded) {
    if (pd.nodes.size() < 2) continue;
    double sx, sy;
    doc_to_screen(pd.nodes[0].x, pd.nodes[0].y, sx, sy);
    cr->move_to(sx, sy);
    for (size_t i = 1; i < pd.nodes.size(); ++i) {
      doc_to_screen(pd.nodes[i].x, pd.nodes[i].y, sx, sy);
      cr->line_to(sx, sy);
    }
    cr->close_path();
    cr->stroke();
  }

  // Float the CAP line (baseline - ascent) down until it clears the eroded
  // top — i.e. until a run appears. That baseline is where the first line
  // sits. (Inside-out "push up until it hits" lands on the same y; this scans
  // the cap downward from above the top, which is the same contact.)
  const double step = std::max(1.0, leading * 0.1);
  double first_baseline = 1e18;
  for (double by = ey0; by <= ey1; by += step) {
    auto caps = Curvz::intervals_for_baseline(eroded, base, by - ascent, ribbon_t);
    if (!caps.empty()) { first_baseline = by; break; }
  }
  if (first_baseline > 1e17) { cr->restore(); return; }

  // Stride baselines down by leading; draw each line's spans. A line with no
  // run (a pinch/gap) is skipped, not a stop — lower lines may still clear.
  cr->set_line_width(2.0);
  bool first = true;
  for (double by = first_baseline; by <= ey1; by += leading) {
    auto spans = Curvz::intervals_for_baseline(eroded, base, by, ribbon_t);
    if (spans.empty()) continue;
    if (first) cr->set_source_rgba(0.20, 0.85, 0.40, 0.95);   // first line: green
    else       cr->set_source_rgba(0.30, 0.60, 1.00, 0.85);   // rest: blue
    for (const auto& sp : spans) {
      double sx0, sy0, sx1, sy1;
      doc_to_screen(sp.start.x, sp.start.y, sx0, sy0);
      doc_to_screen(sp.end.x,   sp.end.y,   sx1, sy1);
      cr->move_to(sx0, sy0);
      cr->line_to(sx1, sy1);
      cr->stroke();
    }
    first = false;
  }

  cr->restore();
}

void Canvas::draw_top_overlay(const Cairo::RefPtr<Cairo::Context> &cr) {
  if (!m_doc)
    return;

  const double ox = doc_origin_x();
  const double oy = doc_origin_y();

  // Highlight selected text node (phase 1)
  if (m_top_text && m_top_phase >= 1) {
    double tx, ty;
    double doc_ty = m_doc->canvas_height() - m_top_text->text_y;
    doc_to_screen(m_top_text->text_x, doc_ty, tx, ty);
    cr->save();
    cr->set_source_rgba(0.3, 0.6, 1.0, 0.6);
    cr->set_line_width(1.5);
    double approx_w = m_top_text->text_content.size() *
                      m_top_text->text_font_size * 0.6 * m_zoom;
    double approx_h = m_top_text->text_font_size * m_zoom;
    cr->rectangle(tx - 2, ty - approx_h - 2, approx_w + 4, approx_h + 4);
    cr->stroke();
    cr->restore();
  }

  // Phase 2: draw offset drag handle at text_path_offset position
  if (m_top_phase == 2 && m_top_text && !m_top_text->text_path_id.empty()) {
    SceneNode *guide = top_find_path_by_id(m_top_text->text_path_id);
    if (guide && guide->path) {
      BezierPath bp = BezierPath::from_path_data(*guide->path);
      std::vector<double> arc_table;
      double total = build_arc_table(bp, arc_table);

      // Always draw guide path as dashed green line (not just outline mode)
      cr->save();
      cr->translate(ox, oy);
      cr->scale(m_zoom, m_zoom);
      bp.apply_to_cairo(cr);
      std::vector<double> dash = {6.0 / m_zoom, 3.0 / m_zoom};
      cr->set_dash(dash, 0);
      cr->set_source_rgba(0.133, 0.773, 0.369, 0.6);
      cr->set_line_width(1.0 / m_zoom);
      cr->stroke();
      cr->restore();

      // Draw I-beam cursor at offset position (perpendicular to path tangent).
      // flip=true: mirror to show where text actually starts on the reversed
      // path.
      double ibeam_arc = m_top_text->text_path_flip
                             ? total - m_top_text->text_path_offset
                             : m_top_text->text_path_offset;
      Vec2 pos;
      double angle;
      if (path_point_at(bp, arc_table, total, ibeam_arc, pos, angle)) {
        double hsx, hsy;
        doc_to_screen(pos.x, pos.y, hsx, hsy);

        // I-beam geometry: vertical stroke + top/bottom serifs
        // All drawn perpendicular to the path tangent
        double perp = angle + M_PI * 0.5;
        double px = std::cos(perp); // perpendicular unit vector
        double py = std::sin(perp);
        double tx = std::cos(angle); // tangent unit vector
        double ty = std::sin(angle);

        const double stem = 12.0; // half-height of vertical stroke
        const double serif = 5.0; // half-width of top/bottom serifs
        const double stroke_w = 1.5;

        // Top and bottom serif endpoints
        double top_x = hsx + px * stem;
        double top_y = hsy + py * stem;
        double bot_x = hsx - px * stem;
        double bot_y = hsy - py * stem;

        cr->save();
        // Shadow pass for contrast
        cr->set_source_rgba(0.0, 0.0, 0.0, 0.5);
        cr->set_line_width(stroke_w + 2.0);
        cr->set_line_cap(Cairo::Context::LineCap::ROUND);
        // Vertical stem
        cr->move_to(top_x, top_y);
        cr->line_to(bot_x, bot_y);
        cr->stroke();
        // Top serif
        cr->move_to(top_x - tx * serif, top_y - ty * serif);
        cr->line_to(top_x + tx * serif, top_y + ty * serif);
        cr->stroke();
        // Bottom serif
        cr->move_to(bot_x - tx * serif, bot_y - ty * serif);
        cr->line_to(bot_x + tx * serif, bot_y + ty * serif);
        cr->stroke();

        // Green foreground pass
        cr->set_source_rgba(0.133, 0.773, 0.369, 1.0);
        cr->set_line_width(stroke_w);
        cr->move_to(top_x, top_y);
        cr->line_to(bot_x, bot_y);
        cr->stroke();
        cr->move_to(top_x - tx * serif, top_y - ty * serif);
        cr->line_to(top_x + tx * serif, top_y + ty * serif);
        cr->stroke();
        cr->move_to(bot_x - tx * serif, bot_y - ty * serif);
        cr->line_to(bot_x + tx * serif, bot_y + ty * serif);
        cr->stroke();
        cr->restore();
      }
    }
  }
}

// ── Guide rendering
// ─────────────────────────────────────────────────────────── ──
// draw_guides_doc_space ─────────────────────────────────────────────────────
// Draws guide lines for the given guide layer in doc space.
// Called from draw_objects where cr has translate(ox,oy) active but NO scale.
// Guide lines extend across the full canvas in doc units so they clip naturally
// to the artboard when the artboard clip rect is active.
// This replaces the old identity-matrix approach so guides respect z-order.
void Canvas::draw_guides_doc_space(const Cairo::RefPtr<Cairo::Context> &cr,
                                   const SceneNode *gl) {
  if (!gl || !gl->visible)
    return;
  if (!m_doc)
    return;

  const double cw = m_doc->canvas_width();  // doc units
  const double ch = m_doc->canvas_height(); // doc units

  std::vector<double> dash = {6.0, 4.0};
  std::vector<double> no_dash = {};
  cr->save();
  cr->set_line_width(1.0);

  for (const auto &child : gl->children) {
    if (!child->is_guide())
      continue;
    bool hovered = (child.get() == m_guide_hovered);
    bool selected =
        std::find(m_guide_selection.begin(), m_guide_selection.end(),
                  child.get()) != m_guide_selection.end();
    double r = m_doc->guide_color_r;
    double g = m_doc->guide_color_g;
    double b = m_doc->guide_color_b;

    if (selected) {
      cr->set_dash(no_dash, 0);
      cr->set_source_rgba(r, g, b, 1.0);
    } else {
      cr->set_dash(dash, 0);
      cr->set_source_rgba(r, g, b, hovered ? 1.0 : 0.7);
    }

    // Render from the (guide_x, guide_y, guide_angle) triplet.  angle is
    // in degrees, doc-Y-down space (0°=H, 90°=V, +=CW visually).  We draw
    // an infinite line through the anchor at that angle, extended well
    // past the artboard on both sides; the artboard clip in draw_objects
    // trims it to the visible region.
    const double ax = child->guide_x * m_zoom;
    const double ay = child->guide_y * m_zoom;
    const double a_rad = child->guide_angle * M_PI / 180.0;
    const double dxu = std::cos(a_rad);
    const double dyu = std::sin(a_rad);
    // Choose an extent larger than any reasonable artboard diagonal so
    // the line spans the visible region after clipping.
    const double span = std::hypot(cw, ch) * m_zoom * 2.0 + 1000.0;
    const double x0 = ax - dxu * span;
    const double y0 = ay - dyu * span;
    const double x1 = ax + dxu * span;
    const double y1 = ay + dyu * span;
    cr->move_to(x0, y0);
    cr->line_to(x1, y1);
    cr->stroke();
  }
  cr->restore();
}

void Canvas::draw_guides(const Cairo::RefPtr<Cairo::Context> &cr, int w,
                         int h) {
  if (!m_doc)
    return;
  const SceneNode *gl = m_doc->guide_layer();
  if (!gl || !gl->visible)
    return;

  const double ox = doc_origin_x();
  const double oy = doc_origin_y();

  std::vector<double> dash = {6.0, 4.0};
  std::vector<double> no_dash = {};
  cr->save();
  cr->set_line_width(1.0);

  for (const auto &child : gl->children) {
    if (!child->is_guide())
      continue;
    bool hovered = (child.get() == m_guide_hovered);
    bool selected =
        std::find(m_guide_selection.begin(), m_guide_selection.end(),
                  child.get()) != m_guide_selection.end();
    double r = m_doc->guide_color_r;
    double g = m_doc->guide_color_g;
    double b = m_doc->guide_color_b;

    if (selected) {
      cr->set_dash(no_dash, 0);
      cr->set_source_rgba(r, g, b, 1.0);
    } else {
      cr->set_dash(dash, 0);
      cr->set_source_rgba(r, g, b, hovered ? 1.0 : 0.7);
    }

    // See draw_guides_doc_space for the model rationale.  This legacy
    // entry point appears unused as of S49 but is kept consistent so it
    // doesn't render stale H/V-only geometry if anything calls it.
    const double ax = child->guide_x * m_zoom + ox;
    const double ay = child->guide_y * m_zoom + oy;
    const double a_rad = child->guide_angle * M_PI / 180.0;
    const double dxu = std::cos(a_rad);
    const double dyu = std::sin(a_rad);
    const double span = std::hypot((double)w, (double)h) * 2.0 + 1000.0;
    cr->move_to(ax - dxu * span, ay - dyu * span);
    cr->line_to(ax + dxu * span, ay + dyu * span);
    cr->stroke();
  }
  cr->restore();
}

// ── draw_grid_doc_space
// ──────────────────────────────────────────────────────────
// Draws a regular grid overlay in doc space. cr has translate(ox,oy) active
// but NO scale — lines are drawn in doc units, zoom is applied externally.
void Canvas::draw_grid_doc_space(const Cairo::RefPtr<Cairo::Context> &cr,
                                 const SceneNode *gl) {
  if (!gl || !gl->visible || !m_doc)
    return;

  const double cw = (double)m_doc->canvas_width();
  const double ch = (double)m_doc->canvas_height();
  const double sx = gl->grid_spacing_x;
  const double sy = gl->grid_spacing_y;
  if (sx < 0.5 || sy < 0.5)
    return;

  cr->save();
  cr->scale(m_zoom, m_zoom);
  cr->set_source_rgba(gl->grid_color_r, gl->grid_color_g, gl->grid_color_b,
                      gl->grid_color_a);
  cr->set_line_width(1.0 / m_zoom);

  const double ox = gl->grid_offset_x;
  const double oy = gl->grid_offset_y;

  if (gl->grid_dots) {
    // Dots at every grid intersection
    const double r = 1.5 / m_zoom;
    for (double x = std::fmod(ox, sx); x <= cw; x += sx) {
      for (double y = std::fmod(oy, sy); y <= ch; y += sy) {
        cr->arc(x, y, r, 0, 2 * M_PI);
        cr->fill();
      }
    }
  } else {
    // Vertical lines
    for (double x = std::fmod(ox, sx); x <= cw; x += sx) {
      cr->move_to(x, 0);
      cr->line_to(x, ch);
      cr->stroke();
    }
    // Horizontal lines
    for (double y = std::fmod(oy, sy); y <= ch; y += sy) {
      cr->move_to(0, y);
      cr->line_to(cw, y);
      cr->stroke();
    }
  }
  cr->restore();
}

// ── draw_margin_doc_space
// ────────────────────────────────────────────────────
// Draws margin and column/row guide lines as clean coloured lines.
// No tinted fill areas — objects can snap to the lines cleanly.
void Canvas::draw_margin_doc_space(const Cairo::RefPtr<Cairo::Context> &cr,
                                   const SceneNode *ml) {
  if (!ml || !ml->visible || !m_doc)
    return;

  const double cw = (double)m_doc->canvas_width();
  const double ch = (double)m_doc->canvas_height();

  const double left = ml->margin_left;
  const double right = ml->margin_right;
  const double top = ml->margin_top;
  const double bottom = ml->margin_bottom;

  const double ix = left;
  const double iy = top;
  const double iw = cw - left - right;
  const double ih = ch - top - bottom;
  if (iw <= 0 || ih <= 0)
    return;

  cr->save();
  cr->scale(m_zoom, m_zoom);

  const double r = ml->margin_color_r;
  const double g = ml->margin_color_g;
  const double b = ml->margin_color_b;
  // Lines are drawn at full opacity; alpha from the node colour sets intensity
  const double a = std::min(ml->margin_color_a * 3.0, 1.0);

  cr->set_source_rgba(r, g, b, a);
  cr->set_line_width(1.0 / m_zoom);

  // Margin border rectangle
  cr->rectangle(ix, iy, iw, ih);
  cr->stroke();

  // Column dividers — each gap is split at its centre line
  const int cols = std::max(1, ml->margin_columns);
  const double gap_x = ml->margin_col_gap;
  if (cols > 1 && iw > 0) {
    const double col_w = (iw - gap_x * (cols - 1)) / cols;
    for (int c = 1; c < cols; ++c) {
      // Left edge of gap
      double gx_left = ix + c * col_w + (c - 1) * gap_x;
      // Right edge of gap
      double gx_right = gx_left + gap_x;
      // Draw left and right edges of the gap as lines
      cr->move_to(gx_left, iy);
      cr->line_to(gx_left, iy + ih);
      cr->stroke();
      cr->move_to(gx_right, iy);
      cr->line_to(gx_right, iy + ih);
      cr->stroke();
    }
  }

  // Row dividers
  const int rows = std::max(1, ml->margin_rows);
  const double gap_y = ml->margin_row_gap;
  if (rows > 1 && ih > 0) {
    const double row_h = (ih - gap_y * (rows - 1)) / rows;
    for (int row = 1; row < rows; ++row) {
      double gy_top = iy + row * row_h + (row - 1) * gap_y;
      double gy_bottom = gy_top + gap_y;
      cr->move_to(ix, gy_top);
      cr->line_to(ix + iw, gy_top);
      cr->stroke();
      cr->move_to(ix, gy_bottom);
      cr->line_to(ix + iw, gy_bottom);
      cr->stroke();
    }
  }

  cr->restore();
}

} // namespace Curvz
