// xgameruntime.dll shim — real implementations (T-012).
// Each function is member-style: first parameter is the interface object (`self`), then the
// public API's parameters in order (see tools/xgameruntime/api_table.json for prototypes).
// Add an entry to kOverrides to replace the logging stub for that API's vtable slot.
#define GTX_SHIM_DLL "xgameruntime.dll"
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gtx/log.h"
#include "xgr_internal.h"

namespace gtx::xgr {
namespace {

// Layouts mirror the public GDK headers (XGameRuntimeTypes.h / XSystem.h).
struct XVersion {
  uint16_t major, minor, build, revision;
};
struct XSystemAnalyticsInfo {
  XVersion osVersion;
  XVersion hostingOsVersion;
  char family[64];
  char form[64];
};
static_assert(sizeof(XSystemAnalyticsInfo) == 0x90, "matches lib's memset size");
constexpr size_t kConsoleIdBytes = 39;  // XSystemConsoleIdBytes

XVersion host_version() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW vi{sizeof(vi)};
  auto fn = reinterpret_cast<RtlGetVersionFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
  if (fn) fn(&vi);
  return {static_cast<uint16_t>(vi.dwMajorVersion), static_cast<uint16_t>(vi.dwMinorVersion),
          static_cast<uint16_t>(vi.dwBuildNumber), 0};
}

// STDAPI_(XSystemAnalyticsInfo) XSystemGetAnalyticsInfo()  — struct via hidden pointer.
XSystemAnalyticsInfo* XSystemGetAnalyticsInfo_impl(void*, XSystemAnalyticsInfo* out) {
  std::memset(out, 0, sizeof(*out));
  out->osVersion = host_version();
  out->hostingOsVersion = out->osVersion;
  // [inferred] Console titles expect an Xbox family; per-title override belongs in config (T-later).
  strcpy_s(out->family, "Windows.Xbox");
  strcpy_s(out->form, "Xbox Series X");
  return out;
}

// STDAPI XSystemGetConsoleId(size_t size, char* consoleId, size_t* used)
HRESULT XSystemGetConsoleId_impl(void*, size_t size, char* id, size_t* used) {
  // Stable, obviously-synthetic id ("GTX" + zeros), NUL-terminated, 39 bytes incl. NUL.
  if (used) *used = kConsoleIdBytes;
  if (!id || size < kConsoleIdBytes) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  std::memset(id, '0', kConsoleIdBytes - 1);
  std::memcpy(id, "GTX", 3);
  id[kConsoleIdBytes - 1] = '\0';
  return S_OK;
}

const Override kOverrides[] = {
    {"XSystemGetAnalyticsInfo", reinterpret_cast<void*>(&XSystemGetAnalyticsInfo_impl)},
    {"XSystemGetConsoleId", reinterpret_cast<void*>(&XSystemGetConsoleId_impl)},
};

}  // namespace

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

}  // namespace gtx::xgr
