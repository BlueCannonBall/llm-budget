#!/usr/bin/env python3
"""Test scheduler control flow with no network, model, or repository changes."""

from pathlib import Path
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]


class CatalogWrapperTests(unittest.TestCase):
    def test_check_and_agent_statuses(self):
        cases = [
            (0, 0, 0, 0, 2, 'complete', False),
            (1, 0, 0, 0, 2, 'complete', False),
            (2, 0, 0, 0, 2, 'complete', False),
            (2, 0, 2, 0, 2, 'complete', False),
            (0, 7, 0, 7, 1, 'blocked', False),
            (0, 0, 1, 0, 2, 'complete', False),
            (0, 0, 0, 1, 2, 'blocked', False),
            (0, 0, 0, 1, 2, 'complete', True),
            (0, 0, 0, 0, 2, 'retry', True),
            (0, 0, 0, 1, 2, 'missing', False),
        ]
        for pre, agent_status, post, expected, checks, outcome, pending in cases:
            with self.subTest(pre=pre, agent=agent_status, post=post,
                              outcome=outcome, pending=pending):
                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    (root / 'CATALOG_UPDATE.md').write_text('Test runbook\n')
                    count = root / 'count'
                    count.write_text('0')
                    state = root / '.local/state/llm-budget'
                    state.mkdir(parents=True)
                    pending_file = state / 'catalog-deploy-pending'
                    result_file = state / 'catalog-update-result'
                    # A previous success must never mask an interrupted/blocked run.
                    result_file.write_text('complete\n')
                    if pending:
                        pending_file.write_text('previously pushed commit\n')
                    checker = root / 'checker'
                    checker.write_text(f'''#!/usr/bin/python3
from pathlib import Path
p = Path({str(count)!r})
n = int(p.read_text())
p.write_text(str(n + 1))
print('checker diagnostic', flush=True)
raise SystemExit([{pre}, {post}][n])
''')
                    agent = root / 'agent'
                    marker = root / 'agent-ran'
                    agent.write_text(f'''#!/usr/bin/python3
import sys
from pathlib import Path
assert sys.argv[1:5] == ['run', '--auto', '--model', 'openai/gpt-6-luna']
prompt = sys.argv[5]
assert prompt.startswith('Test runbook')
assert 'Exit status: {pre}' in prompt
assert 'checker diagnostic' in prompt
assert 'repair catalog-check.py' in prompt
assert 'must not block deployment' in prompt
assert 'A server outage must not delay' in prompt
assert 'Pending deployment file: {pending_file}' in prompt
assert Path({str(result_file)!r}).read_text() == 'blocked\\n'
assert Path.cwd() == Path({str(root)!r})
Path({str(marker)!r}).touch()
outcome = {outcome!r}
if outcome == 'retry':
    assert 'pending; retry deployment even if catalogs are unchanged' in prompt
    Path({str(pending_file)!r}).unlink()
    Path({str(result_file)!r}).write_text('complete\\n')
elif outcome == 'missing':
    Path({str(result_file)!r}).unlink()
else:
    Path({str(result_file)!r}).write_text(outcome + '\\n')
raise SystemExit({agent_status})
''')
                    checker.chmod(0o755)
                    agent.chmod(0o755)
                    source = (REPO / 'catalog-update-daily.sh').read_text()
                    source = source.replace('export HOME=/home/optimal', f'export HOME={root}')
                    source = source.replace('repo=/home/optimal/Documents/llm-budget', f'repo={root}')
                    source = source.replace('python3 "$repo/catalog-check.py"', f'"{checker}"')
                    wrapper = root / 'wrapper.sh'
                    wrapper.write_text(source)
                    result = subprocess.run(
                        ['bash', str(wrapper)], env={'OPENCODE_BIN': str(agent)},
                    )
                    self.assertEqual(result.returncode, expected)
                    self.assertEqual(int(count.read_text()), checks)
                    self.assertTrue(marker.exists())
                    self.assertEqual(pending_file.exists(), pending and outcome != 'retry')
                    log = (root / '.local/state/llm-budget/catalog-update.log').read_text()
                    if agent_status == 0 and post != 0:
                        self.assertIn(f'WARNING: final catalog check exited {post}', log)
                    if agent_status == 0 and expected != 0:
                        self.assertIn('ERROR: maintenance incomplete', log)


if __name__ == '__main__':
    unittest.main()
