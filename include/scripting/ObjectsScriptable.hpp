// scripting/ObjectsScriptable.hpp ────────────────────────────────────────────
//
// s230 m1 — sixth row-bound model Scriptable, OPENING the multi-session
// `objects` arc. Wraps the active document's scene-content surface as
// a collection Scriptable under abbrev `objects`, materialising
// transient `objects.<iid>` proxies for per-instance read operations
// via the s216 m1 router hooks (Scriptable::can_resolve / proxy_for).
//
// s231 m2 — adds two collection-level structural verbs (`new <type>`
// and `delete <iid>`) so the smoke can build up its own test objects
// and tear them back down — the same owns-its-conditions BUILD UP /
// VALIDATE / TEAR DOWN structure 33_layers / 27_guides established.
// Element queries from m1 are now exercised end-to-end on iids the
// smoke itself minted, not on whatever happened to be in the doc at
// test time.
//
// s232 m3 — adds five element mutating verbs on the proxy:
// `toggle_visible`, `set_visible <bool>`, `toggle_locked`,
// `set_locked <bool>`, `rename "<name>"`. Each is a thin shell over
// the new `EditObjectFieldCommand` (a direct analog of
// `EditLayerFieldCommand` minus the Color field). The s230 m1
// header's prediction was that these verbs could ride existing
// `EditObjectCommand` / `EditNodeCommand` surfaces — that turned
// out to be wrong. `EditObjectCommand` is the inspector's
// appearance/path-edit command (path / fill / stroke / shadow);
// it doesn't carry name / visible / locked. m3 adds
// `EditObjectFieldCommand` to close the gap. The existing user-
// facing rename path in LayersPanel for non-layer objects has
// been DIRECT mutation with no undo support since forever; m3
// keeps the change scoped to the script path, leaving the panel
// sweep as future band-3 mop-up.
//
// s234 m4 — adds three element structural verbs on the proxy:
// `move "<direction>"`, `reparent "<new_parent_iid>"`, and
// `duplicate`. Plus a new element query `child_index` (int) so
// `move` round-trips are observable from script land.
//
// s235 m1 — wires a NotifyDocChanged callback through the ctor and
// down into ObjectProxy so every mutating verb (collection-level
// `new` / `delete`; element-level toggle_visible / set_visible /
// toggle_locked / set_locked / rename / move / reparent / duplicate)
// fires the canvas-refresh cascade after a successful push. Closes
// the gap Scott caught at the end of s234: script-path mutations
// were updating CurvzDocument and the iid index but never telling
// Canvas anything changed, so the smoke ran clean but the canvas
// kept rendering its previous frame. MainWindow supplies a callback
// that runs `m_canvas.queue_draw(); m_canvas.notify_document_changed();`
// — the canonical two-call shape for external mutators, mirroring
// s227 m1's themes_refresh_cascade. The callback may be nullptr; in
// that case every verb silently skips the refresh step (matches the
// graceful-degradation contract the history pointer already uses).
// The gap is arc-wide — LayersScriptable / GuidesScriptable et al.
// have the same hole — but s235 m1 scope is ObjectsScriptable only;
// the sibling Scriptables get the same callback retrofit in a
// follow-up sweep.
//
// s236 m1 — OPENS the canvas-observable scripts arc by adding the
// first geometry-bearing verb on the proxy: `set_path_data
// "<svg_d_string>"`. Plus two new element queries: `node_count`
// (int — size of the Path's BezierNode vector, 0 for non-Path) and
// `path_data` (text — re-emitted SVG-d string for round-trip /
// inspection use cases). Together these let scripts BUILD UP a
// canvas-observable Path end-to-end without depending on pre-
// existing canvas state — the gap the s235 close logged in the
// "Self-contained canvas-observable tests" canon entry.
//
// Rides EXISTING `EditPathCommand` (the s167/s168-migrated whole-
// path replace command). First-task verification confirmed it's a
// perfect shape match — captures CurvzProject* + obj_iid + before/
// after PathData, executes / undoes via `*obj->path = after/before`.
// No parallel command class needed, unlike m3's EditObjectFieldCommand
// (vs EditObjectCommand) and m4's MoveNodeIndexCommand (vs
// ZOrderCommand). The s230-m1 prediction about command-class reuse
// was wrong for m3 and m4 but right here — third matching
// application of the "verify before assuming" first-task discipline.
//
// New `curvz::utils::svg_d_to_path_data` and `path_data_to_svg_d`
// pumps in curvz_utils. The path_data_to_svg_d emitter is a full
// lift from SvgWriter's two inline emitters (duplicates of each
// other; the writer keeps its inline copies for now — sweep
// deferred). svg_d_to_path_data bridges to SvgParser's existing
// parse_path_d via a non-static wrapper added in m1 (the parser body
// is 260 lines plus a 60-line arc_to_bezier helper, both file-static
// and intertwined with parse_path_d_multi; full lift deferred to a
// future SvgParser sweep milestone). The visible-from-outside seam
// is the curvz_utils pump pair; the file-static parser body becomes
// callable through the bridge for now.
//
//   `set_path_data` early-returns (no command pushed) when:
//     - args.empty() — no string supplied.
//     - resolved node has no `path` slot (non-Path types: Group,
//       Compound, Image, Ref, Measurement, etc.). Silent no-op,
//       matching how other proxy verbs handle off-type calls (no
//       error sentinel through ScriptValue — that's a future
//       structured-error-channel concern, see s235 backlog).
//   On success: direct mutation (*node->path = after) precedes the
//   EditPathCommand push, mirroring m2/m3/m4's shape. The owning
//   doc's iid index is NOT invalidated by this verb — path-data
//   changes don't reshape the iid space (no internal_id mints, no
//   tree-structure changes, no parent/child slot moves). The s168
//   m6 "invalidate before resolve" inside EditPathCommand::execute
//   covers redo/undo cycles defensively.
//
//   `node_count` returns the size of node->path->nodes; 0 if path
//   is null. The observable for set_path_data — pure model side,
//   no Cairo, deterministic.
//
//   `path_data` returns the re-emitted d-string via
//   path_data_to_svg_d; "" if path is null or empty. NOT byte-
//   identical to the most-recent set_path_data input (fmt2
//   normalisation, segment-class reclassification); a future
//   structural round-trip probe would parse both and compare
//   PathData. m1 ships the read query for capture/inspection use
//   cases; a literal-RHS round-trip assert isn't in scope.
//
//   `move` rides a NEW command class — `MoveNodeIndexCommand`
//   (single-target, iid-keyed intra-parent reorder). The s231
//   handoff's m4 prediction was that move could ride the existing
//   `ZOrderCommand`; that turned out to be wrong. ZOrderCommand's
//   `apply_order` keys on SVG `id` (not internal_id), and script-
//   minted objects have empty SVG `id` (it's assigned lazily at
//   SVG-export time). Multiple script-minted siblings would
//   collide on the empty key and the redo / undo would silently
//   drop entries. Same first-task-of-milestone "verify before
//   assuming" precedent as m3's discovery about EditObjectCommand
//   vs EditObjectFieldCommand.
//
//   `reparent` rides existing `MoveObjectToLayerCommand`
//   unmodified. The verification (also first task of m4) confirms
//   its src_layer_iid / dst_layer_iid resolution via find_by_iid
//   is container-agnostic — it walks the whole project tree, not
//   just doc->layers — so Group/Compound/ClipGroup containers
//   work as destinations the same as Layers do. The command's
//   description string is the only layer-flavoured cosmetic
//   ("Move to layer"); preserved for now.
//
//   `duplicate` rides existing `DuplicateCommand`. Script-side
//   prep mints a FRESH internal_id on the cloned subtree before
//   insertion — clone_node copies internal_id verbatim (it has to,
//   for command snap/undo to re-insert with the same identity),
//   and an unfresh duplicate would collide with the original in
//   the iid index. Canvas's `freshen_ids` helper only freshens
//   SVG `id` and `name`, not `internal_id` — that's a separate
//   latent issue in canvas duplicate, out of scope for m4;
//   scope here is the script path. The verb returns the new
//   iid into `result` so a script can `set <var> to result` and
//   address the duplicate immediately.
//
// This is the largest of the row-bound Scriptables in scope. The five
// shipped before this one (layers / guides / swatches / styles /
// themes) each wrapped a single collection — either a SceneNode-type-
// filter on the doc's top-level layers vector, or a project-scoped
// library. `objects` wraps "everything addressable in the scene tree"
// and walks recursively. Pattern stays the same as the five shipped
// (collection-as-router with transient per-instance proxies); the
// shape of the collection differs.
//
// s237 m1 — RETROFITS set_path_data and path_data to speak user-space.
//
// The s236 m1 visual channel surfaced an architectural call: the
// scripter is a user surface, an extension of the app *to* the user.
// The d-string a user types into a script must be interpreted in the
// space the user sees (Y-up, origin at bottom-left of artboard), not
// in the doc-space (Y-down, top-left) the engine stores internally.
// s236 m1's geometry landed at the doc-space origin and Scott noted
// the visual mismatch — the trace passed but the user-facing axes
// were inverted. m1 reuses the existing `CoordSpace` Y-flip seam from
// `include/CoordSpace.hpp` (the canonical "(canvas_height - y)
// appears nowhere else" home) through a new `curvz::utils` pump
// pair: `path_data_user_to_doc` and `path_data_doc_to_user`. Each
// walks the PathData's BezierNode vector and routes every (y, cy1,
// cy2) through CoordSpace.
//
// Third matching application of the "verify before assume" first-
// task discipline in a row: s232 m3 + s234 m4 found commands NEEDED
// new parallel classes, s236 m1 found EditPathCommand FIT unmodified,
// s237 m1 found the Y-flip pump ALREADY EXISTED (CoordSpace) — the
// retrofit is a lift, not a build. The pattern is not "predict, then
// look"; it's "look first."
//
// Wiring change at the script-side seam:
//   - set_path_data: parse → look up owning doc → flip user→doc →
//     direct mutation + EditPathCommand push (carrying doc-space
//     PathData, the convention every canvas-tool site already uses).
//   - path_data query: copy stored PathData → look up owning doc →
//     flip doc→user → emit user-space d-string.
// EditPathCommand undo/redo behaviour is unchanged — it has no
// awareness of user-space; only the script-side seam crosses.
//
// "Infrastructure pump in curvz_utils, called from every site that
// crosses the seam" is now banked at three pumps:
//   - find_by_iid       (s167 m1 — iid migration seam)
//   - SVG-d pump pair   (s236 m1 — d-string ↔ PathData seam)
//   - user-space pair   (s237 m1 — user ↔ doc geometry seam)
// Promotes to a tier of its own in the LEDGER on next consolidation.
//
// ── Arc progress (multi-session by construction) ──────────────────────────
//
// What m1 + m2 + m3 + m4 (+ s235 m1 + s236 m1 + s237 m1) ship combined:
//   - The collection registered as `objects` in the registry.
//   - Collection queries: count, all_iids, find_by_name, find_by_type.
//   - Per-instance proxies materialised by proxy_for(iid).
//   - Element queries: name, type, visible, locked, parent_iid,
//     child_count, child_index, iid, node_count, path_data (the
//     last returns user-space coords as of s237 m1).
//   - Collection-level structural verbs: new <type>, delete <iid>.
//   - Element mutating verbs: toggle_visible, set_visible <bool>,
//     toggle_locked, set_locked <bool>, rename "<name>",
//     set_path_data "<svg_d_string>" (interprets in user-space as
//     of s237 m1; user→doc flip happens between parse and store).
//   - Element structural verbs: move "<direction>",
//     reparent "<new_parent_iid>", duplicate.
//   - History pointer captured in the ctor and ACTIVELY USED by the
//     collection structural verbs (AddNodeCommand / DeleteObjectCommand)
//     AND by the element mutating verbs (EditObjectFieldCommand,
//     EditPathCommand) AND by the element structural verbs
//     (MoveNodeIndexCommand / MoveObjectToLayerCommand /
//     DuplicateCommand). All verbs degrade gracefully to direct
//     mutation when history is null.
//   - Canvas-refresh callback fires after every successful mutating
//     verb (s235 m1 wiring; s236 m1's set_path_data participates;
//     s237 m1 retrofit doesn't change the callback shape).
//
// What s237 m1 still does NOT ship:
//   - `set_fill` / `set_stroke`. Scheduled m2 / m3 of the canvas-
//     observable arc. Geometry alone is wireframe-only on canvas;
//     fill/stroke make the Path canvas-visible in fill-shaded modes.
//   - `bbox_w` / `bbox_h` queries. Bbox math needs the Cairo path
//     pass — non-trivial promotion. Deferred until a smoke needs it.
//   - `closed` read query (the bool flag on PathData). Natural pair
//     to node_count for round-tripping shape; future milestone.
//   - Collection-level grouping verbs (group, ungroup). Scheduled
//     for the objects-arc-close milestone (still pending; the
//     canvas-observable arc takes priority over closing objects).
//   - selected_iids query. CANON places `selection` as a separate
//     singleton Scriptable, not a query on `objects`. Scheduled
//     with the `selection` singleton when that lands.
//
// Closing the canvas-observable arc may take 2-4 sessions total.
// m1 opens geometry; m2 adds fill; m3 adds stroke; a possible m4
// integrates them with a canvas-observable smoke. The objects arc
// proper (group/ungroup) waits until after — m5 grouping would
// also benefit from canvas-observable smoke shape, which is
// exactly what this arc is building.
//
// ── Scope of the collection ──────────────────────────────────────────────
//
// The s230 handoff surfaced three plausible boundaries for what
// `objects` should address:
//
//   (i) All SceneNodes including layers / guides / refs / measure
//       nodes. Most honest to the tree shape, but `objects.<iid>`
//       would collide with `layers.<iid>` and `guides.<iid>` for
//       the same iid — both addressing surfaces work but the
//       proxies differ. The router would need to handle precedence.
//
//   (ii) Only non-layer/guide/ref/measure SceneNodes — "real scene
//        contents only." Cleanest non-overlap with the four existing
//        Scriptables. Expandable to (i) later if scripts genuinely
//        need layer-typed nodes via `objects`.
//
//   (iii) Only the active layer's children. Mirrors how current
//         selection / tools think. Most restrictive; possibly too
//         restrictive — scripts that want to walk the whole document
//         can't.
//
// **m1 picks (ii).** Clean separation with `layers` / `guides`; the
// types `objects` covers are exactly those a user would call "scene
// contents." The doc tree walks every layer's children (not just the
// active layer's), so cross-layer scripts work without a layer-
// switching dance. If a future script genuinely needs to address a
// Layer node via `objects`, (i) is reachable from here — extend
// is_scene_object() in the .cpp; no API change.
//
// Types in scope for `objects`:
//   - Path             — vector-shape leaf, the most common case
//   - Group            — container of arbitrary scene children
//   - Compound         — boolean-result container (path-shaped output)
//   - ClipGroup        — clip-shape + clipped children
//   - Blend            — interpolation between two source nodes
//   - Warp             — envelope-deformed source
//   - Text             — text leaf
//   - Image            — raster leaf
//   - Ref              — reference-point marker (lives under RefLayer)
//   - Measurement      — measurement leaf (lives under MeasureLayer)
//
// Types explicitly OUT of scope (their own Scriptable, or no
// addressability at this tier):
//   - Layer            — owned by `layers`
//   - Guide            — owned by `guides`
//   - GuideLayer       — special structural container; owned by `guides`
//                        implicitly via guide_layer()
//   - RefLayer         — special structural container; would belong to
//                        a future `refs` Scriptable
//   - MeasureLayer     — special structural container; would belong
//                        to a future `measurements` Scriptable
//   - GridLayer        — special structural container; doc-level
//                        property, no per-row addressability
//   - MarginLayer      — special structural container; doc-level
//                        property, no per-row addressability
//
// The Ref / Measurement types ARE in scope despite their parents
// being special layers. The reasoning: a Ref or a Measurement is a
// leaf with its own iid, position, and per-instance properties; a
// script wanting to read those properties does so via `objects.<iid>`.
// Their parent containers (RefLayer / MeasureLayer) get their own
// future Scriptable surfaces; the leaves answer to `objects` because
// they're concrete scene leaves with stable identity.
//
// ── Addressing ───────────────────────────────────────────────────────────
//
//   objects                  — the collection Scriptable. Set queries
//                              (count, all_iids, find_by_name,
//                              find_by_type) plus the structural
//                              verbs `new` / `delete`. ONE registry
//                              entry per app session; held as a
//                              member by MainWindow, registered for
//                              the lifetime of the window.
//
//   objects.<iid>            — per-instance proxy, materialised on
//                              demand by the collection. Reads the
//                              specific node's properties through the
//                              project's iid index. NOT registered —
//                              the registry only knows about `objects`.
//                              A `list` from the Scripter shows
//                              `objects` but never shows `objects.<iid>`
//                              entries (same property as the five
//                              shipped Scriptables).
//
// ── Lifetime and the project pointer ─────────────────────────────────────
//
// Same shape as LayersScriptable / GuidesScriptable: a project getter
// (std::function<CurvzProject*()>) resolved fresh on every verb call,
// not a captured raw pointer. MainWindow's m_project unique_ptr gets
// reset and reassigned at runtime (close-project, load-project); the
// getter survives that, the Scriptable does not need re-registration.
//
// MainWindow constructs the ObjectsScriptable once (after m_project
// is initialised in setup_project) and the registry holds the entry
// for the window's lifetime.
//
// ── The proxy ────────────────────────────────────────────────────────────
//
// ObjectProxy is the per-instance Scriptable materialised by
// proxy_for(iid). Same lifetime contract as LayerProxy / GuideProxy:
// transient, unregistered, owned by the listener's ResolvedObject
// wrapper for the duration of one statement. Holds the iid as the
// stable handle; resolves through curvz::utils::find_by_iid every
// query (no cached SceneNode pointer — that would dangle if the
// underlying iid index rebuilds or the node moves).
//
// COLLECTION QUERIES (on `objects`):
//   count          — int          number of in-scope scene objects
//                                  in the active document, summed
//                                  recursively across all layers.
//   all_iids       — string       comma-separated iid list in walk
//                                  order. Empty string if count == 0.
//                                  No trailing comma. Walk order is
//                                  depth-first within each top-level
//                                  layer, layers in their natural
//                                  vector order.
//
// COLLECTION VERBS (on `objects`):
//   new "<type>"                 — Creates a new in-scope scene object
//                                  of the given type and inserts it
//                                  at the front of the active layer's
//                                  children. Returns the new node's
//                                  iid as a string (binds to `result`,
//                                  consumable by `set <var> to result`
//                                  in scripts). Pushes an
//                                  AddNodeCommand so Ctrl+Z removes
//                                  the new node.
//
//                                  Type vocabulary in m2:
//                                    "path"  — Path leaf with empty
//                                              PathData (no nodes,
//                                              not closed). Renders
//                                              as nothing; addressable
//                                              for property queries.
//                                    "group" — Group container with
//                                              no children. Renders
//                                              as nothing; addressable.
//
//                                  Returns "" on miss (no active doc,
//                                  no active layer, unknown type
//                                  token, args.empty()).
//
//                                  Name policy: NEW NODES ARE MINTED
//                                  WITH AN EMPTY NAME. Different from
//                                  canvas-tool creation which assigns
//                                  next_default_name (`"Path 1"` etc.).
//                                  Empty name is the "user hasn't
//                                  named me yet" sentinel — `assert
//                                  objects.<iid> name == ""` is the
//                                  deterministic post-create check.
//                                  m3's `rename` verb is how a script
//                                  assigns a name.
//
//                                  Insertion: front of the active
//                                  layer's children vector. Matches
//                                  the canvas convention (most tools
//                                  insert-at-front so the new node
//                                  draws on top).
//
//   delete "<iid>"               — Removes the in-scope scene object
//                                  identified by iid from its parent
//                                  container, wherever in the tree
//                                  that container lives. Pushes a
//                                  DeleteObjectCommand so Ctrl+Z
//                                  restores the node at its original
//                                  index.
//
//                                  Returns null (no result binding).
//                                  Returns null on miss (no project,
//                                  no active doc, args.empty(), iid
//                                  doesn't resolve to an in-scope
//                                  scene object, the resolved node
//                                  has no findable parent container).
//
//                                  Scrubs the global undo stack for
//                                  raw-pointer-capture references to
//                                  the about-to-be-deleted node and
//                                  its descendants — same defensive
//                                  walk LayersScriptable::delete does.
//
//   find_by_name "<name>"        — iid of the first node in the doc
//                                  tree with name matching exactly,
//                                  or "" on miss. Names aren't unique
//                                  by construction (the document name
//                                  uniquifier on load only normalises
//                                  load-time collisions but live
//                                  collisions can happen);
//                                  "first hit in walk order" is the
//                                  contract — matches
//                                  LayersScriptable's
//                                  find_by_name semantics.
//
//   find_by_type "<type>"        — iid of the first node whose type
//                                  matches, or "" on miss. Type
//                                  tokens are lowercase strings:
//                                  "path", "group", "compound",
//                                  "clipgroup", "blend", "warp",
//                                  "text", "image", "ref",
//                                  "measurement". Walk order and
//                                  "first hit" same as find_by_name.
//                                  Useful for smoke tests that need
//                                  to land an iid of a known type
//                                  without pre-knowing one.
//
// ELEMENT QUERIES (on `objects.<iid>`):
//   name           — string       node.name. Empty for newly-created
//                                  nodes (the contract above); user-
//                                  named once a m3+ `rename` verb
//                                  runs.
//   type           — string       lowercase type token (same
//                                  vocabulary as find_by_type:
//                                  "path", "group", "compound",
//                                  "clipgroup", "blend", "warp",
//                                  "text", "image", "ref",
//                                  "measurement"). Stable across
//                                  the Scriptable's lifetime — the
//                                  string vocabulary is part of the
//                                  contract.
//   visible        — bool         node.visible
//   locked         — bool         node.locked
//   parent_iid     — string       internal_id of the node's parent
//                                  container (Layer / Group /
//                                  Compound / ClipGroup / Blend /
//                                  Warp / RefLayer / MeasureLayer).
//                                  The parent IS the owning slot;
//                                  for a Path that lives directly
//                                  under a Layer, parent_iid is the
//                                  Layer's iid. For a Path inside a
//                                  Group inside a Layer,
//                                  parent_iid is the Group's iid.
//                                  Returns "" if the parent walk
//                                  doesn't find the node (shouldn't
//                                  happen for a node that resolved
//                                  through find_by_iid — defensive).
//   child_count    — int          node.children.size(). Always
//                                  defined; 0 for leaf types (Path,
//                                  Text, Image, Ref, Measurement)
//                                  in practice, but the field
//                                  reads from the live vector so
//                                  even if someone abuses a leaf
//                                  with children it still answers.
//   child_index    — int          node's position within its parent's
//                                  `children` vector. -1 if the node
//                                  is owned by a non-children slot
//                                  (clip_shape, blend_source_*,
//                                  warp_source) — structurally
//                                  positioned, not list-indexed. Also
//                                  -1 if the parent walk can't find
//                                  a slot at all (shouldn't happen
//                                  for a node that resolved through
//                                  find_by_iid — defensive). Used by
//                                  scripts to observe `move` round-
//                                  trips (s234 m4).
//   iid            — string       node.internal_id (for completeness:
//                                  a script can `get objects.<iid>
//                                  iid` to verify it has the right
//                                  handle, useful in tests).
//
// ELEMENT MUTATING VERBS (on `objects.<iid>`):
//   toggle_visible                 — flip visible (push EditObjectFieldCommand).
//                                    No-args.
//   set_visible <bool>             — write visible to the given bool.
//                                    No-op (no command pushed) when the
//                                    new value equals the current one —
//                                    same behaviour as LayerProxy.
//   toggle_locked                  — flip locked (push EditObjectFieldCommand).
//   set_locked  <bool>             — write locked to the given bool.
//                                    No-op when value unchanged.
//   rename "<name>"                — write name to the given string
//                                    (push EditObjectFieldCommand).
//                                    Empty-string arg AND name-unchanged
//                                    are both no-ops. The "" sentinel
//                                    here means "no rename happened";
//                                    scripts that genuinely want to
//                                    clear a name to the "" empty
//                                    sentinel (matching the newly-
//                                    minted state from `objects new`)
//                                    can't do so via `rename` today —
//                                    that's the same constraint
//                                    LayerProxy::rename has and matches
//                                    the LayersPanel's "empty entry
//                                    leaves the name alone" UX
//                                    convention. Worth a future
//                                    `clear_name` if scripts need it.
//
//   All five push EditObjectFieldCommand on the global undo stack on
//   success. Command bodies resolve iid → SceneNode every replay
//   (find_by_iid, project-wide); deleted-between-push-and-replay
//   degrades to a silent skip, same partial-recovery shape
//   EditLayerFieldCommand uses.
//
// ELEMENT STRUCTURAL VERBS (on `objects.<iid>`, s234 m4):
//   move "<direction>"             — reorder within parent's `children`.
//                                    Direction token: "up" (idx-1,
//                                    visually toward top), "down" (idx+1,
//                                    visually toward bottom), "top"
//                                    (idx=0), "bottom" (idx=last).
//                                    Mirrors LayersScriptable's `move`
//                                    direction vocabulary. Pushes
//                                    MoveNodeIndexCommand (single-target,
//                                    iid-keyed; NOT ZOrderCommand —
//                                    that command keys on SVG `id`
//                                    which is empty for script-minted
//                                    nodes and would collide). No-ops:
//                                    unknown direction, parent has
//                                    only one child, target already
//                                    at destination edge ("up" at
//                                    top, "down" at bottom).
//                                    Node owned by a non-children
//                                    slot is also a no-op (structural
//                                    inputs aren't list-positioned).
//   reparent "<new_parent_iid>"    — move the node into the named
//                                    container's `children` (front-
//                                    insert: idx 0, on top). Pushes
//                                    MoveObjectToLayerCommand (the
//                                    name is layer-historical;
//                                    mechanically the command is
//                                    container-agnostic). Refusals
//                                    (all no-ops): dest iid empty
//                                    or doesn't resolve; dest is a
//                                    leaf type (Path / Text / Image
//                                    / Ref / Measurement — can't
//                                    hold children); dest equals
//                                    self; dest is a descendant of
//                                    self (cycle); source is owned
//                                    by a non-children slot (clip_shape
//                                    / blend_source_* / warp_source —
//                                    same refusal `delete` uses for
//                                    the same reason).
//   duplicate                      — clone the node and insert the
//                                    clone at the original's slot
//                                    (the duplicate ends up on top
//                                    of the original; matches canvas
//                                    alt-drag-duplicate convention).
//                                    Pushes DuplicateCommand. Returns
//                                    the new iid as ScriptValue::text;
//                                    a script binds via `set <var>
//                                    to result` and immediately
//                                    addresses the duplicate. The
//                                    clone has a FRESH internal_id
//                                    (and a fresh iid on every
//                                    descendant in the subtree) so
//                                    the iid index doesn't collide.
//                                    Name is preserved as-is (no
//                                    " (2)" suffix machinery; the
//                                    script can rename via the m3
//                                    rename verb if desired).
//                                    No-ops: source owned by a non-
//                                    children slot (same refusal
//                                    shape as `delete` / `move` /
//                                    `reparent`).
//
//   (m5 closes the arc with collection-level grouping: group, ungroup.)
//
// ── On the active-document scope ─────────────────────────────────────────
//
// Same shape as LayersScriptable / GuidesScriptable: collection
// queries operate on project->active_doc(); element queries resolve
// through curvz::utils::find_by_iid(project, iid), which walks every
// document. So `objects count` counts the ACTIVE doc, but `get
// objects.<iid> name` finds the iid in WHICHEVER doc holds it. The
// iid is the address; the doc is just where it lives.
//
// `new` creates into the active doc's active layer specifically;
// scripts that want to add to a different layer's children today must
// activate that layer first (`layers.<iid> activate`). That's the
// same constraint canvas tools observe.
//
// `delete` finds the iid wherever it lives across documents — the
// same project-wide find_by_iid resolution element queries use.
// Symmetric with `layers delete`, which also resolves by iid first
// and only then locates the owning doc.
//
// ── Why no rename and no find_by_iid query ──────────────────────────────
//
// Rename: that's an element mutating verb (m3). Reading via `get
// objects.<iid> name` covers the read side.
//
// find_by_iid: not a query — it's the resolver itself. A script that
// has an iid in hand goes straight to `objects.<iid>` addressing; no
// query needed. `find_by_name` and `find_by_type` exist because
// scripts often want to start from a property (a known name, a
// known type) and bind an iid; `find_by_iid` would be a no-op
// (iid in, iid out).
//
// ── Diagnostic-only ──────────────────────────────────────────────────────
//
// This header is included only from .cpp files compiled in the
// CURVZ_DIAGNOSTIC build; production builds don't see it. MainWindow's
// ObjectsScriptable member is held behind the same scripting-TU
// compilation gate as the five sibling members (always-compiled as of
// s219 m1).

