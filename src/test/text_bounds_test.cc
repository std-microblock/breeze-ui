#include "GLFW/glfw3.h"
#include "glad/glad.h"
#include "nanovg.h"

#define NANOVG_GL3 1
#include "nanovg_gl.h"
#include <cmath>
#include <iostream>
#include <vector>


int main() {
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    // glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window =
        glfwCreateWindow(800, 600, "Text Bounds Test", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        return -1;
    }

    NVGcontext *vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!vg) {
        std::cerr << "Failed to create NanoVG context" << std::endl;
        return -1;
    }

    int font = nvgCreateFont(vg, "sans", "Y:/Windows/Fonts/msyh.ttc");
    if (font == -1) {
        font = nvgCreateFont(vg, "sans", "Y:/Windows/Fonts/simhei.ttf");
    }
    if (font == -1) {
        font = nvgCreateFont(vg, "sans", "Y:/Windows/Fonts/arial.ttf");
    }
    if (font == -1) {
        font = nvgCreateFont(vg, "sans", "C:/Windows/Fonts/msyh.ttc");
    }
    if (font == -1) {
        std::cerr << "Failed to load font" << std::endl;
        return -1;
    }

    const int width = 800;
    const int height = 600;
    const float dpiScale = 1.0f;

    const char *testText = "\xE6\xB5\x8B\xE8\xAF\x95";
    const float fontSize = 48.0f;
    const float testX = 100.0f;
    const float testY = 300.0f;

    glViewport(0, 0, width, height);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, width, height, dpiScale);

    nvgFontSize(vg, fontSize);
    nvgFontFace(vg, "sans");
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

    float bounds[4];
    nvgTextBounds(vg, testX, testY, testText, nullptr, bounds);

    std::cout << "Text bounds: [" << bounds[0] << ", " << bounds[1] << ", "
              << bounds[2] << ", " << bounds[3] << "]" << std::endl;
    std::cout << "Bounds width: " << (bounds[2] - bounds[0])
              << ", height: " << (bounds[3] - bounds[1]) << std::endl;

    nvgBeginPath(vg);
    nvgRect(vg, bounds[0], bounds[1], bounds[2] - bounds[0],
            bounds[3] - bounds[1]);
    nvgFillColor(vg, nvgRGBA(0, 0, 255, 255));
    nvgFill(vg);

    nvgFillColor(vg, nvgRGBA(0, 0, 0, 255));
    nvgText(vg, testX, testY, testText, nullptr);

    nvgEndFrame(vg);

    std::vector<unsigned char> pixels(width * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    int firstBlueY = -1;
    int firstBlackY = -1;

    for (int y = height - 1; y >= 0; y--) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            unsigned char r = pixels[idx];
            unsigned char g = pixels[idx + 1];
            unsigned char b = pixels[idx + 2];
            unsigned char a = pixels[idx + 3];

            if (firstBlueY == -1 && r == 0 && g == 0 && b == 255 && a == 255) {
                firstBlueY = height - 1 - y;
            }

            if (firstBlackY == -1 && r == 0 && g == 0 && b == 0 && a > 128) {
                firstBlackY = height - 1 - y;
            }

            if (firstBlueY != -1 && firstBlackY != -1) {
                break;
            }
        }
        if (firstBlueY != -1 && firstBlackY != -1) {
            break;
        }
    }

    std::cout << "Blue rectangle (text bounds) first Y: " << firstBlueY
              << std::endl;
    std::cout << "Black text first Y: " << firstBlackY << std::endl;

    if (firstBlueY != -1 && firstBlackY != -1) {
        int diff = firstBlackY - firstBlueY;
        std::cout << "Difference: " << diff << " pixels" << std::endl;

        if (std::abs(diff) >= 2) {
            std::cout << "\nWARNING: Text rendering position differs from text "
                         "bounds!"
                      << std::endl;
            std::cout << "Text is " << std::abs(diff) << " pixels "
                      << (diff > 0 ? "lower" : "higher") << " than text bounds"
                      << std::endl;
        } else {
            std::cout << "\nOK: Text rendering position matches text bounds"
                      << std::endl;
        }
    } else {
        std::cout << "\nERROR: Could not detect blue or black pixels"
                  << std::endl;
    }

    glFlush();
    glfwSwapBuffers(window);
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
    }

    nvgDeleteGL3(vg);
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
