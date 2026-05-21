# ![Scripter icon](img/icons/face-monkey-symbolic.svg) Header & menus reference

Covers the **`win` Scriptable** — the single registry entry that
wraps every menu-bar and headerbar GAction Curvz exposes to
scripts. One Scriptable, 48 verbs grouped by menu domain, mirrors
the audit shape in `tier2_action_audit.md`.

Also covers the small set of named substrate widgets in this zone
(headerbar buttons, doc-tab strip, status bar) — most are
visual-naming-only and don't accept verbs.

Parallels manual chapter **3. Header & menus** and uses the same
domain split.

## ❶ `win` — menu and accel actions

Every action in Curvz's menubar / context-menus / accel table that
**doesn't** already have a model-Scriptable equivalent. The
wrap-now subset from the s254 Tier 2 audit — bucket-B actions
(reachable via `proj`, `objects`, `styles`, `swatches`, `themes`)
are deliberately NOT here; their canonical surface is the model
Scriptable. Calling `proj save` rather than `win save` is the
discipline; the latter doesn't exist.

The wrapper invokes the same callback chain a menu click does —
script-driven and user-driven invocations run through identical
code paths. "White-box reads, black-box writes" from the
`Scriptable` contract.

All 48 verbs share the **default mask** `Scripter | TestRunner` —
Macro is excluded for the same reason `proj save` is excluded
from Macro: a recorded automation re-invoking arbitrary menu
commands without the original click trail violates the
recorded-macro mental model. The two diagnostic-style verbs
(`toggle-rulers`, `toggle-outline`) follow the same mask.

The verb name is **always the action name with hyphens**, never
the underscore-suffixed `act_*` widget ID. Widget IDs are the
substrate-naming parallel for the GUI side; scripts address by
verb. `win undo` works; `win act_undo` does not.

### Verbs — Edit menu (8)

