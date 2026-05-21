# Curvz user manual — outline v2

Locked s128. Resynced with disk + ToC in s279 (5.2.1 Motif removed as never-shipped; 5.3.8 / 6.7 / 7.4 / 7.5 added to match runtime; 5.3.2 and 5.4.5 labels updated to match ToC). Resynced again in s281 (7.6 Warp added — its help page and ToC entry shipped in s280 m1 but the outline bump never reached disk; Addendums chapter added with Developer → Scripter overview). s281 m2 added the Reference index plus Language and Singletons reference leaves, with future-session placeholders for the six remaining surface-area pages; the Overview was amended in m2 to correct three factual errors caught during reference-page drafting (a nonexistent `clip` Scriptable, an invented tutorial-script list, and a stale "log line at registration" claim). s282 m1 shipped the Header & menus reference leaf; s282 m2 added the Toolbar reference; s282 m3 added the Inspector reference. s283 m1 shipped the Content reference (partial — layers / swatches / guides; styles / themes / palettes / pnl_styles forthcoming in m2 / m3). s283 m2 extended Content with styles / themes / palettes; pnl_styles still forthcoming in m3. s283 m3 added pnl_styles, completing the Content reference. Supersedes `manual-outline-v1.svg` (s114, pre-Motif, pre-Content, pre-Interface chapter).

**12 chapters · 46 sections · 75 leaves.** Flat list — depth is encoded in the dotted nomenclature (e.g. `4.4.1` is third-level). Sidebar will collapse to chapter and section level; only the leaves load topic content.

## Status legend

- `[ ]` — not started
- `[~]` — stub (lede + structure + image placeholders, real prose pending)
- `[D]` — drafted (real content; may need polish + screenshots)
- `[S]` — fully drafted AND screenshots in place
- `(C)` — conditional inspector section (appears only for some node types) — note in lede
- `### Keys` — page has modifier / accelerator subsection
- `→ N-N-slug.md` — file name in `resources/help/`

Moving an item from `[ ]` → `[~]` → `[D]` → `[S]` is the workflow. Update this file (or its in-handoff copy) when shipping changes.

## TOC

