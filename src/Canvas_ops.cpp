#include "Canvas.hpp"
#include "CommandHistory.hpp"
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"  // s116 m6 — m_project field reads workspace appearance
#include "CurvzSpinButton.hpp"
#include "MacroSystem.hpp"
#include "SvgParser.hpp"
#include "curvz_utils.hpp"  // S97 m2 — box_blur_argb32 for drop-shadow render
#include "color/SwatchLibrary.hpp"  // set_swatch_library + apply_swatch_to_selection
#include "color/FillStyleInterop.hpp"  // to_fillstyle — live-recolour walk (s70 M3)
#include "style/StyleInterop.hpp"  // mutate_appearance funnel for user-driven fill/stroke writes
#include "style/StyleLibrary.hpp"  // set_style_library + signal_style_changed (S78 m3d)
#include <filesystem>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/clipboard.h>
#include <gdkmm/contentprovider.h>
#include <gdkmm/pixbuf.h>
#include <gdkmm/rectangle.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/grid.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/window.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-font.h>
#include <sstream>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>  // s125 m1c — uint8_t for PNG IHDR parser
#include <cstring>  // s125 m1c — std::memcmp for PNG signature check
#include <ctime>    // s125 m1c — strftime/localtime for mtime in info dialog
#include <fstream>  // s125 m1c — std::ifstream for IHDR parser
#include <functional>
#include <glib.h> // g_uuid_string_random via generate_internal_id()
#include <glibmm/main.h>
#include <gtkmm/gestureclick.h>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Canvas_internal.hpp"

namespace Curvz {


// PNG IHDR colour-type byte → human-readable channel layout.
// PNG spec table 11.1: 0=Gray, 2=RGB, 3=Indexed, 4=Gray+A, 6=RGBA.
std::string png_color_type_str(uint8_t ct) {
  switch (ct) {
    case 0: return "Gray";
    case 2: return "RGB";
    case 3: return "Indexed";
    case 4: return "Gray+A";
    case 6: return "RGBA";
    default: return "?";
  }
}

ImageMeta read_image_meta(const std::string &path) {
  ImageMeta m;

  // File size — cheap, do this regardless of format.
  std::error_code ec;
  auto fsz = std::filesystem::file_size(path, ec);
  if (!ec)
    m.file_size = static_cast<uint64_t>(fsz);

  // Format from extension. Used for both the format string and to gate the
  // PNG IHDR fast-path.
  std::string ext;
  auto dot = path.rfind('.');
  if (dot != std::string::npos) {
    ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  }
  if (ext == "png") m.format = "PNG";
  else if (ext == "jpg" || ext == "jpeg") m.format = "JPEG";
  else if (ext == "gif") m.format = "GIF";
  else if (ext == "webp") m.format = "WebP";

  // PNG fast-path: read IHDR for accurate colour depth even for 16-bit /
  // indexed / grayscale variants that Pixbuf would normalise away.
  if (ext == "png") {
    std::ifstream f(path, std::ios::binary);
    if (f) {
      // PNG signature (8 bytes) + IHDR length (4) + "IHDR" (4) + width (4)
      // + height (4) + bit depth (1) + colour type (1) = 26 bytes minimum.
      uint8_t buf[26];
      f.read(reinterpret_cast<char *>(buf), sizeof(buf));
      static const uint8_t png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
      if (f.gcount() == sizeof(buf) &&
          std::memcmp(buf, png_sig, 8) == 0 &&
          buf[12] == 'I' && buf[13] == 'H' &&
          buf[14] == 'D' && buf[15] == 'R') {
        // Big-endian uint32. Cast each byte to uint32_t before shifting so
        // we don't promote to int and risk shifting into the sign bit on
        // pathologically-large (≥2^30 px) PNGs.
        auto be32 = [&](int off) -> uint32_t {
          return (uint32_t(buf[off]) << 24) | (uint32_t(buf[off + 1]) << 16) |
                 (uint32_t(buf[off + 2]) << 8) | uint32_t(buf[off + 3]);
        };
        m.width = static_cast<int>(be32(16));
        m.height = static_cast<int>(be32(20));
        uint8_t bit_depth = buf[24];
        uint8_t color_type = buf[25];
        m.depth = std::to_string(bit_depth) + "-bit " +
                  png_color_type_str(color_type);
        m.valid = (m.width > 0 && m.height > 0);
        return m;
      }
      // Header malformed — fall through to Pixbuf.
    }
  }

  // Generic path: Gdk::Pixbuf reads everything we care about. bits_per_sample
  // × n_channels gives a reasonable depth string post-normalisation.
  try {
    auto pb = Gdk::Pixbuf::create_from_file(path);
    if (pb) {
      m.width = pb->get_width();
      m.height = pb->get_height();
      int bps = pb->get_bits_per_sample();
      int nch = pb->get_n_channels();
      bool has_alpha = pb->get_has_alpha();
      // Build a label like "8-bit RGB" / "8-bit RGBA" / "8-bit Gray".
      // Pixbuf reports bits per *sample* (per channel), not total. nch=3 RGB
      // sans alpha, nch=4 RGB with alpha, nch=1 grayscale, nch=2 gray+alpha.
      const char *layout = "?";
      if (nch == 1) layout = "Gray";
      else if (nch == 2) layout = "Gray+A";
      else if (nch == 3) layout = "RGB";
      else if (nch == 4) layout = has_alpha ? "RGBA" : "RGBX";
      m.depth = std::to_string(bps) + "-bit " + layout;
      m.valid = (m.width > 0 && m.height > 0);
    }
  } catch (...) {
    // m.valid stays false
  }

  return m;
}

// Pretty-print bytes — same scale used in the Image Info dialog.
std::string format_file_size(uint64_t bytes) {
  if (bytes == 0) return "unknown";
  if (bytes >= 1024 * 1024)
    return std::to_string(bytes / (1024 * 1024)) + " MB";
  if (bytes >= 1024)
    return std::to_string(bytes / 1024) + " KB";
  return std::to_string(bytes) + " B";
}

// ── rebuild_blend_cache
// ─────────────────────────────────────────────────────
// Regenerate obj->blend_cache from blend_source_a/b for a Blend node.
// Preconditions (enforced by make_blend at construction, locked
// thereafter): A and B are Paths with equal node counts and equal
// closed flag. If preconditions don't hold at call time, we clear the
// cache and LOG_WARN — render will paint A and B only, which is the
// least-surprising fallback.
//
// Interpolation math (linear, all fields):
//   For N = blend_steps intermediate paths, step i in 1..N uses
//     t = i / (N + 1)   // strictly between A and B, exclusive
//   For each node index k, lerp anchor, both handles, and keep A's
//   type (node type doesn't meaningfully interpolate — Smooth vs Cusp
//   is a discrete property).
//   closed = A.closed (== B.closed by precondition).
//
// Style interpolation (step gets own fill/stroke/opacity):
//   fill.type:   take A's if both are Solid/CurrentColor, otherwise A's
//                (None stays None). Colour interpolated in sRGB.
//   stroke.paint: same rule.
//   stroke.width: lerp(A, B) unless blend_stroke_w_user_set, in which
//                 case lerp(blend_stroke_w_start, blend_stroke_w_end).
//   opacity:     lerp(A.opacity, B.opacity).
//
// Cache entries are full SceneNodes of Type::Path, minted with fresh
// ids (each frame, that's fine — ids are render-only for cache; they
// never hit SVG since the cache isn't persisted as structure).
void rebuild_blend_cache(SceneNode *obj) {
  if (!obj || obj->type != SceneNode::Type::Blend)
    return;
  obj->blend_cache.clear();
  obj->blend_cache_dirty = false;

  SceneNode *A = obj->blend_source_a.get();
  SceneNode *B = obj->blend_source_b.get();
  if (!A || !B || A->type != SceneNode::Type::Path ||
      B->type != SceneNode::Type::Path || !A->path || !B->path) {
    LOG_WARN("Canvas::rebuild_blend_cache: A or B missing/not-Path — "
             "cache cleared");
    return;
  }
  const auto &na = A->path->nodes;
  const auto &nb = B->path->nodes;
  if (na.size() != nb.size() || A->path->closed != B->path->closed) {
    LOG_WARN("Canvas::rebuild_blend_cache: node-count or closed mismatch "
             "(A={} B={} closedA={} closedB={}) — cache cleared",
             na.size(), nb.size(), A->path->closed, B->path->closed);
    return;
  }

  int N = std::clamp(obj->blend_steps, 1, 50);
  auto lerp = [](double x, double y, double t) { return x + (y - x) * t; };

  for (int i = 1; i <= N; ++i) {
    double t = double(i) / double(N + 1);

    auto step = std::make_unique<SceneNode>();
    step->type = SceneNode::Type::Path;
    step->path = std::make_unique<PathData>();
    step->path->closed = A->path->closed;
    step->path->nodes.reserve(na.size());
    for (size_t k = 0; k < na.size(); ++k) {
      BezierNode n;
      n.x = lerp(na[k].x, nb[k].x, t);
      n.y = lerp(na[k].y, nb[k].y, t);
      n.cx1 = lerp(na[k].cx1, nb[k].cx1, t);
      n.cy1 = lerp(na[k].cy1, nb[k].cy1, t);
      n.cx2 = lerp(na[k].cx2, nb[k].cx2, t);
      n.cy2 = lerp(na[k].cy2, nb[k].cy2, t);
      n.type = na[k].type;
      step->path->nodes.push_back(n);
    }

    // Fill: inherit A's Type (None/Solid/CurrentColor). Only interpolate
    // rgba when BOTH ends are Solid — CurrentColor ignores rgba at paint
    // time (apply_fill hardcodes gray), so lerping into CurrentColor's
    // default-zero rgba produces invisible steps. When a user hasn't
    // touched the paint, both sides are CurrentColor; intermediates
    // inherit that — same-colored gradient, visually consistent.
    step->fill.type = A->fill.type;
    if (A->fill.type == FillStyle::Type::Solid &&
        B->fill.type == FillStyle::Type::Solid) {
      step->fill.r = lerp(A->fill.r, B->fill.r, t);
      step->fill.g = lerp(A->fill.g, B->fill.g, t);
      step->fill.b = lerp(A->fill.b, B->fill.b, t);
      step->fill.a = lerp(A->fill.a, B->fill.a, t);
    } else {
      step->fill = A->fill;
    }

    // Stroke paint: same rule. Lerp only when BOTH sides are Solid.
    step->stroke.paint.type = A->stroke.paint.type;
    if (A->stroke.paint.type == FillStyle::Type::Solid &&
        B->stroke.paint.type == FillStyle::Type::Solid) {
      step->stroke.paint.r = lerp(A->stroke.paint.r, B->stroke.paint.r, t);
      step->stroke.paint.g = lerp(A->stroke.paint.g, B->stroke.paint.g, t);
      step->stroke.paint.b = lerp(A->stroke.paint.b, B->stroke.paint.b, t);
      step->stroke.paint.a = lerp(A->stroke.paint.a, B->stroke.paint.a, t);
    } else {
      step->stroke.paint = A->stroke.paint;
    }
    // Stroke width — user-override wins if set.
    if (obj->blend_stroke_w_user_set) {
      step->stroke.width =
          lerp(obj->blend_stroke_w_start, obj->blend_stroke_w_end, t);
    } else {
      step->stroke.width = lerp(A->stroke.width, B->stroke.width, t);
    }
    // Stroke adornments not interpolated in v1 — take A's.
    step->stroke.cap = A->stroke.cap;
    step->stroke.join = A->stroke.join;
    step->stroke.miter = A->stroke.miter;
    step->stroke.opacity = lerp(A->stroke.opacity, B->stroke.opacity, t);

    step->opacity = lerp(A->opacity, B->opacity, t);
    step->visible = true;
    step->locked = true; // cache is non-editable
    step->name = "blend-step-" + std::to_string(i);

    obj->blend_cache.push_back(std::move(step));
  }
}

// ─── s181 m2 / m4: compound winding normalisation ─────────────────────────────
// Convention: each child of a Compound carries a directional role.
//   CCW (signed_area < 0 in Y-down doc-space) = OUTER role (union)
//   CW  (signed_area > 0)                     = HOLE  role (subtract)
//
// Make-Compound rule (m4 update): stack-order alternating starting CCW.
//   children[0] CCW, [1] CW, [2] CCW, [3] CW, ...
// Stack-order is what the user sees in the layers panel — putting the
// desired outer at the bottom matches normal vector-editor convention.
//
// Cairo and SVG render Compound paths via the even-odd fill rule, which is
// indifferent to per-subpath winding — so this normalisation is invisible
// to rendering. Its purpose is to drive boolean ops:
//   (1) Make-Compound assigns initial roles by stack-order.
//   (2) The user reorders children in the layers panel, runs Reverse
//       on a single child via Node tool inspector, or clicks the
//       compound's ↻↺ switch (m3) to override.
//   (3) Boolean ops (m4) read winding per child to dispatch the
//       per-child algorithm: outer ∪/⊖/∩ Y, hole ⊖/∪/∩ Y.
//
// Migration: parse_svg calls normalize_compound_winding_recursive() on every
// Compound after load so legacy documents pick up the convention silently.

// Reverse a PathData in place. Geometry-preserving: handle order on each
// node swaps, then nodes reverse. Pumps via BezierPath round-trip because
// BezierPath::reverse already encodes the cx1↔cx2 swap.
static void reverse_path_data(PathData &pd) {
  if (pd.nodes.size() < 2) return;
  BezierPath bp = BezierPath::from_path_data(pd);
  bp.reverse();
  pd = bp.to_path_data();
}

// |signed_area| via shoelace on anchors. Sign indicates winding direction in
// doc Y-down space: positive = CW, negative = CCW.
static double child_signed_area(const SceneNode *n) {
  if (!n || !n->path || n->path->nodes.size() < 3) return 0.0;
  return BezierPath::from_path_data(*n->path).signed_area();
}

// Force a child into the requested winding direction. desired_ccw=true means
// "make this child CCW (outer role)"; false means "make this child CW (hole)".
// Reads sign of signed_area to decide whether to flip.
static void force_child_winding(SceneNode *child, bool desired_ccw) {
  if (!child || !child->path || child->path->nodes.size() < 3) return;
  double sa = child_signed_area(child);
  bool is_ccw = (sa < 0.0);  // doc Y-down: negative area = CCW on screen
  if (is_ccw != desired_ccw) {
    reverse_path_data(*child->path);
  }
}

// Public: walk a Compound's children, set largest-bbox child CCW (outer),
// the rest CW (holes). First-largest wins ties (strict >). No-op if
// not a Compound or has fewer than 2 children. Idempotent — safe to
// call multiple times on the same compound.
//
// s181 m2: rule shape — directional roles per child.
// s181 m4: rule changed from "largest-bbox child CCW" to "stack-order
// alternating starting CCW."
// s181 m4 fix: convention flipped — bottom-of-stack (highest index in
// children vector = visually-bottom in layers panel) is the outer (CCW).
// Holes alternate going up.
//
// Stack-order is what the user sees in the layers panel — putting the
// desired outer at the bottom matches normal vector-editor mental
// models. children vector order matches z-order: index 0 is top
// (front-most), index N-1 is bottom (back-most). Outer at the bottom
// means children[N-1] = CCW.
//
// Index parity (counted from the bottom: N-1, N-2, ..., 0) and winding
// both encode role, equivalent by construction. Boolean ops read winding
// at runtime so per-child Reverse and the compound switch both work as
// overrides.
void normalize_compound_winding(SceneNode *compound) {
  if (!compound || compound->type != SceneNode::Type::Compound) return;
  if (compound->children.size() < 2) return;

  // Apply alternating convention by stack-order index, counted from the
  // bottom of the stack (visually-bottom = highest index).
  // Bottom child (children[N-1]) → CCW (outer/union role).
  // Next up (children[N-2])      → CW  (hole/subtract role).
  // Then alternating upward.
  const size_t n = compound->children.size();
  for (size_t i = 0; i < n; ++i) {
    // distance_from_bottom = (N-1) - i
    size_t dfb = (n - 1) - i;
    bool desired_ccw = ((dfb % 2) == 0);
    force_child_winding(compound->children[i].get(), desired_ccw);
  }
}

// Recursive walker — applies normalize_compound_winding to every Compound in
// a SceneNode subtree. Used by the doc-load migration in SvgParser.
void normalize_compound_winding_recursive(SceneNode *n) {
  if (!n) return;
  for (auto &ch : n->children) normalize_compound_winding_recursive(ch.get());
  if (n->type == SceneNode::Type::Compound) normalize_compound_winding(n);
}

// Reverse winding of every child in a Compound. Used by the inspector
// switch — flips every per-child role at once. Idempotent in pairs (two
// applications restore the original state).
void reverse_compound_all_children(SceneNode *compound) {
  if (!compound || compound->type != SceneNode::Type::Compound) return;
  for (auto &ch : compound->children) {
    if (ch && ch->path && ch->path->nodes.size() >= 3) {
      reverse_path_data(*ch->path);
    }
  }
}

void Canvas::make_compound_path() {
  if (!m_doc || m_selection.size() < 2)
    return;

  // All selected objects must be paths or compounds
  for (SceneNode *obj : m_selection)
    if (obj->type != SceneNode::Type::Path &&
        obj->type != SceneNode::Type::Compound)
      return;

  // Find parent — all selected must share the same parent
  int insert_idx = 0;
  SceneNode *parent = find_parent(m_doc, m_selection.front(), &insert_idx);
  if (!parent)
    return;

  // Verify all selected share the same parent
  for (SceneNode *obj : m_selection) {
    int idx = 0;
    SceneNode *p = find_parent(m_doc, obj, &idx);
    if (p != parent)
      return;
  }

  // ── Phase 1: expand any Compound members in-place ─────────────────────
  // For each selected Compound, pull its children out into the parent at
  // the compound's position and remove the compound. Collect all resulting
  // path pointers into an expanded selection in parent z-order.
  std::set<SceneNode *> sel_set(m_selection.begin(), m_selection.end());
  for (auto it = parent->children.begin(); it != parent->children.end();) {
    SceneNode *obj = it->get();
    if (!sel_set.count(obj) || obj->type != SceneNode::Type::Compound) {
      ++it;
      continue;
    }
    // Replace the compound with its children at the same position
    int pos = (int)(it - parent->children.begin());
    // Remove compound from sel_set — its children will be selected instead
    sel_set.erase(obj);
    std::vector<std::unique_ptr<SceneNode>> released;
    for (auto &child : obj->children)
      released.push_back(std::move(child));
    it = parent->children.erase(it); // removes compound, it now points to next
    // Insert children at pos (in order)
    for (int ci = 0; ci < (int)released.size(); ++ci) {
      sel_set.insert(released[ci].get());
      parent->children.insert(parent->children.begin() + pos + ci,
                              std::move(released[ci]));
    }
    // Advance past inserted children
    it = parent->children.begin() + pos + (int)released.size();
  }

  // ── Phase 2: find topmost position among expanded selection ───────────
  insert_idx = (int)parent->children.size();
  for (int i = 0; i < (int)parent->children.size(); ++i) {
    if (sel_set.count(parent->children[i].get()))
      insert_idx = std::min(insert_idx, i);
  }

  // ── Phase 3: build new Compound, moving expanded paths in z-order ─────
  auto compound = std::make_unique<SceneNode>();
  compound->type = SceneNode::Type::Compound;
  compound->internal_id = generate_internal_id();
  compound->name = m_doc->next_default_name(CurvzDocument::NameKind::Compound);

  size_t src_count = sel_set.size();
  for (auto it = parent->children.begin(); it != parent->children.end();) {
    if (sel_set.count(it->get())) {
      compound->children.push_back(std::move(*it));
      it = parent->children.erase(it);
    } else {
      ++it;
    }
  }

  // S58g: Seed the Compound's own fill/stroke/opacity from the first moved
  // child so the S58d/S58g "Compound owns its paint" rule doesn't silently
  // drop the user's colour at make-compound time. Without this, a Compound
  // created from two red paths would render as the default CurrentColor
  // because compound->fill was never initialised.
  if (!compound->children.empty() && compound->children.front()) {
    const SceneNode &first = *compound->children.front();
    compound->fill = first.fill;
    compound->stroke = first.stroke;
    compound->opacity = first.opacity;
  }

  insert_idx = std::min(insert_idx, (int)parent->children.size());
  SceneNode *cptr = compound.get();
  parent->children.insert(parent->children.begin() + insert_idx,
                          std::move(compound));

  m_selected = cptr;
  m_selection = {cptr};
  m_selected_node = -1;
  m_node_selection.clear();

  notify_object_selection_changed();
  notify_node_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: made compound from {} paths", src_count);
}

void Canvas::split_compound_path() {
  // Splits every Compound in the selection, not just the focused one.
  // Pre-s127 behaviour matched the singular menu language ("Split Compound
  // Path") but multi-selecting three compounds and hitting split would
  // only release the first — surprising on a multi-selection.
  //
  // Non-compound members of the selection are ignored (a mixed selection
  // is allowed; compounds split, paths stay put). After split, the new
  // selection is the union of all released children. No-op if the
  // selection contains no compounds.
  //
  // Note: like make_compound_path, this does not push to m_history. Both
  // ops are pending undo support — parity preserved here so a future fix
  // wires them together.
  if (!m_doc || m_selection.empty()) return;

  // Collect the compounds to split. Working off a stable copy because
  // we'll be mutating m_selection as the splits commit.
  std::vector<SceneNode *> targets;
  targets.reserve(m_selection.size());
  for (SceneNode *obj : m_selection) {
    if (obj && obj->type == SceneNode::Type::Compound && !obj->children.empty())
      targets.push_back(obj);
  }
  if (targets.empty()) return;

  std::vector<SceneNode *> new_sel;
  size_t total_released = 0;
  size_t compounds_split = 0;

  for (SceneNode *target : targets) {
    int insert_idx = 0;
    SceneNode *parent = find_parent(m_doc, target, &insert_idx);
    if (!parent) continue;

    // Pull children out of the compound, then erase the compound from
    // parent. Insert children at the compound's z-position, top-to-bottom
    // order preserved (matches the within-layer descending convention —
    // children[0] = top).
    std::vector<std::unique_ptr<SceneNode>> released;
    released.reserve(target->children.size());
    for (auto &child : target->children)
      released.push_back(std::move(child));

    parent->children.erase(parent->children.begin() + insert_idx);

    int idx = insert_idx;
    for (auto &child : released) {
      SceneNode *ptr = child.get();
      parent->children.insert(parent->children.begin() + idx, std::move(child));
      new_sel.push_back(ptr);
      ++idx;
    }
    total_released += released.size();
    ++compounds_split;
  }

  // Replace selection with the released children. If nothing was actually
  // split (e.g. all targets had unfindable parents — shouldn't happen in
  // practice), bail without mutating selection.
  if (new_sel.empty()) return;

  m_selection      = new_sel;
  m_selected       = new_sel.front();
  m_selected_node  = -1;
  m_node_selection.clear();

  notify_object_selection_changed();
  notify_node_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: split {} compound{} → {} paths",
           compounds_split, compounds_split == 1 ? "" : "s", total_released);
}

// ── Clipping paths
// ────────────────────────────────────────────────────────────
//
// Workflow:
//   1. User selects N objects of any type.
//   2. Object ▸ Clip menu calls make_clip_group(): arms pick mode,
//      snapshots the selection. The selection itself stays visible so
//      the user can see what they're about to clip.
//   3. User clicks on a Path or Compound in the canvas.
//      on_select_begin detects m_clip_pick_armed at its top and calls
//      finish_clip_pick(clicked_shape). Clicking anything else (empty
//      canvas, non-path, group, etc.) cancels and disarms.
//   4. finish_clip_pick extracts the stashed selection + the clicked
//      shape from their parent(s), wraps them in a new ClipGroup, and
//      inserts it at the topmost stashed-selection z-position of the
//      common parent.
//
// Constraints enforced:
//   - All stashed selection members must share one parent. The clip
//     shape must live in that same parent. (Simplest contract; keeps
//     the extraction unambiguous. Matches make_compound_path rules.)
//   - Clip shape must be a Path or Compound (not a Group, not a Text,
//     not another ClipGroup).
//   - Clip shape cannot itself be in the stashed selection.
//
// release_clip_group does the inverse: dissolves the ClipGroup, returns
// clip_shape and children as normal siblings in parent z-order
// (clip_shape on top — matches the Layer-panel convention where the
// clip shape is rendered above its children in outline mode).
void Canvas::make_clip_group() {
  if (!m_doc || m_selection.empty()) {
    LOG_INFO("Canvas: make_clip_group ignored — no selection");
    return;
  }
  // Reject if the selection contains anything that can't sensibly live
  // inside a ClipGroup. We allow Path, Compound, Group, Text, Image.
  // Refs/guides/measures/layers — no.
  for (SceneNode *obj : m_selection) {
    if (obj->is_ref() || obj->is_guide() || obj->is_measurement() ||
        obj->is_layer() || obj->is_special_layer()) {
      LOG_INFO("Canvas: make_clip_group rejected — selection contains a "
               "non-clippable node");
      return;
    }
  }
  m_clip_pick_armed = true;
  m_clip_pick_selection = m_selection;
  LOG_INFO("Canvas: clip-pick armed, selection={}",
           m_clip_pick_selection.size());
  // Status bar messaging could hook off is_clip_pick_armed() — not wired
  // here to keep Stage 2 diff small. StatusBar poll happens on cursor
  // move anyway.
  queue_draw();
}

// Called from on_select_begin when m_clip_pick_armed is true.
// Returns true if the click was consumed (armed state always cleared
// either way on return).
bool Canvas::finish_clip_pick(SceneNode *clicked) {
  // Disarm no matter what — a single arm = a single click opportunity.
  bool armed_was_true = m_clip_pick_armed;
  m_clip_pick_armed = false;
  std::vector<SceneNode *> stash;
  stash.swap(m_clip_pick_selection);

  if (!armed_was_true)
    return false;

  // Clicked nothing, or clicked an invalid shape → cancel silently.
  if (!clicked) {
    LOG_INFO("Canvas: clip-pick cancelled — empty click");
    queue_draw();
    return true; // still consume the click so selection doesn't reset
  }
  if (clicked->type != SceneNode::Type::Path &&
      clicked->type != SceneNode::Type::Compound) {
    LOG_INFO("Canvas: clip-pick cancelled — clicked node is not a Path or "
             "Compound");
    queue_draw();
    return true;
  }
  // Clip shape can't be in the stashed selection.
  if (std::find(stash.begin(), stash.end(), clicked) != stash.end()) {
    LOG_INFO("Canvas: clip-pick cancelled — clicked shape is part of the "
             "selection");
    queue_draw();
    return true;
  }

  // Find common parent of stashed selection.
  int dummy = 0;
  SceneNode *parent = find_parent(m_doc, stash.front(), &dummy);
  if (!parent) {
    LOG_INFO("Canvas: clip-pick aborted — selection has no parent");
    queue_draw();
    return true;
  }
  for (SceneNode *obj : stash) {
    int d = 0;
    if (find_parent(m_doc, obj, &d) != parent) {
      LOG_INFO("Canvas: clip-pick aborted — selection members across "
               "different parents");
      queue_draw();
      return true;
    }
  }
  // Clip shape must be in the same parent.
  int clip_idx_in_parent = 0;
  if (find_parent(m_doc, clicked, &clip_idx_in_parent) != parent) {
    LOG_INFO("Canvas: clip-pick aborted — clip shape has a different parent "
             "than the selection");
    queue_draw();
    return true;
  }

  // Compute insert index = topmost z-position among stash + clicked in parent.
  std::set<SceneNode *> extract_set(stash.begin(), stash.end());
  extract_set.insert(clicked);
  int insert_idx = (int)parent->children.size();
  for (int i = 0; i < (int)parent->children.size(); ++i) {
    if (extract_set.count(parent->children[i].get()))
      insert_idx = std::min(insert_idx, i);
  }

  // Build new ClipGroup.
  auto cg = std::make_unique<SceneNode>();
  cg->type = SceneNode::Type::ClipGroup;
  cg->internal_id = generate_internal_id();
  cg->name = m_doc->next_default_name(CurvzDocument::NameKind::Clip);
  cg->clip_id = "cp_" + cg->internal_id; // SVG defs id, stable

  // Extract the clip shape first, then the stashed children, preserving
  // their original z-order. Iterate once through parent->children.
  std::unique_ptr<SceneNode> clip_shape_owned;
  std::vector<std::unique_ptr<SceneNode>> children_owned;
  for (auto it = parent->children.begin(); it != parent->children.end();) {
    SceneNode *n = it->get();
    if (n == clicked) {
      clip_shape_owned = std::move(*it);
      it = parent->children.erase(it);
    } else if (extract_set.count(n)) {
      children_owned.push_back(std::move(*it));
      it = parent->children.erase(it);
    } else {
      ++it;
    }
  }

  cg->clip_shape = std::move(clip_shape_owned);
  cg->children = std::move(children_owned);

  insert_idx = std::min(insert_idx, (int)parent->children.size());
  SceneNode *cg_ptr = cg.get();
  parent->children.insert(parent->children.begin() + insert_idx, std::move(cg));

  m_selected = cg_ptr;
  m_selection = {cg_ptr};
  m_selected_node = -1;
  m_node_selection.clear();

  notify_object_selection_changed();
  notify_node_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: clip-pick done → ClipGroup '{}' with {} children",
           cg_ptr->name, (int)cg_ptr->children.size());
  return true;
}

