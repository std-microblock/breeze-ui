#pragma once
#include "breeze_ui/animator.h"
#include "breeze_ui/nanovg_wrapper.h"

#include <yoga/Yoga.h>

#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ui {
struct render_target;
struct ime_composition_state;
struct widget;

struct screen_info {
    int width, height;
    float dpi_scale;
};

enum class mouse_button { left = 0, right = 1, middle = 2 };

struct mouse_event {
    float x = 0, y = 0;
    mouse_button button = mouse_button::left;
    bool handled = false;
};

struct scroll_event {
    float x = 0, y = 0;
    float delta = 0;
    bool handled = false;
};

struct key_event {
    int key = 0;
    int mods = 0;
    bool repeat = false;
    bool handled = false;

    bool shift() const;
    bool ctrl() const;
    bool alt() const;
    bool super() const;
};

struct text_input_event {
    std::u32string text;
    bool handled = false;
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

// Frame: input -> tick -> layout -> paint. Children of non-layout widgets,
// floating and manual_position children are laid out as detached Yoga roots.
struct widget : std::enable_shared_from_this<widget> {
    widget();
    widget(const widget &) = delete;
    widget &operator=(const widget &) = delete;
    virtual ~widget();

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

    float flex_grow = 0.0f;
    float flex_shrink = 0.0f;

    bool manual_position = false;
    bool manual_size = false;
    bool fixed_width = false, fixed_height = false;
    bool enable_child_clipping = false;
    bool hit_self = true;
    bool visible = true;
    bool needs_repaint = true;

    dying_time dying_time;

    widget *parent = nullptr;
    render_target *owner_rt = nullptr;
    YGNodeRef node = nullptr;

    std::vector<std::shared_ptr<widget>> children;
    std::vector<std::shared_ptr<widget>> floating;
    bool children_dirty = false;

    bool focused();
    bool focus_within();
    void set_focus(bool focused = true);
    bool hovered() const;
    bool pressed() const;

    void request_repaint();
    void mark_layout_dirty();

    void set_width(float v);
    void set_width_percent(float v);
    void set_width_auto();
    void set_height(float v);
    void set_height_percent(float v);
    void set_height_auto();
    void set_min_width(float v);
    void set_min_height(float v);
    void set_max_width(float v);
    void set_max_height(float v);
    void set_margin(YGEdge edge, float v);
    void set_flex_basis(float v);
    void set_align_self(YGAlign align);

    float offset_x() const;
    float offset_y() const;
    float abs_x() const { return offset_x() + x->var(); }
    float abs_y() const { return offset_y() + y->var(); }
    virtual float child_offset_x() const { return 0; }
    virtual float child_offset_y() const { return 0; }
    virtual bool lays_out_children() const { return false; }
    virtual bool clips_children() const { return enable_child_clipping; }
    virtual bool reversed_flow() const { return false; }

    virtual void tick(float delta_time) {}
    virtual void before_layout();
    virtual void after_layout() {}
    virtual bool has_measure() const;
    virtual YGSize measure(float width, YGMeasureMode width_mode, float height,
                           YGMeasureMode height_mode);
    void invalidate_measure();

    virtual bool hit_test(float px, float py) const;
    virtual void handle_mouse_enter() {}
    virtual void handle_mouse_leave() {}
    virtual void handle_mouse_move(mouse_event &e) {}
    virtual void handle_mouse_down(mouse_event &e) {}
    virtual void handle_mouse_up(mouse_event &e) {}
    virtual void handle_click(mouse_event &e) {}
    virtual void handle_scroll(scroll_event &e) {}
    virtual void handle_key(key_event &e) {}
    virtual void handle_text_input(text_input_event &e) {}
    virtual void handle_focus_changed(bool focused) {}

    virtual void render(nanovg_context ctx);
    void render_children(nanovg_context ctx,
                         std::vector<std::shared_ptr<widget>> &list);

    bool tick_tree(float delta_time, bool &animating);
    void prepare_layout_tree(render_target *rt);
    void apply_layout_tree();
    void layout_detached(float available_width = YGUndefined,
                         float available_height = YGUndefined);
    void compute_layout_now(render_target *rt);
    bool attached_to_parent() const;
    virtual widget *hit_test_tree(float px, float py);

