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
#include "xtask.h"

#include <iterator>
#include <vector>

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

// STDAPI XSystemGetXboxLiveSandboxId(size_t size, char* sandboxId, size_t* used)
// Matches Microsoft's PC runtime for an unconfigured machine: "RETAIL", used = 7 (incl. NUL).
HRESULT XSystemGetXboxLiveSandboxId_impl(void*, size_t size, char* id, size_t* used) {
  static const char kSandbox[] = "RETAIL";
  if (used) *used = sizeof(kSandbox);
  if (!id || size < sizeof(kSandbox)) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  std::memcpy(id, kSandbox, sizeof(kSandbox));
  return S_OK;
}

// <TitleId>hex</TitleId> from MicrosoftGame.config next to the game exe. [inferred: hex text]
bool read_title_id(uint32_t* out) {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!n || !slash) return false;
  wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"MicrosoftGame.config");
  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;
  char buf[64 * 1024];
  size_t len = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[len] = '\0';
  for (char* p = buf; *p; ++p) *p = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
  const char* tag = std::strstr(buf, "<titleid>");
  if (!tag) return false;
  char* end = nullptr;
  unsigned long v = std::strtoul(tag + 9, &end, 16);
  if (end == tag + 9 || *end != '<') return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

// STDAPI XGameGetXboxTitleId(uint32_t* titleId)
HRESULT XGameGetXboxTitleId_impl(void*, uint32_t* title) {
  if (!title) return E_INVALIDARG;
  static uint32_t cached = 0;
  static const bool found = read_title_id(&cached);
  if (!found) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);  // same as MS runtime without a config
  *title = cached;
  return S_OK;
}

// XThread time-sensitivity is a console scheduling hint; no PC equivalent.
void XThreadAssertNotTimeSensitive_impl(void*) {}
bool XThreadIsTimeSensitive_impl(void*) { return false; }
HRESULT XThreadSetTimeSensitive_impl(void*, bool) { return S_OK; }

const Override kOverrides[] = {
    {"XSystemGetAnalyticsInfo", reinterpret_cast<void*>(&XSystemGetAnalyticsInfo_impl)},
    {"XSystemGetConsoleId", reinterpret_cast<void*>(&XSystemGetConsoleId_impl)},
    {"XSystemGetXboxLiveSandboxId", reinterpret_cast<void*>(&XSystemGetXboxLiveSandboxId_impl)},
    {"XGameGetXboxTitleId", reinterpret_cast<void*>(&XGameGetXboxTitleId_impl)},
    {"XThreadAssertNotTimeSensitive", reinterpret_cast<void*>(&XThreadAssertNotTimeSensitive_impl)},
    {"XThreadIsTimeSensitive", reinterpret_cast<void*>(&XThreadIsTimeSensitive_impl)},
    {"XThreadSetTimeSensitive", reinterpret_cast<void*>(&XThreadSetTimeSensitive_impl)},
};

}  // namespace

// All implemented APIs: this file's plus each subsystem's list (xtask.cpp, ...).
const Override* overrides(size_t* count) {
  static const std::vector<Override> all = [] {
    std::vector<Override> v(std::begin(kOverrides), std::end(kOverrides));
    size_t n = 0;
    const Override* t = task::overrides(&n);
    v.insert(v.end(), t, t + n);
    return v;
  }();
  *count = all.size();
  return all.data();
}

}  // namespace gtx::xgr
