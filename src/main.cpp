// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <winsock2.h>
#include <windows.h>

#include <d3d11.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "app.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_device_context = nullptr;
static IDXGISwapChain* g_swap_chain = nullptr;
static ID3D11RenderTargetView* g_render_target_view = nullptr;
static bool g_swap_chain_occluded = false;
static UINT g_resize_width = 0;
static UINT g_resize_height = 0;
static float g_pending_dpi_scale = 0.0f;

static App g_app;
static std::wstring g_config_path;
static char g_ip_buffer[64] = "";

// ---------------------------------------------------------------------------
// D3D11 boilerplate (from the Dear ImGui example_win32_directx11)

static void create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer != nullptr) {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target_view);
        back_buffer->Release();
    }
}

static void cleanup_render_target() {
    if (g_render_target_view != nullptr) {
        g_render_target_view->Release();
        g_render_target_view = nullptr;
    }
}

static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_device_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_device_context);
    }
    if (FAILED(hr)) {
        return false;
    }
    create_render_target();
    return true;
}

static void cleanup_device() {
    cleanup_render_target();
    if (g_swap_chain) {
        g_swap_chain->Release();
        g_swap_chain = nullptr;
    }
    if (g_device_context) {
        g_device_context->Release();
        g_device_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

// ---------------------------------------------------------------------------

static void apply_style(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

// %APPDATA%\overlay-hid-transmitter\overlay-hid-transmitter\settings.json,
// the same layout SDL_GetPrefPath() gives the other HID Forwarder tools.
static std::wstring config_path() {
    PWSTR appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appdata))) {
        CoTaskMemFree(appdata);
        return L"settings.json";
    }
    std::wstring path = appdata;
    CoTaskMemFree(appdata);
    for (const wchar_t* part : { L"\\overlay-hid-transmitter", L"\\overlay-hid-transmitter" }) {
        path += part;
        CreateDirectoryW(path.c_str(), nullptr);
    }
    return path + L"\\settings.json";
}

static bool g_track_placement = false;  // set once the saved placement has been applied

static void capture_window_placement() {
    if (!g_track_placement) {
        return;
    }
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (!GetWindowPlacement(g_app.main_hwnd, &wp)) {
        return;
    }
    WindowRect w;
    w.valid = true;
    w.x = wp.rcNormalPosition.left;
    w.y = wp.rcNormalPosition.top;
    w.w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    w.h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    w.maximized = wp.showCmd == SW_SHOWMAXIMIZED ||
                  (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    if (!(w == g_app.config.window)) {
        g_app.config.window = w;
        g_app.mark_dirty();
    }
}

static void save_now() {
    save_config(g_config_path, g_app.config);
    g_app.config_dirty = false;
}

static ImVec4 lerp_color(const Color3& c, float t, float a = 1.0f) {
    return ImVec4(c.r * t, c.g * t, c.b * t, a);
}

static void push_button_colors(ImVec4 base) {
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(base.x * 1.2f, base.y * 1.2f, base.z * 1.2f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(base.x * 1.4f, base.y * 1.4f, base.z * 1.4f, 1));
}

// A new region is placed near the center of the primary monitor, cascading
// so several new regions don't stack exactly on top of each other.
static Region default_new_region() {
    const float s = GetDpiForSystem() / 96.0f;
    Region r = g_app.config.new_region();
    r.w = (int) (160 * s);
    r.h = (int) (160 * s);
    int offset = (int) ((g_app.config.regions.size() % 10) * 24 * s);
    r.x = GetSystemMetrics(SM_CXSCREEN) / 2 - r.w / 2 + offset;
    r.y = GetSystemMetrics(SM_CYSCREEN) / 2 - r.h / 2 + offset;
    return r;
}

static void draw_status_line() {
    InputEngine& engine = g_app.engine;
    if (g_app.config.receiver_ip.empty()) {
        ImGui::TextDisabled("not set");
    } else if (!engine.address_valid()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "invalid IPv4 address");
    } else if (engine.send_error()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "send failed");
    } else if (g_app.running && !g_app.editing) {
        ImGui::TextColored(ImVec4(0.4f, 1, 0.5f, 1), "sending (%llu packets)", (unsigned long long) engine.packets_sent());
    } else {
        ImGui::TextDisabled("idle");
    }
}

