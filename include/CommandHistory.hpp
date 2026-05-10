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
    bool        before_bold = false, before_italic = false;
    FillStyle   before_fill;
    StrokeStyle before_stroke;
    // after state
    std::string after_content, after_family, after_anchor, after_align;
    double      after_x = 0, after_y = 0, after_size = 0;
    bool        after_bold = false, after_italic = false;
    FillStyle   after_fill;
    StrokeStyle after_stroke;

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
        c.before_bold    = o->text_bold;
        c.before_italic  = o->text_italic;
        c.before_fill    = o->fill;
        c.before_stroke  = o->stroke;
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
        after_bold    = o->text_bold;
        after_italic  = o->text_italic;
        after_fill    = o->fill;
        after_stroke  = o->stroke;
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
        o->text_bold       = after ? after_bold    : before_bold;
        o->text_italic     = after ? after_italic  : before_italic;
        o->fill            = after ? after_fill    : before_fill;
        o->stroke          = after ? after_stroke  : before_stroke;
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