- **`undo`** — undo the last command on the active document's
  history. Refuses if no command is on the undo stack (silent
  on Gio's side; the GUI menu item is disabled in that state).
- **`redo`** — redo the last undone command. Symmetric to `undo`.
- **`cut`** — cut current selection to the system clipboard.
  Empty selection is a no-op at the canvas layer.
- **`copy`** — copy current selection to the clipboard.
- **`paste`** — paste clipboard contents into the active document.
- **`duplicate`** — selection-duplicate with a small visible
  offset (clipboard-mediated round-trip). Distinct from
  `objects.duplicate <name>` which targets a named object;
  `win duplicate` targets the current selection.
- **`duplicate-in-place`** — selection-duplicate at the same
  coordinates (zero offset, clipboard-mediated). Distinct from
  `objects.duplicate` — same selection-vs-name distinction.
- **`delete-selected`** — delete the currently selected objects.
  Distinct from `objects.delete <name>`; selection vs name.

### Verbs — Selection (2)

- **`select-all`** — select all objects in the active layer.
- **`deselect-all`** — clear the canvas selection.

### Verbs — Z-order / arrange (4)

All four reorder the selection within its parent container's
child list. No-op on an empty selection.

- **`arrange-bring-front`** — raise to top of stack.
- **`arrange-bring-forward`** — raise by one position.
- **`arrange-send-backward`** — lower by one position.
- **`arrange-send-back`** — lower to bottom of stack.

### Verbs — Transforms (2)

- **`flip-horizontal`** — flip selection across the vertical axis
  through the selection bbox centre.
- **`flip-vertical`** — flip selection across the horizontal axis.

(Rotation is NOT here — there's no `win rotate-90-cw` / similar.
Rotation flows through the Translate dialog or manual handle
drag. Possible future verb on a selection-Scriptable.)

### Verbs — Boolean ops (3)

All three combine the selected paths into a single path with the
named set operation. Refuse when the selection has fewer than two
qualifying paths.

- **`bool-union`** — union of all selected paths.
- **`bool-subtract`** — top path minus the others.
- **`bool-intersect`** — intersection of all selected paths.

### Verbs — Path operations (4)

- **`expand-stroke`** — convert the selection's stroke into a
  filled outline path. Refuses on selections that have no stroke
  to expand.
- **`make-compound`** — combine multiple selected paths into a
  single compound path with shared style.
- **`split-compound`** — explode a compound path back to its
  child paths.
- **`text-to-path`** — convert selected text objects into
  outlined path objects.

### Verbs — Align / distribute (8)

The six **align** verbs snap the selection to a common edge or
centre line within its bbox; the two **distribute** verbs space
multi-item selections evenly along an axis.

- **`align-left`** — left edges to leftmost.
- **`align-right`** — right edges to rightmost.
- **`align-center-h`** — horizontal centres to common X.
- **`align-top`** — top edges to topmost.
- **`align-bottom`** — bottom edges to bottommost.
- **`align-center-v`** — vertical centres to common Y.
- **`distribute-h`** — equal horizontal spacing.
- **`distribute-v`** — equal vertical spacing.

### Verbs — Container ops (6)

Clip / blend / warp make-and-release verbs. The corresponding
**edit** verbs (`warp-edit`, `warp-flatten`, etc.) are NOT in the
script surface today — they're dialog-launchers and live in the
audit's bucket C; only their underlying make / release operations
are scriptable.

- **`clip-make`** — make selection into a clip group.
- **`clip-release`** — release a clip group.
- **`blend-make`** — make a blend from selected source nodes.
- **`blend-release`** — release a blend.
- **`warp-make`** — make a warp from selection.
- **`warp-release`** — release a warp.

(Plain groups round-trip through `objects.group` / `objects.ungroup`
on the `objects` Scriptable — not via `win`. See the future
**Canvas & objects reference** for the full `objects` surface.)

### Verbs — Step-and-repeat (1)

- **`step-repeat`** — open the step-and-repeat popover seeded
  from the current selection. The popover's Apply path does the
  multiplication; the verb is a hand-off to the GUI flow.
  (A future parameterised `selection.step_repeat(...)` will
  replace this as the direct-do form; the wrapper exists today
  as the only script entry point.)

### Verbs — View toggles + zoom (10)

Two stateful bool verbs that also surface readback state, plus
six zoom verbs and two document-cycling verbs.

- **`toggle-rulers`** — flip ruler visibility (no args), or set
  explicitly with one Bool. `win toggle-rulers` toggles current;
  `win toggle-rulers true` sets visible explicitly.
- **`toggle-outline`** — flip outline-render mode, or set
  explicitly. **Refuses an outline→preview transition at extreme
  zoom** — the preview mode's drop-shadow Cairo buffer scales
  with device-pixel coverage and would crash; outline mode is
  safe at any zoom. The refusal shows a user-facing alert and
  leaves the state unchanged.
- **`zoom-in`** — discrete zoom-in step.
- **`zoom-out`** — discrete zoom-out step.
- **`zoom-fit`** — zoom to fit all objects in the viewport.
- **`zoom-100`** — zoom to 1× (artboard fills viewport with
  margin).
- **`zoom-200`** — zoom to 2×.
- **`zoom-selection`** — zoom to fit current selection.
- **`doc-next`** — cycle to the next open document in the tab
  strip.
- **`doc-prev`** — cycle to the previous open document.

### Properties — bool action state readback

Two read-only properties matching the two stateful bool verbs.
The property name is the verb suffixed with `_state` (underscore
separator — hyphenated verb name + `_state` parses fine because
the dispatcher tokenises on whitespace).

- **`toggle-rulers_state`** — Bool. True iff rulers are
  currently visible.
- **`toggle-outline_state`** — Bool. True iff the active
  document is in outline-render mode.

Plain-verb (stateless) actions don't surface readback today;
their query property is Null. Future per-action `<verb>_enabled`
properties (so scripts can check whether `win undo` would be a
no-op before invoking) land if a use case names them.

### RunContext masks

All 48 verbs share the same mask:

| Verb category    | TestRunner | Scripter | Macro |
|------------------|:----------:|:--------:|:-----:|
| All 48 win verbs | ✓          | ✓        | —     |

Macro is excluded uniformly. The two readback properties
(`toggle-rulers_state`, `toggle-outline_state`) are queries — not
dispatcher-gated in their query form, but the matching invoke
form for the stateful bool verb IS masked per the row above.

### Examples

Undo / redo round-trip after a real mutation:

```
objects new rect
proj dirty
= true
win undo
proj dirty
= false
win redo
proj dirty
= true
```

Selection sweep before a boolean op:

```
win select-all
win bool-union
assert objects count == 1
```

Toggle outline mode and read it back:

```
get win toggle-outline_state
win toggle-outline
get win toggle-outline_state
win toggle-outline false
```

Expected trace:

```
> get win toggle-outline_state
= false
> win toggle-outline
ok
> get win toggle-outline_state
= true
> win toggle-outline false
ok
```

Refusal — calling a destructive verb from Macro context:

```
> win undo
error invoke refused: verb 'undo' on 'win' not permitted in macro context
```

Unknown-verb refusal — wrap-now is a SUBSET of all menu actions;
bucket-B actions (`win new`, `win open`, `win save`, etc.) are
**not on this Scriptable** because their canonical surface is
the corresponding model Scriptable:

```
> win save
error invoke threw: win save: unknown verb (not in wrap-now subset; see tier2_action_audit.md)
```

Use `proj save` instead — the bucket-B alternative.

## ❷ Substrate widgets in the headerbar zone

A handful of widgets in the headerbar and the status-bar / tab
strip are catalogued in `widget_names.db` for `widget_names_sync`
parity, but most are **not** `ScriptableWidget` subclasses and
therefore not directly script-addressable. They show up in the
visual-ID catalogue because the names are referenced from
hand-written XML / tooltips / inspector overlays.

The full set, with the one script-addressable widget called out:

### Script-addressable

- **`mw_scripter`** — the "monkey button" in the headerbar that
  toggles the Scripter window's visibility. A
  `curvz::widgets::ToggleButton` (substrate). Verbs `click` (no
  args, flips current state) and `set <bool>` (sets explicitly);
  read-only properties `active` (Bool — true iff toggled on) and
  `label` (String).

  ```
  mw_scripter click
  mw_scripter set true
  get mw_scripter active
  ```

  The button's visibility is governed by
  `AppPreferences::scripter_enabled` — if the pref is off, the
  button is hidden but still registered (so `mw_scripter toggle`
  still works from a script regardless of the headerbar's visual
  state).

### Naming-only (not script-addressable)

These widgets have entries in `widget_names.db` for visual-ID
sync, but they're plain `Gtk::Box` / `Gtk::Label` / `Gtk::Button`
subclasses — no Scriptable mixin, no registry entry, no verbs.
Listed here so scripts trying `<id> <verb>` against these names
get a clear "this isn't a thing" diagnosis rather than guessing.

**Headerbar:**
- `mw_hb` — the headerbar root box.
- `mw_logo` — the Curvz logo button (opens About dialog).
- `mw_ham` — the hamburger menu button.

**Doc tab strip (centre of headerbar):**
- `dt` — the tab strip's root box.
- `dt_l` — scroll-left button (visible when tabs overflow).
- `dt_r` — scroll-right button.

**Status bar (bottom of window):**
- `sb` — status bar root.
- `sb_pos` — cursor X / Y position readout.
- `sb_zm` — current zoom readout.
- `sb_un` — current document units readout.
- `sb_cnt` — selection count readout.
- `sb_mod` — current tool / mode readout.
- `sb_prj` — project path readout.
- `sb_doc` — active document name readout.

**Ruler corner:**
- `mw_crn` — the corner square at the ruler intersection.
- `mw_hr` — the horizontal ruler.
- `mw_vr` — the vertical ruler.

(These could become substrate widgets in a future arc, if a use
case names the need — readouts as queryable properties would be
useful for end-to-end tests that want to assert on what the user
sees. Tracked informally; not a current backlog item.)

## What's not here

Three categories of action are **deliberately absent** from the
`win` surface — by audit-bucket classification, not by oversight.
Scripts trying to invoke them get the unknown-verb refusal above.

**Bucket B — already reachable via a model Scriptable.** The
canonical surface is the model Scriptable; wrapping the action
would put a second way to do each thing in the dispatcher.

- File menu verbs (`new`, `open`, `save`, `save-as`, `close`,
  `recent-N`, etc.) — use `proj` instead. See **Singletons
  reference**.
- Export menu verbs (`export-svg`, `export-png`, `export-theme`)
  — use `export` instead.
- Object menu verbs (`group`, `ungroup`) — use `objects` instead.
- Themes I/O verbs — use the `themes` collection.
- Styles panel actions, swatches panel actions — use the
  corresponding panel Scriptable (`pnl_styles`) or collection
  router (`styles`, `swatches`).

**Bucket C — unsafe / launcher / dead.** Wrapper would be a
worse surface than the alternative or the action is
script-hostile.

- Dialog-launchers (`win.print`, `win.offset-path`,
  `win.place-image`, `win.show-shortcuts`, `win.show-help`,
  `win.view-clipboard`, etc.) — the script wants the operation,
  not the UI that configures it. Future parameterised verbs on
  selection / `objects` / `app` Scriptables will subsume these.
- `win.warp-edit` / `win.warp-flatten` — slated for deprecation
  with the planned inspector-Warp-section work; don't wrap a
  deprecated surface.
- Destructive without granular control (`recents.clear`).
- `win.quit` — application quit; a future `app.quit` is the
  right surface if scripted quit ever makes sense.
- `win.toggle-scripting` — a script disabling scripting from
  within a script is self-defeating; refusal-only.

**Bucket D — deferred by design.** Would belong in the wrap-now
subset once the right Scriptable exists.

- `win.translate-dialog` — awaiting the selection-Scriptable
  that would receive a parametric `translate dx dy` verb.

See `tier2_action_audit.md` for the full classification and the
forks that remain open.
