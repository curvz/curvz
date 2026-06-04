//
// TextStyleInterop.cpp — see TextStyleInterop.hpp. Two small composable pumps:
// box_baseline (node scalars -> a ResolvedTextStyle floor) and
// resolve_paragraph_baseline (binding-or-box, the fitter's per-paragraph seam).
//

#include "style/TextStyleInterop.hpp"

namespace Curvz {
namespace style {

ResolvedTextStyle box_baseline(const std::string& family, double size,
                               bool bold, bool italic, double letter_spacing,
                               double leading_px, ParaAlign align) {
    ResolvedTextStyle r;                  // floor defaults: indents 0, tabs "", black
    r.family         = family;
    r.size           = size;
    r.bold           = bold;
    r.italic         = italic;
    r.letter_spacing = letter_spacing;
    r.leading_px     = leading_px;        // 0 == derive from metrics
    r.align          = align;
    return r;
}

ResolvedTextStyle resolve_paragraph_baseline(const std::vector<Curvz::AttrSpan>& spans,
                                             unsigned para_start_byte,
                                             const TextStyleLibrary& lib,
                                             const ResolvedTextStyle& box) {
    const std::string id =
        curvz::utils::paragraph_attr_svalue_for_byte(spans, curvz::utils::kCurvzStyleAttr,
                                                     para_start_byte);
    if (id.empty()) return box;           // unbound -> box-level default
    return lib.resolve(id);               // bound -> resolved chain (total: floor-bottomed)
}

} // namespace style
} // namespace Curvz
