#include "LibraryPanel.hpp"
#include "AppPreferences.hpp"
#include "CurvzLog.hpp"
#include "SvgParser.hpp"
#include "curvz_utils.hpp"  // s121 m5: curvz::utils::set_name
#include "math/BezierPath.hpp"
#include <algorithm>
#include <cairomm/cairomm.h>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <gdkmm/contentprovider.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/popovermenu.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <gtkmm/revealer.h>
#include <gtkmm/separator.h>
#include <gtkmm/window.h>

namespace fs = std::filesystem;

namespace Curvz {

static constexpr int THUMB_SIZE = 56;

// ── Constructor ───────────────────────────────────────────────────────────────
LibraryPanel::LibraryPanel() : Gtk::Box(Gtk::Orientation::VERTICAL) {
    curvz::utils::set_name(*this, "lb", "library_panel_root");
    add_css_class("library-panel");

    // ── Header ────────────────────────────────────────────────────────────
    m_header.set_margin_start(8);
    m_header.set_margin_end(4);
    m_header.set_margin_top(5);
    m_header.set_margin_bottom(3);

    m_title.set_text("  ");
    m_title.set_xalign(0.0f);
    m_title.set_hexpand(true);
    m_title.add_css_class("panel-title");
    m_header.append(m_title);

    m_btn_add.set_icon_name("list-add-symbolic");
    m_btn_add.set_has_frame(false);
    m_btn_add.set_tooltip_text("Add selection to library");
    curvz::utils::set_name(m_btn_add, "lb_add", "library_panel_add_btn");
    m_btn_add.signal_clicked().connect(
        sigc::mem_fun(*this, &LibraryPanel::on_add_clicked));
    m_header.append(m_btn_add);

    m_btn_refresh.set_icon_name("view-refresh-symbolic");
    m_btn_refresh.set_has_frame(false);
    m_btn_refresh.set_tooltip_text("Refresh library");
    curvz::utils::set_name(m_btn_refresh, "lb_rfr", "library_panel_refresh_btn");
    m_btn_refresh.signal_clicked().connect(
        sigc::mem_fun(*this, &LibraryPanel::refresh));
    m_header.append(m_btn_refresh);

    append(m_header);

    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    append(*sep);

    // ── Scrollable content area ───────────────────────────────────────────
    m_content.set_spacing(0);
    m_scroll.set_child(m_content);
    m_scroll.set_vexpand(true);
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    append(m_scroll);

    // Populate on first realise so the widget tree is ready
    signal_realize().connect([this]() {
        Glib::signal_idle().connect_once([this]() { refresh(); });
    });

    LOG_DEBUG("LibraryPanel created");
}

// ── Public ────────────────────────────────────────────────────────────────────
void LibraryPanel::refresh() {
    scan_library();
    rebuild_ui();
}

// s141: seed m_expanded from the project's saved category-expansion list
// when a project is wired (or re-wired on project switch). Refresh after
// so the visual state matches the freshly-seeded map. Setting to nullptr
// (project unload) clears m_expanded so a stale entry doesn't leak into
// the next loaded project.
void LibraryPanel::set_project(CurvzProject* project) {
    m_project = project;
    m_expanded.clear();
    if (m_project) {
        for (const auto& key : m_project->library_expanded_categories) {
            m_expanded[key] = true;
        }
    }
    // Don't unconditionally refresh here — set_project may be called
    // before the panel is realized (during MainWindow::setup_layout).
    // The realize-driven refresh in the ctor will pick up the seeded
    // m_expanded state on the next rebuild_ui() pass. Subsequent
    // project switches that happen post-realize get refreshed by
    // MainWindow's load_project path which calls refresh() explicitly.
}

// s141: write the current expanded set back into the project. Sparse —
// only entries with value true are emitted, sorted for stable diffs in
// the project.json file. Called from the category click handler after
// each toggle; pairs with signal_request_save so MainWindow triggers a
// save through its existing schedule_save debounce.
void LibraryPanel::sync_expanded_to_project() {
    if (!m_project) return;
    std::vector<std::string> keys;
    keys.reserve(m_expanded.size());
    for (const auto& [k, v] : m_expanded) {
        if (v) keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    m_project->library_expanded_categories = std::move(keys);
}

// ── Paths ─────────────────────────────────────────────────────────────────────
// s145 m4: consult AppPreferences::library_path_override first; empty
// override falls through to the historical built-in default.
std::string LibraryPanel::user_library_dir() const {
    const std::string& override_path =
        AppPreferences::instance().library_path_override();
    if (!override_path.empty()) return override_path;
    return Glib::get_user_config_dir() + "/curvz/library";
}

std::string LibraryPanel::system_library_dir() const {
    return "/usr/share/curvz/library";
}

// ── scan_library ─────────────────────────────────────────────────────────────
// Walks system dir (read-only) then user dir. Each immediate subdirectory
// becomes a Category; .svg files inside are LibraryItems.
// If a user category has the same name as a system category, items are merged.
void LibraryPanel::scan_library() {
    m_categories.clear();

    auto scan_dir = [&](const std::string& base, bool is_user) {
        std::error_code ec;
        if (!fs::is_directory(base, ec)) return;

        for (const auto& entry : fs::directory_iterator(base, ec)) {
            if (!entry.is_directory()) continue;
            std::string cat_name = entry.path().filename().string();
            if (cat_name.empty() || cat_name[0] == '.') continue;

            // Find or create category
            Category* cat = nullptr;
            for (auto& c : m_categories) {
                if (c.name == cat_name) { cat = &c; break; }
            }
            if (!cat) {
                m_categories.push_back({ cat_name, is_user, {} });
                cat = &m_categories.back();
            }
            // If this dir is user-writable, mark category as user
            if (is_user) cat->is_user = true;

            // Scan .svg files in category dir
            std::error_code ec2;
            std::vector<LibraryItem> items;
            for (const auto& svg : fs::directory_iterator(entry.path(), ec2)) {
                if (!svg.is_regular_file()) continue;
                auto ext = svg.path().extension().string();
                // Case-insensitive .svg check
                std::string extl = ext;
                std::transform(extl.begin(), extl.end(), extl.begin(), ::tolower);
                if (extl != ".svg") continue;

                LibraryItem item;
                item.path     = svg.path().string();
                item.name     = svg.path().stem().string();
                item.category = cat_name;
                item.is_user  = is_user;
                items.push_back(std::move(item));
            }
            // Sort alphabetically
            std::sort(items.begin(), items.end(),
                      [](const LibraryItem& a, const LibraryItem& b) {
                          return a.name < b.name;
                      });
            for (auto& item : items)
                cat->items.push_back(std::move(item));
        }
    };

    // ── First-launch seeding of default categories ──────────────────────
    // s141: previously this seeded on every scan, which made deletion of
    // a default category (via the new context menu) impossible — the
    // folder reappeared empty on the next refresh. Now it runs once,
    // gated by AppPreferences::library_defaults_seeded. After the seed
    // succeeds the flag flips true and persists to preferences.json, so
    // subsequent launches skip the seed pass entirely.
    std::string user_dir = user_library_dir();
    std::error_code ec_mk;
    fs::create_directories(user_dir, ec_mk);

    auto& prefs = AppPreferences::instance();
    if (!prefs.library_defaults_seeded()) {
        static const char* default_categories[] = {
            "arrows", "shapes", "icons", "ui", nullptr
        };
        for (int i = 0; default_categories[i]; ++i) {
            std::error_code ec_cat;
            fs::create_directories(user_dir + "/" + default_categories[i],
                                   ec_cat);
            if (ec_cat)
                LOG_WARN("LibraryPanel: could not create '{}': {}",
                         user_dir + "/" + default_categories[i],
                         ec_cat.message());
        }
        prefs.set_library_defaults_seeded(true);
        LOG_INFO("LibraryPanel: seeded default categories (first launch)");
    }

    scan_dir(system_library_dir(), false);
    scan_dir(user_dir,             true);

    // Sort categories: system first, then user; alphabetically within each group
    std::stable_sort(m_categories.begin(), m_categories.end(),
                     [](const Category& a, const Category& b) {
                         if (a.is_user != b.is_user) return !a.is_user; // system first
                         return a.name < b.name;
                     });

    LOG_INFO("LibraryPanel: scanned {} categories", m_categories.size());
}

// ── render_thumb ──────────────────────────────────────────────────────────────
// Parse the SVG and render its visible paths to a Cairo surface.
Cairo::RefPtr<Cairo::ImageSurface>
LibraryPanel::render_thumb(const std::string& svg_path, int size) {
    auto surf = Cairo::ImageSurface::create(
        Cairo::Surface::Format::ARGB32, size, size);
    auto cr = Cairo::Context::create(surf);

    // s148 m1: thumb bg reads the active doc's artboard colour (re-
    // promoted from project per-motif to per-doc). Library icons preview
    // as they would look if dropped into the active doc — intuitive
    // alignment with what the user sees on canvas. currentColor
    // luminance derives from the bg luminance; same 0.6 threshold
    // Canvas/Gallery use, so the cc decision is uniform across surfaces.
    double bg_r = 0.157, bg_g = 0.157, bg_b = 0.157;
    if (m_project) {
        if (auto *active = m_project->active_doc()) {
            bg_r = active->artboard_bg_r;
            bg_g = active->artboard_bg_g;
            bg_b = active->artboard_bg_b;
        }
    }
    bool light_bg = std::max({bg_r, bg_g, bg_b}) > 0.60;
    double cc = light_bg ? 0.10 : 0.88;

    cr->set_source_rgb(bg_r, bg_g, bg_b);
    cr->paint();

    auto doc = parse_svg_file(svg_path);
    if (!doc || doc->layers.empty()) return surf;

    int cw = doc->canvas_width();
    int ch = doc->canvas_height();
    if (cw <= 0 || ch <= 0) return surf;

    double margin = 4.0;
    double scale  = std::min((size - margin * 2.0) / cw,
                             (size - margin * 2.0) / ch);
    double ox = (size - cw * scale) * 0.5;
    double oy = (size - ch * scale) * 0.5;

    cr->save();
    cr->translate(ox, oy);
    cr->scale(scale, scale);
    cr->rectangle(0, 0, cw, ch);
    cr->clip();

    // Draw helper — matches DocumentGallery::render_thumb logic
    std::function<void(const SceneNode&)> draw_node = [&](const SceneNode& obj) {
        if (obj.type == SceneNode::Type::Path && obj.path) {
            BezierPath bp = BezierPath::from_path_data(*obj.path);
            bp.apply_to_cairo(cr);

            if (obj.fill.type == FillStyle::Type::CurrentColor)
                cr->set_source_rgb(cc, cc, cc);
            else if (obj.fill.type == FillStyle::Type::Solid)
                cr->set_source_rgb(obj.fill.r, obj.fill.g, obj.fill.b);
            if (obj.fill.type != FillStyle::Type::None)
                cr->fill_preserve();

            if (obj.stroke.paint.type == FillStyle::Type::CurrentColor)
                cr->set_source_rgb(cc, cc, cc);
            else if (obj.stroke.paint.type == FillStyle::Type::Solid)
                cr->set_source_rgb(obj.stroke.paint.r, obj.stroke.paint.g,
                                   obj.stroke.paint.b);
            if (obj.stroke.paint.type != FillStyle::Type::None) {
                cr->set_line_width(obj.stroke.width);
                cr->stroke();
            } else {
                cr->begin_new_path();
            }
        } else if (obj.type == SceneNode::Type::Compound) {
            if (obj.children.empty()) return;
            // S58p: Compound owns its paint (S58d rule).
            FillStyle   fill   = obj.fill;
            StrokeStyle stroke = obj.stroke;
            // s127: descending iteration mirrors Canvas::draw_object's
            // Compound branch. For a single fill_preserve+stroke pass
            // against a combined Cairo path under EVEN_ODD, pixel output
            // is order-independent, but consistency keeps the convention
            // legible across renderers (Canvas, gallery, library, png).
            for (int i = (int)obj.children.size() - 1; i >= 0; --i) {
                const auto& child = obj.children[i];
                if (child->type == SceneNode::Type::Path && child->path) {
                    BezierPath bp = BezierPath::from_path_data(*child->path);
                    bp.apply_to_cairo(cr);
                }
            }
            if (fill.type == FillStyle::Type::CurrentColor)
                cr->set_source_rgb(cc, cc, cc);
            else if (fill.type == FillStyle::Type::Solid)
                cr->set_source_rgb(fill.r, fill.g, fill.b);
            if (fill.type != FillStyle::Type::None) {
                cr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);
                cr->fill_preserve();
                cr->set_fill_rule(Cairo::Context::FillRule::WINDING);
            }
            if (stroke.paint.type == FillStyle::Type::CurrentColor)
                cr->set_source_rgb(cc, cc, cc);
            else if (stroke.paint.type == FillStyle::Type::Solid)
                cr->set_source_rgb(stroke.paint.r, stroke.paint.g, stroke.paint.b);
            if (stroke.paint.type != FillStyle::Type::None) {
                cr->set_line_width(stroke.width);
                cr->stroke();
            } else {
                cr->begin_new_path();
            }
        } else if (obj.type == SceneNode::Type::Group) {
            // s127: same descending convention as the layer-level loop —
            // children[0] = top → paint last → lands on top. Mirrors
            // Canvas::draw_object's Group branch.
            for (int i = (int)obj.children.size() - 1; i >= 0; --i)
                draw_node(*obj.children[i]);
        }
    };

    for (const auto& layer_uptr : doc->layers) {
        if (!layer_uptr->visible) continue;
        if (layer_uptr->is_guide_layer() || layer_uptr->is_ref_layer()) continue;
        // s127: within-layer z-order. Curvz convention is children[0] =
        // top of the LayersPanel within the layer; Cairo paints
        // last-painted-wins, so iterate descending so children[0] paints
        // LAST and lands on top. Pre-s127 ascending order silently
        // inverted within-layer z-order in thumbs (e.g. an icon with a
        // background rect painted over its glyph). Same fix as PngExporter
        // in s126; thumbs were missed at the time. Layer-level iteration
        // remains ascending (layers[n-1] = panel-top = z-top).
        for (int i = (int)layer_uptr->children.size() - 1; i >= 0; --i)
            draw_node(*layer_uptr->children[i]);
    }
    cr->restore();

    // Subtle border
    cr->set_source_rgba(0.5, 0.5, 0.5, 0.4);
    cr->set_line_width(0.5);
    cr->rectangle(ox + 0.25, oy + 0.25,
                  cw * scale - 0.5, ch * scale - 0.5);
    cr->stroke();

    return surf;
}

// ── make_item_card ────────────────────────────────────────────────────────────
Gtk::Widget* LibraryPanel::make_item_card(const LibraryItem& item) {
    auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    outer->set_spacing(2);
    outer->set_margin(3);
    outer->add_css_class("library-card");
    outer->set_cursor(Gdk::Cursor::create("pointer"));

    // Thumbnail drawing area
    auto* da = Gtk::make_managed<Gtk::DrawingArea>();
    da->set_size_request(THUMB_SIZE, THUMB_SIZE);
    da->set_halign(Gtk::Align::CENTER);

    std::string svg_path = item.path; // capture by value
    bool        is_user  = item.is_user;
    da->set_draw_func(
        [this, svg_path](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            if (w < 4 || h < 4) return;
            int sz   = std::min(w, h);
            auto surf = render_thumb(svg_path, sz);
            if (!surf) return;
            double ox = (w - sz) * 0.5;
            double oy = (h - sz) * 0.5;
            cr->set_source(surf, ox, oy);
            cr->paint();
        });
    outer->append(*da);

    // Name label
    auto* lbl = Gtk::make_managed<Gtk::Label>(item.name);
    lbl->set_ellipsize(Pango::EllipsizeMode::END);
    lbl->set_max_width_chars(9);
    lbl->add_css_class("caption-label");
    lbl->set_halign(Gtk::Align::CENTER);
    outer->append(*lbl);

    // ── Left-click: double-click to place, single-click just focuses ──────
    auto click = Gtk::GestureClick::create();
    click->set_button(1);
    click->signal_pressed().connect(
        [this, svg_path](int n_press, double, double) {
            if (n_press == 2)
                m_sig_place_item.emit(svg_path);
        });
    outer->add_controller(click);

    // ── Right-click context menu ──────────────────────────────────────────
    auto rclick = Gtk::GestureClick::create();
    rclick->set_button(3);
    rclick->signal_pressed().connect(
        [this, outer, svg_path, is_user](int, double x, double y) {
            auto menu = Gio::Menu::create();
            menu->append("Place on canvas", "libctx.place");

            // s136 m4: Rename… — user-tier items only. Library system tier
            // is read-only (lives under /usr/share/curvz/library) so renames
            // would silently fail; gating the entry to is_user makes the
            // capability honest at the menu level rather than on click.
            if (is_user)
                menu->append("Rename…", "libctx.rename");

            if (is_user)
                menu->append("Delete from library", "libctx.del");

            auto ag = Gio::SimpleActionGroup::create();

            auto act_place = Gio::SimpleAction::create("place");
            act_place->signal_activate().connect(
                [this, svg_path](const Glib::VariantBase&) {
                    m_sig_place_item.emit(svg_path);
                });
            ag->add_action(act_place);

            if (is_user) {
                // s136 m4: Rename. Pulls the current stem, prompts via
                // show_form, validates the typed name (non-empty, no slashes),
                // checks for collision in the same category folder, and
                // performs an fs::rename on success. Refresh the panel from
                // an idle so the widget tree is stable when we rebuild.
                auto act_rename = Gio::SimpleAction::create("rename");
                act_rename->signal_activate().connect(
                    [this, outer, svg_path](const Glib::VariantBase&) {
                        // Walk up to the parent Window for the dialog.
                        Gtk::Window* win = nullptr;
                        for (Gtk::Widget* w = outer->get_parent(); w;
                             w = w->get_parent()) {
                            win = dynamic_cast<Gtk::Window*>(w);
                            if (win) break;
                        }
                        if (!win) {
                            LOG_WARN("LibraryPanel: rename — no parent window");
                            return;
                        }

                        const std::string current_stem =
                            fs::path(svg_path).stem().string();
                        const std::string parent_dir =
                            fs::path(svg_path).parent_path().string();

                        std::vector<curvz::utils::FormField> fields = {
                            {"name", "Name",
                             curvz::utils::TextField{current_stem,
                                                     "Library item name"}}};
                        std::vector<std::string> buttons = {"Cancel", "Rename"};

                        curvz::utils::show_form(
                            *win, "Rename library item",
                            "New name for this item.",
                            fields, buttons,
                            /*default_button=*/1, /*cancel_button=*/0,
                            [this, win, svg_path, parent_dir, current_stem](
                                int btn,
                                const std::map<std::string,
                                               curvz::utils::FormFieldValue>&
                                    values) {
                                if (btn != 1) return; // Cancel

                                std::string name;
                                auto it = values.find("name");
                                if (it != values.end())
                                    name = it->second.text();
                                // Trim
                                auto trim = [](std::string s) {
                                    size_t i = 0;
                                    while (i < s.size() &&
                                           std::isspace((unsigned char)s[i]))
                                        ++i;
                                    s = s.substr(i);
                                    size_t j = s.size();
                                    while (j > 0 && std::isspace(
                                               (unsigned char)s[j - 1]))
                                        --j;
                                    return s.substr(0, j);
                                };
                                name = trim(name);

                                // Empty → silently no-op. The user's intent
                                // is unclear; refusing without complaint is
                                // friendlier than an alert.
                                if (name.empty()) return;

                                // Reject path separators — names map to
                                // single filenames in the category folder,
                                // not subpaths.
                                if (name.find('/') != std::string::npos ||
                                    name.find('\\') != std::string::npos) {
                                    curvz::utils::show_alert(
                                        *win, "Invalid name",
                                        "Library item names cannot contain "
                                        "slashes.");
                                    return;
                                }

                                // No-op if the user re-typed the same name.
                                if (name == current_stem) return;

                                const std::string new_path =
                                    parent_dir + "/" + name + ".svg";

                                std::error_code ec;
                                if (fs::exists(new_path, ec)) {
                                    curvz::utils::show_alert(
                                        *win, "Item already exists",
                                        "An item named \"" + name +
                                            "\" already exists in this "
                                            "category. Choose a different "
                                            "name.");
                                    return;
                                }

                                fs::rename(svg_path, new_path, ec);
                                if (ec) {
                                    LOG_WARN("LibraryPanel: rename failed "
                                             "for '{}': {}",
                                             svg_path, ec.message());
                                    curvz::utils::show_alert(
                                        *win, "Rename failed",
                                        "Could not rename to \"" + name +
                                            "\".");
                                    return;
                                }
                                LOG_INFO("LibraryPanel: renamed '{}' → '{}'",
                                         svg_path, new_path);
                                Glib::signal_idle().connect_once(
                                    [this]() { refresh(); });
                            });
                    });
                ag->add_action(act_rename);
            }

            if (is_user) {
                auto act_del = Gio::SimpleAction::create("del");
                act_del->signal_activate().connect(
                    [this, svg_path](const Glib::VariantBase&) {
                        std::error_code ec;
                        fs::remove(svg_path, ec);
                        if (ec)
                            LOG_WARN("LibraryPanel: delete failed for '{}': {}",
                                     svg_path, ec.message());
                        else
                            LOG_INFO("LibraryPanel: deleted '{}'", svg_path);
                        // Refresh on next idle so the widget tree is stable
                        Glib::signal_idle().connect_once([this]() { refresh(); });
                    });
                ag->add_action(act_del);
            }

            outer->insert_action_group("libctx", ag);

            auto* popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
            popover->set_parent(*outer);
            popover->set_has_arrow(false);
            Gdk::Rectangle rect;
            rect.set_x((int)x); rect.set_y((int)y);
            rect.set_width(1);  rect.set_height(1);
            popover->set_pointing_to(rect);
            popover->popup();
        });
    outer->add_controller(rclick);

    // ── Drag to place ─────────────────────────────────────────────────────
    auto drag_src = Gtk::DragSource::create();
    drag_src->set_actions(Gdk::DragAction::COPY);
    drag_src->signal_prepare().connect(
        [svg_path](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
            Glib::Value<Glib::ustring> val;
            val.init(Glib::Value<Glib::ustring>::value_type());
            val.set(Glib::ustring(svg_path));
            return Gdk::ContentProvider::create(val);
        }, false);
    outer->add_controller(drag_src);

    return outer;
}

// ── make_category_section ────────────────────────────────────────────────────
Gtk::Widget* LibraryPanel::make_category_section(const Category& cat) {
    auto* section = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);

