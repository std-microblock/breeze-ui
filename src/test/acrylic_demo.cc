#include "breeze_ui/extra_widgets.h"
#include "breeze_ui/ui.h"

#include <chrono>
#include <memory>
#include <thread>

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

    auto acrylic1 = std::make_shared<ui::acrylic_background_widget>();
    acrylic1->x->reset_to(48);
    acrylic1->y->reset_to(52);
    acrylic1->width->reset_to(360);
    acrylic1->height->reset_to(220);
    acrylic1->radius->reset_to(20);
    acrylic1->opacity->reset_to(255);
    acrylic1->bg_color = nvgRGBAf(0.84f, 0.90f, 0.98f, 0.22f);
    acrylic1->acrylic_bg_color = nvgRGBAf(0.92f, 0.97f, 1.0f, 0.08f);
    root->add_child(acrylic1);

    auto acrylic2 = std::make_shared<ui::acrylic_background_widget>();
    acrylic2->x->reset_to(280);
    acrylic2->y->reset_to(220);
    acrylic2->width->reset_to(520);
    acrylic2->height->reset_to(280);
    acrylic2->radius->reset_to(28);
    acrylic2->opacity->reset_to(255);
    acrylic2->bg_color = nvgRGBAf(0.16f, 0.19f, 0.24f, 0.30f);
    acrylic2->acrylic_bg_color = nvgRGBAf(0.70f, 0.80f, 0.96f, 0.05f);
    root->add_child(acrylic2);

    rt.start_loop();
    return 0;
}
