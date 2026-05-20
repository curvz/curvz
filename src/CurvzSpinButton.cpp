#include "CurvzSpinButton.hpp"
#include "UnitSystem.hpp"
#include "CoordConvert.hpp"   // s192 m2 — canonical doc<->display helpers
#include "CurvzLog.hpp"
#include <cmath>
#include <algorithm>
#include <gdk/gdkkeysyms.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/popover.h>
#include <glibmm/main.h>

namespace Curvz {

// ── Diagnostic helpers (Session 47 math debug) ────────────────────────────────
// These stringify enums so log output is human-readable.  Remove once the
// csb conversion bug is nailed down.

static const char* spin_type_name(SpinType t) {
    switch (t) {
    case SpinType::Distance:   return "Distance";
    case SpinType::Width:      return "Width";
    case SpinType::PositionX:  return "PositionX";
    case SpinType::PositionY:  return "PositionY";
    case SpinType::Angle:      return "Angle";
    case SpinType::Percentage: return "Percentage";
    case SpinType::Integer:    return "Integer";
    }
    return "?";
}

static const char* display_mode_name(const CanvasModel* m) {
    if (!m) return "null";
    switch (m->display_mode) {
    case DisplayMode::Pixel:        return "Pixel";
    case DisplayMode::Physical:     return "Physical";
    case DisplayMode::RatioQuality: return "Ratio";
    }
    return "?";
}

// ── DPI-aware unit conversion ────────────────────────────────────────────────
// s192 m2 — promoted to CoordConvert.hpp as inline namespace helpers so
// both this file and the right-click guide dialog use the same math.
// effective_dpi / px_to_unit / unit_to_px now live there.


// ── Construction ──────────────────────────────────────────────────────────────

CurvzSpinButton::CurvzSpinButton(SpinType type, const CanvasModel* model)
    : Gtk::SpinButton()
    , m_type(type)
    , m_model(model)
    , m_ruler_origin(0.0)
{
    init();
}

CurvzSpinButton::CurvzSpinButton(SpinType type,
                                   const CanvasModel* model,
                                   double ruler_origin)
    : Gtk::SpinButton()
    , m_type(type)
    , m_model(model)
    , m_ruler_origin(ruler_origin)
{
    init();
}

void CurvzSpinButton::init()
{
    m_adj = Gtk::Adjustment::create(0.0, -1e9, 1e9, 1.0, 10.0);
    set_adjustment(m_adj);

    // s268: enable click-and-hold auto-repeat on the +/- buttons.
    // GTK4's GtkSpinButton has a built-in repeat timer, but it only
    // accelerates above the base step when climb_rate > 0; with the
    // default 0.0 from the Gtk::SpinButton() ctor, holding the button
    // produces a single bump on press and nothing further until release.
    // 0.5 gives a comfortable acceleration curve without overshooting.
    set_climb_rate(0.5);

    // s219: all spin types accept math expressions.
    // s263 m5: parser is Domain-aware — each SpinType maps to a parser
    // Domain (Length / Angle / Percentage / Dimensionless) via
    // type_domain(); the parser permits the suffixes legal for that
    // domain and refuses cross-domain mixing structurally. Angle fields
    // accept deg/rad, Percentage fields accept %, Length fields accept
    // in/"/mm/pt/px, Integer fields reject all suffixes. is_char_allowed
    // shares the same Domain dispatch so the keystroke filter agrees
    // with the parser on what's legal. The previous policy ("dimensionless
    // types keep GTK's plain numeric behaviour") blocked legitimate math
    // like "360/36" in a rotation field.
    set_numeric(false);

    m_unit_label = Gtk::make_managed<Gtk::Label>("");
    m_unit_label->add_css_class("prop-width-unit");

    apply_unit_params();
    update_unit_label();

    m_adj->signal_value_changed().connect(
        sigc::mem_fun(*this, &CurvzSpinButton::on_value_changed));

    // s219: previously an early-return here gated on parser_enabled
    // skipped the input hook and keystroke filter for dimensionless
    // types. Removed — every type now wires both.

    // ── GTK input hook — the canonical place to override SpinButton parsing.
    // Returning 1 means "I parsed it, use this value". Returning -1 means
    // "parse error, don't commit". Returning 0 lets GTK fall back to strtod
    // (which strips trailing non-numeric — NOT what we want).
    signal_input().connect([this](double &new_value) -> int {
        std::string txt = get_text();
        LOG_INFO("csb::signal_input text='{}'", txt);
        // Trim
        auto first = txt.find_first_not_of(" \t");
        auto last  = txt.find_last_not_of(" \t");
        std::string t = (first == std::string::npos) ? std::string()
                        : txt.substr(first, last - first + 1);
        if (t.empty()) return -1;

        std::string err;
        double display_val = 0.0;

        // s263 m5 — Domain-aware parse path. The Domain enum drives
        // both the legal-suffix set and the result's normalization
        // (Length → px, Angle → degrees, Percentage / Dimensionless
        // → raw). Length-domain results need a from_px conversion
        // back to the field's display unit; other domains return
        // directly in the field's native unit.
        Domain domain = type_domain();
        Unit default_u = type_default_unit();
        double parsed = 0.0;
        if (!UnitSystem::parse_expr(t, domain, default_u, parsed, err)) {
            LOG_INFO("csb::signal_input parse FAIL err='{}'", err);
            show_error_popover(t, err);
            return -1;
        }
        if (domain == Domain::Length) {
            display_val = UnitSystem::from_px(parsed, default_u);
        } else {
            display_val = parsed;
            if (m_type == SpinType::Integer)
                display_val = std::round(display_val);
        }

        double lo = m_adj->get_lower(), hi = m_adj->get_upper();
        if (display_val < lo || display_val > hi) {
            show_error_popover(t, "Value out of range.");
            return -1;
        }

        hide_error_popover();
        new_value = display_val;
        LOG_INFO("csb::signal_input committed display_val={}", display_val);
        return 1;
    }, false);

    // ── Keystroke filter ─────────────────────────────────────────────────
    // Reject disallowed characters before they reach the entry buffer.
    auto kc = Gtk::EventControllerKey::create();
    kc->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    kc->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType mods) -> bool {
            // Always allow control, navigation, backspace, etc.
            if (keyval == GDK_KEY_BackSpace || keyval == GDK_KEY_Delete ||
                keyval == GDK_KEY_Left      || keyval == GDK_KEY_Right ||
                keyval == GDK_KEY_Home      || keyval == GDK_KEY_End ||
                keyval == GDK_KEY_Tab       || keyval == GDK_KEY_ISO_Left_Tab ||
                keyval == GDK_KEY_Return    || keyval == GDK_KEY_KP_Enter ||
                keyval == GDK_KEY_Escape)
                return false;
            // Allow Ctrl/Alt shortcuts through unfiltered
            if ((mods & (Gdk::ModifierType::CONTROL_MASK |
                         Gdk::ModifierType::ALT_MASK)) !=
                Gdk::ModifierType{})
                return false;
            gunichar ch = gdk_keyval_to_unicode(keyval);
            if (ch == 0) return false;
            // s263 m5 — keystroke filter now domain-aware. Angle fields
            // accept d/e/g/r/a; Percentage accepts %; Length accepts
            // i/n/m/p/t/x and the inch quote `"`.
            if (!is_char_allowed(ch, type_domain())) {
                return true; // swallow
            }
            return false;
        },
        false);
    add_controller(kc);
}

