#include "PngExporter.hpp"
#include "CurvzLog.hpp"
#include "math/BezierPath.hpp"
#include <cairomm/cairomm.h>
#include <pango/pangocairo.h>
#include <cairo/cairo.h>
#include <gdk/gdk.h>
#include <gdkmm/pixbuf.h>
#include <algorithm>
#include <cmath>

// ── PngExporter ───────────────────────────────────────────────────────────────
// Renders a CurvzDocument to a Cairo image surface and encodes as PNG.
//
// Coordinate mapping:
//   Document space: Y-down, origin top-left, units = canvas_width/height
//   Export surface: size_px × size_px (square)
//   Scale factor  : min(size_px/cw, size_px/ch) — fit with aspect preserved
//   Translate     : centres the scaled artwork in the square surface
//
// currentColor → black (0,0,0,1).  Symbolic PNGs are black-on-transparent per
// the freedesktop convention.  GTK recolours the SVG at runtime.

namespace Curvz {

// ── Per-surface fill/stroke helpers ──────────────────────────────────────────

static void apply_fill(const Cairo::RefPtr<Cairo::Context>& cr, const FillStyle& f) {
    switch (f.type) {
        case FillStyle::Type::None:
            break;
        case FillStyle::Type::CurrentColor:
            cr->set_source_rgba(0.0, 0.0, 0.0, 1.0);  // black = freedesktop symbolic convention
            break;
        case FillStyle::Type::Solid:
            cr->set_source_rgba(f.r, f.g, f.b, f.a);
            break;
        case FillStyle::Type::LinearGradient:
        case FillStyle::Type::RadialGradient:
            // S90 Stage 1: PNG export degrades gradients to the first stop's
            // flat colour. The local helper has no bbox in scope so a real
            // gradient render can't slot in here without refactoring (the
            // Canvas render path has the bbox via object_bbox()). Stage 2
            // adds a bbox-aware variant here that mirrors Canvas::apply_fill.
            if (!f.stops.empty()) {
                const auto& s = f.stops.front();
                cr->set_source_rgba(s.r, s.g, s.b, s.a);
            } else {
                cr->set_source_rgba(f.r, f.g, f.b, f.a);
            }
            break;
    }
}

static void apply_stroke(const Cairo::RefPtr<Cairo::Context>& cr, const StrokeStyle& s) {
    apply_fill(cr, s.paint);
    cr->set_line_width(s.width);
    switch (s.cap) {
        case LineCap::Butt:   cr->set_line_cap(Cairo::Context::LineCap::BUTT);   break;
        case LineCap::Round:  cr->set_line_cap(Cairo::Context::LineCap::ROUND);  break;
        case LineCap::Square: cr->set_line_cap(Cairo::Context::LineCap::SQUARE); break;
    }
    switch (s.join) {
        case LineJoin::Miter: cr->set_line_join(Cairo::Context::LineJoin::MITER); break;
        case LineJoin::Round: cr->set_line_join(Cairo::Context::LineJoin::ROUND); break;
        case LineJoin::Bevel: cr->set_line_join(Cairo::Context::LineJoin::BEVEL); break;
    }
    if (s.miter > 0.0) cr->set_miter_limit(s.miter);
}

// ── Object rendering ─────────────────────────────────────────────────────────

static void render_object(const Cairo::RefPtr<Cairo::Context>& cr, const SceneNode& obj);

static void render_path_obj(const Cairo::RefPtr<Cairo::Context>& cr, const SceneNode& obj) {
    if (!obj.path || obj.path->nodes.empty()) return;
    BezierPath bp = BezierPath::from_path_data(*obj.path);
    bp.apply_to_cairo(cr);

    if (obj.fill.type != FillStyle::Type::None) {
        apply_fill(cr, obj.fill);
        cr->fill_preserve();
    }
    if (obj.stroke.paint.type != FillStyle::Type::None) {
        apply_stroke(cr, obj.stroke);
        cr->stroke();
    } else {
        cr->begin_new_path();
    }
}

static void render_compound(const Cairo::RefPtr<Cairo::Context>& cr, const SceneNode& obj) {
    // Build combined path from all children
    for (const auto& child : obj.children) {
        if (child->type != SceneNode::Type::Path || !child->path) continue;
        BezierPath bp = BezierPath::from_path_data(*child->path);
        bp.apply_to_cairo(cr);
    }
    // S58p: Compound owns its paint (S58d rule). Read fill from the
    // Compound itself; do not fall back to children. Also use the
    // Compound's stroke for every subpath rather than per-child stroke.
    if (obj.fill.type != FillStyle::Type::None) {
        cr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);
        apply_fill(cr, obj.fill);
        cr->fill();
        cr->set_fill_rule(Cairo::Context::FillRule::WINDING);
    } else {
        cr->begin_new_path();
    }
    if (obj.stroke.paint.type != FillStyle::Type::None) {
        for (const auto& child : obj.children) {
            if (child->type != SceneNode::Type::Path || !child->path) continue;
            BezierPath bp = BezierPath::from_path_data(*child->path);
            bp.apply_to_cairo(cr);
            apply_stroke(cr, obj.stroke);
            cr->stroke();
        }
    }
}

