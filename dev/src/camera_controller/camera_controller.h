#pragma once

#include "camera.h"
#include "input_manager/input_manager.h"
#include "ui/base_ui.h"
#include "update_manager/update_manager.h"

class CameraController : public BaseUI, public InputListener, public Updatable
{
public:
    explicit CameraController(Camera *camera)
        : m_camera(camera)
    {
    }

    Camera *m_camera = nullptr;

    // Movement / look settings
    float m_baseSpeed = 10.f;
    float m_sprintMultiplier = 4.f;
    float m_slowMultiplier = 0.25f;
    float m_sensitivity = 0.1f;
    float m_maxPitch = 89.f;

    // State
    bool m_relativeMouseEnabled = false;
    bool m_uiHidden = false;

    void renderUI() override;

    void onKeyPressed(SDL_Scancode key) override;
    void onMouseMoved(int x, int y, float dx, float dy) override;

    void update(float deltaTime) override;
};
