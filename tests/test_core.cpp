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

TEST_CASE("circular reference is detected") {
    Context c = Context::root()
                    .set("a", make_string("${b}"))
                    .set("b", make_string("${a}"));
    CHECK_THROWS(g(c, "a"));
}

TEST_CASE("link + dotted descend reaches another branch") {
    Context r1 = Context::root();
    Context chain = r1.set("a", make_int(1)).set("b", make_int(2)); // tip = chain
    Context rp = r1.link("pdk", chain);

    RecordPtr w = rp.lookup("pdk");
    REQUIRE(w);
    REQUIRE(std::holds_alternative<RefVal>(w->value));
    Context target(std::get<RefVal>(w->value).target);
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
