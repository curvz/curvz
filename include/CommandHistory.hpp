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
        for (int i = (int)steps.size()-1; i >= 0; --i) steps[i]->undo();
    }
    std::string description() const override { return desc; }
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
};

// ── MoveObjectCommand ─────────────────────────────────────────────────────────
// Kept for API compatibility. Path moves use EditPathCommand instead.
struct MoveObjectCommand : CurvzCommand {
    SceneNode* obj;
    double before_x, before_y, after_x, after_y;
    MoveObjectCommand(SceneNode* o, double bx, double by, double ax, double ay)
        : obj(o), before_x(bx), before_y(by), after_x(ax), after_y(ay) {}
    void execute() override {
        if (obj->is_text())  { obj->text_x  = after_x;  obj->text_y  = after_y;  }
        if (obj->is_image()) { obj->image_x = after_x;  obj->image_y = after_y;  }
    }
    void undo() override {
        if (obj->is_text())  { obj->text_x  = before_x; obj->text_y  = before_y; }
        if (obj->is_image()) { obj->image_x = before_x; obj->image_y = before_y; }
    }
    std::string description() const override { return "Move object"; }
};

// ── EditPathCommand ───────────────────────────────────────────────────────────
// Stores before/after PathData snapshots for any node-level edit.
struct EditPathCommand : CurvzCommand {
    SceneNode* obj;
    PathData   before;
    PathData   after;
    std::string desc;

    EditPathCommand(SceneNode* obj, PathData before, PathData after,
                    std::string desc = "Edit path")
        : obj(obj)
        , before(std::move(before))
        , after(std::move(after))
        , desc(std::move(desc)) {}

    void execute() override { if (obj->path) *obj->path = after;  }
    void undo()    override { if (obj->path) *obj->path = before; }
    std::string description() const override { return desc; }
    bool preserves_selection() const override { return true; }
};

// ── EditAppearanceCommand ─────────────────────────────────────────────────────
// Stores before/after fill + stroke snapshots for inspector appearance edits.
// ── TextEditCommand ───────────────────────────────────────────────────────────
// Full snapshot of all text node fields — used by commit_text_edit for both
// new nodes and edits to existing ones. Undo restores the complete before state.
struct TextEditCommand : CurvzCommand {
    SceneNode*  obj;
    // before state
    std::string before_content, before_family, before_anchor, before_align;
    double      before_x, before_y, before_size;
    bool        before_bold, before_italic;
    FillStyle   before_fill;
    StrokeStyle before_stroke;
    // after state
    std::string after_content, after_family, after_anchor, after_align;
    double      after_x, after_y, after_size;
    bool        after_bold, after_italic;
    FillStyle   after_fill;
    StrokeStyle after_stroke;

    static TextEditCommand snapshot_before(SceneNode* o) {
        TextEditCommand c;
        c.obj            = o;
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
    void execute() override { apply(obj, true);  }
    void undo()    override { apply(obj, false); }
    std::string description() const override { return "Edit text"; }

    TextEditCommand() = default;  // needed for Canvas member storage
};

// S82 m4f: swatch-id capture. Inspector / eyedropper / broadcast writes
// route through style::mutate_appearance, which (as of S82) clears both
// bound_style AND *_swatch_id on direct override. Restoring on undo
// requires the full pre-edit binding state, not just the colour cache —
// otherwise undo restores the colour but leaves the binding dangling
// (the latent bug). Eight fields total: fill / stroke / fill_swatch_id /
// stroke_swatch_id, before and after. The legacy 4-arg constructor is
// retained for callers that don't carry swatch state (none in-tree as
// of S82, but cheap to keep for source compatibility).
struct EditAppearanceCommand : CurvzCommand {
    SceneNode* obj;
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

    EditAppearanceCommand(SceneNode* obj,
                          FillStyle fb, StrokeStyle sb,
                          FillStyle fa, StrokeStyle sa,
                          std::string fsib, std::string ssib,
                          std::string fsia, std::string ssia,
                          std::string bsb,  std::string bsa,
                          std::string desc = "Edit appearance")
        : obj(obj)
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
    EditAppearanceCommand(SceneNode* obj,
                          FillStyle fb, StrokeStyle sb,
                          FillStyle fa, StrokeStyle sa,
                          std::string fsib, std::string ssib,
                          std::string fsia, std::string ssia,
                          std::string desc = "Edit appearance")
        : EditAppearanceCommand(obj,
                                std::move(fb), std::move(sb),
                                std::move(fa), std::move(sa),
                                std::move(fsib), std::move(ssib),
                                std::move(fsia), std::move(ssia),
                                std::string{}, std::string{},
                                std::move(desc)) {}

