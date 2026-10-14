// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include "engine.h"
#include "model.h"
#include "overlay.h"

// Application state, owned and mutated by the UI thread only.
struct App {
    Config config;
    uint32_t next_region_id = 1;
    int selected = -1;  // index into config.regions
    bool running = false;
    bool editing = false;

    bool config_dirty = false;
    ULONGLONG config_dirty_since = 0;

    HWND main_hwnd = nullptr;
    InputEngine engine;
    Overlay overlay;

    // Call after any change to config.regions.
    void regions_changed() {
        engine.set_regions(config.regions);
        overlay.invalidate();
        mark_dirty();
    }

    void mark_dirty() {
        if (!config_dirty) {
            config_dirty_since = GetTickCount64();
        }
        config_dirty = true;
    }

    int add_region(Region r) {
        r.id = next_region_id++;
        config.regions.push_back(r);
        selected = (int) config.regions.size() - 1;
        regions_changed();
        return selected;
    }

    void delete_region(int index) {
        if (index < 0 || index >= (int) config.regions.size()) {
            return;
        }
        config.regions.erase(config.regions.begin() + index);
        if (selected == index) {
            selected = -1;
        } else if (selected > index) {
            selected--;
        }
        regions_changed();
    }
};