    // ── Header row: arrow + name ──────────────────────────────────────────
    auto* hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    hdr->set_spacing(4);
    hdr->set_margin_start(6);
    hdr->set_margin_top(4);
    hdr->set_margin_bottom(2);
    hdr->add_css_class("library-category-header");

    auto* arrow = Gtk::make_managed<Gtk::Label>("▸");
    arrow->add_css_class("dim-label");
    hdr->append(*arrow);

    std::string display = cat.name;
    if (cat.is_user) display += " ✎"; // pencil glyph to indicate user-writable
    auto* cat_lbl = Gtk::make_managed<Gtk::Label>(display);
    cat_lbl->set_xalign(0.0f);
    cat_lbl->set_hexpand(true);
    cat_lbl->add_css_class("layer-label");
    hdr->append(*cat_lbl);

    section->append(*hdr);

    // ── Revealer containing the FlowBox ──────────────────────────────────
    // s141: default to collapsed. Library starts as a list of category
    // headers; the user opens the ones they want to use. Affinity / Sketch
    // / Figma asset libraries follow the same convention. The persisted
    // state below overrides this default for any category the user has
    // explicitly expanded this session.
    auto* rev = Gtk::make_managed<Gtk::Revealer>();
    rev->set_reveal_child(false);
    rev->set_transition_type(Gtk::RevealerTransitionType::SLIDE_DOWN);

