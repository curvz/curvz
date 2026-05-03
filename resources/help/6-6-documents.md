# Documents

The **Documents** panel sits at the bottom of the right pane and
hosts every icon in the project. A Curvz project is normally a
*set* of related icons — a complete icon family, a theme variant,
a set of states for one symbol — and this panel is where you switch
between them.

There are two tabs: **Project** lists the icons saved in this
project, and **System** browses the icon themes installed on your
machine. Project is where you spend time editing; System is a
reference tab for picking up stock art or matching against existing
themes.

![The Documents panel with five icons in the Project tab](img/6-6-documents-gallery.png)

## ❶ Header — search, view toggle, and document buttons

The header row ❶ holds:

- A **search field** that filters by document name.
- A **view-mode** toggle (thumbnail grid / list).
- **+** to add a new document, **Duplicate** to copy the active
  one, **Remove** to delete the selected one, and **Clear all**
  to wipe every non-active document from the project at once.

The Add button creates a fresh document at the project's default
size. Duplicate copies the document under the active focus —
everything from artwork to inspector settings — under a new name.
Clear all is destructive: it removes every document except the
currently-open one, and undo doesn't reach across that boundary.
Curvz asks for confirmation before doing it.

## ❷ Project tab — thumbnail view

The default view ❷ is a flow-grid of thumbnails. Each card shows a
small Cairo render of the document on its motif-correct artboard,
with the document's name underneath. Click a card to open that
document in the editor; the active document gets a highlighted
outline so you can see at a glance which one is open.

Right-clicking a card gives quick access to the same operations
the header buttons do — duplicate, remove, rename — without having
to make the document active first.

## ❸ Project tab — list view

Toggle the view-mode button ❸ to switch to a single-column list
view. Each row shows the document name and a small icon. The list
view is the same data as the grid, just denser — useful when the
project has many documents and you want to scan names quickly.

The same right-click menu and the same single-click-to-open
behaviour apply.

## ❹ System tab — installed themes

The System tab ❹ scans your operating system's icon theme directories
the first time you open it (the scan is lazy to keep startup
fast). Two dropdowns let you narrow what's visible:

- **Theme** — pick which installed theme to browse (Adwaita,
  Papirus, breeze, and so on, depending on what's on your
  machine).
- **Category** — narrow within the theme: actions, places,
  status, devices, and so on.

The grid below shows every icon matching the theme + category
filter. Click an entry to **preview** it — the icon opens in a
read-only viewer where you can study its construction. Right-click
an entry to **copy** it into the active project as a new document,
giving you a starting point for matching that style or referencing
its construction.

The System tab is read-only with respect to the system itself —
Curvz never writes into your installed icon theme directories.

## ❺ Renaming and reordering

Double-click a name in either Project view to **rename** ❺ a
document inline. The name is saved with the project and used as the
document title in the canvas tab strip at the top of the workspace
(see **Header & Document tabs**, 3.1).

Documents are not currently re-orderable in the panel — they appear
in creation order. Re-ordering is a backlog item; if you need a
specific arrangement for an export pipeline, the SVG output uses
the Curvz internal id rather than panel position, so the order
isn't load-bearing for tooling.

## ❻ Search

The search field ❻ filters by document name in real time, on both
tabs. The match is case-insensitive substring against the visible
name. Clear the field to see everything again.

In the System tab, search filters within the active theme +
category — it doesn't expand back across the whole installed icon
set.

## Project scope and persistence

The Documents panel is project-scoped. Every document you see in
the Project tab lives inside the same `.curvz` project file: you
can copy that one file to another machine and the entire icon set
travels with it.

Each document inside the project carries its own:

- Layers, paths, guides, grid, margins.
- Per-document inspector settings (artboard size, units).
- Optional reference images you've imported.

Project-level state — Motif, Swatches, Styles — is shared across
every document and stored once.

## Where to next

- For switching between *open* documents (those in the canvas tab
  strip) by keyboard, see **Header & Document tabs** (3.1) —
  Ctrl+Tab and Ctrl+Page Up / Page Down.
- For the canvas tab strip and document title presentation, also
  see 3.1.
- For exporting documents as SVG icons, see **Import & export**
  (2.3).
- For the project-level appearance settings shared across every
  document in this panel, see **Motif** (5.2.1) and **Themes**
  (9.1).