static void draw_stick_indicator(Stick stick, const float (&value)[2]) {
    const float em = ImGui::GetFontSize();
    const float radius = em * 1.3f;
    const ImVec2 size(radius * 2, radius * 2);
    ImGui::BeginGroup();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + radius, p.y + radius);
    const Color3& col = STICK_COLORS[(int) stick];
    const bool centered = stick_byte(value[0]) == 128 && stick_byte(value[1]) == 128;
    dl->AddCircleFilled(c, radius, IM_COL32(40, 42, 48, 255));
    dl->AddCircle(c, radius, ImGui::GetColorU32(ImVec4(col.r, col.g, col.b, 1)), 0, 1.5f);
    dl->AddLine(ImVec2(c.x - radius, c.y), ImVec2(c.x + radius, c.y), IM_COL32(255, 255, 255, 30));
    dl->AddLine(ImVec2(c.x, c.y - radius), ImVec2(c.x, c.y + radius), IM_COL32(255, 255, 255, 30));
    const ImVec2 dot(c.x + value[0] * (radius - 3), c.y + value[1] * (radius - 3));
    dl->AddCircleFilled(dot, em * 0.22f,
        centered ? IM_COL32(120, 120, 130, 255) : ImGui::GetColorU32(ImVec4(col.r + (1 - col.r) * 0.4f, col.g + (1 - col.g) * 0.4f, col.b + (1 - col.b) * 0.4f, 1)));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(STICK_NAMES[(int) stick]);
    ImGui::TextDisabled("%+.2f, %+.2f", value[0], value[1]);
    ImGui::EndGroup();
    ImGui::EndGroup();
}

static void draw_output() {
    const GamepadState state = g_app.engine.current_state();
    const ImGuiStyle& style = ImGui::GetStyle();
    // Effective trigger travel: L2/R2 button regions count as fully pressed.
    float trigger_travel[TRIGGER_COUNT];
    for (int t = 0; t < TRIGGER_COUNT; t++) {
        trigger_travel[t] = (state.buttons & button_mask(trigger_button((Trigger) t))) ? 1.0f : state.triggers[t];
    }
    float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        bool on = (state.buttons & (1u << i)) != 0;
        if (i == (int) Button::L2 || i == (int) Button::R2) {
            // Same threshold as the digital bit in the report.
            on = trigger_byte(trigger_travel[i == (int) Button::L2 ? 0 : 1]) > 64;
        }
        const char* name = BUTTON_NAMES[i];
        float w = ImGui::CalcTextSize(name).x + style.FramePadding.x * 2;
        if (i > 0) {
            float next = ImGui::GetItemRectMax().x + style.ItemSpacing.x + w;
            if (next <= right_edge) {
                ImGui::SameLine();
            }
        }
        ImVec4 col = on ? lerp_color(BUTTON_COLORS[i], 1.0f) : ImVec4(0.18f, 0.19f, 0.22f, 1);
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 1, 1, 1) : ImVec4(0.5f, 0.5f, 0.55f, 1));
        ImGui::PushID(i);
        ImGui::Button(name);
        ImGui::PopID();
        ImGui::PopStyleColor(4);
    }
    // Fixed slot width, so the right indicator doesn't move as the left one's
    // value text changes width.
    const float em = ImGui::GetFontSize();
    const float slot_w = em * 2.6f + style.ItemSpacing.x +
                         std::max(ImGui::CalcTextSize(STICK_NAMES[(int) Stick::Right]).x,
                             ImGui::CalcTextSize("-0.00, -0.00").x * 1.15f);
    const float x0 = ImGui::GetCursorPosX();
    draw_stick_indicator(Stick::Left, state.sticks[(int) Stick::Left]);
    ImGui::SameLine(x0 + slot_w + em * 2);
    draw_stick_indicator(Stick::Right, state.sticks[(int) Stick::Right]);
    ImGui::SameLine(x0 + (slot_w + em * 2) * 2);
    ImGui::BeginGroup();
    for (int t = 0; t < TRIGGER_COUNT; t++) {
        const Color3& c = BUTTON_COLORS[(int) trigger_button((Trigger) t)];
        char overlay[16];
        snprintf(overlay, sizeof(overlay), "%.2f", trigger_travel[t]);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TRIGGER_NAMES[t]);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(c.r, c.g, c.b, 1));
        ImGui::ProgressBar(trigger_travel[t], ImVec2(em * 7, em * 1.1f), overlay);
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
}

