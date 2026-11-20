#!/usr/bin/env python3
"""GTX dump triage (T-001).

Walks an already-decrypted game folder and reports:
  * file-type histogram (classified by magic bytes, not extension)
  * every PE's imports grouped by DLL (normal + delay-load), exports, sections
  * MicrosoftGame.config / AppxManifest.xml identity and executables
  * shader containers (DXBC/DXIL standalone files and blobs embedded in PEs)

Output: <out>/<folder-name>/triage.json and summary.md.
Read-only: never modifies the input folder. Does not decrypt anything (D-002).
"""
from __future__ import annotations

import argparse
import collections
import datetime as _dt
import json
import mmap
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

try:
    import pefile
except ImportError:  # pragma: no cover
    sys.exit("pefile is required: pip install pefile")

SCHEMA_VERSION = 1
HEADER_PEEK = 0x210  # enough for MZ, DXBC, XVD ("msft-xvd" at 0x200)
EMBED_SCAN_MAX = 512 * 1024 * 1024  # skip embedded-shader scan above this size

# (offset, magic bytes, type name)
MAGICS: list[tuple[int, bytes, str]] = [
    (0, b"MZ", "pe"),
    (0, b"DXBC", "dxbc"),
    (0, b"\x03\x02\x23\x07", "spirv"),
    (0, b"DDS ", "dds"),
    (0, b"PK\x03\x04", "zip"),
    (0, b"MSCF", "cab"),
    (0, b"RIFF", "riff"),
    (0, b"OggS", "ogg"),
    (0, b"BKHD", "wwise_bank"),
    (0, b"AKPK", "wwise_pck"),
    (0, b"FSB5", "fmod_bank"),
    (0, b"\x89PNG", "png"),
    (0, b"\xff\xd8\xff", "jpeg"),
    (0, b"UnityFS", "unity_bundle"),
    (0, b"\x1f\x8b", "gzip"),
    (0, b"\x28\xb5\x2f\xfd", "zstd"),
    (0, b"BIK", "bink"),
    (0, b"KB2", "bink2"),
    (0, b"SQLite format 3", "sqlite"),
    (0x200, b"msft-xvd", "xvd"),
]
TEXT_TYPES = {"xml", "json", "ini_or_text"}
SHADER_EXTS = {".cso", ".dxil", ".sb", ".spv", ".pso", ".fxo", ".shader", ".shaders"}
CONFIG_NAMES = {"microsoftgame.config", "appxmanifest.xml"}


# --------------------------------------------------------------------------- classify
def classify(head: bytes) -> str:
    for off, magic, name in MAGICS:
        if head[off:off + len(magic)] == magic:
            return name
    stripped = head.lstrip(b"\xef\xbb\xbf\xff\xfe\x00 \t\r\n")
    if stripped.startswith(b"<?xml") or stripped.startswith(b"<"):
        return "xml"
    if stripped[:1] in (b"{", b"["):
        return "json"
    if head and all(32 <= b < 127 or b in (9, 10, 13) for b in head[:256]):
        return "ini_or_text"
    return "unknown"


# --------------------------------------------------------------------------- DXBC
def parse_dxbc(buf, offset: int = 0) -> dict | None:
    """Parse a DXBC container header at `offset`. Returns None if not plausible."""
    if buf[offset:offset + 4] != b"DXBC" or len(buf) < offset + 32:
        return None
    version, total, nparts = struct.unpack_from("<III", buf, offset + 20)
    if nparts == 0 or nparts > 64 or total < 32 or offset + total > len(buf):
        return None
    if len(buf) < offset + 32 + 4 * nparts:
        return None
    parts = []
    for i in range(nparts):
        (poff,) = struct.unpack_from("<I", buf, offset + 32 + 4 * i)
        if poff + 8 > total:
            return None
        fourcc = bytes(buf[offset + poff:offset + poff + 4])
        (psize,) = struct.unpack_from("<I", buf, offset + poff + 4)
        parts.append({"fourcc": fourcc.decode("latin-1"), "size": psize})
    return {"offset": offset, "size": total, "version": version, "parts": parts}


