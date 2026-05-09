#pragma once

// ============================================================================
//  SelectionContext — the canonical answer to "what's selected and what can
//  it do?" (s158 m1)
// ============================================================================
//
//  Design intent
//  -------------
//  Capability questions about the current selection (does the menu show
//  Ungroup? is the toolbar's Boolean Union button enabled? is Ctrl+G a
//  no-op right now?) used to be answered ad-hoc by every consumer that
//  cared — menus, toolbars, action-enable handlers, keystroke dispatchers,
//  inspector sections, status bar. Each rephrased the same logical
//  question in slightly different ways. Drift accumulated; rules diverged;
//  bugs hid in the gaps.
//
//  SelectionContext makes the question one-place: a small set of facts
//  about the selection (Info), feeding a small set of permission masks
//  (Actions), recomputed once per selection-change and broadcast on a
//  single signal. Every consumer reads the same masks. Drift is no
//  longer possible by construction.
//
//  Two scopes — Object world vs Node world
//  ---------------------------------------
//  Selection-tool selection (objects) and Node-tool selection (nodes
//  within a path) live in disjoint vocabularies. You can't Boolean-Union
//  a node, you can't Smooth-Corner an object. We model them as two
//  separate worlds with their own Info/Actions pairs, and consumers ask
//  the world that's relevant to them. The active tool decides which
//  world the context menu / inspector targets, but both worlds are
//  always live and accurate.
//
//  Bitmask layout — categorical, with `None` sentinel
//  --------------------------------------------------
//  Each Actions struct holds several small enum-class masks (uint64_t
//  underlying), one per action category (lifecycle, structure, boolean,
//  effect, etc.). Categories mirror UI groupings: the Structure menu
//  region polls the struct mask, the Boolean toolbar group polls the
//  bool mask, and so on. Locality of concern — consumers only test the
//  masks they care about.
//
//  Every category enum has `None = 0` so a default-constructed Actions
//  is empty and quick-checking "anything allowed in category X?" is one
//  word comparison against zero.
//
//  Mod-verifiable
//  --------------
//  Action methods (release_compound, make_group, etc.) can assert their
//  bit at entry: `assert(any(actions.struct_ & StructAction::Ungroup))`.
//  If anything bypasses SelectionContext and fires the action directly,
//  the assert catches it immediately. Cheap, structural, can't drift.
//
//  Living document, computed eagerly
//  ---------------------------------
//  SelectionContext is a member of Canvas. Canvas calls
//  recompute_object() / recompute_node() at every mutation site of the
//  underlying selection vectors (m_selection / m_node_selection). This
//  matches the existing m_sig_selection.emit() fanout pattern.
//
//  Compute is O(N) over selection size (always small) and runs at user
//  pace (clicks, marquees), so cost is unmeasurable. Don't memoise.
//
//  What this is NOT
//  ----------------
//  - Not a replacement for SceneNode itself.
//  - Not a place for tool-mode state (Pen vs Selection vs Node — that
//    sits above this).
//  - Not a place for canvas-mode state (mid-drag, mid-marquee).
//  - Not a place for doc-state (dirty, undo-available).
//  - Not a place for expensive analyses (deep boolean-op viability).
//    The masks are a *fast path enable* — "this could plausibly run."
//    The op's pre-flight does the real validation.

#include <cstdint>
#include <sigc++/sigc++.h>
#include <vector>

