// The online banner and the F9 box. See doa4_net_overlay.h.

#include "doa4_net_overlay.h"

#include <imgui.h>

#include <utility>

namespace doa4 {

NetOverlay::NetOverlay(rex::ui::ImGuiDrawer* drawer, std::function<Content()> poll)
    : rex::ui::ImGuiDialog(drawer), poll_(std::move(poll)) {}

void NetOverlay::OnDraw(ImGuiIO& io) {
  const Content content = poll_ ? poll_() : Content{};
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (!content.banner.empty()) {
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 24.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("##doa4_net_banner", nullptr, flags)) {
      ImGui::TextUnformatted(content.banner.c_str());
    }
    ImGui::End();
  }
  if (!content.stats.empty()) {
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16.0f, 16.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (ImGui::Begin("##doa4_net_stats", nullptr, flags)) {
      for (const std::string& line : content.stats) ImGui::TextUnformatted(line.c_str());
    }
    ImGui::End();
  }
}

}  // namespace doa4
