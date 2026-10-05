#include "glad/glad.h"
#include "swcadef.h"
#include <cmath>
#include <dwmapi.h>
#include <future>
#include <imm.h>
#include <mutex>
#include <optional>
#include <print>
#include <stacktrace>
#include <thread>
#include <unordered_map>

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
    {
        std::lock_guard lock(rt->ime_composition_lock);
        rt->ime_composition = std::move(state);
    }
    rt->ime_composition_dirty = true;
    rt->request_frame();
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

bool system_uses_light_theme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}

void apply_titlebar_theme(HWND hwnd) {
    constexpr DWORD kImmersiveDarkMode = 20;
    constexpr DWORD kImmersiveDarkModeBefore20H1 = 19;
    BOOL dark = !system_uses_light_theme();
    if (FAILED(DwmSetWindowAttribute(hwnd, kImmersiveDarkMode, &dark,
                                     sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, kImmersiveDarkModeBefore20H1, &dark,
                              sizeof(dark));
    }
}

struct module_icon_resource {
    HMODULE module = nullptr;
    std::wstring name;
    WORD id = 0;

    LPCWSTR resource() const {
        return id ? MAKEINTRESOURCEW(id) : name.c_str();
    }
};

std::optional<module_icon_resource> find_own_module_icon() {
    module_icon_resource icon;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&find_own_module_icon),
                       &icon.module);
    if (!icon.module) {
        return std::nullopt;
    }
    EnumResourceNamesW(
        icon.module, reinterpret_cast<LPCWSTR>(RT_GROUP_ICON),
        [](HMODULE, LPCWSTR, LPWSTR name, LONG_PTR param) -> BOOL {
            auto &icon = *reinterpret_cast<module_icon_resource *>(param);
            if (IS_INTRESOURCE(name)) {
                icon.id = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(name));
            } else {
                icon.name = name;
            }
            return FALSE;
        },
        reinterpret_cast<LONG_PTR>(&icon));
    if (!icon.id && icon.name.empty()) {
        return std::nullopt;
    }
    return icon;
}

HICON load_own_module_icon(int size) {
    static const auto resource = find_own_module_icon();
    static std::mutex cache_lock;
    static std::unordered_map<int, HICON> cache;
    if (!resource) {
        return nullptr;
    }
    std::lock_guard lock(cache_lock);
    auto &icon = cache[size];
    if (!icon) {
        icon = static_cast<HICON>(LoadImageW(resource->module,
                                             resource->resource(), IMAGE_ICON,
                                             size, size, LR_DEFAULTCOLOR));
    }
    return icon;
}

