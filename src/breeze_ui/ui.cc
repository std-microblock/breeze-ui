#include "glad/glad.h"
#include "swcadef.h"
#include <cmath>
#include <dwmapi.h>
#include <future>
#include <imm.h>
#include <print>
#include <stacktrace>
#include <thread>

#define GLFW_INCLUDE_GLEXT
#include "GLFW/glfw3.h"
#define GLFW_EXPOSE_NATIVE_WIN32
#include "GLFW/glfw3native.h"

#include "breeze_ui/ui.h"

#include "breeze_ui/font.h"
#include "breeze_ui/widget.h"

#include "nanovg.h"
#define NANOVG_GL3
#include "nanovg_gl.h"

#include "shellscalingapi.h"
#include "simdutf.h"

namespace ui {
std::atomic_int render_target::view_cnt = 0;
thread_local static bool is_in_loop_thread = false;
constexpr wchar_t kRenderTargetPropName[] = L"breeze_ui_render_target";

std::u32string utf16_to_u32(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const auto *input = reinterpret_cast<const char16_t *>(text.data());
    const auto expected =
        simdutf::utf32_length_from_utf16le(input, text.size());
    std::u32string result(expected, U'\0');
    const auto written =
        simdutf::convert_utf16le_to_utf32(input, text.size(), result.data());
    result.resize(written);
    return result;
}

std::wstring get_ime_string(HIMC himc, DWORD which) {
    const auto byte_count = ImmGetCompositionStringW(himc, which, nullptr, 0);
    if (byte_count <= 0) {
        return {};
    }

    std::wstring buffer(static_cast<size_t>(byte_count / sizeof(wchar_t)),
                        L'\0');
    ImmGetCompositionStringW(himc, which, buffer.data(), byte_count);
    return buffer;
}

void set_ime_composition_state(render_target *rt, ime_composition_state state) {
    std::lock_guard lock(rt->ime_composition_lock);
    rt->ime_composition = std::move(state);
}

void sync_ime_window_position(HWND hwnd, bool active, int caret_x, int caret_y,
                              int caret_height, int document_x, int document_y,
                              int document_width, int document_height) {
    if (!active || GetFocus() != hwnd) {
        return;
    }

    HIMC himc = ImmGetContext(hwnd);
    if (!himc) {
        return;
    }

    COMPOSITIONFORM composition_form = {};
    composition_form.dwStyle = CFS_POINT;
    composition_form.ptCurrentPos = POINT{caret_x, caret_y};
    ImmSetCompositionWindow(himc, &composition_form);

    const int candidate_y = caret_y + std::max(caret_height, 1);
    for (DWORD index = 0; index < 4; ++index) {
        CANDIDATEFORM candidate_form = {};
        candidate_form.dwIndex = index;
        candidate_form.dwStyle = CFS_CANDIDATEPOS;
        candidate_form.ptCurrentPos = POINT{caret_x, candidate_y};
        ImmSetCandidateWindow(himc, &candidate_form);
    }

    CANDIDATEFORM exclude_form = {};
    exclude_form.dwIndex = 0;
    exclude_form.dwStyle = CFS_EXCLUDE;
    exclude_form.ptCurrentPos = POINT{caret_x, candidate_y};
    const int exclude_left = std::clamp(
        caret_x - 1, document_x, document_x + std::max(document_width, 1));
    const int exclude_top = std::clamp(
        caret_y, document_y, document_y + std::max(document_height, 1));
    const int exclude_right =
        std::clamp(caret_x + 1, exclude_left + 1,
                   document_x + std::max(document_width, 1));
    const int exclude_bottom =
        std::clamp(caret_y + std::max(caret_height, 1), exclude_top + 1,
                   document_y + std::max(document_height, 1));
    exclude_form.rcArea =
        RECT{exclude_left, exclude_top, exclude_right, exclude_bottom};
    ImmSetCandidateWindow(himc, &exclude_form);

    ImmReleaseContext(hwnd, himc);
}

RECT screen_rect_from_client(HWND hwnd, int x, int y, int width, int height) {
    POINT top_left{x, y};
    POINT bottom_right{x + width, y + height};
    ClientToScreen(hwnd, &top_left);
    ClientToScreen(hwnd, &bottom_right);
    return RECT{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
}

LRESULT CALLBACK render_target_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
                                       LPARAM lparam) {
    auto rt =
        static_cast<render_target *>(GetPropW(hwnd, kRenderTargetPropName));
    auto original =
        rt ? reinterpret_cast<WNDPROC>(rt->original_wndproc) : DefWindowProcW;

    if (!rt) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    if (msg == WM_IME_SETCONTEXT && wparam) {
        lparam &= ~ISC_SHOWUICOMPOSITIONWINDOW;
        return CallWindowProcW(original, hwnd, msg, wparam, lparam);
    }

    if (msg == WM_IME_REQUEST && wparam == IMR_QUERYCHARPOSITION &&
        rt->ime_caret_active && lparam) {
        auto *position = reinterpret_cast<IMECHARPOSITION *>(lparam);
        POINT caret_point{rt->ime_caret_x, rt->ime_caret_y};
        ClientToScreen(hwnd, &caret_point);

        position->pt = caret_point;
        position->cLineHeight =
            static_cast<UINT>(std::max(rt->ime_caret_height, 1));
        position->rcDocument = screen_rect_from_client(
            hwnd, rt->ime_document_x, rt->ime_document_y,
            rt->ime_document_width, rt->ime_document_height);
        return 1;
    }

    if (msg == WM_KILLFOCUS) {
        rt->clear_ime_composition();
    }

    if (msg == WM_IME_STARTCOMPOSITION) {
        set_ime_composition_state(rt, {.active = true});
        sync_ime_window_position(
            hwnd, rt->ime_caret_active, rt->ime_caret_x, rt->ime_caret_y,
            rt->ime_caret_height, rt->ime_document_x, rt->ime_document_y,
            rt->ime_document_width, rt->ime_document_height);
        return 0;
    }

    if (msg == WM_IME_ENDCOMPOSITION) {
        rt->clear_ime_composition();
        return 0;
    }

    if (msg == WM_IME_COMPOSITION) {
        sync_ime_window_position(
            hwnd, rt->ime_caret_active, rt->ime_caret_x, rt->ime_caret_y,
            rt->ime_caret_height, rt->ime_document_x, rt->ime_document_y,
            rt->ime_document_width, rt->ime_document_height);
        HIMC himc = ImmGetContext(hwnd);
        if (!himc) {
            return 0;
        }

        if (lparam & GCS_RESULTSTR) {
            const auto result =
                utf16_to_u32(get_ime_string(himc, GCS_RESULTSTR));
            if (!result.empty()) {
                auto lock = rt->char_input.get_back_lock();
                rt->char_input.get_back() += result;
            }
            set_ime_composition_state(rt, {});
        } else {
            ime_composition_state state = {.active = true};
            if (lparam & GCS_COMPSTR) {
                state.text = utf16_to_u32(get_ime_string(himc, GCS_COMPSTR));
            }
            if (lparam & GCS_CURSORPOS) {
                state.cursor = static_cast<int>(
                    ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0));
            } else {
                state.cursor = static_cast<int>(state.text.size());
            }
            set_ime_composition_state(rt, std::move(state));
        }

        ImmReleaseContext(hwnd, himc);
        return 0;
    }

