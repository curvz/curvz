# ![Scripter icon](img/icons/face-monkey-symbolic.svg) Scripter overview

The **Scripter** is a second window that drives Curvz with a small
text-based language. Every button, toggle, spinner, and menu item
in Curvz also has a *script name*, and a script is a list of lines
that address those names directly — click this button, set that
spinner to 12.5, assert this checkbox is on. Curvz reacts the same
way it would if you had clicked the real button with the real
mouse.

The point of the Scripter isn't to script Curvz from outside; it
is to be a **second pair of fingers** on the same app. You run a
script, you watch the canvas, and you see what the script saw.

This page is the **overview**. It covers what the Scripter is,
how to turn it on, the shape of the language, the addressing
model, the security posture, and where to look next. The full
noun-verb reference — every Scriptable, every verb, every
property, with examples — lives in **Scripter reference**.

## ❶ Turning the Scripter on

The Scripter is off by default. Enable it from either of two
surfaces — both flip the same preference:

- **Developer → Scripting** in the application menu (☰ at the
  right end of the header bar).
- The **Inspector → Document → Scripter** switch.

When scripting is enabled, a small **monkey button** appears on
the header bar between the document tabs and the menu. Click the
monkey to open the Scripter window; click it again — or close the
Scripter with its X — to hide it. The button is a toggle: its
highlighted state reflects whether the Scripter is currently
showing.

Turning scripting off hides the monkey button and dismisses the
Scripter window. Any script that was loaded into the editor is
preserved on disk; re-enabling the feature brings the sidebar and
editor back exactly where you left them.

## ❷ The Scripter window

The Scripter is a top-level window with three regions:

- **Sidebar (left)** — a collapsible outline of the script files
  in the workspace folder. Subfolders become disclosure groups
  (`▾ Tutorials`, `▾ Diagnostics`). Each row carries a checkbox
  to the left of the filename; the checkbox is the **run set**
  (see ❹ below). Click a row's filename to load that script into
  the editor.
- **Editor / Output notebook (centre)** — two tabs. The **Script**
  tab holds the current file, editable in place. The **Output**
  tab is the trace: every line that runs writes one line into
  this pane. Run auto-switches to Output so you can watch the
  trace land.
- **Playback strip (top)** — the Run button, the **Step delay**
  spinner (milliseconds between lines), the **Step mode**
  checkbox (advance one line per Spacebar press), and the
  **Auto-lower** checkbox (drop the Scripter behind the main
  window while a paced script plays).

At the bottom of the window the **status bar** shows the current
workspace folder and acts as a *Change folder* button — click the
strip to pick a different folder. The folder choice persists
across sessions.

A second, read-only **System scripts** section appears at the top
of the sidebar when Curvz ships with bundled scripts (under
`/usr/share/curvz/scripts`). These are tutorial and demo files
installed alongside the application — load them with one click,
but Save is disabled while a system file is in the editor. Use
**Save As…** to write a copy into your user folder where you can
edit freely.

## ❸ The language at a glance

A script is a sequence of lines. Each line is either a comment, a
*verb* (do something), a *query* (read something), an *assertion*
(read something and check it), or one of a small set of control
words. Indentation doesn't matter. Empty lines and lines starting
with `#` are skipped.

The grammar in one block:

```
# a comment

#[sub] a subtitle, shown in the Output trace
       and on Curvz's caption bar above the status bar

<object> <verb> [arg ...]            invoke a verb
get <object> <property>              read a property, echo it
assert <object> <property> == <lit>  read, compare, PASS or FAIL
do <action.name>                     dispatch a Gio action by name
sleep <ms>                           yield to the main loop
list                                 print every registered name
subscribe                            start logging emit events
set <name> to result                 bind the last result
                                     to a name for re-use
quit                                 end the script early
```

A canonical short script — three lines of body plus a caption:

```
#[sub] Switch to the Node tool

tb_nod click
assert tb_nod active == true
assert tb_sel active == false
```

`tb_nod` is the script name of the Node tool button on the
toolbar; `click` is the verb that's equivalent to a mouse click;
`active` is the property the button publishes (whether it's the
current tool). Running this script flips Curvz to the Node tool
exactly as if you had clicked the button — and then proves it,
twice: the Node button is now `active`, and the previously-active
Selection button (`tb_sel`) is no longer.

Literals look the way you'd expect:

- **Booleans:** `true`, `false`.
- **Integers:** `12`, `-3`, `0`.
- **Doubles:** `12.5`, `-0.75` — the decimal point is the
  discriminator.
- **Strings:** `"with quotes"` — the literal is whatever sits
  between the quotes; escapes are not interpreted.
- **Null** — the result of a verb that has no readable return
  (like `click`); never appears in source.