void apply_window_icon(HWND hwnd, UINT dpi) {
    for (auto [type, metric] : {std::pair{ICON_SMALL, SM_CXSMICON},
                                std::pair{ICON_BIG, SM_CXICON}}) {
        if (auto icon =
                load_own_module_icon(GetSystemMetricsForDpi(metric, dpi))) {
            SendMessageW(hwnd, WM_SETICON, type,
                         reinterpret_cast<LPARAM>(icon));
        }
    }
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

    if (msg == WM_SETTINGCHANGE && rt->decorated && lparam &&
        std::wstring_view(reinterpret_cast<LPCWSTR>(lparam)) ==
            L"ImmersiveColorSet") {
        apply_titlebar_theme(hwnd);
    }

    if (msg == WM_DPICHANGED && rt->decorated) {
        apply_window_icon(hwnd, HIWORD(wparam));
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
                for (auto ch : result) {
                    rt->push_input({.type = input_record::kind::character,
                                    .code = static_cast<int>(ch)});
                }
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

float get_dpi_scale_from_monitor(HMONITOR monitor) {
    UINT dpi_x, dpi_y;
    if (GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y) != S_OK) {
        return 1.0f;
    }
    return static_cast<float>(dpi_x) / 96.0f;
}

void tree_lock::unlock() {
    mutex.unlock();
    if (owner && !is_in_loop_thread) {
        owner->request_frame();
    }
}

void render_target::request_frame() {
    {
        std::lock_guard lock(frame_mutex);
        frame_requested = true;
    }
    frame_cv.notify_one();
}

void render_target::schedule_frame(float delay_ms) {
    auto at = clock.now() + std::chrono::microseconds(
                                static_cast<int64_t>(delay_ms * 1000));
    {
        std::lock_guard lock(frame_mutex);
        if (!scheduled_frame || at < *scheduled_frame) {
            scheduled_frame = at;
        }
    }
    frame_cv.notify_one();
}

void render_target::wait_for_frame() {
    std::unique_lock lock(frame_mutex);
    auto now = clock.now();
    auto deadline = now + std::chrono::milliseconds(1000);
    if (idle_poll_ms > 0) {
        deadline = std::min(deadline,
                            now + std::chrono::milliseconds(idle_poll_ms));
    }
    if (scheduled_frame) {
        deadline = std::min(deadline, *scheduled_frame);
    }
    frame_cv.wait_until(lock, deadline, [&] { return frame_requested; });
    frame_requested = false;
    if (scheduled_frame && *scheduled_frame <= clock.now()) {
        scheduled_frame.reset();
    }
}

void render_target::push_input(input_record record) {
    {
        std::lock_guard lock(input_lock);
        pending_input.push_back(record);
    }
    request_frame();
}

bool render_target::key_down(int key) const {
    return window && glfwGetKey(window, key) == GLFW_PRESS;
}

bool render_target::is_hovered(const widget *w) const {
    return std::ranges::find(hover_chain, w) != hover_chain.end();
}

widget *render_target::hovered_widget() const {
    return hover_chain.empty() ? nullptr : hover_chain.front();
}

void render_target::refresh_screen_info() {
    auto monitor =
        MonitorFromWindow(glfwGetWin32Window(window), MONITOR_DEFAULTTONEAREST);
    dpi_scale = get_dpi_scale_from_monitor(monitor);
    MONITORINFOEX monitor_info;
    monitor_info.cbSize = sizeof(MONITORINFOEX);
    GetMonitorInfo(monitor, &monitor_info);
    screen = {
        .width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left,
        .height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top,
        .dpi_scale = dpi_scale,
    };
}

bool render_target::run_loop_tasks() {
    bool ran = false;
    while (true) {
        std::unique_lock lock(loop_thread_tasks_lock);
        if (loop_thread_tasks.empty()) {
            break;
        }
        auto fn = std::move(loop_thread_tasks.front());
        loop_thread_tasks.pop();
        lock.unlock();
        ran = true;
        if (!fn) {
            continue;
        }
        try {
            fn();
        } catch (const std::exception &e) {
            std::print("Error: exception thrown in loop thread task: {}\n",
                       e.what());
        } catch (...) {
            std::print("Error: unknown exception thrown in loop thread task\n");
        }
    }
    return ran;
}

void render_target::update_hover(widget *target) {
    std::vector<widget *> chain;
    for (auto w = target; w; w = w->parent) {
        chain.push_back(w);
    }
    if (chain == hover_chain) {
        return;
    }
    for (auto &ref : hover_chain_refs) {
        if (auto w = ref.lock(); w && std::ranges::find(chain, w.get()) ==
                                          chain.end()) {
            w->handle_mouse_leave();
        }
    }
    auto previous = std::move(hover_chain);
    hover_chain = std::move(chain);
    hover_chain_refs.clear();
    for (auto w : hover_chain) {
        hover_chain_refs.push_back(w->weak_from_this());
    }
    for (auto it = hover_chain.rbegin(); it != hover_chain.rend(); ++it) {
        if (std::ranges::find(previous, *it) == previous.end()) {
            (*it)->handle_mouse_enter();
        }
    }
    request_frame();
}

void render_target::dispatch_key(key_event &e) {
    std::vector<widget *> visited;
    if (focused_widget) {
        if (auto f = focused_widget->lock()) {
            for (auto w = f.get(); w && !e.handled; w = w->parent) {
                w->handle_key(e);
                visited.push_back(w);
            }
        }
    }
    std::function<void(widget *)> broadcast = [&](widget *w) {
        for (auto list : {&w->floating, &w->children}) {
            auto snapshot = *list;
            for (auto &c : snapshot) {
                if (e.handled)
                    return;
                if (c && !c->dying_time)
                    broadcast(c.get());
            }
        }
        if (!e.handled && std::ranges::find(visited, w) == visited.end()) {
            w->handle_key(e);
        }
    };
    if (!e.handled && root) {
        broadcast(root.get());
    }
}

void render_target::dispatch_focus_change() {
    std::shared_ptr<widget> now =
        focused_widget ? focused_widget->lock() : nullptr;
    auto before = last_focused.lock();
    if (now == before) {
        return;
    }
    last_focused = now;
    if (before) {
        before->handle_focus_changed(false);
    }
    if (now) {
        now->handle_focus_changed(true);
    }
    request_frame();
}

void render_target::dispatch_input() {
    double cx = 0, cy = 0;
    glfwGetCursorPos(window, &cx, &cy);
    const float mx = static_cast<float>(cx / dpi_scale),
                my = static_cast<float>(cy / dpi_scale);
    const bool moved = mx != mouse_x || my != mouse_y;
    mouse_x = mx;
    mouse_y = my;

    std::vector<input_record> records;
    {
        std::lock_guard lock(input_lock);
        records.swap(pending_input);
    }

    update_hover(root->hit_test_tree(mx, my));
    auto chain_refs = hover_chain_refs;

    auto bubble = [&](auto &&fn) {
        for (auto &ref : chain_refs) {
            if (auto w = ref.lock()) {
                if (fn(*w))
                    return;
            }
        }
    };
    auto in_press_chain = [&](widget *w) {
        return std::ranges::any_of(press_chain, [w](auto &ref) {
            return ref.lock().get() == w;
        });
    };

    if (moved) {
        mouse_event e{.x = mx, .y = my};
        bubble([&](widget &w) {
            w.handle_mouse_move(e);
            return false;
        });
        if (mouse_down || right_mouse_down) {
            for (auto &ref : press_chain) {
                if (auto w = ref.lock(); w && !is_hovered(w.get())) {
                    w->handle_mouse_move(e);
                }
            }
        }
    }

    if (mouse_down && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) !=
                          GLFW_PRESS &&
        std::ranges::none_of(records, [](auto &r) {
            return r.type == input_record::kind::button;
        })) {
        records.push_back({.type = input_record::kind::button,
                           .code = GLFW_MOUSE_BUTTON_LEFT,
                           .action = GLFW_RELEASE});
    }

    std::u32string text;
    auto flush_text = [&] {
        if (text.empty())
            return;
        text_input_event e{.text = std::move(text)};
        text.clear();
        if (focused_widget) {
            if (auto f = focused_widget->lock()) {
                f->handle_text_input(e);
            }
        }
    };

    for (auto &r : records) {
        if (r.type == input_record::kind::character) {
            text.push_back(static_cast<char32_t>(r.code));
            continue;
        }
        flush_text();
        if (r.type == input_record::kind::button) {
            if (r.code > GLFW_MOUSE_BUTTON_MIDDLE)
                continue;
            mouse_event e{.x = mx,
                          .y = my,
                          .button = static_cast<mouse_button>(r.code)};
            if (r.action == GLFW_PRESS) {
                if (r.code == GLFW_MOUSE_BUTTON_LEFT)
                    mouse_down = true;
                if (r.code == GLFW_MOUSE_BUTTON_RIGHT)
                    right_mouse_down = true;
                if (r.code == GLFW_MOUSE_BUTTON_LEFT && focused_widget) {
                    auto f = focused_widget->lock();
                    if (!f || !is_hovered(f.get()))
                        focused_widget.reset();
                }
                press_chain = chain_refs;
                bubble([&](widget &w) {
                    w.handle_mouse_down(e);
                    return e.handled;
                });
            } else if (r.action == GLFW_RELEASE) {
                if (r.code == GLFW_MOUSE_BUTTON_LEFT)
                    mouse_down = false;
                if (r.code == GLFW_MOUSE_BUTTON_RIGHT)
                    right_mouse_down = false;
                for (auto &ref : press_chain) {
                    if (auto w = ref.lock()) {
                        w->handle_mouse_up(e);
                        if (e.handled)
                            break;
                    }
                }
                mouse_event click = e;
                click.handled = false;
                bubble([&](widget &w) {
                    if (in_press_chain(&w))
                        w.handle_click(click);
                    return click.handled;
                });
                if (!mouse_down && !right_mouse_down)
                    press_chain.clear();
            }
        } else if (r.type == input_record::kind::scroll) {
            scroll_event e{
                .x = mx, .y = my, .delta = static_cast<float>(r.value)};
            bubble([&](widget &w) {
                w.handle_scroll(e);
                return e.handled;
            });
        } else if (r.type == input_record::kind::key) {
            if (r.action == GLFW_RELEASE)
                continue;
            key_event e{.key = r.code,
                        .mods = r.mods,
                        .repeat = r.action == GLFW_REPEAT};
            dispatch_key(e);
        }
    }
    flush_text();
}

