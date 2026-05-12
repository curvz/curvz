// curvz/widgets/DropDown.cpp ─────────────────────────────────────────────────

#include "curvz/widgets/DropDown.hpp"
#include <gtkmm/stringobject.h>

namespace curvz::widgets {

using namespace curvz::scripting;

DropDown::DropDown(std::string_view name,
                    const std::vector<Glib::ustring>& options)
    : ScriptableWidget<Gtk::DropDown>(name) {
    std::vector<Glib::ustring> opts = options;
    set_model(Gtk::StringList::create(opts));
    init_scriptable();
}

// s189 m1 — accept a pre-built StringList directly. Inspector call sites
// often need to walk the model in a loop while building (to find the index
// of the current value), so building the StringList in user code first and
// handing it to the wrapper is the natural shape.
DropDown::DropDown(std::string_view name,
                    const Glib::RefPtr<Gtk::StringList>& model)
    : ScriptableWidget<Gtk::DropDown>(name) {
    set_model(model);
    init_scriptable();
}

// s197 m2 — single-source-of-truth model accessor. The model lives on
// the base Gtk::DropDown; we read it back via get_model() and cast to
// StringList. Call sites that swap models at runtime (palette picker,
// inspector dynamic-list cases) used to leave a stale m_model member
// behind; reading on demand makes that class of bug go away. Returns
// nullptr if no model is set or the model isn't a StringList — callers
// must null-check.
//
// const-correctness: Gtk::DropDown::get_model() returns
// Glib::RefPtr<const Gio::ListModel>, so the cast target is
// const Gtk::StringList*. All script-side uses (get_n_items, get_string)
// are const-methods, so const is enough.
const Gtk::StringList* DropDown::current_model() const {
    auto base = get_model();
    if (!base) return nullptr;
    return dynamic_cast<const Gtk::StringList*>(base.get());
}

void DropDown::bind_canonical() {
    // GTK4: DropDown exposes signal-of-property-change for "selected".
    // We bind to it so changes from either user clicks or set_selected()
    // converge through the same handler.
    property_selected().signal_changed().connect([this]() {
        emit("selected",
             ScriptValue::integer(static_cast<long long>(get_selected())));
    });
}

ScriptValue DropDown::invoke_leaf(std::string_view verb, const ScriptArgs& args) {
    if (verb == "set") {
        if (args.size() != 1 || args[0].kind != ValueKind::Int)
            throw std::runtime_error("DropDown.set expects one int arg");
        long long idx = args[0].i;
        const auto* sl = current_model();
        if (!sl)
            throw std::runtime_error("DropDown.set: no StringList model");
        if (idx < 0 || idx >= static_cast<long long>(sl->get_n_items()))
            throw std::runtime_error("DropDown.set: index out of range");
        // set_selected() is synchronous — property_selected signal fires
        // inline on the calling thread.
        set_selected(static_cast<guint>(idx));
        return ScriptValue::null();
    }
    if (verb == "pick") {
        if (args.size() != 1 || args[0].kind != ValueKind::String)
            throw std::runtime_error("DropDown.pick expects one string arg");
        const auto* sl = current_model();
        if (!sl)
            throw std::runtime_error("DropDown.pick: no StringList model");
        for (guint i = 0; i < sl->get_n_items(); ++i) {
            if (sl->get_string(i) == args[0].s) {
                set_selected(i);
                return ScriptValue::null();
            }
        }
        throw std::runtime_error(
            "DropDown.pick: no option '" + args[0].s + "'");
    }
    throw std::runtime_error(
        "DropDown: unknown verb '" + std::string(verb) + "'");
}

ScriptValue DropDown::query_leaf(std::string_view property) const {
    if (property == "selected")
        return ScriptValue::integer(static_cast<long long>(get_selected()));
    if (property == "text") {
        const auto* sl = current_model();
        if (!sl) return ScriptValue::text("");
        guint idx = get_selected();
        if (idx >= sl->get_n_items()) return ScriptValue::text("");
        return ScriptValue::text(sl->get_string(idx).raw());
    }
    if (property == "count") {
        const auto* sl = current_model();
        if (!sl) return ScriptValue::integer(0);
        return ScriptValue::integer(static_cast<long long>(sl->get_n_items()));
    }
    return ScriptValue::null();
}

std::vector<std::string> DropDown::leaf_verbs() const {
    return { "set", "pick" };
}
std::vector<std::string> DropDown::leaf_properties() const {
    return { "selected", "text", "count" };
}

} // namespace curvz::widgets
