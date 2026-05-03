#include "SvgOptimiser.hpp"
#include "CurvzLog.hpp"
#include <sstream>
#include <cmath>
#include <clocale>

namespace Curvz {

static std::string fmt(double v) {
    // Force C locale for number formatting — %.6g in a non-C locale can emit
    // thousands separators (e.g. 1000 → "1,000") which is invalid in SVG paths.
    char buf[32];
    const char* old_locale = std::setlocale(LC_NUMERIC, nullptr);
    std::setlocale(LC_NUMERIC, "C");
    snprintf(buf, sizeof(buf), "%.6g", v);
    std::setlocale(LC_NUMERIC, old_locale);
    return buf;
}

static std::string fill_attr(const FillStyle& f) {
    switch (f.type) {
        case FillStyle::Type::None:         return "none";
        case FillStyle::Type::CurrentColor: return "currentColor";
        case FillStyle::Type::Solid: {
            char buf[8];
            snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                     (int)std::round(f.r * 255),
                     (int)std::round(f.g * 255),
                     (int)std::round(f.b * 255));
            return buf;
        }
        case FillStyle::Type::LinearGradient:
        case FillStyle::Type::RadialGradient: {
            // S90 Stage 1: gradient SVG round-trip lands in Stage 2. For now
            // we degrade to the first stop's flat colour on emit so the
            // optimiser still produces valid SVG. Stage 2 will replace this
            // with a proper <linearGradient>/<radialGradient> defs entry +
            // url(#...) reference.
            double r = f.r, g = f.g, b = f.b;
            if (!f.stops.empty()) {
                r = f.stops.front().r;
                g = f.stops.front().g;
                b = f.stops.front().b;
            }
            char buf[8];
            snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                     (int)std::round(r * 255),
                     (int)std::round(g * 255),
                     (int)std::round(b * 255));
            return buf;
        }
    }
    return "none";
}

static std::string stroke_attrs(const StrokeStyle& s) {
    if (s.paint.type == FillStyle::Type::None) return "";
    std::string out;
    out += " stroke=\"" + fill_attr(s.paint) + "\"";
    out += " stroke-width=\"" + fmt(s.width) + "\"";
    if (s.cap == LineCap::Round)        out += " stroke-linecap=\"round\"";
    else if (s.cap == LineCap::Square)  out += " stroke-linecap=\"square\"";
    if (s.join == LineJoin::Round)      out += " stroke-linejoin=\"round\"";
    else if (s.join == LineJoin::Bevel) out += " stroke-linejoin=\"bevel\"";
    if (s.opacity < 0.999)
        out += " stroke-opacity=\"" + fmt(s.opacity) + "\"";
    return out;
}

static std::string build_d(const PathData& pd) {
    if (pd.nodes.empty()) return "";
    std::ostringstream d;
    d << "M " << fmt(pd.nodes[0].x) << " " << fmt(pd.nodes[0].y);
    int n = (int)pd.nodes.size();
    int segs = pd.closed ? n : n - 1;
    for (int i = 0; i < segs; ++i) {
        const BezierNode& a = pd.nodes[i];
        const BezierNode& b = pd.nodes[(i + 1) % n];
        bool a_degen = (std::abs(a.cx2 - a.x) < 1e-6 && std::abs(a.cy2 - a.y) < 1e-6);
        bool b_degen = (std::abs(b.cx1 - b.x) < 1e-6 && std::abs(b.cy1 - b.y) < 1e-6);
        if (a_degen && b_degen)
            d << " L " << fmt(b.x) << " " << fmt(b.y);
        else
            d << " C " << fmt(a.cx2) << " " << fmt(a.cy2)
              << " "   << fmt(b.cx1) << " " << fmt(b.cy1)
              << " "   << fmt(b.x)   << " " << fmt(b.y);
    }
    if (pd.closed) d << " Z";
    return d.str();
}

static void emit_object(std::ostringstream& out, const SceneNode& obj, int indent);

static void emit_path(std::ostringstream& out, const SceneNode& obj, int indent) {
    if (!obj.path || obj.path->nodes.empty()) return;
    std::string d = build_d(*obj.path);
    if (d.empty()) return;
    std::string pad(indent * 2, ' ');
    out << pad << "<path"
        << " fill=\"" << fill_attr(obj.fill) << "\""
        << stroke_attrs(obj.stroke)
        << " d=\"" << d << "\"";
    if (obj.opacity < 0.999) out << " opacity=\"" << fmt(obj.opacity) << "\"";
    out << "/>\n";
}

