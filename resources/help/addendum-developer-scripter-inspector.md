# ![Scripter icon](img/icons/face-monkey-symbolic.svg) Inspector reference

Covers the right-edge **Inspector panel** and every script-addressable
widget inside it. The Inspector has no umbrella Scriptable —
there is no `inspector` wrapper whose verbs collapse the whole
panel's worth of controls. Scripts address each substrate widget
individually by name (`ins_sel_fh`, `ins_can_un`, etc.), and use
`inspector open "<title>"` from the `inspector` singleton (see
**Singletons reference**) to ensure the relevant section is open
before driving its widgets.

Parallels manual chapter **5. Inspector**.

## Substrate widget verb surfaces

The Inspector uses six substrate widget kinds. The vocabulary
repeats across every widget below; one place to learn the
contracts rather than per-widget.

- **Button** — verb `click` (signal-bound synchronizer); property
  `label`.
- **ToggleButton** — verbs `click` (flip current state) and
  `set <bool>` (explicit); properties `active`, `label`.
- **CheckButton** — same shape as ToggleButton (`click`,
  `set <bool>`; `active`, `label`). In a radio group, `set true`
  exclusively activates and deactivates siblings.
- **SpinButton** — verbs `set <num>`, `step <int>` (sign chooses
  direction), `parse <str>` (returns Bool); properties `value`,
  `min`, `max`.
- **DropDown** — verbs `set <int>` (by index), `pick <str>` (by
  display text); properties `selected`, `text`, `count`.
- **Scale** — verb `set <num>`; properties `value`, `min`, `max`.
  Same shape as SpinButton minus `step` and `parse`.
- **Entry** — verbs `set <str>`, `clear` (set to empty);
  property `text`.

Every Inspector substrate widget runs under the default mask
`all_three` (TestRunner, Scripter, Macro). Setters are
synchronous; `click` on Button / ToggleButton goes through the
signal synchronizer.

## Opening sections

Section titles registered for `inspector open`:

`"Selection"`, `"Text"`, `"Node"`, `"Blend"`, `"Warp"`,
`"Shadow"`, `"Editing"`, `"Paths"`, `"Margins"`, `"Guides"`,
`"Grid"`, `"Canvas"`, `"Dimensions"`, `"Styling"`, `"Theme"`,
`"Metadata"`, `"Boolean cleanup"`, `"Startup"`, `"Appearance"`,
`"Developer"`.