namespace Curvz {

// Forward declarations to keep this header light.
struct SceneNode;
class CurvzDocument;

// ────────────────────────────────────────────────────────────────────────────
//  Object-world action enums
//
//  Each is a uint64_t-underlying enum class with `None = 0` and bit
//  positions for each action. Operators below give bitwise composition
//  while preserving type safety (you can't | a BoolAction with a
//  StructAction — compile error).
// ────────────────────────────────────────────────────────────────────────────

// Lifecycle — clipboard and existence ops. Apply to almost any selection.
enum class LifeAction : std::uint64_t {
  None      = 0,
  Cut       = 1ULL << 0,
  Copy      = 1ULL << 1,
  Paste     = 1ULL << 2,  // selection-independent in principle, but tracked here
  Duplicate = 1ULL << 3,
  Delete    = 1ULL << 4,
};

// Structure — composition of objects (groups, compound paths).
enum class StructAction : std::uint64_t {
  None            = 0,
  MakeGroup       = 1ULL << 0,
  Ungroup         = 1ULL << 1,
  MakeCompound    = 1ULL << 2,
  ReleaseCompound = 1ULL << 3,
};

// Boolean ops — closed-path combinatorics.
enum class BoolAction : std::uint64_t {
  None      = 0,
  Union     = 1ULL << 0,
  Subtract  = 1ULL << 1,
  Intersect = 1ULL << 2,
  // future: Exclude, Divide
};

// Effect — warp / blend / stroke expansion / flatten.
enum class EffectAction : std::uint64_t {
  None           = 0,
  EditWarp       = 1ULL << 0,
  ReleaseWarp    = 1ULL << 1,
  EditBlend      = 1ULL << 2,
  ReleaseBlend   = 1ULL << 3,
  Flatten        = 1ULL << 4,
  ExpandStroke   = 1ULL << 5,
  ConvertToPath  = 1ULL << 6,  // text/shape primitives → editable path
  MakeWarp       = 1ULL << 7,  // s162 m3: applies a Warp to a single path-like
  MakeBlend      = 1ULL << 8,  // s162 m3: blends a closed path-like pair
};

// Layer — z-order ops within parent.
enum class LayerAction : std::uint64_t {
  None            = 0,
  BringToFront    = 1ULL << 0,
  BringForward    = 1ULL << 1,
  SendBackward    = 1ULL << 2,
  SendToBack      = 1ULL << 3,
};

// Visibility — lock / hide / show.
enum class VisAction : std::uint64_t {
  None   = 0,
  Lock   = 1ULL << 0,
  Unlock = 1ULL << 1,
  Hide   = 1ULL << 2,
  Show   = 1ULL << 3,
};

// Align — six standard alignments. Need ≥2 objects.
enum class AlignAction : std::uint64_t {
  None    = 0,
  Left    = 1ULL << 0,
  CenterH = 1ULL << 1,
  Right   = 1ULL << 2,
  Top     = 1ULL << 3,
  CenterV = 1ULL << 4,
  Bottom  = 1ULL << 5,
};

// Distribute — needs ≥3 objects.
enum class DistAction : std::uint64_t {
  None         = 0,
  Horizontal   = 1ULL << 0,
  Vertical     = 1ULL << 1,
  SpaceH       = 1ULL << 2,
  SpaceV       = 1ULL << 3,
};

// ────────────────────────────────────────────────────────────────────────────
//  Node-world action enums (smaller vocabulary, separate type space)
// ────────────────────────────────────────────────────────────────────────────

// Node-structure — ops that change the node graph of a path.
enum class NodeStructAction : std::uint64_t {
  None       = 0,
  Join       = 1ULL << 0,  // s157 identity-rule lives here
  BreakNode  = 1ULL << 1,
  DeleteNode = 1ULL << 2,
  AddNode    = 1ULL << 3,
};

// Node-kind — change a node's smoothness category.
enum class NodeKindAction : std::uint64_t {
  None         = 0,
  MakeSmooth   = 1ULL << 0,
  MakeCorner   = 1ULL << 1,
  MakeSymmetric= 1ULL << 2,
};

// ────────────────────────────────────────────────────────────────────────────
//  Bitwise operators for the enum-class masks
//
//  enum class doesn't get implicit bitwise ops; we provide them as a
//  small macro expansion to keep the per-enum declarations terse.
//
//  any(mask) — true if any bit set (i.e. mask != None).
// ────────────────────────────────────────────────────────────────────────────

#define CURVZ_BITWISE_ENUM(E)                                                  \
  constexpr E operator|(E a, E b) {                                            \
    return E(std::uint64_t(a) | std::uint64_t(b));                             \
  }                                                                            \
  constexpr E operator&(E a, E b) {                                            \
    return E(std::uint64_t(a) & std::uint64_t(b));                             \
  }                                                                            \
  constexpr E operator~(E a) { return E(~std::uint64_t(a)); }                  \
  constexpr E &operator|=(E &a, E b) { return a = a | b; }                     \
  constexpr E &operator&=(E &a, E b) { return a = a & b; }                     \
  constexpr bool any(E a) { return std::uint64_t(a) != 0; }

CURVZ_BITWISE_ENUM(LifeAction)
CURVZ_BITWISE_ENUM(StructAction)
CURVZ_BITWISE_ENUM(BoolAction)
CURVZ_BITWISE_ENUM(EffectAction)
CURVZ_BITWISE_ENUM(LayerAction)
CURVZ_BITWISE_ENUM(VisAction)
CURVZ_BITWISE_ENUM(AlignAction)
CURVZ_BITWISE_ENUM(DistAction)
CURVZ_BITWISE_ENUM(NodeStructAction)
CURVZ_BITWISE_ENUM(NodeKindAction)

#undef CURVZ_BITWISE_ENUM

// ────────────────────────────────────────────────────────────────────────────
//  ObjectInfo — pure facts about the object selection
//
//  No policy. Just counts, kind tallies, behavioural flags. The
//  compute_object_actions() function reads from here and writes the
//  Actions masks. Bonus consumers (status bar, inspector) can read
//  Info directly without going through Actions.
//
//  Phrase consumers in terms of behaviour, not type tags, wherever
//  possible. `all_closed_paths` scales to a future MeshNode that is
//  closed-and-fillable; `all_kind(Type::Path)` does not.
// ────────────────────────────────────────────────────────────────────────────

struct ObjectInfo {
  // ── Cardinality
  std::size_t count = 0;
  bool        is_empty () const { return count == 0; }
  bool        is_single() const { return count == 1; }
  bool        is_pair  () const { return count == 2; }
  bool        is_multi () const { return count >= 2; }

