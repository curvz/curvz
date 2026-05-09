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

}  // namespace

std::string serialize(const ExportData& data, Format f) {
    switch (f) {
        case Format::Csv:
            return serialize_csv(data);
        case Format::Json:
        case Format::Gcode:
        case Format::Dxf:
            // m2 territory — log and return empty. The dialog won't
            // present these as choices in m1, so this branch is
            // defensive only.
            LOG_WARN("RefptExporter::serialize: format {} not yet "
                     "implemented (m2 ship)", static_cast<int>(f));
            return std::string();
    }
    return std::string();
}

std::string export_refpts(const CurvzDocument& doc, Format f) {
    return serialize(collect_translated(doc), f);
}

}  // namespace RefptExporter
}  // namespace Curvz
