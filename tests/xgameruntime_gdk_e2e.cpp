// End-to-end: link Microsoft's real static xgameruntime.lib (public PC GDK) and call public
// APIs. The lib must find our xgameruntime.dll next to this exe and dispatch into it.
// Built only when GTX_GDK_WINDOWS_DIR points at Microsoft.GDK.Windows/native/<ver>/windows.
#include <windows.h>

#include <XGameRuntime.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

// The static lib only loads a local xgameruntime.dll when Gaming Services is absent or a
// machine-wide registry override is set; otherwise it uses LoadLibraryEx(System32 only).
// The Windows loader returns an already-loaded module with the same base name, so preloading
// our shim by full path (what the GTX launcher will do) redirects the runtime without hooks.
static bool preload_shim() {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  wchar_t* slash = std::wcsrchr(path, L'\\');
  if (!n || !slash) return false;
  wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"xgameruntime.dll");
  return LoadLibraryW(path) != nullptr;
}

int main() {
#ifndef GTX_NO_SELF_PRELOAD
  CHECK(preload_shim());
#else
  (void)&preload_shim;  // gtx-launch injects the shim before main runs
#endif
  HRESULT hr = XGameRuntimeInitialize();
  std::printf("XGameRuntimeInitialize -> %08lx\n", static_cast<unsigned long>(hr));
  CHECK(SUCCEEDED(hr));

  // Proves the loaded runtime is ours, not the installed Gaming Services one.
  HMODULE ours = GetModuleHandleW(L"xgameruntime.dll");
  wchar_t path[MAX_PATH] = {};
  if (ours) GetModuleFileNameW(ours, path, MAX_PATH);
  std::wprintf(L"runtime: %ls\n", path);
  CHECK(ours != nullptr && std::wcsstr(path, L"\\System32\\") == nullptr &&
        std::wcsstr(path, L"\\system32\\") == nullptr);

  XSystemAnalyticsInfo info = XSystemGetAnalyticsInfo();
  std::printf("analytics: os %u.%u.%u family=%s form=%s\n", info.osVersion.major,
              info.osVersion.minor, info.osVersion.build, info.family, info.form);
  CHECK(std::strcmp(info.family, "Windows.Xbox") == 0);

  char id[XSystemConsoleIdBytes] = {};
  size_t used = 0;
  CHECK(SUCCEEDED(XSystemGetConsoleId(sizeof(id), id, &used)));
  CHECK(used == XSystemConsoleIdBytes && std::strncmp(id, "GTX", 3) == 0);

  // Stubbed API returns E_NOTIMPL through the real lib.
  XUserLocalId local{};
  CHECK(XUserGetLocalId(nullptr, &local) == E_NOTIMPL);

  XGameRuntimeUninitialize();
  if (g_failures == 0) std::puts("xgameruntime_gdk_e2e: all passed");
  return g_failures ? 1 : 0;
}
