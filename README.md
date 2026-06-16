# Parameda

**Param + EDA** — a hierarchical configuration system for EDA (Electronic Design Automation) tooling.

Parameda organizes parameters in a tree of nested "config folders" with parent/child
scoping, lazy variable substitution, and JSON-backed load/save. It is designed to give
EDA flows (and any tool with deeply nested, environment-dependent settings) a clean,
reusable way to express and resolve configuration.

## Features

- **Hierarchical config-folder tree** — nested named variables organized in folders,
  with parent/child scoping where child values shadow parents
  (e.g. `cfg.build.pass1.log` vs `cfg.build.pass2.log`).
- **Variable substitution**
  - `${var}` — hierarchical lookup up the parent chain
  - `$ENV{VAR}` — environment variable
  - `$JSON{path}` — load a JSON file as a nested folder
- **Lazy placeholder evaluation** with circular-reference detection.
- **JSON-backed** — load (merge / fill-undefined) and save/serialize.

## Status

Early development.

## License

[MIT](LICENSE)
