// DropDown.hpp ───────────────────────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::DropDown (GTK4's combo replacement).
//
// Built from a StringList of options at construction.
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
#include "ScriptableWidget.hpp"
#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>
#include <vector>

namespace scriptproto {

class DropDown : public ScriptableWidget<Gtk::DropDown> {
public:
    DropDown(std::string_view name,
              const std::vector<Glib::ustring>& options);

    ScriptValue invoke(std::string_view verb,
                        const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

protected:
    void bind_canonical() override;

private:
    Glib::RefPtr<Gtk::StringList> m_model;
};

} // namespace scriptproto
