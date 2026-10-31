// DOA4 netplay phase 1: one online session on the game thread (spec 3.2) - lockstep through GekkoNet, holds with a
// banner, the lost / frame-0 / desync / leave ends and the pacer. No SDK, no Windows, no socket: time and waiting are
// injected (Clock), GekkoNet comes through np_gekko and the event list through np_exec.
// Host-tested by tests/host/np_live_host_test.cpp (two sessions over the real GekkoNet, on a fake clock).
//
// Ownership and teardown (Task 6 review carry): the owner (Task 12) creates the gekko::Session (and checks Create's
// result: the Session constructor requires a CREATED session), one exec::Executor per session, and the live::Session
// over both. It ends the session with Close(), which calls Executor::DropPending() and then gekko::Session::Destroy()
// in that order (a pending Save points into GekkoNet's storage; a Run after Destroy would write freed memory). The
// destructor calls Close(). The live::Session must be destroyed before the gekko::Session and the Executor objects.
//
// Threads (S2a): Frame runs on the game thread only; the thread of the first Frame call is captured and a Frame call
// from any other thread returns End::kInternal at once without touching GekkoNet or the session's state (the debug
// check of the single-producer contract, Task 7 review; the game thread's session goes on). The gekko Send / Receive callbacks (the transport's PushGekko / PopGekko) are therefore called only from
// the game thread, inside Frame (np_live never touches the transport; the transport must outlive the gekko::Session).
// Leave (UI thread or the frame hook) and PeerLeft (the transport's I/O thread) store into atomics Frame reads;
// banner_waiting, frames_ahead and remote_slack are atomics Frame writes and any thread reads.
//
// Clock (#0435): Task 12's real wait_packet and sleep_precise must use a high-resolution waitable timer
// (CreateWaitableTimerExW with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION): a plain 1 ms wait ticks at 15.6 ms on this PC
// (measured in Task 7), which would turn every 1..16 ms slice below into 15.6 ms and the pacer's microseconds into a
// whole tick.
#pragma once
#include <atomic>
#include <climits>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "np_core.h"
#include "np_exec.h"
#include "np_gekko.h"
#include "np_wire.h"

namespace doa4::np::live {

struct Clock {
  std::function<uint64_t()> now_us;
  std::function<void(uint32_t max_ms)> wait_packet;  // returns when a packet arrived or after max_ms (1..16)
  std::function<void(uint32_t us)> sleep_precise;     // the pacer's wait (a high-resolution waitable timer)
};

enum class End : uint8_t { kNone, kLeft, kPeerLeft, kLost, kDesync, kFrame0Timeout, kInternal };

struct Timing { uint64_t net_us = 0, wait_us = 0, pacer_us = 0; };  // this frame's buckets

struct FrameResult {
  bool ok = false;      // input staged for the frame
  FrameInput input;
  End end = End::kNone;
  // The frame of the end: the Leave / PeerLeft frame for kLeft / kPeerLeft (the same number on both sides), else the
  // frame of the Frame call that returned it. A desync names GekkoNet's frame in `why`.
  int32_t end_frame = 0;
  // kDesync: the frame the desync was found at (GekkoNet's own detection, or the frame the other side reported in its
  // Leave with reason desync, Task 15 G1); -1 otherwise.
  int32_t desync_frame = -1;
  // kPeerLeft: the other side's Leave reason (wire::LeaveReason as a number; end_frame -1 = a Bye); -1 otherwise.
  int peer_reason = -1;
  std::string why;      // e.g. "desync at frame 3000 (this side 1A2B3C4D, other side 5E6F7081)"
  Timing timing;
  uint32_t holds = 0;   // holds reported since the session began (this frame's included)
  uint64_t hold_us = 0; // this frame's reported hold, begin to end (0 if none)
};

// A hold = the waiting inside one Frame call, from its first wait_packet; it is REPORTED (events, holds) once it
// reaches hold_report_ms, with the begin time it really had. Deadlines are whole milliseconds since the hold
// began (frame 0: since the first Frame(0) call): elapsed_ms = (now_us() - begin_us) / 1000.
struct Limits { uint32_t banner_ms = 1000, end_ms = 10000, frame0_ms = 60000, hold_report_ms = 100; };  // spec 3.2.5, R4

enum class Event : uint8_t { kHoldBegin, kBannerWaiting, kHoldEnd, kEnd };

struct Hooks {
  // Game thread, called inside the hold (Frame is still blocked) with the Clock's time of the event: kHoldBegin
  // (at = the hold's real begin, delivered when it reaches hold_report_ms), kBannerWaiting (at = the time the hold
  // was seen to reach banner_ms, i.e. begin + banner_ms to within one clock reading), kHoldEnd (at = the Advance),
  // kEnd (at = the end). Task 12 writes events.txt from it (ruling F8).
  std::function<void(Event event, int32_t frame, uint64_t at_us)> on_event;
  // Empty: gekko->FramesAhead(). Tests inject a value (ruling F5): GekkoNet's figure is already averaged over 26
  // frames and halved (backend.cpp:1545-1559), and is used as given.
  std::function<float()> frames_ahead;
  // Empty: gekko->Stats().ping_ms (GekkoNet's averaged round trip to the peer, ms). Tests inject a value.
  std::function<float()> ping_ms;
};

class Session {
 public:
  // `gekko` must be a created session (gekko::Session::Create returned true); `executor` is this session's own.
  Session(gekko::Session* gekko, exec::Executor* executor, Clock clock, Limits limits, Hooks hooks);
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  ~Session();  // Close()

