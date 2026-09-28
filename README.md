# LLM Budget

An experimental LLM API proxy and future spend limiter. It currently forwards
chat-completion requests to DeepSeek and estimates costs, but does not enforce
budgets or authenticate requests using its own user keys yet.

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

The server listens only on `127.0.0.1:8787`. The current proxy route is
`POST /chat/completions`; it forwards the request's `Authorization` header
to DeepSeek.

## Manage users

The CLI and server use `llm-budget.db` in the current working directory. Limits
are dollar amounts with at most nine decimal places (stored as nanodollars).

```sh
./llm-budget user add alice --five-hour-limit 1.25 --weekly-limit 10
./llm-budget user list
./llm-budget user show alice
./llm-budget user set-limits alice --five-hour-limit 2 --weekly-limit 15
./llm-budget key rotate alice
```

`user add` and `key rotate` print the new API key once. Store it securely: the
database stores only its hash, and rotating immediately invalidates the old key.
`user list` and `user show` display the ID, name, five-hour limit, and weekly
limit, but never the key. These keys are not yet used to authenticate proxy
requests.