    // Legacy 4-arg constructor — pre-S82 callers that don't track swatch
    // bindings. Equivalent to passing empty swatch ids on both ends.
    // Safe because callers in this category never bind/unbind via swatch.
    EditAppearanceCommand(SceneNode* obj,
                          FillStyle fb, StrokeStyle sb,
                          FillStyle fa, StrokeStyle sa,
                          std::string desc = "Edit appearance")
        : EditAppearanceCommand(obj,
                                std::move(fb), std::move(sb),
                                std::move(fa), std::move(sa),
                                std::string{}, std::string{},
                                std::string{}, std::string{},
                                std::string{}, std::string{},
                                std::move(desc)) {}

    void execute() override {
        obj->fill              = fill_after;
        obj->stroke            = stroke_after;
        obj->fill_swatch_id    = fill_swatch_id_after;
        obj->stroke_swatch_id  = stroke_swatch_id_after;
        obj->bound_style       = bound_style_after;
    }
    void undo() override {
        obj->fill              = fill_before;
        obj->stroke            = stroke_before;
        obj->fill_swatch_id    = fill_swatch_id_before;
        obj->stroke_swatch_id  = stroke_swatch_id_before;
        obj->bound_style       = bound_style_before;
    }
    std::string description() const override { return desc; }
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
struct EditObjectCommand : CurvzCommand {
    SceneNode*  obj;
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

