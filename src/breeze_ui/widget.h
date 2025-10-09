#pragma once
#include "breeze_ui/animator.h"
#include "breeze_ui/nanovg_wrapper.h"

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace ui {
struct render_target;
struct widget;
struct screen_info {
    int width, height;
    float dpi_scale;
};
struct update_context {
    // time since last frame, in milliseconds
    float delta_time;
    // mouse position in window coordinates
    double mouse_x, mouse_y;
    bool mouse_down, right_mouse_down;
    void *window;
    // only true for one frame
    bool mouse_clicked, right_mouse_clicked;
    bool mouse_up;
    screen_info screen;
    float scroll_y;

    bool &need_repaint;

    // hit test, lifetime is not guaranteed
    std::shared_ptr<std::vector<widget *>> hovered_widgets =
        std::make_shared<std::vector<widget *>>();
    void set_hit_hovered(widget *w);

    bool hovered(widget *w, bool hittest = true) const;
    void print_hover_info(widget *w) const;
    bool mouse_clicked_on(widget *w, bool hittest = true) const;
    bool mouse_down_on(widget *w, bool hittest = true) const;

    bool mouse_clicked_on_hit(widget *w, bool hittest = true);
    bool hovered_hit(widget *w, bool hittest = true);
    bool key_pressed(int key) const;
    void stop_key_propagation(int key);
    bool key_down(int key) const;

    float offset_x = 0, offset_y = 0;
    render_target &rt;
    nanovg_context vg;

    update_context with_offset(float x, float y) const {
        auto copy = *this;
        copy.offset_x = x + offset_x;
        copy.offset_y = y + offset_y;
        return copy;
    }

    update_context with_reset_offset(float x = 0, float y = 0) const {
        auto copy = *this;
        copy.offset_x = x;
        copy.offset_y = y;
        return copy;
    }

    update_context within(widget *w) const;
};

struct dying_time {
    float time = 100;
    bool _last_has_value = false;
    bool _changed = false;
    inline bool changed() const {
        return _last_has_value != has_value || _changed;
    }
    bool has_value = false;

    operator bool() const { return has_value; }
    inline float operator=(float t) {
        time = t;
        has_value = true;
        return t;
    }
    inline float operator-=(float t) {
        time -= t;
        has_value = true;
        return time;
    }
    inline void operator=(std::nullopt_t) { has_value = false; }
    inline void reset() {
        has_value = false;
        time = 0;
    }

    inline void update(float dt) {
        if (has_value && time > 0) {
            time -= dt;
        }

        if (_last_has_value != has_value) {
            _changed = true;
            _last_has_value = has_value;
        } else {
            _changed = false;
        }
    }
};

/*
All the widgets in the tree should be wrapped in a shared_ptr.
If you want to use a widget in multiple places, you should create a new instance
for each place.

It is responsible for updating and rendering its children
It also sets the offset for the children
It's like `posision: relative` in CSS
While all other widgets are like `position: absolute`
*/
struct widget : std::enable_shared_from_this<widget> {
    std::vector<sp_anim_float> anim_floats{};

    std::vector<std::string> class_list{};
    sp_anim_float anim_float(auto &&...args) {
        auto anim = std::make_shared<animated_float>(
            std::forward<decltype(args)>(args)...);
        anim_floats.push_back(anim);
        return anim;
    }

    sp_anim_float x = anim_float("x"), y = anim_float("y"),
                  width = anim_float("width"), height = anim_float("height");

    // Flex grow factor (0 means no growing)
    float flex_grow = 0.0f;

    float _debug_offset_cache[2];
    bool enable_child_clipping = false;
    bool needs_repaint = true;
    float last_offset_x = 0, last_offset_y = 0;

    // Time until the widget is removed from the tree
    // in milliseconds
    // Widget itself will update this value
    // And its parent is responsible for removing it
    // when the time is up
    dying_time dying_time;

    widget *parent = nullptr;
    render_target *owner_rt = nullptr;

    bool focused();
    bool focus_within();
    void set_focus(bool focused = true);

    template <typename T> inline T *search_parent() {
        auto p = parent;
        while (p) {
            if (auto t = dynamic_cast<T *>(p)) {
                return t;
            }
            p = p->parent;
        }
        return nullptr;
    }
    virtual void render(nanovg_context ctx);
    virtual void update(update_context &ctx);
    virtual ~widget() = default;
    // Measure the desired size of the widget
    // It should return the size it wants to be, not the size it is forced to be
    // by the parent
    virtual float measure_height(update_context &ctx);
    virtual float measure_width(update_context &ctx);
    // Update children with the offset.
    // Also deal with the dying time. (If the widget is died, it will be set to
    // nullptr)
    void update_child_basic(update_context &ctx, std::shared_ptr<widget> &w);
    // Render children with the offset.
    void render_child_basic(nanovg_context ctx, std::shared_ptr<widget> &w);

    // Update children list in the widget manner
    // It will remove the dead children
    // It will also update the dying time
    // It will **NOT** update the children with the offset, call it with
    // with_offset(*x, *y) if needed
    void update_children(update_context &ctx,
                         std::vector<std::shared_ptr<widget>> &children);
    // Render children list in the widget manner
    void render_children(nanovg_context ctx,
                         std::vector<std::shared_ptr<widget>> &children);

    template <typename T> inline auto downcast() {
        return std::dynamic_pointer_cast<T>(this->shared_from_this());
    }

    virtual bool check_hit(const update_context &ctx);

    void add_child(std::shared_ptr<widget> child);
    void remove_child(std::shared_ptr<widget> child);
    std::vector<std::shared_ptr<widget>> children;
    bool children_dirty = false;
    template <typename T, typename... Args>
    inline std::shared_ptr<T> emplace_child(Args &&...args) {
        auto child = std::make_shared<T>(std::forward<Args>(args)...);
        children.emplace_back(child);
        return child;
    }

    template <typename T> inline std::shared_ptr<T> get_child() {
        for (auto &child : children) {
            if (auto c = child->downcast<T>()) {
                return c;
            }
        }
        return nullptr;
    }

    template <typename T>
    inline std::vector<std::shared_ptr<T>> get_children() {
        std::vector<std::shared_ptr<T>> res;
        for (auto &child : children) {
            if (auto c = child->downcast<T>()) {
                res.push_back(c);
            }
        }
        return res;
    }
};

// A widget with child which lays out children in a row or column.
//
// Specifically, when `horizontal == false` and `align_items == stretch`,
// it sets its text_widget child's max width to its width.
struct flex_widget : public widget {
    enum class justify {
        start,
        end,
        center,
        space_between,
        space_around,
        space_evenly,
        free
    };

    enum class align { start, end, center, stretch, free };

    // Scrolling stuff
    float max_height = INFINITY;
    bool enable_scrolling = false;
    sp_anim_float scroll_top =
        anim_float(0, 150, easing_type::ease_in_out);
    NVGcolor scroll_bar_color = nvgRGBA(200, 200, 200, 128);
    float scroll_bar_width = 6;
    float scroll_bar_margin = 2;
    float scroll_bar_radius = 3;
    float actual_height = 0;

    bool crop_overflow = true;
    float gap = 0;
    bool horizontal = false;
    bool auto_size = true;
    bool reverse = false;
    justify justify_content = justify::start;
    align align_items = align::start;
    sp_anim_float padding_left = anim_float(), padding_right = anim_float(),
                  padding_top = anim_float(), padding_bottom = anim_float();
    void
    reposition_children_flex(update_context &ctx,
                             std::vector<std::shared_ptr<widget>> &children);
    void update(update_context &ctx) override;
    void render(nanovg_context ctx) override;

    float measure_height(update_context &ctx) override;
    float measure_width(update_context &ctx) override;

    // Determine if the widget should auto size in the given direction.
    // `should_autosize(horizontal)` checks width side.
    // `should_autosize(!horizontal)` checks height side.
    bool should_autosize(bool mainAxis) const;

    struct spacer : public widget {
        float size = 1;
    };
};
// A widget that renders text
struct text_widget : public widget {
    std::string text;
    float font_size = 14;
    std::string font_family = "main";
    animated_color color = {this, 0, 0, 0, 1, "txt"};
    float max_width = -1; // <=0 means no limit

    void render(nanovg_context ctx) override;

    bool shrink_vertical = true, shrink_horizontal = true;
    void update(update_context &ctx) override;
};

// A widget that renders children in it with a padding
struct padding_widget : public widget {
    sp_anim_float padding_left = anim_float(0), padding_right = anim_float(0),
                  padding_top = anim_float(0), padding_bottom = anim_float(0);

    void update(update_context &ctx) override;
    void render(nanovg_context ctx) override;
};

struct button_widget : public ui::padding_widget {
    button_widget();
    button_widget(const std::string &button_text);

    ui::animated_color border_top = {this, 0, 0, 0, 0},
                       border_right = {this, 0, 0, 0, 0},
                       border_bottom = {this, 0, 0, 0, 0},
                       border_left = {this, 0, 0, 0, 0};

    void render(ui::nanovg_context ctx) override;

    ui::animated_color bg_color = {this, 40 / 255.f, 40 / 255.f, 40 / 255.f,
                                   0.6};

    virtual void on_click();

    virtual void update_colors(bool is_active, bool is_hovered);
    ui::update_context *ctx;
    void update(ui::update_context &ctx) override;
};
} // namespace ui