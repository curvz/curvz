#pragma once
#include "ColorPickerPopover.hpp"
#include "SceneNode.hpp"
#include "CurvzDocument.hpp"
#include "CurvzProject.hpp"
#include "CommandHistory.hpp"
#include "CurvzSpinButton.hpp"
#include <chrono>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/entry.h>
#include "CurvzEntry.hpp"
#include <gtkmm/drawingarea.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/grid.h>
#include <sigc++/sigc++.h>
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <vector>

// s205 m1 — forward decl for the Selection-section pivot picker pointer.
// Full header (curvz/widgets/RefPointPicker.hpp) is included in
// PropertiesPanel.cpp at the use site.
namespace curvz::widgets { class RefPointPicker; }

namespace Curvz {

class Canvas; // forward — full include would be circular; only needed to read selection()

class PropertiesPanel : public Gtk::Box {
public:
    PropertiesPanel();

    void set_project(CurvzProject* project) { m_project = project; }
    void set_history(CommandHistory* history) { m_history = history; }
    // S58f: Multi-select fill/stroke broadcast — the panel needs selection
    // visibility (not just the primary). Wired by MainWindow at startup;
    // forward-declared Canvas* avoids pulling the full Canvas header.
    void set_canvas_widget(Canvas* canvas) { m_canvas_widget = canvas; }

    void reset_undo_coalesce() {
        m_undo_last_obj  = nullptr;
        m_undo_last_time = {};
        m_suppress_push  = true;
    }

    // s165 m3 — bump the build generation so any in-flight queued signal
    // handlers (signal_value_changed deliveries that GTK queued during prior
    // set_value() calls in refresh_node, etc.) see their captured `gen` as
    // stale and early-return instead of dereferencing potentially-stale
    // `obj` / `node_idx` captures.
    //
    // Call this at the top of any caller that's about to mutate the document
    // tree out from under the inspector — e.g., on_undo / on_redo before
    // m_history.undo()/redo(). Without this, a pending value-changed signal
    // queued before the mutation can fire after the mutation, hitting
    // dangling pointers (timing-sensitive crash in Node mode after fast-path
    // refresh + immediate Ctrl+Z).
    //
    // Cheap (one increment); idempotent in the sense that bumping a few extra
    // times in a turn is harmless — every captured `gen` snapshots the value
    // at lambda construction.
    void invalidate_lambdas() { ++m_build_gen; }

    void set_ruler_origin(double ox, double oy) {
        m_ruler_ox = ox;
        m_ruler_oy = oy;
        // Invalidate fast path — coordinate display depends on origin,
        // so next refresh_node must do a full rebuild to update all inputs.
        m_current = nullptr;
    }

    // Inspector section open/close state — persisted across sessions
    const std::map<std::string, bool>& section_open_state() const { return m_section_open; }
    void set_section_open_state(const std::map<std::string, bool>& state) { m_section_open = state; }

    void refresh(CanvasModel* canvas, SceneNode* obj);
    void refresh_node(CanvasModel* canvas, SceneNode* obj, int node_idx);
    void sync_selection(SceneNode* obj);  // lightweight position sync during move
    void sync_selected_guide();           // lightweight guide X/Y/A sync during guide drag
    void sync_selected_pivot();           // s205 m1 — live-track Canvas pivot state in the
                                          // inspector's Selection-section pivot picker.
                                          // Cheap: bails unless picker built AND canvas wired.

    void show_document_props(CanvasModel* canvas);
    void show_object_props(SceneNode* obj);
    void show_empty();

    using PropChangedSignal       = sigc::signal<void()>;
    using CanvasChangedSignal     = sigc::signal<void(CanvasModel)>;
    using RequestNodeEditSignal   = sigc::signal<void(int,
                                                      double, double,
                                                      double, double,
                                                      double, double)>;
    using RequestReverseSignal    = sigc::signal<void()>;
    using RequestOpenAtNodeSignal = sigc::signal<void()>;
    using RequestSplitSignal      = sigc::signal<void()>;
    using RequestNodeTypeChangeSignal = sigc::signal<void(BezierNode::Type)>;
    using DocRenamedSignal        = sigc::signal<void(std::string)>;
    using RequestCanvasFocusSignal = sigc::signal<void()>;
    using GuideSelectedSignal             = sigc::signal<void(SceneNode*)>;  // kept for compat
    using GuideSelectionChangedSignal     = sigc::signal<void(std::vector<SceneNode*>)>;
    // Fired when guides are added or deleted — triggers LayersPanel rebuild.
    using GuideLayerChangedSignal         = sigc::signal<void()>;
    // Fired when the user clicks "From 2 points…" in the Guides section.
    using GuideConstructRequestedSignal   = sigc::signal<void()>;

