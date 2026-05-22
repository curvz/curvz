# ![Scripter icon](img/icons/face-monkey-symbolic.svg) Dialogs & popovers reference

The **modal-dialog and popover** surface — every transient
window or popover Curvz raises in response to a menu pick,
toolbar long-press, or context-menu item. Like the Toolbar
and Inspector references, this is a **substrate-only** page:
there's no umbrella `dialogs` Scriptable — the dialogs'
substrate widgets ARE the script surface, addressed by the
names they register with `set_name`.

Nineteen dialog / popover surfaces, **204 substrate
widgets** total. Counts vary from 1 (RotateFromPointDialog,
ImageInfoDialog) to 30+ (ExportDialog, ThemeEditDialog).

A dialog is only addressable **while it is open**. Each
dialog's substrate widgets register their names on
construction; the names disappear from the resolver when the
dialog is destroyed. A script that wants to drive a dialog
opens it first via the relevant menu action (`win.<verb>` —
see **Header & menus reference**) or toolbar long-press,
then addresses the substrate widgets, then closes via the
dialog's own Cancel / OK / Close button.

## Conventions on this page

Same widget-kind vocabulary as the Toolbar and Inspector
references. Each substrate widget is one of these eight
kinds:

- **Button** (`curvz::widgets::Button`) — verb `click` (no
  args, signal-bound); property `label`.
- **ToggleButton** (`curvz::widgets::ToggleButton`) — verbs
  `click` (flip; signal-bound) and `set <bool>` (explicit;
  synchronous); properties `active`, `label`. Radio-grouped
  ToggleButtons follow the "set true exclusively activates"
  rule.
- **CheckButton** (`curvz::widgets::CheckButton`) — verbs
  `click` (synchronous toggle) and `set <bool>`; properties
  `active`, `label`. Inspector and dialog CheckButtons are
  the same kind despite the visual difference (CheckButton
  in inspector, Switch-style on some dialogs); the script
  surface is identical.
- **SpinButton** (`curvz::widgets::SpinButton`) — verbs
  `set <num>`, `step <int>`, `parse <str>` (returns Bool);
  properties `value`, `min`, `max`.
- **DropDown** (`curvz::widgets::DropDown`) — verbs
  `set <int>` (by index) and `pick <str>` (by display text);
  properties `selected`, `text`, `count`.
- **Entry** (`curvz::widgets::Entry`) — verbs `set <str>`
  (write text, fires signal) and `clear` (set to empty);
  property `text`.
- **Scale** (`curvz::widgets::Scale`) — verb `set <num>`;
  properties `value`, `min`, `max`. Same surface as
  SpinButton minus the step/parse verbs.
- **RefPointPicker** (`curvz::widgets::RefPointPicker`) —
  verbs `set_checkbox <bool>`, `set_preset <token>`,
  `set_arbitrary <x> <y>`, `set_bbox <x> <y> <w> <h>`;
  properties `mode`, `preset`, `x`, `y`, `bbox_x`,
  `bbox_y`, `bbox_w`, `bbox_h`. Used in the Gradient
  dialog and the inspector's Reference Point section.

**Non-driveable widget kinds.** Some `set_name`'d widgets in
the dialogs aren't `curvz::widgets::*` subclasses — they're
raw GTK widgets that show up in the resolver but accept no
verbs and expose no properties. They're addressable (a
script can confirm they exist by name) but inert:

- **Label** (`Gtk::Label`) — display-only text.
- **DrawingArea** (`Gtk::DrawingArea`) — custom Cairo
  surfaces (gradient track, angle knob, preview
  thumbnails). Reads pixels through script aren't supported
  today.
- **ProgressBar** (`Gtk::ProgressBar`) — progress fill;
  read-only via inspector channels, not directly via
  resolver.
- **Notebook / Notebook tab** (`Gtk::Notebook`) — the tab
  switcher in multi-page dialogs. Tab activation goes
  through the dialog's own logic (mostly auto-driven by
  the menu action that opened the dialog); no direct
  `switch_page` verb today.
- **ScrolledWindow / container / root** — structural
  parents that hold child substrate widgets. Names
  registered so traces can echo the hierarchy; no driver
  verbs.

**RunContext masks.** None of the dialog widgets override
`context_mask()` — every verb defaults to **`all_three`**
(TestRunner, Scripter, Macro). Same posture as Toolbar and
Inspector substrate.

