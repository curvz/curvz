#pragma once

// ── CSS
// ───────────────────────────────────────────────────────────────────────
//
// Curvz styles in two phases:
//
//   1. The :root block (the unqualified `window` rule below) defines a
//      vocabulary of design tokens via CSS custom properties. Every
//      colour in the rest of the stylesheet is expressed as var(--token).
//   2. The body of the stylesheet uses var() exclusively — no raw hex.
//
// Adding a Light motif (Phase B) is then a single `window.curvz-light {}`
// block that redefines the same tokens with light values. The CSS engine
// re-resolves var() throughout the tree when the class is added/removed
// from the root window — no provider swap, no second stylesheet to
// maintain.
//
// Token vocabulary (semantic, not visual):
//
//   Surface (backgrounds, dark→light):
//     --bg-deepest    statusbar, hard edges, deepest recess
//     --bg-base       window, panels, dialog content
//     --bg-surface    headerbar, toolbar, notebook header
//     --bg-raised     inputs, buttons, gallery thumbs
//     --bg-edge       borders, hover backgrounds, separator lines
//   Foreground (text):
//     --fg-primary    main text
//     --fg-secondary  bright muted (button text, inactive tabs in focus)
//     --fg-muted      secondary text, labels, inactive tabs
//     --fg-dim        dim text, low-emphasis separators
//   Accent (the GNOME blue family):
//     --accent        primary accent, suggested-action, active tab indicator
//     --accent-fg     text on accent surface
//     --accent-hover  accent under hover
//     --accent-deep   pressed/active state of accent
//     --accent-dim    accent at low emphasis (dimmed icons, etc.)
//     --accent-bg     accent-tinted background (selected row, focus chip)
//   State:
//     --selection-bg  text selection background
//     --hover-bg      generic hover background (where edge isn't right)
//     --focus-ring    keyboard focus ring colour
//   Status:
//     --warn-fg       warning text
//     --error-fg      destructive / error text
//     --error-bg      destructive / error background
//   Special:
//     --white-pure    explicit pure white (rare, where var(--white-pure) is the
//     design)
//
// Phase A (this milestone) ships the token block + replaces all hex
// literals with var() — no visual change. Phase B adds the light fork.
// Phase C wires Canvas to read tokens via style_context lookup_color so
// editor chrome (rulers, guides, node dots) themes too.
//
static const char *CURVZ_CSS = R"css(
/* ── Token definitions (Dark — the default motif) ─────────────────────
   Defined on `window` (root). The Light motif (Phase B) will override
   these in `window.curvz-light`. Every other rule references tokens
   via var(--name). */
window {
    --bg-deepest:    #141414;
    --bg-base:       #1e1e1e;
    --bg-surface:    #252525;
    --bg-raised:     #2e2e2e;
    --bg-edge:       #383838;

    --fg-primary:    #e0e0e0;
    --fg-secondary:  #cccccc;
    --fg-muted:      #999999;
    --fg-dim:        #555555;

    --accent:        #3584e4;
    --accent-fg:     #ffffff;
    --accent-hover:  #5aa3f0;
    --accent-deep:   #2e6da8;
    --accent-dim:    #305581;
    --accent-bg:     #1e3a5c;

    --selection-bg:  #305581;
    --hover-bg:      #444444;
    --focus-ring:    #3584e4;

    --warn-fg:       #e8a838;
    --error-fg:      #f5b5b5;
    --error-bg:      #6a2a2a;

    --white-pure:    #ffffff;
}

/* ── Token definitions (Light motif) ────────────────────────────────────
   Active when the main window has the `curvz-light` class. Selector
   `window.curvz-light` outranks `window` so these declarations win.
   The CSS engine re-resolves var() references throughout the tree as
   soon as the class is added/removed — no provider swap needed.

   Design principles applied here:
   • Surface hierarchy preserved. Dark scale ran deepest→edge ascending
     visual prominence; light scale runs the same direction (subtly
     darker base, slightly lighter raised) so the "deeper recess →
     foreground surface" gradient still reads.
   • Foregrounds dark for legibility. Primary near-black, secondary/
     muted/dim go progressively lighter (less contrast) — same
     emphasis ladder as dark mode, inverted absolute values.
   • Accent stays GNOME blue — it's designed to read on both. Hover
     in light mode shifts deeper rather than brighter (brighter against
     a light surface adds glare, not emphasis).
   • Error/warn flip: dark-red text on light-pink ground, light-orange
     fg stays warm-toned.
   • white-pure stays #ffffff — it's "explicit pure white", motif-
     independent. Used as icon-fg on accent surfaces.

   These values are best-judgement v1; a later session can tune
   specific tokens once Scott's eye has lived with them. */
/* S124 m1: rebalanced surface ramp.
   v1 problem: ramp was non-monotonic and stretched too wide.
     bg-deepest #b8 = bg-edge #b8  (deepest = edge, no separation)
     bg-base    #f5 > bg-surface #dd  (panel BRIGHTER than headerbar — inverts dark-mode convention)
     bg-raised  #ff (pure white)        (24-unit jump from bg-surface #dd)
   The whole light surface lived in a 184→255 = 71-unit chasm; dark
   mode lives in 20→68 = 48 units. Every adjacent-token transition
   was loud.

   v2 (this milestone): smooth monotonic ramp from deepest recess
   (192) to raised foreground (251), 6 stops, 8–13 units apart.
   Surface > base (matching dark-mode convention: chrome reads as
   "slightly lifted" off the panel, not a distinct band). Hover
   darker than base (light-mode press-in feel — inverse of dark
   mode's lighter-than-base hover). Pure white retired from the
   token ramp; --white-pure stays for explicit white-on-accent
   roles, --bg-raised slightly off-white so true whites can still
   pop. */
window.curvz-light {
    /* S124 m1 v3: shifted whole ramp down ~10 units (Scott: "darken
       a little bit more overall"). Step sizes and monotonic order
       preserved — only the absolute values move. */
    --bg-deepest:    #b4b4b4;   /* statusbar, hard edges — heaviest grey */
    --bg-edge:       #c0c0c0;   /* borders, hover-alt — clear edge against base */
    --hover-bg:      #cccccc;   /* generic hover — darker than base (press-in direction) */
    --bg-base:       #d6d6d6;   /* window, panels — mid-grey */
    --bg-surface:    #dddddd;   /* headerbar, toolbar — slightly lifted off base */
    --bg-raised:     #e5e5e5;   /* inputs, buttons, gallery thumbs — softly raised (pure white retired) */

    --fg-primary:    #0a0a0a;   /* main text — near-black */
    --fg-secondary:  #1a1a1a;   /* button text, inactive-but-visible — s124 m1 v4: collapsed toward near-black */
    --fg-muted:      #222222;   /* secondary text, labels, icons — s124 m1 v4: collapsed toward near-black */
    --fg-dim:        #333333;   /* low-emphasis separators, dim text — s124 m1 v4: collapsed toward near-black */

    --accent:        #3584e4;   /* brand blue — works on both motifs */
    --accent-fg:     #ffffff;   /* text on accent (always white) */
    --accent-hover:  #2671d6;   /* deeper, not brighter, in light mode */
    --accent-deep:   #1e5cb8;   /* pressed/active state */
    --accent-dim:    #6ba4e8;   /* dimmed accent */
    --accent-bg:     #d6e6f8;   /* accent-tinted highlight ground */

    --selection-bg:  #b8d4f4;   /* text selection — soft blue tint */
    --focus-ring:    #3584e4;   /* keyboard focus — same accent blue */

    --warn-fg:       #b87000;   /* warning text — darker amber */
    --error-fg:      #8a1a1a;   /* error text — dark red on light ground */
    --error-bg:      #f5d0d0;   /* error background — soft pink */

    --white-pure:    #ffffff;   /* explicit pure white, motif-independent */
}

/* ── Window base ─────────────────────────────────────────── */
window, .background, .dialog-content {
    background-color: var(--bg-base);
    color: var(--fg-primary);
}
/* S117 m2: GTK4 client-side-decoration (CSD) windows insert an
   internal `box.vertical` between the `window` node and its
   content. We never had a rule for it — it defaulted to black
   (rgb 0,0,0) — and on light motif that black showed through
   wherever the headerbar/content didn't perfectly cover.
   Symptom: in light motif the headerbar appeared dark; opening
   GTK Inspector triggered a global style invalidation that
   masked it briefly, but the underlying paint was always there.
   Diagnosis confirmed via inspector CSS Nodes panel — headerbar
   correctly paints rgb(221,221,221) = #dddddd in light, but
   parent box.vertical paints (0,0,0).
   Fix: paint the CSD wrapper with the same bg-base as the
   window so the chrome reads as one continuous surface. */
