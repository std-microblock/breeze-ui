#pragma once
#include "breeze_ui/animator.h"
#include "breeze_ui/widget.h"
#include "nanovg.h"
#include <condition_variable>
#include <mutex>
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
} // namespace ui
