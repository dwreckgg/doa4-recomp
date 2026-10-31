// src/net/np_transport.h - one UDP socket and the thread that owns it (spec 3.1, 5). Winsock; no SDK.
//
// Threads and ownership: the socket, the STUN state, the simulator and every on_packet / on_tick call belong to the
// I/O thread that Start creates. PushGekko is called only by the game thread (the one producer of the outbound ring)
// and drained only by the I/O thread; PopGekko the reverse (the I/O thread produces, the game thread consumes). The
// game-thread side makes no system call except SetEvent (PushGekko wakes the I/O thread so the packet leaves at once).
// SetPeer / ClearPeer / stats() may be called from any thread: they work under a mutex and make no socket call.
//
// Receive path (I/O thread, every datagram): an answer to this socket's current STUN request (rules::StunMapped) from
// one of the STUN servers is consumed; anything else goes through wire::Decode, and a refusal is counted in
// stats().dropped[reason] (a datagram longer than the receive buffer counts as kTooLarge). A kGekko packet is then
// accepted only from the peer endpoint (else kSource) AND with the peer's token (else kToken); its body goes to the
// inbound ring (full: queue_full) and gekko_arrived() is set. Every other packet type goes to on_packet, whatever its
// source or token: the lobby judges those. So garbage never reaches PopGekko.
//
// The kGekko rings hold kGekkoQueue payloads each (controller ruling, Task 7: a power of two, several seconds of
// GekkoNet traffic). The game thread's Send/Receive lambdas (np_gekko) hold a reference to the Transport: it must
// outlive the gekko::Session that uses it.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "np_rules.h"
#include "np_wire.h"

namespace doa4::np::net {

enum class Path : uint8_t { kDirect, kRelay };  // spec 8: relay arrives in phase 2

struct Config {
  uint16_t port = rules::kPortBase;  // preferred; 0 = any free port
  bool exact_port = false;           // a session process: this port or fail
  bool loopback = false;             // bind 127.0.0.1 (DOA4_NP_BIND=loopback; host tests always)
  bool stun = true;
  std::vector<std::string> stun_servers = {"stun.l.google.com:19302", "stun.cloudflare.com:3478",
                                           "stun1.l.google.com:19302"};
  bool lan_broadcast = true;         // off with loopback
  std::string sim;                   // DOA4_NP_SIM "delay_ms,jitter_ms,loss_pct" (test switch); "" = off
  uint32_t sim_seed = 1;
  // DOA4_NP_SIM_DROP "k1,k2,..." (test switch, sim::ParseDrop; the app reads it only with DOA4_NP_TEST_DIR, like the
  // other switches): the 0-based indices, among the kGekko datagrams this transport sends, of those to drop. Applied when
  // the I/O thread drains the outbound ring, before the random simulator (a dropped one takes no draw); each drop is
  // counted (sim_drop_hits) and logged with the GekkoNet packet type (the payload's first byte). Empty = none.
  std::vector<uint32_t> sim_drop;
  uint32_t stun_refresh_ms = 15000;  // R20: binding refresh once answered; host tests pass 300
  // Wi-Fi media streaming mode while the transport runs (no background scans: on Wi-Fi they hold outgoing frames for
  // ~2 s about once a minute). Ported from DOA2U recomp by Parker Allan (parkerallan/doa2u-recomp, MIT): nic_net.c
  // wlan_no_background_scans. wlanapi.dll is loaded at run time; the I/O thread sets the mode on every connected
  // interface as it starts, logs one line with the number set, and closes the WLAN handle as it ends (Stop), which
  // releases the mode. Off by default: the session process turns it on (Task 9/12); offline nothing calls WLAN.
  bool wifi_streaming = false;
  // Map the port on the router (np_upnp) when hosting and in a session process. Read by the lobby (np_online) only; the
  // app sets it from netplay_upnp and DOA4_NP_UPNP and leaves it off with loopback. Off by default: host tests call
  // np_upnp only through stand-ins.
  bool upnp = false;
};

struct Stats {
  uint32_t dropped[size_t(wire::Drop::kCount)] = {};
  uint32_t queue_full = 0;        // kGekko payloads lost to a full ring: PushGekko refused, or an arriving one dropped
  uint32_t wrong_thread = 0;      // socket calls attempted off the I/O thread (each also logs a WARN line)
  uint32_t sim_dropped = 0, sim_delayed = 0;
  uint32_t sim_drop_hits = 0;     // kGekko datagrams dropped by Config::sim_drop
  rules::Nat nat = rules::Nat::kNoAnswer;
  // `nat` and `mapped` come only from answers newer than 2 x stun_refresh_ms (Task 9 carry 2e): a server that stopped
  // answering drops out of the class; with no fresh answer at all, kNoAnswer and no mapped address.
  std::optional<rules::Endpoint> mapped;  // this socket as the first STUN server (with a fresh answer) sees it
  uint32_t stun_answers = 0;
  int32_t wifi_interfaces = -1;   // Wi-Fi interfaces put in media streaming mode; -1 = not asked (flag off or not yet)
  // kGekko datagrams that arrived while no peer was set (Task 9 carry 2h): normal at session start, when the other game
  // is a few ms ahead, so they are counted here and NOT in dropped[] (kSource/kToken); never queued for PopGekko.
  uint32_t gekko_early = 0;
};

// The network simulator (DOA4_NP_SIM, a test switch). It sits on the I/O thread's send path: every datagram the
// transport sends to a peer (SendTo, Broadcast, kGekko; STUN requests bypass it) is the next draw n = 0, 1, 2, ...
// of the seeded decision below, and is either dropped (sim_dropped) or held for its delay and then sent
// (sim_delayed). Delays come with jitter, so held datagrams may leave in another order than they were sent.
namespace sim {
struct Params {
  uint32_t delay_ms = 0, jitter_ms = 0, loss_pct = 0;
};
// "delay_ms,jitter_ms,loss_pct": three unsigned decimal integers, delay and jitter at most 10,000, loss at most 100.
// False (and *out left as it was) for anything else.
bool Parse(std::string_view text, Params* out);
struct Decision {
  bool drop = false;
  uint32_t delay_ms = 0;  // delay_ms + a jitter drawn uniformly from [-jitter_ms, +jitter_ms], at least 0
};
// Pure: the fate of draw n under (params, seed). Loss is drawn as (hash % 100) < loss_pct.
Decision Decide(const Params& params, uint32_t seed, uint64_t n);
// DOA4_NP_SIM_DROP: "k1,k2,..." - unsigned decimal indices (at most 9 digits each, at most 64 of them), sorted and
// without repeats in *out. "" is valid (no drops). False (and *out left as it was) for anything else.
bool ParseDrop(std::string_view text, std::vector<uint32_t>* out);
}  // namespace sim

class Transport {
 public:
  using Log = std::function<void(const std::string&)>;
  static constexpr size_t kGekkoQueue = 1024;  // each kGekko ring (Spsc<std::vector<uint8_t>, 1024>)

