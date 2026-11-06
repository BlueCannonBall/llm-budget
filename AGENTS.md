# Security & Credential Rules
- NEVER read, cat, grep, or inspect `keys.json`, `*secret*`, or `*.env`.
- NEVER dispatch external network requests using real provider API keys.
- When testing the proxy, always send requests to `http://127.0.0.1:8787` using local test user keys, never upstream endpoints directly.

# C++ style
Formatting is enforced by `.clang-format` (LLVM base, four-space indent,
`AccessModifierOffset: -4`, `IndentCaseLabels: false`, `SpaceAfterCStyleCast: true`,
`NamespaceIndentation: All`). `ColumnLimit` is `0`, so the formatter never wraps
lines; wrap long conditions and structured test data by hand.

- Keep code readable: use braced, multiline conditional bodies and wrap long
  conditions and structured test data instead of packing them onto single lines.
- Keep short function declarations on one line; do not split a two-argument
  declaration merely to wrap it.
- Functions, members, and file-local variables use snake_case without trailing
  underscores. Types use PascalCase.
- Spell the unsigned integer type `unsigned int`, not bare `unsigned`. Use
  unqualified fixed-width integer types (`uint64_t`, not `std::uint64_t`).
- Prefer C-style casts over functional casts or C++ named casts, and over
  `static_cast`. `.clang-format` adds the space after the cast.
- Declare map lookup iterators in `if` initializers and name them descriptively
  with an `_it` suffix.
- Prefer success in the `if` branch and failure in `else`; retain failure-first
  guards when they avoid unnecessary nesting. Omit redundant `else` after a branch
  that returns or throws unless the next branch needs variables declared in the `if`
  initializer.
- Put `case` labels at the `switch`'s indentation, not indented beneath it.
- Put non-public state before the public API and do not interleave access blocks.
  Default to `protected`; use `private` only when no derived class could need the
  member.
- Separate logical stages with blank lines.
- Declare file-local helpers `static`.

# Namespaces and file layout
Namespaces identify library-like subsystems, not individual application files:
`providers`, `cost`, `database`, and `sqlite`. Application APIs share global scope
with `Channel`: `run_cli`, `parse_nanodollars`, `UsagePageAssets`, and
`handle_usage_request`. Shared types (`User`, `UsageLimits`, `UserUsage`), ID
typedefs, and enums stay global.

Persistence functions and implementation details belong to `namespace database` and
are initialized with `database::init()`. Callers use explicit `database::`
qualification; there is no umbrella namespace or compatibility alias.

# SQL literals
Indent raw SQL bodies one level inside their C++ statements. Align raw-string
closing delimiters with the statement, including inside namespaces.

# JavaScript style
Client code in `web/` uses camelCase for custom identifiers and four-space
indentation. Native DOM API names and wire-format fields keep their own casing.