  // The frame hook, main thread, AFTER det::OnFrame(f) (R1). `checksum_entering` = FoldChecksum of the state
  // entering f; `local` = this side's pad for frame f + delay. Blocks until frame f's inputs are confirmed or the
  // session ends. Every call that does not end at once runs at least one Update (session events are taken once per
  // Update). Once an end is returned, every later call returns the same end without touching GekkoNet.
  FrameResult Frame(int32_t f, uint32_t checksum_entering, const Pad& local);
  // This side leaves at that frame (UI or the script's quit); any thread. Repeated calls keep the EARLIEST frame. The
  // call that reaches it (or the next call, if it is already past) returns kLeft with end_frame = that frame.
  void Leave(int32_t at_frame);
  // From the lobby channel (I/O thread). Frame and reason are stored as one atomic word, so they cannot tear.
  void PeerLeft(int32_t at_frame, wire::LeaveReason why);
  bool banner_waiting() const;          // a hold has lasted >= banner_ms (any thread)
  float frames_ahead() const;
  float ping_ms() const;                // the round trip to the peer, refreshed after every advanced frame (any thread)
  // Frames of the peer's input this side holds beyond the frame it is about to advance (carry 2b), >= 0; updated
  // after every Update. Read from np_gekko's peek at the Inputs payloads (gekko::Session::last_remote_input_frame),
  // which is UNAUTHENTICATED (a well-formed payload with any magic moves it): display and diagnostics only - Task 12
  // must not base a decision on it.
  int32_t remote_slack() const;
  // Executor::DropPending() then gekko::Session::Destroy(); idempotent. GAME THREAD ONLY (the thread that calls Frame,
  // or any thread once Frame is no longer called). Later Frame calls return End::kInternal ("session closed") unless
  // an end was already returned (then that end).
  void Close();

 private:
  FrameResult Finish(FrameResult r, End end, int32_t end_frame, std::string why, uint64_t at_us);

  gekko::Session* gekko_;
  exec::Executor* executor_;
  Clock clock_;
  Limits limits_;
  Hooks hooks_;

  std::atomic<int32_t> leave_at_{INT32_MAX};
  // PeerLeft: (uint64(reason) << 32) | uint32(frame); kNoPeerLeft until a PeerLeft call.
  static constexpr uint64_t kNoPeerLeft = uint64_t(uint32_t(INT32_MAX));
  std::atomic<uint64_t> peer_left_{kNoPeerLeft};
  std::atomic<bool> banner_{false};
  std::atomic<float> frames_ahead_{0.f};
  std::atomic<float> ping_ms_{0.f};
  std::atomic<int32_t> remote_slack_{0};

  // Game thread only (Frame and Close). A Frame call from another thread reads only thread_ and returns.
  bool closed_ = false;
  bool ended_ = false;
  FrameResult end_result_;
  std::atomic<std::thread::id> thread_{};  // the thread of the first Frame call (default id: none yet)
  bool frame0_started_ = false;
  uint64_t frame0_begin_us_ = 0;
  uint32_t holds_ = 0;
};

}  // namespace doa4::np::live