// ── Fluent builder ────────────────────────────────────────────────────────────

CurvzSpinButton* CurvzSpinButton::with_value(double internal)
{
    set_internal_value(internal);
    return this;
}

CurvzSpinButton* CurvzSpinButton::with_tooltip(const char* tip)
{
    set_tooltip_text(tip);
    return this;
}

CurvzSpinButton* CurvzSpinButton::with_css(const char* css_class)
{
    add_css_class(css_class);
    return this;
}

CurvzSpinButton* CurvzSpinButton::with_width_chars(int n)
{
    set_width_chars(n);
    return this;
}

CurvzSpinButton* CurvzSpinButton::on_changed(std::function<void(double)> cb)
{
    m_signal_internal_changed.connect([cb](double v) { cb(v); });
    return this;
}

// ── Model / ruler ─────────────────────────────────────────────────────────────

void CurvzSpinButton::set_model(const CanvasModel* model)
{
    m_model = model;
    apply_unit_params();
    update_unit_label();
    m_updating = true;
    m_adj->set_value(to_display(m_internal));
    m_updating = false;
}

void CurvzSpinButton::set_ruler_origin(double origin)
{
    m_ruler_origin = origin;
    m_updating = true;
    m_adj->set_value(to_display(m_internal));
    m_updating = false;
}

