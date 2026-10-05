#include "breeze_ui/animator.h"
#include "breeze_ui/widget.h"
#include <array>
#include <cstdio>
#include <numbers>
#include <print>
#include <utility>

void ui::animated_float::update_slow(float delta_time) {
    const bool touched = std::exchange(_touched, false);
    step(delta_time);
    _updated = _updated || touched;
}
void ui::animated_float::step(float delta_time) {
    if (easing == easing_type::mutation) {
        if (destination != value || progress != 1.f) {
            value = destination;
            progress = 1.f;
            if (after_animate)
                after_animate.value()(destination);
            _updated = true;
        } else {
            _updated = false;
        }
        return;
    }

    if (delay_timer < delay) {
        delay_timer += delta_time;
        _updated = false;
        return;
    }

    progress += delta_time / duration;

    if (progress < 0.f) {
        _updated = false;
        return;
    }

    if (progress >= 1.f) {
        progress = 1.f;
        if (value != destination) {
            value = destination;
            _updated = true;
            if (after_animate) {
                after_animate.value()(destination);
            }
        } else {
            _updated = false;
        }
        return;
    }

    if (easing == easing_type::linear) {
        value = std::lerp(from, destination, progress);
    } else if (easing == easing_type::ease_in) {
        value = std::lerp(from, destination, progress * progress);
    } else if (easing == easing_type::ease_out) {
        value = std::lerp(from, destination, 1 - std::sqrt(1 - progress));
    } else if (easing == easing_type::ease_in_out) {
        value = std::lerp(from, destination,
                          (0.5f * std::sin(progress * std::numbers::pi -
                                           std::numbers::pi / 2) +
                           0.5f));
    }

    _updated = true;
}
void ui::animated_float::animate_to(float dest) {
    if (this->destination == dest)
        return;
    this->from = value;
    this->destination = dest;
    progress = 0.f;
    delay_timer = 0.f;
    _touched = true;

    if (before_animate) {
        before_animate.value()(dest);
    }

    if (this->easing == easing_type::mutation) {
        value = dest;
    }
    if (touch_log)
        touch_log->push_back(this);
}
void ui::animated_float::reset_to(float dest) {
    if (value == dest && destination == dest && progress >= 1.f) {
        delay_timer = 0.f;
        return;
    }
    if (value != dest)
        _touched = true;
    value = dest;
    this->from = dest;
    this->destination = dest;
    progress = 0.999999999f; // to avoid lerp issues
    delay_timer = 0.f;
    if (touch_log)
        touch_log->push_back(this);
}
void ui::animated_float::set_easing(easing_type easing) {
    this->easing = easing;
}
void ui::animated_float::set_duration(float duration) {
    this->duration = duration;
}
void ui::animated_float::set_delay(float delay) {
    this->delay = delay;
    delay_timer = 0.f;
}
std::array<float, 4> ui::animated_color::operator*() const {
    return {r->var(), g->var(), b->var(), a->var()};
}
ui::animated_color::animated_color(ui::widget *thiz, float r, float g, float b,
                                   float a, std::string name_prefix)
    : r(thiz->anim_float(r, name_prefix + ".r")),
      g(thiz->anim_float(g, name_prefix + ".g")),
      b(thiz->anim_float(b, name_prefix + ".b")),
      a(thiz->anim_float(a, name_prefix + ".a")) {
    for (auto &channel : {this->r, this->g, this->b, this->a})
        channel->affects_layout = false;
}
