#include "SvgWriter.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"
#include "TextCursor.hpp"  // compute_text_layout — for TextBox per-baseline emit
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cmath>
#include <clocale>
#include <cstring>
#include <map>
#include <vector>
#include <algorithm>
#include <pango/pango.h>  // s325 m3 — PANGO_ATTR_* / PANGO_STYLE_* for markup encode

namespace Curvz {

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string fmt2(double v) {
    // Force C locale — %.6g in a non-C locale emits thousands separators
    // (e.g. 1000 → "1,000") which is invalid in SVG path data.
    char buf[32];
    const char* old = std::setlocale(LC_NUMERIC, nullptr);
    std::setlocale(LC_NUMERIC, "C");
    snprintf(buf, sizeof(buf), "%.6g", v);
    std::setlocale(LC_NUMERIC, old);
    return buf;
}

// 6 decimal places — used for guide positions where sub-unit precision matters.
// Same locale guard as fmt2: %.6f in a non-C locale emits a comma decimal or
// thousands separators (e.g. fr_FR → "1234,567890"), which is invalid in SVG
// attribute values.
static std::string fmt6(double v) {
    char buf[32];
    const char* old = std::setlocale(LC_NUMERIC, nullptr);
    std::setlocale(LC_NUMERIC, "C");
    snprintf(buf, sizeof(buf), "%.6f", v);
    std::setlocale(LC_NUMERIC, old);
    return buf;
}

// ── S96 m2 — SVG id emission ─────────────────────────────────────────────────
//
// The SVG `id` is a derived, write-only handle constructed from a node's
// human-readable name and its internal UUID. Source of truth for both
// remains the in-memory SceneNode fields (`name` and `internal_id`),
// round-tripped via data-curvz-name and data-curvz-iid respectively.
//
// Two thin wrappers below let every emission site stay a one-liner. The
// "_str" form returns a ready-to-concat attribute (or empty); the "_emit"
// form streams it. Both delegate to curvz::utils::encode_svg_id.
static std::string id_attr_str(const std::string& name,
                               const std::string& iid) {
    const std::string encoded = curvz::utils::encode_svg_id(name, iid);
    return encoded.empty() ? std::string()
                           : " id=\"" + encoded + "\"";
}
template <typename Obj>
static std::string id_attr_str(const Obj& obj) {
    return id_attr_str(obj.name, obj.internal_id);
}

// ── S90 Stage 2 — gradient defs collector ────────────────────────────────────
// A single SvgWrite walks the tree twice: once to collect every gradient
// FillStyle (fill + stroke.paint slots, recursively through Compounds and
// Groups), and once to emit. Each unique gradient gets a minted id like
// "grad1", "grad2", emitted inside <defs>. Shapes reference the gradient
// via fill="url(#gradN)" — fill_attr() looks up the active collector.
//
// Identity is by pointer (FillStyle*). All FillStyles live in the doc
// tree owned by SceneNodes which don't move during a write, so pointer
// identity is stable for the duration of the call. If the SAME stops/
// geometry appears on N shapes we'll mint N defs — dedup is a Stage-3+
// optimisation; for now repeated-same-gradient is a writer cost only and
// every renderer handles it.
struct GradientCollector {
    // Insertion order = emission order; pointer → minted-id lookup.
    std::vector<const FillStyle*> entries;
    std::map<const FillStyle*, std::string> id_map;
    int next_idx = 1;

    void add(const FillStyle* fs) {
        if (!fs || !fs->is_gradient()) return;
        if (id_map.count(fs)) return;
        std::string id = "grad" + std::to_string(next_idx++);
        id_map[fs] = id;
        entries.push_back(fs);
    }

    // Returns minted id for fs, or empty string if not collected.
    std::string lookup(const FillStyle* fs) const {
        auto it = id_map.find(fs);
        return (it == id_map.end()) ? std::string() : it->second;
    }
};

// File-scope active collector. Set on entry to write_svg, cleared on exit.
// This avoids threading a new parameter through write_object and every
// helper that calls fill_attr — there are too many sites and the change
// would be mostly noise. Safe because write_svg isn't reentrant or
// concurrent in practice.
static const GradientCollector* g_grad_collector = nullptr;

// Recursive walk: collect every gradient FillStyle in fill + stroke.paint
// slots. Used as the pre-pass at the top of write_svg.
static void collect_gradients(const SceneNode& n, GradientCollector& gc) {
    gc.add(&n.fill);
    gc.add(&n.stroke.paint);
    for (const auto& c : n.children)
        if (c) collect_gradients(*c, gc);
    if (n.clip_shape) collect_gradients(*n.clip_shape, gc);
    if (n.blend_source_a) collect_gradients(*n.blend_source_a, gc);
    if (n.blend_source_b) collect_gradients(*n.blend_source_b, gc);
    if (n.warp_source) collect_gradients(*n.warp_source, gc);
}

// Emit a single gradient FillStyle into <defs> at the given indent.
// Format follows SVG canonicals: gradientUnits="objectBoundingBox" (the
// only mode S90 Stage 2 ships), spreadMethod="pad" (Cairo default;
// reflect/repeat is a Stage-3 follow-up tied to the gradient editor).
static void write_gradient_def(std::ostringstream& out,
                                const std::string& id,
                                const FillStyle& f,
                                int indent) {
    std::string pad(indent * 2, ' ');
    // Hex colour helper for stop-color.
    auto hex = [](double r, double g, double b) {
        char buf[8];
        snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                 (int)std::round(r * 255),
                 (int)std::round(g * 255),
                 (int)std::round(b * 255));
        return std::string(buf);
    };
    if (f.type == FillStyle::Type::LinearGradient) {
        out << pad << "<linearGradient id=\"" << id << "\""
            << " gradientUnits=\"objectBoundingBox\""
            << " x1=\"" << fmt2(f.g_x1) << "\""
            << " y1=\"" << fmt2(f.g_y1) << "\""
            << " x2=\"" << fmt2(f.g_x2) << "\""
            << " y2=\"" << fmt2(f.g_y2) << "\">\n";
    } else { // RadialGradient
        // SVG radial: cx/cy/r are the outer circle, fx/fy the focal point.
        // Curvz convention: g_x1/g_y1 are the focal (fx,fy), g_x2/g_y2 are
        // the centre (cx,cy), g_r is the radius. Match that mapping on emit.
        out << pad << "<radialGradient id=\"" << id << "\""
            << " gradientUnits=\"objectBoundingBox\""
            << " cx=\"" << fmt2(f.g_x2) << "\""
            << " cy=\"" << fmt2(f.g_y2) << "\""
            << " r=\""  << fmt2(f.g_r)  << "\""
            << " fx=\"" << fmt2(f.g_x1) << "\""
            << " fy=\"" << fmt2(f.g_y1) << "\">\n";
    }
    for (const auto& s : f.stops) {
        out << pad << "  <stop offset=\"" << fmt2(s.offset) << "\""
            << " stop-color=\"" << hex(s.r, s.g, s.b) << "\"";
        if (s.a < 0.999)
            out << " stop-opacity=\"" << fmt2(s.a) << "\"";
        out << "/>\n";
    }
    if (f.type == FillStyle::Type::LinearGradient)
        out << pad << "</linearGradient>\n";
    else
        out << pad << "</radialGradient>\n";
}

// ── S97 m1 — drop shadow defs collector ──────────────────────────────────────
// Shadows are a per-object effect (see SceneNode shadow_* fields). When a node
// has shadow_enabled, write_svg's pre-pass collects it into ShadowCollector,
// emits one <filter> element into <defs> per collected node, and the host
// node's open tag references it via filter="url(#sh_<short_iid>)". Same
// pattern as the gradient collector: pre-pass walk, defs-up emit, identifier
// looked up by pointer at the host emit site.
//
// Filter id format is "sh_" + short_iid (8 chars of the host's UUID). Short
// id keeps the attribute compact and is unique enough — collisions across a
// single document would require a UUID-prefix collision, which is roughly
// 1-in-2^32 even within the same doc. If a node has shadow_enabled but no
// internal_id (legacy load before iid was minted), we skip the filter — the
// host's iid is minted on load now (S96), so this is a defensive null-check.
struct ShadowCollector {
    std::vector<const SceneNode*> entries;
    std::map<const SceneNode*, std::string> id_map;

    void add(const SceneNode* n) {
        if (!n || !n->shadow_enabled) return;
        if (n->internal_id.empty()) return;
        if (id_map.count(n)) return;
        std::string id = "sh_" + curvz::utils::short_iid(n->internal_id);
        id_map[n] = id;
        entries.push_back(n);
    }

    std::string lookup(const SceneNode* n) const {
        auto it = id_map.find(n);
        return (it == id_map.end()) ? std::string() : it->second;
    }
};

// File-scope active collector — same threading approach as g_grad_collector.
// Set on entry to write_svg, cleared on exit.
static const ShadowCollector* g_shadow_collector = nullptr;

// Recursive walk: every node with shadow_enabled. The walker descends into the
// same alt slots as collect_gradients (clip_shape, blend sources, warp source)
// so a shadowed path nested inside any of them is still collected. Children
// are walked normally.
static void collect_shadows(const SceneNode& n, ShadowCollector& sc) {
    sc.add(&n);
    for (const auto& c : n.children)
        if (c) collect_shadows(*c, sc);
    if (n.clip_shape)     collect_shadows(*n.clip_shape, sc);
    if (n.blend_source_a) collect_shadows(*n.blend_source_a, sc);
    if (n.blend_source_b) collect_shadows(*n.blend_source_b, sc);
    if (n.warp_source)    collect_shadows(*n.warp_source, sc);
}

// Emit a single <filter> for one shadowed node into <defs> at the given
// indent. Standard SVG drop-shadow filter chain: source alpha → offset →
// gaussian blur → flood-fill with shadow colour → composite back under
// the original. filterUnits="userSpaceOnUse" so dx/dy/stdDeviation are
// in doc units and don't need bbox-relative remapping.
//
// x/y/width/height pad the filter region so the blur isn't clipped at the
// host bbox. We use a generous fixed pad (-50% / 200%) that covers the
// vast majority of real shadows; SVG renderers default to a similar value
// but emitting it explicitly avoids surprises in foreign tools that
// default differently.
static void write_shadow_filter(std::ostringstream& out,
                                 const std::string& id,
                                 const SceneNode& n,
                                 int indent) {
    std::string pad(indent * 2, ' ');
    // Hex colour helper — same as in write_gradient_def. Local copy keeps
    // this function self-contained and lets it move freely if we later
    // hoist the filter logic into its own translation unit.
    auto hex = [](double r, double g, double b) {
        char buf[8];
        snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                 (int)std::round(r * 255),
                 (int)std::round(g * 255),
                 (int)std::round(b * 255));
        return std::string(buf);
    };
    // Final shadow alpha is colour.a * shadow_opacity. Pre-multiply here so
    // the filter chain is a single flood + composite pair, rather than two
    // alpha multiplies.
    double final_a = std::max(0.0, std::min(1.0, n.shadow_color_a * n.shadow_opacity));

    out << pad << "<filter id=\"" << id << "\""
        << " filterUnits=\"userSpaceOnUse\""
        << " x=\"-50%\" y=\"-50%\" width=\"200%\" height=\"200%\">\n";
    // 1. Offset the source alpha by (dx, dy).
    out << pad << "  <feOffset in=\"SourceAlpha\""
        << " dx=\"" << fmt2(n.shadow_dx) << "\""
        << " dy=\"" << fmt2(n.shadow_dy) << "\""
        << " result=\"off\"/>\n";
    // 2. Blur it (stdDeviation in doc units, matches Cairo Gaussian sigma).
    out << pad << "  <feGaussianBlur in=\"off\""
        << " stdDeviation=\"" << fmt2(std::max(0.0, n.shadow_blur)) << "\""
        << " result=\"blur\"/>\n";
    // 3. Flood-fill with the shadow colour, then composite-in with the blur
    //    so the colour only shows where the blurred alpha is non-zero.
    out << pad << "  <feFlood flood-color=\"" << hex(n.shadow_color_r,
                                                       n.shadow_color_g,
                                                       n.shadow_color_b) << "\""
        << " flood-opacity=\"" << fmt2(final_a) << "\""
        << " result=\"flood\"/>\n";
    out << pad << "  <feComposite in=\"flood\" in2=\"blur\" operator=\"in\""
        << " result=\"shadow\"/>\n";
    // 4. Place the original artwork on top of the shadow. Painters-order
    //    via <feMerge>: shadow first (under), source on top.
    out << pad << "  <feMerge>\n";
    out << pad << "    <feMergeNode in=\"shadow\"/>\n";
    out << pad << "    <feMergeNode in=\"SourceGraphic\"/>\n";
    out << pad << "  </feMerge>\n";
    out << pad << "</filter>\n";
}

