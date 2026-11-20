#!/usr/bin/env python3
"""Extract the XGameRuntime dispatch table from the public PC GDK (T-012).

The GDK's static `xgameruntime.lib` implements each public API (XUserGetId, ...) as a thin
wrapper:  QueryApiImpl(&GUID, &GUID, &obj); obj->vtbl[slot](obj, args...); obj->Release().
To replace xgameruntime.dll we need, per API: interface GUID + vtable slot. This tool reads a
`dumpbin /disasm /relocations` listing of the lib's main.obj and recovers that mapping, then
joins return types from the public headers.

Inputs come from the public NuGet package Microsoft.GDK.Windows (see TOOLS.md). Usage:
  python tools/xgameruntime/extract_api_table.py --gdk <...>/native/261000/windows -o table.json
(dumps main.obj via lib.exe + dumpbin.exe found through vswhere.)
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

LABEL = re.compile(r"^([A-Za-z_?$@][^\s:(]*)(?: \(.*\))?:$")
GUID_SYM = re.compile(r"_GUID_([0-9a-f]{8})_([0-9a-f]{4})_([0-9a-f]{4})_([0-9a-f]{4})_([0-9a-f]{12})")
VCALL = re.compile(r"mov\s+rax,qword ptr \[rax(?:\+([0-9A-F]+)h)?\]")
API_NAME = re.compile(r"^X[A-Z][A-Za-z0-9]+$")


def guid_str(m: re.Match) -> str:
    return "-".join(m.groups()).upper()


def parse_disasm(text: str) -> dict[str, dict]:
    """Map API name -> {"guid", "slot"} from dumpbin disassembly of main.obj."""
    funcs: dict[str, list[str]] = {}
    cur = None
    for line in text.splitlines():
        m = LABEL.match(line)
        if m:
            cur = m.group(1)
            funcs[cur] = []
        elif cur is not None and line.startswith("  0"):
            funcs[cur].append(line)
    table = {}
    for name, body in funcs.items():
        if not API_NAME.match(name):
            continue
        # QueryApiImpl(provider = rcx, iid = rdx, out = r8). Often `mov rdx,rcx` (same GUID).
        regs: dict[str, str | None] = {"rcx": None, "rdx": None}
        provider = iid = None
        after_query = False
        for line in body:
            g = GUID_SYM.search(line)
            if g and "lea" in line:
                reg = "rcx" if "lea         rcx" in line else "rdx" if "lea         rdx" in line else None
                if reg:
                    regs[reg] = guid_str(g)
            elif re.search(r"mov\s+rdx,rcx\b", line):
                regs["rdx"] = regs["rcx"]
            if "call" in line and "QueryApiImpl" in line:
                provider, iid = regs["rcx"], regs["rdx"]
                after_query = True
                continue
            if after_query:
                v = VCALL.search(line)
                if v:
                    off = int(v.group(1) or "0", 16)
                    if off >= 0x18:  # skip IUnknown (QI/AddRef/Release)
                        table[name] = {"guid": iid, "provider": provider, "slot": off // 8}
                        break
    return table


SIG = re.compile(
    r"\b(STDAPI(?:_\(\s*([^)]+?)\s*\))?|STDAPI_?\b)\s+(X[A-Za-z0-9]+)\s*\(([^;]*?)\)\s*(?:noexcept\s*)?;",
    re.S)


def parse_headers(include: Path) -> dict[str, dict]:
    """API name -> {"ret": C return type, "params": raw param text} from public headers."""
    out = {}
    for h in sorted(include.glob("*.h")):
        text = h.read_text(encoding="utf-8", errors="replace")
        for m in SIG.finditer(text):
            ret = m.group(2) or "HRESULT"
            params = " ".join(m.group(4).split())
            out[m.group(3)] = {"ret": ret.strip(), "params": params, "header": h.name}
    return out


def find_vs_tool(tool: str) -> Path:
    vswhere = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
    root = subprocess.run([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"],
                          capture_output=True, text=True, check=True).stdout.strip()
    hits = sorted(Path(root, "VC", "Tools", "MSVC").glob(f"*/bin/Hostx64/x64/{tool}"))
    if not hits:
        sys.exit(f"{tool} not found under {root}")
    return hits[-1]


def disasm_lib(lib: Path) -> str:
    libexe, dumpbin = find_vs_tool("lib.exe"), find_vs_tool("dumpbin.exe")
    members = subprocess.run([str(libexe), "-nologo", "-list", str(lib)],
                             capture_output=True, text=True, check=True).stdout.split()
    main = next(m for m in members if m.lower().endswith("\\main.obj"))
    with tempfile.TemporaryDirectory() as d:
        obj = Path(d) / "main.obj"
        subprocess.run([str(libexe), "-nologo", f"-extract:{main}", f"-out:{obj}", str(lib)],
                       capture_output=True, check=True)
        return subprocess.run([str(dumpbin), "-nologo", "-disasm", "-relocations", str(obj)],
                              capture_output=True, text=True, check=True).stdout


def build(gdk: Path, disasm_text: str | None = None) -> dict:
    text = disasm_text if disasm_text is not None else disasm_lib(gdk / "lib" / "x64" / "xgameruntime.lib")
    dispatch = parse_disasm(text)
    sigs = parse_headers(gdk / "include")
    apis = []
    for name in sorted(dispatch):
        d = dispatch[name]
        s = sigs.get(name, {})
        apis.append({"name": name, "guid": d["guid"], "provider": d["provider"], "slot": d["slot"],
                     "ret": s.get("ret", "HRESULT"), "params": s.get("params"),
                     "header": s.get("header")})
    interfaces: dict[str, int] = {}
    for a in apis:
        interfaces[a["guid"]] = max(interfaces.get(a["guid"], 0), a["slot"])
    return {"source": "Microsoft.GDK.Windows xgameruntime.lib (public NuGet)",
            "gdk_dir": "/".join(gdk.parts[-2:]), "api_count": len(apis), "interface_count": len(interfaces),
            "interfaces": [{"guid": g, "max_slot": s} for g, s in sorted(interfaces.items())],
            "apis": apis}


STRUCT_RETURNS = {"XSystemAnalyticsInfo"}  # >8 bytes: returned via hidden pointer (2nd arg)


def ret_kind(ret: str) -> str:
    if ret == "HRESULT":
        return "Hr"
    if ret == "void":
        return "Void"
    if ret in STRUCT_RETURNS:
        return "Struct"
    return "Zero"  # bool / size_t / ints / enums / <=8-byte structs: returned in rax


def render_inc(table: dict) -> str:
    """C++ X-macro include consumed by src/shims/xgameruntime."""
    lines = ["// GENERATED by tools/xgameruntime/extract_api_table.py — do not edit.",
             f"// Source: {table['source']} ({table['gdk_dir']}). "
             f"{table['api_count']} APIs, {table['interface_count']} interfaces.",
             "// GTX_XGR_IFACE(index, guid_string, slot_count)",
             "// GTX_XGR_API(name, iface_index, slot, kind)  kind: Hr|Void|Zero|Struct", ""]
    index = {i["guid"]: n for n, i in enumerate(table["interfaces"])}
    lines.append("#ifdef GTX_XGR_IFACE")
    for n, i in enumerate(table["interfaces"]):
        lines.append(f'GTX_XGR_IFACE({n}, "{i["guid"]}", {i["max_slot"] + 1})')
    lines += ["#endif", "", "#ifdef GTX_XGR_API"]
    for a in table["apis"]:
        lines.append(f'GTX_XGR_API({a["name"]}, {index[a["guid"]]}, {a["slot"]}, {ret_kind(a["ret"])})')
    lines += ["#endif", ""]
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--gdk", type=Path, required=True,
                    help="Microsoft.GDK.Windows .../native/<ver>/windows directory")
    ap.add_argument("-o", "--out", type=Path, required=True, help="JSON table output")
    ap.add_argument("--inc", type=Path, help="also write the C++ X-macro include here")
    args = ap.parse_args(argv)
    table = build(args.gdk)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(table, indent=1) + "\n", encoding="utf-8")
    if args.inc:
        args.inc.parent.mkdir(parents=True, exist_ok=True)
        args.inc.write_text(render_inc(table), encoding="utf-8")
    print(f"{table['api_count']} APIs over {table['interface_count']} interfaces -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
