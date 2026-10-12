# Daily provider catalog check

You are an unattended maintenance agent for the `llm-budget` proxy. Once a day
you verify that the hardcoded provider price and model catalogs still match each
provider's published documentation. **Only when a value actually differs** do
you update the catalogs, then push to GitHub and deploy. Never invent a change
to look busy; a no-change run is the expected outcome most days.

Run this from the repository root (the directory containing this file). The
scheduler entry is `catalog-update-daily.sh`; it pipes this document to
`codex exec` and appends the transcript to
`~/.local/state/llm-budget/catalog-update.log`.

## Sources of truth

Fetch each page fresh and read the current values; do not rely on memory of a
previous run.

- OpenCode Go pricing and per-plan allowances: <https://opencode.ai/v2/docs/console/go>
- DeepSeek pricing: <https://api-docs.deepseek.com/quick_start/pricing/>
- DeepSeek Responses compatibility: <https://api-docs.deepseek.com/guides/responses_api>
- OpenAI pricing: <https://developers.openai.com/api/docs/pricing>
- OpenAI per-model endpoint support: <https://developers.openai.com/api/docs/models>

Catalogs and prose to compare against:

- `providers/deepseek.hpp` — `DeepSeekProvider::model_catalog`
- `providers/openai.hpp` — `OpenAIProvider::model_catalog`
- `providers/opencode_go.hpp` — `OpenCodeGoProvider::model_catalog` and `go_allowances`
- `README.md` — the provider bullets, the model counts ("31 documented models",
  "35 text/coding models"), and the OpenAI family table

Rate fields are integer picodollars per token: `150'000` means
$0.15 per million tokens. `input_token_threshold` plus `above_threshold_rates`
encode a whole-request price step. `schedule = SCHEDULE_*` encodes a time-based
multiplier. Go `go_allowances[]` rows carry `.go_monthly_usd` and
`.go_plus_monthly_usd` for the two plans.

## Hard rules

- Never read, print, or copy `keys.json`, any path matching `*secret*` or
  `*.env`, or `~/start_services.sh` on any host. They contain credentials.
- Never rewrite pushed history: no `git commit --amend`, `git rebase`,
  `git reset`, or `git push --force`. Fix a mistake with a new commit.
- Stage changes by explicit path only. Never run `git add -A`, `git add .`,
  `git stash`, or `git clean`.
- Do not modify `DEPLOY.md`, `CATALOG_UPDATE.md`, `catalog-update-daily.sh`,
  `catalog-check.py`, or
  the `Polyweb/`, `SJSON/`, `spdlog/` submodules.
- Treat a missing page, a non-2xx response, or a value you cannot read clearly
  as "could not verify" — never as "changed". Leave the catalog alone and say so.
- When in doubt, make no edit.

## Step 1 — Preconditions

1. Run `git status --porcelain`. Untracked files are fine. If any **tracked**
   file is modified, stop and report; do not build on a dirty tree.
2. Run `git fetch origin` and confirm the current branch is `main`. If the local
   branch is behind `origin/main`, run `git pull --ff-only origin main`. If the
   branches have diverged, stop and report — do not merge or rebase.

## Step 2 — Compare each provider

Run `python3 catalog-check.py` first. It fetches public documentation and
deterministically compares direct DeepSeek rates and Go model/tier rows, token
rates, and **both** plan allowance columns. Exit 1 means differences to fix;
exit 2 means could not verify: stop without edits, commits, or deployment.
The scheduler also runs it before and after the agent and propagates failures.
Never override a reported difference with a prose claim that the catalogs match.
This check does not cover OpenAI, protocols, README, or the schedule implementation;
those still need the comparisons below.

For every source above, fetch the page (for example
`curl -sSL <url>`), then compare field by field: model IDs added or removed,
`input`, `cached_read`, `output`, `cached_write`, input-token thresholds and
above-threshold rates, schedules, permitted protocols, and — for Go — both plan
allowances. Also check the README counts and family table.

Write a short per-provider finding: which URL you read, the values you saw, and
whether each matches the catalog.

## Step 3 — Nothing changed

If every value matches, print your findings and **stop**. Do not commit, push,
deploy, or bump the `Checked <date>` comments. (The dates only move when a rate
or model actually changes, so that a quiet day produces no commit.)

## Step 4 — Something changed

1. Edit only the affected rows and the matching
   `// ... Checked <Month D, YYYY>.` comment, using the exact published values
   and keeping the surrounding formatting and quoting style.
2. Update the README counts and tables if the model set changed.
3. Build: `make -j2` (`.polybuild.mk` is committed; no regeneration needed).
4. Run `python3 tests/cli.py ./llm-budget`. If you changed pricing math rather
   than just catalog data, also run the standalone cost test:
    `g++ -std=c++23 -O2 -I. tests/cost.cpp SJSON/src/value.cpp Polyweb/string.cpp -o /tmp/opencode/test_cost && /tmp/opencode/test_cost`
   Run `python3 catalog-check.py` again before committing. Only exit 0 permits
   a commit, push, or deployment; unresolved differences and unverifiable sources
   are failures, not a successful no-change run.
5. If the build or tests fail, revert your edits (`git checkout -- <paths>`) and
   stop. Do not push a tree that does not build or pass its tests.
6. Commit by explicit path, with a specific imperative message in the repo's
   existing style, for example:
   `git add providers/opencode_go.hpp README.md && git commit -m 'Raise Grok 4.7 output rate in the Go catalog'`
7. `git push origin main`.
8. Deploy by following `DEPLOY.md` exactly. It rebuilds and tests on the remote
   host before restarting the server, and verifies TLS without a paid provider
   request. Stop and report if the remote checkout is dirty or verification
   fails; never reset the remote checkout.

## Step 5 — Report

End with a concise report: the URLs you checked, what changed (or that nothing
did), and, if you deployed, the commit hash plus message and the deploy result.
The wrapper appends this to the log named above.
