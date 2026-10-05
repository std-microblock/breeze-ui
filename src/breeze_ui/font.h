#pragma once

#include <string>
#include <string_view>
#include <vector>

struct NVGcontext;

namespace ui {

// Entries are family names ("Segoe UI") or font file paths, optionally
// suffixed with "#<index>" to pick a face inside a collection.
struct font_settings {
    std::vector<std::string> main = {"Segoe UI"};
    std::vector<std::string> monospace = {"Consolas"};
    std::vector<std::string> fallback;
    // Selects regional glyphs (e.g. Han) in system fallback; empty follows the user locale.
    std::string locale = "zh-CN";
    bool system_fallback = true;
    bool color_glyphs = true;
};

void configure_fonts(NVGcontext *nvg, font_settings settings);
void release_fonts(NVGcontext *nvg);
int resolve_font(NVGcontext *nvg, std::string_view family, int weight = 400);
void begin_font_frame(NVGcontext *nvg);
bool font_source_available(std::string_view source);

} // namespace ui
