#!/usr/bin/env python3
"""GTX import coverage DB (T-005).

Aggregates triage.json files (from tools/triage) across games and ranks every imported
(dll, function) by how many games need it, so shim work is ordered by impact (Wine-style).

Each import is classified:
  bundled    DLL ships inside the game folder (loaded as-is, not our job first)
  host       host Windows DLL exports this function -> forward
  host_miss  DLL exists on host but lacks this export -> Xbox-specific addition, needs shim
  apiset     api-ms-* / ext-ms-* contract; resolved by host loader (export not checked)
  xbox_only  DLL does not exist on host -> needs a shim DLL
  implemented  listed in implemented.txt (overrides the above)

Usage: python tools/coverage/import_coverage.py reports/*/triage.json [-o reports/_coverage]
"""
from __future__ import annotations

import argparse
import collections
import fnmatch
import glob
import json
import os
import sys
from functools import lru_cache
from pathlib import Path

try:
    import pefile
except ImportError:  # pragma: no cover
    sys.exit("pefile is required: pip install -r requirements.txt")

REPO = Path(__file__).resolve().parents[2]
DEFAULT_IMPLEMENTED = Path(__file__).with_name("implemented.txt")
HOST_DIR = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
NEEDS_WORK = ("xbox_only", "host_miss")


@lru_cache(maxsize=None)
def host_exports(dll: str, host_dir: str) -> frozenset | None:
    """Exports of a host DLL, or None if the DLL is not on the host."""
    p = Path(host_dir) / dll
    if not p.is_file():
        return None
    try:
        pe = pefile.PE(str(p), fast_load=True)
        pe.parse_data_directories(
            directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
        exp = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
        names = set()
        if exp:
            for s in exp.symbols:
                if s.name:
                    names.add(s.name.decode("latin-1"))
                names.add(f"#{s.ordinal}")  # games may import by ordinal
        pe.close()
        return frozenset(names)
    except Exception:
        return frozenset()


def load_implemented(path: Path) -> list[tuple[str, str]]:
    """Lines like `kernelx.dll!CreateFileW` or `xgameruntime.dll!*`; '#' comments."""
    pats = []
    if path and path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if "!" in line:
                dll, fn = line.split("!", 1)
                pats.append((dll.strip().lower(), fn.strip()))
    return pats


def is_implemented(dll: str, fn: str, pats) -> bool:
    return any(fnmatch.fnmatchcase(dll, d) and fnmatch.fnmatchcase(fn, f) for d, f in pats)


def classify(dll: str, fn: str, bundled: set[str], host_dir: str, pats) -> str:
    if is_implemented(dll, fn, pats):
        return "implemented"
    if dll in bundled:
        return "bundled"
    if dll.startswith(("api-ms-", "ext-ms-")):
        return "apiset"
    exports = host_exports(dll, host_dir)
    if exports is None:
        return "xbox_only"
    return "host" if fn in exports else "host_miss"


def aggregate(reports: list[dict], host_dir: Path = HOST_DIR,
              implemented: list | None = None) -> dict:
    pats = implemented or []
    games = []
    # (dll, fn) -> {"games": set, "pe_count": int}
    funcs: dict = collections.defaultdict(lambda: {"games": set(), "pe_count": 0})
    bundled_by_game = {}
    for r in reports:
        game = r.get("root_name", "?")
        games.append(game)
        bundled = {Path(pe["path"]).name.lower() for pe in r.get("pe", []) if pe.get("is_dll")}
        bundled_by_game[game] = bundled
        for pe in r.get("pe", []):
            for table in ("imports", "delay_imports"):
                for dll, names in pe.get(table, {}).items():
                    for fn in names:
                        rec = funcs[(dll, fn)]
                        rec["games"].add(game)
                        rec["pe_count"] += 1

    rows = []
    for (dll, fn), rec in funcs.items():
        # bundled only if bundled in every game that imports it
        bundled_everywhere = all(dll in bundled_by_game[g] for g in rec["games"])
        cls = classify(dll, fn, {dll} if bundled_everywhere else set(), str(host_dir), pats)
        rows.append({"dll": dll, "function": fn, "class": cls,
                     "games": len(rec["games"]), "pe_count": rec["pe_count"],
                     "game_names": sorted(rec["games"])})
    rows.sort(key=lambda x: (x["class"] not in NEEDS_WORK, -x["games"], -x["pe_count"],
                             x["dll"], x["function"]))

    dlls: dict = {}
    for row in rows:
        d = dlls.setdefault(row["dll"], {"dll": row["dll"], "functions": 0, "games": set(),
                                         "classes": collections.Counter()})
        d["functions"] += 1
        d["games"].update(row["game_names"])
        d["classes"][row["class"]] += 1
    dll_rows = sorted(
        ({"dll": d["dll"], "functions": d["functions"], "games": len(d["games"]),
          "classes": dict(d["classes"])} for d in dlls.values()),
        key=lambda d: (-sum(d["classes"].get(c, 0) for c in NEEDS_WORK), -d["games"], d["dll"]))

    totals = collections.Counter(r["class"] for r in rows)
    return {"games": sorted(games), "totals": dict(totals), "dlls": dll_rows, "functions": rows}


def render(cov: dict, top: int = 200) -> str:
    t = cov["totals"]
    total = sum(t.values()) or 1
    lines = ["# Import coverage", "",
             f"Games: {len(cov['games'])} ({', '.join(cov['games'])})", "",
             "| Class | Functions | % |", "|---|---|---|"]
    for k in ("xbox_only", "host_miss", "implemented", "host", "apiset", "bundled"):
        lines.append(f"| {k} | {t.get(k, 0)} | {100 * t.get(k, 0) / total:.1f} |")
    lines += ["", "## DLLs (most shim work first)", "",
              "| DLL | Games | Functions | xbox_only | host_miss | implemented | host | apiset | bundled |",
              "|---|---|---|---|---|---|---|---|---|"]
    for d in cov["dlls"]:
        c = d["classes"]
        lines.append(f"| {d['dll']} | {d['games']} | {d['functions']} | " + " | ".join(
            str(c.get(k, 0)) for k in
            ("xbox_only", "host_miss", "implemented", "host", "apiset", "bundled")) + " |")
    todo = [r for r in cov["functions"] if r["class"] in NEEDS_WORK]
    lines += ["", f"## Top {min(top, len(todo))} unimplemented functions (of {len(todo)})", "",
              "| # | DLL | Function | Class | Games | PEs |", "|---|---|---|---|---|---|"]
    for i, r in enumerate(todo[:top], 1):
        lines.append(f"| {i} | {r['dll']} | `{r['function']}` | {r['class']} | "
                     f"{r['games']} | {r['pe_count']} |")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("reports", nargs="+", help="triage.json files or globs")
    ap.add_argument("-o", "--out", type=Path, default=REPO / "reports" / "_coverage")
    ap.add_argument("--implemented", type=Path, default=DEFAULT_IMPLEMENTED)
    ap.add_argument("--host-dir", type=Path, default=HOST_DIR)
    ap.add_argument("--top", type=int, default=200)
    args = ap.parse_args(argv)

    paths = sorted({p for pat in args.reports for p in (glob.glob(pat) or [pat])})
    reports = [json.loads(Path(p).read_text(encoding="utf-8")) for p in paths]
    cov = aggregate(reports, args.host_dir, load_implemented(args.implemented))
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "coverage.json").write_text(json.dumps(cov, indent=1), encoding="utf-8")
    (args.out / "coverage.md").write_text(render(cov, args.top), encoding="utf-8")
    print(f"{len(reports)} reports, {len(cov['functions'])} functions: {cov['totals']} -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
