#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <print>
#include <queue>
#include <string>
#include <type_traits>
#include <utility>

#include "GLFW/glfw3.h"
#include "nanovg.h"

#include "breeze_ui/acrylic_host.h"
#include "breeze_ui/widget.h"

namespace ui {

struct ime_composition_state {
    std::u32string text;
    int cursor = 0;
    bool active = false;
};

struct render_target;

struct tree_lock {
    render_target *owner = nullptr;
    std::recursive_mutex mutex;
    void lock() { mutex.lock(); }
    bool try_lock() { return mutex.try_lock(); }
    void unlock();
};

struct input_record {
    enum class kind { key, character, button, scroll } type;
    int code = 0;
    int action = 0;
    int mods = 0;
    double value = 0;
};

struct render_target {
    std::shared_ptr<widget> root;
    GLFWwindow *window = nullptr;
    static thread_local render_target *current;
    // float: darkness of the acrylic effect, 0~1
    std::optional<float> acrylic = {};
    bool extend = false;
    bool transparent = false;
    bool no_activate = false;
    bool capture_all_input = false;
    bool decorated = true;
    bool topmost = false;
    bool resizable = false;
    bool vsync = true;
    std::string title = "Window";
    NVGcontext *nvg = nullptr;
    int width = 1280;
    int height = 720;
    static std::atomic_int view_cnt;
    int view_id = view_cnt++;
    float dpi_scale = 1;
    screen_info screen{};
    ime_composition_state ime_composition;
    std::mutex ime_composition_lock{};
    std::atomic_bool ime_composition_dirty = false;
    std::expected<bool, std::string> init();

    float mouse_x = -1, mouse_y = -1;
    float delta_time = 0;
    std::uint64_t frame_index = 0;
    int idle_poll_ms = 0;
    bool key_down(int key) const;
    bool is_hovered(const widget *w) const;
    widget *hovered_widget() const;
    void request_frame();
    void schedule_frame(float delay_ms);
    void refresh_screen_info();
    void begin_acrylic_frame();
    void register_acrylic_region(acrylic_region region);
    void commit_acrylic_frame();
    void sync_acrylic_host();

    std::optional<std::weak_ptr<widget>> focused_widget = {};

    static std::queue<std::function<void()>> main_thread_tasks;
    static std::mutex main_thread_tasks_mutex;
    static void post_main_thread_task(std::function<void()> task);

    static std::expected<bool, std::string> init_global();
    void start_loop();
    void resize(int width, int height);
    void set_position(int x, int y);
    void reset_view();
    void close();
    void hide();
    void show();
    void focus();
    void hide_as_close();
    void set_ime_caret_rect(float x, float y, float height, bool active,
                            float document_x = 0, float document_y = 0,
                            float document_width = 0,
                            float document_height = 0);
    void clear_ime_composition();
    void *hwnd() const;
    bool should_loop_stop_hide_as_close = false;
    std::optional<std::function<void(bool)>> on_focus_changed;
    std::chrono::steady_clock clock{};
    tree_lock rt_lock{this};
    std::mutex loop_thread_tasks_lock{};
    std::queue<std::function<void()>> loop_thread_tasks{};
    void post_loop_thread_task(std::function<void()> task, bool delay = false);
    template <typename T>
    T inline post_loop_thread_task(std::function<T()> task) {
        std::promise<T> p;
        post_loop_thread_task([&]() { p.set_value(task()); });
        return p.get_future().get();
    }
    decltype(clock.now()) last_time = clock.now();
    bool mouse_down = false, right_mouse_down = false;
    bool ime_caret_active = false;
    int ime_caret_x = 0;
    int ime_caret_y = 0;
    int ime_caret_height = 0;
    int ime_document_x = 0;
    int ime_document_y = 0;
    int ime_document_width = 0;
    int ime_document_height = 0;
    void *original_wndproc = nullptr;
    void *parent = nullptr;
    std::unique_ptr<acrylic_host> acrylic_host_window = nullptr;
    std::vector<acrylic_region> acrylic_regions = {};

    void push_input(input_record record);

    render_target() = default;
    ~render_target();
    render_target operator=(const render_target &) = delete;
    render_target(const render_target &) = delete;

  private:
    std::mutex frame_mutex;
    std::condition_variable frame_cv;
    bool frame_requested = true;
    std::optional<std::chrono::steady_clock::time_point> scheduled_frame;
    std::atomic_bool screen_dirty = true;
    std::atomic_bool force_paint = true;
    int64_t last_paint = 0;
    std::mutex input_lock;
    std::vector<input_record> pending_input;
    std::vector<widget *> hover_chain;
    std::vector<std::weak_ptr<widget>> hover_chain_refs;
    std::vector<std::weak_ptr<widget>> press_chain;
    std::weak_ptr<widget> last_focused;

    void wait_for_frame();
    bool run_loop_tasks();
    void frame();
    void dispatch_input();
    void update_hover(widget *target);
    void dispatch_key(key_event &e);
    void dispatch_focus_change();
};
} // namespace ui
