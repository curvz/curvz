// curvz_utils.cpp ─────────────────────────────────────────────────────────
// See curvz_utils.hpp for design notes and the SVG-id contract.

#include "curvz_utils.hpp"
#include "SceneNode.hpp"        // FillStyle, GradientStop, SceneNode walk
#include "CurvzDocument.hpp"    // s132 m2 — doc.layers for counting pumps
#include "CurvzLog.hpp"    // s126: diagnostics for confirm callback path

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

// s125 m2a — dialog factory. Heavy widget includes go here, not in the
// header, so consumers of the small utility pumps don't pay for them.
#include <gtkmm/adjustment.h>    // Gtk::Adjustment::create for SpinButton
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>  // s145 m4 — focus-leave commit in path-row
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/filedialog.h>    // s145 m4 — Browse button in path-override row
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <giomm/file.h>          // s145 m4 — FileDialog set_initial_folder/file
#include <gdk/gdkkeysyms.h>      // GDK_KEY_Escape, GDK_KEY_Return
#include <glibmm/main.h>         // s126: Glib::signal_idle for deferred delete
#include <filesystem>            // s145 m4 — initial-folder seed in path-row

namespace curvz::utils {

// Pilcrow separator. UTF-8: 0xC2 0xB6. Defined here once.
static const std::string kPilcrow = "\xC2\xB6";

// ── sanitise_for_xml_id ─────────────────────────────────────────────
std::string sanitise_for_xml_id(const std::string& name) {
    std::string out;
    out.reserve(name.size());

    bool last_was_underscore = false;
    size_t i = 0;
    while (i < name.size()) {
        // Strip the pilcrow (key trap). UTF-8 byte pair.
        if (i + 1 < name.size() &&
            (unsigned char)name[i]     == 0xC2 &&
            (unsigned char)name[i + 1] == 0xB6) {
            i += 2;
            continue;
        }

        const unsigned char c = (unsigned char)name[i];

        if (std::isspace(c)) {
            if (!last_was_underscore) {
                out.push_back('_');
                last_was_underscore = true;
            }
            ++i;
            continue;
        }

        const bool is_alnum   = std::isalnum(c) != 0;
        const bool is_allowed = is_alnum || c == '-' || c == '_' || c == '.';
        if (is_allowed) {
            out.push_back((char)c);
            last_was_underscore = (c == '_');
            ++i;
            continue;
        }

        // Anything else (specials, multi-byte Unicode) is dropped.
        ++i;
    }

    // XML id can't start with a digit, hyphen, or dot. If we landed on
    // one of those (or empty), prepend '_'.
    if (out.empty() ||
        std::isdigit((unsigned char)out[0]) ||
        out[0] == '-' || out[0] == '.') {
        out.insert(out.begin(), '_');
    }

    return out;
}

// ── short_iid ───────────────────────────────────────────────────────
std::string short_iid(const std::string& iid) {
    if (iid.size() <= 8) return iid;
    return iid.substr(0, 8);
}

// ── encode_svg_id ───────────────────────────────────────────────────
std::string encode_svg_id(const std::string& name,
                          const std::string& iid) {
    const bool name_empty = name.empty();
    const bool iid_empty  = iid.empty();

    if (name_empty && iid_empty) return "";

    const std::string clean_name = name_empty ? "" : sanitise_for_xml_id(name);
    const std::string short_part = short_iid(iid);

    if (name_empty) return short_part;
    if (iid_empty)  return clean_name;
    return clean_name + kPilcrow + short_part;
}

// ── Document counting pumps (s132 m2) ────────────────────────────────
//
// Five sites in MainWindow had open-coded `iterate doc.layers, sum
// children.size()` with `nodes=0` hardcoded — selection / undo / redo /
// doc-active-changed / doc-changed all duplicating the same loop. The
// node-counter in the StatusBar has therefore read "0 nodes" since day
// one because there was no quick recursive walk and nobody wanted to
// inline one at five sites.
//
// Replacing the lot with three pumps:
//   • count_anchors      — recursive over containers
//   • doc_anchor_count   — sum across non-overlay layers
//   • doc_object_count   — top-level object count, mirroring the
//                          existing duplicated loops
//
// Containers that recurse: Compound (multiple sub-paths), Group
// (arbitrary children), ClipGroup (geometric children, ignores the
// clip shape itself — that's not a user-visible "node" in the editing
// sense), Blend (its computed steps are children), Warp (the warped
// content is a child). MarginLayer / GridLayer / Guide are scaffolding
// and don't contribute. Text / Image / Ref / Measurement are leaves
// without anchors.

int count_anchors(const Curvz::SceneNode& n) {
    using T = Curvz::SceneNode::Type;
    switch (n.type) {
        case T::Path:
            return n.path ? (int)n.path->nodes.size() : 0;
        case T::Compound:
        case T::Group:
        case T::ClipGroup:
        case T::Blend:
        case T::Warp: {
            int total = 0;
            for (const auto& c : n.children) {
                if (c) total += count_anchors(*c);
            }
            return total;
        }
        default:
            return 0;
    }
}

int doc_anchor_count(const Curvz::CurvzDocument& doc) {
    int total = 0;
    for (const auto& l : doc.layers) {
        if (!l) continue;
        // Skip overlay/scaffolding layers — these aren't user content.
        if (l->is_guide_layer())   continue;
        if (l->is_grid_layer())    continue;
        if (l->is_margin_layer())  continue;
        if (l->is_ref_layer())     continue;
        if (l->is_measure_layer()) continue;
        for (const auto& c : l->children) {
            if (c) total += count_anchors(*c);
        }
    }
    return total;
}

int doc_object_count(const Curvz::CurvzDocument& doc) {
    int total = 0;
    for (const auto& l : doc.layers) {
        if (!l) continue;
        if (l->is_guide_layer())   continue;
        if (l->is_grid_layer())    continue;
        if (l->is_margin_layer())  continue;
        if (l->is_ref_layer())     continue;
        if (l->is_measure_layer()) continue;
        total += (int)l->children.size();
    }
    return total;
}

// ── box_blur_argb32 ─────────────────────────────────────────────────
// Three-pass separable box blur on Cairo ARGB32 pixels (premultiplied,
// BGRA byte order on little-endian). One pass is a horizontal row blur
// then a vertical column blur, producing a triangle filter; three passes
// approximate a Gaussian to within ~3% for the radii relevant to drop
// shadows.
//
// Edge handling: clamp-to-edge (pixels outside the surface read as the
// nearest edge pixel). This is the standard choice for shadows because
// it avoids the dark-fringe artefacts of zero-padding.
//
// Implementation note: rather than a naïve O(W * H * R) two-loop sum,
// this uses a sliding-window accumulator that's O(W * H) per pass,
// independent of radius. Each row pass walks the row left-to-right
// maintaining a running sum; on each step add the entering pixel and
// subtract the leaving pixel. Same trick on columns.
void box_blur_argb32(unsigned char* data, int stride,
                     int width, int height, int radius) {
    if (!data || width <= 0 || height <= 0 || radius <= 0) return;

    // Working buffer for the column pass (out-of-place to avoid aliasing
    // when stride > width*4). Reused across the three passes.
    std::vector<unsigned char> scratch((size_t)stride * (size_t)height);

    auto clamp = [](int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };

    const int kernel = 2 * radius + 1;

    // One pass = horizontal blur (data → scratch) then vertical blur
    // (scratch → data). Three passes ≈ Gaussian.
    for (int pass = 0; pass < 3; ++pass) {
        // ── Horizontal pass: write into scratch ──────────────────────
        for (int y = 0; y < height; ++y) {
            const unsigned char* src_row = data + (size_t)y * stride;
            unsigned char*       dst_row = scratch.data() + (size_t)y * stride;

            // Initial window sum: the 'kernel' pixels centred on x=0,
            // with reads outside [0, width) clamped to the edge pixel.
            unsigned int sb = 0, sg = 0, sr = 0, sa = 0;
            for (int k = -radius; k <= radius; ++k) {
                int xi = clamp(k, 0, width - 1);
                const unsigned char* p = src_row + xi * 4;
                sb += p[0]; sg += p[1]; sr += p[2]; sa += p[3];
            }

            for (int x = 0; x < width; ++x) {
                unsigned char* dp = dst_row + x * 4;
                dp[0] = (unsigned char)(sb / kernel);
                dp[1] = (unsigned char)(sg / kernel);
                dp[2] = (unsigned char)(sr / kernel);
                dp[3] = (unsigned char)(sa / kernel);

                // Slide window right by one: add (x+radius+1), drop (x-radius).
                int x_in  = clamp(x + radius + 1, 0, width - 1);
                int x_out = clamp(x - radius,     0, width - 1);
                const unsigned char* pin  = src_row + x_in  * 4;
                const unsigned char* pout = src_row + x_out * 4;
                sb += pin[0]; sb -= pout[0];
                sg += pin[1]; sg -= pout[1];
                sr += pin[2]; sr -= pout[2];
                sa += pin[3]; sa -= pout[3];
            }
        }

        // ── Vertical pass: read scratch, write data ──────────────────
        for (int x = 0; x < width; ++x) {
            // Initial window sum, vertical.
            unsigned int sb = 0, sg = 0, sr = 0, sa = 0;
            for (int k = -radius; k <= radius; ++k) {
                int yi = clamp(k, 0, height - 1);
                const unsigned char* p = scratch.data() + (size_t)yi * stride + x * 4;
                sb += p[0]; sg += p[1]; sr += p[2]; sa += p[3];
            }

            for (int y = 0; y < height; ++y) {
                unsigned char* dp = data + (size_t)y * stride + x * 4;
                dp[0] = (unsigned char)(sb / kernel);
                dp[1] = (unsigned char)(sg / kernel);
                dp[2] = (unsigned char)(sr / kernel);
                dp[3] = (unsigned char)(sa / kernel);

                int y_in  = clamp(y + radius + 1, 0, height - 1);
                int y_out = clamp(y - radius,     0, height - 1);
                const unsigned char* pin  = scratch.data() + (size_t)y_in  * stride + x * 4;
                const unsigned char* pout = scratch.data() + (size_t)y_out * stride + x * 4;
                sb += pin[0]; sb -= pout[0];
                sg += pin[1]; sg -= pout[1];
                sr += pin[2]; sr -= pout[2];
                sa += pin[3]; sa -= pout[3];
            }
        }
    }
}

// ── build_gradient_pattern ──────────────────────────────────────────
// Mirrors Canvas::apply_fill(cr, fill, bbox)'s gradient branch. Endpoints
// stored as 0..1 fractions of the shape's bbox; lerp into doc-space here.
// Empty stops → fully transparent pattern (a transparent solid). Non-
// gradient types → empty RefPtr. See header for parameter semantics.
Cairo::RefPtr<Cairo::Pattern> build_gradient_pattern(
    const Curvz::FillStyle& fill,
    double bbox_x, double bbox_y, double bbox_w, double bbox_h)
{
    using namespace Curvz;

    if (fill.type != FillStyle::Type::LinearGradient &&
        fill.type != FillStyle::Type::RadialGradient) {
        return {};
    }

    if (fill.stops.empty()) {
        // Match Canvas: a gradient with no stops paints transparent.
        // Returning a transparent solid lets the caller use this as a
        // drop-in replacement without special-casing.
        auto pat = Cairo::SolidPattern::create_rgba(0, 0, 0, 0);
        return pat;
    }

    // Lerp endpoints from 0..1 into doc coords.
    const double x1 = bbox_x + fill.g_x1 * bbox_w;
    const double y1 = bbox_y + fill.g_y1 * bbox_h;
    const double x2 = bbox_x + fill.g_x2 * bbox_w;
    const double y2 = bbox_y + fill.g_y2 * bbox_h;

    Cairo::RefPtr<Cairo::Gradient> pat;
    if (fill.type == FillStyle::Type::LinearGradient) {
        pat = Cairo::LinearGradient::create(x1, y1, x2, y2);
    } else {
        // Radial: focal at (g_x1,g_y1), centre at (g_x2,g_y2), outer radius
        // is a fraction of the bbox's larger dimension. Same approximation
        // Canvas uses (objectBoundingBox spec calls for sqrt((w²+h²)/2) but
        // the larger-dim approximation is close enough at the visual level
        // and matches user intuition).
        const double R = fill.g_r * std::max(bbox_w, bbox_h);
        pat = Cairo::RadialGradient::create(x1, y1, 0.0, x2, y2, R);
    }

    // Sort defensively in case stops weren't pre-sorted.
    std::vector<GradientStop> sorted = fill.stops;
    std::sort(sorted.begin(), sorted.end(),
              [](const GradientStop& a, const GradientStop& b) {
                  return a.offset < b.offset;
              });
    for (const auto& s : sorted) {
        pat->add_color_stop_rgba(s.offset, s.r, s.g, s.b, s.a);
    }

    return pat;
}

// ── cairo_set_source_pixbuf ─────────────────────────────────────────
// See header for design notes. Format conversion in a single pass,
// row-by-row to respect rowstride padding. Endianness assumption:
// little-endian host (BGRA in memory == 0xAARRGGBB as uint32_t). Curvz
// targets aarch64 + x86_64; both are little-endian. If a big-endian
// build is ever needed, this is the one place that changes.
void cairo_set_source_pixbuf(
    const Cairo::RefPtr<Cairo::Context>& cr,
    const Glib::RefPtr<Gdk::Pixbuf>& pb,
    double x, double y)
{
    if (!cr || !pb) return;
    int w = pb->get_width();
    int h = pb->get_height();
    if (w <= 0 || h <= 0) return;

    bool has_alpha = pb->get_has_alpha();
    int n_channels = pb->get_n_channels();   // 3 (RGB) or 4 (RGBA)
    int src_stride = pb->get_rowstride();
    const guint8* src = pb->get_pixels();
    if (!src) return;

    // ARGB32 surface — Cairo allocates with appropriate stride for the
    // platform (multiple of 4 bytes typically). Surface owns its bytes;
    // no manual lifetime juggling beyond the RefPtr.
    auto surf = Cairo::ImageSurface::create(
        Cairo::ImageSurface::Format::ARGB32, w, h);
    surf->flush();
    int dst_stride = surf->get_stride();
    guint8* dst = surf->get_data();

    // Per-pixel: swap RB, premultiply by alpha. Cairo ARGB32 expects
    // colour channels already multiplied by alpha — straight rendering
    // a pixbuf in without premultiplying produces visible halos at
    // partially-transparent edges (the classic GdkPixbuf-on-Cairo bug).
    for (int row = 0; row < h; ++row) {
        const guint8* sp = src + row * src_stride;
        guint8* dp = dst + row * dst_stride;
        for (int col = 0; col < w; ++col) {
            guint8 r = sp[0];
            guint8 g = sp[1];
            guint8 b = sp[2];
            guint8 a = has_alpha ? sp[3] : 0xff;
            if (has_alpha && a != 0xff) {
                // Round-half-up integer premultiply: (c*a + 127) / 255.
                // Equivalent within 1 LSB to the float formula c*(a/255).
                r = static_cast<guint8>((r * a + 127) / 255);
                g = static_cast<guint8>((g * a + 127) / 255);
                b = static_cast<guint8>((b * a + 127) / 255);
            }
            // Little-endian BGRA byte order = ARGB32 in memory.
            dp[0] = b;
            dp[1] = g;
            dp[2] = r;
            dp[3] = a;
            sp += n_channels;
            dp += 4;
        }
    }
    surf->mark_dirty();

    cr->set_source(surf, x, y);
}

// ── render_drop_shadow_under ────────────────────────────────────────
// Mirrors Canvas::render_shadow_under but takes its inputs as values
// rather than reading from a SceneNode + Canvas member. Skips the
// canvas-viewport intersect (see header note).
void render_drop_shadow_under(
    const Cairo::RefPtr<Cairo::Context>& cr,
    const Cairo::RefPtr<Cairo::Pattern>& host_pat,
    double host_bbox_x, double host_bbox_y,
    double host_bbox_w, double host_bbox_h,
    double blur_doc, double dx_doc, double dy_doc,
    double color_r, double color_g, double color_b, double color_a,
    double opacity)
{
    if (!host_pat) return;
    blur_doc = std::max(0.0, blur_doc);

    // Pad in doc units. blur radius reach + offset push + safety.
    const double pad_doc =
        std::ceil(blur_doc * 2.0)
        + std::abs(dx_doc) + std::abs(dy_doc)
        + 4.0;

    const double doc_x0 = host_bbox_x                  - pad_doc;
    const double doc_y0 = host_bbox_y                  - pad_doc;
    const double doc_x1 = host_bbox_x + host_bbox_w    + pad_doc;
    const double doc_y1 = host_bbox_y + host_bbox_h    + pad_doc;

    // Doc rect → device pixel rect via cr's CTM.
    double dx0 = doc_x0, dy0 = doc_y0;
    double dx1 = doc_x1, dy1 = doc_y1;
    cr->user_to_device(dx0, dy0);
    cr->user_to_device(dx1, dy1);
    if (dx1 < dx0) std::swap(dx0, dx1);
    if (dy1 < dy0) std::swap(dy0, dy1);

    int pix_x = (int)std::floor(dx0);
    int pix_y = (int)std::floor(dy0);
    int pix_w = (int)std::ceil(dx1) - pix_x;
    int pix_h = (int)std::ceil(dy1) - pix_y;
    if (pix_w <= 0 || pix_h <= 0) return;

    // CTM scale (doc → device) for blur-radius conversion. Reads the
    // same scale Canvas computes via m_zoom, but generalised: works in
    // any Cairo context (canvas, print page, SVG). Take the X-axis
    // norm of the CTM — uniform scale is the common case; for non-
    // uniform scale this still produces a reasonable radius.
    Cairo::Matrix ctm = cr->get_matrix();
    const double ctm_scale =
        std::sqrt(ctm.xx * ctm.xx + ctm.yx * ctm.yx);

    // Offscreen ARGB32 (premultiplied) — only format box_blur supports.
    auto surf = Cairo::ImageSurface::create(
        Cairo::Surface::Format::ARGB32, pix_w, pix_h);
    if (!surf) return;
    auto sc = Cairo::Context::create(surf);

    // Matrix-correct paint of host_pat into surf: take cr's current CTM
    // and offset its translation column by (-pix_x, -pix_y) so surface
    // pixel (0,0) corresponds to device pixel (pix_x, pix_y).
    Cairo::Matrix m = ctm;
    m.x0 -= pix_x;
    m.y0 -= pix_y;
    sc->set_matrix(m);
    sc->set_source(host_pat);
    sc->paint();
    sc.reset();
    surf->flush();

    // Blur in place. radius=0 = no-op (box_blur handles that).
    const int radius_px = (int)std::round(blur_doc * ctm_scale);
    if (radius_px > 0) {
        box_blur_argb32(surf->get_data(), surf->get_stride(),
                        pix_w, pix_h, radius_px);
        surf->mark_dirty();
    }

    // Shadow offset in device pixels: take (0,0) and (dx,dy) doc-space
    // through the CTM; subtract.
    double off_a_x = 0.0, off_a_y = 0.0;
    double off_b_x = dx_doc, off_b_y = dy_doc;
    cr->user_to_device(off_a_x, off_a_y);
    cr->user_to_device(off_b_x, off_b_y);
    const double off_dx = off_b_x - off_a_x;
    const double off_dy = off_b_y - off_a_y;

    // Final shadow alpha = colour.a × opacity.
    const double final_a =
        std::max(0.0, std::min(1.0, color_a * opacity));

    cr->save();
    cr->set_identity_matrix();
    cr->set_source_rgba(color_r, color_g, color_b, final_a);
    cr->mask(surf, pix_x + off_dx, pix_y + off_dy);
    cr->restore();
}

// ──────────────────────────────────────────────────────────────────────
// Dialog factory implementation (s125 m2a)
// ──────────────────────────────────────────────────────────────────────
//
// What this file holds: the runtime side of the spec-driven dialog
// system declared in curvz_utils.hpp. The header is the contract; this
// is the only place that knows what GTK widgets implement it.
//
// The implementation has two layers, deliberately separated:
//
//   FIELD LAYER (build_field_widget, extract_value)
//     One pair of operations per field type. build creates the widget
//     for a given FormFieldSpec; extract reads the widget's state back
//     into a FormFieldValue. Each field type touches exactly one arm
//     in each operation. The std::visit on FormFieldSpec / FieldWidget
//     enforces this — if you add a field type and forget to handle it
//     in either operation, the static_assert fires at build time.
//     This is what makes new field types safe to add.
//
//   DIALOG LAYER (build_dialog)
//     The shell. Builds the title row, detail row, optional field
//     grid (delegating to the field layer), button row, and wires
//     dismissal. Knows nothing about specific field types — it just
//     iterates the spec and calls the field layer.
//
// Layered this way, the two extension axes stay independent:
//
//   • Adding a field type   = field layer change only
//   • Changing dialog shape = dialog layer change only
//   • Restyling             = css.hpp change only, never code
//
// Most maintenance edits should land in exactly one of these places.
// If you find yourself touching all three, step back — that usually
// means the layers have leaked into each other.
//
// Layout the dialog layer produces:
//
//   ┌─────────────────────────────────────────┐
//   │ Title                  .curvz-alert-title
//   │ Detail (selectable)    .curvz-alert-detail
//   │ ┌─ field grid (show_form only) ────────┐
//   │ │ Label : Widget                        │   ← built by field layer
//   │ │ Label : Widget                        │
//   │ └───────────────────────────────────────┘
//   │                          [Cancel] [OK] .curvz-alert-buttons
//   └─────────────────────────────────────────┘
//
// Visual treatment of every class above lives in css.hpp under the
// `Curvz alert / confirm / form dialog` section. Don't add equivalent
// inline styling here.
//
// build_dialog parameter cheat-sheet (verbose docs in curvz_utils.hpp):
//   parent          - transient_for + motif inheritance source
//   title           - "" allowed (empty title row collapses out)
//   detail          - "" allowed (no detail row)
//   fields          - empty allowed (no field grid)
//   buttons         - at least one required
//   default_button  - -1 = none
//   cancel_button   - -1 = no cancel-equivalent (Esc/X return -1)
//   on_finished     - fired exactly once on dismissal
namespace {

// ── Field layer ─────────────────────────────────────────────────────
//
// FieldWidget is the runtime mirror of FormFieldSpec. Each arm of the
// spec variant has a corresponding arm here. The spec is what the
// caller hands us; the widget is what we built from it. Holding them
// in parallel variants means build/extract are simple visit walks
// across matching shapes — no maps, no enums, no manual dispatch.
//
// ComboField is the only one that needs more than a raw widget pointer:
// the dropdown holds an integer index, but callers usually want the
// item TEXT, so we cache the items list alongside the dropdown for
// the index→text lookup at extract time.
struct ComboHandle {
    Gtk::DropDown* dropdown = nullptr;
    std::vector<std::string> items;
};

using FieldWidget = std::variant<Gtk::Entry*,        // TextField
                                 Gtk::SpinButton*,   // NumberField
                                 Gtk::CheckButton*,  // CheckboxField
                                 ComboHandle>;       // ComboField

// build_field_widget: spec → widget. One arm per FormFieldSpec arm.
// Attaches the widget into `grid` at column 1, row `row` (column 0
// holds the field's label, painted by the dialog layer).
//
// The if-constexpr-else chain is the standard "exhaustive visit" idiom
// in C++17. Each arm handles one type; the trailing else hits
// static_assert with a clear message if a new spec arm slips in
// without a matching builder.
FieldWidget build_field_widget(const curvz::utils::FormFieldSpec& spec,
                               Gtk::Grid* grid, int row) {
    return std::visit(
        [grid, row](const auto& s) -> FieldWidget {
            using T = std::decay_t<decltype(s)>;

            if constexpr (std::is_same_v<T, curvz::utils::TextField>) {
                auto* entry = Gtk::make_managed<Gtk::Entry>();
                curvz::utils::set_name(entry, "dlg_alert_field_text",
                                       "curvz_alert_form_text_field");
                entry->set_text(s.default_text);
                if (!s.placeholder.empty())
                    entry->set_placeholder_text(s.placeholder);
                entry->set_hexpand(true);
                entry->set_width_chars(40);
                grid->attach(*entry, 1, row, 1, 1);
                return entry;
            }
            else if constexpr (std::is_same_v<T, curvz::utils::NumberField>) {
                auto adj = Gtk::Adjustment::create(
                    s.default_value, s.min, s.max,
                    s.step, s.step * 10.0, 0.0);
                auto* spin = Gtk::make_managed<Gtk::SpinButton>(adj);
                curvz::utils::set_name(spin, "dlg_alert_field_num",
                                       "curvz_alert_form_number_field");
                spin->set_digits(s.decimals);
                spin->set_hexpand(true);
                grid->attach(*spin, 1, row, 1, 1);
                return spin;
            }
            else if constexpr (std::is_same_v<T, curvz::utils::CheckboxField>) {
                auto* chk = Gtk::make_managed<Gtk::CheckButton>();
                curvz::utils::set_name(chk, "dlg_alert_field_chk",
                                       "curvz_alert_form_checkbox_field");
                chk->set_active(s.default_value);
                chk->set_halign(Gtk::Align::START);
                grid->attach(*chk, 1, row, 1, 1);
                return chk;
            }
            else if constexpr (std::is_same_v<T, curvz::utils::ComboField>) {
                std::vector<Glib::ustring> items_u;
                items_u.reserve(s.items.size());
                for (const auto& str : s.items)
                    items_u.emplace_back(str);
                auto string_list = Gtk::StringList::create(items_u);
                auto* dd = Gtk::make_managed<Gtk::DropDown>(string_list);
                curvz::utils::set_name(dd, "dlg_alert_field_combo",
                                       "curvz_alert_form_combo_field");
                if (s.default_index >= 0 &&
                    s.default_index < (int)s.items.size())
                    dd->set_selected(s.default_index);
                dd->set_hexpand(true);
                grid->attach(*dd, 1, row, 1, 1);
                return ComboHandle{dd, s.items};
            }
            else {
                // Compile-time exhaustiveness guard. Adding a new arm to
                // FormFieldSpec without updating this function fails the
                // build here with a clear message.
                static_assert(!sizeof(T*),
                    "build_field_widget: unhandled FormFieldSpec arm. "
                    "Add an `if constexpr` branch for the new type.");
                return FieldWidget{static_cast<Gtk::Entry*>(nullptr)};  // unreachable
            }
        },
        spec);
}

// extract_value: widget → result. The inverse pump to build_field_widget.
// Reads the current state of one field's widget back into a typed
// FormFieldValue. Same exhaustive-visit shape — every FieldWidget arm
// must have a corresponding handler, enforced at compile time.
//
// Note that this reads CURRENT state, not initial state. By the time
// the user has picked a button, they may have edited the field; this
// is what produces the "what the user typed" in the result map.
curvz::utils::FormFieldValue extract_value(const FieldWidget& fw) {
    return std::visit(
        [](const auto& w) -> curvz::utils::FormFieldValue {
            using T = std::decay_t<decltype(w)>;
            curvz::utils::FormFieldValue v;
            if constexpr (std::is_same_v<T, Gtk::Entry*>) {
                v.value = w ? std::string(w->get_text()) : std::string{};
            }
            else if constexpr (std::is_same_v<T, Gtk::SpinButton*>) {
                v.value = w ? w->get_value() : 0.0;
            }
            else if constexpr (std::is_same_v<T, Gtk::CheckButton*>) {
                v.value = w ? w->get_active() : false;
            }
            else if constexpr (std::is_same_v<T, ComboHandle>) {
                int idx = w.dropdown
                    ? (int)w.dropdown->get_selected()
                    : 0;
                v.value = idx;
                if (idx >= 0 && idx < (int)w.items.size())
                    v.combo_item = w.items[idx];
            }
            else {
                static_assert(!sizeof(T*),
                    "extract_value: unhandled FieldWidget arm. "
                    "Add an `if constexpr` branch for the new type.");
            }
            return v;
        },
        fw);
}

// ── Dialog layer ────────────────────────────────────────────────────
//
// The shell that delegates to the field layer. Knows about title,
// detail, button row, dismissal — never about specific field types.

// ── build_dialog: lifecycle rests on a single ownership convention ───
//
// The dialog owns itself. There is no caller-held handle, no scoped
// guard, no manual close. The lifecycle is:
//
//   present()                 → window appears
//   user picks button OR Esc OR X → dlg->close() → signal_hide fires
//   signal_hide               → fire callback once → delete dlg
//
// Three things follow from this convention, all worth knowing if you
// edit this function:
//
// 1. `dlg` is heap-allocated and never owned by a smart pointer. Its
//    lifetime is bounded entirely by signal_hide. Any lambda capturing
//    `dlg` may call dlg->close(), but must NOT delete and must NOT
//    touch `dlg` after close() returns. The hide handler is the only
//    site that deletes.
//
// 2. The callback fires EXACTLY ONCE, on hide. We use a `fired` latch
//    not because hide can fire twice (it can't), but because hide
//    needs to distinguish "user picked a button" (latch true → use
//    captured index) from "user dismissed via X / Esc" (latch false
//    → fall back to cancel_button index).
//
// 3. All state the callback needs must be captured by VALUE in lambdas
//    that outlive build_dialog's stack frame. The function parameters
//    `fields` and friends will be gone by the time hide fires. We
//    extract field ids into a value-captured vector for exactly this
//    reason. Any new state the callback needs to see must be similarly
//    arranged or it will dangle.
//
// The reward for following this convention is that callsites never
// have to think about dialog lifetime. Call show_alert / show_confirm
// / show_form, optionally pass a callback, and forget. The dialog
// cleans itself up.
void build_dialog(
    Gtk::Window& parent,
    const std::string& title,
    const std::string& detail,
    const std::vector<curvz::utils::FormField>& fields,
    const std::vector<std::string>& buttons,
    int default_button,
    int cancel_button,
    std::function<void(int, const std::map<std::string, curvz::utils::FormFieldValue>&)>
        on_finished)
{
    auto* dlg = new Gtk::Window();
    curvz::utils::set_name(dlg, "dlg_curvz_alert", "curvz_alert_dialog_root");
    dlg->set_title(title.empty() ? "Curvz" : title);
    dlg->set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*dlg, parent);
    dlg->set_modal(true);
    dlg->set_resizable(false);
    dlg->add_css_class("curvz-alert");

    auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    outer->set_margin(16);

    // Title row.
    if (!title.empty()) {
        auto* lbl_title = Gtk::make_managed<Gtk::Label>(title);
        lbl_title->set_halign(Gtk::Align::START);
        lbl_title->set_xalign(0.0f);
        lbl_title->set_wrap(true);
        lbl_title->set_max_width_chars(60);
        lbl_title->add_css_class("curvz-alert-title");
        curvz::utils::set_name(lbl_title, "dlg_alert_title",
                               "curvz_alert_title_label");
        outer->append(*lbl_title);
    }

    // Detail row — selectable Label. Was a read-only Gtk::Entry (lesson
    // from m1j), but a focusable Entry with set_editable(false) bells
    // on every edit-attempt key (printable chars, Backspace, Delete,
    // Ctrl+V) — that was the source of the s125 m2b beep storm. A
    // Label with set_selectable(true) gives us copy/select-all behaviour
    // without claiming editable-widget semantics.
    //
    // set_can_focus(false) keeps the label out of the focus chain so
    // the default button gets the natural initial focus (visual cue
    // for "the safe action"). Mouse-drag selection still works on a
    // non-focusable selectable label; only keyboard selection requires
    // focus, which our users don't need on a static detail blurb.
    if (!detail.empty()) {
        auto* lbl_detail = Gtk::make_managed<Gtk::Label>(detail);
        lbl_detail->set_selectable(true);
        lbl_detail->set_can_focus(false);
        lbl_detail->set_focusable(false);
        lbl_detail->set_halign(Gtk::Align::START);
        lbl_detail->set_xalign(0.0f);
        lbl_detail->set_wrap(true);
        lbl_detail->set_max_width_chars(60);
        lbl_detail->add_css_class("curvz-alert-detail");
        curvz::utils::set_name(lbl_detail, "dlg_alert_detail",
                               "curvz_alert_detail_label");
        outer->append(*lbl_detail);
    }

