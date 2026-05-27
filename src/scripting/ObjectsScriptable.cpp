// scripting/ObjectsScriptable.cpp ────────────────────────────────────────────
//
// s230 m1 — implementation of the sixth row-bound model Scriptable.
// See ObjectsScriptable.hpp for the verb/query surface, lifetime
// notes, the scope choice (real scene contents only — option (ii)
// from the s230 handoff's design fork), and the "READ-SIDE only"
// scope of m1.
//
// s231 m2 — adds collection-level structural verbs `new <type>` and
// `delete <iid>` so the smoke can mint and tear down its own test
// objects. The element queries from m1 now get exercised via a full
// proxy round-trip on iids the smoke itself created.
//
// s232 m3 — wires five element mutating verbs onto `ObjectProxy::invoke`,
// which was a no-op stub through m1+m2. Each verb (toggle_visible,
// set_visible, toggle_locked, set_locked, rename) does the direct
// mutation on the SceneNode field then pushes an EditObjectFieldCommand
// (a new command class added in m3, direct analog of
// EditLayerFieldCommand minus the Color field). Mirrors
// LayerProxy::invoke's shape exactly — see LayersScriptable.cpp for
// the parallel implementation. Helpers `push_bool_field` /
// `push_string_field` on the proxy match the layer-side helpers
// of the same names; `arg_as_bool` is added to the anon namespace.
//
// s234 m4 — wires three element structural verbs onto `ObjectProxy::invoke`:
// `move "<direction>"`, `reparent "<new_parent_iid>"`, and `duplicate`.
// Plus a new element query `child_index` on the proxy. Direct mutation
// happens in the verb body BEFORE the command push, matching the m2 /
// m3 shape and LayerProxy's convention.
//
// Command rides:
//   - `move` rides MoveNodeIndexCommand (NEW in m4, single-target
//     iid-keyed intra-parent reorder). Verification (first task of
//     the milestone — same precedent as m3's first task) confirmed
//     ZOrderCommand's apply_order keys on SVG `id`, which is empty
//     for script-minted objects, so script-driven multi-sibling
//     reorder would silently drop entries on undo/redo. New command
//     uses internal_id-keyed lookup at execute/undo time. Same
//     "ship a parallel command class" precedent m3 set with
//     EditObjectFieldCommand.
//   - `reparent` rides MoveObjectToLayerCommand unmodified. Its src/dst
//     resolution via find_by_iid is container-agnostic — it walks the
//     entire project tree, not just doc->layers, so Group / Compound /
//     ClipGroup containers work as destinations the same as Layers.
//     Only the description string ("Move to layer") is layer-flavored;
//     a cosmetic concern that stays preserved this session.
//   - `duplicate` rides DuplicateCommand. Script-side prep clones the
//     subtree then MINTS A FRESH internal_id on every node in the
//     cloned subtree (canvas's freshen_ids helper only touches SVG id
//     and name, not internal_id — that's a latent canvas-side issue;
//     out of scope for m4). Without the fresh internal_id mint, the
//     duplicate and original would collide in the iid index and
//     `set <var> to result` would resolve to whichever node tree-walk
//     visits last. Returns the new iid as ScriptValue::text so scripts
//     can bind it and immediately address the duplicate.
//
// New query `child_index` on the proxy: the smoke needs an observable
// for `move` round-trips (the move verb's success leaves no visible
// trace through name / visible / locked / type / parent_iid /
// child_count — those are all unchanged by reorder). `child_index`
// closes the gap; returns the node's slot position in parent->children,
// or -1 if the node lives in a non-children slot.
//
// s235 m1 — adds a NotifyDocChanged callback (`std::function<void()>`)
// threaded through the ObjectsScriptable ctor and into ObjectProxy via
// `proxy_for`. Every successful mutating verb (collection `new` /
// `delete`; element-level toggle_visible / set_visible / toggle_locked
// / set_locked / rename / move / reparent / duplicate) calls the
// callback at the END of its success path, AFTER the command push.
// MainWindow supplies a lambda that runs `m_canvas.queue_draw();
// m_canvas.notify_document_changed();` — the canonical two-call
// shape for external mutators (s227 m1's themes_refresh_cascade is
// the in-tree precedent). No-op paths (args.empty(), value
// unchanged, unknown direction, refused refusal-class) do NOT
// invoke the callback — there's no canvas state to refresh in
// those cases. The callback may be null, in which case every verb
// silently skips the refresh step (graceful degradation, same shape
// the history pointer uses). Closes the s234-end-of-session gap
// where the smoke ran clean but the canvas kept rendering its
// previous frame.
//
// s236 m1 — OPENS the canvas-observable scripts arc. Adds the
// first geometry-bearing verb (`set_path_data "<svg_d_string>"`)
// to ObjectProxy::invoke, plus two new proxy queries (`node_count`
// and `path_data`). Rides EXISTING EditPathCommand unmodified —
// first-task verification confirmed its CurvzProject* + obj_iid +
// before/after PathData shape (added in s167 m1) is the exact fit
// set_path_data needs. The "ship a parallel command rather than
// retrofit" precedent m3 and m4 set doesn't apply here; m1 reuses
// EditPathCommand directly. New `curvz::utils::svg_d_to_path_data`
// and `path_data_to_svg_d` pumps in curvz_utils — the emitter is
// a full lift from SvgWriter's two duplicate inline emitters; the
// parser side bridges to SvgParser's parse_path_d via a non-static
// wrapper (full parser lift deferred to a future SvgParser sweep
// milestone — the body is 260 lines plus arc_to_bezier, intertwined
// with parse_path_d_multi, not surgical-sized).
//
// The verb does:
//   1. Resolve node via the proxy's iid (returns null if iid no
//      longer addresses a scene-object — same shape as every other
//      proxy verb).
//   2. Refuse if `node->path == nullptr` (non-Path types: Group,
//      Compound, Image, Ref, etc. don't carry a PathData slot).
//      Silent no-op, no command pushed, no callback fired.
//   3. Refuse if args.empty(). Empty-string arg IS allowed and
//      resolves to an empty PathData — useful clearing primitive.
//   4. Capture `before = *node->path`; parse arg via
//      svg_d_to_path_data; capture `after = parsed`.
//   5. Direct mutation: `*node->path = after` (mirrors how every
//      existing EditPathCommand call site applies before pushing).
//   6. Push EditPathCommand(proj, m_iid, before, after, "Set path
//      data (script)"). Skipped if history is null (graceful
//      degrade, same shape every other proxy verb uses).
//   7. notify_doc_changed() — the s235 m1 cascade.
//
// The iid index is NOT invalidated by this verb — path-data changes
// don't reshape the iid space (no internal_id mints, no tree-
// structure changes). EditPathCommand::execute does its own
// invalidate-before-resolve defensively (s168 m6 pattern), which
// covers redo/undo cycles.
//
// Two new proxy queries:
//   - `node_count` (int) — size of node->path->nodes. 0 when
//     node->path is null (non-Path types) or empty. The observable
//     for set_path_data — deterministic, no Cairo, pure model side.
//   - `path_data` (text) — re-emitted SVG-d string via
//     path_data_to_svg_d. Empty string for null/empty paths. NOT
//     byte-identical to the most-recent set_path_data input (fmt2
//     normalisation, segment-class reclassification); the read
//     surface for capture/inspection rather than literal-RHS
//     round-trip assertion.
//
// s237 m1 — RETROFIT set_path_data and path_data to speak user-space.
//
// The s236 m1 visual channel surfaced an architectural call: the
// scripter is a user surface — an extension of the app *to* the
// user, not a window into the engine *for* the user. Scott saw
// triangles at the doc-space origin (top-left of the page, Y-down)
// instead of the user-space origin (bottom-left of the page, Y-up)
// where the user expects geometry typed in script to land.
//
// First task (verify before assume): looked for the existing
// screen → doc helper in Canvas. Found something better — the pump
// already exists. `CoordSpace` in include/CoordSpace.hpp encodes
// exactly the Y-flip the script seam needs: `to_doc_y(user_y)` and
// `to_user_y(doc_y)`, taking canvas_height as the per-doc constant.
// CoordSpace.hpp explicitly documents itself as "the ONLY place in
// the codebase where the Y-flip is performed" — the rule the
// retrofit must respect.
//
// The retrofit lifts a PathData-level walker into curvz_utils:
// `path_data_user_to_doc(pd, canvas_h)` and `path_data_doc_to_user
// (pd, canvas_h)`. Each walks pd.nodes and routes every (y, cy1,
// cy2) through a CoordSpace — the canonical seam stays the only
// place (canvas_height - y) appears. X is a pass-through.
//
// Wiring change at the script-side seam:
//
//   set_path_data: ... parse via svg_d_to_path_data → look up the
//     owning doc via curvz::utils::find_doc_by_iid → flip
//     `after` from user-space to doc-space via path_data_user_to_doc
//     → direct mutation + EditPathCommand push (now carrying the
//     doc-space PathData, same shape the canvas tools have always
//     produced).
//
//   path_data query: ... copy the stored (doc-space) PathData →
//     look up the owning doc → flip the copy via path_data_doc_to_user
//     → emit the user-space d-string via path_data_to_svg_d. The
//     stored PathData stays untouched (it remains canonical in
//     doc-space; the flip lives in a transient local).
//
// Third complementary verify-before-assume case in the row: s232 m3
// + s234 m4 found commands NEEDED new parallel classes, s236 m1
// found EditPathCommand FIT unmodified, s237 m1 found the pump
// ALREADY EXISTED (CoordSpace) and the retrofit lifts it into the
// script-side seam via a PathData walker. The discipline is the
// same in every direction: don't predict the shape, look at the
// candidate first.
//
// The EditPathCommand undo/redo behaviour is unchanged. Before and
// after PathData are captured in doc-space (post-flip on the verb
// side, pre-existing on the canvas-tool side). The history layer
// has no awareness of user-space; only the script-side seam does.
//
// Both the user-space pump and the find_doc_by_iid lookup can fail
// gracefully: if the doc lookup returns nullptr (defensive — should
// not happen since find_by_iid just resolved the node), the flip is
// skipped and the PathData lands in doc-space without Y-flip. That
// matches the s236 m1 "engine-space landing" — a regression, not a
// crash. Two-channel acceptance catches it (the visual would show
// inverted geometry).
//
// s238 m1 — APPEARANCE branch of the canvas-observable arc opens.
// Adds the first appearance-bearing verb (`set_fill "<color_token>"`)
// to ObjectProxy::invoke, plus a matching `fill` read query. Rides
// EXISTING EditAppearanceCommand unmodified — verify-before-assume's
// FOURTH "existing command fits" case (after s236 m1 reusing
// EditPathCommand and s237 m1 reusing CoordSpace; s232 m3 and s234
// m4 had to ship parallel classes). EditAppearanceCommand's capture
// is whole-FillStyle + whole-StrokeStyle plus swatch-id and bound-
// style snapshots; for a fill-only edit the verb writes only
// `fill_after`, leaving stroke_after / fill_swatch_id_after /
// stroke_swatch_id_after / bound_style_after equal to their `_before`
// snapshots so undo restores those four to themselves (no-op for
// stroke / swatches / binding).
//
// New `curvz::utils::fill_attr_to_fill_style` and
// `fill_style_to_fill_attr` pumps in curvz_utils — third
// infrastructure-pump candidate after find_by_iid (s167 m1) and the
// svg-d pair (s236 m1). Both legacy seams (SvgParser::parse_fill,
// SvgWriter::fill_attr) stay put as file-statics with their
// gradient-server branches intact; the script-side pumps cover the
// four context-free fill types (None, CurrentColor, Solid, with a
// graceful first-stop-hex degrade for gradients written from
// script). Future SvgParser sweep collapses to one canonical pair.
//
// Vocabulary on set_fill:
//   - "none"             → FillStyle::None
//   - "currentColor"     → FillStyle::CurrentColor
//   - "#RRGGBB"          → FillStyle::Solid
//   - "#RGB"             → FillStyle::Solid (short-form, expanded)
//   - "rgb(r,g,b)"       → FillStyle::Solid
//   - "rgba(r,g,b,a)"    → FillStyle::Solid
//   - named (19 colours) → FillStyle::Solid
//   - anything else      → FillStyle::CurrentColor (safe default)
//
// The verb does:
//   1. Resolve node via the proxy's iid.
//   2. Refuse if args.empty(). Unlike set_path_data, an empty-string
//      arg is ALSO refused — empty fill token has no defensible
//      vocabulary slot here (SVG's "fill omitted" maps to CurrentColor
//      from the renderer's side, but the user typing `set_fill ""`
//      reads as accidental and we silently no-op rather than convert
//      a typo into a mutation). Future calibration if the field
//      reports the convention as surprising.
//   3. No node-type refusal — appearance lives on every SceneNode
//      subtype (Path, Group, Compound, Image, Ref, Text, etc.).
//      Setting fill on a Group/Compound has a semantic effect via
//      style inheritance even though the Group itself doesn't draw
//      a filled region directly. Matches inspector and broadcast
//      behaviour.
//   4. Parse arg via fill_attr_to_fill_style → `fill_new`.
//   5. Capture full pre-edit appearance snapshot: fill_before,
//      stroke_before, fill_swatch_id_before, stroke_swatch_id_before,
//      bound_style_before. Same shape EditAppearanceCommand carries.
//   6. No-op (no command, no callback) when fill_new equals
//      fill_before — same value-unchanged early-return shape as
//      set_visible / set_locked / rename. Comparison uses the
//      FillStyle field-level == below (anon namespace helper).
//   7. Direct mutation: write fill_new onto node->fill. Also clear
//      fill_swatch_id and bound_style — direct-override semantics
//      that mirror style::mutate_appearance on the inspector side
//      (a direct fill set breaks the swatch binding and the style
//      binding; the s82 / s92 capture shape exists exactly to undo
//      this break correctly).
//   8. Push EditAppearanceCommand with the 14-arg constructor
//      (proj, m_iid, fill_before, stroke_before, fill_new,
//      stroke_before, fsib, ssib, "", ssib, bsb, "", desc) —
//      fill_after = fill_new, the four "stroke / swatches / binding"
//      after-values are: stroke unchanged, fill_swatch cleared,
//      stroke_swatch unchanged, bound_style cleared. Skipped if
//      history is null.
//   9. notify_doc_changed() — the s235 m1 cascade.
//
// The iid index is NOT invalidated by this verb — appearance changes
// don't reshape iid space. EditAppearanceCommand::execute does its
// own invalidate-before-resolve defensively (s168 m6 pattern), which
// covers redo/undo cycles.
//
// New proxy query:
//   - `fill` (text) — re-emitted fill attribute string via
//     fill_style_to_fill_attr. "none" / "currentColor" / "#RRGGBB"
//     forms. Byte-clean round-trip for the canonical input forms
//     (lowercase hex normalises via "%02x"; named colours
//     normalise to hex; rgb()/rgba() normalise to hex). The literal-
//     RHS assert in smoke 36 round-trips on the normalised forms.
//
// Canvas-observable: the wireframe rect from s237 m1 becomes a SOLID
// (or stroked, once set_stroke lands in m2/m3) shape that's visible
// in fill-shaded canvas modes. First colour milestone of the canvas-
// observable arc; geometry from s237 m1 + appearance from s238 m1 =
// a path the user can see in every render mode.
//
// s239 m1 — SYMMETRIC second half of the appearance branch. Adds
// `set_stroke "<color_token>"` and the matching `stroke` read query
// to ObjectProxy. Mirror-shape sibling to set_fill / fill: same
// command (EditAppearanceCommand, fifth "existing command fits"
// case for verify-before-assume in a row after s236 m1 / s237 m1 /
// s238 m1), same pump pair (fill_attr_to_fill_style /
// fill_style_to_fill_attr — `stroke.paint` is a FillStyle so the
// pumps cover it without modification), same direct-override
// semantics (clears stroke_swatch_id, clears bound_style — the
// same break_swatch_binding canon as the fill side), same
// universal-across-SceneNode-subtypes posture, same no-op early-
// return on value-unchanged. No new pump, no new helper, no new
// command — the architectural shape s238 m1 established lifts
// verbatim to the stroke axis.
//
// The anon-namespace equality helper renames from
// `fill_style_equals_for_set_fill` to `fill_style_equals_byte_rounded`
// — same body, neutral name. The "for_set_fill" suffix earned its
// retirement the moment a second caller (set_stroke) needed the
// same byte-rounded comparison. Same Unix-style naming as the
// curvz_utils pumps: describe what the function does, not who
// calls it.
//
// What set_stroke does NOT touch: width, cap, join, miter, opacity.
// All five stroke fields beyond `paint` are preserved through the
// verb — stroke_new is constructed as `node->stroke` with only the
// `paint` field overwritten. EditAppearanceCommand carries the
// full StrokeStyle before AND after, so undo restores the entire
// stroke struct (paint reverts, the unchanged fields self-assign).
// This is scope-limit by design: stroke-width / cap / join would
// each be a NUMERIC-arg or ENUM-arg verb, materially different
// from set_stroke's color-token vocabulary, and bundling them
// would blur the "what does this verb do" contract. The numeric
// stroke verbs are scheduled for s240+ as set_stroke_width etc.
//
// Visual scope-limit for m1: fresh-mint Paths have stroke.width
// defaulting to 0.0, so set_stroke "black" lands a black paint on
// a zero-width stroke — the canvas-visible outline doesn't appear
// until set_stroke_width sets a nonzero width. The smoke is
// trace-channel-only on the stroke side (asserts `stroke ==
// "#000000"` etc.); the visual outline closes in the m2 set_stroke_width
// milestone. Same canon as set_fill — the BUILD UP / VALIDATE /
// TEAR DOWN structure owns its conditions, so the smoke can't
// pre-set stroke-width via the inspector to fake the visual; that
// preconditions-violation would defeat the smoke's purpose.
// Three-channel acceptance for m1 = trace pass + clean build +
// (deferred) visual outline once width verb lands.
//
// Verify-before-assume's fifth "fits unmodified" case in a row.
// First task this session: reading EditAppearanceCommand's
// stroke_before / stroke_after fields and confirming both are
// whole-StrokeStyle (FillStyle paint + 5 numeric/enum fields), not
// just paint. Confirmed: the 14-arg constructor's `sb` and `sa`
// parameters are StrokeStyle, same shape as `fb` / `fa` for fill.
// Symmetric capture across axes. The discipline keeps paying back.
//
// s239 m2 — CLOSES the appearance branch's visual loop. Adds
// `set_stroke_width <number>` and the matching `stroke_width`
// read query to ObjectProxy. First NUMERIC-arg verb on this
// proxy — earlier verbs took string tokens (path data, colour
// tokens, direction tokens) or no args (toggle / duplicate).
// Same EditAppearanceCommand ride as set_fill and set_stroke
// (SIXTH "fits unmodified" case for verify-before-assume in a
// row); the command's StrokeStyle slot covers width.
//
// Anon namespace gains `arg_as_double` (lifted in shape from
// StylesScriptable / LayersScriptable / GuidesScriptable's
// helpers of the same name — accepts both ScriptValue::Double
// (native) and ScriptValue::Int (coerced via static_cast<double>),
// returns fallback for any other kind). The script DSL emits
// `3` as Int and `3.0` as Double, so the coercion lets users
// write either `set_stroke_width 2` or `set_stroke_width 2.0`
// without surprise.
//
// What set_stroke_width does NOT touch: paint, cap, join, miter,
// opacity. StrokeStyle has six fields; m1 owns paint, m2 owns
// width, the remaining four (cap, join, miter, opacity) wait
// for their own milestones. stroke_new is constructed as
// `node->stroke` with only the `width` field overwritten — same
// shape as set_stroke's paint-only overwrite, just a different
// scalar slot.
//
// No clamp / no refusal on negative or zero values. The script
// surface accepts whatever the user supplies; the renderer
// handles edge cases (zero width = no outline rendered, which
// is the m1 default state; negative widths zero out at the
// Cairo seam). This matches StylesScriptable::stroke_width's
// shipped convention: "Inspector clamps at its UI layer; the
// model accepts whatever the script supplies." Consistent
// behaviour across the two scriptables that touch the same
// underlying field.
//
// Direct-override semantics: clears bound_style on the after side
// (a direct width edit breaks the style binding, mirroring set_fill
// / set_stroke / inspector). Does NOT clear stroke_swatch_id or
// fill_swatch_id — the swatch bindings represent paint references,
// and width edits don't touch paint. This is the same partial-clear
// convention set_stroke established in m1 (set_stroke clears only
// stroke_swatch_id, not fill_swatch_id; set_fill clears only
// fill_swatch_id, not stroke_swatch_id). The inspector's
// mutate_appearance funnel goes further and clears BOTH swatch
// bindings on ANY appearance edit (see style/StyleInterop.cpp
// lines 95-104) — script-side has been narrower throughout this
// arc. Whether to align the script-side with mutate_appearance's
// both-sides-always-clear is a backlog item for s240+; m2 holds
// the line set by m1 to avoid a behaviour-shift mid-arc.
//
// New proxy query:
//   - `stroke_width` (real) — returns ScriptValue::real(node->
//     stroke.width). Same shape as LayersProxy's `opacity` and
//     GuidesProxy's `x` / `y` / `angle`. The DSL's assert uses
//     a Double literal RHS (`assert obj stroke_width == 2.0`)
//     for byte-equal compare; integer-like values can be asserted
//     either as `== 2.0` (Double-Double match) or — since
//     ScriptValue equality is STRICT-kind — must NOT use `== 2`
//     (Int-Double mismatch).
//
// Visual loop closes here. With m1's set_stroke + m2's
// set_stroke_width, a script can produce a fully-styled visible
// shape on canvas: geometry from s237 m1, fill from s238 m1,
// stroke paint from s239 m1, stroke width from s239 m2. The
// canvas-observable arc's appearance branch is now complete on
// the colour-and-thickness axis; remaining stroke fields (cap,
// join, miter, opacity) and gradient / swatch bindings stay on
// the backlog.
//
// s240 m1 — closes the appearance branch's ENUM-TOKEN sub-axis.
// Adds the paired verbs `set_stroke_cap "<token>"` and
// `set_stroke_join "<token>"` plus the matching `cap` and
// `join` read queries to ObjectProxy. First enum-token verbs on
// this proxy — earlier verbs took strings (path data, names),
// colour tokens (set_fill, set_stroke via fill_attr_to_fill_style),
// or numeric values (set_stroke_width via arg_as_double). The
// vocabulary here is the LineCap / LineJoin enums surfaced as
// lowercase strings ("butt" / "round" / "square" for cap;
// "miter" / "round" / "bevel" for join) — matches SVG's
// stroke-linecap / stroke-linejoin attribute conventions
// exactly.
//
// Same EditAppearanceCommand ride as set_fill / set_stroke /
// set_stroke_width — SEVENTH and EIGHTH "fits unmodified" cases
// in a row for verify-before-assume (fourth and fifth in two
// sessions on EditAppearanceCommand). The command's whole-
// StrokeStyle capture covers cap and join along with paint /
// width / miter / opacity without modification; both fields are
// already on StrokeStyle (SceneNode.hpp:76-77), so the existing
// capture shape covers cap-only and join-only edits by
// construction.
//
// Anon namespace gains four helpers, lifted in shape from
// StylesScriptable.cpp's anon-namespace helpers of the same
// names (s225 m1 vintage): decode_cap / cap_to_string and
// decode_join / join_to_string. Encode + decode live next to
// each other in the same file (Unix-pump convention). Logged
// as a candidate pump pair for promotion to curvz_utils when
// a third caller appears — backlog item this session. Two
// callers (StylesScriptable + ObjectsScriptable) is duplication
// but not yet a pattern; three is the threshold for lifting.
//
// Unknown-token policy diverges from set_fill / set_stroke: an
// unknown cap or join token is a SILENT NO-OP, not a degrade to
// a safe default. Cap and join have no documented safe-default
// the way colour has CurrentColor; matching
// StylesScriptable::stroke_cap's "unknown vocab" early return
// is the right read. The model state stays where it was; no
// command pushed, no notify_doc_changed.
//
// Partial binding-clear: clears bound_style on the after side
// (style binding break, mirroring every other appearance verb),
// preserves both fill_swatch_id and stroke_swatch_id (swatch
// bindings represent paint references, cap/join edits don't
// touch paint). Same convention set_stroke_width established.
//
// Visual: with a stroke wide enough to read (40px+ from s239 m2's
// vocabulary) and a polygon with visible corners (rect / star /
// etc.), join = "round" bevels corners into smooth arcs; join =
// "bevel" cuts them as straight diagonals; cap = "round" makes
// stroke ends bulge into half-circles. Immediately observable on
// canvas. The appearance branch's enum-token sub-axis closes
// here; remaining stroke fields (miter, opacity) are numeric
// and reuse arg_as_double from s239 m2 — scheduled for s241.
//
// s241 m1 — closes the appearance branch's remaining stroke
// axis. Adds `set_stroke_miter <number>` and `set_stroke_opacity
// <number>` plus the matching `miter` and `stroke_opacity` read
// queries. Both verbs reuse arg_as_double from s239 m2 — no
// new helper this milestone, no new pump pair, no new command.
// NINTH and TENTH "fits unmodified" applications of verify-
// before-assume in a row (sixth and seventh in three sessions
// on EditAppearanceCommand). The appearance branch is now
// CLOSED on all six StrokeStyle fields: paint (s239 m1) +
// width (s239 m2) + cap (s240 m1) + join (s240 m1) + miter
// (s241 m1) + opacity (s241 m1). Plus the fill axis (s238 m1).
// Remaining stroke / fill work is gradient and swatch bindings;
// neither is on this milestone's path.
//
// Naming divergence noted: SceneNode::stroke.miter (this file)
// vs style::Style::stroke.miter_limit (StylesScriptable). Both
// default to 4.0; same concept, different field names. The
// SceneNode field's shorter name wins for the verb / query
// here (`set_stroke_miter` / `miter`); the styles surface keeps
// `stroke_miter_limit` because its proxy IS a Style, where
// stroke_* / shadow_* / fill_* prefixes disambiguate across
// the surface. Backlog item: whether to align the field names
// across the two structs.
//
// VISUAL: miter only matters at acute corners with miter join;
// opacity is observable as alpha multiplier on the resolved
// stroke paint. Phase 10 of smoke 40 layers a hairpin polygon
// (sharp angle, miter join, miter=100 to extend the point)
// with stroke_opacity=0.5 so the fill colour shows through
// the outline — both effects visible in the same shape.
//
// ── Command pushes (s231 m2) ─────────────────────────────────────────────
//
// `new` pushes an AddNodeCommand; `delete` pushes a DeleteObjectCommand.
// Both commands existed before m2 (canvas tools have been using them
// since well before the iid migration); m2 wires them into a new code
// path without changing their shape. Each command captures the parent
// as a raw SceneNode* — same legacy capture LayersScriptable's `new`
// and Canvas's tool-driven AddNodeCommand use. The s167 migration
// was specifically about EditPathCommand and friends; structural
// commands (Add / Delete / Insert) retain their pre-migration capture
// shape and remain in scope for a future structural-iid migration.
//
// Direct mutation is performed BEFORE the command push, mirroring
// LayersScriptable::new — the user's perspective is "the verb did
// the thing"; Ctrl+Z then UNDOES it. The push site does not call
// command->execute() (that's the redo path, only invoked via the
// history's redo()).
//
// After every direct mutation, `doc->invalidate_iid_index()` runs so
// the lazy iid map rebuilds on the next find_by_iid call. The s168 m6
// "invalidate before resolve" inside each command's execute/undo path
// covers redo / undo themselves; the push site's invalidation covers
// the initial mutation.
//
// ── find_by_name and find_by_type are invoke-shaped ──────────────────────
//
// Same grammar constraint LayersScriptable surfaces: today's query()
// can't take args. `get objects find_by_name "X"` would put
// "find_by_name" in the property slot and "X" would be ignored. We
// expose find_by_name and find_by_type as INVOKE verbs on the
// collection — read-shaped semantics, invoke-shaped dispatch.
// Scripts write `objects find_by_name "X"` (no `get` prefix) and
// pick up the result through the `=` output line / `set X to result`
// binding. Future grammar extension: when query() grows an args
// parameter, both can graduate to real queries.
//
// ── Tree walk ────────────────────────────────────────────────────────────
//
// Several queries (count, all_iids, find_by_name, find_by_type) walk
// every layer's subtree depth-first. The walk is file-local because
// it's a small helper and the iteration shape (filter by
// is_scene_object) is specific to this Scriptable's scope choice.
// If a future Scriptable needs the same walk it can be promoted to
// curvz_utils — but right now this is the only caller, and the
// helper stays adjacent to the predicate it uses.
//
// The walk visits every node in the tree rooted at each top-level
// layer, applies a visitor function, and recurses into `children`
// and the structural-input slots (clip_shape, blend_source_a / _b,
// warp_source). The non-children slots matter because a ClipGroup's
// clip_shape, a Blend's source nodes, and a Warp's source node are
// all "real" scene nodes with iids that should be addressable. The
// derived caches (blend_cache, warp_cache, warp_glyph_cache) are
// NOT walked — same exclusion as CurvzDocument's m_iid_index,
// because iids inside caches aren't stable across rebuilds.
//
// ── parent_iid walk ──────────────────────────────────────────────────────
//
// The element query `parent_iid` needs the immediate parent container
// of a given node. Canvas.cpp has a `find_parent` helper but it only
// descends one level into Group/Compound — insufficient for the full
// tree shape we cover (ClipGroup, Blend, Warp containers; nested
// Group-inside-Group). The file-local parent walk here is a clean
// recursive scan: visit every container slot, check whether the
// target lives directly under it, return the container's iid if so.
//
// O(N) per call where N is the tree size. The number of parent_iid
// queries in a script is bounded by however many `get objects.<iid>
// parent_iid` lines the author writes; the walk cost is microseconds
// on typical scenes (hundreds of nodes). If parent_iid becomes hot
// or a sibling Scriptable wants the same walk, promote to
// curvz_utils as `find_parent_iid` — same iid-resolution shape as
// find_by_iid, but returning the owner's iid instead of the node.
//
// s231 m2 adds a SIBLING walker `find_parent_node_in_*` that returns
// the parent SceneNode pointer instead of its iid (needed by `delete`
// to manipulate the parent's children vector and capture the
// original index for DeleteObjectCommand). The two walkers share the
// same recursive structure; both file-local. If a future caller
// wants both pieces of information out of a single walk, the seam
// for a promoted helper is clear — but two single-purpose walkers
// is cheaper today than one general one.

