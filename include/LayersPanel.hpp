#pragma once
#include "ColorPickerPopover.hpp"
#include "CurvzDocument.hpp"
#include "CurvzEntry.hpp"
#include <gtkmm/box.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/label.h>
#include <gtkmm/button.h>
#include <gtkmm/revealer.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/gestureclick.h>
#include <set>
#include <unordered_map>
#include <vector>

namespace Curvz {

// s171 m1 — forward declarations only; full types are needed in
// LayersPanel.cpp for push sites but not at header-include time.
struct CurvzProject;
class  CommandHistory;

class LayersPanel : public Gtk::Box {
public:
    LayersPanel();

    void set_document(CurvzDocument* doc);
    void refresh();

    // s171 m1 — wired so that structural mutations (layer add/delete,
    // and later: cross-layer object move, layer reorder) push commands
    // onto the undo stack instead of mutating the doc directly. Both
    // pointers are non-owning; LayersPanel just borrows them. nullptrs
    // are tolerated (silent no-op at push site) so test harnesses and
    // partial-init paths don't crash.
    void set_history(CommandHistory* history) { m_history = history; }
    void set_project(CurvzProject*   project) { m_project = project; }

    // Called from MainWindow when canvas selection changes
    void set_canvas_selection(const std::vector<SceneNode*>& selection);

    // Sync guide selection from canvas → panel (highlights rows)
    void set_guide_selection(const std::vector<SceneNode*>& sel);

    using LayerSelectedSignal        = sigc::signal<void(int)>;
    using LayerChangedSignal         = sigc::signal<void()>;
    using ObjectSelectedSignal       = sigc::signal<void(SceneNode*)>;
    using MultiSelectSignal          = sigc::signal<void(std::vector<SceneNode*>)>;
    using GuideSelectionChangedSignal = sigc::signal<void(std::vector<SceneNode*>)>;
    using RunMacroSignal             = sigc::signal<void(std::string /*macro_id*/)>;
    // Emitted when the user picks "Rebuild Blend Steps" from the right-
    // click menu on a Blend row. MainWindow routes it to Canvas::rebuild_blend.
    using RebuildBlendSignal         = sigc::signal<void(SceneNode* /*blend*/)>;

    LayerSelectedSignal&  signal_layer_selected()  { return m_sig_layer_selected;  }
    LayerChangedSignal&   signal_layer_changed()   { return m_sig_layer_changed;   }
    ObjectSelectedSignal& signal_object_selected() { return m_sig_object_selected; }
    MultiSelectSignal&    signal_multi_selected()  { return m_sig_multi_selected;  }
    GuideSelectionChangedSignal& signal_guide_selection_changed() { return m_sig_guide_selection; }
    RunMacroSignal&       signal_run_macro()       { return m_sig_run_macro;       }
    RebuildBlendSignal&   signal_rebuild_blend()   { return m_sig_rebuild_blend;   }

    // s237 m2 — rebuild the panel's row list from current doc state.
    // Promoted from private to public so script-side mutators
    // (LayersScriptable's `new`/`delete`/`move`/etc.) can request the
    // same panel refresh the +/- button handlers do at their tails.
    // The button handlers wrap it in Glib::signal_idle().connect_once
    // for the deferred-rebuild shape; the script-side caller can do
    // the same (MainWindow's panel-getter lambda is the natural site
    // for the idle scheduling, not LayersScriptable itself).
    void rebuild();

private:
    void add_layer_row(int layer_idx, Gtk::Box* parent);
    void add_guide_layer_row(int layer_idx, Gtk::Box* parent);
    void add_ref_layer_row(int layer_idx, Gtk::Box* parent);
    void add_measure_layer_row(int layer_idx, Gtk::Box* parent);
    void add_grid_layer_row(int layer_idx, Gtk::Box* parent);
    void add_margin_layer_row(int layer_idx, Gtk::Box* parent);
    void add_path_row(int layer_idx, int obj_idx, Gtk::Box* parent);
    void add_group_row(SceneNode* group, int layer_idx, int indent, Gtk::Box* parent);
    void add_child_row(SceneNode* obj, int layer_idx, int indent, Gtk::Box* parent);
    void refresh_highlights();

    // s112 — layer-lock enforcement helper. Reads live state every call so
    // a lock toggled mid-session takes effect at the next click/drag without
    // requiring a panel rebuild. Returns false on out-of-range indices.
    bool layer_at_locked(int layer_idx) const;

    // s172 m4 — push EditLayerFieldCommand for a layer-state mutation
    // (rename / color / visible / lock). Helpers exist because there are
    // ~14 push sites across the panel (the four canonical fields plus
    // the five special-layer-row variants of visible+lock); centralizing
    // keeps them consistent and avoids 14 copies of the same iid-snap +
    // history-guard boilerplate. Two overloads keep the call sites
    // readable: string-valued fields (rename, color) take before/after
    // strings; bool-valued fields (visible, lock) take before/after
    // bools. The `is_color` flag on the string form picks Color vs Name;
    // the `is_locked` flag on the bool form picks Locked vs Visible.
    // Silently no-op if m_history/m_project aren't wired or the layer
    // iid is empty.
    void push_edit_layer_string_field(int layer_idx,
                                      bool is_color,
                                      const std::string& before_str,
                                      const std::string& after_str);
    void push_edit_layer_bool_field(int layer_idx,
                                    bool is_locked,
                                    bool before_bool,
                                    bool after_bool);

    void on_add_layer();
    void on_delete_layer();

