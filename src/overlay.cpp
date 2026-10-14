// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "overlay.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "app.h"

static const wchar_t* OVERLAY_CLASS = L"OverlayHidTransmitterOverlay";
static constexpr int MIN_REGION_SIZE = 10;
static constexpr int MIN_CREATE_SIZE = 8;

template <typename T>
static void safe_release(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

static std::wstring widen(const char* s) {
    std::wstring out;
    while (*s) {
        out.push_back((wchar_t) (unsigned char) *s++);
    }
    return out;
}

static float system_scale() {
    return GetDpiForSystem() / 96.0f;
}

bool Overlay::create(HINSTANCE instance, App* app) {
    app_ = app;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_))) {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**) &dwrite_factory_))) {
        return false;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = OVERLAY_CLASS;
    RegisterClassExW(&wc);

    // Not owned by the main window, so minimizing the main window keeps it visible.
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        OVERLAY_CLASS, L"Overlay HID Transmitter regions", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return false;
    }
    fit_to_virtual_screen();
    return true;
}

void Overlay::destroy() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    for (auto& [key, fmt] : text_formats_) {
        fmt->Release();
    }
    text_formats_.clear();
    safe_release(brush_);
    safe_release(target_);
    release_surface();
    safe_release(dwrite_factory_);
    safe_release(d2d_factory_);
}

void Overlay::set_mode(bool visible, bool editing) {
    if (editing != editing_) {
        editing_ = editing;
        if (dragging()) {
            on_mouse_up();
        }
        LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        if (editing) {
            ex &= ~WS_EX_TRANSPARENT;
        } else {
            ex |= WS_EX_TRANSPARENT;
        }
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
        dirty_ = true;
        full_redraw_ = true;
    }
    if (visible != visible_) {
        visible_ = visible;
        if (visible) {
            render();
            dirty_ = false;
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        } else {
            ShowWindow(hwnd_, SW_HIDE);
        }
    }
}

void Overlay::render_if_dirty() {
    if (dirty_ && visible_) {
        dirty_ = false;
        render();
    }
}

void Overlay::fit_to_virtual_screen() {
    bounds_.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    bounds_.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    bounds_.right = bounds_.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    bounds_.bottom = bounds_.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    SetWindowPos(hwnd_, HWND_TOPMOST, bounds_.left, bounds_.top,
        bounds_.right - bounds_.left, bounds_.bottom - bounds_.top, SWP_NOACTIVATE);
    dirty_ = true;
    full_redraw_ = true;
}

bool Overlay::ensure_surface() {
    int w = bounds_.right - bounds_.left;
    int h = bounds_.bottom - bounds_.top;
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (dib_ == nullptr || w != surface_w_ || h != surface_h_) {
        release_surface();
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        mem_dc_ = CreateCompatibleDC(nullptr);
        dib_ = CreateDIBSection(mem_dc_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib_ == nullptr) {
            release_surface();
            return false;
        }
        old_bitmap_ = SelectObject(mem_dc_, dib_);
        full_redraw_ = true;
        surface_w_ = w;
        surface_h_ = h;
    }
    if (target_ == nullptr) {
        full_redraw_ = true;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2d_factory_->CreateDCRenderTarget(&props, &target_))) {
            return false;
        }
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_))) {
            safe_release(target_);
            return false;
        }
    }
    return true;
}

void Overlay::release_surface() {
    if (mem_dc_ != nullptr) {
        if (old_bitmap_ != nullptr) {
            SelectObject(mem_dc_, old_bitmap_);
            old_bitmap_ = nullptr;
        }
        DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
    }
    if (dib_ != nullptr) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    surface_w_ = surface_h_ = 0;
}

IDWriteTextFormat* Overlay::text_format(float size, bool bold) {
    int px = std::max(6, (int) (size + 0.5f));
    int key = px * 2 + (bold ? 1 : 0);
    auto it = text_formats_.find(key);
    if (it != text_formats_.end()) {
        return it->second;
    }
    if (text_formats_.size() > 128) {
        for (auto& [k, fmt] : text_formats_) {
            fmt->Release();
        }
        text_formats_.clear();
    }
    IDWriteTextFormat* fmt = nullptr;
    if (FAILED(dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, (float) px, L"en-us", &fmt))) {
        return nullptr;
    }
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    text_formats_[key] = fmt;
    return fmt;
}