void Canvas::release_clip_group() {
  if (!m_doc || !m_selected || !m_selected->is_clip_group()) {
    LOG_INFO("Canvas: release_clip_group ignored — selection is not a "
             "ClipGroup");
    return;
  }

  int cg_idx = 0;
  SceneNode *parent = find_parent(m_doc, m_selected, &cg_idx);
  if (!parent)
    return;

  SceneNode *cg = m_selected;

  // Collect releasable children: clip_shape first (goes on top), then
  // the clipped children in their existing order.
  std::vector<std::unique_ptr<SceneNode>> released;
  if (cg->clip_shape)
    released.push_back(std::move(cg->clip_shape));
  for (auto &ch : cg->children)
    released.push_back(std::move(ch));
  cg->children.clear();

  // Remove ClipGroup from parent.
  parent->children.erase(parent->children.begin() + cg_idx);

  // Insert released nodes at cg_idx in order (first = top = cg_idx).
  std::vector<SceneNode *> new_sel;
  int idx = cg_idx;
  for (auto &n : released) {
    SceneNode *ptr = n.get();
    parent->children.insert(parent->children.begin() + idx, std::move(n));
    new_sel.push_back(ptr);
    ++idx;
  }

  m_selection = new_sel;
  m_selected = new_sel.empty() ? nullptr : new_sel.front();
  m_selected_node = -1;
  m_node_selection.clear();

  notify_object_selection_changed();
  notify_node_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: released ClipGroup → {} objects", (int)new_sel.size());
}

// ── Canvas::make_blend ───────────────────────────────────────────────────────
// M1 scope: exactly two Path nodes selected, sharing a common parent,
// with equal node counts and equal closed-flag. Violations are
// rejected with LOG_WARN + a user-visible message; M3 dialog will
// cover the unequal-node-count path with interactive equalization,
// M4 will insert nodes via De Casteljau midpoint splits.
//
// Operation:
//   1. Validate selection (exactly 2, both Path, same parent, equal
//      node counts, equal closed flag).
//   2. Snapshot both originals for undo (with their indices).
//   3. Build the Blend node: type=Blend, name="Blend", deep-clone A
//      and B into blend_source_a/b, blend_steps=4, dirty=true. First
//      draw will call rebuild_blend_cache.
//   4. Remove both originals from parent (descending index order).
//   5. Insert the Blend at the lower of the two original indices.
//   6. Push MakeBlendCommand (atomic undo).
//   7. Select the new Blend, emit signals.
void Canvas::make_blend(int steps, bool reverse, bool stroke_w_override,
                        double stroke_w_start, double stroke_w_end) {
  if (!m_doc)
    return;

  if (m_selection.size() != 2) {
    LOG_WARN("Canvas::make_blend: need exactly 2 selected, got {}",
             m_selection.size());
    m_sig_show_message.emit("Blend",
                            "Blend requires exactly two selected paths.");
    return;
  }
  SceneNode *a = m_selection[0];
  SceneNode *b = m_selection[1];
  if (reverse)
    std::swap(a, b);
  if (!a || !b || a->type != SceneNode::Type::Path ||
      b->type != SceneNode::Type::Path || !a->path || !b->path) {
    LOG_WARN("Canvas::make_blend: both selections must be Paths");
    m_sig_show_message.emit(
        "Blend",
        "Blend requires both selected objects to be paths. Compound and "
        "other types are not supported in this version.");
    return;
  }

  int idx_a = -1, idx_b = -1;
  SceneNode *pa = find_parent(m_doc, a, &idx_a);
  SceneNode *pb = find_parent(m_doc, b, &idx_b);
  if (!pa || !pb || pa != pb) {
    LOG_WARN("Canvas::make_blend: sources must share the same parent");
    m_sig_show_message.emit(
        "Blend", "Blend requires both paths to be in the same group or layer.");
    return;
  }
  SceneNode *parent = pa;

  // M1: reject unequal node counts / mismatched closed flag. M4 will
  // offer interactive equalization via De Casteljau midpoint inserts.
  if (a->path->nodes.size() != b->path->nodes.size() ||
      a->path->closed != b->path->closed) {
    LOG_WARN("Canvas::make_blend: node-count/closed mismatch (A={} B={} "
             "closedA={} closedB={})",
             a->path->nodes.size(), b->path->nodes.size(), a->path->closed,
             b->path->closed);
    m_sig_show_message.emit(
        "Blend", "The two paths must have the same number of nodes and both be "
                 "open or both be closed. Automatic node-count equalization is "
                 "not yet implemented.");
    return;
  }

  // Order by z-index: "lower" (earlier in children) = bottom. Blend
  // takes the lower slot so it occupies the combined z-range without
  // jumping to the top.
  int lo_idx = std::min(idx_a, idx_b);
  int hi_idx = std::max(idx_a, idx_b);

  // Snapshot originals in ascending index order (matches MakeBlendCommand
  // insertion order on undo).
  std::vector<MakeBlendCommand::Original> originals;
  originals.push_back({clone_node(*parent->children[lo_idx]), lo_idx});
  originals.push_back({clone_node(*parent->children[hi_idx]), hi_idx});

  // Build Blend node. Deep-clone A and B into slots so the Blend is
  // self-contained — sources can be mutated via Layers panel without
  // aliasing into freed memory after we remove them from `children`.
  auto blend = std::make_unique<SceneNode>();
  blend->type = SceneNode::Type::Blend;
  blend->id = next_id();
  blend->internal_id = last_iid();
  blend->name = m_doc->next_default_name(CurvzDocument::NameKind::Blend);
  blend->visible = true;
  blend->locked = false;
  blend->opacity = 1.0;
  blend->blend_source_a = clone_node(*a);
  blend->blend_source_b = clone_node(*b);
  blend->blend_steps = std::clamp(steps, 1, 50);
  blend->blend_stroke_w_user_set = stroke_w_override;
  blend->blend_stroke_w_start = stroke_w_start;
  blend->blend_stroke_w_end = stroke_w_end;
  blend->blend_cache_dirty = true;

  // Remove both originals — descending index order so lo_idx stays valid.
  // After this point, `a` and `b` are dangling pointers.
  parent->children.erase(parent->children.begin() + hi_idx);
  parent->children.erase(parent->children.begin() + lo_idx);

  int ins = std::clamp(lo_idx, 0, (int)parent->children.size());
  SceneNode *blend_ptr = blend.get();

  // Snapshot for undo before we move the unique_ptr.
  auto blend_snap = clone_node(*blend);

  parent->children.insert(parent->children.begin() + ins, std::move(blend));

  if (m_history)
    m_history->push(std::make_unique<MakeBlendCommand>(
        project(), parent->internal_id,
        std::move(originals), std::move(blend_snap), ins));

  m_selected = blend_ptr;
  m_selection = {blend_ptr};
  m_selected_node = -1;
  m_node_selection.clear();

  notify_object_selection_changed();
  notify_node_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: made Blend '{}' with {} source nodes each, steps={}",
           blend_ptr->name, blend_ptr->blend_source_a->path->nodes.size(),
           blend_ptr->blend_steps);
}

// ── Canvas::find_blend_owner ─────────────────────────────────────────────────
// Returns the Blend SceneNode whose blend_source_a or blend_source_b slot
// holds `target`, or nullptr if none. Walks the full tree; Blends are
// rare enough that this is cheap. Used by delete_selected to route
// deletion of A/B through release_blend (matches ClipGroup semantics:
// deleting the clip_shape dissolves, restoring the clipped children).
SceneNode *Canvas::find_blend_owner(SceneNode *target) {
  if (!m_doc || !target)
    return nullptr;
  std::function<SceneNode *(SceneNode *)> walk =
      [&](SceneNode *root) -> SceneNode * {
    if (!root)
      return nullptr;
    if (root->is_blend()) {
      if (root->blend_source_a.get() == target ||
          root->blend_source_b.get() == target)
        return root;
      // Descend into Blend's slots as well, in case of future nested Blends.
      if (auto *r = walk(root->blend_source_a.get()))
        return r;
      if (auto *r = walk(root->blend_source_b.get()))
        return r;
    }
    for (auto &c : root->children)
      if (auto *r = walk(c.get()))
        return r;
    if (root->clip_shape)
      if (auto *r = walk(root->clip_shape.get()))
        return r;
    return nullptr;
  };
  for (auto &layer : m_doc->layers)
    if (auto *r = walk(layer.get()))
      return r;
  return nullptr;
}

// ── Canvas::mark_all_blends_dirty ────────────────────────────────────────────
// Walks every SceneNode in the doc and sets blend_cache_dirty=true on any
// Blend encountered. Called from the prop_changed signal pathway so that
// downstream inspector edits (fill, stroke, opacity, node anchors/handles)
// invalidate cached intermediates. Next draw's lazy rebuild picks it up.
// Cheap — just a bool flip — and correct even when the edited node wasn't
// a Blend source (false positive just triggers one redundant rebuild).
void Canvas::mark_all_blends_dirty() {
  if (!m_doc)
    return;
  std::function<void(SceneNode *)> walk = [&](SceneNode *n) {
    if (!n)
      return;
    if (n->is_blend())
      n->blend_cache_dirty = true;
    for (auto &c : n->children)
      walk(c.get());
    if (n->clip_shape)
      walk(n->clip_shape.get());
    if (n->is_blend()) {
      walk(n->blend_source_a.get());
      walk(n->blend_source_b.get());
    }
    if (n->is_warp()) {
      walk(n->warp_source.get());
      walk(n->warp_glyph_cache.get());
      walk(n->warp_cache.get());
    }
  };
  for (auto &layer : m_doc->layers)
    walk(layer.get());
}

// ── Canvas::find_warp_owner ──────────────────────────────────────────────────
// Returns the Warp SceneNode whose warp_source slot transitively holds
// `target`, or nullptr if none. Walks the full tree. A hit inside the
// source's own subtree (e.g. a Path inside a Group that is warp_source)
// also counts — matches ClipGroup/Blend semantics where touching the
// payload routes through the wrapper.
SceneNode *Canvas::find_warp_owner(SceneNode *target) {
  if (!m_doc || !target)
    return nullptr;
  // Subtree containment check — does `root` contain `target` somewhere?
  std::function<bool(SceneNode *)> contains = [&](SceneNode *root) -> bool {
    if (!root)
      return false;
    if (root == target)
      return true;
    for (auto &c : root->children)
      if (contains(c.get()))
        return true;
    if (root->clip_shape && contains(root->clip_shape.get()))
      return true;
    if (root->is_blend()) {
      if (contains(root->blend_source_a.get()))
        return true;
      if (contains(root->blend_source_b.get()))
        return true;
      for (auto &s : root->blend_cache)
        if (contains(s.get()))
          return true;
    }
    if (root->is_warp()) {
      if (contains(root->warp_source.get()))
        return true;
      if (contains(root->warp_glyph_cache.get()))
        return true;
      if (contains(root->warp_cache.get()))
        return true;
    }
    return false;
  };
  std::function<SceneNode *(SceneNode *)> walk =
      [&](SceneNode *root) -> SceneNode * {
    if (!root)
      return nullptr;
    if (root->is_warp()) {
      if (contains(root->warp_source.get()))
        return root;
      // Descend into Warp slots in case of future nested Warps.
      if (auto *r = walk(root->warp_source.get()))
        return r;
      if (auto *r = walk(root->warp_glyph_cache.get()))
        return r;
      if (auto *r = walk(root->warp_cache.get()))
        return r;
    }
    for (auto &c : root->children)
      if (auto *r = walk(c.get()))
        return r;
    if (root->clip_shape)
      if (auto *r = walk(root->clip_shape.get()))
        return r;
    if (root->is_blend()) {
      if (auto *r = walk(root->blend_source_a.get()))
        return r;
      if (auto *r = walk(root->blend_source_b.get()))
        return r;
    }
    return nullptr;
  };
  for (auto &layer : m_doc->layers)
    if (auto *r = walk(layer.get()))
      return r;
  return nullptr;
}

// ── Canvas::mark_all_warps_dirty ─────────────────────────────────────────────
// Walks every SceneNode and sets both warp_glyph_cache_dirty and
// warp_cache_dirty on any Warp encountered. Called from prop_changed
// signal pathway so downstream inspector edits to source geometry
// invalidate cached outlines/warped geometry on next draw. Coarse but
// cheap — one pair of bool flips per Warp, zero cost when no Warps exist.
void Canvas::mark_all_warps_dirty() {
  if (!m_doc)
    return;
  std::function<void(SceneNode *)> walk = [&](SceneNode *n) {
    if (!n)
      return;
    if (n->is_warp()) {
      n->warp_glyph_cache_dirty = true;
      n->warp_cache_dirty = true;
    }
    for (auto &c : n->children)
      walk(c.get());
    if (n->clip_shape)
      walk(n->clip_shape.get());
    if (n->is_blend()) {
      walk(n->blend_source_a.get());
      walk(n->blend_source_b.get());
    }
    if (n->is_warp()) {
      walk(n->warp_source.get());
      walk(n->warp_glyph_cache.get());
      walk(n->warp_cache.get());
    }
  };
  for (auto &layer : m_doc->layers)
    walk(layer.get());
}

// ─────────────────────────────────────────────────────────────────────────────
// Warp math (M2)
// ─────────────────────────────────────────────────────────────────────────────
//
// Model: ruled surface between two Bézier boundary curves.
//
//   P(u, v) = (1 - v) · C_bottom(u) + v · C_top(u)      for u,v ∈ [0,1]
//
// where:
//   u ∈ [0,1] is horizontal position, normalized across the glyph_cache bbox
//   v ∈ [0,1] is vertical position, normalized across the glyph_cache bbox
//   C_top(u), C_bottom(u) are the top and bottom envelope curves, each a
//     PathData with 1..N cubic segments (N = nodes.size()-1 for an open
//     polybezier).
//
// Left and right sides are implicit straight lines between the top/bottom
// corner endpoints — trapezoidal perspective warps fall out naturally when
// the top is narrower than the bottom, no special-case needed.
//
// u parametrization: segment-uniform. A 3-segment envelope treats u as
// [0, 1/3] = segment 0, [1/3, 2/3] = segment 1, [2/3, 1] = segment 2.
// This is simpler than arclength-accurate and matches how users draw
// multi-anchor envelopes (each segment controls roughly the same amount
// of output). Arclength-accurate can be a later refinement.
//
// Subdivision: warp_quality (1..16) controls how many equal t-slices each
// source cubic segment is split into before warping. Each slice produces
// one output cubic fit via the "implicit-handle through 4 samples"
// technique — given warped p(0), p(1/3), p(2/3), p(1), solve for the
// cubic's two handle control points exactly. Cheap, accurate for cubics,
// no least-squares needed.

// Evaluate a single cubic segment at parameter t ∈ [0,1].
// p0/p3 are the anchor endpoints, p1/p2 the bezier handles (absolute coords).
static void eval_cubic(double t, double p0x, double p0y, double p1x, double p1y,
                       double p2x, double p2y, double p3x, double p3y,
                       double &ox, double &oy) {
  double mt = 1.0 - t;
  double mt2 = mt * mt;
  double t2 = t * t;
  double a = mt2 * mt;
  double b = 3.0 * mt2 * t;
  double c = 3.0 * mt * t2;
  double d = t2 * t;
  ox = a * p0x + b * p1x + c * p2x + d * p3x;
  oy = a * p0y + b * p1y + c * p2y + d * p3y;
}

// Evaluate an envelope curve (a PathData with 1..N cubic segments) at the
// global parameter u ∈ [0,1]. Uses segment-uniform parametrization: each
// segment gets an equal slice of u.
//
// Degenerate fallbacks:
//   empty nodes           → (0,0), caller handles
//   single node           → that anchor's position
//   single cubic (2 nodes)→ eval_cubic on the one segment
static void eval_envelope(const PathData &env, double u, double &ox,
                          double &oy) {
  const auto &ns = env.nodes;
  if (ns.empty()) {
    ox = 0.0;
    oy = 0.0;
    return;
  }
  if (ns.size() == 1) {
    ox = ns[0].x;
    oy = ns[0].y;
    return;
  }
  int seg_count = (int)ns.size() - 1; // open polybezier: N nodes = N-1 segs
  if (seg_count <= 0) {
    ox = ns[0].x;
    oy = ns[0].y;
    return;
  }
  // Clamp u to [0,1]
  if (u < 0.0)
    u = 0.0;
  if (u > 1.0)
    u = 1.0;
  // Which segment? Segment i owns [i/seg_count, (i+1)/seg_count].
  double scaled = u * seg_count;
  int seg = (int)scaled;
  if (seg >= seg_count)
    seg = seg_count - 1; // u == 1.0 edge case
  double local_t = scaled - (double)seg;
  const BezierNode &a = ns[seg];
  const BezierNode &b = ns[seg + 1];
  // Outgoing handle of a is a.cx2/cy2; incoming handle of b is b.cx1/cy1
  eval_cubic(local_t, a.x, a.y, a.cx2, a.cy2, b.cx1, b.cy1, b.x, b.y, ox, oy);
}

// Normalize a doc-space point (px, py) into (u, v) ∈ [0,1]² given a
// glyph_cache bbox. No clamping — points outside the bbox extrapolate,
// which is fine for degenerate inputs (extrapolation just lands outside
// the envelope, which is where they'd land anyway).
static void normalize_uv(double px, double py, double bbx, double bby,
                         double bbw, double bbh, double &u, double &v) {
  u = (bbw > 1e-12) ? (px - bbx) / bbw : 0.0;
  v = (bbh > 1e-12) ? (py - bby) / bbh : 0.0;
}

// Map a doc-space point through the warp envelope. Returns the warped
// position. envelope_top is at v=1 (top of source bbox maps to top curve);
// envelope_bottom is at v=0 (bottom of source bbox maps to bottom curve).
//
// Y convention: PathData is stored in doc space Y-down (canonical Cairo
// convention used throughout Curvz). Source bbox y=bby is the top edge
// in Y-down, y=bby+bbh is the bottom. We want the point with v=0 (small
// y) to map to the TOP envelope and v=1 (large y) to map to the BOTTOM
// envelope. So warp_env_top corresponds to v=0 and warp_env_bottom to
// v=1 in our blend.
static void warp_point(double px, double py, double bbx, double bby, double bbw,
                       double bbh, const PathData &env_top,
                       const PathData &env_bottom, double &ox, double &oy) {
  double u, v;
  normalize_uv(px, py, bbx, bby, bbw, bbh, u, v);
  double tx, ty, bx, by;
  eval_envelope(env_top, u, tx, ty);
  eval_envelope(env_bottom, u, bx, by);
  // v=0 → top envelope, v=1 → bottom envelope (Y-down doc space)
  double mv = 1.0 - v;
  ox = mv * tx + v * bx;
  oy = mv * ty + v * by;
}

// Fit a cubic Bézier through four samples at t = 0, 1/3, 2/3, 1.
// Given: q0 = p(0), q1 = p(1/3), q2 = p(2/3), q3 = p(1)
// Cubic: p(t) = (1-t)³·c0 + 3(1-t)²t·c1 + 3(1-t)t²·c2 + t³·c3
// c0 = q0 and c3 = q3 (endpoints match).
// At t=1/3: q1 = 8/27·c0 + 12/27·c1 + 6/27·c2 + 1/27·c3
// At t=2/3: q2 = 1/27·c0 + 6/27·c1 + 12/27·c2 + 8/27·c3
// Solving this 2×2 system for c1, c2:
//   c1 = ( -5·q0 + 18·q1 -  9·q2 + 2·q3) / 6
//   c2 = (  2·q0 -  9·q1 + 18·q2 - 5·q3) / 6
static void fit_cubic_4samples(double q0x, double q0y, double q1x, double q1y,
                               double q2x, double q2y, double q3x, double q3y,
                               double &c1x, double &c1y, double &c2x,
                               double &c2y) {
  c1x = (-5.0 * q0x + 18.0 * q1x - 9.0 * q2x + 2.0 * q3x) / 6.0;
  c1y = (-5.0 * q0y + 18.0 * q1y - 9.0 * q2y + 2.0 * q3y) / 6.0;
  c2x = (2.0 * q0x - 9.0 * q1x + 18.0 * q2x - 5.0 * q3x) / 6.0;
  c2y = (2.0 * q0y - 9.0 * q1y + 18.0 * q2y - 5.0 * q3y) / 6.0;
}

// s161 split: path_anchor_bbox was originally between fit_cubic_4samples
//   and default_envelope_from_bbox here. It was moved to Canvas.cpp (CORE)
//   alongside its sole caller subtree_path_bbox. The s160 handoff routed
//   path_anchor_bbox to OPS as single-TU static, but subtree_path_bbox
//   was correctly routed to CORE; keeping path_anchor_bbox here would
//   have made it cross-TU for one caller in another file.

// Seed a Warp's envelope curves as straight 2-anchor lines across the
// top and bottom of the glyph_cache bbox. Identity warp — the output
// will look exactly like the input. Called when rebuild_warp_cache is
// asked to work on a Warp whose envelopes are empty (hand-built in
// test code, or a freshly-parsed M1-stub file that had no envelope
// encoding). M3's MakeWarpCommand will also call this at construction.
//
// Handles are colinear with the endpoints (classic "straight Bézier"
// with handles at 1/3 and 2/3 along the line), so the curve IS a line.
static void default_envelope_from_bbox(PathData &env_top, PathData &env_bottom,
                                       double bx, double by, double bw,
                                       double bh) {
  env_top.nodes.clear();
  env_top.closed = false;
  env_bottom.nodes.clear();
  env_bottom.closed = false;
  // Top envelope — two anchors at top corners. y = by (top in Y-down).
  double x0 = bx, x1 = bx + bw;
  double yt = by, yb = by + bh;
  auto mk = [](double ax, double ay) {
    BezierNode n;
    n.x = ax;
    n.y = ay;
    n.cx1 = ax;
    n.cy1 = ay;
    n.cx2 = ax;
    n.cy2 = ay;
    return n;
  };
  // Position handles at 1/3 along the straight segment so the cubic
  // degenerates to the line (tangent direction matches the chord).
  double dx = (x1 - x0) / 3.0;
  BezierNode t0 = mk(x0, yt);
  t0.cx2 = x0 + dx;
  t0.cy2 = yt;
  BezierNode t1 = mk(x1, yt);
  t1.cx1 = x1 - dx;
  t1.cy1 = yt;
  env_top.nodes.push_back(t0);
  env_top.nodes.push_back(t1);
  BezierNode b0 = mk(x0, yb);
  b0.cx2 = x0 + dx;
  b0.cy2 = yb;
  BezierNode b1 = mk(x1, yb);
  b1.cx1 = x1 - dx;
  b1.cy1 = yb;
  env_bottom.nodes.push_back(b0);
  env_bottom.nodes.push_back(b1);
}

// Warp a single PathData through the envelope, producing a new PathData.
// Each source cubic segment between nodes[i] and nodes[i+1] is split
// into `quality` equal t-slices, each slice warped and fit as a single
// cubic via the 4-sample method. Output has (source_seg_count * quality)
// cubic segments, plus the original closed-flag and one more leading
// anchor (for N+1 anchors describing N segments).
//
// Handles of the first and last output anchors (cx1 of [0], cx2 of
// [last]) inherit from the colinear fit for continuity with neighbours
// on closed paths. For open paths they can be left equal to the anchor
// (effectively untangented at the endpoints).
static PathData warp_path_data(const PathData &src, const PathData &env_top,
                               const PathData &env_bottom, double bx, double by,
                               double bw, double bh, int quality) {
  PathData out;
  out.closed = src.closed;
  if (src.nodes.size() < 2) {
    // Single-anchor or empty path — warp the lone anchor (if any) and
    // return a degenerate path.
    for (const auto &n : src.nodes) {
      double wx, wy;
      warp_point(n.x, n.y, bx, by, bw, bh, env_top, env_bottom, wx, wy);
      BezierNode wn;
      wn.x = wx;
      wn.y = wy;
      wn.cx1 = wx;
      wn.cy1 = wy;
      wn.cx2 = wx;
      wn.cy2 = wy;
      out.nodes.push_back(wn);
    }
    return out;
  }
  if (quality < 1)
    quality = 1;
  if (quality > 16)
    quality = 16;
  // For a closed path the "segment" from nodes[last] back to nodes[0]
  // also warps. Open paths stop at nodes[last]. We iterate segments
  // accordingly.
  int n_nodes = (int)src.nodes.size();
  int n_segs = src.closed ? n_nodes : (n_nodes - 1);
  // Preallocate first anchor (gets filled below with first slice's p0).
  // Each segment emits `quality` output segments; for the very first
  // segment of an open path we also need to push the leading anchor
  // before the first slice.
  bool first_anchor_emitted = false;
  for (int si = 0; si < n_segs; ++si) {
    const BezierNode &a = src.nodes[si];
    const BezierNode &b = src.nodes[(si + 1) % n_nodes];
    for (int q = 0; q < quality; ++q) {
      double t0 = (double)q / quality;
      double t1 = (double)(q + 1) / quality;
      double t13 = t0 + (t1 - t0) / 3.0;
      double t23 = t0 + 2.0 * (t1 - t0) / 3.0;
      // Sample the source cubic at t0, 1/3, 2/3, t1 and warp each sample
      double s0x, s0y, s1x, s1y, s2x, s2y, s3x, s3y;
      eval_cubic(t0, a.x, a.y, a.cx2, a.cy2, b.cx1, b.cy1, b.x, b.y, s0x, s0y);
      eval_cubic(t13, a.x, a.y, a.cx2, a.cy2, b.cx1, b.cy1, b.x, b.y, s1x, s1y);
      eval_cubic(t23, a.x, a.y, a.cx2, a.cy2, b.cx1, b.cy1, b.x, b.y, s2x, s2y);
      eval_cubic(t1, a.x, a.y, a.cx2, a.cy2, b.cx1, b.cy1, b.x, b.y, s3x, s3y);
      double w0x, w0y, w1x, w1y, w2x, w2y, w3x, w3y;
      warp_point(s0x, s0y, bx, by, bw, bh, env_top, env_bottom, w0x, w0y);
      warp_point(s1x, s1y, bx, by, bw, bh, env_top, env_bottom, w1x, w1y);
      warp_point(s2x, s2y, bx, by, bw, bh, env_top, env_bottom, w2x, w2y);
      warp_point(s3x, s3y, bx, by, bw, bh, env_top, env_bottom, w3x, w3y);
      // Fit the warped cubic: control points c1, c2 from the 4 samples
      double c1x, c1y, c2x, c2y;
      fit_cubic_4samples(w0x, w0y, w1x, w1y, w2x, w2y, w3x, w3y, c1x, c1y, c2x,
                         c2y);
      // Emit anchor w0 on the first slice (or when starting a new
      // segment's first slice on an open path's very first iteration)
      if (!first_anchor_emitted) {
        BezierNode wn;
        wn.x = w0x;
        wn.y = w0y;
        wn.cx1 = w0x;
        wn.cy1 = w0y; // patched below if prev slice exists
        wn.cx2 = c1x;
        wn.cy2 = c1y;
        out.nodes.push_back(wn);
        first_anchor_emitted = true;
      } else {
        // Patch the previous anchor's outgoing handle to match THIS
        // slice's c1. (It was provisionally set from the PREVIOUS
        // slice's fit; overwriting with the continuation maintains
        // C0 continuity and locks in per-slice tangents.)
        BezierNode &prev = out.nodes.back();
        prev.cx2 = c1x;
        prev.cy2 = c1y;
      }
      // Emit anchor w3 with incoming handle c2 and outgoing provisional
      // (set equal to w3 for now; next slice will overwrite, or if this
      // is the last slice on a closed path, the first-anchor patch
      // below finishes the loop).
      BezierNode wn;
      wn.x = w3x;
      wn.y = w3y;
      wn.cx1 = c2x;
      wn.cy1 = c2y;
      wn.cx2 = w3x;
      wn.cy2 = w3y;
      out.nodes.push_back(wn);
    }
  }
  // Closed path: the last emitted anchor is actually a duplicate of the
  // first (we walked N segments and emitted N+1 anchors, but on a closed
  // path nodes[0] == nodes[last] logically). Merge the duplicate:
  // take the last anchor's cx1 and move it to the first anchor's cx1,
  // then drop the last anchor.
  if (src.closed && out.nodes.size() > 1) {
    BezierNode &last = out.nodes.back();
    BezierNode &first = out.nodes.front();
    first.cx1 = last.cx1;
    first.cy1 = last.cy1;
    out.nodes.pop_back();
  }
  return out;
}