    void add_child(std::shared_ptr<widget> child);
    void insert_child(size_t index, std::shared_ptr<widget> child);
    void remove_child(std::shared_ptr<widget> child);
    void add_floating(std::shared_ptr<widget> child);
    void remove_floating(std::shared_ptr<widget> child);
    template <typename T, typename... Args>
    inline std::shared_ptr<T> emplace_child(Args &&...args) {
        auto child = std::make_shared<T>(std::forward<Args>(args)...);
        add_child(child);
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

    template <typename T> inline auto downcast() {
        return std::dynamic_pointer_cast<T>(this->shared_from_this());
    }

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

    nanovg_context measure_context() const;

  protected:
    float user_width = 0, user_height = 0;
    bool explicit_width = false, explicit_height = false;
    void apply_user_size(bool w, bool h);

  private:
    friend struct render_target;
    bool is_floating = false;
    bool measure_attached = false;
    float applied_width = NAN, applied_height = NAN;
    void sync_yoga_children();
    bool axis_free(bool horizontal_axis) const;
};

struct text_measure_scope {
    nanovg_context vg;
    explicit text_measure_scope(const widget &w);
    ~text_measure_scope();
    text_measure_scope(const text_measure_scope &) = delete;
    explicit operator bool() const { return vg.ctx != nullptr; }
};

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

    float max_height = INFINITY;
    bool enable_scrolling = false;
    sp_anim_float scroll_top =
        anim_float(0, 150, easing_type::ease_in_out);
    NVGcolor scroll_bar_color = nvgRGBA(200, 200, 200, 128);
    float scroll_bar_width = 6;
    float scroll_bar_margin = 2;
    float scroll_bar_radius = 3;
    float actual_height = 0;

    bool crop_overflow = false;
    float gap = 0;
    bool horizontal = false;
    bool auto_size = true;
    bool reverse = false;
    justify justify_content = justify::start;
    align align_items = align::start;
    sp_anim_float padding_left = anim_float(), padding_right = anim_float(),
                  padding_top = anim_float(), padding_bottom = anim_float();

    bool lays_out_children() const override { return true; }
    bool clips_children() const override {
        return enable_child_clipping || crop_overflow || enable_scrolling;
    }
    float child_offset_y() const override { return scroll_top->var(); }
    bool reversed_flow() const override;
    void before_layout() override;
    void after_layout() override;
    void handle_scroll(scroll_event &e) override;
    void render(nanovg_context ctx) override;
    void render_scrollbar(nanovg_context &ctx);
    float max_scroll() const;

    struct spacer : public widget {
        float size = 1;
        void before_layout() override;
    };
};

struct text_widget : public widget {
    std::string text;
    float font_size = 14;
    int font_weight = 400;
    std::string font_family = "main";
    animated_color color = {this, 0, 0, 0, 1, "txt"};
    float max_width = -1;
    bool shrink_vertical = true, shrink_horizontal = true;

    void render(nanovg_context ctx) override;
    void before_layout() override;
    bool has_measure() const override { return true; }
    YGSize measure(float width, YGMeasureMode width_mode, float height,
                   YGMeasureMode height_mode) override;

