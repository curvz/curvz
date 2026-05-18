// scripting/ScriptListener.hpp ───────────────────────────────────────────────
//
// Reads a script (from any istream) and dispatches it line-by-line
// against the registry. Tracks assertion results. Writes its trace
// through an output callback — the caller plugs in stdout (for CLI
// runs) or a TextView appender (for the scripter window). The
// listener never touches std::cout directly.
//
// Threading: runs entirely on the GTK main thread. Lines are pumped
// via Glib::signal_idle() so each line returns control to GTK between
// dispatches — that's how widgets' canonical signals get to actually
// fire (their handlers run on the main loop) and how the output
// callback's TextView appends get a chance to repaint.
//
// DSL grammar (m1, sandbox / m2, Curvz integration / s217 m1, variables):
//
//   # comment                                  (ignored)
//   #[sub] subtitle text                       (rendered as a caption)
//   <object> <verb> [arg ...]                  invoke
//   get <object> <property>                    query, echo
//   assert <object> <property> == <literal>    query, compare
//   sleep <ms>                                 yield to main loop
//   list                                       print registered names
//   subscribe                                  start logging emit events
//   do <action.name>                           (s201 m3) dispatch a Gio
//                                              action by its fully-
//                                              qualified name — opens
//                                              menus the substrate
//                                              can't address as widgets
//   set <name> to result                       (s217 m1) bind the
//                                              current value of the
//                                              `result` slot into a
//                                              named variable; the
//                                              name becomes addressable
//                                              as a substitution token
//                                              on later lines (same
//                                              boundary rules as
//                                              `result` itself).
//   quit                                       end the script early
//
// Substitution tokens (s216 m2 / s217 m1):
//
//   result      — the value emitted by the most recent `=` line. Each
//                 `get` and each non-Null `invoke` refreshes it. Lines
//                 emitting `ok` / `PASS` / `FAIL` / `error` leave it
//                 alone. Substituted text-level before tokenisation,
//                 so the trace's `>` echo shows the substituted form.
//   <name>      — any identifier bound earlier via `set <name> to result`.
//                 Same substitution rules as `result`: boundary on both
//                 sides is start / whitespace / `.`; never substituted
//                 inside quoted strings.
//
// Tag markers (s191) — the `#[tag]` shape is a small extensible DSL
// inside the comment marker. Today only `#[sub]` is defined; future
// tags (e.g. `#[chapter]`, `#[pause ms]`) can be added without
// changing the grammar shell. Unknown tags are treated as plain
// comments (skipped silently) so a script using a future tag stays
// forward-compatible with an older listener.
//
// Output protocol (one line per script line, sent to the output cb):
//
//   > <verbatim script line>
//   ok                                         on successful invoke
//   = <repr>                                   on get
//   PASS / FAIL: expected <a>, got <b>         on assert
//   event <name> <event> <repr>                on subscribed emit
//   error <message>                            on parse / dispatch error
//
//   ── <subtitle text> ──                      on `#[sub]` line
//
// Lifted from scriptproto/ during s186 m2. Gated by CURVZ_DIAGNOSTIC
// — production builds don't compile this TU at all.

#pragma once
#include "scripting/RunContext.hpp"
#include "scripting/ScriptValue.hpp"
#include <functional>
#include <istream>
#include <string>
#include <unordered_map>
#include <vector>

namespace curvz::scripting {

class ScriptListener {
public:
    using OutputCallback = std::function<void(const std::string&)>;

    ScriptListener() = default;
    explicit ScriptListener(std::istream& in);

    void set_output_callback(OutputCallback cb) { m_out = std::move(cb); }