// Walk src recursively, producing a structurally-parallel tree in dst
// where every Path has its PathData replaced by the warped version.
// Group/Compound containers are cloned preserving type, children are
// recursed into. Non-path leaves (Text, Image, Ref — which don't appear
// in the M1-M6 source types but future text Warp will hit) are cloned
// as-is for forward-compat; they render un-warped, which is the
// reasonable fallback until text-warp math lands.
static std::unique_ptr<SceneNode> warp_subtree(const SceneNode &src,
                                               const PathData &env_top,
                                               const PathData &env_bottom,
                                               double bx, double by, double bw,
                                               double bh, int quality) {
  auto dst = std::make_unique<SceneNode>();
  dst->type = src.type;
  dst->id = src.id;
  dst->internal_id = src.internal_id;
  dst->name = src.name;
  dst->visible = src.visible;
  dst->locked = src.locked;
  dst->opacity = src.opacity;
  dst->color = src.color;
  dst->transform = src.transform;
  dst->fill = src.fill;
  dst->stroke = src.stroke;
  dst->fill_swatch_id = src.fill_swatch_id;
  dst->stroke_swatch_id = src.stroke_swatch_id;
  if (src.type == SceneNode::Type::Path && src.path) {
    dst->path = std::make_unique<PathData>(warp_path_data(
        *src.path, env_top, env_bottom, bx, by, bw, bh, quality));
  }
  for (const auto &c : src.children) {
    dst->children.push_back(
        warp_subtree(*c, env_top, env_bottom, bx, by, bw, bh, quality));
  }
  return dst;
}

// ── Canvas::rebuild_warp_caches ──────────────────────────────────────────────
// Brings both caches in sync with the source + envelope state. Three-phase:
//
//   1. Glyph cache: deep-clone source → glyph_cache. This is a no-op for
//      today's Path/Compound/Group sources (glyph_cache is literally a
//      copy). Future text-source Warp replaces this with render-string-
//      to-outlines. Triggered by warp_glyph_cache_dirty.
//
//   2. Envelope defaults: if env_top or env_bottom is empty, seed them
//      from the glyph_cache bbox as straight 2-anchor lines. Identity
//      warp. Lets hand-constructed and freshly-parsed M1-stub Warps
//      render without blowing up.
//
//   3. Warp cache: walk glyph_cache, warp each path through the
//      envelope using warp_quality subdivision, emit into warp_cache.
//      Triggered by warp_cache_dirty.
//
// Safe on nodes that aren't Warps (no-op). Safe on Warps with null
// source (clears caches, logs warn). Idempotent — calling twice with
// no state change does nothing after the first.
void Canvas::rebuild_warp_caches(SceneNode *w) {
  if (!w || !w->is_warp())
    return;
  if (!w->warp_source) {
    LOG_WARN("Canvas::rebuild_warp_caches: Warp '{}' has null source — "
             "clearing caches",
             w->name);
    w->warp_glyph_cache.reset();
    w->warp_cache.reset();
    w->warp_glyph_cache_dirty = false;
    w->warp_cache_dirty = false;
    return;
  }
  // Phase 1: glyph cache
  if (w->warp_glyph_cache_dirty || !w->warp_glyph_cache) {
    w->warp_glyph_cache = clone_node(*w->warp_source);
    w->warp_glyph_cache_dirty = false;
    // Changing glyph cache implies warp cache must also rebuild
    w->warp_cache_dirty = true;
  }
  // Phase 2: compute bbox across glyph_cache, seed default envelopes
  double bx = 0, by = 0, bw = 0, bh = 0;
  if (!subtree_path_bbox(w->warp_glyph_cache.get(), bx, by, bw, bh)) {
    LOG_WARN("Canvas::rebuild_warp_caches: Warp '{}' glyph_cache has no "
             "paths — cannot compute bbox, clearing warp_cache",
             w->name);
    w->warp_cache.reset();
    w->warp_cache_dirty = false;
    return;
  }
  if (w->warp_env_top.nodes.empty() || w->warp_env_bottom.nodes.empty()) {
    default_envelope_from_bbox(w->warp_env_top, w->warp_env_bottom, bx, by, bw,
                               bh);
    // Envelope changed implicitly; ensure warp_cache rebuild runs
    w->warp_cache_dirty = true;
  }
  // Phase 3: warp cache
  if (w->warp_cache_dirty || !w->warp_cache) {
    int q = std::clamp(w->warp_quality, 1, 16);
    w->warp_cache = warp_subtree(*w->warp_glyph_cache, w->warp_env_top,
                                 w->warp_env_bottom, bx, by, bw, bh, q);
    w->warp_cache_dirty = false;
  }
}

// ── Canvas::sync_warp_env_picks_to_selection ────────────────────────────────
// Lazy invalidation: if the recorded pick-set owner no longer matches the
// current primary selection (or that primary isn't a Warp anymore), clear
// the pick set. Called at every read site that consults the pick set.
void Canvas::sync_warp_env_picks_to_selection() {
  const SceneNode *cur =
      (m_selected && m_selected->is_warp()) ? m_selected : nullptr;
  if (m_warp_env_picks_owner != cur) {
    m_warp_env_picks.clear();
    m_warp_env_picks_owner = cur;
  }
}

// ── Canvas::warp_env_picks_clear ────────────────────────────────────────────
void Canvas::warp_env_picks_clear() {
  if (m_warp_env_picks.empty())
    return;
  m_warp_env_picks.clear();
  queue_draw();
}

// ── Canvas::warp_env_picks_select_all_top_anchors ───────────────────────────
// Picks every anchor AND every visible handle on the top envelope.
// Coincident handles are skipped since they're not visually aimable.
void Canvas::warp_env_picks_select_all_top_anchors() {
  if (!m_selected || !m_selected->is_warp())
    return;
  m_warp_env_picks.clear();
  m_warp_env_picks_owner = m_selected;
  const PathData &env = m_selected->warp_env_top;
  for (int i = 0; i < (int)env.nodes.size(); ++i) {
    const BezierNode &n = env.nodes[i];
    m_warp_env_picks.push_back({true, i, EnvelopePart::Anchor});
    if (std::hypot(n.cx1 - n.x, n.cy1 - n.y) > 1e-6)
      m_warp_env_picks.push_back({true, i, EnvelopePart::HandleIn});
    if (std::hypot(n.cx2 - n.x, n.cy2 - n.y) > 1e-6)
      m_warp_env_picks.push_back({true, i, EnvelopePart::HandleOut});
  }
  queue_draw();
}

// ── Canvas::warp_env_picks_select_all_bottom_anchors ────────────────────────
// Picks every anchor AND every visible handle on the bottom envelope.
void Canvas::warp_env_picks_select_all_bottom_anchors() {
  if (!m_selected || !m_selected->is_warp())
    return;
  m_warp_env_picks.clear();
  m_warp_env_picks_owner = m_selected;
  const PathData &env = m_selected->warp_env_bottom;
  for (int i = 0; i < (int)env.nodes.size(); ++i) {
    const BezierNode &n = env.nodes[i];
    m_warp_env_picks.push_back({false, i, EnvelopePart::Anchor});
    if (std::hypot(n.cx1 - n.x, n.cy1 - n.y) > 1e-6)
      m_warp_env_picks.push_back({false, i, EnvelopePart::HandleIn});
    if (std::hypot(n.cx2 - n.x, n.cy2 - n.y) > 1e-6)
      m_warp_env_picks.push_back({false, i, EnvelopePart::HandleOut});
  }
  queue_draw();
}

// ── Canvas::warp_env_picks_select_leftmost_pair ─────────────────────────────
// Picks the leftmost anchor (min x) of top + leftmost of bottom, along
// with each picked anchor's visible handles so the anchor+handles move
// as a unit when dragged.
void Canvas::warp_env_picks_select_leftmost_pair() {
  if (!m_selected || !m_selected->is_warp())
    return;
  m_warp_env_picks.clear();
  m_warp_env_picks_owner = m_selected;
  auto pick_leftmost = [&](const PathData &env, bool is_top) {
    if (env.nodes.empty())
      return;
    int best = 0;
    double best_x = env.nodes[0].x;
    for (int i = 1; i < (int)env.nodes.size(); ++i) {
      if (env.nodes[i].x < best_x) {
        best = i;
        best_x = env.nodes[i].x;
      }
    }
    const BezierNode &n = env.nodes[best];
    m_warp_env_picks.push_back({is_top, best, EnvelopePart::Anchor});
    if (std::hypot(n.cx1 - n.x, n.cy1 - n.y) > 1e-6)
      m_warp_env_picks.push_back({is_top, best, EnvelopePart::HandleIn});
    if (std::hypot(n.cx2 - n.x, n.cy2 - n.y) > 1e-6)
      m_warp_env_picks.push_back({is_top, best, EnvelopePart::HandleOut});
  };
  pick_leftmost(m_selected->warp_env_top, true);
  pick_leftmost(m_selected->warp_env_bottom, false);
  queue_draw();
}

// ── Canvas::warp_env_picks_select_rightmost_pair ────────────────────────────
// Picks the rightmost anchor (max x) of top + rightmost of bottom,
// with each picked anchor's visible handles so they travel together.
void Canvas::warp_env_picks_select_rightmost_pair() {
  if (!m_selected || !m_selected->is_warp())
    return;
  m_warp_env_picks.clear();
  m_warp_env_picks_owner = m_selected;
  auto pick_rightmost = [&](const PathData &env, bool is_top) {
    if (env.nodes.empty())
      return;
    int best = 0;
    double best_x = env.nodes[0].x;
    for (int i = 1; i < (int)env.nodes.size(); ++i) {
      if (env.nodes[i].x > best_x) {
        best = i;
        best_x = env.nodes[i].x;
      }
    }
    const BezierNode &n = env.nodes[best];
    m_warp_env_picks.push_back({is_top, best, EnvelopePart::Anchor});
    if (std::hypot(n.cx1 - n.x, n.cy1 - n.y) > 1e-6)
      m_warp_env_picks.push_back({is_top, best, EnvelopePart::HandleIn});
    if (std::hypot(n.cx2 - n.x, n.cy2 - n.y) > 1e-6)
      m_warp_env_picks.push_back({is_top, best, EnvelopePart::HandleOut});
  };
  pick_rightmost(m_selected->warp_env_top, true);
  pick_rightmost(m_selected->warp_env_bottom, false);
  queue_draw();
}

// ── Canvas::warp_env_picks_select_interior_anchors ──────────────────────────
// All anchors except the leftmost and rightmost (by x) on each envelope.
// For a 2-anchor envelope this yields nothing; for 3-anchor it's the
// middle; for 4-anchor it's the two middles.
void Canvas::warp_env_picks_select_interior_anchors() {
  if (!m_selected || !m_selected->is_warp())
    return;
  // Build into a local list first — only swap into m_warp_env_picks if
  // something was found. Keeps C non-destructive on 2-anchor envelopes
  // where there are no interior anchors.
  std::vector<EnvelopePick> picks;
  auto pick_interior = [&](const PathData &env, bool is_top) {
    int n = (int)env.nodes.size();
    if (n <= 2)
      return;
    // Find leftmost and rightmost indices.
    int lm = 0, rm = 0;
    double lx = env.nodes[0].x, rx = env.nodes[0].x;
    for (int i = 1; i < n; ++i) {
      if (env.nodes[i].x < lx) {
        lx = env.nodes[i].x;
        lm = i;
      }
      if (env.nodes[i].x > rx) {
        rx = env.nodes[i].x;
        rm = i;
      }
    }
    for (int i = 0; i < n; ++i) {
      if (i == lm || i == rm)
        continue;
      picks.push_back({is_top, i, EnvelopePart::Anchor});
    }
  };
  pick_interior(m_selected->warp_env_top, true);
  pick_interior(m_selected->warp_env_bottom, false);
  if (picks.empty())
    return; // No-op on 2-anchor envelopes.
  m_warp_env_picks = std::move(picks);
  m_warp_env_picks_owner = m_selected;
  queue_draw();
}

// ── Canvas::warp_env_picks_select_all_anchors ───────────────────────────────
void Canvas::warp_env_picks_select_all_anchors() {
  if (!m_selected || !m_selected->is_warp())
    return;
  m_warp_env_picks.clear();
  m_warp_env_picks_owner = m_selected;
  for (int i = 0; i < (int)m_selected->warp_env_top.nodes.size(); ++i)
    m_warp_env_picks.push_back({true, i, EnvelopePart::Anchor});
  for (int i = 0; i < (int)m_selected->warp_env_bottom.nodes.size(); ++i)
    m_warp_env_picks.push_back({false, i, EnvelopePart::Anchor});
  queue_draw();
}

// ── Canvas::warp_env_picks_select_all ───────────────────────────────────────
// Every anchor + every handle on both envelopes. Skips coincident handles
// (those that overlap their anchor and aren't visually distinct).
void Canvas::warp_env_picks_select_all() {
  if (!m_selected || !m_selected->is_warp())
    return;
  m_warp_env_picks.clear();
  m_warp_env_picks_owner = m_selected;
  auto add_env = [&](const PathData &env, bool is_top) {
    for (int i = 0; i < (int)env.nodes.size(); ++i) {
      const BezierNode &n = env.nodes[i];
      m_warp_env_picks.push_back({is_top, i, EnvelopePart::Anchor});
      // Handle dots are only drawn when offset from the anchor — include
      // them in the pick only when visually distinct to avoid confusing
      // "I picked something invisible" cases.
      if (std::hypot(n.cx1 - n.x, n.cy1 - n.y) > 1e-6)
        m_warp_env_picks.push_back({is_top, i, EnvelopePart::HandleIn});
      if (std::hypot(n.cx2 - n.x, n.cy2 - n.y) > 1e-6)
        m_warp_env_picks.push_back({is_top, i, EnvelopePart::HandleOut});
    }
  };
  add_env(m_selected->warp_env_top, true);
  add_env(m_selected->warp_env_bottom, false);
  queue_draw();
}

// ── Canvas::make_warp ────────────────────────────────────────────────────────
// Wraps the single selected Path / Compound / Group in a Warp container
// at the same position in the parent's children. Envelope is left empty
// — rebuild_warp_caches seeds a default identity envelope from the
// glyph_cache bbox on first draw. M3a ships with this identity default
// (user sees an unchanged-looking result after Make); M3b's dialog lets
// the user shape the envelope. Undoable via MakeWarpCommand.
//
// Preconditions (mirror Blend for consistency):
//   exactly 1 selected node, not already inside a Warp/Blend/ClipGroup
//   wrapper at the same parent level, type ∈ {Path, Compound, Group}.
// Deeper preconditions get a user-visible error message; the menu-
// sensitivity gate in MainWindow only checks count and type.
void Canvas::make_warp() {
  if (!m_doc)
    return;

  if (m_selection.size() != 1) {
    LOG_WARN("Canvas::make_warp: need exactly 1 selected, got {}",
             m_selection.size());
    m_sig_show_message.emit(
        "Warp", "Warp requires exactly one selected path, compound, or group.");
    return;
  }
  SceneNode *src = m_selection[0];
  if (!src || (src->type != SceneNode::Type::Path &&
               src->type != SceneNode::Type::Compound &&
               src->type != SceneNode::Type::Group)) {
    LOG_WARN("Canvas::make_warp: selection must be Path, Compound, or Group");
    m_sig_show_message.emit(
        "Warp",
        "Warp requires the selection to be a path, compound, or group.");
    return;
  }
  int src_idx = -1;
  SceneNode *parent = find_parent(m_doc, src, &src_idx);
  if (!parent) {
    LOG_WARN("Canvas::make_warp: could not find parent of source");
    return;
  }
  // Snapshot the original for undo BEFORE any mutation.
  auto source_snap = clone_node(*src);

  // Build the Warp node. Source is deep-cloned into the slot so the
  // Warp is self-contained — the original's unique_ptr will be erased
  // from parent->children below.
  auto warp = std::make_unique<SceneNode>();
  warp->type = SceneNode::Type::Warp;
  warp->id = next_id();
  warp->internal_id = last_iid();
  warp->name = m_doc->next_default_name(CurvzDocument::NameKind::Warp);
  warp->visible = true;
  warp->locked = false;
  warp->opacity = 1.0;
  warp->warp_source = clone_node(*src);
  warp->warp_glyph_cache_dirty = true;
  warp->warp_cache_dirty = true;
  // Leave warp_env_top / warp_env_bottom empty — rebuild_warp_caches
  // seeds identity from the glyph_cache bbox on first draw.
  warp->warp_quality = 4;

  // Remove the original and insert the Warp at the same position.
  parent->children.erase(parent->children.begin() + src_idx);
  int ins = std::clamp(src_idx, 0, (int)parent->children.size());
  SceneNode *warp_ptr = warp.get();

  // Snapshot the built Warp for undo BEFORE moving the unique_ptr.
  auto warp_snap = clone_node(*warp);

  parent->children.insert(parent->children.begin() + ins, std::move(warp));

  if (m_history)
    m_history->push(std::make_unique<MakeWarpCommand>(
        project(), parent->internal_id,
        std::move(source_snap), src_idx, std::move(warp_snap), ins));

  m_selected = warp_ptr;
  m_selection = {warp_ptr};
  m_selected_node = -1;
  m_node_selection.clear();

  notify_object_selection_changed();
  notify_node_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: made Warp '{}' around source type={}", warp_ptr->name,
           (int)warp_ptr->warp_source->type);
}

// ── Canvas::release_warp ─────────────────────────────────────────────────────
// Undoes a Warp back to its source node. The Warp's envelope is
// discarded — use Flatten Warp if the visible warped geometry should
// be preserved. Result takes the Warp's slot position and is selected.
void Canvas::release_warp() {
  if (!m_doc || !m_selected || !m_selected->is_warp()) {
    LOG_INFO("Canvas::release_warp: ignored — selection is not a Warp");
    return;
  }
  if (!m_selected->warp_source) {
    LOG_WARN("Canvas::release_warp: Warp has null source — cannot release");
    m_sig_show_message.emit(
        "Release Warp",
        "This Warp has no source to release — it appears to be corrupted "
        "or was loaded from a malformed file. Use Delete instead.");
    return;
  }
  int warp_idx = 0;
  SceneNode *parent = find_parent(m_doc, m_selected, &warp_idx);
  if (!parent) {
    LOG_WARN("Canvas::release_warp: could not find parent of Warp");
    return;
  }
  SceneNode *warp = m_selected;

  // Snapshot the Warp (with envelope + caches) for undo.
  auto warp_snap = clone_node(*warp);

  // Build the source-out node: clone warp_source, mint fresh id/iid
  // so it doesn't alias the slot clone held by warp_snap if the Warp
  // is later restored via undo.
  auto source_out = clone_node(*warp->warp_source);
  source_out->id = next_id();
  source_out->internal_id = last_iid();

  // Snapshot the source-out for the command before moving it in.
  auto source_snap = clone_node(*source_out);

  // Mutate the live tree: remove Warp, insert source at same index.
  parent->children.erase(parent->children.begin() + warp_idx);
  int ins = std::clamp(warp_idx, 0, (int)parent->children.size());
  parent->children.insert(parent->children.begin() + ins,
                          std::move(source_out));
  SceneNode *new_sel = parent->children[ins].get();

  if (m_history)
    m_history->push(std::make_unique<ReleaseWarpCommand>(
        project(), parent->internal_id,
        std::move(warp_snap), std::move(source_snap), warp_idx));

  m_selected = new_sel;
  m_selection = {new_sel};
  m_selected_node = -1;
  m_node_selection.clear();

  notify_object_selection_changed();
  notify_node_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: released Warp → source");
}

// ── Canvas::flatten_warp ─────────────────────────────────────────────────────
// Replaces the selected Warp with a plain clone of its warp_cache —
// the baked warped geometry. Forces a cache rebuild first so the
// flatten reflects the current envelope state. Envelope is lost in
// the process; undo restores the Warp intact.
//
// For Path sources the flattened result is a Path; for Compound a
// Compound; for Group a Group. The Warp's visible/locked/opacity/
// transform do NOT transfer to the flattened result (the warp_cache
// clones already carry the source's own styling) — any "on the Warp"
// styling the user may have set via the Warp node itself will be
// lost, which matches how Flatten destructive-commits work.
void Canvas::flatten_warp() {
  if (!m_doc || !m_selected || !m_selected->is_warp()) {
    LOG_INFO("Canvas::flatten_warp: ignored — selection is not a Warp");
    return;
  }
  // Force a fresh rebuild so the flatten reflects the current envelope.
  rebuild_warp_caches(m_selected);

  if (!m_selected->warp_cache) {
    LOG_WARN("Canvas::flatten_warp: warp_cache is null — nothing to flatten");
    m_sig_show_message.emit(
        "Flatten Warp",
        "This Warp has no baked geometry to flatten. Try Release Warp "
        "instead.");
    return;
  }
  int warp_idx = 0;
  SceneNode *parent = find_parent(m_doc, m_selected, &warp_idx);
  if (!parent) {
    LOG_WARN("Canvas::flatten_warp: could not find parent of Warp");
    return;
  }
  SceneNode *warp = m_selected;

  // Snapshot the Warp for undo.
  auto warp_snap = clone_node(*warp);

  // Clone warp_cache, mint fresh id/iid so it doesn't alias the Warp's
  // internal cache clone held by warp_snap if the Warp is later
  // restored via undo.
  auto flat = clone_node(*warp->warp_cache);
  flat->id = next_id();
  flat->internal_id = last_iid();

  auto flat_snap = clone_node(*flat);

  parent->children.erase(parent->children.begin() + warp_idx);
  int ins = std::clamp(warp_idx, 0, (int)parent->children.size());
  parent->children.insert(parent->children.begin() + ins, std::move(flat));
  SceneNode *new_sel = parent->children[ins].get();

  if (m_history)
    m_history->push(std::make_unique<FlattenWarpCommand>(
        project(), parent->internal_id,
        std::move(warp_snap), std::move(flat_snap), warp_idx));

  m_selected = new_sel;
  m_selection = {new_sel};
  m_selected_node = -1;
  m_node_selection.clear();

  notify_object_selection_changed();
  notify_node_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: flattened Warp");
}

// Brings A and B to the same node count by inserting nodes into whichever
// has fewer, via greedy longest-segment-midpoint subdivision. De Casteljau
// split at t=0.5 is shape-preserving (the visual curve doesn't change) so
// equalization is safe to apply before the user even confirms the Blend.
//
// Greedy selection — each iteration picks the segment with the largest
// current arc length and splits it. This spreads inserted nodes evenly
// across the path regardless of its topology: a path with one very long
// segment and several short ones gets all its new nodes in the long one,
// while a roughly-uniform path gets nodes sprinkled around. Either way
// no two inserted nodes are "stacked" unless the starting geometry is
// degenerate.
//
// Atomic undo: one EditPathCommand per equalized side (typically just
// the shorter one; if counts already match, no command is pushed).
bool Canvas::equalize_blend_sources(SceneNode *a, SceneNode *b) {
  if (!a || !b || !a->path || !b->path || a->type != SceneNode::Type::Path ||
      b->type != SceneNode::Type::Path) {
    LOG_WARN("Canvas::equalize_blend_sources: invalid inputs");
    return false;
  }
  if (a->path->closed != b->path->closed) {
    LOG_WARN("Canvas::equalize_blend_sources: closed flag mismatch — "
             "equalization cannot resolve this");
    return false;
  }
  int na = (int)a->path->nodes.size();
  int nb = (int)b->path->nodes.size();
  if (na == nb)
    return true; // already equal

  SceneNode *target = (na < nb) ? a : b;
  int need = std::abs(na - nb);

  PathData before_pd = *target->path;
  BezierPath bp = BezierPath::from_path_data(*target->path);

  for (int iter = 0; iter < need; ++iter) {
    int segs = bp.segment_count();
    if (segs <= 0) {
      LOG_WARN("Canvas::equalize_blend_sources: zero-segment path — "
               "cannot split further");
      return false;
    }
    // Find segment with longest arc length.
    int best_i = 0;
    double best_len = -1.0;
    for (int i = 0; i < segs; ++i) {
      double len = bp.segment(i).length();
      if (len > best_len) {
        best_len = len;
        best_i = i;
      }
    }
    bp.insert_node_at(best_i, 0.5);
  }

  PathData after_pd = bp.to_path_data();
  *target->path = after_pd;

  if (m_history)
    m_history->push(std::make_unique<EditPathCommand>(
        project(), target->internal_id,
        std::move(before_pd), std::move(after_pd),
        "Equalize nodes for Blend"));

  LOG_INFO("Canvas: equalized path '{}' — inserted {} node(s), now {} nodes "
           "(match with other at {})",
           target->id, need, (int)target->path->nodes.size(),
           (na < nb) ? nb : na);
  return true;
}

// ── Canvas::rebuild_blend ────────────────────────────────────────────────────
// User-facing manual refresh. Sets blend_cache_dirty on the given Blend
// and requests a redraw. The existing lazy-rebuild in draw_object picks
// it up on the next frame. Safe to call with a null or non-Blend arg
// (logs warning, no-op) so context-menu handlers don't have to validate.
void Canvas::rebuild_blend(SceneNode *b) {
  if (!b)
    return;
  if (!b->is_blend()) {
    LOG_WARN("Canvas::rebuild_blend: node is not a Blend — ignoring");
    return;
  }
  b->blend_cache_dirty = true;
  queue_draw();
  LOG_INFO("Canvas::rebuild_blend: forced refresh on Blend '{}'", b->name);
}

