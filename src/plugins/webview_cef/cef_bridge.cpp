// SPDX-License-Identifier: MIT
/*
 * CEF bridge
 *
 * Implements the plain-C API declared in cef_bridge.h on top of CEF's C++ API.
 *
 * This file must not include any flutter-pi header -- see cef_bridge.h for why.
 *
 * Coordinate systems
 * ------------------
 * Everything crossing this boundary (view size, pointer positions) is in
 * *logical* pixels, which is what Flutter hands us. CEF works the same way:
 * CefRenderHandler::GetViewRect is logical, and CEF multiplies it by the scale
 * factor from GetScreenInfo to get the size of the pixel buffer it passes to
 * OnPaint. So the only place physical pixels appear is on_paint().
 *
 * Copyright (c) 2026, EGT
 */

#include "cef_bridge.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_version.h"

// CEF 120 renamed the int64/uint64 typedefs to the standard int64_t/uint64_t,
// which changes the signature of OnScheduleMessagePumpWork. Rather than letting
// that surface as a confusing "marked override but does not override" error,
// fail loudly here.
//
// NOTE: CEF 126 added an `int popup_id` parameter to
// CefLifeSpanHandler::OnBeforePopup. If you bump CEF past 125, that override
// below needs the extra parameter.
#if CEF_VERSION_MAJOR < 120
    #error "The webview_cef plugin requires CEF 120 or newer."
#endif

#define LOG_CEF(...) fprintf(stderr, "[webview_cef] " __VA_ARGS__)

namespace {

// ---------------------------------------------------------------------------
// CefApp -- one per process.
// ---------------------------------------------------------------------------

class WebviewApp : public CefApp, public CefBrowserProcessHandler {
public:
    WebviewApp(std::vector<std::string> switches, wvcef_schedule_pump_cb schedule_pump, void *schedule_pump_userdata) :
        switches_(std::move(switches)),
        schedule_pump_(schedule_pump),
        schedule_pump_userdata_(schedule_pump_userdata) {}

    WebviewApp(const WebviewApp &) = delete;
    WebviewApp &operator=(const WebviewApp &) = delete;

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    void OnBeforeCommandLineProcessing(const CefString &process_type, CefRefPtr<CefCommandLine> command_line) override {
        // An empty process type means the browser process. Switches appended
        // here are inherited by the subprocesses, so only do it once.
        if (!process_type.empty()) {
            return;
        }

        for (const std::string &sw : switches_) {
            const size_t eq = sw.find('=');
            if (eq == std::string::npos) {
                command_line->AppendSwitch(sw);
            } else {
                command_line->AppendSwitchWithValue(sw.substr(0, eq), sw.substr(eq + 1));
            }
        }
    }

    // MAY BE CALLED FROM ANY THREAD.
    void OnScheduleMessagePumpWork(int64_t delay_ms) override {
        if (schedule_pump_ != nullptr) {
            schedule_pump_(schedule_pump_userdata_, delay_ms);
        }
    }

private:
    std::vector<std::string> switches_;
    wvcef_schedule_pump_cb schedule_pump_;
    void *schedule_pump_userdata_;

