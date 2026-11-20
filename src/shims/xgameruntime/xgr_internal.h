// xgameruntime.dll shim — internal interfaces (T-012).
//
// How the real runtime is reached (R-008): the GDK's static xgameruntime.lib loads
// xgameruntime.dll (a copy next to the game exe wins over the installed Gaming Services one),
// resolves the exports in exports.def, then for every public API does:
//     QueryApiImpl(&IID, &IID, &obj);  obj->vtbl[slot](obj, args...);  obj->Release();
// api_table.inc (generated from the public GDK lib) maps each API to (interface, slot).
// Every slot defaults to a logging stub; real implementations override slots by API name.
#pragma once
#include <cstddef>
#include <cstdint>

namespace gtx::xgr {

enum class Kind { Hr, Void, Zero, Struct };

struct Override {
  const char* api;  // public API name, e.g. "XSystemGetAnalyticsInfo"
  void* fn;         // member-style: first parameter is the interface `this`
};

// Implemented in impl.cpp.
const Override* overrides(size_t* count);

// Introspection used by tests and the trace tooling.
struct ApiInfo {
  const char* name;
  int iface;
  int slot;
  Kind kind;
};
const ApiInfo* find_api(const char* name);
bool is_overridden(const char* name);

}  // namespace gtx::xgr
