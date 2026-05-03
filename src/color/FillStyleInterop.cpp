//
// FillStyleInterop.cpp — implementation of the Paint <-> FillStyle bridge.
//

#include "color/FillStyleInterop.hpp"

#include "color/SwatchLibrary.hpp"  // Swatch lookup for the library-aware overload
#include "CurvzLog.hpp"

namespace Curvz {
namespace color {

Paint to_paint(const FillStyle& fs) {
    switch (fs.type) {
        case FillStyle::Type::None:
            return None{};
        case FillStyle::Type::CurrentColor:
            return CurrentColor{};
        case FillStyle::Type::Solid:
            return Solid{ Color{ fs.r, fs.g, fs.b, fs.a } };
        case FillStyle::Type::LinearGradient:
        case FillStyle::Type::RadialGradient: {
            // S93 m1: faithful gradient round-trip. Pre-S93 this branch
            // degraded to "first stop's flat colour" because color::Paint
            // had no gradient arm; that was the documented Phase-5
            // placeholder. Now that Paint has a Gradient arm we copy
            // stops + geometry straight across.
            //
            // FillStyle::GradientStop carries bare r/g/b/a; the color::
            // sibling carries Color. The mapping is mechanical.
            Gradient g;
            g.kind = (fs.type == FillStyle::Type::RadialGradient)
                         ? Gradient::Kind::Radial
                         : Gradient::Kind::Linear;
            g.stops.reserve(fs.stops.size());
            for (const auto& s : fs.stops) {
                g.stops.push_back(
                    GradientStop{ s.offset, Color{ s.r, s.g, s.b, s.a } });
            }
            g.g_x1 = fs.g_x1; g.g_y1 = fs.g_y1;
            g.g_x2 = fs.g_x2; g.g_y2 = fs.g_y2;
            g.g_r  = fs.g_r;
            return g;
        }
    }
    // Defensive default. The cases above are exhaustive for the
    // current FillStyle::Type enum, so this is unreachable in practice —
    // but an enum can gain a case without this file knowing, and -Wswitch
    // will flag us at that point. Log + fall back to CurrentColor (the
    // FillStyle default state) if it ever does execute.
    LOG_WARN("to_paint: unknown FillStyle::Type {}; falling back to CurrentColor",
             static_cast<int>(fs.type));
    return CurrentColor{};
}

// Private helper: populate a FillStyle from a resolved Color as a Solid.
// Both overloads converge here for the SwatchRef case.
static void fill_solid(FillStyle& out, const Color& c) {
    out.type = FillStyle::Type::Solid;
    out.r = c.r;
    out.g = c.g;
    out.b = c.b;
    out.a = c.a;
}

// Core visit that handles the four always-pure cases. The SwatchRef
// handler is passed in because it differs between the two overloads
// (one has a library, one doesn't).
template <typename SwatchRefHandler>
static FillStyle to_fillstyle_impl(const Paint& p, SwatchRefHandler sr_handler) {
    FillStyle out;
    std::visit(Overloaded{
        [&](const None&) {
            out.type = FillStyle::Type::None;
            // Leave rgba at default (0,0,0,1). Semantically undefined for
            // type=None, but consistent with how existing code initialises
            // a "none" FillStyle — keeps any legacy RGBA-reading code from
            // seeing garbage.
        },
        [&](const CurrentColor&) {
            out.type = FillStyle::Type::CurrentColor;
            // Same comment as None: rgba left at default.
        },
        [&](const Solid& s) {
            fill_solid(out, s.color);
        },
        [&](const SwatchRef& sr) {
            sr_handler(out, sr);
        },
        [&](const Gradient& g) {
            // S93 m1: round-trip gradients straight across.
            // FillStyle::Type discriminates Linear vs Radial; the
            // FillStyle layer doesn't have a separate Kind enum.
            out.type = (g.kind == Gradient::Kind::Radial)
                           ? FillStyle::Type::RadialGradient
                           : FillStyle::Type::LinearGradient;
            // Leave the legacy r/g/b/a fields at default — FillStyle
            // ignores them for gradient types but the contract says
            // "kept untouched so that flipping back to Solid restores
            // the previous solid colour". For a Paint→FillStyle path
            // there's no "previous" solid to remember, so default is
            // the right answer.
            out.stops.clear();
            out.stops.reserve(g.stops.size());
            for (const auto& s : g.stops) {
                // The FillStyle (path-side) stop type is Curvz::GradientStop —
                // bare r/g/b/a fields. We're inside namespace Curvz::color so
                // bare `GradientStop` resolves to color::GradientStop instead;
                // qualify explicitly.
                ::Curvz::GradientStop fs_stop;
                fs_stop.offset = s.offset;
                fs_stop.r = s.color.r;
                fs_stop.g = s.color.g;
                fs_stop.b = s.color.b;
                fs_stop.a = s.color.a;
                out.stops.push_back(fs_stop);
            }
            out.g_x1 = g.g_x1; out.g_y1 = g.g_y1;
            out.g_x2 = g.g_x2; out.g_y2 = g.g_y2;
            out.g_r  = g.g_r;
        },
    }, p);
    return out;
}

FillStyle to_fillstyle(const Paint& p) {
    // Library-less overload. SwatchRef degrades to its cached fallback
    // colour — always a visible result, never an assert. This is the form
    // used by renderers that don't have the project in scope (e.g. the
    // icon scanner, preview thumbnails).
    return to_fillstyle_impl(p, [](FillStyle& out, const SwatchRef& sr) {
        fill_solid(out, sr.fallback);
    });
}

FillStyle to_fillstyle(const Paint& p, const SwatchLibrary& lib) {
    // Library-aware overload. Prefer the live swatch colour; fall back to
    // the cached one if the ref is dead. Only solid swatches resolve to a
    // FillStyle::Solid — future gradient kinds that can't be represented
    // as a single colour will need a different resolution path, which is
    // why we dispatch through std::visit on the Swatch variant rather
    // than blindly assuming it's a SolidSwatch.
    return to_fillstyle_impl(p, [&](FillStyle& out, const SwatchRef& sr) {
        if (const Swatch* s = lib.find_swatch(sr.id)) {
            std::visit(Overloaded{
                [&](const SolidSwatch& ss) {
                    fill_solid(out, ss.color);
                },
                // Future variant cases compile-fail here. When gradients
                // land, the decision will be: do we resolve to fallback,
                // or to a gradient-approximation colour? That's a Phase
                // 4.5 call, not one we make today.
            }, *s);
            return;
        }
        // Dead ref. Cached fallback keeps it visible.
        fill_solid(out, sr.fallback);
    });
}

} // namespace color
} // namespace Curvz

