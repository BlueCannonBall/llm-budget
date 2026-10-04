"""Run with: python3 tests/usage_page.py ./llm-budget (port 8787 must be free)."""

from datetime import datetime, timezone
from html.parser import HTMLParser
import http.client
import pathlib
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.parse


class ResetTimes(HTMLParser):
    def __init__(self, body):
        super().__init__()
        self.values = []
        self.feed(body.decode())

    def handle_starttag(self, tag, attrs):
        if tag == "time":
            self.values.append(datetime.fromisoformat(dict(attrs)["datetime"]))


binary = str(pathlib.Path(sys.argv[1]).resolve())
with socket.socket() as available_port:
    available_port.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    available_port.bind(("127.0.0.1", 8787))

with tempfile.TemporaryDirectory() as directory:
    pathlib.Path(directory, "keys.json").write_text('{"deepseek":"dummy"}')
    name = "<script>alert(1)</script>"
    created = subprocess.run(
        [binary, "user", "add", name, "--five-hour-limit", "1.25", "--weekly-limit", "10"],
        cwd=directory, text=True, capture_output=True, check=True,
    )
    key = created.stdout.split("API key: ")[1].strip()

    db = sqlite3.connect(pathlib.Path(directory, "llm-budget.db"))
    started = int(time.time() * 1000) - 1000
    db.execute("UPDATE users SET five_hour_window_started_at=?, weekly_window_started_at=? WHERE id=1", (started, started))
    db.execute("INSERT INTO requests (user_id, started_at, state, cost_nanodollars) VALUES (1, ?, 'completed', 625000000)", (started,))
    db.commit()

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

        def request(method, body=None, content_type="application/x-www-form-urlencoded"):
            connection = http.client.HTTPConnection("127.0.0.1", 8787, timeout=5)
            connection.request(method, "/usage", body=body, headers={"Content-Type": content_type})
            response = connection.getresponse()
            result = response.status, dict(response.getheaders()), response.read()
            connection.close()
            return result

        status, headers, body = request("GET")
        assert status == 200 and b'type="password"' in body
        assert headers["Cache-Control"] == "no-store" and headers["Referrer-Policy"] == "no-referrer"
        assert "style-src https://cdn.jsdelivr.net" in headers["Content-Security-Policy"]
        status, headers, _ = request("PUT")
        assert status == 405
        assert {method.strip() for method in headers["Allow"].split(",")} == {"GET", "POST"}
        assert request("POST", "api_key=invalid")[0] == 401
        assert request("POST", "api_key=invalid", "text/plain")[0] == 400

        status, headers, body = request("POST", urllib.parse.urlencode({"api_key": key}))
        assert status == 200 and headers["Cache-Control"] == "no-store"
        assert b"50.00%" in body and b"6.25%" in body
        expected_reset_seconds = (started + 999) // 1000
        assert ResetTimes(body).values == [
            datetime.fromtimestamp(expected_reset_seconds + 5 * 3600, timezone.utc),
            datetime.fromtimestamp(expected_reset_seconds + 7 * 86400, timezone.utc),
        ]
        assert b"&#60;script&#62;alert&#40;1&#41;&#60;&#47;script&#62;" in body and b"<script>" not in body
        assert key.encode() not in body

        db.execute("UPDATE users SET five_hour_limit_nanodollars=0, five_hour_window_started_at=NULL, weekly_window_started_at=? WHERE id=1", (started - 604_800_001,))
        db.commit()
        status, _, body = request("POST", urllib.parse.urlencode({"api_key": key}))
        assert status == 200
        assert ResetTimes(body).values == []
        assert db.execute("SELECT COUNT(*) FROM requests").fetchone()[0] == 1
    finally:
        server.terminate()
        output, _ = server.communicate(timeout=5)
    assert key not in output
