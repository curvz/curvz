#include "SvgParser.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s147 m2: warp_presets::PRESET_COUNT for preset clamp
#include <pango/pango.h>    // s325 — pango_parse_markup for the per-run spine

// ── s325 — markup decode (the per-run spine's read side) ────────────────────
// Pango markup in the Mgr CDATA -> clean text_content + flat AttrSpan list.
// This is the "born at the parse pump" half of §2: pango_parse_markup hands
// back the clean UTF-8 string (no formatting bytes) and a flat PangoAttrList,
// which we mirror into text_attr_spans. The string goes to text_content so
// every existing reader (fitter, caret, slicer) is untouched — they only ever
// see clean bytes. Only called when the Mgr def carried data-curvz-markup="1";
// raw (pre-s325) buffers never reach here, so legacy load is byte-identical.
static void decode_markup_into(Curvz::SceneNode* mgr, const std::string& payload) {
    if (!mgr) return;
    char*           clean = nullptr;
    PangoAttrList*  attrs = nullptr;
    GError*         err   = nullptr;
    if (!pango_parse_markup(payload.c_str(), (int)payload.size(), 0,
                            &attrs, &clean, nullptr, &err)) {
        // Malformed markup: fall back to treating the payload as plain text
        // so the box still loads (degraded, no formatting) rather than losing
        // the buffer entirely.
        LOG_WARN("SvgParser: TextBoxMgr '{}' markup parse failed ({}); "
                 "loading buffer as plain text",
                 mgr->name, err ? err->message : "unknown");
        if (err) g_error_free(err);
        mgr->text_content = payload;
        mgr->text_attr_spans.clear();
        return;
    }
    mgr->text_content = clean ? std::string(clean) : std::string{};
    if (clean) g_free(clean);
    mgr->text_attr_spans.clear();
    if (attrs) {
        // Enumerate every attribute (filter callback returning FALSE leaves
        // the list intact; we use it purely to walk). Each PangoAttribute maps
        // to one AttrSpan; the value field read depends on the attr klass.
        pango_attr_list_filter(
            attrs,
            [](PangoAttribute* a, gpointer data) -> gboolean {
                auto* spans = static_cast<std::vector<Curvz::AttrSpan>*>(data);
                Curvz::AttrSpan s;
                s.type       = (int)a->klass->type;
                s.start_byte = a->start_index;
                s.end_byte   = a->end_index;
                switch (a->klass->type) {
                    case PANGO_ATTR_WEIGHT:
                    case PANGO_ATTR_STYLE:
                    case PANGO_ATTR_UNDERLINE:
                    case PANGO_ATTR_SIZE:
                    case PANGO_ATTR_ABSOLUTE_SIZE:
                    case PANGO_ATTR_LETTER_SPACING:
                        s.ivalue = ((PangoAttrInt*)a)->value;
                        break;
                    case PANGO_ATTR_FAMILY:
                        s.svalue = ((PangoAttrString*)a)->value
                                       ? ((PangoAttrString*)a)->value : "";
                        break;
                    case PANGO_ATTR_FOREGROUND: {
                        const PangoColor& c = ((PangoAttrColor*)a)->color;
                        // 16-bit channels -> packed 0xRRGGBB.
                        s.ivalue = ((long)(c.red   >> 8) << 16)
                                 | ((long)(c.green >> 8) <<  8)
                                 | ((long)(c.blue  >> 8));
                        break;
                    }
                    default:
                        // Unhandled attr type for m1 — recorded so it survives
                        // (round-trip fidelity) but ignored at the apply seam.
                        s.ivalue = 0;
                        break;
                }
                spans->push_back(std::move(s));
                return FALSE;  // keep the attribute in the list
            },
            &mgr->text_attr_spans);
        pango_attr_list_unref(attrs);
    }
    LOG_INFO("SvgParser: TextBoxMgr '{}' markup decoded — {} bytes, {} spans",
             mgr->name, mgr->text_content.size(), mgr->text_attr_spans.size());
}
#include <fstream>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <set>          // s311 m1c-redux — placed_mgrs tracking
#include <sstream>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>   // s203 m4 — fprintf(stderr, ...) in catastrophic catch

// Minimal hand-written XML parser for our own SVG subset.
// No external library — matches the HANDOFF constraint.

namespace Curvz {

// ── Tiny attribute reader ─────────────────────────────────────────────────────

static std::string attr(const std::string& tag, const std::string& name) {
    // Must be preceded by whitespace or be at start to avoid matching
    // substrings (e.g. "d=" inside "id=")
    char delim = '"';
    for (int pass = 0; pass < 2; ++pass) {
        delim = (pass == 0) ? '"' : '\'';
        std::string key = name + "=" + delim;
        size_t search_from = 0;
        while (true) {
            auto pos = tag.find(key, search_from);
            if (pos == std::string::npos) break;
            // Check that pos is at start of tag or preceded by whitespace
            if (pos == 0 || std::isspace((unsigned char)tag[pos - 1])) {
                pos += key.size();
                auto end = tag.find(delim, pos);
                if (end == std::string::npos) break;
                return tag.substr(pos, end - pos);
            }
            search_from = pos + 1;
        }
    }
    return {};
}

static double dbl(const std::string& s, double def = 0.0) {
    if (s.empty()) return def;
    try { return std::stod(s); } catch (...) { return def; }
}

// ── decode_warp_envelope ─────────────────────────────────────────────────────
// Inverse of SvgWriter::encode_warp_envelope. Parses "x,y,cx1,cy1,cx2,cy2;..."
// into a PathData with Smooth anchors and no closed flag. Malformed
// input (wrong field count per anchor, parse failures) → returns an
// empty PathData. Caller falls back to default-identity in that case
// via rebuild_warp_caches.
static PathData decode_warp_envelope(const std::string& s) {
    PathData pd;
    pd.closed = false;
    if (s.empty()) return pd;
    // Split on ';' into anchor tokens
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(';', start);
        std::string anchor_tok =
            (end == std::string::npos) ? s.substr(start) : s.substr(start, end - start);
        // Split anchor_tok on ',' into exactly 6 fields
        double vals[6] = {0, 0, 0, 0, 0, 0};
        int    count = 0;
        size_t p = 0;
        while (p <= anchor_tok.size() && count < 6) {
            size_t c = anchor_tok.find(',', p);
            std::string field =
                (c == std::string::npos) ? anchor_tok.substr(p)
                                         : anchor_tok.substr(p, c - p);
            try {
                vals[count] = field.empty() ? 0.0 : std::stod(field);
            } catch (...) {
                // Malformed — abort, return empty envelope.
                return PathData();
            }
            ++count;
            if (c == std::string::npos) break;
            p = c + 1;
        }
        if (count != 6) return PathData();  // wrong field count per anchor
        BezierNode n;
        n.x = vals[0];  n.y   = vals[1];
        n.cx1 = vals[2]; n.cy1 = vals[3];
        n.cx2 = vals[4]; n.cy2 = vals[5];
        n.type = BezierNode::Type::Smooth;
        pd.nodes.push_back(n);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    // Degenerate (single anchor or zero anchors) — still return. Parser
    // handler clamps to "empty envelope → default identity".
    if (pd.nodes.size() < 2) return PathData();
    return pd;
}

// S90 Stage 2 — file-scope pointer to the parser's gradient_defs map.
// Set on entry to parse_svg, cleared on exit. parse_fill reads it when
// resolving url(#id) references. The pointer-based approach avoids
// threading a new parameter through apply_style_attrs and 9 call sites.
// Safe because parse_svg isn't reentrant or concurrent.
static const std::map<std::string, FillStyle>* g_gradient_defs = nullptr;

// Parse a fill/stroke colour value. Recognises url(#id) references via
// g_gradient_defs (S90 Stage 2). Other paint-server forms (patterns,
// foreign mesh gradients) fall back to CurrentColor with a log line.
static FillStyle parse_fill(const std::string& val_raw)
{
    // Trim leading/trailing whitespace
    std::string val = val_raw;
    while (!val.empty() && val.front() == ' ') val.erase(val.begin());
    while (!val.empty() && val.back()  == ' ') val.pop_back();

    FillStyle f;
    if (val.empty() || val == "currentColor") {
        f.type = FillStyle::Type::CurrentColor;
    } else if (val == "none") {
        f.type = FillStyle::Type::None;
    } else if (val.rfind("url(#", 0) == 0) {
        // S90 Stage 2 — gradient reference. Strip "url(#" prefix and ")"
        // suffix, look up in g_gradient_defs. Hit → return the gradient
        // FillStyle (copy). Miss → degrade to CurrentColor and log; this
        // covers (a) foreign SVGs using paint servers Curvz doesn't model
        // (e.g. patterns, mesh gradients), and (b) malformed files where
        // the def is missing. Either way the shape gets a safe fallback.
        std::string id;
        if (val.size() > 5 && val.back() == ')')
            id = val.substr(5, val.size() - 6);
        if (g_gradient_defs) {
            auto it = g_gradient_defs->find(id);
            if (it != g_gradient_defs->end()) {
                return it->second;  // copy the resolved gradient
            }
        }
        LOG_INFO("SvgParser: unresolved paint-server reference '{}' — "
                 "falling back to CurrentColor", val);
        f.type = FillStyle::Type::CurrentColor;
    } else if (val.size() == 7 && val[0] == '#') {
        // #RRGGBB
        f.type = FillStyle::Type::Solid;
        unsigned rgb = 0;
        sscanf(val.c_str() + 1, "%x", &rgb);
        f.r = ((rgb >> 16) & 0xff) / 255.0;
        f.g = ((rgb >>  8) & 0xff) / 255.0;
        f.b = ( rgb        & 0xff) / 255.0;
    } else if (val.size() == 4 && val[0] == '#') {
        // #RGB → #RRGGBB
        f.type = FillStyle::Type::Solid;
        unsigned rgb = 0;
        sscanf(val.c_str() + 1, "%x", &rgb);
        int r = (rgb >> 8) & 0xf;
        int g = (rgb >> 4) & 0xf;
        int b =  rgb       & 0xf;
        f.r = (r | (r << 4)) / 255.0;
        f.g = (g | (g << 4)) / 255.0;
        f.b = (b | (b << 4)) / 255.0;
    } else if (val.rfind("rgb(", 0) == 0) {
        // rgb(r,g,b) — integer 0-255
        f.type = FillStyle::Type::Solid;
        int r = 0, g = 0, b = 0;
        sscanf(val.c_str(), "rgb(%d,%d,%d)", &r, &g, &b);
        f.r = r / 255.0; f.g = g / 255.0; f.b = b / 255.0;
    } else if (val.rfind("rgba(", 0) == 0) {
        // rgba(r,g,b,a) — integer 0-255, alpha 0.0-1.0
        f.type = FillStyle::Type::Solid;
        int r = 0, g = 0, b = 0; float a = 1.0f;
        sscanf(val.c_str(), "rgba(%d,%d,%d,%f)", &r, &g, &b, &a);
        f.r = r / 255.0; f.g = g / 255.0; f.b = b / 255.0;
        // alpha handled separately via opacity if needed
    } else {
        // Named colours — common subset
        struct { const char* name; uint32_t rgb; } named[] = {
            {"black",   0x000000}, {"white",   0xffffff},
            {"red",     0xff0000}, {"green",   0x008000},
            {"blue",    0x0000ff}, {"yellow",  0xffff00},
            {"cyan",    0x00ffff}, {"magenta", 0xff00ff},
            {"orange",  0xffa500}, {"purple",  0x800080},
            {"grey",    0x808080}, {"gray",    0x808080},
            {"silver",  0xc0c0c0}, {"navy",    0x000080},
            {"teal",    0x008080}, {"maroon",  0x800000},
            {"lime",    0x00ff00}, {"aqua",    0x00ffff},
            {"fuchsia", 0xff00ff}, {"olive",   0x808000},
            {nullptr, 0}
        };
        for (int i = 0; named[i].name; ++i) {
            if (val == named[i].name) {
                f.type = FillStyle::Type::Solid;
                uint32_t rgb = named[i].rgb;
                f.r = ((rgb >> 16) & 0xff) / 255.0;
                f.g = ((rgb >>  8) & 0xff) / 255.0;
                f.b = ( rgb        & 0xff) / 255.0;
                break;
            }
        }
        // If still unrecognised, leave as CurrentColor (safe default)
        if (f.type != FillStyle::Type::Solid)
            f.type = FillStyle::Type::CurrentColor;
    }
    return f;
}

// Parse a CSS property value from a style="" string.
// e.g. style_prop("fill:red; stroke:none;", "fill") → "red"
static std::string style_prop(const std::string& style, const std::string& prop) {
    std::string key = prop + ":";
    size_t search_from = 0;
    while (true) {
        auto pos = style.find(key, search_from);
        if (pos == std::string::npos) return {};
        // Must be at start of string or preceded by ';' or whitespace
        // to avoid matching "fill-rule:" when searching for "fill:"
        if (pos == 0 || style[pos-1] == ';' || style[pos-1] == ' ') {
            pos += key.size();
            // Skip whitespace
            while (pos < style.size() && style[pos] == ' ') ++pos;
            auto end = style.find(';', pos);
            std::string val = (end == std::string::npos)
                ? style.substr(pos)
                : style.substr(pos, end - pos);
            // Trim trailing whitespace
            while (!val.empty() && val.back() == ' ') val.pop_back();
            return val;
        }
        search_from = pos + 1;
    }
}

static LineCap parse_linecap(const std::string& v) {
    if (v == "round")  return LineCap::Round;
    if (v == "square") return LineCap::Square;
    return LineCap::Butt;
}

static LineJoin parse_linejoin(const std::string& v) {
    if (v == "round") return LineJoin::Round;
    if (v == "bevel") return LineJoin::Bevel;
    return LineJoin::Miter;
}

// ── S97 m1 — drop shadow attrs reader ────────────────────────────────────────
// Reads data-curvz-shadow-* attributes (written by SvgWriter::shadow_attr_str)
// onto the host node. The native <filter> element is intentionally NOT parsed
// — Curvz emits it for foreign renderers but treats data-curvz-shadow-* as
// the source of truth on load, same dual-source pattern as data-curvz-name
// over SVG id.
//
// Gated by the "data-curvz-shadow" marker attr (always emitted as "1" by the
// writer). Missing marker → leave shadow_enabled at default (false). Present
// marker → enable and read all six numeric fields, plus the colour as a
// 6-hex-char string. Each individual field is independent: missing fields
// fall back to the SceneNode default values defined in the header.
//
// Called from apply_style_attrs (covers Path / Compound / Text / Image) and
// directly from each container parse site (Group / Blend / Warp / ClipGroup)
// since those don't pass through apply_style_attrs.
static void apply_shadow_attrs(GlyphObject& obj, const std::string& tag) {
    auto marker = attr(tag, "data-curvz-shadow");
    if (marker.empty()) return;
    obj.shadow_enabled = true;
    auto v = attr(tag, "data-curvz-shadow-dx");
    if (!v.empty()) obj.shadow_dx = dbl(v, obj.shadow_dx);
    v = attr(tag, "data-curvz-shadow-dy");
    if (!v.empty()) obj.shadow_dy = dbl(v, obj.shadow_dy);
    v = attr(tag, "data-curvz-shadow-blur");
    if (!v.empty()) obj.shadow_blur = std::max(0.0, dbl(v, obj.shadow_blur));
    v = attr(tag, "data-curvz-shadow-color-a");
    if (!v.empty()) obj.shadow_color_a = std::max(0.0, std::min(1.0, dbl(v, obj.shadow_color_a)));
    v = attr(tag, "data-curvz-shadow-opacity");
    if (!v.empty()) obj.shadow_opacity = std::max(0.0, std::min(1.0, dbl(v, obj.shadow_opacity)));
    // Colour: 6 hex chars "rrggbb" (no "#" prefix — emitted bare for
    // compactness, distinct from CSS hex notation). Tolerate a leading "#"
    // anyway in case of hand-edits.
    v = attr(tag, "data-curvz-shadow-color");
    if (!v.empty()) {
        std::string h = (v.size() && v[0] == '#') ? v.substr(1) : v;
        if (h.size() >= 6) {
            auto from_hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return -1;
            };
            int r1 = from_hex(h[0]), r2 = from_hex(h[1]);
            int g1 = from_hex(h[2]), g2 = from_hex(h[3]);
            int b1 = from_hex(h[4]), b2 = from_hex(h[5]);
            // All-or-nothing — any malformed nibble keeps the default.
            if (r1 >= 0 && r2 >= 0 && g1 >= 0 && g2 >= 0 && b1 >= 0 && b2 >= 0) {
                obj.shadow_color_r = ((r1 << 4) | r2) / 255.0;
                obj.shadow_color_g = ((g1 << 4) | g2) / 255.0;
                obj.shadow_color_b = ((b1 << 4) | b2) / 255.0;
            }
        }
    }
}

static void apply_style_attrs(GlyphObject& obj, const std::string& tag) {
    // First apply CSS style="" attribute as base values
    auto style_val = attr(tag, "style");
    if (!style_val.empty()) {
        auto v = style_prop(style_val, "fill");
        if (!v.empty()) obj.fill = parse_fill(v);
        v = style_prop(style_val, "stroke");
        if (!v.empty()) obj.stroke.paint = parse_fill(v);
        v = style_prop(style_val, "stroke-width");
        if (!v.empty()) obj.stroke.width = dbl(v, 2.0);
        v = style_prop(style_val, "stroke-linecap");
        if (!v.empty()) obj.stroke.cap = parse_linecap(v);
        v = style_prop(style_val, "stroke-linejoin");
        if (!v.empty()) obj.stroke.join = parse_linejoin(v);
        v = style_prop(style_val, "opacity");
        if (!v.empty()) obj.opacity = dbl(v, 1.0);
        v = style_prop(style_val, "stroke-opacity");
        if (!v.empty()) obj.stroke.opacity = dbl(v, 1.0);
        // fill-opacity: multiply into fill alpha (stored separately if needed)
        v = style_prop(style_val, "fill-opacity");
        if (!v.empty()) { /* stored on stroke.opacity for now — future enhancement */ }
        // display / visibility
        v = style_prop(style_val, "display");
        if (v == "none") obj.visible = false;
        v = style_prop(style_val, "visibility");
        if (v == "hidden" || v == "collapse") obj.visible = false;
    }

    // Standalone presentation attributes override style="" values
    auto fill_val   = attr(tag, "fill");
    auto stroke_val = attr(tag, "stroke");
    auto sw_val     = attr(tag, "stroke-width");
    auto sc_val     = attr(tag, "stroke-linecap");
    auto sj_val     = attr(tag, "stroke-linejoin");
    auto op_val     = attr(tag, "opacity");
    auto so_val     = attr(tag, "stroke-opacity");
    auto fo_val     = attr(tag, "fill-opacity");
    auto disp_val   = attr(tag, "display");
    auto vis_val    = attr(tag, "visibility");

    if (!fill_val.empty())   obj.fill  = parse_fill(fill_val);
    if (!stroke_val.empty()) obj.stroke.paint = parse_fill(stroke_val);
    if (!sw_val.empty())     obj.stroke.width = dbl(sw_val, 2.0);
    if (!sc_val.empty())     obj.stroke.cap   = parse_linecap(sc_val);
    if (!sj_val.empty())     obj.stroke.join  = parse_linejoin(sj_val);
    if (!op_val.empty())     obj.opacity      = dbl(op_val, 1.0);
    if (!so_val.empty())     obj.stroke.opacity = dbl(so_val, 1.0);
    if (disp_val == "none")  obj.visible = false;
    if (vis_val == "hidden" || vis_val == "collapse") obj.visible = false;

    // Swatch-binding sidecar attrs (s70 M2). When present, the paint is a
    // SwatchRef with this id — the fill=/stroke= attrs above are the cached
    // resolved render colour that the Curvz-unaware world sees. On load,
    // the id is stored on the SceneNode and the cached FillStyle stands as
    // a dead-ref fallback until the SwatchLibrary confirms the id is live
    // (or doesn't — stale ids degrade to their cached colour via
    // resolve_paint, see Paint.hpp).
    //
    // A non-Curvz editor that stripped these attrs on its round-trip would
    // leave fill=/stroke= intact and simply lose the binding. That's the
    // designed lossy-fallback behaviour — same principle as data-curvz-types.
    auto fs_val = attr(tag, "data-curvz-fill-swatch");
    if (!fs_val.empty()) obj.fill_swatch_id = fs_val;
    auto ss_val = attr(tag, "data-curvz-stroke-swatch");
    if (!ss_val.empty()) obj.stroke_swatch_id = ss_val;

    // S77 m3c — style-binding sidecar attr. Same lossy-fallback model as the
    // swatch sidecars above: fill=/stroke= are the resolved render cache and
    // would suffice on their own; data-curvz-style is the binding key that
    // lets CurvzProject::load re-materialise the node from the referenced
    // Style after parse. Empty / missing attribute → leave bound_style at
    // its default (empty string), which is what pre-m3c projects produce
    // and what the post-load walk treats as "no binding to resolve".
    //
    // Value shape: "app:<slug>" or "stl_<uuid>". Stored verbatim — id
    // resolution and unknown-id fallback (clear bound_style, keep cached
    // fill/stroke) is the post-load walk's job, not ours.
    auto bound_style_val = attr(tag, "data-curvz-style");
    if (!bound_style_val.empty()) obj.bound_style = bound_style_val;

    // S97 m1 — drop shadow. Read data-curvz-shadow-* attrs onto the node.
    apply_shadow_attrs(obj, tag);
}

