"""Generate an SVG of total and daily per-user spending using Python's standard library."""

import argparse
from datetime import datetime, timezone
from decimal import Decimal
from html import escape
from pathlib import Path
import sqlite3
import time


DAY_MS = 86_400_000


def dollars(nanodollars):
    return f"${Decimal(nanodollars) / Decimal(1_000_000_000):,.9f}"


def utc_label(timestamp_ms):
    return datetime.fromtimestamp(timestamp_ms / 1000, timezone.utc).strftime(
        "%Y-%m-%d %H:%M:%S UTC"
    )


def read_usage(database, now_ms):
    # mode=ro prevents accidentally creating a new database for a mistyped path.
    connection = sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True)
    try:
        connection.execute("BEGIN")  # Coverage, totals, and daily costs share a snapshot.
        earliest = connection.execute(
            "SELECT MIN(started_at) FROM requests WHERE started_at <= ?", (now_ms,)
        ).fetchone()[0]
        start_ms = max(now_ms - 30 * DAY_MS, earliest) if earliest is not None else now_ms - 30 * DAY_MS
        rows = connection.execute(
            """
            SELECT u.name, COALESCE(SUM(r.cost_nanodollars), 0),
                   COUNT(r.id) - COUNT(r.cost_nanodollars)
            FROM users AS u
            LEFT JOIN requests AS r ON r.user_id = u.id
                AND r.started_at >= ? AND r.started_at <= ?
            GROUP BY u.id, u.name
            ORDER BY 2 DESC, u.name, u.id
            """,
            (start_ms, now_ms),
        ).fetchall()
        daily_rows = connection.execute(
            """
            SELECT u.name, r.started_at / ? AS day,
                   COALESCE(SUM(r.cost_nanodollars), 0),
                   COUNT(*) - COUNT(r.cost_nanodollars)
            FROM requests AS r JOIN users AS u ON r.user_id = u.id
            WHERE r.started_at >= ? AND r.started_at <= ?
            GROUP BY u.id, u.name, day
            """,
            (DAY_MS, start_ms, now_ms),
        ).fetchall()
        daily = {}
        for name, day, cost, missing in daily_rows:
            daily.setdefault(name, {})[day] = (cost, missing)
        return start_ms, rows, daily
    finally:
        connection.close()


