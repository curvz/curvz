#include "PrintManager.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"
#include "math/BezierPath.hpp"
#include <gtkmm/printoperation.h>
#include <gtk/gtkunixprint.h>
#include <cairomm/cairomm.h>
#include <gdkmm/pixbuf.h>
#include <gdk/gdk.h>
#include <filesystem>
#include <memory>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <string>
#include <vector>

namespace Curvz {
namespace fs = std::filesystem;

// ── Construction ──────────────────────────────────────────────────────────────

PrintManager::PrintManager() {
    curvz::utils::set_name(*this, "win_pr", "print_manager_window_root");
    set_title("Print");
    set_modal(true);
    set_resizable(true);
    set_default_size(720, 500);
    set_hide_on_close(true);

    m_page_setup    = Gtk::PageSetup::create();
    m_print_settings = Gtk::PrintSettings::create();

    build_ui();
    set_child(m_root);
}

// ── UI ────────────────────────────────────────────────────────────────────────

void PrintManager::build_ui() {
    m_root.set_spacing(0);

    // ── Top: content area (doc list + options) ────────────────────────────
    m_content.set_spacing(0);
    m_content.set_hexpand(true);
    m_content.set_vexpand(true);

    // Left: doc list
    auto *list_frame = Gtk::make_managed<Gtk::Frame>("Documents");
    list_frame->set_margin_start(10);
    list_frame->set_margin_top(10);
    list_frame->set_margin_bottom(10);
    list_frame->set_size_request(220, -1);

    m_doc_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_doc_scroll.set_vexpand(true);
    m_doc_box.set_spacing(2);
    m_doc_box.set_margin_top(6);
    m_doc_box.set_margin_bottom(6);
    m_doc_box.set_margin_start(8);
    m_doc_box.set_margin_end(8);
    m_doc_scroll.set_child(m_doc_box);
    list_frame->set_child(m_doc_scroll);
    m_content.append(*list_frame);

    // Right: style selector + options
    auto *right = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    right->set_spacing(8);
    right->set_margin_start(10);
    right->set_margin_top(10);
    right->set_margin_end(10);
    right->set_margin_bottom(10);
    right->set_hexpand(true);

    // Style toggle row
    m_style_bar.set_spacing(4);
    auto *style_lbl = Gtk::make_managed<Gtk::Label>("Style:");
    style_lbl->set_xalign(0.0f);
    m_style_bar.append(*style_lbl);
    m_sheet_btn.set_active(true);
    m_sheet_btn.add_css_class("tb-type-btn");
    m_plot_btn.add_css_class("tb-type-btn");
    m_normal_btn.add_css_class("tb-type-btn");
    curvz::utils::set_name(m_sheet_btn, "win_pr_sht", "print_manager_window_sheet_toggle");
    curvz::utils::set_name(m_plot_btn, "win_pr_plt", "print_manager_window_plot_toggle");
    curvz::utils::set_name(m_normal_btn, "win_pr_nrm", "print_manager_window_normal_toggle");
    // All three buttons share the same group so they act as a radio set.
    m_sheet_btn.set_group(m_plot_btn);
    m_normal_btn.set_group(m_plot_btn);
    m_sheet_btn.signal_toggled().connect([this]() {
        if (m_sheet_btn.get_active()) {
            m_style = Style::Sheet;
            update_style_panel();
        }
    });
    m_plot_btn.signal_toggled().connect([this]() {
        if (m_plot_btn.get_active()) {
            m_style = Style::Plot;
            update_style_panel();
        }
    });
    m_normal_btn.signal_toggled().connect([this]() {
        if (m_normal_btn.get_active()) {
            m_style = Style::Normal;
            update_style_panel();
        }
    });
    m_style_bar.append(m_sheet_btn);
    m_style_bar.append(m_plot_btn);
    m_style_bar.append(m_normal_btn);
    right->append(m_style_bar);

    // Options frame
    m_options_frame.set_label("Options");
    m_options_frame.set_vexpand(true);
    m_options_frame.set_child(m_options_box);
    m_options_box.set_spacing(6);
    m_options_box.set_margin_top(8);
    m_options_box.set_margin_bottom(8);
    m_options_box.set_margin_start(8);
    m_options_box.set_margin_end(8);
    right->append(m_options_frame);
    m_content.append(*right);

    m_root.append(m_content);

    // ── Separator ─────────────────────────────────────────────────────────
    auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    m_root.append(*sep);

    // ── Bottom bar ────────────────────────────────────────────────────────
    m_bottom_bar.set_spacing(8);
    m_bottom_bar.set_margin_top(8);
    m_bottom_bar.set_margin_bottom(8);
    m_bottom_bar.set_margin_start(10);
    m_bottom_bar.set_margin_end(10);

    auto *page_btn = Gtk::make_managed<Gtk::Button>("Page Setup…");
    curvz::utils::set_name(page_btn, "win_pr_pg", "print_manager_window_page_setup_btn");
    page_btn->signal_clicked().connect([this]() {
        // gtk_print_run_page_setup_dialog is the GTK4 way — modal, returns new setup
        auto* new_setup = gtk_print_run_page_setup_dialog(
            m_parent ? GTK_WINDOW(m_parent->gobj()) : nullptr,
            m_page_setup->gobj(),
            m_print_settings->gobj());
        if (new_setup)
            m_page_setup = Glib::wrap(new_setup);
    });
    m_bottom_bar.append(*page_btn);

    auto *spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    m_bottom_bar.append(*spacer);

    auto *cancel_btn = Gtk::make_managed<Gtk::Button>("Cancel");
    curvz::utils::set_name(cancel_btn, "win_pr_cnc", "print_manager_window_cancel_btn");
    cancel_btn->signal_clicked().connect([this]() { hide(); });
    m_bottom_bar.append(*cancel_btn);

    auto *print_btn = Gtk::make_managed<Gtk::Button>("Print");
    curvz::utils::set_name(print_btn, "win_pr_prn", "print_manager_window_print_btn");
    print_btn->add_css_class("suggested-action");
    print_btn->signal_clicked().connect([this]() { run_print(); });
    m_bottom_bar.append(*print_btn);

    m_root.append(m_bottom_bar);

    update_style_panel();
}

void PrintManager::build_doc_list() {
    // Clear existing
    while (auto *child = m_doc_box.get_first_child())
        m_doc_box.remove(*child);
    m_doc_checks.clear();

    if (!m_project) return;

    // Select all / none row
    auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    hdr->set_spacing(4);
    auto *all_btn = Gtk::make_managed<Gtk::Button>("All");
    auto *none_btn = Gtk::make_managed<Gtk::Button>("None");
    curvz::utils::set_name(all_btn, "win_pr_all", "print_manager_window_all_btn");
    curvz::utils::set_name(none_btn, "win_pr_non", "print_manager_window_none_btn");
    all_btn->add_css_class("flat");
    none_btn->add_css_class("flat");
    all_btn->signal_clicked().connect([this]() {
        for (size_t i = 0; i < m_doc_checks.size(); ++i) {
            m_doc_checks[i]->set_active(true);
            m_selected[i] = true;
        }
    });
    none_btn->signal_clicked().connect([this]() {
        for (size_t i = 0; i < m_doc_checks.size(); ++i) {
            m_doc_checks[i]->set_active(false);
            m_selected[i] = false;
        }
    });
    hdr->append(*all_btn);
    hdr->append(*none_btn);
    m_doc_box.append(*hdr);
    m_doc_box.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

    m_selected.resize(m_project->documents.size(), true);

    for (size_t i = 0; i < m_project->documents.size(); ++i) {
        const auto& doc = m_project->documents[i];
        std::string name = doc->filename.empty()
            ? "untitled"
            : fs::path(doc->filename).stem().string();

        // Node count
        int nodes = 0;
        for (const auto& layer : doc->layers)
            for (const auto& obj : layer->children)
                if (obj->path) nodes += (int)obj->path->nodes.size();

        char info[64];
        snprintf(info, sizeof(info), "%d nodes", nodes);

        auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        row->set_margin_bottom(4);

        auto *check = Gtk::make_managed<Gtk::CheckButton>(name);
        check->set_active(m_selected[i]);
        check->signal_toggled().connect([this, i, check]() {
            m_selected[i] = check->get_active();
        });
        row->append(*check);

        auto *info_lbl = Gtk::make_managed<Gtk::Label>(info);
        info_lbl->add_css_class("dim-label");
        info_lbl->set_xalign(0.0f);
        info_lbl->set_margin_start(22);
        row->append(*info_lbl);

        m_doc_checks.push_back(check);
        m_doc_box.append(*row);
    }
}

void PrintManager::update_style_panel() {
    // Remove all children
    while (auto *child = m_options_box.get_first_child())
        m_options_box.remove(*child);

    auto add_check = [&](const char* lbl, bool& state) {
        auto *cb = Gtk::make_managed<Gtk::CheckButton>(lbl);
        cb->set_active(state);
        cb->signal_toggled().connect([&state, cb]() { state = cb->get_active(); });
        m_options_box.append(*cb);
    };

    auto add_spin = [&](const char* lbl, int& val, int lo, int hi) {
        auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        row->set_spacing(8);
        auto *l = Gtk::make_managed<Gtk::Label>(lbl);
        l->set_xalign(0.0f);
        l->set_hexpand(true);
        auto adj = Gtk::Adjustment::create(val, lo, hi, 1, 10);
        auto *spin = Gtk::make_managed<Gtk::SpinButton>(adj, 1, 0);
        spin->set_width_chars(4);
        adj->signal_value_changed().connect([&val, adj]() {
            val = (int)adj->get_value();
        });
        row->append(*l);
        row->append(*spin);
        m_options_box.append(*row);
    };

    auto add_section_label = [&](const char* lbl) {
        auto *l = Gtk::make_managed<Gtk::Label>(lbl);
        l->set_xalign(0.0f);
        l->set_margin_top(4);
        auto ctx = l->get_style_context();
        (void)ctx;
        // Use pango markup for a subtle section header.
        l->set_markup(std::string("<small><b>") + lbl + "</b></small>");
        m_options_box.append(*l);
    };

    if (m_style == Style::Sheet) {
        add_spin("Icons per row",  m_icons_per_row, 1, 12);
        add_spin("Cell size (px)", m_cell_px,       32, 256);
        auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep->set_margin_top(4); sep->set_margin_bottom(4);
        m_options_box.append(*sep);
        add_check("Show name",      m_show_name);
        add_check("Show path",      m_show_path);
        add_check("Show node count",m_show_nodes);
        add_check("Show file size", m_show_filesize);
    } else if (m_style == Style::Plot) {
        add_check("Show handles",     m_plot_handles);
        add_check("Show coordinates", m_plot_coords);
        add_check("Show node indices",m_plot_indices);
    } else {
        // Normal — "show off" mode. Clean gallery print by default; users
        // opt in to header metadata and/or footer palette strip.
        add_section_label("Header");
        add_check("Filename",     m_normal_hdr_filename);
        add_check("Dimensions",   m_normal_hdr_dimensions);
        add_check("Node count",   m_normal_hdr_nodes);
        add_check("Layer count",  m_normal_hdr_layers);
        add_check("Date",         m_normal_hdr_date);
        auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep->set_margin_top(4); sep->set_margin_bottom(4);
        m_options_box.append(*sep);
        add_section_label("Footer");
        add_check("Show colour list",     m_normal_show_colors);
        add_check("Show artboard border", m_normal_show_border);
    }
}

// ── Public entry point ────────────────────────────────────────────────────────

void PrintManager::show(Gtk::Window& parent, CurvzProject& project) {
    m_parent  = &parent;
    m_project = &project;
    m_selected.assign(project.documents.size(), true);
    build_doc_list();
    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);  // s117 m18 v2
    set_visible(true);
    present();
}

