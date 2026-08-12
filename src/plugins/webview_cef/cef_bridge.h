// SPDX-License-Identifier: MIT
/*
 * CEF bridge
 *
 * A plain-C ABI around the (C++) Chromium Embedded Framework API.
 *
 * The rest of flutter-pi is C, and flutter-pi's headers can't be included from
 * C++ (platformchannel.h & friends use compound literals, which don't exist in
 * C++). So all CEF-facing code lives in cef_bridge.cpp behind this header, and
 * the plugin (plugin.c) only ever sees plain C types.
 *
 * Threading contract
 * ------------------
 * CEF is initialized with an *external message pump*, which means the CEF
 * "UI thread" is whichever thread called @ref wvcef_initialize -- for us that's
 * flutter-pi's platform thread. As a consequence:
 *
 *   - every wvcef_* function except @ref wvcef_schedule_pump_cb must be called
 *     on the platform thread, and
 *   - every callback in @ref wvcef_host_callbacks is invoked on the platform
 *     thread, so implementations may touch flutter-pi state directly.
 *
 * The single exception is the pump-scheduling callback, which CEF may invoke
 * from any thread.
 *
 * Copyright (c) 2026, EGT
 */

#ifndef _FLUTTERPI_SRC_PLUGINS_WEBVIEW_CEF_CEF_BRIDGE_H
#define _FLUTTERPI_SRC_PLUGINS_WEBVIEW_CEF_CEF_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wvcef_browser;

enum wvcef_pointer_button {
    WVCEF_BUTTON_LEFT = 0,
    WVCEF_BUTTON_MIDDLE = 1,
    WVCEF_BUTTON_RIGHT = 2,
};

enum wvcef_touch_phase {
    WVCEF_TOUCH_RELEASED = 0,
    WVCEF_TOUCH_PRESSED = 1,
    WVCEF_TOUCH_MOVED = 2,
    WVCEF_TOUCH_CANCELLED = 3,
};

enum wvcef_key_type {
    WVCEF_KEY_RAWDOWN = 0,
    WVCEF_KEY_DOWN = 1,
    WVCEF_KEY_UP = 2,
    WVCEF_KEY_CHAR = 3,
};

/**
 * @brief Callbacks a webview instance delivers to the host (plugin.c).
 *
 * All of these are invoked on the platform thread. Strings are only valid for
 * the duration of the call.
 */
struct wvcef_host_callbacks {
    /**
     * @brief A new frame was rendered.
     *
     * @param buffer BGRA8888 (byte order B,G,R,A), @ref width * @ref height * 4 bytes,
     *               tightly packed. Only valid during the call.
     * @param width,height Size of the buffer in *physical* pixels.
     */
    void (*on_paint)(void *userdata, const void *buffer, int width, int height);

    void (*on_loading_state_changed)(void *userdata, bool is_loading, bool can_go_back, bool can_go_forward);
    void (*on_url_changed)(void *userdata, const char *url);
    void (*on_title_changed)(void *userdata, const char *title);
    void (*on_load_error)(void *userdata, int error_code, const char *error_text, const char *failed_url);

    /// Called once the underlying browser is gone. After this, the
    /// struct wvcef_browser handle must not be used anymore.
    void (*on_closed)(void *userdata);
};

/**
 * @brief Called by CEF when @ref wvcef_do_message_loop_work should be called
 * again, in @ref delay_ms milliseconds.
 *
 * MAY BE CALLED FROM ANY THREAD.
 */
typedef void (*wvcef_schedule_pump_cb)(void *userdata, int64_t delay_ms);

struct wvcef_init_options {
    /// Absolute path of the CEF subprocess helper executable
    /// (flutter-pi-cef-helper). Required.
    const char *subprocess_path;

    /// Directory containing icudtl.dat, *.pak and the *.bin snapshot files.
    /// Required.
    const char *resources_dir;

    /// Directory containing the *.pak locale files. Usually
    /// <resources_dir>/locales. Required.
    const char *locales_dir;

    /// Where Chromium may persist its cache. NULL/empty for a fully in-memory
    /// ("incognito") profile.
    const char *cache_path;

    /// Overrides the User-Agent header. NULL to keep Chromium's default.
    const char *user_agent;

    /// Chromium log file, NULL for stderr.
    const char *log_file;

