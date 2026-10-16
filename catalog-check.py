#!/usr/bin/env python3
"""Compare published DeepSeek/Go numeric catalogs, without an LLM or API keys.

Exit 0: matched; 1: differences; 2: could not verify. OpenAI is not covered.
Unexpected document/catalog structure fails closed rather than implying a match.
"""

import argparse
from decimal import Decimal
from html.parser import HTMLParser
from pathlib import Path
import re
import subprocess
import sys


GO_URL = "https://opencode.ai/v2/docs/console/go"
DEEPSEEK_URL = "https://api-docs.deepseek.com/quick_start/pricing/"


class Tables(HTMLParser):
    """Read table cells, including DeepSeek's row/column spans and Go panels."""

    def __init__(self, html):
        super().__init__()
        self.tables = []
        self.panels = []
        self.rows = None
        self.row = None
        self.cell = None
        self.deleted_depth = 0
        self.panel = None
        self.feed(html)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "div":
            self.panels.append(self.panel)
            self.panel = attrs.get("id", self.panel)
        elif tag == "table":
            if self.rows is not None:
                raise ValueError("nested table")
            self.rows = []
        elif tag == "tr" and self.rows is not None:
            self.row = []
        elif tag in ("td", "th") and self.row is not None:
            self.cell = [[], int(attrs.get("rowspan", 1)), int(attrs.get("colspan", 1))]
        elif tag == "br" and self.cell is not None:
            self.cell[0].append(" ")
        elif tag == "del" and self.cell is not None:
            self.deleted_depth += 1

    def handle_data(self, data):
        if self.cell is not None and not self.deleted_depth:
            self.cell[0].append(data)

    def handle_endtag(self, tag):
        if tag == "del" and self.deleted_depth:
            self.deleted_depth -= 1
        elif tag == "div" and self.panels:
            self.panel = self.panels.pop()
        elif tag in ("td", "th") and self.cell is not None:
            if self.deleted_depth:
                raise ValueError("unclosed deleted table text")
            text, rowspan, colspan = self.cell
            self.row.append((" ".join("".join(text).split()), rowspan, colspan))
            self.cell = None
        elif tag == "tr" and self.row is not None:
            self.rows.append(self.row)
            self.row = None
        elif tag == "table" and self.rows is not None:
            self.tables.append((self.panel, expand_spans(self.rows)))
            self.rows = None


def expand_spans(rows):
    pending = {}
    result = []
    for cells in rows:
        row = []
        column = 0

        def append_pending():
            nonlocal column
            while column in pending:
                text, remaining = pending.pop(column)
                row.append(text)
                if remaining > 1:
                    pending[column] = (text, remaining - 1)
                column += 1

        for text, rowspan, colspan in cells:
            append_pending()
            for _ in range(colspan):
                row.append(text)
                if rowspan > 1:
                    pending[column] = (text, rowspan - 1)
                column += 1
        append_pending()
        result.append(row)
    if pending:
        raise ValueError("incomplete row spans")
    return result


def money(text):
    if text == "Free":
        return 0
    if not re.fullmatch(r"\$\d+(?:\.\d+)?", text):
        raise ValueError(f"unrecognized price: {text!r}")
    value = Decimal(text[1:]) * 1_000_000
    if value != int(value):
        raise ValueError(f"price not representable in picodollars/token: {text}")
    return int(value)


def model_id(label):
    # Display names are not always API IDs; all other current names normalize.
    if label == "Claude Haiku 5.5":
        return "claude-haiku-5-5"
    return label.lower().replace(" ", "-")


def go_allowance(text):
    # Tables omits <del> amounts; only the active published allowance remains.
    # Accept the known promotion label, not arbitrary prose or multiple prices.
    if text in ("Unlimited", "Unlimited limited time"):
        return 0
    match = re.fullmatch(r"\$(\d+)(?: \d+x · Limited time)?", text)
    if not match:
        raise ValueError(f"unrecognized allowance: {text!r}")
    return int(match.group(1))


