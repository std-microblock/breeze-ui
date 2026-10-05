#include "breeze_ui/font.h"
#include "breeze_ui/ui.h"
#include "breeze_ui/widget.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>

#include "simdutf.h"

namespace {
void apply_font_face(ui::nanovg_context &ctx, std::string_view family,
                     int weight) {
    ctx.fontFaceId(ui::resolve_font(ctx.ctx, family, weight));
}

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
                                    int font_weight, bool multiline,
                                    float inner_width,
                                    float line_height_multiplier) {
    textbox_layout layout;

    vg.fontSize(font_size);
    apply_font_face(vg, "main", font_weight);
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

struct textbox_palette {
    std::array<float, 4> background;
    std::array<float, 4> readonly_background;
    std::array<float, 4> disabled_background;
    std::array<float, 4> stroke;
    std::array<float, 4> accent;
    std::array<float, 4> text;
    std::array<float, 4> disabled_text;
    std::array<float, 4> placeholder;
    std::array<float, 4> selection;
    std::array<float, 4> caret;
    float hover_fill_alpha = 1.f;
    float focus_fill_alpha = 1.f;
    float stroke_boost = 1.5f;
};

textbox_palette fluent_textbox_palette(bool light) {
    if (light) {
        return {
            .background = {1.f, 1.f, 1.f, 0.7f},
            .readonly_background = {1.f, 1.f, 1.f, 0.3f},
            .disabled_background = {0.976f, 0.976f, 0.976f, 0.3f},
            .stroke = {0.f, 0.f, 0.f, 0.07f},
            .accent = {0.f, 95.f / 255.f, 184.f / 255.f, 1.f},
            .text = {0.f, 0.f, 0.f, 0.9f},
            .disabled_text = {0.f, 0.f, 0.f, 0.36f},
            .placeholder = {0.f, 0.f, 0.f, 0.6f},
            .selection = {0.f, 95.f / 255.f, 184.f / 255.f, 0.4f},
            .caret = {0.f, 0.f, 0.f, 0.9f},
            .hover_fill_alpha = 0.92f,
            .focus_fill_alpha = 1.f,
            .stroke_boost = 1.4f,
        };
    }
    return {
        .background = {1.f, 1.f, 1.f, 0.08f},
        .readonly_background = {1.f, 1.f, 1.f, 0.04f},
        .disabled_background = {1.f, 1.f, 1.f, 0.05f},
        .stroke = {1.f, 1.f, 1.f, 0.08f},
        .accent = {96.f / 255.f, 205.f / 255.f, 1.f, 1.f},
        .text = {1.f, 1.f, 1.f, 1.f},
        .disabled_text = {1.f, 1.f, 1.f, 0.36f},
        .placeholder = {197.f / 255.f, 197.f / 255.f, 197.f / 255.f, 1.f},
        .selection = {96.f / 255.f, 205.f / 255.f, 1.f, 0.4f},
        .caret = {1.f, 1.f, 1.f, 1.f},
        .hover_fill_alpha = 0.12f,
        .focus_fill_alpha = 0.16f,
        .stroke_boost = 1.5f,
    };
}

float vertical_content_offset(bool multiline, float inner_height,
                              float content_height) {
    if (multiline) {
        return 0.f;
    }
    return std::max((inner_height - content_height) * 0.5f, 0.f);
}
} // namespace

