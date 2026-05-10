#include "CommandHistory.hpp"
#include "AppPreferences.hpp"  // s145 m1 — undo depth is a user pref
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"    // s167 m1 — full CurvzProject for find_by_iid
#include "curvz_utils.hpp"     // s167 m1 — find_by_iid pump
#include <algorithm>
#include <unordered_map>       // s172 m3 — ReorderLayersCommand apply helper

namespace Curvz {

// s168 m6: invalidate every doc's iid index across a project. Called at
// the top of each iid-using command's execute() and undo() so the next
// find_by_iid call rebuilds from a fresh tree walk.
//
// Why this is needed: the per-doc iid index is invalidated only on
// destructive ops that flow through Canvas::scrub_node_refs (s167 m1).
// Many node-destruction paths don't go through that seam — e.g. a
// command's own undo that erases a node from a parent's children
// (AddNodeCommand::undo, DeleteObjectCommand::execute, etc.) — leaving
// the cache holding pointers to freed memory. A subsequent find_by_iid
// hit on a stale entry then dereferences garbage and may crash with
// reads of non-enum field values (e.g. type=474372240) before any
// safety check can fire.
//
// The s168 m3 self-heal in find_by_iid covers cache MISS (rebuild on
// not-found), but doesn't help with cache HIT-on-stale-pointer. This
// helper closes that gap from the other direction: invalidate proactively
// at every command dispatch so the next lookup forces a fresh walk.
//
// Cost: one rebuild per command dispatch per doc. Rebuild is O(n) on
// document node count, which is small relative to what commands
// already do. Run on user-action timescales (Ctrl+Z, mouse release),
// not per-frame. Negligible.
//
// STRIP: do not strip. This is structural, not diagnostic.
static void invalidate_iid_indexes(CurvzProject* proj) {
    if (!proj) return;
    for (const auto& doc : proj->documents) {
        if (doc) doc->invalidate_iid_index();
    }
}

// ── EditPathCommand bodies (s167 m1) ─────────────────────────────────
// Out-of-line because the resolver needs the full CurvzProject type,
// which we forward-declare in the header to avoid pulling CurvzProject
// (and its transitive includes) into every CommandHistory consumer.
//
// Both execute() and undo() share the same shape: resolve the iid back
// to a live SceneNode, early-return if it's gone, otherwise apply the
// snapshot. If the node is gone (e.g. its document was closed, or a
// destructive op removed it after this command was queued), the
// command becomes a no-op rather than crashing on a dangling pointer.
//
// The `obj->path` guard is preserved from the original — Path nodes
// have a path; container nodes don't, and a misrouted EditPathCommand
// shouldn't crash on the latter case either.
//
// Empty-iid diagnostic: a SceneNode entering the tree without an iid
// set is a class of latent bug — the command will always resolve to
// nullptr, silently. Logging at DEBUG when we see one keeps it visible
// without cluttering INFO. Remove in s167 stage 6 cleanup once the
// migration has run for several sessions without surfacing one. Same
// rationale applies to the s167 m2 commands below.
// s168 m1 DIAG: empty-iid promoted DEBUG→INFO, success-path entry
// log added. Tag prefix [IIDDIAG] for grep. STRIP after triage.
void EditPathCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] EditPath::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        LOG_INFO("[IIDDIAG] EditPath::exec  empty obj_iid (push site bug?)");
        return;
    }
    auto* obj = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!obj) {
        LOG_INFO("[IIDDIAG] EditPath::exec  iid='{}' resolved=nullptr", obj_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] EditPath::exec  iid='{}' resolved name='{}' "
             "type={} live_iid='{}' has_path={}",
             obj_iid, obj->name, (int)obj->type, obj->internal_id,
             obj->path ? 1 : 0);
    if (!obj->path) return;
    *obj->path = after;
}

void EditPathCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] EditPath::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        LOG_INFO("[IIDDIAG] EditPath::undo  empty obj_iid (push site bug?)");
        return;
    }
    auto* obj = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!obj) {
        LOG_INFO("[IIDDIAG] EditPath::undo  iid='{}' resolved=nullptr", obj_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] EditPath::undo  iid='{}' resolved name='{}' "
             "type={} live_iid='{}' has_path={}",
             obj_iid, obj->name, (int)obj->type, obj->internal_id,
             obj->path ? 1 : 0);
    if (!obj->path) return;
    *obj->path = before;
}

// ── EditAppearanceCommand bodies (s167 m2) ───────────────────────────
// Same shape as EditPathCommand. Five fields restored on each side
// (fill, stroke, two swatch ids, bound_style); no `obj->path` guard
// needed because appearance lives on every SceneNode subtype.
void EditAppearanceCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] EditAppearance::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        LOG_INFO("[IIDDIAG] EditAppearance::exec  empty obj_iid (push site bug?)");
        return;
    }
    auto* obj = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!obj) {
        LOG_INFO("[IIDDIAG] EditAppearance::exec  iid='{}' resolved=nullptr",
                 obj_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] EditAppearance::exec  iid='{}' resolved name='{}' "
             "type={} live_iid='{}'",
             obj_iid, obj->name, (int)obj->type, obj->internal_id);
    obj->fill              = fill_after;
    obj->stroke            = stroke_after;
    obj->fill_swatch_id    = fill_swatch_id_after;
    obj->stroke_swatch_id  = stroke_swatch_id_after;
    obj->bound_style       = bound_style_after;
}

void EditAppearanceCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] EditAppearance::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        LOG_INFO("[IIDDIAG] EditAppearance::undo  empty obj_iid (push site bug?)");
        return;
    }
    auto* obj = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!obj) {
        LOG_INFO("[IIDDIAG] EditAppearance::undo  iid='{}' resolved=nullptr",
                 obj_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] EditAppearance::undo  iid='{}' resolved name='{}' "
             "type={} live_iid='{}'",
             obj_iid, obj->name, (int)obj->type, obj->internal_id);
    obj->fill              = fill_before;
    obj->stroke            = stroke_before;
    obj->fill_swatch_id    = fill_swatch_id_before;
    obj->stroke_swatch_id  = stroke_swatch_id_before;
    obj->bound_style       = bound_style_before;
}

// ── EditObjectCommand bodies (s167 m2) ───────────────────────────────
// Combined path + fill + stroke + binding + shadow snapshot. Inspector
// edits coalesce into one of these per object per 1500ms window. The
// `obj->path` guard around path_after / path_before mirrors the
// pre-migration code — non-Path nodes have no path member to write.
void EditObjectCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] EditObject::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        LOG_INFO("[IIDDIAG] EditObject::exec  empty obj_iid (push site bug?)");
        return;
    }
    auto* obj = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!obj) {
        LOG_INFO("[IIDDIAG] EditObject::exec  iid='{}' resolved=nullptr",
                 obj_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] EditObject::exec  iid='{}' resolved name='{}' "
             "type={} live_iid='{}' has_path={}",
             obj_iid, obj->name, (int)obj->type, obj->internal_id,
             obj->path ? 1 : 0);
    if (obj->path) *obj->path = path_after;
    obj->fill              = fill_after;
    obj->stroke            = stroke_after;
    obj->fill_swatch_id    = fill_swatch_id_after;
    obj->stroke_swatch_id  = stroke_swatch_id_after;
    obj->bound_style       = bound_style_after;
    obj->write_shadow(shadow_after);
}

void EditObjectCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] EditObject::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        LOG_INFO("[IIDDIAG] EditObject::undo  empty obj_iid (push site bug?)");
        return;
    }
    auto* obj = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!obj) {
        LOG_INFO("[IIDDIAG] EditObject::undo  iid='{}' resolved=nullptr",
                 obj_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] EditObject::undo  iid='{}' resolved name='{}' "
             "type={} live_iid='{}' has_path={}",
             obj_iid, obj->name, (int)obj->type, obj->internal_id,
             obj->path ? 1 : 0);
    if (obj->path) *obj->path = path_before;
    obj->fill              = fill_before;
    obj->stroke            = stroke_before;
    obj->fill_swatch_id    = fill_swatch_id_before;
    obj->stroke_swatch_id  = stroke_swatch_id_before;
    obj->bound_style       = bound_style_before;
    obj->write_shadow(shadow_before);
}

// ── TextEditCommand bodies (s167 m2) ─────────────────────────────────
// Resolves the iid then delegates to the existing `apply()` member,
// which handles the field-by-field write for either direction. A
// default-constructed TextEditCommand (Canvas's m_text_snapshot in its
// idle state) has empty obj_iid — early-return makes execute/undo a
// clean no-op in that case, no log spam.
void TextEditCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] TextEdit::exec  proj=NULL  iid='{}'", obj_iid);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        // Default-constructed snapshot replay — clean no-op, expected.
        LOG_INFO("[IIDDIAG] TextEdit::exec  empty obj_iid "
                 "(default-ctor replay or push site bug?)");
        return;
    }
    auto* obj = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!obj) {
        LOG_INFO("[IIDDIAG] TextEdit::exec  iid='{}' resolved=nullptr",
                 obj_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] TextEdit::exec  iid='{}' resolved name='{}' "
             "type={} live_iid='{}'  before='{}' after='{}'",
             obj_iid, obj->name, (int)obj->type, obj->internal_id,
             before_content, after_content);
    apply(obj, /*after=*/true);
}

void TextEditCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] TextEdit::undo  proj=NULL  iid='{}'", obj_iid);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        LOG_INFO("[IIDDIAG] TextEdit::undo  empty obj_iid "
                 "(default-ctor replay or push site bug?)");
        return;
    }
    auto* obj = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!obj) {
        LOG_INFO("[IIDDIAG] TextEdit::undo  iid='{}' resolved=nullptr",
                 obj_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] TextEdit::undo  iid='{}' resolved name='{}' "
             "type={} live_iid='{}'  before='{}' after='{}'",
             obj_iid, obj->name, (int)obj->type, obj->internal_id,
             before_content, after_content);
    apply(obj, /*after=*/false);
}

// ── RefMoveCommand bodies (s167 m2) ──────────────────────────────────
// Two-axis ref-point move on a Ref node. Smallest of the migrated
// commands — four doubles plus identity.
void RefMoveCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] RefMove::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (node_iid.empty()) {
        LOG_INFO("[IIDDIAG] RefMove::exec  empty node_iid (push site bug?)");
        return;
    }
    auto* node = curvz::utils::find_by_iid(*proj, node_iid);
    if (!node) {
        LOG_INFO("[IIDDIAG] RefMove::exec  iid='{}' resolved=nullptr "
                 "(node gone)  before=({},{}) after=({},{})",
                 node_iid, before_x, before_y, after_x, after_y);
        return;
    }
    LOG_INFO("[IIDDIAG] RefMove::exec  iid='{}' resolved name='{}' "
             "type={} live_iid='{}'  cur=({},{}) -> writing after=({},{})",
             node_iid, node->name, (int)node->type, node->internal_id,
             node->ref_x, node->ref_y, after_x, after_y);
    node->ref_x = after_x;
    node->ref_y = after_y;
}

void RefMoveCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] RefMove::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (node_iid.empty()) {
        LOG_INFO("[IIDDIAG] RefMove::undo  empty node_iid (push site bug?)");
        return;
    }
    auto* node = curvz::utils::find_by_iid(*proj, node_iid);
    if (!node) {
        LOG_INFO("[IIDDIAG] RefMove::undo  iid='{}' resolved=nullptr "
                 "(node gone)  before=({},{}) after=({},{})",
                 node_iid, before_x, before_y, after_x, after_y);
        return;
    }
    LOG_INFO("[IIDDIAG] RefMove::undo  iid='{}' resolved name='{}' "
             "type={} live_iid='{}'  cur=({},{}) -> writing before=({},{})",
             node_iid, node->name, (int)node->type, node->internal_id,
             node->ref_x, node->ref_y, before_x, before_y);
    node->ref_x = before_x;
    node->ref_y = before_y;
}

// ── GuideMoveCommand bodies (s180 m1) ────────────────────────────────
// Axis-aligned and angled guide moves. iid-resolves the guide on every
// execute/undo so layer-deletion or guide-deletion between push and
// replay is a clean no-op. Mirrors RefMoveCommand's shape; the only
// addition is the angle field, carried for symmetry with the inspector's
// X/Y/A editor even though current canvas drag mutates only x/y.
void GuideMoveCommand::execute() {
    if (!proj) return;
    invalidate_iid_indexes(proj);
    if (node_iid.empty()) return;
    auto* node = curvz::utils::find_by_iid(*proj, node_iid);
    if (!node) return;
    node->guide_x     = after_x;
    node->guide_y     = after_y;
    node->guide_angle = after_angle;
}

void GuideMoveCommand::undo() {
    if (!proj) return;
    invalidate_iid_indexes(proj);
    if (node_iid.empty()) return;
    auto* node = curvz::utils::find_by_iid(*proj, node_iid);
    if (!node) return;
    node->guide_x     = before_x;
    node->guide_y     = before_y;
    node->guide_angle = before_angle;
}

// ── MoveObjectCommand bodies (s169 m1) ───────────────────────────────
// Text-anchor and image-origin moves. Branches on the resolved node's
// type — text writes text_x/text_y, image writes image_x/image_y. The
// pre-migration code branched the same way; only the resolution path
// changes. If the node is gone, no-op cleanly. If the node has somehow
// been retyped between capture and replay (shouldn't happen — type is
// immutable post-construction), neither branch fires and we no-op.
void MoveObjectCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] MoveObject::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        LOG_INFO("[IIDDIAG] MoveObject::exec  empty obj_iid (push site bug?)");
        return;
    }
    auto* obj = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!obj) {
        LOG_INFO("[IIDDIAG] MoveObject::exec  iid='{}' resolved=nullptr "
                 "(node gone)  before=({},{}) after=({},{})",
                 obj_iid, before_x, before_y, after_x, after_y);
        return;
    }
    LOG_INFO("[IIDDIAG] MoveObject::exec  iid='{}' resolved name='{}' "
             "type={} live_iid='{}'  writing after=({},{})",
             obj_iid, obj->name, (int)obj->type, obj->internal_id,
             after_x, after_y);
    if (obj->is_text())  { obj->text_x  = after_x;  obj->text_y  = after_y;  }
    if (obj->is_image()) { obj->image_x = after_x;  obj->image_y = after_y; }
}

void MoveObjectCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] MoveObject::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        LOG_INFO("[IIDDIAG] MoveObject::undo  empty obj_iid (push site bug?)");
        return;
    }
    auto* obj = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!obj) {
        LOG_INFO("[IIDDIAG] MoveObject::undo  iid='{}' resolved=nullptr "
                 "(node gone)  before=({},{}) after=({},{})",
                 obj_iid, before_x, before_y, after_x, after_y);
        return;
    }
    LOG_INFO("[IIDDIAG] MoveObject::undo  iid='{}' resolved name='{}' "
             "type={} live_iid='{}'  writing before=({},{})",
             obj_iid, obj->name, (int)obj->type, obj->internal_id,
             before_x, before_y);
    if (obj->is_text())  { obj->text_x  = before_x; obj->text_y  = before_y; }
    if (obj->is_image()) { obj->image_x = before_x; obj->image_y = before_y; }
}

// ── ZOrderCommand bodies (s169 m2) ───────────────────────────────────
// Resolve the parent layer iid, then call the static apply_order helper
// with the captured id-order vector. If the layer is gone, no-op cleanly.
// The id-order vector itself holds SVG ids (strings, not pointers) so it
// stays valid across destructive ops; only the live `parent` pointer
// needed iid-resolution.
void ZOrderCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] ZOrder::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] ZOrder::exec  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] ZOrder::exec  iid='{}' resolved=nullptr "
                 "(layer gone)  desc='{}'", parent_iid, desc);
        return;
    }
    LOG_INFO("[IIDDIAG] ZOrder::exec  iid='{}' resolved name='{}' "
             "type={} live_iid='{}'  applying after_order ({} children)",
             parent_iid, parent->name, (int)parent->type, parent->internal_id,
             after_order.size());
    apply_order(parent, after_order);
}

void ZOrderCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] ZOrder::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] ZOrder::undo  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] ZOrder::undo  iid='{}' resolved=nullptr "
                 "(layer gone)  desc='{}'", parent_iid, desc);
        return;
    }
    LOG_INFO("[IIDDIAG] ZOrder::undo  iid='{}' resolved name='{}' "
             "type={} live_iid='{}'  applying before_order ({} children)",
             parent_iid, parent->name, (int)parent->type, parent->internal_id,
             before_order.size());
    apply_order(parent, before_order);
}

// ── AddLayerCommand bodies (s171 m1) ─────────────────────────────────
// First of the layer-undoable thread. Resolution shape is doc-relative:
// resolve the anchor iid to a CurvzDocument*, then operate on
// doc->layers directly (top-level, no parent SceneNode needed).
//
// execute() = redo: insert a clone of the snapshot at the recorded
//   index. active_layer_index restored to active_after (the post-op
//   value at original push time).
//
// undo() = remove the layer we added. Look up the live layer by iid in
//   the same doc the anchor lives in; if found, erase. active_layer_index
//   restored to active_before.
//
// Doc-gone path: no-op cleanly. find_doc_by_iid returns nullptr if the
// anchor's doc has been closed; we log and return without touching
// anything.
void AddLayerCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] AddLayer::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (anchor_iid.empty() || !snap) {
        LOG_INFO("[IIDDIAG] AddLayer::exec  empty anchor_iid or snap "
                 "(push site bug?)");
        return;
    }
    auto* doc = curvz::utils::find_doc_by_iid(*proj, anchor_iid);
    if (!doc) {
        LOG_INFO("[IIDDIAG] AddLayer::exec  anchor_iid='{}' resolved=nullptr "
                 "(doc gone) — skipping", anchor_iid);
        return;
    }
    int ins = std::clamp(index, 0, (int)doc->layers.size());
    LOG_INFO("[IIDDIAG] AddLayer::exec  anchor_iid='{}' new_iid='{}' "
             "inserting at {} (active_after={})",
             anchor_iid, new_layer_iid, ins, active_after);
    doc->layers.insert(doc->layers.begin() + ins, clone_node(*snap));
    doc->active_layer_index = active_after;
    doc->invalidate_iid_index();
}

void AddLayerCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] AddLayer::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (anchor_iid.empty() || new_layer_iid.empty()) {
        LOG_INFO("[IIDDIAG] AddLayer::undo  empty anchor_iid or new_layer_iid "
                 "(push site bug?)");
        return;
    }
    auto* doc = curvz::utils::find_doc_by_iid(*proj, anchor_iid);
    if (!doc) {
        LOG_INFO("[IIDDIAG] AddLayer::undo  anchor_iid='{}' resolved=nullptr "
                 "(doc gone) — skipping", anchor_iid);
        return;
    }
    int idx = curvz::utils::find_layer_index_by_iid(*doc, new_layer_iid);
    if (idx < 0) {
        LOG_INFO("[IIDDIAG] AddLayer::undo  new_layer_iid='{}' not in doc "
                 "(layer already gone) — skipping erase, restoring active",
                 new_layer_iid);
        doc->active_layer_index = active_before;
        doc->invalidate_iid_index();
        return;
    }
    LOG_INFO("[IIDDIAG] AddLayer::undo  new_layer_iid='{}' erasing at {} "
             "(active_before={})", new_layer_iid, idx, active_before);
    doc->layers.erase(doc->layers.begin() + idx);
    doc->active_layer_index = std::clamp(active_before, 0,
                                         std::max(0, (int)doc->layers.size()-1));
    doc->invalidate_iid_index();
}

// ── DeleteLayerCommand bodies (s171 m1) ──────────────────────────────
// Mirror of AddLayer. execute() = redo deletes the layer; undo() puts
// it back at its original index.
//
// The anchor mechanism is essential here: at delete time, the deleted
// layer's iid is no longer in the doc, so we can't use it for doc
// resolution. Anchor is a peer layer that survives the delete (the
// caller guarantees one — the delete handler refuses if it would leave
// zero normal layers).
//
// On undo, the anchor still resolves to its doc, and we re-insert the
// snapshot's clone at the recorded index. Crucially, the snapshot
// preserves all child iids — any commands queued on those children
// (transforms, edits, etc.) will resolve correctly when undo walks
// further back through the history.
void DeleteLayerCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] DeleteLayer::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (anchor_iid.empty() || deleted_iid.empty()) {
        LOG_INFO("[IIDDIAG] DeleteLayer::exec  empty anchor or deleted iid "
                 "(push site bug?)");
        return;
    }
    auto* doc = curvz::utils::find_doc_by_iid(*proj, anchor_iid);
    if (!doc) {
        LOG_INFO("[IIDDIAG] DeleteLayer::exec  anchor_iid='{}' resolved=nullptr "
                 "(doc gone) — skipping", anchor_iid);
        return;
    }
    int idx = curvz::utils::find_layer_index_by_iid(*doc, deleted_iid);
    if (idx < 0) {
        LOG_INFO("[IIDDIAG] DeleteLayer::exec  deleted_iid='{}' not in doc "
                 "(already gone?) — skipping erase, applying active",
                 deleted_iid);
        doc->active_layer_index = active_after;
        doc->invalidate_iid_index();
        return;
    }
    LOG_INFO("[IIDDIAG] DeleteLayer::exec  deleted_iid='{}' erasing at {} "
             "(active_after={})", deleted_iid, idx, active_after);
    doc->layers.erase(doc->layers.begin() + idx);
    doc->active_layer_index = std::clamp(active_after, 0,
                                         std::max(0, (int)doc->layers.size()-1));
    doc->invalidate_iid_index();
}

void DeleteLayerCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] DeleteLayer::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (anchor_iid.empty() || !snap) {
        LOG_INFO("[IIDDIAG] DeleteLayer::undo  empty anchor_iid or snap "
                 "(push site bug?)");
        return;
    }
    auto* doc = curvz::utils::find_doc_by_iid(*proj, anchor_iid);
    if (!doc) {
        LOG_INFO("[IIDDIAG] DeleteLayer::undo  anchor_iid='{}' resolved=nullptr "
                 "(doc gone) — skipping", anchor_iid);
        return;
    }
    int ins = std::clamp(index, 0, (int)doc->layers.size());
    LOG_INFO("[IIDDIAG] DeleteLayer::undo  deleted_iid='{}' restoring at {} "
             "(active_before={})", deleted_iid, ins, active_before);
    doc->layers.insert(doc->layers.begin() + ins, clone_node(*snap));
    doc->active_layer_index = active_before;
    doc->invalidate_iid_index();
}

// ── MoveObjectToLayerCommand bodies (s171 m2) ────────────────────────
// Cross-layer object move. execute() = redo: erase by obj_iid from
// src_layer, insert clone at dst_index in dst_layer. undo() = reverse:
// erase by obj_iid from dst_layer, insert clone at src_index in
// src_layer. Both directions partial-recover on missing layer iids.
//
// Why we erase-by-iid instead of erase-by-index: the index is the
// snapshot of where the object lived at push time, but other commands
// queued between push and replay (z-order, paste, cut) may have
// shuffled siblings. Erase-by-iid is robust against that shuffle;
// the recorded index is only used as the *insertion* hint in the
// destination, where it's clamped anyway.
//
// Why we insert a clone of `snap` rather than holding a unique_ptr we
// move around: redo can fire after undo (which already moved the object
// back), and the live object's identity in memory is whatever the doc
// holds. Using a clone keeps the snapshot intact and survives any
// number of execute/undo round-trips.
void MoveObjectToLayerCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] MoveToLayer::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (src_layer_iid.empty() || dst_layer_iid.empty() || obj_iid.empty()
        || !snap) {
        LOG_INFO("[IIDDIAG] MoveToLayer::exec  missing required iid/snap "
                 "(push site bug?)");
        return;
    }
    auto* src = curvz::utils::find_by_iid(*proj, src_layer_iid);
    auto* dst = curvz::utils::find_by_iid(*proj, dst_layer_iid);
    if (!src || !dst) {
        LOG_INFO("[IIDDIAG] MoveToLayer::exec  src='{}' dst='{}' "
                 "src_resolved={} dst_resolved={} — skipping",
                 src_layer_iid, dst_layer_iid,
                 src ? "yes" : "NO", dst ? "yes" : "NO");
        return;
    }
    // Erase by iid from src (skip silently if not there — already moved
    // by a prior op; partial-recovery shape).
    auto& src_ch = src->children;
    bool erased = false;
    for (int i = (int)src_ch.size()-1; i >= 0; --i) {
        if (src_ch[i] && src_ch[i]->internal_id == obj_iid) {
            src_ch.erase(src_ch.begin() + i);
            erased = true;
            break;
        }
    }
    int ins = std::clamp(dst_index, 0, (int)dst->children.size());
    LOG_INFO("[IIDDIAG] MoveToLayer::exec  obj_iid='{}' src='{}'->dst='{}' "
             "erased_from_src={} inserting at dst_idx={}",
             obj_iid, src_layer_iid, dst_layer_iid,
             erased ? "yes" : "no", ins);
    dst->children.insert(dst->children.begin() + ins, clone_node(*snap));
}

void MoveObjectToLayerCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] MoveToLayer::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (src_layer_iid.empty() || dst_layer_iid.empty() || obj_iid.empty()
        || !snap) {
        LOG_INFO("[IIDDIAG] MoveToLayer::undo  missing required iid/snap "
                 "(push site bug?)");
        return;
    }
    auto* src = curvz::utils::find_by_iid(*proj, src_layer_iid);
    auto* dst = curvz::utils::find_by_iid(*proj, dst_layer_iid);
    if (!src || !dst) {
        LOG_INFO("[IIDDIAG] MoveToLayer::undo  src='{}' dst='{}' "
                 "src_resolved={} dst_resolved={} — skipping",
                 src_layer_iid, dst_layer_iid,
                 src ? "yes" : "NO", dst ? "yes" : "NO");
        return;
    }
    // Erase by iid from dst.
    auto& dst_ch = dst->children;
    bool erased = false;
    for (int i = (int)dst_ch.size()-1; i >= 0; --i) {
        if (dst_ch[i] && dst_ch[i]->internal_id == obj_iid) {
            dst_ch.erase(dst_ch.begin() + i);
            erased = true;
            break;
        }
    }
    int ins = std::clamp(src_index, 0, (int)src->children.size());
    LOG_INFO("[IIDDIAG] MoveToLayer::undo  obj_iid='{}' dst='{}'->src='{}' "
             "erased_from_dst={} inserting at src_idx={}",
             obj_iid, dst_layer_iid, src_layer_iid,
             erased ? "yes" : "no", ins);
    src->children.insert(src->children.begin() + ins, clone_node(*snap));
}