    EditObjectCommand(SceneNode* obj,
                      PathData pb, FillStyle fb, StrokeStyle sb,
                      PathData pa, FillStyle fa, StrokeStyle sa,
                      std::string fsib, std::string ssib,
                      std::string fsia, std::string ssia,
                      std::string bsb,  std::string bsa,
                      ShadowParams shb, ShadowParams sha,
                      std::string desc = "Edit object")
        : obj(obj)
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
    EditObjectCommand(SceneNode* obj,
                      PathData pb, FillStyle fb, StrokeStyle sb,
                      PathData pa, FillStyle fa, StrokeStyle sa,
                      std::string fsib, std::string ssib,
                      std::string fsia, std::string ssia,
                      std::string bsb,  std::string bsa,
                      std::string desc = "Edit object")
        : EditObjectCommand(obj,
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
    EditObjectCommand(SceneNode* obj,
                      PathData pb, FillStyle fb, StrokeStyle sb,
                      PathData pa, FillStyle fa, StrokeStyle sa,
                      std::string fsib, std::string ssib,
                      std::string fsia, std::string ssia,
                      std::string desc = "Edit object")
        : EditObjectCommand(obj,
                            std::move(pb), std::move(fb), std::move(sb),
                            std::move(pa), std::move(fa), std::move(sa),
                            std::move(fsib), std::move(ssib),
                            std::move(fsia), std::move(ssia),
                            std::string{}, std::string{},
                            std::move(desc)) {}

    // Legacy 6-arg constructor — pre-S82 source compatibility.
    EditObjectCommand(SceneNode* obj,
                      PathData pb, FillStyle fb, StrokeStyle sb,
                      PathData pa, FillStyle fa, StrokeStyle sa,
                      std::string desc = "Edit object")
        : EditObjectCommand(obj,
                            std::move(pb), std::move(fb), std::move(sb),
                            std::move(pa), std::move(fa), std::move(sa),
                            std::string{}, std::string{},
                            std::string{}, std::string{},
                            std::string{}, std::string{},
                            std::move(desc)) {}

    void execute() override {
        if (obj->path) *obj->path = path_after;
        obj->fill              = fill_after;
        obj->stroke            = stroke_after;
        obj->fill_swatch_id    = fill_swatch_id_after;
        obj->stroke_swatch_id  = stroke_swatch_id_after;
        obj->bound_style       = bound_style_after;
        obj->write_shadow(shadow_after);   // S97 m3
    }
    void undo() override {
        if (obj->path) *obj->path = path_before;
        obj->fill              = fill_before;
        obj->stroke            = stroke_before;
        obj->fill_swatch_id    = fill_swatch_id_before;
        obj->stroke_swatch_id  = stroke_swatch_id_before;
        obj->bound_style       = bound_style_before;
        obj->write_shadow(shadow_before);  // S97 m3
    }
    std::string description() const override { return desc; }
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
// Undo/redo for scale/rotate/skew applied to an Image node.
// Stores before/after image geometry + transform matrix.
struct ScaleImageCommand : CurvzCommand {
    struct Snap {
        SceneNode* obj;
        double bef_x, bef_y, bef_w, bef_h;
        Transform  bef_t;
        double aft_x, aft_y, aft_w, aft_h;
        Transform  aft_t;
    };
    std::vector<Snap> snaps;
    std::string desc;

    ScaleImageCommand(std::vector<Snap> s, std::string d = "Transform image")
        : snaps(std::move(s)), desc(std::move(d)) {}

    void execute() override {
        for (auto& s : snaps) {
            s.obj->image_x = s.aft_x; s.obj->image_y = s.aft_y;
            s.obj->image_w = s.aft_w; s.obj->image_h = s.aft_h;
            s.obj->transform = s.aft_t;
        }
    }
    void undo() override {
        for (auto& s : snaps) {
            s.obj->image_x = s.bef_x; s.obj->image_y = s.bef_y;
            s.obj->image_w = s.bef_w; s.obj->image_h = s.bef_h;
            s.obj->transform = s.bef_t;
        }
    }
    std::string description() const override { return desc; }
    bool preserves_selection() const override { return true; }
};

// ── ScaleObjectsCommand ───────────────────────────────────────────────────────
// Atomically undoes/redoes a scale transform across multiple leaf path nodes.
// One entry per drag-end regardless of how many leaves were affected.
struct ScaleObjectsCommand : CurvzCommand {
    struct LeafSnap {
        SceneNode* obj;
        PathData   before;
        PathData   after;
    };
    std::vector<LeafSnap> snaps;
    std::string desc;

    ScaleObjectsCommand(std::vector<LeafSnap> snaps, std::string desc = "Scale object")
        : snaps(std::move(snaps)), desc(std::move(desc)) {}

    void execute() override {
        for (auto& s : snaps) if (s.obj->path) *s.obj->path = s.after;
    }
    void undo() override {
        for (auto& s : snaps) if (s.obj->path) *s.obj->path = s.before;
    }
    std::string description() const override { return desc; }
    bool preserves_selection() const override { return true; }
};

// ── AlignObjectsCommand ───────────────────────────────────────────────────────
// Atomically undoes/redoes an align or distribute operation across multiple paths.
// Stores before/after PathData per leaf.
struct AlignObjectsCommand : CurvzCommand {
    struct LeafSnap {
        SceneNode* obj;
        PathData   before;
        PathData   after;
    };
    std::vector<LeafSnap> snaps;
    std::string desc;

    AlignObjectsCommand(std::vector<LeafSnap> snaps, std::string desc = "Align objects")
        : snaps(std::move(snaps)), desc(std::move(desc)) {}

    void execute() override {
        for (auto& s : snaps) if (s.obj->path) *s.obj->path = s.after;
    }
    void undo() override {
        for (auto& s : snaps) if (s.obj->path) *s.obj->path = s.before;
    }
    std::string description() const override { return desc; }
};

// ── SetOpacityCommand ─────────────────────────────────────────────────────────
// Records a per-target before/after opacity snapshot for every node touched
// by Canvas::apply_opacity_to_selection — including the children that get
// flattened to 1.0 when a group-like is in the selection. Undo restores the
// full pre-edit state (group + members) atomically.
//
// One Snap per affected node. The caller (apply_opacity_to_selection) is
// responsible for deduping by pointer when a node could be visited twice
// (e.g. shift-selecting both a group and one of its members) — first-touch
// wins, so the snap holds the genuine pre-edit opacity rather than an
// already-mutated value, and undo round-trips cleanly.
struct SetOpacityCommand : CurvzCommand {
    struct Snap {
        SceneNode* obj;
        double     before;
        double     after;
    };
    std::vector<Snap> snaps;
    std::string desc;

    SetOpacityCommand(std::vector<Snap> snaps, std::string desc = "Set opacity")
        : snaps(std::move(snaps)), desc(std::move(desc)) {}

    void execute() override {
        for (auto& s : snaps) if (s.obj) s.obj->opacity = s.after;
    }
    void undo() override {
        for (auto& s : snaps) if (s.obj) s.obj->opacity = s.before;
    }
    std::string description() const override { return desc; }
};

// ── ZOrderCommand ─────────────────────────────────────────────────────────────
// Records a full reorder of a parent's children as two permutations
// (before and after), so undo/redo can restore exact state regardless of
// how complex the rearrangement was.
struct ZOrderCommand : CurvzCommand {
    struct Entry { std::string id; int before_idx; int after_idx; };

    SceneNode*         parent;
    std::vector<Entry> entries;   // one per selected object (informational)
    std::vector<std::string> before_order; // child ids in order before the op
    std::vector<std::string> after_order;  // child ids in order after the op
    std::string        desc;

    ZOrderCommand(SceneNode* parent, std::vector<Entry> entries,
                  std::vector<std::string> before_order,
                  std::vector<std::string> after_order,
                  std::string desc = "Arrange")
        : parent(parent), entries(std::move(entries))
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

    void execute() override { apply_order(parent, after_order);  }
    void undo()    override { apply_order(parent, before_order); }
    std::string description() const override { return desc; }
};

// ── RefMoveCommand ────────────────────────────────────────────────────────────
// Moves a Ref point's position. Undo restores original x/y.
// ── MoveToLayerCommand ────────────────────────────────────────────────────────
// Moves a SceneNode from one layer to another. Undo puts it back.
struct MoveToLayerCommand : CurvzCommand {
    SceneNode* from_layer;
    SceneNode* to_layer;
    int        from_idx;          // original position in from_layer->children
    std::unique_ptr<SceneNode> snap; // deep clone of the node

    MoveToLayerCommand(SceneNode* from, int fi, SceneNode* to,
                       std::unique_ptr<SceneNode> snapshot)
        : from_layer(from), to_layer(to), from_idx(fi)
        , snap(std::move(snapshot)) {}

    void execute() override {
        // Move: find the node in from_layer by matching the snap (by id/name),
        // remove it, prepend to to_layer.
        // NOTE: execute() is called at push time via history replay.
        // The actual live node is already moved by Canvas::move_to_layer().
        // This is a replay-only command — see undo() for reversal.
    }
    void undo() override {
        // Find the node in to_layer (match by id) and move it back.
        auto& dst = to_layer->children;
        auto it = std::find_if(dst.begin(), dst.end(),
            [this](const std::unique_ptr<SceneNode>& n){ return n->id == snap->id; });
        if (it == dst.end()) return;
        auto node = std::move(*it);
        dst.erase(it);
        // Re-insert at original index in from_layer (clamped)
        auto& src = from_layer->children;
        int idx = std::min(from_idx, (int)src.size());
        src.insert(src.begin() + idx, std::move(node));
    }
    std::string description() const override { return "Move to layer"; }
};

struct RefMoveCommand : CurvzCommand {
    SceneNode* node;
    double before_x, before_y;
    double after_x,  after_y;

    RefMoveCommand(SceneNode* n,
                   double bx, double by,
                   double ax, double ay)
        : node(n)
        , before_x(bx), before_y(by)
        , after_x(ax),  after_y(ay) {}

    void execute() override { node->ref_x = after_x;  node->ref_y = after_y;  }
    void undo()    override { node->ref_x = before_x; node->ref_y = before_y; }
    std::string description() const override { return "Move ref point"; }
};

// ── PasteCommand ─────────────────────────────────────────────────────────────
// Inserts one or more pasted nodes into a parent at the front.
// Undo removes them by id; redo re-inserts clones.
struct PasteCommand : CurvzCommand {
    SceneNode* parent;
    // Snapshots in insertion order (front = top of layer)
    std::vector<std::unique_ptr<SceneNode>> snaps;

    PasteCommand(SceneNode* parent,
                 std::vector<std::unique_ptr<SceneNode>> snaps)
        : parent(parent), snaps(std::move(snaps)) {}

    void execute() override {
        // Insert each in reverse so first snap ends up at front
        for (int i = (int)snaps.size()-1; i >= 0; --i)
            parent->children.insert(parent->children.begin(), clone_node(*snaps[i]));
    }
    void undo() override {
        auto& ch = parent->children;
        for (auto& s : snaps)
            for (int i = (int)ch.size()-1; i >= 0; --i)
                if (ch[i]->id == s->id) { ch.erase(ch.begin()+i); break; }
    }
    std::string description() const override { return "Paste"; }
};

// ── CutCommand ────────────────────────────────────────────────────────────────
// Records the removal of one or more nodes for undo purposes.
// Undo re-inserts them at their original positions.
struct CutCommand : CurvzCommand {
    struct Entry {
        SceneNode*                 parent;
        std::unique_ptr<SceneNode> snap;
        int                        index;
    };
    std::vector<Entry> entries; // sorted ascending by index

    explicit CutCommand(std::vector<Entry> entries)
        : entries(std::move(entries)) {}

    void execute() override {
        // Remove in descending index order to avoid shifting
        std::vector<int> order(entries.size());
        for (int i = 0; i < (int)entries.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
            [&](int a, int b){ return entries[a].index > entries[b].index; });
        for (int i : order) {
            auto& ch = entries[i].parent->children;
            for (int j = (int)ch.size()-1; j >= 0; --j)
                if (ch[j]->id == entries[i].snap->id) { ch.erase(ch.begin()+j); break; }
        }
    }
    void undo() override {
        // Re-insert in ascending index order
        std::vector<int> order(entries.size());
        for (int i = 0; i < (int)entries.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
            [&](int a, int b){ return entries[a].index < entries[b].index; });
        for (int i : order) {
            auto& ch = entries[i].parent->children;
            int ins = std::clamp(entries[i].index, 0, (int)ch.size());
            ch.insert(ch.begin() + ins, clone_node(*entries[i].snap));
        }
    }
    std::string description() const override { return "Cut"; }
};

// ── DuplicateCommand ──────────────────────────────────────────────────────────
// Inserts duplicated nodes directly above their originals.
// Undo removes by id; redo re-inserts.
struct DuplicateCommand : CurvzCommand {
    struct Entry {
        SceneNode*                 parent;
        std::unique_ptr<SceneNode> snap;   // the new duplicate
        int                        index;  // insertion index (above original)
    };
    std::vector<Entry> entries;

    explicit DuplicateCommand(std::vector<Entry> entries)
        : entries(std::move(entries)) {}

    void execute() override {
        // Insert in descending index order to keep positions stable
        std::vector<int> order(entries.size());
        for (int i = 0; i < (int)entries.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
            [&](int a, int b){ return entries[a].index > entries[b].index; });
        for (int i : order) {
            auto& ch = entries[i].parent->children;
            int ins = std::clamp(entries[i].index, 0, (int)ch.size());
            ch.insert(ch.begin() + ins, clone_node(*entries[i].snap));
        }
    }
    void undo() override {
        for (auto& e : entries) {
            auto& ch = e.parent->children;
            for (int i = (int)ch.size()-1; i >= 0; --i)
                if (ch[i]->id == e.snap->id) { ch.erase(ch.begin()+i); break; }
        }
    }
    std::string description() const override { return "Duplicate"; }
};

// ── BooleanOpCommand ──────────────────────────────────────────────────────────
// Stores deep clones of the two input nodes and all result nodes.
// Undo: removes results, reinserts originals at their positions.
// Redo: removes originals, reinserts results.
// ── CornerTreatmentCommand ────────────────────────────────────────────────────
// Stores before/after PathData for all paths affected by one corner treatment.
// One push covers all affected paths atomically.
struct CornerTreatmentCommand : CurvzCommand {
    struct Entry { SceneNode* obj; PathData before; PathData after; };
    std::vector<Entry> entries;

    void add(SceneNode* obj, PathData before, PathData after) {
        entries.push_back({obj, std::move(before), std::move(after)});
    }
    void execute() override {
        for (auto& e : entries) if (e.obj->path) *e.obj->path = e.after;
    }
    void undo() override {
        for (auto& e : entries) if (e.obj->path) *e.obj->path = e.before;
    }
    std::string description() const override { return "Corner Treatment"; }
    bool preserves_selection() const override { return true; }
};

struct BooleanOpCommand : CurvzCommand {
    SceneNode* parent;

    // Originals — in ascending index order
    struct Original {
        std::unique_ptr<SceneNode> snap;
        int index;
    };
    std::vector<Original> originals;

    // Results — inserted at insert_index in ascending order
    std::vector<std::unique_ptr<SceneNode>> results;
    int insert_index;
    std::string desc;

    BooleanOpCommand(SceneNode* parent,
                     std::vector<Original> originals,
                     std::vector<std::unique_ptr<SceneNode>> results,
                     int insert_index,
                     std::string desc = "Boolean operation")
        : parent(parent)
        , originals(std::move(originals))
        , results(std::move(results))
        , insert_index(insert_index)
        , desc(std::move(desc)) {}

    void execute() override {
        if (!parent) return;
        auto& ch = parent->children;

        // Remove originals by id
        for (auto& orig : originals) {
            if (!orig.snap) continue;
            for (int i = (int)ch.size()-1; i >= 0; --i) {
                if (ch[i] && ch[i]->id == orig.snap->id) {
                    ch.erase(ch.begin()+i);
                    break;
                }
            }
        }

        // Insert results at insert_index
        int ins = std::clamp(insert_index, 0, (int)ch.size());
        for (int i = 0; i < (int)results.size(); ++i) {
            if (!results[i]) continue;
            ch.insert(ch.begin() + ins + i, clone_node(*results[i]));
        }
    }
    void undo() override {
        if (!parent) return;
        auto& ch = parent->children;

        // Remove results by id
        for (auto& r : results) {
            if (!r) continue;
            for (int i = (int)ch.size()-1; i >= 0; --i) {
                if (ch[i] && ch[i]->id == r->id) {
                    ch.erase(ch.begin()+i);
                    break;
                }
            }
        }

        // Reinsert originals at their original positions
        for (auto& orig : originals) {
            if (!orig.snap) continue;
            int ins = std::clamp(orig.index, 0, (int)ch.size());
            ch.insert(ch.begin() + ins, clone_node(*orig.snap));
        }
    }
    std::string description() const override { return desc; }
};

// ── MakeBlendCommand ──────────────────────────────────────────────────────────
// Undoable creation of a Blend container from exactly two source Paths
// that share a common parent. Same shape as BooleanOpCommand: removes
// the two originals, inserts the Blend at the lower index.
//
// On undo: removes the Blend and reinserts both originals at their
// original positions. The Blend snapshot holds cloned copies of
// blend_source_a / blend_source_b internally, so nothing is lost.
struct MakeBlendCommand : CurvzCommand {
    SceneNode* parent;

    struct Original {
        std::unique_ptr<SceneNode> snap;
        int index;
    };
    std::vector<Original> originals;             // always size 2 for v1

    std::unique_ptr<SceneNode> blend_snap;       // deep-cloned Blend node
    int insert_index;

    MakeBlendCommand(SceneNode* parent,
                     std::vector<Original> originals,
                     std::unique_ptr<SceneNode> blend_snap,
                     int insert_index)
        : parent(parent)
        , originals(std::move(originals))
        , blend_snap(std::move(blend_snap))
        , insert_index(insert_index) {}

    void execute() override {
        auto& ch = parent->children;
        for (auto& orig : originals) {
            for (int i = (int)ch.size()-1; i >= 0; --i)
                if (ch[i]->id == orig.snap->id) { ch.erase(ch.begin()+i); break; }
        }
        int ins = std::clamp(insert_index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*blend_snap));
    }
    void undo() override {
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i]->id == blend_snap->id) { ch.erase(ch.begin()+i); break; }
        for (auto& orig : originals) {
            int ins = std::clamp(orig.index, 0, (int)ch.size());
            ch.insert(ch.begin() + ins, clone_node(*orig.snap));
        }
    }
    std::string description() const override { return "Make Blend"; }
};

// ── ReleaseBlendCommand ───────────────────────────────────────────────────────
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
    SceneNode* parent;
    std::unique_ptr<SceneNode> blend_snap;
    std::vector<std::unique_ptr<SceneNode>> results;  // A, Steps-Group, B
    int insert_index;