**Dialog vs popover.** Two affordances ship the same UX in
parallel — BlendDialog vs BlendPopover, StepRepeatDialog vs
StepRepeatPopover. The popover is the canonical surface
today (it's what the menu actions and toolbar long-presses
open); the dialog form is retained for the right-click
context menu and a few legacy entry points. The substrate
widget shapes are identical between the two; only the name
prefix differs (`dlg_<tag>_*` vs `pop_<tag>_*`). Scripts
target whichever one is open.

## Dialogs and popovers covered

| # | Surface | Substrate widgets |
|---|---|--:|
| ❶ | New document dialog | 14 |
| ❷ | Save-as-template dialog | 7 |
| ❸ | Manage templates dialog | 4 |
| ❹ | Export dialog | 30 |
| ❺ | Image info dialog | 1 |
| ❻ | Shortcuts dialog | 3 |
| ❼ | Style editor dialog | 22 |
| ❽ | Theme editor dialog | 29 |
| ❾ | Gradient dialog | 16 |
| ❿ | Color picker popover | 2 |
| ⓫ | Blend dialog | 10 |
| ⓬ | Blend popover | 10 |
| ⓭ | Step and Repeat dialog | 12 |
| ⓮ | Step and Repeat popover | 12 |
| ⓯ | Translate dialog | 13 |
| ⓰ | Offset path dialog | 6 |
| ⓱ | Rotate from point dialog | 1 |
| ⓲ | Warp popover | 6 |
| ⓳ | Progress dialog | 6 |

Total: **204 substrate widgets across 19 surfaces.**

## ❶ New document dialog

Opens via **File → New document** (Ctrl+N). Lets the user
name a new document, pick a theme, set canvas size and
units, and choose portrait / landscape orientation before
clicking Create.

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_nd` | `new_document_dialog_root` | root |
| `dlg_nd_nm` | `new_document_dialog_name_entry` | Entry |
| `dlg_nd_thm` | `new_document_dialog_theme_dd` | DropDown |
| `dlg_nd_nb` | `new_document_dialog_notebook` | Notebook |
| `dlg_nd_tb` | `new_document_dialog_thumb_da` | DrawingArea |
| `dlg_nd_pvl` | `new_document_dialog_preview_lbl` | Label |
| `dlg_nd_un` | `new_document_dialog_units_dd` | DropDown |
| `dlg_nd_op` | `new_document_dialog_orient_portrait_btn` | Button |
| `dlg_nd_ol` | `new_document_dialog_orient_landscape_btn` | Button |
| `dlg_nd_sw` | `new_document_dialog_size_w_spn` | SpinButton |
| `dlg_nd_sh` | `new_document_dialog_size_h_spn` | SpinButton |
| `dlg_nd_sl` | `new_document_dialog_size_lock_toggle` | ToggleButton |
| `dlg_nd_cnc` | `new_document_dialog_cancel_btn` | Button |
| `dlg_nd_cre` | `new_document_dialog_create_btn` | Button |

The size-lock toggle keeps the width / height spinners
ratio-linked when active. The thumbnail drawing area shows
a live preview of the canvas as the user adjusts size and
orientation; not driveable from script.

## ❷ Save-as-template dialog

Opens via **File → Save as template…** Lets the user save
the active document as a new template with a name, category
(an existing one from a dropdown or a brand-new one typed
into the Other field), and a description.

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_sat` | `save_as_template_dialog_root` | root |
| `dlg_sat_nm` | `save_as_template_dialog_name_entry` | Entry |
| `dlg_sat_cat` | `save_as_template_dialog_category_dd` | DropDown |
| `dlg_sat_oth` | `save_as_template_dialog_other_entry` | Entry |
| `dlg_sat_dsc` | `save_as_template_dialog_description_entry` | Entry |
| `dlg_sat_cnc` | `save_as_template_dialog_cancel_btn` | Button |
| `dlg_sat_cre` | `save_as_template_dialog_create_btn` | Button |

The Other entry is only meaningful when the Category
dropdown is set to "Other" (the conventional "new category"
sentinel); otherwise it's ignored. The Create button is the
commit; Cancel discards.

## ❸ Manage templates dialog