#include "scripting/ObjectsScriptable.hpp"
#include "scripting/Scriptable.hpp"

#include "CommandHistory.hpp"
#include "CurvzDocument.hpp"
#include "CurvzProject.hpp"
#include "SceneNode.hpp"
#include "curvz_utils.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace curvz::scripting {

// ── Scope predicate and type-string mapping ──────────────────────────────
//
// File-local helpers. is_scene_object decides whether a SceneNode
// belongs in the `objects` collection (option (ii) from the s230
// design fork — real scene contents only, no Layer/Guide/special-
// layer types). type_to_string returns the lowercase token used in
// find_by_type args and in the `type` element query.

namespace {

bool is_scene_object(const Curvz::SceneNode* n) {
    if (!n) return false;
    using T = Curvz::SceneNode::Type;
    switch (n->type) {
        case T::Path:
        case T::Group:
        case T::Compound:
        case T::ClipGroup:
        case T::Blend:
        case T::Warp:
        case T::Text:
        case T::TextBox:
        // s309 m1a — Mgr-with-views shape. Both are scriptable scene
        // objects, same as TextBox. Construction lands in m1b; the
        // enum value is reachable as soon as that ships.
        case T::TextBoxMgr:
        case T::TextBoxView:
        case T::Image:
        case T::Ref:
        case T::Measurement:
            return true;
        case T::Layer:
        case T::Guide:
        case T::GuideLayer:
        case T::RefLayer:
        case T::MeasureLayer:
        case T::GridLayer:
        case T::MarginLayer:
            return false;
    }
    return false;
}

// Lowercase type-token vocabulary. Stable across the Scriptable's
// lifetime (part of the documented contract — see header). The
// inverse mapping in find_by_type uses the same strings.
std::string type_to_string(Curvz::SceneNode::Type t) {
    using T = Curvz::SceneNode::Type;
    switch (t) {
        case T::Path:        return "path";
        case T::Group:       return "group";
        case T::Compound:    return "compound";
        case T::ClipGroup:   return "clipgroup";
        case T::Blend:       return "blend";
        case T::Warp:        return "warp";
        case T::Text:        return "text";
        case T::TextBox:     return "textbox";
        // s309 m1a — Mgr-with-views shape. Tokens lowercased to match
        // the existing convention; the scripting API exposes these
        // strings as the type vocabulary.
        case T::TextBoxMgr:  return "textboxmgr";
        case T::TextBoxView: return "textboxview";
        case T::Image:       return "image";
        case T::Ref:         return "ref";
        case T::Measurement: return "measurement";
        // Out-of-scope types — type_to_string never gets called on
        // these in m1 because callers gate on is_scene_object first.
        // Return "" anyway so an unanticipated caller path doesn't
        // fall through to UB; "" is the "not in scope" sentinel
        // matching how find_by_name / find_by_type signal miss.
        case T::Layer:
        case T::Guide:
        case T::GuideLayer:
        case T::RefLayer:
        case T::MeasureLayer:
        case T::GridLayer:
        case T::MarginLayer:
            return "";
    }
    return "";
}

// Pull a string-shaped arg (quoted or token). Returns "" for any
// non-string ScriptValue. Matches the equivalent helper in
// LayersScriptable.cpp and GuidesScriptable.cpp.
std::string arg_as_string(const ScriptValue& v) {
    if (v.kind == ValueKind::String) return v.s;
    return {};
}

// Parse a bool from a ScriptValue. Accepts Bool directly; falls back
// to text "true"/"false" (case-sensitive — matches the DSL literal
// lexer). Returns `fallback` for anything else. Mirrors the
// LayersScriptable.cpp helper of the same name (s232 m3 — added
// when element mutating verbs landed on the proxy).
bool arg_as_bool(const ScriptValue& v, bool fallback) {
    if (v.kind == ValueKind::Bool)   return v.b;
    if (v.kind == ValueKind::String) {
        if (v.s == "true")  return true;
        if (v.s == "false") return false;
    }
    return fallback;
}

// Parse a double from a ScriptValue. Accepts Double directly; accepts
// Int and coerces via static_cast<double> (so the DSL's `2` and
// `2.0` literals both reach the same value at the verb's seam,
// matching the user's typical expectation when typing whole-number
// widths). Returns `fallback` for any other kind. Lifted in shape
// from StylesScriptable / LayersScriptable / GuidesScriptable's
// helpers of the same name (s225 m1 / s232 m3 vintage). Added to
// this file in s239 m2 — first numeric-arg verb on ObjectProxy
// (set_stroke_width).
double arg_as_double(const ScriptValue& v, double fallback) {
    switch (v.kind) {
        case ValueKind::Double: return v.d;
        case ValueKind::Int:    return static_cast<double>(v.i);
        default:                return fallback;
    }
}

// ── LineCap / LineJoin string enums ──────────────────────────────────────
//
// s240 m1. Lifted in shape from StylesScriptable.cpp's anon-namespace
// helpers of the same name (s225 m1 vintage). The C++ enums are
// surfaced as lowercase strings:
//
//   LineCap : "butt" / "round" / "square"
//   LineJoin: "miter" / "round" / "bevel"
//
// encode_* is total (every enum value maps); decode_* returns
// nullopt for unknown / mis-cased input. Verb-side treats nullopt
// as no-op — silent rejection, NOT the CurrentColor-degrade
// pattern of set_fill / set_stroke. Cap and join have no
// documented safe-default to fall back to; an unknown token is a
// no-op and the model state stays where it was. Matches
// StylesScriptable::stroke_cap's "unknown vocab" early return.
//
// Why lowercase: matches SVG attribute conventions and is
// consistent with how `cap` / `join` would round-trip through SVG
// export. No translation table needed at the export seam.
//
// Pump-shape: encode + decode live next to each other in the same
// file, Unix-pump convention. Logged candidate for promotion to
// `curvz_utils` as a `cap_join_strings` pump pair when a second
// caller appears — backlog item this session.
const char* cap_to_string(Curvz::LineCap c) {
    switch (c) {
        case Curvz::LineCap::Butt:   return "butt";
        case Curvz::LineCap::Round:  return "round";
        case Curvz::LineCap::Square: return "square";
    }
    return "butt";  // unreachable; satisfies the compiler
}

std::optional<Curvz::LineCap> decode_cap(const std::string& s) {
    if (s == "butt")   return Curvz::LineCap::Butt;
    if (s == "round")  return Curvz::LineCap::Round;
    if (s == "square") return Curvz::LineCap::Square;
    return std::nullopt;
}

const char* join_to_string(Curvz::LineJoin j) {
    switch (j) {
        case Curvz::LineJoin::Miter: return "miter";
        case Curvz::LineJoin::Round: return "round";
        case Curvz::LineJoin::Bevel: return "bevel";
    }
    return "miter";  // unreachable
}

std::optional<Curvz::LineJoin> decode_join(const std::string& s) {
    if (s == "miter") return Curvz::LineJoin::Miter;
    if (s == "round") return Curvz::LineJoin::Round;
    if (s == "bevel") return Curvz::LineJoin::Bevel;
    return std::nullopt;
}

// ── Scene-tree walker ────────────────────────────────────────────────────
//
// Depth-first visit of every node under each top-level layer in `doc`,
// calling `visit(SceneNode*)` for each. Visits structural-input slots
// (clip_shape, blend sources, warp source) in addition to children,
// matching the iid-index walk coverage documented in CurvzDocument's
// m_iid_index comment.
//
// The visitor is called for EVERY node (in-scope and out-of-scope);
// callers gate on is_scene_object themselves. Keeps the walker
// reusable for future Scriptables that want a different filter.
//
// Early-exit support via a bool return — visitors that return true
// signal "found what I want, stop walking." Useful for find_by_*
// implementations.
template <class F>
bool walk_doc_tree(Curvz::CurvzDocument* doc, F&& visit) {
    if (!doc) return false;
    std::function<bool(Curvz::SceneNode*)> rec =
        [&](Curvz::SceneNode* n) -> bool {
            if (!n) return false;
            if (visit(n)) return true;
            for (auto& c : n->children) if (rec(c.get())) return true;
            if (n->clip_shape     && rec(n->clip_shape.get()))     return true;
            if (n->blend_source_a && rec(n->blend_source_a.get())) return true;
            if (n->blend_source_b && rec(n->blend_source_b.get())) return true;
            if (n->warp_source    && rec(n->warp_source.get()))    return true;
            return false;
        };
    for (auto& l : doc->layers) {
        if (rec(l.get())) return true;
    }
    return false;
}

// ── Parent walk ──────────────────────────────────────────────────────────
//
// Find the immediate parent of `target` in the doc tree. The parent
// is whichever container slot owns `target` — `children[i]`,
// `clip_shape`, `blend_source_a`, `blend_source_b`, or `warp_source`.
// Returns the parent's iid, or "" if no container holds target.
//
// "Container slot owns target" means: target is the .get() of some
// unique_ptr held by the parent. We compare pointers, not iids —
// pointer identity is the canonical "is this the slot that owns
// you" test for unique_ptr-tree shapes.
std::string find_parent_iid_in_doc(Curvz::CurvzDocument* doc,
                                   Curvz::SceneNode* target) {
    if (!doc || !target) return {};
    std::string result;
    std::function<bool(Curvz::SceneNode*)> rec =
        [&](Curvz::SceneNode* n) -> bool {
            if (!n) return false;
            for (auto& c : n->children) {
                if (c.get() == target) {
                    result = n->internal_id;
                    return true;
                }
            }
            if (n->clip_shape && n->clip_shape.get() == target) {
                result = n->internal_id;
                return true;
            }
            if (n->blend_source_a && n->blend_source_a.get() == target) {
                result = n->internal_id;
                return true;
            }
            if (n->blend_source_b && n->blend_source_b.get() == target) {
                result = n->internal_id;
                return true;
            }
            if (n->warp_source && n->warp_source.get() == target) {
                result = n->internal_id;
                return true;
            }
            // Recurse: descend into every owned slot.
            for (auto& c : n->children)
                if (rec(c.get())) return true;
            if (n->clip_shape && rec(n->clip_shape.get())) return true;
            if (n->blend_source_a && rec(n->blend_source_a.get())) return true;
            if (n->blend_source_b && rec(n->blend_source_b.get())) return true;
            if (n->warp_source && rec(n->warp_source.get())) return true;
            return false;
        };
    for (auto& l : doc->layers) {
        if (rec(l.get())) return result;
    }
    return {};
}

// Project-wide parent search: the iid we hold may live in any doc.
// Walk each doc until one finds the target node and returns its
// parent iid.
std::string find_parent_iid_in_project(Curvz::CurvzProject* proj,
                                       Curvz::SceneNode* target) {
    if (!proj || !target) return {};
    for (auto& doc_up : proj->documents) {
        auto* doc = doc_up.get();
        std::string p = find_parent_iid_in_doc(doc, target);
        if (!p.empty()) return p;
    }
    return {};
}

// ── Parent-node walk (s231 m2 — sibling of the iid walker) ───────────────
//
// Same recursive shape as find_parent_iid_in_doc, but returns the
// parent SceneNode* AND the index of `target` within the parent's
// children vector. `delete` needs both: the SceneNode* to construct
// the DeleteObjectCommand (which captures `parent` directly), and
// the index for the command's undo path so the snapshot re-inserts
// at the original position.
//
// If the target is owned by a non-`children` slot (clip_shape,
// blend_source_a, blend_source_b, warp_source), `index_out` is set
// to -1 and the parent pointer still returns. delete refuses to
// proceed in that case — pulling a node out of a structural slot
// would dissolve the containing ClipGroup / Blend / Warp, which is
// a different surface than "delete a scene leaf." Scheduled for a
// later milestone if scripts genuinely need to dissolve clip /
// blend / warp containers via `objects delete`.
struct ParentSlot {
    Curvz::SceneNode* parent;
    int               child_index;  // -1 if target lives in a non-children slot
};

ParentSlot find_parent_node_in_doc(Curvz::CurvzDocument* doc,
                                   Curvz::SceneNode* target) {
    ParentSlot found{nullptr, -1};
    if (!doc || !target) return found;
    std::function<bool(Curvz::SceneNode*)> rec =
        [&](Curvz::SceneNode* n) -> bool {
            if (!n) return false;
            for (int i = 0; i < (int)n->children.size(); ++i) {
                if (n->children[i].get() == target) {
                    found = {n, i};
                    return true;
                }
            }
            if (n->clip_shape && n->clip_shape.get() == target) {
                found = {n, -1};
                return true;
            }
            if (n->blend_source_a && n->blend_source_a.get() == target) {
                found = {n, -1};
                return true;
            }
            if (n->blend_source_b && n->blend_source_b.get() == target) {
                found = {n, -1};
                return true;
            }
            if (n->warp_source && n->warp_source.get() == target) {
                found = {n, -1};
                return true;
            }
            for (auto& c : n->children)
                if (rec(c.get())) return true;
            if (n->clip_shape && rec(n->clip_shape.get())) return true;
            if (n->blend_source_a && rec(n->blend_source_a.get())) return true;
            if (n->blend_source_b && rec(n->blend_source_b.get())) return true;
            if (n->warp_source && rec(n->warp_source.get())) return true;
            return false;
        };
    for (auto& l : doc->layers) {
        if (rec(l.get())) return found;
    }
    return found;
}

ParentSlot find_parent_node_in_project(Curvz::CurvzProject* proj,
                                       Curvz::SceneNode* target) {
    if (!proj || !target) return {nullptr, -1};
    for (auto& doc_up : proj->documents) {
        auto* doc = doc_up.get();
        ParentSlot p = find_parent_node_in_doc(doc, target);
        if (p.parent) return p;
    }
    return {nullptr, -1};
}

// ── Type-token → SceneNode::Type (s231 m2) ───────────────────────────────
//
// Inverse of type_to_string for the small subset of types `new` can
// mint in m2. Returns nullopt for unrecognised tokens or for types
// that aren't safely creatable from scratch via this verb today
// (Compound / ClipGroup / Blend / Warp need structural inputs;
// Text / Image need content; Ref / Measurement live under special
// layers). Those are reachable later milestones; m2 ships path +
// group, the two leaf-vs-container types that are minimal to mint.
//
// std::optional avoided to keep the header surface clean — sentinel
// is the bool return; the out-param carries the type.
bool parse_type_token(std::string_view tok, Curvz::SceneNode::Type& out) {
    using T = Curvz::SceneNode::Type;
    if (tok == "path")  { out = T::Path;  return true; }
    if (tok == "group") { out = T::Group; return true; }
    return false;
}

// ── Mint a fresh SceneNode of the given type (s231 m2) ───────────────────
//
// File-local factory. Sets the bare-minimum fields each type needs to
// be a valid in-scope scene object — type set, internal_id minted
// (default ctor already does this; we don't re-mint), name LEFT EMPTY
// (the documented contract — scripts use rename to assign a name),
// children empty by default ctor.
//
// Path-specific: allocates a default-constructed PathData so the
// node is structurally a Path (path != nullptr is the discriminant
// renderers and serialisers rely on). The PathData has no nodes; the
// renderer skips empty paths defensively (Canvas_draw.cpp / SvgWriter
// both check nodes.empty()).
//
// Group-specific: no extra setup beyond type — children vector is
// already empty.
std::unique_ptr<Curvz::SceneNode>
mint_scene_object(Curvz::SceneNode::Type t) {
    auto n = std::make_unique<Curvz::SceneNode>();
    // internal_id and a default name vocabulary are NOT touched —
    // default ctor mints internal_id, and the contract is name == ""
    // until a future rename verb assigns one.
    n->type = t;
    n->name.clear();  // explicit: documented "newly created" sentinel
    if (t == Curvz::SceneNode::Type::Path) {
        n->path = std::make_unique<Curvz::PathData>();
    }
    return n;
}

// ── Freshen internal_ids on a cloned subtree (s234 m4) ───────────────────
//
// `clone_node` copies internal_id verbatim from source — it has to,
// because m2's `delete` relies on snap->internal_id matching the
// erased node's iid for redo / undo to reach the same logical node.
// For `duplicate` we want the OPPOSITE: the clone must have its own
// stable identity so the new node and the original coexist in the
// iid index without collision. Walk the cloned subtree recursively
// and mint a fresh internal_id at every visited slot (children,
// clip_shape, blend_source_*, warp_source — same descent rules
// CurvzDocument's index_walk uses, since those are the slots that
// participate in the iid index).
//
// blend_cache / warp_glyph_cache / warp_cache are NOT walked — same
// exclusion CurvzDocument applies. Those are derived rebuilds; their
// iids aren't stable across renders.
//
// Note: this is the script path's analog of canvas's freshen_ids,
// which mints fresh SVG `id` and `name` but NOT internal_id. Canvas
// has the same latent risk (duplicating a complex subtree leaves the
// clone with the original's iids in the index), but addressing that
// is a separate s234-or-later band-3 mop-up; m4 scopes the fix to
// the script path where the smoke would otherwise definitely trip
// the collision.
void freshen_internal_ids(Curvz::SceneNode* n) {
    if (!n) return;
    n->internal_id = Curvz::generate_internal_id();
    for (auto& c : n->children) freshen_internal_ids(c.get());
    if (n->clip_shape)     freshen_internal_ids(n->clip_shape.get());
    if (n->blend_source_a) freshen_internal_ids(n->blend_source_a.get());
    if (n->blend_source_b) freshen_internal_ids(n->blend_source_b.get());
    if (n->warp_source)    freshen_internal_ids(n->warp_source.get());
}

// ── Descendant check for cycle-refusal in reparent (s234 m4) ─────────────
//
// Returns true if `candidate` is anywhere in the subtree rooted at
// `ancestor` (excluding ancestor itself — that's a separate equals-
// self check at the verb level). Used by `reparent` to refuse moves
// that would form a cycle (the new parent can't be a descendant of
// the moved node; doing so would orphan the moved node from the
// tree while leaving it pointing into its own subtree as the parent).
//
// Same descent rules as the iid index walk and freshen_internal_ids
// — children plus the four structural slots. Pointer equality is the
// "is this slot exactly that node" test; iid equality would also work
// but pointer equality is the canonical unique_ptr-tree shape check.
bool is_descendant_of(Curvz::SceneNode* ancestor, Curvz::SceneNode* candidate) {
    if (!ancestor || !candidate) return false;
    for (auto& c : ancestor->children) {
        if (c.get() == candidate) return true;
        if (is_descendant_of(c.get(), candidate)) return true;
    }
    if (ancestor->clip_shape) {
        if (ancestor->clip_shape.get() == candidate) return true;
        if (is_descendant_of(ancestor->clip_shape.get(), candidate)) return true;
    }
    if (ancestor->blend_source_a) {
        if (ancestor->blend_source_a.get() == candidate) return true;
        if (is_descendant_of(ancestor->blend_source_a.get(), candidate)) return true;
    }
    if (ancestor->blend_source_b) {
        if (ancestor->blend_source_b.get() == candidate) return true;
        if (is_descendant_of(ancestor->blend_source_b.get(), candidate)) return true;
    }
    if (ancestor->warp_source) {
        if (ancestor->warp_source.get() == candidate) return true;
        if (is_descendant_of(ancestor->warp_source.get(), candidate)) return true;
    }
    return false;
}

// ── Container-type check for reparent destination (s234 m4) ──────────────
//
// `reparent` accepts containers — types whose `children` vector is
// part of the user-controlled tree shape. Leaves (Path / Text /
// Image / Ref / Measurement) are refused as destinations even if
// they happen to have a non-empty children vector at the moment of
// the call (which shouldn't be possible by construction, but a
// type-level refusal is the cleaner contract than a runtime
// children.size() check that depends on whatever state the leaf
// happens to be in).
//
// Layer and the special-layer types (GuideLayer, RefLayer, etc.)
// ARE valid containers — they sit at the top of doc->layers and
// hold children directly. They're "out of scope" for `objects` as
// addressing surfaces (the iid wouldn't resolve through
// `objects.<iid>` because is_scene_object filters them out), but
// they CAN be the destination of a reparent issued via
// `objects.<iid> reparent` — the dest iid is resolved through
// find_by_iid which walks the whole project tree without
// scope-filtering. That keeps the reparent verb's reach broad
// (a script can move a Path into a different Layer just as easily
// as into a sibling Group). The only types refused are leaves.
bool is_container_type(Curvz::SceneNode::Type t) {
    using T = Curvz::SceneNode::Type;
    switch (t) {
        case T::Group:
        case T::Compound:
        case T::ClipGroup:
        case T::Layer:
        case T::GuideLayer:
        case T::RefLayer:
        case T::MeasureLayer:
        case T::GridLayer:
        case T::MarginLayer:
            return true;
        case T::Path:
        case T::Text:
        case T::TextBox:
        // s309 m1a — Mgr-with-views shape. Both refused as reparent
        // destinations, same reasoning as TextBox: their `children`
        // slots are structurally assigned (Mgr holds an ordered list
        // of views; View holds a boundary at [0]), not a user-
        // controlled list. Accepting reparents would break the
        // container's invariants.
        case T::TextBoxMgr:
        case T::TextBoxView:
        case T::Image:
        case T::Ref:
        case T::Measurement:
        case T::Blend:
        case T::Warp:
        case T::Guide:
            // Blend and Warp have structural slots (blend_source_*,
            // warp_source), not a user-controlled `children` vector.
            // Refused as reparent destinations — reparenting into a
            // structural slot is a separate operation than reparenting
            // into a children list, and the verb's contract is the
            // latter. TextBox is in the same family: even though its
            // parts live in `children`, the slots are structurally
            // assigned (boundary at [0], text at [1]) and not a user-
            // controlled list — accepting reparents would break the
            // container's invariants. Guide is a leaf typed like a
            // marker, no children. All return false.
            return false;
    }
    return false;
}

// s238 m1 / s239 m1 — field-level FillStyle equality for the no-op
// early-return on set_fill and set_stroke. We compare exactly the
// fields fill_attr_to_fill_style can produce: type plus (for Solid)
// r/g/b/a. Stops / gradient geometry are NOT compared — the script-
// side pump can't produce gradients in m1, so two gradient FillStyles
// that differ only in stops would always pass equality from the
// verb's perspective; the verb body guarantees the new FillStyle is
// one of {None, CurrentColor, Solid} so the gradient-stops difference
// never reaches here. Strict-equal on gradient fields would defeat
// the m1 no-op check; we deliberately omit them. If a future
// milestone teaches set_fill / set_stroke to accept gradient tokens,
// the comparison surface expands here too.
//
// Neutral name (s239 m1 rename from `fill_style_equals_for_set_fill`):
// `byte_rounded` describes WHAT the helper does (channel comparison
// at hex-byte resolution), not WHO calls it. Two callers now use it
// — fill side and stroke side — and the symmetry will hold for any
// future paint-bearing verb on this surface.
bool fill_style_equals_byte_rounded(const Curvz::FillStyle& a,
                                    const Curvz::FillStyle& b) {
    if (a.type != b.type) return false;
    if (a.type != Curvz::FillStyle::Type::Solid) return true;
    // Solid: hex-byte resolution matters (the round-trip through
    // %02x clamps differences smaller than 1/255 to byte-equal).
    // Compare on the rounded byte values, not raw doubles, so a
    // hex-clean fill_new doesn't fail equality on float-precision
    // drift from a prior parse.
    auto byte = [](double v) { return (int)std::round(v * 255.0); };
    return byte(a.r) == byte(b.r)
        && byte(a.g) == byte(b.g)
        && byte(a.b) == byte(b.b)
        && byte(a.a) == byte(b.a);
}

} // anon namespace

