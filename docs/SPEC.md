# Parameda — Design Specification

**Status:** Draft (design). No implementation committed yet beyond build scaffolding.
**Last updated:** 2026-06-16

Parameda (**Param + EDA**) is a hierarchical configuration system aimed at EDA
tooling. It is a public, reusable implementation of a long-standing
configuration-graph design; an earlier private implementation exists in
ChipFlow, but Parameda is a clean re-design from first principles.

This document is the authoritative description of the model. It is written to be
implementable directly; where a decision is deliberately deferred it is marked
**OPEN** or **DEFERRED**.

---

## 1. Core idea

The configuration space is a **persistent (immutable, append-only) tree of
records**. There is exactly one structural relationship — `parent` — and all
reads resolve by walking **up** the parent chain. You never look *down*, and you
never traverse link/merge edges as parent steps.

A **record** has three fields:

```
record { parent, key, value }
```

- `parent` — the record this one was layered on (its enclosing scope). The root
  record has no parent.
- `key` — the single key this record binds, stored as an **AST** (§4). A literal
  key like `"v"` is just an AST string literal; a computed key like
  `${tool}_flags` is an expression resolved the same way as a value (§4.4). A
  **merge** record has *no* key.
- `value` — see §3.

A **context** (equivalently a **view**) is simply a handle to one record — the
record you are currently "standing on." It carries no extra state; the entire
ancestry above it *is* its scope.

### 1.1 Writing is appending

```
B = A.set("v", 10)
```

`set` does **not** mutate `A`. It allocates a new record `{parent: A, key: "v",
value: 10}` and returns it as the new context `B`. Therefore:

```
A.get("v")   # → undefined : A is above the binding; resolution only walks up
B.get("v")   # → 10        : found locally at B
```

This single mechanic explains the whole structure:

- **Immutability** — ancestors are never edited; you only grow leaves. The chain
  of records *is* the recording/history.
- **Why `A` can't see `v`** — `v` lives in a child; resolution never descends.
- **Split / branching** — `set` from *any* record produces a child; several
  children off the same record are independent branches that share all common
  ancestry (structural sharing):

  ```
  B = A.set("v", 10)
  C = A.set("v", 20)
  B.get("v")  # 10
  C.get("v")  # 20      ← B and C share everything above A
  ```

Branching and writing are the *same operation*. Snapshots are free.

---

## 2. Resolution as an upward stream

Resolution is modeled as **consuming a lazy stream of records yielded upward**
from the context. The base generator yields the context record, then its parent,
then its parent's parent, and so on. A lookup scans that stream for a record
whose key matches the queried key; the **first** match wins.

Because keys are ASTs (§4.4), matching is **resolve-then-compare**, not raw
string equality: each candidate's key is resolved from the querying view and
compared to the (resolved) queried key. A **literal fast-path** short-circuits
the common case — if a key AST is a pure literal, it is treated as a static
string with no evaluation.

```
def gen(R, visited):
    while R is not None:
        v = R.value
        if v is Merge(target, merge_first):
            if target in visited:          # cycle guard (§5)
                R = R.parent
                continue
            v2 = visited | {target}
            if merge_first:
                yield from gen(target, v2)
                yield from gen(R.parent, visited)
            else:                          # walk-up first
                yield from gen(R.parent, visited)
                yield from gen(target, v2)
            return
        else:
            yield R                        # merge records carry no key → never yielded here
            R = R.parent

def lookup(view, key):                     # key: already-resolved string
    for R in gen(view, set()):
        if R.value is Merge: continue      # no key
        rk = resolve_key(R, view)          # literal fast-path; else eval from view (§4.4)
        if rk == key:
            if R.value is Deleted:
                return UNDEFINED           # tombstone: nearest match is a deletion
            return R                        # winning record
    return UNDEFINED
```

Consequences, all of which fall out of stream order rather than special rules:

- **Shadowing** — a nearer binding wins over a farther one (it is yielded first).
- **Deletion** (`dnode`) — if the *first* record matching the key is a `Deleted`
  value, the key resolves to undefined, even if a farther ancestor defines it. A
  closer deletion masks a farther definition.
