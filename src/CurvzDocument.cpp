#include "CurvzDocument.hpp"

namespace Curvz {

// ── Iid index walk (s167 m1) ─────────────────────────────────────────
//
// Recursive walk that registers every SceneNode reachable from a root
// into the index map. Mirrors the descent rules described in
// CurvzDocument.hpp's index comment block:
//
//   • children                  — recurse
//   • clip_shape                — recurse (ClipGroup authoritative slot)
//   • blend_source_a / _b       — recurse (Blend authoritative inputs)
//   • warp_source               — recurse (Warp authoritative input)
//   • blend_cache / warp_glyph_cache / warp_cache  — SKIPPED. These are
//     derived from the authoritative slots above and get rebuilt by
//     the rendering pipeline; iids inside them are not stable across
//     rebuilds. Indexing them would mean a command could capture an
//     iid that vanishes on the next paint — exactly the dangling-
//     handle class the index exists to prevent.
//
// Nodes with empty internal_id (rare — short-lived intermediates that
// never made it into the persistent tree) are skipped; an empty key
// in the map would collide with every other empty-iid node and serve
// no caller's lookup.
//
// File-static helper rather than a member: the walk is implementation
// detail, doesn't need access to private state beyond the map itself,
// and keeping it out of the class definition avoids leaking SceneNode
// internals into the header's compile dependencies.
static void index_walk(const SceneNode* n,
                       std::unordered_map<std::string, SceneNode*>& idx) {
    if (!n) return;

    if (!n->internal_id.empty()) {
        // Last-write-wins on iid collision. Collisions shouldn't happen
        // for real (UUIDs are unique by construction), but if a caller
        // duplicates a node verbatim — e.g. snapshot held by a command
        // momentarily inserted into the tree before the original is
        // erased — only one entry survives. The walker visits children
        // in tree order, so the deepest-most-recent occurrence wins,
        // which matches "look up the live node" intent in any plausible
        // collision scenario.
        idx[n->internal_id] = const_cast<SceneNode*>(n);
    }

    for (const auto& c : n->children)
        index_walk(c.get(), idx);

    // Authoritative non-children slots. Each is a unique_ptr<SceneNode>
    // that holds a sub-tree the renderer treats as part of the node's
    // logical content. Same descent rules as `children` for these —
    // the slot is structural, not derived.
    if (n->clip_shape)
        index_walk(n->clip_shape.get(), idx);
    if (n->blend_source_a)
        index_walk(n->blend_source_a.get(), idx);
    if (n->blend_source_b)
        index_walk(n->blend_source_b.get(), idx);
    if (n->warp_source)
        index_walk(n->warp_source.get(), idx);
    // Note: blend_cache, warp_glyph_cache, warp_cache deliberately not
    // walked — see header comment.
}

SceneNode* CurvzDocument::find_by_iid(const std::string& iid) const {
    if (iid.empty()) return nullptr;

    if (m_iid_index_dirty) {
        m_iid_index.clear();
        for (const auto& l : layers)
            index_walk(l.get(), m_iid_index);
        m_iid_index_dirty = false;
    }

    auto it = m_iid_index.find(iid);
    if (it != m_iid_index.end()) return it->second;

    // s168 m3: self-heal on miss. The cache is invalidated on destructive
    // ops (Canvas::scrub_node_refs) but NOT on additions — additions are
    // scattered across many paths (Canvas_input creation sites, paste,
    // duplicate, SVG load, layer creation, blend/warp construction, etc.)
    // and there's no single seam to hook. A miss here can mean either
    // "node genuinely not in tree" (history scrub, cross-doc closed) or
    // "node was added since the last walk and the cache is stale." We
    // can't distinguish without walking, so: rebuild once, look again.
    // If still missing, the node really isn't there and we return nullptr.
    //
    // Cost: one extra full walk per genuine miss. That's the path taken
    // by history-scrub and cross-doc-closed lookups, both of which are
    // meant to return nullptr — so the rebuild is wasted work in those
    // cases. The walk is O(n) on the document's node count and only
    // happens when a command resolves an iid that isn't in the live cache,
    // which is a small fraction of total resolutions. Acceptable.
    //
    // The structural alternative (invalidate at every insertion site) was
    // ruled out: too many fingers, same failure mode as the s167 m2 sweep.
    // Closing the gap by construction here means callers stay ignorant of
    // cache lifecycle — same shape as the s168 m2 default-init fix.
    m_iid_index.clear();
    for (const auto& l : layers)
        index_walk(l.get(), m_iid_index);
    // m_iid_index_dirty stays false — we just rebuilt.

    it = m_iid_index.find(iid);
    return it == m_iid_index.end() ? nullptr : it->second;
}

} // namespace Curvz
