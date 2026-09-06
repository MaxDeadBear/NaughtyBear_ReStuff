# rexglue SDK: wait-multiple is a 1 ms sleep-poll on the POSIX backend; guest FP exception mask can be dropped on APC / host-callback dispatch paths

**Reported against:** rexglue SDK source tree (files/lines cited below re-verified against the checkout at the time of writing)
**Reporter context:** downstream static-recompilation project of a commercial Xbox 360 title (Naughty Bear), running the recompiled binary on a Linux x86-64 host through the SDK runtime.

---

## Summary

Two independent runtime issues, both found by reading the SDK source after profiling/crash-triaging the downstream title:

1. **Performance:** `KeWaitForMultipleObjects` / `NtWaitForMultipleObjectsEx` (and everything else routed through `rex::thread::WaitMultiple`) is implemented on the POSIX backend as a **sleep-poll loop with a 1 ms period**, not a blocking multi-wait. Object signal paths never wake a multi-waiter; the waiter only notices on its next 1 ms poll tick. Every producer→consumer handoff through a multi-wait pays up to a full poll interval of added latency (plus `sleep_for` overshoot), and every parked multi-waiter burns CPU locking and scanning all handles once per millisecond. There is also an unsorted trylock-all/`yield` retry inside the loop that degenerates to a busy-spin under contention. The Windows backend does not have this problem (it uses `WaitForMultipleObjectsEx` natively), so this is POSIX-backend-specific.

2. **Correctness/crash:** the guest FP environment (MXCSR on x86-64) is initialized — all exceptions masked — by `FPSCRRegister::InitHost()`, which is called from exactly **one** place in the runtime: partway through `XThread::Execute()`. Two guest-dispatch paths run recompiled guest code **before/without** that call, on a `PPCContext` whose FPSCR shadow word was `memset` to zero (which on x86 means "all SSE exceptions unmasked"): (a) APCs delivered at the top of `XThread::Execute()` before `InitHost()` runs, and (b) all guest callbacks dispatched on `XHostThread`s (e.g. the audio worker), whose `Execute()` override never calls `InitHost()` at all. On either path, a guest full-field `mtfsf` (recompiled to `ctx.fpscr.storeFromGuest(...)`) writes the zero-mask shadow into the live MXCSR, unmasking every SSE exception — after which the first inexact/denormal/invalid operation raises SIGFPE in code that is architecturally silent on the 360's PPC. The downstream title saw rare one-off SIGFPEs in gameplay float code consistent with this class and had to carry a defensive re-mask.

Suggested fixes for both are outlined below against the SDK's own object/threading model.

---

## Environment

- Host: Linux x86-64 (glibc), POSIX threading backend (`src/core/threading_posix.cpp`), SDK built from source.
- Guest: recompiled X360 PPC title; guest threads created via `ExCreateThread`/`XThread`, frame pacing waits on fences with `KeWaitForMultipleObjects`.
- Issue 1 is specific to the POSIX backend. Issue 2 is host-architecture-general in structure (the ARM64 `FPSCRPlatform` has the same shadow-word design), but all concrete evidence below is x86-64.

---

## Issue 1 — `WaitMultiple` on POSIX is a 1 ms sleep-poll, never woken by `Signal()`

### Dispatch chain

The guest-facing export delegates straight down to the host threading primitive:

`src/kernel/xboxkrnl/xboxkrnl_threading.cpp:885` (`KeWaitForMultipleObjects_entry`, similarly `NtWaitForMultipleObjectsEx_entry` at `:936`):

```cpp
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  X_STATUS result = XObject::WaitMultiple(
      uint32_t(objects.size()), reinterpret_cast<XObject**>(objects.data()), wait_type, wait_reason,
      processor_mode, alertable, timeout_ptr ? &timeout : nullptr);
```
(`src/kernel/xboxkrnl/xboxkrnl_threading.cpp:903-906`)

`XObject::WaitMultiple` (`src/system/xobject.cpp:260`) collects each object's `GetWaitHandle()` and calls `rex::thread::WaitAny` / `WaitAll` (`src/system/xobject.cpp:273-295`), which land in `rex::thread::WaitMultiple` (`src/core/threading_posix.cpp:1107`). The non-alertable case goes directly to the polling core:

