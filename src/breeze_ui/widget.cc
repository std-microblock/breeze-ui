#include "breeze_ui/font.h"
#include "breeze_ui/widget.h"
#include "breeze_ui/ui.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>
#include <string_view>

namespace {
YGConfigRef yoga_config() {
    static YGConfigRef config = [] {
        auto c = YGConfigNew();
        YGConfigSetPointScaleFactor(c, 0.f);
        return c;
    }();
    return config;
}

YGSize measure_trampoline(YGNodeConstRef node, float width,
                          YGMeasureMode width_mode, float height,
                          YGMeasureMode height_mode) {
    auto self = static_cast<ui::widget *>(YGNodeGetContext(node));
    return self->measure(width, width_mode, height, height_mode);
}

YGJustify to_yoga(ui::flex_widget::justify j) {
    using enum ui::flex_widget::justify;
    switch (j) {
    case end:
        return YGJustifyFlexEnd;
    case center:
        return YGJustifyCenter;
    case space_between:
        return YGJustifySpaceBetween;
    case space_around:
        return YGJustifySpaceAround;
    case space_evenly:
        return YGJustifySpaceEvenly;
    default:
        return YGJustifyFlexStart;
    }
}

YGAlign to_yoga(ui::flex_widget::align a) {
    using enum ui::flex_widget::align;
    switch (a) {
    case end:
        return YGAlignFlexEnd;
    case center:
        return YGAlignCenter;
    case stretch:
        return YGAlignStretch;
    default:
        return YGAlignFlexStart;
    }
}

const std::string &cached_font_face(ui::nanovg_context &ctx,
                                    std::string &cache_key,
                                    std::string &cache_value,
                                    std::string_view family, int weight) {
    auto key = std::format("{}#{}", family, weight);
    if (key != cache_key) {
        cache_key = std::move(key);
        cache_value = ui::resolve_font_face_name(ctx.ctx, family, weight);
        if (cache_value.empty()) {
            cache_value = std::string(family);
        }
    }
    return cache_value;
}
} // namespace

bool ui::key_event::shift() const { return mods & GLFW_MOD_SHIFT; }
bool ui::key_event::ctrl() const { return mods & GLFW_MOD_CONTROL; }
bool ui::key_event::alt() const { return mods & GLFW_MOD_ALT; }
bool ui::key_event::super() const { return mods & GLFW_MOD_SUPER; }

ui::widget::widget() {
    node = YGNodeNewWithConfig(yoga_config());
    YGNodeSetContext(node, this);
}

ui::widget::~widget() {
    if (node) {
        YGNodeFree(node);
        node = nullptr;
    }
}

bool ui::widget::has_measure() const { return false; }

YGSize ui::widget::measure(float, YGMeasureMode, float, YGMeasureMode) {
    return {user_width, user_height};
}

void ui::widget::invalidate_measure() {
    if (measure_attached && YGNodeHasMeasureFunc(node)) {
        YGNodeMarkDirty(node);
    }
    if (owner_rt) {
        owner_rt->request_frame();
    }
}

void ui::widget::apply_user_size(bool w, bool h) {
    w = w || fixed_width;
    h = h || fixed_height;
    if (!explicit_width) {
        if (w)
            YGNodeStyleSetWidth(node, user_width);
        else
            YGNodeStyleSetWidthAuto(node);
    }
    if (!explicit_height) {
        if (h)
            YGNodeStyleSetHeight(node, user_height);
        else
            YGNodeStyleSetHeightAuto(node);
    }
}

void ui::widget::before_layout() {
    if (width->dest() != applied_width)
        user_width = width->dest();
    if (height->dest() != applied_height)
        user_height = height->dest();
    YGNodeStyleSetFlexGrow(node, flex_grow);
    YGNodeStyleSetFlexShrink(node, flex_shrink);
    const bool plain = !lays_out_children() && !has_measure();
    apply_user_size(plain || manual_size, plain || manual_size);
}

bool ui::widget::attached_to_parent() const {
    return parent && parent->lays_out_children() && !is_floating &&
           !manual_position;
}

