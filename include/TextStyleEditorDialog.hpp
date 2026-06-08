#pragma once
//
// TextStyleEditorDialog — s342 — modal-less field editor for a
// style::TextStyle (the named paragraph-style layer, text_formatting_design
// §6/§7).
//
// The text twin of StyleEditorDialog. Same lifecycle shape (s201 m1): a
// hide-on-close singleton owned by MainWindow, built once on the first show()
// via the m_built latch, re-populated from the working style on every
// subsequent show() via sync_from_working(). Non-modal so the Scripter window
// stays reachable. OK fires on_committed with the harvested TextStyle and
// closes; Cancel / X discard.
//
// What is genuinely DIFFERENT from StyleEditorDialog, and the reason this is a
// separate class rather than a reuse:
//
//   * The DELTA MODEL is the UI. A TextStyle's two halves (ParaFormat /
//     CharDefaults) are SPARSE — every field is std::optional<T>: set == "this
//     style pins this value", unset == "inherit from parent". So every field in
//     this dialog carries an inherit/override affordance:
//       - dropdown fields (family / bold / italic / align) get an "(inherit)"
//         item at index 0; selecting it leaves the optional unset.
//       - numeric / colour fields (size / leading / tracking / indents /
//         colour) get an "override" CheckButton; off == unset, and the greyed
//         control PREVIEWS the value the style would inherit (resolved up the
//         parent chain) so the user sees what they're inheriting.
//     harvest_into_working() reads the affordances back into the sparse halves;
//     a field whose override is off / whose dropdown sits on "(inherit)" is
//     left unset.
//
//   * INHERITANCE is exposed. An "Inherits from" parent DropDown lists every
//     library style EXCEPT the one being edited and its descendants (so the
//     user can't author a cycle; the resolve()'s visited-set guard is the
//     belt, this dropdown filter is the braces). Changing the parent re-resolves
//     the inherited previews on every unset field. Index 0 is "(none — root)"
//     -> empty parent_id.
//
// What is the SAME idiom as StyleEditorDialog:
//   * Identity row (name + category dropdown with the inline "+ New category…"
//     sentinel-reveals-an-entry flow).
//   * Mode::New / Edit / Duplicate driving the title and the on_ok clear-id
//     branch; the caller's on_committed closure builds the Add/Update command.
//
// DEFERRED (noted in the s342 handoff): the tabs field. A paragraph style's tab
// stops want the StyleBar's master-detail tab editor, not a field here; for now
// the dialog CARRIES the working style's tabs through untouched (an edit
// preserves them) and the StyleBar tab editor + redefine-from-paragraph remain
// the way to set them. Adding a tab sub-editor here is a follow-up.
//

#include "CurvzColorPicker.hpp"
#include "CurvzSpinButton.hpp"
#include "color/Color.hpp"
#include "style/TextStyle.hpp"
#include "widgets/DropDown.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <functional>
#include <string>
#include <vector>

namespace Curvz {

struct CanvasModel;
namespace style { class TextStyleLibrary; }

class TextStyleEditorDialog : public Gtk::Window {
public:
    enum class Mode {
        // Edit an existing user-tier text style in place. OK returns the
        // modified TextStyle with its id intact; the caller pushes
        // UpdateTextStyleCommand.
        Edit,
        // Create a new text style. The seed's header.id is ignored (the
        // library mints a txs_<uuid> on add). OK returns the value with empty
        // id; caller pushes AddTextStyleCommand.
        New,
        // Edit a copy of an app-tier style. Pre-fills from the app style with a
        // " copy" name; OK path is identical to New (AddTextStyleCommand). The
        // discriminant only drives the title bar.
        Duplicate,
    };

    using CommittedFn = std::function<void(style::TextStyle result)>;

    TextStyleEditorDialog();
    ~TextStyleEditorDialog() override = default;

    TextStyleEditorDialog(const TextStyleEditorDialog&) = delete;
    TextStyleEditorDialog& operator=(const TextStyleEditorDialog&) = delete;

    // parent          — transient-for; re-applied each show().
    // library         — the project's TextStyleLibrary, for the parent dropdown
    //                   options, the descendant cycle-filter, and resolving the
    //                   inherited previews on unset fields. Borrowed; must
    //                   outlive the open session. Rebound on every show().
    // canvas_model    — for the indent spins' doc-unit conversion. Optional
    //                   (nullptr = px-only). Borrowed.
    // user_categories — existing user category strings (for the category
    //                   dropdown); "(uncategorised)" + the sentinel are added
    //                   by the dialog.
    // mode            — see enum. Drives title + the on_ok clear-id branch.
    // initial         — starting TextStyle. Copied into the working buffer.
    // on_committed    — fires once on OK with the harvested TextStyle.
    void show(Gtk::Window& parent,
              const style::TextStyleLibrary& library,
              CanvasModel* canvas_model,
              const std::vector<std::string>& user_categories,
              Mode mode,
              style::TextStyle initial,
              CommittedFn on_committed);

private:
    // ── Build (once, via m_built) ─────────────────────────────────────────
    void build();
    Gtk::Box* make_row(Gtk::Box& root, const char* label, Gtk::Widget& control,
                       Gtk::CheckButton* override_check = nullptr);
    Gtk::Label* make_section_label(const char* text);