// ── Value accessors ───────────────────────────────────────────────────────────

void CurvzSpinButton::set_internal_value(double internal)
{
    m_internal = internal;
    m_updating = true;
    m_adj->set_value(to_display(internal));
    m_updating = false;
}

// ── Unit refresh ─────────────────────────────────────────────────────────────

void CurvzSpinButton::refresh_units()
{
    if (!is_distance_type()) return;
    apply_unit_params();
    update_unit_label();
    m_updating = true;
    m_adj->set_value(to_display(m_internal));
    m_updating = false;
}

// ── Private helpers ───────────────────────────────────────────────────────────

bool CurvzSpinButton::is_distance_type() const
{
    return m_type == SpinType::Distance
        || m_type == SpinType::Width
        || m_type == SpinType::PositionX
        || m_type == SpinType::PositionY;
}

void CurvzSpinButton::apply_unit_params()
{
    switch (m_type) {

    case SpinType::Distance:
    case SpinType::Width:
    case SpinType::PositionX:
    case SpinType::PositionY: {
        Unit u = m_model ? m_model->display_unit : Unit::Px;
        double lo, hi, step, page;
        int    digits;
        // s268: step/page sized for per-click usefulness, not maximum
        // typing precision. The +/- buttons auto-repeat after climb_rate
        // was added, so single-click steps that took 1000+ clicks to
        // move anywhere became immediately user-hostile. New values
        // give a small-but-useful bump per click; users wanting finer
        // precision still type the value or drag on canvas.
        switch (u) {
        case Unit::In:
            hi = 1000.0;   step = 0.01;     page = 0.1;   digits = 6; break;
        case Unit::Mm:
            hi = 25400.0;  step = 0.5;      page = 5.0;   digits = 3; break;
        case Unit::Pt:
            hi = 72000.0;  step = 1.0;      page = 10.0;  digits = 2; break;
        case Unit::Px:
        default:
            hi = 100000.0; step = 1.0;      page = 10.0;  digits = 1; break;
        }
        // Width is always positive; Distance and Position allow negative
        lo = (m_type == SpinType::Width) ? 0.0 : -hi;
        m_adj->configure(m_adj->get_value(), lo, hi, step, page, 0.0);
        set_digits(digits);
        break;
    }

    case SpinType::Angle:
        // s268: 1°/click, 10°/page — degrees are unitless of canvas
        // unit and 0.1°/click required 10 clicks to move a single degree.
        m_adj->configure(m_adj->get_value(), -360.0, 360.0, 1.0, 10.0, 0.0);
        set_digits(1);
        break;

    case SpinType::Percentage:
        // s268: 1%/click, 10%/page — same rationale as Angle.
        m_adj->configure(m_adj->get_value(), 0.0, 100.0, 1.0, 10.0, 0.0);
        set_digits(1);
        break;

    case SpinType::Integer:
        m_adj->configure(m_adj->get_value(), 0.0, 1e6, 1.0, 10.0, 0.0);
        set_digits(0);
        break;
    }
}

void CurvzSpinButton::update_unit_label()
{
    if (!m_unit_label) return;
    if (!is_distance_type()) {
        m_unit_label->set_text("");
        return;
    }
    Unit u = m_model ? m_model->display_unit : Unit::Px;
    m_unit_label->set_text(UnitSystem::label(u));
}

// ── Coordinate conversion ─────────────────────────────────────────────────────
//
// Distance / Width: pure UnitSystem px conversion, no origin or flip.
// PositionX / Y:    delegate to CoordConvert helpers (the canonical seam shared
//                   with the right-click guide dialog and any other doc-space
//                   <-> display-space consumer).

double CurvzSpinButton::to_display(double internal) const
{
    if (!m_model) return internal;

    double result = internal;

    switch (m_type) {

    case SpinType::Distance:
    case SpinType::Width:
        result = px_to_unit(internal, m_model->display_unit, m_model);
        break;

    case SpinType::PositionX:
        result = doc_to_display_x(internal, m_model, m_ruler_origin);
        break;

    case SpinType::PositionY:
        result = doc_to_display_y(internal, m_model, m_ruler_origin);
        break;

    default:
        result = internal;
        break;
    }

    // ── DEBUG: log full context of each conversion.
    LOG_INFO("csb::to_display  type={} mode={} unit={} q={} dpi={} phys_w={} phys_h={} "
             "ruler_origin={} internal={} -> display={}",
             spin_type_name(m_type),
             display_mode_name(m_model),
             UnitSystem::label(m_model->display_unit),
             m_model->quality,
             m_model->dpi,
             m_model->phys_width,
             m_model->phys_height,
             m_ruler_origin,
             internal,
             result);

    return result;
}

