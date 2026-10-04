"""Run with: python3 tests/proxy_limits.py ./llm-budget (port 8787 must be free)."""

import http.client
import json
import math
import pathlib
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time


binary = str(pathlib.Path(sys.argv[1]).resolve())
with socket.socket() as available_port:
    available_port.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    available_port.bind(("127.0.0.1", 8787))

with tempfile.TemporaryDirectory() as directory:
    pathlib.Path(directory, "keys.json").write_text('{"deepseek":"dummy"}')
    created = subprocess.run(
        [binary, "user", "add", "alice", "--five-hour-limit", "1", "--weekly-limit", "1"],
        cwd=directory, text=True, capture_output=True, check=True,
    )
    key = created.stdout.split("API key: ")[1].strip()
    db = sqlite3.connect(pathlib.Path(directory, "llm-budget.db"))
    server = subprocess.Popen([binary], cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        for _ in range(100):
            try:
                with socket.create_connection(("127.0.0.1", 8787), timeout=0.2):
                    break
            except OSError:
                time.sleep(0.02)
        else:
            raise AssertionError("server did not start")

        for route in ("/chat/completions", "/v1/messages"):
            for scenario in ("five", "weekly", "both", "both-five-later", "disabled"):
                now_ms = time.time_ns() // 1_000_000
                five_start = now_ms - 1000
                weekly_start = now_ms - 2000
                five_limit = 1_000_000_000
                weekly_limit = 1_000_000_000
                if scenario == "five":
                    weekly_limit = 3_000_000_000
                elif scenario == "weekly":
                    five_limit = 3_000_000_000
                elif scenario == "both-five-later":
                    weekly_start = now_ms - 7 * 86_400_000 + 60_000
                elif scenario == "disabled":
                    five_limit = 0
                db.execute("DELETE FROM requests")
                db.execute("UPDATE users SET five_hour_limit_nanodollars=?, weekly_limit_nanodollars=?, five_hour_window_started_at=?, weekly_window_started_at=?", (five_limit, weekly_limit, five_start, weekly_start))
                db.execute("INSERT INTO requests (user_id, started_at, state, cost_nanodollars) VALUES (1, ?, 'completed', 2000000000)", (now_ms,))
                db.commit()
                connection = http.client.HTTPConnection("127.0.0.1", 8787, timeout=5)
                headers = {"Content-Type": "application/json"}
                headers["Authorization" if route == "/chat/completions" else "x-api-key"] = "Bearer " + key if route == "/chat/completions" else key
                before_ms = time.time_ns() // 1_000_000
                connection.request("POST", route, json.dumps({"model": "deepseek-flash", "messages": []}), headers)
                response = connection.getresponse()
                body = response.read()
                after_ms = time.time_ns() // 1_000_000
                if scenario == "disabled":
                    assert response.status == 403, (route, scenario, response.status, body)
                    assert response.getheader("Retry-After") is None
                else:
                    assert response.status == 429, (route, scenario, response.status, body)
                    delay = response.getheader("Retry-After")
                    assert delay is not None and delay.isdigit(), delay
                    reset = five_start + 5 * 3_600_000 if scenario == "five" else weekly_start + 7 * 86_400_000
                    if scenario.startswith("both"):
                        reset = max(five_start + 5 * 3_600_000, weekly_start + 7 * 86_400_000)
                    assert max(1, math.ceil((reset - after_ms) / 1000)) <= int(delay) <= max(1, math.ceil((reset - before_ms) / 1000)), (scenario, delay, reset)
                connection.close()
                assert db.execute("SELECT COUNT(*) FROM requests").fetchone() == (1,)
    finally:
        server.terminate()
        server.communicate(timeout=5)
        db.close()

print("PASS: both routes reject exhausted budgets with 429 and reset-based Retry-After; disabled budgets remain 403")