    // Field grid — only when show_form is in play. Builds widgets per
    // field type and stashes them in the same order as the input vector
    // so the result map can be assembled on button click.
    auto field_widgets = std::make_shared<std::vector<FieldWidget>>();
    field_widgets->reserve(fields.size());

    if (!fields.empty()) {
        auto* grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_row_spacing(8);
        grid->set_column_spacing(12);
        grid->add_css_class("curvz-alert-fields");

        int row = 0;
        for (const auto& f : fields) {
            // Label column.
            auto* lbl = Gtk::make_managed<Gtk::Label>(f.label);
            lbl->set_halign(Gtk::Align::END);
            lbl->set_valign(Gtk::Align::CENTER);
            lbl->add_css_class("dim-label");
            grid->attach(*lbl, 0, row, 1, 1);

            // Widget column. One arm per FormFieldSpec variant lives in
            // build_field_widget; adding a new field type means adding
            // an arm there and in extract_value (below). Compiler enforces
            // exhaustiveness via the static_assert in each visit.
            field_widgets->push_back(build_field_widget(f.spec, grid, row));
            ++row;
        }

        outer->append(*grid);
    }

    // Button row — right-aligned, visually separated from the body.
    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    btn_row->set_halign(Gtk::Align::END);
    btn_row->set_margin_top(12);
    btn_row->add_css_class("curvz-alert-buttons");

