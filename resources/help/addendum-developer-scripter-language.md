# ![Scripter icon](img/icons/face-monkey-symbolic.svg) Language reference

This page is the complete reference for the script language —
grammar, literals, control words, the trace format, and the
substitution rules. Every other page under **Scripter
reference** describes a particular surface's verbs and properties
and assumes the language itself is understood from here.

## ❶ Line structure

A script is a list of newline-separated lines. Each line is one
of these forms:

- **Empty** — skipped.
- **Comment** — `#` at the start of a (whitespace-trimmed) line
  skips the rest of the line. The Curvz convention is a
  multi-line `#`-prefixed docstring at the top of every script.
- **Subtitle** — `#[sub] <text>` — a tagged comment that renders
  as a flanked caption in the Output pane and on the main
  window's caption bar (see ❹). The next dispatched line waits
  for double the current Step delay.
- **Verb** — `<object> <verb> [arg …]` — call a verb on a
  registered Scriptable.
- **Query** — `get <object> <property>` — read a property and
  echo it.
- **Assertion** — `assert <object> <property> == <literal>` —
  read, compare, PASS or FAIL.
- **Control word** — one of `set`, `list`, `subscribe`, `do`,
  `sleep`, `quit` (see ❼).

Indentation, blank lines between blocks, and trailing whitespace
are all ignored. A script's smallest valid form is the empty
file — it runs cleanly and produces no trace.

## ❷ Tokens and quoting

Each line is tokenised left-to-right. Whitespace separates
tokens; double quotes group a sequence of characters as a single
literal string token.

- **Unquoted token** — `tb_nod`, `12.5`, `true`, `Background` —
  passed to the literal parser (see ❸).
- **Quoted token** — `"with quotes"` — the contents between the
  quotes are the exact string, no escapes interpreted. A quoted
  token is always a String value, even if its contents look
  like a number (`"12"` is the string "12", not the integer 12).
- **Empty quotes** — `""` — the empty string, a valid token.

There is no escape syntax for embedding a double quote inside a
quoted token; if you need a string containing `"`, that's not
expressible in source today. Tokens are also single-line — a
quoted string cannot span lines.

## ❸ Literals

Unquoted tokens are parsed against the rules below in order. The
first match wins; if none match, the token is treated as a
String.

- **`true` / `false`** → Bool.
- **`null`** is *not* a literal in source — it only appears in
  the trace as the printed form of a Null return.
- **Token contains a `.`** → parsed as Double. The whole token
  must consume cleanly (`12.5` is Double 12.5; `12.5x` is the
  string `"12.5x"`).
- **Token is all digits, optionally with a leading `-`** →
  parsed as Int.
- **Anything else** → String.

The five value kinds and how they print in the trace via the
`repr()` form:

- **Null** → `null`.
- **Bool** → `true` / `false`.
- **Int** → `12`, `-3`, `0`.
- **Double** → standard C++ `<<` formatting. A whole number
  prints without a trailing zero (`1.0` shows as `1`); a
  non-whole number shows up to six significant digits by
  default (`0.333333`). Trace output matches what the C++
  `ostream << double` would produce.
- **String** → `"with quotes"`. The trace always wraps string
  values in double quotes so a captured value pastes back
  into a script verbatim.

**Comparison is strict.** Integers and doubles do not
cross-compare. `assert sp_qual value == 8` matches the Int 8
only; if the property returns Double 8.0 the assertion fails.
The trace's `FAIL` line shows both expected and actual `repr()`
forms so the kind mismatch is visible.

## ❹ Subtitles

Lines starting `#[sub]` are tagged comments — the `[sub]` tag
opts the comment into the subtitle dispatch.

```
#[sub] Switch to the Node tool
```

A subtitle has two effects:

1. **Output pane** — writes a flanked caption: a blank line, then
   `── <text> ──`, then another blank line. Easy to spot in a
   long trace.
2. **Caption bar** — pushes the text to Curvz's caption strip
   above the status bar. The caption stays visible until the
   next subtitle line replaces it (or the script ends).

After a subtitle fires, the **next** dispatched line waits 2× the
current Step delay before it runs. That gives the reader twice
as long to read the caption before the next thing happens. The
multiplier is one-shot — only the very next line is doubled, not
every subsequent line.

If Step delay is 0, the multiplier still applies but `2 × 0`
is still 0 — subtitles fly past at full speed alongside
everything else. To see captions, set a non-zero Step delay
(300-500 ms reads comfortably).

Unknown tagged comments (`#[anything-else] body`) are skipped
silently, so scripts that use future tags stay forward-compatible
with older listeners.

## ❺ Verbs, queries, assertions

The three core dispatch shapes:

### Verb — `<object> <verb> [arg …]`

Looks up `<object>` in the registry, calls `invoke(<verb>, args)`
on the Scriptable. Arguments after the verb name are passed
positionally; the listener parses each one as a literal (see ❸)
before calling.