static void render_group(const Cairo::RefPtr<Cairo::Context>& cr, const SceneNode& obj) {
    cr->save();
    if (obj.opacity < 0.999) cr->push_group();
    for (int i = (int)obj.children.size() - 1; i >= 0; --i) {
        const SceneNode& child = *obj.children[i];
        if (!child.visible) continue;
        render_object(cr, child);
    }
    if (obj.opacity < 0.999) {
        cr->pop_group_to_source();
        cr->paint_with_alpha(obj.opacity);
    }
    cr->restore();
}

static void render_text(const Cairo::RefPtr<Cairo::Context>& cr, const SceneNode& obj) {
    if (obj.text_content.empty()) return;

    cr->save();
    cr->translate(obj.text_x, obj.text_y);
    apply_fill(cr, obj.fill);

    PangoLayout* layout = pango_cairo_create_layout(cr->cobj());
    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, obj.text_font_family.c_str());
    pango_font_description_set_absolute_size(desc, obj.text_font_size * PANGO_SCALE);
    if (obj.text_bold)   pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    if (obj.text_italic) pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, obj.text_content.c_str(), -1);

    // Anchor offset
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, nullptr, &logical);
    double off_x = 0.0;
    if (obj.text_anchor == "middle") off_x = -logical.width * 0.5;
    if (obj.text_anchor == "end")    off_x = -logical.width;

    int baseline = pango_layout_get_baseline(layout);
    double base_px = baseline / (double)PANGO_SCALE;

    cr->move_to(off_x, -base_px);
    pango_cairo_show_layout(cr->cobj(), layout);
    g_object_unref(layout);
    cr->restore();
}

