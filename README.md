# LLM Budget

An LLM API proxy and budget limiter for DeepSeek, OpenCode Go, and selected OpenAI
models. It supports OpenAI Chat Completions, OpenAI Responses, and Anthropic Messages formats,
authenticates callers with per-user API keys, and rejects new requests when a
user's five-hour or weekly dollar budget is exhausted. Direct API requests count
estimated token cost; Go requests count an allocated share of one shared subscription.

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

First-party C++ follows `main.cpp` and `.clang-format`: four-space indentation,
snake_case functions and members without trailing underscores, protected state
before the public API, unqualified fixed-width integer types, C-style numeric
casts, and `case` labels aligned with `switch`. Keep blank lines between logical
stages. File-local C++ helpers are `static`. Namespaces identify library-like
subsystems (`providers`, `cost`, `database`, and the existing dependency libraries),
not individual application files. Application APIs such as `run_cli`,
`parse_nanodollars`, `UsagePageAssets`, and `handle_usage_request` share application
scope with `Channel`. Persistence functions and implementation details belong to
`namespace database`, initialized with `database::init()`. Shared types (`User`,
`UsageLimits`, `UserUsage`), ID typedefs, and enums remain global. Callers use explicit
`database::` qualification; there is no `init_db` alias, umbrella namespace, or
compatibility alias. JavaScript follows `lux/index.js`: camelCase custom identifiers
and four-space indentation. Native DOM API names and wire-format fields are unchanged.
Raw SQL bodies are indented one level inside their C++ statements; raw-string
closing delimiters align with the statement, including inside namespaces.

The server loads data and page assets from the current working directory, so run
it from the directory that holds them:

- `keys.json` — the upstream provider keys. It is gitignored and not created for
  you; create it before the first run.

  ```json
  {
      "deepseek": "sk-...",
      "opencode-go": {
          "api_key": "your-go-key",
          "plan": "go"
      },
      "openai": "sk-..."
  }
  ```

  For `opencode-go`, `plan` must be `"go"` or `"go-plus"`. Subscription prices are
  fixed at **$10/month for Go** and **$40/month for Go Plus**, alongside the
  published token rates and allowances. The object contains only `api_key` and
  `plan`; the old bare Go key string is not accepted. Direct-provider keys remain
  strings. Invalid Go configuration stops startup.

- `llm-budget.db` — the SQLite database. It is created on first use and uses WAL
  mode, so SQLite may also create `llm-budget.db-wal` and `llm-budget.db-shm`.

- `web/` — the usage page's HTML, CSS, and JavaScript. Ship this tracked directory
  with the executable. The server reads the assets once at startup and fails with
  an explicit error if a file is missing or the HTML template marker is absent.

```sh
./llm-budget
```

The server listens only on `127.0.0.1:8787`; its address and port are not
configurable. For remote access, put it behind an HTTPS reverse proxy that
preserves the routes below. Protect `keys.json`, the database, and logs with
restrictive permissions; use `umask 077` before creating users or starting the
server. Provider keys are loaded once at startup, so changes require a restart.

## Proxy

All three routes forward JSON to the provider selected by the request's model:

| Local route | User authentication | Available providers |
| --- | --- | --- |
| `POST /chat/completions` | `Authorization: Bearer <user API key>` | DeepSeek, Go Chat models, OpenAI Chat-capable models |
| `POST /v1/messages` | `x-api-key: <user API key>` or Bearer authorization | DeepSeek, Go Messages models |
| `POST /responses` | `Authorization: Bearer <user API key>` | DeepSeek, Go Responses models, OpenAI models |

Provider-qualified names pin routing without ambiguity:

```text
deepseek/deepseek-flash
opencode-go/deepseek-v4-flash
opencode-go/minimax-m2.7
opencode-go/gpt-5.6-luna
openai/gpt-4.1-mini
```

The proxy replaces the outbound `model` with the upstream model ID. Unqualified
`deepseek-flash` and `deepseek-v4-pro` keep their direct DeepSeek defaults; other
models require qualification. There is no automatic fallback to another provider.
Each concrete provider owns its routing/pricing catalog in `providers/`. Checked
October 6, 2026:

