#pragma once

// The parameda expression mini-language (SPEC §4).
//
// A stored key or value (when it is a string) is a TEMPLATE: literal text
// interleaved with `$name{ arg, arg, … }` placeholders.
//   - empty name → built-in substitution        ${ key }
//   - a name     → a registered function call    $ENV{…}  $fn{…}
// Args are comma-separated, each itself a nested template (so `${${x}}` and
// `$ENV{${prefix}_HOME}` work). `\X` escapes a literal character.
//
// We hand-roll the parser into this small AST (no grammar): a TEMPLATE is a
// list of segments, each either literal text or an action. The evaluator
// (Context, §5) walks it view-anchored.

#include <string>
#include <vector>

namespace parameda {

struct Segment;
using Expr = std::vector<Segment>; // a template, or a single argument

struct Action {
    std::string name;          // empty ⇒ substitution
    std::vector<Expr> args;     // each arg is a nested template
};

struct Segment {
    enum class Kind { Text, Action };
    Kind kind;
    std::string text;          // Kind::Text — literal run (escapes already applied)
    Action action;             // Kind::Action
};

// Parse a template string into an Expr. Throws std::runtime_error on a syntax
// error (e.g. an unterminated `$name{`).
Expr parse_template(const std::string& src);

} // namespace parameda