window.csd > box.vertical {
    background-color: var(--bg-base);
}

/* ── Headerbar ─────────────────────────────────────────────
   `headerbar.titlebar` selector confirmed to win the cascade
   in inspector — already paints rgb(221,221,221) (#dddddd) in
   light motif, the correct --bg-surface token. The visible
   "dark headerbar" was actually the unstyled CSD wrapper box
   bleeding through; see comment block immediately above. */
headerbar.titlebar {
    background-color: var(--bg-surface);
    border-bottom: 1px solid var(--bg-deepest);
    color: var(--fg-primary);
    box-shadow: none;
    /* S60 M3: default GTK4 HeaderBar has significant internal padding
       on the title region that creates a visible gap between the
       pack_start widgets and the title widget (our DocTabBar). Zero it
       out so the DocTabBar can sit flush against the logo. */
    padding: 0;
    min-height: 46px;
}
/* S60 M3: the internal CenterBox of HeaderBar spaces its three slots.
   Remove spacing so center-slot (DocTabBar) expands to the actual
   available width between start and end slots. */
headerbar.titlebar > windowhandle > box,
headerbar.titlebar > windowhandle > box.center {
    padding: 0;
}
headerbar.titlebar .title,
headerbar.titlebar .subtitle { color: var(--fg-primary); }
headerbar.titlebar button {
    background-color: transparent;
    color: var(--fg-secondary);
    border: none;
    border-radius: 5px;
    padding: 4px 6px;
    min-height: 0;
    box-shadow: none;
}
headerbar.titlebar button:hover  { background-color: var(--bg-edge); color: var(--fg-primary); }
headerbar.titlebar button:active { background-color: var(--bg-edge); color: var(--fg-primary); }
/* s190 m2 — Diagnostic-mode Scripter toggle in the headerbar. :checked
   means the Scripter window is currently visible; unchecked means it's
   hidden. Accent blue mirrors the tool-btn:checked styling so "active /
   engaged" reads consistently across the app. The button is wired
   bidirectionally with the Scripter's property_visible so the X-button
   close path also flips the toggle.
   s190 m2-final — added `background-image: none` to override system-
   theme gradients painted on headerbar buttons. The toolbar's
   .tool-btn:checked rule has the same line for the same reason. Without
   it, background-color is masked by the theme gradient and the blue
   never shows through. */
headerbar.titlebar button.mw-scripter-btn:checked,
headerbar.titlebar button.mw-scripter-btn:checked:hover {
    background-color: var(--accent-deep);
    background-image: none;
    color: #ffffff;
}
headerbar.titlebar button.mw-scripter-btn:checked image {
    color: #ffffff;
}

/* s191 m3 — Diagnostic-mode caption bar above the status bar.
   Subtitled scripts (`#[sub]` lines in the Scripter) show captions
   here while paced playback runs. Translucent accent-tinted
   background (30% alpha) so the canvas reads through — the caption
   narrates the action without blocking it. The label itself stays
   fully opaque so the text remains readable against whatever's
   showing through. */
.mw-caption-bar {
    background-color: alpha(var(--accent-bg), 0.3);
    border-top: 1px solid alpha(var(--bg-deepest), 0.3);
    border-bottom: 1px solid alpha(var(--bg-deepest), 0.3);
}
.mw-caption-label {
    color: var(--fg-primary);
    font-size: 16pt;
    font-weight: 500;
    padding: 10px 16px;
}

/* ── Notebook (tabs + page) ──────────────────────────────── */
notebook > header {
    background-color: var(--bg-surface);
    border-bottom: 1px solid var(--bg-deepest);
    box-shadow: none;
    padding: 0;
}
notebook > header tab {
    background-color: transparent;
    color: var(--fg-muted);
    font-size: 12px;
    padding: 5px 14px;
    border: none;
    border-bottom: 2px solid transparent;
    border-radius: 0;
    margin: 0;
}
notebook > header tab:checked {
    background-color: transparent;
    color: var(--fg-primary);
    border-bottom: 2px solid var(--fg-primary);
}
notebook > header tab:hover:not(:checked) {
    background-color: var(--bg-raised);
    color: var(--fg-secondary);
}
/* Page content — must be explicit or GTK renders white */
notebook > stack,
notebook stack {
    background-color: var(--bg-base);
    color: var(--fg-primary);
}

/* ── All buttons (default) ─────────────────────────────────
   S117 m8: include `background-image: none`. Same diagnosis as m5
   (disabled toolbar buttons), m6 (dropdowns), m7 (checkboxes) —
   system theme paints a grey `background-image` on `button` that
   sits above our `background-color`, so the white `--bg-raised`
   token never reaches the screen in light motif. Repeat the
   override on the state rules so hover/active don't reintroduce
   the image. */
button {
    background-color: var(--bg-raised);
    background-image: none;
    color: var(--fg-primary);
    border: 1px solid var(--bg-edge);
    border-radius: 5px;
    padding: 5px 10px;
    box-shadow: none;
    outline: none;
}
button:hover  { background-color: var(--bg-edge); background-image: none; color: var(--fg-primary); }
button:active { background-color: var(--hover-bg); background-image: none; color: var(--fg-primary); }
button:disabled { opacity: 0.4; }

/* Flat buttons (toolbar icons, preset pills, etc.) */
button.flat {
    background-color: transparent;
    border-color: transparent;
    color: var(--fg-secondary);
    padding: 4px 8px;
}
button.flat:hover  { background-color: var(--bg-raised); border-color: var(--bg-edge); color: var(--fg-primary); }
button.flat:active { background-color: var(--bg-edge); }

/* ── Properties panel ────────────────────────────────────── */

/* Section sub-header inside properties panel */
/* ── Properties panel ────────────────────────────────────────────────────────
   Design targets:
   - Row height: 22px (tight but readable — matches Linear/Figma inspector)
   - Label column: 44px fixed, 10px, var(--fg-dim)
   - Values: 11px, var(--fg-secondary)
   - Toggles: 18px tall, no hexpand stretching to full width
   ─────────────────────────────────────────────────────────────────────────── */

.prop-section-hdr {
    padding: 10px 10px 3px;
    background-color: transparent;
}
.prop-section-lbl {
    font-size: 10px;
    font-weight: bold;
    /* S117 m1: was var(--hover-bg) — bg token used as fg colour. In
       dark mode --hover-bg #444 reads as muted text against #1e1e1e
       base, but in light mode #e0e0e0 against #f5f5f5 is invisible.
       Switch to fg-muted, the proper "secondary text" role. */
    color: var(--fg-muted);
    letter-spacing: 0.6px;
    text-transform: uppercase;
}

/* Base property row */
.prop-row {
    padding: 4px 10px;
    min-height: 24px;
}
.guide-row {
    padding: 2px 6px;
    min-height: 22px;
    border-radius: 4px;
}
.guide-row-selected {
    background-color: alpha(var(--accent-hover), 0.25);
}
.prop-lbl {
    font-size: 10px;
    color: var(--fg-muted);
    min-width: 40px;
    /* Guarantee at least 8px of breathing room between key label and value
       even when the label fully fills its set_width_chars allocation
       (e.g. "Documents" at width-chars=9). Without this, long labels can
       butt directly against the value. Structural fix: every prop-row
       across the Inspector benefits, no per-row tuning required. */
    margin-right: 8px;
}
.csb-error > contents {
    background-color: var(--error-bg);
    color: var(--error-fg);
    border: 1px solid var(--error-bg);
    border-radius: 4px;
}
.csb-error label {
    font-size: 11px;
    color: var(--error-fg);
}
button.prop-xf-apply,
button.prop-xf-apply:hover,
button.prop-xf-apply:active,
button.prop-xf-apply:checked {
    padding: 0;
    margin: 0;
    min-height: 0;
    min-width: 0;
    border: none;
    background: transparent;
    box-shadow: none;
    outline: none;
}
button.prop-xf-apply > label {
    padding: 0;
    margin: 0;
}
.prop-dropdown {
    font-size: 11px;
    color: var(--fg-muted);
}
/* The global rule at line ~408 sets bg on BOTH the `dropdown` element AND
   its inner `dropdown button`. Killing the bg on only one leaves the other
   bleeding through. Specificity needs a class chained with the element:
   `dropdown.prop-dropdown` (0,1,1) beats `dropdown` (0,0,1), and
   `dropdown.prop-dropdown button` (0,1,2) beats `dropdown button` (0,0,2).
   GTK's CSS parser rejects `!important` (silently makes the whole rule
   invalid — that's what the v7 theme-parser warnings were), so we win
   purely on specificity. */
