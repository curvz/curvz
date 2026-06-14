// ── GlyphOutline.cpp — shared glyph-to-outline implementation ────────────────
// See GlyphOutline.hpp. Moved from Canvas_ops.cpp (FT extraction + box outline)
// and Canvas_draw.cpp (pattern walk); the originals delegate / are deleted.

#include "GlyphOutline.hpp"
#include "TextCursor.hpp"          // BaselineLayout, TextLayout, compute_text_layout, pattern_walk_path
#include "CurvzLog.hpp"
#include "math/BezierPath.hpp"     // from_path_data, arc_table_for, point_at_arc
#include "math/Vec2.hpp"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <pango/pangocairo.h>
#include <pango/pangofc-font.h>

#include <algorithm>
#include <cmath>
#include <cstring>   // s358 — std::strlen for the trailing hyphen dash
#include <map>

namespace Curvz {

// ─────────────────────────────────────────────────────────────────────────────
// FreeType outline decomposition — build PathData contours (glyph space, Y-up).
// Each move_to starts a new closed contour; quadratics elevate to cubics.
// (Moved verbatim from Canvas_ops.cpp.)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

struct FTOutlineCtx {
  std::vector<PathData> contours;
  PathData *cur = nullptr;
  double scale = 1.0; // FT units → doc units

  double cx = 0, cy = 0; // current point (for quadratic elevation)

