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
#include <memory>
#include <sstream>
#include <glib.h>   // g_usleep for `sleep` verb

namespace curvz::scripting {

// Forward declarations for static helpers defined later in this TU.
// is_runnable_line() (s193 m1) needs both before line ~140; rather
// than reorder the file, declare them here. The CommentTag struct
// belongs to the `#[tag]` extensible-comment-DSL (s191) — see
// parse_comment_tag below for the full grammar treatment.
static std::string trim(const std::string& s);
struct CommentTag {
    std::string tag;   // lowercased, empty if no marker
    std::string body;  // trimmed text after the closing bracket
};
static CommentTag parse_comment_tag(const std::string& line);

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
    m_last_result.clear();  // s216 m2 — fresh `result` slot per run
    m_vars.clear();         // s217 m1 — fresh variable table per run
    // m_subscribed is preserved — if the previous run subscribed, the
    // subscription on the registry is still live; flipping it back to
    // false here would silently drop emit_trace output on the next run.
}

int ScriptListener::take_delay_multiplier() {
    int m = m_next_delay_mult;
    m_next_delay_mult = 1;
    return m;
}

// s193 m1 — same skip rules as run_line(), but as a pure predicate.
// Kept structurally aligned with run_line's early-return branches so
// the two can't drift; if a new "silently skip" case is added there,
// add the same case here.
bool ScriptListener::is_runnable_line(const std::string& raw) {
    std::string line = trim(raw);
    if (line.empty()) return false;
    if (line[0] == '#') {
        CommentTag ct = parse_comment_tag(line);
        // Only `#[sub]` with a non-empty body is "runnable" — it emits
        // the flanked caption and triggers the subtitle callback.
        return (ct.tag == "sub" && !ct.body.empty());
    }
    return true;
}

