#pragma once
//
// Canvas_internal.hpp — implementation-shared declarations for the four
// Canvas*.cpp translation units. NOT part of the public Canvas surface;
// included only by Canvas.cpp, Canvas_input.cpp, Canvas_ops.cpp, and
// Canvas_draw.cpp.
//
// Created in s161 when Canvas.cpp was split into four TUs (CORE/INPUT/OPS/
// DRAW). File-scope helpers and statics that span multiple TUs live here;
// helpers used by exactly one TU stay `static` in their TU.
//
// See HANDOFF.md (s160→s161) for the full split design.

#include "Canvas.hpp"        // SceneNode, PathData, color::SwatchId, etc.
#include "MacroSystem.hpp"   // MacroManager, MacroStep
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Curvz {

// ── Cross-TU file-scope state ────────────────────────────────────────────────
//
// Inline variables (C++17) give us single-definition semantics across the
// four Canvas*.cpp TUs without needing a separate definition.
inline int s_next_id = 1;
inline int s_next_id_pen = 100;  // separate id range for pen paths
inline std::string s_last_iid;   // set by next_id(), grabbed by caller via last_iid()

// ── Cross-TU helper structs ─────────────────────────────────────────────────

// Used by collect_selection_entries() (defined in Canvas.cpp / CORE) and
// by callers spanning CORE+INPUT+OPS.
struct SelectionEntry {
  SceneNode *parent;
  SceneNode *node;
  int        index;  // index of node in parent->children
};

// Image metadata reader return type (s125 m1c). Used by both the constructor's
// Image Info dialog (CORE) and import_image_to_canvas (OPS).
struct ImageMeta {
  int width = 0;        // pixels
  int height = 0;       // pixels
  std::string depth;    // human-readable colour depth, e.g. "8-bit RGBA",
                        // "16-bit RGB", "8-bit Indexed". Empty when unknown.
  std::string format;   // "PNG", "JPEG", "GIF", "WebP", or "" if unrecognised
  uint64_t file_size = 0;  // bytes, 0 if unreadable
  bool valid = false;   // true iff width/height were read successfully
};

// ── Cross-TU inline helpers ─────────────────────────────────────────────────
// Tiny, hot-path; making them inline avoids cross-TU call overhead and keeps
// the body visible in every TU.

inline std::string next_id() {
  s_last_iid = generate_internal_id();
  return "obj" + std::to_string(s_next_id++);
}

inline const std::string &last_iid() { return s_last_iid; }

inline void record_step_if_recording(MacroStep step) {
  if (MacroManager::instance().is_recording())
    MacroManager::instance().record_step(std::move(step));
}

// ── Cross-TU non-inline helper declarations ─────────────────────────────────
// Definitions live in the TU named in the comment.

// Defined in Canvas.cpp (CORE):
SceneNode *find_parent(CurvzDocument *doc, SceneNode *target, int *out_idx);
void freshen_ids(SceneNode *node, CurvzDocument *doc, int &counter);
void collect_paths(SceneNode *obj, std::vector<SceneNode *> &out);
std::vector<SelectionEntry>
collect_selection_entries(CurvzDocument *doc,
                          const std::vector<SceneNode *> &selection);
void collect_text_image_leaves(SceneNode *obj, std::vector<SceneNode *> &out);
bool subtree_path_bbox(const SceneNode *n, double &bx, double &by,
                       double &bw, double &bh);
void unbind_swatch_walk(SceneNode *node, const color::SwatchId &removed_id);
void selection_handle_positions(double sx1, double sy1, double sx2, double sy2,
                                double hx[8], double hy[8]);

// Defined in Canvas_ops.cpp (OPS):
void rebuild_blend_cache(SceneNode *obj);
ImageMeta read_image_meta(const std::string &path);
std::string png_color_type_str(uint8_t ct);
std::string format_file_size(uint64_t bytes);

// s181 m2: compound winding normalisation. CCW = outer (union role), CW =
// hole (subtract role). Even-odd render is invariant to per-subpath winding;
// these are read by boolean ops to feed Clipper2 well-formed input. Make
// Compound calls normalize_compound_winding() to set the convention; the
// SvgParser load path calls _recursive on doc roots; the inspector switch
// calls reverse_compound_all_children() to flip every child.
void normalize_compound_winding(SceneNode *compound);
void normalize_compound_winding_recursive(SceneNode *n);
void reverse_compound_all_children(SceneNode *compound);

// Defined in Canvas_draw.cpp (DRAW):
void foreach_corner_node(CurvzDocument *doc,
                         std::function<bool(SceneNode *, int)> fn);

// s161 split note: FTOutlineCtx + s_ft_callbacks are NOT in this header.
// The original split design proposed they live here (with the callback table
// defined in DRAW), but they are referenced exclusively from text_to_paths_op()
// in Canvas_ops.cpp. Keeping them as `static` in Canvas_ops.cpp matches actual
// usage and avoids a needless cross-TU declaration.

}  // namespace Curvz
