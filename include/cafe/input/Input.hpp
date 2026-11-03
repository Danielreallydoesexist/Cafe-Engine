#pragma once

#include "cafe/core/Types.hpp"

#include <array>

namespace cafe {

enum class KeyCode : std::size_t {
    Backspace = 8, Enter = 13, Escape = 27, Space = 32,
    Left = 128, Right, Up, Down
};

struct ButtonState { bool down{false}; bool pressed{false}; bool released{false}; };
struct AnalogStick { double x{0}, y{0}; };
struct Acceleration { double x{0}, y{0}, z{0}; };
struct GamePadState {
    ButtonState A, B, X, Y, L, R, ZL, ZR, plus, minus, up, down, left, right;
    AnalogStick leftStick, rightStick;
};
struct NunchukState {
    AnalogStick stick;
    ButtonState C, Z;
};
struct WiiRemoteState {
    bool connected{false};
    bool pointerValid{false};
    double x{0}, y{0};
    ButtonState A, B, one, two, plus, minus, home, up, down, left, right;
    Acceleration accelerometer;
    NunchukState nunchuk;
};
struct KeyboardState { std::array<ButtonState, 256> keys{}; };
struct MouseState { double x{0}, y{0}; ButtonState left, right; };

inline void holdButton(ButtonState& button, bool held) {
    button.pressed = held && !button.down;
    button.released = !held && button.down;
    button.down = held;
}

inline void clearEdges(ButtonState& button) {
    button.pressed = false;
    button.released = false;
}

class InputSystem {
public:
    GamePadState gamePad;
    WiiRemoteState wiiRemote;
    KeyboardState keyboard;
    MouseState mouse;
    Display pointerDisplay{Display::GamePad};

    void beginFrame() {
        clearEdges(gamePad.A); clearEdges(gamePad.B); clearEdges(gamePad.X); clearEdges(gamePad.Y);
        clearEdges(gamePad.L); clearEdges(gamePad.R); clearEdges(gamePad.ZL); clearEdges(gamePad.ZR);
        clearEdges(gamePad.plus); clearEdges(gamePad.minus);
        clearEdges(gamePad.up); clearEdges(gamePad.down); clearEdges(gamePad.left); clearEdges(gamePad.right);
        clearEdges(wiiRemote.A); clearEdges(wiiRemote.B);
        clearEdges(wiiRemote.one); clearEdges(wiiRemote.two);
        clearEdges(wiiRemote.plus); clearEdges(wiiRemote.minus); clearEdges(wiiRemote.home);
        clearEdges(wiiRemote.up); clearEdges(wiiRemote.down); clearEdges(wiiRemote.left); clearEdges(wiiRemote.right);
        clearEdges(wiiRemote.nunchuk.C); clearEdges(wiiRemote.nunchuk.Z);
        clearEdges(mouse.left); clearEdges(mouse.right);
        for (auto& key : keyboard.keys) clearEdges(key);
    }
};

} // namespace cafe