Opens via **File → Manage templates…** Shows the existing
template categories in a list view with per-template
controls (delete, rename, recategorise). Most of the
interaction lives in per-row UI elements that aren't
script-addressable today; only the top-level surface
widgets are.

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_mt` | `manage_templates_dialog_root` | root |
| `dlg_mt_nc` | `manage_templates_dialog_new_category_btn` | Button |
| `dlg_mt_tst` | `manage_templates_dialog_toast_lbl` | Label |
| `dlg_mt_cls` | `manage_templates_dialog_close_btn` | Button |

The toast label flashes confirmation messages after a
destructive action. Per-row template entries (the list
view's children) aren't substrate-registered — a future
milestone might add per-row addressability if scripts need
to drive specific templates.

## ❹ Export dialog

Opens via **File → Export…** (Ctrl+E). The unified export
dialog with two notebook tabs: **Documents** (export the
active project's documents as SVG / PNG / reference-point
JSON) and **Theme** (export the active theme as a
distributable bundle). The densest dialog in the app — 30
substrate widgets.

### Top-level

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_xu` | `export_unified_dialog_root` | root |
| `dlg_xu_nb` | `export_unified_dialog_notebook` | Notebook |
| `dlg_xu_docs` | `export_unified_dialog_docs_tab` | Notebook tab |
| `dlg_xu_theme` | `export_unified_dialog_theme_tab` | Notebook tab |
| `dlg_xu_close` | `export_unified_dialog_close_btn` | Button |

### Documents tab

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_xu_d_sa` | `export_unified_dialog_docs_select_all_btn` | Button |
| `dlg_xu_d_sn` | `export_unified_dialog_docs_select_none_btn` | Button |
| `dlg_xu_d_rsvg` | `export_unified_dialog_docs_radio_svg` | ToggleButton (radio) |
| `dlg_xu_d_rpng` | `export_unified_dialog_docs_radio_png` | ToggleButton (radio) |
| `dlg_xu_d_rref` | `export_unified_dialog_docs_radio_refpt` | ToggleButton (radio) |
| `dlg_xu_d_rfw` | `export_unified_dialog_docs_radio_fit_w` | ToggleButton (radio) |
| `dlg_xu_d_rfh` | `export_unified_dialog_docs_radio_fit_h` | ToggleButton (radio) |
| `dlg_xu_d_val` | `export_unified_dialog_docs_value_spn` | SpinButton |
| `dlg_xu_d_unt` | `export_unified_dialog_docs_units_dd` | DropDown |
| `dlg_xu_d_dpi` | `export_unified_dialog_docs_dpi_dd` | DropDown |
| `dlg_xu_d_dpc` | `export_unified_dialog_docs_dpi_custom_spn` | SpinButton |
| `dlg_xu_d_rpf` | `export_unified_dialog_docs_refpts_format_dd` | DropDown |
| `dlg_xu_d_rpi` | `export_unified_dialog_docs_refpts_info_lbl` | Label |
| `dlg_xu_d_fld` | `export_unified_dialog_docs_folder_entry` | Entry |
| `dlg_xu_d_brw` | `export_unified_dialog_docs_browse_btn` | Button |
| `dlg_xu_d_sts` | `export_unified_dialog_docs_status_lbl` | Label |
| `dlg_xu_d_exp` | `export_unified_dialog_docs_export_btn` | Button |

The `_rsvg` / `_rpng` / `_rref` group picks the output
format; the `_rfw` / `_rfh` group picks how PNG dimensions
are derived (fit-to-width vs fit-to-height). The `_unt`
DropDown's vocabulary is the same unit token list as
inspector unit controls (`px`, `pt`, `mm`, `cm`, `in`,
`pc`). The `_dpi` DropDown carries the preset DPI values
(72 / 96 / 150 / 300 / 600 / Custom); selecting Custom
enables the `_dpc` spinner for a free-form value. **Use the
script via the headless `export` Scriptable** for batched
work — see **Singletons reference** — this dialog is the
GUI surface and ergonomic for one-off exports.

### Theme tab

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_xu_t_nm` | `export_unified_dialog_theme_name_entry` | Entry |
| `dlg_xu_t_cmt` | `export_unified_dialog_theme_comment_entry` | Entry |
| `dlg_xu_t_warn` | `export_unified_dialog_theme_warn_lbl` | Label |
| `dlg_xu_t_fld` | `export_unified_dialog_theme_folder_entry` | Entry |
| `dlg_xu_t_brw` | `export_unified_dialog_theme_browse_btn` | Button |
| `dlg_xu_t_prg` | `export_unified_dialog_theme_progress_bar` | ProgressBar |
| `dlg_xu_t_sts` | `export_unified_dialog_theme_status_lbl` | Label |
| `dlg_xu_t_exp` | `export_unified_dialog_theme_export_btn` | Button |

The progress bar drives during the bundle-creation phase.
The status label echoes the export step.

## ❺ Image info dialog

