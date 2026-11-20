// XTaskQueue + XAsync behaviour through Microsoft's real static xgameruntime.lib (T-014).
#include "gdk_test_util.h"

#include <XAsync.h>
#include <XAsyncProvider.h>
#include <XGameRuntime.h>
#include <XTaskQueue.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

struct Counter {
  std::atomic<int> runs{0};
  std::atomic<int> canceled{0};
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  ~Counter() { CloseHandle(event); }
};

void CALLBACK count_cb(void* ctx, bool canceled) {
  auto* c = static_cast<Counter*>(ctx);
  (canceled ? c->canceled : c->runs)++;
  SetEvent(c->event);
}

void test_manual_queue() {
  XTaskQueueHandle q = nullptr;
  CHECK(SUCCEEDED(XTaskQueueCreate(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &q)));
  Counter c;
  CHECK(!XTaskQueueDispatch(q, XTaskQueuePort::Work, 0));  // empty
  CHECK(SUCCEEDED(XTaskQueueSubmitCallback(q, XTaskQueuePort::Work, &c, count_cb)));
  CHECK(c.runs == 0);  // manual: nothing runs until dispatched
  CHECK(!XTaskQueueDispatch(q, XTaskQueuePort::Completion, 0));  // other port is empty
  CHECK(XTaskQueueDispatch(q, XTaskQueuePort::Work, 0));
  CHECK(c.runs == 1);
  XTaskQueueCloseHandle(q);
}

void test_threadpool_and_immediate() {
  XTaskQueueHandle q = nullptr;
  CHECK(SUCCEEDED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::Immediate, &q)));
  Counter c;
  CHECK(SUCCEEDED(XTaskQueueSubmitCallback(q, XTaskQueuePort::Work, &c, count_cb)));
  CHECK(WaitForSingleObject(c.event, 5000) == WAIT_OBJECT_0);
  CHECK(c.runs == 1);
  CHECK(SUCCEEDED(XTaskQueueSubmitCallback(q, XTaskQueuePort::Completion, &c, count_cb)));
  CHECK(c.runs == 2);  // immediate: ran inline
  CHECK(!XTaskQueueDispatch(q, XTaskQueuePort::Work, 0));  // non-manual ports never dispatch
  XTaskQueueCloseHandle(q);
}

struct Order {
  std::mutex m;
  std::vector<int> seen;
  std::atomic<int> active{0};
  std::atomic<bool> overlap{false};
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ~Order() { CloseHandle(done); }
};
struct OrderItem {
  Order* o;
  int i;
};

void CALLBACK order_cb(void* ctx, bool) {
  auto* it = static_cast<OrderItem*>(ctx);
  if (it->o->active.fetch_add(1) != 0) it->o->overlap = true;
  Sleep(0);
  bool last;
  {
    std::lock_guard<std::mutex> lock(it->o->m);
    it->o->seen.push_back(it->i);
    last = it->o->seen.size() == 64;
  }
  it->o->active.fetch_sub(1);
  if (last) SetEvent(it->o->done);  // must be the final touch: the test frees `o` after this
}

void test_serialized_order() {
  XTaskQueueHandle q = nullptr;
  CHECK(SUCCEEDED(XTaskQueueCreate(XTaskQueueDispatchMode::SerializedThreadPool, XTaskQueueDispatchMode::Manual, &q)));
  Order o;
  std::vector<OrderItem> items(64);
  for (int i = 0; i < 64; ++i) {
    items[i] = {&o, i};
    CHECK(SUCCEEDED(XTaskQueueSubmitCallback(q, XTaskQueuePort::Work, &items[i], order_cb)));
  }
  CHECK(WaitForSingleObject(o.done, 5000) == WAIT_OBJECT_0);
  bool in_order = true;
  for (int i = 0; i < 64; ++i) in_order = in_order && o.seen[i] == i;
  CHECK(in_order);
  CHECK(!o.overlap);
  XTaskQueueCloseHandle(q);
}

