// scripting/AppScriptable.cpp ────────────────────────────────────────────────
//
// s263 m2 — THIRD headless-verb singleton implementation. Opens the
// `app` Scriptable at the code level. One verb (`version`) ships in
// m2 mirroring the proj.path / proj.has_path read template; m3 adds
// `gtk_version` as the second pure-read verb on the same singleton.
//
// s263 m3 — `gtk_version` added (no row tick; verb-on-existing-
// singleton). Both verbs are pure-read; same trust profile; same
// shape. The two ship in one delivery because they share structural
// shape — see header for the "discipline-transfer SIXTH instance"
// note.
//
// s263 v2 fix — both reads promoted to DUAL-SURFACE (verb + property)
// after smoke 63's first run showed `assert app version == "0.9.0"`
// failing with `expected "0.9.0", got null`. The DSL's `assert` is
// equality-only on query results (same constraint smoke 44's proj-
// save comment block names verbatim); the v1 ship took the verb-only
// path on a wrong reading of how the assertion routes. v2 adds
// matching query() branches and properties() entries — both reads are
// now reachable BOTH ways: imperative `app version` returns the value
// via invoke; `assert app version == "..."` reaches the same value via
// query. Convergent-evidence rule held: the v1 verb echo printed the
// right string (`= "0.9.0"`); the assertion's null was the gap. Same
// shape as proj.path / proj.has_path — pure-read identity exposed as
// property so assertions can reach it.
//
// ── Why this file is small ─────────────────────────────────────────────────
//
// Unlike ProjScriptable.cpp (~660 lines) and ExportScriptable.cpp
// (~700 lines), this file is tiny — both verbs return a const string
// with no refusal branches, no path-safety pre-flight, no project-
// state interrogation, no command-history threading. The shape isn't
// a sign of underbuilt-ness; it's a sign that "report process
// identity" is genuinely a one-liner once the build-time and runtime
// hooks are in place. The CMakeLists block that defines CURVZ_VERSION
// IS the work for `version`; this file just dereferences it.

#include "scripting/AppScriptable.hpp"

#include "CurvzLog.hpp"
#include "SvgParser.hpp"             // s290 m1a — for animating_parser_smoke
#include "AnimatingSvgParser.hpp"    // s290 m1a — for animating_parser_smoke
#include "SvgEmitter.hpp"            // s290 m1b.1 — for animating_emitter_smoke
#include "CurvzDocument.hpp"         // s290 m1a — for animating_parser_smoke
#include "SceneNode.hpp"             // s290 m1a — for animating_parser_smoke

#include <gtk/gtk.h>  // gtk_get_major_version() / _minor_ / _micro_ for m3

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// CURVZ_VERSION is plumbed via target_compile_definitions in
// CMakeLists.txt from project(curvz VERSION X.Y.Z). The macro is
// always defined by a normal build of the project; this fallback
// exists for two reasons:
//
//   1. Defensive — if a future CMakeLists edit accidentally drops
//      the target_compile_definitions line, the file still compiles
//      and the version verb returns "unknown" rather than failing
//      to build. The smoke would catch the regression
//      (assert app version == "0.9.0" would FAIL, observable in the
//      trace), so the defensive fallback doesn't hide bugs — it
//      shifts the failure from compile-time-error to test-time-
//      failure, which is the right shape (test failure is louder
//      and more diagnostic than a missing-macro compile error
//      thirty lines deep in a preprocessor expansion).
//
//   2. Sandbox-safe — if anyone ever builds this file outside the
//      project CMake (e.g. a single-file fuzz harness or a doc-
//      generation tool), the file still compiles. Same shape as
//      the GTK4 LOG_INFO macros' defensive #ifndef guards.
#ifndef CURVZ_VERSION
#define CURVZ_VERSION "unknown"
#endif

// ── s290 m1a — animating_parser_smoke helpers ──────────────────────────────
//
// Tree-walker that produces a one-line "shape signature" of a parsed
// CurvzDocument. Counts of major node types, top-level layer count,
// total nodes. Two docs with the same signature on the same SVG
// constitutes evidence that AnimatingSvgParser is producing identical
// output to the base parser.
//
// Not a deep equality check — it doesn't compare path data or fills.
// Convergent evidence: counts match AND Scott opens the file and
// sees the doc render correctly. If counts diverge, that's a bug.
// If counts match but visual differs, we know the issue is data-
// level, not structural.

