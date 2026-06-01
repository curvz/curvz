#include "Canvas.hpp"
#include "CommandHistory.hpp"
#include "CoordConvert.hpp"  // s192 m2 — doc<->display helpers for guide dialog
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"  // s116 m6 — m_project field reads workspace appearance
#include "CurvzSpinButton.hpp"
#include "MacroSystem.hpp"
#include "SvgParser.hpp"
#include "TextCursor.hpp"   // s301 m1c — full type needed for unique_ptr dtor + dispatch
#include <glibmm/main.h>     // s301 m1c — Glib::signal_timeout for caret blink
#include "widgets/RefPointPicker.hpp"  // s204 m4 — pivot right-click picker
#include "widgets/Button.hpp"  // s209 m5: unregistered substrate Button (spiral Apply, blend Rebuild popovers)
#include "curvz_utils.hpp"  // S97 m2 — box_blur_argb32 for drop-shadow render
#include "color/SwatchLibrary.hpp"  // set_swatch_library + apply_swatch_to_selection
#include "color/FillStyleInterop.hpp"  // to_fillstyle — live-recolour walk (s70 M3)
#include "style/StyleInterop.hpp"  // mutate_appearance funnel for user-driven fill/stroke writes
#include "style/StyleLibrary.hpp"  // set_style_library + signal_style_changed (S78 m3d)
#include <filesystem>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/clipboard.h>
#include <gdkmm/contentprovider.h>
#include <gdkmm/pixbuf.h>
#include <gdkmm/rectangle.h>
#include <giomm/menu.h>            // s162 m3: object-world context menu
#include <giomm/simpleaction.h>    // s162 m3 — local "save to library" action
#include <giomm/simpleactiongroup.h>// s162 m3
#include <gtkmm/alertdialog.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/grid.h>
#include <gtkmm/popover.h>
#include <gtkmm/popovermenu.h>     // s162 m3 — object-world context menu
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/window.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-font.h>
#include <sstream>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>  // s125 m1c — uint8_t for PNG IHDR parser
#include <cstring>  // s125 m1c — std::memcmp for PNG signature check
#include <ctime>    // s125 m1c — strftime/localtime for mtime in info dialog
#include <fstream>  // s125 m1c — std::ifstream for IHDR parser
#include <functional>
#include <random>  // s259 — std::mt19937 for Welzl shuffle in selection_true_center
#include <glib.h> // g_uuid_string_random via generate_internal_id()
#include <glibmm/main.h>
#include <gtkmm/gestureclick.h>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Canvas_internal.hpp"

namespace Curvz {

// ─── s162 m3: object-world context menu builder ───────────────────────────────
// Builds a Gio::Menu from the current ObjectActions mask. Sections appear only
// when at least one of their bits is set, so the menu has no greyed entries —
// what's visible is what's currently doable. Section dividers (the visual gap
// between groups in PopoverMenu) come from the Gio::Menu section append idiom.
//
// Action references use existing MainWindow `win.*` action names (cut, copy,
// paste, group-make, bool-union, …). The exception is "Save to Library…",
// which is wired locally via the `ctx` action group set up in the right-click
// handler — it has no MainWindow action because its target signal lives on
// Canvas.
//
// Section order follows Affinity/Illustrator convention:
//   1. Lifecycle  (Cut/Copy/Paste/Duplicate/Delete)
//   2. Structure  (Group/Ungroup/Compound/Release)
//   3. Boolean    (Union/Subtract/Intersect)
//   4. Arrange    (Bring Front/Forward, Send Backward/Back)
//   4.5 Move to layer ▸ (s275 m12 — cross-parent move; pairs with Arrange's
//                  z-order-within-parent)
//   4.7 Translate… (s205 m4 — Move/Scale/Rotate/Skew hub)
//   5. Effects    (Make/Edit/Release Warp+Blend, Release Clip, Rebuild
//                  Blend Steps, Flatten, Expand Stroke, Offset Path,
//                  Convert to Path)
//   6. Library    (Save to Library…) — always present
//
// Lock/Unlock/Hide/Show (VisAction) and Align/Distribute are intentionally
// omitted from the right-click menu in this ship: visibility belongs in the
// inspector visibility row per the s145 "in your face" rule, and align/distribute
// already have a dedicated toolbar group plus inspector section. Both are
// trivial follow-ups if Scott wants them later.
//
// s275 m12 — three additions bring this menu to parity with the layers
// panel row right-click menu (s274 m11):
//   • Move to layer ▸ — built inline from m_doc + m_selection rather than
//     an ObjectActions bit. Same logic as LayersPanel; both call sites
//     use curvz::utils::is_ordinary_target_layer + layer_display_name.
//     Gated on every selection member being top-level.
//   • Release Clip — uses the existing win.clip-release action (Ctrl+
//     Alt+7). Inline-gated because SelectionInfo doesn't carry an
//     any_clip flag (ClipGroup falls through `default:` in the kind
//     switch; deeper threading is a separate refactor).
//   • Rebuild Blend Steps — uses a local ctx.rebuild-blend action that
//     calls Canvas::rebuild_blend on the lone Blend, mirroring the
//     LayersPanel m11 wiring.
//
// `doc` and `selection` are passed in for the inline-gating work. They
// may be null/empty — the function handles both cases (sections just
// stay empty and get filtered out).
static Glib::RefPtr<Gio::Menu>
build_object_context_menu(const ObjectActions &oa,
                          const CurvzDocument *doc,
                          const std::vector<SceneNode *> &selection) {
  auto menu = Gio::Menu::create();

  // Helper: build a section, append non-empty sections to `menu`. Appending
  // a Gio::Menu as a section produces a visual divider between groups in the
  // resulting PopoverMenu — the canonical idiom across LibraryPanel,
  // DocumentGallery, MainWindow's main menubar.
  auto append_section_if_nonempty = [&menu](Glib::RefPtr<Gio::Menu> section) {
    if (section->get_n_items() > 0)
      menu->append_section(/*label=*/"", section);
  };

  // s165 m2 (continued): inline helper to append an item with an optional
  // accelerator hint. PopoverMenu renders the "accel" attribute as a right-
  // aligned grey label (system-styled). Standard accel format: "<Primary>x"
  // for Ctrl+X, "<Primary><Shift>u" for Ctrl+Shift+U, "Delete" for the bare
  // Delete key, "<Primary>Up" for Ctrl+Up, etc.
  //
  // Important: accel strings are DISPLAY-ONLY in this codebase. Per the
  // project's hotkey convention, every shortcut is dispatched in the
  // CAPTURE-phase key controller in MainWindow_bindings.cpp; GTK accelerator
  // dispatch is unreachable because CAPTURE claims events first. We use the
  // "accel" attribute purely as a label-rendering mechanism. The accel
  // strings here MUST stay in sync with the controller — when a hotkey
  // changes, change both sites or the menu will lie about the shortcut.
  auto add = [](Glib::RefPtr<Gio::Menu> &sec, const char *label,
                const char *action, const char *accel = nullptr) {
    auto item = Gio::MenuItem::create(label, action);
    if (accel)
      item->set_attribute_value(
          "accel", Glib::Variant<Glib::ustring>::create(accel));
    sec->append_item(item);
  };

  // Mask reads use the any(mask & Bit) idiom from SelectionContext.hpp's
  // CURVZ_BITWISE_ENUM macro — preserves the enum-class type and reads
  // declaratively as "is this bit set?".

  // ── 1. Lifecycle ─────────────────────────────────────────────────────────
  {
    auto sec = Gio::Menu::create();
    if (any(oa.life & LifeAction::Cut))
      add(sec, "Cut",       "win.cut",              "<Primary>x");
    if (any(oa.life & LifeAction::Copy))
      add(sec, "Copy",      "win.copy",             "<Primary>c");
    if (any(oa.life & LifeAction::Paste))
      add(sec, "Paste",     "win.paste",            "<Primary>v");
    if (any(oa.life & LifeAction::Duplicate))
      add(sec, "Duplicate", "win.duplicate",        "<Primary>d");
    // s181: Duplicate in Place sits under Duplicate. Same eligibility as
    // Duplicate (zero-offset variant), so gated on the same LifeAction bit
    // rather than introducing a separate flag — one source of truth for
    // "this selection can be duplicated." Renamed from "Clone" because
    // that name promised a source/instance link the operation never had;
    // see the s174 design-debt note in the handoff.
    if (any(oa.life & LifeAction::Duplicate))
      add(sec, "Duplicate in Place", "win.duplicate-in-place", "<Alt>d");
    if (any(oa.life & LifeAction::Delete))
      add(sec, "Delete",    "win.delete-selected",  "Delete");
    append_section_if_nonempty(sec);
  }

  // ── 1.5. Reset to Rectangle (s320 m2) ────────────────────────────────────
  // TextBox/Mgr only: undo accumulated rotation (and, later, skew/reshape),
  // returning the box to its canonical upright rectangle. A destructive
  // one-shot verb, so it lives as a menu item per the "in your face" rule
  // (no persistent inspector control needed). Inline-gated rather than via
  // an ObjectActions bit: eligibility is purely "single TextBox selected",
  // which the selection vector answers directly.
  if (selection.size() == 1 && selection[0] &&
      selection[0]->is_text_box_mgr()) {
    auto sec = Gio::Menu::create();
    add(sec, "Reset to Rectangle", "win.reset-transform");
    append_section_if_nonempty(sec);
  }

  // ── 2. Structure ─────────────────────────────────────────────────────────
  {
    auto sec = Gio::Menu::create();
    if (any(oa.struct_ & StructAction::MakeGroup))
      add(sec, "Group",            "win.group-make",     "<Primary>g");
    if (any(oa.struct_ & StructAction::Ungroup))
      add(sec, "Ungroup",          "win.group-release",  "<Primary><Shift>g");
    if (any(oa.struct_ & StructAction::MakeCompound))
      add(sec, "Make Compound",    "win.make-compound",  "<Primary>8");
    if (any(oa.struct_ & StructAction::ReleaseCompound))
      add(sec, "Release Compound", "win.split-compound", "<Primary><Shift>8");
    append_section_if_nonempty(sec);
  }

  // ── 3. Boolean (submenu) ─────────────────────────────────────────────────
  // s165 m2: Boolean ops collapsed into a "Boolean" submenu. Three items
  // share a coherent verb family and are uncommon in everyday workflow,
  // so nesting clears top-level real estate without hiding the verbs.
  {
    auto sub = Gio::Menu::create();
    if (any(oa.bool_ & BoolAction::Union))
      add(sub, "Union",     "win.bool-union",     "<Primary><Shift>u");
    if (any(oa.bool_ & BoolAction::Subtract))
      add(sub, "Subtract",  "win.bool-subtract",  "<Primary><Shift>e");
    if (any(oa.bool_ & BoolAction::Intersect))
      add(sub, "Intersect", "win.bool-intersect", "<Primary><Shift>i");
    if (sub->get_n_items() > 0) {
      auto sec = Gio::Menu::create();
      sec->append_submenu("Boolean", sub);
      menu->append_section("", sec);
    }
  }

  // ── 4. Arrange (submenu, z-order) ────────────────────────────────────────
  // s165 m2: Arrange collapsed to a submenu — four directional items
  // sharing a common verb. Standard idiom across vector editors (Affinity,
  // Illustrator).
  {
    auto sub = Gio::Menu::create();
    if (any(oa.layer & LayerAction::BringToFront))
      add(sub, "Bring to Front", "win.arrange-bring-front",
          "<Primary><Shift>Up");
    if (any(oa.layer & LayerAction::BringForward))
      add(sub, "Bring Forward",  "win.arrange-bring-forward",
          "<Primary>Up");
    if (any(oa.layer & LayerAction::SendBackward))
      add(sub, "Send Backward",  "win.arrange-send-backward",
          "<Primary>Down");
    if (any(oa.layer & LayerAction::SendToBack))
      add(sub, "Send to Back",   "win.arrange-send-back",
          "<Primary><Shift>Down");
    if (sub->get_n_items() > 0) {
      auto sec = Gio::Menu::create();
      sec->append_submenu("Arrange", sub);
      menu->append_section("", sec);
    }
  }

  // ── 4.5. Move to layer ▸ (s275 m12) ─────────────────────────────────────
  // Cross-parent reparent. Sits next to Arrange because the two are
  // siblings — Arrange moves an object within its parent layer (z-order);
  // Move to layer ▸ moves an object between parent layers. Same shape
  // of "do something to this object's parent slot."
  //
  // Inline-gated rather than ObjectActions-bit-gated, for two reasons:
  // (a) the eligibility depends on `doc->layers` and on each selection
  // member's parent, neither of which lives in ObjectActions today;
  // (b) the submenu items themselves are doc-driven (one per ordinary
  // layer), so the bit gate would only tell us "show the submenu"
  // anyway — we still have to walk layers to populate it. Doing both
  // here keeps the rule in one place.
  //
  // Eligibility (mirrors LayersPanel row right-click, s274 m11):
  //   • doc and selection both non-empty
  //   • every selection member is a top-level layer child (nested
  //     objects must be released from their container first via the
  //     Ungroup / Release Compound / Release Clip / Release Blend /
  //     Release Warp verbs above)
  //   • at least one ordinary unlocked target layer exists
  //     where not every selected object already lives there (no-op
  //     destinations are skipped)
  //
  // Action wiring: each item dispatches to ctx.move-to-layer-N (local
  // per-popup action group). The action's handler calls
  // Canvas::move_top_level_selection_to_layer(N). See the right-click
  // setup site for the per-popup ctx action group, and Canvas.cpp's
  // method for the iid-capture + push pattern.
  if (doc && !selection.empty()) {
    bool all_top_level = true;
    for (SceneNode *s : selection) {
      if (curvz::utils::find_object_parent(
              const_cast<CurvzDocument*>(doc), s) != nullptr) {
        all_top_level = false;
        break;
      }
    }
    if (all_top_level) {
      // Compute the layer every selection member is in, if they all share
      // one — that layer is the "home" we exclude from the target list
      // (moving everyone to where they already are is a no-op).
      int common_home = -1;
      bool same = true;
      for (SceneNode *s : selection) {
        int found = -1;
        for (int li = 0; li < (int)doc->layers.size(); ++li) {
          for (auto &c : doc->layers[li]->children) {
            if (c.get() == s) { found = li; break; }
          }
          if (found >= 0) break;
        }
        if (found < 0) { same = false; break; }
        if (common_home < 0) common_home = found;
        else if (common_home != found) { same = false; break; }
      }

      auto sub = Gio::Menu::create();
      for (int li = 0; li < (int)doc->layers.size(); ++li) {
        if (!curvz::utils::is_ordinary_target_layer(*doc->layers[li])) continue;
        if (same && li == common_home) continue;
        std::string action = "ctx.move-to-layer-" + std::to_string(li);
        std::string label  = curvz::utils::layer_display_name(*doc->layers[li], li);
        auto item = Gio::MenuItem::create(label, action);
        sub->append_item(item);
      }
      if (sub->get_n_items() > 0) {
        auto sec = Gio::Menu::create();
        sec->append_submenu("Move to layer", sub);
        menu->append_section("", sec);
      }
    }
  }

  // ── 4.7. Translate hub (s205 m4) ─────────────────────────────────────────
  // Always-present entry that opens the Translate hub dialog — one place
  // for Move / Scale / Rotate / Skew sharing the same refpt picker. Not
  // gated on ObjectAction bits; the dialog no-ops on empty / non-path
  // selection, mirroring Save to Library's "always-show, handler does the
  // pre-flight" idiom. Lives between Arrange and Effects because it sits
  // semantically between layer ops and effect ops — it's a do-this-to-
  // the-selection verb.
  {
    auto sec = Gio::Menu::create();
    add(sec, "Translate…", "win.translate-dialog");
    menu->append_section("", sec);
  }

  // ── 5. Effects (submenu) ─────────────────────────────────────────────────
  // s165 m2: Effects collapsed to a submenu. Up to 8 items spanning Warp,
  // Blend, Stroke, and primitive→path conversion — too many to leave at top
  // level and a coherent group. ConvertToPath maps to text-to-path (font-
  // glyph rasterisation) — the only primitive→path conversion currently
  // wired. EditBlend is a bit in EffectAction but no win.blend-edit
  // action exists; the verb is not surfaced anywhere in the menu today.
  // Make Warp and Make Blend (s162 m3) appear at the top of this section
  // so creation reads before lifecycle/release verbs.
  //
  // Most Effects items have no hotkey wired in the CAPTURE controller;
  // those omit the accel attribute. Expand Stroke (Ctrl+Shift+X) is the
  // exception.
  //
  // s275 m12: Release Clip and Rebuild Blend Steps added for parity with
  // the LayersPanel row right-click (s274 m11). Both inline-gated on the
  // selection rather than via ObjectActions bits — ClipGroup currently
  // falls through `default:` in SelectionInfo's kind switch (no any_clip
  // flag), and Rebuild Blend Steps is a cache-maintenance verb that
  // doesn't fit the Make/Edit/Release bit family. Release Clip uses the
  // existing win.clip-release action; Rebuild Blend Steps uses a local
  // ctx.rebuild-blend action (handler wired at the right-click site).
  {
    auto sub = Gio::Menu::create();
    if (any(oa.effect & EffectAction::MakeWarp))
      add(sub, "Make Warp",      "win.warp-make");
    if (any(oa.effect & EffectAction::MakeBlend))
      add(sub, "Make Blend",     "win.blend-make");
    if (any(oa.effect & EffectAction::EditWarp))
      add(sub, "Edit Warp",      "win.warp-edit");
    if (any(oa.effect & EffectAction::ReleaseWarp))
      add(sub, "Release Warp",   "win.warp-release");
    if (any(oa.effect & EffectAction::ReleaseBlend))
      add(sub, "Release Blend",  "win.blend-release");
    // Release Clip — single ClipGroup. Inline-gated, no action bit.
    if (selection.size() == 1 && selection[0]
        && selection[0]->type == SceneNode::Type::ClipGroup
        && !selection[0]->locked) {
      add(sub, "Release Clip",   "win.clip-release", "<Primary><Alt>7");
    }
    // Rebuild Blend Steps — single Blend. Inline-gated, no action bit.
    // Wired via local ctx.rebuild-blend at the right-click setup site;
    // the action calls Canvas::rebuild_blend on the lone Blend.
    if (selection.size() == 1 && selection[0]
        && selection[0]->type == SceneNode::Type::Blend
        && !selection[0]->locked) {
      add(sub, "Rebuild Blend Steps", "ctx.rebuild-blend");
    }
    if (any(oa.effect & EffectAction::Flatten))
      add(sub, "Flatten",        "win.warp-flatten");
    if (any(oa.effect & EffectAction::ExpandStroke))
      add(sub, "Expand Stroke",  "win.expand-stroke", "<Primary><Shift>x");
    if (any(oa.effect & EffectAction::OffsetPath))
      add(sub, "Offset Path…",   "win.offset-path",   "<Primary><Shift>o");
    if (any(oa.effect & EffectAction::ConvertToPath))
      add(sub, "Convert to Path", "win.text-to-path");
    if (sub->get_n_items() > 0) {
      auto sec = Gio::Menu::create();
      sec->append_submenu("Effects", sub);
      menu->append_section("", sec);
    }
  }

  // ── 6. Library — always present (selection-independent at this layer; the
  //    save-to-library handler in MainWindow does its own pre-flight on the
  //    current selection, and "Save current selection" reads naturally even
  //    when nothing else is doable on it).
  {
    auto sec = Gio::Menu::create();
    sec->append("Save to Library…", "ctx.save-to-library");
    menu->append_section("", sec);
  }

  return menu;
}

// ─── s165 m2: node-world context menu builder ─────────────────────────────────
// Builds a Gio::Menu of the J/M/K/A/B/C node-tool operations. Companion to
// build_object_context_menu but for the Node tool's right-click — the s162 m3
// object-context-menu work didn't ship a node equivalent, so right-clicking
// in the Node tool was falling through to the object branch and showing the
// wrong items.
//
// Item-to-key correspondence (matches Canvas::node_tool_key dispatch):
//   J — Join paths or Close/Open path        (NodeStructAction::Join is too
//                                              narrow; the J handler does
//                                              more than the policy bit
//                                              suggests, so this item is
//                                              shown unconditionally and
//                                              the handler shows a helpful
//                                              dialog if the configuration
//                                              isn't actionable)
//   B — Break path at node                   (same reasoning — always shown)
//   A — Make node Symmetric                  (gated on NodeKindAction::MakeSymmetric)
//   M — Make node Smooth                     (gated on NodeKindAction::MakeSmooth)
//   C — Make node Cusp                       (no policy bit exists; shown
//                                              alongside the other kind keys
//                                              when any is set)
//   K — Make node Corner                     (gated on NodeKindAction::MakeCorner)
//
// Action references use the local `nodectx` action group set up in the
// right-click handler — its actions invoke node_tool_key() directly, so the
// menu and the keypress paths share a single source of truth (blend-source
// rejection, atomic-undo wrapping, error dialogs, etc. all live in the key
// handler and the menu inherits all of it for free).
static Glib::RefPtr<Gio::Menu>
build_node_context_menu(const NodeActions &na) {
  auto menu = Gio::Menu::create();

  auto append_section_if_nonempty = [&menu](Glib::RefPtr<Gio::Menu> section) {
    if (section->get_n_items() > 0)
      menu->append_section(/*label=*/"", section);
  };

  // ── 1. Structural ops (Join, Break) ──────────────────────────────────────
  // Always shown. The key handlers self-validate and surface user-facing
  // dialogs when the configuration isn't actionable, so a "do nothing"
  // click is impossible — the user always sees feedback.
  //
  // Hotkey hints are baked into the label text as "(X)" because nodectx
  // actions are local to the popup and have no real Gio accel registration
  // to drive PopoverMenu's right-aligned accel rendering. The hotkey
  // strings here are display-only; the actual key dispatch lives in
  // Canvas::node_tool_key. Inline-parenthetical reads naturally in
  // PopoverMenu's proportional UI font without needing column alignment.
  {
    auto sec = Gio::Menu::create();
    sec->append("Join or Close   (J)", "nodectx.join");
    sec->append("Break Path   (B)",    "nodectx.break");
    append_section_if_nonempty(sec);
  }

  // ── 2. Node kind (Symmetric, Smooth, Cusp, Corner) ───────────────────────
  // Gated on NodeKindAction bits. compute_node_actions() sets these together
  // when any node is selected, so the section appears as a unit (or not at
  // all). Cusp has no policy bit but is included alongside its siblings.
  {
    auto sec = Gio::Menu::create();
    if (any(na.kind & NodeKindAction::MakeSymmetric))
      sec->append("Make Symmetric   (A)", "nodectx.type-symmetric");
    if (any(na.kind & NodeKindAction::MakeSmooth))
      sec->append("Make Smooth   (M)",    "nodectx.type-smooth");
    // Cusp has no NodeKindAction bit; tie its visibility to the same gate as
    // the other kind ops.
    if (any(na.kind & (NodeKindAction::MakeSymmetric |
                       NodeKindAction::MakeSmooth |
                       NodeKindAction::MakeCorner)))
      sec->append("Make Cusp   (C)",      "nodectx.type-cusp");
    if (any(na.kind & NodeKindAction::MakeCorner))
      sec->append("Make Corner   (K)",    "nodectx.type-corner");
    append_section_if_nonempty(sec);
  }

  return menu;
}


// ── Constructor
// ───────────────────────────────────────────────────────────────
Canvas::Canvas() {
  curvz::utils::set_name(*this, "cv", "canvas_drawing_area");
  set_expand(true);
  set_draw_func(sigc::mem_fun(*this, &Canvas::on_draw));
  set_focusable(true);

  // Scroll → pan (both axes); Ctrl+scroll → zoom
  m_scroll_ctrl = Gtk::EventControllerScroll::create();
  m_scroll_ctrl->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES |
                           Gtk::EventControllerScroll::Flags::KINETIC);
  m_scroll_ctrl->signal_scroll().connect(
      sigc::mem_fun(*this, &Canvas::on_scroll), false);
  add_controller(m_scroll_ctrl);

  // Pinch → zoom
  auto zoom_gest = Gtk::GestureZoom::create();
  zoom_gest->signal_scale_changed().connect([this, zoom_gest](double scale) {
    // scale is cumulative from gesture begin — compute delta from last frame
    double delta = scale / m_pinch_last_scale;
    m_pinch_last_scale = scale;
    // Zoom toward gesture centroid
    double cx = 0.0, cy = 0.0;
    zoom_gest->get_bounding_box_center(cx, cy);
    zoom_toward(cx, cy, delta);
  });
  zoom_gest->signal_begin().connect(
      [this](Gdk::EventSequence *) { m_pinch_last_scale = 1.0; });
  add_controller(zoom_gest);

  // Middle-drag → pan
  auto pan_drag = Gtk::GestureDrag::create();
  pan_drag->set_button(2);
  pan_drag->signal_drag_begin().connect(
      sigc::mem_fun(*this, &Canvas::on_pan_begin));
  pan_drag->signal_drag_update().connect(
      sigc::mem_fun(*this, &Canvas::on_pan_update));
  pan_drag->signal_drag_end().connect(
      sigc::mem_fun(*this, &Canvas::on_pan_end));
  add_controller(pan_drag);

  // Left-drag → draw shapes / move selection
  auto draw_drag = Gtk::GestureDrag::create();
  draw_drag->set_button(1);
  draw_drag->signal_drag_begin().connect([this, draw_drag](double x, double y) {
    // Read modifier state from the gesture's current event
    auto evt = draw_drag->get_current_event();
    if (evt) {
      auto state = evt->get_modifier_state();
      m_mod_alt = (state & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
      m_mod_shift =
          (state & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
      m_mod_ctrl =
          (state & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
    }
    on_draw_begin(x, y);
  });
  draw_drag->signal_drag_update().connect([this, draw_drag](double dx,
                                                            double dy) {
    auto evt = draw_drag->get_current_event();
    if (evt) {
      auto state = evt->get_modifier_state();
      m_mod_alt = (state & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
      m_mod_shift =
          (state & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
      m_mod_ctrl =
          (state & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
    }
    on_draw_update(dx, dy);
  });
  draw_drag->signal_drag_end().connect([this, draw_drag](double dx, double dy) {
    // Do NOT re-read modifier state from the drag-end event.
    // Button-release events on most platforms clear Alt before the event
    // is delivered, so get_modifier_state() here returns 0 even when the
    // user is holding Alt.  The correct value was already captured by the
    // last drag_update or on_motion call — leave m_mod_alt as-is.
    on_draw_end(dx, dy);
  });
  add_controller(draw_drag);

  // Double-click detector — re-enters text edit when a Text node is
  // double-clicked while the Selection tool is active.
  //
  // s301 m1f — Also enters edit when a TEXT BOUNDARY is double-clicked.
  // The boundary is the user-facing primitive in the unified container
  // model; clicking it (and seeing handles) is "select the frame";
  // double-clicking it is "edit the text inside this frame." The model
  // matches InDesign: single = arrange, double = edit. Find the bound
  // text node by walking the doc for a Text whose first
  // text_boundary_ids entry matches the clicked boundary's iid.
  auto dbl_click = Gtk::GestureClick::create();
  dbl_click->set_button(1);
  dbl_click->signal_pressed().connect([this, dbl_click](int n_press, double x,
                                                        double y) {
    // s326 m2c — Multi-click selection WHILE EDITING: 2 = word, 3 = line,
    //   4 = paragraph. Runs only when a cursor is active and the click is on
    //   the editing target. Deferred to idle because the GestureDrag press
    //   (on_select_begin -> place_caret_at) collapses the selection on the
    //   same press; the idle callback runs after that and re-establishes the
    //   granularity selection, robust to gesture firing order. byte_index_at
    //   is pure geometry (caret-independent), so re-mapping in idle is safe.
    if (m_text_cursor && m_text_editing && n_press >= 2 && n_press <= 4) {
      double mdx = 0.0, mdy = 0.0;
      screen_to_doc(x, y, mdx, mdy);
      SceneNode* hit = hit_test(mdx, mdy);
      SceneNode* editing_tb = find_text_box_for_text(m_text_editing);
      bool on_edit =
          (hit && editing_tb && hit == editing_tb) ||
          (hit == m_text_editing) ||
          (hit && hit == m_text_boundary_editing);
      if (on_edit) {
        int press = n_press;
        Glib::signal_idle().connect_once([this, mdx, mdy, press]() {
          if (!m_text_cursor) return;
          auto b = m_text_cursor->byte_index_at(mdx, mdy);
          if (!b) return;
          if (press == 2)      m_text_cursor->select_word_at(*b);
          else if (press == 3) m_text_cursor->select_line_at(*b);
          else                 m_text_cursor->select_paragraph_at(*b);
          m_text_select_dragging = false;  // don't let a stray drag fight it
          m_text_cursor->set_visible(true);
          queue_draw();
        });
        return;  // consumed; don't fall through to enter-edit handling
      }
    }

    if (n_press != 2)
      return;
    // Selection and Text tools both honour double-click → edit on a
    // textbox. Selection promotes to Text first; Text is already
    // there. Other tools ignore double-clicks (their own gestures
    // own the double-click vocabulary).
    if (m_tool != ActiveTool::Selection && m_tool != ActiveTool::Text)
      return;
    double dx, dy;
    screen_to_doc(x, y, dx, dy);

    // ── s319 — Double-click inside a PRESENTED OA's text area enters edit
    //   in the overflow region, caret at the clicked byte. Tested before
    //   member resolution: the panel floats above member space, so a click
    //   on it must not fall through to a member underneath. Establish the
    //   edit (begin_textbox_edit sets up the cursor + snapshot), then
    //   cross_into_overflow re-anchors the caret to the OA region (scroll
    //   resets to 0, so the click maps directly through byte_index_at).
    if (SceneNode* oa_mgr = overflow_hit_mgr(dx, dy)) {
      auto enter_oa = [this, oa_mgr, dx, dy]() {
        begin_textbox_edit(oa_mgr, /*have_point=*/false);
        if (cross_into_overflow(oa_mgr) && m_text_cursor) {
          if (auto b = m_text_cursor->byte_index_at(dx, dy)) {
            m_text_cursor->set_byte_index(*b);
            m_text_cursor->collapse_selection();
          }
        }
        queue_draw();
      };
      LOG_INFO("Canvas: DBLCLICK into OA text area → edit overflow region");
      if (m_tool == ActiveTool::Selection) {
        m_sig_request_tool.emit(ActiveTool::Text);
        Glib::signal_idle().connect_once(enter_oa);
      } else {
        enter_oa();
      }
      return;
    }

    // ── s318 — Edit-entry resolver. Single-click selection already
    //   resolves a TBM member via member_hit_test (reverse member order =
    //   topmost-wins, tests each member's boundary directly, NO object_bbox
    //   gate). Double-click now goes through the SAME door so the two
    //   gestures can't disagree, and so overlapping members resolve to the
    //   newest (top) box. A point that lands on no member's boundary is NOT
    //   a Mgr edit — the transparent gap of a TBM passes through (per the
    //   model: TBM area is just the union of real objects; empty space is
    //   click-through). hit_test's object_bbox union is no longer
    //   load-bearing for edit-entry.
    SceneNode* tb = nullptr;
    if (SceneNode* member_b = member_hit_test(dx, dy)) {
      SceneNode* owner = nullptr;
      if (find_textbox_member(member_b, &owner, /*out_view=*/nullptr) && owner)
        tb = owner;
    }
    LOG_INFO("Canvas: DBLCLICK doc=({:.2f},{:.2f}) tool={} member_resolver_mgr='{}'",
             dx, dy, (int)m_tool, tb ? tb->name : "(none)");

    // ── TextBox: double-click → edit, regardless of which of the
    //   two honouring tools we're in. The user model: muscle memory
    //   says "double-click a thing to drill in." For a textbox,
    //   drilling in means editing its text.
    //
    //   Selection tool: emit a tool switch to Text (text editing
    //     lives in the Text tool), then schedule begin_textbox_edit
    //     via signal_idle so the tool flip settles before edit
    //     state is set up. Same async pattern Case A below uses
    //     for legacy text re-entry.
    //
    //   Text tool: already in the right tool. Call the helper
    //     directly — no scheduling needed.
    if (tb) {
      LOG_INFO("Canvas: DBLCLICK → Mgr edit, tool={} (Selection schedules "
               "via idle; Text calls direct)", (int)m_tool);
      if (m_tool == ActiveTool::Selection) {
        m_sig_request_tool.emit(ActiveTool::Text);
        Glib::signal_idle().connect_once([this, tb, dx, dy]() {
          LOG_INFO("Canvas: DBLCLICK idle-fire → begin_textbox_edit");
          begin_textbox_edit(tb, /*have_point=*/true, dx, dy);
        });
      } else {
        begin_textbox_edit(tb, /*have_point=*/true, dx, dy);
      }
      return;
    }

    // Not on a TBM member → legacy paired-sibling text (bare Text node or
    //   boundary Path at the layer level), resolved via the general
    //   hit_test. These only fire in Selection tool; Text tool's own press
    //   path already handles legacy re-entry directly (double-firing here
    //   would conflict).
    if (m_tool != ActiveTool::Selection)
      return;
    SceneNode *hit = hit_test(dx, dy);
    LOG_INFO("Canvas: DBLCLICK legacy-path hit='{}' type={}",
             hit ? hit->name : "(null)", hit ? (int)hit->type : -1);
    if (!hit) return;

    // Case A: double-clicked the text node itself (legacy unbound or
    // bound). Enter edit via Text tool's on_text_begin.
    if (hit->is_text()) {
      m_sig_request_tool.emit(ActiveTool::Text);
      Glib::signal_idle().connect_once([this, x, y]() { on_text_begin(x, y); });
      return;
    }

    // Case B: double-clicked a path that's a text boundary. Find the
    // bound text node and begin canvas-cursor edit on it. Stay in
    // Selection tool — edit is an overlay, not a tool mode.
    if (hit->type == SceneNode::Type::Path && hit->path && m_doc) {
      SceneNode* bound_text = nullptr;
      for (auto& layer : m_doc->layers) {
        for (auto& c : layer->children) {
          if (c && c->is_text() && !c->text_boundary_ids.empty() &&
              c->text_boundary_ids.front() == hit->internal_id) {
            bound_text = c.get();
            break;
          }
        }
        if (bound_text) break;
      }
      if (bound_text) {
        m_text_editing = bound_text;
        m_text_boundary_editing = hit;
        m_text_is_new = false;
        m_text_snapshot = TextEditCommand::snapshot_before(project(), bound_text);
        m_text_has_snapshot = true;
        m_selected = hit;
        m_selection = {hit};
        notify_object_selection_changed();
        // (legacy Gtk::Entry was never shown for this bound path; no
        //  need to hide it here. Avoids pulling widgets/Entry.hpp into
        //  this TU just for an unreachable defensive hide.)
        begin_text_cursor_edit(bound_text, hit);
      }
    }
  });
  add_controller(dbl_click);

  // s308 m1 — Overflow indicator click. Sizer-pattern affordance: Canvas
  // paints a small symbolic glyph at the bottom-right of every
  // overflowing textbox boundary (see Canvas_draw.cpp), and this
  // GestureClick hit-tests it BEFORE any tool dispatch. On hit:
  // claim the sequence (so the underlying GestureDrag doesn't also
  // fire and start a marquee/move) and pop up the overflow inspector.
  //
  // capture-phase routing isn't required — GestureClick fires on press,
  // GestureDrag's drag_begin only fires once pointer crosses the drag
  // threshold AFTER a press. Claiming here defuses the drag for the
  // current sequence.
  //
  // s312 m2.3 — The canvas TextCursor and the popover's TextView are
  // two surfaces on the same buffer. Clicking the indicator during an
  // active canvas edit is a deliberate cross-boundary gesture: the user
  // wants to keep editing, just on the other surface. The cursor stays
  // alive (cross_back_to_canvas reactivates it when the user crosses
  // back); only focus moves. Removed the m_text_cursor early-return
  // that pre-s312 blocked clicks during an active edit.
  auto overflow_click = Gtk::GestureClick::create();
  overflow_click->set_button(1);
  overflow_click->signal_pressed().connect(
      [this, overflow_click](int n_press, double x, double y) {
        if (n_press != 1) return;
        if (!m_doc) return;
        // s316 m4a — If a bubble is presented, a click on its link button
        //   is the create-next-tb verb. Test it before the `!` toggle so
        //   the button (which sits over the bubble) wins.
        double doc_x = 0.0, doc_y = 0.0;
        screen_to_doc(x, y, doc_x, doc_y);
        if (SceneNode* link_mgr = hit_overflow_link(doc_x, doc_y)) {
          overflow_click->set_state(Gtk::EventSequenceState::CLAIMED);
          link_textbox_member(link_mgr);
          return;
        }
        SceneNode* hit_text = nullptr;
        SceneNode* hit_boundary = nullptr;
        if (check_overflow_hit(x, y, &hit_text, &hit_boundary)) {
          overflow_click->set_state(Gtk::EventSequenceState::CLAIMED);
          // s316 m3 — the `!` now toggles the custom canvas overflow
          //   region instead of opening the DepotWindow. The window is
          //   being retired; the overflow lives on the canvas.
          toggle_overflow_region(hit_text);
        }
      });
  add_controller(overflow_click);

  // Right-click context menu. Two branches by hit type:
  //   • Image → modal "Image Info" dialog with file/pixel/size details
  //     (the s124-era branch).
  //   • Anything else hit-testable (path, text, group, compound, ref,
  //     blend, warp, clip-group) → unified object context menu driven
  //     by SelectionContext, with inline-gated additions for Move to
  //     layer ▸ / Release Clip / Rebuild Blend Steps (s275 m12).
  // Empty-canvas right-click is intentionally a no-op (no document-level
  // verbs wired yet).
  //
  // History: through s274 the Blend case had a separate one-button
  // popover ("Rebuild Blend Steps" and nothing else) that pre-empted the
  // unified menu. s275 m12 folded that verb into the unified menu —
  // mirrors what s274 m11 did for the LayersPanel row right-click.
  auto rclick = Gtk::GestureClick::create();
  rclick->set_button(3);
  rclick->signal_pressed().connect([this](int, double x, double y) {
    if (!m_doc)
      return;

    // s317 — Live TextBoxMgr member right-click: "Delete text box". A miss
    //   falls through to the normal object menu.
    {
      double mdx = 0.0, mdy = 0.0;
      screen_to_doc(x, y, mdx, mdy);
      SceneNode* member_boundary = member_hit_test(mdx, mdy);
      if (member_boundary) {
        SceneNode* mm_mgr = nullptr;
        SceneNode* mm_view = nullptr;
        if (find_textbox_member(member_boundary, &mm_mgr, &mm_view) &&
            mm_mgr && mm_view) {
          auto menu = Gio::Menu::create();
          menu->append("Reset to Rectangle", "tbmemberctx.reset");
          menu->append("Reset Text Format", "tbmemberctx.text-reset");
          menu->append("Delete text box", "tbmemberctx.delete");

          auto ag = Gio::SimpleActionGroup::create();
          auto act = Gio::SimpleAction::create("delete");
          SceneNode* cap_mgr = mm_mgr;
          SceneNode* cap_view = mm_view;
          act->signal_activate().connect(
              [this, cap_mgr, cap_view](const Glib::VariantBase&) {
                Glib::signal_idle().connect_once([this, cap_mgr, cap_view]() {
                  // Liveness guard: the Mgr (a layer child) is checkable via
                  // is_node_alive. The view is nested deeper, so we let
                  // delete_textbox_member re-validate it (it looks the view
                  // up in the Mgr's child list and no-ops if it's gone).
                  if (is_node_alive(cap_mgr))
                    delete_textbox_member(cap_mgr, cap_view);
                });
              });
          ag->add_action(act);
          // s320 m2 — Reset to Rectangle on the same menu. Selects the Mgr
          //   (so the selection-based reset/rotate path acts on it) then
          //   un-rotates it back to its canonical rect. Deferred one idle
          //   like the delete sibling so the popover dismisses first.
          auto act_reset = Gio::SimpleAction::create("reset");
          SceneNode* cap_boundary = member_boundary;
          act_reset->signal_activate().connect(
              [this, cap_mgr, cap_boundary](const Glib::VariantBase&) {
                Glib::signal_idle().connect_once([this, cap_mgr, cap_boundary]() {
                  if (is_node_alive(cap_mgr)) {
                    set_selection_single(cap_mgr);
                    reset_textbox_transform(cap_boundary);
                  }
                });
              });
          ag->add_action(act_reset);

          // s331 — Reset Text Format: strip every per-run span and return the
          // Mgr's character style to defaults (content kept). Passes the Mgr
          // directly to reset_text_to_default, so no selection side-effect.
          // Deferred one idle like its siblings so the popover dismisses first.
          auto act_text_reset = Gio::SimpleAction::create("text-reset");
          act_text_reset->signal_activate().connect(
              [this, cap_mgr](const Glib::VariantBase&) {
                Glib::signal_idle().connect_once([this, cap_mgr]() {
                  if (is_node_alive(cap_mgr))
                    reset_text_to_default(cap_mgr);
                });
              });
          ag->add_action(act_text_reset);
          insert_action_group("tbmemberctx", ag);

          auto* popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
          popover->set_parent(*this);
          popover->set_has_arrow(false);
          Gdk::Rectangle rect((int)x, (int)y, 1, 1);
          popover->set_pointing_to(rect);
          popover->signal_closed().connect([popover]() {
            Glib::signal_idle().connect_once(
                [popover]() { popover->unparent(); });
          });
          popover->popup();
          return;
        }
      }
    }

    // TextOnPath tool intercepts right-click for detach
    if (m_tool == ActiveTool::TextOnPath) {
      on_top_rclick(x, y);
      return;
    }
    // s165 m2: Node tool right-click context menu.
    //
    // Independent of the object-world rclick branch below — the node menu's
    // verbs (Join/Break/Make-X) operate on m_node_selection / m_selected_node
    // and dispatch through Canvas::node_tool_key, NOT on the hit-tested
    // object. We require a primary path with an active node (or node
    // selection) to show anything; otherwise fall through to the object
    // branch so the user can still get the object menu while transiently in
    // Node tool.
    if (m_tool == ActiveTool::Node && m_selected &&
        m_selected->type == SceneNode::Type::Path && m_selected->path &&
        (m_selected_node >= 0 || !m_node_selection.empty())) {
      auto menu = build_node_context_menu(m_sel_ctx.node_actions());
      if (menu->get_n_items() == 0) {
        // No items would render — fall through to the object branch so the
        // right-click isn't a silent no-op.
      } else {
        // Local "nodectx" action group — each action invokes node_tool_key
        // with the corresponding keyval. Single source of truth: the key
        // handler does blend-source rejection, atomic-undo wrapping, and
        // user-facing error dialogs. The menu inherits all of it.
        auto ag = Gio::SimpleActionGroup::create();
        auto add = [&](const char *name, guint keyval) {
          auto act = Gio::SimpleAction::create(name);
          act->signal_activate().connect(
              [this, keyval](const Glib::VariantBase &) {
                // Defer one idle so the popover finishes dismissing before
                // the (potential) error dialog from node_tool_key opens.
                // Mirrors the s125 m1a / object-ctx defer pattern.
                Glib::signal_idle().connect_once([this, keyval]() {
                  node_tool_key(keyval, /*shift=*/false, /*ctrl=*/false,
                                /*alt=*/false);
                });
              });
          ag->add_action(act);
        };
        add("join",            GDK_KEY_j);
        add("break",           GDK_KEY_b);
        add("type-symmetric",  GDK_KEY_a);
        add("type-smooth",     GDK_KEY_m);
        add("type-cusp",       GDK_KEY_c);
        add("type-corner",     GDK_KEY_k);
        insert_action_group("nodectx", ag);

        auto *popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
        popover->set_parent(*this);
        popover->set_has_arrow(false);
        Gdk::Rectangle rect((int)x, (int)y, 1, 1);
        popover->set_pointing_to(rect);

        // Lifetime: same pattern as the object-world popover below — break
        // the popdown→destroy ordering race by unparenting from idle.
        popover->signal_closed().connect([popover]() {
          Glib::signal_idle().connect_once(
              [popover]() { popover->unparent(); });
        });

        popover->popup();
        return;
      }
    }

    // s191 m5 — Guide right-click: "Set position…" dialog.
    //
    // The hover hit-test (on_motion) keeps m_guide_hovered current; we
    // re-use it here. If the right-click landed within hover range of
    // a guide, route to the guide dialog before the object hit-test
    // runs — guides sit visually above the object layer and the user's
    // intent is to address the guide they can see under the cursor,
    // not the object behind it.
    //
    // Field shape follows the guide's geometry: each kind exposes
    // exactly the degrees of freedom it has. A horizontal guide has
    // one position (Y); a vertical has one (X); an angled guide has
    // an anchor point plus angle. No "set X on a horizontal" trap.
    //
    // Write path is the canonical one: capture before-state, mutate,
    // push GuideMoveCommand, emit signal_doc_changed. Matches the
    // drag's path at on_select_end in Canvas_input.cpp — same undo
    // command, same emit, same coalescing semantics. The inspector's
    // direct write path (no command) is a separate concern; this new
    // entry point lands on the undo stack from day one.
    if (m_guide_hovered) {
      auto *win = dynamic_cast<Gtk::Window *>(get_root());
      if (!win) return;  // no toplevel, can't show modal

      SceneNode *g = m_guide_hovered;

      // s191 m5 followup — refuse the dialog when the guide (or its
      // layer) is locked. The drag path at on_select_update already
      // swallows drag on a locked guide; the inspector spinners go
      // insensitive via edit_on = !layer_locked && !g->locked. Without
      // this check the right-click dialog would be the one edit path
      // that ignores locked state — silently inconsistent. An alert
      // is the right surface: silent no-op would have the user
      // re-clicking thinking the right-click didn't register.
      bool layer_locked = false;
      if (m_doc) {
        const SceneNode *gl = m_doc->guide_layer();
        if (gl) layer_locked = gl->locked;
      }
      if (g->locked || layer_locked) {
        curvz::utils::show_alert(
            *win, "Guide locked",
            layer_locked
                ? "The guide layer is locked. Unlock the layer to edit "
                  "guide positions."
                : "This guide is locked. Unlock it from the inspector "
                  "to edit its position.");
        return;
      }

      const bool is_h = g->guide_is_horizontal();
      const bool is_v = g->guide_is_vertical();
      // For undo capture — read once before showing the dialog so the
      // before-state is stable even if some other code path touches
      // the guide while the dialog is open (defensive; user input
      // shouldn't normally do this since the dialog is modal).
      const double bx = g->guide_x;
      const double by = g->guide_y;
      const double ba = g->guide_angle;
      const std::string giid = g->internal_id;

      std::vector<curvz::utils::FormField> fields;

      // s192 m2 — Pre-convert defaults to display space, then post-convert
      // OK values back to doc space. Inspector spin buttons run the same
      // pipeline via doc_to_display_x/y / display_to_doc_x/y in
      // CoordConvert.hpp; this dialog calls the same helpers so the two
      // edit surfaces agree byte-for-byte. Without this conversion the
      // dialog showed raw doc-space Y (Y-down, top-left origin) while the
      // inspector showed user-space Y (Y-up, ruler-origin) — the same
      // guide read two different values depending on which surface the
      // user opened. Angle flips via CoordSpace::to_user_angle_deg.
      const CanvasModel* mdl = m_doc ? &m_doc->canvas : nullptr;
      const double rox = m_doc ? m_doc->ruler_origin_x : 0.0;
      const double roy = m_doc ? m_doc->ruler_origin_y : 0.0;
      // canvas_h needed for the CoordSpace angle flip — same Y-axis-inverted
      // convention CoordSpace uses everywhere else.
      CoordSpace cs{mdl ? (double)mdl->canvas_height() : 1.0};

      const double dx = doc_to_display_x(bx, mdl, rox);
      const double dy = doc_to_display_y(by, mdl, roy);
      const double da = cs.to_user_angle_deg(ba);

      // Label suffix shows the unit the user is typing in. Matches the
      // inspector spin button's m_unit_label widget — same "(in)" / "(mm)"
      // / "(px)" / "(pt)" tag, just inlined into the form label because
      // show_form has no companion-label slot.
      const char* unit_lbl = UnitSystem::label(
          mdl ? mdl->display_unit : Unit::Px);
      const std::string x_label = std::string("X (") + unit_lbl + ")";
      const std::string y_label = std::string("Y (") + unit_lbl + ")";

      auto make_num = [](double dflt) {
        curvz::utils::NumberField nf;
        nf.default_value = dflt;
        // Wide range — guides can live anywhere on the canvas
        // including off-edge. Six decimals matches the s180 m1
        // inspector guide precision contract.
        nf.min      = -1e7;
        nf.max      =  1e7;
        nf.step     = 1.0;
        nf.decimals = 6;
        return nf;
      };
      if (is_h) {
        fields.push_back({"y", y_label,
                          make_num(dy)});
      } else if (is_v) {
        fields.push_back({"x", x_label,
                          make_num(dx)});
      } else {
        // Angled — anchor (x,y) plus angle (degrees, no unit suffix).
        fields.push_back({"x",     x_label,    make_num(dx)});
        fields.push_back({"y",     y_label,    make_num(dy)});
        auto na = make_num(da);
        na.min = -360.0;
        na.max =  360.0;
        fields.push_back({"angle", "Angle (\u00B0)", na});
      }

      std::string title = is_h ? "Set horizontal guide position" :
                           is_v ? "Set vertical guide position"   :
                                  "Set guide position and angle";

      // Capture by value into the callback so the lambda doesn't
      // outlive its parts. `this` is needed for m_history and
      // m_sig_doc_changed. giid resolves the guide via find_by_iid
      // inside the GuideMoveCommand — safe even if the guide were
      // somehow deleted between dialog show and OK. mdl/rox/roy
      // captured for post-conversion of the user-typed values.
      curvz::utils::show_form(
          *win, title, /*detail=*/"",
          fields, {"Cancel", "OK"},
          /*default_button=*/1, /*cancel_button=*/0,
          [this, giid, bx, by, ba, is_h, is_v, mdl, rox, roy](
              int button,
              const std::map<std::string, curvz::utils::FormFieldValue>&
                  values) {
            if (button != 1) return;  // Cancel or close

            // Resolve guide fresh — the user may have done something
            // in the gap. find_by_iid returns nullptr cleanly if gone.
            auto *proj = project();
            if (!proj) return;
            SceneNode *g = curvz::utils::find_by_iid(*proj, giid);
            if (!g || !g->is_guide()) return;

            // Pull user-typed (display-space) values, default to the
            // pre-conversion display values so an unchanged field
            // round-trips cleanly to its original doc-space value.
            CoordSpace cs2{mdl ? (double)mdl->canvas_height() : 1.0};
            double user_x = doc_to_display_x(bx, mdl, rox);
            double user_y = doc_to_display_y(by, mdl, roy);
            double user_a = cs2.to_user_angle_deg(ba);
            if (is_h) {
              auto it = values.find("y");
              if (it != values.end()) user_y = it->second.num();
            } else if (is_v) {
              auto it = values.find("x");
              if (it != values.end()) user_x = it->second.num();
            } else {
              auto ix = values.find("x");
              auto iy = values.find("y");
              auto ia = values.find("angle");
              if (ix != values.end()) user_x = ix->second.num();
              if (iy != values.end()) user_y = iy->second.num();
              if (ia != values.end()) user_a = ia->second.num();
            }

            // Post-convert display -> doc. Same pipeline the inspector
            // spin button runs on every adjustment commit.
            const double nx = display_to_doc_x(user_x, mdl, rox);
            const double ny = display_to_doc_y(user_y, mdl, roy);
            const double na = cs2.to_doc_angle_deg(user_a);

            // No-op guard. Same epsilon as the drag's push path.
            if (std::abs(nx - bx) < 0.001 &&
                std::abs(ny - by) < 0.001 &&
                std::abs(na - ba) < 0.001) {
              return;
            }

            g->guide_x     = nx;
            g->guide_y     = ny;
            g->guide_angle = na;

            if (m_history) {
              m_history->push(std::make_unique<GuideMoveCommand>(
                  project(), giid,
                  bx, by, ba,
                  nx, ny, na));
            }
            m_sig_doc_changed.emit();
            queue_draw();
          });
      return;
    }

    double dx, dy;
    screen_to_doc(x, y, dx, dy);

    // ── s204 m4: pivot right-click ────────────────────────────────────────
    //
    // If the pivot crosshair is currently visible (R held, or a custom
    // pivot was placed by clicking the canvas while R was held) AND the
    // right-click hit near it, open the RefPointPicker popover instead of
    // falling through to the SceneNode hit_test path. The crosshair is
    // not a SceneNode, so hit_test never finds it — without this branch,
    // right-clicking the pivot was indistinguishable from right-clicking
    // empty canvas (just dismissed nothing). Now it opens the picker
    // bound to the current selection's union bbox; the user can pick a
    // preset or arbitrary X/Y, and the pivot live-updates as they pick.
    //
    // Pixel tolerance: matches the visible arm length of the crosshair
    // (~10px screen). Computed in screen space so the catch radius
    // doesn't shrink with zoom.
    if ((m_r_held || m_has_custom_pivot) && m_tool == ActiveTool::Selection
        && !m_selection.empty()) {
      double piv_sx, piv_sy;
      doc_to_screen(m_custom_pivot_x, m_custom_pivot_y, piv_sx, piv_sy);
      const double catch_px = 12.0;
      if (std::hypot(x - piv_sx, y - piv_sy) <= catch_px) {
        // Compute selection union bbox — same math as notify_r_pressed's
        // default-pivot path. The picker's preset coords are relative to
        // this rectangle.
        bool found = false;
        double bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
        for (SceneNode *obj : m_selection) {
          auto bb = object_bbox(*obj);
          if (!bb) continue;
          if (!found) {
            bx1 = bb->x; by1 = bb->y;
            bx2 = bb->x + bb->w; by2 = bb->y + bb->h;
            found = true;
          } else {
            bx1 = std::min(bx1, bb->x);
            by1 = std::min(by1, bb->y);
            bx2 = std::max(bx2, bb->x + bb->w);
            by2 = std::max(by2, bb->y + bb->h);
          }
        }
        if (found) {
          const CanvasModel* mdl = m_doc ? &m_doc->canvas : nullptr;
          const double rox = m_doc ? m_doc->ruler_origin_x : 0.0;
          const double roy = m_doc ? m_doc->ruler_origin_y : 0.0;

          // Use a stable scriptable name. The registry refuses duplicate
          // live names; popovers are constructed and destroyed around
          // the user's pick, so "refpoint_picker.pivot" works fine in
          // practice (only one popover open at a time, the previous
          // instance is destroyed before the next opens).
          auto *picker = Gtk::make_managed<curvz::widgets::RefPointPicker>(
              "refpoint_picker.pivot", mdl, rox, roy);
          picker->set_bbox(bx1, by1, bx2 - bx1, by2 - by1);

          // Seed arbitrary-mode coords from the current pivot so
          // flipping to "Other" mid-edit doesn't reset to 0,0.
          picker->set_arbitrary_xy(m_custom_pivot_x, m_custom_pivot_y);
          // Then put it back in preset mode (default). If the current
          // pivot exactly matches one of the 9 preset points we could
          // pre-select it, but the live-preview design makes that
          // cosmetic; defaults to last preset (C on first open).
          picker->set_mode(curvz::widgets::RefPointPicker::Mode::Preset);

          // Live preview: every point change updates the pivot and
          // redraws. Subscriber lives only as long as the picker / its
          // popover.
          picker->signal_point_changed().connect(
              [this](double px, double py) {
                m_custom_pivot_x = px;
                m_custom_pivot_y = py;
                m_has_custom_pivot = true;
                queue_draw();
              });

          auto *popover = Gtk::make_managed<Gtk::Popover>();
          popover->set_parent(*this);
          popover->set_has_arrow(true);
          popover->set_autohide(true);  // click-outside dismisses

          // s204 m4 tweak: wrap picker + Apply button in a vertical Box.
          // Apply is purely a dismiss action — live preview already wired
          // the pivot before the user clicks Apply, so there's nothing
          // to commit. Apply just gives an explicit "I'm done" gesture
          // alongside the other dismiss paths (Escape, click-outside).
          // Pivot is session-only state (no undo, no persistence,
          // m3's R-toggle clears it), so no need for cancel-revert
          // semantics — Escape and Apply both keep the picked value.
          auto *vbox = Gtk::make_managed<Gtk::Box>(
              Gtk::Orientation::VERTICAL, 6);
          vbox->set_margin(8);
          vbox->append(*picker);
          auto *apply_row = Gtk::make_managed<Gtk::Box>(
              Gtk::Orientation::HORIZONTAL, 0);
          apply_row->set_halign(Gtk::Align::END);
          // s209 m5 — substrate Button with the unregistered tag.
          // Per-click popover (spiral-stop context menu); rebuilt every
          // right-click. No per-instance script addressability needed —
          // the Apply button is a one-shot dismiss. Sibling pattern to
          // ContextBar::show_context_menu (s209 m2).
          auto *apply_btn = Gtk::make_managed<curvz::widgets::Button>(
                                curvz::scripting::unregistered, "Apply");
          apply_btn->signal_clicked().connect(
              [popover]() { popover->popdown(); });
          apply_row->append(*apply_btn);
          vbox->append(*apply_row);
          popover->set_child(*vbox);

          Gdk::Rectangle rect((int)x, (int)y, 1, 1);
          popover->set_pointing_to(rect);

          // Lifetime: same pattern as build_object_context_menu's
          // popover. set_parent() attaches without normal child cleanup;
          // signal_closed + idle-unparent breaks the popdown→destroy
          // race that otherwise leaks each popover against the canvas.
          popover->signal_closed().connect([popover]() {
            Glib::signal_idle().connect_once(
                [popover]() { popover->unparent(); });
          });

          popover->popup();
          return;
        }
      }
    }

    SceneNode *hit = hit_test(dx, dy);
    // s275 m12: the s162 m3 "Blend right-click shows a one-button popover
    // with Rebuild Blend Steps and nothing else" branch was removed here.
    // It was the canvas-side counterpart of the popover the LayersPanel
    // folded into its unified row menu in s274 m11, and it had the same
    // problem: right-clicking a Blend gave you ONLY Rebuild Blend Steps
    // and missed every other verb (Delete, Move to layer, Cut, Copy,
    // Release Blend, Flatten...). The s274 m11 fix on the panel side was
    // to fold the Rebuild Blend Steps verb into the unified menu via a
    // local ctx.rebuild-blend action. This change does the same here:
    // Blends now flow through the standard object-context menu, which
    // surfaces Rebuild Blend Steps as one item in Effects (gated on
    // single-Blend selection) alongside everything else a Blend can do.
    // One right-click behaviour for Blends, not two competing ones.
    if (!hit) {
      // Right-click on empty canvas — no menu. Affinity/Illustrator convention:
      // an empty-canvas context menu would only carry document-level verbs
      // (paste, etc.), and we don't have those wired yet. Add later if needed.
      return;
    }
    if (!hit->is_image()) {
      // s162 m3: object-world context menu, driven by SelectionContext.
      //
      // Replaces the s125 m1a hand-rolled Box+Button popover (single "Save to
      // Library…" entry) with a Gio::Menu populated from the current
      // ObjectActions mask. Sections only appear when at least one of their
      // bits is set, so the menu is always exactly what's currently doable.
      // Action references resolve through MainWindow's `win.*` action group
      // for everything except "Save to Library…", which lives on Canvas
      // (signal_request_save_to_library) and gets a local `ctx` action group.
      //
      // If the hit isn't already in the selection, select-then-show — matches
      // Illustrator/Affinity. Otherwise leave the selection as-is so a
      // multi-selection's verbs (Union, Group, …) operate on the whole
      // selection. notify_object_selection_changed() refreshes
      // m_sel_ctx so the action mask we build the menu from is current.
      if (!is_selected(hit)) {
        // s162 m3: set_selection_single now folds in
        // notify_object_selection_changed() (see Canvas.cpp:~2873). The
        // previous explicit notify here was needed before that change to
        // keep m_sel_ctx fresh for the menu build below; it's now
        // redundant. Pre-s162 m3 comment was a class-C audit fix marker
        // for the s159 migration — kept in source-control history.
        set_selection_single(hit);
        queue_draw();
      }

      // Build the menu from the now-current SelectionContext. s275 m12
      // also passes m_doc + m_selection so the builder can inline-build
      // the Move to layer ▸ submenu and the Release Clip / Rebuild Blend
      // Steps gates that don't fit the ObjectActions bit scheme.
      auto menu = build_object_context_menu(m_sel_ctx.object_actions(),
                                            m_doc, m_selection);

      // Local "ctx" action group: hosts the verbs whose targets live on
      // Canvas rather than as MainWindow win.* actions. Same per-popup
      // action group pattern as DocTabBar's "tabctx" and LibraryPanel's
      // "libctx" — attached to the parent of the PopoverMenu, scoped to
      // the popover's lifetime.
      //
      //   • save-to-library — emits m_sig_request_save_to_library
      //   • rebuild-blend (s275 m12) — calls rebuild_blend on the lone
      //     Blend selection. The menu only references this action when
      //     selection is a single Blend, so unconditional registration
      //     here is fine.
      //   • move-to-layer-N (s275 m12) — one per ordinary unlocked
      //     layer, calls move_top_level_selection_to_layer(N). The menu
      //     only references each layer's action when that layer is an
      //     eligible target, but we register actions for ALL ordinary
      //     layers (uniformity over conditional registration). Per-popup
      //     namespace pollution dies with the popover.
      auto ag = Gio::SimpleActionGroup::create();

      auto act_save_lib = Gio::SimpleAction::create("save-to-library");
      act_save_lib->signal_activate().connect(
          [this](const Glib::VariantBase &) {
            // Defer the signal emission so the popover finishes dismissing
            // before MainWindow opens the folder-picker dialog. Without
            // this, popover popdown and dialog modal-show race in a way
            // that sometimes leaves the popover frame painted under the
            // dialog. Same defer the s125 m1a Button handler used.
            Glib::signal_idle().connect_once(
                [this]() { m_sig_request_save_to_library.emit(); });
          });
      ag->add_action(act_save_lib);

      // rebuild-blend — single-Blend gating happens at menu-build time.
      // Acts on m_selected (the primary selection); the menu only adds
      // the item when selection size is 1 and that one is a Blend.
      auto act_rebuild_blend = Gio::SimpleAction::create("rebuild-blend");
      act_rebuild_blend->signal_activate().connect(
          [this](const Glib::VariantBase &) {
            if (m_selected && m_selected->is_blend())
              rebuild_blend(m_selected);
          });
      ag->add_action(act_rebuild_blend);

      // move-to-layer-N — register one per ordinary layer in the current
      // doc. Each action calls Canvas::move_top_level_selection_to_layer
      // with the captured layer index. Per-popup; eligible-target gating
      // happens at menu-build time inside build_object_context_menu.
      if (m_doc) {
        for (int li = 0; li < (int)m_doc->layers.size(); ++li) {
          if (!curvz::utils::is_ordinary_target_layer(*m_doc->layers[li]))
            continue;
          std::string name = "move-to-layer-" + std::to_string(li);
          auto act = Gio::SimpleAction::create(name);
          int captured_li = li;
          act->signal_activate().connect(
              [this, captured_li](const Glib::VariantBase &) {
                move_top_level_selection_to_layer(captured_li);
              });
          ag->add_action(act);
        }
      }

      insert_action_group("ctx", ag);

      auto *popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
      popover->set_parent(*this);
      popover->set_has_arrow(false);
      Gdk::Rectangle rect((int)x, (int)y, 1, 1);
      popover->set_pointing_to(rect);

      // Lifetime: a popover attached via set_parent() is NOT a normal child
      // of its parent — managed-widget cleanup doesn't reach it, so each
      // right-click would leak its own PopoverMenu against the long-lived
      // Canvas. The s125 m1a hand-rolled popover had this same leak.
      // Pattern from DocumentGallery.cpp: hook signal_closed and unparent
      // from an idle, breaking the popdown→destroy ordering race. Capture
      // popover by raw pointer (managed widget; Glib idle keeps it alive
      // until the lambda runs).
      popover->signal_closed().connect([popover]() {
        Glib::signal_idle().connect_once([popover]() { popover->unparent(); });
      });

      popover->popup();
      return;
    }

    // Image branch — info dialog. Reaching here means hit is non-null and
    // is_image() is true (gated above).
    //
    // s125 m1g: replaced Gtk::AlertDialog with a Curvz-themed Gtk::Window.
    // AlertDialog is a system-level alert primitive — it paints in the OS
    // theme (so dark when the app is in lightmode), and its body text is
    // a non-selectable Pango string. Both wrong for an info dialog: the
    // user wants to read it in the app's chosen theme, and to copy the
    // path/filename out into other tools. This Gtk::Window inherits the
    // app motif via apply_motif_class_from_parent, and each value is a
    // Gtk::Label with set_selectable(true).

    // Filename (last path component)
    std::string full_path = hit->image_path;
    std::string fname = full_path;
    auto slash = full_path.rfind('/');
    if (slash != std::string::npos)
      fname = full_path.substr(slash + 1);

    // s125 m1c: read pixel dimensions, colour depth, format, file size via
    // the shared image_meta helper. Same source-of-truth as the importer,
    // so what the user sees in the info dialog matches what was loaded.
    ImageMeta meta = read_image_meta(full_path);
    std::string size_str = format_file_size(meta.file_size);

    // Modified time — best-effort; missing or unreadable file gets "unknown".
    std::string mtime_str = "unknown";
    {
      std::error_code mec;
      auto ftime = std::filesystem::last_write_time(full_path, mec);
      if (!mec) {
        // file_time_type → system_clock isn't portable in C++17 the strict way,
        // but std::chrono::clock_cast lands in C++20. The widely-supported
        // hack is to subtract clocks' "now" deltas. We just want a date string
        // for a human; second-resolution drift between mounts is fine.
        auto sys_time = std::chrono::system_clock::now() +
                        (ftime - std::filesystem::file_time_type::clock::now());
        std::time_t tt = std::chrono::system_clock::to_time_t(sys_time);
        std::tm tm_local{};
#ifdef _WIN32
        localtime_s(&tm_local, &tt);
#else
        localtime_r(&tt, &tm_local);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_local);
        mtime_str = buf;
      }
    }

    // Canvas (placed) size in doc units — what the image actually occupies
    // in the document, distinct from its source pixel dimensions.
    char canvas_size[64];
    snprintf(canvas_size, sizeof(canvas_size), "%.1f × %.1f", hit->image_w,
             hit->image_h);

    // s210 m1 — Build the ImageInfo payload and emit. MainWindow's
    // ImageInfoDialog member (a hide-on-close singleton, peer to
    // m_theme_edit_dialog / m_style_editor_dialog) is the presenter.
    // Canvas still does the data gathering — it has the SceneNode, the
    // path, and the helpers in Canvas_internal (read_image_meta,
    // format_file_size) — so the dialog stays a pure presenter and
    // doesn't need to know about Canvas internals.
    //
    // Replaces the prior s125 m1g..m1j inline form (heap-allocated
    // Gtk::Window self-deleting on hide). The visual decisions
    // (frameless flat-class Entries, dim-label name column, 480px
    // non-resizable, the s125 m1h focus-idle dance, blank-when-unknown
    // Format/Depth rows) are preserved verbatim inside ImageInfoDialog.
    ImageInfo info;
    info.filename = fname;
    info.full_path = full_path;
    info.pixels = meta.valid
                      ? std::to_string(meta.width) + " × " +
                            std::to_string(meta.height)
                      : "unknown";
    info.format = meta.format;   // "" → row hides
    info.depth = meta.depth;     // "" → row hides
    info.file_size = size_str;
    info.modified = mtime_str;
    info.placed_size = std::string(canvas_size) + " doc units";
    info.linkage = "External file (not embedded)";

    m_sig_request_image_info.emit(std::move(info));
  });
  add_controller(rclick);

  // Motion → cursor pos + rubber band
  auto motion = Gtk::EventControllerMotion::create();
  motion->signal_motion().connect(sigc::mem_fun(*this, &Canvas::on_motion));
  add_controller(motion);

  // Alt is forwarded by MainWindow::notify_alt_pressed/released — no local
  // key controller needed (it would require canvas focus which is unreliable).

  LOG_DEBUG("Canvas created");
}

// ── Public setters
// ────────────────────────────────────────────────────────────
void Canvas::set_document(CurvzDocument *doc) {
  m_doc = doc;
  m_selected = nullptr;
  m_selection.clear();
  m_node_selection.clear();
  m_selected_node = -1;
  m_corner_selection.clear();
  m_ref_hovered = nullptr;
  m_alt_drag_dup = false;
  m_pan_x = 0.0;
  m_pan_y = 0.0;
  // s266 m1 followup: doc-switch zoom sync.
  //
  // Pre-fix: m_fit_pending = true; first draw runs zoom_fit. That works for
  // the very first doc on app open (widget not yet sized), but on a doc-
  // switch during the running app it leaves a window where the canvas's
  // m_zoom is still the PREVIOUS doc's zoom while update_all_panels pushes
  // RulerState (and the rulers paint a snapshot at the stale zoom). Rulers
  // appear "wrong" until the user hides/shows them or otherwise forces a
  // repaint after the deferred fit fires.
  //
  // Fix: if the widget is already sized (running-app doc switch), run
  // zoom_fit synchronously NOW. fit_zoom() needs valid get_width/height,
  // and we have them. m_sig_zoom.emit inside zoom_fit propagates the new
  // zoom to the rulers before update_all_panels's update_rulers call later
  // in the doc-switch chain. The first-open path (widget not yet sized)
  // falls through to m_fit_pending and the first-draw fit, unchanged.
  if (doc && get_width() > 0 && get_height() > 0) {
    m_fit_pending = false;
    zoom_fit();  // sets m_zoom = fit_zoom(), pan=0, emits m_sig_zoom
  } else {
    m_fit_pending = true; // defer fit until first draw when widget is sized
  }
  // s160 m2: wholesale selection wipe on document switch — broadcast to
  // SelectionContext so cached object/node info doesn't carry over from the
  // previous document. (Pre-m2 there was no signal at all here; the audit
  // flagged this as a candidate for a standalone node-side notify, and
  // re-verification showed both sides need a notify because there's no
  // emit later in this function to cover the object side either.)
  notify_object_selection_changed();
  notify_node_selection_changed();
  queue_draw();
}

void Canvas::set_zoom(double zoom) {
  double mn = fit_zoom() * 0.01;
  mn = std::max(mn, MIN_ZOOM);
  m_zoom = std::clamp(zoom, mn, MAX_ZOOM);
  clamp_pan();
  m_sig_zoom.emit(m_zoom);
  queue_draw();
}

void Canvas::set_history(CommandHistory *history) { m_history = history; }

// ── Live-recolour walker (s70 M3) ────────────────────────────────────────────
//
// Descends the whole scene under `node` and, for every SceneNode whose
// fill_swatch_id / stroke_swatch_id matches `changed_id`, refreshes its
// cached FillStyle from the library's current swatch colour. Binding
// itself is unchanged — M1 already guaranteed id preservation; M3 only
// refreshes the render cache so the screen reflects the edit without a
// save/reload.
//
// Walk shape mirrors collect_paths (Canvas.cpp) and the scene-walker
// family more generally — descend into children, clip_shape, blend
// sources + cache, and warp_source + warp_glyph_cache + warp_cache.
// Touching caches keeps this frame consistent; any subsequent dirty-
// flag-driven regen will recompute them cleanly from the (now
// refreshed) sources.
static void live_recolour_walk(SceneNode* node,
                               const color::SwatchId& changed_id,
                               const color::SwatchLibrary& lib) {
  if (!node) return;

  // Refresh this node's cached FillStyle(s) if bound to the changed id.
  // Re-resolution goes through to_fillstyle with the library — same path
  // set_paint uses — so the Paint-with-swatch case lands on find_swatch
  // and writes a concrete Solid. Dead-ref degrades to the old fallback,
  // which is acceptable here (an edit to a deleted swatch is nonsense
  // anyway; M4 will prevent it).
  if (node->fill_swatch_id == changed_id) {
    color::Paint p = color::SwatchRef{ changed_id,
                                       color::Color{ node->fill.r, node->fill.g,
                                                     node->fill.b, node->fill.a } };
    node->fill = color::to_fillstyle(p, lib);
    // S106 m1 — fill colour just changed via swatch live-recolour.
    // No-op if not a gradient fill, so safe on solid swatch binds.
    mark_gradient_cache_dirty(*node);
  }
  if (node->stroke_swatch_id == changed_id) {
    color::Paint p = color::SwatchRef{ changed_id,
                                       color::Color{ node->stroke.paint.r,
                                                     node->stroke.paint.g,
                                                     node->stroke.paint.b,
                                                     node->stroke.paint.a } };
    node->stroke.paint = color::to_fillstyle(p, lib);
  }

  // Descend into every structural slot that can hold stylable nodes.
  for (auto& child : node->children)
    live_recolour_walk(child.get(), changed_id, lib);
  if (node->clip_shape)
    live_recolour_walk(node->clip_shape.get(), changed_id, lib);
  if (node->blend_source_a)
    live_recolour_walk(node->blend_source_a.get(), changed_id, lib);
  if (node->blend_source_b)
    live_recolour_walk(node->blend_source_b.get(), changed_id, lib);
  for (auto& step : node->blend_cache)
    live_recolour_walk(step.get(), changed_id, lib);
  if (node->warp_source)
    live_recolour_walk(node->warp_source.get(), changed_id, lib);
  if (node->warp_glyph_cache)
    live_recolour_walk(node->warp_glyph_cache.get(), changed_id, lib);
  if (node->warp_cache)
    live_recolour_walk(node->warp_cache.get(), changed_id, lib);
}

// ── unbind_swatch_walk (S83 m4h v4) ──────────────────────────────────────────
// Sibling to live_recolour_walk — same shape, different action. Walks the
// scene tree clearing fill_swatch_id / stroke_swatch_id on every node bound
// to the given id. The cached FillStyle (object's actual colour) is
// deliberately NOT touched — break-on-override v1 says the moment-of-unbind
// appearance is what the user keeps. This mirrors UnbindSwatchCommand's
// execute() shape and the manual ×-button unbind path in PropertiesPanel.
//
// Used by Canvas::unbind_swatch_from_doc, called from SwatchesPanel's
// delete-swatch path BEFORE remove_swatch runs. Without it,
// remove_swatch leaves dangling fill_swatch_id / stroke_swatch_id values
// on objects (the comment in remove_swatch acknowledges this and defers
// cleanup to a "delete-with-usage-check flow" — m4h v4 implements the
// no-confirm version of that flow as part of the inspector unification
// arc).
//
// Slot list MUST stay in sync with live_recolour_walk above. Any new
// structural slot added there needs adding here too.
void unbind_swatch_walk(SceneNode* node,
                               const color::SwatchId& removed_id) {
  if (!node) return;

  if (node->fill_swatch_id == removed_id)   node->fill_swatch_id.clear();
  if (node->stroke_swatch_id == removed_id) node->stroke_swatch_id.clear();

  for (auto& child : node->children)
    unbind_swatch_walk(child.get(), removed_id);
  if (node->clip_shape)
    unbind_swatch_walk(node->clip_shape.get(), removed_id);
  if (node->blend_source_a)
    unbind_swatch_walk(node->blend_source_a.get(), removed_id);
  if (node->blend_source_b)
    unbind_swatch_walk(node->blend_source_b.get(), removed_id);
  for (auto& step : node->blend_cache)
    unbind_swatch_walk(step.get(), removed_id);
  if (node->warp_source)
    unbind_swatch_walk(node->warp_source.get(), removed_id);
  if (node->warp_glyph_cache)
    unbind_swatch_walk(node->warp_glyph_cache.get(), removed_id);
  if (node->warp_cache)
    unbind_swatch_walk(node->warp_cache.get(), removed_id);
}

void Canvas::set_swatch_library(color::SwatchLibrary *lib) {
  // Drop any live-recolour connection tied to the previous library. Bare
  // disconnect is safe on a default-constructed sigc::connection and a
  // no-op when already disconnected. Called again on project switch —
  // MainWindow invokes set_swatch_library both at boot and when a new
  // doc becomes active.
  m_library_swatch_changed_conn.disconnect();

  m_swatch_library = lib;

  if (!m_swatch_library) return;

  // Hook live-recolour. On swatch edit, walk the whole doc tree, refresh
  // every SceneNode whose binding id matches, and queue a redraw. We
  // capture `this` (Canvas outlives any individual library handoff —
  // set_swatch_library is only called while Canvas is alive) and the
  // handler re-reads m_doc / m_swatch_library on each fire so project
  // switches between connect and signal are safe.
  m_library_swatch_changed_conn =
      m_swatch_library->signal_swatch_changed().connect(
          [this](const color::SwatchId& id) {
            if (!m_doc || !m_swatch_library) return;
            for (auto& L : m_doc->layers)
              live_recolour_walk(L.get(), id, *m_swatch_library);
            queue_draw();
          });
}

// ── Canvas::set_style_library (S78 m3d) ──────────────────────────────────────
// Cross-document live-propagation wiring for styles. Symmetric to
// set_swatch_library above — same lifecycle (boot + project switch via
// MainWindow::update_all_panels), same disconnect-then-reconnect dance.
//
// On signal_style_changed(id), every SceneNode in this canvas's document
// whose bound_style == id needs its fill/stroke cache refreshed from the
// (now-mutated) library entry. Without this walk, an edit to a user
// style would only be visible after close-and-reopen.
//
// Override model: break-on-override v1. Any direct fill/stroke edit
// clears bound_style via mutate_appearance, so any node still bound at
// signal time has not been overridden — overwriting fill/stroke
// unconditionally is correct under v1. If/when sticky-override tracking
// lands (StyleOverrides per StyleInterop.hpp), the per-node lambda
// here grows to honour the override flags; the walk shape doesn't.
//
// Drop-on-miss policy: this walk does NOT clear bound_style when
// materialise_from_style returns false. The load-time walk in
// CurvzProject does that (last-persisted-truth resolves; anything
// dangling is genuinely unresolvable). The live walk is purely a cache
// refresh — auto-clearing here would race with BindStyleCommand undo
// and the m4 delete-with-usage-check dialog. A WARN log is enough; the
// only path that actually fires this with an unknown id is a stale
// signal from a library that's mid-rename, which materialise itself
// will already log.
void Canvas::set_style_library(style::StyleLibrary *lib) {
  // Drop the previous library's connection. Default-constructed
  // sigc::connection on first call is a harmless no-op disconnect.
  m_library_style_changed_conn.disconnect();

  m_style_library = lib;

  if (!m_style_library) return;

  // Hook live propagation. Same capture-this discipline as the swatch
  // counterpart — Canvas outlives any individual library handoff, and
  // the handler re-reads m_doc / m_style_library on each fire so a
  // project switch between connect and signal-fire is safe.
  m_library_style_changed_conn =
      m_style_library->signal_style_changed().connect(
          [this](const style::StyleId& changed_id) {
            if (!m_doc || !m_style_library) return;
            for (auto& L : m_doc->layers) {
              style::walk_style_bindings(L.get(),
                  [this, &changed_id](SceneNode& n) {
                    if (n.bound_style != changed_id) return;
                    style::materialise_from_style(n, *m_style_library);
                    // Dropped binding (return false) is logged inside
                    // materialise_from_style; we deliberately do NOT
                    // clear bound_style here — see policy comment above.
                  });
            }
            queue_draw();
          });
}

// ── Canvas::mint_id / last_minted_iid ────────────────────────────────────────
// Thin public wrappers over the file-local next_id() / last_iid() helpers
// so callers outside this TU can produce fresh ids without duplicating
// the generate_internal_id() / s_last_iid book-keeping.
std::string Canvas::mint_id() { return next_id(); }
std::string Canvas::last_minted_iid() { return last_iid(); }

bool Canvas::object_bbox_query(const SceneNode &obj, double &x, double &y,
                               double &w, double &h,
                               bool include_stroke) const {
  auto bb = object_bbox(obj, include_stroke);
  if (!bb)
    return false;
  x = bb->x;
  y = bb->y;
  w = bb->w;
  h = bb->h;
  return true;
}

// ── Coordinate helpers
// ────────────────────────────────────────────────────────
double Canvas::doc_origin_x() const {
  if (!m_doc)
    return 0.0;
  return (get_width() - m_doc->canvas_width() * m_zoom) * 0.5 + m_pan_x;
}
double Canvas::doc_origin_y() const {
  if (!m_doc)
    return 0.0;
  return (get_height() - m_doc->canvas_height() * m_zoom) * 0.5 + m_pan_y;
}
void Canvas::screen_to_doc(double sx, double sy, double &dx, double &dy) const {
  dx = (sx - doc_origin_x()) / m_zoom;
  dy = (sy - doc_origin_y()) / m_zoom;
}
void Canvas::doc_to_screen(double dx, double dy, double &sx, double &sy) const {
  sx = dx * m_zoom + doc_origin_x();
  sy = dy * m_zoom + doc_origin_y();
}

// s204 m1: the no-op Canvas::snap(double v) used to live here as a TODO
// stub that returned v unchanged. Every caller — pivot setters, shape-tool
// draw start/motion, Line-tool, Ref-tool down/drag/up, ruler-corner refpt
// placement, cursor readout — silently fell back to raw coords because of
// it, ignoring the real snap engine in snap_x / snap_y below. Per the s203
// m3 CANON entry "pumps need to be the only path," the no-op was deleted
// outright (rather than implemented) so a caller can't pick the wrong
// pump by mistake: x-coords go through snap_x, y-coords through snap_y,
// and the compiler refuses anything else. ~18 call sites across
// Canvas_input.cpp and Canvas_draw.cpp were swept in the same change.

// s204 m2 — Walk visible artwork layers and emit bbox snap candidates.
//
// For each visible non-special-layer top-level child object, compute its
// bbox and contribute {left, midX, right} to out_x_candidates and
// {top, midY, bottom} to out_y_candidates. Group / Compound objects
// contribute their union bbox only, not their child bboxes — a group
// acts as one object for snap purposes, matching how object_bbox already
// handles the recursion internally.
//
// `exclude` is a list of object pointers to skip. Linear scan over
// `exclude` is fine: the typical exclude set is the multi-select being
// dragged (1–10 objects), and the dominant cost is the bbox computation
// loop, not exclusion testing.
//
// Per-object visibility (obj->visible) is honored on top of layer
// visibility — a hidden child of a visible layer is not a snap target.
// This mirrors hit_test and the render path.
void Canvas::gather_object_edge_snap_candidates(
    const std::vector<const SceneNode*>& exclude,
    std::vector<double>& out_x_candidates,
    std::vector<double>& out_y_candidates) const {
  if (!m_doc)
    return;
  for (const auto &layer : m_doc->layers) {
    if (!layer->visible || layer->is_special_layer())
      continue;
    for (const auto &obj_ptr : layer->children) {
      const SceneNode *obj = obj_ptr.get();
      if (!obj || !obj->visible)
        continue;
      // Skip any object in the exclude set — typically the moving set
      // during a drag, so a bbox doesn't snap to its own pre-move edges.
      bool skipped = false;
      for (const SceneNode *ex : exclude) {
        if (ex == obj) { skipped = true; break; }
      }
      if (skipped)
        continue;
      // include_stroke=false: snap to the geometric edge of the shape,
      // not the half-stroke-width inflated edge. A user dragging a
      // rect's left edge to touch another rect's right edge expects
      // the geometry to align, not the stroke envelopes.
      auto bb = object_bbox(*obj, /*include_stroke=*/false);
      if (!bb)
        continue;
      const double left  = bb->x;
      const double right = bb->x + bb->w;
      const double top   = bb->y;
      const double bot   = bb->y + bb->h;
      out_x_candidates.push_back(left);
      out_x_candidates.push_back((left + right) * 0.5);
      out_x_candidates.push_back(right);
      out_y_candidates.push_back(top);
      out_y_candidates.push_back((top + bot) * 0.5);
      out_y_candidates.push_back(bot);
    }
  }
}

double Canvas::snap_x(double doc_x, double tolerance_px) const {
  if (!m_doc || !m_doc->snap.enabled)
    return doc_x;
  double best = doc_x;
  double best_d = tolerance_px;

  if (m_doc->snap.snap_guides) {
    const SceneNode *gl = m_doc->guide_layer();
    // s192 m1 — snap-candidacy gates on visibility, not lock. Locking a
    // guide (or its layer) is a write-protection: the user still sees the
    // guide on canvas and expects to be able to snap to it. Hiding it is
    // the gesture that says "do not consider this." Same rule applies to
    // refs / grid / margins below and in snap_y / snap_move.
    if (gl && gl->visible) {
      for (const auto &child : gl->children) {
        if (!child->is_guide() || !child->guide_is_vertical())
          continue;
        double sx_guide, sy_dummy;
        doc_to_screen(child->guide_x, 0, sx_guide, sy_dummy);
        double sx_cur, sy_dummy2;
        doc_to_screen(doc_x, 0, sx_cur, sy_dummy2);
        double d = std::abs(sx_cur - sx_guide);
        if (d < best_d) {
          best_d = d;
          best = child->guide_x;
        }
      }
    }
    // Ref points snap X
    const SceneNode *rl = m_doc->ref_layer();
    if (rl && rl->visible) {
      for (const auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        double sx_ref, sy_dummy;
        doc_to_screen(child->ref_x, 0, sx_ref, sy_dummy);
        double sx_cur, sy_dummy2;
        doc_to_screen(doc_x, 0, sx_cur, sy_dummy2);
        double d = std::abs(sx_cur - sx_ref);
        if (d < best_d) {
          best_d = d;
          best = child->ref_x;
        }
      }
    }
  }

  // ── Grid X ─────────────────────────────────────────────────────────────
  // Closest vertical gridline to doc_x. Arithmetic — O(1), no enumeration.
  if (m_doc->snap.snap_grid) {
    const SceneNode *grid = m_doc->grid_layer();
    if (grid && grid->visible &&
        grid->grid_spacing_x >= 0.5) {
      double sx = grid->grid_spacing_x;
      double ox = grid->grid_offset_x;
      double cand = ox + std::round((doc_x - ox) / sx) * sx;
      double scx, scy_dummy;
      doc_to_screen(cand, 0, scx, scy_dummy);
      double sx_cur, sy_dummy2;
      doc_to_screen(doc_x, 0, sx_cur, sy_dummy2);
      double d = std::abs(sx_cur - scx);
      if (d < best_d) {
        best_d = d;
        best = cand;
      }
    }
  }

  // ── Margin X ───────────────────────────────────────────────────────────
  // Left edge, right edge, and column gutter dividers (if any).
  if (m_doc->snap.snap_margins) {
    const SceneNode *ml = m_doc->margin_layer();
    if (ml && ml->visible) {
      const double cw = (double)m_doc->canvas_width();
      const double left  = ml->margin_left;
      const double right = cw - ml->margin_right;
      auto try_cand = [&](double cand) {
        double scx, scy_dummy;
        doc_to_screen(cand, 0, scx, scy_dummy);
        double sx_cur, sy_dummy2;
        doc_to_screen(doc_x, 0, sx_cur, sy_dummy2);
        double d = std::abs(sx_cur - scx);
        if (d < best_d) { best_d = d; best = cand; }
      };
      try_cand(left);
      try_cand(right);
      // Column gutter dividers — only if columns subdivide the inner area
      const int cols = std::max(1, ml->margin_columns);
      const double iw = right - left;
      const double gap_x = ml->margin_col_gap;
      if (cols > 1 && iw > 0) {
        const double col_w = (iw - gap_x * (cols - 1)) / cols;
        for (int c = 1; c < cols; ++c) {
          double gx_left = left + c * col_w + (c - 1) * gap_x;
          double gx_right = gx_left + gap_x;
          try_cand(gx_left);
          try_cand(gx_right);
        }
      }
    }
  }

  // ── Edges X (s204 m2) ───────────────────────────────────────────────────
  // Object bbox edges: for every visible non-special-layer object, snap to
  // its left edge, horizontal center, and right edge. Gated on snap_edges.
  // The exclude list is empty in the snap_x entry path: this is called
  // from draw-start gestures (Rect / Ellipse / Line / Ref tools placing a
  // new shape) where no objects are moving. The drag path uses snap_move,
  // which passes its own exclude set so a moving bbox doesn't snap to its
  // own pre-drag position.
  if (m_doc->snap.snap_edges) {
    std::vector<double> edge_xs, edge_ys_unused;
    gather_object_edge_snap_candidates(/*exclude=*/{}, edge_xs, edge_ys_unused);
    for (double cand : edge_xs) {
      double scx, scy_dummy;
      doc_to_screen(cand, 0, scx, scy_dummy);
      double sx_cur, sy_dummy2;
      doc_to_screen(doc_x, 0, sx_cur, sy_dummy2);
      double d = std::abs(sx_cur - scx);
      if (d < best_d) {
        best_d = d;
        best = cand;
      }
    }
  }

  return best;
}

double Canvas::snap_y(double doc_y, double tolerance_px) const {
  if (!m_doc || !m_doc->snap.enabled)
    return doc_y;
  double best = doc_y;
  double best_d = tolerance_px;

  if (m_doc->snap.snap_guides) {
    const SceneNode *gl = m_doc->guide_layer();
    if (gl && gl->visible) {
      for (const auto &child : gl->children) {
        if (!child->is_guide() || !child->guide_is_horizontal())
          continue;
        double sx_dummy, sy_guide;
        doc_to_screen(0, child->guide_y, sx_dummy, sy_guide);
        double sx_dummy2, sy_cur;
        doc_to_screen(0, doc_y, sx_dummy2, sy_cur);
        double d = std::abs(sy_cur - sy_guide);
        if (d < best_d) {
          best_d = d;
          best = child->guide_y;
        }
      }
    }
    // Ref points snap Y
    const SceneNode *rl = m_doc->ref_layer();
    if (rl && rl->visible) {
      for (const auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        double sx_dummy, sy_ref;
        doc_to_screen(0, child->ref_y, sx_dummy, sy_ref);
        double sx_dummy2, sy_cur;
        doc_to_screen(0, doc_y, sx_dummy2, sy_cur);
        double d = std::abs(sy_cur - sy_ref);
        if (d < best_d) {
          best_d = d;
          best = child->ref_y;
        }
      }
    }
  }

  // ── Grid Y ─────────────────────────────────────────────────────────────
  if (m_doc->snap.snap_grid) {
    const SceneNode *grid = m_doc->grid_layer();
    if (grid && grid->visible &&
        grid->grid_spacing_y >= 0.5) {
      double sy = grid->grid_spacing_y;
      double oy = grid->grid_offset_y;
      double cand = oy + std::round((doc_y - oy) / sy) * sy;
      double sx_dummy, scy;
      doc_to_screen(0, cand, sx_dummy, scy);
      double sx_dummy2, sy_cur;
      doc_to_screen(0, doc_y, sx_dummy2, sy_cur);
      double d = std::abs(sy_cur - scy);
      if (d < best_d) {
        best_d = d;
        best = cand;
      }
    }
  }

  // ── Margin Y ───────────────────────────────────────────────────────────
  if (m_doc->snap.snap_margins) {
    const SceneNode *ml = m_doc->margin_layer();
    if (ml && ml->visible) {
      const double ch = (double)m_doc->canvas_height();
      const double top    = ml->margin_top;
      const double bottom = ch - ml->margin_bottom;
      auto try_cand = [&](double cand) {
        double sx_dummy, scy;
        doc_to_screen(0, cand, sx_dummy, scy);
        double sx_dummy2, sy_cur;
        doc_to_screen(0, doc_y, sx_dummy2, sy_cur);
        double d = std::abs(sy_cur - scy);
        if (d < best_d) { best_d = d; best = cand; }
      };
      try_cand(top);
      try_cand(bottom);
      const int rows = std::max(1, ml->margin_rows);
      const double ih = bottom - top;
      const double gap_y = ml->margin_row_gap;
      if (rows > 1 && ih > 0) {
        const double row_h = (ih - gap_y * (rows - 1)) / rows;
        for (int r = 1; r < rows; ++r) {
          double gy_top = top + r * row_h + (r - 1) * gap_y;
          double gy_bottom = gy_top + gap_y;
          try_cand(gy_top);
          try_cand(gy_bottom);
        }
      }
    }
  }

  // ── Edges Y (s204 m2) ───────────────────────────────────────────────────
  // Same shape as Edges X in snap_x above — snap to bbox top, vertical
  // center, and bottom for every visible non-special-layer object.
  if (m_doc->snap.snap_edges) {
    std::vector<double> edge_xs_unused, edge_ys;
    gather_object_edge_snap_candidates(/*exclude=*/{}, edge_xs_unused, edge_ys);
    for (double cand : edge_ys) {
      double sx_dummy, scy;
      doc_to_screen(0, cand, sx_dummy, scy);
      double sx_dummy2, sy_cur;
      doc_to_screen(0, doc_y, sx_dummy2, sy_cur);
      double d = std::abs(sy_cur - scy);
      if (d < best_d) {
        best_d = d;
        best = cand;
      }
    }
  }

  return best;
}

// Snap a moving selection to guides and ref points.
// BBX from snapshot positions — stable across frames.
// Hysteresis: engages at ENGAGE_PX, releases at RELEASE_PX.
std::pair<double, double> Canvas::snap_move(double raw_dx, double raw_dy) {
  if (!m_doc || !m_doc->snap.enabled)
    return {raw_dx, raw_dy};
  // s204 m2: include snap_edges in the "any class active?" guard. Without
  // this, turning off guides/grid/margins but leaving edges on would
  // short-circuit here and never reach the edge-snap blocks added below.
  if (!m_doc->snap.snap_guides && !m_doc->snap.snap_grid &&
      !m_doc->snap.snap_margins && !m_doc->snap.snap_edges)
    return {raw_dx, raw_dy};

  const SceneNode *gl = m_doc->guide_layer();
  bool guides_active = m_doc->snap.snap_guides &&
                       (gl && gl->visible);
  const SceneNode *rl = m_doc->ref_layer();
  bool refs_active = m_doc->snap.snap_guides &&
                     (rl && rl->visible);
  const SceneNode *grid_l = m_doc->grid_layer();
  bool grid_active = m_doc->snap.snap_grid &&
                     (grid_l && grid_l->visible &&
                      grid_l->grid_spacing_x >= 0.5 &&
                      grid_l->grid_spacing_y >= 0.5);
  const SceneNode *ml = m_doc->margin_layer();
  bool margins_active = m_doc->snap.snap_margins &&
                        (ml && ml->visible);
  // s204 m2 — edges snap is a property of all visible artwork objects,
  // not a dedicated layer; gate is the doc flag alone.
  bool edges_active = m_doc->snap.snap_edges;
  if (!guides_active && !refs_active && !grid_active && !margins_active &&
      !edges_active)
    return {raw_dx, raw_dy};

  static constexpr double ENGAGE_PX = 12.0;
  static constexpr double RELEASE_PX = 20.0;

  bool found = false;
  double bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
  for (auto &snap : m_move_snaps) {
    if (!snap.obj->path)
      continue;
    auto saved = snap.obj->path->nodes;
    snap.obj->path->nodes = snap.orig_nodes;
    for (auto &nd : snap.obj->path->nodes) {
      nd.x += raw_dx;
      nd.cx1 += raw_dx;
      nd.cx2 += raw_dx;
      nd.y += raw_dy;
      nd.cy1 += raw_dy;
      nd.cy2 += raw_dy;
    }
    auto bb = object_bbox(*snap.obj, /*include_stroke=*/false);
    snap.obj->path->nodes = saved;
    if (!bb)
      continue;
    if (!found) {
      bx1 = bb->x;
      by1 = bb->y;
      bx2 = bb->x + bb->w;
      by2 = bb->y + bb->h;
      found = true;
    } else {
      bx1 = std::min(bx1, bb->x);
      by1 = std::min(by1, bb->y);
      bx2 = std::max(bx2, bb->x + bb->w);
      by2 = std::max(by2, bb->y + bb->h);
    }
  }
  // Also include text/image nodes — temporarily apply the raw delta to measure
  // snap candidates.
  for (auto &tsnap : m_text_move_snaps) {
    // Temporarily shift the node to its would-be position.
    double saved_x =
        tsnap.obj->is_image() ? tsnap.obj->image_x : tsnap.obj->text_x;
    double saved_y =
        tsnap.obj->is_image() ? tsnap.obj->image_y : tsnap.obj->text_y;
    if (tsnap.obj->is_image()) {
      tsnap.obj->image_x = tsnap.orig_x + raw_dx;
      tsnap.obj->image_y = tsnap.orig_y + raw_dy;
    } else {
      tsnap.obj->text_x = tsnap.orig_x + raw_dx;
      tsnap.obj->text_y = tsnap.orig_y + raw_dy;
    }
    auto bb = object_bbox(*tsnap.obj, false);
    if (tsnap.obj->is_image()) {
      tsnap.obj->image_x = saved_x;
      tsnap.obj->image_y = saved_y;
    } else {
      tsnap.obj->text_x = saved_x;
      tsnap.obj->text_y = saved_y;
    }
    if (!bb)
      continue;
    if (!found) {
      bx1 = bb->x;
      by1 = bb->y;
      bx2 = bb->x + bb->w;
      by2 = bb->y + bb->h;
      found = true;
    } else {
      bx1 = std::min(bx1, bb->x);
      by1 = std::min(by1, bb->y);
      bx2 = std::max(bx2, bb->x + bb->w);
      by2 = std::max(by2, bb->y + bb->h);
    }
  }
  // Refpts — treat each as a degenerate bbox at its translated position.
  // Multi-refpt selection unions to the bounding rect of all refpts, so
  // {bx1, cx, bx2} candidates correspond to leftmost / centre / rightmost.
  for (auto &rsnap : m_ref_move_snaps) {
    if (!rsnap.obj || !rsnap.obj->is_ref())
      continue;
    double px = rsnap.orig_x + raw_dx;
    double py = rsnap.orig_y + raw_dy;
    if (!found) {
      bx1 = bx2 = px;
      by1 = by2 = py;
      found = true;
    } else {
      bx1 = std::min(bx1, px);
      by1 = std::min(by1, py);
      bx2 = std::max(bx2, px);
      by2 = std::max(by2, py);
    }
  }
  if (!found)
    return {raw_dx, raw_dy};
  double cx = (bx1 + bx2) * 0.5, cy = (by1 + by2) * 0.5;

  double adj_dx = raw_dx, adj_dy = raw_dy;

  // s204 m2 — Build exclude list of moving objects once for the whole
  // snap_move call. Used by edge-snap blocks below (X-acquire and
  // Y-acquire) so a moving bbox doesn't snap its own edges to its own
  // pre-drag position.
  //
  // CRITICAL: build from m_selection, NOT from m_move_snaps. The selection-
  // tool's drag-begin walks each selected object via collect_paths to push
  // leaf paths into m_move_snaps — so for a selected Group, m_move_snaps
  // contains the group's child leaves, while m_selection contains the
  // Group itself. The gather walks top-level layer children (where the
  // Group lives), so excluding by leaf pointer wouldn't match and the
  // Group's own bbox would snap to itself → snap engages at distance 0.
  // Building from m_selection matches the gather's level of recursion.
  //
  // Refpts in m_selection are filtered out by gather itself (refpts live
  // on a special layer, never enumerated as candidates). Including them
  // here is harmless but unnecessary; we leave them in for code simplicity.
  std::vector<const SceneNode*> edge_exclude;
  if (edges_active) {
    edge_exclude.reserve(m_selection.size());
    for (const SceneNode *obj : m_selection)
      edge_exclude.push_back(obj);
  }

  // ── X snap (vertical guides + ref X) ─────────────────────────────────
  if (m_snap_x_locked) {
    double gsx, gsy;
    doc_to_screen(m_snap_locked_x, 0, gsx, gsy);
    double best_cand = bx1, best_d = 1e9;
    for (double c : {bx1, cx, bx2}) {
      double sx, sy;
      doc_to_screen(c, 0, sx, sy);
      double d = std::abs(sx - gsx);
      if (d < best_d) {
        best_d = d;
        best_cand = c;
      }
    }
    if (best_d < RELEASE_PX)
      adj_dx = raw_dx + (m_snap_locked_x - best_cand);
    else
      m_snap_x_locked = false;
  }
  if (!m_snap_x_locked) {
    double best_d = ENGAGE_PX;
    // Guide X
    if (guides_active) {
      for (const auto &child : gl->children) {
        if (!child->is_guide() || !child->guide_is_vertical())
          continue;
        double gsx, gsy;
        doc_to_screen(child->guide_x, 0, gsx, gsy);
        for (double c : {bx1, cx, bx2}) {
          double sx, sy;
          doc_to_screen(c, 0, sx, sy);
          double d = std::abs(sx - gsx);
          if (d < best_d) {
            best_d = d;
            m_snap_x_locked = true;
            m_snap_locked_x = child->guide_x;
            adj_dx = raw_dx + (child->guide_x - c);
          }
        }
      }
    }
    // Ref X
    if (refs_active) {
      for (const auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        // Skip self when dragging refpts — a refpt in the move set
        // would otherwise snap to its own current position.
        bool is_self = false;
        for (auto &rsnap : m_ref_move_snaps)
          if (rsnap.obj == child.get()) { is_self = true; break; }
        if (is_self) continue;
        double gsx, gsy;
        doc_to_screen(child->ref_x, 0, gsx, gsy);
        for (double c : {bx1, cx, bx2}) {
          double sx, sy;
          doc_to_screen(c, 0, sx, sy);
          double d = std::abs(sx - gsx);
          if (d < best_d) {
            best_d = d;
            m_snap_x_locked = true;
            m_snap_locked_x = child->ref_x;
            adj_dx = raw_dx + (child->ref_x - c);
          }
        }
      }
    }
    // Grid X — closest vertical gridline to each candidate (arithmetic)
    if (grid_active) {
      const double sx_grid = grid_l->grid_spacing_x;
      const double ox_grid = grid_l->grid_offset_x;
      for (double c : {bx1, cx, bx2}) {
        double cand = ox_grid + std::round((c - ox_grid) / sx_grid) * sx_grid;
        double gsx, gsy;
        doc_to_screen(cand, 0, gsx, gsy);
        double sx, sy;
        doc_to_screen(c, 0, sx, sy);
        double d = std::abs(sx - gsx);
        if (d < best_d) {
          best_d = d;
          m_snap_x_locked = true;
          m_snap_locked_x = cand;
          adj_dx = raw_dx + (cand - c);
        }
      }
    }
    // Margin X — left, right, column dividers
    if (margins_active) {
      const double cw = (double)m_doc->canvas_width();
      const double left  = ml->margin_left;
      const double right = cw - ml->margin_right;
      std::vector<double> mcands{left, right};
      const int cols = std::max(1, ml->margin_columns);
      const double iw = right - left;
      const double gap_x = ml->margin_col_gap;
      if (cols > 1 && iw > 0) {
        const double col_w = (iw - gap_x * (cols - 1)) / cols;
        for (int ci = 1; ci < cols; ++ci) {
          double gx_left = left + ci * col_w + (ci - 1) * gap_x;
          mcands.push_back(gx_left);
          mcands.push_back(gx_left + gap_x);
        }
      }
      for (double cand : mcands) {
        double gsx, gsy;
        doc_to_screen(cand, 0, gsx, gsy);
        for (double c : {bx1, cx, bx2}) {
          double sx, sy;
          doc_to_screen(c, 0, sx, sy);
          double d = std::abs(sx - gsx);
          if (d < best_d) {
            best_d = d;
            m_snap_x_locked = true;
            m_snap_locked_x = cand;
            adj_dx = raw_dx + (cand - c);
          }
        }
      }
    }
    // Edges X (s204 m2) — bbox candidates from other visible objects.
    // For each candidate X (left/centre/right of every static object),
    // try snapping each of the moving union's {bx1, cx, bx2} to it.
    // The exclude list (built once above) prevents a moving bbox from
    // snapping to its own pre-drag left/centre/right.
    if (edges_active) {
      std::vector<double> edge_xs, edge_ys_unused;
      gather_object_edge_snap_candidates(edge_exclude, edge_xs, edge_ys_unused);
      for (double cand : edge_xs) {
        double gsx, gsy;
        doc_to_screen(cand, 0, gsx, gsy);
        for (double c : {bx1, cx, bx2}) {
          double sx, sy;
          doc_to_screen(c, 0, sx, sy);
          double d = std::abs(sx - gsx);
          if (d < best_d) {
            best_d = d;
            m_snap_x_locked = true;
            m_snap_locked_x = cand;
            adj_dx = raw_dx + (cand - c);
          }
        }
      }
    }
  }

  // ── Y snap (horizontal guides + ref Y) ───────────────────────────────
  if (m_snap_y_locked) {
    double gsx, gsy;
    doc_to_screen(0, m_snap_locked_y, gsx, gsy);
    double best_cand = by1, best_d = 1e9;
    for (double c : {by1, cy, by2}) {
      double sx, sy;
      doc_to_screen(0, c, sx, sy);
      double d = std::abs(sy - gsy);
      if (d < best_d) {
        best_d = d;
        best_cand = c;
      }
    }
    if (best_d < RELEASE_PX)
      adj_dy = raw_dy + (m_snap_locked_y - best_cand);
    else
      m_snap_y_locked = false;
  }
  if (!m_snap_y_locked) {
    double best_d = ENGAGE_PX;
    // Guide Y
    if (guides_active) {
      for (const auto &child : gl->children) {
        if (!child->is_guide() || !child->guide_is_horizontal())
          continue;
        double gsx, gsy;
        doc_to_screen(0, child->guide_y, gsx, gsy);
        for (double c : {by1, cy, by2}) {
          double sx, sy;
          doc_to_screen(0, c, sx, sy);
          double d = std::abs(sy - gsy);
          if (d < best_d) {
            best_d = d;
            m_snap_y_locked = true;
            m_snap_locked_y = child->guide_y;
            adj_dy = raw_dy + (child->guide_y - c);
          }
        }
      }
    }
    // Ref Y
    if (refs_active) {
      for (const auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        // Skip self — see X-acquire branch.
        bool is_self = false;
        for (auto &rsnap : m_ref_move_snaps)
          if (rsnap.obj == child.get()) { is_self = true; break; }
        if (is_self) continue;
        double gsx, gsy;
        doc_to_screen(0, child->ref_y, gsx, gsy);
        for (double c : {by1, cy, by2}) {
          double sx, sy;
          doc_to_screen(0, c, sx, sy);
          double d = std::abs(sy - gsy);
          if (d < best_d) {
            best_d = d;
            m_snap_y_locked = true;
            m_snap_locked_y = child->ref_y;
            adj_dy = raw_dy + (child->ref_y - c);
          }
        }
      }
    }
    // Grid Y — closest horizontal gridline (arithmetic)
    if (grid_active) {
      const double sy_grid = grid_l->grid_spacing_y;
      const double oy_grid = grid_l->grid_offset_y;
      for (double c : {by1, cy, by2}) {
        double cand = oy_grid + std::round((c - oy_grid) / sy_grid) * sy_grid;
        double gsx, gsy;
        doc_to_screen(0, cand, gsx, gsy);
        double sx, sy;
        doc_to_screen(0, c, sx, sy);
        double d = std::abs(sy - gsy);
        if (d < best_d) {
          best_d = d;
          m_snap_y_locked = true;
          m_snap_locked_y = cand;
          adj_dy = raw_dy + (cand - c);
        }
      }
    }
    // Margin Y — top, bottom, row dividers
    if (margins_active) {
      const double ch = (double)m_doc->canvas_height();
      const double top    = ml->margin_top;
      const double bottom = ch - ml->margin_bottom;
      std::vector<double> mcands{top, bottom};
      const int rows = std::max(1, ml->margin_rows);
      const double ih = bottom - top;
      const double gap_y = ml->margin_row_gap;
      if (rows > 1 && ih > 0) {
        const double row_h = (ih - gap_y * (rows - 1)) / rows;
        for (int ri = 1; ri < rows; ++ri) {
          double gy_top = top + ri * row_h + (ri - 1) * gap_y;
          mcands.push_back(gy_top);
          mcands.push_back(gy_top + gap_y);
        }
      }
      for (double cand : mcands) {
        double gsx, gsy;
        doc_to_screen(0, cand, gsx, gsy);
        for (double c : {by1, cy, by2}) {
          double sx, sy;
          doc_to_screen(0, c, sx, sy);
          double d = std::abs(sy - gsy);
          if (d < best_d) {
            best_d = d;
            m_snap_y_locked = true;
            m_snap_locked_y = cand;
            adj_dy = raw_dy + (cand - c);
          }
        }
      }
    }
    // Edges Y (s204 m2) — same shape as Edges X above, on the Y axis.
    if (edges_active) {
      std::vector<double> edge_xs_unused, edge_ys;
      gather_object_edge_snap_candidates(edge_exclude, edge_xs_unused, edge_ys);
      for (double cand : edge_ys) {
        double gsx, gsy;
        doc_to_screen(0, cand, gsx, gsy);
        for (double c : {by1, cy, by2}) {
          double sx, sy;
          doc_to_screen(0, c, sx, sy);
          double d = std::abs(sy - gsy);
          if (d < best_d) {
            best_d = d;
            m_snap_y_locked = true;
            m_snap_locked_y = cand;
            adj_dy = raw_dy + (cand - c);
          }
        }
      }
    }
  }

  return {adj_dx, adj_dy};
}

// ── Zoom ─────────────────────────────────────────────────────────────────────
void Canvas::zoom_toward(double wx, double wy, double factor) {
  const double old_zoom = m_zoom;
  const double min_z = std::max(fit_zoom() * 0.01, MIN_ZOOM);
  const double new_zoom = std::clamp(m_zoom * factor, min_z, MAX_ZOOM);
  if (new_zoom == old_zoom)
    return;

  // s113 m3: in preview mode, a zoom-in past the device-pixel safety
  // ceiling would crash the app on the next draw (drop-shadow Cairo
  // buffer scales with device pixels). Auto-flip to outline so the zoom
  // proceeds safely. Same hazard the m2 toggle gate prevents on the
  // outline→preview cliff; here we cover the in-preview-zoom-in path.
  if (!m_outline_mode && m_doc && new_zoom > old_zoom) {
    const double dw =
        static_cast<double>(m_doc->canvas_width()) * new_zoom;
    const double dh =
        static_cast<double>(m_doc->canvas_height()) * new_zoom;
    if (std::max(dw, dh) > PREVIEW_SAFE_DEVICE_PIXELS) {
      m_outline_mode = true;
      m_sig_outline_mode_changed.emit();
      m_sig_show_message.emit(
          "Switched to outline view",
          "Zoom is too high to render preview mode safely. The view has "
          "been switched to outline so the zoom can continue. Zoom out "
          "and toggle preview again when you're ready.");
    }
  }

  const double ox = doc_origin_x();
  const double oy = doc_origin_y();
  const double doc_x = (wx - ox) / old_zoom;
  const double doc_y = (wy - oy) / old_zoom;

  m_zoom = new_zoom;

  const double new_cw = m_doc ? m_doc->canvas_width() * m_zoom : 0;
  const double new_ch = m_doc ? m_doc->canvas_height() * m_zoom : 0;
  const double centre_x = (get_width() - new_cw) * 0.5;
  const double centre_y = (get_height() - new_ch) * 0.5;
  m_pan_x = wx - doc_x * m_zoom - centre_x;
  m_pan_y = wy - doc_y * m_zoom - centre_y;

  clamp_pan();
  m_sig_zoom.emit(m_zoom);
  queue_draw();
}

// Compute the zoom level that fits the entire canvas in the widget with margin
double Canvas::fit_zoom() const {
  if (!m_doc)
    return 1.0;
  int w = get_width();
  int h = get_height();
  if (w <= 0 || h <= 0)
    return 1.0;
  constexpr double MARGIN = 0.85; // use 85% of widget — leaves visible border
  double zw = (double)w / m_doc->canvas_width() * MARGIN;
  double zh = (double)h / m_doc->canvas_height() * MARGIN;
  return std::max(std::min(zw, zh), MIN_ZOOM);
}

// Fit canvas to window, centered, with margin
void Canvas::zoom_fit() {
  if (!m_doc)
    return;
  m_zoom = fit_zoom();
  m_pan_x = 0.0;
  m_pan_y = 0.0;
  m_sig_zoom.emit(m_zoom);
  queue_draw();
}

// Zoom to fit the screen-space rectangle (sx1,sy1)-(sx2,sy2) into the viewport.
void Canvas::zoom_to_rect(double sx1, double sy1, double sx2, double sy2) {
  if (!m_doc)
    return;
  double left = std::min(sx1, sx2);
  double top = std::min(sy1, sy2);
  double right = std::max(sx1, sx2);
  double bottom = std::max(sy1, sy2);
  if ((right - left) < 4 || (bottom - top) < 4)
    return;

  double dx1, dy1, dx2, dy2;
  screen_to_doc(left, top, dx1, dy1);
  screen_to_doc(right, bottom, dx2, dy2);
  double dw = dx2 - dx1;
  double dh = dy2 - dy1;
  if (dw < 1.0 || dh < 1.0)
    return;

  const double new_zoom =
      std::clamp(std::min(get_width() / dw, get_height() / dh) * 0.90,
                 std::max(fit_zoom() * 0.01, MIN_ZOOM), MAX_ZOOM);

  // s113 m3: same safety auto-switch as zoom_toward — marquee zoom can
  // land on an unsafe zoom in one shot (small marquee → large factor).
  // Only fires when zooming IN past the threshold; zoom-out and zoom-out
  // marquees are unaffected.
  if (!m_outline_mode && new_zoom > m_zoom) {
    const double pdw =
        static_cast<double>(m_doc->canvas_width()) * new_zoom;
    const double pdh =
        static_cast<double>(m_doc->canvas_height()) * new_zoom;
    if (std::max(pdw, pdh) > PREVIEW_SAFE_DEVICE_PIXELS) {
      m_outline_mode = true;
      m_sig_outline_mode_changed.emit();
      m_sig_show_message.emit(
          "Switched to outline view",
          "Zoom is too high to render preview mode safely. The view has "
          "been switched to outline so the zoom can continue. Zoom out "
          "and toggle preview again when you're ready.");
    }
  }

  double cx = (dx1 + dx2) * 0.5;
  double cy = (dy1 + dy2) * 0.5;
  m_zoom = new_zoom;
  // Re-centre the zoomed rect in the viewport
  m_pan_x = get_width() * 0.5 - cx * m_zoom -
            (get_width() - m_doc->canvas_width() * m_zoom) * 0.5;
  m_pan_y = get_height() * 0.5 - cy * m_zoom -
            (get_height() - m_doc->canvas_height() * m_zoom) * 0.5;

  m_sig_zoom.emit(m_zoom);
  queue_draw();
}

// Zoom to fit the active selection, centred in the viewport.
// Falls back to zoom_fit() if nothing is selected.
//
// In Node tool mode:
//   - Multiple nodes selected: zoom to tight bbox over all selected anchors.
//   - Single node selected: compute a comfortable view radius using the
//     distances to the 2 nearest other nodes in the same path, then pad
//     by 1.5× so the target node sits comfortably centred with neighbours
//     visible. Falls back to whole-object bbox if path has ≤ 2 nodes.
void Canvas::zoom_to_selection() {
  // ── Node tool branch ────────────────────────────────────────────────────
  if (m_tool == ActiveTool::Node && m_selected &&
      m_selected->type == SceneNode::Type::Path && m_selected->path) {

    const auto &nodes = m_selected->path->nodes;
    int n = (int)nodes.size();

    // Collect which node indices are selected
    std::vector<int> sel_indices;
    if (m_selected_node >= 0 && m_selected_node < n)
      sel_indices.push_back(m_selected_node);
    for (const auto &ns : m_node_selection) {
      if (ns.obj == m_selected && ns.node_idx >= 0 && ns.node_idx < n) {
        if (std::find(sel_indices.begin(), sel_indices.end(), ns.node_idx) ==
            sel_indices.end())
          sel_indices.push_back(ns.node_idx);
      }
    }

    if (sel_indices.empty()) {
      // Node tool active but no node selected — fall through to object bbox
      goto object_bbox_fallback;
    }

    if (sel_indices.size() == 1 && n >= 3) {
      // ── Single node: find 2 nearest neighbours by anchor distance ──────
      int si = sel_indices[0];
      double ax = nodes[si].x;
      double ay = nodes[si].y;

      // Collect distances from selected node to all other nodes
      std::vector<std::pair<double, int>> dists;
      dists.reserve(n - 1);
      for (int i = 0; i < n; ++i) {
        if (i == si)
          continue;
        double dx = nodes[i].x - ax;
        double dy = nodes[i].y - ay;
        dists.push_back({std::sqrt(dx * dx + dy * dy), i});
      }
      std::sort(dists.begin(), dists.end());

      // Take up to 2 nearest neighbours; measure their axis distances from
      // the selected node to compute a comfortable view half-extent.
      int k = std::min((int)dists.size(), 2);
      double max_dx = 0, max_dy = 0;
      for (int i = 0; i < k; ++i) {
        int ni = dists[i].second;
        max_dx = std::max(max_dx, std::abs(nodes[ni].x - ax));
        max_dy = std::max(max_dy, std::abs(nodes[ni].y - ay));
      }

      // Half-extent = neighbour distance * 1.5 — neighbours land inside
      // the view while the selected node stays exactly centred.
      double half_extent_x = max_dx * 1.5;
      double half_extent_y = max_dy * 1.5;
      // Minimum floor: 3% of canvas width so isolated nodes get a sensible view
      double min_half = m_doc ? m_doc->canvas_width() * 0.03 : 10.0;
      half_extent_x = std::max(half_extent_x, min_half);
      half_extent_y = std::max(half_extent_y, min_half);

      // Build rect centred on the selected node anchor
      double vx1 = ax - half_extent_x;
      double vy1 = ay - half_extent_y;
      double vx2 = ax + half_extent_x;
      double vy2 = ay + half_extent_y;

      double sx1, sy1, sx2, sy2;
      doc_to_screen(vx1, vy1, sx1, sy1);
      doc_to_screen(vx2, vy2, sx2, sy2);
      zoom_to_rect(sx1, sy1, sx2, sy2);
      return;

    } else {
      // ── Multiple nodes (or path too small for neighbour calc): ──────────
      // zoom to tight bbox of all selected anchors with a light padding.
      double minx = 0, miny = 0, maxx = 0, maxy = 0;
      bool first = true;
      for (int si : sel_indices) {
        double ax = nodes[si].x;
        double ay = nodes[si].y;
        if (first) {
          minx = maxx = ax;
          miny = maxy = ay;
          first = false;
        } else {
          minx = std::min(minx, ax);
          miny = std::min(miny, ay);
          maxx = std::max(maxx, ax);
          maxy = std::max(maxy, ay);
        }
      }

      // Pad by 20% each axis so nodes aren't right at the viewport edge
      double pw = (maxx - minx) * 0.20;
      double ph = (maxy - miny) * 0.20;
      double min_pad = m_doc ? m_doc->canvas_width() * 0.02 : 8.0;
      pw = std::max(pw, min_pad);
      ph = std::max(ph, min_pad);

      double sx1, sy1, sx2, sy2;
      doc_to_screen(minx - pw, miny - ph, sx1, sy1);
      doc_to_screen(maxx + pw, maxy + ph, sx2, sy2);
      zoom_to_rect(sx1, sy1, sx2, sy2);
      return;
    }
  }

  // ── Object selection fallback (Selection tool or no node selected) ───────
object_bbox_fallback:
  if (!m_doc || m_selection.empty()) {
    zoom_fit();
    return;
  }

  {
    bool found = false;
    double minx = 0, miny = 0, maxx = 0, maxy = 0;

    for (const SceneNode *obj : m_selection) {
      auto bb = object_bbox(*obj, /*include_stroke=*/false);
      if (!bb)
        continue;
      if (!found) {
        minx = bb->x;
        miny = bb->y;
        maxx = bb->x + bb->w;
        maxy = bb->y + bb->h;
        found = true;
      } else {
        minx = std::min(minx, bb->x);
        miny = std::min(miny, bb->y);
        maxx = std::max(maxx, bb->x + bb->w);
        maxy = std::max(maxy, bb->y + bb->h);
      }
    }

    if (!found) {
      zoom_fit();
      return;
    }

    double sx1, sy1, sx2, sy2;
    doc_to_screen(minx, miny, sx1, sy1);
    doc_to_screen(maxx, maxy, sx2, sy2);
    zoom_to_rect(sx1, sy1, sx2, sy2);
  }
}

// the artboard. Primary recovery tool for stranded off-canvas objects.
// Ctrl+Shift+0 per the HANDOFF spec.
void Canvas::zoom_to_all_objects() {
  if (!m_doc)
    return;

  // Collect union of all object bboxes in document space
  bool found = false;
  double minx, miny, maxx, maxy;
  minx = miny = maxx = maxy = 0.0;

  for (const auto &layer : m_doc->layers) {
    if (!layer->visible || layer->is_special_layer())
      continue;
    for (const auto &obj_ptr : layer->children) {
      auto bb = object_bbox(*obj_ptr);
      if (!bb)
        continue;
      if (!found) {
        minx = bb->x;
        miny = bb->y;
        maxx = bb->x + bb->w;
        maxy = bb->y + bb->h;
        found = true;
      } else {
        minx = std::min(minx, bb->x);
        miny = std::min(miny, bb->y);
        maxx = std::max(maxx, bb->x + bb->w);
        maxy = std::max(maxy, bb->y + bb->h);
      }
    }
  }

  if (!found) {
    // No objects — fall back to fit-artboard
    zoom_fit();
    return;
  }

  // Also include the artboard itself so it stays visible
  minx = std::min(minx, 0.0);
  miny = std::min(miny, 0.0);
  maxx = std::max(maxx, (double)m_doc->canvas_width());
  maxy = std::max(maxy, (double)m_doc->canvas_height());

  // Convert doc-space bbox to screen space at current zoom for zoom_to_rect
  double sx1, sy1, sx2, sy2;
  doc_to_screen(minx, miny, sx1, sy1);
  doc_to_screen(maxx, maxy, sx2, sy2);
  zoom_to_rect(sx1, sy1, sx2, sy2);
}
// ── Delete selected object(s) (Selection tool) ───────────────────────────────
bool Canvas::delete_selected() {
  if (m_tool == ActiveTool::Node)
    return false;
  if (m_selection.empty() || !m_doc)
    return false;

  // s317 — If the active selection is a TextBoxMgr member's boundary, the
  //   Delete key deletes the MEMBER (remove the view, reflow the text, and
  //   auto-delete the Mgr when it was the last member) — not the raw
  //   boundary path, which would corrupt the Mgr. Routed here so both the
  //   Delete accelerator and any delete_selected caller get it.
  if (m_selected) {
    SceneNode* mm_mgr = nullptr;
    SceneNode* mm_view = nullptr;
    if (find_textbox_member(m_selected, &mm_mgr, &mm_view) &&
        mm_mgr && mm_view) {
      delete_textbox_member(mm_mgr, mm_view);
      return true;
    }
  }

  bool deleted_any = false;
  // If a Compound dissolves during this delete, its surviving child is
  // tracked here so the post-loop block can promote it as the new
  // selection. Reset to nullptr if a subsequent iteration deletes the
  // survivor itself (multi-select of all Compound children).
  SceneNode *promoted_survivor = nullptr;
  const SceneNode *rl = m_doc->ref_layer();
  bool ref_layer_locked = (rl && rl->locked);

  // Helper — find the ClipGroup whose clip_shape slot holds `node`, or
  //   nullptr if none. We walk the whole tree; ClipGroups are rare
  //   enough that this is cheap. Returns the ClipGroup ancestor, not
  //   the immediate parent of the clip_shape (clip_shape has no
  //   parent->children slot).
  std::function<SceneNode *(SceneNode *, SceneNode *)> find_clip_owner =
      [&](SceneNode *root, SceneNode *target) -> SceneNode * {
    if (!root)
      return nullptr;
    if (root->is_clip_group() && root->clip_shape &&
        root->clip_shape.get() == target)
      return root;
    for (auto &c : root->children) {
      if (auto *r = find_clip_owner(c.get(), target))
        return r;
    }
    if (root->clip_shape) {
      if (auto *r = find_clip_owner(root->clip_shape.get(), target))
        return r;
    }
    return nullptr;
  };

  // Snapshot the selection vector before iterating. Several branches
  // below (ClipGroup release, Blend release, Compound dissolve)
  // reassign m_selection mid-flight; iterating the live vector with a
  // range-for would walk into UB after the reassignment. The snapshot
  // is cheap (vector of pointers) and decouples iteration from the
  // selection state's evolution.
  std::vector<SceneNode *> to_delete = m_selection;

  for (SceneNode *obj : to_delete) {
    // Refs not deletable when ref layer is locked
    if (obj->is_ref() && ref_layer_locked)
      continue;

    // ── Clip-shape special case ──────────────────────────────────────
    // If `obj` is the clip_shape of some ClipGroup, the user's intent
    // is "stop clipping" — dissolve the ClipGroup. We route through
    // release_clip_group which handles the proper unwind (clip_shape
    // and children return as siblings in parent, clip_shape on top).
    // We don't individually "delete" the clip_shape: the release flow
    // already restores it as a normal Path in the parent, matching the
    // spec Scott confirmed back at spec time ("if deleted it breaks
    // the clip and restores the items clipped").
    SceneNode *clip_owner = nullptr;
    for (auto &layer : m_doc->layers)
      if ((clip_owner = find_clip_owner(layer.get(), obj)))
        break;
    if (clip_owner) {
      LOG_INFO("Canvas: delete of clip_shape → dissolving ClipGroup '{}'",
               clip_owner->name);
      SceneNode *prev_selected = m_selected;
      m_selected = clip_owner;
      m_selection = {clip_owner};
      release_clip_group();
      (void)prev_selected; // selection is replaced by release_clip_group
      deleted_any = true;
      continue;
    }

    // ── Blend-source special case ────────────────────────────────────
    // If `obj` is blend_source_a or blend_source_b of some Blend, the
    // user's intent is "break the blend" — dissolve it. Matches the
    // ClipGroup precedent and keeps A/B + Steps available as siblings.
    // Both sources are restored; if the user truly wants the source
    // gone, they delete again after release.
    if (SceneNode *blend_owner = find_blend_owner(obj)) {
      LOG_INFO("Canvas: delete of Blend source → dissolving Blend '{}'",
               blend_owner->name);
      m_selected = blend_owner;
      m_selection = {blend_owner};
      release_blend();
      deleted_any = true;
      continue;
    }

    // Locate the parent of `obj`. find_parent descends one level into
    // Group/Compound, so this handles top-level layer children AND
    // children of a Group or Compound. Returns nullptr if not found
    // (e.g. obj was already removed by a prior iteration of the outer
    // loop, or it lives in a slot we don't walk like ClipGroup
    // clip_shape — that case is routed above).
    int idx = -1;
    SceneNode *parent = find_parent(m_doc, obj, &idx);
    if (!parent)
      continue;

    // Snapshot the doomed child for undo BEFORE mutation.
    auto child_snap = clone_node(*parent->children[idx]);

    // ── Compound subpath delete: extra invariants ──
    // A Compound exists only when it has ≥2 children. Removing a child
    // may push it to 1 (auto-dissolve into the survivor) or 0 (delete
    // the empty Compound). Each case uses a single command — not a
    // composite — chosen to capture the structural change atomically:
    //   - ≥2 children remaining → DeleteObjectCommand on the Compound.
    //   - 1 remaining → ReplaceNodeCommand on the outer parent
    //     (Compound → survivor); the deleted child rides along inside
    //     the `before` snapshot for undo.
    //   - 0 remaining → DeleteObjectCommand on the OUTER parent with
    //     the full pre-delete Compound subtree as snapshot.
    if (parent->is_compound()) {
      // Locate the Compound itself within ITS parent (the layer or an
      // outer container). Required for both the dissolve (replace
      // Compound with survivor) and the empty-collapse (delete
      // Compound) paths.
      int comp_idx = -1;
      SceneNode *comp_parent = find_parent(m_doc, parent, &comp_idx);
      if (!comp_parent) {
        // Compound not findable — should not happen for a well-formed
        // doc, but bail safely rather than corrupt the tree.
        LOG_WARN("delete_selected: Compound subpath delete found no "
                 "Compound parent — skipping");
        continue;
      }

      const int new_child_count = (int)parent->children.size() - 1;

      if (new_child_count >= 2) {
        // ── Plain subpath delete, Compound stays. Same shape as a
        //    layer-level delete, just one level down.
        if (m_history)
          m_history->push(std::make_unique<DeleteObjectCommand>(
              parent, std::move(child_snap), idx));
        parent->children.erase(parent->children.begin() + idx);
      } else if (new_child_count == 1) {
        // ── Dissolve: Compound + 1 surviving child → just the child
        //    promoted into the Compound's slot. Survivor keeps its own
        //    name (per spec: Compound's name was a label for the
        //    combination, not the surviving piece).
        //
        // The transformation is naturally a single
        // ReplaceNodeCommand: swap the original (2-child) Compound for
        // the surviving child at the same slot in comp_parent. The
        // doomed child rides along inside compound_before_snap (the
        // command's `before`); on undo the whole pre-mutation Compound
        // is restored, including both children.
        //
        // No composite is needed here. An earlier draft used
        // CompositeCommand of {DeleteObjectCommand on Compound,
        // ReplaceNodeCommand}, but DeleteObjectCommand's `parent`
        // pointer would dangle after ReplaceNodeCommand destroyed the
        // original Compound — undo would write to freed memory.
        const int survivor_idx = (idx == 0) ? 1 : 0;
        (void)child_snap; // doomed child is captured inside compound_before_snap.

        // Snapshots:
        //   - compound_before_snap: full pre-mutation Compound (both
        //     children present). Restored on undo.
        //   - survivor_snap: surviving child standing alone. Re-applied
        //     on redo.
        auto compound_before_snap = clone_node(*parent);
        auto survivor_snap = clone_node(*parent->children[survivor_idx]);

        // Mutate the live tree. The unique_ptr assignment at
        // comp_parent->children[comp_idx] destroys the original
        // Compound (and with it the doomed child plus the moved-from
        // null at survivor_idx). After this line, `parent` is a
        // dangling pointer — do NOT use it again in this branch.
        auto survivor_owned =
            std::move(parent->children[survivor_idx]);
        SceneNode *survivor_ptr_in_tree = survivor_owned.get();
        comp_parent->children[comp_idx] = std::move(survivor_owned);

        if (m_history)
          m_history->push(std::make_unique<ReplaceNodeCommand>(
              comp_parent, comp_idx, std::move(compound_before_snap),
              std::move(survivor_snap)));

        // Promote the survivor as the new selection — useful affordance
        // matching split_compound_path's "select the result of the
        // structural change" idiom. Recorded for the post-loop block;
        // cleared if a later iteration deletes the survivor itself.
        promoted_survivor = survivor_ptr_in_tree;
        LOG_INFO("Canvas: subpath delete → Compound dissolved, survivor "
                 "'{}' promoted",
                 survivor_ptr_in_tree->name.empty()
                     ? survivor_ptr_in_tree->id
                     : survivor_ptr_in_tree->name);
        deleted_any = true;
        continue;
      } else {
        // ── new_child_count == 0: Compound becomes empty → delete it.
        //    Only reachable from batch-delete that removes both children
        //    of a 2-child Compound in one operation; single-step
        //    delete dissolves at 1 first.
        //
        // Undo is one command, not a composite: snapshot the WHOLE
        // pre-delete Compound (with both children) and restore it via
        // DeleteObjectCommand on the OUTER parent. Forward erases the
        // Compound from comp_parent; undo re-inserts the snapshot
        // (with both children intact). The doomed child restoration is
        // implicit in the subtree snapshot.
        //
        // Note: child_snap is unused on this branch — the whole-
        // Compound snapshot already contains it. clone_node is cheap
        // enough that we don't bother avoiding the redundant clone.
        (void)child_snap;
        auto compound_before_snap = clone_node(*parent);

        // Mutate: erase the Compound from its outer parent. This
        // destroys the Compound and its surviving (doomed) child in
        // one shot.
        comp_parent->children.erase(comp_parent->children.begin() +
                                    comp_idx);

        if (m_history)
          m_history->push(std::make_unique<DeleteObjectCommand>(
              comp_parent, std::move(compound_before_snap), comp_idx));

        LOG_INFO("Canvas: subpath delete → Compound emptied, removed");
      }

      deleted_any = true;
      continue;
    }

    // ── Non-Compound parent (Layer or Group): plain delete.
    if (m_history)
      m_history->push(std::make_unique<DeleteObjectCommand>(
          parent, std::move(child_snap), idx));
    parent->children.erase(parent->children.begin() + idx);
    // If this delete consumed an earlier-dissolve survivor, drop the
    // promotion record — the post-loop block must then fall through to
    // the plain "clear selection" path.
    if (promoted_survivor == obj)
      promoted_survivor = nullptr;
    deleted_any = true;
  }
  if (deleted_any) {
    // Record macro step
    {
      MacroStep s;
      s.op = MacroStep::Op::Delete;
      record_step_if_recording(s);
    }
    if (promoted_survivor) {
      m_selection = {promoted_survivor};
      m_selected = promoted_survivor;
    } else {
      m_selected = nullptr;
      m_selection.clear();
    }
    m_selected_node = -1;
    // s160 m2: m_node_selection may hold NodeSel entries pointing at objects
    // that were just erased above — defensive clear before the helper call so
    // SelectionContext doesn't recompute over dangling pointers. The broader
    // scrub_node_refs audit (carry-forward backlog) will plumb this through
    // properly per destructive op.
    m_node_selection.clear();
    notify_object_selection_changed();
    notify_node_selection_changed();
    m_sig_doc_changed.emit();
    queue_draw();
    LOG_INFO("Canvas: deleted {} selected objects", m_selection.size());
  }
  return deleted_any;
}
std::vector<SelectionEntry>
collect_selection_entries(CurvzDocument *doc,
                          const std::vector<SceneNode *> &selection) {
  std::vector<SelectionEntry> result;
  // s298 DIAG — log every input pointer and whether it matched a direct
  // child of any layer. This walk is ONE LEVEL DEEP only — selected nodes
  // nested inside Group/Compound/ClipGroup will NOT match and get silently
  // dropped. Suspected root cause of duplicate/group misbehavior reported
  // s297. STRIP after triage.
  LOG_INFO("[GRPDIAG] collect_selection_entries: selection.size={}",
           selection.size());
  for (SceneNode *sel : selection) {
    LOG_INFO("[GRPDIAG]   selection entry: ptr={} iid='{}' name='{}' "
             "type={}",
             (void *)sel,
             sel ? sel->internal_id : std::string{"(null)"},
             sel ? sel->name : std::string{"(null)"},
             sel ? (int)sel->type : -1);
  }
  for (auto &layer : doc->layers) {
    for (int i = 0; i < (int)layer->children.size(); ++i) {
      SceneNode *child = layer->children[i].get();
      for (SceneNode *sel : selection) {
        if (sel == child)
          result.push_back({layer.get(), child, i});
      }
    }
  }
  // s298 DIAG — log what we found and what we lost.
  LOG_INFO("[GRPDIAG] collect_selection_entries: result.size={} (input={})",
           result.size(), selection.size());
  if (result.size() < selection.size()) {
    LOG_INFO("[GRPDIAG]   *** {} selected node(s) were NOT found as direct "
             "layer children — likely nested inside a container ***",
             selection.size() - result.size());
    // For each selection pointer, report whether it landed.
    for (SceneNode *sel : selection) {
      bool found = false;
      for (const auto &e : result) {
        if (e.node == sel) { found = true; break; }
      }
      LOG_INFO("[GRPDIAG]     sel iid='{}' name='{}' → {}",
               sel ? sel->internal_id : std::string{"(null)"},
               sel ? sel->name : std::string{"(null)"},
               found ? "FOUND" : "MISSING");
    }
  }
  return result;
}

// Strip any existing " (N)" suffix, then find the lowest N≥2 that is unique
// across all ids and names in the document.
static void collect_all_names(const SceneNode *node,
                              std::set<std::string> &out) {
  out.insert(node->name);
  for (const auto &child : node->children)
    collect_all_names(child.get(), out);
}

static std::string generate_unique_name(const std::string &base_name,
                                        CurvzDocument *doc) {
  // Strip existing " (N)" suffix
  std::string base = base_name;
  auto paren = base.rfind(" (");
  if (paren != std::string::npos) {
    std::string suffix = base.substr(paren + 2);
    if (!suffix.empty() && suffix.back() == ')') {
      suffix.pop_back();
      bool is_num = !suffix.empty() &&
                    std::all_of(suffix.begin(), suffix.end(), ::isdigit);
      if (is_num)
        base = base.substr(0, paren);
    }
  }

  // Collect all existing names
  std::set<std::string> names;
  for (auto &layer : doc->layers)
    for (auto &child : layer->children)
      collect_all_names(child.get(), names);

  // Try base (2), (3), ... until unique
  for (int n = 2; n < 10000; ++n) {
    std::string candidate = base + " (" + std::to_string(n) + ")";
    if (names.find(candidate) == names.end())
      return candidate;
  }
  return base + " (copy)";
}

// Recursively assign fresh ids and unique names to a cloned subtree.
// s298: also regenerate internal_id (the stable UUID). Pre-s298 this
// function refreshed id (SVG id) and name but inherited internal_id
// verbatim from the source, leaving the clone with the SAME iid as
// the original. That violated the invariant that internal_id is
// globally unique (per SceneNode.hpp line 248: "Stable UUID — unique
// across all docs, used for cross-node links") and caused iid-based
// command resolution (s167+ migration) to ambiguously match either
// the original or the clone depending on tree-walk order. Symptom:
// after cloning a group, an operation on the clone (group, edit,
// scale) silently targeted the original instead. Confirmed by
// GRPDIAG log evidence in s298 m1 — clone produced new ptr + new
// name but identical iid.
void freshen_ids(SceneNode *node, CurvzDocument *doc, int &counter) {
  node->internal_id = generate_internal_id();
  node->id = "obj" + std::to_string(counter++);
  node->name = generate_unique_name(node->name, doc);
  for (auto &child : node->children)
    freshen_ids(child.get(), doc, counter);
}

// ── copy_selected
// ─────────────────────────────────────────────────────────────
void Canvas::select_all() {
  if (!m_doc)
    return;

  m_selection.clear();
  m_selected = nullptr;

  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->locked)
      continue;
    if (layer->is_special_layer())
      continue;
    for (auto &child : layer->children) {
      if (!child->visible)
        continue;
      m_selection.push_back(child.get());
    }
  }

  if (!m_selection.empty())
    m_selected = m_selection[0];

  notify_object_selection_changed();
  queue_draw();
  LOG_INFO("Canvas: select_all — {} objects", m_selection.size());
}

// s136 m5: counterpart to select_all. Wired to Ctrl+Shift+A. Cheap to call
// when nothing is selected (no signal storm, no redraw) — guards on the
// already-empty case so accidental double-presses don't churn.
void Canvas::clear_selection() {
  if (m_selection.empty() && m_selected == nullptr)
    return;

  m_selection.clear();
  m_selected = nullptr;

  // s158 m1 — canonical site wired to the new notify helper.
  // s160 m2 — every other object-selection mutation site has now also
  // been migrated to the helper; only two E-class panel-refresh emits
  // remain bare (search "s159 m2: kept as bare emit"), pending the
  // s155-backlog inspector refactor.
  notify_object_selection_changed();
  queue_draw();
  LOG_INFO("Canvas: clear_selection");
}

// s290 — Node-tool select-all. Walks every visible non-special-layer path
// and adds every node index to m_node_selection. Primary slot points to
// the first found path at node index 0 so the inspector + drag handlers
// have a coherent primary; the n-order selection set is what drag /
// delete / cross-path operations read.
//
// Independent of object-world select_all() — that mutates m_selection
// (object-world primary set). Node mode operates on m_node_selection,
// orthogonal data.
void Canvas::node_select_all() {
  if (!m_doc)
    return;

  m_node_selection.clear();
  SceneNode *first_path = nullptr;

  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->locked)
      continue;
    if (layer->is_special_layer())
      continue;
    // Recursive walk for nested Group/Compound children. Same pattern
    // hit_test uses for descent.
    std::function<void(std::vector<std::unique_ptr<SceneNode>> &)> walk;
    walk = [&](std::vector<std::unique_ptr<SceneNode>> &children) {
      for (auto &child : children) {
        if (!child->visible)
          continue;
        if (child->type == SceneNode::Type::Group ||
            child->type == SceneNode::Type::Compound ||
            child->type == SceneNode::Type::ClipGroup) {
          walk(child->children);
          continue;
        }
        if (child->type != SceneNode::Type::Path || !child->path)
          continue;
        const int n = (int)child->path->nodes.size();
        if (n == 0)
          continue;
        if (!first_path)
          first_path = child.get();
        for (int i = 0; i < n; ++i)
          m_node_selection.push_back({child.get(), i});
      }
    };
    walk(layer->children);
  }

  if (first_path) {
    m_selected = first_path;
    m_selection = {first_path};
    m_selected_node = 0;
    notify_object_selection_changed();
    notify_node_selection_changed();
    m_sig_node_changed.emit(m_selected, m_selected_node);
  }
  queue_draw();
  LOG_INFO("Canvas: node_select_all — {} nodes across visible paths",
           m_node_selection.size());
}

// s290 — Node-tool counterpart to clear_selection. Clears node selection
// and the primary node slot. Object-world m_selection is left alone —
// the user may still want the path selected as an object (so the
// inspector can show stroke/fill); clearing the node selection just
// means "no nodes are picked for next operation."
void Canvas::node_clear_selection() {
  LOG_INFO("[s290 diag] node_clear_selection: ENTRY  "
           "m_node_selection.size={} m_selected_node={} "
           "m_selected2={} m_selected_node2={}",
           m_node_selection.size(), m_selected_node,
           (void*)m_selected2, m_selected_node2);
  if (m_node_selection.empty() && m_selected_node < 0)
    return;
  m_node_selection.clear();
  m_selected_node = -1;
  // s290 — defensive: also clear secondary node slot. The cross-path
  // join/connect machinery sets m_selected2 / m_selected_node2 to mark a
  // second path's "target" node, drawn as an orange ring separately from
  // m_node_selection (Canvas_draw.cpp:852). A bare "clear nodes" must
  // include this slot or it leaves a stray highlight.
  m_selected2 = nullptr;
  m_selected_node2 = -1;
  notify_node_selection_changed();
  m_sig_node_changed.emit(m_selected, m_selected_node);
  queue_draw();
  LOG_INFO("[s290 diag] node_clear_selection: EXIT  cleared");
}

void Canvas::copy_selected() {
  if (m_selection.empty() || !m_doc)
    return;
  m_clipboard.clear();
  m_clipboard_was_cut = false;

  auto entries = collect_selection_entries(m_doc, m_selection);
  for (auto &e : entries)
    m_clipboard.push_back(clone_node(*e.node));

  LOG_INFO("Canvas: copied {} object(s) to clipboard", m_clipboard.size());
}

// ── cut_selected
// ──────────────────────────────────────────────────────────────
void Canvas::cut_selected() {
  if (m_selection.empty() || !m_doc)
    return;

  auto entries = collect_selection_entries(m_doc, m_selection);
  if (entries.empty())
    return;

  // Snapshot for clipboard (keep original ids — cut reuses them on paste)
  m_clipboard.clear();
  m_clipboard_was_cut = true;
  for (auto &e : entries)
    m_clipboard.push_back(clone_node(*e.node));

  // Build CutCommand entries (descending index order for safe removal)
  // s170 m2 — iid-based capture: Entry stores parent_iid (string) instead
  // of a raw SceneNode*, push passes project() so execute()/undo() can
  // resolve via find_by_iid.
  std::vector<CutCommand::Entry> cmd_entries;
  for (auto &e : entries)
    cmd_entries.push_back({e.parent ? e.parent->internal_id : std::string(),
                           clone_node(*e.node), e.index});

  // Remove from document (high index first)
  std::vector<int> order(entries.size());
  for (int i = 0; i < (int)entries.size(); ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](int a, int b) { return entries[a].index > entries[b].index; });
  for (int i : order) {
    auto &ch = entries[i].parent->children;
    for (int j = (int)ch.size() - 1; j >= 0; --j)
      if (ch[j].get() == entries[i].node) {
        ch.erase(ch.begin() + j);
        break;
      }
  }

  if (m_history)
    m_history->push(std::make_unique<CutCommand>(project(), std::move(cmd_entries)));

  m_selected = nullptr;
  m_selection.clear();
  m_selected_node = -1;
  // s160 m2: m_node_selection may hold NodeSel entries pointing at objects
  // that were just erased above — defensive clear before the helper call so
  // SelectionContext doesn't recompute over dangling pointers.
  m_node_selection.clear();
  notify_object_selection_changed();
  notify_node_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: cut {} object(s)", m_clipboard.size());
}

// ── paste_clipboard
// ───────────────────────────────────────────────────────────
void Canvas::paste_clipboard() {
  // s298 DIAG — STRIP after triage. Entry-point dump.
  LOG_INFO("[GRPDIAG] paste_clipboard: ENTRY  clipboard.size={} was_cut={} "
           "m_doc={}",
           m_clipboard.size(), m_clipboard_was_cut, (void *)m_doc);
  if (m_clipboard.empty() || !m_doc) {
    LOG_INFO("[GRPDIAG] paste_clipboard: BAIL — clipboard empty or no doc");
    return;
  }

  SceneNode *target_layer = m_doc->active_layer();
  if (!target_layer) {
    LOG_INFO("[GRPDIAG] paste_clipboard: BAIL — no active layer");
    return;
  }
  LOG_INFO("[GRPDIAG] paste_clipboard: target_layer iid='{}' name='{}'",
           target_layer->internal_id, target_layer->name);

  // Build pasted nodes: copy from clipboard, freshen ids/names unless cut
  std::vector<std::unique_ptr<SceneNode>> pasted;
  int id_counter = s_next_id;

  for (auto &cb : m_clipboard) {
    auto node = clone_node(*cb);
    if (!m_clipboard_was_cut) {
      freshen_ids(node.get(), m_doc, id_counter);
    }
    pasted.push_back(std::move(node));
  }
  if (!m_clipboard_was_cut)
    s_next_id = id_counter;

  // Compute viewport centre in doc space — paste centres objects there
  double vp_cx, vp_cy;
  screen_to_doc(get_width() * 0.5, get_height() * 0.5, vp_cx, vp_cy);

  // Compute bounding box of pasted objects to centre them
  double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
  for (auto &node : pasted) {
    auto bb = object_bbox(*node, false);
    if (!bb)
      continue;
    bx1 = std::min(bx1, bb->x);
    by1 = std::min(by1, bb->y);
    bx2 = std::max(bx2, bb->x + bb->w);
    by2 = std::max(by2, bb->y + bb->h);
  }
  double paste_cx = (bx1 + bx2) * 0.5;
  double paste_cy = (by1 + by2) * 0.5;
  double dx = vp_cx - paste_cx;
  double dy = vp_cy - paste_cy;

  // Translate all pasted nodes to viewport centre
  if (std::abs(dx) > 0.01 || std::abs(dy) > 0.01) {
    for (auto &node : pasted) {
      std::vector<SceneNode *> paths;
      collect_paths(node.get(), paths);
      for (SceneNode *p : paths) {
        if (!p->path)
          continue;
        for (auto &n : p->path->nodes) {
          n.x += dx;
          n.y += dy;
          n.cx1 += dx;
          n.cy1 += dy;
          n.cx2 += dx;
          n.cy2 += dy;
        }
      }
    }
  }

  // Build command snapshots before moving ownership
  std::vector<std::unique_ptr<SceneNode>> cmd_snaps;
  for (auto &node : pasted)
    cmd_snaps.push_back(clone_node(*node));

  // Insert into target layer (front = top of layer)
  std::vector<SceneNode *> new_selection;
  for (int i = (int)pasted.size() - 1; i >= 0; --i) {
    new_selection.push_back(pasted[i].get());
    target_layer->children.insert(target_layer->children.begin(),
                                  std::move(pasted[i]));
  }

  if (m_history)
    m_history->push(
        std::make_unique<PasteCommand>(
            project(), target_layer->internal_id, std::move(cmd_snaps)));

  // After a cut-paste, the clipboard contents are consumed (can't paste again
  // with same ids). Clear the cut flag so a re-paste would freshen ids.
  if (m_clipboard_was_cut)
    m_clipboard_was_cut = false;

  // Select the pasted objects
  m_selection = new_selection;
  m_selected = new_selection.empty() ? nullptr : new_selection[0];
  m_selected_node = -1;
  notify_object_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: pasted {} object(s)", new_selection.size());
  // s298 DIAG — STRIP after triage. Exit dump.
  LOG_INFO("[GRPDIAG] paste_clipboard: EXIT  new_selection.size={}",
           new_selection.size());
  for (size_t i = 0; i < new_selection.size(); ++i) {
    SceneNode *n = new_selection[i];
    LOG_INFO("[GRPDIAG]   new_selection[{}]: ptr={} iid='{}' name='{}'",
             i, (void *)n,
             n ? n->internal_id : std::string{"(null)"},
             n ? n->name : std::string{"(null)"});
  }
}

// ── duplicate_selected
// ────────────────────────────────────────────────────────
void Canvas::duplicate_selected() {
  // s298 DIAG — STRIP after triage. Entry-point dump.
  LOG_INFO("[GRPDIAG] duplicate_selected: ENTRY  m_selection.size={} "
           "m_selected={} m_doc={}",
           m_selection.size(), (void *)m_selected, (void *)m_doc);
  for (size_t i = 0; i < m_selection.size(); ++i) {
    SceneNode *s = m_selection[i];
    LOG_INFO("[GRPDIAG]   m_selection[{}]: ptr={} iid='{}' name='{}'",
             i, (void *)s,
             s ? s->internal_id : std::string{"(null)"},
             s ? s->name : std::string{"(null)"});
  }
  if (m_selection.empty() || !m_doc) {
    LOG_INFO("[GRPDIAG] duplicate_selected: BAIL — selection empty or no doc");
    return;
  }

  auto entries = collect_selection_entries(m_doc, m_selection);
  if (entries.empty()) {
    LOG_INFO("[GRPDIAG] duplicate_selected: BAIL — entries empty after "
             "collect_selection_entries (all selected nodes are likely "
             "nested inside containers, which this verb doesn't handle)");
    return;
  }

  constexpr double OFFSET = 10.0; // doc-space nudge so duplicate is visible

  // s170 m3 — iid-based capture: Entry stores parent_iid (string) instead
  // of a raw SceneNode*, push passes project() so execute()/undo() can
  // resolve via find_by_iid.
  std::vector<DuplicateCommand::Entry> cmd_entries;
  std::vector<SceneNode *> new_selection;
  int id_counter = s_next_id;

  // Process in ascending index order so insertions don't shift later indices
  // We insert each duplicate immediately above its original (index + offset).
  // We accumulate an offset_shift to account for already-inserted duplicates.
  int shift = 0;
  for (auto &e : entries) {
    auto dup = clone_node(*e.node);
    freshen_ids(dup.get(), m_doc, id_counter);

    // Translate path nodes by OFFSET
    std::vector<SceneNode *> paths;
    collect_paths(dup.get(), paths);
    for (SceneNode *p : paths) {
      if (!p->path)
        continue;
      for (auto &n : p->path->nodes) {
        n.x += OFFSET;
        n.y -= OFFSET; // y-up: subtract moves up visually
        n.cx1 += OFFSET;
        n.cy1 -= OFFSET;
        n.cx2 += OFFSET;
        n.cy2 -= OFFSET;
      }
    }

    int ins = e.index +
              shift; // insert above original (lower index = higher in layer)
    auto snap = clone_node(*dup);
    new_selection.push_back(dup.get());
    e.parent->children.insert(e.parent->children.begin() + ins, std::move(dup));
    cmd_entries.push_back({e.parent ? e.parent->internal_id : std::string(),
                           std::move(snap), ins});
    ++shift;
  }
  s_next_id = id_counter;

  if (m_history)
    m_history->push(std::make_unique<DuplicateCommand>(project(), std::move(cmd_entries)));

  // Record macro step
  {
    MacroStep s;
    s.op = MacroStep::Op::Duplicate;
    s.dx = OFFSET;
    s.dy = -OFFSET; // y-up: negative moves up visually
    record_step_if_recording(s);
  }

  m_selection = new_selection;
  m_selected = new_selection.empty() ? nullptr : new_selection[0];
  m_selected_node = -1;
  notify_object_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: duplicated {} object(s)", new_selection.size());
  // s298 DIAG — STRIP after triage. Exit dump.
  LOG_INFO("[GRPDIAG] duplicate_selected: EXIT  new_selection.size={}",
           new_selection.size());
  for (size_t i = 0; i < new_selection.size(); ++i) {
    SceneNode *n = new_selection[i];
    LOG_INFO("[GRPDIAG]   new_selection[{}]: ptr={} iid='{}' name='{}'",
             i, (void *)n,
             n ? n->internal_id : std::string{"(null)"},
             n ? n->name : std::string{"(null)"});
  }
}

// ── duplicate_in_place_selected
// ───────────────────────────────────────────────────────────── Duplicate
// in-place (zero offset) — duplicate lands exactly on top of original.
// Recordable as MacroStep::Op::DuplicateInPlace.
//
// s181: renamed from clone_selected. Honest name — there is no source/
// instance link, no propagation, no live binding to the original. It is
// a duplicate that does not offset.
void Canvas::duplicate_in_place_selected() {
  // s298 DIAG — STRIP after triage. Entry-point dump.
  LOG_INFO("[GRPDIAG] duplicate_in_place_selected: ENTRY  "
           "m_selection.size={} m_selected={} m_doc={}",
           m_selection.size(), (void *)m_selected, (void *)m_doc);
  for (size_t i = 0; i < m_selection.size(); ++i) {
    SceneNode *s = m_selection[i];
    LOG_INFO("[GRPDIAG]   m_selection[{}]: ptr={} iid='{}' name='{}'",
             i, (void *)s,
             s ? s->internal_id : std::string{"(null)"},
             s ? s->name : std::string{"(null)"});
  }
  if (m_selection.empty() || !m_doc) {
    LOG_INFO("[GRPDIAG] duplicate_in_place_selected: BAIL — selection "
             "empty or no doc");
    return;
  }

  auto entries = collect_selection_entries(m_doc, m_selection);
  if (entries.empty()) {
    LOG_INFO("[GRPDIAG] duplicate_in_place_selected: BAIL — entries empty");
    return;
  }

  // s170 m3 — iid-based capture: same migration as duplicate_selected.
  std::vector<DuplicateCommand::Entry> cmd_entries;
  std::vector<SceneNode *> new_selection;
  int id_counter = s_next_id;

  int shift = 0;
  for (auto &e : entries) {
    auto dup = clone_node(*e.node);
    freshen_ids(dup.get(), m_doc, id_counter);
    // No position offset — duplicate lands exactly on top
    int ins = e.index + shift;
    auto snap = clone_node(*dup);
    new_selection.push_back(dup.get());
    e.parent->children.insert(e.parent->children.begin() + ins, std::move(dup));
    cmd_entries.push_back({e.parent ? e.parent->internal_id : std::string(),
                           std::move(snap), ins});
    ++shift;
  }
  s_next_id = id_counter;

  if (m_history)
    m_history->push(std::make_unique<DuplicateCommand>(project(), std::move(cmd_entries)));

  // Record macro step
  {
    MacroStep s;
    s.op = MacroStep::Op::DuplicateInPlace;
    record_step_if_recording(s);
  }

  m_selection = new_selection;
  m_selected = new_selection.empty() ? nullptr : new_selection[0];
  m_selected_node = -1;
  notify_object_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Canvas: duplicated-in-place {} object(s)", new_selection.size());
  // s298 DIAG — STRIP after triage.
  LOG_INFO("[GRPDIAG] duplicate_in_place_selected: EXIT  "
           "new_selection.size={}", new_selection.size());
  for (size_t i = 0; i < new_selection.size(); ++i) {
    SceneNode *n = new_selection[i];
    LOG_INFO("[GRPDIAG]   new_selection[{}]: ptr={} iid='{}' name='{}'",
             i, (void *)n,
             n ? n->internal_id : std::string{"(null)"},
             n ? n->name : std::string{"(null)"});
  }
}

void Canvas::select_object(SceneNode *obj) {
  // Selecting an object clears guide selection
  if (!m_guide_selection.empty()) {
    m_guide_selection.clear();
    m_sig_guide_selection_changed.emit(m_guide_selection);
  }
  m_selected = obj;
  m_selection.clear();
  if (obj)
    m_selection.push_back(obj);
  m_selected_node = -1;
  notify_object_selection_changed();
  queue_draw();
}

void Canvas::set_multi_selection(const std::vector<SceneNode *> &sel) {
  if (!sel.empty() && !m_guide_selection.empty()) {
    m_guide_selection.clear();
    m_sig_guide_selection_changed.emit(m_guide_selection);
  }
  m_selection = sel;
  m_selected = sel.empty() ? nullptr : sel.front();
  m_selected_node = -1;
  notify_object_selection_changed();
  queue_draw();
}

// ── Helper: find the layer and index of a SceneNode
// ─────────────────────────── ── collect_paths
// ───────────────────────────────────────────────────────────── Recursively
// collect all leaf Path nodes from obj (handles Group, Compound, ClipGroup).
//
// ClipGroup: descend into both `children` (the clipped objects) AND
// `clip_shape` (the path defining the clip region). Transforms driven
// off the returned list — nudge, drag, scale, rotate, skew — then
// mutate the clip shape in lock-step with its children, which is the
// correct rigid-transform behaviour: move/scale/rotate the ClipGroup
// and the clipped content stays clipped at its new location.
void collect_paths(SceneNode *obj, std::vector<SceneNode *> &out) {
  if (!obj)
    return;
  if (obj->type == SceneNode::Type::Path && obj->path) {
    out.push_back(obj);
    return;
  }
  if (obj->type == SceneNode::Type::Group ||
      obj->type == SceneNode::Type::Compound) {
    for (auto &child : obj->children)
      collect_paths(child.get(), out);
    return;
  }
  if (obj->type == SceneNode::Type::ClipGroup) {
    for (auto &child : obj->children)
      collect_paths(child.get(), out);
    if (obj->clip_shape)
      collect_paths(obj->clip_shape.get(), out);
    return;
  }
  // s310 m1bc — TextBoxMgr: descend through views to reach the
  // boundary Path of each canvas view. The structure is:
  //   Mgr → children[i] (TextBoxView) → children[0] (boundary Path)
  // collect_paths recurses on each child; TextBoxView itself isn't a
  // Path, but its boundary Path child gets picked up by the
  // recursion. Popover views have no children, so they contribute
  // nothing. Drag-move walks the returned list and translates each
  // Path's nodes — translating the boundary moves the visible frame;
  // the text inside reflows automatically. Same shape as ClipGroup's
  // descent.
  if (obj->is_text_box_mgr()) {
    for (auto &child : obj->children)
      collect_paths(child.get(), out);
    return;
  }
  // TextBoxView: descend into its children (the boundary Path lives
  // there for canvas views; popover views are empty). Recursive
  // approach keeps collect_paths agnostic to the view-vs-Mgr
  // distinction — both are just containers we descend through.
  if (obj->is_text_box_view()) {
    for (auto &child : obj->children)
      collect_paths(child.get(), out);
    return;
  }
  // Blend: A and B live in dedicated slots (not in `children`), and
  // blend_cache holds the generated intermediates. Transforms driven
  // off the returned list (drag, nudge, scale, rotate, skew) then
  // mutate sources AND cache in lock-step. Because the cache is
  // regenerated from A/B on every dirty rebuild, mutating cache
  // entries here is harmless — they'd get overwritten next frame
  // anyway — but including them keeps the bounding-box / transform
  // math correct *this* frame before the rebuild lands.
  if (obj->type == SceneNode::Type::Blend) {
    if (obj->blend_source_a)
      collect_paths(obj->blend_source_a.get(), out);
    if (obj->blend_source_b)
      collect_paths(obj->blend_source_b.get(), out);
    for (auto &step : obj->blend_cache)
      collect_paths(step.get(), out);
    return;
  }
  // Warp: only descend into warp_source (stable). The derived caches
  // (warp_glyph_cache, warp_cache) get regenerated on every envelope
  // edit — any pointer we hold into them becomes dangling as soon as
  // rebuild_warp_caches runs. Since callers use the returned list for
  // drag-begin snapshots consumed at drag-end, dangling pointers cause
  // use-after-free crashes (reproduced when selecting a Warp and
  // releasing without dragging — the overlay rebuild ran between press
  // and release). The caches will regenerate naturally from moved
  // source + moved envelope on the next draw, so we don't need them
  // in the drag-translate path.
  if (obj->type == SceneNode::Type::Warp) {
    if (obj->warp_source)
      collect_paths(obj->warp_source.get(), out);
    return;
  }
}

// ── collect_text_image_leaves
// ─────────────────────────────────────────────────────
// Complement to collect_paths: walks a SceneNode subtree and pushes every
// Text and Image node into `out`. Needed because the drag-move and nudge
// paths snapshot position fields (text_x/text_y, image_x/image_y) for
// those types separately — collect_paths only returns Path leaves.
//
// Containers recursed: Group, Compound, ClipGroup (including clip_shape,
// even though clip_shape can currently only be Path/Compound — kept
// symmetric with collect_paths for forward-compat).
// Text / Image themselves terminate recursion.
void collect_text_image_leaves(SceneNode *obj,
                                      std::vector<SceneNode *> &out) {
  if (!obj)
    return;
  if (obj->type == SceneNode::Type::Text ||
      obj->type == SceneNode::Type::Image) {
    out.push_back(obj);
    return;
  }
  if (obj->type == SceneNode::Type::Group ||
      obj->type == SceneNode::Type::Compound ||
      obj->type == SceneNode::Type::ClipGroup) {
    for (auto &child : obj->children)
      collect_text_image_leaves(child.get(), out);
    if (obj->type == SceneNode::Type::ClipGroup && obj->clip_shape)
      collect_text_image_leaves(obj->clip_shape.get(), out);
  }
  // s310 m1bc — TextBoxMgr: do NOT descend. The Mgr itself carries
  // the text_* fields (it plays the text-bearing role) but it isn't
  // a Type::Text node, and its children are TextBoxViews (not Text
  // or Image). Returning nothing here means the drag-move text-snap
  // list stays empty for Mgrs; the boundary Path inside the canvas
  // view still translates via collect_paths above. Mgr-level
  // text_x/text_y are cosmetic and ignored at render — not
  // translating them along with the boundary is acceptable; they'd
  // be no-ops anyway. (Pre-s310 TextBox-shape boxes descended here
  // to pick up the synthetic Text child for drag-move text_x/text_y
  // updates; that work is moot under the Mgr shape.)
  // Blend: descend into A, B, and cache. In v1 sources are required to
  // be Paths so this always produces empty out, but we descend
  // symmetrically with collect_paths so future expansion (Text blend,
  // Image blend) doesn't need a separate pass here.
  if (obj->type == SceneNode::Type::Blend) {
    if (obj->blend_source_a)
      collect_text_image_leaves(obj->blend_source_a.get(), out);
    if (obj->blend_source_b)
      collect_text_image_leaves(obj->blend_source_b.get(), out);
    for (auto &step : obj->blend_cache)
      collect_text_image_leaves(step.get(), out);
  }
  // Warp: descend into source + both caches. In M1–M6 sources are
  // Path/Compound/Group so this produces empty `out`, but descending
  // symmetrically with collect_paths means the future text-source Warp
  // milestone won't need a separate pass here.
  if (obj->type == SceneNode::Type::Warp) {
    if (obj->warp_source)
      collect_text_image_leaves(obj->warp_source.get(), out);
    if (obj->warp_glyph_cache)
      collect_text_image_leaves(obj->warp_glyph_cache.get(), out);
    if (obj->warp_cache)
      collect_text_image_leaves(obj->warp_cache.get(), out);
  }
}

// s161 split: a 27-line comment block describing rebuild_blend_cache (OPS)
//   was stranded here by an off-by-N block boundary during the split. The
//   stranded comment was removed in s161 and rebuild_blend_cache's full
//   comment restored in Canvas_ops.cpp where the body lives. find_parent
//   itself is unrelated — its routing to CORE per the s160 handoff is
//   correct.
SceneNode *find_parent(CurvzDocument *doc, SceneNode *target,
                              int *out_idx) {
  if (!doc)
    return nullptr;
  // Search layers
  for (auto &layer : doc->layers) {
    for (int i = 0; i < (int)layer->children.size(); ++i) {
      if (layer->children[i].get() == target) {
        if (out_idx)
          *out_idx = i;
        return layer.get();
      }
    }
    // Search groups and compounds within layers (one level)
    for (auto &child : layer->children) {
      if (child->type != SceneNode::Type::Group &&
          child->type != SceneNode::Type::Compound)
        continue;
      for (int i = 0; i < (int)child->children.size(); ++i) {
        if (child->children[i].get() == target) {
          if (out_idx)
            *out_idx = i;
          return child.get();
        }
      }
    }
  }
  return nullptr;
}

// ── Canvas::is_node_alive ────────────────────────────────────────────────────
// Pointer-safe liveness check. Walks the doc tree comparing slot pointers
// against `target` — never derefs `target`, so calling this with a freed
// pointer is safe (it'll just return false). Used by deferred idle handlers
// that captured a SceneNode* before a destructive mutation may have freed
// it. Same coverage as find_parent (layer children + one level into
// Group/Compound).
bool Canvas::is_node_alive(const SceneNode *target) const {
  if (!m_doc || !target)
    return false;
  for (auto &layer : m_doc->layers) {
    for (auto &child : layer->children) {
      if (child.get() == target)
        return true;
      if (child->type == SceneNode::Type::Group ||
          child->type == SceneNode::Type::Compound) {
        for (auto &gc : child->children) {
          if (gc.get() == target)
            return true;
        }
      }
    }
  }
  return false;
}

// ── Canvas::scrub_node_refs ──────────────────────────────────────────────────
// s156 — Single seam for "this SceneNode is about to be erased — null any
// Canvas state pointing at it."  The renderer reads m_node_selection /
// m_selection directly off Canvas every paint, and various tool members
// (m_align_anchor, m_snap_target_obj, …) hold raw SceneNode pointers
// across mutations. Every destructive op (Join cross-path, Split, Delete,
// Boolean ops, …) must call this BEFORE the layer-children erase.
//
// What this pump does NOT do: it does not touch m_selected. Callers
// reassign m_selected to its replacement immediately after the erase.
// The window between erase and reassign must contain no signal emits or
// renderer paints — keep it tight.
//
// Coverage list — extend as new pointer-holders appear. The compile-time
// rule: any new "SceneNode * m_…" member should be triaged against this
// pump and added if it can persist across destructive ops.
// s298 m2 (A1) — recursive walk that clears any text node's text-on-path
// fields (text_path_id, text_path_offset, text_path_flip) when they point
// at the target node's internal_id. Called from scrub_node_refs below to
// close the silent-dangling class described in s297's text-on-path recon
// (text_on_path_redesign.md, finding 3 / bug B2): deleting a guide path
// previously left attached text with a stale text_path_id; the renderer
// fell back to straight text on next paint, the SVG round-trip preserved
// the dead iid on save, and the link became impossible to repair without
// hand-editing the file.
//
// Descent rules mirror CurvzDocument::index_walk: children, plus the
// authoritative non-children slots (clip_shape, blend_source_a/_b,
// warp_source). Derived caches (blend_cache, warp_glyph_cache,
// warp_cache) are deliberately skipped — same reasoning as the iid
// walker, those rebuild on next paint and their iids aren't stable.
//
// Why fields are zeroed but text_x/text_y is left alone: when scrub
// fires, the path is about to be destroyed and we don't have a sensible
// new anchor position. The text node stays where it was; the renderer
// falls through to draw_text_node's straight-text path on the next
// paint. Compare release_text_from_path, which DOES reposition (via
// top_compute_detach_position) because there the path is still alive
// at detach time and we can compute a position along it.
//
// Not undoable on its own — scrub_node_refs is the destructive-op seam,
// and the caller (delete command, etc.) owns the undo for what's being
// destroyed. Open question deferred: if a future delete-path command
// captures the partner text's link state and restores it on undo, the
// pair could come back as a unit. For now, undoing a delete that took
// a guide path with it leaves any partner text as straight text — a
// known limitation, smaller than the silent-dangling original.
static void scrub_text_path_refs(const SceneNode *n, const std::string &dead_iid) {
  if (!n)
    return;
  for (const auto &c : n->children) {
    if (c->is_text() && c->text_path_id == dead_iid) {
      c->text_path_id = "";
      c->text_path_offset = 0.0;
      c->text_path_flip = false;
    }
    // s301 m1a — also scrub the unified container-model bindings.
    // Removing any boundary iid that points at the dead node, and clearing
    // the line-pattern id if it matches. Same rationale as the text_path_id
    // scrub above: leaving a stale iid produces silent dangling references
    // that re-bind to whatever the iid index resolves to next (or to nothing,
    // which presents as text reverting to legacy unbound rendering). For
    // the boundary list, erasing rather than zeroing preserves the chain
    // order of the remaining boundaries; if the dead node was the first
    // boundary, the next-in-chain becomes the new first naturally.
    if (c->is_text()) {
      auto &bs = c->text_boundary_ids;
      bs.erase(std::remove(bs.begin(), bs.end(), dead_iid), bs.end());
      if (c->text_line_path_id == dead_iid) {
        c->text_line_path_id = "";
      }
    }
    scrub_text_path_refs(c.get(), dead_iid);
  }
  if (n->clip_shape)
    scrub_text_path_refs(n->clip_shape.get(), dead_iid);
  if (n->blend_source_a)
    scrub_text_path_refs(n->blend_source_a.get(), dead_iid);
  if (n->blend_source_b)
    scrub_text_path_refs(n->blend_source_b.get(), dead_iid);
  if (n->warp_source)
    scrub_text_path_refs(n->warp_source.get(), dead_iid);
}

// ── s301 m1b — Cross-TU boundary lookup used by TextCursor ──────────────────
// Walks every layer's direct children searching for a node whose
// internal_id equals the requested iid. Linear scan: documents in active
// edit are typically dozens to low hundreds of nodes; the cursor calls
// this O(1) times per frame (only during text-edit mode), so we don't
// need the lazy iid-index pump that the undo/redo machinery uses. The
// scan is intentionally direct-children-only — boundaries always live at
// the active layer level in 1b (no nesting inside groups), matching how
// the text-tool marquee places them.
SceneNode* Canvas::find_text_boundary(const std::string& iid) {
  if (!m_doc || iid.empty()) return nullptr;
  for (auto& layer : m_doc->layers) {
    if (!layer) continue;
    for (auto& c : layer->children) {
      if (!c) continue;
      // Direct layer-child match — covers legacy paired-sibling text
      // (loaded from pre-TextBox files) where boundaries live at the
      // layer level.
      if (c->internal_id == iid) return c.get();
      // s310 m1bc — TextBoxMgr-nested match. The boundary lives one
      // extra level deeper now:
      //   layer → Mgr → CanvasView → children[0] = boundary Path
      // Walk each Mgr's canvas-view children and check the first
      // child of each. Popover views have no children, so they're
      // skipped naturally.
      if (c->is_text_box_mgr()) {
        for (auto& view : c->children) {
          if (!view || !view->is_canvas_view()) continue;
          if (view->children.empty() || !view->children[0]) continue;
          if (view->children[0]->internal_id == iid)
            return view->children[0].get();
        }
      }
    }
  }
  return nullptr;
}

// Find the TextBoxMgr container that owns `text` as its text-bearing
// node. s310 m1bc — under the Mgr-with-views shape, the Mgr IS the
// text-bearing node (it carries text_content / font defaults / caret
// directly). So when callers pass m_text_editing (which now points
// at the Mgr itself), the answer is just the Mgr.
//
// For any non-Mgr argument (legacy bare Text node), there's no Mgr
// container to find — the s310 m1bc refactor stopped constructing
// any new TextBox-shape nodes, and pre-s310 saves load into Mgr
// shape via the parser compat path. Returns nullptr in that case,
// letting callers fall back to their legacy-unbound-text paths.
SceneNode* Canvas::find_text_box_for_text(const SceneNode* text) {
  if (!m_doc || !text) return nullptr;
  if (text->is_text_box_mgr())
    return const_cast<SceneNode*>(text);
  return nullptr;
}

// One place that defines what "enter editing on this TextBoxMgr" means.
// Called from the double-click handlers in Selection and Text tools,
// and (potentially) from any future menu / scripting verb that wants
// to drop the user straight into editing a textbox.
//
// Tool-agnostic. The caller decides whether a tool switch is needed
// (Selection tool's double-click emits ActiveTool::Text before
// scheduling this call); the helper sets up edit state and starts
// the cursor regardless of which tool is active.
//
// s310 m1bc — `text_box` is now a Type::TextBoxMgr. The Mgr fills
// the role m_text_editing used to fill: it carries the text_* fields
// directly. The boundary lives one level deeper at
// mgr->children[0]->children[0] (the canvas view's only child).
// Malformed Mgr → silent return. Caller's contract is "pass a real
// TextBoxMgr with at least one Canvas TextBoxView holding a Path
// boundary"; defensive shape-check here protects against corrupted
// state without crashing.
void Canvas::begin_textbox_edit(SceneNode* text_box, bool have_point,
                                double doc_x, double doc_y) {
  if (!text_box || !text_box->is_text_box_mgr()) return;
  if (text_box->children.empty()) return;
  // s317 — Region-aware entry. Default: anchor to the FIRST member (byte 0)
  //   — fresh construction and point-less callers. When a click point is
  //   given, find which member box it landed in and anchor the caret to
  //   THAT member's boundary + its flow byte_start, so double-clicking any
  //   box (not just the first) enters edit there. The single-region cursor
  //   lays out [byte_start, …] into that boundary; absolute byte offsets
  //   make the caret math identical to the byte-0 case.
  SceneNode* boundary = nullptr;
  size_t byte_start = 0;
  bool resolved_by_point = false;
  if (have_point &&
      member_region_at_point(text_box, doc_x, doc_y, &boundary, &byte_start)) {
    // boundary + byte_start resolved to the clicked member.
    resolved_by_point = true;
  } else {
    // First canvas view's boundary, byte 0.
    SceneNode* canvas_view = nullptr;
    for (auto& v : text_box->children) {
      if (v && v->is_canvas_view()) { canvas_view = v.get(); break; }
    }
    if (!canvas_view || canvas_view->children.empty() ||
        !canvas_view->children[0] ||
        canvas_view->children[0]->type != SceneNode::Type::Path) {
      LOG_INFO("Canvas: begin_textbox_edit BAILED — no first canvas-view "
               "boundary (have_point={} resolved_by_point={})",
               have_point, resolved_by_point);
      return;
    }
    boundary = canvas_view->children[0].get();
    byte_start = 0;
  }
  if (!boundary) {
    LOG_INFO("Canvas: begin_textbox_edit BAILED — null boundary");
    return;
  }
  LOG_INFO("Canvas: begin_textbox_edit Mgr='{}' have_point={} pt=({:.2f},{:.2f}) "
           "→ resolved_by_point={} boundary_iid='{}' byte_start={}",
           text_box->name, have_point, doc_x, doc_y, resolved_by_point,
           boundary->internal_id, byte_start);

  m_text_editing = text_box;          // Mgr is the text-bearing node
  m_text_boundary_editing = boundary;
  m_text_is_new = false;
  m_text_snapshot = TextEditCommand::snapshot_before(project(), text_box);
  m_text_has_snapshot = true;

  // The Mgr stays selected as the user-visible atom — handles
  // continue to draw around its frame during edit, matching the
  // post-construction selection behaviour.
  m_selected = text_box;
  m_selection = {text_box};
  notify_object_selection_changed();

  begin_text_cursor_edit(text_box, boundary, byte_start);

  // s317 — Place the caret at the clicked position WITHIN the anchored
  //   member. byte_index_at maps the doc point through this region's
  //   layout (which starts at byte_start) to an absolute byte.
  if (have_point && m_text_cursor) {
    if (auto b = m_text_cursor->byte_index_at(doc_x, doc_y)) {
      m_text_cursor->set_byte_index(*b);
      // s318 — Collapse the anchor to the caret. The TextCursor ctor seeds
      //   the caret (and anchor) at the persisted/end-of-buffer byte; moving
      //   only the caret via set_byte_index would leave the anchor stranded
      //   at that old byte, producing a spurious selection that spans from
      //   the click to end-of-buffer — and, across a multi-member Mgr, that
      //   selection bridges region boundaries (the highlight bled into the
      //   other box). A plain double-click is an insertion point, not a
      //   selection, so collapse here.
      m_text_cursor->collapse_selection();
      LOG_INFO("Canvas: caret placed via byte_index_at → byte={} "
               "(region byte_start={})", *b, byte_start);
      queue_draw();
    } else {
      LOG_INFO("Canvas: byte_index_at FAILED for pt=({:.2f},{:.2f}) — caret "
               "stays at ctor default byte={} (may be outside this region's "
               "range → no visible caret)",
               doc_x, doc_y, m_text_cursor->byte_index());
    }
  }
}

// ── s301 m1g — Caret contrast color ─────────────────────────────────────────
// CurrentColor-style rule: the caret should be visible against whatever
// the artboard background is. Sample the doc's artboard color (same
// source the renderer uses), compute perceived luma, and return either
// white or black based on a 50% threshold. The threshold is the same
// one the system theme uses internally for "is this a dark background"
// decisions; works for the dark canvas the user is currently editing in
// AND for hypothetical light artboards (printed-paper-style designs).
void Canvas::caret_contrast_color(double& cr_r, double& cr_g, double& cr_b) const {
  // Default: white on dark. Used when m_doc isn't available.
  cr_r = cr_g = cr_b = 1.0;
  if (!m_doc) return;

  double bg_r = m_doc->artboard_bg_r(doc_motif());
  double bg_g = m_doc->artboard_bg_g(doc_motif());
  double bg_b = m_doc->artboard_bg_b(doc_motif());

  // ITU-R BT.601 luma weights — close enough for contrast decisions
  // and matches how every other "should I use light or dark text on
  // this background" decision in design tooling gets made.
  double luma = 0.299 * bg_r + 0.587 * bg_g + 0.114 * bg_b;
  if (luma > 0.5) {
    cr_r = cr_g = cr_b = 0.0;  // black on light
  } else {
    cr_r = cr_g = cr_b = 1.0;  // white on dark
  }
}

// ── s301 m1c — Canvas destructor ────────────────────────────────────────────
// Defaulted destructor moved out of the header so std::unique_ptr<TextCursor>
// sees the full type. Without this, every TU that includes Canvas.hpp would
// need to know about TextCursor — defeating the forward-decl. The blink
// connection is disconnected here too to avoid a dangling callback firing
// during teardown.
Canvas::~Canvas() {
  if (m_text_cursor_blink_conn.connected()) {
    m_text_cursor_blink_conn.disconnect();
  }
}

// ── s301 m1c — Begin an on-canvas text edit ─────────────────────────────────
// Installs a TextCursor for the given text node + boundary, starts the
// 530ms blink timer, and queues a redraw so the caret appears immediately.
// The blink timer toggles the cursor's visibility and queues a redraw on
// every tick; the connection is stored so we can disconnect on end.
//
// Per-platform-typical 530ms cadence (matches GTK/GNOME text widgets and
// is close to the Mac/Windows defaults). Could become a preference later.
void Canvas::begin_text_cursor_edit(SceneNode* text_node, SceneNode* boundary,
                                    size_t byte_start) {
  if (!text_node) return;
  // boundary may be null if a legacy unbound text somehow routes here —
  // TextCursor's position_on_canvas returns invalid in that case and the
  // caret simply doesn't render, but the buffer mutation still works.
  // When set (paired-sibling re-entry, TextBox edit, fresh construction
  // marquee), it's passed through to the cursor so it can lay out
  // geometry without needing to look the boundary up by iid — which
  // matters specifically for TextBox-owned text where the boundary is
  // a structural sibling, not an iid-linked peer.

  end_text_cursor_edit();  // idempotent: clear any prior cursor first
  m_text_cursor = std::make_unique<TextCursor>(this, text_node, boundary,
                                               byte_start);

  // Blink timer. 530ms is the visual cadence; we toggle visibility and
  // queue_draw each tick. Capturing `this` is safe because Canvas owns
  // the connection and disconnects it in end_text_cursor_edit / dtor.
  m_text_cursor_blink_conn = Glib::signal_timeout().connect([this]() -> bool {
    if (!m_text_cursor) return false;
    m_text_cursor->toggle_visible();
    queue_draw();
    return true;
  }, 530);

  // Make sure the canvas has focus so key events arrive.
  grab_focus();

  // s329 — announce the edit began. MainWindow shows the docked style bar off
  // this; the tab bar will subscribe too. Emitting at this single funnel covers
  // every edit-entry path.
  m_sig_text_edit_changed.emit(true);
  emit_text_style_changed();  // s330 — initial face read on entering the edit

  queue_draw();
}

void Canvas::end_text_cursor_edit() {
  const bool was_active = (bool)m_text_cursor;  // s329 — gate the end signal
  // s305 m1 — Write the cursor's byte position back to the text node
  //   before tearing down. The next entry to the same text (via
  //   double-click in either tool) restores from text_caret_byte in
  //   TextCursor's ctor. Both commit and cancel paths route through
  //   here, so the byte persists regardless of whether the user
  //   confirms or aborts the edit — the byte is a UI position, not
  //   a content edit.
  if (m_text_cursor && m_text_editing) {
    size_t b = m_text_cursor->byte_index();
    // Clamp into int32_t — buffers don't approach 2GB but defensive.
    if (b > (size_t)std::numeric_limits<int32_t>::max())
      b = (size_t)std::numeric_limits<int32_t>::max();
    m_text_editing->text_caret_byte = (int32_t)b;
  }
  if (m_text_cursor_blink_conn.connected()) {
    m_text_cursor_blink_conn.disconnect();
  }
  // s307 m5 — Disconnect the typing-pause timer alongside the blink
  //   timer. Without this, a pause timer armed in the last 600ms
  //   before teardown could fire after the cursor is gone. The
  //   callback already checks m_text_cursor as a defensive guard,
  //   but explicit disconnect at teardown is the cleaner contract.
  if (m_text_typing_pause_conn.connected()) {
    m_text_typing_pause_conn.disconnect();
  }
  m_text_cursor.reset();
  // s329 — announce the edit ended, but only if one was actually active.
  // end_text_cursor_edit also runs as the idempotent clear at the top of
  // begin_text_cursor_edit; without this guard a fresh edit would emit a
  // spurious false (hide) immediately before its true (show).
  if (was_active)
    m_sig_text_edit_changed.emit(false);
  queue_draw();
}

// s312 m2.3 — Hide the canvas caret while the popover holds focus.
//   Without this, the canvas-side caret keeps blinking under
//   m_text_cursor while the TextView's native caret blinks on top —
//   two visible cursors, breaking the one-caret-two-surfaces
//   illusion. Disconnecting the timer stops the toggle; forcing
//   visible=false ensures the next draw renders nothing for the
//   canvas caret regardless of whichever phase the blink was on.
//   queue_draw fires the redraw immediately so the user sees the
//   canvas caret vanish the moment focus crosses.
void Canvas::suspend_text_cursor_blink() {
  if (m_text_cursor_blink_conn.connected()) {
    m_text_cursor_blink_conn.disconnect();
  }
  if (m_text_cursor) {
    m_text_cursor->set_visible(false);
  }
  queue_draw();
}

// s312 m2.3 — Re-arm the blink timer + force-visible after the
//   popover dismisses. Called from the popover's signal_closed
//   handler so every dismiss path (cross_back, autohide-from-
//   click-outside, Escape inside popover) restores the canvas
//   caret. Idempotent: no cursor → no-op; timer already connected
//   → re-anchor visibility without double-connecting (a duplicate
//   connection would double the blink rate).
void Canvas::resume_text_cursor_blink() {
  if (!m_text_cursor) return;
  if (!m_text_cursor_blink_conn.connected()) {
    m_text_cursor_blink_conn = Glib::signal_timeout().connect(
        [this]() -> bool {
          if (!m_text_cursor) return false;
          m_text_cursor->toggle_visible();
          queue_draw();
          return true;
        }, 530);
  }
  m_text_cursor->set_visible(true);
  queue_draw();
}

// ── s301 m1c — Key dispatch for on-canvas text editing ──────────────────────
// Called from MainWindow_bindings.cpp's CAPTURE-phase key controller
// BEFORE the global shortcut cascade. Returns true if the keystroke was
// consumed by the active edit (the caller then returns true to stop
// further dispatch). Returns false in all other cases — including when
// the edit isn't active or the key is something we don't handle (e.g.
// Ctrl+Z still passes through to the global undo handler).
//
// Modifier policy:
//   - Plain key → buffer mutation or caret navigation.
//   - Ctrl+key → NOT consumed here; falls through so Ctrl+Z, Ctrl+S,
//     Ctrl+C/V/X reach their shortcut handlers. (Paste/copy/cut
//     integration with the cursor is a later milestone — for now
//     they're inert during edit.)
//   - Alt+key → not consumed (Alt is shortcut signal in this codebase).
//
// Commit triggers in this handler:
//   - Escape    → cancel (revert if new, restore snapshot if existing).
//                 MainWindow's existing Escape handler still calls
//                 cancel_text_edit; this returns true to stop further
//                 dispatch first only if we DON'T want global handling.
//                 Since we DO want cancel_text_edit to run (it lives in
//                 MainWindow_bindings.cpp's Esc cascade), we return
//                 false on Escape and let the cascade fire.
//   - Enter     → insert newline into the buffer (multi-line edit). The
//                 existing global Enter handler also calls
//                 commit_text_edit; we return true here to override
//                 that — Enter during canvas-cursor edit means newline,
//                 NOT commit.
// ── s326 m2 — Per-run character formatting toggle ───────────────────────────
// The keyboard spine for the styler: take the active selection, toggle one
// attribute over it, commit one undoable edit. The render + persistence are
// already banked (s325), and the span pump (curvz::utils) is proven in
// isolation, so this is just the wiring: selection -> pump -> commit.
//
// Commit model: a format toggle is a discrete edit, not part of a typing
// run, so flush_text_segment() first to close any in-flight typing as its
// own history step. flush re-snapshots to the current state when it pushes;
// when it no-ops (no pending typing) m_text_snapshot already reflects the
// current buffer -- either way m_text_snapshot.before_* is the pre-toggle
// state. Then push the span delta directly: flush's guard is content-only
// (before_content == text_content) and a toggle changes text_attr_spans,
// not text_content, so flush would no-op it -- record_after + push + re-
// snapshot here, mirroring flush's tail.
//
// m2 is selection-only: no selection -> no-op. The caret-insertion case
// (set the "pending format" for the next typed char) is the bar's job and
// comes with m3. Returns true when a toggle was applied.
bool Canvas::apply_text_format_toggle(int attr_type, long ivalue,
                                      const std::string& svalue) {
  if (!m_text_cursor || !m_text_editing) return false;
  auto [a, b] = m_text_cursor->selection_range();
  // s326 m2 — log at entry (before the selection gate) so a no-selection
  //   chord is distinguishable from a non-arriving keystroke in the trace.
  LOG_INFO("[s326] format toggle ENTER type={} sel=[{},{}) has_sel={}",
           attr_type, (unsigned)a, (unsigned)b, (a < b));
  if (a >= b) return false;  // selection-only in m2

  // Close any in-flight typing run as its own undo step.
  flush_text_segment();

  bool now_on = curvz::utils::toggle_attr_over_range(
      m_text_editing->text_attr_spans, attr_type, ivalue, svalue,
      (unsigned)a, (unsigned)b);

  // Push the span delta. (flush's content-only guard would skip it.)
  if (m_text_has_snapshot && m_history) {
    size_t cb = m_text_cursor->byte_index();
    if (cb > (size_t)std::numeric_limits<int32_t>::max())
      cb = (size_t)std::numeric_limits<int32_t>::max();
    m_text_editing->text_caret_byte = (int32_t)cb;

    m_text_snapshot.record_after(m_text_editing);
    m_history->push(
        std::make_unique<TextEditCommand>(std::move(m_text_snapshot)));
    // Re-open a fresh segment from the post-toggle state.
    m_text_snapshot = TextEditCommand::snapshot_before(project(), m_text_editing);
  }

  LOG_INFO("[s326] format toggle type={} ivalue={} range=[{},{}) -> {}",
           attr_type, ivalue, (unsigned)a, (unsigned)b,
           now_on ? "on" : "off");

  m_sig_doc_changed.emit();
  emit_text_style_changed();  // s330 — refresh faces after the toggle
  queue_draw();
  return true;
}

bool Canvas::apply_text_format_set(int attr_type, long ivalue,
                                   const std::string& svalue) {
  if (!m_text_cursor || !m_text_editing) return false;
  auto [a, b] = m_text_cursor->selection_range();
  LOG_INFO("[s330] format set ENTER type={} val={} sel=[{},{}) has_sel={}",
           attr_type, ivalue, (unsigned)a, (unsigned)b, (a < b));
  if (a >= b) return false;  // selection-only, matching the toggle path

  // Close any in-flight typing run as its own undo step.
  flush_text_segment();

  // Value-set: clear this attr in the range, then lay one span carrying the
  // picked value. (vs toggle_attr_over_range's flip.)
  curvz::utils::set_attr_over_range(
      m_text_editing->text_attr_spans, attr_type, ivalue, svalue,
      (unsigned)a, (unsigned)b);

  // Push the span delta. (flush's content-only guard would skip it.)
  if (m_text_has_snapshot && m_history) {
    size_t cb = m_text_cursor->byte_index();
    if (cb > (size_t)std::numeric_limits<int32_t>::max())
      cb = (size_t)std::numeric_limits<int32_t>::max();
    m_text_editing->text_caret_byte = (int32_t)cb;

    m_text_snapshot.record_after(m_text_editing);
    m_history->push(
        std::make_unique<TextEditCommand>(std::move(m_text_snapshot)));
    m_text_snapshot = TextEditCommand::snapshot_before(project(), m_text_editing);
  }

  LOG_INFO("[s330] format set type={} ivalue={} range=[{},{}) done",
           attr_type, ivalue, (unsigned)a, (unsigned)b);

  m_sig_doc_changed.emit();
  emit_text_style_changed();  // s330 — refresh faces after the set
  queue_draw();
  return true;
}

// s330 — style-bar live-read helpers. Sweep type-T spans clipped to [a,b),
// returning whether the effective value is uniform (one value everywhere,
// counting `def` wherever no span covers) and what that single value is. A
// bare caret (a==b) samples the byte before it. Family is string-valued;
// weight is int-valued — two variants rather than a template to stay readable.
namespace {
bool sweep_family_uniform(const std::vector<Curvz::AttrSpan>& spans,
                          const std::string& def, unsigned a, unsigned b,
                          std::string& out) {
  if (a == b) {  // caret: the font of the byte just before it
    unsigned p = (a > 0) ? a - 1 : 0;
    out = def;
    for (const auto& s : spans)
      if (s.type == PANGO_ATTR_FAMILY &&
          (unsigned)s.start_byte <= p && (unsigned)s.end_byte > p)
        out = s.svalue;
    return true;
  }
  std::vector<const Curvz::AttrSpan*> sp;
  for (const auto& s : spans)
    if (s.type == PANGO_ATTR_FAMILY &&
        (unsigned)s.end_byte > a && (unsigned)s.start_byte < b)
      sp.push_back(&s);
  std::sort(sp.begin(), sp.end(),
            [](const Curvz::AttrSpan* x, const Curvz::AttrSpan* y) {
              return x->start_byte < y->start_byte;
            });
  std::string first;
  bool have = false, mixed = false;
  unsigned cur = a;
  auto note = [&](const std::string& v) {
    if (!have) { first = v; have = true; }
    else if (v != first) mixed = true;
  };
  for (auto* s : sp) {
    unsigned ss = std::max((unsigned)s->start_byte, a);
    unsigned se = std::min((unsigned)s->end_byte, b);
    if (ss > cur) note(def);  // gap before this span -> node default
    note(s->svalue);
    cur = std::max(cur, se);
    if (mixed) break;
  }
  if (!mixed && cur < b) note(def);  // trailing gap
  out = have ? first : def;
  return !mixed;
}

bool sweep_weight_uniform(const std::vector<Curvz::AttrSpan>& spans,
                          long def, unsigned a, unsigned b, long& out) {
  if (a == b) {
    unsigned p = (a > 0) ? a - 1 : 0;
    out = def;
    for (const auto& s : spans)
      if (s.type == PANGO_ATTR_WEIGHT &&
          (unsigned)s.start_byte <= p && (unsigned)s.end_byte > p)
        out = s.ivalue;
    return true;
  }
  std::vector<const Curvz::AttrSpan*> sp;
  for (const auto& s : spans)
    if (s.type == PANGO_ATTR_WEIGHT &&
        (unsigned)s.end_byte > a && (unsigned)s.start_byte < b)
      sp.push_back(&s);
  std::sort(sp.begin(), sp.end(),
            [](const Curvz::AttrSpan* x, const Curvz::AttrSpan* y) {
              return x->start_byte < y->start_byte;
            });
  long first = 0;
  bool have = false, mixed = false;
  unsigned cur = a;
  auto note = [&](long v) {
    if (!have) { first = v; have = true; }
    else if (v != first) mixed = true;
  };
  for (auto* s : sp) {
    unsigned ss = std::max((unsigned)s->start_byte, a);
    unsigned se = std::min((unsigned)s->end_byte, b);
    if (ss > cur) note(def);
    note(s->ivalue);
    cur = std::max(cur, se);
    if (mixed) break;
  }
  if (!mixed && cur < b) note(def);
  out = have ? first : def;
  return !mixed;
}

// s331 — per-decoration lit-state sweep. Returns a tri-state over [a,b):
// 0 = off everywhere, 1 = on everywhere, 2 = mixed. A byte is "on" iff the
// last span of `type` covering it has a non-zero value, OR (no span covers
// it AND def_on) — i.e. gaps inherit the node default. "Non-zero" rather
// than a specific value so any underline variant / oblique-or-italic counts
// as on, matching the user's "is this emphasised?" reading. A bare caret
// (a==b) samples the byte before it and reports off(0)/on(1), never mixed.
int sweep_decoration(const std::vector<Curvz::AttrSpan>& spans, int type,
                     bool def_on, unsigned a, unsigned b) {
  auto byte_on = [&](unsigned p) -> bool {
    bool covered = false; long v = 0;
    for (const auto& s : spans)
      if (s.type == type &&
          (unsigned)s.start_byte <= p && (unsigned)s.end_byte > p) {
        covered = true; v = s.ivalue;  // last covering span wins
      }
    return covered ? (v != 0) : def_on;
  };
  if (a == b) {  // caret: sample the byte just before it
    unsigned p = (a > 0) ? a - 1 : 0;
    return byte_on(p) ? 1 : 0;
  }
  bool any_on = false, any_off = false;
  for (unsigned p = a; p < b; ++p) {
    if (byte_on(p)) any_on = true; else any_off = true;
    if (any_on && any_off) return 2;  // mixed — stop early
  }
  return any_on ? 1 : 0;
}

// s331 — size lit-read. Per-run sizes live as PANGO_ATTR_SIZE in Pango units
// (point x PANGO_SCALE) so they round-trip cleanly through the markup `size`
// wire form. Sweep clipped to [a,b): uniform value (counting `def` in gaps)
// or mixed. `def` is the node default already expressed in the same point-
// scaled units. Mirrors sweep_weight_uniform.
bool sweep_size_uniform(const std::vector<Curvz::AttrSpan>& spans,
                        long def, unsigned a, unsigned b, long& out) {
  if (a == b) {
    unsigned p = (a > 0) ? a - 1 : 0;
    out = def;
    for (const auto& s : spans)
      if (s.type == PANGO_ATTR_SIZE &&
          (unsigned)s.start_byte <= p && (unsigned)s.end_byte > p)
        out = s.ivalue;
    return true;
  }
  std::vector<const Curvz::AttrSpan*> sp;
  for (const auto& s : spans)
    if (s.type == PANGO_ATTR_SIZE &&
        (unsigned)s.end_byte > a && (unsigned)s.start_byte < b)
      sp.push_back(&s);
  std::sort(sp.begin(), sp.end(),
            [](const Curvz::AttrSpan* x, const Curvz::AttrSpan* y) {
              return x->start_byte < y->start_byte;
            });
  long first = 0;
  bool have = false, mixed = false;
  unsigned cur = a;
  auto note = [&](long v) {
    if (!have) { first = v; have = true; }
    else if (v != first) mixed = true;
  };
  for (auto* s : sp) {
    unsigned ss = std::max((unsigned)s->start_byte, a);
    unsigned se = std::min((unsigned)s->end_byte, b);
    if (ss > cur) note(def);
    note(s->ivalue);
    cur = std::max(cur, se);
    if (mixed) break;
  }
  if (!mixed && cur < b) note(def);
  out = have ? first : def;
  return !mixed;
}

// s332 — foreground (text fill) lit-read. Per-run fill lives as
// PANGO_ATTR_FOREGROUND spans carrying a packed 0xRRGGBB ivalue (the same
// fixed form render/encode/decode round-trip). `def` is the node's own fill
// colour packed the same way, folded into any gap. Sweep clipped to [a,b):
// uniform value or mixed. Structurally identical to sweep_size_uniform — only
// the attr type differs — but kept as its own function to stay readable
// alongside the other per-type sweeps.
bool sweep_foreground_uniform(const std::vector<Curvz::AttrSpan>& spans,
                              long def, unsigned a, unsigned b, long& out) {
  if (a == b) {
    unsigned p = (a > 0) ? a - 1 : 0;
    out = def;
    for (const auto& s : spans)
      if (s.type == PANGO_ATTR_FOREGROUND &&
          (unsigned)s.start_byte <= p && (unsigned)s.end_byte > p)
        out = s.ivalue;
    return true;
  }
  std::vector<const Curvz::AttrSpan*> sp;
  for (const auto& s : spans)
    if (s.type == PANGO_ATTR_FOREGROUND &&
        (unsigned)s.end_byte > a && (unsigned)s.start_byte < b)
      sp.push_back(&s);
  std::sort(sp.begin(), sp.end(),
            [](const Curvz::AttrSpan* x, const Curvz::AttrSpan* y) {
              return x->start_byte < y->start_byte;
            });
  long first = 0;
  bool have = false, mixed = false;
  unsigned cur = a;
  auto note = [&](long v) {
    if (!have) { first = v; have = true; }
    else if (v != first) mixed = true;
  };
  for (auto* s : sp) {
    unsigned ss = std::max((unsigned)s->start_byte, a);
    unsigned se = std::min((unsigned)s->end_byte, b);
    if (ss > cur) note(def);
    note(s->ivalue);
    cur = std::max(cur, se);
    if (mixed) break;
  }
  if (!mixed && cur < b) note(def);
  out = have ? first : def;
  return !mixed;
}
} // namespace

bool Canvas::text_style_query_family(Glib::ustring& out_family,
                                     bool& out_mixed) const {
  if (!m_text_cursor || !m_text_editing) return false;
  auto [a, b] = m_text_cursor->selection_range();
  std::string fam;
  bool uniform = sweep_family_uniform(m_text_editing->text_attr_spans,
                                      m_text_editing->text_font_family,
                                      (unsigned)a, (unsigned)b, fam);
  out_mixed = !uniform;
  out_family = fam;
  return true;
}

bool Canvas::text_style_query_weight(long& out_weight, bool& out_mixed) const {
  if (!m_text_cursor || !m_text_editing) return false;
  auto [a, b] = m_text_cursor->selection_range();
  long def = m_text_editing->text_bold ? 700 : 400;
  long w = def;
  bool uniform = sweep_weight_uniform(m_text_editing->text_attr_spans,
                                      def, (unsigned)a, (unsigned)b, w);
  out_mixed = !uniform;
  out_weight = w;
  return true;
}

bool Canvas::text_style_query_emphasis(int& out_italic, int& out_underline,
                                       int& out_strike, int& out_overline) const {
  if (!m_text_cursor || !m_text_editing) return false;
  auto [a, b] = m_text_cursor->selection_range();
  const auto& spans = m_text_editing->text_attr_spans;
  // Italic gaps inherit the node's scalar italic default; the other three
  // decorations have no node-level scalar, so their gaps are off.
  out_italic    = sweep_decoration(spans, PANGO_ATTR_STYLE,
                                   m_text_editing->text_italic,
                                   (unsigned)a, (unsigned)b);
  out_underline = sweep_decoration(spans, PANGO_ATTR_UNDERLINE,     false,
                                   (unsigned)a, (unsigned)b);
  out_strike    = sweep_decoration(spans, PANGO_ATTR_STRIKETHROUGH, false,
                                   (unsigned)a, (unsigned)b);
  out_overline  = sweep_decoration(spans, PANGO_ATTR_OVERLINE,      false,
                                   (unsigned)a, (unsigned)b);
  return true;
}

bool Canvas::text_style_query_size(double& out_pt, bool& out_mixed) const {
  if (!m_text_cursor || !m_text_editing) return false;
  auto [a, b] = m_text_cursor->selection_range();
  // Node default: text_font_size is doc-px (user units); the spans are point-
  // scaled, so express the default in the same units for an apples-to-apples
  // sweep. 1 pt = 96/72 px, via UnitSystem (pure typographic, doc-independent).
  long def = std::lround(
      UnitSystem::from_px(m_text_editing->text_font_size, Unit::Pt) * PANGO_SCALE);
  long v = def;
  bool uniform = sweep_size_uniform(m_text_editing->text_attr_spans,
                                    def, (unsigned)a, (unsigned)b, v);
  out_mixed = !uniform;
  out_pt = (double)v / (double)PANGO_SCALE;
  return true;
}

bool Canvas::text_style_query_fill(unsigned long& out_rgb, bool& out_mixed,
                                   bool& out_none) const {
  if (!m_text_cursor || !m_text_editing) return false;
  auto [a, b] = m_text_cursor->selection_range();
  // Node default: the text object's own fill paint, packed 0xRRGGBB the same
  // way the FOREGROUND spans are. (Foreground spans carry only a solid colour;
  // gradient / swatch object fills collapse to their stored solid r/g/b for
  // this read, which is what the swatch face shows when no run overrides.)
  const FillStyle& f = m_text_editing->fill;
  auto pack = [](double r, double g, double b) -> long {
    auto ch = [](double v) {
      return (long)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
    };
    return (ch(r) << 16) | (ch(g) << 8) | ch(b);
  };
  long def = pack(f.r, f.g, f.b);
  long v = def;
  bool uniform = sweep_foreground_uniform(m_text_editing->text_attr_spans,
                                          def, (unsigned)a, (unsigned)b, v);
  out_mixed = !uniform;
  out_rgb = (unsigned long)(v & 0xFFFFFF);
  // "None" only when the effective colour is the node default AND that default
  // is a None / fully-transparent paint with no per-run override in range. A
  // per-run FOREGROUND span is always a real colour, so a resolved span never
  // reads as none.
  bool def_is_none = (f.type == FillStyle::Type::None) || (f.a <= 0.0);
  out_none = !out_mixed && def_is_none && (v == def);
  return true;
}

bool Canvas::text_style_query_stroke(unsigned long& out_rgb,
                                     bool& out_has_color,
                                     double& out_width_px) const {
  if (!m_text_cursor || !m_text_editing) return false;
  auto [a, b] = m_text_cursor->selection_range();
  // Representative byte: selection start, or the byte before a bare caret.
  unsigned p = (a < b) ? (unsigned)a : (a > 0 ? (unsigned)a - 1 : 0);
  long color = -2;     // -2 = no stroke-colour span here
  long wscaled = -1;   // no width span here
  for (const auto& s : m_text_editing->text_attr_spans) {
    if ((unsigned)s.start_byte <= p && (unsigned)s.end_byte > p) {
      if      (s.type == curvz::utils::kCurvzStrokeColorAttr) color   = s.ivalue;
      else if (s.type == curvz::utils::kCurvzStrokeWidthAttr) wscaled = s.ivalue;
    }
  }
  if (color == -2) {
    // No per-run span here: text has no stroke (the object/box stroke is the
    // inspector's domain and does not apply to text glyphs).
    out_has_color = false;
    out_rgb = 0;
    out_width_px = 0.0;
    return true;
  }
  if (color == curvz::utils::kCurvzStrokeNone) {  // explicit no-stroke
    out_has_color = false;
    out_rgb = 0;
    out_width_px = (wscaled >= 0) ? (double)wscaled / (double)PANGO_SCALE : 0.0;
    return true;
  }
  out_has_color = true;
  out_rgb = (unsigned long)(color & 0xFFFFFF);
  out_width_px = (wscaled >= 0) ? (double)wscaled / (double)PANGO_SCALE
                                : UnitSystem::to_px(1.0, Unit::Pt);
  return true;
}

bool Canvas::text_style_query_leading(double& out_pt, bool& out_auto) const {
  if (!m_text_cursor || !m_text_editing) return false;
  const std::string& buf = m_text_editing->text_content;
  auto [a, b] = m_text_cursor->selection_range();
  // Snap to the caret paragraph (run between '\n' breaks); sample its start.
  unsigned pa = (unsigned)std::min<size_t>(a, buf.size());
  while (pa > 0 && buf[pa - 1] != '\n') --pa;
  // A per-paragraph leading run covering the paragraph start wins.
  for (const auto& s : m_text_editing->text_attr_spans) {
    if (s.type == curvz::utils::kCurvzLeadingAttr &&
        (unsigned)s.start_byte <= pa && (unsigned)s.end_byte > pa) {
      out_auto = false;
      out_pt = UnitSystem::from_px((double)s.ivalue / (double)PANGO_SCALE,
                                   Unit::Pt);
      return true;
    }
  }
  // No run: legacy buffer-global scalar, else the true metric-derived auto
  // leading (same (ascent+descent)*1.2 the flow strides — not font x 1.2).
  double lh = m_text_editing->text_line_height;
  out_auto = !(lh > 0.0);
  double px = (lh > 0.0) ? lh : metric_leading_px(m_text_editing);
  out_pt = UnitSystem::from_px(px, Unit::Pt);
  return true;
}

bool Canvas::text_style_query_alignment(int& out_align) const {
  if (!m_text_cursor || !m_text_editing) return false;
  const std::string& buf = m_text_editing->text_content;
  auto [a, b] = m_text_cursor->selection_range();
  (void)b;
  // Sample the caret paragraph start (run between '\n' breaks), like leading.
  unsigned pa = (unsigned)std::min<size_t>(a, buf.size());
  while (pa > 0 && buf[pa - 1] != '\n') --pa;
  out_align = 0;  // default = left (no align run)
  for (const auto& s : m_text_editing->text_attr_spans) {
    if (s.type == curvz::utils::kCurvzAlignAttr &&
        (unsigned)s.start_byte <= pa && (unsigned)s.end_byte > pa) {
      out_align = (int)s.ivalue;
      break;
    }
  }
  return true;
}

bool Canvas::handle_text_edit_key(guint keyval, Gdk::ModifierType mods) {
  if (!m_text_cursor) return false;

  bool ctrl  = (mods & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
  bool alt   = (mods & Gdk::ModifierType::ALT_MASK)     != Gdk::ModifierType{};
  bool shift = (mods & Gdk::ModifierType::SHIFT_MASK)   != Gdk::ModifierType{};

  // s305 m4 — Ctrl+A: select-all inside the active edit. Intercepted
  //   ABOVE the ctrl-passthrough gate so the global Select All shortcut
  //   doesn't fire on the canvas selection — during a text edit the
  //   user means "select all the text," not "select all the objects."
  //   Ctrl+C/X/V are still passed through here (they hit the global
  //   handler in m5 which will know about text-edit context).
  if (ctrl && !alt && !shift &&
      (keyval == GDK_KEY_a || keyval == GDK_KEY_A)) {
    m_text_cursor->select_all();
    m_text_cursor->set_visible(true);
    queue_draw();
    return true;
  }

  // s326 m2 — Ctrl+B / Ctrl+I / Ctrl+U: per-run character formatting on the
  //   current selection. Intercepted above the ctrl-passthrough gate (same
  //   reason as Ctrl+A): the chord means "format the selected text," not any
  //   global shortcut. No selection -> no-op (the bar's chips will drive the
  //   caret-insertion-style case later; m2 is selection-only). The attribute
  //   values are the Pango constants, passed as ints so this TU stays
  //   pango-free: weight bold = PANGO_WEIGHT_BOLD (700), style italic =
  //   PANGO_STYLE_ITALIC (2), underline single = PANGO_UNDERLINE_SINGLE (1);
  //   the attr-type ids are PANGO_ATTR_WEIGHT (4), PANGO_ATTR_STYLE (3),
  //   PANGO_ATTR_UNDERLINE (11). s326 — pass the real PangoAttrType symbols
  //   (NOT hardcoded ints) so they match build_line_attrs / encode_markup;
  //   a prior transcription used 8/5/10 (FONT_DESC/VARIANT/BACKGROUND), which
  //   every consumer dropped, so toggles never rendered or saved.
  if (ctrl && !alt && !shift &&
      (keyval == GDK_KEY_b || keyval == GDK_KEY_B)) {
    apply_text_format_toggle(PANGO_ATTR_WEIGHT,
                             /*PANGO_WEIGHT_BOLD*/ 700, "");
    return true;
  }
  if (ctrl && !alt && !shift &&
      (keyval == GDK_KEY_i || keyval == GDK_KEY_I)) {
    apply_text_format_toggle(PANGO_ATTR_STYLE,
                             /*PANGO_STYLE_ITALIC*/ 2, "");
    return true;
  }
  if (ctrl && !alt && !shift &&
      (keyval == GDK_KEY_u || keyval == GDK_KEY_U)) {
    apply_text_format_toggle(PANGO_ATTR_UNDERLINE,
                             /*PANGO_UNDERLINE_SINGLE*/ 1, "");
    return true;
  }

  // s305 m4 — Ctrl+Home / Ctrl+End: caret to start / end of buffer.
  //   Shift variants extend the selection. These need to intercept
  //   above the ctrl-passthrough gate for the same reason as Ctrl+A:
  //   without it, the global handler eats the chord and the
  //   text-edit caret never sees it. (On Apple keyboards Ctrl+Home
  //   is Fn+Ctrl+Left and Ctrl+End is Fn+Ctrl+Right — the kernel
  //   translates Fn+arrow to the Home/End keysyms before GTK sees
  //   the event, so this code is keyboard-agnostic.)
  //
  //   Shift-extend uses the same snapshot-and-restore pattern as
  //   the other Shift+navigation keys below. Inlined here rather
  //   than reusing the extend_with lambda because the lambda is
  //   declared after this block (extends only the plain-Arrow /
  //   Home / End paths). Refactoring to share would mean moving
  //   extend_with up; the duplication is small enough to be
  //   tolerable.
  if (ctrl && !alt && keyval == GDK_KEY_Home) {
    if (shift) {
      size_t saved_anchor = m_text_cursor->anchor_byte();
      m_text_cursor->move_buffer_start();
      m_text_cursor->set_anchor_byte(saved_anchor);
    } else {
      // s307 m3 — Caret motion that leaves the current typing run.
      //   Flush any in-flight typing before moving so subsequent typing
      //   at the new caret position becomes its own segment. No post-
      //   flush — motion doesn't mutate the buffer. Shift-extend is
      //   selection growth, not navigation away — no flush.
      flush_text_segment();
      m_text_cursor->move_buffer_start();
    }
    m_text_cursor->set_visible(true);
    queue_draw();
    return true;
  }
  if (ctrl && !alt && keyval == GDK_KEY_End) {
    if (shift) {
      size_t saved_anchor = m_text_cursor->anchor_byte();
      m_text_cursor->move_buffer_end();
      m_text_cursor->set_anchor_byte(saved_anchor);
    } else {
      flush_text_segment();  // s307 m3
      m_text_cursor->move_buffer_end();
    }
    m_text_cursor->set_visible(true);
    queue_draw();
    return true;
  }

  // ── s307 m6 — Mid-edit Ctrl+Z / Ctrl+Shift+Z ─────────────────────────────
  //
  //   Without these intercepts, Ctrl+Z mid-edit falls through to the
  //   global MainWindow::on_undo handler, which applies the top
  //   TextEditCommand's before_content to the buffer directly —
  //   bypassing the cursor entirely. The cursor's m_byte_index becomes
  //   stale (pointing past the new buffer size), and the in-flight
  //   segment's before_content also becomes stale (snapshot taken
  //   before the global undo ran). m5b's defensive clamps in
  //   TextCursor prevent the crash from a stale byte_index, but the
  //   in-flight stale snapshot causes a different bug: the next flush
  //   pushes a TextEditCommand with a desynced before_content, AND
  //   clears the redo stack of legitimately-queued redo commands.
  //
  //   The fix: intercept Ctrl+Z (and Ctrl+Shift+Z) above the
  //   ctrl-passthrough gate, do the right thing manually, and don't
  //   let the global handler see the keystroke.
  //
  //   Mid-edit Ctrl+Z:
  //     1. Flush the in-flight segment so it lands on history as a
  //        real command. After this, m_text_snapshot is a fresh empty
  //        segment.
  //     2. Peek the top of the undo stack. If it's a TextEditCommand
  //        for THIS textbox, proceed; otherwise no-op (the user's
  //        Ctrl+Z in textbox edit mode means "undo my text edit," not
  //        "undo some unrelated prior operation").
  //     3. Read the command's before_caret_byte for caret restore.
  //     4. Call m_history->undo() — applies before_content to the
  //        buffer and moves the command to redo.
  //     5. Restore the cursor's caret to before_caret_byte (with
  //        anchor collapse).
  //     6. Re-snapshot the in-flight segment from the now-current
  //        node state so subsequent typing has an accurate baseline.
  //
  //   Mid-edit Ctrl+Shift+Z (redo): symmetric.
  //     1. Flush in-flight (so the redo-stack-clear in push doesn't
  //        wipe the redo branch — but actually flush ALSO clears
  //        redo via push's contract, so we have to choose: either
  //        we flush BEFORE peeking redo (which wipes the redo we
  //        wanted), or we DON'T flush and accept that a non-empty
  //        in-flight segment is lost. Better choice: don't flush;
  //        if there's an in-flight delta, the user pressed redo
  //        with un-committed typing, which is unusual, but the
  //        right semantic is "their typing was uncommitted, redo
  //        of a prior segment now happens, the un-typed buffer
  //        state is what it is." If we DID flush, the user would
  //        see two distinct undo steps land on the stack (their
  //        new typing + the redone segment) with the redo branch
  //        gone, which is worse.)
  //     2. Peek the top of the redo stack. If TextEditCommand for
  //        this textbox, proceed.
  //     3. Read after_caret_byte for caret restore.
  //     4. Call m_history->redo() — applies after_content, moves
  //        command back to undo.
  //     5. Restore cursor caret to after_caret_byte.
  //     6. Re-snapshot.
  if (ctrl && !alt && !shift &&
      (keyval == GDK_KEY_z || keyval == GDK_KEY_Z)) {
    flush_text_segment();
    auto* peek = dynamic_cast<const TextEditCommand*>(m_history->peek_undo());
    if (peek && peek->obj_iid == m_text_editing->internal_id) {
      size_t target_caret = peek->before_caret_byte;
      m_history->undo();
      // Clamp the restored caret against the post-undo buffer in
      // case before_caret_byte was captured before m6 landed (legacy
      // commands have 0) or is somehow past the new size.
      size_t bsize = m_text_editing->text_content.size();
      if (target_caret > bsize) target_caret = bsize;
      m_text_cursor->set_byte_index(target_caret);
      m_text_cursor->set_anchor_byte(target_caret);
      // Re-snapshot so the in-flight segment matches the new buffer.
      m_text_editing->text_caret_byte = (int32_t)target_caret;
      m_text_snapshot = TextEditCommand::snapshot_before(project(),
                                                         m_text_editing);
      m_text_has_snapshot = true;
      m_text_cursor->set_visible(true);
      m_sig_doc_changed.emit();
      queue_draw();
    }
    // If the top isn't a TextEditCommand for this textbox, silently
    // no-op. Don't fall through to global undo — that would touch
    // unrelated state while the user is mid-edit, which is the
    // bug we just fixed.
    return true;
  }
  if (ctrl && !alt && shift &&
      (keyval == GDK_KEY_z || keyval == GDK_KEY_Z)) {
    auto* peek = dynamic_cast<const TextEditCommand*>(m_history->peek_redo());
    if (peek && peek->obj_iid == m_text_editing->internal_id) {
      size_t target_caret = peek->after_caret_byte;
      m_history->redo();
      size_t bsize = m_text_editing->text_content.size();
      if (target_caret > bsize) target_caret = bsize;
      m_text_cursor->set_byte_index(target_caret);
      m_text_cursor->set_anchor_byte(target_caret);
      m_text_editing->text_caret_byte = (int32_t)target_caret;
      m_text_snapshot = TextEditCommand::snapshot_before(project(),
                                                         m_text_editing);
      m_text_has_snapshot = true;
      m_text_cursor->set_visible(true);
      m_sig_doc_changed.emit();
      queue_draw();
    }
    return true;
  }
  // Ctrl+Y as alt-redo binding — mirror the symmetry from MainWindow.
  if (ctrl && !alt && !shift &&
      (keyval == GDK_KEY_y || keyval == GDK_KEY_Y)) {
    auto* peek = dynamic_cast<const TextEditCommand*>(m_history->peek_redo());
    if (peek && peek->obj_iid == m_text_editing->internal_id) {
      size_t target_caret = peek->after_caret_byte;
      m_history->redo();
      size_t bsize = m_text_editing->text_content.size();
      if (target_caret > bsize) target_caret = bsize;
      m_text_cursor->set_byte_index(target_caret);
      m_text_cursor->set_anchor_byte(target_caret);
      m_text_editing->text_caret_byte = (int32_t)target_caret;
      m_text_snapshot = TextEditCommand::snapshot_before(project(),
                                                         m_text_editing);
      m_text_has_snapshot = true;
      m_text_cursor->set_visible(true);
      m_sig_doc_changed.emit();
      queue_draw();
    }
    return true;
  }

  // ── s306 m5 — Clipboard inside the text edit ─────────────────────────────
  //
  //   Ctrl+C / Ctrl+X / Ctrl+V intercept above the ctrl-passthrough gate
  //   (same as Ctrl+A and Ctrl+Home/End) so the global object-clipboard
  //   handler in MainWindow_bindings doesn't fire during a text edit.
  //   The global handler runs copy_selected / cut_selected /
  //   paste_clipboard on the object selection; during a text edit the
  //   user means "copy the highlighted bytes," not "copy the TextBox."
  //
  //   System clipboard (Gdk::Clipboard) via the display, matching the
  //   pattern used elsewhere in the codebase (Ruler labels copy,
  //   ClipboardViewWindow async-read). The model side is all done:
  //   selection_text() returns the UTF-8 slice, delete_selection()
  //   collapses the range, insert_string() splices arbitrary UTF-8.
  //
  //   - Ctrl+C: write selection_text to clipboard. No buffer mutation
  //     (no doc_changed emit). Silently no-ops if no selection — same
  //     as every text editor.
  //   - Ctrl+X: write selection_text to clipboard, then delete_selection.
  //     One doc_changed emit at the end. Silently no-ops if no selection.
  //   - Ctrl+V: async read_text_async; on callback, delete_selection if
  //     selection active, then insert_string. The async read means the
  //     cursor could be torn down before the callback fires (user hits
  //     Escape mid-read). Guard the callback with text_cursor_active()
  //     and a re-grab of m_text_cursor.get() — if either fails we
  //     silently drop the paste. The captured this is safe because
  //     Canvas outlives any plausible async read; clipboard async ops
  //     are single-shot and complete within a few ms in practice.
  if (ctrl && !alt && !shift &&
      (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) {
    if (m_text_cursor->has_selection()) {
      std::string sel = m_text_cursor->selection_text();
      auto disp = get_display();
      if (disp) {
        auto clip = disp->get_clipboard();
        if (clip) {
          Glib::Value<Glib::ustring> val;
          val.init(Glib::Value<Glib::ustring>::value_type());
          val.set(Glib::ustring(sel));
          clip->set_content(Gdk::ContentProvider::create(val));
        }
      }
    }
    m_text_cursor->set_visible(true);
    return true;
  }
  if (ctrl && !alt && !shift &&
      (keyval == GDK_KEY_x || keyval == GDK_KEY_X)) {
    if (m_text_cursor->has_selection()) {
      // s307 m2 — Cut is an atomic event. Flush any in-flight typing
      //   first so the cut doesn't get fused with what came before,
      //   then perform the cut, then flush the cut itself as its own
      //   segment. Ctrl+Z after cut restores the cut content; Ctrl+Z
      //   again rewinds any pre-cut typing.
      flush_text_segment();
      std::string sel = m_text_cursor->selection_text();
      auto disp = get_display();
      if (disp) {
        auto clip = disp->get_clipboard();
        if (clip) {
          Glib::Value<Glib::ustring> val;
          val.init(Glib::Value<Glib::ustring>::value_type());
          val.set(Glib::ustring(sel));
          clip->set_content(Gdk::ContentProvider::create(val));
        }
      }
      m_text_cursor->delete_selection();
      flush_text_segment();  // cut becomes its own segment
      m_sig_doc_changed.emit();
      queue_draw();
    }
    m_text_cursor->set_visible(true);
    return true;
  }
  if (ctrl && !alt && !shift &&
      (keyval == GDK_KEY_v || keyval == GDK_KEY_V)) {
    auto disp = get_display();
    if (disp) {
      auto clip = disp->get_clipboard();
      if (clip) {
        // s307 m2 — Paste is an atomic event. Flush any in-flight
        //   typing BEFORE dispatching the async clipboard read so the
        //   paste doesn't get fused with what came before. The
        //   post-paste flush has to live inside the async callback —
        //   placing it synchronously here would fire before insert_string
        //   has run, see zero delta, and no-op. The two flushes bracket
        //   the paste mutation so it becomes its own segment.
        flush_text_segment();

        // Capture the text-node pointer we're editing so we can verify
        // the same edit session is still live when the async callback
        // fires. If the user committed/cancelled and started editing a
        // different node, or no edit is active at all, drop the paste.
        SceneNode* edit_node = m_text_cursor->text_node();
        clip->read_text_async(
            [this, edit_node, clip](Glib::RefPtr<Gio::AsyncResult>& res) {
              if (!m_text_cursor) return;
              if (m_text_cursor->text_node() != edit_node) return;
              try {
                Glib::ustring text = clip->read_text_finish(res);
                if (text.empty()) return;
                std::string utf8(text.c_str());
                if (m_text_cursor->has_selection()) {
                  m_text_cursor->delete_selection();
                }
                if (m_text_cursor->insert_string(utf8)) {
                  // s307 m2 — Post-paste flush. The paste mutation is
                  //   now in the buffer; flush_text_segment captures it
                  //   as its own segment and opens a fresh one for any
                  //   subsequent typing. If insert_string somehow
                  //   no-op'd (shouldn't, but defensive), flush sees
                  //   zero delta and does nothing — same shape as a
                  //   no-typing deactivate.
                  flush_text_segment();
                  m_text_cursor->set_visible(true);
                  m_sig_doc_changed.emit();
                  queue_draw();
                }
              } catch (const Glib::Error&) {
                // Clipboard had no text representation, or read failed.
                // Silently drop — every text editor does the same.
              }
            });
      }
    }
    m_text_cursor->set_visible(true);
    return true;
  }

  if (ctrl || alt) return false;  // other shortcuts pass through

  // Reset blink to visible on any user input so the cursor doesn't
  // disappear mid-keystroke. Brief but noticeable polish.
  if (m_text_cursor) m_text_cursor->set_visible(true);

  // s305 m4 — Shift-extend helper. The base move_* methods always
  //   collapse anchor=caret (m2's default for callers that don't
  //   know about selection). To extend, snapshot the anchor before
  //   the move and restore it after. The caret end is whatever the
  //   move method computed; the anchor end stays at the press point.
  //   Closure captures m_text_cursor by reference so it's always
  //   the current cursor.
  auto extend_with = [this](auto&& mover) {
    size_t saved_anchor = m_text_cursor->anchor_byte();
    mover();
    m_text_cursor->set_anchor_byte(saved_anchor);
  };

  switch (keyval) {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      // s305 m4 — Enter while selection active: replace the range
      //   with a newline. delete_selection runs first; then the
      //   newline inserts at the collapsed caret. Matches every
      //   text editor.
      // s307 m2 — Enter declares a thought boundary. Flush any
      //   in-flight typing first, perform the newline (with
      //   selection-replace if applicable, as one user gesture),
      //   then flush the newline itself as its own segment. Ctrl+Z
      //   peels back exactly the Enter press; the typing before it
      //   stays.
      flush_text_segment();
      if (m_text_cursor->has_selection()) {
        m_text_cursor->delete_selection();
      }
      m_text_cursor->insert_newline();
      flush_text_segment();
      m_sig_doc_changed.emit();
      queue_draw();
      return true;

    case GDK_KEY_BackSpace:
      // s305 m4 — Backspace with selection: delete the range,
      //   leave the caret at the range start. No further char
      //   delete (the selection IS the deletion).
      // s307 m2 — Selection-delete is an atomic event regardless
      //   of which key triggered it (Backspace, Delete, or any
      //   printable). Flush before, delete, flush the deletion as
      //   its own segment. Plain Backspace (no selection) folds
      //   into the typing run — no flush.
      if (m_text_cursor->has_selection()) {
        flush_text_segment();
        m_text_cursor->delete_selection();
        flush_text_segment();
        m_sig_doc_changed.emit();
        queue_draw();
      } else if (m_text_cursor->backspace()) {
        // s307 m5 — Backspace is correction-mode typing; re-arm the
        //   pause timer so a thinking pause after deletions also
        //   becomes an undo boundary.
        restart_text_typing_pause_timer();
        m_sig_doc_changed.emit();
        queue_draw();
      }
      return true;

    case GDK_KEY_Delete:
      // s305 m4 — Delete with selection: same shape as Backspace
      //   above. The selection IS the deletion; forward-delete
      //   would otherwise eat one extra codepoint past the
      //   selection's end, which no text editor does.
      // s307 m2 — Same flush bracket as Backspace-with-selection.
      //   Plain Delete (no selection) folds into the typing run.
      if (m_text_cursor->has_selection()) {
        flush_text_segment();
        m_text_cursor->delete_selection();
        flush_text_segment();
        m_sig_doc_changed.emit();
        queue_draw();
      } else if (m_text_cursor->delete_forward()) {
        // s307 m5 — Same as Backspace: re-arm pause timer.
        restart_text_typing_pause_timer();
        m_sig_doc_changed.emit();
        queue_draw();
      }
      return true;

    case GDK_KEY_Left:
      // s319 — Caret region flow. At the current region's first byte, plain
      //   Left steps out: from the OA back to the tail member (closes the
      //   OA); from a member back to the previous member. No-op at the very
      //   first member. Shift-extend across regions deferred.
      if (!shift && caret_at_region_start()) {
        if (caret_in_overflow()) {
          if (cross_out_of_overflow()) return true;
        } else if (cross_member_region(-1)) {
          return true;
        }
      }
      // s305 m4 — Shift+Left extends; plain Left collapses (m2
      //   behaviour preserved by calling the base move which
      //   already collapses).
      // s307 m3 — Plain Left flushes the in-flight typing (motion
      //   leaves the current run). Shift+Left extends selection
      //   without flushing — still inside the current work context.
      if (shift) {
        extend_with([this]() { m_text_cursor->move_left(); });
      } else {
        flush_text_segment();
        m_text_cursor->move_left();
      }
      queue_draw();
      return true;

    case GDK_KEY_Right:
      // s319 — Caret region flow. At the current member's end byte, plain
      //   Right re-anchors to the NEXT member; at the TAIL member's end it
      //   steps into the overflow region (presents the OA, caret in). When
      //   already in the OA, neither fires and we fall through (scroll is a
      //   follow-on). Shift-extend across regions is deferred.
      if (!shift && caret_at_region_end()) {
        if (cross_member_region(+1)) return true;
        if (!caret_in_overflow() && cross_into_overflow(m_text_editing))
          return true;
      }
      // s312 m2.3 — Cross-boundary at canvas-right-edge. If the caret
      //   is at compute_text_layout's bytes_consumed AND overflow
      //   exists, Right means "step into the popover" rather than
      //   "advance one codepoint past the canvas-rendered region."
      //   Shift+Right does NOT cross — selection-extend across the
      //   boundary would require cross-surface selection mirroring
      //   we haven't designed; fall through to stock move_right
      //   which advances byte_index but renders no visible change
      //   beyond the canvas-end (a small honest cosmetic glitch
      //   pending selection-mirror work).
      if (!shift) {
        SceneNode* xb_mgr = nullptr;
        if (caret_at_canvas_end(&xb_mgr) && xb_mgr) {
          flush_text_segment();  // motion leaves the typing run
          LOG_INFO("Canvas: cross-into-depot from Right at canvas "
                   "end — Mgr '{}'", xb_mgr->name);
          cross_into_depot(xb_mgr);
          return true;
        }
      }
      if (shift) {
        extend_with([this]() { m_text_cursor->move_right(); });
      } else {
        flush_text_segment();  // s307 m3
        m_text_cursor->move_right();
      }
      queue_draw();
      return true;

    case GDK_KEY_Home:
      if (shift) {
        extend_with([this]() { m_text_cursor->move_line_start(); });
      } else {
        flush_text_segment();  // s307 m3
        m_text_cursor->move_line_start();
      }
      queue_draw();
      return true;

    case GDK_KEY_End:
      if (shift) {
        extend_with([this]() { m_text_cursor->move_line_end(); });
      } else {
        flush_text_segment();  // s307 m3
        m_text_cursor->move_line_end();
      }
      queue_draw();
      return true;

    case GDK_KEY_Escape:
      // Let the global Esc cascade run cancel_text_edit. Do NOT consume.
      return false;

    case GDK_KEY_Up:
      // s306 m6 — Vertical caret nav. move_up returns true when the
      //   caret actually moved (so queue_draw is only gated on a real
      //   visual change). Shift+Up extends the selection via the same
      //   snapshot-and-restore pattern as the other arrow keys: the
      //   base move_up collapses anchor=caret, extend_with restores
      //   the press-point anchor afterward. preferred_x is preserved
      //   across the chain so a column-march through short
      //   intermediate lines doesn't collapse.
      // s307 m3 — Plain Up flushes. Note: a chain of Up presses
      //   each flushes — but after the first, the in-flight segment
      //   is empty (no typing happened between Up presses), so each
      //   subsequent flush is a zero-delta no-op. preferred_x lives
      //   on TextCursor across the chain regardless of flush; the
      //   column-march behaviour from m6 is preserved.
      if (shift) {
        extend_with([this]() { m_text_cursor->move_up(); });
      } else {
        flush_text_segment();
        // s319 — Top line: step out of the OA back to the tail member
        //   (closes the OA), or to the previous member. No-op at the first.
        if (!m_text_cursor->move_up()) {
          if (caret_in_overflow())
            cross_out_of_overflow();
          else
            cross_member_region(-1);
        }
      }
      queue_draw();
      return true;

    case GDK_KEY_Down:
      // s312 m2.3 — Cross-boundary at canvas-last-baseline. move_down
      //   returns false when there's no canvas baseline below; if the
      //   Mgr has overflow at that point, the next "line down" is the
      //   popover's first line. Cross. Same shift policy as Right:
      //   plain Down crosses, Shift+Down extends within the canvas
      //   only (selection-mirror is a follow-on).
      //
      //   The cross condition doesn't depend on the cursor's column —
      //   pressing Down anywhere on the last canvas baseline crosses
      //   to the popover. The TextView's caret lands at offset 0
      //   (top-left of depot), collapsing the preferred-x column;
      //   true preferred-x continuity across the boundary needs more
      //   plumbing and is a separate follow-on.
      if (shift) {
        extend_with([this]() { m_text_cursor->move_down(); });
        queue_draw();
        return true;
      }
      flush_text_segment();  // s307 m3
      if (!m_text_cursor->move_down()) {
        // s319 — Bottom line: next member first; at the tail, step into the
        //   overflow region (presents the OA, caret in). Skip when already
        //   in the OA (scroll is a follow-on).
        if (!cross_member_region(+1)) {
          if (!caret_in_overflow())
            cross_into_overflow(m_text_editing);
        }
      }
      queue_draw();
      return true;

    default: {
      // Printable character? gdk_keyval_to_unicode returns 0 for
      // non-printable keys (function keys, modifiers alone, etc.).
      guint32 uc = gdk_keyval_to_unicode(keyval);
      if (uc != 0 && uc >= 0x20 && uc != 0x7f) {
        // s305 m4 — Typing-with-selection replaces. delete_selection
        //   runs first (collapses anchor=caret at range start);
        //   insert_char then runs at the collapsed caret.
        // s307 m2 — Selection-replacement is one user gesture
        //   (single keystroke that conceptually means "replace this
        //   range with this character"). Flush in-flight typing
        //   before, do the delete + insert as a fused mutation, then
        //   flush the replacement as its own segment. The new
        //   character does NOT start a fresh typing run from the
        //   user's perspective — it IS the replacement payload.
        //   Subsequent typed characters extend the freshly-opened
        //   segment via the no-selection branch below.
        if (m_text_cursor->has_selection()) {
          flush_text_segment();
          m_text_cursor->delete_selection();
          if (m_text_cursor->insert_char((gunichar)uc)) {
            flush_text_segment();
            m_sig_doc_changed.emit();
            queue_draw();
          }
        } else if (m_text_cursor->insert_char((gunichar)uc)) {
          // s307 m4 — Word-boundary flush. After inserting a printable
          //   character, if that character is whitespace or punctuation,
          //   flush the segment. The boundary char itself belongs to the
          //   run that preceded it: Ctrl+Z rewinds to before the word
          //   AND its trailing space/punct in one step, which is what
          //   every text editor does. g_unichar_isspace and
          //   g_unichar_ispunct give Unicode-aware definitions — works
          //   for foreign-script content as well as ASCII. Non-boundary
          //   chars (letters, digits) build the in-flight segment
          //   without flushing; word-by-word granularity falls out of
          //   the natural rhythm of typing.
          if (g_unichar_isspace((gunichar)uc) ||
              g_unichar_ispunct((gunichar)uc)) {
            flush_text_segment();
          } else {
            // s307 m5 — Non-boundary typing arms/re-arms the pause
            //   timer. Word-boundary chars don't need to arm because
            //   they just flushed; the post-flush segment will arm
            //   on the next non-boundary char typed into it.
            restart_text_typing_pause_timer();
          }
          m_sig_doc_changed.emit();
          queue_draw();
        }
        return true;
      }
      return false;
    }
  }
}

void Canvas::scrub_node_refs(const SceneNode *target) {
  if (!target)
    return;

  // Multi-node selection (Node tool) — renderer iterates this every paint
  // and dereferences ns.obj->path; a stale entry pointing at freed memory
  // is the canonical std::bad_array_new_length cliff.
  m_node_selection.erase(
      std::remove_if(m_node_selection.begin(), m_node_selection.end(),
                     [target](const NodeSel &ns) { return ns.obj == target; }),
      m_node_selection.end());

  // Multi-object selection (Selection tool) — iterated by various overlays
  // and inspector consumers.
  m_selection.erase(
      std::remove(m_selection.begin(), m_selection.end(), target),
      m_selection.end());

  // Tool-mode scratch members. align_anchor has its own validator-on-read
  // so this is belt-and-braces; the others are raw reads.
  if (m_align_anchor == target)
    m_align_anchor = nullptr;
  if (m_nudge_last_obj == target)
    m_nudge_last_obj = nullptr;
  if (m_snap_target_obj == target) {
    m_snap_target_obj = nullptr;
    m_snap_target_end = -1;
  }
  if (m_selected2 == target) {
    m_selected2 = nullptr;
    m_selected_node2 = -1;
  }
  // m_continue_target — pen-tool "continue path" stash. Set by node-mode
  // double-click on an open path, consumed when the user resumes pen tool.
  // Could persist across an unrelated destructive op in node mode.
  if (m_continue_target == target)
    m_continue_target = nullptr;
  // m_top_text / m_top_path_node — text-on-path cached pointers.
  if (m_top_text == target)
    m_top_text = nullptr;
  if (m_top_path_node == target)
    m_top_path_node = nullptr;
  // Eyedropper / ruler / ref / text-edit hover state — short-lived but
  // could survive a mutation triggered by a hotkey while the pointer is
  // hovering. Cheap to scrub.
  if (m_eyedropper_hovered == target)
    m_eyedropper_hovered = nullptr;
  if (m_ruler_node_a_obj == target)
    m_ruler_node_a_obj = nullptr;
  if (m_ruler_node_b_obj == target)
    m_ruler_node_b_obj = nullptr;
  if (m_ref_hovered == target)
    m_ref_hovered = nullptr;
  if (m_ref_selected == target)
    m_ref_selected = nullptr;
  if (m_text_editing == target)
    m_text_editing = nullptr;
  // Guide hover state — guides are SceneNodes too.
  if (m_guide_drag_node == target)
    m_guide_drag_node = nullptr;
  if (m_guide_hovered == target)
    m_guide_hovered = nullptr;
  if (m_warp_env_picks_owner == target)
    m_warp_env_picks_owner = nullptr;

  // s158 m1 — let SelectionContext drop any cached pointer to target.
  // Belt-and-braces; the next recompute_object will rebuild Info from
  // scratch, but defence in depth is cheap.
  m_sel_ctx.scrub_node_ref(target);

  // s165 m3 — also scrub the undo/redo stacks. Commands that captured raw
  // pointers to `target` will dereference dangling memory at undo() time
  // and crash; dropping them here keeps the history consistent with the
  // live tree. Cost: a single linear walk of both stacks per destructive
  // op. The default base CurvzCommand::references() returns false so
  // commands without raw-pointer captures aren't affected.
  if (m_history)
    m_history->scrub_command_history(target);

  // s298 m2 (A1) — clear any dangling text-on-path back-references in
  // the live document tree. See scrub_text_path_refs above for descent
  // rules and rationale. Walks every layer subtree; cost is O(n) on
  // document node count per destructive op, which matches the existing
  // index-invalidate-then-rebuild cost shape on the next find_by_iid.
  if (m_doc && !target->internal_id.empty()) {
    for (const auto &l : m_doc->layers)
      scrub_text_path_refs(l.get(), target->internal_id);
  }

  // s167 m1 — invalidate the iid → SceneNode* index on the active
  // document. scrub_node_refs is the canonical "node about to be
  // destroyed" seam (s156); marking the index dirty here ensures the
  // next find_by_iid rebuilds the map, so iid-based commands never
  // resolve to a stale pointer. Cheap (single bool flip); idempotent
  // across multiple scrubs in the same destructive op. Migrating
  // commands to id-based captures (s167 stages 0-6) progressively
  // retires the scrub_command_history call above; the invalidate
  // call here is the structural replacement.
  if (m_doc)
    m_doc->invalidate_iid_index();
}

// ── Canvas::notify_object_selection_changed ──────────────────────────────────
// s158 m1 — single seam for object-selection-changed fanout. Refreshes the
// SelectionContext from current m_selection + m_selected, then emits the
// existing m_sig_selection signal. Call in place of bare
// m_sig_selection.emit() at object-selection mutation sites.
//
// s160 m2 — full migration complete. Every object-selection mutation site
// across Canvas.cpp now routes through this helper, with the exception of
// two E-class panel-refresh hijacks (node marquee start, measurements
// changed) that intentionally remain bare emits — search for the
// "s159 m2: kept as bare emit" comment markers. Those are deferred until
// the s155-backlog inspector refactor retires the panel-refresh hijack
// pattern.
//
// Invariant for callers: m_selection and m_selected must be consistent
// before calling — either both point at the same primary, or m_selection
// is empty and m_selected is nullptr. The helper does not enforce or
// repair the invariant; SelectionContext::build_object_info walks
// m_selection, so a stale m_selection silently corrupts info-flags.
// (~9 latent inconsistencies were caught and fixed during the s159/s160
// migration; see "s160 m2:" / "s159 m2:" comment markers.)
void Canvas::notify_object_selection_changed() {
  LOG_INFO("csb::HOLD Canvas::notify_object_selection_changed CALLED sel={} sel_size={}",
           (void*)m_selected, (int)m_selection.size());
  m_sel_ctx.recompute_object(m_selection, m_selected);
  m_sig_selection.emit(m_selected);
}

// ── Canvas::notify_node_selection_changed ────────────────────────────────────
// s158 m1 — node-side counterpart. Builds the parallel arrays
// SelectionContext expects from m_node_selection's NodeSel records, then
// hands off to recompute_node along with the primary/secondary node slots.
//
// Does NOT emit a "node selection changed" signal — Canvas doesn't have
// one today; node-side consumers refresh from m_sig_selection. If a
// dedicated node signal lands later, this is the seam to wire it on.
void Canvas::notify_node_selection_changed() {
  std::vector<SceneNode *> paths;
  std::vector<int>         indices;
  paths.reserve(m_node_selection.size());
  indices.reserve(m_node_selection.size());
  for (const NodeSel &ns : m_node_selection) {
    paths.push_back(ns.obj);
    indices.push_back(ns.node_idx);
  }
  m_sel_ctx.recompute_node(paths, indices,
                           m_selected, m_selected_node,
                           m_selected2, m_selected_node2);
}

// ── Canvas::set_selection_single ─────────────────────────────────────────────
// s162 m3 — moved out-of-line so it can fold in
// notify_object_selection_changed(). Previously a header inline that callers
// were expected to follow up with a manual notify; the s159 audit caught and
// fixed every Canvas.cpp `m_sig_selection.emit()` site, but missed external
// callers (notably MainWindow::on_warp_make at line 6648) that mutated the
// canonical selection through this accessor without driving the SelectionContext
// recompute. The bug surfaced as a stale right-click context menu after Make
// Warp — the menu showed Compound verbs because m_sel_ctx still reflected the
// pre-warp selection. Folding the notify here is the structural fix per the
// project memory rule "structural fix makes the problem class go away":
// every present and future caller becomes correct by construction. Pivot
// reset preserved verbatim.
void Canvas::set_selection_single(SceneNode *node) {
  m_selection.clear();
  m_selected = node;
  if (node)
    m_selection.push_back(node);
  // New selection → reset custom pivot
  m_has_custom_pivot = false;
  m_pivot_dragging = false;

  notify_object_selection_changed();
}

// ── Canvas::perform_pen_path (s288 m2, renamed s291 m2) ─────────────────
// Thin forwarder to the SvgPerformer member. Resolves active doc and
// active layer from m_doc (the performer can't see m_doc directly — it's
// private — but Canvas knows how to find both). The performer does
// everything else: parses the d-string, builds the beat list, runs the
// timeout, commits at end. No SvgEmitter machinery touched — there's
// no SVG file in scope.
void Canvas::perform_pen_path(const std::string& d_string,
                              double speed) {
  if (!m_doc) return;
  SceneNode* layer = m_doc->active_layer();
  if (!layer && !m_doc->layers.empty()) {
    layer = m_doc->layers[0].get();
  }
  if (!layer) return;
  m_svg_performer.enact_pen_path(d_string, speed, m_doc, layer);
}

// ── Canvas::perform_svg_file (s288 m3, renamed s291 m2) ─────────────────
// Sibling thin forwarder for the SVG-file entry point. Resolves active
// doc + layer from m_doc; the performer constructs an AnimatingSvgParser
// with itself as SvgEmitter, runs the parse (events populate the queue),
// runs the y-flip + bbox-fit pass, then plays the queue back-to-back.
void Canvas::perform_svg_file(const std::string& svg_path,
                              double speed) {
  if (!m_doc) return;
  SceneNode* layer = m_doc->active_layer();
  if (!layer && !m_doc->layers.empty()) {
    layer = m_doc->layers[0].get();
  }
  if (!layer) return;
  m_svg_performer.perform(svg_path, speed, m_doc, layer);
}

// ── Canvas::abort_svg_performance (s294 m5c) ───────────────────────────
// Thin forwarder. Both callers (Esc-key handler in MainWindow's capture
// controller, and the app.stop_animation Scriptable verb) route through
// here so SvgPerformer stays the single source of truth for what abort
// means.
void Canvas::abort_svg_performance() {
  m_svg_performer.abort();
}

bool Canvas::is_svg_playing() const {
  return m_svg_performer.is_playing();
}

// s161 split: path_anchor_bbox moved here from Canvas_ops.cpp.
//   Original Canvas.cpp routed it to OPS, but its sole caller
//   subtree_path_bbox is in CORE. Keep call site in-TU.
// Axis-aligned bbox of a PathData (anchors only — sufficient for
// normalization purposes; handles that poke outside the anchor bbox
// just extrapolate slightly, which is harmless). Returns false for
// empty paths. Matches canonical Y-down doc space.
static bool path_anchor_bbox(const PathData &pd, double &bx, double &by,
                             double &bw, double &bh) {
  if (pd.nodes.empty())
    return false;
  double minx = pd.nodes[0].x, maxx = pd.nodes[0].x;
  double miny = pd.nodes[0].y, maxy = pd.nodes[0].y;
  for (const auto &n : pd.nodes) {
    minx = std::min(minx, n.x);
    maxx = std::max(maxx, n.x);
    miny = std::min(miny, n.y);
    maxy = std::max(maxy, n.y);
  }
  bx = minx;
  by = miny;
  bw = maxx - minx;
  bh = maxy - miny;
  // Guard against degenerate zero-width or zero-height paths
  if (bw < 1e-9)
    bw = 1e-9;
  if (bh < 1e-9)
    bh = 1e-9;
  return true;
}

// Bbox across the union of leaf paths in a subtree — walks into Group /
// Compound children to find the overall extent of the source payload.
// Used to compute the (u,v) normalization frame for the whole Warp.
bool subtree_path_bbox(const SceneNode *n, double &bx, double &by,
                              double &bw, double &bh) {
  if (!n)
    return false;
  bool found = false;
  double minx = 0, miny = 0, maxx = 0, maxy = 0;
  std::function<void(const SceneNode *)> walk = [&](const SceneNode *nd) {
    if (!nd)
      return;
    if (nd->type == SceneNode::Type::Path && nd->path &&
        !nd->path->nodes.empty()) {
      double lx, ly, lw, lh;
      if (!path_anchor_bbox(*nd->path, lx, ly, lw, lh))
        return;
      if (!found) {
        minx = lx;
        miny = ly;
        maxx = lx + lw;
        maxy = ly + lh;
        found = true;
      } else {
        minx = std::min(minx, lx);
        miny = std::min(miny, ly);
        maxx = std::max(maxx, lx + lw);
        maxy = std::max(maxy, ly + lh);
      }
      return;
    }
    // Recurse into containers
    for (const auto &c : nd->children)
      walk(c.get());
  };
  walk(n);
  if (!found)
    return false;
  bx = minx;
  by = miny;
  bw = std::max(maxx - minx, 1e-9);
  bh = std::max(maxy - miny, 1e-9);
  return true;
}

// ── Canvas::warp_source_bbox ─────────────────────────────────────────────────
// Public canonical answer to "what rectangle does this Warp's envelope
// describe a remapping over?" Single source of truth, shared by
// rebuild_warp_caches' internal use of subtree_path_bbox(glyph_cache)
// and any caller (notably the inspector's preset regen) that needs to
// produce envelope nodes in the same coordinate frame warp_subtree
// will interpret them against.
//
// Why not object_bbox_query? Because object_bbox can include recipe
// metadata, stroke padding, transform-aware bounds — none of which
// warp_subtree sees. The math layer walks the actual path nodes via
// subtree_path_bbox; we expose the same path-node bbox here so the
// inspector's envelope-generation rectangle matches the warp
// renderer's interpretation rectangle exactly.
//
// Side-effect: brings warp_glyph_cache current if dirty/null. The
// glyph_cache is what subtree_path_bbox walks, and it's a clone of
// warp_source — measuring source directly via subtree_path_bbox would
// produce the same answer for typical content, but going through the
// cache mirrors exactly what rebuild_warp_caches does, eliminating
// any divergence under future caching changes.
bool Canvas::warp_source_bbox(SceneNode &w, double &bx, double &by,
                              double &bw, double &bh) {
  if (!w.is_warp() || !w.warp_source)
    return false;
  // Ensure glyph_cache is current. rebuild_warp_caches will phase-1
  // clone source into glyph_cache when dirty, then proceed; we only
  // need the glyph_cache to exist for the bbox call below. Letting
  // it run all phases is safe and keeps caches coherent for the next
  // draw — cheaper than duplicating the phase-1 logic here.
  if (w.warp_glyph_cache_dirty || !w.warp_glyph_cache) {
    rebuild_warp_caches(&w);
  }
  if (!w.warp_glyph_cache)
    return false;
  return subtree_path_bbox(w.warp_glyph_cache.get(), bx, by, bw, bh);
}

// ── Canvas::hit_test_warp_envelope ───────────────────────────────────────────
// Screen-space hit test against the envelope anchors + handles of the
// given Warp. Hit threshold matches the overlay's visual sizes: 5 px
// for anchors (8px filled square), 5 px for handle dots (6px ring with
// some tolerance).
//
// Handles are checked FIRST so that in degenerate cases where a handle
// overlaps its anchor (straight envelope with colinear handle at
// anchor distance zero) the drag still works — though in practice the
// overlay skips drawing such handles, so they shouldn't be clickable.
// We still skip hit-testing them since the user can't see them to aim.
//
// Returns true on hit; kind/is_top/idx describe what was picked.
bool Canvas::hit_test_warp_envelope(double x_screen, double y_screen,
                                    const SceneNode &warp, WarpDragKind &kind,
                                    bool &is_top, int &idx) const {
  if (warp.type != SceneNode::Type::Warp)
    return false;
  const double hit_r = 6.0; // screen-space hit radius
  const double hit_r2 = hit_r * hit_r;

  auto scan_envelope = [&](const PathData &env, bool top_flag) -> bool {
    // Pass 1: handles first (preferred over anchors on overlap).
    for (int i = 0; i < (int)env.nodes.size(); ++i) {
      const BezierNode &n = env.nodes[i];
      double asx, asy;
      doc_to_screen(n.x, n.y, asx, asy);
      double h1sx, h1sy, h2sx, h2sy;
      doc_to_screen(n.cx1, n.cy1, h1sx, h1sy);
      doc_to_screen(n.cx2, n.cy2, h2sx, h2sy);
      // Skip handles coincident with their anchor — overlay doesn't
      // draw them, so they can't be aimed at.
      if (std::hypot(h1sx - asx, h1sy - asy) > 0.5) {
        double dx = x_screen - h1sx, dy = y_screen - h1sy;
        if (dx * dx + dy * dy <= hit_r2) {
          kind = WarpDragKind::HandleIn;
          is_top = top_flag;
          idx = i;
          return true;
        }
      }
      if (std::hypot(h2sx - asx, h2sy - asy) > 0.5) {
        double dx = x_screen - h2sx, dy = y_screen - h2sy;
        if (dx * dx + dy * dy <= hit_r2) {
          kind = WarpDragKind::HandleOut;
          is_top = top_flag;
          idx = i;
          return true;
        }
      }
    }
    // Pass 2: anchors.
    for (int i = 0; i < (int)env.nodes.size(); ++i) {
      const BezierNode &n = env.nodes[i];
      double asx, asy;
      doc_to_screen(n.x, n.y, asx, asy);
      double dx = x_screen - asx, dy = y_screen - asy;
      if (dx * dx + dy * dy <= hit_r2) {
        kind = WarpDragKind::Anchor;
        is_top = top_flag;
        idx = i;
        return true;
      }
    }
    return false;
  };

  if (scan_envelope(warp.warp_env_top, true))
    return true;
  if (scan_envelope(warp.warp_env_bottom, false))
    return true;
  return false;
}

void Canvas::clamp_pan() {
  if (!m_doc)
    return;
  const double cw = m_doc->canvas_width() * m_zoom;
  const double ch = m_doc->canvas_height() * m_zoom;
  // Allow panning until only a small margin strip of the artboard remains
  // visible. margin = max(40px, 10% of canvas screen size) — scales with zoom
  // so you can't lose the artboard entirely at any zoom level.
  const double margin_x = std::max(40.0, cw * 0.10);
  const double margin_y = std::max(40.0, ch * 0.10);
  const double cx = (get_width() - cw) * 0.5;
  const double cy = (get_height() - ch) * 0.5;
  m_pan_x =
      std::clamp(m_pan_x, -cx - cw + margin_x, -cx + get_width() - margin_x);
  m_pan_y =
      std::clamp(m_pan_y, -cy - ch + margin_y, -cy + get_height() - margin_y);
}

// ── Bounding box ─────────────────────────────────────────────────────────────
std::optional<Canvas::BBox> Canvas::object_bbox(const SceneNode &obj,
                                                bool include_stroke) const {
  // Ref points have no geometric bbox
  if (obj.type == SceneNode::Type::Ref)
    return std::nullopt;

  // ── Image: axis-aligned bbox from stored position/size ────────────────
  if (obj.type == SceneNode::Type::Image) {
    if (obj.image_w <= 0 || obj.image_h <= 0)
      return std::nullopt;
    return BBox{obj.image_x, obj.image_y, obj.image_w, obj.image_h};
  }

  // ── Group / Compound: union of all children's bboxes ─────────────────
  if (obj.type == SceneNode::Type::Group ||
      obj.type == SceneNode::Type::Compound) {
    std::optional<BBox> result;
    for (const auto &child : obj.children) {
      auto bb = object_bbox(*child, include_stroke);
      if (!bb)
        continue;
      if (!result) {
        result = bb;
        continue;
      }
      double x1 = std::min(result->x, bb->x);
      double y1 = std::min(result->y, bb->y);
      double x2 = std::max(result->x + result->w, bb->x + bb->w);
      double y2 = std::max(result->y + result->h, bb->y + bb->h);
      result = BBox{x1, y1, x2 - x1, y2 - y1};
    }
    return result;
  }

  // ── TextBox: bbox is the boundary's extent. The boundary
  //   (children[1]) defines the user-facing frame; selecting the
  //   TextBox draws handles around that frame. The text child
  //   (children[0])'s glyph extent isn't included because the
  //   text-as-rendered always sits inside the boundary's interior,
  //   and the user thinks of the textbox's selectable rectangle as
  //   the frame, not the glyph cluster.
  //
  //   Falls back gracefully if the container is malformed: missing
  //   boundary child → std::nullopt, same shape as Image with zero
  //   dimensions.
  // s317 — TextBoxMgr bbox = UNION of all canvas-view boundary bboxes.
  //   Pre-s317 (m1bc) this returned the FIRST canvas view's bbox and
  //   stopped — fine when a Mgr had one member, but once a Mgr owns
  //   several member boxes (link), that left the Mgr's bbox covering only
  //   box 1. hit_test gates the Mgr on this bbox, so a click in box 2 fell
  //   outside it and returned null → double-click couldn't enter edit on
  //   any box but the first. Unioning every member view makes the whole
  //   Mgr grabbable. (The overflow panel is chrome, not a member box, so
  //   it's intentionally excluded here — the container frame in the
  //   renderer handles enclosing the panel separately.) Malformed Mgr
  //   (no canvas view with a Path child) → std::nullopt as before.
  if (obj.is_text_box_mgr()) {
    bool found = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (const auto &view : obj.children) {
      if (!view || !view->is_canvas_view()) continue;
      if (view->children.empty() || !view->children[0]) continue;
      auto vb = object_bbox(*view->children[0], include_stroke);
      if (!vb) continue;
      if (!found) {
        x0 = vb->x; y0 = vb->y; x1 = vb->x + vb->w; y1 = vb->y + vb->h;
        found = true;
      } else {
        x0 = std::min(x0, vb->x);
        y0 = std::min(y0, vb->y);
        x1 = std::max(x1, vb->x + vb->w);
        y1 = std::max(y1, vb->y + vb->h);
      }
    }
    if (found) return BBox{x0, y0, x1 - x0, y1 - y0};
    return std::nullopt;
  }

  // ── ClipGroup: bbox is the clip shape's own physical extent.
  //   The clip shape defines the region; clipped children that lie
  //   outside it aren't visible, so the ClipGroup's selection bbox
  //   shouldn't shrink around their visible portions. If there is no
  //   clip shape yet (degenerate) fall back to the children union.
  if (obj.type == SceneNode::Type::ClipGroup) {
    if (obj.clip_shape) {
      auto clip_bb = object_bbox(*obj.clip_shape, include_stroke);
      if (clip_bb)
        return clip_bb;
    }
    std::optional<BBox> kids;
    for (const auto &child : obj.children) {
      auto bb = object_bbox(*child, include_stroke);
      if (!bb)
        continue;
      if (!kids) {
        kids = bb;
        continue;
      }
      double x1 = std::min(kids->x, bb->x);
      double y1 = std::min(kids->y, bb->y);
      double x2 = std::max(kids->x + kids->w, bb->x + bb->w);
      double y2 = std::max(kids->y + kids->h, bb->y + bb->h);
      kids = BBox{x1, y1, x2 - x1, y2 - y1};
    }
    return kids;
  }

  // ── Blend: union of A, B, and cache bboxes. Because every cache
  //   entry's geometry lies strictly between A and B (linear lerp of
  //   anchors/handles), the A∪B bbox actually bounds everything in v1.
  //   We still union the cache for symmetry and forward-compat (future
  //   non-linear spines, easing curves, etc).
  if (obj.type == SceneNode::Type::Blend) {
    std::optional<BBox> result;
    auto accumulate = [&](const SceneNode *n) {
      if (!n)
        return;
      auto bb = object_bbox(*n, include_stroke);
      if (!bb)
        return;
      if (!result) {
        result = bb;
        return;
      }
      double x1 = std::min(result->x, bb->x);
      double y1 = std::min(result->y, bb->y);
      double x2 = std::max(result->x + result->w, bb->x + bb->w);
      double y2 = std::max(result->y + result->h, bb->y + bb->h);
      result = BBox{x1, y1, x2 - x1, y2 - y1};
    };
    accumulate(obj.blend_source_a.get());
    accumulate(obj.blend_source_b.get());
    for (const auto &step : obj.blend_cache)
      accumulate(step.get());
    return result;
  }

  // ── Warp: bbox is the union of source + both caches. In the steady
  //   state warp_cache is what's actually visible, and its bbox bounds
  //   the rendered warped geometry. Including warp_source keeps the
  //   bbox sensible in transient states where the caches are empty
  //   (just-constructed Warp before first rebuild) or being rebuilt.
  //   warp_glyph_cache is included symmetrically — for path sources
  //   its bbox equals the source's, but this future-proofs when text
  //   sources make glyph_cache diverge from source.
  if (obj.type == SceneNode::Type::Warp) {
    std::optional<BBox> result;
    auto accumulate = [&](const SceneNode *n) {
      if (!n)
        return;
      auto bb = object_bbox(*n, include_stroke);
      if (!bb)
        return;
      if (!result) {
        result = bb;
        return;
      }
      double x1 = std::min(result->x, bb->x);
      double y1 = std::min(result->y, bb->y);
      double x2 = std::max(result->x + result->w, bb->x + bb->w);
      double y2 = std::max(result->y + result->h, bb->y + bb->h);
      result = BBox{x1, y1, x2 - x1, y2 - y1};
    };
    accumulate(obj.warp_source.get());
    accumulate(obj.warp_glyph_cache.get());
    accumulate(obj.warp_cache.get());
    return result;
  }

  // ── Text: measure actual layout extents via Pango ──────────────────────
  if (obj.type == SceneNode::Type::Text) {
    // Linked text-on-path: use the guide path's bbox so the selection box
    // reflects where the text actually renders, not the creation point.
    if (!obj.text_path_id.empty()) {
      SceneNode *guide = top_find_path_by_id(obj.text_path_id);
      if (guide)
        return object_bbox(*guide, include_stroke);
    }
    // Use a scratch Cairo surface — we only need the layout metrics.
    auto surf =
        Cairo::ImageSurface::create(Cairo::ImageSurface::Format::ARGB32, 1, 1);
    auto cr = Cairo::Context::create(surf);

    PangoLayout *layout = pango_cairo_create_layout(cr->cobj());
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, obj.text_font_family.c_str());
    pango_font_description_set_absolute_size(desc,
                                             obj.text_font_size * PANGO_SCALE);
    if (obj.text_bold)
      pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    if (obj.text_italic)
      pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, obj.text_content.c_str(), -1);

    // Match draw_text_node: apply letter spacing so width is accurate.
    if (obj.text_letter_spacing != 0.0) {
      PangoAttrList *attrs = pango_attr_list_new();
      pango_attr_list_insert(attrs,
                             pango_attr_letter_spacing_new(
                                 (int)(obj.text_letter_spacing * PANGO_SCALE)));
      pango_layout_set_attributes(layout, attrs);
      pango_attr_list_unref(attrs);
    }

    PangoRectangle ink, logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);

    int baseline = pango_layout_get_baseline(layout);
    double base_px = baseline / (double)PANGO_SCALE;

    g_object_unref(layout);

    // Use ink extents for tightest bbox (logical includes descender space).
    double w = ink.width > 0 ? ink.width : logical.width;
    double h = ink.height > 0 ? ink.height : logical.height;

    // Anchor offset — same logic as draw_text_node.
    double off_x = 0.0;
    if (obj.text_anchor == "middle")
      off_x = -logical.width * 0.5;
    if (obj.text_anchor == "end")
      off_x = -logical.width;

    // ink.x/y are offsets from the layout origin (which is top-left).
    // text_y is the baseline in doc-space Y-down.
    // Baseline shift offsets the whole block upward in doc space (Y-down =
    // subtract).
    double bx = obj.text_x + off_x + ink.x;
    double by = obj.text_y - base_px + ink.y - obj.text_baseline_shift;

    double outset = include_stroke ? obj.stroke.width * 0.5 : 0.0;
    return BBox{bx - outset, by - outset, w + outset * 2, h + outset * 2};
  }

  if (obj.type == SceneNode::Type::Path && obj.path &&
      !obj.path->nodes.empty()) {
    const auto &nodes = obj.path->nodes;
    double minx = nodes[0].x, maxx = minx;
    double miny = nodes[0].y, maxy = miny;

    // Compute tight curve bounds by sampling each cubic segment
    // Segments: node[i] → node[i+1], using out-handle of i and in-handle of i+1
    auto expand_cubic = [&](double p0x, double p0y, double p1x, double p1y,
                            double p2x, double p2y, double p3x, double p3y) {
      // Sample 32 points along the cubic — sufficient for icon/glyph scale
      for (int s = 0; s <= 32; ++s) {
        double t = s / 32.0;
        double mt = 1.0 - t;
        double x = mt * mt * mt * p0x + 3 * mt * mt * t * p1x +
                   3 * mt * t * t * p2x + t * t * t * p3x;
        double y = mt * mt * mt * p0y + 3 * mt * mt * t * p1y +
                   3 * mt * t * t * p2y + t * t * t * p3y;
        minx = std::min(minx, x);
        maxx = std::max(maxx, x);
        miny = std::min(miny, y);
        maxy = std::max(maxy, y);
      }
    };

    int n = (int)nodes.size();
    for (int i = 0; i < n - 1; ++i) {
      expand_cubic(nodes[i].x, nodes[i].y, nodes[i].cx2, nodes[i].cy2,
                   nodes[i + 1].cx1, nodes[i + 1].cy1, nodes[i + 1].x,
                   nodes[i + 1].y);
    }
    // Closing segment if path is closed
    if (obj.path->closed && n > 1) {
      expand_cubic(nodes[n - 1].x, nodes[n - 1].y, nodes[n - 1].cx2,
                   nodes[n - 1].cy2, nodes[0].cx1, nodes[0].cy1, nodes[0].x,
                   nodes[0].y);
    }

    // ── Stroke bbox expansion ─────────────────────────────────────────
    //
    // The stroke contributes width/2 perpendicular to the path along
    // every segment — that's covered by uniform AABB outset and is
    // correct for closed paths and for round/square caps on open paths.
    //
    // Cap type only affects the LONGITUDINAL extension at OPEN-PATH
    // ENDPOINTS. Round and Square caps extend width/2 past the endpoint
    // along the tangent. Butt does NOT — it ends flat at the endpoint.
    //
    // The pre-S104 code padded uniformly by width/2 on all four sides
    // regardless of cap, which was wrong for Butt: drawing a horizontal
    // or vertical line with butt cap would show a bbox extending past
    // the endpoints by width/2 in the tangent direction, when the
    // actual ink stops at the endpoint.
    //
    // Fix: pad uniformly first (covers perpendicular bulge + joins +
    // round/square caps). Then, for open paths with Butt cap, carve
    // back the bbox by `width/2 × |tangent_axis_component|` at each
    // endpoint, but ONLY on the side(s) where the endpoint is the
    // current AABB extreme. The "owner" check is critical: if some
    // interior cubic bulge is what drove maxy, we mustn't shrink maxy
    // just because the start endpoint's tangent points down.
    double outset = include_stroke ? obj.stroke.width * 0.5 : 0.0;
    double bx = minx - outset;
    double by = miny - outset;
    double bw = (maxx - minx) + outset * 2;
    double bh = (maxy - miny) + outset * 2;

    if (include_stroke && outset > 0.0 && !obj.path->closed && n >= 2 &&
        obj.stroke.cap == LineCap::Butt) {
      // Compute outward tangents at the two open endpoints. Outward =
      // pointing away from the path interior.
      //
      // Start endpoint (node 0): outward = node[0] - out-handle of node[0].
      //   Fallback for degenerate handle: node[0] - node[1] (straight seg).
      // End endpoint   (node n-1): outward = node[n-1] - in-handle of n-1.
      //   Fallback: node[n-1] - node[n-2].
      auto outward_tangent = [](double nx, double ny,
                                double hx, double hy,
                                double anx, double any,
                                double& tx, double& ty) {
        double dx = nx - hx;
        double dy = ny - hy;
        double len2 = dx * dx + dy * dy;
        if (len2 < 1e-12) {
          dx = nx - anx;
          dy = ny - any;
          len2 = dx * dx + dy * dy;
          if (len2 < 1e-12) { tx = 0.0; ty = 0.0; return; }
        }
        double inv = 1.0 / std::sqrt(len2);
        tx = dx * inv;
        ty = dy * inv;
      };

      double s_tx = 0.0, s_ty = 0.0;
      double e_tx = 0.0, e_ty = 0.0;
      outward_tangent(nodes[0].x, nodes[0].y,
                      nodes[0].cx2, nodes[0].cy2,
                      nodes[1].x, nodes[1].y,
                      s_tx, s_ty);
      outward_tangent(nodes[n - 1].x, nodes[n - 1].y,
                      nodes[n - 1].cx1, nodes[n - 1].cy1,
                      nodes[n - 2].x, nodes[n - 2].y,
                      e_tx, e_ty);

      // For each endpoint, on each axis, if the endpoint's coordinate
      // is the current bbox extreme on the side its tangent points to,
      // carve back the over-pad by the correct amount.
      //
      // The correct cap-AABB contribution for a butt-capped endpoint
      // with outward tangent (tx, ty) is endpoint ± (h·|ty|, h·|tx|).
      // (The cap segment is perpendicular to tangent, length width;
      // its AABB-x is h × |perp_x| = h × |ty|, AABB-y is h × |tx|.)
      //
      // The uniform pad outset h on the side the endpoint owns was
      // therefore an over-pad by:
      //   x: h × (1 - |ty|)   when endpoint is the x-extreme
      //   y: h × (1 - |tx|)   when endpoint is the y-extreme
      //
      // Sanity: horizontal line (tx=±1, ty=0) → x carve = h, y carve = 0.
      // Bbox shrinks by h past each endpoint in x (correct — butt cap
      // is flush at endpoint), no carve in y (correct — perpendicular
      // is full h). Vertical line: symmetric. Diagonal: partial carve
      // both axes, matching the rotated cap's AABB exactly.
      //
      // Equality check uses a small epsilon: floating-point identity
      // between sampled cubic extreme and node coordinate isn't
      // guaranteed in the curved-segment case, but at the endpoint
      // (t=0 / t=1) the sample IS the node, so equality holds modulo
      // arithmetic.
      auto carve = [&](double ex, double ey, double tx, double ty) {
        const double eps = 1e-9;
        double abs_tx = std::fabs(tx);
        double abs_ty = std::fabs(ty);
        // X axis — carve amount is outset × (1 - |ty|).
        double x_carve = outset * (1.0 - abs_ty);
        if (tx > 0.0 && std::fabs(ex - maxx) < eps) {
          bw -= x_carve;
        } else if (tx < 0.0 && std::fabs(ex - minx) < eps) {
          bx += x_carve;
          bw -= x_carve;
        }
        // Y axis — carve amount is outset × (1 - |tx|).
        double y_carve = outset * (1.0 - abs_tx);
        if (ty > 0.0 && std::fabs(ey - maxy) < eps) {
          bh -= y_carve;
        } else if (ty < 0.0 && std::fabs(ey - miny) < eps) {
          by += y_carve;
          bh -= y_carve;
        }
      };

      carve(nodes[0].x, nodes[0].y, s_tx, s_ty);
      carve(nodes[n - 1].x, nodes[n - 1].y, e_tx, e_ty);
    }

    return BBox{bx, by, bw, bh};
  }
  return std::nullopt;
}

// ── Hit test ─────────────────────────────────────────────────────────────────
SceneNode *Canvas::hit_test(double doc_x, double doc_y) {
  if (!m_doc)
    return nullptr;

  // Recursive helper — tests a list of children, returns first hit
  // Returns the child itself, OR the group if hit is inside a group.
  //
  // s112 m4: when parent_is_compound is true, the children are subpaths
  // of a Compound. By rendering convention the Compound parent owns the
  // fill and child Paths have fill.type==None — so the normal "filled
  // path interior counts as a hit" test never fires for them, and a
  // click in the Compound's filled interior would miss. We patch that
  // here by treating a closed-path interior-bbox hit as a fill hit
  // when recursing inside a Compound. (Cycle-through Ctrl+click relies
  // on hit_test/hit_test_next correctly returning the Compound for
  // interior clicks.)
  std::function<SceneNode *(std::vector<std::unique_ptr<SceneNode>> &, bool)>
      test_children;
  test_children =
      [&](std::vector<std::unique_ptr<SceneNode>> &children,
          bool parent_is_compound) -> SceneNode * {
    // Index 0 is drawn last = visually on top. Iterate 0→N to hit front objects
    // first.
    for (int oi = 0; oi < (int)children.size(); ++oi) {
      SceneNode &obj = *children[oi];
      if (!obj.visible)
        continue;

      if (obj.type == SceneNode::Type::Group ||
          obj.type == SceneNode::Type::Compound) {
        // Test bbox first
        auto bb = object_bbox(obj);
        if (!bb)
          continue;
        if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
            doc_y > bb->y + bb->h)
          continue;
        // Hit inside bbox — test children. Compound children inherit the
        // parent's fill semantics for hit purposes (see helper banner).
        bool nested_is_compound = (obj.type == SceneNode::Type::Compound);
        SceneNode *child_hit = test_children(obj.children, nested_is_compound);
        if (child_hit)
          return &obj; // return the GROUP/COMPOUND, not the child
        continue;
      }

      // ── ClipGroup: like Group, but the click must additionally fall
      //   within the clip shape's bbox — otherwise the pixel the user
      //   clicked is masked out and shouldn't count as a hit.
      //   We return the ClipGroup itself (not a child), matching how
      //   Group hits return the group.
      if (obj.type == SceneNode::Type::ClipGroup) {
        auto bb = object_bbox(obj);
        if (!bb)
          continue;
        if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
            doc_y > bb->y + bb->h)
          continue;
        // Extra gate on the clip shape bbox (if present).
        if (obj.clip_shape) {
          auto cbb = object_bbox(*obj.clip_shape);
          if (cbb) {
            if (doc_x < cbb->x || doc_x > cbb->x + cbb->w || doc_y < cbb->y ||
                doc_y > cbb->y + cbb->h)
              continue;
          }
        }
        SceneNode *child_hit = test_children(obj.children, false);
        if (child_hit)
          return &obj;
        continue;
      }

      // ── TextBox: a sealed container. Any click inside its bbox
      //   (which is the boundary's extent) selects the TextBox as a
      //   whole. We do NOT descend into children — the user can't
      //   independently select the boundary or text from outside the
      //   edit mode. The boundary's fill being None by default means
      //   the click won't land on a stroked outline; treating the
      //   bbox interior as a hit is what makes the textbox feel like
      //   a tangible object the user can grab.
      if (obj.is_text_box_mgr()) {
        auto bb = object_bbox(obj);
        if (!bb)
          continue;
        if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
            doc_y > bb->y + bb->h)
          continue;
        return &obj;
      }

      // ── Blend: bbox gate on A/B/cache. A/B live in slots, not
      //   children, so we build a temporary children-like vector of
      //   pointers. Returning the Blend itself (not A/B/a step) is
      //   the right answer: the Blend is the user-visible object,
      //   and selection of internal A/B will come via LayersPanel in M2.
      //   Any hit inside the Blend's rendered region selects the Blend.
      if (obj.type == SceneNode::Type::Blend) {
        auto bb = object_bbox(obj);
        if (!bb)
          continue;
        if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
            doc_y > bb->y + bb->h)
          continue;
        // Test the actual geometry — walk A/B/cache with bbox + path
        // check via object_bbox on each. Cheap and correct: if any
        // sub-element's bbox contains the point, claim the Blend.
        auto inside = [&](const SceneNode *n) -> bool {
          if (!n)
            return false;
          auto sb = object_bbox(*n);
          return sb && doc_x >= sb->x && doc_x <= sb->x + sb->w &&
                 doc_y >= sb->y && doc_y <= sb->y + sb->h;
        };
        if (inside(obj.blend_source_a.get()) ||
            inside(obj.blend_source_b.get())) {
          return &obj;
        }
        bool cache_hit = false;
        for (const auto &step : obj.blend_cache) {
          if (inside(step.get())) {
            cache_hit = true;
            break;
          }
        }
        if (cache_hit)
          return &obj;
        continue;
      }

      // ── Warp: same pattern as Blend — bbox gate, then per-slot
      //   containment against source + caches. Returning the Warp
      //   itself is the right answer: the Warp is the user-visible
      //   object; source/cache selection comes via LayersPanel.
      if (obj.type == SceneNode::Type::Warp) {
        auto bb = object_bbox(obj);
        if (!bb)
          continue;
        if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
            doc_y > bb->y + bb->h)
          continue;
        auto inside = [&](const SceneNode *n) -> bool {
          if (!n)
            return false;
          auto sb = object_bbox(*n);
          return sb && doc_x >= sb->x && doc_x <= sb->x + sb->w &&
                 doc_y >= sb->y && doc_y <= sb->y + sb->h;
        };
        if (inside(obj.warp_source.get()) ||
            inside(obj.warp_glyph_cache.get()) ||
            inside(obj.warp_cache.get())) {
          return &obj;
        }
        continue;
      }

      // ── Text / Image: use object_bbox for hit testing ─────────────
      if (obj.type == SceneNode::Type::Text ||
          obj.type == SceneNode::Type::Image) {
        auto bb = object_bbox(obj, false);
        if (bb && doc_x >= bb->x && doc_x <= bb->x + bb->w && doc_y >= bb->y &&
            doc_y <= bb->y + bb->h)
          return &obj;
        continue;
      }

      if (obj.type != SceneNode::Type::Path || !obj.path)
        continue;
      const auto &nodes = obj.path->nodes;
      if (nodes.empty())
        continue;

      auto bb = object_bbox(obj);
      if (!bb)
        continue;
      if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
          doc_y > bb->y + bb->h)
        continue;

      // s112 m4: compound children carry no fill of their own (parent
      // owns it by convention), so a click inside the compound's filled
      // interior would otherwise miss. Treat closed-path interior bbox
      // hits as fill hits when recursing inside a Compound. The parent
      // re-tests bbox-containment via the outer Compound branch, so
      // this is sound: we only get here for closed children of a
      // Compound whose own bbox already contains the click.
      // s301 m1e — Text boundaries are interior-hittable even without
      // fill. The boundary is the user-facing primitive ("the bbox");
      // it has fill==None by default so it's visually invisible, but a
      // click in the interior must hit it so the user can select and
      // drag the frame. We treat any closed Path whose iid is
      // referenced by some Text node's text_boundary_ids as having an
      // implicit "fill" for hit-test purposes. The check is O(doc)
      // per Path candidate; fine for click frequency.
      bool is_text_boundary = false;
      if (obj.path->closed && m_doc) {
        for (auto &lyr : m_doc->layers) {
          for (auto &c : lyr->children) {
            if (c && c->is_text() && !c->text_boundary_ids.empty()) {
              for (auto &bid : c->text_boundary_ids) {
                if (bid == obj.internal_id) { is_text_boundary = true; break; }
              }
            }
            if (is_text_boundary) break;
          }
          if (is_text_boundary) break;
        }
      }

      bool has_fill = (obj.fill.type != FillStyle::Type::None) ||
                      (parent_is_compound && obj.path->closed) ||
                      is_text_boundary;
      if (has_fill)
        return &obj;

      double threshold = std::max(obj.stroke.width * 0.5, 2.0);
      double thresh2 = threshold * threshold;

      auto point_on_cubic = [&](double p0x, double p0y, double p1x, double p1y,
                                double p2x, double p2y, double p3x,
                                double p3y) -> bool {
        for (int s = 0; s <= 32; ++s) {
          double t = s / 32.0;
          double mt = 1.0 - t;
          double x = mt * mt * mt * p0x + 3 * mt * mt * t * p1x +
                     3 * mt * t * t * p2x + t * t * t * p3x;
          double y = mt * mt * mt * p0y + 3 * mt * mt * t * p1y +
                     3 * mt * t * t * p2y + t * t * t * p3y;
          double dx = x - doc_x, dy = y - doc_y;
          if (dx * dx + dy * dy <= thresh2)
            return true;
        }
        return false;
      };

      int n = (int)nodes.size();
      bool hit = false;
      for (int i = 0; i < n - 1 && !hit; ++i) {
        hit = point_on_cubic(nodes[i].x, nodes[i].y, nodes[i].cx2, nodes[i].cy2,
                             nodes[i + 1].cx1, nodes[i + 1].cy1, nodes[i + 1].x,
                             nodes[i + 1].y);
      }
      if (!hit && obj.path->closed && n > 1) {
        hit = point_on_cubic(nodes[n - 1].x, nodes[n - 1].y, nodes[n - 1].cx2,
                             nodes[n - 1].cy2, nodes[0].cx1, nodes[0].cy1,
                             nodes[0].x, nodes[0].y);
      }
      if (hit)
        return &obj;
    }
    return nullptr;
  };

  // Iterate layers top-to-bottom (after the z-order fix, index n-1 = visually
  // on top, so walk highest-index first and pick the first hit).
  for (int li = (int)m_doc->layers.size() - 1; li >= 0; --li) {
    auto &layer = m_doc->layers[li];
    if (!layer->visible || layer->locked || layer->is_special_layer())
      continue;
    SceneNode *hit = test_children(layer->children, false);
    if (hit)
      return hit;
  }
  return nullptr;
}

// ── hit_test_next
// ───────────────────────────────────────────────────────────── Like hit_test
// but skips `skip` — returns the next object underneath it. Used for Ctrl+click
// select-through (Illustrator convention).
SceneNode *Canvas::hit_test_next(double doc_x, double doc_y, SceneNode *skip) {
  if (!m_doc || !skip)
    return hit_test(doc_x, doc_y);

  bool found_skip = false;

  // s113: non-skip-aware "does this subtree contain the click" probe.
  // Used by the Group/Compound/ClipGroup branches below to decide
  // "is this composite under the click" independent of where `skip`
  // lives. The original code piggy-backed on the recursive test_children
  // truthiness as the "is hit" signal, but when skip IS the composite
  // itself, the recursion returns null (no child matches skip, found_skip
  // never flips inside) — the caller then bails silently and Ctrl+click
  // gets stuck on the group instead of advancing past it. This helper
  // gives a clean boolean answer with no skip-state coupling.
  std::function<bool(SceneNode &, bool)> subtree_hit;
  subtree_hit = [&](SceneNode &obj, bool parent_is_compound) -> bool {
    if (!obj.visible)
      return false;
    auto bb = object_bbox(obj);
    if (!bb)
      return false;
    if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
        doc_y > bb->y + bb->h)
      return false;

    if (obj.type == SceneNode::Type::Group ||
        obj.type == SceneNode::Type::Compound) {
      bool nested_is_compound = (obj.type == SceneNode::Type::Compound);
      for (auto &ch : obj.children)
        if (subtree_hit(*ch, nested_is_compound))
          return true;
      return false;
    }
    if (obj.type == SceneNode::Type::ClipGroup) {
      if (obj.clip_shape) {
        auto cbb = object_bbox(*obj.clip_shape);
        if (cbb && (doc_x < cbb->x || doc_x > cbb->x + cbb->w ||
                    doc_y < cbb->y || doc_y > cbb->y + cbb->h))
          return false;
      }
      for (auto &ch : obj.children)
        if (subtree_hit(*ch, false))
          return true;
      return false;
    }
    if (obj.type == SceneNode::Type::Text ||
        obj.type == SceneNode::Type::Image) {
      // bbox already passed above
      return true;
    }
    if (obj.type == SceneNode::Type::Blend ||
        obj.type == SceneNode::Type::Warp) {
      // Mirror hit_test's per-slot containment for these — just bbox
      // is sufficient here since the outer bbox already passed; a
      // tighter check matches hit_test's behaviour.
      auto inside = [&](const SceneNode *n) -> bool {
        if (!n)
          return false;
        auto sb = object_bbox(*n);
        return sb && doc_x >= sb->x && doc_x <= sb->x + sb->w &&
               doc_y >= sb->y && doc_y <= sb->y + sb->h;
      };
      if (obj.type == SceneNode::Type::Blend) {
        if (inside(obj.blend_source_a.get()) ||
            inside(obj.blend_source_b.get()))
          return true;
        for (const auto &step : obj.blend_cache)
          if (inside(step.get()))
            return true;
        return false;
      } else { // Warp
        return inside(obj.warp_source.get()) ||
               inside(obj.warp_glyph_cache.get()) ||
               inside(obj.warp_cache.get());
      }
    }
    if (obj.type != SceneNode::Type::Path || !obj.path)
      return false;
    const auto &nodes = obj.path->nodes;
    if (nodes.empty())
      return false;
    bool has_fill = (obj.fill.type != FillStyle::Type::None) ||
                    (parent_is_compound && obj.path->closed);
    if (has_fill)
      return true;
    double threshold = std::max(obj.stroke.width * 0.5, 2.0);
    double thresh2 = threshold * threshold;
    auto point_on_cubic = [&](double p0x, double p0y, double p1x, double p1y,
                              double p2x, double p2y, double p3x,
                              double p3y) -> bool {
      for (int s = 0; s <= 32; ++s) {
        double t = s / 32.0;
        double mt = 1.0 - t;
        double x = mt * mt * mt * p0x + 3 * mt * mt * t * p1x +
                   3 * mt * t * t * p2x + t * t * t * p3x;
        double y = mt * mt * mt * p0y + 3 * mt * mt * t * p1y +
                   3 * mt * t * t * p2y + t * t * t * p3y;
        double ddx = x - doc_x, ddy = y - doc_y;
        if (ddx * ddx + ddy * ddy <= thresh2)
          return true;
      }
      return false;
    };
    int n = (int)nodes.size();
    for (int i = 0; i < n - 1; ++i)
      if (point_on_cubic(nodes[i].x, nodes[i].y, nodes[i].cx2, nodes[i].cy2,
                         nodes[i + 1].cx1, nodes[i + 1].cy1, nodes[i + 1].x,
                         nodes[i + 1].y))
        return true;
    if (obj.path->closed && n > 1) {
      if (point_on_cubic(nodes[n - 1].x, nodes[n - 1].y, nodes[n - 1].cx2,
                         nodes[n - 1].cy2, nodes[0].cx1, nodes[0].cy1,
                         nodes[0].x, nodes[0].y))
        return true;
    }
    return false;
  };

  // s112 m4: same parent_is_compound thread as in hit_test — closed
  // children of a Compound count as filled for hit purposes since the
  // parent owns the fill by convention.
  std::function<SceneNode *(std::vector<std::unique_ptr<SceneNode>> &, bool)>
      test_children;
  test_children =
      [&](std::vector<std::unique_ptr<SceneNode>> &children,
          bool parent_is_compound) -> SceneNode * {
    // Index 0 is drawn last = visually on top. Iterate 0→N to hit front objects
    // first.
    for (int oi = 0; oi < (int)children.size(); ++oi) {
      SceneNode &obj = *children[oi];
      if (!obj.visible)
        continue;

      if (obj.type == SceneNode::Type::Group ||
          obj.type == SceneNode::Type::Compound) {
        // s113: use the non-skip-aware subtree_hit probe so the "is this
        // group hit" decision is independent of where `skip` lives. The
        // old code used the recursive test_children's truthiness as the
        // hit signal, which silently failed when skip == &obj (the group
        // itself): the recursion never matches skip in its own children,
        // returns null, and the caller falls through without setting
        // found_skip — Ctrl+click then gets stuck on the group instead
        // of advancing to the next layer-level sibling.
        bool nested_is_compound = (obj.type == SceneNode::Type::Compound);
        bool group_hit = subtree_hit(obj, parent_is_compound);
        if (!group_hit) {
          continue;
        }
        if (&obj == skip) {
          found_skip = true;
          continue;
        }
        if (found_skip) {
          return &obj;
        }
        // Hit but pre-skip — recurse so that found_skip flips if skip
        // lives inside this group's subtree. After the recursion, if
        // found_skip is now true, this group itself is the "next" hit
        // (cycle-through escapes the group containing skip).
        (void)test_children(obj.children, nested_is_compound);
        if (found_skip) {
          return &obj;
        }
        continue;
      }

      // ── ClipGroup parallel to the hit_test branch above. ─────────────
      // s113: same fix shape as the Group/Compound branch — use
      // subtree_hit for "is this hit" so skip == &obj (the ClipGroup)
      // is handled correctly.
      if (obj.type == SceneNode::Type::ClipGroup) {
        bool group_hit = subtree_hit(obj, false);
        if (!group_hit) {
          continue;
        }
        if (&obj == skip) {
          found_skip = true;
          continue;
        }
        if (found_skip) {
          return &obj;
        }
        // Pre-skip — recurse to let found_skip flip if skip is inside.
        (void)test_children(obj.children, false);
        if (found_skip) {
          return &obj;
        }
        continue;
      }

      if (obj.type == SceneNode::Type::Text ||
          obj.type == SceneNode::Type::Image) {
        auto bb = object_bbox(obj, false);
        if (bb && doc_x >= bb->x && doc_x <= bb->x + bb->w && doc_y >= bb->y &&
            doc_y <= bb->y + bb->h) {
          if (&obj == skip) {
            found_skip = true;
            continue;
          }
          if (found_skip)
            return &obj;
        }
        continue;
      }

      if (obj.type != SceneNode::Type::Path || !obj.path)
        continue;
      const auto &nodes = obj.path->nodes;
      if (nodes.empty())
        continue;

      auto bb = object_bbox(obj);
      if (!bb)
        continue;
      if (doc_x < bb->x || doc_x > bb->x + bb->w || doc_y < bb->y ||
          doc_y > bb->y + bb->h)
        continue;

      // s112 m4: see hit_test for the parent_is_compound rationale.
      bool has_fill = (obj.fill.type != FillStyle::Type::None) ||
                      (parent_is_compound && obj.path->closed);
      bool hit = has_fill;

      if (!hit) {
        double threshold = std::max(obj.stroke.width * 0.5, 2.0);
        double thresh2 = threshold * threshold;
        auto point_on_cubic = [&](double p0x, double p0y, double p1x,
                                  double p1y, double p2x, double p2y,
                                  double p3x, double p3y) -> bool {
          for (int s = 0; s <= 32; ++s) {
            double t = s / 32.0;
            double mt = 1.0 - t;
            double x = mt * mt * mt * p0x + 3 * mt * mt * t * p1x +
                       3 * mt * t * t * p2x + t * t * t * p3x;
            double y = mt * mt * mt * p0y + 3 * mt * mt * t * p1y +
                       3 * mt * t * t * p2y + t * t * t * p3y;
            double ddx = x - doc_x, ddy = y - doc_y;
            if (ddx * ddx + ddy * ddy <= thresh2)
              return true;
          }
          return false;
        };
        int n = (int)nodes.size();
        for (int i = 0; i < n - 1 && !hit; ++i)
          hit = point_on_cubic(nodes[i].x, nodes[i].y, nodes[i].cx2,
                               nodes[i].cy2, nodes[i + 1].cx1, nodes[i + 1].cy1,
                               nodes[i + 1].x, nodes[i + 1].y);
        if (!hit && obj.path->closed && n > 1)
          hit = point_on_cubic(nodes[n - 1].x, nodes[n - 1].y, nodes[n - 1].cx2,
                               nodes[n - 1].cy2, nodes[0].cx1, nodes[0].cy1,
                               nodes[0].x, nodes[0].y);
      }

      if (hit) {
        if (&obj == skip) {
          found_skip = true;
          continue;
        }
        if (found_skip)
          return &obj;
      }
    }
    return nullptr;
  };

  // Iterate layers top-to-bottom (after the z-order fix, index n-1 = visually
  // on top, so walk highest-index first and pick the first hit).
  for (int li = (int)m_doc->layers.size() - 1; li >= 0; --li) {
    auto &layer = m_doc->layers[li];
    if (!layer->visible || layer->locked || layer->is_special_layer())
      continue;
    SceneNode *hit = test_children(layer->children, false);
    if (hit)
      return hit;
  }

  // Wrapped around — nothing else under the cursor, return nullptr
  return nullptr;
}

// ── selection_bbox / selection_bbox_center
// ──────────────────────────── Aggregate bbox across the current selection
// (doc coords, Y-down).  Pattern lifted from rotate_selection_by.
bool Canvas::selection_bbox(double &x, double &y, double &w, double &h) const {
  if (m_selection.empty())
    return false;
  double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
  bool any = false;
  for (SceneNode *obj : m_selection) {
    auto bb = object_bbox(*obj, /*include_stroke=*/false);
    if (!bb)
      continue;
    bx1 = std::min(bx1, bb->x);
    by1 = std::min(by1, bb->y);
    bx2 = std::max(bx2, bb->x + bb->w);
    by2 = std::max(by2, bb->y + bb->h);
    any = true;
  }
  if (!any)
    return false;
  x = bx1;
  y = by1;
  w = bx2 - bx1;
  h = by2 - by1;
  return true;
}

bool Canvas::selection_bbox_center(double &cx, double &cy) const {
  double x, y, w, h;
  if (!selection_bbox(x, y, w, h))
    return false;
  cx = x + w * 0.5;
  cy = y + h * 0.5;
  return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// selection_true_center — s259
//
// Computes the centre of the minimum enclosing circle ("smallest bounding
// circle", "Welzl's circle") of the union of all extremal points across
// the current selection. For each leaf in the selection:
//
//   - Path leaves contribute their anchor positions (transformed by the
//     leaf's `transform`). Handle positions are excluded — for typical
//     shapes the anchors are the extremes, and including handles can
//     pull the centre toward off-shape control points on heavily
//     curved paths.
//   - Image / Text leaves contribute their four bbox corners (using
//     object_bbox, which already accounts for transforms).
//
// Welzl's algorithm runs in expected linear time. We use an iterative
// formulation that operates on a shuffled point copy, maintaining the
// invariant that the current circle encloses all visited points and
// is determined by 0-3 boundary points.
//
// For shapes whose bounding box centre equals their centroid (rect,
// ellipse, regular polygon, regular star), the min-enclosing-circle
// centre coincides with the bbox centre and there is no visible change.
// For irregular shapes (a hand-drawn closed curve, an edited star with
// one tip dragged out, a glyph outline) it gives a no-wobble rotation
// pivot — rotation around the min-enclosing-circle centre keeps the
// shape inside the same disc throughout, where the bbox centre would
// swing extremes out past their starting positions.
// ──────────────────────────────────────────────────────────────────────────────
namespace {

struct Pt2 { double x, y; };

static double pt_dist_sq(Pt2 a, Pt2 b) {
  double dx = a.x - b.x, dy = a.y - b.y;
  return dx * dx + dy * dy;
}

// Circle = (cx, cy, r). Empty circle: r < 0.
struct MecCircle { double cx = 0, cy = 0, r2 = -1.0; };

static bool circle_contains(const MecCircle &c, Pt2 p) {
  if (c.r2 < 0) return false;
  // Small epsilon to tolerate floating-point drift on boundary points.
  return pt_dist_sq({c.cx, c.cy}, p) <= c.r2 + 1e-10;
}

static MecCircle circle_from_2(Pt2 a, Pt2 b) {
  MecCircle c;
  c.cx = 0.5 * (a.x + b.x);
  c.cy = 0.5 * (a.y + b.y);
  double dx = a.x - b.x, dy = a.y - b.y;
  c.r2 = 0.25 * (dx * dx + dy * dy);
  return c;
}

static MecCircle circle_from_3(Pt2 a, Pt2 b, Pt2 c) {
  // Circumcircle of triangle abc. If the three points are collinear the
  // determinant goes to zero — caller should handle that by falling
  // back to the 2-point circle of the diameter pair.
  double ax = a.x, ay = a.y;
  double bx = b.x, by = b.y;
  double cx = c.x, cy = c.y;
  double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
  if (std::abs(d) < 1e-20) {
    // Degenerate — return empty so caller falls through.
    MecCircle bad; bad.r2 = -1.0; return bad;
  }
  double ux = ((ax * ax + ay * ay) * (by - cy) +
               (bx * bx + by * by) * (cy - ay) +
               (cx * cx + cy * cy) * (ay - by)) / d;
  double uy = ((ax * ax + ay * ay) * (cx - bx) +
               (bx * bx + by * by) * (ax - cx) +
               (cx * cx + cy * cy) * (bx - ax)) / d;
  MecCircle out;
  out.cx = ux;
  out.cy = uy;
  double dx = ax - ux, dy = ay - uy;
  out.r2 = dx * dx + dy * dy;
  return out;
}

// Trivial circle through ≤ 3 boundary points.
static MecCircle trivial_circle(const std::vector<Pt2> &R) {
  if (R.empty()) return MecCircle{};
  if (R.size() == 1) {
    MecCircle c; c.cx = R[0].x; c.cy = R[0].y; c.r2 = 0.0; return c;
  }
  if (R.size() == 2) return circle_from_2(R[0], R[1]);
  // 3 — try circumcircle; if degenerate (collinear), the MEC is the
  // 2-point circle of the diameter pair.
  MecCircle c = circle_from_3(R[0], R[1], R[2]);
  if (c.r2 >= 0) return c;
  // Pick the diameter pair = the two points furthest apart.
  double d01 = pt_dist_sq(R[0], R[1]);
  double d02 = pt_dist_sq(R[0], R[2]);
  double d12 = pt_dist_sq(R[1], R[2]);
  if (d01 >= d02 && d01 >= d12) return circle_from_2(R[0], R[1]);
  if (d02 >= d12)               return circle_from_2(R[0], R[2]);
  return circle_from_2(R[1], R[2]);
}

// Welzl's algorithm — iterative form with shuffled input.
static MecCircle min_enclosing_circle(std::vector<Pt2> pts) {
  if (pts.empty()) return MecCircle{};
  // Shuffle for expected-linear-time. Deterministic seed so the same
  // input gives the same output across runs (no jitter on repeated
  // calls during a drag).
  std::mt19937 rng(0xC0FFEEu);
  std::shuffle(pts.begin(), pts.end(), rng);

  MecCircle c = trivial_circle({pts[0]});
  for (size_t i = 1; i < pts.size(); ++i) {
    if (circle_contains(c, pts[i])) continue;
    // pts[i] is on the boundary of the new MEC. Find MEC of pts[0..i]
    // with pts[i] on boundary.
    c = trivial_circle({pts[i]});
    for (size_t j = 0; j < i; ++j) {
      if (circle_contains(c, pts[j])) continue;
      // pts[j] also on boundary now.
      c = circle_from_2(pts[i], pts[j]);
      for (size_t k = 0; k < j; ++k) {
        if (circle_contains(c, pts[k])) continue;
        // pts[i], pts[j], pts[k] all on boundary — try circumcircle.
        MecCircle cc = circle_from_3(pts[i], pts[j], pts[k]);
        if (cc.r2 >= 0 && cc.r2 >= c.r2 - 1e-10) c = cc;
        else c = trivial_circle({pts[i], pts[j], pts[k]});
      }
    }
  }
  return c;
}

// Apply 2D affine transform (SVG matrix(a,b,c,d,e,f)) to a point.
static Pt2 apply_xform(const Transform &t, double x, double y) {
  return { t.a * x + t.c * y + t.e,
           t.b * x + t.d * y + t.f };
}

} // anonymous namespace

bool Canvas::selection_true_center(double &cx, double &cy) const {
  if (m_selection.empty()) return false;
  std::vector<Pt2> pts;
  pts.reserve(64);
  for (SceneNode *obj : m_selection) {
    std::vector<SceneNode *> leaves;
    collect_paths(obj, leaves);
    for (SceneNode *leaf : leaves) {
      if (!leaf->path) continue;
      for (const auto &nd : leaf->path->nodes) {
        pts.push_back(apply_xform(leaf->transform, nd.x, nd.y));
      }
    }
    // For non-path leaves (Image, Text — not collected by collect_paths),
    // fall back to bbox corners. object_bbox accounts for transforms.
    if (obj->is_image() || obj->type == SceneNode::Type::Text) {
      auto bb = object_bbox(*obj, /*include_stroke=*/false);
      if (bb) {
        pts.push_back({bb->x,             bb->y});
        pts.push_back({bb->x + bb->w,     bb->y});
        pts.push_back({bb->x + bb->w,     bb->y + bb->h});
        pts.push_back({bb->x,             bb->y + bb->h});
      }
    }
  }
  if (pts.empty()) {
    // No path vertices and no non-path leaves we could read — fall back
    // to bbox centre so callers always get *some* answer.
    return selection_bbox_center(cx, cy);
  }
  MecCircle c = min_enclosing_circle(std::move(pts));
  if (c.r2 < 0) return selection_bbox_center(cx, cy);
  cx = c.cx;
  cy = c.cy;
  return true;
}

// ── Corner Treatment Tool
// ─────────────────────────────────────────────────────

bool Canvas::corner_sel_contains(SceneNode *obj, int idx) const {
  for (auto &cs : m_corner_selection)
    if (cs.obj == obj && cs.node_idx == idx)
      return true;
  return false;
}

// ── Handle geometry helpers
// ──────────────────────────────────────────────────── Returns the 8 handle
// screen positions for a given BBX. Order: NW, N, NE, E, SE, S, SW, W  (index
// matches HandleKind enum - 1)
void selection_handle_positions(double sx1, double sy1, double sx2,
                                       double sy2, double hx[8], double hy[8]) {
  double mx = (sx1 + sx2) * 0.5;
  double my = (sy1 + sy2) * 0.5;
  hx[0] = sx1;
  hy[0] = sy1; // NW
  hx[1] = mx;
  hy[1] = sy1; // N
  hx[2] = sx2;
  hy[2] = sy1; // NE
  hx[3] = sx2;
  hy[3] = my; // E
  hx[4] = sx2;
  hy[4] = sy2; // SE
  hx[5] = mx;
  hy[5] = sy2; // S
  hx[6] = sx1;
  hy[6] = sy2; // SW
  hx[7] = sx1;
  hy[7] = my; // W
}

Canvas::HandleKind Canvas::handle_hit_test(double sx, double sy) const {
  if (m_selection.empty())
    return HandleKind::None;

  // Compute union BBX of all selected objects
  bool found = false;
  double bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
  for (SceneNode *obj : m_selection) {
    auto bb = object_bbox(*obj);
    if (!bb)
      continue;
    double s1x, s1y, s2x, s2y;
    doc_to_screen(bb->x, bb->y, s1x, s1y);
    doc_to_screen(bb->x + bb->w, bb->y + bb->h, s2x, s2y);
    if (!found) {
      bx1 = s1x;
      by1 = s1y;
      bx2 = s2x;
      by2 = s2y;
      found = true;
    } else {
      bx1 = std::min(bx1, s1x);
      by1 = std::min(by1, s1y);
      bx2 = std::max(bx2, s2x);
      by2 = std::max(by2, s2y);
    }
  }
  if (!found)
    return HandleKind::None;

  double hx[8], hy[8];
  selection_handle_positions(bx1, by1, bx2, by2, hx, hy);

  // Test corners first (higher priority — they overlap edge mid zones at small
  // sizes) Array index matches HandleKind enum order:
  // NW=1,N=2,NE=3,E=4,SE=5,S=6,SW=7,W=8 positions array:
  // 0=NW,1=N,2=NE,3=E,4=SE,5=S,6=SW,7=W
  const int pos_idx[8] = {0, 2, 4, 6, 1, 3, 5, 7};
  const HandleKind hkind[8] = {
      HandleKind::NW, HandleKind::NE,
      HandleKind::SE, HandleKind::SW, // corners first
      HandleKind::N,  HandleKind::E,
      HandleKind::S,  HandleKind::W // then edges
  };
  for (int i = 0; i < 8; ++i) {
    double ddx = sx - hx[pos_idx[i]];
    double ddy = sy - hy[pos_idx[i]];
    if (std::abs(ddx) <= HANDLE_HIT_PX && std::abs(ddy) <= HANDLE_HIT_PX)
      return hkind[i];
  }

  // Rotate zones — annular ring just outside each corner square.
  // Inner radius = HANDLE_HIT_PX (edge of corner square).
  // Outer radius = HANDLE_HIT_PX + ROTATE_RING_PX.
  static constexpr double ROTATE_RING_PX = 10.0;
  const int corner_pos[4] = {0, 2, 4, 6};
  const HandleKind rotate_kind[4] = {HandleKind::RotateNW, HandleKind::RotateNE,
                                     HandleKind::RotateSE,
                                     HandleKind::RotateSW};
  for (int i = 0; i < 4; ++i) {
    double ddx = sx - hx[corner_pos[i]];
    double ddy = sy - hy[corner_pos[i]];
    double dist = std::hypot(ddx, ddy);
    if (dist >= HANDLE_HIT_PX && dist <= HANDLE_HIT_PX + ROTATE_RING_PX)
      return rotate_kind[i];
  }

  return HandleKind::None;
}

// ── Ruler Tool
// ────────────────────────────────────────────────────────────────

// Called on tool switch from Node tool — inherit exactly 2 selected nodes.
void Canvas::ruler_try_inherit_node_selection() {
  m_ruler_node_a_obj = nullptr;
  m_ruler_node_a_idx = -1;
  m_ruler_node_b_obj = nullptr;
  m_ruler_node_b_idx = -1;
  if (m_node_selection.size() == 2) {
    m_ruler_node_a_obj = m_node_selection[0].obj;
    m_ruler_node_a_idx = m_node_selection[0].node_idx;
    m_ruler_node_b_obj = m_node_selection[1].obj;
    m_ruler_node_b_idx = m_node_selection[1].node_idx;
  }
  queue_draw();
}

// Collect all visible, unlocked PATH NODES across all layers. Used by
// non-ruler callers (guide-construct snap, selection-tool node-snap,
// hover highlight) that explicitly want path-node-only results — they
// read obj->path->nodes[ni] directly. Refpt callers must use
// ruler_collect_all_endpoints.
//
// s182: descends into Compound and Group containers so child Path nodes
// are pickable too. Pre-s182 the walk was flat over each layer's direct
// children, which silently skipped path nodes that lived inside a
// compound or group — invisible to the ruler tool's snap-and-highlight
// pass even though they were real, visible points on the canvas. Special
// layers (Guide / Grid / Margin / Measure / Ref) stay excluded by the
// is_special_layer filter at the top; refpts ride a separate explicit
// pass in ruler_collect_all_endpoints.
//
// Lock semantics: the codebase has no automatic ancestor-lock
// inheritance (a locked group doesn't auto-lock its children — the
// LayersPanel just greys the names). The recursion still bails on a
// locked container as a guard rail, since the user's intent in locking
// a container is generally "leave the contents alone," and the alternative
// (descending into a locked group's children) would let the ruler tool
// pick endpoints from a thing the user has signalled is hands-off.
void Canvas::ruler_collect_all_path_nodes(
    std::vector<std::pair<SceneNode *, int>> &out) const {
  if (!m_doc)
    return;
  // Recursive walk: emit endpoints from Path nodes, descend into Compound
  // and Group containers. Anything else (Text, Image, Blend, Warp, ...)
  // is intentionally skipped — those types don't expose pickable BezierNode
  // anchors. Future container-style types should be added here.
  std::function<void(SceneNode &)> walk = [&](SceneNode &node) {
    if (node.locked)
      return;
    if (node.type == SceneNode::Type::Path && node.path) {
      for (int i = 0; i < (int)node.path->nodes.size(); ++i)
        out.push_back({&node, i});
      return;
    }
    if (node.type == SceneNode::Type::Compound ||
        node.type == SceneNode::Type::Group) {
      for (auto &ch_uptr : node.children) {
        if (!ch_uptr)
          continue;
        walk(*ch_uptr);
      }
    }
  };
  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->locked || layer->is_special_layer())
      continue;
    for (auto &obj_uptr : layer->children) {
      if (!obj_uptr)
        continue;
      walk(*obj_uptr);
    }
  }
}

// Collect all visible, unlocked endpoints across all layers — path nodes
// AND refpts. Refpts emitted with idx = -1 (sentinel; see ruler_endpoint_pos).
// The ref layer is a special layer normally skipped by the regular layer
// filter — handled explicitly here so refpts are pickable from the ruler
// tool. Renamed from ruler_collect_all_nodes in S89.
void Canvas::ruler_collect_all_endpoints(
    std::vector<std::pair<SceneNode *, int>> &out) const {
  if (!m_doc)
    return;
  // Path nodes — same set as ruler_collect_all_path_nodes.
  ruler_collect_all_path_nodes(out);
  // Refpts — visible + unlocked entries on the ref layer, idx = -1.
  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->locked)
      continue;
    if (!layer->is_ref_layer())
      continue;
    for (auto &child_uptr : layer->children) {
      SceneNode &child = *child_uptr;
      if (!child.is_ref())
        continue;
      if (child.locked)
        continue;
      out.push_back({&child, -1});
    }
  }
}

// Resolve an endpoint to a doc-space (Y-down) position. Branch on kind:
//   - Ref:  returns (ref_x, ref_y) directly.
//   - Node: returns the BezierNode's (x, y) at the given index.
// Returns false if the SceneNode pointer is null or not a recognised
// endpoint kind, or if a Node idx is out of range.
bool Canvas::ruler_endpoint_pos(SceneNode *obj, int idx,
                                double &x_doc, double &y_doc) const {
  if (!obj)
    return false;
  if (obj->is_ref()) {
    x_doc = obj->ref_x;
    y_doc = obj->ref_y;
    return true;
  }
  if (obj->type == SceneNode::Type::Path && obj->path &&
      idx >= 0 && idx < (int)obj->path->nodes.size()) {
    const BezierNode &n = obj->path->nodes[idx];
    x_doc = n.x;
    y_doc = n.y;
    return true;
  }
  return false;
}

// Clear ruler state — Space key, fresh pick.
void Canvas::ruler_clear() {
  m_ruler_node_a_obj = nullptr;
  m_ruler_node_a_idx = -1;
  m_ruler_node_b_obj = nullptr;
  m_ruler_node_b_idx = -1;
  m_ruler_labels.clear();
  if (m_ruler_toast_conn.connected())
    m_ruler_toast_conn.disconnect();
  m_ruler_toast_ms = 0;
  m_marquee_active = false;
  queue_draw();
}

// s182 m4 — show a toast at a given screen-space position. Reused by
// click-to-copy success ("Copied measurement data") and click-rejection
// ("No measurement point at this location"). Cancels any in-flight
// toast and starts a fresh 1500ms countdown ticking at 50ms.
void Canvas::ruler_show_toast(const std::string &text, double screen_x,
                              double screen_y) {
  m_ruler_toast_text = text;
  m_ruler_toast_x = screen_x;
  m_ruler_toast_y = screen_y;
  m_ruler_toast_ms = 1500;
  if (m_ruler_toast_conn.connected())
    m_ruler_toast_conn.disconnect();
  m_ruler_toast_conn = Glib::signal_timeout().connect(
      [this]() -> bool {
        m_ruler_toast_ms -= 50;
        if (m_ruler_toast_ms <= 0)
          m_ruler_toast_ms = 0;
        queue_draw();
        return m_ruler_toast_ms > 0;
      },
      50);
  queue_draw();
}

// Called when Enter is pressed while Ruler tool is active.
void Canvas::ruler_place_measurement() {
  if (!m_doc)
    return;
  if (!m_ruler_node_a_obj || !m_ruler_node_b_obj)
    return;

  // S89: endpoint position resolves both Node and Ref kinds. If the read
  // fails for either side (e.g. a stale path-node idx after a path edit
  // wiped the underlying nodes), bail rather than write garbage coords.
  double na_x, na_y, nb_x, nb_y;
  if (!ruler_endpoint_pos(m_ruler_node_a_obj, m_ruler_node_a_idx, na_x, na_y))
    return;
  if (!ruler_endpoint_pos(m_ruler_node_b_obj, m_ruler_node_b_idx, nb_x, nb_y))
    return;

  // Convert doc-space Y-down → user-space Y-up
  double ax = na_x, ay = m_doc->canvas_height() - na_y;
  double bx = nb_x, by = m_doc->canvas_height() - nb_y;

  SceneNode *ml = m_doc->ensure_measure_layer();

  // S89: no display name stored — the layers panel and inspector both
  // synthesise a coords-with-unit string at render time so the label is
  // always live with respect to the doc's display unit. internal_id is
  // a fresh UUID giving stable identity across renames/reorders/round-
  // trip, mirroring how every other SceneNode is identified.
  auto mn = std::make_unique<SceneNode>();
  mn->type = SceneNode::Type::Measurement;
  mn->internal_id = generate_internal_id();
  mn->measure_x1 = ax;
  mn->measure_y1 = ay;
  mn->measure_x2 = bx;
  mn->measure_y2 = by;
  ml->children.push_back(std::move(mn));

  // Signal layer panel + inspector to rebuild. Selection signal hits the
  // existing layers-panel refresh chain; measurements_changed is the
  // dedicated S89 signal so MainWindow can refresh the inspector's
  // saved-measurements list (signal_selection_changed dedups against
  // current_object and skips when no object is selected).
  // s159 m2: kept as bare emit — refresh-side-effect, not selection change.
  // Revisit when the s155-backlog "always-visible inspector categories"
  // refactor retires the panel-refresh hijack pattern.
  //
  // s182 m6: also emit doc_changed. Adding a Measurement node to the
  // measure layer is a structural change to the doc tree — same shape
  // as any other "object added to scene" site. The MainWindow's
  // doc_changed listener calls m_layers.refresh(), which is what makes
  // the new measurement appear in Layers > Measurements right away.
  // Pre-s182 the bare m_sig_selection.emit(nullptr) was hooked to the
  // inspector refresh path only, leaving the LayersPanel stale until
  // some unrelated event (selection click, tool switch) re-triggered
  // the refresh chain.
  m_sig_selection.emit(nullptr);
  m_sig_measurements_changed.emit();
  m_sig_doc_changed.emit();
  queue_draw();
}

// S89: auto-save the live {A,B} measurement when measure_save_to_layer is ON.
// Called from completion paths in on_ruler_begin (shift+click) and on_ruler_end
// (marquee-with-2). On save the live A/B is cleared (acts like an implicit
// Space) — the persistent overlay then re-renders the same triangle + labels
// from the saved entry, so the user gets visual continuity. Returns true on
// save. NOT called from ruler_try_inherit_node_selection — inheritance from
// the Node tool is not a user-initiated completion.
bool Canvas::ruler_try_auto_save() {
  if (!m_doc)
    return false;
  if (!m_doc->measure_save_to_layer)
    return false;
  if (!m_ruler_node_a_obj || !m_ruler_node_b_obj)
    return false;
  ruler_place_measurement();
  // Clear live picks — implicit Space. The persistent overlay (drawn after
  // the live one in on_draw) will render the just-saved entry at the same
  // screen coords with the same triangle + labels, so visual continuity is
  // preserved. m_ruler_labels are rebuilt every frame by draw_ruler_overlay,
  // so clearing here just lets the next frame populate them from the
  // persistent measurement instead of the live picks.
  m_ruler_node_a_obj = nullptr;
  m_ruler_node_a_idx = -1;
  m_ruler_node_b_obj = nullptr;
  m_ruler_node_b_idx = -1;
  m_ruler_labels.clear();
  return true;
}

// S89: structured copy block formatter — takes user-space coords explicitly
// so it can be called for both live ruler picks AND saved measurements.
// Returns the multi-line "x1 = ..., y1 = ...\n..." block, values formatted
// in the doc's display unit. Static so the persistent overlay can call it
// without an instance reference.
std::string Canvas::format_structured_block_for(const CurvzDocument *doc,
                                                double ax, double ay,
                                                double bx, double by) {
  if (!doc)
    return {};
  double dx = std::abs(bx - ax);
  double dy = std::abs(by - ay);
  double dist = std::hypot(bx - ax, by - ay);
  // Angle CCW from +X axis (Y-up), in degrees
  double angle_rad = std::atan2(by - ay, bx - ax);
  double angle_deg = angle_rad * 180.0 / M_PI;
  if (angle_deg < 0)
    angle_deg += 360.0;
  double alpha = std::atan2(dy, dx) * 180.0 / M_PI; // angle at A
  double beta = 90.0 - alpha;                       // angle at B

  Unit u = doc->canvas.display_unit;
  const char *ul = UnitSystem::label(u);
  auto fmt = [&](double v) -> std::string {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4g %s", UnitSystem::from_px(v, u), ul);
    return buf;
  };
  auto fmt_deg = [](double v) -> std::string {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f°", v);
    return buf;
  };

  std::string s;
  s += "x\u2081 = " + fmt(ax) + ",  y\u2081 = " + fmt(ay) + "\n";
  s += "x\u2082 = " + fmt(bx) + ",  y\u2082 = " + fmt(by) + "\n";
  s += "\u0394X = " + fmt(dx) + ",  \u0394Y = " + fmt(dy) + "\n";
  s += "Distance = " + fmt(dist) + "\n";
  s += "Angle = " + fmt_deg(angle_deg) + ",  \u03b1 = " + fmt_deg(alpha) +
       ",  \u03b2 = " + fmt_deg(beta) + "\n";
  return s;
}

// Thin wrapper — resolve live A/B endpoints to user-space coords, then
// defer to format_structured_block_for. Returns empty when picks are
// incomplete or the resolver fails.
std::string Canvas::ruler_structured_block() const {
  if (!m_ruler_node_a_obj || !m_ruler_node_b_obj)
    return {};
  if (!m_doc)
    return {};
  // S89: endpoint position resolves both Node and Ref kinds.
  double na_x, na_y, nb_x, nb_y;
  if (!ruler_endpoint_pos(m_ruler_node_a_obj, m_ruler_node_a_idx, na_x, na_y))
    return {};
  if (!ruler_endpoint_pos(m_ruler_node_b_obj, m_ruler_node_b_idx, nb_x, nb_y))
    return {};
  // User-space Y-up
  double ax = na_x, ay = m_doc->canvas_height() - na_y;
  double bx = nb_x, by = m_doc->canvas_height() - nb_y;
  return format_structured_block_for(m_doc, ax, ay, bx, by);
}

// ── Text-on-Path Tool
// ─────────────────────────────────────────────────────────

// Build cumulative arc-length table. arc_table[i] = total length up to
// the start of segment i. Returns total path length.
double Canvas::build_arc_table(const BezierPath &bp,
                               std::vector<double> &arc_table) const {
  int n = bp.segment_count();
  arc_table.clear();
  arc_table.reserve(n + 1);
  double total = 0.0;
  arc_table.push_back(0.0);
  for (int i = 0; i < n; ++i) {
    double len = bp.segment(i).length(32);
    total += len;
    arc_table.push_back(total);
  }
  LOG_DEBUG("build_arc_table: {} segs total_len={:.1f}", n, total);
  return total;
}

// Given arc offset (clamped/wrapped), return position + tangent angle on path.
bool Canvas::path_point_at(const BezierPath &bp,
                           const std::vector<double> &arc_table,
                           double total_len, double arc_offset, Vec2 &pos,
                           double &angle) const {
  if (total_len < 0.001 || arc_table.empty())
    return false;
  // Clamp to path length
  arc_offset = std::max(0.0, std::min(arc_offset, total_len));

  int n = bp.segment_count();
  for (int i = 0; i < n; ++i) {
    double seg_start = arc_table[i];
    double seg_end = arc_table[i + 1];
    double seg_len = seg_end - seg_start;
    if (arc_offset <= seg_end || i == n - 1) {
      double local =
          (seg_len > 0.001) ? (arc_offset - seg_start) / seg_len : 0.0;
      local = std::max(0.0, std::min(1.0, local));
      CubicSegment seg = bp.segment(i);
      Vec2 pt = seg.at(local);
      Vec2 tan = seg.tangent(local);
      pos = pt;
      angle = std::atan2(tan.y, tan.x);
      return true;
    }
  }
  return false;
}

// Find a path SceneNode in the document by its id string.
SceneNode *Canvas::top_find_path_by_id(const std::string &id) const {
  if (!m_doc || id.empty())
    return nullptr;
  // Search by internal_id (stable UUID) — not SVG id which collides across docs
  std::function<SceneNode *(SceneNode *)> find =
      [&](SceneNode *n) -> SceneNode * {
    if (n->is_path() && n->internal_id == id)
      return n;
    for (auto &ch : n->children) {
      if (auto *r = find(ch.get()))
        return r;
    }
    return nullptr;
  };
  for (auto &layer : m_doc->layers) {
    if (auto *r = find(layer.get())) {
      if (r->path && !r->path->nodes.empty()) {
        const auto &n0 = r->path->nodes.front();
        const auto &n1 = r->path->nodes.back();
        LOG_DEBUG("top_find_path_by_id: found iid='{}' svg_id='{}' nodes={} "
                  "first=({:.1f},{:.1f}) last=({:.1f},{:.1f})",
                  id, r->id, r->path->nodes.size(), n0.x, n0.y, n1.x, n1.y);
      }
      return r;
    }
  }
  // Dump all path internal_ids to diagnose string mismatch
  LOG_DEBUG("top_find_path_by_id: NOT FOUND len={} — searching iid='{}'",
            id.size(), id);
  std::function<void(SceneNode *)> dump_ids = [&](SceneNode *n) {
    if (n->is_path()) {
      LOG_DEBUG("  path svg_id='{}' iid='{}' iid_len={} match={}", n->id,
                n->internal_id, n->internal_id.size(), (n->internal_id == id));
    }
    for (auto &ch : n->children)
      dump_ids(ch.get());
  };
  for (auto &layer : m_doc->layers)
    dump_ids(layer.get());
  return nullptr;
}

// Computes the doc-space position where a linked text node's anchor sits on
// its guide path.  Used when detaching so text_x/text_y land where the text
// was visually, making the node immediately re-selectable.
// Returns false if the guide path can't be found.
bool Canvas::top_compute_detach_position(const SceneNode &tn, double &out_x,
                                         double &out_y) const {
  if (tn.text_path_id.empty())
    return false;
  SceneNode *guide = top_find_path_by_id(tn.text_path_id);
  if (!guide || !guide->path)
    return false;
  BezierPath bp = BezierPath::from_path_data(*guide->path);
  std::vector<double> arc_table;
  double total = build_arc_table(bp, arc_table);
  double arc =
      tn.text_path_flip ? total - tn.text_path_offset : tn.text_path_offset;
  arc = std::max(0.0, std::min(arc, total));
  Vec2 pos;
  double angle;
  if (!path_point_at(bp, arc_table, total, arc, pos, angle))
    return false;
  out_x = pos.x;
  // Convert doc Y-down back to text_y (which is Y-down baseline)
  out_y = pos.y;
  return true;
}

// node in the document.  Used to suppress fill/stroke on guide paths in
// normal render mode (they should be invisible outside outline mode).
bool Canvas::is_top_guide_path(const SceneNode &node) const {
  if (!m_doc || node.internal_id.empty())
    return false;
  for (const auto &layer : m_doc->layers) {
    for (const auto &child : layer->children) {
      if (child->is_text() && child->text_path_id == node.internal_id)
        return true;
    }
  }
  return false;
}

// Returns the partner of a PTT pair:
//   - If node is a linked text node  → returns its guide path SceneNode
//   - If node is a guide path        → returns the text node that links to it
//   - Otherwise                      → returns nullptr
SceneNode *Canvas::top_pair_partner(SceneNode *node) const {
  if (!m_doc || !node)
    return nullptr;
  // Text node case: look up its guide path
  if (node->is_text() && !node->text_path_id.empty())
    return top_find_path_by_id(node->text_path_id);
  // Path node case: scan for a text node referencing it
  if (node->is_path() && !node->internal_id.empty()) {
    for (const auto &layer : m_doc->layers) {
      for (const auto &child : layer->children) {
        if (child->is_text() && child->text_path_id == node->internal_id)
          return child.get();
      }
    }
  }
  return nullptr;
}

void Canvas::set_guide_selection(const std::vector<SceneNode *> &sel) {
  m_guide_selection = sel;
  // Selecting guides clears object selection silently — no signal emit
  // to avoid a round-trip that would wipe the guide selection we just set.
  if (!sel.empty() && !m_selection.empty()) {
    m_selection.clear();
    m_selected = nullptr;
    m_selected_node = -1;
  }
  queue_draw();
  // Called from external code (LayersPanel/inspector) — do not re-emit.
}

void Canvas::clear_guide_selection() {
  if (m_guide_selection.empty())
    return;
  m_guide_selection.clear();
  queue_draw();
}

void Canvas::delete_selected_guides() {
  if (!m_doc)
    return;
  SceneNode *gl = m_doc->guide_layer();
  if (!gl || m_guide_selection.empty())
    return;
  for (SceneNode *g : m_guide_selection) {
    if (m_guide_hovered == g)
      m_guide_hovered = nullptr;
    if (m_guide_drag_node == g) {
      m_guide_drag_node = nullptr;
      m_guide_drag_active = false;
    }
    gl->children.erase(std::remove_if(gl->children.begin(), gl->children.end(),
                                      [g](const std::unique_ptr<SceneNode> &c) {
                                        return c.get() == g;
                                      }),
                       gl->children.end());
  }
  m_guide_selection.clear();
  m_sig_guide_selection_changed.emit(m_guide_selection);
  m_sig_doc_changed.emit();
  queue_draw();
}

void Canvas::delete_guide(SceneNode *g) {
  if (!m_doc || !g)
    return;
  SceneNode *gl = m_doc->guide_layer();
  if (!gl)
    return;
  auto it = std::find_if(
      gl->children.begin(), gl->children.end(),
      [g](const std::unique_ptr<SceneNode> &c) { return c.get() == g; });
  if (it == gl->children.end())
    return;
  // Remove from selection set, clear hover/drag if pointing at this guide
  m_guide_selection.erase(
      std::remove(m_guide_selection.begin(), m_guide_selection.end(), g),
      m_guide_selection.end());
  if (m_guide_hovered == g)
    m_guide_hovered = nullptr;
  if (m_guide_drag_node == g) {
    m_guide_drag_node = nullptr;
    m_guide_drag_active = false;
  }
  gl->children.erase(it);
  m_sig_doc_changed.emit();
  queue_draw();
}

} // namespace Curvz
