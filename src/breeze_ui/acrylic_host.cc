#include "breeze_ui/acrylic_host.h"

#include <algorithm>
#include <dwmapi.h>

#include <windows.ui.composition.interop.h>

#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.h>

namespace ui {
namespace {
using namespace winrt;
using namespace Windows::Foundation::Numerics;
using namespace Windows::System;
using namespace Windows::UI;
using namespace Windows::UI::Composition;
using namespace Windows::UI::Composition::Desktop;

constexpr wchar_t kAcrylicHostClassName[] = L"mbui-acrylic-host";

RECT get_client_screen_rect(HWND hwnd) {
    RECT client_rect{};
    if (!GetClientRect(hwnd, &client_rect)) {
        return {};
    }

    POINT top_left{client_rect.left, client_rect.top};
    POINT bottom_right{client_rect.right, client_rect.bottom};
    if (!ClientToScreen(hwnd, &top_left) || !ClientToScreen(hwnd, &bottom_right)) {
        return {};
    }

    return RECT{
        .left = top_left.x,
        .top = top_left.y,
        .right = bottom_right.x,
        .bottom = bottom_right.y,
    };
}

uint8_t to_byte(float value) {
    return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
}

Color to_color(const NVGcolor &color) {
    return Color{
        .A = to_byte(color.a),
        .R = to_byte(color.r),
        .G = to_byte(color.g),
        .B = to_byte(color.b),
    };
}

LRESULT CALLBACK AcrylicHostWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
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

void ensure_host_window_class() {
    static bool registered = false;
    if (registered) {
        return;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = AcrylicHostWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kAcrylicHostClassName;
    RegisterClassW(&wc);
    registered = true;
}
} // namespace

acrylic_host::~acrylic_host() { shutdown(); }

void acrylic_host::update(HWND parent_hwnd, int logical_width,
                          int logical_height, float dpi_scale,
                          const std::vector<acrylic_region> &regions) {
    ensure_initialized(parent_hwnd);
    if (!hwnd_ || !root_) {
        return;
    }

    ensure_region_count(regions.size());
    root_.Size(
        float2{logical_width * dpi_scale, logical_height * dpi_scale});
    root_.Children().RemoveAll();

    for (size_t i = 0; i < regions.size(); ++i) {
        update_region_visual(region_visuals_[i], regions[i], dpi_scale);
    }

    for (size_t i = regions.size(); i-- > 0;) {
        root_.Children().InsertAtTop(region_visuals_[i].container);
    }

    sync_window(parent_hwnd, logical_width, logical_height, dpi_scale,
                !regions.empty());
    pump_messages();
}

void acrylic_host::sync(HWND parent_hwnd, int logical_width, int logical_height,
                        float dpi_scale) {
    if (!hwnd_) {
        return;
    }

    sync_window(parent_hwnd, logical_width, logical_height, dpi_scale, visible_);
    pump_messages();
}

void acrylic_host::hide() {
    visible_ = false;
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void acrylic_host::shutdown() {
    hide();
    region_visuals_.clear();
    root_ = nullptr;
    target_ = nullptr;
    compositor_ = nullptr;
    dispatcher_queue_controller_ = nullptr;

    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    parent_hwnd_ = nullptr;
}

void acrylic_host::ensure_initialized(HWND parent_hwnd) {
    ensure_window(parent_hwnd);
    ensure_dispatcher_queue();
    ensure_compositor();
}

void acrylic_host::ensure_window(HWND parent_hwnd) {
    if (hwnd_) {
        return;
    }

    ensure_host_window_class();

    RECT rect = get_client_screen_rect(parent_hwnd);
    hwnd_ = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_TRANSPARENT,
        kAcrylicHostClassName, L"", WS_POPUP, rect.left, rect.top,
        std::max(rect.right - rect.left, 1L),
        std::max(rect.bottom - rect.top, 1L), nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    parent_hwnd_ = parent_hwnd;

    if (!hwnd_) {
        return;
    }

    const BOOL enable_backdrop = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_HOSTBACKDROPBRUSH,
                          &enable_backdrop, sizeof(enable_backdrop));
}

void acrylic_host::ensure_dispatcher_queue() {
    if (dispatcher_queue_controller_) {
        return;
    }

    static thread_local bool apartment_initialized = false;
    if (!apartment_initialized) {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        apartment_initialized = true;
    }

    namespace abi = ABI::Windows::System;

    DispatcherQueueOptions options{
        sizeof(DispatcherQueueOptions),
        DQTYPE_THREAD_CURRENT,
        DQTAT_COM_STA,
    };

    DispatcherQueueController controller{nullptr};
    check_hresult(CreateDispatcherQueueController(
        options,
        reinterpret_cast<abi::IDispatcherQueueController **>(
            put_abi(controller))));

    dispatcher_queue_controller_ = controller;
}

void acrylic_host::ensure_compositor() {
    if (compositor_) {
        return;
    }

    namespace abi = ABI::Windows::UI::Composition::Desktop;

    compositor_ = Compositor();
    auto interop = compositor_.as<abi::ICompositorDesktopInterop>();

    DesktopWindowTarget target{nullptr};
    check_hresult(interop->CreateDesktopWindowTarget(
        hwnd_, true,
        reinterpret_cast<abi::IDesktopWindowTarget **>(put_abi(target))));

    target_ = target;
    root_ = compositor_.CreateContainerVisual();
    target_.Root(root_);
}

void acrylic_host::ensure_region_count(size_t count) {
    while (region_visuals_.size() < count) {
        region_visual visual{};
        visual.container = compositor_.CreateContainerVisual();
        visual.backdrop = compositor_.CreateSpriteVisual();
        visual.tint = compositor_.CreateSpriteVisual();
        visual.geometry = compositor_.CreateRoundedRectangleGeometry();
        visual.clip = compositor_.CreateGeometricClip(visual.geometry);

        visual.container.Clip(visual.clip);
        visual.backdrop.Brush(compositor_.CreateHostBackdropBrush());
        visual.container.Children().InsertAtTop(visual.tint);
        visual.container.Children().InsertAtBottom(visual.backdrop);
        region_visuals_.push_back(std::move(visual));
    }

    if (region_visuals_.size() > count) {
        region_visuals_.resize(count);
    }
}

void acrylic_host::update_region_visual(region_visual &visual,
                                        const acrylic_region &region,
                                        float dpi_scale) {
    const float width = std::max(region.width * dpi_scale, 0.0f);
    const float height = std::max(region.height * dpi_scale, 0.0f);
    const float radius = std::clamp(region.radius * dpi_scale, 0.0f,
                                    std::min(width, height) / 2.0f);
    const float opacity = std::clamp(region.opacity, 0.0f, 1.0f);

    visual.container.Offset(
        float3{region.x * dpi_scale, region.y * dpi_scale, 0.0f});
    visual.container.Opacity(opacity);

    visual.geometry.Offset({0.0f, 0.0f});
    visual.geometry.Size({width, height});
    visual.geometry.CornerRadius({radius, radius});

    visual.backdrop.Size({width, height});
    visual.tint.Size({width, height});
    visual.tint.Brush(compositor_.CreateColorBrush(to_color(region.tint)));
}

void acrylic_host::sync_window(HWND parent_hwnd, int logical_width,
                               int logical_height, float dpi_scale,
                               bool visible) {
    if (!hwnd_ || !parent_hwnd) {
        return;
    }

    RECT rect = get_client_screen_rect(parent_hwnd);
    const int width = std::max(static_cast<int>(logical_width * dpi_scale), 1);
    const int height =
        std::max(static_cast<int>(logical_height * dpi_scale), 1);

    const auto flags =
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING | SWP_SHOWWINDOW;
    SetWindowPos(hwnd_, parent_hwnd, rect.left, rect.top, width, height, flags);

    if (visible && IsWindowVisible(parent_hwnd) && !IsIconic(parent_hwnd)) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        visible_ = true;
    } else {
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
    }
}

void acrylic_host::pump_messages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

} // namespace ui