  static int move_to_cb(const FT_Vector *to, void *user) {
    auto *c = static_cast<FTOutlineCtx *>(user);
    c->contours.emplace_back();
    c->cur = &c->contours.back();
    c->cur->closed = true;
    double x = to->x * c->scale;
    double y = to->y * c->scale;
    BezierNode n;
    n.x = x;  n.y = y;
    n.cx1 = x; n.cy1 = y;
    n.cx2 = x; n.cy2 = y;
    n.type = BezierNode::Type::Corner;
    c->cur->nodes.push_back(n);
    c->cx = x; c->cy = y;
    return 0;
  }
  static int line_to_cb(const FT_Vector *to, void *user) {
    auto *c = static_cast<FTOutlineCtx *>(user);
    if (!c->cur) return 0;
    double x = to->x * c->scale;
    double y = to->y * c->scale;
    BezierNode n;
    n.x = x;  n.y = y;
    n.cx1 = x; n.cy1 = y;
    n.cx2 = x; n.cy2 = y;
    n.type = BezierNode::Type::Corner;
    c->cur->nodes.push_back(n);
    c->cx = x; c->cy = y;
    return 0;
  }
  static int conic_to_cb(const FT_Vector *ctrl, const FT_Vector *to,
                         void *user) {
    auto *c = static_cast<FTOutlineCtx *>(user);
    if (!c->cur || c->cur->nodes.empty()) return 0;
    double qx0 = c->cx, qy0 = c->cy;
    double qx1 = ctrl->x * c->scale, qy1 = ctrl->y * c->scale;
    double qx2 = to->x * c->scale,   qy2 = to->y * c->scale;
    double cx1 = qx0 + 2.0 / 3.0 * (qx1 - qx0);
    double cy1 = qy0 + 2.0 / 3.0 * (qy1 - qy0);
    double cx2 = qx2 + 2.0 / 3.0 * (qx1 - qx2);
    double cy2 = qy2 + 2.0 / 3.0 * (qy1 - qy2);
    c->cur->nodes.back().cx2 = cx1;
    c->cur->nodes.back().cy2 = cy1;
    BezierNode n;
    n.x = qx2; n.y = qy2;
    n.cx1 = cx2; n.cy1 = cy2;
    n.cx2 = qx2; n.cy2 = qy2;
    n.type = BezierNode::Type::Corner;
    c->cur->nodes.push_back(n);
    c->cx = qx2; c->cy = qy2;
    return 0;
  }
  static int cubic_to_cb(const FT_Vector *c1, const FT_Vector *c2,
                         const FT_Vector *to, void *user) {
    auto *c = static_cast<FTOutlineCtx *>(user);
    if (!c->cur || c->cur->nodes.empty()) return 0;
    double cx1 = c1->x * c->scale, cy1 = c1->y * c->scale;
    double cx2 = c2->x * c->scale, cy2 = c2->y * c->scale;
    double tx = to->x * c->scale,  ty = to->y * c->scale;
    c->cur->nodes.back().cx2 = cx1;
    c->cur->nodes.back().cy2 = cy1;
    BezierNode n;
    n.x = tx; n.y = ty;
    n.cx1 = cx2; n.cy1 = cy2;
    n.cx2 = tx;  n.cy2 = ty;
    n.type = BezierNode::Type::Corner;
    c->cur->nodes.push_back(n);
    c->cx = tx; c->cy = ty;
    return 0;
  }
};

FT_Outline_Funcs s_ft_callbacks = {
    FTOutlineCtx::move_to_cb,
    FTOutlineCtx::line_to_cb,
    FTOutlineCtx::conic_to_cb,
    FTOutlineCtx::cubic_to_cb,
    0, // shift
    0  // delta
};

// s347 m4 — each pattern line rides its OWN walk path (the guide, or an inward
// parallel offset). Derive per distinct offset through THE pump
// (pattern_walk_path) and cache the arc table per offset for the pass.
struct GuideWalk {
  BezierPath bp;
  std::vector<double> arc_table;
  double total = 0.0;
  bool closed = false;
  bool ok = false;
};

// Resolve a run's foreground span colour + rise, if any. Shared by the pattern
// walk and the box outliner so both colour glyphs identically.
struct RunInk {
  bool has_fg = false;
  double r = 0, g = 0, b = 0, a = 1;
  double rise_px = 0.0;
};
RunInk resolve_run_ink(PangoLayoutRun *run) {
  RunInk ink;
  guint16 fr = 0, fg = 0, fb = 0, fa = 0xffff;
  for (GSList *a = run->item->analysis.extra_attrs; a; a = a->next) {
    PangoAttribute *attr = (PangoAttribute *)a->data;
    if (attr->klass->type == PANGO_ATTR_FOREGROUND) {
      const PangoColor &c = ((PangoAttrColor *)attr)->color;
      fr = c.red; fg = c.green; fb = c.blue;
      ink.has_fg = true;
    } else if (attr->klass->type == PANGO_ATTR_FOREGROUND_ALPHA) {
      fa = ((PangoAttrInt *)attr)->value;
    } else if (attr->klass->type == PANGO_ATTR_RISE) {
      ink.rise_px = ((PangoAttrInt *)attr)->value / (double)PANGO_SCALE;
    }
  }
  if (ink.has_fg) {
    ink.r = fr / 65535.0; ink.g = fg / 65535.0;
    ink.b = fb / 65535.0; ink.a = fa / 65535.0;
  }
  return ink;
}

// Open the FT face backing a resolved Pango run font. Returns nullptr on
// failure (logged by the caller's context). Caller owns FT_Done_Face.
FT_Face open_run_face(FT_Library ft_lib, PangoFont *pfont, int &face_idx_out,
                      double &px_size_out) {
  const char *font_file = nullptr;
  int face_idx = 0;
  double px_size = 0.0;
  PangoFcFont *fc_font = PANGO_FC_FONT(pfont);
  if (fc_font) {
    FcPattern *pat = pango_fc_font_get_pattern(fc_font);
    FcPatternGetString(pat, FC_FILE, 0, (FcChar8 **)&font_file);
    FcPatternGetInteger(pat, FC_INDEX, 0, &face_idx);
    FcPatternGetDouble(pat, FC_PIXEL_SIZE, 0, &px_size);
  }
  face_idx_out = face_idx;
  px_size_out = px_size;
  if (!font_file) return nullptr;
  FT_Face face = nullptr;
  if (FT_New_Face(ft_lib, font_file, face_idx, &face) != 0)
    return nullptr;
  return face;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// pattern_glyph_walk (free) — moved from Canvas_draw.cpp; project() replaced
// by the lib parameter, Canvas arc helpers replaced by the BezierPath free
// functions. Behaviour is identical to the member it replaces.
// ─────────────────────────────────────────────────────────────────────────────
bool pattern_glyph_walk(const SceneNode &text_obj, const SceneNode &guide,
                        const style::TextStyleLibrary *lib,
                        const std::function<void(const PatternGlyph &)> &fn) {
  if (!guide.path || guide.path->nodes.size() < 2)
    return false;

  TextLayout tl = compute_text_layout(&guide, &text_obj, 0, lib);
  if (tl.baselines.empty())
    return false;

  std::map<double, GuideWalk> walks;
  auto walk_for = [&](double offset) -> const GuideWalk & {
    auto it = walks.find(offset);
    if (it != walks.end()) return it->second;
    GuideWalk w;
    PathData pd = pattern_walk_path(*guide.path, offset);
    if (pd.nodes.size() >= 2) {
      w.bp = BezierPath::from_path_data(pd);
      w.total = arc_table_for(w.bp, w.arc_table);
      w.closed = pd.closed;
      w.ok = (w.total >= 0.001);
    }
    return walks.emplace(offset, std::move(w)).first->second;
  };
  if (!walk_for(0.0).ok)
    return false;

  for (const auto &bl : tl.baselines) {
    if (!bl.pango || !bl.pattern)
      continue;
    const GuideWalk &w = walk_for(bl.pattern->offset);
    if (!w.ok)
      continue;
    const double arc_start = bl.pattern->arc_start;
    const bool flip = bl.pattern->flip;

    PangoLayoutIter *iter = pango_layout_get_iter(bl.pango.get());
    do {
      PangoLayoutRun *run = pango_layout_iter_get_run(iter);
      if (!run)
        continue;

      PangoGlyphString *gs = run->glyphs;
      PangoFont *pfont = run->item->analysis.font;

      PangoRectangle run_ext;
      pango_layout_iter_get_run_extents(iter, nullptr, &run_ext);
      double glyph_x_px = run_ext.x / (double)PANGO_SCALE;

      const RunInk ink = resolve_run_ink(run);

      for (int gi = 0; gi < gs->num_glyphs; ++gi) {
        PangoGlyphInfo &gi_info = gs->glyphs[gi];
        const double adv_px = gi_info.geometry.width / (double)PANGO_SCALE;

        if (gi_info.glyph == PANGO_GLYPH_EMPTY ||
            (gi_info.glyph & PANGO_GLYPH_UNKNOWN_FLAG)) {
          glyph_x_px += adv_px;
          continue;
        }

        const double glyph_centre_arc =
            arc_start + (glyph_x_px - bl.x_start) + adv_px * 0.5;
        double lookup_arc =
            flip ? w.total - glyph_centre_arc : glyph_centre_arc;

        if (w.closed) {
          lookup_arc = std::fmod(lookup_arc, w.total);
          if (lookup_arc < 0.0)
            lookup_arc += w.total;
        } else if (lookup_arc < 0.0 || lookup_arc > w.total) {
          glyph_x_px += adv_px;
          continue;
        }

        Vec2 pos;
        double angle;
        if (!point_at_arc(w.bp, w.arc_table, w.total, lookup_arc, pos, angle))
          break;

        const double perp = flip ? bl.ascent : 0.0;

        PatternGlyph g;
        g.info   = &gi_info;
        g.font   = pfont;
        g.pos    = pos;
        g.angle  = flip ? angle + M_PI : angle;
        g.adv_px = adv_px;
        g.pen_y  = bl.y + perp - ink.rise_px;
        g.has_fg = ink.has_fg;
        if (ink.has_fg) {
          g.fg_r = ink.r; g.fg_g = ink.g; g.fg_b = ink.b; g.fg_a = ink.a;
        }
        fn(g);

        glyph_x_px += adv_px;
      }
    } while (pango_layout_iter_next_run(iter));
    pango_layout_iter_free(iter);
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// outline_box_text — moved from Canvas_ops::emit_baseline_glyph_nodes, now
// compute_text_layout + FT init internal, and EXTENDED with per-run foreground
// colour (was a deferred follow-up: box glyph fill is now the run's span colour
// when present, else the node fill — matching what the canvas paints).
// ─────────────────────────────────────────────────────────────────────────────
void outline_box_text(const SceneNode *boundary, const SceneNode *text,
                      size_t byte_start, const style::TextStyleLibrary *lib,
                      const FillStyle &fill, const StrokeStyle &stroke,
                      double fallback_px,
                      std::vector<std::unique_ptr<SceneNode>> &out) {
  if (!boundary || !text)
    return;

  TextLayout tl = compute_text_layout(boundary, text, byte_start, lib);
  if (tl.baselines.empty())
    return;

  FT_Library ft_lib = nullptr;
  if (FT_Init_FreeType(&ft_lib) != 0) {
    LOG_WARN("outline_box_text: FreeType init failed");
    return;
  }

  // Frame rotation (upright -> doc), matching the cr transform in draw.
  const bool rot = (tl.frame_angle != 0.0);
  const double ca = std::cos(tl.frame_angle), sa = std::sin(tl.frame_angle);
  auto to_doc = [&](double &x, double &y) {
    if (!rot) return;
    double rx = x - tl.frame_cx, ry = y - tl.frame_cy;
    x = tl.frame_cx + rx * ca - ry * sa;
    y = tl.frame_cy + rx * sa + ry * ca;
  };

  // s358 — shared glyph emit. Loads + decomposes one glyph, maps its contours
  // to doc space at (origin_x, base_y), and pushes a Path (single contour) or
  // even-odd Compound (holes) into out. The per-run glyph loop and the trailing
  // hyphen dash on a wrapped line both go through this, so they produce
  // byte-identical contours. The face must already be sized by the caller.
  auto emit_glyph_outline = [&](FT_Face face, FT_UInt glyph_id,
                                double origin_x, double base_y) {
    if (FT_Load_Glyph(face, glyph_id, FT_LOAD_NO_BITMAP) != 0 ||
        face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
      return;
    FTOutlineCtx ctx;
    ctx.scale = 1.0 / 64.0;
    FT_Outline_Decompose(&face->glyph->outline, &s_ft_callbacks, &ctx);

    std::vector<PathData> contours;
    for (auto &pd : ctx.contours) {
      if (pd.nodes.empty())
        continue;
      for (auto &n : pd.nodes) {
        auto map = [&](double &nx, double &ny) {
          double dx = origin_x + nx;
          double dy = base_y - ny;
          to_doc(dx, dy);
          nx = dx; ny = dy;
        };
        map(n.x, n.y);
        map(n.cx1, n.cy1);
        map(n.cx2, n.cy2);
      }
      contours.push_back(std::move(pd));
    }

    if (contours.size() == 1) {
      auto p = std::make_unique<SceneNode>();
      p->type = SceneNode::Type::Path;
      p->name = "glyph";
      p->fill = fill;
      p->stroke = stroke;
      p->path = std::make_unique<PathData>(std::move(contours[0]));
      out.push_back(std::move(p));
    } else if (contours.size() >= 2) {
      auto comp = std::make_unique<SceneNode>();
      comp->type = SceneNode::Type::Compound;
      comp->name = "glyph";
      comp->fill = fill;
      comp->stroke = stroke;
      for (auto &pd : contours) {
        auto child = std::make_unique<SceneNode>();
        child->type = SceneNode::Type::Path;
        child->fill = fill;
        child->stroke = stroke;
        child->path = std::make_unique<PathData>(std::move(pd));
        comp->children.push_back(std::move(child));
      }
      out.push_back(std::move(comp));
    }
  };

  for (const auto &bl : tl.baselines) {
    if (!bl.pango)
      continue;
    PangoLayout *layout = bl.pango.get();

    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    PangoFont *last_pfont = nullptr;  // s358 — trailing run's font for the dash
    do {
      PangoLayoutRun *run = pango_layout_iter_get_run(iter);
      if (!run)
        continue;
      PangoFont *pfont = run->item->analysis.font;
      last_pfont = pfont;
      PangoGlyphString *gs = run->glyphs;

      PangoRectangle run_ext;
      pango_layout_iter_get_run_extents(iter, nullptr, &run_ext);
      double run_x_px = run_ext.x / (double)PANGO_SCALE;

      int face_idx = 0;
      double px_size = 0.0;
      FT_Face ft_face = open_run_face(ft_lib, pfont, face_idx, px_size);
      if (!ft_face) {
        LOG_WARN("outline_box_text: no FT face for run, skipping");
        continue;
      }
      if (px_size <= 0.0)
        px_size = fallback_px;
      // 72 dpi so 1pt == 1px: FC_PIXEL_SIZE px maps straight to FT char size.
      FT_Set_Char_Size(ft_face, 0, (FT_F26Dot6)(px_size * 64.0), 72, 72);

      double pen_x = run_x_px;
      for (int gi = 0; gi < gs->num_glyphs; ++gi) {
        PangoGlyphInfo &g = gs->glyphs[gi];
        PangoGlyph glyph_id = g.glyph;
        double adv = g.geometry.width / (double)PANGO_SCALE;
        if (glyph_id == PANGO_GLYPH_EMPTY ||
            (glyph_id & PANGO_GLYPH_UNKNOWN_FLAG)) {
          pen_x += adv;
          continue;
        }
        double gx = pen_x + g.geometry.x_offset / (double)PANGO_SCALE;
        double glyph_base_y =
            bl.y + g.geometry.y_offset / (double)PANGO_SCALE;

        emit_glyph_outline(ft_face, glyph_id, bl.x_start + gx, glyph_base_y);
        pen_x += adv;
      }
      FT_Done_Face(ft_face);
    } while (pango_layout_iter_next_run(iter));
    pango_layout_iter_free(iter);

    // s358 — trailing hyphen dash for a wrapped line. The cairo renderers
    // paint a '-' overlay at the line's true glyph end (draw_hyphen_dash); the
    // saved-SVG compat outline must carry the same dash as a real glyph contour
    // or foreign viewers (Inkscape, browsers) show the hyphenated wrapping with
    // no hyphen at the breaks. The dash rides the LAST run's font; x_end comes
    // from index_to_pos at end-of-line text (the actual glyph end under any
    // alignment, justified included). Hyphen-width reservation already lives in
    // the layout (compute_text_layout), so x_end sits inside the line box and no
    // extra budgeting is needed here.
    if (bl.ended_by_hyphen && last_pfont) {
      const char *ltxt = pango_layout_get_text(layout);
      const int lnb = ltxt ? (int)std::strlen(ltxt) : 0;
      PangoRectangle pe;
      pango_layout_index_to_pos(layout, lnb, &pe);
      const double x_end = bl.x_start + (double)pe.x / (double)PANGO_SCALE;

      int dfi = 0;
      double dpx = 0.0;
      FT_Face dash_face = open_run_face(ft_lib, last_pfont, dfi, dpx);
      if (dash_face) {
        if (dpx <= 0.0)
          dpx = fallback_px;
        FT_Set_Char_Size(dash_face, 0, (FT_F26Dot6)(dpx * 64.0), 72, 72);
        FT_UInt dgid = FT_Get_Char_Index(dash_face, '-');
        if (dgid != 0) {
          emit_glyph_outline(dash_face, dgid, x_end, bl.y);
          LOG_DEBUG("outline_box_text: emitted hyphen dash at x_end={:.2f} "
                    "base_y={:.2f}",
                    x_end, bl.y);
        }
        FT_Done_Face(dash_face);
      }
    }
  }

  FT_Done_FreeType(ft_lib);
}

// ─────────────────────────────────────────────────────────────────────────────
// outline_pattern_text — the geometry core of the convert verb's v2 branch,
// returning colour-bucketed Compounds. Caller names + wraps. (Moved from
// Canvas_ops::text_to_paths_op; behaviour identical.)
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::unique_ptr<SceneNode>>
outline_pattern_text(const SceneNode &text_obj, const SceneNode &guide,
                     const style::TextStyleLibrary *lib) {
  std::vector<std::unique_ptr<SceneNode>> result;
  if (!guide.path)
    return result;

  FT_Library ft_lib = nullptr;
  if (FT_Init_FreeType(&ft_lib) != 0) {
    LOG_WARN("outline_pattern_text: FreeType init failed");
    return result;
  }

  // Colour buckets: glyphs covered by a foreground span outline into a Solid
  // bucket per span colour; everything else into the node-fill bucket.
  struct Bucket {
    bool has_fg;
    double r, g, b, a;
    std::vector<PathData> contours;
  };
  std::vector<Bucket> buckets;
  auto bucket_for = [&](const PatternGlyph &g) -> Bucket & {
    for (auto &bk : buckets) {
      if (bk.has_fg != g.has_fg)
        continue;
      if (!g.has_fg ||
          (bk.r == g.fg_r && bk.g == g.fg_g && bk.b == g.fg_b &&
           bk.a == g.fg_a))
        return bk;
    }
    buckets.push_back({g.has_fg, g.fg_r, g.fg_g, g.fg_b, g.fg_a, {}});
    return buckets.back();
  };

  // FT face per Pango run font, opened lazily and sized from the FONT's own
  // absolute size — styles/spans make size per-RUN; node text_font_size is only
  // the baseline tier and would mis-size styled runs.
  struct FaceEntry { FT_Face face = nullptr; bool tried = false; };
  std::map<PangoFont *, FaceEntry> faces;
  auto face_for = [&](PangoFont *pfont) -> FT_Face {
    auto &e = faces[pfont];
    if (e.tried)
      return e.face;
    e.tried = true;
    int face_idx = 0;
    double px_unused = 0.0;
    FT_Face face = open_run_face(ft_lib, pfont, face_idx, px_unused);
    if (!face) {
      LOG_WARN("outline_pattern_text: could not resolve font file for run, "
               "skipping its glyphs");
      e.face = nullptr;
      return nullptr;
    }
    double size_px = text_obj.text_font_size;
    if (PangoFontDescription *d =
            pango_font_describe_with_absolute_size(pfont)) {
      if (pango_font_description_get_size(d) > 0)
        size_px = pango_font_description_get_size(d) / (double)PANGO_SCALE;
      pango_font_description_free(d);
    }
    FT_Set_Char_Size(face, 0, (FT_F26Dot6)(size_px * 64.0), 72, 72);
    e.face = face;
    return face;
  };

  pattern_glyph_walk(text_obj, guide, lib, [&](const PatternGlyph &g) {
    FT_Face face = face_for(g.font);
    if (!face)
      return;
    if (FT_Load_Glyph(face, g.info->glyph, FT_LOAD_NO_BITMAP) != 0 ||
        face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
      return;

    FTOutlineCtx ctx;
    ctx.scale = 1.0 / 64.0;
    FT_Outline_Decompose(&face->glyph->outline, &s_ft_callbacks, &ctx);

    // EXACTLY the renderer's frame: pen at (-adv/2, pen_y) in the rotated
    // frame; show_glyph_string applies glyph geometry offsets on top of the
    // pen and draws FT Y-up ink downward, so an FT point (fx, fy) lands at
    //   local = (fx + x_off - adv/2,  pen_y + y_off - fy)
    // then rotates by angle about the walk point.
    const double ca = std::cos(g.angle), sa = std::sin(g.angle);
    const double x_off = g.info->geometry.x_offset / (double)PANGO_SCALE;
    const double y_off = g.info->geometry.y_offset / (double)PANGO_SCALE;
    auto xform = [&](double &nx, double &ny) {
      const double lx = nx + x_off - g.adv_px * 0.5;
      const double ly = g.pen_y + y_off - ny;
      nx = g.pos.x + lx * ca - ly * sa;
      ny = g.pos.y + lx * sa + ly * ca;
    };
    Bucket &bucket = bucket_for(g);
    for (auto &pd : ctx.contours) {
      for (auto &n : pd.nodes) {
        xform(n.x, n.y);
        xform(n.cx1, n.cy1);
        xform(n.cx2, n.cy2);
      }
      if (!pd.nodes.empty())
        bucket.contours.push_back(std::move(pd));
    }
  });

  for (auto &fe : faces)
    if (fe.second.face)
      FT_Done_Face(fe.second.face);
  FT_Done_FreeType(ft_lib);

  buckets.erase(std::remove_if(buckets.begin(), buckets.end(),
                               [](const Bucket &b) {
                                 return b.contours.empty();
                               }),
                buckets.end());
  if (buckets.empty()) {
    LOG_WARN("outline_pattern_text: '{}' produced no contours",
             text_obj.text_content);
    return result;
  }

  // One Compound per bucket. A Compound paints with ITS OWN fill (even-odd,
  // child fills inert -- S58g); fills/stroke/opacity seeded from text_obj.
  for (auto &b : buckets) {
    auto compound = std::make_unique<SceneNode>();
    compound->type = SceneNode::Type::Compound;
    if (b.has_fg) {
      FillStyle f = text_obj.fill;
      f.type = FillStyle::Type::Solid;
      f.r = b.r; f.g = b.g; f.b = b.b; f.a = b.a;
      compound->fill = f;
    } else {
      compound->fill = text_obj.fill;
    }
    compound->stroke = text_obj.stroke;
    compound->opacity = text_obj.opacity;
    for (auto &pd : b.contours) {
      auto path_child = std::make_unique<SceneNode>();
      path_child->type = SceneNode::Type::Path;
      path_child->fill = compound->fill;
      path_child->stroke = text_obj.stroke; // Compound draws stroke per-child
      path_child->path = std::make_unique<PathData>(std::move(pd));
      compound->children.push_back(std::move(path_child));
    }
    result.push_back(std::move(compound));
  }
  return result;
}

} // namespace Curvz
