# scriptproto

Sandbox for the **Script-Driven Curvz** programme — proves the script-
driven concepts in isolation before they land in the main app.

The whole point of this folder is to thrash here, not in Curvz. The
day the foundations get folded back, the seams are already drawn.

## What this proves

1. **GTK widget interception, the GTK way first.** Each widget wrapper
   binds the *canonical* GTK signal (`signal_toggled`,
   `signal_changed`, `signal_value_changed`, `property_selected`'s
   change signal) and reads/writes through the *canonical* GTK API
   (`get_active` / `set_active`, `get_text` / `set_text`, `get_value` /
   `set_value`, `get_selected` / `set_selected`). No clever overrides
   of GTK's behaviour — the wrapper is purely additive.

2. **The Scriptable mixin sits on top, via a templated base.**
   Every wrapper inherits from `ScriptableWidget<GtkBase>`, which
   itself inherits from both `GtkBase` AND `Scriptable`. The template
   centralises the discipline:

   - Name validation (non-empty, no whitespace, no DSL keyword)
     happens in the base ctor. Leaves can't bypass it because they
     can't reach the registration path except through the base.
   - The registration call to `Scriptable(name)` lives in one place,
     not duplicated across seven leaf ctors.
   - Canonical-signal binding is a pure-virtual hook (`bind_canonical()`)
     every leaf MUST override. Forgetting it is a compile error
     (can't instantiate the leaf class).
   - `init_scriptable()` is the leaf's one-line discipline: called as
     the last line of every leaf ctor, dispatches through the now-
     complete vtable into the leaf's `bind_canonical()`. A debug-build
     assertion in `~ScriptableWidget` catches a leaf that forgot.

   Net: the type system, not human attention, is what guarantees a
   well-formed Scriptable. Adding an eighth widget means writing the
   four overrides and one ctor line — invariants come for free.

3. **Bidirectional channel.** The wrapper's canonical-signal handler
   calls `Scriptable::emit()`, which routes through the registry's
   subscriber list. Whether the signal fired because of a real click
   or because a script called `widget.click`, the emit path is
   identical. One emit point; many subscribers.

4. **A DSL covering a general widget surface.** Seven widget types
   prove the property-intake variety GTK ships:
   - `ToggleButton` — toggle-state intake
   - `Button`       — clickable (no readable state)
   - `Entry`        — text intake
   - `SpinButton`   — numeric intake with bounds
   - `Scale`        — range intake (slider)
   - `DropDown`     — indexed-selection intake
   - `CheckButton`  — independent bool toggle

5. **Lifetime invariants.** `src/lifetime_test.cpp` runs at startup
   before any window opens. Six checks:
   - construct → registry finds it
   - destroy → registry is clean
   - reconstruct under same name → no leftover state
   - empty name throws
   - whitespace name throws
   - reserved DSL keyword throws
   Loud failure: prints to stderr and exits non-zero before the user
   sees a window. Foundation tests belong at boot.

## Build

```sh
cd scriptproto
cmake -B build -S .
cmake --build build
```

Depends on the same gtkmm-4.0 the main Curvz app does. No other
external libraries.

## Run

Two windows open: the **base** window (one of each registered widget,
named `tool.node`, `filename.entry`, etc.) and the **scripter**
window (script library + editor + output). Lifetime test runs first;
if it fails the program exits before any window opens.

```sh
./build/scriptproto
```

In the scripter window, pick a `.curvzs` from the left, edit if you
want, click **Run**. Output streams into the right pane. The base
window updates live as scripts dispatch.

## DSL grammar

```
# comment                                  (ignored)
<object> <verb> [arg ...]                  invoke
get <object> <property>                    query, echoes the value
assert <object> <property> == <literal>    query, compares, PASS/FAIL
sleep <ms>                                 yield to main loop
list                                       print registered objects
subscribe                                  start tracing emit events
quit                                       end the script early
```

Literals lex by form:

```
true | false                               → Bool
-?[0-9]+                                   → Int
-?[0-9]+\.[0-9]+                           → Double
"..."                                      → String  (no escapes)
<bareword>                                 → String  (symbolic args)
```

Output format (one line per script line):

```
> <verbatim script line>
ok                                         on successful invoke
= <repr>                                   on `get`
PASS / FAIL: expected <a>, got <b>         on `assert`
event <name> <event> <repr>                on subscribed emit
error <message>                            on parse/dispatch error
```

## Test scripts (shipped under `scripts/`)

- `01_toggle_click.curvzs`   — the canonical first proof, 2 lines.
- `02_entry_text.curvzs`     — text intake set/clear/read.
- `03_spin_value.curvzs`     — numeric intake, including clamp.
- `04_scale_value.curvzs`    — slider range intake.
- `05_dropdown_select.curvzs`— indexed-selection by index AND by text.
- `06_bidirectional.curvzs`  — `subscribe` then drive — events emit
                                from script-driven dispatches.
- `07_errors.curvzs`         — exercises the error surface.
- `08_list.curvzs`           — registry introspection.

## What this is NOT

- Not yet wired into Curvz. By design — main-app changes happen only
  after the seams here are stable.
- Not a UI-replay tester driven from outside via accessibility APIs.
  The hook is privileged and inside the binary.
- Not a backdoor. Writes dispatch through the same canonical GTK API
  paths a user's input does.

## Seams to lift back into Curvz

The interesting surfaces to carry over, in order of how tightly they
should match in the main app:

1. `Scriptable` + `ScriptableRegistry` + `ScriptableWidget` — copy
   verbatim. The template base is the load-bearing piece for m3.
2. `ScriptValue` + DSL lexer/parser            — copy verbatim.
3. `ScriptListener`                            — copy as-is.
4. `ScripterWindow`                            — copy with theme
   integration on the way in (Curvz's `apply_motif_to_window`
   already walks every top-level via `gtk_window_get_toplevels()`,
   so the Scripter window picks up the `curvz-light` CSS class
   automatically).
5. Widget wrappers — patterns transfer directly; per-widget verb /
   property tables grow as Curvz's actual widgets get migrated.
6. Compile-time gating (`CURVZ_DIAGNOSTIC` flag) — the main-app
   build wraps the listener and Scripter window in `#ifdef`.
   The `Scriptable` mixin compiles into production (cheap; name
   string + registry call at ctor/dtor, no other cost).

## Version history

- **v1 (s185)**: hand-rolled wrappers each inheriting
  `(Gtk::X, Scriptable)` directly. Proved the design end-to-end across
  seven widget types and eight test scripts.
- **v2 (s186)**: introduced `ScriptableWidget<GtkBase>` template base.
  Centralised name validation, registration, and canonical-signal
  discipline. Added `lifetime_test.cpp` for the parked open question
  on construct/destroy/recreate cycles. Same eight test scripts run
  unchanged — wire format is independent of wrapper internals.
