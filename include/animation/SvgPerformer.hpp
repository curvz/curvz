// animation/SvgPerformer.hpp ─────────────────────────────────────────────────
//
// s291 m2 — RENAMES `WelcomeAnimator` to `SvgPerformer` and re-architects
// it as a tree-walking animator that drives the FINISHED output of the
// base SvgParser. The phantom-overlay vocabulary and the per-path beat
// machinery (AnchorPlace / HandleOut / SegmentGrow / Interlude) survive
// from s288 m2; the structural change is everything around them.
//
// ── How m2 differs from earlier shapes ───────────────────────────────────────
//
// s288 m3 (the WelcomeAnimator name): parse SVG to a temp doc, walk the
// temp doc's tree FLATTENING containers into a list of Path SceneNodes,
// queue one PendingPerformance per Path, then bbox-fit + y-flip + center
// the queue against the target doc. Animate each Path independently and
// commit it at the layer's top level.
//
// Earlier s291 m2 drafts: subscribe to AnimatingSvgParser's parse stream
// via SvgEmitter, build a queue from the on_path callbacks. Same flatten
// problem — Compound bracketing didn't translate to Compound commits, so
// compounds rendered as overlapping subpaths instead of one filled-with-
// holes shape.
//
// s291 m2 final: drop the SvgEmitter machinery and walk the parser's
// finished tree directly, type-dispatched the same way Canvas::draw_object
// and DocumentGallery::render_thumb do. Containers (Compound, Group,
// ClipGroup, Blend, Warp, Layer) preserve their identity through the
// performance and commit as real container SceneNodes carrying the
// container-specific bits the parser stamped on them. The renderer then
// paints them correctly because that's the renderer's job — we don't
// duplicate any render logic in the performer.
//
// The parser handles every SVG feature already (transforms, primitives,
// fill-rule, paint inheritance, nested containers, foreign-tool quirks).
// File→Open, Import, Library, Template — every existing call site uses
// `Curvz::parse_svg_file()` and gets a correct tree. The performer is
// just another consumer of that same correct tree, with beats interpolated
// in front of each Path's commit and container-commits deferred until
// their children's beats complete.
//
// ── The ghost-draw shape ─────────────────────────────────────────────────────
//
// For each leaf Path encountered while walking the tree:
//
//   1. Beat-perform the path's geometry on the phantom overlay layer.
//      Anchors appear, handles extend, segments grow — same vocabulary
//      as s288 m2. No real SceneNode exists yet; nothing is in the doc.
//
//   2. When the beats finish:
//      - If the path's nearest enclosing container is the user's target
//        layer (i.e. the Path is top-level), commit a real Path SceneNode
//        into that layer with fill/stroke from the parser's tree, then
//        clear the path's phantoms. The renderer takes over.
//      - If the path is inside a Compound / Group / ClipGroup / Blend /
//        Warp, the path's final geometry is HELD on the overlay as a
//        static outline (no fill yet), and a fresh Path SceneNode is
//        stashed in the enclosing container's "pending children" list.
//        Phantoms persist on the overlay; no doc commit yet.
//
//   3. When the last child of a container finishes, mint a real container
//      SceneNode of the right type, copy the parser's container-specific
//      bits (Compound fill, Group transform, ClipGroup clip_shape, Blend
//      sources, Warp envelope), move the pending children inside, insert
//      the container into ITS enclosing container (or the target layer
//      for top-level containers). At this commit, all the held outlines
//      for that container's contents clear, and the renderer paints the
//      committed container correctly — fill-rule, blend mode, clip shape,
//      transform — using the same logic File→Open's output uses.
//
// Insertion is always at `children.begin()` so each commit lands at index
// 0 (top in Curvz's convention). Walking children in reverse (size-1 → 0)
// gives the visual sequence "bottom paints first, top paints last,"
// matching the order Canvas::draw_object and DocumentGallery::render_thumb
// iterate.
//
// ── m2 deliberate non-goals ──────────────────────────────────────────────────
//
//   - Page setup. The performance lands in the caller-supplied doc + layer.
//     If the doc's canvas isn't sized to host the SVG's viewBox, paths
//     render at SVG-authored coordinates which may extend off-canvas. The
//     "create a doc sized to match the SVG" work belongs in a separate
//     milestone (m3) and is independently scoped.
//
//   - Specialty beats for non-Path leaves (Image, Text, Ref). m2 commits
//     these directly without animation — they appear when their nearest
//     enclosing container commits. Future enhancement: type-write beats
//     for Text, fade-in for Image, etc.
//
//   - Animating container effects themselves. A Warp's deformation
//     activates at warp-commit instantaneously; we don't animate the
//     envelope morphing into place. Compound's evenodd fill activates at
//     compound-commit. Future enhancement banked.
//
//   - Scripter hide. m4 work.
//   - Humanization, easing, primitive-aware beats. m6+.
//
// ── Convergent-evidence gate ─────────────────────────────────────────────────
//
// Smoke 68 (welcome_scott_bug.curvzs): scott-bug renders as a faithful
// reproduction of what File→Open produces — background circle at the
// bottom, blue body above, white highlight, dark features on top, each
// constructed anchor-by-anchor, fills appearing as compounds commit
// (the three detail paths are foreign multi-subpath compounds; the parser
// builds them as Compound SceneNodes with evenodd fill-rule by topology;
// the performer commits them as Compounds and the renderer fills correctly).
// Trace shows tree walk depth, container open/close events, beats per
// path, commits.

