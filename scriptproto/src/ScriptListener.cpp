// ScriptListener.cpp ─────────────────────────────────────────────────────────

#include "ScriptListener.hpp"
#include "Scriptable.hpp"
#include "ScriptableRegistry.hpp"

#include <algorithm>
#include <sstream>
#include <glib.h>   // g_usleep for `sleep` verb

namespace scriptproto {

// ── tokenise ─────────────────────────────────────────────────────────────────
// Whitespace-split with double-quoted-string preservation. No escapes.
// Quoted vs unquoted is preserved on the Token so the listener can treat
// `""` as an empty-string literal and `42` as an Int.
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
// Disambiguates bool / int / double from a single token. Strings are
// already split out by the tokeniser; this helper is for the post-split
// tail tokens that need typing.
ScriptValue parse_literal(const std::string& tok) {
    if (tok == "true")  return ScriptValue::boolean(true);
    if (tok == "false") return ScriptValue::boolean(false);
    if (tok.empty())    return ScriptValue::null();

    // Try double if a dot is present, integer otherwise. The lexer's
    // type discipline is "literal form determines kind."
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
    // Falls through to string interpretation. Bare-word strings are
    // permissible — useful for symbolic args ("union", "subtract").
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
    // m_subscribed is preserved — if the previous run subscribed, the
    // subscription on the registry is still live; flipping it back to
    // false here would silently drop emit_trace output on the next run.
}

int ScriptListener::exit_status() const {
    return (m_stats.asserts_fail == 0 && m_stats.errors == 0) ? 0 : 1;
}

bool ScriptListener::pump_next() {
    if (m_finished) return false;
    if (m_cursor >= m_lines.size()) {
        // No more lines — emit summary, fire done, and end the idle source.
        out("# summary: "
            + std::to_string(m_stats.lines_run)     + " lines, "
            + std::to_string(m_stats.asserts_pass)  + " pass, "
            + std::to_string(m_stats.asserts_fail)  + " fail, "
            + std::to_string(m_stats.errors)        + " error\n");
        m_finished = true;
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

// Trim leading/trailing whitespace. Empty-after-trim returns "".
static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Closest-match helper for unknown-object diagnostics. O(n*m) is fine —
// registry is tiny and this only runs on error.
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
    return best_score <= 4 ? best : std::string{};
}

// Helper: given a Token, produce a ScriptValue. Quoted tokens are
// String unconditionally; bareword tokens lex as bool/int/double/string.
static ScriptValue token_to_value(const Token& t) {
    if (t.quoted) return ScriptValue::text(t.text);
    return parse_literal(t.text);
}

void ScriptListener::run_line(const std::string& raw) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') return;   // comment / blank
    out("> " + line + "\n");
    ++m_stats.lines_run;

    auto toks = tokenise(line);
    if (toks.empty()) return;

    auto& reg = ScriptableRegistry::instance();

    // Helper used in many branches: report "unknown object 'X' (did you
    // mean 'Y'?)" with closest-match suggestion when sensible.
    auto report_unknown = [&](const std::string& which) {
        std::string suggest = closest_name(which, reg.all_names());
        std::string msg = "error unknown object '" + which + "'";
        if (!suggest.empty()) msg += " (did you mean '" + suggest + "'?)";
        msg += "\n";
        out(msg);
        ++m_stats.errors;
    };

    // ── Built-in verbs ───────────────────────────────────────────────────────
    // Note: built-in keywords (`list`, `subscribe`, `sleep`, `quit`, `get`,
    // `assert`) are only recognised if the first token is unquoted. A
    // quoted "list" would be a regular invoke target — though no widget
    // is named that, so it would error sensibly.
    const std::string& head = toks[0].text;
    if (!toks[0].quoted && head == "list") {
        for (const auto& n : reg.all_names()) out("  " + n + "\n");
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
        auto* obj = reg.find(toks[1].text);
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
        auto* obj = reg.find(toks[1].text);
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
    auto* obj = reg.find(toks[0].text);
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

} // namespace scriptproto
