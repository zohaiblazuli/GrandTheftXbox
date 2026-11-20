"""Tests for gtx-triage. Run: python -m unittest discover -s tools -t . -p "test_*.py" """
import json
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import triage  # noqa: E402

SYS32 = Path(r"C:\Windows\System32")

GAME_CONFIG = """<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <Identity Name="Contoso.TestGame" Publisher="CN=Contoso" Version="1.0.0.0"/>
  <TitleId>ABCD1234</TitleId>
  <StoreId>9NBLGGH00000</StoreId>
  <ExecutableList>
    <Executable Name="TestGame.exe" TargetDeviceFamily="Scarlett" Id="Game"/>
    <Executable Name="TestGame_xb1.exe" TargetDeviceFamily="XboxOne" Id="GameXb1"/>
  </ExecutableList>
</Game>
"""

APPX = """<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10">
  <Identity Name="Contoso.TestGame" Publisher="CN=Contoso" Version="1.0.0.0"/>
  <Dependencies><TargetDeviceFamily Name="Windows.Xbox" MinVersion="10.0.0.0"/></Dependencies>
  <Applications><Application Id="App" Executable="TestGame.exe"/></Applications>
</Package>
"""


def make_dxbc(parts: list[tuple[bytes, bytes]]) -> bytes:
    """Build a minimal valid DXBC container with the given (fourcc, payload) parts."""
    header_len = 32 + 4 * len(parts)
    offsets, body, cur = [], b"", header_len
    for fourcc, payload in parts:
        offsets.append(cur)
        chunk = fourcc + struct.pack("<I", len(payload)) + payload
        body += chunk
        cur += len(chunk)
    total = header_len + len(body)
    head = b"DXBC" + b"\0" * 16 + struct.pack("<III", 1, total, len(parts))
    head += b"".join(struct.pack("<I", o) for o in offsets)
    return head + body


class TriageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="gtx-triage-"))
        game = cls.tmp / "TestGame"
        (game / "bin").mkdir(parents=True)
        (game / "shaders").mkdir()
        # Real host PEs stand in for game binaries.
        shutil.copy(SYS32 / "notepad.exe", game / "TestGame.exe")
        shutil.copy(SYS32 / "version.dll", game / "bin" / "helper.dll")
        (game / "MicrosoftGame.config").write_text(GAME_CONFIG, encoding="utf-8")
        (game / "AppxManifest.xml").write_text(APPX, encoding="utf-8")
        cls.dxil = make_dxbc([(b"DXIL", b"\0" * 16), (b"ISG1", b"\0" * 8)])
        (game / "shaders" / "ps.cso").write_bytes(cls.dxil)
        (game / "shaders" / "weird.bin").write_bytes(make_dxbc([(b"SHEX", b"\1" * 12)]))
        (game / "data.bin").write_bytes(b"\x00\x01\x02garbage" * 10 + cls.dxil + b"tail")
        (game / "readme.txt").write_text("hello\n")
        (game / "broken.dll").write_bytes(b"MZ" + b"\0" * 100)  # truncated PE
        cls.game = game
        cls.report = triage.triage(game)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def pe(self, rel):
        return next(p for p in self.report["pe"] if p["path"] == rel)

    def test_classification(self):
        types = {f["path"]: f["type"] for f in self.report["files"]}
        self.assertEqual(types["TestGame.exe"], "pe")
        self.assertEqual(types["shaders/ps.cso"], "dxbc")
        self.assertEqual(types["shaders/weird.bin"], "dxbc")  # magic beats extension
        self.assertEqual(types["MicrosoftGame.config"], "xml")
        self.assertEqual(types["readme.txt"], "ini_or_text")

    def test_pe_imports_grouped_lowercase(self):
        exe = self.pe("TestGame.exe")
        self.assertEqual(exe["machine"], "AMD64")
        self.assertFalse(exe["is_dll"])
        self.assertTrue(exe["imports"])
        for dll, funcs in exe["imports"].items():
            self.assertEqual(dll, dll.lower())
            self.assertEqual(funcs, sorted(set(funcs)))
        dll = self.pe("bin/helper.dll")
        self.assertTrue(dll["is_dll"])
        self.assertGreater(dll["export_count"], 0)

    def test_broken_pe_is_error_not_crash(self):
        self.assertTrue(any(e["file"] == "broken.dll" and e["stage"] == "pe"
                            for e in self.report["errors"]))

    def test_configs(self):
        cfgs = {c["path"]: c for c in self.report["configs"]}
        game = cfgs["MicrosoftGame.config"]
        self.assertEqual(game["titleId"], "ABCD1234")
        self.assertEqual(game["identity"]["Name"], "Contoso.TestGame")
        self.assertEqual(game["target_device_families"], ["Scarlett", "XboxOne"])
        self.assertEqual(len(game["executables"]), 2)
        appx = cfgs["AppxManifest.xml"]
        self.assertEqual(appx["target_device_families"], ["Windows.Xbox"])
        self.assertEqual(appx["executables"][0]["Executable"], "TestGame.exe")

    def test_shaders(self):
        sh = {s["path"]: s for s in self.report["shaders"]}
        self.assertEqual(sh["shaders/ps.cso"]["dxbc"]["parts"], ["DXIL", "ISG1"])
        self.assertEqual(sh["shaders/weird.bin"]["dxbc"]["parts"], ["SHEX"])
        self.assertNotIn("data.bin", sh)  # embedded blobs are counted on PEs only

    def test_embedded_scan(self):
        found = triage.scan_embedded_dxbc(self.game / "data.bin",
                                          (self.game / "data.bin").stat().st_size)
        self.assertEqual(len(found), 1)
        self.assertEqual([p["fourcc"] for p in found[0]["parts"]], ["DXIL", "ISG1"])

    def test_dll_string_scan(self):
        f = self.tmp / "strs.bin"
        f.write_bytes(b"\x00junk xgameruntime.dll\x00" + "XGameSave.DLL".encode("utf-16-le") + b"\x00\x00")
        self.assertEqual(triage.scan_dll_strings(f, f.stat().st_size),
                         ["xgameruntime.dll".replace("i", "i"), "xgamesave.dll"])
        exe = self.pe("TestGame.exe")
        self.assertNotIn("kernel32.dll", exe["dll_string_refs"])  # imported ones excluded

    def test_parse_dxbc_rejects_garbage(self):
        self.assertIsNone(triage.parse_dxbc(b"DXBC" + b"\xff" * 40))
        self.assertIsNone(triage.parse_dxbc(b"nope"))

    def test_cli_writes_outputs(self):
        out = self.tmp / "reports"
        self.assertEqual(triage.main([str(self.game), "-o", str(out)]), 0)
        self.assertEqual(triage.main([str(self.game), "-o", str(out), "--name", "Renamed"]), 0)
        self.assertTrue((out / "Renamed" / "triage.json").is_file())
        data = json.loads((out / "TestGame" / "triage.json").read_text(encoding="utf-8"))
        self.assertEqual(data["schema"], triage.SCHEMA_VERSION)
        md = (out / "TestGame" / "summary.md").read_text(encoding="utf-8")
        self.assertIn("ABCD1234", md)
        self.assertIn("kernel32.dll", md.lower())


if __name__ == "__main__":
    unittest.main()
