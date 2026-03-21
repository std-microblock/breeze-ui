#pragma once

#include "nanovg.h"
#include <windows.h>

#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>

#include <vector>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/base.h>

namespace ui {

struct acrylic_region {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    float radius = 0;
    float opacity = 0;
    NVGcolor tint = nvgRGBAf(0, 0, 0, 0);
};

class acrylic_host {
public:
    acrylic_host() = default;
    ~acrylic_host();

    void update(HWND parent_hwnd, int logical_width, int logical_height,
                float dpi_scale, const std::vector<acrylic_region> &regions);
    void sync(HWND parent_hwnd, int logical_width, int logical_height,
              float dpi_scale);
    void hide();
    void shutdown();

private:
    struct region_visual {
        winrt::Windows::UI::Composition::ContainerVisual container{nullptr};
        winrt::Windows::UI::Composition::SpriteVisual backdrop{nullptr};
        winrt::Windows::UI::Composition::SpriteVisual tint{nullptr};
        winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry
            geometry{nullptr};
        winrt::Windows::UI::Composition::CompositionGeometricClip clip{
            nullptr};
    };

    void ensure_initialized(HWND parent_hwnd);
    void ensure_window(HWND parent_hwnd);
    void ensure_dispatcher_queue();
    void ensure_compositor();
    void ensure_region_count(size_t count);
    void update_region_visual(region_visual &visual,
                              const acrylic_region &region, float dpi_scale);
    void sync_window(HWND parent_hwnd, int logical_width, int logical_height,
                     float dpi_scale, bool visible);
    void pump_messages();

    HWND hwnd_ = nullptr;
    HWND parent_hwnd_ = nullptr;

    winrt::Windows::UI::Composition::Compositor compositor_{nullptr};
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget target_{
        nullptr};
    winrt::Windows::System::DispatcherQueueController
        dispatcher_queue_controller_{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual root_{nullptr};

    std::vector<region_visual> region_visuals_{};
    bool visible_ = false;
};

} // namespace ui