- **Precedence is stream order** — there is no global precedence rule to reason
  about; whatever the generator emits first wins.

---

## 3. Values

```
value  =  AST            # data + expressions, owned by rawast (§4)
       |  Ref(target)    # a link: value *is* another record/sub-context
       |  Merge(target, order_flag)
       |  Deleted        # tombstone
```

The partition is deliberate:

- **rawast owns the value payload** — scalars, arrays, structured literals, and
  expressions are all rawast ASTs, and their serialization is rawast's job (§4).
- **Parameda owns the graph constructs** — `Ref`, `Merge`, and `Deleted` are
  about the *record graph*, not about text, so they are native parameda value
  cases and never appear in the value grammar.

### 3.1 Ref (link)

A `Ref` value points at another record. Reading it yields the target as a
**sub-context** (a view you can query further). Links are **inert with respect to
the walk**: the resolver never follows a `Ref` as a parent step. You get the
target as a value; if you then query *into* it, the target becomes your new
view (§6).

### 3.2 Merge

A `Merge` record has **no key** — it is purely an injection point. It carries:

- `target` — the record whose stream is spliced in.
- `order_flag` — `merge_first` vs `walk_up_first` (see `gen` above). This decides,
  *at the merge point*, whether the target's stream or the rest of the upward
  chain takes precedence. Records *between* the context and the merge always come
  first regardless, because the stream reaches them earlier.

`merge_first` ≈ "imported defaults override what's above the merge."
`walk_up_first` ≈ "what's above the merge overrides the import."

A merge splices the target's **entire upward generator** (target, then up the
target's own chain) — there is no "just the target's local bindings" option,
because a record carries exactly one binding.

### 3.3 Deleted

A `Deleted` value is a tombstone for the record's `key`. See §2.

---

## 4. Keys and values

A record's `value` is stored as a **rawast Value** (Null/Bool/Int/UInt/Real/
String/Array/Dict). rawast is the load-bearing value type — it gives typing and
JSON load/save for free. A record's `key` is a string (literal or template).

When a value (or key) is a **string**, it is a **template** in parameda's
expression mini-language (§4.2): literal text interleaved with `$name{…}`
placeholders. Templates are parsed by a small hand-rolled parser (§4.1) into an
`Expr` AST and evaluated against the view (§5). This gives:

- **Typing for free** — a value set as `10` is an Int; a value set as `"10"` is
  the string "10". "Is `${v}` an int or a string?" is answered by what was
  stored, not by a parameda rule (§4.3).
- **Computed keys** — a key may itself be a template (§4.4), the dual of dynamic
  values.
- **Trivial serialization** — the stored form *is* the authored string (plus
  rawast Values for non-string data), so persisting/round-tripping needs no
  AST→text step; the original text is kept verbatim.

### 4.1 Hand-rolled parser, not a grammar

The expression language is parsed by a small hand-written recursive-descent
parser (`src/expr.cpp`) into the `Expr` AST (`include/parameda/expr.hpp`) — **not**
by a rawast grammar. A rawast grammar was specced and prototyped, then dropped:

- Its marquee benefit — free *bidirectional* serialization (AST → text on save) —
  doesn't apply: parameda stores the authored string and never reconstructs text
  from the expression AST.
- It wasn't even "declarative for free": the literal-text runs needed custom C++
  terminal parsers anyway, at which point a full hand-rolled parser for a
  one-production language is *less* code than grammar + terminals + registration +
  AST-walker — and it is compiled, with no runtime grammar interpretation.

rawast remains the **value/data model** (typed Values + JSON load/save); it is no
longer in the expression path. The `Expr` AST: a template is a list of segments,
each either literal `Text` or an `Action{name, args}`; each arg is a nested
template.

### 4.2 Expression language — one form, two behaviors

There is a **single placeholder production**:

```
$name{ arg, arg, … }
```

`name` is an optional identifier; the braces hold a comma-separated list of
**argument expressions**, each recursively parsed (so `${${x}}` and
`$ENV{${prefix}_HOME}` work — see §4.4). It parses to one AST node,
`Action(name, args)`. Evaluation splits on whether `name` is empty:

