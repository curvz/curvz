# ![Scripter icon](img/icons/face-monkey-symbolic.svg) Toolbar reference

Covers the **main toolbar** (left edge of the Curvz window) and its
fifteen **right-click default popovers** — the surfaces a script
uses to drive tools, defaults, and shape-creation parameters.

The toolbar's script surface is mostly substrate widgets — each
widget addresses by its own name from the registry (`tb_sel`,
`pop_tb_rec_w`, etc.) and accepts the canonical verbs for its
widget kind (`click`, `set`, `pick`, `step`, `parse`). There is
no toolbar-scope Scriptable that wraps them collectively — the
widgets themselves ARE the surface.

Parallels manual chapter **4. Toolbar**.

## Substrate widget verb surfaces

Every substrate widget on the toolbar follows the kind contracts
defined by its base class. The same vocabulary repeats across all
toolbar widgets, so it's listed once here rather than per-widget
below.

- **Button** (`curvz::widgets::Button`) — verb `click` (no args,
  uses signal-bound synchronizer); property `label`.
- **ToggleButton** (`curvz::widgets::ToggleButton`) — verbs
  `click` (flip current state, synchronized) and `set <bool>`
  (explicit; synchronous); properties `active` and `label`.
- **CheckButton** (`curvz::widgets::CheckButton`) — verbs
  `click` (synchronous toggle) and `set <bool>`; properties
  `active` and `label`. When a CheckButton is in a radio group,
  `set true` exclusively activates it and deactivates siblings.
- **SpinButton** (`curvz::widgets::SpinButton`) — verbs `set
  <num>` (set the value directly), `step <int>` (step
  forward / back N times; sign chooses direction), and `parse
  <str>` (route through the field's text-parse path, returns
  Bool); properties `value`, `min`, `max`.
- **DropDown** (`curvz::widgets::DropDown`) — verbs `set <int>`
  (by index) and `pick <str>` (by display text); properties
  `selected`, `text`, `count`.

Verbs that map to a GTK event (`click` on Button/ToggleButton)
go through the synchronizer — the verb doesn't return until the
canonical signal fires. Direct setters (`set` on SpinButton,
CheckButton, DropDown) are synchronous in the calling thread.

The default RunContext mask for every substrate widget is
`all_three` (TestRunner, Scripter, Macro). Scripts in any caller
context can drive any substrate widget unless its host has wired
a custom mask — none of the toolbar widgets do.

## ❶ Main toolbar tool buttons

Fifteen `ToggleButton`s, one per active tool. The widgets sit in
a radio group — `click` cycles the active tool to this button's
mode; `set false` doesn't quite work the way you might expect
(the radio invariant forces one tool to stay active, so the last
button left active will stay active).

The full list, in toolbar-layout order:

| Abbrev    | Tool                | Hotkey |
|-----------|---------------------|:------:|
| `tb_sel`  | Selection           | S      |
| `tb_nod`  | Node                | N      |
| `tb_pen`  | Pen                 | P      |
| `tb_rec`  | Rectangle           | R      |
| `tb_ova`  | Ellipse             | E      |
| `tb_lin`  | Line                | L      |
| `tb_ref`  | Reference point     | F      |
| `tb_txt`  | Text                | T      |
| `tb_top`  | Text on Path        | U      |
| `tb_pol`  | Polygon / Star      | G      |
| `tb_spi`  | Spiral              | W      |
| `tb_cor`  | Corner Treatment    | K      |
| `tb_eyd`  | Eyedropper          | I      |
| `tb_meas` | Measure             | M      |
| `tb_zom`  | Zoom                | Z      |

Each accepts the **ToggleButton** verbs and properties as
listed above.

### Examples

Switch to the rectangle tool, confirm, then back to selection:

```
tb_rec click
assert tb_rec active == true
tb_sel click
assert tb_sel active == true
assert tb_rec active == false
```

Use `set true` for an explicit (non-toggling) selection. Setting
`set false` on an already-active tool button is effectively a
no-op — the radio handler in the toolbar will re-activate it on
the next paint cycle to maintain the radio invariant.

## ❷ Bool op picker

The toolbar's transforms section includes an **Align** popover
and a **Bool op** popover. The Bool op picker contains three
substrate Buttons that emit the matching `BoolOp` and update the
toolbar's bool-icon to reflect "last picked op":