// ── Print pipeline ────────────────────────────────────────────────────────────

void PrintManager::run_print() {
    // Collect selected docs
    std::vector<CurvzDocument*> docs;
    for (size_t i = 0; i < m_project->documents.size(); ++i)
        if (i < m_selected.size() && m_selected[i])
            docs.push_back(m_project->documents[i].get());

    if (docs.empty()) return;

    auto op = Gtk::PrintOperation::create();
    op->set_print_settings(m_print_settings);
    op->set_default_page_setup(m_page_setup);
    op->set_unit(Gtk::Unit::POINTS);

    if (m_style == Style::Sheet) {
        int per_row = m_icons_per_row;

        auto per_page_ref = std::make_shared<int>(per_row);

        op->signal_begin_print().connect(
            [&, per_row, per_page_ref](const Glib::RefPtr<Gtk::PrintContext>& ctx) {
                double pw      = ctx->get_width();
                double ph      = ctx->get_height();
                double margin  = 36.0;
                double cell    = m_cell_px * 0.75;
                double label_h = m_show_name ? 14.0 : 0.0;
                double cell_h  = cell + label_h + 8.0;
                int rows_per_page = std::max(1, (int)((ph - 2*margin) / cell_h));
                *per_page_ref  = per_row * rows_per_page;
                int n_pages    = std::max(1, ((int)docs.size() + *per_page_ref - 1) / *per_page_ref);
                op->set_n_pages(n_pages);
            });

        op->signal_draw_page().connect(
            [&, docs, per_row, per_page_ref](const Glib::RefPtr<Gtk::PrintContext>& ctx,
                                              int page_nr) {
                double pw      = ctx->get_width();
                double margin  = 36.0;
                double cell    = m_cell_px * 0.75;
                double label_h = m_show_name ? 14.0 : 0.0;
                double cell_h  = cell + label_h + 8.0;
                double cell_w  = (pw - 2*margin) / per_row;
                int    per_page = *per_page_ref;
                auto   cr      = ctx->get_cairo_context();

                int start = page_nr * per_page;
                int end   = std::min(start + per_page, (int)docs.size());

                cr->set_source_rgb(0, 0, 0);
                cr->select_font_face("sans", Cairo::ToyFontFace::Slant::NORMAL,
                                     Cairo::ToyFontFace::Weight::NORMAL);

                for (int di = start; di < end; ++di) {
                    int local = di - start;
                    int col   = local % per_row;
                    int row   = local / per_row;
                    double cx = margin + col * cell_w + (cell_w - cell) * 0.5;
                    double cy = margin + row * cell_h;
                    render_icon_cell(cr, *docs[di], cx, cy, cell);

                    if (m_show_name) {
                        std::string name = docs[di]->filename.empty()
                            ? "untitled"
                            : fs::path(docs[di]->filename).stem().string();
                        cr->set_source_rgb(0, 0, 0);
                        cr->set_font_size(9.0);
                        Cairo::TextExtents te;
                        cr->get_text_extents(name, te);
                        cr->move_to(cx + (cell - te.width) * 0.5, cy + cell + 11.0);
                        cr->show_text(name);
                    }

                    std::string info_parts;
                    if (m_show_nodes) {
                        int n = 0;
                        for (const auto& layer : docs[di]->layers)
                            for (const auto& obj : layer->children)
                                if (obj->path) n += (int)obj->path->nodes.size();
                        char buf[32]; snprintf(buf, sizeof(buf), "%d nodes", n);
                        info_parts += buf;
                    }
                    if (m_show_filesize && !docs[di]->filename.empty()
                            && !m_project->directory.empty()) {
                        fs::path p = fs::path(m_project->directory) / docs[di]->filename;
                        try {
                            char buf[32];
                            uintmax_t sz = fs::file_size(p);
                            if (sz < 1024) snprintf(buf, sizeof(buf), "%dB", (int)sz);
                            else           snprintf(buf, sizeof(buf), "%.1fKB", sz/1024.0);
                            if (!info_parts.empty()) info_parts += "  ";
                            info_parts += buf;
                        } catch (...) {}
                    }
                    if (!info_parts.empty()) {
                        cr->set_source_rgb(0, 0, 0);
                        cr->set_font_size(7.0);
                        cr->move_to(cx, cy + cell + (m_show_name ? 22.0 : 11.0));
                        cr->show_text(info_parts);
                    }
                }
            });

    } else if (m_style == Style::Plot) {
        // Plot mode — one doc per page
        op->signal_begin_print().connect(
            [&, docs](const Glib::RefPtr<Gtk::PrintContext>&) {
                op->set_n_pages((int)docs.size());
            });
        op->signal_draw_page().connect(
            [&, docs](const Glib::RefPtr<Gtk::PrintContext>& ctx, int page_nr) {
                auto cr = ctx->get_cairo_context();
                render_plot(cr, *docs[page_nr], ctx->get_width(), ctx->get_height());
            });
    } else {
        // Normal mode — one doc per page, rendered as the canvas shows it
        op->signal_begin_print().connect(
            [&, docs](const Glib::RefPtr<Gtk::PrintContext>&) {
                op->set_n_pages((int)docs.size());
            });
        op->signal_draw_page().connect(
            [&, docs](const Glib::RefPtr<Gtk::PrintContext>& ctx, int page_nr) {
                auto cr = ctx->get_cairo_context();
                render_normal(cr, *docs[page_nr], ctx->get_width(), ctx->get_height());
            });
    }

    try {
        auto result = op->run(Gtk::PrintOperation::Action::PRINT_DIALOG, *m_parent);
        if (result == Gtk::PrintOperation::Result::APPLY)
            m_print_settings = op->get_print_settings();
        // Only close on success or error — not on cancel
        if (result != Gtk::PrintOperation::Result::CANCEL)
            hide();
    } catch (const Gtk::PrintError& e) {
        LOG_ERROR("PrintManager: {}", e.what());
    }
}