    // Capture only the field ids (by value) for the result-map keys.
    // The full `fields` vector is a parameter — referencing it from the
    // signal_hide lambda would dangle, since signal_hide fires after
    // build_dialog has returned and `fields` is gone.
    std::vector<std::string> field_ids;
    field_ids.reserve(fields.size());
    for (const auto& f : fields)
        field_ids.push_back(f.id);

    // Helper to gather field values once a button is clicked. Each
    // widget handle knows its own type via the FieldWidget variant —
    // extract_value does a std::visit and produces the right
    // FormFieldValue. Adding a new field type only requires adding an
    // arm to extract_value, not editing this site.
    auto collect_values = [field_widgets, field_ids]() {
        std::map<std::string, curvz::utils::FormFieldValue> result;
        for (size_t i = 0; i < field_widgets->size(); ++i) {
            result[field_ids[i]] = extract_value((*field_widgets)[i]);
        }
        return result;
    };

    // Track which button (if any) the user picked. Esc/X dispatch the
    // cancel button; if the user picks one, they all skip the cancel
    // dispatch on hide. Wrapping in a shared_ptr means the lambdas can
    // capture it by value and still mutate.
    auto chosen = std::make_shared<int>(-1);
    auto fired = std::make_shared<bool>(false);

    Gtk::Button* default_btn_ptr = nullptr;
    for (size_t i = 0; i < buttons.size(); ++i) {
        auto* btn = Gtk::make_managed<Gtk::Button>(buttons[i]);
        // Per-button names use a literal abbrev so widget_names_sync's
        // regex (which only harvests string-literal arguments) catches
        // it. Inspector disambiguates buttons within one dialog by
        // position in the tree + the CSS classes added below.
        curvz::utils::set_name(btn, "dlg_alert_btn",
                               "curvz_alert_dialog_button");
        const int idx = (int)i;
        if ((int)i == default_button) {
            btn->add_css_class("suggested-action");
            btn->add_css_class("curvz-alert-default");
            default_btn_ptr = btn;
        }
        if ((int)i == cancel_button) {
            btn->add_css_class("destructive-action");
            btn->add_css_class("curvz-alert-cancel");
        }
        btn->signal_clicked().connect([dlg, chosen, fired, idx]() {
            LOG_INFO("build_dialog: button-click idx={}", idx);
            *chosen = idx;
            *fired = true;
            dlg->close();
        });
        btn_row->append(*btn);
    }
    outer->append(*btn_row);
    dlg->set_child(*outer);