- **`tb_bop_un`** — Union button. Same as `win bool-union`.
- **`tb_bop_sb`** — Subtract button. Same as `win bool-subtract`.
- **`tb_bop_in`** — Intersect button. Same as `win bool-intersect`.

The popover root itself is **`pop_tb_bop`** (plain Gtk::Popover,
not script-addressable; the three button children are).

For most scripts the corresponding `win` verbs are the better
surface — they fire the operation directly without going through
the picker's "set last-icon" side effect. Use the substrate
buttons only when explicitly testing the picker's wiring.

## ❸ Macro button

- **`tb_mb`** — substrate Button next to the snap toggle, shown
  in the toolbar's tail cluster. Activates the macro-record
  toggle behind the scenes. Single verb `click`; no settable
  state from script.

## ❹ Toolbar density popover

Right-clicking on toolbar background (not on a tool button) pops
the density picker — the "in your face" control for changing
toolbar button sizing.

Popover root: **`pop_tb_density`**.

Four `CheckButton`s in a radio group:

- **`pop_tb_dn_com`** — Comfortable (default).
- **`pop_tb_dn_std`** — Standard.
- **`pop_tb_dn_cpt`** — Compact.
- **`pop_tb_dn_tgt`** — Tight.

### Example

```
pop_tb_dn_cpt set true
assert pop_tb_dn_cpt active == true
assert pop_tb_dn_com active == false
```

## ❺ Fill defaults popover

Default fill applied to newly-created objects. Popover root:
**`pop_tb_fill`**.

The popover offers four mutually-exclusive **fill types** (radio
group of CheckButtons), and — when Solid or Gradient is picked —
inputs for the color or gradient.

Fill-type radio:

- **`pop_tb_fill_non`** — None.
- **`pop_tb_fill_cc`** — currentColor.
- **`pop_tb_fill_sol`** — Solid.
- **`pop_tb_fill_grd`** — Gradient.

Solid-color inputs (visible when Solid is active):

- **`pop_tb_fill_ch`** — color-swatch drawing area (read-only
  visual; not interactive).
- **`pop_tb_fill_hex`** — hex-entry field for the color (plain
  Gtk::Entry, not script-addressable today; the swatch popover
  is the standard write path).
- **`pop_tb_fill_sw`** — open the swatch picker.

Gradient inputs (visible when Gradient is active):

- **`pop_tb_fill_grm`** — gradient-ramp drawing area.
- **`pop_tb_fill_ged`** — Edit Gradient button.

Other:

- **`pop_tb_fill_pdd`** — Pick from Document drop-down.
- **`pop_tb_fill_cf`** — flow toggle for chip layout.

## ❻ Stroke defaults popover

Default stroke applied to newly-created objects. Popover root:
**`pop_tb_strk`**. Largest single popover — 16 named widgets.

Stroke-type radio (mirrors fill):

- **`pop_tb_strk_non`** — None.
- **`pop_tb_strk_cc`** — currentColor.
- **`pop_tb_strk_sol`** — Solid.

Solid-color inputs (visible when Solid is active):

- **`pop_tb_strk_ch`** — color swatch drawing area.
- **`pop_tb_strk_hex`** — hex-entry field.
- **`pop_tb_strk_sw`** — open swatch picker.

Width / dash:

- **`pop_tb_strk_w`** — stroke width SpinButton.
- **`pop_tb_strk_pdd`** — Pick from Document drop-down.

Cap-style radio:

- **`pop_tb_strk_cb`** — Butt cap.
- **`pop_tb_strk_cr`** — Round cap.
- **`pop_tb_strk_cs`** — Square cap.

Join-style radio:

- **`pop_tb_strk_jb`** — Bevel join.
- **`pop_tb_strk_jr`** — Round join.
- **`pop_tb_strk_jm`** — Miter join.

Other:

- **`pop_tb_strk_cf`** — flow toggle for chip layout.

(Note: the currentColor toggle and the Butt-cap button have
similar-looking abbrevs — `strk_cc` and `strk_cb` — but they're
distinct widgets at distinct popover locations. They're listed
in their respective radio groups above.)

### Example — set the default stroke to 2 px solid

```
pop_tb_strk_sol set true
pop_tb_strk_w set 2.0
assert pop_tb_strk_w value == 2.0
```