// Build the per-host attribute string for a shadowed node. Returns empty
// string when the node has no shadow (or no minted filter id, e.g. missing
// iid). Otherwise returns a leading-space string of the form:
//
//   filter="url(#sh_xxxx)" data-curvz-shadow-dx="..." data-curvz-shadow-dy="..."
//   data-curvz-shadow-blur="..." data-curvz-shadow-color="rrggbb"
//   data-curvz-shadow-color-a="..." data-curvz-shadow-opacity="..."
//
// The filter ref makes foreign renderers (browsers, Folio, Inkscape) draw
// the shadow correctly. The data-curvz-* attrs are the source of truth on
// Curvz round-trip — same dual-source pattern as data-curvz-name vs id.
static std::string shadow_attr_str(const SceneNode& obj) {
    if (!obj.shadow_enabled) return std::string();
    if (!g_shadow_collector) return std::string();
    const std::string fid = g_shadow_collector->lookup(&obj);
    if (fid.empty()) return std::string();
    auto hex = [](double r, double g, double b) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02x%02x%02x",
                 (int)std::round(r * 255),
                 (int)std::round(g * 255),
                 (int)std::round(b * 255));
        return std::string(buf);
    };
    std::ostringstream s;
    s << " filter=\"url(#" << fid << ")\""
      << " data-curvz-shadow=\"1\""
      << " data-curvz-shadow-dx=\"" << fmt2(obj.shadow_dx) << "\""
      << " data-curvz-shadow-dy=\"" << fmt2(obj.shadow_dy) << "\""
      << " data-curvz-shadow-blur=\"" << fmt2(obj.shadow_blur) << "\""
      << " data-curvz-shadow-color=\"" << hex(obj.shadow_color_r,
                                                obj.shadow_color_g,
                                                obj.shadow_color_b) << "\""
      << " data-curvz-shadow-color-a=\"" << fmt2(obj.shadow_color_a) << "\""
      << " data-curvz-shadow-opacity=\"" << fmt2(obj.shadow_opacity) << "\"";
    return s.str();
}

// ── encode_warp_envelope ──────────────────────────────────────────────────────
// Serialize an envelope PathData into a single attribute string.
// Format: per anchor, six doubles "x,y,cx1,cy1,cx2,cy2" comma-separated.
// Anchors separated by ";". Envelopes are always open — no closed flag
// encoded. Node types are always Smooth — not encoded.
// Example (2-anchor straight line): "0,0,0,0,100,0;300,0,200,0,300,0"
// Precision via fmt2 (%.6g — 6 significant digits, C locale).
// Empty or single-anchor envelopes encode as empty string; the parser
// treats empty as "no envelope → fall back to default identity".
static std::string encode_warp_envelope(const PathData& env) {
    if (env.nodes.size() < 2) return std::string();
    std::string s;
    for (size_t i = 0; i < env.nodes.size(); ++i) {
        const auto& n = env.nodes[i];
        if (i > 0) s += ';';
        s += fmt2(n.x);   s += ',';
        s += fmt2(n.y);   s += ',';
        s += fmt2(n.cx1); s += ',';
        s += fmt2(n.cy1); s += ',';
        s += fmt2(n.cx2); s += ',';
        s += fmt2(n.cy2);
    }
    return s;
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
            // S90 Stage 2: emit a url(#gradN) reference. The matching
            // <linearGradient>/<radialGradient> element is in <defs>,
            // emitted by write_svg's pre-pass against g_grad_collector.
            // If the collector somehow doesn't have us (shouldn't happen
            // since collect_gradients walks the same tree), degrade to
            // first-stop flat colour so the file is still valid SVG.
            if (g_grad_collector) {
                std::string id = g_grad_collector->lookup(&f);
                if (!id.empty()) return "url(#" + id + ")";
            }
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
    std::string out;
    out += " stroke=\"" + fill_attr(s.paint) + "\"";
    out += " stroke-width=\"" + fmt2(s.width) + "\"";
    if (s.cap == LineCap::Round)         out += " stroke-linecap=\"round\"";
    else if (s.cap == LineCap::Square)   out += " stroke-linecap=\"square\"";
    if (s.join == LineJoin::Round)       out += " stroke-linejoin=\"round\"";
    else if (s.join == LineJoin::Bevel)  out += " stroke-linejoin=\"bevel\"";
    if (s.opacity < 0.999)
        out += " stroke-opacity=\"" + fmt2(s.opacity) + "\"";
    return out;
}

// Swatch-binding sidecar attributes. Emits data-curvz-fill-swatch and/or
// data-curvz-stroke-swatch when the SceneNode has a non-empty binding id
// on that slot. These are the round-trip carriers for the Phase 5 / s70
// binding model: the id IS the paint, the fill=/stroke= SVG attrs are the
// resolved render cache. Readers unaware of the data-* attrs get a valid
// solid-colour SVG; Curvz on reload reconstructs the SwatchRef.
//
// Called from the same paint-bearing emit sites as fill_attr / stroke_attrs
// (Path/generic, Compound, Text) so the binding sits immediately next to
// the FillStyle cache it shadows. Empty string when no binding is set on
// either slot — zero cost for pre-S70 or never-bound objects.
static std::string swatch_binding_attrs(const GlyphObject& o) {
    std::string out;
    if (!o.fill_swatch_id.empty())
        out += " data-curvz-fill-swatch=\"" + o.fill_swatch_id + "\"";
    if (!o.stroke_swatch_id.empty())
        out += " data-curvz-stroke-swatch=\"" + o.stroke_swatch_id + "\"";
    return out;
}

// ── S77 m3c — style binding sidecar ──────────────────────────────────────────
// Round-trip carrier for SceneNode::bound_style. Same lossy-fallback model
// as the swatch bindings above: fill=/stroke= (and stroke-width, cap, join,
// etc.) are the resolved render cache, while data-curvz-style is the binding
// key. A non-Curvz reader sees a fully-styled SVG and silently loses the
// binding on round-trip; Curvz on reload re-materialises from the Style
// referenced by the id.
//
// Value shape: either "app:<slug>" (built-in tier) or "stl_<uuid>" (user
// tier). Both are valid SVG attribute content, no escaping needed.
//
// Emitted at the same three paint-bearing sites as swatch_binding_attrs
// (Path/generic, Compound, Text). Empty string when bound_style is empty —
// zero cost for never-bound objects, and pre-m3c projects naturally have
// nothing to lose.
//
// On the parser side this is read in apply_style_attrs (single anchor for
// every node-emit site). The post-load walk in CurvzProject::load runs
// materialise_from_style on every node with a non-empty bound_style and
// drops unknown ids — so a project saved with m3c and reopened with the
// referenced style still in the library re-projects fill/stroke from that
// Style, validating the binding survived the SVG hop.
static std::string style_binding_attr(const GlyphObject& o) {
    if (o.bound_style.empty()) return {};
    return " data-curvz-style=\"" + o.bound_style + "\"";
}

// ── TextBoxMgrCollector — s311 m1c-redux ─────────────────────────────────
// Dual-block disk format. Every TextBoxMgr in the document is emitted
// into <defs> as a non-rendering blob carrying the buffer, caret, font
// defaults, fill/stroke and (later) sub-manager state. The Mgr's canvas
// TextBoxView children emit into the layer body at the Mgr's layer
// position, each as a standalone <g data-curvz-textbox-view> carrying
// only identity (view-iid + mgr-iid back-pointer + view-index/count),
// boundary path, and the per-baseline render snapshot for foreign tools.
//
// m1c-redux keeps the in-memory shape unchanged: views remain children
// of the Mgr SceneNode. The writer walks the Mgr's children to find its
// views, and emits them at the Mgr's slot in the layer (no z-order
// interleaving with other layer children yet — that's a future
// in-memory refactor). What changes on disk: the Mgr's <g> wrapper
// goes away from the layer body and reappears in <defs>; the views
// emit as bare siblings, each tagged with the Mgr's iid.
//
// Collector mechanism mirrors GradientCollector / ShadowCollector. The
// file-scope pointer g_mgr_collector lets write_object's TextBoxMgr
// branch consult the collector for the dual-block defs lookup; in
// practice every Mgr in the tree gets collected, so the check is just
// defensive.
struct MgrCollector {
    std::vector<const SceneNode*> entries;
    std::map<std::string, const SceneNode*> by_iid;

    void add(const SceneNode* n) {
        if (!n || n->type != SceneNode::Type::TextBoxMgr) return;
        if (n->internal_id.empty()) return;
        if (by_iid.count(n->internal_id)) return;
        by_iid[n->internal_id] = n;
        entries.push_back(n);
    }
};

static const MgrCollector* g_mgr_collector = nullptr;

static void collect_mgrs(const SceneNode& n, MgrCollector& mc) {
    mc.add(&n);
    for (const auto& c : n.children)
        if (c) collect_mgrs(*c, mc);
    if (n.clip_shape)     collect_mgrs(*n.clip_shape, mc);
    if (n.blend_source_a) collect_mgrs(*n.blend_source_a, mc);
    if (n.blend_source_b) collect_mgrs(*n.blend_source_b, mc);
    if (n.warp_source)    collect_mgrs(*n.warp_source, mc);
}

