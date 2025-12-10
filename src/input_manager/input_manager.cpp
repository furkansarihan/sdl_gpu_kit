#include "input_manager.h"
#include <SDL3/SDL_scancode.h>
#include <algorithm>

InputManager::~InputManager()
{
    // Close all gamepads
    for (auto &pair : gamepads)
    {
        if (pair.second)
        {
            SDL_CloseGamepad(pair.second);
        }
    }
    gamepads.clear();
}

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

    case SDL_EVENT_GAMEPAD_ADDED:
        openGamepad(event.gdevice.which);
        notifyGamepadConnected(event.gdevice.which);
        break;

    case SDL_EVENT_GAMEPAD_REMOVED:
        notifyGamepadDisconnected(event.gdevice.which);
        closeGamepad(event.gdevice.which);
        break;

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        notifyGamepadButtonPressed(event.gbutton.which, event.gbutton.button);
        break;

    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        notifyGamepadButtonReleased(event.gbutton.which, event.gbutton.button);
        break;

    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        notifyGamepadAxisMoved(event.gaxis.which, event.gaxis.axis, event.gaxis.value);
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

bool InputManager::isGamepadConnected(SDL_JoystickID id) const
{
    return gamepads.find(id) != gamepads.end();
}

bool InputManager::isGamepadButtonDown(SDL_JoystickID id, Uint8 button) const
{
    auto it = gamepads.find(id);
    if (it != gamepads.end() && it->second)
    {
        return SDL_GetGamepadButton(it->second, static_cast<SDL_GamepadButton>(button));
    }
    return false;
}

Sint16 InputManager::getGamepadAxis(SDL_JoystickID id, Uint8 axis) const
{
    auto it = gamepads.find(id);
    if (it != gamepads.end() && it->second)
    {
        return SDL_GetGamepadAxis(it->second, static_cast<SDL_GamepadAxis>(axis));
    }
    return 0;
}

const std::vector<SDL_JoystickID> &InputManager::getConnectedGamepads() const
{
    return connectedGamepadIds;
}

void InputManager::openGamepad(SDL_JoystickID id)
{
    if (gamepads.find(id) == gamepads.end())
    {
        SDL_Gamepad *gamepad = SDL_OpenGamepad(id);
        if (gamepad)
        {
            gamepads[id] = gamepad;
            connectedGamepadIds.push_back(id);
        }
    }
}

void InputManager::closeGamepad(SDL_JoystickID id)
{
    auto it = gamepads.find(id);
    if (it != gamepads.end())
    {
        if (it->second)
        {
            SDL_CloseGamepad(it->second);
        }
        gamepads.erase(it);

        auto idIt = std::find(connectedGamepadIds.begin(), connectedGamepadIds.end(), id);
        if (idIt != connectedGamepadIds.end())
        {
            connectedGamepadIds.erase(idIt);
        }
    }
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

void InputManager::notifyGamepadConnected(SDL_JoystickID id)
{
    for (auto *listener : listeners)
    {
        listener->onGamepadConnected(id);
    }
}

void InputManager::notifyGamepadDisconnected(SDL_JoystickID id)
{
    for (auto *listener : listeners)
    {
        listener->onGamepadDisconnected(id);
    }
}

void InputManager::notifyGamepadButtonPressed(SDL_JoystickID id, Uint8 button)
{
    for (auto *listener : listeners)
    {
        listener->onGamepadButtonPressed(id, button);
    }
}

void InputManager::notifyGamepadButtonReleased(SDL_JoystickID id, Uint8 button)
{
    for (auto *listener : listeners)
    {
        listener->onGamepadButtonReleased(id, button);
    }
}

void InputManager::notifyGamepadAxisMoved(SDL_JoystickID id, Uint8 axis, Sint16 value)
{
    for (auto *listener : listeners)
    {
        listener->onGamepadAxisMoved(id, axis, value);
    }
}