// ── Image rendering ──────────────────────────────────────────────────────────
// Concept: an Image node is a raster placed at (image_x, image_y) with size
// (image_w × image_h) in document units. Loading is a one-shot from
// image_path on each render call — there's no caching layer here because
// PNG export is a transient operation, not a hot path.
//
// PNG inputs use Cairo's native loader; everything else (jpg/gif/webp/…)
// goes through GdkPixbuf and is blitted into an ARGB32 surface so the
// downstream paint pipeline is uniform. Mirrors Canvas::draw_object's
// image branch — the source-of-truth for on-screen rendering — so the
// PNG export visually matches what the user sees.
//
// Transform (a/b/c/d 2x2) applies around the image centre, matching the
// Canvas convention. Failed loads emit no fallback marker in export — a
// red-X placeholder belongs on-canvas (designer feedback) but would be
// noise in a final exported file. Silent skip is the right call.
static void render_image(const Cairo::RefPtr<Cairo::Context>& cr, const SceneNode& obj) {
    const double dx = obj.image_x;
    const double dy = obj.image_y;
    const double dw = obj.image_w;
    const double dh = obj.image_h;
    if (dw < 0.01 || dh < 0.01 || obj.image_path.empty()) return;

    Cairo::RefPtr<Cairo::ImageSurface> img_surf;
    try {
        auto dot = obj.image_path.rfind('.');
        std::string ext = (dot == std::string::npos)
            ? std::string()
            : obj.image_path.substr(dot + 1);
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
                gdk_cairo_set_source_pixbuf(cr2->cobj(), pb->gobj(), 0, 0);
                cr2->paint();
                img_surf = surf2;
            }
        }
    } catch (...) {
        LOG_WARN("PngExporter: failed to load image '{}'", obj.image_path);
        return;
    }
    if (!img_surf) return;

    int iw = img_surf->get_width();
    int ih = img_surf->get_height();
    if (iw <= 0 || ih <= 0) return;

    cr->save();

    // Apply 2x2 transform around image centre (matches Canvas).
    const Transform& t = obj.transform;
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

    cr->translate(dx, dy);
    cr->scale(dw / iw, dh / ih);
    cr->set_source(img_surf, 0, 0);
    cr->paint();
    cr->restore();
}

static void render_object(const Cairo::RefPtr<Cairo::Context>& cr, const SceneNode& obj) {
    if (obj.is_ref() || obj.is_guide()) return;
    cr->save();
    if (obj.opacity < 0.999 &&
        obj.type != SceneNode::Type::Group)  // groups handle opacity themselves
    {
        cr->push_group();
    }
    switch (obj.type) {
        case SceneNode::Type::Path:     render_path_obj(cr, obj); break;
        case SceneNode::Type::Compound: render_compound(cr, obj); break;
        case SceneNode::Type::Group:    render_group(cr, obj);    break;
        case SceneNode::Type::Text:     render_text(cr, obj);     break;
        case SceneNode::Type::Image:    render_image(cr, obj);    break;
        default: break;
    }
    if (obj.opacity < 0.999 && obj.type != SceneNode::Type::Group) {
        cr->pop_group_to_source();
        cr->paint_with_alpha(obj.opacity);
    }
    cr->restore();
}

// ── Surface render ────────────────────────────────────────────────────────────

static Cairo::RefPtr<Cairo::ImageSurface> render_to_surface(
        const CurvzDocument& doc, int size_px) {

    auto surface = Cairo::ImageSurface::create(
        Cairo::ImageSurface::Format::ARGB32, size_px, size_px);
    auto cr = Cairo::Context::create(surface);

    // Transparent background
    cr->set_operator(Cairo::Context::Operator::CLEAR);
    cr->paint();
    cr->set_operator(Cairo::Context::Operator::OVER);

    const int cw = doc.canvas_width();
    const int ch = doc.canvas_height();

    // Scale to fit the target square with aspect ratio preserved
    double scale = std::min((double)size_px / cw, (double)size_px / ch);
    double tx = (size_px - cw * scale) * 0.5;
    double ty = (size_px - ch * scale) * 0.5;

    cr->translate(tx, ty);
    cr->scale(scale, scale);

    // Layer z-order matches Canvas::draw_objects: paint layers[0] first
    // (bottom of z-order, last in panel) and layers[n-1] last (top of
    // z-order, first in panel). Cairo is last-painted-wins so iteration
    // is ascending. Children iterate descending — children[size-1] is
    // the bottom of within-layer z-order.
    for (int li = 0; li < (int)doc.layers.size(); ++li) {
        const SceneNode& layer = *doc.layers[li];
        if (layer.is_guide_layer() || layer.is_ref_layer()) continue;
        if (!layer.visible) continue;
        for (int oi = (int)layer.children.size() - 1; oi >= 0; --oi) {
            const SceneNode& obj = *layer.children[oi];
            if (!obj.visible) continue;
            render_object(cr, obj);
        }
    }

    surface->flush();
    return surface;
}

