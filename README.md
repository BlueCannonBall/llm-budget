# LLM Budget

An HTTP server scaffold for a future LLM API spend limiter. It does not
forward requests, track spending, or enforce a budget yet. Do not point coding
agents at it expecting a working API proxy.

## Build and run

Requires Polybuild, a C++23 compiler, GNU Make, CMake (to build spdlog),
and OpenSSL and SQLite development headers.
For a fresh checkout, first run `git submodule update --init --recursive`.
Polybuild compiles Polyweb, its Polynet submodule, and SJSON from source.

```sh
polybuild generate
make -j2
./llm-budget
```

The server listens only on `127.0.0.1:8787`. Check it with:

```sh
curl -i http://127.0.0.1:8787/health
```

`GET /health` returns `200` and `ok`. Other paths are not implemented.
