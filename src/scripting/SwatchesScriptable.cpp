// scripting/SwatchesScriptable.cpp ────────────────────────────────────────────
//
// s221 m1 — implementation of the third row-bound model Scriptable.
// See SwatchesScriptable.hpp for the verb/query surface, lifetime
// notes, and the design rationale for why this one pushes commands
// (unlike GuidesScriptable, which is direct-write only).
//
// ── Why execute-then-push, not mutate-then-push ─────────────────────────────
//
// LayersScriptable follows the panel's "mutate first, push command
// with before/after" shape because EditLayerFieldCommand carries only
// the diff — its execute() body re-applies after-value (used on redo)
// and undo() applies before-value, but the INITIAL application
// happens at the call site.
//
// AddSwatchCommand / RemoveSwatchCommand / EditSwatchCommand all carry
// the mutation in their own execute() body. The closest in-tree
// precedent is StylesPanel::action_create_empty's on_committed
// callback, which builds AddStyleCommand, calls execute() to do the
// mutation AND capture the minted id (m_assigned_id), then pushes the
// command. That's the exact pattern this file uses for every mutating
// verb. The panel's s220 m1a sweep uses the same shape (see
// on_ctx_duplicate_swatch, on_ctx_delete_swatch, on_ctx_rename_swatch
// in SwatchesPanel.cpp).
//
// ── Defaults-tier guard ─────────────────────────────────────────────────────
//
// Every mutating verb refuses on a defaults-tier id BEFORE constructing
// any command. The library would also refuse (update_swatch /
// remove_swatch return false on defaults ids), but checking up-front
// keeps the command stack clean — no zombie commands pushed for ops
// that were no-ops in the library. The panel hides the affordance
// entirely via its menu; the Scriptable's verb is the parallel guard.

#include "scripting/SwatchesScriptable.hpp"
#include "scripting/Scriptable.hpp"

#include "CommandHistory.hpp"
#include "CurvzProject.hpp"
#include "color/Color.hpp"
#include "color/Swatch.hpp"
#include "color/SwatchLibrary.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace curvz::scripting {

namespace {

// ── Argument coercion helpers ──────────────────────────────────────────────
//
// Same shape as the equivalent block in LayersScriptable.cpp and
// GuidesScriptable.cpp. Kept duplicated rather than hoisted because the
// coercion is small, file-local, and folding it to a shared site would
// add a header dependency for what amounts to two-line helpers.

std::string arg_as_string(const ScriptValue& v) {
    if (v.kind == ValueKind::String) return v.s;
    return {};
}

// Pull the SwatchHeader::name out of any Swatch variant. Wraps the
// shared swatch_header accessor in a name-only form so the bodies below
// stay readable.
std::string name_of(const Curvz::color::Swatch& s) {
    return Curvz::color::swatch_header(s).name;
}

// Pull the colour out of a Swatch — but only if it's a SolidSwatch.
// Returns black for non-solid variants. The proxy color verb/query
// guards on get_if<SolidSwatch> separately for the variant case;
// callers that need the variant kind discriminated should use that
// guard, not this helper.
Curvz::color::Color solid_color_or_black(const Curvz::color::Swatch& s) {
    if (const auto* solid = std::get_if<Curvz::color::SolidSwatch>(&s)) {
        return solid->color;
    }
    return Curvz::color::Color::black();
}

} // anon namespace

// ── SwatchProxy — transient per-instance Scriptable ────────────────────────
//
// Same lifetime contract as LayerProxy / GuideProxy: materialised by
// SwatchesScriptable::proxy_for(id), destroyed when the listener's
// ResolvedObject wrapper goes out of scope at end-of-statement.
// Registered via the `unregistered` tag so proxies are invisible to
// the global registry — `list` shows `swatches` exactly once
// regardless of how many proxies are materialised across script runs.
class SwatchProxy : public Scriptable {
public:
    SwatchProxy(SwatchesScriptable::ProjectGetter get_project,
                Curvz::CommandHistory* history,
                Curvz::color::SwatchId id)
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
    // Resolve our id to a live Swatch through the current project's
    // library. Returns nullptr if the id no longer addresses any
    // swatch (deleted between dispatch lines). Same defensive shape as
    // LayerProxy::resolve / GuideProxy::resolve.
    const Curvz::color::Swatch* resolve() const {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return nullptr;
        return proj->swatches.find_swatch(m_id);
    }

    // Library accessor — needed for mutating verbs. Returns nullptr if
    // the project is missing.
    Curvz::color::SwatchLibrary* library() const {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        return proj ? &proj->swatches : nullptr;
    }

