#pragma once
#include "SceneNode.hpp"
#include "color/SwatchLibrary.hpp"
#include "style/StyleLibrary.hpp"   // BindStyleCommand / UnbindStyleCommand (S79 m4a)
#include "style/StyleInterop.hpp"   // materialise_from_style on redo (S79 m4a)
#include "theme/ThemeLibrary.hpp"   // AddThemeCommand / RemoveThemeCommand (S103 m2)
#include <functional>
#include <deque>
#include <memory>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <utility>  // s220 m1a: std::pair in RemoveSwatchCommand
#include <vector>

namespace Curvz {

// ── Base command ──────────────────────────────────────────────────────────────
struct CurvzCommand {
    virtual ~CurvzCommand() = default;
    virtual void execute() = 0;
    virtual void undo()    = 0;
    virtual std::string description() const = 0;
    // Return true if undo/redo should NOT clear the canvas node selection.
    // Used for path edits that only mutate geometry, not scene structure.
    virtual bool preserves_selection() const { return false; }
    // s165 m3 — undo-stack scrub support. Return true if this command holds
    // any captured raw pointer to `target`. Used by Canvas::scrub_node_refs
    // → CommandHistory::scrub_command_history to drop commands whose
    // captured SceneNode pointers have been invalidated by a destructive
    // op. Default `false` is safe for commands that capture only by id /
    // unique_ptr / value; commands that hold raw SceneNode* fields override
    // this to declare their references.
    //
    // Note: walking owned children of a captured object (e.g. CompositeCommand
    // walking its steps) is the override's responsibility — base default is
    // correct for leaf commands with no captures.
    virtual bool references(const SceneNode* target) const { (void)target; return false; }
};

// ── AddNodeCommand ────────────────────────────────────────────────────────────
// Adds a SceneNode child to a parent. execute() re-inserts, undo() removes.
struct AddNodeCommand : CurvzCommand {
    SceneNode*                     parent;
    std::unique_ptr<SceneNode>     snapshot; // deep copy

    AddNodeCommand(SceneNode* parent, std::unique_ptr<SceneNode> snap)
        : parent(parent), snapshot(std::move(snap)) {}

    void execute() override {
        parent->children.insert(parent->children.begin(), clone_node(*snapshot));
    }
    void undo() override {
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i) {
            if (ch[i]->id == snapshot->id) {
                ch.erase(ch.begin() + i);
                return;
            }
        }
    }
    std::string description() const override { return "Add object"; }
    bool references(const SceneNode* target) const override {
        return parent == target;
    }
};

// Legacy alias used by Canvas pen/draw tools during transition
using AddObjectCommand = AddNodeCommand;

// ── DeleteObjectCommand ───────────────────────────────────────────────────────
// Removes a SceneNode child from its parent layer.
// execute() erases by id, undo() re-inserts at the original index.
struct DeleteObjectCommand : CurvzCommand {
    SceneNode*                 parent;
    std::unique_ptr<SceneNode> snapshot; // deep copy taken before erase
    int                        index;    // original position in parent->children

    DeleteObjectCommand(SceneNode* parent, std::unique_ptr<SceneNode> snap, int idx)
        : parent(parent), snapshot(std::move(snap)), index(idx) {}

    void execute() override {
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i) {
            if (ch[i]->id == snapshot->id) {
                ch.erase(ch.begin() + i);
                return;
            }
        }
    }
    void undo() override {
        auto& ch = parent->children;
        int ins = std::clamp(index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*snapshot));
    }
    std::string description() const override { return "Delete object"; }
    bool references(const SceneNode* target) const override {
        return parent == target;
    }
};

// ── InsertObjectCommand ───────────────────────────────────────────────────────
// Inserts a SceneNode child into a parent layer at a specific index.
// execute() inserts a clone at `index`, undo() erases by id.
// Inverse of DeleteObjectCommand — used by split so undo removes the new half.
struct InsertObjectCommand : CurvzCommand {
    SceneNode*                 parent;
    std::unique_ptr<SceneNode> snapshot; // deep copy of the object to insert
    int                        index;    // position in parent->children

    InsertObjectCommand(SceneNode* parent, std::unique_ptr<SceneNode> snap, int idx)
        : parent(parent), snapshot(std::move(snap)), index(idx) {}

    void execute() override {
        auto& ch = parent->children;
        int ins = std::clamp(index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*snapshot));
    }
    void undo() override {
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i) {
            if (ch[i]->id == snapshot->id) {
                ch.erase(ch.begin() + i);
                return;
            }
        }
    }
    std::string description() const override { return "Insert object"; }
    bool references(const SceneNode* target) const override {
        return parent == target;
    }
};

// ── CompositeCommand ──────────────────────────────────────────────────────────
// Executes/undoes a sequence of commands as a single undoable unit.
// Commands are executed in order, undone in reverse.
struct CompositeCommand : CurvzCommand {
    std::vector<std::unique_ptr<CurvzCommand>> steps;
    std::string desc;

    CompositeCommand(std::string description) : desc(std::move(description)) {}

    void add(std::unique_ptr<CurvzCommand> cmd) {
        steps.push_back(std::move(cmd));
    }
    void execute() override {
        for (auto& s : steps) s->execute();
    }
    void undo() override {
        for (int i = (int)steps.size()-1; i >= 0; --i) {
            steps[i]->undo();
        }
    }
    std::string description() const override { return desc; }
    bool references(const SceneNode* target) const override {
        for (const auto& s : steps)
            if (s->references(target))
                return true;
        return false;
    }
};

// ── ReplaceNodeCommand ────────────────────────────────────────────────────────
// Swaps one child node for another at the same index in a parent layer.
// Used by text_to_paths_op so Ctrl+Z restores the original text node.
struct ReplaceNodeCommand : CurvzCommand {
    SceneNode*                 parent;
    int                        index;      // position in parent->children
    std::unique_ptr<SceneNode> before;     // deep copy of original node
    std::unique_ptr<SceneNode> after;      // deep copy of replacement node

    ReplaceNodeCommand(SceneNode* parent, int idx,
                       std::unique_ptr<SceneNode> before_snap,
                       std::unique_ptr<SceneNode> after_snap)
        : parent(parent), index(idx)
        , before(std::move(before_snap)), after(std::move(after_snap)) {}

    void execute() override {
        auto& ch = parent->children;
        int ins = std::clamp(index, 0, (int)ch.size());
        // Remove the before node (find by id in case list shifted)
        for (int i = (int)ch.size()-1; i >= 0; --i) {
            if (ch[i]->id == before->id) { ch.erase(ch.begin() + i); break; }
        }
        ch.insert(ch.begin() + ins, clone_node(*after));
    }
    void undo() override {
        auto& ch = parent->children;
        int ins = std::clamp(index, 0, (int)ch.size());
        for (int i = (int)ch.size()-1; i >= 0; --i) {
            if (ch[i]->id == after->id) { ch.erase(ch.begin() + i); break; }
        }
        ch.insert(ch.begin() + ins, clone_node(*before));
    }
    std::string description() const override { return "Convert text to path"; }
    bool references(const SceneNode* target) const override {
        return parent == target;
    }
};

// ── MoveObjectCommand ─────────────────────────────────────────────────────────
// s169 m1 — Migrated from raw-SceneNode* capture to iid + project capture.
//
// Pre-migration this command held a raw `SceneNode* obj` pointer with NO
// `references()` override. If the user moved a text/image node, then deleted
// it (or any destructive op flowed through scrub_node_refs), the s165 m3
// scrub pass couldn't find this command (no override) and would leave it on
// the stack. A subsequent Ctrl+Z then dereferenced freed memory. Real
// hazard, just never tested — every other Stage 1 command has an override.
//
// Post-migration: the command holds an iid + project pointer. Resolution
// happens at execute()/undo() time via curvz::utils::find_by_iid; if the
// node is gone, the command no-ops cleanly. Same shape as EditPathCommand
// and the other s167/s168-migrated commands.
//
// Used for text-anchor and image-origin moves only. Path moves go through
// EditPathCommand. This command's body branches on obj->is_text() /
// obj->is_image() — those checks now run against the resolved live node.
struct CurvzProject;  // forward decl — full include lives in CommandHistory.cpp.
                      // Hoisted above MoveObjectCommand in s169 m1 (was below
                      // EditPathCommand previously); needed here because
                      // MoveObjectCommand also takes CurvzProject*.
struct MoveObjectCommand : CurvzCommand {
    CurvzProject* proj;     // resolution root
    std::string   obj_iid;  // SceneNode::internal_id of the target
    double        before_x, before_y, after_x, after_y;

    MoveObjectCommand(CurvzProject* proj, std::string obj_iid,
                      double bx, double by, double ax, double ay)
        : proj(proj)
        , obj_iid(std::move(obj_iid))
        , before_x(bx), before_y(by)
        , after_x(ax), after_y(ay) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Move object"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── EditPathCommand ───────────────────────────────────────────────────────────
// s167 m1 — Migrated from raw-SceneNode* capture to iid + project capture.
//
// Stores the internal_id (UUID) of the target node plus a CurvzProject*
// for resolution. At execute()/undo() time, the iid is looked up via
// curvz::utils::find_by_iid; if the node has been destroyed since
// capture, the resolver returns nullptr and the command no-ops.
//
// This replaces the s165 m3 `references()`-override pattern: the
// dangling-pointer class is now impossible by construction. Whatever
// happens to the tree between capture and execute, the worst the
// command can do is no-op.
//
// Bodies live in CommandHistory.cpp so this header doesn't need the
// full CurvzProject type (forward-declared above MoveObjectCommand
// since s169 m1).

struct EditPathCommand : CurvzCommand {
    CurvzProject* proj;     // resolution root; lifetime guaranteed by
                            // close-project resetting m_history before
                            // m_project drops
    std::string   obj_iid;  // SceneNode::internal_id of the target
    PathData      before;
    PathData      after;
    std::string   desc;

