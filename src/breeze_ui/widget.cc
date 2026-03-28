#include "breeze_ui/widget.h"
#include "breeze_ui/ui.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <print>
#include <ranges>
#include <string_view>

#include "simdutf.h"

namespace {
struct utf8_index_map {
    std::vector<int> byte_offsets = {0};

    [[nodiscard]] int char_count() const {
        return static_cast<int>(byte_offsets.size()) - 1;
    }
};

struct text_row_layout {
    int start = 0;
    int end = 0;
    float y = 0;
    float width = 0;
    bool soft_wrap_to_next = false;
    std::vector<float> caret_xs = {0};
};

struct textbox_layout {
    std::vector<text_row_layout> rows;
    float ascender = 0;
    float descender = 0;
    float line_height = 0;
    float content_width = 0;
    float content_height = 0;
};

struct textbox_visual_state {
    std::string text;
    utf8_index_map map;
    int selection_start = 0;
    int selection_end = 0;
    int caret_index = 0;
    int composition_start = -1;
    int composition_end = -1;
};

std::u32string utf32_from_utf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const auto expected = simdutf::utf32_length_from_utf8(text.data(), text.size());
    std::u32string result(expected, U'\0');
    const auto written =
        simdutf::convert_utf8_to_utf32(text.data(), text.size(), result.data());
    result.resize(written);
    return result;
}

utf8_index_map build_utf8_index_map(std::string_view text) {
    utf8_index_map map;
    const auto utf32 = utf32_from_utf8(text);
    map.byte_offsets.reserve(utf32.size() + 1);

    size_t byte_offset = 0;
    for (char32_t cp : utf32) {
        byte_offset += simdutf::utf8_length_from_utf32(&cp, 1);
        map.byte_offsets.push_back(static_cast<int>(byte_offset));
    }

    if (utf32.empty() && !text.empty()) {
        map.byte_offsets.resize(text.size() + 1);
        for (size_t i = 1; i <= text.size(); ++i) {
            map.byte_offsets[i] = static_cast<int>(i);
        }
    }
    return map;
}

int clamp_char_index(const utf8_index_map &map, int index) {
    return std::clamp(index, 0, map.char_count());
}

size_t byte_offset_for_char(const utf8_index_map &map, int index) {
    return static_cast<size_t>(map.byte_offsets[clamp_char_index(map, index)]);
}

int char_index_for_byte(const utf8_index_map &map, int byte_offset) {
    auto it = std::lower_bound(map.byte_offsets.begin(), map.byte_offsets.end(),
                               byte_offset);
    if (it == map.byte_offsets.end()) {
        return map.char_count();
    }
    return static_cast<int>(it - map.byte_offsets.begin());
}

std::string utf8_substr_chars(std::string_view text, const utf8_index_map &map,
                              int start, int end) {
    auto start_byte = byte_offset_for_char(map, start);
    auto end_byte = byte_offset_for_char(map, end);
    if (end_byte < start_byte) {
        std::swap(start_byte, end_byte);
    }
    return std::string(text.substr(start_byte, end_byte - start_byte));
}

std::string utf8_from_codepoints(const std::u32string &text) {
    if (text.empty()) {
        return {};
    }

    const auto expected = simdutf::utf8_length_from_utf32(text.data(), text.size());
    std::string result(expected, '\0');
    const auto written =
        simdutf::convert_utf32_to_utf8(text.data(), text.size(), result.data());
    result.resize(written);
    return result;
}

std::string normalize_text_for_textbox(std::string text, bool multiline) {
    std::string normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            normalized.push_back(multiline ? '\n' : ' ');
            continue;
        }
        if (text[i] == '\n' && !multiline) {
            normalized.push_back(' ');
            continue;
        }
        normalized.push_back(text[i]);
    }
    return normalized;
}

text_row_layout make_text_row_layout(ui::nanovg_context &vg,
                                     std::string_view full_text,
                                     const utf8_index_map &full_map,
                                     int start_index, int end_index,
                                     float y_offset, bool soft_wrap_to_next) {
    text_row_layout row;
    row.start = start_index;
    row.end = end_index;
    row.y = y_offset;
    row.soft_wrap_to_next = soft_wrap_to_next;

    const auto row_text = utf8_substr_chars(full_text, full_map, start_index,
                                            end_index);
    const auto row_map = build_utf8_index_map(row_text);
    row.caret_xs.assign(static_cast<size_t>(row_map.char_count()) + 1,
                        std::numeric_limits<float>::quiet_NaN());
    row.caret_xs[0] = 0.0f;

    if (!row_text.empty()) {
        std::string probe_text = row_text;
        probe_text.push_back('|');
        std::vector<NVGglyphPosition> glyphs(probe_text.size() + 1);
        int glyph_count = vg.textGlyphPositions(0, 0, probe_text.c_str(),
                                                nullptr, glyphs.data(),
                                                static_cast<int>(glyphs.size()));
        bool width_from_probe = false;

        for (int i = 0; i < glyph_count; ++i) {
            auto local_byte =
                static_cast<int>(glyphs[i].str - probe_text.c_str());
            if (local_byte >= static_cast<int>(row_text.size())) {
                row.width = glyphs[i].x;
                width_from_probe = true;
                break;
            }
            auto local_char = char_index_for_byte(row_map, local_byte);
            if (local_char >= 0 && local_char < row_map.char_count()) {
                row.caret_xs[static_cast<size_t>(local_char)] = glyphs[i].x;
            }
        }

        if (!width_from_probe) {
            row.width = glyph_count > 0 ? glyphs[glyph_count - 1].maxx
                                        : vg.measureText(row_text.c_str()).first;
        }
    }

    for (size_t i = 0; i < row.caret_xs.size(); ++i) {
        if (std::isnan(row.caret_xs[i])) {
            const auto prefix = utf8_substr_chars(row_text, row_map, 0,
                                                  static_cast<int>(i));
            row.caret_xs[i] =
                prefix.empty() ? 0.0f : vg.measureText(prefix.c_str()).first;
        }
    }
    row.caret_xs.back() = std::max(row.caret_xs.back(), row.width);
    for (size_t i = 1; i < row.caret_xs.size(); ++i) {
        row.caret_xs[i] = std::max(row.caret_xs[i], row.caret_xs[i - 1]);
    }

    return row;
}

textbox_visual_state make_textbox_visual_state(
    const std::string &text, int selection_start, int selection_end,
    int caret_index, bool multiline,
    const ui::ime_composition_state *composition = nullptr) {
    textbox_visual_state state;
    state.text = normalize_text_for_textbox(text, multiline);
    state.map = build_utf8_index_map(state.text);
    state.selection_start = selection_start;
    state.selection_end = selection_end;
    state.caret_index = caret_index;

    if (!composition || !composition->active) {
        return state;
    }

    const auto composition_text =
        normalize_text_for_textbox(utf8_from_codepoints(composition->text),
                                   multiline);
    const auto composition_map = build_utf8_index_map(composition_text);
    const auto replace_start =
        std::min(state.selection_start, state.selection_end);
    const auto replace_end = std::max(state.selection_start, state.selection_end);

    const auto start_byte = byte_offset_for_char(state.map, replace_start);
    const auto end_byte = byte_offset_for_char(state.map, replace_end);
    state.text.replace(start_byte, end_byte - start_byte, composition_text);
    state.map = build_utf8_index_map(state.text);
    state.selection_start = replace_start;
    state.selection_end = replace_start;
    state.composition_start = replace_start;
    state.composition_end =
        replace_start + composition_map.char_count();
    state.caret_index =
        replace_start +
        std::clamp(composition->cursor, 0, composition_map.char_count());
    return state;
}

