// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "engine.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cmath>

#include "stadia.h"

static constexpr uint16_t RECEIVER_PORT = 42734;
static constexpr int TICK_MS = 4;
// UDP has no retransmission, so we repeat the current state periodically
// to recover from lost packets (and from the receiver rebooting when it
// switches to the Stadia descriptor).
static constexpr uint64_t KEEPALIVE_MS = 100;
static constexpr int NEUTRAL_RESENDS = 3;

static InputEngine* g_engine = nullptr;

// dwExtraInfo of clicks we re-inject after moving the mouse shield away.
static constexpr ULONG_PTR REINJECT_TAG = 0x4F485452;
static constexpr UINT WM_APP_REINJECT = WM_APP + 10;

// Millisecond clock with better resolution than GetTickCount64's ~16 ms, so
// hover delays and their fill-up display are smooth.
static uint64_t now_ms() {
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (uint64_t) f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t) c.QuadPart * 1000 / freq;
}

bool InputEngine::start(HWND main_window, HWND notify_window, UINT notify_msg) {
    main_window_ = main_window;
    notify_window_ = notify_window;
    notify_msg_ = notify_msg;
    g_engine = this;
    quit_ = false;
    thread_ = std::thread(&InputEngine::thread_main, this);
    return true;
}

void InputEngine::stop() {
    if (!thread_.joinable()) {
        return;
    }
    quit_ = true;
    thread_.join();
    g_engine = nullptr;
}

void InputEngine::set_regions(const std::vector<Region>& regions) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_regions_ = regions;
    regions_pending_ = true;
}

void InputEngine::set_active(bool active) {
    active_requested_ = active;
}

void InputEngine::set_receiver(const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_receiver_ = ip;
    receiver_pending_ = true;
}

GamepadState InputEngine::current_state() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_state_;
}

void InputEngine::get_active_regions(std::vector<ActiveRegion>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out = active_regions_;
}