dropdown.prop-dropdown,
dropdown.prop-dropdown button {
    background-color: transparent;
    border-color: transparent;
    box-shadow: none;
}
dropdown.prop-dropdown button:hover {
    background-color: var(--bg-raised);
    border-color: transparent;
}
dropdown.prop-dropdown button label {
    font-size: 11px;
    color: var(--fg-secondary);
}
/* Orientation buttons (Portrait / Landscape) on the canvas Mode row. The
   inactive button is a dim flat icon; the active button (matching the
   current canvas orientation) gets a subtle blue tint + brighter glyph so
   the user can see at a glance which orientation they're on. */
button.orient-btn {
    min-width:  22px;
    min-height: 22px;
    padding:    2px;
    margin:     0;
    color:      var(--fg-muted);
}
button.orient-btn:hover {
    color: var(--fg-secondary);
    background-color: rgba(255, 255, 255, 0.05);
}
button.orient-btn.orient-active {
    color: var(--accent-hover);
    background-color: rgba(58, 155, 240, 0.12);
}
button.orient-btn.orient-active:hover {
    color: var(--accent-hover);
    background-color: rgba(58, 155, 240, 0.2);
}
.prop-entry {
    font-size: 10px;
    min-height: 0;
    padding: 1px 4px;
}
.layer-name-entry {
    font-size: 13px;
    min-height: 0;
    padding: 1px 4px;
}
.prop-val-lbl {
    font-size: 11px;
    color: var(--fg-secondary);
}

/* Colour well */
.color-well {
    border-radius: 3px;
    margin-right: 2px;
}

/* Hex entry — compact */
.prop-hex-entry {
    font-size: 11px;
    font-family: monospace;
    min-height: 18px;
    padding: 0 4px;
    background-color: var(--bg-base);
    border: 1px solid var(--bg-raised);
    border-radius: 3px;
    color: var(--fg-secondary);
}
.prop-hex-entry:focus { border-color: var(--hover-bg); }

/* Width entry (number + units) */
.prop-width-entry {
    font-size: 11px;
    font-family: monospace;
    min-height: 20px;
    padding: 0 5px;
    background-color: var(--bg-base);
    border: 1px solid var(--bg-raised);
    border-radius: 3px 0 0 3px;
    color: var(--fg-secondary);
}
.prop-width-entry:focus { border-color: var(--hover-bg); }
.prop-width-unit {
    font-size: 10px;
    color: var(--fg-dim);
    background-color: var(--bg-base);
    border: 1px solid var(--bg-raised);
    border-left: none;
    border-radius: 0 3px 3px 0;
    padding: 0 5px;
    min-height: 20px;
}

/* Swatches row — smaller, tighter */
.prop-swatches {
    padding: 3px 10px 8px;
}
button.swatch-btn {
    padding: 0;
    min-width: 14px;
    min-height: 14px;
    background-color: transparent;
    border: 1px solid var(--bg-surface);
    border-radius: 2px;
}
button.swatch-btn:hover { border-color: var(--fg-muted); }

/* Toggle pills — compact, NOT full-width */
togglebutton.prop-toggle,
button.prop-toggle {
    background-color: var(--bg-base);
    color: var(--hover-bg);
    border: 1px solid var(--bg-base);
    border-radius: 3px;
    padding: 0 6px;
    font-size: 10px;
    min-height: 17px;
    min-width: 0;
    box-shadow: none;
}
togglebutton.prop-toggle:hover,
button.prop-toggle:hover {
    background-color: var(--bg-base);
    color: var(--fg-muted);
}
togglebutton.prop-toggle:checked,
button.prop-toggle:checked {
    background-color: var(--accent-bg);
    color: var(--accent-hover);
    border-color: var(--accent-deep);
    box-shadow: none;
}

/* Closed/Open state button — compact, inline, not full-width */
togglebutton.prop-toggle-closed {
    background-color: var(--accent-bg);
    color: var(--accent-hover);
    border-color: var(--accent-deep);
}
togglebutton.prop-toggle-open {
    background-color: var(--bg-base);
    color: var(--fg-dim);
    border-color: var(--bg-surface);
}

/* Visual picker cells — smaller, tighter */
.visual-picker-cell { border-radius: 4px; }

/* Visual picker button wrapper — transparent, no chrome */
button.visual-picker-btn {
    background-color: transparent;
    border: none;
    border-radius: 4px;
    padding: 0;
    min-width: 0;
    min-height: 0;
    box-shadow: none;
}
button.visual-picker-btn:hover  { background-color: transparent; }
button.visual-picker-btn:active { background-color: transparent; }

/* Destructive */
button.destructive-action {
    background-color: var(--error-bg);
    color: var(--error-fg);
    border-color: var(--error-bg);
}
button.destructive-action:hover { background-color: var(--error-bg); }

/* Suggested (primary action — blue OK/Apply/Confirm).
   GTK4 has a built-in `suggested-action` style that paints the button
   blue, but its text colour comes from the *system* GTK theme's
   foreground — which tracks the system's dark/light preference, not
   Curvz's motif class. The mismatch shows up as black-text-on-blue
   in our light mode, where the system theme would normally pair blue
   with white but our motif overrides shift the foreground to dark.
   Explicit rule fixes both motifs uniformly. */
button.suggested-action,
button.suggested-action label {
    background-color: var(--accent);
    color: var(--accent-fg);
    border: 1px solid var(--accent-deep);
}
button.suggested-action:hover {
    background-color: var(--accent-hover);
    color: var(--accent-fg);
}
button.suggested-action:active {
    background-color: var(--accent-deep);
    color: var(--accent-fg);
}
button.suggested-action:disabled {
    opacity: 0.4;
}

/* ── Text inputs & spin buttons ──────────────────────────── */
entry,
spinbutton,
spinbutton text {
    background-color: var(--bg-base);
    color: var(--fg-primary);
    border: 1px solid var(--bg-edge);
    border-radius: 5px;
    caret-color: var(--fg-primary);
    box-shadow: none;
}
entry:focus,
spinbutton:focus { border-color: var(--fg-muted); }
entry selection,
spinbutton selection {
    background-color: var(--fg-dim);
    color: var(--white-pure);
}
/* Spin buttons — keep the +/- buttons dark */
spinbutton button {
    background-color: var(--bg-raised);
    border: none;
    border-left: 1px solid var(--bg-edge);
    border-radius: 0;
    color: var(--fg-secondary);
    min-width: 28px;
    padding: 0;
}
spinbutton button:hover  { background-color: var(--bg-edge); }
spinbutton button:active { background-color: var(--hover-bg); }

/* Node grid handle labels — alt-click retracts handle to node */
.hdl-reset-lbl { }
.hdl-reset-lbl:hover { color: var(--accent); }
.node-spin button {
    min-width:  14px;
    min-height: 0;
    padding:    0 2px;
    font-size:  9px;
}

/* ── Dropdowns ───────────────────────────────────────────── */
/* S117 m6: include `background-image: none`. Same diagnosis as the
   m5 disabled-toolbar-button fix — system theme paints a grey
   background-image on dropdown / dropdown button that sits above our
   background-color, so without explicitly clearing the image our
   `--bg-raised` token never reaches the screen. Inspector confirmed
   the same image-overlay pattern. */
dropdown,
dropdown button,
combobox,
combobox button {
    background-color: var(--bg-raised);
    background-image: none;
    color: var(--fg-primary);
    border: 1px solid var(--bg-edge);
    border-radius: 5px;
    box-shadow: none;
}
dropdown button:hover,
combobox button:hover { background-color: var(--bg-edge); }
dropdown button arrow,
combobox button arrow { color: var(--fg-muted); }

/* Dropdown popover list */
popover {
    background-color: var(--bg-surface);
    border: 1px solid var(--bg-edge);
    border-radius: 6px;
    box-shadow: none;
}
popover contents { background-color: var(--bg-surface); }
popover listview,
popover list { background-color: transparent; }
popover row,
listview row {
    background-color: transparent;
    color: var(--fg-primary);
    padding: 4px 8px;
    border-radius: 4px;
}
/* S117 m13: hover/active for menu items — both the `row` flow
   (popover with custom listbox content) and the `modelbutton` flow
   (Gio::Menu via PopoverMenu — every File/Edit/View menu uses this).
   Previously only :row had a :hover rule, so PopoverMenu items fell
   through to the system theme on hover and painted near-black-on-
   black in light mode. Use --bg-edge (medium grey both motifs) with
   explicit fg-primary text + background-image: none to win against
   the system theme image-bleed pattern. */
