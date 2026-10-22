"""Builds tests/fastread_test.c with mingw-w64 and runs it under Wine.

Skipped when either is missing. Uses the runtime from ./tm install if present, else `wine` on PATH.
Run: python3 -m unittest discover tests
"""
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.environ.get("TM_DATA", os.path.join(ROOT, "data"))
TM_WINE = os.path.join(DATA, "runtime", "wswine.bundle", "bin", "wine")
CC = "x86_64-w64-mingw32-gcc"


def wine_command():
    if os.access(TM_WINE, os.X_OK):
        return [os.path.join(ROOT, "tm"), "wine"]
    for name in ("wine64", "wine"):
        if shutil.which(name):
            return [name]
    return None


@unittest.skipUnless(shutil.which(CC) and wine_command(), f"needs {CC} and Wine")
class FastreadUnderWineTest(unittest.TestCase):
    def test_fast_read_behaves_like_read_process_memory(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "fastread_test.exe")
            subprocess.run([CC, "-Wall", "-Wextra", "-Werror", "-O2", "-o", exe,
                            os.path.join(ROOT, "tests", "fastread_test.c")], check=True)
            result = subprocess.run([*wine_command(), exe], capture_output=True, text=True,
                                    timeout=300)
        self.assertNotIn("FAIL", result.stdout)
        self.assertIn("0 failure(s)", result.stdout, result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
