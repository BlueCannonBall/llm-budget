# LLM Budget

An LLM API proxy and budget limiter for DeepSeek, OpenCode Go, and selected OpenAI
models. It supports OpenAI Chat Completions, OpenAI Responses, Anthropic Messages,
and Google Gemini formats, authenticates callers with per-user API keys, and rejects
new requests when a user's five-hour or weekly dollar budget is exhausted. Direct
API requests count estimated token cost; Go requests count an allocated share of one
shared subscription.

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

Code, documentation, and asset style rules are in `AGENTS.md`.

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

  For `opencode-go`, `plan` must be `"go"` or `"go-plus"`. Direct-provider keys
  remain strings. The object contains only `api_key` and `plan`; invalid Go
  configuration stops startup.

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

All four routes forward JSON to the provider selected by the request's model:

| Local route | User authentication | Available providers |
| --- | --- | --- |
| `POST /chat/completions` | `Authorization: Bearer <user API key>` | DeepSeek, all cataloged Go models, OpenAI Chat-capable models |
| `POST /v1/messages` | `x-api-key: <user API key>` or Bearer authorization | DeepSeek, all cataloged Go models |
| `POST /responses` | `Authorization: Bearer <user API key>` | DeepSeek, all cataloged Go models, OpenAI models |
| `POST /v1beta/models/{model}:generateContent` and `:streamGenerateContent` | `x-goog-api-key` or `key` query parameter | all cataloged Go models |

Provider-qualified names pin routing without ambiguity:

```text
deepseek/deepseek-flash
opencode-go/deepseek-v4.1-flash
opencode-go/minimax-m3
opencode-go/gpt-5.6-luna
opencode-go/gemini-4-argon
openai/gpt-4.1-mini
```

The proxy replaces the outbound `model` with the upstream model ID. On the Gemini
route the model is in the path instead, and the proxy replaces it there.
Unqualified `deepseek-flash` and `deepseek-v4-pro` keep their direct DeepSeek
defaults; other models require qualification. There is no automatic fallback to
another provider.

Each concrete provider owns its routing and pricing catalog in `providers/`, and
those headers hold the only copy: `deepseek.hpp`, `openai.hpp`, and `opencode_go.hpp`
define the model IDs, rates, token tiers, schedules, permitted protocols, and Go
allowances. This README does not repeat them. The comment above each catalog records
the published sources and the date the rows were last checked.

Routing behavior that the catalogs do not express:

- Every cataloged Go model is permitted through all four proxy routes. Go determines
  protocol compatibility; upstream rejections are forwarded unchanged, without
  protocol translation. Permitted routing does not guarantee upstream support.
- The OpenAI catalog is limited to text and coding models. Audio, image generation,
  embedding, research/tool-fee, and restricted cyber models, dated snapshots, and
  moving ChatGPT aliases are intentionally excluded.

Use `http://127.0.0.1:8787` as the SDK base URL for all four formats. Do not append
`/v1` or `/anthropic`; the SDK supplies the route path. For example:

```sh
curl http://127.0.0.1:8787/responses \
    -H "Authorization: Bearer $LLM_BUDGET_API_KEY" \
    -H 'Content-Type: application/json' \
    -d '{"model":"opencode-go/gpt-5.6-luna","input":"Explain this code","stream":true}'
```

Responses request fields are forwarded without protocol conversion, subject to
the shared accounting restrictions below. The provider-qualified model
name is replaced. Both JSON and SSE responses are
passed through. This endpoint implements response creation, not retrieval,
deletion, cancellation, compaction, or WebSocket transport.

The Gemini route accepts `generateContent` and `streamGenerateContent`. The SDK
builds `/v1beta/models/<model>:<method>` from its configured model ID, so use
`opencode-go/gemini-4-argon` (or another provider-qualified Go model) as that ID;
the proxy resolves the qualification and forwards the bare upstream name. Query
parameters such as `alt=sse` are preserved. `streamGenerateContent` works with
SSE and with its default JSON array. Only Go models are available on this route.

```sh
curl 'http://127.0.0.1:8787/v1beta/models/opencode-go%2Fgemini-4-argon:generateContent' \
    -H "x-goog-api-key: $LLM_BUDGET_API_KEY" \
    -H 'Content-Type: application/json' \
    -d '{"contents":[{"role":"user","parts":[{"text":"Explain this code"}]}]}'
```

