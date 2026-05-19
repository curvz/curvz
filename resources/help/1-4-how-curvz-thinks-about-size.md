# How Curvz thinks about size

## The plane and the page

Curvz draws on a plane, not on paper. Set a Size in the Dimensions inspector
— 16 px, 16 inches, A4 — and that is how the drawing will deliver. But the
plane Curvz draws on, internally, is held at a generous working scale
regardless. The drawing lives in pure proportion; the size you typed in is
a contract for the moment of export. Until then you are working in
relationships: shapes, distances, curves, all measured against each other.

![Three rectangles sharing one corner, each with its diagonal showing its ratio](img/1-4-ratio-diagram.svg)

## Designing in ratios

This way of thinking is older than computers. Drafting students used to
learn it with a proportion wheel — a small plastic disk that let them turn
any object, of any size, into a ratio that fit on a sheet of vellum.
Architects, engineers and illustrators have always worked this way, because
the alternative — drawing everything at its real-world size — is
impractical the moment your subject is larger than your hand or smaller than
a coin. Vector graphics inherited this idea directly. PostScript, and
its smaller sibling SVG, describe a drawing as a *plane* of mathematical
relationships; the physical size is not realized until it is rendered.

Raster editors work differently. In a raster application like Photoshop
the canvas number *is* the size — a 16×16 image is sixteen pixels across,
full stop, and that number is both the working space and the final output.
In a vector tool the canvas number is a proportion, and a Size of "16 px"
just says where the drawing lands when it is rendered. The plane it was
drawn on is held at a working scale generous enough that every tool has
room to work properly, regardless of the Size you typed.

## Why the wider plane matters

Tools have a minimum useful precision. A handle that lands on a half-pixel
becomes a handle that lands on a whole one, and the curve suffers. Boolean
operations have a quality coefficient — unioning a circle with a square at
16-unit resolution gives you a chunky result, because there are only so
many places the algorithm can put a point. None of these are bugs in the
tool. They are what happens when the plane is too small for the algorithm
to work.

Curvz solves this by making the working plane generous regardless of what
Size you typed. You ask for a 16 px icon and Curvz draws it on a thousand-
unit plane; you ask for a 16-inch poster and Curvz still draws it on a
thousand-unit plane. Snap to grid means something. Boolean unions come
back clean. Handles land where the eye wants them. When you save, the
working plane is encoded in the SVG's `viewBox` and the Size you typed
becomes the SVG's `width` and `height`. The file delivers at the size you
asked for, and every renderer that opens it sees the drawing at the
precision it was made with.

## What you actually do

Set Size in the Dimensions inspector. Pick units that suit the work — `px`
for screen icons, `in` or `mm` for print, `pt` for typography. Use the
aspect-lock toggle if you want W and H to scale together. The presets are
there for the common cases.

That is all. The wider working plane is not something you configure; it is
how Curvz draws, the way a sketchpad is bigger than the eventual print.
The rulers, the inspector readouts, the layer panel coordinates — all read
in the Size units you set. The viewBox is the SVG file's bookkeeping.

## The payoff

A 24 px toolbar icon and a 24-inch poster are drawn the same way in Curvz,
with the same tools, at the same precision. Only the contract on the way
out is different. The tools never feel cramped at a small Size; boolean
operations produce results worth keeping; exports are clean because the
drawing they came from was made on a plane with room for it.

In other vector tools you would have set up the document at a generous
working scale yourself, and remembered to normalize on export. Curvz does
that part for you, every time, invisibly. The drawing is the same drawing;
only the rendering changes — and the rendering is yours to choose.

This is how Curvz wants to be used.
