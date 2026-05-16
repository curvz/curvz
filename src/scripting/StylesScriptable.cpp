// scripting/StylesScriptable.cpp ─────────────────────────────────────────────
//
// s222 m1 — implementation of the fourth row-bound model Scriptable.
// See StylesScriptable.hpp for the verb/query surface, lifetime notes,
// and the design rationale for the s222 m1 fix-1 panel-navigation step
// (after-mutation `set_active_category`) — different mechanism from
// SwatchesScriptable's library-side palette-membership fix, same
// underlying lesson: library-collection Scriptables must drive panel
// visibility through whatever the panel uses to filter.
//
// ── Why execute-then-push, not mutate-then-push ─────────────────────────────
//
// LayersScriptable follows the panel's "mutate first, push command with
// before/after" shape because EditLayerFieldCommand carries only the
// diff. AddStyleCommand / UpdateStyleCommand / RemoveStyleCommand all
// carry the mutation in their own execute() body — same shape as the
// AddSwatchCommand / EditSwatchCommand / RemoveSwatchCommand family
// SwatchesScriptable rides on. The closest in-tree precedent is
// StylesPanel::action_create_empty's on_committed callback (which is
// the canonical example cited in SwatchesScriptable.cpp), and every
// new StylesPanel action since uses the same "build, execute, push"
// shape.
//
// ── App-tier guard ──────────────────────────────────────────────────────────
//
// Every mutating verb refuses on an app-tier id BEFORE constructing any
// command. The library would also refuse (update_style / remove_style
// return false on app-tier ids), but checking up-front keeps the command
// stack clean — no zombie commands pushed for ops that were no-ops in
// the library. The panel hides the affordance entirely via its menu;
// the Scriptable's verb is the parallel guard.
//
// ── all_style_ids() — assembled, not provided ───────────────────────────────
//
// StyleLibrary doesn't expose a single flat-id accessor (it iterates by
// category to match its panel's chooser-driven UI). The Scriptable
// assembles the flat list by walking app_categories() and then
// user_categories(), pulling styles per category. Same iteration order
// SwatchLibrary::all_swatch_ids() promises (app/default first, then
// user/custom, insertion order within each tier — though "insertion
// order" for styles is per-category insertion order).
//
// The helper lives in this TU as a file-local function rather than on
// StyleLibrary itself. Reasoning: the Scriptable is currently the only
// consumer that needs a flat list; the library's category-grouped API
// matches its panel's UX. If a second consumer arrives (e.g. a future
// import/export bridge that wants a flat scan), the right move is to
// promote this to StyleLibrary as a real method — but that's a
// "find the seam" decision that costs more than it saves with one
// consumer.

#include "scripting/StylesScriptable.hpp"
#include "scripting/Scriptable.hpp"

#include "CommandHistory.hpp"
#include "CurvzProject.hpp"
#include "StylesPanel.hpp"     // s222 m1 fix-1 — set_active_category
#include "color/Color.hpp"     // s225 m1 — from_hex / to_hex for paint + shadow
#include "color/Paint.hpp"     // s225 m1 — Paint variant for fill/stroke setters
#include "color/SwatchLibrary.hpp" // s225 m1 — swatch fallback resolution
#include "style/Style.hpp"
#include "style/StyleLibrary.hpp"

#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace curvz::scripting {

