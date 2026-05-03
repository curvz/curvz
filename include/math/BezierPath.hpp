#pragma once
#include "Vec2.hpp"
#include "CubicSegment.hpp"
#include "../SceneNode.hpp"   // BezierNode, PathData
#include <cairomm/cairomm.h>
#include <optional>
#include <vector>
#include <unordered_set>

namespace Curvz {

struct HitResult {
    enum class Kind { None, Node, HandleIn, HandleOut, OnCurve };
    Kind   kind         = Kind::None;
    int    node_index   = -1;
    double t            = 0.0;   // curve parameter if OnCurve
    int    segment_index = -1;
    double distance     = 1e9;
};

class BezierPath {
public:
    std::vector<BezierNode> nodes;
    std::vector<NodeSet>    node_sets; // preserved from PathData; cleared by mutating ops
    bool closed   = false;

    // ── Construction from data model ────────────────────────────────────────
    static BezierPath from_path_data(const PathData& pd);
    PathData to_path_data() const;

    // ── Segment access ──────────────────────────────────────────────────────
    int segment_count() const;
    CubicSegment segment(int i) const; // i wraps if closed

    // ── Node manipulation ───────────────────────────────────────────────────
    // Insert a new node by splitting segment seg_idx at parameter t
    void insert_node_at(int seg_idx, double t);

    // Delete node at idx; reconnects neighbours
    void delete_node(int idx);

    // Set node type and adjust handles to match
    void set_node_type(int idx, BezierNode::Type type);

    // Move anchor; adjust handles for smooth/symmetric nodes
    void move_node(int idx, Vec2 new_pos);

    // Move in-handle of node idx; mirror if symmetric, keep direction if smooth
    void move_handle_in(int idx, Vec2 new_pos);

    // Move out-handle of node idx
    void move_handle_out(int idx, Vec2 new_pos);

    // Recompute cusp handles to lock to neighbour segment vectors
    void recompute_cusp_handles(int idx);

    // Recompute handles at a newly joined node (after open→close or close→open)
    // Respects node type constraints at both ends of the new segment
    void recompute_join_handles(int idx);

    // Reverse winding direction — swaps cx1/cx2 on all nodes then reverses array
    void reverse();

    // Shoelace signed area on anchor points.
    // In doc space (Y-down): positive = CW traversal, negative = CCW.
    // Returns 0 for paths with fewer than 3 nodes.
    double signed_area() const;

    // Open a closed path at node idx: rotates so idx becomes node 0,
    // duplicates it as the tail, and opens the path.
    // No-op if path is already open or idx out of range.
    void open_at_node(int idx);

    // Split an open path at node idx into two PathData halves.
    // Left:  nodes [0 .. idx]      — idx is the tail
    // Right: nodes [idx .. n-1]    — idx is the head
    // Both halves are open. Returns {left, right}.
    // No-op (returns empty pair) if closed, idx is 0 or n-1, or n < 3.
    std::pair<PathData, PathData> split_at_node(int idx) const;

    // ── Hit testing ─────────────────────────────────────────────────────────
    // Returns nearest hit within tolerance (screen pixels)
    HitResult hit_test(Vec2 doc_pt, double zoom, double tolerance_screen_px = 6.0) const;

    // ── Cairo rendering ─────────────────────────────────────────────────────
    void apply_to_cairo(const Cairo::RefPtr<Cairo::Context>& cr) const;

    // Draw node handles for the editor overlay
    void draw_editor_overlay(const Cairo::RefPtr<Cairo::Context>& cr,
                             double zoom,
                             int selected_node = -1,
                             bool is_closed = false) const;

    void draw_editor_overlay(const Cairo::RefPtr<Cairo::Context>& cr,
                             double zoom,
                             const std::unordered_set<int>& selected_nodes,
                             bool is_closed = false) const;
};

} // namespace Curvz