popover row:hover,
listview row:hover,
modelbutton:hover,
popover modelbutton:hover {
    background-color: var(--bg-edge);
    background-image: none;
    color: var(--fg-primary);
}
popover row:active,
listview row:active,
modelbutton:active,
popover modelbutton:active {
    background-color: var(--bg-edge);
    background-image: none;
    color: var(--fg-primary);
}
popover row:selected,
listview row:selected {
    background-color: var(--bg-edge);
    background-image: none;
    color: var(--fg-primary);
}
/* Modelbutton base — explicit transparent so we win against system
   theme's modelbutton bg. Text inherits from popover. */
modelbutton,
popover modelbutton {
    background-color: transparent;
    background-image: none;
    color: var(--fg-primary);
    padding: 4px 8px;
    border-radius: 4px;
}
/* Disabled menu items — Gio::SimpleAction::set_enabled(false) sets the
 * :disabled pseudo on the corresponding menu widget. In GTK4 a
 * PopoverMenu renders each item as a `modelbutton`, not a `row`, so
 * the rule needs both selectors. Matches button:disabled (line ~93)
 * so the theme feels consistent. */
popover row:disabled,
listview row:disabled,
modelbutton:disabled,
popover modelbutton:disabled {
    opacity: 0.4;
}

/* ── Scale (slider) ──────────────────────────────────────── */
/* S117 m10: re-tuned for light motif. Original used --fg-dim for the
   filled trough and --fg-secondary for the knob, which both work on
   dark surfaces (light values on dark) but on light surfaces resolved
   to too-dark greys (#7a7a7a fill, #2a2a2a knob). Standard convention
   is to use the brand accent for the active portions of a slider —
   filled trough + knob — which reads correctly in both motifs without
   needing motif-specific overrides (--accent is motif-invariant).
   background-image: none defeats the system-image bleed pattern we've
   hit repeatedly this session. */
scale { color: var(--fg-primary); }
scale trough,
scale trough contents {
    background-color: var(--bg-raised);
    background-image: none;
    border: 1px solid var(--bg-edge);
    border-radius: 3px;
    min-height: 4px;
}
scale trough highlight,
scale highlight {
    background-color: var(--accent);
    background-image: none;
    border-radius: 3px;
    min-height: 4px;
}
scale slider {
    background-color: var(--accent);
    background-image: none;
    border: 1px solid var(--accent-deep);
    border-radius: 50%;
    min-width: 14px;
    min-height: 14px;
    margin: -5px 0;
    box-shadow: none;
    -gtk-icon-source: none;
}
scale slider:hover  { background-color: var(--accent-hover); border-color: var(--accent-deep); }
scale slider:active { background-color: var(--accent-deep); border-color: var(--accent-deep); }
scale marks mark indicator {
    background-color: var(--fg-dim);
    min-width: 1px;
    min-height: 6px;
}
/* ── Scrollbars ──────────────────────────────────────────── */
scrollbar {
    background-color: transparent;
    border: none;
}
scrollbar trough { background-color: var(--bg-base); }
scrollbar slider {
    background-color: var(--bg-edge);
    border-radius: 4px;
    min-width: 6px;
    min-height: 6px;
}
scrollbar slider:hover  { background-color: var(--fg-dim); }
scrollbar slider:active { background-color: var(--fg-muted); }

/* ── Check & radio ─────────────────────────────────────────
   S117 m7: rebuilt for both motifs.
   Unchecked:  stroked rect — transparent bg, fg-muted border, no glyph.
   Checked:    accent-blue fill with white tick.
   background-image: none defeats the system theme's grey image overlay
   (same shape as the m5 disabled-button + m6 dropdown bug we kept hitting). */
checkbutton check,
radiobutton radio {
    background-color: transparent;
    background-image: none;
    border: 1px solid var(--fg-muted);
    border-radius: 3px;
    color: transparent;  /* hide glyph in unchecked state */
}
checkbutton check:checked,
radiobutton radio:checked {
    background-color: var(--accent);
    background-image: none;
    border-color: var(--accent);
    color: var(--white-pure);  /* white tick on blue body */
}
checkbutton label,
radiobutton label { color: var(--fg-primary); }

/* ── Separators ──────────────────────────────────────────── */
separator { background-color: var(--bg-edge); min-height: 1px; min-width: 1px; }

/* ── Tooltips ────────────────────────────────────────────── */
tooltip {
    background-color: var(--bg-surface);
    color: var(--fg-primary);
    border: 1px solid var(--bg-edge);
    border-radius: 4px;
    padding: 4px 8px;
}
tooltip label { color: var(--fg-primary); }

/* ── Labels (generic) ────────────────────────────────────── */
label { color: var(--fg-primary); }

/* ══════════════════════════════════════════════════════════
   CURVZ CUSTOM COMPONENT CLASSES
   ══════════════════════════════════════════════════════════ */

/* ── Context bar ─────────────────────────────────────────── */
.context-bar {
    background-color: var(--bg-base);
    border-bottom: 1px solid var(--bg-deepest);
    min-height: 28px;
    padding: 0;
}

/* Tool identity: icon glyph */
.ctx-tool-icon {
    font-size: 13px;
    color: var(--fg-primary);
    min-width: 16px;
}

/* Tool name */
.ctx-tool-name {
    font-size: 11px;
    font-weight: bold;
    color: var(--fg-muted);
    letter-spacing: 0.4px;
}

/* Plain hint text */
.ctx-hint {
    font-size: 11px;
    color: var(--fg-dim);
    margin-left: 2px;
    margin-right: 2px;
}

/* Keyboard key badge */
.ctx-key {
    font-size: 10px;
    color: var(--fg-muted);
    background-color: var(--bg-surface);
    border: 1px solid var(--bg-edge);
    border-bottom-width: 2px;
    border-radius: 3px;
    padding: 0px 5px;
    min-height: 16px;
    font-family: monospace;
    margin-left: 3px;
    margin-right: 3px;
}

/* Action buttons in the centre zone */
.ctx-action-btn {
    background-color: transparent;
    border: none;
    border-radius: 5px;
    color: var(--fg-muted);
    min-width: 26px;
    min-height: 22px;
    padding: 2px 4px;
    margin: 0 1px;
}
.ctx-action-btn:hover  { background-color: var(--bg-raised); color: var(--fg-primary); }
.ctx-action-btn:active { background-color: var(--bg-edge); }
.ctx-action-btn:checked {
    background-color: var(--bg-raised);
    border: 1px solid var(--hover-bg);
    color: var(--fg-primary);
}

/* (ctx-zoom/fit classes removed — zoom controls moved to Toolbar) */
/* (ctx-well classes removed — defaults well moved to Toolbar) */

/* ── Toolbar ─────────────────────────────────────────────── */
/*
 * s152: CSS-driven active state. The previous implementation
 * allocated a Gtk::CssProvider per call to select_tool() and
 * loaded raw hex literals to override Adwaita. That hack is
 * gone; .tool-btn:checked carries the active styling here, and
 * select_tool() now only flips the toggle's checked bit.
 *
 * Tokens: every colour goes through var(--token) so light motif
 * works automatically. The active blue uses --accent-deep (the
 * GNOME blue dark-shade family token) so light motif gets a
 * darker accent that reads correctly on a light surface.
 *
 * Density: four classes on the .toolbar-panel root scale every
 * sizing rule. Comfortable is the default rule set (no extra
 * class). .standard / .compact / .tight stack on top, narrowing
 * the column and shrinking buttons. set_density() flips the
 * class; GTK relayouts on the change, no widget rebuild.
 *
 *   Comfortable  (no class)     48px col, 40×40 buttons   curvz historical
 *   Standard     .standard      40px col, 32×32 buttons   curvz default
 *   Compact      .compact       32px col, 28×28 buttons
 *   Tight        .tight         28px col, 24×24 buttons
 */
.toolbar-panel {
    background-color: var(--bg-surface);
    border-right: 1px solid var(--bg-deepest);
    min-width: 48px;
    padding: 4px 2px;
}
.toolbar-panel > separator {
    background-color: var(--bg-edge);
    margin: 4px 0;
    min-height: 1px;
}
/* Tool buttons (.tool-btn class applied by add_tool_button).
   Selector targets .tool-btn directly so :checked rules apply
   regardless of widget type (togglebutton, etc). */
