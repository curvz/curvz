// scripting/ScriptListener.cpp ───────────────────────────────────────────────
//
// Lifted from scriptproto/ during s186 m2. s187 m2 added long-name
// resolution via WidgetNameResolver — every object-lookup path tries
// the registry directly first (the canonical key is the abbrev), then
// falls back to looking the token up as a long_name in widget_names.db.
// `list` now renders both columns; `closest_name` suggests across both.
// Sample resolution:
//
//   tb_nod click                              → reg.find("tb_nod") hits.
//   main_toolbar_node_tool_btn click          → resolver translates to
//                                               "tb_nod", reg.find hits.
//   nonsense click                            → reg.find misses, resolver
//                                               misses, report_unknown
//                                               suggests across both
//                                               columns plus the registry's
//                                               live names.

#include "scripting/ScriptListener.hpp"
#include "scripting/Scriptable.hpp"
#include "scripting/ScriptableRegistry.hpp"
#include "scripting/WidgetNameResolver.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <glib.h>   // g_usleep for `sleep` verb

namespace curvz::scripting {

// ── tokenise ─────────────────────────────────────────────────────────────────
std::vector<Token> tokenise(const std::string& line) {
    std::vector<Token> toks;
    std::string cur;
    bool in_quotes = false;
    bool cur_quoted = false;
    auto flush = [&](bool was_quoted) {
        if (was_quoted || !cur.empty()) {
            toks.push_back({ std::move(cur), was_quoted });
            cur.clear();
        }
    };
    for (char c : line) {
        if (in_quotes) {
            if (c == '"') {
                flush(true);
                in_quotes  = false;
                cur_quoted = false;
            } else {
                cur.push_back(c);
            }
        } else {
            if (c == '"') {
                flush(false);
                in_quotes  = true;
                cur_quoted = true;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                flush(false);
            } else {
                cur.push_back(c);
            }
        }
    }
    flush(cur_quoted);
    return toks;
}

// ── parse_literal ────────────────────────────────────────────────────────────
ScriptValue parse_literal(const std::string& tok) {
    if (tok == "true")  return ScriptValue::boolean(true);
    if (tok == "false") return ScriptValue::boolean(false);
    if (tok.empty())    return ScriptValue::null();

    bool has_dot = tok.find('.') != std::string::npos;
    try {
        if (has_dot) {
            size_t pos = 0;
            double v = std::stod(tok, &pos);
            if (pos == tok.size()) return ScriptValue::real(v);
        } else {
            size_t pos = 0;
            long long v = std::stoll(tok, &pos);
            if (pos == tok.size()) return ScriptValue::integer(v);
        }
    } catch (...) {}
    return ScriptValue::text(tok);
}

// ── ScriptListener ───────────────────────────────────────────────────────────

ScriptListener::ScriptListener(std::istream& in) : m_in(&in) {}

void ScriptListener::out(const std::string& s) {
    if (m_out) m_out(s);
}

void ScriptListener::load() {
    if (!m_in) return;
    std::string line;
    while (std::getline(*m_in, line)) {
        m_lines.push_back(std::move(line));
    }
    out("# loaded " + std::to_string(m_lines.size()) + " script line(s)\n");
}

void ScriptListener::load_text(const std::string& body) {
    m_lines.clear();
    std::string cur;
    for (char c : body) {
        if (c == '\n') {
            m_lines.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) m_lines.push_back(std::move(cur));
    out("# loaded " + std::to_string(m_lines.size()) + " script line(s)\n");
}

void ScriptListener::reset() {
    m_cursor   = 0;
    m_stats    = {};
    m_finished = false;
    m_next_delay_mult = 1;  // s191 m3 — drop any leftover subtitle hint
    // m_subscribed is preserved — if the previous run subscribed, the
    // subscription on the registry is still live; flipping it back to
    // false here would silently drop emit_trace output on the next run.
}

int ScriptListener::take_delay_multiplier() {
    int m = m_next_delay_mult;
    m_next_delay_mult = 1;
    return m;
}

int ScriptListener::exit_status() const {
    return (m_stats.asserts_fail == 0 && m_stats.errors == 0) ? 0 : 1;
}

bool ScriptListener::pump_next() {
    if (m_finished) return false;
    if (m_cursor >= m_lines.size()) {
        out("# summary: "
            + std::to_string(m_stats.lines_run)     + " lines, "
            + std::to_string(m_stats.asserts_pass)  + " pass, "
            + std::to_string(m_stats.asserts_fail)  + " fail, "
            + std::to_string(m_stats.errors)        + " error\n");
        m_finished = true;
        // s191 m3 — clear any visible caption from the last `#[sub]`
        // line so the bar doesn't linger across runs. Empty body is
        // the documented "clear" signal in the subtitle callback
        // contract.
        if (m_sub) m_sub("");
        if (m_on_done) m_on_done();
        return false;
    }
    run_line(m_lines[m_cursor++]);
    return true;
}

void ScriptListener::emit_trace(const std::string& name,
                                 const std::string& event,
                                 const ScriptValue& payload) {
    if (!m_subscribed) return;
    std::string line = "event " + name + " " + event;
    if (payload.kind != ValueKind::Null) line += " " + payload.repr();
    line += "\n";
    out(line);
}

static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string closest_name(const std::string& target,
                                 const std::vector<std::string>& names) {
    if (names.empty()) return {};
    auto edit = [](const std::string& a, const std::string& b) {
        std::vector<std::vector<int>> d(a.size() + 1,
                                        std::vector<int>(b.size() + 1, 0));
        for (size_t i = 0; i <= a.size(); ++i) d[i][0] = static_cast<int>(i);
        for (size_t j = 0; j <= b.size(); ++j) d[0][j] = static_cast<int>(j);
        for (size_t i = 1; i <= a.size(); ++i) {
            for (size_t j = 1; j <= b.size(); ++j) {
                int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                d[i][j] = std::min({ d[i - 1][j] + 1,
                                     d[i][j - 1] + 1,
                                     d[i - 1][j - 1] + cost });
            }
        }
        return d[a.size()][b.size()];
    };
    std::string best = names.front();
    int best_score   = edit(target, best);
    for (const auto& n : names) {
        int s = edit(target, n);
        if (s < best_score) { best_score = s; best = n; }
    }
    // Threshold scales with target length so long names get more slack.
    // A 4-edit budget on "tb_nod" is generous; on "main_toolbar_node_tool_btn"
    // it's tight. Keep absolute floor of 4 for short tokens.
    int threshold = std::max(4, static_cast<int>(target.size()) / 4);
    return best_score <= threshold ? best : std::string{};
}

// Suggest the closest match considering BOTH abbrev and long_name columns
// in the resolver's DB, plus the live registry's names (which may include
// runtime-registered objects not in the DB). Returns the suggestion as an
// abbrev — the user-facing form is decorated in `report_unknown` so they
// see "did you mean 'tb_nod' (main_toolbar_node_tool_btn)?"
static std::string closest_name_across_columns(
        const std::string& target,
        const std::vector<std::string>& registry_names) {
    auto& resolver = WidgetNameResolver::instance();

    // Best candidate from each pool. Comparison happens via the same
    // edit-distance metric inside closest_name; we just call it three
    // times and pick the best resulting (or first non-empty if any
    // returned nothing).
    std::string from_abbrevs    = closest_name(target, resolver.all_abbrevs());
    std::string from_long_names = closest_name(target, resolver.all_long_names());
    std::string from_registry   = closest_name(target, registry_names);

    // If the long_name pool produced a match, translate it back to abbrev
    // for the canonical suggestion.
    if (!from_long_names.empty()) {
        auto abbrev = resolver.resolve(from_long_names);
        if (abbrev) from_long_names = *abbrev;
    }

    // Pick whichever is non-empty; prefer the registry result because
    // it's the source of truth for what's actually addressable right now.
    if (!from_registry.empty())   return from_registry;
    if (!from_abbrevs.empty())    return from_abbrevs;
    if (!from_long_names.empty()) return from_long_names;
    return {};
}

// Find a Scriptable by token, accepting either abbrev or long_name.
// Returns nullptr if the token resolves to no registered object.
static Scriptable* find_object(const std::string& token) {
    auto& reg = ScriptableRegistry::instance();

    // Try as abbrev first — common case.
    if (auto* obj = reg.find(token)) return obj;

    // Try as long_name via the resolver.
    auto abbrev = WidgetNameResolver::instance().resolve(token);
    if (abbrev) {
        if (auto* obj = reg.find(*abbrev)) return obj;
    }
    return nullptr;
}

static ScriptValue token_to_value(const Token& t) {
    if (t.quoted) return ScriptValue::text(t.text);
    return parse_literal(t.text);
}

// s191 — extract a `#[tag]` marker from a trimmed `#…`-prefixed
// line. Returns the tag name (lowercased) and the body that
// follows. If the line is a plain comment (no `[tag]` after the
// `#`) or malformed (unclosed bracket, etc.) the tag is empty
// and the line should be treated as a plain comment-skip.
//
// The `#[tag]` shape is a small extensible DSL: today `[sub]` is
// the only defined tag, but `[chapter]`, `[pause 500]`, `[mark]`
// can slot in without changing the grammar shell. Unknown tags
// fall through to comment-skip so old listeners are forward-
// compatible with new scripts.
struct CommentTag {
    std::string tag;   // lowercased, empty if no marker
    std::string body;  // trimmed text after the closing bracket
};

static CommentTag parse_comment_tag(const std::string& line) {
    // Caller guarantees line is trimmed and starts with '#'.
    if (line.size() < 2 || line[1] != '[') return {};
    auto close = line.find(']', 2);
    if (close == std::string::npos) return {};
    CommentTag ct;
    ct.tag = line.substr(2, close - 2);
    std::transform(ct.tag.begin(), ct.tag.end(), ct.tag.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    ct.body = trim(line.substr(close + 1));
    return ct;
}

void ScriptListener::run_line(const std::string& raw) {
    std::string line = trim(raw);
    if (line.empty()) return;

    // s191 — `#` starts a comment. The `#[tag]` shape selects a
    // tagged-comment dispatch; everything else is plain comment-
    // skip. Today only `[sub]` is wired (renders as a flanked
    // caption in the output pane). Existing scripts use `#` for
    // file-header docstrings (10-30 lines each, common across all
    // tests/scripts/*.curvzs) and stay zero-touch under this rule:
    // plain `#` lines never tripped a tag check, so they keep
    // skipping silently.
    //
    // Tutorial scripts paired with the s187 m4 pacing knob
    // (200-300ms step delay) use `#[sub]` for mid-script narration
    // between verbs. The marker is opt-in.
    if (line[0] == '#') {
        CommentTag ct = parse_comment_tag(line);
        if (ct.tag == "sub" && !ct.body.empty()) {
            // Two surfaces, same trigger:
            //   trace pane — flanked caption in the script output
            //   caption bar — MainWindow's diagnostic-only overlay
            out("\n── " + ct.body + " ──\n\n");
            if (m_sub) m_sub(ct.body);
            // s191 m3 — make the next line wait long enough to read
            // the caption. We double the user's existing step delay
            // rather than inventing our own duration: the spin
            // button in the Scripter window already represents the
            // user's chosen pace, and doubling respects it. Honest
            // at delay=0: subtitles fly past with everything else,
            // because the user opted out of pacing.
            m_next_delay_mult = 2;
        }
        // Unknown tag, empty-body sub, or plain comment — skip.
        return;
    }
    out("> " + line + "\n");
    ++m_stats.lines_run;

    auto toks = tokenise(line);
    if (toks.empty()) return;

    auto& reg = ScriptableRegistry::instance();

    auto report_unknown = [&](const std::string& which) {
        std::string suggest = closest_name_across_columns(which, reg.all_names());
        std::string msg = "error unknown object '" + which + "'";
        if (!suggest.empty()) {
            // Suggestion is the abbrev. If the resolver knows a long_name
            // for it, surface both forms — same data, different surface.
            std::string lng = WidgetNameResolver::instance().long_name_for(suggest);
            msg += " (did you mean '" + suggest + "'";
            if (!lng.empty() && lng != suggest) msg += " / '" + lng + "'";
            msg += "?)";
        }
        msg += "\n";
        out(msg);
        ++m_stats.errors;
    };

    // ── Built-in verbs ───────────────────────────────────────────────────────
    const std::string& head = toks[0].text;
    if (!toks[0].quoted && head == "list") {
        // Render each registered object as: "  abbrev  (long_name)".
        // If the resolver doesn't know a long_name for an abbrev (e.g. a
        // runtime-registered Scriptable that's not in widget_names.db),
        // just print the abbrev.
        auto& resolver = WidgetNameResolver::instance();
        for (const auto& n : reg.all_names()) {
            std::string lng = resolver.long_name_for(n);
            if (lng.empty() || lng == n) {
                out("  " + n + "\n");
            } else {
                out("  " + n + "  (" + lng + ")\n");
            }
        }
        return;
    }
    if (!toks[0].quoted && head == "subscribe") {
        if (!m_subscribed) {
            m_subscribed = true;
            reg.subscribe([this](const std::string& n,
                                  const std::string& e,
                                  const ScriptValue& p) {
                emit_trace(n, e, p);
            });
        }
        out("ok\n");
        return;
    }
    if (!toks[0].quoted && head == "sleep") {
        if (toks.size() < 2) {
            out("error sleep needs a duration in ms\n");
            ++m_stats.errors;
            return;
        }
        ScriptValue v = parse_literal(toks[1].text);
        long long ms = 0;
        if      (v.kind == ValueKind::Int)    ms = v.i;
        else if (v.kind == ValueKind::Double) ms = static_cast<long long>(v.d);
        if (ms > 0) g_usleep(static_cast<gulong>(ms) * 1000);
        out("ok\n");
        return;
    }
    if (!toks[0].quoted && head == "quit") {
        m_cursor = m_lines.size();
        out("ok\n");
        return;
    }
    if (!toks[0].quoted && head == "get") {
        if (toks.size() < 3) {
            out("error get needs <object> <property>\n");
            ++m_stats.errors;
            return;
        }
        auto* obj = find_object(toks[1].text);
        if (!obj) { report_unknown(toks[1].text); return; }
        ScriptValue v = obj->query(toks[2].text);
        out("= " + v.repr() + "\n");
        return;
    }
    if (!toks[0].quoted && head == "assert") {
        if (toks.size() < 5 || toks[3].text != "==") {
            out("error assert syntax: assert <object> <property> == <literal>\n");
            ++m_stats.errors;
            return;
        }
        auto* obj = find_object(toks[1].text);
        if (!obj) { report_unknown(toks[1].text); return; }
        ScriptValue actual   = obj->query(toks[2].text);
        ScriptValue expected = token_to_value(toks[4]);
        if (actual.equals(expected)) {
            out("PASS\n");
            ++m_stats.asserts_pass;
        } else {
            out("FAIL expected " + expected.repr() + ", got " + actual.repr() + "\n");
            ++m_stats.asserts_fail;
        }
        return;
    }

    // ── Default: invoke verb on object ───────────────────────────────────────
    if (toks.size() < 2) {
        out("error need at least <object> <verb>\n");
        ++m_stats.errors;
        return;
    }
    auto* obj = find_object(toks[0].text);
    if (!obj) { report_unknown(toks[0].text); return; }
    ScriptArgs args;
    for (size_t i = 2; i < toks.size(); ++i) args.push_back(token_to_value(toks[i]));
    try {
        ScriptValue result = obj->invoke(toks[1].text, args);
        if (result.kind == ValueKind::Null) {
            out("ok\n");
        } else {
            out("= " + result.repr() + "\n");
        }
    } catch (const std::exception& e) {
        out(std::string("error invoke threw: ") + e.what() + "\n");
        ++m_stats.errors;
    }
}

} // namespace curvz::scripting