// ── PNG write callback ────────────────────────────────────────────────────────

static cairo_status_t write_to_vector(void* closure,
                                       const unsigned char* data,
                                       unsigned int length) {
    auto* vec = static_cast<std::vector<unsigned char>*>(closure);
    vec->insert(vec->end(), data, data + length);
    return CAIRO_STATUS_SUCCESS;
}

// ── Public API ────────────────────────────────────────────────────────────────

std::vector<unsigned char> render_png(const CurvzDocument& doc, int size_px) {
    auto surface = render_to_surface(doc, size_px);
    std::vector<unsigned char> result;
    cairo_surface_write_to_png_stream(surface->cobj(), write_to_vector, &result);
    return result;
}

bool export_png(const CurvzDocument& doc, const std::string& path, int size_px) {
    auto surface = render_to_surface(doc, size_px);
    cairo_status_t status = cairo_surface_write_to_png(surface->cobj(), path.c_str());
    if (status != CAIRO_STATUS_SUCCESS) {
        LOG_ERROR("PngExporter: failed to write '{}' ({})", path,
                  cairo_status_to_string(status));
        return false;
    }
    LOG_DEBUG("PngExporter: wrote {}px PNG '{}'", size_px, path);
    return true;
}

// ── S104 m1 — non-square aspect-preserving renderer ──────────────────────────
//
// Differs from render_to_surface() in three ways:
//   1. Surface is width_px × height_px (caller-chosen, may be non-square).
//   2. The doc is scaled to FILL the surface — there is no letterboxing.
//      The caller is expected to pick width_px / height_px so the doc's
//      aspect ratio is preserved (i.e. height_px = width_px * ch/cw, or
//      vice-versa). If the caller picks dimensions whose ratio doesn't
//      match the doc's aspect, the output gets stretched. That's a caller
//      bug, not this function's problem.
//   3. No translation — origin is (0, 0). With matched aspect the doc
//      lands flush against the surface edges.
//
// Otherwise identical: transparent background, currentColor→black, layers
// drawn bottom-up, guide/ref layers skipped, hidden layers/objects skipped.
static Cairo::RefPtr<Cairo::ImageSurface> render_to_surface_sized(
        const CurvzDocument& doc, int width_px, int height_px) {

    auto surface = Cairo::ImageSurface::create(
        Cairo::ImageSurface::Format::ARGB32, width_px, height_px);
    auto cr = Cairo::Context::create(surface);

    // Transparent background
    cr->set_operator(Cairo::Context::Operator::CLEAR);
    cr->paint();
    cr->set_operator(Cairo::Context::Operator::OVER);

    const int cw = doc.canvas_width();
    const int ch = doc.canvas_height();

    // Independent X/Y scale — caller picks dimensions; with matched
    // aspect these are equal and the doc fills the surface flush.
    double sx = (double)width_px  / cw;
    double sy = (double)height_px / ch;

    cr->scale(sx, sy);

    // Layer z-order matches Canvas::draw_objects — see render_to_surface()
    // for the full reasoning. Layers ascending (last-painted-wins), children
    // descending (children[size-1] is bottom of within-layer z-order).
    for (int li = 0; li < (int)doc.layers.size(); ++li) {
        const SceneNode& layer = *doc.layers[li];
        if (layer.is_guide_layer() || layer.is_ref_layer()) continue;
        if (!layer.visible) continue;
        for (int oi = (int)layer.children.size() - 1; oi >= 0; --oi) {
            const SceneNode& obj = *layer.children[oi];
            if (!obj.visible) continue;
            render_object(cr, obj);
        }
    }

    surface->flush();
    return surface;
}