void ui::widget::sync_yoga_children() {
    std::vector<widget *> desired;
    if (lays_out_children()) {
        desired.reserve(children.size());
        for (auto &c : children)
            if (!c->manual_position)
                desired.push_back(c.get());
        if (reversed_flow())
            std::ranges::reverse(desired);
    }

    const bool want_measure = has_measure() && desired.empty();
    if (!want_measure && measure_attached) {
        YGNodeSetMeasureFunc(node, nullptr);
        measure_attached = false;
    }

    bool same = YGNodeGetChildCount(node) == desired.size();
    for (size_t i = 0; same && i < desired.size(); ++i) {
        same = YGNodeGetChild(node, i) == desired[i]->node;
    }
    if (!same) {
        YGNodeRemoveAllChildren(node);
        for (size_t i = 0; i < desired.size(); ++i) {
            auto child = desired[i]->node;
            if (auto owner = YGNodeGetOwner(child)) {
                YGNodeRemoveChild(owner, child);
            }
            YGNodeInsertChild(node, child, i);
        }
    }

    if (want_measure && !measure_attached) {
        YGNodeSetMeasureFunc(node, measure_trampoline);
        measure_attached = true;
    }
}

void ui::widget::prepare_layout_tree(render_target *rt) {
    owner_rt = rt;
    std::erase_if(children, [](auto &c) { return !c; });
    std::erase_if(floating, [](auto &c) { return !c; });
    before_layout();

    auto prepare = [&](std::shared_ptr<widget> &c, bool is_floating) {
        c->parent = this;
        c->is_floating = is_floating;
        YGNodeStyleSetDisplay(c->node,
                              c->visible ? YGDisplayFlex : YGDisplayNone);
        c->prepare_layout_tree(rt);
    };
    for (auto &c : children)
        prepare(c, false);
    for (auto &c : floating)
        prepare(c, true);
    sync_yoga_children();
    children_dirty = false;
}

bool ui::widget::axis_free(bool horizontal_axis) const {
    auto fp = dynamic_cast<const flex_widget *>(parent);
    if (!fp)
        return false;
    if (horizontal_axis == fp->horizontal)
        return fp->justify_content == flex_widget::justify::free;
    return fp->align_items == flex_widget::align::free;
}

void ui::widget::apply_layout_tree() {
    const bool attached = attached_to_parent();
    if (attached) {
        if (!axis_free(true))
            x->animate_to(YGNodeLayoutGetLeft(node));
        if (!axis_free(false))
            y->animate_to(YGNodeLayoutGetTop(node));
    }
    const bool yoga_sized = attached || lays_out_children() || has_measure();
    if (!manual_size && yoga_sized) {
        width->animate_to(YGNodeLayoutGetWidth(node));
        height->animate_to(YGNodeLayoutGetHeight(node));
        applied_width = width->dest();
        applied_height = height->dest();
    }
    YGNodeSetHasNewLayout(node, false);
    after_layout();
    for (auto list : {&children, &floating}) {
        auto snapshot = *list;
        for (auto &c : snapshot) {
            if (!c)
                continue;
            if (c->attached_to_parent())
                c->apply_layout_tree();
            else
                c->layout_detached();
        }
    }
}

void ui::widget::layout_detached(float available_width,
                                 float available_height) {
    YGNodeCalculateLayout(node, available_width, available_height,
                          YGDirectionLTR);
    apply_layout_tree();
}

void ui::widget::compute_layout_now(render_target *rt) {
    prepare_layout_tree(rt);
    layout_detached();
}

bool ui::widget::tick_tree(float delta_time, bool &animating) {
    bool changed = false;
    for (auto &anim : anim_floats) {
        anim->update(delta_time);
        changed |= anim->updated();
        animating |= anim->animating();
    }
    if (needs_repaint) {
        changed = true;
        needs_repaint = false;
    }
    dying_time.update(delta_time);
    if (dying_time) {
        animating = true;
    }
    tick(delta_time);

    auto step = [&](std::vector<std::shared_ptr<widget>> &list) {
        auto snapshot = list;
        bool removed = false;
        for (auto &c : snapshot) {
            if (!c)
                continue;
            if (c->dying_time && c->dying_time.time <= 0) {
                std::erase(list, c);
                removed = true;
                continue;
            }
            c->parent = this;
            c->owner_rt = owner_rt;
            changed |= c->tick_tree(delta_time, animating);
        }
        changed |= removed;
    };
    step(children);
    step(floating);
    return changed;
}