- **DeepSeek:** both canonical current IDs, `deepseek-flash` (V4.1 Flash) and
  `deepseek-v4-pro`, on Chat Completions, Messages, and Responses. Retired Flash
  aliases and old Chat/Reasoner IDs are not separate catalog entries.
  Sources: [models/pricing](https://api-docs.deepseek.com/quick_start/pricing/),
  [Responses compatibility](https://api-docs.deepseek.com/guides/responses_api).
- **OpenCode Go:** all 30 [documented models](https://opencode.ai/v2/docs/console/go),
  with their native protocols, token rates, long-context tiers, and both Go/Go Plus
  allowances. GPT Luna, Grok, and Muse Spark use Responses; Qwen and MiniMax use
  Messages; the remaining models use Chat Completions.
- **OpenAI:** 35 text/coding models, with endpoint support checked against each
  [model page](https://developers.openai.com/api/docs/models) and standard rates
  from [pricing](https://developers.openai.com/api/docs/pricing):

| Family | Model IDs (prefix with `openai/`) |
| --- | --- |
| GPT-6 | `gpt-6-astra`, `gpt-6.1-sol`, `gpt-6-sol`, `gpt-6-luna` |
| GPT-5.6 | `gpt-5.6-sol`, `gpt-5.6-terra`, `gpt-5.6-luna` |
| GPT-5.5 / 5.4 | `gpt-5.5`, `gpt-5.5-pro`, `gpt-5.4`, `gpt-5.4-mini`, `gpt-5.4-nano`, `gpt-5.4-pro` |
| GPT-5.2 / 5.1 / 5 | `gpt-5.2`, `gpt-5.2-pro`, `gpt-5.1`, `gpt-5`, `gpt-5-mini`, `gpt-5-nano`, `gpt-5-pro` |
| Codex | `gpt-5.3-codex`, `gpt-5.2-codex`, `gpt-5.1-codex`, `gpt-5.1-codex-max`, `gpt-5.1-codex-mini`, `gpt-5-codex` |
| GPT-4 | `gpt-4.1`, `gpt-4.1-mini`, `gpt-4.1-nano`, `gpt-4o`, `gpt-4o-mini` |
| Reasoning | `o3`, `o3-pro`, `o4-mini`, `o3-mini` |

All listed OpenAI models support Responses. Codex, GPT Pro, and `o3-pro` models
are Responses-only; the others also support Chat Completions. Prefer Responses
for tools: individual models can restrict Chat Completions features even when
the endpoint itself is supported. Published long-context prices apply to the
whole request above 272,000 input tokens where a model defines that tier.
Models without a published cached-input discount use ordinary input pricing.
Audio, image generation, embeddings, research/tool-fee models, restricted cyber
models, dated snapshots, and moving ChatGPT aliases are intentionally excluded:
this is a useful text/coding catalog, not every OpenAI product.

Use `http://127.0.0.1:8787` as the SDK base URL for all three formats. Do not append
`/v1` or `/anthropic`; the SDK supplies the route path. For example:

```sh
curl http://127.0.0.1:8787/responses \
    -H "Authorization: Bearer $LLM_BUDGET_API_KEY" \
    -H 'Content-Type: application/json' \
    -d '{"model":"opencode-go/gpt-5.6-luna","input":"Explain this code","stream":true}'
```

Responses request fields are forwarded without protocol conversion; only the
provider-qualified model name is replaced. Both JSON and SSE responses are
passed through. This endpoint implements response creation, not retrieval,
deletion, cancellation, compaction, or WebSocket transport.

- Missing, malformed, or unrecognized user credentials get `401`. On the
  Messages route, `Authorization` takes precedence if both headers are supplied.
- The body must be JSON with a `model` string. Unknown models and models
  incompatible with the incoming API format get `400`. Methods other than
  `POST` get `405`.
- Only configure keys for providers you use. A missing, empty, or non-string
  key for a selected provider gets `503` before request admission.
- A model whose provider cannot resolve its cost multiplier gets `503` before
  scheduling upstream work or creating a request row. A resolved zero multiplier
  is valid and records zero cost; normal user budget admission still applies.
- Caller credentials are replaced with the selected provider key from
  `keys.json`: Bearer authorization for Chat Completions and Responses, `x-api-key` for Messages.
- Go requests preserve the caller's `User-Agent`, falling back to `llm-budget/1.0`
  when it is absent or empty. `x-opencode-session`, Claude Code's
  `X-Claude-Code-Session-Id`, and Codex's `session-id` and `thread-id` are forwarded
  unchanged when present; they are not translated into one another. See
  [Go's client guidance](https://opencode.ai/v2/docs/console/go#where-can-i-use-it),
  [Claude Code's request headers](https://code.claude.com/docs/en/llm-gateway#request-headers),
  and [Codex's session headers](https://github.com/openai/codex/pull/22193).
- For direct DeepSeek upstream user isolation, Chat Completions sets `user_id`,
  Responses sets `user`, and Messages sets `metadata.user_id` to the authenticated
  user's name, overriding caller-supplied
  values. Other Messages metadata is preserved; missing or null metadata is
  created, and non-object metadata gets `400`. Names are forwarded unchanged;
  the CLI does not validate upstream user-ID requirements.
- Messages sends `anthropic-version: 2023-06-01`. Caller version/beta headers are
  not forwarded.
- The upstream response is streamed back as received, with `Content-Type` and
  `Retry-After` passed through. Responses are capped at 32,000,000 bytes.
- All upstream fetches use a 120-second socket read timeout and a 30-second
  send timeout. The read timeout bounds inactivity, not total stream duration.
  A fetch failure returns `502` if response headers have not been sent; otherwise
  it closes the stream. The request is recorded as interrupted.
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

- Exhausting either limit rejects new requests with `429`, including the UTC reset
  time in the response body and a `Retry-After` header in whole seconds, rounded
  up to the reset (minimum one second). If both limits are exhausted, retry waits
  for the later reset.
- Setting either limit to zero blocks requests with `403`; zero does not mean
  unlimited. Disabled budgets have no automatic reset and no `Retry-After`.
- Active requests are not stopped when they exhaust a budget, and no estimated
  cost is reserved before starting a request. Enforcement uses costs already
  recorded in the database.
- Changing limits applies to subsequent budget checks without restarting the
  server, but does not reset window start times or erase recorded spending.
- All providers contribute their calculated request costs to the same user limits.
  These local windows do not reproduce Go's account-wide subscription limits.
  Go can still reject a request even when the user's local budget remains.

## Browser usage

Open `GET /usage` to enter a user API key. The form submits it with `POST /usage`
and displays that user's used percentages and reset times in the browser's
timezone and locale. The reset column names the timezone; with JavaScript
disabled, both the heading and timestamps explicitly use UTC. Deploy behind
HTTPS to protect the key in transit. The key is not put in the URL or the response,
and the page is marked `no-store`.

The page uses a compact layout with light/dark colors following the browser's
preference. After a successful lookup, the API key form collapses into **Key
settings**, leaving usage and the **Refresh usage** action visible. Expand the
settings to change or forget a key. Percentages remain accurate above 100%;
the accompanying progress bars stop at full.

Page markup, styles, and client behavior live in `web/usage.html`,
`web/usage.css`, and `web/usage.js`; `usage_page.hpp` handles server rendering and
form authentication. Assets are ordinary runtime files, cached at startup.
No CDN, compiler extensions, generated asset headers, or JavaScript build step
are needed. Asset edits require a server restart, but not a C++ rebuild.

With JavaScript enabled, select **Remember on this browser** before submitting
to save a successfully validated key in this site's `localStorage`. Later visits
automatically load usage; **Refresh usage** retrieves the latest spending without
re-entering the key. Remembering is opt-in. **Forget key** removes the saved key
and clears the displayed usage; unchecking the option also removes the saved key.
An invalid or rotated saved key is removed after a rejected lookup.

Browser storage holds the API key itself, not a restricted usage-only credential.
Anyone with access to the browser profile, or scripts running on the same origin,
can access it and use it for API requests. Enable remembering only on a trusted,
private device. Forgetting does not revoke the key; use `key rotate` to revoke it.
Without JavaScript or available browser storage, manual key entry still works,
but remembering is unavailable.
Other methods return `405` with `Allow: GET, POST`.

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
key. `user usage` prints recorded budget debits in dollars and as a percentage of
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

The CLI, usage page, and graph aggregate request costs: direct token costs plus
allocated Go subscription costs. These totals are not actual provider invoices.

## Cost estimation

`providers.hpp` defines the shared `Model` and `Rates` records, native protocol
flags, and pricing schedules. Model records use local upstream names and named
fields for rates, optional input-token tiers, and schedules.
They contain neither registry indexes nor repeated provider prefixes.

Each concrete provider in `providers/` owns its metadata and static model catalog:
`DeepSeekProvider`, `OpenAIProvider`, and `OpenCodeGoProvider` inherit directly
from the `ConfiguredProvider` interface in `configured_provider.hpp`.
Credentials and ordinary authentication are shared; configuration and request
behavior specific to a provider live in its concrete implementation. Only
`OpenCodeGoProvider` owns Go subscription state and the separate plan allowance table.

`provider_config.hpp` constructs the providers and resolves names to a
provider/model pair. Lookup uses provider names, not positions in a registry or
an aligned configuration array. Known models remain resolvable when credentials
are absent, so unsupported protocols still get `400` and unavailable providers
get `503`.

Adding a model with an existing pricing scheme requires a named catalog row in
its provider, not a new pricing function. Go models also need allowances for
both Go and Go Plus in `providers/opencode_go.hpp`. Use `PROTOCOL_*` flags for
native protocols and `SCHEDULE_*` constants for schedules.

`cost.hpp` normalizes API usage and calculates token costs, applying the
provider's resolved exact multiplier before rounding. It has no subscription
plan or allowance logic. All routes require a resolved multiplier before
dispatch. Background accounting reads static model/provider metadata and that
multiplier value; it does not retain configured-provider objects or make virtual
calls for streamed usage events.

`input_token_threshold` selects `above_threshold_rates` when the total input
token count exceeds the threshold, including cache reads and cache writes but
excluding output. The selected rates apply to the whole request, not only the
tokens above the threshold.

Rates use integer picodollars per token to represent fractional nanodollar rates.
Checked 128-bit intermediates retain precision. The final request cost is rounded
up once to integer nanodollars (1,000,000,000 = $1).

### Direct pricing

Direct providers count the calculated token cost without a subscription multiplier.
Existing DeepSeek off-peak prices per million tokens remain:

| Model | Cache-hit input | Cache-miss input | Output |
| --- | --- | --- | --- |
| `deepseek-flash` | $0.003 | $0.15 | $0.60 |
| `deepseek-v4-pro` | $0.022 | $0.66 | $1.98 |

DeepSeek rates double during 01:00–04:00 and 06:00–10:00 UTC on Beijing-time
weekdays, excluding Chinese public holidays. Request start time selects pricing.
Make-up working weekends remain off-peak. Only the 2026 holiday calendar is
implemented; direct DeepSeek requests in other years cannot be priced.
The calculator uses the current catalog rates, with no historical rate lookup
or pricing-start cutoff. OpenAI Mini rows use standard text-token rates, not
batch, priority, tool, or other extra charges.

### One shared Go subscription

All proxy users use the operator's one Go key. The `opencode-go` object in
`keys.json` selects the plan. Its published monthly price is fixed in code:
$10 for Go or $40 for Go Plus, with no configurable price override.
The separate `go_allowances` table in `providers/opencode_go.hpp` supplies each model's
published monthly allowance for the selected plan. Go Plus allowances are not
a uniform multiple of Go allowances.

```text
request cost = token cost × published monthly plan price
               ÷ model's monthly included usage allowance
```

For Go DeepSeek V4 Flash, $0.30 of nominal usage against its $30 monthly allowance
allocates $0.10 of the $10 subscription to the user. The same $0.30 of usage
through direct DeepSeek counts as $0.30. Free Go models cost zero.
Go DeepSeek rows use Go's documented UTC-weekday peak schedule, independently
of direct DeepSeek's Chinese holiday calendar. Other rows use flat or
context-tiered prices from [Go's published plan tables](https://opencode.ai/v2/docs/console/go).

This is a stable operator allocation policy, not an additional invoice charge
or an exact replica of Go's mixed-model allowance enforcement. Unused allowance
leaves some subscription expense unallocated. Do not add these allocated costs
to the subscription purchase when computing actual cash expenses.
Disable Go's **Use balance** setting for this policy: automatic paid Zen fallback
is not distinguished in usage reports and would invalidate subscription-only accounting.
Changing the configured plan requires a restart and affects new requests only;
previously stored request costs are not recalculated.

### Usage accounting

- Chat Completions supports DeepSeek's explicit cache buckets and OpenAI's nested
  `prompt_tokens_details.cached_tokens`; uncached input derives from total input.
  Conflicting counts are rejected. Reasoning tokens are already included in output.
  Nested `prompt_tokens_details.cache_write_tokens` counts are charged at the
  model's cache-write rate, replacing ordinary input pricing for those tokens.
- Messages input, cache reads, and cache writes are priced separately. Streaming
  combines initial `message_start.message.usage` with later `message_delta.usage`;
  supplied fields replace earlier counts, omitted fields remain, and cumulative
  counts are never added together.
- Responses uses `input_tokens`, `input_tokens_details.cached_tokens`,
  `input_tokens_details.cache_write_tokens`, and `output_tokens`. Reads and writes
  are subsets of total input: writes use their own rate, not an additive charge.
  Reasoning tokens are already included in output and are not added again. JSON uses top-level
  `usage`; SSE reads `response.usage` from `response.completed`,
  `response.incomplete`, and `response.failed`, including unnamed SSE events with
  a JSON `type`. Initial null usage and output-item events do not overwrite the
  request estimate. See [Responses streaming events](https://developers.openai.com/api/reference/resources/responses/streaming-events).
  See [OpenAI prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching)
  for cache-write accounting on GPT-5.6 and later models. Invalid counts or cache
  reads plus writes exceeding total input are rejected for accounting.
- `cost::calculate()` returns one request cost. The proxy passes it to the existing
  `database::update_request()` function, which stores it in `cost_nanodollars`.
  The database schema and stored costs are unchanged; no migration or additional
  accounting columns are required.
- Unpriceable usage logs a warning without replacing an earlier estimate. Usage
  parsers return `cost::UsageResult`, carrying the offending field and rejection
  reason on failure. Warnings identify the user, request ID, provider, model,
  protocol, and JSON/SSE event source, followed by `field`, `reason`, and a compact
  JSON `usage` snapshot. Snapshots contain only whitelisted numeric token counters
  and nested cache details; other values are represented by their types. Headers,
  credentials, prompts, response text, and arbitrary provider fields are not logged.
  Missing usage fields default to zero. Optional Chat Completions/Responses
  `cache_write_tokens: null` likewise means no reported cache-write count; cached
  reads are still accounted for. Other null counters and malformed or inconsistent
  counts remain rejected. Incomplete reports can underestimate costs, and unknown
  costs do not contribute to budget totals.

## Known limitations

- Request state tracks HTTP transport completion, not model outcome. Messages
  `error`/`message_stop` and Responses terminal event types do not determine the
  stored state. A failed model response or a stream that closes before its
  terminal event can be marked completed if the HTTP fetch succeeds.
- Background Responses can return before usage is available; later polling is
  not proxied or accounted for. Tool fees and nonstandard service-tier pricing
  are not included in the text-token estimate.
- Proxy-generated errors are plain text, not provider-native JSON error objects.
- Missing usage or an unavailable pricing schedule can leave a dispatched request
  without a recorded cost. The multiplier admission check does not eliminate these
  separate post-dispatch accounting failures.
- Tests cover local HTTP upstream fixtures and accounting, not live provider
  compatibility or exact billing reconciliation.

## Tests

Run the Python tests from the repository root after building the server:

```sh
python3 tests/cli.py ./llm-budget
python3 tests/usage_page.py ./llm-budget  # port 8787 must be free
python3 tests/proxy_limits.py ./llm-budget  # port 8787 must be free
python3 tests/proxy_callbacks.py          # GNU linker, g++; uses built obj/*.o
python3 tests/usage_graph.py
```

These tests use temporary directories and dummy keys. None contact external providers.

`proxy_callbacks.py` links the built server objects with a test-only transport
destination redirect, then exercises the real proxy against a local HTTP fixture.
It checks provider-qualified routing, upstream authentication, model rewriting,
fragmented JSON/SSE usage (including Responses terminal snapshots), Go cost
allocation, unsupported protocols, unavailable providers/pricing, valid zero
multipliers, and admission limits using stored rather than nominal Go costs.

`proxy_limits.py` exercises all three proxy routes against the real local server,
checking exhausted five-hour, weekly, and combined budgets, reset-based
`Retry-After`, and disabled (`403`) budgets without contacting the upstream.

`usage_graph.py` checks UTC daily buckets, the rolling cutoff, future-row
exclusion, shorter history, missing costs, zero-spending days/users, and escaped
SVG labels using a temporary SQLite database.

The three C++ tests are standalone `main()` programs, built with the same include
paths as the server:

```sh
g++ -std=c++23 -O2 -I. tests/cost.cpp SJSON/src/value.cpp Polyweb/string.cpp -o test_cost
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
