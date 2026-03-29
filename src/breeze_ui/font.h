#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct NVGcontext;

namespace ui {

struct font_face_source {
    std::filesystem::path path;
    int collection_index = 0;
};

struct weighted_font_face {
    int weight = 400;
    font_face_source source;
};

struct font_family_definition {
    std::string family_name;
    std::vector<weighted_font_face> faces;
    std::vector<std::string> fallback_families;
};

struct windows_font_face_candidate {
    int weight = 400;
    std::string file_name;
    int collection_index = 0;
};

struct default_windows_font_suite_definition {
    font_face_source main_regular;
    font_face_source fallback_regular;
    font_face_source monospace_regular;
};

std::filesystem::path windows_font_directory();
font_family_definition
make_windows_font_family(std::string family_name,
                         std::span<const windows_font_face_candidate> candidates,
                         std::vector<std::string> fallback_families = {});
bool register_font_family(NVGcontext *nvg,
                          const font_family_definition &definition);
void clear_font_registry(NVGcontext *nvg);
std::string resolve_font_face_name(NVGcontext *nvg, std::string_view family_name,
                                   int weight = 400);
void register_default_windows_font_suite(
    NVGcontext *nvg, const default_windows_font_suite_definition &suite);

} // namespace ui