textbox_layout build_textbox_layout(ui::nanovg_context &vg,
                                    std::string_view text, float font_size,
                                    bool multiline, float inner_width,
                                    float line_height_multiplier) {
    textbox_layout layout;

    vg.fontSize(font_size);
    vg.fontFace("main");
    vg.textAlign(NVG_ALIGN_TOP | NVG_ALIGN_LEFT);
    vg.textMetrics(&layout.ascender, &layout.descender, &layout.line_height);
    layout.line_height =
        std::max(layout.line_height * std::max(line_height_multiplier, 0.1f),
                 1.0f);

    const auto map = build_utf8_index_map(text);
    float y_offset = 0.0f;

    auto push_row = [&](int start, int end, bool soft_wrap_to_next = false) {
        auto row = make_text_row_layout(vg, text, map, start, end, y_offset,
                                        soft_wrap_to_next);
        layout.content_width = std::max(layout.content_width, row.width);
        layout.rows.push_back(std::move(row));
        y_offset += layout.line_height;
    };

    if (multiline) {
        const float wrap_width = std::max(inner_width, 1.0f);
        int line_start = 0;

        for (int i = 0; i <= map.char_count(); ++i) {
            const bool is_end = i == map.char_count();
            const bool is_newline =
                !is_end && text[byte_offset_for_char(map, i)] == '\n';
            if (!is_end && !is_newline) {
                continue;
            }

            const int line_end = i;
            const auto line_text =
                utf8_substr_chars(text, map, line_start, line_end);
            const auto line_map = build_utf8_index_map(line_text);

            if (line_text.empty()) {
                push_row(line_start, line_end);
            } else {
                const char *cursor = line_text.c_str();
                const char *end = cursor + line_text.size();
                while (cursor < end) {
                    NVGtextRow row_data[1];
                    int row_count =
                        vg.textBreakLines(cursor, end, wrap_width, row_data, 1);
                    if (row_count <= 0 || row_data[0].start >= row_data[0].end) {
                        break;
                    }

                    const auto row_start_byte =
                        static_cast<int>(row_data[0].start - line_text.c_str());
                    const auto row_end_byte =
                        static_cast<int>(row_data[0].end - line_text.c_str());
                    const auto row_next_byte =
                        static_cast<int>(row_data[0].next - line_text.c_str());
                    const bool soft_wrap_to_next = row_data[0].next < end;

                    push_row(line_start + char_index_for_byte(line_map,
                                                              row_start_byte),
                             line_start +
                                 char_index_for_byte(line_map, row_next_byte),
                             soft_wrap_to_next);
                    cursor = row_data[0].next;
                }
            }

            line_start = i + 1;
        }
    } else {
        push_row(0, map.char_count());
    }

    if (layout.rows.empty()) {
        push_row(0, 0);
    }

    layout.content_height = std::max(y_offset, layout.line_height);
    return layout;
}

int find_row_for_index(const textbox_layout &layout, int char_index) {
    if (layout.rows.empty()) {
        return 0;
    }
    for (int i = 0; i < static_cast<int>(layout.rows.size()); ++i) {
        const auto &row = layout.rows[static_cast<size_t>(i)];
        if (char_index < row.start) {
            return i;
        }
        if (char_index == row.end &&
            i + 1 < static_cast<int>(layout.rows.size()) &&
            layout.rows[static_cast<size_t>(i + 1)].start == row.end &&
            row.soft_wrap_to_next) {
            return i + 1;
        }
        if (char_index <= row.end) {
            return i;
        }
    }
    return static_cast<int>(layout.rows.size()) - 1;
}

float caret_x_for_index(const text_row_layout &row, int char_index) {
    auto local_index = std::clamp(char_index - row.start, 0, row.end - row.start);
    return row.caret_xs[static_cast<size_t>(local_index)];
}

int caret_index_from_x(const text_row_layout &row, float x) {
    if (row.caret_xs.empty()) {
        return row.start;
    }
    for (int i = 0; i < row.end - row.start; ++i) {
        const float left = row.caret_xs[static_cast<size_t>(i)];
        const float right = row.caret_xs[static_cast<size_t>(i + 1)];
        if (x < (left + right) * 0.5f) {
            return row.start + i;
        }
    }
    return row.end;
}

int caret_index_from_point(const textbox_layout &layout, float x, float y) {
    if (layout.rows.empty()) {
        return 0;
    }

    const auto row_index = std::clamp(static_cast<int>(
                                          std::floor(y / layout.line_height)),
                                      0,
                                      static_cast<int>(layout.rows.size()) - 1);
    return caret_index_from_x(layout.rows[static_cast<size_t>(row_index)], x);
}

std::string selected_text(const ui::textbox_widget &widget) {
    const auto map = build_utf8_index_map(widget.text);
    const auto start = std::min(widget.selection_start(), widget.selection_end());
    const auto end = std::max(widget.selection_start(), widget.selection_end());
    return utf8_substr_chars(widget.text, map, start, end);
}
} // namespace

void ui::widget::update_child_basic(update_context &ctx,
                                    std::shared_ptr<widget> &w) {
    if (!w)
        return;
    // handle dying time
    if (w->dying_time && w->dying_time.time <= 0) {
        w = nullptr;
        return;
    }
    w->parent = this;
    w->update(ctx);
}

void ui::widget::render_child_basic(nanovg_context ctx,
                                    std::shared_ptr<widget> &w) {
    if (!w)
        return;

    auto can_render_width =
             enable_child_clipping
                 ? std::max(std::min(**w->width, **width - *w->x), 0.f)
                 : INFINITY,
         can_render_height =
             enable_child_clipping
                 ? std::max(std::min(**w->height, **height - *w->y), 0.f)
                 : INFINITY;

    if (can_render_width > 0 && can_render_height > 0) {
        ctx.save();
        w->render(ctx);
        ctx.restore();
    }
}

void ui::widget::render(nanovg_context ctx) {
    if constexpr (false)
        if (_debug_offset_cache[0] != ctx.offset_x ||
            _debug_offset_cache[1] != ctx.offset_y) {
            if (_debug_offset_cache[0] != -1 || _debug_offset_cache[1] != -1) {
                std::println(
                    "[Warning] The offset during render is different from the "
                    "offset during update: (update) {} {} vs (render) {} {} "
                    "({}, {})",
                    _debug_offset_cache[0], _debug_offset_cache[1],
                    ctx.offset_x, ctx.offset_y, (void *)this,
                    typeid(*this).name());
            } else {
                std::println("[Warning] The update function is not called "
                             "before render: {}",
                             (void *)this);
            }
        }

    _debug_offset_cache[0] = -1;
    _debug_offset_cache[1] = -1;

    render_children(ctx.with_offset(*x, *y), children);
}
void ui::widget::update(update_context &ctx) {
    children_dirty = false;
    owner_rt = &ctx.rt;
    for (auto anim : anim_floats) {
        anim->update(ctx.delta_time);

        if (anim->updated()) {
            ctx.need_repaint = true;
        }
    }

    if (this->needs_repaint) {
        ctx.need_repaint = true;
        this->needs_repaint = false;
    }

    dying_time.update(ctx.delta_time);
    update_context upd = ctx.with_offset(*x, *y);
    update_children(upd, children);
    if constexpr (false)
        if (_debug_offset_cache[0] != -1 || _debug_offset_cache[1] != -1) {
            std::println(
                "[Warning] The update function is called twice with different "
                "offsets: {} {} vs {} {} ({})",
                _debug_offset_cache[0], _debug_offset_cache[1], ctx.offset_x,
                ctx.offset_y, (void *)this);
        }
    _debug_offset_cache[0] = ctx.offset_x;
    _debug_offset_cache[1] = ctx.offset_y;

    last_offset_x = ctx.offset_x;
    last_offset_y = ctx.offset_y;
}
void ui::widget::add_child(std::shared_ptr<widget> child) {
    children.push_back(std::move(child));
    children_dirty = true;
    needs_repaint = true;
}

