// GTX trace logging (ARCHITECTURE "Trace first").
// Every shim entry that is stubbed or unimplemented calls GTX_STUB() so the
// coverage DB (T-005) can be fed from runtime traces, not just static imports.
//
// Output: OutputDebugString always; plus a file when env GTX_LOG=<path> is set.
// Line format (tab-separated, stable for tools):  <kind>\t<dll>\t<function>\t<caller>\t<msg>
#pragma once
#include <cstdarg>

namespace gtx::log {

enum class Kind { Info, Stub, Unimplemented, Warn, Error };

const char* kind_name(Kind k);

// Low-level: writes one record. `caller` is the return address of the shim entry (may be null).
void write(Kind kind, const char* dll, const char* function, const void* caller,
           const char* fmt, ...);
void vwrite(Kind kind, const char* dll, const char* function, const void* caller,
            const char* fmt, va_list args);

// Formats one record into `buf` (used by write; exposed for tests). Returns chars written.
int format(char* buf, int buf_size, Kind kind, const char* dll, const char* function,
           const void* caller, const char* fmt, va_list args);

// Records each (dll, function) stub only once per process to avoid log floods.
// Returns true the first time a given pair is seen.
bool first_time(const char* dll, const char* function);

}  // namespace gtx::log

#ifndef GTX_SHIM_DLL
#define GTX_SHIM_DLL "unknown.dll"
#endif

#define GTX_CALLER() _ReturnAddress()
extern "C" void* _ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)

#define GTX_STUB(...)                                                                    \
  do {                                                                                   \
    if (::gtx::log::first_time(GTX_SHIM_DLL, __func__))                                  \
      ::gtx::log::write(::gtx::log::Kind::Stub, GTX_SHIM_DLL, __func__, GTX_CALLER(),    \
                        "" __VA_ARGS__);                                                 \
  } while (0)

#define GTX_UNIMPLEMENTED(...)                                                           \
  ::gtx::log::write(::gtx::log::Kind::Unimplemented, GTX_SHIM_DLL, __func__, GTX_CALLER(), \
                    "" __VA_ARGS__)
