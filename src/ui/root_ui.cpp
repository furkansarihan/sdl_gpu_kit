#include "root_ui.h"

#include <SDL3/SDL_gpu.h>
#include <imgui.h>

#include "../external/imgui/imgui_impl_sdl3.h"
#include "../external/imgui/imgui_impl_sdlgpu3.h"

RootUI::RootUI()
    : m_enabled(true)
{
    ImGuiStyle &style = ImGui::GetStyle();

    // default values
    style.WindowBorderSize = 0.f;
    style.ChildBorderSize = 0.f;
    style.PopupBorderSize = 0.f;
    style.FrameRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.7f);
}

RootUI::~RootUI()
{
    m_uiList.clear();
}

void RootUI::render(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUTexture *swapchainTexture)
{
    if (!m_enabled)
        return;

    // Start the Dear ImGui frame
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    {
        static int corner = 0;
        ImGuiIO &io = ImGui::GetIO();
        if (corner != -1)
        {
            ImVec2 window_pos = ImVec2((corner & 1) ? io.DisplaySize.x : 0, (corner & 2) ? io.DisplaySize.y : 0);
            ImVec2 window_pos_pivot = ImVec2((corner & 1) ? 1.0f : 0.0f, (corner & 2) ? 1.0f : 0.0f);
            ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
        }

        bool p_open = true;
        if (ImGui::Begin(
                "RootUI",
                &p_open,
                (corner != -1 ? ImGuiWindowFlags_NoMove : 0) |
                    ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoFocusOnAppearing |
                    ImGuiWindowFlags_NoNav))
        {
            for (int i = 0; i < m_uiList.size(); i++)
                m_uiList.at(i)->renderUI();
        }
        ImGui::End();
    }

    // Rendering
    ImGui::Render();
    ImDrawData *draw_data = ImGui::GetDrawData();
    const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);

    if (swapchainTexture != nullptr && !is_minimized)
    {
        // This is mandatory: call ImGui_ImplSDLGPU3_PrepareDrawData() to upload the vertex/index buffer!
        ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, commandBuffer);

        // Setup and start a render pass
        SDL_GPUColorTargetInfo target_info = {};
        target_info.texture = swapchainTexture;
        target_info.load_op = SDL_GPU_LOADOP_LOAD;
        target_info.store_op = SDL_GPU_STOREOP_STORE;
        target_info.mip_level = 0;
        target_info.layer_or_depth_plane = 0;
        target_info.cycle = false;
        SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(commandBuffer, &target_info, 1, nullptr);

        // Render ImGui
        ImGui_ImplSDLGPU3_RenderDrawData(draw_data, commandBuffer, render_pass);

        SDL_EndGPURenderPass(render_pass);
    }
}

void RootUI::add(BaseUI *ui)
{
    m_uiList.push_back(ui);
}