#pragma once
#include "scripting/Scriptable.hpp"
#include "scripting/ScriptValue.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Curvz {
struct CurvzProject;
class  CommandHistory;
}

namespace curvz::scripting {

class ObjectsScriptable : public Scriptable {
public:
    using ProjectGetter    = std::function<Curvz::CurvzProject*()>;
    using NotifyDocChanged = std::function<void()>;

    // Registers as "objects" via the Scriptable base ctor. The project
    // getter is called fresh on every verb invocation — never cached —
    // so MainWindow can swap its m_project unique_ptr freely (open
    // another project, close project) without invalidating us. The
    // getter is allowed to return nullptr; queries degrade gracefully
    // (count returns 0, all_iids returns "", find_by_* returns "").
    //
    // `history` is non-owning and may be nullptr. Captured in m1 for
    // shape-symmetry; ACTIVELY USED from m2 onwards — the `new` and
    // `delete` verbs push AddNodeCommand / DeleteObjectCommand on
    // every successful invocation, and m3's element mutating verbs
    // (toggle_visible / set_visible / toggle_locked / set_locked /
    // rename) push EditObjectFieldCommand. m4's element structural
    // verbs (move / reparent / duplicate) push MoveNodeIndexCommand /
    // MoveObjectToLayerCommand / DuplicateCommand respectively. A
    // null history degrades every verb to direct mutation without
    // undo support (the same graceful-degradation pattern guides uses).
    //
    // `notify_doc_changed` is the s235 m1 canvas-refresh callback.
    // Called at the end of every successful mutating verb (collection
    // and proxy alike) AFTER the command push. MainWindow supplies
    // a lambda that runs `m_canvas.queue_draw(); m_canvas.
    // notify_document_changed();` — the canonical two-call shape for
    // external mutators (mirrors s227 m1's themes_refresh_cascade).
    // May be nullptr; verbs silently skip the refresh step in that
    // case (same graceful-degradation shape as the history pointer).
    ObjectsScriptable(ProjectGetter get_project,
                      Curvz::CommandHistory* history,
                      NotifyDocChanged notify_doc_changed = {});
    ~ObjectsScriptable() override = default;

    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

    // Router hooks — see CANON's "Row-bound model Scriptables".
    bool can_resolve(std::string_view key) const override;
    std::unique_ptr<Scriptable> proxy_for(std::string_view key) override;

private:
    ProjectGetter          m_get_project;
    Curvz::CommandHistory* m_history;   // non-owning; captured in m1
                                        // for shape-symmetry, actively
                                        // used from m2 onwards by `new`
                                        // and `delete`, from m3 onwards
                                        // by the element mutating verbs
                                        // on the proxy, and from m4
                                        // onwards by the element
                                        // structural verbs (move /
                                        // reparent / duplicate). Null
                                        // history degrades every verb
                                        // to direct mutation.
    NotifyDocChanged       m_notify_doc_changed;  // s235 m1 — canvas-
                                        // refresh callback. Threaded
                                        // into ObjectProxy via the
                                        // proxy ctor so element verbs
                                        // can fire the cascade too.
                                        // May be empty; verbs skip the
                                        // refresh step in that case.
};

} // namespace curvz::scripting