def render_daily(start_ms, now_ms, rows, daily, width, top):
    days = list(range(start_ms // DAY_MS, now_ms // DAY_MS + 1))
    maximum = max((cost for values in daily.values() for cost, _ in values.values()), default=0)
    scale = maximum or 1_000_000_000
    left = 150
    chart_width = width - left - 40
    step = chart_width / len(days)
    chart_height = 120
    panel_height = 235
    parts = [
        f'<text x="24" y="{top}" font-size="22" font-weight="bold">Daily spending by user</text>',
        f'<text x="24" y="{top + 26}">UTC days; first and last days may be partial. '
        'Shared USD scale; hover over bars for exact amounts and missing-cost counts.</text>',
    ]
    for index, (name, _, _) in enumerate(rows):
        panel_top = top + 65 + index * panel_height
        baseline = panel_top + chart_height
        parts.append(
            f'<text x="24" y="{panel_top - 15}" font-weight="bold">{escape(name)}</text>'
        )
        for tick in range(3):
            y = baseline - chart_height * tick / 2
            value = dollars(Decimal(scale) * tick / 2)
            parts.extend([
                f'<line x1="{left}" y1="{y}" x2="{width - 40}" y2="{y}" stroke="#e2e8f0"/>',
                f'<text x="{left - 10}" y="{y + 4}" text-anchor="end" font-size="12">{value}</text>',
            ])
        for offset, day in enumerate(days):
            cost, missing = daily.get(name, {}).get(day, (0, 0))
            date = datetime.fromtimestamp(day * DAY_MS / 1000, timezone.utc).strftime("%Y-%m-%d")
            x = left + offset * step
            bar_height = chart_height * cost / scale
            # Transparent full-height hit area also exposes zero-spend days.
            tooltip = f'{escape(name)} | {date} UTC: {dollars(cost)}; {missing} requests with missing cost'
            parts.extend([
                f'<rect x="{x + 2:.3f}" y="{baseline - bar_height:.3f}" '
                f'width="{step - 4:.3f}" height="{bar_height:.3f}" fill="#2563eb"/>',
                f'<rect x="{x:.3f}" y="{panel_top}" width="{step:.3f}" height="{chart_height}" '
                f'fill="transparent"><title>{tooltip}</title></rect>',
                f'<text transform="translate({x + step / 2:.3f} {baseline + 16}) rotate(-45)" '
                f'text-anchor="end" font-size="12">{date}</text>',
            ])
    return parts, top + 65 + len(rows) * panel_height


def render_svg(start_ms, now_ms, rows, daily):
    label_width = max(120, max((len(name) * 8 + 24 for name, _, _ in rows), default=0))
    chart_width = 640
    width = max(1100, label_width + chart_width + 240)
    top = 145
    row_height = 38
    bottom = top + max(len(rows), 1) * row_height
    daily_parts, daily_bottom = render_daily(start_ms, now_ms, rows, daily, width, bottom + 80)
    height = daily_bottom + 55
    maximum = max((cost for _, cost, _ in rows), default=0)
    total = sum(cost for _, cost, _ in rows)
    unknown = sum(count for _, _, count in rows)
    title = "Recorded spending by user"
    period = f"{utc_label(start_ms)} to {utc_label(now_ms)}"
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title description">',
        f'<title id="title">{title}</title>',
        f'<desc id="description">Total and daily recorded USD by user. {period}. '
        'Costs are attributed by request start time; missing costs are excluded.</desc>',
        '<rect width="100%" height="100%" fill="white"/>',
        '<g font-family="sans-serif" font-size="14" fill="#172033">',
        f'<text x="24" y="36" font-size="24" font-weight="bold">{title}</text>',
        f'<text x="24" y="64">{period}</text>',
        f'<text x="24" y="90">Total: {dollars(total)} | Users: {len(rows)} | '
        f'Requests with missing cost: {unknown}</text>',
    ]
    scale = maximum or 1_000_000_000
    for tick in range(5):
        x = label_width + chart_width * tick / 4
        value = dollars(Decimal(scale) * tick / 4)
        parts.append(
            f'<line x1="{x}" y1="{top - 12}" x2="{x}" y2="{bottom}" stroke="#e2e8f0"/>'
        )
        parts.append(
            f'<text x="{x}" y="{bottom + 25}" text-anchor="middle" font-size="12">{value}</text>'
        )
    for index, (name, cost, missing) in enumerate(rows):
        y = top + index * row_height
        bar_width = chart_width * cost / scale
        parts.extend([
            f'<text x="{label_width - 12}" y="{y + 19}" text-anchor="end">{escape(name)}</text>',
            f'<rect x="{label_width}" y="{y}" width="{bar_width:.3f}" height="28" '
            f'fill="#2563eb"><title>{escape(name)}: {dollars(cost)}; '
            f'{missing} requests with missing cost</title></rect>',
            f'<text x="{label_width + bar_width + 10:.3f}" y="{y + 19}">{dollars(cost)}</text>',
        ])
    if not rows:
        parts.append(f'<text x="24" y="{top + 19}">No users in the database.</text>')
    parts.extend(daily_parts)
    parts.extend([
        f'<text x="24" y="{daily_bottom + 20}">Recorded estimates, not provider invoices. '
        'Includes all request states; missing costs are excluded.</text>',
        '</g>',
        '</svg>',
    ])
    return "\n".join(parts) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", type=Path, default=Path("llm-budget.db"), help="SQLite database (default: llm-budget.db)")
    parser.add_argument("--output", type=Path, default=Path("usage.svg"), help="SVG destination (default: usage.svg)")
    args = parser.parse_args()
    if args.output.resolve() == args.db.resolve():
        parser.error("output must not overwrite the database")
    try:
        now_ms = time.time_ns() // 1_000_000
        start_ms, rows, daily = read_usage(args.db, now_ms)
        args.output.write_text(render_svg(start_ms, now_ms, rows, daily), encoding="utf-8")
    except (OSError, sqlite3.Error) as error:
        parser.exit(1, f"error: {error}\n")
    print(f"Wrote {args.output} ({len(rows)} users; {utc_label(start_ms)} to {utc_label(now_ms)})")


if __name__ == "__main__":
    main()
