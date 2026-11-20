// Unit test: load our xgameruntime.dll directly and exercise the dispatch contract.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

using QueryApiImplFn = HRESULT (*)(const GUID&, const GUID&, void**);
using InitExFn = HRESULT (*)(uint64_t, uint64_t, uint64_t);

// From api_table.inc: XUser interface (XUserGetId = slot 11), XSystem analytics interface.
constexpr GUID kXUser = {0x01ACD177, 0x91F9, 0x4763, {0xA3, 0x8E, 0xCC, 0xBB, 0x55, 0xCE, 0x32, 0xE0}};
constexpr GUID kAnalytics = {0xB884675D, 0xB738, 0x4A9C, {0x81, 0x5D, 0x9A, 0x9A, 0x1E, 0x0C, 0x6C, 0x9B}};
constexpr GUID kBogus = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};

struct XVersion { uint16_t major, minor, build, revision; };
struct XSystemAnalyticsInfo { XVersion os, hosting; char family[64]; char form[64]; };

int main() {
  char tmp[MAX_PATH], log[MAX_PATH];
  GetTempPathA(MAX_PATH, tmp);
  std::snprintf(log, sizeof(log), "%sgtx_xgr_test_%lu.log", tmp, GetCurrentProcessId());
  DeleteFileA(log);
  SetEnvironmentVariableA("GTX_LOG", log);

  HMODULE dll = LoadLibraryW(L"xgameruntime.dll");
  CHECK(dll != nullptr);
  if (!dll) return 1;
  for (const char* e : {"QueryApiImpl", "InitializeApiImpl", "InitializeApiImplEx",
                        "InitializeApiImplEx2", "UninitializeApiImpl", "XErrorReport"})
    CHECK(GetProcAddress(dll, e) != nullptr);

  auto init = reinterpret_cast<InitExFn>(reinterpret_cast<void*>(GetProcAddress(dll, "InitializeApiImplEx")));
  CHECK(init(0x633600002712ull, 0x65F4000025BDull, 2) == S_OK);

  auto query = reinterpret_cast<QueryApiImplFn>(reinterpret_cast<void*>(GetProcAddress(dll, "QueryApiImpl")));
  void* obj = reinterpret_cast<void*>(1);
  CHECK(query(kBogus, kBogus, &obj) == E_NOINTERFACE);
  CHECK(obj == nullptr);

  // Stubbed API: XUserGetId -> E_NOTIMPL, logged once.
  CHECK(query(kXUser, kXUser, &obj) == S_OK && obj);
  auto vtbl = *reinterpret_cast<void***>(obj);
  using GetIdFn = HRESULT (*)(void*, void*, uint64_t*);
  uint64_t id = 0;
  CHECK(reinterpret_cast<GetIdFn>(vtbl[11])(obj, nullptr, &id) == E_NOTIMPL);
  CHECK(reinterpret_cast<GetIdFn>(vtbl[11])(obj, nullptr, &id) == E_NOTIMPL);
  using ReleaseFn = ULONG (*)(void*);
  CHECK(reinterpret_cast<ReleaseFn>(vtbl[2])(obj) == 1);

  // Implemented API: XSystemGetAnalyticsInfo (struct via hidden pointer, slot 3).
  CHECK(query(kAnalytics, kAnalytics, &obj) == S_OK && obj);
  vtbl = *reinterpret_cast<void***>(obj);
  using AnalyticsFn = XSystemAnalyticsInfo* (*)(void*, XSystemAnalyticsInfo*);
  XSystemAnalyticsInfo info{};
  CHECK(reinterpret_cast<AnalyticsFn>(vtbl[3])(obj, &info) == &info);
  CHECK(info.os.major >= 10);
  CHECK(std::strcmp(info.family, "Windows.Xbox") == 0);

  std::ifstream in(log);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  CHECK(text.find("INFO\txgameruntime.dll\tInitializeApiImplEx") != std::string::npos);
  CHECK(text.find("STUB\txgameruntime.dll\tXUserGetId") != std::string::npos);
  CHECK(text.find("XUserGetId", text.find("XUserGetId") + 1) == std::string::npos);  // deduped
  CHECK(text.find("UNIMPL\txgameruntime.dll\tQueryApiImpl") != std::string::npos);
  CHECK(text.find("XSystemGetAnalyticsInfo") == std::string::npos);  // implemented: no stub line

  if (g_failures == 0) std::puts("xgameruntime_shim_test: all passed");
  return g_failures ? 1 : 0;
}
