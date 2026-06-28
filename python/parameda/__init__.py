"""parameda — Param + EDA: a hierarchical configuration system for EDA tooling.

The configuration space is a persistent, append-only tree of records. A
``Context`` is a handle to one record — the "view" you stand on — and all reads
resolve by walking *up* the parent chain. Writes never mutate; they append a
child and return a new context.

    import parameda

    cfg = parameda.root().set("root", "/opt").set("log", "${root}/build.log")
    cfg.get("log")        # "/opt/build.log"  (resolved from this view)

See docs/SPEC.md for the full model.
"""

from ._native import Context, root, Undefined, __version__

__all__ = ["Context", "root", "Undefined", "__version__"]
