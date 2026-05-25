#pragma once
#include "../SceneNode.hpp"

// ══════════════════════════════════════════════════════════════════════════════
// PathNormalize — repair dishonest node types on polyline-derived PathData
//
// Producers lie about node types in different ways. The fixes:
//
//   1. DEMOTE Corner → Smooth where tangent discontinuity < SMOOTH_THRESHOLD.
//      Clipper2's straight-cubic refit emits every output node tagged Corner
//      even when handles are colocated on the anchor and the geometry flows
//      smoothly through. Demote restores honest types and chord-bisect
//      handles via BezierPath::set_node_type.
//
//   2. PROMOTE Smooth → Corner where tangent discontinuity > CORNER_PROMOTE.
//      Warp's per-slice cubic-fit emits every node tagged Smooth (the
//      BezierNode default) regardless of geometry — letterform corners,
//      cusp-like features, even sharp polygonal joints all come out
//      type-Smooth. Promote re-tags real corners so the downstream reduce
//      step has structural breakpoints to anchor against.
//
// The two thresholds are far apart (5° / 60°) so a "no honest signal" middle
// band exists where the producer's tag is left alone.
//
// Tangent angle is computed using the node's handles when non-degenerate,
// falling back to neighbour-chord direction when a handle is colocated with
// the anchor. This handles both Clipper2 (colocated handles → chord
// fallback) and Warp (real fitted handles → use directly) correctly.
//
// Once tags are honest, reduce can trust them as structural signals. The
// keeper pre-pass identifies runs delimited by pivots (Corner / Cusp);
// promote ensures real corners actually appear as pivots.
//
// Idempotent: a second call retypes nothing. Threshold parameter is the
// same shape-knob as cleanup_loop_v4's apex_min_turn_deg for the demote
// side; the promote threshold is fixed at 60°.
//
// Open-path endpoints are skipped automatically — tangent is only defined
// when at least one side has a valid handle or neighbour.
// ══════════════════════════════════════════════════════════════════════════════

namespace Curvz {

// Walk every node in `path` and retype based on tangent discontinuity angle.
//
//   angle < threshold_deg (default 5°)   AND was Corner   → Smooth (demote)
//   angle >= 60°                         AND was Smooth/Sym → Corner (promote)
//   otherwise                                              → keep current type
//
// Returns the total count of type changes (demotes + promotes).
int normalize_node_types(PathData& path, double threshold_deg = 5.0);

} // namespace Curvz
