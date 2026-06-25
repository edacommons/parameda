import pytest

import parameda
from parameda import Context


def test_version():
    assert parameda.__version__


def test_set_get_and_immutability():
    a = parameda.root()
    b = a.set("v", 10)
    assert b.get("v") == 10
    assert not a.has("v")  # a is above the binding; cannot see down


def test_walk_up_and_shadowing():
    r = parameda.root().set("mode", "debug")
    assert r.set("x", 1).get("mode") == "debug"  # inherited
    assert r.set("mode", "release").get("mode") == "release"  # shadowed


def test_split_branches():
    a = parameda.root()
    assert a.set("v", 10).get("v") == 10
    assert a.set("v", 20).get("v") == 20


def test_interpolation_view_anchored():
    cfg = parameda.root().set("root", "/opt").set("log", "${root}/build.log")
    assert cfg.get("log") == "/opt/build.log"


def test_passthrough_preserves_type():
    cfg = parameda.root().set("n", 42).set("m", "${n}")
    v = cfg.get("m")
    assert v == 42 and isinstance(v, int)


def test_delete_tombstone():
    base = parameda.root().set("k", 1)
    d = base.set("mid", 0).delete("k")
    assert not d.has("k")
    assert base.has("k")


def test_link_and_dotted_path():
    r1 = parameda.root()
    chain = r1.set("a", 1).set("b", 2)
    rp = r1.link("pdk", chain)
    assert rp.path("pdk.a") == 1
    assert rp.path("pdk.b") == 2
    assert not rp.has("a")  # bare key is down the other branch


def test_self_reference_uses_shadowed_value():
    outer = parameda.root().set("path", "/base")
    inner = outer.set("path", "${path}:/extra")
    assert inner.get("path") == "/base:/extra"


def test_merge_order():
    defaults = parameda.root().set("opt", "D")
    base = parameda.root().set("opt", "B")
    assert base.merge(defaults, merge_first=True).get("opt") == "D"
    assert base.merge(defaults, merge_first=False).get("opt") == "B"


def test_env_function(monkeypatch):
    monkeypatch.setenv("PARAMEDA_PY_ENV", "abc")
    assert parameda.root().set("e", "$ENV{PARAMEDA_PY_ENV}").get("e") == "abc"


def test_register_python_function():
    root = parameda.root()
    root.register("join", lambda args: "/".join(args))
    cfg = root.set("a", "x").set("p", "$join{${a}, y, z}")
    assert cfg.get("p") == "x/y/z"


def test_computed_indirect_name():
    cfg = (parameda.root()
           .set("which", "target")
           .set("target", 99)
           .set("v", "${${which}}"))
    assert cfg.get("v") == 99


def test_missing_key_raises():
    with pytest.raises(KeyError):
        parameda.root().get("nope")
