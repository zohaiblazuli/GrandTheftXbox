// Minimal self-checking test for src/common (no framework dependency yet).
#define GTX_SHIM_DLL "testshim.dll"
#include "gtx/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

static std::string fmt(gtx::log::Kind k, const char* f, ...) {
  char buf[256];
  va_list a;
  va_start(a, f);
  int n = gtx::log::format(buf, sizeof(buf), k, "kernelx.dll", "XMemAlloc", nullptr, f, a);
  va_end(a);
  return std::string(buf, n);
}

static void StubbedThing() { GTX_STUB("size=%d", 42); }

int main() {
  // Record format is tab-separated and stable.
  std::string s = fmt(gtx::log::Kind::Stub, "size=%d", 7);
  CHECK(s.rfind("STUB\tkernelx.dll\tXMemAlloc\t", 0) == 0);
  CHECK(s.size() > 6 && s.substr(s.size() - 6) == "size=7");

  // Truncation never overruns.
  char tiny[8];
  va_list none{};
  int n = gtx::log::format(tiny, sizeof(tiny), gtx::log::Kind::Info, "a", "b", nullptr, "", none);
  CHECK(n == 7);

  // first_time dedups per (dll, function).
  CHECK(gtx::log::first_time("x.dll", "F"));
  CHECK(!gtx::log::first_time("x.dll", "F"));
  CHECK(gtx::log::first_time("x.dll", "G"));

  // GTX_STUB writes once to the GTX_LOG file.
  char tmp[MAX_PATH], path[MAX_PATH];
  GetTempPathA(MAX_PATH, tmp);
  std::snprintf(path, sizeof(path), "%sgtx_common_test_%lu.log", tmp, GetCurrentProcessId());
  DeleteFileA(path);
  SetEnvironmentVariableA("GTX_LOG", path);
  StubbedThing();
  StubbedThing();
  {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string log = ss.str();
    CHECK(log.find("STUB\ttestshim.dll\tStubbedThing\t") != std::string::npos);
    CHECK(log.find("size=42") != std::string::npos);
    CHECK(log.find("StubbedThing", log.find("StubbedThing") + 1) == std::string::npos);
  }

  if (g_failures == 0) std::puts("gtx_common_test: all passed");
  return g_failures == 0 ? 0 : 1;
}
