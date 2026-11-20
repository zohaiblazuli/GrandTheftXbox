# GrandTheftXbox (GTX)

**A compatibility layer for running Xbox Series X|S and Xbox One games on Windows PCs. It works like Wine or Proton: the game runs directly on the PC, and its Xbox-specific pieces are replaced, not emulated.**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Status: early development](https://img.shields.io/badge/status-early%20development-orange)
![Platform: Windows x64](https://img.shields.io/badge/platform-Windows%20x64-lightgrey)

> **Status:** early development, between Milestones 0 and 1. No console game runs yet. What works today:
> analysis tooling that maps what a game needs, a launcher, and a replacement `xgameruntime.dll`
> that Microsoft's own GDK runtime library loads and talks to. A GDK sample program runs on it.

---

## Why a translation layer, not an emulator?

Modern Xbox consoles are close relatives of a Windows PC:

| | Xbox Series X\|S | Typical gaming PC |
|---|---|---|
| CPU | AMD Zen 2, **x86-64** | x86-64 |
| OS | Windows-derived "GameOS" | Windows |
| Executables | **PE** (`.exe`/`.dll`) | PE |
| Graphics API | D3D12.X (console variant) | D3D12 |
| GPU | AMD RDNA2 | Various |

The game's CPU code already runs on any modern x86-64 processor, so GTX doesn't emulate or
recompile it. It replaces only the parts that are specific to the console:

- **System libraries** that exist only on Xbox (kernel, WinRT and GDK runtime APIs).
- **Graphics**: D3D12.X calls are translated to PC D3D12.
- **GPU shaders**: the console's precompiled shaders are recompiled for PC GPUs and cached.
- **Storage**: DirectStorage and Xbox-specific decompression (zlib, BCPack).
- **Audio, input and services**: XAudio2, controllers, and local stand-ins for online services.

## Architecture

```
Game folder (supplied by the user)
  └─► [1] Loader ─ map the game's .exe, resolve imports, redirect Xbox-only DLLs
         ├─ Xbox-only DLL ───────► [2] GTX shim DLL (implement, or forward to Windows)
         ├─ DLL bundled with game ─► load as-is
         └─ standard Win32 DLL ───► real Windows DLL
  Game code runs natively on the host CPU
     ├─ D3D12.X ──────► [3] D3D12.X → PC D3D12   (+ shader recompiler → DXIL, cached)
     ├─ file I/O ─────► [4] virtual file system + DirectStorage / BCPack / zlib
     └─ audio / input ─► [5] XAudio2, GameInput, local service stubs
```

Design principles:

- **Trace first.** Every replacement function logs calls to the parts it doesn't implement yet,
  and a coverage database ranks the remaining work by how many games need each function.
- **Forward before reimplementing.** If Windows or the public PC GDK already has an API, use it.
- **No hard-coded per-game hacks.** Game-specific settings live in data files, not in core code.

## Roadmap

| Milestone | Goal | State |
|---|---|---|
| **M0** | Foundations: analysis tooling, toolchain, project layout | 🟡 waiting on the first real game dump |
| **M1** | Loader + first pixels: a GDK sample runs on GTX's replacement libraries and clears the screen with PC D3D12 | 🟡 started (launcher + `xgameruntime` shim) |
| **M2** | A 2D indie game reaches gameplay with input and audio | ⚪ |
| **M3** | A 3D Xbox One-era game is playable, with shader translation working | ⚪ |
| **M4** | A game built specifically for Series X\|S boots (Series-only graphics path, BCPack decoding) | ⚪ |

## What's in the repo today

```
tools/triage/         Dump triage: inventory a game folder (Python)
tools/coverage/       Coverage database: static imports + runtime traces across games (Python)
tools/xgameruntime/   Extracts the XGameRuntime API table from Microsoft's public PC GDK (Python)
src/common/           Native trace logger used by every replacement library (C++20)
src/shims/xgameruntime/  Replacement xgameruntime.dll (all 396 public APIs routed; XAsync/XTaskQueue implemented)
src/launcher/         gtx-launch: starts a game with GTX's replacement DLLs preloaded
tests/                Native tests (CTest)
```

### `src/shims/xgameruntime`: the GDK runtime, replaced

Games built with Microsoft's GDK link a small static library that loads `xgameruntime.dll` and
reaches every API (`XUserGetId`, `XGameSave…`, `XSystem…`) through `QueryApiImpl`, which returns
interfaces identified by GUID, plus a vtable slot per function. GTX's replacement DLL implements
that contract:

- The dispatch table (396 APIs across 34 interfaces) is **generated** from Microsoft's public PC
  GDK by `tools/xgameruntime/extract_api_table.py`, so it tracks new GDK releases.
- Every API starts out as a stub that logs its first call and returns a safe failure. Real
  implementations replace stubs one at a time (`impl.cpp`, `xtask.cpp`).
- Implemented so far: the full **XAsync + XTaskQueue** async framework that most GDK APIs are
  built on (`xtask.cpp`), plus system and identity basics (`XSystemGetAnalyticsInfo`,
  `XSystemGetConsoleId`, `XSystemGetXboxLiveSandboxId`, `XGameGetXboxTitleId`, `XThread*`).
- Tested end to end: test programs link Microsoft's real `xgameruntime.lib`, and their calls arrive in GTX's DLL.
- **Checked against the real thing:** the same test suite also runs against Microsoft's installed
  runtime, and GTX must behave identically. This is how details like termination order,
  cancellation and result-consumption rules were pinned down.
- Microsoft's `SimpleTriangleDesktop` sample runs on GTX's runtime with no unimplemented calls.

### `gtx-launch`: run something on GTX

```bash
gtx-launch [--shims <dir>] [--wait-ms N] <game.exe> [args...]
```

Starts the program suspended, loads GTX's replacement DLLs into it, then lets it run. The
Windows loader reuses an already-loaded DLL of the same name, so the game ends up talking to
GTX instead of the system copy. Set `GTX_LOG=<file>` to record every call that hits a stub.

### `tools/triage`: what does this game need?

Point it at a game folder. It checks each file's actual contents, not just its extension, and reports:

- every executable and DLL, with the functions it imports from each DLL (including delay-loaded ones) and what it exports
- DLL names that appear only as text inside the binaries, which catches libraries loaded at runtime that never show up in import tables
- the game's `MicrosoftGame.config` / `AppxManifest.xml`: identity, title ID, executables and target devices
- shader containers (DXBC/DXIL), both as separate files and embedded inside executables, including what type of shader code they hold
- a breakdown of file types by count and size

Output is `triage.json` plus a readable `summary.md`, written to `reports/<name>/`.

### `tools/coverage`: what should we build first?

Combines triage reports from several games and sorts every imported function into one of these groups:

| Class | Meaning |
|---|---|
| `xbox_only` | The DLL doesn't exist on Windows, so it needs a GTX replacement library |
| `host_miss` | The DLL exists on Windows but lacks this function (an Xbox addition), so it needs a replacement |
| `host` | Windows already provides it, so the call can be passed straight through |
| `apiset` | An `api-ms-*` / `ext-ms-*` API set that Windows resolves itself |
| `bundled` | Provided by a DLL that ships with the game |
| `implemented` | Already listed in `tools/coverage/implemented.txt` |
| `runtime_stub` | Not in any import table, but a GTX replacement hit its stub while the game ran |
| `runtime_unimpl` | A call GTX couldn't even identify at runtime, such as an unknown interface |

Add runtime traces with `--trace GAME=gtx.log`. Functions a game actually called while running
rank first, followed by the rest ranked by how many games use them.

## Getting started

### Requirements

- Windows 10/11 x64
- Python 3.12+ (`pip install -r requirements.txt`)
- Visual Studio 2026 (or 2022) with the **Desktop development with C++** workload; it includes MSVC, CMake and the Windows SDK

### Analyze a game

```bash
python tools/triage/triage.py "D:\path\to\game" --name MyGame
python tools/coverage/import_coverage.py reports/*/triage.json
```

Results go to `reports/MyGame/` and `reports/_coverage/`. Git ignores that folder, so nothing from a game is ever committed.

### Build the native code

From a *Developer PowerShell for VS*, or with the CMake that ships with Visual Studio on your `PATH`:

```bash
cmake --preset vs2026-x64
cmake --build --preset debug
ctest --preset debug
```

Optional: point CMake at the public PC GDK to also build the end-to-end tests, which link
Microsoft's real `xgameruntime.lib`. The GDK is the `Microsoft.GDK.Windows` NuGet package; unzip the `.nupkg` anywhere.

```bash
cmake --preset vs2026-x64 -DGTX_GDK_WINDOWS_DIR=<unzipped>/native/261000/windows
```

### Run something under GTX

```bat
set GTX_LOG=%TEMP%\gtx.log
build\vs2026-x64\bin\Debug\gtx-launch.exe path\to\Program.exe
python tools/coverage/import_coverage.py --trace Program=%TEMP%\gtx.log
```

### Run the tests

```bash
python -m unittest discover -s tools -t . -p "test_*.py"
ctest --preset debug
```

## Scope

GTX works only with game files you have **already extracted from a console you own**. The project
does not and will not include tools or code for dumping, decryption, key extraction, DRM or
license circumvention, or exploits. Never commit or attach game files, Microsoft binaries, or keys;
`.gitignore` blocks the common ones.

GTX is not affiliated with or endorsed by Microsoft. Xbox, Xbox Series X|S, DirectX and related
names are trademarks of Microsoft Corporation.

## Prior art and thanks

- **[WinDurango](https://github.com/WinDurango/WinDurango)** showed that the shim-DLL approach works
  for Xbox One-era games. GTX is MIT-licensed and does **not** include WinDurango's (LGPL-3.0) code;
  we study its approach and write our own implementation.
- **Wine / Proton**: the model for translating one platform's APIs onto another.
- Microsoft's public [PC GDK](https://www.nuget.org/packages/Microsoft.GDK.Windows) and
  [Xbox-GDK-Samples](https://github.com/microsoft/Xbox-GDK-Samples): public API definitions and test programs.

## Contributing

The project is early, and the most useful input right now is **triage reports** (summaries only, never
game files) and feedback on the architecture. Please open an issue first to discuss larger changes.
Any code you contribute must be your own work or under an MIT-compatible license; we can't accept
GPL/LGPL code.

## License

[MIT](LICENSE) © The GrandTheftXbox contributors
