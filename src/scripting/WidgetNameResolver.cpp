// scripting/WidgetNameResolver.cpp ───────────────────────────────────────────

#include "scripting/WidgetNameResolver.hpp"
#include "CurvzLog.hpp"

#include <algorithm>
#include <gio/gio.h>

namespace curvz::scripting {

namespace {
constexpr const char* k_db_resource = "/com/curvz/app/data/widget_names.db";
}

WidgetNameResolver& WidgetNameResolver::instance() {
    static WidgetNameResolver inst;
    return inst;
}

WidgetNameResolver::WidgetNameResolver() {
    load_from_gresource();
}

bool WidgetNameResolver::ready() const {
    return m_ready;
}

void WidgetNameResolver::load_from_gresource() {
    GError* err = nullptr;
    GBytes* bytes = g_resources_lookup_data(
        k_db_resource, G_RESOURCE_LOOKUP_FLAGS_NONE, &err);
    if (!bytes) {
        LOG_WARN("WidgetNameResolver: bundled widget_names.db not found "
                 "in GResources: {}. Long-name resolution disabled; "
                 "scripts must use abbrevs.",
                 err ? err->message : "?");
        if (err) g_error_free(err);
        return;
    }

    gsize sz = 0;
    const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &sz));
    std::string body(data, sz);
    g_bytes_unref(bytes);

    // DB format (one row per line, tab-separated):
    //   abbrev<TAB>long_name<TAB>file:line
    // Comments and blank lines are skipped (defensive — the generator
    // doesn't emit them, but a hand-edited DB might).
    size_t line_count = 0;
    size_t start = 0;
    while (start < body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string::npos) end = body.size();
        std::string_view line(body.data() + start, end - start);
        start = end + 1;

        if (line.empty() || line.front() == '#') continue;

        // Split on tabs. We only need the first two columns; the third
        // (file:line) is for the widget_names_sync tool, not us.
        size_t t1 = line.find('\t');
        if (t1 == std::string_view::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        std::string abbrev(line.substr(0, t1));
        std::string long_name(line.substr(
            t1 + 1,
            (t2 == std::string_view::npos ? line.size() : t2) - t1 - 1));
        if (abbrev.empty() || long_name.empty()) continue;

        // First-wins on collisions. After the s187 m1 popover-rename
        // cleanup, the DB is 1:1 in practice; this guard is belt-and-
        // braces for any future drift.
        m_abbrev_to_long.emplace(abbrev, long_name);
        m_long_to_abbrev.emplace(long_name, abbrev);
        ++line_count;
    }

    m_ready = !m_abbrev_to_long.empty();
    LOG_INFO("WidgetNameResolver: loaded {} rows from widget_names.db "
             "(ready={})", line_count, m_ready);
}

std::optional<std::string> WidgetNameResolver::resolve(
        std::string_view token) const {
    if (!m_ready) return std::nullopt;

    std::string key(token);

    // Fast path: token IS an abbrev. The hashmap probe is ~10ns and
    // handles the common case (scripts that author with abbrevs).
    if (m_abbrev_to_long.find(key) != m_abbrev_to_long.end()) {
        return key;
    }

    // Slow path: try as long_name.
    auto it = m_long_to_abbrev.find(key);
    if (it != m_long_to_abbrev.end()) return it->second;

    return std::nullopt;
}

std::string WidgetNameResolver::long_name_for(std::string_view abbrev) const {
    if (!m_ready) return {};
    auto it = m_abbrev_to_long.find(std::string(abbrev));
    if (it == m_abbrev_to_long.end()) return {};
    return it->second;
}

std::vector<std::string> WidgetNameResolver::all_abbrevs() const {
    std::vector<std::string> out;
    out.reserve(m_abbrev_to_long.size());
    for (const auto& kv : m_abbrev_to_long) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> WidgetNameResolver::all_long_names() const {
    std::vector<std::string> out;
    out.reserve(m_long_to_abbrev.size());
    for (const auto& kv : m_long_to_abbrev) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace curvz::scripting
