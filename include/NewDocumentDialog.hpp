#pragma once
#include "CurvzDocument.hpp"
#include "CurvzEntry.hpp"
#include "TemplateLibrary.hpp"
#include "theme/Theme.hpp"
#include <atomic>
#include <functional>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/frame.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/notebook.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>
#include <gdkmm/pixbuf.h>
#include <glibmm/dispatcher.h>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// s208 m5 — forward-declare substrate DropDown to keep this header light.
// Full include in NewDocumentDialog.cpp.
namespace curvz::widgets { class DropDown; }

namespace Curvz {

// Modal "New Document" dialog.
//
// Presents two tabs:
//   1. Template — thumbnail picker; includes two synthetic built-ins
//                 ("Blank" and "Default") plus any user/system templates
//                 from TemplateLibrary.
//   2. Size     — Units + Size W/H + presets + ratio presets, mirroring
//                 the inspector's Dimensions section. Produces an empty
//                 document carrying the chosen CanvasModel (with render
//                 intent populated when the user picked a Size).
//
// Template is the default tab on show(). Picking a template or built-in
// locks the dialog into "template mode" — the Create button will produce
// a document cloned from the template's seed. Switching to the Size tab
// clears any template selection, and the Create button will produce an
// empty document with the Size tab's CanvasModel.
//
// s266 m1: the previous four-tab structure (Template / Pixel / Physical /
// Ratio) collapsed alongside the s265 inspector restructure. Quality, DPI,
// and the Mode trichotomy are gone — there is one Size driving intent and
// ratio, and Units chooses how Size is spelled. See
// resources/help/1-4-how-curvz-thinks-about-size.md.
//
// Callback signature:
//   void(std::unique_ptr<CurvzDocument> seed, std::string name)
//
// The seed doc has its filename cleared; the caller assigns a project-
// appropriate filename. Name is the user-entered display name (may be
// empty, in which case the caller should fall back to a default).

class NewDocumentDialog : public Gtk::Window {
public:
    // Callback receives:
    //   seed     — newly-built document, ownership transferred to caller.
    //   name     — user-entered display name (may be empty).
    //   theme_id — saved-theme picked from the dropdown, or nullopt for
    //              "No theme — use defaults". Caller is responsible for
    //              applying the theme to the seed before adding the doc
    //              to the project. (NDD doesn't apply itself because it
    //              doesn't own the project; keeping the apply outside
    //              keeps NDD focused on doc construction.)
    using Callback = std::function<void(std::unique_ptr<CurvzDocument>,
                                        std::string,
                                        std::optional<theme::ThemeId>)>;

    NewDocumentDialog();

    // Show modal, attached to parent. callback called on Create.
    //
    // available_themes is the project's saved theme list (typically
    // project->themes.user_tier()). When non-empty, a "Theme" dropdown
    // appears directly below the Name row offering "No theme" plus one
    // entry per saved theme. When empty, the row is hidden.
    //
    // motif tells the dialog which of each disk template's two PNG variants
    // (thumbnail-dark.png / thumbnail-light.png) to display, and is also
    // used to choose the cache filename when lazy-regenerating a missing
    // variant. workspace_r/g/b and artboard_r/g/b are the project's current
    // motif-resolved colours — used for built-in tile rendering AND passed
    // to the regen worker so newly-rendered PNGs match the active motif.
    // Cached at show() — the dialog is modal so the motif can't change
    // while it's open.
    //
    // Persistent selection: NDD remembers the last-picked theme id
    // across show() calls. On re-open, if the remembered id is still
    // present in available_themes, it's pre-selected; otherwise the
    // dropdown defaults back to "No theme".
    void show(Gtk::Window& parent,
              const std::vector<theme::Theme>& available_themes,
              templates::MotifTag motif,
              double workspace_r, double workspace_g, double workspace_b,
              double artboard_r,  double artboard_g,  double artboard_b,
              Callback cb);

private:
    // ── Tab builders ──────────────────────────────────────────────────────
    Gtk::Widget& build_template_tab();
    Gtk::Widget& build_size_tab();

    // Rebuild the Size tab's interior. Called from build_size_tab() at
    // construction and from any preset-click / Units-change handler that
    // needs the panel to re-render (mirrors the inspector's refresh()
    // pattern). Reads m_current (CanvasModel) as the source of truth and
    // writes back to it as the user interacts. Bumps m_build_gen so
    // stale signal lambdas captured before the rebuild bail out.
    void populate_size_tab();

