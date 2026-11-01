// Host test for src/doa4_online_view (Task 14): what the Online tab shows and enables, the banner words, the F9 lines
// and the four session locks. Built and run by run_online_view_test.bat. No files, no sockets.

#include <cstdio>
#include <string>
#include <vector>

#include "../../src/doa4_online_view.h"

using namespace doa4::online_view;
using doa4::np::online::State;
using doa4::np::online::Status;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      std::fflush(stdout);                                             \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)
#define CHECK_EQ(a, b)                                                                                      \
  do {                                                                                                      \
    ++g_checks;                                                                                             \
    const std::string va_ = (a), vb_ = (b);                                                                 \
    if (va_ != vb_) {                                                                                       \
      std::printf("FAIL %s:%d: %s\n   got:  [%s]\n   want: [%s]\n", __FILE__, __LINE__, #a, va_.c_str(),   \
                  vb_.c_str());                                                                             \
      std::fflush(stdout);                                                                                  \
      ++g_failures;                                                                                         \
    }                                                                                                       \
  } while (0)

static In Open(State state, bool host) {
  In in;
  in.lobby_open = true;
  in.lobby.state = state;
  in.lobby.is_host = host;
  in.release = "netplay-v1.2.3";
  in.build = 0x0123456789abcdefull;
  return in;
}

static bool Has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static void TestOffline() {
  In in;  // the lobby is not open: nothing exists yet
  const View v = Describe(in);
  CHECK(v.show_join && v.show_host);
  CHECK(!v.show_leave && !v.show_disconnect && !v.show_start && !v.show_code && !v.show_reach_out);
  CHECK(!v.start_enabled);
  CHECK(v.delay_enabled);
  CHECK_EQ(v.status_line, "Not connected");
  CHECK(v.stats_lines.empty());
  CHECK_EQ(v.diagnostics_line, "");
  CHECK_EQ(v.display_note, "");
  // an open lobby that is Offline looks the same
  View o = Describe(Open(State::kOffline, false));
  CHECK(o.show_join && o.show_host && !o.show_disconnect && o.stats_lines.empty() && o.diagnostics_line.empty());
  // Offline with show_stats on: still no stats lines, and no dropped counter even when the lobby counted some
  In s = Open(State::kOffline, false);
  s.show_stats = true;
  s.lobby.dropped = 9;
  CHECK(Describe(s).stats_lines.empty());
  CHECK_EQ(Describe(s).diagnostics_line, "");
}

static void TestHosting() {
  In in = Open(State::kHosting, true);
  in.lobby.my_code = "AbCdEfG";
  in.lobby.code_note = "Public address found";
  in.show_stats = true;
  const View v = Describe(in);
  CHECK(v.show_code && v.show_reach_out && v.show_disconnect);
  CHECK(!v.show_join && !v.show_host && !v.show_start && !v.show_leave);
  CHECK_EQ(v.code_note, "Public address found");
  CHECK_EQ(v.status_line, "Waiting for the other player to join");
  CHECK(v.stats_lines.empty());  // not connected yet
  CHECK_EQ(v.diagnostics_line, "Dropped packets: 0");
  CHECK(v.delay_enabled);
  // a hosting lobby with no code yet (the STUN answer is pending) shows no code
  In nocode = in;
  nocode.lobby.my_code.clear();
  CHECK(!Describe(nocode).show_code);
}

