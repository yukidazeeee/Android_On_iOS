import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "configure_app_build", ROOT / "scripts/configure_app_build.py"
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class AppCustomizationTests(unittest.TestCase):
    def test_valid_identity(self):
        self.assertEqual(module.validate_name("Android Test"), "Android Test")
        self.assertEqual(
            module.validate_bundle_id("com.example.android-test"),
            "com.example.android-test",
        )

    def test_invalid_name(self):
        for value in ["", "x" * 65, "bad\nname"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                module.validate_name(value)

    def test_invalid_bundle_id(self):
        for value in ["app", ".com.example", "com..example", "com.example_app"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                module.validate_bundle_id(value)

if __name__ == "__main__":
    unittest.main()
