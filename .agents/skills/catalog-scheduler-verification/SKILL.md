---
name: catalog-scheduler-verification
description: Verify changes to the daily catalog scheduler's completion reporting and deployment retry behavior without launching a model, accessing credentials, or contacting the server.
---

# Catalog scheduler verification

## Prerequisites

Run from the llm-budget repository with Python 3 and Bash available. Preserve
existing catalog edits. Never invoke the real daily wrapper just to test it:
it launches a model and can commit, push, and deploy.

## Workflow

1. Read `catalog-update-daily.sh`, `CATALOG_UPDATE.md`, and the wrapper tests.
2. Keep local verification and commit/push before all remote prechecks.
3. Require an explicit per-run completion result, initialized to blocked under
   the scheduler lock. A zero agent process exit is not evidence of completion.
4. Record a pending deployment after push and before any remote command. Keep
   it across failure and timeout. Retry on no-change days; remove it only after
   deployment verification. Bound SSH connection and total command duration.
5. Run `python3 tests/catalog_wrapper.py`, `python3 tests/catalog_check.py`,
   `bash -n catalog-update-daily.sh`, and `git diff --check`.

## Verification and pitfalls

The offline tests use isolated temporary directories and stub agents/checkers.
They verify explicit success, blocked/missing results, stale prior success,
pending deployment, no-change retry, and agent exit propagation. These scenarios
were verified locally; actual agent compliance and remote deployment are not
covered by the stubs. Keep model instructions consistent with the wrapper's
result-file contract. Checker failures remain advisory to allow documented
manual source verification; they must not bypass the completion contract.

`DEPLOY.md` is ignored in this checkout. Keep essential unattended timeout and
ordering requirements in tracked `CATALOG_UPDATE.md` as well as that local guide.