bool ui::update_context::hovered(widget *w, bool hittest) const {
    auto hit = w->check_hit(*this);
    if (!hit)
        return false;

    if (hittest) {
        if (!hovered_widgets->empty()) {
            // iterate through parent chain
            auto p = w;
            while (p) {
                if (std::ranges::contains(*hovered_widgets, p)) {
                    return true;
                }

                p = p->parent;
            }
            return false;
        }
    }

    return true;
}
float ui::widget::measure_height(update_context &ctx) { return height->dest(); }
float ui::widget::measure_width(update_context &ctx) { return width->dest(); }
void ui::flex_widget::update(update_context &ctx) {
    if (ctx.hovered(this) && enable_scrolling) {
        scroll_top->animate_to(
            std::clamp(scroll_top->dest() + ctx.scroll_y * 100,
                       height->dest() - actual_height, 0.f));
    }

    auto forkctx2 = ctx.with_offset(*x, *y + *scroll_top);
    reposition_children_flex(forkctx2, children);

    auto forkctx = ctx.with_offset(0, *scroll_top);
    widget::update(forkctx);

    actual_height = height->dest();
    if (max_height < actual_height)
        height->reset_to(max_height);
}
void ui::flex_widget::render(nanovg_context ctx) {
    auto t = ctx.transaction();
    if (crop_overflow || enable_scrolling)
        ctx.scissor(*x, *y, *width, *height);
    widget::render(ctx.with_offset(0, *scroll_top));

    if (enable_scrolling && actual_height > height->dest()) {
        auto scrollbar_height = height->dest() * height->dest() / actual_height;
        auto scrollbar_x = width->dest() - scroll_bar_width - 2 + *x;
        auto scrollbar_y = *y - *scroll_top / (actual_height - height->dest()) *
                                    (height->dest() - scrollbar_height);

        ctx.fillColor(scroll_bar_color);
        ctx.fillRoundedRect(scrollbar_x, scrollbar_y, scroll_bar_width,
                            scrollbar_height, scroll_bar_radius);
    }
}
void ui::flex_widget::reposition_children_flex(
    update_context &ctx, std::vector<std::shared_ptr<widget>> &children) {
    float x = *padding_left, y = *padding_top;

    constexpr bool floor_position = true;
    auto r = [](float value) {
        return floor_position ? std::floor(value) : value;
    };

    auto children_rev =
        reverse ? children | std::views::reverse |
                      std::ranges::to<std::vector<std::shared_ptr<widget>>>()
                : children;

    auto spacer_count =
        std::ranges::count_if(children_rev, [](const auto &child) {
            return dynamic_cast<const spacer *>(child.get());
        });

    std::vector<std::pair<float, float>> measure_cache;
    measure_cache.reserve(children_rev.size());

    // Pass 1: Measure all children and calculate total flex grow/shrink
    float max_child_width = 0, max_child_height = 0;
    float total_fixed_size = 0;
    float total_flex_grow = 0.0f;
    float total_flex_shrink = 0.0f;

    for (auto &child : children_rev) {
        // Set max_width for text_widget before measuring to get correct size
        if (!horizontal && align_items == align::stretch) {
            float container_width =
                width->dest() - *padding_left - *padding_right;
            if (auto tw = dynamic_cast<text_widget *>(child.get())) {
                tw->max_width = container_width;
            }
        }

        float child_width = child->measure_width(ctx);
        float child_height = child->measure_height(ctx);
        measure_cache.emplace_back(child_width, child_height);

        if (horizontal) {
            max_child_height = std::max(max_child_height, child_height);
            if (!dynamic_cast<const spacer *>(child.get())) {
                total_fixed_size += child_width;
                total_flex_grow += child->flex_grow;
                total_flex_shrink += child->flex_shrink;
            }
        } else {
            max_child_width = std::max(max_child_width, child_width);
            if (!dynamic_cast<const spacer *>(child.get())) {
                total_fixed_size += child_height;
                total_flex_grow += child->flex_grow;
                total_flex_shrink += child->flex_shrink;
            }
        }
    }

    float gap_space = (children_rev.size() - 1) * gap;
    // Calculate spacer size if needed
    float spacer_size = 0;
    if (spacer_count > 0 && !should_autosize(true)) {
        float available_space =
            horizontal ? (width->dest() - *padding_left - *padding_right)
                       : (height->dest() - *padding_top - *padding_bottom);

        spacer_size =
            std::max(0.0f, (available_space - total_fixed_size - gap_space) /
                               spacer_count);
    }

    // Calculate total content size (including gaps and spacers)
    float total_content_size =
        total_fixed_size + gap_space + spacer_count * spacer_size;

    // Set container dimensions if auto_size enabled
    // should_autosize decides if we should auto size in the main axis
    // When horizontal is true, the main axis is width side, so we check if we
    // should auto size width by passing in true When horizontal is false, the
    // main axis is not width side, so we check if we should auto size width by
    // passing in false
    if (should_autosize(horizontal)) {
        width->animate_to(
            r((horizontal ? total_content_size : max_child_width) +
              *padding_left + *padding_right));
    }

    if (should_autosize(!horizontal)) {
        height->animate_to(
            r((horizontal ? max_child_height : total_content_size) +
              *padding_top + *padding_bottom));
    }

    // Get final container dimensions for layout
    float container_width = width->dest() - *padding_left - *padding_right;
    float container_height = height->dest() - *padding_top - *padding_bottom;

    // Pass 2: Apply align-items to children
    for (size_t i = 0; i < children_rev.size(); ++i) {
        auto &child = children_rev[i];
        if (dynamic_cast<const spacer *>(child.get())) {
            if (horizontal) {
                child->width->animate_to(spacer_size);
            } else {
                child->height->animate_to(spacer_size);
            }
            continue;
        }

        auto [cached_width, cached_height] = measure_cache[i];
        if (horizontal) {
            switch (align_items) {
            case align::center:
                child->y->animate_to(
                    r(y + (container_height - cached_height) / 2));
                break;
            case align::end:
                child->y->animate_to(r(y + container_height - cached_height));
                break;
            case align::stretch:
                child->height->animate_to(r(container_height));
                child->y->animate_to(r(y));
                break;
            case align::start:
                child->y->animate_to(r(y));
                break;
            default:
                break;
            }
        } else {
            switch (align_items) {
            case align::center:
                child->x->animate_to(
                    r(x + (container_width - cached_width) / 2));
                break;
            case align::end:
                child->x->animate_to(r(x + container_width - cached_width));
                break;
            case align::stretch:
                child->width->animate_to(r(container_width));
                child->x->animate_to(r(x));
                break;
            case align::start:
                child->x->animate_to(r(x));
                break;
            default:
                break;
            }
        }
    }

    // Pass 3: Apply justify-content and position children
    float remaining_space =
        (horizontal ? container_width : container_height) - total_content_size;
    float initial_offset = 0;
    float effective_gap = gap;

    switch (justify_content) {
    case justify::end:
        initial_offset = remaining_space;
        break;
    case justify::center:
        initial_offset = remaining_space / 2;
        break;
    case justify::space_between:
        if (children.size() > 1) {
            effective_gap += remaining_space / (children.size() - 1);
        }
        break;
    case justify::space_around:
        effective_gap += remaining_space / children.size();
        initial_offset = effective_gap / 2;
        break;
    case justify::space_evenly:
        effective_gap += remaining_space / (children.size() + 1);
        initial_offset = effective_gap;
        break;
    case justify::start:
    default:
        break;
    }

    // Calculate space to distribute among flex-growing/shrinking children
    float flex_space_to_distribute = 0.0f;
    if (remaining_space > 0.0f && total_flex_grow > 0.0f) {
        flex_space_to_distribute = remaining_space;
    } else if (remaining_space < 0.0f && total_flex_shrink > 0.0f) {
        flex_space_to_distribute = remaining_space; // negative
    }

    if (horizontal) {
        x += initial_offset;
        for (size_t i = 0; i < children_rev.size(); ++i) {
            auto &child = children_rev[i];
            child->x->animate_to(r(x));

            float child_base_size = dynamic_cast<const spacer *>(child.get())
                                        ? spacer_size
                                        : measure_cache[i].first;

            // Apply flex grow/shrink if this child can flex
            float flex_extra = 0.0f;
            if (flex_space_to_distribute != 0.0f) {
                float flex_factor = 0.0f;
                float total_flex = 0.0f;
                if (flex_space_to_distribute > 0.0f && total_flex_grow > 0.0f) {
                    flex_factor = child->flex_grow;
                    total_flex = total_flex_grow;
                } else if (flex_space_to_distribute < 0.0f &&
                           total_flex_shrink > 0.0f) {
                    flex_factor = child->flex_shrink;
                    total_flex = total_flex_shrink;
                }
                if (flex_factor > 0.0f && total_flex > 0.0f) {
                    flex_extra =
                        (flex_factor / total_flex) * flex_space_to_distribute;
                    if (horizontal) {
                        child->width->animate_to(
                            r(child_base_size + flex_extra));
                    }
                }
            }

            float child_size = child_base_size + flex_extra;
            x += child_size + effective_gap;
        }
    } else {
        y += initial_offset;
        for (size_t i = 0; i < children_rev.size(); ++i) {
            auto &child = children_rev[i];
            child->y->animate_to(r(y));

            float child_base_size = dynamic_cast<const spacer *>(child.get())
                                        ? spacer_size
                                        : measure_cache[i].second;

            // Apply flex grow/shrink if this child can flex
            float flex_extra = 0.0f;
            if (flex_space_to_distribute != 0.0f) {
                float flex_factor = 0.0f;
                float total_flex = 0.0f;
                if (flex_space_to_distribute > 0.0f && total_flex_grow > 0.0f) {
                    flex_factor = child->flex_grow;
                    total_flex = total_flex_grow;
                } else if (flex_space_to_distribute < 0.0f &&
                           total_flex_shrink > 0.0f) {
                    flex_factor = child->flex_shrink;
                    total_flex = total_flex_shrink;
                }
                if (flex_factor > 0.0f && total_flex > 0.0f) {
                    flex_extra =
                        (flex_factor / total_flex) * flex_space_to_distribute;
                    if (!horizontal) {
                        child->height->animate_to(
                            r(child_base_size + flex_extra));
                    }
                }
            }

            float child_size = child_base_size + flex_extra;
            y += child_size + effective_gap;
        }
    }
}

