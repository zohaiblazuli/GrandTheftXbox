// xgameruntime.dll shim — exports + interface dispatch (T-012). See xgr_internal.h.
#define GTX_SHIM_DLL "xgameruntime.dll"
#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "gtx/log.h"
#include "xgr_internal.h"

namespace gtx::xgr {
namespace {

struct IfaceInfo {
  const char* guid;
  int slots;
};

constexpr IfaceInfo kIfaces[] = {
#define GTX_XGR_IFACE(index, guid, slots) {guid, slots},
#include "api_table.inc"
#undef GTX_XGR_IFACE
};
constexpr size_t kIfaceCount = sizeof(kIfaces) / sizeof(kIfaces[0]);

constexpr ApiInfo kApis[] = {
#define GTX_XGR_API(name, iface, slot, kind) {#name, iface, slot, Kind::kind},
#include "api_table.inc"
#undef GTX_XGR_API
};

constexpr size_t total_slots() {
  size_t n = 0;
  for (const auto& i : kIfaces) n += static_cast<size_t>(i.slots);
  return n;
}
constexpr size_t kTotalSlots = total_slots();
constexpr size_t kMaxStubs = 1024;
static_assert(kTotalSlots <= kMaxStubs, "raise kMaxStubs");

// Per global slot: which API lives there (nullptr = no known API).
const ApiInfo* g_slot_api[kMaxStubs] = {};
size_t g_iface_base[kIfaceCount] = {};
struct Object {
  void* const* vtbl;
};
std::vector<void*> g_vtbls[kIfaceCount];
Object g_objects[kIfaceCount];
std::once_flag g_init_once;

uintptr_t on_stub(size_t global_slot, void* self, uintptr_t a1) {
  const ApiInfo* api = g_slot_api[global_slot];
  if (!api) {
    size_t iface = 0;
    while (iface + 1 < kIfaceCount && g_iface_base[iface + 1] <= global_slot) ++iface;
    char fn[96];
    std::snprintf(fn, sizeof(fn), "{%s}[%zu]", kIfaces[iface].guid,
                  global_slot - g_iface_base[iface]);
    if (gtx::log::first_time(GTX_SHIM_DLL, fn))
      gtx::log::write(gtx::log::Kind::Unimplemented, GTX_SHIM_DLL, fn, self, "unknown slot");
    return static_cast<uintptr_t>(static_cast<uint32_t>(E_NOTIMPL));
  }
  if (gtx::log::first_time(GTX_SHIM_DLL, api->name))
    gtx::log::write(gtx::log::Kind::Stub, GTX_SHIM_DLL, api->name, self, "");
  switch (api->kind) {
    case Kind::Hr:
      return static_cast<uintptr_t>(static_cast<uint32_t>(E_NOTIMPL));
    case Kind::Struct:
      return a1;  // hidden return buffer: caller pre-zeroed its copy; hand back the pointer
    default:
      return 0;
  }
}

// One distinct stub per global slot so each knows its identity without a trampoline.
using StubFn = uintptr_t (*)(void*, uintptr_t, uintptr_t, uintptr_t);
template <size_t N>
uintptr_t stub(void* self, uintptr_t a1, uintptr_t, uintptr_t) {
  return on_stub(N, self, a1);
}
template <size_t... N>
constexpr std::array<StubFn, sizeof...(N)> make_stubs(std::index_sequence<N...>) {
  return {&stub<N>...};
}
constexpr auto kStubs = make_stubs(std::make_index_sequence<kTotalSlots>{});

// IUnknown on static singletons: no refcounting needed.
HRESULT STDMETHODCALLTYPE obj_query_interface(void*, REFIID, void** out) {
  if (out) *out = nullptr;
  return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE obj_add_ref(void*) { return 1; }
ULONG STDMETHODCALLTYPE obj_release(void*) { return 1; }

void init_tables() {
  size_t base = 0;
  for (size_t i = 0; i < kIfaceCount; ++i) {
    g_iface_base[i] = base;
    auto& v = g_vtbls[i];
    v.resize(static_cast<size_t>(kIfaces[i].slots));
    for (size_t s = 0; s < v.size(); ++s) v[s] = reinterpret_cast<void*>(kStubs[base + s]);
    v[0] = reinterpret_cast<void*>(&obj_query_interface);
    v[1] = reinterpret_cast<void*>(&obj_add_ref);
    v[2] = reinterpret_cast<void*>(&obj_release);
    g_objects[i].vtbl = v.data();
    base += v.size();
  }
  for (const auto& a : kApis) g_slot_api[g_iface_base[a.iface] + static_cast<size_t>(a.slot)] = &a;

  size_t n = 0;
  const Override* ov = overrides(&n);
  for (size_t k = 0; k < n; ++k) {
    const ApiInfo* a = find_api(ov[k].api);
    if (!a) {
      gtx::log::write(gtx::log::Kind::Error, GTX_SHIM_DLL, "init", nullptr,
                      "override for unknown API %s", ov[k].api);
      continue;
    }
    g_vtbls[a->iface][static_cast<size_t>(a->slot)] = ov[k].fn;
  }
}

void ensure_init() { std::call_once(g_init_once, init_tables); }

void guid_to_string(const GUID& g, char (&buf)[37]) {
  std::snprintf(buf, sizeof(buf), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1,
                g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4],
                g.Data4[5], g.Data4[6], g.Data4[7]);
}

}  // namespace

const ApiInfo* find_api(const char* name) {
  for (const auto& a : kApis)
    if (std::strcmp(a.name, name) == 0) return &a;
  return nullptr;
}

bool is_overridden(const char* name) {
  size_t n = 0;
  const Override* ov = overrides(&n);
  for (size_t k = 0; k < n; ++k)
    if (std::strcmp(ov[k].api, name) == 0) return true;
  return false;
}

}  // namespace gtx::xgr