  // ── Single-selection convenience (only meaningful when is_single)
  // The lone selected node, or nullptr if not is_single().
  SceneNode *single = nullptr;

  // ── Kind-presence flags (any-of)
  // Behavioural-leaning tallies. Kind tags are kept for the few rules
  // that genuinely need to dispatch on identity (release-compound only
  // applies to a Compound, etc.).
  bool any_path     = false;
  bool any_compound = false;
  bool any_group    = false;
  bool any_warp     = false;
  bool any_blend    = false;
  bool any_text     = false;
  bool any_image    = false;
  bool any_ref      = false;

  // ── Homogeneity flags (all-of, false on empty)
  bool all_paths    = false;  // every selected object is type Path
  bool all_groups   = false;
  bool all_path_like= false;  // Path or Compound (boolean-op input class)

  // ── Behavioural flags
  bool all_closed   = false;  // every PATH-typed selection has closed=true
  bool any_open_path= false;  // at least one path is open
  bool any_locked   = false;
  bool any_hidden   = false;
  bool any_bound_swatch = false;
  bool any_bound_style  = false;

  // ── Container flags
  // Common parent across all selected — null if selection spans
  // multiple parents (or selection is empty).
  SceneNode *common_parent = nullptr;
  bool       shares_parent = false;  // common_parent != null && count >= 1
  bool       any_in_group  = false;  // any selection has a Group ancestor

  // ── Reset to default-constructed empty state. Used at the top of
  //    every recompute pass.
  void clear() { *this = ObjectInfo{}; }
};

// ────────────────────────────────────────────────────────────────────────────
//  ObjectActions — the permission masks for the object world
//
//  Eight category masks, each with `None = 0` default. A default-
//  constructed ObjectActions is "nothing allowed" — the right answer
//  for empty selections.
// ────────────────────────────────────────────────────────────────────────────

struct ObjectActions {
  LifeAction   life   = LifeAction::None;
  StructAction struct_= StructAction::None;
  BoolAction   bool_  = BoolAction::None;
  EffectAction effect = EffectAction::None;
  LayerAction  layer  = LayerAction::None;
  VisAction    vis    = VisAction::None;
  AlignAction  align  = AlignAction::None;
  DistAction   dist   = DistAction::None;

  // Convenience: anything allowed at all?
  bool any_allowed() const {
    return any(life) || any(struct_) || any(bool_) || any(effect) ||
           any(layer) || any(vis) || any(align) || any(dist);
  }

  void clear() { *this = ObjectActions{}; }
};

// ────────────────────────────────────────────────────────────────────────────
//  NodeInfo — facts about the node-tool selection
//
//  Mirrors ObjectInfo's shape but in the node vocabulary. m_selected2
//  / m_selected_node2 (the cross-path slot) feeds in here too; the
//  cross-path Join rule reads `cross_path && both_endpoints`.
// ────────────────────────────────────────────────────────────────────────────

struct NodeInfo {
  std::size_t count = 0;
  bool        is_empty () const { return count == 0; }
  bool        is_single() const { return count == 1; }
  bool        is_pair  () const { return count == 2; }

  // How many distinct paths are touched by the node selection.
  std::size_t paths_involved = 0;

