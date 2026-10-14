// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cursor.h"
#include "shield.h"
#include "model.h"

// Runs on its own thread: owns the low-level mouse hook, tracks hover state,
// composes the gamepad report and sends it to the receiver over UDP.
// Keeping this off the UI thread means the hook is never delayed by rendering.
class InputEngine {
public:
    // A region currently contributing to the output, or waiting out its hover
    // delay, for display.
    struct ActiveRegion {
        uint32_t id = 0;
        bool on = false;     // contributing to the output
        float dwell = 0.0f;  // hover delay progress (0..1) while waiting to activate
        float x = 0.0f;      // stick output of AnalogStick regions, travel of AnalogTrigger regions
        float y = 0.0f;

        bool operator==(const ActiveRegion& o) const {
            return id == o.id && on == o.on && dwell == o.dwell && x == o.x && y == o.y;
        }
    };

    bool start(HWND main_window, HWND notify_window, UINT notify_msg);
    void stop();

    // Called from the UI thread whenever regions are edited.
    void set_regions(const std::vector<Region>& regions);
    // Active = regions respond to the mouse. When inactive, everything is released.
    void set_active(bool active);
    void set_receiver(const std::string& ip);
    void set_cursor_mode(CursorMode mode) { cursor_mode_ = (int) mode; }
    void set_block_mouse(bool block) { block_mouse_ = block; }
    // Our cursor window (owned by the engine thread), for z-order management.
    HWND cursor_hwnd() const { return cursor_hwnd_.load(); }

    GamepadState current_state();
    void get_active_regions(std::vector<ActiveRegion>& out);

    bool hook_installed() const { return hook_installed_.load(); }
    bool address_valid() const { return address_valid_.load(); }
    bool send_error() const { return send_error_.load(); }
    uint64_t packets_sent() const { return packets_sent_.load(); }

private:
    struct RegionState {
        bool held = false;     // Click: mouse button held after clicking in the region
        bool latched = false;  // Toggle, HoverToggle
        bool inside = false;   // Hover, HoverToggle: cursor is inside the region
        uint64_t inside_since_ms = 0;
        bool hovered = false;  // inside for at least the hover delay
        float latched_x = 0.0f;  // AnalogStick/AnalogTrigger value captured when latching
        float latched_y = 0.0f;
    };

    static LRESULT CALLBACK hook_proc(int code, WPARAM wparam, LPARAM lparam);
    bool on_mouse(WPARAM msg, const MSLLHOOKSTRUCT* info);
    bool handle_mouse(WPARAM msg, const MSLLHOOKSTRUCT* info);
    void thread_main();
    void sync_from_ui();
    void update_hover();
    void release_all();
    bool region_output(const Region& r, const RegionState& s, float& x, float& y) const;
    void compose_and_send(bool force);
    void send_packet(const GamepadState& state);
    void update_cursor();
    void update_shield();
    void queue_reinject(DWORD flags, DWORD data, bool move_first);
    void flush_reinject();
    bool blank_cursor(HCURSOR cursor);

    HWND main_window_ = nullptr;
    HWND notify_window_ = nullptr;
    UINT notify_msg_ = 0;

    std::thread thread_;
    std::atomic<bool> quit_{ false };

    // Shared with the UI thread.
    std::mutex mutex_;
    std::vector<Region> pending_regions_;
    bool regions_pending_ = false;
    std::string pending_receiver_;
    bool receiver_pending_ = false;
    std::atomic<bool> active_requested_{ false };
    std::vector<ActiveRegion> active_regions_;  // guarded by mutex_
    GamepadState current_state_;                // guarded by mutex_

    std::atomic<bool> hook_installed_{ false };
    std::atomic<bool> address_valid_{ false };
    std::atomic<bool> send_error_{ false };
    std::atomic<uint64_t> packets_sent_{ 0 };
    std::atomic<int> cursor_mode_{ (int) CursorMode::Off };
    std::atomic<HWND> cursor_hwnd_{ nullptr };
    std::atomic<bool> block_mouse_{ false };

    // Engine thread only (the hook callback runs on the engine thread too).
    std::vector<Region> regions_;
    std::unordered_map<uint32_t, RegionState> state_;
    std::vector<ActiveRegion> last_active_;
    POINT cursor_{};
    bool active_ = false;
    bool swallowing_click_ = false;
    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in dest_{};
    bool have_dest_ = false;
    GamepadState last_sent_;
    uint64_t last_send_ms_ = 0;
    int neutral_resends_ = 0;
    CursorWindow cursor_window_;
    HCURSOR checked_cursor_ = nullptr;
    bool checked_cursor_blank_ = false;
    MouseShield shield_;
    int passing_buttons_ = 0;  // buttons whose clicks are passing through the shield
    uint64_t shield_restore_ms_ = 0;
    std::vector<INPUT> reinject_queue_;
    DWORD thread_id_ = 0;
};
