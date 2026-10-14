// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>

#include <map>
#include <vector>

#include "model.h"

struct App;

// A single borderless, per-pixel-alpha layered window covering the whole
// virtual screen. Regions are drawn into it with Direct2D.
//
// In run mode the window is fully click-through (WS_EX_TRANSPARENT) and the
// InputEngine's mouse hook decides which clicks to take. In edit mode the
// window takes the mouse itself (a faint tint makes every pixel hit-testable)
// so regions can be drawn, moved and resized directly on screen.
class Overlay {
public:
    static constexpr UINT WM_APP_ENGINE_STATE = WM_APP + 1;

    bool create(HINSTANCE instance, App* app);
    void destroy();

    HWND hwnd() const { return hwnd_; }
    void set_mode(bool visible, bool editing);
    bool visible() const { return visible_; }
    bool dragging() const { return drag_.kind != DragKind::None; }
    void invalidate() { dirty_ = true; }
    void render_if_dirty();

private:
    enum class DragKind {
        None,
        Move,
        Resize,
        Create
    };
    enum Edge {
        EDGE_LEFT = 1,
        EDGE_RIGHT = 2,
        EDGE_TOP = 4,
        EDGE_BOTTOM = 8
    };
    // What a region looked like when last drawn; used to find what changed.
    struct DrawnRegion {
        Region region;
        bool on = false;
        bool selected = false;
        float value_x = 0.0f;  // live output of AnalogStick regions
        float value_y = 0.0f;
        float dwell = 0.0f;  // hover delay progress while waiting to activate

        bool operator==(const DrawnRegion& o) const {
            return region == o.region && on == o.on && selected == o.selected && value_x == o.value_x &&
                   value_y == o.value_y && dwell == o.dwell;
        }
    };
    struct Drag {
        DragKind kind = DragKind::None;
        int index = -1;
        int edges = 0;
        POINT start{};
        RECT start_rect{};
    };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam);

    void fit_to_virtual_screen();
    bool ensure_surface();
    void release_surface();
    void render();
    void draw_region(int index, const DrawnRegion& d, float opacity, float text_alpha);
    RECT region_paint_rect(const Region& r) const;
    IDWriteTextFormat* text_format(float size, bool bold);

    int hit_test(POINT screen_pt, int* edges) const;
    int edge_margin() const;
    POINT client_to_screen(LPARAM lparam) const;
    void on_mouse_down(POINT pt);
    void on_mouse_move(POINT pt);
    void on_mouse_up();

    App* app_ = nullptr;
    HWND hwnd_ = nullptr;
    bool visible_ = false;
    bool editing_ = false;
    bool dirty_ = true;        // something may have changed, diff on next render
    bool full_redraw_ = true;  // redraw the whole surface
    std::vector<DrawnRegion> last_drawn_;
    float last_opacity_ = -1.0f;
    float last_active_opacity_ = -1.0f;

    RECT bounds_{};  // virtual screen rect
    HDC mem_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
    int surface_w_ = 0;
    int surface_h_ = 0;

    ID2D1Factory* d2d_factory_ = nullptr;
    IDWriteFactory* dwrite_factory_ = nullptr;
    ID2D1DCRenderTarget* target_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    std::map<int, IDWriteTextFormat*> text_formats_;

    Drag drag_;
};
