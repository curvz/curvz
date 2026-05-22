// SvgEmitter.hpp ────────────────────────────────────────────────────────────
//
// s290 m1b — Consumer-side abstraction for AnimatingSvgParser. The
// parser fires events as it commits pieces of model into the
// CurvzDocument; the consumer overrides whichever events it cares
// about. SvgPerformer is one consumer; future thumbnail renderers and
// diff-import visualisers are others.
//
// All event methods default to no-ops so a consumer only overrides
// what matters to it. The peer parser
// (`AnimatingSvgParser::parse`) holds a non-owning pointer to one
// SvgEmitter for the duration of a single parse call.
//
// ── m1b shipment plan ──────────────────────────────────────────────────────
//
//   m1b.1 (shipped) — on_path. Path commits are the most numerous
//     event and the spine of any animator's consumption.
//
//   m1b.2 (shipped) — on_compound_begin/end, on_group_begin/end.
//     Container bracketing surrounds path events to express the
//     tree structure (a Path inside a Compound, a Compound inside a
//     Group, etc.).
//
//   m1b.3 (this header's current shape) — on_doc_metadata,
//     on_layer_begin/end. Setup events that fire before any
//     construction events. on_doc_metadata declares the target
//     canvas geometry; on_layer_begin/end brackets top-level
//     content layers (Type::Layer only; technical layer kinds are
//     infrastructure and do not get bracketing).
//
// At m1b close, every model-translation point the parser knows
// about has a matching event, and a consumer subscribing to all
// of them can reconstruct the parser's full output tree from the
// event stream alone.
//
// ── Why a SceneNode reference, not field-by-field args ────────────────────
//
// `on_path` takes a `const SceneNode&` rather than enumerating fields
// (path_data, fill, stroke, fill_rule, opacity, transform, etc.). Two
// reasons:
//
//   1. The SceneNode IS the parsed product. A consumer that wants
//      "the same thing the doc will commit" gets exactly that —
//      including any subtle field the parser sets that we'd forget
//      to thread through a flat signature.
//   2. As SceneNode gains fields (already has many; will gain more),
//      no signature churn here. The signature is structurally
//      future-proof.
//
// The reference is to the node BEFORE it gets std::move'd into the
// tree. Consumers may read but must not retain the pointer/reference
// beyond the callback — the node is moved into the tree immediately
// after.

#pragma once
#include "SceneNode.hpp"

namespace Curvz {

struct CanvasModel; // m1b.3 — declared in CurvzDocument.hpp; consumers
                    // that override on_doc_metadata must include it.

class SvgEmitter {
public:
    virtual ~SvgEmitter() = default;

    // m1b.1 — fires once per Path SceneNode at commit time, before
    // the node is moved into its parent in the tree. The reference
    // is valid only for the duration of the callback; consumers must
    // not retain it. Read fields synchronously and stash whatever
    // the consumer needs (path data, fill, stroke, etc.) into the
    // consumer's own state.
    //
    // Fires for every Path the parser would commit — both top-level
    // paths and paths nested inside Compounds, Groups, etc. The
    // container structure is expressed via the bracketing events
    // below (m1b.2).
    virtual void on_path(const SceneNode& /*path_node*/) {}

    // m1b.2 — container bracketing events. Fire in document order
    // around the on_path events for the contained paths. A Path
    // inside a Compound inside a Group emits as:
    //
    //   on_group_begin
    //     on_compound_begin
    //       on_path
    //       on_path
    //     on_compound_end
    //   on_group_end
    //
    // The reference is valid for the callback only; same lifetime
    // rules as on_path. Consumers reading container metadata
    // (name, id, transform, fill on Compound, opacity, visible)
    // should stash what they need synchronously.
    //
    // ── Stack-based vs atomic containers ──────────────────────
    //
    // The base SvgParser builds Compounds two different ways:
    //   - stack-based: <g data-curvz-compound> with children
    //     streaming in as separate path tags between open and
    //     close — begin fires at <g> open, end fires at </g>.
    //   - atomic: <path data-curvz-compound data-curvz-multi-d>
    //     or foreign multi-subpath <path> — children are built
    //     in a tight loop inside the path handler and the
    //     compound is committed as a complete unit. begin fires
    //     just before the loop, end fires just after.
    //
    // Both patterns produce the SAME event sequence from the
    // consumer's perspective — begin / on_path* / end — so a
    // consumer doesn't need to distinguish them.
    //
    // Groups are always stack-based; only Compounds have the
    // atomic-construction case.

    virtual void on_compound_begin(const SceneNode& /*compound_node*/) {}
    virtual void on_compound_end() {}
    virtual void on_group_begin(const SceneNode& /*group_node*/) {}
    virtual void on_group_end() {}

    // m1b.3 — setup events. Fire BEFORE any construction events.
    //
    // on_doc_metadata fires once near the start of parse, right
    // after the <svg> root tag's attributes have been read into
    // doc->canvas. The consumer learns the target canvas geometry
    // (viewBox, intended size, units, physical/pixel mode) and can
    // configure its target doc accordingly. Fires exactly once per
    // parse; if the SVG has no <svg> root (shouldn't happen for
    // well-formed input) the event doesn't fire and the canvas
    // model stays at its default.
    virtual void on_doc_metadata(const CanvasModel& /*canvas*/) {}

    // on_layer_begin / on_layer_end bracket the top-level layers
    // a parsed SVG contains. Fires for Type::Layer only — the
    // technical layer kinds (RefLayer, MeasureLayer, GuideLayer,
    // GridLayer, MarginLayer) are infrastructure and do not get
    // bracketing events. The consumer reads layer name, opacity,
    // visibility, color, locked, transform off the const ref.
    //
    // A Curvz-authored doc typically has one or more Type::Layer
    // entries; an empty doc gets a single safety-net Layer. A
    // foreign SVG with no <g> markup gets a single fallback Layer
    // populated by bare top-level objects. All three cases fire
    // on_layer_begin/end pairs.
    virtual void on_layer_begin(const SceneNode& /*layer_node*/) {}
    virtual void on_layer_end() {}
};

} // namespace Curvz
