#include "system_monitor_ui.h"

#include <imgui.h>

#include "../../utils/utils.h"

void SystemMonitorUI::renderUI()
{
    if (!ImGui::CollapsingHeader("System Monitor", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGuiIO &io = ImGui::GetIO();
    ImGui::Text("FPS: %.1f", io.Framerate);
    uint64_t m_ramUsage = Utils::getRamUsage();
    ImGui::Text("RAM: %.2f MB", static_cast<float>(m_ramUsage) / (1024.0f * 1024.0f));
}
