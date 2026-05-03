#pragma once
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/notebook.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/window.h>

namespace Curvz {

// Non-modal keyboard shortcuts reference window.
// Shows all shortcuts and mouse gestures grouped by category.
// Open with show(parent); stays open until user closes it.
// ? or Ctrl+/ to open from MainWindow.

class ShortcutsDialog : public Gtk::Window {
public:
  ShortcutsDialog();
  void show(Gtk::Window &parent);

private:
  // ── Tab builders ──────────────────────────────────────────────────────
  Gtk::Widget &build_keyboard_tab();
  Gtk::Widget &build_mouse_tab();

  // ── Helpers ───────────────────────────────────────────────────────────
  // Add a section heading to a grid at the given row
  int add_heading(Gtk::Grid &grid, const std::string &title, int row);
  // Add a key + description row; returns next row index
  int add_row(Gtk::Grid &grid, const std::string &keys, const std::string &desc,
              int row);
  // Add a blank spacer row
  int add_spacer(Gtk::Grid &grid, int row);

  // ── Widgets ───────────────────────────────────────────────────────────
  Gtk::Box m_root{Gtk::Orientation::VERTICAL};
  Gtk::Notebook m_notebook;
  Gtk::Button m_btn_close{"Close"};

  // Tab content boxes (owned by notebook)
  Gtk::ScrolledWindow m_keyboard_scroll;
  Gtk::ScrolledWindow m_mouse_scroll;
  Gtk::Grid m_keyboard_grid;
  Gtk::Grid m_mouse_grid;
};

} // namespace Curvz