    PropChangedSignal&      signal_prop_changed()        { return m_sig_prop_changed;        }
    CanvasChangedSignal&    signal_canvas_changed()      { return m_sig_canvas_changed;      }
    RequestNodeEditSignal&  signal_request_node_edit()   { return m_sig_request_node_edit;   }
    RequestReverseSignal&   signal_request_reverse()     { return m_sig_request_reverse;     }
    RequestOpenAtNodeSignal& signal_request_open_at_node(){ return m_sig_request_open_at_node;}
    RequestSplitSignal&     signal_request_split()       { return m_sig_request_split;       }
    RequestNodeTypeChangeSignal& signal_request_node_type_change() { return m_sig_request_node_type_change; }
    using RequestFlipSignal = sigc::signal<void(bool /*horizontal*/)>;
    RequestFlipSignal&      signal_request_flip()        { return m_sig_request_flip;        }
    using RequestDetachTextSignal = sigc::signal<void(SceneNode*)>;
    RequestDetachTextSignal& signal_request_detach_text() { return m_sig_request_detach_text; }
    // Emitted when the user clicks "Release" in the Blend section of the
    // inspector. MainWindow connects this to Canvas::release_blend.
    using RequestReleaseBlendSignal = sigc::signal<void()>;
    RequestReleaseBlendSignal& signal_request_release_blend() { return m_sig_request_release_blend; }
    // s146 m2 — Emitted when the user clicks Release / Flatten in the
    // Warp section of the inspector. MainWindow connects these to its
    // existing on_warp_release / on_warp_flatten handlers (same code
    // path as the Path menu entries — single source of truth).
    using RequestReleaseWarpSignal = sigc::signal<void()>;
    using RequestFlattenWarpSignal = sigc::signal<void()>;
    RequestReleaseWarpSignal& signal_request_release_warp() { return m_sig_request_release_warp; }
    RequestFlattenWarpSignal& signal_request_flatten_warp() { return m_sig_request_flatten_warp; }
    DocRenamedSignal&       signal_doc_renamed()         { return m_sig_doc_renamed;         }
    RequestCanvasFocusSignal& signal_request_canvas_focus(){ return m_sig_request_canvas_focus;}
    GuideSelectedSignal&           signal_guide_selected()           { return m_sig_guide_selected;           }
    GuideSelectionChangedSignal&   signal_guide_selection_changed()  { return m_sig_guide_selection_changed;  }
    GuideLayerChangedSignal&       signal_guide_layer_changed()      { return m_sig_guide_layer_changed;      }
    GuideConstructRequestedSignal& signal_guide_construct_requested(){ return m_sig_guide_construct_requested;}

    // S91: User clicked the Edit gradient button in the inspector's
    // appearance section. Carries the current FillStyle to seed the
    // editor with, plus a callback the host invokes on Apply with the
    // edited result. The inspector itself does not own a GradientDialog;
    // MainWindow does, and it routes show() through this signal.
    //
    // The callback is captured by the inspector at signal-emit time and
    // closes over: the SceneNode being edited, the slot (fill vs
    // stroke), and the gen + canvas pointers needed to push the
    // EditAppearanceCommand. MainWindow just calls cb(edited) inside
    // its GradientDialog Apply lambda — no per-host knowledge of the
    // commit path lives in MainWindow.
    using RequestGradientEditSignal =
        sigc::signal<void(FillStyle /*current*/,
                          std::function<void(FillStyle /*edited*/)> /*apply_cb*/)>;
    RequestGradientEditSignal& signal_request_gradient_edit() {
        return m_sig_request_gradient_edit;
    }

    SceneNode* current_object() const { return m_current;  }
    int        current_node()   const { return m_node_idx; }

    // Called by MainWindow to sync guide selection → inspector display.
    void set_selected_guide(SceneNode* g);  // single (primary)
    void set_guide_selection(const std::vector<SceneNode*>& sel);
    const std::vector<SceneNode*>& guide_selection() const { return m_guide_selection; }