static void TestConnectedHost() {
  In in = Open(State::kConnected, true);
  in.lobby.my_code = "AbCdEfG";
  in.lobby.ping_ms = 42;
  in.lobby.delay = 2;
  in.lobby.ping_samples = 3;
  in.lobby.can_start = false;  // the lobby's R14: >= 4 ping samples
  View v = Describe(in);
  CHECK(v.show_start && !v.start_enabled);  // 3 samples: shown, disabled
  CHECK(v.show_disconnect && !v.show_leave && !v.show_join && !v.show_host);
  CHECK(!v.show_reach_out);
  CHECK(v.delay_enabled);
  CHECK(Has(v.status_line, "42 ms"));
  // final review R2-I3: the Connected line names the peer's address (so the host can tell its friend from a stranger)
  in.lobby.peer_address = "198.51.100.7:7200";
  CHECK_EQ(Describe(in).status_line, "Connected to the other player at 198.51.100.7:7200 (ping 42 ms)");
  in.lobby.peer_address.clear();
  CHECK_EQ(Describe(in).status_line, "Connected to the other player (ping 42 ms)");  // no address known: as before
  in.lobby.ping_samples = 4;
  in.lobby.can_start = true;
  v = Describe(in);
  CHECK(v.show_start && v.start_enabled);  // 4 samples: enabled
  // the hint comes from the lobby when it is the host's
  in.lobby.hint = "Press Start online session (both games restart).";
  CHECK_EQ(Describe(in).hint, "Press Start online session (both games restart).");
  // Connected with no ping sample yet
  In none = Open(State::kConnected, true);
  CHECK(Has(Describe(none).status_line, "measuring"));
}

static void TestConnectedJoiner() {
  In in = Open(State::kConnected, false);
  in.lobby.ping_ms = 42;
  in.lobby.ping_samples = 9;
  in.lobby.can_start = false;
  in.lobby.hint = "something else the lobby said";
  const View v = Describe(in);
  CHECK(!v.show_start);
  CHECK(!v.show_code);  // a joiner has no code to hand out, even when its lobby holds one
  In coded = in;
  coded.lobby.my_code = "AbCdEfG";
  CHECK(!Describe(coded).show_code);
  CHECK(v.show_disconnect && !v.show_leave);
  CHECK_EQ(v.hint, "The host starts the online session; both games then restart.");
  CHECK_EQ(v.display_note, "The host's display settings are used during the session");
  // a joiner that is still Connecting has no hint of ours
  CHECK(Describe(Open(State::kConnecting, false)).hint !=
        "The host starts the online session; both games then restart.");
  CHECK_EQ(Describe(Open(State::kConnecting, false)).status_line, "Connecting...");
  CHECK(Describe(Open(State::kConnecting, false)).show_disconnect);
}

static void TestStartingAndRestarting() {
  const View st = Describe(Open(State::kStarting, true));
  CHECK_EQ(st.status_line, "Starting the online session...");
  CHECK(!st.delay_enabled);  // locked while starting
  CHECK(st.show_disconnect && !st.show_start);
  const View re = Describe(Open(State::kRestarting, false));
  CHECK_EQ(re.status_line, "Restarting into the online session...");
  CHECK(!re.delay_enabled && !re.show_disconnect && !re.show_leave);
}

static void TestInSession() {
  In in = Open(State::kInSession, true);
  in.in_session = true;
  in.frame = 1200;
  in.delay = 3;
  in.ping_ms = 42.4f;
  in.show_stats = true;
  in.lobby.dropped = 3;
  View v = Describe(in);
  CHECK(v.show_leave && !v.show_disconnect && !v.show_join && !v.show_host && !v.show_start);
  CHECK(!v.delay_enabled);
  CHECK(Has(v.status_line, "host") && !Has(v.status_line, "joiner"));
  CHECK(Has(v.status_line, "3 frames") && Has(v.status_line, "42 ms"));
  CHECK_EQ(v.diagnostics_line, "Dropped packets: 3");
  CHECK_EQ(v.display_note, "The host's display settings are used during the session");
  // the process-level flag wins over a lobby that has not reached kInSession yet (the peer's first Hello is pending)
  In early = in;
  early.lobby.state = State::kConnecting;
  early.lobby.is_host = false;
  const View e = Describe(early);
  CHECK(e.show_leave && !e.show_disconnect && !e.delay_enabled);
  CHECK(Has(e.status_line, "joiner"));
  // the session's ping and delay come from the session, not from the lobby's own fields
  in.lobby.ping_ms = 99;
  in.lobby.delay = 7;
  v = Describe(in);
  CHECK(Has(v.status_line, "3 frames") && Has(v.status_line, "42 ms") && !Has(v.status_line, "99"));
  CHECK(v.stats_lines.size() == 2);
  if (v.stats_lines.size() == 2) {
    CHECK_EQ(v.stats_lines[0], "Ping 42 ms");
    CHECK_EQ(v.stats_lines[1], "Delay 3 frames (50 ms)");
  }
}

