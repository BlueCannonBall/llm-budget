#!/usr/bin/env python3
"""Offline regression tests for catalog verification and the cron wrapper."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("catalog_check", ROOT / "catalog-check.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def go_html(go=80, plus=160):
    html = "Peak hours are 01:00-04:00 and 06:00-10:00 UTC, Monday through Friday; all other hours, including weekends, are Off-Peak."
    for plan, allowance in (("go", go), ("go-plus", plus)):
        html += f'<div id="go-pricing-panel-{plan}"><div><table><tr>'
        html += "".join(f"<th>{cell}</th>" for cell in
                        ("Model", "Input", "Output", "Cached Read", "Cached Write", "Monthly limit"))
        html += "</tr>"
        for tier, prices in (("Off-Peak", ("$0.15", "$0.60", "$0.003")),
                             ("Peak", ("$0.30", "$1.20", "$0.006"))):
            html += "<tr>" + "".join(f"<td>{cell}</td>" for cell in
                                     (f"DeepSeek V4.1 Flash ({tier})", *prices, "-", f"${allowance}")) + "</tr>"
        html += "</table></div></div>"
    return html


DIRECT_HTML = """
<table><tr><td colspan="3">MODEL</td><td>deepseek-flash<sup>(1)</sup></td></tr>
<tr><td rowspan="6">PRICING<sup>(2)</sup></td>
<td rowspan="2">1M INPUT TOKENS<br>(CACHE HIT)</td><td>OFF-PEAK</td><td>$0.003</td></tr>
<tr><td>PEAK</td><td>$0.006</td></tr>
<tr><td rowspan="2">1M INPUT TOKENS<br>(CACHE MISS)</td><td>OFF-PEAK</td><td>$0.15</td></tr>
<tr><td>PEAK</td><td>$0.30</td></tr>
<tr><td rowspan="2">1M OUTPUT TOKENS</td><td>OFF-PEAK</td><td>$0.60</td></tr>
<tr><td>PEAK</td><td>$1.20</td></tr></table>
Peak hours are 01:00 - 04:00 and 06:00 - 10:00 UTC, Monday through Friday, excluding Chinese public holidays
"""


def local_source(name, schedule):
    return f'''Model model_catalog[] {{
            {{
                .name = "{name}",
                .rates = {{.input = 150'000, .cached_read = 3'000, .output = 600'000, .cached_write = 150'000}},
                .schedule = {schedule},
            }},
        }};
'''


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="catalog-test-", dir="/tmp/opencode")
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        (self.repo / "providers").mkdir()
        (self.repo / "providers/deepseek.hpp").write_text(local_source("deepseek-flash", "SCHEDULE_DEEPSEEK_2026"))
        self.go_path = self.repo / "providers/opencode_go.hpp"
        self.go_path.write_text(local_source("deepseek-v4.1-flash", "SCHEDULE_WEEKDAY_UTC_DOUBLED") +
                                '.model = "deepseek-v4.1-flash", .go_monthly_usd = 80, .go_plus_monthly_usd = 160')

    def test_matching(self):
        self.assertEqual(checker.compare(self.repo, go_html(), DIRECT_HTML), [])

    def test_october_12_missed_allowances(self):
        self.go_path.write_text(self.go_path.read_text().replace("= 80", "= 60").replace("= 160", "= 120"))
        differences = checker.compare(self.repo, go_html(), DIRECT_HTML)
        self.assertEqual(len(differences), 4)  # Both plans in both peak rows.
        self.assertTrue(any("catalog=60, published=80" in item for item in differences))
        self.assertTrue(any("catalog=120, published=160" in item for item in differences))

    def test_only_plus_changed(self):
        differences = checker.compare(self.repo, go_html(plus=180), DIRECT_HTML)
        self.assertEqual(len(differences), 2)
        self.assertTrue(all("go-plus allowance" in item for item in differences))

    def test_promotional_allowances_use_active_not_deleted_prices(self):
        html = go_html(go=60, plus=240)
        html = html.replace("<td>$60</td>", '<td><del>$30</del> <strong>$60</strong><br/><small>2x · Limited time</small></td>')
        html = html.replace("<td>$240</td>", '<td><del>$120</del> <strong>$240</strong><br/><small>2x · Limited time</small></td>')
        html = html.replace("DeepSeek V4.1 Flash", "Gemini 4 Argon Contributor")
        catalog = checker.go_catalog(html)
        for tier in ("Off-Peak", "Peak"):
            model = catalog[("gemini-4-argon-contributor", tier)]
            self.assertEqual(model["go"], 60)
            self.assertEqual(model["go-plus"], 240)
        differences = checker.compare(self.repo, html, DIRECT_HTML)
        self.assertTrue(any("Go models" in item and "gemini-4-argon-contributor" in item for item in differences))

    def test_promotion_detects_allowance_changes(self):
        html = go_html().replace("<td>$80</td>", '<td><del>$80</del> <strong>$160</strong><br/><small>2x · Limited time</small></td>')
        differences = checker.compare(self.repo, html, DIRECT_HTML)
        self.assertEqual(len(differences), 2)
        self.assertTrue(all("catalog=80, published=160" in item for item in differences))

    def test_four_times_promotion_reads_active_allowance(self):
        html = go_html(go=60, plus=240)
        html = html.replace("<td>$60</td>", '<td><del>$15</del> <strong>$60</strong><br/><small>4x · Limited time</small></td>')
        html = html.replace("<td>$240</td>", '<td><del>$60</del> <strong>$240</strong><br/><small>4x · Limited time</small></td>')
        catalog = checker.go_catalog(html)
        self.assertEqual(catalog[("deepseek-v4.1-flash", "Off-Peak")]["go"], 60)
        self.assertEqual(catalog[("deepseek-v4.1-flash", "Peak")]["go-plus"], 240)

    def test_ambiguous_or_unknown_allowances_fail_closed(self):
        for allowance in ("$30 $60 2x · Limited time", "$60 TBD", "$60 limited time", "Unlimited TBD", "$60.50"):
            with self.subTest(allowance=allowance), self.assertRaises(ValueError):
                checker.go_catalog(go_html().replace("<td>$80</td>", f"<td>{allowance}</td>"))

    def test_unlimited_allowances(self):
        for allowance in ("Unlimited", "Unlimited limited time"):
            with self.subTest(allowance=allowance):
                catalog = checker.go_catalog(go_html().replace("<td>$80</td>", f"<td>{allowance}</td>"))
                self.assertEqual(catalog[("deepseek-v4.1-flash", "Off-Peak")]["go"], 0)

    def test_direct_rate_changed(self):
        differences = checker.compare(self.repo, go_html(), DIRECT_HTML.replace("$0.003", "$0.004"))
        self.assertEqual(len(differences), 1)
        self.assertIn("DeepSeek deepseek-flash OFF-PEAK cached_read", differences[0])

    def test_added_model_detected(self):
        html = go_html().replace("</table>", '<tr><td>New Model</td><td>$1</td><td>$2</td><td>$0.1</td><td>-</td><td>$60</td></tr></table>')
        self.assertTrue(any("Go models" in item for item in checker.compare(self.repo, html, DIRECT_HTML)))

    def test_missing_plus_table_fails_closed(self):
        with self.assertRaises(ValueError):
            checker.go_catalog(go_html().replace('id="go-pricing-panel-go-plus"', 'id="unknown"'))

    def test_malformed_price_fails_closed(self):
        with self.assertRaises(ValueError):
            checker.go_catalog(go_html().replace("$0.003", "TBD"))

    def test_duplicate_row_fails_closed(self):
        with self.assertRaises(ValueError):
            checker.go_catalog(go_html().replace("(Peak)", "(Off-Peak)"))

    def test_go_plan_rates_disagree_fails_closed(self):
        html = go_html()
        split = html.index('id="go-pricing-panel-go-plus"')
        html = html[:split] + html[split:].replace("$0.003", "$0.004")
        with self.assertRaises(ValueError):
            checker.go_catalog(html)

    def test_missing_direct_price_fails_closed(self):
        with self.assertRaises(ValueError):
            checker.direct_catalog(DIRECT_HTML.replace("<td>$1.20</td>", ""))

    def test_context_tiers_and_cache_write_rates(self):
        self.go_path.write_text('''Model model_catalog[] {
            {
                .name = "qwen3.7-plus",
                .rates = {.input = 400'000, .cached_read = 40'000, .output = 1'600'000, .cached_write = 500'000},
                .input_token_threshold = 256'000,
                .above_threshold_rates = {.input = 1'200'000, .cached_read = 120'000, .output = 4'800'000, .cached_write = 1'500'000},
            },
        };
        .model = "qwen3.7-plus", .go_monthly_usd = 60, .go_plus_monthly_usd = 180
''')
        html = go_html(go=60, plus=180)
        html = html.replace("DeepSeek V4.1 Flash (Off-Peak)", "Qwen3.7 Plus (≤ 256K tokens)")
        html = html.replace("DeepSeek V4.1 Flash (Peak)", "Qwen3.7 Plus (&gt; 256K tokens)")
        html = html.replace("<td>$0.15</td><td>$0.60</td><td>$0.003</td><td>-</td>",
                            "<td>$0.40</td><td>$1.60</td><td>$0.04</td><td>$0.50</td>")
        html = html.replace("<td>$0.30</td><td>$1.20</td><td>$0.006</td><td>-</td>",
                            "<td>$1.20</td><td>$4.80</td><td>$0.12</td><td>$1.50</td>")
        self.assertEqual(checker.compare(self.repo, html, DIRECT_HTML), [])
        differences = checker.compare(self.repo, html.replace("$1.50", "$1.60"), DIRECT_HTML)
        self.assertEqual(len(differences), 1)
        self.assertIn("cached_write", differences[0])

    def test_schedule_changed_fails_closed(self):
        with self.assertRaises(ValueError):
            checker.compare(self.repo, go_html().replace("01:00-04:00", "02:00-04:00"), DIRECT_HTML)

    def test_published_schedule_whitespace_and_case(self):
        html = go_html().replace("01:00-04:00", "01:00 - 04:00").replace("Off-Peak.", "off-peak.")
        self.assertEqual(checker.compare(self.repo, html, DIRECT_HTML), [])

    def test_cli_exit_codes(self):
        go = self.repo / "go.html"
        direct = self.repo / "deepseek.html"
        direct.write_text(DIRECT_HTML)
        for html, expected in ((go_html(), 0), (go_html(go=90), 1), ("unreadable", 2)):
            go.write_text(html)
            result = subprocess.run(["python3", str(ROOT / "catalog-check.py"), "--repo", str(self.repo),
                                     "--go-html", str(go), "--deepseek-html", str(direct)], capture_output=True)
            self.assertEqual(result.returncode, expected, result.stdout + result.stderr)

    def wrapper(self, pre=0, post=0, agent=0):
        # Run an isolated copy: never launch real OpenCode, fetch docs, or touch real logs.
        source = (ROOT / "catalog-update-daily.sh").read_text()
        source = source.replace("export HOME=/home/optimal", f"export HOME={self.repo}")
        source = source.replace("repo=/home/optimal/Documents/llm-budget", f"repo={self.repo}")
        script = self.repo / "wrapper.sh"
        script.write_text(source)
        (self.repo / "CATALOG_UPDATE.md").write_text("test runbook")
        (self.repo / "catalog-check.py").write_text(
            "from pathlib import Path\nimport sys\np=Path(__file__).with_name('calls')\n"
            f"status={post} if p.exists() else {pre}\np.touch()\nsys.exit(status)\n")
        stub = self.repo / "stub"
        stub.write_text(f"#!/bin/sh\ntouch '{self.repo}/agent-ran'\nexit {agent}\n")
        stub.chmod(0o700)
        result = subprocess.run(["bash", str(script)], env={"OPENCODE_BIN": str(stub)}, capture_output=True)
        return result.returncode, (self.repo / ".local/state/llm-budget/catalog-update.log").read_text()

    def test_wrapper_agent_failure_propagates(self):
        code, log = self.wrapper(agent=7)
        self.assertEqual(code, 7)
        self.assertIn("exit 7 at", log)

    def test_wrapper_remaining_differences_are_logged(self):
        code, log = self.wrapper(pre=1, post=1)
        self.assertEqual(code, 0)
        self.assertIn("WARNING: final catalog check exited 1", log)

    def test_wrapper_unknown_source_does_not_block_agent(self):
        code, log = self.wrapper(pre=2, post=2)
        self.assertEqual(code, 0)
        self.assertIn("WARNING: final catalog check exited 2", log)
        self.assertTrue((self.repo / "agent-ran").exists())

    def test_wrapper_fixed_difference_succeeds(self):
        code, _ = self.wrapper(pre=1, post=0)
        self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