bool export_png_sized(const CurvzDocument& doc,
                      const std::string& path,
                      int width_px,
                      int height_px) {
    if (width_px <= 0 || height_px <= 0) {
        LOG_ERROR("PngExporter: invalid dimensions {}x{} for '{}'",
                  width_px, height_px, path);
        return false;
    }
    auto surface = render_to_surface_sized(doc, width_px, height_px);
    cairo_status_t status = cairo_surface_write_to_png(surface->cobj(), path.c_str());
    if (status != CAIRO_STATUS_SUCCESS) {
        LOG_ERROR("PngExporter: failed to write '{}' ({})", path,
                  cairo_status_to_string(status));
        return false;
    }
    LOG_DEBUG("PngExporter: wrote {}x{} PNG '{}'", width_px, height_px, path);
    return true;
}

// ── Template thumbnail renderer (S63 M4c — DEFAULT-LIKE PREVIEW) ────────────
//
// Thumb should read as a miniature of what the document will look like when
// instantiated, matching the Default template's on-canvas appearance:
//   • Gray canvas background
//   • Light-gray grid lines at the proportional spacing
//   • Red margin rectangle at the proportional inset
// Aspect-preserving letterbox, transparent around the canvas.
//
// Grid + margin colors match the SceneNode defaults used by Canvas's
// draw_grid_doc_space / draw_margin_doc_space:
//   grid:   (0.5, 0.5, 0.8, a) — bluish, low alpha by default
//   margin: (0.8, 0.3, 0.3, a) — pale red
// Alphas pushed higher than on-canvas so the 128×128 thumb reads clearly.
//
// All drawing is in thumb-pixel space. Grid step = canvas_short_axis /
// grid_divisions, scaled by the letterbox scale.
//
// Per-step LOG_INFO so any crash or no-op is visible in curvz.log.

