#include "ShortcutsDialog.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2
#include <gtkmm/label.h>

namespace Curvz {

// ── Constructor
// ───────────────────────────────────────────────────────────────
ShortcutsDialog::ShortcutsDialog() {
  curvz::utils::set_name(*this, "dlg_sh", "shortcuts_dialog_root");
  set_title("Keyboard Shortcuts");
  set_modal(false);
  set_resizable(true);
  set_default_size(640, 700);
  set_hide_on_close(true);

  // ── Notebook tabs ─────────────────────────────────────────────────────
  m_notebook.set_expand(true);
  curvz::utils::set_name(m_notebook, "dlg_sh_nb", "shortcuts_dialog_notebook");
  m_notebook.append_page(build_keyboard_tab(), "Keyboard");
  m_notebook.append_page(build_mouse_tab(), "Mouse & Modifiers");

  // ── Close button ──────────────────────────────────────────────────────
  m_btn_close.set_halign(Gtk::Align::END);
  m_btn_close.set_margin(8);
  curvz::utils::set_name(m_btn_close, "dlg_sh_cls", "shortcuts_dialog_close_btn");
  m_btn_close.signal_clicked().connect([this]() { hide(); });

  // ── Root layout ───────────────────────────────────────────────────────
  m_root.append(m_notebook);
  m_root.append(m_btn_close);
  set_child(m_root);
}

void ShortcutsDialog::show(Gtk::Window &parent) {
  set_transient_for(parent);
  curvz::utils::apply_motif_class_from_parent(*this, parent);  // s117 m18 v2
  present();
}

// ── Helpers
// ───────────────────────────────────────────────────────────────────

int ShortcutsDialog::add_heading(Gtk::Grid &grid, const std::string &title,
                                 int row) {
  auto *lbl = Gtk::make_managed<Gtk::Label>(title);
  lbl->set_halign(Gtk::Align::START);
  lbl->set_margin_top(12);
  lbl->set_margin_bottom(2);
  lbl->add_css_class("heading");
  grid.attach(*lbl, 0, row, 2, 1);
  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  sep->set_margin_bottom(4);
  grid.attach(*sep, 0, row + 1, 2, 1);
  return row + 2;
}

int ShortcutsDialog::add_row(Gtk::Grid &grid, const std::string &keys,
                             const std::string &desc, int row) {
  auto *key_lbl = Gtk::make_managed<Gtk::Label>(keys);
  key_lbl->set_halign(Gtk::Align::START);
  key_lbl->set_margin_start(8);
  key_lbl->set_margin_end(16);
  key_lbl->set_margin_top(2);
  key_lbl->set_margin_bottom(2);
  key_lbl->add_css_class("monospace");

  auto *desc_lbl = Gtk::make_managed<Gtk::Label>(desc);
  desc_lbl->set_halign(Gtk::Align::START);
  desc_lbl->set_margin_top(2);
  desc_lbl->set_margin_bottom(2);

  grid.attach(*key_lbl, 0, row);
  grid.attach(*desc_lbl, 1, row);
  return row + 1;
}

int ShortcutsDialog::add_spacer(Gtk::Grid &grid, int row) {
  auto *lbl = Gtk::make_managed<Gtk::Label>("");
  lbl->set_margin_top(4);
  grid.attach(*lbl, 0, row, 2, 1);
  return row + 1;
}

// ── Keyboard tab  (sections sorted A–Z)
// ──────────────────────────────────────────────────────────────
Gtk::Widget &ShortcutsDialog::build_keyboard_tab() {
  m_keyboard_grid.set_margin(12);
  m_keyboard_grid.set_column_spacing(8);
  m_keyboard_grid.set_row_spacing(0);
  m_keyboard_grid.set_column_homogeneous(false);

  int r = 0;

  // ── Align ─────────────────────────────────────────────────────────────
  // s135 m1: Align & Distribute promoted to a menu+hotkey path. Six align
  // ops have hotkeys; distribute is menu-only by design. All gated on
  // Selection tool active + 2+ objects selected.
  r = add_heading(m_keyboard_grid, "Align", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+L",   "Align left", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+H",   "Align center horizontal", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+R",   "Align right", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+P",   "Align top", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+M",   "Align center vertical (middle)", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+B",   "Align bottom", r);

  // ── Arrange ───────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Arrange", r);
  r = add_row(m_keyboard_grid, "Ctrl+↑", "Bring forward", r);
  r = add_row(m_keyboard_grid, "Ctrl+↓", "Send backward", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+↑", "Bring to front", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+↓", "Send to back", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+H", "Flip horizontal", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+V", "Flip vertical", r);

  // ── Developer ─────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Developer", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+D", "Open GTK Inspector", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+Shift+B", "Toggle Clipper2 boolean engine (dev A/B)", r);

  // ── Edit ──────────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Edit", r);
  r = add_row(m_keyboard_grid, "Ctrl+Z", "Undo", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+Z", "Redo", r);
  r = add_row(m_keyboard_grid, "Ctrl+Y", "Redo (alternate)", r);
  r = add_row(m_keyboard_grid, "Ctrl+A", "Select all", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+A", "Deselect all", r);
  r = add_row(m_keyboard_grid, "Ctrl+C", "Copy", r);
  r = add_row(m_keyboard_grid, "Ctrl+X", "Cut", r);
  r = add_row(m_keyboard_grid, "Ctrl+V", "Paste", r);
  r = add_row(m_keyboard_grid, "Ctrl+D", "Duplicate", r);
  r = add_row(m_keyboard_grid, "Alt+D", "Clone (duplicate, original stays)", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+D", "Step and Repeat…", r);
  r = add_row(m_keyboard_grid, "Del / Backspace", "Delete selected object or guide", r);

  // ── File ──────────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "File", r);
  r = add_row(m_keyboard_grid, "Ctrl+N", "New document", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+N", "New project", r);
  r = add_row(m_keyboard_grid, "Ctrl+O", "Open document", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+W", "Close project", r);
  r = add_row(m_keyboard_grid, "Ctrl+S", "Save", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+S", "Save as", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+S", "Save as Template…", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+T", "Manage Templates…", r);
  r = add_row(m_keyboard_grid, "Ctrl+I", "Import SVG…", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+I", "Import as Icon…", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+P", "Place Image…", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+T", "Export Theme…", r);
  r = add_row(m_keyboard_grid, "Ctrl+P", "Print / export dialog", r);
  r = add_row(m_keyboard_grid, "Ctrl+Q", "Quit", r);
  r = add_row(m_keyboard_grid, "Ctrl+W", "Quit (alternate)", r);

  // ── Documents ─────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Documents", r);
  r = add_row(m_keyboard_grid, "Ctrl+Tab  /  Ctrl+PgDn",       "Next document", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+Tab  /  Ctrl+PgUp", "Previous document", r);

  // ── Help ──────────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Help", r);
  r = add_row(m_keyboard_grid, "?  or  /", "Show this shortcuts window", r);
  r = add_row(m_keyboard_grid, "F1  or  Alt+?", "Open the help manual", r);

  // ── Line Tool ─────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Line Tool", r);
  r = add_row(m_keyboard_grid, "Enter", "Commit line", r);
  r = add_row(m_keyboard_grid, "Escape", "Cancel line", r);

  // ── Macros ────────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Macros", r);
  r = add_row(m_keyboard_grid, "Ctrl+M", "Run current macro", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+M", "Macro Manager…", r);

  // ── Node Tool ─────────────────────────────────────────────────────────
  // All Node-tool keys require the Node tool active, a Path selected,
  // and no modifier held. Type keys: one key per type (aliases S, Q
  // removed in Session 59 to unshadow global S→Selection, Q→toggle-snap).
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Node Tool", r);
  r = add_row(m_keyboard_grid, "Del / Backspace", "Delete selected node (deletes object if last node)", r);
  r = add_row(m_keyboard_grid, "Tab", "Select next node", r);
  r = add_row(m_keyboard_grid, "Shift+Tab", "Select previous node", r);
  r = add_row(m_keyboard_grid, "J", "Join paths / close or open path (context-aware)", r);
  r = add_row(m_keyboard_grid, "B", "Break path at selected node", r);
  r = add_row(m_keyboard_grid, "R", "Reverse path direction", r);
  r = add_row(m_keyboard_grid, "A", "Set node type: Symmetric", r);
  r = add_row(m_keyboard_grid, "M", "Set node type: Smooth", r);
  r = add_row(m_keyboard_grid, "C", "Set node type: Cusp", r);
  r = add_row(m_keyboard_grid, "K", "Set node type: Corner", r);
  r = add_row(m_keyboard_grid, "↑↓←→", "Nudge node (2 screen px)", r);
  r = add_row(m_keyboard_grid, "Shift+↑↓←→", "Nudge node (8 screen px)", r);
  r = add_row(m_keyboard_grid, "Alt+↑↓←→", "Nudge node (32 screen px)", r);

  // ── Object ────────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Object", r);
  r = add_row(m_keyboard_grid, "Ctrl+G", "Group selection", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+G", "Ungroup selection", r);
  r = add_row(m_keyboard_grid, "Ctrl+8", "Make compound path", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+8", "Split compound path", r);
  r = add_row(m_keyboard_grid, "Ctrl+7", "Make clip group (arms pick mode)", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+7", "Release clip group", r);

  // ── Path ──────────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Path", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+U", "Union", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+E", "Subtract", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+I", "Intersect", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+O", "Offset Path…", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+X", "Expand Stroke", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+T", "Convert Text to Path", r);
  r = add_row(m_keyboard_grid, "Ctrl+B", "Blend", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+B", "Release Blend", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+Y", "Warp", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+Y", "Edit Warp", r);
  r = add_row(m_keyboard_grid, "Ctrl+Alt+F", "Flatten Warp", r);

  // ── Pen Tool ──────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Pen Tool", r);
  r = add_row(m_keyboard_grid, "Enter", "Commit open path", r);
  r = add_row(m_keyboard_grid, "Escape", "Cancel / discard path", r);

  // ── Ruler Tool ────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Ruler Tool", r);
  r = add_row(m_keyboard_grid, "Enter", "Place measurement annotation", r);
  r = add_row(m_keyboard_grid, "Space", "Clear and start new measurement", r);

  // ── Text ──────────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Text", r);
  r = add_row(m_keyboard_grid, "Shift+U", "Release text from path", r);

  // ── Text Tool ─────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Text Tool", r);
  r = add_row(m_keyboard_grid, "Enter", "Commit text edit", r);
  r = add_row(m_keyboard_grid, "Escape", "Cancel text edit", r);

  // ── Tools ─────────────────────────────────────────────────────────────
  // Single-key tool hotkeys only fire when no modifier is held and no
  // text widget has focus.
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Tools", r);
  r = add_row(m_keyboard_grid, "S", "Selection tool", r);
  r = add_row(m_keyboard_grid, "N", "Node tool", r);
  r = add_row(m_keyboard_grid, "P", "Pen tool", r);
  r = add_row(m_keyboard_grid, "R", "Rectangle tool  (or toggle pivot mode if Selection+selection)", r);
  r = add_row(m_keyboard_grid, "E", "Ellipse tool", r);
  r = add_row(m_keyboard_grid, "L", "Line tool", r);
  r = add_row(m_keyboard_grid, "T", "Text tool", r);
  r = add_row(m_keyboard_grid, "U", "Text-on-Path tool", r);
  r = add_row(m_keyboard_grid, "F", "Reference Point tool", r);
  r = add_row(m_keyboard_grid, "I", "Eyedropper tool", r);
  r = add_row(m_keyboard_grid, "K", "Corner tool", r);
  r = add_row(m_keyboard_grid, "G", "Polygon / Star tool", r);
  r = add_row(m_keyboard_grid, "W", "Spiral tool", r);
  r = add_row(m_keyboard_grid, "M", "Ruler / Measure tool", r);
  r = add_row(m_keyboard_grid, "Z", "Zoom tool", r);
  r = add_row(m_keyboard_grid, "Q", "Toggle snap", r);
  r = add_row(m_keyboard_grid, "↑ / ↓", "Cycle through toolbar tools (when nothing selected)", r);

  // ── View ──────────────────────────────────────────────────────────────
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "View", r);
  r = add_row(m_keyboard_grid, "Ctrl+E", "Toggle outline mode", r);
  r = add_row(m_keyboard_grid, "Ctrl+R", "Toggle rulers", r);
  r = add_row(m_keyboard_grid, "+  /  −", "Zoom in / out", r);
  r = add_row(m_keyboard_grid, "Ctrl+0", "Fit artboard to window", r);
  r = add_row(m_keyboard_grid, "Ctrl+1", "Zoom 1× (fit artboard)", r);
  r = add_row(m_keyboard_grid, "Ctrl+2", "Zoom 2× (double fit)", r);
  r = add_row(m_keyboard_grid, "Ctrl+3", "Zoom to selection", r);
  r = add_row(m_keyboard_grid, "Ctrl+Shift+0", "Fit all objects to window", r);

  // ── Warp Envelope Edit ────────────────────────────────────────────────
  // Active only when Selection tool + Warp object is primary. Keys
  // operate on the envelope pick set used for multi-anchor editing.
  r = add_spacer(m_keyboard_grid, r);
  r = add_heading(m_keyboard_grid, "Warp Envelope Edit  (Selection tool + Warp selected)", r);
  r = add_row(m_keyboard_grid, "T", "Select all top envelope anchors", r);
  r = add_row(m_keyboard_grid, "B", "Select all bottom envelope anchors", r);
  r = add_row(m_keyboard_grid, "L", "Select leftmost anchor pair (top + bottom)", r);
  r = add_row(m_keyboard_grid, "R", "Select rightmost anchor pair (top + bottom)", r);
  r = add_row(m_keyboard_grid, "C", "Select interior anchors on both envelopes", r);
  r = add_row(m_keyboard_grid, "A", "Select all anchors + visible handles", r);
  r = add_row(m_keyboard_grid, "Escape", "Clear envelope pick set", r);
  r = add_row(m_keyboard_grid, "↑↓←→", "Nudge picked envelope elements (1 / 8 / 32 px with Shift / Alt)", r);

  m_keyboard_scroll.set_child(m_keyboard_grid);
  m_keyboard_scroll.set_policy(Gtk::PolicyType::NEVER,
                               Gtk::PolicyType::AUTOMATIC);
  m_keyboard_scroll.set_expand(true);
  return m_keyboard_scroll;
}

// ── Mouse & Modifiers tab  (sections sorted A–Z)
// ─────────────────────────────────────────────────────
Gtk::Widget &ShortcutsDialog::build_mouse_tab() {
  m_mouse_grid.set_margin(12);
  m_mouse_grid.set_column_spacing(8);
  m_mouse_grid.set_row_spacing(0);
  m_mouse_grid.set_column_homogeneous(false);

  int r = 0;

  // ── Canvas — Always Available ─────────────────────────────────────────
  r = add_heading(m_mouse_grid, "Canvas — Always Available", r);
  r = add_row(m_mouse_grid, "Scroll", "Pan canvas", r);
  r = add_row(m_mouse_grid, "Ctrl+Scroll", "Zoom in / out", r);
  r = add_row(m_mouse_grid, "Middle-drag", "Pan canvas", r);
  r = add_row(m_mouse_grid, "Space+drag", "Pan canvas (any tool)", r);
  r = add_row(m_mouse_grid, "Pinch gesture", "Zoom in / out", r);

  // ── Corner Tool ───────────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Corner Tool  (K)", r);
  r = add_row(m_mouse_grid, "Click node", "Select corner/cusp node", r);
  r = add_row(m_mouse_grid, "Shift+click node", "Add / remove from selection", r);
  r = add_row(m_mouse_grid, "Drag", "Rubber-band select nodes", r);
  r = add_row(m_mouse_grid, "Context Bar", "Set type + radius, then Apply", r);

  // ── Defaults Well  (toolbar fill/stroke swatch) ───────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Defaults Well  (toolbar fill/stroke swatch)", r);
  r = add_row(m_mouse_grid, "Click top-left", "Open fill popover", r);
  r = add_row(m_mouse_grid, "Click bottom-right", "Open stroke popover", r);
  r = add_row(m_mouse_grid, "Ctrl+click", "Reset fill + stroke to defaults", r);

  // ── Ellipse Tool ──────────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Ellipse Tool  (E)", r);
  r = add_row(m_mouse_grid, "Drag", "Draw ellipse", r);
  r = add_row(m_mouse_grid, "Shift+drag", "Constrain to circle", r);
  r = add_row(m_mouse_grid, "Alt+drag", "Draw from centre", r);
  r = add_row(m_mouse_grid, "Right-click toolbar", "Place precisely (popover)", r);

  // ── Eyedropper Tool ───────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Eyedropper Tool  (I)", r);
  r = add_row(m_mouse_grid, "Click", "Sample fill → apply to selection fill", r);
  r = add_row(m_mouse_grid, "Alt+click", "Sample stroke colour (instead of fill)", r);
  r = add_row(m_mouse_grid, "Shift+click", "Apply sampled colour to stroke (instead of fill)", r);
  r = add_row(m_mouse_grid, "Shift+Alt+click", "Sample stroke → apply to stroke", r);

  // ── Guides & Rulers ───────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Guides & Rulers", r);
  r = add_row(m_mouse_grid, "Drag from ruler", "Create guide", r);
  r = add_row(m_mouse_grid, "Drag guide", "Move guide", r);
  r = add_row(m_mouse_grid, "Right-click ruler", "Change display units (popover)", r);
  r = add_row(m_mouse_grid, "Alt+click ruler corner", "Set ruler origin (dialog)", r);
  r = add_row(m_mouse_grid, "Drag ruler corner", "Set ruler origin (approximate)", r);
  r = add_row(m_mouse_grid, "Double-click corner", "Reset ruler origin to 0,0", r);
  r = add_row(m_mouse_grid, "Del", "Delete selected guide", r);

  // ── Line Tool ─────────────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Line Tool  (L)", r);
  r = add_row(m_mouse_grid, "Click+drag", "Draw line segment", r);
  r = add_row(m_mouse_grid, "Shift+drag", "Snap to 45° angles", r);
  r = add_row(m_mouse_grid, "Right-click toolbar", "Place precisely (popover)", r);

  // ── Node Tool ─────────────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Node Tool  (N)", r);
  r = add_row(m_mouse_grid, "Click node", "Select node", r);
  r = add_row(m_mouse_grid, "Shift+click node", "Add / remove node from selection", r);
  r = add_row(m_mouse_grid, "Shift+click path", "Add entire path to node selection", r);
  r = add_row(m_mouse_grid, "Drag handle", "Adjust curve", r);
  r = add_row(m_mouse_grid, "Alt+drag handle", "Break handle symmetry / smooth", r);

  // ── Pen Tool ──────────────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Pen Tool  (P)", r);
  r = add_row(m_mouse_grid, "Click", "Place anchor (sharp)", r);
  r = add_row(m_mouse_grid, "Click+drag", "Place anchor with handles", r);
  r = add_row(m_mouse_grid, "Alt+drag handle", "Break handle symmetry", r);
  r = add_row(m_mouse_grid, "Click first node", "Close path", r);
  r = add_row(m_mouse_grid, "Click open endpoint", "Continue existing path", r);

  // ── Polygon / Star Tool ───────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Polygon / Star Tool  (G)", r);
  r = add_row(m_mouse_grid, "Drag", "Draw polygon/star (center = drag start)", r);
  r = add_row(m_mouse_grid, "Alt+drag", "Draw from center", r);
  r = add_row(m_mouse_grid, "Shift+drag", "Snap rotation to 15°", r);
  r = add_row(m_mouse_grid, "Right-click toolbar", "Configure sides & inflection (popover)", r);

  // ── Rectangle Tool ────────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Rectangle Tool  (R)", r);
  r = add_row(m_mouse_grid, "Drag", "Draw rectangle", r);
  r = add_row(m_mouse_grid, "Shift+drag", "Constrain to square", r);
  r = add_row(m_mouse_grid, "Alt+drag", "Draw from centre", r);
  r = add_row(m_mouse_grid, "Right-click toolbar", "Place precisely (popover)", r);

  // ── Reference Point Tool ──────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Reference Point Tool  (F)", r);
  r = add_row(m_mouse_grid, "Click", "Place named reference point", r);
  r = add_row(m_mouse_grid, "Right-click toolbar", "Place precisely (popover)", r);
  r = add_row(m_mouse_grid, "Properties panel", "Position precisely via X / Y", r);

  // ── Ruler / Measure Tool ──────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Ruler / Measure Tool  (M)", r);
  r = add_row(m_mouse_grid, "Click node", "Select first point", r);
  r = add_row(m_mouse_grid, "Shift+click node", "Add second point", r);
  r = add_row(m_mouse_grid, "Drag", "Marquee-select two nodes", r);
  r = add_row(m_mouse_grid, "Space", "Clear and start new measurement", r);
  r = add_row(m_mouse_grid, "Enter", "Place measurement on layer", r);
  r = add_row(m_mouse_grid, "Click label", "Copy value", r);
  r = add_row(m_mouse_grid, "Alt+click label", "Copy all values", r);

  // ── Selection Tool ────────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Selection Tool  (S)", r);
  r = add_row(m_mouse_grid, "Click", "Select object", r);
  r = add_row(m_mouse_grid, "Click selected object", "Cycle to object underneath", r);
  r = add_row(m_mouse_grid, "Alt+drag", "Duplicate and move (original stays)", r);
  r = add_row(m_mouse_grid, "Shift+click", "Add / remove from selection", r);
  r = add_row(m_mouse_grid, "Ctrl+Alt+click", "Toggle align anchor (key object)", r);
  r = add_row(m_mouse_grid, "Drag (empty area)", "Rubber-band select", r);
  r = add_row(m_mouse_grid, "Drag handle corner", "Scale object", r);
  r = add_row(m_mouse_grid, "Drag handle edge", "Scale or skew object", r);
  r = add_row(m_mouse_grid, "Drag rotate corner", "Rotate object", r);
  r = add_row(m_mouse_grid, "Shift+drag rotate", "Snap rotation to 15°", r);
  r = add_row(m_mouse_grid, "Alt+drag skew", "Skew from centre (symmetric)", r);
  r = add_row(m_mouse_grid, "↑↓←→", "Nudge selection (2px)", r);
  r = add_row(m_mouse_grid, "Shift+↑↓←→", "Nudge selection (8px)", r);
  r = add_row(m_mouse_grid, "Alt+↑↓←→", "Nudge selection (32px)", r);
  r = add_row(m_mouse_grid, "R  (with selection)", "Toggle custom pivot placement mode", r);

  // ── Snap ──────────────────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Snap  (toolbar switch)", r);
  r = add_row(m_mouse_grid, "Click switch", "Toggle snap on/off", r);
  r = add_row(m_mouse_grid, "Right-click switch", "Open snap-targets popover", r);

  // ── Spiral Tool ───────────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Spiral Tool  (W)", r);
  r = add_row(m_mouse_grid, "Drag", "Draw spiral (center = drag start, radius = drag distance)", r);
  r = add_row(m_mouse_grid, "Shift+drag", "Snap rotation to 15°", r);
  r = add_row(m_mouse_grid, "Right-click toolbar", "Open spiral configuration popover", r);

  // ── Text Tool ─────────────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Text Tool  (T)", r);
  r = add_row(m_mouse_grid, "Click", "Place text", r);
  r = add_row(m_mouse_grid, "Drag", "Place text box", r);
  r = add_row(m_mouse_grid, "Right-click toolbar", "Text options (popover)", r);

  // ── Text-on-Path Tool ─────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Text-on-Path Tool  (U)", r);
  r = add_row(m_mouse_grid, "Click text node", "Select text", r);
  r = add_row(m_mouse_grid, "Click path", "Link text to path", r);
  r = add_row(m_mouse_grid, "Drag text node", "Adjust offset along path", r);
  r = add_row(m_mouse_grid, "Right-click text node", "Detach text from path", r);

  // ── Zoom Tool ─────────────────────────────────────────────────────────
  r = add_spacer(m_mouse_grid, r);
  r = add_heading(m_mouse_grid, "Zoom Tool  (Z)", r);
  r = add_row(m_mouse_grid, "Click", "Zoom in 2×", r);
  r = add_row(m_mouse_grid, "Alt+click", "Zoom out 2×", r);
  r = add_row(m_mouse_grid, "Ctrl+click", "Fit to window", r);
  r = add_row(m_mouse_grid, "Right-click toolbar", "Set zoom level (popover)", r);
  r = add_row(m_mouse_grid, "Drag", "Marquee zoom in", r);

  m_mouse_scroll.set_child(m_mouse_grid);
  m_mouse_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  m_mouse_scroll.set_expand(true);
  return m_mouse_scroll;
}

} // namespace Curvz