namespace {

struct DocShape {
    int layer_count    = 0;
    int group_count    = 0;
    int path_count     = 0;
    int compound_count = 0;
    int clip_group     = 0;
    int blend_count    = 0;
    int warp_count     = 0;
    int text_count     = 0;
    int image_count    = 0;
    int ref_count      = 0;
    int guide_count    = 0;
    int other_count    = 0;
    int total_nodes    = 0;
};

void walk_for_shape(const Curvz::SceneNode* node, DocShape& out) {
    if (!node) return;
    ++out.total_nodes;
    using T = Curvz::SceneNode::Type;
    switch (node->type) {
        case T::Layer:      ++out.layer_count;    break;
        case T::Group:      ++out.group_count;    break;
        case T::Path:       ++out.path_count;     break;
        case T::Compound:   ++out.compound_count; break;
        case T::ClipGroup:  ++out.clip_group;     break;
        case T::Blend:      ++out.blend_count;    break;
        case T::Warp:       ++out.warp_count;     break;
        case T::Text:       ++out.text_count;     break;
        case T::Image:      ++out.image_count;    break;
        case T::Ref:        ++out.ref_count;      break;
        case T::Guide:      ++out.guide_count;    break;
        default:            ++out.other_count;    break;
    }
    for (const auto& child : node->children) {
        walk_for_shape(child.get(), out);
    }
    // ClipGroup's authoritative shape lives in clip_shape, not children
    if (node->clip_shape) walk_for_shape(node->clip_shape.get(), out);
    // Blend authoritative inputs
    if (node->blend_source_a) walk_for_shape(node->blend_source_a.get(), out);
    if (node->blend_source_b) walk_for_shape(node->blend_source_b.get(), out);
    // Warp authoritative input
    if (node->warp_source) walk_for_shape(node->warp_source.get(), out);
}

DocShape doc_shape(const Curvz::CurvzDocument* doc) {
    DocShape s;
    if (!doc) return s;
    for (const auto& layer : doc->layers) walk_for_shape(layer.get(), s);
    return s;
}

std::string format_shape(const DocShape& s) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "layers=%d groups=%d paths=%d compounds=%d clipgroups=%d "
                  "blends=%d warps=%d texts=%d images=%d refs=%d guides=%d "
                  "other=%d total=%d",
                  s.layer_count, s.group_count, s.path_count,
                  s.compound_count, s.clip_group, s.blend_count,
                  s.warp_count, s.text_count, s.image_count,
                  s.ref_count, s.guide_count, s.other_count,
                  s.total_nodes);
    return buf;
}

// s290 m1b.1 — LoggingEmitter is the consumer for the diagnostic
// animating_emitter_smoke verb. Counts on_path calls and logs each
// one with a brief summary of the path's shape (node count, fill
// type, stroke presence). The counter is the convergent-evidence
// channel: equal to `paths=N` from animating_parser_smoke on the
// same file = m1b.1 wiring is faithful.
//
// s290 m1b.2 — extended to track on_compound_begin/end and
// on_group_begin/end. Tracks running depth for each container kind
// so end-of-parse can verify all opens were balanced by closes
// (compound_depth and group_depth must return to 0). Reports
// totals against doc_shape: compound_begin count == compounds in
// tree; group_begin count == groups in tree.
//
// s290 m1b.3 — extended to track on_doc_metadata (fires once) and
// on_layer_begin/end. Layer depth tracked just like compound and
// group; layer_begin count == doc_shape.layer_count (the m1a walk
// only counts Type::Layer, matching m1b.3's filtering).
class LoggingEmitter : public Curvz::SvgEmitter {
public:
    int path_count = 0;

    int compound_begin_count = 0;
    int compound_end_count   = 0;
    int compound_depth       = 0;
    int compound_depth_max   = 0;

    int group_begin_count = 0;
    int group_end_count   = 0;
    int group_depth       = 0;
    int group_depth_max   = 0;

    int layer_begin_count = 0;
    int layer_end_count   = 0;
    int layer_depth       = 0;
    int layer_depth_max   = 0;

    int doc_metadata_count = 0;

