#pragma once
#include "UnitSystem.hpp"
#include "SceneNode.hpp"
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace Curvz {

// ── Canvas model ──────────────────────────────────────────────────────────────
// All documents are stored as ratio + quality. Physical sizes, DPI, and pixel
// dimensions are presentation hints only — they never affect geometry.
//
// Normalisation rule: min(ratio_w, ratio_h) == 1.0 always.
// quality == unit count on the short axis.
//
// Derived dimensions (never stored):
//   canvas_width()  = round(quality * ratio_w)
//   canvas_height() = round(quality * ratio_h)

enum class DisplayMode { Pixel, Physical, RatioQuality };

// Motif — the project-level workspace appearance. Drives which CSS fork the
// app paints (Dark/Light), and (future) which palette Canvas reads for editor
// chrome (rulers, guides, node dots, marquee). Distinct from icon-design
// output — the user's drawn art is unaffected by motif. Saved per-project
// in project.json; old projects without the key load as Dark.
//
// Adding a new motif: extend the enum, extend motif_to_string /
// motif_from_string in CurvzProject.cpp, add a CSS fork in css.hpp.
enum class Motif { Dark, Light };

struct CanvasModel {
    // ── Core (stored) ─────────────────────────────────────────────────────
    double ratio_w  = 1.0;    // width  ratio component, normalised: min(w,h)=1.0
    double ratio_h  = 1.0;    // height ratio component, normalised: min(w,h)=1.0
    int    quality  = 24;     // unit count on the short axis

    // ── Display hint (stored, UI only — does not affect geometry) ─────────
    DisplayMode display_mode  = DisplayMode::Pixel;
    // Physical mode
    double      phys_width    = 1.0;   // in phys_unit
    double      phys_height   = 1.0;
    std::string phys_unit     = "in";  // "in", "mm", "cm"
    int         dpi           = 300;
    // Pixel mode
    int         px_width      = 24;
    int         px_height     = 24;
    // Unit preference — affects rulers and inspector fields (px/in/mm/pt)
    Unit        display_unit  = Unit::Px;

    // ── Derived ───────────────────────────────────────────────────────────
    int canvas_width()  const { return (int)std::round(quality * ratio_w); }
    int canvas_height() const { return (int)std::round(quality * ratio_h); }

    // ── Factory helpers ───────────────────────────────────────────────────

    // From raw ratio doubles (e.g. 16.0, 9.0) and quality
    static CanvasModel from_ratio(double raw_w, double raw_h, int qual) {
        CanvasModel m;
        double short_axis = std::min(raw_w, raw_h);
        m.ratio_w      = raw_w / short_axis;
        m.ratio_h      = raw_h / short_axis;
        m.quality      = qual;
        m.display_mode = DisplayMode::RatioQuality;
        return m;
    }

    // From pixel dimensions (24x24, 32x32, etc.)
    static CanvasModel from_pixels(int pw, int ph) {
        CanvasModel m;
        double short_axis = (double)std::min(pw, ph);
        m.ratio_w      = pw / short_axis;
        m.ratio_h      = ph / short_axis;
        m.quality      = std::min(pw, ph);
        m.display_mode = DisplayMode::Pixel;
        m.px_width     = pw;
        m.px_height    = ph;
        return m;
    }

    // From physical dimensions + DPI. unit: "in", "mm", "cm"
    static CanvasModel from_physical(double w, double h,
                                     const std::string& unit, int dpi_val) {
        double scale = dpi_val;
        if      (unit == "mm") scale = dpi_val / 25.4;
        else if (unit == "cm") scale = dpi_val / 2.54;
        double px_w = w * scale;
        double px_h = h * scale;
        double short_axis = std::min(px_w, px_h);
        CanvasModel m;
        m.ratio_w      = px_w / short_axis;
        m.ratio_h      = px_h / short_axis;
        m.quality      = (int)std::round(short_axis);
        m.display_mode = DisplayMode::Physical;
        m.phys_width   = w;
        m.phys_height  = h;
        m.phys_unit    = unit;
        m.dpi          = dpi_val;
        return m;
    }

    // Backwards compatibility: load from legacy integer canvas_w / canvas_h
    static CanvasModel from_legacy(int cw, int ch) {
        return from_pixels(cw, ch);
    }
};

// ── SnapSettings ──────────────────────────────────────────────────────────────
struct SnapSettings {
    bool enabled       = true;
    bool snap_guides   = true;
    bool snap_grid     = false;
    bool snap_margins  = false;
    bool snap_nodes    = true;
    bool snap_edges    = false;
    bool snap_centers  = false;
};

// ── Document ──────────────────────────────────────────────────────────────────
struct CurvzDocument {
    CanvasModel canvas;
    std::string filename;

    // Top-level children are always Layer or GuideLayer nodes
    std::vector<std::unique_ptr<SceneNode>> layers;

    // ── Iid → SceneNode* index (s167 m1) ──────────────────────────────
    //
    // Flat map projection of the tree, keyed on SceneNode::internal_id
    // (the stable UUID, not the SVG `id` attribute). The tree is the
    // authoritative structure; this map is the "poor man's tree" — a
    // lookup view rebuilt lazily when the dirty bit fires.
    //
    // Why: the command system needs to capture node identity in a way
    // that survives the node being destroyed and re-inserted (undo /
    // redo of structural ops). Raw SceneNode* dangles; iids don't. The
    // resolver `find_by_iid` is the migration's pump (s167 stages 0-6),
    // and several other walks across the codebase will eventually
    // retire onto this same index — find_warp_owner, parser forward-ref
    // fixup, selection-by-iid, cross-link follows.
    //
    // Walk coverage:
    //   • children                  — every container (Layer, Group,
    //                                 Compound, ClipGroup, etc.)
    //   • clip_shape                — ClipGroup's authoritative slot
    //   • blend_source_a / _b       — Blend's authoritative inputs
    //   • warp_source               — Warp's authoritative input
    //
    // NOT walked (deliberately): blend_cache, warp_glyph_cache,
    // warp_cache. Those are derived; iids inside them aren't stable
    // across cache rebuilds. A command that tried to capture a cached
    // node's iid would lose it on the next rebuild — by design, we
    // never let that capture happen.
    //
    // Freshness contract:
    //   1. Index starts dirty. First lookup walks the tree to build it.
    //   2. Pure data edits (EditPath, EditAppearance, TextEdit, …) do
    //      NOT change topology — the index stays valid across them.
    //   3. Any code path that destroys a SceneNode MUST call
    //      `invalidate_iid_index()` BEFORE the destruction. The single
    //      seam is `Canvas::scrub_node_refs`, which runs before every
    //      destructive op (s156). Stage 0 wires the call there; new
    //      destruction sites must extend that seam, not bypass it.
    //   4. Bulk loads (SvgParser) leave the index dirty by default —
    //      first lookup after load rebuilds.
    //
    // Failure mode if rule 3 is forgotten: stale pointer in the map
    // dereferenced by a caller. SAME failure mode as today's raw-
    // pointer-in-command. Rule 3 doesn't make things worse than the
    // status quo; rules 1-2 make the common case safe by construction.
    //
    // The index is `mutable` because find_by_iid is conceptually const
    // (no observable change to the document), and rebuild happens
    // through a const path. Standard idiom for lazy caches behind a
    // logically-const interface (cf. gradient_cache in SceneNode).
    mutable std::unordered_map<std::string, SceneNode*> m_iid_index;
    mutable bool m_iid_index_dirty = true;

    // Mark the iid index stale. Call before any code path that destroys
    // or replaces a SceneNode in this document's tree. The next
    // find_by_iid will rebuild from a fresh tree walk.
    //
    // Cheap (single bool flip) — safe to call defensively. Idempotent.
    void invalidate_iid_index() const { m_iid_index_dirty = true; }

    // Look up a live SceneNode in this document by internal_id.
    // Returns nullptr if iid is empty or no node matches.
    //
    // Rebuilds the index on first call after invalidation; O(1) on
    // subsequent calls within the same fresh window. Const because the
    // index is mutable; callers can use this from const contexts.
    //
    // Implementation lives in CurvzDocument.cpp to keep the recursive
    // walk out of the header.
    SceneNode* find_by_iid(const std::string& iid) const;

    int canvas_width()  const { return canvas.canvas_width();  }
    int canvas_height() const { return canvas.canvas_height(); }

    // Ruler origin — stored in user space (Y-up, bottom-left = 0,0 by default).
    double ruler_origin_x = 0.0;
    double ruler_origin_y = 0.0;

    // Active layer index (used by tools when placing new objects)
    int active_layer_index = 0;
    SceneNode* active_layer() {
        // Never return the guide layer as the active artwork layer
        if (active_layer_index >= 0 && active_layer_index < (int)layers.size()
            && layers[active_layer_index]->is_layer())
            return layers[active_layer_index].get();
        // Fallback: find first normal layer
        for (auto& l : layers)
            if (l->is_layer()) return l.get();
        return nullptr;
    }

    // Snap settings — per document
    SnapSettings snap;

    // Guide color — default cyan, user-changeable
    double guide_color_r = 0.0;
    double guide_color_g = 0.749;
    double guide_color_b = 1.0;

    // ── Editor presentation Motif (s183 m5a — dual-pair, mode-aware) ──
    //
    // History:
    //   * S98: per-doc single triple.
    //   * s116 m6: promoted to project-scope per-motif (every doc in a
    //     project shared one tone, swapped by Application Appearance).
    //   * s148 m1: demoted back to per-doc single-triple — users want
    //     per-icon choice. App Appearance swap no longer altered canvas
    //     colours; each doc carried "the colours its author picked".
    //   * s183 m5a (this commit): per-doc DUAL-PAIR. Each doc carries
    //     dark and light triples for artboard / workspace / creation.
    //     Application Appearance toggle (Dark/Light) now selects which
    //     pair the canvas paints from; the data is on the doc, the
    //     mode selects the slot. Theme apply writes BOTH pairs so a
    //     theme defines a doc's appearance in both modes.
    //
    // Why dual-pair: the theme editor (s183 m2-m4) exposes both pairs
    // for user editing. Without dual-pair storage on docs, applying a
    // theme could only write one pair (the current mode's), and
    // flipping the app's appearance afterwards left the doc looking
    // wrong in the other mode. s148's "single slot per doc" decision
    // pre-dated the theme editor's reality; s183 catches up.
    //
    // Defaults — match the s149 m1 MotifSettings defaults so a
    // default-constructed doc looks the same in either mode as it did
    // pre-s183 in the corresponding mode.
    //
    // Field access — every read site passes the active motif. The
    // accessor functions below dispatch; no caller indexes the raw
    // fields directly (the dark_* / light_* names are storage detail).
    // Setter methods accept a motif and write into that slot only.

    // Dark-mode triple
    double dark_artboard_bg_r  = 0.157;  // #282828
    double dark_artboard_bg_g  = 0.157;
    double dark_artboard_bg_b  = 0.157;
    double dark_workspace_bg_r = 0.09;   // #171717
    double dark_workspace_bg_g = 0.09;
    double dark_workspace_bg_b = 0.09;
    double dark_creation_color_r = 0.30; // light-blue, reads on dark artboard
    double dark_creation_color_g = 0.60;
    double dark_creation_color_b = 1.00;

    // Light-mode triple — defaults from MotifSettings's factory light.
    double light_artboard_bg_r  = 1.00;  // #FFFFFF
    double light_artboard_bg_g  = 1.00;
    double light_artboard_bg_b  = 1.00;
    double light_workspace_bg_r = 0.91;  // #E8E8E8
    double light_workspace_bg_g = 0.91;
    double light_workspace_bg_b = 0.91;
    double light_creation_color_r = 0.10; // dark-blue, reads on light artboard
    double light_creation_color_g = 0.35;
    double light_creation_color_b = 0.85;

    // Mode-aware accessors. Reads. Pass m_project->motif (or the
    // motif the caller is drawing for — e.g. the theme thumbnail
    // passes its own preview mode, decoupled from app motif).
    double artboard_bg_r(Motif m) const {
        return m == Motif::Light ? light_artboard_bg_r  : dark_artboard_bg_r;
    }
    double artboard_bg_g(Motif m) const {
        return m == Motif::Light ? light_artboard_bg_g  : dark_artboard_bg_g;
    }
    double artboard_bg_b(Motif m) const {
        return m == Motif::Light ? light_artboard_bg_b  : dark_artboard_bg_b;
    }
    double workspace_bg_r(Motif m) const {
        return m == Motif::Light ? light_workspace_bg_r : dark_workspace_bg_r;
    }
    double workspace_bg_g(Motif m) const {
        return m == Motif::Light ? light_workspace_bg_g : dark_workspace_bg_g;
    }
    double workspace_bg_b(Motif m) const {
        return m == Motif::Light ? light_workspace_bg_b : dark_workspace_bg_b;
    }
    double creation_color_r(Motif m) const {
        return m == Motif::Light ? light_creation_color_r : dark_creation_color_r;
    }
    double creation_color_g(Motif m) const {
        return m == Motif::Light ? light_creation_color_g : dark_creation_color_g;
    }
    double creation_color_b(Motif m) const {
        return m == Motif::Light ? light_creation_color_b : dark_creation_color_b;
    }

    // Mode-aware setters. Writes go to the named mode's slot only.
    void set_artboard_bg(Motif m, double r, double g, double b) {
        if (m == Motif::Light) {
            light_artboard_bg_r = r;
            light_artboard_bg_g = g;
            light_artboard_bg_b = b;
        } else {
            dark_artboard_bg_r = r;
            dark_artboard_bg_g = g;
            dark_artboard_bg_b = b;
        }
    }
    void set_workspace_bg(Motif m, double r, double g, double b) {
        if (m == Motif::Light) {
            light_workspace_bg_r = r;
            light_workspace_bg_g = g;
            light_workspace_bg_b = b;
        } else {
            dark_workspace_bg_r = r;
            dark_workspace_bg_g = g;
            dark_workspace_bg_b = b;
        }
    }
    void set_creation_color(Motif m, double r, double g, double b) {
        if (m == Motif::Light) {
            light_creation_color_r = r;
            light_creation_color_g = g;
            light_creation_color_b = b;
        } else {
            dark_creation_color_r = r;
            dark_creation_color_g = g;
            dark_creation_color_b = b;
        }
    }

    // Measurement settings
    // measure_save_to_layer: when true, every completed measurement (a pair
    //   of nodes locked in via shift+click or 2-node marquee) auto-appends
    //   to the MeasureLayer. When false, measurements are transient — they
    //   live as the live A/B picks on the ruler tool until the next click.
    // measure_destruct_after_copy: only meaningful when save_to_layer is OFF.
    //   When true, copying the live measurement also dismisses it from canvas
    //   (clears live A/B). Saved layer entries are NEVER auto-deleted by copy.
    bool measure_save_to_layer       = false;
    bool measure_destruct_after_copy = false;

    // Export metadata — used by the icon theme export pipeline
    std::string export_name;      // freedesktop icon name e.g. "edit-copy-symbolic"
    std::string export_category;  // subfolder e.g. "actions", "apps", "mimetypes", ""

    // Returns the single GuideLayer node, creating it at the top of the stack
    // if it doesn't exist yet. Always exactly one per document.
    SceneNode* ensure_guide_layer() {
        for (auto& l : layers)
            if (l->is_guide_layer()) return l.get();
        auto gl = std::make_unique<SceneNode>();
        gl->type = SceneNode::Type::GuideLayer;
        gl->name = "Guides";
        SceneNode* ptr = gl.get();
        // Under the z-order convention `layers[n-1]` = top, default the
        // guide layer to the very top by pushing at the back.
        layers.push_back(std::move(gl));
        return ptr;
    }

    // Returns the GuideLayer if present, nullptr if not.
    SceneNode* guide_layer() const {
        for (auto& l : layers)
            if (l->is_guide_layer()) return l.get();
        return nullptr;
    }

    // Returns the single RefLayer node, creating it just below the GuideLayer
    // (i.e. one stack position below guide's z-order) if it doesn't exist yet.
    SceneNode* ensure_ref_layer() {
        for (auto& l : layers)
            if (l->is_ref_layer()) return l.get();
        auto rl = std::make_unique<SceneNode>();
        rl->type  = SceneNode::Type::RefLayer;
        rl->name  = "References";
        rl->color = "#D91ABF";  // default magenta
        SceneNode* ptr = rl.get();
        // Under the new convention `layers[n-1]` = top. Stack order
        // top-down: Guide > Ref > Measure > Grid > Margin > Art. To insert
        // Ref one slot below Guide, find the Guide's index and insert at
        // `guide_idx` (which pushes Guide up by one).
        int ins = (int)layers.size();
        for (int i = 0; i < (int)layers.size(); ++i) {
            if (layers[i]->is_guide_layer()) { ins = i; break; }
        }
        layers.insert(layers.begin() + ins, std::move(rl));
        return ptr;
    }

    // Returns the RefLayer if present, nullptr if not.
    SceneNode* ref_layer() const {
        for (auto& l : layers)
            if (l->is_ref_layer()) return l.get();
        return nullptr;
    }

    // Returns the single MeasureLayer node, creating it just below the
    // GuideLayer and RefLayer (two stack positions below Guide) if not present.
    SceneNode* ensure_measure_layer() {
        for (auto& l : layers)
            if (l->is_measure_layer()) return l.get();
        auto ml = std::make_unique<SceneNode>();
        ml->type  = SceneNode::Type::MeasureLayer;
        ml->name  = "Measurements";
        ml->color = "#22C55E";  // green
        SceneNode* ptr = ml.get();
        // Stack order top-down: Guide > Ref > Measure > Grid > Margin > Art.
        // Find the lowest-indexed Guide/Ref slot — that's the insertion point,
        // which places Measure immediately below them in z-order.
        int ins = (int)layers.size();
        for (int i = 0; i < (int)layers.size(); ++i) {
            if (layers[i]->is_guide_layer() || layers[i]->is_ref_layer()) {
                ins = i;
                break;
            }
        }
        layers.insert(layers.begin() + ins, std::move(ml));
        return ptr;
    }

    // Returns the MeasureLayer if present, nullptr if not.
    SceneNode* measure_layer() const {
        for (auto& l : layers)
            if (l->is_measure_layer()) return l.get();
        return nullptr;
    }

    // Returns the single GridLayer node, creating it if needed. Inserted
    // just below the Guide/Ref/Measure trio in z-order (slot index below them).
    SceneNode* ensure_grid_layer() {
        for (auto& l : layers)
            if (l->is_grid_layer()) return l.get();
        auto gl = std::make_unique<SceneNode>();
        gl->type  = SceneNode::Type::GridLayer;
        gl->name  = "Grid";
        gl->color = "#8080CC";
        SceneNode* ptr = gl.get();
        // Find the lowest-index Guide/Ref/Measure — insert there so Grid
        // sits one z-slot below them (and above Margin + art).
        int ins = (int)layers.size();
        for (int i = 0; i < (int)layers.size(); ++i) {
            if (layers[i]->is_guide_layer() || layers[i]->is_ref_layer() ||
                layers[i]->is_measure_layer()) {
                ins = i;
                break;
            }
        }
        layers.insert(layers.begin() + ins, std::move(gl));
        return ptr;
    }

    SceneNode* grid_layer() const {
        for (auto& l : layers)
            if (l->is_grid_layer()) return l.get();
        return nullptr;
    }

    // Returns the single MarginLayer node, creating it if needed. Inserted
    // just below Guide/Ref/Measure/Grid (sits above art, below everything else).
    SceneNode* ensure_margin_layer() {
        for (auto& l : layers)
            if (l->is_margin_layer()) return l.get();
        auto ml = std::make_unique<SceneNode>();
        ml->type  = SceneNode::Type::MarginLayer;
        ml->name  = "Margins";
        ml->color = "#CC4040";
        SceneNode* ptr = ml.get();
        int ins = (int)layers.size();
        for (int i = 0; i < (int)layers.size(); ++i) {
            if (layers[i]->is_guide_layer() || layers[i]->is_ref_layer() ||
                layers[i]->is_measure_layer() || layers[i]->is_grid_layer()) {
                ins = i;
                break;
            }
        }
        layers.insert(layers.begin() + ins, std::move(ml));
        return ptr;
    }

    SceneNode* margin_layer() const {
        for (auto& l : layers)
            if (l->is_margin_layer()) return l.get();
        return nullptr;
    }

    // ── Name uniqueness funnel (S95 m1) ──────────────────────────────────
    //
    // Curvz emits SceneNode `name` fields into SVG `id` attributes
    // (Layer ids = layer.name, see SvgWriter line 837). When two nodes
    // share a name they collide at SVG-id level — and that's how
    // compounds end up referencing the wrong child via
    // data-curvz-child-ids, and how a load-save round-trip can produce
    // mysterious dangling references.
    //
    // The fix lives at the *creation* side: every new node grabs a name
    // that's already known unique. Callers ask the document for a fresh
    // default ("Layer 5", "Group 2", "Compound 3") via next_default_name,
    // or pass a user-typed name through uniquify_name to get a
    // collision-free version back. Either path the document hands back a
    // string that is guaranteed not to clash with any existing name in
    // the document at the moment of the call.
    //
    // Naming uses a flat, document-wide namespace because that's what
    // collides at SVG-emit time. A layer named "Background" and an
    // object named "Background" would both write id="Background" — so
    // we treat them as the same namespace for collision purposes.
    //
    // NameKind only governs the *prefix* of next_default_name's result
    // ("Layer 5" vs "Group 2"), not which scope the search runs in. The
    // search is always document-wide.
    enum class NameKind {
        Layer,
        Group,
        Compound,
        Object,
        Path,
        Text,
        Image,
        // Shape-tool kinds (S96 m3): each tool gets its own prefix so
        // users see "Rectangle 1", "Star 1", "Spiral 1" rather than
        // the generic "Object 1". Gap-fill is still document-wide.
        Rectangle,
        Ellipse,
        Star,
        Polygon,
        Spiral,
        Line,
        // Container/operation kinds (S96 m3): created by group/blend/
        // warp/step-repeat/clip/expand-stroke/offset flows.
        Blend,
        Warp,
        Steps,
        Clip,
        Offset,
        Outline,
        ExpandedStroke,
        // s177: refpts auto-named "Ref 1", "Ref 2", ... by the funnel.
        // Pre-s177 the Ref tool wrote "%.6f_%.6f" coordinate-as-name,
        // and dragging a refpt stomped its name with the new coords —
        // both violations of the "non-empty name + UUIDs/derived
        // strings never appear in user-facing UI" rule.
        Ref
    };

    // Walks every SceneNode in the document and inserts its non-empty
    // `name` into the provided set. Used by the uniqueness funnel.
    void collect_names(std::unordered_set<std::string>& out,
                       const SceneNode* exclude = nullptr) const {
        std::function<void(const SceneNode*)> visit =
            [&](const SceneNode* n) {
                if (!n || n == exclude) return;
                if (!n->name.empty()) out.insert(n->name);
                for (const auto& c : n->children) visit(c.get());
                if (n->clip_shape) visit(n->clip_shape.get());
            };
        for (const auto& l : layers) visit(l.get());
    }

    // Returns `proposed` if no other node in the document already owns
    // that name, else returns `proposed (2)`, `proposed (3)`, ... until
    // a free slot is found. `exclude` lets callers (e.g. a rename
    // command) tell the funnel "ignore this one node when looking for
    // collisions" — otherwise renaming a node to its current name
    // would falsely collide with itself.
    std::string uniquify_name(const std::string& proposed,
                              const SceneNode* exclude = nullptr) const {
        if (proposed.empty()) return proposed; // policy: empty stays empty
        std::unordered_set<std::string> taken;
        collect_names(taken, exclude);
        if (taken.find(proposed) == taken.end()) return proposed;
        for (int n = 2; n < 100000; ++n) {
            std::string candidate = proposed + " (" + std::to_string(n) + ")";
            if (taken.find(candidate) == taken.end()) return candidate;
        }
        // Practically unreachable; fall through to UUID-suffixed safety.
        return proposed + " (" + generate_internal_id().substr(0, 8) + ")";
    }

    // Returns a fresh default name for a new node of the given kind,
    // guaranteed unique within the document. Counter starts at 1 and
    // walks up until it finds a free slot — so on a freshly-loaded doc
    // with "Layer 1", "Layer 3" already present, this returns "Layer 2"
    // (lowest free), not "Layer 4". That matches user expectation that
    // "next layer" fills gaps before extending.
    std::string next_default_name(NameKind kind) const {
        const char* prefix = "";
        switch (kind) {
            case NameKind::Layer:          prefix = "Layer ";           break;
            case NameKind::Group:          prefix = "Group ";           break;
            case NameKind::Compound:       prefix = "Compound ";        break;
            case NameKind::Object:         prefix = "Object ";          break;
            case NameKind::Path:           prefix = "Path ";            break;
            case NameKind::Text:           prefix = "Text ";            break;
            case NameKind::Image:          prefix = "Image ";           break;
            case NameKind::Rectangle:      prefix = "Rectangle ";       break;
            case NameKind::Ellipse:        prefix = "Ellipse ";         break;
            case NameKind::Star:           prefix = "Star ";            break;
            case NameKind::Polygon:        prefix = "Polygon ";         break;
            case NameKind::Spiral:         prefix = "Spiral ";          break;
            case NameKind::Line:           prefix = "Line ";            break;
            case NameKind::Blend:          prefix = "Blend ";           break;
            case NameKind::Warp:           prefix = "Warp ";            break;
            case NameKind::Steps:          prefix = "Steps ";           break;
            case NameKind::Clip:           prefix = "Clip ";            break;
            case NameKind::Offset:         prefix = "Offset ";          break;
            case NameKind::Outline:        prefix = "Outline ";         break;
            case NameKind::ExpandedStroke: prefix = "Expanded Stroke "; break;
            case NameKind::Ref:            prefix = "Ref ";             break;
        }
        std::unordered_set<std::string> taken;
        collect_names(taken);
        for (int n = 1; n < 100000; ++n) {
            std::string candidate = std::string(prefix) + std::to_string(n);
            if (taken.find(candidate) == taken.end()) return candidate;
        }
        // Practically unreachable.
        return std::string(prefix) + generate_internal_id().substr(0, 8);
    }

    // Walk the entire document tree and rename every node whose name
    // collides with another's. First occurrence (in tree-walk order)
    // keeps the original name; subsequent ones get suffixed. Used at
    // load time to clean up files written by older Curvz builds (or
    // hand-edited files) that have name collisions.
    //
    // Returns the number of renames performed. Zero means the document
    // was already clean.
    int dedup_names() {
        std::unordered_set<std::string> seen;
        int renames = 0;
        std::function<void(SceneNode*)> visit = [&](SceneNode* n) {
            if (!n) return;
            if (!n->name.empty()) {
                if (seen.find(n->name) != seen.end()) {
                    // Find a free suffix
                    for (int k = 2; k < 100000; ++k) {
                        std::string cand = n->name + " (" + std::to_string(k) + ")";
                        if (seen.find(cand) == seen.end()) {
                            n->name = cand;
                            ++renames;
                            break;
                        }
                    }
                }
                seen.insert(n->name);
            }
            for (auto& c : n->children) visit(c.get());
            if (n->clip_shape) visit(n->clip_shape.get());
        };
        for (auto& l : layers) visit(l.get());
        return renames;
    }

    CurvzDocument() {
        canvas = CanvasModel::from_pixels(24, 24);
        // Under the z-order convention `layers[n-1]` = top, default stack
        // is: Layer 1 (art, bottom) → Guides (top). Order below mirrors that.
        // The constructor seeds an empty doc, so next_default_name returns
        // "Layer 1" trivially — but routing through the funnel keeps the
        // creation path consistent with every other layer-create site
        // (S95 m1).
        auto layer = std::make_unique<SceneNode>();
        layer->type = SceneNode::Type::Layer;
        layer->name = next_default_name(NameKind::Layer);
        layers.push_back(std::move(layer));
        auto gl = std::make_unique<SceneNode>();
        gl->type = SceneNode::Type::GuideLayer;
        gl->name = "Guides";
        layers.push_back(std::move(gl));
    }
};

} // namespace Curvz