double CurvzSpinButton::to_internal(double display) const
{
    if (!m_model) return display;

    double result = display;

    switch (m_type) {

    case SpinType::Distance:
    case SpinType::Width:
        result = unit_to_px(display, m_model->display_unit, m_model);
        break;

    case SpinType::PositionX:
        result = display_to_doc_x(display, m_model, m_ruler_origin);
        break;

    case SpinType::PositionY:
        result = display_to_doc_y(display, m_model, m_ruler_origin);
        break;

    default:
        result = display;
        break;
    }

    // ── DEBUG: log full context of each conversion.
    LOG_INFO("csb::to_internal type={} mode={} unit={} q={} dpi={} phys_w={} phys_h={} "
             "ruler_origin={} display={} -> internal={}",
             spin_type_name(m_type),
             display_mode_name(m_model),
             UnitSystem::label(m_model->display_unit),
             m_model->quality,
             m_model->dpi,
             m_model->phys_width,
             m_model->phys_height,
             m_ruler_origin,
             display,
             result);

    return result;
}

void CurvzSpinButton::on_value_changed()
{
    if (m_updating) return;
    m_internal = to_internal(m_adj->get_value());
    m_signal_internal_changed.emit(m_internal);
}

// ── Expression parsing ───────────────────────────────────────────────────────

bool CurvzSpinButton::type_allows_units() const
{
    return is_distance_type();
}

Unit CurvzSpinButton::type_default_unit() const
{
    if (!type_allows_units()) return Unit::Px;
    return m_model ? m_model->display_unit : Unit::Px;
}

// s263 m5 — map SpinType to parser Domain. The mapping reflects each
// type's intended editing surface:
//   Distance / Width / PositionX / PositionY → Length (in/mm/pt/px)
//   Angle                                    → Angle (deg/rad)
//   Percentage                               → Percentage (%)
//   Integer                                  → Dimensionless (no suffix)
// This is the structural truth that previously hid behind the binary
// type_allows_units() helper. type_allows_units() is now equivalent to
// `type_domain() == Domain::Length`.
Domain CurvzSpinButton::type_domain() const
{
    switch (m_type) {
        case SpinType::Distance:
        case SpinType::Width:
        case SpinType::PositionX:
        case SpinType::PositionY:
            return Domain::Length;
        case SpinType::Angle:
            return Domain::Angle;
        case SpinType::Percentage:
            return Domain::Percentage;
        case SpinType::Integer:
            return Domain::Dimensionless;
    }
    return Domain::Dimensionless;  // unreachable; switch is exhaustive
}

// s263 m5 — domain-aware keystroke filter. Decides which characters
// reach the entry buffer based on what's legal in the field's domain.
// Digits, operators, parens, sign, scientific-e, whitespace are
// always allowed. Suffix letters are allowed per-domain:
//   Length        — `i n m p t x "` (forms in, mm, pt, px and `"`)
//   Angle         — `d e g r a` (forms deg, rad)
//   Percentage    — `%`
//   Dimensionless — none
//
// The previous bool overload (`units_allowed`) is preserved below as a
// thin forwarder: true → Length, false → Dimensionless. Same as the
// parser's compat-wrapper pattern; same backwards-compat shape.
bool CurvzSpinButton::is_char_allowed(gunichar ch, Domain domain)
{
    // Digits
    if (ch >= '0' && ch <= '9') return true;
    // Operators, decimal, parens, sign, scientific-e, whitespace
    if (ch == '+' || ch == '-' || ch == '*' || ch == '/' ||
        ch == '(' || ch == ')' || ch == '.' || ch == ',' ||
        ch == 'e' || ch == 'E' || ch == ' ' || ch == '\t')
        return true;
    switch (domain) {
        case Domain::Length:
            // Letters forming in / mm / pt / px, plus the inch quote.
            if (ch == 'i' || ch == 'n' || ch == 'm' || ch == 'p' ||
                ch == 't' || ch == 'x' || ch == '"')
                return true;
            return false;
        case Domain::Angle:
            // Letters forming deg / rad.
            if (ch == 'd' || ch == 'e' || ch == 'g' ||
                ch == 'r' || ch == 'a')
                return true;
            return false;
        case Domain::Percentage:
            if (ch == '%') return true;
            return false;
        case Domain::Dimensionless:
            return false;
    }
    return false;  // unreachable; switch is exhaustive
}