// ── ObjectProxy — transient per-instance Scriptable ──────────────────────
//
// Materialised on demand by ObjectsScriptable::proxy_for(iid).
// Lifetime is bounded by the listener's ResolvedObject wrapper —
// destroyed when the wrapper goes out of scope at end-of-statement.
//
// Registered via the `unregistered` tag — proxies are invisible to
// the global registry. Same lifetime contract and registry posture
// as LayerProxy / GuideProxy.
//
// m1 + m2 shipped READ-SIDE only. m3 wires invoke() to the five
// element mutating verbs: toggle_visible / set_visible / toggle_locked
// / set_locked / rename. Each pushes EditObjectFieldCommand on the
// global undo stack on success (direct mutation in the verb body
// happens BEFORE the push, mirroring LayerProxy::invoke).
class ObjectProxy : public Scriptable {
public:
    ObjectProxy(ObjectsScriptable::ProjectGetter get_project,
                Curvz::CommandHistory* history,
                ObjectsScriptable::NotifyDocChanged notify_doc_changed,
                ObjectsScriptable::AnimateHandle animate_handle,
                std::string iid)
        : Scriptable(unregistered)
        , m_get_project(std::move(get_project))
        , m_history(history)
        , m_notify_doc_changed(std::move(notify_doc_changed))
        , m_animate_handle(std::move(animate_handle))
        , m_iid(std::move(iid)) {}

    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

private:
    // Resolve our iid to a live SceneNode through the current project,
    // filtered to is_scene_object. Returns nullptr if the iid no
    // longer addresses a scene-object-typed node (deleted, somehow
    // re-typed to a Layer / Guide / special-layer type).
    Curvz::SceneNode* resolve() const {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return nullptr;
        auto* n = curvz::utils::find_by_iid(*proj, m_iid);
        return is_scene_object(n) ? n : nullptr;
    }