Trace shape:

```
> tb_nod click
ok
```

A successful invoke that returns no value writes `ok`. A
successful invoke that *does* return a value writes `= <repr>`
and the value lands in the `result` slot (see ❻):

```
> layers new
= "7f3a4b…"
```

A failure path writes `error <message>`; the script continues to
the next line, the error counter increments.

### Query — `get <object> <property>`

Looks up `<object>`, calls `query(<property>)` on the
Scriptable. Always echoes `= <repr>` regardless of whether the
property is recognised — an unknown property returns Null and
prints `= null`.

```
> get tb_nod active
= true
```

The returned value lands in the `result` slot.

### Assertion — `assert <object> <property> == <literal>`

Reads `<property>` from `<object>` (same as `get`), parses
`<literal>` against the literal rules (❸), compares the two with
strict equality. Strict-equality means different kinds never
match.

```
> assert tb_nod active == true
PASS

> assert sp_qual value == 8
FAIL expected 8, got 8.0
```

`PASS` increments the pass counter; `FAIL` increments the fail
counter (separate from the error counter — assertions don't
count as errors).

Note the `==` is a literal token in the assertion; it has no
other use in the language. Other comparison operators (`!=`,
`<`, `>`) are not in the grammar.

## ❻ The `result` slot

The most recently emitted non-Null value (from a `get` or a
value-returning verb) is held in a slot accessible as the token
`result` on subsequent lines. Substitution happens
*pre-tokenisation* so the trace echoes the substituted form:

```
> layers new
= "7f3a4b…"
> get layers.7f3a4b… name
= "Layer 2"
```

The slot stores the *bare* representation — string values are
substituted without their wrapping quotes, so
`layers.<iid>` resolves cleanly. Bool / Int / Double / Null all
substitute as their `repr()` form (which doesn't quote any of
them).

The slot is overwritten on every value-producing line. Lines
that emit `ok`, `PASS`, `FAIL`, `error`, or nothing leave the
slot untouched.

### Boundaries

`result` is substituted only as a complete token:

- **Start** — beginning of line, or whitespace, or `.` on the
  left.
- **End** — end of line, or whitespace, or `.` on the right.

This means `layers.result` substitutes (the `.` is a boundary on
the left of `result`); `resultx` does not (no boundary on the
right). Tokens inside double quotes are never substituted —
`set name to "result"` would bind the literal string `"result"`,
not the slot's contents.

### `set <name> to result` — naming the slot

The `set` keyword binds the current slot value to a named
variable for re-use:

```
> layers new
= "7f3a4b…"
> set bg to result
ok
> layers new
= "9c2e1d…"
> set fg to result
ok
> layers.fg move "top"
ok
> layers.bg rename "Background"
ok
```

The shape is fixed: exactly four tokens, `set`, `<name>`, `to`,
`result`. No other right-hand-side expression is parsed today —
you can't bind a literal, can't bind another variable, can't
bind a verb result inline. If those are needed, the slot pattern
covers them: run the verb, `set` the slot.

Naming rules:

- **First character** — letter or underscore.
- **Subsequent characters** — letters, digits, underscores.
- **Reserved names refused** — `result`, plus the language's own
  keywords (`set`, `get`, `assert`, `list`, `subscribe`, `do`,
  `sleep`, `quit`). Reserved-name use returns `error set:
  '<name>' is a reserved name and cannot be used as a variable`.

Bound names substitute under the same boundary rules as
`result`:

```
> layers.bg rename "Behind everything"
ok
> get layers.bg name
= "Behind everything"
```

Variables live for the duration of one Run. The table clears on
every fresh Run; nothing persists.

## ❼ Control words

The five non-dispatch control words.

### `list`

Prints every registered object name with its long name (if the
resolver knows one). Useful one-shot inspection at the top of a
script while you're exploring:

```
> list
  app  (app)
  cb_tn  (context_bar_tool_name_lbl)
  cv  (canvas_drawing_area)
  …
  tb_nod  (main_toolbar_node_tool_btn)
  …
```

Each row is indented two spaces. The output is sorted
alphabetically by abbrev. Entries that don't have a long name
(runtime-registered Scriptables not in `widget_names.db`) show
just the abbrev.

### `subscribe`

Turns on event subscription for the rest of the script. Every
emit from every registered Scriptable — driven by a real user
click as well as by a script verb — appears in the trace as
`event <name> <event> <payload>`:

```
> subscribe
ok
event tb_nod toggled true
event tb_sel toggled false
```

`subscribe` is idempotent — calling it more than once in the
same Run still produces only one subscription. There is no
`unsubscribe`; the only way to stop the stream is to end the
Run.

### `do <action.name>`

Dispatches a Gio action by its fully-qualified name. Used to
drive menu items and action-driven buttons that aren't substrate-
addressable (some menu items only have an action behind them, no
widget wrapper).