The Inspector orchestrates these across two registries (the
right-panel section list and PropertiesPanel's own collapsibles)
— `inspector open "<title>"` cascades into whichever registry
holds the named section. Unknown titles are silent no-ops.

Several titles appear more than once in the Inspector at
different scopes (`"Selection"` and `"Text"` both have
per-object-type variants; `"Guides"` appears once under Object
and once under Document; etc.). `inspector open` opens whichever
section is registered first for the named title; for unambiguous
addressing, drive the substrate widgets directly.

`inspector collapse_all` closes every section across both
registries.

## ❶ Selection

Geometric controls on the current selection — flip, scale,
rotate, skew. Eleven substrate widgets.

### Flips

- **`ins_sel_fh`** — Button. Flip selection horizontally.
- **`ins_sel_fv`** — Button. Flip selection vertically.
- **`ins_sel_img_fh`** — Button. Flip image-mode horizontal (for
  image selections; same operation but routed through the image
  selection sub-section).
- **`ins_sel_img_fv`** — Button. Flip image-mode vertical.
- **`ins_sel_ref_fh`** — Button. Flip reference-point selection
  horizontal.
- **`ins_sel_ref_fv`** — Button. Flip reference-point selection
  vertical.

### Scale / rotate / skew

- **`ins_sel_sa`** — Button. Scale apply.
- **`ins_sel_sln`** — ToggleButton. Scale-link toggle (preserve
  aspect when scaling).
- **`ins_sel_rt`** — SpinButton. Rotation angle (degrees).
- **`ins_sel_ra`** — Button. Rotate apply.
- **`ins_sel_ka`** — Button. Skew apply.

For most scripts the `win flip-horizontal` / `win flip-vertical`
verbs (see **Header & menus reference**) are equivalent to the
Selection-section flip buttons — the inspector buttons exist as
the GUI surface, and clicking them dispatches the same
operations. Scale, rotate, and skew don't have `win` equivalents
today; the inspector apply-buttons ARE the script surface.

### Example — rotate selection 45°

```
inspector open "Selection"
ins_sel_rt set 45
ins_sel_ra click
```

## ❷ Text

Controls visible when a text or text-on-path object is selected.
Ten substrate widgets.

### Style toggles

- **`ins_txt_b`** — CheckButton. Bold.
- **`ins_txt_i`** — CheckButton. Italic.

### Alignment radio

- **`ins_txt_al`** — ToggleButton. Align left.
- **`ins_txt_ac`** — ToggleButton. Align centre.
- **`ins_txt_ar`** — ToggleButton. Align right.
- **`ins_txt_aj`** — ToggleButton. Justify.

### Content

- **`ins_txt_ct`** — Entry. The text content of the selected
  object. `ins_txt_ct set "Hello"` writes new text into the
  selection.
- **`ins_txt_fam`** — DropDown. Font family.

### Text-on-path

- **`ins_txt_pfp`** — CheckButton. Flip on path (mirror text
  along the path).
- **`ins_txt_dt`** — Button. Detach from path.

### Example — change selected text and bold it

```
inspector open "Text"
ins_txt_ct set "Curvz"
ins_txt_b set true
ins_txt_fam pick "DejaVu Sans"
```

## ❸ Fill / Stroke

The Fill/Stroke section has heavy color-picker UI (drawing areas,
hex entries, gradient ramps) — most of which are plain widgets,
not substrate. The six substrate widgets are the stroke
cap and join style radios:

### Cap-style radio

- **`ins_fs_strk_cb`** — ToggleButton. Butt cap.
- **`ins_fs_strk_cr`** — ToggleButton. Round cap.
- **`ins_fs_strk_cs`** — ToggleButton. Square cap.

### Join-style radio

- **`ins_fs_strk_jb`** — ToggleButton. Bevel join.
- **`ins_fs_strk_jr`** — ToggleButton. Round join.
- **`ins_fs_strk_jm`** — ToggleButton. Miter join.

For setting fill / stroke colours and gradients, the canonical
script surface is `styles` and `swatches` collections (see the
future **Content reference** — pending). The Inspector's
color-picker widgets are GUI-only; scripts work at the model
level.

## ❹ Node

Visible when the Node tool is active. Five substrate widgets.

- **`ins_nod_tp`** — DropDown. Node type (smooth / cusp /
  symmetric / etc.).
- **`ins_nod_cl`** — ToggleButton. Path closed (toggles whether
  the selected path is closed or open).
- **`ins_nod_op`** — Button. Open here (break the path at the
  selected node).
- **`ins_nod_rv`** — Button. Reverse path direction.
- **`ins_nod_sp`** — Button. Split path at the selected node
  into two paths.

### Example — open a path at the selected node

```
inspector open "Node"
ins_nod_op click
```

(Requires a Node-tool selection — without one the button is
disabled and `click` silently no-ops.)

## ❺ Blend

Visible when a Blend object is selected. Four substrate widgets.

- **`ins_blnd_st`** — SpinButton. Step count between blend
  source A and B.
- **`ins_blnd_rv`** — CheckButton. Reverse direction.
- **`ins_blnd_so`** — CheckButton. Stroke override (use blend
  stroke instead of A/B's).
- **`ins_blnd_rl`** — Button. Release blend.

The blend's source A/B addressing is on the **`objects`**
collection (Canvas & objects reference, pending) — the Inspector
controls only the parameters of the blend itself, not its
sources.

## ❻ Warp

Visible when a Warp object is selected. Six substrate widgets.

- **`ins_wrp_pr`** — DropDown. Preset (Wave / Bulge / Pinch /
  custom).
- **`ins_wrp_q`** — Scale. Quality (subdivision density).
- **`ins_wrp_tn`** — SpinButton. Top envelope node count.
- **`ins_wrp_bn`** — SpinButton. Bottom envelope node count.
- **`ins_wrp_fl`** — Button. Flatten the warp (bake into source
  geometry).
- **`ins_wrp_rl`** — Button. Release warp (revert to source).

(The Edit Warp dialog flagged for deprecation in the backlog is
**not** here — when the inspector-driven envelope handles ship,
that's where they'll live; until then the section reads as
warp-config only.)

## ❼ Shadow

Visible per-object when a drop-shadow effect is enabled. Three
substrate widgets.

- **`ins_shdw_en`** — CheckButton. Enable shadow.
- **`ins_shdw_op`** — Scale. Shadow opacity (0..1).
- **`ins_shdw_ca`** — Scale. Shadow colour alpha (0..1; same
  channel as opacity but on the colour swatch).

The shadow's blur radius, offset X / Y, and colour aren't
substrate (plain widgets); the model-level surface for these
will land when an Object-level shadow API ships.

## ❽ Object → Guides

A "Guides" sub-section under the Object group (distinct from
the Document-scope Guides section in §❿ below) — these buttons
operate on a guide associated with the **currently selected
object**, not on a free-standing document guide. Four substrate
widgets.

- **`ins_obj_gd_f2p`** — Button. Make guides from 2 points
  (derive a pair of guides from the current 2-point selection).
- **`ins_obj_gd_del`** — Button. Delete the guide associated
  with the selected object.
- **`ins_obj_gd_lock`** — Button. Lock the guide associated
  with the selected object.
- **`ins_obj_gd_mdel`** — Button. Multi-delete guides on the
  selection.

These overlap with `guides` collection verbs (Content reference,
pending). The inspector buttons act on the **current selection's
associated guide** specifically; `guides` collection verbs work
by guide-iid regardless of selection.

(Heads-up: the title "Guides" appears for both this Object-scope
section AND the Document-scope Guides section below. `inspector
open "Guides"` opens whichever is registered first — generally
the visible one for the current context. If both are visible
simultaneously, behaviour is implementation-defined; use the
substrate widgets directly to disambiguate.)

## ❾ Margins

Page margins (the printable region within the canvas). Three
substrate widgets.

- **`ins_mrg_en`** — CheckButton. Enable margins.
- **`ins_mrg_cn`** — SpinButton. Column count.
- **`ins_mrg_rn`** — SpinButton. Row count.

(Margin distances per side are on the document model — plain
widgets in the Inspector; future `margins` Scriptable would
expose them.)

## ❿ Grid

Document grid configuration. Two substrate widgets.

- **`ins_grd_en`** — CheckButton. Enable grid.
- **`ins_grd_st`** — DropDown. Grid style (Dot / Line /
  Crosshatch / etc.).

## ⓫ Canvas (Dimensions)

The document's geometric properties — units, orientation, size.
Six substrate widgets.

- **`ins_can_un`** — DropDown. Document units (px / in / mm /
  pt / cm).
- **`ins_can_op`** — Button. Set orientation portrait.
- **`ins_can_ol`** — Button. Set orientation landscape.
- **`ins_can_sw`** — SpinButton. Document width (in current
  units).
- **`ins_can_sh`** — SpinButton. Document height (in current
  units).
- **`ins_can_sl`** — ToggleButton. Size lock (preserve aspect
  ratio when editing width or height).

### Example — set up a 1024×768 landscape document

```
inspector open "Dimensions"
ins_can_un pick "px"
ins_can_ol click
ins_can_sl set false
ins_can_sw set 1024
ins_can_sh set 768
```

(Aspect-lock disabled first so width and height can be set
independently to non-matching values.)

The DPI / ratio / pixel size sub-fields aren't substrate (plain
SpinButtons); they're driven by the same model that `ins_can_sw`
and `ins_can_sh` write to, so setting size in current units
flows through to the pixel readouts automatically.

## ⓬ Theme (formerly Motif)

The colour-theme selector for the active document. One substrate
widget — the rest of the section is colour swatches (plain).

- **`ins_motif_rst`** — Button. Reset theme to defaults.

For applying themes by name, the canonical surface is the
**`themes`** collection (Content reference, pending). The
inspector's Theme section is a per-document quick-picker; the
collection is the cross-document admin surface.

## ⓭ Metadata

Per-document metadata. One substrate widget.

- **`ins_meta_xc`** — DropDown. Export category (the icon-theme
  bundle's freedesktop category — for export-as-icon-theme
  workflows). Same value the `export theme <dir>` verb writes
  into the bundle's `index.theme`.

## ⓮ App preferences

Application-level prefs visible when no document is open (or
when the App group is expanded in the Inspector tail). Four
substrate widgets.

- **`ins_app_undo_depth`** — SpinButton. Undo history depth
  (number of commands kept on the stack).
- **`ins_app_recents_max`** — SpinButton. Maximum number of
  recent projects to remember.
- **`ins_app_tooltip_delay`** — SpinButton. Tooltip delay (ms).
- **`ins_app_bcq`** — Scale. Boolean-op cleanup quality (the
  threshold for the post-boolean-op smoothing pass; higher
  values smooth more aggressively).

These persist via `AppPreferences` on next launch.

## Plain widgets in the Inspector

The Inspector also has **~80 named plain widgets** —
`Gtk::Label`, `Gtk::Image`, drawing areas, frames, and similar.
They're catalogued in `widget_names.db` for inspector-overlay
diagnostics, but they're **not** `ScriptableWidget` subclasses
— they have no verbs, no registry entry, no script surface.
Trying `<plain_id> <verb>` from a script returns an unknown-
object refusal.

The full list is too long to enumerate here; check the
`widget_names.db` rows whose abbrev starts with `ins_` and whose
long-name suggests a label / image / static control (e.g.
`inspector_blend_ab_nodes_lbl` — a label, not a control).

## What's not here

A handful of Inspector-adjacent operations belong to other
Scriptables and don't appear above:

- **Fill / stroke colour as a value** — use `styles` and
  `swatches` collections (Content reference, pending) for
  setting colour values; the Inspector's colour pickers are
  GUI-only.
- **Style application by name** — `styles.<iid> apply` on the
  styles collection.
- **Theme application by name** — `themes.<iid> apply` on the
  themes collection.
- **Per-blend / per-warp source addressing** — `objects`
  collection by iid.
- **Selection-on-canvas membership changes** — coming on the
  selection-Scriptable when it lands (bucket D in the Tier 2
  audit).
- **`inspector open` / `inspector collapse_all`** — on the
  `inspector` singleton (Singletons reference).