// ── ReorderLayersCommand bodies (s172 m3) ────────────────────────────
// Final structural piece of the layer-undoable arc. Closes the last
// "direct mutation of doc->layers" path (DnD layer reorder).
//
// Apply algorithm: take everything out of doc->layers into a temp map
// keyed by iid, then push back in the recorded target order. Layers
// missing from the target sequence (because they were deleted by some
// other command sitting between this and replay) are skipped on the
// pull-from-map side — they get appended at the tail to keep the doc
// shape valid. Layers in the target sequence whose iids no longer
// resolve in the map (deleted before replay) are silently skipped.
//
// Same algorithm in both directions, only the target sequence differs:
// execute() (= redo) targets iids_after; undo() targets iids_before.
//
// The `move into temp map -> push back in order` pattern is necessary
// because std::vector<unique_ptr> can't be rearranged in place by iid
// without first moving things out — unique_ptr is non-copyable, so any
// in-place permutation either juggles via swap (which requires solving
// the permutation up front) or routes through a temp buffer. The temp
// map is the cleanest version: O(L) extraction, O(L) re-insertion, O(L)
// total memory — trivial at typical layer counts (<20).
namespace {
// Apply a target iid sequence to doc->layers. Move out of doc->layers
// into a map<iid, unique_ptr>, then push back in the order specified by
// `target`. iids in `target` that aren't in the map are skipped; layers
// in the map whose iids aren't in `target` get appended at the tail.
void apply_layer_sequence(CurvzDocument* doc,
                          const std::vector<std::string>& target) {
    if (!doc) return;
    // Extract every layer into a temp map keyed by iid. Doc shape goes
    // empty during the rearrangement.
    //
    // Layers with empty internal_id (degenerate / pre-iid legacy) are
    // held aside in a separate vector and appended at the tail rather
    // than packed into the map under key "" — empty key would collide
    // and silently drop all but the last such layer.
    std::unordered_map<std::string, std::unique_ptr<SceneNode>> by_iid;
    std::vector<std::unique_ptr<SceneNode>> orphans;
    by_iid.reserve(doc->layers.size());
    for (auto& ly : doc->layers) {
        if (!ly) continue;
        if (ly->internal_id.empty()) {
            orphans.push_back(std::move(ly));
            continue;
        }
        std::string iid = ly->internal_id;  // stable copy before move
        by_iid.emplace(std::move(iid), std::move(ly));
    }
    doc->layers.clear();
    doc->layers.reserve(by_iid.size() + orphans.size());
    // Push back in target order, consuming the map.
    int skipped_target = 0;
    for (const auto& iid : target) {
        if (iid.empty()) { ++skipped_target; continue; }
        auto it = by_iid.find(iid);
        if (it == by_iid.end()) {
            ++skipped_target;
            continue;  // iid in target sequence no longer in doc — skip
        }
        doc->layers.push_back(std::move(it->second));
        by_iid.erase(it);
    }
    // Append any leftovers (layers added since this command was pushed
    // and therefore not in the recorded target sequence). Tail position
    // is arbitrary but defensible — they exist post-op, so don't drop
    // them.
    int leftover_count = 0;
    for (auto& kv : by_iid) {
        if (kv.second) {
            doc->layers.push_back(std::move(kv.second));
            ++leftover_count;
        }
    }
    // Empty-iid orphans tagged on at the very tail.
    for (auto& o : orphans) {
        if (o) {
            doc->layers.push_back(std::move(o));
            ++leftover_count;
        }
    }
    if (skipped_target || leftover_count) {
        LOG_INFO("[IIDDIAG] ReorderLayers::apply  skipped_target={} "
                 "leftover_appended={} final_size={}",
                 skipped_target, leftover_count, (int)doc->layers.size());
    }
    doc->invalidate_iid_index();
}
}  // namespace

void ReorderLayersCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] ReorderLayers::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (anchor_iid.empty() || iids_after.empty()) {
        LOG_INFO("[IIDDIAG] ReorderLayers::exec  empty anchor_iid or "
                 "iids_after (push site bug?)");
        return;
    }
    auto* doc = curvz::utils::find_doc_by_iid(*proj, anchor_iid);
    if (!doc) {
        LOG_INFO("[IIDDIAG] ReorderLayers::exec  anchor_iid='{}' "
                 "resolved=nullptr (doc gone) — skipping", anchor_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] ReorderLayers::exec  anchor_iid='{}' "
             "applying iids_after (n={}) active_after={}",
             anchor_iid, (int)iids_after.size(), active_after);
    apply_layer_sequence(doc, iids_after);
    doc->active_layer_index = std::clamp(active_after, 0,
                                         std::max(0, (int)doc->layers.size()-1));
}

void ReorderLayersCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] ReorderLayers::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (anchor_iid.empty() || iids_before.empty()) {
        LOG_INFO("[IIDDIAG] ReorderLayers::undo  empty anchor_iid or "
                 "iids_before (push site bug?)");
        return;
    }
    auto* doc = curvz::utils::find_doc_by_iid(*proj, anchor_iid);
    if (!doc) {
        LOG_INFO("[IIDDIAG] ReorderLayers::undo  anchor_iid='{}' "
                 "resolved=nullptr (doc gone) — skipping", anchor_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] ReorderLayers::undo  anchor_iid='{}' "
             "applying iids_before (n={}) active_before={}",
             anchor_iid, (int)iids_before.size(), active_before);
    apply_layer_sequence(doc, iids_before);
    doc->active_layer_index = std::clamp(active_before, 0,
                                         std::max(0, (int)doc->layers.size()-1));
}

// ── EditLayerFieldCommand bodies (s172 m4) ───────────────────────────
// One body for all four fields. Resolve the layer by iid (layers are in
// the iid index), then either set the relevant field to `after_*` (on
// execute / redo) or to `before_*` (on undo). Skip silently if the
// layer no longer resolves (deleted by another command between push and
// replay) — same partial-recovery shape as Cut/Duplicate.
//
// Why we apply explicit before/after rather than toggling: toggle works
// for bool fields but not strings (rename/color), and inverting a bool
// twice doesn't compose with a partial-recovery skip. Explicit values
// make replay idempotent — running execute() twice ends in the same
// state as running it once.
namespace {
void apply_layer_field(SceneNode* layer,
                       EditLayerFieldCommand::Field field,
                       const std::string& s, bool b) {
    using Field = EditLayerFieldCommand::Field;
    switch (field) {
        case Field::Name:    layer->name    = s; break;
        case Field::Color:   layer->color   = s; break;
        case Field::Visible: layer->visible = b; break;
        case Field::Locked:  layer->locked  = b; break;
    }
}

const char* field_name(EditLayerFieldCommand::Field f) {
    using Field = EditLayerFieldCommand::Field;
    switch (f) {
        case Field::Name:    return "Name";
        case Field::Color:   return "Color";
        case Field::Visible: return "Visible";
        case Field::Locked:  return "Locked";
    }
    return "?";
}
}  // namespace

void EditLayerFieldCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] EditLayerField::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (layer_iid.empty()) {
        LOG_INFO("[IIDDIAG] EditLayerField::exec  empty layer_iid "
                 "(push site bug?)");
        return;
    }
    auto* layer = curvz::utils::find_by_iid(*proj, layer_iid);
    if (!layer) {
        LOG_INFO("[IIDDIAG] EditLayerField::exec  layer_iid='{}' "
                 "resolved=nullptr (layer gone) — skipping", layer_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] EditLayerField::exec  layer_iid='{}' field={} "
             "applying after",
             layer_iid, field_name(field));
    apply_layer_field(layer, field, after_str, after_bool);
}

void EditLayerFieldCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] EditLayerField::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (layer_iid.empty()) {
        LOG_INFO("[IIDDIAG] EditLayerField::undo  empty layer_iid "
                 "(push site bug?)");
        return;
    }
    auto* layer = curvz::utils::find_by_iid(*proj, layer_iid);
    if (!layer) {
        LOG_INFO("[IIDDIAG] EditLayerField::undo  layer_iid='{}' "
                 "resolved=nullptr (layer gone) — skipping", layer_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] EditLayerField::undo  layer_iid='{}' field={} "
             "applying before",
             layer_iid, field_name(field));
    apply_layer_field(layer, field, before_str, before_bool);
}

// ── PasteCommand bodies (s169 m3) ────────────────────────────────────
// Resolve the parent layer iid, then insert clones of each snapshot at
// the front of children (execute) or erase by id (undo). If the layer
// is gone, no-op cleanly. The snaps vector itself holds unique_ptrs to
// deep clones, so it stays valid across destructive ops; only the live
// `parent` pointer needed iid-resolution.
void PasteCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] Paste::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] Paste::exec  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] Paste::exec  iid='{}' resolved=nullptr "
                 "(layer gone)  snaps={}", parent_iid, snaps.size());
        return;
    }
    LOG_INFO("[IIDDIAG] Paste::exec  iid='{}' resolved name='{}' "
             "type={} live_iid='{}'  inserting {} snaps",
             parent_iid, parent->name, (int)parent->type, parent->internal_id,
             snaps.size());
    // Insert each in reverse so first snap ends up at front
    for (int i = (int)snaps.size()-1; i >= 0; --i)
        parent->children.insert(parent->children.begin(), clone_node(*snaps[i]));
}

void PasteCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] Paste::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] Paste::undo  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] Paste::undo  iid='{}' resolved=nullptr "
                 "(layer gone)  snaps={}", parent_iid, snaps.size());
        return;
    }
    LOG_INFO("[IIDDIAG] Paste::undo  iid='{}' resolved name='{}' "
             "type={} live_iid='{}'  erasing {} snaps",
             parent_iid, parent->name, (int)parent->type, parent->internal_id,
             snaps.size());
    auto& ch = parent->children;
    for (auto& s : snaps)
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i]->id == s->id) { ch.erase(ch.begin()+i); break; }
}

// ── CutCommand bodies (s170 m2) ──────────────────────────────────────
// First Stage 3 command with per-Entry parent_iid (different parents
// possible — selecting across layers and Cut). Per-Entry resolution +
// per-Entry no-op-on-miss, mirroring m4/m5/m6 partial-recovery
// semantics; only here each Entry holds its own resolution key.
//
// execute() = redo: re-cuts. Walks the live parent's children, finds
// match by snap's SVG id, erases. Operates in descending index order
// across all entries to avoid shift hazards within any one parent.
//
// undo()    = re-insert. Inserts a clone at the recorded index in the
// live parent. Operates in ascending index order so earlier-index
// inserts don't shift later-index ones.
void CutCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] Cut::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] Cut::exec  applying {} entries", entries.size());
    // Sort entry indices descending for safe removal; iterate that order.
    std::vector<int> order(entries.size());
    for (int i = 0; i < (int)entries.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
        [&](int a, int b){ return entries[a].index > entries[b].index; });
    for (int i : order) {
        auto& e = entries[i];
        if (e.parent_iid.empty()) {
            LOG_INFO("[IIDDIAG] Cut::exec  entry has empty parent_iid "
                     "(push site bug?)");
            continue;
        }
        auto* parent = curvz::utils::find_by_iid(*proj, e.parent_iid);
        if (!parent) {
            LOG_INFO("[IIDDIAG] Cut::exec  parent_iid='{}' resolved=nullptr "
                     "(layer gone) — skipping entry", e.parent_iid);
            continue;
        }
        auto& ch = parent->children;
        for (int j = (int)ch.size()-1; j >= 0; --j) {
            if (ch[j]->id == e.snap->id) {
                ch.erase(ch.begin()+j);
                break;
            }
        }
    }
}

void CutCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] Cut::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] Cut::undo  reverting {} entries", entries.size());
    // Sort entry indices ascending for safe insertion; iterate that order.
    std::vector<int> order(entries.size());
    for (int i = 0; i < (int)entries.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
        [&](int a, int b){ return entries[a].index < entries[b].index; });
    for (int i : order) {
        auto& e = entries[i];
        if (e.parent_iid.empty()) {
            LOG_INFO("[IIDDIAG] Cut::undo  entry has empty parent_iid "
                     "(push site bug?)");
            continue;
        }
        auto* parent = curvz::utils::find_by_iid(*proj, e.parent_iid);
        if (!parent) {
            LOG_INFO("[IIDDIAG] Cut::undo  parent_iid='{}' resolved=nullptr "
                     "(layer gone) — skipping entry", e.parent_iid);
            continue;
        }
        auto& ch = parent->children;
        int ins = std::clamp(e.index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*e.snap));
    }
}

// ── DuplicateCommand bodies (s170 m3) ────────────────────────────────
// Mirror image of Cut (s170 m2): per-Entry parent_iid resolution, but
// execute() inserts and undo() erases (Cut's the other way around).
//
// execute() = redo: re-inserts each duplicate at its recorded index.
// Operates in descending index order across entries so earlier high-
// index inserts don't shift later low-index ones (matches the original
// in-place behavior — see comment in pre-migration code).
//
// undo()    = remove each duplicate by snap's SVG id. Order doesn't
// matter for erase-by-id (each lookup is independent).
void DuplicateCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] Duplicate::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] Duplicate::exec  applying {} entries", entries.size());
    // Sort entry indices descending for safe insertion across multiple
    // entries within the same parent.
    std::vector<int> order(entries.size());
    for (int i = 0; i < (int)entries.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
        [&](int a, int b){ return entries[a].index > entries[b].index; });
    for (int i : order) {
        auto& e = entries[i];
        if (e.parent_iid.empty()) {
            LOG_INFO("[IIDDIAG] Duplicate::exec  entry has empty parent_iid "
                     "(push site bug?)");
            continue;
        }
        auto* parent = curvz::utils::find_by_iid(*proj, e.parent_iid);
        if (!parent) {
            LOG_INFO("[IIDDIAG] Duplicate::exec  parent_iid='{}' resolved="
                     "nullptr (layer gone) — skipping entry", e.parent_iid);
            continue;
        }
        auto& ch = parent->children;
        int ins = std::clamp(e.index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*e.snap));
    }
}

void DuplicateCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] Duplicate::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] Duplicate::undo  reverting {} entries", entries.size());
    for (auto& e : entries) {
        if (e.parent_iid.empty()) {
            LOG_INFO("[IIDDIAG] Duplicate::undo  entry has empty parent_iid "
                     "(push site bug?)");
            continue;
        }
        auto* parent = curvz::utils::find_by_iid(*proj, e.parent_iid);
        if (!parent) {
            LOG_INFO("[IIDDIAG] Duplicate::undo  parent_iid='{}' resolved="
                     "nullptr (layer gone) — skipping entry", e.parent_iid);
            continue;
        }
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i) {
            if (ch[i]->id == e.snap->id) {
                ch.erase(ch.begin()+i);
                break;
            }
        }
    }
}

// ── CornerTreatmentCommand bodies (s169 m4) ──────────────────────────
// First multi-target migration. Each Entry resolves independently; if a
// resolution misses, that entry no-ops and the loop continues. The
// invalidate-and-resolve pattern stays the same as the single-target
// commands; only the loop structure differs.
//
// Loop runs `find_by_iid` per entry. For typical corner-treatment ops
// (handful of selected paths), this is N small map lookups. For very
// large selections we could rebuild the index once and use it directly,
// but that's optimization the current scale doesn't need.
void CornerTreatmentCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] CornerTreatment::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] CornerTreatment::exec  applying {} entries",
             entries.size());
    for (auto& e : entries) {
        if (e.obj_iid.empty()) {
            LOG_INFO("[IIDDIAG] CornerTreatment::exec  entry has empty iid "
                     "(push site bug?)");
            continue;
        }
        auto* obj = curvz::utils::find_by_iid(*proj, e.obj_iid);
        if (!obj) {
            LOG_INFO("[IIDDIAG] CornerTreatment::exec  iid='{}' "
                     "resolved=nullptr (path gone) — skipping entry",
                     e.obj_iid);
            continue;
        }
        if (!obj->path) {
            LOG_INFO("[IIDDIAG] CornerTreatment::exec  iid='{}' name='{}' "
                     "resolved but has no path — skipping entry",
                     e.obj_iid, obj->name);
            continue;
        }
        *obj->path = e.after;
    }
}

void CornerTreatmentCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] CornerTreatment::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] CornerTreatment::undo  reverting {} entries",
             entries.size());
    for (auto& e : entries) {
        if (e.obj_iid.empty()) {
            LOG_INFO("[IIDDIAG] CornerTreatment::undo  entry has empty iid "
                     "(push site bug?)");
            continue;
        }
        auto* obj = curvz::utils::find_by_iid(*proj, e.obj_iid);
        if (!obj) {
            LOG_INFO("[IIDDIAG] CornerTreatment::undo  iid='{}' "
                     "resolved=nullptr (path gone) — skipping entry",
                     e.obj_iid);
            continue;
        }
        if (!obj->path) {
            LOG_INFO("[IIDDIAG] CornerTreatment::undo  iid='{}' name='{}' "
                     "resolved but has no path — skipping entry",
                     e.obj_iid, obj->name);
            continue;
        }
        *obj->path = e.before;
    }
}

// ── AlignObjectsCommand bodies (s169 m5) ─────────────────────────────
// Same shape as CornerTreatmentCommand: per-LeafSnap iid resolution,
// per-entry no-op on miss. desc string carried for logs.
void AlignObjectsCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] Align::exec  proj=NULL  desc='{}'", desc);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] Align::exec  desc='{}'  applying {} snaps",
             desc, snaps.size());
    for (auto& s : snaps) {
        if (s.obj_iid.empty()) {
            LOG_INFO("[IIDDIAG] Align::exec  snap has empty iid (push site bug?)");
            continue;
        }
        auto* obj = curvz::utils::find_by_iid(*proj, s.obj_iid);
        if (!obj) {
            LOG_INFO("[IIDDIAG] Align::exec  iid='{}' resolved=nullptr "
                     "(path gone) — skipping", s.obj_iid);
            continue;
        }
        if (!obj->path) {
            LOG_INFO("[IIDDIAG] Align::exec  iid='{}' name='{}' resolved but "
                     "has no path — skipping", s.obj_iid, obj->name);
            continue;
        }
        *obj->path = s.after;
    }
}

void AlignObjectsCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] Align::undo  proj=NULL  desc='{}'", desc);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] Align::undo  desc='{}'  reverting {} snaps",
             desc, snaps.size());
    for (auto& s : snaps) {
        if (s.obj_iid.empty()) {
            LOG_INFO("[IIDDIAG] Align::undo  snap has empty iid (push site bug?)");
            continue;
        }
        auto* obj = curvz::utils::find_by_iid(*proj, s.obj_iid);
        if (!obj) {
            LOG_INFO("[IIDDIAG] Align::undo  iid='{}' resolved=nullptr "
                     "(path gone) — skipping", s.obj_iid);
            continue;
        }
        if (!obj->path) {
            LOG_INFO("[IIDDIAG] Align::undo  iid='{}' name='{}' resolved but "
                     "has no path — skipping", s.obj_iid, obj->name);
            continue;
        }
        *obj->path = s.before;
    }
}

// ── ScaleObjectsCommand bodies (s169 m6) ─────────────────────────────
// Same shape as AlignObjectsCommand (s169 m5): per-LeafSnap iid resolution,
// per-entry no-op on miss. desc string carried for logs.
void ScaleObjectsCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] Scale::exec  proj=NULL  desc='{}'", desc);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] Scale::exec  desc='{}'  applying {} snaps",
             desc, snaps.size());
    for (auto& s : snaps) {
        if (s.obj_iid.empty()) {
            LOG_INFO("[IIDDIAG] Scale::exec  snap has empty iid (push site bug?)");
            continue;
        }
        auto* obj = curvz::utils::find_by_iid(*proj, s.obj_iid);
        if (!obj) {
            LOG_INFO("[IIDDIAG] Scale::exec  iid='{}' resolved=nullptr "
                     "(path gone) — skipping", s.obj_iid);
            continue;
        }
        if (!obj->path) {
            LOG_INFO("[IIDDIAG] Scale::exec  iid='{}' name='{}' resolved but "
                     "has no path — skipping", s.obj_iid, obj->name);
            continue;
        }
        *obj->path = s.after;
    }
}

void ScaleObjectsCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] Scale::undo  proj=NULL  desc='{}'", desc);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] Scale::undo  desc='{}'  reverting {} snaps",
             desc, snaps.size());
    for (auto& s : snaps) {
        if (s.obj_iid.empty()) {
            LOG_INFO("[IIDDIAG] Scale::undo  snap has empty iid (push site bug?)");
            continue;
        }
        auto* obj = curvz::utils::find_by_iid(*proj, s.obj_iid);
        if (!obj) {
            LOG_INFO("[IIDDIAG] Scale::undo  iid='{}' resolved=nullptr "
                     "(path gone) — skipping", s.obj_iid);
            continue;
        }
        if (!obj->path) {
            LOG_INFO("[IIDDIAG] Scale::undo  iid='{}' name='{}' resolved but "
                     "has no path — skipping", s.obj_iid, obj->name);
            continue;
        }
        *obj->path = s.before;
    }
}

// ── SetOpacityCommand bodies (s170 m1) ───────────────────────────────
// Same shape as Align (s169 m5) and Scale (s169 m6): per-Snap iid
// resolution, per-entry no-op on miss. Note the field touched is
// `obj->opacity` (a SceneNode field), not `obj->path` — so the
// `if (!obj->path)` guard from path-mutating commands is omitted here.
void SetOpacityCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] SetOpacity::exec  proj=NULL  desc='{}'", desc);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] SetOpacity::exec  desc='{}'  applying {} snaps",
             desc, snaps.size());
    for (auto& s : snaps) {
        if (s.obj_iid.empty()) {
            LOG_INFO("[IIDDIAG] SetOpacity::exec  snap has empty iid "
                     "(push site bug?)");
            continue;
        }
        auto* obj = curvz::utils::find_by_iid(*proj, s.obj_iid);
        if (!obj) {
            LOG_INFO("[IIDDIAG] SetOpacity::exec  iid='{}' resolved=nullptr "
                     "(node gone) — skipping", s.obj_iid);
            continue;
        }
        obj->opacity = s.after;
    }
}

void SetOpacityCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] SetOpacity::undo  proj=NULL  desc='{}'", desc);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] SetOpacity::undo  desc='{}'  reverting {} snaps",
             desc, snaps.size());
    for (auto& s : snaps) {
        if (s.obj_iid.empty()) {
            LOG_INFO("[IIDDIAG] SetOpacity::undo  snap has empty iid "
                     "(push site bug?)");
            continue;
        }
        auto* obj = curvz::utils::find_by_iid(*proj, s.obj_iid);
        if (!obj) {
            LOG_INFO("[IIDDIAG] SetOpacity::undo  iid='{}' resolved=nullptr "
                     "(node gone) — skipping", s.obj_iid);
            continue;
        }
        obj->opacity = s.before;
    }
}

// ── ScaleImageCommand bodies (s170 m4) ───────────────────────────────
// Same shape as SetOpacity (s170 m1): per-Snap iid resolution, per-entry
// no-op on miss. Note the fields touched are SceneNode image fields
// (image_x/y/w/h + transform) — the obj->path guard from path-mutating
// commands is omitted (image nodes have geometry stored on the node
// itself, not in a path).
void ScaleImageCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] ScaleImage::exec  proj=NULL  desc='{}'", desc);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] ScaleImage::exec  desc='{}'  applying {} snaps",
             desc, snaps.size());
    for (auto& s : snaps) {
        if (s.obj_iid.empty()) {
            LOG_INFO("[IIDDIAG] ScaleImage::exec  snap has empty iid "
                     "(push site bug?)");
            continue;
        }
        auto* obj = curvz::utils::find_by_iid(*proj, s.obj_iid);
        if (!obj) {
            LOG_INFO("[IIDDIAG] ScaleImage::exec  iid='{}' resolved=nullptr "
                     "(node gone) — skipping", s.obj_iid);
            continue;
        }
        obj->image_x = s.aft_x; obj->image_y = s.aft_y;
        obj->image_w = s.aft_w; obj->image_h = s.aft_h;
        obj->transform = s.aft_t;
    }
}

void ScaleImageCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] ScaleImage::undo  proj=NULL  desc='{}'", desc);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] ScaleImage::undo  desc='{}'  reverting {} snaps",
             desc, snaps.size());
    for (auto& s : snaps) {
        if (s.obj_iid.empty()) {
            LOG_INFO("[IIDDIAG] ScaleImage::undo  snap has empty iid "
                     "(push site bug?)");
            continue;
        }
        auto* obj = curvz::utils::find_by_iid(*proj, s.obj_iid);
        if (!obj) {
            LOG_INFO("[IIDDIAG] ScaleImage::undo  iid='{}' resolved=nullptr "
                     "(node gone) — skipping", s.obj_iid);
            continue;
        }
        obj->image_x = s.bef_x; obj->image_y = s.bef_y;
        obj->image_w = s.bef_w; obj->image_h = s.bef_h;
        obj->transform = s.bef_t;
    }
}

// ── BooleanOpCommand bodies (s173 m2) ────────────────────────────────
// Resolve the parent iid, then on execute remove originals and insert
// result clones at insert_index; on undo, remove results and reinsert
// originals at their captured indices. Same shape as PasteCommand at
// the resolution layer; same shape as the original inline bodies at
// the mutation layer (matching id-based child erase + clone_node insert
// pattern). If the parent layer is gone, both methods no-op cleanly.
//
// Note: results vector intentionally retains its clones across multiple
// execute()/undo() round trips. Each insert is `clone_node(*results[i])`
// (a fresh clone), preserving the originals for the next replay. Same
// for originals — `clone_node(*orig.snap)` produces a fresh insertable
// each time. This mirrors the inline pre-migration behaviour.
void BooleanOpCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] BooleanOp::exec  proj=NULL  desc='{}'", desc);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] BooleanOp::exec  empty parent_iid (push site bug?)  desc='{}'",
                 desc);
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] BooleanOp::exec  iid='{}' resolved=nullptr "
                 "(parent gone)  desc='{}'  originals={} results={}",
                 parent_iid, desc, originals.size(), results.size());
        return;
    }
    LOG_INFO("[IIDDIAG] BooleanOp::exec  iid='{}' resolved name='{}' "
             "live_iid='{}'  desc='{}'  originals={} results={}",
             parent_iid, parent->name, parent->internal_id,
             desc, originals.size(), results.size());

    auto& ch = parent->children;

    // Remove originals by SVG id
    for (auto& orig : originals) {
        if (!orig.snap) continue;
        for (int i = (int)ch.size()-1; i >= 0; --i) {
            if (ch[i] && ch[i]->id == orig.snap->id) {
                ch.erase(ch.begin()+i);
                break;
            }
        }
    }

    // Insert results at insert_index
    int ins = std::clamp(insert_index, 0, (int)ch.size());
    for (int i = 0; i < (int)results.size(); ++i) {
        if (!results[i]) continue;
        ch.insert(ch.begin() + ins + i, clone_node(*results[i]));
    }
}

void BooleanOpCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] BooleanOp::undo  proj=NULL  desc='{}'", desc);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] BooleanOp::undo  empty parent_iid (push site bug?)  desc='{}'",
                 desc);
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] BooleanOp::undo  iid='{}' resolved=nullptr "
                 "(parent gone)  desc='{}'  originals={} results={}",
                 parent_iid, desc, originals.size(), results.size());
        return;
    }
    LOG_INFO("[IIDDIAG] BooleanOp::undo  iid='{}' resolved name='{}' "
             "live_iid='{}'  desc='{}'  originals={} results={}",
             parent_iid, parent->name, parent->internal_id,
             desc, originals.size(), results.size());

    auto& ch = parent->children;

    // Remove results by SVG id
    for (auto& r : results) {
        if (!r) continue;
        for (int i = (int)ch.size()-1; i >= 0; --i) {
            if (ch[i] && ch[i]->id == r->id) {
                ch.erase(ch.begin()+i);
                break;
            }
        }
    }

    // Reinsert originals at their original positions
    for (auto& orig : originals) {
        if (!orig.snap) continue;
        int ins = std::clamp(orig.index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*orig.snap));
    }
}

// ── MakeBlendCommand bodies (s173 m3) ────────────────────────────────
// Resolve the parent iid; on execute remove the two originals and
// insert the Blend snap; on undo remove the Blend and reinsert both
// originals at their captured indices. Same shape as BooleanOpCommand
// at the resolution layer; same shape as the original inline bodies at
// the mutation layer. If the parent layer is gone, both methods no-op
// cleanly (pre-migration this would have crashed — no null-guard, no
// references() override, raw SceneNode* parent could dangle).
void MakeBlendCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] MakeBlend::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] MakeBlend::exec  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] MakeBlend::exec  iid='{}' resolved=nullptr "
                 "(parent gone)  originals={} blend_snap={}",
                 parent_iid, originals.size(),
                 blend_snap ? "present" : "null");
        return;
    }
    LOG_INFO("[IIDDIAG] MakeBlend::exec  iid='{}' resolved name='{}' "
             "live_iid='{}'  originals={} insert_index={}",
             parent_iid, parent->name, parent->internal_id,
             originals.size(), insert_index);

    auto& ch = parent->children;
    for (auto& orig : originals) {
        if (!orig.snap) continue;
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i] && ch[i]->id == orig.snap->id) {
                ch.erase(ch.begin()+i);
                break;
            }
    }
    if (!blend_snap) return;
    int ins = std::clamp(insert_index, 0, (int)ch.size());
    ch.insert(ch.begin() + ins, clone_node(*blend_snap));
}

void MakeBlendCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] MakeBlend::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] MakeBlend::undo  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] MakeBlend::undo  iid='{}' resolved=nullptr "
                 "(parent gone)  originals={} blend_snap={}",
                 parent_iid, originals.size(),
                 blend_snap ? "present" : "null");
        return;
    }
    LOG_INFO("[IIDDIAG] MakeBlend::undo  iid='{}' resolved name='{}' "
             "live_iid='{}'  originals={} insert_index={}",
             parent_iid, parent->name, parent->internal_id,
             originals.size(), insert_index);

    auto& ch = parent->children;
    if (blend_snap) {
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i] && ch[i]->id == blend_snap->id) {
                ch.erase(ch.begin()+i);
                break;
            }
    }
    for (auto& orig : originals) {
        if (!orig.snap) continue;
        int ins = std::clamp(orig.index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*orig.snap));
    }
}

// ── ReleaseBlendCommand bodies (s173 m3) ─────────────────────────────
// Inverse of MakeBlend — execute removes the Blend and inserts the
// three results at insert_index; undo removes the results and reinserts
// the Blend. Same iid-resolution pattern; same null-safe no-op on
// missing parent.
void ReleaseBlendCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] ReleaseBlend::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] ReleaseBlend::exec  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] ReleaseBlend::exec  iid='{}' resolved=nullptr "
                 "(parent gone)  results={} blend_snap={}",
                 parent_iid, results.size(),
                 blend_snap ? "present" : "null");
        return;
    }
    LOG_INFO("[IIDDIAG] ReleaseBlend::exec  iid='{}' resolved name='{}' "
             "live_iid='{}'  results={} insert_index={}",
             parent_iid, parent->name, parent->internal_id,
             results.size(), insert_index);

    auto& ch = parent->children;
    if (blend_snap) {
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i] && ch[i]->id == blend_snap->id) {
                ch.erase(ch.begin()+i);
                break;
            }
    }
    int ins = std::clamp(insert_index, 0, (int)ch.size());
    for (int i = 0; i < (int)results.size(); ++i) {
        if (!results[i]) continue;
        ch.insert(ch.begin() + ins + i, clone_node(*results[i]));
    }
}

void ReleaseBlendCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] ReleaseBlend::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] ReleaseBlend::undo  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] ReleaseBlend::undo  iid='{}' resolved=nullptr "
                 "(parent gone)  results={} blend_snap={}",
                 parent_iid, results.size(),
                 blend_snap ? "present" : "null");
        return;
    }
    LOG_INFO("[IIDDIAG] ReleaseBlend::undo  iid='{}' resolved name='{}' "
             "live_iid='{}'  results={} insert_index={}",
             parent_iid, parent->name, parent->internal_id,
             results.size(), insert_index);

    auto& ch = parent->children;
    for (auto& r : results) {
        if (!r) continue;
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i] && ch[i]->id == r->id) {
                ch.erase(ch.begin()+i);
                break;
            }
    }
    if (blend_snap) {
        int ins = std::clamp(insert_index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*blend_snap));
    }
}

// ── MakeWarpCommand bodies (s173 m4) ─────────────────────────────────
// Resolve the parent iid; on execute remove the source and insert the
// Warp at insert_index; on undo remove the Warp and reinsert source at
// source_index. Same shape as MakeBlend at the resolution layer; same
// shape as the original inline bodies at the mutation layer.
void MakeWarpCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] MakeWarp::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] MakeWarp::exec  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] MakeWarp::exec  iid='{}' resolved=nullptr "
                 "(parent gone)", parent_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] MakeWarp::exec  iid='{}' resolved name='{}' "
             "live_iid='{}'  source_index={} insert_index={}",
             parent_iid, parent->name, parent->internal_id,
             source_index, insert_index);

    auto& ch = parent->children;
    if (source_snap) {
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i] && ch[i]->id == source_snap->id) {
                ch.erase(ch.begin()+i);
                break;
            }
    }
    if (!warp_snap) return;
    int ins = std::clamp(insert_index, 0, (int)ch.size());
    ch.insert(ch.begin() + ins, clone_node(*warp_snap));
}

void MakeWarpCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] MakeWarp::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] MakeWarp::undo  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] MakeWarp::undo  iid='{}' resolved=nullptr "
                 "(parent gone)", parent_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] MakeWarp::undo  iid='{}' resolved name='{}' "
             "live_iid='{}'  source_index={} insert_index={}",
             parent_iid, parent->name, parent->internal_id,
             source_index, insert_index);

    auto& ch = parent->children;
    if (warp_snap) {
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i] && ch[i]->id == warp_snap->id) {
                ch.erase(ch.begin()+i);
                break;
            }
    }
    if (!source_snap) return;
    int ins = std::clamp(source_index, 0, (int)ch.size());
    ch.insert(ch.begin() + ins, clone_node(*source_snap));
}

// ── ReleaseWarpCommand bodies (s173 m4) ──────────────────────────────
// Inverse of MakeWarp at the tree-mutation layer; same iid-resolution
// pattern. execute removes the Warp and inserts the source; undo
// removes the source and reinserts the Warp.
void ReleaseWarpCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] ReleaseWarp::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] ReleaseWarp::exec  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] ReleaseWarp::exec  iid='{}' resolved=nullptr "
                 "(parent gone)", parent_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] ReleaseWarp::exec  iid='{}' resolved name='{}' "
             "live_iid='{}'  insert_index={}",
             parent_iid, parent->name, parent->internal_id, insert_index);

    auto& ch = parent->children;
    if (warp_snap) {
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i] && ch[i]->id == warp_snap->id) {
                ch.erase(ch.begin()+i);
                break;
            }
    }
    if (!source_snap) return;
    int ins = std::clamp(insert_index, 0, (int)ch.size());
    ch.insert(ch.begin() + ins, clone_node(*source_snap));
}

void ReleaseWarpCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] ReleaseWarp::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] ReleaseWarp::undo  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] ReleaseWarp::undo  iid='{}' resolved=nullptr "
                 "(parent gone)", parent_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] ReleaseWarp::undo  iid='{}' resolved name='{}' "
             "live_iid='{}'  insert_index={}",
             parent_iid, parent->name, parent->internal_id, insert_index);

    auto& ch = parent->children;
    if (source_snap) {
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i] && ch[i]->id == source_snap->id) {
                ch.erase(ch.begin()+i);
                break;
            }
    }
    if (!warp_snap) return;
    int ins = std::clamp(insert_index, 0, (int)ch.size());
    ch.insert(ch.begin() + ins, clone_node(*warp_snap));
}

// ── FlattenWarpCommand bodies (s173 m4) ──────────────────────────────
// Same shape as ReleaseWarp at the tree level (1→1 swap), but the swap
// targets are warp_snap ↔ flattened_snap rather than warp_snap ↔
// source_snap. Same iid-resolution pattern.
void FlattenWarpCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] FlattenWarp::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] FlattenWarp::exec  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] FlattenWarp::exec  iid='{}' resolved=nullptr "
                 "(parent gone)", parent_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] FlattenWarp::exec  iid='{}' resolved name='{}' "
             "live_iid='{}'  insert_index={}",
             parent_iid, parent->name, parent->internal_id, insert_index);

    auto& ch = parent->children;
    if (warp_snap) {
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i] && ch[i]->id == warp_snap->id) {
                ch.erase(ch.begin()+i);
                break;
            }
    }
    if (!flattened_snap) return;
    int ins = std::clamp(insert_index, 0, (int)ch.size());
    ch.insert(ch.begin() + ins, clone_node(*flattened_snap));
}

void FlattenWarpCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] FlattenWarp::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (parent_iid.empty()) {
        LOG_INFO("[IIDDIAG] FlattenWarp::undo  empty parent_iid (push site bug?)");
        return;
    }
    auto* parent = curvz::utils::find_by_iid(*proj, parent_iid);
    if (!parent) {
        LOG_INFO("[IIDDIAG] FlattenWarp::undo  iid='{}' resolved=nullptr "
                 "(parent gone)", parent_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] FlattenWarp::undo  iid='{}' resolved name='{}' "
             "live_iid='{}'  insert_index={}",
             parent_iid, parent->name, parent->internal_id, insert_index);

    auto& ch = parent->children;
    if (flattened_snap) {
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i] && ch[i]->id == flattened_snap->id) {
                ch.erase(ch.begin()+i);
                break;
            }
    }
    if (!warp_snap) return;
    int ins = std::clamp(insert_index, 0, (int)ch.size());
    ch.insert(ch.begin() + ins, clone_node(*warp_snap));
}

// ── EditWarpCommand bodies (s174 m1) ─────────────────────────────────
// EditPath template — single-target field swap. Resolve the warp node
// by iid; if it's gone (closed doc, destructive op), no-op cleanly.
// Both directions (execute / undo) share the same shape: resolve, sanity-
// check is_warp(), swap the four fields, force cache rebuild.
//
// The is_warp() guard is defensive: the iid resolver returns whatever
// node has that internal_id, regardless of type. A misrouted command
// with a mismatched iid that happens to point at a non-Warp node
// shouldn't corrupt that node's warp_env_* fields. (Those fields exist
// on every SceneNode but are only meaningful on Warp.)
void EditWarpCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] EditWarp::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        LOG_INFO("[IIDDIAG] EditWarp::exec  empty obj_iid (push site bug?)");
        return;
    }
    auto* warp = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!warp) {
        LOG_INFO("[IIDDIAG] EditWarp::exec  iid='{}' resolved=nullptr",
                 obj_iid);
        return;
    }
    if (!warp->is_warp()) {
        LOG_INFO("[IIDDIAG] EditWarp::exec  iid='{}' resolved type={} "
                 "(not a Warp) — skipping", obj_iid, (int)warp->type);
        return;
    }
    LOG_INFO("[IIDDIAG] EditWarp::exec  iid='{}' resolved name='{}' "
             "live_iid='{}'  post_quality={} post_preset={}",
             obj_iid, warp->name, warp->internal_id,
             post_quality, post_preset_idx);
    warp->warp_env_top     = post_env_top;
    warp->warp_env_bottom  = post_env_bottom;
    warp->warp_quality     = post_quality;
    warp->warp_preset_idx  = post_preset_idx;
    warp->warp_cache_dirty = true;
}

void EditWarpCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] EditWarp::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        LOG_INFO("[IIDDIAG] EditWarp::undo  empty obj_iid (push site bug?)");
        return;
    }
    auto* warp = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!warp) {
        LOG_INFO("[IIDDIAG] EditWarp::undo  iid='{}' resolved=nullptr",
                 obj_iid);
        return;
    }
    if (!warp->is_warp()) {
        LOG_INFO("[IIDDIAG] EditWarp::undo  iid='{}' resolved type={} "
                 "(not a Warp) — skipping", obj_iid, (int)warp->type);
        return;
    }
    LOG_INFO("[IIDDIAG] EditWarp::undo  iid='{}' resolved name='{}' "
             "live_iid='{}'  pre_quality={} pre_preset={}",
             obj_iid, warp->name, warp->internal_id,
             pre_quality, pre_preset_idx);
    warp->warp_env_top     = pre_env_top;
    warp->warp_env_bottom  = pre_env_bottom;
    warp->warp_quality     = pre_quality;
    warp->warp_preset_idx  = pre_preset_idx;
    warp->warp_cache_dirty = true;
}

// ── StepRepeatCommand bodies (s175 m1) ───────────────────────────────
// Multi-target shape — same template as Duplicate (s170 m3) at the loop
// level: execute() inserts at recorded indices in descending order so
// earlier high-index inserts don't shift later low-index ones; undo()
// removes by snap's SVG id (order doesn't matter for erase-by-id since
// each lookup is independent).
//
// Per-Entry no-op-on-miss: if a parent_iid resolves to nullptr (layer
// destroyed between push and replay), that entry's contribution is
// skipped and the rest of the entries continue. This makes partial
// destruction fully reversible for the surviving entries — matches the
// established multi-target pattern from Cut/Duplicate/CornerTreatment/
// Align/ScaleObjects.
void StepRepeatCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] StepRepeat::exec  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] StepRepeat::exec  applying {} entries", entries.size());
    // Sort entry indices descending for safe insertion across multiple
    // entries within the same parent (matches pre-migration ordering).
    std::vector<int> order(entries.size());
    for (int i = 0; i < (int)entries.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
        [&](int a, int b){ return entries[a].index > entries[b].index; });
    for (int i : order) {
        auto& e = entries[i];
        if (e.parent_iid.empty()) {
            LOG_INFO("[IIDDIAG] StepRepeat::exec  entry has empty parent_iid "
                     "(push site bug?)");
            continue;
        }
        auto* parent = curvz::utils::find_by_iid(*proj, e.parent_iid);
        if (!parent) {
            LOG_INFO("[IIDDIAG] StepRepeat::exec  parent_iid='{}' resolved="
                     "nullptr (layer gone) — skipping entry", e.parent_iid);
            continue;
        }
        auto& ch = parent->children;
        int ins = std::clamp(e.index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*e.snap));
    }
}

void StepRepeatCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] StepRepeat::undo  proj=NULL");
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    LOG_INFO("[IIDDIAG] StepRepeat::undo  reverting {} entries", entries.size());
    for (auto& e : entries) {
        if (e.parent_iid.empty()) {
            LOG_INFO("[IIDDIAG] StepRepeat::undo  entry has empty parent_iid "
                     "(push site bug?)");
            continue;
        }
        auto* parent = curvz::utils::find_by_iid(*proj, e.parent_iid);
        if (!parent) {
            LOG_INFO("[IIDDIAG] StepRepeat::undo  parent_iid='{}' resolved="
                     "nullptr (layer gone) — skipping entry", e.parent_iid);
            continue;
        }
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i) {
            if (ch[i]->id == e.snap->id) {
                ch.erase(ch.begin()+i);
                break;
            }
        }
    }
}

// ── LinkTextToPathCommand bodies (s175 m2) ───────────────────────────
// Single-target field swap — TextEdit template. Resolves obj_iid then
// writes the five paired fields (text_path_id, text_path_offset,
// text_path_flip, text_x, text_y) for the requested direction.
//
// text_path_id is *already* an iid — see header docstring for the s174
// finding. It's stored and resolved verbatim; on the other side, the
// path lookup via Canvas::top_find_path_by_id treats it as an iid match
// against SceneNode::internal_id.
void LinkTextToPathCommand::execute() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] LinkTextToPath::exec  proj=NULL  iid='{}'", obj_iid);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        LOG_INFO("[IIDDIAG] LinkTextToPath::exec  empty obj_iid "
                 "(push site bug?)");
        return;
    }
    auto* obj = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!obj) {
        LOG_INFO("[IIDDIAG] LinkTextToPath::exec  iid='{}' resolved=nullptr",
                 obj_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] LinkTextToPath::exec  iid='{}' resolved name='{}' "
             "live_iid='{}'  before_pid='{}' after_pid='{}'",
             obj_iid, obj->name, obj->internal_id,
             before_path_id, after_path_id);
    obj->text_path_id     = after_path_id;
    obj->text_path_offset = after_offset;
    obj->text_path_flip   = after_flip;
    obj->text_x           = after_x;
    obj->text_y           = after_y;
}

void LinkTextToPathCommand::undo() {
    if (!proj) {
        LOG_INFO("[IIDDIAG] LinkTextToPath::undo  proj=NULL  iid='{}'", obj_iid);
        return;
    }
    invalidate_iid_indexes(proj);  // s168 m6 — fresh walk before resolve
    if (obj_iid.empty()) {
        LOG_INFO("[IIDDIAG] LinkTextToPath::undo  empty obj_iid "
                 "(push site bug?)");
        return;
    }
    auto* obj = curvz::utils::find_by_iid(*proj, obj_iid);
    if (!obj) {
        LOG_INFO("[IIDDIAG] LinkTextToPath::undo  iid='{}' resolved=nullptr",
                 obj_iid);
        return;
    }
    LOG_INFO("[IIDDIAG] LinkTextToPath::undo  iid='{}' resolved name='{}' "
             "live_iid='{}'  before_pid='{}' after_pid='{}'",
             obj_iid, obj->name, obj->internal_id,
             before_path_id, after_path_id);
    obj->text_path_id     = before_path_id;
    obj->text_path_offset = before_offset;
    obj->text_path_flip   = before_flip;
    obj->text_x           = before_x;
    obj->text_y           = before_y;
}

// s145 m1 — undo depth is now a user pref (AppPreferences::undo_history_depth),
// read on every push. The MAX_HISTORY constant in the header is retained as a
// compile-time hard ceiling and as the default if the pref ever fails to load,
// but the live trim consults the pref. Reducing the pref trims the live stack
// at the next push (does not retroactively prune); increasing the pref simply
// allows the stack to grow further from that point on.
void CommandHistory::push(std::unique_ptr<CurvzCommand> cmd) {
    m_redo_stack.clear();
    m_undo_stack.push_back(std::move(cmd));
    const int cap = AppPreferences::instance().undo_history_depth();
    while ((int)m_undo_stack.size() > cap)
        m_undo_stack.pop_front();
    LOG_DEBUG("History push — undo depth={} cap={}",
              m_undo_stack.size(), cap);
}

void CommandHistory::undo() {
    if (m_undo_stack.empty()) return;
    auto& cmd = m_undo_stack.back();
    LOG_INFO("Undo: {}", cmd->description());
    cmd->undo();
    m_redo_stack.push_back(std::move(cmd));
    m_undo_stack.pop_back();
}

void CommandHistory::redo() {
    if (m_redo_stack.empty()) return;
    auto& cmd = m_redo_stack.back();
    LOG_INFO("Redo: {}", cmd->description());
    cmd->execute();
    m_undo_stack.push_back(std::move(cmd));
    m_redo_stack.pop_back();
}

std::string CommandHistory::undo_description() const {
    return m_undo_stack.empty() ? "" : m_undo_stack.back()->description();
}

std::string CommandHistory::redo_description() const {
    return m_redo_stack.empty() ? "" : m_redo_stack.back()->description();
}

// s165 m3 — drop commands from both stacks whose captured raw SceneNode
// pointers match `target`. Called from Canvas::scrub_node_refs when a
// SceneNode is destroyed by a destructive op, so undo/redo can never
// reach a command that would dereference a dangling pointer.
//
// Returns the total number of commands dropped (across both stacks).
// Logged so we can see at a glance whether the scrub is doing real work.
int CommandHistory::scrub_command_history(const SceneNode* target) {
    if (!target) return 0;
    int dropped = 0;
    auto walk = [&](std::deque<std::unique_ptr<CurvzCommand>>& stack) {
        for (auto it = stack.begin(); it != stack.end(); ) {
            if ((*it)->references(target)) {
                LOG_DEBUG("scrub_command_history: dropping '{}'",
                          (*it)->description());
                it = stack.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
    };
    walk(m_undo_stack);
    walk(m_redo_stack);
    if (dropped > 0) {
        LOG_DEBUG("scrub_command_history: dropped {} commands referencing target={}",
                  dropped, (void*)target);
    }
    return dropped;
}

} // namespace Curvz