using namespace gtx::xgr;

// ---------------------------------------------------------------- exports (exports.def)
extern "C" HRESULT QueryApiImpl(const GUID& provider, const GUID& iid, void** out) {
  ensure_init();
  if (!out) return E_POINTER;
  *out = nullptr;
  char want[37];
  guid_to_string(iid, want);
  for (size_t i = 0; i < kIfaceCount; ++i) {
    if (std::strcmp(kIfaces[i].guid, want) == 0) {
      *out = &g_objects[i];
      return S_OK;
    }
  }
  char prov[37];
  guid_to_string(provider, prov);
  if (gtx::log::first_time(GTX_SHIM_DLL, want))
    gtx::log::write(gtx::log::Kind::Unimplemented, GTX_SHIM_DLL, "QueryApiImpl", _ReturnAddress(),
                    "unknown interface {%s} provider {%s}", want, prov);
  return E_NOINTERFACE;
}

// The static lib passes packed version numbers (e.g. 0x633600002712 = OS 25398 / 10002,
// 0x65F4000025BD = GDK 26100 / 9661) plus flags; we accept every version.
extern "C" HRESULT InitializeApiImpl(uint64_t a, uint64_t b) {
  ensure_init();
  gtx::log::write(gtx::log::Kind::Info, GTX_SHIM_DLL, __func__, _ReturnAddress(),
                  "a=%llx b=%llx", a, b);
  return S_OK;
}

extern "C" HRESULT InitializeApiImplEx(uint64_t a, uint64_t b, uint64_t flags) {
  ensure_init();
  gtx::log::write(gtx::log::Kind::Info, GTX_SHIM_DLL, __func__, _ReturnAddress(),
                  "a=%llx b=%llx flags=%llx", a, b, flags);
  return S_OK;
}

extern "C" HRESULT InitializeApiImplEx2(uint64_t a, uint64_t b, uint64_t flags, void* options) {
  ensure_init();
  gtx::log::write(gtx::log::Kind::Info, GTX_SHIM_DLL, __func__, _ReturnAddress(),
                  "a=%llx b=%llx flags=%llx options=%p", a, b, flags, options);
  return S_OK;
}

extern "C" void UninitializeApiImpl() {
  gtx::log::write(gtx::log::Kind::Info, GTX_SHIM_DLL, __func__, _ReturnAddress(), "");
}

extern "C" void XErrorReport(HRESULT hr, const char* message) {
  gtx::log::write(gtx::log::Kind::Error, GTX_SHIM_DLL, __func__, _ReturnAddress(),
                  "hr=%08lx %s", static_cast<unsigned long>(hr), message ? message : "");
}