  // True if every node in the selection is an endpoint (head/tail) of
  // its containing path. Required by Join.
  bool all_endpoints = false;

  // True if the pair selection spans two different paths. Required by
  // cross-path Join.
  bool cross_path = false;

  // True if all selected nodes belong to the same path. (Mutually
  // exclusive with cross_path when count >= 2.)
  bool same_path = true;

  void clear() { *this = NodeInfo{}; }
};

// ────────────────────────────────────────────────────────────────────────────
//  NodeActions — permission masks for the node world
// ────────────────────────────────────────────────────────────────────────────

struct NodeActions {
  NodeStructAction struct_ = NodeStructAction::None;
  NodeKindAction   kind    = NodeKindAction::None;

  bool any_allowed() const { return any(struct_) || any(kind); }
  void clear() { *this = NodeActions{}; }
};

// ────────────────────────────────────────────────────────────────────────────
//  SelectionContext — the live, computed answer
//
//  Owned by Canvas. Canvas mutates the underlying selection vectors,
//  then calls recompute_object() / recompute_node() to refresh the
//  cached Info+Actions and emit signal_changed().
//
//  Consumers (context menu, action-enable, inspector dispatch, status
//  bar, toolbars) read the Info / Actions structs and listen to
//  signal_changed() for invalidation.
// ────────────────────────────────────────────────────────────────────────────

class SelectionContext {
public:
  // ── Read-only accessors. Always return current state (never stale —
  //    recompute is synchronous before signal_changed emits).
  const ObjectInfo    &object_info()    const { return m_obj_info; }
  const ObjectActions &object_actions() const { return m_obj_actions; }
  const NodeInfo      &node_info()      const { return m_node_info; }
  const NodeActions   &node_actions()   const { return m_node_actions; }

  // ── Recompute entry points. Canvas calls these at every selection
  //    mutation site. Both refresh their respective Info, regenerate
  //    the matching Actions, and emit signal_changed if anything
  //    changed. (No-op on no-change; consumers don't get spurious
  //    refreshes.)
  //
  //  selection: the std::vector<SceneNode*> from Canvas (m_selection).
  //  primary  : Canvas's m_selected (may be null).
  void recompute_object(const std::vector<SceneNode *> &selection,
                        SceneNode *primary);

  // For the node side, signature mirrors how Canvas stores it: an
  // array of (path, node_index) pairs plus the primary path/index, and
  // the secondary slot (for Join). Defined in Canvas.hpp as
  // Canvas::NodeSel; we accept it via parallel vectors here to avoid
  // pulling Canvas.hpp into this header.
  void recompute_node(const std::vector<SceneNode *> &paths,
                      const std::vector<int>         &node_indices,
                      SceneNode *primary_path, int primary_index,
                      SceneNode *secondary_path, int secondary_index);

  // ── Signal — emits when EITHER world's Info+Actions changes.
  //    Recipients can compare against their cached prior state to see
  //    which fields actually moved (Info structs are POD-comparable).
  using ChangedSignal = sigc::signal<void()>;
  ChangedSignal &signal_changed() { return m_sig_changed; }

  // ── Drop a target SceneNode pointer from any cached state.
  //    Called by Canvas::scrub_node_refs after the node has been
  //    erased from the document tree. Folds the selection portion of
  //    the s158 scrub audit into one call.
  //
  //    Note: this only scrubs the cached Info (the `single` and
  //    `common_parent` fields). The underlying Canvas selection
  //    vectors are scrubbed by Canvas itself; recompute_*() runs
  //    afterwards to bring the Info back in sync.
  void scrub_node_ref(const SceneNode *target);

private:
  ObjectInfo    m_obj_info;
  ObjectActions m_obj_actions;
  NodeInfo      m_node_info;
  NodeActions   m_node_actions;

  ChangedSignal m_sig_changed;
};

// ────────────────────────────────────────────────────────────────────────────
//  Free functions — the policy lives here, side-by-side, top-to-bottom
//
//  These take an Info and produce Actions. Pure functions — easy to
//  test, easy to read, easy to diff across versions of the codebase.
//  When a rule changes, it changes here, in one place, and every
//  consumer downstream picks up the new behaviour automatically.
// ────────────────────────────────────────────────────────────────────────────

ObjectActions compute_object_actions(const ObjectInfo &info);
NodeActions   compute_node_actions  (const NodeInfo   &info);

}  // namespace Curvz
