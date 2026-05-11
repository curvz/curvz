// curvz/widgets/Scale.hpp ────────────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::Scale (slider). Lifted from
// scriptproto/ during s187 m3.
//
// Defaults to horizontal orientation; if vertical is needed, a future
// overload can take Gtk::Orientation as a parameter.
//
// Verbs:
//   set <number>        → set_value via the Adjustment
//
// Queries:
//   value               → Double
//   min                 → Double
//   max                 → Double
//
// Emits:
//   value_changed <number>   when the underlying Adjustment changes value

#pragma once
#include "scripting/ScriptableWidget.hpp"
#include <gtkmm/scale.h>

namespace curvz::widgets {

class Scale
    : public curvz::scripting::ScriptableWidget<Gtk::Scale> {
public:
    Scale(std::string_view name,
          double min, double max, double step);

    curvz::scripting::ScriptValue invoke(
            std::string_view verb,
            const curvz::scripting::ScriptArgs& args) override;
    curvz::scripting::ScriptValue query(
            std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

protected:
    void bind_canonical() override;
};

} // namespace curvz::widgets
