# Animating SVGs

The welcome animation that plays on launch is not a baked-in feature.
It is a one-line script that Curvz happens to run for you on startup.
This page shows the pattern so you can build your own — replay a
favourite SVG on demand, queue several into a longer ceremony, or
wire any other arrangement you can write down.

## The verb

The animation engine is exposed to the Scripter as a single verb on
the `app` Scriptable:

```
app animate_svg "<path>" <speed>
```

The first arg is a filesystem path to an SVG file. The second arg is
the tempo — either a tempo name (recommended) or a raw number.

The tempo names match the welcome-speed setting in **Application ▸
Startup**:

| Name        | Feel                                    |
|-------------|-----------------------------------------|
| `very_slow` | Half-speed, contemplative               |
| `slow`      | Nominal — what the engine was tuned for |
| `medium`    | Brisk                                   |
| `fast`      | Snappy                                  |
| `very_fast` | Nearly instant, dramatic-reveal feel    |

The raw-number form takes a duration multiplier — smaller is faster.
`1.0` is the `slow` baseline; `0.5` matches `medium`; `0.1` matches
`very_fast`. Use the names unless you want a tempo between presets
(e.g. `0.3` for "snappier than medium but not quite fast").

## Replay your last SVG

The simplest script: open a fresh tab and animate one file into it.

```
proj new_doc 500 500
app animate_svg "/home/you/Pictures/my-icon.svg" very_fast
```

Save this as a `.curvzs` file in your scripts directory (the
Scripter's File menu can point you at where that is), and you can
replay it any time from the Scripter's script list.

## A two-act ceremony

You are not limited to one file. The verb returns immediately; the
animation runs asynchronously. To play several files in sequence,
queue them through separate `new_doc` + `animate_svg` pairs — each
animation lands in its own fresh tab and the next pair starts as
soon as you advance to it. (The engine does not currently chain
animations automatically; the Scripter's line-by-line execution is
what advances them.)

```
proj new_doc 500 500
app animate_svg "/home/you/Pictures/intro.svg" slow

proj new_doc 800 600
app animate_svg "/home/you/Pictures/finale.svg" very_fast
```

## Stopping an animation mid-run

The `app stop_animation` verb aborts whatever is currently playing.
It is the script-side equivalent of pressing **Esc** during
playback:

```
app stop_animation
```

Both surfaces — the verb and the Esc key — route through the same
abort path inside the animation engine. Already-drawn paths stay
on the canvas; only the in-progress phantoms wipe and no further
beats fire.

## What the welcome autoplay does

The welcome animation on startup is the simplest possible composition:
on launch, if the **Welcome animation** switch in **Application ▸
Startup** is on, Curvz scans `~/.config/curvz/welcome/` for SVGs, picks
one at random (or falls back to the bundled `scott-bug.svg` if the
folder is empty), opens a fresh `500 × 500` tab, and dispatches
`animate_svg` at your chosen welcome speed. That is the entire
boot-time behaviour, end to end.

If you want on-demand replayability — a menu item, a hotkey, anything
beyond "watch on launch" — that is the script you write. Drop it in
your scripts directory and run it whenever the mood strikes.