bool CurvzSpinButton::is_char_allowed(gunichar ch, bool units_allowed)
{
    return is_char_allowed(ch,
        units_allowed ? Domain::Length : Domain::Dimensionless);
}

bool CurvzSpinButton::try_commit_text(const std::string& txt)
{
    LOG_INFO("csb::try_commit_text raw='{}' type={} allows_units={}",
             txt, spin_type_name(m_type), type_allows_units());
    // Trim leading/trailing whitespace
    auto first = txt.find_first_not_of(" \t");
    auto last  = txt.find_last_not_of(" \t");
    std::string t = (first == std::string::npos) ? std::string()
                    : txt.substr(first, last - first + 1);
    if (t.empty()) { LOG_INFO("csb::try_commit_text empty"); return false; }

    double display_val = 0.0;
    std::string err;

    // s263 m5 — Domain-aware parse path. See signal_input's matching
    // block for the same shape. Length-domain results normalize to
    // pixels and need from_px to project back to the field's display
    // unit; other domains return in the field's native unit directly.
    Domain domain = type_domain();
    Unit default_u = type_default_unit();
    double parsed = 0.0;
    if (!UnitSystem::parse_expr(t, domain, default_u, parsed, err)) {
        LOG_INFO("csb::try_commit_text parse FAIL txt='{}' err='{}'", t, err);
        show_error_popover(t, err);
        return false;
    }
    if (domain == Domain::Length) {
        LOG_INFO("csb::try_commit_text parsed screen_px={} default_u={}",
                 parsed, (int)default_u);
        // parse_expr computed using 96-dpi assumption. For Physical mode
        // with a custom dpi, rescale: screen_px represents
        // `value_in_default_unit` converted at 96dpi. We want internal
        // doc px using effective_dpi.
        display_val = UnitSystem::from_px(parsed, default_u);
    } else {
        // Angle / Percentage / Integer — result already in field's
        // native unit (degrees for Angle, raw for the others).
        display_val = parsed;
        if (m_type == SpinType::Integer)
            display_val = std::round(display_val);
    }

    double lo = m_adj->get_lower();
    double hi = m_adj->get_upper();
    if (display_val < lo || display_val > hi) {
        std::string range_msg =
            "Value out of range (" + std::to_string(lo) +
            " to " + std::to_string(hi) + ").";
        show_error_popover(t, range_msg);
        return false;
    }

    hide_error_popover();
    m_adj->set_value(display_val);
    return true;
}

void CurvzSpinButton::show_error_popover(const std::string& bad_input,
                                         const std::string& msg)
{
    if (!m_err_popover) {
        m_err_popover = Gtk::make_managed<Gtk::Popover>();
        m_err_popover->set_parent(*this);
        m_err_popover->set_position(Gtk::PositionType::BOTTOM);
        m_err_popover->set_autohide(false);
        m_err_popover->add_css_class("csb-error");

        auto *box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        box->set_spacing(2);
        box->set_margin(8);
        m_err_label = Gtk::make_managed<Gtk::Label>("");
        m_err_label->set_xalign(0.0f);
        m_err_label->set_max_width_chars(40);
        m_err_label->set_wrap(true);
        box->append(*m_err_label);
        m_err_popover->set_child(*box);
    }
    std::string text = "\u26A0  " + bad_input + "\n" + msg;
    m_err_label->set_text(text);
    m_err_popover->popup();

    // Auto-dismiss after 3s
    m_err_hide_timer.disconnect();
    m_err_hide_timer = Glib::signal_timeout().connect(
        [this]() -> bool {
            hide_error_popover();
            return false;
        },
        3000);
}

void CurvzSpinButton::hide_error_popover()
{
    m_err_hide_timer.disconnect();
    if (m_err_popover) m_err_popover->popdown();
}

} // namespace Curvz