## ❼ Shape-defaults popovers — Rectangle / Ellipse / Line / Reference

Right-click on each shape-creation tool button pops a "create
precisely" popover that sets default position and size for a
new shape, then places it. The four shapes share an identical
shape pattern: X / Y position SpinButtons, width / height (or
endpoint) SpinButtons, and a **Place** Button that commits the
creation.

### Rectangle popover

Root: **`pop_tb_rec`**.

- **`pop_tb_rec_x`** — X position SpinButton.
- **`pop_tb_rec_y`** — Y position SpinButton.
- **`pop_tb_rec_w`** — width SpinButton.
- **`pop_tb_rec_h`** — height SpinButton.
- **`pop_tb_rec_pl`** — Place Button (commits the creation).

### Ellipse popover

Root: **`pop_tb_ova`**.

- **`pop_tb_ova_x`** — X position.
- **`pop_tb_ova_y`** — Y position.
- **`pop_tb_ova_w`** — width (bounding box).
- **`pop_tb_ova_h`** — height (bounding box).
- **`pop_tb_ova_pl`** — Place Button.

### Line popover

Root: **`pop_tb_lin`**.

- **`pop_tb_lin_x1`** — first endpoint X.
- **`pop_tb_lin_y1`** — first endpoint Y.
- **`pop_tb_lin_x2`** — second endpoint X.
- **`pop_tb_lin_y2`** — second endpoint Y.
- **`pop_tb_lin_pl`** — Place Button.

### Reference point popover

Root: **`pop_tb_ref`**.

- **`pop_tb_ref_x`** — X position.
- **`pop_tb_ref_y`** — Y position.
- **`pop_tb_ref_pl`** — Place Button.

### Example — place a precise rectangle

```
pop_tb_rec_x set 100
pop_tb_rec_y set 50
pop_tb_rec_w set 200
pop_tb_rec_h set 150
pop_tb_rec_pl click
```

The `click` on `pop_tb_rec_pl` commits a rectangle at the named
coordinates and closes the popover.

## ❽ Polygon popover

Polygon / Star configuration AND placement. Two more inputs than
the other shape popovers because polygons have a side-count and
inner-radius (star) toggle.

Root: **`pop_tb_pol`**.

Geometry inputs:

- **`pop_tb_pol_cx`** — centre X SpinButton.
- **`pop_tb_pol_cy`** — centre Y SpinButton.
- **`pop_tb_pol_r`** — outer radius SpinButton.
- **`pop_tb_pol_sd`** — sides SpinButton (integer).
- **`pop_tb_pol_in`** — inner radius SpinButton (only relevant
  in star mode).

Mode + placement:

- **`pop_tb_pol_pv`** — preview drawing area (visual only).
- **`pop_tb_pol_pl`** — Place Button.

## ❾ Spiral popover

Spiral configuration AND placement.

Root: **`pop_tb_spi`**.

- **`pop_tb_spi_cx`** — centre X.
- **`pop_tb_spi_cy`** — centre Y.
- **`pop_tb_spi_r`** — outer radius.
- **`pop_tb_spi_ir`** — inner radius (start of the spiral).
- **`pop_tb_spi_tn`** — number of turns.
- **`pop_tb_spi_pv`** — preview drawing area.
- **`pop_tb_spi_pl`** — Place Button.

## ❿ Text popover

Default font, size, and style applied to new text objects.

Root: **`pop_tb_txt`**.

- **`pop_tb_txt_fam`** — Font family DropDown.
- **`pop_tb_txt_sz`** — Font size SpinButton.
- **`pop_tb_txt_b`** — Bold CheckButton.
- **`pop_tb_txt_i`** — Italic CheckButton.
- **`pop_tb_txt_an`** — Text-anchor DropDown (start / middle /
  end).
- **`pop_tb_txt_x`** — anchor X.
- **`pop_tb_txt_y`** — anchor Y.
- **`pop_tb_txt_pl`** — Place Button.

### Example — set defaults for new text

```
pop_tb_txt_fam pick "DejaVu Sans"
pop_tb_txt_sz set 24
pop_tb_txt_b set true
```

The next text object created (by clicking on canvas with the
text tool active) inherits these defaults.

## ⓫ Align popover

Same six align verbs and two distribute verbs as `win.align-*` /
`win.distribute-*`, exposed as buttons in the toolbar's transforms
align dropdown.

