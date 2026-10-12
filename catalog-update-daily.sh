#!/usr/bin/env bash
# Daily check that llm-budget's provider price/model catalogs match the
# providers' published documentation. The codex agent follows CATALOG_UPDATE.md;
# deterministic DeepSeek/Go checks prevent a false no-change report.
#
# Cron entry (system time is America/New_York):
#   0 3 * * * /home/optimal/Documents/llm-budget/catalog-update-daily.sh
#
# Test the plumbing without spending tokens:
#   CODEX_BIN=/path/to/a/stub ./catalog-update-daily.sh

set -u -o pipefail

export HOME=/home/optimal
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin

repo=/home/optimal/Documents/llm-budget
runbook=$repo/CATALOG_UPDATE.md
state_dir=$HOME/.local/state/llm-budget
log=$state_dir/catalog-update.log
lock=$state_dir/catalog-update.lock
codex_bin=${CODEX_BIN:-codex}
max_log_bytes=$((5 * 1024 * 1024))

mkdir -p "$state_dir" || exit 1

exec 9>"$lock" || exit 1
if ! flock -n 9; then
    printf '%s skipped: previous run still active\n' "$(date -Is)" >>"$log"
    exit 0
fi

# Rotate under the lock so concurrent invocations cannot move the active log.
if [ -s "$log" ] && [ "$(stat -c %s "$log")" -gt "$max_log_bytes" ]; then
    mv -f "$log" "$log.1" || exit 1
fi

run_agent() {
    cd "$repo" || { echo "missing repository: $repo"; return 1; }
    python3 "$repo/catalog-check.py"
    check_status=$?
    # Differences are work for the agent; unreadable sources are not.
    if [ "$check_status" -gt 1 ]; then
        return "$check_status"
    fi
    timeout 3600 "$codex_bin" exec \
        --cd "$repo" \
        --dangerously-bypass-approvals-and-sandbox \
        --skip-git-repo-check \
        - <"$runbook"
    agent_status=$?
    if [ "$agent_status" -ne 0 ]; then
        return "$agent_status"
    fi
    # An agent's prose is not evidence that the numeric catalog matches.
    python3 "$repo/catalog-check.py"
}

{
    printf '\n===== daily catalog check: start %s =====\n' "$(date -Is)"
    run_agent
    status=$?
    printf '===== daily catalog check: exit %s at %s =====\n' "$status" "$(date -Is)"
} >>"$log" 2>&1
exit "$status"
