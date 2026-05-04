#pragma once
#include "CurvzDocument.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

// ── TemplateLibrary ───────────────────────────────────────────────────────────
// Document templates are a user-global resource, shared across all projects.
// See the M1 comment block for the directory bundle format. This header
// extends M1's read-only scan + save + remove with the management ops needed
// by Manage Templates (M4b):
//   • user default pointer  — single user-chosen "preselect on New Document"
//   • rename/move template  — change slug or migrate between categories
//   • category ops          — create/rename/delete user categories
//
// All mutating operations refuse to touch the system root (/usr/share/...).
// Bundles there are presented alongside user bundles at scan-time but the
// mutating API treats them as read-only.

namespace Curvz {
namespace templates {

// ── Metadata carried in template.json ─────────────────────────────────────────
//
// Proportional rules (grid_divisions / margin_ratio / grid_offset_ratio) are
// applied by load_document() after SVG parse. They describe the *relationship*
// between grid + margin and the canvas width — not absolute values — so they
// scale cleanly across any seed size.
//
//   grid_divisions     — count of grid cells across the short axis (0 = off)
//                        grid_spacing = canvas_short_axis / grid_divisions
//                        Grid is square (same spacing X and Y).
//   margin_ratio       — margin = grid_spacing * margin_ratio  (0 = no margin)
//                        Applied symmetrically to all four sides.
//   grid_offset_ratio  — grid origin offset = grid_spacing * grid_offset_ratio
//                        (so e.g. 0.5 centers a grid cell on the margin line)
//
// For Scott's S63 seed rule: 10 divisions, margin_ratio 0.5, offset_ratio 0.5.
// Seeds with all three zero (the default) get an empty canvas with no grid
// or margins applied — matches pre-S63 template behavior.
struct TemplateMeta {
    std::string name;
    std::string category;
    std::string description;
    std::string author;
    std::string created_utc;

    // Proportional rules (S63). All default 0.0/0 = "do not apply".
    int    grid_divisions    = 0;
    double margin_ratio      = 0.0;
    double grid_offset_ratio = 0.0;
};

// ── Motif tag ────────────────────────────────────────────────────────────────
// Local stand-in for CurvzProject's Motif enum. Kept inside the templates
// namespace so this header doesn't need to include CurvzProject.hpp; callers
// translate from project->motif at the call site.
enum class MotifTag { Dark, Light };

// ── One resolved template on disk ─────────────────────────────────────────────
struct TemplateEntry {
    std::string   bundle_path;
    std::string   svg_path;

    // Thumbnail PNGs, one per motif. Each is the absolute path to the bundle's
    // cached thumb for that motif, or empty if the file doesn't exist on disk
    // yet (lazy-regen will produce it on first viewing in that motif).
    //
    // Legacy bundles written before the motif split shipped only `thumbnail.png`
    // (no suffix). scan() resolves a legacy file into thumb_path_dark and leaves
    // thumb_path_light empty so the regen path will produce a light variant on
    // first view in light mode without disturbing the legacy file.
    std::string   thumb_path_dark;
    std::string   thumb_path_light;

    // Aspect ratio (width / height) parsed cheaply from the SVG's viewBox at
    // scan time. Used by NewDocumentDialog's placeholder draw func to match the
    // canvas shape while a motif's PNG is regenerating in the background. Zero
    // when scan couldn't read the viewBox (corrupt SVG, exotic units) — callers
    // fall back to a 1:1 placeholder.
    double        aspect_ratio = 0.0;

