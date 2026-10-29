// DOA4 netplay phase 0: shared parts of the determinism layer's files
// (np_det.cpp, np_det_middleware.cpp, np_det_workers.cpp, np_det_trace.cpp).

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>

#include <rex/ppc/context.h>

namespace doa4::np::det {

enum Fix : uint32_t {
  kSeed = 1u << 0,      // MT seeds from the session seed
  kClock = 1u << 1,     // the game's clock wrappers on frame time
  kSdkClock = 1u << 2,  // inline mftb / direct KeQuerySystemTime on frame time (SDK override)
  kXam = 1u << 3,       // XAM overlapped completions inline, dialogs headless
  kAudio = 1u << 4,     // guest audio callbacks budgeted per frame
  kMovie = 1u << 5,     // movies skipped
  kLoader = 1u << 6,    // loader thread jobs only at the frame boundary
  kCri = 1u << 7,       // CRI server passes on the main thread (+ spin steps)
  kCriJob = 1u << 8,    // CRI job-thread jobs inline
  kWorkers = 1u << 9,   // job workers one at a time in a fixed order
};

bool On(uint32_t fix);
bool Stopped();                // the session has ended
uint32_t Frame();              // the frame being run (set at the frame hook)
uint32_t ThreadStart();        // the calling thread's guest entry point (0: none)
bool IsMainThread();

// Counters, written per frame to counters.d4k by the session (np_core.h). fb_*: a wait gave up
// and the game ran on anyway - any non-zero value fails the gate; pr_*: progress, equal in every
// run of one recording.
enum Counter : int {
  kFbWorkerToken,    // a job worker waited 2 s for its turn
  kFbMainWait,       // the main thread waited 2 s for the job workers
  kFbUnranked,       // a job worker ran without its rank (modes 2 and 3)
  kFbLoaderOutside,  // a loader job waited 500 ms for the frame boundary
  kFbLoaderIdle,     // the loader was not idle 5 s into a frame boundary
  kFbAudioLate,      // the frame's audio callbacks were not done after 250 ms
  kFbSpinCap,        // a frame's CRI spin steps reached the cap
  kFbCriReady,       // the CRI server thread was not in its loop 2 s after it was created
  kFbAudioStart,     // the first audio callbacks were not done after 250 ms
  kPrCriPasses,      // CRI server passes run on the main thread
  kPrSpinSteps,      // of those, spin steps
  kPrLoaderJobs,     // loader jobs run
  kPrCriJobs,        // CRI job-thread jobs run inline
  kPrReseeds,        // MT seedings
  kPrNotify,         // system notifications the guest dequeued (Task 9)
  kCounterCount
};
void Count(Counter c, uint32_t n = 1);

// np_det_middleware.cpp
void MiddlewareStart();
void MiddlewareFrame(uint32_t frame, PPCContext& ctx, uint8_t* base);
// Called by Stop() once Stopped() is true: wakes a loader job waiting at the frame-boundary gate so it
// sees Stopped() at once.
void MiddlewareStop();
// The tick-count hook's spin detector (main thread, after the frame's first
// read from a call site): one middleware step.
void SpinRead(PPCContext& ctx, uint8_t* base, uint32_t lr);

// np_det_workers.cpp
void WorkersFrame();
void WorkersStop();
// A thread the game created through its CreateThread path (np_det.cpp, hook on sub_82793B68), in
// creation order; start = its guest entry point, tid = XThread::thread_id().
void ThreadCreated(uint32_t start, uint32_t tid);
void WorkerCreated(uint32_t start, uint32_t tid);  // np_det_workers.cpp

// np_det_trace.cpp (DOA4_NP_TRACE)
enum ReadKind {
  kRdQpc, kRdTick, kRdFileTime, kRdSdkTb, kRdSdkFt,
  kRdCall0, kRdCall1, kRdMt, kRdWorker, kRdCall2, kRdCall3, kRdKinds
};
void TraceStart(const std::filesystem::path& dir);
bool Tracing();
void TraceRead(int kind, uint32_t lr);
void TraceFrame(uint32_t frame);  // writes the previous frame's line, logs the thread census

}  // namespace doa4::np::det
