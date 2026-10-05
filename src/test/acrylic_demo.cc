#include "breeze_ui/extra_widgets.h"
#include "breeze_ui/ui.h"
#include "breeze_ui/widget.h"

#include <chrono>
#include <memory>
#include <thread>

namespace {
template <typename base_t> struct demo_hover_acrylic_widget : base_t {
    demo_hover_acrylic_widget() : base_t() {
        opacity->reset_to(idle_opacity);
        opacity->set_duration(220.0f);
        opacity->set_easing(ui::easing_type::ease_in_out);
        radius->set_duration(220.0f);
        radius->set_easing(ui::easing_type::ease_in_out);
    }

    void tick(float delta_time) override {
        base_t::tick(delta_time);
        const bool is_hovered = this->hovered();
        opacity->animate_to(is_hovered ? hover_opacity : idle_opacity);
        radius->animate_to(is_hovered ? hover_radius : idle_radius);
    }

    float idle_opacity = 170.0f;
    float hover_opacity = 255.0f;
    float idle_radius = 20.0f;
    float hover_radius = 32.0f;
};

std::shared_ptr<ui::text_widget> make_caption(float x, float y,
                                              std::string text) {
    auto caption = std::make_shared<ui::text_widget>();
    caption->x->reset_to(x);
    caption->y->reset_to(y);
    caption->text = std::move(text);
    caption->font_size = 15;
    caption->color.reset_to({1.0f, 1.0f, 1.0f, 0.92f});
    return caption;
}
} // namespace

int main() {
    ui::render_target rt;
    rt.title = "Breeze Acrylic Demo";
    rt.width = 960;
    rt.height = 640;
    rt.transparent = true;
    // rt.decorated = false;
    rt.resizable = true;

    auto init_res = rt.init();
    if (!init_res || !*init_res) {
        return -1;
    }

    auto root = rt.root;

    auto composition = std::make_shared<
        demo_hover_acrylic_widget<ui::acrylic_background_widget>>();
    composition->x->reset_to(48);
    composition->y->reset_to(96);
    composition->width->reset_to(400);
    composition->height->reset_to(340);
    composition->idle_radius = 20.0f;
    composition->hover_radius = 28.0f;
    composition->bg_color = nvgRGBAf(0.84f, 0.90f, 0.98f, 0.22f);
    composition->acrylic_bg_color = nvgRGBAf(0.92f, 0.97f, 1.0f, 0.08f);
    root->add_child(composition);
    root->add_child(make_caption(48, 68, "Windows.UI.Composition host backdrop"));

    auto dwm = std::make_shared<
        demo_hover_acrylic_widget<ui::dwm_acrylic_background_widget>>();
    dwm->x->reset_to(500);
    dwm->y->reset_to(96);
    dwm->width->reset_to(400);
    dwm->height->reset_to(340);
    dwm->idle_opacity = 150.0f;
    dwm->idle_radius = 20.0f;
    dwm->hover_radius = 28.0f;
    dwm->bg_color = nvgRGBAf(0.16f, 0.19f, 0.24f, 0.30f);
    dwm->acrylic_bg_color = nvgRGBAf(0.70f, 0.80f, 0.96f, 0.05f);
    root->add_child(dwm);
    root->add_child(make_caption(500, 68, "DWM accent policy (fallback)"));

    auto tip = std::make_shared<ui::text_widget>();
    tip->x->reset_to(48);
    tip->y->reset_to(24);
    tip->text = "Hover acrylic blocks to preview opacity animation";
    tip->font_size = 18;
    tip->color.reset_to({1.0f, 1.0f, 1.0f, 0.92f});
    root->add_child(tip);

    rt.start_loop();
    return 0;
}
