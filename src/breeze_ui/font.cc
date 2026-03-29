#include "breeze_ui/font.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>

#include "nanovg.h"
#include "windows.h"

namespace {

struct registered_font_face {
    int weight = 400;
    std::string face_name;
};

struct registered_font_family {
    std::vector<registered_font_face> faces;
    std::vector<std::string> fallback_families;
    std::string alias_face_name;
};

struct font_registry {
    std::unordered_map<std::string, registered_font_family> families;
};

std::mutex g_font_registry_mutex;
std::unordered_map<NVGcontext *, font_registry> g_font_registries;

std::string to_lower_ascii(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool same_filename(std::filesystem::path path, std::string_view file_name) {
    return to_lower_ascii(path.filename().string()) == to_lower_ascii(file_name);
}

std::string make_face_name(std::string_view family_name, int weight) {
    return std::string(family_name) + "@w" + std::to_string(weight);
}

const registered_font_face *
find_best_face(const registered_font_family &family, int requested_weight) {
    if (family.faces.empty()) {
        return nullptr;
    }

    const registered_font_face *best = &family.faces.front();
    for (const auto &face : family.faces) {
        const auto current_diff = std::abs(face.weight - requested_weight);
        const auto best_diff = std::abs(best->weight - requested_weight);
        if (current_diff < best_diff) {
            best = &face;
            continue;
        }
        if (current_diff > best_diff) {
            continue;
        }

        if (requested_weight >= 500) {
            if (face.weight > best->weight) {
                best = &face;
            }
        } else if (face.weight < best->weight) {
            best = &face;
        }
    }
    return best;
}

void rebuild_fallbacks_locked(NVGcontext *nvg, font_registry &registry) {
    for (const auto &[family_name, family] : registry.families) {
        for (const auto &face : family.faces) {
            nvgResetFallbackFonts(nvg, face.face_name.c_str());
            for (const auto &fallback_name : family.fallback_families) {
                const auto fallback_it = registry.families.find(fallback_name);
                if (fallback_it == registry.families.end()) {
                    continue;
                }

                const auto *fallback_face =
                    find_best_face(fallback_it->second, face.weight);
                if (!fallback_face) {
                    continue;
                }

                nvgAddFallbackFont(nvg, face.face_name.c_str(),
                                   fallback_face->face_name.c_str());
            }
        }

        if (!family.alias_face_name.empty()) {
            nvgResetFallbackFonts(nvg, family.alias_face_name.c_str());
            for (const auto &fallback_name : family.fallback_families) {
                const auto fallback_it = registry.families.find(fallback_name);
                if (fallback_it == registry.families.end()) {
                    continue;
                }

                const auto *fallback_face =
                    find_best_face(fallback_it->second, 400);
                if (!fallback_face) {
                    continue;
                }

                const auto fallback_alias_name =
                    fallback_it->second.alias_face_name.empty()
                        ? fallback_face->face_name
                        : fallback_it->second.alias_face_name;
                nvgAddFallbackFont(nvg, family.alias_face_name.c_str(),
                                   fallback_alias_name.c_str());
            }
        }
    }
}

std::vector<ui::weighted_font_face>
dedupe_faces(std::vector<ui::weighted_font_face> faces) {
    std::ranges::sort(faces, {}, &ui::weighted_font_face::weight);
    std::vector<ui::weighted_font_face> result;
    for (auto &face : faces) {
        if (face.source.path.empty() || !std::filesystem::exists(face.source.path)) {
            continue;
        }
        if (!result.empty() && result.back().weight == face.weight) {
            result.back() = std::move(face);
        } else {
            result.push_back(std::move(face));
        }
    }
    return result;
}

std::vector<ui::weighted_font_face> build_default_family_faces(
    const ui::font_face_source &regular_face,
    std::string_view regular_file_name,
    std::span<const ui::windows_font_face_candidate> system_candidates) {
    std::vector<ui::weighted_font_face> faces;
    if (regular_face.path.empty()) {
        return faces;
    }

    faces.push_back({.weight = 400, .source = regular_face});
    if (!same_filename(regular_face.path, regular_file_name)) {
        return faces;
    }

    const auto windows_dir = ui::windows_font_directory();
    for (const auto &candidate : system_candidates) {
        if (candidate.weight == 400) {
            continue;
        }
        auto path = windows_dir / candidate.file_name;
        if (!std::filesystem::exists(path)) {
            continue;
        }
        faces.push_back(
            {.weight = candidate.weight,
             .source = {.path = std::move(path),
                        .collection_index = candidate.collection_index}});
    }
    return faces;
}

} // namespace

namespace ui {

std::filesystem::path windows_font_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    const auto written = GetWindowsDirectoryW(buffer.data(),
                                              static_cast<UINT>(buffer.size()));
    if (written > 0) {
        buffer.resize(written);
        return std::filesystem::path(buffer) / "Fonts";
    }
    return std::filesystem::path("C:\\Windows\\Fonts");
}

font_family_definition
make_windows_font_family(std::string family_name,
                         std::span<const windows_font_face_candidate> candidates,
                         std::vector<std::string> fallback_families) {
    font_family_definition definition{
        .family_name = std::move(family_name),
        .fallback_families = std::move(fallback_families),
    };

    const auto font_dir = windows_font_directory();
    for (const auto &candidate : candidates) {
        auto path = font_dir / candidate.file_name;
        if (!std::filesystem::exists(path)) {
            continue;
        }
        definition.faces.push_back(
            {.weight = candidate.weight,
             .source = {.path = std::move(path),
                        .collection_index = candidate.collection_index}});
    }
    return definition;
}

bool register_font_family(NVGcontext *nvg,
                          const font_family_definition &definition) {
    if (!nvg || definition.family_name.empty()) {
        return false;
    }

    auto faces = dedupe_faces(definition.faces);
    if (faces.empty()) {
        return false;
    }

    registered_font_family family;
    family.fallback_families = definition.fallback_families;
    int default_weight = faces.front().weight;
    int default_diff = std::abs(default_weight - 400);
    for (const auto &face : faces) {
        const auto current_diff = std::abs(face.weight - 400);
        if (current_diff < default_diff ||
            (current_diff == default_diff && face.weight < default_weight)) {
            default_weight = face.weight;
            default_diff = current_diff;
        }
    }
    for (const auto &face : faces) {
        auto face_name = make_face_name(definition.family_name, face.weight);
        const auto font_path = face.source.path.string();
        const auto font_id =
            face.source.collection_index == 0
                ? nvgCreateFont(nvg, face_name.c_str(), font_path.c_str())
                : nvgCreateFontAtIndex(nvg, face_name.c_str(), font_path.c_str(),
                                       face.source.collection_index);
        if (font_id < 0) {
            continue;
        }

        family.faces.push_back(
            {.weight = face.weight, .face_name = std::move(face_name)});

        if (face.weight == default_weight && family.alias_face_name.empty()) {
            const auto alias_id =
                face.source.collection_index == 0
                    ? nvgCreateFont(nvg, definition.family_name.c_str(),
                                    font_path.c_str())
                    : nvgCreateFontAtIndex(nvg, definition.family_name.c_str(),
                                           font_path.c_str(),
                                           face.source.collection_index);
            if (alias_id >= 0) {
                family.alias_face_name = definition.family_name;
            }
        }
    }

    if (family.faces.empty()) {
        return false;
    }

    std::lock_guard lock(g_font_registry_mutex);
    auto &registry = g_font_registries[nvg];
    registry.families[definition.family_name] = std::move(family);
    rebuild_fallbacks_locked(nvg, registry);
    return true;
}

void clear_font_registry(NVGcontext *nvg) {
    std::lock_guard lock(g_font_registry_mutex);
    g_font_registries.erase(nvg);
}

std::string resolve_font_face_name(NVGcontext *nvg, std::string_view family_name,
                                   int weight) {
    if (!nvg || family_name.empty()) {
        return {};
    }

    std::lock_guard lock(g_font_registry_mutex);
    const auto registry_it = g_font_registries.find(nvg);
    if (registry_it == g_font_registries.end()) {
        return std::string(family_name);
    }

    const auto family_it =
        registry_it->second.families.find(std::string(family_name));
    if (family_it == registry_it->second.families.end()) {
        return std::string(family_name);
    }

    const auto *face = find_best_face(family_it->second, weight);
    return face ? face->face_name : std::string(family_name);
}

void register_default_windows_font_suite(
    NVGcontext *nvg, const default_windows_font_suite_definition &suite) {
    static constexpr windows_font_face_candidate main_candidates[] = {
        {.weight = 200, .file_name = "segoeuisl.ttf"},
        {.weight = 300, .file_name = "segoeuil.ttf"},
        {.weight = 400, .file_name = "segoeui.ttf"},
        {.weight = 600, .file_name = "seguisb.ttf"},
        {.weight = 700, .file_name = "segoeuib.ttf"},
    };
    static constexpr windows_font_face_candidate fallback_candidates[] = {
        {.weight = 300, .file_name = "msyhl.ttc"},
        {.weight = 400, .file_name = "msyh.ttc"},
        {.weight = 700, .file_name = "msyhbd.ttc"},
    };
    static constexpr windows_font_face_candidate monospace_candidates[] = {
        {.weight = 400, .file_name = "consola.ttf"},
        {.weight = 700, .file_name = "consolab.ttf"},
    };

    register_font_family(
        nvg,
        {.family_name = "fallback",
         .faces = build_default_family_faces(suite.fallback_regular, "msyh.ttc",
                                             fallback_candidates)});
    register_font_family(
        nvg, {.family_name = "main",
              .faces = build_default_family_faces(suite.main_regular,
                                                  "segoeui.ttf", main_candidates),
              .fallback_families = {"fallback"}});
    register_font_family(
        nvg,
        {.family_name = "monospace",
         .faces = build_default_family_faces(suite.monospace_regular,
                                             "consola.ttf", monospace_candidates),
         .fallback_families = {"main", "fallback"}});
}

} // namespace ui