void ui::textbox_widget::render(nanovg_context ctx) {
    const bool is_focused = focused() && !disabled;
    const float inner_width = std::max(width->dest() - padding_x * 2.0f, 1.0f);
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
                                             font_weight, multiline,
                                             inner_width, line_height_multiplier);

    const bool interactive = !disabled && !readonly;
    auto fill_color = disabled   ? disabled_background_color.nvg()
                      : readonly ? readonly_background_color.nvg()
                                 : background_color.nvg();
    const float fill_alpha =
        is_focused ? focus_fill_alpha : (hovered() ? hover_fill_alpha : 0.f);
    if (interactive && fill_alpha > fill_color.a) {
        fill_color.a = fill_alpha;
    }
    auto stroke_color = border_color.nvg();
    const bool emphasized = interactive && (is_focused || hovered());
    if (emphasized) {
        stroke_color.a = std::min(1.f, stroke_color.a * stroke_emphasize);
    }
    const auto foreground_color =
        disabled ? disabled_text_color.nvg() : text_color.nvg();
    const float content_offset_y = vertical_content_offset(
        multiline, inner_height, layout.content_height);

    ctx.fillColor(fill_color);
    ctx.fillRoundedRect(*x, *y, *width, *height, border_radius);
    ctx.strokeWidth(1.0f);
    ctx.strokeColor(stroke_color);
    ctx.strokeRoundedRect(*x + 0.5f, *y + 0.5f,
                          std::max(*width - 1.0f, 0.0f),
                          std::max(*height - 1.0f, 0.0f),
                          std::max(border_radius - 0.5f, 0.0f));

    const float underline_inset = std::min(border_radius, *width * 0.5f);
    const float underline_y = *y + *height - 1.0f;
    if (is_focused) {
        const float reveal = std::clamp(focus_underline->var(), 0.f, 1.f);
        const float half = (*width * 0.5f - underline_inset) * reveal;
        const float centre = *x + *width * 0.5f;
        auto accent = focus_border_color.nvg();
        accent.a *= reveal;
        ctx.beginPath();
        ctx.strokeWidth(2.0f);
        ctx.strokeColor(accent);
        ctx.moveTo(centre - half, underline_y);
        ctx.lineTo(centre + half, underline_y);
        ctx.stroke();
    } else if (interactive) {
        auto underline = border_color.nvg();
        underline.a =
            std::min(1.f, underline.a * (hovered() ? 3.6f : 2.4f));
        ctx.beginPath();
        ctx.strokeWidth(1.0f);
        ctx.strokeColor(underline);
        ctx.moveTo(*x + underline_inset, underline_y);
        ctx.lineTo(*x + *width - underline_inset, underline_y);
        ctx.stroke();
    }

    auto t = ctx.transaction();
    ctx.intersectScissor(*x + 1.0f, *y + 1.0f,
                         std::max(*width - 2.0f, 0.0f),
                         std::max(*height - 2.0f, 0.0f));
    ctx.translate(*x + padding_x - horizontal_scroll + ctx.offset_x,
                  *y + padding_y + content_offset_y - vertical_scroll +
                      ctx.offset_y);
    ctx = ctx.with_reset_offset();
    ctx.fontSize(font_size);
    apply_font_face(ctx, "main", font_weight);
    ctx.textAlign(NVG_ALIGN_TOP | NVG_ALIGN_LEFT);

    for (const auto &row : layout.rows) {
        if (row.y + layout.line_height < vertical_scroll ||
            row.y > vertical_scroll + inner_height) {
            continue;
        }

        const int highlight_start = std::max(visual.selection_start, row.start);
        const int highlight_end = std::min(visual.selection_end, row.end);
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
        const int composition_end = std::min(visual.composition_end, row.end);
        if (ime_active && composition_end > composition_start) {
            const float left = caret_x_for_index(row, composition_start);
            const float right = caret_x_for_index(row, composition_end);
            ctx.beginPath();
            ctx.strokeWidth(2.0f);
            ctx.strokeColor(composition_underline_color.nvg());
            ctx.moveTo(left, row.y + layout.line_height - 1.0f);
            ctx.lineTo(std::max(right, left + 1.0f),
                       row.y + layout.line_height - 1.0f);
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
        ctx.strokeWidth(1.0f);
        ctx.strokeColor(caret_color.nvg());
        ctx.moveTo(caret_x, row.y + 1.0f);
        ctx.lineTo(caret_x, row.y + layout.line_height - 1.0f);
        ctx.stroke();
    }
}

ui::textbox_widget::textbox_widget() : widget() {
    const auto palette = fluent_textbox_palette(ui::system_uses_light_theme());
    background_color.reset_to(palette.background);
    readonly_background_color.reset_to(palette.readonly_background);
    disabled_background_color.reset_to(palette.disabled_background);
    border_color.reset_to(palette.stroke);
    focus_border_color.reset_to(palette.accent);
    text_color.reset_to(palette.text);
    disabled_text_color.reset_to(palette.disabled_text);
    placeholder_color.reset_to(palette.placeholder);
    selection_color.reset_to(palette.selection);
    caret_color.reset_to(palette.caret);
    composition_underline_color.reset_to(palette.accent);
    hover_fill_alpha = palette.hover_fill_alpha;
    focus_fill_alpha = palette.focus_fill_alpha;
    stroke_emphasize = palette.stroke_boost;
}