Opens via right-click on an `image` scene object → **Image
info**. Shows the image's filename, dimensions, embedded
metadata. The dialog has no driver controls today — it's
a read-only display surface — so only the root window is
substrate-registered.

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_imginfo` | `image_info_dialog_root` | root |

Useful for tests that need to confirm "the image info
dialog opened" without needing to interact with it. Close
via window-frame X.

## ❻ Shortcuts dialog

Opens via **Help → Keyboard shortcuts…** A multi-tabbed
read-only reference of every hotkey, grouped by area
(View, Edit, Selection, etc.). The notebook holds the
per-area tabs; only the outer structure is substrate-
registered today.

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_sh` | `shortcuts_dialog_root` | root |
| `dlg_sh_nb` | `shortcuts_dialog_notebook` | Notebook |
| `dlg_sh_cls` | `shortcuts_dialog_close_btn` | Button |

## ❼ Style editor dialog

Opens from the **Styles** panel via the **+** button or
double-click on an existing style row. Edits a style's
name / category / fill / stroke / shadow.

### Identity and category

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_se` | `style_editor_dialog_window` | root |
| `dlg_se_nm` | `style_editor_dialog_name_entry` | Entry |
| `dlg_se_cat` | `style_editor_dialog_category_dd` | DropDown |
| `dlg_se_cnew` | `style_editor_dialog_category_new_entry` | Entry |

The category dropdown lists existing style categories; the
"new category" entry only takes effect when the dropdown
is set to the "(new category)" sentinel option.

### Fill and stroke compound editors

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_se_fill` | `style_editor_dialog_fill_editor` | compound (PaintEditor) |
| `dlg_se_strk` | `style_editor_dialog_stroke_editor` | compound (PaintEditor) |

Each `_editor` is a `PaintEditor` instance — a compound
widget that holds the per-paint controls (None /
CurrentColor / Solid / Swatch / Gradient toggles plus the
type-specific sub-controls). The compound widget itself
has no script-callable verbs at the compound level; its
child widgets are addressable via `pe_pal` (the palette
dropdown — registered by PaintEditor itself) plus the
inline color picker spawned via popover. **Driving fill /
stroke through script is more tractable via the
`styles.<iid>` proxy** (see **Content reference**) than
through the editor dialog.

### Stroke width / cap / join

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_se_sw` | `style_editor_dialog_stroke_width_spn` | SpinButton |
| `dlg_se_cb` | `style_editor_dialog_cap_butt_btn` | Button |
| `dlg_se_cr` | `style_editor_dialog_cap_round_btn` | Button |
| `dlg_se_cs` | `style_editor_dialog_cap_square_btn` | Button |
| `dlg_se_jm` | `style_editor_dialog_join_miter_btn` | Button |
| `dlg_se_jr` | `style_editor_dialog_join_round_btn` | Button |
| `dlg_se_jb` | `style_editor_dialog_join_bevel_btn` | Button |

The cap and join groups are rendered as button rows
rather than radios; clicking one activates that mode.
Same tri-state vocabulary as `objects.<iid> set_stroke_cap`
and `set_stroke_join` — see **Canvas & objects
reference**.

### Shadow

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_se_sen` | `style_editor_dialog_shadow_enable_check` | CheckButton |
| `dlg_se_sdx` | `style_editor_dialog_shadow_dx_spn` | SpinButton |
| `dlg_se_sdy` | `style_editor_dialog_shadow_dy_spn` | SpinButton |
| `dlg_se_sbl` | `style_editor_dialog_shadow_blur_spn` | SpinButton |
| `dlg_se_ssw` | `style_editor_dialog_shadow_swatch_da` | DrawingArea |
| `dlg_se_sca` | `style_editor_dialog_shadow_color_alpha_slider` | Scale |
| `dlg_se_sop` | `style_editor_dialog_shadow_opacity_slider` | Scale |

The shadow swatch drawing area opens a color picker on
click; not driveable from script (use `styles.<iid>
shadow_color` instead).

### Confirm

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_se_cnc` | `style_editor_dialog_cancel_btn` | Button |
| `dlg_se_ok` | `style_editor_dialog_ok_btn` | Button |

## ❽ Theme editor dialog

Opens from the **Themes** panel via the **+** button or
double-click on a theme row. Edits every doc-appearance
field — unit, motif, guides, grid, margins, snap —
grouped into notebook tabs.

### Identity, mode, and outer chrome

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_te` | `theme_editor_dialog_window` | root |
| `dlg_te_scroll` | `theme_editor_dialog_props_scroll` | ScrolledWindow |
| `dlg_te_body` | `theme_editor_dialog_body` | container |
| `dlg_te_nm` | `theme_editor_dialog_name_entry` | Entry |
| `dlg_te_mode_l` | `theme_editor_dialog_mode_light_btn` | Button |
| `dlg_te_mode_d` | `theme_editor_dialog_mode_dark_btn` | Button |
| `dlg_te_thumb` | `theme_editor_dialog_thumbnail` | DrawingArea |
| `dlg_te_nb` | `theme_editor_dialog_notebook` | Notebook |
| `dlg_te_cnc` | `theme_editor_dialog_cancel_btn` | Button |
| `dlg_te_rst` | `theme_editor_dialog_reset_btn` | Button |
| `dlg_te_ok` | `theme_editor_dialog_ok_btn` | Button |

