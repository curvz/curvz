// ScriptListener.hpp ─────────────────────────────────────────────────────────
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
// DSL grammar (m1, sandbox):
//
//   # comment                                  (ignored)
//   <object> <verb> [arg ...]                  invoke
//   get <object> <property>                    query, echo
//   assert <object> <property> == <literal>    query, compare
//   sleep <ms>                                 yield to main loop
//   list                                       print registered names
//   subscribe                                  start logging emit events
//   quit                                       end the script early
//
// Output protocol (one line per script line, sent to the output cb):
//
//   > <verbatim script line>
//   ok                                         on successful invoke
//   = <repr>                                   on get
//   PASS / FAIL: expected <a>, got <b>         on assert
//   event <name> <event> <repr>                on subscribed emit
//   error <message>                            on parse / dispatch error

#pragma once
#include "ScriptValue.hpp"
#include <functional>
#include <istream>
#include <string>
#include <vector>

namespace scriptproto {

class ScriptListener {
public:
    // Output sink. The listener calls this once per line of output.
    // Trailing newlines are included — the sink just appends.
    using OutputCallback = std::function<void(const std::string&)>;

    // Default-construct: no source stream. Use load_text() to feed
    // lines directly (scripter-window mode). load() is a no-op without
    // a source.
    ScriptListener() = default;

    // Stream-backed ctor: used by CLI-style flows that load() from a
    // file or stdin. Sandbox no longer wires this path, but the API is
    // kept so the listener stays useful outside the GUI.
    explicit ScriptListener(std::istream& in);

    // Plug in where output goes. If not set, output is dropped.
    void set_output_callback(OutputCallback cb) { m_out = std::move(cb); }

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

    // After load/run completion, exit status: 0 if all asserts passed,
    // 1 if any failed or any error occurred.
    int exit_status() const;

    struct Stats {
        int lines_run    = 0;
        int asserts_pass = 0;
        int asserts_fail = 0;
        int errors       = 0;
    };
    const Stats& stats() const { return m_stats; }

    // Callback fired after the last line dispatches. Used by main to
    // call Application::quit() in CLI mode; unused in scripter-window
    // mode.
    using DoneCallback = std::function<void()>;
    void set_done_callback(DoneCallback cb) { m_on_done = std::move(cb); }

private:
    void run_line(const std::string& raw);
    void emit_trace(const std::string& name,
                    const std::string& event,
                    const ScriptValue& payload);
    void out(const std::string& s);   // routes to m_out if set

    std::istream*            m_in = nullptr;
    std::vector<std::string> m_lines;
    size_t                   m_cursor = 0;
    Stats                    m_stats;
    bool                     m_subscribed = false;
    bool                     m_finished   = false;
    OutputCallback           m_out;
    DoneCallback             m_on_done;
};

// Lexer / literal parser exposed for the listener but useful for tests.
// Returns Null on parse failure.
ScriptValue parse_literal(const std::string& tok);

// A single lexed token. Quoted-vs-unquoted matters: an empty string
// literal `""` is a valid String token, while a bareword empty string
// can't be expressed (the tokeniser only emits tokens with content
// for barewords). The flag also disambiguates `"42"` (String "42")
// from `42` (Int 42).
struct Token {
    std::string text;
    bool        quoted = false;
};

// Split a script line into tokens. Whitespace-separated, except double-
// quoted strings preserve their interior verbatim. No escape sequences
// at m1 — simplest thing that works for the test scripts.
std::vector<Token> tokenise(const std::string& line);

} // namespace scriptproto
