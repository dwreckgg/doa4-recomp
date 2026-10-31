// What the Online tab, the waiting/ended banner and the F9 box show and enable (netplay phase 1, Task 14). Pure: no
// ImGui, no SDK, no Windows; the ImGui code (doa4_online_tab, doa4_net_overlay, doa4_menu) only draws what Describe
// and the helpers below decide, so the decisions are host-tested (tests/host/online_view_host_test.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "net/np_online.h"

namespace doa4::online_view {

struct In {
  np::online::Status lobby;     // the lobby's Status snapshot (default Offline when lobby_open is false)
  bool lobby_open = false;  // the Online tab has created the lobby (spec 4: nothing exists before)
  bool in_session = false;  // this process runs an online session
  int32_t frame = -1;       // the session frame (-1 outside a session)
  int delay = 2;            // the session's input delay in frames
  float ping_ms = 0, frames_ahead = 0;
  std::string release;      // DOA4_RELEASE_TAG
  uint64_t build = 0;       // np::BuildIdValue() (0 = unreadable)
  bool show_stats = false;  // netplay_show_stats (the F9 box)
};

struct View {
  std::string status_line;
  std::string event_line;   // Status::event; the session_refused text when the lobby set only the flag
  bool show_join = false, show_host = false, show_code = false, show_reach_out = false, show_start = false,
       show_leave = false, show_disconnect = false;
  bool start_enabled = false, delay_enabled = false;  // delay locked while starting / in a session
  std::string code_note, hint, version_line;           // version_line: "<tag> (build <16 hex>)" or "<tag>, self-built ..."
  std::vector<std::string> stats_lines;                // F9 box; empty when not connected or show_stats is off
  std::string diagnostics_line;                        // "Dropped packets: N" (spec 7); empty when Offline
  std::string display_note;                            // "The host's display settings are used during the session" or empty
};

View Describe(const In& in);

// The four game-state controls a session locks (spec 6): every one of them reads its disabled state from here, and the
// refusal behind them (Refuse) reads the same answer, so the UI and the refusal cannot disagree.
enum class Control : uint8_t { kExtras, kVideoRestart, kAudioRestart, kGameChange };
bool LockedInSession(Control control, bool in_session);
inline constexpr const char* kLockedText = "Not during an online session";
// True = refused: *status (may be null) gets kLockedText and `log` (may be empty) gets
// "[doa4][NET] menu: refused <action> in session". Refuse(c, x, ...) == LockedInSession(c, x).
bool Refuse(Control control, bool in_session, std::string_view action, std::string* status,
            const std::function<void(const std::string&)>& log);
// The one line the menu logs when in_session turns true:
// "[doa4][NET] menu: locked in session: extras=1 video_restart=1 audio_restart=1 game_change=1".
std::string LockedLine(bool in_session);

// The banner (spec 7): how a session ended, as the overlay's own enum (the overlay maps live::End onto it).
enum class Ending : uint8_t { kNone, kLeft, kPeerLeft, kLost, kDesync, kFrame0Timeout, kInternal };
// "" = no banner. An ending wins over a hold; a hold (waiting) is "Waiting for the other player...".
std::string BannerText(bool waiting, Ending ending);

// The F9 box lines for a connection (empty when `connected` is false): "Ping 42 ms", "Delay 2 frames (33 ms)".
std::vector<std::string> StatsLines(bool connected, float ping_ms, int delay_frames);
// "Last line the tab logs once per change": "[doa4][NET] tab: <state> start=<0|1> leave=<0|1>".
std::string TabLogLine(const In& in, const View& view);

}  // namespace doa4::online_view