    // s235 m1 — canvas-refresh callback trigger. Called at the end of
    // every successful mutating verb's body, AFTER the command push.
    // No-op when the callback is empty (graceful degradation, same
    // shape the history pointer uses for null-history degradation).
    void notify_doc_changed() {
        if (m_notify_doc_changed) m_notify_doc_changed();
    }

    // Push an EditObjectFieldCommand with our iid. The DIRECT mutation
    // must happen at the caller site (mirrors the LayersPanel /
    // LayerProxy pattern: mutate the SceneNode field, then push the
    // command with before/after). Skips silently if history isn't
    // wired or proj is missing — same fallback shape as LayerProxy.
    // (s232 m3 — direct mirror of LayerProxy's helpers of the same
    // names. EditObjectFieldCommand is the analog of
    // EditLayerFieldCommand minus the Color field.)
    void push_bool_field(Curvz::EditObjectFieldCommand::Field field,
                         bool before, bool after) {
        if (!m_history) return;
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return;
        if (m_iid.empty()) return;
        auto cmd = std::make_unique<Curvz::EditObjectFieldCommand>(
            proj, m_iid, field,
            /*before_str=*/std::string{}, /*after_str=*/std::string{},
            before, after);
        m_history->push(std::move(cmd));
    }
    void push_string_field(Curvz::EditObjectFieldCommand::Field field,
                           const std::string& before,
                           const std::string& after) {
        if (!m_history) return;
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return;
        if (m_iid.empty()) return;
        auto cmd = std::make_unique<Curvz::EditObjectFieldCommand>(
            proj, m_iid, field, before, after,
            /*before_bool=*/false, /*after_bool=*/false);
        m_history->push(std::move(cmd));
    }

    ObjectsScriptable::ProjectGetter    m_get_project;
    Curvz::CommandHistory*              m_history;   // wired in m3 — pushes
                                                     // EditObjectFieldCommand
                                                     // for the five element
                                                     // mutating verbs
    ObjectsScriptable::NotifyDocChanged m_notify_doc_changed;  // s235 m1 —
                                                     // canvas-refresh callback,
                                                     // invoked at the end of
                                                     // every successful
                                                     // mutating verb. May be
                                                     // empty; refresh step
                                                     // silently skips.
    ObjectsScriptable::AnimateHandle    m_animate_handle;  // s288 m1 —
                                                     // welcome-demo animation
                                                     // callback. Routes the
                                                     // animate_handle verb to
                                                     // Canvas::animate_handle
                                                     // via MainWindow's
                                                     // lambda. May be empty;
                                                     // verb is a silent
                                                     // no-op in that case.
    std::string                         m_iid;
};

// ── ObjectProxy: invoke ──────────────────────────────────────────────────
//
// s232 m3 — five element mutating verbs. Direct mirror of
// LayerProxy::invoke's shape, swapping EditLayerFieldCommand for
// EditObjectFieldCommand. Direct mutation precedes the command push;
// no-op (no command) when the new value matches the old. `rename`
// also no-ops on empty-string args — matches LayerProxy::rename's
// "empty entry leaves the name alone" UX convention.

