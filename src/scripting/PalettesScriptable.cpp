// scripting/PalettesScriptable.cpp ───────────────────────────────────────────
//
// s243 m2 — implementation of the seventh row-bound model Scriptable
// (closing the s243 arc). See PalettesScriptable.hpp for the verb /
// query surface, lifetime notes, and the design rationale for why
// `activate` is deliberately not undoable (transient working state).
//
// ── Why execute-then-push ─────────────────────────────────────────────────
//
// Same shape as SwatchesScriptable's CRUD verbs (and for the same
// reason): AddPaletteCommand / RemovePaletteCommand /
// RenamePaletteCommand all carry the mutation in their execute() body.
// The closest in-tree precedents are SwatchesPanel's on_new_palette /
// on_delete_palette / on_rename_palette / on_duplicate_palette — this
// file mirrors those handlers verb-for-verb, except the
// add_to_palette / set_active_palette side effects are handled by the
// commands themselves (via set_make_active) rather than by trailing
// panel-side calls.
//
// ── Defaults-tier guard ───────────────────────────────────────────────────
//
// Every mutating CRUD verb refuses on a defaults-tier id BEFORE
// constructing any command. The library would also refuse
// (remove_palette / rename_palette return false on defaults ids), but
// checking up-front keeps the command stack clean — no zombie commands
// pushed for ops that were no-ops in the library. Same posture as
// SwatchesScriptable.
//
// `activate` is the one mutating verb that ACCEPTS defaults ids —
// activation is a per-project pointer at any palette in either tier,
// not a mutation of the palette itself. Setting a defaults palette as
// active makes that palette's swatches the panel's visible grid; the
// palette stays read-only for rename/delete.

#include "scripting/PalettesScriptable.hpp"
#include "scripting/Scriptable.hpp"

#include "CommandHistory.hpp"
#include "CurvzProject.hpp"
#include "color/Palette.hpp"
#include "color/SwatchLibrary.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace curvz::scripting {

namespace {

// ── Argument coercion helpers ─────────────────────────────────────────────
//
// Same shape as the equivalent block in SwatchesScriptable.cpp /
// LayersScriptable.cpp / GuidesScriptable.cpp. Kept duplicated rather
// than hoisted — the coercion is small, file-local, and folding it
// into a shared TU would add a header dependency for two-line helpers.

std::string arg_as_string(const ScriptValue& v) {
    if (v.kind == ValueKind::String) return v.s;
    return {};
}

} // anon namespace

// ── PaletteProxy — transient per-instance Scriptable ──────────────────────
//
// Same lifetime contract as SwatchProxy / LayerProxy / GuideProxy:
// materialised by PalettesScriptable::proxy_for(iid), destroyed when
// the listener's ResolvedObject wrapper goes out of scope at
// end-of-statement. Registered via the `unregistered` tag so proxies
// are invisible to the global registry — `list` shows `palettes`
// exactly once regardless of how many proxies are materialised across
// script runs.
class PaletteProxy : public Scriptable {
public:
    PaletteProxy(PalettesScriptable::ProjectGetter get_project,
                 Curvz::CommandHistory* history,
                 Curvz::color::PaletteId iid)
        : Scriptable(unregistered)
        , m_get_project(std::move(get_project))
        , m_history(history)
        , m_iid(std::move(iid)) {}

    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

private:
    // Resolve our iid to a live Palette through the current project's
    // library. Returns nullptr if the iid no longer addresses any
    // palette (deleted between dispatch lines). Same defensive shape
    // as SwatchProxy::resolve / LayerProxy::resolve.
    const Curvz::color::Palette* resolve() const {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return nullptr;
        return proj->swatches.find_palette(m_iid);
    }

    // Library accessor — needed for mutating verbs. Returns nullptr if
    // the project is missing.
    Curvz::color::SwatchLibrary* library() const {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        return proj ? &proj->swatches : nullptr;
    }

    PalettesScriptable::ProjectGetter m_get_project;
    Curvz::CommandHistory*            m_history;
    Curvz::color::PaletteId           m_iid;
};

// ── PaletteProxy: invoke ──────────────────────────────────────────────────