    void on_path(const Curvz::SceneNode& node) override {
        ++path_count;
        const int node_count = (node.path ? static_cast<int>(node.path->nodes.size()) : 0);
        LOG_INFO("animating_emitter_smoke: on_path #{} "
                 "(ldepth={} cdepth={} gdepth={}) "
                 "nodes={} fill.type={} stroke.width={:.2f}",
                 path_count,
                 layer_depth, compound_depth, group_depth,
                 node_count,
                 static_cast<int>(node.fill.type),
                 node.stroke.width);
    }

    void on_compound_begin(const Curvz::SceneNode& node) override {
        ++compound_begin_count;
        ++compound_depth;
        if (compound_depth > compound_depth_max) compound_depth_max = compound_depth;
        LOG_INFO("animating_emitter_smoke: on_compound_begin #{} "
                 "name='{}' (ldepth={} cdepth->{} gdepth={})",
                 compound_begin_count, node.name,
                 layer_depth, compound_depth, group_depth);
    }

    void on_compound_end() override {
        ++compound_end_count;
        --compound_depth;
        LOG_INFO("animating_emitter_smoke: on_compound_end #{} "
                 "(ldepth={} cdepth->{} gdepth={})",
                 compound_end_count, layer_depth, compound_depth, group_depth);
    }

    void on_group_begin(const Curvz::SceneNode& node) override {
        ++group_begin_count;
        ++group_depth;
        if (group_depth > group_depth_max) group_depth_max = group_depth;
        LOG_INFO("animating_emitter_smoke: on_group_begin #{} "
                 "name='{}' (ldepth={} cdepth={} gdepth->{})",
                 group_begin_count, node.name,
                 layer_depth, compound_depth, group_depth);
    }

    void on_group_end() override {
        ++group_end_count;
        --group_depth;
        LOG_INFO("animating_emitter_smoke: on_group_end #{} "
                 "(ldepth={} cdepth={} gdepth->{})",
                 group_end_count, layer_depth, compound_depth, group_depth);
    }

    void on_layer_begin(const Curvz::SceneNode& node) override {
        ++layer_begin_count;
        ++layer_depth;
        if (layer_depth > layer_depth_max) layer_depth_max = layer_depth;
        LOG_INFO("animating_emitter_smoke: on_layer_begin #{} "
                 "name='{}' (ldepth->{} cdepth={} gdepth={})",
                 layer_begin_count, node.name,
                 layer_depth, compound_depth, group_depth);
    }

    void on_layer_end() override {
        ++layer_end_count;
        --layer_depth;
        LOG_INFO("animating_emitter_smoke: on_layer_end #{} "
                 "(ldepth->{} cdepth={} gdepth={})",
                 layer_end_count, layer_depth, compound_depth, group_depth);
    }

    void on_doc_metadata(const Curvz::CanvasModel& canvas) override {
        ++doc_metadata_count;
        LOG_INFO("animating_emitter_smoke: on_doc_metadata #{} "
                 "ratio_w={:.3f} ratio_h={:.3f} quality={} "
                 "intended=({:.2f}x{:.2f} {}) display_mode={}",
                 doc_metadata_count,
                 canvas.ratio_w, canvas.ratio_h, canvas.quality,
                 canvas.intended_w, canvas.intended_h,
                 canvas.intended_unit.empty() ? "px" : canvas.intended_unit,
                 static_cast<int>(canvas.display_mode));
    }
};

} // anonymous namespace