- **Empty name → built-in substitution.** `${expr}` is core engine behavior: it
  resolves `expr` to a key and looks it up from the view, applying
  skip-and-continue (§5) and passthrough (§4.3). Hard-wired in C++ — substitution
  has to understand the record graph, the view, and shadowing, so it is *not* a
  registered callback.

- **Named → a registered callback.** `$fn{args}` looks `fn` up in a **function
  registry** (shared across a context lineage) and calls it. Functions don't
  understand the graph; they receive evaluated arguments and return a value. `ENV`
  is a preregistered built-in, and user functions register the same way. This is
  the language's extension surface; the engine core stays minimal.

**Callback contract:**

- **Signature:** `callback(args: [Value], ctx) -> Value`. Most functions touch
  only `args`; `ctx` is a handle back into the calling view.
- **Eager arguments.** Args are evaluated from the view *before* the callback is
  called, with each arg's leading/trailing *literal* whitespace trimmed at parse
  time (template text only — never a value's content). A future special-form door
  — a callback opting into raw, unevaluated arg ASTs — is what a lazy
  `$if{cond, then, else}` would need; deferred, see §9.
- **Return.** Currently a callback returns a **data** Value. Widening the return
  to the full value union (so a callback like `JSON` can return a `Ref`/
  sub-context) is a deferred follow-up.
- **Registration from C++ and Python.** A Python callable can be registered as a
  config function (values cross the same rawast⇄Python boundary as everywhere
  else) — `cfg.register("slug", fn)` — making the language user-extensible without
  touching the core. Registration mutates the shared registry (engine-wide
  setup), not the record graph.
- **Unknown `name`** → an error at evaluation (and flagged by the future `check`
  pass, §11).

**Built-ins:**

- `$ENV{VAR}` — environment variable, empty if unset (shipped).
- `$JSON{path}` — load a JSON file as a lazily-loaded, **cached** sub-folder /
  sub-context (**deferred**: needs the callback return widened to the value union;
  memoized — see §11). NOT inlined text.

Functions will be tagged **pure** vs **effectful** so the `check` pass and the
memoization layer know what is safe to cache (§11).

**Escaping:** `\X` emits the literal character `X` (so `\$` → `$`, `\}` → `}`).

**Staging.** Shipped: the hand-rolled parser + `Expr` AST, built-in substitution,
the function registry with the `ENV` built-in, and C++/Python function
registration. Next: a small standard function set (path join, string
concat/format, arithmetic, conditional) as registered callbacks; `$JSON{}` (needs
the value-union return); a lazy special-form (`if`) once the raw-args door opens.

### 4.3 Typing model: EIAS-lite with passthrough

Strings are the substrate (Tcl-flavored "everything is a string"), with numbers
interpreted on demand — **with one carve-out**: a value that is **exactly** a
single `${x}` returns the underlying value *untouched* (so a `Ref`, sub-context,
or typed number survives). Embed `${x}` inside a larger string and it coerces to
text. This keeps string modeling simple without stringifying refs.

### 4.4 Keys as ASTs

A key is resolved by the same evaluator as a value, with these rules that keep
lookup well-defined and affordable:

- **Resolved from the querying view.** A record's key resolves against the view
  the lookup started from — *not* the record's authored position — consistent
  with the view-anchored rule (§5). This is what makes computed-key templating
  work: a merged template's record keys are computed in the *importing* view's
  context. **(DECIDED — flip candidate.)**
- **Must evaluate to a string.** Numbers coerce to text; a key that resolves to a
  `Ref`, array, or other non-string is an error. Identifiers are strings.
  **(DECIDED — flip candidate.)**
- **Literal fast-path.** A pure-literal key AST is treated as a static string with
  no evaluation, so the common lookup stays cheap (§2).
- **Computed / indirect names.** A placeholder body may itself be an expression,
  so `${${x}}` evaluates the inner expression to produce the name (see §11).

Two records "have the same key" iff their keys resolve to the same string from
the querying view; shadowing and tombstones are defined in those terms (§2).

---

## 5. View-anchored (dynamic) evaluation

**The view node is a stable evaluation context — like `self`/`this` — and it does
not move during evaluation.** Every access re-roots at it:

- To resolve a key from view `V`, scan `gen(V)` (§2), resolving each candidate's
  key from `V`.
- When the winning value (or a key) itself needs evaluating (an expression, a
  referenced key), that nested evaluation **starts over from `V` again** — *not*
  from where the value/key was authored.

Therefore the resolved meaning of a key or value is a function of **(AST, view)**.
The same record evaluated from a different view legitimately yields a different
result — **by design**. This is the templating engine: author a subtree of
`${param}` placeholders (in keys and/or values), link or merge it anywhere, and
it resolves against whatever *that* view supplies.

```
resolve(V, key):
    R = lookup(V, key)               # find winning record (§2); key matching re-roots at V
    if R is UNDEFINED: error
    return eval(R.value, view=V)     # ← always re-root at V

eval(AST, view):
    # interpret AST; every ${k} inside → resolve(view, k)  (fresh stream from view)
    # bare single ${x} → passthrough of the underlying value (§4.3)
```

**Cycle guards** (resolution must stay finite):

- `Merge` targets — visited-set in `gen` (§2).
- Expression reference cycles — the resolver tracks the records currently being
  evaluated (`active`) and **skips them as lookup candidates**. A placeholder
  that would resolve to the binding currently being computed instead walks past
  it to the next (shadowed) match. This makes `x = "${x}-extra"` resolve to the
  *inherited* value of `x` — a `super`-style reference (e.g. `PATH = "${PATH}:/x"`).
  A reference with no such escape falls off the chain and is reported undefined;
  there is no distinct "circular reference" error.
- **Key resolution** — because matching a candidate now evaluates its key (which
  triggers lookups), key resolution carries the same visited-set protection and a
  bound on match-recursion depth. The literal fast-path keeps the common case off
  this path entirely.

---

## 6. What a read returns

- **Scalar / array AST** → the evaluated rawast `Value`, mapped to the host
  language's natural type at the binding boundary (e.g. Python `int`/`str`/`list`).
- **Ref** → a **sub-context** (a view on the target). The engine never auto-walks
  it; the caller may query into it, at which point the target becomes the new
  view and resolution proceeds from there.
- **Deleted / not found** → undefined (an error on `get`, falsy on a `has`-style
  probe — exact API **OPEN**).

---

## 7. Public API (sketch — names OPEN)

Operations are functional: every mutating call returns a new context.

```
ctx.set(key, value)             -> ctx'          # append a binding child
ctx.delete(key)                 -> ctx'          # append a Deleted child (tombstone)
ctx.link(key, target)           -> ctx'          # append a Ref child
ctx.merge(target, merge_first)  -> ctx'          # append a keyless Merge child

ctx.get(key)                    -> value         # resolve (§5); error if undefined
ctx.has(key)                    -> bool          # resolves without raising
ctx.raw(key)                    -> AST | None    # winning record's unevaluated value

ctx.parent()                    -> ctx | None

register(name, callback)                         # register a $name{…} function (§4.2)
```

- **`set` takes a single key only.** Dotted-path convenience (`"build.pass1.log"`)
  is provided by **separate helper functions** layered on top, not by `set` (see
  §7.1).
- `key` arguments are parsed to ASTs like values; pass a plain string for a
  literal key, or an expression for a computed key (§4.4).
- **DEFERRED:** `cnode` index→value (array-style positional children).
- **OPEN:** graph serialization format — keys and values serialize via rawast
  (§4), but the *record graph itself* (parent/key/value rows) needs its own
  on-disk form. A flat `(parent, key, value)` row dump is a natural candidate
  given the model.
- **OPEN:** whether `value` may also carry an opaque host (e.g. Python) object as
  a non-serializable escape hatch.

### 7.1 Dotted-path access

Single-key `set`/`get` are the primitives; dotted paths like `"pdk.a"` are a thin
helper that **folds over the segments, descending through links**. The recipe:

> Resolve the first segment from the current view. If it resolves to a link
> (`Ref` / sub-context), the **target becomes the new view**, and the next segment
> is resolved from there. Repeat to the last segment.

This is just the §6 rule ("a `Ref` yields a sub-context; query into it and the
target becomes the new view") applied in a loop. The general pattern that makes
it useful: **a "folder" is a link to the tip of a chain that holds its entries.**

**Worked example.** Build two branches off a record `r1`:

```
r1
├─ branch A:  r1 ← ra{key:a, val:1} ← r2{key:b, val:2}
└─ branch B:  r1 ← rp{key:pdk, val:Ref(r2)}        ← view = rp
```

i.e. `set a=1` then `set b=2` builds branch A with tip `r2`; then
`set("pdk", Ref(r2))` puts `rp` on a *separate* branch off `r1`, linking to `r2`.

`path(rp, "pdk.a")`:
1. `lookup("pdk")` from view `rp` → `rp` itself matches → value `Ref(r2)`; descend,
   `r2` becomes the view.
2. `lookup("a")` from view `r2` → `gen(r2)` yields `r2{b=2}` (no), `ra{a=1}` (yes)
   → **1**.

`path(rp, "pdk.b")` → descend to `r2`, `gen(r2)` yields `r2{b=2}` → **2**.

Notes:

- **It is Ref-descend, not merge.** A merge record has no key, so it cannot be
  addressed as a path segment; `pdk` is a *named link*. "Nearer wins" inside the
  target chain is automatic — merge-first precedence only matters if `pdk` *also*
  layered its own bindings over the linked target.
- **Bare `a` does not resolve from `rp`.** `rp`'s upward chain is `rp → r1`; the
  `a`/`b` chain is down the *other* branch. Only `pdk.a` reaches them, via the
  link — `pdk` is a named gateway across to another branch's bindings.
- **No namespace boundary.** `gen(r2)` does not stop at `r1`; it keeps walking up
  into `r1`'s ancestors. So `pdk.<x>` also resolves anything visible above `r1`.
  This is consistent with the walk-up-everywhere model (a folder is a *viewpoint*,
  not a closed box). True encapsulation, if ever wanted, needs an explicit stop
  (a parentless root or a halting sentinel) — **OPEN/DEFERRED**.
- **Expressions re-root at the descended view.** If `a`'s value were `"${b}"`,
  descending makes `r2` the view, so it resolves `b=2` from the folder's own tip —
  consistent with §5.

---

## 8. Implementation shape

- **Language / build:** C++17 core + Python bindings via **nanobind**, built with
  **scikit-build-core**. Mirrors the conventions of the sibling project
  [`rawast`](https://github.com/edacommons/rawast).
- **Persistent tree:** records are immutable and shared
  (`shared_ptr<const Record>`), each holding a `parent` handle plus a `key` string
  and a `value` (rawast Value or a graph construct). A context is a record handle
  plus a shared function registry. `set`/`link`/`merge`/`delete` allocate one new
  record, reuse all existing ancestry, and share the registry. Cheap branching and
  snapshots fall out for free.
- **Resolver:** the upward walk (§2) is recursive; a `Merge` recurses into its
  target per its flag, with a visited-set guard. Lookup advances until a
  candidate's resolved key matches and the record is not currently being
  evaluated (the skip-and-continue `active` set, §5).
- **Expressions:** a hand-rolled recursive-descent parser (`src/expr.cpp`) turns a
  string template into the `Expr` AST (`expr.hpp`); the evaluator walks it
  view-anchored (§4.1, §5). Functions live in a `Registry` shared across the
  lineage; `ENV` is built in, and callbacks register from C++ or Python.
- **rawast dependency:** linked via CMake **`FetchContent`**, pinned to a rawast
  **tag** (not a moving branch) for reproducible builds. Local co-development uses
  `-DFETCHCONTENT_SOURCE_DIR_RAWAST=…` to build against a working tree without
  re-fetching. (A git submodule was considered and rejected: it complicates
  scikit-build-core sdists and adds a recursive-clone footgun; the source-dir
  override already covers co-development.)

```cmake
FetchContent_Declare(rawast
    GIT_REPOSITORY https://github.com/edacommons/rawast.git
    GIT_TAG v0.1.9           # pinned
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(rawast)
# target_link_libraries(parameda PUBLIC rawast::rawast)
```

---

## 9. Open questions (consolidated)

1. ~~Expression escaping~~ — RESOLVED: blanket `\X` → literal `X` (§4.2).
2. Function calls: (a) widen the callback return to the value union so a function
   can yield a `Ref`/sub-context (needed for `$JSON{}`); (b) lazy/special-form
   support (raw, unevaluated arg ASTs) for a `$if{…}`-style conditional — when to
   open that door (§4.2); (c) may user functions **override** a built-in name
   (`ENV`), or are built-ins reserved?
3. `get`/`has` error/return conventions for undefined and for `Ref` results.
4. On-disk serialization format for the record graph (keys/values already covered
   by rawast). Flat `(parent, key, value)` rows are the leading candidate.
5. Opaque host-object values as a non-serializable escape hatch — in or out?
6. `cnode` positional/indexed children (arrays) — deferred; revisit when needed.
7. Confirm the two key-resolution decisions in §4.4 (resolve-from-view;
   keys-must-be-strings) — currently DECIDED with defaults, flagged as flip
   candidates.
8. Python-registered callbacks + nanobind GC. The C++ registry holds the Python
   callable as an `nb::object`; under normal scoping it collects fine, but (a) a
   `Context` retained in module globals to interpreter exit prints a benign
   nanobind "leaked instance" teardown warning, and (b) a callback that *closes
   over a Context* forms a reference cycle nanobind cannot trace (a real leak).
   Proper fix: integrate the registry with Python's cyclic GC (tp_traverse) or
   hold callables on the Python side. Deferred.

---

## 10. Superseded material

A first-pass `include/parameda/cfg.hpp` / `src/cfg.cpp` sketch predated this
design (mutable, **definitional** rather than view-anchored scoping, no
record/value model). It has been **removed**; Milestone 1 (`core.hpp`/`core.cpp`)
implements this spec instead.

---

## 11. Reference-implementation notes (ChipFlow `cfg.py`)

The private ChipFlow `cfg.py` is an earlier, **mutable** implementation of the
same underlying idea. It is not a blueprint — Parameda's graph is persistent and
rawast-backed, where `cfg.py` edits folder dicts in place — but several of its
mechanisms validate or inform decisions here.

Confirms what Milestone 1 already does:

- **Shadow-skip ≡ our `active` set (§5).** `cfg.py` carries a per-evaluation
  `stack` mapping name→folder; when `${name}` re-enters while `name` is being
  resolved, it restarts the walk from `stack[name].parent` — skipping the binding
  under computation and taking the next one up. That is exactly our
  skip-and-continue, keyed by name→folder there vs. record identity here.
  Equivalent for a shadow chain; record identity is finer-grained for computed
  keys, so we keep it. (Their `stack` is transient eval state; our `active` plays
  the same role over an immutable graph.)
- **Passthrough ≡ single-part expression (§4.3).** Its expression object returns
  a single part un-stringified and joins multiple parts as text — identical to
  our whole-string-`${x}` passthrough vs. embedded coercion.

Mechanisms to ADOPT at the grammar milestone (§4.1):

1. **Computed / indirect names.** `cfg.py` recursively parses the text *inside* a
   placeholder, so `${${x}}` evaluates the inner expression to produce the name.
   This is the concrete realization of "keys are ASTs" (§4.4): the grammar should
   let a placeholder body be a full expression.
2. **`$JSON{path}` yields a folder, not text.** It loads the file as a
   lazily-evaluated, **cached** sub-folder (sub-context). Implement `$JSON` as a
   `Ref`-like sub-context with memoization — not the inlined-text behavior of the
   throwaway early scaffold.
3. **`check` vs `evaluate` split + memoization.** A resolvability pass (is every
   referenced name defined? does the env var / file exist?) distinct from
   evaluation, plus per-node memoization of evaluated expressions. Maps to a
   richer `has`/`check` than Milestone 1's `has`, and a caching layer once
   evaluation is non-trivial.
```