// ── Tag splitter ──────────────────────────────────────────────────────────────
// Yields each complete XML tag from the source string.
//
// s311 m1c-redux — CDATA support. A naive `find('>', open)` cuts a
// <![CDATA[...]]> token at the first '>' inside the payload, which is wrong
// when the payload itself contains '>' (rare in textbox buffers but possible).
// Special-case the CDATA prefix: when we see `<![CDATA[`, find the matching
// `]]>` and emit the whole stretch as one token. The token form returned to
// the main loop is `![CDATA[<payload>]]` (the same convention as the rest
// of split_tags — leading `<` and trailing `>` stripped). Downstream the
// main loop recognises the `![CDATA[` prefix and harvests the payload.
static std::vector<std::string> split_tags(const std::string& src) {
    std::vector<std::string> tags;
    size_t i = 0;
    while (i < src.size()) {
        auto open = src.find('<', i);
        if (open == std::string::npos) break;
        // CDATA fast-path: <![CDATA[...]]> — find the closing ]]> rather
        // than the next bare '>'. If the closing marker is missing (file
        // truncated mid-CDATA), bail out same as the naive case.
        if (src.compare(open, 9, "<![CDATA[") == 0) {
            auto close = src.find("]]>", open + 9);
            if (close == std::string::npos) break;
            // Token body excludes the outer < and the closing > of ]]>.
            tags.push_back(src.substr(open + 1, (close + 2) - open - 1));
            i = close + 3;
            continue;
        }
        auto close = src.find('>', open);
        if (close == std::string::npos) break;
        tags.push_back(src.substr(open + 1, close - open - 1));
        i = close + 1;
    }
    return tags;
}

// ── SVG path d parser ─────────────────────────────────────────────────────────
// Parses the "d" attribute of a <path> element into a PathData.
// We only need to handle the commands we write: M, C, L, Z (and their
// lowercase relative variants). Enough for full round-trip of Curvz output.

static double next_num(const char*& p) {
    while (*p && (std::isspace((unsigned char)*p) || *p == ',')) ++p;
    char* end;
    double v = std::strtod(p, &end);
    p = end;
    return v;
}

// Arc flags are single bits (0 or 1) and may be written without separators
// e.g. "0 00.5 1.0" = laf=0, sf=0, x=0.5, y=1.0.
// Must read exactly one character after skipping whitespace/commas.
static int next_flag(const char*& p) {
    while (*p && (std::isspace((unsigned char)*p) || *p == ',')) ++p;
    int v = (*p == '1') ? 1 : 0;
    if (*p == '0' || *p == '1') ++p;
    return v;
}

// ── SVG transform parser ──────────────────────────────────────────────────────
// Parses a transform="" attribute into a 2D affine matrix [a,b,c,d,e,f]
// matching the SVG matrix(a,b,c,d,e,f) convention:
//   x' = a*x + c*y + e
//   y' = b*x + d*y + f
struct AffineMatrix {
    double a=1,b=0,c=0,d=1,e=0,f=0;
    bool is_identity() const {
        return a==1&&b==0&&c==0&&d==1&&e==0&&f==0;
    }
};

static AffineMatrix affine_multiply(const AffineMatrix& m, const AffineMatrix& n) {
    // m * n
    AffineMatrix r;
    r.a = m.a*n.a + m.c*n.b;
    r.b = m.b*n.a + m.d*n.b;
    r.c = m.a*n.c + m.c*n.d;
    r.d = m.b*n.c + m.d*n.d;
    r.e = m.a*n.e + m.c*n.f + m.e;
    r.f = m.b*n.e + m.d*n.f + m.f;
    return r;
}

static AffineMatrix parse_transform(const std::string& xfm) {
    AffineMatrix result; // identity
    if (xfm.empty()) return result;

    const char* p = xfm.c_str();
    while (*p) {
        // Skip whitespace and commas
        while (*p && (std::isspace((unsigned char)*p) || *p == ',')) ++p;
        if (!*p) break;

        // Read function name
        const char* name_start = p;
        while (*p && std::isalpha((unsigned char)*p)) ++p;
        std::string name(name_start, p);

        // Skip to '('
        while (*p && *p != '(') ++p;
        if (!*p) break;
        ++p; // consume '('

        // Read comma-separated numbers
        std::vector<double> args;
        while (*p && *p != ')') {
            while (*p && (std::isspace((unsigned char)*p) || *p == ',')) ++p;
            if (*p == ')') break;
            char* end;
            double v = std::strtod(p, &end);
            if (end == p) { ++p; continue; }
            args.push_back(v);
            p = end;
        }
        if (*p == ')') ++p;

        AffineMatrix m; // identity
        if (name == "matrix" && args.size() >= 6) {
            m.a=args[0]; m.b=args[1]; m.c=args[2];
            m.d=args[3]; m.e=args[4]; m.f=args[5];
        } else if (name == "translate") {
            m.e = args.size() >= 1 ? args[0] : 0;
            m.f = args.size() >= 2 ? args[1] : 0;
        } else if (name == "scale") {
            m.a = args.size() >= 1 ? args[0] : 1;
            m.d = args.size() >= 2 ? args[1] : m.a;
        } else if (name == "rotate") {
            double angle = args.size() >= 1 ? args[0] * M_PI / 180.0 : 0;
            double cx_r  = args.size() >= 2 ? args[1] : 0;
            double cy_r  = args.size() >= 3 ? args[2] : 0;
            double cosA  = std::cos(angle), sinA = std::sin(angle);
            // rotate(a,cx,cy) = translate(cx,cy) rotate(a) translate(-cx,-cy)
            m.a =  cosA; m.b = sinA;
            m.c = -sinA; m.d = cosA;
            m.e = cx_r - cx_r*cosA + cy_r*sinA;
            m.f = cy_r - cx_r*sinA - cy_r*cosA;
        } else if (name == "skewX") {
            double angle = args.size() >= 1 ? args[0] * M_PI / 180.0 : 0;
            m.c = std::tan(angle);
        } else if (name == "skewY") {
            double angle = args.size() >= 1 ? args[0] * M_PI / 180.0 : 0;
            m.b = std::tan(angle);
        }

        result = affine_multiply(result, m);
    }
    return result;
}

// Apply an affine matrix to all node positions and handles in a PathData.
static void apply_transform_to_path(PathData& pd, const AffineMatrix& m) {
    if (m.is_identity()) return;
    auto xfm_pt = [&](double& x, double& y) {
        double nx = m.a*x + m.c*y + m.e;
        double ny = m.b*x + m.d*y + m.f;
        x = nx; y = ny;
    };
    for (auto& nd : pd.nodes) {
        xfm_pt(nd.x,   nd.y);
        xfm_pt(nd.cx1, nd.cy1);
        xfm_pt(nd.cx2, nd.cy2);
    }
}

// Apply an affine matrix to all path children of a SceneNode tree.
static void apply_transform_to_node(SceneNode& node, const AffineMatrix& m) {
    if (m.is_identity()) return;
    if (node.is_path() && node.path)
        apply_transform_to_path(*node.path, m);
    for (auto& child : node.children)
        apply_transform_to_node(*child, m);
}

// Shared arc-to-cubic-bezier helper used by both parse_path_d and parse_path_d_multi.
// Appends bezier nodes to out_nodes for the arc from (cx,cy) to (ex,ey).
// Updates cx,cy to endpoint on return.
static void arc_to_bezier(std::vector<BezierNode>& out_nodes,
                           double& cx, double& cy,
                           double arx, double ary, double /*xrot*/,
                           int laf, int sf, double ex, double ey) {
    if (arx<=0||ary<=0||(std::abs(ex-cx)<1e-6&&std::abs(ey-cy)<1e-6)){
        if (!out_nodes.empty()){out_nodes.back().cx2=cx;out_nodes.back().cy2=cy;}
        BezierNode nd;nd.x=ex;nd.y=ey;nd.cx1=ex;nd.cy1=ey;nd.cx2=ex;nd.cy2=ey;
        nd.type=BezierNode::Type::Corner;out_nodes.push_back(nd);
        cx=ex;cy=ey;return;
    }
    double dx2=(cx-ex)/2, dy2=(cy-ey)/2;
    double rx=std::abs(arx), ry=std::abs(ary);
    double lambda=dx2*dx2/(rx*rx)+dy2*dy2/(ry*ry);
    if(lambda>1){rx*=std::sqrt(lambda);ry*=std::sqrt(lambda);}
    double sign=(laf==sf)?-1.0:1.0;
    double num_v=rx*rx*ry*ry-rx*rx*dy2*dy2-ry*ry*dx2*dx2;
    double den_v=rx*rx*dy2*dy2+ry*ry*dx2*dx2;
    double sq=std::sqrt(std::max(0.0,num_v/den_v));
    double cxp= sign*sq*(rx*dy2/ry);
    double cyp=-sign*sq*(ry*dx2/rx);
    double ccx=(cx+ex)/2+cxp, ccy=(cy+ey)/2+cyp;
    auto angle_fn=[](double ux,double uy,double vx,double vy){
        double d=std::sqrt(ux*ux+uy*uy)*std::sqrt(vx*vx+vy*vy);
        if(d<1e-10)return 0.0;
        double a=std::acos(std::max(-1.0,std::min(1.0,(ux*vx+uy*vy)/d)));
        return (ux*vy-uy*vx<0)?-a:a;
    };
    double th1=angle_fn(1,0,(dx2-cxp)/rx,(dy2-cyp)/ry);
    double dth=angle_fn((dx2-cxp)/rx,(dy2-cyp)/ry,(-dx2-cxp)/rx,(-dy2-cyp)/ry);
    if(!sf&&dth>0) dth-=2*M_PI;
    if( sf&&dth<0) dth+=2*M_PI;
    int nsegs=std::max(1,(int)std::ceil(std::abs(dth)/(M_PI/2)));
    double dt=dth/nsegs;
    double alpha=std::sin(dt)*(std::sqrt(4+3*std::tan(dt/2)*std::tan(dt/2))-1)/3;
    double th=th1;
    double dpx=-rx*std::sin(th), dpy=ry*std::cos(th);
    if(!out_nodes.empty()){
        out_nodes.back().cx2=(ccx+rx*std::cos(th))+alpha*dpx;
        out_nodes.back().cy2=(ccy+ry*std::sin(th))+alpha*dpy;
    }
    for(int seg=0;seg<nsegs;++seg){
        th+=dt;
        double qx=ccx+rx*std::cos(th), qy=ccy+ry*std::sin(th);
        double dqx=-rx*std::sin(th), dqy=ry*std::cos(th);
        BezierNode nd;
        nd.x=qx;nd.y=qy;
        nd.cx1=qx-alpha*dqx;nd.cy1=qy-alpha*dqy;
        nd.cx2=qx;nd.cy2=qy;
        nd.type=BezierNode::Type::Smooth;
        out_nodes.push_back(nd);
        if(seg<nsegs-1){
            out_nodes.back().cx2=qx+alpha*dqx;
            out_nodes.back().cy2=qy+alpha*dqy;
        }
    }
    cx=ex;cy=ey;
}

static PathData parse_path_d(const std::string& d) {
    PathData pd;
    if (d.empty()) return pd;

    const char* p = d.c_str();
    double cx = 0, cy = 0;   // current point
    double mx = 0, my = 0;   // moveto point (for Z)
    // Track the in-handle (cx1) of the most recently added node so that
    // S/s and T/t can compute the correct reflected control point.
    // SVG spec: S reflects the "second control point of the previous command",
    // which is cx1 of the node that command ended on.
    // After C: last_cx1 = x2,y2 of that C (the in-handle of the new node).
    // After S: last_cx1 = x2,y2 of that S (the explicit handle, now the in-handle).
    // After L/H/V/M/Z: last_cx1 = anchor (degenerate → reflection = identity).
    double last_cx1 = 0, last_cy1 = 0;
    char   cmd  = 0;
    bool   first = true;

    while (*p) {
        while (*p && (std::isspace((unsigned char)*p) || *p == ',')) ++p;
        if (!*p) break;

        // A letter starts a new command; a digit/sign/-/. continues prev
        if (std::isalpha((unsigned char)*p)) {
            cmd = *p++;
        } else if (first) {
            // Skip any non-alpha garbage before the first command
            if (!std::isdigit((unsigned char)*p) && *p != '-' && *p != '+' && *p != '.') {
                ++p;
                continue;
            }
            break; // malformed numeric start with no command
        }

        switch (cmd) {
        // ── Moveto ──────────────────────────────────────────────────────
        case 'M': case 'm': {
            double x = next_num(p);
            double y = next_num(p);
            if (cmd == 'm' && !first) { x += cx; y += cy; }
            cx = mx = x; cy = my = y;
            last_cx1 = cx; last_cy1 = cy;
            BezierNode nd;
            nd.x = x; nd.y = y;
            nd.cx1 = x; nd.cy1 = y;
            nd.cx2 = x; nd.cy2 = y;
            nd.type = BezierNode::Type::Smooth;
            pd.nodes.push_back(nd);
            first = false;
            // Subsequent coordinate pairs after M become implicit L
            cmd = (cmd == 'M') ? 'L' : 'l';
            break;
        }
        // ── Cubicto ─────────────────────────────────────────────────────
        case 'C': case 'c': {
            double x1 = next_num(p), y1 = next_num(p);
            double x2 = next_num(p), y2 = next_num(p);
            double x  = next_num(p), y  = next_num(p);
            if (cmd == 'c') { x1+=cx; y1+=cy; x2+=cx; y2+=cy; x+=cx; y+=cy; }
            // x1,y1 is the out-handle of the CURRENT (last) node
            if (!pd.nodes.empty()) {
                pd.nodes.back().cx2 = x1;
                pd.nodes.back().cy2 = y1;
            }
            // x2,y2 is the in-handle of the NEW node; x,y is the new anchor
            BezierNode nd;
            nd.x   = x;  nd.y   = y;
            nd.cx1 = x2; nd.cy1 = y2;
            nd.cx2 = x;  nd.cy2 = y;  // out-handle degenerate until next cmd sets it
            nd.type = BezierNode::Type::Smooth;
            pd.nodes.push_back(nd);
            // Track x2,y2 — the in-handle of the node just added.
            // S reflects this to compute the in-handle of its new node.
            last_cx1 = x2; last_cy1 = y2;
            cx = x; cy = y;
            break;
        }
        // ── Lineto ──────────────────────────────────────────────────────
        case 'L': case 'l': {
            double x = next_num(p), y = next_num(p);
            if (cmd == 'l') { x += cx; y += cy; }
            // Make a corner node with degenerate handles (straight line)
            if (!pd.nodes.empty()) {
                pd.nodes.back().cx2 = cx;
                pd.nodes.back().cy2 = cy;
            }
            BezierNode nd;
            nd.x = x; nd.y = y;
            nd.cx1 = x; nd.cy1 = y;
            nd.cx2 = x; nd.cy2 = y;
            nd.type = BezierNode::Type::Corner;
            pd.nodes.push_back(nd);
            last_cx1 = cx; last_cy1 = cy; // degenerate — S after L reflects identity
            cx = x; cy = y;
            break;
        }
        // ── Horizontal / Vertical lineto ─────────────────────────────────
        case 'H': case 'h': {
            double x = next_num(p);
            if (cmd == 'h') x += cx;
            if (!pd.nodes.empty()) { pd.nodes.back().cx2 = cx; pd.nodes.back().cy2 = cy; }
            BezierNode nd; nd.x=x; nd.y=cy; nd.cx1=x; nd.cy1=cy; nd.cx2=x; nd.cy2=cy;
            nd.type = BezierNode::Type::Corner;
            pd.nodes.push_back(nd);
            last_cx1 = cx; last_cy1 = cy;
            cx = x;
            break;
        }
        case 'V': case 'v': {
            double y = next_num(p);
            if (cmd == 'v') y += cy;
            if (!pd.nodes.empty()) { pd.nodes.back().cx2 = cx; pd.nodes.back().cy2 = cy; }
            BezierNode nd; nd.x=cx; nd.y=y; nd.cx1=cx; nd.cy1=y; nd.cx2=cx; nd.cy2=y;
            nd.type = BezierNode::Type::Corner;
            pd.nodes.push_back(nd);
            last_cx1 = cx; last_cy1 = cy;
            cy = y;
            break;
        }
        // ── Closepath ───────────────────────────────────────────────────
        case 'Z': case 'z': {
            pd.closed = true;
            cx = mx; cy = my;
            last_cx1 = cx; last_cy1 = cy;
            // The Curvz writer emits n C segments for n nodes, with the last
            // segment ending at nodes[0].  The C parser creates a new node
            // there, giving nodes[0] a duplicate at the back.  Remove it so
            // node count stays stable across save/load round-trips.
            if (pd.nodes.size() >= 2) {
                const auto& first_nd = pd.nodes.front();
                const auto& last_nd  = pd.nodes.back();
                constexpr double EPS = 1e-4;
                if (std::abs(last_nd.x - first_nd.x) < EPS &&
                    std::abs(last_nd.y - first_nd.y) < EPS) {
                    pd.nodes.front().cx1 = last_nd.cx1;
                    pd.nodes.front().cy1 = last_nd.cy1;
                    pd.nodes.pop_back();
                }
            }
            break;
        }
        // ── Smooth cubic (S/s) ─────────────────────────────────────────
        case 'S': case 's': {
            // S x2 y2 x y
            // ix,iy = reflected first control point → out-handle of previous node
            // x2,y2 = explicit second control point → in-handle of new node
            double x2 = next_num(p), y2 = next_num(p);
            double x  = next_num(p), y  = next_num(p);
            if (cmd == 's') { x2+=cx; y2+=cy; x+=cx; y+=cy; }
            // Reflect last_cx1 (in-handle of current endpoint) to get
            // the implied out-handle of the previous node
            double ix = 2*cx - last_cx1;
            double iy = 2*cy - last_cy1;
            if (!pd.nodes.empty()) {
                pd.nodes.back().cx2 = ix;  // out-handle of prev node
                pd.nodes.back().cy2 = iy;
            }
            BezierNode nd;
            nd.x   = x;   nd.y   = y;
            nd.cx1 = x2;  nd.cy1 = y2;  // explicit in-handle of new node
            nd.cx2 = x;   nd.cy2 = y;   // out-handle degenerate until next cmd
            nd.type = BezierNode::Type::Smooth;
            pd.nodes.push_back(nd);
            // x2,y2 is the in-handle of the new node — track for next S
            last_cx1 = x2; last_cy1 = y2;
            cx=x; cy=y;
            break;
        }
        // ── Quadratic (Q/q) — convert to cubic ─────────────────────────
        case 'Q': case 'q': {
            double qx1=next_num(p), qy1=next_num(p);
            double x=next_num(p),   y=next_num(p);
            if (cmd=='q'){qx1+=cx;qy1+=cy;x+=cx;y+=cy;}
            // Degree-elevation: cubic control points from quadratic
            double cx1 = cx  + 2.0/3.0*(qx1-cx);
            double cy1 = cy  + 2.0/3.0*(qy1-cy);
            double cx2 = x   + 2.0/3.0*(qx1-x);
            double cy2 = y   + 2.0/3.0*(qy1-y);
            if (!pd.nodes.empty()) { pd.nodes.back().cx2=cx1; pd.nodes.back().cy2=cy1; }
            BezierNode nd;
            nd.x=x; nd.y=y; nd.cx1=cx2; nd.cy1=cy2; nd.cx2=x; nd.cy2=y;
            nd.type=BezierNode::Type::Smooth;
            pd.nodes.push_back(nd);
            last_cx1 = qx1; last_cy1 = qy1; // store original quad control pt for T
            cx=x; cy=y;
            break;
        }
        // ── Smooth quadratic (T/t) — reflect prev quadratic control point ──
        case 'T': case 't': {
            double x=next_num(p), y=next_num(p);
            if (cmd=='t'){x+=cx;y+=cy;}
            // Reflect previous quadratic control point (stored in last_cx1)
            double qx1 = 2*cx - last_cx1;
            double qy1 = 2*cy - last_cy1;
            // Degree-elevate quadratic to cubic
            double bcx1 = cx + 2.0/3.0*(qx1-cx);
            double bcy1 = cy + 2.0/3.0*(qy1-cy);
            double bcx2 = x  + 2.0/3.0*(qx1-x);
            double bcy2 = y  + 2.0/3.0*(qy1-y);
            if (!pd.nodes.empty()){pd.nodes.back().cx2=bcx1;pd.nodes.back().cy2=bcy1;}
            BezierNode nd; nd.x=x;nd.y=y;nd.cx1=bcx2;nd.cy1=bcy2;nd.cx2=x;nd.cy2=y;
            nd.type=BezierNode::Type::Smooth;
            pd.nodes.push_back(nd);
            last_cx1 = qx1; last_cy1 = qy1; // store reflected quad pt for chained T
            cx=x;cy=y;
            break;
        }
        // ── Arc (A/a) — approximate as cubic bezier segments ───────────
        // SVG arc: A rx ry x-rotation large-arc-flag sweep-flag x y
        case 'A': case 'a': {
            double arx=next_num(p), ary=next_num(p);
            double xrot=next_num(p);
            int laf=(int)next_flag(p), sf=(int)next_flag(p);
            double ex=next_num(p), ey=next_num(p);
            if (cmd=='a'){ex+=cx;ey+=cy;}
            arc_to_bezier(pd.nodes, cx, cy, arx, ary, xrot, laf, sf, ex, ey);
            last_cx1 = cx; last_cy1 = cy;
            break;
        }
        default:
            ++p; // skip unknown command char
            break;
        }
    }

    // Strip trailing single-node degenerate subpaths (e.g. the "m 0 0"
    // terminator that Cairo/Inkscape sometimes appends). A subpath with
    // exactly 1 node and no segments contributes nothing visible.
    // We detect them by finding nodes that were M-start points with no
    // subsequent segment nodes — i.e. the last node if it equals the
    // second-to-last M start and nothing followed.
    // Simple heuristic: if the last node is a degenerate moveto (cx1==x,
    // cy1==y, cx2==x, cy2==y, type==Smooth) and the path is closed or
    // the node came after a Z, remove it.
    while (pd.nodes.size() >= 2) {
        const auto& last = pd.nodes.back();
        constexpr double EPS2 = 1e-5;
        bool degen = (std::abs(last.cx1 - last.x) < EPS2 &&
                      std::abs(last.cy1 - last.y) < EPS2 &&
                      std::abs(last.cx2 - last.x) < EPS2 &&
                      std::abs(last.cy2 - last.y) < EPS2 &&
                      last.type == BezierNode::Type::Smooth);
        if (!degen) break;
        // Check it's a lone M — no segment connects to it (prev node's cx2
        // is still its own anchor, meaning it didn't get a C drawn from it)
        const auto& prev = pd.nodes[pd.nodes.size()-2];
        bool prev_no_out = (std::abs(prev.cx2 - prev.x) < EPS2 &&
                            std::abs(prev.cy2 - prev.y) < EPS2);
        if (!prev_no_out) break; // prev has real out-handle — not a lone M
        pd.nodes.pop_back();
        break;
    }

    return pd;
}

