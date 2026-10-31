// src/net/np_online.h - the lobby (spec 5 Connecting, Handshake, Session start), on the transport's I/O thread.
// Ported from DOA2U (doa2u-release 6c35c7b, doa2u/src/online/netplay.c): handle_control 1093-1291, poll_network
// 1350-1432 and the session start 2100-2231 (fill_own_side, netplay_boot_send, boot_host_restart, netplay_boot_reply,
// boot_answer, netplay_boot_joiner_received). DOA2U's pacing, match exchange and resync stay out (Task 11).
//
// Threads. Every state change happens on the transport's I/O thread (in its on_packet / on_tick callbacks), under one
// mutex; the public calls only queue a command (applied at the next 5 ms tick) or read a snapshot. The providers are
// called on the I/O thread, except Providers::files, which runs once on a helper thread started by Start (it may take
// seconds); restart_into_session and on_peer_left are called on the I/O thread with the mutex released, and must not
// call Stop (Stop refuses on the I/O thread: joining it from itself would end the process, Task 7 review M-2).
//
// Message contract (S2a: every message type states its refusal reply):
//   Hello (lobby, token 0) -> Welcome(accept, token) or Welcome(refusal kind, text). The joiner composes its own text
//     for kBuild (rules::MismatchAdvice of its tag and the host's) and for its own unreadable/missing game files; it
//     shows the host's text otherwise.
//   A datagram of another protocol version (transport on_foreign_proto): a hosting lobby answers a Hello-type one with
//     Welcome(kProto) in its own version; a connecting joiner that receives any datagram of another version from its
//     target goes Offline with advice naming both versions. Neither side needs to read the other's body.
//   kMsgBoot (reliable, chunks + acks) -> BootReply(ok) or BootReply(not ok, why). The host restarts ONLY on BootReply
//     ok carrying the hand-off's token (never on the chunk acks); a refusal (its token, or 0 when the joiner could not
//     read the hand-off) leaves it Connected with the joiner's reason; no reply within 15 s leaves it Connected with
//     "The other player's game did not answer the session start." The joiner restarts 700 ms after its first ok
//     (DOA2U 2230: the last acks go out first) and repeats the reply every 100 ms until then. A yes that arrives after
//     the 15 s (packet token and the hand-off's token both right) still restarts the host, as long as it is Connected to
//     the same peer and no newer StartSession was made (fix round I-1b); a yes for an older boot token is ignored. A
//     joiner that cancels (Disconnect, Join, Host, the peer lost) before its restart fires deletes its hand-off (M-2).
//   Hello carrying a session token (a session process) -> a lobby (any state) answers Welcome(kSession), as a stranger
//     reply; a session process that gets Welcome(kSession) from its hand-off peer before the peer's Hello gives up at
//     once: Offline, Status::session_refused, event "The other player's game did not start the session." (I-1a; the
//     app polls the flag - Task 12 ends the session and restarts offline; np_live's frame-0 limit stays the backstop).
//   Ping -> Pong (peer only). Leave / Bye: no reply (one-way, sent 3 times).
// Authentication: Ping, Pong, Chunk, Ack, Leave, Bye and BootReply are taken only from the peer (else kSource) with
// the peer's token (else kToken); a kGekko is the transport's to filter. Every reply to a datagram from anyone but an
// authenticated peer (Welcome, the protocol answer) is at most one per datagram, never larger than that datagram (a
// refusal text is shortened to fit; if even an empty one does not fit, nothing is sent), and at most 10 in any 1,000 ms;
// the Welcome to the joiner being accepted (or resent to the known peer) keeps the size rule but not that budget (M-4).
// A refusal of a stranger changes the event line (and writes a log/events.txt line) at most once per refusal kind per
// kStrangerEventMs (I-3).
// LAN answers: a joiner also takes the Welcome from a private address on a 7200-7215 port (the answer to its LAN
// broadcast Hello) - DOA2U's behaviour; the lobby token travels in clear, so it authenticates the path, not the person.
// Input delay: the host decides it (auto from its ping, or its manual value) and carries it in the hand-off; the
// joiner's Hello preference is only logged/displayed.
// Effects: log lines, file writes (events.txt, status.txt, the hand-off) and the callbacks run on the I/O thread with
// the mutex released, in the order they were decided (M-1).
// UPnP: with net::Config::upnp, Host maps the port and so does StartInSession (the session process maps it again);
// Stop unmaps. The app leaves upnp off with loopback.
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "np_handoff.h"
#include "np_rules.h"
#include "np_transport.h"
#include "np_wire.h"