ScriptValue ObjectProxy::invoke(std::string_view verb,
                                const ScriptArgs& args) {
    using Field = Curvz::EditObjectFieldCommand::Field;
    auto* node = resolve();
    if (!node) return ScriptValue::null();

    if (verb == "toggle_visible") {
        bool before = node->visible;
        bool after  = !before;
        node->visible = after;
        push_bool_field(Field::Visible, before, after);
        notify_doc_changed();
        return ScriptValue::null();
    }

    if (verb == "set_visible") {
        if (args.empty()) return ScriptValue::null();
        bool before = node->visible;
        bool after  = arg_as_bool(args[0], before);
        if (before == after) return ScriptValue::null();  // no-op, no command
        node->visible = after;
        push_bool_field(Field::Visible, before, after);
        notify_doc_changed();
        return ScriptValue::null();
    }

    if (verb == "toggle_locked") {
        bool before = node->locked;
        bool after  = !before;
        node->locked = after;
        push_bool_field(Field::Locked, before, after);
        notify_doc_changed();
        return ScriptValue::null();
    }

    if (verb == "set_locked") {
        if (args.empty()) return ScriptValue::null();
        bool before = node->locked;
        bool after  = arg_as_bool(args[0], before);
        if (before == after) return ScriptValue::null();
        node->locked = after;
        push_bool_field(Field::Locked, before, after);
        notify_doc_changed();
        return ScriptValue::null();
    }

    if (verb == "rename") {
        if (args.empty()) return ScriptValue::null();
        std::string before = node->name;
        std::string after  = arg_as_string(args[0]);
        if (after.empty() || before == after) return ScriptValue::null();
        node->name = after;
        push_string_field(Field::Name, before, after);
        notify_doc_changed();
        return ScriptValue::null();
    }

    // ── set_path_data "<svg_d_string>" (s236 m1) ─────────────────────────
    //
    // Replace the Path's PathData with the parsed result of an SVG
    // `d`-attribute string. The verb takes a single string arg; the
    // empty string IS allowed and resolves to an empty PathData (a
    // clearing primitive without deleting the node — matches how a
    // freshly-minted Path starts out).
    //
    // Rides EditPathCommand UNMODIFIED — its CurvzProject* + obj_iid
    // + before/after PathData shape (added in the s167 m1 iid
    // migration) is exactly what whole-path-replace needs. No
    // parallel command class. First-task verification confirmed
    // the fit before writing the verb.
    //
    // No-op returns (no command pushed, no callback fired):
    //   - args.empty() — no string supplied.
    //   - args[0] is not a string-shaped ScriptValue.
    //   - node->path == nullptr — non-Path types (Group / Compound /
    //     Image / Ref / Measurement / Blend / Warp / Text). Silent
    //     refusal, no error sentinel through ScriptValue (future
    //     structured-error-channel concern; see s235 backlog).
    //
    // The iid index is NOT invalidated here — path-data changes
    // don't reshape iid space. EditPathCommand::execute does its own
    // invalidate-before-resolve (s168 m6 defensive pattern), which
    // covers redo/undo paths.
    if (verb == "set_path_data") {
        if (args.empty()) return ScriptValue::null();
        std::string d_str = arg_as_string(args[0]);
        // Note: empty d_str is intentionally allowed below — it
        // produces an empty PathData. We don't early-return on the
        // empty-arg case the way `rename` does, because clearing
        // path data is a meaningful operation distinct from "do
        // nothing." Only the wrong-shape case (args[0] isn't a
        // string) is handled by arg_as_string returning ""; if a
        // user genuinely passed empty quotes we honour it.

        if (!node->path) return ScriptValue::null();  // non-Path

        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::null();

        // Capture before BEFORE the parse — the parse is the
        // expensive step; if it produced something we want to
        // commit, the before-snapshot is already in hand.
        Curvz::PathData before = *node->path;
        Curvz::PathData after  = curvz::utils::svg_d_to_path_data(d_str);

        // s237 m1 — flip the parsed user-space PathData to doc-space
        // before mutating the model. The d-string the user typed is
        // in user-space (Y-up, bottom-left origin); the engine stores
        // in doc-space (Y-down, top-left origin). Look up the owning
        // doc to get canvas_height — the per-doc constant that
        // anchors the flip. find_doc_by_iid walks every doc in the
        // project; in the normal path it returns the same doc
        // find_by_iid (which has already resolved `node`) saw.
        // Empty PathData round-trips through the flip as a no-op
        // (no nodes to walk), preserving the clearing-primitive
        // semantics of set_path_data "".
        if (auto* owner = curvz::utils::find_doc_by_iid(*proj, m_iid)) {
            double ch = (double)owner->canvas_height();
            curvz::utils::path_data_user_to_doc(after, ch);
        }
        // (Defensive: if find_doc_by_iid returns nullptr — should not
        // happen since find_by_iid just resolved `node` in resolve()
        // above — the path lands unflipped. That's the s236 m1 visual
        // regression rather than a crash; the visual channel surfaces
        // it the same way it surfaced the original miss.)

        // Direct mutation precedes the push (mirrors every existing
        // EditPathCommand call site in Canvas_ops.cpp / Canvas_input.cpp
        // — user perspective is "the verb did the thing", Ctrl+Z then
        // undoes it). The PathData stored is in doc-space (post-flip),
        // matching the convention the canvas-tool sites have always
        // produced; EditPathCommand and the renderer have no awareness
        // of user-space.
        *node->path = after;

        if (m_history) {
            auto cmd = std::make_unique<Curvz::EditPathCommand>(
                proj, m_iid,
                std::move(before), std::move(after),
                "Set path data (script)");
            m_history->push(std::move(cmd));
        }
        notify_doc_changed();
        return ScriptValue::null();
    }

    // ── animate_handle <node_index> <which> <x> <y> <ms> (s288 m1) ───────
    //
    // OPENS the welcome-demo animation arc. Args (all positional):
    //   0  node_index : int     — index into node->path->nodes
    //   1  which      : string  — "in" or "out"
    //   2  target_x   : double  — user-space X of the final handle pos
    //   3  target_y   : double  — user-space Y of the final handle pos
    //   4  duration_ms: double  — animation duration in milliseconds
    //
    // The verb resolves the target node, reads the current handle
    // position (the animation's `start`), flips the target to doc-
    // space, and hands the gesture off to the m_animate_handle
    // callback. MainWindow wires the callback to Canvas::animate_handle
    // which owns the actual loop — entering the mouse-drag idiom at
    // begin, writing per-frame through BezierPath::move_handle_*,
    // leaving the idiom at end. The renderer sees identical state to
    // a real mouse drag and animates the live handle line frame by
    // frame.
    //
    // Why the verb doesn't do the loop directly: the renderer's Node-
    // tool overlay branch only draws anchors+handles when m_selected
    // == <the path>. A script-minted path is never auto-selected, so
    // direct PathData mutation per tick from this verb produces no
    // visible result. The fix is to use the same path a real drag
    // uses — and the right home for that is Canvas, which owns the
    // drag state.
    //
    // No-op early returns (verb dispatches OK but does nothing):
    //   - args.size() < 5
    //   - which is not "in" or "out"
    //   - node->path == nullptr (non-Path types)
    //   - node_index out of range
    //   - m_animate_handle callback is empty (MainWindow didn't wire)
    //
    // Non-positive duration_ms passes through to Canvas::animate_handle
    // which treats it as a snap (write end immediately, no timeout).
    if (verb == "animate_handle") {
        if (args.size() < 5) return ScriptValue::null();
        if (!node->path) return ScriptValue::null();
        if (!m_animate_handle) return ScriptValue::null();

        int  node_idx    = (int)arg_as_double(args[0], -1.0);
        auto which       = arg_as_string(args[1]);
        double tx_user   = arg_as_double(args[2], 0.0);
        double ty_user   = arg_as_double(args[3], 0.0);
        double duration  = arg_as_double(args[4], 0.0);

        if (which != "in" && which != "out") return ScriptValue::null();
        if (node_idx < 0
            || node_idx >= (int)node->path->nodes.size()) {
            return ScriptValue::null();
        }

        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::null();

        // Read the start position from the live node, in doc-space
        // (that's how the model stores). The handle in question is
        // either cx1/cy1 (in-handle) or cx2/cy2 (out-handle).
        const auto& bn = node->path->nodes[node_idx];
        const bool is_in = (which == "in");
        const double sx_doc = is_in ? bn.cx1 : bn.cx2;
        const double sy_doc = is_in ? bn.cy1 : bn.cy2;

        // User-space → doc-space Y-flip for the target. Mirrors the
        // s237 m1 convention set_path_data adopted. find_doc_by_iid
        // walks every doc to locate the owner and read canvas_height.
        double tx_doc = tx_user;
        double ty_doc = ty_user;
        if (auto* owner = curvz::utils::find_doc_by_iid(*proj, m_iid)) {
            double ch = (double)owner->canvas_height();
            ty_doc = ch - ty_user;
        }

        // Hand off. The callback owns everything from here — Canvas
        // re-resolves the SceneNode via the iid (the proxy is
        // transient and may not survive the timeout loop), enters
        // drag state, runs the timeout, leaves drag state.
        m_animate_handle(m_iid, node_idx, is_in,
                         sx_doc, sy_doc, tx_doc, ty_doc, duration);
        return ScriptValue::null();
    }

    // ── set_fill "<color_token>" (s238 m1) ───────────────────────────────
    //
    // First appearance-bearing verb on ObjectProxy. Replaces the
    // SceneNode's `fill` FillStyle with the value parsed from the
    // single string arg. Rides EditAppearanceCommand unmodified —
    // its 14-arg constructor (proj + obj_iid + whole-FillStyle and
    // whole-StrokeStyle before/after + swatch-id snapshots +
    // bound_style snapshots) is the exact iid-shaped capture this
    // verb needs.
    //
    // Vocabulary (see fill_attr_to_fill_style in curvz_utils):
    //   "none" / "currentColor" / "#RRGGBB" / "#RGB" / "rgb(...)" /
    //   "rgba(...)" / named (19-name common subset, case-insensitive).
    //   Unknown tokens degrade to CurrentColor (matches SvgParser's
    //   safe-default for unrecognised paint values).
    //
    // No node-type refusal: appearance lives on every SceneNode
    // subtype, unlike `set_path_data` which refused non-Paths via
    // node->path == nullptr. Setting fill on a Group / Compound has
    // a documented effect via style inheritance even though the
    // container itself doesn't draw a filled region directly. Same
    // shape the inspector's fill controls have always taken — they
    // don't grey out for non-Path selections.
    //
    // Direct-override semantics on swatch / style bindings: a direct
    // fill set BREAKS the fill_swatch_id binding and the bound_style
    // binding (clearing both fields on the SceneNode at execute time).
    // The same break SvgParser.cpp's style::mutate_appearance does on
    // the inspector / eyedropper / broadcast paths — see S82 m4f and
    // S92 m3 narratives on EditAppearanceCommand for the rationale.
    // Without this break, "I set my object's fill to red, undo to
    // green, then edit the swatch" produces a colour-on-bound-swatch
    // surprise. The capture in EditAppearanceCommand restores the
    // pre-edit bindings on undo, so the break is undoable.
    //
    // Stroke is NOT touched: every stroke_after field equals its
    // stroke_before counterpart, so undo round-trips stroke through
    // a self-assignment (effectively a no-op on stroke). The 14-arg
    // constructor covers fill_swatch_id and bound_style as separate
    // before/after pairs; we pass swatch_id_after = "" and
    // bound_style_after = "" to capture the break.
    //
    // No-op returns (no command pushed, no callback fired):
    //   - args.empty() — no token supplied.
    //   - args[0] is not a string-shaped ScriptValue.
    //   - args[0] is the empty string after arg_as_string — unlike
    //     set_path_data, an empty fill token has no defensible
    //     vocabulary slot and we treat it as accidental.
    //   - parsed fill_new equals current node->fill on the field
    //     surface fill_attr_to_fill_style produces (see
    //     fill_style_equals_byte_rounded in anon namespace).
    //
    // The iid index is NOT invalidated here — appearance changes
    // don't reshape iid space. EditAppearanceCommand::execute does
    // its own invalidate-before-resolve defensively (s168 m6
    // pattern), which covers redo/undo paths.
    if (verb == "set_fill") {
        if (args.empty()) return ScriptValue::null();
        std::string tok = arg_as_string(args[0]);
        if (tok.empty()) return ScriptValue::null();

        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::null();

        Curvz::FillStyle fill_new = curvz::utils::fill_attr_to_fill_style(tok);
        Curvz::FillStyle fill_before = node->fill;

        if (fill_style_equals_byte_rounded(fill_new, fill_before)) {
            // Same colour as the current state — no command, no
            // callback. Matches the value-unchanged early-return
            // shape set_visible / set_locked / rename use.
            return ScriptValue::null();
        }

        // Pre-edit snapshot of every field EditAppearanceCommand
        // restores on undo. Stroke is captured before (and after,
        // unchanged) so the round-trip is field-symmetric — the
        // command body writes every after-value, and the undo body
        // writes every before-value, regardless of which the verb
        // actually meant to change.
        Curvz::StrokeStyle stroke_before     = node->stroke;
        std::string        fsib              = node->fill_swatch_id;
        std::string        ssib              = node->stroke_swatch_id;
        std::string        bsb               = node->bound_style;

        // Direct mutation precedes the push. Same shape as
        // set_path_data and the canvas tools — the user perspective
        // is "the verb did the thing"; Ctrl+Z then undoes it.
        node->fill           = fill_new;
        node->fill_swatch_id = "";       // break the swatch binding
        node->bound_style    = "";       // break the style binding

        if (m_history) {
            auto cmd = std::make_unique<Curvz::EditAppearanceCommand>(
                proj, m_iid,
                /*fb=*/std::move(fill_before),
                /*sb=*/stroke_before,
                /*fa=*/fill_new,
                /*sa=*/stroke_before,        // stroke unchanged
                /*fsib=*/std::move(fsib),
                /*ssib=*/ssib,
                /*fsia=*/std::string{},      // swatch binding cleared
                /*ssia=*/ssib,                // stroke swatch unchanged
                /*bsb=*/std::move(bsb),
                /*bsa=*/std::string{},       // style binding cleared
                "Set fill (script)");
            m_history->push(std::move(cmd));
        }
        notify_doc_changed();
        return ScriptValue::null();
    }

    // ── set_stroke "<color_token>" (s239 m1) ─────────────────────────────
    //
    // Symmetric second half of the appearance branch. Mirror-shape
    // sibling of set_fill: same EditAppearanceCommand (the existing
    // s167-migrated command, FIFTH "fits unmodified" case in a row),
    // same fill_attr_to_fill_style pump (operates on stroke.paint —
    // a FillStyle — without modification), same direct-override
    // semantics (clears stroke_swatch_id and bound_style on the after
    // side; mirrors mutate_appearance's inspector behaviour), same
    // universal-across-SceneNode-subtypes posture, same no-op early-
    // return on value-unchanged via fill_style_equals_byte_rounded
    // (the anon helper formerly known as fill_style_equals_for_set_fill,
    // renamed neutrally this session).
    //
    // Scope: COLOUR only. The verb writes into stroke.paint and leaves
    // stroke.width / stroke.cap / stroke.join / stroke.miter /
    // stroke.opacity unchanged. Each of those five fields will be a
    // separate verb in a future milestone — set_stroke_width takes
    // a numeric token, set_stroke_cap / set_stroke_join take enum
    // tokens. Bundling them into set_stroke would blur the "what
    // does this verb do" contract; the numeric/enum verbs are
    // scope-limited out of this milestone by design.
    //
    // Visual scope-limit: fresh-mint Paths have stroke.width == 0.0,
    // so set_stroke "black" lands a black paint on a zero-width
    // stroke — the canvas shows NOTHING new until the width verb
    // sets a nonzero width. This is by design (the smoke owns its
    // conditions, so it can't pre-set width via the inspector); the
    // visual outline closes in m2 with set_stroke_width. Trace
    // channel (the `stroke` query reading back the colour) is the
    // full validation surface for m1.
    //
    // Direct-override semantics: a direct stroke colour set BREAKS
    // the stroke_swatch_id binding and the bound_style binding,
    // same as set_fill does on the fill side. The fill bindings
    // (fill_swatch_id) are NOT touched — only the stroke-swatch
    // and the (whole-node) bound_style are cleared. Without the
    // bound_style break, "set_stroke black on a style-bound node,
    // undo, then edit the bound style" produces a colour-on-bound-
    // style surprise where the style edit overrides the script's
    // direct stroke. The capture restores the binding on undo, so
    // the break is undoable.
    //
    // Pre-edit snapshot: every field EditAppearanceCommand restores
    // on undo. fill_before is captured (and fill_after equals
    // fill_before, unchanged — the symmetric mirror of set_fill's
    // stroke_before/stroke_after invariant). The 14-arg constructor
    // takes fill_swatch_id_before/_after AND bound_style_before/
    // _after as the swatch/style binding capture — fill_swatch
    // unchanged on both sides; stroke_swatch cleared on after;
    // bound_style cleared on after.
    //
    // No-op returns (no command pushed, no callback fired):
    //   - args.empty().
    //   - args[0] is not a string-shaped ScriptValue.
    //   - args[0] is the empty string after arg_as_string.
    //   - parsed stroke_new.paint equals current node->stroke.paint
    //     via fill_style_equals_byte_rounded. Since set_stroke only
    //     touches the paint slot, paint-equal => StrokeStyle-equal
    //     (the other five fields are preserved from the current
    //     stroke), so paint-equality is the necessary AND sufficient
    //     no-op condition.
    //
    // The iid index is NOT invalidated (appearance changes don't
    // reshape iid space). EditAppearanceCommand::execute does its
    // own invalidate-before-resolve defensively (s168 m6 pattern).
    if (verb == "set_stroke") {
        if (args.empty()) return ScriptValue::null();
        std::string tok = arg_as_string(args[0]);
        if (tok.empty()) return ScriptValue::null();

        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::null();

        // Parse the token through the shared pump. stroke.paint is
        // a FillStyle — same vocabulary, same fallback (unknown
        // tokens degrade to CurrentColor).
        Curvz::FillStyle paint_new = curvz::utils::fill_attr_to_fill_style(tok);
        Curvz::FillStyle paint_before = node->stroke.paint;

        if (fill_style_equals_byte_rounded(paint_new, paint_before)) {
            // Same colour as the current stroke paint — no command,
            // no callback. width / cap / join / miter / opacity are
            // not touched by this verb, so paint-equality is also
            // StrokeStyle-equality from this verb's perspective.
            return ScriptValue::null();
        }

        // Pre-edit snapshot. Same shape as set_fill's snapshot, with
        // the axes swapped: fill side stays fixed, stroke side is
        // what changes.
        Curvz::FillStyle   fill_before = node->fill;
        Curvz::StrokeStyle stroke_before = node->stroke;
        std::string        fsib          = node->fill_swatch_id;
        std::string        ssib          = node->stroke_swatch_id;
        std::string        bsb           = node->bound_style;

        // Build stroke_new from stroke_before with only paint
        // overwritten. The other five fields self-preserve through
        // the command (stroke_after == stroke_new == stroke_before
        // except for paint).
        Curvz::StrokeStyle stroke_new = stroke_before;
        stroke_new.paint = paint_new;

        // Direct mutation precedes the push. Same shape as set_fill.
        node->stroke           = stroke_new;
        node->stroke_swatch_id = "";   // break the stroke swatch binding
        node->bound_style      = "";   // break the style binding

        if (m_history) {
            auto cmd = std::make_unique<Curvz::EditAppearanceCommand>(
                proj, m_iid,
                /*fb=*/fill_before,
                /*sb=*/std::move(stroke_before),
                /*fa=*/fill_before,              // fill unchanged
                /*sa=*/stroke_new,
                /*fsib=*/fsib,
                /*ssib=*/std::move(ssib),
                /*fsia=*/fsib,                   // fill swatch unchanged
                /*ssia=*/std::string{},          // stroke swatch cleared
                /*bsb=*/std::move(bsb),
                /*bsa=*/std::string{},           // style binding cleared
                "Set stroke (script)");
            m_history->push(std::move(cmd));
        }
        notify_doc_changed();
        return ScriptValue::null();
    }

    // ── set_stroke_width <number> (s239 m2) ──────────────────────────────
    //
    // Closes the appearance branch's visual loop. m1 (set_stroke) gave
    // a Path's outline a colour; m2 gives it a thickness. Without the
    // thickness, m1's colour landed on a zero-width stroke (the
    // SceneNode default) and the canvas painted no outline regardless
    // of the colour — set_stroke "blue" was trace-channel-only. With
    // m2 in place, `set_stroke "blue"; set_stroke_width 2.0` produces
    // a visible blue outline. The arc's geometry+fill+stroke triad is
    // complete.
    //
    // First numeric-arg verb on ObjectProxy. Earlier verbs took string
    // tokens (path data, colour tokens, direction tokens, type tokens)
    // or no args at all (toggle_visible, duplicate). The DSL emits
    // bare integer literals as ScriptValue::Int and decimal literals as
    // ScriptValue::Double; arg_as_double accepts both (Int coerced via
    // static_cast<double>), so `set_stroke_width 2` and `set_stroke_width
    // 2.0` both reach the same value at this seam. ScriptValue's
    // strict-equality contract on the assert side means RHS literals
    // for stroke_width queries must be Double-shaped (e.g. `== 2.0`,
    // not `== 2`).
    //
    // Rides EditAppearanceCommand unmodified (SIXTH "fits unmodified"
    // case for verify-before-assume in a row). The command's
    // StrokeStyle slots already capture every stroke field including
    // width; the verb writes a stroke_after with only `width`
    // overwritten relative to stroke_before. Same architectural shape
    // as set_stroke's paint-only overwrite, just a different scalar
    // slot on the StrokeStyle struct.
    //
    // No clamp, no negative-refusal, no zero-refusal. Matches
    // StylesScriptable::stroke_width's shipped convention: the model
    // accepts whatever the script supplies; the renderer handles
    // edge cases. Zero width = no outline rendered (which is exactly
    // the m1 default state, so set_stroke_width 0.0 is the explicit
    // "remove stroke thickness" gesture). Negative widths zero out
    // at the Cairo seam (renderer treats negative as no-stroke).
    //
    // Direct-override on bindings: clears bound_style (a direct width
    // edit breaks the style binding, mirroring set_fill / set_stroke /
    // inspector). Does NOT clear stroke_swatch_id or fill_swatch_id —
    // swatch bindings represent PAINT references; width edits don't
    // touch paint. Same partial-clear convention set_fill (m1, fill-
    // swatch only) and set_stroke (m1 of this session, stroke-swatch
    // only) established. The inspector's mutate_appearance funnel goes
    // further and clears BOTH swatch bindings on any appearance edit;
    // whether to align script-side to that broader convention is a
    // backlog item.
    //
    // No-op returns (no command pushed, no callback fired):
    //   - args.empty().
    //   - arg_as_double falls through to the fallback (passed as
    //     current node->stroke.width). For non-numeric / non-bool
    //     ScriptValues this yields width_new == width_before, which
    //     the equality check immediately catches.
    //   - parsed width_new equals current node->stroke.width on
    //     direct double-equal compare. The values that flow through
    //     this verb come from arg_as_double without any pump-stage
    //     parsing, so float-precision drift between successive
    //     identical calls is not a concern — same value typed twice
    //     produces byte-equal doubles.
    if (verb == "set_stroke_width") {
        if (args.empty()) return ScriptValue::null();

        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::null();

        double width_before = node->stroke.width;
        double width_new    = arg_as_double(args[0], width_before);
        if (width_new == width_before) {
            // Same width as the current state — also catches the
            // non-numeric-arg case (arg_as_double returns the
            // fallback, which is width_before).
            return ScriptValue::null();
        }

        // Pre-edit snapshot. Same shape as set_stroke's snapshot:
        // both axes captured whole, plus all three binding fields.
        Curvz::FillStyle   fill_before   = node->fill;
        Curvz::StrokeStyle stroke_before = node->stroke;
        std::string        fsib          = node->fill_swatch_id;
        std::string        ssib          = node->stroke_swatch_id;
        std::string        bsb           = node->bound_style;

        // Build stroke_new from stroke_before with only width
        // overwritten. paint / cap / join / miter / opacity all
        // self-preserve through the command.
        Curvz::StrokeStyle stroke_new = stroke_before;
        stroke_new.width = width_new;

        // Direct mutation precedes the push. Same shape as set_stroke
        // and set_fill. Note: fill_swatch_id and stroke_swatch_id are
        // NOT cleared — see narrative above (width edits don't touch
        // paint, so swatch bindings stay intact).
        node->stroke      = stroke_new;
        node->bound_style = "";   // break the style binding

        if (m_history) {
            auto cmd = std::make_unique<Curvz::EditAppearanceCommand>(
                proj, m_iid,
                /*fb=*/fill_before,
                /*sb=*/std::move(stroke_before),
                /*fa=*/fill_before,              // fill unchanged
                /*sa=*/stroke_new,
                /*fsib=*/fsib,
                /*ssib=*/ssib,
                /*fsia=*/fsib,                   // fill swatch unchanged
                /*ssia=*/ssib,                   // stroke swatch unchanged
                /*bsb=*/std::move(bsb),
                /*bsa=*/std::string{},           // style binding cleared
                "Set stroke width (script)");
            m_history->push(std::move(cmd));
        }
        notify_doc_changed();
        return ScriptValue::null();
    }

    // ── set_stroke_cap "<token>" (s240 m1) ───────────────────────────────
    //
    // Closes the appearance branch's enum-token sub-axis (paired with
    // set_stroke_join below). First enum-token verb on ObjectProxy;
    // earlier verbs took strings (set_path_data, rename), colour
    // tokens via fill_attr_to_fill_style (set_fill, set_stroke),
    // or numbers via arg_as_double (set_stroke_width). The
    // vocabulary here is the LineCap enum's three values surfaced
    // as the lowercase strings "butt" / "round" / "square" —
    // matches SVG's stroke-linecap attribute conventions exactly.
    //
    // Same EditAppearanceCommand ride as set_fill / set_stroke /
    // set_stroke_width — SEVENTH "fits unmodified" case in a row
    // for verify-before-assume (fourth in two sessions on
    // EditAppearanceCommand). The command's whole-StrokeStyle
    // capture covers cap and join along with paint / width /
    // miter / opacity without modification. Both `stroke_before`
    // and `stroke_after` are whole structs; cap is a field on
    // StrokeStyle (SceneNode.hpp:76), so the existing capture
    // shape covers a cap-only edit by construction.
    //
    // Architectural shape (parallels set_stroke_width):
    //   1. args.empty() → no-op return.
    //   2. Project lookup → no-op on miss.
    //   3. Parse arg via decode_cap (anon-namespace helper added
    //      this session, lifted from StylesScriptable). Returns
    //      std::optional<Curvz::LineCap>; nullopt on unknown
    //      token. UNLIKE set_fill / set_stroke, an unknown token
    //      is NOT degraded to a safe default — it's a silent
    //      no-op. Cap/join have no documented safe-default the
    //      way colour has CurrentColor; matching
    //      StylesScriptable::stroke_cap's posture is the right
    //      read.
    //   4. Same-value early return on `*parsed == cap_before`.
    //   5. Pre-edit snapshot (same shape as set_stroke_width).
    //   6. Build stroke_new from stroke_before with only `cap`
    //      overwritten. paint / width / join / miter / opacity
    //      self-preserve.
    //   7. Direct mutation precedes the push.
    //   8. Push EditAppearanceCommand with fill_before echoed
    //      into both fb/fa slots, stroke_before/stroke_new on
    //      the stroke axis, fsib/ssib echoed into fsia/ssia
    //      (swatch bindings preserved — see binding-clear note
    //      below).
    //
    // Partial binding-clear convention. set_stroke_cap clears
    // bound_style (a cap edit is a direct appearance edit that
    // breaks the style binding, mirroring set_fill / set_stroke /
    // set_stroke_width / inspector). Does NOT clear
    // fill_swatch_id or stroke_swatch_id (swatch bindings
    // represent PAINT references; cap edits don't touch paint,
    // so neither swatch binding is disturbed). Matches the
    // partial-clear convention set_stroke_width established —
    // width and cap both live on StrokeStyle but neither
    // touches StrokeStyle::paint, so neither clears stroke_swatch_id.
    //
    // Universal across SceneNode subtypes. Appearance lives on
    // every node; cap is a field on StrokeStyle which every
    // SceneNode has. Group / Compound / etc. accept the verb;
    // visual effect depends on the container's render path.
    if (verb == "set_stroke_cap") {
        if (args.empty()) return ScriptValue::null();

        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::null();

        auto parsed = decode_cap(arg_as_string(args[0]));
        if (!parsed) {
            // Unknown vocab — silent no-op. Matches
            // StylesScriptable::stroke_cap. NOT a degrade to a
            // safe default (no such default exists for cap).
            return ScriptValue::null();
        }
        Curvz::LineCap cap_before = node->stroke.cap;
        if (*parsed == cap_before) {
            // Same cap as the current state — no command, no
            // notify. Same-value early-return.
            return ScriptValue::null();
        }

        // Pre-edit snapshot. Same shape as set_stroke_width's
        // snapshot: both axes captured whole, plus all three
        // binding fields.
        Curvz::FillStyle   fill_before   = node->fill;
        Curvz::StrokeStyle stroke_before = node->stroke;
        std::string        fsib          = node->fill_swatch_id;
        std::string        ssib          = node->stroke_swatch_id;
        std::string        bsb           = node->bound_style;

        // Build stroke_new from stroke_before with only cap
        // overwritten. paint / width / join / miter / opacity
        // all self-preserve through the command.
        Curvz::StrokeStyle stroke_new = stroke_before;
        stroke_new.cap = *parsed;

        // Direct mutation precedes the push. Same shape as
        // set_stroke_width. Swatch ids preserved (cap edits
        // don't touch paint).
        node->stroke      = stroke_new;
        node->bound_style = "";   // break the style binding

        if (m_history) {
            auto cmd = std::make_unique<Curvz::EditAppearanceCommand>(
                proj, m_iid,
                /*fb=*/fill_before,
                /*sb=*/std::move(stroke_before),
                /*fa=*/fill_before,              // fill unchanged
                /*sa=*/stroke_new,
                /*fsib=*/fsib,
                /*ssib=*/ssib,
                /*fsia=*/fsib,                   // fill swatch unchanged
                /*ssia=*/ssib,                   // stroke swatch unchanged
                /*bsb=*/std::move(bsb),
                /*bsa=*/std::string{},           // style binding cleared
                "Set stroke cap (script)");
            m_history->push(std::move(cmd));
        }
        notify_doc_changed();
        return ScriptValue::null();
    }

    // ── set_stroke_join "<token>" (s240 m1) ──────────────────────────────
    //
    // Symmetric paired verb to set_stroke_cap. Vocabulary is the
    // LineJoin enum's three values as lowercase strings:
    // "miter" / "round" / "bevel". Same SVG conventions; same
    // anon-namespace decode helper shape (decode_join);
    // EIGHTH "fits unmodified" case in a row for verify-before-
    // assume.
    //
    // Architectural shape, partial binding-clear convention,
    // unknown-token policy, and universality posture all
    // identical to set_stroke_cap — see the narrative there. The
    // only difference is the field touched (StrokeStyle::join
    // vs StrokeStyle::cap) and the enum vocabulary.
    //
    // Visual effect: with a stroke wide enough to read (40px+
    // from set_stroke_width's vocabulary) and a polygon with
    // visible corners (rect / star / etc.), join = "round"
    // bevels the corners into smooth arcs; join = "bevel" cuts
    // them as straight diagonals; join = "miter" extends the
    // edges to meet at the corner point (the default).
    // Immediately observable on canvas without further work.
    if (verb == "set_stroke_join") {
        if (args.empty()) return ScriptValue::null();

        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::null();

        auto parsed = decode_join(arg_as_string(args[0]));
        if (!parsed) {
            // Unknown vocab — silent no-op. Same posture as
            // set_stroke_cap.
            return ScriptValue::null();
        }
        Curvz::LineJoin join_before = node->stroke.join;
        if (*parsed == join_before) {
            return ScriptValue::null();
        }

        // Pre-edit snapshot. Same shape as set_stroke_cap.
        Curvz::FillStyle   fill_before   = node->fill;
        Curvz::StrokeStyle stroke_before = node->stroke;
        std::string        fsib          = node->fill_swatch_id;
        std::string        ssib          = node->stroke_swatch_id;
        std::string        bsb           = node->bound_style;

        // Build stroke_new from stroke_before with only join
        // overwritten. paint / width / cap / miter / opacity
        // all self-preserve.
        Curvz::StrokeStyle stroke_new = stroke_before;
        stroke_new.join = *parsed;

        node->stroke      = stroke_new;
        node->bound_style = "";   // break the style binding

        if (m_history) {
            auto cmd = std::make_unique<Curvz::EditAppearanceCommand>(
                proj, m_iid,
                /*fb=*/fill_before,
                /*sb=*/std::move(stroke_before),
                /*fa=*/fill_before,              // fill unchanged
                /*sa=*/stroke_new,
                /*fsib=*/fsib,
                /*ssib=*/ssib,
                /*fsia=*/fsib,                   // fill swatch unchanged
                /*ssia=*/ssib,                   // stroke swatch unchanged
                /*bsb=*/std::move(bsb),
                /*bsa=*/std::string{},           // style binding cleared
                "Set stroke join (script)");
            m_history->push(std::move(cmd));
        }
        notify_doc_changed();
        return ScriptValue::null();
    }

    // ── set_stroke_miter <number> (s241 m1) ──────────────────────────────
    //
    // Numeric scalar verb on stroke's miter-limit field. Reuses
    // arg_as_double from s239 m2's anon-namespace helper — no new
    // helper this milestone, no new pump pair, no new command.
    // NINTH "fits unmodified" application of verify-before-assume
    // (EditAppearanceCommand's whole-StrokeStyle capture covers
    // miter along with everything else).
    //
    // NAMING NOTE: the SceneNode field is `stroke.miter`
    // (SceneNode.hpp:78); the style::Style field is
    // `stroke.miter_limit` (style/Style.hpp:142). Both default to
    // 4.0. This is a latent inconsistency in the codebase
    // (different field names for the same concept on two
    // closely-related structs); ObjectsScriptable surfaces the
    // verb as `set_stroke_miter` to match the SceneNode field —
    // shorter, and the verb operates on SceneNode::stroke
    // directly, not on style::Style::stroke. StylesScriptable
    // (s225 m1) surfaces its setter as `stroke_miter_limit`
    // because it operates on style::Style. Whether to align the
    // field names across the two structs is a backlog item.
    //
    // Material similarity to set_stroke_width (s239 m2):
    // identical shape modulo the field touched. Snapshot pattern,
    // arg_as_double coercion (Int → Double via static_cast),
    // same-value early-return, partial binding-clear (clear
    // bound_style only; preserve both swatch ids — miter edits
    // don't touch paint), no clamp / no negative-refusal. Group
    // accepts the verb (universal across SceneNode subtypes).
    //
    // VISUAL EFFECT: miter only matters when join == "miter"
    // AND the corner is acute enough that extending the edges
    // to a point would exceed miter * stroke_width. At that
    // threshold the corner is "miter-clipped" — a flat bevel
    // replaces the sharp point. So set_stroke_miter is the
    // dial that decides "how sharp before we bevel." On a
    // rect (90° corners) the threshold rarely triggers; on a
    // star or hairpin polygon it's immediately observable.
    if (verb == "set_stroke_miter") {
        if (args.empty()) return ScriptValue::null();

        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::null();

        double miter_before = node->stroke.miter;
        double miter_new    = arg_as_double(args[0], miter_before);
        if (miter_new == miter_before) {
            // Same miter as current state — also catches the
            // non-numeric-arg case via arg_as_double's fallback.
            return ScriptValue::null();
        }

        // Pre-edit snapshot. Same shape as set_stroke_width /
        // set_stroke_cap / set_stroke_join.
        Curvz::FillStyle   fill_before   = node->fill;
        Curvz::StrokeStyle stroke_before = node->stroke;
        std::string        fsib          = node->fill_swatch_id;
        std::string        ssib          = node->stroke_swatch_id;
        std::string        bsb           = node->bound_style;

        // Build stroke_new with only miter overwritten. paint /
        // width / cap / join / opacity self-preserve.
        Curvz::StrokeStyle stroke_new = stroke_before;
        stroke_new.miter = miter_new;

        node->stroke      = stroke_new;
        node->bound_style = "";   // break the style binding

        if (m_history) {
            auto cmd = std::make_unique<Curvz::EditAppearanceCommand>(
                proj, m_iid,
                /*fb=*/fill_before,
                /*sb=*/std::move(stroke_before),
                /*fa=*/fill_before,              // fill unchanged
                /*sa=*/stroke_new,
                /*fsib=*/fsib,
                /*ssib=*/ssib,
                /*fsia=*/fsib,                   // fill swatch unchanged
                /*ssia=*/ssib,                   // stroke swatch unchanged
                /*bsb=*/std::move(bsb),
                /*bsa=*/std::string{},           // style binding cleared
                "Set stroke miter (script)");
            m_history->push(std::move(cmd));
        }
        notify_doc_changed();
        return ScriptValue::null();
    }

    // ── set_stroke_opacity <number> (s241 m1) ────────────────────────────
    //
    // Numeric scalar verb on stroke's opacity field. Same shape
    // as set_stroke_miter modulo the field touched (opacity vs
    // miter). TENTH "fits unmodified" application of verify-
    // before-assume.
    //
    // StylesScriptable does NOT surface a stroke_opacity verb
    // (it surfaces shadow_opacity for the drop-shadow's alpha,
    // but stroke opacity stays at the FillStyle/Solid alpha or
    // the path-level opacity multiplier — different mechanism
    // at the style surface). ObjectsScriptable adds the direct
    // SceneNode::stroke.opacity dial here without a styles-side
    // counterpart. New verb name (`set_stroke_opacity`),
    // matching the field name directly.
    //
    // VISUAL EFFECT: stroke.opacity is a 0.0-1.0 alpha multiplier
    // applied to the resolved stroke paint at the Cairo seam.
    // 1.0 = fully opaque (default), 0.0 = invisible (no outline
    // rendered), 0.5 = half-transparent (the fill colour shows
    // through the outline; visible on a coloured fill).
    //
    // No clamp / no refusal. Matches set_stroke_width's
    // convention: the model accepts whatever the script supplies;
    // the renderer handles edge cases. Negative opacity zeros
    // out at Cairo's clamp; values > 1.0 saturate at 1.0.
    if (verb == "set_stroke_opacity") {
        if (args.empty()) return ScriptValue::null();

        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::null();

        double opacity_before = node->stroke.opacity;
        double opacity_new    = arg_as_double(args[0], opacity_before);
        if (opacity_new == opacity_before) {
            return ScriptValue::null();
        }

        // Pre-edit snapshot. Same shape as set_stroke_miter.
        Curvz::FillStyle   fill_before   = node->fill;
        Curvz::StrokeStyle stroke_before = node->stroke;
        std::string        fsib          = node->fill_swatch_id;
        std::string        ssib          = node->stroke_swatch_id;
        std::string        bsb           = node->bound_style;

        // Build stroke_new with only opacity overwritten. paint /
        // width / cap / join / miter self-preserve.
        Curvz::StrokeStyle stroke_new = stroke_before;
        stroke_new.opacity = opacity_new;

        node->stroke      = stroke_new;
        node->bound_style = "";   // break the style binding

        if (m_history) {
            auto cmd = std::make_unique<Curvz::EditAppearanceCommand>(
                proj, m_iid,
                /*fb=*/fill_before,
                /*sb=*/std::move(stroke_before),
                /*fa=*/fill_before,              // fill unchanged
                /*sa=*/stroke_new,
                /*fsib=*/fsib,
                /*ssib=*/ssib,
                /*fsia=*/fsib,                   // fill swatch unchanged
                /*ssia=*/ssib,                   // stroke swatch unchanged
                /*bsb=*/std::move(bsb),
                /*bsa=*/std::string{},           // style binding cleared
                "Set stroke opacity (script)");
            m_history->push(std::move(cmd));
        }
        notify_doc_changed();
        return ScriptValue::null();
    }

    // ── move "<direction>" (s234 m4) ─────────────────────────────────────
    //
    // Reorder within the parent's `children` vector. Direction token
    // vocabulary: "up" (idx-1), "down" (idx+1), "top" (idx=0),
    // "bottom" (idx=last). Mirrors LayersScriptable::move's vocabulary.
    // Z-order convention: lower index = visually on top (the canvas
    // draws children[0] first, children[last] last — first paint goes
    // under subsequent paints). "up" raises in z-order (toward index
    // 0); "down" lowers in z-order (toward last index). Same direction
    // semantics canvas's Arrange { BringForward / SendBackward /
    // BringToFront / SendToBack } use.
    //
    // No-op returns (no command pushed):
    //   - args.empty() — no direction supplied.
    //   - direction token isn't one of the four vocabulary entries.
    //   - target is owned by a non-children slot (structural input;
    //     no list position to move within).
    //   - parent has only one child (target = self).
    //   - target is already at the destination edge ("up" / "top" on
    //     idx 0; "down" / "bottom" on idx last).
    if (verb == "move") {
        if (args.empty()) return ScriptValue::null();
        std::string dir = arg_as_string(args[0]);
        if (dir.empty()) return ScriptValue::null();
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::null();
        ParentSlot slot = find_parent_node_in_project(proj, node);
        if (!slot.parent) return ScriptValue::null();
        if (slot.child_index < 0) return ScriptValue::null();  // non-children slot

        auto& ch = slot.parent->children;
        int n = (int)ch.size();
        int src = slot.child_index;
        int dst = src;
        if      (dir == "up")     dst = std::max(0, src - 1);
        else if (dir == "down")   dst = std::min(n - 1, src + 1);
        else if (dir == "top")    dst = 0;
        else if (dir == "bottom") dst = n - 1;
        else return ScriptValue::null();  // unknown direction
        if (dst == src) return ScriptValue::null();  // already there

        // Direct mutation first (mirrors m2 `new` / `delete` and m3
        // mutating verbs' shape — user perspective is "the verb did
        // the thing"; Ctrl+Z then undoes it). Move the unique_ptr out
        // of the source slot, erase, insert at the destination.
        auto moved = std::move(ch[src]);
        ch.erase(ch.begin() + src);
        ch.insert(ch.begin() + dst, std::move(moved));

        // Invalidate the owning doc's iid index — same shape as m2
        // `new`/`delete`. Find the doc via the parent iid (cheaper
        // than walking docs again).
        auto* owning_doc = curvz::utils::find_doc_by_iid(
            *proj, slot.parent->internal_id);
        if (owning_doc) owning_doc->invalidate_iid_index();

        if (m_history) {
            auto cmd = std::make_unique<Curvz::MoveNodeIndexCommand>(
                proj, slot.parent->internal_id, m_iid, src, dst);
            m_history->push(std::move(cmd));
        }
        notify_doc_changed();
        return ScriptValue::null();
    }

    // ── reparent "<new_parent_iid>" (s234 m4) ────────────────────────────
    //
    // Move the node from its current container's `children` slot
    // into the named container's `children` (front-insert at idx 0,
    // matching `objects new`'s "new on top" convention).
    //
    // Pushes MoveObjectToLayerCommand (unmodified from s171 m2).
    // Its src_layer_iid / dst_layer_iid fields are named for the
    // historical use case (cross-layer DnD), but mechanically the
    // command resolves either iid via find_by_iid which walks the
    // entire project tree — Layers, Groups, Compounds, ClipGroups
    // all participate as resolution targets. Reusing it preserves
    // the existing s170-crash-resilient undo behaviour for cross-
    // container moves.
    //
    // No-op returns (no command pushed):
    //   - args.empty() — no destination supplied.
    //   - dest iid empty or doesn't resolve.
    //   - dest type isn't a container (Path / Text / Image / Ref /
    //     Measurement / Blend / Warp / Guide — see
    //     is_container_type). Blend / Warp have structural slots
    //     not children vectors; refusing them keeps the contract
    //     "children-slot reparent only."
    //   - dest equals self (same node).
    //   - dest is a descendant of self (cycle — would orphan the
    //     moved node into its own subtree).
    //   - source is owned by a non-children slot (clip_shape /
    //     blend_source_* / warp_source — same refusal as `delete`
    //     and `move`).
    if (verb == "reparent") {
        if (args.empty()) return ScriptValue::null();
        std::string dst_iid = arg_as_string(args[0]);
        if (dst_iid.empty()) return ScriptValue::null();
        if (dst_iid == m_iid) return ScriptValue::null();  // self
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::null();

        auto* dst_parent = curvz::utils::find_by_iid(*proj, dst_iid);
        if (!dst_parent) return ScriptValue::null();
        if (!is_container_type(dst_parent->type)) return ScriptValue::null();
        // Cycle refusal: dst must not be in the subtree of node.
        if (is_descendant_of(node, dst_parent)) return ScriptValue::null();

        ParentSlot slot = find_parent_node_in_project(proj, node);
        if (!slot.parent) return ScriptValue::null();
        if (slot.child_index < 0) return ScriptValue::null();  // non-children slot

        // No-op when the new parent equals the current parent AND
        // the front insertion would be a self-move on idx 0 (the
        // node is already on top of the same parent's children).
        // Distinct-parent reparents always proceed; same-parent
        // reparents bail to avoid a redundant erase+insert on the
        // same slot.
        if (slot.parent == dst_parent && slot.child_index == 0) {
            return ScriptValue::null();
        }

        // Capture src / dst doc identities before mutation. Both
        // resolve through find_doc_by_iid; the docs may differ
        // (cross-doc reparent isn't a normal use case but the
        // command is iid-keyed so it would work).
        auto* src_doc = curvz::utils::find_doc_by_iid(
            *proj, slot.parent->internal_id);
        auto* dst_doc = curvz::utils::find_doc_by_iid(*proj, dst_iid);

        // Snapshot for the command BEFORE the mutation. clone_node
        // preserves internal_id; the command's redo / undo each
        // re-insert a fresh clone of this snap, keeping the iid
        // stable so the script's bound variable still resolves
        // post-replay.
        std::unique_ptr<Curvz::SceneNode> snap_for_cmd;
        if (m_history) {
            snap_for_cmd = Curvz::clone_node(*node);
        }

        std::string src_layer_iid = slot.parent->internal_id;
        std::string dst_layer_iid = dst_iid;
        int src_index = slot.child_index;
        int dst_index = 0;  // front-insert ("new on top" convention)

        // Direct mutation: move the unique_ptr from src.children to
        // dst.children at dst_index. The live node's identity stays
        // the same; the snapshot above is the command's redo-source
        // (a fresh clone gets inserted on every redo/undo cycle).
        auto& src_ch = slot.parent->children;
        auto moved = std::move(src_ch[src_index]);
        src_ch.erase(src_ch.begin() + src_index);
        int dst_n = (int)dst_parent->children.size();
        int ins = std::clamp(dst_index, 0, dst_n);
        dst_parent->children.insert(dst_parent->children.begin() + ins,
                                    std::move(moved));

        if (src_doc) src_doc->invalidate_iid_index();
        if (dst_doc && dst_doc != src_doc) dst_doc->invalidate_iid_index();

        if (snap_for_cmd && m_history) {
            auto cmd = std::make_unique<Curvz::MoveObjectToLayerCommand>(
                proj,
                std::move(src_layer_iid),
                std::move(dst_layer_iid),
                m_iid,
                std::move(snap_for_cmd),
                src_index, dst_index);
            m_history->push(std::move(cmd));
        }
        notify_doc_changed();
        return ScriptValue::null();
    }

    // ── duplicate (s234 m4) ──────────────────────────────────────────────
    //
    // Clone the node (and its full subtree) and insert the clone at
    // the original's slot position — the duplicate ends up at the
    // original's index, pushing the original down by one. This
    // matches canvas's alt-drag-duplicate convention (the duplicate
    // ends up on top of the original in z-order).
    //
    // The clone has FRESH internal_ids on every node in its subtree
    // (see freshen_internal_ids comment) so the iid index can hold
    // both original and duplicate distinctly. SVG `id` and `name`
    // are NOT freshened — script-minted objects start with empty
    // `id` (assigned lazily at SVG-export time) and the script
    // controls naming via `rename`. A script that wants " (copy)"
    // suffix semantics can rename after binding the new iid.
    //
    // Pushes DuplicateCommand. Returns the new iid as
    // ScriptValue::text so `set <var> to result` binds for
    // immediate addressing.
    //
    // No-op returns (ScriptValue::text("")):
    //   - source is owned by a non-children slot (clip_shape /
    //     blend_source_* / warp_source — duplicating into a
    //     non-children slot doesn't fit the verb's contract, same
    //     refusal as `delete` / `move` / `reparent`).
    if (verb == "duplicate") {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::text("");

        ParentSlot slot = find_parent_node_in_project(proj, node);
        if (!slot.parent) return ScriptValue::text("");
        if (slot.child_index < 0) return ScriptValue::text("");  // non-children

        // Clone the subtree, then mint fresh internal_ids on every
        // node so the duplicate and the original coexist in the
        // iid index without collision.
        auto dup = Curvz::clone_node(*node);
        freshen_internal_ids(dup.get());
        std::string new_iid = dup->internal_id;

        // Insert at the original's slot index — duplicate goes on
        // top of the original (lower index = visually on top), the
        // original shifts down by one. Matches canvas alt-drag
        // duplicate's index choice.
        int insert_idx = slot.child_index;

        // Snapshot for the command BEFORE inserting. clone_node
        // preserves internal_id of the freshly-iid'd duplicate;
        // the command captures snap + parent_iid + index so undo
        // can find and erase the duplicate by its (now fresh) iid.
        std::unique_ptr<Curvz::SceneNode> snap_for_cmd;
        if (m_history) {
            snap_for_cmd = Curvz::clone_node(*dup);
        }

        slot.parent->children.insert(slot.parent->children.begin() + insert_idx,
                                     std::move(dup));

        auto* owning_doc = curvz::utils::find_doc_by_iid(
            *proj, slot.parent->internal_id);
        if (owning_doc) owning_doc->invalidate_iid_index();

        if (snap_for_cmd && m_history) {
            std::vector<Curvz::DuplicateCommand::Entry> entries;
            entries.push_back({slot.parent->internal_id,
                               std::move(snap_for_cmd),
                               insert_idx});
            auto cmd = std::make_unique<Curvz::DuplicateCommand>(
                proj, std::move(entries));
            m_history->push(std::move(cmd));
        }
        notify_doc_changed();
        return ScriptValue::text(new_iid);
    }

    return ScriptValue::null();
}