    return CallWindowProcW(original, hwnd, msg, wparam, lparam);
}

HMONITOR get_closest_monitor(HWND hwnd) {
    std::vector<std::pair<HMONITOR, MONITORINFO>> monitors;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM lParam) {
            auto &monitors = *reinterpret_cast<
                std::vector<std::pair<HMONITOR, MONITORINFO>> *>(lParam);
            MONITORINFO info;
            info.cbSize = sizeof(MONITORINFO);

            if (GetMonitorInfo(monitor, &info)) {
                monitors.emplace_back(monitor, info);
            }

            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));

    if (monitors.empty()) {
        return nullptr;
    }

    HMONITOR closest_monitor = nullptr;
    RECT window_rect;
    GetWindowRect(hwnd, &window_rect);
    LONG window_center_x = (window_rect.left + window_rect.right) / 2;
    LONG window_center_y = (window_rect.top + window_rect.bottom) / 2;

    LONG min_distance = LONG_MAX;
    for (const auto &[monitor, info] : monitors) {
        LONG monitor_center_x =
            (info.rcMonitor.left + info.rcMonitor.right) / 2;
        LONG monitor_center_y =
            (info.rcMonitor.top + info.rcMonitor.bottom) / 2;
        LONG distance = abs(monitor_center_x - window_center_x) +
                        abs(monitor_center_y - window_center_y);
        if (distance < min_distance) {
            min_distance = distance;
            closest_monitor = monitor;
        }
    }

    return closest_monitor;
}