ScriptValue PaletteProxy::invoke(std::string_view verb,
                                 const ScriptArgs& args) {
    auto* lib = library();
    if (!lib) return ScriptValue::null();
    const Curvz::color::Palette* live = resolve();
    if (!live) return ScriptValue::null();

    if (verb == "rename") {
        // Mirror SwatchesPanel::on_rename_palette's shape. Snapshot
        // is taken inside the RenamePaletteCommand ctor (which reads
        // the pre-rename name from the library). Defaults-tier guard
        // applies — defaults palettes are read-only for rename.
        // Empty name is a cancel (matches panel's blank-rename
        // posture: a palette must have a name).
        //
        // Skip-no-op guard: rename to the same name doesn't push a
        // command. Matches RenamePaletteCommand's idempotent shape —
        // the command would round-trip fine, but pushing a no-op into
        // history adds noise to the undo stack.
        if (lib->is_default_palette(m_iid)) return ScriptValue::null();
        if (args.empty()) return ScriptValue::null();
        std::string new_name = arg_as_string(args[0]);
        if (new_name.empty()) return ScriptValue::null();
        if (new_name == live->name) return ScriptValue::null();

        if (!m_history) {
            // No-history fallback. rename_palette fires
            // signal_palette_changed internally (s243 m1), so the
            // panel refresh path still runs — just no undo coverage.
            lib->rename_palette(m_iid, new_name);
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::RenamePaletteCommand>(
            lib, m_iid, new_name, "Rename palette (script)");
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    if (verb == "activate") {
        // Set this palette as the library's active palette. NOT
        // undoable — set_active_palette is transient working state,
        // deliberately outside undo (same posture as the panel's
        // dropdown click). Works on either tier — activation is a
        // per-project pointer at any palette, not a palette mutation.
        //
        // No args; the iid is the proxy's iid.
        lib->set_active_palette(m_iid);
        // Re-emit signal_palette_changed so the panel refreshes.
        // set_active_palette itself doesn't fire any signal (no
        // backing field changes on the palette value type); the
        // panel listens on signal_palette_changed for "active state
        // is stale" too. Same belt-and-braces emit pattern the
        // panel handlers use after activation.
        lib->signal_palette_changed().emit(m_iid);
        return ScriptValue::null();
    }

    return ScriptValue::null();
}

// ── PaletteProxy: query ───────────────────────────────────────────────────

ScriptValue PaletteProxy::query(std::string_view property) const {
    auto* lib = library();
    if (!lib) {
        // No project — defensible empties (mirrors
        // SwatchProxy::query's degraded shape).
        if (property == "name")          return ScriptValue::text("");
        if (property == "is_default")    return ScriptValue::boolean(false);
        if (property == "is_active")     return ScriptValue::boolean(false);
        if (property == "swatch_count")  return ScriptValue::integer(0);
        if (property == "swatches")      return ScriptValue::text("");
        if (property == "iid")           return ScriptValue::text(m_iid);
        return ScriptValue::null();
    }
    const Curvz::color::Palette* live = resolve();
    if (!live) {
        // iid no longer resolves (palette deleted between lines).
        // Return defensible empties for everything except iid, which
        // round-trips the proxy's binding key regardless of liveness.
        if (property == "iid") return ScriptValue::text(m_iid);
        if (property == "name")          return ScriptValue::text("");
        if (property == "is_default")    return ScriptValue::boolean(false);
        if (property == "is_active")     return ScriptValue::boolean(false);
        if (property == "swatch_count")  return ScriptValue::integer(0);
        if (property == "swatches")      return ScriptValue::text("");
        return ScriptValue::null();
    }

    if (property == "name")        return ScriptValue::text(live->name);
    if (property == "is_default")  return ScriptValue::boolean(
        lib->is_default_palette(m_iid));
    if (property == "is_active")   return ScriptValue::boolean(
        lib->active_palette() == m_iid);
    if (property == "swatch_count") return ScriptValue::integer(
        static_cast<long long>(live->swatches.size()));
    if (property == "swatches") {
        // Comma-separated swatch ids in palette order. Same shape as
        // PalettesScriptable::all_ids — the sentinel is comma, picked
        // because SwatchId values are UUIDs without commas. Empty
        // string for an empty palette (same posture as
        // SwatchesScriptable::query's empty-tier fallbacks).
        std::ostringstream os;
        bool first = true;
        for (const auto& sid : live->swatches) {
            if (!first) os << ',';
            os << sid;
            first = false;
        }
        return ScriptValue::text(os.str());
    }
    if (property == "iid")         return ScriptValue::text(m_iid);
    return ScriptValue::null();
}

std::vector<std::string> PaletteProxy::verbs() const {
    return {"rename", "activate"};
}

std::vector<std::string> PaletteProxy::properties() const {
    return {"name", "is_default", "is_active", "swatch_count", "swatches",
            "iid"};
}

// ── PalettesScriptable ─────────────────────────────────────────────────────

PalettesScriptable::PalettesScriptable(ProjectGetter get_project,
                                       Curvz::CommandHistory* history)
    : Scriptable("palettes")
    , m_get_project(std::move(get_project))
    , m_history(history) {}

// ── Router hooks ──────────────────────────────────────────────────────────

bool PalettesScriptable::can_resolve(std::string_view key) const {
    if (key.empty()) return false;
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return false;
    // SwatchLibrary::find_palette checks BOTH tiers (custom first,
    // then defaults). Either-tier addressability is correct here —
    // a defaults palette is queryable even if it's not editable.
    return proj->swatches.find_palette(std::string(key)) != nullptr;
}

std::unique_ptr<Scriptable>
PalettesScriptable::proxy_for(std::string_view key) {
    if (!can_resolve(key)) return nullptr;
    return std::make_unique<PaletteProxy>(m_get_project, m_history,
                                          std::string(key));
}

// ── Collection invoke ─────────────────────────────────────────────────────

ScriptValue PalettesScriptable::invoke(std::string_view verb,
                                       const ScriptArgs& args) {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return ScriptValue::null();
    auto& lib = proj->swatches;

    if (verb == "new") {
        // Create a fresh custom-tier palette with the given name and
        // push AddPaletteCommand. set_make_active(true) mirrors the
        // panel's UX contract — new palettes become active so the
        // user can immediately add swatches to them.
        //
        // Empty name is a cancel (matches SwatchesPanel::on_new_palette's
        // blank-name posture: palettes need names; there's no derived
        // fallback for them the way swatches get auto-region names).
        // Returns "" without pushing.
        //
        // The library mints a fresh id inside execute(); we read it
        // back from cmd->m_assigned_id (mirrored into palette_value's
        // id by execute()) and return it. Empty return signals
        // library rejection — defensive, shouldn't happen for a
        // freshly-created palette with an empty id.
        if (args.empty()) return ScriptValue::text("");
        std::string name = arg_as_string(args[0]);
        if (name.empty()) return ScriptValue::text("");

        Curvz::color::Palette p;
        p.name   = name;
        p.source = "user";  // matches SwatchesPanel::on_new_palette

        if (!m_history) {
            // No-history fallback. add_palette fires
            // signal_palette_added internally (s243 m1), so the panel
            // refresh path still runs — just no undo coverage. Mirror
            // the panel's set_active_palette follow-up so the panel
            // shows the new palette as active.
            Curvz::color::PaletteId new_id = lib.add_palette(std::move(p));
            if (!new_id.empty()) {
                lib.set_active_palette(new_id);
            }
            return ScriptValue::text(new_id);
        }
        auto cmd = std::make_unique<Curvz::AddPaletteCommand>(
            &lib, std::move(p), "Add palette (script)");
        // s243 m1 contract: the new-palette UX makes the new palette
        // active. Declare on the command so redo restores active too
        // (otherwise undo-then-redo leaves active empty and subsequent
        // Delete Palette silently bails on its empty-active guard).
        cmd->set_make_active(true);
        cmd->execute();
        Curvz::color::PaletteId new_id = cmd->m_assigned_id;
        m_history->push(std::move(cmd));
        // execute() with m_make_active=true already called
        // set_active_palette + signal_palette_changed. No further
        // panel sync needed.
        return ScriptValue::text(new_id);
    }

    if (verb == "delete") {
        // Mirror SwatchesPanel::on_delete_palette's shape.
        // RemovePaletteCommand's ctor snapshots the palette value
        // AND the active-state pre-remove for full round-trip undo.
        //
        // Defaults-tier guard: refuse before constructing the command.
        if (args.empty()) return ScriptValue::null();
        std::string iid = arg_as_string(args[0]);
        if (iid.empty()) return ScriptValue::null();
        if (!lib.find_palette(iid)) return ScriptValue::null();  // unknown
        if (lib.is_default_palette(iid)) return ScriptValue::null();

        if (!m_history) {
            lib.remove_palette(iid);
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::RemovePaletteCommand>(
            &lib, iid, "Delete palette (script)");
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    if (verb == "duplicate") {
        // Mirror SwatchesPanel::on_duplicate_palette's shape.
        // duplicate_palette stays the single source of truth for
        // "what's in a duplicate" — source lookup, name fallback
        // (empty new_name → appends " copy" to source name),
        // cross-tier swatch refs, builtin flag clearing. We call it
        // directly, then wrap the result in
        // AddPaletteCommand::already_added so the duplicate is
        // undoable. Mirrors the panel's pattern verbatim.
        //
        // Source can be from EITHER tier — the duplicate always lands
        // in custom, which IS the panel's "duplicate to edit"
        // affordance for defaults palettes.
        //
        // Args:
        //   args[0] = source iid (REQUIRED)
        //   args[1] = new_name  (OPTIONAL; empty → library appends
        //                        " copy" to source name)
        if (args.empty()) return ScriptValue::text("");
        std::string src_iid = arg_as_string(args[0]);
        if (src_iid.empty()) return ScriptValue::text("");
        if (!lib.find_palette(src_iid)) return ScriptValue::text("");

        std::string new_name = args.size() >= 2
            ? arg_as_string(args[1])
            : std::string{};

        Curvz::color::PaletteId new_id =
            lib.duplicate_palette(src_iid, new_name);
        if (new_id.empty()) return ScriptValue::text("");

        if (!m_history) {
            // No-history fallback — still set active (matches panel
            // UX). duplicate_palette already fired
            // signal_palette_added; the activation follow-up is
            // belt-and-braces.
            lib.set_active_palette(new_id);
            return ScriptValue::text(new_id);
        }
        const Curvz::color::Palette* live = lib.find_palette(new_id);
        if (live) {
            auto cmd = Curvz::AddPaletteCommand::already_added(
                &lib, *live, "Duplicate palette (script)");
            // s243 m1 contract: duplicate UX makes new palette active.
            cmd->set_make_active(true);
            cmd->execute();   // no-op for already_added (just flips
                              // m_first_execute_consumed); subsequent
                              // redoes go through the normal add path
                              // with m_make_active honoured.
            m_history->push(std::move(cmd));
        }
        // Set active outside the command path too — already_added's
        // execute() doesn't touch active on the first-execute pass
        // (the caller is expected to have already set it live, same
        // pattern as the panel). Mirror that here.
        lib.set_active_palette(new_id);
        // Re-emit signal_palette_changed so panel rebuilds with
        // correct active state (mirrors panel handler's belt-and-
        // braces double-fire).
        lib.signal_palette_changed().emit(new_id);
        return ScriptValue::text(new_id);
    }

    if (verb == "rename") {
        // Collection-level rename — same as the proxy rename, just
        // accessed by "<iid>" + "<name>" pair instead of dotted
        // addressing. Kept on the collection too for scripts that
        // have the iid in hand without wanting to materialise the
        // proxy:
        //
        //   palettes rename "abc-123" "Warm autumn"
        //
        // vs.
        //
        //   set pid to "abc-123"
        //   palettes.pid rename "Warm autumn"
        //
        // Both push the same RenamePaletteCommand. Same shape as
        // SwatchesScriptable::rename.
        if (args.size() < 2) return ScriptValue::null();
        std::string iid  = arg_as_string(args[0]);
        std::string name = arg_as_string(args[1]);
        if (iid.empty()) return ScriptValue::null();
        if (name.empty()) return ScriptValue::null();
        const Curvz::color::Palette* live = lib.find_palette(iid);
        if (!live) return ScriptValue::null();
        if (lib.is_default_palette(iid)) return ScriptValue::null();
        if (name == live->name) return ScriptValue::null();  // skip no-op

        if (!m_history) {
            lib.rename_palette(iid, name);
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::RenamePaletteCommand>(
            &lib, iid, name, "Rename palette (script)");
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    if (verb == "activate") {
        // Set the library's active palette. NOT undoable —
        // set_active_palette is transient working state. Works on
        // ids from either tier (activation isn't a palette mutation).
        //
        // Unknown / empty id returns Null without touching active —
        // we don't want to clear active on a typo'd id, which would
        // be a destructive silent side-effect.
        if (args.empty()) return ScriptValue::null();
        std::string iid = arg_as_string(args[0]);
        if (iid.empty()) return ScriptValue::null();
        if (!lib.find_palette(iid)) return ScriptValue::null();
        lib.set_active_palette(iid);
        lib.signal_palette_changed().emit(iid);
        return ScriptValue::null();
    }

    if (verb == "find_by_name") {
        // Parameterised query workaround — until the listener grammar
        // grows query-with-args, find_by_name lives on invoke() (same
        // shape as SwatchesScriptable::find_by_name and
        // LayersScriptable::find_by_name). Returns the id of the
        // first palette (any tier) whose name exactly matches.
        // Returns "" on miss. Names aren't unique by construction.
        //
        // Iteration order matches all_palette_ids: defaults first,
        // then custom. A defaults palette named "Earth tones" will
        // hide a custom palette also named "Earth tones" from
        // find_by_name. Matches SwatchesScriptable's posture.
        if (args.empty()) return ScriptValue::text("");
        std::string target = arg_as_string(args[0]);
        for (const auto& id : lib.all_palette_ids()) {
            const Curvz::color::Palette* p = lib.find_palette(id);
            if (!p) continue;
            if (p->name == target) return ScriptValue::text(id);
        }
        return ScriptValue::text("");
    }

    return ScriptValue::null();
}

// ── Collection query ──────────────────────────────────────────────────────

ScriptValue PalettesScriptable::query(std::string_view property) const {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) {
        // No project — defensible empties (same shape as
        // SwatchesScriptable::query: a test running before project
        // open sees 0 / "" not null).
        if (property == "count")        return ScriptValue::integer(0);
        if (property == "all_ids")      return ScriptValue::text("");
        if (property == "custom_ids")   return ScriptValue::text("");
        if (property == "default_ids")  return ScriptValue::text("");
        if (property == "active_id")    return ScriptValue::text("");
        return ScriptValue::null();
    }
    const auto& lib = proj->swatches;

    if (property == "count") {
        return ScriptValue::integer(
            static_cast<long long>(lib.palette_count()));
    }

    if (property == "all_ids") {
        // all_palette_ids emits defaults first then custom, in
        // insertion order within each tier. Comma-separated for the
        // same future-foreach-grammar sentinel reason as
        // SwatchesScriptable::all_ids.
        std::ostringstream os;
        bool first = true;
        for (const auto& id : lib.all_palette_ids()) {
            if (!first) os << ',';
            os << id;
            first = false;
        }
        return ScriptValue::text(os.str());
    }

    if (property == "custom_ids" || property == "default_ids") {
        // Partition all_palette_ids by is_default_palette. Same shape
        // as SwatchesScriptable::query's tier-partitioning block.
        const bool want_default = (property == "default_ids");
        std::ostringstream os;
        bool first = true;
        for (const auto& id : lib.all_palette_ids()) {
            if (lib.is_default_palette(id) != want_default) continue;
            if (!first) os << ',';
            os << id;
            first = false;
        }
        return ScriptValue::text(os.str());
    }

    if (property == "active_id") {
        // The currently-active palette id, or "" if no active is set.
        // May reference either tier — activation is cross-tier.
        return ScriptValue::text(lib.active_palette());
    }

    // find_by_name is handled in invoke() — parameterised. Same
    // shape as SwatchesScriptable. Anything else is unknown.
    return ScriptValue::null();
}

std::vector<std::string> PalettesScriptable::verbs() const {
    return {
        "new",
        "delete",
        "duplicate",
        "rename",
        "activate",
        // find_by_name lives here too — see the note in query() about
        // parameterised queries needing the verb form. Same shape as
        // SwatchesScriptable.
        "find_by_name",
    };
}

std::vector<std::string> PalettesScriptable::properties() const {
    return {
        "count",
        "all_ids",
        "custom_ids",
        "default_ids",
        "active_id",
    };
}

} // namespace curvz::scripting