    bool current_node_type_matches(SceneNode* obj, int node_idx) const {
        if (!obj || !obj->path) return false;
        if (node_idx < 0 || node_idx >= (int)obj->path->nodes.size()) return false;
        return obj->path->nodes[node_idx].type == m_node_type;
    }

private:
    void do_clear();
    void emit_prop_changed();
    void emit_canvas_focus();
    void emit_request_node_edit();
    void push_inspector_command(SceneNode* obj);
    // Recompute Xv/Yv/Wv/Hv readout labels from m_current's geometry +
    // strokes. Updates label text and toggles visibility of the visual
    // rows based on whether any leaf has a stroke contribution. Safe to
    // call when the labels haven't been built (m_sel_lbl_xv == nullptr) —
    // it bails early.
    void update_visual_labels();

    // Group-level collapsible — wraps a set of child sections.
    // Returns the container box; pass it as `parent` to add_collapsible().
    Gtk::Box* add_group_collapsible(const char* title, bool expanded = true);

    // Child section collapsible.  If parent != nullptr, appends into that box
    // instead of m_inner (used for sections inside a group).
    //
    // accessory: optional dim text appended to the right of the section
    // title (e.g. "Dimensions  IN" — the IN is the accessory). Useful for
    // sections that carry an at-a-glance state read-out in the header.
    // Pass nullptr (default) for no accessory.
    Gtk::Box* add_collapsible(const char* title, bool expanded = true,
                              Gtk::Box* parent = nullptr,
                              const char* accessory = nullptr);
    void      add_collapsible_disabled(const char* title, const char* placeholder,
                                       Gtk::Box* parent = nullptr);