def scan_embedded_dxbc(path: Path, size: int) -> list[dict]:
    if size < 32 or size > EMBED_SCAN_MAX:
        return []
    found = []
    with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
        pos = mm.find(b"DXBC")
        while pos != -1:
            info = parse_dxbc(mm, pos)
            if info:
                found.append(info)
                pos = mm.find(b"DXBC", pos + info["size"])
            else:
                pos = mm.find(b"DXBC", pos + 4)
    return found


# --------------------------------------------------------------------------- DLL strings
_DLL_ASCII = re.compile(rb"[A-Za-z0-9_.\-]{2,64}\.dll", re.IGNORECASE)
_DLL_UTF16 = re.compile(rb"(?<![A-Za-z0-9_.\-])(?:[A-Za-z0-9_.\-]\x00){2,64}\.\x00[dD]\x00[lL]\x00[lL]\x00")


def scan_dll_strings(path: Path, size: int) -> list[str]:
    """DLL names referenced as strings (ASCII or UTF-16LE): catches runtime LoadLibrary
    targets that never appear in import tables (e.g. XGameRuntime via xgameruntime.lib)."""
    if size > EMBED_SCAN_MAX:
        return []
    names = set()
    with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
        for m in _DLL_ASCII.finditer(mm):
            names.add(m.group().decode("latin-1").lower())
        for m in _DLL_UTF16.finditer(mm):
            names.add(m.group().decode("utf-16-le").lower())
    return sorted(n for n in names if not n.startswith("."))


# --------------------------------------------------------------------------- PE
MACHINES = {0x8664: "AMD64", 0x14C: "I386", 0xAA64: "ARM64", 0x1C4: "ARMNT"}


def _imp_name(imp) -> str:
    if imp.name:
        return imp.name.decode("latin-1")
    return f"#{imp.ordinal}"


def parse_pe(path: Path) -> dict:
    pe = pefile.PE(str(path), fast_load=True)
    try:
        pe.parse_data_directories(directories=[
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_TLS"],
        ])
        fh, oh = pe.FILE_HEADER, pe.OPTIONAL_HEADER
        imports: dict[str, list[str]] = {}
        delay: dict[str, list[str]] = {}
        for attr, target in (("DIRECTORY_ENTRY_IMPORT", imports),
                             ("DIRECTORY_ENTRY_DELAY_IMPORT", delay)):
            for entry in getattr(pe, attr, []) or []:
                dll = entry.dll.decode("latin-1").lower()
                target.setdefault(dll, []).extend(_imp_name(i) for i in entry.imports)
        exports = []
        exp = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
        if exp:
            exports = [s.name.decode("latin-1") if s.name else f"#{s.ordinal}" for s in exp.symbols]
        return {
            "machine": MACHINES.get(fh.Machine, hex(fh.Machine)),
            "is_dll": bool(fh.Characteristics & 0x2000),
            "subsystem": pefile.SUBSYSTEM_TYPE.get(oh.Subsystem, oh.Subsystem),
            "image_base": hex(oh.ImageBase),
            "entry_rva": hex(oh.AddressOfEntryPoint),
            "timestamp": fh.TimeDateStamp,
            "has_tls": hasattr(pe, "DIRECTORY_ENTRY_TLS"),
            "sections": [
                {"name": s.Name.rstrip(b"\0").decode("latin-1"),
                 "vsize": s.Misc_VirtualSize, "rawsize": s.SizeOfRawData,
                 "entropy": round(s.get_entropy(), 2)}
                for s in pe.sections
            ],
            "imports": {k: sorted(set(v)) for k, v in sorted(imports.items())},
            "delay_imports": {k: sorted(set(v)) for k, v in sorted(delay.items())},
            "export_count": len(exports),
            "exports_sample": exports[:50],
        }
    finally:
        pe.close()


# --------------------------------------------------------------------------- configs
def _strip_ns(tag: str) -> str:
    return tag.split("}", 1)[-1]


def parse_config(path: Path) -> dict:
    root = ET.parse(path).getroot()
    out: dict = {"root": _strip_ns(root.tag), "attributes": dict(root.attrib)}
    executables, devices = [], set()
    for el in root.iter():
        tag = _strip_ns(el.tag)
        attrs = {_strip_ns(k): v for k, v in el.attrib.items()}
        if tag == "Identity":
            out["identity"] = attrs
        elif tag in ("TitleId", "StoreId", "MSAAppId"):
            out[tag[0].lower() + tag[1:]] = (el.text or "").strip()
        elif tag == "Executable":
            executables.append(attrs)
            if "TargetDeviceFamily" in attrs:
                devices.add(attrs["TargetDeviceFamily"])
        elif tag == "Application" and "Executable" in attrs:
            executables.append(attrs)
        elif tag == "TargetDeviceFamily" and "Name" in attrs:
            devices.add(attrs["Name"])
    out["executables"] = executables
    out["target_device_families"] = sorted(devices)
    out["elements"] = sorted({_strip_ns(e.tag) for e in root.iter()})
    return out


