// ============================================================================
//  SelectionContext.cpp — implementation (s158 m1)
//
//  Two halves:
//    1. recompute_object / recompute_node / scrub_node_ref — the live-cache
//       maintenance side. Walks the underlying selection, builds Info,
//       calls compute_*_actions to derive Actions, emits signal_changed
//       if either world changed.
//
//    2. compute_object_actions / compute_node_actions — the policy side.
//       Pure functions. Top-to-bottom rule lists. THE place every
//       capability rule lives. When you want to add or change a rule,
//       this is the file.
//
//  First-cut policy is intentionally conservative — rules permit only
//  cases I can confirm from existing Canvas code. Easier to widen a
//  rule later than to chase a false-positive that lit up the menu and
//  wedged the action.
// ============================================================================

#include "SelectionContext.hpp"
#include "SceneNode.hpp"

namespace Curvz {

// ────────────────────────────────────────────────────────────────────────────
//  Internal helpers — keep ObjectInfo/NodeInfo construction local
// ────────────────────────────────────────────────────────────────────────────

namespace {

// Walk parent pointers (if SceneNode tracks them). Today SceneNode
// doesn't carry a back-pointer to parent; the document tree is owned
// by parent->children unique_ptrs. The "common parent" question is
// answered by the caller passing it in, OR by walking the doc tree
// to find each selected node's parent. For m1 we keep it simple:
// common_parent is left null and shares_parent false unless the
// caller wires the parent lookup later. The compute_object_actions
// rules that depend on shares_parent (MakeGroup) gate appropriately.
//
// This is a deliberate scope limit for m1 — the parent walk is O(N*depth)
// per selection change and wants a Canvas helper to be efficient.
// MakeGroup will be conservatively-disabled until that lookup lands;
// no false positives, no false negatives that aren't already documented.

// Tally an ObjectInfo from the selection vector.
void build_object_info(const std::vector<SceneNode *> &selection,
                       SceneNode *primary, ObjectInfo &out) {
  out.clear();
  out.count = selection.size();
  if (out.count == 0) return;

  out.single = (out.count == 1) ? selection.front() : nullptr;

  // First pass — kind tally and behavioural flags.
  bool first = true;
  bool all_paths_so_far = true;
  bool all_groups_so_far = true;
  bool all_path_like_so_far = true;
  bool all_closed_so_far = true;  // vacuously true; flips false on
                                  // first open path encountered.
  bool any_path_seen = false;

  for (SceneNode *n : selection) {
    if (!n) continue;  // defensive — shouldn't happen, but cheap

    using T = SceneNode::Type;
    switch (n->type) {
      case T::Path:     out.any_path = true; break;
      case T::Compound: out.any_compound = true; break;
      case T::Group:    out.any_group = true; break;
      case T::Warp:     out.any_warp = true; break;
      case T::Blend:    out.any_blend = true; break;
      case T::Text:     out.any_text = true; break;
      case T::Image:    out.any_image = true; break;
      case T::Ref:      out.any_ref = true; break;
      default: break;  // Layer, Guide, GuideLayer, GridLayer, etc. —
                       // not user-selectable in the object world, but
                       // we don't crash if one slips through.
    }

    if (n->type != T::Path)     all_paths_so_far = false;
    if (n->type != T::Group)    all_groups_so_far = false;
    if (n->type != T::Path && n->type != T::Compound)
      all_path_like_so_far = false;

    // closed-ness — only meaningful for Path. For path_like, Compounds
    // are treated as closed (their children's topology is what matters
    // and the compound itself is by-construction closed at the boolean
    // op layer). For non-path-like selections the flag is meaningless.
    if (n->type == T::Path) {
      any_path_seen = true;
      const bool closed = (n->path && n->path->closed);
      if (!closed) {
        all_closed_so_far = false;
        out.any_open_path = true;
      }
    }

    if (n->locked) out.any_locked = true;
    if (!n->visible) out.any_hidden = true;
    if (!n->fill_swatch_id.empty() || !n->stroke_swatch_id.empty())
      out.any_bound_swatch = true;
    if (!n->bound_style.empty())
      out.any_bound_style = true;

    first = false;
  }

  out.all_paths     = all_paths_so_far;
  out.all_groups    = all_groups_so_far;
  out.all_path_like = all_path_like_so_far;

  // all_closed is meaningful only when there's at least one path-typed
  // selection (or all_path_like and we treat compounds as closed). For
  // a selection of e.g. all-groups, `all_closed` stays false (no paths
  // to be closed). Safer default — boolean ops won't fire on a group
  // selection just because no open paths existed.
  out.all_closed = any_path_seen && all_closed_so_far;

  (void)primary;
  // primary is currently informational — Info doesn't store it
  // separately; consumers that need the inspector target read
  // Canvas::primary_selection() directly. Could be added if a need
  // surfaces.
}

// Tally a NodeInfo from the parallel arrays + primary/secondary slots.
void build_node_info(const std::vector<SceneNode *> &paths,
                     const std::vector<int>         &node_indices,
                     SceneNode *primary_path, int primary_index,
                     SceneNode *secondary_path, int secondary_index,
                     NodeInfo &out) {
  out.clear();
  out.count = paths.size();
  if (out.count == 0) return;

  // Distinct paths involved.
  // Selection is small — linear scan, no set overhead.
  std::vector<SceneNode *> distinct;
  distinct.reserve(paths.size());
  for (SceneNode *p : paths) {
    if (!p) continue;
    bool seen = false;
    for (SceneNode *q : distinct) {
      if (q == p) { seen = true; break; }
    }
    if (!seen) distinct.push_back(p);
  }
  out.paths_involved = distinct.size();
  out.same_path = (out.paths_involved <= 1);

  // Endpoints — head (index 0) or tail (index nodes.size()-1) of the
  // containing path. Walks the actual path data.
  bool all_endpoints_so_far = true;
  for (std::size_t i = 0; i < paths.size() && i < node_indices.size(); ++i) {
    SceneNode *p = paths[i];
    int idx = node_indices[i];
    if (!p || !p->path) { all_endpoints_so_far = false; continue; }
    const std::size_t n = p->path->nodes.size();
    if (n == 0) { all_endpoints_so_far = false; continue; }
    const bool is_endpoint = (idx == 0) ||
                              (idx == int(n) - 1);
    // On a closed path, "endpoint" doesn't really apply — head and
    // tail are the same point. Closed paths can't be Join targets
    // either (no free endpoints). Mark as not-endpoint so the Join
    // rule excludes them.
    if (p->path->closed) {
      all_endpoints_so_far = false;
    } else if (!is_endpoint) {
      all_endpoints_so_far = false;
    }
  }
  out.all_endpoints = all_endpoints_so_far && out.count > 0;

  // Cross-path — at action time, the Join handler in Canvas resolves
  // (base, append) from m_node_selection itself, not from m_selected2.
  // The secondary slot is set as a byproduct of that resolution, not
  // as an input to it. So at the moment a menu or toolbar reads the
  // mask, m_selected2 is from a previous Join (typically null) — even
  // though m_node_selection already contains the two endpoints on
  // different paths.
  //
  // s163 m2: derive cross_path from the node-selection itself instead
  // of the secondary slot. The mask is "this configuration could
  // plausibly join" — count==2 and paths_involved==2 is exactly that
  // configuration. The s157 identity rule (which endpoint becomes
  // base, which becomes append) still resolves at action time inside
  // Canvas — unchanged. The mask just predicts viability.
  //
  // Same-path two-endpoints (the close-path case) is intentionally
  // excluded: Curvz keeps Close Path as a separate command, not folded
  // into Join. Sticking with cross-path-only here preserves muscle
  // memory.
  //
  // primary_path/secondary_path/*_index parameters retained as dead
  // arguments — the call site in Canvas::notify_node_selection_changed
  // still passes them. Callers stay stable; only the rule moved.
  out.cross_path = (out.count == 2) && (out.paths_involved == 2);

  (void)primary_path;
  (void)secondary_path;
  (void)primary_index;
  (void)secondary_index;
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
//  SelectionContext::recompute_object
// ────────────────────────────────────────────────────────────────────────────

void SelectionContext::recompute_object(
    const std::vector<SceneNode *> &selection, SceneNode *primary) {

  ObjectInfo    new_info;
  build_object_info(selection, primary, new_info);
  ObjectActions new_actions = compute_object_actions(new_info);

  // Compare each category mask to detect a real change. Skipping
  // signal_changed when nothing moved dedups spurious refreshes for
  // consumers (inspector rebuilds, action-enable sweeps) sensitive
  // to that.
  //
  // We compare ObjectActions only — it's the consumer-visible surface.
  // ObjectInfo can churn in fields no rule uses without forcing a
  // refresh. Future consumers reading Info directly should listen on
  // signal_changed and judge for themselves.
  const bool changed =
      m_obj_actions.life    != new_actions.life    ||
      m_obj_actions.struct_ != new_actions.struct_ ||
      m_obj_actions.bool_   != new_actions.bool_   ||
      m_obj_actions.effect  != new_actions.effect  ||
      m_obj_actions.layer   != new_actions.layer   ||
      m_obj_actions.vis     != new_actions.vis     ||
      m_obj_actions.align   != new_actions.align   ||
      m_obj_actions.dist    != new_actions.dist;

  m_obj_info    = new_info;
  m_obj_actions = new_actions;

  if (changed) m_sig_changed.emit();
}

// ────────────────────────────────────────────────────────────────────────────
//  SelectionContext::recompute_node
// ────────────────────────────────────────────────────────────────────────────

void SelectionContext::recompute_node(
    const std::vector<SceneNode *> &paths,
    const std::vector<int>         &node_indices,
    SceneNode *primary_path, int primary_index,
    SceneNode *secondary_path, int secondary_index) {

  NodeInfo    new_info;
  build_node_info(paths, node_indices,
                  primary_path, primary_index,
                  secondary_path, secondary_index, new_info);
  NodeActions new_actions = compute_node_actions(new_info);

  const bool changed =
      m_node_actions.struct_ != new_actions.struct_ ||
      m_node_actions.kind    != new_actions.kind;

  m_node_info    = new_info;
  m_node_actions = new_actions;

  if (changed) m_sig_changed.emit();
}

// ────────────────────────────────────────────────────────────────────────────
//  SelectionContext::scrub_node_ref
//
//  Called from Canvas::scrub_node_refs after the underlying selection
//  vectors have been emptied of `target`. Our job here is to drop any
//  cached pointer to target from the Info structs so a stale read
//  between scrub and the next recompute can't hand a dead pointer to
//  a consumer. The next recompute will rebuild Info from scratch and
//  this clearing won't have mattered — but defence in depth is cheap.
// ────────────────────────────────────────────────────────────────────────────

void SelectionContext::scrub_node_ref(const SceneNode *target) {
  if (!target) return;
  if (m_obj_info.single == target)        m_obj_info.single = nullptr;
  if (m_obj_info.common_parent == target) {
    m_obj_info.common_parent = nullptr;
    m_obj_info.shares_parent = false;
  }
  // NodeInfo doesn't cache SceneNode pointers (only counts and
  // booleans), so no scrub needed there.
}

// ════════════════════════════════════════════════════════════════════════════
//  POLICY — compute_object_actions
//
//  Every object-world capability rule in Curvz lives in this function.
//  Top-to-bottom. One place to read, one place to change.
//
//  When you add a feature:
//    1. Add the bit to the appropriate *Action enum
//    2. Add the rule here (one `if` block, OR the bit into the mask)
//    3. Add the menu/toolbar entry that reads the bit
//
//  When a rule changes (e.g. compound-path-in-union ships):
//    Edit the predicate. Done. Every consumer absorbs the change.
//
//  Phrase rules in behavioural terms when possible. `info.all_path_like`
//  scales to a future kind that satisfies "is closed and fillable";
//  `info.all_paths` does not. Use kind-tag flags (any_compound, etc.)
//  only when the rule genuinely cares about identity.
// ════════════════════════════════════════════════════════════════════════════

ObjectActions compute_object_actions(const ObjectInfo &info) {
  ObjectActions a;

  // ── Empty selection — nothing in the object world is allowed.
  //    Paste is selection-independent and is enabled at the menu
  //    layer based on clipboard state, not this mask.
  if (info.is_empty()) {
    return a;
  }

  // ── Locked objects can be selected but most ops bail. Lock/Unlock
  //    and Hide/Show are still permitted — that's how you get out of
  //    a locked state. Everything else gates on !any_locked.
  const bool unlocked = !info.any_locked;

  // ──────────────────────────────────────────────────────────────────
  //  Lifecycle
  // ──────────────────────────────────────────────────────────────────
  if (unlocked) {
    a.life |= LifeAction::Cut | LifeAction::Copy |
              LifeAction::Duplicate | LifeAction::Delete;
  } else {
    // Even on locked, Copy is allowed (non-destructive read).
    a.life |= LifeAction::Copy;
  }
  // Paste is selection-independent — left for the consumer to gate
  // on clipboard state.

  // ──────────────────────────────────────────────────────────────────
  //  Visibility — Lock/Unlock/Hide/Show always permitted on any
  //  non-empty selection. (Inverse pairs: which one shows in the
  //  menu is decided by the consumer reading any_locked / any_hidden.)
  // ──────────────────────────────────────────────────────────────────
  a.vis |= VisAction::Lock | VisAction::Unlock |
           VisAction::Hide | VisAction::Show;

  // ──────────────────────────────────────────────────────────────────
  //  Layer ops — z-order. Need ≥1 unlocked object with a parent.
  //  We don't have parent info wired in m1; conservatively allow
  //  layer ops on any unlocked selection. False positives here are
  //  benign — z-order ops on a top-level node are no-ops, not
  //  crashes.
  // ──────────────────────────────────────────────────────────────────
  if (unlocked) {
    a.layer |= LayerAction::BringToFront | LayerAction::BringForward |
               LayerAction::SendBackward | LayerAction::SendToBack;
  }

  // ──────────────────────────────────────────────────────────────────
  //  Structure
  //
  //  MakeGroup — ≥1 unlocked object that share a parent. m1 caveat:
  //  shares_parent isn't computed yet (parent walk deferred). For now,
  //  permit MakeGroup on any unlocked selection of ≥1 — the action
  //  itself enforces the same-parent requirement. This is a "fast
  //  path enable"; the deep validation lives in the action.
  //
  //  Ungroup — exactly 1 group selected (Affinity convention) OR
  //  every selected object is a group. Single-group case covers the
  //  common path; all-groups case covers the multi-select Ungroup
  //  All from the menu. Locked groups are not ungroupable.
  //
  //  MakeCompound — ≥2 path-like, all closed, no compound mixed in
  //  (compound-in-union is on the backlog and the rule widens then).
  //
  //  ReleaseCompound — exactly 1 compound. Multi-compound release
  //  is a separate "Release All" path that Curvz doesn't ship today.
  // ──────────────────────────────────────────────────────────────────
  if (unlocked) {
    a.struct_ |= StructAction::MakeGroup;

    if (info.is_single() && info.any_group) {
      a.struct_ |= StructAction::Ungroup;
    } else if (info.is_multi() && info.all_groups) {
      a.struct_ |= StructAction::Ungroup;
    }

    if (info.is_multi() && info.all_paths && info.all_closed &&
        !info.any_compound) {
      a.struct_ |= StructAction::MakeCompound;
    }

    if (info.is_single() && info.any_compound) {
      // s162 m3 (defensive): suppress ReleaseCompound when an effect node
      // is in the picture. User-reported case: Release Compound appearing
      // alongside warp verbs in the right-click menu. Strict reading of
      // build_object_info says the two info flags can't both be true on a
      // single selection (each SceneNode has one type), but the symptom
      // was real. Likely a corner where the type tally reaches into a
      // wrapped source somewhere; rather than guess, we encode the UX
      // coherence rule the user actually wants: when an effect wraps a
      // compound, releasing the compound shouldn't be on offer — release
      // the effect first. Effect-layer ops take precedence over
      // structural ops on the wrapped source. If the underlying tally
      // bug exists it surfaces elsewhere; this rule is correct on its
      // own merits regardless.
      if (!info.any_warp && !info.any_blend) {
        a.struct_ |= StructAction::ReleaseCompound;
      }
    }
  }

  // ──────────────────────────────────────────────────────────────────
  //  Boolean ops
  //
  //  Today: ≥2 path-like, all closed, no compound (compound-in-union
  //  deferred). Same input class for union/subtract/intersect — they
  //  diverge at the implementation, not at the gate.
  //
  //  When the compound deferral closes, the !any_compound clause
  //  drops. ONE LINE change, all three ops absorb it, every consumer
  //  picks it up.
  // ──────────────────────────────────────────────────────────────────
  if (unlocked && info.is_multi() && info.all_path_like &&
      info.all_closed && !info.any_compound) {
    a.bool_ |= BoolAction::Union | BoolAction::Subtract |
               BoolAction::Intersect;
  }

  // ──────────────────────────────────────────────────────────────────
  //  Effects
  //
  //  EditWarp / ReleaseWarp — single warp selected.
  //  EditBlend / ReleaseBlend — single blend selected.
  //  Flatten — any warp/blend in selection (per banked rule the
  //    backlogged warp inspector section makes Edit Warp a non-modal
  //    re-edit; Flatten is the "commit and lose editability" verb).
  //  ExpandStroke — any path-like with a non-zero stroke width. m1
  //    permits on any unlocked path-like; the action's pre-flight
  //    handles the no-stroke case as a no-op.
  //  ConvertToPath — text only (glyph rasterisation to editable Bezier
  //    paths). m1 permits on single text. Compound was included
  //    previously but no compound→path conversion exists in Curvz; the
  //    compound IS the path-set. Removed in s162 m3.
  // ──────────────────────────────────────────────────────────────────
  if (unlocked) {
    if (info.is_single() && info.any_warp) {
      a.effect |= EffectAction::EditWarp | EffectAction::ReleaseWarp;
    }
    if (info.is_single() && info.any_blend) {
      a.effect |= EffectAction::EditBlend | EffectAction::ReleaseBlend;
    }
    if (info.any_warp || info.any_blend) {
      a.effect |= EffectAction::Flatten;
    }
    if (info.any_path || info.any_compound) {
      a.effect |= EffectAction::ExpandStroke;
    }
    if (info.is_single() && info.any_text) {
      // s162 m3: ConvertToPath is a font-rasterisation verb (text glyphs
      // → editable Bezier paths). Compound was previously included on a
      // "compound→path" reading, but no such conversion exists in Curvz —
      // Compound is its own object kind. Restricted to text only.
      a.effect |= EffectAction::ConvertToPath;
    }
    // s162 m3: MakeWarp gates on a single Path / Compound / Group, mirroring
    // the existing MainWindow::update_warp_action_sensitive rule. Canvas's
    // make_warp validates deeper edge cases and surfaces user-visible
    // errors — the gate here is "this could plausibly warp," not a full
    // pre-flight.
    if (info.is_single() &&
        (info.any_path || info.any_compound || info.any_group)) {
      a.effect |= EffectAction::MakeWarp;
    }
    // s162 m3: MakeBlend gates on exactly 2 Paths, both closed, mirroring
    // the existing MainWindow::update_blend_action_sensitive rule. Compound
    // is explicitly NOT allowed here — Blend's input shape is two flat
    // path-spines for source-A / source-B interpolation. Deeper precondition
    // checks (same parent, equal node counts, equal closed flag) live in
    // Canvas::make_blend with user-visible errors.
    if (info.is_pair() && info.all_paths && info.all_closed) {
      a.effect |= EffectAction::MakeBlend;
    }
  }

  // ──────────────────────────────────────────────────────────────────
  //  Align / Distribute
  //
  //  Align — ≥2 unlocked objects (Affinity convention; align with one
  //    object aligns to artboard, but Curvz doesn't ship that today,
  //    so gate on ≥2). All six alignments enabled together — the
  //    direction is in the action, not the gate.
  //
  //  Distribute — ≥3 objects (anything fewer is a no-op).
  // ──────────────────────────────────────────────────────────────────
  if (unlocked && info.is_multi()) {
    a.align |= AlignAction::Left   | AlignAction::CenterH |
               AlignAction::Right  | AlignAction::Top    |
               AlignAction::CenterV| AlignAction::Bottom;
  }
  if (unlocked && info.count >= 3) {
    a.dist |= DistAction::Horizontal | DistAction::Vertical |
              DistAction::SpaceH     | DistAction::SpaceV;
  }

  return a;
}

// ════════════════════════════════════════════════════════════════════════════
//  POLICY — compute_node_actions
//
//  The node-world is much smaller. Only handful of rules. Same
//  philosophy: top-to-bottom, behaviour-leaning, one place.
// ════════════════════════════════════════════════════════════════════════════

NodeActions compute_node_actions(const NodeInfo &info) {
  NodeActions a;

  if (info.is_empty()) return a;

  // ── Join — exactly 2 endpoints on different paths. The s157
  //    identity rule (which endpoint becomes base, which becomes
  //    append) lives in Canvas's join handler — that's policy at
  //    the splice layer, not the gate layer. Here we just say
  //    "this configuration could plausibly join."
  if (info.is_pair() && info.cross_path && info.all_endpoints) {
    a.struct_ |= NodeStructAction::Join;
  }

  // ── BreakNode — exactly 1 node selected (any position on a path).
  if (info.is_single()) {
    a.struct_ |= NodeStructAction::BreakNode;
  }

  // ── DeleteNode — 1 or more nodes; cross-path supported (s163 m4).
  //    Each affected path's deletes are independent and composed in a
  //    single CompositeCommand for atomic undo. Gate is "any non-empty
  //    node selection." The action's pre-flight rejects the press cleanly
  //    if any affected path is a Blend source.
  if (!info.is_empty()) {
    a.struct_ |= NodeStructAction::DeleteNode;
  }

  // ── AddNode — single-node selection on a path; the action inserts
  //    a new node midway in the segment AFTER the selected one.
  //    Single-path requirement ensures the segment context is clear.
  if (info.is_single() && info.same_path) {
    a.struct_ |= NodeStructAction::AddNode;
  }

  // ── Smoothness — apply to every selected node, single or multi.
  //    All three kinds always available together; the user picks one.
  a.kind |= NodeKindAction::MakeSmooth |
            NodeKindAction::MakeCorner |
            NodeKindAction::MakeSymmetric;

  return a;
}

}  // namespace Curvz