.toolbar-panel .tool-btn {
    min-width: 40px;
    min-height: 40px;
    padding: 0;
    border-radius: 4px;
    background-color: transparent;
    background-image: none;
    border: none;
    box-shadow: none;
    color: var(--fg-muted);
    transition: background-color 80ms ease, color 80ms ease;
}
.toolbar-panel .tool-btn image {
    color: var(--fg-muted);
}
.toolbar-panel .tool-btn:hover {
    background-color: var(--hover-bg);
    color: var(--fg-primary);
}
.toolbar-panel .tool-btn:hover image {
    color: var(--fg-primary);
}
/* Active state — replaces the inline-CSS provider hack.
   --accent-deep matches the GNOME-family selection blue used
   throughout curvz; both motifs supply a tone that reads as
   "engaged" without colliding with hover. */
.toolbar-panel .tool-btn:checked {
    background-color: var(--accent-deep);
    background-image: none;
    color: #ffffff;
}
.toolbar-panel .tool-btn:checked image {
    color: #ffffff;
}
.toolbar-panel .tool-btn:checked:hover {
    background-color: var(--accent);
}
/* s152 — Snap toggle exception. The snap button uses the same .tool-btn
   sizing/hover/density rules but the on/off state is conveyed by the
   icon itself (curvz-switch-on/off-symbolic), not by background colour.
   The `.tb-snap-btn` class (added alongside `.tool-btn` on the snap
   button) wins specificity here and reverts the :checked background to
   transparent. Hover still highlights normally. */
.toolbar-panel .tool-btn.tb-snap-btn:checked,
.toolbar-panel .tool-btn.tb-snap-btn:checked:hover {
    background-color: transparent;
    background-image: none;
    color: var(--fg-primary);
}
.toolbar-panel .tool-btn.tb-snap-btn:checked image {
    color: var(--fg-primary);
}
.toolbar-panel .tool-btn.tb-snap-btn:checked:hover {
    background-color: var(--hover-bg);
}
/* S117 m5 (preserved): hold disabled toolbar buttons transparent.
   The system theme paints a grey background-image on :disabled —
   we clear both background-color and background-image. opacity
   stays for icon de-emphasis. */
.toolbar-panel .tool-btn:disabled {
    background-color: transparent;
    background-image: none;
    opacity: 0.4;
}
/* s152 sub-ship 1: faked-disabled look. Used by widgets that need to
   *appear* disabled while staying GTK-sensitive — so they can still
   absorb events (e.g. the right-click swallow on the align button
   needs to fire even when align has nothing to align, otherwise
   right-click bubbles up to the toolbar density picker). The action
   itself is gated in the click handler.
   Visual matches the real :disabled rule; hover is suppressed (a
   sensitive widget would otherwise paint --hover-bg, contradicting
   the disabled look). */
.toolbar-panel .tool-btn.tool-btn-disabled {
    background-color: transparent;
    background-image: none;
    opacity: 0.4;
}
.toolbar-panel .tool-btn.tool-btn-disabled:hover,
.toolbar-panel .tool-btn.tool-btn-disabled:hover image {
    background-color: transparent;
    color: var(--fg-muted);
}
/* Legacy fallback for any toolbar buttons NOT yet carrying the
   .tool-btn class (e.g. align/macro built before this refactor).
   Same rule shape as before; remove once all toolbar buttons are
   .tool-btn-tagged. */
.toolbar-panel togglebutton:not(.tool-btn),
.toolbar-panel button:not(.tool-btn) {
    background-color: transparent;
    border: none;
    border-radius: 4px;
    color: var(--fg-muted);
    min-width: 0;
    min-height: 0;
    padding: 0;
}
.toolbar-panel togglebutton:not(.tool-btn):hover,
.toolbar-panel button:not(.tool-btn):hover {
    background-color: var(--hover-bg);
    color: var(--fg-primary);
}
.toolbar-panel togglebutton:not(.tool-btn):hover image {
    color: var(--fg-primary);
}
.toolbar-panel togglebutton:not(.tool-btn):disabled,
.toolbar-panel button:not(.tool-btn):disabled {
    background-color: transparent;
    background-image: none;
    opacity: 0.4;
}
/* Legacy .tool-active class — kept for any code path that still
   adds it via add_css_class("tool-active"). Now a no-op visual
   (the :checked rule handles active state); preserved so a stray
   add/remove doesn't cause a crash or warning. The old left-
   accent bar is gone — full background fill is clearer and more
   consistent with industry conventions. */
togglebutton.tool-active { /* no-op, see .tool-btn:checked above */ }

.tb-switch {
    border: none;
    padding: 0px;
    margin: 0px;
    min-width: 0px;
    min-height: 0px;
    font-size: 0px;
}

/* ── Toolbar density: Standard (curvz default) ──────────── */
.toolbar-panel.standard {
    min-width: 40px;
    padding: 3px 2px;
}
.toolbar-panel.standard > separator {
    margin: 3px 0;
}
.toolbar-panel.standard .tool-btn {
    min-width: 32px;
    min-height: 32px;
    border-radius: 3px;
}

/* ── Toolbar density: Compact ───────────────────────────── */
.toolbar-panel.compact {
    min-width: 32px;
    padding: 2px 1px;
}
.toolbar-panel.compact > separator {
    margin: 2px 0;
}
.toolbar-panel.compact .tool-btn {
    min-width: 28px;
    min-height: 28px;
    border-radius: 3px;
}

/* ── Toolbar density: Tight (minimum-viable for symbolic icons) */
.toolbar-panel.tight {
    min-width: 28px;
    padding: 1px;
}
.toolbar-panel.tight > separator {
    margin: 1px 0;
}
.toolbar-panel.tight .tool-btn {
    min-width: 24px;
    min-height: 24px;
    border-radius: 2px;
}

/* ── Toolbar defaults well ───────────────────────────────── */
.tb-well {
    border-radius: 2px;
    margin-top: 2px;
    margin-bottom: 2px;
}
.tb-well-spin {
    min-width: 56px;
    min-height: 24px;
    font-size: 11px;
    padding: 0 2px;
    background-color: var(--bg-raised);
    border: 1px solid var(--bg-edge);
    border-radius: 4px;
    color: var(--fg-secondary);
}
.tb-swatch {
    border-radius: 3px;
}
.tb-hex-entry {
    min-width: 80px;
    min-height: 24px;
    font-size: 11px;
    background-color: var(--bg-raised);
    border: 1px solid var(--bg-edge);
    border-radius: 4px;
    color: var(--fg-secondary);
    padding: 0 4px;
}
togglebutton.tb-type-btn,
button.tb-type-btn {
    font-size: 11px;
    padding: 4px 10px;
    min-height: 26px;
    background-color: var(--bg-raised);
    border: 1px solid var(--hover-bg);
    border-radius: 4px;
    color: var(--fg-muted);
    box-shadow: none;
}
togglebutton.tb-type-btn:hover,
button.tb-type-btn:hover { background-color: var(--bg-edge); color: var(--fg-secondary); }
togglebutton.tb-type-btn:checked,
button.tb-type-btn:checked {
    background-color: var(--accent-dim);
    border-color: var(--accent);
    color: var(--white-pure);
    box-shadow: none;
}
/* S87 m1 fix4: active-state rule for the manually-managed CSS class.
   The toolbar's fill/stroke type-row buttons are plain Gtk::Button
   (not ToggleButton — historical reason: they're a radio-like row
   handled in code), so :checked never matches. The refresh handlers
   add `tb-type-btn-active` on the active button and remove it from
   the others. Without this rule, the class was a no-op visually —
   long-standing bug noted in S87 backlog.
   Mirrors the :checked styling so a future migration to ToggleButton
   needs no CSS change. */