namespace {

// ── Argument coercion helpers ──────────────────────────────────────────────
//
// Same shape as the equivalent block in SwatchesScriptable.cpp /
// LayersScriptable.cpp / GuidesScriptable.cpp. Kept duplicated rather
// than hoisted because the coercion is small, file-local, and folding
// it to a shared site would add a header dependency for what amounts
// to a two-line helper.
//
// s225 m1: extended with arg_as_bool and arg_as_double (lifted in shape
// from GuidesScriptable / LayersScriptable) for the per-field setter
// surface. Same fallback contract as the layer/guide helpers — accept
// the native ScriptValue kind, fall back to the supplied default for
// anything else (no exceptions, no logging; mismatched arg = silent
// no-op from the caller's perspective).
std::string arg_as_string(const ScriptValue& v) {
    if (v.kind == ValueKind::String) return v.s;
    return {};
}

bool arg_as_bool(const ScriptValue& v, bool fallback) {
    if (v.kind == ValueKind::Bool) return v.b;
    if (v.kind == ValueKind::String) {
        if (v.s == "true")  return true;
        if (v.s == "false") return false;
    }
    return fallback;
}

double arg_as_double(const ScriptValue& v, double fallback) {
    switch (v.kind) {
        case ValueKind::Double: return v.d;
        case ValueKind::Int:    return static_cast<double>(v.i);
        default:                return fallback;
    }
}

// ── Paint discriminator ────────────────────────────────────────────────────
//
// s225 m1. Encode/decode a Paint variant to/from the script's string-
// shaped wire format:
//
//   "none"             ↔ Paint::None
//   "currentcolor"     ↔ Paint::CurrentColor
//   "#rrggbb[aa]"      ↔ Paint::Solid (alpha defaults to opaque if 6-hex)
//   "swatch:<id>"      ↔ Paint::SwatchRef
//
// Gradient is unaddressable through this surface — encode returns ""
// for it (the caller's read query produces an empty string, which is
// distinguishable from any valid spec because every valid spec is
// non-empty). The same "" return covers any future variant we add
// without an encoder update; the right thing is to extend this site
// rather than route around it.
//
// Both pumps live next to each other in the same file (curvz utils
// rule: round-trippable encoders go in pairs). Adding a sixth Paint
// variant means editing this block — std::visit's exhaustive-match
// requirement makes that visible at compile time.
//
// Why no `app:`-style strict-prefix check on swatch ids: the swatch
// library mints both "default_*" (system) and "sw_*" (user) ids, and
// future tier discriminators may add more. The Scriptable accepts
// anything after "swatch:" as a candidate id; SwatchLibrary::find_swatch
// returns nullptr on miss, the fallback stays at its default Color{}
// value (transparent), and resolve_paint's dead-ref graceful-degradation
// path renders the default. This matches how the panel's swatch picker
// behaves when a referenced swatch is later deleted — the ref persists
// in the Paint, the visible result degrades to the cached fallback.
constexpr std::string_view kSwatchPrefix = "swatch:";

std::string encode_paint(const Curvz::color::Paint& p) {
    return std::visit(Curvz::color::Overloaded{
        [](const Curvz::color::None&)         -> std::string { return "none"; },
        [](const Curvz::color::CurrentColor&) -> std::string { return "currentcolor"; },
        [](const Curvz::color::Solid& s)      -> std::string { return Curvz::color::to_hex(s.color); },
        [](const Curvz::color::SwatchRef& r)  -> std::string { return std::string(kSwatchPrefix) + r.id; },
        [](const Curvz::color::Gradient&)     -> std::string { return ""; },
    }, p);
}

// Parse a paint spec. Returns nullopt for unknown / malformed input
// (caller treats it as no-op, same as an invalid hex on `shadow_color`).
// `swatch_lib` is consulted for SwatchRef fallback resolution; nullptr
// is tolerated (test-harness path) — the SwatchRef writes through with
// a default-Color fallback, matching the "dead ref" degradation path
// resolve_paint already handles.
std::optional<Curvz::color::Paint>
decode_paint(const std::string& spec,
             const Curvz::color::SwatchLibrary* swatch_lib) {
    if (spec == "none")          return Curvz::color::Paint{Curvz::color::None{}};
    if (spec == "currentcolor")  return Curvz::color::Paint{Curvz::color::CurrentColor{}};

    // "#rrggbb" or "#rrggbbaa" — Color::from_hex handles both, plus the
    // shorthand "#rgb" / "#rgba" cases. We accept anything from_hex
    // accepts; the Scriptable doesn't gatekeep stricter than the
    // colour atom does.
    if (!spec.empty() && spec.front() == '#') {
        auto parsed = Curvz::color::from_hex(spec);
        if (!parsed) return std::nullopt;
        return Curvz::color::Paint{Curvz::color::Solid{*parsed}};
    }

    if (spec.rfind(kSwatchPrefix, 0) == 0) {
        std::string sw_id = spec.substr(kSwatchPrefix.size());
        if (sw_id.empty()) return std::nullopt;
        Curvz::color::SwatchRef ref{sw_id};
        // Resolve the fallback colour from the live swatch if we can.
        // Same lockstep the inspector's picker maintains — set_paint
        // captures the resolved colour at the moment of setting; the
        // fallback diverges from "live" only when the swatch is later
        // edited or deleted. For the script-driven path the verb runs
        // once at set-time; live re-resolution is the swatch-edit
        // signal's job, not ours.
        if (swatch_lib) {
            if (const auto* sw = swatch_lib->find_swatch(sw_id)) {
                if (const auto* solid =
                        std::get_if<Curvz::color::SolidSwatch>(sw)) {
                    ref.fallback = solid->color;
                }
                // Future swatch variants (GradientSwatch / PathBlendSwatch):
                // fallback stays default Color{} (transparent). Same
                // shape as the panel's behaviour — the swatch is a
                // reference, the Paint side carries a fallback colour,
                // and non-solid swatches don't project cleanly to a
                // single colour. This is academic in v1 (SolidSwatch
                // is the only variant), but the right shape is in
                // place for forward-compat.
            }
        }
        return Curvz::color::Paint{std::move(ref)};
    }

    return std::nullopt;
}

// ── LineCap / LineJoin string enums ────────────────────────────────────────
//
// s225 m1. The C++ enums are surfaced as lowercase strings:
//   LineCap : "butt" / "round" / "square"
//   LineJoin: "miter" / "round" / "bevel"
//
// Encode is total (every enum value maps); decode returns nullopt for
// unknown / mis-cased input. The verb-side treats nullopt as no-op,
// matching the "unknown spec is silent" posture of the paint discriminator.
//
// Why lowercase: matches SVG attribute conventions and is consistent
// with how `cap`/`join` would round-trip through SVG export. No
// translation table needed at the export seam.
const char* encode_cap(Curvz::LineCap c) {
    switch (c) {
        case Curvz::LineCap::Butt:   return "butt";
        case Curvz::LineCap::Round:  return "round";
        case Curvz::LineCap::Square: return "square";
    }
    return "butt";  // unreachable; satisfies the compiler
}

std::optional<Curvz::LineCap> decode_cap(const std::string& s) {
    if (s == "butt")   return Curvz::LineCap::Butt;
    if (s == "round")  return Curvz::LineCap::Round;
    if (s == "square") return Curvz::LineCap::Square;
    return std::nullopt;
}

const char* encode_join(Curvz::LineJoin j) {
    switch (j) {
        case Curvz::LineJoin::Miter: return "miter";
        case Curvz::LineJoin::Round: return "round";
        case Curvz::LineJoin::Bevel: return "bevel";
    }
    return "miter";  // unreachable
}

std::optional<Curvz::LineJoin> decode_join(const std::string& s) {
    if (s == "miter") return Curvz::LineJoin::Miter;
    if (s == "round") return Curvz::LineJoin::Round;
    if (s == "bevel") return Curvz::LineJoin::Bevel;
    return std::nullopt;
}

// ── Flat id iteration over a StyleLibrary ──────────────────────────────────
//
// Walks app categories first then user categories, in the same order
// the library would render them. Order within a category matches
// app_styles_in_category / user_styles_in_category's iteration (which
// is insertion order through the per-tier vector).
//
// The two boolean flags let one helper serve all three queries
// (`all_ids`, `user_ids`, `app_ids`) without copy-pasting the walk.
//
// Pointers returned by *_styles_in_category are into library-owned
// storage and stay valid for the duration of this call (no mutations
// happen between the categories() and the per-category fetch).
enum class TierFilter { All, AppOnly, UserOnly };

std::vector<Curvz::style::StyleId>
collect_style_ids(const Curvz::style::StyleLibrary& lib, TierFilter filter) {
    std::vector<Curvz::style::StyleId> out;
    if (filter == TierFilter::All || filter == TierFilter::AppOnly) {
        for (const auto& cat : lib.app_categories()) {
            for (const auto* s : lib.app_styles_in_category(cat)) {
                if (s) out.push_back(s->header.id);
            }
        }
    }
    if (filter == TierFilter::All || filter == TierFilter::UserOnly) {
        for (const auto& cat : lib.user_categories()) {
            for (const auto* s : lib.user_styles_in_category(cat)) {
                if (s) out.push_back(s->header.id);
            }
        }
    }
    return out;
}

// Comma-join a vector of StyleIds. Mirrors the inline join blocks in
// SwatchesScriptable::query — same sentinel shape, same future-foreach-
// grammar caveat (comma-substituted into a `set X to result` line then
// re-used would break the line tokeniser; smoke tests use visual `get`
// for these values rather than `assert ==`).
std::string join_csv(const std::vector<Curvz::style::StyleId>& ids) {
    std::ostringstream os;
    bool first = true;
    for (const auto& id : ids) {
        if (!first) os << ',';
        os << id;
        first = false;
    }
    return os.str();
}

} // anon namespace

