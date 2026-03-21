#include "breeze_ui/extra_widgets.h"
#include "breeze_ui/ui.h"
#include "breeze_ui/widget.h"

#include <chrono>
#include <memory>
#include <thread>

namespace {
struct demo_hover_acrylic_widget : ui::acrylic_background_widget {
    float idle_opacity = 170.0f;
    float hover_opacity = 255.0f;
    float idle_radius = 20.0f;
    float hover_radius = 32.0f;

    demo_hover_acrylic_widget() : ui::acrylic_background_widget() {
        opacity->reset_to(idle_opacity);
        opacity->set_duration(220.0f);
        opacity->set_easing(ui::easing_type::ease_in_out);
        radius->set_duration(220.0f);
        radius->set_easing(ui::easing_type::ease_in_out);
    }

    void update(ui::update_context &ctx) override {
        ui::acrylic_background_widget::update(ctx);
        const bool is_hovered = ctx.hovered(this);
        opacity->animate_to(is_hovered ? hover_opacity : idle_opacity);
        radius->animate_to(is_hovered ? hover_radius : idle_radius);
    }
};
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

    auto acrylic1 = std::make_shared<demo_hover_acrylic_widget>();
    acrylic1->x->reset_to(48);
    acrylic1->y->reset_to(52);
    acrylic1->width->reset_to(360);
    acrylic1->height->reset_to(220);
    acrylic1->idle_radius = 20.0f;
    acrylic1->hover_radius = 28.0f;
    acrylic1->bg_color = nvgRGBAf(0.84f, 0.90f, 0.98f, 0.22f);
    acrylic1->acrylic_bg_color = nvgRGBAf(0.92f, 0.97f, 1.0f, 0.08f);
    root->add_child(acrylic1);

    auto acrylic2 = std::make_shared<demo_hover_acrylic_widget>();
    acrylic2->x->reset_to(280);
    acrylic2->y->reset_to(220);
    acrylic2->width->reset_to(520);
    acrylic2->height->reset_to(280);
    acrylic2->idle_opacity = 150.0f;
    acrylic2->hover_opacity = 255.0f;
    acrylic2->idle_radius = 28.0f;
    acrylic2->hover_radius = 40.0f;
    acrylic2->opacity->reset_to(acrylic2->idle_opacity);
    acrylic2->radius->reset_to(acrylic2->idle_radius);
    acrylic2->bg_color = nvgRGBAf(0.16f, 0.19f, 0.24f, 0.30f);
    acrylic2->acrylic_bg_color = nvgRGBAf(0.70f, 0.80f, 0.96f, 0.05f);
    root->add_child(acrylic2);

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
