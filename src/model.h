// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Stadia controller buttons, in the order they are presented to the user.
enum class Button : int {
    A,
    B,
    X,
    Y,
    L1,
    R1,
    L2,
    R2,
    L3,
    R3,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Options,
    Menu,
    Stadia,
    Capture,
    Assistant,
    Count
};

constexpr int BUTTON_COUNT = (int) Button::Count;

enum class Stick : int {
    Left,
    Right,
    Count
};

constexpr int STICK_COUNT = (int) Stick::Count;

enum class Trigger : int {
    L2,
    R2,
    Count
};

constexpr int TRIGGER_COUNT = (int) Trigger::Count;

// What a region outputs.
enum class RegionType : int {
    Button,         // a gamepad button
    StickPosition,  // a fixed stick position over the whole region
    AnalogStick,    // stick position follows the cursor within the region
    AnalogTrigger,  // trigger travel follows the cursor along the region
    Count
};

constexpr int REGION_TYPE_COUNT = (int) RegionType::Count;

// When a region is active.
enum class RegionMode : int {
    Click,        // while the mouse button is held after clicking in the region
    Hover,        // while the cursor is inside the region (clicks pass through)
    Toggle,       // a click latches the output, the next click releases it
    HoverToggle,  // like Hover, but a click latches the current output until the next click
    Count
};

constexpr int REGION_MODE_COUNT = (int) RegionMode::Count;

// When to draw our own cursor on top of everything.
enum class CursorMode : int {
    Off,
    WhenHidden,  // only while the system cursor is hidden (as games often do)
    Always,
    Count
};

constexpr int CURSOR_MODE_COUNT = (int) CursorMode::Count;

extern const char* const BUTTON_NAMES[BUTTON_COUNT];
extern const char* const STICK_NAMES[STICK_COUNT];        // "Left stick"
extern const char* const STICK_SHORT_NAMES[STICK_COUNT];  // "LS"
extern const char* const TRIGGER_NAMES[TRIGGER_COUNT];
extern const char* const REGION_TYPE_NAMES[REGION_TYPE_COUNT];
extern const char* const REGION_MODE_NAMES[REGION_MODE_COUNT];
extern const char* const REGION_MODE_DESCRIPTIONS[REGION_MODE_COUNT];
extern const char* const CURSOR_MODE_NAMES[CURSOR_MODE_COUNT];

// Display colors (RGB, 0-1).
struct Color3 {
    float r, g, b;
};
extern const Color3 BUTTON_COLORS[BUTTON_COUNT];
extern const Color3 STICK_COLORS[STICK_COUNT];

inline uint32_t button_mask(Button b) {
    return 1u << (int) b;
}

inline Button trigger_button(Trigger t) {
    return t == Trigger::L2 ? Button::L2 : Button::R2;
}

// Region coordinates are in virtual screen space, physical pixels.
// Stick values are -1..1, with -1 = left/up. Trigger values are 0..1.
struct Region {
    uint32_t id = 0;
    RegionType type = RegionType::Button;
    Button button = Button::A;
    Stick stick = Stick::Left;
    Trigger trigger = Trigger::L2;
    RegionMode mode = RegionMode::Click;
    float stick_x = 0.0f;  // StickPosition
    float stick_y = -1.0f;
    float inner_deadzone = 0.1f;  // AnalogStick, fraction of the radius
    float outer_deadzone = 0.1f;
    bool vertical = false;  // AnalogTrigger: released at the left (or bottom) end
    bool reversed = false;  // AnalogTrigger: released at the right (or top) end instead
    float trigger_deadzone = 0.05f;  // AnalogTrigger, fraction of the length at each end
    int hover_delay_ms = 0;  // Hover, HoverToggle: how long the cursor must stay inside first
    int x = 0;
    int y = 0;
    int w = 100;
    int h = 100;