// Emit one Mgr blob into <defs>. Buffer goes in CDATA so future Pango
// markup ("<span foreground='red'>word</span>") rides through cleanly
// without per-byte attribute escaping. Always emit CDATA even when the
// buffer is empty — its presence on disk says "this Mgr has a buffer
// slot", which is structural truth regardless of content.
// ── s325 m3 — markup encode (the per-run spine's write side) ────────────────
// Inverse of SvgParser::decode_markup_into. clean text_content + flat AttrSpan
// list -> Pango markup string. The "markup of unformatted text is just text"
// identity is load-bearing: zero spans returns the escaped buffer unchanged,
// so the marker can ride EVERY Mgr def and a plain box round-trips byte-for-
// byte through parse_markup. Pango parses markup but will not emit it, so this
// is the one hand-written half of the round-trip (the deferred-doubt piece).
//
// Overlapping spans are handled by segmenting at every span edge and emitting
// one merged <span> per segment carrying all attrs active over it (close-all /
// reopen-all). Not minimal markup, but valid and exactly round-tripping — the
// decode pump re-flattens abutting same-value runs if it ever matters.
static std::string encode_markup(const std::string& text,
                                 const std::vector<AttrSpan>& spans) {
    auto esc = [](const std::string& s, bool in_attr) {
        std::string r;
        for (char c : s) {
            if      (c == '&') r += "&amp;";
            else if (c == '<') r += "&lt;";
            else if (c == '>') r += "&gt;";
            else if (in_attr && c == '"') r += "&quot;";
            else r += c;
        }
        return r;
    };
    if (spans.empty())
        return esc(text, false);   // plain text is valid markup

    std::vector<size_t> bounds{0, text.size()};
    for (const auto& s : spans) {
        if (s.type == curvz::utils::kCurvzLeadingAttr) continue;  // s331 — not a Pango attr
        // s332 — per-run stroke private attrs aren't Pango markup either; skip
        // them from the bounds so they don't fragment the text into empty
        // spans. (Their own data-curvz-stroke-* persistence is a follow-up;
        // for now they're in-session only.)
        if (s.type == curvz::utils::kCurvzStrokeColorAttr ||
            s.type == curvz::utils::kCurvzStrokeWidthAttr) continue;
        if (s.type == curvz::utils::kCurvzAlignAttr) continue;  // s332 — not a Pango attr
        if (s.type == curvz::utils::kCurvzTabsAttr) continue;   // s335 — not a Pango attr
        if (s.start_byte <= text.size()) bounds.push_back(s.start_byte);
        if (s.end_byte   <= text.size()) bounds.push_back(s.end_byte);
    }
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    std::string out;
    for (size_t i = 0; i + 1 < bounds.size(); ++i) {
        size_t a = bounds[i], b = bounds[i + 1];
        if (b <= a) continue;
        std::string attrs;
        for (const auto& s : spans) {
            if ((size_t)s.start_byte > a || (size_t)s.end_byte < b) continue; // not covering
            switch ((PangoAttrType)s.type) {
                case PANGO_ATTR_WEIGHT:
                    attrs += " weight=\"" + std::to_string(s.ivalue) + "\"";
                    break;
                case PANGO_ATTR_STYLE:
                    attrs += std::string(" style=\"")
                           + (s.ivalue == PANGO_STYLE_ITALIC  ? "italic"
                            : s.ivalue == PANGO_STYLE_OBLIQUE ? "oblique"
                                                              : "normal")
                           + "\"";
                    break;
                case PANGO_ATTR_UNDERLINE:
                    attrs += std::string(" underline=\"")
                           + (s.ivalue == PANGO_UNDERLINE_SINGLE ? "single"
                            : s.ivalue == PANGO_UNDERLINE_DOUBLE ? "double"
                            : s.ivalue == PANGO_UNDERLINE_LOW    ? "low"
                            : s.ivalue == PANGO_UNDERLINE_ERROR  ? "error"
                                                                 : "none")
                           + "\"";
                    break;
                case PANGO_ATTR_STRIKETHROUGH:
                    attrs += std::string(" strikethrough=\"")
                           + (s.ivalue ? "true" : "false") + "\"";
                    break;
                case PANGO_ATTR_OVERLINE:
                    attrs += std::string(" overline=\"")
                           + (s.ivalue == PANGO_OVERLINE_SINGLE ? "single"
                                                                : "none")
                           + "\"";
                    break;
                case PANGO_ATTR_SIZE:
                case PANGO_ATTR_ABSOLUTE_SIZE:
                    // Emitted as markup `size` (re-decodes as PANGO_ATTR_SIZE);
                    // absolute vs point reconciliation is m2-panel work.
                    attrs += " size=\"" + std::to_string(s.ivalue) + "\"";
                    break;
                case PANGO_ATTR_LETTER_SPACING:
                    attrs += " letter_spacing=\"" + std::to_string(s.ivalue) + "\"";
                    break;
                case PANGO_ATTR_RISE:
                    attrs += " rise=\"" + std::to_string(s.ivalue) + "\"";
                    break;
                case PANGO_ATTR_FONT_SCALE:
                    attrs += std::string(" font_scale=\"")
                           + (s.ivalue == PANGO_FONT_SCALE_SUPERSCRIPT ? "superscript"
                            : s.ivalue == PANGO_FONT_SCALE_SUBSCRIPT   ? "subscript"
                                                                       : "none")
                           + "\"";
                    break;
                case PANGO_ATTR_FAMILY:
                    attrs += " font_family=\"" + esc(s.svalue, true) + "\"";
                    break;
                case PANGO_ATTR_FOREGROUND: {
                    static const char* hexd = "0123456789ABCDEF";
                    unsigned long v = (unsigned long)(s.ivalue & 0xFFFFFF);
                    std::string hx = "#";
                    for (int sh = 20; sh >= 0; sh -= 4) hx += hexd[(v >> sh) & 0xF];
                    attrs += " foreground=\"" + hx + "\"";
                    break;
                }
                default: break;  // unhandled-for-m1 type: dropped on encode
            }
        }
        std::string seg = esc(text.substr(a, b - a), false);
        if (attrs.empty()) out += seg;
        else                out += "<span" + attrs + ">" + seg + "</span>";
    }
    return out;
}

static void write_textbox_mgr_def(std::ostringstream& out,
                                  const SceneNode& mgr,
                                  int indent) {
    std::string pad(indent * 2, ' ');
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
    // CDATA escape: split any "]]>" so the section terminator can't
    // appear inside. Rare in real text but cheap insurance.
    auto cdata_escape = [](const std::string& s) {
        std::string r;
        size_t i = 0;
        while (i < s.size()) {
            if (i + 2 < s.size() && s[i] == ']' && s[i+1] == ']' && s[i+2] == '>') {
                r += "]]]]><![CDATA[>";
                i += 3;
            } else {
                r += s[i++];
            }
        }
        return r;
    };

    out << pad << "<g data-curvz-textbox-mgr=\"1\"";
    out << id_attr_str(mgr);
    if (!mgr.internal_id.empty())
        out << " data-curvz-iid=\"" << mgr.internal_id << "\"";
    if (!mgr.name.empty())
        out << " data-curvz-name=\"" << xml_escape(mgr.name) << "\"";
    if (!mgr.visible) out << " display=\"none\"";
    if (mgr.opacity < 0.999)
        out << " opacity=\"" << fmt2(mgr.opacity) << "\"";
    if (mgr.text_caret_byte > 0)
        out << " data-curvz-caret-byte=\"" << mgr.text_caret_byte << "\"";
    out << " data-curvz-font-family=\"" << xml_escape(mgr.text_font_family) << "\"";
    out << " data-curvz-font-size=\""   << fmt2(mgr.text_font_size) << "\"";
    // s326 m2b — explicit leading; only emitted when set (>0) so unmarked /
    //   pre-s326 boxes stay byte-identical and load with the derived 1.2x.
    if (mgr.text_line_height > 0.0)
        out << " data-curvz-line-height=\"" << fmt2(mgr.text_line_height) << "\"";
    // s331 — per-paragraph leading runs (kCurvzLeadingAttr). Can't ride the
    //   Pango span markup (not a Pango attr), so they persist as a flat
    //   "start:end:ivalue;..." list on the mgr (ivalue = doc-px x PANGO_SCALE).
    //   Only emitted when present so unmarked boxes stay byte-identical.
    {
        std::string lead;
        for (const auto& s : mgr.text_attr_spans) {
            if (s.type != curvz::utils::kCurvzLeadingAttr) continue;
            if (!lead.empty()) lead += ";";
            lead += std::to_string(s.start_byte) + ":" +
                    std::to_string(s.end_byte) + ":" +
                    std::to_string(s.ivalue);
        }
        if (!lead.empty())
            out << " data-curvz-leading=\"" << lead << "\"";
    }
    // s332 — per-run stroke colour + width runs (kCurvzStrokeColorAttr /
    //   kCurvzStrokeWidthAttr). Same persistence shape as leading: flat
    //   "start:end:ivalue;..." lists on the mgr, since they're not Pango
    //   attrs. Colour ivalue is packed 0xRRGGBB or -1 (explicit none); width
    //   ivalue is doc-px x PANGO_SCALE. Only emitted when present.
    {
        std::string sc, sw;
        for (const auto& s : mgr.text_attr_spans) {
            if (s.type == curvz::utils::kCurvzStrokeColorAttr) {
                if (!sc.empty()) sc += ";";
                sc += std::to_string(s.start_byte) + ":" +
                      std::to_string(s.end_byte) + ":" +
                      std::to_string(s.ivalue);
            } else if (s.type == curvz::utils::kCurvzStrokeWidthAttr) {
                if (!sw.empty()) sw += ";";
                sw += std::to_string(s.start_byte) + ":" +
                      std::to_string(s.end_byte) + ":" +
                      std::to_string(s.ivalue);
            }
        }
        if (!sc.empty())
            out << " data-curvz-stroke-color=\"" << sc << "\"";
        if (!sw.empty())
            out << " data-curvz-stroke-width=\"" << sw << "\"";
    }
    // s332 — per-paragraph alignment runs (kCurvzAlignAttr). Same flat
    //   "start:end:ivalue;..." shape; ivalue = 1 (centre) / 2 (right). Left
    //   (0) is stored as no run, so it never appears here. Only emitted when
    //   present (left-only boxes stay byte-identical).
    {
        std::string al;
        for (const auto& s : mgr.text_attr_spans) {
            if (s.type != curvz::utils::kCurvzAlignAttr) continue;
            if (!al.empty()) al += ";";
            al += std::to_string(s.start_byte) + ":" +
                  std::to_string(s.end_byte) + ":" +
                  std::to_string(s.ivalue);
        }
        if (!al.empty())
            out << " data-curvz-align=\"" << al << "\"";
    }
    // s334 — per-paragraph indent runs (left / right / first-line). Same flat
    //   "start:end:ivalue;..." shape as alignment; ivalue = doc-px x PANGO_SCALE.
    //   0 is stored as no run, so un-indented boxes stay byte-identical.
    {
        struct { int type; const char* attr; } kinds[] = {
            { curvz::utils::kCurvzIndentLeftAttr,  "data-curvz-indent-left"  },
            { curvz::utils::kCurvzIndentRightAttr, "data-curvz-indent-right" },
            { curvz::utils::kCurvzIndentFirstAttr, "data-curvz-indent-first" },
        };
        for (const auto& k : kinds) {
            std::string runs;
            for (const auto& s : mgr.text_attr_spans) {
                if (s.type != k.type) continue;
                if (!runs.empty()) runs += ";";
                runs += std::to_string(s.start_byte) + ":" +
                        std::to_string(s.end_byte) + ":" +
                        std::to_string(s.ivalue);
            }
            if (!runs.empty())
                out << " " << k.attr << "=\"" << runs << "\"";
        }
    }
    // s335 — per-paragraph TAB STOPS (kCurvzTabsAttr). STRING-valued, unlike the
    //   int run-lists above: the value is the "pos,type;..." spec, which itself
    //   uses ';' and ',', so the run separator here is '|' (which can never
    //   appear in a tab spec) instead of ';'. Shape: "start:end:spec|...". The
    //   spec contains no XML-special chars by construction, so it rides raw (no
    //   xml_escape). 0 runs -> attribute omitted (byte-identical to no tabs).
    {
        std::string runs;
        for (const auto& s : mgr.text_attr_spans) {
            if (s.type != curvz::utils::kCurvzTabsAttr) continue;
            if (s.svalue.empty()) continue;
            if (!runs.empty()) runs += "|";
            runs += std::to_string(s.start_byte) + ":" +
                    std::to_string(s.end_byte) + ":" + s.svalue;
        }
        if (!runs.empty())
            out << " data-curvz-tabs=\"" << runs << "\"";
    }
    // s327 m1 — baseline flow angle (radians). Default 0 omits cleanly so
    //   pre-s327 / un-rotated boxes stay byte-identical. fmt6 matches the
    //   guide-angle precision precedent; fmt2 would snap free-rotated text on
    //   reload (round-trip fidelity). Lives on the mgr/buffer node alongside
    //   the other baseline text properties, NOT the boundary shape.
    if (mgr.text_baseline_angle != 0.0)
        out << " data-curvz-baseline-angle=\"" << fmt6(mgr.text_baseline_angle) << "\"";
    if (mgr.text_bold)   out << " data-curvz-font-bold=\"1\"";
    if (mgr.text_italic) out << " data-curvz-font-italic=\"1\"";
    // s325 m3 — the CDATA below is Pango markup (plain text is valid markup,
    // so the marker rides every Mgr def, not only formatted ones).
    out << " data-curvz-markup=\"1\"";
    out << " fill=\"" << fill_attr(mgr.fill) << "\"";
    if (mgr.stroke.paint.type != FillStyle::Type::None)
        out << stroke_attrs(mgr.stroke);
    out << shadow_attr_str(mgr);
    out << ">\n";
    out << pad << "  <![CDATA[" << cdata_escape(encode_markup(mgr.text_content,
                                                              mgr.text_attr_spans))
        << "]]>\n";
    // Future sub-manager element children (TabMgr, StyleMgr, etc.)
    // land here.
    out << pad << "</g>\n";
}

