# ![Scripter icon](img/icons/face-monkey-symbolic.svg) Widget name index

A flat alphabetical index of every named widget in Curvz. The
rest of the reference pages catalogue widgets *by zone* — this
page catalogues every widget *by name*, alongside its abbrev,
its widget type, and a pointer back to the reference leaf that
describes the surface it lives on.

It is meant to be **the page you grep**. If you know a widget
exists but cannot remember which Inspector section or dialog it
belongs to, Ctrl-F here is the shortest path to the answer.

## Why every widget has two names

Every named widget in Curvz carries two names: a short **abbrev**
(`tb_rect`, `dlg_bld_ok`, `pnl_lay_add_btn`) and a verbose
**long name** (`main_toolbar_rect_tool_btn`,
`blend_dialog_ok_btn`, `layers_panel_add_button`). Both resolve
to the same widget — they are aliases for one identifier, not
two different handles. The decision to carry both rather than
pick one was made deliberately because each form solves a
problem the other one cannot.

### Purpose 1 — scripting

The script substrate needs to address widgets from outside the
C++ heap. When a Scriptable verb says "click the OK button on
the Blend dialog," something has to map that intent back to a
`Gtk::Button*` inside the running window. The name is the only
stable handle the script side has: pointers are private, child
indices shift when the layout changes, and labels are
translatable. A name is the contract.

Two names because the script writer has two postures:

- **Typing.** When you are writing a script in the Scripter or
  in a macro, `tb_rect.click` is fast to type and easy to read
  inline. Abbrevs are short on purpose — they are the form the
  scripter optimises for *writing*.
- **Reading.** When you are reading someone else's script —
  your own from six months ago, a tutorial in the docs, a bug
  report — `main_toolbar_rect_tool_btn` tells you exactly what
  is being clicked without you having to look it up. Long names
  are the form the scripter optimises for *grep-ability*.

The resolver accepts either and treats them as identical. A
script that mixes the two — abbrev where convenient, long name
where clarity matters — runs the same as a script that picks
one form and sticks to it. The choice is rhetorical, not
semantic.

### Purpose 2 — debugging

The half that is easier to forget. A GTK4 app of any size has
an anonymity problem: a hundred widgets in the tree, every one
of them a `Gtk::Button*` or `Gtk::SpinButton*`, and when
something misbehaves you have no straightforward way to say
*which* button. Logs print pointer addresses. Widget-tree dumps
print class names. Crash traces print stack frames against type
names. None of these tell you whether the misbehaving widget is
the OK button on the Blend dialog or the Apply button on the
Style editor.

Calling `gtk_widget_set_name` on every widget at construction
time solves this. The same string the scripter uses to address
the widget is the string that appears in:

- `g_warning` / `g_critical` output — GTK's warnings reference
  the widget by its set name when one is present.
- The GTK Inspector's widget tree — the names are the labels
  on the tree nodes.
- CSS selectors — `#main_toolbar_rect_tool_btn { ... }` styles
  one specific button.
- Curvz's own `spdlog` instrumentation — any log line that
  includes `widget->get_name()` becomes self-documenting.

A widget you cannot name is a widget you cannot grep for. In a
50-kloc codebase that means you cannot reason about it. The DB
lets the scripter and the developer share one handle: the
scripter uses it as a script address, the developer reads it in
a log line, and both are looking at the same thing.

### Why both — together, not one or the other

Either form alone would have been worse than both:

- **Abbrev-only** would have been hostile to readers. `tb_rect`
  is fine when you know the system; it is opaque when you do
  not. A log line saying `tb_rect emitted clicked` is a
  riddle. A log line saying `main_toolbar_rect_tool_btn`
  emitted `clicked` is a sentence.
- **Long-name-only** would have been hostile to writers.
  Typing `main_toolbar_rect_tool_btn.click` four times in a
  macro for a multi-shape drawing tour becomes friction.
  Abbrevs exist so the writer does not pay that cost.

Two names, one widget, one resolver — the abbrev for the typer,
the long name for the reader, and the developer reads whichever
one is in the log. This is the same shape that command-line
tools have used for forty years (`ls -l` vs `ls --long`) and
the same reasoning applies here.

## How the scripter uses them

At runtime the `WidgetNameResolver` singleton loads the
`widget_names.db` table from the GResource bundle once at
startup and keeps it in memory as two lookup maps: abbrev →
long-name and long-name → abbrev. The resolver is consulted by:

- **`ScriptListener`** — when a script line tokenises a target
  like `tb_rect.click`, the listener calls the resolver to
  expand the abbrev before walking the widget tree. The walker
  matches against `Gtk::Widget::get_name()`, which stores the
  long form (set at construction by `curvz::utils::set_name`).
- **Trace output** — when a verb succeeds, the trace prints the
  long name so the trace is grep-able against source. A script
  written with abbrevs produces a trace written with long
  names — the abbrev is a writing affordance, not a runtime
  identity.
- **Macros** — recorded macros store the long name. When the
  user-facing recorder sees the user click on a widget, it
  reads `get_name()` (long form) and writes that into the
  macro. A macro is therefore portable to a future build even
  if the abbrev system changes — the long names are the
  durable identifiers.

Names are added to the DB at compile time by the
`widget_names_sync` harvester. It scans every `set_name(widget,
"abbrev", "long_name")` call in `src/` and `include/` and
emits the resulting table to `resources/data/widget_names.db`,
which is then bundled into the GResource. The source code is
the single source of truth — there is no hand-maintained list
to drift out of sync.

## Doc legend

The **REF** column points at the reference leaf where the
widget's zone is documented:

| ID | Leaf | Covers |
|---|---|---|
| **HM** | Header & menus reference | `win` actions, headerbar, document tabs, status bar |
| **TB** | Toolbar reference | Main toolbar, tool buttons, toolbar popovers, context bar |
| **IN** | Inspector reference | Every Inspector section on the right edge |
| **CT** | Content reference | Layers, Swatches, Styles, Themes, Library, Palettes panels |
| **CO** | Canvas & objects reference | Canvas widget and on-canvas object surface |
| **DG** | Dialogs & popovers reference | Modal dialogs and standalone popovers |

## The index

Every named widget in Curvz, sourced from the same
`widget_names.db` the runtime resolver loads. The live count
in the filter bar below shows how many rows match the current
filters (and the total). Use the **REF** dropdown to narrow by
zone (HM/TB/IN/CT/CO/DG), the **Type** dropdown to narrow
by widget kind (Button/SpinButton/...), and the **Search**
field for substring matches across both name columns. The
three controls AND together — a Type of "Button" plus a
REF of "IN" shows every button inside the Inspector. Click
any column header to sort by that column.

<!-- WIDGET_INDEX_TABLE -->

## What's not here

Widgets that are *not* named — anonymous boxes, labels used
as throwaway separators, transient popovers built on demand — do
not appear in this index because they have no script address
and no debug handle. If a widget is missing from this page and
you want to address it, the fix is to add a
`curvz::utils::set_name(widget, "abbrev", "long_name")` call at
construction; the harvester will pick it up on the next build
and the row appears here automatically.

Substrate verbs (`click`, `set_text`, `get_value`, …) are not
documented here — those live in the Language reference. This
page tells you *which* widget; the language page tells you
*what to do with it*.