// ── Canvas::release_blend ────────────────────────────────────────────────────
// Dissolves the currently-selected Blend into three siblings in its
// parent, z-order bottom→top:
//   [blend_idx + 0] = A (clone of blend_source_a)
//   [blend_idx + 1] = Group "Steps" containing clones of blend_cache
//   [blend_idx + 2] = B (clone of blend_source_b)
// The Blend node itself is removed. Selection becomes the three new
// siblings. Atomic undo via ReleaseBlendCommand.
//
// If blend_cache is empty (e.g. the Blend has never been rendered),
// the Steps Group is emitted empty — benign, user can delete it.
// Alternative would be to force a rebuild here, but that would require
// exposing rebuild_blend_cache (currently file-static). For M2 we keep
// the flow simple; M3 may add a guaranteed-fresh guarantee on release.
void Canvas::release_blend() {
  if (!m_doc || !m_selected || !m_selected->is_blend()) {
    LOG_INFO("Canvas::release_blend: ignored — selection is not a Blend");
    return;
  }

  int blend_idx = 0;
  SceneNode *parent = find_parent(m_doc, m_selected, &blend_idx);
  if (!parent) {
    LOG_WARN("Canvas::release_blend: could not find parent of Blend");
    return;
  }

  SceneNode *blend = m_selected;

  // Snapshot the Blend for undo BEFORE any mutation. Clone deep so the
  // snapshot carries A, B, and cache independently of the live node.
  auto blend_snap = clone_node(*blend);

  // Build the three result nodes in ascending z-order.
  std::vector<std::unique_ptr<SceneNode>> results;

  // A (bottom). Deep clone with a fresh id+iid so it doesn't alias the
  // internal slot clone if the Blend is ever restored via undo.
  auto a_out = clone_node(*blend->blend_source_a);
  a_out->id = next_id();
  a_out->internal_id = last_iid();
  results.push_back(std::move(a_out));

  // Steps Group — always emitted, even if cache is empty. Contains
  // clones of each cache entry, in panel [0]=top convention so the
  // topmost step is children[0]. Since cache is stored in render
  // order (bottom→top, matching A→B progression) we reverse on push
  // to land at [0]=top.
  auto steps_grp = std::make_unique<SceneNode>();
  steps_grp->type = SceneNode::Type::Group;
  steps_grp->id = next_id();
  steps_grp->internal_id = last_iid();
  steps_grp->name = m_doc->next_default_name(CurvzDocument::NameKind::Steps);
  steps_grp->visible = true;
  steps_grp->locked = false;
  steps_grp->opacity = 1.0;
  for (int i = (int)blend->blend_cache.size() - 1; i >= 0; --i) {
    auto step = clone_node(*blend->blend_cache[i]);
    step->id = next_id();
    step->internal_id = last_iid();
    step->locked = false; // steps are now independent, editable
    steps_grp->children.push_back(std::move(step));
  }
  results.push_back(std::move(steps_grp));

  // B (top).
  auto b_out = clone_node(*blend->blend_source_b);
  b_out->id = next_id();
  b_out->internal_id = last_iid();
  results.push_back(std::move(b_out));

  // Mutate the live tree: remove Blend, insert results in ascending
  // z-order at blend_idx. Keep pointers to the inserted nodes for
  // the new selection.
  parent->children.erase(parent->children.begin() + blend_idx);
  std::vector<SceneNode *> new_sel;
  for (int i = 0; i < (int)results.size(); ++i) {
    int ins = blend_idx + i;
    ins = std::clamp(ins, 0, (int)parent->children.size());
    auto *ptr = results[i].get();
    parent->children.insert(parent->children.begin() + ins,
                            clone_node(*results[i]));
    // After insert, the live node is at ins; results[i] itself is still
    // the source-of-truth copy we'll hand to the command.
    new_sel.push_back(parent->children[ins].get());
    (void)ptr;
  }

  if (m_history)
    m_history->push(std::make_unique<ReleaseBlendCommand>(
        project(), parent->internal_id,
        std::move(blend_snap), std::move(results), blend_idx));

  m_selection = new_sel;
  m_selected = new_sel.empty() ? nullptr : new_sel.front();
  m_selected_node = -1;
  m_node_selection.clear();

  notify_object_selection_changed();
  notify_node_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: released Blend → A + Steps + B (3 siblings)");
}

void Canvas::group_selection() {
  if (!m_doc || m_selection.size() < 2)
    return;

  // Find parent layer of first selected object
  int dummy;
  SceneNode *parent = find_parent(m_doc, m_selection.front(), &dummy);
  if (!parent)
    return;

  // Generate group name via the document funnel (gap-fill, document-wide
  // unique). The SVG `id` is now derived from name + iid by SvgWriter, so
  // we no longer need to set group->id explicitly here.
  std::string gname = m_doc->next_default_name(CurvzDocument::NameKind::Group);

  // Create the group
  auto group = std::make_unique<SceneNode>();
  group->type = SceneNode::Type::Group;
  group->internal_id = generate_internal_id();
  group->name = gname;

  // Find the highest z-position (lowest index) of any selected object in parent
  int insert_idx = (int)parent->children.size();
  for (SceneNode *obj : m_selection) {
    for (int i = 0; i < (int)parent->children.size(); ++i) {
      if (parent->children[i].get() == obj)
        insert_idx = std::min(insert_idx, i);
    }
  }

  // Move selected objects into group in PARENT Z-ORDER, not selection order.
  //
  // Pre-fix: the loop iterated m_selection, which is the order the user
  // clicked. Selecting a back object first then a top object produced a
  // group whose children were [back, top] — visually fine, but if the user
  // had clicked the top first then the back, the group came out [top, back],
  // reversing the layer's stacking. Same input geometry, different group
  // because of click order. Comment said "in original order" but the
  // author meant "in user's click order" which is not what feels original
  // to the user.
  //
  // Fix: walk parent->children once and pluck anything that's in the
  // selection. This preserves the layer's existing z-relationship inside
  // the group regardless of how the user assembled the selection. Same
  // pattern make_compound_path already uses (Canvas.cpp ~3357 — Phase 3
  // builds the compound in parent z-order).
  std::set<SceneNode *> sel_set(m_selection.begin(), m_selection.end());
  for (auto it = parent->children.begin(); it != parent->children.end();) {
    if (sel_set.count(it->get())) {
      group->children.push_back(std::move(*it));
      it = parent->children.erase(it);
    } else {
      ++it;
    }
  }

  // Insert group at the position of the topmost selected object
  insert_idx = std::min(insert_idx, (int)parent->children.size());
  SceneNode *gptr = group.get();
  parent->children.insert(parent->children.begin() + insert_idx,
                          std::move(group));

  // Select the new group
  m_selected = gptr;
  m_selection = {gptr};
  notify_object_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  {
    MacroStep s;
    s.op = MacroStep::Op::Group;
    record_step_if_recording(s);
  }
  LOG_INFO("Canvas: grouped {} objects → '{}'", m_selection.size(), gname);
}

void Canvas::ungroup_selection() {
  if (!m_doc || !m_selected || m_selected->type != SceneNode::Type::Group)
    return;

  int group_idx;
  SceneNode *parent = find_parent(m_doc, m_selected, &group_idx);
  if (!parent)
    return;

  // Collect children to move out
  std::vector<std::unique_ptr<SceneNode>> children;
  for (auto &child : m_selected->children)
    children.push_back(std::move(child));

  // Remove the group from parent
  parent->children.erase(parent->children.begin() + group_idx);

  // Insert children at the group's position
  std::vector<SceneNode *> new_selection;
  for (int i = (int)children.size() - 1; i >= 0; --i) {
    SceneNode *ptr = children[i].get();
    new_selection.push_back(ptr);
    parent->children.insert(parent->children.begin() + group_idx,
                            std::move(children[i]));
  }

  // Select the ungrouped objects
  m_selection = new_selection;
  m_selected = m_selection.empty() ? nullptr : m_selection.front();
  notify_object_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  {
    MacroStep s;
    s.op = MacroStep::Op::Ungroup;
    record_step_if_recording(s);
  }
  LOG_INFO("Canvas: ungrouped → {} objects", new_selection.size());
}

// ── align_anchor (validator-on-read)
// ─────────────────────────────────────────────
// Self-cleaning gate for m_align_anchor. The anchor is meaningful only
// while the marked object is still part of the current selection; the
// moment it leaves (Esc, replace, delete, Shift-toggle, marquee replace,
// undo redo, etc.) the stored pointer is stale. Rather than instrument
// every selection-mutation site to clear the anchor (that's the ~40+
// m_selection writes scattered across this file — the procedural fix),
// gate every read through this function (the structural fix). All
// consumers — align_selection, the canvas glyph render, future toolbar
// status — call align_anchor() instead of touching the member, and the
// invariant "anchor is valid or null" is preserved automatically.
SceneNode *Canvas::align_anchor() {
  if (!m_align_anchor)
    return nullptr;
  if (std::find(m_selection.begin(), m_selection.end(), m_align_anchor) ==
      m_selection.end()) {
    m_align_anchor = nullptr;
    return nullptr;
  }
  return m_align_anchor;
}

// ── align_selection
// ───────────────────────────────────────────────────────────
void Canvas::align_selection(AlignOp op) {
  if (m_selection.size() < 2)
    return;

  // ── 1. Separate ref points from regular objects ───────────────────────
  // If ref points are in the selection they act as immovable alignment anchors.
  // The target bbox comes from ref points alone; non-ref objects align to it.
  // If no ref points, fall through to normal union-bbox behavior.
  struct ObjBB {
    SceneNode *obj;
    BBox bb;
  };
  std::vector<ObjBB> items;     // non-ref objects to move
  std::vector<ObjBB> ref_items; // ref points (anchor, never moved)
  items.reserve(m_selection.size());

  for (SceneNode *obj : m_selection) {
    if (obj->is_ref()) {
      // Ref point — treat as a zero-size bbox at its position
      BBox bb{obj->ref_x, obj->ref_y, 0.0, 0.0};
      ref_items.push_back({obj, bb});
      continue;
    }
    auto bb = object_bbox(*obj);
    if (!bb)
      continue;
    items.push_back({obj, *bb});
  }
  if (items.empty())
    return;

  // ── 1.5. Locate align anchor (selection-time key object) ──────────────
  // Validator-on-read: align_anchor() returns the anchor SceneNode* only
  // if it's still in m_selection, otherwise self-clears. Anchor is
  // considered only for Align ops; Distribute ignores it entirely
  // (matches Affinity — distribute geometry has no "anchor" semantics).
  // Anchor wins over ref-points (selection-time user intent overrides
  // persistent ref-point geometry for the duration of one click).
  // Find the anchor's index in items so its dx/dy can be pinned to zero.
  SceneNode *anchor = nullptr;
  size_t anchor_idx = items.size(); // sentinel: "not found"
  bool is_distribute =
      (op == AlignOp::DistributeH || op == AlignOp::DistributeV);
  if (!is_distribute) {
    if (SceneNode *a = align_anchor()) {
      for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].obj == a) {
          anchor = a;
          anchor_idx = i;
          break;
        }
      }
      // Anchor present in selection but is itself a ref-point (zero-size
      // bbox stored in ref_items, not items): fall through to the normal
      // ref-points-as-target path. The visible behaviour is identical
      // (ref point already pins the target) so no special handling needed.
    }
  }

  // ── 2. Compute alignment target bbox ─────────────────────────────────
  // Priority for align ops: anchor > ref points > union bbox.
  // Distribute always uses the union span.
  double ux1, uy1, ux2, uy2;

  if (anchor) {
    // Target = anchor's bbox alone. Anchor stays put; others align to it.
    const BBox &abb = items[anchor_idx].bb;
    ux1 = abb.x;
    uy1 = abb.y;
    ux2 = abb.x + abb.w;
    uy2 = abb.y + abb.h;
  } else if (!ref_items.empty()) {
    // Target from ref points
    ux1 = ref_items[0].bb.x;
    uy1 = ref_items[0].bb.y;
    ux2 = ux1;
    uy2 = uy1;
    for (auto &r : ref_items) {
      ux1 = std::min(ux1, r.bb.x);
      uy1 = std::min(uy1, r.bb.y);
      ux2 = std::max(ux2, r.bb.x);
      uy2 = std::max(uy2, r.bb.y);
    }
    // Include the movable items in union only for distribute ops
    if (op == AlignOp::DistributeH || op == AlignOp::DistributeV) {
      for (auto &it : items) {
        ux1 = std::min(ux1, it.bb.x);
        uy1 = std::min(uy1, it.bb.y);
        ux2 = std::max(ux2, it.bb.x + it.bb.w);
        uy2 = std::max(uy2, it.bb.y + it.bb.h);
      }
    }
  } else {
    // Normal: union of all items
    if (items.size() < 2)
      return;
    ux1 = items[0].bb.x;
    uy1 = items[0].bb.y;
    ux2 = items[0].bb.x + items[0].bb.w;
    uy2 = items[0].bb.y + items[0].bb.h;
    for (auto &it : items) {
      ux1 = std::min(ux1, it.bb.x);
      uy1 = std::min(uy1, it.bb.y);
      ux2 = std::max(ux2, it.bb.x + it.bb.w);
      uy2 = std::max(uy2, it.bb.y + it.bb.h);
    }
  }
  double ucx = (ux1 + ux2) * 0.5;
  double ucy = (uy1 + uy2) * 0.5;

  // ── 3. Collect all leaf paths before mutation (for undo snapshots) ────
  std::vector<AlignObjectsCommand::LeafSnap> snaps;

  // Distribute: needs objects sorted; compute once.
  std::vector<size_t> sorted_idx(items.size());
  std::iota(sorted_idx.begin(), sorted_idx.end(), 0);

  if (op == AlignOp::DistributeH || op == AlignOp::DistributeV) {
    std::sort(sorted_idx.begin(), sorted_idx.end(), [&](size_t a, size_t b) {
      return (op == AlignOp::DistributeH) ? items[a].bb.x < items[b].bb.x
                                          : items[a].bb.y < items[b].bb.y;
    });
  }

  std::vector<double> dx(items.size(), 0.0), dy(items.size(), 0.0);

  if (op == AlignOp::DistributeH || op == AlignOp::DistributeV) {
    int N = (int)sorted_idx.size();
    if (op == AlignOp::DistributeH) {
      size_t fi = sorted_idx.front(), li = sorted_idx.back();
      double span = (items[li].bb.x + items[li].bb.w) - items[fi].bb.x;
      double total_w = 0;
      for (size_t i : sorted_idx)
        total_w += items[i].bb.w;
      double total_gap = span - total_w;
      double gap = (N > 1) ? total_gap / (N - 1) : 0.0;
      double cursor = items[fi].bb.x + items[fi].bb.w + gap;
      for (int k = 1; k < N - 1; ++k) {
        size_t idx = sorted_idx[k];
        dx[idx] = cursor - items[idx].bb.x;
        cursor += items[idx].bb.w + gap;
      }
    } else {
      size_t fi = sorted_idx.front(), li = sorted_idx.back();
      double span = (items[li].bb.y + items[li].bb.h) - items[fi].bb.y;
      double total_h = 0;
      for (size_t i : sorted_idx)
        total_h += items[i].bb.h;
      double total_gap = span - total_h;
      double gap = (N > 1) ? total_gap / (N - 1) : 0.0;
      double cursor = items[fi].bb.y + items[fi].bb.h + gap;
      for (int k = 1; k < N - 1; ++k) {
        size_t idx = sorted_idx[k];
        dy[idx] = cursor - items[idx].bb.y;
        cursor += items[idx].bb.h + gap;
      }
    }
  } else {
    for (size_t i = 0; i < items.size(); ++i) {
      // Anchor is the alignment target — its bbox IS ux1..ux2, uy1..uy2.
      // Computing dx/dy on it yields zero in the perfect-arithmetic case,
      // but we skip explicitly so floating-point dust never nudges the
      // anchor. Matches the ref-points-don't-move guarantee in shape.
      if (i == anchor_idx)
        continue;
      const BBox &bb = items[i].bb;
      switch (op) {
      case AlignOp::AlignLeft:
        dx[i] = ux1 - bb.x;
        break;
      case AlignOp::AlignCenterH:
        dx[i] = ucx - (bb.x + bb.w * 0.5);
        break;
      case AlignOp::AlignRight:
        dx[i] = ux2 - (bb.x + bb.w);
        break;
      case AlignOp::AlignTop:
        dy[i] = uy1 - bb.y;
        break;
      case AlignOp::AlignCenterV:
        dy[i] = ucy - (bb.y + bb.h * 0.5);
        break;
      case AlignOp::AlignBottom:
        dy[i] = uy2 - (bb.y + bb.h);
        break;
      default:
        break;
      }
    }
  }

  // ── 4. Snapshot leaves, apply translation, snapshot after ─────────────
  // Ref points are never moved.
  for (size_t i = 0; i < items.size(); ++i) {
    double ddx = dx[i], ddy = dy[i];
    if (std::abs(ddx) < 1e-9 && std::abs(ddy) < 1e-9)
      continue;

    std::vector<SceneNode *> leaves;
    collect_paths(items[i].obj, leaves);
    for (SceneNode *leaf : leaves) {
      if (!leaf->path)
        continue;
      PathData before = *leaf->path;
      for (auto &n : leaf->path->nodes) {
        n.x += ddx;
        n.y += ddy;
        n.cx1 += ddx;
        n.cy1 += ddy;
        n.cx2 += ddx;
        n.cy2 += ddy;
      }
      snaps.push_back({leaf->internal_id, std::move(before), *leaf->path});
    }
  }

  if (snaps.empty())
    return;

  // ── 4. Push undo + signal redraw ──────────────────────────────────────
  static const char *op_names[] = {
      "Align left",     "Align center H", "Align right",  "Align top",
      "Align center V", "Align bottom",   "Distribute H", "Distribute V"};
  std::string desc = op_names[static_cast<int>(op)];

  if (m_history)
    m_history->push(
        std::make_unique<AlignObjectsCommand>(
            project(), std::move(snaps), desc));

  // ── Record macro step ─────────────────────────────────────────────────
  {
    MacroStep s;
    switch (op) {
    case AlignOp::AlignLeft:
      s.op = MacroStep::Op::AlignLeft;
      break;
    case AlignOp::AlignCenterH:
      s.op = MacroStep::Op::AlignCenterH;
      break;
    case AlignOp::AlignRight:
      s.op = MacroStep::Op::AlignRight;
      break;
    case AlignOp::AlignTop:
      s.op = MacroStep::Op::AlignTop;
      break;
    case AlignOp::AlignCenterV:
      s.op = MacroStep::Op::AlignMiddleV;
      break;
    case AlignOp::AlignBottom:
      s.op = MacroStep::Op::AlignBottom;
      break;
    case AlignOp::DistributeH:
      s.op = MacroStep::Op::DistributeH;
      break;
    case AlignOp::DistributeV:
      s.op = MacroStep::Op::DistributeV;
      break;
    }
    record_step_if_recording(s);
  }

  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: {}", desc);
}

