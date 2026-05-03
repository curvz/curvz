#include "CommandHistory.hpp"
#include "CurvzLog.hpp"
#include <algorithm>

namespace Curvz {

void CommandHistory::push(std::unique_ptr<CurvzCommand> cmd) {
    m_redo_stack.clear();
    m_undo_stack.push_back(std::move(cmd));
    if ((int)m_undo_stack.size() > MAX_HISTORY)
        m_undo_stack.pop_front();
    LOG_DEBUG("History push — undo depth={}", m_undo_stack.size());
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
