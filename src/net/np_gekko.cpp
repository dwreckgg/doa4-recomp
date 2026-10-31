#include "np_gekko.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include "gekkonet.h"

namespace doa4::np::gekko {

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
  if (!c || !c->send || length <= 0) return;
  c->send(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data), size_t(length)));
}

// GekkoNet frees addr.data, data and the result itself with free_data (three calls per result), so all three come
// from malloc here.
GekkoNetResult** AdapterReceive(int* length) {
  SessionImpl* c = g_current;
  *length = 0;
  if (!c) return nullptr;
  c->results.clear();
  if (c->receive) {
    std::vector<uint8_t> buf;
    while (c->receive(&buf)) {
      if (buf.empty()) continue;  // nothing to parse in an empty payload
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
