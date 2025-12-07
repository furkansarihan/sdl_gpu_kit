#pragma once
#include <SDL3/SDL.h>
#include <vector>

// Base class for input listeners
class InputListener
{
public:
    virtual ~InputListener() = default;
    virtual void onKeyPressed(SDL_Scancode key) {};
    virtual void onKeyReleased(SDL_Scancode key) {};
    virtual void onMouseButtonPressed(Uint8 button, int x, int y) {};
    virtual void onMouseButtonReleased(Uint8 button, int x, int y) {};
    virtual void onMouseMoved(int x, int y, int dx, int dy) {};
    virtual void onMouseWheel(int dx, int dy) {};
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

private:
    // Private constructor for singleton
    InputManager() = default;
    ~InputManager() = default;

    std::vector<InputListener *> listeners;
    const bool *keyboardState = nullptr;
    int mouseX = 0;
    int mouseY = 0;

    void notifyKeyPressed(SDL_Scancode key);
    void notifyKeyReleased(SDL_Scancode key);
    void notifyMouseButtonPressed(Uint8 button, int x, int y);
    void notifyMouseButtonReleased(Uint8 button, int x, int y);
    void notifyMouseMoved(int x, int y, int dx, int dy);
    void notifyMouseWheel(int dx, int dy);
};
