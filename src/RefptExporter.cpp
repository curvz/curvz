#include "RefptExporter.hpp"

#include "CoordSpace.hpp"
#include "CurvzDocument.hpp"
#include "CurvzLog.hpp"
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

// Find the refpt promoted as export origin, or nullptr if none.
// Walks the same scope as collect_visible_refs (visible RefLayer
// children only) — a refpt on a hidden layer can't be the export
// origin, since the user can't see it. If no refpt is promoted,
// the caller falls back to canvas (0, 0).
const SceneNode* find_export_origin(const CurvzDocument& doc) {
    for (const auto& layer : doc.layers) {
        if (!layer || !layer->visible) continue;
        if (!layer->is_ref_layer()) continue;
        for (const auto& child : layer->children) {
            if (!child) continue;
            if (!child->is_ref()) continue;
            if (!child->visible) continue;
            if (child->is_export_origin) return child.get();
        }
    }
    return nullptr;
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

    // Origin: same selection rule as collect_translated. The promoted
    // refpt wins; fall back to "(canvas)" sentinel.
    const SceneNode* origin = find_export_origin(doc);
    if (origin) {
        s.origin_name = display_name(*origin);
    } else {
        s.origin_name = "(canvas)";
    }
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

    // Origin selection. The promoted refpt wins if present; otherwise we
    // fall back to canvas (0, 0). In Y-down storage, canvas (0, 0) is
    // the top-left; after the Y-flip below, that becomes the bottom-left
    // corner in user/export coordinates — i.e. the artboard origin under
    // the engineering convention. A user who wants a different origin
    // promotes a refpt.
    const SceneNode* origin_node = find_export_origin(doc);
    double origin_x_doc = 0.0;
    double origin_y_doc = 0.0;
    if (origin_node) {
        out.origin_name = display_name(*origin_node);
        origin_x_doc = origin_node->ref_x;
        origin_y_doc = origin_node->ref_y;
    } else {
        out.origin_name = "(canvas)";
    }

    // Transform pipeline — origin translation → Y-flip → unit conversion.
    // CoordSpace is the canonical Y-flip; using it here keeps the
    // "(canvas_height - y) appears nowhere except CoordSpace" rule
    // intact.
    const double canvas_h = static_cast<double>(doc.canvas.canvas_height());
    CoordSpace cs(canvas_h);
    const Unit display_unit = doc.canvas.display_unit;

    out.refpts.reserve(refs.size());
    for (const SceneNode* r : refs) {
        // Step 1: translate so the chosen origin reads as (0, 0) in
        // doc space. Still Y-down at this point.
        const double dx_doc = r->ref_x - origin_x_doc;
        const double dy_doc = r->ref_y - origin_y_doc;

        // Step 2: Y-flip via CoordSpace. We're transforming a delta,
        // not an absolute position — to_user_dy negates. (X is a
        // pass-through.)
        const double user_x = dx_doc;
        const double user_y = cs.to_user_dy(dy_doc);

        // Step 3: unit conversion. UnitSystem::from_px maps raw pixels
        // to the display unit (px is identity, mm/in/pt scale).
        Refpt out_pt;
        out_pt.name = display_name(*r);
        out_pt.x    = UnitSystem::from_px(user_x, display_unit);
        out_pt.y    = UnitSystem::from_px(user_y, display_unit);
        out.refpts.push_back(out_pt);
    }

    LOG_DEBUG("RefptExporter::collect_translated doc='{}' units='{}' "
              "origin='{}' refpts={}",
              out.document_name, out.units_label, out.origin_name,
              out.refpts.size());
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
    os << "# origin: "   << d.origin_name   << "\n";
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
