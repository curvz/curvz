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

    // s189 m1 overload — inspector call sites build the StringList up front
    // (often appending in a loop with simultaneous index-of-current tracking),
    // so the wrapper accepts a pre-built model directly. The TODO from s187
    // m3's "if a callsite needs a different model shape" became the first
    // such case during the inspector migration.
    DropDown(std::string_view name,
              const Glib::RefPtr<Gtk::StringList>& model);

    curvz::scripting::ScriptValue invoke_leaf(
            std::string_view verb,
            const curvz::scripting::ScriptArgs& args) override;
    curvz::scripting::ScriptValue query_leaf(
            std::string_view property) const override;
    std::vector<std::string> leaf_verbs()      const override;
    std::vector<std::string> leaf_properties() const override;

protected:
    void bind_canonical() override;

private:
    // s197 m2: the model is read on demand from Gtk::DropDown::get_model()
    // and cast to Gtk::StringList. Previously this was stored as an
    // m_model member, which went stale when call sites swapped the model
    // at runtime via set_model() (palette picker in Toolbar.cpp is the
    // canonical case). Reading via get_model() means the base widget is
    // the single source of truth; runtime swaps just work.
    const Gtk::StringList* current_model() const;
};

} // namespace curvz::widgets