```
1 Getting started
  1.1   [D] Welcome                                    → 1-1-welcome.md
  1.2   [D] About Curvz                                → 1-2-about-curvz.md
  1.3   [D] Workspace tour                             → 1-3-workspace-tour.md         (was 1-3-workspace-overview.png era)

2 Documents and files
  2.1   [D] Projects & documents                       → 2-1-projects-and-documents.md
  2.2   [D] Templates                                  → 2-2-templates.md
  2.3   [D] Import & export                            → 2-3-import-export.md

3 The interface                                                                         (NEW chapter — was missing)
  3.1   [D] Header & Document tabs                     → 3-1-header-and-tabs.md
  3.2   [D] Menus                            ### Keys  → 3-2-menus.md
  3.3   [D] Toolbar                          ### Keys  → 3-3-toolbar.md
  3.4   [D] Context bar                                → 3-4-context-bar.md
  3.5   [D] Status bar                                 → 3-5-status-bar.md
  3.6   [D] Canvas, rulers & corner                    → 3-6-canvas-rulers-corner.md
  3.7   [D] Color picker & paint editor                → 3-7-color-picker.md           (referenced from many pages)

4 Tools                                                                                 (15 tools)
  4.1   [ ] Tools overview                             → 4-1-tools-overview.md         (replaces 3-1-tools-overview.md)
  4.2     Selection & Node
  4.2.1 [ ] Selection tool                   ### Keys  → 4-2-1-selection-tool.md
  4.2.2 [ ] Node tool                        ### Keys  → 4-2-2-node-tool.md
  4.3   [ ] Pen                              ### Keys  → 4-3-pen.md
  4.4     Shape tools
  4.4.1 [ ] Rectangle                        ### Keys  → 4-4-1-rectangle.md
  4.4.2 [ ] Ellipse                          ### Keys  → 4-4-2-ellipse.md
  4.4.3 [ ] Line                             ### Keys  → 4-4-3-line.md
  4.4.4 [ ] Polygon                          ### Keys  → 4-4-4-polygon.md
  4.4.5 [ ] Spiral                           ### Keys  → 4-4-5-spiral.md
  4.5     Text family
  4.5.1 [ ] Text                             ### Keys  → 4-5-1-text.md
  4.5.2 [ ] Text on Path                               → 4-5-2-text-on-path.md
  4.6     Utility tools
  4.6.1 [ ] Eyedropper                       ### Keys  → 4-6-1-eyedropper.md
  4.6.2 [ ] Zoom                             ### Keys  → 4-6-2-zoom.md
  4.6.3 [ ] Ruler                                      → 4-6-3-ruler.md
  4.7   [ ] Reference points                           → 4-7-reference-points.md
  4.8   [ ] Corner                                     → 4-8-corner.md

5 Inspector
  5.1   [ ] Inspector overview                         → 5-1-inspector-overview.md
  5.3     Document group
  5.3.1 [ ] Metadata                                   → 5-3-1-metadata.md
  5.3.2 [ ] Dimensions                                 → 5-3-2-canvas.md                (filename predates Canvas → Dimensions rename)
  5.3.3 [ ] Guides                                     → 5-3-3-guides.md
  5.3.4 [ ] Grid                                       → 5-3-4-grid.md
  5.3.5 [ ] Margins                                    → 5-3-5-margins.md
  5.3.6 [ ] Snap                             ### Keys  → 5-3-6-snap.md
  5.3.7 [ ] Measure                                    → 5-3-7-measure.md
  5.3.8 [ ] Theme                                      → 5-3-8-theme.md
  5.4     Object group
  5.4.1 [ ] Selection                        ### Keys  → 5-4-1-selection.md
  5.4.2 [ ] Text                         (C)           → 5-4-2-text.md
  5.4.3 [ ] Blend                        (C)           → 5-4-3-blend.md
  5.4.4 [ ] Node                             ### Keys  → 5-4-4-node.md
  5.4.5 [ ] Styling                                    → 5-4-5-appearance.md            (filename predates Appearance → Styling rename)
  5.4.6 [ ] Shadow                       (C)           → 5-4-6-shadow.md

6 Content                                                                               (NEW chapter — was missing)
  6.1   [ ] Content overview                           → 6-1-content-overview.md
  6.2   [ ] Layers                           ### Keys  → 6-2-layers.md
  6.3   [ ] Library                                    → 6-3-library.md
  6.4   [ ] Swatches                                   → 6-4-swatches.md
  6.5   [ ] Styles                                     → 6-5-styles.md
  6.6   [ ] Documents                                  → 6-6-documents.md
  6.7   [ ] Themes                                     → 6-7-themes.md                 (the panel doc — concept page at 9.1)

7 Working with objects
  7.1   [ ] Step and Repeat                            → 7-1-step-and-repeat.md
  7.2   [ ] Clip masks                                 → 7-2-clip-masks.md
  7.3   [ ] Blends                                     → 7-3-blends.md
  7.4   [ ] Align & Distribute               ### Keys  → 7-4-align-distribute.md
  7.5   [ ] Group and Ungroup                          → 7-5-group-ungroup.md
  7.6   [D] Warp                             ### Keys  → 7-6-warp.md                 (shipped s280 m1)

8 Path operations
  8.1   [ ] Editing paths                    ### Keys  → 8-1-editing-paths.md
  8.2   [ ] Boolean operations                         → 8-2-boolean-operations.md
  8.3   [ ] Compound & derived                         → 8-3-compound-and-derived.md

9 Style and appearance
  9.1   [ ] Themes                                     → 9-1-themes.md
  9.2   [ ] currentColor & symbolic icons              → 9-2-currentcolor.md

10 Layout and precision
  10.1  [ ] View options                     ### Keys  → 10-1-view-options.md
  10.2  [ ] Units                                      → 10-2-units.md

11 Reference
  11.1  [ ] Macros                                     → 11-1-macros.md
  11.2  [ ] Keyboard shortcuts                         → 11-2-keyboard-shortcuts.md
  11.3  [ ] Troubleshooting                            → 11-3-troubleshooting.md

Addendums                                                                                 (NEW chapter — outside numbered sequence)
  Developer
    [D] Scripter overview                              → addendum-developer-scripter-overview.md      (s281 m1, corrected s281 m2)
    [D] Scripter reference                             → addendum-developer-scripter-reference.md     (s281 m2 — index page)
    [D] Language reference                             → addendum-developer-scripter-language.md      (s281 m2)
    [D] Singletons reference                           → addendum-developer-scripter-singletons.md    (s281 m2)
    [D] Toolbar reference                              → addendum-developer-scripter-toolbar.md       (s282 m2)
    [D] Inspector reference                            → addendum-developer-scripter-inspector.md     (s282 m3)
    [D] Content reference                              → addendum-developer-scripter-content.md       (s283 m1+m2+m3 — layers / swatches / guides / styles / themes / palettes / pnl_styles, complete)
    [ ] Canvas & objects reference                     → addendum-developer-scripter-canvas-objects.md (future session)
    [ ] Dialogs & popovers reference                   → addendum-developer-scripter-dialogs.md       (future session)
    [D] Header & menus reference                       → addendum-developer-scripter-header-menus.md  (s282 m1)
```