float get_dpi_scale_from_monitor(HMONITOR monitor) {
    UINT dpi_x, dpi_y;
    if (GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y) != S_OK) {
        return 1.0f;
    }
    return static_cast<float>(dpi_x) / 96.0f;
}

void render_target::start_loop() {
    is_in_loop_thread = true;
    glfwMakeContextCurrent(window);
    while (!glfwWindowShouldClose(window) && !should_loop_stop_hide_as_close) {
        sync_acrylic_host();
        render();
        {
            while (true) {
                std::unique_lock lock(loop_thread_tasks_lock);
                if (loop_thread_tasks.empty()) {
                    break;
                }
                auto fn = std::move(loop_thread_tasks.front());
                loop_thread_tasks.pop();
                lock.unlock();
                if (!fn) {
                    std::print("Warning: empty task posted to loop thread, "
                               "skipping\n");
                    continue;
                }
                try {
                    fn();
                } catch (const std::exception &e) {
                    std::print(
                        "Error: exception thrown in loop thread task: {}\n",
                        e.what());
                } catch (...) {
                    std::print("Error: unknown exception thrown in loop thread "
                               "task\n");
                }
            }
        }
    }
    if (should_loop_stop_hide_as_close) {
        should_loop_stop_hide_as_close = false;
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                GL_STENCIL_BUFFER_BIT);
        glFlush();
        glfwSwapBuffers(window);
        resize(0, 0);
        hide();
        {
            std::lock_guard lock(rt_lock);
            root->children.clear();
        }
    }
    glfwMakeContextCurrent(nullptr);
}
std::expected<bool, std::string> render_target::init() {
    root = std::make_shared<widget>();

    std::ignore = init_global();
    std::promise<void> p;

    render_target::post_main_thread_task([&]() {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_RESIZABLE, resizable);
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, 1);
        if (transparent) {
            glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, 1);
            glfwWindowHint(GLFW_FLOATING, 1);
        } else {
            glfwWindowHint(GLFW_FLOATING, 0);
            glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, 0);
        }
        glfwWindowHint(GLFW_DECORATED, decorated);
        glfwWindowHint(GLFW_VISIBLE, 0);
        glfwWindowHint(GLFW_WIN32_EXSTYLE,
                       topmost && transparent
                           ? (WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED)
                           : WS_EX_APPWINDOW);
        window =
            glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
        glfwMakeContextCurrent(nullptr);
        p.set_value();
    });

    p.get_future().get();

    if (!window) {
        return std::unexpected("Failed to create window");
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(vsync);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        return std::unexpected("Failed to initialize OpenGL loader");
    }

    auto h = glfwGetWin32Window(window);

    if (acrylic || extend) {
        MARGINS margins = {
            .cxLeftWidth = -1,
            .cxRightWidth = -1,
            .cyTopHeight = -1,
            .cyBottomHeight = -1,
        };
        DwmExtendFrameIntoClientArea(h, &margins);
    }
    if (acrylic) {
        DWM_BLURBEHIND bb = {0};
        bb.dwFlags = DWM_BB_ENABLE;
        bb.fEnable = true;
        DwmEnableBlurBehindWindow(h, &bb);

        ACCENT_POLICY accent = {
            ACCENT_ENABLE_ACRYLICBLURBEHIND,
            Flags::AllowSetWindowRgn | Flags::AllBorder | Flags::GradientColor,
            RGB(*acrylic * 255, *acrylic * 255, *acrylic * 255), 0};
        WINDOWCOMPOSITIONATTRIBDATA data = {WCA_ACCENT_POLICY, &accent,
                                            sizeof(accent)};
        pSetWindowCompositionAttribute((HWND)h, &data);

        // dwm round corners
        auto round_value = DWMWCP_ROUND;
        DwmSetWindowAttribute((HWND)h, DWMWA_WINDOW_CORNER_PREFERENCE,
                              &round_value, sizeof(round_value));

    } else {
        DwmEnableBlurBehindWindow(h, nullptr);
    }

    if (no_activate) {
        SetWindowLongPtr(h, GWL_EXSTYLE,
                         GetWindowLongPtr(h, GWL_EXSTYLE) | WS_EX_LAYERED |
                             WS_EX_NOACTIVATE);
        ShowWindow(h, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(h, SW_SHOWNORMAL);
    }
    if (capture_all_input) {
        // retrieve all mouse messages
        SetCapture(h);
    }

    if (topmost) {
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    glfwSetWindowUserPointer(window, this);
    SetPropW(h, kRenderTargetPropName, this);
    original_wndproc = reinterpret_cast<void *>(SetWindowLongPtrW(
        h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(render_target_wndproc)));
    glfwSetFramebufferSizeCallback(
        window, [](GLFWwindow *window, int width, int height) {
            auto rt =
                static_cast<render_target *>(glfwGetWindowUserPointer(window));
            rt->width = width / rt->dpi_scale;
            rt->height = height / rt->dpi_scale;
            rt->reset_view();
        });

    glfwSetWindowFocusCallback(window, [](GLFWwindow *window, int focused) {
        auto thiz =
            static_cast<render_target *>(glfwGetWindowUserPointer(window));
        if (thiz->on_focus_changed) {
            thiz->on_focus_changed.value()(focused);
        }
    });

    glfwSetWindowContentScaleCallback(
        window, [](GLFWwindow *window, float x, float y) {
            auto rt =
                static_cast<render_target *>(glfwGetWindowUserPointer(window));
            rt->dpi_scale = x;
        });

    glfwSetScrollCallback(
        window, [](GLFWwindow *window, double xoffset, double yoffset) {
            auto rt =
                static_cast<render_target *>(glfwGetWindowUserPointer(window));
            rt->scroll_y += yoffset;
        });

    glfwSetKeyCallback(window, [](GLFWwindow *window, int key, int scancode,
                                  int action, int mods) {
        auto rt =
            static_cast<render_target *>(glfwGetWindowUserPointer(window));
        if (key >= 0 && key <= GLFW_KEY_LAST) {
            auto lock = rt->key_states.get_back_lock();
            auto &back = rt->key_states.get_back();
            if (action == GLFW_PRESS) {
                back[key] |= key_state::pressed;
            } else if (action == GLFW_RELEASE) {
                back[key] |= key_state::released;
            } else if (action == GLFW_REPEAT) {
                back[key] |= key_state::repeated;
            }
        }
    });

    glfwSetCharCallback(window, [](GLFWwindow *window, unsigned int codepoint) {
        auto rt =
            static_cast<render_target *>(glfwGetWindowUserPointer(window));
        if (!rt || codepoint == 0) {
            return;
        }
        auto lock = rt->char_input.get_back_lock();
        rt->char_input.get_back().push_back(static_cast<char32_t>(codepoint));
    });

    dpi_scale = get_dpi_scale_from_monitor(
        get_closest_monitor(glfwGetWin32Window(window)));

    reset_view();

    if (!nvg) {
        return std::unexpected("Failed to create NanoVG context");
    }
    glfwMakeContextCurrent(nullptr);
    return true;
}

render_target::~render_target() {
    if (window) {
        auto hwnd = glfwGetWin32Window(window);
        clear_ime_composition();
        if (original_wndproc) {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(original_wndproc));
        }
        RemovePropW(hwnd, kRenderTargetPropName);
    }

    if (acrylic_host_window) {
        acrylic_host_window->shutdown();
    }

    if (nvg) {
        if (window) {
            glfwMakeContextCurrent(window);
        }
        clear_font_registry(nvg);
        nvgDeleteGL3(nvg);
        if (window) {
            glfwMakeContextCurrent(nullptr);
        }
    }

    glfwDestroyWindow(window);
}