// Screen area (in surface coordinates) a region can paint to, including the
// selection outline and handles.
RECT Overlay::region_paint_rect(const Region& r) const {
    const int m = (int) std::ceil(6 * system_scale()) + 2;
    return RECT{ r.x - bounds_.left - m, r.y - bounds_.top - m, r.x - bounds_.left + r.w + m, r.y - bounds_.top + r.h + m };
}

// Redraws only what changed since the last frame: a full 4K surface takes
// ~16 ms to render and push, a single region well under 1 ms.
void Overlay::render() {
    if (!ensure_surface()) {
        return;
    }

    const Config& cfg = app_->config;
    // Keep regions visible enough to work with while editing.
    const float opacity = editing_ ? std::max(cfg.opacity, 0.35f) : cfg.opacity;
    const float text_alpha = editing_ ? 1.0f : std::min(1.0f, cfg.opacity * 3.0f);

    std::vector<InputEngine::ActiveRegion> active;
    if (!editing_) {
        app_->engine.get_active_regions(active);
    }
    std::vector<DrawnRegion> drawn(cfg.regions.size());
    for (size_t i = 0; i < cfg.regions.size(); i++) {
        drawn[i].region = cfg.regions[i];
        drawn[i].selected = editing_ && (int) i == app_->selected;
        for (const auto& a : active) {
            if (a.id == cfg.regions[i].id) {
                drawn[i].on = a.on;
                drawn[i].dwell = a.dwell;
                drawn[i].value_x = a.x;
                drawn[i].value_y = a.y;
                break;
            }
        }
    }

    if (cfg.opacity != last_opacity_ || cfg.active_opacity != last_active_opacity_) {
        full_redraw_ = true;
    }

    const RECT surface = { 0, 0, surface_w_, surface_h_ };
    RECT dirty = {};
    if (full_redraw_) {
        dirty = surface;
    } else {
        auto add = [&](const Region& r) {
            RECT pr = region_paint_rect(r);
            UnionRect(&dirty, &dirty, &pr);
        };
        // Compare by list position, so reordering (e.g. deleting a region,
        // which renumbers the ones after it) is picked up too.
        size_t n = std::max(drawn.size(), last_drawn_.size());
        for (size_t i = 0; i < n; i++) {
            bool have_old = i < last_drawn_.size();
            bool have_new = i < drawn.size();
            if (have_old && have_new && last_drawn_[i] == drawn[i]) {
                continue;
            }
            if (have_old) {
                add(last_drawn_[i].region);
            }
            if (have_new) {
                add(drawn[i].region);
            }
        }
        IntersectRect(&dirty, &dirty, &surface);
    }
    last_drawn_ = drawn;
    last_opacity_ = cfg.opacity;
    last_active_opacity_ = cfg.active_opacity;
    full_redraw_ = false;
    if (IsRectEmpty(&dirty)) {
        return;
    }

    // Render only the dirty part of the DIB.
    if (FAILED(target_->BindDC(mem_dc_, &dirty))) {
        full_redraw_ = true;
        return;
    }
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Translation((float) -dirty.left, (float) -dirty.top));
    target_->Clear(D2D1::ColorF(0, 0, 0, 0));

    const float s = system_scale();
    if (editing_) {
        // Faint tint: shows we're in edit mode and makes every pixel hit-testable.
        brush_->SetColor(D2D1::ColorF(0.05f, 0.10f, 0.25f, 0.22f));
        target_->FillRectangle(D2D1::RectF(0, 0, (float) surface_w_, (float) surface_h_), brush_);
    }

    for (int i = 0; i < (int) drawn.size(); i++) {
        RECT pr = region_paint_rect(drawn[i].region);
        RECT tmp;
        if (IntersectRect(&tmp, &pr, &dirty)) {
            draw_region(i, drawn[i], opacity, text_alpha);
        }
    }

    if (editing_) {
        // Help banner at the top of the primary monitor.
        const wchar_t* help =
            L"EDIT MODE  •  drag on empty space to draw a region  •  drag a region to move it  •  drag its edges to resize";
        if (IDWriteTextFormat* fmt = text_format(15.0f * s, true)) {
            float px = (float) -bounds_.left;
            float py = (float) -bounds_.top;
            float pw = (float) GetSystemMetrics(SM_CXSCREEN);
            float bw = std::min(pw - 20 * s, 900.0f * s);
            D2D1_RECT_F br = D2D1::RectF(px + (pw - bw) / 2, py + 10 * s, px + (pw + bw) / 2, py + 42 * s);
            brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.7f));
            target_->FillRoundedRectangle(D2D1::RoundedRect(br, 6 * s, 6 * s), brush_);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 1));
            target_->DrawText(help, (UINT32) wcslen(help), fmt, br, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        safe_release(brush_);
        safe_release(target_);
        full_redraw_ = true;
        dirty_ = true;
        return;
    }

    POINT dst = { bounds_.left, bounds_.top };
    SIZE size = { surface_w_, surface_h_ };
    POINT src = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UPDATELAYEREDWINDOWINFO info = { sizeof(info) };
    info.pptDst = &dst;
    info.psize = &size;
    info.hdcSrc = mem_dc_;
    info.pptSrc = &src;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = &dirty;
    if (!UpdateLayeredWindowIndirect(hwnd_, &info)) {
        info.prcDirty = nullptr;
        UpdateLayeredWindowIndirect(hwnd_, &info);
    }
}