float ui::flex_widget::measure_height(update_context &ctx) {
    if (!auto_size) {
        return height->dest();
    }

    if (horizontal) {
        float max_height = 0;
        for (auto &child : children) {
            max_height = std::max(max_height, child->measure_height(ctx));
        }
        return max_height + *padding_top + *padding_bottom;
    } else {
        float total_height = 0;
        for (auto &child : children) {
            total_height += child->measure_height(ctx);
        }
        total_height += (children.size() - 1) * gap;
        return total_height + *padding_top + *padding_bottom;
    }
}
float ui::flex_widget::measure_width(update_context &ctx) {
    if (!auto_size) {
        return width->dest();
    }

    if (horizontal) {
        float total_width = 0;
        for (auto &child : children) {
            total_width += child->measure_width(ctx);
        }
        total_width += (children.size() - 1) * gap;
        return total_width + *padding_left + *padding_right;
    } else {
        float max_width = 0;
        for (auto &child : children) {
            max_width = std::max(max_width, child->measure_width(ctx));
        }
        return max_width + *padding_left + *padding_right;
    }
}

bool ui::flex_widget::should_autosize(bool mainAxis) const {
    if (!auto_size)
        return false;
    if (!parent)
        return true;
    auto flex_parent = dynamic_cast<flex_widget *>(parent);
    if (!flex_parent)
        return true;
    // If the parent is horizontal and is stretching, do not auto size in the
    // cross axis of the parent
    // When the parent is horizontal, the cross axis is vertical
    // When the parent is vertical, the cross axis is horizontal
    // so it would be
    // if ((flex_parent->horizontal && horizontal && !mainAxis) ||
    //     (!flex_parent->horizontal && !horizontal && mainAxis) ||
    //     (flex_parent->horizontal && !horizontal && mainAxis) ||
    //     (!flex_parent->horizontal && horizontal && !mainAxis))
    //         return true;
    // simplified to:
    if (flex_parent->align_items == align::stretch)
        return horizontal != mainAxis;

    // For flex_grow > 0, the side of parent's main axis should be determined by
    // parent
    if (flex_grow > 0)
        return (mainAxis == horizontal) != flex_parent->horizontal;

    return true;
}

void ui::update_context::set_hit_hovered(widget *w) {
    hovered_widgets->push_back(w);
}
bool ui::update_context::mouse_clicked_on(widget *w, bool hittest) const {
    return mouse_clicked && hovered(w, hittest);
}
bool ui::update_context::mouse_down_on(widget *w, bool hittest) const {
    return mouse_down && hovered(w, hittest);
}
bool ui::update_context::mouse_clicked_on_hit(widget *w, bool hittest) {
    if (mouse_clicked_on(w, hittest)) {
        set_hit_hovered(w);
        return true;
    }
    return false;
}
bool ui::update_context::hovered_hit(widget *w, bool hittest) {
    if (hovered(w, hittest)) {
        set_hit_hovered(w);
        return true;
    } else {
        return false;
    }
}
bool ui::widget::check_hit(const update_context &ctx) {
    return ctx.mouse_x >= (x->dest() + ctx.offset_x) &&
           ctx.mouse_x <= (x->dest() + width->dest() + ctx.offset_x) &&
           ctx.mouse_y >= (y->dest() + ctx.offset_y) &&
           ctx.mouse_y <= (y->dest() + height->dest() + ctx.offset_y);
}
void ui::widget::update_children(
    update_context &ctx, std::vector<std::shared_ptr<widget>> &children) {
    for (auto &child : children) {
        update_child_basic(ctx, child);
        if (children_dirty)
            break;
    }

    // Remove dead children
    std::erase_if(children, [](auto &child) { return !child; });
}
void ui::widget::render_children(
    nanovg_context ctx, std::vector<std::shared_ptr<widget>> &children) {
    for (auto &child : children) {
        render_child_basic(ctx, child);
    }
}
void ui::text_widget::render(nanovg_context ctx) {
    widget::render(ctx);
    ctx.fontSize(font_size);
    ctx.fillColor(color.nvg());
    ctx.textAlign(NVG_ALIGN_TOP | NVG_ALIGN_LEFT);
    ctx.fontFace(font_family.c_str());

    if (max_width > 0) {
        ctx.textBox(*x, *y + _yoffset_when_update, max_width, text.c_str(),
                    nullptr);
        return;
    } else {
        ctx.text(*x, *y + _yoffset_when_update, text.c_str(), nullptr);
    }
}
void ui::text_widget::update(update_context &ctx) {
    widget::update(ctx);
    ctx.vg.fontSize(font_size);
    ctx.vg.fontFace(font_family.c_str());
    ctx.vg.textAlign(NVG_ALIGN_TOP | NVG_ALIGN_LEFT);

    auto [w, h, yoffset] =
        max_width < 0
            ? ctx.vg.measureTextWithYOffset(this->text.c_str())
            : ctx.vg.measureTextBoxWithYOffset(this->text.c_str(), max_width);

    _yoffset_when_update = yoffset;

    if (shrink_horizontal) {
        width->animate_to(max_width > 0 ? std::min(w, max_width) : w);
    }

    if (shrink_vertical) {
        height->animate_to(h);
    }
}
ui::textbox_widget::textbox_widget() : widget() {
    width->reset_to(160);
    height->reset_to(min_height);
}