// ── StyleProxy — transient per-instance Scriptable ─────────────────────────
//
// Same lifetime contract as SwatchProxy / LayerProxy / GuideProxy:
// materialised by StylesScriptable::proxy_for(id), destroyed when the
// listener's ResolvedObject wrapper goes out of scope at end-of-statement.
// Registered via the `unregistered` tag so proxies are invisible to the
// global registry — `list` shows `styles` exactly once regardless of
// how many proxies are materialised across script runs.
class StyleProxy : public Scriptable {
public:
    StyleProxy(StylesScriptable::ProjectGetter get_project,
               Curvz::CommandHistory* history,
               Curvz::style::StyleId id)
        : Scriptable(unregistered)
        , m_get_project(std::move(get_project))
        , m_history(history)
        , m_id(std::move(id)) {}

    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

private:
    // Resolve our id to a live Style through the current project's
    // library. Returns nullptr if the id no longer addresses any style
    // (deleted between dispatch lines). Same defensive shape as
    // SwatchProxy::resolve.
    const Curvz::style::Style* resolve() const {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return nullptr;
        return proj->styles.find_style(m_id);
    }

    // Library accessor — needed for mutating verbs. Returns nullptr if
    // the project is missing.
    Curvz::style::StyleLibrary* library() const {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        return proj ? &proj->styles : nullptr;
    }

    // s225 m1 — swatch library accessor. Needed by `fill` / `stroke_paint`
    // when the spec is "swatch:<id>"; we resolve the fallback colour
    // through the project's SwatchLibrary at set-time, matching the
    // panel's picker behaviour. Returns nullptr if the project is
    // missing — the SwatchRef still writes through with a default
    // Color{} fallback (transparent), and resolve_paint's dead-ref
    // path handles the render-side degradation.
    const Curvz::color::SwatchLibrary* swatch_library() const {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        return proj ? &proj->swatches : nullptr;
    }

