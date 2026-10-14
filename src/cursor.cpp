// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "cursor.h"

#include <d2d1.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

static const wchar_t* CURSOR_CLASS = L"OverlayHidTransmitterCursor";

template <typename T>
static void safe_release(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

bool is_blank_cursor(HCURSOR cursor) {
    ICONINFO ii;
    if (cursor == nullptr || !GetIconInfo(cursor, &ii)) {
        return false;
    }
    BITMAP bm = {};
    GetObjectW(ii.hbmMask, sizeof(bm), &bm);
    // Monochrome cursors have no color bitmap; their mask is the AND mask
    // stacked on top of the XOR mask.
    const int w = bm.bmWidth;
    const int h = ii.hbmColor != nullptr ? bm.bmHeight : bm.bmHeight / 2;
    bool blank = w > 0 && h > 0;

    if (blank) {
        HDC dc = GetDC(nullptr);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -bm.bmHeight;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint32_t> mask((size_t) w * bm.bmHeight);
        std::vector<uint32_t> color;
        bool has_alpha = false;
        blank = GetDIBits(dc, ii.hbmMask, 0, bm.bmHeight, mask.data(), &bi, DIB_RGB_COLORS) != 0;
        if (blank && ii.hbmColor != nullptr) {
            color.resize((size_t) w * h);
            bi.bmiHeader.biHeight = -h;
            blank = GetDIBits(dc, ii.hbmColor, 0, h, color.data(), &bi, DIB_RGB_COLORS) != 0;
            has_alpha = std::any_of(color.begin(), color.end(), [](uint32_t px) { return (px >> 24) != 0; });
        }
        ReleaseDC(nullptr, dc);

        for (int i = 0; blank && i < w * h; i++) {
            if (has_alpha) {
                blank = (color[i] >> 24) == 0;
            } else {
                // Classic masks: a pixel shows unless AND = 1 (keep screen) and XOR = 0.
                const bool and_bit = (mask[i] & 0xFFFFFF) != 0;
                const uint32_t xor_px = ii.hbmColor != nullptr ? color[i] : mask[(size_t) w * h + i];
                blank = and_bit && (xor_px & 0xFFFFFF) == 0;
            }
        }
    }

    DeleteObject(ii.hbmMask);
    if (ii.hbmColor != nullptr) {
        DeleteObject(ii.hbmColor);
    }
    return blank;
}

bool CursorWindow::create(HINSTANCE instance) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = CURSOR_CLASS;
    RegisterClassExW(&wc);

    const float s = GetDpiForSystem() / 96.0f;
    hotspot_ = (int) std::ceil(2 * s);  // room for the outline around the tip
    size_w_ = (int) std::ceil(12 * s) + 2 * hotspot_;
    size_h_ = (int) std::ceil(20 * s) + 2 * hotspot_;

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        CURSOR_CLASS, L"Overlay HID Transmitter cursor", WS_POPUP,
        0, 0, size_w_, size_h_, nullptr, nullptr, instance, nullptr);
    if (hwnd_ == nullptr) {
        return false;
    }
    if (!render()) {
        destroy();
        return false;
    }
    return true;
}

void CursorWindow::destroy() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    visible_ = false;
}

// Draws a white arrow with a black outline into the layered window, once.
bool CursorWindow::render() {
    ID2D1Factory* factory = nullptr;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory))) {
        return false;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = size_w_;
    bmi.bmiHeader.biHeight = -size_h_;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem_dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old_bitmap = dib ? SelectObject(mem_dc, dib) : nullptr;

    bool ok = false;
    ID2D1DCRenderTarget* target = nullptr;
    ID2D1PathGeometry* arrow = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    RECT rc = { 0, 0, size_w_, size_h_ };
    if (dib && SUCCEEDED(factory->CreateDCRenderTarget(&props, &target)) && SUCCEEDED(target->BindDC(mem_dc, &rc)) &&
        SUCCEEDED(factory->CreatePathGeometry(&arrow)) &&
        SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush))) {
        // Classic arrow outline, tip at the origin, in 96 DPI units.
        const float s = GetDpiForSystem() / 96.0f;
        const float o = (float) hotspot_;
        const D2D1_POINT_2F pts[] = {
            { 0, 0 }, { 0, 16 }, { 4, 12.5f }, { 7, 19 }, { 9.5f, 18 }, { 6.5f, 11.5f }, { 11.5f, 11.5f },
        };
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(arrow->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(o + pts[0].x * s, o + pts[0].y * s), D2D1_FIGURE_BEGIN_FILLED);
            for (size_t i = 1; i < sizeof(pts) / sizeof(pts[0]); i++) {
                sink->AddLine(D2D1::Point2F(o + pts[i].x * s, o + pts[i].y * s));
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            safe_release(sink);

            target->BeginDraw();
            target->Clear(D2D1::ColorF(0, 0, 0, 0));
            brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
            target->FillGeometry(arrow, brush);
            brush->SetColor(D2D1::ColorF(0, 0, 0, 1));
            target->DrawGeometry(arrow, brush, std::max(1.0f, 1.2f * s));
            ok = SUCCEEDED(target->EndDraw());
        }
    }

    if (ok) {
        POINT dst = { 0, 0 };
        SIZE size = { size_w_, size_h_ };
        POINT src = { 0, 0 };
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        ok = UpdateLayeredWindow(hwnd_, nullptr, &dst, &size, mem_dc, &src, 0, &blend, ULW_ALPHA) != 0;
    }

    safe_release(brush);
    safe_release(arrow);
    safe_release(target);
    if (old_bitmap != nullptr) {
        SelectObject(mem_dc, old_bitmap);
    }
    if (dib != nullptr) {
        DeleteObject(dib);
    }
    DeleteDC(mem_dc);
    safe_release(factory);
    return ok;
}

void CursorWindow::move_to(POINT pt) {
    if (hwnd_ == nullptr || (pt.x == pos_.x && pt.y == pos_.y)) {
        return;
    }
    pos_ = pt;
    SetWindowPos(hwnd_, nullptr, pt.x - hotspot_, pt.y - hotspot_, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void CursorWindow::set_visible(bool visible) {
    if (hwnd_ == nullptr || visible == visible_) {
        return;
    }
    visible_ = visible;
    if (visible) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(hwnd_, SW_HIDE);
    }
}