void render_target::frame() {
    frame_index++;
    auto now = clock.now();
    delta_time = std::min(
        1000 * std::chrono::duration<float>(now - last_time).count(), 50.f);
    last_time = now;
    if (screen_dirty.exchange(false)) {
        refresh_screen_info();
    }

    std::lock_guard lock(rt_lock.mutex);
    render_target::current = this;
    if (!root) {
        return;
    }
    root->owner_rt = this;
    root->parent = nullptr;

    dispatch_input();
    bool animating = false;
    bool changed = root->tick_tree(delta_time, animating);
    changed |= ime_composition_dirty.exchange(false);
    dispatch_focus_change();
    if (should_loop_stop_hide_as_close || glfwWindowShouldClose(window)) {
        return;
    }

    root->explicit_width = root->explicit_height = true;
    YGNodeStyleSetWidth(root->node, static_cast<float>(width));
    YGNodeStyleSetHeight(root->node, static_cast<float>(height));
    {
        nanovg_context vg{nvg, this};
        auto t = vg.transaction();
        vg.resetTransform();
        vg.scale(dpi_scale, dpi_scale);
        root->prepare_layout_tree(this);
        root->layout_detached(static_cast<float>(width),
                              static_cast<float>(height));
    }

    std::function<void(widget &)> settle = [&](widget &w) {
        for (auto &anim : w.anim_floats) {
            anim->update(0);
            changed |= anim->updated();
            animating |= anim->animating();
        }
        for (auto list : {&w.children, &w.floating})
            for (auto &c : *list)
                if (c)
                    settle(*c);
    };
    settle(*root);

    update_hover(root->hit_test_tree(mouse_x, mouse_y));
    dispatch_focus_change();

    const auto ms_now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count();
    const bool stale = ms_now - last_paint > 1000 &&
                       glfwGetWindowAttrib(window, GLFW_VISIBLE);
    const bool paint = changed || force_paint.exchange(false) || stale;
    if (paint) {
        int fb_width, fb_height;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        glViewport(0, 0, fb_width, fb_height);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                GL_STENCIL_BUFFER_BIT);
        nanovg_context vg{nvg, this};
        begin_font_frame(nvg);
        vg.beginFrame(fb_width, fb_height, 1);
        vg.scale(dpi_scale, dpi_scale);
        begin_acrylic_frame();
        root->render(vg);
        commit_acrylic_frame();
        vg.endFrame();
        glFlush();
        glfwSwapBuffers(window);
        last_paint = ms_now;
    } else {
        commit_acrylic_frame();
    }

    if (animating || changed) {
        if (paint && vsync)
            request_frame();
        else
            schedule_frame(8);
    }
}

