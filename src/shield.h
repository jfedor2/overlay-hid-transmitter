// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <windows.h>

// An invisible window covering the whole screen that sits between the cursor
// and the apps below, so they don't get mouse movement (WM_MOUSEMOVE, hover).
// It can be made click-through briefly so clicks still reach them. Created
// and driven by the InputEngine thread, so its mouse hook can switch it
// before Windows routes a click.
class MouseShield {
public:
    bool create(HINSTANCE instance);
    void destroy();

    void set_visible(bool visible);
    bool visible() const { return visible_; }
    // Click-through while a click passes to the window below.
    void set_passthrough(bool passthrough);
    bool passthrough() const { return passthrough_; }
    // Leaves a hole over this rect (our main window), or none if empty.
    void set_hole(const RECT& hole);

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    void fit_to_virtual_screen();
    void apply_region();

    HWND hwnd_ = nullptr;
    bool visible_ = false;
    bool passthrough_ = false;
    RECT bounds_{};
    RECT hole_{};
};