#pragma once

#include "math/BezierPath.hpp"
#include "SceneNode.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Curvz {

class CurvzDocument;
class Canvas;

namespace animation {

// One phantom shape the renderer should draw. Tagged union; the kind
// determines which fields are meaningful. Doc-space coordinates throughout.
struct PhantomShape {
    enum class Kind {
        Anchor,         // single point with optional scale-in fraction
        HandleLine,     // anchor → tip with handle-square at tip
        PartialSegment, // cubic from p1→p2 along control points, fractional t
    };

    Kind   kind = Kind::Anchor;

    // Anchor + handle-line share the anchor position (a, b).
    // PartialSegment uses p1/p2/c1/c2 + t.
    double a_x = 0.0, a_y = 0.0;
    double b_x = 0.0, b_y = 0.0;
    double c1_x = 0.0, c1_y = 0.0;
    double c2_x = 0.0, c2_y = 0.0;
    double t   = 1.0;
};

class SvgPerformer {
public:
    explicit SvgPerformer(Canvas* canvas);

    // No copies; the performer owns its Glib::Timeout connection.
    SvgPerformer(const SvgPerformer&)            = delete;
    SvgPerformer& operator=(const SvgPerformer&) = delete;
    SvgPerformer(SvgPerformer&&)                 = delete;
    SvgPerformer& operator=(SvgPerformer&&)      = delete;

    // ── Single-path entry point (smoke 67) ──────────────────────────────────
    //
    // Kick off a Pen-path performance from a raw d-string. Parses the
    // d-string, builds a beat list, runs the timeout. Returns immediately.
    // No tree walking — there's no SVG, just a path.
    void enact_pen_path(const std::string& d_string, double speed,
                        CurvzDocument* doc, SceneNode* layer);

    // ── SVG-file entry point (smoke 68) ─────────────────────────────────────
    //
    // Parses the SVG via `Curvz::parse_svg_file()` — the same entry every
    // File→Open, Import, Library, and Template-loading code path uses.
    // Walks the resulting tree depth-first, building a performance plan
    // of leaf-Path beats and container open/close steps. Plays the plan
    // on the GTK main loop.
    void perform(const std::string& svg_path, double speed,
                 CurvzDocument* doc, SceneNode* layer);

    bool is_playing() const { return m_playing; }

    void draw_overlay(const Cairo::RefPtr<Cairo::Context>& cr,
                      double zoom,
                      double creation_r,
                      double creation_g,
                      double creation_b) const;

private:
    // ── Beat machinery (unchanged from s288 m2) ─────────────────────────────

    enum class BeatKind {
        AnchorPlace,
        HandleOut,
        SegmentGrow,
        Interlude,
        SubpathBreak,
    };

    struct Beat {
        BeatKind kind = BeatKind::Interlude;
        int      node_idx = -1;
        double   duration_ms = 0.0;
    };

    void build_beat_list(const PathData& pd, double speed);
    bool tick();
    void rebuild_phantoms();

