//
// TextStyleLibrary.cpp — implementation of the named paragraph-text-style
// library. See TextStyleLibrary.hpp for the contract and TextStyle.hpp for the
// data model. Mirrors src/style/StyleLibrary.cpp in structure; the genuinely new
// parts are the seeded default SET (a cascade-demonstrating forest, not three
// flat stubs) and the resolve() inheritance pump.
//

#include "style/TextStyleLibrary.hpp"
#include "SceneNode.hpp"   // generate_internal_id() — GLib UUID v4
#include "CurvzLog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace Curvz {
namespace style {

using nlohmann::json;

// ── Seeded app set ────────────────────────────────────────────────────────────
//
// A macOS-ish handful, NOT LibreOffice's hundred (design §6). Every style except
// the root inherits from app:text-default and stores ONLY its deltas, so the set
// itself demonstrates the cascade: change Default's family and every heading's
// family follows (none override it); each heading pins its own size and weight.
//
// Sizes are document units (matching SceneNode::text_font_size). Leading is left
// UNSET on every seed except where a style genuinely wants a non-metric stride —
// here all leave it unset so leading derives from metrics (tier-3) until the
// leading-floor migration moves the tier-2 scalar onto Default. Category groups
// them under one "Text" header in the panel.

namespace {

constexpr char kCat[] = "Text";

// Root. Pins EVERYTHING — it is the cascade floor's named face. Its values match
// the SceneNode text defaults so a fresh document and a Default-styled paragraph
// look identical before any override.
TextStyle make_default() {
    TextStyle s;
    s.header.id        = "app:text-default";
    s.header.name      = "Default";
    s.header.category  = kCat;
    s.header.parent_id = "";              // root
    s.para.align            = ParaAlign::Left;
    s.para.leading_px       = 0.0;        // derive from metrics
    s.para.indent_left_px   = 0.0;
    s.para.indent_right_px  = 0.0;
    s.para.indent_first_px  = 0.0;
    s.para.tabs             = std::string{};   // no explicit stops
    s.chars.family          = "Sans";
    s.chars.size            = 24.0;
    s.chars.bold            = false;
    s.chars.italic          = false;
    s.chars.letter_spacing  = 0.0;
    s.chars.colour          = color::Solid{color::Color::black()};
    return s;
}

// Helper: a child of Default carrying only the listed deltas.
TextStyle make_child(const char* id, const char* name) {
    TextStyle s;
    s.header.id        = id;
    s.header.name      = name;
    s.header.category  = kCat;
    s.header.parent_id = "app:text-default";
    return s;   // caller fills deltas
}

TextStyle make_title() {
    TextStyle s = make_child("app:text-title", "Title");
    s.chars.size = 56.0;
    s.chars.bold = true;
    return s;
}
TextStyle make_subtitle() {
    TextStyle s = make_child("app:text-subtitle", "Subtitle");
    s.chars.size   = 32.0;
    s.chars.italic = true;
    return s;
}
TextStyle make_heading_1() {
    TextStyle s = make_child("app:text-heading-1", "Heading 1");
    s.chars.size = 40.0;
    s.chars.bold = true;
    return s;
}
TextStyle make_heading_2() {
    TextStyle s = make_child("app:text-heading-2", "Heading 2");
    s.chars.size = 32.0;
    s.chars.bold = true;
    return s;
}
TextStyle make_heading_3() {
    TextStyle s = make_child("app:text-heading-3", "Heading 3");
    s.chars.size = 26.0;
    s.chars.bold = true;
    return s;
}
TextStyle make_quote() {
    TextStyle s = make_child("app:text-quote", "Quote");
    s.chars.italic         = true;
    s.para.indent_left_px  = 32.0;
    s.para.indent_right_px  = 32.0;
    return s;
}
TextStyle make_caption() {
    TextStyle s = make_child("app:text-caption", "Caption");
    s.chars.size           = 18.0;
    // A grey caption colour — distinct enough to read as a byline cue.
    s.chars.colour         = color::Solid{color::Color{0.4, 0.4, 0.4, 1.0}};
    return s;
}

} // namespace

// ── Construction ──────────────────────────────────────────────────────────────

TextStyleLibrary::TextStyleLibrary() {
    m_app_styles.push_back(make_default());      // root first
    m_app_styles.push_back(make_title());
    m_app_styles.push_back(make_subtitle());
    m_app_styles.push_back(make_heading_1());
    m_app_styles.push_back(make_heading_2());
    m_app_styles.push_back(make_heading_3());
    m_app_styles.push_back(make_quote());
    m_app_styles.push_back(make_caption());
    LOG_DEBUG("TextStyleLibrary: constructed with {} app styles, 0 user styles",
              m_app_styles.size());
}

// ── id generation ───────────────────────────────────────────────────────────

TextStyleId TextStyleLibrary::generate_unique_user_id() const {
    for (int attempt = 0; attempt < 2; ++attempt) {
        TextStyleId candidate = "txs_" + generate_internal_id();
        if (find_user(candidate) == m_user_styles.end() &&
            find_app(candidate)  == m_app_styles.end()) {
            return candidate;
        }
    }
    LOG_ERROR("TextStyleLibrary::generate_unique_user_id: 2 consecutive UUID "
              "collisions (user {}, app {}) -- RNG failure?",
              m_user_styles.size(), m_app_styles.size());
    return {};
}

// ── Internal lookups ────────────────────────────────────────────────────────

std::vector<TextStyle>::iterator TextStyleLibrary::find_user(const TextStyleId& id) {
    return std::find_if(m_user_styles.begin(), m_user_styles.end(),
                        [&](const TextStyle& s) { return s.header.id == id; });
}
std::vector<TextStyle>::const_iterator TextStyleLibrary::find_user(const TextStyleId& id) const {
    return std::find_if(m_user_styles.begin(), m_user_styles.end(),
                        [&](const TextStyle& s) { return s.header.id == id; });
}
std::vector<TextStyle>::const_iterator TextStyleLibrary::find_app(const TextStyleId& id) const {
    return std::find_if(m_app_styles.begin(), m_app_styles.end(),
                        [&](const TextStyle& s) { return s.header.id == id; });
}

// ── User CRUD ───────────────────────────────────────────────────────────────

TextStyleId TextStyleLibrary::add_text_style(TextStyle s) {
    if (s.header.id.empty()) {
        s.header.id = generate_unique_user_id();
        if (s.header.id.empty()) return {};
    } else {
        if (is_built_in_text(s.header.id)) {
            LOG_WARN("TextStyleLibrary::add_text_style: refusing 'app:' id '{}' on add",
                     s.header.id);
            return {};
        }
        if (find_user(s.header.id) != m_user_styles.end() ||
            find_app(s.header.id)  != m_app_styles.end()) {
            LOG_WARN("TextStyleLibrary::add_text_style: id '{}' already exists, refusing",
                     s.header.id);
            return {};
        }
    }
    TextStyleId id = s.header.id;
    m_user_styles.push_back(std::move(s));
    m_sig_added.emit(id);
    return id;
}

bool TextStyleLibrary::update_text_style(const TextStyleId& id, TextStyle s) {
    if (is_built_in_text(id)) {
        LOG_WARN("TextStyleLibrary::update_text_style: '{}' is built-in (read-only)", id);
        return false;
    }
    if (s.header.id != id) {
        LOG_WARN("TextStyleLibrary::update_text_style: header.id mismatch ('{}' vs '{}')",
                 s.header.id, id);
        return false;
    }
    auto it = find_user(id);
    if (it == m_user_styles.end()) {
        LOG_WARN("TextStyleLibrary::update_text_style: id '{}' not found in user list", id);
        return false;
    }
    *it = std::move(s);
    m_sig_changed.emit(id);
    return true;
}

bool TextStyleLibrary::remove_text_style(const TextStyleId& id) {
    if (is_built_in_text(id)) {
        LOG_WARN("TextStyleLibrary::remove_text_style: '{}' is built-in (read-only)", id);
        return false;
    }
    auto it = find_user(id);
    if (it == m_user_styles.end()) {
        LOG_WARN("TextStyleLibrary::remove_text_style: id '{}' not found in user list", id);
        return false;
    }
    m_user_styles.erase(it);
    m_sig_removed.emit(id);
    return true;
}

TextStyleId TextStyleLibrary::duplicate_to_user(const TextStyleId& src) {
    const TextStyle* p = find_text_style(src);
    if (!p) {
        LOG_WARN("TextStyleLibrary::duplicate_to_user: id '{}' not found", src);
        return {};
    }
    TextStyle copy = *p;                    // by value -- safe across vector growth
    copy.header.id = generate_unique_user_id();
    if (copy.header.id.empty()) return {};
    // Keep parent_id: a duplicated app style still inherits from the same parent,
    // so the copy starts visually identical to the source and editable.
    if (!copy.header.name.empty()) copy.header.name += " copy";
    TextStyleId id = copy.header.id;
    m_user_styles.push_back(std::move(copy));
    m_sig_added.emit(id);
    return id;
}

// ── Lookup ──────────────────────────────────────────────────────────────────

const TextStyle* TextStyleLibrary::find_text_style(const TextStyleId& id) const {
    if (auto it = find_user(id); it != m_user_styles.end()) return &*it;
    if (auto it = find_app(id);  it != m_app_styles.end())  return &*it;
    return nullptr;
}

bool TextStyleLibrary::is_built_in(const TextStyleId& id) const {
    return find_app(id) != m_app_styles.end();
}

// ── Resolve (the inheritance pump) ────────────────────────────────────────────

void TextStyleLibrary::overlay(ResolvedTextStyle& acc, const TextStyle& s) {
    // Paragraph half.
    if (s.para.align)           acc.align           = *s.para.align;
    if (s.para.leading_px)      acc.leading_px      = *s.para.leading_px;
    if (s.para.indent_left_px)  acc.indent_left_px  = *s.para.indent_left_px;
    if (s.para.indent_right_px) acc.indent_right_px = *s.para.indent_right_px;
    if (s.para.indent_first_px) acc.indent_first_px = *s.para.indent_first_px;
    if (s.para.tabs)            acc.tabs            = *s.para.tabs;
    // Character half.
    if (s.chars.family)         acc.family          = *s.chars.family;
    if (s.chars.size)           acc.size            = *s.chars.size;
    if (s.chars.bold)           acc.bold            = *s.chars.bold;
    if (s.chars.italic)         acc.italic          = *s.chars.italic;
    if (s.chars.letter_spacing) acc.letter_spacing  = *s.chars.letter_spacing;
    if (s.chars.colour)         acc.colour          = *s.chars.colour;
}

ResolvedTextStyle TextStyleLibrary::resolve(const TextStyleId& id) const {
    ResolvedTextStyle out;   // starts at the hardcoded floor (== SceneNode defaults)

    // Collect the chain leaf->root, breaking cycles with a visited set. A missing
    // style (unknown id / dangling parent) simply stops the walk; the floor (or
    // whatever was already accumulated below it) stands.
    std::vector<const TextStyle*> chain;
    std::unordered_set<TextStyleId> visited;
    TextStyleId cur = id;
    while (!cur.empty()) {
        if (!visited.insert(cur).second) {
            LOG_WARN("TextStyleLibrary::resolve: parent cycle detected at '{}', "
                     "stopping walk", cur);
            break;
        }
        const TextStyle* s = find_text_style(cur);
        if (!s) break;             // unknown id / dangling parent -> stop
        chain.push_back(s);
        cur = s->header.parent_id;
    }

    // Overlay root-first so descendants win: chain is leaf->root, so iterate it
    // in reverse.
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        overlay(out, **it);
    }
    return out;
}