// ── ObjectProxy: query ───────────────────────────────────────────────────

ScriptValue ObjectProxy::query(std::string_view property) const {
    auto* node = resolve();
    if (!node) return ScriptValue::null();

    if (property == "name")        return ScriptValue::text(node->name);
    if (property == "type")        return ScriptValue::text(type_to_string(node->type));
    if (property == "visible")     return ScriptValue::boolean(node->visible);
    if (property == "locked")      return ScriptValue::boolean(node->locked);
    if (property == "child_count") return ScriptValue::integer(
                                       static_cast<long long>(node->children.size()));
    if (property == "iid")         return ScriptValue::text(node->internal_id);

    if (property == "parent_iid") {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::text("");
        return ScriptValue::text(find_parent_iid_in_project(proj, node));
    }

    // child_index (s234 m4) — node's slot position in parent->children,
    // or -1 if the node is owned by a non-children structural slot
    // (clip_shape, blend_source_*, warp_source). The smoke uses this
    // to assert that `move` actually changed position; no other element
    // query observes z-order.
    if (property == "child_index") {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::integer(-1);
        ParentSlot slot = find_parent_node_in_project(proj, node);
        return ScriptValue::integer(slot.child_index);
    }

    // node_count (s236 m1) — size of the Path's BezierNode vector.
    // For non-Path types (Group / Compound / Image / etc.) the
    // node->path slot is nullptr; the query returns 0 by contract
    // (treats "no path" as "no nodes" rather than erroring or
    // returning a sentinel). This is the observable for
    // set_path_data — deterministic, no Cairo, pure model side.
    if (property == "node_count") {
        if (!node->path) return ScriptValue::integer(0);
        return ScriptValue::integer(
            static_cast<long long>(node->path->nodes.size()));
    }

    // path_data (s236 m1) — re-emitted SVG-d string via curvz_utils'
    // path_data_to_svg_d pump. Empty string if path is null or has
    // no nodes. NOT byte-identical to the input that last set the
    // path data (fmt2 normalises decimals, segment classification
    // can shift between L and C based on handle degeneracy); the
    // read surface for capture/inspection rather than literal-RHS
    // round-trip assertion. A future structural round-trip probe
    // would parse both strings to PathData and compare.
    //
    // s237 m1 — flip the stored (doc-space) PathData back to
    // user-space before emitting the d-string. The user sees what
    // they typed: set_path_data with "M 0 0 L 100 100 Z" round-
    // trips through the model as a user-space d-string starting
    // with "M 0.00 0.00 ..." (or fmt2-normalised variant), not as
    // doc-space coordinates reflecting the stored Y-flipped form.
    //
    // The stored PathData stays untouched — flip into a transient
    // local copy. node->path is the canonical doc-space store;
    // EditPathCommand undo/redo and the renderer both read it
    // directly. Only the script-side seam crosses to user-space.
    if (property == "path_data") {
        if (!node->path) return ScriptValue::text("");
        // Copy so the flip doesn't touch the canonical doc-space
        // PathData. Empty PathData walks as a no-op (zero nodes).
        Curvz::PathData pd = *node->path;
        if (auto* proj = m_get_project ? m_get_project() : nullptr) {
            if (auto* owner = curvz::utils::find_doc_by_iid(*proj, m_iid)) {
                double ch = (double)owner->canvas_height();
                curvz::utils::path_data_doc_to_user(pd, ch);
            }
            // (Defensive: same shape as the set_path_data verb —
            // missing owner returns the unflipped doc-space form,
            // which the visual channel surfaces. Trace and visual
            // disagree → not done, per the two-channel rule.)
        }
        return ScriptValue::text(curvz::utils::path_data_to_svg_d(pd));
    }

    // fill (s238 m1) — re-emitted SVG fill-attribute string via
    // curvz_utils' fill_style_to_fill_attr pump. The observable for
    // set_fill — byte-clean round-trip on the canonical input forms
    // (lowercase #RRGGBB hex, "none", "currentColor"). Named colours
    // and rgb()/rgba() inputs normalise to lowercase hex on the read
    // side; the trace channel shows the normalised form, the
    // literal-RHS assert in smoke 36 uses the normalised form for
    // cleanness.
    //
    // Unlike path_data, this query is NOT space-aware — fill values
    // don't carry coordinates, so the user-space / doc-space pump
    // pair doesn't apply. A SwatchRef binding or a gradient FillStyle
    // emits a #RRGGBB fallback (first stop or remembered solid
    // colour); the round-trip through script is lossy on those cases
    // by design, since the script-side seam doesn't carry the
    // gradient defs / swatch library context the file IO path does.
    // Scripts that need to apply or read gradient bindings go through
    // the swatches / styles surface, not through set_fill.
    if (property == "fill") {
        return ScriptValue::text(curvz::utils::fill_style_to_fill_attr(node->fill));
    }

    // stroke (s239 m1) — re-emitted SVG fill-attribute string for
    // stroke.paint, via curvz_utils' fill_style_to_fill_attr pump.
    // The observable for set_stroke; same byte-clean round-trip on
    // canonical input forms, same normalising behaviour on named
    // colours / rgb() / rgba() / uppercase hex / short hex. Same
    // not-space-aware posture as the fill query (stroke paint
    // values don't carry coordinates either).
    //
    // Returns the PAINT only, not width / cap / join / miter /
    // opacity. Each of those is its own future query — stroke_width
    // returning a number, stroke_cap / stroke_join returning enum
    // tokens. Set-side and read-side stay symmetric: set_stroke
    // writes only paint and the stroke query reads only paint.
    //
    // For a fresh-mint Path: stroke.paint defaults to
    // FillStyle::CurrentColor (the SceneNode/FillStyle struct
    // default), so the baseline `stroke` query returns "currentColor"
    // before any set_stroke call — same as the baseline `fill`
    // query. width defaults to 0.0, which is why a fresh-mint Path
    // renders no outline even when stroke.paint is set to a visible
    // colour; that's the visual scope-limit closing in m2.
    if (property == "stroke") {
        return ScriptValue::text(curvz::utils::fill_style_to_fill_attr(node->stroke.paint));
    }

    // stroke_width (s239 m2) — direct read of node->stroke.width as a
    // Double-kind ScriptValue. Symmetric observable for set_stroke_width.
    // Same shape as LayersProxy's `opacity` query and GuidesProxy's
    // `x` / `y` / `angle` queries — model field is a double; return
    // it through ScriptValue::real without coercion or formatting.
    //
    // For a fresh-mint Path: stroke.width defaults to 0.0 (the
    // SceneNode/StrokeStyle struct default). The baseline query
    // returns 0.0 before any set_stroke_width call. With m2 in place,
    // a set_stroke + set_stroke_width pair produces a visible outline
    // — the canvas-observable arc's appearance branch closes its
    // visual loop here.
    //
    // ScriptValue strict-equality has implications for the assert
    // side: `assert obj stroke_width == 2.0` compares a Double query
    // result against a Double-kind RHS literal — matches. `assert
    // obj stroke_width == 2` compares Double against an Int-kind RHS
    // and FAILS (different kinds, no coercion). Smoke 38 uses
    // Double literals throughout to make the comparison kind-clean.
    if (property == "stroke_width") {
        return ScriptValue::real(node->stroke.width);
    }

    // cap (s240 m1) — text token for stroke.cap via the anon-
    // namespace cap_to_string helper. Symmetric observable for
    // set_stroke_cap. Vocabulary: "butt" / "round" / "square"
    // (matches StylesScriptable::cap's text query).
    //
    // For a fresh-mint Path: stroke.cap defaults to LineCap::Butt
    // (SceneNode.hpp:76 struct default), so the baseline `cap`
    // query returns "butt" before any set_stroke_cap call.
    //
    // ScriptValue strict-equality: the query returns ScriptValue::text
    // and the assert RHS literal must be a quoted string
    // ("butt" / "round" / "square"). Same kind-cleanness rules
    // as the `stroke` query.
    if (property == "cap") {
        return ScriptValue::text(cap_to_string(node->stroke.cap));
    }

    // join (s240 m1) — text token for stroke.join via
    // join_to_string. Symmetric observable for set_stroke_join.
    // Vocabulary: "miter" / "round" / "bevel". Default is
    // LineJoin::Miter (SceneNode.hpp:77 struct default), so
    // baseline returns "miter".
    if (property == "join") {
        return ScriptValue::text(join_to_string(node->stroke.join));
    }

    // miter (s241 m1) — numeric observable for set_stroke_miter.
    // Returns the double-valued node->stroke.miter directly via
    // ScriptValue::real. Same shape as stroke_width. Default is
    // 4.0 (SceneNode.hpp:78 struct default). ScriptValue
    // strict-equality means asserts use Double literals
    // (`== 4.0`, not `== 4`).
    //
    // Naming note: the query is `miter`, matching the SceneNode
    // field name. The verb is `set_stroke_miter`; the query name
    // drops the `stroke_` prefix to match the proxy's "the
    // proxy IS an object, you query its surface" naming
    // convention (same as `cap` and `join` queries, where the
    // proxy doesn't need to repeat its context in the
    // observable name). StylesScriptable surfaces both sides as
    // `stroke_miter_limit` because the proxy there is a Style,
    // not a SceneNode, and stroke_* prefixes disambiguate from
    // shadow_* / fill_* on the same surface.
    if (property == "miter") {
        return ScriptValue::real(node->stroke.miter);
    }

    // stroke_opacity (s241 m1) — numeric observable for
    // set_stroke_opacity. Returns the double-valued
    // node->stroke.opacity directly. Default is 1.0
    // (SceneNode.hpp:79 struct default — fully opaque).
    //
    // Naming note: kept as `stroke_opacity` rather than
    // `opacity` because `opacity` could be confused with a
    // node-level opacity multiplier (LayersProxy uses `opacity`
    // for that meaning). Explicit `stroke_opacity` disambiguates.
    if (property == "stroke_opacity") {
        return ScriptValue::real(node->stroke.opacity);
    }

    return ScriptValue::null();
}

