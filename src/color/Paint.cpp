//
// Paint.cpp — implementation of Paint resolution.
//
// Single entry point (resolve_paint) that turns any Paint variant into a
// concrete Color. Separated from Paint.hpp so the header doesn't need to
// include SwatchLibrary.hpp (which would drag nlohmann/json into every
// translation unit that touches Paint).
//
// Phase 5 M1: this is the only new consumer. Existing renderers still go
// through FillStyleInterop::to_fillstyle and FillStyle directly; the
// interop bridge now calls resolve_paint internally when a SwatchRef is
// encountered and a library is available.
//

#include "color/Paint.hpp"

#include "color/Swatch.hpp"
#include "color/SwatchLibrary.hpp"
#include "CurvzLog.hpp"

#include <nlohmann/json.hpp>

namespace Curvz {
namespace color {

Color resolve_paint(const Paint& p,
                    const SwatchLibrary* lib,
                    const Color& current_color) {
    return std::visit(Overloaded{
        [&](const None&) -> Color {
            // Transparent black. Callers that distinguish "no fill" from
            // "transparent fill" should check the variant type directly
            // (is_none(p) helper below) rather than relying on alpha==0.
            return Color::transparent();
        },
        [&](const CurrentColor&) -> Color {
            // The SVG currentColor semantic: defer to the containing
            // element's `color` property. Curvz's equivalent is whatever
            // the renderer passes in as the ambient. For icons destined
            // for Folio, this is the parent widget's foreground.
            return current_color;
        },
        [&](const Solid& s) -> Color {
            return s.color;
        },
        [&](const SwatchRef& sr) -> Color {
            // Library lookup. Fall back to the cached colour if:
            //   - no library was supplied (e.g. SVG preview before the
            //     project is fully loaded — rare but possible)
            //   - the id is unknown (swatch was deleted without
            //     convert-to-solid; the ref is a tombstone)
            //   - the swatch exists but is a non-solid kind the caller
            //     hasn't taught itself about yet (future gradients)
            //
            // The fallback is always a safe visible colour because it's
            // refreshed on every set_paint() and every live recolour.
            if (lib != nullptr) {
                if (const Swatch* s = lib->find_swatch(sr.id)) {
                    // SolidSwatch is the only kind today. std::visit here
                    // future-proofs: when gradients land, this branch is
                    // where they'd rasterize-or-approximate to a single
                    // representative colour for fallback contexts (e.g.
                    // the active-paint ring). For now, only solid applies.
                    return std::visit(Overloaded{
                        [](const SolidSwatch& ss) -> Color {
                            return ss.color;
                        },
                        // Future cases land here. Compiler will flag the
                        // missing overload when a new variant case is
                        // added to Swatch.
                    }, *s);
                }
            }
            // Dead ref or no library. fallback is the last-known-good
            // resolved colour.
            return sr.fallback;
        },
        [&](const Gradient& g) -> Color {
            // S93 m1: a gradient has no single representative colour.
            // resolve_paint is used for "I need ONE colour" contexts —
            // the active-paint chip, fallback rasterisation, contexts
            // where a renderer can't actually paint a gradient. Return
            // the first stop's colour as a recognisable approximation;
            // empty stops degrade to opaque black (matches the legacy
            // FillStyleInterop placeholder shape pre-S93).
            if (!g.stops.empty()) return g.stops.front().color;
            return Color::black();
        },
    }, p);
}

// ── JSON round-trip ─────────────────────────────────────────────────────────
//
// See Paint.hpp for schema notes. Canonical serialiser — used by the Style
// library's project.json round-trip (S76 m3b) and, later, by SwatchRef SVG
// round-trip in Phase 5. Dispatches on variant type via Overloaded, same
// shape as resolve_paint above.
//
// from_json uses .value(...) defaulting throughout. Anything malformed —
// missing "type", unknown "type", unparseable hex — degrades to None{}
// with a LOG_WARN. Matches SwatchLibrary::load_pool's permissive shape.

nlohmann::json paint_to_json(const Paint& p) {
    return std::visit(Overloaded{
        [](const None&) -> nlohmann::json {
            return { {"type", "none"} };
        },
        [](const CurrentColor&) -> nlohmann::json {
            return { {"type", "currentcolor"} };
        },
        [](const Solid& s) -> nlohmann::json {
            return {
                {"type",  "solid"},
                {"color", to_hex(s.color)}
            };
        },
        [](const SwatchRef& sr) -> nlohmann::json {
            // fallback is always serialised: the whole point of the
            // cached colour is that it survives a swatch deletion. See
            // Paint.hpp header on SwatchRef.
            return {
                {"type",     "swatchref"},
                {"id",       sr.id},
                {"fallback", to_hex(sr.fallback)}
            };
        },
        [](const Gradient& g) -> nlohmann::json {
            // S93 m1: gradient round-trip. Schema mirrors Paint.hpp's
            // "type"-discriminated shape. Geometry fields use short
            // names (x1/y1/x2/y2/r) — they're already namespaced under
            // the gradient object, no need for the `g_` prefix the
            // C++ struct uses (which is there to disambiguate from
            // SceneNode bbox fields).
            nlohmann::json stops = nlohmann::json::array();
            for (const auto& s : g.stops) {
                stops.push_back({
                    {"offset", s.offset},
                    {"color",  to_hex(s.color)}
                });
            }
            return {
                {"type",  "gradient"},
                {"kind",  g.kind == Gradient::Kind::Radial ? "radial" : "linear"},
                {"stops", std::move(stops)},
                {"x1",    g.g_x1}, {"y1", g.g_y1},
                {"x2",    g.g_x2}, {"y2", g.g_y2},
                {"r",     g.g_r}
            };
        },
    }, p);
}

Paint paint_from_json(const nlohmann::json& j) {
    if (!j.is_object()) {
        LOG_WARN("paint_from_json: not a JSON object, defaulting to None");
        return None{};
    }
    std::string type = j.value("type", std::string{});
    if (type == "none") {
        return None{};
    }
    if (type == "currentcolor") {
        return CurrentColor{};
    }
    if (type == "solid") {
        std::string hex = j.value("color", std::string{"#000000"});
        auto parsed = from_hex(hex);
        if (!parsed) {
            LOG_WARN("paint_from_json: solid color '{}' unparseable, using black",
                     hex);
            return Solid{Color::black()};
        }
        return Solid{*parsed};
    }
    if (type == "swatchref") {
        SwatchRef sr;
        sr.id = j.value("id", std::string{});
        if (sr.id.empty()) {
            LOG_WARN("paint_from_json: swatchref missing id, defaulting to None");
            return None{};
        }
        std::string fb_hex = j.value("fallback", std::string{"#000000"});
        auto fb = from_hex(fb_hex);
        // A missing/bad fallback is survivable — the ref still resolves
        // via the library when present. Use black as the safe default;
        // this mirrors SolidSwatch load_pool behaviour.
        if (!fb) {
            LOG_WARN("paint_from_json: swatchref '{}' fallback '{}' unparseable, "
                     "using black", sr.id, fb_hex);
            sr.fallback = Color::black();
        } else {
            sr.fallback = *fb;
        }
        return sr;
    }
    if (type == "gradient") {
        Gradient g;
        std::string kind = j.value("kind", std::string{"linear"});
        g.kind = (kind == "radial") ? Gradient::Kind::Radial
                                    : Gradient::Kind::Linear;
        g.g_x1 = j.value("x1", 0.0);
        g.g_y1 = j.value("y1", 0.5);
        g.g_x2 = j.value("x2", 1.0);
        g.g_y2 = j.value("y2", 0.5);
        g.g_r  = j.value("r",  0.5);
        if (j.contains("stops") && j["stops"].is_array()) {
            for (const auto& sj : j["stops"]) {
                if (!sj.is_object()) continue;
                GradientStop s;
                s.offset = sj.value("offset", 0.0);
                std::string hex = sj.value("color", std::string{"#000000"});
                auto parsed = from_hex(hex);
                if (parsed) {
                    s.color = *parsed;
                } else {
                    LOG_WARN("paint_from_json: gradient stop color '{}' "
                             "unparseable, using black", hex);
                    s.color = Color::black();
                }
                g.stops.push_back(s);
            }
        }
        return g;
    }
    LOG_WARN("paint_from_json: unknown type '{}', defaulting to None", type);
    return None{};
}

} // namespace color
} // namespace Curvz