Integers and doubles do **not** cross-compare. `assert sp_qual
value == 8` matches an integer 8 only; if the spinner publishes
`8.0`, the assertion fails. Write the literal the same way the
property is published — the trace always shows the actual returned
form so you can correct mismatches without guessing.

The Output pane reads as a transcript:

```
> tb_nod click
ok
> assert tb_nod active == true
PASS
> assert tb_sel active == false
PASS
```

Every line that runs produces a `>` echo and one of:

- **`ok`** — a verb succeeded with no readable result.
- **`= <value>`** — a `get` returned that value.
- **`PASS`** / **`FAIL`** — an assertion's outcome (`FAIL` shows
  the expected and actual values).
- **`error <message>`** — the line couldn't be dispatched (typo,
  wrong type, missing object).

`subscribe` turns on a third trace shape, `event <name> <event>
<payload>`, which prints whenever a registered widget fires its
canonical signal — driven by a real user click as well as by the
script. The same channel, in other words: real interactions and
scripted interactions are indistinguishable from the trace's
point of view.

## ❹ Running scripts

The Run button has two behaviours, chosen by the sidebar's
checkboxes:

- **No rows checked** — Run executes the text in the editor.
  Type a few lines, hit Run, see the trace, edit, hit Run again.
  The editor is its own scratchpad even when no file is loaded.
- **One or more rows checked** — Run executes the checked
  scripts in sidebar order. Check three, hit Run, watch all
  three play through in sequence. The editor's current contents
  are ignored.

While a script is running the Run button reads **Stop**. Click
it to abort cleanly — the current line finishes, the trace stops
advancing, and the editor unlocks. Stop also fires automatically
if the script hits a fatal error or executes `quit`.

**Pacing.** The *Step delay* spinner inserts a wait between
lines. Set it to 0 for full speed (suitable for fast tests); set
it to 300–500 ms for live demos so the eye can keep up. A
`#[sub]` line gets a 2× multiplier — the caption sits visible for
twice the step delay before the next verb dispatches.

**Step mode.** Check the *Step* box and Run pauses after every
runnable line. Press **Spacebar** in the Scripter window to
advance one line. The editor highlights the next line that will
fire so you can read the source as you go. Comments and empty
lines are skipped automatically — every press lands on a line
that *does* something.

**Auto-lower.** When set, Run drops the Scripter behind the main
window at start and brings it back at end. Lets you watch Curvz
work without managing window stacking yourself. Ignored in Step
mode (where the Scripter needs focus to receive Spacebar).

## ❺ How addressing works

Every script line that does work begins with an **object name**.
The Scripter resolves that name against a process-wide registry
populated by Curvz at startup. The registry holds three families
of entries plus a handful of singletons.

- **Widget Scriptables** — buttons, toggles, spinners, entries,
  drop-downs. The single source of names for every widget in the
  application. Click `tb_pen` and the toolbar's Pen button
  clicks; read `sp_qual value` and you get the Quality spinner's
  current number.
- **Panel Scriptables** — companion objects that panels register
  on their own behalf so scripts can address panel-level
  outcomes (highlight, refresh, navigate) without poking through
  widget internals. Today `pnl_styles` is the only shipped
  panel Scriptable; other panels gain siblings as use cases name
  the need.
- **Collection Scriptables** — `layers`, `swatches`, `styles`,
  `themes`, `palettes`, `guides`, `objects`. These wrap the
  document's model — the things that have stable identity (an
  internal id) and aren't strictly UI. They route per-instance
  addresses through a `<collection>.<iid>` syntax (see ❻).
- **Singleton-shaped Scriptables** — `proj` (project lifecycle:
  save, save_as, close, load, new), `export` (the export
  dialog's outcomes from a headless surface), `app` (read-only
  introspection: version, gtk version), and `inspector` (open
  named Inspector sections, collapse them all, and a set of
  read-only diagnostic queries for path-safety state). These
  cover work the widget substrate can't reach because the
  outcome isn't a single widget click — file pickers, system
  dialogs, cross-section Inspector state.

**Two surfaces, same registry.** Every widget Scriptable carries
both a short **abbrev** (e.g. `tb_nod`) and a **long name** (e.g.
`main_toolbar_node_tool_btn`). Scripts can use either. The abbrev
is what you'd type for speed; the long name is what appears in
log lines and what the `list` verb shows so you can look up an
unfamiliar widget. The Scripter translates between the two
transparently — write whichever feels readable.

### Discovering names

Two ways to find what's registered:

- **`list`** — the simplest. A line containing just `list` prints
  every registered name in the trace, with the long name shown
  alongside the abbrev. Useful as a one-shot inspection line at
  the top of a script while you're exploring.
- **`widget_names.db`** — the on-disk index of every abbrev,
  long name, and source file, bundled into the application's
  resources as `widget_names.db`. The `list` verb reads from
  this file. The `widget_names_sync` tool in the source tree
  regenerates it from the C++ sources whenever a new widget
  ships; the regenerated file is committed alongside the code
  change.

