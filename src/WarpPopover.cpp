#include "WarpPopover.hpp"
#include "AppPreferences.hpp"
#include "curvz_utils.hpp"
#include "widgets/Button.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#include <gtkmm/adjustment.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>

namespace Curvz {

WarpPopover::WarpPopover() {
    curvz::utils::set_name(*this, "pop_wrp", "warp_popover_root");
    set_position(Gtk::PositionType::RIGHT);
    // s155 fix: autohide OFF. The original (s154 m4a) used autohide=true
    // for outside-click dismissal, but the embedded Gtk::DropDown
    // breaks the outer popover's grab once the dropdown's internal
    // popover dismisses. Mirroring StepRepeatPopover's shape: explicit
    // Done button + Esc key controller for dismissal, no autohide.
    set_autohide(false);
    build_form();

    // Esc dismisses the popover. Mirrors StepRepeatPopover's pattern.
    auto key = Gtk::EventControllerKey::create();
    key->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) -> bool {
            if (keyval == GDK_KEY_Escape) {
                popdown();
                return true;
            }
            return false;
        }, true);
    add_controller(key);
}

void WarpPopover::build_form() {
    m_outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    m_outer->set_spacing(6);
    m_outer->set_margin_top(10);
    m_outer->set_margin_bottom(10);
    m_outer->set_margin_start(12);
    m_outer->set_margin_end(12);

    // Title
    auto* title = Gtk::make_managed<Gtk::Label>("Warp Defaults");
    title->add_css_class("tb-pop-title");
    title->set_xalign(0.0f);
    m_outer->append(*title);

    // Helper to make a label + control row matching the inspector's
    // shape (label-on-left, control-on-right, hexpand label).
    auto make_row = [](const char* label_text, Gtk::Widget& ctl) -> Gtk::Box* {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        row->set_spacing(8);
        auto* lbl = Gtk::make_managed<Gtk::Label>(label_text);
        lbl->set_xalign(0.0f);
        lbl->set_hexpand(true);
        lbl->add_css_class("tb-pop-label");
        row->append(*lbl);
        row->append(ctl);
        return row;
    };

    // ── Top anchors (2..4 spin) ──────────────────────────────────────────
    {
        auto adj = Gtk::Adjustment::create(2.0, 2.0, 4.0, 1.0, 1.0, 0.0);
        m_top_spin = Gtk::make_managed<curvz::widgets::SpinButton>(
            "pop_wrp_tn", adj, 1.0, 0);
        curvz::utils::set_name(m_top_spin, "pop_wrp_tn",
                               "warp_popover_top_count_spin");
        m_top_spin->set_valign(Gtk::Align::CENTER);
        m_top_spin->set_width_chars(4);
        m_top_spin->set_tooltip_text(
            "Default top-envelope anchor count for new Warps.\n"
            "Range 2–4. Wave preset auto-bumps to 3 if needed.");
        m_outer->append(*make_row("Top anchors", *m_top_spin));

        m_top_spin->signal_value_changed().connect([this]() {
            if (m_loading) return;
            int n = static_cast<int>(std::lround(m_top_spin->get_value()));
            AppPreferences::instance().set_warp_default_top_count(n);
        });
    }

    // ── Bottom anchors (2..4 spin) ───────────────────────────────────────
    {
        auto adj = Gtk::Adjustment::create(2.0, 2.0, 4.0, 1.0, 1.0, 0.0);
        m_bot_spin = Gtk::make_managed<curvz::widgets::SpinButton>(
            "pop_wrp_bn", adj, 1.0, 0);
        curvz::utils::set_name(m_bot_spin, "pop_wrp_bn",
                               "warp_popover_bot_count_spin");
        m_bot_spin->set_valign(Gtk::Align::CENTER);
        m_bot_spin->set_width_chars(4);
        m_bot_spin->set_tooltip_text(
            "Default bottom-envelope anchor count for new Warps.\n"
            "Range 2–4. Wave preset auto-bumps to 3 if needed.");
        m_outer->append(*make_row("Bottom anchors", *m_bot_spin));

        m_bot_spin->signal_value_changed().connect([this]() {
            if (m_loading) return;
            int n = static_cast<int>(std::lround(m_bot_spin->get_value()));
            AppPreferences::instance().set_warp_default_bot_count(n);
        });
    }

    // ── Preset dropdown (8 entries, no "(Custom)") ───────────────────────
    // The dropdown's internal popover used to break the outer popover's
    // autohide grab — fixed at the constructor level by autohide=false
    // (see s155 note in the ctor and class comment). Dropdown is the
    // natural control for "pick one from a labelled list" and is
    // preserved here.
    {
        std::vector<Glib::ustring> preset_strs;
        const char* const* names = curvz::utils::warp_presets::preset_names();
        for (int i = 0; i < curvz::utils::warp_presets::PRESET_COUNT; ++i) {
            preset_strs.push_back(names[i]);
        }
        auto preset_model = Gtk::StringList::create(preset_strs);
        m_preset_dd = Gtk::make_managed<curvz::widgets::DropDown>(
            "pop_wrp_pr", preset_model);
        curvz::utils::set_name(m_preset_dd, "pop_wrp_pr",
                               "warp_popover_preset_dd");
        m_preset_dd->set_valign(Gtk::Align::CENTER);
        m_preset_dd->set_tooltip_text(
            "Default envelope shape for new Warps. The just-created Warp's\n"
            "Object ▸ Warp section can switch presets per-instance.");
        m_outer->append(*make_row("Preset", *m_preset_dd));

        m_preset_dd->property_selected().signal_changed().connect([this]() {
            if (m_loading) return;
            int idx = static_cast<int>(m_preset_dd->get_selected());
            AppPreferences::instance().set_warp_default_preset(idx);
        });
    }

    // ── Quality slider (1..16) ───────────────────────────────────────────
    {
        auto adj = Gtk::Adjustment::create(4.0, 1.0, 16.0, 1.0, 1.0, 0.0);
        m_quality_sc = Gtk::make_managed<curvz::widgets::Scale>(
            "pop_wrp_q", adj, Gtk::Orientation::HORIZONTAL);
        curvz::utils::set_name(m_quality_sc, "pop_wrp_q",
                               "warp_popover_quality_scale");
        m_quality_sc->set_hexpand(true);
        m_quality_sc->set_draw_value(true);
        m_quality_sc->set_digits(0);
        m_quality_sc->set_value_pos(Gtk::PositionType::RIGHT);
        m_quality_sc->set_tooltip_text(
            "Default subdivision density for new Warps. Range 1–16; higher\n"
            "values produce smoother warped curves at greater cost.");
        // Quality gets its own label-row with the slider taking the
        // hexpand. Width-constrained popover means the slider can't
        // really stretch much, but at least it's the wide one.
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        row->set_spacing(8);
        auto* lbl = Gtk::make_managed<Gtk::Label>("Quality");
        lbl->set_xalign(0.0f);
        lbl->add_css_class("tb-pop-label");
        row->append(*lbl);
        row->append(*m_quality_sc);
        m_outer->append(*row);

        adj->signal_value_changed().connect([this, adj]() {
            if (m_loading) return;
            int q = static_cast<int>(std::lround(adj->get_value()));
            AppPreferences::instance().set_warp_default_quality(q);
        });
    }

    // ── Done button ──────────────────────────────────────────────────────
    // s155 fix: explicit dismissal control, since autohide=false. The
    // popover is a write-on-change defaults editor, so there's nothing
    // to "apply" — the button is dismissal only, hence "Done" rather
    // than "OK"/"Apply". Esc also dismisses (via the key controller in
    // the ctor). Right-aligned to follow the Affinity/Illustrator
    // dismissal-on-the-right convention.
    {
        auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        btn_row->set_spacing(8);
        btn_row->set_margin_top(4);
        btn_row->set_halign(Gtk::Align::END);

        auto* done_btn = Gtk::make_managed<curvz::widgets::Button>(
            "pop_wrp_done", "Done");
        curvz::utils::set_name(done_btn, "pop_wrp_done",
                               "warp_popover_done_btn");
        done_btn->signal_clicked().connect([this]() { popdown(); });
        btn_row->append(*done_btn);

        m_outer->append(*btn_row);
    }

    set_child(*m_outer);
}