    // s225 m1 — common shape for every appearance-field setter. Takes a
    // mutator lambda that writes the new value into the `after` Style;
    // builds the before/after pair, applies the m_history-or-direct
    // dispatch, pushes UpdateStyleCommand with the supplied description.
    // Caller is responsible for the no-op skip BEFORE calling this
    // (since the skip predicate is field-specific and the lambda is
    // post-decision).
    //
    // Returns Null on success, Null on failure — same shape as the
    // existing rename/category proxy verbs (their return value isn't
    // meaningful; the result slot from the listener doesn't capture
    // void).
    template <typename Mutator>
    ScriptValue push_field_edit(Curvz::style::StyleLibrary* lib,
                                const Curvz::style::Style& live,
                                std::string desc,
                                Mutator&& mutate) {
        Curvz::style::Style before = live;
        Curvz::style::Style after  = before;
        mutate(after);

        if (!m_history) {
            after.header.id = m_id;
            lib->update_style(m_id, std::move(after));
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::UpdateStyleCommand>(
            lib, m_id, std::move(before), std::move(after), std::move(desc));
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    StylesScriptable::ProjectGetter m_get_project;
    Curvz::CommandHistory*          m_history;   // non-owning; outlives us;
                                                 // DEREFERENCED for every
                                                 // mutating verb
    Curvz::style::StyleId           m_id;
};

// ── StyleProxy: invoke ─────────────────────────────────────────────────────

ScriptValue StyleProxy::invoke(std::string_view verb,
                               const ScriptArgs& args) {
    auto* lib = library();
    if (!lib) return ScriptValue::null();
    const Curvz::style::Style* live = resolve();
    if (!live) return ScriptValue::null();

    // App-tier guard. Every mutating verb refuses on a built-in style —
    // the Scriptable provides no "edit the app style" path; the
    // collection-level `duplicate` verb is the affordance.
    if (Curvz::style::is_built_in(m_id)) return ScriptValue::null();

    if (verb == "rename") {
        // Mirror StylesPanel::action_rename's shape. Snapshot the live
        // Style, build the after by copying and changing only
        // header.name, push UpdateStyleCommand. Skip if name didn't
        // change (matches the panel's `if (now->header.name == nm)
        // return;` guard).
        //
        // Empty name is meaningful — the panel's prompt_text path
        // cancels-as-empty (a rename to "" is treated as a cancel),
        // but a script that wants to clear a name should be able to.
        // The Scriptable accepts empty as a real value here; the
        // panel's cancel-as-empty is a UX choice for the modal, not a
        // model rule. The user-visible fallback (panel display)
        // happens at the panel level, not the library.
        if (args.empty()) return ScriptValue::null();
        std::string new_name = arg_as_string(args[0]);
        if (new_name == live->header.name) return ScriptValue::null();

        Curvz::style::Style before = *live;
        Curvz::style::Style after  = before;
        after.header.name = new_name;

        if (!m_history) {
            // Test-harness / no-history path: fall through to direct
            // library call so the verb still functions, just without
            // undo coverage. Mirrors after's header.id back to m_id
            // (defensive — copy from before should already match).
            after.header.id = m_id;
            lib->update_style(m_id, std::move(after));
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::UpdateStyleCommand>(
            lib, m_id, std::move(before), std::move(after),
            std::string("Rename style (script)"));
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    if (verb == "category") {
        // Mirror StylesPanel::action_set_category's shape. Same
        // before/after UpdateStyleCommand, only header.category differs.
        // Empty cat is meaningful — it moves the style to the
        // "(uncategorised)" panel bucket; "uncategorised" is a panel
        // rendering convention for empty-string category, not a
        // separate enum value.
        if (args.empty()) return ScriptValue::null();
        std::string new_cat = arg_as_string(args[0]);
        if (new_cat == live->header.category) return ScriptValue::null();

        Curvz::style::Style before = *live;
        Curvz::style::Style after  = before;
        after.header.category = new_cat;

        if (!m_history) {
            after.header.id = m_id;
            lib->update_style(m_id, std::move(after));
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::UpdateStyleCommand>(
            lib, m_id, std::move(before), std::move(after),
            std::string("Set style category (script)"));
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    // ── s225 m1 appearance fields ──────────────────────────────────────────
    //
    // Per-field setters for fill / stroke / shadow. Same shape as
    // rename / category above — full before/after Style snapshots into
    // UpdateStyleCommand via push_field_edit. The skip-no-op guard
    // lives before the helper call so we don't push commands for
    // value-unchanged writes (matches the inspector's "skip unchanged
    // commit" predicate, S98 lesson).
    //
    // The fields are grouped (fill, then stroke_*, then shadow_*) to
    // keep related setters together in the file. The decode helpers
    // in the anon namespace handle the spec-string parsing; the
    // verbs themselves are short.

    if (verb == "fill") {
        // s225 m1. Discriminating string for the fill Paint. See the
        // `decode_paint` header comment for the spec format.
        if (args.empty()) return ScriptValue::null();
        std::string spec = arg_as_string(args[0]);
        auto parsed = decode_paint(spec, swatch_library());
        if (!parsed) return ScriptValue::null();  // unknown / malformed
        if (*parsed == live->fill) return ScriptValue::null();  // no-op
        return push_field_edit(lib, *live, "Set style fill (script)",
            [&](Curvz::style::Style& after) { after.fill = std::move(*parsed); });
    }

    if (verb == "stroke_paint") {
        if (args.empty()) return ScriptValue::null();
        std::string spec = arg_as_string(args[0]);
        auto parsed = decode_paint(spec, swatch_library());
        if (!parsed) return ScriptValue::null();
        if (*parsed == live->stroke.paint) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set style stroke paint (script)",
            [&](Curvz::style::Style& after) { after.stroke.paint = std::move(*parsed); });
    }

    if (verb == "stroke_width") {
        // No clamp — see header verb-table comment. Inspector clamps at
        // its UI layer; the model accepts whatever the script supplies.
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->stroke.width);
        if (v == live->stroke.width) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set style stroke width (script)",
            [&](Curvz::style::Style& after) { after.stroke.width = v; });
    }

    if (verb == "stroke_cap") {
        if (args.empty()) return ScriptValue::null();
        auto parsed = decode_cap(arg_as_string(args[0]));
        if (!parsed) return ScriptValue::null();  // unknown vocab
        if (*parsed == live->stroke.cap) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set style stroke cap (script)",
            [&](Curvz::style::Style& after) { after.stroke.cap = *parsed; });
    }

    if (verb == "stroke_join") {
        if (args.empty()) return ScriptValue::null();
        auto parsed = decode_join(arg_as_string(args[0]));
        if (!parsed) return ScriptValue::null();
        if (*parsed == live->stroke.join) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set style stroke join (script)",
            [&](Curvz::style::Style& after) { after.stroke.join = *parsed; });
    }

    if (verb == "stroke_miter_limit") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->stroke.miter_limit);
        if (v == live->stroke.miter_limit) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set style stroke miter (script)",
            [&](Curvz::style::Style& after) { after.stroke.miter_limit = v; });
    }

    if (verb == "shadow_enabled") {
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->shadow.enabled);
        if (v == live->shadow.enabled) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set style shadow enabled (script)",
            [&](Curvz::style::Style& after) { after.shadow.enabled = v; });
    }

    if (verb == "shadow_dx") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->shadow.dx);
        if (v == live->shadow.dx) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set style shadow dx (script)",
            [&](Curvz::style::Style& after) { after.shadow.dx = v; });
    }

    if (verb == "shadow_dy") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->shadow.dy);
        if (v == live->shadow.dy) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set style shadow dy (script)",
            [&](Curvz::style::Style& after) { after.shadow.dy = v; });
    }

    if (verb == "shadow_blur") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->shadow.blur);
        if (v == live->shadow.blur) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set style shadow blur (script)",
            [&](Curvz::style::Style& after) { after.shadow.blur = v; });
    }

    if (verb == "shadow_color") {
        // Writes the RGBA bundle (color_r/g/b/a). NOT the separate
        // opacity dial — that's `shadow_opacity` below. Hex parse
        // failure is silent no-op (mirror of `fill`'s parse failure
        // behaviour).
        if (args.empty()) return ScriptValue::null();
        auto parsed = Curvz::color::from_hex(arg_as_string(args[0]));
        if (!parsed) return ScriptValue::null();
        // Equality at 8-bit hex granularity (Color::operator==).
        Curvz::color::Color current{
            live->shadow.color_r, live->shadow.color_g,
            live->shadow.color_b, live->shadow.color_a};
        if (*parsed == current) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set style shadow color (script)",
            [&](Curvz::style::Style& after) {
                after.shadow.color_r = parsed->r;
                after.shadow.color_g = parsed->g;
                after.shadow.color_b = parsed->b;
                after.shadow.color_a = parsed->a;
            });
    }

    if (verb == "shadow_opacity") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->shadow.opacity);
        if (v == live->shadow.opacity) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set style shadow opacity (script)",
            [&](Curvz::style::Style& after) { after.shadow.opacity = v; });
    }

    return ScriptValue::null();
}