    /// 0 = default (warning), 1 = verbose, 2 = info, 3 = warning, 4 = error,
    /// 5 = fatal, 99 = disable.
    int log_severity;

    /// Extra command line switches, e.g. "disable-gpu" or "ozone-platform=headless".
    /// Written without the leading "--". A "key=value" entry becomes a switch
    /// with a value, a bare "key" becomes a boolean switch.
    const char *const *switches;
    size_t n_switches;

    wvcef_schedule_pump_cb schedule_pump;
    void *schedule_pump_userdata;
};

/**
 * @brief Entry point for the CEF helper (subprocess) executable.
 *
 * Returns the exit code the helper process should exit with, or -1 if this
 * process is not a CEF subprocess.
 */
int wvcef_execute_subprocess(int argc, char **argv);

/**
 * @brief Initialize CEF. Must be called exactly once, on the platform thread.
 *
 * @returns 0 on success, an errno-style code otherwise.
 */
int wvcef_initialize(const struct wvcef_init_options *options);

/// True if @ref wvcef_initialize succeeded and @ref wvcef_shutdown wasn't called yet.
bool wvcef_is_initialized(void);

/// Let CEF do some work. Call from the platform thread, whenever the
/// @ref wvcef_schedule_pump_cb deadline expires.
void wvcef_do_message_loop_work(void);

/// Number of browsers that were created and haven't fully closed yet.
int wvcef_n_live_browsers(void);

/// Tears CEF down. All browsers must have been closed before.
void wvcef_shutdown(void);

/**
 * @brief Create an off-screen browser.
 *
 * @param width,height Size in *logical* pixels (what Flutter calls logical pixels
 *                     and CSS calls px). The buffer delivered to on_paint is
 *                     width*device_pixel_ratio by height*device_pixel_ratio.
 * @param device_pixel_ratio Scale factor reported to the page (window.devicePixelRatio).
 * @param frame_rate Maximum frames per second CEF will render, 1..60.
 * @returns The browser, or NULL on failure.
 */
struct wvcef_browser *wvcef_browser_create(
    const char *url,
    int width,
    int height,
    double device_pixel_ratio,
    int frame_rate,
    const struct wvcef_host_callbacks *callbacks,
    void *userdata
);

/// Asks the browser to close. @ref wvcef_host_callbacks::on_closed is called
/// once it's really gone; only then is the handle released.
void wvcef_browser_close(struct wvcef_browser *browser);

void wvcef_browser_load_url(struct wvcef_browser *browser, const char *url);
void wvcef_browser_reload(struct wvcef_browser *browser, bool ignore_cache);
void wvcef_browser_stop_load(struct wvcef_browser *browser);
void wvcef_browser_go_back(struct wvcef_browser *browser);
void wvcef_browser_go_forward(struct wvcef_browser *browser);
void wvcef_browser_execute_javascript(struct wvcef_browser *browser, const char *code);

/// Resize the off-screen surface. Sizes are in logical pixels.
void wvcef_browser_resize(struct wvcef_browser *browser, int width, int height, double device_pixel_ratio);

void wvcef_browser_set_focus(struct wvcef_browser *browser, bool focused);

/// Ask CEF to re-send the whole frame via on_paint.
void wvcef_browser_invalidate(struct wvcef_browser *browser);

/// Coordinates are in logical pixels, relative to the webview's top left corner.
void wvcef_browser_send_mouse_move(struct wvcef_browser *browser, int x, int y, bool mouse_leave, uint32_t modifiers);
void wvcef_browser_send_mouse_button(
    struct wvcef_browser *browser,
    int x,
    int y,
    enum wvcef_pointer_button button,
    bool is_up,
    int click_count,
    uint32_t modifiers
);
void wvcef_browser_send_mouse_wheel(struct wvcef_browser *browser, int x, int y, int delta_x, int delta_y, uint32_t modifiers);
void wvcef_browser_send_touch(struct wvcef_browser *browser, int pointer_id, int x, int y, enum wvcef_touch_phase phase, uint32_t modifiers);
void wvcef_browser_send_key(
    struct wvcef_browser *browser,
    enum wvcef_key_type type,
    int windows_key_code,
    int native_key_code,
    uint32_t character,
    uint32_t modifiers
);

#ifdef __cplusplus
}
#endif

#endif  // _FLUTTERPI_SRC_PLUGINS_WEBVIEW_CEF_CEF_BRIDGE_H