    // ── Template-tab helpers ──────────────────────────────────────────────
    // Built-in synthetic entry kinds for the first two tiles. Tracked
    // separately from on-disk TemplateEntry so we can render thumbnails
    // synthetically (no PNG).
    enum class BuiltIn { None, Blank, Default };

    void populate_template_grid();        // called from show()
    void select_builtin(BuiltIn kind);
    void select_template(int disk_index); // index into m_disk_templates
    void clear_template_selection();      // switch back to "nothing picked"

    // Visual highlight of the currently-picked tile (adds/removes the
    // `.selected` CSS class). Called by select_builtin / select_template
    // after they update the selection state. Cheap: iterates m_tile_* which
    // is bounded by the number of tiles in the current grid.
    void refresh_tile_selection();

    // Resolve what the "Default" tile and "nothing picked, fall through to
    // Default" paths should actually produce. If the user has set a default
    // template via Manage Templates (green dot), that choice wins:
    //   - user default = disk template    → ResolvedDefault::Disk, disk_idx set
    //   - user default = builtin/blank    → ResolvedDefault::Blank
    //   - user default = builtin/default  → ResolvedDefault::AppDefault
    //   - user default unset / unresolved → ResolvedDefault::AppDefault
    // Called by both select_builtin(Default) (for preview + selection state)
    // and on_create() (for final seed build) so the preview and the created
    // doc always agree.
    enum class ResolvedDefault { AppDefault, Blank, Disk };
    struct DefaultResolution {
        ResolvedDefault kind = ResolvedDefault::AppDefault;
        int disk_idx = -1;  // only valid when kind == Disk
    };
    DefaultResolution resolve_effective_default() const;

    // Build a freshly-allocated seed doc for a built-in kind.
    static std::unique_ptr<CurvzDocument> build_blank_seed();
    static std::unique_ptr<CurvzDocument> build_default_seed();

    // Reset the name field to "Untitled - <label>" if the user hasn't
    // typed something custom.
    void maybe_autofill_name(const std::string& label);

    // Draw a synthetic thumbnail for built-ins. `cr` is the Cairo context
    // from a DrawingArea draw func; `w`/`h` are the allocation. Uses the
    // motif colours cached at show() so preview matches the actual canvas
    // appearance in the active light/dark motif.
    void draw_builtin_thumb(const Cairo::RefPtr<Cairo::Context>& cr,
                            int w, int h, BuiltIn kind) const;

    // Draw a placeholder thumbnail for a disk template while its real PNG
    // is regenerating. Uses entry.aspect_ratio for the canvas shape and
    // entry.meta.{grid_divisions,margin_ratio,grid_offset_ratio} for the
    // overlay. Motif-aware (uses cached workspace+artboard colours). This
    // is what the user sees during the regen window — and also what stays
    // visible underneath the crossfade as the real PNG fades in.
    void paint_disk_placeholder(const Cairo::RefPtr<Cairo::Context>& cr,
                                int w, int h,
                                const templates::TemplateEntry& entry) const;

    // ── Disk-template thumb regen (m4) ────────────────────────────────────
    // A DiskTileState lives one-per-disk-tile while the dialog is shown.
    // It owns the DrawingArea pointer (non-owning — widget tree owns the
    // memory), the cached pixbuf once regen lands, and the alpha animation
    // state for crossfade-in. The state vector is rebuilt on every show()
    // alongside m_disk_templates so indices match; in-flight worker
    // callbacks compare against m_regen_generation to detect staleness.
    struct DiskTileState {
        Gtk::DrawingArea*           area = nullptr;
        Glib::RefPtr<Gdk::Pixbuf>   pb;             // null until regen lands
        // Crossfade alpha 0..1. Starts at 0 when pb is set; animates to 1 over
        // ~150ms via a Glib::signal_timeout. Once at 1, the timer disconnects.
        double                      alpha = 0.0;
        bool                        fading = false;
        // Generation at the time this tile's regen was kicked off. Used to
        // discard stale callbacks from a closed-then-reopened dialog cycle.
        uint64_t                    generation = 0;
    };

    // Schedule the background regen for every disk template that doesn't
    // already have a cached PNG for the current motif. Staggered by
    // ~k_regen_stagger_ms so they pop in sequentially rather than as a clump.
    // Called from populate_template_grid() at the end of disk-template setup.
    void kickoff_disk_regens();