// ── parse_path_d_bridge (s236 m1) ───────────────────────────────────────────
// Non-static wrapper exposing parse_path_d to curvz_utils. The pump pair
// `svg_d_to_path_data` / `path_data_to_svg_d` lives in curvz_utils so
// script-side callers (ObjectProxy::set_path_data) reach the same shape
// SvgParser / SvgWriter use, without duplicating the parser. Full lift
// of parse_path_d + arc_to_bezier into curvz_utils is deferred to a
// future milestone (paired with the writer-emitter sweep); m1 keeps the
// parser body here and adds this thin wrapper as the visible seam.
//
// Lives in Curvz:: (not curvz::utils::) — matches the existing public
// surface SvgParser exposes through other parser entry points.
PathData parse_path_d_bridge(const std::string& d) {
    return parse_path_d(d);
}

// Parse a full d string that may contain multiple subpaths (M...Z M...Z ...)
// and return one PathData per subpath. This avoids re-parsing each subpath
// in isolation (which would misinterpret relative 'm' commands as starting
// from (0,0) instead of the correct post-Z current point).
static std::vector<PathData> parse_path_d_multi(const std::string& d) {
    std::vector<PathData> result;
    if (d.empty()) return result;

    const char* p = d.c_str();
    double cx = 0, cy = 0;
    double mx = 0, my = 0;
    double last_cx1 = 0, last_cy1 = 0;
    char   cmd  = 0;
    bool   first = true;

    PathData current;

    auto finish_subpath = [&]() {
        // Strip trailing lone-M degenerate node
        while (current.nodes.size() >= 2) {
            const auto& last = current.nodes.back();
            constexpr double EPS2 = 1e-5;
            bool degen = (std::abs(last.cx1 - last.x) < EPS2 &&
                          std::abs(last.cy1 - last.y) < EPS2 &&
                          std::abs(last.cx2 - last.x) < EPS2 &&
                          std::abs(last.cy2 - last.y) < EPS2 &&
                          last.type == BezierNode::Type::Smooth);
            if (!degen) break;
            const auto& prev = current.nodes[current.nodes.size()-2];
            bool prev_no_out = (std::abs(prev.cx2 - prev.x) < EPS2 &&
                                std::abs(prev.cy2 - prev.y) < EPS2);
            if (!prev_no_out) break;
            current.nodes.pop_back();
            break;
        }
        if (!current.nodes.empty())
            result.push_back(std::move(current));
        current = PathData{};
    };

    while (*p) {
        while (*p && (std::isspace((unsigned char)*p) || *p == ',')) ++p;
        if (!*p) break;

        if (std::isalpha((unsigned char)*p)) {
            cmd = *p++;
        } else if (first) {
            if (!std::isdigit((unsigned char)*p) && *p != '-' && *p != '+' && *p != '.') {
                ++p; continue;
            }
            break;
        }

        switch (cmd) {
        case 'M': case 'm': {
            // New M after content starts a new subpath
            if (!first && !current.nodes.empty()) {
                finish_subpath();
            }
            double x = next_num(p), y = next_num(p);
            if (cmd == 'm' && !first) { x += cx; y += cy; }
            cx = mx = x; cy = my = y;
            last_cx1 = cx; last_cy1 = cy;
            BezierNode nd; nd.x=x;nd.y=y;nd.cx1=x;nd.cy1=y;nd.cx2=x;nd.cy2=y;
            nd.type = BezierNode::Type::Smooth;
            current.nodes.push_back(nd);
            first = false;
            cmd = (cmd == 'M') ? 'L' : 'l';
            break;
        }
        case 'C': case 'c': {
            double x1=next_num(p),y1=next_num(p),x2=next_num(p),y2=next_num(p),x=next_num(p),y=next_num(p);
            if (cmd=='c'){x1+=cx;y1+=cy;x2+=cx;y2+=cy;x+=cx;y+=cy;}
            if (!current.nodes.empty()){current.nodes.back().cx2=x1;current.nodes.back().cy2=y1;}
            BezierNode nd;nd.x=x;nd.y=y;nd.cx1=x2;nd.cy1=y2;nd.cx2=x;nd.cy2=y;
            nd.type=BezierNode::Type::Smooth;
            current.nodes.push_back(nd);
            last_cx1=x2;last_cy1=y2;cx=x;cy=y;
            break;
        }
        case 'S': case 's': {
            double x2=next_num(p),y2=next_num(p),x=next_num(p),y=next_num(p);
            if (cmd=='s'){x2+=cx;y2+=cy;x+=cx;y+=cy;}
            double ix=2*cx-last_cx1,iy=2*cy-last_cy1;
            if (!current.nodes.empty()){current.nodes.back().cx2=ix;current.nodes.back().cy2=iy;}
            BezierNode nd;nd.x=x;nd.y=y;nd.cx1=x2;nd.cy1=y2;nd.cx2=x;nd.cy2=y;
            nd.type=BezierNode::Type::Smooth;
            current.nodes.push_back(nd);
            last_cx1=x2;last_cy1=y2;cx=x;cy=y;
            break;
        }
        case 'L': case 'l': {
            double x=next_num(p),y=next_num(p);
            if (cmd=='l'){x+=cx;y+=cy;}
            if (!current.nodes.empty()){current.nodes.back().cx2=cx;current.nodes.back().cy2=cy;}
            BezierNode nd;nd.x=x;nd.y=y;nd.cx1=x;nd.cy1=y;nd.cx2=x;nd.cy2=y;
            nd.type=BezierNode::Type::Corner;
            current.nodes.push_back(nd);
            last_cx1=x;last_cy1=y;cx=x;cy=y;
            break;
        }
        case 'H': case 'h': {
            double x=next_num(p);
            if (cmd=='h')x+=cx;
            if (!current.nodes.empty()){current.nodes.back().cx2=cx;current.nodes.back().cy2=cy;}
            BezierNode nd;nd.x=x;nd.y=cy;nd.cx1=x;nd.cy1=cy;nd.cx2=x;nd.cy2=cy;
            nd.type=BezierNode::Type::Corner;
            current.nodes.push_back(nd);
            last_cx1=x;last_cy1=cy;cx=x;
            break;
        }
        case 'V': case 'v': {
            double y=next_num(p);
            if (cmd=='v')y+=cy;
            if (!current.nodes.empty()){current.nodes.back().cx2=cx;current.nodes.back().cy2=cy;}
            BezierNode nd;nd.x=cx;nd.y=y;nd.cx1=cx;nd.cy1=y;nd.cx2=cx;nd.cy2=y;
            nd.type=BezierNode::Type::Corner;
            current.nodes.push_back(nd);
            last_cx1=cx;last_cy1=y;cy=y;
            break;
        }
        case 'Q': case 'q': {
            double qx1=next_num(p),qy1=next_num(p),x=next_num(p),y=next_num(p);
            if (cmd=='q'){qx1+=cx;qy1+=cy;x+=cx;y+=cy;}
            double bcx1=cx+2.0/3.0*(qx1-cx),bcy1=cy+2.0/3.0*(qy1-cy);
            double bcx2=x +2.0/3.0*(qx1-x), bcy2=y +2.0/3.0*(qy1-y);
            if (!current.nodes.empty()){current.nodes.back().cx2=bcx1;current.nodes.back().cy2=bcy1;}
            BezierNode nd;nd.x=x;nd.y=y;nd.cx1=bcx2;nd.cy1=bcy2;nd.cx2=x;nd.cy2=y;
            nd.type=BezierNode::Type::Smooth;
            current.nodes.push_back(nd);
            last_cx1=qx1;last_cy1=qy1;cx=x;cy=y;
            break;
        }
        case 'T': case 't': {
            double x=next_num(p),y=next_num(p);
            if (cmd=='t'){x+=cx;y+=cy;}
            double qx1=2*cx-last_cx1,qy1=2*cy-last_cy1;
            double bcx1=cx+2.0/3.0*(qx1-cx),bcy1=cy+2.0/3.0*(qy1-cy);
            double bcx2=x +2.0/3.0*(qx1-x), bcy2=y +2.0/3.0*(qy1-y);
            if (!current.nodes.empty()){current.nodes.back().cx2=bcx1;current.nodes.back().cy2=bcy1;}
            BezierNode nd;nd.x=x;nd.y=y;nd.cx1=bcx2;nd.cy1=bcy2;nd.cx2=x;nd.cy2=y;
            nd.type=BezierNode::Type::Smooth;
            current.nodes.push_back(nd);
            last_cx1=qx1;last_cy1=qy1;cx=x;cy=y;
            break;
        }
        case 'Z': case 'z': {
            current.closed = true;
            // Remove coincident closing node
            if (current.nodes.size() >= 2) {
                const auto& fn = current.nodes.front();
                const auto& ln = current.nodes.back();
                constexpr double EPS = 1e-4;
                if (std::abs(ln.x-fn.x)<EPS && std::abs(ln.y-fn.y)<EPS) {
                    current.nodes.front().cx1 = ln.cx1;
                    current.nodes.front().cy1 = ln.cy1;
                    current.nodes.pop_back();
                }
            }
            finish_subpath();
            cx=mx; cy=my; last_cx1=cx; last_cy1=cy;
            cmd = 0;
            break;
        }
        case 'A': case 'a': {
            double arx=next_num(p), ary=next_num(p), xrot=next_num(p);
            int laf=next_flag(p), sf=next_flag(p);
            double ex=next_num(p), ey=next_num(p);
            if (cmd=='a'){ex+=cx;ey+=cy;}
            arc_to_bezier(current.nodes, cx, cy, arx, ary, xrot, laf, sf, ex, ey);
            last_cx1=cx;last_cy1=cy;
            break;
        }
        default: ++p; break;
        }
    }

    // Flush any open (unclosed) final subpath
    if (!current.nodes.empty())
        finish_subpath();

    return result;
}

// Split a compound d string into individual subpath d strings.
// Each subpath starts with M/m and ends at the next M/m (after Z) or end.
static std::vector<std::string> split_subpath_strings(const std::string& d) {
    std::vector<std::string> result;
    if (d.empty()) return result;

    std::vector<size_t> starts;
    bool after_z = true;
    for (size_t i = 0; i < d.size(); ++i) {
        char c = d[i];
        if (c == 'Z' || c == 'z') {
            after_z = true;
        } else if ((c == 'M' || c == 'm') && after_z) {
            starts.push_back(i);
            after_z = false;
        } else if (!std::isspace((unsigned char)c) && c != ',') {
            after_z = false;
        }
    }

    if (starts.size() <= 1) {
        result.push_back(d);
        return result;
    }

    for (size_t i = 0; i < starts.size(); ++i) {
        size_t begin = starts[i];
        size_t end   = (i + 1 < starts.size()) ? starts[i + 1] : d.size();
        result.push_back(d.substr(begin, end - begin));
    }
    return result;
}


// ── parse_svg ─────────────────────────────────────────────────────────────────

