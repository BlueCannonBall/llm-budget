"""Run with: python3 tests/usage_graph.py"""

from datetime import datetime, timezone
import importlib.util
from pathlib import Path
import sqlite3
import sys
import tempfile
import xml.etree.ElementTree as ET


sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("graph", Path(__file__).resolve().parents[1] / "usage_graph.py")
graph = importlib.util.module_from_spec(spec)
spec.loader.exec_module(graph)

now = int(datetime(2026, 10, 3, 12, tzinfo=timezone.utc).timestamp() * 1000)
cutoff = now - 30 * graph.DAY_MS
midnight = now - graph.DAY_MS // 2
namespace = {"svg": "http://www.w3.org/2000/svg"}

with tempfile.TemporaryDirectory() as directory:
    database = Path(directory) / "usage.db"
    connection = sqlite3.connect(database)
    connection.executescript("""
        CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT);
        CREATE TABLE requests(id INTEGER PRIMARY KEY, user_id INTEGER,
                              started_at INTEGER, cost_nanodollars INTEGER);
    """)
    connection.executemany("INSERT INTO users VALUES (?, ?)", [(1, "Alice & <team>"), (2, "Bob"), (3, "Idle")])
    connection.executemany("INSERT INTO requests VALUES (?, ?, ?, ?)", [
        (1, 1, cutoff - 1, 99_000_000_000),  # Outside the rolling window.
        (2, 1, cutoff, 1),
        (3, 1, midnight - 1, 1_000_000_000),
        (4, 1, midnight, 2_000_000_000),
        (5, 1, now, 3_000_000_000),
        (6, 1, now + 1, 99_000_000_000),  # Future rows do not contribute.
        (7, 2, midnight, None),
        (8, 2, midnight - 1, 7_000_000_000),
    ])
    connection.commit()
    start, rows, daily = graph.read_usage(database, now)
    assert start == cutoff
    assert rows == [("Bob", 7_000_000_000, 1), ("Alice & <team>", 6_000_000_001, 0), ("Idle", 0, 0)]
    assert daily == {
        "Alice & <team>": {cutoff // graph.DAY_MS: (1, 0), midnight // graph.DAY_MS - 1: (1_000_000_000, 0), midnight // graph.DAY_MS: (5_000_000_000, 0)},
        "Bob": {midnight // graph.DAY_MS - 1: (7_000_000_000, 0), midnight // graph.DAY_MS: (0, 1)},
    }
    root = ET.fromstring(graph.render_svg(start, now, rows, daily))
    titles = [node.text for node in root.findall(".//svg:rect/svg:title", namespace)]
    assert "Alice & <team> | 2026-10-03 UTC: $5.000000000; 0 requests with missing cost" in titles
    assert "Bob | 2026-10-03 UTC: $0.000000000; 1 requests with missing cost" in titles
    assert "Idle | 2026-09-20 UTC: $0.000000000; 0 requests with missing cost" in titles
    assert "Alice & <team> | 2026-09-03 UTC: $0.000000001; 0 requests with missing cost" in titles

    connection.execute("DELETE FROM requests WHERE started_at <= ?", (cutoff,))
    connection.commit()
    start, rows, daily = graph.read_usage(database, now)
    assert start == midnight - 1
    assert daily["Alice & <team>"][midnight // graph.DAY_MS] == (5_000_000_000, 0)
    connection.execute("DELETE FROM requests")
    connection.commit()
    start, rows, daily = graph.read_usage(database, now)
    assert start == cutoff and daily == {}
    assert rows == [("Alice & <team>", 0, 0), ("Bob", 0, 0), ("Idle", 0, 0)]
    ET.fromstring(graph.render_svg(start, now, rows, daily))
    connection.execute("DELETE FROM users")
    connection.commit()
    start, rows, daily = graph.read_usage(database, now)
    assert rows == [] and daily == {}
    ET.fromstring(graph.render_svg(start, now, rows, daily))
    connection.close()

print("Usage graph tests passed")
