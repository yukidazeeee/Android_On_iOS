import importlib.util
import json
from pathlib import Path
import plistlib
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('engine_package', ROOT / 'scripts/package_engine_frameworks.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)

class EnginePackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.engine = root / 'qemu.dylib'
        self.engine.write_bytes(b'engine fixture')
        self.prefix = root / 'prefix'
        (self.prefix / 'lib').mkdir(parents=True)
        self.glib = self.prefix / 'lib/libglib-2.0.0.dylib'
        self.glib.write_bytes(b'dependency fixture')
        self.destination = root / 'frameworks'
        self.platform = 'IOS'
        self.minimum = '17.0'
        self.architecture = 'arm64'
        self.foreign = None
        self.missing_exports = set()

    def tool(self, *args):
        if args[:2] == ('xcrun', 'nm'):
            return ''.join('_' + name + '\n' for name in sorted(package.REQUIRED_ENGINE_SYMBOLS - self.missing_exports))
        if args[:2] == ('xcrun', 'lipo'): return self.architecture + '\n'
        if args[:2] == ('xcrun', 'vtool'): return 'platform ' + self.platform + '\nminos ' + self.minimum + '\n'
        if args[:2] == ('otool', '-L'):
            path = Path(args[2])
            result = f'{path}:\n  {path} (compatibility version 1.0.0)\n'
            if path == self.engine:
                result += f'  {self.foreign or self.glib} (compatibility version 1.0.0)\n'
            return result + '  /usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n'
        self.fail(f'Unexpected inspection: {args}')

    def invoke(self):
        with patch.object(package, 'run', self.tool), patch.object(package.subprocess, 'run') as mutation:
            package.package(self.engine, self.prefix, self.destination)
            return mutation.call_args_list

    def test_dependency_closure_and_relocatable_install_names(self):
        calls = self.invoke()
        binary = self.destination / 'AndroidQEMU.framework/AndroidQEMU'
        self.assertEqual(binary.read_bytes(), b'engine fixture')
        info = plistlib.loads((self.destination / 'libglib_2_0_0.framework/Info.plist').read_bytes())
        self.assertNotIn('_', info['CFBundleIdentifier'])
        self.assertTrue(any('-change' in call.args[0] and '@rpath/libglib_2_0_0.framework/libglib_2_0_0' in call.args[0] for call in calls))

    def test_dynamically_loaded_gpu_libraries_are_packaged(self):
        libraries = []
        for name in ('libEGL', 'libGLESv1_CM', 'libGLESv2'):
            library = self.prefix / 'lib' / (name + '.dylib')
            library.write_bytes(name.encode())
            libraries.append(library)
        with patch.object(package, 'run', self.tool), patch.object(package.subprocess, 'run'):
            package.package(self.engine, self.prefix, self.destination, libraries)
        manifest = json.loads((self.destination / 'engine-manifest.json').read_text())
        for name in ('libEGL', 'libGLESv1_CM', 'libGLESv2'):
            self.assertIn(name, manifest['frameworks'])
            self.assertEqual((self.destination / (name + '.framework') / name).read_bytes(), name.encode())

    def test_angle_framework_dependency_is_found_in_staged_ios_libraries(self):
        egl = self.prefix / 'lib/libEGL.dylib'
        gles = self.prefix / 'lib/libGLESv2.dylib'
        egl.write_bytes(b'EGL')
        gles.write_bytes(b'GLES')
        inspect = self.tool
        def angle_tools(*args):
            if args[:2] == ('otool', '-L') and Path(args[2]) == egl:
                return f'{egl}:\n  @rpath/libEGL.framework/libEGL (compatibility version 1.0.0)\n  @rpath/libGLESv2.framework/libGLESv2 (compatibility version 1.0.0)\n'
            return inspect(*args)
        with patch.object(package, 'run', angle_tools), patch.object(package.subprocess, 'run'):
            package.package(self.engine, self.prefix, self.destination, [egl])
        self.assertEqual((self.destination / 'libGLESv2.framework/libGLESv2').read_bytes(), b'GLES')

    def test_rejects_macos_binaries(self):
        self.platform = 'MACOS'
        with self.assertRaises(ValueError): self.invoke()

    def test_rejects_non_arm64_binary(self):
        self.architecture = 'x86_64'
        with self.assertRaises(ValueError): self.invoke()

    def test_rejects_unbundled_host_dependency(self):
        self.foreign = Path(self.temp.name) / 'brew-library.dylib'
        self.foreign.write_bytes(b'host fixture')
        with self.assertRaises(ValueError): self.invoke()

    def test_rejects_engine_missing_each_application_export(self):
        for symbol in package.REQUIRED_ENGINE_SYMBOLS:
            with self.subTest(symbol=symbol):
                self.missing_exports = {symbol}
                with self.assertRaisesRegex(ValueError, symbol):
                    self.invoke()

    def test_rechecks_exports_after_framework_relocation(self):
        inspect = self.tool
        def lose_export(*args):
            if args[:2] == ('xcrun', 'nm') and Path(args[-1]) != self.engine:
                return ''
            return inspect(*args)
        with patch.object(package, 'run', lose_export), patch.object(package.subprocess, 'run'):
            with self.assertRaisesRegex(ValueError, 'missing application exports'):
                package.package(self.engine, self.prefix, self.destination)

    def test_rejects_stale_ios26_dependency(self):
        self.minimum = '26.0'
        with self.assertRaisesRegex(ValueError, 'newer iOS'):
            self.invoke()