    ReleaseBlendCommand(SceneNode* parent,
                        std::unique_ptr<SceneNode> blend_snap,
                        std::vector<std::unique_ptr<SceneNode>> results,
                        int insert_index)
        : parent(parent)
        , blend_snap(std::move(blend_snap))
        , results(std::move(results))
        , insert_index(insert_index) {}

    void execute() override {
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i]->id == blend_snap->id) { ch.erase(ch.begin()+i); break; }
        int ins = std::clamp(insert_index, 0, (int)ch.size());
        for (int i = 0; i < (int)results.size(); ++i)
            ch.insert(ch.begin() + ins + i, clone_node(*results[i]));
    }
    void undo() override {
        auto& ch = parent->children;
        for (auto& r : results)
            for (int i = (int)ch.size()-1; i >= 0; --i)
                if (ch[i]->id == r->id) { ch.erase(ch.begin()+i); break; }
        int ins = std::clamp(insert_index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*blend_snap));
    }
    std::string description() const override { return "Release Blend"; }
};

// ── MakeWarpCommand ───────────────────────────────────────────────────────────
// Undoable creation of a Warp container around a single source node
// (Path, Compound, or Group). The source is cloned into the Warp's
// warp_source slot and removed from the parent's children; the Warp
// takes the source's position in the parent.
//
// On undo: removes the Warp and reinserts the original at its original
// position. The Warp snapshot holds a cloned copy of warp_source
// internally so nothing is lost.
struct MakeWarpCommand : CurvzCommand {
    SceneNode* parent;
    std::unique_ptr<SceneNode> source_snap;          // clone of the original
    int source_index;                                 // original's position
    std::unique_ptr<SceneNode> warp_snap;             // fully-built Warp
    int insert_index;