ui::widget *ui::widget::hit_test_tree(float px, float py) {
    if (!visible || dying_time)
        return nullptr;
    const bool inside = hit_test(px, py);
    const float lx = px - x->var(), ly = py - y->var();
    for (auto it = floating.rbegin(); it != floating.rend(); ++it) {
        if (*it)
            if (auto hit = (*it)->hit_test_tree(lx, ly))
                return hit;
    }
    if (!clips_children() || inside) {
        const float cx = lx - child_offset_x(), cy = ly - child_offset_y();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (*it)
                if (auto hit = (*it)->hit_test_tree(cx, cy))
                    return hit;
        }
    }
    return inside && hit_self ? this : nullptr;
}

bool ui::widget::hit_test(float px, float py) const {
    return px >= x->var() && px <= x->var() + width->var() &&
           py >= y->var() && py <= y->var() + height->var();
}

void ui::widget::render(nanovg_context ctx) {
    {
        auto t = ctx.transaction();
        if (clips_children())
            ctx.intersectScissor(*x, *y, *width, *height);
        render_children(
            ctx.with_offset(*x + child_offset_x(), *y + child_offset_y()),
            children);
    }
    render_children(ctx.with_offset(*x, *y), floating);
}

void ui::widget::render_children(nanovg_context ctx,
                                 std::vector<std::shared_ptr<widget>> &list) {
    for (auto &child : list) {
        if (!child || !child->visible)
            continue;
        ctx.save();
        child->render(ctx);
        ctx.restore();
    }
}

void ui::widget::add_child(std::shared_ptr<widget> child) {
    child->parent = this;
    child->owner_rt = owner_rt;
    children.push_back(std::move(child));
    children_dirty = true;
    request_repaint();
}

void ui::widget::insert_child(size_t index, std::shared_ptr<widget> child) {
    child->parent = this;
    child->owner_rt = owner_rt;
    index = std::min(index, children.size());
    children.insert(children.begin() + index, std::move(child));
    children_dirty = true;
    request_repaint();
}

void ui::widget::remove_child(std::shared_ptr<widget> child) {
    if (child->parent == this)
        child->parent = nullptr;
    std::erase(children, child);
    children_dirty = true;
    request_repaint();
}

void ui::widget::add_floating(std::shared_ptr<widget> child) {
    child->parent = this;
    child->owner_rt = owner_rt;
    child->is_floating = true;
    floating.push_back(std::move(child));
    request_repaint();
}

void ui::widget::remove_floating(std::shared_ptr<widget> child) {
    if (child->parent == this)
        child->parent = nullptr;
    std::erase(floating, child);
    request_repaint();
}

void ui::widget::request_repaint() {
    needs_repaint = true;
    if (owner_rt)
        owner_rt->request_frame();
}

void ui::widget::mark_layout_dirty() {
    if (owner_rt)
        owner_rt->request_frame();
}

#define BREEZE_STYLE_SETTER(name, flag, call)                                  \
    void ui::widget::name(float v) {                                           \
        flag;                                                                  \
        call;                                                                  \
        mark_layout_dirty();                                                   \
    }
BREEZE_STYLE_SETTER(set_width, explicit_width = true,
                    YGNodeStyleSetWidth(node, v))
BREEZE_STYLE_SETTER(set_width_percent, explicit_width = true,
                    YGNodeStyleSetWidthPercent(node, v))
BREEZE_STYLE_SETTER(set_height, explicit_height = true,
                    YGNodeStyleSetHeight(node, v))
BREEZE_STYLE_SETTER(set_height_percent, explicit_height = true,
                    YGNodeStyleSetHeightPercent(node, v))
BREEZE_STYLE_SETTER(set_min_width, , YGNodeStyleSetMinWidth(node, v))
BREEZE_STYLE_SETTER(set_min_height, , YGNodeStyleSetMinHeight(node, v))
BREEZE_STYLE_SETTER(set_max_width, , YGNodeStyleSetMaxWidth(node, v))
BREEZE_STYLE_SETTER(set_max_height, , YGNodeStyleSetMaxHeight(node, v))
BREEZE_STYLE_SETTER(set_flex_basis, , YGNodeStyleSetFlexBasis(node, v))
#undef BREEZE_STYLE_SETTER

