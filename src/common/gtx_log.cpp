#include "gtx/log.h"

#include <windows.h>

#include <share.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace gtx::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
bool g_file_init = false;

FILE* log_file() {  // caller holds g_mutex
  if (!g_file_init) {
    g_file_init = true;
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("GTX_LOG", path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      // Shared so the log can be tailed while the game runs (fopen_s would lock it).
      g_file = _fsopen(path, "a", _SH_DENYNO);
    }
  }
  return g_file;
}

}  // namespace

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Info: return "INFO";
    case Kind::Stub: return "STUB";
    case Kind::Unimplemented: return "UNIMPL";
    case Kind::Warn: return "WARN";
    case Kind::Error: return "ERROR";
  }
  return "?";
}

int format(char* buf, int buf_size, Kind kind, const char* dll, const char* function,
           const void* caller, const char* fmt, va_list args) {
  if (buf_size <= 0) return 0;
  int n = std::snprintf(buf, static_cast<size_t>(buf_size), "%s\t%s\t%s\t%p\t", kind_name(kind),
                        dll ? dll : "-", function ? function : "-", caller);
  if (n < 0) n = 0;
  if (n < buf_size && fmt) {
    int m = std::vsnprintf(buf + n, static_cast<size_t>(buf_size - n), fmt, args);
    if (m > 0) n += m;
  }
  if (n >= buf_size) n = buf_size - 1;
  return n;
}

void vwrite(Kind kind, const char* dll, const char* function, const void* caller,
            const char* fmt, va_list args) {
  char buf[1024];
  int n = format(buf, sizeof(buf) - 1, kind, dll, function, caller, fmt, args);
  buf[n] = '\n';
  buf[n + 1] = '\0';
  OutputDebugStringA(buf);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (FILE* f = log_file()) {
    std::fputs(buf, f);
    std::fflush(f);
  }
}

void write(Kind kind, const char* dll, const char* function, const void* caller,
           const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vwrite(kind, dll, function, caller, fmt, args);
  va_end(args);
}

bool first_time(const char* dll, const char* function) {
  static std::mutex m;
  static std::set<std::pair<std::string, std::string>> seen;
  std::lock_guard<std::mutex> lock(m);
  return seen.emplace(dll ? dll : "", function ? function : "").second;
}

}  // namespace gtx::log