ui::textbox_widget::~textbox_widget() = default;

void ui::textbox_widget::clamp_indices() {
    text = normalize_text_for_textbox(text, multiline);
    const auto map = build_utf8_index_map(text);
    caret_index = clamp_char_index(map, caret_index);
    selection_anchor_index = clamp_char_index(map, selection_anchor_index);
}

void ui::textbox_widget::reset_caret_blink() {
    caret_blink_elapsed = 0;
    request_repaint();
}

void ui::textbox_widget::notify_change() {
    request_repaint();
    if (!on_change || !owner_rt) {
        return;
    }
    owner_rt->post_loop_thread_task(
        [callback = on_change, changed_text = text]() mutable {
            callback(changed_text);
        },
        true);
}

void ui::textbox_widget::before_layout() {
    widget::before_layout();
    auto key = std::make_tuple(multiline, min_height, font_size, padding_y,
                               preferred_multiline_height);
    if (measured_key != key) {
        measured_key = key;
        invalidate_measure();
    }
}

YGSize ui::textbox_widget::measure(float w, YGMeasureMode wm, float,
                                   YGMeasureMode) {
    float mw = wm == YGMeasureModeExactly ? w : 160.0f;
    if (wm == YGMeasureModeAtMost)
        mw = std::min(mw, w);
    return {mw, multiline ? preferred_multiline_height
                          : std::max(font_size + padding_y * 2.0f + 6.0f,
                                     min_height)};
}

int ui::textbox_widget::caret_from_point(float px, float py) {
    text_measure_scope scope(*this);
    auto &vg = scope.vg;
    if (!vg.ctx)
        return caret_index;
    const float inner_width = std::max(width->dest() - padding_x * 2.0f, 1.0f);
    const float inner_height =
        std::max(height->dest() - padding_y * 2.0f, 1.0f);
    auto layout = build_textbox_layout(vg, text, font_size, font_weight,
                                       multiline, inner_width,
                                       line_height_multiplier);
    const float content_offset_y =
        vertical_content_offset(multiline, inner_height, layout.content_height);
    const float local_x = px - (abs_x() + padding_x) + horizontal_scroll;
    const float local_y = py - (abs_y() + padding_y + content_offset_y) +
                          vertical_scroll;
    return caret_index_from_point(layout, std::max(local_x, 0.0f),
                                  std::max(local_y, 0.0f));
}

void ui::textbox_widget::handle_mouse_enter() {
    request_repaint();
}

void ui::textbox_widget::handle_mouse_leave() {
    request_repaint();
}

void ui::textbox_widget::handle_mouse_down(mouse_event &e) {
    if (e.button != mouse_button::left || disabled)
        return;
    e.handled = true;
    set_focus(true);
    clamp_indices();
    auto hit_index = caret_from_point(e.x, e.y);
    if (!(owner_rt && (owner_rt->key_down(GLFW_KEY_LEFT_SHIFT) ||
                       owner_rt->key_down(GLFW_KEY_RIGHT_SHIFT)))) {
        selection_anchor_index = hit_index;
    }
    caret_index = hit_index;
    preferred_caret_x.reset();
    dragging_selection = true;
    reset_caret_blink();
}

void ui::textbox_widget::handle_mouse_move(mouse_event &e) {
    if (!dragging_selection || !focused() || disabled)
        return;
    if (!owner_rt || !owner_rt->mouse_down) {
        dragging_selection = false;
        return;
    }
    caret_index = caret_from_point(e.x, e.y);
    preferred_caret_x.reset();
    reset_caret_blink();
}

void ui::textbox_widget::handle_mouse_up(mouse_event &e) {
    if (e.button == mouse_button::left)
        dragging_selection = false;
}

void ui::textbox_widget::handle_scroll(scroll_event &e) {
    if (!multiline)
        return;
    text_measure_scope scope(*this);
    auto &vg = scope.vg;
    if (!vg.ctx)
        return;
    const float inner_width = std::max(width->dest() - padding_x * 2.0f, 1.0f);
    const float inner_height =
        std::max(height->dest() - padding_y * 2.0f, 1.0f);
    auto layout = build_textbox_layout(vg, text, font_size, font_weight,
                                       multiline, inner_width,
                                       line_height_multiplier);
    const float max_scroll =
        std::max(layout.content_height - inner_height, 0.0f);
    if (max_scroll <= 0)
        return;
    vertical_scroll =
        std::clamp(vertical_scroll - e.delta * 40.0f, 0.0f, max_scroll);
    e.handled = true;
    request_repaint();
}

