#pragma once

// Parameda core — the persistent record graph and the view-anchored resolver.
//
// This is Milestone 1 of the design in docs/SPEC.md. It implements:
//   - the immutable (parent, key, value) record tree (§1)
//   - the upward-stream resolver with merge splicing + cycle guards (§2)
//   - view-anchored evaluation (§5) with rung-1 interpolation (§4.2)
//
// Values are stored as rawast Values (the load-bearing value type), so typing
// and serialization come from rawast. Two things from the spec are deliberately
// NOT yet here and are tracked as later milestones:
//   - the dedicated `parameda.rawast` grammar that parses templates into a
//     walkable expression AST. For now expressions live inside rawast String
//     values and are interpolated by hand (rung 1: ${key}, $ENV{}).
//   - computed keys are supported only as string templates with a literal
//     fast-path; full AST keys arrive with the grammar.

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>

#include <rawast/value.hpp>

namespace parameda {

struct Record;
using RecordPtr = std::shared_ptr<const Record>;

// Splice order for a merge record (§3.2).
enum class MergeOrder { MergeFirst, WalkUpFirst };

// --- value variants (§3) ------------------------------------------------
struct DataVal { rawast::ValuePtr value; };          // scalar/array/dict (+ string templates)
struct RefVal { RecordPtr target; };                 // a link (§3.1)
struct MergeVal { RecordPtr target; MergeOrder order; }; // merge injection (§3.2); record has no key
struct DeletedVal {};                                // tombstone (§3.3)

using Value = std::variant<DataVal, RefVal, MergeVal, DeletedVal>;

// A node in the persistent configuration tree. Immutable once created.
struct Record {
    RecordPtr parent;   // null at the root
    bool has_key;       // false for the root and for merge records
    std::string key;    // literal, or a rung-1 template (may contain ${...})
    Value value;
};

// A handle to one record — the "view" you are standing on. All operations are
// functional: builders return a new Context and never mutate an existing one.
class Context {
public:
    explicit Context(RecordPtr rec) : rec_(std::move(rec)) {}

    // A fresh, empty root context.
    static Context root();

    const RecordPtr& record() const { return rec_; }
    std::optional<Context> parent() const;

    // --- builders (append a child layer, return the new view) -----------
    Context set(std::string key, rawast::ValuePtr value) const;
    Context del(std::string key) const;
    Context link(std::string key, const Context& target) const;
    Context merge(const Context& target, MergeOrder order) const;

    // --- resolution (this context is the view) --------------------------
    // The winning record for `key` (nullptr if none). The result may hold a
    // DeletedVal — callers treat that as "undefined".
    RecordPtr lookup(const std::string& key) const;

    // True if `key` resolves to a live (non-deleted) record.
    bool has(const std::string& key) const;

    // Evaluate a winning record's DataVal as data, interpolating from this
    // view (§5). Precondition: `winner` holds a DataVal.
    rawast::ValuePtr eval(const RecordPtr& winner) const;

private:
    RecordPtr lookup_in(const RecordPtr& start, const std::string& key,
                        std::set<const Record*>& merge_visited) const;
    std::string resolve_key(const Record& r) const;
    bool key_matches(const Record& r, const std::string& key) const;

    rawast::ValuePtr eval_value(const rawast::ValuePtr& v,
                                std::set<const Record*>& active) const;
    rawast::ValuePtr interpolate(const std::string& tmpl,
                                 std::set<const Record*>& active) const;
    rawast::ValuePtr resolve_ref(const std::string& key,
                                 std::set<const Record*>& active) const;

    RecordPtr rec_;
};

} // namespace parameda
