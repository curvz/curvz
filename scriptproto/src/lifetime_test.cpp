// lifetime_test.cpp ──────────────────────────────────────────────────────────
//
// C++-side test for the open question parked by sandbox v1:
//
//   "What happens when a Scriptable widget is constructed, destroyed,
//    and then another widget is constructed with the same name?"
//
// This case isn't exercisable from a .curvzs script (the sandbox has no
// way to spawn or kill widgets from inside the DSL), so it lives here
// in C++. Runs once at startup before the demo window opens, on the
// theory that "verify the invariant during boot" is exactly when this
// kind of foundation test belongs.
//
// What's proved:
//
//   1. After a wrapper is constructed, registry.find(name) returns it.
//   2. After the wrapper is destroyed, registry.find(name) returns null
//      — the dtor's unregister actually ran.
//   3. A new wrapper with the same name can be constructed and lives
//      cleanly — proving that the destroy/recreate cycle a popover or
//      a tab close/open performs in real GTK app code doesn't leave
//      the registry in a partial state.
//   4. Validation: empty name throws.
//   5. Validation: whitespace name throws.
//   6. Validation: reserved keyword throws.
//
// On failure: any of these emits to stderr and the program exits
// non-zero before the windows open. Loud rather than silent.

#include "ScriptableRegistry.hpp"
#include "widgets/ToggleButton.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace scriptproto {

namespace {

void fail(const char* what) {
    std::fprintf(stderr,
        "lifetime_test FAIL: %s\n", what);
    std::exit(1);
}

void expect_throws(const char* what, void (*fn)()) {
    bool threw = false;
    try { fn(); } catch (const std::exception&) { threw = true; }
    if (!threw) {
        std::fprintf(stderr,
            "lifetime_test FAIL: expected throw, got success: %s\n", what);
        std::exit(1);
    }
}

// Wrappers are GTK widgets, and gtkmm prefers heap-managed lifetimes
// (make_managed / unique_ptr) over stack widgets because GObject
// reference counting can clash with automatic storage destruction.
// We mirror DemoWindow's std::make_unique<...> pattern throughout.
void make_empty()         { auto p = std::make_unique<ToggleButton>("",     ""); (void)p; }
void make_whitespace()    { auto p = std::make_unique<ToggleButton>("a b",  ""); (void)p; }
void make_reserved_list() { auto p = std::make_unique<ToggleButton>("list", ""); (void)p; }
void make_reserved_quit() { auto p = std::make_unique<ToggleButton>("quit", ""); (void)p; }

} // anon

void run_lifetime_test() {
    auto& reg = ScriptableRegistry::instance();

    // ── Case 1+2+3: construct, find, destroy, find-null, reconstruct ──
    {
        // Use a name unlikely to collide with anything DemoWindow
        // creates later — the "lifetest.*" prefix is reserved here for
        // this test only. If DemoWindow ever names something with
        // this prefix, the duplicate-name throw in Registry will
        // surface that loudly.
        const std::string nm = "lifetest.tb";

        if (reg.find(nm) != nullptr)
            fail("registry not clean before test (name in use)");

        {
            auto tb = std::make_unique<ToggleButton>(nm, "");
            if (reg.find(nm) != tb.get())
                fail("find() did not return the just-constructed wrapper");
        }
        // tb out of scope here; unique_ptr destroys; dtor ran; registry
        // should be clean.

        if (reg.find(nm) != nullptr)
            fail("dtor did not unregister — find() still returns non-null");

        {
            // Reconstruct under the same name. Duplicate-name throw
            // from Registry would fire here if unregister was a no-op.
            auto tb2 = std::make_unique<ToggleButton>(nm, "");
            if (reg.find(nm) != tb2.get())
                fail("re-construction under same name produced wrong pointer");
        }

        if (reg.find(nm) != nullptr)
            fail("second dtor did not unregister");
    }

    // ── Case 4: empty name rejected at construction ───────────────────
    expect_throws("empty name should throw", make_empty);

    // ── Case 5: whitespace in name rejected ───────────────────────────
    expect_throws("whitespace in name should throw", make_whitespace);

    // ── Case 6: reserved DSL keywords rejected ────────────────────────
    expect_throws("'list' keyword should throw",   make_reserved_list);
    expect_throws("'quit' keyword should throw",   make_reserved_quit);

    std::printf("lifetime_test PASS: 6 cases\n");
}

} // namespace scriptproto
