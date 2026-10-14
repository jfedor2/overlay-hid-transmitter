// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "shield.h"

static const wchar_t* SHIELD_CLASS = L"OverlayHidTransmitterShield";

bool MouseShield::create(HINSTANCE instance) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = SHIELD_CLASS;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, SHIELD_CLASS,
        L"Overlay HID Transmitter mouse shield", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return false;
    }
    // Practically invisible, but unlike fully transparent pixels it still
    // takes the mouse.
    SetLayeredWindowAttributes(hwnd_, 0, 1, LWA_ALPHA);
    fit_to_virtual_screen();
    return true;
}

void MouseShield::destroy() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    visible_ = false;
    passthrough_ = false;
}

void MouseShield::set_visible(bool visible) {
    if (hwnd_ == nullptr || visible == visible_) {
        return;
    }
    visible_ = visible;
    if (visible) {
        set_passthrough(false);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void MouseShield::set_passthrough(bool passthrough) {
    if (hwnd_ == nullptr || passthrough == passthrough_) {
        return;
    }
    passthrough_ = passthrough;
    if (!visible_) {
        return;
    }
    if (passthrough) {
        ShowWindow(hwnd_, SW_HIDE);
    } else {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void MouseShield::set_hole(const RECT& hole) {
    if (EqualRect(&hole, &hole_)) {
        return;
    }
    hole_ = hole;
    apply_region();
}

void MouseShield::fit_to_virtual_screen() {
    bounds_.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    bounds_.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    bounds_.right = bounds_.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    bounds_.bottom = bounds_.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    SetWindowPos(hwnd_, nullptr, bounds_.left, bounds_.top, bounds_.right - bounds_.left,
        bounds_.bottom - bounds_.top, SWP_NOZORDER | SWP_NOACTIVATE);
    apply_region();
}

void MouseShield::apply_region() {
    if (hwnd_ == nullptr) {
        return;
    }
    if (IsRectEmpty(&hole_)) {
        SetWindowRgn(hwnd_, nullptr, TRUE);
        return;
    }
    // Window regions are in window coordinates.
    HRGN rgn = CreateRectRgn(0, 0, bounds_.right - bounds_.left, bounds_.bottom - bounds_.top);
    HRGN hole = CreateRectRgn(hole_.left - bounds_.left, hole_.top - bounds_.top, hole_.right - bounds_.left,
        hole_.bottom - bounds_.top);
    CombineRgn(rgn, rgn, hole, RGN_DIFF);
    DeleteObject(hole);
    SetWindowRgn(hwnd_, rgn, TRUE);  // the system owns rgn now
}

LRESULT CALLBACK MouseShield::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) ((CREATESTRUCTW*) lparam)->lpCreateParams);
    }
    auto* self = (MouseShield*) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_DISPLAYCHANGE:
            if (self != nullptr && self->hwnd_ == hwnd) {
                self->fit_to_virtual_screen();
            }
            return 0;
        case WM_DPICHANGED:
            return 0;  // always covers the whole virtual screen
        case WM_CLOSE:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