    IMPLEMENT_REFCOUNTING(WebviewApp);
};

// ---------------------------------------------------------------------------
// CefClient -- one per webview.
// ---------------------------------------------------------------------------

typedef void (*gone_cb_t)(void *userdata);

class WebviewClient : public CefClient,
                      public CefLifeSpanHandler,
                      public CefRenderHandler,
                      public CefLoadHandler,
                      public CefDisplayHandler {
public:
    WebviewClient(int logical_width, int logical_height, double scale, const struct wvcef_host_callbacks *callbacks, void *userdata) :
        logical_width_(logical_width),
        logical_height_(logical_height),
        scale_(scale),
        callbacks_(*callbacks),
        userdata_(userdata) {}

    WebviewClient(const WebviewClient &) = delete;
    WebviewClient &operator=(const WebviewClient &) = delete;

    // -- CefClient ----------------------------------------------------------
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

    // -- CefLifeSpanHandler -------------------------------------------------
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override { browser_ = browser; }

    bool DoClose(CefRefPtr<CefBrowser> browser) override {
        (void) browser;
        // Let the close proceed; OnBeforeClose follows.
        return false;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        (void) browser;
        browser_ = nullptr;

        // This is the last callback for this browser, and the host frees the
        // object `userdata_` points at while handling it. Drop both before
        // calling out, so a stray later callback can't use a dangling pointer.
        const struct wvcef_host_callbacks callbacks = callbacks_;
        void *const userdata = userdata_;

        memset(&callbacks_, 0, sizeof callbacks_);
        userdata_ = nullptr;

        if (callbacks.on_closed != nullptr) {
            callbacks.on_closed(userdata);
        }

        // Hand the C handle to the bridge for disposal. It only queues it --
        // releasing the last reference to this client from inside one of its own
        // methods would destroy `this` while we're still running.
        if (on_gone_ != nullptr) {
            gone_cb_t cb = on_gone_;
            void *ud = on_gone_userdata_;
            on_gone_ = nullptr;
            cb(ud);
        }
    }

    /// A kiosk has nowhere to put a second window, so load target=_blank and
    /// window.open() URLs into this view instead of spawning a popup browser
    /// that nothing would ever render.
    bool OnBeforePopup(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        const CefString &target_url,
        const CefString &target_frame_name,
        CefLifeSpanHandler::WindowOpenDisposition target_disposition,
        bool user_gesture,
        const CefPopupFeatures &popup_features,
        CefWindowInfo &window_info,
        CefRefPtr<CefClient> &client,
        CefBrowserSettings &settings,
        CefRefPtr<CefDictionaryValue> &extra_info,
        bool *no_javascript_access
    ) override {
        (void) frame;
        (void) target_frame_name;
        (void) target_disposition;
        (void) user_gesture;
        (void) popup_features;
        (void) window_info;
        (void) client;
        (void) settings;
        (void) extra_info;
        (void) no_javascript_access;

        if (browser != nullptr && !target_url.empty()) {
            browser->GetMainFrame()->LoadURL(target_url);
        }

        // Cancel the popup.
        return true;
    }

    // -- CefRenderHandler ---------------------------------------------------
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect) override {
        (void) browser;
        rect.x = 0;
        rect.y = 0;
        rect.width = logical_width_;
        rect.height = logical_height_;
    }

    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo &screen_info) override {
        (void) browser;

        screen_info.device_scale_factor = static_cast<float>(scale_);
        screen_info.depth = 32;
        screen_info.depth_per_component = 8;
        screen_info.is_monochrome = 0;
        screen_info.rect = CefRect(0, 0, logical_width_, logical_height_);
        screen_info.available_rect = screen_info.rect;
        return true;
    }

    bool GetScreenPoint(CefRefPtr<CefBrowser> browser, int view_x, int view_y, int &screen_x, int &screen_y) override {
        (void) browser;
        screen_x = view_x;
        screen_y = view_y;
        return true;
    }

    void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override {
        popup_visible_ = show;

        if (!show) {
            popup_rect_ = CefRect();
            popup_buffer_.clear();
            view_buffer_.clear();
        }

        // Repaint so the popup appears / disappears right away. This also gives
        // us the full view frame we need to composite the popup onto.
        if (browser != nullptr) {
            browser->GetHost()->Invalidate(PET_VIEW);
        }
    }

    void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect &rect) override {
        (void) browser;
        popup_rect_ = rect;
    }

    void OnPaint(
        CefRefPtr<CefBrowser> browser,
        PaintElementType type,
        const RectList &dirty_rects,
        const void *buffer,
        int width,
        int height
    ) override {
        (void) browser;
        // Dirty rects are ignored: GLES2 has no GL_UNPACK_ROW_LENGTH, so a
        // partial upload would need a per-row loop that costs more than it saves.
        (void) dirty_rects;

        if (callbacks_.on_paint == nullptr || buffer == nullptr || width <= 0 || height <= 0) {
            return;
        }

        const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
        const size_t n_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;

        if (type == PET_POPUP) {
            popup_width_ = width;
            popup_height_ = height;
            popup_buffer_.assign(bytes, bytes + n_bytes);

            if (!view_buffer_.empty()) {
                composite_and_emit();
            }
            return;
        }

        if (!popup_visible_) {
            // Fast path: hand CEF's buffer straight to the host, no copy.
            callbacks_.on_paint(userdata_, buffer, width, height);
            return;
        }

        view_width_ = width;
        view_height_ = height;
        view_buffer_.assign(bytes, bytes + n_bytes);
        composite_and_emit();
    }

    bool OnCursorChange(
        CefRefPtr<CefBrowser> browser,
        CefCursorHandle cursor,
        cef_cursor_type_t type,
        const CefCursorInfo &custom_cursor_info
    ) override {
        (void) browser;
        (void) cursor;
        (void) type;
        (void) custom_cursor_info;
        // No window system to set a cursor on. Swallow it.
        return true;
    }

    // -- CefLoadHandler -----------------------------------------------------
    void OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool is_loading, bool can_go_back, bool can_go_forward) override {
        (void) browser;
        if (callbacks_.on_loading_state_changed != nullptr) {
            callbacks_.on_loading_state_changed(userdata_, is_loading, can_go_back, can_go_forward);
        }
    }

    void OnLoadError(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        ErrorCode error_code,
        const CefString &error_text,
        const CefString &failed_url
    ) override {
        (void) browser;

        // Sub-frame errors are noise for the app; only report the main frame.
        if (frame != nullptr && !frame->IsMain()) {
            return;
        }

        if (callbacks_.on_load_error != nullptr) {
            const std::string text = error_text.ToString();
            const std::string url = failed_url.ToString();
            callbacks_.on_load_error(userdata_, static_cast<int>(error_code), text.c_str(), url.c_str());
        }
    }

    // -- CefDisplayHandler --------------------------------------------------
    void OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString &url) override {
        (void) browser;

        if (frame != nullptr && !frame->IsMain()) {
            return;
        }

        if (callbacks_.on_url_changed != nullptr) {
            const std::string str = url.ToString();
            callbacks_.on_url_changed(userdata_, str.c_str());
        }
    }

    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString &title) override {
        (void) browser;
        if (callbacks_.on_title_changed != nullptr) {
            const std::string str = title.ToString();
            callbacks_.on_title_changed(userdata_, str.c_str());
        }
    }

    // -- Used by the C API --------------------------------------------------
    CefRefPtr<CefBrowser> browser() const { return browser_; }

    void set_on_gone(gone_cb_t cb, void *userdata) {
        on_gone_ = cb;
        on_gone_userdata_ = userdata;
    }

    void set_size(int logical_width, int logical_height, double scale) {
        logical_width_ = logical_width;
        logical_height_ = logical_height;
        scale_ = scale;
    }