```
> do styles.create-empty
ok
```

If the action doesn't resolve, the listener writes `error do
action '<name>' not recognised by the host` and continues.

`do` takes exactly one argument — the action name. Action
arguments (for parameterised actions) are not yet expressible
through `do`. Use a direct widget invoke instead where possible;
many actions also have a wrapped form on `win.<verb>` (see the
Header & menus reference).

### `sleep <ms>`

Yields the main loop for the given number of milliseconds. The
argument can be Int or Double (rounded down to Int). Used
sparingly — the Scripter's Step delay knob is usually the right
pacing tool. `sleep` is for in-script "wait for X to finish" or
demonstrations that need a deliberate pause separate from the
global pace.

```
> sleep 500
ok
```

Zero or negative `<ms>` is a no-op (still writes `ok`).

### `quit`

Ends the script early. The cursor jumps to the end of the line
queue; the Run completes cleanly with whatever stats have
accumulated so far. Used in conditional-shaped scripts where an
early-exit clause makes a script self-document its termination
condition.

```
> get app version
= "0.13.0"
> quit
ok
```

## ❽ Trace format

Every dispatched line writes one or more lines into the Output
pane. The exact forms:

```
> <source line>                       echo (pre-tokenisation, post-substitution)
ok                                    side-effect verb succeeded with no return
= <repr>                              get / verb / set succeeded with a value
PASS                                  assertion held
FAIL expected <a>, got <b>            assertion failed (both as repr())
error <message>                       parse error or dispatch refusal
event <name> <event> <payload>        subscribed emit (subscribe must be active)

── <subtitle text> ──                 #[sub] line, flanked by blanks
```

The echo line is post-substitution — what you see in the trace
is the address that was actually addressed. If `result` resolved
to `7f3a4b…`, the echo shows `layers.7f3a4b…`, not `layers.result`.
This makes the trace forensically reconstructible: nothing in the
echo is symbolic.

The four counters tracked across a Run, accumulating until reset:

- **Lines run** — every dispatched line, including comments
  (which dispatch as no-ops). Used for "expected N lines" sanity
  checks.
- **Asserts pass** — `PASS` emits.
- **Asserts fail** — `FAIL` emits.
- **Errors** — `error` emits.

These show in the trailing summary the Scripter writes at end
of Run. Pre-stating the expected counts before pressing Run is
a discipline pattern — when the count matches, the script
behaved as designed; when it doesn't, something is off.

## ❾ Error categories

Errors fall into roughly six shapes. All write to the trace as
`error <message>` and increment the error counter; the script
keeps running.

- **Parse error** — bad syntax on a control word (`assert`
  without `==`, `set` without `to result`, `sleep` with no
  argument). Message names the expected form.
- **Unknown object** — `<object>` didn't resolve in the
  registry. The listener computes a closest-name suggestion
  across both the abbrev and long-name columns: `error unknown
  object 'tb_nod_x' (did you mean 'tb_nod' / 'main_toolbar_node_tool_btn'?)`.
- **Unknown verb / property** — `<object>` resolved, but the
  Scriptable's `invoke` / `query` didn't recognise the name.
  Each Scriptable returns its own message text here; conventions
  are not yet uniform.
- **Refused — wrong RunContext** — the verb declares a narrower
  mask than the caller. Message: `error invoke refused: verb
  '<verb>' on '<obj>' not permitted in <context> context`. Also
  logged at WARN level for audit.
- **Refused — path containment** — a verb that takes a path
  argument received a path outside `$HOME` and `/tmp`. The
  exact message is the path-safe checker's reason
  (`outside $HOME and /tmp`, `realpath failed`, …).
- **Invoke threw** — the Scriptable's `invoke` raised a C++
  exception. The listener catches and writes `error invoke
  threw: <what>`. Rare; usually indicates a programmer error
  inside Curvz rather than a script bug.

## ❿ End-to-end example

A complete short script that exercises most of the language:

```
# Demonstrate the language.

#[sub] Look around

list

#[sub] Switch tools and check the radio invariant

tb_nod click
assert tb_nod active == true
assert tb_sel active == false

#[sub] Capture and re-use a value

layers new
set newest to result

layers.newest rename "Scratch"
get layers.newest name

#[sub] Done

quit
```

Expected trace:

```
── Look around ──

> list
  app  (app)
  …

── Switch tools and check the radio invariant ──

> tb_nod click
ok
> assert tb_nod active == true
PASS
> assert tb_sel active == false
PASS

── Capture and re-use a value ──

> layers new
= "7f3a4b…"
> set newest to result
ok
> layers.7f3a4b… rename "Scratch"
ok
> get layers.7f3a4b… name
= "Scratch"

── Done ──

> quit
ok
```

Counters at end of Run: 11 lines run, 2 asserts pass, 0 asserts
fail, 0 errors. Pre-stating those counts before pressing Run is
the convention.