togglebutton.tb-type-btn.tb-type-btn-active,
button.tb-type-btn.tb-type-btn-active {
    background-color: var(--accent-dim);
    border-color: var(--accent);
    color: var(--white-pure);
    box-shadow: none;
}
.tb-pop-title {
    font-size: 11px;
    font-weight: bold;
    color: var(--fg-secondary);
}
.tb-pop-label {
    font-size: 10px;
    color: var(--fg-muted);
}
.tb-icon-btn {
    padding: 1px;
    border-radius: 4px;
    background-color: transparent;
    min-width: 0;
    min-height: 0;
}
.tb-icon-btn:hover { background-color: var(--hover-bg); }
.tb-icon-btn-active {
    background-color: var(--accent-dim);
    border-radius: 4px;
}
/* ── Toolbar zoom controls ───────────────────────────────── */
.tb-zoom-btn {
    min-width: 0;
    min-height: 20px;
    font-size: 13px;
    padding: 0 2px;
    color: var(--fg-muted);
    border-radius: 4px;
    border: none;
}
.tb-zoom-btn:hover { background-color: var(--bg-edge); color: var(--fg-secondary); }
.tb-zoom-entry {
    min-width: 0;
    min-height: 20px;
    font-size: 10px;
    padding: 0 3px;
    background-color: var(--bg-raised);
    border: 1px solid var(--bg-edge);
    border-radius: 4px;
    color: var(--fg-secondary);
}
.tb-fit-btn {
    min-width: 0;
    min-height: 20px;
    font-size: 10px;
    padding: 0 2px;
    color: var(--fg-muted);
    border-radius: 4px;
    border: none;
}
.tb-fit-btn:hover { background-color: var(--bg-edge); color: var(--fg-secondary); }

/* ── Doc tab bar (in headerbar) ──────────────────────────── */
.doc-tab-bar {
    background-color: transparent;
    min-height: 36px;
}
.doc-tab-bar-label {
    font-size: 11px;
    font-weight: bold;
    letter-spacing: 0.6px;
    color: var(--fg-muted);
    text-transform: uppercase;
}
.doc-tab-scroll {
    background-color: transparent;
}
.doc-tab-row {
    background-color: transparent;
    padding: 0 2px;
}
/* Individual tab */
.doc-tab {
    background-color: transparent;
    border-radius: 6px 6px 0 0;
    border: 1px solid transparent;
    border-bottom: none;
    padding: 0;
    min-height: 30px;
    margin-top: 4px;
}
.doc-tab:hover {
    background-color: var(--bg-raised);
}
.doc-tab-active {
    background-color: var(--bg-base);
    border-color: var(--bg-raised);
    border-bottom: 1px solid var(--bg-base);
}
.doc-tab-label {
    font-size: 12px;
    color: var(--fg-dim);
    margin-right: 2px;
}
.doc-tab-active .doc-tab-label {
    color: var(--fg-secondary);
}
/* Unsaved indicator dot */
.doc-tab-dot {
    font-size: 7px;
    color: var(--accent);
    margin-right: 2px;
}
/* Close button inside tab */
.doc-tab-close {
    background-color: transparent;
    border: none;
    border-radius: 4px;
    color: var(--hover-bg);
    min-width: 18px;
    min-height: 18px;
    padding: 1px;
    margin-right: 4px;
    -gtk-icon-size: 10px;
}
.doc-tab-close:hover {
    background-color: var(--bg-edge);
    color: var(--fg-secondary);
}
/* + button */
.doc-tab-add-btn {
    background-color: transparent;
    border: none;
    border-radius: 5px;
    color: var(--hover-bg);
    min-width: 28px;
    min-height: 28px;
    margin: 4px 4px 0;
    padding: 2px;
}
.doc-tab-add-btn:hover {
    background-color: var(--bg-raised);
    color: var(--fg-secondary);
}

/* S60 M3: scroll chevrons — shown when the tab row overflows its
   viewport. Styled to match the flat, frameless tab-bar aesthetic.
   Use set_sensitive (not set_visible) to toggle; keeping the widget
   visible but insensitive avoids visibility-driven layout churn
   that triggers negative-width allocations on sibling widgets. */
.doc-tab-chevron {
    background-color: transparent;
    border: none;
    border-radius: 4px;
    color: var(--fg-dim);
    min-width: 22px;
    min-height: 22px;
    margin: 6px 2px 0;
    padding: 2px;
    -gtk-icon-size: 12px;
}
.doc-tab-chevron:hover {
    background-color: var(--bg-raised);
    color: var(--fg-secondary);
}
.doc-tab-chevron:disabled {
    color: var(--bg-edge);
    background-color: transparent;
}

/* ── Document gallery (kept for thumbnail rendering, hidden) ── */
.gallery-panel {
    background-color: var(--bg-base);
    border-bottom: 1px solid var(--bg-deepest);
}
.gallery-thumb {
    background-color: var(--bg-raised);
    border: 1px solid var(--bg-edge);
    border-radius: 5px;
    min-width: 76px;
    min-height: 76px;
}
.gallery-thumb-active {
    border: 2px solid var(--fg-muted);
}

/* ── Panel expanders (legacy — unused) ──────────────────── */

/* ── Inspector sections ─────────────────────────────────── */
.inspector-section {
    background-color: var(--bg-base);
}
/* Slim header row — Box + GestureClick, not a Button.
   No full-width pressable banner. Quiet category label only. */
.inspector-section-header {
    background-color: var(--bg-base);
    border-bottom: 1px solid var(--bg-base);
    padding: 3px 8px;
    min-height: 24px;
}
.inspector-section-header:hover {
    background-color: var(--bg-base);
}
.inspector-arrow {
    /* S117 m1: was var(--hover-bg) — bg token used as fg colour.
       Same fix as .prop-section-lbl: arrows are muted glyphs, so
       fg-muted is the correct semantic role for both motifs. */
    color: var(--fg-muted);
    font-size: 9px;
    min-width: 10px;
}
.inspector-section-title {
    font-size: 10px;
    font-weight: bold;
    letter-spacing: 0.5px;
    color: var(--fg-dim);
    text-transform: uppercase;
}

/* s147 m3: ThemesPanel library row. The themes-row class gives a small
   bit of layout breathing room around each row. The selected state
   tints the row with the accent colour — ToggleButton's built-in
   active visual is too subtle in this theme to read as "this is the
   apply source," so we add an explicit background tint via this
   class which the panel toggles on/off based on the selected id. */
.themes-row {
    border-radius: 3px;
    padding: 1px;
}
.themes-row-selected {
    background-color: var(--accent-bg);
}

/* Group-level titles (Document / Object / Content) — bolder and brighter
   than inner section titles so top-level hierarchy reads at a glance. */
.inspector-group-title {
    font-size: 11px;
    font-weight: bold;
    letter-spacing: 0.6px;
    color: var(--fg-muted);
    text-transform: uppercase;
}

/* Indent inner sections inside Document/Object/Content group containers
   so the collapsible hierarchy reads visually. */
.inspector-group-container {
    padding-left: 15px;
}

/* s148 m2 fix3: indent the body of a nested disclosure (e.g. the
   "Motif" disclosure in the Document group, which contains Canvas /
   Margins / Grid / Guides as inner sections). 12px reads as a
   distinct indent level beyond the group container's 15px without
   stacking too deeply — the inner sections clearly belong to the
   disclosure but don't run off the right edge. Apply via
   add_css_class("inspector-disclosure-body") on the body returned
   by add_collapsible() at the disclosure-builder call site. */
.inspector-disclosure-body {
    padding-left: 12px;
}

/* ── Right panels (Properties, Preview, Layers, Library) ─── */
.properties-panel,
.preview-panel,
.layers-panel,
.library-panel {
    background-color: var(--bg-base);
    border-left: 1px solid var(--bg-surface);
}
.preview-panel  { border-top: 1px solid var(--bg-surface); }
.layers-panel   { border-top: 1px solid var(--bg-surface); }
.library-panel  { border-top: 1px solid var(--bg-surface); }

/* Layer drag-and-drop */
.layer-dragging    { opacity: 0.4; }
.layer-drop-top    { border-top:    2px solid var(--accent); }
.layer-drop-bottom { border-bottom: 2px solid var(--accent); }
.layer-drop-into   { background-color: var(--accent-bg); border: 1px solid var(--accent); }

/* Template manager drag-and-drop */
.template-dragging        { opacity: 0.4; }
.template-category-drop   { background-color: var(--accent-bg);
                            border-radius: 4px; }

/* Template manager — default-template indicator.
   Exactly one template can be THE default at a time (stored in
   defaults.json). The active dot renders in the Curvz accent blue that's
   already used for drop highlights and selection; inactive dots are dim.
   Selectors bind to `button.default-dot-*` with `label` child so they
   beat the `button.flat { color: var(--fg-secondary); }` rule in specificity and
   actually reach the glyph text node. */
button.default-dot-active,
button.default-dot-active label {
    color: var(--accent);
}
button.default-dot-active:hover,
button.default-dot-active:hover label {
    color: var(--accent-hover);
}
button.default-dot-inactive,
button.default-dot-inactive label {
    color: var(--fg-secondary);
    opacity: 0.35;
}