std::vector<std::string> ObjectProxy::verbs() const {
    return {
        // s232 m3 — five element mutating verbs. Each pushes
        // EditObjectFieldCommand on success; direct mutation
        // happens in the verb body before the push.
        "toggle_visible",
        "set_visible",
        "toggle_locked",
        "set_locked",
        "rename",
        // s236 m1 — geometry-bearing mutating verb. Pushes
        // EditPathCommand (whole-path replace; s167-migrated
        // existing command, no parallel class needed). Direct
        // mutation precedes the push. Refuses on non-Path
        // node types (silent no-op).
        "set_path_data",
        // s288 m1 — first timed-motion verb on ObjectProxy. Drives a
        // single BezierNode handle (in or out) from its current
        // position to a target over a duration. Routes through the
        // m_animate_handle callback to Canvas::animate_handle which
        // owns the timeout loop and the drag-idiom impersonation.
        // NO undo entry — animation is presentation, not edit.
        // Refuses on non-Path / out-of-range index / unknown handle
        // token / empty callback (silent no-op each).
        "animate_handle",
        // s238 m1 — appearance-bearing mutating verb. Pushes
        // EditAppearanceCommand (whole-FillStyle replace plus
        // swatch-id / bound-style break-on-override capture;
        // s167-migrated existing command, fourth "fits unmodified"
        // case for verify-before-assume). Accepts the four
        // context-free fill types (None, CurrentColor, Solid via
        // hex / named / rgb forms); gradients via script land in
        // a future milestone via the swatches / styles surface.
        "set_fill",
        // s239 m1 — symmetric appearance-bearing mutating verb.
        // Same EditAppearanceCommand ride as set_fill (the existing
        // s167-migrated command — FIFTH "fits unmodified" case for
        // verify-before-assume in a row). Writes into stroke.paint
        // only; the other five StrokeStyle fields (width / cap /
        // join / miter / opacity) are preserved and will each be
        // a separate numeric/enum verb in a future milestone. Same
        // colour vocabulary as set_fill via the shared
        // fill_attr_to_fill_style pump in curvz_utils. Universal
        // across SceneNode subtypes.
        "set_stroke",
        // s239 m2 — first NUMERIC-arg verb on ObjectProxy. Closes
        // the appearance branch's visual loop: m1's set_stroke gave
        // a Path's outline a colour; m2 gives it a thickness. With
        // both landed, set_stroke "blue" + set_stroke_width 2.0
        // produces a visible blue outline. Same EditAppearanceCommand
        // ride (SIXTH "fits unmodified" case). Accepts Int and Double
        // via arg_as_double (Int coerced); no clamp, no negative-
        // refusal — matches StylesScriptable::stroke_width's
        // convention.
        "set_stroke_width",
        // s240 m1 — paired enum-token verbs closing the appearance
        // branch's enum-token sub-axis. set_stroke_cap takes one of
        // "butt" / "round" / "square"; set_stroke_join takes one of
        // "miter" / "round" / "bevel". Same EditAppearanceCommand
        // ride (SEVENTH and EIGHTH "fits unmodified" cases in a
        // row). Unknown tokens silent no-op — no CurrentColor-style
        // safe-default for these enum vocabularies. Universal
        // across SceneNode subtypes.
        "set_stroke_cap",
        "set_stroke_join",
        // s241 m1 — closes the appearance branch's remaining
        // numeric stroke fields. set_stroke_miter takes a
        // positive double (4.0 default per StrokeStyle); only
        // matters at sharp corners with miter join. set_stroke_opacity
        // takes a 0.0-1.0 double (1.0 default); alpha multiplier
        // on the resolved stroke paint at the Cairo seam. Both
        // reuse arg_as_double; both ride EditAppearanceCommand
        // (NINTH and TENTH "fits unmodified" cases). No clamp /
        // no refusal — matches set_stroke_width's convention.
        // Universal across SceneNode subtypes.
        "set_stroke_miter",
        "set_stroke_opacity",
        // s234 m4 — three element structural verbs. `move` pushes
        // MoveNodeIndexCommand (new in m4); `reparent` pushes
        // MoveObjectToLayerCommand (existing); `duplicate` pushes
        // DuplicateCommand (existing). Direct mutation happens
        // in the verb body before the push, same shape as m2 / m3.
        "move",
        "reparent",
        "duplicate",
        // m5 (now blocked behind the canvas-observable arc) closes
        // with collection-level grouping (group, ungroup) — those
        // live on the collection, not the proxy.
    };
}

std::vector<std::string> ObjectProxy::properties() const {
    return {
        "name", "type", "visible", "locked",
        "parent_iid", "child_count", "child_index", "iid",
        // s236 m1 — geometry queries. node_count is the observable
        // for set_path_data; path_data is the re-emitted d-string
        // for round-trip / inspection. Both degrade to "" / 0 for
        // non-Path types where node->path is null.
        "node_count", "path_data",
        // s238 m1 — appearance query. `fill` is the observable for
        // set_fill: re-emitted SVG fill-attribute string via
        // curvz_utils' fill_style_to_fill_attr pump. Universal
        // across SceneNode types (appearance lives on every node).
        "fill",
        // s239 m1 — symmetric stroke-side observable. Returns the
        // re-emitted SVG fill-attribute string for stroke.paint via
        // the same pump. Universal across SceneNode types. Reads
        // paint only — width / cap / join / miter / opacity each
        // get their own query when their matching verbs land.
        "stroke",
        // s239 m2 — numeric stroke-width observable. Returns the
        // double-valued node->stroke.width directly via
        // ScriptValue::real. Same shape as LayersProxy's `opacity`.
        // The first numeric ObjectProxy property (others are Int
        // counts or String tokens). Asserts on the script side need
        // Double-kind RHS literals (`== 2.0` not `== 2`).
        "stroke_width",
        // s240 m1 — enum-token stroke observables. cap returns
        // "butt" / "round" / "square" via cap_to_string; join
        // returns "miter" / "round" / "bevel" via join_to_string.
        // Symmetric paired observables for set_stroke_cap and
        // set_stroke_join.
        "cap",
        "join",
        // s241 m1 — numeric stroke observables. `miter` returns
        // node->stroke.miter directly (matches SceneNode field
        // name; default 4.0). `stroke_opacity` returns
        // node->stroke.opacity (default 1.0; explicit stroke_
        // prefix disambiguates from layer-level opacity meanings).
        "miter",
        "stroke_opacity",
    };
}

// ── ObjectsScriptable ────────────────────────────────────────────────────

ObjectsScriptable::ObjectsScriptable(ProjectGetter get_project,
                                     Curvz::CommandHistory* history,
                                     NotifyDocChanged notify_doc_changed,
                                     AnimateHandle animate_handle)
    : Scriptable("objects")
    , m_get_project(std::move(get_project))
    , m_history(history)
    , m_notify_doc_changed(std::move(notify_doc_changed))
    , m_animate_handle(std::move(animate_handle)) {
    // Registry registration happens in the Scriptable base ctor under
    // the name "objects". MainWindow holds us as a member; the
    // registry entry lives for the window's lifetime.
}

// ── Router hooks ─────────────────────────────────────────────────────────

bool ObjectsScriptable::can_resolve(std::string_view key) const {
    if (key.empty()) return false;
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return false;
    auto* node = curvz::utils::find_by_iid(*proj, std::string(key));
    return is_scene_object(node);
}

std::unique_ptr<Scriptable>
ObjectsScriptable::proxy_for(std::string_view key) {
    if (!can_resolve(key)) return nullptr;
    return std::make_unique<ObjectProxy>(m_get_project, m_history,
                                         m_notify_doc_changed,
                                         m_animate_handle,
                                         std::string(key));
}

// ── Collection invoke ────────────────────────────────────────────────────
//
// m1 had two read-shaped invoke verbs (find_by_name, find_by_type)
// living here because today's query() can't take args. m2 adds two
// MUTATING structural verbs (new, delete) — the first writes on the
// `objects` surface. See header note "find_by_name and find_by_type
// are invoke-shaped" above.

