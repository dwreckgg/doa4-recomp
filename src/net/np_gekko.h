// DOA4 netplay phase 1: one GekkoNet game session behind the executor's event type; game thread only.
// No socket: packets go through the Send / Receive callbacks (the transport, Task 7, knows the one peer).
// Host-tested by tests/host/np_exec_host_test.cpp against the real library (third_party/GekkoNet).
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
#include "np_exec.h"

namespace doa4::np::gekko {
// Receive is called at most this many times per Update (Task 7, carry 3a); the rest stay queued for the next Update.
inline constexpr int kMaxReceivePerUpdate = 256;
struct Config { int num_players = 2; int local_slot = 0; uint8_t delay = 2; };
struct SessionEvent { enum Kind : uint8_t { kStarted, kDesync, kDisconnected } kind; int32_t frame = 0; uint32_t local = 0, remote = 0; };
struct NetStats { float ping_ms = 0, jitter_ms = 0, kb_sent = 0, kb_received = 0; };

struct SessionImpl;  // np_gekko.cpp: the GekkoNet handle, its adapter and the callbacks

class Session {
 public:
  using Send = std::function<void(std::span<const uint8_t> payload)>;  // to the one peer
  using Receive = std::function<bool(std::vector<uint8_t>* payload)>;  // from the peer; false when none queued; <= 256 calls per Update
  Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  bool Create(const Config& config, Send send, Receive receive);       // window 0, disconnect timeout 0 (R4)
  // Once per frame, before the frame's first Update. GekkoNet files it at its current frame + delay and drops a
  // second add for the same frame (input.cpp:50-65): the delay is GekkoNet's, never the caller's (ruling F4).
  void AddLocalInput(const Pad& pad);
  uint32_t local_inputs_added() const;             // calls of AddLocalInput so far (Task 11's once-per-frame check)
  // Incoming GekkoNet SyncResponses dropped by the guard (after kStarted, at most one per 200 ms is passed: np_gekko.cpp).
  uint32_t sync_responses_dropped() const;
  // The newest frame of the peer's input carried by any Inputs payload passed to GekkoNet so far (start_frame +
  // input_count - 1, read from the payload: np_gekko.cpp kGekkoInputs), -1 before the first. Task 11's remote slack.
  // An upper bound on what GekkoNet has filed: it files received inputs in order (game_session.cpp:711-735), so on a
  // link that loses a payload this runs ahead of GekkoNet until the gap is resent. Only well-formed Inputs payloads
  // are read, but UNAUTHENTICATED (read before GekkoNet checks the magic): display and diagnostics only.
  int32_t last_remote_input_frame() const;
  std::vector<exec::Event> Update();               // one gekko_update_session, converted
  std::vector<SessionEvent> TakeSessionEvents();   // since the last call
  // One gekko_network_poll: receives, and sends what GekkoNet has queued, without advancing (no game event). GekkoNet
  // queues a frame's SessionHealth inside an update and sends it at the start of the NEXT one (game_session.cpp:
  // 203 and 365-390 queue it, Poll's SendPendingOutput at :690 sends it); Task 11 calls this once when it ends a
  // session on a desync, so the peer still gets this side's checksum for the frame and ends on the desync too.
  void Poll();
  float FramesAhead();
  NetStats Stats();
  void Destroy();
  ~Session();

 private:
  SessionImpl* impl_ = nullptr;
};
}  // namespace doa4::np::gekko
