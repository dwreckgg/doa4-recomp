#include "np_gekko.h"

#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "gekkonet.h"

namespace doa4::np::gekko {

// The SyncResponse guard (Task 7 review, question 5, ruling (b')). Once connected, GekkoNet answers every SyncResponse
// it receives with another one (backend.cpp:955-958 count it, 984-987 send it), and the peer, connected too, answers
// that: a duplicated or replayed SyncResponse becomes a ping-pong that only a lost datagram ends, and each further one
// adds another (measured: +1 datagram per frame per side each; a replay every 10 frames grew 5 -> 40 per frame).
// After this side has seen kStarted, at most one incoming SyncResponse per kSyncResponseGap is passed to GekkoNet; the
// rest are dropped and counted. Before kStarted nothing is touched, so the handshake is exactly GekkoNet's.
// The one byte of GekkoNet's wire layout this relies on: a payload starts with the packet header, serialised before
// the body (backend.cpp:781-782, out(pkt->pkt.header)), whose first member is `PacketType type` (u8), and SyncResponse
// is 5 in that enum (include/private/net.h:27-33: Inputs = 1, SpectatorInputs, InputAck, SyncRequest, SyncResponse).
// tests/host/np_exec_host_test.cpp's drip group goes red if a vendor bump moves it.
constexpr uint8_t kGekkoSyncResponse = 5;
constexpr auto kSyncResponseGap = std::chrono::milliseconds(200);  // = NetStats::SYNC_MSG_DELAY (net.h:125)
// The remote-input peek (Task 11, carry 2b; GekkoNet's API has no accessor for the inputs it holds). An Inputs payload
// (type 1, net.h:27-28) is the 3-byte header (type u8, magic u16 little-endian, net.h:42-45), then the body variant:
// one index byte (0 = InputMsg, the first alternative of MsgBody, net.h:99-109) and InputMsg's members in order,
// start_frame (i32 LE) then input_count (u16 LE) (net.h:47-53), packed by zpp::bits (backend.cpp:766-782). So bytes
// 4..7 are the first frame and 8..9 the count. tests/host/np_live_host_test.cpp checks the parse against the frames
// GekkoNet advances (a frame is never advanced before the peek has seen its input).
constexpr uint8_t kGekkoInputs = 1;
constexpr size_t kInputsMinBytes = 10;

struct SessionImpl {
  GekkoSession* s = nullptr;
  GekkoNetAdapter adapter{};
  Session::Send send;
  Session::Receive receive;
  int local_handle = -1;
  int remote_handle = -1;
  uint32_t added = 0;
  uint8_t peer_addr = 1;  // the one peer's address as GekkoNet sees it; the transport never reads it
  std::vector<GekkoNetResult*> results;
  std::vector<SessionEvent> events;
  bool started = false;                                // this side has seen GekkoSessionStarted
  bool sync_response_passed = false;                   // a SyncResponse was passed after kStarted ...
  std::chrono::steady_clock::time_point sync_response_at{};  // ... at this time
  uint32_t sync_responses_dropped = 0;
  int32_t last_remote_input = -1;  // the remote-input peek (top of this file)
};

namespace {
// GekkoNet's adapter functions carry no user pointer: the session that is inside a call is found here (game thread
// only; every call into GekkoNet goes through a Scope).
thread_local SessionImpl* g_current = nullptr;

struct Scope {
  SessionImpl* prev;
  explicit Scope(SessionImpl* now) : prev(g_current) { g_current = now; }
  ~Scope() { g_current = prev; }
};

void AdapterSend(GekkoNetAddress*, const char* data, int length) {
  SessionImpl* c = g_current;
  if (!c || !c->send || length <= 0) return;  // GekkoNet never sends an empty packet; the guards are defensive, untested
  c->send(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data), size_t(length)));
}