    void add_row(const char* label, const std::string& value);
    void build_canvas_section(std::shared_ptr<CanvasModel> cm, Gtk::Box* parent = nullptr);
    // s150 — Theme disclosure (renamed from build_motif_disclosure in s148 m2).
    // Wraps the four "draftsman setup" facets of a doc (Canvas colours,
    // Margins, Grid, Guides) under a single nested collapsible. The
    // wrapper is labelled "Theme" because that's now the user-facing
    // word for the saveable doc-style bundle (Themes panel saves and
    // applies what's inside this disclosure). Lives in the Document
    // group between Metadata and Dimensions. ARC.md vocabulary: Motif
    // is now internal-only (the Motif enum + the MotifSettings colour
    // sub-bundle inside a Theme); Theme is the user-facing word.
    void build_theme_disclosure(CurvzDocument* doc, Gtk::Box* parent = nullptr);
    // s148 m2 — Canvas-colours section (header label "Canvas"; was
    // "Motif" in m1/early m2). Per-document artboard / workspace /
    // creation chips + Reset. Lives inside the Theme disclosure built
    // by build_theme_disclosure. s150 renamed the builder from
    // build_motif_section → build_canvas_colours_section to match
    // what it actually builds, after the user-facing "Motif" word
    // went internal-only.
    void build_canvas_colours_section(CurvzDocument* doc, Gtk::Box* parent = nullptr);
    // s143 m1 — Application group section: user-tier app preferences
    // surfaced from AppPreferences. First inhabitant: boolean cleanup
    // quality slider. Future additions (recent-projects max, autosave
    // debounce, log path, custom CSS path) land here as additional rows
    // or sibling sections under the same Application group. Doesn't
    // depend on a project/document — reads/writes AppPreferences::instance.
    void build_app_section(Gtk::Box* parent = nullptr);
    // s148 m2 — Application ▸ Appearance subsection. Currently houses
    // the Dark/Light Theme switch only. Sorted first alphabetically
    // within the App group (Appearance / Boolean cleanup / Editing /
    // Paths / Startup / Warp). m2 sub-ship 1 reads/writes
    // m_project->motif (existing project-level state); sub-ship 2
    // migrates the source to AppPreferences::instance for true
    // app-tier persistence. Takes CurvzProject* during sub-ship 1
    // for that reason; sub-ship 2 will drop the parameter.
    void build_app_appearance_section(CurvzProject* project, Gtk::Box* parent = nullptr);
    // s150: build_snap_section deleted — Snap behaviour now lives at the
    // toolbar Snap switch + its right-click popover. Storage on doc.snap
    // is unchanged; only the inspector surface went away.
    // s150: build_measure_section deleted — Measure behaviour now lives
    // at the toolbar Ruler button + its right-click popover. Storage on
    // doc.measure_save_to_layer / doc.measure_destruct_after_copy is
    // unchanged; only the inspector surface went away.
    void build_grid_section(CurvzDocument* doc, Gtk::Box* parent = nullptr);
    void build_margin_section(CurvzDocument* doc, Gtk::Box* parent = nullptr);
    void update_grid_margin_units();  // called in-place when display unit changes
    void build_selection_section(SceneNode* obj, Gtk::Box* parent = nullptr);
    void build_node_section(SceneNode* obj, int node_idx, Gtk::Box* parent = nullptr);
    // Blend-specific section — visible only when obj is a Blend container.
    //   Steps spin (live cache rebuild on change), reverse toggle, stroke-
    //   width override + start/end, A/B node-count labels, Release button.
    //   Emits prop_changed on every change so Canvas redraws and the cache
    //   gets invalidated via mark_all_blends_dirty in MainWindow.
    void build_blend_section(SceneNode* obj, Gtk::Box* parent = nullptr);
    // s146 m2 — Warp inspector section. Mirrors WarpDialog's controls
    // (top/bot anchor counts, preset dropdown, quality slider) but lives
    // in the Object group so a selected Warp is editable in place rather
    // than via a re-opened dialog. Same preset machinery (lifted to
    // curvz::utils::generate_warp_preset) feeds both surfaces. Edits
    // write directly to obj.warp_env_top / warp_env_bottom / warp_quality
    // and flag warp_cache_dirty; canvas redraws on emit_prop_changed.
    // Like build_blend_section, this section's edits don't push undo —
    // future milestone may add coalesced EditWarpCommand pushes here.
    void build_warp_section(SceneNode* obj, Gtk::Box* parent = nullptr);
    // s179 m3 — Object ▸ Guides section. Always visible; mirrors the
    // build_warp_section pattern (always-present, dual-mode). When
    // guides are selected (m_guide_selection non-empty) the section
    // shows the per-guide editor — single-select X/Y/A spinners + lock
    // + delete; multi-select count + bulk delete. When no guides are
    // selected it shows just the "From 2 points…" construct tool, which
    // is a creation verb available regardless of selection state.
    //
    // Selection-driven content moved here from build_guide_section
    // (Document ▸ Theme ▸ Guides). The Document-tier section retains
    // only the colour swatch — colour is theme-saveable style, the rest
    // is selection-driven editing that belongs at object scope.
    void build_object_guides_section(CurvzDocument* doc,
                                     Gtk::Box* parent = nullptr);
    // S97 m3 — drop shadow inspector section. Shown for any node that
    // can_have_shadow() (Path, Compound, Group, Text, Image, ClipGroup,
    // Blend, Warp). Controls: enable toggle, dx/dy/blur spinbuttons,
    // colour swatch, colour-alpha slider, opacity slider. Edits route
    // through push_inspector_command for undo coalescing — each window
    // captures shadow_before/after on EditObjectCommand alongside the
    // existing fill/stroke/binding snapshots.
    void build_shadow_section(SceneNode* obj, Gtk::Box* parent = nullptr);
    void build_metadata_section(Gtk::Box* parent = nullptr);
    // S83 m4h: build_style_section is gone. The "Bound to style: <name>
    // [Unbind]" row moved into add_fill_stroke_section as the first row
    // of the Appearance section, alongside per-slot fill/stroke
    // swatch-binding rows ("Fill swatch: <name> [×]",
    // "Stroke swatch: <name> [×]"). All three binding indicators
    // (style, fill swatch, stroke swatch) live together at the top of
    // Appearance — single place to glance for binding state. The
    // binding-row block is invisible when nothing is bound.
    void add_fill_stroke_section(SceneNode* obj, Gtk::Box* parent = nullptr);
    // S58f: After a primary-object fill/stroke commit, broadcast the same
    // change to every sibling in the canvas selection. Pushes ONE composite
    // command per broadcast so Ctrl+Z reverts the whole multi-object edit.
    // No-op when there's only one selected object.
    void broadcast_appearance_to_siblings(SceneNode* primary,
                                          bool fill_changed,
                                          bool stroke_changed);

    Gtk::ScrolledWindow m_scroll;
    Gtk::Box            m_inner{Gtk::Orientation::VERTICAL};