    // ── Collapse tracking ─────────────────────────────────────────────────
    // Two pieces, deliberately separated:
    //
    //   m_collapse_state   — persistent across rebuilds and doc switches.
    //                        Keyed by stable string (layer internal_id or
    //                        group internal_id, with "L:"/"G:" prefix). This
    //                        is the authoritative "is it expanded" record.
    //
    //   m_collapse_entries — transient.  Holds live Revealer/Button pointers
    //                        so toggle_* can animate the currently-rendered
    //                        widgets. Rebuilt from scratch each rebuild().
    //
    struct CollapseEntry {
        int            layer_idx;
        SceneNode*     group_node = nullptr;  // nullptr = layer, non-null = group
        Gtk::Revealer* revealer  = nullptr;
        Gtk::Button*   arrow     = nullptr;
        bool           expanded  = true;
        std::string    state_key; // persistence key into m_collapse_state
    };
    void toggle_layer(int layer_idx);
    void toggle_group(SceneNode* group);
    CollapseEntry* find_collapse(int layer_idx);
    CollapseEntry* find_collapse_group(SceneNode* group);

    // Persistence helpers
    static std::string layer_state_key(const SceneNode& layer);
    static std::string group_state_key(const SceneNode& group);
    bool  lookup_expanded(const std::string& key, bool default_expanded) const;
    void  write_expanded(const std::string& key, bool expanded);

    std::unordered_map<std::string, bool> m_collapse_state;

    // ── Row tracking for highlight sync ──────────────────────────────────
    struct RowEntry {
        int          layer_idx;
        int          obj_idx;   // -1 = layer header
        SceneNode*   obj;       // nullptr for layer rows
        Gtk::Widget* widget;
    };

    // ── Drop zones ────────────────────────────────────────────────────────
    enum class DropZone { None, Before, After, Inside };
    DropZone compute_drop_zone(Gtk::Widget* w, double y, bool is_layer) const;
    void apply_drop_highlight(Gtk::Widget* w, DropZone zone);
    void clear_drop_highlight();

    // ── DnD ───────────────────────────────────────────────────────────────
    static int  encode_payload(int type, int li, int oi)
        { return (type << 24) | (li << 12) | oi; }
    static void decode_payload(int v, int& type, int& li, int& oi)
        { type = (v >> 24) & 0xFF; li = (v >> 12) & 0xFFF; oi = v & 0xFFF; }

    void setup_layer_drag(Gtk::Widget* w, int layer_idx);
    void setup_layer_drop(Gtk::Widget* w, int layer_idx);
    void setup_path_drag(Gtk::Widget* w, int layer_idx, int obj_idx);
    void setup_path_drop(Gtk::Widget* w, int layer_idx, int obj_idx);

    // ── Palette ───────────────────────────────────────────────────────────
    static constexpr int PALETTE_SIZE = 8;
    static const char* palette(int i);

    // ── State ─────────────────────────────────────────────────────────────
    CurvzDocument* m_doc           = nullptr;
    // s171 m1 — borrowed pointers for undoable structural ops. Set via
    // set_history / set_project from MainWindow_zones during panel
    // assembly. Read-only outside the panel; we never mutate them.
    CommandHistory* m_history      = nullptr;
    CurvzProject*   m_project      = nullptr;
    int            m_active_layer  = 0;
    bool           m_rebuilding    = false;
    std::vector<SceneNode*> m_canvas_selection;  // mirrors canvas m_selection

    // Panel multi-select: {layer_idx, obj_idx} pairs (-1 obj_idx = layer)
    std::set<std::pair<int,int>> m_panel_selection;

    std::vector<CollapseEntry> m_collapse_entries;
    std::vector<RowEntry>      m_row_entries;

    struct DropHighlight {
        Gtk::Widget* widget = nullptr;
        DropZone     zone   = DropZone::None;
    } m_drop_hl;

    // s207 m2: ColorPickerPopover is the app-wide singleton — accessed
    // via ColorPickerPopover::shared(). The earlier `m_color_popover`
    // member is gone; the shared instance's ensure_attached() runs on
    // first open().

    // ── Widgets ───────────────────────────────────────────────────────────
    Gtk::Box            m_toolbar{Gtk::Orientation::HORIZONTAL};
    Gtk::Label          m_title;
    Gtk::Button         m_btn_add;
    Gtk::Button         m_btn_delete;
    Gtk::ScrolledWindow m_scroll;
    Gtk::Box            m_content{Gtk::Orientation::VERTICAL};

    // ── Guide selection state — mirrors canvas ────────────────────────────
    std::vector<SceneNode*> m_guide_selection;

    // ── Signals ───────────────────────────────────────────────────────────
    LayerSelectedSignal         m_sig_layer_selected;
    LayerChangedSignal          m_sig_layer_changed;
    ObjectSelectedSignal        m_sig_object_selected;
    MultiSelectSignal           m_sig_multi_selected;
    GuideSelectionChangedSignal m_sig_guide_selection;
    RunMacroSignal              m_sig_run_macro;
    RebuildBlendSignal          m_sig_rebuild_blend;

    // ── Macro area ────────────────────────────────────────────────────────
    // A collapsible section at the top of m_content lists starred macros
    // for one-click runs. Rebuilt alongside layers; subscribes to
    // MacroManager::signal_changed() so ★-toggles refresh the panel.
    void add_macro_section(Gtk::Box* parent);
    bool m_macros_expanded = true;
    sigc::connection m_macro_mgr_conn;
};

} // namespace Curvz
