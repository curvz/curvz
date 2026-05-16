// scripting/ThemesScriptable.cpp ─────────────────────────────────────────────
//
// s223 m1 — implementation of the fifth row-bound model Scriptable,
// third library-collection variant. See ThemesScriptable.hpp for the
// verb/query surface, lifetime notes, and the "Panel visibility" block
// (third application of the canon entry — this time the answer is
// "no panel-side step needed because the panel doesn't filter").
//
// s226 m1 — per-field setter surface added. 35 proxy verbs covering all
// six sub-bundles, paired 1:1 with 35 reads. Same execute-then-push
// shape as s225 m1 styles, riding on the push_field_edit template
// helper lifted in shape from StyleProxy. See "Appearance fields —
// added in s226 m1" block in the header for the full design rationale
// (hex composite shape for colours, lowercase enum vocabulary for unit,
// snap as bool toggles).
//
// s227 m1 — apply / capture verbs added. Two verbs that bridge the
// library and the active document, closing the last two
// ThemesScriptable backlog items. Apply is proxy-AND-collection
// (`themes.<id> apply` and `themes apply "<id>"` — same shape as
// rename/category, both shapes route through one helper). Capture is
// collection-only — it CREATES a new theme so there's no proxy id to
// address (`themes capture` / `themes capture "<name>"` /
// `themes capture "<name>" "<category>"`). See "Apply and capture —
// added in s227 m1" block in the header for the full design rationale.
//
// Both verbs need active_doc; both refuse silently when there's no
// active doc (mirror of the panel's "no active doc" path in
// on_save_current_as_theme). Apply additionally syncs m_project->snap
// back from the active doc post-apply (panel does the same in
// on_apply_clicked's do_apply lambda) and fires the new
// ThemeLibrary::signal_theme_applied signal so MainWindow's
// canvas-refresh cascade runs.
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
#include "UnitSystem.hpp"          // s226 m1 — Unit enum + parse_unit / label
#include "color/Color.hpp"         // s226 m1 — from_hex / to_hex for sub-bundle colours
#include "theme/Theme.hpp"
#include "theme/ThemeLibrary.hpp"

#include <array>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace curvz::scripting {

