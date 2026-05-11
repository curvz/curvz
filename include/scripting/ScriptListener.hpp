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
// DSL grammar (m1, sandbox / m2, Curvz integration — unchanged):
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
//
// Lifted from scriptproto/ during s186 m2. Gated by CURVZ_DIAGNOSTIC
// — production builds don't compile this TU at all.

#pragma once
#include "scripting/ScriptValue.hpp"
#include <functional>
#include <istream>
#include <string>
#include <vector>

namespace curvz::scripting {

class ScriptListener {
public:
    using OutputCallback = std::function<void(const std::string&)>;

    ScriptListener() = default;
    explicit ScriptListener(std::istream& in);

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
    DoneCallback             m_on_done;
};

// Lexer / literal parser exposed for the listener but useful for tests.
ScriptValue parse_literal(const std::string& tok);

struct Token {
    std::string text;
    bool        quoted = false;
};

std::vector<Token> tokenise(const std::string& line);

} // namespace curvz::scripting