    MakeWarpCommand(SceneNode* parent,
                    std::unique_ptr<SceneNode> source_snap,
                    int source_index,
                    std::unique_ptr<SceneNode> warp_snap,
                    int insert_index)
        : parent(parent)
        , source_snap(std::move(source_snap))
        , source_index(source_index)
        , warp_snap(std::move(warp_snap))
        , insert_index(insert_index) {}

    void execute() override {
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i]->id == source_snap->id) { ch.erase(ch.begin()+i); break; }
        int ins = std::clamp(insert_index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*warp_snap));
    }
    void undo() override {
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i]->id == warp_snap->id) { ch.erase(ch.begin()+i); break; }
        int ins = std::clamp(source_index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*source_snap));
    }
    std::string description() const override { return "Make Warp"; }
};

// ── ReleaseWarpCommand ────────────────────────────────────────────────────────
// Inverse of MakeWarpCommand: removes the Warp and reinserts the source
// (cloned from warp_source) at the Warp's position. Envelope data is
// discarded — release is meant to be the "undo the Make" operation.
// If the user wants to preserve the baked warped geometry, they should
// use Flatten Warp instead.
struct ReleaseWarpCommand : CurvzCommand {
    SceneNode* parent;
    std::unique_ptr<SceneNode> warp_snap;             // cloned Warp
    std::unique_ptr<SceneNode> source_snap;           // cloned source
    int insert_index;

