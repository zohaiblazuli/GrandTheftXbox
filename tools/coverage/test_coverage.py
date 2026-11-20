"""Tests for gtx-coverage. Run: python -m unittest discover -s tools -t . -p "test_*.py" """
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import import_coverage as coverage  # noqa: E402


def report(name, pes):
    return {"root_name": name, "pe": pes}


def pe(path, imports, is_dll=False):
    return {"path": path, "is_dll": is_dll, "imports": imports, "delay_imports": {}}


GAME_A = report("GameA", [
    pe("a.exe", {"kernel32.dll": ["CreateFileW", "XMemAllocDefault"],
                 "kernelx.dll": ["XSystemGetDeviceType"],
                 "api-ms-win-core-synch-l1-2-0.dll": ["Sleep"],
                 "bink2w64.dll": ["BinkOpen"]}),
    pe("bink2w64.dll", {"kernel32.dll": ["CreateFileW"]}, is_dll=True),
])
GAME_B = report("GameB", [
    pe("b.exe", {"kernelx.dll": ["XSystemGetDeviceType", "XMemFree"],
                 "xgameruntime.dll": ["XGameRuntimeInitialize"]}),
])


class CoverageTests(unittest.TestCase):
    def setUp(self):
        self.cov = coverage.aggregate([GAME_A, GAME_B])
        self.by = {(r["dll"], r["function"]): r for r in self.cov["functions"]}

    def test_classes(self):
        self.assertEqual(self.by[("kernel32.dll", "CreateFileW")]["class"], "host")
        self.assertEqual(self.by[("kernel32.dll", "XMemAllocDefault")]["class"], "host_miss")
        self.assertEqual(self.by[("kernelx.dll", "XSystemGetDeviceType")]["class"], "xbox_only")
        self.assertEqual(self.by[("api-ms-win-core-synch-l1-2-0.dll", "Sleep")]["class"], "apiset")
        self.assertEqual(self.by[("bink2w64.dll", "BinkOpen")]["class"], "bundled")

    def test_ranking_needs_work_first_by_game_count(self):
        first = self.cov["functions"][0]
        self.assertEqual((first["dll"], first["function"]), ("kernelx.dll", "XSystemGetDeviceType"))
        self.assertEqual(first["games"], 2)
        seen_done = False
        for r in self.cov["functions"]:
            if r["class"] not in coverage.NEEDS_WORK:
                seen_done = True
            else:
                self.assertFalse(seen_done, "needs-work rows must come first")

    def test_pe_count_and_games(self):
        r = self.by[("kernel32.dll", "CreateFileW")]
        self.assertEqual(r["pe_count"], 2)
        self.assertEqual(r["games"], 1)

    def test_implemented_globs(self):
        pats = [("kernelx.dll", "XSystem*")]
        cov = coverage.aggregate([GAME_A, GAME_B], implemented=pats)
        by = {(r["dll"], r["function"]): r for r in cov["functions"]}
        self.assertEqual(by[("kernelx.dll", "XSystemGetDeviceType")]["class"], "implemented")
        self.assertEqual(by[("kernelx.dll", "XMemFree")]["class"], "xbox_only")

    def test_cli(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            for g in (GAME_A, GAME_B):
                (d / f"{g['root_name']}.json").write_text(json.dumps(g), encoding="utf-8")
            impl = d / "impl.txt"
            impl.write_text("# c\nxgameruntime.dll!XGame*\n", encoding="utf-8")
            out = d / "out"
            rc = coverage.main([str(d / "*.json"), "-o", str(out), "--implemented", str(impl)])
            self.assertEqual(rc, 0)
            md = (out / "coverage.md").read_text(encoding="utf-8")
            self.assertIn("kernelx.dll", md)
            data = json.loads((out / "coverage.json").read_text(encoding="utf-8"))
            self.assertEqual(data["totals"]["implemented"], 1)

    TRACE = ("INFO\txgameruntime.dll\tInitializeApiImplEx2\t0000\ta=1\n"
             "STUB\txgameruntime.dll\tXAsyncRun\t0000\t\n"
             "STUB\txgameruntime.dll\tXAsyncRun\t0000\t\n"
             "UNIMPL\txgameruntime.dll\tQueryApiImpl\t0000\tunknown interface {X}\n"
             "STUB\tkernelx.dll\tXMemFree\t0000\t\n"
             "garbage line\n")

    def test_parse_trace(self):
        self.assertEqual(coverage.parse_trace(self.TRACE), [
            ("STUB", "xgameruntime.dll", "XAsyncRun"),
            ("UNIMPL", "xgameruntime.dll", "QueryApiImpl"),
            ("STUB", "kernelx.dll", "XMemFree")])

    def test_trace_merges_and_ranks_first(self):
        cov = coverage.aggregate([GAME_A, GAME_B],
                                 traces={"GameB": coverage.parse_trace(self.TRACE)})
        by = {(r["dll"], r["function"]): r for r in cov["functions"]}
        self.assertEqual(by[("xgameruntime.dll", "XAsyncRun")]["class"], "runtime_stub")
        self.assertEqual(by[("xgameruntime.dll", "QueryApiImpl")]["class"], "runtime_unimpl")
        # Static import also seen at runtime keeps its class but gains the runtime mark.
        free = by[("kernelx.dll", "XMemFree")]
        self.assertEqual(free["class"], "xbox_only")
        self.assertEqual(free["runtime"], ["STUB"])
        self.assertEqual(free["games"], 1)
        # Every runtime-hit row outranks every static-only needs-work row.
        work = [r for r in cov["functions"] if r["class"] in coverage.NEEDS_WORK]
        hit = [bool(r["runtime_games"]) for r in work]
        self.assertEqual(hit, sorted(hit, reverse=True))
        self.assertEqual(cov["games"], ["GameA", "GameB"])

    def test_cli_trace_only(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            log = d / "gtx.log"
            log.write_text(self.TRACE, encoding="utf-8")
            out = d / "out"
            self.assertEqual(coverage.main(["--trace", f"Sample={log}", "-o", str(out)]), 0)
            md = (out / "coverage.md").read_text(encoding="utf-8")
            self.assertIn("`XAsyncRun` | runtime_stub | STUB", md)


if __name__ == "__main__":
    unittest.main()