size_t ScriptListener::next_runnable_index_from(size_t from) const {
    size_t i = from;
    while (i < m_lines.size() && !is_runnable_line(m_lines[i])) {
        ++i;
    }
    return i;
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

// ── ResolvedObject (s216 m1) ─────────────────────────────────────────────────
//
// Small RAII wrapper around `find_object`'s result. Holds either a
// borrowed pointer (registered objects — listener doesn't own, registry
// does) or an owned `unique_ptr` (transient proxies — listener owns for
// the duration of a single dispatch). The implicit-bool conversion and
// `operator->` keep the three call sites in `run_line` reading the
// same way they always did:
//
//   auto obj = find_object(toks[1].text);
//   if (!obj) { report_unknown(toks[1].text); return; }
//   obj->invoke(...);
//
// The wrapper destructs at end-of-statement scope; transient proxies
// allocated by collection Scriptables' `proxy_for()` are released
// cleanly without the caller having to know whether the resolution
// went through the registry or the router. Private to this TU — see
// the canon entry for the placement decision (promote when a second
// consumer surfaces).
class ResolvedObject {
    Scriptable*                 m_borrowed = nullptr;
    std::unique_ptr<Scriptable> m_owned;
public:
    static ResolvedObject borrow(Scriptable* p) {
        ResolvedObject r;
        r.m_borrowed = p;
        return r;
    }
    static ResolvedObject own(std::unique_ptr<Scriptable> p) {
        ResolvedObject r;
        r.m_owned = std::move(p);
        return r;
    }
    static ResolvedObject none() { return {}; }

    Scriptable* get() const {
        return m_owned ? m_owned.get() : m_borrowed;
    }
    explicit operator bool() const { return get() != nullptr; }
    Scriptable* operator->() const { return get(); }
};

// Find a Scriptable by token, accepting either abbrev, long_name, or
// the s216 m1 parameterised `<prefix>.<key>` address. Returns a
// `ResolvedObject` that either borrows a registered pointer (registry
// or resolver hit) or owns a transient proxy materialised by a
// collection Scriptable's router hooks. Empty ResolvedObject on miss
// — listener's `if (!obj)` shape is unchanged.
//
// Lookup order:
//   1. Direct registry lookup — common case for widgets and singletons.
//   2. WidgetNameResolver — long_name → abbrev → registry.
//   3. (s216 m1) parameterised address — if token contains '.', split
//      on the first dot, look up the prefix as a collection Scriptable,
//      ask `can_resolve(key)`, materialise via `proxy_for(key)`.
//
// Only the FIRST dot splits the address. UUIDs (the keys used by every
// model collection in 5a) contain hyphens, not dots — so
// `layer.7f3a4b8c-9e2d-4c1a-8b3f-2e9d8c7a6b54` splits cleanly to
// (`layer`, `7f3a4b8c-9e2d-4c1a-8b3f-2e9d8c7a6b54`). User-facing
// names that contain dots (swatches, styles, themes) won't reach the
// router stage because the keys are iids — name lookups happen via
// collection queries like `find_by_name "name.with.dots"` and resolve
// to an iid before the router sees them.
static ResolvedObject find_object(const std::string& token) {
    auto& reg = ScriptableRegistry::instance();

    // 1. Try as abbrev first — common case.
    if (auto* obj = reg.find(token)) return ResolvedObject::borrow(obj);

    // 2. Try as long_name via the resolver.
    if (auto abbrev = WidgetNameResolver::instance().resolve(token)) {
        if (auto* obj = reg.find(*abbrev)) return ResolvedObject::borrow(obj);
    }

    // 3. (s216 m1) Parameterised address — split on first dot.
    auto dot = token.find('.');
    if (dot != std::string::npos && dot > 0 && dot + 1 < token.size()) {
        std::string prefix = token.substr(0, dot);
        std::string key    = token.substr(dot + 1);
        if (auto* collection = reg.find(prefix)) {
            if (collection->can_resolve(key)) {
                if (auto proxy = collection->proxy_for(key)) {
                    return ResolvedObject::own(std::move(proxy));
                }
            }
        }
    }

    return ResolvedObject::none();
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
// (CommentTag struct forward-declared at top of TU; definition with
// field comments lives there.)

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

// ── Token substitution (s216 m2 / s217 m1) ──────────────────────────────────
//
// Replace standalone identifier tokens with their slot or variable
// values. "Standalone" means: outside any quoted string, and bounded
// on both sides by either end-of-string, whitespace, or `.`. The `.`
// boundary is the load-bearing case — it lets `layers.bg` substitute
// cleanly to `layers.<iid>` without the user needing to insert a
// separator the router would have to peel off again.
//
// Two kinds of substitution share this pass:
//
//   `result`    — the listener's last-`=` slot (single-shot, refreshed
//                 every time an `=` line emits). This is the s216 m2
//                 mechanism with its old `@` token replaced by the
//                 AppleScript-style word.
//   user names  — anything bound earlier via `set <name> to result`.
//                 The slot's value at the time of `set` was copied into
//                 m_vars[name]; whenever `name` appears as a standalone
//                 identifier, its stored value substitutes in.
//
// `result` is checked first and is the only "built-in" — it can never
// be set by the user (rejected at `set` parse time), so the lookup is
// unambiguous. After `result`, the token is checked against m_vars.
// Other identifiers — verb names, property names, language keywords —
// fall through and are left in place; the tokeniser sees them as
// regular tokens and dispatch handles them.
//
// What the substitution rules buy:
//
//   layers find_by_name "X"          --> = "<iid>"   (slot := <iid>)
//   set bg to result                  --> ok          (m_vars["bg"] := <iid>)
//   layers.bg toggle_visible          --> ok          (substitutes to layers.<iid>)
//   assert layers.bg visible == false --> PASS        (substitutes again; slot unchanged)
//
// Quoted strings are NEVER touched — a layer named literally `result`
// is hypothetical-but-legal and the script writer's `"result"` literal
// must survive verbatim. The substitution is text-level, pre-tokenise;
// the quote-state walk mirrors tokenise()'s own quote handling so the
// two agree on what "inside quotes" means.
//
// Slot semantics: starts empty. First substitution of `result` before
// any `=` line yields an empty string, which then propagates through
// the line — e.g. `layers.result X` becomes `layers. X`, which
// tokenises to two tokens (`layers.`, `X`) and falls through to the
// unknown-object path. That's defensive: a script that uses `result`
// before producing a result gets a loud failure, not silent-wrong. The
// slot is updated only by lines that emit `=` output (get, and non-Null
// invokes); `ok`, `PASS`, `FAIL`, `error`, and subscribed `event` lines
// leave it alone.
//
// Identifier shape: ASCII letter or `_` to start, then any mix of
// letters / digits / `_`. Matches the natural shape of every keyword
// and verb in the grammar; matches valid C-style variable names so
// `set` rejects anything that wouldn't have parsed as a single token
// anyway.
static bool is_ident_start(unsigned char c) {
    return std::isalpha(c) || c == '_';
}
static bool is_ident_cont(unsigned char c) {
    return std::isalnum(c) || c == '_';
}

static std::string substitute_tokens(
        const std::string& line,
        const std::string& last_result,
        const std::unordered_map<std::string, std::string>& vars) {
    std::string out_str;
    out_str.reserve(line.size() + last_result.size());
    bool in_quotes = false;
    size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        if (in_quotes) {
            out_str.push_back(c);
            if (c == '"') in_quotes = false;
            ++i;
            continue;
        }
        if (c == '"') {
            in_quotes = true;
            out_str.push_back(c);
            ++i;
            continue;
        }
        // Identifier candidate: must start at a left-boundary position
        // (start-of-line, whitespace, or `.`). Anything else means we're
        // mid-token — a verb name, a keyword, a property — and we leave
        // it alone. The dispatch layer handles those as plain text.
        bool left_ok = (i == 0)
            || std::isspace(static_cast<unsigned char>(line[i-1]))
            || line[i-1] == '.';
        if (left_ok && is_ident_start(static_cast<unsigned char>(c))) {
            size_t j = i + 1;
            while (j < line.size()
                   && is_ident_cont(static_cast<unsigned char>(line[j]))) {
                ++j;
            }
            // Right boundary check — must terminate cleanly. If the
            // next char is anything else (`(`, `:`, etc.), this isn't
            // a substitution target.
            bool right_ok = (j == line.size())
                || std::isspace(static_cast<unsigned char>(line[j]))
                || line[j] == '.';
            if (right_ok) {
                std::string ident = line.substr(i, j - i);
                if (ident == "result") {
                    out_str.append(last_result);
                    i = j;
                    continue;
                }
                auto it = vars.find(ident);
                if (it != vars.end()) {
                    out_str.append(it->second);
                    i = j;
                    continue;
                }
                // Not a substitution token — copy the identifier through
                // verbatim. Cheaper than re-scanning.
                out_str.append(line, i, j - i);
                i = j;
                continue;
            }
        }
        out_str.push_back(c);
        ++i;
    }
    return out_str;
}

// Reserved names that `set <name> to result` rejects. Anything that's
// a built-in verb/keyword would shadow grammar if it became a variable;
// `result` would break the slot contract (the var would silently
// shadow the slot in substitution, and the user's expectation that
// `result` means "the last `=`" no longer holds). Keep this list
// aligned with the verbs handled in run_line and the substitution
// special-case in substitute_tokens. New built-ins added later should
// be added here too.
static bool is_reserved_var_name(const std::string& name) {
    static const char* const reserved[] = {
        "result", "set", "to", "get", "assert", "list", "subscribe",
        "sleep", "quit", "do", "true", "false", "null",
    };
    for (const char* r : reserved) {
        if (name == r) return true;
    }
    return false;
}

// ── bare_repr — string repr WITHOUT the wrapping quotes ─────────────────────
//
// ScriptValue::repr() wraps strings in quotes (`"<value>"`) so the
// printed form is paste-back-compatible with the script DSL. For the
// `result`-slot substitution (and stored variable values) we want the
// BARE form — substituting "abc-def" into `layers.result` should
// produce `layers.abc-def`, not `layers."abc-def"`. Use repr() for
// printing to the trace; use this for the slot value.
//
// Non-string kinds (Null, Bool, Int, Double) come back identical to
// repr() because none of them quote — `123`, `true`, `1.5`, `null`. A
// caller substituting one of those into the next line gets the bare
// literal, same as if they'd typed it.
static std::string bare_repr(const ScriptValue& v) {
    if (v.kind == ValueKind::String) return v.s;
    return v.repr();
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

    // s217 m1 — `set <name> to result` is recognised BEFORE token
    // substitution so the name slot is the literal user text. If we
    // substituted first, `set result to result` would post-substitute
    // both `result` tokens to the slot value and bypass the reserved-
    // name check. Recognising the keyword head against the raw
    // (trimmed, pre-substitution) line is clean: `set` is not in
    // m_vars and not the `result` token, so substitution would never
    // touch it; the only reason to look at the raw line at all is
    // catching `set result to result` cleanly.
    //
    // Cheap prefix-check first so we only tokenise twice on actual
    // `set` lines. Every other line falls through to the normal
    // substitute → tokenise → dispatch flow with no extra work.
    //
    // Form is fixed: exactly four tokens, `set <name> to result`.
    // Future extensions — `set X to "literal"`, `set X to Y` — slot
    // into the same handler by checking what follows `to`. For s217 m1
    // the strict shape is both sufficient (the test ergonomics need
    // exactly this) and safe (no expression parser to maintain).
    if (line.size() >= 4
        && line.compare(0, 3, "set") == 0
        && std::isspace(static_cast<unsigned char>(line[3]))) {
        auto raw_toks = tokenise(line);   // `line` is trimmed, not yet substituted
        if (!raw_toks.empty()
            && !raw_toks[0].quoted
            && raw_toks[0].text == "set") {
            // Echo BEFORE handling so the trace shows the literal
            // source. (substitute_tokens would have echoed the post-
            // substitution form, which for `set` lines would just be
            // less informative — the user wrote `set bg to result`
            // and that's exactly what they want to see in the trace.)
            out("> " + line + "\n");
            ++m_stats.lines_run;

            if (raw_toks.size() != 4
                || raw_toks[1].quoted
                || raw_toks[2].text != "to" || raw_toks[2].quoted
                || raw_toks[3].text != "result" || raw_toks[3].quoted) {
                out("error set syntax: set <name> to result\n");
                ++m_stats.errors;
                return;
            }
            const std::string& name = raw_toks[1].text;
            // Identifier shape: must look like a valid name. Catches
            // `set 123 to result`, `set foo.bar to result`, etc.
            if (name.empty()
                || !is_ident_start(static_cast<unsigned char>(name[0]))) {
                out("error set: '" + name
                    + "' is not a valid variable name (must start with a letter or '_')\n");
                ++m_stats.errors;
                return;
            }
            for (char c : name) {
                if (!is_ident_cont(static_cast<unsigned char>(c))) {
                    out("error set: '" + name
                        + "' is not a valid variable name (letters, digits, and '_' only)\n");
                    ++m_stats.errors;
                    return;
                }
            }
            if (is_reserved_var_name(name)) {
                out("error set: '" + name
                    + "' is a reserved name and cannot be used as a variable\n");
                ++m_stats.errors;
                return;
            }
            m_vars[name] = m_last_result;
            out("ok\n");
            return;
        }
    }

    // s216 m2 / s217 m1 — token substitution. Apply before echoing the
    // line so the trace shows the substituted form (the address that's
    // actually about to be dispatched), not the symbolic source. The
    // slot is updated below at each `=` emit site; the variable table
    // is updated by the `set <name> to result` keyword handler above.
    line = substitute_tokens(line, m_last_result, m_vars);

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
    // s201 m3 — `do <action.name>` dispatches a Gio action through
    // the host's installed callback. Used to drive menu items and
    // action-driven buttons that aren't substrate-addressable (the
    // kebab "+ New style" menu, etc.). The host knows which widget
    // owns which action group; the listener just routes the name.
    //
    // Errors land in one of three buckets:
    //   1. No callback installed (host forgot to wire it). Treated
    //      as a script-level error so a test author sees the gap
    //      rather than a silent skip.
    //   2. Callback returned false (name didn't resolve). Same
    //      treatment — the test made a typo or the action lives
    //      somewhere the host doesn't know about yet.
    //   3. Callback threw. The script's pump catches at a higher
    //      level; this branch doesn't try-catch.
    if (!toks[0].quoted && head == "do") {
        if (toks.size() < 2) {
            out("error do needs an action name (e.g. 'do styles.create-empty')\n");
            ++m_stats.errors;
            return;
        }
        if (!m_action) {
            out("error do has no action callback installed by the host\n");
            ++m_stats.errors;
            return;
        }
        const std::string& action_name = toks[1].text;
        if (m_action(action_name)) {
            out("ok\n");
        } else {
            out("error do action '" + action_name + "' not recognised by the host\n");
            ++m_stats.errors;
        }
        return;
    }
    if (!toks[0].quoted && head == "get") {
        if (toks.size() < 3) {
            out("error get needs <object> <property>\n");
            ++m_stats.errors;
            return;
        }
        auto obj = find_object(toks[1].text);
        if (!obj) { report_unknown(toks[1].text); return; }
        ScriptValue v = obj->query(toks[2].text);
        out("= " + v.repr() + "\n");
        m_last_result = bare_repr(v);   // s216 m2 — feed the `result` slot
        return;
    }
    if (!toks[0].quoted && head == "assert") {
        if (toks.size() < 5 || toks[3].text != "==") {
            out("error assert syntax: assert <object> <property> == <literal>\n");
            ++m_stats.errors;
            return;
        }
        auto obj = find_object(toks[1].text);
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
    auto obj = find_object(toks[0].text);
    if (!obj) { report_unknown(toks[0].text); return; }
    ScriptArgs args;
    for (size_t i = 2; i < toks.size(); ++i) args.push_back(token_to_value(toks[i]));
    try {
        ScriptValue result = obj->invoke(toks[1].text, args);
        if (result.kind == ValueKind::Null) {
            out("ok\n");
        } else {
            out("= " + result.repr() + "\n");
            m_last_result = bare_repr(result);   // s216 m2 — feed the `result` slot
        }
    } catch (const std::exception& e) {
        out(std::string("error invoke threw: ") + e.what() + "\n");
        ++m_stats.errors;
    }
}

} // namespace curvz::scripting