std::unique_ptr<CurvzDocument> parse_svg(const std::string& svg) {
    auto doc = std::make_unique<CurvzDocument>();
    doc->layers.clear(); // remove default layer; we'll rebuild from file

    // Stack of current containers — layers and groups.
    // Each entry carries the node pointer and that node's local transform
    // so we can apply it to children when the group closes.
    // is_curvz: true if this <g> was emitted by Curvz (has data-curvz-*
    // markers). Foreign <g> children are in SVG document order (first=bottom);
    // Curvz <g> children are already in Curvz internal order ([0]=top).
    // We reverse children on close only for foreign.
    struct StackEntry { SceneNode* node; AffineMatrix xfm; bool is_curvz = false; };
    std::vector<StackEntry> stack;
    SceneNode* current_layer = nullptr;  // kept for logging/compat
    int    obj_counter   = 1;

    // ── Clip-path defs state ──────────────────────────────────────────────
    // While inside <clipPath id="X">…</clipPath>, `in_clip_def_id` holds
    // "X" and the tag handlers for <path>/<g data-curvz-compound> etc.
    // push their built SceneNode into `clip_defs[X]` instead of the
    // normal tree.
    //
    // When a <g clip-path="url(#X)" …> closes, we look up X in clip_defs
    // and move the resolved SceneNode into the ClipGroup's clip_shape
    // slot. ClipGroups whose referenced id didn't appear in defs (e.g.
    // malformed file) survive with clip_shape=null — they render
    // un-clipped, matching the Stage 3 fallback.
    std::string in_clip_def_id;
    std::map<std::string, std::unique_ptr<SceneNode>> clip_defs;
    // While inside a ClipGroup's <g>, pending_clip_id[stack_depth] holds
    // the url-fragment id so the close handler can attach the shape.
    // We stash on the StackEntry instead — extending the struct by one
    // optional field keeps the look-up O(1). But to avoid touching every
    // push site, we use a parallel map keyed by the ClipGroup pointer.
    std::map<SceneNode*, std::string> clipgroup_pending;
    // While inside a Blend's <g>, each child <path> may carry a
    // data-curvz-blend-role="a"/"b" attribute. We stash the raw pointer
    // of the just-built child against its role so the close-tag handler
    // for the Blend can pluck them out of children[] and route them into
    // blend_source_a / blend_source_b slots.
    //
    // Entries survive only long enough for the Blend's close handler to
    // consume them; any leftovers after close (e.g. the file has dangling
    // role tags on paths whose Blend parent malformed) are harmless.
    std::map<SceneNode*, char> blend_role_pending;

    // Parallel mechanism for Warp: the single child of a Warp wrapper
    // carries data-curvz-warp-role="source". We stash the pointer → 's'
    // at emit-time (both Path-child and Group/Compound-child sites) and
    // the Warp's close-tag handler plucks it into warp_source.
    std::map<SceneNode*, char> warp_role_pending;

    // While parsing inside a TextBoxMgr's <g>, the group's
    // data-curvz-content attribute (and caret-byte attribute) are
    // hoisted onto the Mgr SceneNode itself at open-time — the Mgr
    // owns those fields directly, no transient stash needed. What
    // does need transient storage is the font attrs harvested from
    // the first non-baseline <text> child encountered: those <text>
    // elements get discarded after the close-handler runs, but their
    // font_family / font_size / bold / italic / fill attributes
    // become the Mgr's text defaults. The writer emits the same
    // attrs on every per-baseline <text>, but one suffices, so the
    // font_seen guard locks the harvest after the first hit.
    struct TextBoxPending {
        std::string font_family = "Sans";
        double      font_size   = 24.0;
        bool        bold        = false;
        bool        italic      = false;
        FillStyle   fill;               // from a per-baseline <text>'s fill attr
        bool        fill_set    = false;
        bool        font_seen   = false; // set on first harvest so subsequent
                                         // baselines don't overwrite (they're
                                         // identical, but the guard makes
                                         // intent explicit)
    };
    std::map<SceneNode*, TextBoxPending> textbox_pending;
    // Parse-time marker set: pointers to <path> nodes that were tagged
    // data-curvz-baseline="1" on disk. The TextBox close-handler walks
    // its children, removes any child whose pointer is in this set,
    // then erases the set entry. Baseline-markers exist only between
    // their <path> tag and the </g> close of their parent TextBox.
    std::unordered_set<SceneNode*> baseline_markers;

    // ── S90 Stage 2 — gradient defs state machine ─────────────────────
    // While inside <linearGradient id="X"> or <radialGradient id="X">,
    // `in_gradient_def_id` holds X and `pending_gradient` accumulates
    // the FillStyle being built. <stop offset=".." stop-color=".." .../>
    // self-closing tags append to pending_gradient.stops while in def
    // mode. The </linearGradient> / </radialGradient> handler flushes
    // pending_gradient into gradient_defs[X] and clears the state.
    //
    // Activated as g_gradient_defs at parse_svg entry so parse_fill
    // can resolve url(#X) references from any subsequent shape element.
    // SVG-spec writers (including SvgWriter) emit defs before referrers,
    // so the lookup will succeed in normal cases.
    std::string in_gradient_def_id;
    FillStyle pending_gradient;
    std::map<std::string, FillStyle> gradient_defs;
    g_gradient_defs = &gradient_defs;

    auto current_parent = [&]() -> SceneNode* {
        return stack.empty() ? nullptr : stack.back().node;
    };

    // S97 m5 — track <defs> nesting depth. While in_defs > 0, the parser
    // is inside an SVG <defs> block (gradients, clipPaths, filters, etc.).
    // Most defs content has a dedicated state machine (gradient_defs,
    // clip_defs); the only case currently NOT modelled is <filter>, added
    // by drop-shadow in S97 m1. The writer emits <filter> with no Curvz
    // markers — the parser's "bare objects fallback" then incorrectly
    // fires for <filter>'s self-closing children (<feOffset>, <feFlood>,
    // ...) and for <defs> itself, fabricating a spurious "Layer 1" Layer
    // before the real layer arrives. The real <g data-curvz-layer="1">
    // then lands inside the spurious Layer (stack non-empty) and is
    // misclassified as a Group via the is_group=(... || !stack.empty())
    // clause at the <g> dispatch site. dedup_names later renames the
    // inner Group to "Layer 1 (2)" because of the name collision.
    //
    // Fix: gate the bare-object fallback so it does not fire while
    // in_defs > 0. Defs content shouldn't land in any layer — it's
    // referenced by id from elsewhere, not part of the visible tree.
    int in_defs = 0;

    // ── s311 m1c-redux — dual-block textbox state ─────────────────────────
    // Mgrs defined in <defs> are hydrated into pending_mgrs (owned) and
    // mgr_def_by_iid (back-pointer index). When a TextBoxView in the
    // visible tree references a Mgr by data-curvz-mgr-iid, the parser
    // splices the Mgr into the view's current layer (the first view's
    // layer wins) and attaches the view as a child of the Mgr. Mgrs that
    // no view ever references get placed in the first regular layer at
    // end-of-parse as a fallback, so a Mgr with no canvas views round-
    // trips as a Mgr-with-only-popover (matching legacy save shapes for
    // freshly-constructed-but-never-bounded textboxes).
    //
    // in_textbox_mgr_def points at the Mgr currently being hydrated from
    // a <g data-curvz-textbox-mgr> open inside <defs>. CDATA tokens flow
    // into its text_content while non-null; the </g> close clears it.
    std::vector<std::unique_ptr<SceneNode>> pending_mgrs;
    std::map<std::string, SceneNode*> mgr_def_by_iid;
    std::set<std::string> placed_mgrs;
    SceneNode* in_textbox_mgr_def = nullptr;
    bool mgr_def_is_markup = false;  // s325 — current Mgr def's CDATA is Pango markup

    // Push a freshly-built SceneNode into the correct destination:
    //   - When in_clip_def_id is active we're inside <clipPath id="X">;
    //     the node is stashed in clip_defs[X] and will later be attached
    //     as the clip_shape of a ClipGroup referencing X.
    //   - Otherwise it goes into the current parent's children.
    // Only Path and Compound are sensible clip-shape types. If something
    // else slips in (shouldn't, SVG-wise), we still stash it — better
    // than silently dropping, and the ClipGroup consumer tolerates any
    // SceneNode type at render (draw_object checks for Path/Compound).
    auto push_into_parent = [&](std::unique_ptr<SceneNode> node) {
        if (!in_clip_def_id.empty()) {
            // If there are already multiple defs entries for this id
            // (malformed — clipPath with multiple top-level shapes),
            // keep the first and drop the rest. Standard SVG allows
            // multiple children inside <clipPath> but Curvz's model
            // stores a single clip_shape (Path or Compound). A multi-
            // shape clipPath should have been emitted as a Compound.
            if (clip_defs.find(in_clip_def_id) == clip_defs.end())
                clip_defs[in_clip_def_id] = std::move(node);
            else
                LOG_INFO("SvgParser: extra shape in <clipPath id='{}'> — "
                         "ignored (only the first is kept)",
                         in_clip_def_id);
            return;
        }
        if (auto *p = current_parent())
            p->children.push_back(std::move(node));
    };

    auto tags = split_tags(svg);
    for (const auto& tag : tags) {
        // s203 m4 — Per-element exception guard. Wraps a single tag's
        // handler dispatch. If a malformed element causes an std-exception
        // (numeric parse, allocation failure, out-of-range etc.), log a
        // WARN with the tag prefix and move on rather than aborting the
        // whole parse. This does NOT protect against null-deref segfaults
        // or other signals — those are not catchable by try/catch in C++.
        // For those, the only defense is upstream null-checks (m3 did the
        // first sweep; the structural pump push_into_parent is the right
        // long-term answer).
        try {
        // s311 m1c-redux — CDATA harvest. When we're inside a
        // <g data-curvz-textbox-mgr> open in <defs>, the next CDATA
        // section is the Mgr's buffer. Token form is `![CDATA[...]]`
        // (split_tags strips the outer < and trailing > of ]]>).
        // The payload starts at offset 8 ("![CDATA[".size()) and ends
        // at the last `]]`; we strip that off too. Outside a Mgr def
        // we drop CDATA silently — Curvz files don't otherwise carry
        // CDATA today, and dropping is safe (no semantic loss for
        // tools that emit incidental CDATA in unrelated places).
        if (tag.size() >= 8 && tag.compare(0, 8, "![CDATA[") == 0) {
            if (in_textbox_mgr_def) {
                // Payload is the substring between ![CDATA[ and ]]:
                //   tag = "![CDATA[<payload>]]"
                size_t pl_start = 8;
                size_t pl_end   = tag.size();
                if (pl_end >= 2 && tag.compare(pl_end - 2, 2, "]]") == 0)
                    pl_end -= 2;
                if (pl_end < pl_start) pl_end = pl_start;
                if (mgr_def_is_markup) {
                    // s325 — CDATA is Pango markup: decode to clean text +
                    // per-run spans. text_content ends up clean, so all
                    // downstream readers are unaffected.
                    decode_markup_into(in_textbox_mgr_def,
                                       std::string(tag, pl_start,
                                                   pl_end - pl_start));
                } else {
                    in_textbox_mgr_def->text_content.assign(
                        tag, pl_start, pl_end - pl_start);
                    LOG_INFO("SvgParser: TextBoxMgr def '{}' buffer "
                             "loaded — {} bytes",
                             in_textbox_mgr_def->name,
                             in_textbox_mgr_def->text_content.size());
                }
            }
            continue;
        }
        if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;

        // Closing tags — pop the stack
        if (tag[0] == '/') {
            // Close of <clipPath>: leave the "currently defining" state.
            // The definition tag inside it (a <path> or <g data-curvz-compound>)
            // has already been stashed into clip_defs by the normal element
            // handlers, which check in_clip_def_id before emitting into
            // the tree.
            if (tag == "/clipPath") {
                LOG_INFO("SvgParser: </clipPath> id='{}' has_shape={}",
                         in_clip_def_id,
                         clip_defs.count(in_clip_def_id) ? 1 : 0);
                in_clip_def_id.clear();
                continue;
            }
            // S90 Stage 2 — close <linearGradient> / <radialGradient>.
            // Flush pending_gradient into gradient_defs[id] and reset.
            if (tag == "/linearGradient" || tag == "/radialGradient") {
                if (!in_gradient_def_id.empty()) {
                    LOG_INFO("SvgParser: </{}> id='{}' stops={}",
                             (tag == "/linearGradient") ? "linearGradient"
                                                        : "radialGradient",
                             in_gradient_def_id,
                             pending_gradient.stops.size());
                    gradient_defs[in_gradient_def_id] = pending_gradient;
                    in_gradient_def_id.clear();
                    pending_gradient = FillStyle{};
                }
                continue;
            }
            // S97 m5 — close </defs>. Decrement nesting depth; gate
            // for the "bare objects" fallback re-opens once we leave
            // the defs subtree. See in_defs declaration for full notes.
            if (tag == "/defs") {
                if (in_defs > 0) --in_defs;
                continue;
            }
            // s311 m1c-redux — close the in-defs TextBoxMgr blob. Fires
            // for the `</g>` that pairs with a <g data-curvz-textbox-mgr>
            // opened in <defs>. The Mgr is already in pending_mgrs and
            // mgr_def_by_iid; this handler just clamps the caret against
            // the now-known buffer length and clears the in-defs pointer.
            // Placement into a layer happens later (when a view
            // references the Mgr, or at end-of-parse as fallback).
            if (tag == "/g" && in_textbox_mgr_def) {
                SceneNode* mgr = in_textbox_mgr_def;
                if (mgr->text_caret_byte > (int32_t)mgr->text_content.size())
                    mgr->text_caret_byte = (int32_t)mgr->text_content.size();
                LOG_INFO("SvgParser: <defs> textbox-mgr '{}' closed — "
                         "content_len={} caret={}",
                         mgr->name, mgr->text_content.size(),
                         mgr->text_caret_byte);
                in_textbox_mgr_def = nullptr;
                mgr_def_is_markup = false;  // s325
                continue;
            }
            if ((tag == "/g" || tag.rfind("/g", 0) == 0) && !stack.empty()) {
                // Apply this group's transform to all its direct children
                auto& entry = stack.back();
                if (!entry.xfm.is_identity()) {
                    for (auto& child : entry.node->children)
                        apply_transform_to_node(*child, entry.xfm);
                }
                // Reverse children for every Layer and Group container.
                // SVG paint order: first child = bottom. Curvz internal:
                // [0] = top. SvgWriter always emits in reverse to produce
                // spec-correct SVG; SvgParser must always reverse on read
                // to restore internal convention. The round-trip is then
                // a net-identity.
                // Compound children control evenodd topology, not z-order —
                // do NOT reverse. Ref/Measure/Guide layers have no z-order
                // among their children.
                // ClipGroup follows the same [0]=top convention as Group
                // (write_object reverses on emit; we reverse on read).
                if (entry.node->type == SceneNode::Type::Layer ||
                    entry.node->type == SceneNode::Type::Group ||
                    entry.node->type == SceneNode::Type::ClipGroup ||
                    entry.node->type == SceneNode::Type::Blend ||
                    entry.node->type == SceneNode::Type::Warp) {
                    std::reverse(entry.node->children.begin(),
                                 entry.node->children.end());
                }
                // ClipGroup: attach the previously-parsed shape from defs.
                // If the id wasn't defined (malformed SVG), leave clip_shape
                // null — ClipGroup draws un-clipped as a degenerate fallback
                // per draw_object.
                if (entry.node->type == SceneNode::Type::ClipGroup) {
                    auto pit = clipgroup_pending.find(entry.node);
                    if (pit != clipgroup_pending.end()) {
                        const std::string& want_id = pit->second;
                        entry.node->clip_id = want_id;
                        auto dit = clip_defs.find(want_id);
                        if (dit != clip_defs.end()) {
                            entry.node->clip_shape = std::move(dit->second);
                            clip_defs.erase(dit);
                        } else {
                            LOG_INFO("SvgParser: ClipGroup '{}' references "
                                     "undefined clip id '{}' — leaving "
                                     "clip_shape null",
                                     entry.node->name, want_id);
                        }
                        clipgroup_pending.erase(pit);
                    }
                }
                // Blend: walk children, pull out the one tagged role="a" and
                // the one tagged role="b" from the pending map, move them
                // into dedicated slots. Discard the rest — baked cache steps
                // emitted by the writer for foreign-tool display are derived
                // and regenerated from A/B on first draw.
                //
                // If A or B is missing (malformed file, old M1 stub marker,
                // or writer bug) the slot stays null. rebuild_blend_cache
                // handles that gracefully by logging and clearing the cache;
                // the user sees an empty Blend in the layers panel and can
                // delete it.
                if (entry.node->type == SceneNode::Type::Blend) {
                    std::vector<std::unique_ptr<SceneNode>> kept;
                    for (auto &child : entry.node->children) {
                        auto it = blend_role_pending.find(child.get());
                        if (it == blend_role_pending.end()) {
                            // Untagged — a baked cache step; discard.
                            continue;
                        }
                        char role = it->second;
                        blend_role_pending.erase(it);
                        if (role == 'a') {
                            entry.node->blend_source_a = std::move(child);
                        } else if (role == 'b') {
                            entry.node->blend_source_b = std::move(child);
                        }
                    }
                    // All children processed — wipe the children vector
                    // (unique_ptrs either moved to slots or to be destroyed).
                    entry.node->children.clear();
                    entry.node->blend_cache_dirty = true;
                    LOG_INFO("SvgParser: Blend '{}' restored — A={} B={} steps={}",
                             entry.node->name,
                             (void*)entry.node->blend_source_a.get(),
                             (void*)entry.node->blend_source_b.get(),
                             entry.node->blend_steps);
                    if (!entry.node->blend_source_a || !entry.node->blend_source_b) {
                        LOG_WARN("SvgParser: Blend '{}' missing A or B slot — "
                                 "malformed or legacy M1-stub file",
                                 entry.node->name);
                    }
                }
                // Warp: walk children, pull the one tagged role="source"
                // into warp_source. Discard the rest (shouldn't exist —
                // M1 writer emits exactly one tagged child, caches are
                // derived-not-authoritative). If the role-tagged child
                // is missing, leave warp_source null; the user sees an
                // empty Warp in layers panel and can delete it.
                if (entry.node->type == SceneNode::Type::Warp) {
                    for (auto &child : entry.node->children) {
                        auto it = warp_role_pending.find(child.get());
                        if (it == warp_role_pending.end())
                            continue; // untagged — discard
                        char role = it->second;
                        warp_role_pending.erase(it);
                        if (role == 's') {
                            entry.node->warp_source = std::move(child);
                        }
                    }
                    entry.node->children.clear();
                    entry.node->warp_glyph_cache_dirty = true;
                    entry.node->warp_cache_dirty       = true;
                    LOG_INFO("SvgParser: Warp '{}' restored — source={}",
                             entry.node->name,
                             (void*)entry.node->warp_source.get());
                    if (!entry.node->warp_source) {
                        LOG_WARN("SvgParser: Warp '{}' missing source slot — "
                                 "malformed file",
                                 entry.node->name);
                    }
                }
                // TextBoxView: rebuild the view's in-memory shape from
                // the on-disk shape. s310 m1c — the view's <g> on disk
                // holds:
                //   children = [boundary-path,
                //               baseline-marker-path, <text>, repeat...]
                //
                // The in-memory shape:
                //   view (this node; Type::TextBoxView)
                //   └── children[0] = boundary Path
                //
                // Steps:
                //   1. Find the boundary (the one Path child whose
                //      pointer is NOT in baseline_markers). Other Paths
                //      are baseline markers → discarded.
                //   2. Find the first Text child and harvest font attrs
                //      into the ancestor Mgr's pending entry. All
                //      <text> elements are discarded.
                //   3. Replace view->children with [boundary].
                //
                // The ancestor Mgr is at stack[size-2] when this fires
                // (the view's </g> closes before the Mgr's </g>). We
                // can hoist font attrs straight into the Mgr's pending
                // entry; the Mgr's close-handler will apply them.
                if (entry.node->type == SceneNode::Type::TextBoxView) {
                    SceneNode* view = entry.node;
                    std::unique_ptr<SceneNode> boundary;
                    // Find the Mgr ancestor for font-attr hoisting.
                    SceneNode* mgr_ancestor = nullptr;
                    if (stack.size() >= 2) {
                        SceneNode* parent = stack[stack.size()-2].node;
                        if (parent && parent->type ==
                                SceneNode::Type::TextBoxMgr) {
                            mgr_ancestor = parent;
                        }
                    }
                    for (auto &child : view->children) {
                        if (!child) continue;
                        if (child->type == SceneNode::Type::Path) {
                            auto bm = baseline_markers.find(child.get());
                            if (bm != baseline_markers.end()) {
                                baseline_markers.erase(bm);
                                // child goes out of scope at clear() below.
                                continue;
                            }
                            // Real boundary path. First one wins if a
                            // malformed file has multiples.
                            if (!boundary) {
                                boundary = std::move(child);
                            } else {
                                LOG_WARN("SvgParser: TextBoxView '{}' "
                                         "has multiple boundary paths — "
                                         "keeping the first",
                                         view->internal_id);
                            }
                        } else if (child->type == SceneNode::Type::Text) {
                            // Harvest font attrs into the Mgr ancestor's
                            // pending entry (first hit wins). Only
                            // matters for canvas views — popover views
                            // shouldn't carry <text> children on disk.
                            if (mgr_ancestor) {
                                auto pit = textbox_pending.find(mgr_ancestor);
                                if (pit != textbox_pending.end() &&
                                        !pit->second.font_seen) {
                                    pit->second.font_family =
                                        child->text_font_family;
                                    pit->second.font_size =
                                        child->text_font_size;
                                    pit->second.bold = child->text_bold;
                                    pit->second.italic = child->text_italic;
                                    pit->second.fill = child->fill;
                                    pit->second.fill_set = true;
                                    pit->second.font_seen = true;
                                }
                            }
                            // Discard the <text> wrapper either way.
                        }
                        // Any other child type is ignored.
                    }
                    view->children.clear();
                    if (boundary) {
                        view->children.push_back(std::move(boundary));
                    } else if (view->view_kind ==
                               SceneNode::ViewKind::Canvas) {
                        LOG_WARN("SvgParser: TextBoxView '{}' (canvas) "
                                 "missing boundary child — emitting "
                                 "empty view",
                                 view->internal_id);
                        // Popover views legitimately have no boundary;
                        // canvas views without one are degenerate but
                        // we keep the empty view so the Mgr's view
                        // count stays consistent with what was on disk.
                    }
                    LOG_INFO("SvgParser: TextBoxView '{}' (kind={}) "
                             "restored — children={}",
                             view->internal_id,
                             view->view_kind == SceneNode::ViewKind::Canvas
                                 ? "canvas" : "popover",
                             view->children.size());
                }
                // TextBoxMgr: by the time we get here, each view child
                // has been processed by its own TextBoxView close arm
                // above, and font attrs have been hoisted into pending.
                // s310 m1c — the close arm now applies pending font
                // attrs to the Mgr and appends a synthesized Popover
                // view if none was present on disk (the m1bc writer
                // intentionally skips popover views; they're
                // structural fixtures regenerated on load).
                //
                // The in-memory shape (s310 m1bc):
                //   mgr (the entry node itself; carries the buffer +
                //        font defaults + caret + fill/stroke)
                //   ├── children[0..N-1] = Canvas TextBoxView(s)
                //   │       └── children[0] = boundary Path
                //   └── children[N]   = Popover TextBoxView (synthesized
                //                       here if not on disk; no Path
                //                       child; boundary geometry comes
                //                       from the widget allocation at
                //                       runtime in m2)
                //
                // Round-trip-safe even when textbox_pending has no
                // entry (foreign tool wrote the marker with no <text>
                // children): the Mgr's font defaults stay at the
                // SceneNode defaults set in the open-handler.
                if (entry.node->type == SceneNode::Type::TextBoxMgr) {
                    SceneNode* mgr = entry.node;
                    auto pit = textbox_pending.find(mgr);
                    TextBoxPending pending = (pit != textbox_pending.end())
                                             ? pit->second
                                             : TextBoxPending{};
                    if (pit != textbox_pending.end()) {
                        textbox_pending.erase(pit);
                    }
                    // s310 m1c — Two on-disk shapes to handle:
                    //
                    //   (a) Legacy (pre-s310 or m1a-only files):
                    //       data-curvz-textbox="1" with direct
                    //       children [boundary-path,
                    //       baseline-marker-path, <text>, ...]. No
                    //       TextBoxView <g> wrappers. We walk the
                    //       direct children, sweep baselines, harvest
                    //       font attrs, route boundary into a
                    //       synthesized CanvasView.
                    //
                    //   (b) New (m1bc emit): TextBoxView children are
                    //       already correctly populated by the view's
                    //       own close-arm above; this Mgr close-arm
                    //       just appends a Popover view at the end.
                    //
                    // Distinguish by inspecting the Mgr's children: if
                    // any are TextBoxView, shape (b); otherwise (a).
                    bool has_views = false;
                    for (auto& c : mgr->children) {
                        if (c && c->is_text_box_view()) {
                            has_views = true;
                            break;
                        }
                    }
                    if (!has_views) {
                        // Shape (a) — legacy direct-children walk.
                        std::unique_ptr<SceneNode> boundary;
                        for (auto &child : mgr->children) {
                            if (!child) continue;
                            if (child->type == SceneNode::Type::Path) {
                                auto bm = baseline_markers.find(child.get());
                                if (bm != baseline_markers.end()) {
                                    baseline_markers.erase(bm);
                                    continue;
                                }
                                if (!boundary) {
                                    boundary = std::move(child);
                                } else {
                                    LOG_WARN("SvgParser: TextBoxMgr '{}' "
                                             "(legacy shape) has "
                                             "multiple boundary paths "
                                             "— keeping the first",
                                             mgr->name);
                                }
                            } else if (child->type ==
                                       SceneNode::Type::Text) {
                                if (!pending.font_seen) {
                                    pending.font_family =
                                        child->text_font_family;
                                    pending.font_size =
                                        child->text_font_size;
                                    pending.bold = child->text_bold;
                                    pending.italic = child->text_italic;
                                    pending.fill = child->fill;
                                    pending.fill_set = true;
                                    pending.font_seen = true;
                                }
                            }
                        }
                        mgr->children.clear();
                        if (boundary) {
                            // Synthesize the CanvasView wrapping the
                            // boundary.
                            auto canvas_view = std::make_unique<SceneNode>();
                            canvas_view->type = SceneNode::Type::TextBoxView;
                            canvas_view->internal_id = generate_internal_id();
                            canvas_view->view_kind = SceneNode::ViewKind::Canvas;
                            canvas_view->view_byte_start = 0;
                            canvas_view->view_bytes_consumed = 0;
                            canvas_view->children.push_back(std::move(boundary));
                            mgr->children.push_back(std::move(canvas_view));
                        } else {
                            LOG_WARN("SvgParser: TextBoxMgr '{}' "
                                     "(legacy shape) missing boundary "
                                     "child — emitting empty container",
                                     mgr->name);
                        }
                    }
                    // Hoist the harvested font attrs onto the Mgr. For
                    // shape (a) this happens here; for shape (b) the
                    // view close-arm already pushed attrs into pending.
                    // pending.font_seen=false means no <text> child was
                    // present (foreign-tool or freshly-empty case);
                    // leave the Mgr's defaults untouched in that case.
                    if (pending.font_seen) {
                        mgr->text_font_family = pending.font_family;
                        mgr->text_font_size   = pending.font_size;
                        mgr->text_bold        = pending.bold;
                        mgr->text_italic      = pending.italic;
                        if (pending.fill_set) {
                            mgr->fill = pending.fill;
                        }
                    }
                    // s310 m1bc — Anchor/align are not user-tunable for
                    // Mgr-owned text (the boundary determines layout);
                    // set the canonical "start"/"left" values explicitly
                    // so any downstream consumer sees consistent state.
                    mgr->text_anchor = "start";
                    mgr->text_align  = "left";
                    // Count canvas views and check whether a popover
                    // view is already present.
                    int canvas_count = 0;
                    bool has_popover = false;
                    for (auto& c : mgr->children) {
                        if (!c || !c->is_text_box_view()) continue;
                        if (c->view_kind == SceneNode::ViewKind::Popover) {
                            has_popover = true;
                        } else {
                            ++canvas_count;
                        }
                    }
                    if (canvas_count == 0) {
                        LOG_WARN("SvgParser: TextBoxMgr '{}' has no "
                                 "canvas views — emitting Mgr with "
                                 "popover only", mgr->name);
                    }
                    // Synthesize the Popover view if not on disk. It's
                    // always the last child of the Mgr; m2 wires the
                    // widget that brings it to life.
                    if (!has_popover) {
                        auto popover_view = std::make_unique<SceneNode>();
                        popover_view->type = SceneNode::Type::TextBoxView;
                        popover_view->internal_id = generate_internal_id();
                        popover_view->view_kind = SceneNode::ViewKind::Popover;
                        popover_view->view_byte_start = 0;
                        popover_view->view_bytes_consumed = 0;
                        mgr->children.push_back(std::move(popover_view));
                    }
                    LOG_INFO("SvgParser: TextBoxMgr '{}' restored — "
                             "content_len={} font='{}' size={:.1f} "
                             "canvas_views={} shape={}",
                             mgr->name, mgr->text_content.size(),
                             mgr->text_font_family,
                             mgr->text_font_size,
                             canvas_count,
                             has_views ? "new" : "legacy");
                }
                stack.pop_back();
                current_layer = nullptr;
                for (auto& e : stack)
                    if (e.node->type == SceneNode::Type::Layer) current_layer = e.node;
            }
            continue;
        }

        // ── <defs> ────────────────────────────────────────────────────────
        // S97 m5 — eat the <defs> opening tag and bump nesting depth so
        // the bare-object fallback below knows to skip the rest of this
        // subtree. SVG defs subtree is paint-server / filter / clipPath
        // material referenced by id, not part of the visible tree —
        // anything inside it must NOT land in a Layer.
        //
        // The matching </defs> handler lives in the closing-tag block
        // above. Most defs content (gradients, clipPaths) already has
        // its own state machine; <filter> (added by S97 drop-shadow) has
        // none — but we don't need to model its internals, only stop
        // them landing in a layer.
        if (tag == "defs" || tag.rfind("defs ", 0) == 0 ||
            tag == "defs/" || tag.rfind("defs/", 0) == 0) {
            // Self-closing <defs/> (rare but legal) — open and immediately
            // close. Match either via trailing '/' on the tag string.
            const bool self_close = !tag.empty() && tag.back() == '/';
            ++in_defs;
            if (self_close && in_defs > 0) --in_defs;
            continue;
        }

        // ── svg root ──────────────────────────────────────────────────────
        if (tag.rfind("svg ", 0) == 0 || tag == "svg") {
            auto vb = attr(tag, "viewBox");
            auto w_attr = attr(tag, "width");
            auto h_attr = attr(tag, "height");

            // s150 fix1: measure behaviour prefs read from root <svg>
            // attributes. Legacy files (pre-s150) carry these on the
            // measure-layer wrapper instead; the layer-side read below
            // is preserved as a fallback for that case. New writes
            // always go to the root, so over time legacy files re-saved
            // by a post-s150 binary migrate naturally.
            if (attr(tag, "data-curvz-measure-save-to-layer") == "1")
                doc->measure_save_to_layer = true;
            if (attr(tag, "data-curvz-measure-destruct-after-copy") == "1")
                doc->measure_destruct_after_copy = true;

            // s264 m1 / s265 m2: render intent from data-curvz-intended-*
            // attrs. Read AFTER the doc->canvas = from_legacy/from_physical
            // assignments below, because s265 m2 moved intended_* into
            // CanvasModel — a `doc->canvas = ...` reassignment would wipe
            // any intent set first. Block is deferred to lines below the
            // canvas-build code; the read function is captured here as a
            // lambda so the original logical position is preserved in the
            // file's flow.
            auto read_explicit_intent = [&]() {
                auto iw_attr = attr(tag, "data-curvz-intended-w");
                auto ih_attr = attr(tag, "data-curvz-intended-h");
                auto iu_attr = attr(tag, "data-curvz-intended-unit");
                if (!iw_attr.empty() && !ih_attr.empty()) {
                    try {
                        doc->canvas.intended_w = std::stod(iw_attr);
                        doc->canvas.intended_h = std::stod(ih_attr);
                        doc->canvas.intended_unit = iu_attr;   // "" treated as "px"
                    } catch (...) {
                        doc->canvas.intended_w = 0.0;
                        doc->canvas.intended_h = 0.0;
                        doc->canvas.intended_unit.clear();
                    }
                }
            };

            // Detect a physical-unit suffix on width/height ("1in", "25.4mm",
            // "2.54cm").  If present alongside a viewBox, the file was
            // exported from a Physical-mode canvas — reconstruct phys_width,
            // phys_height, phys_unit, and dpi from (viewBox_w, phys_w).
            auto phys_suffix = [](const std::string& s, double& val_out,
                                  std::string& unit_out) -> bool {
                // Trim
                std::string v = s;
                while (!v.empty() && v.back() == ' ') v.pop_back();
                static const char* suffixes[] = {"in", "mm", "cm"};
                for (const char* suf : suffixes) {
                    size_t n = std::strlen(suf);
                    if (v.size() > n &&
                        v.compare(v.size() - n, n, suf) == 0) {
                        try {
                            val_out  = std::stod(v.substr(0, v.size() - n));
                            unit_out = suf;
                            return true;
                        } catch (...) { return false; }
                    }
                }
                return false;
            };

            double phys_w = 0.0, phys_h = 0.0;
            std::string phys_u_w, phys_u_h;
            bool is_phys = phys_suffix(w_attr, phys_w, phys_u_w)
                        && phys_suffix(h_attr, phys_h, phys_u_h)
                        && phys_u_w == phys_u_h;

            if (!vb.empty()) {
                // "minx miny W H"
                std::istringstream ss(vb);
                double minx, miny, w, h;
                if (ss >> minx >> miny >> w >> h) {
                    if (is_phys && phys_w > 0.0 && phys_h > 0.0) {
                        // Reconstruct Physical-mode canvas.  dpi derived from
                        // viewBox_w (doc-px) ÷ phys_w (in phys_u):
                        //   in:  dpi = W / phys_w
                        //   mm:  dpi = W / (phys_w / 25.4)
                        //   cm:  dpi = W / (phys_w / 2.54)
                        double phys_w_inches = phys_w;
                        if (phys_u_w == "mm") phys_w_inches = phys_w / 25.4;
                        else if (phys_u_w == "cm") phys_w_inches = phys_w / 2.54;
                        int dpi = (phys_w_inches > 0.0)
                                  ? (int)std::round(w / phys_w_inches)
                                  : 96;
                        doc->canvas = CanvasModel::from_physical(
                            phys_w, phys_h, phys_u_w, dpi);
                        // s265 m2: read explicit intent AFTER the reassign.
                        read_explicit_intent();
                    } else {
                        doc->canvas = CanvasModel::from_legacy(
                            (int)std::round(w), (int)std::round(h));
                        // s265 m2: read explicit intent AFTER the reassign.
                        read_explicit_intent();

                        // s264 m1: implicit render-intent inference. If the
                        // SVG declares a viewBox AND a bare-number
                        // width/height that DISAGREES with the viewBox, the
                        // file's author intended "design at viewBox scale,
                        // deliver at width/height" — the SVG-native idiom
                        // every other tool emits (Inkscape, Illustrator,
                        // hand-written). Curvz reads this as render intent
                        // so the file round-trips with its semantics
                        // preserved.
                        //
                        // Guard 1: explicit data-curvz-intended-* attrs win
                        //   (already read above; if intended_w > 0 the
                        //   doc-level intent is set, don't re-infer).
                        // Guard 2: Physical-mode reconstruction wins (the
                        //   is_phys branch above takes the file and we
                        //   never reach this else).
                        // Guard 3: Curvz's own legacy files emit
                        //   width == viewBox.W; the != check rejects them
                        //   and leaves intent unset.
                        if (doc->canvas.intended_w == 0.0 && !w_attr.empty()
                                && !h_attr.empty()) {
                            double iw = 0.0, ih = 0.0;
                            try {
                                iw = std::stod(w_attr);
                                ih = std::stod(h_attr);
                            } catch (...) { iw = ih = 0.0; }
                            if (iw > 0.0 && ih > 0.0
                                    && (iw != w || ih != h)) {
                                doc->canvas.intended_w = iw;
                                doc->canvas.intended_h = ih;
                                doc->canvas.intended_unit = "px";
                            }
                        }
                    }
                }
            } else {
                int cw = w_attr.empty() ? 24 : (int)dbl(w_attr, 24);
                int ch = h_attr.empty() ? 24 : (int)dbl(h_attr, 24);
                doc->canvas = CanvasModel::from_legacy(cw, ch);
                // s265 m2: read explicit intent AFTER the reassign.
                read_explicit_intent();
            }
            // Don't create a layer here — wait for <g> tags
            // current_layer stays null until first <g>
            continue;
        }

        // ── <clipPath id="..."> ────────────────────────────────────────
        // Enter clip-def capture mode. The element inside (a <path>, or
        // a <g data-curvz-compound> of paths) is redirected into
        // clip_defs[id] by the handlers below (checking in_clip_def_id).
        // Children of <clipPath> don't become tree nodes — they become
        // the clip_shape of some ClipGroup that references this id.
        if (tag.rfind("clipPath ", 0) == 0 || tag == "clipPath") {
            in_clip_def_id = attr(tag, "id");
            LOG_INFO("SvgParser: <clipPath id='{}'> begin", in_clip_def_id);
            continue;
        }

        // ── S90 Stage 2 — <linearGradient id="..." x1=".." y1=".." …> ──
        // Enter gradient-def capture mode. Geometry attributes are read
        // from the element directly; <stop> children are absorbed into
        // pending_gradient.stops while in def mode. The </linearGradient>
        // close-handler flushes into gradient_defs[id]. Other gradient
        // attributes (gradientUnits, spreadMethod, gradientTransform) are
        // not yet honoured — Stage 2 ships objectBoundingBox + Pad only,
        // matching what the writer emits. Foreign SVGs with userSpaceOnUse
        // / Reflect / Repeat will read in but render as objectBoundingBox/
        // Pad equivalents (visually wrong for those edge cases — Stage 3+
        // adds full-fidelity support).
        // Helper: parse a gradient geometry attribute that may be a bare
        // number (0..1 fraction) or a percentage ("50%" → 0.5). Matches
        // SVG's relaxed grammar for objectBoundingBox-mode coords.
        auto parse_grad_coord = [](const std::string& s, double def) {
            if (s.empty()) return def;
            if (s.back() == '%')
                return dbl(s.substr(0, s.size() - 1), def * 100.0) / 100.0;
            return dbl(s, def);
        };
        if (tag.rfind("linearGradient ", 0) == 0 || tag == "linearGradient") {
            in_gradient_def_id = attr(tag, "id");
            pending_gradient = FillStyle{};
            pending_gradient.type = FillStyle::Type::LinearGradient;
            // SVG defaults: x1=0%, y1=0%, x2=100%, y2=0% (horizontal L→R).
            pending_gradient.g_x1 = parse_grad_coord(attr(tag, "x1"), 0.0);
            pending_gradient.g_y1 = parse_grad_coord(attr(tag, "y1"), 0.0);
            pending_gradient.g_x2 = parse_grad_coord(attr(tag, "x2"), 1.0);
            pending_gradient.g_y2 = parse_grad_coord(attr(tag, "y2"), 0.0);
            LOG_INFO("SvgParser: <linearGradient id='{}'> begin",
                     in_gradient_def_id);
            continue;
        }
        if (tag.rfind("radialGradient ", 0) == 0 || tag == "radialGradient") {
            in_gradient_def_id = attr(tag, "id");
            pending_gradient = FillStyle{};
            pending_gradient.type = FillStyle::Type::RadialGradient;
            // SVG radial: cx/cy/r are the outer circle, fx/fy the focal.
            // Curvz stores focal as g_x1/g_y1, centre as g_x2/g_y2,
            // radius as g_r. SVG defaults: cx=50%, cy=50%, r=50%, fx=cx,
            // fy=cy.
            pending_gradient.g_x2 = parse_grad_coord(attr(tag, "cx"), 0.5);
            pending_gradient.g_y2 = parse_grad_coord(attr(tag, "cy"), 0.5);
            pending_gradient.g_r  = parse_grad_coord(attr(tag, "r"),  0.5);
            std::string sfx = attr(tag, "fx");
            std::string sfy = attr(tag, "fy");
            pending_gradient.g_x1 = sfx.empty() ? pending_gradient.g_x2
                                                : parse_grad_coord(sfx, 0.5);
            pending_gradient.g_y1 = sfy.empty() ? pending_gradient.g_y2
                                                : parse_grad_coord(sfy, 0.5);
            LOG_INFO("SvgParser: <radialGradient id='{}'> begin",
                     in_gradient_def_id);
            continue;
        }
        // ── <stop offset="..." stop-color="..." stop-opacity="..."/> ───
        // Only honoured while inside a gradient def. Self-closing tag
        // (no close handler needed). The colour parsing reuses parse_fill
        // for solid-colour values (#RGB, #RRGGBB, rgb(...), named); url(#)
        // and currentColor stops are not Stage-2 features (Stage 3+).
        if ((tag.rfind("stop ", 0) == 0 || tag == "stop" ||
             tag.rfind("stop/", 0) == 0) &&
            !in_gradient_def_id.empty()) {
            GradientStop s;
            std::string off = attr(tag, "offset");
            // SVG offset can be "0.5" or "50%". Strip trailing % if present.
            if (!off.empty() && off.back() == '%') {
                s.offset = dbl(off.substr(0, off.size() - 1), 0.0) / 100.0;
            } else {
                s.offset = dbl(off, 0.0);
            }
            std::string sc = attr(tag, "stop-color");
            if (sc.empty()) {
                // Try style="" fallback
                std::string st = attr(tag, "style");
                if (!st.empty()) sc = style_prop(st, "stop-color");
            }
            FillStyle sf = parse_fill(sc);  // resolves #..., named, rgb()
            s.r = sf.r; s.g = sf.g; s.b = sf.b;
            std::string so = attr(tag, "stop-opacity");
            if (so.empty()) {
                std::string st = attr(tag, "style");
                if (!st.empty()) so = style_prop(st, "stop-opacity");
            }
            s.a = so.empty() ? 1.0 : dbl(so, 1.0);
            pending_gradient.stops.push_back(s);
            continue;
        }

        // ── <g> — guide layer, ref layer, layer, group, or compound ─────────
        if (tag.rfind("g ", 0) == 0 || tag == "g") {
bool is_guide_layer   = (attr(tag, "data-curvz-guide-layer") == "1");
            bool is_ref_layer     = !is_guide_layer && (attr(tag, "data-curvz-ref-layer") == "1");
            bool is_measure_layer = !is_guide_layer && !is_ref_layer && (attr(tag, "data-curvz-measure-layer") == "1");
            // ClipGroup recognised either by explicit Curvz marker or by
            // the standard SVG clip-path="url(#...)" attribute on any <g>.
            // The url() form makes us interop with foreign SVGs that use
            // clipPath without Curvz's extra marker.
            bool is_clipgroup     = !is_guide_layer && !is_ref_layer && !is_measure_layer &&
                                    (attr(tag, "data-curvz-clipgroup") == "1" ||
                                     !attr(tag, "clip-path").empty());
            // Blend — Curvz-specific container marked with data-curvz-blend="1".
            // Tolerates the old M1 marker data-curvz-blend-m1 on legacy files
            // (degraded save: that variant didn't carry A/B role tags, so the
            // close-handler will leave source slots null — the Blend renders
            // as an empty container until the user rebuilds).
            bool is_blend         = !is_guide_layer && !is_ref_layer && !is_measure_layer && !is_clipgroup &&
                                    (attr(tag, "data-curvz-blend")    == "1" ||
                                     attr(tag, "data-curvz-blend-m1") == "1");
            // Warp — Curvz-specific envelope distortion wrapper marked with
            // data-curvz-warp="1". Single source child tagged
            // data-curvz-warp-role="source" is routed into warp_source by
            // the close-tag handler. M1 serialization does not yet encode
            // the envelope curves; parsed Warps get default identity
            // envelopes until M6 adds full round-trip.
            bool is_warp          = !is_guide_layer && !is_ref_layer && !is_measure_layer && !is_clipgroup && !is_blend &&
                                    (attr(tag, "data-curvz-warp") == "1");
            bool is_compound      = !is_guide_layer && !is_ref_layer && !is_measure_layer && !is_clipgroup && !is_blend && !is_warp && (attr(tag, "data-curvz-compound") == "1");
            // TextBoxMgr — s310 m1bc — the user-visible text-flow
            // container. On-disk marker is `data-curvz-textbox-mgr="1"`
            // for new-format files; the legacy `data-curvz-textbox="1"`
            // is still recognised so pre-s310 files load through the
            // same branch and end up in the new shape. The close-tag
            // handler builds the in-memory tree:
            //     Mgr (Type::TextBoxMgr, owns the buffer)
            //     ├── children[0] Canvas TextBoxView
            //     │       └── children[0] Path (the boundary)
            //     └── children[1] Popover TextBoxView (no Path)
            // from the on-disk shape (boundary path + baseline-marker
            // paths + <text><textPath> pairs). See
            // docs/text_flow_architecture.md for the rationale.
            bool is_textbox       = !is_guide_layer && !is_ref_layer && !is_measure_layer && !is_clipgroup && !is_blend && !is_warp && !is_compound &&
                                    (attr(tag, "data-curvz-textbox")     == "1" ||
                                     attr(tag, "data-curvz-textbox-mgr") == "1");
            // TextBoxView — s310 m1c — the on-canvas (or in-popover)
            // window onto a TextBoxMgr's buffer. Marker is
            // data-curvz-textbox-view="1". Always nested inside a
            // <g data-curvz-textbox-mgr="1"> at the current open shape
            // (m1c reads the nested-on-disk format m1bc shipped; a
            // future m1d-redux moves to flat-with-mgr-iid emit, at
            // which point the nesting requirement goes away — but the
            // open-handler is the same either way: see a textbox-view
            // marker, construct a Type::TextBoxView, push onto the
            // stack, accumulate children, close-handler sweeps).
            bool is_textbox_view  = !is_guide_layer && !is_ref_layer && !is_measure_layer && !is_clipgroup && !is_blend && !is_warp && !is_compound && !is_textbox &&
                                    (attr(tag, "data-curvz-textbox-view") == "1");
            // Foreign SVG <g> without Curvz markers is treated as a Group when
            // already inside a layer (stack non-empty), not a new top-level Layer.
            bool is_group         = !is_guide_layer && !is_ref_layer && !is_measure_layer && !is_clipgroup && !is_blend && !is_warp && !is_compound && !is_textbox && !is_textbox_view &&
                                    (attr(tag, "data-curvz-group") == "1" || !stack.empty());

            if (is_clipgroup && !stack.empty()) {
                // ── ClipGroup: child of current parent ────────────────
                auto cg = std::make_unique<SceneNode>();
                cg->type = SceneNode::Type::ClipGroup;
                auto id = attr(tag, "id");
                if (!id.empty()) cg->id = id;
                auto iid = attr(tag, "data-curvz-iid");
                cg->internal_id = iid.empty() ? generate_internal_id() : iid;
                auto nm = attr(tag, "data-curvz-name");
                cg->name = nm.empty() ? (id.empty() ? "Clip" : id) : nm;
                if (attr(tag, "display") == "none") cg->visible = false;
                auto op = attr(tag, "opacity");
                if (!op.empty()) cg->opacity = dbl(op, 1.0);
                apply_shadow_attrs(*cg, tag);  // S97 m1
                // Extract the url(#id) target from clip-path="url(#id)"
                std::string cp = attr(tag, "clip-path");
                std::string want_id;
                {
                    auto open = cp.find('#');
                    auto close = cp.find(')', open == std::string::npos ? 0 : open);
                    if (open != std::string::npos && close != std::string::npos && close > open + 1)
                        want_id = cp.substr(open + 1, close - open - 1);
                }
                SceneNode* cgptr = cg.get();
                stack.back().node->children.push_back(std::move(cg));
                AffineMatrix cg_xfm = parse_transform(attr(tag, "transform"));
                // Curvz-originated clipgroups are always considered curvz
                // so children don't get reverse-reversed.
                stack.push_back({cgptr, cg_xfm, true});
                if (!want_id.empty())
                    clipgroup_pending[cgptr] = want_id;
                LOG_INFO("SvgParser: <g> clipgroup '{}' clip-ref='{}' in '{}'",
                         cgptr->name, want_id,
                         stack[stack.size()-2].node->name);
                continue;
            }

            if (is_blend && !stack.empty()) {
                // ── Blend: child of current parent ────────────────────
                // Children (both baked steps and role-tagged A/B) get
                // appended to node->children during the inner parse.
                // The close-tag handler below extracts A/B by their
                // data-curvz-blend-role attribute (stashed via a parallel
                // map: id_of_parsed_child -> role) and discards the rest
                // (baked cache — regenerated on first draw).
                auto bl = std::make_unique<SceneNode>();
                bl->type = SceneNode::Type::Blend;
                auto id = attr(tag, "id");
                if (!id.empty()) bl->id = id;
                auto iid = attr(tag, "data-curvz-iid");
                bl->internal_id = iid.empty() ? generate_internal_id() : iid;
                auto nm = attr(tag, "data-curvz-name");
                bl->name = nm.empty() ? (id.empty() ? "Blend" : id) : nm;
                if (attr(tag, "display") == "none") bl->visible = false;
                auto op = attr(tag, "opacity");
                if (!op.empty()) bl->opacity = dbl(op, 1.0);
                auto stp = attr(tag, "data-curvz-blend-steps");
                if (!stp.empty()) bl->blend_steps = std::max(1, (int)dbl(stp, 4.0));
                auto sw_user = attr(tag, "data-curvz-blend-sw-user");
                bl->blend_stroke_w_user_set = (sw_user == "1");
                auto sw_start = attr(tag, "data-curvz-blend-sw-start");
                if (!sw_start.empty()) bl->blend_stroke_w_start = dbl(sw_start, 0.0);
                auto sw_end   = attr(tag, "data-curvz-blend-sw-end");
                if (!sw_end.empty()) bl->blend_stroke_w_end = dbl(sw_end, 0.0);
                bl->blend_cache_dirty = true;
                apply_shadow_attrs(*bl, tag);  // S97 m1
                SceneNode* blptr = bl.get();
                stack.back().node->children.push_back(std::move(bl));
                AffineMatrix bl_xfm = parse_transform(attr(tag, "transform"));
                stack.push_back({blptr, bl_xfm, true});
                LOG_INFO("SvgParser: <g> blend '{}' steps={} in '{}'",
                         blptr->name, blptr->blend_steps,
                         stack[stack.size()-2].node->name);
                continue;
            }

            if (is_warp && !stack.empty()) {
                // ── Warp: child of current parent ─────────────────────
                // The single role-tagged source child gets appended to
                // node->children during the inner parse; the close-tag
                // handler below routes it into warp_source and clears
                // the children vector. M6 decodes envelope + quality
                // attrs on the wrapper here; malformed or missing
                // envelope attrs leave the envelopes empty so
                // rebuild_warp_caches falls back to default identity.
                auto wp = std::make_unique<SceneNode>();
                wp->type = SceneNode::Type::Warp;
                auto id = attr(tag, "id");
                if (!id.empty()) wp->id = id;
                auto iid = attr(tag, "data-curvz-iid");
                wp->internal_id = iid.empty() ? generate_internal_id() : iid;
                auto nm = attr(tag, "data-curvz-name");
                wp->name = nm.empty() ? (id.empty() ? "Warp" : id) : nm;
                if (attr(tag, "display") == "none") wp->visible = false;
                auto op = attr(tag, "opacity");
                if (!op.empty()) wp->opacity = dbl(op, 1.0);
                // Quality — clamp to [1,16], default 4 on missing/invalid.
                auto qstr = attr(tag, "data-curvz-warp-quality");
                if (!qstr.empty()) {
                    int q = (int)dbl(qstr, 4.0);
                    wp->warp_quality = std::clamp(q, 1, 16);
                } else {
                    wp->warp_quality = 4;
                }
                // s147 m2: preset provenance. Three attrs, all optional
                // for forward-compat with older files. Defaults: preset
                // = -1 (Custom), counts = 2 (matches default-constructed
                // SceneNode values, set as fallback so older files
                // without the attrs land on something sane).
                auto pstr = attr(tag, "data-curvz-warp-preset");
                if (!pstr.empty()) {
                    int p = (int)dbl(pstr, -1.0);
                    // Clamp to known preset range; out-of-range values
                    // collapse to -1 (Custom) rather than crash on
                    // future-format files with newer preset indices.
                    if (p >= 0 && p < curvz::utils::warp_presets::PRESET_COUNT)
                        wp->warp_preset_idx = p;
                    else
                        wp->warp_preset_idx = -1;
                } else {
                    wp->warp_preset_idx = -1;
                }
                auto tcstr = attr(tag, "data-curvz-warp-top-count");
                if (!tcstr.empty()) {
                    int tc = (int)dbl(tcstr, 2.0);
                    wp->warp_top_count = std::clamp(tc, 2, 4);
                } else {
                    wp->warp_top_count = 2;
                }
                auto bcstr = attr(tag, "data-curvz-warp-bot-count");
                if (!bcstr.empty()) {
                    int bc = (int)dbl(bcstr, 2.0);
                    wp->warp_bot_count = std::clamp(bc, 2, 4);
                } else {
                    wp->warp_bot_count = 2;
                }
                // Envelopes — decode attrs if present. Malformed input
                // leaves the envelope empty; rebuild_warp_caches will
                // seed a default identity from bbox on first draw.
                auto et = attr(tag, "data-curvz-warp-env-top");
                auto eb = attr(tag, "data-curvz-warp-env-bottom");
                if (!et.empty()) wp->warp_env_top    = decode_warp_envelope(et);
                if (!eb.empty()) wp->warp_env_bottom = decode_warp_envelope(eb);
                wp->warp_glyph_cache_dirty = true;
                wp->warp_cache_dirty       = true;
                apply_shadow_attrs(*wp, tag);  // S97 m1
                SceneNode* wpptr = wp.get();
                stack.back().node->children.push_back(std::move(wp));
                AffineMatrix wp_xfm = parse_transform(attr(tag, "transform"));
                stack.push_back({wpptr, wp_xfm, true});
                LOG_INFO("SvgParser: <g> warp '{}' quality={} env_top_n={} env_bot_n={} in '{}'",
                         wpptr->name, wpptr->warp_quality,
                         (int)wpptr->warp_env_top.nodes.size(),
                         (int)wpptr->warp_env_bottom.nodes.size(),
                         stack[stack.size()-2].node->name);
                continue;
            }

            if (is_textbox && in_defs > 0) {
                // ── TextBoxMgr in <defs> (s311 m1c-redux dual-block) ──────
                // Dual-block format: the Mgr's flow-state lives in a
                // non-rendering blob in <defs>. We hydrate the Mgr from
                // attributes here; the CDATA buffer is harvested by the
                // ![CDATA[ token handler at the top of the main loop
                // while in_textbox_mgr_def points at us. The </g> close
                // arm finalizes (clears in_textbox_mgr_def + records
                // pending placement).
                //
                // Crucially we do NOT push onto the regular parse stack.
                // The defs subtree must not contribute to any layer's
                // children — see in_defs comments for the rationale. The
                // Mgr is owned by pending_mgrs and gets spliced into a
                // layer when its first canvas view appears (or as a
                // fallback at end-of-parse if no view references it).
                auto mgr = std::make_unique<SceneNode>();
                mgr->type = SceneNode::Type::TextBoxMgr;
                auto id = attr(tag, "id");
                if (!id.empty()) mgr->id = id;
                auto iid = attr(tag, "data-curvz-iid");
                mgr->internal_id = iid.empty() ? generate_internal_id() : iid;
                auto nm = attr(tag, "data-curvz-name");
                mgr->name = nm.empty() ? (id.empty() ? "Text" : id) : nm;
                if (attr(tag, "display") == "none") mgr->visible = false;
                auto op = attr(tag, "opacity");
                if (!op.empty()) mgr->opacity = dbl(op, 1.0);
                apply_shadow_attrs(*mgr, tag);
                // Font defaults — read directly from the Mgr's own attrs
                // in dual-block format; no per-baseline <text> harvest
                // needed because the Mgr blob is the sole authority.
                {
                    auto ff = attr(tag, "data-curvz-font-family");
                    if (!ff.empty()) mgr->text_font_family = ff;
                    auto fs = attr(tag, "data-curvz-font-size");
                    if (!fs.empty()) mgr->text_font_size = dbl(fs,
                                                  mgr->text_font_size);
                    // s326 m2b — explicit leading (absent -> stays 0 -> derived)
                    auto lh = attr(tag, "data-curvz-line-height");
                    if (!lh.empty()) mgr->text_line_height = dbl(lh,
                                                  mgr->text_line_height);
                    if (attr(tag, "data-curvz-font-bold")   == "1")
                        mgr->text_bold = true;
                    if (attr(tag, "data-curvz-font-italic") == "1")
                        mgr->text_italic = true;
                }
                // Fill + stroke flow through the standard parser helper.
                // The Mgr is the text-bearing node, so its fill/stroke
                // are what the text glyphs render in.
                apply_style_attrs(*mgr, tag);
                // text_anchor / text_align are canonical for Mgr-owned
                // text — boundary determines layout, not anchoring.
                mgr->text_anchor = "start";
                mgr->text_align  = "left";
                // Caret byte. Buffer content arrives via CDATA after
                // this open tag; the caret will be re-clamped against
                // the final content size at close-time.
                {
                    auto cb = attr(tag, "data-curvz-caret-byte");
                    if (!cb.empty()) {
                        try {
                            int64_t v = std::stol(cb);
                            if (v < 0) v = 0;
                            mgr->text_caret_byte = (int32_t)v;
                        } catch (...) {
                            mgr->text_caret_byte = 0;
                        }
                    }
                }
                SceneNode* mgr_raw = mgr.get();
                pending_mgrs.push_back(std::move(mgr));
                mgr_def_by_iid[mgr_raw->internal_id] = mgr_raw;
                in_textbox_mgr_def = mgr_raw;
                mgr_def_is_markup = (attr(tag, "data-curvz-markup") == "1"); // s325
                LOG_INFO("SvgParser: <defs> textbox-mgr '{}' opened "
                         "(iid='{}', font='{}' size={:.1f})",
                         mgr_raw->name, mgr_raw->internal_id,
                         mgr_raw->text_font_family,
                         mgr_raw->text_font_size);
            } else if (is_textbox && !stack.empty()) {
                // ── Legacy TextBoxMgr in layer (s310 m1bc nested format) ──
                // Pre-s311 files emitted the Mgr's <g> directly in the
                // layer with the buffer in data-curvz-content. This arm
                // is the back-compat path: construct the Mgr at the
                // current parent's children, push onto the stack so its
                // inner views (or legacy direct children) populate
                // normally.
                auto mgr = std::make_unique<SceneNode>();
                mgr->type = SceneNode::Type::TextBoxMgr;
                auto id = attr(tag, "id");
                if (!id.empty()) mgr->id = id;
                auto iid = attr(tag, "data-curvz-iid");
                mgr->internal_id = iid.empty() ? generate_internal_id() : iid;
                auto nm = attr(tag, "data-curvz-name");
                mgr->name = nm.empty() ? (id.empty() ? "Text" : id) : nm;
                if (attr(tag, "display") == "none") mgr->visible = false;
                auto op = attr(tag, "opacity");
                if (!op.empty()) mgr->opacity = dbl(op, 1.0);
                apply_shadow_attrs(*mgr, tag);
                // Legacy: buffer rides on data-curvz-content attr.
                mgr->text_content = attr(tag, "data-curvz-content");
                {
                    auto cb = attr(tag, "data-curvz-caret-byte");
                    if (!cb.empty()) {
                        try {
                            int64_t v = std::stol(cb);
                            if (v < 0) v = 0;
                            if (v > (int64_t)mgr->text_content.size())
                                v = (int64_t)mgr->text_content.size();
                            mgr->text_caret_byte = (int32_t)v;
                        } catch (...) {
                            mgr->text_caret_byte = 0;
                        }
                    }
                }
                TextBoxPending pending;
                SceneNode* mgrptr = mgr.get();
                textbox_pending[mgrptr] = pending;
                stack.back().node->children.push_back(std::move(mgr));
                AffineMatrix mgr_xfm = parse_transform(attr(tag, "transform"));
                stack.push_back({mgrptr, mgr_xfm, /*is_curvz=*/true});
                LOG_INFO("SvgParser: <g> textbox-mgr '{}' (legacy in-layer) "
                         "created in '{}'",
                         mgrptr->name, stack[stack.size()-2].node->name);
            } else if (is_textbox_view && !stack.empty()) {
                // ── TextBoxView: dual-block or legacy-nested ──────────────
                // Two cases, distinguished by whether the view carries a
                // data-curvz-mgr-iid back-pointer (dual-block) or sits
                // inside an Mgr on the parse stack (legacy nested).
                auto view = std::make_unique<SceneNode>();
                view->type = SceneNode::Type::TextBoxView;
                auto id = attr(tag, "id");
                if (!id.empty()) view->id = id;
                auto iid = attr(tag, "data-curvz-iid");
                view->internal_id = iid.empty() ? generate_internal_id() : iid;
                auto nm = attr(tag, "data-curvz-name");
                if (!nm.empty()) view->name = nm;
                if (attr(tag, "display") == "none") view->visible = false;
                auto op = attr(tag, "opacity");
                if (!op.empty()) view->opacity = dbl(op, 1.0);
                {
                    auto vk = attr(tag, "data-curvz-view-kind");
                    if (vk == "popover") {
                        view->view_kind = SceneNode::ViewKind::Popover;
                    } else {
                        view->view_kind = SceneNode::ViewKind::Canvas;
                    }
                }
                view->view_byte_start = 0;
                view->view_bytes_consumed = 0;

                // Dual-block path: data-curvz-mgr-iid present → look up
                // the Mgr in mgr_def_by_iid (hydrated from <defs>) and
                // splice it into the current layer if it hasn't been
                // placed yet. The view becomes a child of the Mgr.
                std::string mgr_iid = attr(tag, "data-curvz-mgr-iid");
                bool routed_to_mgr_def = false;
                if (!mgr_iid.empty()) {
                    auto it = mgr_def_by_iid.find(mgr_iid);
                    if (it != mgr_def_by_iid.end()) {
                        SceneNode* mgr = it->second;
                        // Splice Mgr into current layer (the parent of
                        // this view's <g>) if not already placed. The
                        // Mgr's pending_mgrs unique_ptr gets transferred
                        // into the layer's children; mgr_def_by_iid
                        // raw pointer remains valid (ownership change
                        // doesn't invalidate the raw).
                        if (!placed_mgrs.count(mgr_iid)) {
                            // Find the unique_ptr owning this Mgr in
                            // pending_mgrs and move it into the layer.
                            // O(N) scan but pending_mgrs is small (one
                            // entry per Mgr in the document).
                            std::unique_ptr<SceneNode> owned;
                            for (auto pit = pending_mgrs.begin();
                                 pit != pending_mgrs.end(); ++pit) {
                                if (pit->get() == mgr) {
                                    owned = std::move(*pit);
                                    pending_mgrs.erase(pit);
                                    break;
                                }
                            }
                            if (owned) {
                                stack.back().node->children.push_back(
                                    std::move(owned));
                                placed_mgrs.insert(mgr_iid);
                                LOG_INFO("SvgParser: TextBoxMgr '{}' "
                                         "placed in layer '{}' (first "
                                         "view referenced it)",
                                         mgr->name,
                                         stack.back().node->name);
                            } else {
                                LOG_WARN("SvgParser: TextBoxView '{}' "
                                         "references Mgr iid='{}' but "
                                         "ownership transfer failed "
                                         "(Mgr already moved?)",
                                         view->internal_id, mgr_iid);
                            }
                        }
                        // Push the view as a child of the Mgr (NOT the
                        // current layer parent). The stack entry uses
                        // the Mgr's accumulated transform — for now
                        // identity, since dual-block views don't carry
                        // transforms on the Mgr blob.
                        SceneNode* vptr = view.get();
                        mgr->children.push_back(std::move(view));
                        AffineMatrix v_xfm = parse_transform(attr(tag,
                                                  "transform"));
                        stack.push_back({vptr, v_xfm, /*is_curvz=*/true});
                        routed_to_mgr_def = true;
                        LOG_INFO("SvgParser: <g> textbox-view '{}' "
                                 "(kind={}, idx={}/{}) routed to Mgr "
                                 "'{}' via mgr-iid back-pointer",
                                 vptr->internal_id,
                                 vptr->view_kind ==
                                     SceneNode::ViewKind::Canvas
                                     ? "canvas" : "popover",
                                 attr(tag, "data-curvz-view-index"),
                                 attr(tag, "data-curvz-view-count"),
                                 mgr->name);
                    } else {
                        LOG_WARN("SvgParser: TextBoxView '{}' references "
                                 "mgr-iid='{}' but no matching Mgr in "
                                 "<defs> — treating as legacy",
                                 view->internal_id, mgr_iid);
                    }
                }
                if (!routed_to_mgr_def) {
                    // Legacy nested-shape: view's <g> sits inside its
                    // Mgr's <g> on disk, so the current stack parent IS
                    // the Mgr. Push as today.
                    SceneNode* vptr = view.get();
                    stack.back().node->children.push_back(std::move(view));
                    AffineMatrix v_xfm = parse_transform(attr(tag,
                                              "transform"));
                    stack.push_back({vptr, v_xfm, /*is_curvz=*/true});
                    LOG_INFO("SvgParser: <g> textbox-view '{}' (kind={}) "
                             "created in '{}' (legacy nested)",
                             vptr->internal_id,
                             vptr->view_kind ==
                                 SceneNode::ViewKind::Canvas
                                 ? "canvas" : "popover",
                             stack[stack.size()-2].node->name);
                }
            } else if ((is_group || is_compound) && !stack.empty()) {
                // ── Group or Compound: child of current parent ─────────────
                auto g = std::make_unique<SceneNode>();
                g->type = is_compound ? SceneNode::Type::Compound : SceneNode::Type::Group;
                auto id = attr(tag, "id");
                if (!id.empty()) g->id = id;
                auto nm = attr(tag, "data-curvz-name");
                g->name = nm.empty() ? id : nm;
                auto disp = attr(tag, "display");
                if (disp == "none") g->visible = false;
                auto op = attr(tag, "opacity");
                if (!op.empty()) g->opacity = dbl(op, 1.0);
                apply_shadow_attrs(*g, tag);  // S97 m1
                SceneNode* gptr = g.get();
                stack.back().node->children.push_back(std::move(g));
                // If this Group/Compound is the source child of a Warp
                // wrapper, capture its role here (the <g> tag itself
                // carries data-curvz-warp-role="source" rather than any
                // inner path). The Warp close-handler will route it
                // into warp_source.
                {
                    auto warp_role = attr(tag, "data-curvz-warp-role");
                    if (!warp_role.empty() && warp_role == "source") {
                        SceneNode *par = stack.back().node;
                        if (par && par->is_warp())
                            warp_role_pending[gptr] = 's';
                    }
                }
                // Store the group's own transform so we can apply it on close
                AffineMatrix g_xfm = parse_transform(attr(tag, "transform"));
                // Curvz-originated groups carry data-curvz-group="1" or
                // data-curvz-compound="1"; foreign groups lack these.
                bool g_is_curvz = (attr(tag, "data-curvz-group")    == "1" ||
                                   attr(tag, "data-curvz-compound") == "1");
                stack.push_back({gptr, g_xfm, g_is_curvz});
                LOG_INFO("SvgParser: <g> {} '{}' created in '{}'",
                         is_compound ? "compound" : "group",
                         gptr->name, stack[stack.size()-2].node->name);
            } else if (is_ref_layer) {
                // ── RefLayer: dedicated references layer ───────────────────
                auto rl = std::make_unique<SceneNode>();
                rl->type = SceneNode::Type::RefLayer;
                rl->name = "References";
                rl->visible = true;
                rl->color = "#D91ABF"; // default magenta
                if (attr(tag, "display") == "none") rl->visible = false;
                if (attr(tag, "data-curvz-locked") == "1") rl->locked = true;
                auto rcol = attr(tag, "data-curvz-color");
                if (!rcol.empty()) rl->color = rcol;
                // S110 m4: round-trip iid+name on the layer wrapper itself.
                // Without this the assign_iids safety net mints a fresh iid
                // every load even though SvgWriter now emits one.
                auto riid = attr(tag, "data-curvz-iid");
                if (!riid.empty()) rl->internal_id = riid;
                auto rname = attr(tag, "data-curvz-name");
                if (!rname.empty()) rl->name = rname;
                SceneNode* rlptr = rl.get();
                doc->layers.push_back(std::move(rl));
                current_layer = rlptr;
                stack.clear();
                stack.push_back({rlptr, AffineMatrix{}});
                LOG_INFO("SvgParser: RefLayer loaded");
            } else if (is_measure_layer) {
                // ── MeasureLayer: persistent measurement layer ─────────────
                auto ml = std::make_unique<SceneNode>();
                ml->type = SceneNode::Type::MeasureLayer;
                ml->name = "Measurements";
                ml->visible = true;
                ml->color = "#22C55E";
                if (attr(tag, "display") == "none") ml->visible = false;
                if (attr(tag, "data-curvz-locked") == "1") ml->locked = true;
                // s150 fix1: legacy fallback for pre-s150 files. New
                // writes go to the root <svg> attrs (see svg-root parse
                // above). Reading both sides means a pre-s150 file with
                // the prefs on the layer still loads correctly; on next
                // save the writer puts them on the root, completing the
                // migration silently.
                if (attr(tag, "data-curvz-save-to-layer") == "1")
                    doc->measure_save_to_layer = true;
                if (attr(tag, "data-curvz-destruct-after-copy") == "1")
                    doc->measure_destruct_after_copy = true;
                // S110 m4: round-trip iid+name on the layer wrapper itself.
                auto miid = attr(tag, "data-curvz-iid");
                if (!miid.empty()) ml->internal_id = miid;
                auto mname = attr(tag, "data-curvz-name");
                if (!mname.empty()) ml->name = mname;
                SceneNode* mlptr = ml.get();
                doc->layers.push_back(std::move(ml));
                current_layer = mlptr;
                stack.clear();
                stack.push_back({mlptr, AffineMatrix{}});
                LOG_INFO("SvgParser: MeasureLayer loaded");
            } else if (is_guide_layer) {
                // ── GuideLayer: special top-level layer ────────────────────
                auto gl = std::make_unique<SceneNode>();
                gl->type = SceneNode::Type::GuideLayer;
                gl->name = "Guides";
                gl->visible = true;
                if (attr(tag, "data-curvz-locked") == "1") gl->locked = true;
                auto gcol = attr(tag, "data-curvz-guide-color");
                if (!gcol.empty()) {
                    auto p1 = gcol.find(',');
                    auto p2 = gcol.find(',', p1+1);
                    if (p1 != std::string::npos && p2 != std::string::npos) {
                        doc->guide_color_r = dbl(gcol.substr(0, p1));
                        doc->guide_color_g = dbl(gcol.substr(p1+1, p2-p1-1));
                        doc->guide_color_b = dbl(gcol.substr(p2+1));
                    }
                }
                // S110 m4: round-trip iid+name on the layer wrapper itself.
                auto giid = attr(tag, "data-curvz-iid");
                if (!giid.empty()) gl->internal_id = giid;
                auto gname = attr(tag, "data-curvz-name");
                if (!gname.empty()) gl->name = gname;
                current_layer = gl.get();
                stack.clear();
                stack.push_back({current_layer, AffineMatrix{}});
                doc->layers.push_back(std::move(gl));
                LOG_INFO("SvgParser: <g> guide layer created");
            } else {
                // ── Layer: top-level container ────────────────────────────
                auto l = std::make_unique<SceneNode>();
                l->type = SceneNode::Type::Layer;
                // S96 m2: source of truth for layer name is data-curvz-name
                // (verbatim, with spaces and specials intact). The SVG `id`
                // is now a derived handle (sanitised_name + ¶ + short_iid)
                // and is not meant to be parsed back. Legacy files written
                // by older Curvz builds had id="<layer.name>" with no
                // data-curvz-name — fall back to id for those.
                auto id  = attr(tag, "id");
                auto nm  = attr(tag, "data-curvz-name");
                auto iid = attr(tag, "data-curvz-iid");
                if (!nm.empty())      l->name = nm;
                else if (!id.empty()) l->name = id;
                l->internal_id = iid.empty() ? generate_internal_id() : iid;
                auto disp = attr(tag, "display");
                if (disp == "none") l->visible = false;
                auto op = attr(tag, "opacity");
                if (!op.empty()) l->opacity = dbl(op, 1.0);
                if (attr(tag, "data-curvz-locked") == "1") l->locked = true;
                auto col = attr(tag, "data-curvz-color");
                if (!col.empty()) l->color = col;
                // Curvz-originated layers carry data-curvz-layer="1".
                bool l_is_curvz = (attr(tag, "data-curvz-layer") == "1");
                // Capture the layer's own transform so it can be applied to
                // children on close. Foreign SVGs often put a master scale/
                // translate on the outermost <g> (Illustrator, Affinity, etc.);
                // without this, authored coordinates render at raw values
                // instead of scaled to fill the viewBox. Curvz-written layers
                // never have transforms, so this is a no-op for them.
                AffineMatrix l_xfm = parse_transform(attr(tag, "transform"));
                current_layer = l.get();
                stack.clear();  // layers are always top-level
                stack.push_back({current_layer, l_xfm, l_is_curvz});
                doc->layers.push_back(std::move(l));
                LOG_INFO("SvgParser: <g> layer '{}' created", current_layer->name);
            }
            continue;
        }

        if (!current_parent() && in_defs == 0) {
            // Bare objects outside any <g> — put in a default layer.
            // S97 m5: gated by in_defs==0 so we don't fabricate a
            // spurious Layer when walking through <defs>/<filter>/...
            // content. Defs material isn't part of the visible tree.
            auto l = std::make_unique<SceneNode>();
            l->type = SceneNode::Type::Layer;
            l->name = "Layer 1";
            current_layer = l.get();
            stack.push_back({current_layer, AffineMatrix{}});
            doc->layers.push_back(std::move(l));
        }

        // ── line (guide) ──────────────────────────────────────────────────
        if (tag.rfind("line ", 0) == 0 && attr(tag, "data-curvz-guide") == "1") {
            if (current_layer && current_layer->is_guide_layer()) {
                auto g = std::make_unique<SceneNode>();
                g->type = SceneNode::Type::Guide;
                // S96 m2: name from data-curvz-name (verbatim) over id
                // (encoded handle). iid mints fresh if absent (legacy).
                auto id  = attr(tag, "id");
                auto nm  = attr(tag, "data-curvz-name");
                auto iid = attr(tag, "data-curvz-iid");
                if (!nm.empty())      g->name = nm;
                else if (!id.empty()) g->name = id;
                g->internal_id = iid.empty() ? generate_internal_id() : iid;
                if (attr(tag, "data-curvz-locked") == "1") g->locked = true;

                // S49+ new-model fields only.
                g->guide_x     = dbl(attr(tag, "data-curvz-guide-x"));
                g->guide_y     = dbl(attr(tag, "data-curvz-guide-y"));
                g->guide_angle = dbl(attr(tag, "data-curvz-guide-angle"));

                LOG_DEBUG("SvgParser: guide anchor=({:.2f},{:.2f}) angle={:.2f}",
                          g->guide_x, g->guide_y, g->guide_angle);
                current_layer->children.push_back(std::move(g));
            }
            continue;
        }

        // ── line (measurement) ────────────────────────────────────────────
        if (tag.rfind("line ", 0) == 0 && attr(tag, "data-curvz-measure") == "1") {
            if (current_layer && current_layer->is_measure_layer()) {
                auto mn = std::make_unique<SceneNode>();
                mn->type = SceneNode::Type::Measurement;
                // S89: stable identity via internal_id. Legacy files (pre-S89)
                // have no data-curvz-iid — mint one on load so subsequent
                // saves carry it. Display name is synthesised live from
                // coords + active doc unit, so we no longer read `id`.
                auto iid = attr(tag, "data-curvz-iid");
                mn->internal_id = iid.empty() ? generate_internal_id() : iid;
                mn->measure_x1 = dbl(attr(tag, "data-curvz-mx1"));
                mn->measure_y1 = dbl(attr(tag, "data-curvz-my1"));
                mn->measure_x2 = dbl(attr(tag, "data-curvz-mx2"));
                mn->measure_y2 = dbl(attr(tag, "data-curvz-my2"));
                if (attr(tag, "display") == "none") mn->visible = false;
                LOG_DEBUG("SvgParser: measurement iid='{}' ({:.2f},{:.2f})-({:.2f},{:.2f})",
                          mn->internal_id, mn->measure_x1, mn->measure_y1,
                          mn->measure_x2, mn->measure_y2);
                current_layer->children.push_back(std::move(mn));
            }
            continue;
        }

        // ── rect ──────────────────────────────────────────────────────────
        if (tag.rfind("rect ", 0) == 0) {
            GlyphObject obj;
            obj.type = GlyphObject::Type::Path;
            obj.id   = attr(tag, "id");
            if (obj.id.empty()) obj.id = "obj" + std::to_string(obj_counter++);
            double rx_ = dbl(attr(tag, "rx")), ry_ = dbl(attr(tag, "ry"));
            obj.path = std::make_unique<PathData>(rect_to_path(
                dbl(attr(tag, "x")), dbl(attr(tag, "y")),
                dbl(attr(tag, "width"), 1.0), dbl(attr(tag, "height"), 1.0),
                rx_, ry_));
            apply_style_attrs(obj, tag);
            { auto xfm = parse_transform(attr(tag, "transform"));
              if (obj.path) apply_transform_to_path(*obj.path, xfm); }
            // s203 m3: route through push_into_parent so a <rect> inside
            // <clipPath id="X"> stashes into clip_defs[X] instead of trying
            // to dereference current_parent() (which is null at top-level
            // <defs> scope before any <g> has opened). Yelp.svg hits this
            // exact path. push_into_parent also no-ops gracefully if
            // current_parent() is null AND we're not in clipPath def mode
            // — silent drop, but it's better than the segfault that was
            // there before.
            push_into_parent(std::make_unique<SceneNode>(std::move(obj)));
            continue;
        }

        // ── ellipse / circle ──────────────────────────────────────────────
        if (tag.rfind("ellipse ", 0) == 0) {
            GlyphObject obj;
            obj.type = GlyphObject::Type::Path;
            obj.id   = attr(tag, "id");
            if (obj.id.empty()) obj.id = "obj" + std::to_string(obj_counter++);
            obj.path = std::make_unique<PathData>(ellipse_to_path(
                dbl(attr(tag, "cx"), 12.0), dbl(attr(tag, "cy"), 12.0),
                dbl(attr(tag, "rx"),  8.0), dbl(attr(tag, "ry"),  8.0)));
            apply_style_attrs(obj, tag);
            { auto xfm = parse_transform(attr(tag, "transform"));
              if (obj.path) apply_transform_to_path(*obj.path, xfm); }
            // s203 m3: route through push_into_parent — same rationale as
            // the <rect> handler above.
            push_into_parent(std::make_unique<SceneNode>(std::move(obj)));
            continue;
        }

        // ── Image ─────────────────────────────────────────────────────────
        if (tag.rfind("image ", 0) == 0 || tag == "image") {
            if (attr(tag, "data-curvz-image") == "1") {
                auto img = std::make_unique<SceneNode>();
                img->type       = SceneNode::Type::Image;
                img->id         = attr(tag, "id");
                if (img->id.empty()) img->id = "obj" + std::to_string(obj_counter++);
                img->name       = attr(tag, "data-curvz-name");
                if (img->name.empty()) img->name = "Image";
                img->image_path = attr(tag, "href");
                if (img->image_path.empty()) img->image_path = attr(tag, "xlink:href");
                img->image_x    = dbl(attr(tag, "x"));
                img->image_y    = dbl(attr(tag, "y"));
                img->image_w    = dbl(attr(tag, "width"));
                img->image_h    = dbl(attr(tag, "height"));
                // Parse transform matrix if present
                auto xfm = attr(tag, "transform");
                if (!xfm.empty() && xfm.find("matrix(") != std::string::npos) {
                    auto p = xfm.find("matrix(") + 7;
                    auto q = xfm.find(")", p);
                    if (q != std::string::npos) {
                        std::string vals = xfm.substr(p, q - p);
                        // Parse 6 comma-separated values: a,b,c,d,e,f
                        double mv[6] = {1,0,0,1,0,0};
                        int vi = 0;
                        size_t pos = 0;
                        while (vi < 6 && pos < vals.size()) {
                            size_t next = vals.find_first_of(",", pos);
                            std::string tok = vals.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
                            try { mv[vi++] = std::stod(tok); } catch (...) {}
                            if (next == std::string::npos) break;
                            pos = next + 1;
                        }
                        // Recover 2x2 rotation/shear from full matrix
                        // (e,f encode translation-around-centre which we store in image_x/y)
                        img->transform.a = mv[0]; img->transform.b = mv[1];
                        img->transform.c = mv[2]; img->transform.d = mv[3];
                        img->transform.e = 0.0;   img->transform.f = 0.0;
                    }
                }
                if (!attr(tag, "display").empty()) img->visible = false;
                auto op = attr(tag, "opacity");
                if (!op.empty()) img->opacity = dbl(op, 1.0);
                LOG_DEBUG("SvgParser: image '{}' {}x{}", img->image_path,
                          img->image_w, img->image_h);
                // s203 m3: route through push_into_parent — same rationale
                // as the <rect> handler above.
                push_into_parent(std::move(img));
                continue;
            }
        }

        if (tag.rfind("circle ", 0) == 0) {
            // ── Ref point ─────────────────────────────────────────────────
            if (attr(tag, "data-curvz-ref") == "1") {
                auto ref = std::make_unique<SceneNode>();
                ref->type  = SceneNode::Type::Ref;
                ref->id    = attr(tag, "id");
                if (ref->id.empty()) ref->id = "obj" + std::to_string(obj_counter++);
                ref->name  = attr(tag, "data-curvz-name");
                if (ref->name.empty()) {
                    // s177: empty-name fallback routed through
                    // the funnel so two unnamed legacy refpts
                    // don't both become literal "Ref" and
                    // collide. Non-empty parsed names pass
                    // through verbatim — round-trip fidelity
                    // for user-typed names is non-negotiable.
                    ref->name = doc->uniquify_name("Ref");
                }
                // S99: read data-curvz-iid so ref points keep stable identity
                // across save→load. Pre-S99 the parser silently dropped this
                // attribute, and the parser's safety-net assign_iids() sweep
                // minted a fresh one — so every ref point regenerated its iid
                // on every load. Suspected contributor to the "iid regen on
                // unrelated nodes" hunt (S97/S99 backlog).
                auto ref_iid = attr(tag, "data-curvz-iid");
                ref->internal_id = ref_iid.empty() ? generate_internal_id() : ref_iid;
                ref->ref_x = dbl(attr(tag, "cx"));
                ref->ref_y = dbl(attr(tag, "cy"));
                if (!attr(tag, "display").empty()) ref->visible = false;
                // s177: data-curvz-export-origin removed from the save
                // format (export coord system became the ruler).
                // s179 m2: the runtime is_export_origin field was also
                // removed — the entire promotion stack is gone, since
                // the ruler origin is the canonical export origin and
                // a per-refpt promotion concept no longer applies.
                LOG_DEBUG("SvgParser: ref point at ({:.4f},{:.4f})", ref->ref_x, ref->ref_y);
                // s203 m3: route through push_into_parent — same rationale
                // as the <rect> handler above. (A ref-point inside a
                // clipPath is meaningless SVG-wise, but the helper still
                // routes safely.)
                push_into_parent(std::move(ref));
                continue;
            }
            // ── Regular circle ────────────────────────────────────────────
            GlyphObject obj;
            obj.type = GlyphObject::Type::Path;
            obj.id   = attr(tag, "id");
            if (obj.id.empty()) obj.id = "obj" + std::to_string(obj_counter++);
            double r = dbl(attr(tag, "r"), 8.0);
            obj.path = std::make_unique<PathData>(ellipse_to_path(
                dbl(attr(tag, "cx"), 12.0), dbl(attr(tag, "cy"), 12.0), r, r));
            apply_style_attrs(obj, tag);
            { auto xfm = parse_transform(attr(tag, "transform"));
              if (obj.path) apply_transform_to_path(*obj.path, xfm); }
            // s203 m3: route through push_into_parent — same rationale as
            // the <rect> handler above.
            push_into_parent(std::make_unique<SceneNode>(std::move(obj)));
            continue;
        }

        // ── text ──────────────────────────────────────────────────────────
        if (tag.rfind("text ", 0) == 0 || tag == "text") {
            auto obj = std::make_unique<SceneNode>();
            obj->type = SceneNode::Type::Text;
            obj->id          = attr(tag, "id");
            if (obj->id.empty()) obj->id = "text" + std::to_string(obj_counter++);
            obj->name        = attr(tag, "data-curvz-name");
            obj->internal_id = attr(tag, "data-curvz-iid");
            if (obj->name.empty()) obj->name = "Text";

            obj->text_x           = dbl(attr(tag, "x"));
            obj->text_y           = dbl(attr(tag, "y"));
            obj->text_font_family = attr(tag, "font-family");
            if (obj->text_font_family.empty()) obj->text_font_family = "Sans";
            auto fs = attr(tag, "font-size");
            if (!fs.empty()) obj->text_font_size = dbl(fs, 24.0);
            obj->text_bold   = (attr(tag, "font-weight") == "bold");
            obj->text_italic = (attr(tag, "font-style")  == "italic");
            auto anchor = attr(tag, "text-anchor");
            obj->text_anchor = anchor.empty() ? "start" : anchor;

            auto align_val = attr(tag, "data-curvz-align");
            obj->text_align = align_val.empty() ? "left" : align_val;

            // Content: prefer the round-trip attribute; split_tags discards char data.
            auto content = attr(tag, "data-curvz-content");
            obj->text_content = content;

            // Baseline shift + letter spacing
            auto bs = attr(tag, "data-curvz-baseline-shift");
            if (!bs.empty()) obj->text_baseline_shift = dbl(bs);
            auto ls = attr(tag, "letter-spacing");
            if (!ls.empty()) obj->text_letter_spacing = dbl(ls);

            // Text-on-path attributes
            auto path_id = attr(tag, "data-curvz-path-id");
            if (!path_id.empty()) {
                obj->text_path_id = path_id;
                auto off = attr(tag, "data-curvz-path-offset");
                if (!off.empty()) obj->text_path_offset = dbl(off);
                obj->text_path_flip = (attr(tag, "data-curvz-path-flip") == "1");
            }

            // s301 m1a — text container model parsing. Iid migration for
            // boundary list and line-path id is handled below in the same
            // pass that already migrates text_path_id (see svgid_to_iid map
            // around line 2666). Margins are simple scalars, no migration
            // needed.
            auto bids = attr(tag, "data-curvz-boundary-ids");
            if (!bids.empty()) {
                obj->text_boundary_ids.clear();
                size_t i = 0;
                while (i < bids.size()) {
                    while (i < bids.size() && std::isspace((unsigned char)bids[i])) ++i;
                    size_t j = i;
                    while (j < bids.size() && !std::isspace((unsigned char)bids[j])) ++j;
                    if (j > i) obj->text_boundary_ids.emplace_back(bids.substr(i, j - i));
                    i = j;
                }
            }
            auto lpid = attr(tag, "data-curvz-line-path-id");
            if (!lpid.empty()) obj->text_line_path_id = lpid;
            auto mt = attr(tag, "data-curvz-margin-top");    if (!mt.empty()) obj->text_margin_top    = dbl(mt);
            auto mb = attr(tag, "data-curvz-margin-bottom"); if (!mb.empty()) obj->text_margin_bottom = dbl(mb);
            auto ml = attr(tag, "data-curvz-margin-left");   if (!ml.empty()) obj->text_margin_left   = dbl(ml);
            auto mr = attr(tag, "data-curvz-margin-right");  if (!mr.empty()) obj->text_margin_right  = dbl(mr);

            if (attr(tag, "display") == "none") obj->visible = false;
            auto op = attr(tag, "opacity");
            if (!op.empty()) obj->opacity = dbl(op, 1.0);

            // Apply fill/stroke from SVG presentation attributes via shared helper.
            {
                GlyphObject tmp;
                apply_style_attrs(tmp, tag);
                obj->fill   = tmp.fill;
                obj->stroke = tmp.stroke;
                obj->fill_swatch_id   = tmp.fill_swatch_id;
                obj->stroke_swatch_id = tmp.stroke_swatch_id;
            }

            if (!current_parent() && in_clip_def_id.empty()) {
                LOG_WARN("SvgParser: <text> found with no parent, skipping");
                continue;
            }
            LOG_DEBUG("SvgParser: text \"{}\" at ({:.1f},{:.1f})", obj->text_content, obj->text_x, obj->text_y);
            // s203 m3: route through push_into_parent — same rationale as
            // the <rect> handler above. Text inside <clipPath> is unusual
            // but no longer crashes; the existing null-check above only
            // skips when we're ALSO not in clipPath def mode.
            push_into_parent(std::move(obj));
            continue;
        }

        // ── path ──────────────────────────────────────────────────────────
        if (tag.rfind("path ", 0) == 0) {
            auto d_val = attr(tag, "d");
            LOG_INFO("SvgParser: <path> tag found, d empty={}, layer={}, d_prefix='{}'", 
                     d_val.empty(), current_layer ? current_layer->name : "NULL",
                     d_val.substr(0, std::min((int)d_val.size(), 40)));
            if (!d_val.empty()) {
                // ── Check for compound-as-single-path (new format) ─────────
                bool is_compound_path = (attr(tag, "data-curvz-compound") == "1");
                if (is_compound_path) {
                    // Build a Compound node and split d into subpaths by M
                    auto compound = std::make_unique<SceneNode>();
                    compound->type = SceneNode::Type::Compound;
                    compound->id   = attr(tag, "id");
                    if (compound->id.empty()) compound->id = "compound" + std::to_string(obj_counter++);
                    auto nm = attr(tag, "data-curvz-name");
                    compound->name = nm.empty() ? compound->id : nm;
                    if (attr(tag, "display") == "none") compound->visible = false;
                    auto op = attr(tag, "opacity");
                    if (!op.empty()) compound->opacity = dbl(op, 1.0);
                    // S58k: apply the SVG tag's fill/stroke style attrs onto
                    // the Compound itself. Pre-S58k this branch skipped this
                    // step; Compound.fill defaulted to CurrentColor while the
                    // children received their own fills individually. That
                    // was invisible under the old renderer (which read
                    // children), but S58g's renderer reads Compound.fill —
                    // so without this, loaded compound-paths render as
                    // CurrentColor regardless of saved colour.
                    apply_style_attrs(*compound, tag);

                    // Split d by subpaths: each starts with M
                    // Collect "M ..." segments
                    std::vector<std::string> sub_ds;
                    {
                        std::string cur;
                        size_t i = 0;
                        while (i < d_val.size()) {
                            // Find next 'M' or 'm' that starts a new subpath
                            size_t next = d_val.find_first_of("Mm", i + 1);
                            if (next == std::string::npos) {
                                cur = d_val.substr(i);
                                sub_ds.push_back(cur);
                                break;
                            }
                            sub_ds.push_back(d_val.substr(i, next - i));
                            i = next;
                        }
                        if (sub_ds.empty()) sub_ds.push_back(d_val);
                    }

                    // Split types by '|' — one segment per child
                    auto types_val = attr(tag, "data-curvz-types");
                    std::vector<std::string> sub_types;
                    {
                        std::istringstream ss(types_val);
                        std::string tok;
                        while (std::getline(ss, tok, '|'))
                            sub_types.push_back(tok);
                    }

                    // Style from compound itself
                    GlyphObject style_obj;
                    style_obj.type = GlyphObject::Type::Path;
                    apply_style_attrs(style_obj, tag);

                    // Child ids from data-curvz-child-ids
                    auto child_ids_str = attr(tag, "data-curvz-child-ids");
                    std::vector<std::string> child_ids;
                    {
                        std::istringstream ss(child_ids_str);
                        std::string tok;
                        while (std::getline(ss, tok, ','))
                            child_ids.push_back(tok);
                    }

                    for (size_t ci = 0; ci < sub_ds.size(); ++ci) {
                        auto child = std::make_unique<SceneNode>();
                        child->type  = SceneNode::Type::Path;
                        child->id    = (ci < child_ids.size()) ? child_ids[ci]
                                       : "obj" + std::to_string(obj_counter++);
                        child->fill  = style_obj.fill;
                        child->stroke = style_obj.stroke;
                        child->fill_swatch_id   = style_obj.fill_swatch_id;
                        child->stroke_swatch_id = style_obj.stroke_swatch_id;
                        child->path  = std::make_unique<PathData>(parse_path_d(sub_ds[ci]));

                        // Restore node types for this child
                        if (ci < sub_types.size() && child->path) {
                            auto& nodes = child->path->nodes;
                            int ni = 0;
                            for (char ch : sub_types[ci]) {
                                if (ch == ' ' || ch == '\t') continue;
                                if (ni >= (int)nodes.size()) break;
                                switch (ch) {
                                    case 'S': nodes[ni].type = BezierNode::Type::Symmetric; break;
                                    case 'M': nodes[ni].type = BezierNode::Type::Smooth;    break;
                                    case 'C': nodes[ni].type = BezierNode::Type::Cusp;      break;
                                    case 'K': nodes[ni].type = BezierNode::Type::Corner;    break;
                                    default: break;
                                }
                                ++ni;
                            }
                        }
                        compound->children.push_back(std::move(child));
                    }

                    LOG_INFO("SvgParser: <path> compound '{}' created with {} children",
                             compound->name, compound->children.size());
                    // If this Compound is the source child of a Warp wrapper,
                    // capture its role here (the <path data-curvz-compound>
                    // tag itself carries data-curvz-warp-role="source").
                    // The Warp close-handler will route it into warp_source.
                    {
                        auto warp_role = attr(tag, "data-curvz-warp-role");
                        if (!warp_role.empty() && warp_role == "source") {
                            SceneNode *par = stack.empty() ? nullptr : stack.back().node;
                            if (par && par->is_warp())
                                warp_role_pending[compound.get()] = 's';
                        }
                    }
                    push_into_parent(std::move(compound));
                    continue;
                }

                // ── Regular path ───────────────────────────────────────────
                // Check for multiple subpaths — if found, create a Compound.
                // Use parse_path_d_multi which parses the whole d string in
                // one pass, preserving correct relative-command context across
                // subpath boundaries (avoids the 'm after Z' offset bug).
                auto types_val = attr(tag, "data-curvz-types");
                auto subpaths  = (types_val.empty())
                                 ? parse_path_d_multi(d_val)
                                 : std::vector<PathData>{};

                if (subpaths.size() > 1) {
                    // External SVG with multiple subpaths — import as Compound
                    auto compound = std::make_unique<SceneNode>();
                    compound->type = SceneNode::Type::Compound;
                    compound->id   = attr(tag, "id");
                    if (compound->id.empty())
                        compound->id = "compound" + std::to_string(obj_counter++);
                    compound->name = attr(tag, "data-curvz-name");
                    if (compound->name.empty()) compound->name = compound->id;
                    if (attr(tag, "display") == "none") compound->visible = false;

                    apply_style_attrs(*compound, tag);
                    LOG_INFO("SvgParser: compound fill type={} r={:.2f} g={:.2f} b={:.2f}",
                             (int)compound->fill.type, compound->fill.r, compound->fill.g, compound->fill.b);

                    for (size_t si = 0; si < subpaths.size(); ++si) {
                        auto child = std::make_unique<SceneNode>();
                        child->type   = SceneNode::Type::Path;
                        child->id     = compound->id + "_" + std::to_string(si);
                        child->fill   = compound->fill;
                        child->stroke = compound->stroke;
                        child->fill_swatch_id   = compound->fill_swatch_id;
                        child->stroke_swatch_id = compound->stroke_swatch_id;
                        child->path   = std::make_unique<PathData>(std::move(subpaths[si]));
                        LOG_INFO("SvgParser: compound subpath {} nodes={} closed={}",
                                 si, child->path->nodes.size(), child->path->closed);
                        compound->children.push_back(std::move(child));
                    }

                    LOG_INFO("SvgParser: imported multi-subpath path '{}' as Compound ({} subpaths)",
                             compound->id, compound->children.size());
                    { auto xfm = parse_transform(attr(tag, "transform"));
                      apply_transform_to_node(*compound, xfm); }
                    push_into_parent(std::move(compound));

                } else {
                    // Single subpath or Curvz-originated path with types attr
                    GlyphObject obj;
                    obj.type = GlyphObject::Type::Path;
                    obj.id   = attr(tag, "id");
                    if (obj.id.empty()) obj.id = "obj" + std::to_string(obj_counter++);
                    obj.name = attr(tag, "data-curvz-name");
                    // Use already-parsed result if available (foreign SVG single subpath),
                    // otherwise parse fresh (Curvz-originated path with types_val).
                    if (!subpaths.empty())
                        obj.path = std::make_unique<PathData>(std::move(subpaths[0]));
                    else
                        obj.path = std::make_unique<PathData>(parse_path_d(d_val));

                    if (!types_val.empty() && obj.path) {
                        LOG_DEBUG("SvgParser: restoring types for path id={}: \"{}\"",
                                  obj.id, types_val);
                        auto& nodes = obj.path->nodes;
                        int ni = 0;
                        for (char ch : types_val) {
                            if (ch == ' ' || ch == '\t') continue;
                            if (ni >= (int)nodes.size()) break;
                            switch (ch) {
                                case 'S': nodes[ni].type = BezierNode::Type::Symmetric; break;
                                case 'M': nodes[ni].type = BezierNode::Type::Smooth;    break;
                                case 'C': nodes[ni].type = BezierNode::Type::Cusp;      break;
                                case 'K': nodes[ni].type = BezierNode::Type::Corner;    break;
                                default: break;
                            }
                            ++ni;
                        }
                    } else if (obj.path) {
                        LOG_DEBUG("SvgParser: no data-curvz-types for path id={}, using heuristic types",
                                  obj.id);
                    }

                    LOG_INFO("SvgParser: parsed path id={} nodes={} closed={}",
                             obj.id, obj.path->nodes.size(), obj.path->closed);
                    apply_style_attrs(obj, tag);
                    LOG_INFO("SvgParser: path fill type={} r={:.2f} g={:.2f} b={:.2f} style='{}'",
                             (int)obj.fill.type, obj.fill.r, obj.fill.g, obj.fill.b,
                             attr(tag,"style").substr(0,60));
                    { auto xfm = parse_transform(attr(tag, "transform"));
                      if (obj.path) apply_transform_to_path(*obj.path, xfm); }
                    // Capture data-curvz-blend-role if this path is a child
                    // of a Blend. Stash pointer → role so the Blend's close
                    // handler can route A/B into their dedicated slots.
                    auto blend_role = attr(tag, "data-curvz-blend-role");
                    auto warp_role  = attr(tag, "data-curvz-warp-role");
                    auto path_node = std::make_unique<SceneNode>(std::move(obj));
                    SceneNode* raw = path_node.get();
                    // Margins ride on the boundary path of a TextBox per
                    // stage 3d ownership. The four attrs are emitted
                    // conditionally (only when non-zero); reads are also
                    // conditional so plain Paths stay at zero. This is
                    // safe to do unconditionally — plain paths just won't
                    // have the attrs.
                    {
                        auto mt = attr(tag, "data-curvz-margin-top");
                        auto mb = attr(tag, "data-curvz-margin-bottom");
                        auto ml = attr(tag, "data-curvz-margin-left");
                        auto mr = attr(tag, "data-curvz-margin-right");
                        if (!mt.empty()) raw->text_margin_top    = dbl(mt);
                        if (!mb.empty()) raw->text_margin_bottom = dbl(mb);
                        if (!ml.empty()) raw->text_margin_left   = dbl(ml);
                        if (!mr.empty()) raw->text_margin_right  = dbl(mr);
                    }
                    // Baseline-marker flag — the writer emits these as
                    // transparent lines that hold the textPath bindings
                    // for external SVG viewers. They are SVG-output-only;
                    // Curvz regenerates baselines via compute_text_layout
                    // on every redraw. The TextBox close-handler reads
                    // baseline_markers and discards any matching child,
                    // leaving only the real boundary as the structural
                    // child.
                    if (attr(tag, "data-curvz-baseline") == "1") {
                        baseline_markers.insert(raw);
                    }
                    if (!blend_role.empty()) {
                        SceneNode *par = stack.empty() ? nullptr : stack.back().node;
                        if (par && par->is_blend()) {
                            char r = (blend_role == "a") ? 'a'
                                   : (blend_role == "b") ? 'b' : '\0';
                            if (r) blend_role_pending[raw] = r;
                        }
                    }
                    // Parallel capture for Warp source when the parent is a
                    // Warp wrapper. role "source" → 's' in the role map.
                    if (!warp_role.empty()) {
                        SceneNode *par = stack.empty() ? nullptr : stack.back().node;
                        if (par && par->is_warp() && warp_role == "source") {
                            warp_role_pending[raw] = 's';
                        }
                    }
                    push_into_parent(std::move(path_node));
                }
            }
            continue;
        }
        // s203 m4 — close of per-element try. WARN-and-continue is the
        // right posture: a single bad element shouldn't take out the rest
        // of an otherwise-valid SVG. The tag prefix in the log gives Scott
        // a starting point if a file consistently misparses an element.
        } catch (const std::exception& e) {
            LOG_WARN("SvgParser: element threw std::exception what='{}' tag-prefix='{}'",
                     e.what(), tag.substr(0, std::min((size_t)60, tag.size())));
        } catch (...) {
            LOG_WARN("SvgParser: element threw unknown exception tag-prefix='{}'",
                     tag.substr(0, std::min((size_t)60, tag.size())));
        }
    }

    // Ensure at least one layer
    if (doc->layers.empty()) {
        auto l = std::make_unique<SceneNode>();
        l->type = SceneNode::Type::Layer;
        l->name = "Layer 1";
        doc->layers.push_back(std::move(l));
    }

    // S58k: Backfill Compound fill/stroke from first child when the Compound
    // itself holds default paint. Fixes SVGs saved before S58g where old
    // Compounds rendered by reading children's fills — the Compound's own
    // fill was never written to disk and comes back as the default
    // CurrentColor. Without this heal, such files render as CurrentColor
    // checkerboards under the new Compound-owns-paint renderer (S58g).
    //
    // Heuristic: if Compound.fill is CurrentColor AND first child has a
    // Solid fill, copy child → Compound. Same for stroke. Explicit
    // CurrentColor on the Compound's SVG tag would come through
    // apply_style_attrs intact, but the default-initial path could not —
    // so this check only fires when we have good reason to believe the
    // Compound's fill was never deliberately set.
    {
        std::function<void(SceneNode*)> heal = [&](SceneNode* n) {
            if (!n) return;
            if (n->type == SceneNode::Type::Compound && !n->children.empty()) {
                const SceneNode* first = n->children.front().get();
                if (first && first->type == SceneNode::Type::Path) {
                    if (n->fill.type == FillStyle::Type::CurrentColor &&
                        first->fill.type == FillStyle::Type::Solid) {
                        n->fill = first->fill;
                        // Paint lifted: binding follows. An empty id on the
                        // child is a no-op here (compound stays unbound).
                        n->fill_swatch_id = first->fill_swatch_id;
                    }
                    if (n->stroke.paint.type == FillStyle::Type::CurrentColor &&
                        first->stroke.paint.type == FillStyle::Type::Solid) {
                        n->stroke = first->stroke;
                        n->stroke_swatch_id = first->stroke_swatch_id;
                    }
                }
            }
            for (auto &c : n->children) heal(c.get());
        };
        for (auto &L : doc->layers) heal(L.get());
    }

    LOG_INFO("SvgParser: parsed {}x{}, {} layers",
              doc->canvas_width(), doc->canvas_height(), doc->layers.size());
    for (size_t li = 0; li < doc->layers.size(); ++li)
        LOG_INFO("  layer[{}] '{}' children={}", li, 
                 doc->layers[li]->name, doc->layers[li]->children.size());

    // SVG z-order: last element = on top. Curvz draws layers[0] on top.
    // So foreign SVG layers arrive in reverse order — fix by reversing.
    // But Curvz-originated SVGs already have correct order (SvgWriter writes
    // layers[0] first = bottom, matching Canvas draw order). Detect by checking
    // for data-curvz-layer attribute presence.
    {
        bool is_curvz_svg = false;
        for (const auto& l : doc->layers)
            if (!l->color.empty() || l->is_guide_layer() || l->is_ref_layer() ||
                l->is_measure_layer()) { is_curvz_svg = true; break; }
        // Also detect by checking if any layer has a data-curvz-* name pattern
        // The real test: Curvz SVGs always have a GuideLayer already; foreign don't
        // Actually simplest: check if we have exactly the special layers a Curvz file has
        // Use a flag set during parsing instead — check if any layer had data-curvz-layer attr

        // Simpler heuristic: if any layer is GuideLayer/RefLayer/MeasureLayer, it's a Curvz file
        for (const auto& l : doc->layers) {
            if (l->is_guide_layer() || l->is_ref_layer() || l->is_measure_layer()) {
                is_curvz_svg = true; break;
            }
        }

        if (!is_curvz_svg) {
            // Foreign SVG — reverse layer order so SVG z-order matches Curvz draw order
            std::reverse(doc->layers.begin(), doc->layers.end());
            LOG_INFO("SvgParser: foreign SVG — reversed layer order for correct z-order");
        }
    }

    // Ensure every document has exactly one GuideLayer, even if loading an
    // older file that predates guide support.
    doc->ensure_guide_layer();

    // ── s311 m1c-redux — dual-block Mgr finalization ─────────────────
    // Two cleanups happen here, after the main parse but before the
    // assign_iids / dedup_names / find_active_layer passes that
    // expect the layer tree to be in its final shape:
    //
    //   1. Any Mgr still in pending_mgrs is orphaned — its <g data-
    //      curvz-textbox-mgr> hydrated from <defs> but no TextBoxView
    //      in the visible tree referenced it via mgr-iid. Splice the
    //      Mgr into the first regular Layer so it round-trips on save.
    //      (Mgrs with no canvas views still need to exist — they can
    //      hold buffer state that the popover view will surface once
    //      the user re-engages the textbox.)
    //
    //   2. For every Mgr in mgr_def_by_iid (placed or orphaned), if
    //      no Popover-kind TextBoxView is among its children, synthesize
    //      one. Same shape the legacy nested-format close-arm uses.
    //      The popover view is a structural fixture — every Mgr always
    //      has exactly one, regardless of how many canvas views there
    //      are. Its boundary geometry comes from the widget allocation
    //      at runtime; it has no Path child on disk or in memory.
    {
        // Step 1: place orphaned Mgrs in the first regular Layer.
        SceneNode* first_layer = nullptr;
        for (auto& l : doc->layers) {
            if (l && l->is_layer()) { first_layer = l.get(); break; }
        }
        if (!pending_mgrs.empty()) {
            if (first_layer) {
                for (auto& mgr_uptr : pending_mgrs) {
                    if (!mgr_uptr) continue;
                    LOG_INFO("SvgParser: TextBoxMgr '{}' was orphaned "
                             "(no view referenced it) — placing in "
                             "first layer '{}'",
                             mgr_uptr->name, first_layer->name);
                    first_layer->children.push_back(std::move(mgr_uptr));
                }
            } else {
                LOG_WARN("SvgParser: {} orphaned TextBoxMgr(s) and no "
                         "regular layer to place them in — dropping",
                         pending_mgrs.size());
            }
            pending_mgrs.clear();
        }
        // Step 2: synthesize Popover view per Mgr if absent.
        for (auto& kv : mgr_def_by_iid) {
            SceneNode* mgr = kv.second;
            if (!mgr) continue;
            bool has_popover = false;
            int  canvas_count = 0;
            for (auto& c : mgr->children) {
                if (!c || !c->is_text_box_view()) continue;
                if (c->view_kind == SceneNode::ViewKind::Popover)
                    has_popover = true;
                else
                    ++canvas_count;
            }
            if (!has_popover) {
                auto popover = std::make_unique<SceneNode>();
                popover->type = SceneNode::Type::TextBoxView;
                popover->internal_id = generate_internal_id();
                popover->view_kind = SceneNode::ViewKind::Popover;
                popover->view_byte_start = 0;
                popover->view_bytes_consumed = 0;
                mgr->children.push_back(std::move(popover));
            }
            LOG_INFO("SvgParser: TextBoxMgr '{}' finalized — "
                     "content_len={} canvas_views={} popover={}",
                     mgr->name, mgr->text_content.size(),
                     canvas_count, has_popover ? "kept" : "synthesized");
        }
    }

    // active_layer_index must point at a normal Layer, not the GuideLayer.
    // Find the first non-guide layer.
    doc->active_layer_index = 0;
    for (int i = 0; i < (int)doc->layers.size(); ++i) {
        if (doc->layers[i]->is_layer()) { doc->active_layer_index = i; break; }
    }

    // Assign internal_ids to any node that doesn't have one.
    // This covers both newly created nodes and nodes loaded from older files
    // that predate the internal_id system.
    //
    // S99: instrumented for the iid-regen hunt. Each fresh mint here means
    // either (a) a legitimately new node (new doc, just-imported foreign
    // SVG), or (b) a writer/parser asymmetry — the writer didn't emit an
    // iid for this node type, or the parser didn't read it. The latter is
    // a bug. Watch the logs after a save→load cycle: any minting on a
    // round-tripped doc points at the next asymmetry to fix. Logged at
    // WARN so it's visible without enabling debug.
    std::function<void(SceneNode*)> assign_iids = [&](SceneNode* n) {
        if (n->internal_id.empty()) {
            n->internal_id = generate_internal_id();
            LOG_WARN("SvgParser: minted fresh iid '{}' for node type={} name='{}' id='{}'",
                     n->internal_id, (int)n->type, n->name, n->id);
        }
        for (auto& ch : n->children)
            assign_iids(ch.get());
    };
    for (auto& layer : doc->layers)
        assign_iids(layer.get());

    // Build SVG-id → internal_id map for text_path_id migration.
    // Text nodes loaded from older files store the SVG id in text_path_id.
    // Now that all nodes have internal_ids, migrate to use internal_id.
    std::unordered_map<std::string, std::string> svgid_to_iid;
    std::function<void(SceneNode*)> collect_ids = [&](SceneNode* n) {
        if (!n->id.empty() && !n->internal_id.empty())
            svgid_to_iid[n->id] = n->internal_id;
        for (auto& ch : n->children)
            collect_ids(ch.get());
    };
    for (auto& layer : doc->layers)
        collect_ids(layer.get());

    // Migrate text_path_id: if it looks like an SVG id (not a UUID), remap it.
    // Also clear stale UUID references that no longer match any path's internal_id.
    auto is_uuid = [](const std::string& s) {
        // UUID format: 8-4-4-4-12 hex chars with dashes
        return s.size() == 36 && s[8] == '-' && s[13] == '-' &&
               s[18] == '-' && s[23] == '-';
    };

    // Build set of all known internal_ids so we can detect orphaned UUID refs.
    std::unordered_set<std::string> known_iids;
    std::function<void(SceneNode*)> collect_iids = [&](SceneNode* n) {
        if (!n->internal_id.empty()) known_iids.insert(n->internal_id);
        for (auto& ch : n->children) collect_iids(ch.get());
    };
    for (auto& layer : doc->layers) collect_iids(layer.get());

    std::function<void(SceneNode*)> migrate_path_ids = [&](SceneNode* n) {
        if (n->is_text() && !n->text_path_id.empty()) {
            if (!is_uuid(n->text_path_id)) {
                // Old format: SVG id string — remap to current internal_id.
                auto it = svgid_to_iid.find(n->text_path_id);
                if (it != svgid_to_iid.end()) {
                    LOG_DEBUG("SvgParser: migrated text_path_id '{}' → '{}'",
                              n->text_path_id, it->second);
                    n->text_path_id = it->second;
                }
            } else if (known_iids.find(n->text_path_id) == known_iids.end()) {
                // UUID format but no matching path — stale reference from a
                // previous session before data-curvz-iid was stable.
                // Try to rescue: find a path whose SVG id maps to some iid,
                // and if there is exactly one path in the doc, re-link to it.
                // Otherwise clear the link so the user sees honest state.
                std::string rescue_iid;
                int path_count = 0;
                std::function<void(SceneNode*)> find_paths = [&](SceneNode* nd) {
                    if (nd->is_path() && nd->path) {
                        ++path_count;
                        rescue_iid = nd->internal_id;
                    }
                    for (auto& ch : nd->children) find_paths(ch.get());
                };
                for (auto& layer : doc->layers) find_paths(layer.get());

                if (path_count == 1 && !rescue_iid.empty()) {
                    LOG_INFO("SvgParser: rescued stale text_path_id '{}' → '{}' (only path in doc)",
                             n->text_path_id, rescue_iid);
                    n->text_path_id = rescue_iid;
                } else {
                    LOG_WARN("SvgParser: cleared stale text_path_id '{}' — no matching path found (paths={})",
                             n->text_path_id, path_count);
                    n->text_path_id.clear();
                    n->text_path_offset = 0.0;
                    n->text_path_flip   = false;
                }
            }
        }
        // s301 m1a — migrate the unified container model refs the same way.
        // SVG-id format (rare, only relevant for hand-edited files) gets
        // remapped via svgid_to_iid; stale UUID format gets cleared
        // (no auto-rescue: a multi-boundary chain has no "only path in doc"
        // analog to rescue against). The text_path_id rescue is kept for
        // backward compatibility with old files; the new fields don't need
        // it because they're brand-new and no pre-s301 file can carry them.
        if (n->is_text()) {
            for (auto it = n->text_boundary_ids.begin();
                 it != n->text_boundary_ids.end(); /* advance below */) {
                if (it->empty()) { it = n->text_boundary_ids.erase(it); continue; }
                if (!is_uuid(*it)) {
                    auto m = svgid_to_iid.find(*it);
                    if (m != svgid_to_iid.end()) {
                        LOG_DEBUG("SvgParser: migrated text_boundary_id '{}' → '{}'",
                                  *it, m->second);
                        *it = m->second;
                        ++it;
                    } else {
                        LOG_WARN("SvgParser: dropped unresolved text_boundary_id '{}'", *it);
                        it = n->text_boundary_ids.erase(it);
                    }
                } else if (known_iids.find(*it) == known_iids.end()) {
                    LOG_WARN("SvgParser: dropped stale text_boundary_id '{}'", *it);
                    it = n->text_boundary_ids.erase(it);
                } else {
                    ++it;
                }
            }
            if (!n->text_line_path_id.empty()) {
                if (!is_uuid(n->text_line_path_id)) {
                    auto m = svgid_to_iid.find(n->text_line_path_id);
                    if (m != svgid_to_iid.end()) {
                        LOG_DEBUG("SvgParser: migrated text_line_path_id '{}' → '{}'",
                                  n->text_line_path_id, m->second);
                        n->text_line_path_id = m->second;
                    } else {
                        LOG_WARN("SvgParser: cleared unresolved text_line_path_id '{}'",
                                 n->text_line_path_id);
                        n->text_line_path_id.clear();
                    }
                } else if (known_iids.find(n->text_line_path_id) == known_iids.end()) {
                    LOG_WARN("SvgParser: cleared stale text_line_path_id '{}'",
                             n->text_line_path_id);
                    n->text_line_path_id.clear();
                }
            }
        }
        for (auto& ch : n->children)
            migrate_path_ids(ch.get());
    };
    for (auto& layer : doc->layers)
        migrate_path_ids(layer.get());

    // S90 Stage 2 — clear active gradient resolver. Paired with the
    // activation near the top of parse_svg. See file-scope pointer note.
    g_gradient_defs = nullptr;

    // S95 m1 — Name uniqueness pass.
    //
    // Older Curvz writes (and any hand-edited SVGs) can produce nodes
    // that share `name` values — most commonly multiple layers all
    // called "Layer 1", or multiple paths defaulted to "obj1". Because
    // SvgWriter emits node names into SVG `id` attributes (Layer ids =
    // layer.name), name collisions become id collisions on the next
    // save, and that's how compounds end up with broken
    // data-curvz-child-ids references. dedup_names walks the document
    // tree and renames collisions in-place; first occurrence keeps the
    // original name, subsequent ones get suffixed " (2)", " (3)", etc.
    if (doc) {
        int renames = doc->dedup_names();
        if (renames > 0) {
            LOG_INFO("SvgParser: dedup_names renamed {} colliding node "
                     "name{} on load", renames, renames == 1 ? "" : "s");
        }
    }

    return doc;
}

