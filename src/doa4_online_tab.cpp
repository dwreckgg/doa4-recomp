// The Online tab. See doa4_online_tab.h.

#include "doa4_online_tab.h"

#include <imgui.h>

#include <cstdio>

#include <rex/cvar.h>

#include "doa4_updater.h"  // Message() only (the app starts checks)
#include "net/np_rules.h"

namespace doa4 {

namespace {
constexpr ImVec4 kNoteColor(1.0f, 0.85f, 0.4f, 1.0f);
constexpr ImVec4 kErrorColor(1.0f, 0.45f, 0.4f, 1.0f);
constexpr ImVec4 kDoneColor(0.45f, 0.9f, 0.5f, 1.0f);
}  // namespace

void OnlineTab::Draw(const OnlineHost& host, const online_view::In& in, const online_view::View& view,
                     const std::function<bool(std::string_view, std::string_view, bool)>& save_then_set,
                     const std::function<void()>& open_logs) {
  if (!host.state) {
    ImGui::TextDisabled("Online play is not available in this build.");
    return;
  }
  if (!join_seeded_) {
    join_seeded_ = true;
    const std::string last = rex::cvar::Query<std::string>("netplay_last_join");
    std::snprintf(join_, sizeof join_, "%s", last.c_str());
  }

  ImGui::SeparatorText("Online play");
  ImGui::TextWrapped("%s", view.status_line.c_str());
  if (!view.event_line.empty()) {
    const bool refusal = in.lobby.refusal != "none" || in.lobby.session_refused;
    ImGui::TextColored(refusal ? kErrorColor : kNoteColor, "%s", view.event_line.c_str());
  }
  if (!view.diagnostics_line.empty()) ImGui::TextDisabled("%s", view.diagnostics_line.c_str());
  if (!view.display_note.empty()) ImGui::TextColored(kNoteColor, "%s", view.display_note.c_str());
  if (!view.session_note.empty()) ImGui::TextColored(kNoteColor, "%s", view.session_note.c_str());

  if (view.show_code) {
    ImGui::SeparatorText("Your code");
    ImGui::TextUnformatted(in.lobby.my_code.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Copy")) ImGui::SetClipboardText(in.lobby.my_code.c_str());
    if (!view.code_note.empty()) ImGui::TextDisabled("%s", view.code_note.c_str());
  }

  if (view.show_join || view.show_host) {
    ImGui::SeparatorText("Play with a friend");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("##join_code", join_, sizeof join_);
    ImGui::SameLine();
    np::rules::Endpoint ep;
    const bool code_ok = join_[0] != '\0' && np::rules::ParseJoinCode(join_, &ep);
    // fix round I-1: Join and Host wait for an update check, and for the restart after a swap (the view decides)
    ImGui::BeginDisabled(!code_ok || !host.join || !view.join_host_enabled);
    if (ImGui::Button("Join") && host.join) {
      const std::string code = join_;
      save_then_set("netplay_last_join", code, true);
      host.join(code);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!host.host || !view.join_host_enabled);
    if (ImGui::Button("Host") && host.host) host.host();
    ImGui::EndDisabled();
    if (!view.join_host_note.empty()) ImGui::TextColored(kNoteColor, "%s", view.join_host_note.c_str());
    const std::string last = rex::cvar::Query<std::string>("netplay_last_join");
    if (!last.empty()) {
      if (ImGui::SmallButton("Last code")) std::snprintf(join_, sizeof join_, "%s", last.c_str());
    }
    if (join_[0] != '\0' && !code_ok) ImGui::TextColored(kErrorColor, "%s", np::rules::kBadCodeText);
  }

  if (view.show_reach_out) {
    ImGui::SeparatorText("Strict router?");
    ImGui::TextWrapped("If the other player cannot connect, type the code they show and press Reach out.");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("##reach_code", reach_, sizeof reach_);
    ImGui::SameLine();
    np::rules::Endpoint ep;
    const bool reach_ok = reach_[0] != '\0' && np::rules::ParseJoinCode(reach_, &ep);
    ImGui::BeginDisabled(!reach_ok || !host.reach_out);
    if (ImGui::Button("Reach out") && host.reach_out) host.reach_out(reach_);
    ImGui::EndDisabled();
  }

  if (view.show_start || view.show_leave || view.show_disconnect) {
    ImGui::Spacing();
    if (view.show_start) {
      ImGui::BeginDisabled(!view.start_enabled || !host.start_session);
      if (ImGui::Button("Start online session") && host.start_session) host.start_session();
      ImGui::EndDisabled();
      ImGui::SameLine();
    }
    if (view.show_leave) {
      if (ImGui::Button("Leave online session") && host.leave_session) host.leave_session();
      ImGui::SameLine();
    }
    if (view.show_disconnect) {
      if (ImGui::Button("Disconnect") && host.disconnect) host.disconnect();
    }
    ImGui::NewLine();
  }
  if (!view.hint.empty()) ImGui::TextWrapped("%s", view.hint.c_str());

  ImGui::SeparatorText("Input delay");
  ImGui::BeginDisabled(!view.delay_enabled);
  const bool automatic = rex::cvar::Query<bool>("netplay_delay_auto");
  int frames = rex::cvar::Query<int32_t>("netplay_delay");
  bool changed = false;
  if (ImGui::RadioButton("Auto", automatic) && !automatic) {
    if (save_then_set("netplay_delay_auto", "true", false)) changed = true;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Manual", !automatic) && automatic) {
    if (save_then_set("netplay_delay_auto", "false", false)) changed = true;
  }
  ImGui::BeginDisabled(automatic);
  ImGui::SetNextItemWidth(220.0f);
  if (ImGui::SliderInt("frames", &frames, 1, 8)) {
    if (save_then_set("netplay_delay", std::to_string(frames), false)) changed = true;
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  if (changed && host.set_delay) {
    host.set_delay(rex::cvar::Query<bool>("netplay_delay_auto"), rex::cvar::Query<int32_t>("netplay_delay"));
  }
  ImGui::TextDisabled("The host decides the delay for both players.");

  bool show = rex::cvar::Query<bool>("netplay_show_stats");
  if (ImGui::Checkbox("Show ping and input delay over the game (F9)", &show)) {
    // In a session nothing is written to the config (it is restored at the session's end): the setting only changes in
    // memory, as F9 does.
    const char* value = show ? "true" : "false";
    const bool ok = in.in_session ? rex::cvar::SetFlagByName("netplay_show_stats", value)
                                  : save_then_set("netplay_show_stats", value, false);
    if (ok && host.shown_changed) host.shown_changed();
  }

  ImGui::SeparatorText("About");
  ImGui::TextWrapped("%s", view.version_line.c_str());
  // Check for Updates (plan 1b Task 4; DOA2U DrawUpdateButton, doa2u_ui.cpp 433-454): disabled while online and while a
  // check runs (the view decides); the app starts the check on its own thread and its message shows below.
  ImGui::BeginDisabled(!view.update_enabled || !host.check_updates);
  if (ImGui::Button("Check for Updates") && host.check_updates) host.check_updates();
  ImGui::EndDisabled();
  if (!view.update_note.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", view.update_note.c_str());
  }
  if (const std::string msg = updater::Message(); !msg.empty()) ImGui::TextWrapped("%s", msg.c_str());
  if (open_logs && ImGui::Button("Open session logs")) open_logs();
}

}  // namespace doa4