namespace {

// ── Argument coercion helpers ──────────────────────────────────────────────
//
// Same shape as the equivalent block in StylesScriptable.cpp /
// SwatchesScriptable.cpp / LayersScriptable.cpp / GuidesScriptable.cpp.
// Kept duplicated rather than hoisted because the coercion is small,
// file-local, and folding it to a shared site would add a header
// dependency for what amounts to a two-line helper.
//
// s226 m1: extended with arg_as_bool / arg_as_double / arg_as_int
// (lifted in shape from StylesScriptable s225 m1 and StylesPanelScriptable)
// for the per-field setter surface. Same fallback contract as the
// style/layer/guide helpers — accept the native ScriptValue kind, fall
// back to the supplied default for anything else (no exceptions, no
// logging; mismatched arg = silent no-op from the caller's perspective).
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

// arg_as_int — int / long-long coercion. Accepts native Int, and
// defensively Double (rounds toward zero via truncation). Same shape
// as StylesPanelScriptable's arg_as_int. Used by margin_columns /
// margin_rows where the model field is int (not double).
long long arg_as_int(const ScriptValue& v, long long fallback) {
    switch (v.kind) {
        case ValueKind::Int:    return v.i;
        case ValueKind::Double: return static_cast<long long>(v.d);
        default:                return fallback;
    }
}

// ── Hex composite pumps for sub-bundle colours ─────────────────────────────
//
// s226 m1. Encode/decode a Color triple (rgb only) or quad (rgba) to/from
// the script's hex-string wire format. Two flavours:
//
//   * rgb_to_hex / rgb_from_hex — for MotifSettings and GuideSettings
//     fields that carry only r/g/b doubles (no alpha). The parser
//     accepts "#rrggbbaa" tolerantly but discards the alpha on write;
//     the encoder always emits "#rrggbb" (no alpha suffix) since a is
//     unconditionally 1.0 for these fields.
//
//   * rgba_to_hex / rgba_from_hex — for GridSettings and MarginSettings
//     fields that carry full r/g/b/a. Round-trips alpha. Encoder emits
//     "#rrggbb" when a == 1.0, "#rrggbbaa" otherwise (same to_hex shape
//     styles' shadow_color uses).
//
// Both pumps live next to each other in the same file (curvz utils
// rule: round-trippable encoders go in pairs). Adding a future
// sub-bundle field with a different colour shape (HSL? OKLCH?) means
// adding a new pump here, not routing around the existing ones.
//
// Why the rgb variant rather than just stuffing 1.0 into the alpha
// channel on every write: keeps reads honest. A motif colour reads
// back as "#rrggbb" not "#rrggbbff" — the alpha is conceptually
// not part of that field, and the wire format reflects that. Less
// noise in `get themes.t motif_dark_artboard` output for the operator.

std::string rgb_to_hex(double r, double g, double b) {
    // Always alpha = 1.0; to_hex omits the suffix for alpha == 1.0
    // (its lowercase "#rrggbb" form). Wraps Color's hex pump rather
    // than duplicating the channel→u8 conversion.
    return Curvz::color::to_hex(Curvz::color::Color{r, g, b, 1.0});
}

// Parse a hex spec into r/g/b doubles. Returns nullopt on malformed
// input. Discards any alpha channel the parser extracts (motif and
// guide colours have no alpha).
std::optional<std::array<double, 3>> rgb_from_hex(const std::string& spec) {
    auto parsed = Curvz::color::from_hex(spec);
    if (!parsed) return std::nullopt;
    return std::array<double, 3>{parsed->r, parsed->g, parsed->b};
}

std::string rgba_to_hex(double r, double g, double b, double a) {
    return Curvz::color::to_hex(Curvz::color::Color{r, g, b, a});
}

// Parse a hex spec into r/g/b/a doubles. Returns nullopt on malformed
// input. The parser tolerates "#rgb" / "#rrggbb" (alpha implied 1.0)
// and "#rgba" / "#rrggbbaa" (alpha as given). For the grid/margin
// reading side, the alpha returned matches what was parsed.
std::optional<std::array<double, 4>> rgba_from_hex(const std::string& spec) {
    auto parsed = Curvz::color::from_hex(spec);
    if (!parsed) return std::nullopt;
    return std::array<double, 4>{parsed->r, parsed->g, parsed->b, parsed->a};
}

// ── Unit string enum ───────────────────────────────────────────────────────
//
// s226 m1. The Unit enum (Px, In, Mm, Pt) surfaces as lowercase strings
// matching UnitSystem::label()'s output: "px" / "in" / "mm" / "pt".
//
// Encode wraps UnitSystem::label (so a future Unit::Cm gets picked up
// automatically with no edit here). Decode wraps UnitSystem::parse_unit
// — BUT parse_unit returns Unit::Px on unknown input, which conflicts
// with our "unknown vocab is silent no-op" contract. So decode does
// its own switch on the four known lowercase tokens and returns
// nullopt for anything else; matches the cap/join lowercase-only
// vocabulary in StylesScriptable.
//
// Why not just use parse_unit and accept its Unit::Px fallback: that
// would silently accept "" or "garbage" as "px," writing through a
// no-op-looking change. The Scriptable's contract is "unknown vocab
// doesn't write," not "unknown vocab maps to a default" — same shape
// the styles paint discriminator follows.

const char* encode_unit(Curvz::Unit u) {
    return Curvz::UnitSystem::label(u);
}

std::optional<Curvz::Unit> decode_unit(const std::string& s) {
    if (s == "px") return Curvz::Unit::Px;
    if (s == "in") return Curvz::Unit::In;
    if (s == "mm") return Curvz::Unit::Mm;
    if (s == "pt") return Curvz::Unit::Pt;
    return std::nullopt;
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

// ── apply helper (s227 m1) ─────────────────────────────────────────────────
//
// Shared body for the proxy form (`themes.<id> apply`) and the
// collection form (`themes apply "<id>"`). Both call this with the
// resolved theme id; the body handles everything else:
//
//   1. Resolve the theme by id (caller has the id but not the value;
//      this is the seam that lets the caller skip a duplicate find_theme
//      call).
//   2. Resolve the active doc.
//   3. Refuse if either is missing (returns false; caller returns Null).
//   4. Snapshot the theme value before the funnel call so a concurrent
//      library mutation can't pull the value out from under us mid-
//      apply (defensive; same pattern as the panel's src_snapshot in
//      on_apply_clicked).
//   5. Call apply_theme_to_doc.
//   6. Mirror active_doc->snap back into project->snap (the funnel
//      doesn't have a project pointer; the panel does this manually
//      in its do_apply lambda for the same reason).
//   7. Fire ThemeLibrary::signal_theme_applied with the theme id.
//      MainWindow's zone wiring connects this to the canvas-refresh
//      cascade. Library-internal listeners (the panel's library-list
//      refresh path) won't fire on signal_theme_applied — the library
//      ITSELF didn't mutate; only the doc did.
//
// Returns true iff the apply ran. False on missing-precondition or
// theme-not-found. Callers translate true → ScriptValue::null() and
// false → ScriptValue::null() too (same shape — Null is the universal
// "no value to return" for verb invocations whose meaningful side
// effect is elsewhere); the boolean is for internal flow, not the
// caller's return value.
//
// No app-tier guard anywhere on the apply path — neither here nor at
// the caller. Apply is a non-mutating READ of the theme record; the
// tier guard that protects mutating verbs (rename / category / delete /
// sub-bundle setters) doesn't apply. Forward-compat for when curated
// app themes ship: the panel's apply button doesn't tier-check the
// source, so neither does the script form. v1 has no app themes so
// the find_theme call returns nullptr for any "app:" id passed in
// (no library entry), and the helper exits with false.
bool apply_theme_to_active_doc(Curvz::CurvzProject* proj,
                               const Curvz::theme::ThemeId& id) {
    if (!proj) return false;
    auto* doc = proj->active_doc();
    if (!doc) return false;
    const Curvz::theme::Theme* live = proj->themes.find_theme(id);
    if (!live) return false;

    // Defensive snapshot — panel does the same in on_apply_clicked.
    // Cheap (Theme is value-typed with sub-bundles of POD-ish fields)
    // and protects against mid-flight library mutations even though
    // we're single-threaded.
    Curvz::theme::Theme snapshot = *live;

    // s183 m5a: m_project->motif is cosmetic for this call (funnel
    // writes both dark and light pairs regardless). Pass it for API
    // stability — when current_motif eventually gets dropped from the
    // funnel signature, this site changes alongside the panel's site.
    Curvz::theme::apply_theme_to_doc(snapshot, *doc, proj->motif);

    // Sync the project's snap mirror from the (now-updated) active
    // doc. The Toolbar's snap popover is the canonical writer for
    // m_project->snap, so any doc-side write that didn't go through
    // the toolbar leaves the project mirror stale. The panel does
    // exactly this in its do_apply lambda; we do the same here. No
    // active-was-target check needed — we always apply to active.
    proj->snap = doc->snap;

    // Fire the s227 m1 signal. MainWindow's zone wiring picks this
    // up and runs the same canvas-refresh cascade the panel's
    // on_apply_clicked uses (via m_themes.set_on_changed). The
    // library itself doesn't see apply at all; this signal is the
    // out-of-band hook for non-library listeners.
    proj->themes.signal_theme_applied().emit(id);
    return true;
}

// (Helper kept const-friendly — find_theme returns const* and the
// snapshot copy is enough for the funnel. If a future apply mutates
// the source theme through the library, this site grows a non-const
// alternative path.)

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

    // s226 m1 — common shape for every sub-bundle-field setter. Takes
    // a mutator lambda that writes the new value into the `after`
    // Theme; builds the before/after pair, applies the m_history-or-
    // direct dispatch, pushes UpdateThemeCommand with the supplied
    // description. Caller is responsible for the no-op skip BEFORE
    // calling this (since the skip predicate is field-specific and
    // the lambda is post-decision).
    //
    // Returns Null on success, Null on failure — same shape as the
    // existing rename/category proxy verbs (their return value isn't
    // meaningful; the result slot from the listener doesn't capture
    // void). Identical shape to StyleProxy::push_field_edit s225 m1,
    // just on Theme instead of Style.
    template <typename Mutator>
    ScriptValue push_field_edit(Curvz::theme::ThemeLibrary* lib,
                                const Curvz::theme::Theme& live,
                                std::string desc,
                                Mutator&& mutate) {
        Curvz::theme::Theme before = live;
        Curvz::theme::Theme after  = before;
        mutate(after);

        if (!m_history) {
            after.header.id = m_id;
            lib->update_theme(m_id, std::move(after));
            return ScriptValue::null();
        }
        auto cmd = std::make_unique<Curvz::UpdateThemeCommand>(
            lib, m_id, std::move(before), std::move(after), std::move(desc));
        cmd->execute();
        m_history->push(std::move(cmd));
        return ScriptValue::null();
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

    // ── s227 m1: apply (BEFORE the app-tier guard) ─────────────────────────
    //
    // Apply this theme to the project's active doc. Placed BEFORE the
    // app-tier guard below because apply is a non-mutating READ of the
    // theme — it doesn't edit the theme record, just reads it and writes
    // the values into the doc. Forward-compat: when curated app themes
    // ship, the affordance "apply a curated theme to my doc" should work
    // by-script the same way it works in the panel (where on_apply_clicked
    // doesn't tier-check the source). v1 has no app themes so this
    // ordering distinction is academic, but the placement records the
    // intent.
    //
    // The shared helper does the work — see apply_theme_to_active_doc
    // in the anon namespace at the top of the file. Steps:
    //
    //   1. Resolve project + active doc (helper does both; returns
    //      false if either is missing).
    //   2. Snapshot the theme value, call apply_theme_to_doc, mirror
    //      doc->snap into project->snap, fire signal_theme_applied.
    //   3. Return Null. apply has no meaningful return value (no id
    //      to mint, no count to report); the visible effect is on the
    //      doc, observable via the active doc's queries through
    //      whatever doc Scriptable lands.
    //
    // NOT undoable. No command pushed. From the script's perspective the
    // result slot is Null whether the helper ran or refused (helper's
    // bool indicates internal flow only; see helper docs).
    if (verb == "apply") {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        apply_theme_to_active_doc(proj, m_id);
        return ScriptValue::null();
    }

    // App-tier guard. Every MUTATING verb refuses on a built-in theme —
    // the Scriptable provides no "edit the app theme" path; the
    // collection-level `duplicate` verb is the affordance. v1 has no
    // app themes so this branch is unreachable in practice; the guard
    // is forward-compat. Apply was handled above the guard because it's
    // a read of the theme record (s227 m1); every verb below is a
    // mutation of the library and refuses on app-tier.
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

    // ── s226 m1 sub-bundle field setters ───────────────────────────────────
    //
    // Per-field setters for unit / motif / guides / grid / margins /
    // snap. Same shape as rename / category above — full before/after
    // Theme snapshots into UpdateThemeCommand via push_field_edit.
    // The skip-no-op guard lives before the helper call so we don't
    // push commands for value-unchanged writes (matches the inspector's
    // "skip unchanged commit" predicate, S98 lesson).
    //
    // The fields are grouped by sub-bundle (unit, then motif_*, guide_*,
    // grid_*, margin_*, snap_*) to keep related setters together.
    // Helpers in the anon namespace handle hex / unit string parsing;
    // the verb bodies themselves are short.

    // ── UnitSettings ──
    if (verb == "unit") {
        // s226 m1. Lowercase enum vocabulary: "px" / "in" / "mm" / "pt".
        // Verb name drops the `display_` prefix the model field carries
        // (UnitSettings::display_unit) — same way `category` drops
        // `header.`. Unknown vocab is silent no-op.
        if (args.empty()) return ScriptValue::null();
        auto parsed = decode_unit(arg_as_string(args[0]));
        if (!parsed) return ScriptValue::null();
        if (*parsed == live->units.display_unit) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme unit (script)",
            [&](Curvz::theme::Theme& after) { after.units.display_unit = *parsed; });
    }

    // ── MotifSettings (six RGB hex verbs) ──
    //
    // Six fields × dark/light × artboard/workspace/creation = six
    // verbs. The model has no alpha channel for motif colours; hex
    // parser tolerates alpha, write discards it. Read always emits
    // the no-alpha form. See rgb_to_hex / rgb_from_hex in the anon
    // namespace.

    if (verb == "motif_dark_artboard") {
        if (args.empty()) return ScriptValue::null();
        auto parsed = rgb_from_hex(arg_as_string(args[0]));
        if (!parsed) return ScriptValue::null();
        // Equality at 8-bit hex granularity (round-trip through Color::operator==).
        Curvz::color::Color current{
            live->motif.dark_artboard_r,
            live->motif.dark_artboard_g,
            live->motif.dark_artboard_b, 1.0};
        Curvz::color::Color incoming{(*parsed)[0], (*parsed)[1], (*parsed)[2], 1.0};
        if (incoming == current) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme motif dark artboard (script)",
            [&](Curvz::theme::Theme& after) {
                after.motif.dark_artboard_r = (*parsed)[0];
                after.motif.dark_artboard_g = (*parsed)[1];
                after.motif.dark_artboard_b = (*parsed)[2];
            });
    }