    CurvzProject*    m_project  = nullptr;
    CommandHistory*  m_history  = nullptr;
    CanvasModel*     m_canvas   = nullptr;
    Canvas*          m_canvas_widget = nullptr; // S58f: multi-select broadcast for Appearance
    SceneNode*       m_current  = nullptr;
    // Selection section — CurvzSpinButton pointers updated by build_selection_section,
    // used by sync_selection for lightweight in-place updates during drag
    CurvzSpinButton*  m_sel_sp_x = nullptr;
    CurvzSpinButton*  m_sel_sp_y = nullptr;
    CurvzSpinButton*  m_sel_sp_w = nullptr;
    CurvzSpinButton*  m_sel_sp_h = nullptr;
    // Visual-size readout labels — Xv/Yv under sp_x/sp_y, Wv/Hv under
    // sp_w/sp_h. Read-only; they show construction bbox + stroke padding.
    // Hidden when no leaf in the selection has a non-None positive-width
    // stroke (visual == construction). See compute_visual_bbox.
    Gtk::Label*       m_sel_lbl_xv = nullptr;
    Gtk::Label*       m_sel_lbl_yv = nullptr;
    Gtk::Label*       m_sel_lbl_wv = nullptr;
    Gtk::Label*       m_sel_lbl_hv = nullptr;
    // s205 m1 — pivot picker embedded under the Rotate row in the
    // Selection section. Bidirectional live sync with Canvas's pivot
    // state via signal_pivot_changed + sync_selected_pivot. Cleared in
    // do_clear() alongside m_sel_sp_*. Forward-declared above to avoid
    // pulling the widget header into this file (only the .cpp uses it).
    curvz::widgets::RefPointPicker* m_sel_pivot_picker = nullptr;
    // Ref-pt selection section uses its own X/Y spin buttons (no W/H —
    // refpts are points). Stored so sync_selection can live-update them
    // during refpt drag/nudge, mirroring the path-object path.
    CurvzSpinButton*  m_ref_sp_x = nullptr;
    CurvzSpinButton*  m_ref_sp_y = nullptr;
    // Guide section X/Y/A spin buttons. Stored so sync_selected_guide
    // can live-update them during canvas guide drag, the same way the
    // refpt pointers above support live-update during refpt drag. Cleared
    // in do_clear()'s prelude alongside m_ref_sp_*; set in
    // build_object_guides_section's single-guide branch.
    CurvzSpinButton*  m_guide_sp_x = nullptr;
    CurvzSpinButton*  m_guide_sp_y = nullptr;
    CurvzSpinButton*  m_guide_sp_a = nullptr;
    // Keep adj refs for legacy sync_selection path (adj is owned by spinbutton)
    Glib::RefPtr<Gtk::Adjustment> m_sel_adj_x;
    Glib::RefPtr<Gtk::Adjustment> m_sel_adj_y;
    Glib::RefPtr<Gtk::Adjustment> m_sel_adj_w;
    Glib::RefPtr<Gtk::Adjustment> m_sel_adj_h;

    // Grid section — CurvzSpinButton pointers; refresh_units() called on unit change
    Gtk::Label*       m_grid_lbl_spacing = nullptr;
    Gtk::Label*       m_grid_lbl_offset  = nullptr;
    CurvzSpinButton*  m_grid_sp_sx  = nullptr, *m_grid_sp_sy  = nullptr;
    CurvzSpinButton*  m_grid_sp_ox  = nullptr, *m_grid_sp_oy  = nullptr;

    // Margin section — CurvzSpinButton pointers; refresh_units() called on unit change
    Gtk::Label*       m_margin_lbl_insets = nullptr;
    Gtk::Label*       m_margin_lbl_cgap   = nullptr;
    Gtk::Label*       m_margin_lbl_rgap   = nullptr;
    CurvzSpinButton*  m_margin_sp_t    = nullptr, *m_margin_sp_b    = nullptr;
    CurvzSpinButton*  m_margin_sp_l    = nullptr, *m_margin_sp_r    = nullptr;
    CurvzSpinButton*  m_margin_sp_cgap = nullptr, *m_margin_sp_rgap = nullptr;
    Gtk::SpinButton*  m_margin_sp_cg   = nullptr, *m_margin_sp_rg   = nullptr;
    int              m_node_idx = -1;
    BezierNode::Type m_node_type = BezierNode::Type::Smooth;
    bool             m_loading  = false;
    double           m_ruler_ox = 0.0;
    double           m_ruler_oy = 0.0;
    bool             m_scale_linked = true;  // uniform scale link state
    // s265 m2: Dimensions Size W/H aspect lock. When true, editing W
    // updates H proportionally (and vice versa) to preserve the canvas
    // aspect ratio. Defaults true — the common case is "I want a 256
    // square" or "I want to scale 4×8 → 5×10 keeping the 1:2 shape."
    // Lives on the panel (not the model) because it's an editor
    // preference, not document state.
    bool             m_canvas_aspect_locked = true;

