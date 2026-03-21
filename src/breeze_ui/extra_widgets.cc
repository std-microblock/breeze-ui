#include "breeze_ui/extra_widgets.h"
#include "breeze_ui/widget.h"
#include <iostream>

#include "breeze_ui/ui.h"

namespace ui {
void acrylic_background_widget::update(update_context &ctx) {
    rect_widget::update(ctx);
}

void acrylic_background_widget::render(nanovg_context ctx) {
    widget::render(ctx);

    if (!owner_rt) {
        std::cerr << "[acrylic host] Missing render target" << std::endl;
        return;
    }

    owner_rt->register_acrylic_region({
        .x = *x + ctx.offset_x,
        .y = *y + ctx.offset_y,
        .width = *width,
        .height = *height,
        .radius = *radius,
        .opacity = *opacity / 255.f,
        .tint = acrylic_bg_color,
    });

    auto bg_color_tmp = bg_color;
    bg_color_tmp.a *= *opacity / 255.f;
    ctx.fillColor(bg_color_tmp);
    ctx.fillRoundedRect(*x, *y, *width, *height, *radius);
}

acrylic_background_widget::~acrylic_background_widget() = default;

void rect_widget::render(nanovg_context ctx) {
    bg_color.a = *opacity / 255.f;
    ctx.fillColor(bg_color);
    ctx.fillRoundedRect(*x, *y, *width, *height, *radius);
}
rect_widget::rect_widget() : widget() {}
rect_widget::~rect_widget() {}
} // namespace ui