    auto* flow = Gtk::make_managed<Gtk::FlowBox>();
    flow->set_homogeneous(true);
    flow->set_row_spacing(2);
    flow->set_column_spacing(2);
    flow->set_margin(4);
    flow->set_selection_mode(Gtk::SelectionMode::NONE);
    flow->set_max_children_per_line(20);
    flow->set_min_children_per_line(2);

    for (const auto& item : cat.items) {
        auto* card = make_item_card(item);
        flow->append(*card);
    }

    if (cat.items.empty()) {
        auto* empty = Gtk::make_managed<Gtk::Label>("No items");
        empty->add_css_class("dim-label");
        empty->set_margin(8);
        flow->append(*empty);
    }

    rev->set_child(*flow);
    section->append(*rev);

    // ── Toggle collapse on header click ──────────────────────────────────
    // s141: pin the collapse gesture to the primary (left) button so a
    // right-click on a user-category header doesn't double-fire — collapse
    // toggle and context-menu popup would otherwise both run from the same
    // press. The default GestureClick::set_button(0) means "any button."
    //
    // Expanded-state persistence: keyed by "sys:<name>" / "usr:<name>"
    // in m_expanded. Lookup of a missing key returns false (= use default,
    // which is collapsed). User clicks to expand → entry flips true and
    // stays expanded across refresh()/rebuild_ui() rebuilds. In-memory
    // only — closing the app resets every category back to collapsed,
    // which matches the "fresh launch starts quiet" intent.
    const std::string expand_key =
        (cat.is_user ? std::string("usr:") : std::string("sys:")) + cat.name;
    const bool start_expanded = m_expanded[expand_key];
    if (start_expanded) {
        rev->set_reveal_child(true);
        arrow->set_text("▾");
    }