    // ── Undo coalescing ───────────────────────────────────────────────────────
    SceneNode*  m_undo_last_obj  = nullptr;
    std::chrono::steady_clock::time_point m_undo_last_time;
    bool        m_suppress_push  = false;
    PathData    m_undo_before;
    FillStyle   m_undo_fill_before;
    StrokeStyle m_undo_stroke_before;
    // S82 m4f: swatch-id capture for the inspector coalesce window.
    // Snapshotted at the same points as m_undo_fill_before /
    // m_undo_stroke_before — panel-rebuild snapshots and the post-
    // window slide. Restored alongside fill/stroke on undo so an
    // inspector edit on a swatch-bound object restores both colour
    // AND binding. Without these the funnel's break-on-override
    // clears the binding mid-edit but leaves the cleared id outside
    // the command's snapshot, so undo no-ops on the binding.
    std::string m_undo_fill_swatch_id_before;
    std::string m_undo_stroke_swatch_id_before;
    // S92 m3: bound_style capture for the inspector coalesce window.
    // Mirror of the S82 m4f swatch-id pattern. Inspector edits that
    // route through style::mutate_appearance now clear bound_style as
    // a break-on-override side effect; capturing it here lets undo
    // restore the binding alongside fill/stroke.
    std::string m_undo_bound_style_before;
    // S97 m3: shadow capture for the inspector coalesce window. Same
    // shape as fill/stroke snapshots — captured at refresh time and at
    // each window boundary, restored as a unit by EditObjectCommand.
    // Bundled as ShadowParams so the nine shadow_* fields move as one
    // value rather than nine parallel members.
    ShadowParams m_undo_shadow_before;
    // Incremented on every do_clear(). Lambdas capture this at build time and
    // check it before acting — stale widgets from a previous build are silently
    // ignored even if GTK delivers a pending signal after the rebuild.
    uint32_t    m_build_gen = 0;

    // Persistent open/close state per section title.
    std::map<std::string, bool> m_section_open;

    // Stored adjustments for live node tracking.
    Glib::RefPtr<Gtk::Adjustment> m_adj_ix, m_adj_iy;
    Glib::RefPtr<Gtk::Adjustment> m_adj_ax, m_adj_ay;
    Glib::RefPtr<Gtk::Adjustment> m_adj_ox, m_adj_oy;

    // ── Guide section state ───────────────────────────────────────────────────
    SceneNode*               m_selected_guide  = nullptr;  // primary (first)
    std::vector<SceneNode*>  m_guide_selection;            // full set

    PropChangedSignal        m_sig_prop_changed;
    CanvasChangedSignal      m_sig_canvas_changed;
    RequestNodeEditSignal    m_sig_request_node_edit;
    RequestReverseSignal     m_sig_request_reverse;
    RequestOpenAtNodeSignal  m_sig_request_open_at_node;
    RequestSplitSignal       m_sig_request_split;
    RequestNodeTypeChangeSignal m_sig_request_node_type_change;
    RequestFlipSignal        m_sig_request_flip;
    RequestDetachTextSignal  m_sig_request_detach_text;
    RequestReleaseBlendSignal m_sig_request_release_blend;
    RequestReleaseWarpSignal m_sig_request_release_warp;
    RequestFlattenWarpSignal m_sig_request_flatten_warp;
    DocRenamedSignal         m_sig_doc_renamed;
    RequestCanvasFocusSignal m_sig_request_canvas_focus;
    GuideSelectedSignal              m_sig_guide_selected;
    GuideSelectionChangedSignal      m_sig_guide_selection_changed;
    GuideLayerChangedSignal          m_sig_guide_layer_changed;
    GuideConstructRequestedSignal    m_sig_guide_construct_requested;
    RequestGradientEditSignal        m_sig_request_gradient_edit;

    // s207 m2: ColorPickerPopover is the app-wide singleton — accessed
    // via ColorPickerPopover::shared(). The earlier `m_color_popover`
    // member is gone; all Guide/Grid/Margin/object-fill swatches route
    // through the shared instance.
};

} // namespace Curvz