// ── StyleProxy: query ──────────────────────────────────────────────────────

ScriptValue StyleProxy::query(std::string_view property) const {
    const Curvz::style::Style* live = resolve();
    if (!live) return ScriptValue::null();

    if (property == "name") {
        return ScriptValue::text(live->header.name);
    }
    if (property == "category") {
        return ScriptValue::text(live->header.category);
    }
    if (property == "iid") {
        return ScriptValue::text(m_id);
    }
    if (property == "is_built_in") {
        // Free function from Style.hpp — cheap prefix check. The
        // StyleLibrary::is_built_in() member is tighter (also checks
        // the id exists in the app list), but we already know the id
        // resolves to a live style (resolve() returned non-null), so
        // the prefix check is sufficient. Matches the discriminant
        // every panel guard uses.
        return ScriptValue::boolean(Curvz::style::is_built_in(m_id));
    }

    // ── s225 m1 appearance reads ───────────────────────────────────────
    //
    // Mirror the setter shape — every property here corresponds to a
    // verb above. Empty return on Gradient (encode_paint maps it to "")
    // is intentional; see encode_paint header comment for rationale.

    if (property == "fill") {
        return ScriptValue::text(encode_paint(live->fill));
    }
    if (property == "stroke_paint") {
        return ScriptValue::text(encode_paint(live->stroke.paint));
    }
    if (property == "stroke_width") {
        return ScriptValue::real(live->stroke.width);
    }
    if (property == "stroke_cap") {
        return ScriptValue::text(encode_cap(live->stroke.cap));
    }
    if (property == "stroke_join") {
        return ScriptValue::text(encode_join(live->stroke.join));
    }
    if (property == "stroke_miter_limit") {
        return ScriptValue::real(live->stroke.miter_limit);
    }
    if (property == "shadow_enabled") {
        return ScriptValue::boolean(live->shadow.enabled);
    }
    if (property == "shadow_dx") {
        return ScriptValue::real(live->shadow.dx);
    }
    if (property == "shadow_dy") {
        return ScriptValue::real(live->shadow.dy);
    }
    if (property == "shadow_blur") {
        return ScriptValue::real(live->shadow.blur);
    }
    if (property == "shadow_color") {
        // Reconstruct a Color from the four channel doubles and
        // route through to_hex — same shape SwatchesScriptable uses
        // for swatch colour reads (lowercase, alpha suffix when
        // a < 1.0).
        Curvz::color::Color c{
            live->shadow.color_r, live->shadow.color_g,
            live->shadow.color_b, live->shadow.color_a};
        return ScriptValue::text(Curvz::color::to_hex(c));
    }
    if (property == "shadow_opacity") {
        return ScriptValue::real(live->shadow.opacity);
    }

    return ScriptValue::null();
}