// ── write_object ─────────────────────────────────────────────────────────────
// Recursive SVG element emitter.
//
// role_hint: if non-null, the string is inserted as an extra attribute on the
//   first opening tag emitted for this object. Used by the Blend writer to
//   tag A and B sources with data-curvz-blend-role="a"/"b" so the parser can
//   route them back into blend_source_a/b slots on reload.
static void write_object(std::ostringstream& out, const GlyphObject& obj, int indent,
                         const char* role_hint = nullptr) {
    std::string pad(indent * 2, ' ');

    // ── ClipGroup ──────────────────────────────────────────────────────
    // Emits <g clip-path="url(#<clip_id>)" data-curvz-clipgroup="1">children</g>.
    // The actual <clipPath> element lives in a top-level <defs> block
    // written by write_svg itself (see collect_clip_defs). Children are
    // reversed on emit per the S51 z-order contract (same as Group).
    if (obj.type == GlyphObject::Type::ClipGroup) {
        out << pad << "<g data-curvz-clipgroup=\"1\"";
        if (!obj.clip_id.empty())
            out << " clip-path=\"url(#" << obj.clip_id << ")\"";
        out << id_attr_str(obj);
        if (!obj.internal_id.empty())  out << " data-curvz-iid=\"" << obj.internal_id << "\"";
        if (!obj.name.empty())         out << " data-curvz-name=\"" << obj.name << "\"";
        if (!obj.visible)              out << " display=\"none\"";
        if (obj.opacity < 0.999)       out << " opacity=\"" << fmt2(obj.opacity) << "\"";
        out << shadow_attr_str(obj);  // S97 m1
        out << ">\n";
        for (int i = (int)obj.children.size() - 1; i >= 0; --i)
            write_object(out, *obj.children[i], indent + 1);
        out << pad << "</g>\n";
        return;
    }

    // ── Blend ──────────────────────────────────────────────────────────
    // Full round-trip emission (M6). The wrapper carries all params needed
    // to reconstruct the Blend on parse. Children:
    //   - A as a path tagged data-curvz-blend-role="a"
    //   - Cached intermediates as plain paths (no role tag — foreign tools
    //     see them, parser discards them on reload since cache is derived)
    //   - B as a path tagged data-curvz-blend-role="b"
    // z-order contract: same as Group/ClipGroup — write_object reverses
    // on emit so the first-emitted child is z-bottom in SVG paint order.
    // Blend's logical order is A(bottom) → cache → B(top), so emission
    // order unreversed is [A, cache..., B], which means emit iterates the
    // logical sequence in reverse: B first, cache reversed, A last.
    if (obj.type == GlyphObject::Type::Blend) {
        out << pad << "<g data-curvz-blend=\"1\"";
        out << id_attr_str(obj);
        if (!obj.internal_id.empty())  out << " data-curvz-iid=\"" << obj.internal_id << "\"";
        if (!obj.name.empty())         out << " data-curvz-name=\"" << obj.name << "\"";
        if (!obj.visible)              out << " display=\"none\"";
        if (obj.opacity < 0.999)       out << " opacity=\"" << fmt2(obj.opacity) << "\"";
        out << " data-curvz-blend-steps=\"" << obj.blend_steps << "\"";
        out << " data-curvz-blend-sw-user=\""
            << (obj.blend_stroke_w_user_set ? "1" : "0") << "\"";
        if (obj.blend_stroke_w_user_set) {
            out << " data-curvz-blend-sw-start=\"" << fmt2(obj.blend_stroke_w_start) << "\"";
            out << " data-curvz-blend-sw-end=\""   << fmt2(obj.blend_stroke_w_end)   << "\"";
        }
        out << shadow_attr_str(obj);  // S97 m1
        out << ">\n";
        // Emit in REVERSE of logical order (z-order contract). Logical
        // order is A(bottom) → steps → B(top). Emit: B first, then steps
        // in reverse, then A.
        if (obj.blend_source_b)
            write_object(out, *obj.blend_source_b, indent + 1,
                         "data-curvz-blend-role=\"b\"");
        for (int i = (int)obj.blend_cache.size() - 1; i >= 0; --i)
            if (obj.blend_cache[i]) write_object(out, *obj.blend_cache[i], indent + 1);
        if (obj.blend_source_a)
            write_object(out, *obj.blend_source_a, indent + 1,
                         "data-curvz-blend-role=\"a\"");
        out << pad << "</g>\n";
        return;
    }

    // ── Warp ───────────────────────────────────────────────────────────
    // M6 full round-trip. Wrapper carries:
    //   data-curvz-warp="1"                      — marker
    //   data-curvz-warp-quality="N"              — subdivision density (1..16)
    //   data-curvz-warp-env-top="..."            — encoded top envelope
    //   data-curvz-warp-env-bottom="..."         — encoded bottom envelope
    //   data-curvz-warp-preset="N"               — last-applied preset (s147)
    //   data-curvz-warp-top-count="N"            — anchor count fed to preset
    //   data-curvz-warp-bot-count="N"            — anchor count fed to preset
    // Envelope attrs are omitted when empty so M1-stub files and newly-
    // constructed Warps (before first Apply of a dialog preset) round-
    // trip as identity-warp fallbacks, matching pre-M6 behavior. Preset
    // attr is omitted when warp_preset_idx == -1 (Custom — the implicit
    // default), keeping hand-edited warps' SVG attribute set minimal.
    //
    // Single child: the source, tagged data-curvz-warp-role="source".
    // Caches (glyph_cache, warp_cache) are NOT emitted — derived-not-
    // authoritative, rebuilt on first draw after parse.
    if (obj.type == GlyphObject::Type::Warp) {
        out << pad << "<g data-curvz-warp=\"1\"";
        out << id_attr_str(obj);
        if (!obj.internal_id.empty())  out << " data-curvz-iid=\"" << obj.internal_id << "\"";
        if (!obj.name.empty())         out << " data-curvz-name=\"" << obj.name << "\"";
        if (!obj.visible)              out << " display=\"none\"";
        if (obj.opacity < 0.999)       out << " opacity=\"" << fmt2(obj.opacity) << "\"";
        out << " data-curvz-warp-quality=\"" << obj.warp_quality << "\"";
        // s147 m2: preset provenance. Emit preset only when set
        // (Custom is the unmarked default). Counts always emit so a
        // saved (Custom) warp still records what counts the inspector
        // should show on reload.
        if (obj.warp_preset_idx >= 0)
            out << " data-curvz-warp-preset=\"" << obj.warp_preset_idx << "\"";
        out << " data-curvz-warp-top-count=\"" << obj.warp_top_count << "\"";
        out << " data-curvz-warp-bot-count=\"" << obj.warp_bot_count << "\"";
        std::string top_enc = encode_warp_envelope(obj.warp_env_top);
        std::string bot_enc = encode_warp_envelope(obj.warp_env_bottom);
        if (!top_enc.empty())
            out << " data-curvz-warp-env-top=\""    << top_enc << "\"";
        if (!bot_enc.empty())
            out << " data-curvz-warp-env-bottom=\"" << bot_enc << "\"";
        out << shadow_attr_str(obj);  // S97 m1
        out << ">\n";
        if (obj.warp_source)
            write_object(out, *obj.warp_source, indent + 1,
                         "data-curvz-warp-role=\"source\"");
        out << pad << "</g>\n";
        return;
    }

    // ── Group ──────────────────────────────────────────────────────────────
    if (obj.type == GlyphObject::Type::Group) {
        out << pad << "<g data-curvz-group=\"1\"";
        if (role_hint)                out << " " << role_hint;
        out << id_attr_str(obj);
        if (!obj.internal_id.empty())  out << " data-curvz-iid=\"" << obj.internal_id << "\"";
        if (!obj.name.empty())         out << " data-curvz-name=\"" << obj.name << "\"";
        if (!obj.visible)              out << " display=\"none\"";
        if (obj.opacity < 0.999)       out << " opacity=\"" << fmt2(obj.opacity) << "\"";
        out << shadow_attr_str(obj);  // S97 m1
        out << ">\n";
        // Reverse children on write: Curvz internal convention is [0]=top,
        // but SVG paint order is first-element-emitted-is-painted-first
        // (= bottom). Emitting children[n-1] first restores spec-correct
        // paint order for external viewers.
        for (int i = (int)obj.children.size() - 1; i >= 0; --i)
            write_object(out, *obj.children[i], indent + 1);
        out << pad << "</g>\n";
        return;
    }

    // ── TextBox ────────────────────────────────────────────────────────────
    // A typed container holding a text frame as one user-visible atom.
    // The on-disk format is composable SVG that any viewer renders
    // correctly without knowing the data-curvz-textbox marker:
    //
    //   <g data-curvz-textbox="1" iid name [opacity] [display]>
    //     <path ... boundary path with margins as data-curvz-margin-*>
    //     <!-- For each laid-out baseline: -->
    //     <path id="baseline_<iid>_<N>" d="M xs y L xe y"
    //           fill="none" stroke="none" data-curvz-baseline="1"/>
    //     <text font-family=... font-size=... fill=...>
    //       <textPath href="#baseline_<iid>_<N>">slice</textPath>
    //     </text>
    //   </g>
    //
    // The boundary's fill/stroke/path-d round-trip through the normal
    // Path branch. Margins ride as data-curvz-margin-* attributes on the
    // boundary's <path> tag (the existing text emit's margin attrs,
    // relocated to their structural owner per the stage-3d refactor).
    //
    // Baseline paths are SVG-output-only: Curvz regenerates them on load
    // via compute_text_layout, so the parser discards them. They exist
    // in the file because external SVG viewers need real paths for the
    // <textPath href="#..."> binding to work. Without them, Inkscape /
    // Firefox / preview tools would render the text as a single
    // overflowing line at the text's x/y, ignoring the boundary.
    //
    // The full buffer is also emitted as data-curvz-content on the <g>
    // so the parser has a single round-trip-safe source for the buffer.
    // The textPath slices are for external rendering fidelity; the
    // attribute is for parse correctness. Same redundancy idiom the
    // existing text emit uses (split_tags discards inter-tag char data,
    // so attribute is the safe path).
    if (obj.type == GlyphObject::Type::TextBox) {
        // Safety: malformed TextBox (wrong child count or types) emits
        // a degraded form — the marker group with nothing inside, so the
        // file still validates and the parser doesn't crash on load.
        // Should never happen if construction follows the contract.
        if (obj.children.size() < 2 ||
            !obj.children[0] || !obj.children[1] ||
            !obj.children[0]->is_text() ||
            obj.children[1]->type != GlyphObject::Type::Path) {
            out << pad << "<g data-curvz-textbox=\"1\"";
            if (!obj.internal_id.empty())
                out << " data-curvz-iid=\"" << obj.internal_id << "\"";
            out << "></g>\n";
            LOG_WARN("SvgWriter: TextBox '{}' has malformed children — "
                     "emitting empty marker", obj.id);
            return;
        }
        const SceneNode& text     = *obj.children[0];
        const SceneNode& boundary = *obj.children[1];

        // Inline XML escape (local to this branch — matches the text
        // emit's helper a few lines below).
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

        // ── Open the group with identity + container attributes. ─────────
        out << pad << "<g data-curvz-textbox=\"1\"";
        if (role_hint)                  out << " " << role_hint;
        out << id_attr_str(obj);
        if (!obj.internal_id.empty())   out << " data-curvz-iid=\"" << obj.internal_id << "\"";
        if (!obj.name.empty())          out << " data-curvz-name=\"" << xml_escape(obj.name) << "\"";
        if (!obj.visible)               out << " display=\"none\"";
        if (obj.opacity < 0.999)        out << " opacity=\"" << fmt2(obj.opacity) << "\"";
        // Full buffer as the round-trip-safe source of truth.
        out << " data-curvz-content=\"" << xml_escape(text.text_content) << "\"";
        // s305 m1 — Caret persistence. The byte lives on the Text child
        //   at runtime; rides on the TextBox group at rest because
        //   that's the round-trip vehicle for the whole textbox.
        //   Omitted when 0 (the default "no saved position" state) to
        //   keep the SVG clean — most files have textboxes whose caret
        //   was never moved off zero, and emitting the attr on every
        //   one is noise.
        if (text.text_caret_byte > 0)
            out << " data-curvz-caret-byte=\"" << text.text_caret_byte << "\"";
        out << shadow_attr_str(obj);
        out << ">\n";

        // ── Boundary child — emit via the normal Path branch so the
        //    boundary's fill, stroke, path data, and margins (now owned
        //    by the boundary per stage 3d) all serialise through the
        //    same code path any other Path uses. Recursion via
        //    write_object keeps the boundary's representation uniform
        //    with the rest of the document.
        write_object(out, boundary, indent + 1);

        // ── Per-baseline path + textPath pairs. compute_text_layout
        //    runs Pango against the current boundary + text to find
        //    where each visual line sits in doc space. For empty
        //    buffers the baseline list is empty, and we emit nothing —
        //    the textbox loads back as an empty frame the user can
        //    type into. Same behaviour as a freshly-created textbox.
        TextLayout layout = compute_text_layout(&boundary, &text);
        const std::string ipad((indent + 1) * 2, ' ');
        for (size_t bi = 0; bi < layout.baselines.size(); ++bi) {
            const auto& bl = layout.baselines[bi];
            // Stable id from the textbox iid + baseline index. Re-savable
            // (same textbox produces same ids on re-emit, which lets diff
            // tools compare versions cleanly).
            const std::string bid = "baseline_" + obj.internal_id + "_" +
                                    std::to_string(bi);
            // Transparent line for the textPath reference. fill="none"
            // and stroke="none" make it invisible in external viewers;
            // the marker data-curvz-baseline lets the parser identify
            // and discard these on load (regenerated from compute_text_layout).
            out << ipad << "<path id=\"" << bid << "\""
                << " d=\"M " << fmt2(bl.x_start) << " " << fmt2(bl.y)
                << " L "    << fmt2(bl.x_end)   << " " << fmt2(bl.y) << "\""
                << " fill=\"none\" stroke=\"none\""
                << " data-curvz-baseline=\"1\"/>\n";

            // Slice of the buffer for this baseline. Guard against
            // out-of-range byte ranges (defensive — compute_text_layout
            // should produce coherent ranges).
            const std::string& buf = text.text_content;
            size_t bs = std::min(bl.byte_start, buf.size());
            size_t be = std::min(bl.byte_end,   buf.size());
            if (be < bs) be = bs;
            std::string slice = buf.substr(bs, be - bs);

            // <text><textPath href="#baseline_iid_N">slice</textPath></text>.
            // Font + fill attrs ride on the <text> so the textPath
            // renders correctly in external viewers — same attribute
            // set as the normal text emit, minus the position attrs
            // (textPath positions itself along its href target).
            out << ipad << "<text"
                << " font-family=\"" << text.text_font_family << "\""
                << " font-size=\""   << fmt2(text.text_font_size) << "\"";
            if (text.text_bold)   out << " font-weight=\"bold\"";
            if (text.text_italic) out << " font-style=\"italic\"";
            out << " fill=\"" << fill_attr(text.fill) << "\"";
            if (text.stroke.paint.type != FillStyle::Type::None)
                out << stroke_attrs(text.stroke);
            out << ">"
                << "<textPath href=\"#" << bid << "\">"
                << xml_escape(slice)
                << "</textPath>"
                << "</text>\n";
        }

        out << pad << "</g>\n";
        return;
    }

    // ── TextBoxMgr (s311 m1c-redux dual-block) ────────────────────────────
    // The Mgr itself emits nothing in the layer body — it's already in
    // <defs> as a non-rendering blob via the write_svg pre-pass (see
    // MgrCollector and write_textbox_mgr_def). What we emit here, at the
    // Mgr's slot in the layer, is each of its canvas TextBoxView children
    // as a sibling-style <g data-curvz-textbox-view>. Each view carries
    // identity (view-iid + mgr-iid back-pointer + view-index/count) plus
    // its boundary path and a per-baseline render snapshot so foreign SVG
    // readers (browsers, Inkscape) see actual text-on-path glyphs.
    //
    // m1c-redux keeps the in-memory tree unchanged: views remain children
    // of the Mgr SceneNode, and we walk obj.children here to find them.
    // A future refactor (in-memory views-as-layer-children) will let views
    // interleave with other layer children at their own z-order positions;
    // until then they group at the Mgr's slot, matching today's behavior.
    //
    // On disk:
    //   <!-- in <defs>: -->
    //   <g data-curvz-textbox-mgr="1" data-curvz-iid="mgr-A" ...>
    //     <![CDATA[buffer]]>
    //   </g>
    //   <!-- in layer body at the Mgr's slot: -->
    //   <g data-curvz-textbox-view="1"
    //      data-curvz-iid="view-0-iid"
    //      data-curvz-mgr-iid="mgr-A"
    //      data-curvz-view-index="0"
    //      data-curvz-view-count="N">
    //     <path ... boundary .../>
    //     <path id="baseline_..." data-curvz-baseline="1" .../>
    //     <text><textPath href="#baseline_...">slice</textPath></text>
    //     ...
    //   </g>
    //   <!-- one such <g> per canvas view; popover skipped (regenerated). -->
    //
    // Compute layout per-view from the Mgr (as the text-bearing node) and
    // the view's boundary Path. byte_start cascades down the view list;
    // in m1 the runtime tree typically has one canvas view, so byte_start=0
    // covers the common case. compute_text_layout will grow a byte_start
    // parameter when m3 lands multi-view support.
    if (obj.type == GlyphObject::Type::TextBoxMgr) {
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

        // Defensive: confirm the Mgr was collected for defs emission.
        // If not (Mgr without iid, or fresh Mgr appearing mid-write
        // somehow), the Mgr's flow-state won't survive round-trip and
        // we log a warning. Emission proceeds either way so the views
        // still hit disk and a human reader sees what's there.
        if (!g_mgr_collector ||
            (!obj.internal_id.empty() &&
             !g_mgr_collector->by_iid.count(obj.internal_id))) {
            LOG_WARN("SvgWriter: TextBoxMgr '{}' (iid='{}') not in defs "
                     "collector — buffer/caret will not round-trip",
                     obj.name, obj.internal_id);
        }

        // Count canvas views up front so each emitted view can carry
        // an accurate view-count. Popover views excluded — they don't
        // persist on disk.
        size_t canvas_total = 0;
        for (size_t vi = 0; vi < obj.children.size(); ++vi) {
            const auto& cv = obj.children[vi];
            if (!cv) continue;
            if (cv->type != GlyphObject::Type::TextBoxView) continue;
            if (cv->view_kind == SceneNode::ViewKind::Popover) continue;
            ++canvas_total;
        }

        // Walk views in order, emitting canvas views at this indent
        // (the Mgr's slot in the layer). Each view's index counts only
        // canvas views — the popover doesn't participate in the index.
        size_t canvas_index = 0;
        for (size_t vi = 0; vi < obj.children.size(); ++vi) {
            const auto& view_ptr = obj.children[vi];
            if (!view_ptr) continue;
            const SceneNode& view = *view_ptr;
            if (view.type != GlyphObject::Type::TextBoxView) continue;
            if (view.view_kind == SceneNode::ViewKind::Popover) continue;

            // Helper to emit the view's identity attributes consistently
            // for both the boundary-present and the malformed-empty
            // cases below.
            auto emit_view_open_attrs = [&](void) {
                out << " data-curvz-view-kind=\"canvas\"";
                if (!view.internal_id.empty())
                    out << " data-curvz-iid=\"" << view.internal_id << "\"";
                if (!obj.internal_id.empty())
                    out << " data-curvz-mgr-iid=\"" << obj.internal_id << "\"";
                out << " data-curvz-view-index=\"" << canvas_index << "\""
                    << " data-curvz-view-count=\"" << canvas_total << "\"";
                if (!view.name.empty())
                    out << " data-curvz-name=\"" << xml_escape(view.name) << "\"";
                if (!view.visible) out << " display=\"none\"";
                if (view.opacity < 0.999)
                    out << " opacity=\"" << fmt2(view.opacity) << "\"";
            };

            // Canvas view must have a Path child as its boundary.
            // Malformed → emit empty view marker so the file still loads
            // back as the right structural shape (just no boundary).
            if (view.children.empty() || !view.children[0] ||
                view.children[0]->type != GlyphObject::Type::Path) {
                out << pad << "<g data-curvz-textbox-view=\"1\"";
                emit_view_open_attrs();
                out << "></g>\n";
                LOG_WARN("SvgWriter: TextBoxMgr '{}' view[{}] has no "
                         "boundary Path — emitting empty view marker",
                         obj.name, vi);
                ++canvas_index;
                continue;
            }
            const SceneNode& boundary = *view.children[0];

            // Open the view group.
            out << pad << "<g data-curvz-textbox-view=\"1\"";
            emit_view_open_attrs();
            out << ">\n";

            // Boundary child — emit via the normal Path branch so the
            // boundary's fill, stroke, path data, and margins all
            // serialise through the same code path any other Path uses.
            write_object(out, boundary, indent + 1);

            // Per-baseline path + textPath pairs for THIS view. The Mgr
            // plays the role of the "text node" (carries buffer + font
            // defaults) — same as the pre-m1c-redux emit did, just at
            // one less indent level since there's no Mgr wrapper now.
            const std::string vipad((indent + 1) * 2, ' ');
            TextLayout layout = compute_text_layout(&boundary, &obj);
            for (size_t bi = 0; bi < layout.baselines.size(); ++bi) {
                const auto& bl = layout.baselines[bi];
                const std::string bid = "baseline_" + view.internal_id + "_" +
                                        std::to_string(bi);
                out << vipad << "<path id=\"" << bid << "\""
                    << " d=\"M " << fmt2(bl.x_start) << " " << fmt2(bl.y)
                    << " L "    << fmt2(bl.x_end)   << " " << fmt2(bl.y) << "\""
                    << " fill=\"none\" stroke=\"none\""
                    << " data-curvz-baseline=\"1\"/>\n";

                const std::string& buf = obj.text_content;
                size_t bs = std::min(bl.byte_start, buf.size());
                size_t be = std::min(bl.byte_end,   buf.size());
                if (be < bs) be = bs;
                std::string slice = buf.substr(bs, be - bs);

                out << vipad << "<text"
                    << " font-family=\"" << obj.text_font_family << "\""
                    << " font-size=\""   << fmt2(obj.text_font_size) << "\"";
                if (obj.text_bold)   out << " font-weight=\"bold\"";
                if (obj.text_italic) out << " font-style=\"italic\"";
                out << " fill=\"" << fill_attr(obj.fill) << "\"";
                if (obj.stroke.paint.type != FillStyle::Type::None)
                    out << stroke_attrs(obj.stroke);
                out << ">"
                    << "<textPath href=\"#" << bid << "\">"
                    << xml_escape(slice)
                    << "</textPath>"
                    << "</text>\n";
            }
            out << pad << "</g>\n";
            ++canvas_index;
        }

        // role_hint: currently used by Blend's A/B path emit. TextBoxMgr
        // isn't a Blend role child today, but if a future Mgr ever ended
        // up nested as a Blend source the hint would have no obvious
        // attachment site in dual-block emit. Logging-only is safest.
        if (role_hint) {
            LOG_WARN("SvgWriter: TextBoxMgr '{}' got role_hint='{}' but "
                     "dual-block emit has no Mgr wrapper to attach it to",
                     obj.name, role_hint);
        }
        return;
    }

    // ── Compound ───────────────────────────────────────────────────────────
    if (obj.type == GlyphObject::Type::Compound) {
        // Emit as a single <path> with all child subpaths concatenated.
        // fill-rule="evenodd" produces the cutout effect in all SVG renderers.
        // data-curvz-compound preserves the structure for round-trip parsing.

        // Build combined d string from all Path children
        std::ostringstream d;
        std::string all_types;
        std::string child_ids;

        for (const auto& child : obj.children) {
            if (child->type != GlyphObject::Type::Path || !child->path) continue;
            const auto& pd = *child->path;
            if (pd.nodes.empty()) continue;

            if (d.tellp() > 0) d << " ";
            d << "M " << fmt2(pd.nodes[0].x) << " " << fmt2(pd.nodes[0].y);
            int n = (int)pd.nodes.size();
            int segs = pd.closed ? n : n - 1;
            for (int i = 0; i < segs; ++i) {
                const BezierNode& a = pd.nodes[i];
                const BezierNode& b = pd.nodes[(i+1) % n];
                bool a_degen = (std::abs(a.cx2 - a.x) < 1e-6 && std::abs(a.cy2 - a.y) < 1e-6);
                bool b_degen = (std::abs(b.cx1 - b.x) < 1e-6 && std::abs(b.cy1 - b.y) < 1e-6);
                if (a_degen && b_degen)
                    d << " L " << fmt2(b.x) << " " << fmt2(b.y);
                else
                    d << " C " << fmt2(a.cx2) << " " << fmt2(a.cy2)
                      << " "   << fmt2(b.cx1) << " " << fmt2(b.cy1)
                      << " "   << fmt2(b.x)   << " " << fmt2(b.y);
            }
            if (pd.closed) d << " Z";

            // Accumulate node types per child separated by "|"
            std::string child_types;
            for (const auto& nd : pd.nodes) {
                if (!child_types.empty()) child_types += ' ';
                switch (nd.type) {
                    case BezierNode::Type::Symmetric: child_types += 'S'; break;
                    case BezierNode::Type::Smooth:    child_types += 'M'; break;
                    case BezierNode::Type::Cusp:      child_types += 'C'; break;
                    case BezierNode::Type::Corner:    child_types += 'K'; break;
                }
            }
            if (!all_types.empty()) all_types += "|";
            all_types += child_types;

            // Accumulate child ids for round-trip — encoded the same way
            // as the SVG `id` attribute itself (name + iid), so external
            // references to data-curvz-child-ids resolve correctly.
            if (!child_ids.empty()) child_ids += ",";
            child_ids += curvz::utils::encode_svg_id(child->name, child->internal_id);
        }

        if (d.tellp() == 0) return; // no valid children

        // Fill/stroke from compound node itself. Per S58d/g, "Compound owns
        // its paint" — children's individual fill/stroke values are stale
        // remnants of pre-compound state and must NOT be the source of
        // truth on save. (Using children[0] here was a long-standing bug:
        // it caused compounds to revert to a child's pre-compound colour
        // on reopen, even after the compound's own fill had been edited.)
        std::string fill_s   = " fill=\""   + fill_attr(obj.fill) + "\"";
        std::string stroke_s = stroke_attrs(obj.stroke);
        std::string swatch_s = swatch_binding_attrs(obj);
        // S77 m3c: bound_style lives on the compound node itself.
        std::string style_s  = style_binding_attr(obj);

        out << pad << "<path"
            << " data-curvz-compound=\"1\""
            << (role_hint ? std::string(" ") + role_hint : std::string())
            << id_attr_str(obj)
            << (obj.internal_id.empty() ? "" : " data-curvz-iid=\"" + obj.internal_id + "\"")
            << (obj.name.empty() ? "" : " data-curvz-name=\"" + obj.name + "\"")
            << (!obj.visible     ? " display=\"none\"" : "")
            << (obj.opacity < 0.999 ? " opacity=\"" + fmt2(obj.opacity) + "\"" : "")
            << " fill-rule=\"evenodd\""
            << " d=\"" << d.str() << "\""
            << " data-curvz-types=\"" << all_types << "\""
            << " data-curvz-child-ids=\"" << child_ids << "\""
            << fill_s << stroke_s << swatch_s << style_s
            << shadow_attr_str(obj)  // S97 m1
            << "/>\n";
        return;
    }

    // ── Image ──────────────────────────────────────────────────────────────
    if (obj.type == GlyphObject::Type::Image) {
        out << pad << "<image";
        out << id_attr_str(obj);
        if (!obj.internal_id.empty())  out << " data-curvz-iid=\""  << obj.internal_id << "\"";
        if (!obj.name.empty())    out << " data-curvz-name=\"" << obj.name      << "\"";
        out << " x=\""            << fmt2(obj.image_x)         << "\"";
        out << " y=\""            << fmt2(obj.image_y)         << "\"";
        out << " width=\""        << fmt2(obj.image_w)         << "\"";
        out << " height=\""       << fmt2(obj.image_h)         << "\"";
        out << " href=\""         << obj.image_path            << "\"";
        out << " data-curvz-image=\"1\"";
        // Emit transform if non-identity
        const Transform& t = obj.transform;
        bool has_t = (std::abs(t.a-1.0)>1e-6 || std::abs(t.b)>1e-6 ||
                      std::abs(t.c)>1e-6 || std::abs(t.d-1.0)>1e-6);
        if (has_t) {
            // Apply around image centre: translate(cx,cy) matrix(a,b,c,d,0,0) translate(-cx,-cy)
            double icx = obj.image_x + obj.image_w * 0.5;
            double icy = obj.image_y + obj.image_h * 0.5;
            double te = icx - t.a*icx - t.c*icy;
            double tf = icy - t.b*icx - t.d*icy;
            out << " transform=\"matrix(" << fmt2(t.a) << "," << fmt2(t.b)
                << "," << fmt2(t.c) << "," << fmt2(t.d)
                << "," << fmt2(te) << "," << fmt2(tf) << ")\"";
        }
        if (!obj.visible)         out << " display=\"none\"";
        if (obj.opacity < 0.999)  out << " opacity=\"" << fmt2(obj.opacity) << "\"";
        out << shadow_attr_str(obj);  // S97 m1
        out << "/>\n";
        return;
    }

    // ── Ref point ──────────────────────────────────────────────────────────
    if (obj.type == GlyphObject::Type::Ref) {
        out << pad << "<circle data-curvz-ref=\"1\""
            << " cx=\"" << fmt6(obj.ref_x) << "\""
            << " cy=\"" << fmt6(obj.ref_y) << "\"";
        out << id_attr_str(obj);
        if (!obj.internal_id.empty()) out << " data-curvz-iid=\""  << obj.internal_id << "\"";
        if (!obj.name.empty()) out << " data-curvz-name=\"" << obj.name << "\"";
        if (!obj.visible)      out << " display=\"none\"";
        out << " r=\"0\" fill=\"none\" stroke=\"none\"/>\n";
        return;
    }

    // ── Text ───────────────────────────────────────────────────────────────
    if (obj.type == GlyphObject::Type::Text) {
        // SVG <text> uses Y-down coordinates; text_y is already in doc space (Y-down).
        // text_content is stored both as element content (for external SVG viewers)
        // and as data-curvz-content attribute (for reliable round-trip parsing,
        // since SvgParser's split_tags tokeniser discards inter-tag character data).
        auto xml_escape = [](const std::string& s) {
            std::string out;
            for (char c : s) {
                if      (c == '&') out += "&amp;";
                else if (c == '<') out += "&lt;";
                else if (c == '>') out += "&gt;";
                else if (c == '"') out += "&quot;";
                else               out += c;
            }
            return out;
        };
        out << pad << "<text";
        out << id_attr_str(obj);
        if (!obj.internal_id.empty()) out << " data-curvz-iid=\""  << obj.internal_id << "\"";
        if (!obj.name.empty())   out << " data-curvz-name=\""  << xml_escape(obj.name)            << "\"";
        out << " x=\""           << fmt2(obj.text_x)           << "\"";
        out << " y=\""           << fmt2(obj.text_y)           << "\"";
        out << " font-family=\"" << obj.text_font_family       << "\"";
        out << " font-size=\""   << fmt2(obj.text_font_size)   << "\"";
        if (obj.text_bold)   out << " font-weight=\"bold\"";
        if (obj.text_italic) out << " font-style=\"italic\"";
        if (obj.text_baseline_shift != 0.0)
            out << " data-curvz-baseline-shift=\"" << fmt2(obj.text_baseline_shift) << "\"";
        if (obj.text_letter_spacing != 0.0)
            out << " letter-spacing=\"" << fmt2(obj.text_letter_spacing) << "\"";
        if (obj.text_anchor != "start")
            out << " text-anchor=\"" << obj.text_anchor << "\"";
        if (obj.text_align != "left" && !obj.text_align.empty())
            out << " data-curvz-align=\"" << obj.text_align << "\"";
        out << " fill=\""        << fill_attr(obj.fill)        << "\"";
        if (obj.stroke.paint.type != FillStyle::Type::None)
            out << stroke_attrs(obj.stroke);
        out << swatch_binding_attrs(obj);
        out << style_binding_attr(obj);  // S77 m3c
        if (!obj.visible)        out << " display=\"none\"";
        if (obj.opacity < 0.999) out << " opacity=\"" << fmt2(obj.opacity) << "\"";
        out << shadow_attr_str(obj);  // S97 m1 — on the <text> open tag, applies to both branches below
        // Custom attribute for reliable round-trip (split_tags discards char data).
        out << " data-curvz-content=\"" << xml_escape(obj.text_content) << "\"";
        if (!obj.text_path_id.empty()) {
            LOG_DEBUG("SvgWriter: text node '{}' text_path_id='{}'",
                      obj.id, obj.text_path_id);
            out << " data-curvz-path-id=\"" << obj.text_path_id << "\"";
            out << " data-curvz-path-offset=\"" << fmt6(obj.text_path_offset) << "\"";
            if (obj.text_path_flip) out << " data-curvz-path-flip=\"1\"";
        }
        // s301 m1a — text container model round-trip. Boundary list emitted as
        // space-separated iids in document order (= overflow-chain order); line
        // pattern id and margins emitted only when non-default. All under the
        // existing data-curvz-* namespace so non-Curvz SVG readers ignore them.
        if (!obj.text_boundary_ids.empty()) {
            out << " data-curvz-boundary-ids=\"";
            for (size_t i = 0; i < obj.text_boundary_ids.size(); ++i) {
                if (i) out << " ";
                out << obj.text_boundary_ids[i];
            }
            out << "\"";
        }
        if (!obj.text_line_path_id.empty()) {
            out << " data-curvz-line-path-id=\"" << obj.text_line_path_id << "\"";
        }
        if (obj.text_margin_top    != 0.0) out << " data-curvz-margin-top=\""    << fmt2(obj.text_margin_top)    << "\"";
        if (obj.text_margin_bottom != 0.0) out << " data-curvz-margin-bottom=\"" << fmt2(obj.text_margin_bottom) << "\"";
        if (obj.text_margin_left   != 0.0) out << " data-curvz-margin-left=\""   << fmt2(obj.text_margin_left)   << "\"";
        if (obj.text_margin_right  != 0.0) out << " data-curvz-margin-right=\""  << fmt2(obj.text_margin_right)  << "\"";
        if (!obj.text_path_id.empty()) {
            // Emit SVG-standard textPath for interoperability with other SVG viewers
            out << ">\n";
            out << pad << "  <textPath href=\"#" << obj.text_path_id << "\"";
            out << " startOffset=\"" << fmt2(obj.text_path_offset) << "px\"";
            if (obj.text_path_flip) out << " side=\"right\"";
            out << ">" << xml_escape(obj.text_content) << "</textPath>\n";
            out << pad << "</text>\n";
        } else {
            out << ">" << xml_escape(obj.text_content) << "</text>\n";
        }
        return;
    }

    std::string fill   = " fill=\""   + fill_attr(obj.fill) + "\"";
    std::string stroke = stroke_attrs(obj.stroke);
    std::string swatch_bind = swatch_binding_attrs(obj);
    std::string style_bind  = style_binding_attr(obj);  // S77 m3c
    std::string id_attr  = id_attr_str(obj);
    std::string iid_attr = obj.internal_id.empty() ? "" : " data-curvz-iid=\"" + obj.internal_id + "\"";
    std::string opacity_attr = (obj.opacity < 0.999)
        ? " opacity=\"" + fmt2(obj.opacity) + "\"" : "";

    if (obj.type == GlyphObject::Type::Path && obj.path) {
        const auto& pd = *obj.path;
        if (pd.nodes.empty()) { return; }

        std::ostringstream d;
        d << "M " << fmt2(pd.nodes[0].x) << " " << fmt2(pd.nodes[0].y);
        int n = (int)pd.nodes.size();
        int segs = pd.closed ? n : n - 1;
        for (int i = 0; i < segs; ++i) {
            const BezierNode& a = pd.nodes[i];
            const BezierNode& b = pd.nodes[(i+1) % n];
            bool a_degen = (std::abs(a.cx2 - a.x) < 1e-6 && std::abs(a.cy2 - a.y) < 1e-6);
            bool b_degen = (std::abs(b.cx1 - b.x) < 1e-6 && std::abs(b.cy1 - b.y) < 1e-6);
            if (a_degen && b_degen) {
                d << " L " << fmt2(b.x) << " " << fmt2(b.y);
            } else {
                d << " C " << fmt2(a.cx2) << " " << fmt2(a.cy2)
                  << " " << fmt2(b.cx1)   << " " << fmt2(b.cy1)
                  << " " << fmt2(b.x)     << " " << fmt2(b.y);
            }
        }
        if (pd.closed) d << " Z";

        // ── Node type metadata ─────────────────────────────────────────────
        // Store editor node types as a custom data attribute so they survive
        // save/load. One char per node: S=Symmetric M=sMooth C=Cusp K=corner
        // (K avoids collision with the SVG 'C' cubic command letter).
        std::string types_str;
        for (const auto& nd : pd.nodes) {
            if (!types_str.empty()) types_str += ' ';
            switch (nd.type) {
                case BezierNode::Type::Symmetric: types_str += 'S'; break;
                case BezierNode::Type::Smooth:    types_str += 'M'; break;
                case BezierNode::Type::Cusp:      types_str += 'C'; break;
                case BezierNode::Type::Corner:    types_str += 'K'; break;
            }
        }

        out << pad << "<path";
        if (role_hint) out << " " << role_hint;
        out << id_attr
            << iid_attr
            << " d=\"" << d.str() << "\""
            << " data-curvz-types=\"" << types_str << "\""
            << (obj.name.empty() ? "" : " data-curvz-name=\"" + obj.name + "\"")
            << fill << stroke << swatch_bind << style_bind << opacity_attr
            << shadow_attr_str(obj);  // S97 m1
        // Margins ride on the Path when it's owned by a TextBox as the
        // boundary child — the stage-3d ownership move put them here.
        // Plain Paths have zero on all four sides; the conditional emit
        // keeps non-textbox files clean. Same data-curvz-margin-*
        // attribute names the legacy text emit used; parser routes them
        // to the boundary's fields rather than the text's now.
        if (obj.text_margin_top    != 0.0) out << " data-curvz-margin-top=\""    << fmt2(obj.text_margin_top)    << "\"";
        if (obj.text_margin_bottom != 0.0) out << " data-curvz-margin-bottom=\"" << fmt2(obj.text_margin_bottom) << "\"";
        if (obj.text_margin_left   != 0.0) out << " data-curvz-margin-left=\""   << fmt2(obj.text_margin_left)   << "\"";
        if (obj.text_margin_right  != 0.0) out << " data-curvz-margin-right=\""  << fmt2(obj.text_margin_right)  << "\"";
        out << "/>\n";
    }
}