// ── Category accessors ──────────────────────────────────────────────────────

std::vector<const TextStyle*>
TextStyleLibrary::app_styles_in_category(const std::string& cat) const {
    std::vector<const TextStyle*> out;
    for (const TextStyle& s : m_app_styles)
        if (s.header.category == cat) out.push_back(&s);
    return out;
}
std::vector<const TextStyle*>
TextStyleLibrary::user_styles_in_category(const std::string& cat) const {
    std::vector<const TextStyle*> out;
    for (const TextStyle& s : m_user_styles)
        if (s.header.category == cat) out.push_back(&s);
    return out;
}
std::vector<std::string> TextStyleLibrary::app_categories() const {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const TextStyle& s : m_app_styles)
        if (seen.insert(s.header.category).second) out.push_back(s.header.category);
    return out;
}
std::vector<std::string> TextStyleLibrary::user_categories() const {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const TextStyle& s : m_user_styles)
        if (seen.insert(s.header.category).second) out.push_back(s.header.category);
    return out;
}

// ── JSON round-trip ───────────────────────────────────────────────────────────
//
// Sparse encoding: an UNSET optional is omitted entirely (no key), so a missing
// key reads back as unset. This is what makes the delta model round-trip — a
// child style writes only the fields it pins. The reader sets an optional only
// when its key is present.

