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
            (0, 0, 0, 0, 2),
            (1, 0, 0, 0, 2),
            (2, 0, 0, 0, 2),
            (2, 0, 2, 2, 2),
            (0, 7, 0, 7, 1),
            (0, 0, 1, 1, 2),
        ]
        for pre, agent_status, post, expected, checks in cases:
            with self.subTest(pre=pre, agent=agent_status, post=post):
                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    (root / 'CATALOG_UPDATE.md').write_text('Test runbook\n')
                    count = root / 'count'
                    count.write_text('0')
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
assert Path.cwd() == Path({str(root)!r})
Path({str(marker)!r}).touch()
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


if __name__ == '__main__':
    unittest.main()
