// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "model.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include <windows.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

const char* const BUTTON_NAMES[BUTTON_COUNT] = {
    "A", "B", "X", "Y",
    "L1", "R1", "L2", "R2", "L3", "R3",
    "Up", "Down", "Left", "Right",
    "Options", "Menu", "Stadia", "Capture", "Assistant",
};

const char* const STICK_NAMES[STICK_COUNT] = {
    "Left stick",
    "Right stick",
};

const char* const STICK_SHORT_NAMES[STICK_COUNT] = {
    "LS",
    "RS",
};

const char* const TRIGGER_NAMES[TRIGGER_COUNT] = {
    "L2",
    "R2",
};

const char* const REGION_TYPE_NAMES[REGION_TYPE_COUNT] = {
    "Button",
    "Stick position",
    "Analog stick",
    "Analog trigger",
};

const char* const REGION_MODE_NAMES[REGION_MODE_COUNT] = {
    "Click",
    "Hover",
    "Toggle",
    "Hover toggle",
};

const char* const REGION_MODE_DESCRIPTIONS[REGION_MODE_COUNT] = {
    "Active while the mouse button is held after clicking in the region.",
    "Active while the cursor is inside the region. Clicks pass through.",
    "A click latches the output, the next click releases it.",
    "Active while the cursor is inside the region.\nA click latches the current output until the next click.",
};

const char* const CURSOR_MODE_NAMES[CURSOR_MODE_COUNT] = {
    "Off",
    "When hidden",
    "Always",
};

const Color3 BUTTON_COLORS[BUTTON_COUNT] = {
    { 0.20f, 0.75f, 0.30f },  // A
    { 0.85f, 0.25f, 0.25f },  // B
    { 0.25f, 0.45f, 0.90f },  // X
    { 0.90f, 0.75f, 0.15f },  // Y
    { 0.65f, 0.35f, 0.85f },  // L1
    { 0.65f, 0.35f, 0.85f },  // R1
    { 0.90f, 0.50f, 0.15f },  // L2
    { 0.90f, 0.50f, 0.15f },  // R2
    { 0.20f, 0.70f, 0.75f },  // L3
    { 0.20f, 0.70f, 0.75f },  // R3
    { 0.50f, 0.55f, 0.65f },  // Up
    { 0.50f, 0.55f, 0.65f },  // Down
    { 0.50f, 0.55f, 0.65f },  // Left
    { 0.50f, 0.55f, 0.65f },  // Right
    { 0.60f, 0.60f, 0.60f },  // Options
    { 0.60f, 0.60f, 0.60f },  // Menu
    { 0.95f, 0.35f, 0.55f },  // Stadia
    { 0.60f, 0.60f, 0.60f },  // Capture
    { 0.60f, 0.60f, 0.60f },  // Assistant
};

const Color3 STICK_COLORS[STICK_COUNT] = {
    { 0.15f, 0.60f, 0.85f },  // Left stick
    { 0.85f, 0.30f, 0.75f },  // Right stick
};

void analog_stick_value(const Region& r, int px, int py, float& out_x, float& out_y) {
    out_x = out_y = 0.0f;
    if (r.w <= 0 || r.h <= 0) {
        return;
    }
    const float half_w = r.w / 2.0f;
    const float half_h = r.h / 2.0f;
    const float cx = std::clamp((float) px + 0.5f, (float) r.x, (float) (r.x + r.w));
    const float cy = std::clamp((float) py + 0.5f, (float) r.y, (float) (r.y + r.h));
    const float nx = (cx - (r.x + half_w)) / half_w;
    const float ny = (cy - (r.y + half_h)) / half_h;
    const float mag = std::sqrt(nx * nx + ny * ny);

    const float inner = std::clamp(r.inner_deadzone, 0.0f, 0.95f);
    const float outer = std::max(1.0f - std::clamp(r.outer_deadzone, 0.0f, 0.95f), inner + 0.01f);
    if (mag <= inner) {
        return;
    }
    const float scaled = std::min(1.0f, (mag - inner) / (outer - inner));
    out_x = nx / mag * scaled;
    out_y = ny / mag * scaled;
}