void ui::widget::set_width_auto() {
    explicit_width = false;
    YGNodeStyleSetWidthAuto(node);
    mark_layout_dirty();
}
void ui::widget::set_height_auto() {
    explicit_height = false;
    YGNodeStyleSetHeightAuto(node);
    mark_layout_dirty();
}
void ui::widget::set_margin(YGEdge edge, float v) {
    YGNodeStyleSetMargin(node, edge, v);
    mark_layout_dirty();
}
void ui::widget::set_align_self(YGAlign align) {
    YGNodeStyleSetAlignSelf(node, align);
    mark_layout_dirty();
}

float ui::widget::offset_x() const {
    if (!parent)
        return 0;
    return parent->abs_x() + (is_floating ? 0 : parent->child_offset_x());
}

float ui::widget::offset_y() const {
    if (!parent)
        return 0;
    return parent->abs_y() + (is_floating ? 0 : parent->child_offset_y());
}

ui::nanovg_context ui::widget::measure_context() const {
    return nanovg_context{owner_rt ? owner_rt->nvg : nullptr, owner_rt};
}

ui::text_measure_scope::text_measure_scope(const widget &w)
    : vg(w.measure_context()) {
    if (!vg.ctx)
        return;
    vg.save();
    vg.resetTransform();
    vg.resetScissor();
    const float scale = vg.rt ? vg.rt->dpi_scale : 1.f;
    vg.scale(scale, scale);
}

ui::text_measure_scope::~text_measure_scope() {
    if (vg.ctx)
        vg.restore();
}

bool ui::widget::hovered() const {
    return owner_rt && owner_rt->is_hovered(this);
}

bool ui::widget::pressed() const {
    return owner_rt && owner_rt->mouse_down && hovered();
}

bool ui::widget::focused() {
    return owner_rt && owner_rt->focused_widget &&
           !owner_rt->focused_widget->expired() &&
           owner_rt->focused_widget->lock().get() == this;
}

bool ui::widget::focus_within() {
    return focused() || std::ranges::any_of(children, [](const auto &child) {
               return child && child->focus_within();
           });
}

void ui::widget::set_focus(bool focused) {
    if (!owner_rt)
        return;
    if (focused) {
        owner_rt->focused_widget = this->weak_from_this();
    } else if (owner_rt->focused_widget &&
               owner_rt->focused_widget->lock().get() == this) {
        owner_rt->focused_widget.reset();
    }
    owner_rt->request_frame();
}

void ui::flex_widget::before_layout() {
    widget::before_layout();
    YGNodeStyleSetFlexDirection(node, horizontal ? YGFlexDirectionRow
                                                 : YGFlexDirectionColumn);
    YGNodeStyleSetJustifyContent(node, to_yoga(justify_content));
    YGNodeStyleSetAlignItems(node, to_yoga(align_items));
    YGNodeStyleSetGap(node, YGGutterAll, gap);
    YGNodeStyleSetPadding(node, YGEdgeLeft, *padding_left);
    YGNodeStyleSetPadding(node, YGEdgeRight, *padding_right);
    YGNodeStyleSetPadding(node, YGEdgeTop, *padding_top);
    YGNodeStyleSetPadding(node, YGEdgeBottom, *padding_bottom);
    YGNodeStyleSetMaxHeight(node,
                            std::isfinite(max_height) ? max_height : YGUndefined);
    YGNodeStyleSetOverflow(node, enable_scrolling ? YGOverflowScroll
                                 : crop_overflow  ? YGOverflowHidden
                                                  : YGOverflowVisible);
    apply_user_size(!auto_size || manual_size, !auto_size || manual_size);
}

float ui::flex_widget::max_scroll() const {
    return std::max(0.f, actual_height - height->dest());
}