    auto click = Gtk::GestureClick::create();
    click->set_button(GDK_BUTTON_PRIMARY);
    click->signal_pressed().connect(
        [this, rev, arrow, expand_key](int, double, double) {
            // Flip stored state, then drive the UI from the stored state.
            // Reading the map back rather than tracking a local bool keeps
            // the source of truth in one place.
            const bool now_expanded = !m_expanded[expand_key];
            m_expanded[expand_key] = now_expanded;
            rev->set_reveal_child(now_expanded);
            arrow->set_text(now_expanded ? "▾" : "▸");
            // s141: persist the new expansion state to the project file.
            // sync_expanded_to_project writes m_project's vector; the
            // signal asks MainWindow to schedule a save through its
            // existing debounce. Pair runs even if m_project is null
            // (no-op inside sync); the signal still fires but with no
            // listener attached during early bring-up.
            sync_expanded_to_project();
            m_sig_request_save.emit();
        });
    hdr->add_controller(click);
    hdr->set_cursor(Gdk::Cursor::create("pointer"));

    // ── Right-click context menu on user categories ─────────────────────
    // s141: mirrors the item-card context menu (s136 m4). System
    // categories (read-only, under /usr/share/curvz/library) get no
    // menu — gating at the gesture level is the same "honest at the
    // menu, not on click" discipline the item card already uses.
    if (cat.is_user) {
        const std::string cat_name  = cat.name;
        const std::string user_dir  = user_library_dir();
        const std::string cat_dir   = user_dir + "/" + cat_name;
        const std::size_t item_count = cat.items.size();

        // s141: capture item names for the delete confirm preview. Showing
        // names rather than just a count makes the destructive nature
        // concrete — "delete arrow-left, arrow-right, +3 more" is harder
        // to dismiss than "delete 5 items."
        std::vector<std::string> item_names;
        item_names.reserve(cat.items.size());
        for (const auto& it : cat.items) item_names.push_back(it.name);

        auto rclick = Gtk::GestureClick::create();
        rclick->set_button(GDK_BUTTON_SECONDARY);
        rclick->signal_pressed().connect(
            [this, hdr, cat_name, cat_dir, item_count, item_names](
                int, double x, double y) {
                auto menu = Gio::Menu::create();
                menu->append("Rename folder…",  "libcatctx.rename");
                menu->append("Delete folder",   "libcatctx.del");

                auto ag = Gio::SimpleActionGroup::create();

                // ── Rename folder ─────────────────────────────────────
                auto act_rename = Gio::SimpleAction::create("rename");
                act_rename->signal_activate().connect(
                    [this, hdr, cat_name, cat_dir](
                        const Glib::VariantBase&) {
                        Gtk::Window* win = nullptr;
                        for (Gtk::Widget* w = hdr->get_parent(); w;
                             w = w->get_parent()) {
                            win = dynamic_cast<Gtk::Window*>(w);
                            if (win) break;
                        }
                        if (!win) {
                            LOG_WARN("LibraryPanel: rename folder — "
                                     "no parent window");
                            return;
                        }

                        const std::string parent_dir =
                            fs::path(cat_dir).parent_path().string();

                        std::vector<curvz::utils::FormField> fields = {
                            {"name", "Name",
                             curvz::utils::TextField{cat_name,
                                                     "Folder name"}}};
                        std::vector<std::string> buttons = {"Cancel",
                                                            "Rename"};

                        curvz::utils::show_form(
                            *win, "Rename library folder",
                            "New name for this folder.",
                            fields, buttons,
                            /*default_button=*/1, /*cancel_button=*/0,
                            [this, win, cat_name, cat_dir, parent_dir](
                                int btn,
                                const std::map<std::string,
                                               curvz::utils::FormFieldValue>&
                                    values) {
                                if (btn != 1) return; // Cancel

                                std::string name;
                                auto it = values.find("name");
                                if (it != values.end())
                                    name = it->second.text();

                                // Trim (matches item-rename behaviour)
                                auto trim = [](std::string s) {
                                    size_t i = 0;
                                    while (i < s.size() &&
                                           std::isspace(
                                               (unsigned char)s[i]))
                                        ++i;
                                    s = s.substr(i);
                                    size_t j = s.size();
                                    while (j > 0 &&
                                           std::isspace(
                                               (unsigned char)s[j - 1]))
                                        --j;
                                    return s.substr(0, j);
                                };
                                name = trim(name);

                                // Empty / unchanged → silent no-op.
                                if (name.empty()) return;
                                if (name == cat_name) return;

                                // Reject path separators — folder names
                                // map to single dir entries under user
                                // library, not subpaths.
                                if (name.find('/') != std::string::npos ||
                                    name.find('\\') != std::string::npos) {
                                    curvz::utils::show_alert(
                                        *win, "Invalid name",
                                        "Folder names cannot contain "
                                        "slashes.");
                                    return;
                                }
                                // Reject leading dot — would create a
                                // hidden folder the panel would skip on
                                // its next scan.
                                if (name[0] == '.') {
                                    curvz::utils::show_alert(
                                        *win, "Invalid name",
                                        "Folder names cannot start with "
                                        "a dot.");
                                    return;
                                }

                                const std::string new_path =
                                    parent_dir + "/" + name;

                                std::error_code ec;
                                if (fs::exists(new_path, ec)) {
                                    curvz::utils::show_alert(
                                        *win, "Folder already exists",
                                        "A folder named \"" + name +
                                            "\" already exists. Choose a "
                                            "different name.");
                                    return;
                                }

                                fs::rename(cat_dir, new_path, ec);
                                if (ec) {
                                    LOG_WARN("LibraryPanel: folder "
                                             "rename failed for '{}': {}",
                                             cat_dir, ec.message());
                                    curvz::utils::show_alert(
                                        *win, "Rename failed",
                                        "Could not rename folder to \"" +
                                            name + "\".");
                                    return;
                                }
                                LOG_INFO("LibraryPanel: renamed folder "
                                         "'{}' → '{}'",
                                         cat_dir, new_path);
                                Glib::signal_idle().connect_once(
                                    [this]() { refresh(); });
                            });
                    });
                ag->add_action(act_rename);

                // ── Delete folder ─────────────────────────────────────
                auto act_del = Gio::SimpleAction::create("del");
                act_del->signal_activate().connect(
                    [this, hdr, cat_name, cat_dir, item_count, item_names](
                        const Glib::VariantBase&) {
                        Gtk::Window* win = nullptr;
                        for (Gtk::Widget* w = hdr->get_parent(); w;
                             w = w->get_parent()) {
                            win = dynamic_cast<Gtk::Window*>(w);
                            if (win) break;
                        }
                        if (!win) {
                            LOG_WARN("LibraryPanel: delete folder — "
                                     "no parent window");
                            return;
                        }

                        // s141: build a louder, content-aware confirm
                        // message. Empty folder is a small commitment;
                        // folder with items lists up to 5 of them so the
                        // user sees concrete content before confirming.
                        // The "+ N more" tail keeps the dialog from
                        // ballooning on big folders without hiding the
                        // total count.
                        constexpr std::size_t PREVIEW_CAP = 5;
                        std::string detail;
                        if (item_count == 0) {
                            detail = "Delete the empty folder \"" +
                                     cat_name + "\"?";
                        } else {
                            detail = "Delete the folder \"" + cat_name +
                                     "\" and ";
                            if (item_count == 1) {
                                detail += "the 1 item inside it?";
                            } else {
                                detail += "all " +
                                          std::to_string(item_count) +
                                          " items inside it?";
                            }
                            detail += "\n\nItems that will be permanently "
                                      "lost:\n";
                            const std::size_t shown =
                                std::min(item_count, PREVIEW_CAP);
                            for (std::size_t i = 0; i < shown; ++i) {
                                detail += "  • " + item_names[i] + "\n";
                            }
                            if (item_count > PREVIEW_CAP) {
                                detail += "  • …and " +
                                          std::to_string(item_count -
                                                         PREVIEW_CAP) +
                                          " more\n";
                            }
                            detail += "\nThis cannot be undone.";
                        }

                        std::vector<std::string> buttons = {"Cancel",
                                                            "Delete"};

                        curvz::utils::show_confirm(
                            *win, "Delete library folder",
                            detail, buttons,
                            /*default_button=*/0, /*cancel_button=*/0,
                            [this, cat_dir, cat_name](int btn) {
                                if (btn != 1) return; // Cancel

                                std::error_code ec;
                                fs::remove_all(cat_dir, ec);
                                if (ec) {
                                    LOG_WARN("LibraryPanel: delete "
                                             "folder failed for '{}': {}",
                                             cat_dir, ec.message());
                                } else {
                                    LOG_INFO("LibraryPanel: deleted "
                                             "folder '{}' ('{}')",
                                             cat_dir, cat_name);
                                }
                                Glib::signal_idle().connect_once(
                                    [this]() { refresh(); });
                            });
                    });
                ag->add_action(act_del);

                hdr->insert_action_group("libcatctx", ag);

                auto* popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
                popover->set_parent(*hdr);
                popover->set_has_arrow(false);
                Gdk::Rectangle rect;
                rect.set_x((int)x); rect.set_y((int)y);
                rect.set_width(1);  rect.set_height(1);
                popover->set_pointing_to(rect);
                popover->popup();
            });
        hdr->add_controller(rclick);
    }