    SwatchesScriptable::ProjectGetter m_get_project;
    Curvz::CommandHistory*            m_history;   // non-owning;
                                                   // outlives us;
                                                   // DEREFERENCED for
                                                   // every mutating
                                                   // verb (unlike
                                                   // GuideProxy's
                                                   // captured-but-unused
                                                   // history pointer)
    Curvz::color::SwatchId            m_id;
};

// ── SwatchProxy: invoke ────────────────────────────────────────────────────

ScriptValue SwatchProxy::invoke(std::string_view verb,
                                const ScriptArgs& args) {
    auto* lib = library();
    if (!lib) return ScriptValue::null();
    const Curvz::color::Swatch* live = resolve();
    if (!live) return ScriptValue::null();

    // Defaults-tier guard. Every mutating verb refuses on a defaults
    // swatch — the Scriptable provides no "edit the default" path; the
    // collection-level `duplicate` verb is the affordance.
    if (lib->is_default_swatch(m_id)) return ScriptValue::null();

    if (verb == "rename") {
        // Mirror SwatchesPanel::on_ctx_rename_swatch's shape. Snapshot
        // colour pre-rename, push EditSwatchCommand carrying name_before /
        // name_after with colour unchanged. Empty name is meaningful —
        // it clears the user-supplied name; the inspector falls back
        // to the auto-derived region name. Skip the push entirely if
        // the name didn't change (matches the panel's `if (name ==
        // current) return;` guard).
        if (args.empty()) return ScriptValue::null();
        std::string new_name = arg_as_string(args[0]);
        std::string current  = name_of(*live);
        if (new_name == current) return ScriptValue::null();

        Curvz::color::Color color_snapshot = solid_color_or_black(*live);
        if (!m_history) {
            // Test-harness / no-history path: fall through to direct
            // library call so the verb still functions, just without
            // undo coverage. Same fallback shape as SwatchesPanel.
            lib->rename_swatch(m_id, new_name);
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::EditSwatchCommand>(
            lib, m_id, color_snapshot, color_snapshot,
            current, new_name);
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    if (verb == "color") {
        // Mirror EditSwatchCommand's apply path. Parse the hex, push a
        // command carrying colour_before / colour_after with name
        // unchanged. Invalid hex is silently rejected — same shape as
        // GuidesScriptable's color verb, where from_hex parse failures
        // degrade as no-op.
        //
        // SolidSwatch-only. Gradient/path-blend variants would need
        // their own verbs; we return Null without pushing here.
        if (args.empty()) return ScriptValue::null();
        const auto* solid = std::get_if<Curvz::color::SolidSwatch>(live);
        if (!solid) return ScriptValue::null();

        std::string hex = arg_as_string(args[0]);
        if (hex.empty()) return ScriptValue::null();
        auto parsed = Curvz::color::from_hex(hex);
        if (!parsed) return ScriptValue::null();
        if (*parsed == solid->color) return ScriptValue::null();  // no-op

        std::string name_snapshot = solid->header.name;
        Curvz::color::Color before = solid->color;
        Curvz::color::Color after  = *parsed;

        if (!m_history) {
            // No-history fallback. Reconstruct the swatch with the new
            // colour and call update_swatch directly. Matches the
            // shape EditSwatchCommand::apply uses.
            Curvz::color::SolidSwatch updated = *solid;
            updated.color = after;
            lib->update_swatch(m_id, Curvz::color::Swatch{updated});
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::EditSwatchCommand>(
            lib, m_id, before, after, name_snapshot, name_snapshot);
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    return ScriptValue::null();
}

// ── SwatchProxy: query ─────────────────────────────────────────────────────

ScriptValue SwatchProxy::query(std::string_view property) const {
    const Curvz::color::Swatch* live = resolve();
    if (!live) return ScriptValue::null();

    if (property == "name") {
        return ScriptValue::text(name_of(*live));
    }
    if (property == "iid") {
        return ScriptValue::text(m_id);
    }
    if (property == "is_default") {
        auto* lib = library();
        if (!lib) return ScriptValue::boolean(false);
        return ScriptValue::boolean(lib->is_default_swatch(m_id));
    }
    if (property == "color") {
        // SolidSwatch only — non-solid variants return "". Same
        // defensive empty as the proxy's color verb uses for the
        // mutating side; reads and writes are symmetric on the
        // variant guard.
        if (const auto* solid = std::get_if<Curvz::color::SolidSwatch>(live)) {
            return ScriptValue::text(Curvz::color::to_hex(solid->color));
        }
        return ScriptValue::text("");
    }
    return ScriptValue::null();
}

std::vector<std::string> SwatchProxy::verbs() const {
    return {
        "rename",
        "color",
    };
}

std::vector<std::string> SwatchProxy::properties() const {
    return {
        "name", "color", "is_default", "iid",
    };
}

// ── SwatchesScriptable ─────────────────────────────────────────────────────

SwatchesScriptable::SwatchesScriptable(ProjectGetter get_project,
                                       Curvz::CommandHistory* history)
    : Scriptable("swatches")
    , m_get_project(std::move(get_project))
    , m_history(history) {
    // Registry registration happens in the Scriptable base ctor under
    // the name "swatches". MainWindow holds us as a member; the
    // registry entry lives for the window's lifetime.
}

// ── Router hooks ───────────────────────────────────────────────────────────

bool SwatchesScriptable::can_resolve(std::string_view key) const {
    if (key.empty()) return false;
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return false;
    // SwatchLibrary::find_swatch checks BOTH tiers (custom first, then
    // defaults). Either-tier addressability is correct here — a
    // defaults swatch is queryable even if it's not editable.
    return proj->swatches.find_swatch(std::string(key)) != nullptr;
}

std::unique_ptr<Scriptable>
SwatchesScriptable::proxy_for(std::string_view key) {
    if (!can_resolve(key)) return nullptr;
    return std::make_unique<SwatchProxy>(m_get_project, m_history,
                                         std::string(key));
}

// ── Collection invoke ──────────────────────────────────────────────────────

ScriptValue SwatchesScriptable::invoke(std::string_view verb,
                                       const ScriptArgs& args) {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return ScriptValue::null();
    auto& lib = proj->swatches;

    if (verb == "new") {
        // Create a fresh SolidSwatch and push AddSwatchCommand
        // (standard ctor — scripted adds bypass the popover's
        // already_added factory, which exists only to bridge the
        // live-mid-popover panel UX with command-history semantics).
        //
        // Optional args:
        //   args[0] = name (string, empty allowed — UI falls back to
        //             auto-derived region name on display)
        //   args[1] = hex  (string, "#rrggbb" or "#rrggbbaa"; defaults
        //             to pure black on missing/parse-failure)
        //
        // The library mints a fresh id inside execute(); we read it
        // back from cmd->m_assigned_id (mirrored into swatch_value's
        // header by execute()) and return it. Empty return signals
        // the library rejected the add — defensive, shouldn't happen
        // for a freshly-created swatch with an empty id.
        Curvz::color::SolidSwatch s;
        s.header.id.clear();  // library mints
        s.header.name = args.size() >= 1 ? arg_as_string(args[0])
                                         : std::string{};
        Curvz::color::Color seed = Curvz::color::Color::black();
        if (args.size() >= 2) {
            std::string hex = arg_as_string(args[1]);
            if (!hex.empty()) {
                if (auto parsed = Curvz::color::from_hex(hex)) {
                    seed = *parsed;
                }
                // Unparseable hex falls through to black — matches the
                // proxy color verb's silent-reject posture for hex
                // failures, and the swatch is still created so callers
                // don't get nothing-happened on a typo.
            }
        }
        s.color = seed;

        if (!m_history) {
            // No-history fallback. add_swatch fires
            // signal_swatch_added internally (s220 m1a), so the panel
            // refresh path still runs — just no undo coverage.
            return ScriptValue::text(lib.add_swatch(Curvz::color::Swatch{s}));
        }
        auto cmd = std::make_unique<Curvz::AddSwatchCommand>(
            &lib, Curvz::color::Swatch{s}, "Add swatch (script)");
        cmd->execute();
        Curvz::color::SwatchId new_id = cmd->m_assigned_id;
        m_history->push(std::move(cmd));

        // s221 m1 fix-1: mirror SwatchesPanel::on_new_swatch — add the
        // new swatch to the active palette so the chip appears in the
        // visible grid. Without this, scripted-new swatches land in the
        // library but never appear in the panel (which renders the
        // active palette's contents, not "every swatch in the library").
        //
        // add_to_palette itself is NOT undoable (palette mutations have
        // no command class yet — backlog). But this side-effect
        // round-trips cleanly: undoing the AddSwatchCommand removes the
        // swatch, and remove_swatch strips palette references as part
        // of its library-side cleanup. Redo re-adds the swatch but does
        // NOT replay the palette membership (the standard AddSwatchCommand
        // ctor doesn't snapshot membership — only the already_added
        // factory does, which the panel uses to bridge the popover's
        // live-mid-edit model). For scripted new, the rare redo-after-
        // undo case means the swatch returns to the library but not to
        // the palette. Acceptable for now; the full palette-undoability
        // sweep is its own backlog milestone.
        const Curvz::color::PaletteId& active_pal = lib.active_palette();
        if (!active_pal.empty() && lib.find_palette(active_pal)) {
            lib.add_to_palette(active_pal, new_id);
            // add_to_palette doesn't emit any signal today (palette
            // mutation signals are a backlog item too). Force a refresh
            // by re-emitting signal_swatch_added — same hack pattern
            // the s220 m1a commands use for the palette-replay step
            // (see AddSwatchCommand::execute and RemoveSwatchCommand::undo).
            lib.signal_swatch_added().emit(new_id);
        }
        return ScriptValue::text(new_id);
    }

    if (verb == "delete") {
        // Mirror SwatchesPanel::on_ctx_delete_swatch's shape — except
        // we DON'T do the canvas->unbind_swatch_from_doc step the
        // panel does. That step clears fill_swatch_id / stroke_swatch_id
        // on bound objects, which is a scene-tree mutation that isn't
        // covered by RemoveSwatchCommand's snapshot (see the s220 m1a
        // note in the panel handler). Scripts that need to clean
        // bindings should do their own scene-walk; the Scriptable
        // surface is library-only, by design.
        //
        // Defaults-tier guard: refuse before constructing the command.
        if (args.empty()) return ScriptValue::null();
        std::string id = arg_as_string(args[0]);
        if (id.empty()) return ScriptValue::null();
        if (!lib.find_swatch(id)) return ScriptValue::null();  // unknown id
        if (lib.is_default_swatch(id)) return ScriptValue::null();

        if (!m_history) {
            lib.remove_swatch(id);
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::RemoveSwatchCommand>(&lib, id);
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    if (verb == "duplicate") {
        // Mirror SwatchesPanel::on_ctx_duplicate_swatch's shape.
        // Deep-copy the source SolidSwatch, clear its id (library
        // mints a fresh one on add), append " copy" to the name (empty
        // name stays empty). Source can be from EITHER tier — the
        // duplicate always lands in custom, which IS the panel's
        // "duplicate to edit" affordance for defaults swatches.
        //
        // The Scriptable does NOT do the panel's add_to_palette /
        // touch_recent follow-ups — those are UX state (the duplicate
        // appears in the active palette next to the original; the
        // recents MRU surfaces it on the colour picker). Scripts that
        // want palette placement compose their own AddToPalette verb
        // when palettes get a Scriptable surface; until then, scripted
        // duplicates land in the library without palette membership.
        if (args.empty()) return ScriptValue::null();
        std::string src_id = arg_as_string(args[0]);
        if (src_id.empty()) return ScriptValue::null();
        const Curvz::color::Swatch* src = lib.find_swatch(src_id);
        if (!src) return ScriptValue::null();
        const auto* solid = std::get_if<Curvz::color::SolidSwatch>(src);
        if (!solid) return ScriptValue::null();  // non-solid not yet

        Curvz::color::SolidSwatch copy = *solid;
        copy.header.id.clear();
        if (!copy.header.name.empty()) copy.header.name += " copy";

        if (!m_history) {
            return ScriptValue::text(
                lib.add_swatch(Curvz::color::Swatch{copy}));
        }
        auto cmd = std::make_unique<Curvz::AddSwatchCommand>(
            &lib, Curvz::color::Swatch{copy}, "Duplicate swatch (script)");
        cmd->execute();
        Curvz::color::SwatchId new_id = cmd->m_assigned_id;
        m_history->push(std::move(cmd));

        // s221 m1 fix-1: mirror SwatchesPanel::on_ctx_duplicate_swatch —
        // add the duplicate to the active palette so the chip appears
        // in the visible grid. See the matching block in `new` above
        // for the rationale and the redo-after-undo caveat.
        const Curvz::color::PaletteId& active_pal = lib.active_palette();
        if (!active_pal.empty() && lib.find_palette(active_pal)) {
            lib.add_to_palette(active_pal, new_id);
            lib.signal_swatch_added().emit(new_id);
        }
        return ScriptValue::text(new_id);
    }

    if (verb == "find_by_name") {
        // Parameterised query workaround — until the listener grammar
        // grows query-with-args, find_by_name lives on invoke() (same
        // shape as LayersScriptable's find_by_name). Returns the id
        // of the first swatch (any tier) with header.name == arg.
        // Returns "" on miss. Names aren't unique by construction.
        //
        // Iteration order matches all_swatch_ids: defaults first, then
        // custom. A defaults swatch named "Red" will hide a custom
        // swatch also named "Red" from find_by_name. This matches the
        // panel's first-hit semantics in find_solid_by_color (which
        // walks custom first because the panel WANTS custom to win for
        // "did the user already name this colour" — but for arbitrary
        // name lookup we don't have that hint, so plain library
        // iteration order is fine).
        if (args.empty()) return ScriptValue::text("");
        std::string target = arg_as_string(args[0]);
        for (const auto& id : lib.all_swatch_ids()) {
            const Curvz::color::Swatch* s = lib.find_swatch(id);
            if (!s) continue;
            if (name_of(*s) == target) return ScriptValue::text(id);
        }
        return ScriptValue::text("");
    }

    if (verb == "rename") {
        // Collection-level rename — same as the proxy rename, just
        // accessed by "<id>" + "<name>" pair instead of dotted
        // addressing. Kept on the collection too because the handoff
        // plan lists it explicitly there, and it reads naturally for
        // scripts that have the id in hand without wanting to
        // materialise the proxy:
        //
        //   swatches rename "abc-123" "Sunset orange"
        //
        // vs.
        //
        //   set sid to "abc-123"
        //   swatches.sid rename "Sunset orange"
        //
        // Both are supported; both push the same EditSwatchCommand.
        if (args.size() < 2) return ScriptValue::null();
        std::string id   = arg_as_string(args[0]);
        std::string name = arg_as_string(args[1]);
        if (id.empty()) return ScriptValue::null();
        const Curvz::color::Swatch* live = lib.find_swatch(id);
        if (!live) return ScriptValue::null();
        if (lib.is_default_swatch(id)) return ScriptValue::null();
        std::string current = name_of(*live);
        if (name == current) return ScriptValue::null();
        Curvz::color::Color color_snapshot = solid_color_or_black(*live);

        if (!m_history) {
            lib.rename_swatch(id, name);
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::EditSwatchCommand>(
            &lib, id, color_snapshot, color_snapshot,
            current, name);
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    return ScriptValue::null();
}

// ── Collection query ───────────────────────────────────────────────────────

ScriptValue SwatchesScriptable::query(std::string_view property) const {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) {
        // No project — defensible empties (same shape as
        // LayersScriptable::query / GuidesScriptable::query: a test
        // running before project open sees 0 / "" not null).
        if (property == "count")        return ScriptValue::integer(0);
        if (property == "all_ids")      return ScriptValue::text("");
        if (property == "custom_ids")   return ScriptValue::text("");
        if (property == "default_ids")  return ScriptValue::text("");
        return ScriptValue::null();
    }
    const auto& lib = proj->swatches;

    if (property == "count") {
        return ScriptValue::integer(
            static_cast<long long>(lib.swatch_count()));
    }

    if (property == "all_ids") {
        // all_swatch_ids emits defaults first then custom, in
        // insertion order within each tier. Comma-separated for the
        // same future-foreach-grammar sentinel reason as
        // LayersScriptable::all_iids.
        std::ostringstream os;
        bool first = true;
        for (const auto& id : lib.all_swatch_ids()) {
            if (!first) os << ',';
            os << id;
            first = false;
        }
        return ScriptValue::text(os.str());
    }

    if (property == "custom_ids" || property == "default_ids") {
        // Partition all_swatch_ids by is_default_swatch. The library
        // doesn't expose per-tier iteration directly; the filter is
        // cheap and keeps the shape uniform across queries.
        const bool want_default = (property == "default_ids");
        std::ostringstream os;
        bool first = true;
        for (const auto& id : lib.all_swatch_ids()) {
            if (lib.is_default_swatch(id) != want_default) continue;
            if (!first) os << ',';
            os << id;
            first = false;
        }
        return ScriptValue::text(os.str());
    }

    // find_by_name is handled in invoke() — it's parameterised, and
    // today's query() can't take args. Listed in verbs() (not
    // properties()) so the listener routes `swatches find_by_name "X"`
    // through invoke. Anything that gets here is an unknown property.
    return ScriptValue::null();
}

std::vector<std::string> SwatchesScriptable::verbs() const {
    return {
        "new",
        "delete",
        "duplicate",
        "rename",
        // find_by_name lives here too — see the note in query() about
        // parameterised queries needing the verb form. The listener
        // accepts both `swatches find_by_name "<name>"` (which arrives
        // through invoke and produces a return value) and the future
        // `get swatches find_by_name "<name>"` form once query-args
        // grammar lands.
        "find_by_name",
    };
}

std::vector<std::string> SwatchesScriptable::properties() const {
    return {
        "count",
        "all_ids",
        "custom_ids",
        "default_ids",
    };
}

} // namespace curvz::scripting