# --------------------------------------------------------------------------- walk
def triage(root: Path, scan_embedded: bool = True) -> dict:
    root = root.resolve()
    files, pes, configs, shaders, errors = [], [], [], [], []
    type_hist: collections.Counter = collections.Counter()
    type_bytes: collections.Counter = collections.Counter()

    for dirpath, _dirs, names in os.walk(root):
        for name in sorted(names):
            p = Path(dirpath) / name
            rel = p.relative_to(root).as_posix()
            try:
                size = p.stat().st_size
                with open(p, "rb") as f:
                    head = f.read(HEADER_PEEK)
            except OSError as e:
                errors.append({"file": rel, "stage": "read", "error": str(e)})
                continue
            ftype = classify(head)
            type_hist[ftype] += 1
            type_bytes[ftype] += size
            files.append({"path": rel, "size": size, "type": ftype})

            if ftype == "pe":
                try:
                    info = parse_pe(p)
                    info["path"] = rel
                    imported = set(info["imports"]) | set(info["delay_imports"])
                    info["dll_string_refs"] = [d for d in scan_dll_strings(p, size)
                                               if d not in imported]
                    if scan_embedded:
                        emb = scan_embedded_dxbc(p, size)
                        info["embedded_dxbc_count"] = len(emb)
                        info["embedded_dxbc_parts"] = dict(collections.Counter(
                            part["fourcc"] for b in emb for part in b["parts"]))
                    pes.append(info)
                except Exception as e:  # pefile raises many types
                    errors.append({"file": rel, "stage": "pe", "error": repr(e)})

            if name.lower() in CONFIG_NAMES:
                try:
                    cfg = parse_config(p)
                    cfg["path"] = rel
                    configs.append(cfg)
                except Exception as e:
                    errors.append({"file": rel, "stage": "config", "error": repr(e)})

            ext = p.suffix.lower()
            if ftype in ("dxbc", "spirv") or ext in SHADER_EXTS:
                entry = {"path": rel, "size": size, "type": ftype, "ext": ext}
                if ftype == "dxbc":
                    with open(p, "rb") as f:
                        info = parse_dxbc(f.read(min(size, 1 << 20)))
                    if info:
                        entry["dxbc"] = {"version": info["version"],
                                         "parts": [x["fourcc"] for x in info["parts"]]}
                shaders.append(entry)

    return {
        "schema": SCHEMA_VERSION,
        "tool": "gtx-triage",
        "generated": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
        "root_name": root.name,
        "totals": {"files": len(files), "bytes": sum(f["size"] for f in files),
                   "pe": len(pes), "configs": len(configs), "shaders": len(shaders),
                   "errors": len(errors)},
        "file_types": {k: {"count": type_hist[k], "bytes": type_bytes[k]}
                       for k in sorted(type_hist, key=lambda k: -type_bytes[k])},
        "configs": configs,
        "pe": pes,
        "shaders": shaders,
        "errors": errors,
        "files": files,
    }


