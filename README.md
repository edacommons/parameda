# Parameda

**Param + EDA** — a hierarchical configuration system for EDA (Electronic Design
Automation) tooling.

Parameda models configuration as a **persistent, append-only graph of records**.
A *context* is a handle to one record — the view you're standing on — and every
read resolves by walking **up** the parent chain. Writes never mutate: they
append a child and hand back a new context, so branching, snapshots, and shared
ancestry come for free. Values are stored as [rawast](https://github.com/edacommons/rawast)
ASTs, giving typed values and bidirectional text serialization.

It's aimed at EDA flows (and any tool with deeply nested, environment-dependent
settings) that need a clean, reusable way to express and resolve configuration.

## Model in one minute

- **Record** = `(parent, key, value)`. The only structural edge is `parent`.
- **Context / view** = a handle to one record; resolution walks up from it.
- **`set` appends a child** and returns a new context — the original is unchanged
  (immutability + branching are the same operation).
- **Resolution** scans the upward stream; the nearest binding wins.
- **Value kinds**: a data value (scalar/array/dict, possibly an expression), a
  **link** to another record, a **merge** that splices another record's stream in
  (with a precedence flag), or a **deletion** tombstone.
- **View-anchored evaluation**: an expression always resolves from the view you
  started from, so a shared subtree of `${param}` placeholders resolves
  differently depending on where it's used — the templating engine.

See [`docs/SPEC.md`](docs/SPEC.md) for the full design.

## Example (Python)

```python
import parameda

cfg = (parameda.root()
       .set("root", "/opt/app")
       .set("log", "${root}/build.log"))   # template, resolved lazily from the view

cfg.get("log")        # "/opt/app/build.log"
cfg.get("root")       # "/opt/app"

# Writes append; the original context is untouched
base = parameda.root().set("mode", "debug")
base.set("mode", "release").get("mode")    # "release"  (nearer binding shadows)
base.get("mode")                           # "debug"    (unchanged)

# A whole-string ${x} passes the underlying value through with its type intact
parameda.root().set("n", 42).set("m", "${n}").get("m")   # 42 (still an int)

# Links + dotted access reach across branches
r1 = parameda.root()
pdk = r1.set("a", 1).set("b", 2)
view = r1.link("pdk", pdk)
view.path("pdk.a")     # 1
view.path("pdk.b")     # 2

# Merge another context's bindings, with a precedence flag
defaults = parameda.root().set("opt", "D")
parameda.root().set("x", 1).merge(defaults).get("opt")   # "D"

# A self-reference picks up the inherited (shadowed) value — super-style
base = parameda.root().set("path", "/base")
base.set("path", "${path}:/extra").get("path")           # "/base:/extra"

# Register your own $fn{...}; computed names work too
cfg = parameda.root()
cfg.register("join", lambda args: "/".join(args))
cfg.set("a", "x").set("p", "$join{${a}, y, z}").get("p") # "x/y/z"

# Load nested JSON; sub-objects become folders that inherit enclosing vars
cfg = parameda.root().load_json(
    '{"root": "/opt", "build": {"dir": "${root}/b"}}')
cfg.path("build.dir")   # "/opt/b"  (the build folder inherits ${root})
cfg.to_dict()           # {"root": "/opt", "build": {"dir": "/opt/b"}}  (evaluated)
cfg.save_json("out.json")   # raw — templates preserved for round-trip

# Anything unresolved is Undefined (a propagating bottom value), not an error
parameda.root().get("missing") is parameda.Undefined          # True
parameda.root().set("v", "a/${missing}/b").get("v") is parameda.Undefined  # True
parameda.root().set("v", "${missing}").has("v")               # False
```

Also available: `$ENV{VAR}`, `\X` to escape a literal character, `delete(key)`
tombstones, `has(key)`, `parent()`, and the `Undefined` sentinel.

## Status

Early development. The core is in place and tested (C++ + Python): the persistent
record graph, the upward-stream resolver (merge splicing + cycle handling), and
the expression engine — a hand-rolled parser into an `Expr` AST, evaluated
view-anchored, with built-in `${…}` substitution, comma-arg functions via a
**registry** (`$ENV{}` built in, plus C++/Python-registerable functions),
computed names (`${${x}}`), whole-string passthrough, and `super`-style
self-reference. Plus **JSON persistence** — `load_json`/`save_json`/`to_dict`,
where nested objects become inheriting sub-folders and templates round-trip raw —
and an **`Undefined`** bottom value: unresolved references propagate `Undefined`
instead of raising, so `has(key)` *is* the resolvability check (no separate pass).

Next up: a small standard function set. File composition (loading another config
as a sub-folder) lives in the load/format layer — `load_json_file` + `link`/`merge`
today, a load-time include directive later — not as an expression function. Open
design questions are tracked in [`docs/SPEC.md`](docs/SPEC.md) §9.

## Building

C++17 core with Python bindings via [nanobind](https://github.com/wjakob/nanobind),
built with [scikit-build-core](https://github.com/scikit-build/scikit-build-core).
[rawast](https://github.com/edacommons/rawast) is fetched and linked automatically.

```bash
# Python package
pip install .

# C++ core + test suite
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

For local co-development against a rawast working tree, configure with
`-DFETCHCONTENT_SOURCE_DIR_RAWAST=/path/to/rawast`.

## License

[MIT](LICENSE)