## ❻ Collections and per-instance addressing

A document has *many* layers, *many* swatches, *many* objects on
the canvas. Each one has a stable internal id (an *iid*) — a
string of characters that doesn't change as the user reorders,
renames, or scrolls. Scripts address these by iid, not by row
position.

The pattern is `<collection>.<iid>`:

```
get layers count                       # collection-level query
layers new                             # collection-level verb,
                                       #   returns the new layer's iid
set bg to result                       # bind it
layers.bg rename "Background"          # per-instance verb
get layers.bg visible                  # per-instance query
```

The collection itself (`layers`, `swatches`, etc.) holds the
**set verbs** — create, delete, count, list. Per-instance
addressing (`layers.<iid>`) reaches the individual model object
and works on it directly. The dot syntax is a router: the
listener splits on the first dot, asks the collection whether it
owns the part after the dot, and materialises a transient proxy
that handles the verb or query.

**Why iid, not row index?** Because rows scroll, reorder,
collapse, and disappear. A script that says `layers.7f3a4b…
visible` keeps meaning what it meant even if the layer moved or
the Layers panel is closed entirely. Identity belongs to the
model object, not to its current UI representation.

**Reads of deleted iids return Null cleanly** — a script that
addresses an iid no longer in the document gets an empty result
and a single trace line, not a crash. Undoing a deletion makes
the iid resolve again on the next line.

## ❼ The `result` slot and named variables

Many verbs return a value. `layers new` returns the iid of the
freshly-created layer; `proj save_as "/tmp/test.svg"` returns the
saved path. The most recent non-Null return is held in a slot
called `result`, addressable on the next line:

```
layers new
get layers.result name        # "Layer 2", say
```

The slot is overwritten on every value-producing line. For
scripts that work with several objects, bind each one to a name
with `set <name> to result`:

```
layers new
set bg to result              # remember it as "bg"

layers new
set fg to result              # and the next as "fg"

layers.fg move "top"
layers.bg rename "Background"
```

Bound names live for the duration of the run — a fresh Run
starts with an empty table. Reserved names (`result`, language
keywords) can't be bound; the listener refuses with a clear
error rather than silently shadowing grammar.

## ❽ Security and the run context

The Scripter is a developer tool, and the lines it dispatches go
through the same code paths real user clicks go through. That
power is bounded by two layers.

**Path containment.** Verbs that take a filesystem path (save,
save_as, export, import) refuse paths outside `$HOME` and the
system temp directory. The check resolves symlinks first — a
script can't write `/tmp/innocent` if that name symlinks to
`/etc/passwd`. The refusal appears in the trace as a structured
`error` line; the dispatch never reaches disk.

**Run-context gating.** Every verb declares which callers may
invoke it. Three caller contexts exist — `TestRunner` (a future
CLI test harness), `Macro` (recorded user automation), and
`Scripter` (this window). Sensitive verbs may opt down to a
narrower mask; the dispatcher refuses calls from contexts not in
the mask. The Scripter today has the widest practical surface:
wider than Macro (debugging needs reach), narrower than
TestRunner (which can run save / load / snapshot for integration
tests). The mask is caller-determined and cannot be elevated
from inside a script — there is no "elevate this Scripter
session" verb.

What this protects against:

- A script that arrived as a clipboard paste, or downloaded from
  an untrusted source, cannot write outside your home or the
  temp directory.
- A script cannot escalate its own permissions by toggling a
  preference. The run context is set when the Scripter is
  constructed and stays fixed.
- A script that asks for an unknown verb, or for an iid that
  doesn't exist, gets a clean `error` line and the rest of the
  script continues.

What this does **not** protect against — and what to keep in
mind:

- A script *can* modify files inside `$HOME`. If you load
  somebody else's script, read it first. Same posture as running
  any other script in any other tool.
- A script *can* delete things in your project (layers, objects,
  styles) — that's the same surface the GUI has, gated by Undo,
  not by permission. Undo works after a scripted operation
  exactly as it does after a click.

## ❾ Where to look next

For a thorough catalogue of every Scriptable, verb, and property,
see **Scripter reference** — the next page in the Addendums.

For *worked examples*, the closest existing material lives in
the source tree under `tests/scripts/`. These are diagnostic and
regression scripts written across the Scripter's development,
each carrying a multi-line `#` docstring at the top explaining
what it exercises and what to look for in the trace. They're
not curated as a learning sequence, but reading a few of them
end-to-end is the fastest way to develop intuition for the
language and the noun surface.

A curated **tutorial bundle** is planned to ship in the system-
scripts section of the Scripter sidebar (the read-only area
under `/usr/share/curvz/scripts`). When it lands, this page will
name what's in it and how to read each tutorial in order. Until
then, the existing `tests/scripts/` files are the working
examples.
