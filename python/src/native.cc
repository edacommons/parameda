// parameda Python binding — exposes the Context handle and its functional
// builders / resolvers. Values convert directly between rawast's Value family
// and native Python objects (None/bool/int/float/str/list/dict). `$name{}`
// functions can be registered as Python callables.

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>

#include <fstream>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <parameda/core.hpp>
#include <parameda/version.hpp>
#include <rawast/value.hpp>

namespace nb = nanobind;
using parameda::Context;
using parameda::DeletedVal;
using parameda::MergeOrder;
using parameda::RecordPtr;
using parameda::RefVal;

namespace {

nb::object value_to_py(const rawast::ValuePtr& v) {
    using namespace rawast;
    if (!v) return nb::none();
    switch (v->type()) {
    case ValueType::Null:
        return nb::none();
    case ValueType::Bool:
        return nb::cast(std::static_pointer_cast<BoolValue>(v)->data());
    case ValueType::Int:
        return nb::cast(std::static_pointer_cast<IntValue>(v)->data());
    case ValueType::UInt:
        return nb::cast(std::static_pointer_cast<UIntValue>(v)->data());
    case ValueType::Real:
        return nb::cast(std::static_pointer_cast<RealValue>(v)->data());
    case ValueType::String:
        return nb::cast(std::static_pointer_cast<StringValue>(v)->data());
    case ValueType::Array: {
        nb::list out;
        for (const auto& e : std::static_pointer_cast<ArrayValue>(v)->data())
            out.append(value_to_py(e));
        return out;
    }
    case ValueType::Dict: {
        nb::dict out;
        for (const auto& [k, e] : std::static_pointer_cast<DictValue>(v)->data())
            out[nb::cast(k)] = value_to_py(e);
        return out;
    }
    }
    return nb::none();
}

rawast::ValuePtr py_to_value(nb::handle o) {
    if (o.is_none()) return rawast::null_value();
    // bool must precede int (Python bool is a subclass of int).
    if (nb::isinstance<nb::bool_>(o))
        return nb::cast<bool>(o) ? rawast::true_value() : rawast::false_value();
    if (nb::isinstance<nb::int_>(o))
        return rawast::make_int(nb::cast<std::int64_t>(o));
    if (nb::isinstance<nb::float_>(o))
        return rawast::make_real(nb::cast<double>(o));
    if (nb::isinstance<nb::str>(o))
        return rawast::make_string(nb::cast<std::string>(o));
    if (nb::isinstance<nb::list>(o) || nb::isinstance<nb::tuple>(o)) {
        rawast::ValuePtr arr = rawast::make_array();
        auto& data = std::static_pointer_cast<rawast::ArrayValue>(arr)->data();
        for (nb::handle e : o) data.push_back(py_to_value(e));
        return arr;
    }
    if (nb::isinstance<nb::dict>(o)) {
        rawast::ValuePtr d = rawast::make_dict();
        auto& data = std::static_pointer_cast<rawast::DictValue>(d)->data();
        for (auto [k, e] : nb::cast<nb::dict>(o))
            data[nb::cast<std::string>(k)] = py_to_value(e);
        return d;
    }
    throw nb::type_error("parameda: unsupported value type");
}

// Resolve `key` from view `c`, returning a Python value, a sub-context (for a
// link), or raising KeyError when undefined / deleted.
nb::object get_object(const Context& c, const std::string& key) {
    RecordPtr w = c.lookup(key);
    if (!w || std::holds_alternative<DeletedVal>(w->value))
        throw nb::key_error(key.c_str());
    if (std::holds_alternative<RefVal>(w->value))
        return nb::cast(c.sub(std::get<RefVal>(w->value).target));
    return value_to_py(c.eval(w));
}

std::vector<std::string> split_path(const std::string& s) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        std::size_t dot = s.find('.', start);
        if (dot == std::string::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, dot - start));
        start = dot + 1;
    }
    return parts;
}

} // namespace

