#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <sigc++/signal.h>

// ── MacroSystem ───────────────────────────────────────────────────────────────
// App-wide macro storage, serialization, and management.
// Macros are stored in ~/.config/curvz/macros.json
//
// Object model:
//   MacroFolder
//     └─ Macro[]
//          └─ MacroStep[]
//
// MacroManager is a singleton owned by Application / accessed via instance().
// ─────────────────────────────────────────────────────────────────────────────

namespace Curvz {

// ── MacroStep ────────────────────────────────────────────────────────────────
// One recorded operation.  All numeric values are in document (user) space.
// Steps that don't use a field leave it at its default (0.0 / "" / false).

struct MacroStep {
    enum class Op {
        // Object ops
        Clone,          // duplicate in-place (no offset)
        Duplicate,      // duplicate with (dx, dy) offset
        Delete,
        Group,
        Ungroup,

        // Transform ops
        Move,           // dx, dy
        Scale,          // scale_x, scale_y (factors, e.g. 2.0 = double)
        Rotate,         // angle_deg; pivot_x, pivot_y (doc space; 0,0 = bbox centre)
        FlipH,
        FlipV,

        // Alignment ops
        AlignLeft,      // ref_iid = "" → selection bbox; else ref point internal_id
        AlignCenterH,
        AlignRight,
        AlignTop,
        AlignMiddleV,
        AlignBottom,
        DistributeH,
        DistributeV,

        // Style ops
        SetFill,        // color_hex  e.g. "#FF0000", "" = currentColor
        SetStroke,      // color_hex
        SetStrokeWidth, // value (doc units)
        SetOpacity,     // value  0.0–1.0

        // Path ops
        BooleanUnion,
        BooleanSubtract,
        BooleanIntersect,
        OffsetPath,     // value = offset distance
        ReversePath,

        // Arrangement
        BringToFront,
        SendToBack,
        BringForward,
        SendBackward,
    };

    Op          op          = Op::Move;

    // Transform payload
    double      dx          = 0.0;
    double      dy          = 0.0;
    double      scale_x     = 1.0;
    double      scale_y     = 1.0;
    double      angle_deg   = 0.0;
    double      pivot_x     = 0.0;  // used when rotate has explicit pivot
    double      pivot_y     = 0.0;
    bool        pivot_is_explicit = false;

    // Style payload
    std::string color_hex;          // fill or stroke colour
    double      value       = 0.0;  // stroke width, opacity, offset distance

    // Alignment payload
    std::string ref_iid;            // internal_id of ref point / guide anchor
                                    // empty = align relative to selection bbox

    // Human-readable description (auto-generated on record, editable by user)
    std::string label;

    // Serialization
    nlohmann::json to_json() const;
    static MacroStep from_json(const nlohmann::json& j);

    // Auto-generate a readable label from the op + payload
    std::string auto_label() const;
};

// ── Macro ────────────────────────────────────────────────────────────────────
struct Macro {
    std::string              internal_id;   // UUID
    std::string              name;
    std::string              folder_id;     // owning MacroFolder::internal_id
    std::vector<MacroStep>   steps;
    bool                     in_layer_panel = false; // shown in layers macro area
    std::string              hotkey;        // e.g. "<Primary>1" — optional

    nlohmann::json to_json() const;
    static Macro   from_json(const nlohmann::json& j);
};

// ── MacroFolder ──────────────────────────────────────────────────────────────
struct MacroFolder {
    std::string              internal_id;
    std::string              name;
    std::vector<std::string> macro_ids;    // ordered list of Macro::internal_id

    nlohmann::json to_json() const;
    static MacroFolder from_json(const nlohmann::json& j);
};

// ── MacroManager ─────────────────────────────────────────────────────────────
// Singleton.  Owns all folders and macros.  Persists to macros.json.
// Thread-safety: single-threaded GTK main loop only.

class MacroManager {
public:
    static MacroManager& instance();

    // ── Persistence ──────────────────────────────────────────────────────
    void load();   // call once at startup
    void save();   // call after any mutation

    // ── Folder ops ───────────────────────────────────────────────────────
    MacroFolder& add_folder(const std::string& name);
    void         delete_folder(const std::string& folder_id); // deletes member macros
    void         rename_folder(const std::string& folder_id, const std::string& name);
    MacroFolder* find_folder(const std::string& folder_id);

    // ── Macro ops ────────────────────────────────────────────────────────
    Macro&  add_macro(const std::string& folder_id, const std::string& name);
    void    delete_macro(const std::string& macro_id);
    void    rename_macro(const std::string& macro_id, const std::string& name);
    void    move_macro(const std::string& macro_id, const std::string& dest_folder_id);
    Macro*  find_macro(const std::string& macro_id);

    // Returns macros marked in_layer_panel, in folder order
    std::vector<Macro*> layer_panel_macros();

    // All folders in order
    const std::vector<MacroFolder>& folders() const { return m_folders; }

    // ── Current macro (for Ctrl+M) ────────────────────────────────────────
    void    set_current(const std::string& macro_id);
    Macro*  current_macro();

    // ── Recording ────────────────────────────────────────────────────────
    bool    is_recording() const { return m_recording; }
    void    start_recording(const std::string& macro_id);
    void    stop_recording();
    void    record_step(MacroStep step); // append to recording target

    // ── Change notification ───────────────────────────────────────────────
    // Emitted after any structural change (add/delete/rename/reorder/save).
    using ChangedSignal = sigc::signal<void()>;
    ChangedSignal& signal_changed() { return m_sig_changed; }

private:
    MacroManager() = default;

    std::string macros_path() const;

    std::vector<MacroFolder> m_folders;
    // All macros keyed by internal_id for O(1) lookup
    // (order within folders is authoritative via folder.macro_ids)
    std::unordered_map<std::string, Macro> m_macros;

    std::string m_current_macro_id;
    bool        m_recording        = false;
    std::string m_recording_macro_id;

    ChangedSignal m_sig_changed;
};

} // namespace Curvz
