// scripting/ThemesScriptable.cpp ─────────────────────────────────────────────
//
// s223 m1 — implementation of the fifth row-bound model Scriptable,
// third library-collection variant. See ThemesScriptable.hpp for the
// verb/query surface, lifetime notes, and the "Panel visibility" block
// (third application of the canon entry — this time the answer is
// "no panel-side step needed because the panel doesn't filter").
//
// ── Why execute-then-push, not mutate-then-push ────────────────────────────
//
// LayersScriptable follows the panel's "mutate first, push command with
// before/after" shape because EditLayerFieldCommand carries only the
// diff. AddThemeCommand / UpdateThemeCommand / RemoveThemeCommand all
// carry the mutation in their own execute() body — same shape as the
// AddStyleCommand / UpdateStyleCommand / RemoveStyleCommand family
// StylesScriptable rides on, and the AddSwatchCommand / EditSwatchCommand
// / RemoveSwatchCommand family for SwatchesScriptable before it. The
// closest in-tree precedent for theme command construction is
// ThemesPanel::on_rename_theme's commit lambda (UpdateThemeCommand with
// before/after) and ThemesPanel::on_duplicate_theme (AddThemeCommand
// with the copy).
//
// ── App-tier guard ─────────────────────────────────────────────────────────
//
// Every mutating verb refuses on an app-tier id BEFORE constructing any
// command. The library would also refuse (update_theme / remove_theme
// reject built-in ids), but checking up-front keeps the command stack
// clean — no zombie commands pushed for ops that were no-ops in the
// library. v1 has no app themes so the guard is academic; the contract
// is in place for forward-compat when curated app themes land.
//
// ── all_theme_ids() — assembled, not provided ──────────────────────────────
//
// ThemeLibrary doesn't expose a single flat-id accessor (it iterates by
// category to match StyleLibrary's category-grouped API, even though
// the panel doesn't render categories today). The Scriptable assembles
// the flat list by walking app_categories() and then user_categories(),
// pulling themes per category. Same iteration order StylesScriptable
// promises (app/built-in first, then user, insertion order within each
// tier).
//
// In v1 with no app themes, the app-walk produces an empty vector;
// `app_ids` always returns "" and `all_ids` == `user_ids`. The walk
// shape is still correct for forward-compat.

#include "scripting/ThemesScriptable.hpp"
#include "scripting/Scriptable.hpp"

#include "CommandHistory.hpp"
#include "CurvzProject.hpp"
#include "theme/Theme.hpp"
#include "theme/ThemeLibrary.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace curvz::scripting {

