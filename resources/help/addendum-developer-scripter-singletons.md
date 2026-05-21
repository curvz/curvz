# ![Scripter icon](img/icons/face-monkey-symbolic.svg) Singletons reference

The four **singleton-shaped** Scriptables. Each one has exactly
one registry entry per app session, no per-instance proxy, and
covers an outcome the widget substrate can't reach as a plain
button click — project lifecycle, file export, process
identity, cross-section Inspector state.

The four singletons:

- **`proj`** — open / save / save-as / close / new / load on
  the current project.
- **`export`** — write the active document as SVG, PNG, or a
  freedesktop icon-theme bundle.
- **`app`** — read process-scope identity (Curvz build version,
  GTK version).
- **`inspector`** — orchestrate Inspector section open/close
  across both registries, plus read-only path-safety diagnostics.

## ❶ `proj` — project lifecycle

Routes the same File-menu outcomes the GUI offers (New / Open /
Save / Save As / Close) past their picker dialogs. Verbs that
take a path argument are gated by **path containment** (see
**Scripter overview**, §❽) — the path resolves through
`realpath` and is refused if it lands outside `$HOME` or the
system temp directory.

### Verbs

- **`save`** — write the project to its current directory. No
  arguments. Refuses if no project is loaded, if the project
  has no directory yet (use `save_as` first), or if a canvas
  drag is in flight.
- **`save_as <path>`** — write the project to `<path>`. The
  path argument is required and goes through path-containment.
  Refuses on any of: bad arg shape, path outside $HOME / $TMPDIR,
  no project loaded, canvas drag in flight, I/O failure.
- **`close`** — close the currently-loaded project. No
  arguments. Refuses if no project is loaded, if a drag is in
  flight, or if the project has unsaved work (use `save` first).
- **`load <path>`** — replace the currently-loaded project
  with the one at `<path>`. Same path-containment and
  drag-in-flight refusals as `save_as`. Also refuses if the
  current project has unsaved work (stricter than the GUI, by
  design — silent automation should not destroy unsaved state).
- **`new <path>`** — create a fresh empty project at `<path>`
  and load it. Same refusals as `load`; additionally refuses if
  `<path>` already exists (no overwrite path today).

### Properties

- **`path`** — String. The current project's directory, or
  empty string if no project is loaded or the project has never
  been saved.
- **`has_path`** — Bool. True iff `path` is non-empty.
- **`dirty`** — Bool. True iff a project is loaded AND its
  undo history has work on it (the same proxy the GUI uses
  end-to-end for "is this project unsaved").

### RunContext masks

| Verb        | TestRunner | Scripter | Macro |
|-------------|:----------:|:--------:|:-----:|
| `save`      | ✓          | ✓        | —     |
| `save_as`   | ✓          | —        | —     |
| `close`     | ✓          | —        | —     |
| `load`      | ✓          | —        | —     |
| `new`       | ✓          | —        | —     |

`save` is in the Scripter's mask because it writes to a path
the user has already authorised through the GUI; the other four
either accept arbitrary paths (`save_as`, `load`, `new`) or
destroy the project the script is reasoning about (`close`).
Both rules narrow them out of the Scripter under the
**surface preservation** rule (a verb that destroys the script's
own readable surface is TestRunner-only).

Reads (`path` / `has_path` / `dirty`) are not gated — every
caller context can read them.

### Examples

A scripter-context preflight before saving:

```
assert proj has_path == true
assert proj dirty == true
proj save
assert proj dirty == false
```

Reading the current path:

```
get proj path
= "/home/me/Projects/curvz/myproject.curvz"
```

`save_as`, `load`, `new`, and `close` cannot be exercised from
the Scripter today — they all fall outside the Scripter's
RunContext mask and would write to the trace as a refusal:

```
> proj save_as "/tmp/test.curvz"
error invoke refused: verb 'save_as' on 'proj' not permitted in scripter context
```

A future TestRunner-context caller would have these available.

## ❷ `export` — write artefacts

Writes the project's **active document** to disk as a standalone
file (SVG, PNG) or directory tree (icon-theme bundle). Same
path-containment rules as `proj`; all three verbs sit in the
Scripter mask because export produces a *side artefact* (the
project itself is unchanged).

### Verbs

- **`svg <path>`** — write the active document as a standalone
  `.svg` file at `<path>`. Refuses on: bad arg shape,
  path-containment failure, no project loaded, no active
  document, I/O failure.
