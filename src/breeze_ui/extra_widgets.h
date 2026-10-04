#pragma once
#include "breeze_ui/animator.h"
#include "breeze_ui/widget.h"
#include "nanovg.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace ui {

struct rect_widget : public widget {
    rect_widget();
    ~rect_widget();
    sp_anim_float opacity = anim_float(0, 200);
    sp_anim_float radius = anim_float(0, 0);

    NVGcolor bg_color = nvgRGBAf(0, 0, 0, 0);

    void render(nanovg_context ctx) override;
};

struct acrylic_background_widget : public rect_widget {
    ~acrylic_background_widget();
    NVGcolor acrylic_bg_color = nvgRGBAf(1, 0, 0, 0);

    void render(nanovg_context ctx) override;

    void update(update_context &ctx) override;
};

struct dwm_acrylic_background_widget : public rect_widget {
    explicit dwm_acrylic_background_widget(bool dwm_rounded_corners = true);
    ~dwm_acrylic_background_widget();
    NVGcolor acrylic_bg_color = nvgRGBAf(1, 0, 0, 0);
    void update_color();

    void render(nanovg_context ctx) override;

    void update(update_context &ctx) override;

private:
    void *hwnd = nullptr;
    bool dwm_rounded_corners = true;
    std::atomic<bool> should_update = true;
    std::optional<std::thread> render_thread;
    std::atomic<bool> to_close = false;
    float offset_x = 0, offset_y = 0, dpi_scale = 1;
    static thread_local void *last_hwnd;
    void *last_hwnd_self = nullptr;
    int current_xywh[4] = {};
};
} // namespace ui