The mode buttons switch between editing the theme's light
mode and dark mode appearance. The thumbnail drawing area
shows a live preview.

### Unit and visibility

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_te_un` | `theme_editor_dialog_units_dd` | DropDown |
| `dlg_te_gd_vis` | `theme_editor_dialog_guides_visible_chk` | CheckButton |

The units dropdown vocabulary is `px` / `pt` / `mm` / `cm`
/ `in` / `pc` — same as the inspector and Export dialog
unit controls.

### Snap

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_te_sn_en` | `theme_editor_dialog_snap_enabled_chk` | CheckButton |
| `dlg_te_sn_gd` | `theme_editor_dialog_snap_guides_chk` | CheckButton |
| `dlg_te_sn_gr` | `theme_editor_dialog_snap_grid_chk` | CheckButton |
| `dlg_te_sn_mg` | `theme_editor_dialog_snap_margins_chk` | CheckButton |
| `dlg_te_sn_nd` | `theme_editor_dialog_snap_nodes_chk` | CheckButton |
| `dlg_te_sn_eg` | `theme_editor_dialog_snap_edges_chk` | CheckButton |
| `dlg_te_sn_cn` | `theme_editor_dialog_snap_centers_chk` | CheckButton |

Master enable plus six per-target toggles. Sub-target
toggles are no-ops when master is off (the model fields
are stored independently but the snap engine only consults
them when master is on).

### Grid

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_te_gr_en` | `theme_editor_dialog_grid_enabled_chk` | CheckButton |
| `dlg_te_gr_vis` | `theme_editor_dialog_grid_visible_chk` | CheckButton |
| `dlg_te_gr_dot` | `theme_editor_dialog_grid_dots_chk` | CheckButton |
| `dlg_te_gr_a` | `theme_editor_dialog_grid_alpha_pct` | SpinButton (percent) |

`_gr_a` is a SpinButton with 0–100 percent vocabulary.

### Margins

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_te_mg_en` | `theme_editor_dialog_margin_enabled_chk` | CheckButton |
| `dlg_te_mg_vis` | `theme_editor_dialog_margin_visible_chk` | CheckButton |
| `dlg_te_mg_col` | `theme_editor_dialog_margin_cols_spn` | SpinButton |
| `dlg_te_mg_row` | `theme_editor_dialog_margin_rows_spn` | SpinButton |
| `dlg_te_mg_a` | `theme_editor_dialog_margin_alpha_pct` | SpinButton (percent) |

`_mg_a` is the per-cell alpha percent; same 0–100
vocabulary.

**For batched theme work, use the `themes.<iid>` proxy** —
see **Content reference** — rather than driving every
spinner through the dialog.

## ❾ Gradient dialog

Opens from a fill / stroke paint editor's Gradient toggle.
Edits the gradient's type (linear / radial), stop list, per-
stop position / colour / opacity, and angle / radius
parameters.

### Top-level

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_gr` | `gradient_dialog_root` | root |
| `dlg_gr_lin` | `gradient_dialog_linear_toggle` | ToggleButton |
| `dlg_gr_rad` | `gradient_dialog_radial_toggle` | ToggleButton |
| `dlg_gr_trk` | `gradient_dialog_track_da` | DrawingArea |

The linear / radial toggles form a 2-button radio. The
track drawing area shows the gradient with click-to-add-
stop and drag-to-reposition affordances; not driveable
from script.

### Stop controls

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_gr_as` | `gradient_dialog_add_stop_btn` | Button |
| `dlg_gr_rs` | `gradient_dialog_remove_stop_btn` | Button |
| `dlg_gr_rv` | `gradient_dialog_reverse_btn` | Button |
| `dlg_gr_ds` | `gradient_dialog_distribute_btn` | Button |
| `dlg_gr_pos` | `gradient_dialog_pos_spn` | SpinButton |
| `dlg_gr_op` | `gradient_dialog_opacity_spn` | SpinButton |
| `dlg_gr_cp` | `gradient_dialog_color_picker` | RefPointPicker |

