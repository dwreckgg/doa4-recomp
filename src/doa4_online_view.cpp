#include "doa4_online_view.h"

#include <cmath>
#include <cstdio>

#include "net/np_rules.h"

namespace doa4::online_view {

namespace {

using np::online::State;

constexpr const char* kSessionRefusedText = "The other player's game did not start the session.";
constexpr const char* kDisplayNote = "The host's display settings are used during the session";
constexpr const char* kJoinerHint = "The host starts the online session; both games then restart.";

// The names status.txt uses (np_online StateName); the pure view cannot link the lobby.
const char* StateText(State s) {
  switch (s) {
    case State::kOffline: return "offline";
    case State::kHosting: return "hosting";
    case State::kConnecting: return "connecting";
    case State::kConnected: return "connected";
    case State::kStarting: return "starting";
    case State::kRestarting: return "restarting";
    case State::kInSession: return "in_session";
  }
  return "offline";
}

std::string PingText(float ping_ms) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%lld ms", std::llround(ping_ms));
  return buf;
}

std::string DelayText(int frames) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "%d frame%s (%lld ms)", frames, frames == 1 ? "" : "s",
                std::llround(frames * 1000.0 / 60.0));
  return buf;
}

bool Connected(State s) {
  return s == State::kConnected || s == State::kStarting || s == State::kRestarting || s == State::kInSession;
}

}  // namespace

std::vector<std::string> StatsLines(bool connected, float ping_ms, int delay_frames) {
  if (!connected) return {};
  return {"Ping " + PingText(ping_ms), "Delay " + DelayText(delay_frames)};
}

View Describe(const In& in) {
  View v;
  const np::online::Status& st = in.lobby;
  // A session process is "in session" whatever its lobby says yet (before the peer's first Hello the lobby is
  // kConnecting); a lobby that was never opened is Offline.
  const State state = !in.lobby_open && !in.in_session ? State::kOffline : st.state;
  const bool session = in.in_session;
  const bool open = state != State::kOffline || session;

  if (session) {
    v.status_line = std::string("In an online session as the ") + (st.is_host ? "host" : "joiner") + " - delay " +
                    DelayText(in.delay) + ", ping " + PingText(in.ping_ms);
    v.show_leave = true;
    v.delay_enabled = false;
    v.display_note = kDisplayNote;
  } else {
    switch (state) {
      case State::kOffline:
        v.status_line = "Not connected";
        v.show_join = v.show_host = true;
        v.delay_enabled = true;
        break;
      case State::kHosting:
        v.status_line = "Waiting for the other player to join";
        v.show_disconnect = true;
        v.show_reach_out = true;
        v.delay_enabled = true;
        break;
      case State::kConnecting:
        v.status_line = "Connecting...";
        v.show_disconnect = true;
        v.delay_enabled = true;
        break;
      case State::kConnected:
        v.status_line = "Connected to the other player (" +
                        (st.ping_samples > 0 ? "ping " + PingText(st.ping_ms) : std::string("measuring the ping...")) +
                        ")";
        v.show_disconnect = true;
        v.show_start = st.is_host;
        v.start_enabled = st.is_host && st.can_start;
        v.delay_enabled = true;
        break;
      case State::kStarting:
        v.status_line = "Starting the online session...";
        v.show_disconnect = true;
        break;
      case State::kRestarting:
        v.status_line = "Restarting into the online session...";
        break;
      case State::kInSession:  // a lobby that says so without the process flag: treat as a session too
        v.status_line = std::string("In an online session as the ") + (st.is_host ? "host" : "joiner");
        v.show_leave = true;
        break;
    }
    if (state == State::kConnected || state == State::kStarting || state == State::kRestarting) {
      if (!st.is_host) v.display_note = kDisplayNote;
    }
  }

  // the code: shown while hosting or connected as the host, when the lobby has one
  if (!session && (state == State::kHosting || (state == State::kConnected && st.is_host)) && !st.my_code.empty()) {
    v.show_code = true;
  }
  v.code_note = st.code_note;
  v.hint = (state == State::kConnected && !st.is_host && !session) ? kJoinerHint : st.hint;

  v.event_line = !st.event.empty() ? st.event : (st.session_refused ? kSessionRefusedText : "");
  if (open) v.diagnostics_line = "Dropped packets: " + std::to_string(st.dropped);

  if (in.show_stats) {
    const bool connected = session || Connected(state);
    v.stats_lines = StatsLines(connected, session ? in.ping_ms : st.ping_ms, session ? in.delay : st.delay);
  }

  if (np::rules::IsDevBuild(in.release)) {
    v.version_line = in.release + ", self-built - only the same build can join";
  } else {
    v.version_line = in.release;
  }
  if (in.build == 0) {
    v.version_line += " (build unreadable)";
  } else {
    char buf[40];
    std::snprintf(buf, sizeof buf, " (build %016llx)", static_cast<unsigned long long>(in.build));
    v.version_line += buf;
  }
  return v;
}

bool LockedInSession(Control, bool in_session) { return in_session; }

bool Refuse(Control control, bool in_session, std::string_view action, std::string* status,
            const std::function<void(const std::string&)>& log) {
  if (!LockedInSession(control, in_session)) return false;
  if (status) *status = kLockedText;
  if (log) log("[doa4][NET] menu: refused " + std::string(action) + " in session");
  return true;
}

std::string LockedLine(bool in_session) {
  if (!in_session) return {};
  auto flag = [&](Control c) { return LockedInSession(c, in_session) ? "1" : "0"; };
  return std::string("[doa4][NET] menu: locked in session: extras=") + flag(Control::kExtras) +
         " video_restart=" + flag(Control::kVideoRestart) + " audio_restart=" + flag(Control::kAudioRestart) +
         " game_change=" + flag(Control::kGameChange);
}

std::string BannerText(bool waiting, Ending ending) {
  switch (ending) {
    case Ending::kDesync: return "The games went out of sync. The session ended; both games restart.";
    case Ending::kLost: return "The other player stopped responding. The session ended; both games restart.";
    case Ending::kPeerLeft: return "The other player left the session.";
    case Ending::kFrame0Timeout: return "The other player's game did not come back.";
    case Ending::kLeft: return "You left the session.";
    case Ending::kInternal: return "The session ended unexpectedly; both games restart.";
    case Ending::kNone: break;
  }
  return waiting ? "Waiting for the other player..." : "";
}

std::string TabLogLine(const In& in, const View& view) {
  const State state = in.in_session ? State::kInSession : (in.lobby_open ? in.lobby.state : State::kOffline);
  return std::string("[doa4][NET] tab: ") + StateText(state) + " start=" + (view.start_enabled ? "1" : "0") +
         " leave=" + (view.show_leave ? "1" : "0");
}

}  // namespace doa4::online_view