    // Esc → cancel_button (if defined), otherwise -1. GTK4 doesn't bind
    // Esc on transient windows by default — wire it explicitly. Bubble
    // phase: focused widgets get first crack at every key; we only
    // see what they declined. Esc isn't claimed by Entry/SpinButton/
    // CheckButton/DropDown, so it bubbles up here.
    auto key_ctrl = Gtk::EventControllerKey::create();
    key_ctrl->set_propagation_phase(Gtk::PropagationPhase::BUBBLE);
    key_ctrl->signal_key_pressed().connect(
        [dlg, chosen, fired, cancel_button](
            guint keyval, guint, Gdk::ModifierType) -> bool {
            if (keyval == GDK_KEY_Escape) {
                *chosen = cancel_button;  // -1 if no cancel button
                *fired = true;
                dlg->close();
                return true;  // claimed
            }
            return false;  // let other handlers / window machinery see it
        }, false);
    dlg->add_controller(key_ctrl);

    // Default button activates on Enter (suggested-action convention).
    if (default_btn_ptr) {
        default_btn_ptr->set_receives_default(true);
        dlg->set_default_widget(*default_btn_ptr);
    }

    // On close-request: invoke callback with the result, then delete.
    //
    // We listen on close_request rather than signal_hide because GTK4's
    // Window::close() can destroy the window directly without emitting
    // signal_hide — observed when this dialog is stacked on top of an
    // already-modal parent (e.g. an Export dialog presenting a Replace
    // confirm). signal_hide never fired in that case and the user's
    // callback was orphaned. close_request is the canonical "the window
    // is going away" hook in GTK4 and fires for button-driven close(),
    // X-button click, and Esc-driven close uniformly.
    //
    // Latch with `fired` so we don't double-invoke if both close_request
    // and signal_hide somehow fire for the same close.
    auto cb_fired = std::make_shared<bool>(false);
    dlg->signal_close_request().connect(
        [dlg, chosen, fired, cancel_button, on_finished,
         collect_values, cb_fired]() {
            if (*cb_fired) return false;  // already handled — let close proceed
            *cb_fired = true;
            LOG_INFO("build_dialog: close_request fired={} chosen={}",
                     *fired, *chosen);
            if (!*fired) {
                // X-button / Esc path — treat as cancel.
                *chosen = cancel_button;
            }
            // Collect form field values BEFORE delete, since collect_values
            // dereferences the captured widget pointers.
            auto values = collect_values();
            LOG_INFO("build_dialog: invoking on_finished with chosen={}", *chosen);
            on_finished(*chosen, values);
            // Queue delete on idle so we return from this signal handler
            // first — deleting the window from inside its own close_request
            // is a use-after-free hazard.
            Glib::signal_idle().connect_once([dlg]() { delete dlg; });
            return false;  // allow close to proceed
        },
        /*after=*/false);

