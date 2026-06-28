#include <parameda/core.hpp>

#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

#include <rawast/grammar.hpp>
#include <rawast/value.hpp>

// JSON persistence (SPEC §4 / persistence design). The config (a tree-shaped
// projection of the record graph) maps to nested JSON: a folder ⟺ an object, a
// nested object ⟺ a linked sub-folder. We serialize the *actual* structure — a
// chain of value records under each node — rather than a flattened view, so the
// folder boundary is just the branch point (a link record's parent), with no
// special marker record needed. rawast supplies JSON parse + save.
//
// Limitation (v1): only the set/link/delete tree round-trips. Merge records are
// not serialized, and cross-branch links assume the canonical construction (the
// linked chain passes through the link's parent), which always holds for
// load_json-built configs.

namespace parameda {

namespace {

const rawast::Grammar& json_grammar() {
    static const rawast::Grammar g = rawast::make_json_grammar();
    return g;
}

rawast::ValuePtr parse_json(const std::string& text) {
    auto r = json_grammar().parse(text);
    if (!r)
        throw std::runtime_error("parameda: JSON parse error: " + r.error().message);
    return *r;
}

std::string write_json(const rawast::ValuePtr& v) {
    std::ostringstream out;
    auto r = json_grammar().save(out, v, /*pretty=*/true);
    if (!r)
        throw std::runtime_error("parameda: JSON save error: " + r.error().message);
    return out.str();
}

// Apply a JSON object onto `cur`, returning the new tip. Scalars first, then
// sub-folders built off the completed tip — so a sub-folder inherits *all* of
// this object's keys (regardless of key order), not just the ones set before it.
Context apply_object(Context cur, const rawast::ValuePtr& obj) {
    auto d = std::static_pointer_cast<rawast::DictValue>(obj);
    for (const auto& [key, val] : d->data())
        if (!val || val->type() != rawast::ValueType::Dict)
            cur = cur.set(key, val);
    for (const auto& [key, val] : d->data())
        if (val && val->type() == rawast::ValueType::Dict)
            cur = cur.link(key, apply_object(cur, val));
    return cur;
}

} // namespace

Context Context::load_json(const std::string& json_text) const {
    rawast::ValuePtr v = parse_json(json_text);
    if (!v || v->type() != rawast::ValueType::Dict)
        throw std::runtime_error("parameda: top-level JSON must be an object");
    return apply_object(*this, v);
}

// Enumerate this folder's local records — from the tip up to `boundary`
// (exclusive) — nearest-wins per key, into a dict. A `Ref` recurses into the
// sub-folder (boundary = the link record's parent). Merge records are skipped.
rawast::ValuePtr Context::dump_folder(const Record* boundary, bool evaluated) const {
    auto out = std::static_pointer_cast<rawast::DictValue>(rawast::make_dict());
    std::set<std::string> seen;
    for (RecordPtr R = rec_; R && R.get() != boundary; R = R->parent) {
        if (!R->has_key) continue; // root / merge: no key to enumerate
        if (seen.count(R->key)) continue; // a nearer binding already won
        seen.insert(R->key);
        if (std::holds_alternative<DeletedVal>(R->value)) continue; // tombstone: hide
        if (std::holds_alternative<RefVal>(R->value)) {
            RecordPtr target = std::get<RefVal>(R->value).target;
            out->data()[R->key] =
                sub(target).dump_folder(R->parent.get(), evaluated);
        } else { // DataVal
            if (evaluated) {
                rawast::ValuePtr v = eval(R);
                // Unresolved values are omitted from an evaluated snapshot (and
                // Undefined has no JSON form). Raw values are never Undefined.
                if (v && v->type() == rawast::ValueType::Undefined) continue;
                out->data()[R->key] = v;
            } else {
                out->data()[R->key] = std::get<DataVal>(R->value).value;
            }
        }
    }
    return out;
}

rawast::ValuePtr Context::to_value(bool evaluated) const {
    return dump_folder(nullptr, evaluated); // top folder: up to the parentless root
}

std::string Context::dump_json(bool evaluated) const {
    return write_json(to_value(evaluated));
}

} // namespace parameda