ui::textbox_widget::~textbox_widget() = default;

void ui::textbox_widget::clamp_indices() {
    text = normalize_text_for_textbox(text, multiline);
    const auto map = build_utf8_index_map(text);
    caret_index = clamp_char_index(map, caret_index);
    selection_anchor_index = clamp_char_index(map, selection_anchor_index);
}

void ui::textbox_widget::reset_caret_blink() { caret_blink_elapsed = 0; }

void ui::textbox_widget::notify_change(update_context &ctx) {
    needs_repaint = true;
    if (!on_change) {
        return;
    }
    auto changed_text = text;
    ctx.rt.post_loop_thread_task(
        [callback = on_change, changed_text]() mutable { callback(changed_text); },
        true);
}

void ui::textbox_widget::render(nanovg_context ctx) {
    widget::render(ctx);

    const bool is_focused = focused() && !disabled;
    const float inner_width =
        std::max(width->dest() - padding_x * 2.0f, 1.0f);
    const float inner_height =
        std::max(height->dest() - padding_y * 2.0f, 1.0f);
    auto layout_vg = ctx.with_reset_offset();
    ui::ime_composition_state ime;
    {
        std::lock_guard lock(ctx.rt->ime_composition_lock);
        ime = ctx.rt->ime_composition;
    }
    const bool ime_active = is_focused && ime.active;
    const auto visual = make_textbox_visual_state(
        text, selection_start(), selection_end(), caret_index, multiline,
        ime_active ? &ime : nullptr);
    const auto layout = build_textbox_layout(layout_vg, visual.text, font_size,
                                             multiline, inner_width,
                                             line_height_multiplier);

    const auto fill_color = disabled ? disabled_background_color.nvg()
                            : readonly ? readonly_background_color.nvg()
                                       : background_color.nvg();
    const auto border_paint =
        is_focused ? focus_border_color.nvg() : border_color.nvg();
    const auto foreground_color =
        disabled ? disabled_text_color.nvg() : text_color.nvg();

    ctx.fillColor(fill_color);
    ctx.fillRoundedRect(*x, *y, *width, *height, border_radius);
    ctx.strokeWidth(is_focused ? 2.0f : 1.0f);
    ctx.strokeColor(border_paint);
    ctx.strokeRoundedRect(*x, *y, *width, *height, border_radius);

    auto t = ctx.transaction();
    ctx.scissor(*x + 1, *y + 1, std::max(*width - 2.0f, 0.0f),
                std::max(*height - 2.0f, 0.0f));
    ctx.translate(*x + padding_x - horizontal_scroll,
                  *y + padding_y - vertical_scroll);
    ctx.fontSize(font_size);
    ctx.fontFace("main");
    ctx.textAlign(NVG_ALIGN_TOP | NVG_ALIGN_LEFT);

    const int selection_begin = visual.selection_start;
    const int selection_finish = visual.selection_end;
    for (const auto &row : layout.rows) {
        if (row.y + layout.line_height < vertical_scroll ||
            row.y > vertical_scroll + inner_height) {
            continue;
        }

        const int highlight_start = std::max(selection_begin, row.start);
        const int highlight_end = std::min(selection_finish, row.end);
        if (highlight_end > highlight_start) {
            const float left = caret_x_for_index(row, highlight_start);
            const float right = caret_x_for_index(row, highlight_end);
            ctx.fillColor(selection_color.nvg());
            ctx.fillRect(left, row.y, std::max(right - left, 1.0f),
                         layout.line_height);
        }

        const auto row_text =
            utf8_substr_chars(visual.text, visual.map, row.start, row.end);
        ctx.fillColor(foreground_color);
        ctx.text(0, row.y, row_text.c_str(), nullptr);

        const int composition_start =
            std::max(visual.composition_start, row.start);
        const int composition_end =
            std::min(visual.composition_end, row.end);
        if (ime_active && composition_end > composition_start) {
            const float left = caret_x_for_index(row, composition_start);
            const float right = caret_x_for_index(row, composition_end);
            ctx.beginPath();
            ctx.strokeWidth(1.0f);
            ctx.strokeColor(composition_underline_color.nvg());
            ctx.moveTo(left, row.y + layout.line_height - 1.5f);
            ctx.lineTo(std::max(right, left + 1.0f),
                       row.y + layout.line_height - 1.5f);
            ctx.stroke();
        }
    }

    if (visual.text.empty() && !placeholder.empty()) {
        ctx.fillColor(placeholder_color.nvg());
        if (multiline) {
            ctx.textBox(0, 0, inner_width, placeholder.c_str(), nullptr);
        } else {
            ctx.text(0, 0, placeholder.c_str(), nullptr);
        }
    }

    if (is_focused && std::fmod(caret_blink_elapsed, 1000.0f) < 500.0f) {
        const auto row_index = find_row_for_index(layout, visual.caret_index);
        const auto &row = layout.rows[static_cast<size_t>(row_index)];
        const float caret_x = caret_x_for_index(row, visual.caret_index);
        ctx.beginPath();
        ctx.strokeWidth(1.5f);
        ctx.strokeColor(caret_color.nvg());
        ctx.moveTo(caret_x, row.y + 2.0f);
        ctx.lineTo(caret_x, row.y + layout.line_height - 2.0f);
        ctx.stroke();
    }
}