    // Initial focus on the default button. Set BEFORE present() so GTK
    // resolves it during the first focus-traversal pass, after layout
    // settles — no idle defer, no race with selectable-label focus
    // claims. Pairing this with set_focusable(false) on the detail
    // Label (above) keeps the focus chain clean: button gets focus
    // first, Tab cycles through form fields, Esc closes.
    if (default_btn_ptr) {
        dlg->set_focus(*default_btn_ptr);
    }

    dlg->present();
}

} // anonymous namespace

void show_alert(Gtk::Window& parent,
                const std::string& title,
                const std::string& detail) {
    build_dialog(parent, title, detail, /*fields=*/{}, /*buttons=*/{"OK"},
                 /*default=*/0, /*cancel=*/0,
                 [](int, const std::map<std::string, FormFieldValue>&) {
                     // Fire-and-forget — caller doesn't care about result.
                 });
}

void show_confirm(Gtk::Window& parent,
                  const std::string& title,
                  const std::string& detail,
                  const std::vector<std::string>& buttons,
                  int default_button,
                  int cancel_button,
                  std::function<void(int)> callback) {
    build_dialog(parent, title, detail, /*fields=*/{}, buttons,
                 default_button, cancel_button,
                 [callback](int idx,
                            const std::map<std::string, FormFieldValue>&) {
                     callback(idx);
                 });
}