// ── boolean_op
// ────────────────────────────────────────────────────────────────
void Canvas::boolean_op(BooleanOpType op) {
  if (!m_doc)
    return;

  const char *op_name = (op == BooleanOpType::Union)      ? "Union"
                        : (op == BooleanOpType::Subtract) ? "Subtract"
                                                          : "Intersect";

  // s122 m3: boolean ops require AT LEAST 2 selected objects. The N-way
  // path goes through Clipper2 in a single call (or, for Intersect, an
  // associative iteration that monotonically shrinks). The S58b "exactly
  // 2" gate is gone — Clipper2's native N-way Union replaces the previous
  // hand-rolled fold that corrupted on disjoint intermediate results.
  // Subtract follows the Affinity / Illustrator convention: bottommost
  // (lowest-Z) operand minus the union of the rest.
  if (m_selection.size() < 2) {
    std::string msg = "Boolean " + std::string(op_name) +
                      " requires at least 2 selected objects.\n\n"
                      "Select 2 or more closed paths or compounds to combine.";
    m_sig_show_message.emit("Boolean Operation Failed", msg);
    LOG_WARN("Canvas: boolean_op requires >=2 selected objects (got {})",
             m_selection.size());
    return;
  }

  // ── Validate: collect closed path objects sharing one parent ─────────────
  // Each candidate must be a Path (or Compound of paths) in the same
  // parent. Open paths are skipped with a warning; if fewer than 2 valid
  // candidates remain after filtering, abort.

  struct Candidate {
    SceneNode *node;
    SceneNode *parent;
    int index;
  };
  std::vector<Candidate> candidates;
  int skipped_open = 0;
  int skipped_nopath = 0;

  SceneNode *common_parent = nullptr;

  for (SceneNode *obj : m_selection) {
    // S58b Ship 2: Compound operands are handled by the Clipper2 engine.
    const bool is_path = obj->path != nullptr;
    const bool is_compound =
        obj->type == SceneNode::Type::Compound && !obj->children.empty();
    if (!is_path && !is_compound) {
      ++skipped_nopath;
      continue;
    }

    int idx = -1;
    SceneNode *par = find_parent(m_doc, obj, &idx);
    if (!par)
      continue;

    if (!common_parent)
      common_parent = par;
    if (par != common_parent) {
      LOG_WARN("Canvas: boolean_op — objects must be in the same layer/group");
      m_sig_show_message.emit(
          "Boolean Operation Failed",
          "All selected objects must be in the same layer or group.");
      return;
    }

    // Closedness check:
    //  - Path: obj->path->closed.
    //  - Compound: require every child path to be closed.
    bool all_closed = true;
    if (is_path) {
      all_closed = obj->path->closed;
    } else {
      for (auto &ch : obj->children) {
        if (!ch->path || !ch->path->closed) {
          all_closed = false;
          break;
        }
      }
    }
    if (!all_closed) {
      ++skipped_open;
      continue;
    }

    candidates.push_back({obj, par, idx});
  }

  if (candidates.size() < 2) {
    std::string msg =
        "Boolean " + std::string(op_name) + " requires at least 2 closed paths.";
    if (skipped_open > 0)
      msg +=
          "\n" + std::to_string(skipped_open) + " open path(s) were skipped.";
    if (skipped_nopath > 0)
      msg += "\n" + std::to_string(skipped_nopath) +
             " non-path object(s) were skipped.";
    m_sig_show_message.emit("Boolean Operation Failed", msg);
    return;
  }

  // ── Sort candidates by layer index DESCENDING ───────────────────────────
  //
  // Convention in this codebase (see paste handler at ~line 2862,
  // "Insert into target layer (front = top of layer)"): new SceneNodes
  // are inserted at children.begin() — i.e. the **top of the LayersPanel
  // is index 0**, and higher indices are progressively older / further
  // back in z-order. So children[0] is the most recently added shape,
  // visually on top of everything below it; children.back() is the
  // foundational, oldest, back-most shape in the layer.
  //
  // For boolean ops, the user's mental model is "the back-most thing is
  // the subject (the dough); the front-most thing is the operator (the
  // cookie cutter pressed down through it)" — Affinity / Illustrator
  // convention. Subject = highest index; operator(s) = lower indices.
  //
  // Sort descending so candidates[0] is the highest index = back-most
  // = subject. The Clipper2 wrapper takes operands[0] as subject for
  // Subtract; for Union and Intersect order doesn't affect geometry but
  // does affect style donation (operands[0] donates fill/stroke).
  //
  // (Earlier versions of this code sorted ascending and called the
  // result "lowest-Z" — wrong, because in this codebase index 0 is
  // front-most, not back-most. Symptom: little-oval-inside-big-rect
  // Subtract produced an empty result because the oval was being
  // treated as subject.)
  std::sort(
      candidates.begin(), candidates.end(),
      [](const Candidate &a, const Candidate &b) { return a.index > b.index; });

  SceneNode *parent = common_parent;
  // Result lands at the highest index = back-most = where the subject
  // was. After the descending erase loop below shifts everything, the
  // clamp ensures we're never out of bounds.
  int insert_idx = candidates[0].index;

  // ── Snapshot all originals for undo before any mutation ──────────────────
  std::vector<BooleanOpCommand::Original> originals;
  for (auto &c : candidates)
    originals.push_back({clone_node(*c.node), c.index});

  // ── Build per-operand subpath vectors ────────────────────────────────────
  // Walk all candidates (sorted descending = back-to-front), produce one
  // subpath vector per operand. operand[0] is the back-most shape (the
  // foundational subject for Subtract; the style donor for all ops).
  // A simple Path → 1 subpath. A Compound → N subpaths, one per child.
  auto collect_subpaths = [](SceneNode *n) -> std::vector<BezierPath> {
    std::vector<BezierPath> out;
    if (n->path) {
      out.push_back(BezierPath::from_path_data(*n->path));
    } else if (n->type == SceneNode::Type::Compound) {
      for (auto &ch : n->children) {
        if (ch && ch->path) {
          out.push_back(BezierPath::from_path_data(*ch->path));
        }
      }
    }
    return out;
  };

  // Style source: candidates[0] is the back-most candidate (the subject)
  // by the descending sort above. For a Path candidate that's it; for a
  // Compound candidate we walk into its bottom-most child (Compound's
  // own fill/stroke are ignored by rendering convention — see SceneNode
  // comment).
  //
  // s122 m5: capture the style as VALUES here, before the originals are
  // erased below. Holding `style_source` as a raw pointer through the
  // erase would dangle — for Compound candidates, style_source pointed
  // at an interior child whose owner unique_ptr is destroyed when the
  // candidate is erased from parent->children. Reading fill/stroke off
  // that pointer in the result-build branches segfaulted on
  // compound+primitive Union (heap reuse made it data-dependent).
  // Same precedent: expand_stroke_op already does this pattern.
  SceneNode *style_source = candidates[0].node;
  if (style_source->type == SceneNode::Type::Compound &&
      !style_source->children.empty() && style_source->children.back()) {
    style_source = style_source->children.back().get();
  }
  const FillStyle   saved_fill    = style_source->fill;
  const StrokeStyle saved_stroke  = style_source->stroke;
  const double      saved_opacity = style_source->opacity;
  // style_source pointer is unused after this point — use the saved_*
  // values in the result-build branches below.

  // ── s181 m4: per-child dispatch for Compound + Path ─────────────────────────
  // Clipper2 with EvenOdd does not preserve operand role across multi-subpath
  // input — it sees N peer subpaths and computes N-way EvenOdd, which gives
  // the wrong answer when one operand is a compound path expressing
  // outer-minus-hole semantics. Diagnosed via s181 m3 logs: a compound
  // (rect-with-hole) Unioned with a yellow rect produced 3 disjoint output
  // pieces because Clipper2 treated the hole as a positive peer region
  // rather than a subtractive sub-region of the compound.
  //
  // Fix: when the selection is exactly one Compound + one Path, decompose
  // the Compound into its children and run a per-child boolean against the
  // Path operand. Role assignment is by INDEX FROM THE BOTTOM of the
  // children stack:
  //   distance_from_bottom = 0  → OUTER role  (union with opY)
  //   distance_from_bottom = 1  → HOLE  role  (subtract opY)
  //   distance_from_bottom = 2  → island       (union with opY)
  //   ...
  // i.e. the visually-bottom child of the compound is the foundational
  // outer; everything stacked above alternates. The user owns role by
  // stacking order in the layers panel.
  //
  // Per-role op dispatch table (unchanged from m4):
  //                   Outer            Hole
  //   Union           outer ∪ Y        hole ⊖ Y
  //   Subtract        outer ⊖ Y        hole ∪ Y
  //   Intersect       outer ∩ Y        hole ∩ Y
  //
  // Result handling: each per-child Clipper2 call produces 0+ loops. All
  // loops from one input child become children at that input child's slot
  // in the rebuilt compound (multi-loop ops expand inline). The opY
  // operand is consumed: removed from parent.
  //
  // Wrap-up: the rebuilt compound replaces the original compound at its
  // original index. Original compound + original opY are captured in
  // the BooleanOpCommand undo snapshot; the rebuilt compound is the
  // result snapshot. Atomic undo restores both.
  bool used_per_child_dispatch = false;
  if (candidates.size() == 2) {
    SceneNode *cand_a = candidates[0].node;
    SceneNode *cand_b = candidates[1].node;
    const bool a_is_compound = (cand_a->type == SceneNode::Type::Compound);
    const bool b_is_compound = (cand_b->type == SceneNode::Type::Compound);
    const bool a_is_path     = (cand_a->path != nullptr);
    const bool b_is_path     = (cand_b->path != nullptr);
    if ((a_is_compound && b_is_path) || (b_is_compound && a_is_path)) {
      SceneNode *cmp_node = a_is_compound ? cand_a : cand_b;
      SceneNode *path_node = a_is_compound ? cand_b : cand_a;
      LOG_INFO("Canvas: boolean {} — s181 m4 per-child dispatch "
               "(compound={} children, +path)",
               op_name, cmp_node->children.size());

      // Cache opY once.
      std::vector<BezierPath> opY_subpaths;
      opY_subpaths.push_back(BezierPath::from_path_data(*path_node->path));

      // Walk children TOP→BOTTOM in index order (i = 0..N-1), but the
      // role index counts FROM THE BOTTOM. So distance_from_bottom = (N-1)-i.
      const size_t N = cmp_node->children.size();

      // Build the rebuilt compound's new children list. Walk in original
      // children order so multi-loop expansions land inline at the right
      // slot.
      std::vector<std::unique_ptr<SceneNode>> new_children;
      new_children.reserve(N);

      for (size_t ci = 0; ci < N; ++ci) {
        SceneNode *child = cmp_node->children[ci].get();
        if (!child || !child->path || child->path->nodes.size() < 3) {
          // Defensive: keep degenerate children verbatim (clone forward).
          new_children.push_back(clone_node(*child));
          continue;
        }

        // dfb = distance from bottom of compound stack.
        //
        // Per-child dispatch rule, per user-op:
        //   user_op = Union     → dfb=0: Union(obj0, opY);  rest: Subtract(_, opY)
        //   user_op = Subtract  → every child: Subtract(_, opY)  (uniform — opY cuts all)
        //   user_op = Intersect → every child: Intersect(_, opY) (uniform — opY keeps overlap)
        //
        // Only Union has a special-case at dfb=0: opY's additive behaviour
        // only makes sense for the foundational outer; everywhere else opY
        // can only be "cut out" because we'd otherwise duplicate opY's
        // contribution. Subtract and Intersect are uniform — the user-op's
        // verb applies the same to every child.
        //
        // Each child keeps its slot in the recomposed compound. Compound
        // structure is preserved (children may expand to multiple pieces
        // if a per-child op produces disjoint output, or drop entirely
        // if the op produces empty).
        size_t dfb = (N - 1) - ci;
        BooleanOpType per_op;
        switch (op) {
          case BooleanOpType::Union:
            per_op = (dfb == 0) ? BooleanOpType::Union
                                : BooleanOpType::Subtract;
            break;
          case BooleanOpType::Subtract:
            per_op = BooleanOpType::Subtract;  // uniform
            break;
          case BooleanOpType::Intersect:
            per_op = BooleanOpType::Intersect;  // uniform
            break;
          default:
            per_op = op;
        }
        // is_outer retained for log output / clarity — the dfb==0 child is
        // the "outer," everything above is structurally a hole or island.
        bool is_outer = (dfb == 0);

        // Build 2-operand vector for this per-child call.
        // s181 m4 fix: BOTH operands must share consistent winding before
        // Clipper2. Two-operand calls with one subpath each fall through
        // to the NonZero fill-rule heuristic in BooleanOpsClipper.cpp —
        // and NonZero with opposite-wound subpaths cancels in their
        // overlap, producing XOR-like geometry instead of the intended op.
        //
        // Winding direction is op-dependent (empirical):
        //   per_op = Subtract  → both operands CW
        //   per_op = Union     → both operands CCW
        //   per_op = Intersect → both operands CCW (assume Union-side
        //                        until proven otherwise)
        const bool want_ccw = (per_op != BooleanOpType::Subtract);
        auto force_dir = [want_ccw](BezierPath bp) -> BezierPath {
          double sa = bp.signed_area();
          // Y-down doc: signed_area < 0 = CCW, signed_area > 0 = CW.
          bool is_ccw = (sa < 0.0);
          if (is_ccw != want_ccw) bp.reverse();
          return bp;
        };
        std::vector<std::vector<BezierPath>> per_operands;
        per_operands.reserve(2);
        std::vector<BezierPath> child_subpaths;
        child_subpaths.push_back(force_dir(BezierPath::from_path_data(*child->path)));
        per_operands.push_back(std::move(child_subpaths));
        std::vector<BezierPath> opY_dir;
        opY_dir.reserve(opY_subpaths.size());
        for (const auto &bp : opY_subpaths) opY_dir.push_back(force_dir(bp));
        per_operands.push_back(std::move(opY_dir));

        LOG_INFO("Canvas: boolean {} — per-child[{}] dfb={} role={} per_op={}",
                 op_name, ci, dfb, is_outer ? "OUTER" : "HOLE",
                 (per_op == BooleanOpType::Union)     ? "Union"
                 : (per_op == BooleanOpType::Subtract) ? "Subtract"
                                                       : "Intersect");

        // Run enrichment + Clipper2 + cleanup for this child against opY.
        int q_pc = m_boolean_cleanup_quality;
        if (q_pc < 0)  q_pc = 0;
        if (q_pc > 10) q_pc = 10;
        const bool pc_cleanup_on = (q_pc < 10);
        double pc_apex = 0.0;
        int    pc_run  = 0;
        if (pc_cleanup_on) {
          if (q_pc <= 5) {
            const double t = double(q_pc) / 5.0;
            pc_apex = 30.0 - 15.0 * t;
            pc_run  = static_cast<int>(std::lround(20.0 - 10.0 * t));
          } else {
            const double t = double(q_pc - 5) / 4.0;
            pc_apex = 15.0 - 10.0 * t;
            pc_run  = static_cast<int>(std::lround(10.0 - 5.0 * t));
          }
        }
        std::vector<std::vector<BezierPath>> pc_enriched;
        Curvz::refit::KeeperSet pc_keepers;
        const auto *pc_for_clipper = &per_operands;
        if (pc_cleanup_on) {
          pc_keepers = Curvz::refit::enrich_at_intersections_and_build_keepers(
              per_operands, pc_enriched, per_op);
          pc_for_clipper = &pc_enriched;
        }
        std::vector<PathData> pc_loops =
            Curvz::boolean_op_clipper(*pc_for_clipper, per_op);
        if (pc_cleanup_on && !pc_loops.empty()) {
          for (auto &loop : pc_loops) {
            loop = Curvz::refit::cleanup_loop_v4(std::move(loop), pc_keepers,
                                                 pc_apex, pc_run);
          }
        }

        LOG_INFO("Canvas: boolean {} — per-child[{}] produced {} loop(s)",
                 op_name, ci, pc_loops.size());

        // Empty result → drop this child entirely (skip).
        // Single result → one new child, mirroring style of the original.
        // Multi result → multiple new children, all mirroring style.
        for (auto &loop : pc_loops) {
          if (loop.nodes.size() < 3) continue;
          auto new_child = std::make_unique<SceneNode>();
          new_child->type = SceneNode::Type::Path;
          new_child->id = next_id();
          new_child->internal_id = last_iid();
          new_child->name = child->name;       // preserve original name
          new_child->fill = child->fill;
          new_child->stroke = child->stroke;
          new_child->opacity = child->opacity;
          new_child->visible = child->visible;
          new_child->locked = child->locked;
          new_child->path = std::make_unique<PathData>(std::move(loop));
          new_children.push_back(std::move(new_child));
        }
      }

      // Build the result compound. If new_children is empty, the
      // operation effectively erased the compound — degrade to the
      // single-loop / empty branches as the existing flow would.
      if (new_children.empty()) {
        LOG_WARN("Canvas: boolean {} — per-child dispatch produced empty result",
                 op_name);
        m_sig_show_message.emit(
            "Boolean Operation Failed",
            std::string(op_name) +
                " produced an empty result. No changes were made.");
        return;
      }

      // Snapshot originals: both the compound and the path operand. The
      // compound goes in at its index, the path at its index, sorted by
      // BooleanOpCommand convention (ascending).
      std::vector<BooleanOpCommand::Original> per_originals;
      for (auto &c : candidates) {
        per_originals.push_back({clone_node(*c.node), c.index});
      }
      std::sort(per_originals.begin(), per_originals.end(),
                [](const BooleanOpCommand::Original &a,
                   const BooleanOpCommand::Original &b) {
                  return a.index < b.index;
                });

      // Build the result compound in place. Single child means we collapse
      // to a Path; multi means a Compound. Mirrors the existing branch.
      std::unique_ptr<SceneNode> result_node;
      if (new_children.size() == 1) {
        result_node = std::move(new_children[0]);
        result_node->name = std::string(op_name);
      } else {
        result_node = std::make_unique<SceneNode>();
        result_node->type = SceneNode::Type::Compound;
        result_node->id = next_id();
        result_node->internal_id = last_iid();
        result_node->name = std::string(op_name);
        result_node->fill = saved_fill;
        result_node->stroke = saved_stroke;
        result_node->opacity = saved_opacity;
        result_node->visible = true;
        result_node->locked = false;
        for (auto &nc : new_children) {
          result_node->children.push_back(std::move(nc));
        }
      }

      // Find the parent insert position — same logic as existing flow:
      // result lands at the highest index = back-most slot. After erasing
      // both originals (descending), the clamp ensures we're in bounds.
      int per_insert_idx = candidates[0].index;

      // Remove both originals from parent (descending order).
      std::vector<int> per_indices;
      for (auto &c : candidates) per_indices.push_back(c.index);
      std::sort(per_indices.rbegin(), per_indices.rend());
      for (int idx : per_indices)
        parent->children.erase(parent->children.begin() + idx);

      // Insert the result at the clamped position.
      int per_ins = std::clamp(per_insert_idx, 0,
                                (int)parent->children.size());
      SceneNode *result_raw = result_node.get();
      parent->children.insert(parent->children.begin() + per_ins,
                              std::move(result_node));

      // Build result snapshot for undo.
      std::vector<std::unique_ptr<SceneNode>> per_result_snaps;
      per_result_snaps.push_back(clone_node(*parent->children[per_ins]));

      // Push atomic undo command.
      if (m_history)
        m_history->push(std::make_unique<BooleanOpCommand>(
            project(), parent->internal_id,
            std::move(per_originals), std::move(per_result_snaps),
            per_insert_idx, std::string(op_name)));

      // Notify if any objects were skipped during validation.
      if (skipped_open > 0 || skipped_nopath > 0) {
        std::string msg;
        if (skipped_open > 0)
          msg += std::to_string(skipped_open) + " open path(s) were skipped.\n";
        if (skipped_nopath > 0)
          msg += std::to_string(skipped_nopath) +
                 " non-path object(s) were skipped.\n";
        msg += "The operation was performed on the remaining closed paths.";
        m_sig_show_message.emit("Boolean " + std::string(op_name), msg);
      }

      // Selection update — point at the new result.
      m_selected = result_raw;
      m_selection.clear();
      m_selection.push_back(result_raw);
      m_selected_node = -1;
      m_node_selection.clear();

      LOG_INFO("Canvas: boolean {} — per-child dispatch DONE, "
               "result has {} children",
               op_name, result_raw->children.size());

      notify_object_selection_changed();
      notify_node_selection_changed();
      m_sig_doc_changed.emit();
      queue_draw();

      // s181 m4: Macro recording — same op type, same recording machinery.
      // We don't enter the existing post-Clipper2 flow at all; the recording
      // sites past this point would never see the op. Mirror them here.
      MacroStep s_rec;
      switch (op) {
        case BooleanOpType::Union:     s_rec.op = MacroStep::Op::BooleanUnion; break;
        case BooleanOpType::Subtract:  s_rec.op = MacroStep::Op::BooleanSubtract; break;
        case BooleanOpType::Intersect: s_rec.op = MacroStep::Op::BooleanIntersect; break;
      }
      record_step_if_recording(s_rec);

      used_per_child_dispatch = true;
      return;  // ← skip the existing N-way Clipper2 flow entirely
    }
  }
  // ── End s181 m4 per-child dispatch branch ───────────────────────────────────
  // If we get here, used_per_child_dispatch was false — fall through to the
  // existing N-way Clipper2 path that handles Path+Path, Compound+Compound,
  // and N>=3 cases. (Dispatch path early-returns above.)

  std::vector<std::vector<BezierPath>> operands;
  operands.reserve(candidates.size());
  for (auto &c : candidates) {
    auto sub = collect_subpaths(c.node);
    if (sub.empty()) {
      LOG_WARN("Canvas: boolean {} — candidate produced 0 subpaths", op_name);
      m_sig_show_message.emit(
          "Boolean Operation Failed",
          "One of the selected objects produced no usable geometry.");
      return;
    }
    operands.push_back(std::move(sub));
  }

  // s181 m4 fix: force every operand subpath to CCW before handing to
  // Clipper2. The NonZero fill-rule path (binary 2-subpath calls) cancels
  // opposite-wound operands in their overlap regions, producing XOR-like
  // geometry instead of Union/Subtract/Intersect. EvenOdd (multi-subpath)
  // is winding-invariant and unaffected, but the same per-operand
  // canonicalisation is harmless there. Path-by-path: signed_area > 0 in
  // doc Y-down means CW; reverse to flip to CCW.
  for (auto &op_subpaths : operands) {
    for (auto &bp : op_subpaths) {
      if (bp.signed_area() > 0.0) bp.reverse();
    }
  }

  // ── Math call ─────────────────────────────────────────────────────────────
  // s122 m3: N-operand call. For Union: Clipper2 fuses every subpath in a
  // single pass (no iterative fold, no corruption hazard). For Subtract:
  // operands[0] minus union of operands[1..]. For Intersect: associative
  // iteration, monotonically shrinking.
  std::vector<PathData> final_loops;
  std::size_t total_subs = 0;
  for (const auto &o : operands) total_subs += o.size();
  LOG_INFO("Canvas: boolean {} — engine=Clipper2, operands={}, total subpaths={}",
           op_name, operands.size(), total_subs);

  // ── s140 m3 — ENRICH + CLIPPER2 + CLEANUP ───────────────────────────────
  // s143 m1: the user-facing quality slider (0..10) controls how
  // aggressive the cleanup is, and includes a "raw Clipper2" position
  // at the high end.
  //
  // Mapping is piecewise-linear with three anchor points so the slider's
  // default position (q=5) lands exactly on the s142 m6 hardcoded
  // defaults the algorithm was tuned around. q=10 short-circuits the
  // algorithm entirely — most faithful to Clipper2 means not touching
  // its output:
  //
  //   q = 0   → (apex=30°, max_run=20)   most aggressive cleanup
  //   q = 5   → (apex=15°, max_run=10)   default — the s142 m6 anchor
  //   q = 9   → (apex= 5°, max_run= 5)   gentlest cleanup the algo runs
  //   q = 10  → cleanup bypassed entirely (raw Clipper2 polyline)
  //
  // Two linear segments cover [0..5] and [5..9]; q=10 is a hard cutoff.
  //
  // Cleanup walk (s140 m3 — single pass, no pass 2):
  //   - Compute keeper set from ORIGINAL (un-enriched) operand geometry.
  //     This contains originals + intersections only. Synthetic guards
  //     are NOT keepers in the cleanup phase — they served their purpose
  //     at Clipper2 time (carrying curve shape into the boolean output
  //     via on-curve smooth tangent samples) and are now interpolant-
  //     equivalent.
  //   - Per keeper, find its slot in the output polyline and tag it.
  //     Originals restore byte-for-byte from KeeperPoint::source (the
  //     authored handles + type — corner stays corner, smooth stays
  //     smooth). Intersections retract to Corner.
  //   - Delete every untagged node.
  int q = m_boolean_cleanup_quality;
  if (q < 0)  q = 0;
  if (q > 10) q = 10;
  const bool cleanup_on = (q < 10);   // q=10 → raw Clipper2 (algo bypassed)
  double apex_min_turn_deg = 0.0;
  int    max_untagged_run  = 0;
  if (cleanup_on) {
    if (q <= 5) {
      // [0..5]: 30°→15°,  20→10
      const double t = double(q) / 5.0;              // 0 at q=0, 1 at q=5
      apex_min_turn_deg = 30.0 - 15.0 * t;
      max_untagged_run  =
          static_cast<int>(std::lround(20.0 - 10.0 * t));
    } else {
      // [5..9]: 15°→5°,  10→5  (q=9 is gentlest the algo runs;
      //                         q=10 doesn't reach this branch)
      const double t = double(q - 5) / 4.0;          // 0 at q=5, 1 at q=9
      apex_min_turn_deg = 15.0 - 10.0 * t;
      max_untagged_run  =
          static_cast<int>(std::lround(10.0 - 5.0 * t));
    }
  }

  std::vector<std::vector<BezierPath>> enriched_operands;
  Curvz::refit::KeeperSet m3_keepers;
  const auto* operands_for_clipper = &operands;
  if (cleanup_on) {
    // s142 m4 — targeted enrichment on BOTH sides of every intersection.
    // Inject intersection anchors + flanking triplet guards into every
    // operand at its analytic intersection points. Originals away from
    // intersections are not touched. Keepers built in the same call.
    m3_keepers = Curvz::refit::enrich_at_intersections_and_build_keepers(
        operands, enriched_operands, op);
    operands_for_clipper = &enriched_operands;
    LOG_INFO("Canvas: boolean {} — s142 m4 targeted enrichment ON "
             "(quality={}, apex={:.1f}°, max_run={}); "
             "{} operands modified at their intersection points; "
             "{} keepers ready for cleanup_loop_v4.",
             op_name, q, apex_min_turn_deg, max_untagged_run,
             (int)operands.size(), m3_keepers.size());
  } else {
    LOG_INFO("Canvas: boolean {} — quality={} → raw Clipper2 mode "
             "(enrichment + cleanup both bypassed).",
             op_name, q);
  }

  final_loops = Curvz::boolean_op_clipper(*operands_for_clipper, op);

  if (cleanup_on && !final_loops.empty()) {
    // s142 m6 — keepers from m4 (both-sides intersection triplets) +
    // apex pinning (m5) + span filler (m6).
    //
    // s143 m1 — apex_min_turn_deg and max_untagged_run are no longer
    // hardcoded; they're derived from m_boolean_cleanup_quality (the
    // user-facing slider).  See the mapping at the top of this block.
    LOG_INFO("Canvas: boolean {} — cleanup: {} keepers across {} loops "
             "(s142 m6 — apex+span_filler at apex>={:.1f}°, max_run={} "
             "from quality={} via cleanup_loop_v4)",
             op_name, m3_keepers.size(), final_loops.size(),
             apex_min_turn_deg, max_untagged_run, q);
    for (auto &loop : final_loops) {
      // s142 m6 — using cleanup_loop_v4 with both promotion passes.
      // Rollback options:
      //   - To s142 m5 (apex only): hardcode max_untagged_run = 0 below.
      //   - To s142 m4 (no promotion): change v4 → v3, drop tunables.
      //   - To s142 m3 (subject-only triplets): also revert m4's
      //     enrich_at_intersections_and_build_keepers loop.
      //   - To s140 m3 (byte-for-byte): change v4 → cleanup_loop and
      //     use compute_keeper_set instead.
      loop = Curvz::refit::cleanup_loop_v4(std::move(loop), m3_keepers,
                                           apex_min_turn_deg,
                                           max_untagged_run);
    }
  }

  if (final_loops.empty()) {
    LOG_WARN("Canvas: boolean {} returned empty result", op_name);
    m_sig_show_message.emit(
        "Boolean Operation Failed",
        std::string(op_name) +
            " produced an empty result. No changes were made.");
    return;
  }

  // ── Remove all original nodes from parent (high index first) ─────────────
  // Collect indices in descending order to avoid shifting
  std::vector<int> indices;
  for (auto &c : candidates)
    indices.push_back(c.index);
  std::sort(indices.rbegin(), indices.rend());
  for (int idx : indices)
    parent->children.erase(parent->children.begin() + idx);

  // ── Insert result node at insert_idx ─────────────────────────────────────
  // Single-loop result: emit a Path with the loop's geometry.
  // Multi-loop result:  emit a Compound whose children are Paths, one per
  //                     loop. The Compound's even/odd fill renders holes
  //                     (Subtract B-in-A) and disjoint islands (disjoint
  //                     Union) correctly.
  SceneNode *first_result = nullptr;
  int ins = std::clamp(insert_idx, 0, (int)parent->children.size());

  if (final_loops.size() == 1) {
    auto result_node = std::make_unique<SceneNode>();
    result_node->type = SceneNode::Type::Path;
    result_node->id = next_id();
    result_node->internal_id = last_iid();
    result_node->name = std::string(op_name);
    result_node->fill = saved_fill;          // s122 m5: by-value, not via dangling style_source
    result_node->stroke = saved_stroke;
    result_node->opacity = saved_opacity;
    result_node->visible = true;
    result_node->locked = false;
    result_node->path = std::make_unique<PathData>(std::move(final_loops[0]));

    first_result = result_node.get();
    parent->children.insert(parent->children.begin() + ins,
                            std::move(result_node));
  } else {
    // Multi-loop → Compound. Fill/stroke live on the Compound parent so
    // the even/odd fill paints the whole thing as a single shape.
    auto compound = std::make_unique<SceneNode>();
    compound->type = SceneNode::Type::Compound;
    compound->id = next_id();
    compound->internal_id = last_iid();
    compound->name = std::string(op_name);
    compound->fill = saved_fill;             // s122 m5: by-value
    compound->stroke = saved_stroke;
    compound->opacity = saved_opacity;
    compound->visible = true;
    compound->locked = false;
    for (auto &loop : final_loops) {
      auto child = std::make_unique<SceneNode>();
      child->type = SceneNode::Type::Path;
      child->id = next_id();
      child->internal_id = last_iid();
      // Compound renders fill from the topmost child; stroke per-child.
      // Mirror the style source on each child so fill/stroke render
      // regardless of Z-order decisions later. (See draw_object's Compound
      // branch for the rendering convention.)
      child->fill = saved_fill;              // s122 m5: by-value
      child->stroke = saved_stroke;
      child->opacity = saved_opacity;
      child->visible = true;
      child->locked = false;
      child->path = std::make_unique<PathData>(std::move(loop));
      compound->children.push_back(std::move(child));
    }
    LOG_INFO("Canvas: boolean {} → Compound with {} subpaths", op_name,
             compound->children.size());

    first_result = compound.get();
    parent->children.insert(parent->children.begin() + ins,
                            std::move(compound));
  }

  // ── Build result snapshot for undo ───────────────────────────────────────
  std::vector<std::unique_ptr<SceneNode>> result_snaps;
  result_snaps.push_back(clone_node(*parent->children[ins]));

  // ── Push single atomic undo command ──────────────────────────────────────
  if (m_history)
    m_history->push(std::make_unique<BooleanOpCommand>(
        project(), parent->internal_id,
        std::move(originals), std::move(result_snaps), insert_idx,
        std::string(op_name)));

  // ── Notify if any objects were skipped ───────────────────────────────────
  if (skipped_open > 0 || skipped_nopath > 0) {
    std::string msg;
    if (skipped_open > 0)
      msg += std::to_string(skipped_open) + " open path(s) were skipped.\n";
    if (skipped_nopath > 0)
      msg += std::to_string(skipped_nopath) +
             " non-path object(s) were skipped.\n";
    msg += "The operation was performed on the remaining closed paths.";
    m_sig_show_message.emit("Boolean " + std::string(op_name), msg);
  }

  // ── Update selection ─────────────────────────────────────────────────────
  m_selected = first_result;
  m_selection.clear();
  m_selection.push_back(first_result);
  m_selected_node = -1;
  m_node_selection.clear();

  notify_object_selection_changed();
  notify_node_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  {
    MacroStep s;
    switch (op) {
    case BooleanOpType::Union:
      s.op = MacroStep::Op::BooleanUnion;
      break;
    case BooleanOpType::Subtract:
      s.op = MacroStep::Op::BooleanSubtract;
      break;
    case BooleanOpType::Intersect:
      s.op = MacroStep::Op::BooleanIntersect;
      break;
    }
    record_step_if_recording(s);
  }
  LOG_INFO("Canvas: boolean {} complete — {} originals → 1 result", op_name,
           candidates.size());
}

// ── offset_path_op
// ────────────────────────────────────────────────────────────
void Canvas::offset_path_op(double distance, OffsetSide side,
                            bool keep_original) {
  if (!m_doc || m_selection.empty()) {
    LOG_WARN("Canvas: offset_path_op — nothing selected");
    return;
  }

  const char *side_name = (side == OffsetSide::Outside)  ? "Outside"
                          : (side == OffsetSide::Inside) ? "Inside"
                                                         : "Both";

  int applied = 0;
  int skipped = 0;

  for (SceneNode *obj : m_selection) {
    if (!obj->path || !obj->path->closed) {
      ++skipped;
      continue;
    }

    int idx = -1;
    SceneNode *parent = find_parent(m_doc, obj, &idx);
    if (!parent) {
      ++skipped;
      continue;
    }

    std::vector<PathData> results =
        Curvz::offset_path(*obj->path, distance, side);
    if (results.empty()) {
      ++skipped;
      continue;
    }

    // Capture style before any mutation (obj may dangle after erase)
    FillStyle saved_fill = obj->fill;
    StrokeStyle saved_stroke = obj->stroke;
    double saved_opacity = obj->opacity;

    if (keep_original) {
      // ── Keep original: insert result above original, undo via AddNodeCommand
      for (int ri = 0; ri < (int)results.size(); ++ri) {
        auto rnode = std::make_unique<SceneNode>();
        rnode->type = SceneNode::Type::Path;
        rnode->id = next_id();
        rnode->internal_id = last_iid();
        rnode->name = m_doc->uniquify_name(std::string("Offset ") + side_name);
        rnode->fill = saved_fill;
        rnode->stroke = saved_stroke;
        rnode->opacity = saved_opacity;
        rnode->visible = true;
        rnode->locked = false;
        rnode->path = std::make_unique<PathData>(results[ri]);

        // Insert above (before) the original
        int insert_at = std::clamp(idx + ri, 0, (int)parent->children.size());
        parent->children.insert(parent->children.begin() + insert_at,
                                std::move(rnode));

        if (m_history)
          m_history->push(std::make_unique<AddNodeCommand>(
              parent, clone_node(*parent->children[insert_at])));
      }
    } else {
      // ── Consume original: replace with result, undo via BooleanOpCommand
      std::vector<BooleanOpCommand::Original> originals;
      originals.push_back({clone_node(*obj), idx});

      std::vector<std::unique_ptr<SceneNode>> result_snaps;
      int ins = idx;

      parent->children.erase(parent->children.begin() + idx);

      for (int ri = 0; ri < (int)results.size(); ++ri) {
        auto rnode = std::make_unique<SceneNode>();
        rnode->type = SceneNode::Type::Path;
        rnode->id = next_id();
        rnode->name = m_doc->uniquify_name(std::string("Offset ") + side_name);
        rnode->fill = saved_fill;
        rnode->stroke = saved_stroke;
        rnode->opacity = saved_opacity;
        rnode->visible = true;
        rnode->locked = false;
        rnode->path = std::make_unique<PathData>(results[ri]);

        int insert_at = std::clamp(ins + ri, 0, (int)parent->children.size());
        parent->children.insert(parent->children.begin() + insert_at,
                                std::move(rnode));
        result_snaps.push_back(clone_node(*parent->children[insert_at]));
      }

      if (m_history)
        m_history->push(std::make_unique<BooleanOpCommand>(
            project(), parent->internal_id,
            std::move(originals), std::move(result_snaps), ins,
            "Offset Path"));
    }

    ++applied;
  }

  if (skipped > 0) {
    std::string msg =
        std::to_string(skipped) + " object(s) skipped (must be closed paths).";
    m_sig_show_message.emit("Offset Path", msg);
  }

  if (applied > 0) {
    m_selected = nullptr;
    m_selection.clear();
    m_selected_node = -1;
    m_node_selection.clear();
    notify_object_selection_changed();
    notify_node_selection_changed();
    m_sig_doc_changed.emit();
    queue_draw();
    {
      MacroStep s;
      s.op = MacroStep::Op::OffsetPath;
      s.value = distance;
      record_step_if_recording(s);
    }
    LOG_INFO("Canvas: offset_path_op({}, {}, keep={}) — applied to {} path(s)",
             distance, side_name, keep_original, applied);
  }
}

// ── expand_stroke_op
// ────────────────────────────────────────────────────────── Converts each
// selected stroked path into a filled outline shape. Open paths  → single
// closed filled Path (stroke outline joined at ends). ── Text tool
// ─────────────────────────────────────────────────────────────────

void Canvas::set_text_overlay(Gtk::Fixed *fixed) {
  m_text_fixed = fixed;

  // Create the floating entry once; hide it until a text edit begins.
  auto *entry = Gtk::make_managed<Gtk::Entry>();
  m_text_entry = entry;
  m_text_entry->set_visible(false);
  m_text_entry->add_css_class("text-tool-entry");
  // Give it a transparent background so it blends with the canvas.
  m_text_fixed->put(*m_text_entry, 0, 0);
}

