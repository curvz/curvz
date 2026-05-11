// scripting/WidgetNameResolver.hpp ───────────────────────────────────────────
//
// Resolves between the two forms of widget naming Curvz uses:
//
//   - **abbrev** (e.g. "tb_nod") — what every widget gets from
//     curvz::utils::set_name(); also the canonical registry key for
//     scriptable wrappers. Stable, short, fast to type.
//   - **long_name** (e.g. "main_toolbar_node_tool_btn") — the
//     self-documenting form. Same data, different surface.
//
// Source of truth: `widget_names.db`, generated from source by the
// `widget_names_sync` script. The DB is bundled into the binary as a
// GResource at `/com/curvz/app/data/widget_names.db`; loaded lazily on
// first resolution attempt and cached for the lifetime of the process.
//
// The resolver is **passive infrastructure**. The registry remains
// keyed by abbrev. The resolver translates long_name tokens to abbrev
// tokens before lookup, so existing code that does `reg.find(token)`
// keeps working as-is for abbrev tokens; the listener calls
// `resolve()` on miss to give long_name a chance.
//
// This is the s187 m2 ship: addressability becomes human-readable
// without changing any existing wrapper behaviour or registry keys.

#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace curvz::scripting {

class WidgetNameResolver {
public:
    // Process-wide singleton. Loads the DB on first use; subsequent
    // calls return the cached instance.
    static WidgetNameResolver& instance();

    // Given a token from a script, return the canonical abbrev that
    // the registry is keyed by. Returns nullopt if the token doesn't
    // match either an abbrev or a long_name in the DB.
    //
    //   resolve("tb_nod")                          → "tb_nod"
    //   resolve("main_toolbar_node_tool_btn")      → "tb_nod"
    //   resolve("nonsense")                        → nullopt
    //
    // Resolution order: if the token IS an abbrev, return it directly.
    // Otherwise look it up as a long_name. This keeps the fast path
    // fast — scripts that use abbrevs pay only an unordered_map probe.
    std::optional<std::string> resolve(std::string_view token) const;

    // Reverse direction: given an abbrev, return the long_name
    // documented for it (or empty string if no entry). Used by `list`
    // to render both columns and by closest_name suggestions.
    std::string long_name_for(std::string_view abbrev) const;

    // All known abbrevs (for help / introspection). Sorted alphabetical.
    std::vector<std::string> all_abbrevs() const;

    // All known long_names (for closest_name suggestions across both
    // columns). Sorted alphabetical.
    std::vector<std::string> all_long_names() const;

    // True if the DB loaded successfully. Empty DB or load failure
    // means the resolver is a no-op (resolve always returns nullopt,
    // long_name_for always returns "").
    bool ready() const;

private:
    WidgetNameResolver();
    void load_from_gresource();

    // Two maps for O(1) lookup in either direction.
    // m_long_to_abbrev: long_name → abbrev. Multiple long_names may
    //   resolve to the same abbrev (different files declare the same
    //   widget); we store the first encountered. After the s187 m1
    //   popover-rename cleanup this should be 1:1 in practice.
    // m_abbrev_to_long: abbrev → long_name. Same.
    std::unordered_map<std::string, std::string> m_long_to_abbrev;
    std::unordered_map<std::string, std::string> m_abbrev_to_long;
    bool m_ready = false;
};

} // namespace curvz::scripting