// ── Render helpers ────────────────────────────────────────────────────────────

// Forward decls — render_icon_cell (Sheet) calls these; their definitions
// live further down with the rest of the Normal-mode renderer that Sheet
// now shares. Plain forward-decls keep the file's top-down call order
// readable without reshuffling.
static Cairo::RefPtr<Cairo::SurfacePattern> make_current_color_pattern();
static void render_normal_node(const Cairo::RefPtr<Cairo::Context>& cr,
                                const SceneNode& obj,
                                const Cairo::RefPtr<Cairo::Pattern>& ccpat,
                                bool cc_as_black = false);

// Recursively collect all path nodes for plot view (outline)
static void collect_plot_paths(const SceneNode& obj,
                                std::vector<const SceneNode*>& out) {
    if (!obj.visible) return;
    if (obj.type == SceneNode::Type::Path && obj.path) {
        out.push_back(&obj);
    } else if (obj.type == SceneNode::Type::Compound ||
               obj.type == SceneNode::Type::Group) {
        for (const auto& child : obj.children)
            collect_plot_paths(*child, out);
    }
}

void PrintManager::render_icon_cell(const Cairo::RefPtr<Cairo::Context>& cr,
                                     const CurvzDocument& doc,
                                     double x, double y, double side) {
    double cw = doc.canvas_width();
    double ch = doc.canvas_height();
    if (cw <= 0 || ch <= 0) return;

    double scale = std::min(side / cw, side / ch);
    double tx = x + (side - cw * scale) * 0.5;
    double ty = y + (side - ch * scale) * 0.5;

    cr->save();
    cr->translate(tx, ty);
    cr->scale(scale, scale);
    cr->rectangle(0, 0, cw, ch);
    cr->clip();

    // Sheet thumbnails route through render_normal_node so colour fills,
    // gradients, drop shadows, opacity, and stroke cap/join all render
    // identically to canvas — modulo the currentColor handling. Sheet
    // wants a concrete colour for each cell (black) rather than the
    // checkerboard / dashed-grey "indicator" treatment that Normal mode
    // uses to flag variable-colour fills. Passing cc_as_black=true
    // does that swap.
    //
    // ccpat is unused by the Sheet path because cc_as_black short-circuits
    // every currentColor branch before the pattern is touched, but the
    // signature still requires one. Built once per cell — cost is one
    // 12×12 ARGB32 surface allocation, well below frame budgets.
    auto ccpat = make_current_color_pattern();

    // Layers[0] paints first (bottom of z-stack), layers[n-1] last (top).
    // Matches Canvas::draw_objects. Visibility / special-layer filtering
    // is done inside render_normal_node via normal_node_is_printable, so
    // we only filter the obvious non-art layer types here for early exit.
    for (int li = 0; li < (int)doc.layers.size(); ++li) {
        const SceneNode& layer = *doc.layers[li];
        if (layer.is_guide_layer() || layer.is_ref_layer() || !layer.visible) continue;
        for (int oi = (int)layer.children.size() - 1; oi >= 0; --oi)
            render_normal_node(cr, *layer.children[oi], ccpat,
                               /*cc_as_black=*/true);
    }
    cr->restore();

    // Cell border
    cr->set_source_rgb(0.85, 0.85, 0.85);
    cr->set_line_width(0.5);
    cr->rectangle(x, y, side, side);
    cr->stroke();
}