def go_catalog(html):
    tables = Tables(html).tables
    result = {}
    for plan in ("go", "go-plus"):
        matches = [rows for panel, rows in tables if panel == f"go-pricing-panel-{plan}"]
        if len(matches) != 1:
            raise ValueError(f"missing/duplicate {plan} pricing table")
        rows = matches[0]
        if rows[0] != ["Model", "Input", "Output", "Cached Read", "Cached Write", "Monthly limit"]:
            raise ValueError(f"unexpected {plan} pricing columns")
        seen = set()
        for row in rows[1:]:
            if len(row) != 6:
                raise ValueError(f"unexpected {plan} pricing row: {row}")
            label, input_price, output, cached, write, allowance = row
            tier = re.search(r" \((Off-Peak|Peak|[≤>] \d+K tokens)\)$", label)
            suffix = tier.group(1) if tier else ""
            name = model_id(label[:tier.start()] if tier else label)
            key = (name, suffix)
            if key in seen:
                raise ValueError(f"duplicate {plan} row: {key}")
            seen.add(key)
            rates = dict(input=money(input_price), output=money(output),
                         cached_read=money(cached), cached_write=money(input_price if write == "-" else write))
            limit = go_allowance(allowance)
            if plan == "go":
                result[key] = dict(rates=rates, go=limit)
            else:
                if key not in result or result[key]["rates"] != rates:
                    raise ValueError(f"plan token prices/model rows disagree: {key}")
                result[key]["go-plus"] = limit
        if seen != set(result):
            raise ValueError("plan model rows disagree")
    if not result:
        raise ValueError("empty Go catalog")
    return result


def direct_catalog(html):
    tables = [rows for _, rows in Tables(html).tables
              if rows and rows[0][0] == "MODEL"]
    if len(tables) != 1:
        raise ValueError("missing/duplicate DeepSeek model table")
    rows = tables[0]
    if rows[0][:3] != ["MODEL"] * 3:
        raise ValueError("unrecognized DeepSeek model columns")
    names = [re.sub(r"\(\d+\)$", "", cell) for cell in rows[0][3:]]
    if not names or len(set(names)) != len(names) or any(not name.startswith("deepseek-") for name in names):
        raise ValueError("unrecognized DeepSeek model IDs")
    fields = {"1M INPUT TOKENS (CACHE HIT)": "cached_read",
              "1M INPUT TOKENS (CACHE MISS)": "input", "1M OUTPUT TOKENS": "output"}
    result = {(name, tier): {} for name in names for tier in ("OFF-PEAK", "PEAK")}
    for row in rows:
        if not row[0].startswith("PRICING"):
            continue
        if len(row) != len(names) + 3 or row[1] not in fields or row[2] not in ("OFF-PEAK", "PEAK"):
            raise ValueError(f"unrecognized DeepSeek pricing row: {row}")
        for name, price in zip(names, row[3:]):
            rates = result[(name, row[2])]
            field = fields[row[1]]
            if field in rates:
                raise ValueError("duplicate DeepSeek rate")
            rates[field] = money(price)
    for rates in result.values():
        if set(rates) != set(fields.values()):
            raise ValueError("incomplete DeepSeek rates")
        rates["cached_write"] = rates["input"]
    return result


def local_catalog(path):
    text = path.read_text()
    section = text.split("Model model_catalog[] {", 1)[1].split("\n        };", 1)[0]
    result = {}
    for block in re.split(r"\n\s*\{\n", section)[1:]:
        name = re.search(r'\.name = "([^"]+)"', block).group(1)
        if name in result:
            raise ValueError(f"duplicate local model: {name}")
        rates = {}
        for field in ("rates", "above_threshold_rates"):
            match = re.search(r"\." + field + r" = \{([^}]+)\}", block)
            if match:
                rates[field] = {key: int(value.replace("'", ""))
                                for key, value in re.findall(r"\.(\w+) = ([\d']+)", match.group(1))}
                if set(rates[field]) != {"input", "cached_read", "output", "cached_write"}:
                    raise ValueError(f"unrecognized local rates: {name}")
        if "rates" not in rates:
            raise ValueError(f"missing local rates: {name}")
        threshold = re.search(r"\.input_token_threshold = ([\d']+)", block)
        schedule = re.search(r"\.schedule = (\w+)", block)
        result[name] = dict(**rates, threshold=int(threshold.group(1).replace("'", "")) if threshold else 0,
                            schedule=schedule.group(1) if schedule else "")
    if not result or len(result) != section.count(".name ="):
        raise ValueError("incomplete local model catalog")
    return text, result


