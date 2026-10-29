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

// np_det_middleware.cpp
void MiddlewareStart();
void MiddlewareFrame(uint32_t frame, PPCContext& ctx, uint8_t* base);
// The tick-count hook's spin detector (main thread, after the frame's first
// read from a call site): one middleware step.
void SpinRead(PPCContext& ctx, uint8_t* base, uint32_t lr);

// np_det_workers.cpp
void WorkersFrame();
void WorkersStop();

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
