// curvz/widgets/DropDown.hpp ─────────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::DropDown (GTK4's combo replacement).
// Lifted from scriptproto/ during s187 m3.
//
// Built from a StringList of options at construction. If a callsite
// needs a different model shape, it constructs the model first and
// passes it via a future overload (TODO when first such case arises).
//
// Verbs:
//   set <int>           → set_selected(index)
//   pick "<text>"       → find string in model, set_selected; error if absent
//
// Queries:
//   selected            → Int (zero-based index)
//   text                → String (text of selected entry)
//   count               → Int (number of entries)
//
// Emits:
//   selected <int>      on property_selected change

#pragma once
#include "scripting/ScriptableWidget.hpp"
#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>
#include <vector>

namespace curvz::widgets {

class DropDown
    : public curvz::scripting::ScriptableWidget<Gtk::DropDown> {
public:
    DropDown(std::string_view name,
              const std::vector<Glib::ustring>& options);

    curvz::scripting::ScriptValue invoke(
            std::string_view verb,
            const curvz::scripting::ScriptArgs& args) override;
    curvz::scripting::ScriptValue query(
            std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

protected:
    void bind_canonical() override;

private:
    Glib::RefPtr<Gtk::StringList> m_model;
};

} // namespace curvz::widgets