void PrintManager::render_plot(const Cairo::RefPtr<Cairo::Context>& cr,
                                const CurvzDocument& doc,
                                double page_w, double page_h) {
    const double margin  = 36.0;
    const double avail_w = page_w - 2.0 * margin;
    const double avail_h = page_h - 2.0 * margin - 40.0; // 40pt for title

    double cw = doc.canvas_width();
    double ch = doc.canvas_height();
    double scale = std::min(avail_w / cw, avail_h / ch);
    double ox = margin + (avail_w - cw * scale) * 0.5;
    double oy = margin + 40.0 + (avail_h - ch * scale) * 0.5;

    // Title block
    std::string name = doc.filename.empty()
        ? "untitled"
        : fs::path(doc.filename).stem().string();
    int total_nodes = 0;
    int total_paths = 0;
    for (const auto& layer : doc.layers)
        for (const auto& obj : layer->children)
            if (obj->path) { ++total_paths; total_nodes += (int)obj->path->nodes.size(); }

    cr->set_source_rgb(0, 0, 0);
    cr->select_font_face("sans", Cairo::ToyFontFace::Slant::NORMAL,
                         Cairo::ToyFontFace::Weight::BOLD);
    cr->set_font_size(14.0);
    cr->move_to(margin, margin + 14.0);
    cr->show_text(name);

    char sub[128];
    snprintf(sub, sizeof(sub), "%d paths · %d nodes · %d × %d px",
             total_paths, total_nodes, (int)cw, (int)ch);
    cr->select_font_face("sans", Cairo::ToyFontFace::Slant::NORMAL,
                         Cairo::ToyFontFace::Weight::NORMAL);
    cr->set_font_size(9.0);
    cr->move_to(margin, margin + 28.0);
    cr->show_text(sub);

    // Artboard border
    cr->set_source_rgb(0.8, 0.8, 0.8);
    cr->set_line_width(0.5);
    cr->rectangle(ox, oy, cw * scale, ch * scale);
    cr->stroke();

    // Draw paths in outline mode
    cr->save();
    cr->translate(ox, oy);
    cr->scale(scale, scale);

    // Layers[0] paints first (bottom of z-stack), layers[n-1] last (top).
    // Matches Canvas::draw_objects.
    for (int li = 0; li < (int)doc.layers.size(); ++li) {
        const SceneNode& layer = *doc.layers[li];
        if (layer.is_guide_layer() || layer.is_ref_layer() || !layer.visible) continue;
        for (int oi = (int)layer.children.size() - 1; oi >= 0; --oi) {
            // Collect all leaf paths recursively (handles compounds + groups)
            std::vector<const SceneNode*> paths;
            collect_plot_paths(*layer.children[oi], paths);

            for (const SceneNode* obj : paths) {
                BezierPath bp = BezierPath::from_path_data(*obj->path);
                bp.apply_to_cairo(cr);
                cr->set_source_rgb(0.1, 0.1, 0.1);
                cr->set_line_width(1.0 / scale);
                cr->stroke();

                if (m_plot_handles || m_plot_coords || m_plot_indices) {
                    for (int ni = 0; ni < (int)bp.nodes.size(); ++ni) {
                        const auto& nd = bp.nodes[ni];
                        double nx = nd.x, ny = nd.y;

                        if (m_plot_handles) {
                            cr->set_source_rgba(0.4, 0.4, 0.9, 0.8);
                            cr->set_line_width(0.5 / scale);
                            cr->move_to(nd.cx1, nd.cy1);
                            cr->line_to(nx, ny);
                            cr->stroke();
                            cr->move_to(nd.cx2, nd.cy2);
                            cr->line_to(nx, ny);
                            cr->stroke();
                            double hr = 2.0 / scale;
                            cr->arc(nd.cx1, nd.cy1, hr, 0, 2*M_PI); cr->fill();
                            cr->arc(nd.cx2, nd.cy2, hr, 0, 2*M_PI); cr->fill();
                        }

                        double nr = 3.0 / scale;
                        cr->set_source_rgb(0.1, 0.1, 0.1);
                        cr->rectangle(nx - nr, ny - nr, nr*2, nr*2);
                        cr->fill();

                        if (m_plot_coords) {
                            char buf[48];
                            snprintf(buf, sizeof(buf), "%.1f,%.1f", nx, ny);
                            cr->save();
                            cr->set_identity_matrix();
                            cr->set_source_rgb(0.2, 0.2, 0.6);
                            cr->set_font_size(6.0);
                            cr->move_to(nx * scale + ox + 4, ny * scale + oy - 2);
                            cr->show_text(buf);
                            cr->restore();
                        }

                        if (m_plot_indices) {
                            char buf[16];
                            snprintf(buf, sizeof(buf), "%d", ni);
                            cr->save();
                            cr->set_identity_matrix();
                            cr->set_source_rgb(0.7, 0.1, 0.1);
                            cr->set_font_size(6.0);
                            cr->move_to(nx * scale + ox + 4, ny * scale + oy + 8);
                            cr->show_text(buf);
                            cr->restore();
                        }
                    }
                }
            }
        }
    }
    cr->restore();
}

// ══════════════════════════════════════════════════════════════════════════════
// Normal-mode rendering
// ══════════════════════════════════════════════════════════════════════════════

// Does a fill/stroke use the currentColor keyword?
static bool is_current_color(const FillStyle& f) {
    return f.type == FillStyle::Type::CurrentColor;
}

// A colour entry collected from a document's visible objects.
// For currentColor we flag is_current and ignore r/g/b.
struct NormalColorEntry {
    bool   is_current = false;
    double r = 0.0, g = 0.0, b = 0.0;
};

// Two solid-colour entries are equal at DECIMAL_PREC ≈ 2 decimals (0.01 in
// normalized 0..1 RGB) — printer colour precision well below this anyway.
static bool colors_equal(const NormalColorEntry& a, const NormalColorEntry& b) {
    if (a.is_current != b.is_current) return false;
    if (a.is_current) return true; // one canonical currentColor
    const double tol = 0.01;
    return std::abs(a.r - b.r) < tol
        && std::abs(a.g - b.g) < tol
        && std::abs(a.b - b.b) < tol;
}

// Build a 6pt-square light-grey + white checkerboard Cairo pattern for
// currentColor fills. Colours chosen to read as "decorative/variable" on
// white paper without being visually loud.
static Cairo::RefPtr<Cairo::SurfacePattern> make_current_color_pattern() {
    // 12pt tile = two 6pt squares on each axis, repeating.
    constexpr int TILE = 12;
    auto surf = Cairo::ImageSurface::create(
        Cairo::Surface::Format::ARGB32, TILE, TILE);
    auto cr = Cairo::Context::create(surf);

    // Background: white.
    cr->set_source_rgb(1.0, 1.0, 1.0);
    cr->paint();
    // Top-left and bottom-right squares: light grey.
    cr->set_source_rgb(0.80, 0.80, 0.80);
    cr->rectangle(0, 0, TILE / 2, TILE / 2);
    cr->fill();
    cr->rectangle(TILE / 2, TILE / 2, TILE / 2, TILE / 2);
    cr->fill();

    auto pat = Cairo::SurfacePattern::create(surf);
    pat->set_extend(Cairo::Pattern::Extend::REPEAT);
    pat->set_filter(Cairo::SurfacePattern::Filter::NEAREST);
    return pat;
}