void test_delayed_and_terminate() {
  XTaskQueueHandle q = nullptr;
  CHECK(SUCCEEDED(XTaskQueueCreate(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &q)));
  Counter c;
  ULONGLONG t0 = GetTickCount64();
  CHECK(SUCCEEDED(XTaskQueueSubmitDelayedCallback(q, XTaskQueuePort::Work, 60, &c, count_cb)));
  CHECK(!XTaskQueueDispatch(q, XTaskQueuePort::Work, 0));       // not due yet
  CHECK(XTaskQueueDispatch(q, XTaskQueuePort::Work, 2000));     // arrives after the delay
  ULONGLONG elapsed = GetTickCount64() - t0;
  std::fprintf(stderr, "  delayed 60 ms item dispatched after %llu ms\n", elapsed);
  CHECK(elapsed >= 30 && elapsed < 1000);  // tick granularity ~16 ms; MS runtime fires slightly early
  CHECK(c.runs == 1);

  // Terminate cancels both queued and delayed items, then calls the terminated callback.
  Counter pending;
  CHECK(SUCCEEDED(XTaskQueueSubmitCallback(q, XTaskQueuePort::Work, &pending, count_cb)));
  CHECK(SUCCEEDED(XTaskQueueSubmitDelayedCallback(q, XTaskQueuePort::Completion, 100000, &pending, count_cb)));
  static std::atomic<int> terminated{0};
  // wait=false: on a Manual queue, termination may need the owner to keep dispatching.
  CHECK(SUCCEEDED(XTaskQueueTerminate(q, false, &terminated, [](void* p) {
    static_cast<std::atomic<int>*>(p)->fetch_add(1);
  })));
  int canceled_inline = pending.canceled;
  ULONGLONG deadline = GetTickCount64() + 5000;
  int pumped = 0;
  while (terminated == 0 && GetTickCount64() < deadline) {
    pumped += XTaskQueueDispatch(q, XTaskQueuePort::Work, 10);
    pumped += XTaskQueueDispatch(q, XTaskQueuePort::Completion, 10);
  }
  std::fprintf(stderr, "  terminate: canceled inline=%d, dispatched=%d, canceled total=%d\n",
               canceled_inline, pumped, pending.canceled.load());
  CHECK(pending.canceled == 2 && pending.runs == 0);
  CHECK(terminated == 1);
  CHECK(FAILED(XTaskQueueSubmitCallback(q, XTaskQueuePort::Work, &c, count_cb)));
  XTaskQueueCloseHandle(q);
}

void test_monitor_waiter_composite() {
  XTaskQueueHandle q = nullptr;
  CHECK(SUCCEEDED(XTaskQueueCreate(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &q)));
  static std::atomic<int> monitored{0};
  XTaskQueueRegistrationToken mon{};
  CHECK(SUCCEEDED(XTaskQueueRegisterMonitor(q, nullptr, [](void*, XTaskQueueHandle, XTaskQueuePort port) {
    if (port == XTaskQueuePort::Work) monitored++;
  }, &mon)));
  Counter c;
  CHECK(SUCCEEDED(XTaskQueueSubmitCallback(q, XTaskQueuePort::Work, &c, count_cb)));
  CHECK(monitored == 1);
  XTaskQueueUnregisterMonitor(q, mon);
  CHECK(XTaskQueueDispatch(q, XTaskQueuePort::Work, 0));

  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  XTaskQueueRegistrationToken wt{};
  CHECK(SUCCEEDED(XTaskQueueRegisterWaiter(q, XTaskQueuePort::Completion, ev, &c, count_cb, &wt)));
  CHECK(!XTaskQueueDispatch(q, XTaskQueuePort::Completion, 0));
  SetEvent(ev);
  CHECK(XTaskQueueDispatch(q, XTaskQueuePort::Completion, 2000));
  CHECK(c.runs == 2);
  XTaskQueueUnregisterWaiter(q, wt);
  CloseHandle(ev);

  // Composite: work goes to q's work port, completion to a thread-pool queue.
  XTaskQueueHandle tp = nullptr, comp = nullptr;
  XTaskQueuePortHandle work_port = nullptr, done_port = nullptr;
  CHECK(SUCCEEDED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::ThreadPool, &tp)));
  CHECK(SUCCEEDED(XTaskQueueGetPort(q, XTaskQueuePort::Work, &work_port)));
  CHECK(SUCCEEDED(XTaskQueueGetPort(tp, XTaskQueuePort::Completion, &done_port)));
  CHECK(SUCCEEDED(XTaskQueueCreateComposite(work_port, done_port, &comp)));
  XTaskQueueCloseHandle(tp);  // composite keeps the source alive
  Counter cc;
  CHECK(SUCCEEDED(XTaskQueueSubmitCallback(comp, XTaskQueuePort::Work, &cc, count_cb)));
  CHECK(XTaskQueueDispatch(q, XTaskQueuePort::Work, 0));  // landed on q's manual port
  ResetEvent(cc.event);  // the inline run above already signaled it
  CHECK(SUCCEEDED(XTaskQueueSubmitCallback(comp, XTaskQueuePort::Completion, &cc, count_cb)));
  CHECK(WaitForSingleObject(cc.event, 5000) == WAIT_OBJECT_0);
  CHECK(cc.runs == 2);
  XTaskQueueHandle dup = nullptr;
  CHECK(SUCCEEDED(XTaskQueueDuplicateHandle(comp, &dup)));
  XTaskQueueCloseHandle(dup);
  XTaskQueueCloseHandle(comp);
  XTaskQueueCloseHandle(q);
}

