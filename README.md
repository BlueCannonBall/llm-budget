# LLM Budget

An experimental LLM API proxy and future spend limiter. It forwards
chat-completion requests to DeepSeek, authenticates callers with its own per-user
API keys, and records an estimated cost for each request. It does not enforce
budgets yet: limits are stored and displayed, but the proxy still serves a user who
has exceeded them.

## Build and run

Requires Polybuild, a C++23 compiler, GNU Make, CMake (used to build spdlog), and
the OpenSSL and SQLite development headers. For a fresh checkout, first run
`git submodule update --init --recursive`. Polybuild compiles Polyweb, its Polynet
submodule, and SJSON from source.

```sh
polybuild generate
make -j2
```

The server reads two files from the current working directory, so run it from the
directory that holds them:

- `keys.json` — the upstream provider keys. It is gitignored and not created for
  you; create it before the first run.

  ```json
  {
      "deepseek": "sk-..."
  }
  ```

- `llm-budget.db` — the SQLite database. It is created on first use.

```sh
./llm-budget
```

The server listens only on `127.0.0.1:8787`.

## Proxy

The proxy route is `POST /chat/completions`.

- Requests must carry `Authorization: Bearer <user API key>`. A missing or
  unrecognized key gets `401`.
- The body must be JSON with a `model` string. Supported models are
  `deepseek-v4-pro` and `deepseek-flash`. Anything else gets `400`.
- The server does not forward the caller's `Authorization` header. It uses the
  upstream key from `keys.json`, and sets `user_id` to the user's name for
  DeepSeek.
- The upstream response is streamed back as received, with `Content-Type` and
  `Retry-After` passed through. Responses are capped at 32 MB.
- While the response streams, token usage is read from the SSE events (or a plain
  JSON body) and priced. The estimate is logged and stored on the request row.

## Browser usage

Open `GET /usage` to enter a user API key. The form submits it with `POST /usage`
and displays that user's used percentages and UTC reset times. Deploy behind
HTTPS to protect the key in transit. The key
is not put in the URL or the response, and the page is marked `no-store`. Users
must enter their key again on a later visit.

## Manage users

The CLI shares the database with the server. Limits are dollar amounts with at
most nine decimal places (stored as nanodollars).

```sh
./llm-budget --help
./llm-budget user add alice --five-hour-limit 1.25 --weekly-limit 10
./llm-budget user list
./llm-budget user show alice
./llm-budget user usage alice
./llm-budget user set-limits alice --five-hour-limit 2 --weekly-limit 15
./llm-budget key rotate alice
```

`user add` and `set-limits` require both limits. `user add` and `key rotate` print
the new API key once. Store it securely: the database stores only its SHA-256
hash, and rotating immediately invalidates the old key. `user list` and
`user show` print the ID, name, five-hour limit, and weekly limit, but never the
key. `user usage` prints recorded spending in dollars and as a percentage of
each limit, with active window reset times in the machine's local timezone.
A missing user is an error.

## Cost estimation

Costs are priced in USD (DeepSeek only) and stored as nanodollars, or billionths of
a dollar, so small requests do not round to zero.

- DeepSeek off-peak rates are used, doubled during peak hours: Beijing-time
  weekdays, 01:00–04:00 and 06:00–10:00 UTC, excluding Chinese public holidays.
  Weekdays and holidays are determined in Beijing time, and only the 2026
  calendar is implemented.
- If the model, the usage fields, or the time cannot be priced, the cost is left
  unset and a warning is logged. The request is still proxied.

## Tests

```sh
python3 tests/cli.py ./llm-budget
python3 tests/usage_page.py ./llm-budget  # port 8787 must be free
```

The three C++ tests are standalone `main()` programs, built with the same include
paths as the server:

```sh
g++ -std=c++23 -O2 -pthread -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE \
    -I. -ISJSON/src -Ispdlog/include tests/cost.cpp \
    SJSON/src/value.cpp SJSON/src/token.cpp SJSON/src/sjson.cpp \
    -o test_cost -lssl -lcrypto
g++ -std=c++23 -O2 -pthread -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE \
    -I. -ISJSON/src -Ispdlog/include tests/database.cpp database.cpp \
    -o test_database -lssl -lcrypto -lsqlite3
g++ -std=c++23 -O2 -pthread -I. -ISJSON/src -Ispdlog/include tests/sqlite.cpp \
    -o test_sqlite -lsqlite3
```

`test_database` uses `llm-budget.db` in the current directory, so run it from an
empty temporary directory.
