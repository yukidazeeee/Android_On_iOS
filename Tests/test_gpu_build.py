import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class GPUBuildTests(unittest.TestCase):
    def test_export_patches_remain_idempotent_together(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'system').mkdir()
            symbols = root / 'system/qemu.symbols'
            symbols.write_text('''{
  qemu_init;
  qemu_main_loop;
  qemu_cleanup;
  bql_lock_impl;
  bql_unlock;
  g_assertion_message_expr;
  qemu_thread_create;
  replay_mutex_lock;
  replay_mutex_unlock;
};
''')
            patches = sorted((ROOT / 'ThirdParty/AndroidQemuCompat/patches').glob('000[56]*.patch'))
            for _ in range(2):
                for patch in patches:
                    already = subprocess.run(['git', 'apply', '--reverse', '--check', str(patch)], cwd=root, capture_output=True)
                    if already.returncode:
                        subprocess.run(['git', 'apply', str(patch)], cwd=root, check=True, capture_output=True)
            self.assertEqual(symbols.read_text().count('android51_gpu_start;'), 1)
            self.assertEqual(symbols.read_text().count('android51_host_run;'), 1)


class ANGLEBootstrapTests(unittest.TestCase):
    def run_bootstrap(self, fail=None):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        angle = root / 'angle checkout'
        outer = root / 'outer tools'
        bundled = angle / 'third_party/depot_tools'
        outer.mkdir(parents=True)
        bundled.mkdir(parents=True)
        log = root / 'events'
        for name, directory in [('outer', outer), ('bundled', bundled)]:
            scripts = {
                'ensure_bootstrap': f'''#!/bin/bash
set -eu
[[ "$DEPOT_TOOLS_DIR" == "$(dirname "$0")" ]]
[[ "$DEPOT_TOOLS_UPDATE" == 0 ]]
[[ "$DEPOT_TOOLS_BOOTSTRAP_PYTHON3" == 1 ]]
echo {name}-bootstrap >> "$EVENT_LOG"
[[ "${{FAIL_BOOTSTRAP:-}}" != {name} ]] || exit 23
[[ "${{FAIL_BOOTSTRAP:-}}" != {name}-missing-marker ]] || exit 0
printf 'python-bin\\n' > "$DEPOT_TOOLS_DIR/python3_bin_reldir.txt"
''',
                'python-bin/python3': f'''#!/bin/bash
set -eu
test -s "$(dirname "$0")/../python3_bin_reldir.txt"
echo {name}-python >> "$EVENT_LOG"
''',
                'gclient': f'''#!/bin/bash
set -eu
[[ "$DEPOT_TOOLS_DIR" == "$(dirname "$0")" ]]
if [[ "$1" == sync ]]; then
  [[ "$*" == 'sync --no-history --shallow --nohooks' ]]
  echo sync >> "$EVENT_LOG"
else
  [[ "$1" == runhooks ]]
  test -s "$PWD/third_party/depot_tools/python3_bin_reldir.txt"
  echo {name}-runhooks >> "$EVENT_LOG"
fi
''',
            }
            for filename, content in scripts.items():
                path = directory / filename
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
                path.chmod(0o755)
        env = dict(os.environ, EVENT_LOG=str(log), DEPOT_TOOLS_DIR='/wrong/inherited/path')
        if fail:
            env['FAIL_BOOTSTRAP'] = fail
        result = subprocess.run(['bash', str(ROOT / 'scripts/prepare_angle_tools.sh'),
                                 str(angle), str(outer)], env=env, capture_output=True, text=True)
        events = log.read_text().splitlines() if log.exists() else []
        return result, events

    def test_both_checkouts_bootstrap_before_hooks(self):
        result, events = self.run_bootstrap()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(events, ['outer-bootstrap', 'outer-python', 'sync',
                                 'bundled-bootstrap', 'bundled-python', 'bundled-runhooks'])

    def test_outer_bootstrap_failure_stops_sync(self):
        result, events = self.run_bootstrap('outer')
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertEqual(events, ['outer-bootstrap'])

    def test_missing_python_marker_stops_hooks(self):
        result, events = self.run_bootstrap('bundled-missing-marker')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('python3_bin_reldir.txt missing or empty', result.stderr)
        self.assertEqual(events, ['outer-bootstrap', 'outer-python', 'sync', 'bundled-bootstrap'])

    def test_bundled_bootstrap_failure_stops_hooks(self):
        result, events = self.run_bootstrap('bundled')
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertEqual(events, ['outer-bootstrap', 'outer-python', 'sync', 'bundled-bootstrap'])


if __name__ == '__main__':
    unittest.main()