// ---------------------------------------------------------------- XAsync
std::atomic<int> g_work_runs{0};
HRESULT CALLBACK work_ok(XAsyncBlock*) {
  g_work_runs++;
  return S_OK;
}
HRESULT CALLBACK work_fail(XAsyncBlock*) { return E_UNEXPECTED; }

void test_async_run_manual() {
  XTaskQueueHandle q = nullptr;
  CHECK(SUCCEEDED(XTaskQueueCreate(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &q)));
  static std::atomic<int> completions{0};
  XAsyncBlock b{};
  b.queue = q;
  b.callback = [](XAsyncBlock* blk) {
    CHECK(XAsyncGetStatus(blk, false) != E_PENDING);  // status is final inside the callback
    completions++;
  };
  int before = g_work_runs;
  CHECK(SUCCEEDED(XAsyncRun(&b, work_ok)));
  CHECK(XAsyncGetStatus(&b, false) == E_PENDING);
  CHECK(XTaskQueueDispatch(q, XTaskQueuePort::Work, 0));
  CHECK(g_work_runs == before + 1);
  CHECK(XAsyncGetStatus(&b, false) == S_OK);
  CHECK(completions == 0);  // completion port not dispatched yet
  CHECK(XTaskQueueDispatch(q, XTaskQueuePort::Completion, 0));
  CHECK(completions == 1);
  size_t size = 99;
  CHECK(XAsyncGetResultSize(&b, &size) == S_OK && size == 0);

  // Block can be reused once the previous call is done; failure HRESULT propagates.
  CHECK(SUCCEEDED(XAsyncRun(&b, work_fail)));
  CHECK(XTaskQueueDispatch(q, XTaskQueuePort::Work, 0));
  CHECK(XAsyncGetStatus(&b, false) == E_UNEXPECTED);
  CHECK(XTaskQueueDispatch(q, XTaskQueuePort::Completion, 0));

  // Cancel before the work is dispatched.
  int runs = g_work_runs;
  CHECK(SUCCEEDED(XAsyncRun(&b, work_ok)));
  XAsyncCancel(&b);
  HRESULT after_cancel = XAsyncGetStatus(&b, false);
  bool work_dispatched = XTaskQueueDispatch(q, XTaskQueuePort::Work, 0);
  HRESULT after_dispatch = XAsyncGetStatus(&b, false);
  bool completion_dispatched = XTaskQueueDispatch(q, XTaskQueuePort::Completion, 0);
  std::fprintf(stderr, "  cancel: status=%08lx, work dispatched=%d -> status=%08lx, work ran=%d, completion=%d\n",
               after_cancel, work_dispatched, after_dispatch, g_work_runs - runs, completion_dispatched);
  // Verified against Microsoft's runtime: cancel does not stop XAsyncRun work.
  CHECK_EQ(after_cancel, E_PENDING);
  CHECK(work_dispatched);
  CHECK_EQ(after_dispatch, S_OK);
  CHECK_EQ(g_work_runs - runs, 1);
  CHECK(completion_dispatched);
  while (XTaskQueueDispatch(q, XTaskQueuePort::Work, 0) || XTaskQueueDispatch(q, XTaskQueuePort::Completion, 0)) {
  }
  XTaskQueueCloseHandle(q);
}