The `_pos` and `_op` spinners apply to the currently-
selected stop; `_cp` is the color-picker compound widget
for the same.

### Geometry (radial-mode-specific)

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_gr_ang` | `gradient_dialog_angle_spn` | SpinButton |
| `dlg_gr_akb` | `gradient_dialog_angle_knob_da` | DrawingArea |
| `dlg_gr_r` | `gradient_dialog_radius_spn` | SpinButton |

Angle is for linear gradients; radius for radial. The
angle knob drawing area is a circular dial that mirrors
the spinner; click-drag rotates.

### Confirm

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_gr_cnc` | `gradient_dialog_cancel_btn` | Button |
| `dlg_gr_app` | `gradient_dialog_apply_btn` | Button |

## ❿ Color picker popover

Opens inline from any swatch chip, paint-type Solid mode,
or shadow swatch. Hosts a hue / saturation / lightness
picker, an alpha slider, and a name entry for saving the
result as a custom swatch. Only the root and the name
entry are substrate-registered today.

| Abbrev | Long name | Kind |
|---|---|---|
| `pop_cp` | `color_picker_popover_root` | root |
| `pop_cp_nm` | `color_picker_popover_name_entry` | Entry |

The picker's HSL controls are GTK-side custom drawing
that doesn't go through the substrate registry. **For
script-driven swatch creation, use the `swatches new`
verb** — see **Content reference**.

## ⓫ Blend dialog

Opens via right-click on a Blend object → **Edit blend
parameters**. Edits the existing blend's interpolation
parameters.

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_bld` | `blend_dialog_root` | root |
| `dlg_bld_warn` | `blend_dialog_warn_lbl` | Label |
| `dlg_bld_eq` | `blend_dialog_equalize_btn` | Button |
| `dlg_bld_st` | `blend_dialog_steps_spn` | SpinButton |
| `dlg_bld_sw` | `blend_dialog_start_w_spn` | SpinButton |
| `dlg_bld_ew` | `blend_dialog_end_w_spn` | SpinButton |
| `dlg_bld_rv` | `blend_dialog_reverse_check` | CheckButton |
| `dlg_bld_so` | `blend_dialog_stroke_override_check` | CheckButton |
| `dlg_bld_cnc` | `blend_dialog_cancel_btn` | Button |
| `dlg_bld_ok` | `blend_dialog_ok_btn` | Button |

The warn label flashes when an invalid step count is
typed. The equalize button sets `_sw` equal to `_ew`. The
stroke override checkbox controls whether the blend
respects per-source stroke or uses the start/end width
spinners.

## ⓬ Blend popover

The popover sibling of ❶❶ — same surface, different
prefix (`pop_bld_*` instead of `dlg_bld_*`). Opens via the
**Object → Blend** menu and is the canonical Blend
affordance today.

| Abbrev | Long name | Kind |
|---|---|---|
| `pop_bld` | `blend_popover_root` | root |
| `pop_bld_warn` | `blend_popover_warn_lbl` | Label |
| `pop_bld_eq` | `blend_popover_equalize_btn` | Button |
| `pop_bld_st` | `blend_popover_steps_spn` | SpinButton |
| `pop_bld_sw` | `blend_popover_start_w_spn` | SpinButton |
| `pop_bld_ew` | `blend_popover_end_w_spn` | SpinButton |
| `pop_bld_rv` | `blend_popover_reverse_check` | CheckButton |
| `pop_bld_so` | `blend_popover_stroke_override_check` | CheckButton |
| `pop_bld_cnc` | `blend_popover_cancel_btn` | Button |
| `pop_bld_ok` | `blend_popover_ok_btn` | Button |

## ⓭ Step and Repeat dialog

Opens via right-click on a selection → **Step and repeat**.
Configures the per-step copy parameters and previews the
result.

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_sr` | `step_repeat_dialog_root` | root |
| `dlg_sr_cp` | `step_repeat_dialog_copies_spn` | SpinButton |
| `dlg_sr_ox` | `step_repeat_dialog_offset_x_spn` | SpinButton |
| `dlg_sr_oy` | `step_repeat_dialog_offset_y_spn` | SpinButton |
| `dlg_sr_re` | `step_repeat_dialog_rotate_enable_check` | CheckButton |
| `dlg_sr_am` | `step_repeat_dialog_angle_mode_dd` | DropDown |
| `dlg_sr_ang` | `step_repeat_dialog_angle_spn` | SpinButton |
| `dlg_sr_px` | `step_repeat_dialog_pivot_x_spn` | SpinButton |
| `dlg_sr_py` | `step_repeat_dialog_pivot_y_spn` | SpinButton |
| `dlg_sr_pv` | `step_repeat_dialog_preview_da` | DrawingArea |
| `dlg_sr_cnc` | `step_repeat_dialog_cancel_btn` | Button |
| `dlg_sr_ok` | `step_repeat_dialog_ok_btn` | Button |