namespace doa4::np::online {

enum class State : uint8_t { kOffline, kHosting, kConnecting, kConnected, kStarting, kRestarting, kInSession };
const char* StateName(State s);  // "offline", "hosting", ... "in_session" (status.txt)

struct Status {
  State state = State::kOffline;
  bool is_host = false;
  std::string my_code, code_note;   // the code and what it covers (public address / LAN only / symmetric NAT)
  std::string event, hint;          // the latest notable event; what to do next
  std::string peer_release;
  float ping_ms = 0; int ping_samples = 0;
  int delay = 2; bool delay_auto = true;   // what Start would use
  rules::Nat nat = rules::Nat::kNoAnswer;
  std::string upnp;                 // upnp::Result message ("UPnP off" when net::Config::upnp is off or loopback)
  uint16_t port = 0;
  uint32_t dropped = 0;             // all Drop reasons summed (the diagnostics line, spec 7): transport + lobby
  bool can_start = false;           // host, connected, >= 4 ping samples (R14)
  // Additions (Task 9): the drops reason by reason (transport + lobby; their sum is `dropped`), and the kGekko
  // datagrams that arrived before SetPeer (normal at session start: NOT in `dropped`; carry 2h).
  std::array<uint32_t, size_t(wire::Drop::kCount)> drops{};
  uint32_t gekko_early = 0;
  uint16_t peer_port = 0;           // the peer's port as the socket saw it (status.txt; 0 without a peer)
  bool session_refused = false;     // session process: the hand-off's peer answered as a lobby (I-1a); end the session
  // The kind of the latest refusal this side received (joiner, session process) or issued (host), for the harness:
  // "none" | "build" | "files" | "busy" | "proto" | "session" | "other". Set with the refusal; "none" again with the
  // next event that is not a refusal (and so with a new Host or Join). A host's busy and kSession refusals set it
  // without changing the event line (the pair's own event stays). status.txt carries it as its last line, refusal=<kind>.
  std::string refusal = "none";
};

struct Providers {
  std::function<uint64_t()> build;  // np::BuildIdValue() (0 = unreadable: Judge refuses it); a function, so the
                                    // host test can change it after connecting
  uint32_t connect_timeout_ms = 90000;  // the game's; the host test passes 2000
  std::string release;              // DOA4_RELEASE_TAG
  std::function<std::string()> files;           // np_files Text of the game folder (may block on first use)
  std::function<std::string()> guest_settings;  // handoff::CaptureGuestSettings over rex::cvar
  std::function<std::vector<handoff::File>(std::string* error)> save_files;  // the host's save under kSavePrefix
  std::filesystem::path handoff_path;           // <user data>/netplay/handoff.d4h
  std::function<void()> restart_into_session;   // after the hand-off is written; posts to the UI thread
  std::filesystem::path events_path;            // <user data>/netplay/events.txt
  std::filesystem::path test_dir;               // DOA4_NP_TEST_DIR, or empty
  std::map<std::string, std::string> test;      // the test switches the lobby reads (only with test_dir): DOA4_NP_SEED,
                                                // DOA4_NP_DELAY, DOA4_NP_TEST_BUILD_SALT, DOA4_NP_TEST_FILES_SALT
                                                // (the app fills it from the environment; host tests fill it directly)
  std::function<void(const std::string&)> log;  // game.log ([doa4][NET] ...)
};

// Timing constants (DOA2U netplay.c 57-64 where it has them).
inline constexpr uint32_t kHelloMs = 400, kPingMs = 500, kPunchMs = 300, kPeerTimeoutMs = 20000;
inline constexpr uint32_t kRetxMs = 150, kBootAnswerMs = 15000, kJoinerRestartMs = 700, kBootReplyRepeatMs = 100;
inline constexpr uint32_t kRxIdleResetMs = 5000;       // carry 2a: a partial transfer with no chunk for 5 s is Reset
inline constexpr uint32_t kCtlPollMs = 250, kStatusEveryMs = 1000;
inline constexpr int kUnauthRepliesPerSecond = 10;     // carry 2d
inline constexpr uint32_t kStrangerEventMs = 10000;    // fix round I-3: one stranger-refusal event per kind per 10 s

class Lobby {
 public:
  Lobby();
  ~Lobby();  // Stop()
  Lobby(const Lobby&) = delete;
  Lobby& operator=(const Lobby&) = delete;