void ui::textbox_widget::handle_key(key_event &e) {
    if (!focused() || disabled || !owner_rt)
        return;
    {
        std::lock_guard lock(owner_rt->ime_composition_lock);
        if (owner_rt->ime_composition.active)
            return;
    }

    if (!on_key_down && !multiline &&
        (e.key == GLFW_KEY_UP || e.key == GLFW_KEY_DOWN ||
         e.key == GLFW_KEY_ENTER))
        return;

    if (on_key_down) {
        e.handled = true;
        if (pending_key_batches.empty() ||
            pending_key_batches.back().frame != owner_rt->frame_index) {
            pending_key_batches.push_back(
                {.id = next_pending_key_batch_id++,
                 .frame = owner_rt->frame_index,
                 .mods = e.mods});
        }
        auto &batch = pending_key_batches.back();
        const auto batch_id = batch.id;
        const auto event_index = batch.events.size();
        batch.events.push_back({.key = e.key});

        auto weak_self = std::weak_ptr<textbox_widget>(
            std::static_pointer_cast<textbox_widget>(shared_from_this()));
        owner_rt->post_loop_thread_task(
            [weak_self, callback = on_key_down, batch_id, event_index,
             key = e.key, ev = e]() mutable {
                const bool canceled =
                    callback ? callback(key, ev.shift(), ev.ctrl(), ev.alt(),
                                        ev.super())
                             : false;
                auto self = weak_self.lock();
                if (!self || !self->owner_rt) {
                    return;
                }
                std::lock_guard lock(self->owner_rt->rt_lock);
                auto it = std::ranges::find_if(
                    self->pending_key_batches,
                    [batch_id](const auto &b) { return b.id == batch_id; });
                if (it == self->pending_key_batches.end() ||
                    event_index >= it->events.size()) {
                    return;
                }
                it->events[event_index].canceled = canceled;
                it->events[event_index].resolved = true;
                self->request_repaint();
            },
            true);
        return;
    }

    bool text_changed = false;
    clamp_indices();
    apply_key(e.key, e.mods, text_changed);
    e.handled = true;
    if (text_changed)
        notify_change();
}

void ui::textbox_widget::handle_text_input(text_input_event &e) {
    if (!focused() || disabled || !owner_rt)
        return;
    e.handled = true;
    if (!pending_key_batches.empty() &&
        pending_key_batches.back().frame == owner_rt->frame_index) {
        pending_key_batches.back().text_input += e.text;
        return;
    }
    if (on_key_down && !pending_key_batches.empty()) {
        pending_key_batches.push_back({.id = next_pending_key_batch_id++,
                                       .frame = owner_rt->frame_index,
                                       .text_input = e.text});
        return;
    }
    bool text_changed = false;
    clamp_indices();
    apply_text(e.text, text_changed);
    if (text_changed)
        notify_change();
}

void ui::textbox_widget::drain_key_batches() {
    bool text_changed = false;
    while (!pending_key_batches.empty()) {
        auto &front = pending_key_batches.front();
        if (!std::ranges::all_of(front.events,
                                 [](const auto &ev) { return ev.resolved; }))
            break;
        auto batch = std::move(front);
        pending_key_batches.pop_front();
        clamp_indices();
        const bool modifier = batch.mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT |
                                            GLFW_MOD_SUPER);
        bool suppress_text = false;
        for (const auto &ev : batch.events) {
            if (ev.canceled) {
                suppress_text |= !modifier;
                continue;
            }
            apply_key(ev.key, batch.mods, text_changed);
        }
        if (!suppress_text)
            apply_text(batch.text_input, text_changed);
    }
    if (text_changed)
        notify_change();
}

void ui::textbox_widget::apply_text(const std::u32string &input,
                                    bool &text_changed) {
    if (readonly || disabled || input.empty())
        return;
    const auto normalized =
        normalize_text_for_textbox(utf8_from_codepoints(input), multiline);
    auto map = build_utf8_index_map(text);
    const auto start = selection_start();
    const auto start_byte = byte_offset_for_char(map, start);
    const auto end_byte = byte_offset_for_char(map, selection_end());
    text.replace(start_byte, end_byte - start_byte, normalized);
    caret_index = start + build_utf8_index_map(normalized).char_count();
    selection_anchor_index = caret_index;
    clamp_indices();
    preferred_caret_x.reset();
    reset_caret_blink();
    text_changed = true;
}

