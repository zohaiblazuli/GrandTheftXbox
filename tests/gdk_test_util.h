// Shared helpers for tests that link Microsoft's real xgameruntime.lib.
#pragma once
#include <windows.h>

#include <cstdio>
#include <cwchar>

inline int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

#define CHECK_EQ(actual, expected)                                                         \
  do {                                                                                     \
    auto a_ = (actual);                                                                    \
    auto e_ = (expected);                                                                  \
    if (!(a_ == e_)) {                                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s == %s (got 0x%llx, want 0x%llx)\n", __FILE__,   \
                   __LINE__, #actual, #expected, static_cast<unsigned long long>(a_),      \
                   static_cast<unsigned long long>(e_));                                   \
      ++g_failures;                                                                        \
    }                                                                                      \
  } while (0)

// The static lib loads the System32 runtime when Gaming Services is installed; preloading our
// shim by full path makes the loader hand it back instead (what gtx-launch does, D-007).
inline bool preload_shim() {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  wchar_t* slash = std::wcsrchr(path, L'\\');
  if (!n || !slash) return false;
  wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"xgameruntime.dll");
  return LoadLibraryW(path) != nullptr;
}