private:
    /// Draws popup_buffer_ over a copy of view_buffer_ and emits the result.
    void composite_and_emit() {
        if (view_buffer_.empty() || view_width_ <= 0 || view_height_ <= 0) {
            return;
        }

        composite_buffer_ = view_buffer_;

        if (!popup_buffer_.empty() && popup_width_ > 0 && popup_height_ > 0) {
            // popup_rect_ is logical, the buffers are physical pixels.
            const int off_x = static_cast<int>(popup_rect_.x * scale_);
            const int off_y = static_cast<int>(popup_rect_.y * scale_);

            for (int row = 0; row < popup_height_; row++) {
                const int dst_y = off_y + row;
                if (dst_y < 0 || dst_y >= view_height_) {
                    continue;
                }

                int dst_x = off_x;
                int src_x = 0;
                int n_px = popup_width_;

                if (dst_x < 0) {
                    src_x = -dst_x;
                    n_px -= src_x;
                    dst_x = 0;
                }
                if (dst_x + n_px > view_width_) {
                    n_px = view_width_ - dst_x;
                }
                if (n_px <= 0) {
                    continue;
                }

                memcpy(
                    composite_buffer_.data() + (static_cast<size_t>(dst_y) * view_width_ + dst_x) * 4,
                    popup_buffer_.data() + (static_cast<size_t>(row) * popup_width_ + src_x) * 4,
                    static_cast<size_t>(n_px) * 4
                );
            }
        }

        callbacks_.on_paint(userdata_, composite_buffer_.data(), view_width_, view_height_);
    }

    CefRefPtr<CefBrowser> browser_;

    int logical_width_;
    int logical_height_;
    double scale_;

    struct wvcef_host_callbacks callbacks_;
    void *userdata_;

    gone_cb_t on_gone_ = nullptr;
    void *on_gone_userdata_ = nullptr;

    bool popup_visible_ = false;
    CefRect popup_rect_;
    std::vector<uint8_t> popup_buffer_;
    int popup_width_ = 0;
    int popup_height_ = 0;

    std::vector<uint8_t> view_buffer_;
    std::vector<uint8_t> composite_buffer_;
    int view_width_ = 0;
    int view_height_ = 0;

    IMPLEMENT_REFCOUNTING(WebviewClient);
};

