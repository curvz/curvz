// scripting/proxy_helpers.hpp ─────────────────────────────────────────────────
//
// s228 m1 — shared body for sub-bundle / sub-field setters across the
// row-bound model Scriptables' per-instance proxy types.
//
// ── Why this header exists (Rule of Three) ───────────────────────────────────
//
// StylesScriptable.cpp (s225 m1) and ThemesScriptable.cpp (s226 m1)
// each carried a private `push_field_edit` template member on their
// proxy class — structurally identical modulo the model record type
// and the UpdateXxxCommand type. s227 m1's themes apply/capture
// extended the themes proxy's surface around that same helper without
// disturbing its shape; the helper sat at the head of three milestones
// in a row, all riding the same pattern.
//
// Rule of Three met. The helper lifts here.
//
// ── Why a free function, not a CRTP base ─────────────────────────────────────
//
// CRTP would put the helper on a `ProxyBase<Library, Record, Command,
// Id>` and have StyleProxy / ThemeProxy inherit from it. That works
// but inflates the proxy class hierarchy with no payoff — the helper
// has zero need for `this`-state (no member access on the proxy; the
// caller passes everything in). A free template function is the lighter
// shape: no inheritance edit at the call sites, no template parameter
// plumbing on the class headers, no need to touch Scriptable.hpp.
//
// ── Why one explicit template arg, others deduced ────────────────────────────
//
// The Command type cannot be deduced from the call's value arguments
// (it appears only inside the function body as `std::make_unique<Command>`).
// The remaining four (Library, Id, Record, Mutator) are all deducible
// from the call. So the call site reads:
//
//     push_field_edit<UpdateStyleCommand>(
//         lib, m_history, m_id, *live, "Set style fill (script)",
//         [&](Style& after) { after.fill = ...; });
//
// One explicit type tag, otherwise call-site shape unchanged from
// the s225/s226 inlined version.
//
// ── No-history path (test harness defensive) ─────────────────────────────────
//
// The proxies' m_history can be null in test-harness configurations.
// The s225/s226 inlined versions special-cased this with a direct
// `library->update_xxx(id, std::move(after))` call, bypassing the
// command machinery. This header collapses that into the same
// `cmd->execute()` path the history-bearing branch uses — the command
// is built, executed (which writes through library->update_xxx the
// same way the direct call did), and then discarded if there's no
// history to push it into.
//
// Behaviourally identical:
//   - UpdateStyleCommand::apply forces copy.header.id = style_id and
//     calls library->update_style(style_id, std::move(copy));
//   - UpdateThemeCommand::apply forces copy.header.id = theme_id and
//     calls library->update_theme(theme_id, std::move(copy));
// Both match what the inlined no-history branches did directly.
//
// One extra Record copy on the no-history path (the cmd's `apply`
// takes by const ref and copies internally). The no-history path is
// the test-harness defensive branch — never hot in production. Cost
// accepted for the unification.
//
// ── Skip-no-op contract ──────────────────────────────────────────────────────
//
// CALLERS are responsible for the no-op skip BEFORE invoking this
// helper. The skip predicate is field-specific and the mutator is
// post-decision — the helper cannot know what "no change" means for
// an arbitrary mutator. Existing call sites all guard with:
//     if (incoming == live->field) return ScriptValue::null();
// before the push_field_edit call. That contract stays.
//
// ── Return shape ─────────────────────────────────────────────────────────────
//
// Returns ScriptValue::null() unconditionally — matches the existing
// rename/category proxy verbs. The verb result slot from the listener
// doesn't capture void, so any sentinel is fine; null is the
// in-house convention.

#pragma once

#include "scripting/ScriptValue.hpp"
#include "CommandHistory.hpp"   // s228 m1 fix-1 — need complete type for
                                // history->push() in the template body
                                // (non-dependent member access; forward
                                // declaration not sufficient)

#include <memory>
#include <string>
#include <utility>

namespace curvz::scripting {

// Build a before/after pair, run the caller's mutator on `after`,
// construct an UpdateXxxCommand, execute it (which performs the
// library write), and push the command to history if history is
// non-null.
//
// See header comment for the design rationale and the
// caller-responsibility-for-no-op-skip contract.
template <typename Command,
          typename Library,
          typename Id,
          typename Record,
          typename Mutator>
ScriptValue push_field_edit(Library* lib,
                            Curvz::CommandHistory* history,
                            const Id& id,
                            const Record& live,
                            std::string desc,
                            Mutator&& mutate) {
    Record before = live;
    Record after  = before;
    std::forward<Mutator>(mutate)(after);

    auto cmd = std::make_unique<Command>(lib,
                                         id,
                                         std::move(before),
                                         std::move(after),
                                         std::move(desc));
    cmd->execute();
    if (history) history->push(std::move(cmd));
    return ScriptValue::null();
}

} // namespace curvz::scripting