- Missing, malformed, or unrecognized user credentials get `401`. On the
  Messages route, `Authorization` takes precedence if both headers are supplied.
  On the Gemini route, `x-goog-api-key` takes precedence over a `key` query
  parameter. The query key is removed before forwarding. Prefer the header:
  credentials in URLs are more likely to be recorded by reverse proxies or shell
  history.
- The body must be JSON. Chat Completions, Messages, and Responses require a
  `model` string in the body; Gemini takes the model from the path. Unknown models
  and locally restricted protocol/model combinations get `400`. Go models have no
  local protocol restrictions; unsupported combinations return Go's upstream error.
  Methods other than `POST` get `405`; an unrecognized Gemini path or generate
  method gets `404`.
- Only configure keys for providers you use. A missing, empty, or non-string
  key for a selected provider gets `503` before request admission.
- A model whose provider cannot resolve its cost multiplier gets `503` before
  scheduling upstream work or creating a request row. A resolved zero multiplier
  is valid and records zero cost; normal user budget admission still applies.
- Caller credentials are replaced with the selected provider key from
  `keys.json`: Bearer authorization for Chat Completions and Responses, `x-api-key`
  for Messages, and `x-goog-api-key` for Gemini.
- Go requests preserve the caller's `User-Agent`, falling back to `llm-budget/1.0`
  when it is absent or empty. `x-opencode-session`, Claude Code's
  `X-Claude-Code-Session-Id`, and Codex's `session-id` and `thread-id` are forwarded
  unchanged when present; they are not translated into one another. See
  [Go's client guidance](https://opencode.ai/v2/docs/console/go#where-can-i-use-it),
  [Claude Code's request headers](https://code.claude.com/docs/en/llm-gateway#request-headers),
  and [Codex's session headers](https://github.com/openai/codex/pull/22193).
- For direct DeepSeek upstream user isolation, Chat Completions sets `user_id`,
  Responses sets `user`, and Messages sets `metadata.user_id` to the authenticated
  user's name, overriding caller-supplied values. Other Messages metadata is
  preserved; missing or null metadata is created, and non-object metadata gets
  `400`. Names are forwarded unchanged; the CLI does not validate upstream user-ID
  requirements.
- Messages forwards caller `anthropic-version` and `anthropic-beta` headers
  unchanged. When the version header is absent, it defaults to `2023-06-01`.
  The upstream decides whether supplied versions and beta flags are supported.
- Chat Completions and Responses request standard processing for every provider:
  the proxy explicitly sets `service_tier` to `"default"` to avoid inheriting
  project-level Fast defaults. Caller-supplied tiers other than `"default"`
  (including `"auto"`, Fast/Priority, Ultrafast, and Flex) get `400` before
  admission or upstream dispatch.
- All providers permit only client-executed tools: `function` and `custom` tools
  on Chat Completions and Responses; Messages tools with a `name`, `input_schema`,
  and either no `type` or `type: "custom"`; Gemini `functionDeclarations` with a
  string `name`. Hosted tools, including image generation, web/file search, and
  hosted execution, get `400` because their additional charges are not accounted
  for. This also applies to tool definitions in Responses `additional_tools` input
  items. Chat Completions `web_search_options` and non-empty `mcp_servers` are also
  rejected. Gemini `googleSearch`, `codeExecution`, `urlContext`, `googleMaps`,
  `fileSearch`, and unknown tool fields are rejected for the same reason. Unknown
  tool types are rejected by default, including on Go and DeepSeek. Ordinary image
  inputs remain permitted. Rejections happen before budget admission or upstream
  dispatch.
- Gemini rejects explicit `cachedContent` references with `400`, because cache
  storage charges are not accounted for. It also permits only `TEXT` in
  `generationConfig.responseModalities`; image and audio output prices are not
  accounted for. Both checks run before admission or upstream dispatch.
- Messages normalizes existing ephemeral `cache_control` markers to `ttl: "5m"`,
  including automatic top-level caching, system blocks, tool definitions, and
  message content (including nested tool-result content). It does not add cache
  markers or modify function arguments.
- Responses normalizes existing `prompt_cache_options` and message content-block
  `prompt_cache_breakpoint` objects to `ttl: "30m"` for every provider, matching
  the catalog's cache-write rates. It preserves caching modes and other options,
  does not add caching options or breakpoints, and leaves older models'
  `prompt_cache_retention` unchanged. Caller-requested 12-hour retention is
  overridden.
- Responses rejects `background: true` with `400` for every provider before
  admission or upstream dispatch: later usage cannot be captured by this proxy.
  Omitted or false `background` is permitted; non-boolean values get `400`.
- The upstream response is streamed back as received, with `Content-Type` and
  `Retry-After` passed through. Responses are capped at 32,000,000 bytes.
- All upstream fetches use a 120-second socket read timeout and a 30-second
  send timeout. The read timeout bounds inactivity, not total stream duration.
  A fetch failure returns `502` if response headers have not been sent; otherwise
  it closes the stream. The request is recorded as interrupted.
- While the response streams, token usage is read from the SSE events (or a plain
  JSON body) and priced. Gemini usage comes from cumulative `usageMetadata`
  objects in SSE events or JSON array elements. The estimate is logged and stored
  on the request row.
- Streaming Chat Completions always sets `stream_options.include_usage` to
  `true`, overriding caller opt-outs and preserving other options. Missing or
  null options are created; non-object options get `400`. An upstream response
  without usage still cannot be priced.

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
and the page is marked `no-store`. Scheduled resets also show a countdown with
JavaScript enabled, updated once per minute. **Reset due** indicates that the
deadline has passed; **Refresh usage** reloads the recorded usage.

The page uses a compact layout with light/dark colors following the browser's
preference. After a successful lookup, the API key form collapses into **Key
settings**, leaving usage and the **Refresh usage** action visible. Expand the
settings to change or forget a key. Percentages remain accurate above 100%;
the accompanying progress bars stop at full.

Page markup, styles, and client behavior live in `web/usage.html`,
`web/usage.css`, and `web/usage.js`; `usage_page.hpp` handles server rendering and
form authentication. Assets are ordinary runtime files, cached at startup. No CDN,
compiler extensions, generated asset headers, or JavaScript build step are needed.
Asset edits require a server restart, but not a C++ rebuild.

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
but remembering is unavailable. Other methods return `405` with `Allow: GET, POST`.

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
`user list` and `user show` print the ID, name, five-hour limit, and weekly limit, but
never the key. `user usage` prints recorded budget debits in dollars and as a
percentage of each limit, with active window reset times in the machine's local
timezone. A missing user is an error.

## Usage graph

Generate total and day-by-day per-user spending charts with Python 3 (standard
library only):

```sh
python3 usage_graph.py
# Optional database and output paths:
python3 usage_graph.py --db /path/to/llm-budget.db --output spending.svg
```

Open the generated `usage.svg` in a browser or image viewer. Chart layout, date
handling, cost attribution, and the missing-cost rules are documented in the
header comment of `usage_graph.py`.

The CLI, usage page, and graph aggregate request costs: direct token costs plus
allocated Go subscription costs. These totals are not actual provider invoices.

## Cost estimation

`provider_types.hpp` defines the shared `ProviderDefinition`, `Model`, and `Rates`
records, the permitted proxy protocol flags, and the pricing schedules. Model
records use local upstream names and named fields for rates, optional input-token
tiers, and schedules. They contain neither registry indexes nor repeated provider
prefixes.

Each concrete provider in `providers/` owns its metadata and static model catalog.
`DeepSeekProvider`, `OpenAIProvider`, and `OpenCodeGoProvider` inherit directly
from the `Provider` interface in `provider.hpp`. Credentials and ordinary
authentication are shared; configuration and request behavior specific to a
provider live in its concrete implementation. Only `OpenCodeGoProvider` owns Go
subscription state and the separate plan allowance table.

`provider_registry.hpp` builds the provider set with `configure` and resolves names
with `resolve`. Lookup uses provider names, not positions in a registry or an
aligned configuration array. Known models remain resolvable when credentials are
absent. Locally restricted protocols still get `400`; unavailable providers get
`503`.

Adding a model with an existing pricing scheme requires a named catalog row in its
provider, not a new pricing function. Go models also need allowances for both Go
and Go Plus in `providers/opencode_go.hpp`. Go catalog rows use
`permitted_protocols` to delegate compatibility decisions to Go. Other providers
use `PROTOCOL_*` flags for permitted routes. Use `SCHEDULE_*` constants for
schedules.

`cost.hpp` normalizes API usage and calculates token costs, applying the provider's
resolved exact multiplier before rounding. It has no subscription plan or allowance
logic. All routes require a resolved multiplier before dispatch. Accounting reads
static model/provider metadata and that multiplier value; it does not retain
configured-provider objects or make virtual calls for streamed usage events.

`input_token_threshold` selects `above_threshold_rates` when the total input
token count exceeds the threshold, including cache reads and cache writes but
excluding output. The selected rates apply to the whole request, not only the
tokens above the threshold.

Rates use integer picodollars per token to represent fractional nanodollar rates.
Checked 128-bit intermediates retain precision. The final request cost is rounded
up once to integer nanodollars (1,000,000,000 = $1).

### Direct pricing

Direct providers count the calculated token cost without a subscription
multiplier. DeepSeek uses the `SCHEDULE_DEEPSEEK_2026` peak schedule: rates double
during 01:00–04:00 and 06:00–10:00 UTC on Beijing-time weekdays, excluding Chinese
public holidays. Request start time selects pricing. Make-up working weekends
remain off-peak. Only the 2026 holiday calendar is implemented; direct DeepSeek
requests in other years cannot be priced. The calculator uses the current catalog
rates, with no historical rate lookup or pricing-start cutoff. OpenAI rows use
standard text-token rates, not batch, priority, tool, or other extra charges.

### One shared Go subscription

All proxy users use the operator's one Go key. The `opencode-go` object in
`keys.json` selects the plan. Its published monthly price is fixed in code
($10 for Go, $40 for Go Plus), with no configurable price override. The separate
`go_allowances` table in `providers/opencode_go.hpp` supplies each model's
published monthly allowance for the selected plan. Go Plus allowances are not
a uniform multiple of Go allowances.

```text
request cost = token cost × published monthly plan price
               ÷ model's monthly included usage allowance
```

For example, $0.30 of nominal usage against a $30 monthly allowance allocates
$0.10 of the $10 subscription to the user. The same $0.30 of usage through direct
DeepSeek counts as $0.30. Free Go models cost zero. Go DeepSeek rows use Go's
documented UTC-weekday peak schedule, independently of direct DeepSeek's Chinese
holiday calendar. Other rows use flat or context-tiered prices from
[Go's published plan tables](https://opencode.ai/v2/docs/console/go).

This is a stable operator allocation policy, not an additional invoice charge
or an exact replica of Go's mixed-model allowance enforcement. Unused allowance
leaves some subscription expense unallocated. Do not add these allocated costs
to the subscription purchase when computing actual cash expenses.
Disable Go's **Use balance** setting for this policy: automatic paid Zen fallback
is not distinguished in usage reports and would invalidate subscription-only
accounting. Changing the configured plan requires a restart and affects new
requests only; previously stored request costs are not recalculated.

### Usage accounting

Per-provider usage parsing is documented in
[`docs/accounting.md`](docs/accounting.md).

## Known limitations

- Request state tracks HTTP transport completion, not model outcome. Messages
  `error`/`message_stop` and Responses terminal event types do not determine the
  stored state. A failed model response or a stream that closes before its
  terminal event can be marked completed if the HTTP fetch succeeds.
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
python3 tests/proxy_callbacks.py --tools-only  # focused shared tool-policy checks
python3 tests/usage_graph.py
```

These tests use temporary directories and dummy keys. None contact external providers.

`proxy_callbacks.py` links the built server objects with a test-only transport
destination redirect, then exercises the real proxy against a local HTTP fixture.
It checks provider-qualified routing, upstream authentication, model rewriting,
fragmented JSON/SSE usage (including Responses terminal snapshots and Gemini
`usageMetadata` over SSE and JSON array streams), Go cost allocation, unsupported
protocols, unavailable providers/pricing, valid zero multipliers, and admission
limits using stored rather than nominal Go costs.
The `--tools-only` option checks hosted-tool rejection before admission and
client-tool forwarding across all providers and supported protocols, with JSON
and SSE responses, without running the pricing/accounting scenarios.

`proxy_limits.py` exercises the three body-model proxy routes against the real local
server, checking exhausted five-hour, weekly, and combined budgets, reset-based
`Retry-After`, and disabled (`403`) budgets without contacting the upstream.

`usage_graph.py` checks UTC calendar-day grouping, the rolling cutoff, future-row
exclusion, shorter history, missing costs, zero-spending days/users, and escaped
SVG labels using a temporary SQLite database.

The four C++ tests are standalone `main()` programs, built with the same include
paths as the server:

```sh
g++ -std=c++23 -O2 -I. tests/providers.cpp SJSON/src/value.cpp Polyweb/string.cpp -o test_providers
./test_providers
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

## License

GNU Affero General Public License, version 3. See [`LICENSE`](LICENSE).
