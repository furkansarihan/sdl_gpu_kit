#pragma once

#include <vector>

#include <SDL3/SDL_gpu.h>

#include "base_ui.h"

class RootUI
{
public:
    RootUI();
    ~RootUI();

    std::vector<BaseUI *> m_uiList;
    std::vector<BaseUI *> m_overlayList;
    bool m_hidden;

    void render(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUTexture *swapchainTexture);
    void add(BaseUI *ui);
    void addOverlay(BaseUI *ui);
};
