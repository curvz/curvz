#pragma once
#include "CurvzDocument.hpp"
#include "CurvzEntry.hpp"
#include "TemplateLibrary.hpp"
#include "theme/Theme.hpp"
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
#include <gtkmm/scale.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Curvz {

// Modal "New Document" dialog.
//
// Presents four tabs:
//   1. Template         — thumbnail picker; includes two synthetic built-ins
//                         ("Blank" and "Default") plus any user/system
//                         templates from TemplateLibrary.
//   2. Pixel            — raw pixel dimensions
//   3. Physical         — inches / mm / cm + DPI
//   4. Ratio / Quality  — aspect ratio + unit count on short axis
//
// Template is the default tab on show(). Picking a template or built-in
// locks the dialog into "template mode" — the Create button will produce
// a document cloned from the template's seed. The other three tabs produce
// an empty document with the tab's CanvasModel.
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
    // Persistent selection: NDD remembers the last-picked theme id
    // across show() calls. On re-open, if the remembered id is still
    // present in available_themes, it's pre-selected; otherwise the
    // dropdown defaults back to "No theme".
    void show(Gtk::Window& parent,
              const std::vector<theme::Theme>& available_themes,
              Callback cb);

private:
    // ── Tab builders ──────────────────────────────────────────────────────
    Gtk::Widget& build_template_tab();
    Gtk::Widget& build_pixel_tab();
    Gtk::Widget& build_physical_tab();
    Gtk::Widget& build_ratio_tab();

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
    // from a DrawingArea draw func; `w`/`h` are the allocation.
    static void draw_builtin_thumb(const Cairo::RefPtr<Cairo::Context>& cr,
                                   int w, int h, BuiltIn kind);

    // ── Live update helpers ───────────────────────────────────────────────
    void update_from_pixel();
    void update_from_physical();
    void update_from_ratio();
    void refresh_preview();

    CanvasModel compute_pixel()    const;
    CanvasModel compute_physical() const;
    CanvasModel compute_ratio()    const;

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

    // ── Pixel tab widgets ─────────────────────────────────────────────────
    Gtk::Box        m_pixel_box{Gtk::Orientation::VERTICAL};
    Gtk::SpinButton m_px_w;
    Gtk::SpinButton m_px_h;

    // ── Physical tab widgets ──────────────────────────────────────────────
    Gtk::Box        m_phys_box{Gtk::Orientation::VERTICAL};
    Gtk::SpinButton m_phys_w;
    Gtk::SpinButton m_phys_h;
    Gtk::DropDown   m_phys_unit;
    Gtk::DropDown   m_phys_dpi;

    // ── Ratio/Quality tab widgets ─────────────────────────────────────────
    Gtk::Box        m_ratio_box{Gtk::Orientation::VERTICAL};
    Gtk::SpinButton m_ratio_w;
    Gtk::SpinButton m_ratio_h;
    Gtk::Scale      m_quality_slider;
    Gtk::SpinButton m_quality_spin;

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
    Gtk::DropDown*                  m_theme_drop = nullptr;
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
};

} // namespace Curvz