float analog_trigger_value(const Region& r, int px, int py) {
    if (r.w <= 0 || r.h <= 0) {
        return 0.0f;
    }
    // 0 at the released end (left, or bottom when vertical), 1 at the other.
    float t = r.vertical ? ((float) (r.y + r.h) - ((float) py + 0.5f)) / (float) r.h
                         : (((float) px + 0.5f) - (float) r.x) / (float) r.w;
    t = std::clamp(t, 0.0f, 1.0f);
    if (r.reversed) {
        t = 1.0f - t;
    }
    const float dz = std::clamp(r.trigger_deadzone, 0.0f, 0.45f);
    if (t <= dz) {
        return 0.0f;
    }
    if (t >= 1.0f - dz) {
        return 1.0f;
    }
    return (t - dz) / (1.0f - 2.0f * dz);
}

bool GamepadState::operator==(const GamepadState& o) const {
    if (buttons != o.buttons) {
        return false;
    }
    // Compare as sent, so changes too small to show up in the report don't count.
    for (int s = 0; s < STICK_COUNT; s++) {
        for (int a = 0; a < 2; a++) {
            if (stick_byte(sticks[s][a]) != stick_byte(o.sticks[s][a])) {
                return false;
            }
        }
    }
    for (int t = 0; t < TRIGGER_COUNT; t++) {
        if (trigger_byte(triggers[t]) != trigger_byte(o.triggers[t])) {
            return false;
        }
    }
    return true;
}

template <typename E, int N>
static bool enum_from_name(const std::string& name, const char* const (&names)[N], E& out) {
    for (int i = 0; i < N; i++) {
        if (name == names[i]) {
            out = (E) i;
            return true;
        }
    }
    return false;
}