def compare(repo, go_html, direct_html):
    differences = []

    def check(label, local, published):
        if local != published:
            differences.append(f"{label}: catalog={local!r}, published={published!r}")

    text, local = local_catalog(repo / "providers/opencode_go.hpp")
    published = go_catalog(go_html)
    allowances = {}
    for name, go, plus in re.findall(r'\.model = "([^"]+)", \.go_monthly_usd = (\d+), \.go_plus_monthly_usd = (\d+)', text):
        if name in allowances:
            raise ValueError(f"duplicate local allowance: {name}")
        allowances[name] = {"go": int(go), "go-plus": int(plus)}
    check("Go models", sorted(local), sorted({name for name, _ in published}))
    check("Go allowance models", sorted(allowances), sorted(local))
    expected = set()
    for name, model in local.items():
        if model["schedule"]:
            if model["schedule"] != "SCHEDULE_WEEKDAY_UTC_DOUBLED" or model["threshold"]:
                raise ValueError(f"unrecognized Go schedule: {name}")
            tiers = [("Off-Peak", model["rates"]), ("Peak", {k: v * 2 for k, v in model["rates"].items()})]
        elif model["threshold"]:
            threshold = model["threshold"]
            if threshold % 1000 or "above_threshold_rates" not in model:
                raise ValueError(f"unrecognized Go threshold: {name}")
            tiers = [(f"≤ {threshold // 1000}K tokens", model["rates"]),
                     (f"> {threshold // 1000}K tokens", model["above_threshold_rates"])]
        else:
            tiers = [("", model["rates"])]
        for tier, rates in tiers:
            key = (name, tier)
            expected.add(key)
            if key not in published:
                continue
            for field, value in rates.items():
                check(f"Go {name} {tier} {field}", value, published[key]["rates"][field])
            for plan in ("go", "go-plus"):
                check(f"Go {name} {tier} {plan} allowance", allowances.get(name, {}).get(plan), published[key][plan])
    check("Go model/tier rows", sorted(expected), sorted(published))
    # Fail closed if the schedule prose no longer states the implemented policy.
    go_text = re.sub(r"<[^>]*>", " ", go_html)
    if not re.search(r"Peak hours are 01:00\s*-\s*04:00 and 06:00\s*-\s*10:00 UTC, Monday through Friday(?:; all other hours, including weekends, are Off-Peak\.|, all other hours are Off-Peak\.)", go_text, re.IGNORECASE):
        raise ValueError("could not verify Go peak schedule")

    _, local = local_catalog(repo / "providers/deepseek.hpp")
    published = direct_catalog(direct_html)
    check("DeepSeek models", sorted(local), sorted({name for name, _ in published}))
    for name, model in local.items():
        if model["schedule"] != "SCHEDULE_DEEPSEEK_2026" or model["threshold"]:
            raise ValueError(f"unrecognized direct DeepSeek schedule: {name}")
        for tier, factor in (("OFF-PEAK", 1), ("PEAK", 2)):
            if (name, tier) not in published:
                continue
            for field, value in model["rates"].items():
                check(f"DeepSeek {name} {tier} {field}", value * factor, published[(name, tier)][field])
    direct_text = re.sub(r"<[^>]*>", " ", direct_html)
    if not re.search(r"Peak hours are 01:00\s*-\s*04:00 and 06:00\s*-\s*10:00 UTC, Monday through Friday, excluding Chinese public holidays", direct_text):
        raise ValueError("could not verify direct DeepSeek peak schedule")
    return differences


def fetch(url):
    # curl works with the providers' public documentation; no credentials used.
    return subprocess.run(["curl", "--fail", "--silent", "--show-error", "--location",
                           "--max-time", "60", url], check=True, capture_output=True, text=True).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--go-html", type=Path)
    parser.add_argument("--deepseek-html", type=Path)
    args = parser.parse_args()
    try:
        differences = compare(args.repo, args.go_html.read_text() if args.go_html else fetch(GO_URL),
                              args.deepseek_html.read_text() if args.deepseek_html else fetch(DEEPSEEK_URL))
    except (OSError, ValueError, IndexError, AttributeError, subprocess.SubprocessError) as error:
        print(f"COULD NOT VERIFY DeepSeek/Go: {error}", file=sys.stderr)
        return 2
    for difference in differences:
        print(f"DIFFERENCE {difference}")
    if differences:
        return 1
    print("MATCH: direct DeepSeek rates; Go model/tier rows, rates, and both plan allowances.")
    print(f"Sources: {DEEPSEEK_URL} {GO_URL}")
    print("OpenAI, protocol support, README, and schedule implementation require separate verification.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