LRESULT CALLBACK InputEngine::hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && g_engine != nullptr) {
        if (g_engine->on_mouse(wparam, (const MSLLHOOKSTRUCT*) lparam)) {
            return 1;  // swallow the event
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

// Returns whether to swallow the event. Clicks that aren't swallowed make the
// mouse shield click-through, so they reach the window below; the shield comes
// back shortly after all buttons are released.
bool InputEngine::on_mouse(WPARAM msg, const MSLLHOOKSTRUCT* info) {
    if (info->dwExtraInfo == REINJECT_TAG) {
        return false;  // a click we re-injected ourselves
    }
    const bool swallow = handle_mouse(msg, info);
    if (swallow || !shield_.visible()) {
        return swallow;
    }
    int button = 0;
    bool down = false;
    DWORD flags = 0;
    DWORD data = 0;
    switch (msg) {
        case WM_LBUTTONDOWN: button = 1; down = true; flags = MOUSEEVENTF_LEFTDOWN; break;
        case WM_LBUTTONUP: button = 1; flags = MOUSEEVENTF_LEFTUP; break;
        case WM_RBUTTONDOWN: button = 2; down = true; flags = MOUSEEVENTF_RIGHTDOWN; break;
        case WM_RBUTTONUP: button = 2; flags = MOUSEEVENTF_RIGHTUP; break;
        case WM_MBUTTONDOWN: button = 4; down = true; flags = MOUSEEVENTF_MIDDLEDOWN; break;
        case WM_MBUTTONUP: button = 4; flags = MOUSEEVENTF_MIDDLEUP; break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            data = HIWORD(info->mouseData);
            button = data == XBUTTON1 ? 8 : 16;
            down = msg == WM_XBUTTONDOWN;
            flags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            break;
    }
    if (button == 0) {
        return false;
    }
    if (down) {
        passing_buttons_ |= button;
        if (!shield_.passthrough()) {
            // Windows has already picked the shield as this click's target, so
            // swallow it, get the shield out of the way and re-inject the click.
            shield_.set_passthrough(true);
            queue_reinject(flags, data, true);
            return true;
        }
    } else {
        passing_buttons_ &= ~button;
        if (passing_buttons_ == 0) {
            shield_restore_ms_ = now_ms() + 100;
        }
    }
    if (!reinject_queue_.empty()) {
        // Keep the order behind a re-injected press that hasn't gone out yet.
        queue_reinject(flags, data, false);
        return true;
    }
    return false;
}

void InputEngine::queue_reinject(DWORD flags, DWORD data, bool move_first) {
    if (reinject_queue_.empty()) {
        // Sent from the message loop, not from inside the hook callback.
        PostThreadMessageW(thread_id_, WM_APP_REINJECT, 0, 0);
    }
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwExtraInfo = REINJECT_TAG;
    if (move_first) {
        // A null move first, so Windows re-evaluates the window under the cursor.
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        reinject_queue_.push_back(in);
    }
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    reinject_queue_.push_back(in);
}

void InputEngine::flush_reinject() {
    if (!reinject_queue_.empty()) {
        SendInput((UINT) reinject_queue_.size(), reinject_queue_.data(), sizeof(INPUT));
        reinject_queue_.clear();
    }
}

bool InputEngine::handle_mouse(WPARAM msg, const MSLLHOOKSTRUCT* info) {
    // In a low-level hook GetCursorPos() still returns the previous position,
    // so use the event's.
    switch (msg) {
        case WM_LBUTTONDOWN: {
            sync_from_ui();
            if (!active_) {
                return false;
            }
            // Clicks on our own main window always go through.
            HWND window = WindowFromPoint(info->pt);
            if (window != nullptr && GetAncestor(window, GA_ROOT) == main_window_) {
                return false;
            }
            cursor_ = info->pt;
            bool hit = false;
            for (const Region& r : regions_) {
                if (r.mode == RegionMode::Hover || !r.contains(info->pt.x, info->pt.y)) {
                    continue;
                }
                hit = true;
                RegionState& s = state_[r.id];
                if (r.mode == RegionMode::Click) {
                    s.held = true;
                } else {
                    s.latched = !s.latched;
                    if (s.latched && r.type == RegionType::AnalogStick) {
                        analog_stick_value(r, info->pt.x, info->pt.y, s.latched_x, s.latched_y);
                    } else if (s.latched && r.type == RegionType::AnalogTrigger) {
                        s.latched_x = analog_trigger_value(r, info->pt.x, info->pt.y);
                    }
                }
            }
            if (!hit) {
                return false;
            }
            swallowing_click_ = true;
            compose_and_send(false);
            return true;
        }
        case WM_LBUTTONUP: {
            // Swallow the release of any press we swallowed, even if we were
            // deactivated in the meantime, so the app below never sees a stray up.
            if (!swallowing_click_) {
                return false;
            }
            swallowing_click_ = false;
            for (auto& [id, s] : state_) {
                s.held = false;
            }
            compose_and_send(false);
            return true;
        }
        case WM_MOUSEMOVE:
            if (active_) {
                cursor_ = info->pt;
                update_hover();
                compose_and_send(false);
            }
            return false;
        default:
            return false;
    }
}

void InputEngine::sync_from_ui() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (regions_pending_) {
            regions_ = std::move(pending_regions_);
            pending_regions_.clear();
            regions_pending_ = false;
            // Forget state of deleted regions, keep latched toggles of the rest.
            for (auto it = state_.begin(); it != state_.end();) {
                bool exists = std::any_of(regions_.begin(), regions_.end(),
                    [&](const Region& r) { return r.id == it->first; });
                it = exists ? std::next(it) : state_.erase(it);
            }
        }
        if (receiver_pending_) {
            receiver_pending_ = false;
            memset(&dest_, 0, sizeof(dest_));
            dest_.sin_family = AF_INET;
            dest_.sin_port = htons(RECEIVER_PORT);
            have_dest_ = inet_pton(AF_INET, pending_receiver_.c_str(), &dest_.sin_addr) == 1;
            address_valid_ = have_dest_;
            send_error_ = false;
        }
    }
    bool want_active = active_requested_.load();
    if (want_active != active_) {
        active_ = want_active;
        if (!active_) {
            release_all();
            neutral_resends_ = NEUTRAL_RESENDS;
        }
    }
}