    EditPathCommand(CurvzProject* proj, std::string obj_iid,
                    PathData before, PathData after,
                    std::string desc = "Edit path")
        : proj(proj)
        , obj_iid(std::move(obj_iid))
        , before(std::move(before))
        , after(std::move(after))
        , desc(std::move(desc)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return desc; }
    bool preserves_selection() const override { return true; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── TextEditCommand ───────────────────────────────────────────────────────────
// Full snapshot of all text node fields — used by commit_text_edit for both
// new nodes and edits to existing ones. Undo restores the complete before state.
//
// s167 m2: migrated from raw SceneNode* obj to CurvzProject* proj +
// std::string obj_iid. Bodies move to CommandHistory.cpp; the static
// snapshot_before factory now takes the project pointer alongside the
// node, and reads internal_id off the node to capture identity. The
// `record_after(SceneNode*)` and `apply(SceneNode*, bool)` helpers
// keep their SceneNode* parameter — they're called with a node already
// in hand (record_after by commit_text_edit, apply internally by
// execute/undo after the resolver runs), so they don't need to do
// their own lookup.
//
// The default constructor remains for Canvas's m_text_snapshot member
// storage. A default-constructed TextEditCommand has proj=nullptr and
// empty obj_iid, so accidental execute() / undo() on it is a clean
// no-op via the empty-iid early return.
struct TextEditCommand : CurvzCommand {
    CurvzProject* proj = nullptr;
    std::string   obj_iid;
    // before state
    std::string before_content, before_family, before_anchor, before_align;
    double      before_x = 0, before_y = 0, before_size = 0;
    double      before_line_height = 0;  // s326 m2b
    double      before_baseline_angle = 0;  // s327 m2
    bool        before_bold = false, before_italic = false;
    FillStyle   before_fill;
    StrokeStyle before_stroke;
    // s298 m3 (A2): text-on-path triple + baseline_shift + letter_spacing.
    // Previously snapshot_before/record_after/apply walked only the
    // typographic basics (content/family/anchor/align/x/y/size/bold/
    // italic/fill/stroke). text_path_id, text_path_offset, text_path_flip,
    // text_baseline_shift, text_letter_spacing were all silently
    // unobserved — meaning a content edit that ran during text-on-path
    // mode (or after a slide-along-path) would roll back the typographic
    // basics on undo while leaving the path attachment / offset in their
    // post-edit state, producing the "history desync" symptom logged
    // against B1 in text_on_path_redesign.md (recon finding 1). Adding
    // them here makes TextEditCommand a faithful round-trip across the
    // full text-node state surface. Default-init values are the
    // "unlinked text" baseline — empty id, zero offset, no flip, zero
    // shift, zero spacing — so a snapshot of a plain text node looks
    // the same as before this change and the field additions are a
    // pure superset.
    std::string before_text_path_id;
    double      before_text_path_offset = 0.0;
    bool        before_text_path_flip = false;
    double      before_baseline_shift = 0.0;
    double      before_letter_spacing = 0.0;
    // s301 m1a — text container model snapshot fields. Empty list / empty id /
    // zero margins are the unbound-legacy baseline so a TextEditCommand
    // snapshot of a pre-s301 text node looks identical to before this change.
    std::vector<std::string> before_boundary_ids;
    std::string before_line_path_id;
    double      before_margin_top    = 0.0;
    double      before_margin_bottom = 0.0;
    double      before_margin_left   = 0.0;
    double      before_margin_right  = 0.0;
    // s326 m1 — per-run formatting spine carrier. The s325 spine put
    //   text_attr_spans (the flat AttrSpan list mirroring the Pango markup)
    //   on the Mgr but did NOT thread it through the command, correct at the
    //   time because nothing edited the spans. The styler (m2+) mutates them
    //   on every B/I/U/colour/size apply, so undo/redo must round-trip the
    //   list or a formatting edit rolls back content while leaving the spans
    //   in their post-edit state (the same desync class s298 fixed for the
    //   text-on-path triple). AttrSpan is a plain value type, so the snapshot
    //   is a free vector copy. Empty list is the pre-s326 baseline — a plain
    //   box snapshots an empty list, identical to before this field existed.
    std::vector<AttrSpan> before_attr_spans;
    // s307 m6 — Caret position captured alongside the content snapshot.
    //   Mid-edit Ctrl+Z and Ctrl+Shift+Z need to restore the cursor's
    //   caret to where it was at segment open / segment close,
    //   otherwise undo lands the caret in a stale post-typing
    //   position. The fields default to 0 so a TextEditCommand for a
    //   pre-s307 SVG load (no cursor active) snapshots a sensible
    //   start-of-buffer position. snapshot_before and record_after
    //   take the live caret byte as an additional argument now;
    //   apply() does NOT touch the caret because the live cursor
    //   lives on Canvas, not on the SceneNode — the caller in
    //   handle_text_edit_key restores the caret explicitly after
    //   apply() runs.
    size_t      before_caret_byte = 0;
    size_t      after_caret_byte  = 0;
    // after state
    std::string after_content, after_family, after_anchor, after_align;
    double      after_x = 0, after_y = 0, after_size = 0;
    double      after_line_height = 0;   // s326 m2b
    double      after_baseline_angle = 0;   // s327 m2
    bool        after_bold = false, after_italic = false;
    FillStyle   after_fill;
    StrokeStyle after_stroke;
    std::string after_text_path_id;
    double      after_text_path_offset = 0.0;
    bool        after_text_path_flip = false;
    double      after_baseline_shift = 0.0;
    double      after_letter_spacing = 0.0;
    // s301 m1a — text container model after-state fields.
    std::vector<std::string> after_boundary_ids;
    std::string after_line_path_id;
    double      after_margin_top    = 0.0;
    double      after_margin_bottom = 0.0;
    double      after_margin_left   = 0.0;
    double      after_margin_right  = 0.0;
    // s326 m1 — per-run formatting spine carrier (after-state half).
    std::vector<AttrSpan> after_attr_spans;

    static TextEditCommand snapshot_before(CurvzProject* proj, SceneNode* o) {
        TextEditCommand c;
        c.proj           = proj;
        c.obj_iid        = o ? o->internal_id : std::string{};
        if (!o) return c;
        c.before_content = o->text_content;
        c.before_family  = o->text_font_family;
        c.before_anchor  = o->text_anchor;
        c.before_align   = o->text_align;
        c.before_x       = o->text_x;
        c.before_y       = o->text_y;
        c.before_size    = o->text_font_size;
        c.before_line_height = o->text_line_height;  // s326 m2b
        c.before_baseline_angle = o->text_baseline_angle;  // s327 m2
        c.before_bold    = o->text_bold;
        c.before_italic  = o->text_italic;
        c.before_fill    = o->fill;
        c.before_stroke  = o->stroke;
        // s298 m3 (A2)
        c.before_text_path_id     = o->text_path_id;
        c.before_text_path_offset = o->text_path_offset;
        c.before_text_path_flip   = o->text_path_flip;
        c.before_baseline_shift   = o->text_baseline_shift;
        c.before_letter_spacing   = o->text_letter_spacing;
        // s301 m1a — text container model
        c.before_boundary_ids  = o->text_boundary_ids;
        c.before_line_path_id  = o->text_line_path_id;
        c.before_margin_top    = o->text_margin_top;
        c.before_margin_bottom = o->text_margin_bottom;
        c.before_margin_left   = o->text_margin_left;
        c.before_margin_right  = o->text_margin_right;
        // s326 m1 — per-run formatting spine
        c.before_attr_spans    = o->text_attr_spans;
        // s307 m6 — Caret position. Reads off text_caret_byte, which
        //   the caller is responsible for keeping current: at edit
        //   entry the persisted byte is correct (last edit's caret);
        //   at flush time the caller writes m_text_cursor->byte_index()
        //   into text_caret_byte before calling snapshot_before so
        //   this read picks up the live cursor position.
        c.before_caret_byte = (o->text_caret_byte > 0)
                                  ? (size_t)o->text_caret_byte
                                  : 0;
        return c;
    }
    void record_after(SceneNode* o) {
        if (!o) return;
        after_content = o->text_content;
        after_family  = o->text_font_family;
        after_anchor  = o->text_anchor;
        after_align   = o->text_align;
        after_x       = o->text_x;
        after_y       = o->text_y;
        after_size    = o->text_font_size;
        after_line_height = o->text_line_height;  // s326 m2b
        after_baseline_angle = o->text_baseline_angle;  // s327 m2
        after_bold    = o->text_bold;
        after_italic  = o->text_italic;
        after_fill    = o->fill;
        after_stroke  = o->stroke;
        // s298 m3 (A2)
        after_text_path_id     = o->text_path_id;
        after_text_path_offset = o->text_path_offset;
        after_text_path_flip   = o->text_path_flip;
        after_baseline_shift   = o->text_baseline_shift;
        after_letter_spacing   = o->text_letter_spacing;
        // s301 m1a — text container model
        after_boundary_ids  = o->text_boundary_ids;
        after_line_path_id  = o->text_line_path_id;
        after_margin_top    = o->text_margin_top;
        after_margin_bottom = o->text_margin_bottom;
        after_margin_left   = o->text_margin_left;
        after_margin_right  = o->text_margin_right;
        // s326 m1 — per-run formatting spine
        after_attr_spans    = o->text_attr_spans;
        // s307 m6 — Caret position; same convention as snapshot_before.
        //   Caller writes m_text_cursor->byte_index() into
        //   text_caret_byte before calling record_after.
        after_caret_byte = (o->text_caret_byte > 0)
                                ? (size_t)o->text_caret_byte
                                : 0;
    }
    void apply(SceneNode* o, bool after) const {
        if (!o) return;
        o->text_content    = after ? after_content : before_content;
        o->text_font_family= after ? after_family  : before_family;
        o->text_anchor     = after ? after_anchor  : before_anchor;
        o->text_align      = after ? after_align   : before_align;
        o->text_x          = after ? after_x       : before_x;
        o->text_y          = after ? after_y       : before_y;
        o->text_font_size  = after ? after_size    : before_size;
        o->text_line_height= after ? after_line_height : before_line_height; // s326 m2b
        o->text_baseline_angle = after ? after_baseline_angle : before_baseline_angle; // s327 m2
        o->text_bold       = after ? after_bold    : before_bold;
        o->text_italic     = after ? after_italic  : before_italic;
        o->fill            = after ? after_fill    : before_fill;
        o->stroke          = after ? after_stroke  : before_stroke;
        // s298 m3 (A2)
        o->text_path_id       = after ? after_text_path_id     : before_text_path_id;
        o->text_path_offset   = after ? after_text_path_offset : before_text_path_offset;
        o->text_path_flip     = after ? after_text_path_flip   : before_text_path_flip;
        o->text_baseline_shift= after ? after_baseline_shift   : before_baseline_shift;
        o->text_letter_spacing= after ? after_letter_spacing   : before_letter_spacing;
        // s301 m1a — text container model
        o->text_boundary_ids  = after ? after_boundary_ids  : before_boundary_ids;
        o->text_line_path_id  = after ? after_line_path_id  : before_line_path_id;
        o->text_margin_top    = after ? after_margin_top    : before_margin_top;
        o->text_margin_bottom = after ? after_margin_bottom : before_margin_bottom;
        o->text_margin_left   = after ? after_margin_left   : before_margin_left;
        o->text_margin_right  = after ? after_margin_right  : before_margin_right;
        // s326 m1 — per-run formatting spine
        o->text_attr_spans    = after ? after_attr_spans    : before_attr_spans;
    }
    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Edit text"; }

    TextEditCommand() = default;  // needed for Canvas member storage
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── EditAppearanceCommand ─────────────────────────────────────────────────────
// S82 m4f: swatch-id capture. Inspector / eyedropper / broadcast writes
// route through style::mutate_appearance, which (as of S82) clears both
// bound_style AND *_swatch_id on direct override. Restoring on undo
// requires the full pre-edit binding state, not just the colour cache —
// otherwise undo restores the colour but leaves the binding dangling
// (the latent bug). Eight fields total: fill / stroke / fill_swatch_id /
// stroke_swatch_id, before and after. The legacy 4-arg constructor is
// retained for callers that don't carry swatch state (none in-tree as
// of S82, but cheap to keep for source compatibility).
//
// s167 m2: migrated from raw SceneNode* obj to CurvzProject* proj +
// std::string obj_iid. Bodies move to CommandHistory.cpp so this
// header doesn't need full CurvzProject. Same shape as EditPathCommand
// (s167 m1) — pure data edit, no structural mutation, resolves at
// execute/undo time and no-ops if the target node is gone.
struct EditAppearanceCommand : CurvzCommand {
    CurvzProject* proj;
    std::string   obj_iid;
    FillStyle   fill_before,   fill_after;
    StrokeStyle stroke_before, stroke_after;
    std::string fill_swatch_id_before,   fill_swatch_id_after;
    std::string stroke_swatch_id_before, stroke_swatch_id_after;
    // S92 m3: bound_style capture. Mirrors EditObjectCommand. Sibling
    // broadcast (broadcast_appearance_to_siblings) routes through
    // style::mutate_appearance which clears bound_style on every
    // sibling target — undo needs the pre-edit binding to round-trip
    // correctly.
    std::string bound_style_before,       bound_style_after;
    std::string desc;

    EditAppearanceCommand(CurvzProject* proj, std::string obj_iid,
                          FillStyle fb, StrokeStyle sb,
                          FillStyle fa, StrokeStyle sa,
                          std::string fsib, std::string ssib,
                          std::string fsia, std::string ssia,
                          std::string bsb,  std::string bsa,
                          std::string desc = "Edit appearance")
        : proj(proj), obj_iid(std::move(obj_iid))
        , fill_before(std::move(fb)), stroke_before(std::move(sb))
        , fill_after(std::move(fa)),  stroke_after(std::move(sa))
        , fill_swatch_id_before(std::move(fsib))
        , fill_swatch_id_after(std::move(fsia))
        , stroke_swatch_id_before(std::move(ssib))
        , stroke_swatch_id_after(std::move(ssia))
        , bound_style_before(std::move(bsb))
        , bound_style_after(std::move(bsa))
        , desc(std::move(desc)) {}

    // S82 8-arg constructor — pre-S92 callers that captured swatch ids
    // but not bound_style. Equivalent to empty bound_style on both
    // ends.
    EditAppearanceCommand(CurvzProject* proj, std::string obj_iid,
                          FillStyle fb, StrokeStyle sb,
                          FillStyle fa, StrokeStyle sa,
                          std::string fsib, std::string ssib,
                          std::string fsia, std::string ssia,
                          std::string desc = "Edit appearance")
        : EditAppearanceCommand(proj, std::move(obj_iid),
                                std::move(fb), std::move(sb),
                                std::move(fa), std::move(sa),
                                std::move(fsib), std::move(ssib),
                                std::move(fsia), std::move(ssia),
                                std::string{}, std::string{},
                                std::move(desc)) {}

    // Legacy 4-arg constructor — pre-S82 callers that don't track swatch
    // bindings. Equivalent to passing empty swatch ids on both ends.
    // Safe because callers in this category never bind/unbind via swatch.
    EditAppearanceCommand(CurvzProject* proj, std::string obj_iid,
                          FillStyle fb, StrokeStyle sb,
                          FillStyle fa, StrokeStyle sa,
                          std::string desc = "Edit appearance")
        : EditAppearanceCommand(proj, std::move(obj_iid),
                                std::move(fb), std::move(sb),
                                std::move(fa), std::move(sa),
                                std::string{}, std::string{},
                                std::string{}, std::string{},
                                std::string{}, std::string{},
                                std::move(desc)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return desc; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── EditObjectCommand ─────────────────────────────────────────────────────────
// Combined snapshot of path data + fill + stroke for inspector edits.
// One command covers all inspector changes — no need to track which field changed.
//
// S82 m4f: swatch-id capture extended here too. push_inspector_command in
// PropertiesPanel coalesces inspector edits into ONE EditObjectCommand per
// object per 1500ms window — that's the command that fires on every hex
// entry, type toggle, and stroke-width change for the primary. Without
// swatch_id capture, undoing a hex edit on a swatch-bound object restores
// the colour but leaves the binding empty (because the funnel cleared it
// during the edit). Eight fields, legacy 6-arg constructor preserved.
//
// s167 m2: migrated from raw SceneNode* obj to CurvzProject* proj +
// std::string obj_iid. Bodies move to CommandHistory.cpp. Coalescing
// dynamic_cast site at PropertiesPanel.cpp:381 (push_inspector_command's
// same-window branch) updated to compare cmd->obj_iid against
// obj->internal_id.
struct EditObjectCommand : CurvzCommand {
    CurvzProject* proj;
    std::string   obj_iid;
    PathData    path_before,   path_after;
    FillStyle   fill_before,   fill_after;
    StrokeStyle stroke_before, stroke_after;
    std::string fill_swatch_id_before,   fill_swatch_id_after;
    std::string stroke_swatch_id_before, stroke_swatch_id_after;
    // S92 m3: bound_style capture. Mirrors the S82 m4f swatch_id capture.
    // Inspector edits that route through style::mutate_appearance() now
    // clear bound_style as a break-on-override side effect; without
    // capturing the pre-edit value, undo restores fill/stroke but loses
    // the Style binding. The before/after pair below makes type-toggle,
    // colour-change, gradient-apply, and any other funneled inspector
    // edit fully round-trip through the undo stack including the binding.
    std::string bound_style_before,       bound_style_after;
    // S97 m3: drop shadow capture. Same shape as fill/stroke — bundled
    // as ShadowParams so the nine field flat-list collapses to one
    // member-wise copy via SceneNode::write_shadow on execute/undo.
    // Default-constructed ShadowParams matches a default unshadowed
    // SceneNode, so legacy 6/12/14-arg constructors below can omit it
    // and shadow will undo as a no-op for callers that don't touch it.
    ShadowParams shadow_before,           shadow_after;
    std::string desc;

    EditObjectCommand(CurvzProject* proj, std::string obj_iid,
                      PathData pb, FillStyle fb, StrokeStyle sb,
                      PathData pa, FillStyle fa, StrokeStyle sa,
                      std::string fsib, std::string ssib,
                      std::string fsia, std::string ssia,
                      std::string bsb,  std::string bsa,
                      ShadowParams shb, ShadowParams sha,
                      std::string desc = "Edit object")
        : proj(proj), obj_iid(std::move(obj_iid))
        , path_before(std::move(pb)), fill_before(std::move(fb)), stroke_before(std::move(sb))
        , path_after(std::move(pa)),  fill_after(std::move(fa)),  stroke_after(std::move(sa))
        , fill_swatch_id_before(std::move(fsib))
        , fill_swatch_id_after(std::move(fsia))
        , stroke_swatch_id_before(std::move(ssib))
        , stroke_swatch_id_after(std::move(ssia))
        , bound_style_before(std::move(bsb))
        , bound_style_after(std::move(bsa))
        , shadow_before(shb), shadow_after(sha)
        , desc(std::move(desc)) {}

    // S92 14-arg constructor — pre-S97 source compatibility (callers
    // that captured bound_style but not shadow). Equivalent to default-
    // ShadowParams on both ends → undo is a no-op for shadow.
    EditObjectCommand(CurvzProject* proj, std::string obj_iid,
                      PathData pb, FillStyle fb, StrokeStyle sb,
                      PathData pa, FillStyle fa, StrokeStyle sa,
                      std::string fsib, std::string ssib,
                      std::string fsia, std::string ssia,
                      std::string bsb,  std::string bsa,
                      std::string desc = "Edit object")
        : EditObjectCommand(proj, std::move(obj_iid),
                            std::move(pb), std::move(fb), std::move(sb),
                            std::move(pa), std::move(fa), std::move(sa),
                            std::move(fsib), std::move(ssib),
                            std::move(fsia), std::move(ssia),
                            std::move(bsb), std::move(bsa),
                            ShadowParams{}, ShadowParams{},
                            std::move(desc)) {}

    // S82 12-arg constructor — pre-S92 source compatibility (callers
    // that captured swatch ids but not bound_style). Equivalent to
    // empty bound_style on both ends, which is the right default for
    // call sites that don't touch bound_style.
    EditObjectCommand(CurvzProject* proj, std::string obj_iid,
                      PathData pb, FillStyle fb, StrokeStyle sb,
                      PathData pa, FillStyle fa, StrokeStyle sa,
                      std::string fsib, std::string ssib,
                      std::string fsia, std::string ssia,
                      std::string desc = "Edit object")
        : EditObjectCommand(proj, std::move(obj_iid),
                            std::move(pb), std::move(fb), std::move(sb),
                            std::move(pa), std::move(fa), std::move(sa),
                            std::move(fsib), std::move(ssib),
                            std::move(fsia), std::move(ssia),
                            std::string{}, std::string{},
                            std::move(desc)) {}

    // Legacy 6-arg constructor — pre-S82 source compatibility.
    EditObjectCommand(CurvzProject* proj, std::string obj_iid,
                      PathData pb, FillStyle fb, StrokeStyle sb,
                      PathData pa, FillStyle fa, StrokeStyle sa,
                      std::string desc = "Edit object")
        : EditObjectCommand(proj, std::move(obj_iid),
                            std::move(pb), std::move(fb), std::move(sb),
                            std::move(pa), std::move(fa), std::move(sa),
                            std::string{}, std::string{},
                            std::string{}, std::string{},
                            std::string{}, std::string{},
                            std::move(desc)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return desc; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};


// ── SplitPathCommand ──────────────────────────────────────────────────────────
// Undo: removes the two split children, re-inserts the original.
// Redo: removes the original, re-inserts the two halves at the same position.
struct SplitPathCommand : CurvzCommand {
    SceneNode*                 parent;
    std::unique_ptr<SceneNode> original;   // deep copy of pre-split object
    int                        orig_index; // position in parent->children
    std::unique_ptr<SceneNode> left_snap;  // deep copy of left half
    std::unique_ptr<SceneNode> right_snap; // deep copy of right half
    std::string                left_id;
    std::string                right_id;

    SplitPathCommand(SceneNode* parent,
                     std::unique_ptr<SceneNode> orig, int orig_idx,
                     std::unique_ptr<SceneNode> left,
                     std::unique_ptr<SceneNode> right)
        : parent(parent)
        , original(std::move(orig)), orig_index(orig_idx)
        , left_snap(std::move(left)), right_snap(std::move(right))
        , left_id(left_snap->id), right_id(right_snap->id) {}

    void execute() override {
        // Remove original, insert two halves at orig_index
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i]->id == original->id) { ch.erase(ch.begin()+i); break; }
        int ins = std::clamp(orig_index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins,     clone_node(*left_snap));
        ch.insert(ch.begin() + ins + 1, clone_node(*right_snap));
    }
    void undo() override {
        // Remove the two halves, re-insert original
        auto& ch = parent->children;
        int left_pos = -1;
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i]->id == right_id) { ch.erase(ch.begin()+i); break; }
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i]->id == left_id)  { left_pos = i; ch.erase(ch.begin()+i); break; }
        int ins = std::clamp(left_pos >= 0 ? left_pos : orig_index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*original));
    }
    std::string description() const override { return "Split path"; }
};

// ── ScaleImageCommand ────────────────────────────────────────────────────────
// s170 m4 — Migrated from raw-SceneNode* capture to iid + project capture.
// Same multi-target pattern as Align (s169 m5), Scale (s169 m6), and
// SetOpacity (s170 m1): per-Snap iid resolved at execute()/undo() time.
// If a node is gone, that entry no-ops while the others still apply.
//
// Pre-migration this command held a raw `SceneNode* obj` per Snap with
// NO `references()` override. Same shape of latent hazard as the rest
// of Stage 3.
//
// Undo/redo for scale/rotate/skew applied to an Image node.
// Stores before/after image geometry + transform matrix.
struct ScaleImageCommand : CurvzCommand {
    struct Snap {
        std::string obj_iid;
        double bef_x, bef_y, bef_w, bef_h;
        Transform  bef_t;
        double aft_x, aft_y, aft_w, aft_h;
        Transform  aft_t;
    };
    CurvzProject*     proj;
    std::vector<Snap> snaps;
    std::string       desc;

