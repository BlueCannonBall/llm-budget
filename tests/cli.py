"""Run with: python3 tests/cli.py ./llm-budget"""

import os
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta, timezone


binary = str(pathlib.Path(sys.argv[1]).resolve())

with tempfile.TemporaryDirectory() as directory:
    def run(*args, timezone_name=None):
        env = None if timezone_name is None else {**os.environ, "TZ": timezone_name}
        return subprocess.run([binary, *args], cwd=directory, text=True, capture_output=True, env=env)

    assert run("--help").returncode == 0
    database = pathlib.Path(directory, "llm-budget.db")
    assert not database.exists()
    assert run("user", "add", "alice", "--five-hour-limit", "1").returncode != 0
    assert not database.exists()

    added = run("user", "add", "alice", "--five-hour-limit", "1.25", "--weekly-limit", "10.000000001")
    assert added.returncode == 0, added.stderr
    old_key = added.stdout.split("API key: ")[1].strip()
    assert len(old_key) == 64

    db = sqlite3.connect(database)
    assert db.execute("SELECT five_hour_limit_nanodollars, weekly_limit_nanodollars FROM users").fetchone() == (1250000000, 10000000001)

    empty_usage = run("user", "usage", "alice")
    assert empty_usage.returncode == 0, empty_usage.stderr
    assert "Five-hour: $0.000000000 / $1.250000000 (0.00%)" in empty_usage.stdout
    assert "Weekly: $0.000000000 / $10.000000001 (0.00%)" in empty_usage.stdout
    assert empty_usage.stdout.count("reset not scheduled") == 2

    now_ms = int(db.execute("SELECT unixepoch() * 1000").fetchone()[0])
    db.execute("UPDATE users SET five_hour_window_started_at = ?, weekly_window_started_at = ? WHERE name = 'alice'", (now_ms, now_ms))
    db.execute("INSERT INTO requests (user_id, started_at, state, cost_nanodollars) VALUES (1, ?, 'completed', 625000000)", (now_ms,))
    db.execute("INSERT INTO requests (user_id, started_at, state) VALUES (1, ?, 'in_flight')", (now_ms,))
    db.commit()
    current_usage = run("user", "usage", "alice", timezone_name="EST5")
    assert current_usage.returncode == 0, current_usage.stderr
    assert "Five-hour: $0.625000000 / $1.250000000 (50.00%)" in current_usage.stdout
    assert "Weekly: $0.625000000 / $10.000000001 (6.25%)" in current_usage.stdout
    eastern = timezone(timedelta(hours=-5))
    five_hour_reset = datetime.fromtimestamp((now_ms + 18_000_000) / 1000, eastern).strftime("%Y-%m-%d %H:%M:%S.000")
    weekly_reset = datetime.fromtimestamp((now_ms + 604_800_000) / 1000, eastern).strftime("%Y-%m-%d %H:%M:%S.000")
    assert f"resets at {five_hour_reset} EST -0500" in current_usage.stdout
    assert f"resets at {weekly_reset} EST -0500" in current_usage.stdout

    db.execute("UPDATE users SET five_hour_window_started_at = ? WHERE name = 'alice'", (now_ms - 18_000_001,))
    db.commit()
    expired_usage = run("user", "usage", "alice")
    assert expired_usage.returncode == 0, expired_usage.stderr
    assert "Five-hour: $0.000000000 / $1.250000000 (0.00%)" in expired_usage.stdout
    assert "Weekly: $0.625000000 / $10.000000001 (6.25%)" in expired_usage.stdout
    assert "Five-hour: $0.000000000 / $1.250000000 (0.00%); reset not scheduled" in expired_usage.stdout

    listed = run("user", "list")
    assert listed.returncode == 0
    assert "alice\t1.250000000\t10.000000001" in listed.stdout
    assert old_key not in listed.stdout
    assert run("user", "show", "alice").stdout == listed.stdout

    updated = run("user", "set-limits", "alice", "--five-hour-limit", "0", "--weekly-limit", "9223372036.854775807")
    assert updated.returncode == 0, updated.stderr
    assert db.execute("SELECT five_hour_limit_nanodollars, weekly_limit_nanodollars FROM users").fetchone() == (0, 9223372036854775807)
    assert "Five-hour: $0.000000000 / $0.000000000 (n/a: zero limit); no automatic reset (zero limit)" in run("user", "usage", "alice").stdout

    old_hash = db.execute("SELECT api_key_hash FROM users").fetchone()[0]
    rotated = run("key", "rotate", "alice")
    assert rotated.returncode == 0, rotated.stderr
    assert rotated.stdout.startswith("API key: ") and rotated.stdout.strip() != "API key: " + old_key
    assert db.execute("SELECT api_key_hash FROM users").fetchone()[0] != old_hash

    for bad_limit in ("-1", "1.0000000001", "9223372036.854775808", "1e5", "1.", "9223372037"):
        result = run("user", "set-limits", "alice", "--five-hour-limit", bad_limit, "--weekly-limit", "1")
        assert result.returncode != 0, bad_limit

    assert db.execute("SELECT five_hour_limit_nanodollars FROM users").fetchone()[0] == 0
    reordered = run("user", "set-limits", "alice", "--weekly-limit", "3", "--five-hour-limit", "2")
    assert reordered.returncode == 0, reordered.stderr
    assert db.execute("SELECT five_hour_limit_nanodollars, weekly_limit_nanodollars FROM users").fetchone() == (2000000000, 3000000000)
    assert run("user", "show", "missing").returncode != 0
    assert run("user", "usage", "missing").returncode != 0
    assert run("key", "rotate", "missing").returncode != 0
    assert run("user", "add", "alice", "--five-hour-limit", "1", "--weekly-limit", "1").returncode != 0
