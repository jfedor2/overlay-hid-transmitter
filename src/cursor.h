// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <windows.h>

#include <climits>

// Whether a cursor image draws nothing at all. Some apps (Qt, SDL) "hide" the
// cursor by setting a fully transparent one instead of hiding it.
bool is_blank_cursor(HCURSOR cursor);

// A small click-through topmost window showing an arrow cursor, for games
// that hide the system cursor over their window. Created and moved by the
// InputEngine thread, which sees every mouse move first.
class CursorWindow {
public:
    bool create(HINSTANCE instance);
    void destroy();

    HWND hwnd() const { return hwnd_; }
    // Moves the arrow tip to the given screen point.
    void move_to(POINT pt);
    void set_visible(bool visible);

private:
    bool render();

    HWND hwnd_ = nullptr;
    bool visible_ = false;
    POINT pos_{ LONG_MIN, LONG_MIN };
    int size_w_ = 0;
    int size_h_ = 0;
    int hotspot_ = 0;
};