void Canvas::position_text_entry() {
  if (!m_text_editing || !m_text_entry || !m_text_fixed)
    return;

  double sx, sy;
  doc_to_screen(m_text_editing->text_x, m_text_editing->text_y, sx, sy);

  // m_text_fixed covers the whole overlay. The canvas (this) sits inside
  // a grid inside the overlay, offset by the ruler widgets.
  // get_allocation() on `this` gives position relative to its parent (the
  // grid). We need position relative to the overlay root — walk the parent
  // chain.
  int cx = 0, cy = 0;
  Gtk::Widget *w = this;
  while (w) {
    Gtk::Widget *parent = w->get_parent();
    if (!parent || parent == m_text_fixed->get_parent())
      break;
    auto alloc = w->get_allocation();
    cx += alloc.get_x();
    cy += alloc.get_y();
    w = parent;
  }

  double ex = cx + sx;
  double ey = cy + sy - m_text_editing->text_font_size * m_zoom;

  // Clamp so entry stays inside the overlay.
  ex = std::max(0.0, ex);
  ey = std::max(0.0, ey);

  // Size entry to current text width or a minimum.
  int entry_w =
      std::max(120, (int)(m_text_editing->text_content.size() *
                          m_text_editing->text_font_size * 0.65 * m_zoom) +
                        24);
  m_text_entry->set_size_request(entry_w, -1);
  m_text_fixed->move(*m_text_entry, (int)ex, (int)ey);
}

// ── Convert Text to Path outlines (FreeType2) ────────────────────────────────
// Strategy:
//   1. Use PangoCairo to lay out the text and iterate glyph runs,
//      getting each glyph's index and x/y position within the layout.
//   2. Use PangoFcFont to retrieve the underlying FcPattern and from it
//      the font file path + face index, then open it with FreeType.
//   3. Call FT_Load_Glyph → FT_Outline_Decompose with callbacks that
//      build PathData contours.  Each FreeType contour → one closed Path
//      child of a Compound node.
//   4. Quadratic (conic) control points are elevated to cubics so they fit
//      our BezierNode format.
//   5. The assembled Compound replaces the original Text node in-place.
//      Undo is a single AddNodeCommand recording the new Compound.
// ─────────────────────────────────────────────────────────────────────────────

// FreeType outline decomposition callbacks — build PathData contours.
struct FTOutlineCtx {
  // Each call to move_to starts a new contour.
  std::vector<PathData> contours;
  PathData *cur = nullptr;
  double scale = 1.0; // FT units → doc units (1/64 px)

  // Current point (needed for quadratic elevation)
  double cx = 0, cy = 0;

  static int move_to_cb(const FT_Vector *to, void *user) {
    auto *c = static_cast<FTOutlineCtx *>(user);
    c->contours.emplace_back();
    c->cur = &c->contours.back();
    c->cur->closed = true;
    double x = to->x * c->scale;
    double y = to->y * c->scale;
    BezierNode n;
    n.x = x;
    n.y = y;
    n.cx1 = x;
    n.cy1 = y;
    n.cx2 = x;
    n.cy2 = y;
    n.type = BezierNode::Type::Corner;
    c->cur->nodes.push_back(n);
    c->cx = x;
    c->cy = y;
    return 0;
  }
  static int line_to_cb(const FT_Vector *to, void *user) {
    auto *c = static_cast<FTOutlineCtx *>(user);
    if (!c->cur)
      return 0;
    double x = to->x * c->scale;
    double y = to->y * c->scale;
    BezierNode n;
    n.x = x;
    n.y = y;
    n.cx1 = x;
    n.cy1 = y;
    n.cx2 = x;
    n.cy2 = y;
    n.type = BezierNode::Type::Corner;
    c->cur->nodes.push_back(n);
    c->cx = x;
    c->cy = y;
    return 0;
  }
  // Quadratic (conic) control point — elevate to cubic.
  // Two successive conic points imply an implicit on-curve midpoint.
  static int conic_to_cb(const FT_Vector *ctrl, const FT_Vector *to,
                         void *user) {
    auto *c = static_cast<FTOutlineCtx *>(user);
    if (!c->cur || c->cur->nodes.empty())
      return 0;
    double qx0 = c->cx, qy0 = c->cy;
    double qx1 = ctrl->x * c->scale, qy1 = ctrl->y * c->scale;
    double qx2 = to->x * c->scale, qy2 = to->y * c->scale;
    // Cubic control points: p1 = p0 + 2/3*(q1-p0), p2 = p2 + 2/3*(q1-p2)
    double cx1 = qx0 + 2.0 / 3.0 * (qx1 - qx0);
    double cy1 = qy0 + 2.0 / 3.0 * (qy1 - qy0);
    double cx2 = qx2 + 2.0 / 3.0 * (qx1 - qx2);
    double cy2 = qy2 + 2.0 / 3.0 * (qy1 - qy2);
    // Set outgoing handle of previous node
    c->cur->nodes.back().cx2 = cx1;
    c->cur->nodes.back().cy2 = cy1;
    // New on-curve node with incoming handle
    BezierNode n;
    n.x = qx2;
    n.y = qy2;
    n.cx1 = cx2;
    n.cy1 = cy2;
    n.cx2 = qx2;
    n.cy2 = qy2;
    n.type = BezierNode::Type::Corner;
    c->cur->nodes.push_back(n);
    c->cx = qx2;
    c->cy = qy2;
    return 0;
  }
  static int cubic_to_cb(const FT_Vector *c1, const FT_Vector *c2,
                         const FT_Vector *to, void *user) {
    auto *c = static_cast<FTOutlineCtx *>(user);
    if (!c->cur || c->cur->nodes.empty())
      return 0;
    double cx1 = c1->x * c->scale, cy1 = c1->y * c->scale;
    double cx2 = c2->x * c->scale, cy2 = c2->y * c->scale;
    double tx = to->x * c->scale, ty = to->y * c->scale;
    // Set outgoing handle of previous node
    c->cur->nodes.back().cx2 = cx1;
    c->cur->nodes.back().cy2 = cy1;
    // New on-curve node with incoming handle
    BezierNode n;
    n.x = tx;
    n.y = ty;
    n.cx1 = cx2;
    n.cy1 = cy2;
    n.cx2 = tx;
    n.cy2 = ty;
    n.type = BezierNode::Type::Corner;
    c->cur->nodes.push_back(n);
    c->cx = tx;
    c->cy = ty;
    return 0;
  }
};

static FT_Outline_Funcs s_ft_callbacks = {
    FTOutlineCtx::move_to_cb,
    FTOutlineCtx::line_to_cb,
    FTOutlineCtx::conic_to_cb,
    FTOutlineCtx::cubic_to_cb,
    0, // shift
    0  // delta
};

void Canvas::text_to_paths_op() {
  if (!m_doc || m_selection.empty()) {
    LOG_WARN("Canvas: text_to_paths_op — nothing selected");
    return;
  }

  std::vector<SceneNode *> text_nodes;
  for (SceneNode *obj : m_selection)
    if (obj->is_text())
      text_nodes.push_back(obj);

  if (text_nodes.empty()) {
    m_sig_show_message.emit("Convert Text to Path",
                            "Select one or more text objects first.");
    return;
  }

  // Initialise FreeType library once per call.
  FT_Library ft_lib = nullptr;
  if (FT_Init_FreeType(&ft_lib) != 0) {
    m_sig_show_message.emit("Convert Text to Path",
                            "FreeType initialisation failed.");
    return;
  }

  for (SceneNode *obj : text_nodes) {
    if (obj->text_content.empty())
      continue;

    // ── Detect text-on-path ───────────────────────────────────────────
    bool is_top = !obj->text_path_id.empty();
    SceneNode *guide_node =
        is_top ? top_find_path_by_id(obj->text_path_id) : nullptr;
    if (is_top && (!guide_node || !guide_node->path)) {
      LOG_WARN("text_to_paths_op: PTT guide not found for '{}', falling back "
               "to normal",
               obj->text_content);
      is_top = false;
    }

    // Build arc table once if PTT
    BezierPath top_bp;
    std::vector<double> top_arc_table;
    double top_total = 0.0;
    if (is_top) {
      top_bp = BezierPath::from_path_data(*guide_node->path);
      top_total = build_arc_table(top_bp, top_arc_table);
    }

    // ── 1. Build Pango layout to resolve font + glyph positions ──────
    // Use a 1×1 scratch Cairo surface — we only need layout metrics, not
    // rendering.
    auto surf =
        Cairo::ImageSurface::create(Cairo::ImageSurface::Format::ARGB32, 1, 1);
    auto cr = Cairo::Context::create(surf);

    PangoLayout *layout = pango_cairo_create_layout(cr->cobj());
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, obj->text_font_family.c_str());
    // Use absolute size in Pango units (= points * PANGO_SCALE).
    // We want doc-unit pixels → treat as points at 72dpi (1pt = 1px at 72dpi).
    pango_font_description_set_absolute_size(desc,
                                             obj->text_font_size * PANGO_SCALE);
    if (obj->text_bold)
      pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    if (obj->text_italic)
      pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, obj->text_content.c_str(), -1);

    // Apply letter spacing so glyph positions match rendering exactly.
    if (obj->text_letter_spacing != 0.0) {
      PangoAttrList *attrs = pango_attr_list_new();
      pango_attr_list_insert(
          attrs, pango_attr_letter_spacing_new(
                     (int)(obj->text_letter_spacing * PANGO_SCALE)));
      pango_layout_set_attributes(layout, attrs);
      pango_attr_list_unref(attrs);
    }

    // Baseline offset for Y positioning (same as draw_text_node).
    int baseline_pango = pango_layout_get_baseline(layout);
    double baseline_px = baseline_pango / (double)PANGO_SCALE;

    // Anchor horizontal offset.
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, nullptr, &logical);
    double anchor_off_x = 0.0;
    if (obj->text_anchor == "middle")
      anchor_off_x = -logical.width * 0.5;
    if (obj->text_anchor == "end")
      anchor_off_x = -logical.width;

    // PTT: compute anchor arc position and perp offset (mirrors
    // draw_text_on_path)
    double top_anchor_arc = obj->text_path_offset;
    double top_perp_offset = -obj->text_baseline_shift;
    if (is_top) {
      if (obj->text_anchor == "middle")
        top_anchor_arc -= logical.width * 0.5;
      else if (obj->text_anchor == "end")
        top_anchor_arc -= logical.width;
    }

    // ── 2. Iterate glyph runs ─────────────────────────────────────────
    // We'll collect all contours across all glyphs into one Compound.
    std::vector<PathData> all_contours;

    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    do {
      PangoLayoutRun *run = pango_layout_iter_get_run(iter);
      if (!run)
        continue;

      PangoFont *pfont = run->item->analysis.font;
      PangoGlyphString *gs = run->glyphs;

      // Get run origin in Pango units (layout coordinates).
      PangoRectangle run_ext;
      pango_layout_iter_get_run_extents(iter, nullptr, &run_ext);
      double run_x_px = run_ext.x / (double)PANGO_SCALE;

      // ── 3. Resolve font file via PangoFc ─────────────────────────
      // PangoFont → PangoFcFont → FcPattern → FC_FILE
      const char *font_file = nullptr;
      int face_idx = 0;

#if defined(PANGO_VERSION_CHECK) && PANGO_VERSION_CHECK(1, 18, 0)
      // pango_fc_font_get_pattern is available when PangoCairo uses fontconfig.
      PangoFcFont *fc_font = PANGO_FC_FONT(pfont);
      if (fc_font) {
        FcPattern *pat = pango_fc_font_get_pattern(fc_font);
        FcPatternGetString(pat, FC_FILE, 0, (FcChar8 **)&font_file);
        FcPatternGetInteger(pat, FC_INDEX, 0, &face_idx);
      }
#endif
      if (!font_file) {
        LOG_WARN(
            "text_to_paths_op: could not resolve font file for run, skipping");
        continue;
      }

      // ── 4. Open FreeType face ─────────────────────────────────────
      FT_Face ft_face = nullptr;
      if (FT_New_Face(ft_lib, font_file, face_idx, &ft_face) != 0) {
        LOG_WARN("text_to_paths_op: FT_New_Face failed for {}", font_file);
        continue;
      }
      // Set size to match Pango's absolute size.
      // FT size is in 26.6 fixed-point (1/64 pt), at 72dpi so 1pt = 1px.
      FT_Set_Char_Size(ft_face, 0, (FT_F26Dot6)(obj->text_font_size * 64.0), 72,
                       72);

      // Scale from FT 26.6 units to doc pixels: 1 FT unit = 1/64 px.
      double ft_scale = 1.0 / 64.0;

      // ── 5. Per-glyph outline extraction ───────────────────────────
      double glyph_x_px = run_x_px;
      for (int gi = 0; gi < gs->num_glyphs; ++gi) {
        PangoGlyphInfo &gi_info = gs->glyphs[gi];
        PangoGlyph glyph_id = gi_info.glyph;

        // Skip invalid/space glyphs.
        if (glyph_id == PANGO_GLYPH_EMPTY ||
            (glyph_id & PANGO_GLYPH_UNKNOWN_FLAG)) {
          glyph_x_px += gi_info.geometry.width / (double)PANGO_SCALE;
          continue;
        }

        double adv_px = gi_info.geometry.width / (double)PANGO_SCALE +
                        obj->text_letter_spacing;

        // Glyph x offset within the run (geometry.x_offset in Pango units).
        double gx =
            glyph_x_px + gi_info.geometry.x_offset / (double)PANGO_SCALE;
        double gy = -gi_info.geometry.y_offset / (double)PANGO_SCALE;

        if (FT_Load_Glyph(ft_face, glyph_id, FT_LOAD_NO_BITMAP) == 0 &&
            ft_face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {

          FTOutlineCtx ctx;
          ctx.scale = ft_scale;

          FT_Outline_Decompose(&ft_face->glyph->outline, &s_ft_callbacks, &ctx);

          if (is_top) {
            // ── PTT placement: rotate + translate to path point ──
            // Mirror draw_text_on_path exactly.
            double glyph_centre_arc =
                top_anchor_arc + glyph_x_px + adv_px * 0.5;
            double lookup_arc = obj->text_path_flip
                                    ? top_total - glyph_centre_arc
                                    : glyph_centre_arc;
            lookup_arc = std::max(0.0, std::min(lookup_arc, top_total));

            Vec2 pos;
            double angle;
            if (!path_point_at(top_bp, top_arc_table, top_total, lookup_arc,
                               pos, angle)) {
              glyph_x_px += adv_px;
              continue;
            }
            double eff_angle = obj->text_path_flip ? angle + M_PI : angle;

            // Local glyph origin in rotated frame:
            // glyph draw point = (-adv/2, perp_offset) in rotated coords.
            // FT point (ft_x, ft_y) in Y-up glyph space maps to:
            //   local_x = ft_x - adv/2
            //   local_y = -ft_y + perp_offset  (Y-flip + perp shift)
            // Then rotate by eff_angle and translate to pos.
            double cos_a = std::cos(eff_angle);
            double sin_a = std::sin(eff_angle);
            double half_adv = adv_px * 0.5;

            auto xform = [&](double &nx, double &ny) {
              double lx = nx - half_adv;
              double ly = -ny + top_perp_offset;
              nx = pos.x + lx * cos_a - ly * sin_a;
              ny = pos.y + lx * sin_a + ly * cos_a;
            };

            for (auto &pd : ctx.contours) {
              for (auto &n : pd.nodes) {
                xform(n.x, n.y);
                xform(n.cx1, n.cy1);
                xform(n.cx2, n.cy2);
              }
              if (!pd.nodes.empty())
                all_contours.push_back(std::move(pd));
            }
          } else {
            // ── Normal text placement: translate + Y-flip ────────
            double tx = obj->text_x + anchor_off_x + gx;
            double ty = obj->text_y - baseline_px + gy;

            for (auto &pd : ctx.contours) {
              for (auto &n : pd.nodes) {
                n.x = n.x + tx;
                n.y = ty - n.y;
                n.cx1 = n.cx1 + tx;
                n.cy1 = ty - n.cy1;
                n.cx2 = n.cx2 + tx;
                n.cy2 = ty - n.cy2;
              }
              if (!pd.nodes.empty())
                all_contours.push_back(std::move(pd));
            }
          }
        }

        glyph_x_px += adv_px;
      }

      FT_Done_Face(ft_face);

    } while (pango_layout_iter_next_run(iter));
    pango_layout_iter_free(iter);
    g_object_unref(layout);

    if (all_contours.empty())
      continue;

    // DIAG S55: surface contour details so we can diagnose text-to-path
    // rendering failures empirically. Remove once issue is resolved.
    LOG_INFO("text_to_paths: collected {} contour(s) for '{}'",
             all_contours.size(), obj->text_content);
    for (size_t ci = 0; ci < all_contours.size(); ++ci) {
      const auto &pd = all_contours[ci];
      if (pd.nodes.empty()) {
        LOG_INFO("  contour[{}]: EMPTY (0 nodes), closed={}", ci, pd.closed);
      } else {
        LOG_INFO("  contour[{}]: {} nodes, closed={}, first=({:.3f},{:.3f}), "
                 "last=({:.3f},{:.3f})",
                 ci, pd.nodes.size(), pd.closed, pd.nodes.front().x,
                 pd.nodes.front().y, pd.nodes.back().x, pd.nodes.back().y);
      }
    }

    // ── 6. Build a Compound node — one Path child per contour ────────
    auto compound = std::make_unique<SceneNode>();
    compound->type = SceneNode::Type::Compound;
    compound->name = m_doc->uniquify_name(obj->name + " (outline)");
    compound->fill = obj->fill;
    compound->stroke = obj->stroke;
    compound->opacity = obj->opacity;

    for (auto &pd : all_contours) {
      auto path_child = std::make_unique<SceneNode>();
      path_child->type = SceneNode::Type::Path;
      path_child->fill = obj->fill;
      // Stroke must sit on each child — Compound's draw reads stroke
      // per-child, never from the Compound itself (see draw_object's
      // Compound branch). Setting this to None would silently discard
      // any stroke the user had on the original text, requiring them to
      // release the compound first before a stroke could render.
      path_child->stroke = obj->stroke;
      path_child->path = std::make_unique<PathData>(std::move(pd));
      compound->children.push_back(std::move(path_child));
    }

    // ── 7. Replace Text node with Compound in-place ───────────────────
    SceneNode *owner_layer = nullptr;
    int insert_idx = 0;
    for (auto &layer : m_doc->layers) {
      for (int i = 0; i < (int)layer->children.size(); ++i) {
        if (layer->children[i].get() == obj) {
          owner_layer = layer.get();
          insert_idx = i;
          break;
        }
      }
      if (owner_layer)
        break;
    }
    if (!owner_layer)
      continue;

    SceneNode *raw_compound = compound.get();

    // Clone the original text node before replacing it — needed for undo.
    auto before_snap = clone_node(*obj);

    owner_layer->children.insert(owner_layer->children.begin() + insert_idx,
                                 std::move(compound));
    owner_layer->children.erase(owner_layer->children.begin() + insert_idx + 1);

    // ── 8. Remove guide path (PTT only) ───────────────────────────────
    // The guide path is now an invisible orphan — remove it.
    // Find it before pushing undo so we can store its index.
    SceneNode *guide_parent = nullptr;
    int guide_idx = -1;
    std::unique_ptr<SceneNode> guide_snap;
    if (is_top && guide_node) {
      for (auto &layer : m_doc->layers) {
        for (int i = 0; i < (int)layer->children.size(); ++i) {
          if (layer->children[i].get() == guide_node) {
            guide_parent = layer.get();
            guide_idx = i;
            break;
          }
        }
        if (guide_parent)
          break;
      }
      if (guide_parent) {
        guide_snap = clone_node(*guide_node);
        guide_parent->children.erase(guide_parent->children.begin() +
                                     guide_idx);
      }
    }

    if (m_history) {
      auto composite =
          std::make_unique<CompositeCommand>("Convert text to path");
      composite->add(std::make_unique<ReplaceNodeCommand>(
          owner_layer, insert_idx, std::move(before_snap),
          clone_node(*raw_compound)));
      if (guide_parent && guide_snap) {
        composite->add(std::make_unique<DeleteObjectCommand>(
            guide_parent, std::move(guide_snap), guide_idx));
      }
      m_history->push(std::move(composite));
    }

    LOG_INFO("text_to_paths_op: '{}' → {} contour(s){}", obj->text_content,
             all_contours.size(), is_top ? " (PTT, guide removed)" : "");
  }

  FT_Done_FreeType(ft_lib);

  m_selection.clear();
  m_selected = nullptr;
  notify_object_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
}

// Closed paths → Compound with outer + inner (even-odd, stroke colour as fill).
// Operation is undoable via BooleanOpCommand.
void Canvas::expand_stroke_op() {
  if (!m_doc || m_selection.empty()) {
    LOG_WARN("Canvas: expand_stroke_op — nothing selected");
    return;
  }

  // S96 m3: name uniqueness comes from the document funnel; SVG `id`
  // is derived from name + internal_id at write time. The previous
  // static counter is no longer needed.
  int applied = 0;
  int skipped = 0;

  for (SceneNode *obj : m_selection) {
    if (!obj->path) {
      ++skipped;
      continue;
    }
    if (obj->stroke.paint.type == FillStyle::Type::None ||
        obj->stroke.width <= 0.0) {
      ++skipped;
      continue;
    }

    int idx = -1;
    SceneNode *parent = find_parent(m_doc, obj, &idx);
    if (!parent) {
      ++skipped;
      continue;
    }

    double half = obj->stroke.width * 0.5;
    bool is_open = !obj->path->closed;

    std::vector<PathData> outline_pd;
    std::vector<PathData> outer_pd, inner_pd;

    if (is_open) {
      outline_pd = Curvz::offset_path(*obj->path, half, OffsetSide::Both);
      if (outline_pd.empty()) {
        ++skipped;
        continue;
      }
    } else {
      outer_pd = Curvz::offset_path(*obj->path, half, OffsetSide::Outside);
      inner_pd = Curvz::offset_path(*obj->path, half, OffsetSide::Inside);
      if (outer_pd.empty() || inner_pd.empty()) {
        ++skipped;
        continue;
      }
    }

    FillStyle result_fill = obj->stroke.paint;
    double saved_opacity = obj->opacity;

    std::vector<BooleanOpCommand::Original> originals;
    originals.push_back({clone_node(*obj), idx});
    parent->children.erase(parent->children.begin() + idx);

    int insert_at = std::clamp(idx, 0, (int)parent->children.size());

    if (is_open) {
      // Open path -> single closed filled Path
      auto result_node = std::make_unique<SceneNode>();
      result_node->type = SceneNode::Type::Path;
      result_node->id = next_id();
      result_node->internal_id = last_iid();
      result_node->name =
          m_doc->next_default_name(CurvzDocument::NameKind::ExpandedStroke);
      result_node->fill = result_fill;
      result_node->stroke.paint.type = FillStyle::Type::None;
      result_node->opacity = saved_opacity;
      result_node->visible = true;
      result_node->locked = false;
      result_node->path = std::make_unique<PathData>(outline_pd[0]);
      parent->children.insert(parent->children.begin() + insert_at,
                              std::move(result_node));
    } else {
      // Closed path -> compound (outer + inner, even-odd)
      auto outer_node = std::make_unique<SceneNode>();
      outer_node->type = SceneNode::Type::Path;
      outer_node->id = next_id();
      outer_node->internal_id = last_iid();
      outer_node->name = "Outer";
      outer_node->fill = result_fill;
      outer_node->stroke.paint.type = FillStyle::Type::None;
      outer_node->opacity = 1.0;
      outer_node->visible = true;
      outer_node->locked = false;
      outer_node->path = std::make_unique<PathData>(outer_pd[0]);

      auto inner_node = std::make_unique<SceneNode>();
      inner_node->type = SceneNode::Type::Path;
      inner_node->id = next_id();
      inner_node->internal_id = last_iid();
      inner_node->name = "Inner";
      inner_node->fill = result_fill;
      inner_node->stroke.paint.type = FillStyle::Type::None;
      inner_node->opacity = 1.0;
      inner_node->visible = true;
      inner_node->locked = false;
      inner_node->path = std::make_unique<PathData>(inner_pd[0]);

      auto compound = std::make_unique<SceneNode>();
      compound->type = SceneNode::Type::Compound;
      compound->internal_id = generate_internal_id();
      compound->name = m_doc->next_default_name(CurvzDocument::NameKind::ExpandedStroke);
      // S58g: Compound owns its paint per the S58d rule — set the expanded
      // stroke's target colour on the Compound itself so the canvas renderer
      // (which reads Compound.fill) picks it up. The per-child fills we
      // also set above are now redundant for rendering but kept intact so
      // that if the Compound is ever split back into plain paths, the
      // children carry the right colour.
      compound->fill = result_fill;
      compound->stroke.paint.type = FillStyle::Type::None;
      compound->opacity = saved_opacity;
      compound->visible = true;
      compound->locked = false;
      compound->children.push_back(std::move(outer_node));
      compound->children.push_back(std::move(inner_node));
      parent->children.insert(parent->children.begin() + insert_at,
                              std::move(compound));
    }

    std::vector<std::unique_ptr<SceneNode>> result_snaps;
    result_snaps.push_back(clone_node(*parent->children[insert_at]));

    if (m_history)
      m_history->push(std::make_unique<BooleanOpCommand>(
          project(), parent->internal_id,
          std::move(originals), std::move(result_snaps), idx,
          "Expand Stroke"));

    ++applied;
  }

  if (skipped > 0) {
    std::string msg =
        std::to_string(skipped) +
        " object(s) skipped (must have a stroke with non-zero width).";
    m_sig_show_message.emit("Expand Stroke", msg);
  }

  if (applied > 0) {
    m_selected = nullptr;
    m_selection.clear();
    m_selected_node = -1;
    m_node_selection.clear();
    notify_object_selection_changed();
    notify_node_selection_changed();
    m_sig_doc_changed.emit();
    queue_draw();
    LOG_INFO("Canvas: expand_stroke_op — applied to {} path(s)", applied);
  }
}

