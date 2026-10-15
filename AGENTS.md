# Security & Credential Rules
- NEVER read, cat, grep, or inspect `keys.json`, `*secret*`, or `*.env`.
- NEVER dispatch external network requests using real provider API keys.
- When testing the proxy, always send requests to `http://127.0.0.1:8787` using local test user keys, never upstream endpoints directly.

# C++ Style
- Spell the unsigned integer type `unsigned int`, not bare `unsigned`.
- Declare map lookup iterators in `if` initializers and name them descriptively with an `_it` suffix.
- Prefer C-style casts over functional casts or C++ named casts.