    // Render m_target's full geometry as a static outline (all anchors
    // placed, all handles extended, all segments fully grown). Used when
    // the path is inside an open container: append to m_held_phantoms so
    // it stays on the overlay until the container commits.
    void append_target_to_held();

    // ── Performance plan (new in s291 m2) ───────────────────────────────────

    enum class StepKind {
        PerformPath,      // beat-perform a Path's geometry, then commit
        OpenContainer,    // push a pending-container onto m_container_stack
        CloseContainer,   // pop, mint the container SceneNode, insert
        CommitLeaf,       // non-Path leaf (Image, Text, Ref) — no beats
    };

    struct Step {
        StepKind kind = StepKind::PerformPath;

        // PerformPath / CommitLeaf use this. Holds a deep-copied SceneNode
        // sans children — the source's geometry + all metadata, ready to
        // be moved into the doc at commit time.
        std::unique_ptr<SceneNode> node_prototype;

        double speed = 1.0;  // PerformPath only
    };

    // Build the plan by recursive descent through the parser's tree.
    void build_plan_for_node(const SceneNode& src, int depth);
    void build_plan_for_layer_children(const SceneNode& layer);

    // Deep-copy a SceneNode WITHOUT its children + composite slots. The
    // performer rebuilds the children/composites via the plan's
    // OpenContainer / CloseContainer bracketing; copying them here would
    // double-insert. Container-specific composite slots (clip_shape,
    // blend_source_a/b, warp_source) are also reset to null and rebuilt
    // through the plan.
    static std::unique_ptr<SceneNode> clone_node_shell(const SceneNode& src);

    // ── Plan execution state ────────────────────────────────────────────────

    std::vector<Step> m_plan;
    size_t            m_plan_idx = 0;

    // Pending-container stack. Each entry holds the container being
    // assembled (its prototype carrying type + container-specific bits)
    // and its accumulating children. When CloseContainer fires, the top
    // of this stack is consumed.
    struct PendingContainer {
        std::unique_ptr<SceneNode>              prototype;
        std::vector<std::unique_ptr<SceneNode>> pending_children;
    };
    std::vector<PendingContainer> m_container_stack;

    // ── Phantom state ───────────────────────────────────────────────────────

    std::vector<PhantomShape> m_inflight_phantoms;
    std::vector<PhantomShape> m_held_phantoms;

    // ── Per-current-path state ──────────────────────────────────────────────

    PathData     m_target;
    FillStyle    m_current_fill;
    StrokeStyle  m_current_stroke;

    std::vector<Beat> m_beats;
    size_t            m_beat_idx = 0;
    std::chrono::steady_clock::time_point m_t_beat_start;

    int    m_anchors_finished     = 0;
    double m_current_anchor_t     = 0.0;
    double m_current_handle_t     = 0.0;
    double m_current_segment_t    = 0.0;
    int    m_current_segment_dest = -1;

    // ── Orchestration ───────────────────────────────────────────────────────

    // Advance through the plan. Synchronous steps (OpenContainer /
    // CloseContainer / CommitLeaf) execute and loop to the next step;
    // PerformPath steps kick off beats and return. Returns false when
    // the plan is exhausted (and the reveal sequence schedules).
    bool advance_plan();

    // Mint a Path SceneNode from m_plan[m_plan_idx]'s prototype + the
    // just-finished m_target geometry; route into the user's layer or
    // the open container's pending children. Called at end-of-beats.
    void commit_current_path();

    // Pop m_container_stack's top, move pending children inside the
    // prototype, insert into the next-outer commit target.
    void commit_pending_container();

    // Where does a freshly-minted SceneNode go? Either the top of
    // m_container_stack (if any) or the user's target layer.
    void route_committed_node(std::unique_ptr<SceneNode> node);

    // ── Performance bookkeeping ─────────────────────────────────────────────

    Canvas*  m_canvas = nullptr;
    bool     m_playing = false;

    CurvzDocument* m_target_doc   = nullptr;
    SceneNode*     m_target_layer = nullptr;

    double m_inter_step_breath_ms_base = 200.0;
    double m_current_speed = 1.0;
};

} // namespace animation
} // namespace Curvz
