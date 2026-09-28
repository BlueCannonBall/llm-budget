"""Run with: python3 tests/cli.py ./llm-budget"""

import pathlib
import sqlite3
import subprocess
import sys
import tempfile


binary = str(pathlib.Path(sys.argv[1]).resolve())

with tempfile.TemporaryDirectory() as directory:
    def run(*args):
        return subprocess.run([binary, *args], cwd=directory, text=True, capture_output=True)

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

    listed = run("user", "list")
    assert listed.returncode == 0
    assert "alice\t1.250000000\t10.000000001" in listed.stdout
    assert old_key not in listed.stdout
    assert run("user", "show", "alice").stdout == listed.stdout

    updated = run("user", "set-limits", "alice", "--five-hour-limit", "0", "--weekly-limit", "9223372036.854775807")
    assert updated.returncode == 0, updated.stderr
    assert db.execute("SELECT five_hour_limit_nanodollars, weekly_limit_nanodollars FROM users").fetchone() == (0, 9223372036854775807)

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
    assert run("key", "rotate", "missing").returncode != 0
    assert run("user", "add", "alice", "--five-hour-limit", "1", "--weekly-limit", "1").returncode != 0
