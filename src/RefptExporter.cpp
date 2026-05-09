#include "RefptExporter.hpp"

#include "CurvzDocument.hpp"
#include "CurvzLog.hpp"
#include "DocUnits.hpp"
#include "SceneNode.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace Curvz {
namespace RefptExporter {

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers — local, file-scope. All format-agnostic; they prepare the
//  shared (name, x, y) triples that every format writer consumes.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Walk every Ref node under visible RefLayer ancestors. Hidden layers
// and hidden refpts are skipped. The walk is shallow on purpose:
// refpts live as direct children of RefLayer in Curvz's data model;
// we don't recurse into Path children or other non-RefLayer nodes.
//
// `out` is appended to; the visitor doesn't reset it.
void collect_visible_refs(const SceneNode* layer,
                          std::vector<const SceneNode*>& out)
{
    if (!layer || !layer->visible) return;
    if (!layer->is_ref_layer())   return;
    for (const auto& child : layer->children) {
        if (!child) continue;
        if (!child->is_ref()) continue;
        if (!child->visible) continue;
        out.push_back(child.get());
    }
}

// Build an ISO-8601 UTC timestamp string ("2026-05-09T15:32:00Z").
// Used in the metadata header so a downstream consumer can tell when
// the file was generated, without timezone ambiguity.
std::string iso_timestamp_utc() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

// Format a double to exactly 6 decimal places. Per the s170-banked
// SvgWriter convention plus the s176 design conversation: 6 dp is
// the precision floor for refpt exports unless the format specifies
// otherwise. Uses snprintf with "%.6f" — portable, no dependency on
// fmt/spdlog header layout (the codebase already mixes system fmt
// and bundled-spdlog setups).
std::string fmt6(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return std::string(buf);
}

// Resolve display_unit to the human-readable label written into the
// metadata header. Mirrors UnitSystem::label but returns std::string
// for header-building convenience.
std::string units_label_for(Unit u) {
    return std::string(UnitSystem::label(u));
}

// Pick a stable display name for a refpt. Curvz's "non-empty name"
// rule (banked memory: swatches/styles/objects always have a name)
// applies — but defend against legacy files that lack names by
// falling back to "Ref". Internal-id is never user-facing per the
// "UUIDs must never appear in user-facing UI" rule.
std::string display_name(const SceneNode& ref) {
    if (!ref.name.empty()) return ref.name;
    return "Ref";
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────

const char* extension(Format f) {
    switch (f) {
        case Format::Csv:   return "csv";
        case Format::Json:  return "json";
        case Format::Gcode: return "nc";
        case Format::Dxf:   return "dxf";
    }
    return "txt";  // unreachable; defensive
}

std::size_t count_exportable(const CurvzDocument& doc) {
    std::size_t n = 0;
    for (const auto& layer : doc.layers) {
        std::vector<const SceneNode*> tmp;
        collect_visible_refs(layer.get(), tmp);
        n += tmp.size();
    }
    return n;
}

DocSummary summarize(const CurvzDocument& doc) {
    DocSummary s;
    // Count: same walk as count_exportable. Cheap enough we don't bother
    // sharing the loop — the function gets called once per doc per UI
    // refresh, and a typical project has under a dozen docs.
    s.refpt_count = count_exportable(doc);
    return s;
}

ExportData collect_translated(const CurvzDocument& doc) {
    ExportData out;
    out.document_name = doc.filename.empty() ? "(unnamed)" : doc.filename;
    out.units_label   = units_label_for(doc.canvas.display_unit);
    out.iso_timestamp = iso_timestamp_utc();

    // Gather every visible refpt. Doc-space, Y-down, raw pixels — that's
    // the storage convention; we transform on the way out.
    std::vector<const SceneNode*> refs;
    for (const auto& layer : doc.layers)
        collect_visible_refs(layer.get(), refs);

    // Coordinate transform: route every refpt through DocUnits, the
    // canonical doc->display pump that the ruler and inspector also
    // use. The pump:
    //   - subtracts the ruler origin (the user's chosen (0,0))
    //   - applies CoordSpace's Y-flip (Y-up: above-origin is positive,
    //     below-origin is negative)
    //   - converts to the doc's display unit, honouring all three
    //     DisplayMode branches (Pixel / Physical / RatioQuality)
    //
    // s177: pre-fix this loop did its own incomplete conversion that
    // ignored the ruler origin and assumed Pixel display mode. Routing
    // through the shared pump fixes both bugs at once and keeps the
    // export coordinate system identical to what the user sees on the
    // ruler.
    const CanvasModel* cm = &doc.canvas;
    const double ruler_ox = doc.ruler_origin_x;
    const double ruler_oy = doc.ruler_origin_y;

    out.refpts.reserve(refs.size());
    for (const SceneNode* r : refs) {
        Refpt out_pt;
        out_pt.name = display_name(*r);
        out_pt.x = DocUnits::doc_to_display_x(r->ref_x, cm, ruler_ox);
        out_pt.y = DocUnits::doc_to_display_y(r->ref_y, cm, ruler_oy);
        out.refpts.push_back(out_pt);
    }

    LOG_DEBUG("RefptExporter::collect_translated doc='{}' units='{}' "
              "ruler_origin=({:.3f},{:.3f}) refpts={}",
              out.document_name, out.units_label,
              ruler_ox, ruler_oy, out.refpts.size());
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CSV serializer (m1 ship)
//
//  Header is a block of comment lines (`# key: value`). Body is a
//  single header row followed by one row per refpt. Column order is
//  fixed: name, x, y. Names are emitted unquoted; the field separator
//  is comma. Per the s176 design conversation: name uniqueness is the
//  document's responsibility (Curvz's name funnel guarantees no two
//  nodes in a doc share a name), so a refpt name with an embedded
//  comma would be a structural anomaly — but defend against it by
//  quoting the name field if it contains comma, quote, or newline.
//  This is the standard CSV escaping rule (RFC 4180-flavoured).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

bool csv_needs_quoting(const std::string& s) {
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') return true;
    }
    return false;
}

std::string csv_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');  // double-up the quote
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string csv_field(const std::string& s) {
    return csv_needs_quoting(s) ? csv_quote(s) : s;
}

std::string serialize_csv(const ExportData& d) {
    std::ostringstream os;
    // Metadata header — comment lines.
    os << "# format: curvz.refpts.v1\n";
    os << "# document: " << d.document_name << "\n";
    os << "# units: "    << d.units_label   << "\n";
    os << "# origin: ruler\n";
    os << "# y_axis: up\n";
    os << "# precision: 6\n";
    os << "# generated: " << d.iso_timestamp << "\n";
    // Body — header row + data rows.
    os << "name,x,y\n";
    for (const auto& p : d.refpts) {
        os << csv_field(p.name) << ","
           << fmt6(p.x) << ","
           << fmt6(p.y) << "\n";
    }
    return os.str();
}

// ─────────────────────────────────────────────────────────────────────────
//  JSON serializer (s178 m2)
//
//  Schema id "curvz.refpts.v1" — same as CSV. Frozen format. The metadata
//  fields live as top-level keys; refpts is an array of {name, x, y}
//  objects in document order.
//
//  Strings are escaped per RFC 8259 (json.org). Numbers are emitted with
//  6 decimal places, matching CSV's precision floor; downstream consumers
//  that want fewer can round, ones that want more can't get them, which
//  is the right default — refpt positions are sub-pixel-precise but only
//  meaningful to surveying-class precision.
//
//  Hand-rolled (no JSON lib dependency) — the schema is small and stable,
//  and pulling nlohmann/json into a writer that emits ~10 keys is overkill.
// ─────────────────────────────────────────────────────────────────────────

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string serialize_json(const ExportData& d) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"format\": \"curvz.refpts.v1\",\n";
    os << "  \"document\": \"" << json_escape(d.document_name) << "\",\n";
    os << "  \"units\": \""    << json_escape(d.units_label)   << "\",\n";
    os << "  \"origin\": \"ruler\",\n";
    os << "  \"y_axis\": \"up\",\n";
    os << "  \"precision\": 6,\n";
    os << "  \"generated\": \"" << json_escape(d.iso_timestamp) << "\",\n";
    os << "  \"refpts\": [";
    for (std::size_t i = 0; i < d.refpts.size(); ++i) {
        const auto& p = d.refpts[i];
        os << (i == 0 ? "\n" : ",\n");
        os << "    {\"name\": \"" << json_escape(p.name) << "\""
           << ", \"x\": " << fmt6(p.x)
           << ", \"y\": " << fmt6(p.y)
           << "}";
    }
    if (!d.refpts.empty()) os << "\n  ";
    os << "]\n";
    os << "}\n";
    return os.str();
}

// ─────────────────────────────────────────────────────────────────────────
//  G-code serializer (s178 m2)
//
//  Plotter-style: declare units (G20 inches / G21 mm / fallback comment
//  for px/pt), absolute coordinates (G90), then one G00 (rapid move) per
//  refpt. Refpt names ride in trailing inline comments so an operator
//  reading the file can correlate machine moves to source labels.
//
//  Refpt names that contain a parenthesis would break the G-code comment
//  syntax. We strip parens defensively — the canonical naming funnel in
//  the codebase doesn't produce parens, but legacy files might.
//
//  Coordinate-system note: G-code's Y axis is conventionally Y-up for
//  most plotters/mills, which matches our exporter's Y-up output. No
//  axis flip needed at this layer.
// ─────────────────────────────────────────────────────────────────────────

std::string gcode_strip_parens(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '(' || c == ')') continue;
        out.push_back(c);
    }
    return out;
}

