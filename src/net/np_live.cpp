#include "np_live.h"

#include <algorithm>
#include <cstdio>
#include <span>
#include <utility>
#include <vector>

#include "np_rules.h"

namespace doa4::np::live {

namespace {
const char* ReasonText(wire::LeaveReason why) {
  switch (why) {
    case wire::LeaveReason::kLeft: return "left";
    case wire::LeaveReason::kDesync: return "desync";
    case wire::LeaveReason::kTimeout: return "timeout";
    case wire::LeaveReason::kInternal: return "internal error";
  }
  return "unknown";
}
}  // namespace

Session::Session(gekko::Session* gekko, exec::Executor* executor, Clock clock, Limits limits, Hooks hooks)
    : gekko_(gekko), executor_(executor), clock_(std::move(clock)), limits_(limits), hooks_(std::move(hooks)) {}

Session::~Session() { Close(); }

void Session::Close() {
  if (closed_) return;
  closed_ = true;
  // Order matters: a pending Save points into the gekko session's storage (np_exec.h, DropPending).
  if (executor_) executor_->DropPending();
  if (gekko_) gekko_->Destroy();
}

void Session::Leave(int32_t at_frame) {
  int32_t cur = leave_at_.load();
  while (at_frame < cur && !leave_at_.compare_exchange_weak(cur, at_frame)) {
  }
}

void Session::PeerLeft(int32_t at_frame, wire::LeaveReason why) {
  peer_left_.store(uint64_t(uint8_t(why)) << 32 | uint32_t(at_frame));
}

bool Session::banner_waiting() const { return banner_.load(); }
float Session::frames_ahead() const { return frames_ahead_.load(); }
float Session::ping_ms() const { return ping_ms_.load(); }
int32_t Session::remote_slack() const { return remote_slack_.load(); }

FrameResult Session::Finish(FrameResult r, End end, int32_t end_frame, std::string why, uint64_t at_us) {
  r.ok = false;
  r.end = end;
  r.end_frame = end_frame;
  r.why = std::move(why);
  r.holds = holds_;
  banner_.store(false);
  ended_ = true;
  end_result_ = r;
  end_result_.timing = Timing();  // a later call returns the end, not this call's time
  end_result_.hold_us = 0;
  if (hooks_.on_event) hooks_.on_event(Event::kEnd, r.end_frame, at_us);
  return r;
}

FrameResult Session::Frame(int32_t f, uint32_t checksum_entering, const Pad& local) {
  // The single-producer check first: a call from another thread reads nothing else and writes nothing (no end is
  // latched, no event raised), so it cannot race the game thread; it only reports itself.
  const std::thread::id me = std::this_thread::get_id();
  std::thread::id owner{};
  if (!thread_.compare_exchange_strong(owner, me) && owner != me) {
    FrameResult wrong;
    wrong.end = End::kInternal;
    wrong.end_frame = f;
    wrong.why = "Frame called from a second thread";
    return wrong;
  }
  if (ended_) return end_result_;  // the same end; GekkoNet and the executor are not touched again
  FrameResult r;
  uint64_t t = clock_.now_us();
  if (closed_) return Finish(r, End::kInternal, f, "session closed", t);
  if (f == 0 && !frame0_started_) {
    frame0_started_ = true;
    frame0_begin_us_ = t;
  }

  // Leave / PeerLeft (other threads): checked before GekkoNet is touched, and again between the Updates of a hold.
  auto left = [&](uint64_t at, FrameResult* out) {
    const int32_t leave = leave_at_.load();
    if (f >= leave) {
      char why[64];
      std::snprintf(why, sizeof why, "left at frame %d", int(leave));
      *out = Finish(r, End::kLeft, leave, why, at);
      return true;
    }
    const uint64_t packed = peer_left_.load();
    const int32_t peer = int32_t(uint32_t(packed));
    if (packed != kNoPeerLeft && wire::LeaveReason(uint8_t(packed >> 32)) == wire::LeaveReason::kDesync) {
      // Task 15 G1: the other side found a desync and left: this side ends as a desync too, at once (its own GekkoNet
      // may not have seen the mismatch yet), naming the frame the other side reported, so both sides keep a dump.
      char why[96];
      std::snprintf(why, sizeof why, "desync at frame %d (reported by the other player)", int(peer));
      r.desync_frame = peer;
      *out = Finish(r, End::kDesync, f, why, at);
      return true;
    }
    if (f >= peer) {
      char why[96];
      std::snprintf(why, sizeof why, "the other player left at frame %d (%s)", int(peer),
                    ReasonText(wire::LeaveReason(uint8_t(packed >> 32))));
      r.peer_reason = int(uint8_t(packed >> 32));  // review I2: the banner text follows the reason
      *out = Finish(r, End::kPeerLeft, peer, why, at);
      return true;
    }
    return false;
  };
  FrameResult out;
  if (left(t, &out)) return out;

  // Fill the pending Save of f-1 before GekkoNet's update for f reads it (ruling F10b), then the one add (ruling F4).
  exec::Step step = executor_->Run({}, checksum_entering);
  if (step.end != exec::End::kNone) return Finish(r, End::kInternal, f, step.why, clock_.now_us());
  gekko_->AddLocalInput(local);

  bool in_hold = false, reported = false, bannered = false;
  uint64_t begin = 0, t1 = 0;
  for (;;) {
    std::vector<exec::Event> events = gekko_->Update();
    std::vector<gekko::SessionEvent> session_events = gekko_->TakeSessionEvents();
    step = executor_->Run(events, checksum_entering);
    const int32_t slack = gekko_->last_remote_input_frame() - executor_->next_frame();
    remote_slack_.store(slack > 0 ? slack : 0);
    t1 = clock_.now_us();
    r.timing.net_us += t1 - t;
    for (const gekko::SessionEvent& e : session_events) {
      if (e.kind != gekko::SessionEvent::kDesync) continue;
      char why[96];
      std::snprintf(why, sizeof why, "desync at frame %d (this side %08X, other side %08X)", int(e.frame), unsigned(e.local),
                    unsigned(e.remote));
      // This side's own checksum for that frame may have been queued in this very update (the side that is behind
      // detects first) and GekkoNet sends it only at the next one: one poll sends it without advancing, so the peer
      // ends on the desync too instead of waiting end_ms for input (np_gekko.h, Poll).
      gekko_->Poll();
      r.desync_frame = int32_t(e.frame);
      return Finish(r, End::kDesync, f, why, t1);
    }
    if (step.end != exec::End::kNone) return Finish(r, End::kInternal, f, step.why, t1);
    // A hold: deadlines in whole milliseconds since its begin (frame 0's end: since the first Frame(0) call). The
    // report and the banner are judged on EVERY Update of a hold, the advancing one included: a slice that overshoots
    // (a real timer wakes late) can pass a deadline and end on the packet's release, and that hold still happened.
    // The end deadline is judged only when the frame did not advance (an input that arrived wins).
    auto report = [&](uint64_t elapsed) {
      if (!reported && elapsed >= limits_.hold_report_ms) {
        reported = true;
        ++holds_;
        if (hooks_.on_event) hooks_.on_event(Event::kHoldBegin, f, begin);
      }
      if (!bannered && elapsed >= limits_.banner_ms) {
        bannered = true;
        banner_.store(true);
        if (hooks_.on_event) hooks_.on_event(Event::kBannerWaiting, f, t1);
      }
    };
    if (in_hold) report((t1 - begin) / 1000);
    if (step.advanced) break;
    if (left(t1, &out)) return out;

    if (!in_hold) {
      in_hold = true;
      begin = t1;
    }
    const uint64_t elapsed = (t1 - begin) / 1000;
    report(elapsed);
    uint64_t end_elapsed = elapsed, end_limit = limits_.end_ms;
    if (f == 0) {
      end_elapsed = (t1 - frame0_begin_us_) / 1000;
      end_limit = limits_.frame0_ms;
    }
    if (end_elapsed >= end_limit) {
      if (f == 0) {
        char why[96];
        std::snprintf(why, sizeof why, "the other player did not connect within %u s", unsigned(limits_.frame0_ms / 1000));
        return Finish(r, End::kFrame0Timeout, f, why, t1);
      }
      char why[96];
      std::snprintf(why, sizeof why, "no input from the other player for %u s at frame %d", unsigned(limits_.end_ms / 1000), int(f));
      return Finish(r, End::kLost, f, why, t1);
    }
    // The slice ends on the nearest deadline not yet reached, 1..16 ms.
    uint64_t slice = 16;
    if (!reported) slice = std::min<uint64_t>(slice, limits_.hold_report_ms - elapsed);
    if (!bannered) slice = std::min<uint64_t>(slice, limits_.banner_ms - elapsed);
    slice = std::min<uint64_t>(slice, end_limit - end_elapsed);
    if (slice < 1) slice = 1;
    clock_.wait_packet(uint32_t(slice));
    t = clock_.now_us();
    r.timing.wait_us += t - t1;
  }

  // Advanced.
  if (reported) {
    r.hold_us = t1 - begin;
    if (hooks_.on_event) hooks_.on_event(Event::kHoldEnd, f, t1);
  }
  banner_.store(false);
  r.ok = true;
  r.input = step.input;
  r.holds = holds_;
  const float fa = hooks_.frames_ahead ? hooks_.frames_ahead() : gekko_->FramesAhead();
  frames_ahead_.store(fa);
  ping_ms_.store(hooks_.ping_ms ? hooks_.ping_ms() : gekko_->Stats().ping_ms);
  const int extra = rules::PacerExtraUs(fa);
  if (extra > 0) {
    const uint64_t p0 = clock_.now_us();
    clock_.sleep_precise(uint32_t(extra));
    r.timing.pacer_us = clock_.now_us() - p0;
  }
  return r;
}

}  // namespace doa4::np::live