bool load_config(const std::wstring& path, Config& config) {
    std::ifstream f(path);
    if (!f) {
        return false;
    }
    try {
        json j = json::parse(f);
        config.receiver_ip = j.value("receiver_ip", "");
        config.opacity = j.value("opacity", 0.3f);
        config.active_opacity = j.value("active_opacity", 0.75f);
        config.keep_on_top = j.value("keep_on_top", false);
        enum_from_name(j.value("draw_cursor", ""), CURSOR_MODE_NAMES, config.cursor_mode);
        config.block_mouse = j.value("block_mouse", false);
        enum_from_name(j.value("new_type", ""), REGION_TYPE_NAMES, config.new_type);
        enum_from_name(j.value("new_button", ""), BUTTON_NAMES, config.new_button);
        enum_from_name(j.value("new_stick", ""), STICK_NAMES, config.new_stick);
        enum_from_name(j.value("new_trigger", ""), TRIGGER_NAMES, config.new_trigger);
        enum_from_name(j.value("new_mode", ""), REGION_MODE_NAMES, config.new_mode);
        if (j.contains("window") && j["window"].is_object()) {
            const json& jw = j["window"];
            config.window.x = jw.value("x", 0);
            config.window.y = jw.value("y", 0);
            config.window.w = jw.value("w", 0);
            config.window.h = jw.value("h", 0);
            config.window.maximized = jw.value("maximized", false);
            config.window.valid = config.window.w > 0 && config.window.h > 0;
        }
        config.regions.clear();
        uint32_t next_id = 1;
        for (const auto& jr : j.value("regions", json::array())) {
            Region r;
            r.id = next_id++;
            enum_from_name(jr.value("type", ""), REGION_TYPE_NAMES, r.type);
            enum_from_name(jr.value("button", ""), BUTTON_NAMES, r.button);
            enum_from_name(jr.value("stick", ""), STICK_NAMES, r.stick);
            enum_from_name(jr.value("trigger", ""), TRIGGER_NAMES, r.trigger);
            enum_from_name(jr.value("mode", ""), REGION_MODE_NAMES, r.mode);
            r.stick_x = std::clamp(jr.value("stick_x", 0.0f), -1.0f, 1.0f);
            r.stick_y = std::clamp(jr.value("stick_y", -1.0f), -1.0f, 1.0f);
            r.inner_deadzone = std::clamp(jr.value("inner_deadzone", 0.1f), 0.0f, 0.9f);
            r.outer_deadzone = std::clamp(jr.value("outer_deadzone", 0.1f), 0.0f, 0.9f);
            r.vertical = jr.value("vertical", false);
            r.reversed = jr.value("reversed", false);
            r.trigger_deadzone = std::clamp(jr.value("trigger_deadzone", 0.05f), 0.0f, 0.45f);
            r.hover_delay_ms = std::clamp(jr.value("hover_delay_ms", 0), 0, 60000);
            r.x = jr.value("x", 0);
            r.y = jr.value("y", 0);
            r.w = jr.value("w", 100);
            r.h = jr.value("h", 100);
            config.regions.push_back(r);
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool save_config(const std::wstring& path, const Config& config) {
    json j;
    j["receiver_ip"] = config.receiver_ip;
    j["opacity"] = config.opacity;
    j["active_opacity"] = config.active_opacity;
    j["keep_on_top"] = config.keep_on_top;
    j["draw_cursor"] = CURSOR_MODE_NAMES[(int) config.cursor_mode];
    j["block_mouse"] = config.block_mouse;
    j["new_type"] = REGION_TYPE_NAMES[(int) config.new_type];
    j["new_button"] = BUTTON_NAMES[(int) config.new_button];
    j["new_stick"] = STICK_NAMES[(int) config.new_stick];
    j["new_trigger"] = TRIGGER_NAMES[(int) config.new_trigger];
    j["new_mode"] = REGION_MODE_NAMES[(int) config.new_mode];
    if (config.window.valid) {
        j["window"] = {
            { "x", config.window.x },
            { "y", config.window.y },
            { "w", config.window.w },
            { "h", config.window.h },
            { "maximized", config.window.maximized },
        };
    }
    json regions = json::array();
    for (const Region& r : config.regions) {
        json jr = {
            { "type", REGION_TYPE_NAMES[(int) r.type] },
            { "mode", REGION_MODE_NAMES[(int) r.mode] },
            { "x", r.x },
            { "y", r.y },
            { "w", r.w },
            { "h", r.h },
        };
        if (r.hover_delay_ms > 0) {
            jr["hover_delay_ms"] = r.hover_delay_ms;
        }
        switch (r.type) {
            case RegionType::Button:
                jr["button"] = BUTTON_NAMES[(int) r.button];
                break;
            case RegionType::StickPosition:
                jr["stick"] = STICK_NAMES[(int) r.stick];
                jr["stick_x"] = r.stick_x;
                jr["stick_y"] = r.stick_y;
                break;
            case RegionType::AnalogStick:
                jr["stick"] = STICK_NAMES[(int) r.stick];
                jr["inner_deadzone"] = r.inner_deadzone;
                jr["outer_deadzone"] = r.outer_deadzone;
                break;
            case RegionType::AnalogTrigger:
                jr["trigger"] = TRIGGER_NAMES[(int) r.trigger];
                jr["vertical"] = r.vertical;
                jr["reversed"] = r.reversed;
                jr["trigger_deadzone"] = r.trigger_deadzone;
                break;
            default:
                break;
        }
        regions.push_back(jr);
    }
    j["regions"] = regions;

    std::wstring tmp_path = path + L".tmp";
    {
        std::ofstream f(tmp_path, std::ios::trunc);
        if (!f) {
            return false;
        }
        f << j.dump(2);
        if (!f) {
            return false;
        }
    }
    return MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
