"""./tm fastread and ./tm openplanet against a fake game directory, with fake mingw and 7zz.

Run: python3 -m unittest discover tests
"""
import os
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TM = os.path.join(ROOT, "tm")
GAME_SUBDIR = "prefix/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/Trackmania"
# The only host commands tm and the fakes use here. PATH holds nothing else, so a real mingw-w64
# or 7-Zip on the machine never leaks into a test.
HOST_TOOLS = ("bash", "dirname", "grep", "mkdir", "mv", "cp", "rm", "tar")
LOADER = b"MZ Openplanet loader v1"
NEW_LOADER = b"MZ Openplanet loader v2"

with open(TM) as tm_file:
    MARKER = re.search(r'^FASTREAD_MARKER="([^"]+)"', tm_file.read(), re.M).group(1)

# Writes the shim marker to the file after -o, like the real build would.
FAKE_GCC = """#!/usr/bin/env bash
while [[ $# -gt 0 ]]; do [[ "$1" == -o ]] && out="$2"; shift; done
printf 'MZ %s' "$FASTREAD_MARKER" >"$out"
"""
FAILING_GCC = "#!/usr/bin/env bash\necho 'fastread.c: error' >&2\nexit 1\n"
# 7zz x -y -o<dir> <installer> ...: the fake installer is a tar archive.
FAKE_7ZZ = """#!/usr/bin/env bash
for arg in "$@"; do
  case "$arg" in -o*) out="${arg#-o}" ;; -*) ;; *.exe) installer="$arg" ;; esac
done
tar -xf "$installer" -C "$out"
"""


class FastreadTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.data = os.path.join(self.tmp.name, "data")
        self.game = os.path.join(self.data, GAME_SUBDIR)
        self.bin = os.path.join(self.tmp.name, "bin")
        os.makedirs(self.game)
        os.makedirs(self.bin)
        for tool in HOST_TOOLS:
            os.symlink(shutil.which(tool), os.path.join(self.bin, tool))
        self.write_game_file("Trackmania.exe", b"MZ game")
        self.write_game_file("Openplanet.dll", b"MZ Openplanet")
        self.write_game_file("dinput8.dll", LOADER)
        self.fake_tool("x86_64-w64-mingw32-gcc", FAKE_GCC)
        self.fake_tool("7zz", FAKE_7ZZ)

    def tearDown(self):
        self.tmp.cleanup()

    def fake_tool(self, name, script):
        path = os.path.join(self.bin, name)
        with open(path, "w") as f:
            f.write(script)
        os.chmod(path, os.stat(path).st_mode | stat.S_IEXEC)

    def write_game_file(self, name, content):
        with open(os.path.join(self.game, name), "wb") as f:
            f.write(content)

    def game_file(self, name):
        path = os.path.join(self.game, name)
        if not os.path.exists(path):
            return None
        with open(path, "rb") as f:
            return f.read()

    def is_shim(self, name):
        content = self.game_file(name)
        return content is not None and MARKER.encode() in content

    def tm(self, *args):
        env = dict(os.environ, TM_DATA=self.data, FASTREAD_MARKER=MARKER, PATH=self.bin)
        return subprocess.run([TM, *args], env=env, capture_output=True, text=True, timeout=30)

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)

    def openplanet_installer(self, loader):
        """A fake Openplanet installer that drops `loader` as dinput8.dll."""
        payload = os.path.join(self.tmp.name, "payload")
        os.makedirs(payload, exist_ok=True)
        for name, content in (("dinput8.dll", loader), ("Openplanet.dll", b"MZ Openplanet")):
            with open(os.path.join(payload, name), "wb") as f:
                f.write(content)
        installer = os.path.join(self.tmp.name, "OpenplanetNext_1.0.0.exe")
        with tarfile.open(installer, "w") as tar:
            for name in os.listdir(payload):
                tar.add(os.path.join(payload, name), arcname=name)
        return installer

    # on / off

    def test_on_puts_the_shim_in_front_of_the_loader(self):
        self.assert_ok(self.tm("fastread", "on"))
        self.assertTrue(self.is_shim("dinput8.dll"))
        self.assertEqual(self.game_file("dinput8_openplanet.dll"), LOADER)

    def test_on_twice_keeps_the_loader(self):
        self.assert_ok(self.tm("fastread", "on"))
        self.assert_ok(self.tm("fastread", "on"))
        self.assertTrue(self.is_shim("dinput8.dll"))
        self.assertEqual(self.game_file("dinput8_openplanet.dll"), LOADER)

    def test_off_restores_the_loader(self):
        self.assert_ok(self.tm("fastread", "on"))
        self.assert_ok(self.tm("fastread", "off"))
        self.assertEqual(self.game_file("dinput8.dll"), LOADER)
        self.assertIsNone(self.game_file("dinput8_openplanet.dll"))

    def test_off_when_already_off_changes_nothing(self):
        self.assert_ok(self.tm("fastread", "off"))
        self.assertEqual(self.game_file("dinput8.dll"), LOADER)
        self.assertIsNone(self.game_file("dinput8_openplanet.dll"))

    def test_on_after_off_reinstalls_the_shim(self):
        self.assert_ok(self.tm("fastread", "on"))
        self.assert_ok(self.tm("fastread", "off"))
        self.assert_ok(self.tm("fastread", "on"))
        self.assertTrue(self.is_shim("dinput8.dll"))
        self.assertEqual(self.game_file("dinput8_openplanet.dll"), LOADER)

    # Openplanet replaced its loader by itself while fastread was on

    def test_on_keeps_a_loader_that_openplanet_updated_itself(self):
        self.assert_ok(self.tm("fastread", "on"))
        self.write_game_file("dinput8.dll", NEW_LOADER)
        self.assert_ok(self.tm("fastread", "on"))
        self.assertTrue(self.is_shim("dinput8.dll"))
        self.assertEqual(self.game_file("dinput8_openplanet.dll"), NEW_LOADER)

    def test_off_keeps_a_loader_that_openplanet_updated_itself(self):
        self.assert_ok(self.tm("fastread", "on"))
        self.write_game_file("dinput8.dll", NEW_LOADER)
        self.assert_ok(self.tm("fastread", "off"))
        self.assertEqual(self.game_file("dinput8.dll"), NEW_LOADER)
        self.assertIsNone(self.game_file("dinput8_openplanet.dll"))

    # ./tm openplanet while fastread is on

    def test_openplanet_reinstall_keeps_the_shim(self):
        self.assert_ok(self.tm("fastread", "on"))
        self.assert_ok(self.tm("openplanet", self.openplanet_installer(NEW_LOADER)))
        self.assertTrue(self.is_shim("dinput8.dll"))
        self.assertEqual(self.game_file("dinput8_openplanet.dll"), NEW_LOADER)

    def test_openplanet_reinstall_rebuilds_a_missing_shim(self):
        self.assert_ok(self.tm("fastread", "on"))
        os.remove(os.path.join(self.data, "fastread", "dinput8.dll"))
        self.assert_ok(self.tm("openplanet", self.openplanet_installer(NEW_LOADER)))
        self.assertTrue(self.is_shim("dinput8.dll"))
        self.assertEqual(self.game_file("dinput8_openplanet.dll"), NEW_LOADER)

    def test_openplanet_install_without_fastread_leaves_the_loader_alone(self):
        self.assert_ok(self.tm("openplanet", self.openplanet_installer(NEW_LOADER)))
        self.assertEqual(self.game_file("dinput8.dll"), NEW_LOADER)
        self.assertIsNone(self.game_file("dinput8_openplanet.dll"))

    # failures leave the game directory untouched

    def test_on_without_mingw_fails_and_changes_nothing(self):
        os.remove(os.path.join(self.bin, "x86_64-w64-mingw32-gcc"))
        result = self.tm("fastread", "on")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("brew install mingw-w64", result.stderr)
        self.assertEqual(self.game_file("dinput8.dll"), LOADER)
        self.assertIsNone(self.game_file("dinput8_openplanet.dll"))

    def test_on_with_a_failing_build_changes_nothing(self):
        self.fake_tool("x86_64-w64-mingw32-gcc", FAILING_GCC)
        result = self.tm("fastread", "on")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("could not build", result.stderr)
        self.assertEqual(self.game_file("dinput8.dll"), LOADER)
        self.assertIsNone(self.game_file("dinput8_openplanet.dll"))

    def test_on_without_openplanet_fails(self):
        os.remove(os.path.join(self.game, "Openplanet.dll"))
        result = self.tm("fastread", "on")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Install Openplanet first", result.stderr)
        self.assertEqual(self.game_file("dinput8.dll"), LOADER)

    def test_on_without_a_loader_fails(self):
        os.remove(os.path.join(self.game, "dinput8.dll"))
        result = self.tm("fastread", "on")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("./tm openplanet", result.stderr)
        self.assertIsNone(self.game_file("dinput8.dll"))

    def test_on_when_the_loader_was_deleted_fails(self):
        self.assert_ok(self.tm("fastread", "on"))
        os.remove(os.path.join(self.game, "dinput8_openplanet.dll"))
        result = self.tm("fastread", "on")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("./tm openplanet", result.stderr)

    def test_rejects_an_unknown_argument(self):
        for args in (("fastread",), ("fastread", "maybe")):
            result = self.tm(*args)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("usage: ./tm fastread on|off", result.stderr)
        self.assertEqual(self.game_file("dinput8.dll"), LOADER)


class MarkerTest(unittest.TestCase):
    def test_the_shim_source_embeds_the_marker_tm_looks_for(self):
        with open(os.path.join(ROOT, "fastread", "fastread.c")) as f:
            self.assertIn(f'"{MARKER}"', f.read())


if __name__ == "__main__":
    unittest.main()
