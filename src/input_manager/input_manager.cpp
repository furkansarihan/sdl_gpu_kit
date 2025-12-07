#include "input_manager.h"
#include <SDL3/SDL_scancode.h>

void InputManager::addListener(InputListener *listener)
{
    if (listener)
    {
        listeners.push_back(listener);
    }
}

void InputManager::removeListener(InputListener *listener)
{
    auto it = std::find(listeners.begin(), listeners.end(), listener);
    if (it != listeners.end())
    {
        listeners.erase(it);
    }
}

void InputManager::clearListeners()
{
    listeners.clear();
}

void InputManager::processEvent(const SDL_Event &event)
{
    keyboardState = SDL_GetKeyboardState(nullptr);

    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
        if (!event.key.repeat)
        {
            notifyKeyPressed(event.key.scancode);
        }
        break;

    case SDL_EVENT_KEY_UP:
        notifyKeyReleased(event.key.scancode);
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        mouseX = static_cast<int>(event.button.x);
        mouseY = static_cast<int>(event.button.y);
        notifyMouseButtonPressed(event.button.button, mouseX, mouseY);
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        mouseX = static_cast<int>(event.button.x);
        mouseY = static_cast<int>(event.button.y);
        notifyMouseButtonReleased(event.button.button, mouseX, mouseY);
        break;

    case SDL_EVENT_MOUSE_MOTION:
        mouseX = static_cast<int>(event.motion.x);
        mouseY = static_cast<int>(event.motion.y);
        notifyMouseMoved(mouseX, mouseY,
                         static_cast<int>(event.motion.xrel),
                         static_cast<int>(event.motion.yrel));
        break;

    case SDL_EVENT_MOUSE_WHEEL:
        notifyMouseWheel(static_cast<int>(event.wheel.x),
                         static_cast<int>(event.wheel.y));
        break;
    }
}

bool InputManager::isKeyDown(SDL_Scancode key) const
{
    return keyboardState && keyboardState[key];
}

bool InputManager::isMouseButtonDown(Uint8 button) const
{
    SDL_MouseButtonFlags mouseState = SDL_GetMouseState(nullptr, nullptr);
    return (mouseState & SDL_BUTTON_MASK(button)) != 0;
}

void InputManager::getMousePosition(int &x, int &y) const
{
    x = mouseX;
    y = mouseY;
}

void InputManager::notifyKeyPressed(SDL_Scancode key)
{
    for (auto *listener : listeners)
    {
        listener->onKeyPressed(key);
    }
}

void InputManager::notifyKeyReleased(SDL_Scancode key)
{
    for (auto *listener : listeners)
    {
        listener->onKeyReleased(key);
    }
}

void InputManager::notifyMouseButtonPressed(Uint8 button, int x, int y)
{
    for (auto *listener : listeners)
    {
        listener->onMouseButtonPressed(button, x, y);
    }
}

void InputManager::notifyMouseButtonReleased(Uint8 button, int x, int y)
{
    for (auto *listener : listeners)
    {
        listener->onMouseButtonReleased(button, x, y);
    }
}

void InputManager::notifyMouseMoved(int x, int y, int dx, int dy)
{
    for (auto *listener : listeners)
    {
        listener->onMouseMoved(x, y, dx, dy);
    }
}

void InputManager::notifyMouseWheel(int dx, int dy)
{
    for (auto *listener : listeners)
    {
        listener->onMouseWheel(dx, dy);
    }
}