/* New Document picker — tile selection outline.
   Applied by refresh_tile_selection() on exactly one tile at a time. Uses
   `.tile-selected` (not `.selected`) to avoid collision with Gtk's own
   internal `.selected` state class which various themes target. Outline
   is rendered via box-shadow so it doesn't fight with `button.flat`'s
   transparent border rule — box-shadow sits on a different CSS axis and
   wins unconditionally.

   Selector deliberately un-qualified (no `button` prefix): tiles are
   Gtk::Frame so a single GestureClick can detect double-click, and the
   same selection styling applies regardless of the underlying widget. */
.template-tile.tile-selected,
.template-tile.tile-selected:hover,
.template-tile.tile-selected:active {
    box-shadow: inset 0 0 0 2px var(--accent);
    background-color: var(--accent-bg);
}

/* Layer tree rows */
.layer-header { border-radius: 4px; }
.layer-header:hover { background-color: var(--bg-raised); }
.layer-active { background-color: var(--accent-bg); }
.path-row { border-radius: 3px; }
.path-row:hover { background-color: var(--bg-raised); }
.layer-hidden-btn { opacity: 0.55; }
.layer-hidden-text { opacity: 0.55; font-style: italic; }
.layer-locked-btn { color: var(--warn-fg); }
.layer-locked-text { opacity: 0.4; }

/* Macro Manager rows — selected row needs to read clearly in dark mode.
   Lighter than .layer-active because the macro picker has no surrounding
   row background to contrast against; the highlight has to do the work
   on its own. Hover stays subtle so it doesn't compete with selection. */
.macro-row-selected {
    background-color: var(--accent-deep);
    border-radius: 3px;
}
.macro-row-selected:hover {
    background-color: var(--accent-deep);
}

/* Active macro label — the one that will run on Ctrl+M / be recorded
   into. Distinct from .macro-row-selected (which marks the *clicked*
   row, target of toolbar buttons). Bold + Curvz accent blue reads
   through both default and selected row backgrounds, so the user can
   tell at a glance which row is "active" even when a different row
   is currently selected. */
label.accent {
    font-weight: 700;
    color: var(--accent-hover);
}

/* ── Status bar ──────────────────────────────────────────── */
.statusbar {
    background-color: var(--bg-deepest);
    border-top: 1px solid var(--bg-surface);
    min-height: 22px;
}
.statusbar-label {
    font-size: 11px;
    color: var(--fg-muted);
    font-family: monospace;
}
.statusbar-doc-label {
    
}

/* s150 — active display unit lives next to zoom in the status bar.
   Bold + slightly more saturated colour so it reads at a glance,
   peripheral-vision feedback for "what unit are my numbers in." */
.statusbar-units-label {
    font-size: 11px;
    color: var(--fg-muted);
    font-family: monospace;
}

/* ── Typography classes ──────────────────────────────────── */
.panel-title {
    font-size: 12px;
    font-weight: bold;
    color: var(--fg-secondary);
    margin-bottom: 4px;
}
.section-label {
    font-size: 11px;
    font-weight: bold;
    color: var(--fg-secondary);
}
.caption-label {
    font-size: 10px;
    color: var(--fg-muted);
}
.dim-label {
    font-size: 11px;
    color: var(--fg-muted);
}
/* S117 m11: GTK / libadwaita ships a stock `.dim-label` class that
   the system theme styles with a dimming alpha layer, winning the
   cascade against our bare `.dim-label` rule above. Anchoring through
   the element name (`label.dim-label`) bumps specificity to (0,1,1)
   so we win, and pulling the colour up to fg-secondary makes the
   layer-panel object counts read clearly on the light panel bg. */
label.dim-label {
    color: var(--fg-secondary);
}
.pop-unit-label {
    font-size: 11px;
    color: var(--fg-secondary);
    font-style: italic;
}
.prop-type-btn {
    font-size: 10px;
    padding: 1px 5px;
    min-height: 20px;
}
/* S117 m1: explicit non-checked styling for .prop-type-btn.
   Previously this class had only :checked + .prop-type-btn-active
   styling; the unchecked state fell through to GTK's system theme,
   which tracks the OS dark/light preference rather than our motif
   class. Result: in Curvz Light on a system-Dark host, the toggles
   in PaintEditor (Solid/None/currentColor/Swatch/Gradient) and any
   other prop-type-btn group rendered dark-on-dark.
   Same shape as the suggested-action fix from m7 — explicit base
   state in tokens, so both motifs resolve correctly. Cap/Join
   buttons in PropertiesPanel.cpp also adopt this class in s117 m1
   so they pick up these rules automatically. */
togglebutton.prop-type-btn,
button.prop-type-btn {
    background-color: var(--bg-raised);
    color: var(--fg-secondary);
    border: 1px solid var(--bg-edge);
}
togglebutton.prop-type-btn:hover,
button.prop-type-btn:hover {
    background-color: var(--bg-edge);
    color: var(--fg-primary);
}
togglebutton.prop-type-btn:disabled,
button.prop-type-btn:disabled {
    opacity: 0.4;
}
/* S87 m1 fix4: active-state for PaintEditor's type-row toggles
   (Solid / None / currentColor / Swatch). PaintEditor uses real
   Gtk::ToggleButton with set_group, so :checked drives this; the
   .prop-type-btn-active class variant is also matched in case any
   future host manages it manually like the toolbar does. Same blue
   palette as tb-type-btn for visual consistency between the toolbar
   popovers and the inspector / style-editor PaintEditor surfaces.
   S117 m9: + background-image: none so system theme stops painting
   a grey image atop our colour (the recurring shape we kept hitting
   in m5/m6/m7/m8). */
togglebutton.prop-type-btn:checked,
button.prop-type-btn:checked,
togglebutton.prop-type-btn.prop-type-btn-active,
button.prop-type-btn.prop-type-btn-active {
    background-color: var(--accent-dim);
    background-image: none;
    border-color: var(--accent);
    color: var(--white-pure);
    box-shadow: none;
}
/* S117 m1: light-motif override. Dark mode keeps the blue accent
   (rule above), but light mode wants the toggle grammar to be
   "light bg / dark icon → softer-grey bg / dark icon when active",
   not blue. Selector specificity wins over the unprefixed rule
   above because window.curvz-light adds a class to the ancestor
   chain.
   S117 m9: bg shifted from --bg-edge (#b8b8b8) to --hover-bg
   (#e0e0e0) — one step lighter for a softer "selected" feel.
   Border kept at --bg-edge so there's still a definite outline
   separating checked from unchecked siblings. */
window.curvz-light togglebutton.prop-type-btn:checked,
window.curvz-light button.prop-type-btn:checked,
window.curvz-light togglebutton.prop-type-btn.prop-type-btn-active,
window.curvz-light button.prop-type-btn.prop-type-btn-active {
    background-color: var(--hover-bg);
    background-image: none;
    border-color: var(--bg-edge);
    color: var(--fg-primary);
    box-shadow: none;
}

/* ── New document dialog ─────────────────────────────────── */
.dialog-content-area,
.new-doc-dialog {
    background-color: var(--bg-base);
    color: var(--fg-primary);
}


/* ── ListBox (layers panel, popovers) ────────────────────── */
listbox {
    background-color: var(--bg-base);
    color: var(--fg-primary);
    border: none;
}
listbox row {
    background-color: transparent;
    color: var(--fg-primary);
    padding: 2px 0;
    border-radius: 0;
    border: none;
}
listbox row:hover { background-color: var(--bg-raised); }
listbox row:selected {
    background-color: var(--bg-edge);
    color: var(--white-pure);
}
listbox row:selected:hover { background-color: var(--hover-bg); }

/* S124 m5: libadwaita's `.navigation-sidebar` class (used on the help
   window's topic sidebar) carries a stock background that GTK paints
   #fcfcfc in light mode — much brighter than the panel surface. The
   listbox rule above doesn't beat it because `.navigation-sidebar`
   is a more-specific class selector. Match its specificity here and
   route through the token system.

   S124 m6: sidebar/content tone hierarchy inverted. Earlier draft put
   sidebar on --bg-base; the result read as sidebar lighter than
   content area, which is backwards from convention (the navigation
   list should sit recessed, the content should read as the lifted
   "page" you're reading). Swapped: sidebar on --bg-surface (heavier,
   feels like a docked panel), content on --bg-raised (clearer, the
   foreground reading surface).

   S124 m7: m5/m6 selectors weren't reaching the widget at all (GTK
   Inspector showed background-color = "Default", not lost-cascade).
   Diagnosis: GTK4's internal CSS node name for the painting surface
   inside a Gtk::ListBox is `list`, NOT `listbox`. The `listbox`
   selector matches the outer container; the `list` selector matches
   the actual scrolling list element that paints the background.
   Switching to `list.navigation-sidebar` makes the rule match. */
