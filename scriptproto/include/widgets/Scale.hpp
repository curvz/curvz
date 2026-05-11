// Scale.hpp ──────────────────────────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::Scale (horizontal slider).
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
#include "ScriptableWidget.hpp"
#include <gtkmm/scale.h>

namespace scriptproto {

class Scale : public ScriptableWidget<Gtk::Scale> {
public:
    Scale(std::string_view name,
          double min, double max, double step);

    ScriptValue invoke(std::string_view verb,
                        const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

protected:
    void bind_canonical() override;
};

} // namespace scriptproto