  Transport();
  ~Transport();  // Stop()
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  // Binds, then starts the I/O thread. The bind tries config.port, then (unless exact_port) the next ports up to
  // kPortBase + kPorts - 1, then any free port; exact_port refuses anything but config.port, and the error names it.
  // A Transport starts once: a second Start (running or stopped) fails. on_packet / on_tick are set before Start.
  // The log function is called on the I/O thread, and on any thread that breaks the thread rule (its WARN line), one
  // call at a time (under a mutex: it must not call back into the transport). It is never called once Stop has
  // returned. Whatever log, on_packet and on_tick reach must outlive the I/O thread, i.e. Stop() or ~Transport: the
  // first line is logged by the thread as it starts, after Start has returned.
  bool Start(const Config& config, Log log, std::string* error);
  void Stop();                       // closes the socket and joins the thread; returns within 200 ms; drops the log
  uint16_t port() const;             // the bound port (0 before Start)
  Path path() const { return Path::kDirect; }

  // ---- I/O thread side (the lobby, np_online, runs in these callbacks) ----
  std::function<void(const rules::Endpoint& from, const wire::Packet& packet)> on_packet;  // DOA4 packets except kGekko
  std::function<void(uint32_t now_ms)> on_tick;  // every 5 ms (a high-resolution timer); now_ms = ms since Start
  // A datagram with the DOA4 magic but another protocol version (counted kProto as always): its source, its proto and
  // type bytes and its size, so the lobby can answer a game of another version with advice (Task 9 carry 2a). Only the
  // header's first 6 bytes are trusted to mean anything across versions (magic, proto, type).
  std::function<void(const rules::Endpoint& from, uint8_t proto, uint8_t type, size_t bytes)> on_foreign_proto;
  void SendTo(const rules::Endpoint& to, std::vector<uint8_t> datagram);  // I/O thread only; an empty datagram is not sent
  void Broadcast(uint16_t port, std::vector<uint8_t> datagram);           // I/O thread only; no-op with loopback
  // The peer and token kGekko traffic is accepted from and sent with. Any thread (the lobby calls them on the I/O
  // thread, host tests on their main thread): they store under a mutex and make no socket call.
  void SetPeer(const rules::Endpoint& peer, uint32_t token);
  void ClearPeer();

  // ---- game thread side (lock-free, no syscall except SetEvent/WaitForSingleObject) ----
  // Wrapped as kGekko with the peer's token by the I/O thread. False when the outbound ring is full (queue_full).
  // A payload the I/O thread drains while no peer is set, or one too long for a datagram, is discarded.
  bool PushGekko(std::vector<uint8_t> payload);
  bool PopGekko(std::vector<uint8_t>* payload);
  void* gekko_arrived() const;                   // HANDLE of an auto-reset event, set when PopGekko has data

  Stats stats() const;  // any thread (atomics / a snapshot under a mutex)

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace doa4::np::net