# --------------------------------------------------------------------------- report
def _fmt_bytes(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"


def render_summary(r: dict) -> str:
    t = r["totals"]
    lines = [f"# Triage: {r['root_name']}", "",
             f"Generated {r['generated']} by gtx-triage (schema {r['schema']}).", "",
             f"- Files: {t['files']} ({_fmt_bytes(t['bytes'])})",
             f"- PE images: {t['pe']} · configs: {t['configs']} · shader candidates: {t['shaders']}"
             f" · errors: {t['errors']}", "", "## File types", "",
             "| Type | Count | Size |", "|---|---|---|"]
    for k, v in r["file_types"].items():
        lines.append(f"| {k} | {v['count']} | {_fmt_bytes(v['bytes'])} |")

    for cfg in r["configs"]:
        lines += ["", f"## Config `{cfg['path']}`", ""]
        if "identity" in cfg:
            lines.append(f"- Identity: `{cfg['identity']}`")
        for key in ("titleId", "storeId", "mSAAppId"):
            if cfg.get(key):
                lines.append(f"- {key}: `{cfg[key]}`")
        if cfg["target_device_families"]:
            lines.append(f"- Target devices: {', '.join(cfg['target_device_families'])}")
        for ex in cfg["executables"]:
            lines.append(f"- Executable: `{ex}`")

    dll_funcs: dict[str, set] = collections.defaultdict(set)
    dll_users: collections.Counter = collections.Counter()
    for pe in r["pe"]:
        for table in (pe["imports"], pe["delay_imports"]):
            for dll, funcs in table.items():
                dll_funcs[dll].update(funcs)
        for dll in set(pe["imports"]) | set(pe["delay_imports"]):
            dll_users[dll] += 1
    if dll_funcs:
        lines += ["", "## Imported DLLs (all PEs)", "",
                  "| DLL | Distinct functions | Importing PEs |", "|---|---|---|"]
        for dll in sorted(dll_funcs, key=lambda d: (-len(dll_funcs[d]), d)):
            lines.append(f"| {dll} | {len(dll_funcs[dll])} | {dll_users[dll]} |")

    refs: dict[str, set] = collections.defaultdict(set)
    for pe in r["pe"]:
        for d in pe.get("dll_string_refs", []):
            refs[d].add(pe["path"])
    if refs:
        lines += ["", "## DLL names referenced as strings (possible runtime loads)", "",
                  "| DLL | Referencing PEs |", "|---|---|"]
        for d in sorted(refs):
            lines.append(f"| {d} | {len(refs[d])} |")

    if r["pe"]:
        lines += ["", "## PE images", "",
                  "| Path | Kind | Machine | Subsystem | DLLs | Imports | Exports | Embedded DXBC |",
                  "|---|---|---|---|---|---|---|---|"]
        for pe in sorted(r["pe"], key=lambda p: p["path"]):
            n_imp = sum(len(v) for v in pe["imports"].values()) + \
                sum(len(v) for v in pe["delay_imports"].values())
            n_dll = len(set(pe["imports"]) | set(pe["delay_imports"]))
            kind = "DLL" if pe["is_dll"] else "EXE"
            lines.append(f"| `{pe['path']}` | {kind} | {pe['machine']} | {pe['subsystem']} | "
                         f"{n_dll} | {n_imp} | {pe['export_count']} | "
                         f"{pe.get('embedded_dxbc_count', '-')} |")

    if r["shaders"]:
        parts: collections.Counter = collections.Counter()
        exts: collections.Counter = collections.Counter()
        for s in r["shaders"]:
            exts[s["ext"] or "(none)"] += 1
            for p in s.get("dxbc", {}).get("parts", []):
                parts[p] += 1
        lines += ["", "## Shader candidates", "",
                  f"- By extension: {dict(exts.most_common())}",
                  f"- DXBC part FourCCs: {dict(parts.most_common()) or 'none'}"]

    if r["errors"]:
        lines += ["", "## Errors", ""]
        lines += [f"- `{e['file']}` ({e['stage']}): {e['error']}" for e in r["errors"][:50]]
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("game_dir", type=Path, help="decrypted game folder (outside the repo)")
    ap.add_argument("-o", "--out", type=Path,
                    default=Path(__file__).resolve().parents[2] / "reports",
                    help="output root (default: <repo>/reports, gitignored)")
    ap.add_argument("--name", help="report name (default: game folder name)")
    ap.add_argument("--no-embedded", action="store_true",
                    help="skip scanning PEs for embedded DXBC shader blobs")
    args = ap.parse_args(argv)
    if not args.game_dir.is_dir():
        ap.error(f"not a directory: {args.game_dir}")

    report = triage(args.game_dir, scan_embedded=not args.no_embedded)
    if args.name:
        report["root_name"] = args.name
    out_dir = args.out / report["root_name"]
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "triage.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    (out_dir / "summary.md").write_text(render_summary(report), encoding="utf-8")
    t = report["totals"]
    print(f"{t['files']} files, {t['pe']} PEs, {t['configs']} configs, "
          f"{t['shaders']} shader candidates, {t['errors']} errors -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