void ui::textbox_widget::apply_key(int key, int mods, bool &text_changed) {
    text_measure_scope scope(*this);
    auto &vg = scope.vg;
    if (!vg.ctx)
        return;
    const bool shift = mods & GLFW_MOD_SHIFT;
    const bool ctrl = mods & GLFW_MOD_CONTROL;
    const float inner_width = std::max(width->dest() - padding_x * 2.0f, 1.0f);
    auto layout = build_textbox_layout(vg, text, font_size, font_weight,
                                       multiline, inner_width,
                                       line_height_multiplier);

    auto move_caret = [&](int new_index, bool extend_selection) {
        new_index = clamp_char_index(build_utf8_index_map(text), new_index);
        caret_index = new_index;
        if (!extend_selection) {
            selection_anchor_index = new_index;
        }
        reset_caret_blink();
    };

    auto delete_range = [&](int start, int end) {
        if (readonly || disabled || start == end)
            return;
        delete_text(start, end);
        text_changed = true;
    };

    auto delete_selection_or = [&](int start, int end) {
        if (selection_start() != selection_end())
            delete_range(selection_start(), selection_end());
        else
            delete_range(start, end);
    };

    switch (key) {
    case GLFW_KEY_A:
        if (ctrl)
            select_all();
        break;
    case GLFW_KEY_C:
        if (ctrl)
            copy();
        break;
    case GLFW_KEY_X:
        if (ctrl) {
            copy();
            delete_selection_or(selection_start(), selection_end());
        }
        break;
    case GLFW_KEY_V:
        if (ctrl && owner_rt && owner_rt->window && !readonly) {
            if (const char *clipboard =
                    glfwGetClipboardString(owner_rt->window)) {
                apply_text(utf32_from_utf8(clipboard), text_changed);
            }
        }
        break;
    case GLFW_KEY_BACKSPACE:
        delete_selection_or(caret_index - 1 < 0 ? 0 : caret_index - 1,
                            caret_index);
        preferred_caret_x.reset();
        break;
    case GLFW_KEY_DELETE:
        delete_selection_or(
            caret_index,
            std::min(caret_index + 1, build_utf8_index_map(text).char_count()));
        preferred_caret_x.reset();
        break;
    case GLFW_KEY_LEFT:
        if (!shift && selection_start() != selection_end())
            move_caret(selection_start(), false);
        else
            move_caret(caret_index - 1, shift);
        preferred_caret_x.reset();
        break;
    case GLFW_KEY_RIGHT:
        if (!shift && selection_start() != selection_end())
            move_caret(selection_end(), false);
        else
            move_caret(caret_index + 1, shift);
        preferred_caret_x.reset();
        break;
    case GLFW_KEY_HOME:
        if (ctrl || !multiline) {
            move_caret(0, shift);
        } else {
            const auto &row = layout.rows[static_cast<size_t>(
                find_row_for_index(layout, caret_index))];
            move_caret(row.start, shift);
        }
        preferred_caret_x.reset();
        break;
    case GLFW_KEY_END:
        if (ctrl || !multiline) {
            move_caret(build_utf8_index_map(text).char_count(), shift);
        } else {
            const auto &row = layout.rows[static_cast<size_t>(
                find_row_for_index(layout, caret_index))];
            move_caret(row.end, shift);
        }
        preferred_caret_x.reset();
        break;
    case GLFW_KEY_UP:
    case GLFW_KEY_DOWN: {
        if (!multiline)
            break;
        const auto row_index = find_row_for_index(layout, caret_index);
        const auto &row = layout.rows[static_cast<size_t>(row_index)];
        const float target_x =
            preferred_caret_x.value_or(caret_x_for_index(row, caret_index));
        preferred_caret_x = target_x;
        const auto next_index =
            key == GLFW_KEY_UP
                ? std::max(row_index - 1, 0)
                : std::min(row_index + 1,
                           static_cast<int>(layout.rows.size()) - 1);
        move_caret(caret_index_from_x(
                       layout.rows[static_cast<size_t>(next_index)], target_x),
                   shift);
        break;
    }
    case GLFW_KEY_ENTER:
        if (multiline)
            apply_text(U"\n", text_changed);
        break;
    default:
        break;
    }
}