std::vector<std::string> StyleProxy::verbs() const {
    return {
        "rename",
        "category",
        // s225 m1 — per-field setters
        "fill",
        "stroke_paint",
        "stroke_width",
        "stroke_cap",
        "stroke_join",
        "stroke_miter_limit",
        "shadow_enabled",
        "shadow_dx",
        "shadow_dy",
        "shadow_blur",
        "shadow_color",
        "shadow_opacity",
    };
}

std::vector<std::string> StyleProxy::properties() const {
    return {
        "name", "category", "is_built_in", "iid",
        // s225 m1 — per-field reads (mirror the verb list)
        "fill",
        "stroke_paint",
        "stroke_width",
        "stroke_cap",
        "stroke_join",
        "stroke_miter_limit",
        "shadow_enabled",
        "shadow_dx",
        "shadow_dy",
        "shadow_blur",
        "shadow_color",
        "shadow_opacity",
    };
}

// ── StylesScriptable ───────────────────────────────────────────────────────

StylesScriptable::StylesScriptable(ProjectGetter get_project,
                                   Curvz::CommandHistory* history,
                                   PanelGetter get_panel)
    : Scriptable("styles")
    , m_get_project(std::move(get_project))
    , m_history(history)
    , m_get_panel(std::move(get_panel)) {
    // Registry registration happens in the Scriptable base ctor under
    // the name "styles". MainWindow holds us as a member; the registry
    // entry lives for the window's lifetime.
}

// ── Router hooks ───────────────────────────────────────────────────────────

bool StylesScriptable::can_resolve(std::string_view key) const {
    if (key.empty()) return false;
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return false;
    // StyleLibrary::find_style checks BOTH tiers (user first, then app).
    // Either-tier addressability is correct here — an app style is
    // queryable even if it's not editable.
    return proj->styles.find_style(std::string(key)) != nullptr;
}

std::unique_ptr<Scriptable>
StylesScriptable::proxy_for(std::string_view key) {
    if (!can_resolve(key)) return nullptr;
    return std::make_unique<StyleProxy>(m_get_project, m_history,
                                        std::string(key));
}

// ── Collection invoke ──────────────────────────────────────────────────────

