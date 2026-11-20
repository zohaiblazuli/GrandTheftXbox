// XTaskQueue + XAsync implementation for the xgameruntime shim (T-014). See xtask.h.
//
// Ownership rules (keep them in mind when editing):
//  * Queue is refcounted. Handles (Create/Duplicate/GetProcessQueue) and every in-flight item hold
//    a reference, so a queue outlives its last callback even after XTaskQueueCloseHandle.
//  * Each submitted callback becomes an Item that is finished exactly once: run (canceled=false)
//    or canceled (canceled=true) by termination.
//  * Delayed items wait in one process-wide scheduler thread, not OS timers (simple cancellation).
//  * XAsync state lives on the heap; status/required size are mirrored into the caller's
//    XAsyncBlock::internal so XAsyncGetStatus works after the state is released, and so the
//    caller may free the block inside its completion callback (the GDK allows that).
#define GTX_SHIM_DLL "xgameruntime.dll"
#include "xtask.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "gtx/log.h"

namespace gtx::xgr::task {

// =========================================================================== XTaskQueue
struct Item {
  Callback cb;
  void* ctx;
  Queue* owner;           // queue the item was submitted through (holds a ref + inflight count)
  bool canceled = false;  // set by termination; still delivered through the port (verified vs MS runtime)
};

struct Port {
  DispatchMode mode;
  Queue* owner;  // queue that owns this port's memory
  std::mutex m;
  std::condition_variable cv;
  std::deque<Item> items;  // Manual + SerializedThreadPool
  bool serial_running = false;
};

struct Monitor {
  void* ctx;
  MonitorCallback cb;
};

struct Waiter {
  Queue* q;
  PortId port;
  void* ctx;
  Callback cb;
  HANDLE wait = nullptr;
};

struct Queue {
  std::atomic<long> refs{1};
  Port* ports[2] = {};
  std::unique_ptr<Port> owned[2];
  Queue* sources[2] = {};  // composite: queues owning the borrowed ports
  std::mutex m;
  std::condition_variable idle;
  long inflight = 0;
  bool terminated = false;
  std::map<uint64_t, Monitor> monitors;
  std::map<uint64_t, Waiter*> waiters;
};

namespace {

std::atomic<uint64_t> g_next_token{1};

void add_ref(Queue* q) { q->refs.fetch_add(1, std::memory_order_relaxed); }

void release(Queue* q) {
  if (q->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
  for (Queue* s : q->sources)
    if (s) release(s);
  delete q;
}

Item begin_item(Queue* q, Callback cb, void* ctx) {
  add_ref(q);
  std::lock_guard<std::mutex> lock(q->m);
  ++q->inflight;
  return {cb, ctx, q};
}

void finish_item(const Item& item, bool canceled) {
  item.cb(item.ctx, canceled || item.canceled);
  Queue* q = item.owner;
  {
    std::lock_guard<std::mutex> lock(q->m);
    if (--q->inflight == 0) q->idle.notify_all();
  }
  release(q);
}

VOID CALLBACK tp_item(PTP_CALLBACK_INSTANCE, PVOID p) {
  std::unique_ptr<Item> item(static_cast<Item*>(p));
  finish_item(*item, false);
}

VOID CALLBACK tp_serial_drain(PTP_CALLBACK_INSTANCE, PVOID p) {
  Port* port = static_cast<Port*>(p);
  Queue* keep = port->owner;  // ref taken when the drain was started
  for (;;) {
    Item item;
    {
      std::lock_guard<std::mutex> lock(port->m);
      if (port->items.empty()) {
        port->serial_running = false;
        break;
      }
      item = port->items.front();
      port->items.pop_front();
    }
    finish_item(item, false);
  }
  release(keep);  // may destroy the port; do not touch it afterwards
}

void notify_monitors(Queue* q, PortId port) {
  std::vector<Monitor> copy;
  {
    std::lock_guard<std::mutex> lock(q->m);
    for (const auto& kv : q->monitors) copy.push_back(kv.second);
  }
  for (const auto& mon : copy) mon.cb(mon.ctx, q, port);
}

// Puts a begun item on its port. Never fails: on error the item is finished as canceled.
void enqueue(Queue* q, PortId port_id, const Item& item) {
  Port* p = q->ports[static_cast<uint32_t>(port_id)];
  switch (p->mode) {
    case DispatchMode::Immediate:
      notify_monitors(q, port_id);
      finish_item(item, false);
      return;
    case DispatchMode::ThreadPool: {
      auto* heap = new Item(item);
      if (!TrySubmitThreadpoolCallback(tp_item, heap, nullptr)) {
        delete heap;
        finish_item(item, true);
        return;
      }
      break;
    }
    case DispatchMode::SerializedThreadPool: {
      bool start = false;
      {
        std::lock_guard<std::mutex> lock(p->m);
        p->items.push_back(item);
        if (!p->serial_running) start = p->serial_running = true;
      }
      if (start) {
        add_ref(p->owner);
        if (!TrySubmitThreadpoolCallback(tp_serial_drain, p, nullptr)) tp_serial_drain(nullptr, p);
      }
      break;
    }
    case DispatchMode::Manual:
    default: {
      {
        std::lock_guard<std::mutex> lock(p->m);
        p->items.push_back(item);
      }
      p->cv.notify_all();
      break;
    }
  }
  notify_monitors(q, port_id);
}

// ---- delayed submissions: one scheduler thread for the process
struct Delayed {
  Queue* q;
  PortId port;
  Item item;
};
std::mutex g_delay_m;
std::condition_variable g_delay_cv;
std::multimap<ULONGLONG, Delayed> g_delayed;
bool g_delay_thread_started = false;

bool is_terminated(Queue* q) {
  std::lock_guard<std::mutex> lock(q->m);
  return q->terminated;
}

void delay_thread() {
  std::unique_lock<std::mutex> lock(g_delay_m);
  for (;;) {
    if (g_delayed.empty()) {
      g_delay_cv.wait(lock);
      continue;
    }
    ULONGLONG now = GetTickCount64();
    auto first = g_delayed.begin();
    if (first->first > now) {
      g_delay_cv.wait_for(lock, std::chrono::milliseconds(first->first - now));
      continue;
    }
    Delayed d = first->second;
    g_delayed.erase(first);
    lock.unlock();
    if (is_terminated(d.q)) d.item.canceled = true;
    enqueue(d.q, d.port, d.item);
    lock.lock();
  }
}

std::mutex g_proc_m;
Queue* g_proc = nullptr;
bool g_proc_explicit = false;

Queue* resolve(Queue* q, bool* borrowed) {
  *borrowed = q != nullptr;
  if (q) return q;
  Queue* proc = nullptr;
  return GetProcessQueue(&proc) ? proc : nullptr;
}

VOID CALLBACK waiter_fired(PVOID p, BOOLEAN) {
  auto* w = static_cast<Waiter*>(p);
  if (Submit(w->q, w->port, w->ctx, w->cb) != S_OK) w->cb(w->ctx, true);
}

std::unique_ptr<Port> make_port(DispatchMode mode, Queue* owner) {
  auto p = std::make_unique<Port>();
  p->mode = mode;
  p->owner = owner;
  return p;
}

bool valid_mode(DispatchMode m) { return static_cast<uint32_t>(m) <= 3; }
bool valid_port(PortId p) { return static_cast<uint32_t>(p) <= 1; }

}  // namespace

HRESULT Create(DispatchMode work, DispatchMode completion, Queue** out) {
  if (!out || !valid_mode(work) || !valid_mode(completion)) return E_INVALIDARG;
  auto* q = new Queue();
  q->owned[0] = make_port(work, q);
  q->owned[1] = make_port(completion, q);
  q->ports[0] = q->owned[0].get();
  q->ports[1] = q->owned[1].get();
  *out = q;
  return S_OK;
}

HRESULT CreateComposite(Port* work, Port* completion, Queue** out) {
  if (!out || !work || !completion) return E_INVALIDARG;
  auto* q = new Queue();
  q->ports[0] = work;
  q->ports[1] = completion;
  q->sources[0] = work->owner;
  q->sources[1] = completion->owner;
  add_ref(work->owner);
  add_ref(completion->owner);
  *out = q;
  return S_OK;
}

HRESULT GetPort(Queue* q, PortId port, Port** out) {
  if (!q || !out || !valid_port(port)) return E_INVALIDARG;
  *out = q->ports[static_cast<uint32_t>(port)];
  return S_OK;
}

HRESULT Duplicate(Queue* q, Queue** out) {
  if (!q || !out) return E_INVALIDARG;
  add_ref(q);
  *out = q;
  return S_OK;
}

void Close(Queue* q) {
  if (q) release(q);
}

bool Dispatch(Queue* q, PortId port, uint32_t timeout_ms) {
  if (!q || !valid_port(port)) return false;
  Port* p = q->ports[static_cast<uint32_t>(port)];
  if (p->mode != DispatchMode::Manual) return false;
  Item item;
  {
    std::unique_lock<std::mutex> lock(p->m);
    if (p->items.empty() && timeout_ms) {
      auto ready = [p] { return !p->items.empty(); };
      if (timeout_ms == INFINITE)
        p->cv.wait(lock, ready);
      else
        p->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
    }
    if (p->items.empty()) return false;
    item = p->items.front();
    p->items.pop_front();
  }
  finish_item(item, false);
  return true;
}

HRESULT Submit(Queue* q, PortId port, void* ctx, Callback cb) {
  if (!cb || !valid_port(port)) return E_INVALIDARG;
  bool borrowed = false;
  q = resolve(q, &borrowed);
  if (!q) return E_INVALIDARG;
  HRESULT hr = S_OK;
  if (is_terminated(q)) {
    hr = E_ABORT;
  } else {
    enqueue(q, port, begin_item(q, cb, ctx));
  }
  if (!borrowed) Close(q);
  return hr;
}

HRESULT SubmitDelayed(Queue* q, PortId port, uint32_t delay_ms, void* ctx, Callback cb) {
  if (delay_ms == 0) return Submit(q, port, ctx, cb);
  if (!cb || !valid_port(port)) return E_INVALIDARG;
  bool borrowed = false;
  q = resolve(q, &borrowed);
  if (!q) return E_INVALIDARG;
  HRESULT hr = S_OK;
  if (is_terminated(q)) {
    hr = E_ABORT;
  } else {
    Delayed d{q, port, begin_item(q, cb, ctx)};
    std::lock_guard<std::mutex> lock(g_delay_m);
    if (!g_delay_thread_started) {
      g_delay_thread_started = true;
      std::thread(delay_thread).detach();
    }
    g_delayed.emplace(GetTickCount64() + delay_ms, d);
    g_delay_cv.notify_all();
  }
  if (!borrowed) Close(q);
  return hr;
}

HRESULT Terminate(Queue* q, bool wait, void* ctx, TerminatedCallback cb) {
  if (!q) return E_INVALIDARG;
  std::map<uint64_t, Waiter*> waiters;
  {
    std::lock_guard<std::mutex> lock(q->m);
    q->terminated = true;
    waiters.swap(q->waiters);
  }
  for (auto& kv : waiters) {
    UnregisterWaitEx(kv.second->wait, INVALID_HANDLE_VALUE);
    release(kv.second->q);
    delete kv.second;
  }
  // Cancel items submitted through this queue. Like Microsoft's runtime, cancellations are
  // delivered through the ports (a Manual port needs XTaskQueueDispatch to see them), so
  // XTaskQueueTerminate(wait=true) on a Manual queue must not be called from its only dispatcher.
  for (Port* p : q->ports) {
    {
      std::lock_guard<std::mutex> lock(p->m);
      for (Item& i : p->items)
        if (i.owner == q) i.canceled = true;
    }
    p->cv.notify_all();
  }
  std::vector<Delayed> delayed;
  {
    std::lock_guard<std::mutex> lock(g_delay_m);
    for (auto it = g_delayed.begin(); it != g_delayed.end();) {
      if (it->second.item.owner == q) {
        delayed.push_back(it->second);
        it = g_delayed.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (Delayed& d : delayed) {
    d.item.canceled = true;
    enqueue(d.q, d.port, d.item);
  }

  add_ref(q);  // keep alive until the termination callback has run
  struct Done {
    Queue* q;
    void* ctx;
    TerminatedCallback cb;
  };
  auto finish = [](Done d) {
    {
      std::unique_lock<std::mutex> lock(d.q->m);
      d.q->idle.wait(lock, [&] { return d.q->inflight == 0; });
    }
    if (d.cb) d.cb(d.ctx);
    release(d.q);
  };
  Done d{q, ctx, cb};
  if (wait) {
    finish(d);
  } else {
    auto* heap = new Done(d);
    auto tp = [](PTP_CALLBACK_INSTANCE, PVOID p) {
      std::unique_ptr<Done> dd(static_cast<Done*>(p));
      Done copy = *dd;
      {
        std::unique_lock<std::mutex> lock(copy.q->m);
        copy.q->idle.wait(lock, [&] { return copy.q->inflight == 0; });
      }
      if (copy.cb) copy.cb(copy.ctx);
      release(copy.q);
    };
    if (!TrySubmitThreadpoolCallback(tp, heap, nullptr)) {
      delete heap;
      finish(d);
    }
  }
  return S_OK;
}

HRESULT RegisterWaiter(Queue* q, PortId port, HANDLE h, void* ctx, Callback cb,
                       RegistrationToken* tok) {
  if (!q || !h || !cb || !tok || !valid_port(port)) return E_INVALIDARG;
  if (is_terminated(q)) return E_ABORT;
  auto* w = new Waiter{q, port, ctx, cb};
  add_ref(q);
  // [inferred] One-shot: the callback is queued once when the handle is signaled.
  if (!RegisterWaitForSingleObject(&w->wait, h, waiter_fired, w, INFINITE, WT_EXECUTEONLYONCE)) {
    release(q);
    delete w;
    return HRESULT_FROM_WIN32(GetLastError());
  }
  tok->token = g_next_token++;
  std::lock_guard<std::mutex> lock(q->m);
  q->waiters[tok->token] = w;
  return S_OK;
}

void UnregisterWaiter(Queue* q, RegistrationToken tok) {
  if (!q) return;
  Waiter* w = nullptr;
  {
    std::lock_guard<std::mutex> lock(q->m);
    auto it = q->waiters.find(tok.token);
    if (it == q->waiters.end()) return;
    w = it->second;
    q->waiters.erase(it);
  }
  UnregisterWaitEx(w->wait, INVALID_HANDLE_VALUE);
  release(w->q);
  delete w;
}

HRESULT RegisterMonitor(Queue* q, void* ctx, MonitorCallback cb, RegistrationToken* tok) {
  if (!q || !cb || !tok) return E_INVALIDARG;
  tok->token = g_next_token++;
  std::lock_guard<std::mutex> lock(q->m);
  q->monitors[tok->token] = {ctx, cb};
  return S_OK;
}

void UnregisterMonitor(Queue* q, RegistrationToken tok) {
  if (!q) return;
  std::lock_guard<std::mutex> lock(q->m);
  q->monitors.erase(tok.token);
}

bool GetProcessQueue(Queue** out) {
  if (!out) return false;
  std::lock_guard<std::mutex> lock(g_proc_m);
  if (!g_proc && !g_proc_explicit)
    Create(DispatchMode::ThreadPool, DispatchMode::ThreadPool, &g_proc);
  *out = nullptr;
  if (!g_proc) return false;
  add_ref(g_proc);
  *out = g_proc;
  return true;
}

void SetProcessQueue(Queue* q) {
  Queue* old = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_proc_m);
    old = g_proc;
    if (q) add_ref(q);
    g_proc = q;
    g_proc_explicit = true;
  }
  if (old) release(old);
}

// =========================================================================== XAsync
namespace {

constexpr uint32_t kMagic = 0x58475458;  // "XTGX"

struct AsyncState {
  std::atomic<long> refs{1};  // the block's reference (dropped once the result is consumed)
  AsyncBlock* block = nullptr;
  AsyncProvider provider = nullptr;
  void* ctx = nullptr;
  const void* identity = nullptr;
  Queue* queue = nullptr;
  HANDLE done = nullptr;
  HRESULT status = E_PENDING;
  size_t required = 0;
  std::atomic<bool> canceled{false};
  AsyncCompletionRoutine callback = nullptr;
};

struct BlockInternal {
  AsyncState* state;
  int32_t status;
  uint32_t magic;
  uint64_t required;
  uint64_t spare;
};
static_assert(sizeof(BlockInternal) <= sizeof(AsyncBlock::internal), "fits XAsyncBlock");

std::mutex g_async_m;

BlockInternal* bi(AsyncBlock* b) { return reinterpret_cast<BlockInternal*>(b->internal); }

void add_ref(AsyncState* s) { s->refs.fetch_add(1, std::memory_order_relaxed); }

void release(AsyncState* s) {
  if (s->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
  if (s->queue) Close(s->queue);
  if (s->done) CloseHandle(s->done);
  delete s;
}

HRESULT call(AsyncState* s, AsyncOp op, void* buf = nullptr, size_t size = 0) {
  AsyncProviderData data{s->block, size, buf, s->ctx};
  return s->provider(op, &data);
}

void CALLBACK completion_thunk(void* p, bool) {
  auto* s = static_cast<AsyncState*>(p);
  s->callback(s->block);  // always notify, even when the queue is terminating
  release(s);
}

void complete_state(AsyncState* s, HRESULT hr, size_t required) {
  if (hr == E_PENDING) return;
  bool no_payload;
  {
    std::lock_guard<std::mutex> lock(g_async_m);
    if (s->status != E_PENDING) return;
    s->status = hr;
    s->required = SUCCEEDED(hr) ? required : 0;
    no_payload = FAILED(hr) || s->required == 0;
    s->callback = s->block->callback;
    BlockInternal* in = bi(s->block);
    in->status = hr;
    in->required = s->required;
    if (no_payload) in->state = nullptr;  // block no longer needs the state
  }
  if (no_payload) call(s, AsyncOp::Cleanup);
  SetEvent(s->done);
  if (s->callback) {
    add_ref(s);
    if (FAILED(Submit(s->queue, PortId::Completion, s, completion_thunk))) completion_thunk(s, true);
  }
  if (no_payload) release(s);  // the block's reference
}

void CALLBACK do_work_thunk(void* p, bool canceled) {
  auto* s = static_cast<AsyncState*>(p);
  if (canceled) {
    complete_state(s, E_ABORT, 0);
  } else {
    HRESULT hr = call(s, AsyncOp::DoWork);
    if (hr != E_PENDING && FAILED(hr)) complete_state(s, hr, 0);
  }
  release(s);
}

AsyncState* acquire(AsyncBlock* b) {  // caller holds g_async_m
  AsyncState* s = (b && bi(b)->magic == kMagic) ? bi(b)->state : nullptr;
  if (s) add_ref(s);
  return s;
}

HRESULT CALLBACK run_provider(AsyncOp op, const AsyncProviderData* data) {
  switch (op) {
    case AsyncOp::Begin:
      return AsyncSchedule(data->async, 0);
    case AsyncOp::DoWork: {
      // XAsyncCancel does not stop XAsyncRun work: Microsoft's runtime still runs it and
      // completes with the work's result (verified by tests/xtask_gdk_test reference run).
      HRESULT hr = reinterpret_cast<AsyncWork>(data->context)(data->async);
      AsyncComplete(data->async, hr, 0);
      return S_OK;
    }
    default:
      return S_OK;
  }
}

}  // namespace

HRESULT AsyncBegin(AsyncBlock* b, void* ctx, const void* identity, const char*, AsyncProvider p) {
  if (!b || !p) return E_INVALIDARG;
  Queue* q = nullptr;
  if (b->queue ? FAILED(Duplicate(b->queue, &q)) : !GetProcessQueue(&q)) return E_INVALIDARG;
  auto* s = new AsyncState();
  {
    std::lock_guard<std::mutex> lock(g_async_m);
    BlockInternal* in = bi(b);
    if (in->magic == kMagic && in->status == E_PENDING && in->state) {
      Close(q);
      delete s;
      return E_INVALIDARG;  // block already has a call in flight
    }
    s->block = b;
    s->provider = p;
    s->ctx = ctx;
    s->identity = identity;
    s->queue = q;
    s->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    *in = {s, E_PENDING, kMagic, 0, 0};
  }
  add_ref(s);  // the provider may complete synchronously
  HRESULT hr = call(s, AsyncOp::Begin);
  // A failing Begin completes the call with that error (cleanup + completion callback) and
  // XAsyncBegin itself still succeeds — matches Microsoft's runtime.
  if (FAILED(hr) && hr != E_NOTIMPL) complete_state(s, hr, 0);
  release(s);
  return S_OK;
}

HRESULT AsyncSchedule(AsyncBlock* b, uint32_t delay_ms) {
  AsyncState* s;
  {
    std::lock_guard<std::mutex> lock(g_async_m);
    s = acquire(b);
    if (s && s->status != E_PENDING) {
      release(s);
      s = nullptr;
    }
  }
  if (!s) return E_INVALIDARG;
  HRESULT hr = SubmitDelayed(s->queue, PortId::Work, delay_ms, s, do_work_thunk);
  if (FAILED(hr)) release(s);
  return hr;
}

void AsyncComplete(AsyncBlock* b, HRESULT result, size_t required) {
  AsyncState* s;
  {
    std::lock_guard<std::mutex> lock(g_async_m);
    s = acquire(b);
  }
  if (!s) return;
  complete_state(s, result, required);
  release(s);
}

HRESULT AsyncGetStatus(AsyncBlock* b, bool wait) {
  AsyncState* s = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_async_m);
    if (!b || bi(b)->magic != kMagic) return E_INVALIDARG;
    HRESULT hr = bi(b)->status;
    if (hr != E_PENDING || !wait || !bi(b)->state) return hr;
    s = acquire(b);
  }
  WaitForSingleObject(s->done, INFINITE);
  HRESULT hr;
  {
    std::lock_guard<std::mutex> lock(g_async_m);
    hr = s->status;
  }
  release(s);
  return hr;
}

HRESULT AsyncGetResultSize(AsyncBlock* b, size_t* size) {
  if (!size) return E_INVALIDARG;
  std::lock_guard<std::mutex> lock(g_async_m);
  if (!b || bi(b)->magic != kMagic) return E_INVALIDARG;
  HRESULT hr = bi(b)->status;
  if (hr == E_PENDING || FAILED(hr)) return hr;
  *size = static_cast<size_t>(bi(b)->required);
  return S_OK;
}

HRESULT AsyncGetResult(AsyncBlock* b, const void* identity, size_t size, void* buf, size_t* used) {
  AsyncState* s;
  size_t required;
  {
    std::lock_guard<std::mutex> lock(g_async_m);
    if (!b || bi(b)->magic != kMagic) return E_INVALIDARG;
    HRESULT hr = bi(b)->status;
    if (hr == E_PENDING || FAILED(hr)) return hr;
    required = static_cast<size_t>(bi(b)->required);
    if (required == 0) {
      if (used) *used = 0;
      return S_OK;
    }
    s = bi(b)->state;
    if (!s) return E_ILLEGAL_METHOD_CALL;  // result already consumed
    // Any GetResult on a completed call ends it, even when the arguments are rejected
    // (Microsoft's runtime cleans up and later calls get E_ILLEGAL_METHOD_CALL).
    bi(b)->state = nullptr;  // block hands its reference to us
  }
  HRESULT hr;
  if (s->identity && s->identity != identity) {
    hr = E_INVALIDARG;
  } else if (!buf || size < required) {
    hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  } else {
    hr = call(s, AsyncOp::GetResult, buf, size);
    if (hr == E_NOTIMPL) hr = S_OK;
  }
  call(s, AsyncOp::Cleanup);
  release(s);
  if (SUCCEEDED(hr) && used) *used = required;
  return hr;
}

void AsyncCancel(AsyncBlock* b) {
  AsyncState* s;
  {
    std::lock_guard<std::mutex> lock(g_async_m);
    s = acquire(b);
    if (s && s->status != E_PENDING) {
      release(s);
      return;
    }
  }
  if (!s) return;
  s->canceled = true;
  call(s, AsyncOp::Cancel);
  release(s);
}

HRESULT AsyncRun(AsyncBlock* b, AsyncWork work) {
  if (!work) return E_INVALIDARG;
  return AsyncBegin(b, reinterpret_cast<void*>(work), reinterpret_cast<const void*>(&AsyncRun),
                    "XAsyncRun", run_provider);
}

// =========================================================================== vtable glue
// Member-style entry points: first parameter is the interface object (unused).
namespace {
HRESULT v_GetStatus(void*, AsyncBlock* b, bool w) { return AsyncGetStatus(b, w); }
HRESULT v_GetResultSize(void*, AsyncBlock* b, size_t* s) { return AsyncGetResultSize(b, s); }
void v_Cancel(void*, AsyncBlock* b) { AsyncCancel(b); }
HRESULT v_Run(void*, AsyncBlock* b, AsyncWork w) { return AsyncRun(b, w); }
HRESULT v_Begin(void*, AsyncBlock* b, void* c, const void* id, const char* n, AsyncProvider p) {
  return AsyncBegin(b, c, id, n, p);
}
HRESULT v_Schedule(void*, AsyncBlock* b, uint32_t d) { return AsyncSchedule(b, d); }
void v_Complete(void*, AsyncBlock* b, HRESULT r, size_t s) { AsyncComplete(b, r, s); }
HRESULT v_GetResult(void*, AsyncBlock* b, const void* id, size_t s, void* buf, size_t* u) {
  return AsyncGetResult(b, id, s, buf, u);
}
HRESULT v_Create(void*, DispatchMode w, DispatchMode c, Queue** q) { return Create(w, c, q); }
HRESULT v_CreateComposite(void*, Port* w, Port* c, Queue** q) { return CreateComposite(w, c, q); }
HRESULT v_GetPort(void*, Queue* q, PortId p, Port** out) { return GetPort(q, p, out); }
HRESULT v_Duplicate(void*, Queue* q, Queue** out) { return Duplicate(q, out); }
bool v_Dispatch(void*, Queue* q, PortId p, uint32_t t) { return Dispatch(q, p, t); }
void v_Close(void*, Queue* q) { Close(q); }
HRESULT v_Terminate(void*, Queue* q, bool w, void* c, TerminatedCallback cb) {
  return Terminate(q, w, c, cb);
}
HRESULT v_Submit(void*, Queue* q, PortId p, void* c, Callback cb) { return Submit(q, p, c, cb); }
HRESULT v_SubmitDelayed(void*, Queue* q, PortId p, uint32_t d, void* c, Callback cb) {
  return SubmitDelayed(q, p, d, c, cb);
}
HRESULT v_RegisterWaiter(void*, Queue* q, PortId p, HANDLE h, void* c, Callback cb,
                         RegistrationToken* t) {
  return RegisterWaiter(q, p, h, c, cb, t);
}
void v_UnregisterWaiter(void*, Queue* q, RegistrationToken t) { UnregisterWaiter(q, t); }
HRESULT v_RegisterMonitor(void*, Queue* q, void* c, MonitorCallback cb, RegistrationToken* t) {
  return RegisterMonitor(q, c, cb, t);
}
void v_UnregisterMonitor(void*, Queue* q, RegistrationToken t) { UnregisterMonitor(q, t); }
bool v_GetProcessQueue(void*, Queue** q) { return GetProcessQueue(q); }
void v_SetProcessQueue(void*, Queue* q) { SetProcessQueue(q); }

#define GTX_OV(api, fn) {api, reinterpret_cast<void*>(&fn)}
const Override kOverrides[] = {
    GTX_OV("XAsyncGetStatus", v_GetStatus),
    GTX_OV("XAsyncGetResultSize", v_GetResultSize),
    GTX_OV("XAsyncCancel", v_Cancel),
    GTX_OV("XAsyncRun", v_Run),
    GTX_OV("XAsyncBegin", v_Begin),
    GTX_OV("XAsyncSchedule", v_Schedule),
    GTX_OV("XAsyncComplete", v_Complete),
    GTX_OV("XAsyncGetResult", v_GetResult),
    GTX_OV("XTaskQueueCreate", v_Create),
    GTX_OV("XTaskQueueCreateComposite", v_CreateComposite),
    GTX_OV("XTaskQueueGetPort", v_GetPort),
    GTX_OV("XTaskQueueDuplicateHandle", v_Duplicate),
    GTX_OV("XTaskQueueDispatch", v_Dispatch),
    GTX_OV("XTaskQueueCloseHandle", v_Close),
    GTX_OV("XTaskQueueTerminate", v_Terminate),
    GTX_OV("XTaskQueueSubmitCallback", v_Submit),
    GTX_OV("XTaskQueueSubmitDelayedCallback", v_SubmitDelayed),
    GTX_OV("XTaskQueueRegisterWaiter", v_RegisterWaiter),
    GTX_OV("XTaskQueueUnregisterWaiter", v_UnregisterWaiter),
    GTX_OV("XTaskQueueRegisterMonitor", v_RegisterMonitor),
    GTX_OV("XTaskQueueUnregisterMonitor", v_UnregisterMonitor),
    GTX_OV("XTaskQueueGetCurrentProcessTaskQueue", v_GetProcessQueue),
    GTX_OV("XTaskQueueSetCurrentProcessTaskQueue", v_SetProcessQueue),
};
#undef GTX_OV
}  // namespace

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

}  // namespace gtx::xgr::task
