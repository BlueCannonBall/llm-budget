# Security & Credential Rules
- NEVER read, cat, grep, or inspect `keys.json`, `*secret*`, or `*.env`.
- NEVER dispatch external network requests using real provider API keys.
- When testing the proxy, always send requests to `http://127.0.0.1:8787` using local test user keys, never upstream endpoints directly.
