// AnimatingSvgParser.hpp ────────────────────────────────────────────────────
//
// s290 m1a — Peer class to SvgParser. Owns its own copy of the SVG parse
// pipeline, byte-identical in behaviour to today's `parse_svg` /
// `parse_svg_file` for m1a. The model-translation points are flagged in
// the .cpp for m1b, where they'll fan out to an SvgEmitter consumer in
// addition to building the CurvzDocument.
//
// ── Why a peer class and not a subclass ────────────────────────────────────
//
// Today's SvgParser is not a class. It's two free functions
// (`parse_svg_file`, `parse_svg`) in `namespace Curvz`, with a ~1700-line
// `parse_svg` body and ~17 static helpers. There is nothing to subclass.
//
// Class-ifying the base parser is the right end state but its own arc —
// the base parser is stable, foundational, and exercised by every
// existing call site. Lifting it now would risk subtle behavioural
// regressions across File→Open, smoke tests, and every other ingest path.
//
// So: AnimatingSvgParser is a peer class. It owns a copy of the parse
// code, slated to be modified at the model-translation points in m1b
// (path emit / compound open+close / group open+close / layer open+close
// / swatch register / style register / doc metadata apply). The base
// parser is untouched and byte-identical to today.
//
// The trade-off accepted: two copies of parse logic until the
// class-ification arc happens. Bugs fixed in the base have to be ported
// here. The base parser is stable enough that this shouldn't bite often,
// and the divergence is intentional — the animating side is meant to do
// things the base doesn't (emit events for a consumer).
//
// ── m1a scope ──────────────────────────────────────────────────────────────
//
//   - Class exists. Default-constructable.
//   - `parse_file(path)` and `parse(svg_text)` mirror the free-function
//     entries. They return the fully-built CurvzDocument, identical to
//     what the base parser produces.
//   - No emit calls. The model-translation points exist as code but
//     don't fire events.
//
// ── m1b.1 scope (shipped) ─────────────────────────────────────────────────
//
//   - SvgEmitter abstract class shipped with on_path.
//   - AnimatingSvgParser gained a constructor variant taking
//     `SvgEmitter*`. Default-constructed instances pass nullptr and
//     skip emission.
//   - The parse body fires `on_path` once per Path SceneNode via
//     two funnels: push_into_parent (top-level commits) and
//     commit_child (container-internal commits).
//
// ── m1b.2 scope (shipped) ─────────────────────────────────────────────────
//
//   - SvgEmitter grew on_compound_begin/end, on_group_begin/end.
//   - The parse body fires bracketing events at four sites:
//     stack-based <g> (begin at commit, end at stack-pop) and
//     atomic compounds (Curvz-authored + foreign multi-subpath:
//     begin before child-loop, end after).
//
// ── m1b.3 scope (current) ─────────────────────────────────────────────────
//
//   - SvgEmitter grew on_doc_metadata and on_layer_begin/end.
//   - on_doc_metadata fires once at the end of the <svg> root tag
//     handler.
//   - on_layer_begin fires after current_layer is set in each of
//     the six layer-creation sites (RefLayer, MeasureLayer,
//     GuideLayer, regular Layer via <g>, bare-objects default
//     layer, post-parse safety-net layer). The first five push
//     onto the stack; on_layer_end fires from the existing
//     stack-pop seam, extended to handle Type::Layer (technical
//     layer kinds — RefLayer/MeasureLayer/GuideLayer/Grid/Margin
//     — do not get bracketing events; m1b.3 emits only for
//     Type::Layer).
//   - The safety-net layer at end-of-parse fires begin and end
//     adjacently since it doesn't go on the stack.
//
// At m1b close, the event stream contains everything needed to
// reconstruct the parser's full output tree.

#pragma once
#include "CurvzDocument.hpp"
#include "SvgEmitter.hpp"   // s290 m1b.1
#include <string>
#include <memory>

namespace Curvz {

class AnimatingSvgParser {
public:
    // m1a — default-construct, no emitter. Smoke-test friendly:
    // parse with no consumer, get back the doc, verify shape.
    AnimatingSvgParser() = default;

    // m1b.1 — construct with a non-owning emitter pointer. The
    // emitter must outlive any parse call made on this instance.
    // Passing nullptr is equivalent to the default ctor.
    explicit AnimatingSvgParser(SvgEmitter* emitter) : m_emitter(emitter) {}

    // Parse from a file path. Returns nullptr on failure (file not
    // readable, catastrophic parse exception). Matches the base
    // parse_svg_file contract.
    std::unique_ptr<CurvzDocument> parse_file(const std::string& path);

    // Parse from an SVG string. Returns the built CurvzDocument.
    std::unique_ptr<CurvzDocument> parse(const std::string& svg);

private:
    SvgEmitter* m_emitter = nullptr;  // not owned; nullable
};

} // namespace Curvz