- **`png <path> <size>`** — write the active document as a PNG
  at `<path>`. `<size>` is an Int — the **longest-side** pixel
  count; the other dimension is computed from the document's
  aspect ratio, matching the GUI's Export dialog. Same refusals
  as `svg`, plus refusal on invalid size (≤ 0).
- **`theme <dir>`** — write the project's documents as a
  freedesktop icon-theme bundle at `<dir>` — a tree of
  `scalable/<category>/*.svg` + per-size PNG bundles + an
  `index.theme` and `gresource.xml`. `<dir>` is a directory
  argument (not a file); the contents land inside it. Same
  refusals as `svg`. The theme name defaults to the project's
  basename; comment text defaults to empty.

### Properties

- **`last_path`** — String. The path argument of the most
  recently *successful* export, regardless of format. Empty if
  no export has succeeded this session. Cross-format: a
  successful `svg` followed by a successful `png` leaves
  `last_path` equal to the png path.
- **`last_ok`** — Bool. True iff the most recent export
  attempt succeeded. False at session start and after any
  failed export.

### RunContext masks

| Verb     | TestRunner | Scripter | Macro |
|----------|:----------:|:--------:|:-----:|
| `svg`    | ✓          | ✓        | —     |
| `png`    | ✓          | ✓        | —     |
| `theme`  | ✓          | ✓        | —     |

All three are `Scripter | TestRunner`. Macro is out for the
same reason `proj save` is out of Macro: a recorded automation
writing arbitrary file paths without an explicit click trail
violates the user's mental model of macro behaviour.

### Examples

Export the active document as SVG to /tmp:

```
export svg "/tmp/scratch.svg"
assert export last_ok == true
assert export last_path == "/tmp/scratch.svg"
```

Render the active document as a 512-pixel PNG:

```
export png "/tmp/render.png" 512
```

Build an icon-theme bundle from the project:

```
export theme "/tmp/myicons"
assert export last_path == "/tmp/myicons"
```

The bundle ends up under `/tmp/myicons/scalable/...`,
`/tmp/myicons/16x16/...`, etc. — the standard freedesktop
icon-theme directory layout.

## ❸ `app` — process identity

Surfaces two read-only strings: the Curvz build version
(compiled into the binary from CMake's `project(curvz VERSION
X.Y.Z)`) and the GTK library version linked at runtime.

### Verbs

Both verbs return the same value as their query siblings —
useful when you want to capture the version into the `result`
slot for substitution into a later line:

- **`version`** — returns a String. The Curvz build version
  (e.g. `"0.13.0"`).
- **`gtk_version`** — returns a String. The runtime GTK
  version (e.g. `"4.14.5"`).

### Properties

- **`version`** — String. Same value as the verb.
- **`gtk_version`** — String. Same value as the verb.

Each read is exposed *both* as a verb and as a property because
the DSL's `assert` is equality-only on `query()` results — a
verb-only surface would not be assertable. Pair: verb-form for
imperative capture, property-form for assertion. Both compute
the same value independently.

### RunContext masks

| Verb           | TestRunner | Scripter | Macro |
|----------------|:----------:|:--------:|:-----:|
| `version`      | ✓          | ✓        | —     |
| `gtk_version`  | ✓          | ✓        | —     |

Macro is out for both: a recorded macro replaying `app version`
on a different build, or `app gtk_version` on a different
system, produces stale recorded values that violate the
recorded-macro mental model. Pure reads aren't dispatcher-gated
in their **query** form (queries aren't gated by design) but
the **invoke** form is.

### Examples

Capture and assert:

```
app version
assert app version == "0.13.0"