// Combo for a region's mode, with a description tooltip on each entry.
static bool mode_combo(const char* id, RegionMode& mode) {
    bool changed = false;
    if (ImGui::BeginCombo(id, REGION_MODE_NAMES[(int) mode])) {
        for (int i = 0; i < REGION_MODE_COUNT; i++) {
            if (ImGui::Selectable(REGION_MODE_NAMES[i], (int) mode == i)) {
                mode = (RegionMode) i;
                changed = true;
            }
            ImGui::SetItemTooltip("%s", REGION_MODE_DESCRIPTIONS[i]);
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", REGION_MODE_DESCRIPTIONS[(int) mode]);
    return changed;
}

// Combo for what a region outputs: a button, or which stick.
static bool output_combo(const char* id, RegionType type, Button& button, Stick& stick, Trigger& trigger) {
    if (type == RegionType::Button) {
        int v = (int) button;
        if (ImGui::Combo(id, &v, BUTTON_NAMES, BUTTON_COUNT)) {
            button = (Button) v;
            return true;
        }
        return false;
    }
    if (type == RegionType::AnalogTrigger) {
        int v = (int) trigger;
        if (ImGui::Combo(id, &v, TRIGGER_NAMES, TRIGGER_COUNT)) {
            trigger = (Trigger) v;
            return true;
        }
        return false;
    }
    int v = (int) stick;
    if (ImGui::Combo(id, &v, STICK_NAMES, STICK_COUNT)) {
        stick = (Stick) v;
        return true;
    }
    return false;
}

// Settings that don't fit in the table row.
static bool draw_region_details(Region& r, int index) {
    const float em = ImGui::GetFontSize();
    bool changed = false;
    char title[64];
    snprintf(title, sizeof(title), "Region %d: %s", index + 1, REGION_TYPE_NAMES[(int) r.type]);
    ImGui::SeparatorText(title);

    switch (r.type) {
        case RegionType::Button:
            ImGui::TextDisabled("Presses %s.", BUTTON_NAMES[(int) r.button]);
            break;
        case RegionType::StickPosition: {
            ImGui::SetNextItemWidth(em * 9);
            changed |= ImGui::SliderFloat("X", &r.stick_x, -1.0f, 1.0f, "%+.2f");
            ImGui::SetItemTooltip("-1 = left, +1 = right. Ctrl+click to type a value.");
            ImGui::SameLine(0, em);
            ImGui::SetNextItemWidth(em * 9);
            changed |= ImGui::SliderFloat("Y", &r.stick_y, -1.0f, 1.0f, "%+.2f");
            ImGui::SetItemTooltip("-1 = up, +1 = down. Ctrl+click to type a value.");
            // Presets: diagonals use 0.71 so they stay on a round gate.
            struct Preset {
                float x, y;
            };
            static const Preset presets[] = {
                { -0.71f, -0.71f }, { 0, -1 }, { 0.71f, -0.71f }, { -1, 0 }, { 0, 0 },
                { 1, 0 }, { -0.71f, 0.71f }, { 0, 1 }, { 0.71f, 0.71f },
            };
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Presets:");
            const float fh = ImGui::GetFrameHeight();
            for (int i = 0; i < 9; i++) {
                const Preset& p = presets[i];
                ImGui::SameLine();
                ImGui::PushID(i);
                if (ImGui::Button("##preset", ImVec2(fh * 1.2f, fh))) {
                    r.stick_x = p.x;
                    r.stick_y = p.y;
                    changed = true;
                }
                ImGui::PopID();
                // Drawn rather than using arrow glyphs, which Segoe UI only partly has.
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                ImVec2 c((mn.x + mx.x) / 2, (mn.y + mx.y) / 2);
                ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
                float len = fh * 0.32f;
                if (p.x == 0 && p.y == 0) {
                    dl->AddCircleFilled(c, fh * 0.1f, col);
                } else {
                    float m = std::sqrt(p.x * p.x + p.y * p.y);
                    float dx = p.x / m, dy = p.y / m;
                    ImVec2 tip(c.x + dx * len, c.y + dy * len);
                    float th = std::max(1.5f, fh * 0.07f);
                    dl->AddLine(ImVec2(c.x - dx * len, c.y - dy * len), tip, col, th);
                    float hl = len * 0.7f;
                    dl->AddLine(tip, ImVec2(tip.x - hl * (dx * 0.707f - dy * 0.707f), tip.y - hl * (dy * 0.707f + dx * 0.707f)), col, th);
                    dl->AddLine(tip, ImVec2(tip.x - hl * (dx * 0.707f + dy * 0.707f), tip.y - hl * (dy * 0.707f - dx * 0.707f)), col, th);
                }
                ImGui::SetItemTooltip("X %+.2f, Y %+.2f", p.x, p.y);
            }
            break;
        }
        case RegionType::AnalogStick: {
            int inner = (int) std::lround(r.inner_deadzone * 100);
            int outer = (int) std::lround(r.outer_deadzone * 100);
            ImGui::SetNextItemWidth(em * 9);
            if (ImGui::SliderInt("Inner deadzone", &inner, 0, 90, "%d%%")) {
                r.inner_deadzone = inner / 100.0f;
                r.outer_deadzone = std::min(r.outer_deadzone, 0.95f - r.inner_deadzone);
                changed = true;
            }
            ImGui::SetItemTooltip("Part of the radius around the center that maps to a centered stick.");
            ImGui::SameLine(0, em);
            ImGui::SetNextItemWidth(em * 9);
            if (ImGui::SliderInt("Outer deadzone", &outer, 0, 90, "%d%%")) {
                r.outer_deadzone = outer / 100.0f;
                r.inner_deadzone = std::min(r.inner_deadzone, 0.95f - r.outer_deadzone);
                changed = true;
            }
            ImGui::SetItemTooltip("Part of the radius near the edge that maps to full deflection.");
            ImGui::TextDisabled("The ellipse drawn in the region marks full deflection.");
            break;
        }
        case RegionType::AnalogTrigger: {
            static const char* const orientations[] = { "Horizontal", "Vertical" };
            int o = r.vertical ? 1 : 0;
            ImGui::SetNextItemWidth(em * 7);
            if (ImGui::Combo("Orientation", &o, orientations, 2)) {
                r.vertical = o == 1;
                changed = true;
            }
            ImGui::SameLine(0, em);
            changed |= ImGui::Checkbox("Reversed", &r.reversed);
            ImGui::SameLine(0, em);
            int dz = (int) std::lround(r.trigger_deadzone * 100);
            ImGui::SetNextItemWidth(em * 9);
            if (ImGui::SliderInt("Deadzone", &dz, 0, 45, "%d%%")) {
                r.trigger_deadzone = dz / 100.0f;
                changed = true;
            }
            ImGui::SetItemTooltip("Part of the length at each end that snaps to released / fully pressed.");
            static const char* const ends[2][2] = { { "left", "right" }, { "bottom", "top" } };
            const char* released = ends[r.vertical][r.reversed ? 1 : 0];
            const char* pressed = ends[r.vertical][r.reversed ? 0 : 1];
            ImGui::TextDisabled("Released at the %s end (marked in the region), fully pressed at the %s end.", released,
                pressed);
            break;
        }
        default:
            break;
    }

    // Hover delay, for any type in a hover mode.
    if (r.mode == RegionMode::Hover || r.mode == RegionMode::HoverToggle) {
        ImGui::SetNextItemWidth(em * 9);
        if (ImGui::SliderInt("Hover delay", &r.hover_delay_ms, 0, 3000, r.hover_delay_ms == 0 ? "none" : "%d ms")) {
            r.hover_delay_ms = std::clamp(r.hover_delay_ms, 0, 60000);
            changed = true;
        }
        ImGui::SetItemTooltip("How long the cursor has to stay in the region before it activates.\n"
                              "Ctrl+click to type a value.");
        ImGui::SameLine(0, em);
        ImGui::TextDisabled("The region fills up while waiting.");
    }
    return changed;
}

static void draw_ui() {
    App& app = g_app;
    Config& cfg = app.config;
    ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

    // --- Big toggles
    const ImVec2 big(em * 9, ImGui::GetFrameHeight() * 1.7f);
    push_button_colors(app.running ? ImVec4(0.13f, 0.52f, 0.22f, 1) : ImVec4(0.45f, 0.17f, 0.17f, 1));
    if (ImGui::Button(app.running ? "RUNNING" : "STOPPED", big)) {
        app.running = !app.running;
    }
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip(app.running
            ? "Click to stop: hides the regions and releases all buttons."
            : "Click to start: shows the regions and starts sending inputs.");

    ImGui::SameLine();
    push_button_colors(app.editing ? ImVec4(0.20f, 0.40f, 0.75f, 1) : ImVec4(0.22f, 0.24f, 0.28f, 1));
    if (ImGui::Button(app.editing ? "Done editing" : "Edit regions", big)) {
        app.editing = !app.editing;
    }
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip("Arrange the regions directly on screen. No inputs are sent while editing.");

    // --- Receiver
    ImGui::SeparatorText("Receiver");
    ImGui::SetNextItemWidth(em * 10);
    if (ImGui::InputTextWithHint("IP address", "e.g. 192.168.1.50", g_ip_buffer, sizeof(g_ip_buffer),
            ImGuiInputTextFlags_CharsNoBlank)) {
        cfg.receiver_ip = g_ip_buffer;
        app.engine.set_receiver(cfg.receiver_ip);
        app.mark_dirty();
    }
    ImGui::SetItemTooltip("IP address of the HID Receiver. Inputs are sent as UDP to port 42734.");
    ImGui::SameLine();
    draw_status_line();

    // --- Display
    ImGui::SeparatorText("Display");
    int pct = (int) std::lround(cfg.opacity * 100);
    ImGui::SetNextItemWidth(em * 10);
    if (ImGui::SliderInt("Region opacity", &pct, 2, 100, "%d%%")) {
        cfg.opacity = pct / 100.0f;
        app.overlay.invalidate();
        app.mark_dirty();
    }
    ImGui::SameLine(0, em * 2);
    int active_pct = (int) std::lround(cfg.active_opacity * 100);
    ImGui::SetNextItemWidth(em * 10);
    if (ImGui::SliderInt("Active opacity", &active_pct, 2, 100, "%d%%")) {
        cfg.active_opacity = active_pct / 100.0f;
        app.overlay.invalidate();
        app.mark_dirty();
    }
    ImGui::SetItemTooltip("Opacity of regions while they're active (pressed).");
    if (ImGui::Checkbox("Keep this window on top", &cfg.keep_on_top)) {
        app.mark_dirty();
    }
    ImGui::SameLine(0, em * 2);
    ImGui::SetNextItemWidth(em * 7);
    int cm = (int) cfg.cursor_mode;
    if (ImGui::Combo("Draw cursor", &cm, CURSOR_MODE_NAMES, CURSOR_MODE_COUNT)) {
        cfg.cursor_mode = (CursorMode) cm;
        app.engine.set_cursor_mode(cfg.cursor_mode);
        app.mark_dirty();
    }
    ImGui::SetItemTooltip("Draw our own mouse cursor while running, for games that hide the cursor over their window.\n"
                          "\"When hidden\" only draws it while the system cursor is hidden or fully transparent.");
    if (ImGui::Checkbox("Block mouse movement from apps", &cfg.block_mouse)) {
        app.engine.set_block_mouse(cfg.block_mouse);
        app.mark_dirty();
    }
    ImGui::SetItemTooltip("While running, apps under the cursor don't see it move or hover over them, so they\n"
                          "won't react to the mouse (e.g. menus changing the highlighted item). Clicks still\n"
                          "get through. Doesn't help with apps that read raw mouse input.");
    if (!app.engine.hook_installed()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Mouse hook could not be installed: clicks on regions won't be captured.");
    }

    // --- Output
    ImGui::SeparatorText("Output");
    draw_output();

    // --- Regions
    ImGui::SeparatorText("Regions");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("New region:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 7.5f);
    int nt = (int) cfg.new_type;
    if (ImGui::Combo("##new_type", &nt, REGION_TYPE_NAMES, REGION_TYPE_COUNT)) {
        cfg.new_type = (RegionType) nt;
        app.mark_dirty();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 6.5f);
    if (output_combo("##new_output", cfg.new_type, cfg.new_button, cfg.new_stick, cfg.new_trigger)) {
        app.mark_dirty();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 6.5f);
    if (mode_combo("##new_mode", cfg.new_mode)) {
        app.mark_dirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add")) {
        app.add_region(default_new_region());
    }
    const bool has_selection = app.selected >= 0 && app.selected < (int) cfg.regions.size();
    ImGui::BeginDisabled(!has_selection);
    ImGui::SameLine();
    if (ImGui::Button("Duplicate") && has_selection) {
        const float s = GetDpiForSystem() / 96.0f;
        Region r = cfg.regions[app.selected];
        r.x += (int) (20 * s);
        r.y += (int) (20 * s);
        app.add_region(r);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && has_selection) {
        app.delete_region(app.selected);
    }
    ImGui::EndDisabled();

    if (app.editing) {
        ImGui::TextDisabled("Drag on empty screen space to draw a region (uses the settings above). "
                            "Del deletes the selected region, arrow keys nudge it (Shift = 10px).");
    } else {
        ImGui::TextDisabled("Overlapping regions combine their outputs. Use \"Edit regions\" to arrange them on screen.");
    }

    bool changed = false;
    int delete_index = -1;
    // Leave room for the selected region's details below the table.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float details_h = ImGui::GetFrameHeightWithSpacing() * 3 + ImGui::GetTextLineHeight() + style.ItemSpacing.y * 2 +
                            style.SeparatorTextPadding.y * 2;
    const float table_h = std::max(ImGui::GetFrameHeightWithSpacing() * 3, ImGui::GetContentRegionAvail().y - details_h);
    const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY |
                                        ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("regions", 9, table_flags, ImVec2(0, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, em * 1.8f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, em * 7.0f);
        ImGui::TableSetupColumn("Output", ImGuiTableColumnFlags_WidthFixed, em * 6.0f);
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, em * 7.0f);
        ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Width", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Height", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int) cfg.regions.size(); i++) {
            Region& r = cfg.regions[i];
            ImGui::PushID((int) r.id);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            char label[16];
            snprintf(label, sizeof(label), "%d", i + 1);
            if (ImGui::Selectable(label, app.selected == i,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                    ImVec2(0, ImGui::GetFrameHeight()))) {
                app.selected = i;
            }

            auto select_if_active = [&]() {
                if (ImGui::IsItemActivated()) {
                    app.selected = i;
                }
            };

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            int t = (int) r.type;
            if (ImGui::Combo("##type", &t, REGION_TYPE_NAMES, REGION_TYPE_COUNT)) {
                r.type = (RegionType) t;
                app.selected = i;  // show its settings below
                changed = true;
            }
            select_if_active();

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (output_combo("##output", r.type, r.button, r.stick, r.trigger)) {
                changed = true;
            }
            select_if_active();

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (mode_combo("##mode", r.mode)) {
                changed = true;
            }
            select_if_active();

            int* fields[4] = { &r.x, &r.y, &r.w, &r.h };
            const char* ids[4] = { "##x", "##y", "##w", "##h" };
            for (int f = 0; f < 4; f++) {
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                int min = f < 2 ? -100000 : 10;
                if (ImGui::DragInt(ids[f], fields[f], 1.0f, min, 100000, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                    changed = true;
                }
                select_if_active();
            }

            ImGui::TableNextColumn();
            if (ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), 0))) {
                delete_index = i;
            }
            ImGui::SetItemTooltip("Delete region");

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (app.selected >= 0 && app.selected < (int) cfg.regions.size()) {
        ImGui::PushID("details");
        changed |= draw_region_details(cfg.regions[app.selected], app.selected);
        ImGui::PopID();
    } else {
        ImGui::SeparatorText("Region settings");
        ImGui::TextDisabled("Select a region to see all its settings.");
    }

    if (changed) {
        app.regions_changed();
    }
    if (delete_index >= 0) {
        app.delete_region(delete_index);
    }

    // Keyboard shortcuts for the selected region while editing.
    if (app.editing && app.selected >= 0 && app.selected < (int) cfg.regions.size() &&
        !io.WantTextInput && !ImGui::IsAnyItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            app.delete_region(app.selected);
        } else {
            int step = io.KeyShift ? 10 : 1;
            int dx = 0, dy = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dx -= step;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dx += step;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dy -= step;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dy += step;
            if (dx != 0 || dy != 0) {
                cfg.regions[app.selected].x += dx;
                cfg.regions[app.selected].y += dy;
                app.regions_changed();
            }
        }
    }

    ImGui::End();
}

