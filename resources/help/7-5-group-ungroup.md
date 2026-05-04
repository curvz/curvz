# Group and Ungroup

Some operations bundle multiple objects into a single thing. Compound
paths combine paths by *melding* — two outlines become one fill region
with even/odd hole-cutting, and the original paths lose their
independence. **Group** does the opposite: it *collects* objects
together without merging them. Each child keeps its identity, its own
appearance, and its own anchors; what changes is that you now have a
single handle that moves, transforms, and selects all of them as a
unit.

## What a group is, and isn't

A group is a container. Internally it is a node in the scene tree
whose children are the objects you grouped — the same paths, text,
images or sub-groups you selected, now living one level deeper. The
container has no fill, no stroke, no geometry of its own; it is purely
structural. Click anywhere on a child and you select the whole group;
double-click to enter the group and edit a specific child; press `Esc`
to step back out.

A compound acts similarly in that it bundles paths in a single
container, but it renders very differently. A group draws each child
on its own — the rectangle is filled with the rectangle's fill, the
ellipse with the ellipse's fill, both painted side by side. A compound
treats the bundled outlines as a single fillable region governed by
the **even/odd rule**: at any point in space, count how many outlines
wrap around it. Odd → painted; even → hole. Two nested outlines
produce a ring. Three nested produce a ring with a disc inside. The
constituent paths' individual fills are gone; the compound has one
fill, applied through the even/odd computation, and that's how it
gets its hole-cutting behaviour. (For the full treatment see
**Compound paths**, 8.3.)

Group when you want to keep editing the children. Compound when you
want one fill region with holes.

The two operations sit beside each other in the **Path** menu for
that reason. They're sibling container operations with opposite
intents.

## Making and releasing a group

Select two or more objects and choose **Path → Group**, or press
`Ctrl + G`. The selected objects collapse into one row in the Layers
panel — a group node containing them as children. The group inherits
no fill or stroke of its own; it's a structural container, not a
visible object.

A grouped object's **stacking order is preserved.** If your selection
contained a back-most rectangle and a front-most ellipse, the group's
internal child order will read back-to-front the same way. Click order
during selection does not matter — Curvz reads the parent's existing
z-order and groups in that.

To release a group, select it and choose **Path → Ungroup**, or press
`Ctrl + Shift + G`. The container dissolves and its children return as
siblings at the layer position the group occupied. The children come
back selected, so any operation you intended for them next (a fresh
group with different members, an alignment pass, a delete) works
without re-selecting.

Both operations are undoable as single steps. Groups can be nested —
a group can contain other groups — and ungroup releases only one
level at a time, so deeply-nested structures are dismantled
incrementally rather than flattened all at once.

## When to reach for which

A small rule of thumb covers most cases: **if you'll keep editing the
children, group; if the children become one shape forever, compound.**
A small icon's pieces — a body, a few highlights, a shadow — usually
want a group, because you'll continue to nudge each piece. A donut
wants a compound, because the inner circle isn't a separate thing
anymore; it's a hole.

You can move freely between the two. A group can be ungrouped and the
children compounded if the design changes. A compound can be split and
the resulting paths grouped if the design changes back. Neither
operation is a one-way door.

## Where to next

- **Compound paths** (8.3) — the *melding* sibling of group, with the
  even/odd fill rule and the donut example.
- **Layers** (6.2) — how groups and other containers display in the
  Layers panel and how to enter and exit them for child editing.
- **Editing paths** (8.1) — node-level editing inside a group child.