void InputEngine::update_hover() {
    const uint64_t now = now_ms();
    for (const Region& r : regions_) {
        bool hover_mode = r.mode == RegionMode::Hover || r.mode == RegionMode::HoverToggle;
        bool inside = hover_mode && r.contains(cursor_.x, cursor_.y);
        if (inside) {
            RegionState& s = state_[r.id];
            if (!s.inside) {
                s.inside = true;
                s.inside_since_ms = now;
            }
            s.hovered = now - s.inside_since_ms >= (uint64_t) r.hover_delay_ms;
        } else {
            auto it = state_.find(r.id);
            if (it != state_.end()) {
                it->second.inside = false;
                it->second.hovered = false;
            }
        }
    }
}

void InputEngine::release_all() {
    for (auto& [id, s] : state_) {
        s = RegionState();
    }
}

// Whether the region is currently active, and its stick output if it has one.
bool InputEngine::region_output(const Region& r, const RegionState& s, float& x, float& y) const {
    bool on = false;
    bool use_latched = false;
    switch (r.mode) {
        case RegionMode::Click:
            on = s.held;
            break;
        case RegionMode::Hover:
            on = s.hovered;
            break;
        case RegionMode::Toggle:
            on = use_latched = s.latched;
            break;
        case RegionMode::HoverToggle:
            use_latched = s.latched;
            on = s.latched || s.hovered;
            break;
        default:
            break;
    }
    x = y = 0.0f;
    if (!on) {
        return false;
    }
    switch (r.type) {
        case RegionType::StickPosition:
            x = r.stick_x;
            y = r.stick_y;
            break;
        case RegionType::AnalogStick:
            if (use_latched) {
                x = s.latched_x;
                y = s.latched_y;
            } else {
                // Click regions keep following the cursor while held, even
                // outside the region (clamped to its edges).
                analog_stick_value(r, cursor_.x, cursor_.y, x, y);
            }
            break;
        case RegionType::AnalogTrigger:
            x = use_latched ? s.latched_x : analog_trigger_value(r, cursor_.x, cursor_.y);
            break;
        default:
            break;
    }
    return true;
}

void InputEngine::compose_and_send(bool force) {
    GamepadState state;
    std::vector<ActiveRegion> active;
    if (active_) {
        const uint64_t now = now_ms();
        for (const Region& r : regions_) {
            auto it = state_.find(r.id);
            if (it == state_.end()) {
                continue;
            }
            const RegionState& s = it->second;
            float x, y;
            if (!region_output(r, s, x, y)) {
                if (s.inside && !s.hovered && r.hover_delay_ms > 0) {
                    // Waiting out the hover delay: report progress for the
                    // fill-up display, in steps so we don't notify every tick.
                    float dwell = (float) (now - s.inside_since_ms) / (float) r.hover_delay_ms;
                    dwell = std::floor(std::clamp(dwell, 0.0f, 1.0f) * 50.0f) / 50.0f;
                    active.push_back({ r.id, false, dwell, 0.0f, 0.0f });
                }
                continue;
            }
            if (r.type == RegionType::Button) {
                state.buttons |= button_mask(r.button);
                active.push_back({ r.id, true, 0.0f, 0.0f, 0.0f });
            } else if (r.type == RegionType::AnalogTrigger) {
                state.triggers[(int) r.trigger] += x;
                active.push_back({ r.id, true, 0.0f, x, 0.0f });
            } else {
                // Overlapping stick regions add up.
                state.sticks[(int) r.stick][0] += x;
                state.sticks[(int) r.stick][1] += y;
                active.push_back({ r.id, true, 0.0f, x, y });
            }
        }
        for (auto& stick : state.sticks) {
            stick[0] = std::clamp(stick[0], -1.0f, 1.0f);
            stick[1] = std::clamp(stick[1], -1.0f, 1.0f);
        }
        for (float& t : state.triggers) {
            t = std::clamp(t, 0.0f, 1.0f);
        }
    }

    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_state_ = state;
        if (active != last_active_) {
            active_regions_ = active;
            notify = true;
        }
    }
    if (notify) {
        last_active_ = std::move(active);
        PostMessageW(notify_window_, notify_msg_, 0, 0);
    }

    uint64_t now = GetTickCount64();
    if (active_) {
        if (force || state != last_sent_ || now - last_send_ms_ >= KEEPALIVE_MS) {
            send_packet(state);
        }
    } else if (neutral_resends_ > 0) {
        if (state != last_sent_ || now - last_send_ms_ >= 20) {
            send_packet(GamepadState());
            neutral_resends_--;
        }
    }
}

