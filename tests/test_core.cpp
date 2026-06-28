#include <doctest/doctest.h>

#include <cstdlib>

#include <parameda/core.hpp>
#include <rawast/value.hpp>

using namespace parameda;
using rawast::make_int;
using rawast::make_string;

namespace {

// Resolve and evaluate `key` from view `c`. Fails the test if undefined.
rawast::ValuePtr g(const Context& c, const std::string& key) {
    RecordPtr w = c.lookup(key);
    REQUIRE(w);
    return c.eval(w);
}

std::int64_t as_i(const rawast::ValuePtr& v) {
    return std::static_pointer_cast<rawast::IntValue>(v)->data();
}
std::string as_s(const rawast::ValuePtr& v) {
    return std::static_pointer_cast<rawast::StringValue>(v)->data();
}

bool is_undef(const rawast::ValuePtr& v) {
    return v && v->type() == rawast::ValueType::Undefined;
}

} // namespace

TEST_CASE("set/get and append-only immutability") {
    Context A = Context::root();
    Context B = A.set("v", make_int(10));
    CHECK(B.has("v"));
    CHECK(as_i(g(B, "v")) == 10);
    CHECK(A.lookup("v") == nullptr); // A is above the binding: cannot see down
}

TEST_CASE("walk up and shadowing") {
    Context r = Context::root().set("mode", make_string("debug"));
    Context inherited = r.set("x", make_int(1));
    CHECK(as_s(g(inherited, "mode")) == "debug"); // inherited from ancestor
    Context shadowed = inherited.set("mode", make_string("release"));
    CHECK(as_s(g(shadowed, "mode")) == "release"); // nearer binding wins
}

TEST_CASE("split: independent branches share ancestry") {
    Context A = Context::root().set("base", make_int(0));
    Context B = A.set("v", make_int(10));
    Context C = A.set("v", make_int(20));
    CHECK(as_i(g(B, "v")) == 10);
    CHECK(as_i(g(C, "v")) == 20);
    CHECK(as_i(g(B, "base")) == 0);
    CHECK(as_i(g(C, "base")) == 0);
}

TEST_CASE("delete tombstone masks a farther definition") {
    Context base = Context::root().set("k", make_int(1));
    Context deeper = base.set("mid", make_int(0));
    Context d = deeper.del("k");
    CHECK(!d.has("k"));
    CHECK(base.has("k")); // the ancestor is untouched
}

TEST_CASE("interpolation resolves from the view") {
    Context root = Context::root().set("root", make_string("/opt"));
    Context build = root.set("log", make_string("${root}/build.log"));
    CHECK(as_s(g(build, "log")) == "/opt/build.log");
}

TEST_CASE("single-placeholder passthrough preserves type") {
    Context c = Context::root().set("n", make_int(42)).set("m", make_string("${n}"));
    rawast::ValuePtr v = g(c, "m");
    CHECK(v->type() == rawast::ValueType::Int);
    CHECK(as_i(v) == 42);
}

TEST_CASE("env substitution") {
    ::setenv("PARAMEDA_TEST_ENV", "xyz", 1);
    Context c = Context::root().set("e", make_string("$ENV{PARAMEDA_TEST_ENV}"));
    CHECK(as_s(g(c, "e")) == "xyz");
}

TEST_CASE("escaped dollar is literal") {
    Context c = Context::root().set("e", make_string("price: \\${5}"));
    CHECK(as_s(g(c, "e")) == "price: ${5}");
}

TEST_CASE("self-reference resolves to the shadowed outer value") {
    Context outer = Context::root().set("path", make_string("/base"));
    Context inner = outer.set("path", make_string("${path}:/extra"));
    CHECK(as_s(g(inner, "path")) == "/base:/extra"); // ${path} = inherited value
}

TEST_CASE("unresolvable cycle (no shadowed escape) is Undefined") {
    Context c = Context::root()
                    .set("a", make_string("${b}"))
                    .set("b", make_string("${a}"));
    CHECK(is_undef(g(c, "a"))); // both skipped while active, nothing left to match
}

TEST_CASE("unresolved substitution is Undefined and propagates") {
    Context c = Context::root().set("v", make_string("${missing}"));
    CHECK(is_undef(g(c, "v")));

    // String interpolation short-circuits: any Undefined part poisons the whole.
    Context c2 = Context::root().set("v", make_string("a/${missing}/b"));
    CHECK(is_undef(g(c2, "v")));

    // Propagation through a chain.
    Context c3 = Context::root()
                     .set("a", make_string("${missing}"))
                     .set("b", make_string("${a}"));
    CHECK(is_undef(g(c3, "b")));
}

TEST_CASE("$ENV{unset} is Undefined; set-but-empty is empty string") {
    Context c = Context::root().set("v", make_string("$ENV{PARAMEDA_NOPE_XYZ}"));
    CHECK(is_undef(g(c, "v")));
    ::setenv("PARAMEDA_EMPTY", "", 1);
    Context c2 = Context::root().set("v", make_string("$ENV{PARAMEDA_EMPTY}"));
    CHECK(!is_undef(g(c2, "v")));
    CHECK(as_s(g(c2, "v")) == "");
}