bool g_initialized = false;
int g_n_live_browsers = 0;
CefRefPtr<WebviewApp> g_app;

cef_log_severity_t to_log_severity(int severity) {
    switch (severity) {
        case 1: return LOGSEVERITY_VERBOSE;
        case 2: return LOGSEVERITY_INFO;
        case 3: return LOGSEVERITY_WARNING;
        case 4: return LOGSEVERITY_ERROR;
        case 5: return LOGSEVERITY_FATAL;
        case 99: return LOGSEVERITY_DISABLE;
        default: return LOGSEVERITY_DEFAULT;
    }
}

cef_mouse_button_type_t to_cef_button(enum wvcef_pointer_button button) {
    switch (button) {
        case WVCEF_BUTTON_MIDDLE: return MBT_MIDDLE;
        case WVCEF_BUTTON_RIGHT: return MBT_RIGHT;
        case WVCEF_BUTTON_LEFT:
        default: return MBT_LEFT;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------

/// Handle handed out to the C side. Owned by the bridge, freed once the
/// underlying browser has really gone away (after on_closed).
struct wvcef_browser {
    CefRefPtr<WebviewClient> client;
    bool close_requested;
};

namespace {

/// Handles whose browser has closed, waiting to be freed from a stack that isn't
/// inside one of their own CEF callbacks.
std::vector<struct wvcef_browser *> g_zombies;

void on_client_gone(void *userdata) {
    struct wvcef_browser *handle = static_cast<struct wvcef_browser *>(userdata);

    if (g_n_live_browsers > 0) {
        g_n_live_browsers--;
    }

    if (handle != nullptr) {
        g_zombies.push_back(handle);
    }
}

/// Releases closed browsers. Only safe to call from outside any CEF callback,
/// because dropping the handle's CefRefPtr may destroy the client object.
void reap_zombies() {
    if (g_zombies.empty()) {
        return;
    }

    std::vector<struct wvcef_browser *> zombies;
    zombies.swap(g_zombies);

    for (struct wvcef_browser *handle : zombies) {
        handle->client = nullptr;
        delete handle;
    }
}

CefRefPtr<CefBrowser> browser_of(struct wvcef_browser *browser) {
    if (browser == nullptr || browser->client == nullptr) {
        return nullptr;
    }
    return browser->client->browser();
}

}  // namespace

int wvcef_execute_subprocess(int argc, char **argv) {
    CefMainArgs main_args(argc, argv);
    return CefExecuteProcess(main_args, nullptr, nullptr);
}

int wvcef_initialize(const struct wvcef_init_options *options) {
    if (options == nullptr) {
        return 22 /* EINVAL */;
    }

    if (g_initialized) {
        return 0;
    }

    if (options->subprocess_path == nullptr || options->resources_dir == nullptr || options->locales_dir == nullptr) {
        LOG_CEF("wvcef_initialize: subprocess_path, resources_dir and locales_dir are required.\n");
        return 22 /* EINVAL */;
    }

    std::vector<std::string> switches;
    for (size_t i = 0; i < options->n_switches; i++) {
        if (options->switches[i] != nullptr && options->switches[i][0] != '\0') {
            switches.emplace_back(options->switches[i]);
        }
    }

    g_app = new WebviewApp(std::move(switches), options->schedule_pump, options->schedule_pump_userdata);

    // Don't hand flutter-pi's own argv to Chromium -- it would try to interpret
    // "--release" and the asset bundle path as Chromium switches. Everything we
    // want on the command line is appended in OnBeforeCommandLineProcessing.
    char argv0[] = "flutter-pi";
    char *fake_argv[] = { argv0, nullptr };
    CefMainArgs main_args(1, fake_argv);

    CefSettings settings;
    settings.no_sandbox = 1;
    settings.windowless_rendering_enabled = 1;
    settings.external_message_pump = 1;
    settings.multi_threaded_message_loop = 0;
    settings.command_line_args_disabled = 0;
    settings.log_severity = to_log_severity(options->log_severity);

    CefString(&settings.browser_subprocess_path).FromString(options->subprocess_path);
    CefString(&settings.resources_dir_path).FromString(options->resources_dir);
    CefString(&settings.locales_dir_path).FromString(options->locales_dir);

    if (options->cache_path != nullptr && options->cache_path[0] != '\0') {
        CefString(&settings.cache_path).FromString(options->cache_path);
        // CEF >= 120 wants root_cache_path set, with cache_path equal to it or
        // below it.
        CefString(&settings.root_cache_path).FromString(options->cache_path);
    }
    if (options->user_agent != nullptr && options->user_agent[0] != '\0') {
        CefString(&settings.user_agent).FromString(options->user_agent);
    }
    if (options->log_file != nullptr && options->log_file[0] != '\0') {
        CefString(&settings.log_file).FromString(options->log_file);
    }

    if (!CefInitialize(main_args, settings, g_app, nullptr)) {
        LOG_CEF("CefInitialize failed.\n");
        g_app = nullptr;
        return 5 /* EIO */;
    }

    g_initialized = true;
    // CEF_VERSION is the only version macro that's spelled the same across
    // releases; it reads like "120.2.7+gd3b6c37+chromium-120.0.6099.234".
    LOG_CEF("CEF %s initialized.\n", CEF_VERSION);
    return 0;
}

bool wvcef_is_initialized(void) {
    return g_initialized;
}

void wvcef_do_message_loop_work(void) {
    if (!g_initialized) {
        return;
    }

    reap_zombies();
    CefDoMessageLoopWork();
}

int wvcef_n_live_browsers(void) {
    return g_n_live_browsers;
}

void wvcef_shutdown(void) {
    if (!g_initialized) {
        return;
    }

    reap_zombies();

    g_initialized = false;
    CefShutdown();
    g_app = nullptr;
}

struct wvcef_browser *wvcef_browser_create(
    const char *url,
    int width,
    int height,
    double device_pixel_ratio,
    int frame_rate,
    const struct wvcef_host_callbacks *callbacks,
    void *userdata
) {
    if (!g_initialized || callbacks == nullptr) {
        return nullptr;
    }

    if (width <= 0) {
        width = 1;
    }
    if (height <= 0) {
        height = 1;
    }
    if (device_pixel_ratio <= 0.0) {
        device_pixel_ratio = 1.0;
    }
    if (frame_rate < 1) {
        frame_rate = 30;
    } else if (frame_rate > 60) {
        frame_rate = 60;
    }

    struct wvcef_browser *handle = new struct wvcef_browser();
    handle->close_requested = false;
    handle->client = new WebviewClient(width, height, device_pixel_ratio, callbacks, userdata);
    handle->client->set_on_gone(on_client_gone, handle);

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = frame_rate;
    browser_settings.background_color = CefColorSetARGB(255, 255, 255, 255);

    CefRefPtr<CefBrowser> browser = CefBrowserHost::CreateBrowserSync(
        window_info,
        handle->client,
        CefString(url != nullptr && url[0] != '\0' ? url : "about:blank"),
        browser_settings,
        nullptr,
        nullptr
    );
    if (browser == nullptr) {
        LOG_CEF("CefBrowserHost::CreateBrowserSync failed.\n");
        handle->client->set_on_gone(nullptr, nullptr);
        handle->client = nullptr;
        delete handle;
        return nullptr;
    }

    g_n_live_browsers++;
    return handle;
}

void wvcef_browser_close(struct wvcef_browser *browser) {
    if (browser == nullptr || browser->client == nullptr || browser->close_requested) {
        return;
    }

    browser->close_requested = true;

    CefRefPtr<CefBrowser> b = browser->client->browser();
    if (b != nullptr) {
        b->GetHost()->CloseBrowser(true);
    }
}

void wvcef_browser_load_url(struct wvcef_browser *browser, const char *url) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr || url == nullptr) {
        return;
    }
    b->GetMainFrame()->LoadURL(CefString(url));
}