thread_local render_target *render_target::current = nullptr;

std::expected<bool, std::string> render_target::init_global() {
    static std::atomic_bool initialized = false;
    if (initialized.exchange(true)) {
        return false;
    }

    std::promise<std::expected<bool, std::string>> res;
    auto future = res.get_future();
    std::thread([&]() {
        if (!glfwInit()) {
            res.set_value(
                std::unexpected(std::string("Failed to initialize GLFW")));
            return;
        }

        glfwPollEvents();
        res.set_value(true);

        while (true) {
            std::function<void()> task;
            {
                std::lock_guard lock(main_thread_tasks_mutex);
                if (!main_thread_tasks.empty()) {
                    task = std::move(main_thread_tasks.front());
                    main_thread_tasks.pop();
                }
            }
            if (task) {
                task();
                continue;
            }

            glfwWaitEvents();
        }
    }).detach();

    return future.get();
}
void render_target::render() {
    int fb_width, fb_height;
    glfwGetFramebufferSize(window, &fb_width, &fb_height);
    glViewport(0, 0, fb_width, fb_height);

    auto now = clock.now();
    auto delta_time =
        1000 * std::chrono::duration<float>(now - last_time).count();
    last_time = now;
    if constexpr (true) {
        static float counter = 0, time_ctr = 0;
        counter++;
        time_ctr += delta_time;
        if (time_ctr > 1000) {
            time_ctr = 0;
            std::printf("FPS: %f\n", counter);
            counter = 0;
        }
    }

    auto begin = clock.now();
    auto ms_steady =
        duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
            .count();
    auto time_checkpoints = [&](const char *name) {
        if constexpr (false) {
            auto end = clock.now();
            auto delta = std::chrono::duration<float>(end - begin).count();
            std::printf("%s: %fms\n", name, delta);
            begin = end;
        }
    };

    nanovg_context vg{nvg, this};
    time_checkpoints("NanoVG context");

    vg.beginFrame(fb_width, fb_height, 1);
    vg.scale(dpi_scale, dpi_scale);

    double mouse_x, mouse_y;
    glfwGetCursorPos(window, &mouse_x, &mouse_y);
    int window_x, window_y;
    glfwGetWindowPos(window, &window_x, &window_y);
    auto monitor = get_closest_monitor(glfwGetWin32Window(window));
    dpi_scale = get_dpi_scale_from_monitor(monitor);
    MONITORINFOEX monitor_info;
    monitor_info.cbSize = sizeof(MONITORINFOEX);
    GetMonitorInfo(monitor, &monitor_info);
    bool need_repaint = false;
    update_context ctx{
        .delta_time = delta_time,
        .mouse_x = mouse_x / dpi_scale,
        .mouse_y = mouse_y / dpi_scale,
        .mouse_down =
            glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS,
        .right_mouse_down =
            glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS,
        .window = window,
        .screen =
            {
                .width =
                    monitor_info.rcMonitor.right - monitor_info.rcMonitor.left,
                .height =
                    monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top,
                .dpi_scale = dpi_scale,
            },
        .scroll_y = scroll_y,
        .need_repaint = need_repaint,
        .rt = *this,
        .vg = vg,
    };
    scroll_y = 0;
    ctx.mouse_clicked = ctx.mouse_down && !mouse_down;
    ctx.right_mouse_clicked = ctx.right_mouse_down && !right_mouse_down;
    ctx.mouse_up = !ctx.mouse_down && mouse_down;
    mouse_down = ctx.mouse_down;
    right_mouse_down = ctx.right_mouse_down;
    set_ime_caret_rect(0, 0, 0, false);
    {
        time_checkpoints("Update context");
        {
            std::lock_guard lock(rt_lock);
            root->owner_rt = this;
            render_target::current = this;
            root->update(ctx);
            key_states.flip();
            char_input.flip();
        }
        time_checkpoints("Update root");
        if (need_repaint || (ms_steady - last_repaint) > 1000) {
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                    GL_STENCIL_BUFFER_BIT);
            last_repaint = ms_steady;
            {
                std::lock_guard lock(rt_lock);
                begin_acrylic_frame();
                root->render(vg);
                commit_acrylic_frame();
            }
            vg.endFrame();
            glFlush();
            glfwSwapBuffers(window);

        } else {
            commit_acrylic_frame();
            if (vsync)
                Sleep(5);
        }
        time_checkpoints("Render root");
    }
}
void render_target::reset_view() {
    if (!nvg)
        nvg = nvgCreateGL3(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
}
void render_target::set_position(int x, int y) {
    glfwSetWindowPos(window, x, y);
}
void render_target::resize(int width, int height) {
    this->width = width;
    this->height = height;
    post_main_thread_task([this] {
        glfwSetWindowSize(window, this->width, this->height);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                GL_STENCIL_BUFFER_BIT);
        glFlush();
        glfwSwapBuffers(window);
    });
}
void render_target::close() {
    if (acrylic_host_window) {
        acrylic_host_window->hide();
    }
    ShowWindow(glfwGetWin32Window(window), SW_HIDE);
    glfwSetWindowShouldClose(window, true);
}