TEST_CASE("function arg Undefined auto-propagates (callback skipped)") {
    Context root = Context::root();
    bool called = false;
    root.register_fn("f", [&called](const std::vector<rawast::ValuePtr>&,
                                    const Context&) -> rawast::ValuePtr {
        called = true;
        return rawast::make_string("x");
    });
    Context c = root.set("v", make_string("$f{${missing}}"));
    CHECK(is_undef(g(c, "v")));
    CHECK(!called); // the callback never ran
}

TEST_CASE("has() means resolves-to-defined") {
    Context c = Context::root()
                    .set("ok", make_int(1))
                    .set("bad", make_string("${missing}"));
    CHECK(c.has("ok"));
    CHECK(!c.has("bad"));     // present but unresolved
    CHECK(!c.has("absent")); // not present at all
}

TEST_CASE("link + dotted descend reaches another branch") {
    Context r1 = Context::root();
    Context chain = r1.set("a", make_int(1)).set("b", make_int(2)); // tip = chain
    Context rp = r1.link("pdk", chain);

    RecordPtr w = rp.lookup("pdk");
    REQUIRE(w);
    REQUIRE(std::holds_alternative<RefVal>(w->value));
    Context target = rp.sub(std::get<RefVal>(w->value).target);
    CHECK(as_i(g(target, "a")) == 1);
    CHECK(as_i(g(target, "b")) == 2);

    CHECK(rp.lookup("a") == nullptr); // bare `a` is down the other branch
}

TEST_CASE("merge order flag controls precedence") {
    Context defaults = Context::root().set("opt", make_string("D"));
    Context base = Context::root().set("opt", make_string("B"));

    Context mf = base.merge(defaults, MergeOrder::MergeFirst);
    CHECK(as_s(g(mf, "opt")) == "D"); // merged target wins

    Context wf = base.merge(defaults, MergeOrder::WalkUpFirst);
    CHECK(as_s(g(wf, "opt")) == "B"); // upward chain wins
}

TEST_CASE("merge pulls in keys absent from the base") {
    Context defaults = Context::root().set("extra", make_int(7));
    Context base = Context::root().set("own", make_int(1));
    Context m = base.merge(defaults, MergeOrder::MergeFirst);
    CHECK(as_i(g(m, "extra")) == 7); // from the merge target
    CHECK(as_i(g(m, "own")) == 1);   // from the base chain
}

TEST_CASE("ENV built-in function") {
    ::setenv("PARAMEDA_FN_ENV", "abc", 1);
    Context c = Context::root().set("e", make_string("$ENV{PARAMEDA_FN_ENV}"));
    CHECK(as_s(g(c, "e")) == "abc");
}

TEST_CASE("registered function with multiple args") {
    Context root = Context::root();
    root.register_fn("join", [](const std::vector<rawast::ValuePtr>& args,
                                const Context&) -> rawast::ValuePtr {
        std::string out;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) out += "/";
            out += std::static_pointer_cast<rawast::StringValue>(args[i])->data();
        }
        return rawast::make_string(out);
    });
    Context c = root.set("a", make_string("x")).set("p", make_string("$join{${a}, y, z}"));
    CHECK(as_s(g(c, "p")) == "x/y/z");
}

TEST_CASE("unknown function errors") {
    Context c = Context::root().set("v", make_string("$nope{x}"));
    CHECK_THROWS(g(c, "v"));
}

TEST_CASE("computed / indirect name") {
    Context c = Context::root()
                    .set("which", make_string("target"))
                    .set("target", make_int(99))
                    .set("v", make_string("${${which}}"));
    CHECK(as_i(g(c, "v")) == 99);
}

TEST_CASE("escaped dollar and brace are literal") {
    Context c = Context::root().set("v", make_string("\\${x} and \\}"));
    CHECK(as_s(g(c, "v")) == "${x} and }");
}

TEST_CASE("json load: nested folders inherit and round-trip") {
    std::string json =
        R"({"root":"/opt","log":"${root}/x.log","build":{"dir":"${root}/b","name":"top"}})";
    Context c = Context::root().load_json(json);

    CHECK(as_s(g(c, "root")) == "/opt");
    CHECK(as_s(g(c, "log")) == "/opt/x.log");

    RecordPtr w = c.lookup("build");
    REQUIRE(w);
    REQUIRE(std::holds_alternative<RefVal>(w->value));
    Context b = c.sub(std::get<RefVal>(w->value).target);
    CHECK(as_s(g(b, "dir")) == "/opt/b"); // ${root} inherited through the link
    CHECK(as_s(g(b, "name")) == "top");
    CHECK(c.lookup("dir") == nullptr); // sub-folder keys are not leaked to parent

    // Raw round-trip: dump (templates preserved) and reload.
    Context c2 = Context::root().load_json(c.dump_json(false));
    CHECK(as_s(g(c2, "log")) == "/opt/x.log");
    Context b2 = c2.sub(std::get<RefVal>(c2.lookup("build")->value).target);
    CHECK(as_s(g(b2, "dir")) == "/opt/b");
}

TEST_CASE("json evaluated snapshot resolves templates") {
    Context c = Context::root().load_json(
        R"({"root":"/opt","log":"${root}/x.log"})");
    // The evaluated dump, reloaded, stores the resolved literal — not a template.
    Context snap = Context::root().load_json(c.dump_json(true));
    RecordPtr w = snap.lookup("log");
    REQUIRE(w);
    REQUIRE(std::holds_alternative<DataVal>(w->value));
    CHECK(as_s(std::get<DataVal>(w->value).value) == "/opt/x.log");
}
