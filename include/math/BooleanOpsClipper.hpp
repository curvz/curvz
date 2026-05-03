#pragma once
#include "BooleanOps.hpp"
#include "BezierPath.hpp"
#include "../SceneNode.hpp"
#include <vector>

namespace Curvz {

// ══════════════════════════════════════════════════════════════════════════════
// Clipper2-backed boolean operations.
//
// As of S93 m7 this is the SOLE engine reachable from the running app.
// The hand-rolled Sederberg-Nishita engine in BooleanOps.cpp is retained
// on disk for reference but is no longer called — see the retirement
// banner at the top of that file for the rationale.
//
// Clipper2 operates on straight-line polygons with integer coordinates
// internally. To bridge, each input cubic Bézier segment is flattened to a
// polyline at a small tolerance (imperceptible at typical icon zooms), the
// op is executed, and the resulting polylines are refitted back to cubic
// Bézier paths before being returned as PathData.
//
// Compound operands are supported natively: pass the list of subpaths for
// each operand. For a simple Path, pass a one-element vector. For a
// Compound, pass one BezierPath per Compound child.
//
// Fill rule interpretation:
//   - Single-subpath operands: FillRule::NonZero (direction-agnostic).
//   - Multi-subpath operands (Compound): FillRule::EvenOdd (holes from
//     nested windings render correctly).
// These may be tunable later; for now they match how the Canvas draws.
//
// Returned loops follow the same convention as the hand-rolled entry point:
// the caller wraps multi-loop results in a SceneNode::Compound.
//
// s122 m3: N-operand entry. Each operand is a vector of BezierPath subpaths
// (a Path = 1 subpath, a Compound = N subpaths). The 2-operand entry is
// retained as a thin shim that forwards to the N-operand version with the
// operands packed into a 2-element vector.
//   - Union   N: all operands' subpaths unioned in one Clipper2 call.
//   - Intersect N: iterative running = Intersect(running, next). Intersect
//     is associative and the result only shrinks, so no fold corruption.
//   - Subtract N: operands[0] minus Union(operands[1..]). "Bottommost minus
//     union of the rest" — Affinity / Illustrator Pathfinder convention.
// For N==2 all three reduce to the prior 2-operand binary Clipper2 calls.
//
// Credit: Clipper2 © 2010-2025 Angus Johnson, Boost Software License 1.0.
//         https://www.angusj.com/clipper2/
// ══════════════════════════════════════════════════════════════════════════════

std::vector<PathData> boolean_op_clipper(
    const std::vector<std::vector<BezierPath>>& operands,
    BooleanOpType op);

// Back-compat 2-operand shim — packs (A, B) into a 2-element operands vector
// and forwards. Keeps existing call sites compiling unchanged.
std::vector<PathData> boolean_op_clipper(
    const std::vector<BezierPath>& A,
    const std::vector<BezierPath>& B,
    BooleanOpType op);

} // namespace Curvz