void ui::textbox_widget::tick(float delta_time) {
    focus_underline->animate_to(focused() && !disabled ? 1.f : 0.f);
    if (disabled && focused()) {
        set_focus(false);
    }
    if (focused() && !disabled) {
        const float before = std::fmod(caret_blink_elapsed, 1000.0f);
        caret_blink_elapsed += delta_time;
        const float now = std::fmod(caret_blink_elapsed, 1000.0f);
        if ((before < 500.0f) != (now < 500.0f))
            needs_repaint = true;
        if (owner_rt)
            owner_rt->schedule_frame(now < 500.0f ? 500.0f - now
                                                  : 1000.0f - now);
        drain_key_batches();
    }
}

void ui::textbox_widget::after_layout() {
    if (focused() && !disabled)
        update_scroll_and_ime();
}

void ui::textbox_widget::update_scroll_and_ime() {
    text_measure_scope scope(*this);
    auto &vg = scope.vg;
    if (!vg.ctx || !owner_rt)
        return;
    clamp_indices();
    const float inner_width = std::max(width->dest() - padding_x * 2.0f, 1.0f);
    const float inner_height =
        std::max(height->dest() - padding_y * 2.0f, 1.0f);
    ui::ime_composition_state ime;
    {
        std::lock_guard lock(owner_rt->ime_composition_lock);
        ime = owner_rt->ime_composition;
    }
    const auto visual = make_textbox_visual_state(
        text, selection_start(), selection_end(), caret_index, multiline,
        ime.active ? &ime : nullptr);
    const auto layout = build_textbox_layout(vg, visual.text, font_size,
                                             font_weight, multiline,
                                             inner_width, line_height_multiplier);
    const auto &row = layout.rows[static_cast<size_t>(
        find_row_for_index(layout, visual.caret_index))];
    const float caret_x = caret_x_for_index(row, visual.caret_index);

    if (multiline) {
        const float max_scroll =
            std::max(layout.content_height - inner_height, 0.0f);
        if (row.y < vertical_scroll) {
            vertical_scroll = row.y;
        } else if (row.y + layout.line_height >
                   vertical_scroll + inner_height) {
            vertical_scroll = row.y + layout.line_height - inner_height;
        }
        vertical_scroll = std::clamp(vertical_scroll, 0.0f, max_scroll);
        horizontal_scroll = 0.0f;
    } else {
        vertical_scroll = 0.0f;
        const float max_scroll =
            std::max(layout.content_width - inner_width, 0.0f);
        if (caret_x < horizontal_scroll) {
            horizontal_scroll = caret_x;
        } else if (caret_x > horizontal_scroll + inner_width) {
            horizontal_scroll = caret_x - inner_width;
        }
        horizontal_scroll = std::clamp(horizontal_scroll, 0.0f, max_scroll);
    }

    const float content_offset_y =
        vertical_content_offset(multiline, inner_height, layout.content_height);
    const float left = abs_x() + padding_x,
                top = abs_y() + padding_y + content_offset_y;
    owner_rt->set_ime_caret_rect(left + caret_x - horizontal_scroll,
                                 top + row.y - vertical_scroll,
                                 layout.line_height, true, left, top,
                                 inner_width, inner_height);
}

void ui::textbox_widget::handle_focus_changed(bool focused_now) {
    caret_blink_elapsed = 0;
    request_repaint();
    if (!focused_now) {
        dragging_selection = false;
        preferred_caret_x.reset();
        pending_key_batches.clear();
        if (owner_rt) {
            owner_rt->clear_ime_composition();
            owner_rt->set_ime_caret_rect(0, 0, 0, false);
        }
    }
    auto callback = focused_now ? on_focus : on_blur;
    if (callback && owner_rt) {
        owner_rt->post_loop_thread_task([callback]() mutable { callback(); },
                                        true);
    }
}

void ui::textbox_widget::focus() {
    set_focus(true);
    reset_caret_blink();
}

void ui::textbox_widget::blur() {
    set_focus(false);
    dragging_selection = false;
    preferred_caret_x.reset();
    pending_key_batches.clear();
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
    bool changed = false;
    clamp_indices();
    apply_text(utf32_from_utf8(new_text), changed);
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
