#pragma once

#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

// Base class for input listeners
class InputListener
{
public:
    virtual ~InputListener() = default;
    virtual void onKeyPressed(SDL_Scancode key) {};
    virtual void onKeyReleased(SDL_Scancode key) {};
    virtual void onMouseButtonPressed(Uint8 button, int x, int y) {};
    virtual void onMouseButtonReleased(Uint8 button, int x, int y) {};
    virtual void onMouseMoved(int x, int y, float dx, float dy) {};
    virtual void onMouseWheel(float dx, float dy) {};

    // Gamepad events
    virtual void onGamepadConnected(SDL_JoystickID id) {};
    virtual void onGamepadDisconnected(SDL_JoystickID id) {};
    virtual void onGamepadButtonPressed(SDL_JoystickID id, Uint8 button) {};
    virtual void onGamepadButtonReleased(SDL_JoystickID id, Uint8 button) {};
    virtual void onGamepadAxisMoved(SDL_JoystickID id, Uint8 axis, Sint16 value) {};
};

class InputManager
{
public:
    // Get singleton instance
    static InputManager &getInstance()
    {
        static InputManager instance;
        return instance;
    }

    // Delete copy constructor and assignment operator
    InputManager(const InputManager &) = delete;
    InputManager &operator=(const InputManager &) = delete;

    // Register/unregister listeners
    void addListener(InputListener *listener);
    void removeListener(InputListener *listener);
    void clearListeners();

    // Process SDL events - call this in your main loop
    void processEvent(const SDL_Event &event);

    // Query current state of any key
    bool isKeyDown(SDL_Scancode key) const;
    bool isMouseButtonDown(Uint8 button) const;
    void getMousePosition(int &x, int &y) const;

    // Gamepad queries
    bool isGamepadConnected(SDL_JoystickID id) const;
    bool isGamepadButtonDown(SDL_JoystickID id, Uint8 button) const;
    Sint16 getGamepadAxis(SDL_JoystickID id, Uint8 axis) const;
    const std::vector<SDL_JoystickID> &getConnectedGamepads() const;

private:
    // Private constructor for singleton
    InputManager() = default;
    ~InputManager();

    std::vector<InputListener *> listeners;
    const bool *keyboardState = nullptr;
    int mouseX = 0;
    int mouseY = 0;

    // Gamepad tracking
    std::unordered_map<SDL_JoystickID, SDL_Gamepad *> gamepads;
    std::vector<SDL_JoystickID> connectedGamepadIds;

    void openGamepad(SDL_JoystickID id);
    void closeGamepad(SDL_JoystickID id);

    void notifyKeyPressed(SDL_Scancode key);
    void notifyKeyReleased(SDL_Scancode key);
    void notifyMouseButtonPressed(Uint8 button, int x, int y);
    void notifyMouseButtonReleased(Uint8 button, int x, int y);
    void notifyMouseMoved(int x, int y, float dx, float dy);
    void notifyMouseWheel(float dx, float dy);

    void notifyGamepadConnected(SDL_JoystickID id);
    void notifyGamepadDisconnected(SDL_JoystickID id);
    void notifyGamepadButtonPressed(SDL_JoystickID id, Uint8 button);
    void notifyGamepadButtonReleased(SDL_JoystickID id, Uint8 button);
    void notifyGamepadAxisMoved(SDL_JoystickID id, Uint8 axis, Sint16 value);
};