void wvcef_browser_reload(struct wvcef_browser *browser, bool ignore_cache) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr) {
        return;
    }
    if (ignore_cache) {
        b->ReloadIgnoreCache();
    } else {
        b->Reload();
    }
}

void wvcef_browser_stop_load(struct wvcef_browser *browser) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b != nullptr) {
        b->StopLoad();
    }
}

void wvcef_browser_go_back(struct wvcef_browser *browser) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b != nullptr && b->CanGoBack()) {
        b->GoBack();
    }
}

void wvcef_browser_go_forward(struct wvcef_browser *browser) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b != nullptr && b->CanGoForward()) {
        b->GoForward();
    }
}

void wvcef_browser_execute_javascript(struct wvcef_browser *browser, const char *code) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr || code == nullptr) {
        return;
    }
    CefRefPtr<CefFrame> frame = b->GetMainFrame();
    frame->ExecuteJavaScript(CefString(code), frame->GetURL(), 0);
}

void wvcef_browser_resize(struct wvcef_browser *browser, int width, int height, double device_pixel_ratio) {
    if (browser == nullptr || browser->client == nullptr) {
        return;
    }

    browser->client->set_size(width > 0 ? width : 1, height > 0 ? height : 1, device_pixel_ratio > 0.0 ? device_pixel_ratio : 1.0);

    CefRefPtr<CefBrowser> b = browser->client->browser();
    if (b != nullptr) {
        b->GetHost()->NotifyScreenInfoChanged();
        b->GetHost()->WasResized();
    }
}