// Should this node be rendered on a Normal page?
// Tree walk rule: skip editor-chrome layer types entirely; skip any node with
// visible==false. Layer visibility propagates transitively (an invisible
// Layer => none of its descendants print).
static bool normal_node_is_printable(const SceneNode& n) {
    if (!n.visible) return false;
    if (n.is_special_layer()) return false; // guide/ref/measure/grid/margin
    return true;
}

// Walk a subtree, collecting unique colours used on visible paths/compounds.
// Order = first-appearance. currentColor collapses to a single entry.
static void collect_normal_colors(const SceneNode& n,
                                   std::vector<NormalColorEntry>& out)
{
    if (!normal_node_is_printable(n)) return;

    auto add_from = [&](const FillStyle& f) {
        if (f.type == FillStyle::Type::None) return;
        NormalColorEntry e;
        if (f.type == FillStyle::Type::CurrentColor) {
            e.is_current = true;
        } else {
            e.r = f.r; e.g = f.g; e.b = f.b;
        }
        for (const auto& existing : out)
            if (colors_equal(existing, e)) return;
        out.push_back(e);
    };

    if (n.type == SceneNode::Type::Path && n.path) {
        add_from(n.fill);
        add_from(n.stroke.paint);
    } else if (n.type == SceneNode::Type::Compound) {
        // Compound is one visual object — read style only from the Compound
        // itself, never from children (matches render rule).
        add_from(n.fill);
        add_from(n.stroke.paint);
    }
    // Recurse through ordinary Layer / Group children.
    // Compound children are deliberately NOT recursed into — their styles
    // are inert by rule.
    if (n.type != SceneNode::Type::Compound) {
        for (const auto& child : n.children) {
            if (child) collect_normal_colors(*child, out);
        }
    }
}

// Count visible path nodes across a document's visible tree.
static int count_normal_nodes(const SceneNode& n) {
    if (!normal_node_is_printable(n)) return 0;
    int total = 0;
    if (n.path) total += (int)n.path->nodes.size();
    for (const auto& ch : n.children)
        if (ch) total += count_normal_nodes(*ch);
    return total;
}

// Count visible *ordinary* layers (Layer type, not RefLayer/GuideLayer/etc.).
static int count_normal_layers(const CurvzDocument& doc) {
    int n = 0;
    for (const auto& L : doc.layers) {
        if (!L) continue;
        if (L->type == SceneNode::Type::Layer && L->visible) ++n;
    }
    return n;
}

// Apply a paint to cr. Returns true if the paint is currentColor (so the
// caller knows to use fill_preserve + pattern / dashed stroke treatment).
// For Solid, sets the RGB source directly and returns false. For
// LinearGradient / RadialGradient, builds a Cairo gradient pattern from
// the stops + bbox and sets it as the source; returns false.
//
// For the checkerboard pattern we override the pattern matrix so the squares
// always appear at the tile's native pixel size in *paper* space, regardless
// of the current CTM scale the artwork is being drawn into. Without this the
// squares would scale with the artwork and become tiny/huge depending on how
// small the artboard is relative to the page.
//
// bbox_* parameters describe the shape's geometry bbox in current user-space.
// Used only by gradient types (objectBoundingBox-fraction → doc-space lerp).
// For Solid / CurrentColor / None, the bbox is ignored and may be passed as 0s.
//
// cc_as_black: when true, currentColor renders as solid black instead of the
// checkerboard pattern. Used by Sheet thumbnails — the checkerboard is too
// fussy at thumbnail size and Sheet's whole point is "what does this icon
// look like at small sizes," for which black is a reasonable concrete pick.
static bool apply_normal_fill_paint(const Cairo::RefPtr<Cairo::Context>& cr,
                                     const FillStyle& f,
                                     const Cairo::RefPtr<Cairo::Pattern>& ccpat,
                                     double bbox_x = 0.0, double bbox_y = 0.0,
                                     double bbox_w = 0.0, double bbox_h = 0.0,
                                     bool cc_as_black = false)
{
    if (f.type == FillStyle::Type::CurrentColor) {
        if (cc_as_black) {
            cr->set_source_rgb(0.0, 0.0, 0.0);
            return false;
        }
        // The pattern matrix maps user-space → pattern-space. Scaling the
        // pattern matrix by the current CTM scale cancels out the artwork
        // scale, yielding a tile that reads as fixed pixels on paper.
        Cairo::Matrix ctm = cr->get_matrix();
        double sx = std::sqrt(ctm.xx * ctm.xx + ctm.yx * ctm.yx);
        if (sx < 1e-6) sx = 1.0;
        Cairo::Matrix pm = Cairo::identity_matrix();
        pm.scale(sx, sx);
        ccpat->set_matrix(pm);
        cr->set_source(ccpat);
        return true;
    }
    if (f.type == FillStyle::Type::LinearGradient ||
        f.type == FillStyle::Type::RadialGradient) {
        auto pat = curvz::utils::build_gradient_pattern(
            f, bbox_x, bbox_y, bbox_w, bbox_h);
        if (pat) {
            cr->set_source(pat);
        } else {
            // No stops or build failure — paint transparent so a subsequent
            // fill/stroke draws nothing rather than whatever was last set.
            cr->set_source_rgba(0, 0, 0, 0);
        }
        return false;
    }
    cr->set_source_rgb(f.r, f.g, f.b);
    return false;
}

// Draw a filled region (path already set on cr). Solid = fill;
// gradient = lerped Cairo gradient pattern; currentColor = checkerboard fill
// (or solid black when cc_as_black, used by Sheet thumbnails).
// bbox is the shape's geometry bbox in current user-space, required only for
// gradient fills; pass 0s for non-gradient.
static void apply_normal_fill(const Cairo::RefPtr<Cairo::Context>& cr,
                              const FillStyle& f,
                              const Cairo::RefPtr<Cairo::Pattern>& ccpat,
                              double bbox_x = 0.0, double bbox_y = 0.0,
                              double bbox_w = 0.0, double bbox_h = 0.0,
                              bool cc_as_black = false)
{
    if (f.type == FillStyle::Type::None) { cr->begin_new_path(); return; }
    apply_normal_fill_paint(cr, f, ccpat, bbox_x, bbox_y, bbox_w, bbox_h,
                            cc_as_black);
    cr->fill();
}

