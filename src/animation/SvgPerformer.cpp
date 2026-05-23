// animation/SvgPerformer.cpp ─────────────────────────────────────────────────
//
// s291 m2 — see animation/SvgPerformer.hpp for the arc header. This file
// implements the tree walker, performance-plan builder, plan executor,
// and per-tick beat machinery.

#include "animation/SvgPerformer.hpp"

#include "Canvas.hpp"
#include "CommandHistory.hpp"
#include "CurvzDocument.hpp"
#include "CurvzProject.hpp"
#include "CurvzLog.hpp"
#include "SvgParser.hpp"      // s291 m2 — base parser. Same entry every
                              // File→Open / Import / Library / Template
                              // call site uses. Returns a finished tree.
#include "curvz_utils.hpp"

#include <glibmm/main.h>
#include <algorithm>   // std::reverse, std::min
#include <cmath>
#include <functional>

namespace Curvz {
namespace animation {

namespace {

// Beat durations at speed=1.0 (nominal). Multiplied by the per-performance
// speed parameter.
constexpr double kAnchorPlaceMs   =  80.0;
constexpr double kHandleOutMs     = 120.0;
constexpr double kSegmentGrowMs   = 250.0;
constexpr double kInterludeMs     =  40.0;
constexpr double kSubpathBreakMs  = 120.0;

constexpr int kTickIntervalMs = 16;  // ~60fps target

// Reveal sequence durations (wall-clock, not scaled by speed — these are
// perceptual landmarks, not part of the construction tempo).
constexpr int kRevealConstructionHoldMs = 500;
constexpr int kRevealPaintedHoldMs      = 1500;

// Evaluate a cubic Bezier from p1 to p2 with control points c1, c2 at t.
inline void cubic_eval(double p1x, double p1y,
                       double c1x, double c1y,
                       double c2x, double c2y,
                       double p2x, double p2y,
                       double t,
                       double& out_x, double& out_y) {
    const double u  = 1.0 - t;
    const double u2 = u * u;
    const double u3 = u2 * u;
    const double t2 = t * t;
    const double t3 = t2 * t;
    out_x = u3*p1x + 3.0*u2*t*c1x + 3.0*u*t2*c2x + t3*p2x;
    out_y = u3*p1y + 3.0*u2*t*c1y + 3.0*u*t2*c2y + t3*p2y;
}

} // anon namespace

// ────────────────────────────────────────────────────────────────────────────
// Constructor
// ────────────────────────────────────────────────────────────────────────────

SvgPerformer::SvgPerformer(Canvas* canvas)
    : m_canvas(canvas) {}

// ────────────────────────────────────────────────────────────────────────────
// clone_node_shell — deep copy without children + composite slots
// ────────────────────────────────────────────────────────────────────────────
//
// The performance plan rebuilds container structure through OpenContainer
// / CloseContainer / PerformPath / CommitLeaf bracketing. Deep-copying
// children + composites during prototype creation would double-insert.
// We start from clone_node (so future SceneNode field additions are
// inherited automatically), then reset what we don't want.
std::unique_ptr<SceneNode> SvgPerformer::clone_node_shell(const SceneNode& src) {
    auto shell = clone_node(src);
    shell->children.clear();
    shell->clip_shape.reset();
    shell->blend_source_a.reset();
    shell->blend_source_b.reset();
    shell->blend_cache.clear();
    shell->warp_source.reset();
    shell->warp_glyph_cache.reset();
    shell->warp_cache.reset();
    // Mint a fresh internal_id — the source's iid is the parser's; the
    // committed clone is a separate node with its own identity. Same
    // policy Canvas paste uses for duplicates.
    shell->internal_id = generate_internal_id();
    return shell;
}

// ────────────────────────────────────────────────────────────────────────────
// enact_pen_path — single d-string verb (smoke 67)
// ────────────────────────────────────────────────────────────────────────────
//
// Unchanged from s288 m2 semantically. Parses one d-string, builds one
// beat list, runs the timeout, commits one Path. No tree walking — there
// is no SVG to walk.
void SvgPerformer::enact_pen_path(const std::string& d_string,
                                  double speed,
                                  CurvzDocument* doc,
                                  SceneNode* layer) {
    if (m_playing) return;
    if (d_string.empty()) return;
    if (!m_canvas) return;
    if (!doc) return;
    if (!layer) return;
    if (speed <= 0.0) speed = 1.0;

    PathData parsed = curvz::utils::svg_d_to_path_data(d_string);
    if (parsed.nodes.empty()) return;

    // user-space → doc-space Y-flip
    curvz::utils::path_data_user_to_doc(parsed, (double)doc->canvas_height());

    m_target          = std::move(parsed);
    m_target_doc      = doc;
    m_target_layer    = layer;
    m_current_fill    = FillStyle{};   // CurrentColor default
    m_current_stroke  = StrokeStyle{}; // width=0 default
    m_current_speed   = speed;

    // Plan: one step (PerformPath) with a minimal prototype. commit_current_path
    // will pull fill/stroke/path off the prototype and m_target.
    m_plan.clear();
    m_container_stack.clear();
    {
        Step step;
        step.kind = StepKind::PerformPath;
        step.node_prototype = std::make_unique<SceneNode>();
        step.node_prototype->type = SceneNode::Type::Path;
        step.node_prototype->id = "welcome_path_" + std::to_string(
            (long long)std::chrono::steady_clock::now()
                .time_since_epoch().count());
        step.node_prototype->internal_id = generate_internal_id();
        step.node_prototype->fill = m_current_fill;
        step.node_prototype->stroke = m_current_stroke;
        step.node_prototype->visible = true;
        step.speed = speed;
        m_plan.push_back(std::move(step));
    }
    m_plan_idx = 0;

    build_beat_list(m_target, speed);
    if (m_beats.empty()) {
        m_plan.clear();
        return;
    }

    m_beat_idx              = 0;
    m_anchors_finished      = 0;
    m_current_anchor_t      = 0.0;
    m_current_handle_t      = 0.0;
    m_current_segment_t     = 0.0;
    m_current_segment_dest  = -1;
    m_inflight_phantoms.clear();
    m_held_phantoms.clear();
    m_t_beat_start          = std::chrono::steady_clock::now();
    m_playing               = true;

    Glib::signal_timeout().connect(
        [this]() -> bool { return tick(); },
        kTickIntervalMs);
}

// ────────────────────────────────────────────────────────────────────────────
// perform — SVG-file entry point (smoke 68)
// ────────────────────────────────────────────────────────────────────────────
void SvgPerformer::perform(const std::string& svg_path,
                           double speed,
                           CurvzDocument* doc,
                           SceneNode* layer) {
    if (m_playing) return;
    if (svg_path.empty()) return;
    if (!m_canvas) return;
    if (!doc) return;
    if (!layer) return;
    if (speed <= 0.0) speed = 1.0;

    LOG_INFO("SvgPerformer: perform begin path='{}' speed={:.3f}",
             svg_path, speed);

    // Parse via the base parser — same entry every File→Open and Import
    // call site uses. The returned doc has a fully-built tree with
    // transforms folded, primitives converted to paths, fills inherited,
    // children reversed for paint-order, etc.
    auto src_doc = Curvz::parse_svg_file(svg_path);
    if (!src_doc) {
        LOG_INFO("SvgPerformer: perform — parse_svg_file returned null, no-op");
        return;
    }

    LOG_INFO("SvgPerformer: perform — parsed: canvas {}x{}, {} layers",
             src_doc->canvas_width(), src_doc->canvas_height(),
             src_doc->layers.size());

    // Build the performance plan from the parsed tree.
    m_plan.clear();
    m_container_stack.clear();
    m_held_phantoms.clear();
    m_inflight_phantoms.clear();
    m_current_speed = speed;  // captured here so build_plan_for_node can
                              // stamp it on each PerformPath step.

    // Walk each Layer's children. The SVG's Layer SceneNodes don't commit
    // as Layers in the user's doc — the user's target layer receives the
    // contents. (If we want fresh Layer SceneNodes per SVG layer in a
    // future milestone, that's a doc-setup decision that belongs in m3+.)
    for (const auto& l : src_doc->layers) {
        if (!l) continue;
        // Skip technical layer kinds — RefLayer, MeasureLayer, GuideLayer,
        // GridLayer, MarginLayer. They're infrastructure; the renderer
        // treats them specially. The welcome animation animates Type::Layer
        // content only.
        if (l->type != SceneNode::Type::Layer) continue;
        build_plan_for_layer_children(*l);
    }

    LOG_INFO("SvgPerformer: perform — plan built: {} steps", m_plan.size());

    if (m_plan.empty()) {
        return;  // SVG had no content
    }

    m_target_doc    = doc;
    m_target_layer  = layer;
    m_plan_idx      = 0;
    m_playing       = true;

    // Kick off the first step.
    advance_plan();
}

// ────────────────────────────────────────────────────────────────────────────
// build_plan_for_layer_children — walk a parsed Layer's contents
// ────────────────────────────────────────────────────────────────────────────
//
// The Layer itself doesn't commit as a SceneNode — the user's target
// layer receives its contents. We just walk the children in reverse
// (matching Canvas::draw_object's bottom-up iteration so the visual
// performance order matches paint order: bottom-of-stack performs first,
// top-of-stack last).
void SvgPerformer::build_plan_for_layer_children(const SceneNode& layer) {
    for (int i = (int)layer.children.size() - 1; i >= 0; --i) {
        const auto& child = layer.children[i];
        if (!child) continue;
        build_plan_for_node(*child, 0);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// build_plan_for_node — recursive tree walker
// ────────────────────────────────────────────────────────────────────────────
//
// Type-dispatch matches Canvas::draw_object / DocumentGallery::render_thumb:
//
//   - Path: emit a PerformPath step. The committed node inherits fill,
//           stroke, transform, opacity, etc. from the prototype.
//   - Compound / Group / ClipGroup / Blend / Warp: emit OpenContainer,
//           recurse into children + composite slots in reverse, emit
//           CloseContainer. At commit time the container SceneNode is
//           minted with all its accumulated children inside.
//   - Image / Text / Ref / other leaves: emit CommitLeaf. No beats.
//
// Walk order matches the renderers: children iterated in reverse
// (`children.size()-1` down to `0`), composite slots after children.
// At commit time we insert with `children.begin()` so the just-committed
// node lands at index 0 (top in Curvz's convention), and the resulting
// final order matches the source tree.
void SvgPerformer::build_plan_for_node(const SceneNode& src, int depth) {
    std::string indent(std::min(depth, 12) * 2, ' ');

    switch (src.type) {
        case SceneNode::Type::Path: {
            // Path leaf — the spine of the performance.
            if (!src.path || src.path->nodes.empty()) {
                LOG_INFO("SvgPerformer: {}skip empty Path '{}'",
                         indent, src.name);
                return;
            }
            LOG_INFO("SvgPerformer: {}plan PerformPath name='{}' nodes={}",
                     indent, src.name, src.path->nodes.size());
            Step step;
            step.kind = StepKind::PerformPath;
            step.node_prototype = clone_node_shell(src);
            // clone_node_shell already copied the PathData via clone_node;
            // it's on shell->path. We'll use shell->path at commit time.
            step.speed = m_current_speed;
            m_plan.push_back(std::move(step));
            return;
        }

        case SceneNode::Type::Compound:
        case SceneNode::Type::Group:
        case SceneNode::Type::ClipGroup:
        case SceneNode::Type::Blend:
        case SceneNode::Type::Warp: {
            LOG_INFO("SvgPerformer: {}plan OpenContainer type={} name='{}'",
                     indent, (int)src.type, src.name);
            // OpenContainer
            {
                Step step;
                step.kind = StepKind::OpenContainer;
                step.node_prototype = clone_node_shell(src);
                m_plan.push_back(std::move(step));
            }

            // Recurse children in reverse (bottom-up)
            for (int i = (int)src.children.size() - 1; i >= 0; --i) {
                const auto& child = src.children[i];
                if (child) build_plan_for_node(*child, depth + 1);
            }

            // Recurse composite slots — these are container-specific
            // children that live in dedicated unique_ptr members rather
            // than in `children`. They'll be hooked back into the
            // committed container via the prototype's slot fields at
            // commit time. For m2 we just walk them as additional
            // children: their geometry performs, but they commit into
            // the container's `children` vector via the general routing
            // — NOT back into the container's clip_shape / blend_source_*
            // / warp_source slots.
            //
            // That's a limitation worth banking: ClipGroup / Blend / Warp
            // committed by the performer will have their composite slots
            // empty (null), with the slot's content landing in children
            // instead. The renderer won't apply the clip / blend / warp
            // effect correctly under those conditions. m2 scope is
            // Compound + Group correctness (scott-bug); ClipGroup / Blend
            // / Warp slot routing is its own milestone.
            //
            // For now, walk them so the geometry at least appears — it'll
            // appear as siblings inside the container rather than as the
            // effect's source.
            if (src.clip_shape) build_plan_for_node(*src.clip_shape, depth + 1);
            if (src.blend_source_a) build_plan_for_node(*src.blend_source_a, depth + 1);
            if (src.blend_source_b) build_plan_for_node(*src.blend_source_b, depth + 1);
            if (src.warp_source) build_plan_for_node(*src.warp_source, depth + 1);

            // CloseContainer
            {
                Step step;
                step.kind = StepKind::CloseContainer;
                m_plan.push_back(std::move(step));
            }
            return;
        }

        default: {
            // Non-Path leaf — Image, Text, Ref, Guide, Measure, etc.
            // Commit verbatim, no beats. The audience sees it appear
            // when this step executes (which is synchronous).
            LOG_INFO("SvgPerformer: {}plan CommitLeaf type={} name='{}'",
                     indent, (int)src.type, src.name);
            Step step;
            step.kind = StepKind::CommitLeaf;
            step.node_prototype = clone_node_shell(src);
            m_plan.push_back(std::move(step));
            return;
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// advance_plan — drive the plan forward
// ────────────────────────────────────────────────────────────────────────────
//
// Returns false when the plan is exhausted (and the reveal sequence
// schedules). True means a step is now executing — either synchronously
// (Open / Close / CommitLeaf, which loop into the next step) or
// asynchronously (PerformPath, which kicks off beats and yields control
// to the tick loop).
bool SvgPerformer::advance_plan() {
    while (m_plan_idx < m_plan.size()) {
        Step& step = m_plan[m_plan_idx];

        switch (step.kind) {
            case StepKind::OpenContainer: {
                LOG_INFO("SvgPerformer: exec OpenContainer step={} type={} "
                         "depth={}",
                         m_plan_idx,
                         (int)step.node_prototype->type,
                         m_container_stack.size() + 1);
                PendingContainer pc;
                pc.prototype = std::move(step.node_prototype);
                m_container_stack.push_back(std::move(pc));
                ++m_plan_idx;
                continue;  // synchronous, advance
            }

            case StepKind::CloseContainer: {
                LOG_INFO("SvgPerformer: exec CloseContainer step={} "
                         "depth_before={}",
                         m_plan_idx, m_container_stack.size());
                commit_pending_container();
                ++m_plan_idx;
                continue;  // synchronous, advance
            }

            case StepKind::CommitLeaf: {
                LOG_INFO("SvgPerformer: exec CommitLeaf step={} type={}",
                         m_plan_idx, (int)step.node_prototype->type);
                route_committed_node(std::move(step.node_prototype));
                if (m_canvas) m_canvas->queue_draw();
                ++m_plan_idx;
                continue;  // synchronous, advance
            }

            case StepKind::PerformPath: {
                // Asynchronous: set up the beat machinery, return.
                // tick() drives it; commit_current_path() finishes the step.
                LOG_INFO("SvgPerformer: exec PerformPath step={} name='{}' "
                         "nodes={}",
                         m_plan_idx,
                         step.node_prototype->name,
                         step.node_prototype->path
                             ? step.node_prototype->path->nodes.size() : 0);

                if (!step.node_prototype->path ||
                    step.node_prototype->path->nodes.empty()) {
                    // Defensive — shouldn't happen given build_plan_for_node
                    // gates on empty paths. Skip silently.
                    ++m_plan_idx;
                    continue;
                }

                m_target          = *step.node_prototype->path;
                m_current_fill    = step.node_prototype->fill;
                m_current_stroke  = step.node_prototype->stroke;

                build_beat_list(m_target, step.speed);
                if (m_beats.empty()) {
                    // Degenerate path — commit immediately and continue.
                    commit_current_path();
                    ++m_plan_idx;
                    continue;
                }

                m_beat_idx              = 0;
                m_anchors_finished      = 0;
                m_current_anchor_t      = 0.0;
                m_current_handle_t      = 0.0;
                m_current_segment_t     = 0.0;
                m_current_segment_dest  = -1;
                m_inflight_phantoms.clear();
                m_t_beat_start          = std::chrono::steady_clock::now();

                Glib::signal_timeout().connect(
                    [this]() -> bool { return tick(); },
                    kTickIntervalMs);
                return true;  // asynchronous; tick() will drive
            }
        }
    }

    // Plan exhausted. Schedule the reveal sequence:
    //   construction-hold → toggle outline→preview → painted-hold → end
    LOG_INFO("SvgPerformer: plan exhausted, scheduling reveal");
    Glib::signal_timeout().connect_once(
        [this]() {
            if (m_canvas && m_canvas->is_outline_mode()) {
                m_canvas->toggle_outline_mode();
            }
            Glib::signal_timeout().connect_once(
                [this]() {
                    m_target_doc   = nullptr;
                    m_target_layer = nullptr;
                    m_playing      = false;
                    m_held_phantoms.clear();
                    m_inflight_phantoms.clear();
                    if (m_canvas) m_canvas->queue_draw();
                    LOG_INFO("SvgPerformer: reveal complete");
                },
                kRevealPaintedHoldMs);
        },
        kRevealConstructionHoldMs);

    return false;
}

// ────────────────────────────────────────────────────────────────────────────
// route_committed_node — insert a freshly-minted SceneNode at the right place
// ────────────────────────────────────────────────────────────────────────────
//
// If a container is open, the node goes into its pending_children.
// If no container is open, the node goes into the user's target layer
// at index 0 (top, Curvz's convention).
void SvgPerformer::route_committed_node(std::unique_ptr<SceneNode> node) {
    if (!node) return;
    if (!m_container_stack.empty()) {
        m_container_stack.back().pending_children.push_back(std::move(node));
    } else if (m_target_layer) {
        m_target_layer->children.insert(m_target_layer->children.begin(),
                                        std::move(node));
        if (m_target_doc) m_target_doc->invalidate_iid_index();
    }
}

// ────────────────────────────────────────────────────────────────────────────
// commit_current_path — finish a PerformPath step
// ────────────────────────────────────────────────────────────────────────────
//
// Called from tick() when the beat list is exhausted. Mints a Path
// SceneNode from m_plan[m_plan_idx]'s prototype, sets its final geometry
// from m_target, routes it.
//
// Phantom handling: if no container is open, the path is top-level —
// commit lands in the doc and m_inflight_phantoms clear immediately (the
// renderer takes over). If a container IS open, the path's outline is
// instead appended to m_held_phantoms (so it stays visible while the
// container's other children continue performing); the inflight phantoms
// for this path clear (since the next path's beats need a fresh canvas).
void SvgPerformer::commit_current_path() {
    if (m_plan_idx >= m_plan.size()) return;
    Step& step = m_plan[m_plan_idx];
    if (step.kind != StepKind::PerformPath) return;

    auto obj = std::move(step.node_prototype);
    if (!obj) return;
    obj->path = std::make_unique<PathData>(m_target);
    obj->visible = true;

    const bool inside_container = !m_container_stack.empty();
    LOG_INFO("SvgPerformer: commit_current_path step={} inside_container={} "
             "name='{}' nodes={}",
             m_plan_idx, inside_container ? 1 : 0,
             obj->name,
             obj->path ? obj->path->nodes.size() : 0);

    route_committed_node(std::move(obj));

    if (inside_container) {
        // Hold the finished outline while the container is still assembling.
        append_target_to_held();
    }
    // Clear the in-flight phantoms regardless — the next path (or
    // synchronous step) starts with a clean inflight slate.
    m_inflight_phantoms.clear();

    if (m_canvas) m_canvas->queue_draw();
}

// ────────────────────────────────────────────────────────────────────────────
// commit_pending_container — finish a CloseContainer step
// ────────────────────────────────────────────────────────────────────────────
//
// Pops m_container_stack's top entry. The prototype carries the
// container's type and all container-specific bits the parser stamped
// on it (Compound fill/stroke, Group transform/opacity, ClipGroup clip_id,
// Blend blend_steps, Warp envelope, etc.). The pending children become
// the container's children. Then route to the next-outer commit target.
//
// At this moment, the held phantoms for this container's contents are no
// longer needed — the renderer will paint the committed container
// correctly (fill, fill-rule, blend mode, clip, etc.). Clear m_held_phantoms.
//
// Note: when containers nest (e.g. a Group containing a Compound),
// closing the inner container leaves m_container_stack non-empty (the
// outer is still open). The outer's children continue accumulating, and
// the inner's held outlines... hmm. If we clear m_held_phantoms at every
// container close, then inner-close clears outlines for paths that are
// now inside the inner Compound — but those paths' commit into the inner
// Compound means the renderer paints them correctly THROUGH the inner
// Compound, which is itself an unrendered prototype until the outer
// closes. So we shouldn't clear yet — the outer hasn't committed.
//
// Resolution: m_held_phantoms clears only when the OUTERMOST container
// closes (m_container_stack becomes empty after the pop). Inner closes
// leave the outlines held; they clear when the top-of-stack outer
// finally closes and its container SceneNode commits into the doc.
//
// Edge case: at the moment the inner Compound commits, the renderer can
// see the Compound SceneNode inside the outer Group's pending_children
// — but that pending_children is on the performer's stack, not in the
// doc yet. So the renderer doesn't see it. The outlines stay held until
// the outer Group commits into the doc, at which point the whole subtree
// becomes visible at once with all containers correctly assembled.
void SvgPerformer::commit_pending_container() {
    if (m_container_stack.empty()) {
        LOG_INFO("SvgPerformer: commit_pending_container — stack empty (bug?)");
        return;
    }

    PendingContainer pc = std::move(m_container_stack.back());
    m_container_stack.pop_back();

    if (!pc.prototype) return;
    LOG_INFO("SvgPerformer: commit_pending_container type={} name='{}' "
             "children={} remaining_depth={}",
             (int)pc.prototype->type,
             pc.prototype->name,
             pc.pending_children.size(),
             m_container_stack.size());

    // Move pending children into the prototype's children vector. They
    // were accumulated in performance order (which mirrors the parser's
    // children order due to our reverse-walk + insert-at-begin scheme).
    //
    // route_committed_node uses insert(begin()) for top-level, but
    // pending_children was accumulated via push_back. So pending_children
    // is currently in performance order (= reverse of parser's children
    // order = ordered last-painted to first-painted, [0] = top).
    //
    // ...wait. Let me think again. The parser stores children with [0]=top
    // (Curvz convention). build_plan_for_node iterates parser's
    // children[size-1] → children[0], so performance order is
    // bottom-first. Each performed child route_committed_node's into
    // the open container's pending_children via push_back, so
    // pending_children[0] = first-performed = bottom = last-in-paint-order.
    // For Curvz's [0]=top convention, the container's children should end
    // up with [0]=top=last-performed.
    //
    // To convert pending_children (bottom-first) into Curvz convention
    // ([0]=top), reverse the vector.
    std::reverse(pc.pending_children.begin(), pc.pending_children.end());

    // Hook composite slots back up. For now we leave them empty — the
    // build_plan_for_node logic walks composite slots as plain children
    // (banked limitation; ClipGroup/Blend/Warp won't render their
    // effect, but the geometry appears). A future enhancement would
    // distinguish slot-children from regular-children at plan-build
    // time and route them back into the correct slot here.

    pc.prototype->children = std::move(pc.pending_children);

    auto committed = std::move(pc.prototype);
    route_committed_node(std::move(committed));

    // Clear held phantoms only when we've just committed the outermost
    // container — at that point the just-committed subtree is in the doc
    // and the renderer paints it. While inner containers are still
    // pending up the stack, hold the outlines so the audience continues
    // to see the assembling shape.
    if (m_container_stack.empty()) {
        m_held_phantoms.clear();
    }

    if (m_canvas) m_canvas->queue_draw();

    // Optional inter-step breath. For now, no extra breath at container
    // close — it just feels like part of the same beat.
}

// ────────────────────────────────────────────────────────────────────────────
// append_target_to_held — freeze m_target as a static outline
// ────────────────────────────────────────────────────────────────────────────
//
// Called when a Path inside an open container finishes its beats. The
// path's full geometry becomes a static outline on the held layer until
// the enclosing outermost container commits.
//
// We emit one PartialSegment with t=1.0 for each segment in m_target,
// plus an Anchor for each node. No handle lines — those are construction
// scaffolding; once the path is "done growing" the audience reads it as
// a finished outline, no levers.
void SvgPerformer::append_target_to_held() {
    const int n = (int)m_target.nodes.size();
    if (n == 0) return;

    // Anchors
    for (int i = 0; i < n; ++i) {
        const BezierNode& bn = m_target.nodes[i];
        PhantomShape sq;
        sq.kind = PhantomShape::Kind::Anchor;
        sq.a_x = bn.x;
        sq.a_y = bn.y;
        sq.t   = 1.0;
        m_held_phantoms.push_back(sq);
    }

    // Segments
    for (int i = 1; i < n; ++i) {
        const BezierNode& prev = m_target.nodes[i - 1];
        const BezierNode& curr = m_target.nodes[i];
        PhantomShape s;
        s.kind = PhantomShape::Kind::PartialSegment;
        s.a_x = prev.x;   s.a_y = prev.y;
        s.b_x = curr.x;   s.b_y = curr.y;
        s.c1_x = prev.cx2; s.c1_y = prev.cy2;
        s.c2_x = curr.cx1; s.c2_y = curr.cy1;
        s.t   = 1.0;
        m_held_phantoms.push_back(s);
    }

    // Closing segment if applicable
    if (m_target.closed && n >= 2) {
        const BezierNode& last  = m_target.nodes[n - 1];
        const BezierNode& first = m_target.nodes[0];
        PhantomShape s;
        s.kind = PhantomShape::Kind::PartialSegment;
        s.a_x = last.x;   s.a_y = last.y;
        s.b_x = first.x;  s.b_y = first.y;
        s.c1_x = last.cx2;  s.c1_y = last.cy2;
        s.c2_x = first.cx1; s.c2_y = first.cy1;
        s.t   = 1.0;
        m_held_phantoms.push_back(s);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// build_beat_list — same shape as s288 m2
// ────────────────────────────────────────────────────────────────────────────
void SvgPerformer::build_beat_list(const PathData& pd, double speed) {
    m_beats.clear();
    if (pd.nodes.empty()) return;

    const int n = (int)pd.nodes.size();

    // Anchor 0: just appears + extends out-handle. No incoming segment.
    m_beats.push_back({BeatKind::AnchorPlace, 0, kAnchorPlaceMs * speed});
    m_beats.push_back({BeatKind::HandleOut,   0, kHandleOutMs   * speed});
    m_beats.push_back({BeatKind::Interlude,  -1, kInterludeMs   * speed});

    // Anchors 1..n-1: incoming segment, then anchor, then handle, then pause.
    for (int i = 1; i < n; ++i) {
        m_beats.push_back({BeatKind::SegmentGrow, i, kSegmentGrowMs * speed});
        m_beats.push_back({BeatKind::AnchorPlace, i, kAnchorPlaceMs * speed});
        m_beats.push_back({BeatKind::HandleOut,   i, kHandleOutMs   * speed});
        m_beats.push_back({BeatKind::Interlude,  -1, kInterludeMs   * speed});
    }

    // Closing segment for closed paths.
    if (pd.closed && n >= 2) {
        m_beats.push_back({BeatKind::SegmentGrow, 0, kSegmentGrowMs * speed});
    }
}

// ────────────────────────────────────────────────────────────────────────────
// tick — per-frame value pump
// ────────────────────────────────────────────────────────────────────────────
bool SvgPerformer::tick() {
    if (!m_playing) return false;

    if (m_beat_idx >= m_beats.size()) {
        // Beat list exhausted. Commit the current PerformPath step, then
        // schedule the next step after an inter-step breath.
        commit_current_path();
        ++m_plan_idx;

        // Schedule the next advance after the inter-step breath. The
        // breath gives the audience a small beat of "just-finished
        // outline" before the next path's beats begin (or, if the next
        // step is a CloseContainer or CommitLeaf, before the synchronous
        // commit fires).
        const double breath_ms = m_inter_step_breath_ms_base * m_current_speed;
        Glib::signal_timeout().connect_once(
            [this]() { advance_plan(); },
            (int)breath_ms);
        return false;
    }

    const Beat& beat = m_beats[m_beat_idx];
    const auto now = std::chrono::steady_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(now - m_t_beat_start)
            .count();

    double t = (beat.duration_ms > 0.0)
                 ? (elapsed_ms / beat.duration_ms)
                 : 1.0;
    bool beat_done = (t >= 1.0);
    if (beat_done) t = 1.0;

    switch (beat.kind) {
        case BeatKind::AnchorPlace:
            m_current_anchor_t = t;
            m_current_handle_t = 0.0;
            break;
        case BeatKind::HandleOut:
            m_current_handle_t = t;
            break;
        case BeatKind::SegmentGrow:
            m_current_segment_t    = t;
            m_current_segment_dest = beat.node_idx;
            break;
        case BeatKind::Interlude:
        case BeatKind::SubpathBreak:
            break;
    }

    rebuild_phantoms();
    if (m_canvas) m_canvas->queue_draw();

    if (beat_done) {
        switch (beat.kind) {
            case BeatKind::HandleOut:
                if (beat.node_idx >= m_anchors_finished) {
                    m_anchors_finished = beat.node_idx + 1;
                }
                m_current_handle_t = 1.0;
                break;
            case BeatKind::SegmentGrow:
                m_current_segment_t = 1.0;
                break;
            default:
                break;
        }
        ++m_beat_idx;
        m_t_beat_start = std::chrono::steady_clock::now();
    }

    return true;
}

// ────────────────────────────────────────────────────────────────────────────
// rebuild_phantoms — refresh m_inflight_phantoms from current beat state
// ────────────────────────────────────────────────────────────────────────────
//
// Same vocabulary as s288 m2. Writes to m_inflight_phantoms; m_held_phantoms
// is untouched.
void SvgPerformer::rebuild_phantoms() {
    m_inflight_phantoms.clear();
    if (m_target.nodes.empty()) return;

    const int n = (int)m_target.nodes.size();

    // Fully-placed anchors with handles
    for (int i = 0; i < m_anchors_finished && i < n; ++i) {
        const BezierNode& bn = m_target.nodes[i];
        PhantomShape sq;
        sq.kind = PhantomShape::Kind::Anchor;
        sq.a_x = bn.x;
        sq.a_y = bn.y;
        sq.t   = 1.0;
        m_inflight_phantoms.push_back(sq);

        const double dx = bn.cx2 - bn.x;
        const double dy = bn.cy2 - bn.y;
        if (dx*dx + dy*dy > 1e-6) {
            PhantomShape h;
            h.kind = PhantomShape::Kind::HandleLine;
            h.a_x = bn.x;
            h.a_y = bn.y;
            h.b_x = bn.cx2;
            h.b_y = bn.cy2;
            h.t   = 1.0;
            m_inflight_phantoms.push_back(h);
        }
    }

    // Segments between fully-placed anchors
    for (int i = 1; i < m_anchors_finished && i < n; ++i) {
        const BezierNode& prev = m_target.nodes[i - 1];
        const BezierNode& curr = m_target.nodes[i];
        PhantomShape s;
        s.kind = PhantomShape::Kind::PartialSegment;
        s.a_x = prev.x;   s.a_y = prev.y;
        s.b_x = curr.x;   s.b_y = curr.y;
        s.c1_x = prev.cx2; s.c1_y = prev.cy2;
        s.c2_x = curr.cx1; s.c2_y = curr.cy1;
        s.t   = 1.0;
        m_inflight_phantoms.push_back(s);
    }

    // In-flight segment
    if (m_current_segment_dest >= 0 && m_current_segment_dest < n) {
        int src_idx = (m_current_segment_dest == 0)
                        ? (n - 1)
                        : (m_current_segment_dest - 1);
        if (src_idx >= 0 && src_idx < m_anchors_finished
            && m_current_segment_t < 1.0) {
            const BezierNode& src  = m_target.nodes[src_idx];
            const BezierNode& dest = m_target.nodes[m_current_segment_dest];
            PhantomShape s;
            s.kind = PhantomShape::Kind::PartialSegment;
            s.a_x = src.x;    s.a_y = src.y;
            s.b_x = dest.x;   s.b_y = dest.y;
            s.c1_x = src.cx2; s.c1_y = src.cy2;
            s.c2_x = dest.cx1; s.c2_y = dest.cy1;
            s.t   = m_current_segment_t;
            m_inflight_phantoms.push_back(s);
        }
    }

    // Anchor currently scaling in
    if (m_beat_idx < m_beats.size()) {
        const Beat& cur = m_beats[m_beat_idx];
        if (cur.kind == BeatKind::AnchorPlace && cur.node_idx >= 0
            && cur.node_idx < n) {
            const BezierNode& bn = m_target.nodes[cur.node_idx];
            PhantomShape sq;
            sq.kind = PhantomShape::Kind::Anchor;
            sq.a_x = bn.x;
            sq.a_y = bn.y;
            sq.t   = m_current_anchor_t;
            m_inflight_phantoms.push_back(sq);
        }
        if (cur.kind == BeatKind::HandleOut && cur.node_idx >= 0
            && cur.node_idx < n) {
            const BezierNode& bn = m_target.nodes[cur.node_idx];
            PhantomShape sq;
            sq.kind = PhantomShape::Kind::Anchor;
            sq.a_x = bn.x;
            sq.a_y = bn.y;
            sq.t   = 1.0;
            m_inflight_phantoms.push_back(sq);

            const double tip_x = bn.x + (bn.cx2 - bn.x) * m_current_handle_t;
            const double tip_y = bn.y + (bn.cy2 - bn.y) * m_current_handle_t;
            const double dx = tip_x - bn.x;
            const double dy = tip_y - bn.y;
            if (dx*dx + dy*dy > 1e-6) {
                PhantomShape h;
                h.kind = PhantomShape::Kind::HandleLine;
                h.a_x = bn.x;
                h.a_y = bn.y;
                h.b_x = tip_x;
                h.b_y = tip_y;
                h.t   = 1.0;
                m_inflight_phantoms.push_back(h);
            }
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// draw_overlay — paint phantoms (held + inflight) onto the canvas overlay
// ────────────────────────────────────────────────────────────────────────────
//
// Caller has already done cr->translate(ox, oy); cr->scale(zoom, zoom).
// Doc-space coords throughout. Draws held outlines first (so inflight
// over-paints them slightly when they overlap — but they shouldn't,
// since the in-flight path is the one currently being drawn and is
// inside the same container's scope).
void SvgPerformer::draw_overlay(
    const Cairo::RefPtr<Cairo::Context>& cr,
    double zoom,
    double creation_r,
    double creation_g,
    double creation_b) const {

    if (m_inflight_phantoms.empty() && m_held_phantoms.empty()) return;

    const double lever_r = creation_r + (1.0 - creation_r) * 0.5;
    const double lever_g = creation_g + (1.0 - creation_g) * 0.5;
    const double lever_b = creation_b + (1.0 - creation_b) * 0.5;

    const double sq_half = 3.0 / zoom;
    const double lw      = 1.0 / zoom;

    auto draw_one = [&](const PhantomShape& s) {
        switch (s.kind) {
            case PhantomShape::Kind::Anchor: {
                const double half = sq_half * s.t;
                if (half <= 0.0) break;
                cr->rectangle(s.a_x - half, s.a_y - half,
                              half * 2.0, half * 2.0);
                cr->set_source_rgba(creation_r, creation_g, creation_b, 1.0);
                cr->fill();
                break;
            }
            case PhantomShape::Kind::HandleLine: {
                cr->set_source_rgba(lever_r, lever_g, lever_b, 0.9);
                cr->set_line_width(lw);
                cr->move_to(s.a_x, s.a_y);
                cr->line_to(s.b_x, s.b_y);
                cr->stroke();
                cr->rectangle(s.b_x - sq_half, s.b_y - sq_half,
                              sq_half * 2.0, sq_half * 2.0);
                cr->set_source_rgba(creation_r, creation_g, creation_b, 1.0);
                cr->fill();
                break;
            }
            case PhantomShape::Kind::PartialSegment: {
                cr->set_source_rgba(creation_r, creation_g, creation_b, 1.0);
                cr->set_line_width(2.0 / zoom);
                cr->move_to(s.a_x, s.a_y);
                constexpr int kSamples = 32;
                const double t_end = s.t;
                for (int i = 1; i <= kSamples; ++i) {
                    const double ti = t_end * (double)i / (double)kSamples;
                    double px, py;
                    cubic_eval(s.a_x, s.a_y, s.c1_x, s.c1_y,
                               s.c2_x, s.c2_y, s.b_x, s.b_y,
                               ti, px, py);
                    cr->line_to(px, py);
                }
                cr->stroke();
                break;
            }
        }
    };

    for (const auto& s : m_held_phantoms)    draw_one(s);
    for (const auto& s : m_inflight_phantoms) draw_one(s);
}

} // namespace animation
} // namespace Curvz
