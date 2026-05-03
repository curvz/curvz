#pragma once
//
// ColorPickerRecents — process-wide recent-colour history for CurvzColorPicker.
//
// Stored at ~/.config/curvz/picker_recents.json (or $XDG_CONFIG_HOME/curvz/…).
// Kept separate from settings.json so an empty-gallery session (no project
// loaded → settings.json isn't written) still persists the user's colour
// picking activity across restarts.
//
// Capacity: 12 slots, most-recent-first. add() dedups against existing
// entries using color::Color's 8-bit-granular equality; a duplicate is
// moved to the front rather than creating a second entry. This handles
// the "user drags the spectrum and we record on drag-end" case naturally
// — a long drag that returns near its start point leaves one slot moved,
// not twelve.
//
// Singleton access: get(). Lazy-loads the JSON on first call. Every
// successful add() writes the file. Read load is tiny (12 hex strings);
// write cost is negligible. No background thread, no debouncing.
//

#include "color/Color.hpp"

#include <deque>
#include <mutex>
#include <string>

namespace Curvz {

class ColorPickerRecents {
public:
    // Maximum retained slots. UI renders exactly this many boxes; empty
    // slots render as dashed placeholders.
    static constexpr std::size_t CAPACITY = 12;

    // Access the singleton. Loads from disk on first call.
    static ColorPickerRecents& get();

    // Current history, index 0 = most recent. Size may be < CAPACITY.
    // Returned by value — callers are UI code redrawing on change, the
    // copy cost (12 small structs) is nothing.
    std::deque<color::Color> snapshot() const;

    // Record a colour. Dedups against existing entries. Triggers a write.
    void add(const color::Color& c);

    // Wipe history (future "clear recents" menu action). Writes empty.
    void clear();

private:
    ColorPickerRecents();
    void load_locked();   // assumes m_mutex held
    void save_locked() const;
    std::string path() const;

    mutable std::mutex m_mutex;
    std::deque<color::Color> m_entries;
    bool m_loaded = false;
};

} // namespace Curvz
