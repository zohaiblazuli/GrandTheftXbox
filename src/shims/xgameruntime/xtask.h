// XTaskQueue + XAsync for the xgameruntime shim (T-014).
// Types mirror the public GDK headers XTaskQueue.h / XAsync.h / XAsyncProvider.h (MIT-licensed
// declarations shipped in Microsoft.GDK.Windows). Implementation is GTX's own.
#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "xgr_internal.h"

namespace gtx::xgr::task {

struct Queue;
struct Port;

enum class DispatchMode : uint32_t { Manual, ThreadPool, SerializedThreadPool, Immediate };
enum class PortId : uint32_t { Work, Completion };

using Callback = void(CALLBACK*)(void* context, bool canceled);
using MonitorCallback = void(CALLBACK*)(void* context, Queue* queue, PortId port);
using TerminatedCallback = void(CALLBACK*)(void* context);
struct RegistrationToken {
  uint64_t token;
};

struct AsyncBlock;
using AsyncCompletionRoutine = void(CALLBACK*)(AsyncBlock* block);
using AsyncWork = HRESULT(CALLBACK*)(AsyncBlock* block);
struct AsyncBlock {
  Queue* queue;
  void* context;
  AsyncCompletionRoutine callback;
  unsigned char internal[sizeof(void*) * 4];
};
static_assert(sizeof(AsyncBlock) == 56, "XAsyncBlock layout");

enum class AsyncOp : uint32_t { Begin, DoWork, GetResult, Cancel, Cleanup };
struct AsyncProviderData {
  AsyncBlock* async;
  size_t bufferSize;
  void* buffer;
  void* context;
};
using AsyncProvider = HRESULT(CALLBACK*)(AsyncOp op, const AsyncProviderData* data);

// --- XTaskQueue ---------------------------------------------------------------------------
HRESULT Create(DispatchMode work, DispatchMode completion, Queue** out);
HRESULT CreateComposite(Port* work, Port* completion, Queue** out);
HRESULT GetPort(Queue* q, PortId port, Port** out);
HRESULT Duplicate(Queue* q, Queue** out);
void Close(Queue* q);
bool Dispatch(Queue* q, PortId port, uint32_t timeout_ms);
HRESULT Terminate(Queue* q, bool wait, void* ctx, TerminatedCallback cb);
HRESULT Submit(Queue* q, PortId port, void* ctx, Callback cb);
HRESULT SubmitDelayed(Queue* q, PortId port, uint32_t delay_ms, void* ctx, Callback cb);
HRESULT RegisterWaiter(Queue* q, PortId port, HANDLE h, void* ctx, Callback cb, RegistrationToken* tok);
void UnregisterWaiter(Queue* q, RegistrationToken tok);
HRESULT RegisterMonitor(Queue* q, void* ctx, MonitorCallback cb, RegistrationToken* tok);
void UnregisterMonitor(Queue* q, RegistrationToken tok);
bool GetProcessQueue(Queue** out);  // duplicated handle; caller closes
void SetProcessQueue(Queue* q);

// --- XAsync -------------------------------------------------------------------------------
HRESULT AsyncGetStatus(AsyncBlock* b, bool wait);
HRESULT AsyncGetResultSize(AsyncBlock* b, size_t* size);
void AsyncCancel(AsyncBlock* b);
HRESULT AsyncRun(AsyncBlock* b, AsyncWork work);
HRESULT AsyncBegin(AsyncBlock* b, void* ctx, const void* identity, const char* name, AsyncProvider p);
HRESULT AsyncSchedule(AsyncBlock* b, uint32_t delay_ms);
void AsyncComplete(AsyncBlock* b, HRESULT result, size_t required);
HRESULT AsyncGetResult(AsyncBlock* b, const void* identity, size_t size, void* buf, size_t* used);

// vtable overrides for the XAsync/XTaskQueue interface (registered in impl.cpp).
const Override* overrides(size_t* count);

}  // namespace gtx::xgr::task
