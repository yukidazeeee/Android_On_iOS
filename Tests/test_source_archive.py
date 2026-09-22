import importlib.util
from pathlib import Path
import tarfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('source_archive', ROOT / 'scripts/collect_engine_source.py')
archive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(archive)

class SourceArchiveTests(unittest.TestCase):
    def test_excludes_downloaded_toolchains_and_build_products(self):
        for path in ['ThirdParty/checkouts/angle/third_party/llvm-build/Release/bin/clang',
                     'ThirdParty/checkouts/depot_tools/bootstrap-3.11_bin/python3/bin/python3',
                     'ThirdParty/checkouts/angle/third_party/siso/cipd/siso',
                     'ThirdParty/checkouts/angle/out/ios/libEGL.framework/libEGL',
                     'ThirdParty/checkouts/angle/.git/objects/pack/data']:
            with self.subTest(path=path):
                self.assertIsNone(archive.allowed(tarfile.TarInfo(path)))

    def test_keeps_engine_source_build_recipes_and_licenses(self):
        for path in ['ThirdParty/checkouts/angle/src/libANGLE/renderer/metal/ImageMtl.mm',
                     'ThirdParty/checkouts/angle/DEPS',
                     'ThirdParty/checkouts/angle/third_party/zlib/LICENSE',
                     'ThirdParty/EmuGL/host/libs/libOpenglRender/ColorBuffer.cpp',
                     'GPU/MetalImages.mm', 'scripts/build_gpu_ios.sh']:
            with self.subTest(path=path):
                self.assertIsNotNone(archive.allowed(tarfile.TarInfo(path)))
