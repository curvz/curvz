#pragma once
#include <string>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// Hyphenator — automatic hyphenation via libhyphen (Liang's algorithm, the same
// engine TeX / LibreOffice / browsers use). s357 m2.
//
// This is a pure CANDIDATE PROVIDER, not a layout decision. Given a word it
// returns the set of LEGAL in-word break points the dictionary's patterns
// allow; CHOOSING which one to use (the break that best fills the current line)
// belongs to the line fitter (m3), not here. libhyphen never decides line
// breaks — it only knows where a word may legally split.
//
// Relationship to the manual soft-hyphen path: identical downstream. m1 proved
// that a break + an appended trailing dash renders correctly and leaves the
// caret's byte mapping intact. Automatic hyphenation is just the fitter calling
// this to find a break instead of the user having typed a U+00AD. Both converge
// on the same break-and-append render; only the SOURCE of the break differs.
//
// Optional dependency: when libhyphen (or a dictionary) is absent, hyphenate()
// returns empty and the caller falls back to whole-word wrapping — the editor
// behaves exactly as before, just without mid-word breaks. Gated at build time
// by CURVZ_HAVE_HYPHEN (see CMakeLists.txt).
// ══════════════════════════════════════════════════════════════════════════════

namespace Curvz {

// Legal in-word break points for `word`, as BYTE offsets into `word`. Each
// offset is the index of the first byte AFTER the break, so a break at offset k
// splits the word into [0, k) + [k, n) — the leading prefix is what a line
// keeps (with a dash appended), the suffix flows to the next line. Positions
// honor the dictionary's left/right minimums, so "a-bandon" / "hyphenati-on"
// never come back. Returns empty for "do not hyphenate": short word, no
// dictionary, all-punctuation, or library compiled out. Empty == "break only at
// whole-word boundaries" (current behavior), so hyphenation degrades cleanly.
//
// `lang` is a dictionary key like "en_US". Dictionaries load once per language
// and cache for process lifetime; a failed load is remembered so it is not
// retried on every layout. Main-thread only (the cache is unsynchronized, which
// matches how Curvz lays out text).
std::vector<size_t> hyphenate(const std::string& word, const std::string& lang);

// Override the directory searched for `hyph_<lang>.dic`. Set before the first
// hyphenate() of a language (later calls hit the cache and ignore it). When
// unset, the resolver tries a Curvz data dir then common system locations
// (/usr/share/hyphen, /usr/share/myspell, the Flatpak /app share). Mainly for
// tests or non-standard installs.
void set_hyphen_dict_dir(const std::string& dir);

}  // namespace Curvz