// Map units_label to the right G-code units directive. mm → G21,
// inches → G20. For px/pt (no native G-code analogue) we omit the
// directive and document the unit in the header comments only — the
// downstream consumer can apply a scale factor if needed.
const char* gcode_units_directive(const std::string& units_label) {
    if (units_label == "mm") return "G21";
    if (units_label == "in") return "G20";
    return nullptr;  // px / pt / unknown
}

std::string serialize_gcode(const ExportData& d) {
    std::ostringstream os;
    // Metadata header — G-code comment lines (semicolon style; widely
    // accepted by GRBL, LinuxCNC, Marlin).
    os << "; format: curvz.refpts.v1\n";
    os << "; document: " << d.document_name << "\n";
    os << "; units: "    << d.units_label   << "\n";
    os << "; origin: ruler\n";
    os << "; y_axis: up\n";
    os << "; precision: 6\n";
    os << "; generated: " << d.iso_timestamp << "\n";
    // Setup block.
    if (const char* gu = gcode_units_directive(d.units_label)) {
        os << gu << " ; units\n";
    } else {
        os << "; (no native G-code units directive for "
           << d.units_label << " — coordinates are raw)\n";
    }
    os << "G90 ; absolute coordinates\n";
    // Body — rapid move per refpt with inline name comment.
    for (const auto& p : d.refpts) {
        os << "G00 X" << fmt6(p.x) << " Y" << fmt6(p.y);
        if (!p.name.empty())
            os << " (" << gcode_strip_parens(p.name) << ")";
        os << "\n";
    }
    return os.str();
}