static void TestStats() {
  In in = Open(State::kConnected, true);
  in.show_stats = true;
  in.lobby.ping_ms = 42;
  in.lobby.delay = 2;
  View v = Describe(in);
  CHECK(v.stats_lines.size() == 2);
  if (v.stats_lines.size() == 2) {
    CHECK_EQ(v.stats_lines[0], "Ping 42 ms");
    CHECK_EQ(v.stats_lines[1], "Delay 2 frames (33 ms)");
  }
  in.lobby.delay = 1;
  v = Describe(in);
  CHECK(v.stats_lines.size() == 2);
  if (v.stats_lines.size() == 2) CHECK_EQ(v.stats_lines[1], "Delay 1 frame (17 ms)");
  in.lobby.delay = 8;
  v = Describe(in);
  CHECK(v.stats_lines.size() == 2);
  if (v.stats_lines.size() == 2) CHECK_EQ(v.stats_lines[1], "Delay 8 frames (133 ms)");
  // the box follows the F9 setting
  in.show_stats = false;
  CHECK(Describe(in).stats_lines.empty());
  // rounding of the ping: 41.6 -> 42, 41.4 -> 41
  in.show_stats = true;
  in.lobby.ping_ms = 41.6f;
  v = Describe(in);
  CHECK(v.stats_lines.size() == 2);
  if (!v.stats_lines.empty()) CHECK_EQ(v.stats_lines[0], "Ping 42 ms");
  in.lobby.ping_ms = 41.4f;
  v = Describe(in);
  if (!v.stats_lines.empty()) CHECK_EQ(v.stats_lines[0], "Ping 41 ms");
  // the free function: not connected -> nothing
  CHECK(StatsLines(false, 42, 2).empty());
  CHECK(StatsLines(true, 42, 2).size() == 2);
}

static void TestDiagnostics() {
  In in = Open(State::kConnected, true);
  in.lobby.dropped = 3;
  CHECK_EQ(Describe(in).diagnostics_line, "Dropped packets: 3");
  in.lobby.dropped = 0;
  CHECK_EQ(Describe(in).diagnostics_line, "Dropped packets: 0");
  CHECK_EQ(Describe(Open(State::kOffline, false)).diagnostics_line, "");
}

static void TestVersionLine() {
  In in = Open(State::kOffline, false);
  View v = Describe(in);
  CHECK_EQ(v.version_line, "netplay-v1.2.3 (build 0123456789abcdef)");
  in.release = "dev";
  v = Describe(in);
  CHECK(Has(v.version_line, "dev, self-built"));
  CHECK(Has(v.version_line, "only the same build can join"));
  CHECK(Has(v.version_line, "0123456789abcdef"));
  // a release tag whose build number is 0 is a dev build too
  in.release = "netplay-v1.2.0";
  CHECK(Has(Describe(in).version_line, "self-built"));
  // an unreadable build id
  in.release = "netplay-v1.2.3";
  in.build = 0;
  CHECK(Has(Describe(in).version_line, "build unreadable"));
  CHECK(!Has(Describe(in).version_line, "0000000000000000"));
}

static void TestEventLine() {
  In in = Open(State::kOffline, false);
  in.lobby.event = "Your game and the host's are not the same build.";
  in.lobby.refusal = "build";
  CHECK_EQ(Describe(in).event_line, "Your game and the host's are not the same build.");
  // the session process: the lobby set the flag; the flag alone still shows the text
  In r = Open(State::kOffline, false);
  r.in_session = true;
  r.lobby.session_refused = true;
  CHECK_EQ(Describe(r).event_line, "The other player's game did not start the session.");
  r.lobby.event = "The other player's game did not start the session.";
  CHECK_EQ(Describe(r).event_line, "The other player's game did not start the session.");
  // no event, no flag: nothing
  CHECK_EQ(Describe(Open(State::kOffline, false)).event_line, "");
}