void Overlay::draw_region(int index, const DrawnRegion& d, float opacity, float text_alpha) {
    const float active_opacity = app_->config.active_opacity;
    const Region& r = d.region;
    if (r.w <= 0 || r.h <= 0) {
        return;
    }
    const float s = system_scale();
    const float x = (float) (r.x - bounds_.left);
    const float y = (float) (r.y - bounds_.top);
    const float w = (float) r.w;
    const float h = (float) r.h;
    const D2D1_RECT_F rect = D2D1::RectF(x, y, x + w, y + h);
    const bool on = d.on;

    Color3 c = r.color();
    if (on) {
        c.r += (1 - c.r) * 0.4f;
        c.g += (1 - c.g) * 0.4f;
        c.b += (1 - c.b) * 0.4f;
    }
    const float radius = std::min(8.0f * s, std::min(w, h) / 4);
    const D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, radius, radius);

    brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b, on ? active_opacity : opacity));
    target_->FillRoundedRectangle(rrect, brush_);

    if (!on && d.dwell > 0) {
        // Waiting out the hover delay: fill up from the bottom in the active
        // color, so it looks pressed by the time it activates.
        const float level = y + h * (1 - d.dwell);
        target_->PushAxisAlignedClip(D2D1::RectF(x, level, x + w, y + h), D2D1_ANTIALIAS_MODE_ALIASED);
        brush_->SetColor(D2D1::ColorF(c.r + (1 - c.r) * 0.4f, c.g + (1 - c.g) * 0.4f, c.b + (1 - c.b) * 0.4f,
            active_opacity * 0.8f));
        target_->FillRoundedRectangle(rrect, brush_);
        target_->PopAxisAlignedClip();
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.8f));
        target_->FillRectangle(D2D1::RectF(x, level - 1.0f * s, x + w, level + 1.0f * s), brush_);
    }

    const float border = (on ? 3.0f : 1.5f) * s;
    const D2D1_ROUNDED_RECT brect = D2D1::RoundedRect(
        D2D1::RectF(x + border / 2, y + border / 2, x + w - border / 2, y + h - border / 2), radius, radius);
    brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b, std::min(1.0f, opacity * 2.5f + (on ? 0.3f : 0))));
    target_->DrawRoundedRectangle(brect, brush_, border);

    // Type-specific decoration, under the label.
    const float cx = x + w / 2;
    const float cy = y + h / 2;
    if (r.type == RegionType::StickPosition) {
        // Arrow pointing in the stick direction.
        float mag = std::sqrt(r.stick_x * r.stick_x + r.stick_y * r.stick_y);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, text_alpha * 0.5f));
        if (mag < 0.01f) {
            float rad = std::min(w, h) * 0.08f;
            target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), brush_, 2.0f * s);
        } else {
            float dx = r.stick_x / mag;
            float dy = r.stick_y / mag;
            float len = std::min(w, h) * 0.42f * std::min(mag, 1.0f);
            float head = std::min(len * 0.45f, std::min(w, h) * 0.15f);
            D2D1_POINT_2F tip = D2D1::Point2F(cx + dx * len, cy + dy * len);
            float stroke = std::max(2.0f * s, std::min(w, h) * 0.035f);
            target_->DrawLine(D2D1::Point2F(cx, cy), tip, brush_, stroke);
            // Head: two strokes at +-35 degrees from the reversed direction.
            const float ca = 0.819f, sa = 0.574f;
            target_->DrawLine(tip, D2D1::Point2F(tip.x - head * (dx * ca - dy * sa), tip.y - head * (dy * ca + dx * sa)), brush_, stroke);
            target_->DrawLine(tip, D2D1::Point2F(tip.x - head * (dx * ca + dy * sa), tip.y - head * (dy * ca - dx * sa)), brush_, stroke);
        }
    } else if (r.type == RegionType::AnalogStick) {
        // Gate (full deflection), inner deadzone, crosshair and live position.
        const float outer = std::max(1.0f - r.outer_deadzone, r.inner_deadzone + 0.01f);
        const float orx = w / 2 * outer, ory = h / 2 * outer;
        const float irx = w / 2 * r.inner_deadzone, iry = h / 2 * r.inner_deadzone;
        brush_->SetColor(D2D1::ColorF(1, 1, 1, text_alpha * 0.18f));
        target_->DrawLine(D2D1::Point2F(x, cy), D2D1::Point2F(x + w, cy), brush_, 1.0f * s);
        target_->DrawLine(D2D1::Point2F(cx, y), D2D1::Point2F(cx, y + h), brush_, 1.0f * s);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, text_alpha * 0.5f));
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), orx, ory), brush_, 1.5f * s);
        if (r.inner_deadzone > 0) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, text_alpha * 0.15f));
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), irx, iry), brush_);
        }
        if (on) {
            D2D1_POINT_2F dot = D2D1::Point2F(cx + d.value_x * orx, cy + d.value_y * ory);
            float dot_r = std::max(4.0f * s, std::min(w, h) * 0.04f);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.7f));
            target_->DrawLine(D2D1::Point2F(cx, cy), dot, brush_, 2.0f * s);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 1));
            target_->FillEllipse(D2D1::Ellipse(dot, dot_r, dot_r), brush_);
        }
    } else if (r.type == RegionType::AnalogTrigger) {
        // Deadzones at both ends and the current travel as a bar growing from
        // the released end.
        auto segment = [&](float a, float b) {  // a..b along the travel, 0 = released
            if (r.reversed) {
                std::swap(a, b);
                a = 1 - a;
                b = 1 - b;
            }
            return r.vertical ? D2D1::RectF(x, y + h - b * h, x + w, y + h - a * h)
                              : D2D1::RectF(x + a * w, y, x + b * w, y + h);
        };
        const float dz = std::clamp(r.trigger_deadzone, 0.0f, 0.45f);
        if (dz > 0) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, text_alpha * 0.12f));
            target_->FillRectangle(segment(0, dz), brush_);
            target_->FillRectangle(segment(1 - dz, 1), brush_);
        }
        // A bar along an edge position t: the released-end mark and the travel edge.
        auto edge_bar = [&](float t, float thickness) {
            D2D1_RECT_F e = segment(t, t);
            return r.vertical ? D2D1::RectF(e.left, e.top - thickness / 2, e.right, e.top + thickness / 2)
                              : D2D1::RectF(e.left - thickness / 2, e.top, e.left + thickness / 2, e.bottom);
        };
        brush_->SetColor(D2D1::ColorF(1, 1, 1, std::min(1.0f, text_alpha * 0.9f)));
        target_->FillRectangle(edge_bar(0, 4.0f * s), brush_);
        if (on) {
            // The output's 0..1 maps to the span between the deadzones, so the
            // bar edge follows the cursor and stops at the deadzone edges while
            // the cursor is inside a deadzone.
            const float pos = dz + d.value_x * (1 - 2 * dz);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.3f));
            target_->FillRectangle(segment(0, pos), brush_);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
            target_->FillRectangle(edge_bar(pos, 3.0f * s), brush_);
        }
    }

    // Label. Analog sticks get a small one at the top, out of the dot's way.
    std::wstring name = widen(r.label());
    const bool has_tag = r.mode != RegionMode::Click;
    float label_h = r.type == RegionType::AnalogStick ? h * 0.28f : (has_tag ? h * 0.75f : h);
    float font = std::min(label_h * 0.5f, w / (0.62f * (float) std::max<size_t>(name.size(), 1)));
    if (r.type == RegionType::AnalogStick) {
        font = std::min(font, 20.0f * s);
    }
    font = std::clamp(font, 8.0f, 72.0f * s);
    if (IDWriteTextFormat* fmt = text_format(font, true)) {
        D2D1_RECT_F lr = D2D1::RectF(x, y, x + w, y + label_h);
        D2D1_RECT_F shadow = D2D1::RectF(lr.left + 1, lr.top + 1, lr.right + 1, lr.bottom + 1);
        brush_->SetColor(D2D1::ColorF(0, 0, 0, text_alpha * 0.6f));
        target_->DrawText(name.c_str(), (UINT32) name.size(), fmt, shadow, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, text_alpha));
        target_->DrawText(name.c_str(), (UINT32) name.size(), fmt, lr, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    if (has_tag) {
        std::wstring tag = widen(REGION_MODE_NAMES[(int) r.mode]);
        float tag_font = std::clamp(std::min(h * 0.18f, w / 5.0f), 7.0f, 18.0f * s);
        if (IDWriteTextFormat* fmt = text_format(tag_font, false)) {
            D2D1_RECT_F tr = r.type == RegionType::AnalogStick
                ? D2D1::RectF(x, y + h * 0.8f, x + w, y + h * 0.97f)
                : D2D1::RectF(x, y + h * 0.62f, x + w, y + h * 0.95f);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, text_alpha * 0.85f));
            target_->DrawText(tag.c_str(), (UINT32) tag.size(), fmt, tr, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    if (editing_) {
        // Index matching the list in the main window.
        std::wstring idx = std::to_wstring(index + 1);
        if (IDWriteTextFormat* fmt = text_format(11.0f * s, false)) {
            D2D1_RECT_F ir = D2D1::RectF(x + 2 * s, y + 2 * s, x + 2 * s + 24 * s, y + 2 * s + 15 * s);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.8f));
            target_->DrawText(idx.c_str(), (UINT32) idx.size(), fmt, ir, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    if (d.selected) {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
        target_->DrawRectangle(D2D1::RectF(x - 1, y - 1, x + w + 1, y + h + 1), brush_, 2.0f * s);
        const float hs = 4.0f * s;
        const float hx[3] = { x, x + w / 2, x + w };
        const float hy[3] = { y, y + h / 2, y + h };
        for (int ix = 0; ix < 3; ix++) {
            for (int iy = 0; iy < 3; iy++) {
                if (ix == 1 && iy == 1) {
                    continue;
                }
                D2D1_RECT_F hr = D2D1::RectF(hx[ix] - hs, hy[iy] - hs, hx[ix] + hs, hy[iy] + hs);
                brush_->SetColor(D2D1::ColorF(1, 1, 1, 1));
                target_->FillRectangle(hr, brush_);
                brush_->SetColor(D2D1::ColorF(0, 0, 0, 1));
                target_->DrawRectangle(hr, brush_, 1.0f);
            }
        }
    }
}

int Overlay::edge_margin() const {
    return (int) (7 * system_scale());
}

// Returns the index of the region under the point (or -1) and which of its
// edges the point is on (0 = inside, i.e. move).
int Overlay::hit_test(POINT pt, int* edges) const {
    const auto& regions = app_->config.regions;
    auto edges_of = [this](const Region& r, POINT p) {
        int mx = std::min(edge_margin(), r.w / 3);
        int my = std::min(edge_margin(), r.h / 3);
        int e = 0;
        if (p.x < r.x + mx) {
            e |= EDGE_LEFT;
        } else if (p.x >= r.x + r.w - mx) {
            e |= EDGE_RIGHT;
        }
        if (p.y < r.y + my) {
            e |= EDGE_TOP;
        } else if (p.y >= r.y + r.h - my) {
            e |= EDGE_BOTTOM;
        }
        return e;
    };

    // The edge grab band extends a few pixels outside the region too, so the
    // corner handles (drawn centered on the corners) can be grabbed anywhere.
    const int outer = edge_margin();
    auto in_outer_band = [outer](const Region& r, POINT p) {
        return p.x >= r.x - outer && p.x < r.x + r.w + outer && p.y >= r.y - outer && p.y < r.y + r.h + outer;
    };

    // Edges of the selected region win, so it can be resized even when it's
    // underneath another region.
    int sel = app_->selected;
    if (sel >= 0 && sel < (int) regions.size() && in_outer_band(regions[sel], pt)) {
        int e = edges_of(regions[sel], pt);
        if (e != 0) {
            *edges = e;
            return sel;
        }
    }
    // Otherwise the topmost (last drawn) region wins: first exact hits...
    for (int i = (int) regions.size() - 1; i >= 0; i--) {
        if (regions[i].contains(pt.x, pt.y)) {
            *edges = edges_of(regions[i], pt);
            return i;
        }
    }
    // ...then the outer edge bands, so they never steal clicks from inside
    // a neighboring region.
    for (int i = (int) regions.size() - 1; i >= 0; i--) {
        if (in_outer_band(regions[i], pt)) {
            *edges = edges_of(regions[i], pt);
            return i;
        }
    }
    *edges = 0;
    return -1;
}

POINT Overlay::client_to_screen(LPARAM lparam) const {
    return POINT{ GET_X_LPARAM(lparam) + bounds_.left, GET_Y_LPARAM(lparam) + bounds_.top };
}

void Overlay::on_mouse_down(POINT pt) {
    int edges = 0;
    int index = hit_test(pt, &edges);
    if (index >= 0) {
        const Region& r = app_->config.regions[index];
        app_->selected = index;
        drag_.kind = edges ? DragKind::Resize : DragKind::Move;
        drag_.index = index;
        drag_.edges = edges;
        drag_.start = pt;
        drag_.start_rect = { r.x, r.y, r.x + r.w, r.y + r.h };
    } else {
        Region r = app_->config.new_region();
        r.x = pt.x;
        r.y = pt.y;
        r.w = 0;
        r.h = 0;
        drag_.kind = DragKind::Create;
        drag_.index = app_->add_region(r);
        drag_.edges = 0;
        drag_.start = pt;
        drag_.start_rect = { pt.x, pt.y, pt.x, pt.y };
    }
    SetCapture(hwnd_);
    dirty_ = true;
}

void Overlay::on_mouse_move(POINT pt) {
    auto& regions = app_->config.regions;
    if (drag_.index < 0 || drag_.index >= (int) regions.size()) {
        return;
    }
    Region& r = regions[drag_.index];
    const RECT& sr = drag_.start_rect;
    const int dx = pt.x - drag_.start.x;
    const int dy = pt.y - drag_.start.y;

    switch (drag_.kind) {
        case DragKind::Move:
            r.x = sr.left + dx;
            r.y = sr.top + dy;
            break;
        case DragKind::Resize: {
            int left = sr.left, top = sr.top, right = sr.right, bottom = sr.bottom;
            if (drag_.edges & EDGE_LEFT) {
                left = std::min(left + dx, right - MIN_REGION_SIZE);
            }
            if (drag_.edges & EDGE_RIGHT) {
                right = std::max(right + dx, left + MIN_REGION_SIZE);
            }
            if (drag_.edges & EDGE_TOP) {
                top = std::min(top + dy, bottom - MIN_REGION_SIZE);
            }
            if (drag_.edges & EDGE_BOTTOM) {
                bottom = std::max(bottom + dy, top + MIN_REGION_SIZE);
            }
            r.x = left;
            r.y = top;
            r.w = right - left;
            r.h = bottom - top;
            break;
        }
        case DragKind::Create:
            r.x = std::min(drag_.start.x, pt.x);
            r.y = std::min(drag_.start.y, pt.y);
            r.w = std::abs(pt.x - drag_.start.x);
            r.h = std::abs(pt.y - drag_.start.y);
            break;
        default:
            return;
    }
    app_->regions_changed();
}

void Overlay::on_mouse_up() {
    Drag drag = drag_;
    drag_ = Drag();
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    auto& regions = app_->config.regions;
    if (drag.kind == DragKind::Create && drag.index >= 0 && drag.index < (int) regions.size()) {
        const Region& r = regions[drag.index];
        if (r.w < MIN_CREATE_SIZE || r.h < MIN_CREATE_SIZE) {
            // Just a click on empty space: deselect instead of creating a tiny region.
            app_->delete_region(drag.index);
            app_->selected = -1;
        } else if (r.w < MIN_REGION_SIZE || r.h < MIN_REGION_SIZE) {
            regions[drag.index].w = std::max(r.w, MIN_REGION_SIZE);
            regions[drag.index].h = std::max(r.h, MIN_REGION_SIZE);
            app_->regions_changed();
        }
    }
    dirty_ = true;
}

LRESULT CALLBACK Overlay::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*) lparam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) cs->lpCreateParams);
    }
    auto* self = (Overlay*) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (self != nullptr && self->hwnd_ == hwnd) {
        return self->handle_message(msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT Overlay::handle_message(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_SETCURSOR: {
            if (!editing_) {
                break;
            }
            int edges = drag_.edges;
            bool over_region = drag_.kind == DragKind::Move || drag_.kind == DragKind::Resize;
            if (drag_.kind == DragKind::None) {
                POINT pt;
                GetCursorPos(&pt);
                over_region = hit_test(pt, &edges) >= 0;
            }
            LPCWSTR cursor = IDC_CROSS;
            if (over_region) {
                if (edges == (EDGE_LEFT | EDGE_TOP) || edges == (EDGE_RIGHT | EDGE_BOTTOM)) {
                    cursor = IDC_SIZENWSE;
                } else if (edges == (EDGE_RIGHT | EDGE_TOP) || edges == (EDGE_LEFT | EDGE_BOTTOM)) {
                    cursor = IDC_SIZENESW;
                } else if (edges & (EDGE_LEFT | EDGE_RIGHT)) {
                    cursor = IDC_SIZEWE;
                } else if (edges & (EDGE_TOP | EDGE_BOTTOM)) {
                    cursor = IDC_SIZENS;
                } else {
                    cursor = IDC_SIZEALL;
                }
            }
            SetCursor(LoadCursorW(nullptr, cursor));
            return TRUE;
        }

        case WM_LBUTTONDOWN:
            if (editing_) {
                on_mouse_down(client_to_screen(lparam));
            }
            return 0;

        case WM_MOUSEMOVE:
            if (editing_ && dragging()) {
                on_mouse_move(client_to_screen(lparam));
            }
            return 0;

        case WM_LBUTTONUP:
            if (dragging()) {
                on_mouse_up();
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (dragging() && (HWND) lparam != hwnd_) {
                on_mouse_up();
            }
            return 0;

        case WM_DISPLAYCHANGE:
            fit_to_virtual_screen();
            return 0;

        case WM_CLOSE:
            // The overlay lives as long as the app; only the main window closes it.
            return 0;

        case WM_DPICHANGED:
            // We always cover the whole virtual screen in physical pixels; ignore the suggested rect.
            return 0;

        case WM_APP_ENGINE_STATE:
            if (!editing_) {
                dirty_ = true;
            }
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}