void show_form(Gtk::Window& parent,
               const std::string& title,
               const std::string& detail,
               const std::vector<FormField>& fields,
               const std::vector<std::string>& buttons,
               int default_button,
               int cancel_button,
               std::function<void(int,
                                  const std::map<std::string, FormFieldValue>&)>
                   callback) {
    build_dialog(parent, title, detail, fields, buttons,
                 default_button, cancel_button, std::move(callback));
}

// ── make_path_override_row ────────────────────────────────────────────
// s145 m4 — single helper for path-override pref rows. The four
// path-override prefs (library, templates, log, custom CSS) all share
// this exact shape: label + Entry (current value, placeholder = default)
// + Browse + Reset.
//
// Async lifetime note: the FileDialog is created locally and captured
// by value into the callback lambda. The capture extends its lifetime
// past the create_file/select_folder return. Same idiom as the
// existing on_open / on_import_svg sites in MainWindow.cpp; works
// because Glib::RefPtr is shared-ownership and the dialog's GTK
// internals keep it alive while the dialog is showing.
//
// (FileDialog is NOT subject to the ColorDialog member-RefPtr rule
// from s139 m1 — that rule is specific to ColorDialog's lifetime
// quirk. Lambda-capture is the canonical FileDialog idiom.)
Gtk::Widget* make_path_override_row(
    const char* label_text,
    const std::string& current_value,
    const std::string& default_path,
    const char* tooltip,
    bool pick_folder,
    Gtk::Window* dialog_parent,
    std::function<void(const std::string&)> on_commit) {

    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->add_css_class("prop-row");
    row->set_spacing(6);
    row->set_margin_start(6);
    row->set_margin_end(6);
    row->set_margin_top(2);
    row->set_margin_bottom(2);

    auto* key = Gtk::make_managed<Gtk::Label>(label_text);
    key->add_css_class("prop-lbl");
    key->set_xalign(0.0f);
    row->append(*key);

    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(current_value);
    entry->set_placeholder_text(default_path);
    entry->set_hexpand(true);
    if (tooltip && *tooltip) entry->set_tooltip_text(tooltip);
    row->append(*entry);

    auto* browse = Gtk::make_managed<Gtk::Button>("Browse…");
    browse->set_valign(Gtk::Align::CENTER);
    browse->set_tooltip_text(pick_folder
        ? "Browse for a folder"
        : "Browse for a file");
    row->append(*browse);

    auto* reset = Gtk::make_managed<Gtk::Button>("Reset");
    reset->set_valign(Gtk::Align::CENTER);
    reset->set_tooltip_text("Clear the override and use the default path");
    row->append(*reset);

    // Commit on Enter / focus loss. Both routes funnel through here so
    // the trim-and-store contract in AppPreferences setters does the
    // actual normalisation; this layer is a dumb pass-through.
    auto commit = [entry, on_commit]() {
        on_commit(entry->get_text());
    };
    entry->signal_activate().connect(commit);
    auto focus = Gtk::EventControllerFocus::create();
    focus->signal_leave().connect(commit);
    entry->add_controller(focus);

    // Browse — async FileDialog, write result back into Entry then
    // commit. The pick_folder flag picks select_folder vs open. If
    // the user cancels, no commit happens (Entry stays as-is).
    browse->signal_clicked().connect(
        [dialog_parent, pick_folder, entry, on_commit, default_path]() {
            auto dialog = Gtk::FileDialog::create();
            dialog->set_title(pick_folder ? "Choose folder" : "Choose file");

            // Initial folder: prefer the current Entry value's parent if
            // it exists; else the default path's parent; else nothing.
            std::string seed;
            const std::string entry_text = entry->get_text();
            if (!entry_text.empty())      seed = entry_text;
            else if (!default_path.empty()) seed = default_path;
            if (!seed.empty()) {
                try {
                    namespace fs = std::filesystem;
                    fs::path p(seed);
                    fs::path init = pick_folder ? p : p.parent_path();
                    if (!init.empty() && fs::exists(init)) {
                        dialog->set_initial_folder(
                            Gio::File::create_for_path(init.string()));
                    }
                } catch (...) {}
            }

            auto handler = [dialog, entry, on_commit, pick_folder]
                (Glib::RefPtr<Gio::AsyncResult>& result) {
                try {
                    auto file = pick_folder
                        ? dialog->select_folder_finish(result)
                        : dialog->open_finish(result);
                    if (!file) return;
                    const std::string path = file->get_path();
                    // Defensive: PropertiesPanel rebuilds on every
                    // selection change. If the row was destroyed
                    // between Browse-click and dialog-confirm, the
                    // managed Entry is gone. Detect by checking the
                    // widget is still parented into a tree. If it
                    // isn't, we still call on_commit — the
                    // AppPreferences setter is a singleton, always
                    // valid — but skip the Entry write.
                    if (entry->get_parent() != nullptr) {
                        entry->set_text(path);
                    }
                    on_commit(path);
                } catch (...) {
                    // User cancelled or platform error — nothing to do.
                }
            };

            if (pick_folder) {
                if (dialog_parent)
                    dialog->select_folder(*dialog_parent, handler);
                // else: no-op — dialog_parent is required for FileDialog
                // in this codebase (no precedent for the no-parent overload).
                // PropertiesPanel always has a parent window available.
            } else {
                if (dialog_parent)
                    dialog->open(*dialog_parent, handler);
            }
        });

    // Reset — clear and commit empty.
    reset->signal_clicked().connect([entry, on_commit]() {
        entry->set_text("");
        on_commit("");
    });

    return row;
}