static Cairo::RefPtr<Cairo::ImageSurface> render_template_thumb_surface(
        const CurvzDocument& doc, int size_px,
        const std::string& name_label,
        const std::string& dimensions_label,
        int grid_divisions,
        double margin_ratio,
        double grid_offset_ratio) {

    (void)name_label; (void)dimensions_label;

    const int cw = doc.canvas_width();
    const int ch = doc.canvas_height();
    LOG_INFO("thumb ENTER size={} cw={} ch={} div={} mr={:.3f} off={:.3f}",
             size_px, cw, ch, grid_divisions, margin_ratio, grid_offset_ratio);

    auto surface = Cairo::ImageSurface::create(
        Cairo::ImageSurface::Format::ARGB32, size_px, size_px);
    if (!surface) {
        LOG_ERROR("thumb surface create FAILED");
        return surface;
    }
    auto cr = Cairo::Context::create(surface);

    // Transparent background.
    cr->set_operator(Cairo::Context::Operator::CLEAR);
    cr->paint();
    cr->set_operator(Cairo::Context::Operator::OVER);

    if (cw <= 0 || ch <= 0) {
        LOG_WARN("thumb invalid canvas {}x{} — blank", cw, ch);
        return surface;
    }

    // Canvas rect: letterbox at 3% of thumb size, aspect preserved.
    const double pad    = size_px * 0.03;
    const double avail  = (double)size_px - 2.0 * pad;
    const double scale  = std::min(avail / cw, avail / ch);
    const double rect_w = cw * scale;
    const double rect_h = ch * scale;
    const double rect_x = (size_px - rect_w) * 0.5;
    const double rect_y = (size_px - rect_h) * 0.5;
    LOG_INFO("thumb geom rect=({:.1f},{:.1f} {:.1f}x{:.1f}) scale={:.4f}",
             rect_x, rect_y, rect_w, rect_h, scale);

    // 1. Canvas fill — #2E2E2E, color-grabbed from the Default template's
    //    canvas render. Keeps thumbs visually consistent with what the
    //    instantiated doc looks like in the app.
    cr->set_source_rgba(0x2E / 255.0, 0x2E / 255.0, 0x2E / 255.0, 1.0);
    cr->rectangle(rect_x, rect_y, rect_w, rect_h);
    cr->fill();
    LOG_INFO("thumb step 1: canvas fill done");

    // 2. Grid lines — bluish (SceneNode defaults 0.5, 0.5, 0.8). Alpha 0.45
    //    works well against the dark canvas; the hue still reads blue even
    //    at low opacity. Line width 1.4 at 256px.
    if (grid_divisions > 0) {
        const double doc_short_px = std::min(rect_w, rect_h);
        const double step         = doc_short_px / (double)grid_divisions;
        LOG_INFO("thumb step 2: grid step={:.3f}", step);
        if (step >= 1.5) {
            const double off = step * grid_offset_ratio;
            cr->set_source_rgba(0.55, 0.55, 0.85, 0.45);
            cr->set_line_width(1.4);
            // Vertical lines
            for (double x = rect_x + off; x <= rect_x + rect_w + 0.1; x += step) {
                cr->move_to(x, rect_y);
                cr->line_to(x, rect_y + rect_h);
                cr->stroke();
            }
            // Horizontal lines
            for (double y = rect_y + off; y <= rect_y + rect_h + 0.1; y += step) {
                cr->move_to(rect_x, y);
                cr->line_to(rect_x + rect_w, y);
                cr->stroke();
            }
            LOG_INFO("thumb step 2: grid lines drawn");
        } else {
            LOG_INFO("thumb step 2: grid step too small ({:.2f}) — skipped", step);
        }
    }

    // 3. Margin rectangle — red (SceneNode defaults 0.8, 0.3, 0.3). Slightly
    //    lighter + moderate alpha to read against the dark canvas without
    //    being harsh.
    if (margin_ratio > 0.0 && grid_divisions > 0) {
        const double doc_short_px = std::min(rect_w, rect_h);
        const double step         = doc_short_px / (double)grid_divisions;
        const double m            = step * margin_ratio;
        LOG_INFO("thumb step 3: margin inset={:.3f}", m);
        if (m >= 1.0 && (rect_w - 2 * m) > 2.0 && (rect_h - 2 * m) > 2.0) {
            cr->set_source_rgba(0.85, 0.35, 0.35, 0.80);
            cr->set_line_width(2.0);
            cr->rectangle(rect_x + m, rect_y + m,
                          rect_w - 2 * m, rect_h - 2 * m);
            cr->stroke();
            LOG_INFO("thumb step 3: margin drawn");
        } else {
            LOG_INFO("thumb step 3: margin too tight — skipped");
        }
    }

    // 4. Canvas border — light gray so it's visible against the dark fill
    //    without being harsh. 1.5px at 256 stays crisp post-downscale.
    cr->set_source_rgba(0.55, 0.55, 0.55, 0.9);
    cr->set_line_width(1.5);
    cr->rectangle(rect_x + 0.75, rect_y + 0.75, rect_w - 1.5, rect_h - 1.5);
    cr->stroke();
    LOG_INFO("thumb step 4: border drawn");

    surface->flush();

    LOG_INFO("thumb EXIT ok");
    return surface;
}

bool export_template_thumbnail(const CurvzDocument& doc,
                               const std::string& path,
                               int size_px,
                               const std::string& name_label,
                               const std::string& dimensions_label,
                               int grid_divisions,
                               double margin_ratio,
                               double grid_offset_ratio) {
    auto surface = render_template_thumb_surface(
        doc, size_px, name_label, dimensions_label,
        grid_divisions, margin_ratio, grid_offset_ratio);
    cairo_status_t status = cairo_surface_write_to_png(surface->cobj(), path.c_str());
    if (status != CAIRO_STATUS_SUCCESS) {
        LOG_ERROR("PngExporter: failed to write template thumb '{}' ({})",
                  path, cairo_status_to_string(status));
        return false;
    }
    LOG_DEBUG("PngExporter: wrote {}px template thumb '{}'", size_px, path);
    return true;
}

} // namespace Curvz