namespace {

// Write a sparse paragraph half. Only set fields appear.
json para_to_json(const ParaFormat& p) {
    json o = json::object();
    if (p.align)           o["align"]        = para_align_to_str(*p.align);
    if (p.leading_px)      o["leading_px"]   = *p.leading_px;
    if (p.indent_left_px)  o["indent_left"]  = *p.indent_left_px;
    if (p.indent_right_px) o["indent_right"] = *p.indent_right_px;
    if (p.indent_first_px) o["indent_first"] = *p.indent_first_px;
    if (p.tabs)            o["tabs"]         = *p.tabs;
    return o;
}

void para_from_json(const json& o, ParaFormat& p) {
    if (!o.is_object()) return;
    if (o.contains("align"))        p.align           = para_align_from_str(o.value("align", std::string{"left"}));
    if (o.contains("leading_px"))   p.leading_px      = o.value("leading_px",   0.0);
    if (o.contains("indent_left"))  p.indent_left_px  = o.value("indent_left",  0.0);
    if (o.contains("indent_right")) p.indent_right_px = o.value("indent_right", 0.0);
    if (o.contains("indent_first")) p.indent_first_px = o.value("indent_first", 0.0);
    if (o.contains("tabs"))         p.tabs            = o.value("tabs", std::string{});
}

json chars_to_json(const CharDefaults& c) {
    json o = json::object();
    if (c.family)         o["family"]         = *c.family;
    if (c.size)           o["size"]           = *c.size;
    if (c.bold)           o["bold"]           = *c.bold;
    if (c.italic)         o["italic"]         = *c.italic;
    if (c.letter_spacing) o["letter_spacing"] = *c.letter_spacing;
    if (c.colour)         o["colour"]         = color::paint_to_json(*c.colour);
    return o;
}

void chars_from_json(const json& o, CharDefaults& c) {
    if (!o.is_object()) return;
    if (o.contains("family"))         c.family         = o.value("family", std::string{"Sans"});
    if (o.contains("size"))           c.size           = o.value("size", 24.0);
    if (o.contains("bold"))           c.bold           = o.value("bold", false);
    if (o.contains("italic"))         c.italic         = o.value("italic", false);
    if (o.contains("letter_spacing")) c.letter_spacing = o.value("letter_spacing", 0.0);
    if (o.contains("colour"))         c.colour         = color::paint_from_json(o["colour"]);
}

} // namespace