// ─────────────────────────────────────────────────────────────────────────
//  DXF serializer (s178 m2)
//
//  Minimal HEADER + ENTITIES sections — small enough to read, robust
//  enough to load in mainstream CAD (AutoCAD, QCAD, LibreCAD, Inkscape's
//  DXF importer). HEADER documents $INSUNITS so the consumer knows the
//  unit; ENTITIES contains one POINT per refpt.
//
//  DXF format note: DXF is a tagged-line format. Each "entry" is two
//  lines — a group code (integer) on one line, a value on the next. The
//  code-value pairs nest into sections delimited by 0/SECTION ... 0/
//  ENDSEC. Lines must be CRLF on Windows and LF elsewhere; LF works
//  on every modern DXF reader I'm aware of, so we emit LF only.
//
//  $INSUNITS values (subset relevant to Curvz):
//    0 = unitless
//    1 = inches
//    4 = millimeters
//
//  Pixels and points have no $INSUNITS code — we map them to 0
//  (unitless) and let the metadata comment in the trailing TEXT entity
//  document the actual unit. (POINT entities don't accept comments
//  inline; the standard idiom is to embed informational TEXT or
//  attach XDATA. We use a single TEXT at (0,0) with the metadata
//  block as its content, off-canvas if needed.)
//
//  Refpt names ride along as POINT XDATA — but XDATA requires an
//  application-id registered in the APPID table, which means we'd
//  need full TABLES support to do it cleanly. Instead, we add one
//  TEXT entity per refpt, positioned at the refpt, holding the name.
//  CAD apps render this as a label; consumers that only want the
//  geometry can filter by entity type (POINT-only).
// ─────────────────────────────────────────────────────────────────────────