namespace curvz::scripting {

AppScriptable::AppScriptable(Curvz::MainWindow* main_window,
                             EnactPenPath  enact_pen_path,
                             AnimateSvgFile animate_svg)
    : Scriptable("app")
    , m_main_window(main_window)
    , m_enact_pen_path(std::move(enact_pen_path))
    , m_animate_svg(std::move(animate_svg)) {
    // Registry registration happens in the Scriptable base ctor under
    // the name "app". MainWindow holds us as a member; the registry
    // entry lives for the window's lifetime. The MainWindow pointer
    // is stashed but unused in m2 / m3 — see header for the rationale
    // (kept for symmetry with proj / export and for future verbs).
}

ScriptValue AppScriptable::invoke(std::string_view verb,
                                  const ScriptArgs& args) {
    LOG_INFO("DIAG AppScriptable::invoke verb='{}' args.size={} mw={}",
             std::string(verb), args.size(),
             m_main_window ? "OK" : "NULL");

    // s263 m2 — `version` verb. Pure read; no MainWindow state access.
    // Zero-arg only — any positional argument is a misuse of the verb
    // and refuses with a structured error mirroring the export
    // Scriptable's argument-count guards (Branch B equivalent).
    if (verb == "version") {
        if (!args.empty()) {
            throw std::runtime_error(
                "app version: expected zero arguments");
        }
        // CURVZ_VERSION is a string-literal macro plumbed from
        // CMakeLists via target_compile_definitions. The expansion
        // happens at preprocess time; the runtime cost is one
        // pointer copy into the ScriptValue.
        return ScriptValue::text(CURVZ_VERSION);
    }

    // s263 m3 — `gtk_version` verb. Pure read; reports runtime GTK
    // library version rather than the build-against version. Same
    // zero-arg shape as `version`.
    if (verb == "gtk_version") {
        if (!args.empty()) {
            throw std::runtime_error(
                "app gtk_version: expected zero arguments");
        }
        // gtk_get_major_version() and friends are guaranteed to
        // return values for the currently-loaded GTK; they're
        // initialised by GTK's own startup before main() reaches
        // here. std::to_string is the simplest stringify; the dotted
        // format matches conventional version-string presentation
        // (e.g. "4.14.5"), which is what bug reports want and what
        // the smoke can substring-assert against once the listener
        // gains a starts-with primitive (m4+ work).
        std::string s = std::to_string(gtk_get_major_version());
        s += '.';
        s += std::to_string(gtk_get_minor_version());
        s += '.';
        s += std::to_string(gtk_get_micro_version());
        return ScriptValue::text(s);
    }

    // s288 m2 — `enact_pen_path "<d>" <speed>` verb. Kicks off a
    // Pen-path performance via the m_enact_pen_path callback (MainWindow
    // wires it to Canvas::perform_pen_path under the SvgPerformer
    // rename in s291 m2; the verb name on this surface stays unchanged).
    //
    // Args (positional):
    //   0  d_string : SVG-d attribute string in user-space (Y-up,
    //                 bottom-left origin)
    //   1  speed    : double, multiplier on beat durations (1.0 nominal)
    //
    // Returns immediately. The animation runs asynchronously on the
    // main loop via Glib::Timeout owned by the SvgPerformer. The
    // script's next line dispatches before the animation completes.
    //
    // Gift-shape graceful degradation: missing args, empty d-string,
    // empty callback — all return ScriptValue::null() silently. No
    // exception, no error dialog. The audience sees no animation
    // rather than a broken state.
    //
    // This verb is m2's TESTING surface — the welcome demo's eventual
    // shape is a single `app.animate_svg` verb that orchestrates many
    // enact_pen_path calls (plus enact_color_pick, etc.) internally
    // from a parsed SVG. m2 exposes the lower-level verb so the smoke
    // can prove the Pen-path performance in isolation.
    if (verb == "enact_pen_path") {
        if (args.size() < 1) return ScriptValue::null();
        if (!m_enact_pen_path) return ScriptValue::null();

        std::string d_string;
        if (args[0].kind == ValueKind::String) d_string = args[0].s;
        if (d_string.empty()) return ScriptValue::null();

        double speed = 1.0;
        if (args.size() >= 2) {
            if      (args[1].kind == ValueKind::Double) speed = args[1].d;
            else if (args[1].kind == ValueKind::Int)
                speed = static_cast<double>(args[1].i);
        }
        if (speed <= 0.0) speed = 1.0;

        m_enact_pen_path(d_string, speed);
        return ScriptValue::null();
    }

    // s288 m3 — `animate_svg "<path>" <speed>` verb. SVG orchestrator
    // entry point. Same shape as enact_pen_path, different payload:
    // arg 0 is a filesystem path to an SVG file (m3 takes literal
    // filesystem paths; gresource-alias resolution is a future
    // enhancement).
    //
    // The animator parses the SVG via Curvz's own SvgParser, walks
    // the resulting temp doc tree, enqueues one Pen-path performance
    // per Path SceneNode, and plays them back-to-back with a small
    // inter-path breath. By orchestration end the doc has the SVG's
    // paths rendered as committed SceneNodes with their original
    // fill/stroke.
    //
    // Returns immediately. Same gift-shape graceful degradation as
    // enact_pen_path — empty path, missing file, parse failure, no
    // paths in the SVG: all silent no-ops.
    if (verb == "animate_svg") {
        if (args.size() < 1) return ScriptValue::null();
        if (!m_animate_svg) return ScriptValue::null();

        std::string svg_path;
        if (args[0].kind == ValueKind::String) svg_path = args[0].s;
        if (svg_path.empty()) return ScriptValue::null();

        double speed = 1.0;
        if (args.size() >= 2) {
            if      (args[1].kind == ValueKind::Double) speed = args[1].d;
            else if (args[1].kind == ValueKind::Int)
                speed = static_cast<double>(args[1].i);
        }
        if (speed <= 0.0) speed = 1.0;

        m_animate_svg(svg_path, speed);
        return ScriptValue::null();
    }

    // s290 m1a — diagnostic verb. Parses the given SVG via both the
    // base parser (parse_svg_file) and the new AnimatingSvgParser, then
    // walks both resulting CurvzDocuments and logs a one-line shape
    // signature for each. Equal signatures = m1a passes for that file.
    //
    // Not wired to MainWindow / Canvas; entirely self-contained in this
    // TU. Returns ScriptValue::null() like the other animation verbs.
    if (verb == "animating_parser_smoke") {
        if (args.size() < 1) return ScriptValue::null();
        std::string svg_path;
        if (args[0].kind == ValueKind::String) svg_path = args[0].s;
        if (svg_path.empty()) return ScriptValue::null();

        LOG_INFO("animating_parser_smoke: parsing '{}'", svg_path);

        // Base parser
        auto base_doc = Curvz::parse_svg_file(svg_path);
        if (!base_doc) {
            LOG_INFO("animating_parser_smoke: BASE parser returned null");
        } else {
            DocShape s = doc_shape(base_doc.get());
            LOG_INFO("animating_parser_smoke: BASE   {}", format_shape(s));
        }

        // Animating parser
        Curvz::AnimatingSvgParser ap;
        auto anim_doc = ap.parse_file(svg_path);
        if (!anim_doc) {
            LOG_INFO("animating_parser_smoke: ANIM parser returned null");
        } else {
            DocShape s = doc_shape(anim_doc.get());
            LOG_INFO("animating_parser_smoke: ANIM   {}", format_shape(s));
        }

        if (base_doc && anim_doc) {
            DocShape sb = doc_shape(base_doc.get());
            DocShape sa = doc_shape(anim_doc.get());
            bool equal = (sb.layer_count    == sa.layer_count    &&
                          sb.group_count    == sa.group_count    &&
                          sb.path_count     == sa.path_count     &&
                          sb.compound_count == sa.compound_count &&
                          sb.clip_group     == sa.clip_group     &&
                          sb.blend_count    == sa.blend_count    &&
                          sb.warp_count     == sa.warp_count     &&
                          sb.text_count     == sa.text_count     &&
                          sb.image_count    == sa.image_count    &&
                          sb.ref_count      == sa.ref_count      &&
                          sb.guide_count    == sa.guide_count    &&
                          sb.other_count    == sa.other_count    &&
                          sb.total_nodes    == sa.total_nodes);
            LOG_INFO("animating_parser_smoke: shapes {}",
                     equal ? "MATCH" : "DIFFER");
        }
        return ScriptValue::null();
    }

    // s290 m1b.1 — emitter diagnostic verb. Parses the given SVG via
    // AnimatingSvgParser{&LoggingEmitter}, then compares the
    // LoggingEmitter's path count against the doc-shape walk's
    // paths=N total. Equal counts = on_path is firing once per
    // committed Path SceneNode, which is m1b.1's verification gate.
    //
    // The two counts converge on the same number through different
    // channels: the LoggingEmitter counts at emit time (parser-side
    // signal), and the doc_shape walk counts after the parse (tree
    // inspection). If they differ, the emit stream is missing or
    // duplicating Path commits and the bug is the wiring in
    // AnimatingSvgParser::parse, not the consumer.
    if (verb == "animating_emitter_smoke") {
        if (args.size() < 1) return ScriptValue::null();
        std::string svg_path;
        if (args[0].kind == ValueKind::String) svg_path = args[0].s;
        if (svg_path.empty()) return ScriptValue::null();

        LOG_INFO("animating_emitter_smoke: parsing '{}'", svg_path);

        LoggingEmitter logger;
        Curvz::AnimatingSvgParser ap(&logger);
        auto doc = ap.parse_file(svg_path);
        if (!doc) {
            LOG_INFO("animating_emitter_smoke: parser returned null");
            return ScriptValue::null();
        }

        DocShape shape = doc_shape(doc.get());
        LOG_INFO("animating_emitter_smoke: emitter on_path count = {}",
                 logger.path_count);
        LOG_INFO("animating_emitter_smoke: doc shape paths = {}",
                 shape.path_count);
        const bool path_ok = (logger.path_count == shape.path_count);

        // s290 m1b.2 — container event verification.
        LOG_INFO("animating_emitter_smoke: emitter compound_begin={} compound_end={} "
                 "depth_max={} | doc shape compounds={}",
                 logger.compound_begin_count, logger.compound_end_count,
                 logger.compound_depth_max, shape.compound_count);
        LOG_INFO("animating_emitter_smoke: emitter group_begin={} group_end={} "
                 "depth_max={} | doc shape groups={}",
                 logger.group_begin_count, logger.group_end_count,
                 logger.group_depth_max, shape.group_count);

        // s290 m1b.3 — layer + doc_metadata verification.
        LOG_INFO("animating_emitter_smoke: emitter layer_begin={} layer_end={} "
                 "depth_max={} | doc shape layers={}",
                 logger.layer_begin_count, logger.layer_end_count,
                 logger.layer_depth_max, shape.layer_count);
        LOG_INFO("animating_emitter_smoke: emitter doc_metadata_count={}",
                 logger.doc_metadata_count);

        const bool compound_balanced = (logger.compound_depth == 0);
        const bool compound_total_ok = (logger.compound_begin_count == shape.compound_count);
        const bool compound_pairs_ok = (logger.compound_begin_count == logger.compound_end_count);

        const bool group_balanced = (logger.group_depth == 0);
        const bool group_total_ok = (logger.group_begin_count == shape.group_count);
        const bool group_pairs_ok = (logger.group_begin_count == logger.group_end_count);

        const bool layer_balanced = (logger.layer_depth == 0);
        const bool layer_total_ok = (logger.layer_begin_count == shape.layer_count);
        const bool layer_pairs_ok = (logger.layer_begin_count == logger.layer_end_count);

        // doc_metadata is expected to fire exactly once per parse
        // for well-formed input that has an <svg> root tag.
        const bool doc_metadata_ok = (logger.doc_metadata_count == 1);

        const bool all_ok = path_ok && compound_balanced && compound_total_ok &&
                            compound_pairs_ok && group_balanced && group_total_ok &&
                            group_pairs_ok && layer_balanced && layer_total_ok &&
                            layer_pairs_ok && doc_metadata_ok;

        if (all_ok) {
            LOG_INFO("animating_emitter_smoke: MATCH (emit stream is faithful)");
        } else {
            LOG_INFO("animating_emitter_smoke: DIFFER -- "
                     "path={} compound_balanced={} compound_total={} "
                     "compound_pairs={} group_balanced={} group_total={} "
                     "group_pairs={} layer_balanced={} layer_total={} "
                     "layer_pairs={} doc_metadata={}",
                     path_ok, compound_balanced, compound_total_ok,
                     compound_pairs_ok, group_balanced, group_total_ok,
                     group_pairs_ok, layer_balanced, layer_total_ok,
                     layer_pairs_ok, doc_metadata_ok);
        }
        return ScriptValue::null();
    }

    // Unknown verb falls through to a structured error. Same shape
    // as the unknown-verb branches in proj / export Scriptables.
    throw std::runtime_error(
        "app: unknown verb '" + std::string(verb) + "'");
}

ScriptValue AppScriptable::query(std::string_view property) const {
    // s263 v2 — both reads are dual-surface (verb + property). The
    // property path exists so the DSL's equality-only `assert` can
    // reach the same value the verb path returns when invoked
    // imperatively. See header "Query surface" block for the
    // rationale and the s263 v1 → v2 trace evidence.
    //
    // Queries are NOT RunContext-gated — reads can't corrupt
    // (CANON's RunContext entry: "Every model query. Reads can't
    // corrupt."). Both branches mirror the invoke-side bodies
    // exactly; the duplication is two lines and consolidating into
    // a helper would obscure that the contracts are independent.

    if (property == "version") {
        return ScriptValue::text(CURVZ_VERSION);
    }

    if (property == "gtk_version") {
        std::string s = std::to_string(gtk_get_major_version());
        s += '.';
        s += std::to_string(gtk_get_minor_version());
        s += '.';
        s += std::to_string(gtk_get_micro_version());
        return ScriptValue::text(s);
    }

    // Unknown property — listener handles the Null fall-through as
    // "unknown property" rather than a hard error, matching every
    // Scriptable's behaviour for unrecognised property names.
    return ScriptValue::null();
}

std::vector<std::string> AppScriptable::verbs() const {
    // s263 m2 / s263 m3 — two pure-read verbs.
    // s288 m2 — added the welcome-demo Pen-path performance verb.
    // s288 m3 — added the SVG orchestrator verb.
    // s290 m1a — added the AnimatingSvgParser diagnostic smoke verb.
    // s290 m1b.1 — added the SvgEmitter diagnostic smoke verb.
    return {
        "version",
        "gtk_version",
        "enact_pen_path",
        "animate_svg",
        "animating_parser_smoke",
        "animating_emitter_smoke",
    };
}

std::vector<std::string> AppScriptable::properties() const {
    // s263 v2 — dual-surface posture: both reads are verbs AND
    // properties. See header "Query surface" block.
    return {
        "version",
        "gtk_version",
    };
}

// s263 m2 / s263 m3 — RunContext mask declarations.
//
// Both verbs declare ctx::Scripter | ctx::TestRunner. Macro is OUT
// for both — recorded-macro replay on a different build (for
// `version`) or a different system (for `gtk_version`) means the
// recorded value is stale at replay time, which violates the
// recorded-macro mental model. The mask shape is identical to
// proj.save and export.svg; the catalogue uniformises further at
// the s263 close.
//
// Unknown verbs (typos, future verbs not yet declared) fall through
// to the base default of ctx::all_three. That matches the note in
// Scriptable::context_mask: "Unknown verb names (e.g. typos) SHOULD
// fall through to the default — the invoke() method itself is
// responsible for noticing the verb is unknown." (Invoke's
// unknown-verb branch above handles the actual refusal.)
//
// Tenth and eleventh consumers of context_mask() (Inspector +
// 5 proj verbs + 2 export verbs + 2 app verbs). Registry-promotion
// clock at 10/n and STILL HELD by design — see header
// AppScriptable.hpp's context_mask declaration block for the
// at-ten reasoning (sibling shape to proj.save / export.svg; no
// novel mask shape; catalogue uniformising further).
RunContextMask
AppScriptable::context_mask(std::string_view verb) const {
    if (verb == "version") {
        return ctx::Scripter | ctx::TestRunner;
    }
    if (verb == "gtk_version") {
        return ctx::Scripter | ctx::TestRunner;
    }
    // s288 m2 — welcome-demo Pen-path performance. Scripter for
    // interactive testing during dev (Scott runs it in the Scripter
    // window during the welcome demo arc); TestRunner for headless
    // smokes. Macro OUT: animation is wall-clock-dependent, replaying
    // a recorded macro would land at unpredictable times and the
    // recorded timing wouldn't be meaningful at replay.
    if (verb == "enact_pen_path") {
        return ctx::Scripter | ctx::TestRunner;
    }
    // s288 m3 — SVG orchestrator. Same rationale as enact_pen_path.
    if (verb == "animate_svg") {
        return ctx::Scripter | ctx::TestRunner;
    }
    // s290 m1a — AnimatingSvgParser diagnostic smoke. Scripter for
    // interactive verification during the m1a milestone; TestRunner so
    // it can be invoked from headless smokes. Macro OUT — diagnostic
    // logging output isn't meaningful in a recorded macro.
    if (verb == "animating_parser_smoke") {
        return ctx::Scripter | ctx::TestRunner;
    }
    // s290 m1b.1 — SvgEmitter diagnostic smoke. Same trust profile
    // as animating_parser_smoke; same diagnostic posture.
    if (verb == "animating_emitter_smoke") {
        return ctx::Scripter | ctx::TestRunner;
    }
    return ctx::all_three;
}

} // namespace curvz::scripting