    ReleaseWarpCommand(SceneNode* parent,
                       std::unique_ptr<SceneNode> warp_snap,
                       std::unique_ptr<SceneNode> source_snap,
                       int insert_index)
        : parent(parent)
        , warp_snap(std::move(warp_snap))
        , source_snap(std::move(source_snap))
        , insert_index(insert_index) {}

    void execute() override {
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i]->id == warp_snap->id) { ch.erase(ch.begin()+i); break; }
        int ins = std::clamp(insert_index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*source_snap));
    }
    void undo() override {
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i]->id == source_snap->id) { ch.erase(ch.begin()+i); break; }
        int ins = std::clamp(insert_index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*warp_snap));
    }
    std::string description() const override { return "Release Warp"; }
};

// ── FlattenWarpCommand ────────────────────────────────────────────────────────
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
    SceneNode* parent;
    std::unique_ptr<SceneNode> warp_snap;             // cloned pre-flatten Warp
    std::unique_ptr<SceneNode> flattened_snap;        // cloned baked result
    int insert_index;

    FlattenWarpCommand(SceneNode* parent,
                       std::unique_ptr<SceneNode> warp_snap,
                       std::unique_ptr<SceneNode> flattened_snap,
                       int insert_index)
        : parent(parent)
        , warp_snap(std::move(warp_snap))
        , flattened_snap(std::move(flattened_snap))
        , insert_index(insert_index) {}

    void execute() override {
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i]->id == warp_snap->id) { ch.erase(ch.begin()+i); break; }
        int ins = std::clamp(insert_index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*flattened_snap));
    }
    void undo() override {
        auto& ch = parent->children;
        for (int i = (int)ch.size()-1; i >= 0; --i)
            if (ch[i]->id == flattened_snap->id) { ch.erase(ch.begin()+i); break; }
        int ins = std::clamp(insert_index, 0, (int)ch.size());
        ch.insert(ch.begin() + ins, clone_node(*warp_snap));
    }
    std::string description() const override { return "Flatten Warp"; }
};