    if (verb == "motif_dark_workspace") {
        if (args.empty()) return ScriptValue::null();
        auto parsed = rgb_from_hex(arg_as_string(args[0]));
        if (!parsed) return ScriptValue::null();
        Curvz::color::Color current{
            live->motif.dark_workspace_r,
            live->motif.dark_workspace_g,
            live->motif.dark_workspace_b, 1.0};
        Curvz::color::Color incoming{(*parsed)[0], (*parsed)[1], (*parsed)[2], 1.0};
        if (incoming == current) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme motif dark workspace (script)",
            [&](Curvz::theme::Theme& after) {
                after.motif.dark_workspace_r = (*parsed)[0];
                after.motif.dark_workspace_g = (*parsed)[1];
                after.motif.dark_workspace_b = (*parsed)[2];
            });
    }

    if (verb == "motif_dark_creation") {
        if (args.empty()) return ScriptValue::null();
        auto parsed = rgb_from_hex(arg_as_string(args[0]));
        if (!parsed) return ScriptValue::null();
        Curvz::color::Color current{
            live->motif.dark_creation_r,
            live->motif.dark_creation_g,
            live->motif.dark_creation_b, 1.0};
        Curvz::color::Color incoming{(*parsed)[0], (*parsed)[1], (*parsed)[2], 1.0};
        if (incoming == current) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme motif dark creation (script)",
            [&](Curvz::theme::Theme& after) {
                after.motif.dark_creation_r = (*parsed)[0];
                after.motif.dark_creation_g = (*parsed)[1];
                after.motif.dark_creation_b = (*parsed)[2];
            });
    }

    if (verb == "motif_light_artboard") {
        if (args.empty()) return ScriptValue::null();
        auto parsed = rgb_from_hex(arg_as_string(args[0]));
        if (!parsed) return ScriptValue::null();
        Curvz::color::Color current{
            live->motif.light_artboard_r,
            live->motif.light_artboard_g,
            live->motif.light_artboard_b, 1.0};
        Curvz::color::Color incoming{(*parsed)[0], (*parsed)[1], (*parsed)[2], 1.0};
        if (incoming == current) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme motif light artboard (script)",
            [&](Curvz::theme::Theme& after) {
                after.motif.light_artboard_r = (*parsed)[0];
                after.motif.light_artboard_g = (*parsed)[1];
                after.motif.light_artboard_b = (*parsed)[2];
            });
    }

    if (verb == "motif_light_workspace") {
        if (args.empty()) return ScriptValue::null();
        auto parsed = rgb_from_hex(arg_as_string(args[0]));
        if (!parsed) return ScriptValue::null();
        Curvz::color::Color current{
            live->motif.light_workspace_r,
            live->motif.light_workspace_g,
            live->motif.light_workspace_b, 1.0};
        Curvz::color::Color incoming{(*parsed)[0], (*parsed)[1], (*parsed)[2], 1.0};
        if (incoming == current) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme motif light workspace (script)",
            [&](Curvz::theme::Theme& after) {
                after.motif.light_workspace_r = (*parsed)[0];
                after.motif.light_workspace_g = (*parsed)[1];
                after.motif.light_workspace_b = (*parsed)[2];
            });
    }

    if (verb == "motif_light_creation") {
        if (args.empty()) return ScriptValue::null();
        auto parsed = rgb_from_hex(arg_as_string(args[0]));
        if (!parsed) return ScriptValue::null();
        Curvz::color::Color current{
            live->motif.light_creation_r,
            live->motif.light_creation_g,
            live->motif.light_creation_b, 1.0};
        Curvz::color::Color incoming{(*parsed)[0], (*parsed)[1], (*parsed)[2], 1.0};
        if (incoming == current) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme motif light creation (script)",
            [&](Curvz::theme::Theme& after) {
                after.motif.light_creation_r = (*parsed)[0];
                after.motif.light_creation_g = (*parsed)[1];
                after.motif.light_creation_b = (*parsed)[2];
            });
    }

    // ── GuideSettings ──
    if (verb == "guide_color") {
        // s226 m1. RGB only (no alpha on guide colour). See rgb_from_hex.
        if (args.empty()) return ScriptValue::null();
        auto parsed = rgb_from_hex(arg_as_string(args[0]));
        if (!parsed) return ScriptValue::null();
        Curvz::color::Color current{
            live->guides.color_r, live->guides.color_g, live->guides.color_b, 1.0};
        Curvz::color::Color incoming{(*parsed)[0], (*parsed)[1], (*parsed)[2], 1.0};
        if (incoming == current) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme guide color (script)",
            [&](Curvz::theme::Theme& after) {
                after.guides.color_r = (*parsed)[0];
                after.guides.color_g = (*parsed)[1];
                after.guides.color_b = (*parsed)[2];
            });
    }

    if (verb == "guide_visible") {
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->guides.visible);
        if (v == live->guides.visible) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme guide visible (script)",
            [&](Curvz::theme::Theme& after) { after.guides.visible = v; });
    }

    // ── GridSettings ──
    //
    // grid_enabled is the layer-presence gate (apply ensures / removes
    // the GridLayer). At the library-edit layer, it's just a bool on
    // the Theme — no SceneNode tree to maintain. Apply is the place
    // where the gate has side-effects on the doc.

    if (verb == "grid_enabled") {
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->grid.enabled);
        if (v == live->grid.enabled) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme grid enabled (script)",
            [&](Curvz::theme::Theme& after) { after.grid.enabled = v; });
    }

    if (verb == "grid_visible") {
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->grid.visible);
        if (v == live->grid.visible) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme grid visible (script)",
            [&](Curvz::theme::Theme& after) { after.grid.visible = v; });
    }

    if (verb == "grid_spacing_x") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->grid.spacing_x);
        if (v == live->grid.spacing_x) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme grid spacing x (script)",
            [&](Curvz::theme::Theme& after) { after.grid.spacing_x = v; });
    }

    if (verb == "grid_spacing_y") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->grid.spacing_y);
        if (v == live->grid.spacing_y) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme grid spacing y (script)",
            [&](Curvz::theme::Theme& after) { after.grid.spacing_y = v; });
    }

    if (verb == "grid_offset_x") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->grid.offset_x);
        if (v == live->grid.offset_x) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme grid offset x (script)",
            [&](Curvz::theme::Theme& after) { after.grid.offset_x = v; });
    }

    if (verb == "grid_offset_y") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->grid.offset_y);
        if (v == live->grid.offset_y) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme grid offset y (script)",
            [&](Curvz::theme::Theme& after) { after.grid.offset_y = v; });
    }

    if (verb == "grid_color") {
        // s226 m1. RGBA composite — grid colour has an alpha channel.
        // Same hex shape as styles' shadow_color.
        if (args.empty()) return ScriptValue::null();
        auto parsed = rgba_from_hex(arg_as_string(args[0]));
        if (!parsed) return ScriptValue::null();
        Curvz::color::Color current{
            live->grid.color_r, live->grid.color_g,
            live->grid.color_b, live->grid.color_a};
        Curvz::color::Color incoming{(*parsed)[0], (*parsed)[1],
                                     (*parsed)[2], (*parsed)[3]};
        if (incoming == current) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme grid color (script)",
            [&](Curvz::theme::Theme& after) {
                after.grid.color_r = (*parsed)[0];
                after.grid.color_g = (*parsed)[1];
                after.grid.color_b = (*parsed)[2];
                after.grid.color_a = (*parsed)[3];
            });
    }

    if (verb == "grid_dots") {
        // false = lines, true = dots at intersections. Bool, not a
        // string enum — the model field is bool, so the verb is bool.
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->grid.dots);
        if (v == live->grid.dots) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme grid dots (script)",
            [&](Curvz::theme::Theme& after) { after.grid.dots = v; });
    }

    // ── MarginSettings ──

    if (verb == "margin_enabled") {
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->margins.enabled);
        if (v == live->margins.enabled) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme margin enabled (script)",
            [&](Curvz::theme::Theme& after) { after.margins.enabled = v; });
    }

    if (verb == "margin_visible") {
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->margins.visible);
        if (v == live->margins.visible) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme margin visible (script)",
            [&](Curvz::theme::Theme& after) { after.margins.visible = v; });
    }

    if (verb == "margin_top") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->margins.top);
        if (v == live->margins.top) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme margin top (script)",
            [&](Curvz::theme::Theme& after) { after.margins.top = v; });
    }

    if (verb == "margin_bottom") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->margins.bottom);
        if (v == live->margins.bottom) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme margin bottom (script)",
            [&](Curvz::theme::Theme& after) { after.margins.bottom = v; });
    }

    if (verb == "margin_left") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->margins.left);
        if (v == live->margins.left) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme margin left (script)",
            [&](Curvz::theme::Theme& after) { after.margins.left = v; });
    }

    if (verb == "margin_right") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->margins.right);
        if (v == live->margins.right) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme margin right (script)",
            [&](Curvz::theme::Theme& after) { after.margins.right = v; });
    }

    if (verb == "margin_columns") {
        // Model field is int — see arg_as_int and the column/row note
        // in the header verb-table.
        if (args.empty()) return ScriptValue::null();
        int v = static_cast<int>(arg_as_int(args[0], live->margins.columns));
        if (v == live->margins.columns) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme margin columns (script)",
            [&](Curvz::theme::Theme& after) { after.margins.columns = v; });
    }

    if (verb == "margin_col_gap") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->margins.col_gap);
        if (v == live->margins.col_gap) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme margin col gap (script)",
            [&](Curvz::theme::Theme& after) { after.margins.col_gap = v; });
    }

    if (verb == "margin_rows") {
        if (args.empty()) return ScriptValue::null();
        int v = static_cast<int>(arg_as_int(args[0], live->margins.rows));
        if (v == live->margins.rows) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme margin rows (script)",
            [&](Curvz::theme::Theme& after) { after.margins.rows = v; });
    }

    if (verb == "margin_row_gap") {
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], live->margins.row_gap);
        if (v == live->margins.row_gap) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme margin row gap (script)",
            [&](Curvz::theme::Theme& after) { after.margins.row_gap = v; });
    }

    if (verb == "margin_color") {
        // RGBA composite, same shape as grid_color.
        if (args.empty()) return ScriptValue::null();
        auto parsed = rgba_from_hex(arg_as_string(args[0]));
        if (!parsed) return ScriptValue::null();
        Curvz::color::Color current{
            live->margins.color_r, live->margins.color_g,
            live->margins.color_b, live->margins.color_a};
        Curvz::color::Color incoming{(*parsed)[0], (*parsed)[1],
                                     (*parsed)[2], (*parsed)[3]};
        if (incoming == current) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme margin color (script)",
            [&](Curvz::theme::Theme& after) {
                after.margins.color_r = (*parsed)[0];
                after.margins.color_g = (*parsed)[1];
                after.margins.color_b = (*parsed)[2];
                after.margins.color_a = (*parsed)[3];
            });
    }

    // ── ThemeSnapSettings ──
    //
    // Seven bool toggles. snap_enabled is the overall gate; the six
    // per-target bools (guides/grid/margins/nodes/edges/centers)
    // control which target types snap fires against. Model field
    // names match exactly — verb name = field name (no `snap_` prefix
    // stripping like with unit's `display_`, because the field IS
    // named `snap_*` in SnapSettings).

    if (verb == "snap_enabled") {
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->snap.enabled);
        if (v == live->snap.enabled) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme snap enabled (script)",
            [&](Curvz::theme::Theme& after) { after.snap.enabled = v; });
    }

    if (verb == "snap_guides") {
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->snap.snap_guides);
        if (v == live->snap.snap_guides) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme snap guides (script)",
            [&](Curvz::theme::Theme& after) { after.snap.snap_guides = v; });
    }

    if (verb == "snap_grid") {
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->snap.snap_grid);
        if (v == live->snap.snap_grid) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme snap grid (script)",
            [&](Curvz::theme::Theme& after) { after.snap.snap_grid = v; });
    }

    if (verb == "snap_margins") {
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->snap.snap_margins);
        if (v == live->snap.snap_margins) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme snap margins (script)",
            [&](Curvz::theme::Theme& after) { after.snap.snap_margins = v; });
    }

    if (verb == "snap_nodes") {
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->snap.snap_nodes);
        if (v == live->snap.snap_nodes) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme snap nodes (script)",
            [&](Curvz::theme::Theme& after) { after.snap.snap_nodes = v; });
    }

    if (verb == "snap_edges") {
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->snap.snap_edges);
        if (v == live->snap.snap_edges) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme snap edges (script)",
            [&](Curvz::theme::Theme& after) { after.snap.snap_edges = v; });
    }

    if (verb == "snap_centers") {
        if (args.empty()) return ScriptValue::null();
        bool v = arg_as_bool(args[0], live->snap.snap_centers);
        if (v == live->snap.snap_centers) return ScriptValue::null();
        return push_field_edit(lib, *live, "Set theme snap centers (script)",
            [&](Curvz::theme::Theme& after) { after.snap.snap_centers = v; });
    }

    // (s227 m1 apply branch is at the head of this function, before the
    // app-tier guard — apply is a non-mutating read of the theme.)

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

    // ── s226 m1 sub-bundle field reads ─────────────────────────────────────
    //
    // Mirror the setter shape — every property here corresponds to a
    // verb above. Hex composites always emit lowercase; unit emits
    // lowercase; bools as bool; doubles as double; ints as int (long
    // long under the hood — ScriptValue::integer takes long long).

    // ── UnitSettings ──
    if (property == "unit") {
        return ScriptValue::text(encode_unit(live->units.display_unit));
    }

    // ── MotifSettings ──
    if (property == "motif_dark_artboard") {
        return ScriptValue::text(rgb_to_hex(
            live->motif.dark_artboard_r,
            live->motif.dark_artboard_g,
            live->motif.dark_artboard_b));
    }
    if (property == "motif_dark_workspace") {
        return ScriptValue::text(rgb_to_hex(
            live->motif.dark_workspace_r,
            live->motif.dark_workspace_g,
            live->motif.dark_workspace_b));
    }
    if (property == "motif_dark_creation") {
        return ScriptValue::text(rgb_to_hex(
            live->motif.dark_creation_r,
            live->motif.dark_creation_g,
            live->motif.dark_creation_b));
    }
    if (property == "motif_light_artboard") {
        return ScriptValue::text(rgb_to_hex(
            live->motif.light_artboard_r,
            live->motif.light_artboard_g,
            live->motif.light_artboard_b));
    }
    if (property == "motif_light_workspace") {
        return ScriptValue::text(rgb_to_hex(
            live->motif.light_workspace_r,
            live->motif.light_workspace_g,
            live->motif.light_workspace_b));
    }
    if (property == "motif_light_creation") {
        return ScriptValue::text(rgb_to_hex(
            live->motif.light_creation_r,
            live->motif.light_creation_g,
            live->motif.light_creation_b));
    }

    // ── GuideSettings ──
    if (property == "guide_color") {
        return ScriptValue::text(rgb_to_hex(
            live->guides.color_r, live->guides.color_g, live->guides.color_b));
    }
    if (property == "guide_visible") {
        return ScriptValue::boolean(live->guides.visible);
    }

    // ── GridSettings ──
    if (property == "grid_enabled") {
        return ScriptValue::boolean(live->grid.enabled);
    }
    if (property == "grid_visible") {
        return ScriptValue::boolean(live->grid.visible);
    }
    if (property == "grid_spacing_x") {
        return ScriptValue::real(live->grid.spacing_x);
    }
    if (property == "grid_spacing_y") {
        return ScriptValue::real(live->grid.spacing_y);
    }
    if (property == "grid_offset_x") {
        return ScriptValue::real(live->grid.offset_x);
    }
    if (property == "grid_offset_y") {
        return ScriptValue::real(live->grid.offset_y);
    }
    if (property == "grid_color") {
        return ScriptValue::text(rgba_to_hex(
            live->grid.color_r, live->grid.color_g,
            live->grid.color_b, live->grid.color_a));
    }
    if (property == "grid_dots") {
        return ScriptValue::boolean(live->grid.dots);
    }

    // ── MarginSettings ──
    if (property == "margin_enabled") {
        return ScriptValue::boolean(live->margins.enabled);
    }
    if (property == "margin_visible") {
        return ScriptValue::boolean(live->margins.visible);
    }
    if (property == "margin_top") {
        return ScriptValue::real(live->margins.top);
    }
    if (property == "margin_bottom") {
        return ScriptValue::real(live->margins.bottom);
    }
    if (property == "margin_left") {
        return ScriptValue::real(live->margins.left);
    }
    if (property == "margin_right") {
        return ScriptValue::real(live->margins.right);
    }
    if (property == "margin_columns") {
        return ScriptValue::integer(static_cast<long long>(live->margins.columns));
    }
    if (property == "margin_col_gap") {
        return ScriptValue::real(live->margins.col_gap);
    }
    if (property == "margin_rows") {
        return ScriptValue::integer(static_cast<long long>(live->margins.rows));
    }
    if (property == "margin_row_gap") {
        return ScriptValue::real(live->margins.row_gap);
    }
    if (property == "margin_color") {
        return ScriptValue::text(rgba_to_hex(
            live->margins.color_r, live->margins.color_g,
            live->margins.color_b, live->margins.color_a));
    }

    // ── ThemeSnapSettings ──
    if (property == "snap_enabled") {
        return ScriptValue::boolean(live->snap.enabled);
    }
    if (property == "snap_guides") {
        return ScriptValue::boolean(live->snap.snap_guides);
    }
    if (property == "snap_grid") {
        return ScriptValue::boolean(live->snap.snap_grid);
    }
    if (property == "snap_margins") {
        return ScriptValue::boolean(live->snap.snap_margins);
    }
    if (property == "snap_nodes") {
        return ScriptValue::boolean(live->snap.snap_nodes);
    }
    if (property == "snap_edges") {
        return ScriptValue::boolean(live->snap.snap_edges);
    }
    if (property == "snap_centers") {
        return ScriptValue::boolean(live->snap.snap_centers);
    }

    return ScriptValue::null();
}