// ── write_svg ─────────────────────────────────────────────────────────────────

std::string write_svg(const CurvzDocument& doc) {
    std::ostringstream out;

    char vb[128];
    snprintf(vb, sizeof(vb), "0 0 %d %d", doc.canvas_width(), doc.canvas_height());

    // In Physical mode, emit width/height with the physical unit suffix so
    // downstream renderers display the file at its intended physical size
    // (viewBox stays in doc-units, so geometry is unchanged).  In Pixel /
    // Ratio modes there is no physical intent — emit bare numbers matching
    // the viewBox.  See Inkscape convention: width="1in" + viewBox="0 0 300 300"
    // establishes the doc-unit ↔ inch mapping declaratively.
    //
    // s264 m2: render-intent override. When the doc has intended_w/h set
    // (>0), emit those instead so the SVG declares its intended rendered
    // size while viewBox keeps the working scale. This is the SVG-native
    // idiom for "design at 1000, deliver at 48": every node coordinate
    // stays at full precision in viewBox space; consumers (browsers, file
    // previews, design tools) render the file at delivery size with zero
    // data loss. Priority: render-intent > Physical-mode > Pixel/Ratio
    // legacy. The intent fields are the new canon; the older two branches
    // remain for files that pre-date s264 or never set an intent.
    char wh[64];
    char hh[64];
    if (doc.canvas.intended_w > 0.0 && doc.canvas.intended_h > 0.0) {
        // Render-intent set. Emit with unit suffix unless empty / "px"
        // (SVG default; bare number == px, suffix only when needed).
        const std::string& iu = doc.canvas.intended_unit;
        const bool bare = iu.empty() || iu == "px";
        if (bare) {
            snprintf(wh, sizeof(wh), "%g", doc.canvas.intended_w);
            snprintf(hh, sizeof(hh), "%g", doc.canvas.intended_h);
        } else {
            snprintf(wh, sizeof(wh), "%g%s", doc.canvas.intended_w, iu.c_str());
            snprintf(hh, sizeof(hh), "%g%s", doc.canvas.intended_h, iu.c_str());
        }
    } else if (doc.canvas.display_mode == DisplayMode::Physical) {
        // 2 decimal places for phys w/h; unit is doc.canvas.phys_unit ("in","mm","cm")
        snprintf(wh, sizeof(wh), "%.4f%s",
                 doc.canvas.phys_width,  doc.canvas.phys_unit.c_str());
        snprintf(hh, sizeof(hh), "%.4f%s",
                 doc.canvas.phys_height, doc.canvas.phys_unit.c_str());
    } else {
        snprintf(wh, sizeof(wh), "%d", doc.canvas_width());
        snprintf(hh, sizeof(hh), "%d", doc.canvas_height());
    }
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\""
        << " viewBox=\"" << vb << "\""
        << " width=\""  << wh << "\""
        << " height=\"" << hh << "\"";
    // s150 fix1: measure behaviour prefs are document-level, persist on
    // the root <svg> rather than piggy-backing on the measure layer
    // (which doesn't exist until the user saves a measurement). Pre-fix,
    // toggling the popover before any measurement existed wrote to
    // memory but never persisted.
    if (doc.measure_save_to_layer)
        out << " data-curvz-measure-save-to-layer=\"1\"";
    if (doc.measure_destruct_after_copy)
        out << " data-curvz-measure-destruct-after-copy=\"1\"";

    // s264 m1: persist render intent as explicit data-curvz-intended-*
    // attrs so files round-trip without depending on the parser's
    // viewBox-vs-width disagreement inference. The inference is the
    // compatibility path for files written by other tools; for Curvz's
    // own files we want explicit storage. Unit attribute only emitted
    // when non-default (non-empty / non-"px") to keep the common case
    // (px intent) clean — the parser treats absent unit as "px".
    if (doc.canvas.intended_w > 0.0 && doc.canvas.intended_h > 0.0) {
        char ibuf[32];
        snprintf(ibuf, sizeof(ibuf), "%g", doc.canvas.intended_w);
        out << " data-curvz-intended-w=\"" << ibuf << "\"";
        snprintf(ibuf, sizeof(ibuf), "%g", doc.canvas.intended_h);
        out << " data-curvz-intended-h=\"" << ibuf << "\"";
        if (!doc.canvas.intended_unit.empty() && doc.canvas.intended_unit != "px") {
            out << " data-curvz-intended-unit=\""
                << doc.canvas.intended_unit << "\"";
        }
    }
    out << ">\n";

    // ── S90 Stage 2 — gradient pre-pass ───────────────────────────────
    // Walk the tree once to collect every gradient FillStyle (fill +
    // stroke.paint), mint stable url(#gradN) ids for each, and activate
    // the file-scope collector pointer so fill_attr emits url(...) refs
    // instead of degrading to flat colour. Cleared at the bottom of this
    // function. Empty when the doc has no gradients — the collector
    // pre-pass walk is cheap (just pointer comparisons).
    GradientCollector gc;
    for (const auto& l : doc.layers)
        if (l) collect_gradients(*l, gc);
    g_grad_collector = &gc;

    // ── S97 m1 — drop shadow pre-pass ─────────────────────────────────
    // Same shape as the gradient pre-pass: walk every node, collect any
    // with shadow_enabled, mint per-host filter ids ("sh_<short_iid>"),
    // activate the file-scope collector pointer so write_object's call
    // to shadow_attr_str() can look up the minted id at emit time.
    // Cleared at the bottom of this function.
    ShadowCollector sc;
    for (const auto& l : doc.layers)
        if (l) collect_shadows(*l, sc);
    g_shadow_collector = &sc;

    // ── s311 m1c-redux — TextBoxMgr pre-pass ──────────────────────────
    // Dual-block disk format. Walk the tree once, collect every Mgr by
    // iid. Each collected Mgr emits a <g data-curvz-textbox-mgr> blob
    // into <defs> (buffer + caret + font defaults + fill/stroke);
    // write_object's TextBoxMgr branch emits each canvas view at the
    // Mgr's layer position with the Mgr's iid as back-pointer. The
    // collector exists so the views' write_object branch can confirm
    // the Mgr was indeed emitted (defensive — every in-tree Mgr should
    // make it into the collector).
    MgrCollector mc;
    for (const auto& l : doc.layers)
        if (l) collect_mgrs(*l, mc);
    g_mgr_collector = &mc;

    // ── <defs> with <clipPath> + <linearGradient>/<radialGradient> ────
    // Walk the tree once, collect every ClipGroup, and emit its
    // clip_shape inside <clipPath id="<clip_id>">. The ClipGroup node
    // itself is emitted later via the normal layer → write_object path,
    // with just a clip-path="url(#...)" reference. Illustrator/Inkscape/
    // browsers need the <defs> to precede the referencing <g>.
    //
    // The clip_shape is emitted as plain SVG geometry (path or multiple
    // paths for a Compound) — no fill/stroke on the <clipPath> wrapper
    // itself. When the clip_shape is missing/empty the ClipGroup is
    // still emitted (with an empty <clipPath>), so a degenerate
    // ClipGroup survives a round-trip without being silently dropped.
    {
        std::vector<const SceneNode*> clip_groups;
        std::function<void(const SceneNode&)> collect = [&](const SceneNode& n) {
            if (n.type == SceneNode::Type::ClipGroup)
                clip_groups.push_back(&n);
            for (const auto& c : n.children)
                if (c) collect(*c);
            if (n.clip_shape) collect(*n.clip_shape);
        };
        for (const auto& l : doc.layers)
            if (l) collect(*l);

        if (!clip_groups.empty() || !gc.entries.empty() || !sc.entries.empty()
            || !mc.entries.empty()) {
            out << "  <defs>\n";
            for (const SceneNode* cg : clip_groups) {
                // clipPathUnits defaults to userSpaceOnUse (what we want).
                // clip-rule="evenodd" when the shape is a Compound so holes
                // are honored — matches Cairo EVEN_ODD at render. Path-only
                // shapes don't need it (single subpath, rule irrelevant).
                out << "    <clipPath id=\"" << cg->clip_id << "\"";
                if (cg->clip_shape &&
                    cg->clip_shape->type == GlyphObject::Type::Compound)
                    out << " clip-rule=\"evenodd\"";
                out << ">\n";
                const SceneNode* cs = cg->clip_shape.get();
                if (cs) {
                    // write_object emits at indent 3 → "      <path ..."
                    // Reuse the existing emitter for Path/Compound so the
                    // d string, fill-rule, etc. match everywhere else.
                    write_object(out, *cs, 3);
                }
                out << "    </clipPath>\n";
            }
            // S90 Stage 2 — gradient defs. One <linearGradient> /
            // <radialGradient> per unique FillStyle pointer collected by
            // the pre-pass at the top of this function. Refs from shapes
            // are emitted by fill_attr() via g_grad_collector lookup.
            for (const FillStyle* fs : gc.entries) {
                const std::string& id = gc.id_map.at(fs);
                write_gradient_def(out, id, *fs, 2);
            }
            // S97 m1 — drop shadow defs. One <filter> per shadowed node.
            // Refs are emitted by write_object via shadow_attr_str(). Each
            // filter is named sh_<short_iid> (8-hex of the host's UUID).
            for (const SceneNode* node : sc.entries) {
                const std::string& id = sc.id_map.at(node);
                write_shadow_filter(out, id, *node, 2);
            }
            // s311 m1c-redux — TextBoxMgr defs. One <g data-curvz-textbox-
            // mgr> per Mgr in the document. Sole authority on the Mgr's
            // flow-state on load: the parser hydrates these first, then
            // walks the visible tree and routes views to their Mgr by
            // back-pointer iid.
            for (const SceneNode* mgr : mc.entries) {
                write_textbox_mgr_def(out, *mgr, 2);
            }
            out << "  </defs>\n";
        }
    }

    for (const auto& layer_uptr : doc.layers) {
        const SceneNode& layer = *layer_uptr;

        // ── Guide layer ───────────────────────────────────────────────────
        if (layer.is_guide_layer()) {
            out << "  <g data-curvz-guide-layer=\"1\"";
            if (!layer.visible) out << " display=\"none\"";
            if (layer.locked)   out << " data-curvz-locked=\"1\"";
            // Emit guide color
            out << " data-curvz-guide-color=\""
                << fmt2(doc.guide_color_r) << ","
                << fmt2(doc.guide_color_g) << ","
                << fmt2(doc.guide_color_b) << "\"";
            // S110 m4: emit iid+name for the layer itself, not just children.
            // Without this the assign_iids safety net mints a fresh iid every
            // load, generating warning spam and (more importantly) breaking
            // any feature that relies on stable system-layer identity across
            // saves.
            if (!layer.internal_id.empty())
                out << " data-curvz-iid=\"" << layer.internal_id << "\"";
            if (!layer.name.empty())
                out << " data-curvz-name=\"" << layer.name << "\"";
            out << ">\n";
            for (const auto& g : layer.children) {
                if (!g->is_guide()) continue;
                // S49+: write new-model fields only.  Legacy
                // data-curvz-guide-pos / data-curvz-guide-horiz are no longer
                // emitted.  SvgParser still reads them for back-compat load
                // of older files.
                out << "    <line data-curvz-guide=\"1\""
                    << " data-curvz-guide-x=\""     << fmt6(g->guide_x)     << "\""
                    << " data-curvz-guide-y=\""     << fmt6(g->guide_y)     << "\""
                    << " data-curvz-guide-angle=\"" << fmt6(g->guide_angle) << "\"";
                out << id_attr_str(*g);
                if (!g->internal_id.empty())
                    out << " data-curvz-iid=\"" << g->internal_id << "\"";
                if (!g->name.empty())
                    out << " data-curvz-name=\"" << g->name << "\"";
                if (g->locked) out << " data-curvz-locked=\"1\"";
                out << "/>\n";
            }
            out << "  </g>\n";
            continue;
        }

        // ── Ref layer ─────────────────────────────────────────────────────
        if (layer.is_ref_layer()) {
            out << "  <g data-curvz-ref-layer=\"1\"";
            if (!layer.visible) out << " display=\"none\"";
            if (layer.locked)   out << " data-curvz-locked=\"1\"";
            if (!layer.color.empty()) out << " data-curvz-color=\"" << layer.color << "\"";
            // S110 m4: see guide-layer comment above — same fix here.
            if (!layer.internal_id.empty())
                out << " data-curvz-iid=\"" << layer.internal_id << "\"";
            if (!layer.name.empty())
                out << " data-curvz-name=\"" << layer.name << "\"";
            out << ">\n";
            for (const auto& r : layer.children) {
                if (!r->is_ref()) continue;
                out << "    <circle data-curvz-ref=\"1\""
                    << " cx=\"" << fmt6(r->ref_x) << "\""
                    << " cy=\"" << fmt6(r->ref_y) << "\""
                    << " r=\"0\" fill=\"none\" stroke=\"none\"";
                out << id_attr_str(*r);
                if (!r->internal_id.empty())
                    out << " data-curvz-iid=\"" << r->internal_id << "\"";
                if (!r->name.empty()) out << " data-curvz-name=\"" << r->name << "\"";
                if (!r->visible)      out << " display=\"none\"";
                // s177: refpt-as-export-origin was a save-format leak
                // for an export-only concept; removed. Save/restore is
                // sacred. Export coords come off the ruler origin.
                out << "/>\n";
            }
            out << "  </g>\n";
            continue;
        }

        // ── Measure layer ─────────────────────────────────────────────────
        if (layer.is_measure_layer()) {
            out << "  <g data-curvz-measure-layer=\"1\"";
            if (!layer.visible) out << " display=\"none\"";
            if (layer.locked)   out << " data-curvz-locked=\"1\"";
            // s150 fix1: data-curvz-save-to-layer / data-curvz-destruct-
            // after-copy moved off the measure-layer wrapper onto the
            // root <svg> element. They're document-level prefs, not
            // layer attributes, and the layer doesn't exist until the
            // user saves a measurement (so emitting them here meant
            // toggling the popover before any measurement existed
            // didn't persist). Parser still reads the legacy
            // layer-level attributes for backward compatibility — see
            // SvgParser.cpp.
            // S110 m4: see guide-layer comment above — same fix here.
            if (!layer.internal_id.empty())
                out << " data-curvz-iid=\"" << layer.internal_id << "\"";
            if (!layer.name.empty())
                out << " data-curvz-name=\"" << layer.name << "\"";
            out << ">\n";
            for (const auto& m : layer.children) {
                if (!m->is_measurement()) continue;
                out << "    <line data-curvz-measure=\"1\""
                    << " data-curvz-mx1=\"" << fmt6(m->measure_x1) << "\""
                    << " data-curvz-my1=\"" << fmt6(m->measure_y1) << "\""
                    << " data-curvz-mx2=\"" << fmt6(m->measure_x2) << "\""
                    << " data-curvz-my2=\"" << fmt6(m->measure_y2) << "\"";
                if (!m->visible) out << " display=\"none\"";
                // S89: stable identity via UUID. The display name is
                // synthesised at render time from coords + active doc
                // unit, so no `id` or name attr is round-tripped.
                if (!m->internal_id.empty())
                    out << " data-curvz-iid=\"" << m->internal_id << "\"";
                out << "/>\n";
            }
            out << "  </g>\n";
            continue;
        }

        // Grid and margin layers are stored in project.json — skip in SVG
        if (layer.is_grid_layer() || layer.is_margin_layer())
            continue;

        // Wrap each layer in a <g> — write even if empty so it survives reload
        out << "  <g data-curvz-layer=\"1\"";
        // S96 m2: SVG `id` is now derived from name + iid via the codec.
        // The authoritative source-of-truth for layer identity on load is
        // data-curvz-iid (UUID) and data-curvz-name (verbatim name); the
        // SVG `id` is a derived handle for external tools / <use> refs.
        out << id_attr_str(layer);
        if (!layer.internal_id.empty())
            out << " data-curvz-iid=\"" << layer.internal_id << "\"";
        if (!layer.name.empty())
            out << " data-curvz-name=\"" << layer.name << "\"";
        if (!layer.visible)
            out << " display=\"none\"";
        if (layer.opacity < 0.999)
            out << " opacity=\"" << fmt2(layer.opacity) << "\"";
        if (layer.locked)
            out << " data-curvz-locked=\"1\"";
        if (!layer.color.empty())
            out << " data-curvz-color=\"" << layer.color << "\"";
        out << ">\n";

        // Reverse children on write — see Group branch comment above for
        // rationale. Layer's [0]=top, SVG's first-emitted=bottom.
        for (int i = (int)layer.children.size() - 1; i >= 0; --i)
            write_object(out, *layer.children[i], 2);

        out << "  </g>\n";
    }

    out << "</svg>\n";
    // S90 Stage 2 — clear active collector. Paired with the activation at
    // the top of this function. Failing to clear would leave a dangling
    // pointer if the next write_svg call were on a different doc and
    // didn't reactivate (which it always does, but defensiveness is free).
    g_grad_collector = nullptr;
    g_shadow_collector = nullptr;  // S97 m1
    g_mgr_collector    = nullptr;  // s311 m1c-redux
    return out.str();
}

