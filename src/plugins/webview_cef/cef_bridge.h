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
 * Coordinate systems
 * ------------------
 * Everything crossing this boundary -- view size, pointer positions -- is in
 * *logical* pixels, which is what flutter hands us. CEF works the same way:
 * CefRenderHandler::GetViewRect is logical, and CEF multiplies it by the scale
 * factor from GetScreenInfo to get the size of the pixel buffer it passes to
 * OnPaint. So physical pixels only ever appear in on_paint().
 *
 * Copyright (c) 2026, Bojidar Tonchev <bojidar.tonchev@gmail.com>
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

/**
 * @brief Callbacks a webview instance delivers to the host (plugin.c).
 *
 * All of these are invoked on the platform thread. Strings are only valid for
 * the duration of the call. Every one of them may be NULL.
 */
struct wvcef_host_callbacks {
    /**
     * @brief A new frame was rendered.
     *
     * @param buffer BGRA8888 (byte order B,G,R,A), @ref width * @ref height * 4
     *               bytes, tightly packed. Only valid during the call.
     * @param width,height Size of the buffer in *physical* pixels.
     */
    void (*on_paint)(void *userdata, const void *buffer, int width, int height);

    void (*on_load_start)(void *userdata, const char *url);
    void (*on_load_end)(void *userdata, const char *url, int http_status_code);
    void (*on_load_error)(void *userdata, int error_code, const char *error_text, const char *failed_url);

    void (*on_url_changed)(void *userdata, const char *url);
    void (*on_title_changed)(void *userdata, const char *title);
    void (*on_tooltip)(void *userdata, const char *text);
    void (*on_console_message)(void *userdata, int level, const char *message, const char *source, int line);

    /// @param cursor_type a cef_cursor_type_t value.
    void (*on_cursor_changed)(void *userdata, int cursor_type);

    /// Called once the underlying browser is gone. After this returns, the
    /// struct wvcef_browser handle must not be used anymore -- the bridge frees
    /// it.
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

    /// Extra command line switches, written without the leading "--". A
    /// "key=value" entry becomes a switch with a value, a bare "key" a boolean
    /// switch.
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
 * @brief Initialize CEF. Must be called on the platform thread. Calling it more
 * than once is a no-op.
 *
 * @returns 0 on success, an errno-style code otherwise.
 */
int wvcef_initialize(const struct wvcef_init_options *options);

/// True if @ref wvcef_initialize succeeded and @ref wvcef_shutdown hasn't run yet.
bool wvcef_is_initialized(void);

/// Let CEF do some work. Call from the platform thread, whenever the
/// @ref wvcef_schedule_pump_cb deadline expires.
void wvcef_do_message_loop_work(void);

/// Number of browsers that were created and haven't fully closed yet.
int wvcef_n_live_browsers(void);

/// Tears CEF down. All browsers must have been closed before. CEF cannot be
/// initialized again afterwards, in this process.
void wvcef_shutdown(void);

/**
 * @brief Create an off-screen browser.
 *
 * @param width,height Size in logical pixels. The buffer delivered to on_paint
 *                     is width*device_pixel_ratio by height*device_pixel_ratio.
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

/// CEF's browser identifier, which is what the dart side of the webview_cef
/// package uses to address a webview. 0 if the browser is already gone.
int wvcef_browser_get_id(struct wvcef_browser *browser);

/// Asks the browser to close. @ref wvcef_host_callbacks::on_closed is called
/// once it's really gone; only then is the handle released.
void wvcef_browser_close(struct wvcef_browser *browser);

void wvcef_browser_load_url(struct wvcef_browser *browser, const char *url);
void wvcef_browser_reload(struct wvcef_browser *browser, bool ignore_cache);
void wvcef_browser_go_back(struct wvcef_browser *browser);
void wvcef_browser_go_forward(struct wvcef_browser *browser);
void wvcef_browser_execute_javascript(struct wvcef_browser *browser, const char *code);

/// Resize the off-screen surface. Sizes are in logical pixels.
void wvcef_browser_resize(struct wvcef_browser *browser, int width, int height, double device_pixel_ratio);

void wvcef_browser_set_focus(struct wvcef_browser *browser, bool focused);

/// Feeds UTF-8 text to whatever has focus in the page. This is how the
/// webview_cef dart side delivers typed text.
void wvcef_browser_ime_commit_text(struct wvcef_browser *browser, const char *text);
void wvcef_browser_ime_set_composition(struct wvcef_browser *browser, const char *text);

/// Coordinates are in logical pixels, relative to the webview's top left corner.
/// @param dragging true to report the left button as held down.
void wvcef_browser_send_mouse_move(struct wvcef_browser *browser, int x, int y, bool dragging);
void wvcef_browser_send_mouse_click(struct wvcef_browser *browser, int x, int y, bool is_up);
void wvcef_browser_send_mouse_wheel(struct wvcef_browser *browser, int x, int y, int delta_x, int delta_y);

#ifdef __cplusplus
}
#endif

#endif  // _FLUTTERPI_SRC_PLUGINS_WEBVIEW_CEF_CEF_BRIDGE_H
