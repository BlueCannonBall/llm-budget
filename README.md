# LLM Budget

An LLM API proxy and spend limiter for DeepSeek. It supports OpenAI Chat
Completions and Anthropic Messages formats, authenticates callers with per-user
API keys, records estimated costs in USD, and rejects new requests when a user's
five-hour or weekly budget is exhausted.

Budget checks happen before each upstream request. They are not hard spending
caps: an admitted request can exceed the remaining budget, and concurrent
requests or missing usage reports can cause overspending.

## Build and run

Requires a POSIX environment, a C++23 compiler and standard library with
`std::generator` and explicit object parameter support, GNU Make, CMake (used to
build spdlog), and the OpenSSL and SQLite development headers. SQLite must support
`RETURNING` (3.35 or newer). The generated makefiles are checked in, so normal
builds do not require Polybuild. Make compiles Polyweb, its Polynet submodule, and
SJSON from source.

```sh
git submodule update --init --recursive
make -j2
```

Polybuild is only needed to regenerate the makefiles when source files, include
dependencies, or `Polybuild.toml` change: run `polybuild generate` before rebuilding.
Ordinary edits covered by the existing dependency rules only need `make`.

The server reads two files from the current working directory, so run it from the
directory that holds them:

- `keys.json` — the upstream provider keys. It is gitignored and not created for
  you; create it before the first run.

  ```json
  {
      "deepseek": "sk-..."
  }
  ```

- `llm-budget.db` — the SQLite database. It is created on first use and uses WAL
  mode, so SQLite may also create `llm-budget.db-wal` and `llm-budget.db-shm`.

```sh
./llm-budget
```

The server listens only on `127.0.0.1:8787`; its address and port are not
configurable. For remote access, put it behind an HTTPS reverse proxy that
preserves the routes below. Protect `keys.json`, the database, and logs with
restrictive permissions; use `umask 077` before creating users or starting the
server. Provider keys are loaded once at startup, so changes require a restart.

## Proxy

Both routes forward JSON requests directly to these DeepSeek endpoints:

| Local route | User authentication | Upstream endpoint |
| --- | --- | --- |
| `POST /chat/completions` | `Authorization: Bearer <user API key>` | `https://api.deepseek.com/chat/completions` |
| `POST /v1/messages` | `x-api-key: <user API key>` or Bearer authorization | `https://api.deepseek.com/anthropic/v1/messages` |

Use `http://127.0.0.1:8787` as the SDK base URL for either format. Do not append
`/v1` or `/anthropic`; the SDK supplies the route path. OpenAI Responses is not
implemented.

- Missing, malformed, or unrecognized user credentials get `401`. On the
  Messages route, `Authorization` takes precedence if both headers are supplied.
- The body must be JSON with a `model` string. Supported models are
  `deepseek-v4-pro` and `deepseek-flash`. Other names, including Claude aliases,
  get `400`. Methods other than `POST` get `405`.
- Caller credentials are replaced with the DeepSeek key from `keys.json`:
  Bearer authorization for Chat Completions, `x-api-key` for Messages.
- For upstream user isolation, Chat Completions sets `user_id` and Messages sets
  `metadata.user_id` to the authenticated user's name, overriding caller-supplied
  values. Other Messages metadata is preserved; missing or null metadata is
  created, and non-object metadata gets `400`. Names are forwarded unchanged;
  the CLI does not validate upstream user-ID requirements.
- Messages sends `anthropic-version: 2023-06-01`. Caller version/beta headers are
  not forwarded.
- The upstream response is streamed back as received, with `Content-Type` and
  `Retry-After` passed through. Responses are capped at 32,000,000 bytes.
- While the response streams, token usage is read from the SSE events (or a plain
  JSON body) and priced. The estimate is logged and stored on the request row.
- The proxy does not add `stream_options.include_usage` for streaming Chat
  Completions. Request usage from the upstream when needed; a response without
  usage cannot be priced.

## Budget enforcement

Each user has separate five-hour and weekly limits. These are fixed windows, not
rolling lookbacks: they start with an accepted request and restart on the next
accepted request after expiration. Recorded costs are attributed by request start
time.

- Exhausting either limit rejects new requests with `422`, including the UTC reset
  time in the response body.
- Setting either limit to zero blocks requests with `403`; zero does not mean
  unlimited.
- Active requests are not stopped when they exhaust a budget, and no estimated
  cost is reserved before starting a request. Enforcement uses costs already
  recorded in the database.
- Changing limits applies to subsequent budget checks without restarting the
  server, but does not reset window start times or erase recorded spending.

## Browser usage

Open `GET /usage` to enter a user API key. The form submits it with `POST /usage`
and displays that user's used percentages and UTC reset times. Deploy behind
HTTPS to protect the key in transit. The key is not put in the URL or the response,
and the page is marked `no-store`. Users must enter their key again on a later
visit.

## Manage users

The CLI shares the database with the server: run it from the same working
directory. It does not require `keys.json`. Limits are nonnegative dollar amounts
with at most nine decimal places (stored as nanodollars), up to
`9223372036.854775807`.

```sh
./llm-budget --help
./llm-budget user add alice --five-hour-limit 1.25 --weekly-limit 10
./llm-budget user list
./llm-budget user show alice
./llm-budget user usage alice
./llm-budget user set-limits alice --five-hour-limit 2 --weekly-limit 15
./llm-budget key rotate alice
```