// ── EditWarpCommand ───────────────────────────────────────────────────────────
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
struct EditWarpCommand : CurvzCommand {
    SceneNode* warp;                 // pointer to live Warp in doc tree
    std::string warp_id;              // for re-finding if pointer invalid

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

    EditWarpCommand(SceneNode* warp,
                    PathData pre_top, PathData pre_bottom, int pre_q,
                    PathData post_top, PathData post_bottom, int post_q,
                    int pre_preset = -1, int post_preset = -1)
        : warp(warp)
        , warp_id(warp ? warp->id : std::string())
        , pre_env_top(std::move(pre_top))
        , pre_env_bottom(std::move(pre_bottom))
        , pre_quality(pre_q)
        , pre_preset_idx(pre_preset)
        , post_env_top(std::move(post_top))
        , post_env_bottom(std::move(post_bottom))
        , post_quality(post_q)
        , post_preset_idx(post_preset) {}

    void execute() override {
        if (!warp) return;
        warp->warp_env_top    = post_env_top;
        warp->warp_env_bottom = post_env_bottom;
        warp->warp_quality    = post_quality;
        warp->warp_preset_idx = post_preset_idx;
        warp->warp_cache_dirty = true;
    }
    void undo() override {
        if (!warp) return;
        warp->warp_env_top    = pre_env_top;
        warp->warp_env_bottom = pre_env_bottom;
        warp->warp_quality    = pre_quality;
        warp->warp_preset_idx = pre_preset_idx;
        warp->warp_cache_dirty = true;
    }
    std::string description() const override { return "Edit Warp"; }
};