static void TestLocks() {
  const Control all[] = {Control::kExtras, Control::kVideoRestart, Control::kAudioRestart, Control::kGameChange};
  for (const Control c : all) {
    CHECK(LockedInSession(c, true));
    CHECK(!LockedInSession(c, false));
    // the refusal reads the same answer as the disabled state
    for (const bool s : {false, true}) {
      std::string status = "unchanged";
      std::vector<std::string> log;
      const bool refused = Refuse(c, s, "tweak x", &status, [&](const std::string& l) { log.push_back(l); });
      CHECK(refused == LockedInSession(c, s));
      if (s) {
        CHECK_EQ(status, "Not during an online session");
        CHECK(log.size() == 1);
        if (log.size() == 1) CHECK_EQ(log[0], "[doa4][NET] menu: refused tweak x in session");
      } else {
        CHECK_EQ(status, "unchanged");  // nothing written when not refused
        CHECK(log.empty());
      }
    }
  }
  CHECK(Refuse(Control::kExtras, true, "a", nullptr, {}));  // null status and empty log are allowed
  CHECK_EQ(LockedLine(true),
           "[doa4][NET] menu: locked in session: extras=1 video_restart=1 audio_restart=1 game_change=1");
  CHECK_EQ(LockedLine(false), "");
}

// Fix round M2: one notion of "locked" - in a session, or the lobby is Starting / Restarting - and the input delay, the
// four controls and the tab's session note all follow it.
static void TestSessionLocks() {
  const State all[] = {State::kOffline,    State::kHosting,    State::kConnecting, State::kConnected,
                       State::kStarting,   State::kRestarting, State::kInSession};
  int locked_states = 0;
  for (const State st : all) {
    for (const bool open : {false, true}) {
      for (const bool session : {false, true}) {
        In in = Open(st, true);
        in.lobby_open = open;
        in.in_session = session;
        const bool want = session || (open && (st == State::kStarting || st == State::kRestarting || st == State::kInSession));
        CHECK(SessionLocks(in) == want);
        // the input delay and the controls read the same answer
        CHECK(Describe(in).delay_enabled == !SessionLocks(in));
        for (const Control c : {Control::kExtras, Control::kVideoRestart, Control::kAudioRestart, Control::kGameChange}) {
          CHECK(LockedInSession(c, SessionLocks(in)) == want);
        }
        locked_states += want;
      }
    }
  }
  // 7 states x (lobby open or not) x (session flag): locked = flag, or open and Starting / Restarting / InSession
  CHECK(locked_states == 7 * 2 + 3);
  // the named cases
  CHECK(!SessionLocks(In{}));
  CHECK(!SessionLocks(Open(State::kConnected, true)));
  CHECK(SessionLocks(Open(State::kStarting, true)));
  CHECK(SessionLocks(Open(State::kRestarting, false)));
  In s;
  s.in_session = true;
  CHECK(SessionLocks(s));
  // a Starting lobby locks the four controls
  CHECK(LockedInSession(Control::kExtras, SessionLocks(Open(State::kStarting, true))));
  CHECK(!LockedInSession(Control::kExtras, SessionLocks(Open(State::kHosting, true))));
}

// Fix round I2: the build id is wanted only when the tab draws or a lobby / session exists.
static void TestNeedsBuildId() {
  CHECK(!NeedsBuildId(false, false, false));
  CHECK(NeedsBuildId(true, false, false));
  CHECK(NeedsBuildId(false, true, false));
  CHECK(NeedsBuildId(false, false, true));
  CHECK(NeedsBuildId(true, true, true));
}

// Fix round M3: the session note.
static void TestSessionNote() {
  In in = Open(State::kInSession, true);
  in.in_session = true;
  CHECK_EQ(Describe(in).session_note, "Settings changed during a session are restored when it ends.");
  CHECK_EQ(Describe(Open(State::kConnected, true)).session_note, "");
  CHECK_EQ(Describe(In{}).session_note, "");
  CHECK_EQ(Describe(Open(State::kStarting, true)).session_note, "");
}