void ui::flex_widget::after_layout() {
    float content_bottom = 0;
    for (auto &c : children) {
        if (!c || !c->visible)
            continue;
        content_bottom =
            std::max(content_bottom, YGNodeLayoutGetTop(c->node) +
                                         YGNodeLayoutGetHeight(c->node));
    }
    actual_height =
        content_bottom + YGNodeLayoutGetPadding(node, YGEdgeBottom);
    if (!enable_scrolling) {
        if (scroll_top->dest() != 0)
            scroll_top->reset_to(0);
        return;
    }
    auto clamped = std::clamp(scroll_top->dest(), -max_scroll(), 0.f);
    if (clamped != scroll_top->dest())
        scroll_top->animate_to(clamped);
}

void ui::flex_widget::handle_scroll(scroll_event &e) {
    if (!enable_scrolling || max_scroll() <= 0)
        return;
    scroll_top->animate_to(
        std::clamp(scroll_top->dest() + e.delta * 100, -max_scroll(), 0.f));
    e.handled = true;
    request_repaint();
}

void ui::flex_widget::render(nanovg_context ctx) {
    widget::render(ctx);
    render_scrollbar(ctx);
}

void ui::flex_widget::render_scrollbar(nanovg_context &ctx) {
    if (!enable_scrolling || actual_height <= height->dest())
        return;
    auto h = height->dest();
    auto scrollbar_height = h * h / actual_height;
    auto scrollbar_x = width->dest() - scroll_bar_width - 2 + *x;
    auto scrollbar_y =
        *y - *scroll_top / (actual_height - h) * (h - scrollbar_height);
    ctx.fillColor(scroll_bar_color);
    ctx.fillRoundedRect(scrollbar_x, scrollbar_y, scroll_bar_width,
                        scrollbar_height, scroll_bar_radius);
}

bool ui::flex_widget::reversed_flow() const { return reverse; }

void ui::flex_widget::spacer::before_layout() {
    widget::before_layout();
    apply_user_size(false, false);
    YGNodeStyleSetFlexGrow(node, size);
    YGNodeStyleSetFlexBasis(node, 0);
}

void ui::padding_widget::before_layout() {
    widget::before_layout();
    YGNodeStyleSetFlexDirection(node, YGFlexDirectionColumn);
    YGNodeStyleSetAlignItems(node, YGAlignFlexStart);
    YGNodeStyleSetPadding(node, YGEdgeLeft, *padding_left);
    YGNodeStyleSetPadding(node, YGEdgeRight, *padding_right);
    YGNodeStyleSetPadding(node, YGEdgeTop, *padding_top);
    YGNodeStyleSetPadding(node, YGEdgeBottom, *padding_bottom);
}

const std::string &ui::text_widget::face(nanovg_context &ctx) {
    return cached_font_face(ctx, face_key, resolved_face, font_family,
                            font_weight);
}

void ui::text_widget::before_layout() {
    widget::before_layout();
    apply_user_size(!shrink_horizontal, !shrink_vertical);
    measure_key key{text, font_size, font_weight, font_family, max_width};
    if (last_key != key) {
        last_key = std::move(key);
        invalidate_measure();
    }
}

YGSize ui::text_widget::measure(float w, YGMeasureMode wm, float,
                                YGMeasureMode) {
    text_measure_scope scope(*this);
    if (!scope)
        return {0, 0};
    auto &vg = scope.vg;
    vg.fontSize(font_size);
    vg.fontFace(face(vg).c_str());
    vg.textAlign(NVG_ALIGN_TOP | NVG_ALIGN_LEFT);
    float ascender = 0, descender = 0, line_height = 0;
    vg.textMetrics(&ascender, &descender, &line_height);
    const float line_box = ascender - descender;
    const float nw = vg.measureText(text.c_str()).first;
    natural_width = nw;

    float wrap = max_width > 0 ? max_width : 0;
    if (wm == YGMeasureModeExactly ||
        (wm == YGMeasureModeAtMost && max_width > 0)) {
        if (wrap <= 0 || w < wrap)
            wrap = w;
    }
    if (wrap > 0 && nw > wrap) {
        const char *cursor = text.c_str();
        const char *end = cursor + text.size();
        NVGtextRow rows[16];
        int count = 0;
        float widest = 0;
        while (cursor < end) {
            int n = vg.textBreakLines(cursor, end, wrap, rows, 16);
            if (n <= 0)
                break;
            for (int i = 0; i < n; ++i)
                widest = std::max(widest, rows[i].width);
            count += n;
            cursor = rows[n - 1].next;
        }
        count = std::max(count, 1);
        return {std::min(widest, wrap),
                (count - 1) * line_height + line_box};
    }
    return {nw, line_box};
}