void InputEngine::send_packet(const GamepadState& state) {
    last_sent_ = state;
    last_send_ms_ = GetTickCount64();
    if (!have_dest_ || socket_ == INVALID_SOCKET) {
        return;
    }
    uint8_t data[STADIA_PACKET_SIZE];
    size_t len = build_stadia_packet(state, data);
    int res = sendto(socket_, (const char*) data, (int) len, 0, (const sockaddr*) &dest_, sizeof(dest_));
    send_error_ = (res == SOCKET_ERROR);
    if (res != SOCKET_ERROR) {
        packets_sent_++;
    }
}

// Shows our cursor window at the cursor position when it's wanted. Runs after
// every batch of messages, which includes each mouse move through the hook,
// so it follows the mouse with no visible lag.
// is_blank_cursor() with the result cached per cursor handle, since the cursor
// rarely changes and inspecting its bitmaps on every mouse move would be wasteful.
bool InputEngine::blank_cursor(HCURSOR cursor) {
    if (cursor != checked_cursor_) {
        checked_cursor_ = cursor;
        checked_cursor_blank_ = is_blank_cursor(cursor);
    }
    return checked_cursor_blank_;
}

// Shows the mouse shield while running with "block mouse" on, with a hole over
// our main window, and ends click pass-through once all buttons are up.
void InputEngine::update_shield() {
    const bool want = active_ && block_mouse_.load();
    if (want) {
        RECT hole = {};
        if (IsWindowVisible(main_window_) && !IsIconic(main_window_)) {
            GetWindowRect(main_window_, &hole);
        }
        shield_.set_hole(hole);
    } else {
        passing_buttons_ = 0;
    }
    shield_.set_visible(want);
    if (shield_.passthrough() && passing_buttons_ == 0 && now_ms() >= shield_restore_ms_) {
        shield_.set_passthrough(false);
    }
}

void InputEngine::update_cursor() {
    const CursorMode mode = (CursorMode) cursor_mode_.load();
    bool want = active_ && mode != CursorMode::Off;
    if (want && mode == CursorMode::WhenHidden) {
        CURSORINFO ci = { sizeof(ci) };
        want = GetCursorInfo(&ci) && (!(ci.flags & CURSOR_SHOWING) || ci.hCursor == nullptr || blank_cursor(ci.hCursor));
    }
    if (want) {
        cursor_window_.move_to(cursor_);
    }
    cursor_window_.set_visible(want);
}

void InputEngine::thread_main() {
    thread_id_ = GetCurrentThreadId();
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // The hook must be serviced promptly or Windows drops it.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ != INVALID_SOCKET) {
        u_long nonblocking = 1;
        ioctlsocket(socket_, FIONBIO, &nonblocking);
    }

    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // make sure we have a message queue

    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, hook_proc, GetModuleHandleW(nullptr), 0);
    hook_installed_ = hook != nullptr;

    if (cursor_window_.create(GetModuleHandleW(nullptr))) {
        cursor_hwnd_ = cursor_window_.hwnd();
    }
    shield_.create(GetModuleHandleW(nullptr));

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due;
    due.QuadPart = -10000LL * TICK_MS;
    SetWaitableTimer(timer, &due, TICK_MS, nullptr, nullptr, FALSE);

    while (!quit_) {
        MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.hwnd == nullptr && msg.message == WM_APP_REINJECT) {
                flush_reinject();
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        sync_from_ui();
        if (active_) {
            // Also catches cursor moves that don't go through the hook (SetCursorPos).
            GetCursorPos(&cursor_);
            update_hover();
        }
        compose_and_send(false);
        update_cursor();
        update_shield();
    }

    cursor_hwnd_ = nullptr;
    cursor_window_.destroy();
    shield_.destroy();
    if (hook != nullptr) {
        UnhookWindowsHookEx(hook);
    }
    hook_installed_ = false;

    // Leave the receiver with nothing pressed.
    if (active_) {
        active_ = false;
        release_all();
        compose_and_send(false);
        send_packet(GamepadState());
    }

    CloseHandle(timer);
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}