void WarpPopover::refresh_from_prefs() {
    // Re-entrancy guard: programmatic value writes should NOT trigger
    // the on-change handlers (which would write the same value back to
    // AppPreferences uselessly, and potentially fight a concurrent
    // edit from the inspector subsection if the user has both visible).
    m_loading = true;
    auto& p = AppPreferences::instance();
    if (m_top_spin) {
        m_top_spin->set_value(double(std::clamp(p.warp_default_top_count(), 2, 4)));
    }
    if (m_bot_spin) {
        m_bot_spin->set_value(double(std::clamp(p.warp_default_bot_count(), 2, 4)));
    }
    if (m_preset_dd) {
        int cur = std::clamp(p.warp_default_preset(), 0,
                             curvz::utils::warp_presets::PRESET_COUNT - 1);
        m_preset_dd->set_selected(static_cast<guint>(cur));
    }
    if (m_quality_sc) {
        m_quality_sc->set_value(double(std::clamp(p.warp_default_quality(), 1, 16)));
    }
    m_loading = false;
}

void WarpPopover::show(Gtk::Widget& anchor) {
    refresh_from_prefs();
    if (get_parent() != &anchor) {
        if (get_parent()) unparent();
        set_parent(anchor);
    }
    popup();
}

} // namespace Curvz