Root: **`pop_tb_align`**.

- **`pop_tb_align_l`** — Align left.
- **`pop_tb_align_r`** — Align right.
- **`pop_tb_align_ch`** — Align centre horizontal.
- **`pop_tb_align_t`** — Align top.
- **`pop_tb_align_b`** — Align bottom.
- **`pop_tb_align_cv`** — Align centre vertical.
- **`pop_tb_align_dh`** — Distribute horizontal.
- **`pop_tb_align_dv`** — Distribute vertical.

Each is a substrate Button with `click` as its only verb. For
scripts the `win align-left` / `win distribute-h` verbs are the
direct surface — these buttons exist for visual-driver scripts
that exercise the toolbar's clickable UI explicitly.

## ⓬ Snap settings popover

The snap toggle's right-click popover. Six CheckButtons covering
which classes of target snap-to is active for.

Root: **`pop_tb_snap`**.

- **`pop_tb_snap_g`** — Snap to guides.
- **`pop_tb_snap_gr`** — Snap to grid.
- **`pop_tb_snap_m`** — Snap to margins.
- **`pop_tb_snap_n`** — Snap to nodes.
- **`pop_tb_snap_e`** — Snap to edges.
- **`pop_tb_snap_c`** — Snap to centres.

Each is independently toggleable (no radio group; you can have
several snap classes active simultaneously).

### Example — guide-only snap

```
pop_tb_snap_g set true
pop_tb_snap_gr set false
pop_tb_snap_m set false
pop_tb_snap_n set false
pop_tb_snap_e set false
pop_tb_snap_c set false
```

## ⓭ Measure tool popover

Two CheckButtons controlling measurement persistence and a
counter widget showing measurement count.

Root: **`pop_tb_meas`**.

- **`pop_tb_meas_sv`** — Save measurements CheckButton.
- **`pop_tb_meas_dc`** — Delete on close CheckButton.

## ⓮ Zoom popover

Right-click on the Zoom tool button. Set zoom level numerically,
or pick a preset.

Root: **`pop_tb_zom`**.

- **`pop_tb_zom_spn`** — zoom-level SpinButton (percent).
- **`pop_tb_zom_apl`** — Apply Button (set zoom to spinner
  value).
- **`pop_tb_zom_fit`** — Fit Button (same as `win zoom-fit`).
- **`pop_tb_zom_p25`** — 25% preset.
- **`pop_tb_zom_p50`** — 50% preset.
- **`pop_tb_zom_p100`** — 100% preset.
- **`pop_tb_zom_p200`** — 200% preset.
- **`pop_tb_zom_p400`** — 400% preset.

### Example — zoom to 150% precisely

```
pop_tb_zom_spn set 150
pop_tb_zom_apl click
```

## Plain widgets in this zone

Catalogued in `widget_names.db` for visual-naming sync but
**not** `ScriptableWidget` subclasses — no verbs, no registry
entry. Scripts trying `<id> <verb>` against these names get an
"unknown object" refusal.

- **`tb`** — the toolbar root container.
- **`tb_ab`** — Align dropdown header button (plain
  `Gtk::Button`; the popover it opens contains substrate
  buttons; see §⓫).
- **`tb_ab_icon`** — the align button's icon child.
- **`tb_ss`** — Snap toggle button (plain `Gtk::ToggleButton`;
  the settings popover it opens contains substrate widgets; see
  §⓬).
- **`tb_well`** — the defaults well container (fill/stroke
  chips area).

These could become substrate widgets in a future arc if a use
case names the need.

## What's not here

A handful of toolbar-adjacent operations don't appear above
because they belong to other Scriptables:

- **The actual align / distribute / zoom / boolean operations**
  — addressed via the `win` Scriptable (see **Header & menus
  reference**). The toolbar popovers above are the GUI surface;
  `win align-left` etc. are the direct semantic verbs.
- **Tool defaults that affect new-object styling** —
  fill / stroke defaults landed via `pop_tb_fill_*` /
  `pop_tb_strk_*` apply to the next-created object on this run;
  persistent defaults live in `AppPreferences` (no script
  surface yet — a future `app` verb could expose them).
- **Macros recorded by `tb_mb`** — the macro panel proper is in
  the Inspector (planned for the **Inspector reference**).
