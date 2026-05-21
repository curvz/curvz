# Menus

The application menu lives behind the **☰** button at the far right
of the header bar. It contains every top-level action Curvz
supports, grouped into submenus.

![The menu open, showing the top-level submenus](img/3-2-menu-open.png)

The menu is the canonical place to find an action. Most actions
also have a keyboard shortcut, listed beside the menu item.

## Submenus, top to bottom

- **File** — project lifecycle (New Project, Add to Project, Open,
  Open Recent, Open Image, Close Project, Save, Save As, Save as
  Template, Manage Templates) and the import/export paths (Import
  SVG, Import as Icon, Place Image, Export, Print). See chapters
  2.1, 2.2, and 2.3.
- **Edit** — the conventional edit verbs (Undo, Redo, Select All,
  Deselect All, Cut, Copy, Paste, Duplicate, Duplicate in Place)
  plus View Clipboard for inspecting the system clipboard.
- **Arrange** — z-order operations (Bring to Front, Bring Forward,
  Send Backward, Send to Back) and per-axis flips (Flip Horizontal,
  Flip Vertical).
- **Align** — align operations (Left / Center H / Right / Top /
  Center V / Bottom) and distribute operations (Distribute
  Horizontally / Vertically). Enabled when two or more objects are
  selected with the Selection tool active. See 7.4.
- **Path** — vector operations grouped into sections: boolean
  (Union, Subtract, Intersect, Step and Repeat), compound paths
  (Make / Split), grouping (Group / Ungroup), derived paths (Offset
  Path, Expand Stroke, Convert Text to Path), clipping (Clip,
  Release Clip), blends (Blend, Release Blend), and warps (Warp,
  Edit Warp, Release Warp, Flatten Warp). See chapter 8.
- **View** — toggles for Rulers and Outline Mode, plus a **Zoom**
  submenu (Zoom In, Zoom Out, Zoom to 100%, Zoom to 200%, Zoom to
  Selection, Fit to Window). See chapter 10.
- **Navigate** — Next Document, Previous Document. The menu
  presence is mostly so the keyboard accelerators register at the
  right precedence; you'll usually invoke them via the shortcuts
  rather than the menu.
- **Developer** — power-user surfaces. Currently houses the
  Scripting toggle, which enables the Scripter window and the
  script-driven test harness. See **Scripter overview** in the
  Addendums.

Below the submenus, three top-level items live in their own
section:

- **Help** — opens this manual. Same as **F1**.
- **Keyboard Shortcuts** — opens the shortcuts dialog (see 11.2).
  Same as **?**.
- **Quit** — exits Curvz.

## Action availability

Menu items grey out when their action isn't available — for
example, **Path → Subtract** is enabled only when at least two
paths are selected; **Edit → Paste** is enabled only when there is
something on the clipboard; **Arrange → Bring Forward** is enabled
only when something is selected.

A greyed item is documentation: it's telling you the action exists
but isn't applicable to the current selection. Hover any greyed
item for a tooltip explaining the precondition.

## Right-click menus

Many surfaces in Curvz have their own right-click menu, separate
from the application menu. These are documented inline on each
surface's page rather than collected here:

- Document tabs — see 3.1.
- Toolbar shape tools (Rectangle, Ellipse, Line, Polygon, Spiral) —
  see the per-tool pages in 4.4.
- Layers panel rows — see 6.2.
- Library / Swatches / Styles entries — see 6.3 / 6.4 / 6.5.
- Documents gallery thumbnails — see 6.6.
- The corner square between the rulers — see 3.6.

The same idiom runs throughout: a surface's contextual actions live
on the surface, not in the application menu.

### Keys

A condensed list of menu accelerators. The full master cheatsheet
lives in **Keyboard shortcuts** (11.2).

#### File

- `Ctrl+N` — Add to Project (New Document)
- `Ctrl+O` — Open Project
- `Ctrl+S` — Save
- `Ctrl+Shift+S` — Save As
- `Ctrl+Shift+T` — Export
- `Ctrl+P` — Print
- `Ctrl+Q` / `Ctrl+W` — Quit

#### Edit

- `Ctrl+Z` — Undo
- `Ctrl+Shift+Z` / `Ctrl+Y` — Redo
- `Ctrl+X` / `Ctrl+C` / `Ctrl+V` — Cut / Copy / Paste
- `Ctrl+A` — Select all
- `Ctrl+Shift+A` — Deselect all
- `Ctrl+D` — Duplicate
- `Alt+D` — Duplicate in Place

#### Arrange

- `Ctrl+↑` — Bring Forward
- `Ctrl+↓` — Send Backward
- `Ctrl+Shift+↑` — Bring to Front
- `Ctrl+Shift+↓` — Send to Back

#### Align

- `Ctrl+Alt+L` — Align Left
- `Ctrl+Alt+H` — Align Center H
- `Ctrl+Alt+R` — Align Right
- `Ctrl+Alt+P` — Align Top
- `Ctrl+Alt+M` — Align Center V
- `Ctrl+Alt+B` — Align Bottom

#### Path

- `Ctrl+Shift+U` — Union
- `Ctrl+Shift+E` — Subtract
- `Ctrl+Shift+I` — Intersect
- `Ctrl+Alt+D` — Step and Repeat
- `Ctrl+Shift+O` — Offset Path
- `Ctrl+Shift+X` — Expand Stroke
- `Ctrl+8` — Make Compound Path
- `Ctrl+Shift+8` — Split Compound Path
- `Ctrl+7` — Clip
- `Ctrl+Alt+7` — Release Clip
- `Ctrl+G` — Group
- `Ctrl+Shift+G` — Ungroup

#### View

- `Ctrl+R` — Toggle Rulers
- `Ctrl+E` — Toggle Outline Mode
- `Ctrl+0` — Fit to Window
- `Ctrl+Shift+0` — Fit All (including off-canvas)
- `Ctrl+1` — Zoom 1× fit (artboard fills viewport)
- `Ctrl+2` — Zoom 2× fit
- `Ctrl+3` — Zoom to Selection

#### Navigate

- `Ctrl+Tab` / `Ctrl+PgDn` — Next Document
- `Ctrl+Shift+Tab` / `Ctrl+PgUp` — Previous Document

#### Help

- `F1` — Open this manual
- `?` (or `/`, `Ctrl+?`, `Ctrl+/`) — Keyboard shortcuts dialog