// Stroke current path according to a stroke style. Solid colour, gradient,
// or currentColor (renders as dashed mid-grey — palette legend interprets,
// or solid black when cc_as_black for Sheet thumbnails). Honours line cap
// and line join. bbox required for gradient strokes.
static void apply_normal_stroke(const Cairo::RefPtr<Cairo::Context>& cr,
                                 const StrokeStyle& s,
                                 const Cairo::RefPtr<Cairo::Pattern>& ccpat,
                                 double bbox_x = 0.0, double bbox_y = 0.0,
                                 double bbox_w = 0.0, double bbox_h = 0.0,
                                 bool cc_as_black = false)
{
    if (s.paint.type == FillStyle::Type::None) return;

    cr->set_line_width(s.width);
    switch (s.cap) {
        case LineCap::Butt:
            cr->set_line_cap(Cairo::Context::LineCap::BUTT);   break;
        case LineCap::Round:
            cr->set_line_cap(Cairo::Context::LineCap::ROUND);  break;
        case LineCap::Square:
            cr->set_line_cap(Cairo::Context::LineCap::SQUARE); break;
    }
    switch (s.join) {
        case LineJoin::Miter:
            cr->set_line_join(Cairo::Context::LineJoin::MITER); break;
        case LineJoin::Round:
            cr->set_line_join(Cairo::Context::LineJoin::ROUND); break;
        case LineJoin::Bevel:
            cr->set_line_join(Cairo::Context::LineJoin::BEVEL); break;
    }

    if (s.paint.type == FillStyle::Type::CurrentColor) {
        if (cc_as_black) {
            cr->set_source_rgb(0.0, 0.0, 0.0);
            cr->stroke();
        } else {
            cr->set_source_rgb(0.53, 0.53, 0.53);
            std::vector<double> dash = { 4.0, 2.0 };
            cr->set_dash(dash, 0.0);
            cr->stroke();
            std::vector<double> none;
            cr->set_dash(none, 0.0); // restore for next drawing op
        }
    } else if (s.paint.type == FillStyle::Type::LinearGradient ||
               s.paint.type == FillStyle::Type::RadialGradient) {
        auto pat = curvz::utils::build_gradient_pattern(
            s.paint, bbox_x, bbox_y, bbox_w, bbox_h);
        if (pat) cr->set_source(pat);
        else     cr->set_source_rgba(0, 0, 0, 0);
        cr->stroke();
    } else {
        cr->set_source_rgb(s.paint.r, s.paint.g, s.paint.b);
        cr->stroke();
    }
    // Caller (which may issue further fills/strokes via the same cr) is not
    // expected to inherit our cap/join — Cairo state persists, but the next
    // apply_normal_stroke call will set them again. Leaving them as-is.
}