The angle mode dropdown picks between "fixed angle per
step" and "tangent to path" (gated on the rotate-enable
checkbox; angle / pivot spinners only fire when both are
on). The preview drawing area shows the resulting copies
live.

## ⓮ Step and Repeat popover

The popover sibling of ⓭ — same surface, `pop_sr_*`
prefix. Opens via the **Object → Step and repeat** menu
and is the canonical S+R affordance today.

| Abbrev | Long name | Kind |
|---|---|---|
| `pop_sr` | `step_repeat_popover_root` | root |
| `pop_sr_cp` | `step_repeat_popover_copies_spn` | SpinButton |
| `pop_sr_ox` | `step_repeat_popover_offset_x_spn` | SpinButton |
| `pop_sr_oy` | `step_repeat_popover_offset_y_spn` | SpinButton |
| `pop_sr_re` | `step_repeat_popover_rotate_enable_check` | CheckButton |
| `pop_sr_am` | `step_repeat_popover_angle_mode_dd` | DropDown |
| `pop_sr_ang` | `step_repeat_popover_angle_spn` | SpinButton |
| `pop_sr_px` | `step_repeat_popover_pivot_x_spn` | SpinButton |
| `pop_sr_py` | `step_repeat_popover_pivot_y_spn` | SpinButton |
| `pop_sr_pv` | `step_repeat_popover_preview_da` | DrawingArea |
| `pop_sr_cnc` | `step_repeat_popover_cancel_btn` | Button |
| `pop_sr_ok` | `step_repeat_popover_ok_btn` | Button |

## ⓯ Translate dialog

Opens via **Object → Translate / scale / rotate / skew**.
Combines four transform operations into one persistent
dialog (the Apply button doesn't dismiss the dialog so
multiple transforms can chain).

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_xlt` | `translate_dialog_root` | root |
| `dlg_xlt_verb` | `translate_dialog_verb_dd` | DropDown |
| `dlg_xlt_mx` | `translate_dialog_move_x_spn` | SpinButton |
| `dlg_xlt_my` | `translate_dialog_move_y_spn` | SpinButton |
| `dlg_xlt_sx` | `translate_dialog_scale_x_spn` | SpinButton |
| `dlg_xlt_sy` | `translate_dialog_scale_y_spn` | SpinButton |
| `dlg_xlt_sln` | `translate_dialog_scale_link_toggle` | ToggleButton |
| `dlg_xlt_ra` | `translate_dialog_rotate_a_spn` | SpinButton |
| `dlg_xlt_kh` | `translate_dialog_skew_h_toggle` | ToggleButton |
| `dlg_xlt_kv` | `translate_dialog_skew_v_toggle` | ToggleButton |
| `dlg_xlt_ka` | `translate_dialog_skew_a_spn` | SpinButton |
| `dlg_xlt_cls` | `translate_dialog_close_btn` | Button |
| `dlg_xlt_app` | `translate_dialog_apply_btn` | Button |

The verb dropdown picks which operation Apply commits
(Translate / Scale / Rotate / Skew). The scale link
toggle keeps `_sx` and `_sy` proportional. The skew
toggles select horizontal vs vertical skew direction
(mutually exclusive — `_kh` activating sets `_kv` to
false).

## ⓰ Offset path dialog

Opens via **Object → Offset path…** Generates a new path
parallel to a selected one, offset by a configurable
distance.

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_off` | `offset_path_dialog_root` | root |
| `dlg_off_dist` | `offset_path_dialog_distance_spn` | SpinButton |
| `dlg_off_side` | `offset_path_dialog_side_dd` | DropDown |
| `dlg_off_keep` | `offset_path_dialog_keep_original_check` | CheckButton |
| `dlg_off_cnc` | `offset_path_dialog_cancel_btn` | Button |
| `dlg_off_app` | `offset_path_dialog_apply_btn` | Button |

The side dropdown's vocabulary is `inside` / `outside` /
`both` — picks which side of the source path the offset
generates on (for open paths, only one direction makes
sense; for closed paths, all three are meaningful). The
keep-original checkbox controls whether the source is
preserved or replaced by the offset result.

## ⓱ Rotate from point dialog

