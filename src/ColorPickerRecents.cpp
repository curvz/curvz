//
// ColorPickerRecents.cpp — see header.
//

#include "ColorPickerRecents.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace Curvz {

ColorPickerRecents& ColorPickerRecents::get() {
    // Meyers singleton — thread-safe initialization since C++11, zero
    // runtime overhead after first call.
    static ColorPickerRecents instance;
    return instance;
}

ColorPickerRecents::ColorPickerRecents() {
    std::lock_guard<std::mutex> lk(m_mutex);
    load_locked();
}

std::deque<color::Color> ColorPickerRecents::snapshot() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_entries;
}

void ColorPickerRecents::add(const color::Color& c) {
    std::lock_guard<std::mutex> lk(m_mutex);

    // Dedup: if c is already present (8-bit-granular ==), erase the old
    // entry. The new one goes to the front regardless, so a re-pick of
    // an existing colour pops it back to most-recent.
    auto it = std::find(m_entries.begin(), m_entries.end(), c);
    if (it != m_entries.end()) m_entries.erase(it);

    m_entries.push_front(c);
    while (m_entries.size() > CAPACITY) m_entries.pop_back();

    save_locked();
}

void ColorPickerRecents::clear() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_entries.clear();
    save_locked();
}

// ─── persistence ──────────────────────────────────────────────────────────

std::string ColorPickerRecents::path() const {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::string base = xdg
        ? std::string(xdg)
        : (std::string(home ? home : ".") + "/.config");
    return base + "/curvz/picker_recents.json";
}

void ColorPickerRecents::load_locked() {
    if (m_loaded) return;
    m_loaded = true;

    std::ifstream f(path());
    if (!f) return;                            // no file yet = empty history
    try {
        nlohmann::json j;
        f >> j;
        if (!j.is_array()) return;             // malformed = empty history
        for (auto& el : j) {
            if (!el.is_string()) continue;
            auto c = color::from_hex(el.get<std::string>());
            if (!c) continue;
            m_entries.push_back(*c);
            if (m_entries.size() >= CAPACITY) break;
        }
    } catch (...) {
        // Any parse failure → silent empty. The user will rebuild recents
        // by using the picker; we don't want a corrupted JSON blocking
        // their workflow with error dialogs.
        m_entries.clear();
    }
}

void ColorPickerRecents::save_locked() const {
    std::string p = path();
    try {
        fs::create_directories(fs::path(p).parent_path());
    } catch (...) {
        return;                                // can't create dir → give up silently
    }
    nlohmann::json j = nlohmann::json::array();
    for (const auto& c : m_entries) {
        j.push_back(color::to_hex(c));
    }
    std::ofstream f(p);
    if (f) {
        f << j.dump(2) << "\n";
    }
}

} // namespace Curvz
