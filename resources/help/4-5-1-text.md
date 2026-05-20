# ![Text tool icon](img/icons/curvz-text-symbolic.svg) Text

The **Text tool** drops a text node on the canvas and gives you an
inline entry to type its content. Like the shape tools, it's a
click-to-create flow — but unlike them, the new node enters an
edit state immediately so you can type the run before committing.

Activate it from the toolbox or with the **T** key (canvas focus
required).

For controls *after* the text exists — font, size, alignment,
baseline, letter spacing, fill, stroke — see the **Text** inspector
section (5.4.2). The Text tool itself is intentionally minimal: it
creates and edits the *content*, not the styling.

![A text node mid-edit, with the inline entry overlaying the canvas](img/4-5-1-text-cursor.png)

## Two click flows

The Text tool's click behaviour depends on what's under the cursor:

- **Click empty canvas** — drops a new text node at the click
  point and opens the inline entry. The defaults are **24-pt
  Sans, no stroke**, with the toolbar's fill well colour.
- **Click an existing text node** — re-enters edit mode for that
  node, pre-populating the inline entry with the current content.

The same gesture flow works whether the text is free-floating or
attached to a path — clicking the text or its host path opens the
entry.

## Editing the content

The inline entry sits where the text will render. Type to update
the content; the canvas redraws live as you type. The entry is a
single-line field — newlines are not committed.

Three ways to commit:

- **Press Enter** — commits the text and exits edit mode.
- **Switch tools** — commits and goes to the new tool.
- **Click outside the entry** on canvas — commits.

To **discard** a new node entirely (rather than commit it as
empty), press **Esc**. For an existing node being edited, Esc
reverts to the pre-edit content.

Edits coalesce on undo, so a typing run is one Ctrl+Z step rather
than per-character.

## Defaults at creation

A new text node always starts with these defaults:

- **Font**: Sans (whatever the system resolves "Sans" to —
  typically DejaVu Sans on Linux).
- **Size**: 24 pt.
- **Style**: regular (not bold, not italic).
- **Anchor**: start (left-aligned in left-to-right scripts).
- **Fill**: from the toolbar's fill well.
- **Stroke**: explicitly **None**, regardless of the toolbar's
  stroke well. Most icon work wants stroke-free text — the
  default reflects that.

To change any of these for the *next* text you create, set them
on the current selection in the inspector, draw your text, and
the inspector's last values seed the next defaults.

## Right-click for precise placement

Right-click the Text button in the toolbox to open the **Place
Text** popover — a numeric alternative to click-and-type when
you want the text to land at exact coordinates with specific
styling:

- **X**, **Y** — coordinates in document units (defaults to 10%
  width and 75% height of the canvas).
- **Size (pt)** — font size in points (1 to 2000; default 72).
- **Font** — searchable dropdown of every font family Pango can
  resolve on the system. Defaults to "Sans".
- **Bold** / **Italic** — independent checkboxes.
- **Align** — Left / Centre / Right / Justify.
- **Place** button — drops the text at the typed coordinates
  with the selected styling and dismisses the popover.

Unlike the click flow, the popover places an *empty* text node
at the specified coordinates — no inline entry opens. Switch to
the Text tool and click the placed node to type its content, or
edit content via the inspector. The popover defaults to 72 pt to
match common headline sizing; the click flow defaults to 24 pt
to match icon-caption sizing. Both routes drop a real text node
with the same downstream behaviour.

## Where the text sits

The click point becomes the text's **anchor**, with the text
extending to the right of the click in the default left-aligned
state. If you've changed the alignment to centre or right (in the
inspector after the fact), the anchor sits at the centre or right
edge of the run instead.

The Y coordinate of the click is the **baseline** of the text —
descenders drop below it. Click where you want the baseline to
sit, not where the top or middle of the run goes.

Curvz uses Y-up for everything else, but the text's anchor
follows the same convention — increasing Y moves the text upward.

## Editing existing text from the Selection tool

You can re-enter text edit mode without switching tools first:

- **Double-click a text node** with the Selection tool active.
  Curvz switches to Text and opens the inline entry for that
  node.

This shortcut is built for quick typo fixes — no need to find the
Text tool in the toolbox just to change one character.

## Empty text nodes

A text node committed with empty content is still a real
document object — Curvz does not auto-delete it. The empty node
takes up no visual space but appears in the Layers panel and the
Selection section. To remove it, select it (Selection tool, click
where it would be, or pick from Layers) and press **Delete**.

This matters when you're sketching out a layout — accidentally
clicking the Text tool on an empty area drops an invisible text
node. If your file feels like it has stray nothings, check the
Layers panel.

## What's not in the Text tool

A few things you might expect to be on the Text tool live
elsewhere:

- **Font, size, alignment** — all in the **Text** inspector
  section (5.4.2). Editable while the inline entry is open;
  changes show live.
- **Multi-line text** — not supported. One run per text node;
  the inline entry is single-line and won't accept newlines.
  For multi-line layouts, place separate text nodes per line.
- **Rich text** with mixed fonts or sizes within a run — not
  supported. One font per node.
- **Attaching the text to a path** — the **Text on Path** tool
  (4.5.2) covers this. Draw the text first with the Text tool;
  link it to a path with U.

## Where to next

The companion tool is **Text on Path** (4.5.2) for type that
follows a curve. For numeric editing of the text's font, size,
position, and alignment, the **Text** inspector section (5.4.2)
is the working surface once the text exists.

If you're building a logo or wordmark and want the type as
editable curves rather than as a text run, draw the text here
first, then run **Path → Convert Text to Path** (Ctrl+Alt+T).
The text node is replaced with the equivalent path geometry,
which you can then edit with the Node tool, apply boolean
operations to, and so on. The conversion is one-way — once
converted, the text is no longer editable as type.
