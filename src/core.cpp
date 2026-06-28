#include <parameda/core.hpp>

#include <cstdlib>
#include <stdexcept>
#include <string>

#include <parameda/expr.hpp>

namespace parameda {

namespace {

// Render a rawast scalar as text for embedding in a larger string (§4.3).
// Arrays/dicts cannot be interpolated into a string.
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

bool is_undef(const rawast::ValuePtr& v) {
    return v && v->type() == rawast::ValueType::Undefined;
}

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b - a + 1);
}

// Install the preregistered built-in functions on a fresh registry.
void install_builtins(Registry& reg) {
    // $ENV{VAR} — environment variable (empty if unset).
    reg.fns["ENV"] = [](const std::vector<rawast::ValuePtr>& args,
                        const Context&) -> rawast::ValuePtr {
        if (args.size() != 1)
            throw std::runtime_error("parameda: $ENV{} takes one argument");
        // An unset variable is Undefined (distinct from set-but-empty "").
        const char* e = std::getenv(trim(stringify(args[0])).c_str());
        return e ? rawast::make_string(std::string(e)) : rawast::undefined_value();
    };
    // NOTE: file composition is intentionally NOT an expression function (no
    // `$JSON{}`). File I/O during evaluation is a footgun; loading a config as a
    // sub-folder lives in the load/format layer (load_json + link/merge today, a
    // load-time include later). See SPEC §4.2 / §7.2 / §11.
}

} // namespace

// --- builders -----------------------------------------------------------

Context Context::root() {
    auto reg = std::make_shared<Registry>();
    install_builtins(*reg);
    return Context(std::make_shared<Record>(
                       Record{nullptr, false, std::string{},
                              DataVal{rawast::null_value()}}),
                   std::move(reg));
}

Context Context::set(std::string key, rawast::ValuePtr value) const {
    return sub(std::make_shared<Record>(
        Record{rec_, true, std::move(key), DataVal{std::move(value)}}));
}

Context Context::del(std::string key) const {
    return sub(std::make_shared<Record>(
        Record{rec_, true, std::move(key), DeletedVal{}}));
}

Context Context::link(std::string key, const Context& target) const {
    return sub(std::make_shared<Record>(
        Record{rec_, true, std::move(key), RefVal{target.rec_}}));
}

Context Context::merge(const Context& target, MergeOrder order) const {
    return sub(std::make_shared<Record>(
        Record{rec_, false, std::string{}, MergeVal{target.rec_, order}}));
}

void Context::register_fn(std::string name, Callback cb) const {
    reg_->fns[std::move(name)] = std::move(cb);
}

std::optional<Context> Context::parent() const {
    if (rec_ && rec_->parent) return sub(rec_->parent);
    return std::nullopt;
}

// --- resolution ---------------------------------------------------------

std::optional<std::string> Context::resolve_key(const Record& r) const {
    // Literal fast-path (§2/§4.4): no `$`/`\` means the key needs no parsing.
    if (r.key.find('$') == std::string::npos &&
        r.key.find('\\') == std::string::npos)
        return r.key;
    std::set<const Record*> active;
    rawast::ValuePtr v = eval_expr(parse_template(r.key), active);
    if (is_undef(v)) return std::nullopt; // an unresolved key matches nothing
    return stringify(v);
}

bool Context::key_matches(const Record& r, const std::string& key) const {
    std::optional<std::string> rk = resolve_key(r);
    return rk.has_value() && *rk == key;
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
        if (R->has_key && !active.count(R.get()) && key_matches(*R, key))
            return R;
        R = R->parent;
    }
    return nullptr;
}

bool Context::has(const std::string& key) const {
    // "has" now means "resolves to a defined value" — the resolvability check,
    // unified with evaluation (§ Undefined).
    RecordPtr w = lookup(key);
    if (!w || std::holds_alternative<DeletedVal>(w->value)) return false;
    if (std::holds_alternative<RefVal>(w->value)) return true; // a link is defined
    return !is_undef(eval(w));
}

// --- evaluation (view-anchored, §5) -------------------------------------

rawast::ValuePtr Context::eval(const RecordPtr& winner) const {
    std::set<const Record*> active;
    active.insert(winner.get());
    return eval_value(std::get<DataVal>(winner->value).value, active);
}

rawast::ValuePtr Context::eval_value(const rawast::ValuePtr& v,
                                     std::set<const Record*>& active) const {
    if (is_undef(v)) return v; // Undefined propagates
    // Only String values carry templates; everything else passes through.
    if (v && v->type() == rawast::ValueType::String)
        return eval_expr(
            parse_template(std::static_pointer_cast<rawast::StringValue>(v)->data()),
            active);
    return v;
}

rawast::ValuePtr Context::eval_expr(const Expr& e,
                                    std::set<const Record*>& active) const {
    // Single-action template ⇒ passthrough: return the underlying value with
    // its type intact (§4.3), Undefined included.
    if (e.size() == 1 && e[0].kind == Segment::Kind::Action)
        return eval_action(e[0].action, active);

    // Multi-segment string build: any Undefined part poisons the whole result
    // (§ Undefined — no partial strings).
    std::string out;
    for (const Segment& seg : e) {
        if (seg.kind == Segment::Kind::Text) {
            out += seg.text;
        } else {
            rawast::ValuePtr r = eval_action(seg.action, active);
            if (is_undef(r)) return rawast::undefined_value();
            out += stringify(r);
        }
    }
    return rawast::make_string(std::move(out));
}

rawast::ValuePtr Context::eval_action(const Action& a,
                                      std::set<const Record*>& active) const {
    if (a.name.empty()) {
        // Built-in substitution: resolve the (single) argument as a key and
        // look it up from this view, skipping records under evaluation (§5).
        // Anything unresolved → Undefined (no exceptions, § Undefined).
        if (a.args.size() != 1)
            throw std::runtime_error(
                "parameda: ${} substitution takes exactly one argument");
        rawast::ValuePtr keyv = eval_expr(a.args[0], active);
        if (is_undef(keyv)) return rawast::undefined_value(); // unresolved key name
        std::string key = trim(stringify(keyv));

        std::set<const Record*> mv;
        RecordPtr w = lookup_in(rec_, key, mv, active);
        if (!w || std::holds_alternative<DeletedVal>(w->value))
            return rawast::undefined_value(); // missing / deleted / dead-end cycle
        if (std::holds_alternative<RefVal>(w->value))
            return rawast::undefined_value(); // a link can't interpolate as data
        const Record* wp = w.get();
        active.insert(wp);
        rawast::ValuePtr r = eval_value(std::get<DataVal>(w->value).value, active);
        active.erase(wp);
        return r;
    }

    // Named function: look up in the registry and call with evaluated args. An
    // unknown function is an error (a real mistake, not "unresolved"); an
    // Undefined argument auto-propagates and the callback is skipped.
    auto it = reg_->fns.find(a.name);
    if (it == reg_->fns.end())
        throw std::runtime_error("parameda: unknown function '$" + a.name + "{}'");
    std::vector<rawast::ValuePtr> argv;
    argv.reserve(a.args.size());
    for (const Expr& arg : a.args) {
        rawast::ValuePtr av = eval_expr(arg, active);
        if (is_undef(av)) return rawast::undefined_value();
        argv.push_back(av);
    }
    return it->second(argv, *this);
}

} // namespace parameda
