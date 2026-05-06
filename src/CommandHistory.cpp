#include "CommandHistory.hpp"
#include "AppPreferences.hpp"  // s145 m1 — undo depth is a user pref
#include "CurvzLog.hpp"
#include <algorithm>

namespace Curvz {

// s145 m1 — undo depth is now a user pref (AppPreferences::undo_history_depth),
// read on every push. The MAX_HISTORY constant in the header is retained as a
// compile-time hard ceiling and as the default if the pref ever fails to load,
// but the live trim consults the pref. Reducing the pref trims the live stack
// at the next push (does not retroactively prune); increasing the pref simply
// allows the stack to grow further from that point on.
void CommandHistory::push(std::unique_ptr<CurvzCommand> cmd) {
    m_redo_stack.clear();
    m_undo_stack.push_back(std::move(cmd));
    const int cap = AppPreferences::instance().undo_history_depth();
    while ((int)m_undo_stack.size() > cap)
        m_undo_stack.pop_front();
    LOG_DEBUG("History push — undo depth={} cap={}",
              m_undo_stack.size(), cap);
}

void CommandHistory::undo() {
    if (m_undo_stack.empty()) return;
    auto& cmd = m_undo_stack.back();
    LOG_INFO("Undo: {}", cmd->description());
    cmd->undo();
    m_redo_stack.push_back(std::move(cmd));
    m_undo_stack.pop_back();
}

void CommandHistory::redo() {
    if (m_redo_stack.empty()) return;
    auto& cmd = m_redo_stack.back();
    LOG_INFO("Redo: {}", cmd->description());
    cmd->execute();
    m_undo_stack.push_back(std::move(cmd));
    m_redo_stack.pop_back();
}

std::string CommandHistory::undo_description() const {
    return m_undo_stack.empty() ? "" : m_undo_stack.back()->description();
}

std::string CommandHistory::redo_description() const {
    return m_redo_stack.empty() ? "" : m_redo_stack.back()->description();
}

} // namespace Curvz