## Notes for writers (me)

- **`### Keys`** subsection lives at the bottom of the page. Lists modifiers / accelerators relevant to that page. Bindings come from the CAPTURE-phase key controller in `MainWindow.cpp` (~line 3000), NOT from `set_accels_for_action()` (cosmetic only). Verify before writing.
- **§11.2 Keyboard shortcuts** is the master cheatsheet. Per-page `### Keys` blocks are subsets, in workflow context.
- **Conditional sections** (5.4.2 / 5.4.3 / 5.4.6 marked `(C)`) — lede states "appears when ..." so users don't go hunting for a panel they can't see.
- **Image placeholders** in the .md use `![caption](img/N-N-N-slug.png)` (matches file numbering).
- **resources.xml** has a single block-commented zone for unmade images. Move the `<!--` past each line as the PNG ships, then `touch resources.xml` and rebuild. The block lives between the live aliases and the closing `</gresource>`.
- **Right-click menus, dialogs, and drag-and-drop** are documented INLINE in the page whose surface they belong to — never split into their own pages. So:
  - Layers panel right-click → covered in §6.2 Layers under `### Right-click menu`.
  - Step & Repeat dialog → covered in §7.1 Step and Repeat (the page IS the dialog page).
  - Library DnD into canvas → covered in §6.3 Library under `### Drag and drop`.
  - Document tab right-click → covered in §3.1 Header & Document tabs.
  - Same idiom across the manual: surface lives on the page that triggers it.
- **Color picker / paint editor** has its own page (§3.7) because it's a popover invoked from many surfaces (Motif chips, Appearance fill/stroke, guide colour, swatches, styles). Pages that open the picker reference §3.7 with a one-line link rather than re-documenting it.
- **Themes** is split across two pages by intent — **§6.7 Themes** is the panel doc (UI, save/apply, library, import/export); **§9.1 Themes** is the concept doc (what a theme is, what it bundles, the apply model). Each cross-references the other. **Macros** (§11.1) remains a single self-contained page — concept + UI in one.

## Migration notes

- `1-3-workspace-overview.png` (existing PNG) gets re-used by `1-3-workspace-tour.md`.
- `3-1-tools-overview.md` (existing) becomes `4-1-tools-overview.md`.
- `1-1-welcome.md`, `1-2-about-curvz.md`, `1-3-workspace-tour.md` keep their numbers.
- The s128-planned `5.2.1 Motif` page (was to be derived from `6-1-workspace-appearance.md`) was removed from the outline in s279 — the page never shipped to disk or ToC, and the Project-group section was empty as a result. If a Motif page is wanted later, restore both the 5.2 group heading and the leaf row.

## Open scope decisions captured

- **Sidebar UI (collapsible tree)** is m2, separate from .md content. Pages render in current flat-list view until then.
- **Concept-vs-panel duplication** avoided by:
  - Cutting "§7.1 Layers fundamentals" — concept page collapsed into §6.2 Layers.
  - Cutting "§9.3 Applying styles & swatches" — workflow lives in §6.4 / §6.5.
- **Warps** elided from §7 unless they're a real shipped feature (carry-forward — was speculative in v1).
- **Boolean ops** (§8.2) — page should note subtract/intersect are partially validated (handoff: untested).
