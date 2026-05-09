#pragma once
//
// RefptExporter — refpt-coordinates serializer (s176 m1).
//
// Produces a serialized refpt-coordinate listing for a CurvzDocument in
// one of several formats. v1 covers CSV; m2 will add JSON, G-code, and
// DXF using the same shared transform pipeline.
//
// Pure functions. No GTK, no I/O. Caller owns the file write.
//
// Coordinate model
// ─────────────────
//
// Refpts are stored internally in Cairo/SVG native space (Y-down,
// origin at canvas top-left, raw pixel units). The exporter translates
// every refpt's (x, y) so:
//
//   1. The doc's ruler origin reads as (0, 0). The user controls
//      this origin via the corner-square on the rulers — what the
//      ruler shows is what gets exported. (Pre-s177 the export had
//      its own per-refpt promotion concept; that was wrong and was
//      removed: save/restore is sacred and export concerns don't
//      touch it.)
//
//   2. Y is flipped from Y-down to Y-up (engineering convention).
//      Up is positive, down is negative.
//
//   3. Values are converted to the document's display unit using
//      the canonical DocUnits pump — the same conversion the ruler
//      and inspector use. Honours all three DisplayMode branches
//      (Pixel, Physical, RatioQuality).
//
//   4. Numbers are formatted with 6 decimal places. The exporter
//      enforces this; callers don't pre-format.
//
// All four steps happen in the shared `collect_translated()` helper.
// Format writers are dumb-and-thin — they consume the translated
// (name, x, y) triples and wrap them in their own envelope.
//
// Metadata header
// ────────────────
//
// Every format embeds a self-describing header:
//   format     — "curvz.refpts.v1" (frozen schema id)
//   document   — doc filename (or "(unnamed)" if empty)
//   units      — "px" / "mm" / "in" / "pt" (the doc's display unit
//                 at export time)
//   origin     — "ruler" (always — the ruler origin is the export
//                 origin; coordinates are user-space relative to it)
//   y_axis     — "up" (always — declared for honesty)
//   precision  — 6
//   generated  — ISO-8601 UTC timestamp
//
// The header makes the file self-validating: a downstream consumer
// reading it knows exactly what coordinate system it's looking at,
// without out-of-band conventions.
//

#include "UnitSystem.hpp"
#include <string>
#include <vector>

namespace Curvz {

struct CurvzDocument;
struct SceneNode;

namespace RefptExporter {

// Output formats. CSV is shipped in m1; the others are placeholders
// for m2 — calling export_refpts() with one of those formats today
// returns an empty string and logs a warning.
enum class Format {
    Csv,    // m1 ship
    Json,   // m2
    Gcode,  // m2
    Dxf,    // m2
};

// File extension for a given format ("csv", "json", "nc", "dxf").
// Used by the dialog to compose the sidecar filename.
const char* extension(Format f);

// One refpt's transformed coordinates, ready for serialization.
// All values in display units, Y-up, with the chosen origin at (0, 0).
struct Refpt {
    std::string name;       // human-readable; "Ref" if the node had no name
    double      x = 0.0;    // display units, Y-up, origin-translated
    double      y = 0.0;
};

// Bundle of everything a serializer needs. Built once per export by
// collect_translated() and consumed by every format writer.
//
// s177: origin_name removed. The export coordinate system is the
// document's user space — the same space the ruler shows. Origin is
// always the ruler origin; there is no separate per-export origin
// concept at the exporter layer. Refpt-as-export-origin promotion
// remains as a runtime decoration on SceneNode for future use, but
// does not influence exported coordinates.
struct ExportData {
    std::string         document_name;   // doc.filename (or "(unnamed)")
    std::string         units_label;     // "px" / "mm" / "in" / "pt"
    std::string         iso_timestamp;   // ISO-8601 UTC, e.g. "2026-05-09T15:32:00Z"
    std::vector<Refpt>  refpts;          // ruler-origin-translated, in display units
};

// Walk the document, collect every Ref node, and apply the transform
// pipeline (origin translation → Y-flip → unit conversion).
//
// "Visible" filter: refpts on hidden layers and refpts with
// `visible == false` are excluded. This matches the exporter's
// "what you see is what you ship" intent.
ExportData collect_translated(const CurvzDocument& doc);

// Serialize a prepared ExportData into the chosen format. Returns the
// full file contents as a string. Caller writes to disk.
std::string serialize(const ExportData& data, Format f);

// Convenience: walk + serialize in one call. Equivalent to
// serialize(collect_translated(doc), f).
std::string export_refpts(const CurvzDocument& doc, Format f);

// One-shot count of exportable refpts in a document. Used by the
// dialog to decide whether to enable the refpts-export checkbox
// (don't offer to export from a doc with zero refpts).
std::size_t count_exportable(const CurvzDocument& doc);

// Lightweight summary for UI labels — what a given doc would yield
// without doing the full transform. Returns the count of visible
// refpts. s177: origin breadcrumb removed; the export coordinate
// system is the doc's user space (ruler origin), which is visible to
// the user already.
struct DocSummary {
    std::size_t refpt_count = 0;
};
DocSummary summarize(const CurvzDocument& doc);

} // namespace RefptExporter
} // namespace Curvz