static void TestBanner() {
  CHECK_EQ(BannerText(false, Ending::kNone), "");
  CHECK_EQ(BannerText(true, Ending::kNone), "Waiting for the other player...");
  CHECK_EQ(BannerText(false, Ending::kDesync), "The games went out of sync. The session ended; both games restart.");
  CHECK_EQ(BannerText(false, Ending::kLost),
           "The other player stopped responding. The session ended; both games restart.");
  CHECK_EQ(BannerText(false, Ending::kPeerLeft), "The other player left the session.");  // review I2: by reason
  CHECK_EQ(BannerText(false, Ending::kPeerQuit), "The other player quit the session.");
  CHECK_EQ(BannerText(false, Ending::kPeerNotStarted), "The other player's game did not start the session.");
  // the reason -> ending map (wire::LeaveReason numbers: 0 left, 1 desync, 2 timeout, 3 internal; frame -1 = a Bye)
  CHECK(PeerLeftEnding(0, 4642, false) == Ending::kPeerQuit);       // the quit Leave (Shutdown, menu Leave)
  CHECK(PeerLeftEnding(0, -1, false) == Ending::kPeerLeft);         // a Bye
  CHECK(PeerLeftEnding(2, 100, false) == Ending::kPeerLeft);        // kTimeout
  CHECK(PeerLeftEnding(3, 100, false) == Ending::kPeerLeft);        // kInternal
  CHECK(PeerLeftEnding(7, 100, false) == Ending::kPeerLeft);        // anything else
  CHECK(PeerLeftEnding(3, 0, true) == Ending::kPeerNotStarted);     // the refused-session path
  CHECK(PeerLeftEnding(0, 5, true) == Ending::kPeerNotStarted);
  CHECK_EQ(BannerText(false, Ending::kFrame0Timeout), "The other player's game did not come back.");
  CHECK_EQ(BannerText(false, Ending::kLeft), "You left the session.");
  CHECK_EQ(BannerText(false, Ending::kInternal), "The session ended unexpectedly; both games restart.");
  // an ending wins over a hold that was still showing
  CHECK_EQ(BannerText(true, Ending::kLost), BannerText(false, Ending::kLost));
  CHECK(BannerText(true, Ending::kLeft) != "Waiting for the other player...");
}

static void TestTabLogLine() {
  In in = Open(State::kConnected, true);
  in.lobby.can_start = true;
  CHECK_EQ(TabLogLine(in, Describe(in)), "[doa4][NET] tab: connected start=1 leave=0");
  in.lobby.can_start = false;
  CHECK_EQ(TabLogLine(in, Describe(in)), "[doa4][NET] tab: connected start=0 leave=0");
  In s = Open(State::kInSession, true);
  s.in_session = true;
  CHECK_EQ(TabLogLine(s, Describe(s)), "[doa4][NET] tab: in_session start=0 leave=1");
  In h = Open(State::kHosting, true);
  CHECK_EQ(TabLogLine(h, Describe(h)), "[doa4][NET] tab: hosting start=0 leave=0");
  CHECK_EQ(TabLogLine(In{}, Describe(In{})), "[doa4][NET] tab: offline start=0 leave=0");
  // an in_session process whose lobby still says connecting reports the session (the process-level flag wins)
  In e = Open(State::kConnecting, false);
  e.in_session = true;
  CHECK_EQ(TabLogLine(e, Describe(e)), "[doa4][NET] tab: in_session start=0 leave=1");
}