  private:
    struct measure_key {
        std::string text;
        float font_size;
        int font_weight;
        std::string font_family;
        float max_width;
        bool operator==(const measure_key &) const = default;
    };
    std::optional<measure_key> last_key;
    std::string face_key, resolved_face;
    float natural_width = 0;
    const std::string &face(nanovg_context &ctx);
};

struct textbox_widget : public widget {
    std::string text;
    std::string placeholder;
    float font_size = 14;
    int font_weight = 400;
    float padding_x = 8;
    float padding_y = 6;
    float border_radius = 6;
    float min_height = 32;
    float preferred_multiline_height = 96;
    float line_height_multiplier = 1;
    bool multiline = false;
    bool readonly = false;
    bool disabled = false;
    animated_color background_color = {this, 1.f, 1.f, 1.f, 235.f / 255.f,
                                       "textbox.bg"};
    animated_color readonly_background_color = {
        this, 250.f / 255.f, 250.f / 255.f, 250.f / 255.f, 235.f / 255.f,
        "textbox.readonly_bg"};
    animated_color disabled_background_color = {
        this, 235.f / 255.f, 235.f / 255.f, 235.f / 255.f, 220.f / 255.f,
        "textbox.disabled_bg"};
    animated_color border_color = {this, 180.f / 255.f, 180.f / 255.f,
                                   180.f / 255.f, 1.f, "textbox.border"};
    animated_color focus_border_color = {this, 59.f / 255.f, 130.f / 255.f,
                                         246.f / 255.f, 1.f,
                                         "textbox.focus_border"};
    animated_color text_color = {this, 32.f / 255.f, 32.f / 255.f,
                                 32.f / 255.f, 1.f, "textbox.text"};
    animated_color disabled_text_color = {
        this, 140.f / 255.f, 140.f / 255.f, 140.f / 255.f, 1.f,
        "textbox.disabled_text"};
    animated_color placeholder_color = {
        this, 150.f / 255.f, 150.f / 255.f, 150.f / 255.f, 1.f,
        "textbox.placeholder"};
    animated_color selection_color = {this, 59.f / 255.f, 130.f / 255.f,
                                      246.f / 255.f, 100.f / 255.f,
                                      "textbox.selection"};
    animated_color caret_color = {this, 20.f / 255.f, 20.f / 255.f,
                                  20.f / 255.f, 1.f, "textbox.caret"};
    animated_color composition_underline_color = {
        this, 59.f / 255.f, 130.f / 255.f, 246.f / 255.f, 1.f,
        "textbox.composition"};

    std::function<void(std::string)> on_change;
    std::function<void()> on_focus;
    std::function<void()> on_blur;
    std::function<bool(int, bool, bool, bool, bool)> on_key_down;

    textbox_widget();
    ~textbox_widget() override;

    void render(nanovg_context ctx) override;
    void tick(float delta_time) override;
    void before_layout() override;
    void after_layout() override;
    bool has_measure() const override { return true; }
    YGSize measure(float width, YGMeasureMode width_mode, float height,
                   YGMeasureMode height_mode) override;

    void handle_mouse_down(mouse_event &e) override;
    void handle_mouse_move(mouse_event &e) override;
    void handle_mouse_up(mouse_event &e) override;
    void handle_scroll(scroll_event &e) override;
    void handle_key(key_event &e) override;
    void handle_text_input(text_input_event &e) override;
    void handle_focus_changed(bool focused) override;

    void focus();
    void blur();
    void select_all();
    void select_range(int start, int end);
    int selection_start() const;
    int selection_end() const;
    void set_selection(int start, int end);
    void insert_text(const std::string &new_text);
    void delete_text(int start, int end);
    void clear();
    void copy();
    void cut();
    void paste();

  private:
    struct pending_key_event {
        int key = 0;
        bool canceled = false;
        bool resolved = false;
    };
    struct pending_key_batch {
        std::uint64_t id = 0;
        std::uint64_t frame = 0;
        int mods = 0;
        std::u32string text_input;
        std::vector<pending_key_event> events;
    };

    int caret_index = 0;
    int selection_anchor_index = 0;
    float horizontal_scroll = 0;
    float vertical_scroll = 0;
    float caret_blink_elapsed = 0;
    bool dragging_selection = false;
    std::optional<float> preferred_caret_x;
    std::uint64_t next_pending_key_batch_id = 1;
    std::deque<pending_key_batch> pending_key_batches;
    std::optional<std::tuple<bool, float, float, float, float>> measured_key;
    std::string face_key, resolved_face;

    void clamp_indices();
    void reset_caret_blink();
    void notify_change();
    void apply_key(int key, int mods, bool &text_changed);
    void apply_text(const std::u32string &text, bool &text_changed);
    void drain_key_batches();
    void update_scroll_and_ime();
    int caret_from_point(float px, float py);
};

struct padding_widget : public widget {
    sp_anim_float padding_left = anim_float(0), padding_right = anim_float(0),
                  padding_top = anim_float(0), padding_bottom = anim_float(0);

    bool lays_out_children() const override { return true; }
    void before_layout() override;
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
    void tick(float delta_time) override;
    void handle_mouse_down(mouse_event &e) override;
};
} // namespace ui