void Canvas::arrange(ArrangeOp op) {
  if (!m_doc || !m_selected)
    return;

  // ── 1. Collect selected objects and verify they share one parent ───────
  // All objects in m_selection must be in the same parent for a coherent
  // block move. Find the parent of m_selected; skip any selection member
  // that lives elsewhere (e.g. objects in different layers).
  int primary_idx = -1;
  SceneNode *parent = find_parent(m_doc, m_selected, &primary_idx);
  if (!parent || primary_idx < 0)
    return;

  // Build sorted list of (index, object*) for every selected object that
  // shares this parent.
  std::vector<std::pair<int, SceneNode *>> sel_items; // (index, ptr) ascending
  for (SceneNode *obj : m_selection) {
    int idx = -1;
    SceneNode *p = find_parent(m_doc, obj, &idx);
    if (p == parent && idx >= 0)
      sel_items.push_back({idx, obj});
  }
  if (sel_items.empty())
    return;
  std::sort(sel_items.begin(), sel_items.end());

  int n = (int)parent->children.size();
  if (n < 2)
    return;

  const char *desc = "";

  // ── 2. Compute new positions ──────────────────────────────────────────
  // Strategy: extract the selected block, find the insertion point, reinsert.
  // We work on a temporary index array to figure out before/after pairs
  // without actually mutating yet.

  // Build a vector of all child indices, marking which are selected.
  std::vector<int> order(
      n); // order[i] = original index of child now at position i
  std::iota(order.begin(), order.end(), 0);

  std::unordered_set<int> sel_set;
  for (auto &[idx, _] : sel_items)
    sel_set.insert(idx);

  // Separate selected and non-selected in their current relative order
  std::vector<int> non_sel, sel_indices;
  for (int i = 0; i < n; ++i) {
    if (sel_set.count(i))
      sel_indices.push_back(i);
    else
      non_sel.push_back(i);
  }

  // Determine insertion position in the non-selected list
  // then interleave: selected block inserts before/after a non-sel neighbour.
  int lowest = sel_indices.front();
  int highest = sel_indices.back();

  // Count non-selected objects strictly below and above the selected block
  int non_sel_below = 0, non_sel_above = 0;
  for (int i : non_sel) {
    if (i < lowest)
      ++non_sel_below;
    if (i > highest)
      ++non_sel_above;
  }

  switch (op) {
  case ArrangeOp::BringToFront:
    desc = "Bring to front";
    break;
  case ArrangeOp::BringForward:
    desc = "Bring forward";
    break;
  case ArrangeOp::SendBackward:
    desc = "Send backward";
    break;
  case ArrangeOp::SendToBack:
    desc = "Send to back";
    break;
  }

  // Build the new child order as a permutation of [0..n-1]
  // new_order[i] = which original child ends up at position i
  std::vector<int> new_order;
  new_order.reserve(n);

  if (op == ArrangeOp::BringToFront) {
    // index 0 = visually on top → selected goes at FRONT of new_order
    if (non_sel_below == 0)
      return; // already at top (index 0)
    for (int i : sel_indices)
      new_order.push_back(i);
    for (int i : non_sel)
      new_order.push_back(i);
  } else if (op == ArrangeOp::SendToBack) {
    // selected goes at BACK of new_order (highest index = bottom)
    if (non_sel_above == 0)
      return; // already at bottom
    for (int i : non_sel)
      new_order.push_back(i);
    for (int i : sel_indices)
      new_order.push_back(i);
  } else if (op == ArrangeOp::BringForward) {
    // Move toward lower index (toward index 0 = top)
    if (non_sel_below == 0)
      return; // already at top
    // Find the last non-sel index below lowest (the one to jump over)
    int pivot = -1;
    for (int i = (int)non_sel.size() - 1; i >= 0; --i) {
      if (non_sel[i] < lowest) {
        pivot = non_sel[i];
        break;
      }
    }
    for (int i : non_sel) {
      if (i < pivot)
        new_order.push_back(i);
    }
    for (int i : sel_indices)
      new_order.push_back(i);
    new_order.push_back(pivot);
    for (int i : non_sel) {
      if (i > highest)
        new_order.push_back(i);
    }
  } else { // SendBackward — move toward higher index (toward bottom)
    if (non_sel_above == 0)
      return; // already at bottom
    int pivot = -1;
    for (int i : non_sel) {
      if (i > highest) {
        pivot = i;
        break;
      }
    }
    for (int i : non_sel) {
      if (i < lowest)
        new_order.push_back(i);
    }
    new_order.push_back(pivot);
    for (int i : sel_indices)
      new_order.push_back(i);
    for (int i : non_sel) {
      if (i > pivot)
        new_order.push_back(i);
    }
  }

  if (new_order.size() != (size_t)n)
    return; // sanity

  // ── 3. Apply permutation to children vector ───────────────────────────
  // Snapshot before order
  std::vector<std::string> before_order, after_order;
  before_order.reserve(n);
  for (auto &c : parent->children)
    before_order.push_back(c->id);

  // Build inverse map: original_index → new_position (for undo records)
  std::vector<int> orig_to_new(n);
  for (int pos = 0; pos < n; ++pos)
    orig_to_new[new_order[pos]] = pos;

  // Collect undo entries: for each selected object, record before/after
  std::vector<ZOrderCommand::Entry> entries;
  for (auto &[orig_idx, obj] : sel_items)
    entries.push_back({obj->id, orig_idx, orig_to_new[orig_idx]});

  // Reorder children in-place using the permutation
  {
    auto &ch = parent->children;
    std::vector<std::unique_ptr<SceneNode>> reordered;
    reordered.reserve(n);
    for (int orig : new_order)
      reordered.push_back(std::move(ch[orig]));
    ch = std::move(reordered);
  }

  // Snapshot after order
  after_order.reserve(n);
  for (auto &c : parent->children)
    after_order.push_back(c->id);

  // ── 4. Push undo + signal ─────────────────────────────────────────────
  if (m_history)
    m_history->push(std::make_unique<ZOrderCommand>(
        project(), parent->internal_id,
        std::move(entries), std::move(before_order),
        std::move(after_order), desc));

  m_sig_doc_changed.emit();
  queue_draw();
  {
    MacroStep s;
    switch (op) {
    case ArrangeOp::BringToFront:
      s.op = MacroStep::Op::BringToFront;
      break;
    case ArrangeOp::BringForward:
      s.op = MacroStep::Op::BringForward;
      break;
    case ArrangeOp::SendBackward:
      s.op = MacroStep::Op::SendBackward;
      break;
    case ArrangeOp::SendToBack:
      s.op = MacroStep::Op::SendToBack;
      break;
    }
    record_step_if_recording(s);
  }
  LOG_INFO("Canvas: {} ({} objects)", desc, sel_items.size());
}

void Canvas::reverse_selected_path() {
  if (!m_selected || !m_selected->path)
    return;
  if (m_selected->path->nodes.size() < 2)
    return;

  BezierPath bp = BezierPath::from_path_data(*m_selected->path);
  PathData before = *m_selected->path;
  bp.reverse();
  // Keep selected node index mapped to its reversed position
  if (m_selected_node >= 0) {
    int n = (int)bp.nodes.size();
    m_selected_node = n - 1 - m_selected_node;
  }
  *m_selected->path = bp.to_path_data();
  if (m_history)
    m_history->push(std::make_unique<EditPathCommand>(
        project(), m_selected->internal_id,
        std::move(before), *m_selected->path, "Reverse path"));

  // ── Record macro step ─────────────────────────────────────────────────
  {
    MacroStep s;
    s.op = MacroStep::Op::ReversePath;
    record_step_if_recording(s);
  }

  m_sig_doc_changed.emit();
  if (m_selected_node >= 0)
    m_sig_node_changed.emit(m_selected, m_selected_node);
  queue_draw();
  LOG_INFO("Canvas: reversed path '{}'", m_selected->id);
}

// ── open_selected_at_node
// ─────────────────────────────────────────────────────
void Canvas::open_selected_at_node() {
  if (!m_selected || !m_selected->path) {
    return;
  }
  if (!m_selected->path->closed) {
    // B on an open path is a no-op — caller should have routed to
    // split_selected_at_node instead. Silent here; the dispatch lives
    // at the keypress site.
    return;
  }
  if (m_selected_node < 0) {
    return;
  }
  if ((int)m_selected->path->nodes.size() < 2) {
    return;
  }

  BezierPath bp = BezierPath::from_path_data(*m_selected->path);
  PathData before = *m_selected->path;
  bp.open_at_node(m_selected_node);

  // Nudge the new tail (duplicate of the break node) by a small amount
  // so the user can see the path has separated — ~4 screen pixels in doc space.
  if (!bp.nodes.empty()) {
    double nudge = 4.0 / m_zoom;
    bp.nodes.back().x += nudge;
    bp.nodes.back().y += nudge;
    bp.nodes.back().cx1 += nudge;
    bp.nodes.back().cy1 += nudge;
    bp.nodes.back().cx2 += nudge;
    bp.nodes.back().cy2 += nudge;
  }
  *m_selected->path = bp.to_path_data();

  if (m_history)
    m_history->push(std::make_unique<EditPathCommand>(
        project(), m_selected->internal_id,
        std::move(before), *m_selected->path, "Open path at node"));

  // Select the new tail (last node) — the cut point
  m_selected_node = (int)m_selected->path->nodes.size() - 1;
  m_sig_doc_changed.emit();
  m_sig_node_changed.emit(m_selected, m_selected_node);
  queue_draw();
  LOG_INFO("Canvas: opened path '{}' at node", m_selected->id);
}

// ── split_selected_at_node
// ────────────────────────────────────────────────────
void Canvas::split_selected_at_node() {
  if (!m_selected || !m_selected->path) {
    return;
  }
  if (m_selected->path->closed) {
    // Split only applies to open paths; closed paths use Open here.
    // Caller should have routed; silent here.
    return;
  }
  if (m_selected_node <= 0) {
    return;
  }
  int n = (int)m_selected->path->nodes.size();
  if (m_selected_node >= n - 1) {
    return;
  }
  if (n < 3) {
    return;
  }
  if (!m_doc) {
    return;
  }

  // Find parent layer
  SceneNode *parent_layer = nullptr;
  int orig_index = -1;
  for (auto &layer : m_doc->layers) {
    for (int i = 0; i < (int)layer->children.size(); ++i) {
      if (layer->children[i].get() == m_selected) {
        parent_layer = layer.get();
        orig_index = i;
        break;
      }
    }
    if (parent_layer)
      break;
  }
  if (!parent_layer || orig_index < 0) {
    // Path is nested in a Group/Compound — split currently only
    // supports top-level layer children. Kept as INFO so a user who
    // hits this gets a hint in the log; the hotkey otherwise just
    // appears to do nothing.
    LOG_INFO("Canvas: split_at_node — selected path is nested in a "
             "Group/Compound; not currently supported");
    return;
  }

  BezierPath bp = BezierPath::from_path_data(*m_selected->path);
  auto [left_pd, right_pd] = bp.split_at_node(m_selected_node);
  if (left_pd.nodes.empty() || right_pd.nodes.empty()) {
    LOG_WARN("Canvas: split_at_node({}) produced empty halves "
             "(left={}, right={}) — math layer disagrees with caller's guards",
             m_selected_node, (int)left_pd.nodes.size(),
             (int)right_pd.nodes.size());
    return;
  }

  // Nudge the right path's head (shared break node) so the split is visible
  {
    double nudge = 4.0 / m_zoom;
    auto &nd = right_pd.nodes.front();
    nd.x += nudge;
    nd.y += nudge;
    nd.cx1 += nudge;
    nd.cy1 += nudge;
    nd.cx2 += nudge;
    nd.cy2 += nudge;
  }

  // Build two new SceneNodes inheriting appearance from the original
  auto left_node = std::make_unique<SceneNode>();
  left_node->type = SceneNode::Type::Path;
  left_node->id = m_selected->id + "_L";
  left_node->name = m_selected->name;
  left_node->fill = m_selected->fill;
  left_node->stroke = m_selected->stroke;
  left_node->path = std::make_unique<PathData>(left_pd);

  auto right_node = std::make_unique<SceneNode>();
  right_node->type = SceneNode::Type::Path;
  right_node->id = m_selected->id + "_R";
  right_node->name = m_selected->name;
  right_node->fill = m_selected->fill;
  right_node->stroke = m_selected->stroke;
  right_node->path = std::make_unique<PathData>(right_pd);

  // Push undo before mutating
  if (m_history) {
    auto orig_snap = clone_node(*m_selected);
    auto left_snap = clone_node(*left_node);
    auto right_snap = clone_node(*right_node);
    m_history->push(std::make_unique<SplitPathCommand>(
        parent_layer, std::move(orig_snap), orig_index, std::move(left_snap),
        std::move(right_snap)));
  }

  // Replace original with the two halves
  SceneNode *right_raw = right_node.get();
  SceneNode *erased_obj = m_selected; // freed by the erase below

  // s156 — Scrub Canvas state pointing at the about-to-be-freed original
  // BEFORE the erase. The renderer's node-overlay loop iterates
  // m_node_selection and dereferences ns.obj->path on every paint;
  // a stale entry pointing at the freed original yields
  // std::bad_array_new_length on the next paint. The original comment
  // here noted the hazard but relied on signal_selection listeners to
  // refresh — the renderer reads m_node_selection directly off Canvas,
  // no listener owns it. scrub_node_refs is the seam-level pump.
  scrub_node_refs(erased_obj);
  // m_selected itself will be reassigned to right_raw below. Until then
  // it's a dangling pointer — no signal emit allowed in this window.
  parent_layer->children.erase(parent_layer->children.begin() + orig_index);
  parent_layer->children.insert(parent_layer->children.begin() + orig_index,
                                std::move(right_node));
  parent_layer->children.insert(parent_layer->children.begin() + orig_index,
                                std::move(left_node));

  // Select the right piece, head node (the cut point). All Canvas state
  // that pointed at the freed original was scrubbed above, so the signal
  // emits and the renderer paint that follows see a consistent tree.
  m_selected = right_raw;
  m_selection = {right_raw};  // s159 m2: scrub_node_refs above emptied m_selection of erased_obj; repopulate
  m_selected_node = 0;
  notify_object_selection_changed();
  notify_node_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: split path at node {}", m_selected_node);
}

// ── import_svg_to_canvas
// ─────────────────────────────────────────────────────── Loads an SVG from
// disk, collects all visible objects, centres them on the current viewport, and
// inserts them into the active layer with undo support.
void Canvas::import_svg_to_canvas(const std::string &path) {
  if (!m_doc)
    return;

  auto src_doc = parse_svg_file(path);
  if (!src_doc) {
    LOG_ERROR("import_svg_to_canvas: failed to parse '{}'", path);
    return;
  }

  SceneNode *target_layer = m_doc->active_layer();
  if (!target_layer)
    target_layer = m_doc->layers[0].get();

  // Collect all visible, non-guide, non-ref objects from the imported doc
  std::vector<std::unique_ptr<SceneNode>> imported;
  for (auto &layer_uptr : src_doc->layers) {
    if (!layer_uptr->visible)
      continue;
    if (layer_uptr->is_special_layer())
      continue;
    for (auto &child : layer_uptr->children) {
      if (!child->visible)
        continue;
      imported.push_back(clone_node(*child));
    }
  }

  if (imported.empty()) {
    LOG_WARN("import_svg_to_canvas: no visible objects in '{}'", path);
    return;
  }

  // Freshen IDs so imported nodes don't collide with existing scene
  int id_counter = s_next_id;
  for (auto &node : imported)
    freshen_ids(node.get(), m_doc, id_counter);
  s_next_id = id_counter;

  // Compute bounding box of all imported objects
  double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
  for (auto &node : imported) {
    auto bb = object_bbox(*node, false);
    if (!bb)
      continue;
    bx1 = std::min(bx1, bb->x);
    by1 = std::min(by1, bb->y);
    bx2 = std::max(bx2, bb->x + bb->w);
    by2 = std::max(by2, bb->y + bb->h);
  }

  // Centre on current viewport
  double vp_cx, vp_cy;
  screen_to_doc(get_width() * 0.5, get_height() * 0.5, vp_cx, vp_cy);

  double src_cx = (bx1 + bx2) * 0.5;
  double src_cy = (by1 + by2) * 0.5;
  double dx = vp_cx - src_cx;
  double dy = vp_cy - src_cy;

  if (std::abs(dx) > 0.01 || std::abs(dy) > 0.01) {
    for (auto &node : imported) {
      std::vector<SceneNode *> paths;
      collect_paths(node.get(), paths);
      for (SceneNode *p : paths) {
        if (!p->path)
          continue;
        for (auto &n : p->path->nodes) {
          n.x += dx;
          n.y += dy;
          n.cx1 += dx;
          n.cy1 += dy;
          n.cx2 += dx;
          n.cy2 += dy;
        }
      }
    }
  }

  // Build undo command snapshots and insert into target layer
  std::vector<SceneNode *> new_sel;
  for (int i = (int)imported.size() - 1; i >= 0; --i) {
    auto snap = clone_node(*imported[i]);
    target_layer->children.insert(target_layer->children.begin(),
                                  std::move(imported[i]));
    SceneNode *inserted = target_layer->children.front().get();
    new_sel.push_back(inserted);
    if (m_history)
      m_history->push(
          std::make_unique<AddNodeCommand>(target_layer, std::move(snap)));
  }

  // Update selection
  m_selection.clear();
  for (SceneNode *n : new_sel)
    m_selection.push_back(n);
  m_selected = m_selection.empty() ? nullptr : m_selection.front();

  notify_object_selection_changed();
  m_sig_request_tool.emit(ActiveTool::Selection);
  m_sig_doc_changed.emit();
  queue_draw();

  LOG_INFO("import_svg_to_canvas: placed {} object(s) from '{}'",
           new_sel.size(), path);
}

// ── import_image_to_canvas
// ──────────────────────────────────────────────────── Places a raster image
// (PNG/JPG/GIF/WebP) as an Image node.
//
// fit_canvas_to_image=false (default): scales to ≤80% of the current canvas
//   preserving aspect ratio, centres in viewport, leaves the canvas size
//   unchanged. The original behaviour, used for "drop a reference into an
//   existing doc" workflows.
//
// fit_canvas_to_image=true (s125 m1c): resizes the document's canvas to
//   match the image's natural pixel dimensions, places the image at (0, 0)
//   at 1:1 pixel mapping. Used by File → Place Image as Document for the
//   manual-screenshot annotation workflow. The canvas resize is NOT
//   currently undoable — matches the rest of the canvas-resize paths in
//   the app (PropertiesPanel canvas inspector, NewDocument flow). Backlog
//   item to wire a CanvasResizeCommand and route all three through it.
void Canvas::import_image_to_canvas(const std::string &path,
                                    bool fit_canvas_to_image) {
  if (!m_doc)
    return;

  // Determine natural pixel dimensions via the shared image_meta helper —
  // single seam for both this path and the right-click Image Info dialog.
  ImageMeta meta = read_image_meta(path);
  if (!meta.valid) {
    LOG_ERROR("import_image_to_canvas: could not read dimensions of '{}'",
              path);
    return;
  }
  const int img_w = meta.width;
  const int img_h = meta.height;

  double doc_w = 0.0, doc_h = 0.0;
  double doc_x = 0.0, doc_y = 0.0;

  if (fit_canvas_to_image) {
    // Resize the document canvas to match the image's pixel dimensions
    // exactly. Image goes at (0, 0) at 1:1.
    m_doc->canvas = CanvasModel::from_pixels(img_w, img_h);
    doc_w = static_cast<double>(img_w);
    doc_h = static_cast<double>(img_h);
    doc_x = 0.0;
    doc_y = 0.0;
  } else {
    // Existing behaviour: scale to ≤80% of canvas, centre.
    int cw = m_doc->canvas_width();
    int ch = m_doc->canvas_height();
    double max_w = cw * 0.8;
    double max_h = ch * 0.8;
    double scale = std::min(max_w / img_w, max_h / img_h);
    if (scale > 1.0)
      scale = 1.0; // don't upscale beyond natural size
    doc_w = img_w * scale;
    doc_h = img_h * scale;
    // Centre in doc space (Y-down)
    doc_x = (cw - doc_w) * 0.5;
    doc_y = (ch - doc_h) * 0.5;
  }

  SceneNode *layer = m_doc->active_layer();
  if (!layer)
    layer = m_doc->layers[0].get();

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = std::filesystem::path(path).stem().string();
  obj.type = SceneNode::Type::Image;
  obj.image_path = path;
  obj.image_x = doc_x;
  obj.image_y = doc_y;
  obj.image_w = doc_w;
  obj.image_h = doc_h;

  layer->children.insert(layer->children.begin(), clone_node(obj));
  m_selected = layer->children.front().get();
  if (m_history)
    m_history->push(
        std::make_unique<AddNodeCommand>(layer, clone_node(*m_selected)));

  m_selection = {m_selected};
  notify_object_selection_changed();
  m_sig_request_tool.emit(ActiveTool::Selection);

  // Re-fit viewport when the canvas itself changed size, so the new
  // image-sized canvas lands centred and visible. Matches the
  // PropertiesPanel canvas-inspector handler at MainWindow.cpp:2076.
  if (fit_canvas_to_image)
    zoom_fit();

  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO(
      "import_image_to_canvas: '{}' {}x{} → {:.1f}x{:.1f} at ({:.1f},{:.1f}) "
      "fit_canvas={}",
      path, img_w, img_h, doc_w, doc_h, doc_x, doc_y, fit_canvas_to_image);
}

// ── flip_selection
// ──────────────────────────────────────────────────────────── Flips all
// selected objects horizontally or vertically around the union BBX centre.
// Paths have their node coordinates negated; images get a scale(-1,1) or
// scale(1,-1) composed into their transform.
void Canvas::flip_selection(bool horizontal) {
  if (m_selection.empty() || !m_doc)
    return;

  // Compute union bounding box of selection
  double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
  for (SceneNode *obj : m_selection) {
    auto bb = object_bbox(*obj, false);
    if (!bb)
      continue;
    bx1 = std::min(bx1, bb->x);
    by1 = std::min(by1, bb->y);
    bx2 = std::max(bx2, bb->x + bb->w);
    by2 = std::max(by2, bb->y + bb->h);
  }
  if (bx1 > bx2)
    return;
  double cx = (bx1 + bx2) * 0.5;
  double cy = (by1 + by2) * 0.5;

  std::vector<ScaleObjectsCommand::LeafSnap> path_snaps;
  std::vector<ScaleImageCommand::Snap> img_snaps;

  for (SceneNode *obj : m_selection) {
    if (obj->is_image()) {
      // Save before state
      Transform bef = obj->transform;
      double bef_x = obj->image_x, bef_y = obj->image_y;
      double bef_w = obj->image_w, bef_h = obj->image_h;

      // Image centre
      double icx = obj->image_x + obj->image_w * 0.5;
      double icy = obj->image_y + obj->image_h * 0.5;

      if (horizontal) {
        // Flip image centre across vertical axis through cx
        obj->image_x = 2.0 * cx - icx - obj->image_w * 0.5;
        // Compose scale(-1,1) into transform: negate column 0
        obj->transform.a = -obj->transform.a;
        obj->transform.b = -obj->transform.b;
      } else {
        // Flip image centre across horizontal axis through cy
        obj->image_y = 2.0 * cy - icy - obj->image_h * 0.5;
        // Compose scale(1,-1) into transform: negate column 1
        obj->transform.c = -obj->transform.c;
        obj->transform.d = -obj->transform.d;
      }

      img_snaps.push_back({obj->internal_id, bef_x, bef_y, bef_w, bef_h,
                           bef, obj->image_x, obj->image_y, obj->image_w,
                           obj->image_h, obj->transform});
      continue;
    }

    // Text — reflect anchor point around centre
    if (obj->is_text()) {
      double bef_x = obj->text_x, bef_y = obj->text_y;
      if (horizontal)
        obj->text_x = 2.0 * cx - obj->text_x;
      else
        obj->text_y = 2.0 * cy - obj->text_y;
      if (m_history)
        m_history->push(std::make_unique<MoveObjectCommand>(
            project(), obj->internal_id,
            bef_x, bef_y, obj->text_x, obj->text_y));
      continue;
    }

    // Path / group / compound — collect leaves
    std::vector<SceneNode *> leaves;
    collect_paths(obj, leaves);
    for (SceneNode *leaf : leaves) {
      if (!leaf->path || leaf->path->nodes.empty())
        continue;
      PathData before = *leaf->path;
      for (auto &n : leaf->path->nodes) {
        if (horizontal) {
          n.x = 2.0 * cx - n.x;
          n.cx1 = 2.0 * cx - n.cx1;
          n.cx2 = 2.0 * cx - n.cx2;
        } else {
          n.y = 2.0 * cy - n.y;
          n.cy1 = 2.0 * cy - n.cy1;
          n.cy2 = 2.0 * cy - n.cy2;
        }
      }
      path_snaps.push_back({leaf->internal_id, before, *leaf->path});
    }
  }

  if (m_history) {
    std::string desc = horizontal ? "Flip horizontal" : "Flip vertical";
    if (!path_snaps.empty())
      m_history->push(
          std::make_unique<ScaleObjectsCommand>(
              project(), std::move(path_snaps), desc));
    if (!img_snaps.empty())
      m_history->push(
          std::make_unique<ScaleImageCommand>(
              project(), std::move(img_snaps), desc));
  }

  m_sig_doc_changed.emit();
  queue_draw();
  {
    MacroStep s;
    s.op = horizontal ? MacroStep::Op::FlipH : MacroStep::Op::FlipV;
    record_step_if_recording(s);
  }
  LOG_INFO("Canvas: flip_{}", horizontal ? "horizontal" : "vertical");
}

// ── Macro playback helpers
// ────────────────────────────────────────────────────

// Parse "#RRGGBB" → r,g,b in [0,1].  Returns false if unparseable.
static bool parse_hex_color(const std::string &hex, double &r, double &g,
                            double &b) {
  if (hex.size() != 7 || hex[0] != '#')
    return false;
  auto h2 = [&](int pos) {
    auto c = [](char ch) -> int {
      if (ch >= '0' && ch <= '9')
        return ch - '0';
      if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
      if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
      return 0;
    };
    return c(hex[pos]) * 16 + c(hex[pos + 1]);
  };
  r = h2(1) / 255.0;
  g = h2(3) / 255.0;
  b = h2(5) / 255.0;
  return true;
}

void Canvas::apply_fill_to_selection(const std::string &hex) {
  if (m_selection.empty() || !m_doc)
    return;
  for (SceneNode *obj : m_selection) {
    FillStyle fs;
    double r, g, b;
    if (hex.empty()) {
      fs.type = FillStyle::Type::CurrentColor;
    } else if (parse_hex_color(hex, r, g, b)) {
      fs.type = FillStyle::Type::Solid;
      fs.r = r;
      fs.g = g;
      fs.b = b;
    }
    style::mutate_appearance(*obj, [&](SceneNode& n) {
      n.fill = fs;
    });
  }
  m_sig_doc_changed.emit();
  queue_draw();
}

void Canvas::apply_stroke_to_selection(const std::string &hex) {
  if (m_selection.empty() || !m_doc)
    return;
  for (SceneNode *obj : m_selection) {
    double r, g, b;
    style::mutate_appearance(*obj, [&](SceneNode& n) {
      if (hex.empty()) {
        n.stroke.paint.type = FillStyle::Type::None;
      } else if (parse_hex_color(hex, r, g, b)) {
        n.stroke.paint.type = FillStyle::Type::Solid;
        n.stroke.paint.r = r;
        n.stroke.paint.g = g;
        n.stroke.paint.b = b;
      }
    });
  }
  m_sig_doc_changed.emit();
  queue_draw();
}

// S90 Stage 1 — debug-only. Builds a 3-stop linear gradient (red → yellow
// → blue, horizontal in objectBoundingBox space) and assigns it to every
// selected shape's fill. Bypasses style::mutate_appearance because there's
// no undo or swatch tracking yet — Stage 1 is purely "prove the render
// path." Removed once the gradient editor lands.
void Canvas::debug_apply_test_gradient() {
  if (m_selection.empty() || !m_doc) {
    LOG_WARN("Canvas: debug_apply_test_gradient — nothing selected");
    return;
  }
  FillStyle g;
  g.type = FillStyle::Type::LinearGradient;
  g.g_x1 = 0.0; g.g_y1 = 0.5;
  g.g_x2 = 1.0; g.g_y2 = 0.5;
  g.stops = {
    { 0.0,  1.0, 0.10, 0.10, 1.0 },  // red
    { 0.5,  1.0, 0.85, 0.10, 1.0 },  // yellow
    { 1.0,  0.10, 0.30, 1.00, 1.0 }, // blue
  };
  for (SceneNode *obj : m_selection) {
    obj->fill = g;
  }
  LOG_INFO("Canvas: debug test gradient applied to {} object(s)",
           m_selection.size());
  m_sig_doc_changed.emit();
  queue_draw();
}