// ── Warp envelope presets ───────────────────────────────────────────────
//
// s146 m2: lifted from WarpDialog.cpp's anonymous namespace. See the
// header for design notes.
//
// Anonymous-helper namespace below scopes the build_* helpers to this
// translation unit. Only generate_warp_preset and the warp_presets
// metadata API are exposed.
namespace {

using ::Curvz::BezierNode;
using ::Curvz::PathData;

// Helper to build a single anchor with colinear handles at distance dx
// left and right. Produces a straight-segment cubic.
BezierNode mk_straight(double x, double y, double dx_in, double dx_out) {
    BezierNode n;
    n.x = x; n.y = y;
    n.cx1 = x - dx_in;  n.cy1 = y;
    n.cx2 = x + dx_out; n.cy2 = y;
    n.type = BezierNode::Type::Smooth;
    return n;
}

// Build a flat (straight horizontal) envelope at y=fixed_y across
// [bx, bx+bw] with `count` evenly-spaced anchors.
PathData build_flat(double bx, double bw, double fixed_y, int count) {
    PathData pd;
    pd.closed = false;
    if (count < 2) count = 2;
    if (count > 4) count = 4;
    double seg_span = bw / (count - 1);
    double h = seg_span / 3.0;
    for (int i = 0; i < count; ++i) {
        double x = bx + (bw * i) / (count - 1);
        double dx_in  = (i == 0)         ? 0.0 : h;
        double dx_out = (i == count - 1) ? 0.0 : h;
        pd.nodes.push_back(mk_straight(x, fixed_y, dx_in, dx_out));
    }
    return pd;
}

// Build an arced envelope: `count` anchors along the base y, with the
// interior (non-endpoint) anchors displaced vertically by `amplitude`
// (+ = down in Y-down, - = up). count==2 returns flat (no arc possible).
PathData build_arc(double bx, double bw, double base_y,
                   double amplitude, int count) {
    if (count <= 2)
        return build_flat(bx, bw, base_y, count);
    PathData pd;
    pd.closed = false;
    if (count > 4) count = 4;
    double seg_span = bw / (count - 1);
    double h = seg_span / 3.0;
    for (int i = 0; i < count; ++i) {
        double x = bx + (bw * i) / (count - 1);
        bool interior = (i != 0 && i != count - 1);
        double y = interior ? (base_y + amplitude) : base_y;
        BezierNode n;
        n.x = x; n.y = y;
        n.cx1 = (i == 0)         ? x : (x - h);
        n.cy1 = y;
        n.cx2 = (i == count - 1) ? x : (x + h);
        n.cy2 = y;
        n.type = BezierNode::Type::Smooth;
        pd.nodes.push_back(n);
    }
    return pd;
}

// Build a perspective-trapezoidal envelope side: straight-line top or
// bottom, but with x-span reduced by inset_factor on each end.
PathData build_trapezoid_side(double bx, double bw, double fixed_y,
                              double inset_factor, int count) {
    double inset = bw * inset_factor * 0.5;
    return build_flat(bx + inset, bw - 2.0 * inset, fixed_y, count);
}

// Build a wavy envelope using alternating vertical displacement.
// Endpoints stay on base; interior anchors alternate +amp/-amp.
// count<=2 falls back to flat (single segment can't wave).
PathData build_wave(double bx, double bw, double base_y,
                    double amplitude, int count) {
    if (count <= 2)
        return build_flat(bx, bw, base_y, count);
    PathData pd;
    pd.closed = false;
    if (count > 4) count = 4;
    double seg_span = bw / (count - 1);
    double h = seg_span / 3.0;
    for (int i = 0; i < count; ++i) {
        double x = bx + (bw * i) / (count - 1);
        bool interior = (i != 0 && i != count - 1);
        double y = base_y;
        if (interior) {
            int interior_idx = i - 1;
            y = base_y + ((interior_idx % 2 == 0) ? amplitude : -amplitude);
        }
        BezierNode n;
        n.x = x; n.y = y;
        n.cx1 = (i == 0)         ? x : (x - h);
        n.cy1 = y;
        n.cx2 = (i == count - 1) ? x : (x + h);
        n.cy2 = y;
        n.type = BezierNode::Type::Smooth;
        pd.nodes.push_back(n);
    }
    return pd;
}

} // anonymous namespace

namespace warp_presets {

const char* const* preset_names() {
    static const char* names[PRESET_COUNT] = {
        "Flat", "Arc Up", "Arc Down", "Bulge",
        "Squeeze", "Perspective Near", "Perspective Far", "Wave"
    };
    return names;
}

bool requires_three_anchors(int preset_idx) {
    return preset_idx == WAVE;
}

} // namespace warp_presets

void generate_warp_preset(int preset_idx,
                          double bx, double by, double bw, double bh,
                          int top_count, int bot_count,
                          ::Curvz::PathData& top_env,
                          ::Curvz::PathData& bot_env) {
    double y_top    = by;             // smaller y (up in Y-down)
    double y_bottom = by + bh;        // larger y (down in Y-down)
    double amp      = bh * 0.25;      // 25% bbox height as arc amplitude
    double insetF   = 0.30;           // 30% side-inset for perspective

    using namespace warp_presets;
    switch (preset_idx) {
    case FLAT:
    default:
        top_env = build_flat(bx, bw, y_top,    top_count);
        bot_env = build_flat(bx, bw, y_bottom, bot_count);
        break;
    case ARC_UP:
        // Top curves up (smaller y), bottom straight
        top_env = build_arc(bx, bw, y_top, -amp, top_count);
        bot_env = build_flat(bx, bw, y_bottom, bot_count);
        break;
    case ARC_DOWN:
        // Top straight, bottom curves down (larger y)
        top_env = build_flat(bx, bw, y_top, top_count);
        bot_env = build_arc(bx, bw, y_bottom, +amp, bot_count);
        break;
    case BULGE:
        // Top curves up, bottom curves down → balloon
        top_env = build_arc(bx, bw, y_top,    -amp, top_count);
        bot_env = build_arc(bx, bw, y_bottom, +amp, bot_count);
        break;
    case SQUEEZE:
        // Top curves down, bottom curves up → pinch
        top_env = build_arc(bx, bw, y_top,    +amp, top_count);
        bot_env = build_arc(bx, bw, y_bottom, -amp, bot_count);
        break;
    case PERSPECTIVE_NEAR:
        // Top narrower than bottom (wide base, vanishing top)
        top_env = build_trapezoid_side(bx, bw, y_top, insetF, top_count);
        bot_env = build_flat(bx, bw, y_bottom, bot_count);
        break;
    case PERSPECTIVE_FAR:
        // Bottom narrower than top
        top_env = build_flat(bx, bw, y_top, top_count);
        bot_env = build_trapezoid_side(bx, bw, y_bottom, insetF, bot_count);
        break;
    case WAVE:
        // Both curves wave — requires count >= 3 to look like anything
        top_env = build_wave(bx, bw, y_top,    -amp * 0.6, top_count);
        bot_env = build_wave(bx, bw, y_bottom, +amp * 0.6, bot_count);
        break;
    }
}

} // namespace curvz::utils
