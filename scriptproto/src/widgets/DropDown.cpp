// DropDown.cpp ───────────────────────────────────────────────────────────────
//
// Construction shape: Gtk::DropDown's default ctor takes no args; the
// model is set post-construction via set_model(). The canonical signal
// is on property_selected (not signal_changed); we connect that in
// bind_canonical().

#include "widgets/DropDown.hpp"
#include <gtkmm/stringobject.h>

namespace scriptproto {

DropDown::DropDown(std::string_view name,
                    const std::vector<Glib::ustring>& options)
    : ScriptableWidget<Gtk::DropDown>(name) {
    std::vector<Glib::ustring> opts = options;
    m_model = Gtk::StringList::create(opts);
    set_model(m_model);
    init_scriptable();
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

ScriptValue DropDown::invoke(std::string_view verb, const ScriptArgs& args) {
    if (verb == "set") {
        if (args.size() != 1 || args[0].kind != ValueKind::Int)
            throw std::runtime_error("DropDown.set expects one int arg");
        long long idx = args[0].i;
        if (idx < 0 || idx >= static_cast<long long>(m_model->get_n_items()))
            throw std::runtime_error("DropDown.set: index out of range");
        set_selected(static_cast<guint>(idx));
        return ScriptValue::null();
    }
    if (verb == "pick") {
        if (args.size() != 1 || args[0].kind != ValueKind::String)
            throw std::runtime_error("DropDown.pick expects one string arg");
        for (guint i = 0; i < m_model->get_n_items(); ++i) {
            if (m_model->get_string(i) == args[0].s) {
                set_selected(i);
                return ScriptValue::null();
            }
        }
        throw std::runtime_error("DropDown.pick: no option '" + args[0].s + "'");
    }
    throw std::runtime_error("DropDown: unknown verb '" + std::string(verb) + "'");
}

ScriptValue DropDown::query(std::string_view property) const {
    if (property == "selected")
        return ScriptValue::integer(static_cast<long long>(get_selected()));
    if (property == "text") {
        guint idx = get_selected();
        if (idx >= m_model->get_n_items()) return ScriptValue::text("");
        return ScriptValue::text(m_model->get_string(idx).raw());
    }
    if (property == "count")
        return ScriptValue::integer(static_cast<long long>(m_model->get_n_items()));
    return ScriptValue::null();
}

std::vector<std::string> DropDown::verbs() const {
    return { "set", "pick" };
}
std::vector<std::string> DropDown::properties() const {
    return { "selected", "text", "count" };
}

} // namespace scriptproto
