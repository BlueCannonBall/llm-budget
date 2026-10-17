# Security & Credential Rules
- NEVER read, cat, grep, or inspect `keys.json`, `*secret*`, or `*.env`.
- NEVER dispatch external network requests using real provider API keys.
- When testing the proxy, always send requests to `http://127.0.0.1:8787` using local test user keys, never upstream endpoints directly.

# C++ Style
- Keep code readable: use braced, multiline conditional bodies and wrap long conditions and structured test data instead of packing them onto single lines.
- Keep short function declarations on one line; do not split a two-argument declaration merely to wrap it.
- Spell the unsigned integer type `unsigned int`, not bare `unsigned`.
- Declare map lookup iterators in `if` initializers and name them descriptively with an `_it` suffix.
- Prefer C-style casts over functional casts or C++ named casts.
- Prefer success in the `if` branch and failure in `else`; retain failure-first guards when they avoid unnecessary nesting. Omit redundant `else` after a branch that returns or throws unless the next branch needs variables declared in the `if` initializer.