    // ── RunContext gating (s244 m2) ──────────────────────────────────────
    //
    // Each listener instance carries its caller context — set by the
    // host (ScripterWindow → Scripter; future CLI test runner →
    // TestRunner; future MacroSystem-as-script-client → Macro). The
    // dispatcher checks the context against each verb's mask on every
    // invoke; mismatched calls refuse with a structured error and an
    // audit log entry.
    //
    // Default is `Scripter` — the only caller that exists today is
    // ScripterWindow's listener, so a default-constructed listener
    // behaves like the in-app debugger. A future site that forgets to
    // call set_run_context() inherits "scripter" semantics, which is
    // the right unintended default: wider than Macro, narrower than
    // TestRunner. Closed-door defaults like Macro would silently
    // refuse every sensitive verb in any forgot-to-set site, which is
    // a different kind of failure than what these defaults should
    // optimise for.
    //
    // The context is caller-determined, not user-toggleable — there
    // is no "elevate this Scripter session" path. See CANON's
    // "RunContext gates the verb surface — no elevation toggle" entry.
    // Hosts that legitimately need a wider surface (the future CLI
    // test runner) set the context once at engine-instance creation
    // and never change it.
    void        set_run_context(RunContext c) { m_context = c; }
    RunContext  run_context() const           { return m_context; }

    // Reads all lines from the stream into a queue, returns immediately.
    void load();

    // Replace the loaded queue with the given text body, split on
    // newlines. Lets the scripter window push the TextView contents
    // into the listener without going through a file.
    void load_text(const std::string& body);

    // Reset stats and cursor to run again. Subscription state and
    // output callback are preserved.
    void reset();

    // Runs one line. Returns true if more lines remain, false otherwise.
    // Idle-callback friendly.
    bool pump_next();

    // s193 m1 — peek the cursor for editor-highlight feedback during
    // step mode. Returns the 0-based index of the line that the NEXT
    // pump_next() will dispatch (or lines_count() if the script is
    // exhausted). The Scripter uses this to highlight the about-to-
    // run line in the editor when parked waiting for spacebar.
    size_t next_line_index() const { return m_cursor; }
    size_t lines_count()     const { return m_lines.size(); }

    // s193 m1 — comment-skip in step mode. Returns the index of the
    // next "runnable" line at or after `from` (a line that produces
    // visible output or state change: verbs, get, assert, sleep,
    // quit, list, subscribe, or `#[sub]` markers). Lines that would
    // be silently skipped — empty lines, plain `#` comments, unknown
    // `#[tag]` markers, `#[sub]` with empty body — are passed over.
    // Returns lines_count() if no runnable line remains. Used by the
    // Scripter's step lambda to jump past comment headers so each
    // spacebar press lands on a line that DOES something.
    size_t next_runnable_index_from(size_t from) const;

    // s193 m1 — same classification, single line. Returns true if
    // pump_next on this line would do something visible (output,
    // dispatch, caption). Exposed for tests; the Scripter uses
    // next_runnable_index_from() instead.
    static bool is_runnable_line(const std::string& raw);

    int exit_status() const;

    struct Stats {
        int lines_run    = 0;
        int asserts_pass = 0;
        int asserts_fail = 0;
        int errors       = 0;
    };
    const Stats& stats() const { return m_stats; }

    using DoneCallback = std::function<void()>;
    void set_done_callback(DoneCallback cb) { m_on_done = std::move(cb); }

    // s191 m3 — subtitle callback. Fires when a `#[sub] text` line is
    // pumped. The body text is the caption to display (or empty to
    // clear). The listener still emits the trace-pane caption via the
    // output callback alongside — two surfaces, same trigger.
    //
    // Wiring: ScripterWindow does NOT install this (it's already
    // handling the trace pane). Application installs it after both
    // MainWindow and ScripterWindow exist, bridging the listener to
    // MainWindow's caption bar.
    using SubtitleCallback = std::function<void(const std::string& text)>;
    void set_subtitle_callback(SubtitleCallback cb) { m_sub = std::move(cb); }

    // s191 m3 — next-line delay multiplier. After a `#[sub]` line
    // fires, the next line's step delay is doubled so the caption
    // sits visible long enough to read (relative to whatever pace
    // the user picked via the Scripter's Step-delay spin button).
    // The pump scheduler calls take_delay_multiplier() once after
    // each pump_next() and uses it to scale the next timeout. The
    // value resets to 1 after each take_*.
    //
    // Honest at delay=0: the user opted into "no pacing"; subtitles
    // fly past with everything else. If you want readable subtitles,
    // raise the step delay.
    int take_delay_multiplier();