static void emit_compound(std::ostringstream& out, const SceneNode& obj, int indent) {
    std::ostringstream d;
    for (const auto& child : obj.children) {
        if (child->type != SceneNode::Type::Path || !child->path) continue;
        std::string child_d = build_d(*child->path);
        if (child_d.empty()) continue;
        if (d.tellp() > 0) d << " ";
        d << child_d;
    }
    if (d.tellp() == 0) return;
    // Use the compound node's own fill/stroke if set; fall back to first child.
    // Children of a compound typically have no independent fill set.
    const SceneNode* style_src = &obj;
    if (style_src->fill.type == FillStyle::Type::None && !obj.children.empty())
        style_src = obj.children.front().get();
    std::string pad(indent * 2, ' ');
    out << pad << "<path"
        << " fill-rule=\"evenodd\""
        << " fill=\"" << fill_attr(style_src->fill) << "\""
        << stroke_attrs(style_src->stroke)
        << " d=\"" << d.str() << "\"";
    if (obj.opacity < 0.999) out << " opacity=\"" << fmt(obj.opacity) << "\"";
    out << "/>\n";
}

static void emit_group(std::ostringstream& out, const SceneNode& obj, int indent) {
    std::string pad(indent * 2, ' ');
    out << pad << "<g";
    if (obj.opacity < 0.999) out << " opacity=\"" << fmt(obj.opacity) << "\"";
    out << ">\n";
    for (int i = (int)obj.children.size() - 1; i >= 0; --i) {
        const SceneNode& child = *obj.children[i];
        if (!child.visible) continue;
        emit_object(out, child, indent + 1);
    }
    out << pad << "</g>\n";
}

static void emit_text(std::ostringstream& out, const SceneNode& obj, int indent) {
    auto xml_escape = [](const std::string& s) {
        std::string r;
        for (char c : s) {
            if      (c == '&') r += "&amp;";
            else if (c == '<') r += "&lt;";
            else if (c == '>') r += "&gt;";
            else if (c == '"') r += "&quot;";
            else               r += c;
        }
        return r;
    };
    std::string pad(indent * 2, ' ');
    out << pad << "<text"
        << " x=\"" << fmt(obj.text_x) << "\""
        << " y=\"" << fmt(obj.text_y) << "\""
        << " font-family=\"" << obj.text_font_family << "\""
        << " font-size=\"" << fmt(obj.text_font_size) << "\"";
    if (obj.text_bold)   out << " font-weight=\"bold\"";
    if (obj.text_italic) out << " font-style=\"italic\"";
    if (obj.text_anchor != "start")
        out << " text-anchor=\"" << obj.text_anchor << "\"";
    out << " fill=\"" << fill_attr(obj.fill) << "\"";
    if (obj.stroke.paint.type != FillStyle::Type::None)
        out << stroke_attrs(obj.stroke);
    if (obj.opacity < 0.999) out << " opacity=\"" << fmt(obj.opacity) << "\"";
    out << ">" << xml_escape(obj.text_content) << "</text>\n";
}

static void emit_object(std::ostringstream& out, const SceneNode& obj, int indent) {
    if (obj.is_ref()) return;  // editor-only, skip in export
    switch (obj.type) {
        case SceneNode::Type::Path:     emit_path(out, obj, indent);     break;
        case SceneNode::Type::Compound: emit_compound(out, obj, indent); break;
        case SceneNode::Type::Group:    emit_group(out, obj, indent);    break;
        case SceneNode::Type::Text:     emit_text(out, obj, indent);     break;
        default: break;
    }
}

std::string optimise_svg(const CurvzDocument& doc) {
    std::ostringstream out;
    const int cw = doc.canvas_width();
    const int ch = doc.canvas_height();

    // viewBox preserves the original canvas coordinate space — path data
    // coordinates are left untouched.  width/height are set to the freedesktop
    // scalable icon convention (48×48 nominal, square).  For non-square canvases
    // the viewBox aspect ratio is preserved; renderers letterbox automatically.
    // GTK's icon theme loader ignores width/height and uses viewBox exclusively.
    // snprintf %d bypasses locale — << stream is locale-sensitive for integers.
    char vb[64];
    snprintf(vb, sizeof(vb), "0 0 %d %d", cw, ch);
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\""
        << " viewBox=\"" << vb << "\""
        << " width=\"48\""
        << " height=\"48\""
        << " color=\"black\""   // currentColor fallback for standalone viewers (Inkscape etc.)
        << ">\n";

    // Bottom-up layer order matches Canvas::draw_objects
    for (int li = (int)doc.layers.size() - 1; li >= 0; --li) {
        const SceneNode& layer = *doc.layers[li];
        if (layer.is_guide_layer() || layer.is_ref_layer()) continue;
        if (!layer.visible) continue;
        for (int oi = (int)layer.children.size() - 1; oi >= 0; --oi) {
            const SceneNode& obj = *layer.children[oi];
            if (!obj.visible) continue;
            emit_object(out, obj, 1);
        }
    }

    out << "</svg>\n";
    return out.str();
}

} // namespace Curvz
