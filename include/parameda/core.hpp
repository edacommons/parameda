#pragma once

// Parameda core — the persistent record graph and the view-anchored resolver.
//
// Implements the design in docs/SPEC.md:
//   - the immutable (parent, key, value) record tree (§1)
//   - the upward-stream resolver with merge splicing + cycle guards (§2)
//   - view-anchored evaluation (§5) of the expression mini-language (§4),
//     parsed by the hand-rolled parser in expr.hpp into an Expr AST.
//
// Values are stored as rawast Values (the load-bearing value type); typing and
// JSON serialization come from rawast. Expressions live inside rawast String
// values and are parsed/evaluated by parameda.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <rawast/value.hpp>

#include <parameda/expr.hpp>

namespace parameda {

struct Record;
using RecordPtr = std::shared_ptr<const Record>;

class Context;

// A registered `$name{…}` function. Receives evaluated arguments and a handle
// to the calling view; returns a value. ENV is a preregistered built-in.
using Callback =
    std::function<rawast::ValuePtr(const std::vector<rawast::ValuePtr>& args,
                                   const Context& view)>;

// Engine-wide function registry, shared across a context lineage (§4.2).
struct Registry {
    std::map<std::string, Callback> fns;
};

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
    std::string key;    // literal, or a template (may contain ${...})
    Value value;
};

// A handle to one record — the "view" you are standing on — plus the shared
// function registry. All builders are functional: they append a child layer and
// return a new Context sharing the same registry.
class Context {
public:
    Context(RecordPtr rec, std::shared_ptr<Registry> reg)
        : rec_(std::move(rec)), reg_(std::move(reg)) {}

    // A fresh, empty root context with built-in functions installed (e.g. ENV).
    static Context root();

    const RecordPtr& record() const { return rec_; }
    const std::shared_ptr<Registry>& registry() const { return reg_; }
    std::optional<Context> parent() const;

    // A view on another record, sharing this context's registry.
    Context sub(RecordPtr rec) const { return Context(std::move(rec), reg_); }

    // --- builders (append a child layer, return the new view) -----------
    Context set(std::string key, rawast::ValuePtr value) const;
    Context del(std::string key) const;
    Context link(std::string key, const Context& target) const;
    Context merge(const Context& target, MergeOrder order) const;

    // Register a `$name{…}` function. Mutates the shared registry, so it affects
    // every context in this lineage; it does not change the record graph.
    void register_fn(std::string name, Callback cb) const;

    // --- resolution (this context is the view) --------------------------
    RecordPtr lookup(const std::string& key) const;       // winning record or nullptr
    bool has(const std::string& key) const;

    // Evaluate a winning record's DataVal as data, from this view (§5).
    // Precondition: `winner` holds a DataVal.
    rawast::ValuePtr eval(const RecordPtr& winner) const;

    // --- JSON persistence -----------------------------------------------
    // Apply a JSON object as a chain of records onto this context, returning the
    // new tip. Scalars/arrays become `set`s; nested objects become linked
    // sub-folders (so they inherit via walk-up and delimit their own locals).
    Context load_json(const std::string& json_text) const;

    // The config as a (nested) dict Value: raw stored values, or fully evaluated
    // from each folder's view when `evaluated` is true. Folder locals are the
    // chain records up to the branch point; a link recurses to a nested object.
    rawast::ValuePtr to_value(bool evaluated) const;

    // Serialize to JSON text (raw by default; evaluated snapshot when asked).
    std::string dump_json(bool evaluated) const;

private:
    rawast::ValuePtr dump_folder(const Record* boundary, bool evaluated) const;

    RecordPtr lookup_in(const RecordPtr& start, const std::string& key,
                        std::set<const Record*>& merge_visited,
                        const std::set<const Record*>& active) const;
    std::optional<std::string> resolve_key(const Record& r) const; // nullopt ⇒ unresolved
    bool key_matches(const Record& r, const std::string& key) const;

    rawast::ValuePtr eval_value(const rawast::ValuePtr& v,
                                std::set<const Record*>& active) const;
    rawast::ValuePtr eval_expr(const Expr& e,
                               std::set<const Record*>& active) const;
    rawast::ValuePtr eval_action(const Action& a,
                                 std::set<const Record*>& active) const;

    RecordPtr rec_;
    std::shared_ptr<Registry> reg_;
};

} // namespace parameda