list.navigation-sidebar {
    background-color: var(--bg-surface);
    color: var(--fg-primary);
}
list.navigation-sidebar > row {
    background-color: transparent;
    color: var(--fg-primary);
}
list.navigation-sidebar > row:hover { background-color: var(--bg-raised); }
list.navigation-sidebar > row:selected {
    background-color: var(--bg-edge);
    color: var(--white-pure);
}
list.navigation-sidebar > row:selected:hover { background-color: var(--hover-bg); }

/* S124 m6: help window content area — the right pane where the
   rendered manual page sits.

   m6 v1: painted .help-content at --bg-raised. Worked, but created
   a visible seam between the content box and the surrounding
   `box.vertical` parent (which paints --bg-base via the s117 m2
   `window.csd > box.vertical` rule). The seam read as a stripe of
   medium-grey wherever content padding/margin didn't cover.

   m8: make .help-content transparent so the parent surface shows
   through. The whole right side of the paned now reads as one
   continuous --bg-base reading panel; only the sidebar (recessed
   on --bg-surface, set by m7's `list.navigation-sidebar` rule)
   stands out as a distinct surface. Cleaner visual story —
   sidebar = recessed nav, everything else = the page. */
.help-content,
.help-content scrolledwindow,
.help-content scrolledwindow > viewport,
scrolledwindow > viewport > .help-content {
    background-color: transparent;
    color: var(--fg-primary);
}

/* s150: symbolic tool icons in help-page headings (e.g. the Pen / Measure
   / Selection icons next to chapter titles). Rendered via icon-theme
   set_from_icon_name so GTK applies symbolic recolouring; the
   `color: ...` here is what GTK substitutes for the SVG's currentColor.
   Without it, dark mode shows black-on-black because the raster path
   would have baked in the SVG's native fill. */
.help-symbolic-icon {
    color: var(--fg-primary);
}

/* s131: section header rows in the help sidebar (e.g. "4.2 Selection &
   Node", "4.4 Shape tools"). Pre-s131 these were rendered with italic
   markup + .dim-label, and ended up at GTK's default Label font size
   (~11px) which read as "less important" alongside the .help-leaf
   button rows whose default Button styling is closer to 14pt.

   Both treatments said the wrong thing — section rows are fully
   descriptive labels for the chapter structure, not throwaway
   secondary text. The C++ side now drops the italic + .dim-label;
   this rule pegs the font-size to match the leaf rows so they look
   like sibling rows of equal status, not headings of subordinate
   children.

   Scope: only .help-group-title is touched. Chapter rows
   (.help-chapter-title) already render at the right size via their
   <b> markup + GTK Label defaults — leaving them alone avoids any
   risk of regressing the existing chapter look. */
.help-group-title {
    font-size: 14.666px;
}

/* s131 m18: callout boxes for "Note:" / "Appears when:" advisory
   blockquotes inside help leaves. Pre-m18 the renderer dropped
   markdown blockquotes silently (see HelpWindow.cpp comment), which
   meant every callout in the manual was invisible. m18 wires
   CMARK_NODE_BLOCK_QUOTE rendering and tags the wrapper with
   .help-callout so the styling lives here.

   Visual treatment: a slightly raised tint plus a thicker accent
   border on the left edge. Reads as "this is set apart from the
   surrounding prose" without screaming. The accent bar uses the
   same surface ramp the rest of the help window uses, so it sits
   coherently against .help-content's transparent surface in both
   light and dark motif. */
.help-callout {
    background-color: var(--bg-raised);
    border-left: 4px solid var(--accent);
    border-radius: 4px;
    padding: 8px 12px;
    margin-top: 8px;
    margin-bottom: 8px;
}

/* ── ScrolledWindow ──────────────────────────────────────── */
scrolledwindow,
scrolledwindow undershoot,
scrolledwindow overshoot {
    background-color: transparent;
}

/* ── Frame / viewport (catches stray white boxes) ───────── */
frame,
frame > border { border: none; }
viewport { background-color: transparent; }

/* ── Flat preset buttons in NewDocumentDialog ────────────── */
.new-doc-dialog button.flat,
.new-doc-dialog button.flat:not(.suggested-action) {
    background-color: var(--bg-raised);
    color: var(--fg-primary);
    border: 1px solid var(--bg-edge);
    border-radius: 5px;
    padding: 3px 8px;
    min-width: 32px;
}
.new-doc-dialog button.flat:hover { background-color: var(--bg-edge); }
.new-doc-dialog button.flat:active { background-color: var(--hover-bg); }

/* Cancel button — explicit neutral, never white */
.new-doc-dialog button:not(.suggested-action):not(.flat) {
    background-color: var(--bg-raised);
    color: var(--fg-primary);
    border: 1px solid var(--fg-dim);
}
.new-doc-dialog button:not(.suggested-action):not(.flat):hover {
    background-color: var(--bg-edge);
}

/* ── Curvz alert / confirm / form dialog (s125 m2a) ──────────────────────
 *
 * Built by curvz::utils::show_alert / show_confirm / show_form. Replaces
 * Gtk::AlertDialog throughout the app. Inherits the motif (lightmode /
 * darkmode) from its transient parent via the curvz-light cascade — no
 * separate selector needed, the tokens just resolve correctly.
 *
 * Structure (matches the build_dialog comment in curvz_utils.cpp):
 *   .curvz-alert            window
 *     .curvz-alert-title    headline label
 *     .curvz-alert-detail   detail entry (read-only, selectable)
 *     .curvz-alert-fields   field grid (show_form only)
 *     .curvz-alert-buttons  button row
 *       .curvz-alert-default  suggested-action button
 *       .curvz-alert-cancel   destructive button
 */
.curvz-alert {
    background-color: var(--bg-base);
    color: var(--fg-primary);
}
.curvz-alert-title {
    font-weight: bold;
    font-size: 1.05em;
    color: var(--fg-primary);
    margin-bottom: 8px;
}
.curvz-alert-detail {
    background: transparent;
    border: none;
    box-shadow: none;
    padding: 0;
    color: var(--fg-secondary);
    margin-bottom: 4px;
}
.curvz-alert-detail:focus {
    outline: none;
    box-shadow: none;
}
.curvz-alert-fields {
    margin-top: 4px;
    margin-bottom: 4px;
}
.curvz-alert-buttons {
    margin-top: 8px;
}
.curvz-alert-buttons button {
    background-color: var(--bg-raised);
    color: var(--fg-primary);
    border: 1px solid var(--fg-dim);
    padding: 6px 16px;
}
.curvz-alert-buttons button:hover {
    background-color: var(--bg-edge);
}
.curvz-alert-buttons button.curvz-alert-default {
    background-color: var(--accent);
    color: var(--accent-fg);
    border: 1px solid var(--accent);
}
.curvz-alert-buttons button.curvz-alert-default:hover {
    background-color: var(--accent-hover);
}
.curvz-alert-buttons button.curvz-alert-cancel {
    /* Plain neutral by default — destructive-action red would be too
     * loud for "Cancel". Callers wanting a real destructive button
     * (Delete, Discard) can override with a stronger selector if needed.
     */
    background-color: var(--bg-raised);
}

/* s202 m6_v2 — Quick-jump float rows. Mini-density buttons in a
   chromeless transient window: no frame on the button itself, tight
   vertical padding, smaller font than default. The float is a system-
   menu-shape affordance, not a dialog, so density matches a menu. */
button.qj-row {
    padding: 1px 6px;
    min-height: 18px;
    font-size: 12px;
    border-radius: 3px;
}
button.qj-row:hover {
    background-color: var(--accent-bg);
}

/* s203 m1 — View Clipboard mini float. Same mini-density tone as the
   quick-jump: chromeless transient, small font, tight padding. The
   header strip is dim-label small for "this is metadata"; the body is
   monospace so multi-line structured blocks (Measure-tool output:
   x₁ = ..., y₁ = ..., distance = ...) line up neatly for scanning. */
.clip-view-win {
    /* Subtle outline so the chromeless window is visible against the
       desktop wallpaper. Matches the quick-jump's implicit GTK frame. */
}
.clip-view-header {
    font-size: 11px;
}
textview.clip-view-body, textview.clip-view-body text {
    font-size: 12px;
    background-color: var(--bg-raised);
}
button.clip-view-btn {
    padding: 2px 8px;
    min-height: 20px;
    font-size: 12px;
}

)css";