  // Creates and starts this lobby's one Transport (bound as `transport` says) and the helper thread that computes
  // Providers::files once. The app calls it when the player first hosts or joins (spec 4: no socket or thread before).
  // A Lobby starts once (Start or StartInSession); a second start fails.
  bool Start(Providers providers, const net::Config& transport, std::string* error);
  // Stops the transport and joins the helper thread; silent (no Bye: Disconnect sends one). Any thread except the
  // I/O thread (there it logs a WARN line and does nothing).
  void Stop();
  // UI and request-file actions (any thread; applied on the I/O thread)
  // Host: `port` 0 or the bound port hosts on the lobby's socket; another port is logged and the bound one is used
  // (the socket is bound by Start from net::Config::port; a new port takes a new lobby).
  void Host(uint16_t port);
  void Join(std::string code_or_address);
  void ReachOut(std::string code);   // the host sends punches to the joiner's code (strict routers)
  void Disconnect();
  void StartSession();               // host only, when can_start
  void SetDelay(bool automatic, int frames);
  Status status() const;             // a snapshot under a mutex

  // ---- the session process (after the restart) ----
  // Binds exactly h.local_port, sends Hello carrying h.session_token to h.peer every 400 ms until the peer's Hello
  // with the same token arrives FROM h.peer (a Hello with another token: Welcome kSession, counted kToken; a Hello with
  // the token from another address: counted kSource and logged, never taken - carry 2c: the hand-off's peer is the
  // address the lobby's socket saw, and the session accepts the peer only there), then SetPeer(peer, token) on the
  // transport so GekkoNet traffic flows; pings every 500 ms; a Leave or Bye from the peer calls on_peer_left(frame,
  // reason) once (I/O thread; a Bye gives frame -1, kLeft). The frame-0 limit is np_live's, not the lobby's. The
  // transport config is used as given except port/exact_port (the caller sets wifi_streaming for a session).
  // State: kConnecting until the peer's Hello, then kInSession.
  bool StartInSession(Providers providers, const net::Config& transport, const handoff::Handoff& h,
                      std::function<void(int32_t frame, wire::LeaveReason reason)> on_peer_left, std::string* error);
  void SendLeave(int32_t frame, wire::LeaveReason reason);  // any thread; sent 3 times on the I/O thread
  // Task 15 G2 (a session closed by the player): sends Leave(frame, kLeft), waits until it went out (3 sends) or
  // max_wait_ms passed, then stops the transport. True when every send went out in time. Any thread but the I/O one.
  bool QuitSession(int32_t frame, uint32_t max_wait_ms);
  net::Transport& transport();       // for the game thread's GekkoNet Send/Receive (PushGekko/PopGekko)

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

// Pure helpers (exposed for the host test).
// Printable ASCII only (0x20..0x7E; anything else becomes '?'), at most `max` bytes: every text received is shown or
// logged through this (carry 2a).
std::string Sanitize(std::string_view text, size_t max = wire::kMaxText);
// The files text with 1 added (mod 2^64) to the hash of `group` (DOA4_NP_TEST_FILES_SALT); unchanged when the group is
// absent or not a hash.
std::string SaltFiles(const std::string& files, const std::string& group);

}  // namespace doa4::np::online