// Plan 1b Task 4: Check for Updates is enabled only when the lobby is Offline (or was never opened) and no session runs
// (spec 3.1 "disabled while online"); the note beside the disabled button is the updater's kOnlineText.
static void TestUpdateButton() {
  const View none = Describe(In{});  // no lobby yet
  CHECK(none.update_enabled);
  CHECK_EQ(none.update_note, "");
  const View off = Describe(Open(State::kOffline, false));  // a lobby that is Offline again (after Disconnect)
  CHECK(off.update_enabled);
  CHECK_EQ(off.update_note, "");
  int disabled = 0;
  for (State s : {State::kHosting, State::kConnecting, State::kConnected, State::kStarting, State::kRestarting,
                  State::kInSession}) {
    for (bool host : {false, true}) {
      const View v = Describe(Open(s, host));
      CHECK(!v.update_enabled);
      CHECK_EQ(v.update_note, "Disconnect from online play first.");
      disabled += v.update_enabled ? 0 : 1;
    }
  }
  CHECK(disabled == 12);
  // a session process: disabled whatever its lobby says (no lobby, an Offline one, one still connecting)
  In a;
  a.in_session = true;
  CHECK(!Describe(a).update_enabled);
  In b = Open(State::kOffline, false);
  b.in_session = true;
  CHECK(!Describe(b).update_enabled);
  CHECK_EQ(Describe(b).update_note, "Disconnect from online play first.");
  In c = Open(State::kConnecting, true);
  c.in_session = true;
  CHECK(!Describe(c).update_enabled);
}

// Review fix round I-1: while a check downloads, and after it swapped files in this process, Host and Join are disabled
// with the reason beside them; a download also disables the button itself (one check at a time).
static void TestUpdateLocksJoinHost() {
  const View none = Describe(In{});
  CHECK(none.join_host_enabled);
  CHECK_EQ(none.join_host_note, "");
  In busy;
  busy.update_busy = true;
  const View b = Describe(busy);
  CHECK(!b.join_host_enabled);
  CHECK_EQ(b.join_host_note, "An update is downloading.");
  CHECK(!b.update_enabled);
  CHECK(b.show_join && b.show_host);  // still shown, disabled
  In pending;
  pending.restart_pending = true;
  const View p = Describe(pending);
  CHECK(!p.join_host_enabled);
  CHECK_EQ(p.join_host_note, "Restart the game to finish the update.");
  CHECK(p.update_enabled);  // pressing again only says to restart
  In both = busy;
  both.restart_pending = true;
  CHECK(!Describe(both).join_host_enabled);
  CHECK_EQ(Describe(both).join_host_note, "Restart the game to finish the update.");
  // an Offline lobby (after Disconnect) is gated the same way
  In off = Open(State::kOffline, false);
  off.update_busy = true;
  CHECK(!Describe(off).join_host_enabled);
}

// Review fix round G-R: a joiner that is still connecting shows its own code, so that the host can use "Reach out"
// (strict routers); only when the lobby has one, and never in a session process.
static void TestJoinerCodeWhileConnecting() {
  In in = Open(State::kConnecting, false);
  in.lobby.my_code = "AbCdEfG";
  in.lobby.code_note = "Public address found";
  const View v = Describe(in);
  CHECK(v.show_code);
  CHECK_EQ(v.code_note, "Taking long? Send this code to the host; they type it under Strict router? and press Reach out.");
  In none = Open(State::kConnecting, false);  // no STUN answer and no LAN address: nothing to show
  CHECK(!Describe(none).show_code);
  In session = in;  // a session process reconnecting is not a joiner looking for a host
  session.in_session = true;
  CHECK(!Describe(session).show_code);
  // the host's code and note while hosting are as before
  In host = Open(State::kHosting, true);
  host.lobby.my_code = "AbCdEfG";
  host.lobby.code_note = "Public address found";
  CHECK(Describe(host).show_code);
  CHECK_EQ(Describe(host).code_note, "Public address found");
}

int main() {
  TestOffline();
  TestHosting();
  TestConnectedHost();
  TestConnectedJoiner();
  TestStartingAndRestarting();
  TestInSession();
  TestStats();
  TestDiagnostics();
  TestVersionLine();
  TestEventLine();
  TestLocks();
  TestSessionLocks();
  TestNeedsBuildId();
  TestSessionNote();
  TestBanner();
  TestTabLogLine();
  TestUpdateButton();
  TestUpdateLocksJoinHost();
  TestJoinerCodeWhileConnecting();
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