ScriptValue ObjectsScriptable::invoke(std::string_view verb,
                                      const ScriptArgs& args) {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return ScriptValue::null();
    auto* doc = proj->active_doc();
    if (!doc) return ScriptValue::null();

    if (verb == "find_by_name") {
        // Read-shaped verb — returns iid of the first in-scope scene
        // object whose `name` matches exactly, or "" on miss. Walks
        // the doc tree depth-first; first hit wins. Names aren't
        // unique by construction; matches LayersScriptable's
        // find_by_name "first hit" contract.
        if (args.empty()) return ScriptValue::text("");
        std::string target = arg_as_string(args[0]);
        if (target.empty()) return ScriptValue::text("");
        std::string found;
        walk_doc_tree(doc, [&](Curvz::SceneNode* node) -> bool {
            if (!is_scene_object(node)) return false;
            if (node->name == target) {
                found = node->internal_id;
                return true;  // early exit
            }
            return false;
        });
        return ScriptValue::text(found);
    }

    if (verb == "find_by_type") {
        // Read-shaped verb — returns iid of the first in-scope scene
        // object whose type matches the supplied lowercase token, or
        // "" on miss / unknown type. The token vocabulary is exactly
        // the one type_to_string emits — "path" / "group" /
        // "compound" / "clipgroup" / "blend" / "warp" / "text" /
        // "image" / "ref" / "measurement". Out-of-scope types
        // (layer, guide, etc.) silently don't match — passing
        // "layer" returns "" rather than dispatching against the
        // out-of-scope set.
        if (args.empty()) return ScriptValue::text("");
        std::string target = arg_as_string(args[0]);
        if (target.empty()) return ScriptValue::text("");
        std::string found;
        walk_doc_tree(doc, [&](Curvz::SceneNode* node) -> bool {
            if (!is_scene_object(node)) return false;
            if (type_to_string(node->type) == target) {
                found = node->internal_id;
                return true;  // early exit
            }
            return false;
        });
        return ScriptValue::text(found);
    }

    // ── new <type> (s231 m2) ─────────────────────────────────────────────
    //
    // Mints a fresh in-scope scene object of the given type and
    // inserts it at the FRONT of the active layer's children
    // (canvas convention — new-on-top). Returns the new iid as a
    // string so a script can `set <var> to result` and address the
    // node via `objects.<var>` from then on.
    //
    // Pushes an AddNodeCommand so Ctrl+Z removes the new node.
    // The command captures the parent (active layer) as a raw
    // pointer — same pre-iid-migration shape Canvas tools use
    // when pushing AddNodeCommand. A future structural-iid
    // migration will sweep these to CurvzProject* + iid captures
    // alongside Canvas's own push sites; until then the parent
    // pointer is durable for the lifetime of the active layer
    // (which doesn't get destroyed between push and undo under
    // normal use).
    //
    // No-op returns (text("")):
    //   - args.empty() — caller didn't supply a type token.
    //   - args[0] is not a string-shaped ScriptValue.
    //   - the type token isn't in the m2 vocabulary ("path" or
    //     "group"). Future milestones extend the vocabulary; today
    //     unknown tokens return "" rather than erroring, matching
    //     the find_by_type miss contract.
    //   - active_doc has no active layer (defensive — should not
    //     happen with a real project but degrade gracefully).
    if (verb == "new") {
        if (args.empty()) return ScriptValue::text("");
        std::string type_tok = arg_as_string(args[0]);
        if (type_tok.empty()) return ScriptValue::text("");
        Curvz::SceneNode::Type new_type{};
        if (!parse_type_token(type_tok, new_type)) {
            return ScriptValue::text("");
        }
        auto* active = doc->active_layer();
        if (!active) return ScriptValue::text("");

        auto fresh = mint_scene_object(new_type);
        std::string new_iid = fresh->internal_id;

        // Snapshot for the command BEFORE inserting — clone_node
        // preserves internal_id so the command's redo (a future
        // Ctrl+Y) re-inserts a node with the same iid. The original
        // unique_ptr moves into the children vector; the cloned
        // unique_ptr is what the command owns.
        std::unique_ptr<Curvz::SceneNode> snap_for_cmd;
        if (m_history) {
            snap_for_cmd = Curvz::clone_node(*fresh);
        }

        active->children.insert(active->children.begin(),
                                std::move(fresh));
        doc->invalidate_iid_index();

        if (snap_for_cmd) {
            auto cmd = std::make_unique<Curvz::AddNodeCommand>(
                active, std::move(snap_for_cmd));
            m_history->push(std::move(cmd));
        }
        if (m_notify_doc_changed) m_notify_doc_changed();
        return ScriptValue::text(new_iid);
    }

    // ── delete <iid> (s231 m2) ───────────────────────────────────────────
    //
    // Removes the in-scope scene object identified by iid from its
    // owning container. Resolves the iid project-wide (the node may
    // live in any open doc, same shape as `layers delete`). Refuses
    // to delete nodes owned by a structural slot (clip_shape,
    // blend_source_a, blend_source_b, warp_source) — pulling a node
    // out of those slots would dissolve the containing ClipGroup /
    // Blend / Warp, which is a different surface than the leaf-
    // delete this verb implements. Scheduled for a later milestone.
    //
    // Pushes a DeleteObjectCommand so Ctrl+Z restores the node at
    // its original child-index. Scrubs the global undo stack for
    // raw-pointer-capture references to the about-to-be-deleted
    // node and its descendants — same defensive walk
    // LayersScriptable::delete uses.
    //
    // No-op returns (null):
    //   - args.empty() — caller didn't supply an iid.
    //   - args[0] is not a string-shaped ScriptValue.
    //   - iid doesn't resolve to an in-scope scene object (already
    //     deleted, never existed, or names a layer/guide/special-
    //     layer iid that `objects` doesn't own).
    //   - the resolved node has no findable parent in any doc
    //     (shouldn't happen for a resolved iid — defensive).
    //   - the node is owned by a non-children slot (see above).
    if (verb == "delete") {
        if (args.empty()) return ScriptValue::null();
        std::string target_iid = arg_as_string(args[0]);
        if (target_iid.empty()) return ScriptValue::null();
        auto* node = curvz::utils::find_by_iid(*proj, target_iid);
        if (!is_scene_object(node)) return ScriptValue::null();

        ParentSlot slot = find_parent_node_in_project(proj, node);
        if (!slot.parent) return ScriptValue::null();
        // Refuse non-children-owned targets — see header note above.
        if (slot.child_index < 0) return ScriptValue::null();

        // Defensive scrub: any LEGACY raw-pointer-capture command on
        // the undo stack that references this node (or any of its
        // descendants) gets neutralised before we erase. Same shape
        // LayersScriptable::delete uses — defensive and cheap. The
        // s167 iid migration covers EditPathCommand and friends, but
        // older commands (and any non-migrated ones) may still hold
        // raw pointers; scrub_command_history walks the stack and
        // neutralises those captures by predicate.
        if (m_history) {
            Curvz::SceneNode* to_delete = node;
            std::function<void(Curvz::SceneNode*)> scrub_walk =
                [&](Curvz::SceneNode* n) {
                    if (!n) return;
                    m_history->scrub_command_history(n);
                    for (auto& c : n->children) scrub_walk(c.get());
                    if (n->clip_shape)     scrub_walk(n->clip_shape.get());
                    if (n->blend_source_a) scrub_walk(n->blend_source_a.get());
                    if (n->blend_source_b) scrub_walk(n->blend_source_b.get());
                    if (n->warp_source)    scrub_walk(n->warp_source.get());
                };
            scrub_walk(to_delete);
        }

        // Snapshot for the command BEFORE erasing — clone_node
        // preserves internal_id so undo re-inserts a node with the
        // same iid (the script's bound variable still resolves
        // post-undo).
        std::unique_ptr<Curvz::SceneNode> snap_for_cmd;
        if (m_history) {
            snap_for_cmd = Curvz::clone_node(*node);
        }

        // Erase by iid match on the parent's children vector — the
        // index we captured is also the natural target, but iid
        // match is the safer comparison if anything inserted between
        // the find and the erase (vanishingly unlikely in this
        // single-threaded path; defensive).
        auto& ch = slot.parent->children;
        int erase_idx = slot.child_index;
        for (int i = 0; i < (int)ch.size(); ++i) {
            if (ch[i].get() == node) { erase_idx = i; break; }
        }
        // Find which doc owns the parent so we can invalidate its
        // iid index. The parent walk already told us about the
        // doc-by-finding-it; find_doc_by_iid on the parent's iid is
        // the cheapest reuse of existing curvz_utils.
        auto* owning_doc = curvz::utils::find_doc_by_iid(
            *proj, slot.parent->internal_id);
        ch.erase(ch.begin() + erase_idx);
        if (owning_doc) owning_doc->invalidate_iid_index();

        if (snap_for_cmd && m_history) {
            auto cmd = std::make_unique<Curvz::DeleteObjectCommand>(
                slot.parent, std::move(snap_for_cmd), erase_idx);
            m_history->push(std::move(cmd));
        }
        if (m_notify_doc_changed) m_notify_doc_changed();
        return ScriptValue::null();
    }

    // ── group <iid1> <iid2> ... (s242 m2) ────────────────────────────────
    //
    // Wraps two or more in-scope scene objects in a new Group node.
    // The targets must be SIBLINGS — direct children of the same
    // parent container (a Layer, or another Group / Compound /
    // ClipGroup). The new Group lands at the topmost target's
    // original z-position (lowest index in parent->children); the
    // targets become its children in parent z-order, matching
    // Canvas::group_selection's preservation rule (the group's
    // internal stacking matches the layer's existing stacking,
    // regardless of how the script listed the targets).
    //
    // Pushes a GroupNodesCommand (s242 m1) which captures the
    // operation by iid + index. Ctrl+Z dissolves the group and
    // restores the targets to their original positions; redo
    // re-wraps them under a new Group with the SAME iid (so script-
    // side state referring to the group iid stays consistent
    // across undo/redo cycles).
    //
    // Returns: the new Group's iid as a text ScriptValue (so the
    // caller can `set g to result` and address the group via
    // `objects.g` from then on). Returns "" on any failure
    // condition (atomic posture — no partial group ever lands).
    //
    // No-op returns (text("")):
    //   - args.size() < 2 — need at least two iids to form a group.
    //   - any arg is not a string-shaped ScriptValue, or is empty.
    //   - any iid doesn't resolve to an in-scope scene object
    //     (already deleted, never existed, or names an out-of-
    //     scope type like layer/guide).
    //   - the targets don't all share a parent (cross-container
    //     grouping is not in scope; matches Canvas::group_selection's
    //     posture of refusing if find_parent on the first target
    //     fails or the targets came from different layers).
    //   - any target is in a non-children slot (clip_shape, blend
    //     sources, warp source) — those are structural inputs to
    //     their containers, not free siblings.
    //
    // Why not refuse on a single-iid call: a single-target group
    // is a valid operation in principle (wraps one object in a
    // 1-child Group, useful for forcing a group context). But
    // Canvas::group_selection requires >= 2 by convention; this
    // verb matches the canvas-side rule. A future "wrap" verb
    // could surface the 1-target case if a script need arises.
    if (verb == "group") {
        if (args.size() < 2) return ScriptValue::text("");

        // First pass — resolve every iid to an in-scope scene object,
        // and verify they all share a parent. Build parallel vectors
        // of pointer + iid; the parent comes from the first target
        // and every subsequent target must match.
        std::vector<Curvz::SceneNode*> target_nodes;
        std::vector<std::string>       target_iids;
        target_nodes.reserve(args.size());
        target_iids.reserve(args.size());

        Curvz::SceneNode* common_parent = nullptr;
        for (size_t i = 0; i < args.size(); ++i) {
            std::string iid = arg_as_string(args[i]);
            if (iid.empty()) return ScriptValue::text("");
            auto* node = curvz::utils::find_by_iid(*proj, iid);
            if (!is_scene_object(node)) return ScriptValue::text("");
            ParentSlot slot = find_parent_node_in_project(proj, node);
            if (!slot.parent) return ScriptValue::text("");
            // Refuse non-children-owned targets — same rule as
            // `delete`. A clip_shape / blend source / warp source
            // is structural input, not a free sibling.
            if (slot.child_index < 0) return ScriptValue::text("");
            if (i == 0) {
                common_parent = slot.parent;
            } else if (slot.parent != common_parent) {
                // Cross-parent grouping — refuse atomically.
                return ScriptValue::text("");
            }
            target_nodes.push_back(node);
            target_iids.push_back(std::move(iid));
        }
        if (!common_parent) return ScriptValue::text("");

        // Build the capture in PARENT Z-ORDER. Walk common_parent's
        // children once and pluck the targets in order. This matches
        // Canvas::group_selection's preservation rule.
        std::set<Curvz::SceneNode*> target_set(target_nodes.begin(),
                                               target_nodes.end());
        std::vector<std::string> ordered_iids;
        std::vector<int>         ordered_indices_before;
        ordered_iids.reserve(target_nodes.size());
        ordered_indices_before.reserve(target_nodes.size());
        int insert_idx = (int)common_parent->children.size();
        for (int i = 0; i < (int)common_parent->children.size(); ++i) {
            auto* c = common_parent->children[i].get();
            if (target_set.count(c)) {
                ordered_iids.push_back(c->internal_id);
                ordered_indices_before.push_back(i);
                insert_idx = std::min(insert_idx, i);
            }
        }
        // Defensive: every target must have been found via the walk.
        // If the count doesn't match, something's off (e.g. duplicate
        // iids in args, or a target wasn't actually a child of
        // common_parent despite ParentSlot saying so — shouldn't
        // happen, but bail safely).
        if (ordered_iids.size() != target_nodes.size()) {
            return ScriptValue::text("");
        }

        // Get the doc that owns common_parent so we can mint a fresh
        // default name. Resolve via find_doc_by_iid on the parent's
        // iid.
        auto* owning_doc = curvz::utils::find_doc_by_iid(
            *proj, common_parent->internal_id);
        if (!owning_doc) return ScriptValue::text("");

        std::string gname = owning_doc->next_default_name(
            Curvz::CurvzDocument::NameKind::Group);
        std::string giid = Curvz::generate_internal_id();

        // Build the command. execute() runs synchronously here (same
        // pattern as the structural verbs above and as Canvas's
        // migrated group/ungroup methods — the command IS the record
        // of what we did, not a deferred action).
        auto cmd = std::make_unique<Curvz::GroupNodesCommand>(
            proj,
            common_parent->internal_id,
            std::move(ordered_iids),
            std::move(ordered_indices_before),
            insert_idx,
            giid,
            gname,
            "Group objects (script)");
        cmd->execute();

        if (m_history) {
            m_history->push(std::move(cmd));
        }
        if (m_notify_doc_changed) m_notify_doc_changed();
        return ScriptValue::text(giid);
    }

    // ── ungroup <iid> (s242 m2) ──────────────────────────────────────────
    //
    // Dissolves the Group identified by iid: removes it from its
    // parent, promotes its children to the parent's children vector
    // at the Group's original index. Pushes an UngroupNodeCommand
    // (s242 m1) so Ctrl+Z re-creates the Group with the same iid
    // and the same children.
    //
    // Returns: null on success or failure (the operation is
    // structural; the script doesn't need an iid back — the
    // children kept their iids and are still addressable).
    //
    // No-op returns (null):
    //   - args.empty() — caller didn't supply an iid.
    //   - args[0] is not a string-shaped ScriptValue, or is empty.
    //   - iid doesn't resolve, or resolves to a non-Group SceneNode.
    //   - the target Group has no parent (e.g. it's a top-level
    //     layer, but layers are filtered out by is_scene_object;
    //     defensive).
    //   - the target Group is owned by a non-children slot (e.g. a
    //     ClipGroup's clip_shape — refused for the same reason
    //     `delete` refuses these).
    //   - the target Group is empty (no children to promote;
    //     dissolving an empty group is a delete, and a delete-
    //     shaped op should use `delete`, not `ungroup`).
    if (verb == "ungroup") {
        if (args.empty()) return ScriptValue::null();
        std::string target_iid = arg_as_string(args[0]);
        if (target_iid.empty()) return ScriptValue::null();
        auto* node = curvz::utils::find_by_iid(*proj, target_iid);
        if (!is_scene_object(node)) return ScriptValue::null();
        if (node->type != Curvz::SceneNode::Type::Group) {
            return ScriptValue::null();
        }
        ParentSlot slot = find_parent_node_in_project(proj, node);
        if (!slot.parent) return ScriptValue::null();
        if (slot.child_index < 0) return ScriptValue::null();
        if (node->children.empty()) return ScriptValue::null();

        // Capture the children's iids in their group-children order.
        std::vector<std::string> child_iids;
        child_iids.reserve(node->children.size());
        for (const auto& c : node->children) {
            child_iids.push_back(c->internal_id);
        }

        std::string gname = node->name;
        std::string giid  = node->internal_id;

        auto cmd = std::make_unique<Curvz::UngroupNodeCommand>(
            proj,
            giid,
            slot.parent->internal_id,
            slot.child_index,
            gname,
            std::move(child_iids),
            "Ungroup (script)");
        cmd->execute();

        if (m_history) {
            m_history->push(std::move(cmd));
        }
        if (m_notify_doc_changed) m_notify_doc_changed();
        return ScriptValue::null();
    }

    return ScriptValue::null();
}

// ── Collection query ─────────────────────────────────────────────────────

ScriptValue ObjectsScriptable::query(std::string_view property) const {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) {
        // No project — defensible empties. Matches LayersScriptable /
        // GuidesScriptable: a test running before project open sees
        // 0 / "" not null.
        if (property == "count")    return ScriptValue::integer(0);
        if (property == "all_iids") return ScriptValue::text("");
        return ScriptValue::null();
    }
    auto* doc = proj->active_doc();
    if (!doc) {
        if (property == "count")    return ScriptValue::integer(0);
        if (property == "all_iids") return ScriptValue::text("");
        return ScriptValue::null();
    }

    if (property == "count") {
        long long n = 0;
        walk_doc_tree(doc, [&](Curvz::SceneNode* node) -> bool {
            if (is_scene_object(node)) ++n;
            return false;  // no early exit — full walk
        });
        return ScriptValue::integer(n);
    }

    if (property == "all_iids") {
        // Tree-walk order (depth-first, layer-top-down). Same sentinel
        // shape as LayersScriptable / GuidesScriptable all_iids —
        // comma-separated string awaiting a foreach grammar.
        std::ostringstream os;
        bool first = true;
        walk_doc_tree(doc, [&](Curvz::SceneNode* node) -> bool {
            if (!is_scene_object(node)) return false;
            if (!first) os << ',';
            os << node->internal_id;
            first = false;
            return false;  // no early exit — full walk
        });
        return ScriptValue::text(os.str());
    }

    // find_by_name and find_by_type are invoke-shaped (take args);
    // see invoke() above. The header documents the grammar reason.

    return ScriptValue::null();
}

std::vector<std::string> ObjectsScriptable::verbs() const {
    return {
        // Read-shaped (no-arg query() can't take a string arg today;
        // exposed as invoke). Each returns iid or "".
        "find_by_name",
        "find_by_type",
        // Structural verbs (s231 m2). Each pushes a command on the
        // global undo stack on success.
        "new",
        "delete",
        // s242 m2 — collection-level grouping verbs. `group` wraps
        // two or more siblings in a new Group (returns the group's
        // iid); `ungroup` dissolves a Group and promotes its children
        // to the parent. Both push the s242 m1 iid-migrated structural
        // commands (GroupNodesCommand / UngroupNodeCommand) so Ctrl+Z
        // round-trips cleanly. This is the m5 close — the
        // canvas-observable arc's collection-level branch lands here,
        // and the `objects` Tier 3 entry now books.
        "group",
        "ungroup",
        // Element mutating verbs live on the proxy, not the
        // collection — see ObjectProxy::verbs(). m3 added them
        // (toggle_visible / set_visible / toggle_locked /
        // set_locked / rename). m4 added element structural verbs
        // (move, reparent, duplicate). m5 closes here with the
        // collection-level grouping verbs above.
    };
}

std::vector<std::string> ObjectsScriptable::properties() const {
    return {
        "count",
        "all_iids",
    };
}

} // namespace curvz::scripting