void wvcef_browser_set_focus(struct wvcef_browser *browser, bool focused) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b != nullptr) {
        b->GetHost()->SetFocus(focused);
    }
}

void wvcef_browser_invalidate(struct wvcef_browser *browser) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b != nullptr) {
        b->GetHost()->Invalidate(PET_VIEW);
    }
}

void wvcef_browser_send_mouse_move(struct wvcef_browser *browser, int x, int y, bool mouse_leave, uint32_t modifiers) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr) {
        return;
    }

    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;

    b->GetHost()->SendMouseMoveEvent(event, mouse_leave);
}

void wvcef_browser_send_mouse_button(
    struct wvcef_browser *browser,
    int x,
    int y,
    enum wvcef_pointer_button button,
    bool is_up,
    int click_count,
    uint32_t modifiers
) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr) {
        return;
    }

    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;

    b->GetHost()->SendMouseClickEvent(event, to_cef_button(button), is_up, click_count > 0 ? click_count : 1);
}

void wvcef_browser_send_mouse_wheel(struct wvcef_browser *browser, int x, int y, int delta_x, int delta_y, uint32_t modifiers) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr) {
        return;
    }

    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;

    b->GetHost()->SendMouseWheelEvent(event, delta_x, delta_y);
}

void wvcef_browser_send_touch(struct wvcef_browser *browser, int pointer_id, int x, int y, enum wvcef_touch_phase phase, uint32_t modifiers) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr) {
        return;
    }

    CefTouchEvent event;
    event.id = pointer_id;
    event.x = static_cast<float>(x);
    event.y = static_cast<float>(y);
    event.radius_x = 0;
    event.radius_y = 0;
    event.rotation_angle = 0;
    event.pressure = 1.0f;
    event.modifiers = modifiers;
    event.pointer_type = CEF_POINTER_TYPE_TOUCH;

    switch (phase) {
        case WVCEF_TOUCH_PRESSED: event.type = CEF_TET_PRESSED; break;
        case WVCEF_TOUCH_MOVED: event.type = CEF_TET_MOVED; break;
        case WVCEF_TOUCH_CANCELLED: event.type = CEF_TET_CANCELLED; break;
        case WVCEF_TOUCH_RELEASED:
        default: event.type = CEF_TET_RELEASED; break;
    }

    b->GetHost()->SendTouchEvent(event);
}

void wvcef_browser_send_key(
    struct wvcef_browser *browser,
    enum wvcef_key_type type,
    int windows_key_code,
    int native_key_code,
    uint32_t character,
    uint32_t modifiers
) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr) {
        return;
    }

    CefKeyEvent event;
    switch (type) {
        case WVCEF_KEY_RAWDOWN: event.type = KEYEVENT_RAWKEYDOWN; break;
        case WVCEF_KEY_DOWN: event.type = KEYEVENT_KEYDOWN; break;
        case WVCEF_KEY_UP: event.type = KEYEVENT_KEYUP; break;
        case WVCEF_KEY_CHAR:
        default: event.type = KEYEVENT_CHAR; break;
    }

    event.modifiers = modifiers;
    event.windows_key_code = windows_key_code;
    event.native_key_code = native_key_code;
    event.is_system_key = 0;
    event.character = static_cast<char16_t>(character);
    event.unmodified_character = static_cast<char16_t>(character);
    event.focus_on_editable_field = 0;

    b->GetHost()->SendKeyEvent(event);
}