void test_async_run_default_queue_wait() {
  XAsyncBlock b{};  // no queue: process default (thread pool)
  int before = g_work_runs;
  CHECK(SUCCEEDED(XAsyncRun(&b, work_ok)));
  CHECK(XAsyncGetStatus(&b, true) == S_OK);
  CHECK(g_work_runs == before + 1);

  XTaskQueueHandle proc = nullptr;
  CHECK(XTaskQueueGetCurrentProcessTaskQueue(&proc) && proc);
  XTaskQueueCloseHandle(proc);
}

// A provider with a 4-byte payload: Begin schedules, DoWork completes, GetResult copies.
struct ProviderLog {
  std::atomic<int> begin{0}, work{0}, result{0}, cleanup{0};
};
HRESULT CALLBACK payload_provider(XAsyncOp op, const XAsyncProviderData* data) {
  auto* log = static_cast<ProviderLog*>(data->context);
  switch (op) {
    case XAsyncOp::Begin:
      log->begin++;
      return XAsyncSchedule(data->async, 0);
    case XAsyncOp::DoWork:
      log->work++;
      XAsyncComplete(data->async, S_OK, sizeof(uint32_t));
      return S_OK;
    case XAsyncOp::GetResult:
      log->result++;
      *static_cast<uint32_t*>(data->buffer) = 0xC0FFEEu;
      return S_OK;
    case XAsyncOp::Cleanup:
      log->cleanup++;
      return S_OK;
    default:
      return S_OK;
  }
}

void test_async_provider_payload() {
  ProviderLog log;
  XAsyncBlock b{};
  static const char identity = 0;
  CHECK(SUCCEEDED(XAsyncBegin(&b, &log, &identity, "payload_provider", payload_provider)));
  CHECK(XAsyncGetStatus(&b, true) == S_OK);
  size_t size = 0;
  CHECK(XAsyncGetResultSize(&b, &size) == S_OK && size == 4);
  CHECK(log.cleanup == 0);  // payload pending: no cleanup yet
  uint32_t value = 0;
  size_t used = 0;
  CHECK_EQ(XAsyncGetResult(&b, &identity, sizeof(value), &value, &used), S_OK);
  CHECK(value == 0xC0FFEEu && used == 4);
  // MS runtime may run Cleanup slightly after GetResult returns; allow a short settle time.
  for (int i = 0; i < 100 && log.cleanup == 0; ++i) Sleep(10);
  CHECK(log.begin == 1 && log.work == 1 && log.result == 1 && log.cleanup == 1);
  CHECK_EQ(XAsyncGetResult(&b, &identity, sizeof(value), &value, &used), E_ILLEGAL_METHOD_CALL);
  CHECK_EQ(XAsyncGetStatus(&b, false), S_OK);

  // A rejected GetResult (small buffer / wrong identity) still ends the call: verified vs MS runtime.
  for (int variant = 0; variant < 2; ++variant) {
    ProviderLog lg;
    XAsyncBlock m{};
    CHECK(SUCCEEDED(XAsyncBegin(&m, &lg, &identity, "payload_provider", payload_provider)));
    CHECK_EQ(XAsyncGetStatus(&m, true), S_OK);
    char other = 0;
    uint32_t tiny = 0;
    HRESULT first = variant == 0 ? XAsyncGetResult(&m, &identity, 2, &tiny, nullptr)
                                 : XAsyncGetResult(&m, &other, sizeof(value), &value, &used);
    CHECK_EQ(first, variant == 0 ? HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) : E_INVALIDARG);
    CHECK_EQ(XAsyncGetResult(&m, &identity, sizeof(value), &value, &used), E_ILLEGAL_METHOD_CALL);
    for (int i = 0; i < 100 && lg.cleanup == 0; ++i) Sleep(10);
    CHECK_EQ(lg.result.load(), 0);
    CHECK_EQ(lg.cleanup.load(), 1);
  }

  // Provider-driven cancel: provider completes with E_ABORT when asked.
  XAsyncBlock c{};
  XTaskQueueHandle manual = nullptr;
  CHECK(SUCCEEDED(XTaskQueueCreate(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Immediate, &manual)));
  c.queue = manual;
  CHECK(SUCCEEDED(XAsyncBegin(&c, nullptr, nullptr, nullptr, [](XAsyncOp op, const XAsyncProviderData* d) {
    if (op == XAsyncOp::Begin) return XAsyncSchedule(d->async, 0);
    if (op == XAsyncOp::Cancel) XAsyncComplete(d->async, E_ABORT, 0);
    return S_OK;
  })));
  XAsyncCancel(&c);
  CHECK_EQ(XAsyncGetStatus(&c, false), E_ABORT);
  while (XTaskQueueDispatch(manual, XTaskQueuePort::Work, 0)) {
  }
  CHECK_EQ(XAsyncGetStatus(&c, false), E_ABORT);
  XTaskQueueCloseHandle(manual);

  // Begin failure: XAsyncBegin succeeds, the call completes with the error (callback + cleanup).
  static std::atomic<int> called{0};
  static std::atomic<int> cleanups{0};
  XAsyncBlock f{};
  f.callback = [](XAsyncBlock*) { called++; };
  HRESULT begin = XAsyncBegin(&f, nullptr, nullptr, nullptr, [](XAsyncOp op, const XAsyncProviderData*) {
    if (op == XAsyncOp::Cleanup) cleanups++;
    return op == XAsyncOp::Begin ? E_OUTOFMEMORY : S_OK;
  });
  HRESULT status = XAsyncGetStatus(&f, true);
  Sleep(50);
  std::fprintf(stderr, "  begin failure: XAsyncBegin=%08lx status=%08lx callback=%d cleanup=%d\n", begin,
               status, called.load(), cleanups.load());
  CHECK_EQ(begin, S_OK);
  CHECK_EQ(status, E_OUTOFMEMORY);
  CHECK_EQ(called.load(), 1);
  CHECK_EQ(cleanups.load(), 1);
}

