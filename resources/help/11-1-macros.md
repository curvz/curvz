# Macros

A **macro** in Curvz is a recorded sequence of operations you
can replay against a selection. Move, scale, rotate, set fill,
set stroke, group, align, run a boolean op, offset a path —
anything you do with the canvas tools or the toolbar can be
captured as a macro step and replayed later on different
artwork.

Macros are persistent across sessions — they're stored in
`~/.config/curvz/macros.json` and shared across every project on
the machine, the same way the Library is. Build a macro once and
it's available everywhere Curvz runs.

![The Macro Manager window with folders and macros](img/11-1-macro-manager.png)

## ❶ Opening the Macro Manager

`Ctrl + Shift + M` opens the **Macro Manager** ❶ — the central
hub for recording, organising, editing, and running macros. The
window has two columns: folders on the left, macros in the
selected folder on the right. Each macro shows its name and a
short summary of its steps.

From the manager you can:

- **Create folders** and **rename** them.
- **Create new macros**, **rename**, **duplicate**, **delete**,
  **move between folders**.
- **Star** a macro to add it to the Layers panel's macro strip
  (more on that below).
- **Edit** a macro's steps in the Macro Editor (see ❹).
- **Run** a macro against the current selection.

## ❷ Recording a new macro

To record:

1. Select the source object you want to operate on.
2. Open the Manager, create a new macro under a folder, give it
   a name.
3. Click **Record**. The Manager closes; the canvas re-takes
   focus with the recording indicator visible.
4. Perform the actions you want captured: move, scale, rotate,
   change colour, run a path operation. Each completes as
   normal, with the difference that it's also being appended
   to the macro.
5. Stop recording — open the Manager again and click **Stop**
   (or use the toolbar indicator).

Curvz captures one **MacroStep** per logical operation: a drag
becomes one Move with the final delta; a scale gesture becomes
one Scale; a fill change is one SetFill. Mid-gesture intermediate
states aren't recorded — the step is the final value, not the
journey.

The set of recordable operations covers:

- Object — Duplicate in Place, Duplicate, Delete, Group, Ungroup.
- Transform — Move, Scale, Rotate (around centre or named
  pivot), FlipH, FlipV.
- Alignment — Align L/CenterH/R/T/MiddleV/B, Distribute H/V.
- Style — SetFill, SetStroke, SetStrokeWidth, SetOpacity.
- Path — BooleanUnion, BooleanSubtract, BooleanIntersect,
  OffsetPath, ReversePath.
- Arrangement — BringToFront, SendToBack, BringForward,
  SendBackward.

## ❸ Running a macro

Three ways to run a macro:

- **Manager → Run** — click a macro in the right column and use
  the Run button. The Manager closes, the macro plays against
  the selection.
- **`Ctrl + M`** — runs the **current** macro. The current
  macro is whichever you last selected in the Manager (the one
  highlighted when you closed it). This is the keyboard
  shortcut to repeat the same macro on each new selection.
- **Layers panel macro strip** — see ❺ below.

A run is one undo step. Ctrl+Z reverts the entire macro's
effect; you don't have to undo step-by-step.

## ❹ Editing a macro

Open a macro and click **Edit** to launch the **Macro Editor**
❹ — a separate floating window. The editor is non-modal; you
can keep it open while you work on the canvas.

The editor lets you:

- **Reorder** steps with the up/down arrows.
- **Delete** a step.
- **Edit step parameters** — change the dx/dy of a Move, the
  factor of a Scale, the colour of a SetFill, the angle of a
  Rotate. Step types whose parameters don't apply (a Group
  step has no parameters; a FlipH has no parameters) just
  show a placeholder.
- **Run from here** — play the macro starting from the
  selected step, useful for debugging which step is doing
  something unexpected.

Step labels in the editor are auto-generated from the operation
plus its parameters (e.g. *Move (12, 0)*, *Set fill #FF8000*).
You can rename them by hand if you want clearer documentation.

## ❺ The Layers panel macro strip

At the very top of the **Layers** panel (6.2) is a collapsible
**Macros** ❺ section. It lists every macro you've **starred** in
the Manager, in folder order. Click a starred macro's button to
run it on the current selection — the same operation as
**Manager → Run**, but with no need to open the Manager.

Star a macro by toggling its star icon in the Manager; unstar to
remove it from the strip. The strip is per-document state — each
document remembers whether it last had the strip expanded or
collapsed.

For a macro you run constantly, starring it puts the run gesture
one click away. For a macro you run rarely, leave it unstarred
and use the Manager when needed.

## Macros vs Step and Repeat

It's worth being clear on the boundary between macros and
**Step and Repeat** (7.1):

- **Step and Repeat** is one specific operation — copies of the
  same selection at a regular offset, optionally rotated. It's
  parameterised by Copies, Offset, Rotate. One dialog, one go.
- **Macros** are arbitrary sequences of unrelated operations.
  They can include a Step and Repeat-equivalent (a series of
  Duplicates) but can also include things Step and Repeat
  doesn't do — colour changes, boolean ops, arrangement.

If your "I do this a lot" boils down to "place N copies offset
by X", reach for Step and Repeat. If it's "place a copy, recolour
it, group, align to ref point", reach for a macro.

## Where to next

- The **Macros** strip lives at the top of **Layers** (6.2).
- For one-shot cloning patterns, see **Step and Repeat** (7.1).
- For hotkey reference covering the macro shortcuts (`Ctrl + M`
  and `Ctrl + Shift + M`), see **Keyboard shortcuts** (11.2).
- For interactions where macros most often come in handy
  (alignment, path operations), see **Selection** (5.4.1) and
  **Editing paths** (8.1).
