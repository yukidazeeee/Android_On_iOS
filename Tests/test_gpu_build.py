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


if __name__ == '__main__':
    unittest.main()