// Callers may free the async block inside the completion callback.
void test_free_block_in_callback() {
  static HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  auto* b = new XAsyncBlock{};
  b->callback = [](XAsyncBlock* blk) {
    delete blk;
    SetEvent(done);
  };
  CHECK(SUCCEEDED(XAsyncRun(b, work_ok)));
  CHECK(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0);
}

// Title/sandbox ids for a program without MicrosoftGame.config (like the test exes).
void test_identity_apis() {
  uint32_t title = 0xDEADBEEF;
  HRESULT hr = XGameGetXboxTitleId(&title);
  char sandbox[XSystemXboxLiveSandboxIdMaxBytes] = {};
  size_t used = 0;
  HRESULT hs = XSystemGetXboxLiveSandboxId(sizeof(sandbox), sandbox, &used);
  // Expected values observed from Microsoft's runtime (reference run).
  CHECK_EQ(hr, HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
  CHECK_EQ(title, 0xDEADBEEFu);
  CHECK_EQ(hs, S_OK);
  CHECK(std::strcmp(sandbox, "RETAIL") == 0);
  CHECK_EQ(used, static_cast<size_t>(7));
  XThreadAssertNotTimeSensitive();
}

}  // namespace

int main() {
#ifdef GTX_REFERENCE_RUNTIME
  // Same assertions against Microsoft's installed runtime: proves GTX matches real behaviour.
  if (FAILED(XGameRuntimeInitialize())) {
    std::puts("xtask_gdk_test (reference): Gaming Services runtime not available, skipped");
    return 0;
  }
  CHECK(GetModuleHandleW(L"xgameruntime.dll") != nullptr);
#else
  CHECK(preload_shim());
  CHECK(SUCCEEDED(XGameRuntimeInitialize()));
#endif
#define RUN(t) std::fprintf(stderr, "- %s\n", #t), t()
  RUN(test_manual_queue);
  RUN(test_threadpool_and_immediate);
  RUN(test_serialized_order);
  RUN(test_delayed_and_terminate);
  RUN(test_monitor_waiter_composite);
  RUN(test_async_run_manual);
  RUN(test_async_run_default_queue_wait);
  RUN(test_async_provider_payload);
  RUN(test_free_block_in_callback);
  RUN(test_identity_apis);
  XGameRuntimeUninitialize();
  if (g_failures == 0) std::puts("xtask_gdk_test: all passed");
  return g_failures ? 1 : 0;
}