    ScaleImageCommand(CurvzProject* proj, std::vector<Snap> s,
                      std::string d = "Transform image")
        : proj(proj), snaps(std::move(s)), desc(std::move(d)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return desc; }
    bool preserves_selection() const override { return true; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── ScaleObjectsCommand ───────────────────────────────────────────────────────
// s169 m6 — Migrated from raw-SceneNode* capture to iid + project capture.
// Same multi-target pattern as Align (s169 m5) and CornerTreatment (s169 m4).
// Used by: scale, rotate, skew, flip-path, multi-node drag, inspector
// transforms (PropertiesPanel push_leaves helper). Eight push sites across
// Canvas_ops, Canvas_input, and PropertiesPanel — all changed in this ship.
struct ScaleObjectsCommand : CurvzCommand {
    struct LeafSnap {
        std::string obj_iid;
        PathData    before;
        PathData    after;
    };
    CurvzProject*         proj;
    std::vector<LeafSnap> snaps;
    std::string           desc;

    ScaleObjectsCommand(CurvzProject* proj, std::vector<LeafSnap> snaps,
                        std::string desc = "Scale object")
        : proj(proj), snaps(std::move(snaps)), desc(std::move(desc)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return desc; }
    bool preserves_selection() const override { return true; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── AlignObjectsCommand ───────────────────────────────────────────────────────
// s169 m5 — Migrated from raw-SceneNode* capture to iid + project capture.
// Same multi-target pattern as CornerTreatment (s169 m4): per-LeafSnap iid
// resolved at execute()/undo() time. If a leaf is gone, that entry no-ops
// while the others still apply.
struct AlignObjectsCommand : CurvzCommand {
    struct LeafSnap {
        std::string obj_iid;
        PathData    before;
        PathData    after;
    };
    CurvzProject*         proj;
    std::vector<LeafSnap> snaps;
    std::string           desc;

    AlignObjectsCommand(CurvzProject* proj, std::vector<LeafSnap> snaps,
                        std::string desc = "Align objects")
        : proj(proj), snaps(std::move(snaps)), desc(std::move(desc)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return desc; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── SetOpacityCommand ─────────────────────────────────────────────────────────
// s170 m1 — Migrated from raw-SceneNode* capture to iid + project capture.
// Same multi-target pattern as Align (s169 m5) and CornerTreatment
// (s169 m4): per-Snap iid resolved at execute()/undo() time. If a node
// is gone, that entry no-ops while the others still apply.
//
// Pre-migration this command held a raw `SceneNode* obj` per Snap with
// NO `references()` override — the existing `if (s.obj)` guard inside
// execute()/undo() was a band-aid against a dangling pointer that scrub
// could not clear. Same shape of latent hazard as the rest of Stage 3.
//
// Records a per-target before/after opacity snapshot for every node touched
// by Canvas::apply_opacity_to_selection — including the children that get
// flattened to 1.0 when a group-like is in the selection. Undo restores the
// full pre-edit state (group + members) atomically.
//
// One Snap per affected node. The caller (apply_opacity_to_selection) is
// responsible for deduping by iid when a node could be visited twice
// (e.g. shift-selecting both a group and one of its members) — first-touch
// wins, so the snap holds the genuine pre-edit opacity rather than an
// already-mutated value, and undo round-trips cleanly.
struct SetOpacityCommand : CurvzCommand {
    struct Snap {
        std::string obj_iid;
        double      before;
        double      after;
    };
    CurvzProject*     proj;
    std::vector<Snap> snaps;
    std::string       desc;

    SetOpacityCommand(CurvzProject* proj, std::vector<Snap> snaps,
                      std::string desc = "Set opacity")
        : proj(proj), snaps(std::move(snaps)), desc(std::move(desc)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return desc; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── ZOrderCommand ─────────────────────────────────────────────────────────────
// s169 m2 — Migrated from raw-SceneNode* capture to iid + project capture.
//
// Pre-migration this command held a raw `SceneNode* parent` pointer with
// NO `references()` override. If the parent layer was destroyed between
// push and undo (rare, but possible: delete-layer-with-undo-still-pending),
// scrub_command_history would not find this command and Ctrl+Z would
// dereference freed memory. Same shape of latent hazard as MoveObject in
// s169 m1 — all of Stage 3 is missing `references()` overrides and was
// silently exposed.
//
// Post-migration: command holds an iid + project pointer. Resolution
// happens at execute()/undo() time via curvz::utils::find_by_iid; if the
// layer is gone, command no-ops cleanly. The `entries` / `before_order`
// / `after_order` fields hold strings (SVG ids), no pointer refs in them.
//
// `apply_order` and `move_child` remain static helpers taking `SceneNode*`
// — they're called with already-resolved live pointers from execute()/
// undo() bodies, so they don't need to do their own lookup.
struct ZOrderCommand : CurvzCommand {
    struct Entry { std::string id; int before_idx; int after_idx; };

    CurvzProject*           proj;        // resolution root
    std::string             parent_iid;  // SceneNode::internal_id of the layer
    std::vector<Entry>      entries;     // one per selected object (informational)
    std::vector<std::string> before_order; // child ids in order before the op
    std::vector<std::string> after_order;  // child ids in order after the op
    std::string             desc;

    ZOrderCommand(CurvzProject* proj, std::string parent_iid,
                  std::vector<Entry> entries,
                  std::vector<std::string> before_order,
                  std::vector<std::string> after_order,
                  std::string desc = "Arrange")
        : proj(proj)
        , parent_iid(std::move(parent_iid))
        , entries(std::move(entries))
        , before_order(std::move(before_order))
        , after_order(std::move(after_order))
        , desc(std::move(desc)) {}

    // Reorder parent->children to match the given id order
    static void apply_order(SceneNode* p, const std::vector<std::string>& id_order) {
        auto& ch = p->children;
        if (ch.size() != id_order.size()) return;
        // Build id→ptr map
        std::unordered_map<std::string, std::unique_ptr<SceneNode>> map;
        for (auto& c : ch) map[c->id] = std::move(c);
        ch.clear();
        for (const auto& id : id_order) {
            auto it = map.find(id);
            if (it != map.end()) ch.push_back(std::move(it->second));
        }
    }

    // move_child kept for any callers that may still use it
    static void move_child(SceneNode* p, int src, int dst) {
        auto& ch = p->children;
        if (src < 0 || src >= (int)ch.size()) return;
        if (dst < 0 || dst >= (int)ch.size()) return;
        if (src == dst) return;
        auto node = std::move(ch[src]);
        ch.erase(ch.begin() + src);
        ch.insert(ch.begin() + dst, std::move(node));
    }

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return desc; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── AddLayerCommand ──────────────────────────────────────────────────────────
// s171 m1 — first of the layer-undoable thread. Adds a top-level layer
// to a document. The push site (LayersPanel::on_add_layer) builds the
// layer node, mints its iid, and hands off both the snapshot and the
// metadata for find-the-doc-on-undo.
//
// This command exists because the s170 crash (image moved across layers,
// host layer deleted, undo walks back into transform commands whose iid
// resolves to nullptr) is rooted in non-undoable structural mutations of
// `doc->layers`. Layer add was one of two "no command pushed" sites
// flagged by memory rule #26 and confirmed by the s170 m4 latent-hazard
// repro. Closing the gap means the undo stack stays coherent across all
// layer mutations.
//
// Doc identity: stored as `anchor_iid` — the iid of any SceneNode that
// lives in the same doc as the new layer (typically a sibling layer
// that already existed at push time). The command resolves the doc via
// `find_doc_by_iid(proj, anchor_iid)` rather than a raw `CurvzDocument*`
// to survive doc closures cleanly. If the anchor's doc is gone, the
// command no-ops — same partial-recovery shape as Cut/Duplicate.
//
// Active-layer-index handling: the existing add code adjusts
// active_layer_index after insert. We snap before/after explicitly so
// undo restores the user's pre-op active layer rather than letting
// post-op state leak.
struct AddLayerCommand : CurvzCommand {
    CurvzProject*              proj;            // resolution root
    std::string                anchor_iid;      // iid of any peer in the target doc
    std::unique_ptr<SceneNode> snap;            // deep clone of the new layer
    std::string                new_layer_iid;   // == snap->internal_id (cached for clarity)
    int                        index;           // insertion position in doc->layers
    int                        active_before;   // pre-op active_layer_index
    int                        active_after;    // post-op active_layer_index

    AddLayerCommand(CurvzProject* proj,
                    std::string anchor_iid,
                    std::unique_ptr<SceneNode> snap,
                    int index,
                    int active_before,
                    int active_after)
        : proj(proj)
        , anchor_iid(std::move(anchor_iid))
        , snap(std::move(snap))
        , new_layer_iid(this->snap ? this->snap->internal_id : std::string{})
        , index(index)
        , active_before(active_before)
        , active_after(active_after) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Add layer"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── DeleteLayerCommand ───────────────────────────────────────────────────────
// s171 m1 — second of the layer-undoable thread. Removes a top-level
// layer from a document.
//
// The deleted layer's snapshot is held in `snap` (deep clone, including
// its children — every iid preserved). On undo, the snapshot is
// re-inserted at the recorded index. Crucially, the children's iids
// match what they were before the delete, so any commands queued on
// those children (transforms, edits, etc.) will resolve correctly when
// undo walks back through them. This is the structural fix for the
// s170 crash — the snapshot keeps every iid alive across the
// delete-undo seam.
//
// Doc identity: same `anchor_iid` mechanism as AddLayerCommand. The
// anchor is a peer layer that survives the delete (the delete handler
// guarantees at least one normal layer remains, so a peer always
// exists). If the anchor is also gone (whole doc closed), the command
// no-ops.
struct DeleteLayerCommand : CurvzCommand {
    CurvzProject*              proj;            // resolution root
    std::string                anchor_iid;      // iid of a surviving peer layer
    std::unique_ptr<SceneNode> snap;            // deep clone of the deleted layer
    std::string                deleted_iid;     // == snap->internal_id (cached for clarity)
    int                        index;           // original position in doc->layers
    int                        active_before;   // pre-op active_layer_index
    int                        active_after;    // post-op active_layer_index

    DeleteLayerCommand(CurvzProject* proj,
                       std::string anchor_iid,
                       std::unique_ptr<SceneNode> snap,
                       int index,
                       int active_before,
                       int active_after)
        : proj(proj)
        , anchor_iid(std::move(anchor_iid))
        , snap(std::move(snap))
        , deleted_iid(this->snap ? this->snap->internal_id : std::string{})
        , index(index)
        , active_before(active_before)
        , active_after(active_after) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Delete layer"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── MoveObjectToLayerCommand ─────────────────────────────────────────────────
// s171 m2 — replaces the dead s149-era MoveToLayerCommand struct (which
// had zero push sites and a no-op execute). The two LayersPanel DnD
// cross-layer drop sites (setup_layer_drop type==1 path, and
// setup_path_drop cross-layer branch) now build and push this command
// instead of mutating the doc directly.
//
// Single object per command. Captured state: src_layer_iid + dst_layer_iid
// + obj_iid + src_index + dst_index. The object's own iid resolves the
// node; layer iids resolve the source/destination containers via
// find_by_iid (layers participate in the iid index).
//
// execute() = redo: erase by iid from src_layer's children, insert clone
//   at dst_index in dst_layer's children. obj_iid stays the same — the
//   clone preserves it so future commands resolving against this iid
//   keep working.
//
// undo() = reverse: erase by iid from dst_layer's children, insert clone
//   at src_index in src_layer's children.
//
// Partial-recovery semantics: if either layer iid resolves to nullptr
// (layer destroyed by some other op), skip and log. Same shape as Cut.
//
// Why this resolves the s170 crash class: previously, dragging an image
// from Layer 1 to Layer 2 was direct mutation with no command pushed;
// the undo stack still held transform commands keyed on the image's iid
// from when it lived on Layer 1. Deleting Layer 2 then walked undo back
// through those transforms after the image was gone, leaving the iid
// resolver to return nullptr and downstream code to crash. Now the move
// itself is undoable: undo walks back through Move-to-Layer-2-undo first,
// putting the image back on Layer 1, before reaching the transforms —
// they resolve to a live image and apply cleanly.
struct MoveObjectToLayerCommand : CurvzCommand {
    CurvzProject* proj;             // resolution root
    std::string   src_layer_iid;
    std::string   dst_layer_iid;
    std::string   obj_iid;          // the moved object's iid
    std::unique_ptr<SceneNode> snap;  // deep clone for redo / partial-recovery
    int src_index;                  // original position in src_layer->children
    int dst_index;                  // post-op position in dst_layer->children

    MoveObjectToLayerCommand(CurvzProject* proj,
                             std::string src_layer_iid,
                             std::string dst_layer_iid,
                             std::string obj_iid,
                             std::unique_ptr<SceneNode> snap,
                             int src_index,
                             int dst_index)
        : proj(proj)
        , src_layer_iid(std::move(src_layer_iid))
        , dst_layer_iid(std::move(dst_layer_iid))
        , obj_iid(std::move(obj_iid))
        , snap(std::move(snap))
        , src_index(src_index)
        , dst_index(dst_index) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Move to layer"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── MoveNodeIndexCommand ─────────────────────────────────────────────────────
// s234 m4 — single-target, iid-keyed move within a parent's children vector.
// Sibling of ZOrderCommand in spirit (intra-parent reorder), but with a
// narrower contract: one object, captured by iid + parent_iid + src_idx +
// dst_idx, suited to the script-driven `objects.<iid> move "<direction>"`
// verb.
//
// Why not reuse ZOrderCommand: ZOrderCommand::apply_order builds an
// id-keyed (SVG `id` field) map of the parent's children to apply a
// captured permutation. Script-minted objects (via `objects new`) have
// empty SVG `id` — they're assigned lazily at SVG-export time — so two
// or more script-minted siblings would collide on the same empty key
// and apply_order would silently drop all but one. Canvas tools don't
// hit this because their creation paths populate `id` through
// next_default_name / freshen_ids; the script path skips that. Rather
// than retrofit ZOrderCommand's key strategy (and risk regression in
// canvas's multi-object z-order ops which depend on the existing
// behaviour), m4 ships a single-target iid-keyed command alongside it.
// Same precedent as m3's EditObjectFieldCommand sitting alongside
// EditLayerFieldCommand: separate command class, parallel shape,
// narrower domain.
//
// Resolution shape: parent_iid resolves via curvz::utils::find_by_iid
// (parent participates in the per-doc iid index). obj_iid is captured
// informationally — execute/undo locate the target inside the resolved
// parent's children by iid match, then move it to the recorded slot.
// src_idx / dst_idx are the canonical slot positions, used as the
// insertion target after the by-iid erase.
//
// Partial-recovery shape: if parent_iid no longer resolves (parent
// destroyed by some other op), the command no-ops cleanly. If the
// target object is no longer in the parent's children (somebody else
// moved or deleted it), the command also no-ops — same shape as
// MoveObjectToLayerCommand's "erased=no" log path.
//
// Not structural in the s170 sense — tree shape is unchanged, no iids
// invalidated, no nodes destroyed. The s168 m6 invalidate-before-resolve
// dance applies anyway because find_by_iid is the resolution path.
struct MoveNodeIndexCommand : CurvzCommand {
    CurvzProject* proj;         // resolution root
    std::string   parent_iid;   // SceneNode::internal_id of the parent
    std::string   obj_iid;      // SceneNode::internal_id of the moved node
    int           src_idx;      // original position in parent->children
    int           dst_idx;      // post-op position in parent->children

    MoveNodeIndexCommand(CurvzProject* proj,
                         std::string parent_iid,
                         std::string obj_iid,
                         int src_idx,
                         int dst_idx)
        : proj(proj)
        , parent_iid(std::move(parent_iid))
        , obj_iid(std::move(obj_iid))
        , src_idx(src_idx)
        , dst_idx(dst_idx) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Move object"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── ReorderLayersCommand ─────────────────────────────────────────────────────
// s172 m3 — final structural piece of the layer-undoable arc. Closes the
// last "direct mutation of doc->layers" path: DnD layer reorder.
//
// Unlike Add/Delete (single-iid + index) and MoveObject (single iid +
// src/dst layers), reorder is a whole-vector permutation. Capture is two
// iid sequences: `iids_before` (pre-op order) and `iids_after` (post-op
// order). Both undo and redo apply by re-sorting doc->layers to match the
// recorded sequence — same algorithm in both directions, only the target
// sequence differs.
//
// Apply algorithm: move every entry of doc->layers into a temp map keyed
// by iid, then push them back in the order recorded. iids in the target
// sequence that no longer exist in the doc (e.g. layer deleted by some
// other operation while this command sat on the stack) are skipped; any
// surviving layers whose iids aren't in the target sequence get
// appended at the tail. This is the same partial-recovery shape as
// Cut/Duplicate.
//
// Doc identity: `anchor_iid` — same mechanism as AddLayer/DeleteLayer.
// We use the first non-empty iid from `iids_after` (always populated in
// a successful reorder; layers don't disappear during reorder).
//
// active_layer_index: snapped before/after by integer (matching the
// existing AddLayer/DeleteLayer pattern). The active layer's *iid*
// doesn't change across a reorder — only its index in the vector — so a
// numeric snap is consistent with the post-op vector layout.
//
// Why this isn't an s170-class crash: reorder doesn't destroy nodes or
// change tree shape — it permutes the top-level vector. No iids are
// invalidated; no commands queued on layer children are affected. Closing
// the gap is for undo-stack hygiene: previously a user could drag layer
// rows around freely and Ctrl+Z would skip past those reorderings,
// confusing the perceived undo timeline. After s172 m3 every structural
// mutation of doc->layers is undoable.
struct ReorderLayersCommand : CurvzCommand {
    CurvzProject*            proj;            // resolution root
    std::string              anchor_iid;      // iid of any peer in the target doc
    std::vector<std::string> iids_before;     // pre-op sequence of layer iids
    std::vector<std::string> iids_after;      // post-op sequence of layer iids
    int                      active_before;   // pre-op active_layer_index
    int                      active_after;    // post-op active_layer_index

    ReorderLayersCommand(CurvzProject* proj,
                         std::string anchor_iid,
                         std::vector<std::string> iids_before,
                         std::vector<std::string> iids_after,
                         int active_before,
                         int active_after)
        : proj(proj)
        , anchor_iid(std::move(anchor_iid))
        , iids_before(std::move(iids_before))
        , iids_after(std::move(iids_after))
        , active_before(active_before)
        , active_after(active_after) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Reorder layers"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── EditLayerFieldCommand ────────────────────────────────────────────────────
// s172 m4 — layer state-mutations sub-arc. One unified command class for
// the four undoable layer-field mutations: rename, color, visibility,
// lock. They share the same shape (single layer iid + before/after of
// one field), so collapsing them into one command type avoids four
// near-identical copies of the same body.
//
// This is NOT structural — tree shape is unchanged, no nodes destroyed,
// no iids invalidated. So it doesn't reproduce the s170 crash class.
// The motivation is undo-stack hygiene: previously, toggling a layer's
// visibility / lock / colour / name silently did nothing on Ctrl+Z,
// confusing the user about what's on the stack.
//
// Field discriminator: `Field` enum picks which pair of before/after
// fields to read. String fields (Name, Color) use `before_str` /
// `after_str`; bool fields (Visible, Locked) use `before_bool` /
// `after_bool`. The unused pair is ignored. Coalescing is intentionally
// NOT implemented here — each toggle / commit is one undo step. (Color
// picker live-drag could be coalesced in a future revision; for now the
// single-step model matches how AddLayer / DeleteLayer behave.)
//
// Doc identity: not needed — the layer iid resolves directly to the
// SceneNode via find_by_iid (layers participate in the iid index, same
// as MoveObjectToLayerCommand src_layer_iid / dst_layer_iid). Skip
// silently if the iid no longer resolves (layer deleted by some other
// command between push and replay).
struct EditLayerFieldCommand : CurvzCommand {
    enum class Field { Name, Color, Visible, Locked };

    CurvzProject* proj;             // resolution root
    std::string   layer_iid;        // direct iid of the layer being edited
    Field         field;
    std::string   before_str;       // used for Name / Color
    std::string   after_str;
    bool          before_bool;      // used for Visible / Locked
    bool          after_bool;

    EditLayerFieldCommand(CurvzProject* proj,
                          std::string layer_iid,
                          Field field,
                          std::string before_str,
                          std::string after_str,
                          bool before_bool,
                          bool after_bool)
        : proj(proj)
        , layer_iid(std::move(layer_iid))
        , field(field)
        , before_str(std::move(before_str))
        , after_str(std::move(after_str))
        , before_bool(before_bool)
        , after_bool(after_bool) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override {
        switch (field) {
            case Field::Name:    return "Rename layer";
            case Field::Color:   return "Change layer color";
            case Field::Visible: return "Toggle layer visibility";
            case Field::Locked:  return "Toggle layer lock";
        }
        return "Edit layer";
    }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── EditObjectFieldCommand ───────────────────────────────────────────────────
// s232 m3 — scene-object state-mutations sibling of EditLayerFieldCommand.
// Same shape (single iid + before/after of one field), narrower Field set
// (Name, Visible, Locked — no Color, since scene objects don't carry a
// single colour tag the way layers do; per-object colour lives in
// FillStyle/StrokeStyle and is handled by EditObjectCommand). The need
// arises with the `objects` Scriptable's m3 element mutating verbs
// (toggle_visible / set_visible / toggle_locked / set_locked / rename
// on `objects.<iid>` proxies); the existing user-facing rename path in
// LayersPanel for non-layer objects has been DIRECT mutation with no
// undo support since forever — this command class is the surface that
// future panel sweep can wire to, but m3 keeps the change scoped to the
// script path.
//
// Same structural-vs-cosmetic posture as the layer variant: not
// structural (no tree-shape change, no iid invalidation needed), so
// it doesn't reproduce the s170 crash class. The motivation is undo-
// stack hygiene — Ctrl+Z after a script-driven toggle / rename should
// reverse the change, not silently do nothing.
//
// Field discriminator: `Field` enum picks which pair to read. String
// field (Name) uses `before_str` / `after_str`; bool fields (Visible,
// Locked) use `before_bool` / `after_bool`. The unused pair is ignored.
// No coalescing — each invocation is one undo step, matching the
// layer-side single-step model.
//
// Doc identity: not needed. The scene-object iid resolves directly to
// the SceneNode via curvz::utils::find_by_iid (scene objects participate
// in the per-doc iid index, same as layers). Skip silently if the iid
// no longer resolves (node deleted by another command between push and
// replay) — same partial-recovery shape as the layer variant.
struct EditObjectFieldCommand : CurvzCommand {
    enum class Field { Name, Visible, Locked };

    CurvzProject* proj;             // resolution root
    std::string   obj_iid;          // direct iid of the scene object being edited
    Field         field;
    std::string   before_str;       // used for Name
    std::string   after_str;
    bool          before_bool;      // used for Visible / Locked
    bool          after_bool;

    EditObjectFieldCommand(CurvzProject* proj,
                           std::string obj_iid,
                           Field field,
                           std::string before_str,
                           std::string after_str,
                           bool before_bool,
                           bool after_bool)
        : proj(proj)
        , obj_iid(std::move(obj_iid))
        , field(field)
        , before_str(std::move(before_str))
        , after_str(std::move(after_str))
        , before_bool(before_bool)
        , after_bool(after_bool) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override {
        switch (field) {
            case Field::Name:    return "Rename object";
            case Field::Visible: return "Toggle object visibility";
            case Field::Locked:  return "Toggle object lock";
        }
        return "Edit object field";
    }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── RefMoveCommand ────────────────────────────────────────────────────────────
// Two-axis move of a Ref node's anchor point. s167 m2: migrated to
// iid+project capture; bodies out-of-line in CommandHistory.cpp.
struct RefMoveCommand : CurvzCommand {
    CurvzProject* proj;
    std::string   node_iid;
    double before_x, before_y;
    double after_x,  after_y;

    RefMoveCommand(CurvzProject* proj, std::string node_iid,
                   double bx, double by,
                   double ax, double ay)
        : proj(proj), node_iid(std::move(node_iid))
        , before_x(bx), before_y(by)
        , after_x(ax),  after_y(ay) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Move ref point"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── GuideMoveCommand ─────────────────────────────────────────────────────────
// s180 m1 — Guide drag undo. Same shape as RefMoveCommand: iid + project
// capture, three doubles before / three doubles after. Carries angle even
// though canvas drag only mutates x/y for axis-aligned guides — keeps the
// command symmetrical with the inspector's X/Y/A editor and absorbs any
// future angle-affecting edit (rotate handle, inspector spinner, snap-to-
// arbitrary-angle) without schema churn. If the guide has been deleted
// between push and undo, find_by_iid returns nullptr and the command
// no-ops cleanly. Default `references() == false` is correct here too.
struct GuideMoveCommand : CurvzCommand {
    CurvzProject* proj;
    std::string   node_iid;
    double before_x, before_y, before_angle;
    double after_x,  after_y,  after_angle;

    GuideMoveCommand(CurvzProject* proj, std::string node_iid,
                     double bx, double by, double ba,
                     double ax, double ay, double aa)
        : proj(proj), node_iid(std::move(node_iid))
        , before_x(bx), before_y(by), before_angle(ba)
        , after_x(ax),  after_y(ay),  after_angle(aa) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Move guide"; }
};

// ── PasteCommand ─────────────────────────────────────────────────────────────
// Inserts one or more pasted nodes into a parent at the front.
// Undo removes them by id; redo re-inserts clones.
struct PasteCommand : CurvzCommand {
    // s169 m3 — Migrated from raw-SceneNode* capture to iid + project capture.
    //
    // Pre-migration this command held a raw `SceneNode* parent` pointer with
    // NO `references()` override. If the target layer was destroyed between
    // push and undo, scrub_command_history would not find this command and
    // Ctrl+Z would dereference freed memory. Same shape of latent hazard as
    // ZOrder in s169 m2 — Stage 3 commands are uniformly missing references()
    // overrides and were silently exposed.
    //
    // Post-migration: command holds an iid + project pointer. Resolution
    // happens at execute()/undo() time via curvz::utils::find_by_iid; if the
    // layer is gone, command no-ops cleanly. The `snaps` vector holds
    // unique_ptr<SceneNode> deep clones — no pointer hazards there.
    CurvzProject* proj;        // resolution root
    std::string   parent_iid;  // SceneNode::internal_id of the target layer
    // Snapshots in insertion order (front = top of layer)
    std::vector<std::unique_ptr<SceneNode>> snaps;

    PasteCommand(CurvzProject* proj, std::string parent_iid,
                 std::vector<std::unique_ptr<SceneNode>> snaps)
        : proj(proj)
        , parent_iid(std::move(parent_iid))
        , snaps(std::move(snaps)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Paste"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── CutCommand ────────────────────────────────────────────────────────────────
// s170 m2 — Migrated from raw-SceneNode* capture to iid + project capture.
//
// Pre-migration this command held a raw `SceneNode* parent` per Entry with
// NO `references()` override. If a parent layer was destroyed between push
// and undo, scrub_command_history would not find this command and Ctrl+Z
// would dereference freed memory. Same shape of latent hazard as the rest
// of Stage 3.
//
// Cut differs from Paste (s169 m3) in that each Entry can come from a
// different parent — shift-selecting objects across layers and Cut
// produces a multi-parent command. So `parent_iid` is per-Entry, not on
// the command. Same per-Entry partial-recovery semantics as m4/m5/m6:
// if one parent is gone, that entry no-ops while the others still apply.
//
// Records the removal of one or more nodes for undo purposes.
// Undo re-inserts them at their original positions.
struct CutCommand : CurvzCommand {
    struct Entry {
        std::string                parent_iid;  // iid of the source layer
        std::unique_ptr<SceneNode> snap;
        int                        index;
    };
    CurvzProject*      proj;     // resolution root
    std::vector<Entry> entries;  // sorted ascending by index

    CutCommand(CurvzProject* proj, std::vector<Entry> entries)
        : proj(proj), entries(std::move(entries)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Cut"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── DuplicateCommand ──────────────────────────────────────────────────────────
// s170 m3 — Migrated from raw-SceneNode* capture to iid + project capture.
// Same shape as Cut (s170 m2): per-Entry parent_iid (different parents
// possible — duplicating across layer-spanning selections) with per-Entry
// partial-recovery semantics. If a parent is gone at execute()/undo()
// time, that entry no-ops while the others still apply.
//
// Used by three push sites: Canvas::duplicate_selected (offset duplicate),
// Canvas::duplicate_in_place_selected (in-place; renamed s181 from
// clone_selected), and Canvas_input.cpp Alt-drag (in-place duplicate
// redirected for move).
//
// Inserts duplicated nodes directly above their originals.
// Undo removes by id; redo re-inserts.
struct DuplicateCommand : CurvzCommand {
    struct Entry {
        std::string                parent_iid;  // iid of the target layer
        std::unique_ptr<SceneNode> snap;        // the new duplicate
        int                        index;       // insertion index (above original)
    };
    CurvzProject*      proj;     // resolution root
    std::vector<Entry> entries;

    DuplicateCommand(CurvzProject* proj, std::vector<Entry> entries)
        : proj(proj), entries(std::move(entries)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Duplicate"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── CornerTreatmentCommand ────────────────────────────────────────────────────
// s169 m4 — Migrated from raw-SceneNode* capture to iid + project capture.
//
// First multi-target Stage 3 migration: Entry holds an iid per affected
// path rather than a raw pointer. Each entry resolves independently at
// execute()/undo() time. If any one path is gone, that entry no-ops while
// the others still apply — partial-recovery semantics rather than all-or-
// nothing, which is the desired shape since Stage 3 commands often touch
// many objects whose lifecycles aren't coupled.
//
// Pre-migration this command held raw `Entry::obj` pointers with NO
// `references()` override. Same latent hazard pattern as the other Stage 3
// commands. Post-migration: the command holds iids; entries are resolved
// via curvz::utils::find_by_iid at apply time.
//
// `add()` now takes SceneNode*&const for ergonomic push-site code (caller
// has the live node in hand) and reads `internal_id` internally to
// store the iid. Same call shape at the push site.
struct CornerTreatmentCommand : CurvzCommand {
    struct Entry { std::string obj_iid; PathData before; PathData after; };

    CurvzProject*      proj;
    std::vector<Entry> entries;

    explicit CornerTreatmentCommand(CurvzProject* proj) : proj(proj) {}

    void add(const SceneNode* obj, PathData before, PathData after) {
        entries.push_back({obj->internal_id, std::move(before), std::move(after)});
    }
    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Corner Treatment"; }
    bool preserves_selection() const override { return true; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── BooleanOpCommand ──────────────────────────────────────────────────────────
// s173 m2 — Migrated from raw-SceneNode* capture to iid + project capture.
//
// First Stage 4 migration. Pre-migration this command held a raw
// `SceneNode* parent` pointer with NO `references()` override — same
// latent hazard pattern as the Stage 3 commands had before s169–s170.
// If the parent layer was destroyed between push and undo,
// scrub_command_history would not find this command and Ctrl+Z would
// dereference freed memory.
//
// Post-migration: command holds parent_iid + project pointer. Resolution
// happens at execute()/undo() time via curvz::utils::find_by_iid; if the
// layer is gone, command no-ops cleanly. The originals/results vectors
// hold unique_ptr<SceneNode> deep clones — no pointer hazards there.
//
// Stores deep clones of all input nodes and all result nodes.
// Undo: removes results from parent->children, reinserts originals at
// their positions.
// Redo: removes originals, reinserts results at insert_index.
struct BooleanOpCommand : CurvzCommand {
    // Originals — in ascending index order
    struct Original {
        std::unique_ptr<SceneNode> snap;
        int index;
    };

    CurvzProject* proj;        // resolution root
    std::string   parent_iid;  // SceneNode::internal_id of the target parent
    std::vector<Original> originals;

    // Results — inserted at insert_index in ascending order
    std::vector<std::unique_ptr<SceneNode>> results;
    int insert_index;
    std::string desc;

    BooleanOpCommand(CurvzProject* proj, std::string parent_iid,
                     std::vector<Original> originals,
                     std::vector<std::unique_ptr<SceneNode>> results,
                     int insert_index,
                     std::string desc = "Boolean operation")
        : proj(proj)
        , parent_iid(std::move(parent_iid))
        , originals(std::move(originals))
        , results(std::move(results))
        , insert_index(insert_index)
        , desc(std::move(desc)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return desc; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── MakeBlendCommand ──────────────────────────────────────────────────────────
// s173 m3 — Migrated from raw-SceneNode* capture to iid + project capture.
//
// Pre-migration this command held a raw `SceneNode* parent` pointer with
// NO `references()` override and no null-guard in execute()/undo() —
// same latent hazard pattern as BooleanOpCommand had before s173 m2. If
// the parent layer was destroyed between push and undo, scrub_command_
// history would not find this command and Ctrl+Z would dereference freed
// memory.
//
// Post-migration: command holds parent_iid + project pointer. Resolution
// happens at execute()/undo() time via curvz::utils::find_by_iid; if the
// layer is gone, command no-ops cleanly. The originals vector and
// blend_snap hold unique_ptr<SceneNode> deep clones — no pointer hazards
// there.
//
// Undoable creation of a Blend container from exactly two source Paths
// that share a common parent. Same shape as BooleanOpCommand: removes
// the two originals, inserts the Blend at the lower index.
//
// On undo: removes the Blend and reinserts both originals at their
// original positions. The Blend snapshot holds cloned copies of
// blend_source_a / blend_source_b internally, so nothing is lost.
struct MakeBlendCommand : CurvzCommand {
    struct Original {
        std::unique_ptr<SceneNode> snap;
        int index;
    };

    CurvzProject* proj;        // resolution root
    std::string   parent_iid;  // SceneNode::internal_id of the target parent
    std::vector<Original> originals;             // always size 2 for v1

    std::unique_ptr<SceneNode> blend_snap;       // deep-cloned Blend node
    int insert_index;

    MakeBlendCommand(CurvzProject* proj, std::string parent_iid,
                     std::vector<Original> originals,
                     std::unique_ptr<SceneNode> blend_snap,
                     int insert_index)
        : proj(proj)
        , parent_iid(std::move(parent_iid))
        , originals(std::move(originals))
        , blend_snap(std::move(blend_snap))
        , insert_index(insert_index) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Make Blend"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── ReleaseBlendCommand ───────────────────────────────────────────────────────
// s173 m3 — Migrated from raw-SceneNode* capture to iid + project capture.
//
// Pre-migration this command held a raw `SceneNode* parent` pointer with
// NO `references()` override and no null-guard. Inverse of MakeBlend in
// shape but same hazard class. Post-migration: command holds parent_iid
// + project pointer; resolution via curvz::utils::find_by_iid at
// execute()/undo() time. If the layer is gone, no-op cleanly.
//
// Inverse of MakeBlendCommand in structure (1→3 instead of 2→1).
//
// execute():
//   Removes the Blend from parent (by id) and inserts the three result
//   children in ascending z-order at insert_index:
//     [insert_index + 0] = A (bottom)
//     [insert_index + 1] = Steps Group (middle)
//     [insert_index + 2] = B (top)
//
// undo():
//   Removes the three results (by id) and reinserts the Blend at
//   insert_index.
//
// Selection fixup is the caller's job — ReleaseBlendCommand is a pure
// tree mutation.
struct ReleaseBlendCommand : CurvzCommand {
    CurvzProject* proj;        // resolution root
    std::string   parent_iid;  // SceneNode::internal_id of the target parent
    std::unique_ptr<SceneNode> blend_snap;
    std::vector<std::unique_ptr<SceneNode>> results;  // A, Steps-Group, B
    int insert_index;

    ReleaseBlendCommand(CurvzProject* proj, std::string parent_iid,
                        std::unique_ptr<SceneNode> blend_snap,
                        std::vector<std::unique_ptr<SceneNode>> results,
                        int insert_index)
        : proj(proj)
        , parent_iid(std::move(parent_iid))
        , blend_snap(std::move(blend_snap))
        , results(std::move(results))
        , insert_index(insert_index) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Release Blend"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── MakeWarpCommand ───────────────────────────────────────────────────────────
// s173 m4 — Migrated from raw-SceneNode* capture to iid + project capture.
//
// Pre-migration this command held a raw `SceneNode* parent` pointer with
// NO `references()` override and no null-guard in execute()/undo() — same
// latent hazard pattern as MakeBlend before s173 m3. If the parent layer
// was destroyed between push and undo, scrub_command_history would not
// find this command and Ctrl+Z would dereference freed memory.
//
// Post-migration: command holds parent_iid + project pointer. Resolution
// happens at execute()/undo() time via curvz::utils::find_by_iid; if the
// layer is gone, command no-ops cleanly. The source_snap and warp_snap
// hold unique_ptr<SceneNode> deep clones — no pointer hazards there.
//
// Undoable creation of a Warp container around a single source node
// (Path, Compound, or Group). The source is cloned into the Warp's
// warp_source slot and removed from the parent's children; the Warp
// takes the source's position in the parent.
//
// On undo: removes the Warp and reinserts the original at its original
// position. The Warp snapshot holds a cloned copy of warp_source
// internally so nothing is lost.
struct MakeWarpCommand : CurvzCommand {
    CurvzProject* proj;        // resolution root
    std::string   parent_iid;  // SceneNode::internal_id of the target parent
    std::unique_ptr<SceneNode> source_snap;          // clone of the original
    int source_index;                                 // original's position
    std::unique_ptr<SceneNode> warp_snap;             // fully-built Warp
    int insert_index;

    MakeWarpCommand(CurvzProject* proj, std::string parent_iid,
                    std::unique_ptr<SceneNode> source_snap,
                    int source_index,
                    std::unique_ptr<SceneNode> warp_snap,
                    int insert_index)
        : proj(proj)
        , parent_iid(std::move(parent_iid))
        , source_snap(std::move(source_snap))
        , source_index(source_index)
        , warp_snap(std::move(warp_snap))
        , insert_index(insert_index) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Make Warp"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── ReleaseWarpCommand ────────────────────────────────────────────────────────
// s173 m4 — Migrated from raw-SceneNode* capture to iid + project capture.
//
// Same pre-migration hazard as MakeWarp. Post-migration: parent_iid +
// project resolution at execute()/undo() time; null-safe no-op if the
// parent layer is gone.
//
// Inverse of MakeWarpCommand: removes the Warp and reinserts the source
// (cloned from warp_source) at the Warp's position. Envelope data is
// discarded — release is meant to be the "undo the Make" operation.
// If the user wants to preserve the baked warped geometry, they should
// use Flatten Warp instead.
struct ReleaseWarpCommand : CurvzCommand {
    CurvzProject* proj;        // resolution root
    std::string   parent_iid;  // SceneNode::internal_id of the target parent
    std::unique_ptr<SceneNode> warp_snap;             // cloned Warp
    std::unique_ptr<SceneNode> source_snap;           // cloned source
    int insert_index;

    ReleaseWarpCommand(CurvzProject* proj, std::string parent_iid,
                       std::unique_ptr<SceneNode> warp_snap,
                       std::unique_ptr<SceneNode> source_snap,
                       int insert_index)
        : proj(proj)
        , parent_iid(std::move(parent_iid))
        , warp_snap(std::move(warp_snap))
        , source_snap(std::move(source_snap))
        , insert_index(insert_index) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Release Warp"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── FlattenWarpCommand ────────────────────────────────────────────────────────
// s173 m4 — Migrated from raw-SceneNode* capture to iid + project capture.
//
// Same pre-migration hazard as MakeWarp/ReleaseWarp. Post-migration:
// parent_iid + project resolution at execute()/undo() time; null-safe
// no-op if the parent layer is gone.
//
// Replaces a Warp with its baked warp_cache output. Destructive of the
// envelope (envelope data is lost — to re-edit, the user would need to
// undo the flatten). Unlike Release, the visible geometry doesn't
// change on Flatten; only the editability model does.
//
// The flattened result is whatever was in warp_cache at the moment of
// flatten. For a Path source this is a Path; for a Compound source a
// Compound; for a Group source a Group. Result keeps the Warp's
// visible/locked/opacity/transform to preserve the node's visual
// presence in its parent.
struct FlattenWarpCommand : CurvzCommand {
    CurvzProject* proj;        // resolution root
    std::string   parent_iid;  // SceneNode::internal_id of the target parent
    std::unique_ptr<SceneNode> warp_snap;             // cloned pre-flatten Warp
    std::unique_ptr<SceneNode> flattened_snap;        // cloned baked result
    int insert_index;

    FlattenWarpCommand(CurvzProject* proj, std::string parent_iid,
                       std::unique_ptr<SceneNode> warp_snap,
                       std::unique_ptr<SceneNode> flattened_snap,
                       int insert_index)
        : proj(proj)
        , parent_iid(std::move(parent_iid))
        , warp_snap(std::move(warp_snap))
        , flattened_snap(std::move(flattened_snap))
        , insert_index(insert_index) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Flatten Warp"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── EditWarpCommand ───────────────────────────────────────────────────────────
// s174 m1 — Migrated from raw-SceneNode* capture to iid + project capture.
//
// Undoable edit of a Warp's envelope curves and quality setting. Snapshots
// pre-state (envelope + quality + preset_idx) on construction and swaps to
// post-state on execute. Unlike Make/Release/Flatten, this doesn't change
// the tree topology — it mutates a single Warp node in place.
//
// The warp_cache_dirty flag is forced true by both execute and undo so the
// next draw rebuilds with the updated envelope.
//
// s147 m2: preset_idx is part of the swap so handle-drag drift (which
// clears preset_idx to -1 in the post-state) is undone atomically with
// the envelope shape — undoing a drag restores both the original
// envelope AND the preset label that was on it before the drag.
//
// s174 m1: capture shape is the EditPath template — single-target field
// swap. obj_iid is the warp node's internal_id; resolution via
// curvz::utils::find_by_iid at execute()/undo() time. If the warp is
// destroyed between push and replay, the command no-ops cleanly. Bodies
// move out-of-line into CommandHistory.cpp so the resolver can use the
// full CurvzProject type.
struct EditWarpCommand : CurvzCommand {
    CurvzProject* proj;     // resolution root
    std::string   obj_iid;  // internal_id of the target Warp node

    // Pre-state (what was there before the user's edits).
    PathData pre_env_top;
    PathData pre_env_bottom;
    int      pre_quality;
    int      pre_preset_idx;     // s147 m2

    // Post-state (what the user committed).
    PathData post_env_top;
    PathData post_env_bottom;
    int      post_quality;
    int      post_preset_idx;    // s147 m2

    EditWarpCommand(CurvzProject* proj, std::string obj_iid,
                    PathData pre_top, PathData pre_bottom, int pre_q,
                    PathData post_top, PathData post_bottom, int post_q,
                    int pre_preset = -1, int post_preset = -1)
        : proj(proj)
        , obj_iid(std::move(obj_iid))
        , pre_env_top(std::move(pre_top))
        , pre_env_bottom(std::move(pre_bottom))
        , pre_quality(pre_q)
        , pre_preset_idx(pre_preset)
        , post_env_top(std::move(post_top))
        , post_env_bottom(std::move(post_bottom))
        , post_quality(post_q)
        , post_preset_idx(post_preset) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Edit Warp"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── StepRepeatCommand ─────────────────────────────────────────────────────────
// s175 m1 — Migrated from raw-SceneNode* per-Entry parent to per-Entry
// parent_iid + project capture. Multi-target shape, same template as
// CutCommand (s170 m2): each Entry resolves its own parent independently
// at execute()/undo() time, with per-Entry no-op-on-miss so partial
// destruction (some parents alive, others gone) leaves the still-resolvable
// entries fully reversible.
//
// Inserts N copies of each selected object, each offset by (dx*i, dy*i).
// All copies are removed atomically on undo.
//
// s175 m1: bodies move out-of-line into CommandHistory.cpp so the resolver
// can use the full CurvzProject type. Ordering invariant preserved —
// inserts happen in descending insertion-index order to keep positions
// stable when multiple Entries share a parent.
struct StepRepeatCommand : CurvzCommand {
    struct Entry {
        std::string                parent_iid;  // internal_id of the target parent
        std::unique_ptr<SceneNode> snap;        // deep copy of the inserted node
        int                        index;       // insertion index
    };
    CurvzProject*      proj;     // resolution root
    std::vector<Entry> entries;  // all copies across all steps, in insertion order

    StepRepeatCommand(CurvzProject* proj, std::vector<Entry> e)
        : proj(proj), entries(std::move(e)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return "Step and Repeat"; }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── LinkTextToPathCommand ─────────────────────────────────────────────────────
// s175 m2 — Migrated from raw-SceneNode* obj capture to iid + project
// capture. TextEdit template — single-target field swap.
//
// Undoable link/unlink of a text node to a guide path.
// Stores before/after text_path_id, offset, flip, and text_x/text_y so that
// detach (which repositions the text node) is fully reversible.
//
// s174 finding (banked, applied here): text_path_id is *already* an iid
// (it stores the target path's internal_id, not its SVG id — verified via
// Canvas::top_find_path_by_id which searches by internal_id == id, and
// SvgWriter writes it back as data-curvz-path-id for round-trip). So
// before_path_id / after_path_id are kept verbatim — they're not
// pointers, they're the same iid that find_by_iid resolves on the other
// end. No SVG-id-to-iid upgrade needed.
//
// s175 m2: obj_iid replaces SceneNode* obj. Resolution via
// curvz::utils::find_by_iid at execute()/undo() time. If the text node
// is destroyed between push and replay, the command no-ops cleanly.
// Bodies move out-of-line into CommandHistory.cpp so the resolver can
// use the full CurvzProject type.
struct LinkTextToPathCommand : CurvzCommand {
    CurvzProject* proj;     // resolution root
    std::string   obj_iid;  // internal_id of the target text node
    std::string before_path_id;
    double      before_offset;
    bool        before_flip;
    double      before_x, before_y;   // text_x/text_y before the operation
    std::string after_path_id;
    double      after_offset;
    bool        after_flip;
    double      after_x, after_y;     // text_x/text_y after the operation

    LinkTextToPathCommand(CurvzProject* proj, std::string obj_iid,
                          std::string bpid, double boff, bool bflip,
                          double bx, double by,
                          std::string apid, double aoff, bool aflip,
                          double ax, double ay)
        : proj(proj), obj_iid(std::move(obj_iid))
        , before_path_id(std::move(bpid)), before_offset(boff), before_flip(bflip)
        , before_x(bx), before_y(by)
        , after_path_id(std::move(apid)),  after_offset(aoff),  after_flip(aflip)
        , after_x(ax), after_y(ay) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override {
        return after_path_id.empty() ? "Detach text from path" : "Link text to path";
    }
    // No `references()` override — iid-based capture means no raw
    // SceneNode* is held. Default `references() == false` is correct.
};

// ── EditSwatchCommand ─────────────────────────────────────────────────────────
// Records a colour AND/OR name change to a single swatch in the
// SwatchLibrary. execute()/undo() both call update_swatch() which fires
// signal_swatch_changed → Canvas::live_recolour_walk → queue_draw,
// so bound objects ripple visually in both directions automatically.
//
// SwatchesPanel pushes one of these per popover-edit session that
// commits (Esc cancels and reverts in-place without pushing). Library
// pointer is captured at push time and assumed to outlive the command
// (CommandHistory is owned by MainWindow, swatch library by the
// project; both live for the doc's lifetime).
//
// S85 cont-2: extended to carry name_before / name_after so the
// SwatchesPanel popover's new inline name field rolls into the same
// atomic undo as the colour change. Backwards-compatible constructor
// (colour-only) preserved for any call sites that only mutate colour
// — but in practice the only call site is SwatchesPanel and it now
// always passes both.
struct EditSwatchCommand : CurvzCommand {
    color::SwatchLibrary*  library;
    color::SwatchId        swatch_id;
    color::Color           color_before;   // colour before the edit session
    color::Color           color_after;    // colour at the moment of commit
    std::string            name_before;    // name before the edit session
    std::string            name_after;     // name at the moment of commit

    // Full ctor (colour + name).
    EditSwatchCommand(color::SwatchLibrary* lib,
                      color::SwatchId id,
                      const color::Color& cb,
                      const color::Color& ca,
                      std::string nb,
                      std::string na)
        : library(lib)
        , swatch_id(std::move(id))
        , color_before(cb)
        , color_after(ca)
        , name_before(std::move(nb))
        , name_after(std::move(na)) {}

    // Backwards-compatible colour-only ctor — name fields default to
    // empty, so apply() leaves them at empty before/after, which is a
    // no-op shift if the swatch's name happened to already be empty.
    // For safety, callers that rename should always use the full ctor.
    EditSwatchCommand(color::SwatchLibrary* lib,
                      color::SwatchId id,
                      const color::Color& cb,
                      const color::Color& ca)
        : library(lib)
        , swatch_id(std::move(id))
        , color_before(cb)
        , color_after(ca) {}

    void execute() override { apply(color_after, name_after); }
    void undo()    override { apply(color_before, name_before); }

    std::string description() const override {
        // Tailor the description to what actually changed. The user
        // sees this in the undo menu.
        const bool color_changed = !(color_before == color_after);
        const bool name_changed  = (name_before != name_after);
        if (color_changed && name_changed) return "Edit swatch";
        if (name_changed)                  return "Rename swatch";
        return "Change swatch colour";
    }

    // Don't clobber the canvas selection on undo/redo — a swatch edit
    // is an appearance change on whatever objects are bound to it, not
    // a structural mutation. Same rationale as EditAppearanceCommand.
    bool preserves_selection() const override { return true; }

private:
    void apply(const color::Color& c, const std::string& n) {
        if (!library) return;
        const color::Swatch* existing = library->find_swatch(swatch_id);
        if (!existing) return;
        const auto* solid = std::get_if<color::SolidSwatch>(existing);
        if (!solid) return;  // gradient swatches not handled yet
        color::SolidSwatch updated = *solid;
        updated.color       = c;
        updated.header.name = n;
        library->update_swatch(swatch_id, color::Swatch{updated});
    }
};

// ── AddSwatchCommand (s220 m1a) ───────────────────────────────────────────────
//
// Adds a Swatch to the custom tier of the SwatchLibrary. Long-standing
// backlog item (referenced in AddStyleCommand's header as the symmetric
// future work for the swatch side); the SwatchesScriptable.new verb in
// s220 m1b is the forcing function that closes the gap.
//
// Identity model — mirrors AddStyleCommand:
//   * If swatch_value's header.id is empty at construction, execute()
//     calls library->add_swatch() and lets the library generate a
//     fresh id. The assigned id is captured into m_assigned_id so
//     undo() / redo() target the same id consistently.
//   * If the id is pre-set (uncommon — scripted callers pass empty;
//     re-do of an undone add reuses the captured id), execute() preserves
//     it via add_swatch's pre-set-id branch. add_swatch rejects collisions.
//
// Redo policy: replay the SAME id captured during the first execute.
// add_swatch with a pre-set id succeeds because the id was just freed
// by undo()'s remove_swatch call. Bindings made between the original
// execute and a subsequent undo can resolve cleanly on redo because
// the id is stable.
//
// Two construction modes:
//   * Standard ctor: caller hands over the value, command's first
//     execute() adds. Scripted callers (s220 m1b's `swatches new`)
//     use this.
//   * "Already-added" factory: caller has ALREADY called add_swatch
//     directly (e.g. the popover create flow that adds-on-first-colour-
//     emit and only learns at commit time that it wants undo). The id
//     is pre-populated from the value's header, first execute() is a
//     no-op (the swatch is already there), undo() removes by the
//     captured id, redo() re-adds.
//     Required: value.header.id MUST be non-empty (the live id of the
//     swatch in the library).
//
// Palette membership (s220 m1a fix-3):
//   The popover-commit flow adds the new swatch to the active palette
//   during the live-edit phase, BEFORE the AddSwatchCommand is built.
//   Undo's remove_swatch correctly strips palette membership (via
//   remove_swatch's own scrub of custom palettes). But REDO's add_swatch
//   only restores the swatch to the swatches pool — palette membership
//   is gone, and the chip stays invisible in the grid until something
//   else triggers a palette-rebuild.
//
//   Fix: the already_added factory snapshots palette memberships at
//   commit time (same shape as RemoveSwatchCommand's snapshot). On
//   redo, execute() replays add_to_palette + reorder_in_palette after
//   the swatch is re-added, then re-emits signal_swatch_added so the
//   panel refreshes with palette state correct. Same ORDERING
//   SUBTLETY noted on RemoveSwatchCommand::undo() — add_swatch's
//   signal fires before the replay; the post-replay re-emit closes
//   the gap.
//
//   The standard ctor doesn't snapshot palette memberships; scripted
//   callers (s220 m1b) that want palette assignment alongside swatch
//   creation will compose AddSwatchCommand + add_to_palette as separate
//   undoable units (or wrap in a CompositeCommand). The popover flow's
//   "create + add-to-active-palette in one user gesture" is the only
//   site that needs the composite snapshot, and it has the factory.
//
// Library pointer captured at push time, lifetime guaranteed by the
// project (same as EditSwatchCommand).
//
// preserves_selection: true. Library mutation, not a scene-tree change.
//
// Future restructure (logged for a later session, not this milestone):
//   The popover currently performs a "live mid-popover" create: the
//   swatch enters the library on the first colour-pick inside the
//   popover, and the popover edits in place from then on. Esc undoes
//   the create by removing the swatch; Return commits. A cleaner
//   structural model is "draft until commit" — the popover holds a
//   draft SolidSwatch in local state and only writes to the library
//   on Return. That eliminates the entire already_added factory + the
//   palette-membership-snapshot dance below; the standard AddSwatchCommand
//   ctor would suffice. Out of scope for s220 m1a.
struct AddSwatchCommand : CurvzCommand {
    color::SwatchLibrary* library;
    color::Swatch         swatch_value;     // captured at construction
    color::SwatchId       m_assigned_id;    // captured on first execute()
    std::string           desc;
    bool                  m_first_execute_consumed = false;  // true after
                                                             // the already-
                                                             // added factory
    // Captured by the already_added factory only. Each pair is
    // (palette_id, index-within-that-palette's-swatches) at commit time.
    // Empty for the standard ctor path.
    std::vector<std::pair<color::PaletteId, std::size_t>> palette_memberships;

    AddSwatchCommand(color::SwatchLibrary* lib,
                     color::Swatch s,
                     std::string description = "Add swatch")
        : library(lib)
        , swatch_value(std::move(s))
        , desc(std::move(description)) {}

    // Already-added variant. Caller has done the add_swatch directly
    // (and may have added to one or more palettes). This factory
    // captures the live id AND the swatch's palette memberships, then
    // treats the first execute() as a no-op so push() doesn't double-add.
    // Subsequent redoes (after an undo) replay the add normally AND
    // restore palette memberships.
    static std::unique_ptr<AddSwatchCommand>
    already_added(color::SwatchLibrary* lib,
                  color::Swatch live_value,
                  std::string description = "Add swatch") {
        auto cmd = std::make_unique<AddSwatchCommand>(
            lib, std::move(live_value), std::move(description));
        const color::SwatchId& live_id =
            color::swatch_header(cmd->swatch_value).id;
        cmd->m_assigned_id = live_id;
        cmd->m_first_execute_consumed = true;

        // Snapshot palette memberships while the swatch is live. Same
        // walk shape as RemoveSwatchCommand's ctor (custom palettes
        // only; defaults can't reference custom-tier swatches). If lib
        // is null or the id doesn't resolve, leave empty — redo will
        // just re-add the swatch with no palette restoration, matching
        // the standard-ctor behaviour.
        if (lib && !live_id.empty()) {
            for (const auto& pid : lib->all_palette_ids()) {
                if (lib->is_default_palette(pid)) continue;
                const color::Palette* pal = lib->find_palette(pid);
                if (!pal) continue;
                for (std::size_t i = 0; i < pal->swatches.size(); ++i) {
                    if (pal->swatches[i] == live_id) {
                        cmd->palette_memberships.emplace_back(pid, i);
                        break;
                    }
                }
            }
        }
        return cmd;
    }

    void execute() override {
        if (!library) return;
        if (m_first_execute_consumed) {
            // The swatch is already in the library (caller did the
            // add). Drop this execute, but flip the flag so subsequent
            // redoes go through the normal add path.
            m_first_execute_consumed = false;
            return;
        }
        // First execute: id may be empty → library mints. Subsequent
        // redoes: id is non-empty (carried in m_assigned_id and
        // mirrored back into the swatch header below).
        color::SwatchId result = library->add_swatch(swatch_value);
        if (result.empty()) {
            // add_swatch rejected — typically a redo-after-collision
            // (shouldn't happen since undo just freed the id). Library
            // logs WARN; we leave m_assigned_id empty so undo() becomes
            // a no-op.
            return;
        }
        m_assigned_id = result;
        // Mirror the assigned id back into swatch_value's header so a
        // second redo (after another undo) targets the same id with a
        // pre-set value.
        color::swatch_header(swatch_value).id = result;

        // s220 m1a fix-3: replay palette memberships on redo. Snapshot
        // exists only when the already_added factory captured it (the
        // popover-commit path); empty for standard ctor (scripted) paths.
        // After replay, re-emit signal_swatch_added so the panel
        // refreshes with palette state correct — same ORDERING SUBTLETY
        // story as RemoveSwatchCommand::undo() (add_swatch's own signal
        // fires before the replay).
        if (!palette_memberships.empty()) {
            for (const auto& [pid, idx] : palette_memberships) {
                library->add_to_palette(pid, result);
                library->reorder_in_palette(pid, result, idx);
            }
            library->signal_swatch_added().emit(result);
        }
    }

    void undo() override {
        if (!library) return;
        if (m_assigned_id.empty()) return;  // execute failed earlier
        library->remove_swatch(m_assigned_id);
        // Don't clear m_assigned_id — redo needs to re-add with the
        // same id (so any external bindings or palette memberships made
        // between the original execute and this undo can resolve cleanly
        // on redo).
        //
        // Palette membership scrubbing happens automatically inside
        // remove_swatch's own walk of custom palettes; nothing further
        // needed here.
    }

    std::string description() const override { return desc; }

    bool preserves_selection() const override { return true; }
};

// ── RemoveSwatchCommand (s220 m1a) ────────────────────────────────────────────
//
// Removes a custom-tier Swatch from the SwatchLibrary, capturing enough
// state to round-trip undo through the library's destructive cleanup.
// Mirrors RemoveStyleCommand in shape but carries an extra snapshot
// dimension: palette membership.
//
// What SwatchLibrary::remove_swatch destroys vs what undo must restore:
//   * The swatch record itself        → restored via add_swatch(snapshot)
//                                       with pre-set id (id was freed
//                                       by the remove).
//   * Membership in every custom      → restored by re-walking the
//     palette that referenced the id    captured (palette_id, index)
//                                       list and replaying add_to_palette
//                                       + reorder_in_palette.
//   * Recents entry                   → NOT restored. Recents is a
//                                       transient MRU working list; on
//                                       undo the swatch reappears but
//                                       its recents position is gone.
//                                       A subsequent touch puts it back
//                                       at the front naturally. The
//                                       alternative — capturing and
//                                       restoring the recents index —
//                                       fights the "MRU is current
//                                       working state" semantics for
//                                       no UX win.
//
// Defaults-tier guard: panel never builds a RemoveSwatchCommand for a
// defaults id (the UI hides the delete affordance for read-only swatches);
// the scripted path's `swatches delete` verb refuses on is_default_swatch
// before constructing the command. The command itself doesn't enforce
// — symmetric with RemoveStyleCommand which trusts its panel guard.
//
// Snapshot timing: built in the ctor, BEFORE execute() runs. The panel's
// usage shape is:
//     auto cmd = std::make_unique<RemoveSwatchCommand>(lib, id);
//     cmd->execute();
//     history->push(std::move(cmd));
// The library walk in the ctor sees the live, pre-remove state.
//
// preserves_selection: true. Library mutation, not a scene-tree change.
struct RemoveSwatchCommand : CurvzCommand {
    color::SwatchLibrary* library;
    color::Swatch         swatch_value;   // full snapshot pre-remove
    // Per-palette membership pre-remove. Order matters: each pair is
    // (palette_id, index-within-that-palette's-swatches). On undo we
    // re-add to each palette then reorder to the captured index. Empty
    // if the swatch wasn't in any custom palette.
    std::vector<std::pair<color::PaletteId, std::size_t>> palette_memberships;
    std::string           desc;

    RemoveSwatchCommand(color::SwatchLibrary* lib,
                        const color::SwatchId& id,
                        std::string description = "Delete swatch")
        : library(lib)
        , desc(std::move(description)) {
        // Snapshot the swatch value and palette memberships while the
        // swatch is still live. If lib is null or the id doesn't resolve,
        // leave everything empty — execute() becomes a no-op and undo()
        // matches.
        if (!library) return;
        const color::Swatch* existing = library->find_swatch(id);
        if (!existing) return;
        swatch_value = *existing;

        // Walk custom palettes only — defaults palettes can't reference
        // a custom-tier swatch by construction (defaults palettes were
        // loaded from the same defaults file as defaults swatches; any
        // cross-tier reference goes the other way, custom-palette to
        // defaults-swatch).
        for (const auto& pid : library->all_palette_ids()) {
            if (library->is_default_palette(pid)) continue;
            const color::Palette* pal = library->find_palette(pid);
            if (!pal) continue;
            for (std::size_t i = 0; i < pal->swatches.size(); ++i) {
                if (pal->swatches[i] == id) {
                    palette_memberships.emplace_back(pid, i);
                    break;  // a swatch appears at most once per palette
                            // (add_to_palette is idempotent)
                }
            }
        }
    }

    void execute() override {
        if (!library) return;
        if (color::swatch_header(swatch_value).id.empty()) return;  // ctor failed to snapshot
        library->remove_swatch(color::swatch_header(swatch_value).id);
    }

    void undo() override {
        if (!library) return;
        if (color::swatch_header(swatch_value).id.empty()) return;
        // Re-add with the original id. add_swatch accepts pre-set ids
        // as long as they're not in either tier — the id is free because
        // execute() just removed it.
        //
        // ORDERING SUBTLETY: add_swatch fires signal_swatch_added
        // synchronously, which triggers the panel's refresh() — at that
        // moment the palette hasn't been re-populated yet (the replay
        // loop below hasn't run), so the panel's chip-flow rebuild reads
        // an active_palette_size that's one short of correct. The chip
        // for the restored swatch is missing from the visible grid even
        // though the library has the swatch back.
        //
        // Fix: replay palette memberships, then re-emit signal_swatch_added
        // so the panel refreshes a second time with the palette now
        // correct. Double-fire is harmless (refresh is idempotent;
        // schedule_save is debounced).
        color::SwatchId restored = library->add_swatch(swatch_value);
        if (restored.empty()) return;  // collision shouldn't happen; bail.

        // Replay palette memberships in capture order. add_to_palette
        // appends; reorder_in_palette moves to the captured index. If
        // a palette has been deleted between the original execute and
        // this undo, the calls return false and we move on — best-effort
        // restoration.
        for (const auto& [pid, idx] : palette_memberships) {
            library->add_to_palette(pid, restored);
            library->reorder_in_palette(pid, restored, idx);
        }

        // s220 m1a hotfix-2: nudge listeners to re-read state now that
        // palette membership has been replayed. Without this the panel's
        // chip grid stays one entry short until the next user-triggered
        // refresh. See the ORDERING SUBTLETY comment above for why
        // add_swatch's own signal fires too early. The fix lives on the
        // command because palette membership replay is a command-level
        // concern (the library's add_swatch doesn't know it's part of
        // an undo); a deeper structural fix would add signals to
        // add_to_palette / remove_from_palette themselves and rewire the
        // panel to listen for palette-shape changes, but that's a wider
        // sweep than this hotfix.
        if (!palette_memberships.empty()) {
            library->signal_swatch_added().emit(restored);
        }
    }

    std::string description() const override { return desc; }

    bool preserves_selection() const override { return true; }
};

// ── AddPaletteCommand (s243 m1) ───────────────────────────────────────────────
//
// Adds a Palette to the custom tier of the SwatchLibrary. First of the
// s243 m1 palette-CRUD command quintet (Add/Remove/Rename/
// PaletteMembership + duplicate goes through Add by composition). Mirrors
// AddSwatchCommand in shape — the swatch-CRUD shape is the right fit
// because palettes live on the same library, with the same string-id
// identity model and the same project-tied lifetime. NOT the s242 m1
// scene-tree iid shape (CurvzProject* + node iids) — palettes are not
// in the scene tree.
//
// Identity model — mirrors AddSwatchCommand exactly:
//   * If palette_value.id is empty at construction, execute() calls
//     library->add_palette() and lets the library generate a fresh id.
//     The assigned id is captured into m_assigned_id so undo / redo
//     target the same id consistently.
//   * If the id is pre-set, execute() preserves it; add_palette rejects
//     collisions (returns empty), which we detect and the command
//     no-ops on undo.
//
// Redo policy: replay the SAME id captured during the first execute.
// add_palette with a pre-set id succeeds because the id was just freed
// by undo()'s remove_palette call.
//
// Two construction modes (mirrors AddSwatchCommand):
//   * Standard ctor: caller hands over the value, command's first
//     execute() adds. The panel's on_new_palette path uses this.
//     Scripted callers in s243 m2 will also single-shot through here.
//   * "Already-added" factory: caller has ALREADY called add_palette
//     OR duplicate_palette directly (and so has the live id in hand).
//     The factory captures the live value + id; first execute() is a
//     no-op (palette is already there), undo() removes by the
//     captured id, redo() re-adds. The panel's on_duplicate_palette
//     uses this so duplicate_palette stays the single source of truth
//     for duplicate semantics.
//
// Library pointer captured at push time, lifetime guaranteed by the
// project (same as AddSwatchCommand).
//
// preserves_selection: true. Library mutation, not a scene-tree change.
struct AddPaletteCommand : CurvzCommand {
    color::SwatchLibrary* library;
    color::Palette        palette_value;   // captured at construction
    color::PaletteId      m_assigned_id;   // captured on first execute()
    std::string           desc;
    bool                  m_first_execute_consumed = false;  // true after
                                                             // already_added
    // s243 m1 fix: when true, execute() (including redo) sets the
    // newly-added palette as the library's active palette after the
    // add. Both the panel's on_new_palette and on_duplicate_palette
    // make the new palette active as part of the UX contract; without
    // this flag, undo-then-redo would re-add the palette but leave
    // active empty (cleared by the prior undo's remove_palette side
    // effect), and any subsequent operation that reads active_palette
    // (e.g. Delete Palette) would silently bail on its empty-active
    // guard. The flag is set by the caller at command-build time:
    // the panel knows whether the new palette should become active.
    // Symmetric with RemovePaletteCommand's active_before snapshot.
    bool                  m_make_active = false;

    AddPaletteCommand(color::SwatchLibrary* lib,
                      color::Palette p,
                      std::string description = "Add palette")
        : library(lib)
        , palette_value(std::move(p))
        , desc(std::move(description)) {}

    // Already-added variant. Caller has done the add (directly via
    // add_palette OR via duplicate_palette, which goes through
    // add_palette internally). This factory captures the live palette
    // value + id, then treats the first execute() as a no-op so push()
    // doesn't double-add. Subsequent redoes (after an undo) replay the
    // add normally. Mirrors AddSwatchCommand::already_added in shape.
    //
    // Used by the duplicate-palette path: duplicate_palette returns
    // the new id, the caller looks up the resulting Palette, and wraps
    // it in this factory so duplicate-then-undo cleans up the
    // duplicate. The library's duplicate_palette stays the single
    // source of truth for "what's in a duplicate"; this factory just
    // makes the result undoable.
    //
    // Required: live_value.id must be non-empty (the live id of the
    // palette in the library).
    static std::unique_ptr<AddPaletteCommand>
    already_added(color::SwatchLibrary* lib,
                  color::Palette live_value,
                  std::string description = "Add palette") {
        auto cmd = std::make_unique<AddPaletteCommand>(
            lib, std::move(live_value), std::move(description));
        cmd->m_assigned_id = cmd->palette_value.id;
        cmd->m_first_execute_consumed = true;
        return cmd;
    }

    // Caller declares whether the new palette should become active
    // after this command runs (including on redo). Use when the call
    // site's UX contract makes the new palette active — both
    // on_new_palette and on_duplicate_palette do. See m_make_active
    // doc above for rationale.
    AddPaletteCommand& set_make_active(bool v) {
        m_make_active = v;
        return *this;
    }

    void execute() override {
        if (!library) return;
        if (m_first_execute_consumed) {
            // The palette is already in the library (caller did the
            // add). Drop this execute, but flip the flag so subsequent
            // redoes go through the normal add path. Mirrors
            // AddSwatchCommand's same-named pattern.
            //
            // NOTE: we do NOT touch active_palette here even when
            // m_make_active is true, because for the already_added
            // path the caller has already set active live (the
            // duplicate-palette UX runs set_active_palette as part of
            // the panel handler, BEFORE wrapping in this factory).
            // active is correct at first-execute time; m_make_active
            // only kicks in on subsequent redoes via the normal-add
            // branch below.
            m_first_execute_consumed = false;
            return;
        }
        // First execute: id may be empty -> library mints. Subsequent
        // redoes: id is non-empty (carried in m_assigned_id and
        // mirrored back into palette_value.id below).
        color::PaletteId result = library->add_palette(palette_value);
        if (result.empty()) {
            // add_palette rejected — typically a redo-after-collision
            // (shouldn't happen since undo just freed the id). Library
            // logs WARN; we leave m_assigned_id empty so undo() becomes
            // a no-op.
            return;
        }
        m_assigned_id = result;
        // Mirror the assigned id back into palette_value so a second
        // redo (after another undo) targets the same id with a pre-set
        // value.
        palette_value.id = result;
        // s243 m1 fix: restore active state on redo. The undo that
        // preceded this redo called remove_palette, which cleared
        // m_active_palette as a side effect when this palette had been
        // active. The caller's UX contract (panel duplicate/new) is
        // "the new palette becomes active"; honour it on redo too.
        // Re-emit signal_palette_changed so the panel rebuilds with
        // the correct active state — same ordering-subtlety fix as
        // RemovePaletteCommand::undo().
        if (m_make_active) {
            library->set_active_palette(result);
            library->signal_palette_changed().emit(result);
        }
    }

    void undo() override {
        if (!library) return;
        if (m_assigned_id.empty()) return;  // execute failed earlier
        library->remove_palette(m_assigned_id);
        // Don't clear m_assigned_id — redo needs to re-add with the
        // same id.
    }

    std::string description() const override { return desc; }

    bool preserves_selection() const override { return true; }
};

// ── RemovePaletteCommand (s243 m1) ────────────────────────────────────────────
//
// Removes a custom-tier Palette from the SwatchLibrary. Mirrors
// RemoveSwatchCommand in shape, but with one extra captured dimension:
// the active_palette state.
//
// What SwatchLibrary::remove_palette destroys vs what undo must restore:
//   * The palette record itself      -> restored via add_palette(snapshot)
//                                       with pre-set id (id was freed
//                                       by the remove). The palette's
//                                       swatch list (cross-tier
//                                       references included) round-trips
//                                       intact because it's part of the
//                                       Palette value type.
//   * active_palette                  -> if the removed palette WAS the
//                                       active one, remove_palette
//                                       cleared m_active_palette. Undo
//                                       restores by capturing the
//                                       pre-remove active state and
//                                       calling set_active_palette on it.
//                                       If active was something else,
//                                       it survived remove_palette
//                                       unchanged and we don't touch it
//                                       on undo.
//
// What is NOT restored: nothing else. Palettes don't have a per-project
// MRU-style transient state analogous to swatch recents.
//
// Defaults-tier guard: panel never builds a RemovePaletteCommand for a
// defaults id (default palettes are read-only); the s243 m2 scripted
// `palettes delete` verb will also refuse on is_default_palette before
// constructing. The command itself doesn't enforce — symmetric with
// RemoveSwatchCommand.
//
// Snapshot timing: built in the ctor, BEFORE execute() runs. Standard
// panel usage:
//     auto cmd = std::make_unique<RemovePaletteCommand>(lib, id);
//     cmd->execute();
//     history->push(std::move(cmd));
// The library walk in the ctor sees the live, pre-remove state.
//
// preserves_selection: true. Library mutation, not a scene-tree change.
struct RemovePaletteCommand : CurvzCommand {
    color::SwatchLibrary* library;
    color::Palette        palette_value;       // full snapshot pre-remove
    color::PaletteId      active_before;       // m_active_palette pre-remove,
                                               // captured to restore on undo
                                               // iff this command's palette
                                               // was the active one
    std::string           desc;

    RemovePaletteCommand(color::SwatchLibrary* lib,
                         const color::PaletteId& id,
                         std::string description = "Delete palette")
        : library(lib)
        , desc(std::move(description)) {
        if (!library) return;
        const color::Palette* existing = library->find_palette(id);
        if (!existing) return;
        palette_value = *existing;
        // Capture active state pre-remove. We only need to restore it
        // on undo if it pointed at THIS palette; remove_palette only
        // clears m_active_palette when the removed palette was active.
        active_before = library->active_palette();
    }

    void execute() override {
        if (!library) return;
        if (palette_value.id.empty()) return;  // ctor failed to snapshot
        library->remove_palette(palette_value.id);
    }

    void undo() override {
        if (!library) return;
        if (palette_value.id.empty()) return;
        // Re-add with the original id. add_palette accepts pre-set ids
        // as long as they're not in either tier — the id is free because
        // execute() just removed it.
        //
        // ORDERING SUBTLETY (mirrors RemoveSwatchCommand::undo()):
        // add_palette fires signal_palette_added synchronously, which
        // triggers the panel's refresh() — at that moment
        // m_active_palette has NOT yet been restored (set_active_palette
        // runs below), so the panel's dropdown reads an empty active
        // and renders stale. Fix: set active first, THEN re-emit
        // signal_palette_changed so the panel refreshes a second time
        // with the active correct. Double-fire is harmless (refresh is
        // idempotent; schedule_save is debounced). Same shape as
        // RemoveSwatchCommand's post-replay re-emit.
        color::PaletteId restored = library->add_palette(palette_value);
        if (restored.empty()) return;  // collision shouldn't happen; bail.

        // Restore active_palette IF this palette was the active one
        // pre-remove. If active was something else, it survived the
        // remove and we don't touch it.
        if (active_before == palette_value.id) {
            library->set_active_palette(palette_value.id);
            // Re-emit so panel refreshes with the correct active state.
            library->signal_palette_changed().emit(restored);
        }
    }

    std::string description() const override { return desc; }

    bool preserves_selection() const override { return true; }
};

// ── RenamePaletteCommand (s243 m1) ────────────────────────────────────────────
//
// Renames a custom-tier Palette in place. Lightweight — captures the
// palette id, the old name (for undo), and the new name (for redo).
// Mirrors the rename half of EditSwatchCommand in shape.
//
// Snapshot timing: built in the ctor, BEFORE execute() runs. The ctor
// reads the pre-rename name from the library. Standard panel usage:
//     auto cmd = std::make_unique<RenamePaletteCommand>(lib, id, "New name");
//     cmd->execute();
//     history->push(std::move(cmd));
//
// Defaults-tier guard: panel never builds a RenamePaletteCommand for a
// defaults id. The s243 m2 scripted path will also refuse on
// is_default_palette before constructing.
//
// preserves_selection: true. Library mutation, not a scene-tree change.
struct RenamePaletteCommand : CurvzCommand {
    color::SwatchLibrary* library;
    color::PaletteId      palette_id;
    std::string           old_name;     // captured in ctor
    std::string           new_name;     // captured by caller
    bool                  m_snapshot_ok = false;  // true iff ctor found the palette
    std::string           desc;

    RenamePaletteCommand(color::SwatchLibrary* lib,
                         color::PaletteId id,
                         std::string new_name_,
                         std::string description = "Rename palette")
        : library(lib)
        , palette_id(std::move(id))
        , new_name(std::move(new_name_))
        , desc(std::move(description)) {
        if (!library) return;
        const color::Palette* existing = library->find_palette(palette_id);
        if (!existing) return;
        old_name = existing->name;
        m_snapshot_ok = true;
    }

    void execute() override {
        if (!library || !m_snapshot_ok) return;
        library->rename_palette(palette_id, new_name);
    }

    void undo() override {
        if (!library || !m_snapshot_ok) return;
        library->rename_palette(palette_id, old_name);
    }

    std::string description() const override { return desc; }

    bool preserves_selection() const override { return true; }
};

// ── PaletteMembershipCommand (s243 m1) ────────────────────────────────────────
//
// One atomic command for membership mutations between palettes. m1 ships
// only the MOVE case (closes the on_ctx_move_to_palette site's two-call
// undo gap — a move feels like one user gesture and undo should restore
// both sides in one step). The pure-add and pure-remove cases are
// supported by the underlying apply_() machinery but no factory ships
// for them in m1; a future milestone that needs them can add
// already_added / already_removed factories alongside the existing
// already_moved.
//
// State captured as a "before" and "after" snapshot of the swatch's
// membership in up to two palettes. Each side records:
//   * palette_id   — empty means "not in any palette on this side"
//   * index        — position within that palette's swatches vector;
//                    unused when palette_id is empty
//
// Cases (all four supported by apply_(); only Move is exercised in m1):
//   * Pure add:    before = (empty, 0), after = (pid, idx)
//   * Pure remove: before = (pid, idx), after = (empty, 0)
//   * Move:        before = (src_pid, src_idx), after = (dst_pid, dst_idx)
//   * No-op:       before == after — skipped by apply_().
//
// Execute strategy: apply the AFTER snapshot. If after.palette_id is
// empty, remove from before's palette. If before.palette_id is empty,
// add to after's palette + reorder to after.index. If both are non-empty
// (move), remove from before + add to after + reorder.
//
// Undo strategy: apply the BEFORE snapshot, symmetrically.
//
// Why factories: the call sites think in user-gesture terms ("move",
// "add", "remove"), not in two-snapshot terms. Factories take the
// user-gesture arguments and snapshot the necessary library state
// internally. The raw before/after fields can be tricky to populate
// correctly (especially the index-after-the-remove subtleties); factories
// hide that.
//
// preserves_selection: true. Library mutation, not a scene-tree change.
struct PaletteMembershipCommand : CurvzCommand {
    struct Side {
        color::PaletteId palette_id;   // empty = swatch not in any palette
                                       // on this side
        std::size_t      index = 0;    // position within that palette's
                                       // swatches vector; unused when
                                       // palette_id is empty
    };

    color::SwatchLibrary* library;
    color::SwatchId       swatch_id;
    Side                  before;
    Side                  after;
    bool                  m_snapshot_ok = false;
    std::string           desc;

    PaletteMembershipCommand(color::SwatchLibrary* lib,
                             color::SwatchId sid,
                             Side before_,
                             Side after_,
                             std::string description)
        : library(lib)
        , swatch_id(std::move(sid))
        , before(std::move(before_))
        , after(std::move(after_))
        , desc(std::move(description)) {
        m_snapshot_ok = (library != nullptr) && !swatch_id.empty();
    }

    // Factory: swatch was just moved from src palette to dst palette
    // (caller already did the remove_from_palette + add_to_palette).
    // Captures the src palette pre-remove index (caller passes it in,
    // since by now the library has lost it) and reads the dst palette
    // post-add index back from the library. Mirrors the
    // AddSwatchCommand::already_added live-state-snapshot shape.
    //
    // m1 ships only the already_moved factory; the pure-add and
    // pure-remove cases are not exercised by any current call site.
    // When script verbs `palettes add_swatch_to` / `remove_swatch_from`
    // (or similar) land in a later milestone, add `already_added` /
    // `already_removed` factories then — the apply_() machinery
    // already handles all three sub-cases (and the no-op fourth).
    static std::unique_ptr<PaletteMembershipCommand>
    already_moved(color::SwatchLibrary* lib,
                  color::SwatchId sid,
                  color::PaletteId src_pid, std::size_t src_index,
                  color::PaletteId dst_pid,
                  std::string description = "Move swatch between palettes") {
        Side before{std::move(src_pid), src_index};
        // Read dst index post-move so undo's after-state matches reality.
        std::size_t dst_index = 0;
        if (lib && !dst_pid.empty()) {
            if (const color::Palette* p = lib->find_palette(dst_pid)) {
                for (std::size_t i = 0; i < p->swatches.size(); ++i) {
                    if (p->swatches[i] == sid) { dst_index = i; break; }
                }
            }
        }
        Side after{std::move(dst_pid), dst_index};
        auto cmd = std::make_unique<PaletteMembershipCommand>(
            lib, std::move(sid), std::move(before), std::move(after),
            std::move(description));
        cmd->m_already_applied = true;
        return cmd;
    }

    void execute() override {
        if (!library || !m_snapshot_ok) return;
        if (m_already_applied) {
            // Caller did the mutation; treat this execute as a no-op,
            // but flip the flag so subsequent redoes go through the
            // normal apply path (mirrors AddSwatchCommand's
            // m_first_execute_consumed pattern).
            m_already_applied = false;
            return;
        }
        apply_(after, /*replacing=*/before);
    }

    void undo() override {
        if (!library || !m_snapshot_ok) return;
        apply_(before, /*replacing=*/after);
    }

    std::string description() const override { return desc; }

    bool preserves_selection() const override { return true; }

private:
    bool m_already_applied = false;

    // Transition the swatch from `replacing` state to `target` state.
    // The four sub-cases are:
    //   * target empty, replacing non-empty -> pure remove from replacing.pid
    //   * target non-empty, replacing empty -> pure add to target.pid + reorder
    //   * both non-empty -> move (remove from replacing.pid + add to
    //                       target.pid + reorder)
    //   * both empty -> no-op (caller built a degenerate command)
    void apply_(const Side& target, const Side& replacing) {
        const bool to_empty   = target.palette_id.empty();
        const bool from_empty = replacing.palette_id.empty();
        if (to_empty && from_empty) return;
        // Remove from the OLD palette first (if any).
        if (!from_empty) {
            library->remove_from_palette(replacing.palette_id, swatch_id);
        }
        // Add to the NEW palette (if any) and place at the captured index.
        if (!to_empty) {
            library->add_to_palette(target.palette_id, swatch_id);
            library->reorder_in_palette(target.palette_id, swatch_id,
                                        target.index);
        }
    }
};

// ── BindStyleCommand (S79 m4a) ────────────────────────────────────────────────
//
// Binds one or more SceneNodes to a Style id and re-materialises their
// fill / stroke caches from the library. Atomic across the multi-target
// case (toolbar / panel "Bind to style X" with multi-select pushes ONE
// command, not one-per-node, so undo restores everything in a single step).
//
// Snapshot scope: full appearance per target. Capturing only bound_style
// would be cuter but wrong — if the previously-bound style is edited
// between bind and undo, restoring just the binding would project the
// node to the *current* state of the previous style, not the appearance
// it had at bind time. Six fields per target (bound_style, fill,
// fill_swatch_id, stroke, stroke_swatch_id) is the standard
// EditAppearanceCommand pattern, with the addition of bound_style.
//
// Redo policy: re-materialise from the live library, NOT replay the
// cached after-state. Reasoning: if the user binds, edits the style,
// undoes the bind, then redoes — they expect the redo'd binding to
// reflect the *current* style state, not the appearance at the
// moment of the original bind. This matches how the cross-doc walk
// in m3d behaves: bound nodes always reflect the live library entry.
// The asymmetry (undo restores snapshot, redo re-materialises) is
// intentional and mirrors the inherent asymmetry of binding
// (forward = "track the library", reverse = "freeze at the cached
// pre-bind appearance").
//
// Funnel discipline: this command writes bound_style and fill/stroke
// directly. It does NOT route through style::mutate_appearance — that
// funnel exists for break-on-override correctness, which a binding
// command IS the alternative to. Per StyleInterop.hpp lines 30–35,
// "the inspector's explicit 'Unbind' button uses UnbindStyleCommand,
// not [the funnel]"; same reasoning applies to bind.
//
// Library pointer: captured at push time, assumed to outlive the
// command. CommandHistory is owned by MainWindow, StyleLibrary by
// CurvzProject; both die with the project, so the pointer is valid
// for the entire history lifespan. Same pattern as EditSwatchCommand.
//
// Cross-doc propagation: this command does NOT fire
// signal_style_changed — the library is unchanged; only node bindings
// are. Other documents in the project carry their own bindings to
// the same style id, but they're unaffected by a bind on THIS doc's
// nodes. The Canvas redraw is driven externally via on_undo /
// on_redo's queue_draw call (matches the EditAppearanceCommand
// pattern; commands snapshot state, MainWindow drives redraw).
//
// preserves_selection: true. Binding/unbinding is an appearance change
// on the existing selection, not a structural mutation. Same as
// EditAppearanceCommand and EditSwatchCommand.
struct BindStyleCommand : CurvzCommand {
    // Per-target snapshot: full appearance state before the bind, plus
    // the bound_style id we're applying after. The "after" fill / stroke
    // / *_swatch_id are NOT cached — execute() / redo() recomputes them
    // from the live library every time (re-materialise policy above).
    struct TargetSnap {
        SceneNode*           obj;
        std::string          bound_style_before;
        FillStyle            fill_before;
        std::string          fill_swatch_id_before;
        StrokeStyle          stroke_before;
        std::string          stroke_swatch_id_before;
    };

    style::StyleLibrary*   library;
    std::string            new_style_id;   // the id to bind to
    std::vector<TargetSnap> snaps;
    std::string            desc;

    BindStyleCommand(style::StyleLibrary* lib,
                     std::string new_id,
                     std::vector<TargetSnap> targets,
                     std::string description = "Bind style")
        : library(lib)
        , new_style_id(std::move(new_id))
        , snaps(std::move(targets))
        , desc(std::move(description)) {}

    void execute() override {
        if (!library) return;
        for (auto& s : snaps) {
            if (!s.obj) continue;
            s.obj->bound_style = new_style_id;
            // Materialise from the LIVE library on every execute /
            // redo. If the style has been edited (or even renamed
            // out from under us — the id is stable) since the
            // original bind, we pick up the current state.
            //
            // Returns false if the id is unknown (style was
            // deleted between original bind and this redo). In
            // that case the node keeps its pre-bind fill/stroke
            // (still in the snapshot — we haven't touched them
            // yet on this target on this execute). Clearing
            // bound_style here would surprise the user; leaving
            // it intact lets a subsequent style-undelete (when
            // m4 wires it) restore the binding cleanly.
            if (!style::materialise_from_style(*s.obj, *library)) {
                // materialise_from_style logs WARN internally.
                // Restore the pre-bind cache so the node remains
                // visually stable until the binding can resolve.
                s.obj->fill            = s.fill_before;
                s.obj->fill_swatch_id  = s.fill_swatch_id_before;
                s.obj->stroke          = s.stroke_before;
                s.obj->stroke_swatch_id= s.stroke_swatch_id_before;
            }
        }
    }

    void undo() override {
        for (auto& s : snaps) {
            if (!s.obj) continue;
            s.obj->bound_style       = s.bound_style_before;
            s.obj->fill              = s.fill_before;
            s.obj->fill_swatch_id    = s.fill_swatch_id_before;
            s.obj->stroke            = s.stroke_before;
            s.obj->stroke_swatch_id  = s.stroke_swatch_id_before;
        }
    }

    std::string description() const override { return desc; }

    // Appearance change, not structural — selection survives undo/redo.
    bool preserves_selection() const override { return true; }
};

// ── UnbindStyleCommand (S79 m4a) ──────────────────────────────────────────────
//
// Inverse of BindStyleCommand for the inspector's explicit "Unbind"
// button. Clears bound_style on each target; the existing fill / stroke
// cache stands as-is (per the v1 break-on-override model — the cache
// at unbind time IS the appearance the user wants to keep).
//
// Snapshot scope: same six fields as BindStyleCommand, captured before
// unbind. Restoring on undo re-projects the binding AND the matching
// fill/stroke (in case the user edited fill/stroke directly between
// the unbind and the undo — though under v1 that'd require a manual
// re-bind first, since direct edits clear bound_style).
//
// Symmetric with BindStyleCommand on every other dimension: library
// pointer captured at push time, multi-target atomic, no cross-doc
// signal, redraw driven externally by on_undo/on_redo, preserves
// selection. The execute() body is simpler than BindStyleCommand's
// because there's no library lookup needed — the unbind action is
// just "clear bound_style," and the existing cache survives.
struct UnbindStyleCommand : CurvzCommand {
    struct TargetSnap {
        SceneNode*           obj;
        std::string          bound_style_before;
        FillStyle            fill_before;
        std::string          fill_swatch_id_before;
        StrokeStyle          stroke_before;
        std::string          stroke_swatch_id_before;
    };

    std::vector<TargetSnap> snaps;
    std::string             desc;

    UnbindStyleCommand(std::vector<TargetSnap> targets,
                       std::string description = "Unbind style")
        : snaps(std::move(targets))
        , desc(std::move(description)) {}

    void execute() override {
        for (auto& s : snaps) {
            if (!s.obj) continue;
            s.obj->bound_style.clear();
            // fill / stroke / *_swatch_id are deliberately untouched.
            // The pre-unbind cache IS the post-unbind appearance under
            // break-on-override v1.
        }
    }

    void undo() override {
        for (auto& s : snaps) {
            if (!s.obj) continue;
            s.obj->bound_style       = s.bound_style_before;
            s.obj->fill              = s.fill_before;
            s.obj->fill_swatch_id    = s.fill_swatch_id_before;
            s.obj->stroke            = s.stroke_before;
            s.obj->stroke_swatch_id  = s.stroke_swatch_id_before;
        }
    }

    std::string description() const override { return desc; }

    bool preserves_selection() const override { return true; }
};

// ── BindSwatchCommand (S82 m4f) ───────────────────────────────────────────────
//
// Binds a Swatch id to one or more SceneNodes on a single PaintSlot
// (Fill or Stroke). Atomic across multi-select on the SAME slot —
// Canvas::apply_swatch_to_selection takes one slot for the whole call,
// so one command per call captures the natural user intent.
//
// Why per-slot rather than per-node-with-both-slots: a swatch apply user
// action targets ONE slot (the user picked a fill or a stroke from the
// panel, not both at once). Two separate apply actions would push two
// separate commands and undo independently, which matches Illustrator
// behaviour.
//
// Snapshot scope: per-target, the slot's pre-bind FillStyle/StrokeStyle
// AND the slot's pre-bind swatch_id (which may already be non-empty if
// the user is rebinding an already-bound slot to a different swatch).
// bound_style_before is also captured because the swatch apply path
// routes through SwatchLibrary::set_paint, which uses the
// mutate_appearance funnel — so a Style binding gets broken too. Undo
// must restore that binding for parity with BindStyleCommand's contract.
//
// Funnel discipline: this command writes fill / fill_swatch_id (or
// stroke / stroke_swatch_id) directly. It does NOT route through
// mutate_appearance — same reasoning as BindStyleCommand. The funnel
// exists for break-on-override correctness, but a binding command IS
// the alternative to override; it is itself a binding write.
//
// Library pointer: captured at push time. SwatchLibrary lifetime is
// guaranteed by the project (same as EditSwatchCommand).
//
// Redo policy: replay the cached after-state directly. Asymmetric with
// BindStyleCommand (which re-materialises from the live library on
// redo). The reason for the asymmetry: BindStyleCommand re-materialises
// because a Style is a record (multiple fields) that may have been
// edited between bind and redo. A Swatch is a single colour, and
// EditSwatchCommand already lives in the same history stack — if the
// user edits a swatch, undoes the edit, undoes the bind, redoes the
// bind, the swatch's history is independent and the cached after-colour
// is correct for that snapshot. Re-materialising would actually be
// surprising in the swatch case (cross-history semantics from a single
// Ctrl+Y).
//
// preserves_selection: true. Same as all binding/appearance commands.
struct BindSwatchCommand : CurvzCommand {
    struct TargetSnap {
        SceneNode*      obj;
        // Pre-bind state for this target. Capturing the full FillStyle
        // / StrokeStyle pair lets undo restore the exact pre-bind cache
        // even if a previous Style binding is being broken in the same
        // step (mutate_appearance funnel through set_paint clears
        // bound_style as a side-effect of the bind).
        std::string     bound_style_before;
        FillStyle       fill_before;
        std::string     fill_swatch_id_before;
        StrokeStyle     stroke_before;
        std::string     stroke_swatch_id_before;
        // Post-bind cache for the slot only. The other slot survives
        // unchanged from *_before. Captured at construction by the
        // caller (Canvas::apply_swatch_to_selection in m4g) AFTER
        // calling set_paint, so the resolved FillStyle reflects the
        // library lookup at bind time.
        FillStyle       fill_after;          // valid iff slot == Fill
        StrokeStyle     stroke_after;        // valid iff slot == Stroke
    };

    color::SwatchLibrary*   library;
    color::PaintSlot        slot;
    std::string             new_swatch_id;   // the id being bound
    std::vector<TargetSnap> snaps;
    std::string             desc;

    BindSwatchCommand(color::SwatchLibrary* lib,
                      color::PaintSlot slot,
                      std::string new_id,
                      std::vector<TargetSnap> targets,
                      std::string description = "Bind swatch")
        : library(lib)
        , slot(slot)
        , new_swatch_id(std::move(new_id))
        , snaps(std::move(targets))
        , desc(std::move(description)) {}

    void execute() override {
        for (auto& s : snaps) {
            if (!s.obj) continue;
            // Break any Style binding (mirrors funnel behaviour). The
            // command is replaying what set_paint would do via the
            // funnel, but inline so we don't redundantly re-resolve
            // the swatch through the library on every redo.
            s.obj->bound_style.clear();
            if (slot == color::PaintSlot::Fill) {
                s.obj->fill            = s.fill_after;
                s.obj->fill_swatch_id  = new_swatch_id;
                // Stroke slot untouched — preserved from before-state
                // (which is also the after-state for the unmodified
                // slot, since we didn't write it).
            } else {  // Stroke
                s.obj->stroke           = s.stroke_after;
                s.obj->stroke_swatch_id = new_swatch_id;
            }
        }
    }

    void undo() override {
        for (auto& s : snaps) {
            if (!s.obj) continue;
            // Restore the full pre-bind state. We touch all four
            // appearance fields because the bind path may have cleared
            // a previous Style binding too (via the funnel inside
            // set_paint), and unbinding must reverse that completely.
            s.obj->bound_style       = s.bound_style_before;
            s.obj->fill              = s.fill_before;
            s.obj->fill_swatch_id    = s.fill_swatch_id_before;
            s.obj->stroke            = s.stroke_before;
            s.obj->stroke_swatch_id  = s.stroke_swatch_id_before;
        }
    }

    std::string description() const override { return desc; }
    bool preserves_selection() const override { return true; }
};

// ── UnbindSwatchCommand (S82 m4f) ─────────────────────────────────────────────
//
// Inverse of BindSwatchCommand for the (forthcoming, m4h) inspector
// "Unbind swatch" affordance. Per-slot, multi-target, atomic. Clears
// the slot's *_swatch_id while leaving the cached fill / stroke
// untouched — same break-on-override-style policy as UnbindStyleCommand
// (the cache at unbind time IS the appearance the user wants to keep).
//
// Snapshot scope: per-target swatch_id_before and the slot's
// FillStyle/StrokeStyle cache for the rare "user edited the cache
// between unbind and undo" case (which under v1 break-on-override
// requires a manual rebind first; harmless to capture anyway and keeps
// the command self-contained).
//
// Symmetric with UnbindStyleCommand on every dimension. No library
// pointer needed — the unbind action is just clearing the id field;
// no library lookup involved. Cross-doc propagation: none — this is
// a per-node binding clear, not a library mutation.
struct UnbindSwatchCommand : CurvzCommand {
    struct TargetSnap {
        SceneNode*      obj;
        // Pre-unbind id for the slot in question. The other slot's
        // id is not captured because we don't touch it.
        std::string     swatch_id_before;
        // Cache of the full appearance at unbind time, restored on
        // undo for parity with UnbindStyleCommand.
        FillStyle       fill_before;
        StrokeStyle     stroke_before;
    };

    color::PaintSlot        slot;
    std::vector<TargetSnap> snaps;
    std::string             desc;

    UnbindSwatchCommand(color::PaintSlot slot,
                        std::vector<TargetSnap> targets,
                        std::string description = "Unbind swatch")
        : slot(slot)
        , snaps(std::move(targets))
        , desc(std::move(description)) {}

    void execute() override {
        for (auto& s : snaps) {
            if (!s.obj) continue;
            if (slot == color::PaintSlot::Fill) {
                s.obj->fill_swatch_id.clear();
            } else {
                s.obj->stroke_swatch_id.clear();
            }
            // fill / stroke caches deliberately untouched — see header.
        }
    }

    void undo() override {
        for (auto& s : snaps) {
            if (!s.obj) continue;
            if (slot == color::PaintSlot::Fill) {
                s.obj->fill_swatch_id = s.swatch_id_before;
                s.obj->fill           = s.fill_before;
            } else {
                s.obj->stroke_swatch_id = s.swatch_id_before;
                s.obj->stroke           = s.stroke_before;
            }
        }
    }

    std::string description() const override { return desc; }
    bool preserves_selection() const override { return true; }
};

// ── AddStyleCommand (S81 m4c-3) ───────────────────────────────────────────────
//
// Adds a Style to the user tier of the StyleLibrary. The "+" button on
// StylesPanel pushes one of these per create action — both "Create empty"
// and "Create from selection" go through this command.
//
// Identity model:
//   * If `style_value.header.id` is empty at construction, execute()
//     calls library->add_style() and lets the library generate a fresh
//     "stl_<uuid>". The assigned id is captured into m_assigned_id so
//     undo() / redo() target the same id consistently.
//   * If `style_value.header.id` is non-empty (uncommon — m4c-3 callers
//     always pass empty), execute() preserves it via add_style's
//     pre-set-id branch. add_style rejects "app:" ids and collisions.
//
// Redo policy: replay the SAME id captured during the first execute.
// add_style with a pre-set id succeeds because the id was just freed
// by undo()'s remove_style call. This mirrors the swatch-create pattern
// (currently un-undoable; the open backlog AddSwatchCommand will follow
// this same shape).
//
// Library pointer captured at push time, lifetime guaranteed by the
// project (same as EditSwatchCommand / BindStyleCommand).
//
// Cross-doc propagation: signal_style_added fires on each successful
// add. The panel listens and refreshes; no bound objects exist yet at
// the moment of an "add" (newly-minted id has no bindings) so the
// cross-doc walk is a no-op until a subsequent bind. m3d's walk doesn't
// listen on signal_style_added at all — the add path doesn't need it.
//
// preserves_selection: true. Library mutation, not a scene-tree change.
struct AddStyleCommand : CurvzCommand {
    style::StyleLibrary* library;
    style::Style         style_value;     // captured at construction
    style::StyleId       m_assigned_id;   // captured on first execute()
    std::string          desc;

    AddStyleCommand(style::StyleLibrary* lib,
                    style::Style s,
                    std::string description = "Add style")
        : library(lib)
        , style_value(std::move(s))
        , desc(std::move(description)) {}

    void execute() override {
        if (!library) return;
        // First execute: id may be empty → library mints. Subsequent
        // redoes: id is non-empty (carried in m_assigned_id and
        // copied back into style_value.header.id below).
        style::StyleId result = library->add_style(style_value);
        if (result.empty()) {
            // add_style rejected — typically a redo-after-collision
            // (shouldn't happen since undo just freed the id) or an
            // app: id leaked in. Library logs WARN; we leave
            // m_assigned_id empty so undo() becomes a no-op.
            return;
        }
        m_assigned_id = result;
        // Mirror the assigned id back into style_value so a second redo
        // (after another undo) targets the same id with a pre-set value.
        style_value.header.id = result;
    }

    void undo() override {
        if (!library) return;
        if (m_assigned_id.empty()) return;  // execute failed earlier
        library->remove_style(m_assigned_id);
        // Don't clear m_assigned_id — redo needs to re-add with the
        // same id (so any external bindings made between the original
        // execute and this undo can resolve cleanly on redo).
    }

    std::string description() const override { return desc; }

    bool preserves_selection() const override { return true; }
};

// ── UpdateStyleCommand (S81 m4c-3) ────────────────────────────────────────────
//
// Records an in-place edit to a single user-tier Style. m4c-3 uses this
// for inline rename via the panel's double-click-to-rename Entry; m4d
// will use the same command for fill / stroke field edits in the
// inspector-style Style editor.
//
// Snapshot model: full Style value before AND after. update_style is a
// whole-record replace, so the natural snapshot scope is the whole Style.
// Captures more than strictly necessary for a rename (which only mutates
// header.name) but keeps the command shape stable across all the future
// edit kinds, and the cost is a couple of structs — Style has no heavy
// fields.
//
// Cross-doc propagation: update_style fires signal_style_changed →
// m3d's cross-doc walk re-materialises every bound SceneNode in every
// open document → queue_draw on each canvas. This command is the
// primary lever the cross-doc walk gets exercised by in m4c-3+;
// rename triggers the walk but the appearance projection is stable
// (id is the binding key, name is metadata), so bound objects don't
// visually shift. m4d's fill/stroke edits will produce the visible
// recolour.
//
// Library pointer captured at push time. App-tier ids reach update_style
// during execute and are rejected (returns false); the panel guards
// against this before pushing the command, but execute() is defensive.
//
// preserves_selection: true. Same rationale as EditSwatchCommand.
struct UpdateStyleCommand : CurvzCommand {
    style::StyleLibrary* library;
    style::StyleId       style_id;
    style::Style         before;
    style::Style         after;
    std::string          desc;

    UpdateStyleCommand(style::StyleLibrary* lib,
                       style::StyleId id,
                       style::Style b,
                       style::Style a,
                       std::string description = "Edit style")
        : library(lib)
        , style_id(std::move(id))
        , before(std::move(b))
        , after(std::move(a))
        , desc(std::move(description)) {}

    void execute() override { apply(after); }
    void undo()    override { apply(before); }

    std::string description() const override { return desc; }

    bool preserves_selection() const override { return true; }

private:
    void apply(const style::Style& s) {
        if (!library) return;
        // update_style requires the input's header.id to match the id
        // argument. Caller-built before/after both have the right id
        // (captured at the same time as style_id), but defensive copy
        // here makes the contract obvious in the implementation.
        style::Style copy = s;
        copy.header.id = style_id;
        library->update_style(style_id, std::move(copy));
    }
};

// ── RemoveStyleCommand (S81 m4c-3) ────────────────────────────────────────────
//
// Removes a user-tier Style from the StyleLibrary. The right-click
// "Delete" menu item on a user style row pushes one of these.
//
// Snapshot model: full Style value captured at construction (the panel
// reads the current Style from the library and passes a copy in).
// undo() re-adds via library->add_style() with the captured value's
// pre-set id — StyleLibrary::add_style accepts a pre-set id as long as
// it's free (which it is, because execute() just removed it).
//
// Cross-doc dangling-binding behaviour: m3d's walk listens on
// signal_style_removed and... actually, per StylesPanel.hpp comment
// and architecture notes, the live walk does NOT auto-clear bound_style
// on removal — it logs WARN and lets the post-load walk in
// CurvzProject::load handle dangling bindings on the next document
// open. This is the "drop-on-miss" policy: live walk = preserve,
// load walk = auto-clear. RemoveStyleCommand inherits that policy
// implicitly — execute() just removes; bound objects keep their
// cached fill/stroke and their dangling bound_style id; undo() re-adds
// the id and the bindings resolve again automatically on the next
// re-materialise pass.
//
// Per the m4c-3 handoff: usage-check warning dialog is deferred to
// m4c-4 / m5. m4c-3 deletes silently; bound objects keep their
// appearance and the binding goes dangling until next load.
//
// preserves_selection: true. Library mutation, not a scene-tree change.
struct RemoveStyleCommand : CurvzCommand {
    style::StyleLibrary* library;
    style::Style         style_value;   // full snapshot pre-remove
    std::string          desc;

    RemoveStyleCommand(style::StyleLibrary* lib,
                       style::Style s,
                       std::string description = "Delete style")
        : library(lib)
        , style_value(std::move(s))
        , desc(std::move(description)) {}

    void execute() override {
        if (!library) return;
        library->remove_style(style_value.header.id);
    }

    void undo() override {
        if (!library) return;
        // Re-add with the original id. add_style accepts pre-set ids
        // as long as they're not in either list and not "app:"
        // prefixed. The id is free (we removed it in execute()) and
        // the original id was a "stl_" id by construction (we never
        // create a RemoveStyleCommand for an app-tier style — the
        // panel guards against it).
        library->add_style(style_value);
    }

    std::string description() const override { return desc; }

    bool preserves_selection() const override { return true; }
};

// ── AddThemeCommand (S103 m2) ─────────────────────────────────────────────────
//
// Adds a Theme to the user tier of the ThemeLibrary. The S103 m2
// import path pushes one of these per imported theme, all wrapped in
// a single CompositeCommand for atomic-undo of the whole batch. m3's
// "Save current as theme…" dialog action will push one directly.
//
// Identity model — same as AddStyleCommand:
//   * If `theme_value.header.id` is empty at construction, execute()
//     calls library->add_theme() and lets the library generate a fresh
//     "thm_<uuid>". The assigned id is captured into m_assigned_id so
//     undo() / redo() target the same id consistently across the
//     do-undo-redo cycle.
//   * If non-empty, execute() preserves the pre-set id (used by
//     RemoveThemeCommand::undo, where re-adding the original id is the
//     correct semantic).
//
// No cross-doc propagation — themes don't bind. signal_theme_added is
// consumed only by the m3 dialog itself for refreshing its library
// list view; nothing else listens.
//
// preserves_selection: true. Library mutation, not a scene-tree change.
struct AddThemeCommand : CurvzCommand {
    theme::ThemeLibrary* library;
    theme::Theme         theme_value;     // captured at construction
    theme::ThemeId       m_assigned_id;   // captured on first execute()
    std::string          desc;

    AddThemeCommand(theme::ThemeLibrary* lib,
                    theme::Theme t,
                    std::string description = "Add theme")
        : library(lib)
        , theme_value(std::move(t))
        , desc(std::move(description)) {}

    void execute() override {
        if (!library) return;
        theme::ThemeId result = library->add_theme(theme_value);
        if (result.empty()) {
            // add_theme rejected — leave m_assigned_id empty so undo()
            // becomes a no-op. Library logs WARN.
            return;
        }
        m_assigned_id = result;
        // Mirror the assigned id back into theme_value so a subsequent
        // redo (after another undo) targets the same id with a pre-set
        // value — same id-stability story as AddStyleCommand.
        theme_value.header.id = result;
    }

    void undo() override {
        if (!library) return;
        if (m_assigned_id.empty()) return;
        library->remove_theme(m_assigned_id);
    }

    std::string description() const override { return desc; }

    bool preserves_selection() const override { return true; }
};

// ── UpdateThemeCommand (S103 m2 — wired by m3) ────────────────────────────────
//
// Records an in-place edit to a single user-tier Theme. Used by m3's
// rename / category-edit / save-over-existing flows. The theme apply
// itself is NOT routed through this command (apply is non-undoable in
// v1 — see Theme.hpp top-of-file rationale). What IS undoable is
// edits to the theme record in the library.
//
// Snapshot model: full Theme value before AND after. update_theme is
// a whole-record replace, so the natural snapshot scope is the whole
// Theme — captures more than strictly necessary for a rename but keeps
// the command shape stable across all the future edit kinds.
//
// preserves_selection: true. Library mutation, not a scene-tree change.
struct UpdateThemeCommand : CurvzCommand {
    theme::ThemeLibrary* library;
    theme::ThemeId       theme_id;
    theme::Theme         before;
    theme::Theme         after;
    std::string          desc;

    UpdateThemeCommand(theme::ThemeLibrary* lib,
                       theme::ThemeId id,
                       theme::Theme b,
                       theme::Theme a,
                       std::string description = "Edit theme")
        : library(lib)
        , theme_id(std::move(id))
        , before(std::move(b))
        , after(std::move(a))
        , desc(std::move(description)) {}

    void execute() override { apply(after); }
    void undo()    override { apply(before); }

    std::string description() const override { return desc; }

    bool preserves_selection() const override { return true; }

private:
    void apply(const theme::Theme& t) {
        if (!library) return;
        // update_theme requires the input's header.id to match the id
        // argument. Defensive copy makes the contract obvious.
        theme::Theme copy = t;
        copy.header.id = theme_id;
        library->update_theme(theme_id, std::move(copy));
    }
};

// ── RemoveThemeCommand (S103 m2 — wired by m3) ────────────────────────────────
//
// Removes a user-tier Theme from the ThemeLibrary. The dialog's "Delete"
// menu item on a user theme row pushes one of these.
//
// Snapshot model: full Theme value captured at construction. undo()
// re-adds via library->add_theme() with the captured value's pre-set
// id — add_theme accepts a pre-set id as long as it's free (which it
// is, because execute() just removed it).
//
// No dangling-binding cleanup needed — themes don't bind to docs. A
// removed theme just disappears from the library; previously-applied
// docs keep their applied values (apply was one-shot, not bound).
//
// preserves_selection: true. Library mutation, not a scene-tree change.
struct RemoveThemeCommand : CurvzCommand {
    theme::ThemeLibrary* library;
    theme::Theme         theme_value;   // full snapshot pre-remove
    std::string          desc;

    RemoveThemeCommand(theme::ThemeLibrary* lib,
                       theme::Theme t,
                       std::string description = "Delete theme")
        : library(lib)
        , theme_value(std::move(t))
        , desc(std::move(description)) {}

    void execute() override {
        if (!library) return;
        library->remove_theme(theme_value.header.id);
    }

    void undo() override {
        if (!library) return;
        // Re-add with the original id. add_theme accepts pre-set ids
        // when free (the id is free; execute removed it) and the id
        // was always a "thm_" id (RemoveThemeCommand is only used on
        // user-tier themes; the dialog guards against app-tier).
        library->add_theme(theme_value);
    }

    std::string description() const override { return desc; }

    bool preserves_selection() const override { return true; }
};

// ── GroupNodesCommand ─────────────────────────────────────────────────────────
//
// s242 m1. First new structural command built in the iid-migrated capture
// shape (CurvzProject* + iids, no raw SceneNode* fields). Add / Delete /
// Insert remain in their pre-s167 legacy shape; this command sets the
// precedent for the eventual structural migration sweep.
//
// Captures the state needed to wrap N scene objects in a new Group and
// to dissolve that Group on undo:
//   - parent_iid: the container (Layer or other Group) holding the
//     targets. Resolved at execute/undo time. If the parent has been
//     destroyed in the interim, the command no-ops — same posture as
//     EditAppearanceCommand / AlignObjectsCommand.
//   - target_iids: the objects to group, captured in PARENT Z-ORDER
//     (the order they appear in parent->children, NOT the selection
//     order). This is the same order Canvas::group_selection walks them
//     in; preserving it makes the group's internal stacking match the
//     user's expectations.
//   - target_indices_before: each target's original index in
//     parent->children, paired with target_iids by position. Used only
//     for undo's reverse-restoration.
//   - group_insert_index: where the new Group lands in parent->children
//     (clamped to the topmost target's original index — keeps the new
//     Group at the visually-frontmost target's z-position).
//   - group_iid: the iid of the new Group, generated by execute() and
//     captured for undo to find/dissolve. Must be stable across
//     execute/undo cycles so redo can re-create with the same iid.
//   - group_name: the gap-filled "Group N" name, generated by
//     execute() and captured for redo consistency.
//
// Execute semantics:
//   1. Resolve parent. No-op if gone.
//   2. Resolve all targets. ATOMIC: if ANY target is gone, the whole
//      command no-ops. Partial-group would leave the doc in a
//      semantically-mangled state (some objects grouped, others not,
//      with a mystery Group containing a subset of the originally-
//      intended children). Conservative bail-out is the right call.
//   3. Create a new Group with the captured iid + name.
//   4. For each target in z-order: find by pointer match in
//      parent->children, move into the Group, erase the parent slot.
//   5. Insert the Group at group_insert_index (clamped to current size).
//   6. Invalidate the owning doc's iid index (structural change).
//
// Undo semantics:
//   1. Resolve parent. No-op if gone.
//   2. Resolve the Group via group_iid. No-op if gone.
//   3. Locate the Group's index in parent->children.
//   4. Move each child of the Group back into parent at its captured
//      target_indices_before[i]. Insert in ASCENDING index order so
//      each insert lands at the right slot without index-shift issues.
//   5. Erase the now-empty Group from parent.
//   6. Invalidate doc iid index.
//
// On redo (re-execute): the captured group_iid is reused so any
// dangling references (e.g. selection state in canvas) continue to
// match. The Group's children come back from the same target_iids;
// because UUIDs are unique and the children weren't destroyed (just
// moved out of the Group on undo), find_by_iid resolves them.
struct GroupNodesCommand : CurvzCommand {
    CurvzProject*            proj;
    std::string              parent_iid;
    std::vector<std::string> target_iids;
    std::vector<int>         target_indices_before;
    int                      group_insert_index;
    std::string              group_iid;
    std::string              group_name;
    std::string              desc;

    GroupNodesCommand(CurvzProject* proj,
                      std::string parent_iid,
                      std::vector<std::string> target_iids,
                      std::vector<int> target_indices_before,
                      int group_insert_index,
                      std::string group_iid,
                      std::string group_name,
                      std::string desc = "Group objects")
        : proj(proj)
        , parent_iid(std::move(parent_iid))
        , target_iids(std::move(target_iids))
        , target_indices_before(std::move(target_indices_before))
        , group_insert_index(group_insert_index)
        , group_iid(std::move(group_iid))
        , group_name(std::move(group_name))
        , desc(std::move(desc)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return desc; }
    // iid-based capture means no raw SceneNode* is held; default
    // references() == false is correct.
};

// ── UngroupNodeCommand ────────────────────────────────────────────────────────
//
// s242 m1. Symmetric counterpart to GroupNodesCommand. Dissolves a Group:
// removes it from its parent, promotes its children to the parent's
// children vector at the Group's original index.
//
// Captures:
//   - group_iid: the Group being dissolved (resolved on execute).
//   - parent_iid: the Group's parent at execute time (the Group's
//     container — Layer or other Group).
//   - group_index_before: the Group's index in parent->children at
//     execute time. Children are promoted to parent at this index +
//     position-within-group.
//   - group_name: captured for undo's restoration of the Group with
//     its original name.
//   - child_iids: the Group's children in their order inside the
//     Group (children[0], children[1], ...). Used by undo to find
//     them back in parent and re-collect them into a re-created Group.
//
// Execute semantics:
//   1. Resolve Group. No-op if gone.
//   2. Resolve parent. No-op if gone (shouldn't happen — Group's
//      parent is part of the doc tree by construction).
//   3. For each child of the Group: move out into parent at
//      group_index_before + position. Walk in REVERSE order so each
//      insertion lands at the lowest-index slot without index-shift
//      issues (matches Canvas::ungroup_selection's loop direction).
//   4. Erase the (now-empty) Group from parent.
//   5. Invalidate doc iid index.
//
// Undo semantics:
//   1. Resolve parent. No-op if gone.
//   2. Resolve each child via child_iids. ATOMIC: if any is gone, the
//      whole command no-ops (same conservative posture as
//      GroupNodesCommand).
//   3. Create a new Group with the captured group_iid + group_name.
//   4. Find each child in parent->children by raw pointer match, move
//      it into the new Group's children (preserve capture order), erase
//      the parent slot. Walk in ASCENDING child order to preserve
//      group's internal stacking.
//   5. Insert the Group at group_index_before (clamped).
//   6. Invalidate doc iid index.
struct UngroupNodeCommand : CurvzCommand {
    CurvzProject*            proj;
    std::string              group_iid;
    std::string              parent_iid;
    int                      group_index_before;
    std::string              group_name;
    std::vector<std::string> child_iids;
    std::string              desc;

    UngroupNodeCommand(CurvzProject* proj,
                       std::string group_iid,
                       std::string parent_iid,
                       int group_index_before,
                       std::string group_name,
                       std::vector<std::string> child_iids,
                       std::string desc = "Ungroup")
        : proj(proj)
        , group_iid(std::move(group_iid))
        , parent_iid(std::move(parent_iid))
        , group_index_before(group_index_before)
        , group_name(std::move(group_name))
        , child_iids(std::move(child_iids))
        , desc(std::move(desc)) {}

    void execute() override;  // see CommandHistory.cpp
    void undo()    override;  // see CommandHistory.cpp
    std::string description() const override { return desc; }
    // iid-based capture; default references() == false is correct.
};

class CommandHistory {
public:
    static constexpr int MAX_HISTORY = 500;

    void push(std::unique_ptr<CurvzCommand> cmd);
    void undo();
    void redo();

    bool can_undo() const { return !m_undo_stack.empty(); }
    bool can_redo() const { return !m_redo_stack.empty(); }

    const CurvzCommand* peek_undo() const {
        return m_undo_stack.empty() ? nullptr : m_undo_stack.back().get();
    }
    const CurvzCommand* peek_redo() const {
        return m_redo_stack.empty() ? nullptr : m_redo_stack.front().get();
    }

    // Returns the most recently pushed command, or nullptr if stack is empty.
    // Used by PropertiesPanel to coalesce inspector edits in place.
    CurvzCommand* last_command() const {
        return m_undo_stack.empty() ? nullptr : m_undo_stack.back().get();
    }

    std::string undo_description() const;
    std::string redo_description() const;

    // s165 m3 — drop any command from either stack whose captured raw
    // pointers reference `target`. Called from Canvas::scrub_node_refs
    // when a SceneNode is destroyed, so the undo/redo stacks stay
    // consistent with the live document tree. Without this, a command
    // that captured a now-freed SceneNode crashes on undo() when it
    // dereferences the dangling pointer.
    //
    // Implementation: erase-remove on both deques. Order is preserved.
    // Commands that don't override CurvzCommand::references() default to
    // `false` and are kept (safe assumption: they captured by id /
    // unique_ptr / value, not raw pointer).
    int scrub_command_history(const SceneNode* target);

private:
    std::deque<std::unique_ptr<CurvzCommand>> m_undo_stack;
    std::deque<std::unique_ptr<CurvzCommand>> m_redo_stack;
};

} // namespace Curvz