bool write_svg_file(const CurvzDocument& doc, const std::string& path) {
    std::ofstream f(path);
    if (!f) {
        LOG_ERROR("SvgWriter: cannot open '{}' for writing", path);
        return false;
    }
    f << write_svg(doc);
    LOG_DEBUG("SvgWriter: wrote '{}'", path);
    return true;
}

// ── S104 m1 — Export Documents metadata variant ──────────────────────────────
//
// Generates standard SVG via write_svg(), then injects data-curvz-export-*
// attributes into the root <svg> element via string replacement. Cheaper
// than parameterising write_svg() — the writer is a 1100-line file with
// many internal helpers, and the only call site that cares about export
// metadata is Project → Export Documents. Other callers (Save, icon
// theme export) intentionally produce vanilla SVG.
//
// The injection target is the literal "<svg " prefix on the root element,
// which write_svg() always emits at offset 0 of its output (no XML
// declaration, no comments). Robust as long as write_svg's output shape
// stays "starts with <svg "; if that ever changes the find-replace will
// fall through to no injection (still produces a valid SVG, just without
// metadata).
bool write_svg_file_with_export_meta(const CurvzDocument& doc,
                                     const std::string& path,
                                     const std::string& units,
                                     double size_value,
                                     const std::string& fit_side) {
    std::string svg = write_svg(doc);

    // Build the injection. fmt2 isn't visible here; do it inline with
    // snprintf — same precision as the rest of the writer.
    char size_buf[32];
    snprintf(size_buf, sizeof(size_buf), "%.4f", size_value);

    std::string inject = " data-curvz-export-units=\"" + units +
                         "\" data-curvz-export-size=\""  + size_buf +
                         "\" data-curvz-export-fit=\""   + fit_side + "\"";

    // Find the root <svg element and inject right after the tag name.
    // write_svg always emits "<svg xmlns=..." at offset 0.
    constexpr const char* kSvgPrefix = "<svg ";
    auto pos = svg.find(kSvgPrefix);
    if (pos != std::string::npos) {
        svg.insert(pos + std::strlen(kSvgPrefix) - 1, inject);
    } else {
        LOG_WARN("SvgWriter: could not locate <svg root for export metadata "
                 "injection in '{}' — writing without metadata", path);
    }

    // ── s265 m2 fix: patch the visible width/height on the root <svg>
    // ── to the Export-dialog Size override. ────────────────────────────
    //
    // Pre-fix, the Export dialog's Size field only made it into the file
    // as data-curvz-export-* metadata; the visible width/height kept
    // coming from doc.canvas.intended_* via write_svg. That meant the
    // SVG would render at the doc's intent size, NOT the override the
    // user typed in the Export dialog — a semantic regression flagged
    // in the s264 handoff "carry forward" bucket.
    //
    // Fix: scale doc.canvas.intended_* (or canvas dims, as fallback)
    // to the override side, compute the other side from the doc's
    // aspect, and rewrite the two attrs in place.
    //
    // Aspect source: intended_* if set, else canvas dims. Same source
    // write_svg picks for width/height, so the aspect can never drift
    // between the two paths.
    {
        // Resolve the doc aspect.
        double aspect_w = (doc.canvas.intended_w > 0.0)
                              ? doc.canvas.intended_w
                              : (double)doc.canvas_width();
        double aspect_h = (doc.canvas.intended_h > 0.0)
                              ? doc.canvas.intended_h
                              : (double)doc.canvas_height();
        if (aspect_w > 0.0 && aspect_h > 0.0 && size_value > 0.0) {
            double out_w, out_h;
            if (fit_side == "height") {
                out_h = size_value;
                out_w = size_value * (aspect_w / aspect_h);
            } else { // default: width-fit
                out_w = size_value;
                out_h = size_value * (aspect_h / aspect_w);
            }

            // Format with unit suffix. SVG default is px when bare; emit
            // suffix for any non-px unit (matches write_svg's idiom).
            char out_w_buf[64], out_h_buf[64];
            const bool bare = units.empty() || units == "px";
            if (bare) {
                snprintf(out_w_buf, sizeof(out_w_buf), "%g", out_w);
                snprintf(out_h_buf, sizeof(out_h_buf), "%g", out_h);
            } else {
                snprintf(out_w_buf, sizeof(out_w_buf), "%g%s",
                         out_w, units.c_str());
                snprintf(out_h_buf, sizeof(out_h_buf), "%g%s",
                         out_h, units.c_str());
            }

            // Replace the FIRST occurrence of width="..." and height="..."
            // in the file. write_svg emits these as the first attributes
            // after viewBox on the root <svg> — they never appear
            // earlier in the byte stream than the root tag opens, so
            // first-match is the root tag's pair.
            auto replace_attr = [&](const char* name,
                                    const std::string& new_val) {
                std::string needle = std::string(" ") + name + "=\"";
                auto a = svg.find(needle);
                if (a == std::string::npos) return false;
                auto b = svg.find('"', a + needle.size());
                if (b == std::string::npos) return false;
                svg.replace(a + needle.size(), b - (a + needle.size()),
                            new_val);
                return true;
            };
            bool ok_w = replace_attr("width",  out_w_buf);
            bool ok_h = replace_attr("height", out_h_buf);
            if (!ok_w || !ok_h) {
                LOG_WARN("SvgWriter: could not patch root width/height for "
                         "export size override in '{}' — file uses doc "
                         "intent for size", path);
            }
        }
    }

    std::ofstream f(path);
    if (!f) {
        LOG_ERROR("SvgWriter: cannot open '{}' for writing", path);
        return false;
    }
    f << svg;
    LOG_DEBUG("SvgWriter: wrote '{}' with export metadata "
              "({}={} fit={})", path, units, size_buf, fit_side);
    return true;
}

} // namespace Curvz