// GekkoNet frees addr.data, data and the result itself with free_data (three calls per result), so all three come
// from malloc here. GekkoNet asks once per update (game_session.cpp:177, 666), so the read cap is per Update: a flood
// cannot hold the game thread, and what is left stays in the transport's ring for the next Update.
GekkoNetResult** AdapterReceive(int* length) {
  SessionImpl* c = g_current;
  *length = 0;
  if (!c) return nullptr;
  c->results.clear();
  if (c->receive) {
    std::vector<uint8_t> buf;
    // The cap is tested BEFORE Receive is called: a 257th call would take a payload out of the ring and lose it.
    for (int reads = 0; reads < kMaxReceivePerUpdate && c->receive(&buf); ++reads) {
      // GekkoNet refuses an empty buffer by itself (backend.cpp:155-158: the deserialise fails and a line is printed;
      // the buffers are freed at :165-167), so this skip changes nothing it could observe; it only avoids building a
      // result for nothing. Not tested for that reason.
      if (buf.empty()) continue;
      if (c->started && buf[0] == kGekkoSyncResponse) {  // the SyncResponse guard (top of this file)
        const auto now = std::chrono::steady_clock::now();
        if (c->sync_response_passed && now - c->sync_response_at < kSyncResponseGap) {
          ++c->sync_responses_dropped;
          buf.clear();
          continue;
        }
        c->sync_response_passed = true;
        c->sync_response_at = now;
      }
      if (buf[0] == kGekkoInputs && buf.size() >= kInputsMinBytes && buf[3] == 0) {  // the remote-input peek
        const uint32_t start = uint32_t(buf[4]) | uint32_t(buf[5]) << 8 | uint32_t(buf[6]) << 16 | uint32_t(buf[7]) << 24;
        const uint32_t count = uint32_t(buf[8]) | uint32_t(buf[9]) << 8;
        if (count > 0) {
          const int64_t last = int64_t(int32_t(start)) + int64_t(count) - 1;
          if (last > c->last_remote_input && last <= INT32_MAX) c->last_remote_input = int32_t(last);
        }
      }
      auto* r = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
      r->addr.size = 1;
      r->addr.data = std::malloc(1);
      *static_cast<uint8_t*>(r->addr.data) = c->peer_addr;
      r->data_len = unsigned(buf.size());
      r->data = std::malloc(buf.size());
      std::memcpy(r->data, buf.data(), buf.size());
      c->results.push_back(r);
      buf.clear();
    }
  }
  *length = int(c->results.size());
  return c->results.data();
}

void AdapterFree(void* p) { std::free(p); }
}  // namespace

Session::Session() : impl_(new SessionImpl) {}
Session::~Session() {
  Destroy();
  delete impl_;
}

bool Session::Create(const Config& config, Send send, Receive receive) {
  Destroy();
  SessionImpl& im = *impl_;
  im = SessionImpl();
  im.send = std::move(send);
  im.receive = std::move(receive);
  im.adapter = GekkoNetAdapter{AdapterSend, AdapterReceive, AdapterFree};
  Scope scope(&im);
  if (!gekko_create(&im.s, GekkoGameSession)) return false;
  GekkoConfig cfg{};
  cfg.num_players = uint8_t(config.num_players);
  cfg.max_spectators = 0;
  cfg.input_prediction_window = 0;  // lockstep (R4)
  cfg.spectator_delay = 0;
  cfg.input_size = exec::kPlayerInputBytes;
  cfg.state_size = sizeof(exec::SaveToken);
  cfg.limited_saving = false;
  cfg.desync_detection = true;
  cfg.check_distance = 0;
  gekko_start(im.s, &cfg);
  gekko_net_adapter_set(im.s, &im.adapter);
  GekkoNetAddress addr{&im.peer_addr, 1};
  for (int slot = 0; slot < config.num_players; ++slot) {
    const bool local = slot == config.local_slot;
    const int h = gekko_add_actor(im.s, local ? GekkoLocalPlayer : GekkoRemotePlayer, local ? nullptr : &addr);
    if (h != slot) {
      gekko_destroy(&im.s);
      return false;
    }
    if (local) im.local_handle = h;
    else if (im.remote_handle < 0) im.remote_handle = h;
  }
  if (im.local_handle < 0) {
    gekko_destroy(&im.s);
    return false;
  }
  gekko_set_local_delay(im.s, im.local_handle, config.delay);
  gekko_set_disconnect_timeout(im.s, 0);  // R4: no automatic disconnect
  return true;
}

