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
```

Also available: `$ENV{VAR}` substitution, `\$` to escape a literal `$`,
`delete(key)` tombstones, `has(key)`, and `parent()`.

## Status

Early development. **Milestone 1** is in place and tested: the persistent record
graph, the upward-stream resolver (with merge splicing and cycle detection), and
view-anchored rung-1 interpolation (`${key}`, `$ENV{}`), built on rawast values.

Next up: a dedicated `parameda.rawast` grammar that parses templates into a
walkable expression AST (replacing the current hand-written interpolation), full
AST keys, and JSON load/save via rawast's bidirectional serialization. Open
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