void ui::textbox_widget::update(update_context &ctx) {
    widget::update(ctx);
    clamp_indices();

    if (disabled && focused()) {
        set_focus(false);
    }

    const bool shift_down = ctx.key_down(GLFW_KEY_LEFT_SHIFT) ||
                            ctx.key_down(GLFW_KEY_RIGHT_SHIFT);
    const bool ctrl_down = ctx.key_down(GLFW_KEY_LEFT_CONTROL) ||
                           ctx.key_down(GLFW_KEY_RIGHT_CONTROL);
    const float inner_width =
        std::max(width->dest() - padding_x * 2.0f, 1.0f);
    const float inner_height =
        std::max(height->dest() - padding_y * 2.0f, 1.0f);
    auto layout =
        build_textbox_layout(ctx.vg, text, font_size, multiline, inner_width,
                             line_height_multiplier);
    auto visual = make_textbox_visual_state(text, selection_start(),
                                            selection_end(), caret_index,
                                            multiline);
    auto visual_layout =
        build_textbox_layout(ctx.vg, visual.text, font_size, multiline,
                             inner_width, line_height_multiplier);

    auto rebuild_layouts = [&]() {
        layout =
            build_textbox_layout(ctx.vg, text, font_size, multiline,
                                 inner_width, line_height_multiplier);
        const auto &ime = ctx.ime_composition();
        const bool ime_active = focused() && !disabled && ime.active;
        visual = make_textbox_visual_state(text, selection_start(),
                                           selection_end(), caret_index,
                                           multiline,
                                           ime_active ? &ime : nullptr);
        visual_layout =
            build_textbox_layout(ctx.vg, visual.text, font_size, multiline,
                                 inner_width, line_height_multiplier);
    };

    auto move_caret = [&](int new_index, bool extend_selection) {
        const auto map = build_utf8_index_map(text);
        new_index = clamp_char_index(map, new_index);
        caret_index = new_index;
        if (!extend_selection) {
            selection_anchor_index = new_index;
        }
        reset_caret_blink();
        needs_repaint = true;
    };

    auto replace_range = [&](int start, int end, const std::string &replacement) {
        auto map = build_utf8_index_map(text);
        start = clamp_char_index(map, start);
        end = clamp_char_index(map, end);
        if (end < start) {
            std::swap(start, end);
        }
        const auto start_byte = byte_offset_for_char(map, start);
        const auto end_byte = byte_offset_for_char(map, end);
        text.replace(start_byte, end_byte - start_byte, replacement);
        const auto replacement_chars =
            build_utf8_index_map(replacement).char_count();
        caret_index = start + replacement_chars;
        selection_anchor_index = caret_index;
        clamp_indices();
        reset_caret_blink();
        needs_repaint = true;
    };

    auto insert_text_internal = [&](const std::string &new_text) {
        if (readonly || disabled) {
            return false;
        }
        const auto normalized = normalize_text_for_textbox(new_text, multiline);
        replace_range(selection_start(), selection_end(), normalized);
        rebuild_layouts();
        return true;
    };

    auto delete_selection_or = [&](int start, int end) {
        if (readonly || disabled) {
            return false;
        }
        if (selection_start() != selection_end()) {
            replace_range(selection_start(), selection_end(), "");
        } else if (start != end) {
            replace_range(start, end, "");
        } else {
            return false;
        }
        rebuild_layouts();
        return true;
    };

    auto pointer_to_caret = [&]() {
        const float local_x = static_cast<float>(
            ctx.mouse_x - (ctx.offset_x + x->dest() + padding_x) +
            horizontal_scroll);
        const float local_y = static_cast<float>(
            ctx.mouse_y - (ctx.offset_y + y->dest() + padding_y) +
            vertical_scroll);
        return caret_index_from_point(layout, std::max(local_x, 0.0f),
                                      std::max(local_y, 0.0f));
    };

    if (ctx.mouse_clicked) {
        if (!disabled && check_hit(ctx)) {
            set_focus(true);
            auto hit_index = pointer_to_caret();
            if (shift_down) {
                caret_index = hit_index;
            } else {
                selection_anchor_index = hit_index;
                caret_index = hit_index;
            }
            preferred_caret_x.reset();
            dragging_selection = true;
            reset_caret_blink();
            needs_repaint = true;
        } else if (focused()) {
            set_focus(false);
            dragging_selection = false;
            preferred_caret_x.reset();
            ctx.rt.clear_ime_composition();
        }
    }

    const bool is_focused = focused() && !disabled;
    const bool ime_active = is_focused && ctx.ime_composition().active;

    if (dragging_selection && is_focused && ctx.mouse_down) {
        caret_index = pointer_to_caret();
        preferred_caret_x.reset();
        reset_caret_blink();
        needs_repaint = true;
        rebuild_layouts();
    }
    if (!ctx.mouse_down) {
        dragging_selection = false;
    }

    bool text_changed = false;
    if (is_focused) {
        ctx.need_repaint = true;
        caret_blink_elapsed += ctx.delta_time;

        if (!ime_active) {
            if (ctrl_down && ctx.key_triggered(GLFW_KEY_A)) {
                selection_anchor_index = 0;
                caret_index = build_utf8_index_map(text).char_count();
                ctx.stop_key_propagation(GLFW_KEY_A);
                reset_caret_blink();
                rebuild_layouts();
            }

            if (ctrl_down && ctx.key_triggered(GLFW_KEY_C)) {
                copy();
                ctx.stop_key_propagation(GLFW_KEY_C);
            }

            if (ctrl_down && ctx.key_triggered(GLFW_KEY_X)) {
                copy();
                text_changed |= delete_selection_or(selection_start(),
                                                    selection_end());
                ctx.stop_key_propagation(GLFW_KEY_X);
            }

            if (ctrl_down && ctx.key_triggered(GLFW_KEY_V)) {
                if (const char *clipboard = glfwGetClipboardString(
                        static_cast<GLFWwindow *>(ctx.window))) {
                    text_changed |= insert_text_internal(clipboard);
                }
                ctx.stop_key_propagation(GLFW_KEY_V);
            }

            if (ctx.key_triggered(GLFW_KEY_BACKSPACE)) {
                if (selection_start() != selection_end()) {
                    text_changed |=
                        delete_selection_or(selection_start(), selection_end());
                } else if (caret_index > 0) {
                    text_changed |=
                        delete_selection_or(caret_index - 1, caret_index);
                }
                preferred_caret_x.reset();
                ctx.stop_key_propagation(GLFW_KEY_BACKSPACE);
            }

            if (ctx.key_triggered(GLFW_KEY_DELETE)) {
                if (selection_start() != selection_end()) {
                    text_changed |=
                        delete_selection_or(selection_start(), selection_end());
                } else {
                    auto char_count = build_utf8_index_map(text).char_count();
                    if (caret_index < char_count) {
                        text_changed |=
                            delete_selection_or(caret_index, caret_index + 1);
                    }
                }
                preferred_caret_x.reset();
                ctx.stop_key_propagation(GLFW_KEY_DELETE);
            }

            if (ctx.key_triggered(GLFW_KEY_LEFT)) {
                if (!shift_down && selection_start() != selection_end()) {
                    move_caret(selection_start(), false);
                } else {
                    move_caret(caret_index - 1, shift_down);
                }
                preferred_caret_x.reset();
                ctx.stop_key_propagation(GLFW_KEY_LEFT);
            }

            if (ctx.key_triggered(GLFW_KEY_RIGHT)) {
                if (!shift_down && selection_start() != selection_end()) {
                    move_caret(selection_end(), false);
                } else {
                    move_caret(caret_index + 1, shift_down);
                }
                preferred_caret_x.reset();
                ctx.stop_key_propagation(GLFW_KEY_RIGHT);
            }

            if (ctx.key_triggered(GLFW_KEY_HOME)) {
                if (ctrl_down || !multiline) {
                    move_caret(0, shift_down);
                } else {
                    const auto row =
                        layout.rows[static_cast<size_t>(find_row_for_index(
                            layout, caret_index))];
                    move_caret(row.start, shift_down);
                }
                preferred_caret_x.reset();
                ctx.stop_key_propagation(GLFW_KEY_HOME);
            }

            if (ctx.key_triggered(GLFW_KEY_END)) {
                if (ctrl_down || !multiline) {
                    move_caret(build_utf8_index_map(text).char_count(),
                               shift_down);
                } else {
                    const auto row =
                        layout.rows[static_cast<size_t>(find_row_for_index(
                            layout, caret_index))];
                    move_caret(row.end, shift_down);
                }
                preferred_caret_x.reset();
                ctx.stop_key_propagation(GLFW_KEY_END);
            }

            if (multiline && ctx.key_triggered(GLFW_KEY_UP)) {
                const auto row_index = find_row_for_index(layout, caret_index);
                const auto &row = layout.rows[static_cast<size_t>(row_index)];
                const float target_x = preferred_caret_x.value_or(
                    caret_x_for_index(row, caret_index));
                preferred_caret_x = target_x;
                const auto next_row =
                    layout.rows[static_cast<size_t>(std::max(row_index - 1, 0))];
                move_caret(caret_index_from_x(next_row, target_x), shift_down);
                ctx.stop_key_propagation(GLFW_KEY_UP);
            }

            if (multiline && ctx.key_triggered(GLFW_KEY_DOWN)) {
                const auto row_index = find_row_for_index(layout, caret_index);
                const auto &row = layout.rows[static_cast<size_t>(row_index)];
                const float target_x = preferred_caret_x.value_or(
                    caret_x_for_index(row, caret_index));
                preferred_caret_x = target_x;
                const auto next_row = layout.rows[static_cast<size_t>(std::min(
                    row_index + 1, static_cast<int>(layout.rows.size()) - 1))];
                move_caret(caret_index_from_x(next_row, target_x), shift_down);
                ctx.stop_key_propagation(GLFW_KEY_DOWN);
            }

            if (multiline && ctx.key_triggered(GLFW_KEY_ENTER)) {
                text_changed |= insert_text_internal("\n");
                preferred_caret_x.reset();
                ctx.stop_key_propagation(GLFW_KEY_ENTER);
            }

            if (!readonly) {
                const auto &typed = ctx.text_input();
                if (!typed.empty()) {
                    text_changed |=
                        insert_text_internal(utf8_from_codepoints(typed));
                    preferred_caret_x.reset();
                }
            }
        }
    } else {
        caret_blink_elapsed = 0;
    }

    clamp_indices();
    rebuild_layouts();

    const bool use_visual_layout = is_focused && ctx.ime_composition().active;
    const auto &active_layout = use_visual_layout ? visual_layout : layout;
    const int active_caret_index =
        use_visual_layout ? visual.caret_index : caret_index;

    if (multiline) {
        const float max_scroll =
            std::max(active_layout.content_height - inner_height, 0.0f);
        if ((ctx.hovered(this) || is_focused) && std::abs(ctx.scroll_y) > 0) {
            vertical_scroll =
                std::clamp(vertical_scroll - ctx.scroll_y * 40.0f, 0.0f,
                           max_scroll);
        }
        const auto &row = active_layout.rows[static_cast<size_t>(
            find_row_for_index(active_layout, active_caret_index))];
        if (row.y < vertical_scroll) {
            vertical_scroll = row.y;
        } else if (row.y + active_layout.line_height >
                   vertical_scroll + inner_height) {
            vertical_scroll = row.y + active_layout.line_height - inner_height;
        }
        vertical_scroll = std::clamp(vertical_scroll, 0.0f, max_scroll);
        horizontal_scroll = 0.0f;
    } else {
        vertical_scroll = 0.0f;
        const auto &row = active_layout.rows.front();
        const float caret_x = caret_x_for_index(row, active_caret_index);
        const float max_scroll =
            std::max(active_layout.content_width - inner_width, 0.0f);
        if (caret_x < horizontal_scroll) {
            horizontal_scroll = caret_x;
        } else if (caret_x > horizontal_scroll + inner_width) {
            horizontal_scroll = caret_x - inner_width;
        }
        horizontal_scroll = std::clamp(horizontal_scroll, 0.0f, max_scroll);
    }

    if (is_focused) {
        const auto &row = active_layout.rows[static_cast<size_t>(
            find_row_for_index(active_layout, active_caret_index))];
        const float caret_x = caret_x_for_index(row, active_caret_index);
        ctx.rt.set_ime_caret_rect(
            ctx.offset_x + x->dest() + padding_x + caret_x -
                horizontal_scroll,
            ctx.offset_y + y->dest() + padding_y + row.y -
                vertical_scroll,
            active_layout.line_height, true,
            ctx.offset_x + x->dest() + padding_x,
            ctx.offset_y + y->dest() + padding_y, inner_width,
            inner_height);
    } else {
        ctx.rt.set_ime_caret_rect(0, 0, 0, false);
    }

    const bool focused_now = focused() && !disabled;
    if (focused_now != last_focused) {
        reset_caret_blink();
        auto callback = focused_now ? on_focus : on_blur;
        if (!focused_now) {
            ctx.rt.clear_ime_composition();
        }
        if (callback) {
            ctx.rt.post_loop_thread_task([callback]() mutable { callback(); },
                                         true);
        }
        last_focused = focused_now;
    }

    if (text_changed) {
        notify_change(ctx);
    }
}