std::vector<std::string> ThemeProxy::verbs() const {
    return {
        "rename",
        "category",
        // s226 m1 — per-field setters
        "unit",
        "motif_dark_artboard",
        "motif_dark_workspace",
        "motif_dark_creation",
        "motif_light_artboard",
        "motif_light_workspace",
        "motif_light_creation",
        "guide_color",
        "guide_visible",
        "grid_enabled",
        "grid_visible",
        "grid_spacing_x",
        "grid_spacing_y",
        "grid_offset_x",
        "grid_offset_y",
        "grid_color",
        "grid_dots",
        "margin_enabled",
        "margin_visible",
        "margin_top",
        "margin_bottom",
        "margin_left",
        "margin_right",
        "margin_columns",
        "margin_col_gap",
        "margin_rows",
        "margin_row_gap",
        "margin_color",
        "snap_enabled",
        "snap_guides",
        "snap_grid",
        "snap_margins",
        "snap_nodes",
        "snap_edges",
        "snap_centers",
        // s227 m1 — apply (no proxy capture form: capture is collection-
        // only since it creates a new theme, see header)
        "apply",
    };
}

std::vector<std::string> ThemeProxy::properties() const {
    return {
        "name", "category", "is_built_in", "iid",
        // s226 m1 — per-field reads (mirror the verb list)
        "unit",
        "motif_dark_artboard",
        "motif_dark_workspace",
        "motif_dark_creation",
        "motif_light_artboard",
        "motif_light_workspace",
        "motif_light_creation",
        "guide_color",
        "guide_visible",
        "grid_enabled",
        "grid_visible",
        "grid_spacing_x",
        "grid_spacing_y",
        "grid_offset_x",
        "grid_offset_y",
        "grid_color",
        "grid_dots",
        "margin_enabled",
        "margin_visible",
        "margin_top",
        "margin_bottom",
        "margin_left",
        "margin_right",
        "margin_columns",
        "margin_col_gap",
        "margin_rows",
        "margin_row_gap",
        "margin_color",
        "snap_enabled",
        "snap_guides",
        "snap_grid",
        "snap_margins",
        "snap_nodes",
        "snap_edges",
        "snap_centers",
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
        // today can create an empty one then use the s226 m1 per-field
        // setter verbs to populate it.
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
        // Mirror ThemesPanel::on_delete_theme's shape. Read the live
        // Theme first to take the snapshot — same shape as
        // RemoveStyleCommand, the snapshot is the whole Theme value
        // (not just an id) so undo can re-add the full sub-bundle
        // state.
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
        // Mirror ThemesPanel::on_duplicate_theme's shape. Deep-copy the
        // source Theme, clear its id (library mints a fresh one on
        // add), append " copy" to the name (empty name stays empty —
        // matches the panel's behaviour). Source can be from EITHER
        // tier; the duplicate always lands in user (forward-compat
        // affordance for app themes — academic in v1).
        //
        // Category is preserved verbatim. The panel's
        // on_duplicate_theme has an auto-dedupe walk that appends
        // " copy" / " copy 2" / "copy 3"... until it finds a unique
        // name — that's UX polish for the modal prompt, not a model
        // rule, so the script form just appends one " copy" and lets
        // duplicates through. The library doesn't enforce name
        // uniqueness; ids are the uniqueness key.
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

    // ── s227 m1: apply (collection form) ───────────────────────────────────
    //
    // `themes apply "<id>"` — by-id form. Same shape as collection-level
    // `rename` / `category` — both shapes (proxy and collection) exist
    // for ergonomic parity, both route through the same helper.
    //
    // No is_built_in guard. Apply is a non-mutating READ of the theme
    // record (it writes to the doc, not the library), so the
    // "refuse on app-tier" rule that protects every MUTATING verb in
    // this Scriptable doesn't apply. Forward-compat: when curated app
    // themes ship, the affordance "apply this curated theme to my doc"
    // works by-script the same way it works in the panel (where
    // on_apply_clicked doesn't tier-check the source). v1 has no app
    // themes so the practical behaviour is unchanged; the absence of
    // the guard records the design intent. Mirrors the proxy form's
    // placement above the head-of-invoke app-tier guard.
    //
    // The helper handles project/doc resolution, the funnel call,
    // the project->snap mirror, and signal_theme_applied. Returns
    // Null regardless of helper success (see proxy form's matching
    // rationale).
    if (verb == "apply") {
        if (args.empty()) return ScriptValue::null();
        std::string id = arg_as_string(args[0]);
        if (id.empty()) return ScriptValue::null();
        apply_theme_to_active_doc(proj, id);
        return ScriptValue::null();
    }

    // ── s227 m1: capture ───────────────────────────────────────────────────
    //
    // `themes capture` / `themes capture "<name>"` /
    // `themes capture "<name>" "<category>"` — capture the active doc
    // into a new user-tier theme. Collection-only (no proxy form —
    // capture creates, doesn't edit an existing theme; the proxy id
    // wouldn't be meaningful).
    //
    // Steps:
    //   1. Refuse if no project or no active doc (returns "" — same
    //      sentinel as failed `new` / failed `duplicate`).
    //   2. Capture the doc via capture_theme_from_doc. The current_motif
    //      arg is no longer load-bearing (s183 m5a); capture writes both
    //      dark and light pairs from the doc directly. We pass
    //      proj->motif for API stability.
    //   3. Resolve the name. If args[0] is provided and non-empty, use
    //      it verbatim (no auto-dedupe — the library doesn't enforce
    //      name uniqueness; the panel does that as UX polish at the
    //      modal prompt). If args[0] is missing or empty, walk
    //      `Theme N` from N=1 via has_user_name (mirror of
    //      ThemesPanel::on_save_current_as_theme's proposal walk —
    //      same default the panel offers in its name entry).
    //   4. Resolve the category. args[1] if provided; empty otherwise.
    //   5. Push AddThemeCommand. Library mints the id; we capture
    //      m_assigned_id and return it.
    //
    // Pushes a command — capture IS undoable (Ctrl+Z removes the
    // captured theme, restoring the library to its pre-capture state).
    // The active doc isn't touched in any way; this is pure read of
    // the doc + library mutation.
    //
    // Visual half: the new row appears in ThemesPanel via
    // signal_theme_added → refresh() → rebuild_library_list(). Same
    // path as `new`.
    if (verb == "capture") {
        auto* doc = proj->active_doc();
        if (!doc) return ScriptValue::text("");

        Curvz::theme::Theme captured =
            Curvz::theme::capture_theme_from_doc(*doc, proj->motif);

        // Name resolution. args[0] takes precedence; empty / missing
        // falls back to the panel's "Theme N" walk. Walk caps at
        // 10000 attempts (defensive — matches the panel's cap; in
        // practice the loop hits an unused name within a few iterations).
        std::string name = args.size() >= 1 ? arg_as_string(args[0])
                                            : std::string{};
        if (name.empty()) {
            std::string proposed = "Theme 1";
            for (int n = 1; n < 10000; ++n) {
                std::string candidate = "Theme " + std::to_string(n);
                if (!lib.has_user_name(candidate)) {
                    proposed = candidate;
                    break;
                }
            }
            name = proposed;
        }
        std::string category = args.size() >= 2 ? arg_as_string(args[1])
                                                : std::string{};

        captured.header.id.clear();  // library mints
        captured.header.name = name;
        captured.header.category = category;

        if (!m_history) {
            return ScriptValue::text(lib.add_theme(std::move(captured)));
        }
        auto cmd = std::make_unique<Curvz::AddThemeCommand>(
            &lib, std::move(captured),
            std::string("Capture theme (script)"));
        cmd->execute();
        Curvz::theme::ThemeId new_id = cmd->m_assigned_id;
        m_history->push(std::move(cmd));
        return ScriptValue::text(new_id);
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
        // s227 m1 — apply / capture. apply has a proxy form too
        // (`themes.<id> apply`); capture is collection-only (creates,
        // doesn't edit — see header).
        "apply",
        "capture",
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
