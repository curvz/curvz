#pragma once
//
// GplFormat.hpp — GIMP palette (.gpl) format parser and writer.
//
// Per S101 handoff "palette interchange feature":
//   GPL is the chosen interchange format. Plain text. R G B [name] lines.
//   Industry-standard — interops with Inkscape, GIMP, Krita, lospec.com,
//   and friends. RGB-only in v1; alpha is a future Curvz extension via
//   `# Curvz: alpha=128` comments (GPL ignores comments — backward
//   compatible).
//
// This is the format seam — m1 ships parser + writer, no UI, no
// SwatchLibrary coupling. The module deals in plain `GplPalette` records:
// a header (name, columns) and a list of (r, g, b, name) entries. The
// import / export sites (m3+) translate between GplPalette and Curvz's
// Palette + Swatch types.
//
// Round-trip is non-negotiable: parse(write(p)) == p for any p we
// constructed, and the bytes we write round-trip through a foreign
// reader (GIMP, Inkscape) without semantic loss.
//
// Empty-name handling: the format layer preserves empty names verbatim.
// The auto-name-at-birth rule (no UUID-named swatches in user UI) lives
// at the SwatchLibrary import boundary, not here. A roundtrip of an
// empty-name entry stays empty-name.
//
// ── GPL grammar (informal, what we accept) ───────────────────────────
//
//   First line:    "GIMP Palette" (case-sensitive, exact match required)
//   Header lines:  "Name: <free text>"
//                  "Columns: <integer>"
//                  Both optional. Order doesn't matter to us, but most
//                  writers emit Name then Columns. Header ends at the
//                  first non-header, non-comment line.
//   Comment lines: lines starting with '#' (after optional whitespace
//                  before the '#'? — no, GIMP requires col 0). Ignored
//                  during parse, except the writer emits a single "#"
//                  separator after the header per convention.
//   Entry lines:   "<R> <G> <B>[<whitespace> <name>]"
//                  R, G, B are decimal integers 0–255. Tokens separated
//                  by any whitespace run. Name (if present) is the
//                  remainder of the line after the third number, with
//                  one separator-whitespace consumed; trailing CR/LF
//                  stripped. Tabs and multiple spaces inside the name
//                  are preserved.
//   Blank lines:   tolerated anywhere; ignored.
//
// We DO NOT support:
//   - The "Channels:" header (some GIMP versions emit it; we ignore on
//     read, omit on write).
//   - Floating-point RGB (not part of the format).
//   - BOM-prefixed files. Caller's responsibility to strip if needed.
//
// Errors: parse_gpl returns std::nullopt on any structural failure
// (missing magic line, malformed entry). It does not throw. Soft
// recoveries (e.g. RGB component > 255) are clamped at 255 with no
// error. The decision tree is:
//   - missing magic → fatal (nullopt)
//   - malformed Columns: → ignored, columns stays 0
//   - entry with < 3 numbers → fatal (nullopt) — corrupted file
//   - entry with R/G/B out of [0, 255] → clamped; not fatal
//   - garbage line that isn't comment, header, or entry → fatal
//

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Curvz {
namespace color {

// One row of a GPL file. The format is byte-for-byte symmetric with this
// record — read produces these, write consumes these, no lossy step.
struct GplEntry {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    // Trailing name. May be empty. Preserved verbatim across round-trip
    // including internal whitespace runs. Newlines / carriage returns
    // are stripped by the parser (they would corrupt the format).
    std::string  name;
};

struct GplPalette {
    // Palette display name from the "Name:" header. Empty if absent.
    // Trailing whitespace / CR is stripped on parse; leading whitespace
    // after the colon is consumed. Round-trips verbatim otherwise.
    std::string name;

    // Display-hint column count. 0 means "writer didn't emit it" —
    // we round-trip 0 as omitted, any positive value as "Columns: N".
    // Parser stores 0 for absent or malformed.
    int columns = 0;

    // Entries in source order. Order is user-visible (designer's
    // intended reading order) and round-trips intact.
    std::vector<GplEntry> entries;
};

// Parse a GPL file's textual content. Returns std::nullopt on any
// fatal structural error (see header comment for the decision tree).
//
// The input is the entire file content. Newline style (LF / CRLF) is
// auto-detected and irrelevant — all line endings are normalised on
// read and re-emitted as LF on write.
std::optional<GplPalette> parse_gpl(const std::string& text);

// Write a GplPalette into GPL textual form. Emits LF line endings.
//
// Output shape:
//   GIMP Palette
//   Name: <p.name>           (omitted if p.name is empty)
//   Columns: <p.columns>     (omitted if p.columns <= 0)
//   #
//   <r> <g> <b>\t<name>     (one line per entry; \t separates RGB block
//                             from name when name is non-empty; if name
//                             is empty, the RGB block is the entire line)
//
// RGB components are right-aligned in 3 columns ("  0", " 64", "255")
// to match the convention emitted by GIMP and Inkscape. This is purely
// cosmetic — readers tokenise on whitespace, so any spacing parses
// equivalently — but matches what foreign tools produce, so a Curvz-
// written file looks native to humans inspecting it.
std::string write_gpl(const GplPalette& p);

// ── File-system conveniences ─────────────────────────────────────────
//
// Thin wrappers over parse_gpl / write_gpl that read/write a path.
// Both return false on I/O failure (file not found, permission denied,
// etc.) and log via CurvzLog. Parse failures are also surfaced as
// false with a log line. The caller passes a default-constructed
// GplPalette to read_gpl_file; on success it is filled.

bool read_gpl_file (const std::string& path, GplPalette& out);
bool write_gpl_file(const std::string& path, const GplPalette& p);

} // namespace color
} // namespace Curvz