ScriptValue StylesScriptable::invoke(std::string_view verb,
                                     const ScriptArgs& args) {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return ScriptValue::null();
    auto& lib = proj->styles;

    if (verb == "new") {
        // Create a fresh Style and push AddStyleCommand. Mirrors the
        // shape StylesPanel::action_create_empty uses; the seed is a
        // default-constructed Style (Paint::None fill, default
        // StrokeAppearance, shadow disabled) with header fields supplied
        // by the script (or defaulted to empty).
        //
        // Optional args:
        //   args[0] = name (string, empty allowed — display falls back
        //             to header.id at the panel layer if needed)
        //   args[1] = category (string, empty allowed — lands in the
        //             "(uncategorised)" panel bucket)
        //
        // The library mints a fresh "stl_<uuid>" id inside execute();
        // we read it back from cmd->m_assigned_id and return it.
        // Empty return signals the library rejected the add —
        // defensive, shouldn't happen for a freshly-created style with
        // an empty id.
        Curvz::style::Style s;
        s.header.id.clear();  // library mints
        s.header.name     = args.size() >= 1 ? arg_as_string(args[0])
                                             : std::string{};
        s.header.category = args.size() >= 2 ? arg_as_string(args[1])
                                             : std::string{};
        // s.fill / s.stroke / s.shadow take Paint{None{}} / default
        // StrokeAppearance / disabled-shadow from the data-model
        // defaults — exactly what an "empty" style should look like,
        // matching action_create_empty's seed.

        if (!m_history) {
            // No-history fallback. add_style fires signal_style_added
            // internally, so the panel refresh path still runs — just
            // no undo coverage.
            return ScriptValue::text(lib.add_style(std::move(s)));
        }
        auto cmd = std::make_unique<Curvz::AddStyleCommand>(
            &lib, std::move(s), std::string("Add style (script)"));
        cmd->execute();
        Curvz::style::StyleId new_id = cmd->m_assigned_id;
        m_history->push(std::move(cmd));

        // s222 m1 fix-1: navigate the panel's active category to the
        // new style's category so the user immediately sees the result
        // of the script. Without this, the library is fully populated
        // (signal_style_added has fired, the dropdown contains the new
        // category as an option), but the panel sits on whatever
        // category was selected before the script ran — user sees
        // "nothing happened." See StylesScriptable.hpp's "Panel
        // visibility" block for the full rationale.
        //
        // Read the style back from the library after execute() — this
        // is the authoritative post-add state (in particular if the
        // library normalised anything about the category string,
        // though today it doesn't). Skip the navigation if the library
        // rejected the add (find_style returns nullptr — defensive,
        // shouldn't happen for a freshly-minted empty-id style).
        if (auto* panel = m_get_panel ? m_get_panel() : nullptr) {
            if (const Curvz::style::Style* live = lib.find_style(new_id)) {
                panel->set_active_category(live->header.category,
                                           /*is_app_tier=*/false);
            }
        }
        return ScriptValue::text(new_id);
    }

    if (verb == "delete") {
        // Mirror StylesPanel::action_delete's shape. Read the live
        // Style first to take the snapshot — unlike RemoveSwatchCommand
        // (which only needs the id and snapshots internally),
        // RemoveStyleCommand's ctor takes the full Style value.
        //
        // App-tier guard: refuse before constructing the command.
        if (args.empty()) return ScriptValue::null();
        std::string id = arg_as_string(args[0]);
        if (id.empty()) return ScriptValue::null();
        if (Curvz::style::is_built_in(id)) return ScriptValue::null();
        const Curvz::style::Style* live = lib.find_style(id);
        if (!live) return ScriptValue::null();  // unknown id

        Curvz::style::Style snapshot = *live;  // full pre-remove value

        if (!m_history) {
            lib.remove_style(id);
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::RemoveStyleCommand>(
            &lib, std::move(snapshot), std::string("Delete style (script)"));
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    if (verb == "duplicate") {
        // Mirror StylesPanel::action_duplicate's shape. Deep-copy the
        // source Style, clear its id (library mints a fresh one on
        // add), append " copy" to the name (empty name stays empty —
        // same as StyleLibrary::duplicate_to_user's behaviour and the
        // panel's action_duplicate). Source can be from EITHER tier;
        // the duplicate always lands in user, which IS the panel's
        // "duplicate to user tier" affordance for app styles.
        //
        // Category is preserved verbatim. An app→user duplicate keeps
        // the source's category string (typically "Built-in"); this
        // matches the panel's action_duplicate behaviour exactly (the
        // panel's separate action_edit_copy resets category to "" to
        // push the user into picking a new home in the dialog, but
        // that's a UX flourish — the bare duplicate keeps the
        // category). Scripts that want the reset can chain with the
        // proxy `category ""` verb.
        if (args.empty()) return ScriptValue::null();
        std::string src_id = arg_as_string(args[0]);
        if (src_id.empty()) return ScriptValue::null();
        const Curvz::style::Style* src = lib.find_style(src_id);
        if (!src) return ScriptValue::null();

        Curvz::style::Style copy = *src;
        copy.header.id.clear();
        if (!copy.header.name.empty()) copy.header.name += " copy";

        if (!m_history) {
            return ScriptValue::text(lib.add_style(std::move(copy)));
        }
        auto cmd = std::make_unique<Curvz::AddStyleCommand>(
            &lib, std::move(copy), std::string("Duplicate style (script)"));
        cmd->execute();
        Curvz::style::StyleId new_id = cmd->m_assigned_id;
        m_history->push(std::move(cmd));

        // s222 m1 fix-1: same panel navigation as `new` above. The
        // duplicate always lands in the user tier regardless of the
        // source's tier — so the is_app_tier flag is hardcoded false.
        // For app→user duplicates the category inherits verbatim
        // ("Built-in"), which means the panel switches to the
        // user-tier "Built-in" category (yes, that's a distinct
        // bucket from the app-tier "Built-in" — m_category_order
        // carries the tier discriminant alongside the name). Mirrors
        // exactly what StylesPanel::action_duplicate does in the
        // panel-driven path, just made script-visible.
        if (auto* panel = m_get_panel ? m_get_panel() : nullptr) {
            if (const Curvz::style::Style* live = lib.find_style(new_id)) {
                panel->set_active_category(live->header.category,
                                           /*is_app_tier=*/false);
            }
        }
        return ScriptValue::text(new_id);
    }

    if (verb == "rename") {
        // Collection-level rename — same as the proxy rename, just
        // accessed by "<id>" + "<name>" pair instead of dotted
        // addressing. Kept on the collection too because the handoff
        // plan lists it explicitly there, and it reads naturally for
        // scripts that have the id in hand without wanting to
        // materialise the proxy:
        //
        //   styles rename "stl_abc" "Sunset"
        //
        // vs.
        //
        //   set sid to "stl_abc"
        //   styles.sid rename "Sunset"
        //
        // Both are supported; both push the same UpdateStyleCommand.
        if (args.size() < 2) return ScriptValue::null();
        std::string id   = arg_as_string(args[0]);
        std::string name = arg_as_string(args[1]);
        if (id.empty()) return ScriptValue::null();
        if (Curvz::style::is_built_in(id)) return ScriptValue::null();
        const Curvz::style::Style* live = lib.find_style(id);
        if (!live) return ScriptValue::null();
        if (live->header.name == name) return ScriptValue::null();  // no-op

        Curvz::style::Style before = *live;
        Curvz::style::Style after  = before;
        after.header.name = name;

        if (!m_history) {
            after.header.id = id;
            lib.update_style(id, std::move(after));
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::UpdateStyleCommand>(
            &lib, id, std::move(before), std::move(after),
            std::string("Rename style (script)"));
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    if (verb == "category") {
        // Collection-level category set — same shape as collection
        // rename, just targeting header.category. Two-arg form:
        // "<id>" "<category>". Same proxy alternative:
        //
        //   styles category "stl_abc" "Outlines"
        //   styles.sid category "Outlines"
        //
        // Both push the same UpdateStyleCommand. Empty category is
        // meaningful — moves the style to the "(uncategorised)"
        // bucket; the panel renders this as the dim-label group.
        if (args.size() < 2) return ScriptValue::null();
        std::string id  = arg_as_string(args[0]);
        std::string cat = arg_as_string(args[1]);
        if (id.empty()) return ScriptValue::null();
        if (Curvz::style::is_built_in(id)) return ScriptValue::null();
        const Curvz::style::Style* live = lib.find_style(id);
        if (!live) return ScriptValue::null();
        if (live->header.category == cat) return ScriptValue::null();

        Curvz::style::Style before = *live;
        Curvz::style::Style after  = before;
        after.header.category = cat;

        if (!m_history) {
            after.header.id = id;
            lib.update_style(id, std::move(after));
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::UpdateStyleCommand>(
            &lib, id, std::move(before), std::move(after),
            std::string("Set style category (script)"));
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    if (verb == "find_by_name") {
        // Parameterised query workaround — until the listener grammar
        // grows query-with-args, find_by_name lives on invoke() (same
        // shape as SwatchesScriptable / LayersScriptable). Returns the
        // id of the first style (any tier) with header.name == arg.
        // Returns "" on miss. Names aren't unique by construction.
        //
        // Iteration order: app tier first (in app_categories() order,
        // each category in insertion order), then user tier. An app
        // style named "Outline" will hide a user style also named
        // "Outline" from find_by_name. This matches the panel's
        // first-hit semantics — and crucially, matches the order
        // collect_style_ids uses for `all_ids`, so a script can
        // reason about which one wins.
        if (args.empty()) return ScriptValue::text("");
        std::string target = arg_as_string(args[0]);
        for (const auto& id : collect_style_ids(lib, TierFilter::All)) {
            const Curvz::style::Style* s = lib.find_style(id);
            if (!s) continue;
            if (s->header.name == target) return ScriptValue::text(id);
        }
        return ScriptValue::text("");
    }

    return ScriptValue::null();
}

// ── Collection query ───────────────────────────────────────────────────────

ScriptValue StylesScriptable::query(std::string_view property) const {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) {
        // No project — defensible empties (same shape as the other
        // model Scriptables: a test running before project open sees
        // 0 / "" not null).
        if (property == "count")    return ScriptValue::integer(0);
        if (property == "all_ids")  return ScriptValue::text("");
        if (property == "user_ids") return ScriptValue::text("");
        if (property == "app_ids")  return ScriptValue::text("");
        return ScriptValue::null();
    }
    const auto& lib = proj->styles;

    if (property == "count") {
        return ScriptValue::integer(
            static_cast<long long>(lib.app_style_count() +
                                   lib.user_style_count()));
    }

    if (property == "all_ids") {
        return ScriptValue::text(
            join_csv(collect_style_ids(lib, TierFilter::All)));
    }

    if (property == "user_ids") {
        return ScriptValue::text(
            join_csv(collect_style_ids(lib, TierFilter::UserOnly)));
    }

    if (property == "app_ids") {
        return ScriptValue::text(
            join_csv(collect_style_ids(lib, TierFilter::AppOnly)));
    }

    // find_by_name is handled in invoke() — it's parameterised, and
    // today's query() can't take args. Listed in verbs() (not
    // properties()) so the listener routes `styles find_by_name "X"`
    // through invoke. Anything that gets here is an unknown property.
    return ScriptValue::null();
}

std::vector<std::string> StylesScriptable::verbs() const {
    return {
        "new",
        "delete",
        "duplicate",
        "rename",
        "category",
        // find_by_name lives here too — see the note in query() about
        // parameterised queries needing the verb form. The listener
        // accepts both `styles find_by_name "<name>"` (which arrives
        // through invoke and produces a return value) and the future
        // `get styles find_by_name "<name>"` form once query-args
        // grammar lands.
        "find_by_name",
    };
}

std::vector<std::string> StylesScriptable::properties() const {
    return {
        "count",
        "all_ids",
        "user_ids",
        "app_ids",
    };
}

} // namespace curvz::scripting