json text_style_to_json(const TextStyle& s) {
    json entry;
    entry["header"] = {
        {"id",        s.header.id},
        {"name",      s.header.name},
        {"category",  s.header.category},
        {"parent_id", s.header.parent_id}
    };
    entry["para"]  = para_to_json(s.para);
    entry["chars"] = chars_to_json(s.chars);
    return entry;
}

std::optional<TextStyle> text_style_from_json(const json& entry) {
    if (!entry.is_object()) {
        LOG_WARN("TextStyleLibrary JSON: entry not an object, skipping");
        return std::nullopt;
    }
    TextStyle s;
    if (entry.contains("header") && entry["header"].is_object()) {
        const auto& h = entry["header"];
        s.header.id        = h.value("id",        std::string{});
        s.header.name      = h.value("name",      std::string{});
        s.header.category  = h.value("category",  std::string{});
        s.header.parent_id = h.value("parent_id", std::string{});
    }
    if (s.header.id.empty()) {
        LOG_WARN("TextStyleLibrary JSON: entry missing header.id, skipping");
        return std::nullopt;
    }
    if (is_built_in_text(s.header.id)) {
        LOG_WARN("TextStyleLibrary JSON: refusing 'app:' id '{}' in user tier, "
                 "skipping", s.header.id);
        return std::nullopt;
    }
    if (entry.contains("para"))  para_from_json(entry["para"],   s.para);
    if (entry.contains("chars")) chars_from_json(entry["chars"], s.chars);
    return s;
}

void TextStyleLibrary::to_user_json(json& j) const {
    json arr = json::array();
    for (const TextStyle& s : m_user_styles) arr.push_back(text_style_to_json(s));
    j["text_styles"] = std::move(arr);
}

void TextStyleLibrary::from_user_json(const json& j) {
    m_user_styles.clear();   // atomic replace; no signals during load

    if (!j.contains("text_styles") || !j["text_styles"].is_array()) {
        LOG_INFO("TextStyleLibrary::from_user_json: no 'text_styles' array, "
                 "user tier left empty");
        return;
    }

    std::size_t skipped = 0;
    for (const auto& entry : j["text_styles"]) {
        auto s = text_style_from_json(entry);
        if (!s) { ++skipped; continue; }
        if (find_user(s->header.id) != m_user_styles.end() ||
            find_app(s->header.id)  != m_app_styles.end()) {
            LOG_WARN("TextStyleLibrary::from_user_json: id '{}' collides, skipping",
                     s->header.id);
            ++skipped;
            continue;
        }
        m_user_styles.push_back(std::move(*s));
    }

    if (skipped > 0)
        LOG_INFO("TextStyleLibrary::from_user_json: loaded {} user style(s), skipped {}",
                 m_user_styles.size(), skipped);
    else
        LOG_INFO("TextStyleLibrary::from_user_json: loaded {} user style(s)",
                 m_user_styles.size());
}

} // namespace style
} // namespace Curvz