std::unique_ptr<CurvzDocument> parse_svg_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        LOG_ERROR("SvgParser: cannot open '{}'", path);
        return nullptr;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    // s203 m4 — Outer catastrophic guard. The per-element try/catch inside
    // parse_svg() catches anything thrown from a single tag's handler. This
    // outer block is the last line of defense: if something escapes the
    // inner guard (a top-of-function allocation throws, a std::bad_alloc
    // on a huge SVG, an iterator invalidation we didn't anticipate), we
    // bail with both a log line and a stderr line. The stderr line is
    // there because LOG_ERROR goes to the log file only; if curvz is
    // launched from a terminal and crashes-but-doesn't-die-quietly on a
    // file the user just opened, they want to see SOMETHING in the
    // terminal. Caller (typically Open / drag-drop / icon-scan) sees a
    // null doc and handles gracefully.
    //
    // Does NOT catch signals (SIGSEGV from null-deref etc.) — see the
    // per-element try block comment in parse_svg() for why.
    std::unique_ptr<CurvzDocument> doc;
    try {
        doc = parse_svg(ss.str());
    } catch (const std::exception& e) {
        LOG_ERROR("SvgParser: catastrophic parse failure on '{}' — std::exception "
                  "what='{}'", path, e.what());
        std::fprintf(stderr,
                     "SvgParser: catastrophic parse failure on '%s' — %s\n",
                     path.c_str(), e.what());
        return nullptr;
    } catch (...) {
        LOG_ERROR("SvgParser: catastrophic parse failure on '{}' — unknown "
                  "exception", path);
        std::fprintf(stderr,
                     "SvgParser: catastrophic parse failure on '%s' — unknown "
                     "exception type\n", path.c_str());
        return nullptr;
    }
    if (doc) doc->filename = path;
    return doc;
}

} // namespace Curvz