app gtk_version
get app gtk_version
```

Expected trace:

```
> app version
= "0.13.0"
> assert app version == "0.13.0"
PASS
> app gtk_version
= "4.14.5"
> get app gtk_version
= "4.14.5"
```

## ❹ `inspector` — section state and diagnostics

Orchestrates the Inspector's section open/close state across
both the right-panel section registry (Layers / Library /
Swatches / Styles / Themes / Documents / Preview /
PropertiesPanel) and PropertiesPanel's own internal
collapsibles (Object / Document / Application groups and their
inner sections). The wrap exists because cross-section
operations like "collapse everything" have to drive both
registries at once.

A second responsibility: read-only **path-safety
diagnostics** — the cached `$HOME` and `$TMPDIR` roots, plus
predicate checks against representative paths. Used by
diagnostic scripts that exercise the path-containment rule end
to end.

### Verbs

- **`open "<title>"`** — set the named section open. Other
  sections retain their current state. Title is the
  human-readable label of the section as built ("Styles",
  "Swatches", "Layers", "Dimensions", "Node", "Object", …).
  Unknown titles are silent no-ops.
- **`collapse_all`** — close every Inspector section across
  both registries. Persisted view state is updated; the next
  session load will restore the collapsed state. Same
  semantics as the keyboard-shortcut collapse-all action.
- **`diagnose_universal`** — diagnostic verb wired under the
  `all_three` mask. Used to verify a verb is reachable from
  every caller context.
- **`diagnose_test_runner_only`** — diagnostic verb wired
  under `TestRunner` only. Used to verify the gating mechanism
  refuses non-TestRunner callers correctly.

### Properties

All read-only path-safety diagnostics:

- **`home_root`** — String. The cached `$HOME` root (the
  symlink-resolved absolute path). Empty before
  `path_is_safe::init_roots()` runs at startup.
- **`tmp_root`** — String. The cached `$TMPDIR` root (or
  `/tmp` if `$TMPDIR` is unset).
- **`home_root_initialised`** — Bool. True iff
  `init_roots()` resolved $HOME cleanly.
- **`tmp_root_initialised`** — Bool. True iff `init_roots()`
  resolved $TMPDIR cleanly.
- **`home_root_is_safe`** — Bool. True iff `path_is_safe`
  accepts the cached home_root.
- **`tmp_root_is_safe`** — Bool. True iff `path_is_safe`
  accepts the cached tmp_root.
- **`etc_is_safe`** — Bool. True iff `path_is_safe` accepts
  `/etc`. Expected false (the canonical "outside both roots"
  case).
- **`slash_is_safe`** — Bool. True iff `path_is_safe`
  accepts `/`. Expected false (the canonical root-of-filesystem
  case).
- **`empty_is_safe`** — Bool. True iff `path_is_safe`
  accepts the empty string. Expected false.

### RunContext masks

| Verb                          | TestRunner | Scripter | Macro |
|-------------------------------|:----------:|:--------:|:-----:|
| `open`                        | ✓          | ✓        | ✓     |
| `collapse_all`                | ✓          | ✓        | ✓     |
| `diagnose_universal`          | ✓          | ✓        | ✓     |
| `diagnose_test_runner_only`   | ✓          | —        | —     |

Section operations are widely callable — they're cosmetic and
recoverable (the user can just reopen the section by clicking).
The two diagnose verbs have specific masks by design — they
*are* the test that gating works.

### Examples

Demo prep — collapse everything, then open just one section:

```
inspector collapse_all
inspector open "Styles"
```

Path-safety self-check:

```
assert inspector home_root_initialised == true
assert inspector home_root_is_safe == true
assert inspector etc_is_safe == false
assert inspector slash_is_safe == false
get inspector home_root
```

Expected trace (paths will vary):

```
> assert inspector home_root_initialised == true
PASS
> assert inspector home_root_is_safe == true
PASS
> assert inspector etc_is_safe == false
PASS
> assert inspector slash_is_safe == false
PASS
> get inspector home_root
= "/home/me"
```

Gating sanity check from Scripter context (should refuse the
TestRunner-only diagnostic):

```
inspector diagnose_universal
inspector diagnose_test_runner_only
```

Expected trace:

```
> inspector diagnose_universal
ok
> inspector diagnose_test_runner_only
error invoke refused: verb 'diagnose_test_runner_only' on 'inspector' not permitted in scripter context
```

## What's not here

Two surfaces are mentioned in the source as **planned, not
shipped** and don't appear above:

- **`inspector focus "<title>"`** — collapse_all + open in one
  shot, cascading through parent groups so the named section is
  visible regardless of where it sits in the tree. The
  MainWindow side already implements the cascading
  (`focus_inspector_on`); the verb wrapper is a one-liner
  pending a use case.
- **`inspector close "<title>"`** — symmetric to `open`.
  Needs a per-section close path on MainWindow that doesn't
  exist yet (only `collapse_all` is wired today).

Also planned at the language level (mentioned in the source as
forks) but absent today:

- **`proj force_close` / `force_load` / `force_new`** — the
  explicit-discard variants for the three destructive
  lifecycle verbs. The current refusal on unsaved work is the
  structural default; the force-variants would be the
  explicit opt-in.
