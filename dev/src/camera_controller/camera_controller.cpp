#include "camera_controller.h"

#include <glm/gtx/norm.hpp>

#include "default_runner.h"
#include "ui/root_ui.h"
#include "utils/utils.h"

void CameraController::onKeyPressed(SDL_Scancode key)
{
    if (key == SDL_SCANCODE_ESCAPE)
    {
        m_relativeMouseEnabled = !m_relativeMouseEnabled;
        SDL_SetWindowRelativeMouseMode(Utils::window, m_relativeMouseEnabled);
    }
    else if (key == SDL_SCANCODE_H)
    {
        m_uiHidden = !m_uiHidden;
        DefaultRunner::m_rootUI->m_hidden = m_uiHidden;
    }
}

void CameraController::onMouseMoved(int x, int y, int dx, int dy)
{
    if (!InputManager::getInstance().isMouseButtonDown(SDL_BUTTON_RIGHT))
        return;

    const float xoffset = dx * m_sensitivity;
    const float yoffset = -dy * m_sensitivity;

    m_camera->yaw += xoffset;
    m_camera->pitch += yoffset;

    // Clamp pitch to avoid flipping
    if (m_camera->pitch > m_maxPitch)
        m_camera->pitch = m_maxPitch;
    if (m_camera->pitch < -m_maxPitch)
        m_camera->pitch = -m_maxPitch;

    // Recalculate front vector from yaw/pitch
    glm::vec3 direction;
    direction.x = cos(glm::radians(m_camera->yaw)) * cos(glm::radians(m_camera->pitch));
    direction.y = sin(glm::radians(m_camera->pitch));
    direction.z = sin(glm::radians(m_camera->yaw)) * cos(glm::radians(m_camera->pitch));

    m_camera->front = glm::normalize(direction);

    glm::vec3 worldUp(0.f, 1.f, 0.f);
    m_camera->right = glm::normalize(glm::cross(m_camera->front, worldUp));
    m_camera->up = glm::normalize(glm::cross(m_camera->right, m_camera->front));
}

void CameraController::update(float deltaTime)
{
    float velocity = m_baseSpeed * deltaTime;

    if (InputManager::getInstance().isKeyDown(SDL_SCANCODE_SPACE))
    {
        velocity *= m_sprintMultiplier;
    }
    else if (InputManager::getInstance().isKeyDown(SDL_SCANCODE_LSHIFT) ||
             InputManager::getInstance().isKeyDown(SDL_SCANCODE_RSHIFT))
    {
        velocity *= m_slowMultiplier;
    }

    glm::vec3 move(0.f);

    if (InputManager::getInstance().isKeyDown(SDL_SCANCODE_W))
        move += m_camera->front;
    if (InputManager::getInstance().isKeyDown(SDL_SCANCODE_S))
        move -= m_camera->front;

    if (InputManager::getInstance().isKeyDown(SDL_SCANCODE_A))
        move -= m_camera->right;
    if (InputManager::getInstance().isKeyDown(SDL_SCANCODE_D))
        move += m_camera->right;

    if (InputManager::getInstance().isKeyDown(SDL_SCANCODE_E))
        move += m_camera->up;
    if (InputManager::getInstance().isKeyDown(SDL_SCANCODE_Q))
        move -= m_camera->up;

    if (glm::length2(move) > 0.f)
    {
        move = glm::normalize(move);
        m_camera->position += move * velocity;
    }
}