float ui::textbox_widget::measure_height(update_context &ctx) {
    if (height->dest() > 0) {
        return height->dest();
    }
    return multiline ? preferred_multiline_height
                     : std::max(font_size + padding_y * 2.0f + 6.0f, min_height);
}

float ui::textbox_widget::measure_width(update_context &ctx) {
    if (width->dest() > 0) {
        return width->dest();
    }
    return 160.0f;
}

void ui::textbox_widget::focus() {
    set_focus(true);
    reset_caret_blink();
}

void ui::textbox_widget::blur() {
    set_focus(false);
    dragging_selection = false;
    preferred_caret_x.reset();
}

void ui::textbox_widget::select_all() {
    selection_anchor_index = 0;
    caret_index = build_utf8_index_map(text).char_count();
    reset_caret_blink();
}

void ui::textbox_widget::select_range(int start, int end) {
    set_selection(start, end);
}

int ui::textbox_widget::selection_start() const {
    return std::min(selection_anchor_index, caret_index);
}

int ui::textbox_widget::selection_end() const {
    return std::max(selection_anchor_index, caret_index);
}

void ui::textbox_widget::set_selection(int start, int end) {
    const auto map = build_utf8_index_map(text);
    selection_anchor_index = clamp_char_index(map, start);
    caret_index = clamp_char_index(map, end);
    reset_caret_blink();
}

void ui::textbox_widget::insert_text(const std::string &new_text) {
    if (readonly || disabled) {
        return;
    }
    const auto normalized = normalize_text_for_textbox(new_text, multiline);
    auto map = build_utf8_index_map(text);
    const auto start_byte = byte_offset_for_char(map, selection_start());
    const auto end_byte = byte_offset_for_char(map, selection_end());
    text.replace(start_byte, end_byte - start_byte, normalized);
    caret_index =
        selection_start() + build_utf8_index_map(normalized).char_count();
    selection_anchor_index = caret_index;
    clamp_indices();
    reset_caret_blink();
}

void ui::textbox_widget::delete_text(int start, int end) {
    if (readonly || disabled) {
        return;
    }
    auto map = build_utf8_index_map(text);
    start = clamp_char_index(map, start);
    end = clamp_char_index(map, end);
    if (end < start) {
        std::swap(start, end);
    }
    const auto start_byte = byte_offset_for_char(map, start);
    const auto end_byte = byte_offset_for_char(map, end);
    text.erase(start_byte, end_byte - start_byte);
    selection_anchor_index = start;
    caret_index = start;
    clamp_indices();
    reset_caret_blink();
}

void ui::textbox_widget::clear() {
    if (readonly || disabled) {
        return;
    }
    text.clear();
    selection_anchor_index = 0;
    caret_index = 0;
    horizontal_scroll = 0;
    vertical_scroll = 0;
    reset_caret_blink();
}

void ui::textbox_widget::copy() {
    if (!owner_rt || !owner_rt->window) {
        return;
    }
    const auto selected = selected_text(*this);
    glfwSetClipboardString(owner_rt->window, selected.c_str());
}

void ui::textbox_widget::cut() {
    if (readonly || disabled) {
        return;
    }
    copy();
    delete_text(selection_start(), selection_end());
}