`user add` and `set-limits` require both limits. User names must be nonempty and
unique. `user add` and `key rotate` print the new 64-character hexadecimal API key
once. Store it securely: the database stores only its SHA-256 hash, and rotating
immediately invalidates the old key for new authentication.
`user list` and `user show` print the ID, name, five-hour limit, and weekly limit, but never the
key. `user usage` prints recorded spending in dollars and as a percentage of
each limit, with active window reset times in the machine's local timezone.
A missing user is an error.

## Usage graph

Generate total and day-by-day per-user spending charts with Python 3 (standard
library only):

```sh
python3 usage_graph.py
# Optional database and output paths:
python3 usage_graph.py --db /path/to/llm-budget.db --output spending.svg
```

Open the generated `usage.svg` in a browser or image viewer. The script reads the
database without modifying it and sorts all users by recorded spending in USD,
including users with zero spending. It covers the 30 days ending at run time,
or starts at the earliest recorded request if the database has less history.
The exact interval is shown in UTC; the earliest request is the available
history marker, not the database creation date.

The chart keeps the per-user totals at the top and adds a daily bar chart for
each user beneath them, in the same spending order. Daily charts share a USD
scale for comparison and include zero-spend days. Dates are UTC calendar days;
the first and last days include only the part inside the exact reporting
interval, so a 30-day window can touch 31 dates. Hover over a daily bar (or its
empty column) in a browser to see the exact amount and missing-cost count.

Costs are attributed by request start time, including recorded costs on active
or interrupted requests. Missing costs are excluded and counted above the chart.
These are the application's estimates, not provider invoices. An empty database
produces a chart with no spending. The output contains user names and spending;
share it accordingly.

## Cost estimation

Both API formats use the rates implemented in `cost.hpp`, in USD. Costs and limits
are stored as integer nanodollars (1,000,000,000 = $1), so small requests do not
round to zero.

Implemented off-peak rates per million tokens:

| Model | Cache-hit input | Cache-miss input | Output | Pricing starts (UTC) |
| --- | --- | --- | --- | --- |
| `deepseek-flash` | $0.003 | $0.15 | $0.60 | 2026-09-10 04:00 |
| `deepseek-v4-pro` | $0.022 | $0.66 | $1.98 | 2026-08-16 16:00 |

- Cache-hit input, cache-miss input, and output tokens are priced separately.
  Reasoning tokens are already included in output counts and are not added again.
- Messages streaming combines the initial `message_start.message.usage` with
  later `message_delta.usage` updates. Supplied fields replace earlier values;
  omitted fields are retained. Cumulative counts are never added together.
  Cache-creation input uses DeepSeek's ordinary cache-miss rate, not Anthropic's
  cache-write pricing.
- Off-peak rates are doubled during peak hours: Beijing-time weekdays,
  01:00–04:00 and 06:00–10:00 UTC, excluding Chinese public holidays. Pricing
  uses request start time. Weekdays and holidays are determined in Beijing time;
  make-up working weekends remain off-peak. Only the 2026 calendar is implemented:
  requests outside that Beijing-time year, or before a model's pricing start,
  cannot be priced.
- If a usage report cannot be priced, a warning is logged and no new cost update
  is made. An earlier estimate remains stored; without one, the cost is unset.
  Missing usage fields default to zero during conversion, so incomplete reports
  can underestimate costs. Unknown costs do not contribute to budget totals.

## Known limitations

- Messages SSE `error` events and the terminal `message_stop` event are not used
  to determine request state. A stream that errors or closes before its terminal
  event can be marked completed if the HTTP fetch succeeds.
- Proxy-generated errors are plain text, not provider-native JSON error objects.
- The tests cover local behavior and accounting fixtures, not live DeepSeek API
  compatibility or exact billing reconciliation.

## Tests

Run the Python tests from the repository root after building the server:

```sh
python3 tests/cli.py ./llm-budget
python3 tests/usage_page.py ./llm-budget  # port 8787 must be free
python3 tests/proxy_callbacks.py          # invokes g++ with -std=c++23
python3 tests/usage_graph.py
```

These tests use temporary directories and do not make upstream requests.

`proxy_callbacks.py` compiles the actual proxy callbacks and isolation code from
`main.cpp` with stubbed storage. It checks both formats' JSON/SSE accounting,
fragmented events, cumulative updates, explicit zeros, invalid counts, overflow,
upstream URLs, and trusted Messages user isolation without making upstream requests.

`usage_graph.py` checks UTC daily buckets, the rolling cutoff, future-row
exclusion, shorter history, missing costs, zero-spending days/users, and escaped
SVG labels using a temporary SQLite database.

The three C++ tests are standalone `main()` programs, built with the same include
paths as the server:

```sh
g++ -std=c++23 -O2 -I. tests/cost.cpp SJSON/src/value.cpp -o test_cost
./test_cost
g++ -std=c++23 -O2 -pthread -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE \
    -I. -ISJSON/src -Ispdlog/include tests/database.cpp database.cpp \
    -o test_database -lssl -lcrypto -lsqlite3
g++ -std=c++23 -O2 -pthread -I. -ISJSON/src -Ispdlog/include tests/sqlite.cpp \
    -o test_sqlite -lsqlite3
./test_sqlite
```

`test_database` uses `llm-budget.db` in the current directory, so run it from an
empty temporary directory, not the directory containing your server database:

```sh
database_test="$PWD/test_database"
test_dir=$(mktemp -d)
(cd "$test_dir" && "$database_test")
rm -r "$test_dir"
```