void Session::AddLocalInput(const Pad& pad) {
  SessionImpl& im = *impl_;
  if (!im.s) return;
  uint8_t bytes[exec::kPlayerInputBytes];
  exec::EncodePlayerInput(pad, bytes);
  Scope scope(&im);
  gekko_add_local_input(im.s, im.local_handle, bytes);
  ++im.added;
}

uint32_t Session::local_inputs_added() const { return impl_->added; }

uint32_t Session::sync_responses_dropped() const { return impl_->sync_responses_dropped; }

int32_t Session::last_remote_input_frame() const { return impl_->last_remote_input; }

std::vector<exec::Event> Session::Update() {
  SessionImpl& im = *impl_;
  std::vector<exec::Event> out;
  if (!im.s) return out;
  Scope scope(&im);
  int count = 0;
  GekkoGameEvent** ev = gekko_update_session(im.s, &count);
  for (int i = 0; i < count; ++i) {
    const GekkoGameEvent* e = ev[i];
    exec::Event x;
    switch (e->type) {
      case GekkoAdvanceEvent:
        x.kind = exec::Event::kAdvance;
        x.frame = e->data.adv.frame;
        x.inputs.assign(e->data.adv.inputs, e->data.adv.inputs + e->data.adv.input_len);
        break;
      case GekkoSaveEvent:
        x.kind = exec::Event::kSave;
        x.frame = e->data.save.frame;
        x.checksum = e->data.save.checksum;
        x.state_len = e->data.save.state_len;
        x.state = e->data.save.state;
        break;
      case GekkoLoadEvent:
        x.kind = exec::Event::kLoad;
        x.frame = e->data.load.frame;
        break;
      default:
        continue;
    }
    out.push_back(std::move(x));
  }
  int scount = 0;
  GekkoSessionEvent** se = gekko_session_events(im.s, &scount);
  for (int i = 0; i < scount; ++i) {
    switch (se[i]->type) {
      case GekkoSessionStarted:
        im.events.push_back({SessionEvent::kStarted, 0, 0, 0});
        im.started = true;  // arms the SyncResponse guard from the next Update's receive on
        break;
      case GekkoDesyncDetected:
        im.events.push_back({SessionEvent::kDesync, se[i]->data.desynced.frame, se[i]->data.desynced.local_checksum,
                             se[i]->data.desynced.remote_checksum});
        break;
      case GekkoPlayerDisconnected:
        im.events.push_back({SessionEvent::kDisconnected, 0, 0, 0});
        break;
      default:
        break;
    }
  }
  return out;
}

std::vector<SessionEvent> Session::TakeSessionEvents() { return std::exchange(impl_->events, {}); }

void Session::Poll() {
  SessionImpl& im = *impl_;
  if (!im.s) return;
  Scope scope(&im);
  gekko_network_poll(im.s);
}

float Session::FramesAhead() {
  SessionImpl& im = *impl_;
  if (!im.s) return 0;
  Scope scope(&im);
  return gekko_frames_ahead(im.s);
}

NetStats Session::Stats() {
  SessionImpl& im = *impl_;
  NetStats out;
  if (!im.s || im.remote_handle < 0) return out;
  Scope scope(&im);
  GekkoNetworkStats st{};
  gekko_network_stats(im.s, im.remote_handle, &st);
  out.ping_ms = st.avg_ping;
  out.jitter_ms = st.jitter;
  out.kb_sent = st.kb_sent;
  out.kb_received = st.kb_received;
  return out;
}

void Session::Destroy() {
  SessionImpl& im = *impl_;
  if (!im.s) return;
  Scope scope(&im);
  gekko_destroy(&im.s);
  im.s = nullptr;
}

}  // namespace doa4::np::gekko