```cpp
  if (!is_alertable) {
    return PosixConditionBase::WaitMultiple(std::move(conditions), wait_all, timeout);
  }
```
(`src/core/threading_posix.cpp:1119-1121`)

### The polling structure (verbatim)

`PosixConditionBase::WaitMultiple`, `src/core/threading_posix.cpp:269-368`. Head and the trylock-all pass:

```cpp
  static std::pair<WaitResult, size_t> WaitMultiple(std::vector<PosixConditionBase*>&& handles,
                                                    bool wait_all,
                                                    std::chrono::milliseconds timeout) {
    assert_true(!handles.empty());

    if (handles.size() == 1) {
      auto result = handles[0]->Wait(timeout);
      return std::make_pair(result, 0);
    }

    auto start_time = std::chrono::steady_clock::now();
    auto end_time = (timeout == std::chrono::milliseconds::max())
                        ? std::chrono::steady_clock::time_point::max()
                        : start_time + timeout;

    while (true) {
      size_t first_signaled = std::numeric_limits<size_t>::max();
      bool condition_met = false;
      bool all_locked = true;

      std::vector<std::unique_lock<std::mutex>> locks;
      locks.reserve(handles.size());

      for (size_t i = 0; i < handles.size(); ++i) {
#if REX_PLATFORM_LINUX
        auto native_mutex = static_cast<pthread_mutex_t*>(handles[i]->mutex_.native_handle());
        int result = pthread_mutex_trylock(native_mutex);
        if (result == 0 || result == EOWNERDEAD) {
          if (result == EOWNERDEAD) {
            pthread_mutex_consistent(native_mutex);
          }
          locks.emplace_back(handles[i]->mutex_, std::adopt_lock);
        } else {
          all_locked = false;
          break;
        }
#else
        locks.emplace_back(handles[i]->mutex_, std::try_to_lock);
        if (!locks.back().owns_lock()) {
          all_locked = false;
          break;
        }
#endif
      }

      if (!all_locked) {
        locks.clear();
        std::this_thread::yield();
        continue;
      }
```
(`src/core/threading_posix.cpp:269-318`)

…the loop then scans `signaled()` on each handle (`:320-340`, elided here), and if the condition is not met, unlocks everything and **sleeps for 1 ms** before repeating:

```cpp
      locks.clear();

      auto now = std::chrono::steady_clock::now();
      if (now >= end_time) {
        return std::make_pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
      }

      if (timeout == std::chrono::milliseconds::max()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } else {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now);
        auto sleep_time = std::min(remaining, std::chrono::milliseconds(1));
        std::this_thread::sleep_for(sleep_time);
      }
    }
  }
```
(`src/core/threading_posix.cpp:353-368`)

### Why signals never wake a multi-waiter

The single-object `Wait()` (`src/core/threading_posix.cpp:236-267`) is a genuine `cond_.wait/wait_for` block on the object's condition variable, and the signal path notifies that condvar:

```cpp
  bool Signal() override {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    signal_ = true;
    cond_.notify_all();
    return true;
  }
```
(`PosixCondition<Event>::Signal`, `src/core/threading_posix.cpp:395-400`)

A multi-waiter, however, is **never waiting on any object's `cond_`** — between polls it is inside `std::this_thread::sleep_for`. `notify_all()` therefore has no one to wake; the earliest a multi-waiter can observe a signal is its next poll tick. The multi-wait is a poll in the strict sense: wake-up latency is bounded below only by the poll period, not by the signal.

Two secondary structural problems in the same loop:

