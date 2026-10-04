#include "breeze_ui/extra_widgets.h"
#include "breeze_ui/widget.h"
#include <atomic>
#include <iostream>

#include "breeze_ui/ui.h"
#include "swcadef.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"

namespace ui {
void acrylic_background_widget::update(update_context &ctx) {
    rect_widget::update(ctx);
}

void acrylic_background_widget::render(nanovg_context ctx) {
    widget::render(ctx);

    if (!owner_rt) {
        std::cerr << "[acrylic host] Missing render target" << std::endl;
        return;
    }

    owner_rt->register_acrylic_region({
        .x = *x + ctx.offset_x,
        .y = *y + ctx.offset_y,
        .width = *width,
        .height = *height,
        .radius = *radius,
        .opacity = *opacity / 255.f,
        .tint = acrylic_bg_color,
    });

    auto bg_color_tmp = bg_color;
    bg_color_tmp.a *= *opacity / 255.f;
    ctx.fillColor(bg_color_tmp);
    ctx.fillRoundedRect(*x, *y, *width, *height, *radius);
}

acrylic_background_widget::~acrylic_background_widget() = default;

namespace {
constexpr wchar_t kDwmAcrylicClassName[] = L"mbui-acrylic-bg";

LRESULT CALLBACK dwm_acrylic_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int window_z_order(HWND hwnd) {
    int z = 1;
    for (HWND h = hwnd; h; h = GetWindow(h, GW_HWNDNEXT)) {
        z++;
    }
    return z;
}
} // namespace

dwm_acrylic_background_widget::dwm_acrylic_background_widget(
    bool dwm_rounded_corners)
    : rect_widget(), dwm_rounded_corners(dwm_rounded_corners) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = dwm_acrylic_wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kDwmAcrylicClassName;
        RegisterClassW(&wc);
        registered = true;
    }
}

dwm_acrylic_background_widget::~dwm_acrylic_background_widget() {
    to_close = true;
    if (render_thread && render_thread->joinable()) {
        render_thread->join();
    }
}

void dwm_acrylic_background_widget::update_color() {
    if (!pSetWindowCompositionAttribute || !hwnd) {
        return;
    }

    ACCENT_POLICY accent = {
        ACCENT_ENABLE_ACRYLICBLURBEHIND,
        Flags::GradientColor | Flags::AllBorder | Flags::AllowSetWindowRgn,
        // GradientColor is BGRA
        ARGB(acrylic_bg_color.a * 255, acrylic_bg_color.b * 255,
             acrylic_bg_color.g * 255, acrylic_bg_color.r * 255),
        0};
    WINDOWCOMPOSITIONATTRIBDATA data = {WCA_ACCENT_POLICY, &accent,
                                        sizeof(accent)};
    pSetWindowCompositionAttribute((HWND)hwnd, &data);
}

void dwm_acrylic_background_widget::update(update_context &ctx) {
    if (!render_thread) {
        auto win = glfwGetCurrentContext();
        if (!win) {
            std::cerr << "[acrylic window] Failed to get current context"
                      << std::endl;
            return;
        }
        auto parent_handle = glfwGetWin32Window(win);
        render_thread = std::thread([=, this]() {
            hwnd = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                    WS_EX_LAYERED | WS_EX_TOPMOST,
                kDwmAcrylicClassName, L"", WS_POPUP, *x, *y, 0, 0, nullptr,
                nullptr, GetModuleHandleW(nullptr), nullptr);

            if (!hwnd) {
                std::cerr << "[acrylic window] Failed to create window "
                          << GetLastError() << std::endl;
                return;
            }

            update_color();

            if (dwm_rounded_corners) {
                auto round_value = *radius > 0 ? DWMWCP_ROUND
                                               : DWMWCP_DONOTROUND;
                DwmSetWindowAttribute((HWND)hwnd,
                                      DWMWA_WINDOW_CORNER_PREFERENCE,
                                      &round_value, sizeof(round_value));
            }

            ShowWindow((HWND)hwnd, SW_SHOW);

            SetWindowPos((HWND)hwnd, parent_handle, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                             SWP_NOREDRAW | SWP_NOSENDCHANGING |
                             SWP_NOCOPYBITS);

            while (true) {
                if (to_close) {
                    ShowWindow((HWND)hwnd, SW_HIDE);
                    DestroyWindow((HWND)hwnd);
                    break;
                }

                RECT rect;
                GetWindowRect(parent_handle, &rect);

                SetWindowPos((HWND)hwnd, nullptr,
                             rect.left + (*x + offset_x) * dpi_scale,
                             rect.top + (*y + offset_y) * dpi_scale,
                             *width * dpi_scale, *height * dpi_scale,
                             SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOOWNERZORDER |
                                 SWP_NOSENDCHANGING | SWP_NOCOPYBITS |
                                 SWP_NOREPOSITION | SWP_NOZORDER);

                auto z_order_this = window_z_order((HWND)hwnd);
                auto z_order_last = window_z_order((HWND)last_hwnd_self);
                auto z_order_parent = window_z_order(parent_handle);

                if (z_order_this < z_order_last ||
                    z_order_parent < z_order_this) {
                    SetWindowPos((HWND)hwnd, parent_handle, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                                     SWP_NOREDRAW | SWP_NOSENDCHANGING |
                                     SWP_NOCOPYBITS);
                }

                SetLayeredWindowAttributes((HWND)hwnd, 0, *opacity, LWA_ALPHA);

                should_update = should_update || *x != current_xywh[0] ||
                                *y != current_xywh[1] ||
                                *width != current_xywh[2] ||
                                *height != current_xywh[3];

                if (!dwm_rounded_corners && should_update) {
                    should_update = false;
                    current_xywh[0] = *x;
                    current_xywh[1] = *y;
                    current_xywh[2] = *width;
                    current_xywh[3] = *height;
                    auto rgn = CreateRoundRectRgn(
                        0, 0, *width * dpi_scale, *height * dpi_scale,
                        *radius * 2 * dpi_scale, *radius * 2 * dpi_scale);
                    SetWindowRgn((HWND)hwnd, rgn, 1);

                    if (rgn) {
                        DeleteObject(rgn);
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(16));

                MSG msg;
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }
        });
    }

    rect_widget::update(ctx);
    dpi_scale = ctx.rt.dpi_scale;
    should_update = should_update || width->updated() || height->updated() ||
                    radius->updated() || x->updated() || y->updated() ||
                    opacity->updated();
    last_hwnd = nullptr;
    if (dwm_rounded_corners) {
        radius->reset_to(8.f);
    }
}

thread_local void *dwm_acrylic_background_widget::last_hwnd = nullptr;

void dwm_acrylic_background_widget::render(nanovg_context ctx) {
    widget::render(ctx);

    PostMessageW((HWND)hwnd, WM_NCHITTEST, 0, 0);

    auto bg_color_tmp = bg_color;
    bg_color_tmp.a *= *opacity / 255.f;
    ctx.fillColor(bg_color_tmp);
    ctx.fillRoundedRect(*x, *y, *width, *height, *radius);
    last_hwnd_self = last_hwnd;
    last_hwnd = hwnd;

    offset_x = ctx.offset_x;
    offset_y = ctx.offset_y;
}

void rect_widget::render(nanovg_context ctx) {
    bg_color.a = *opacity / 255.f;
    ctx.fillColor(bg_color);
    ctx.fillRoundedRect(*x, *y, *width, *height, *radius);
}
rect_widget::rect_widget() : widget() {}
rect_widget::~rect_widget() {}
} // namespace ui