// Recursive artwork render — handles visibility, currentColor, gradients,
// shadows, opacity, and Image nodes. Path / Compound / Group / Image all
// covered. Text currently not rendered (deferred — would require Pango setup
// to match Canvas behaviour).
//
// Path / Compound / Image branches are wrapped in a push_group / pop_group_to_
// source pair when the node has a drop shadow or per-object opacity. Mirrors
// Canvas::draw_object: shadow paints first (under the host), then host on
// top with paint_with_alpha if opacity < 1. SVG semantics: filter applied
// before opacity in the rendering pipeline.
//
// Gradient fills/strokes need a bbox in current user-space. Computed via
// cr->get_fill_extents() after the path is set on cr — Cairo gives us the
// tight geometry bbox that objectBoundingBox semantics require.
//
// cc_as_black: when true, currentColor renders as solid black instead of
// the checkerboard / dashed-grey treatment. Set by Sheet thumbnails (which
// want a concrete colour for the small preview); Normal mode passes false
// to keep the indicator look that flags "this is variable colour".
static void render_normal_node(const Cairo::RefPtr<Cairo::Context>& cr,
                                const SceneNode& obj,
                                const Cairo::RefPtr<Cairo::Pattern>& ccpat,
                                bool cc_as_black)
{
    if (!normal_node_is_printable(obj)) return;

    // ── Drop shadow + opacity wrap ──────────────────────────────────────
    // Wrap any node that draws its own pixels (Path / Compound / Image) so
    // shadow can be composited under the host and opacity can apply once
    // to the combined result. Group skips the wrap and recurses — each
    // child applies its own opacity, matching SVG group-multiplicative
    // semantics.
    const bool wants_alpha  = (obj.opacity < 0.999);
    const bool wants_shadow = obj.shadow_enabled
                              && (obj.shadow_color_a > 0.0)
                              && (obj.shadow_opacity > 0.0);
    const bool wants_wrap   = (wants_alpha || wants_shadow)
                              && obj.type != SceneNode::Type::Group;

    // Host bbox in current user-space — captured by the Path/Compound/Image
    // branch after geometry is set, then read by end_wrap if a shadow is
    // requested. Defaults are 0/0 sentinels; if a shadow is requested but
    // no bbox was captured (defensive), end_wrap falls back to the cr's
    // clip extents so the shadow still renders, just over a larger
    // offscreen surface.
    double host_bx = 0.0, host_by = 0.0, host_bw = 0.0, host_bh = 0.0;
    bool   host_bbox_set = false;
    auto set_host_bbox = [&](double x, double y, double w, double h) {
        host_bx = x; host_by = y; host_bw = w; host_bh = h;
        host_bbox_set = true;
    };

    auto begin_wrap = [&]() {
        if (wants_wrap) cr->push_group();
    };
    auto end_wrap = [&]() {
        if (!wants_wrap) return;
        cr->pop_group_to_source();
        Cairo::RefPtr<Cairo::Pattern> host_pat = cr->get_source();
        if (wants_shadow) {
            double bx = host_bx, by = host_by, bw = host_bw, bh = host_bh;
            if (!host_bbox_set) {
                // Fallback: use clip rect. Larger surface than needed but
                // visually identical.
                double cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                cr->get_clip_extents(cx1, cy1, cx2, cy2);
                bx = cx1; by = cy1;
                bw = cx2 - cx1; bh = cy2 - cy1;
            }
            curvz::utils::render_drop_shadow_under(
                cr, host_pat,
                bx, by, bw, bh,
                obj.shadow_blur, obj.shadow_dx, obj.shadow_dy,
                obj.shadow_color_r, obj.shadow_color_g, obj.shadow_color_b,
                obj.shadow_color_a, obj.shadow_opacity);
        }
        cr->set_source(host_pat);
        if (wants_alpha) cr->paint_with_alpha(obj.opacity);
        else             cr->paint();
    };

    if (obj.type == SceneNode::Type::Path && obj.path) {
        begin_wrap();
        BezierPath bp = BezierPath::from_path_data(*obj.path);

        // Apply path; compute bbox via Cairo's path-extents for gradient lerp.
        bp.apply_to_cairo(cr);
        double bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
        cr->get_fill_extents(bx1, by1, bx2, by2);
        const double bw = bx2 - bx1;
        const double bh = by2 - by1;
        set_host_bbox(bx1, by1, bw, bh);

        apply_normal_fill(cr, obj.fill, ccpat, bx1, by1, bw, bh, cc_as_black);
        // Stroke — rebuild path (fill consumed it).
        if (obj.stroke.paint.type != FillStyle::Type::None) {
            bp.apply_to_cairo(cr);
            apply_normal_stroke(cr, obj.stroke, ccpat, bx1, by1, bw, bh,
                                cc_as_black);
        }
        end_wrap();
    } else if (obj.type == SceneNode::Type::Compound) {
        // A Compound is ONE visual object. Its own fill/stroke/opacity is the
        // canonical style; child fills and strokes are ignored entirely. This
        // matches the user's mental model: a donut is a shape with a hole,
        // not two stacked paths. The children supply geometry only.
        begin_wrap();

        // Build the combined path from every visible child Path.
        for (const auto& child : obj.children) {
            if (!child || !normal_node_is_printable(*child)) continue;
            if (child->type != SceneNode::Type::Path || !child->path) continue;
            BezierPath bp = BezierPath::from_path_data(*child->path);
            bp.apply_to_cairo(cr);
        }
        // Combined-path bbox covers all subpaths — exactly what
        // objectBoundingBox semantics want for a Compound's gradient.
        double bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
        cr->get_fill_extents(bx1, by1, bx2, by2);
        const double bw = bx2 - bx1;
        const double bh = by2 - by1;
        set_host_bbox(bx1, by1, bw, bh);

        // Fill with the Compound's own fill, even-odd rule. None = no fill.
        if (obj.fill.type != FillStyle::Type::None) {
            apply_normal_fill_paint(cr, obj.fill, ccpat, bx1, by1, bw, bh,
                                    cc_as_black);
            cr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);
            cr->fill();
            cr->set_fill_rule(Cairo::Context::FillRule::WINDING);
        } else {
            cr->begin_new_path();
        }
        // Stroke: apply the Compound's own stroke to each child subpath. If
        // the Compound has no stroke, no strokes are drawn.
        if (obj.stroke.paint.type != FillStyle::Type::None) {
            for (const auto& child : obj.children) {
                if (!child || !normal_node_is_printable(*child)) continue;
                if (child->type != SceneNode::Type::Path || !child->path) continue;
                BezierPath bp = BezierPath::from_path_data(*child->path);
                bp.apply_to_cairo(cr);
                apply_normal_stroke(cr, obj.stroke, ccpat, bx1, by1, bw, bh,
                                    cc_as_black);
            }
        }
        end_wrap();
    } else if (obj.type == SceneNode::Type::Group) {
        // No wrap — children apply their own opacity/shadow individually.
        // Group-level opacity multiplication is a SVG semantic that would
        // need a single push/pop around the whole group; for print we keep
        // the simpler per-child treatment (Canvas does the same in non-
        // shadow paths for Group when no group-level effect is set).
        for (int i = (int)obj.children.size() - 1; i >= 0; --i)
            if (obj.children[i])
                render_normal_node(cr, *obj.children[i], ccpat, cc_as_black);
    } else if (obj.type == SceneNode::Type::Image) {
        // Raster image — no transform handling in this ship (defers match of
        // canvas-side rotate/skew). Prints at image_x/y/w/h doc-space rect.
        if (obj.image_w < 0.01 || obj.image_h < 0.01) return;
        Cairo::RefPtr<Cairo::ImageSurface> img_surf;
        try {
            auto dot = obj.image_path.rfind('.');
            std::string ext = (dot == std::string::npos)
                ? std::string() : obj.image_path.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == "png") {
                img_surf = Cairo::ImageSurface::create_from_png(obj.image_path);
            } else {
                auto pb = Gdk::Pixbuf::create_from_file(obj.image_path);
                if (pb) {
                    int pw = pb->get_width(), ph = pb->get_height();
                    auto surf2 = Cairo::ImageSurface::create(
                        Cairo::Surface::Format::ARGB32, pw, ph);
                    auto cr2 = Cairo::Context::create(surf2);
                    // s135 m2: pumped — replaces deprecated gdk_cairo_set_source_pixbuf.
                    curvz::utils::cairo_set_source_pixbuf(cr2, pb, 0, 0);
                    cr2->paint();
                    img_surf = surf2;
                }
            }
        } catch (...) {
            // Broken path / missing file — silently skip, consistent with
            // how Canvas handles unreadable images.
            return;
        }
        if (!img_surf) return;
        int iw = img_surf->get_width();
        int ih = img_surf->get_height();
        if (iw <= 0 || ih <= 0) return;

        // Image bbox in current user-space is the image_x/y/w/h rect.
        set_host_bbox(obj.image_x, obj.image_y, obj.image_w, obj.image_h);

        begin_wrap();
        cr->save();
        cr->translate(obj.image_x, obj.image_y);
        cr->scale(obj.image_w / (double)iw, obj.image_h / (double)ih);
        cr->set_source(img_surf, 0, 0);
        cr->paint();
        cr->restore();
        end_wrap();
    }
    // Text deliberately not rendered in Normal this ship — needs Pango setup
    // matching Canvas behaviour. Left for a follow-up if needed.
}

// ── Header / footer helpers ─────────────────────────────────────────────────
// Draw the header metadata strip at the top of the page. `y` is the baseline
// for the text; strip is single-line, left-aligned, 8pt grey.
static void draw_normal_header(const Cairo::RefPtr<Cairo::Context>& cr,
                                const CurvzDocument& doc,
                                double x, double y, double max_w,
                                bool show_filename,
                                bool show_dimensions,
                                bool show_nodes,
                                bool show_layers,
                                bool show_date)
{
    (void)max_w;
    std::string parts;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        if (!parts.empty()) parts += "  ·  ";
        parts += s;
    };

    if (show_filename) {
        std::string name = doc.filename.empty()
            ? "untitled"
            : fs::path(doc.filename).stem().string();
        add(name);
    }
    if (show_dimensions) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d × %d px",
                 doc.canvas_width(), doc.canvas_height());
        add(buf);
    }
    if (show_nodes) {
        int n = 0;
        for (const auto& L : doc.layers)
            if (L) n += count_normal_nodes(*L);
        char buf[48];
        snprintf(buf, sizeof(buf), "%d nodes", n);
        add(buf);
    }
    if (show_layers) {
        int n = count_normal_layers(doc);
        char buf[48];
        snprintf(buf, sizeof(buf), "%d layers", n);
        add(buf);
    }
    if (show_date) {
        std::time_t t = std::time(nullptr);
        std::tm tm = *std::localtime(&t);
        char buf[48];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
        add(buf);
    }

    if (parts.empty()) return;

    cr->save();
    cr->set_source_rgb(0.35, 0.35, 0.35);
    cr->select_font_face("sans", Cairo::ToyFontFace::Slant::NORMAL,
                         Cairo::ToyFontFace::Weight::NORMAL);
    cr->set_font_size(8.0);
    cr->move_to(x, y);
    cr->show_text(parts);
    cr->restore();
}