namespace {

// ── Argument coercion helper ───────────────────────────────────────────────
//
// Same shape as the equivalent block in StylesScriptable.cpp /
// SwatchesScriptable.cpp / LayersScriptable.cpp / GuidesScriptable.cpp.
// Kept duplicated rather than hoisted because the coercion is small,
// file-local, and folding it to a shared site would add a header
// dependency for what amounts to a two-line helper.
std::string arg_as_string(const ScriptValue& v) {
    if (v.kind == ValueKind::String) return v.s;
    return {};
}

// ── Flat id iteration over a ThemeLibrary ──────────────────────────────────
//
// Walks app categories first then user categories, in the same order
// the library would render them. Order within a category matches
// app_themes_in_category / user_themes_in_category's iteration
// (which is insertion order through the per-tier vector).
//
// The TierFilter enum lets one helper serve all three queries
// (`all_ids`, `user_ids`, `app_ids`) without copy-pasting the walk.
//
// Pointers returned by *_themes_in_category are into library-owned
// storage and stay valid for the duration of this call (no mutations
// happen between the categories() and the per-category fetch).
enum class TierFilter { All, AppOnly, UserOnly };

std::vector<Curvz::theme::ThemeId>
collect_theme_ids(const Curvz::theme::ThemeLibrary& lib, TierFilter filter) {
    std::vector<Curvz::theme::ThemeId> out;
    if (filter == TierFilter::All || filter == TierFilter::AppOnly) {
        for (const auto& cat : lib.app_categories()) {
            for (const auto* t : lib.app_themes_in_category(cat)) {
                if (t) out.push_back(t->header.id);
            }
        }
    }
    if (filter == TierFilter::All || filter == TierFilter::UserOnly) {
        for (const auto& cat : lib.user_categories()) {
            for (const auto* t : lib.user_themes_in_category(cat)) {
                if (t) out.push_back(t->header.id);
            }
        }
    }
    return out;
}

// Comma-join a vector of ThemeIds. Mirrors the inline join blocks in
// SwatchesScriptable / StylesScriptable — same sentinel shape, same
// future-foreach-grammar caveat (comma-substituted into a `set X to
// result` line then re-used would break the line tokeniser; smoke
// tests use visual `get` for these values rather than `assert ==`).
std::string join_csv(const std::vector<Curvz::theme::ThemeId>& ids) {
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

// ── ThemeProxy — transient per-instance Scriptable ─────────────────────────
//
// Same lifetime contract as StyleProxy / SwatchProxy / LayerProxy /
// GuideProxy: materialised by ThemesScriptable::proxy_for(id), destroyed
// when the listener's ResolvedObject wrapper goes out of scope at
// end-of-statement. Registered via the `unregistered` tag so proxies
// are invisible to the global registry — `list` shows `themes` exactly
// once regardless of how many proxies are materialised across script
// runs.
class ThemeProxy : public Scriptable {
public:
    ThemeProxy(ThemesScriptable::ProjectGetter get_project,
               Curvz::CommandHistory* history,
               Curvz::theme::ThemeId id)
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
    // Resolve our id to a live Theme through the current project's
    // library. Returns nullptr if the id no longer addresses any theme
    // (deleted between dispatch lines). Same defensive shape as
    // StyleProxy::resolve.
    const Curvz::theme::Theme* resolve() const {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return nullptr;
        return proj->themes.find_theme(m_id);
    }

    // Library accessor — needed for mutating verbs. Returns nullptr if
    // the project is missing.
    Curvz::theme::ThemeLibrary* library() const {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        return proj ? &proj->themes : nullptr;
    }

    ThemesScriptable::ProjectGetter m_get_project;
    Curvz::CommandHistory*          m_history;   // non-owning; outlives us;
                                                 // DEREFERENCED for every
                                                 // mutating verb
    Curvz::theme::ThemeId           m_id;
};

// ── ThemeProxy: invoke ─────────────────────────────────────────────────────

ScriptValue ThemeProxy::invoke(std::string_view verb,
                               const ScriptArgs& args) {
    auto* lib = library();
    if (!lib) return ScriptValue::null();
    const Curvz::theme::Theme* live = resolve();
    if (!live) return ScriptValue::null();

    // App-tier guard. Every mutating verb refuses on a built-in theme —
    // the Scriptable provides no "edit the app theme" path; the
    // collection-level `duplicate` verb is the affordance. v1 has no
    // app themes so this branch is unreachable in practice; the guard
    // is forward-compat.
    if (Curvz::theme::is_built_in(m_id)) return ScriptValue::null();

    if (verb == "rename") {
        // Mirror ThemesPanel::on_rename_theme's commit shape (minus the
        // panel's whitespace-strip and has_user_name auto-dedupe, which
        // are UX polish for the modal prompt). Snapshot the live Theme,
        // build the after by copying and changing only header.name,
        // push UpdateThemeCommand. Skip if name didn't change (matches
        // the panel's `name == live->header.name` guard).
        //
        // Empty name is meaningful — the panel's prompt cancels-as-empty
        // (a rename to "" is treated as a cancel), but a script that
        // wants to clear a name should be able to. The Scriptable
        // accepts empty as a real value here; the panel's
        // cancel-as-empty is a UX choice for the modal, not a model
        // rule. Same call as StyleProxy::rename.
        if (args.empty()) return ScriptValue::null();
        std::string new_name = arg_as_string(args[0]);
        if (new_name == live->header.name) return ScriptValue::null();

        Curvz::theme::Theme before = *live;
        Curvz::theme::Theme after  = before;
        after.header.name = new_name;

        if (!m_history) {
            // Test-harness / no-history path: fall through to direct
            // library call so the verb still functions, just without
            // undo coverage. Mirror after's header.id back to m_id
            // (defensive — copy from before should already match).
            after.header.id = m_id;
            lib->update_theme(m_id, std::move(after));
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::UpdateThemeCommand>(
            lib, m_id, std::move(before), std::move(after),
            std::string("Rename theme (script)"));
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    if (verb == "category") {
        // Same shape as StyleProxy::category. UpdateThemeCommand with
        // before/after; only header.category differs. Empty cat is
        // meaningful — marks the theme as uncategorised. Note that
        // today's ThemesPanel doesn't render a category-grouped view,
        // so the field is metadata-only from the UI's perspective; the
        // verb is in place for parity with StylesScriptable and for
        // forward-compat with a future grouped panel.
        if (args.empty()) return ScriptValue::null();
        std::string new_cat = arg_as_string(args[0]);
        if (new_cat == live->header.category) return ScriptValue::null();

        Curvz::theme::Theme before = *live;
        Curvz::theme::Theme after  = before;
        after.header.category = new_cat;

        if (!m_history) {
            after.header.id = m_id;
            lib->update_theme(m_id, std::move(after));
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::UpdateThemeCommand>(
            lib, m_id, std::move(before), std::move(after),
            std::string("Set theme category (script)"));
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    return ScriptValue::null();
}

// ── ThemeProxy: query ──────────────────────────────────────────────────────

ScriptValue ThemeProxy::query(std::string_view property) const {
    const Curvz::theme::Theme* live = resolve();
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
        // Free function from Theme.hpp — cheap prefix check. The
        // ThemeLibrary::is_built_in() member is tighter (also checks
        // the id exists in the app list), but we already know the id
        // resolves to a live theme (resolve() returned non-null), so
        // the prefix check is sufficient. Matches the discriminant
        // every panel guard uses. v1 always false (no app themes).
        return ScriptValue::boolean(Curvz::theme::is_built_in(m_id));
    }
    return ScriptValue::null();
}

std::vector<std::string> ThemeProxy::verbs() const {
    return {
        "rename",
        "category",
    };
}

std::vector<std::string> ThemeProxy::properties() const {
    return {
        "name", "category", "is_built_in", "iid",
    };
}

// ── ThemesScriptable ───────────────────────────────────────────────────────

ThemesScriptable::ThemesScriptable(ProjectGetter get_project,
                                   Curvz::CommandHistory* history)
    : Scriptable("themes")
    , m_get_project(std::move(get_project))
    , m_history(history) {
    // Registry registration happens in the Scriptable base ctor under
    // the name "themes". MainWindow holds us as a member; the registry
    // entry lives for the window's lifetime.
}

// ── Router hooks ───────────────────────────────────────────────────────────

bool ThemesScriptable::can_resolve(std::string_view key) const {
    if (key.empty()) return false;
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return false;
    // ThemeLibrary::find_theme checks BOTH tiers (user first, then app).
    // Either-tier addressability is correct here — an app theme would
    // be queryable even if it's not editable. v1 has no app themes so
    // the user tier is the only one that ever resolves; the shape is
    // forward-compat.
    return proj->themes.find_theme(std::string(key)) != nullptr;
}

std::unique_ptr<Scriptable>
ThemesScriptable::proxy_for(std::string_view key) {
    if (!can_resolve(key)) return nullptr;
    return std::make_unique<ThemeProxy>(m_get_project, m_history,
                                        std::string(key));
}

// ── Collection invoke ──────────────────────────────────────────────────────

ScriptValue ThemesScriptable::invoke(std::string_view verb,
                                     const ScriptArgs& args) {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return ScriptValue::null();
    auto& lib = proj->themes;

    if (verb == "new") {
        // Create a fresh default-constructed Theme and push
        // AddThemeCommand. The default Theme is the "factory baseline":
        // UnitSettings{}, MotifSettings with the documented factory
        // colours (dark + light pairs), GuideSettings cyan-visible,
        // GridSettings disabled, MarginSettings disabled,
        // ThemeSnapSettings with the project defaults. Applying it to
        // a doc would reset that doc to fresh-out-of-the-box settings.
        //
        // Optional args:
        //   args[0] = name (string, empty allowed)
        //   args[1] = category (string, empty allowed)
        //
        // The library mints a fresh "thm_<uuid>" id inside execute();
        // we read it back from cmd->m_assigned_id and return it. Empty
        // return signals the library rejected the add — defensive,
        // shouldn't happen for a freshly-created theme with an empty id.
        //
        // What this verb does NOT do: capture from the active doc.
        // The panel's [+] button calls capture_theme_from_doc; the
        // scripted equivalent would be a separate `capture` verb,
        // deferred to its own milestone (the doc-targeting model needs
        // its own design call). Scripts that want a doc-flavoured theme
        // today can create an empty one then use the (future) per-field
        // setter verbs to populate it; until those land, the bare
        // factory baseline is what `new` produces.
        Curvz::theme::Theme t;
        t.header.id.clear();  // library mints
        t.header.name     = args.size() >= 1 ? arg_as_string(args[0])
                                             : std::string{};
        t.header.category = args.size() >= 2 ? arg_as_string(args[1])
                                             : std::string{};
        // t.units / t.motif / t.guides / t.grid / t.margins / t.snap
        // take their struct defaults — exactly what an "empty" theme
        // should look like (matching the seed an empty AddThemeCommand
        // would produce in any future panel-driven "new empty theme"
        // affordance, mirroring StylesPanel::action_create_empty).

        if (!m_history) {
            // No-history fallback. add_theme fires signal_theme_added
            // internally, so the panel refresh path still runs — just
            // no undo coverage.
            return ScriptValue::text(lib.add_theme(std::move(t)));
        }
        auto cmd = std::make_unique<Curvz::AddThemeCommand>(
            &lib, std::move(t), std::string("Add theme (script)"));
        cmd->execute();
        Curvz::theme::ThemeId new_id = cmd->m_assigned_id;
        m_history->push(std::move(cmd));

        // No panel-side navigation step. ThemesPanel renders the full
        // user-tier list as a flat vertical box and rebuilds on
        // signal_theme_added — the new row appears automatically.
        // Third application of the "library-collection Scriptables
        // drive panel visibility through the panel's mechanism" canon
        // entry; this time the panel's mechanism IS just-rebuild-the-
        // flat-list, and the library signal already drives it. See
        // ThemesScriptable.hpp's "Panel visibility" block for the full
        // comparison with the swatches (library-side fix) and styles
        // (panel-side fix) shapes.
        return ScriptValue::text(new_id);
    }

    if (verb == "delete") {
        // Mirror ThemesPanel::on_delete_theme's command-construction
        // shape (minus the AlertDialog confirmation, which is panel UX).
        // Read the live Theme first to take the snapshot — like
        // RemoveStyleCommand, RemoveThemeCommand's ctor takes the full
        // Theme value so undo can re-add the whole record.
        //
        // App-tier guard: refuse before constructing the command.
        if (args.empty()) return ScriptValue::null();
        std::string id = arg_as_string(args[0]);
        if (id.empty()) return ScriptValue::null();
        if (Curvz::theme::is_built_in(id)) return ScriptValue::null();
        const Curvz::theme::Theme* live = lib.find_theme(id);
        if (!live) return ScriptValue::null();  // unknown id

        Curvz::theme::Theme snapshot = *live;  // full pre-remove value

        if (!m_history) {
            lib.remove_theme(id);
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::RemoveThemeCommand>(
            &lib, std::move(snapshot), std::string("Delete theme (script)"));
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    if (verb == "duplicate") {
        // Mirror ThemesPanel::on_duplicate_theme's command-construction
        // shape (minus the panel's auto-dedupe walk for name
        // collisions, which is UX polish — has_user_name returns true
        // and the panel walks " 2"/" 3"; the Scriptable trusts the
        // script to handle uniqueness if it cares). Deep-copy the
        // source Theme, clear its id (library mints a fresh one on
        // add), append " copy" to the name (empty name stays empty,
        // matching the StylesScriptable shape rather than the panel's
        // "Theme copy" fallback — the panel's fallback is for the
        // visible-row case, where an unnamed row would be invisible).
        // Source can be from EITHER tier; the duplicate always lands
        // in user, which IS the affordance for app themes when they
        // exist.
        //
        // Category is preserved verbatim. v1 has no app themes so all
        // duplicates are user-to-user.
        if (args.empty()) return ScriptValue::null();
        std::string src_id = arg_as_string(args[0]);
        if (src_id.empty()) return ScriptValue::null();
        const Curvz::theme::Theme* src = lib.find_theme(src_id);
        if (!src) return ScriptValue::null();

        Curvz::theme::Theme copy = *src;
        copy.header.id.clear();
        if (!copy.header.name.empty()) copy.header.name += " copy";

        if (!m_history) {
            return ScriptValue::text(lib.add_theme(std::move(copy)));
        }
        auto cmd = std::make_unique<Curvz::AddThemeCommand>(
            &lib, std::move(copy), std::string("Duplicate theme (script)"));
        cmd->execute();
        Curvz::theme::ThemeId new_id = cmd->m_assigned_id;
        m_history->push(std::move(cmd));

        // No panel-side navigation step — same rationale as `new`
        // above. The new row appears via signal_theme_added →
        // refresh() → rebuild_library_list.
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
        //   themes rename "thm_abc" "Print Setup"
        //
        // vs.
        //
        //   set tid to "thm_abc"
        //   themes.tid rename "Print Setup"
        //
        // Both are supported; both push the same UpdateThemeCommand.
        if (args.size() < 2) return ScriptValue::null();
        std::string id   = arg_as_string(args[0]);
        std::string name = arg_as_string(args[1]);
        if (id.empty()) return ScriptValue::null();
        if (Curvz::theme::is_built_in(id)) return ScriptValue::null();
        const Curvz::theme::Theme* live = lib.find_theme(id);
        if (!live) return ScriptValue::null();
        if (live->header.name == name) return ScriptValue::null();  // no-op

        Curvz::theme::Theme before = *live;
        Curvz::theme::Theme after  = before;
        after.header.name = name;

        if (!m_history) {
            after.header.id = id;
            lib.update_theme(id, std::move(after));
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::UpdateThemeCommand>(
            &lib, id, std::move(before), std::move(after),
            std::string("Rename theme (script)"));
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    if (verb == "category") {
        // Collection-level category set — same shape as collection
        // rename, just targeting header.category. Two-arg form:
        // "<id>" "<category>". Same proxy alternative:
        //
        //   themes category "thm_abc" "Print"
        //   themes.tid category "Print"
        //
        // Both push the same UpdateThemeCommand. Empty category is
        // meaningful — marks the theme as uncategorised. As noted in
        // the header, today's ThemesPanel doesn't render a
        // category-grouped view, but Theme carries the field and the
        // verb is in place for parity / forward-compat.
        if (args.size() < 2) return ScriptValue::null();
        std::string id  = arg_as_string(args[0]);
        std::string cat = arg_as_string(args[1]);
        if (id.empty()) return ScriptValue::null();
        if (Curvz::theme::is_built_in(id)) return ScriptValue::null();
        const Curvz::theme::Theme* live = lib.find_theme(id);
        if (!live) return ScriptValue::null();
        if (live->header.category == cat) return ScriptValue::null();

        Curvz::theme::Theme before = *live;
        Curvz::theme::Theme after  = before;
        after.header.category = cat;

        if (!m_history) {
            after.header.id = id;
            lib.update_theme(id, std::move(after));
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::UpdateThemeCommand>(
            &lib, id, std::move(before), std::move(after),
            std::string("Set theme category (script)"));
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
    }

    if (verb == "find_by_name") {
        // Parameterised query workaround — until the listener grammar
        // grows query-with-args, find_by_name lives on invoke() (same
        // shape as SwatchesScriptable / StylesScriptable / LayersScriptable).
        // Returns the id of the first theme (any tier) with header.name
        // == arg. Returns "" on miss. Names aren't unique by
        // construction (the library enforces uniqueness on ids, not
        // names — has_user_name is for the panel's auto-dedupe).
        //
        // Iteration order: app tier first (in app_categories() order),
        // then user tier. v1 has no app themes so this is effectively
        // user-only. An app theme name would hide a user theme with
        // the same name from find_by_name — same first-hit semantics
        // as StylesScriptable, matches the order collect_theme_ids
        // uses for `all_ids` so a script can reason about which one
        // wins.
        if (args.empty()) return ScriptValue::text("");
        std::string target = arg_as_string(args[0]);
        for (const auto& id : collect_theme_ids(lib, TierFilter::All)) {
            const Curvz::theme::Theme* t = lib.find_theme(id);
            if (!t) continue;
            if (t->header.name == target) return ScriptValue::text(id);
        }
        return ScriptValue::text("");
    }

    return ScriptValue::null();
}

// ── Collection query ───────────────────────────────────────────────────────

ScriptValue ThemesScriptable::query(std::string_view property) const {
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
    const auto& lib = proj->themes;

    if (property == "count") {
        return ScriptValue::integer(
            static_cast<long long>(lib.app_theme_count() +
                                   lib.user_theme_count()));
    }

    if (property == "all_ids") {
        return ScriptValue::text(
            join_csv(collect_theme_ids(lib, TierFilter::All)));
    }

    if (property == "user_ids") {
        return ScriptValue::text(
            join_csv(collect_theme_ids(lib, TierFilter::UserOnly)));
    }

    if (property == "app_ids") {
        return ScriptValue::text(
            join_csv(collect_theme_ids(lib, TierFilter::AppOnly)));
    }

    // find_by_name is handled in invoke() — it's parameterised, and
    // today's query() can't take args. Listed in verbs() (not
    // properties()) so the listener routes `themes find_by_name "X"`
    // through invoke. Anything that gets here is an unknown property.
    return ScriptValue::null();
}

std::vector<std::string> ThemesScriptable::verbs() const {
    return {
        "new",
        "delete",
        "duplicate",
        "rename",
        "category",
        // find_by_name lives here too — see the note in query() about
        // parameterised queries needing the verb form. The listener
        // accepts both `themes find_by_name "<name>"` (which arrives
        // through invoke and produces a return value) and the future
        // `get themes find_by_name "<name>"` form once query-args
        // grammar lands.
        "find_by_name",
    };
}

std::vector<std::string> ThemesScriptable::properties() const {
    return {
        "count",
        "all_ids",
        "user_ids",
        "app_ids",
    };
}

} // namespace curvz::scripting