std::queue<std::function<void()>> render_target::main_thread_tasks = {};
std::mutex render_target::main_thread_tasks_mutex = {};
void render_target::post_main_thread_task(std::function<void()> task) {
    std::lock_guard lock(main_thread_tasks_mutex);
    main_thread_tasks.push(std::move(task));
    glfwPostEmptyEvent();
}
void render_target::show() {
    if (no_activate) {
        ShowWindow(glfwGetWin32Window(window), SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(glfwGetWin32Window(window), SW_SHOWNORMAL);
    }

    if (this->parent) {
        SetWindowLongPtr(glfwGetWin32Window(window), GWLP_HWNDPARENT,
                         (LONG_PTR)this->parent);
    }

    if (topmost)
        SetWindowPos(glfwGetWin32Window(window), HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    sync_acrylic_host();
}
void render_target::hide() {
    if (acrylic_host_window) {
        acrylic_host_window->hide();
    }
    ShowWindow(glfwGetWin32Window(window), SW_HIDE);
}
void render_target::hide_as_close() {
    glfwMakeContextCurrent(nullptr);
    should_loop_stop_hide_as_close = true;
    acrylic_regions.clear();
    focused_widget = std::nullopt;
    clear_ime_composition();
    set_ime_caret_rect(0, 0, 0, false);
    // reset owner widget
    SetWindowLong(glfwGetWin32Window(window), GWLP_HWNDPARENT, 0);
    if (acrylic_host_window) {
        acrylic_host_window->clear();
        acrylic_host_window->hide();
    }
}
void render_target::post_loop_thread_task(std::function<void()> task,
                                          bool delay) {
    if (is_in_loop_thread && !delay) {
        task();
        return;
    }
    std::lock_guard lock(loop_thread_tasks_lock);
    loop_thread_tasks.push(std::move(task));
}
void render_target::focus() {
    if (this->window) {
        if (!no_activate) {
            glfwFocusWindow(this->window);
            SetActiveWindow(glfwGetWin32Window(this->window));
        }
        SetFocus(glfwGetWin32Window(this->window));
    }
}
void render_target::set_ime_caret_rect(float x, float y, float height,
                                       bool active, float document_x,
                                       float document_y, float document_width,
                                       float document_height) {
    ime_caret_active = active;
    if (!active) {
        ime_caret_x = 0;
        ime_caret_y = 0;
        ime_caret_height = 0;
        ime_document_x = 0;
        ime_document_y = 0;
        ime_document_width = 0;
        ime_document_height = 0;
        return;
    }

    ime_caret_x = static_cast<int>(std::lround(x * dpi_scale));
    ime_caret_y = static_cast<int>(std::lround(y * dpi_scale));
    ime_caret_height =
        static_cast<int>(std::max(std::lround(height * dpi_scale), 1L));
    ime_document_x = static_cast<int>(std::lround(document_x * dpi_scale));
    ime_document_y = static_cast<int>(std::lround(document_y * dpi_scale));
    ime_document_width =
        static_cast<int>(std::max(std::lround(document_width * dpi_scale), 1L));
    ime_document_height = static_cast<int>(
        std::max(std::lround(document_height * dpi_scale), 1L));

    if (!window) {
        return;
    }

    auto hwnd = glfwGetWin32Window(window);
    const int caret_x = ime_caret_x;
    const int caret_y = ime_caret_y;
    const int caret_height = ime_caret_height;
    const int doc_x = ime_document_x;
    const int doc_y = ime_document_y;
    const int doc_width = ime_document_width;
    const int doc_height = ime_document_height;
    post_main_thread_task([hwnd, owner = reinterpret_cast<HANDLE>(this),
                           caret_x, caret_y, caret_height, doc_x, doc_y,
                           doc_width, doc_height] {
        if (!IsWindow(hwnd) || GetPropW(hwnd, kRenderTargetPropName) != owner) {
            return;
        }
        sync_ime_window_position(hwnd, true, caret_x, caret_y, caret_height,
                                 doc_x, doc_y, doc_width, doc_height);
    });
}
void render_target::clear_ime_composition() {
    std::lock_guard lock(ime_composition_lock);
    ime_composition = {};
}
void *render_target::hwnd() const {
    return window ? glfwGetWin32Window(window) : nullptr;
}

void render_target::begin_acrylic_frame() { acrylic_regions.clear(); }

void render_target::register_acrylic_region(acrylic_region region) {
    if (!window || region.width <= 0 || region.height <= 0 ||
        region.opacity <= 0) {
        return;
    }

    if (!acrylic_host_window) {
        acrylic_host_window = std::make_unique<acrylic_host>();
    }

    acrylic_regions.push_back(std::move(region));
}

void render_target::commit_acrylic_frame() {
    if (!acrylic_host_window) {
        return;
    }

    acrylic_host_window->update(glfwGetWin32Window(window), width, height,
                                dpi_scale, acrylic_regions);
}

void render_target::sync_acrylic_host() {
    if (!acrylic_host_window || !window) {
        return;
    }

    acrylic_host_window->sync(glfwGetWin32Window(window), width, height,
                              dpi_scale);
}
} // namespace ui