- **Unordered trylock-all + `yield` spin** (`:314-318`): whenever any handle's mutex is momentarily held (a signaler in `Signal()`, another multi-waiter's lock pass over an overlapping handle set), the waiter discards all locks and spins on `std::this_thread::yield()` with **no sleep**. Two multi-waiters over overlapping objects can ping-pong here, and because the SDK requests `SCHED_FIFO` for guest thread priorities (`pthread_setschedparam(thread_, SCHED_FIFO, &param)`, `src/core/threading_posix.cpp:749`; whether it takes effect depends on the host's rtprio limits — failures only log), a FIFO-priority waiter yield-spinning against a lower-priority signaler on the same core is a real livelock risk on hosts where that scheduling engages; even without it, the no-sleep spin burns CPU under contention.
- **Alertable waits add a second 1 ms poll layer.** The alertable wrapper re-enters the polling core in `kAlertablePollSlice` chunks:

```cpp
constexpr auto kAlertablePollSlice = std::chrono::milliseconds(1);
```
(`src/core/threading_posix.cpp:965`)

```cpp
  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return std::make_pair(WaitResult::kUserCallback, 0);
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return std::make_pair(WaitResult::kTimeout, 0);
    }
    auto result = PosixConditionBase::WaitMultiple(std::vector<PosixConditionBase*>(conditions),
                                                   wait_all, ComputeAlertableWaitTimeout(deadline));
    if (result.first != WaitResult::kTimeout) {
      return result;
    }
  }
```
(`src/core/threading_posix.cpp:1123-1137`)

  The same 1 ms slicing applies to **single-object alertable waits** (`rex::thread::Wait`, `src/core/threading_posix.cpp:1049-1073`), which means APC delivery latency (drained on alertable-wait return paths, see Issue 2) is also quantized to the poll slice. The slice presumably exists because the `pthread_sigqueue`-based wakeup hint (`QueueUserCallback`, `:788`) does not reliably terminate a `pthread_cond_timedwait` — glibc restarts the futex wait after EINTR — so the poll is compensating for the lack of a real wakeup channel.

For contrast, the Windows backend is a true blocking multi-wait and needs none of this:

```cpp
  DWORD result =
      WaitForMultipleObjectsEx(DWORD(handles.size()), handles.data(), wait_all ? TRUE : FALSE,
                               DWORD(timeout.count()), is_alertable ? TRUE : FALSE);
```
(`src/core/threading_win.cpp:188-190`)

### Impact (downstream, reported observations)

- The title's render-thread handoff waits on fence/event pairs via `KeWaitForMultipleObjects`. Profiling the recompiled title showed multi-millisecond stalls per frame attributable to this handoff: several producer→consumer wakeups per frame, each paying up to a full 1 ms poll interval (mean ~0.5 ms) plus `sleep_for` overshoot (timer slack + scheduler latency; worse under load). At a 33 ms frame budget this is directly frame-pacing-visible.
- Parked multi-waiters are never idle: each waiting thread wakes 1000×/s, takes and releases N mutexes and scans N predicates per wake. The SDK's own audio worker sits in an alertable `WaitAny` over the client-semaphore array (`src/audio/audio_system.cpp:105-106`) and is on this path continuously.
- Nothing here is title-specific: any guest producer/consumer built on wait-multiple (fences, job queues, sound streaming) inherits the same per-handoff latency floor.

### Suggested fix direction

Keep the existing object model (`PosixConditionBase` = `mutex_` + `cond_` + `signaled()`/`post_execution()`) and make multi-waiters **registered, signal-woken waiters** instead of pollers — effectively a KWAIT_BLOCK:

1. Add a per-wait block owned by the waiting thread, e.g. `struct MultiWaitBlock { std::mutex m; std::condition_variable cv; bool wake = false; };`, and give `PosixConditionBase` a registration list `std::vector<MultiWaitBlock*> multi_waiters_;` guarded by its existing `mutex_`.
2. In `WaitMultiple`: sort the handle set by address and lock all `mutex_` in that global order (deadlock-free, and it deletes the trylock/`yield` retry entirely). Evaluate the any/all predicate; if met, run `post_execution()` under the held locks exactly as today (this keeps wait-all consumption atomic). Otherwise register the block with every handle, drop the handle locks, and block on `wb.cv.wait_until(deadline)` — a real wait, no slice. On wake, re-lock in order, re-evaluate, consume or re-arm; deregister on every exit path.
3. In the signal paths (`Signal()` for Event/Semaphore/Mutant/Thread-exit — every site that flips `signaled()` under `mutex_` and calls `cond_.notify_all()`), additionally walk `multi_waiters_` and `notify` each registered block. The signal path already holds `mutex_`, so registration-list access needs no new locking.
4. Route the APC wakeup through the same mechanism: publish the current wait block in TLS while a thread is inside `Wait`/`WaitMultiple`, and have `QueueUserCallback` (`src/core/threading_posix.cpp:765`) notify it in addition to (or instead of) `pthread_sigqueue`. That removes `kAlertablePollSlice` for single and multi alertable waits alike, and drops APC delivery latency from "up to 1 ms" to a direct wake.

Alternative implementations with the same effect: per-object `eventfd` + `epoll_wait` (kernel-level multi-wait), or `futex_waitv` on Linux ≥ 5.16; the wait-block/condvar variant above is the smallest change that stays portable across the POSIX targets and keeps the current class layout.

---

## Issue 2 — guest FP exception mask can be dropped on APC / host-callback guest dispatch (SIGFPE class)

### Background: what the guest is entitled to

On the 360, title threads run with `MSR.FE0/FE1 = 0` and VMX128 in non-Java mode: FP invalid/denormal/inexact conditions are architecturally **silent** (default results, no traps). The SDK models this by keeping a per-context shadow of the host CSR and masking everything at init:

```cpp
struct FPSCRRegister {
  uint32_t csr;
```
(`include/rex/ppc/types.h:464-465` — note: no default initializer)

```cpp
  // Initialize MXCSR/FPCR with all FP exceptions masked
  inline void InitHost() noexcept {
    csr = getcsr();
    Platform::InitHostExceptions(csr);
    setcsr(csr);
  }
```
(`include/rex/ppc/types.h:513-518`)

```cpp
  // Exception mask bits (1 = exception masked/disabled)
  static constexpr uint32_t ExceptionMask = (1 << 7) |   // IM - Invalid operation
                                            (1 << 8) |   // DM - Denormal operand
                                            (1 << 9) |   // ZM - Zero divide
                                            (1 << 10) |  // OM - Overflow
                                            (1 << 11) |  // UM - Underflow
                                            (1 << 12);   // PM - Precision (Inexact)
  ...
  static inline void InitHostExceptions(uint32_t& csr) noexcept {
    csr |= ExceptionMask;  // Set mask bits to disable exceptions
  }
```
(`include/rex/ppc/detail/fpscr.h:32-38, 46-48`)

The shadow starts at **zero**, because the whole `PPCContext` is zero-filled at `ThreadState` construction:

```cpp
  // Initialize the PPCContext (context_ already points to context_storage_)
  std::memset(context_, 0, sizeof(::PPCContext));
```
(`src/system/thread_state.cpp:35-36`)

On x86, MXCSR bits 7-12 are *mask* bits (1 = masked), so a zero shadow encodes "**every SSE exception unmasked**". That is harmless until something writes the shadow into the live register. Recompiled code does exactly that for a full-field `mtfsf` (the common compiler-emitted form, `mtfsf 255,fN`):

```cpp
bool build_mtfsf(BuilderContext& ctx) {
  ...
  if (mask == 0xFFFFFFFF) {
    ctx.println("\tctx.fpscr.storeFromGuest({}.u32);", ctx.f(ctx.insn.operands[1]));
  } else {
    ctx.println(
        "\tctx.fpscr.storeFromGuest((ctx.fpscr.loadFromHost() & 0x{:08X}) | ({}.u32 & 0x{:08X}));",
        ~mask, ctx.f(ctx.insn.operands[1]), mask);
  }
```
(`src/codegen/builders/system.cpp:338-351`; note the partial-field form is self-healing because `loadFromHost()` re-captures the real host CSR first — the full-field form is not)

```cpp
  inline void storeFromGuest(uint32_t value) noexcept {
    csr &= ~RoundMaskVal;
    csr |= Platform::GuestToHost[value & kRoundMask];
    setcsr(csr);
  }
```
(`include/rex/ppc/types.h:483-487`)

`storeFromGuest` on a never-initialized shadow performs `setcsr(rounding-bits-only)`: MXCSR mask bits 7-12 all cleared. From that point, the first FP operation on that thread that sets *any* status flag — and PM/inexact fires on nearly every operation — delivers SIGFPE, in guest code that could never trap on the original hardware.

`InitHost()` is called from exactly one site in the runtime source:

```
$ grep -rn "InitHost\b" src include
src/system/xthread.cpp:616:  ctx->fpscr.InitHost();
include/rex/ppc/types.h:514:  inline void InitHost() noexcept {
```

So the hazard reduces to: *which paths execute guest code on a context before/without `XThread::Execute` reaching line 616?* We found two.

### Definite hazard site A — APCs delivered before `InitHost()` in `XThread::Execute`

```cpp
void XThread::Execute() {
  REXSYS_DEBUG("Execute thid {} (handle={:08X}, '{}', native={:08X})", thread_id_, handle(),
               thread_name_, thread_->system_id());

  // Let the kernel know we are starting.
  kernel_state_->OnThreadExecute(this);

  // Dispatch any APCs that were queued before the thread was created first.
  DeliverAPCs();
```
(`src/system/xthread.cpp:548-556`)

`ctx->fpscr.InitHost();` only happens 60 lines later, at `src/system/xthread.cpp:616`, after argument setup. `DeliverAPCs()` (`src/system/xthread.cpp:697-763`) runs the guest kernel/normal APC routines through `dispatcher->Execute(thread_state_.get(), ...)` (`:727`, `:745`) on the thread's own — still zero-shadow — context. Any APC queued against a not-yet-started thread (`NtQueueApcThread` on a fresh thread, `XTimer` callbacks via `EnqueueApc`, etc.) therefore executes guest code in the unmask-capable window. `InitHost()` at `:616` re-masks afterwards, so the window is bounded — but any full-field `mtfsf` + subsequent FP inside that window is a SIGFPE. `FunctionDispatcher::Execute` itself (`src/system/function_dispatcher.cpp:32-61`, `:63-102`) touches no FP state, so there is no compensating re-mask anywhere on this path. (`OnThreadExecute` is currently a no-op — `src/system/kernel_state.cpp:755-765` — so `DeliverAPCs` is the only pre-`InitHost` guest dispatch in this function today.)

### Definite hazard site B — `XHostThread::Execute` never calls `InitHost()` at all

```cpp
void XHostThread::Execute() {
  REXSYS_INFO("XThread::Execute thid {} (handle={:08X}, '{}', native={:08X}, <host>)", thread_id_,
              handle(), thread_name_, thread_->system_id());

  // Let the kernel know we are starting.
  kernel_state_->OnThreadExecute(this);

  int ret = host_fn_();

  // Exit.
  Exit(ret);
}
```
(`src/system/xthread.cpp:1420-1431`)

The audio worker is an `XHostThread` (`src/audio/audio_system.cpp:83-87`) and dispatches guest client callbacks on its own thread state:

```cpp
        uint64_t args[] = {client_callback_arg};
        function_dispatcher_->Execute(worker_thread_->thread_state(), client_callback, args,
                                      rex::countof(args));
```
(`src/audio/audio_system.cpp:144-146`)

This thread runs guest code for the process lifetime with a zero FPSCR shadow, and — unlike site A — there is no later `InitHost()` to restore the mask: one full-field `mtfsf` anywhere in the guest audio callback graph permanently unmasks all SSE exceptions on the audio worker.

### The hazard, stated generally

Any path that round-trips FP control state through a context without re-applying the guest's expected exception mask can convert architecturally-silent PPC FP conditions into host SIGFPE. In this SDK the FP "context" is the `fpscr.csr` shadow; the two sites above are the ones where the shadow is live-written from its unmasked reset value. One further path round-trips the *hardware* FP state through a saved context: the POSIX fiber backend (used by the guest `CreateFiber`/`SwitchToFiber` emulation, `src/kernel/crt/threading.cpp:244`, `:329`) is `getcontext`/`makecontext`/`swapcontext`-based (`src/core/fiber_posix.cpp:35-63`); on glibc x86-64, `swapcontext` restores MXCSR from the target ucontext, so the first switch into a `Fiber::Create`d fiber resumes with the MXCSR the *creating* thread had at `getcontext` time (`src/core/fiber_posix.cpp:41`), not the switching guest thread's. That is mask-preserving in practice today (creators are guest threads), but it is the same class of hazard and worth covering in the same audit.

### Paths audited and found clean

For completeness, the APC/signal plumbing itself does **not** perturb FP state:

- The `kThreadUserCallback` signal handler is an empty wakeup hint — no guest code, no FP touch (`src/core/threading_posix.cpp:1405-1416`: "Callbacks are drained from alertable waits in normal thread context. / This signal is only used as a wakeup hint."), and `sigreturn` restores the interrupted thread's full FP state from the kernel-saved frame.
- All three `QueueUserCallback` call sites pass empty lambdas (`src/system/xthread.cpp:662`, `:694`; `src/kernel/xboxkrnl/xboxkrnl_threading.cpp:1149` in `NtQueueApcThread_entry`), and `DispatchQueuedUserCallbacks` runs only in normal thread context, inside the alertable wait/sleep loops (via `DispatchCurrentThreadUserCallback`: `src/core/threading_posix.cpp:181` in `AlertableSleep`, `:1062`, `:1094`, `:1126`) or a same-thread alertable fast path (`:775-779`). Actual guest APC routines execute in `XThread::DeliverAPCs` in normal context.
- The suspend path blocks inside the handler on `sem_wait` (`src/core/threading_posix.cpp:1407-1412`, `:909-914`) and resumes via `sigreturn` — FP state kernel-restored.
- The SIGSEGV/SIGILL exception handler copies only the XMM *data* registers in and out of the `mcontext` (`std::memcpy(thread_context.xmm_registers, mcontext.fpregs->_xmm, ...)`, `src/core/exception_handler_posix.cpp:70-71`; write-back of modified registers only, `:177-182`); the `fpregs` MXCSR field is never touched on x86-64.

### Impact (downstream, reported observations)

- The title exhibited rare one-off SIGFPE crashes in gameplay float code, of the FP-invalid/denormal class that is silent on the 360 (MSR FE0/FE1 = 0, VMX non-Java). They correlated with APC-active periods and did not reproduce deterministically; a defensive MXCSR re-mask on the affected paths eliminated them. We did not catch the faulting site under a debugger, so we cannot say which of the two in-source sites (or the fiber path) fired in our case — but sites A and B above are concrete, reachable unmask mechanisms, and site B in particular leaves a thread permanently unmasked.

### Suggested fix direction

1. **Ordering/coverage (root fix):** run `ctx->fpscr.InitHost()` on the executing thread *before any guest dispatch* — i.e. at the top of `XThread::Execute()` (before `OnThreadExecute`/`DeliverAPCs`, `src/system/xthread.cpp:553-556`) and in `XHostThread::Execute()` (`:1420`). Note it must run on the executing thread (it reads `getcsr()`), so it cannot simply move into the `ThreadState` constructor, which runs on the creating thread.
2. **Hardening (kills the class):** make `storeFromGuest` incapable of dropping mask bits — apply `Platform::InitHostExceptions(csr)` (or a `SanitizeExceptions` helper) before `setcsr` in `include/rex/ppc/types.h:483-487`. Guest FPSCR enable bits have no legitimate mapping to host traps in this SDK (PPC FP trap semantics are not modeled anywhere), so unconditional masking is behavior-preserving and makes the invariant independent of init order. A debug assert in `FunctionDispatcher::Execute` that the context's shadow has the mask bits set would catch future regressions.
3. **APC fidelity (optional):** save/restore `ctx->fpscr` (shadow + live CSR) around the guest routine calls in `DeliverAPCs` (`src/system/xthread.cpp:723-750`). NT delivers user APCs against a captured CONTEXT restored via `NtContinue`, so an APC routine's FP-control changes (rounding mode included) never leak into the interrupted computation; today they do.
4. **Fiber audit:** re-apply the owning thread's FP environment (`setcsr(ctx->fpscr.csr)` equivalent) after `Fiber::SwitchTo` lands in a fiber for the first time, or scrub the ucontext fpstate at `Fiber::Create`, so guest fibers never inherit the creator's CSR (`src/core/fiber_posix.cpp:41`, `:59-63`).

---

## Closing

Both issues were found by source reading driven by symptoms in one downstream title, and every SDK code claim above was re-verified against the tree at the cited file:line. Happy to test patches for either issue against the title — the wait-multiple handoff stall and the (defensively-masked) SIGFPE path are both reproducible in our environment. The wait-block design in Issue 1 also gives Issue 2's APC delivery a real wakeup channel, so the two fixes compose well.