// Applies running/editing state to the overlay, the engine and window z-order.
static void update_window_state() {
    static int applied_main_topmost = -1;
    static ULONGLONG last_topmost_assert = 0;
    static bool was_visible = false;

    App& app = g_app;
    const bool overlay_visible = app.running || app.editing;
    app.overlay.set_mode(overlay_visible, app.editing);
    app.engine.set_active(app.running && !app.editing);

    // While editing, the overlay takes the mouse over the whole screen, so the
    // main window must stay above it to remain usable.
    const bool main_topmost = app.editing || app.config.keep_on_top;
    const ULONGLONG now = GetTickCount64();
    const bool just_shown = overlay_visible && !was_visible;
    was_visible = overlay_visible;

    // Other topmost windows (e.g. the taskbar) can end up above the overlay,
    // so re-assert our z-order periodically.
    if (overlay_visible && (just_shown || now - last_topmost_assert >= 1000)) {
        last_topmost_assert = now;
        SetWindowPos(app.overlay.hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (HWND cursor = app.engine.cursor_hwnd()) {
            // Our cursor goes above the overlay. Async: the window belongs to the engine thread.
            SetWindowPos(cursor, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
        if (main_topmost) {
            SetWindowPos(app.main_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    if ((int) main_topmost != applied_main_topmost) {
        applied_main_topmost = main_topmost;
        SetWindowPos(app.main_hwnd, main_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

static LRESULT WINAPI main_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return true;
    }
    switch (msg) {
        case WM_SIZE:
            capture_window_placement();
            if (wparam == SIZE_MINIMIZED) {
                return 0;
            }
            g_resize_width = (UINT) LOWORD(lparam);
            g_resize_height = (UINT) HIWORD(lparam);
            return 0;
        case WM_MOVE:
            capture_window_placement();
            return 0;
        case WM_CLOSE:
            capture_window_placement();
            break;
        case WM_GETMINMAXINFO: {
            float s = GetDpiForWindow(hwnd) / 96.0f;
            auto* mmi = (MINMAXINFO*) lparam;
            mmi->ptMinTrackSize.x = (LONG) (560 * s);
            mmi->ptMinTrackSize.y = (LONG) (420 * s);
            return 0;
        }
        case WM_DPICHANGED: {
            g_pending_dpi_scale = HIWORD(wparam) / 96.0f;
            const RECT* r = (const RECT*) lparam;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) {  // disable ALT application menu
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    // Region coordinates are physical pixels across all monitors.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    g_config_path = config_path();
    load_config(g_config_path, g_app.config);
    for (const Region& r : g_app.config.regions) {
        g_app.next_region_id = std::max(g_app.next_region_id, r.id + 1);
    }
    snprintf(g_ip_buffer, sizeof(g_ip_buffer), "%s", g_app.config.receiver_ip.c_str());
    if (wcsstr(GetCommandLineW(), L"--start") != nullptr) {
        g_app.running = true;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OverlayHidTransmitter";
    RegisterClassExW(&wc);

    // Restore the window where it was last time, but only if its title bar
    // would still be reachable (monitors may have changed since).
    const WindowRect& saved = g_app.config.window;
    bool restore = false;
    if (saved.valid) {
        RECT title = { saved.x, saved.y, saved.x + saved.w, saved.y + GetSystemMetrics(SM_CYCAPTION) };
        if (HMONITOR mon = MonitorFromRect(&title, MONITOR_DEFAULTTONULL)) {
            MONITORINFO mi = { sizeof(mi) };
            RECT visible;
            restore = GetMonitorInfoW(mon, &mi) && IntersectRect(&visible, &title, &mi.rcWork) &&
                      visible.right - visible.left >= 100;
        }
    }

    const float primary_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    // Creating the window at the saved spot (rather than moving it there
    // later) makes it start with that monitor's DPI, so its size isn't rescaled.
    HWND hwnd = restore
        ? CreateWindowW(wc.lpszClassName, L"Overlay HID Transmitter", WS_OVERLAPPEDWINDOW,
              saved.x, saved.y, saved.w, saved.h, nullptr, nullptr, instance, nullptr)
        : CreateWindowW(wc.lpszClassName, L"Overlay HID Transmitter", WS_OVERLAPPEDWINDOW,
              (int) (100 * primary_scale), (int) (100 * primary_scale), (int) (900 * primary_scale),
              (int) (760 * primary_scale), nullptr, nullptr, instance, nullptr);
    g_app.main_hwnd = hwnd;
    const float scale = GetDpiForWindow(hwnd) / 96.0f;

    if (!create_device(hwnd)) {
        cleanup_device();
        MessageBoxW(nullptr, L"Failed to initialize Direct3D 11.", L"Overlay HID Transmitter", MB_ICONERROR);
        return 1;
    }
    if (!g_app.overlay.create(instance, &g_app)) {
        cleanup_device();
        MessageBoxW(nullptr, L"Failed to create the overlay window.", L"Overlay HID Transmitter", MB_ICONERROR);
        return 1;
    }

    g_app.engine.set_regions(g_app.config.regions);
    g_app.engine.set_receiver(g_app.config.receiver_ip);
    g_app.engine.set_cursor_mode(g_app.config.cursor_mode);
    g_app.engine.set_block_mouse(g_app.config.block_mouse);
    g_app.engine.start(hwnd, g_app.overlay.hwnd(), Overlay::WM_APP_ENGINE_STATE);

    if (restore) {
        // Get/SetWindowPlacement use workspace coordinates, which is what we saved.
        WINDOWPLACEMENT wp = { sizeof(wp) };
        GetWindowPlacement(hwnd, &wp);
        wp.rcNormalPosition = { saved.x, saved.y, saved.x + saved.w, saved.y + saved.h };
        wp.showCmd = show_cmd;
        if (saved.maximized && (show_cmd == SW_SHOWNORMAL || show_cmd == SW_SHOW || show_cmd == SW_SHOWDEFAULT)) {
            wp.showCmd = SW_SHOWMAXIMIZED;
        }
        SetWindowPlacement(hwnd, &wp);
    } else {
        ShowWindow(hwnd, show_cmd);
    }
    UpdateWindow(hwnd);
    g_track_placement = true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    apply_style(scale);
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_device_context);

    int last_selected = g_app.selected;
    bool done = false;
    while (!done) {
        const bool minimized = IsIconic(hwnd) != 0;
        if (minimized || g_swap_chain_occluded) {
            // Nothing to draw in the main window: sleep until something happens.
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        update_window_state();
        g_app.overlay.render_if_dirty();

        if (g_app.config_dirty && !g_app.overlay.dragging() && GetTickCount64() - g_app.config_dirty_since >= 1000) {
            save_now();
        }

        if (minimized) {
            continue;
        }
        if (g_swap_chain_occluded && g_swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            continue;
        }
        g_swap_chain_occluded = false;

        if (g_resize_width != 0 && g_resize_height != 0) {
            cleanup_render_target();
            g_swap_chain->ResizeBuffers(0, g_resize_width, g_resize_height, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_width = g_resize_height = 0;
            create_render_target();
        }
        if (g_pending_dpi_scale > 0) {
            apply_style(g_pending_dpi_scale);
            g_pending_dpi_scale = 0;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_ui();
        ImGui::Render();

        if (g_app.selected != last_selected) {
            last_selected = g_app.selected;
            g_app.overlay.invalidate();
        }

        const float clear[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
        g_device_context->OMSetRenderTargets(1, &g_render_target_view, nullptr);
        g_device_context->ClearRenderTargetView(g_render_target_view, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_swap_chain->Present(1, 0);
        g_swap_chain_occluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    g_app.running = false;
    g_app.editing = false;
    g_app.engine.stop();
    if (g_app.config_dirty) {
        save_now();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_app.overlay.destroy();
    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    WSACleanup();
    return 0;
}