    // Separator below each category
    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    sep->set_margin_top(2);
    section->append(*sep);

    return section;
}

// ── rebuild_ui ────────────────────────────────────────────────────────────────
void LibraryPanel::rebuild_ui() {
    // Remove existing children from content box
    while (auto* child = m_content.get_first_child())
        m_content.remove(*child);

    if (m_categories.empty()) {
        auto* lbl = Gtk::make_managed<Gtk::Label>(
            "No library items found.\n"
            "Add SVGs to ~/.config/curvz/library/{category}/");
        lbl->set_justify(Gtk::Justification::CENTER);
        lbl->set_wrap(true);
        lbl->set_margin(12);
        lbl->add_css_class("dim-label");
        lbl->set_valign(Gtk::Align::CENTER);
        lbl->set_vexpand(true);
        m_content.append(*lbl);
        return;
    }

    for (const auto& cat : m_categories) {
        auto* sec = make_category_section(cat);
        m_content.append(*sec);
    }
}

// ── on_add_clicked ────────────────────────────────────────────────────────────
// Opens a folder-chooser to pick a category, then emits signal_add_to_library
// with a destination path of the form:
//   ~/.config/curvz/library/{category}/{name}.svg
void LibraryPanel::on_add_clicked() {
    // Find the nearest ancestor Window
    Gtk::Window* win = nullptr;
    for (Gtk::Widget* w = get_parent(); w; w = w->get_parent()) {
        win = dynamic_cast<Gtk::Window*>(w);
        if (win) break;
    }
    if (!win) {
        LOG_WARN("LibraryPanel::on_add_clicked: no parent window found");
        return;
    }

    // Ensure user library dir exists before opening the picker
    std::string lib_dir = user_library_dir();
    std::error_code ec;
    fs::create_directories(lib_dir, ec);

    // GTK4 async folder picker — pre-pointed at the user library dir
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Choose library category folder");

    try {
        auto initial = Gio::File::create_for_path(lib_dir);
        dialog->set_initial_folder(initial);
    } catch (...) {}

    dialog->select_folder(*win,
        [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto folder = dialog->select_folder_finish(result);
                if (!folder) return;
                std::string dest_dir = folder->get_path();
                // Emit with chosen directory; MainWindow derives filename
                m_sig_add_to_library.emit(dest_dir);
            } catch (...) {
                // User cancelled — do nothing
            }
        });
}

} // namespace Curvz
