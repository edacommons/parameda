#include <parameda/core.hpp>

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace parameda {

namespace {

// Render a rawast scalar as text for embedding in a larger string (§4.3).
// Arrays/dicts cannot be interpolated into a string in rung 1.
std::string stringify(const rawast::ValuePtr& v) {
    using namespace rawast;
    if (!v) return "";
    switch (v->type()) {
    case ValueType::Null:
        return "";
    case ValueType::Bool:
        return std::static_pointer_cast<BoolValue>(v)->data() ? "true" : "false";
    case ValueType::Int:
        return std::to_string(std::static_pointer_cast<IntValue>(v)->data());
    case ValueType::UInt:
        return std::to_string(std::static_pointer_cast<UIntValue>(v)->data());
    case ValueType::Real:
        return std::to_string(std::static_pointer_cast<RealValue>(v)->data());
    case ValueType::String:
        return std::static_pointer_cast<StringValue>(v)->data();
    default:
        throw std::runtime_error(
            "parameda: cannot interpolate an array/dict into a string");
    }
}

} // namespace

// --- builders -----------------------------------------------------------

Context Context::root() {
    return Context(std::make_shared<Record>(
        Record{nullptr, false, std::string{}, DataVal{rawast::null_value()}}));
}

Context Context::set(std::string key, rawast::ValuePtr value) const {
    return Context(std::make_shared<Record>(
        Record{rec_, true, std::move(key), DataVal{std::move(value)}}));
}

Context Context::del(std::string key) const {
    return Context(std::make_shared<Record>(
        Record{rec_, true, std::move(key), DeletedVal{}}));
}

Context Context::link(std::string key, const Context& target) const {
    return Context(std::make_shared<Record>(
        Record{rec_, true, std::move(key), RefVal{target.rec_}}));
}

Context Context::merge(const Context& target, MergeOrder order) const {
    return Context(std::make_shared<Record>(
        Record{rec_, false, std::string{}, MergeVal{target.rec_, order}}));
}

std::optional<Context> Context::parent() const {
    if (rec_ && rec_->parent) return Context(rec_->parent);
    return std::nullopt;
}

// --- resolution ---------------------------------------------------------

std::string Context::resolve_key(const Record& r) const {
    // Literal fast-path (§2/§4.4): no '$' means the key needs no evaluation.
    if (r.key.find('$') == std::string::npos) return r.key;
    std::set<const Record*> active;
    return stringify(interpolate(r.key, active));
}

bool Context::key_matches(const Record& r, const std::string& key) const {
    return resolve_key(r) == key;
}

RecordPtr Context::lookup(const std::string& key) const {
    std::set<const Record*> merge_visited;
    std::set<const Record*> active; // empty: a plain lookup skips nothing
    return lookup_in(rec_, key, merge_visited, active);
}

// Walk the upward stream (§2). A merge record splices its target's stream in,
// before or after the rest of the upward chain per its order flag. Records in
// `active` (currently being evaluated) are skipped as candidates, so a
// placeholder resolving mid-evaluation falls through to the next match (§5).
RecordPtr Context::lookup_in(const RecordPtr& start, const std::string& key,
                             std::set<const Record*>& mv,
                             const std::set<const Record*>& active) const {
    RecordPtr R = start;
    while (R) {
        if (const MergeVal* m = std::get_if<MergeVal>(&R->value)) {
            const Record* tp = m->target.get();
            if (m->order == MergeOrder::MergeFirst) {
                if (tp && !mv.count(tp)) {
                    mv.insert(tp);
                    RecordPtr r = lookup_in(m->target, key, mv, active);
                    mv.erase(tp);
                    if (r) return r;
                }
                R = R->parent; // then continue up the chain
                continue;
            }
            // WalkUpFirst: exhaust the rest of the upward chain, then the target.
            if (RecordPtr r = lookup_in(R->parent, key, mv, active)) return r;
            if (tp && !mv.count(tp)) {
                mv.insert(tp);
                RecordPtr r = lookup_in(m->target, key, mv, active);
                mv.erase(tp);
                if (r) return r;
            }
            return nullptr;
        }
        if (R->has_key && !active.count(R.get()) && key_matches(*R, key)) return R;
        R = R->parent;
    }
    return nullptr;
}

bool Context::has(const std::string& key) const {
    RecordPtr w = lookup(key);
    return w && !std::holds_alternative<DeletedVal>(w->value);
}

// --- evaluation (view-anchored, §5) -------------------------------------

rawast::ValuePtr Context::eval(const RecordPtr& winner) const {
    std::set<const Record*> active;
    active.insert(winner.get());
    return eval_value(std::get<DataVal>(winner->value).value, active);
}

rawast::ValuePtr Context::eval_value(const rawast::ValuePtr& v,
                                     std::set<const Record*>& active) const {
    // Rung 1: only String values carry templates. Scalars/arrays/dicts pass
    // through unchanged (deep interpolation arrives with the grammar milestone).
    if (v && v->type() == rawast::ValueType::String)
        return interpolate(std::static_pointer_cast<rawast::StringValue>(v)->data(),
                           active);
    return v;
}

rawast::ValuePtr Context::resolve_ref(const std::string& key,
                                      std::set<const Record*>& active) const {
    // Resolve from this view (§5), skipping records already being evaluated:
    // a placeholder that would resolve to the binding currently being computed
    // instead walks past it to the next (shadowed) match, so
    // `x = "${x}-extra"` picks up the inherited value of x. A reference with no
    // such escape falls off the chain and is reported undefined.
    std::set<const Record*> merge_visited;
    RecordPtr w = lookup_in(rec_, key, merge_visited, active);
    if (!w || std::holds_alternative<DeletedVal>(w->value))
        throw std::runtime_error("parameda: undefined variable '${" + key + "}'");
    if (std::holds_alternative<RefVal>(w->value))
        throw std::runtime_error("parameda: cannot interpolate a link '${" + key +
                                 "}'");
    const Record* wp = w.get();
    active.insert(wp);
    rawast::ValuePtr r = eval_value(std::get<DataVal>(w->value).value, active);
    active.erase(wp);
    return r;
}

rawast::ValuePtr Context::interpolate(const std::string& t,
                                      std::set<const Record*>& active) const {
    // Single-${x} passthrough (§4.3): if the whole string is exactly one
    // placeholder, return the underlying value untouched (preserves type).
    if (t.size() >= 3 && t.compare(0, 2, "${") == 0 && t.back() == '}') {
        std::size_t close = t.find('}', 2);
        if (close == t.size() - 1)
            return resolve_ref(t.substr(2, t.size() - 3), active);
    }

    std::string out;
    const std::size_t n = t.size();
    std::size_t i = 0;
    while (i < n) {
        if (t[i] == '\\' && i + 1 < n && t[i + 1] == '$') { // escape: \$ -> $
            out += '$';
            i += 2;
            continue;
        }
        if (t[i] == '$' && i + 1 < n) {
            if (t.compare(i, 2, "${") == 0) {
                std::size_t end = t.find('}', i + 2);
                if (end != std::string::npos) {
                    out += stringify(resolve_ref(t.substr(i + 2, end - i - 2), active));
                    i = end + 1;
                    continue;
                }
            } else if (t.compare(i, 5, "$ENV{") == 0) {
                std::size_t end = t.find('}', i + 5);
                if (end != std::string::npos) {
                    const char* e = std::getenv(t.substr(i + 5, end - i - 5).c_str());
                    if (e) out += e;
                    i = end + 1;
                    continue;
                }
            }
        }
        out += t[i++];
    }
    return rawast::make_string(std::move(out));
}

} // namespace parameda