void ui::text_widget::render(nanovg_context ctx) {
    ctx.fontSize(font_size);
    ctx.fillColor(color.nvg());
    ctx.textAlign(NVG_ALIGN_TOP | NVG_ALIGN_LEFT);
    ctx.fontFace(face(ctx).c_str());

    if (natural_width > width->var() + 0.5f && width->var() > 0) {
        ctx.textBox(*x, *y, width->var(), text.c_str(), nullptr);
    } else {
        ctx.text(*x, *y, text.c_str(), nullptr);
    }
    widget::render(ctx);
}

ui::button_widget::button_widget(const std::string &button_text)
    : button_widget() {
    auto text = emplace_child<ui::text_widget>();
    text->text = button_text;
    text->font_size = 14;
    text->color.reset_to({1, 1, 1, 0.95});
}

ui::button_widget::button_widget() {
    padding_bottom->reset_to(10);
    padding_top->reset_to(10);
    padding_left->reset_to(22);
    padding_right->reset_to(20);

    border_top.reset_to({1, 1, 1, 0.12});
    border_right.reset_to({1, 1, 1, 0.04});
    border_bottom.reset_to({1, 1, 1, 0.02});
    border_left.reset_to({1, 1, 1, 0.04});
}

void ui::button_widget::render(ui::nanovg_context ctx) {
    ctx.fillColor(bg_color);
    ctx.fillRoundedRect(*x, *y, *width, *height, 6);

    float bw = 1.0f;
    float radius = 6.0f;
    auto edge = [&](NVGcolor color, float x1, float y1, float x2, float y2) {
        ctx.beginPath();
        ctx.strokeWidth(bw);
        ctx.strokeColor(color);
        ctx.moveTo(x1, y1);
        ctx.lineTo(x2, y2);
        ctx.stroke();
    };
    auto corner = [&](NVGcolor color, float sx, float sy, float x1, float y1,
                      float x2, float y2) {
        ctx.beginPath();
        ctx.strokeWidth(bw);
        ctx.strokeColor(color);
        ctx.moveTo(sx, sy);
        ctx.arcTo(x1, y1, x2, y2, radius - bw / 2);
        ctx.stroke();
    };
    const float l = *x, t = *y, r = *x + *width, b = *y + *height;
    edge(border_top, l + radius, t + bw / 2, r - radius, t + bw / 2);
    edge(border_right, r - bw / 2, t + radius, r - bw / 2, b - radius);
    edge(border_bottom, r - radius, b - bw / 2, l + radius, b - bw / 2);
    edge(border_left, l + bw / 2, b - radius, l + bw / 2, t + radius);
    corner(border_right.blend(border_top), r - radius, t + bw / 2, r - bw / 2,
           t + bw / 2, r - bw / 2, t + radius);
    corner(border_bottom.blend(border_right), r - bw / 2, b - radius,
           r - bw / 2, b - bw / 2, r - radius, b - bw / 2);
    corner(border_left.blend(border_bottom), l + radius, b - bw / 2, l + bw / 2,
           b - bw / 2, l + bw / 2, b - radius);
    corner(border_top.blend(border_left), l + bw / 2, t + radius, l + bw / 2,
           t + bw / 2, l + radius, t + bw / 2);

    padding_widget::render(ctx);
}

void ui::button_widget::update_colors(bool is_active, bool is_hovered) {
    if (is_active) {
        bg_color.animate_to({0.3, 0.3, 0.3, 0.7});
    } else if (is_hovered) {
        bg_color.animate_to({0.35, 0.35, 0.35, 0.7});
    } else {
        bg_color.animate_to({0.3, 0.3, 0.3, 0.6});
    }
}

void ui::button_widget::on_click() {}

void ui::button_widget::tick(float) { update_colors(pressed(), hovered()); }

void ui::button_widget::handle_mouse_down(mouse_event &e) {
    if (e.button != mouse_button::left)
        return;
    e.handled = true;
    on_click();
}