NB_MODULE(_native, m) {
    m.attr("__version__") = std::string(parameda::version());

    nb::class_<Context>(m, "Context")
        .def("set",
             [](const Context& s, const std::string& key, nb::handle value) {
                 return s.set(key, py_to_value(value));
             },
             nb::arg("key"), nb::arg("value"))
        .def("delete", [](const Context& s, const std::string& key) {
            return s.del(key);
        }, nb::arg("key"))
        .def("link", [](const Context& s, const std::string& key,
                        const Context& target) { return s.link(key, target); },
             nb::arg("key"), nb::arg("target"))
        .def("merge",
             [](const Context& s, const Context& target, bool merge_first) {
                 return s.merge(target, merge_first ? MergeOrder::MergeFirst
                                                    : MergeOrder::WalkUpFirst);
             },
             nb::arg("target"), nb::arg("merge_first") = true)
        // NOTE: the callable is held as an nb::object inside the C++ registry.
        // Under normal scoping it collects fine, but it is not yet wired into
        // Python's cyclic GC, so a callback that closes over a Context leaks,
        // and a module-global Context kept to interpreter exit prints a benign
        // nanobind teardown warning. See SPEC §9(8).
        .def("register",
             [](const Context& s, const std::string& name, nb::object fn) {
                 s.register_fn(name, [fn](const std::vector<rawast::ValuePtr>& args,
                                          const Context&) -> rawast::ValuePtr {
                     nb::gil_scoped_acquire gil;
                     nb::list pyargs;
                     for (const auto& a : args) pyargs.append(value_to_py(a));
                     return py_to_value(fn(pyargs));
                 });
             },
             nb::arg("name"), nb::arg("callback"),
             "Register a $name{...} function. The callable receives a list of "
             "evaluated arguments and returns a value. Affects this lineage.")
        .def("get", &get_object, nb::arg("key"))
        .def("has", [](const Context& s, const std::string& key) {
            return s.has(key);
        }, nb::arg("key"))
        .def("parent", &Context::parent)
        .def("load_json",
             [](const Context& s, const std::string& json) { return s.load_json(json); },
             nb::arg("json"),
             "Apply a JSON object string onto this context, returning the new tip.")
        .def("load_json_file",
             [](const Context& s, const std::string& path) {
                 std::ifstream in(path, std::ios::binary);
                 if (!in)
                     throw nb::value_error(("parameda: cannot open " + path).c_str());
                 std::ostringstream ss;
                 ss << in.rdbuf();
                 return s.load_json(ss.str());
             },
             nb::arg("path"))
        .def("to_dict",
             [](const Context& s) { return value_to_py(s.to_value(true)); },
             "The fully evaluated config as a nested dict.")
        .def("to_json",
             [](const Context& s, bool evaluated) { return s.dump_json(evaluated); },
             nb::arg("evaluated") = false)
        .def("save_json",
             [](const Context& s, const std::string& path, bool evaluated) {
                 std::ofstream out(path, std::ios::binary);
                 if (!out)
                     throw nb::value_error(("parameda: cannot write " + path).c_str());
                 out << s.dump_json(evaluated);
             },
             nb::arg("path"), nb::arg("evaluated") = false)
        .def("path",
             [](const Context& s, const std::string& dotted) -> nb::object {
                 std::vector<std::string> parts = split_path(dotted);
                 Context cur = s;
                 for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
                     RecordPtr w = cur.lookup(parts[i]);
                     if (!w || std::holds_alternative<DeletedVal>(w->value))
                         throw nb::key_error(parts[i].c_str());
                     if (!std::holds_alternative<RefVal>(w->value))
                         throw nb::value_error(
                             ("parameda: path segment is not a link: " + parts[i])
                                 .c_str());
                     cur = cur.sub(std::get<RefVal>(w->value).target);
                 }
                 return get_object(cur, parts.back());
             },
             nb::arg("path"));

    m.def("root", &Context::root, "A fresh, empty root context.");
}