// ── Swatch apply (Phase 5 M3) ────────────────────────────────────────────────
// Routes through library->set_paint for each selected object. Today this
// writes the resolved colour into the object's FillStyle and fires the
// library's paint-changed signal so the SwatchesPanel's active-paint ring
// refreshes. An upcoming milestone adds fill_swatch_id / stroke_swatch_id
// fields to SceneNode; set_paint will also write those, at which point
// this path carries full binding identity through to save/load.
//
// Until then, this method behaves identically to apply_fill_to_selection /
// apply_stroke_to_selection from the user's perspective — the difference
// is plumbing-only. We keep the separate call site because the future
// binding work piggybacks on it without needing to rewire callers again.
//
// Behaviour if the library isn't wired: log a warning and no-op. We don't
// silently fall back to a raw colour apply — a missing library is an
// integration bug and we'd rather surface it.
void Canvas::apply_swatch_to_selection(const color::SwatchId &swatch_id,
                                       color::PaintSlot slot) {
  if (m_selection.empty() || !m_doc)
    return;

  if (!m_swatch_library) {
    LOG_WARN("Canvas::apply_swatch_to_selection: no swatch library wired; "
             "falling back to raw colour apply (binding will be lost)");
    const color::Swatch *sw = nullptr;
    // Can't look it up — no library. Nothing to fall back to. Give up.
    (void)sw;
    return;
  }

  const color::Swatch *sw = m_swatch_library->find_swatch(swatch_id);
  if (!sw) {
    LOG_WARN("Canvas::apply_swatch_to_selection: swatch '{}' not found",
             swatch_id);
    return;
  }

  // Fallback colour for the SwatchRef. The library will overwrite this on
  // its set_paint call (resolving through the library), but we pre-seed
  // with the current resolved colour so the ref is well-formed even if
  // some edge case skipped the library's own refresh.
  color::Color fallback{};
  if (const auto *solid = std::get_if<color::SolidSwatch>(sw)) {
    fallback = solid->color;
  }
  // Future gradient swatches would resolve to a representative colour
  // here — Phase 4.5 territory. For M3 only solid swatches exist.

  color::Paint ref = color::SwatchRef{swatch_id, fallback};

  // S82 m4g: push BindSwatchCommand for proper undo. Mutate-then-push
  // pattern (matches every other history-pushing site in this file):
  // capture pre-bind state per target, route the actual mutation
  // through set_paint (so cache + signal emission match production
  // semantics), then read post-bind state for the after snapshot.
  // ONE atomic command across the whole selection — Ctrl+Z restores
  // every target in a single step.
  std::vector<BindSwatchCommand::TargetSnap> snaps;
  snaps.reserve(m_selection.size());
  for (SceneNode *obj : m_selection) {
    if (!obj)
      continue;
    BindSwatchCommand::TargetSnap ts;
    ts.obj                     = obj;
    ts.bound_style_before      = obj->bound_style;
    ts.fill_before             = obj->fill;
    ts.fill_swatch_id_before   = obj->fill_swatch_id;
    ts.stroke_before           = obj->stroke;
    ts.stroke_swatch_id_before = obj->stroke_swatch_id;
    // set_paint funnels through mutate_appearance which clears
    // bound_style + both swatch ids, then writes the new id for
    // the chosen slot. signal_paint_changed fires for the swatches
    // panel's active-paint ring.
    m_swatch_library->set_paint(*obj, slot, ref);
    // Read post-mutation cache for the after snapshot. BindSwatch
    // Command::execute replays this directly on redo (cached
    // after-state policy — see BindSwatchCommand header for why
    // we don't re-resolve through the library on redo).
    ts.fill_after   = obj->fill;
    ts.stroke_after = obj->stroke;
    snaps.push_back(std::move(ts));
  }

  if (m_history && !snaps.empty()) {
    const bool multi = snaps.size() > 1;
    m_history->push(std::make_unique<BindSwatchCommand>(
        m_swatch_library,
        slot,
        swatch_id,
        std::move(snaps),
        multi ? std::string("Bind swatch (multi)")
              : std::string("Bind swatch")));
  }

  m_sig_doc_changed.emit();
  queue_draw();
}

// ── Canvas::unbind_swatch_from_doc (S83 m4h v4) ──────────────────────────────
//
// Walk every layer in the active document and clear fill_swatch_id /
// stroke_swatch_id on any node whose binding matches `id`. Cached fill /
// stroke (the actual rendered colours) are deliberately untouched —
// break-on-override v1 says the moment-of-unbind appearance is what the
// user keeps.
//
// Caller: SwatchesPanel::on_ctx_delete_swatch, BEFORE
// SwatchLibrary::remove_swatch runs. The library's remove path itself
// has a comment acknowledging it deliberately leaves dangling ids and
// defers cleanup to a "delete-with-usage-check flow" — this method is
// the no-confirm version of that flow. (The full confirm-with-usage-
// counts dialog is still on the backlog.)
//
// Not undoable in v1 — matches the existing un-undoable swatch-create
// path; both are tracked as backlog items (DeleteSwatchCommand +
// AddSwatchCommand). Until those land, the user gets the visual /
// inspector outcome they expect (binding gone, colour preserved) but
// Ctrl+Z won't bring the swatch back. Acceptable for v4; the
// alternative is making delete itself undoable, which is a larger
// scope than m4h.
void Canvas::unbind_swatch_from_doc(const color::SwatchId& id) {
  if (id.empty() || !m_doc) return;
  for (auto& L : m_doc->layers)
    unbind_swatch_walk(L.get(), id);
  // No m_sig_doc_changed.emit() — the visual hasn't changed (cache
  // preserved), and the caller will fire its own signal_inspector_
  // refresh_needed for the panel-side update. Keeping this method
  // narrow — it does the structural unbind and nothing else.
}

void Canvas::apply_stroke_width_to_selection(double width) {
  if (m_selection.empty() || !m_doc)
    return;
  for (SceneNode *obj : m_selection) {
    style::mutate_appearance(*obj, [width](SceneNode& n) {
      n.stroke.width = width;
    });
  }
  m_sig_doc_changed.emit();
  queue_draw();
}

void Canvas::apply_opacity_to_selection(double opacity) {
  if (m_selection.empty() || !m_doc)
    return;
  const double v = std::clamp(opacity, 0.0, 1.0);
  // Group-flatten rule (s108 m2): when the user sets opacity on a
  // group-like container, the visual intent is "this group as a unit
  // looks V transparent" — not "compound V on top of whatever member
  // opacities already have." Flatten direct members to 1.0 so the
  // group's own opacity carries the full effect. The structural change
  // is triggered only by an explicit user/macro action; load/parse
  // doesn't reach here, so externally-authored compounded group
  // opacity round-trips intact when untouched.
  auto is_grouplike = [](const SceneNode *n) {
    return n && (n->type == SceneNode::Type::Group ||
                 n->type == SceneNode::Type::Compound ||
                 n->type == SceneNode::Type::ClipGroup ||
                 n->type == SceneNode::Type::Blend ||
                 n->type == SceneNode::Type::Warp);
  };
  // Capture pre-edit snapshots BEFORE writing — group + every flattened
  // child gets a Snap so undo restores the full pre-edit state. Track
  // already-snapped nodes by iid to avoid double-recording when a
  // group and one of its members are both selected (rare but possible
  // via shift-click).
  // s170 m1 — iid-based capture: Snap stores obj_iid (string) instead of
  // a raw SceneNode*, dedup set is keyed on iid string, push passes
  // project() so execute()/undo() can resolve via find_by_iid.
  std::vector<SetOpacityCommand::Snap> snaps;
  std::unordered_set<std::string> seen;
  auto snap_and_write = [&](SceneNode *n, double after) {
    if (!n) return;
    const std::string& iid = n->internal_id;
    if (iid.empty() || !seen.insert(iid).second) return;
    snaps.push_back({iid, n->opacity, after});
    n->opacity = after;
  };
  for (SceneNode *obj : m_selection) {
    if (!obj) continue;
    snap_and_write(obj, v);
    if (is_grouplike(obj)) {
      for (auto &child : obj->children)
        if (child) snap_and_write(child.get(), 1.0);
    }
  }
  if (snaps.empty()) return;
  if (m_history)
    m_history->push(std::make_unique<SetOpacityCommand>(
        project(), std::move(snaps), "Set opacity"));
  m_sig_doc_changed.emit();
  queue_draw();
  // Macro recording (s108 m3): record the SetOpacity step at the same
  // seam that does the work. Replay re-enters this function but
  // is_recording is false during replay, so record_step_if_recording
  // no-ops — no double-record. Mirrors the pattern at every other
  // canvas-side commit site (flip, transform, etc).
  {
    MacroStep s;
    s.op    = MacroStep::Op::SetOpacity;
    s.value = v;
    record_step_if_recording(s);
  }
}

void Canvas::rotate_selection_by(double angle_deg, double pivot_x,
                                 double pivot_y, bool pivot_explicit) {
  if (m_selection.empty() || !m_doc)
    return;

  // Compute pivot from selection bbox if not explicit
  if (!pivot_explicit) {
    double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
    for (SceneNode *obj : m_selection) {
      auto bb = object_bbox(*obj, false);
      if (!bb)
        continue;
      bx1 = std::min(bx1, bb->x);
      by1 = std::min(by1, bb->y);
      bx2 = std::max(bx2, bb->x + bb->w);
      by2 = std::max(by2, bb->y + bb->h);
    }
    pivot_x = (bx1 + bx2) * 0.5;
    pivot_y = (by1 + by2) * 0.5;
  }

  double rad = angle_deg * M_PI / 180.0;
  double cosA = std::cos(rad), sinA = std::sin(rad);

  std::vector<ScaleObjectsCommand::LeafSnap> snaps;
  for (SceneNode *obj : m_selection) {
    std::vector<SceneNode *> leaves;
    collect_paths(obj, leaves);
    for (SceneNode *leaf : leaves) {
      if (!leaf->path)
        continue;
      PathData before = *leaf->path;
      for (auto &nd : leaf->path->nodes) {
        auto rot = [&](double ox, double oy, double &rx, double &ry) {
          double rx0 = ox - pivot_x, ry0 = oy - pivot_y;
          rx = pivot_x + rx0 * cosA - ry0 * sinA;
          ry = pivot_y + rx0 * sinA + ry0 * cosA;
        };
        rot(nd.x, nd.y, nd.x, nd.y);
        rot(nd.cx1, nd.cy1, nd.cx1, nd.cy1);
        rot(nd.cx2, nd.cy2, nd.cx2, nd.cy2);
      }
      snaps.push_back({leaf->internal_id, before, *leaf->path});
    }
  }
  if (m_history && !snaps.empty())
    m_history->push(std::make_unique<ScaleObjectsCommand>(
        project(), std::move(snaps), "Rotate object"));
  m_sig_doc_changed.emit();
  queue_draw();
}

void Canvas::scale_selection_by(double sx, double sy) {
  if (m_selection.empty() || !m_doc)
    return;

  // Scale around bbox centre
  double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
  for (SceneNode *obj : m_selection) {
    auto bb = object_bbox(*obj, false);
    if (!bb)
      continue;
    bx1 = std::min(bx1, bb->x);
    by1 = std::min(by1, bb->y);
    bx2 = std::max(bx2, bb->x + bb->w);
    by2 = std::max(by2, bb->y + bb->h);
  }
  double cx = (bx1 + bx2) * 0.5, cy = (by1 + by2) * 0.5;

  std::vector<ScaleObjectsCommand::LeafSnap> snaps;
  for (SceneNode *obj : m_selection) {
    std::vector<SceneNode *> leaves;
    collect_paths(obj, leaves);
    for (SceneNode *leaf : leaves) {
      if (!leaf->path)
        continue;
      PathData before = *leaf->path;
      for (auto &nd : leaf->path->nodes) {
        auto sc = [&](double ox, double oy, double &rx, double &ry) {
          rx = cx + (ox - cx) * sx;
          ry = cy + (oy - cy) * sy;
        };
        sc(nd.x, nd.y, nd.x, nd.y);
        sc(nd.cx1, nd.cy1, nd.cx1, nd.cy1);
        sc(nd.cx2, nd.cy2, nd.cx2, nd.cy2);
      }
      snaps.push_back({leaf->internal_id, before, *leaf->path});
    }
  }
  if (m_history && !snaps.empty())
    m_history->push(std::make_unique<ScaleObjectsCommand>(
        project(), std::move(snaps), "Scale object"));
  m_sig_doc_changed.emit();
  queue_draw();
}

// ── run_macro ────────────────────────────────────────────────────────────────
// Plays back a macro on the current selection.
// If selection is ungrouped, runs once per selected object.
// If selection is a single group or compound, runs once on the whole selection.
void Canvas::run_macro(const std::string &macro_id, int from_step) {
  Macro *macro = MacroManager::instance().find_macro(macro_id);
  if (!macro || macro->steps.empty())
    return;
  if (m_selection.empty() || !m_doc)
    return;

  // Determine if we iterate per-object or treat selection as a unit
  bool iterate_per_object = false;
  for (SceneNode *obj : m_selection) {
    if (obj->type == SceneNode::Type::Path ||
        obj->type == SceneNode::Type::Text) {
      iterate_per_object = true;
      break;
    }
  }

  auto run_steps = [&](const std::vector<SceneNode *> &sel) {
    // Temporarily set selection to this object set
    auto saved_sel = m_selection;
    auto saved_selected = m_selected;
    m_selection = sel;
    m_selected = sel.empty() ? nullptr : sel[0];

    int end = (int)macro->steps.size();
    for (int i = std::max(0, from_step); i < end; ++i) {
      const MacroStep &s = macro->steps[i];
      switch (s.op) {
      case MacroStep::Op::DuplicateInPlace:
        duplicate_in_place_selected();
        break;
      case MacroStep::Op::Duplicate:
        duplicate_selected();
        break;
      case MacroStep::Op::Delete:
        delete_selected();
        break;
      case MacroStep::Op::Group:
        group_selection();
        break;
      case MacroStep::Op::Ungroup:
        ungroup_selection();
        break;

      case MacroStep::Op::Move: {
        // Translate all path nodes
        std::vector<ScaleObjectsCommand::LeafSnap> snaps;
        for (SceneNode *obj : m_selection) {
          std::vector<SceneNode *> leaves;
          collect_paths(obj, leaves);
          for (SceneNode *leaf : leaves) {
            if (!leaf->path)
              continue;
            PathData before = *leaf->path;
            for (auto &nd : leaf->path->nodes) {
              nd.x += s.dx;
              nd.y += s.dy;
              nd.cx1 += s.dx;
              nd.cy1 += s.dy;
              nd.cx2 += s.dx;
              nd.cy2 += s.dy;
            }
            snaps.push_back({leaf->internal_id, before, *leaf->path});
          }
          if (obj->is_text()) {
            obj->text_x += s.dx;
            obj->text_y += s.dy;
          }
          if (obj->is_image()) {
            obj->image_x += s.dx;
            obj->image_y += s.dy;
          }
        }
        if (m_history && !snaps.empty())
          m_history->push(std::make_unique<ScaleObjectsCommand>(
              project(), std::move(snaps), "Move object"));
        m_sig_doc_changed.emit();
        queue_draw();
        break;
      }

      case MacroStep::Op::Scale:
        scale_selection_by(s.scale_x, s.scale_y);
        break;

      case MacroStep::Op::Rotate:
        rotate_selection_by(s.angle_deg, s.pivot_x, s.pivot_y,
                            s.pivot_is_explicit);
        break;

      case MacroStep::Op::FlipH:
        flip_selection(true);
        break;
      case MacroStep::Op::FlipV:
        flip_selection(false);
        break;

      case MacroStep::Op::BringToFront:
        arrange(ArrangeOp::BringToFront);
        break;
      case MacroStep::Op::BringForward:
        arrange(ArrangeOp::BringForward);
        break;
      case MacroStep::Op::SendBackward:
        arrange(ArrangeOp::SendBackward);
        break;
      case MacroStep::Op::SendToBack:
        arrange(ArrangeOp::SendToBack);
        break;

      case MacroStep::Op::BooleanUnion:
        boolean_op(BooleanOpType::Union);
        break;
      case MacroStep::Op::BooleanSubtract:
        boolean_op(BooleanOpType::Subtract);
        break;
      case MacroStep::Op::BooleanIntersect:
        boolean_op(BooleanOpType::Intersect);
        break;

      case MacroStep::Op::OffsetPath:
        offset_path_op(s.value, OffsetSide::Both, false);
        break;

      case MacroStep::Op::SetFill:
        apply_fill_to_selection(s.color_hex);
        break;
      case MacroStep::Op::SetStroke:
        apply_stroke_to_selection(s.color_hex);
        break;
      case MacroStep::Op::SetStrokeWidth:
        apply_stroke_width_to_selection(s.value);
        break;
      case MacroStep::Op::SetOpacity:
        apply_opacity_to_selection(s.value);
        break;

      // Alignment ops — run on whole selection regardless of iterate mode
      case MacroStep::Op::AlignLeft:
        align_selection(AlignOp::AlignLeft);
        break;
      case MacroStep::Op::AlignCenterH:
        align_selection(AlignOp::AlignCenterH);
        break;
      case MacroStep::Op::AlignRight:
        align_selection(AlignOp::AlignRight);
        break;
      case MacroStep::Op::AlignTop:
        align_selection(AlignOp::AlignTop);
        break;
      case MacroStep::Op::AlignMiddleV:
        align_selection(AlignOp::AlignCenterV);
        break;
      case MacroStep::Op::AlignBottom:
        align_selection(AlignOp::AlignBottom);
        break;
      case MacroStep::Op::DistributeH:
        align_selection(AlignOp::DistributeH);
        break;
      case MacroStep::Op::DistributeV:
        align_selection(AlignOp::DistributeV);
        break;

      case MacroStep::Op::ReversePath:
        reverse_selected_path();
        break;
      }
    }

    // Restore selection
    m_selection = saved_sel;
    m_selected = saved_selected;
  };

  if (iterate_per_object) {
    // Run once per top-level selected object
    for (SceneNode *obj : m_selection)
      run_steps({obj});
    // Restore after iteration
    notify_object_selection_changed();
  } else {
    // Run on whole selection as a unit (group / compound)
    run_steps(m_selection);
    notify_object_selection_changed();
  }

  LOG_INFO("Canvas: run_macro '{}' from_step={} on {} object(s)", macro->name,
           from_step, m_selection.size());
}

// ── step_repeat (legacy 3-arg)
// ──────────────────────────────────────────────────────── Duplicates the
// current selection `copies` times, each copy offset by (dx*i, dy*i) relative
// to the original.  All copies are one atomic undo step.  Translation-only;
// delegates to the extended overload with rotate_enabled=false.
void Canvas::step_repeat(int copies, double dx, double dy) {
  step_repeat(copies, dx, dy, /*rotate_enabled=*/false,
              /*angle_deg=*/0.0, /*pivot_x=*/0.0, /*pivot_y=*/0.0);
}

// ── step_repeat (extended with rotate-around-pivot)
// ──────────────────────────────────────── Two modes:
//
// 1. rotate_enabled = false  →  translation only.  Each copy i is offset by
//    (dx*i, dy*i).  Old behaviour preserved.
//
// 2. rotate_enabled = true   →  orbit-and-spin.  dx/dy ignored.  Let C0 =
//    group bbox centre, r = distance(refpt, C0), θ0 = atan2(C0 - refpt).
//    For each copy i:
//       θi = θ0 + angle_deg*i
//       Ci = refpt + r·(cos θi, sin θi)      (orbit position)
//       Translate clone by (Ci - C0), then rotate in place by angle_deg*i
//       around Ci.
//    A multi-object selection is treated as a rigid group: same Ci and
//    same per-step spin applied to every object in the selection.
//
// All copies form a single atomic undo step.  pivot is in doc coords,
// Y-down.  Angle sign matches Canvas::rotate_selection_by (positive = CW
// in doc-Y-down space, which reads as CW visually).
void Canvas::step_repeat(int copies, double dx, double dy, bool rotate_enabled,
                         double angle_deg, double pivot_x, double pivot_y) {
  if (m_selection.empty() || !m_doc || copies < 1)
    return;

  auto entries = collect_selection_entries(m_doc, m_selection);
  if (entries.empty())
    return;

  // ── Orbit-mode precomputation ──────────────────────────────────────────
  // C0: group bbox centre of the CURRENT selection (original, pre-copy).
  // r, theta0: polar coords of C0 relative to refpt.
  double C0x = 0.0, C0y = 0.0, r = 0.0, theta0 = 0.0;
  if (rotate_enabled) {
    double bx = 0.0, by = 0.0, bw = 0.0, bh = 0.0;
    if (!selection_bbox(bx, by, bw, bh)) {
      // Can't compute centre → fall back to translation-only (dx/dy ignored
      // since UI has them dimmed; copies will stack on original).
      rotate_enabled = false;
    } else {
      C0x = bx + bw * 0.5;
      C0y = by + bh * 0.5;
      double dxC = C0x - pivot_x;
      double dyC = C0y - pivot_y;
      r = std::hypot(dxC, dyC);
      theta0 = std::atan2(dyC, dxC);
    }
  }

  std::vector<StepRepeatCommand::Entry> cmd_entries;
  std::vector<SceneNode *> new_selection;
  int id_counter = s_next_id;

  // Insert above the originals; track shift as we accumulate insertions.
  int shift = 0;
  for (int step = 1; step <= copies; ++step) {
    // ── Per-step transform parameters ────────────────────────────────
    // Translation-only mode uses (dx*step, dy*step).
    // Orbit mode computes a translation (Ci - C0) plus a rotation by
    // step_angle around Ci.
    double tx = 0.0, ty = 0.0;
    double cosA = 1.0, sinA = 0.0;
    double Cix = 0.0, Ciy = 0.0;

    if (rotate_enabled) {
      double step_angle = angle_deg * (double)step * M_PI / 180.0;
      double theta_i = theta0 + step_angle;
      Cix = pivot_x + r * std::cos(theta_i);
      Ciy = pivot_y + r * std::sin(theta_i);
      tx = Cix - C0x;
      ty = Ciy - C0y;
      cosA = std::cos(step_angle);
      sinA = std::sin(step_angle);
    } else {
      tx = dx * step;
      ty = dy * step;
    }

    auto xform = [&](double &x, double &y) {
      if (!rotate_enabled) {
        x += tx;
        y += ty;
        return;
      }
      // Orbit mode: translate by (Ci - C0) so the point that used to be at
      // C0 is now at Ci, then rotate around Ci in place.
      double tx_pt = x + tx;
      double ty_pt = y + ty;
      double rx = tx_pt - Cix;
      double ry = ty_pt - Ciy;
      x = Cix + rx * cosA - ry * sinA;
      y = Ciy + rx * sinA + ry * cosA;
    };

    for (auto &e : entries) {
      auto dup = clone_node(*e.node);
      freshen_ids(dup.get(), m_doc, id_counter);

      std::vector<SceneNode *> paths;
      collect_paths(dup.get(), paths);
      for (SceneNode *p : paths) {
        if (!p->path)
          continue;
        for (auto &n : p->path->nodes) {
          xform(n.x, n.y);
          xform(n.cx1, n.cy1);
          xform(n.cx2, n.cy2);
        }
      }
      if (dup->type == SceneNode::Type::Text) {
        xform(dup->text_x, dup->text_y);
      }

      int ins = e.index + shift;
      auto snap = clone_node(*dup);
      new_selection.push_back(dup.get());
      e.parent->children.insert(e.parent->children.begin() + ins,
                                std::move(dup));
      cmd_entries.push_back({e.parent->internal_id, std::move(snap), ins});
      ++shift;
    }
  }
  s_next_id = id_counter;

  if (m_history)
    m_history->push(
        std::make_unique<StepRepeatCommand>(project(), std::move(cmd_entries)));

  // Select all new copies
  m_selection = new_selection;
  m_selected = new_selection.empty() ? nullptr : new_selection[0];
  m_selected_node = -1;
  notify_object_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: step_repeat copies={} dx={:.2f} dy={:.2f} "
           "rotate={} angle={:.2f} refpt=({:.2f},{:.2f}) total={}",
           copies, dx, dy, rotate_enabled ? 1 : 0, angle_deg, pivot_x, pivot_y,
           new_selection.size());
}

// ── set_step_repeat_preview
// ─────────────────────────────────────────── Dialog-owned crosshair
// overlay, separate from rotate-from-point's m_custom_pivot_*.
void Canvas::set_step_repeat_preview(bool active, double px, double py) {
  m_sr_preview_active = active;
  m_sr_preview_x = px;
  m_sr_preview_y = py;
  if (!active) {
    m_sr_pivot_dragging = false;
  }
  queue_draw();
}

void Canvas::set_step_repeat_pivot_callback(
    std::function<void(double, double)> cb) {
  m_sr_pivot_change_cb = std::move(cb);
}

void Canvas::set_selected_nodes_type(BezierNode::Type type) {
  if (m_node_selection.empty() || !m_selected || !m_selected->path)
    return;
  if (m_node_drag_started)
    return;

  // Snapshot before state for undo
  PathData before = *m_selected->path;

  BezierPath bp = BezierPath::from_path_data(*m_selected->path);
  for (const auto &ns : m_node_selection) {
    if (ns.obj == m_selected && ns.node_idx >= 0 &&
        ns.node_idx < (int)bp.nodes.size())
      bp.set_node_type(ns.node_idx, type);
  }
  *m_selected->path = bp.to_path_data();

  if (m_history)
    m_history->push(std::make_unique<EditPathCommand>(
        project(), m_selected->internal_id,
        std::move(before), *m_selected->path, "Set node type"));

  m_sig_doc_changed.emit();
  queue_draw();
}

void Canvas::apply_corner_treatment_op(CornerType type, double radius) {
  LOG_INFO("corner_op: ENTER type={} radius={:.6f} selection_size={}",
           type == CornerType::Round ? "Round"
             : type == CornerType::Chamfer ? "Chamfer" : "InverseRound",
           radius, m_corner_selection.size());
  if (m_corner_selection.empty() || !m_doc || !m_history) {
    LOG_INFO("corner_op: EARLY_EXIT empty_sel={} no_doc={} no_history={}",
             m_corner_selection.empty(), !m_doc, !m_history);
    return;
  }

  // Group selected nodes by path object
  std::unordered_map<SceneNode *, std::unordered_set<int>> by_obj;
  for (auto &cs : m_corner_selection)
    by_obj[cs.obj].insert(cs.node_idx);

  LOG_INFO("corner_op: grouped into {} object(s)", by_obj.size());

  auto cmd = std::make_unique<CornerTreatmentCommand>(project());
  bool any = false;

  for (auto &[obj, indices] : by_obj) {
    if (!obj->path) {
      LOG_INFO("corner_op: skipping obj (no path)");
      continue;
    }
    LOG_INFO("corner_op: obj has {} requested indices, path closed={} nodes={}",
             indices.size(), obj->path->closed, obj->path->nodes.size());
    PathData before = *obj->path;
    PathData after =
        Curvz::apply_corner_treatment(*obj->path, indices, type, radius);
    LOG_INFO("corner_op: before.size={} after.size={} -> {}",
             before.nodes.size(), after.nodes.size(),
             after.nodes.size() != before.nodes.size() ? "CHANGED" : "NO_CHANGE");
    // Only record if geometry actually changed
    if (after.nodes.size() != before.nodes.size()) {
      cmd->add(obj, std::move(before), after);
      *obj->path = std::move(after);
      any = true;
    }
  }

  LOG_INFO("corner_op: any={} (pushing cmd={})", any, any);

  if (any) {
    m_history->push(std::move(cmd));
    m_sig_doc_changed.emit();
    queue_draw();
  }

  // Clear selection and notify panel to hide
  m_corner_selection.clear();
  m_sig_corner_sel_changed.emit(0);
  queue_draw();
}

// Works from any tool — scans m_selection for text nodes with text_path_id set,
// or path nodes whose partner text node is implicitly in the pair.
// Each detach is pushed as an undoable LinkTextToPathCommand
// (after_path_id="").
void Canvas::release_text_from_path() {
  if (!m_doc)
    return;

  // Collect all text nodes to release — from m_selection directly, or via
  // partner lookup if the user selected the guide path side of the pair.
  std::vector<SceneNode *> to_release;
  auto add_if_linked = [&](SceneNode *n) {
    if (!n || n->text_path_id.empty())
      return;
    if (std::find(to_release.begin(), to_release.end(), n) == to_release.end())
      to_release.push_back(n);
  };
  for (SceneNode *obj : m_selection) {
    if (obj->is_text())
      add_if_linked(obj);
    else if (obj->is_path()) {
      SceneNode *partner = top_pair_partner(obj);
      if (partner && partner->is_text())
        add_if_linked(partner);
    }
  }
  // Also check m_top_text for when we're in the TOP tool
  if (m_top_text)
    add_if_linked(m_top_text);

  if (to_release.empty())
    return;

  for (SceneNode *tn : to_release) {
    // Compute where the text anchor sits on the path so the detached
    // node lands where it was visually, making it immediately re-selectable.
    double detach_x = tn->text_x, detach_y = tn->text_y;
    top_compute_detach_position(*tn, detach_x, detach_y);

    if (m_history) {
      m_history->push(std::make_unique<LinkTextToPathCommand>(
          project(), tn->internal_id,
          tn->text_path_id, tn->text_path_offset, tn->text_path_flip,
          tn->text_x, tn->text_y,               // before x/y
          "", 0.0, false, detach_x, detach_y)); // after x/y
    }
    tn->text_x = detach_x;
    tn->text_y = detach_y;
    tn->text_path_id = "";
    tn->text_path_offset = 0.0;
    tn->text_path_flip = false;
  }

  // Reset TOP tool state if active
  if (m_tool == ActiveTool::TextOnPath) {
    m_top_text = nullptr;
    m_top_path_node = nullptr;
    m_top_phase = 0;
    m_top_dragging = false;
  }

  m_sig_doc_changed.emit();
  notify_object_selection_changed();
  queue_draw();
  LOG_DEBUG("release_text_from_path: released {} text node(s)",
            to_release.size());
}

} // namespace Curvz