    // Worker thread entry point for one tile. Calls
    // templates::ensure_thumb_for_motif(); on success, posts the resulting
    // path back to the UI thread via m_regen_dispatcher. All inputs are
    // passed by value (snapshot at kickoff time) so the worker never reads
    // shared dialog state while running — populate_template_grid()
    // reassigning m_disk_templates while a worker is mid-flight is safe.
    void regen_worker(int tile_index, uint64_t generation,
                      templates::TemplateEntry entry,
                      templates::MotifTag motif,
                      double workspace_r, double workspace_g, double workspace_b,
                      double artboard_r,  double artboard_g,  double artboard_b);

    // Drains m_regen_results on the UI thread (Glib::Dispatcher emit).
    // For each completed tile, loads the PNG into a Pixbuf and starts the
    // crossfade animation.
    void on_regen_dispatch();

    // Per-frame crossfade tick for a tile. Returns true to keep ticking,
    // false to disconnect. Bumps alpha and queue_draws the area.
    bool tick_crossfade(int tile_index);

    // ── Live update helpers ───────────────────────────────────────────────
    // Compute the Size W/H to display from m_current (CanvasModel),
    // mirroring PropertiesPanel::compute_size_wh. Priority order:
    //   1. intended_w/h if set, converted to display_unit if needed.
    //   2. Physical-mode fallback: phys_width/height in inches.
    //   3. Pixel/Ratio fallback: canvas_width/height at SVG-default 96dpi.
    void compute_size_wh(double& w_out, double& h_out) const;

    void refresh_preview();

    // ── Actions ───────────────────────────────────────────────────────────
    void on_create();
    void on_cancel();

    // ── Template tab widgets ──────────────────────────────────────────────
    Gtk::ScrolledWindow m_tpl_scroll;
    Gtk::Box            m_tpl_box{Gtk::Orientation::VERTICAL};
    // Populated fresh on every show() via populate_template_grid()
    std::vector<templates::TemplateEntry> m_disk_templates;
    BuiltIn             m_selected_builtin = BuiltIn::None;
    int                 m_selected_disk    = -1; // -1 if none

    // Tile widget pointers — populated by populate_template_grid() and
    // indexed so refresh_tile_selection() can apply/clear the `.selected`
    // CSS class on exactly one tile at a time. Pointers are owned by the
    // widget tree (Gtk::make_managed); cleared on grid teardown. Always
    // refreshed together with m_disk_templates so indices match.
    //
    // Tiles are Gtk::Frame (not Gtk::Button) so a single GestureClick
    // controller sees both press 1 (select) and press 2 (commit). Button's
    // internal click controller eats events in a way that breaks the
    // double-click streak — a "guest in GTK's home" lesson. CSS for the
    // selected tile uses the un-qualified `.template-tile.tile-selected`
    // selector so it works regardless of widget type.
    Gtk::Frame*               m_tile_blank   = nullptr;
    Gtk::Frame*               m_tile_default = nullptr;  // the top "Default"
                                                         // (or hijack) tile
    std::vector<Gtk::Frame*>  m_tile_disk;               // parallel to
                                                         // m_disk_templates

    // ── Size tab widgets (s266 m1 — collapses Pixel/Physical/Ratio) ──────
    //
    // The Size tab is rebuilt in place by populate_size_tab() — its
    // interior contents (Units, W/H spinners, presets, ratio presets)
    // are torn down and re-laid-out whenever the user picks a unit-
    // changing preset, mirroring how the inspector's Dimensions section
    // rebuilds on canvas_changed. m_size_box is the Notebook page; its
    // children are the only things populate_size_tab() touches.
    //
    // m_size_aspect_locked persists across rebuilds (it's a user
    // preference for this dialog session, like m_canvas_aspect_locked
    // on PropertiesPanel). Spinner pointers reset on every rebuild —
    // populate_size_tab() owns them.
    Gtk::Box  m_size_box{Gtk::Orientation::VERTICAL};
    bool      m_size_aspect_locked = true;

    // ── Build-generation counter ─────────────────────────────────────────
    // Bumped at the start of every populate_size_tab() rebuild. Signal
    // lambdas capture the generation at connect time and bail if it has
    // moved on, so stale callbacks from torn-down widgets can't write
    // to fresh widgets. Same idiom as PropertiesPanel::m_build_gen.
    uint32_t  m_build_gen = 0;

    // ── Name entry ────────────────────────────────────────────────────────
    CurvzEntry m_name_entry;