Opens via **Object → Rotate from point…** Lets the user
rotate the selection by a specified angle around a chosen
pivot. The dialog's interaction logic lives in the canvas
(click-to-set-pivot then enter angle); only the dialog
root is substrate-registered.

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_rfp` | `rotate_from_point_dialog_root` | root |

A future milestone could expose the pivot and angle
controls as substrate widgets — they're currently
embedded in the canvas's interaction state rather than
the dialog's widget tree.

## ⓲ Warp popover

Opens via right-click on a Warp object → **Edit warp
envelope** or via toolbar long-press on the Warp tool.
Configures the envelope node count and quality
parameters.

| Abbrev | Long name | Kind |
|---|---|---|
| `pop_wrp` | `warp_popover_root` | root |
| `pop_wrp_tn` | `warp_popover_top_count_spin` | SpinButton |
| `pop_wrp_bn` | `warp_popover_bot_count_spin` | SpinButton |
| `pop_wrp_pr` | `warp_popover_preset_dd` | DropDown |
| `pop_wrp_q` | `warp_popover_quality_scale` | Scale |
| `pop_wrp_done` | `warp_popover_done_btn` | Button |

The preset dropdown's vocabulary is the named warp
preset list (Wave, Arch, Bulge, etc. — see the Warp
chapter of the user manual). Selecting a preset
auto-populates the top / bottom count spinners.

## ⓳ Progress dialog

A general-purpose progress surface raised for long-running
operations (export, boolean ops on dense input, theme
export). The work happens elsewhere; this dialog is the
status surface.

| Abbrev | Long name | Kind |
|---|---|---|
| `dlg_pr` | `progress_dialog_root` | root |
| `dlg_pr_msg` | `progress_dialog_message_label` | Label |
| `dlg_pr_bar` | `progress_dialog_progress_bar` | ProgressBar |
| `dlg_pr_el` | `progress_dialog_elapsed_label` | Label |
| `dlg_pr_eta` | `progress_dialog_eta_label` | Label |
| `dlg_pr_cnc` | `progress_dialog_cancel_btn` | Button |

The cancel button signals the underlying long-op to
abort; cancellation granularity depends on the op (some
ops only check between phases — see the
**boolean_op chunked Union slowdown** entry in the
backlog if relevant). The message / elapsed / ETA labels
update as the op progresses.

## Examples

Open the New Document dialog, type a name, set a custom
canvas size, and create:

```
win.new-document
# Wait for the dialog (the menu action raises it synchronously
# on this dispatch path).

dlg_nd_nm set "S+R smoke document"
dlg_nd_un pick "mm"
dlg_nd_sw set 100
dlg_nd_sh set 80
dlg_nd_cre click
```

Switch to the Theme tab of the Export dialog and check the
status label:

```
win.export-unified
dlg_xu_nb get count
# dlg_xu_nb is a Notebook — `get count` returns the tab
# count for sanity. The active-tab switching is not yet
# exposed as a verb; future surface.

get dlg_xu_t_sts text
# Output: dlg_xu_t_sts text = ""   (no export in progress)
```

Drive the Translate dialog to apply a 45° rotation:

```
win.translate
dlg_xlt_verb pick "Rotate"
dlg_xlt_ra set 45
dlg_xlt_app click
# The dialog stays open; apply again or click _cls to close.
dlg_xlt_cls click
```

## Reference complete

This page closes the Scripter reference arc on the
documentation side — every script-addressable surface in
Curvz now has its reference page. The eight reference
leaves under **Addendums → Developer**:

- **Language reference** — DSL grammar, literals,
  control words, trace format.
- **Singletons reference** — `proj`, `export`, `app`,
  `inspector`.
- **Header & menus reference** — `win` action wrapper,
  document tabs, status bar.
- **Toolbar reference** — main toolbar tool buttons and
  right-click default popovers.
- **Inspector reference** — every section of the
  Inspector, matching the §5.x manual structure.
- **Content reference** — `layers`, `swatches`,
  `styles`, `themes`, `palettes`, `guides`, `pnl_styles`.
- **Canvas & objects reference** — `objects` collection
  and per-instance proxy; canvas substrate widgets.
- **Dialogs & popovers reference** *(this page)* — 19
  modal-dialog and popover surfaces, 204 substrate
  widgets.

Plus the **Scripter overview** (Part 1, non-reference)
which introduces what the Scripter is and how to use it.

For the Scripter's design philosophy and the rationale
behind individual decisions (why the grammar is shaped
this way, why certain Scriptables exist, why some
surfaces are intentionally missing), see the related
canon entries in the project's design documentation —
this reference enumerates *what* exists; the canon
explains *why*.