// ── StepRepeatCommand ─────────────────────────────────────────────────────────
// Inserts N copies of each selected object, each offset by (dx*i, dy*i).
// All copies are removed atomically on undo.
struct StepRepeatCommand : CurvzCommand {
    struct Entry {
        SceneNode*                 parent;
        std::unique_ptr<SceneNode> snap;   // deep copy of the inserted node
        int                        index;  // insertion index
    };
    std::vector<Entry> entries; // all copies across all steps, in insertion order

    explicit StepRepeatCommand(std::vector<Entry> e) : entries(std::move(e)) {}

    void execute() override {
        // Insert in descending index order to keep positions stable
        std::vector<int> order(entries.size());
        for (int i = 0; i < (int)entries.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
            [&](int a, int b){ return entries[a].index > entries[b].index; });
        for (int i : order) {
            auto& ch = entries[i].parent->children;
            int ins = std::clamp(entries[i].index, 0, (int)ch.size());
            ch.insert(ch.begin() + ins, clone_node(*entries[i].snap));
        }
    }
    void undo() override {
        // Remove all inserted copies by id (reverse order for stability)
        for (int i = (int)entries.size() - 1; i >= 0; --i) {
            auto& ch = entries[i].parent->children;
            for (int j = (int)ch.size() - 1; j >= 0; --j)
                if (ch[j]->id == entries[i].snap->id) { ch.erase(ch.begin()+j); break; }
        }
    }
    std::string description() const override { return "Step and Repeat"; }
};

// ── LinkTextToPathCommand ─────────────────────────────────────────────────────
// Undoable link/unlink of a text node to a guide path.
// Stores before/after text_path_id, offset, flip, and text_x/text_y so that
// detach (which repositions the text node) is fully reversible.
struct LinkTextToPathCommand : CurvzCommand {
    SceneNode*  obj;
    std::string before_path_id;
    double      before_offset;
    bool        before_flip;
    double      before_x, before_y;   // text_x/text_y before the operation
    std::string after_path_id;
    double      after_offset;
    bool        after_flip;
    double      after_x, after_y;     // text_x/text_y after the operation

    LinkTextToPathCommand(SceneNode* o,
                          std::string bpid, double boff, bool bflip,
                          double bx, double by,
                          std::string apid, double aoff, bool aflip,
                          double ax, double ay)
        : obj(o)
        , before_path_id(std::move(bpid)), before_offset(boff), before_flip(bflip)
        , before_x(bx), before_y(by)
        , after_path_id(std::move(apid)),  after_offset(aoff),  after_flip(aflip)
        , after_x(ax), after_y(ay) {}

    void execute() override {
        obj->text_path_id     = after_path_id;
        obj->text_path_offset = after_offset;
        obj->text_path_flip   = after_flip;
        obj->text_x           = after_x;
        obj->text_y           = after_y;
    }
    void undo() override {
        obj->text_path_id     = before_path_id;
        obj->text_path_offset = before_offset;
        obj->text_path_flip   = before_flip;
        obj->text_x           = before_x;
        obj->text_y           = before_y;
    }
    std::string description() const override {
        return after_path_id.empty() ? "Detach text from path" : "Link text to path";
    }
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

private:
    std::deque<std::unique_ptr<CurvzCommand>> m_undo_stack;
    std::deque<std::unique_ptr<CurvzCommand>> m_redo_stack;
};

} // namespace Curvz