void render_target::start_loop() {
    is_in_loop_thread = true;
    glfwMakeContextCurrent(window);
    last_time = clock.now();
    force_paint = true;
    screen_dirty = true;
    request_frame();
    while (!glfwWindowShouldClose(window) && !should_loop_stop_hide_as_close) {
        wait_for_frame();
        if (glfwWindowShouldClose(window) || should_loop_stop_hide_as_close) {
            break;
        }
        sync_acrylic_host();
        frame();
        if (run_loop_tasks()) {
            request_frame();
        }
    }
    if (should_loop_stop_hide_as_close) {
        should_loop_stop_hide_as_close = false;
        glfwMakeContextCurrent(window);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                GL_STENCIL_BUFFER_BIT);
        glFlush();
        glfwSwapBuffers(window);
        resize(0, 0);
        hide();
        {
            std::lock_guard lock(rt_lock.mutex);
            root->children.clear();
            root->floating.clear();
            hover_chain.clear();
            hover_chain_refs.clear();
            press_chain.clear();
            last_focused.reset();
            mouse_down = right_mouse_down = false;
        }
        {
            std::lock_guard lock(input_lock);
            pending_input.clear();
        }
    }
    glfwMakeContextCurrent(nullptr);
}
std::expected<bool, std::string> render_target::init() {
    root = std::make_shared<widget>();
    root->hit_self = false;

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

    if (decorated) {
        apply_titlebar_theme(h);
        apply_window_icon(h, GetDpiForWindow(h));
    }

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
            rt->force_paint = true;
            rt->request_frame();
        });

    glfwSetWindowFocusCallback(window, [](GLFWwindow *window, int focused) {
        auto thiz =
            static_cast<render_target *>(glfwGetWindowUserPointer(window));
        if (thiz->on_focus_changed) {
            thiz->on_focus_changed.value()(focused);
        }
        thiz->request_frame();
    });

    glfwSetWindowContentScaleCallback(
        window, [](GLFWwindow *window, float x, float y) {
            auto rt =
                static_cast<render_target *>(glfwGetWindowUserPointer(window));
            rt->dpi_scale = x;
            rt->screen_dirty = true;
            rt->force_paint = true;
            rt->request_frame();
        });

    glfwSetWindowPosCallback(window, [](GLFWwindow *window, int, int) {
        auto rt =
            static_cast<render_target *>(glfwGetWindowUserPointer(window));
        rt->screen_dirty = true;
        rt->request_frame();
    });

    glfwSetWindowRefreshCallback(window, [](GLFWwindow *window) {
        auto rt =
            static_cast<render_target *>(glfwGetWindowUserPointer(window));
        rt->force_paint = true;
        rt->request_frame();
    });

    glfwSetCursorPosCallback(window, [](GLFWwindow *window, double, double) {
        static_cast<render_target *>(glfwGetWindowUserPointer(window))
            ->request_frame();
    });

    glfwSetCursorEnterCallback(window, [](GLFWwindow *window, int) {
        static_cast<render_target *>(glfwGetWindowUserPointer(window))
            ->request_frame();
    });

    glfwSetMouseButtonCallback(
        window, [](GLFWwindow *window, int button, int action, int mods) {
            static_cast<render_target *>(glfwGetWindowUserPointer(window))
                ->push_input({.type = input_record::kind::button,
                              .code = button,
                              .action = action,
                              .mods = mods});
        });

    glfwSetScrollCallback(
        window, [](GLFWwindow *window, double xoffset, double yoffset) {
            static_cast<render_target *>(glfwGetWindowUserPointer(window))
                ->push_input({.type = input_record::kind::scroll,
                              .value = yoffset});
        });

    glfwSetKeyCallback(window, [](GLFWwindow *window, int key, int scancode,
                                  int action, int mods) {
        if (key < 0 || key > GLFW_KEY_LAST)
            return;
        static_cast<render_target *>(glfwGetWindowUserPointer(window))
            ->push_input({.type = input_record::kind::key,
                          .code = key,
                          .action = action,
                          .mods = mods});
    });

    glfwSetCharCallback(window, [](GLFWwindow *window, unsigned int codepoint) {
        auto rt =
            static_cast<render_target *>(glfwGetWindowUserPointer(window));
        if (!rt || codepoint == 0) {
            return;
        }
        rt->push_input({.type = input_record::kind::character,
                        .code = static_cast<int>(codepoint)});
    });

    refresh_screen_info();
    screen_dirty = false;

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
        release_fonts(nvg);
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
    request_frame();
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
    request_frame();
}
void render_target::post_loop_thread_task(std::function<void()> task,
                                          bool delay) {
    if (is_in_loop_thread && !delay) {
        task();
        return;
    }
    std::lock_guard lock(loop_thread_tasks_lock);
    loop_thread_tasks.push(std::move(task));
    request_frame();
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
    if (!active && !ime_caret_active) {
        return;
    }
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

    const int previous[] = {ime_caret_x,      ime_caret_y,
                            ime_caret_height, ime_document_x,
                            ime_document_y,   ime_document_width,
                            ime_document_height};
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
    const int current[] = {ime_caret_x,      ime_caret_y,
                           ime_caret_height, ime_document_x,
                           ime_document_y,   ime_document_width,
                           ime_document_height};
    if (std::ranges::equal(previous, current)) {
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
    {
        std::lock_guard lock(ime_composition_lock);
        ime_composition = {};
    }
    ime_composition_dirty = true;
    request_frame();
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
