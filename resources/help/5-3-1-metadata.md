# Metadata

The **Metadata** section is the first row inside the inspector's
**Document** group. It collects everything *about* the current
project and document that does not fit into a geometry or
appearance category — paths on disk, file sizes, object counts,
and the export-name fields that govern what your icon is called
when it leaves Curvz.

Most of Metadata is read-only. The two editable fields are the
**File** name (which renames the document) and the **Icon name**
plus **Category** pair (which control the exported filename and
the GResource folder it lands in).

![The Metadata section expanded](img/5-3-1-metadata-section.png)

## ❶ Project block

The top of the section reports on the open project as a whole:

- **Project** — the project folder's name, e.g. `my-icons`. If
  the project has not yet been saved this reads `unsaved` and
  the rest of the project block is hidden.
- **Path** — the full path to the project folder. Selectable, so
  you can copy it; the field truncates from the start with an
  ellipsis when narrower than the path.
- **Documents** — the count of documents in the project.
- **Pkg size** — the total size on disk of the project folder,
  walking every file recursively.
- **Files** — the file count from the same walk.

These figures refresh whenever the inspector rebuilds — usually
every selection change. Big projects will show a brief pause on
selection while the recursive walk completes.

## ❷ Document block

Below a thin separator the section shows fields for the **active
document**:

- **File** — an editable text entry holding the document's stem
  filename (without the `.svg`). Type a new name and press
  Enter (or click the green checkmark icon) to rename. Renaming
  here is identical to right-clicking the document tab and
  choosing Rename — both routes go through the same handler.
- **SVG size** — the on-disk size of the SVG. Shows `—` if the
  document has not been saved or if the file does not exist yet.
- **Canvas** — the document's pixel dimensions in the form
  `W × H px`. The Canvas section (5.3.2) below is where you
  *change* these; this row is just a glanceable readout in
  pixel-count terms regardless of the display unit.
- **Objects** — the total leaf count across every layer in the
  document.

## ❸ Export block

Below another separator are the fields that drive **export
metadata**. They do not change the artwork; they govern the name
and category your icon ships with when you export.

- **Icon name** — the exported file's basename, e.g.
  `edit-copy-symbolic`. The placeholder text shows that example
  format. This name is what appears in code that calls
  `set_icon_name()`, so freedesktop conventions
  (lowercase, hyphenated, often `-symbolic` for monochrome
  icons) help you fit into the rest of the icon theme.
- **Category** — a dropdown of standard freedesktop icon
  categories: `actions`, `apps`, `mimetypes`, `devices`,
  `emblems`, `places`, `status`, `categories`. Pick `(none)` to
  leave it unset.

The pair flows together: when Curvz exports to a GResource
bundle, the icon goes under
`icons/scalable/<category>/<icon-name>.svg`. Pick a category that
matches the artwork — actions for verbs (edit, delete, save),
apps for application launchers, mimetypes for file-type icons,
and so on.

## Renaming versus saving

The **File** entry renames the document, but it does not save
the project — the rename takes effect immediately in memory and
persists only when you next save with **Ctrl+S**. The same is
true for **Icon name** and **Category**: they are edits to the
in-memory document and are written to disk on save.

If you change your mind before saving, **Edit → Undo** does not
revert metadata edits — they are document-property changes
rather than canvas commands. Re-edit the field to put the old
value back, or revert the whole document by closing without
saving.

## Where to next

Metadata is the only section in the Document group that is
mostly read-only. The next six are all interactive controls
that edit document state directly:

- **Canvas** (5.3.2) sets the document size and units.
- **Guides** (5.3.3), **Grid** (5.3.4), **Margins** (5.3.5)
  control the three layout overlays.
- **Snap** (5.3.6) decides which of those overlays the cursor
  honours.
- **Measure** (5.3.7) configures how the Ruler tool's
  measurements behave.
