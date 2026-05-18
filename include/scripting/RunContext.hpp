// scripting/RunContext.hpp ───────────────────────────────────────────────────
//
// s244 m2 — second instance of ARC m5c (security architecture, Tier 5).
//
// The structural rule: every script verb declares a bitmask of which
// callers (TestRunner / Macro / Scripter) are allowed to invoke it.
// The dispatcher checks the caller's context against the verb's mask
// on every invoke; a mismatch refuses with a structured error and
// an audit log entry. Sibling rule to "Path containment for sensitive
// verbs" (s244 m1) — path containment is the system-integrity layer,
// RunContext gating is the user-data integrity layer.
//
// See CANON.md "RunContext gates the verb surface — caller-determined
// permissions" for the design rationale, threat model, and the
// rule that "TestRunner and Scripter are distinct callers, not one
// caller with an elevation flag."
//
// ── The three contexts ────────────────────────────────────────────────
//
// Three flag bits — caller-determined at engine-instance creation,
// never flippable from script-land or from a preference:
//
//   TestRunner — scripts from the repo's `tests/scripts/` directory,
//                or a CI-configured test path. Full surface; tests
//                need sensitive verbs (save, load, snapshot) to test
//                them at all. Caller is a future CLI test runner;
//                today no caller wires this context.
//   Macro      — user's own automation, recorded or hand-edited,
//                played from the Macros panel. Narrowest surface:
//                what the user can do by clicking. Caller is the
//                future MacroSystem-as-script-engine-client (m6);
//                today MacroSystem replays its own MacroStep ops
//                without going through ScriptListener, so no caller
//                wires this context.
//   Scripter   — the in-app DSL playground, gated by CURVZ_DIAGNOSTIC.
//                Wider than Macro (debugging needs reach), narrower
//                than TestRunner (it takes user-typed input at
//                runtime, including pasted scripts the user hasn't
//                read). Today the ONLY caller is ScripterWindow's
//                listener — every script that runs in Curvz runs in
//                Scripter context.
//
// ── Bitmask semantics ─────────────────────────────────────────────────
//
// Each context is a distinct power-of-two flag. A verb's mask is the
// bitwise-OR of permitted contexts. The gating check is:
//
//   if ((caller_context & verb_mask) == 0) refuse;
//
// A caller has exactly one context bit set; a verb's mask has 1, 2,
// or 3 bits set. The default verb mask is `all_three` — every
// existing verb stays callable from every existing caller without
// touching its declaration site (backward-compatibility under the
// "default is widest reasonable" canon entry).
//
// ── Why a virtual method on Scriptable, not a central registry ────────
//
// CANON's pseudo-code shows `register_verb("save", ctx::TestRunner |
// ctx::Scripter)` as the declaration shape. That implies a central
// table that every Scriptable's verbs feed into at startup. Today's
// Scriptables don't register verbs anywhere — each `invoke()` method
// is an if/else dispatch chain on the verb string. Adding a central
// registration step would touch every Scriptable subclass.
//
// The pump-at-the-seam alternative: a virtual method
// `context_mask(verb)` on Scriptable with default returning
// `all_three`. Existing Scriptables don't override it (they remain
// untouched); new Scriptables with sensitive verbs override it to
// declare per-verb masks. The central-registration syntax becomes
// the right answer when a third consumer earns it; until then,
// virtual-method IS the registration.
//
// See Scriptable.hpp for the virtual method declaration and the
// per-Scriptable override sites for the masks they declare.

#pragma once
#include <cstdint>

namespace curvz::scripting {

// The three distinct caller contexts. Caller-determined at engine-
// instance creation; never flippable from script-land or preferences.
// Values are flag bits so a verb's mask can OR them together.
enum class RunContext : std::uint8_t {
    TestRunner = 1u << 0,
    Macro      = 1u << 1,
    Scripter   = 1u << 2,
};

// A verb's permission set — bitwise-OR of allowed RunContext values.
// Stored as a small unsigned int so the bit operations are obvious;
// no operator overloads on the enum itself (the enum has exactly
// three valid values, the mask has eight).
using RunContextMask = std::uint8_t;

namespace ctx {

// Single-context masks for the three callers (used at verb-mask
// construction sites).
inline constexpr RunContextMask TestRunner =
    static_cast<RunContextMask>(RunContext::TestRunner);
inline constexpr RunContextMask Macro =
    static_cast<RunContextMask>(RunContext::Macro);
inline constexpr RunContextMask Scripter =
    static_cast<RunContextMask>(RunContext::Scripter);

// All three callers — the default mask for every verb that hasn't
// been individually declared. Most verbs stay here forever (widget
// clicks, model queries, document mutations). Only sensitive verbs
// (path-taking, preference-mutating, process-terminating) opt down
// to a narrower mask.
inline constexpr RunContextMask all_three =
    TestRunner | Macro | Scripter;

} // namespace ctx

// True iff the caller's context is permitted by the verb's mask.
// One-line gate; lives here so every gating site uses the same
// predicate (dispatcher today, future test harnesses tomorrow).
inline constexpr bool context_allows(RunContext caller,
                                     RunContextMask verb_mask) {
    return (static_cast<RunContextMask>(caller) & verb_mask) != 0;
}

// Human-readable name for a context. Used in audit log entries and
// refusal error messages so the user can see WHICH context refused
// the verb (rather than reading a numeric flag). All-lowercase to
// match the canonical names from CANON's RunContext entry.
inline const char* context_name(RunContext c) {
    switch (c) {
        case RunContext::TestRunner: return "test_runner";
        case RunContext::Macro:      return "macro";
        case RunContext::Scripter:   return "scripter";
    }
    return "unknown";
}

} // namespace curvz::scripting
