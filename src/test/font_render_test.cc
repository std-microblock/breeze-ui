#include "GLFW/glfw3.h"
#include "glad/glad.h"
#include "nanovg.h"

#define NANOVG_GL3 1
#include "nanovg_gl.h"

#include "breeze_ui/font.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = 0) {
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void put_u32(std::vector<uint8_t> &out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void put_chunk(std::vector<uint8_t> &out, const char *type,
               const std::vector<uint8_t> &data) {
    put_u32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put_u32(out, crc32(body.data(), body.size()));
}

bool write_png(const char *path, int width, int height,
               const std::vector<uint8_t> &rgba_bottom_up) {
    std::vector<uint8_t> raw;
    for (int y = height - 1; y >= 0; --y) {
        raw.push_back(0);
        auto *row = rgba_bottom_up.data() + static_cast<size_t>(y) * width * 4;
        raw.insert(raw.end(), row, row + width * 4);
    }
    std::vector<uint8_t> zlib = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (auto byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t pos = 0; pos < raw.size();) {
        const auto len = std::min<size_t>(65535, raw.size() - pos);
        zlib.push_back(pos + len == raw.size() ? 1 : 0);
        zlib.push_back(static_cast<uint8_t>(len));
        zlib.push_back(static_cast<uint8_t>(len >> 8));
        zlib.push_back(static_cast<uint8_t>(~len));
        zlib.push_back(static_cast<uint8_t>(~len >> 8));
        zlib.insert(zlib.end(), raw.begin() + pos, raw.begin() + pos + len);
        pos += len;
    }
    put_u32(zlib, (b << 16) | a);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> header;
    put_u32(header, width);
    put_u32(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    put_chunk(png, "IHDR", header);
    put_chunk(png, "IDAT", zlib);
    put_chunk(png, "IEND", {});
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(png.data()), png.size());
    return static_cast<bool>(file);
}

struct sample {
    const char *family;
    int weight;
    float size;
    const char *text;
};

} // namespace

int main(int argc, char **argv) {
    const char *output = argc > 1 ? argv[1] : "font_render_test.png";
    if (!glfwInit()) {
        return 1;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    const int width = 900;
    const int height = 620;
    auto *window = glfwCreateWindow(width, height, "font", nullptr, nullptr);
    if (!window) {
        return 1;
    }
    glfwMakeContextCurrent(window);
    gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress));
    auto *vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);

    ui::configure_fonts(vg, {.monospace = {"Cascadia Mono", "Consolas"}});

    const sample samples[] = {
        {"main", 400, 20, "Segoe UI regular: The quick brown fox 0123"},
        {"main", 700, 20, "Bold: The quick brown fox / \xE7\xB2\x97\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87"},
        {"main", 300, 20, "Light: \xE6\xB5\x85\xE8\x89\xB2\xE4\xB8\xAD\xE6\x96\x87 Light"},
        {"main", 400, 20, "\xE4\xB8\xAD\xE6\x96\x87 \xE3\x81\xB2\xE3\x82\x89\xE3\x81\x8C\xE3\x81\xAA \xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4 \xE0\xB9\x84\xE0\xB8\x97\xE0\xB8\xA2 \xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"},
        {"main", 400, 28, "\xE7\x9B\xB4\xE9\xAA\xA8\xE8\xA7\x92\xE5\x86\x99\xE8\xBF\x87\xE5\x86\x85 Han glyph region"},
        {"main", 400, 28, "Emoji: \xF0\x9F\x98\x80 \xF0\x9F\x8E\x89 \xF0\x9F\x91\x8D \xF0\x9F\x94\xA5 \xE2\x9D\xA4 \xF0\x9F\x90\xB1 \xF0\x9F\x8D\x95 \xF0\x9F\x9A\x80"},
        {"main", 400, 14, "Small emoji 14px \xF0\x9F\x93\x81 \xF0\x9F\x93\x84 \xE2\x9C\x85 inline"},
        {"main", 400, 20, "VS16 \xE2\x9D\xA4\xEF\xB8\x8F| zwsp a\xE2\x80\x8B" "b| soft\xC2\xAD" "hyphen"},
        {"main", 400, 20, "Symbols: \xE2\x86\x92 \xE2\x9C\x93 \xE2\x98\x85 \xE2\x99\xAB \xE2\x8C\x98 \xE2\x88\x91 \xE2\x84\x96"},
        {"monospace", 400, 18, "mono: fn main() { let x = 42; } // \xE6\xB3\xA8\xE9\x87\x8A"},
        {"Cascadia Code", 400, 18, "Cascadia Code by family name -> != =>"},
        {"C:/Windows/Fonts/georgia.ttf", 400, 20, "Georgia by path \xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x8E"},
        {"Times New Roman", 700, 20, "Times New Roman Bold"},
        {"NoSuchFont", 400, 20, "Unknown family falls back to main \xF0\x9F\x8C\x88"},
    };

    for (int pass = 0; pass < 2; ++pass) {
        ui::begin_font_frame(vg);
        glViewport(0, 0, width, height);
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        nvgBeginFrame(vg, width, height, 1);
        float y = 16;
        for (const auto &item : samples) {
            nvgFontFaceId(vg, ui::resolve_font(vg, item.family, item.weight));
            nvgFontSize(vg, item.size);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            float bounds[4];
            nvgTextBounds(vg, 16, y, item.text, nullptr, bounds);
            nvgBeginPath(vg);
            nvgRect(vg, bounds[0], bounds[1], bounds[2] - bounds[0],
                    bounds[3] - bounds[1]);
            nvgFillColor(vg, nvgRGBA(0, 120, 255, 30));
            nvgFill(vg);
            nvgFillColor(vg, nvgRGBA(20, 20, 20, 255));
            nvgText(vg, 16, y, item.text, nullptr);
            y += item.size * 1.6f + 6;
        }
        nvgEndFrame(vg);
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const bool ok = write_png(output, width, height, pixels);
    std::printf("%s %s\n", ok ? "wrote" : "failed", output);

    ui::release_fonts(vg);
    nvgDeleteGL3(vg);
    glfwDestroyWindow(window);
    glfwTerminate();
    return ok ? 0 : 1;
}
