#include "MacroSystem.hpp"
#include "CurvzLog.hpp"
#include <glib.h>
#include <glibmm/miscutils.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace fs = std::filesystem;

namespace Curvz {

// ─────────────────────────────────────────────────────────────────────────────
// Op name tables (for JSON serialization and human labels)
// ─────────────────────────────────────────────────────────────────────────────

static const char* op_name(MacroStep::Op op) {
    switch (op) {
        case MacroStep::Op::Clone:           return "Clone";
        case MacroStep::Op::Duplicate:       return "Duplicate";
        case MacroStep::Op::Delete:          return "Delete";
        case MacroStep::Op::Group:           return "Group";
        case MacroStep::Op::Ungroup:         return "Ungroup";
        case MacroStep::Op::Move:            return "Move";
        case MacroStep::Op::Scale:           return "Scale";
        case MacroStep::Op::Rotate:          return "Rotate";
        case MacroStep::Op::FlipH:           return "FlipH";
        case MacroStep::Op::FlipV:           return "FlipV";
        case MacroStep::Op::AlignLeft:       return "AlignLeft";
        case MacroStep::Op::AlignCenterH:    return "AlignCenterH";
        case MacroStep::Op::AlignRight:      return "AlignRight";
        case MacroStep::Op::AlignTop:        return "AlignTop";
        case MacroStep::Op::AlignMiddleV:    return "AlignMiddleV";
        case MacroStep::Op::AlignBottom:     return "AlignBottom";
        case MacroStep::Op::DistributeH:     return "DistributeH";
        case MacroStep::Op::DistributeV:     return "DistributeV";
        case MacroStep::Op::SetFill:         return "SetFill";
        case MacroStep::Op::SetStroke:       return "SetStroke";
        case MacroStep::Op::SetStrokeWidth:  return "SetStrokeWidth";
        case MacroStep::Op::SetOpacity:      return "SetOpacity";
        case MacroStep::Op::BooleanUnion:    return "BooleanUnion";
        case MacroStep::Op::BooleanSubtract: return "BooleanSubtract";
        case MacroStep::Op::BooleanIntersect:return "BooleanIntersect";
        case MacroStep::Op::OffsetPath:      return "OffsetPath";
        case MacroStep::Op::ReversePath:     return "ReversePath";
        case MacroStep::Op::BringToFront:    return "BringToFront";
        case MacroStep::Op::SendToBack:      return "SendToBack";
        case MacroStep::Op::BringForward:    return "BringForward";
        case MacroStep::Op::SendBackward:    return "SendBackward";
    }
    return "Unknown";
}

static MacroStep::Op op_from_name(const std::string& s) {
    if (s == "Clone")            return MacroStep::Op::Clone;
    if (s == "Duplicate")        return MacroStep::Op::Duplicate;
    if (s == "Delete")           return MacroStep::Op::Delete;
    if (s == "Group")            return MacroStep::Op::Group;
    if (s == "Ungroup")          return MacroStep::Op::Ungroup;
    if (s == "Move")             return MacroStep::Op::Move;
    if (s == "Scale")            return MacroStep::Op::Scale;
    if (s == "Rotate")           return MacroStep::Op::Rotate;
    if (s == "FlipH")            return MacroStep::Op::FlipH;
    if (s == "FlipV")            return MacroStep::Op::FlipV;
    if (s == "AlignLeft")        return MacroStep::Op::AlignLeft;
    if (s == "AlignCenterH")     return MacroStep::Op::AlignCenterH;
    if (s == "AlignRight")       return MacroStep::Op::AlignRight;
    if (s == "AlignTop")         return MacroStep::Op::AlignTop;
    if (s == "AlignMiddleV")     return MacroStep::Op::AlignMiddleV;
    if (s == "AlignBottom")      return MacroStep::Op::AlignBottom;
    if (s == "DistributeH")      return MacroStep::Op::DistributeH;
    if (s == "DistributeV")      return MacroStep::Op::DistributeV;
    if (s == "SetFill")          return MacroStep::Op::SetFill;
    if (s == "SetStroke")        return MacroStep::Op::SetStroke;
    if (s == "SetStrokeWidth")   return MacroStep::Op::SetStrokeWidth;
    if (s == "SetOpacity")       return MacroStep::Op::SetOpacity;
    if (s == "BooleanUnion")     return MacroStep::Op::BooleanUnion;
    if (s == "BooleanSubtract")  return MacroStep::Op::BooleanSubtract;
    if (s == "BooleanIntersect") return MacroStep::Op::BooleanIntersect;
    if (s == "OffsetPath")       return MacroStep::Op::OffsetPath;
    if (s == "ReversePath")      return MacroStep::Op::ReversePath;
    if (s == "BringToFront")     return MacroStep::Op::BringToFront;
    if (s == "SendToBack")       return MacroStep::Op::SendToBack;
    if (s == "BringForward")     return MacroStep::Op::BringForward;
    if (s == "SendBackward")     return MacroStep::Op::SendBackward;
    return MacroStep::Op::Move; // safe default
}

// ─────────────────────────────────────────────────────────────────────────────
// MacroStep
// ─────────────────────────────────────────────────────────────────────────────

std::string MacroStep::auto_label() const {
    std::ostringstream ss;
    switch (op) {
        case Op::Clone:     return "Clone";
        case Op::Duplicate:
            ss << "Duplicate (" << std::fixed << std::setprecision(1)
               << dx << ", " << dy << ")";
            return ss.str();
        case Op::Delete:    return "Delete";
        case Op::Group:     return "Group";
        case Op::Ungroup:   return "Ungroup";
        case Op::Move:
            ss << "Move (" << std::fixed << std::setprecision(1)
               << dx << ", " << dy << ")";
            return ss.str();
        case Op::Scale:
            ss << "Scale (" << std::fixed << std::setprecision(2)
               << scale_x << "×, " << scale_y << "×)";
            return ss.str();
        case Op::Rotate:
            ss << "Rotate " << std::fixed << std::setprecision(1)
               << angle_deg << "°";
            return ss.str();
        case Op::FlipH:     return "Flip Horizontal";
        case Op::FlipV:     return "Flip Vertical";
        case Op::AlignLeft:     return "Align Left";
        case Op::AlignCenterH:  return "Align Center H";
        case Op::AlignRight:    return "Align Right";
        case Op::AlignTop:      return "Align Top";
        case Op::AlignMiddleV:  return "Align Middle V";
        case Op::AlignBottom:   return "Align Bottom";
        case Op::DistributeH:   return "Distribute H";
        case Op::DistributeV:   return "Distribute V";
        case Op::SetFill:
            return color_hex.empty() ? "Set Fill: currentColor"
                                     : "Set Fill: " + color_hex;
        case Op::SetStroke:
            return color_hex.empty() ? "Set Stroke: currentColor"
                                     : "Set Stroke: " + color_hex;
        case Op::SetStrokeWidth:
            ss << "Set Stroke Width: " << std::fixed << std::setprecision(1) << value;
            return ss.str();
        case Op::SetOpacity:
            ss << "Set Opacity: " << std::fixed << std::setprecision(0)
               << (value * 100.0) << "%";
            return ss.str();
        case Op::BooleanUnion:      return "Boolean Union";
        case Op::BooleanSubtract:   return "Boolean Subtract";
        case Op::BooleanIntersect:  return "Boolean Intersect";
        case Op::OffsetPath:
            ss << "Offset Path: " << std::fixed << std::setprecision(1) << value;
            return ss.str();
        case Op::ReversePath:   return "Reverse Path";
        case Op::BringToFront:  return "Bring to Front";
        case Op::SendToBack:    return "Send to Back";
        case Op::BringForward:  return "Bring Forward";
        case Op::SendBackward:  return "Send Backward";
    }
    return op_name(op);
}

nlohmann::json MacroStep::to_json() const {
    nlohmann::json j;
    j["op"]    = op_name(op);
    j["label"] = label;

    // Only emit non-default payload fields to keep JSON compact
    if (dx != 0.0)       j["dx"]       = dx;
    if (dy != 0.0)       j["dy"]       = dy;
    if (scale_x != 1.0)  j["scale_x"]  = scale_x;
    if (scale_y != 1.0)  j["scale_y"]  = scale_y;
    if (angle_deg != 0.0) j["angle_deg"] = angle_deg;
    if (pivot_is_explicit) {
        j["pivot_x"] = pivot_x;
        j["pivot_y"] = pivot_y;
        j["pivot_is_explicit"] = true;
    }
    if (!color_hex.empty()) j["color_hex"] = color_hex;
    if (value != 0.0)        j["value"]     = value;
    if (!ref_iid.empty())    j["ref_iid"]   = ref_iid;

    return j;
}

MacroStep MacroStep::from_json(const nlohmann::json& j) {
    MacroStep s;
    s.op    = op_from_name(j.value("op",    std::string("Move")));
    s.label = j.value("label", std::string(""));

    s.dx        = j.value("dx",        0.0);
    s.dy        = j.value("dy",        0.0);
    s.scale_x   = j.value("scale_x",   1.0);
    s.scale_y   = j.value("scale_y",   1.0);
    s.angle_deg = j.value("angle_deg", 0.0);
    s.pivot_x   = j.value("pivot_x",   0.0);
    s.pivot_y   = j.value("pivot_y",   0.0);
    s.pivot_is_explicit = j.value("pivot_is_explicit", false);
    s.color_hex = j.value("color_hex", std::string(""));
    s.value     = j.value("value",     0.0);
    s.ref_iid   = j.value("ref_iid",   std::string(""));

    // If label was not saved, regenerate it
    if (s.label.empty()) s.label = s.auto_label();

    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Macro
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json Macro::to_json() const {
    nlohmann::json j;
    j["internal_id"]    = internal_id;
    j["name"]           = name;
    j["folder_id"]      = folder_id;
    j["in_layer_panel"] = in_layer_panel;
    j["hotkey"]         = hotkey;

    nlohmann::json steps_arr = nlohmann::json::array();
    for (const auto& s : steps)
        steps_arr.push_back(s.to_json());
    j["steps"] = steps_arr;

    return j;
}

Macro Macro::from_json(const nlohmann::json& j) {
    Macro m;
    m.internal_id    = j.value("internal_id",    std::string(""));
    m.name           = j.value("name",           std::string("Untitled"));
    m.folder_id      = j.value("folder_id",      std::string(""));
    m.in_layer_panel = j.value("in_layer_panel", false);
    m.hotkey         = j.value("hotkey",         std::string(""));

    if (j.contains("steps") && j["steps"].is_array()) {
        for (const auto& sj : j["steps"])
            m.steps.push_back(MacroStep::from_json(sj));
    }
    return m;
}

// ─────────────────────────────────────────────────────────────────────────────
// MacroFolder
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json MacroFolder::to_json() const {
    nlohmann::json j;
    j["internal_id"] = internal_id;
    j["name"]        = name;
    j["macro_ids"]   = macro_ids;
    return j;
}

MacroFolder MacroFolder::from_json(const nlohmann::json& j) {
    MacroFolder f;
    f.internal_id = j.value("internal_id", std::string(""));
    f.name        = j.value("name",        std::string("Untitled"));
    if (j.contains("macro_ids") && j["macro_ids"].is_array()) {
        for (const auto& id : j["macro_ids"])
            f.macro_ids.push_back(id.get<std::string>());
    }
    return f;
}

// ─────────────────────────────────────────────────────────────────────────────
// MacroManager
// ─────────────────────────────────────────────────────────────────────────────

MacroManager& MacroManager::instance() {
    static MacroManager s_instance;
    return s_instance;
}

static std::string new_uuid() {
    gchar* u = g_uuid_string_random();
    std::string s(u);
    g_free(u);
    return s;
}

std::string MacroManager::macros_path() const {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string base = xdg ? xdg
                           : (std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.config");
    return base + "/curvz/macros.json";
}

void MacroManager::load() {
    std::string path = macros_path();
    std::ifstream f(path);
    if (!f) {
        LOG_INFO("MacroManager: no macros.json found at {} — starting fresh", path);
        return;
    }
    try {
        nlohmann::json j;
        f >> j;

        m_folders.clear();
        m_macros.clear();

        if (j.contains("folders") && j["folders"].is_array()) {
            for (const auto& fj : j["folders"])
                m_folders.push_back(MacroFolder::from_json(fj));
        }

        if (j.contains("macros") && j["macros"].is_array()) {
            for (const auto& mj : j["macros"]) {
                Macro m = Macro::from_json(mj);
                if (!m.internal_id.empty())
                    m_macros[m.internal_id] = std::move(m);
            }
        }

        m_current_macro_id = j.value("current_macro_id", std::string(""));

        // Validate — if saved ID no longer exists, clear it
        if (!m_current_macro_id.empty() && !find_macro(m_current_macro_id))
            m_current_macro_id = "";

        // Auto-select first macro if nothing current
        if (m_current_macro_id.empty()) {
            for (const auto& fld : m_folders) {
                if (!fld.macro_ids.empty()) {
                    m_current_macro_id = fld.macro_ids.front();
                    break;
                }
            }
        }

        LOG_INFO("MacroManager: loaded {} folders, {} macros from {}",
                 m_folders.size(), m_macros.size(), path);
    } catch (const std::exception& e) {
        LOG_WARN("MacroManager: failed to load {}: {}", path, e.what());
    }
}

void MacroManager::save() {
    std::string path = macros_path();
    fs::create_directories(fs::path(path).parent_path());

    nlohmann::json j;
    j["current_macro_id"] = m_current_macro_id;

    nlohmann::json folders_arr = nlohmann::json::array();
    for (const auto& fld : m_folders)
        folders_arr.push_back(fld.to_json());
    j["folders"] = folders_arr;

    nlohmann::json macros_arr = nlohmann::json::array();
    for (const auto& [id, mac] : m_macros)
        macros_arr.push_back(mac.to_json());
    j["macros"] = macros_arr;

    std::ofstream f(path);
    if (f) {
        f << j.dump(2) << "\n";
        LOG_INFO("MacroManager: saved {} macros to {}", m_macros.size(), path);
    } else {
        LOG_WARN("MacroManager: failed to write {}", path);
    }

    m_sig_changed.emit();
}

// ── Folder ops ────────────────────────────────────────────────────────────────

MacroFolder& MacroManager::add_folder(const std::string& name) {
    MacroFolder f;
    f.internal_id = new_uuid();
    f.name        = name;
    m_folders.push_back(std::move(f));
    save();
    return m_folders.back();
}

void MacroManager::delete_folder(const std::string& folder_id) {
    auto it = std::find_if(m_folders.begin(), m_folders.end(),
        [&](const MacroFolder& f){ return f.internal_id == folder_id; });
    if (it == m_folders.end()) return;

    // Delete all member macros
    for (const auto& mid : it->macro_ids)
        m_macros.erase(mid);

    m_folders.erase(it);
    save();
}

void MacroManager::rename_folder(const std::string& folder_id, const std::string& name) {
    if (auto* f = find_folder(folder_id)) {
        f->name = name;
        save();
    }
}

MacroFolder* MacroManager::find_folder(const std::string& folder_id) {
    for (auto& f : m_folders)
        if (f.internal_id == folder_id) return &f;
    return nullptr;
}

// ── Macro ops ─────────────────────────────────────────────────────────────────

Macro& MacroManager::add_macro(const std::string& folder_id, const std::string& name) {
    Macro m;
    m.internal_id = new_uuid();
    m.name        = name;
    m.folder_id   = folder_id;

    std::string id = m.internal_id;
    m_macros[id]  = std::move(m);

    if (auto* f = find_folder(folder_id))
        f->macro_ids.push_back(id);

    save();
    return m_macros.at(id);
}

void MacroManager::delete_macro(const std::string& macro_id) {
    auto mit = m_macros.find(macro_id);
    if (mit == m_macros.end()) return;

    const std::string& fid = mit->second.folder_id;
    if (auto* f = find_folder(fid)) {
        auto& ids = f->macro_ids;
        ids.erase(std::remove(ids.begin(), ids.end(), macro_id), ids.end());
    }

    if (m_current_macro_id  == macro_id) m_current_macro_id  = "";
    if (m_recording_macro_id == macro_id) { m_recording = false; m_recording_macro_id = ""; }

    m_macros.erase(mit);
    save();
}

void MacroManager::rename_macro(const std::string& macro_id, const std::string& name) {
    if (auto* m = find_macro(macro_id)) {
        m->name = name;
        save();
    }
}

void MacroManager::move_macro(const std::string& macro_id,
                               const std::string& dest_folder_id) {
    auto* m = find_macro(macro_id);
    if (!m) return;

    // Remove from current folder
    if (auto* f = find_folder(m->folder_id)) {
        auto& ids = f->macro_ids;
        ids.erase(std::remove(ids.begin(), ids.end(), macro_id), ids.end());
    }

    // Add to destination folder
    m->folder_id = dest_folder_id;
    if (auto* f = find_folder(dest_folder_id))
        f->macro_ids.push_back(macro_id);

    save();
}

Macro* MacroManager::find_macro(const std::string& macro_id) {
    auto it = m_macros.find(macro_id);
    return (it != m_macros.end()) ? &it->second : nullptr;
}

std::vector<Macro*> MacroManager::layer_panel_macros() {
    std::vector<Macro*> result;
    // Walk folders in order, then macro_ids in order, to preserve user ordering
    for (const auto& f : m_folders) {
        for (const auto& mid : f.macro_ids) {
            auto* m = find_macro(mid);
            if (m && m->in_layer_panel)
                result.push_back(m);
        }
    }
    return result;
}

// ── Current macro ─────────────────────────────────────────────────────────────

void MacroManager::set_current(const std::string& macro_id) {
    m_current_macro_id = macro_id;
    // Persist so Ctrl+M works after restart — lightweight, no signal emit
    std::string path = macros_path();
    std::ifstream fi(path);
    if (fi) {
        try {
            nlohmann::json j; fi >> j; fi.close();
            j["current_macro_id"] = m_current_macro_id;
            std::ofstream fo(path);
            if (fo) fo << j.dump(2) << "\n";
        } catch (...) {}
    }
}

Macro* MacroManager::current_macro() {
    return find_macro(m_current_macro_id);
}

// ── Recording ─────────────────────────────────────────────────────────────────

void MacroManager::start_recording(const std::string& macro_id) {
    if (!find_macro(macro_id)) return;
    m_recording          = true;
    m_recording_macro_id = macro_id;
    set_current(macro_id);

    // Clear any existing steps — recording always starts fresh
    m_macros[macro_id].steps.clear();

    LOG_INFO("MacroManager: recording started → macro {}", macro_id);
    m_sig_changed.emit();
}

void MacroManager::stop_recording() {
    if (!m_recording) return;
    m_recording          = false;
    m_recording_macro_id = "";
    LOG_INFO("MacroManager: recording stopped");
    save();
}

void MacroManager::record_step(MacroStep step) {
    if (!m_recording) return;
    auto* m = find_macro(m_recording_macro_id);
    if (!m) return;

    // Auto-label if empty
    if (step.label.empty()) step.label = step.auto_label();

    m->steps.push_back(std::move(step));
    LOG_INFO("MacroManager: recorded step '{}' (total: {})",
             m->steps.back().label, m->steps.size());
    m_sig_changed.emit();
}

} // namespace Curvz
