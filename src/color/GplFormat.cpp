//
// GplFormat.cpp — implementation of GIMP palette parser / writer.
//
// Parsing is line-based and forgiving on whitespace, strict on shape.
// The format is simple enough that a single pass with explicit state
// suffices; no lexer / parser-generator overhead.
//
// Writing is mechanical: emit magic, optional headers, "#" separator,
// one row per entry. The cosmetic right-alignment of R/G/B in 3-char
// columns matches GIMP / Inkscape output so files look native.
//

#include "color/GplFormat.hpp"
#include "CurvzLog.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace Curvz {
namespace color {

namespace {

// ── Line-level helpers ───────────────────────────────────────────────

// Strip a single trailing '\r' (handles CRLF input on a system that
// would otherwise treat it as part of the name field). Modifies in
// place.
inline void strip_trailing_cr(std::string& s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
}

// True iff the line is whitespace-only. Used for the "blank lines are
// allowed anywhere" rule.
inline bool is_blank(const std::string& s) {
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

// True iff the line is a comment ('#' as the first non-whitespace
// character — though GIMP itself only honours '#' at column 0, we are
// liberal in what we accept since stricter readers won't see our
// comments anyway, only entries).
inline bool is_comment(const std::string& s) {
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        return c == '#';
    }
    return false;
}

// If `line` starts with `prefix:` (case-sensitive on the prefix, then
// optional whitespace, then the value), return the value with leading
// whitespace stripped and trailing CR/whitespace stripped. Otherwise
// returns std::nullopt.
//
// Used for "Name:" and "Columns:" header line detection.
std::optional<std::string> header_value(const std::string& line,
                                        const std::string& prefix) {
    if (line.size() < prefix.size() + 1) return std::nullopt;
    if (line.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    if (line[prefix.size()] != ':') return std::nullopt;

    // Skip the colon, then any leading whitespace.
    std::size_t i = prefix.size() + 1;
    while (i < line.size() &&
           std::isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
    }
    std::string v = line.substr(i);

    // Strip trailing whitespace (incl. CR).
    while (!v.empty() &&
           std::isspace(static_cast<unsigned char>(v.back()))) {
        v.pop_back();
    }
    return v;
}

// Parse an entry line: "<R> <G> <B>[<whitespace><name>]". On success,
// fills `e` and returns true. On structural failure (fewer than three
// integers), returns false — the caller treats this as fatal.
//
// R/G/B values outside [0, 255] are clamped (not fatal); the format
// spec is integers 0–255 and we don't want to refuse files that have
// stray 256 typos.
bool parse_entry(const std::string& line, GplEntry& e) {
    // Tokenise the first three integers. We can't just use istringstream
    // for the whole line because the name may contain whitespace and
    // we need to know exactly where the third number ended so we can
    // take everything after as the name (preserving its internal spaces).
    std::size_t i = 0;
    const std::size_t n = line.size();

    auto skip_ws = [&]() {
        while (i < n &&
               std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
    };

    auto read_int = [&](int& out) -> bool {
        skip_ws();
        if (i >= n || !std::isdigit(static_cast<unsigned char>(line[i]))) {
            return false;
        }
        int v = 0;
        while (i < n && std::isdigit(static_cast<unsigned char>(line[i]))) {
            v = v * 10 + (line[i] - '0');
            // Cap accumulation so a pathological "999999999..." input
            // doesn't overflow before we clamp.
            if (v > 1000000) v = 1000000;
            ++i;
        }
        out = v;
        return true;
    };

    int r = 0, g = 0, b = 0;
    if (!read_int(r)) return false;
    if (!read_int(g)) return false;
    if (!read_int(b)) return false;

    // Clamp to [0, 255]. See header note on soft recovery.
    auto clamp_u8 = [](int v) -> std::uint8_t {
        if (v < 0)   return 0;
        if (v > 255) return 255;
        return static_cast<std::uint8_t>(v);
    };
    e.r = clamp_u8(r);
    e.g = clamp_u8(g);
    e.b = clamp_u8(b);

    // The name is the remainder of the line after one separator-
    // whitespace run. Consume one whitespace run so the name starts
    // at the first non-space char; preserve interior whitespace.
    skip_ws();
    if (i >= n) {
        e.name.clear();
    } else {
        e.name.assign(line, i, std::string::npos);
        // Strip trailing whitespace incl. CR. (Interior whitespace
        // stays — names like "Light  Blue" with two spaces round-trip.)
        while (!e.name.empty() &&
               std::isspace(static_cast<unsigned char>(e.name.back()))) {
            e.name.pop_back();
        }
    }
    return true;
}

} // anonymous namespace

// ── parse_gpl ────────────────────────────────────────────────────────

std::optional<GplPalette> parse_gpl(const std::string& text) {
    GplPalette p;

    std::istringstream in(text);
    std::string line;

    // First line must be the magic. We allow leading whitespace on
    // *internal* lines but not on the magic — GIMP refuses to open
    // files where the magic isn't at byte 0, and we want files we
    // accept to also be openable in GIMP.
    if (!std::getline(in, line)) {
        LOG_WARN("GPL parse: empty input");
        return std::nullopt;
    }
    strip_trailing_cr(line);
    if (line != "GIMP Palette") {
        LOG_WARN("GPL parse: missing 'GIMP Palette' magic line; got '{}'",
                 line);
        return std::nullopt;
    }

    // Header phase: consume lines until we hit something that isn't a
    // header line, comment, or blank. The first such line is the start
    // of the entries body and we'll parse it again below.
    //
    // We track header state with a flag rather than a do/while because
    // we may need to re-consume the line that ended the header phase.
    bool header_done = false;
    std::string pending; // line consumed but not yet classified as entry

    while (!header_done && std::getline(in, line)) {
        strip_trailing_cr(line);

        if (is_blank(line) || is_comment(line)) {
            continue;
        }

        if (auto v = header_value(line, "Name")) {
            p.name = *v;
            continue;
        }
        if (auto v = header_value(line, "Columns")) {
            // Parse integer; ignore on malformed (columns stays 0).
            try {
                p.columns = std::stoi(*v);
                if (p.columns < 0) p.columns = 0;
            } catch (...) {
                p.columns = 0;
            }
            continue;
        }
        // Some GIMP versions emit "Channels: RGBA" — we don't honour
        // it (alpha is a future Curvz extension), but we shouldn't
        // refuse the file over it. Detect via the header_value shape.
        if (header_value(line, "Channels")) {
            continue;
        }

        // Not a header / comment / blank — it's the first entry line.
        // Stash it for the entry loop and move on.
        pending = line;
        header_done = true;
    }

    // Entry phase. parse_entry on the pending line first (if any),
    // then on each subsequent line. Blank / comment lines are still
    // tolerated mid-body; bad entries are fatal.
    auto handle_entry_line = [&](const std::string& el) -> bool {
        if (is_blank(el) || is_comment(el)) return true;
        GplEntry e;
        if (!parse_entry(el, e)) {
            LOG_WARN("GPL parse: malformed entry line: '{}'", el);
            return false;
        }
        p.entries.push_back(std::move(e));
        return true;
    };

    if (!pending.empty()) {
        if (!handle_entry_line(pending)) return std::nullopt;
    }
    while (std::getline(in, line)) {
        strip_trailing_cr(line);
        if (!handle_entry_line(line)) return std::nullopt;
    }

    return p;
}

// ── write_gpl ────────────────────────────────────────────────────────

std::string write_gpl(const GplPalette& p) {
    std::string out;
    // Reserve a reasonable amount up front. Each entry is roughly
    // 12 bytes of RGB + tab + name + LF. 32 bytes/entry is a safe
    // overestimate for short names.
    out.reserve(64 + p.entries.size() * 32);

    out += "GIMP Palette\n";
    if (!p.name.empty()) {
        out += "Name: ";
        out += p.name;
        out += '\n';
    }
    if (p.columns > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Columns: %d\n", p.columns);
        out += buf;
    }
    out += "#\n";

    // Right-align each component in 3 columns ("  0", " 64", "255")
    // — matches GIMP / Inkscape output. The format readers tokenise
    // on whitespace so this is purely cosmetic.
    char rgb[16];
    for (const auto& e : p.entries) {
        std::snprintf(rgb, sizeof(rgb), "%3u %3u %3u",
                      static_cast<unsigned>(e.r),
                      static_cast<unsigned>(e.g),
                      static_cast<unsigned>(e.b));
        out += rgb;
        if (!e.name.empty()) {
            out += '\t';
            out += e.name;
        }
        out += '\n';
    }
    return out;
}

// ── File conveniences ────────────────────────────────────────────────

bool read_gpl_file(const std::string& path, GplPalette& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LOG_WARN("GPL read: cannot open '{}'", path);
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    auto parsed = parse_gpl(buf.str());
    if (!parsed) {
        LOG_WARN("GPL read: parse failure for '{}'", path);
        return false;
    }
    out = std::move(*parsed);
    return true;
}

bool write_gpl_file(const std::string& path, const GplPalette& p) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        LOG_WARN("GPL write: cannot open '{}' for writing", path);
        return false;
    }
    const std::string text = write_gpl(p);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        LOG_WARN("GPL write: write failure on '{}'", path);
        return false;
    }
    return true;
}

} // namespace color
} // namespace Curvz