int dxf_insunits_for(const std::string& units_label) {
    if (units_label == "in") return 1;
    if (units_label == "mm") return 4;
    return 0;  // px / pt / unknown — unitless
}

std::string serialize_dxf(const ExportData& d) {
    std::ostringstream os;
    // ── HEADER section ─────────────────────────────────────────────────
    os << "  0\nSECTION\n";
    os << "  2\nHEADER\n";
    os << "  9\n$INSUNITS\n";
    os << " 70\n" << dxf_insunits_for(d.units_label) << "\n";
    os << "  0\nENDSEC\n";

    // ── ENTITIES section ───────────────────────────────────────────────
    os << "  0\nSECTION\n";
    os << "  2\nENTITIES\n";

    // Self-describing metadata block — one TEXT entity at the ruler
    // origin (0, 0) holding the format header. Layer "REFPTS_META" so
    // the consumer can hide it if undesired.
    {
        std::ostringstream meta;
        meta << "format=curvz.refpts.v1; "
             << "document=" << d.document_name << "; "
             << "units="    << d.units_label   << "; "
             << "origin=ruler; y_axis=up; precision=6; "
             << "generated=" << d.iso_timestamp;
        os << "  0\nTEXT\n";
        os << "  8\nREFPTS_META\n";    // layer name
        os << " 10\n0.0\n";              // x
        os << " 20\n0.0\n";              // y
        os << " 30\n0.0\n";              // z
        os << " 40\n0.1\n";              // text height (small)
        os << "  1\n" << meta.str() << "\n";
    }

    // Each refpt: a POINT for geometry plus a TEXT for the label,
    // both on layer "REFPTS". Consumers wanting geometry-only can
    // filter the layer to POINT entities.
    for (const auto& p : d.refpts) {
        os << "  0\nPOINT\n";
        os << "  8\nREFPTS\n";
        os << " 10\n" << fmt6(p.x) << "\n";
        os << " 20\n" << fmt6(p.y) << "\n";
        os << " 30\n0.0\n";
        if (!p.name.empty()) {
            os << "  0\nTEXT\n";
            os << "  8\nREFPTS\n";
            os << " 10\n" << fmt6(p.x) << "\n";
            os << " 20\n" << fmt6(p.y) << "\n";
            os << " 30\n0.0\n";
            os << " 40\n0.2\n";          // text height
            os << "  1\n" << p.name << "\n";
        }
    }

    os << "  0\nENDSEC\n";
    os << "  0\nEOF\n";
    return os.str();
}

}  // namespace

std::string serialize(const ExportData& data, Format f) {
    switch (f) {
        case Format::Csv:   return serialize_csv(data);
        case Format::Json:  return serialize_json(data);
        case Format::Gcode: return serialize_gcode(data);
        case Format::Dxf:   return serialize_dxf(data);
    }
    return std::string();
}

std::string export_refpts(const CurvzDocument& doc, Format f) {
    return serialize(collect_translated(doc), f);
}

}  // namespace RefptExporter
}  // namespace Curvz