void ui::textbox_widget::paste() {
    if (!owner_rt || !owner_rt->window || readonly || disabled) {
        return;
    }
    if (const char *clipboard = glfwGetClipboardString(owner_rt->window)) {
        insert_text(clipboard);
    }
}

void ui::padding_widget::update(update_context &ctx) {
    auto off = ctx.with_offset(*padding_left, *padding_top);
    widget::update(off);

    float max_width = 0, max_height = 0;
    for (auto &child : children) {
        max_width = std::max(max_width, child->measure_width(ctx));
        max_height = std::max(max_height, child->measure_height(ctx));
    }

    width->animate_to(max_width + *padding_left + *padding_right);
    height->animate_to(max_height + *padding_top + *padding_bottom);
}
void ui::padding_widget::render(nanovg_context ctx) {
    ctx.transaction();
    ctx.translate(**padding_left, **padding_top);
    widget::render(ctx);
}
ui::update_context ui::update_context::within(widget *w) const {
    auto copy = *this;
    copy.offset_x = w->last_offset_x;
    copy.offset_y = w->last_offset_y;
    return copy;
}
void ui::update_context::print_hover_info(widget *w) const {
    std::printf(
        "widget(%p)\n\t hovered: %d x: %f y: %f width: %f height: %f \n\t"
        "mouse_x: %f mouse_y: %f (x: %d %d y: %d %d)\n",
        w, hovered(w), w->x->dest(), w->y->dest(), w->width->dest(),
        w->height->dest(), mouse_x, mouse_y,
        mouse_x >= (w->x->dest() + offset_x),
        mouse_x <= (w->x->dest() + w->width->dest() + offset_x),
        mouse_y >= (w->y->dest() + offset_y),
        mouse_y <= (w->y->dest() + w->height->dest() + offset_y));
}
bool ui::widget::focused() {
    return owner_rt && owner_rt->focused_widget &&
           !owner_rt->focused_widget->expired() &&
           owner_rt->focused_widget->lock().get() == this;
}
bool ui::widget::focus_within() {
    return focused() || std::ranges::any_of(children, [](const auto &child) {
               return child->focus_within();
           });
}
void ui::widget::set_focus(bool focused) {
    if (owner_rt) {
        if (focused) {
            owner_rt->focused_widget = this->shared_from_this();
        } else {
            if (owner_rt->focused_widget &&
                owner_rt->focused_widget->lock().get() == this) {
                owner_rt->focused_widget.reset();
            }
        }
    }
}

bool ui::update_context::key_pressed(int key) const {
    return (bool)(rt.key_states.get()[key] & key_state::pressed);
}
bool ui::update_context::key_triggered(int key) const {
    return (bool)(rt.key_states.get()[key] &
                  (key_state::pressed | key_state::repeated));
}
bool ui::update_context::key_down(int key) const {
    return glfwGetKey((GLFWwindow *)window, key) == GLFW_PRESS;
}
void ui::update_context::stop_key_propagation(int key) {
    if (key >= 0 && key < GLFW_KEY_LAST + 1) {
        rt.key_states.get()[key] = key_state::none;
    }
}
const std::u32string &ui::update_context::text_input() const {
    return rt.char_input.get();
}
ui::ime_composition_state ui::update_context::ime_composition() const {
    std::lock_guard lock(rt.ime_composition_lock);
    return rt.ime_composition;
}
ui::button_widget::button_widget(const std::string &button_text)
    : button_widget() {
    auto text = emplace_child<ui::text_widget>();
    text->text = button_text;
    text->font_size = 14;
    text->color.reset_to({1, 1, 1, 0.95});
}
void ui::button_widget::render(ui::nanovg_context ctx) {

    ctx.fillColor(bg_color);
    ctx.fillRoundedRect(*x, *y, *width, *height, 6);

    float bw = 1.0f;

    float radius = 6.0f;
    // 4 edges
    ctx.beginPath();
    ctx.strokeWidth(bw);
    ctx.strokeColor(border_top);
    ctx.moveTo(*x + radius, *y + bw / 2);
    ctx.lineTo(*x + *width - radius, *y + bw / 2);
    ctx.stroke();

    ctx.beginPath();
    ctx.strokeWidth(bw);
    ctx.strokeColor(border_right);
    ctx.moveTo(*x + *width - bw / 2, *y + radius);
    ctx.lineTo(*x + *width - bw / 2, *y + *height - radius);
    ctx.stroke();

    ctx.beginPath();
    ctx.strokeWidth(bw);
    ctx.strokeColor(border_bottom);
    ctx.moveTo(*x + *width - radius, *y + *height - bw / 2);
    ctx.lineTo(*x + radius, *y + *height - bw / 2);
    ctx.stroke();

    ctx.beginPath();
    ctx.strokeWidth(bw);
    ctx.strokeColor(border_left);
    ctx.moveTo(*x + bw / 2, *y + *height - radius);
    ctx.lineTo(*x + bw / 2, *y + radius);
    ctx.stroke();

    // 4 corners
    float cr = radius - bw / 2;
    ctx.beginPath();
    ctx.strokeWidth(bw);
    ctx.strokeColor(border_right.blend(border_top));
    ctx.moveTo(*x + *width - radius, *y + bw / 2);
    ctx.arcTo(*x + *width - bw / 2, *y + bw / 2, *x + *width - bw / 2,
              *y + radius, cr);
    ctx.stroke();

    ctx.beginPath();
    ctx.strokeWidth(bw);
    ctx.strokeColor(border_bottom.blend(border_right));
    ctx.moveTo(*x + *width - bw / 2, *y + *height - radius);
    ctx.arcTo(*x + *width - bw / 2, *y + *height - bw / 2, *x + *width - radius,
              *y + *height - bw / 2, cr);
    ctx.stroke();

    ctx.beginPath();
    ctx.strokeWidth(bw);
    ctx.strokeColor(border_left.blend(border_bottom));
    ctx.moveTo(*x + radius, *y + *height - bw / 2);
    ctx.arcTo(*x + bw / 2, *y + *height - bw / 2, *x + bw / 2,
              *y + *height - radius, cr);
    ctx.stroke();

    ctx.beginPath();
    ctx.strokeWidth(bw);
    ctx.strokeColor(border_top.blend(border_left));
    ctx.moveTo(*x + bw / 2, *y + radius);
    ctx.arcTo(*x + bw / 2, *y + bw / 2, *x + radius, *y + bw / 2, cr);
    ctx.stroke();

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
void ui::button_widget::update(ui::update_context &ctx) {
    padding_widget::update(ctx);

    if (ctx.mouse_clicked_on_hit(this)) {
        this->ctx = &ctx;
        on_click();
        this->ctx = nullptr;
    }

    update_colors(ctx.mouse_down_on(this), ctx.hovered(this));
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
void ui::widget::remove_child(std::shared_ptr<widget> child) {
    child->parent = nullptr;
    children.erase(std::remove(children.begin(), children.end(), child),
                   children.end());
    children_dirty = true;
}
float ui::text_widget::measure_height(update_context &ctx) {
    ctx.vg.fontSize(font_size);
    ctx.vg.fontFace(font_family.c_str());
    auto text = max_width < 0
                    ? ctx.vg.measureText(this->text.c_str())
                    : ctx.vg.measureTextBox(this->text.c_str(), max_width);
    return text.second;
}
float ui::text_widget::measure_width(update_context &ctx) {
    ctx.vg.fontSize(font_size);
    ctx.vg.fontFace(font_family.c_str());
    auto text = max_width < 0
                    ? ctx.vg.measureText(this->text.c_str())
                    : ctx.vg.measureTextBox(this->text.c_str(), max_width);
    return max_width > 0 ? std::min(text.first, max_width) : text.first;
}