    // ── Action handlers ───────────────────────────────────────────────────
    void on_ok();
    void on_cancel();

    // Pull every affordance into m_working (leaving unset what the user left on
    // inherit). Called from on_ok.
    void harvest_into_working();

    // Re-populate every widget from m_working + m_mode + the category/parent
    // orders. Runs under m_syncing so handlers don't write back.
    void sync_from_working();

    // Recompute the parent dropdown's option list (all styles minus self +
    // descendants), rebuild the StringList, and select the working parent.
    void rebuild_parent_options();

    // Resolve the currently-selected parent up its chain and push the resolved
    // values into every override-gated control that is currently OFF (the
    // inherited preview). Called from sync, on parent change, and when an
    // override is toggled off.
    void apply_inherited_previews();

    // Helpers shared by sync + override-toggle: set a control's sensitivity and,
    // when turning a numeric/colour override OFF, reload the inherited preview.
    void set_size_override(bool on, bool reload_preview);
    void set_leading_override(bool on, bool reload_preview);
    void set_track_override(bool on, bool reload_preview);
    void set_indent_override(bool on, bool reload_preview);
    void set_colour_override(bool on, bool reload_preview);

    // ── State ──────────────────────────────────────────────────────────────
    bool                            m_built        = false;
    const style::TextStyleLibrary*  m_library      = nullptr;
    CanvasModel*                    m_canvas_model = nullptr;
    Mode                            m_mode         = Mode::Edit;
    style::TextStyle                m_working;
    CommittedFn                     m_on_committed;
    bool                            m_syncing      = false;

    // Category dropdown ordering (parallel to the dropdown's StringList):
    // index 0 == "" (uncategorised), middle == user categories, last == the
    // "+ New category…" sentinel.
    std::vector<std::string>        m_category_order;

    // Parent dropdown ordering (parallel to its StringList): index 0 == ""
    // (root / none), the rest are candidate parent ids.
    std::vector<style::TextStyleId> m_parent_order;

    // Live colour value tracked from the picker's signal_changed (the picker
    // exposes no getter; set_initial doesn't emit, so we mirror it on every
    // interactive edit and on the sync set_initial).
    color::Color                    m_colour_value = color::Color::black();

    // ── Widgets (persistent for re-render) ──────────────────────────────────
    // Identity
    Gtk::Entry*               m_name_entry         = nullptr;
    curvz::widgets::DropDown* m_category_dd        = nullptr;
    Gtk::Entry*               m_category_new_entry = nullptr;  // visible on sentinel
    curvz::widgets::DropDown* m_parent_dd          = nullptr;

    // Character defaults
    curvz::widgets::DropDown* m_family_dd          = nullptr;  // [0]=(inherit)
    std::vector<std::string>  m_family_order;                  // parallel; [0]=""
    Gtk::CheckButton*         m_size_ov            = nullptr;
    CurvzSpinButton*          m_size_sp            = nullptr;   // pt-locked
    curvz::widgets::DropDown* m_bold_dd            = nullptr;   // Inherit/Off/On
    curvz::widgets::DropDown* m_italic_dd          = nullptr;   // Inherit/Off/On
    Gtk::CheckButton*         m_track_ov           = nullptr;
    CurvzSpinButton*          m_track_sp           = nullptr;   // pt-locked
    Gtk::CheckButton*         m_colour_ov          = nullptr;
    Gtk::CheckButton*         m_colour_none        = nullptr;   // None vs solid
    Gtk::MenuButton*          m_colour_btn         = nullptr;   // s342 — compact swatch
    Gtk::DrawingArea*         m_colour_swatch      = nullptr;   // swatch face
    CurvzColorPicker*         m_colour_picker      = nullptr;   // lives in the popover

    // Paragraph format
    curvz::widgets::DropDown* m_align_dd           = nullptr;   // Inherit/L/C/R/J
    Gtk::CheckButton*         m_leading_ov         = nullptr;
    CurvzSpinButton*          m_leading_sp         = nullptr;   // pt-locked; 0=auto
    Gtk::CheckButton*         m_indent_ov          = nullptr;
    CurvzSpinButton*          m_indent_left_sp     = nullptr;   // doc units
    CurvzSpinButton*          m_indent_right_sp    = nullptr;
    CurvzSpinButton*          m_indent_first_sp    = nullptr;

    // Buttons
    Gtk::Button*              m_btn_ok             = nullptr;
    Gtk::Button*              m_btn_cancel         = nullptr;
};

} // namespace Curvz