// Draw the colour-list footer. Returns the total used height (0 if no colours).
// Layout: "Colors:" label, then swatch + hex per entry, wrapping at right edge.
static double draw_normal_color_list(const Cairo::RefPtr<Cairo::Context>& cr,
                                       const std::vector<NormalColorEntry>& colors,
                                       const Cairo::RefPtr<Cairo::Pattern>& ccpat,
                                       double x, double y_baseline,
                                       double max_w)
{
    if (colors.empty()) return 0.0;

    const double swatch = 10.0;
    const double gap    = 6.0;
    const double pad    = 4.0;
    const double font   = 7.0;

    cr->save();
    cr->select_font_face("sans", Cairo::ToyFontFace::Slant::NORMAL,
                         Cairo::ToyFontFace::Weight::NORMAL);
    cr->set_font_size(font);

    // Draw leading label.
    cr->set_source_rgb(0.35, 0.35, 0.35);
    const std::string label = "Colors: ";
    Cairo::TextExtents lext;
    cr->get_text_extents(label, lext);
    cr->move_to(x, y_baseline);
    cr->show_text(label);

    double cursor_x = x + lext.x_advance;
    double line_top = y_baseline - swatch + 2.0; // swatch top for first row
    double used_h   = swatch + 2.0;
    const double row_h = swatch + 4.0;

    for (const auto& e : colors) {
        // Build label text.
        std::string hex;
        if (e.is_current) {
            hex = "currentColor";
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                     (int)std::round(e.r * 255.0),
                     (int)std::round(e.g * 255.0),
                     (int)std::round(e.b * 255.0));
            hex = buf;
        }
        Cairo::TextExtents text_ext;
        cr->get_text_extents(hex, text_ext);
        double entry_w = swatch + pad + text_ext.x_advance + gap;

        // Wrap if we'd overflow.
        if (cursor_x + entry_w > x + max_w) {
            cursor_x = x;
            line_top += row_h;
            used_h   += row_h;
        }

        // Swatch.
        if (e.is_current) {
            // Reset pattern matrix — the artwork pass may have scaled it to
            // compensate for the canvas-scale CTM; for the legend swatch we
            // want plain pixel-for-point mapping.
            ccpat->set_matrix(Cairo::identity_matrix());
            cr->save();
            cr->rectangle(cursor_x, line_top, swatch, swatch);
            cr->clip();
            cr->translate(cursor_x, line_top);
            cr->set_source(ccpat);
            cr->rectangle(0, 0, swatch, swatch);
            cr->fill();
            cr->restore();
        } else {
            cr->set_source_rgb(e.r, e.g, e.b);
            cr->rectangle(cursor_x, line_top, swatch, swatch);
            cr->fill();
        }
        // Swatch border — 0.25pt grey for white / very light swatches.
        cr->set_source_rgb(0.6, 0.6, 0.6);
        cr->set_line_width(0.25);
        cr->rectangle(cursor_x, line_top, swatch, swatch);
        cr->stroke();

        // Hex label to the right of the swatch.
        cr->set_source_rgb(0.2, 0.2, 0.2);
        cr->move_to(cursor_x + swatch + pad, line_top + swatch - 1.5);
        cr->show_text(hex);

        cursor_x += entry_w;
    }

    cr->restore();
    return used_h;
}

void PrintManager::render_normal(const Cairo::RefPtr<Cairo::Context>& cr,
                                  const CurvzDocument& doc,
                                  double page_w, double page_h)
{
    const double margin = 36.0; // 0.5" paper margin all around

    // ── Measure header + footer reservation ─────────────────────────────────
    // Header: 1 line ≈ 14pt when present.
    const bool any_hdr =
        m_normal_hdr_filename || m_normal_hdr_dimensions ||
        m_normal_hdr_nodes    || m_normal_hdr_layers     ||
        m_normal_hdr_date;
    const double header_h = any_hdr ? 16.0 : 0.0;

    // Colour list reservation — only if user enabled AND the document
    // actually uses colours. Cap at 3 rows' worth. Actual height is
    // measured by the draw function; we reserve up to 3 * row_h = 42pt.
    std::vector<NormalColorEntry> colors;
    for (const auto& L : doc.layers)
        if (L) collect_normal_colors(*L, colors);
    const bool show_colors = m_normal_show_colors && !colors.empty();
    const double footer_reserve = show_colors ? 42.0 : 0.0;

    // ── Available area for the artwork ──────────────────────────────────────
    const double art_x = margin;
    const double art_y = margin + header_h + (any_hdr ? 8.0 : 0.0);
    const double art_w = page_w - 2.0 * margin;
    const double art_h = page_h - margin - (art_y - margin)
                         - footer_reserve - (show_colors ? 8.0 : 0.0)
                         - margin;

    double cw = doc.canvas_width();
    double ch = doc.canvas_height();
    if (cw <= 0 || ch <= 0 || art_w <= 0 || art_h <= 0) return;
    double scale = std::min(art_w / cw, art_h / ch);
    double ox = art_x + (art_w - cw * scale) * 0.5;
    double oy = art_y + (art_h - ch * scale) * 0.5;

    auto ccpat = make_current_color_pattern();

    // ── Header ──────────────────────────────────────────────────────────────
    if (any_hdr) {
        draw_normal_header(cr, doc, margin, margin + 10.0, page_w - 2.0 * margin,
                           m_normal_hdr_filename,
                           m_normal_hdr_dimensions,
                           m_normal_hdr_nodes,
                           m_normal_hdr_layers,
                           m_normal_hdr_date);
    }

    // ── Optional artboard border ────────────────────────────────────────────
    if (m_normal_show_border) {
        cr->save();
        cr->set_source_rgb(0.75, 0.75, 0.75);
        cr->set_line_width(0.5);
        cr->rectangle(ox, oy, cw * scale, ch * scale);
        cr->stroke();
        cr->restore();
    }

    // ── Artwork ─────────────────────────────────────────────────────────────
    // Clip to the artboard rect (doc-space coords after the scale+translate)
    // so anything extending beyond is cut at the edge, per spec.
    cr->save();
    cr->translate(ox, oy);
    cr->scale(scale, scale);
    cr->rectangle(0, 0, cw, ch);
    cr->clip();

    // Walk layers in z-order: layers[0] paints first (bottom of z-stack),
    // layers[n-1] paints last (top). This matches Canvas::draw_objects so
    // the printed image is identical to what's on screen — earlier this
    // loop walked n-1 → 0, which inverted layer order vs Canvas and let
    // a top-of-stack solid layer (e.g. a black artboard background) print
    // on top of the art rather than behind it. Within a layer, children
    // index n-1 → 0 (child[0] = top) — that part already matched Canvas.
    for (int li = 0; li < (int)doc.layers.size(); ++li) {
        const SceneNode& layer = *doc.layers[li];
        if (!normal_node_is_printable(layer)) continue;
        for (int oi = (int)layer.children.size() - 1; oi >= 0; --oi) {
            if (layer.children[oi])
                render_normal_node(cr, *layer.children[oi], ccpat);
        }
    }
    cr->restore();

    // ── Footer: colour list ─────────────────────────────────────────────────
    if (show_colors) {
        const double footer_baseline = page_h - margin + 2.0;
        draw_normal_color_list(cr, colors, ccpat,
                               margin, footer_baseline,
                               page_w - 2.0 * margin);
    }
}

} // namespace Curvz