    // s201 m3 — action-dispatch callback. The script's `do <name>`
    // verb fires this with the action's fully-qualified name (e.g.
    // "styles.create-empty"). The host (Application) installs a
    // callback that routes the name to whichever panel or window
    // owns the action group with that prefix and calls
    // activate_action() on it. Returns true on success, false if
    // the name didn't resolve to any known action. The listener
    // formats the error message uniformly when the callback returns
    // false; the host doesn't need to.
    //
    // This was added because the substrate DSL today only speaks to
    // objects registered in the live ScriptableRegistry — buttons,
    // toggles, checks. Menu items and inline action-driven buttons
    // (kebab "+ New style", etc.) aren't substrate-addressable, but
    // they ARE action-driven, so dispatching the underlying Gio
    // action by name is the smallest surface that unblocks script-
    // driven UI tests. See s201 conversation for the framing
    // ("the script is just a virtual user").
    using ActionCallback = std::function<bool(const std::string& action_name)>;
    void set_action_callback(ActionCallback cb) { m_action = std::move(cb); }

private:
    void run_line(const std::string& raw);
    void emit_trace(const std::string& name,
                    const std::string& event,
                    const ScriptValue& payload);
    void out(const std::string& s);

    std::istream*            m_in = nullptr;
    std::vector<std::string> m_lines;
    size_t                   m_cursor = 0;
    Stats                    m_stats;
    bool                     m_subscribed = false;
    bool                     m_finished   = false;
    OutputCallback           m_out;
    SubtitleCallback         m_sub;
    DoneCallback             m_on_done;
    ActionCallback           m_action;  // s201 m3
    int                      m_next_delay_mult = 1;

    // s244 m2 — caller context for verb-mask gating. Default is
    // Scripter (the only caller wiring that exists today). See the
    // set_run_context() comment in the public section for the
    // closed-door-defaults reasoning and the audit-log contract.
    RunContext               m_context = RunContext::Scripter;

    // s216 m2 / s217 m1 — last-result slot for the `result` substitution
    // token. (Was `@` in s216 m2; renamed in s217 m1 to match
    // AppleScript's `result` predefined variable, which is the same
    // idea with a more discoverable name.)
    //
    // Holds the bare (un-quoted) string form of the most recent `=`
    // line's value. Persist-until-next-`=` semantics: every `get` and
    // every successful non-Null `invoke` updates this slot; lines that
    // emit `ok` (Null returns), `PASS` / `FAIL`, `error`, or no output
    // at all leave it unchanged. The slot starts empty; substituting
    // `result` before any `=` line has fired produces an empty string,
    // which falls through to the listener's normal unknown-object
    // path (defensive, not silent-wrong).
    //
    // The substitution happens BEFORE `out("> ...")` echoes the line,
    // so the user sees the substituted form in the trace ("> get
    // layers.7f3a4b... visible") not the literal source form ("> get
    // layers.result visible"). That's much more debuggable when the
    // resolution misfires — the trace shows the address that was
    // actually addressed, not the symbolic source.
    std::string              m_last_result;

    // s217 m1 — user-named variable table.
    //
    // `set <name> to result` binds the current value of `m_last_result`
    // (the slot fed by the previous `=` line) into this map under
    // `<name>`. Substitution then resolves any standalone identifier
    // token matching a key here against its stored value, with the same
    // boundary rules as `result` (start / whitespace / `.` on both
    // sides; never inside quoted strings).
    //
    // Why a separate map and not a single slot reused: the slot is
    // single-shot — every `=` overwrites it — and the variable table
    // is the answer to that. Scripts that manipulate multiple objects
    // bind each one to a name once and address them by name through
    // the rest of the script, without `find_by_name` wallpaper.
    //
    // Reserved names (rejected at `set` parse time): the language's
    // own keywords plus `result` itself. Storing values for those would
    // either shadow grammar (`set get to result`) or break the slot
    // contract (`set result to result`).
    //
    // Reset semantics: cleared alongside `m_last_result` in reset().
    // A re-run starts fresh; no leftover variables from the previous
    // run can change addressing.
    std::unordered_map<std::string, std::string> m_vars;
};

// Lexer / literal parser exposed for the listener but useful for tests.
ScriptValue parse_literal(const std::string& tok);

struct Token {
    std::string text;
    bool        quoted = false;
};

std::vector<Token> tokenise(const std::string& line);

} // namespace curvz::scripting