    // ── Theme row (S104 m1 follow-on) ─────────────────────────────────────
    //
    // Sits directly below the Name row when the project has saved themes.
    // Hidden entirely (m_theme_row->set_visible(false)) when the theme
    // list passed to show() is empty.
    //
    // m_theme_row owns the visibility toggle. m_theme_drop is a plain
    // Gtk::DropDown backed by a StringList rebuilt on each show().
    // m_theme_drop_ids is parallel to the dropdown rows: index 0 is
    // always the "No theme" sentinel (empty ThemeId), indices 1..N are
    // the available themes in the order they appear in the dropdown.
    //
    // m_remembered_theme_id is the persistent selection across show()
    // calls. Empty → "No theme". On show() we look up this id in the new
    // theme list; if found, pre-select that row; if missing (theme was
    // deleted between opens), fall back to "No theme" and clear the
    // remembered id.
    Gtk::Box*                       m_theme_row  = nullptr;
    // s208 m5: substrate. Forward-declared above; full include in .cpp.
    curvz::widgets::DropDown*       m_theme_drop = nullptr;
    std::vector<theme::ThemeId>     m_theme_drop_ids;     // parallel to dropdown
    theme::ThemeId                  m_remembered_theme_id;  // persistent

    // ── Preview area ──────────────────────────────────────────────────────
    Gtk::DrawingArea m_thumb;
    Gtk::Label       m_preview_label;

    // ── Bottom bar ────────────────────────────────────────────────────────
    Gtk::Button m_btn_cancel{"Cancel"};
    Gtk::Button m_btn_create{"Create Document"};

    // ── Notebook (tabs) ───────────────────────────────────────────────────
    Gtk::Notebook m_notebook;

    // ── State ─────────────────────────────────────────────────────────────
    CanvasModel m_current;    // live-updated as user adjusts controls
    Callback    m_callback;
    bool        m_updating        = false;  // signal re-entrancy guard
    bool        m_name_user_typed = false;  // true once the user edits the name
    std::string m_last_autofill;            // last auto-filled string, so we
                                            // know whether the entry still
                                            // holds our placeholder

    // ── Motif colour cache (set by show()) ────────────────────────────────
    // Workspace + artboard rgb taken from the project at show() time. Used
    // by draw_builtin_thumb and the dimension-preview thumb so template
    // tiles render in colours that match the actual canvas under the
    // current motif. Defaults are dark-motif greys so a fresh dialog
    // (pre-show) draws sensibly if anything peeks early.
    double m_workspace_r = 0.09, m_workspace_g = 0.09, m_workspace_b = 0.09;
    double m_artboard_r  = 0.157, m_artboard_g = 0.157, m_artboard_b = 0.157;
    templates::MotifTag m_motif = templates::MotifTag::Dark;

    // ── Disk-template thumb regen pipeline (m4) ──────────────────────────
    // One state record per disk-template tile, parallel to m_disk_templates
    // and m_tile_disk. Holds the cached Pixbuf and crossfade animation state.
    // Rebuilt on every show() so a closed-then-reopened dialog starts fresh.
    std::vector<DiskTileState>      m_disk_tile_state;

    // Generation counter. Bumped on every show(). Worker callbacks tag
    // themselves with the generation at kickoff time and discard their
    // result if the generation has moved on (dialog closed and reopened).
    // The PNG cache write still happens — a stale callback's only side
    // effect is the file on disk, which is correct for next time.
    std::atomic<uint64_t>           m_regen_generation{0};

    // UI-thread dispatch from worker threads. Workers push completed tile
    // indices to m_regen_results (under m_regen_mutex) and call emit() on
    // the dispatcher. The UI thread drains m_regen_results in
    // on_regen_dispatch().
    Glib::Dispatcher                m_regen_dispatcher;
    std::mutex                      m_regen_mutex;
    struct RegenResult {
        int      tile_index;
        uint64_t generation;
        std::string out_path;  // empty on failure
    };
    std::queue<RegenResult>         m_regen_results;
    bool                            m_regen_dispatcher_connected = false;

    // Stagger between consecutive worker kickoffs (ms). Small enough to feel
    // continuous, large enough to read as a left-to-right fill rather than
    // a popcorn burst. ~80ms × 6 tiles = ~half-second total kickoff window.
    static constexpr int k_regen_stagger_ms = 80;

    // Crossfade duration (ms) and tick interval (ms). 150ms / 16ms ≈ 9 frames,
    // smooth at 60fps. Easy easing curve applied in tick_crossfade.
    static constexpr int k_crossfade_ms       = 150;
    static constexpr int k_crossfade_tick_ms  = 16;
};

} // namespace Curvz
