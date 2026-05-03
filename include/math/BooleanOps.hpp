#pragma once
#include "BezierPath.hpp"
#include "../SceneNode.hpp"
#include <optional>
#include <vector>

namespace Curvz {

enum class BooleanOpType { Union, Subtract, Intersect };

// ══════════════════════════════════════════════════════════════════════════════
// HAND-ROLLED ENGINE — RETIRED IN S93 m7
//
// The boolean_op function below is the entry point for the hand-rolled
// Sederberg-Nishita Bézier-clipping engine in math/BooleanOps.cpp. As of
// S93 m7 it is no longer called from any live code path; Curvz routes all
// boolean operations through boolean_op_clipper (see BooleanOpsClipper.hpp)
// exclusively.
//
// The function declaration and its .cpp implementation are RETAINED on disk
// because the math has independent value: the file documents the
// Sederberg-Nishita paper formulas, the convex-hull pair-test (all six
// edge pairs of the four Bernstein control points, not just adjacent
// pairs — a non-obvious gotcha that took several sessions to pin down),
// and the union-fold-of-N approach. Future work that needs Bézier
// clipping primitives (e.g. self-intersection detection, offset-curve
// trimming) may want to lift code or commentary from there.
//
// Do NOT call boolean_op from new code. Use boolean_op_clipper.
//
// The BooleanOpType enum above is shared with the Clipper engine and
// remains live.
// ══════════════════════════════════════════════════════════════════════════════

// Perform a boolean operation on two closed paths.
// Both paths must be closed; open paths return nullopt.
// Returns the resulting PathData, or nullopt if the operation
// produces no geometry (e.g. Intersect on disjoint paths).
// Multiple result loops are stitched into a single compound-compatible
// PathData with subpath breaks (open segments between loops) — the
// caller is responsible for splitting into separate SceneNodes if needed.
//
// Result loops are returned as a vector so the caller can decide whether
// to make a single compound path or individual paths.
std::vector<PathData> boolean_op(const BezierPath& A,
                                  const BezierPath& B,
                                  BooleanOpType op);

} // namespace Curvz