    bool contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    bool operator==(const Region& o) const {
        return id == o.id && type == o.type && button == o.button && stick == o.stick && trigger == o.trigger &&
               mode == o.mode && stick_x == o.stick_x && stick_y == o.stick_y &&
               inner_deadzone == o.inner_deadzone && outer_deadzone == o.outer_deadzone &&
               vertical == o.vertical && reversed == o.reversed && trigger_deadzone == o.trigger_deadzone &&
               hover_delay_ms == o.hover_delay_ms &&
               x == o.x && y == o.y && w == o.w && h == o.h;
    }
    const Color3& color() const {
        switch (type) {
            case RegionType::Button:
                return BUTTON_COLORS[(int) button];
            case RegionType::AnalogTrigger:
                return BUTTON_COLORS[(int) trigger_button(trigger)];
            default:
                return STICK_COLORS[(int) stick];
        }
    }
    const char* label() const {
        switch (type) {
            case RegionType::Button:
                return BUTTON_NAMES[(int) button];
            case RegionType::AnalogTrigger:
                return TRIGGER_NAMES[(int) trigger];
            default:
                return STICK_SHORT_NAMES[(int) stick];
        }
    }
};

// Stick output of an AnalogStick region for a cursor position. The point is
// clamped to the region, so dragging outside it saturates. The region maps to
// a circular gate: the center is neutral and the ellipse inscribed in the
// region (minus the outer deadzone) is full deflection.
void analog_stick_value(const Region& r, int px, int py, float& out_x, float& out_y);

// Trigger output (0..1) of an AnalogTrigger region for a cursor position,
// clamped to the region. The deadzone snaps both ends: near the released end
// to 0, near the other end to fully pressed.
float analog_trigger_value(const Region& r, int px, int py);

// Main window's restored (non-maximized) rect, in workspace coordinates as
// used by Get/SetWindowPlacement.
struct WindowRect {
    bool valid = false;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool maximized = false;

    bool operator==(const WindowRect& o) const {
        return valid == o.valid && x == o.x && y == o.y && w == o.w && h == o.h && maximized == o.maximized;
    }
};

struct Config {
    std::string receiver_ip;
    WindowRect window;
    float opacity = 0.3f;
    float active_opacity = 0.75f;  // fill of regions while they're active
    bool keep_on_top = false;
    CursorMode cursor_mode = CursorMode::Off;
    bool block_mouse = false;  // keep mouse movement from reaching apps while running
    RegionType new_type = RegionType::Button;
    Button new_button = Button::A;
    Stick new_stick = Stick::Left;
    Trigger new_trigger = Trigger::L2;
    RegionMode new_mode = RegionMode::Click;
    std::vector<Region> regions;

    // A region with the "new region" settings (geometry left at defaults).
    Region new_region() const {
        Region r;
        r.type = new_type;
        r.button = new_button;
        r.stick = new_stick;
        r.trigger = new_trigger;
        r.mode = new_mode;
        return r;
    }
};

// What gets sent to the receiver. Sticks are -1..1, triggers 0..1 (analog
// travel from trigger regions; L2/R2 button regions are separate, in buttons).
struct GamepadState {
    uint32_t buttons = 0;
    float sticks[STICK_COUNT][2] = {};
    float triggers[TRIGGER_COUNT] = {};

    bool operator==(const GamepadState& o) const;
    bool operator!=(const GamepadState& o) const { return !(*this == o); }
};

// Stick value as sent in the report: 0..255, 128 = center.
inline uint8_t stick_byte(float v) {
    int b = 128 + (int) (v * 127.0f + (v >= 0 ? 0.5f : -0.5f));
    return (uint8_t) (b < 1 ? 1 : (b > 255 ? 255 : b));
}

// Trigger value as sent in the report: 0..255.
inline uint8_t trigger_byte(float v) {
    int b = (int) (v * 255.0f + 0.5f);
    return (uint8_t) (b < 0 ? 0 : (b > 255 ? 255 : b));
}

bool load_config(const std::wstring& path, Config& config);
bool save_config(const std::wstring& path, const Config& config);