    std::string   slug;
    TemplateMeta  meta;
    bool          is_user = false;
};

// ── Built-in templates (Blank + Default) ──────────────────────────────────────
// Blank and Default are first-class templates (not categories). They live in
// a synthetic system-protected category called "builtin". scan() appends
// pseudo-entries for them with is_user=false and empty bundle_path/svg_path;
// callers detect these via is_builtin()/builtin_kind() and route loading to
// their own seed builders instead of parse_svg_file(). The ⭐ default pointer
// accepts them like any other entry — defaults.json stores "builtin/blank"
// or "builtin/default" and scan() resolves it via the synthesized entries.
//
// Users can drag templates INTO the builtin category (it's a valid drop
// target in Manage Templates), but the category itself cannot be renamed or
// deleted, and the Blank / Default entries cannot be renamed or deleted.

constexpr const char* k_builtin_category        = "builtin";
constexpr const char* k_builtin_blank_slug      = "blank";
constexpr const char* k_builtin_default_slug    = "default";

enum class BuiltinKind { None, Blank, Default };

bool        is_builtin(const TemplateEntry& e);
BuiltinKind builtin_kind(const TemplateEntry& e);

// ── Paths ─────────────────────────────────────────────────────────────────────
std::string user_dir();
std::string system_dir();

// ── Slug helpers ──────────────────────────────────────────────────────────────
std::string slugify(const std::string& name);

// ── Scan ──────────────────────────────────────────────────────────────────────
std::vector<TemplateEntry> scan();
std::vector<std::string>   user_categories();

// Returns the set of category names that exist under the system root. Used
// by Manage Templates to mark these as "protected" — their directories are
// recreated by ensure_user_root() on next scan even if deleted, and the UI
// also disables the delete action for system-seeded category names.
std::vector<std::string>   system_categories();

// ── Save ──────────────────────────────────────────────────────────────────────
// Writes the bundle's SVG, metadata, and BOTH motif thumbnail PNGs eagerly.
// Save is a slow user-initiated operation already; rendering twice doubles
// thumbnail cost but keeps the cache fresh in both motifs from day one.
//
// The four colour pairs (dark workspace+artboard and light workspace+artboard)
// come from the project (CurvzProject::workspace_dark_r/g/b etc.). Passing
// both motifs explicitly means save() doesn't need to know about CurvzProject.
bool save(const CurvzDocument& doc, TemplateMeta meta,
          double dark_workspace_r,  double dark_workspace_g,  double dark_workspace_b,
          double dark_artboard_r,   double dark_artboard_g,   double dark_artboard_b,
          double light_workspace_r, double light_workspace_g, double light_workspace_b,
          double light_artboard_r,  double light_artboard_g,  double light_artboard_b,
          std::string* out_bundle_path = nullptr);
bool user_bundle_exists(const std::string& category, const std::string& name);

// ── Lazy thumbnail regen (motif-aware cache) ─────────────────────────────────
// Returns the absolute path to the bundle's thumbnail PNG for the given motif.
// If the file already exists, returns it immediately (cache hit). If it
// doesn't, renders the thumbnail synchronously from the bundle's SVG using
// the supplied motif colours, writes the PNG to disk, and returns the new
// path.
//
// Returns empty string on failure (SVG parse failed, write failed, etc.) —
// callers should fall back to a procedural placeholder.
//
// Builtin entries (is_builtin) are not cached — returns empty string for them
// since they have no bundle directory and no SVG on disk; the dialog renders
// them procedurally.
//
// Designed to be called from a background worker thread. The function
// performs file I/O and Cairo rendering only — no GTK widget access — so it
// is safe to call off the UI thread. The caller is responsible for posting
// the returned path back to the UI thread for display.
std::string ensure_thumb_for_motif(const TemplateEntry& entry,
                                   MotifTag motif,
                                   double workspace_r, double workspace_g, double workspace_b,
                                   double artboard_r,  double artboard_g,  double artboard_b);

// ── Load ──────────────────────────────────────────────────────────────────────
// Parses the SVG and returns a CurvzDocument. Builtin entries (is_builtin)
// have no SVG on disk; callers must handle them separately with their own
// seed builders. This function returns nullptr for builtin entries.
//
// After parsing, load_document() applies proportional rules from the template's
// metadata (grid_divisions / margin_ratio / grid_offset_ratio) so the returned
// document has grid + margin layers populated relative to its canvas size.
// Templates with all three fields at 0 get no grid/margin injection.
std::unique_ptr<CurvzDocument> load_document(const TemplateEntry& entry);

// Applies proportional grid + margin rules from `meta` onto `doc`. No-op when
// all three rule fields are zero. Safe to call on a doc that already has grid
// or margin layers — will overwrite their config but not duplicate them.
// Exposed so callers (tests, alternate load paths) can opt in directly.
void apply_template_proportions(CurvzDocument& doc, const TemplateMeta& meta);

// ── Remove (user bundles only) ────────────────────────────────────────────────
// If the removed bundle was the current user default, the default pointer
// is cleared automatically.
bool remove(const TemplateEntry& entry);

// ── Rename ────────────────────────────────────────────────────────────────────
// Rename a user template to `new_name`. Updates both the slug (directory
// name) and the name field in template.json. If the new slug collides with
// an existing bundle in the same category, appends a numeric suffix
// (new-name-2, new-name-3, ...). System bundles are refused.
// On success, fills out_bundle_path with the new bundle path if non-null.
bool rename_template(const TemplateEntry& entry,
                     const std::string& new_name,
                     std::string* out_bundle_path = nullptr);

// ── Move ──────────────────────────────────────────────────────────────────────
// Move a user template to a different category. Target category is created
// if it doesn't exist. Name-collision handled by suffix as in rename.
// System bundles are refused.
bool move_template(const TemplateEntry& entry,
                   const std::string& new_category,
                   std::string* out_bundle_path = nullptr);

// ── Categories (user root only) ───────────────────────────────────────────────
// Creates a new empty user category. Returns the final name on disk (may
// differ from the input if a collision forced a suffix). Returns empty
// string on failure. `sanitized` writes the slugified form back if non-null.
std::string create_category(const std::string& raw_name);

// Rename a user category. Refuses if new_name collides with an existing
// category. Returns false on failure.
bool rename_category(const std::string& old_name,
                     const std::string& new_name);

// Recursively delete a user category and all its bundles. The caller is
// responsible for confirming with the user first. Also clears the user
// default if its bundle lived under this category.
bool delete_category(const std::string& name);

// ── User default ──────────────────────────────────────────────────────────────
// Reads ~/.config/curvz/templates/defaults.json. Returns nullopt if the
// file is missing, malformed, or the referenced bundle no longer exists
// (dangling references are treated as unset).
std::optional<TemplateEntry> user_default();

// Writes the default pointer. The entry's category+slug are stored; the
// pointer resolves through scan() on read. Any TemplateEntry (user or
// system) is allowed as a default.
bool set_user_default(const TemplateEntry& entry);

// Removes the default pointer file. Next read returns nullopt and callers
// fall back to the built-in Default tile.
bool clear_user_default();

// ── Seed defaults (S63) ───────────────────────────────────────────────────────
// First-run seeding: populates ~/.config/curvz/templates/ with a curated set
// of print / icons / web / social templates. Gated by a marker file at
// ~/.config/curvz/templates/.seeds_v1 — once written, never re-runs (so user
// deletions are respected). Individual bundles are skipped if they already
// exist; safe to bump the marker version to add new seeds without clobbering
// edited ones. Idempotent; safe to call on every app startup.
void seed_defaults_if_needed();

} // namespace templates
} // namespace Curvz
