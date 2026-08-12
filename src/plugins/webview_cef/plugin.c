// SPDX-License-Identifier: MIT
/*
 * CEF powered webview plugin
 *
 * The flutter-pi side of the webview: owns the platform channel, drives CEF's
 * message pump from flutter-pi's event loop, and turns the BGRA frames CEF
 * renders off-screen into flutter external textures.
 *
 * Everything in here runs on the platform thread:
 *   - platform channel handlers are dispatched there by the plugin registry,
 *   - CEF is initialized there, which makes it CEF's "UI thread", so all
 *     cef_bridge callbacks (including on_paint) arrive there too.
 *
 * The only exception is on_schedule_pump_work, which CEF may call from any
 * thread; it just posts a task back to the platform thread.
 *
 * Copyright (c) 2026, EGT
 */

#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>

#include "flutter-pi.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "texture_registry.h"
#include "util/logging.h"

#include "config.h"

#ifndef HAVE_EGL_GLES2
    #error "The webview_cef plugin requires EGL/OpenGL ES support."
#endif

#include "gl_renderer.h"
#include "gles.h"
#include "plugins/webview_cef.h"
#include "plugins/webview_cef/cef_bridge.h"

/// Where the CEF runtime files (libcef.so, icudtl.dat, *.pak, *.bin, locales/)
/// were installed. Overridable at configure time and at runtime, see
/// resolve_path() below.
#ifndef WEBVIEW_CEF_RUNTIME_DIR
    #define WEBVIEW_CEF_RUNTIME_DIR "/usr/lib/cef"
#endif

/// The CEF subprocess helper executable.
#ifndef WEBVIEW_CEF_HELPER_PATH
    #define WEBVIEW_CEF_HELPER_PATH "/usr/bin/flutter-pi-cef-helper"
#endif

#define MAX_SWITCHES 64

/// Chromium switches we pass unless the app overrides them. The important ones
/// are the first three: there is no X server and no wayland compositor on the
/// kiosk, so Chromium has to use the headless ozone platform and must not try to
/// bring up its own GPU stack next to flutter-pi's.
static const char *const default_switches[] = {
    "ozone-platform=headless",
    "disable-gpu",
    "disable-gpu-compositing",
    "disable-dev-shm-usage",
    "autoplay-policy=no-user-gesture-required",
};

struct webview {
    struct webview *next;

    struct texture *texture;
    int64_t texture_id;

    struct wvcef_browser *browser;

    /// Logical size & scale, as last told by the dart side.
    int logical_width, logical_height;
    double pixel_ratio;

    /// The GL texture the CEF frames are uploaded into. Kept for the lifetime of
    /// the webview and re-uploaded in place on every frame.
    GLuint gl_texture;
    int tex_width, tex_height;

    /// Only used if the driver can't take BGRA pixels directly.
    uint8_t *swizzle_buffer;
    size_t swizzle_buffer_size;

    /// dart called `dispose`; the webview is torn down but still waiting for
    /// CEF to confirm the browser is gone.
    bool disposing;
};

struct webview_cef_plugin {
    struct flutterpi *flutterpi;

    EGLDisplay egl_display;
    EGLContext egl_context;
    bool supports_bgra;

    bool cef_initialized;

    /// Guards the pump bookkeeping, which CEF may touch from other threads.
    pthread_mutex_t pump_mutex;
    bool pump_scheduled;
    uint64_t pump_deadline_us;

    struct webview *webviews;
};

/// There's one webview plugin per process, and both the platform channel
/// receiver and the CEF callbacks need to reach it, so it's a file-scope
/// singleton rather than something passed around as userdata.
static struct webview_cef_plugin plugin;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static uint64_t now_monotonic_us(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ull + (uint64_t) ts.tv_nsec / 1000ull;
}

/// dart argument > environment variable > compile time default.
static const char *resolve_path(const char *from_dart, const char *env_name, const char *fallback) {
    const char *env;

    if (from_dart != NULL && from_dart[0] != '\0') {
        return from_dart;
    }

    env = getenv(env_name);
    if (env != NULL && env[0] != '\0') {
        return env;
    }

    return fallback;
}

static struct std_value *arg_get(struct std_value *args, const char *key) {
    if (args == NULL || !STDVALUE_IS_MAP(*args)) {
        return NULL;
    }
    return stdmap_get_str(args, (char *) key);
}

static bool arg_get_int(struct std_value *args, const char *key, int64_t *out) {
    struct std_value *value = arg_get(args, key);

    if (value == NULL || !STDVALUE_IS_INT(*value)) {
        return false;
    }

    *out = STDVALUE_AS_INT(*value);
    return true;
}

static bool arg_get_num(struct std_value *args, const char *key, double *out) {
    struct std_value *value = arg_get(args, key);

    if (value == NULL || !STDVALUE_IS_NUM(*value)) {
        return false;
    }

    *out = (double) STDVALUE_AS_NUM(*value);
    return true;
}

static const char *arg_get_string(struct std_value *args, const char *key) {
    struct std_value *value = arg_get(args, key);

    if (value == NULL || !STDVALUE_IS_STRING(*value)) {
        return NULL;
    }

    return STDVALUE_AS_STRING(*value);
}

static bool arg_get_bool(struct std_value *args, const char *key, bool fallback) {
    struct std_value *value = arg_get(args, key);

    if (value == NULL || !STDVALUE_IS_BOOL(*value)) {
        return fallback;
    }

    return STDVALUE_AS_BOOL(*value);
}

static struct webview *webview_find(int64_t texture_id) {
    for (struct webview *wv = plugin.webviews; wv != NULL; wv = wv->next) {
        if (wv->texture_id == texture_id) {
            return wv;
        }
    }
    return NULL;
}

static void webview_list_add(struct webview *wv) {
    wv->next = plugin.webviews;
    plugin.webviews = wv;
}

static void webview_list_remove(struct webview *wv) {
    struct webview **slot = &plugin.webviews;

    while (*slot != NULL) {
        if (*slot == wv) {
            *slot = wv->next;
            return;
        }
        slot = &(*slot)->next;
    }
}

// ---------------------------------------------------------------------------
// CEF message pump, driven from flutter-pi's event loop
// ---------------------------------------------------------------------------

static int on_pump_message_loop(void *userdata) {
    (void) userdata;

    pthread_mutex_lock(&plugin.pump_mutex);
    plugin.pump_scheduled = false;
    pthread_mutex_unlock(&plugin.pump_mutex);

    wvcef_do_message_loop_work();
    return 0;
}

/// MAY BE CALLED FROM ANY THREAD.
static void on_schedule_pump_work(void *userdata, int64_t delay_ms) {
    uint64_t deadline_us;
    int ok;

    (void) userdata;

    if (delay_ms < 0) {
        delay_ms = 0;
    }

    deadline_us = now_monotonic_us() + (uint64_t) delay_ms * 1000ull;

    pthread_mutex_lock(&plugin.pump_mutex);
    if (plugin.pump_scheduled && plugin.pump_deadline_us <= deadline_us) {
        // A pump is already queued that runs no later than this one wants.
        pthread_mutex_unlock(&plugin.pump_mutex);
        return;
    }
    plugin.pump_scheduled = true;
    plugin.pump_deadline_us = deadline_us;
    pthread_mutex_unlock(&plugin.pump_mutex);

    ok = flutterpi_post_platform_task_with_time(on_pump_message_loop, NULL, deadline_us);
    if (ok != 0) {
        LOG_ERROR("Could not schedule CEF message pump work: %s\n", strerror(ok));

        pthread_mutex_lock(&plugin.pump_mutex);
        plugin.pump_scheduled = false;
        pthread_mutex_unlock(&plugin.pump_mutex);
    }
}

static void pump_soon(void) {
    on_schedule_pump_work(NULL, 0);
}

// ---------------------------------------------------------------------------
// frame upload
// ---------------------------------------------------------------------------

static void on_texture_frame_destroy(const struct texture_frame *frame, void *userdata) {
    // The GL texture belongs to the webview and is reused for every frame, so
    // there is nothing to release per frame.
    (void) frame;
    (void) userdata;
}

/// Converts BGRA to RGBA in place of a scratch buffer, for drivers without
/// GL_EXT_texture_format_BGRA8888.
static const void *swizzle_bgra_to_rgba(struct webview *wv, const void *buffer, int width, int height) {
    const uint8_t *src;
    uint8_t *dst;
    size_t n_pixels, needed;

    n_pixels = (size_t) width * (size_t) height;
    needed = n_pixels * 4;

    if (wv->swizzle_buffer_size < needed) {
        uint8_t *new_buffer = realloc(wv->swizzle_buffer, needed);
        if (new_buffer == NULL) {
            return NULL;
        }
        wv->swizzle_buffer = new_buffer;
        wv->swizzle_buffer_size = needed;
    }

    src = buffer;
    dst = wv->swizzle_buffer;

    for (size_t i = 0; i < n_pixels; i++) {
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = src[i * 4 + 3];
    }

    return dst;
}

static void on_paint(void *userdata, const void *buffer, int width, int height) {
    struct webview *wv;
    const void *pixels;
    GLenum gl_format, gl_error;
    EGLBoolean egl_ok;
    bool uploaded = false;

    wv = userdata;

    // Disposed while a frame was in flight.
    if (wv->texture == NULL || width <= 0 || height <= 0) {
        return;
    }

    if (plugin.supports_bgra) {
        gl_format = GL_BGRA_EXT;
        pixels = buffer;
    } else {
        gl_format = GL_RGBA;
        pixels = swizzle_bgra_to_rgba(wv, buffer, width, height);
        if (pixels == NULL) {
            LOG_ERROR("Out of memory while converting a webview frame.\n");
            return;
        }
    }

    egl_ok = eglMakeCurrent(plugin.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, plugin.egl_context);
    if (egl_ok == EGL_FALSE) {
        LOG_ERROR("Could not make the webview EGL context current. eglMakeCurrent: 0x%04X\n", eglGetError());
        return;
    }

    if (wv->gl_texture == 0) {
        glGenTextures(1, &wv->gl_texture);
        if (wv->gl_texture == 0) {
            LOG_ERROR("Could not create a GL texture for the webview. glGenTextures: 0x%04X\n", glGetError());
            goto clear_context;
        }

        glBindTexture(GL_TEXTURE_2D, wv->gl_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        wv->tex_width = 0;
        wv->tex_height = 0;
    } else {
        glBindTexture(GL_TEXTURE_2D, wv->gl_texture);
    }

    if (wv->tex_width != width || wv->tex_height != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, gl_format, width, height, 0, gl_format, GL_UNSIGNED_BYTE, pixels);

        gl_error = glGetError();
        if (gl_error != GL_NO_ERROR) {
            LOG_ERROR("Could not allocate the webview GL texture. glTexImage2D: 0x%04X\n", gl_error);
            glBindTexture(GL_TEXTURE_2D, 0);
            goto clear_context;
        }

        wv->tex_width = width;
        wv->tex_height = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, gl_format, GL_UNSIGNED_BYTE, pixels);

        gl_error = glGetError();
        if (gl_error != GL_NO_ERROR) {
            LOG_ERROR("Could not upload a webview frame. glTexSubImage2D: 0x%04X\n", gl_error);
            glBindTexture(GL_TEXTURE_2D, 0);
            goto clear_context;
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    // The flutter rasterizer samples this texture from a different thread and a
    // different (shared) context, so the upload has to be complete before we
    // hand the frame over. glFlush() is the minimum the GLES spec asks for, but
    // in practice only glFinish() is reliable across drivers. If this ever shows
    // up in a profile, an EGL fence sync is the way to relax it.
    glFinish();
    uploaded = true;

clear_context:
    eglMakeCurrent(plugin.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (!uploaded) {
        return;
    }

    texture_push_frame(
        wv->texture,
        &(struct texture_frame){
            .gl = {
                .target = GL_TEXTURE_2D,
                .name = wv->gl_texture,
                .format = GL_RGBA8_OES,
                .width = (size_t) width,
                .height = (size_t) height,
            },
            .destroy = on_texture_frame_destroy,
            .userdata = NULL,
        }
    );
}

// ---------------------------------------------------------------------------
// events towards dart
// ---------------------------------------------------------------------------

static void send_event(int64_t texture_id, const char *method, struct std_value *extra_keys, struct std_value *extra_values, size_t n_extra) {
    struct std_value keys[5];
    struct std_value values[5];
    struct std_value event;

    if (n_extra > 4) {
        n_extra = 4;
    }

    keys[0] = STDSTRING("textureId");
    values[0] = STDINT64(texture_id);

    for (size_t i = 0; i < n_extra; i++) {
        keys[i + 1] = extra_keys[i];
        values[i + 1] = extra_values[i];
    }

    event.type = kStdMap;
    event.size = n_extra + 1;
    event.keys = keys;
    event.values = values;

    platch_call_std(WEBVIEW_CEF_CHANNEL, (char *) method, &event, NULL, NULL);
}

static void on_loading_state_changed(void *userdata, bool is_loading, bool can_go_back, bool can_go_forward) {
    struct webview *wv = userdata;

    struct std_value keys[3] = { STDSTRING("isLoading"), STDSTRING("canGoBack"), STDSTRING("canGoForward") };
    struct std_value values[3] = { STDBOOL(is_loading), STDBOOL(can_go_back), STDBOOL(can_go_forward) };

    send_event(wv->texture_id, "onLoadingStateChanged", keys, values, 3);
}

static void on_url_changed(void *userdata, const char *url) {
    struct webview *wv = userdata;

    struct std_value keys[1] = { STDSTRING("url") };
    struct std_value values[1] = { STDSTRING((char *) (url != NULL ? url : "")) };

    send_event(wv->texture_id, "onUrlChanged", keys, values, 1);
}

static void on_title_changed(void *userdata, const char *title) {
    struct webview *wv = userdata;

    struct std_value keys[1] = { STDSTRING("title") };
    struct std_value values[1] = { STDSTRING((char *) (title != NULL ? title : "")) };

    send_event(wv->texture_id, "onTitleChanged", keys, values, 1);
}

static void on_load_error(void *userdata, int error_code, const char *error_text, const char *failed_url) {
    struct webview *wv = userdata;

    struct std_value keys[3] = { STDSTRING("errorCode"), STDSTRING("errorText"), STDSTRING("failedUrl") };
    struct std_value values[3] = {
        STDINT32(error_code),
        STDSTRING((char *) (error_text != NULL ? error_text : "")),
        STDSTRING((char *) (failed_url != NULL ? failed_url : "")),
    };

    send_event(wv->texture_id, "onLoadError", keys, values, 3);
}

/// The browser is really gone now, so the webview can be freed.
static void on_browser_closed(void *userdata) {
    struct webview *wv = userdata;

    wv->browser = NULL;

    if (!wv->disposing) {
        // CEF closed the browser on its own (a crashed renderer, for example).
        // Tell dart, and keep the texture around showing the last frame.
        struct std_value keys[3] = { STDSTRING("errorCode"), STDSTRING("errorText"), STDSTRING("failedUrl") };
        struct std_value values[3] = { STDINT32(0), STDSTRING("The browser was closed unexpectedly."), STDSTRING("") };

        LOG_ERROR("Webview %" PRId64 " was closed by CEF.\n", wv->texture_id);
        send_event(wv->texture_id, "onLoadError", keys, values, 3);
        return;
    }

    webview_list_remove(wv);
    free(wv->swizzle_buffer);
    free(wv);
}

static const struct wvcef_host_callbacks host_callbacks = {
    .on_paint = on_paint,
    .on_loading_state_changed = on_loading_state_changed,
    .on_url_changed = on_url_changed,
    .on_title_changed = on_title_changed,
    .on_load_error = on_load_error,
    .on_closed = on_browser_closed,
};

// ---------------------------------------------------------------------------
// CEF initialization
// ---------------------------------------------------------------------------

/// Splits a comma separated switch list into `switches`, writing into `storage`.
/// Returns the number of switches added.
static size_t parse_switch_list(char *list, const char **switches, size_t n_switches, size_t max_switches) {
    char *cursor = list;

    while (cursor != NULL && *cursor != '\0' && n_switches < max_switches) {
        char *comma = strchr(cursor, ',');

        if (comma != NULL) {
            *comma = '\0';
        }

        if (*cursor != '\0') {
            switches[n_switches++] = cursor;
        }

        cursor = comma != NULL ? comma + 1 : NULL;
    }

    return n_switches;
}

static int ensure_cef_initialized(struct std_value *args) {
    struct wvcef_init_options options;
    const char *switches[MAX_SWITCHES];
    char *env_switches_copy = NULL;
    char resources_dir[512];
    char locales_dir[512];
    const char *runtime_dir;
    const char *env_switches;
    struct std_value *switch_list;
    size_t n_switches = 0;
    int64_t log_severity = 0;
    int ok;

    if (plugin.cef_initialized) {
        return 0;
    }

    memset(&options, 0, sizeof options);

    runtime_dir = resolve_path(arg_get_string(args, "runtimeDir"), "FLUTTERPI_CEF_RUNTIME_DIR", WEBVIEW_CEF_RUNTIME_DIR);

    snprintf(resources_dir, sizeof(resources_dir), "%s", runtime_dir);
    snprintf(locales_dir, sizeof(locales_dir), "%s/locales", runtime_dir);

    options.subprocess_path = resolve_path(arg_get_string(args, "helperPath"), "FLUTTERPI_CEF_HELPER", WEBVIEW_CEF_HELPER_PATH);
    options.resources_dir = resources_dir;
    options.locales_dir = locales_dir;
    options.cache_path = arg_get_string(args, "cachePath");
    options.user_agent = arg_get_string(args, "userAgent");
    options.log_file = resolve_path(arg_get_string(args, "logFile"), "FLUTTERPI_CEF_LOG_FILE", NULL);

    if (arg_get_int(args, "logSeverity", &log_severity)) {
        options.log_severity = (int) log_severity;
    }

    if (arg_get_bool(args, "useDefaultSwitches", true)) {
        for (size_t i = 0; i < ARRAY_SIZE(default_switches) && n_switches < MAX_SWITCHES; i++) {
            switches[n_switches++] = default_switches[i];
        }
    }

    // FLUTTERPI_CEF_SWITCHES=disable-gpu,ozone-platform=headless
    env_switches = getenv("FLUTTERPI_CEF_SWITCHES");
    if (env_switches != NULL && env_switches[0] != '\0') {
        env_switches_copy = strdup(env_switches);
        if (env_switches_copy != NULL) {
            n_switches = parse_switch_list(env_switches_copy, switches, n_switches, MAX_SWITCHES);
        }
    }

    switch_list = arg_get(args, "switches");
    if (switch_list != NULL && STDVALUE_IS_LIST(*switch_list)) {
        for (size_t i = 0; i < switch_list->size && n_switches < MAX_SWITCHES; i++) {
            if (STDVALUE_IS_STRING(switch_list->list[i])) {
                switches[n_switches++] = STDVALUE_AS_STRING(switch_list->list[i]);
            }
        }
    }

    options.switches = switches;
    options.n_switches = n_switches;
    options.schedule_pump = on_schedule_pump_work;
    options.schedule_pump_userdata = NULL;

    LOG_DEBUG("Initializing CEF. runtime dir: %s, helper: %s\n", runtime_dir, options.subprocess_path);

    ok = wvcef_initialize(&options);

    free(env_switches_copy);

    if (ok != 0) {
        return ok;
    }

    plugin.cef_initialized = true;
    pump_soon();
    return 0;
}

// ---------------------------------------------------------------------------
// method handlers
// ---------------------------------------------------------------------------

static int on_init(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    int ok = ensure_cef_initialized(args);

    if (ok != 0) {
        return platch_respond_error_std(response_handle, "cef-init-failed", "Could not initialize CEF. See the flutter-pi log.", &STDNULL);
    }

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_create(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    const char *url;
    double width = 0.0, height = 0.0, pixel_ratio = 1.0;
    int64_t frame_rate = 30;
    int ok;

    ok = ensure_cef_initialized(args);
    if (ok != 0) {
        return platch_respond_error_std(response_handle, "cef-init-failed", "Could not initialize CEF. See the flutter-pi log.", &STDNULL);
    }

    url = arg_get_string(args, "url");

    if (!arg_get_num(args, "width", &width) || !arg_get_num(args, "height", &height)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `width` and `height` to be numbers.");
    }

    arg_get_num(args, "pixelRatio", &pixel_ratio);
    arg_get_int(args, "frameRate", &frame_rate);

    if (pixel_ratio <= 0.0) {
        pixel_ratio = 1.0;
    }

    wv = calloc(1, sizeof *wv);
    if (wv == NULL) {
        return platch_respond_native_error_std(response_handle, ENOMEM);
    }

    wv->logical_width = (int) (width > 1.0 ? width : 1.0);
    wv->logical_height = (int) (height > 1.0 ? height : 1.0);
    wv->pixel_ratio = pixel_ratio;

    wv->texture = flutterpi_create_texture(plugin.flutterpi);
    if (wv->texture == NULL) {
        LOG_ERROR("Could not create a flutter texture for the webview.\n");
        free(wv);
        return platch_respond_error_std(response_handle, "texture-failed", "Could not create a flutter texture.", &STDNULL);
    }

    wv->texture_id = texture_get_id(wv->texture);

    // Registered before the browser exists so a very early on_paint can find it.
    webview_list_add(wv);

    wv->browser = wvcef_browser_create(
        url,
        wv->logical_width,
        wv->logical_height,
        wv->pixel_ratio,
        (int) frame_rate,
        &host_callbacks,
        wv
    );
    if (wv->browser == NULL) {
        LOG_ERROR("Could not create the CEF browser.\n");
        webview_list_remove(wv);
        texture_destroy(wv->texture);
        free(wv);
        return platch_respond_error_std(response_handle, "browser-failed", "Could not create the CEF browser.", &STDNULL);
    }

    LOG_DEBUG(
        "Created webview %" PRId64 " (%dx%d @ %.2f) for %s\n",
        wv->texture_id,
        wv->logical_width,
        wv->logical_height,
        wv->pixel_ratio,
        url != NULL ? url : "about:blank"
    );

    pump_soon();

    return platch_respond_success_std(response_handle, &STDINT64(wv->texture_id));
}

/// Looks up the webview a call refers to, responding with an error if it's gone.
static struct webview *webview_for_call(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle, int *response_out) {
    struct webview *wv;
    int64_t texture_id = 0;

    if (!arg_get_int(args, "textureId", &texture_id)) {
        *response_out = platch_respond_illegal_arg_std(response_handle, "Expected `textureId` to be an integer.");
        return NULL;
    }

    wv = webview_find(texture_id);
    if (wv == NULL || wv->disposing) {
        *response_out = platch_respond_error_std(response_handle, "no-such-webview", "There is no webview with that textureId.", &STDNULL);
        return NULL;
    }

    return wv;
}

static int on_dispose(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    int response = 0;

    wv = webview_for_call(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    wv->disposing = true;

    // Stop rendering into the texture before it goes away.
    if (wv->texture != NULL) {
        texture_destroy(wv->texture);
        wv->texture = NULL;
    }

    if (wv->gl_texture != 0) {
        if (eglMakeCurrent(plugin.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, plugin.egl_context) == EGL_TRUE) {
            glDeleteTextures(1, &wv->gl_texture);
            eglMakeCurrent(plugin.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        } else {
            LOG_ERROR("Could not make the webview EGL context current to delete a texture. eglMakeCurrent: 0x%04X\n", eglGetError());
        }
        wv->gl_texture = 0;
    }

    if (wv->browser != NULL) {
        // `wv` is freed in on_browser_closed.
        wvcef_browser_close(wv->browser);
        pump_soon();
    } else {
        webview_list_remove(wv);
        free(wv->swizzle_buffer);
        free(wv);
    }

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_load_url(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    const char *url;
    int response = 0;

    wv = webview_for_call(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    url = arg_get_string(args, "url");
    if (url == NULL) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `url` to be a string.");
    }

    wvcef_browser_load_url(wv->browser, url);
    pump_soon();

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_set_size(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    double width = 0.0, height = 0.0, pixel_ratio;
    int response = 0;

    wv = webview_for_call(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    if (!arg_get_num(args, "width", &width) || !arg_get_num(args, "height", &height)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `width` and `height` to be numbers.");
    }

    pixel_ratio = wv->pixel_ratio;
    arg_get_num(args, "pixelRatio", &pixel_ratio);
    if (pixel_ratio <= 0.0) {
        pixel_ratio = 1.0;
    }

    wv->logical_width = (int) (width > 1.0 ? width : 1.0);
    wv->logical_height = (int) (height > 1.0 ? height : 1.0);
    wv->pixel_ratio = pixel_ratio;

    wvcef_browser_resize(wv->browser, wv->logical_width, wv->logical_height, wv->pixel_ratio);
    pump_soon();

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_navigation(const char *method, struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    int response = 0;

    wv = webview_for_call(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    if (streq(method, "reload")) {
        wvcef_browser_reload(wv->browser, arg_get_bool(args, "ignoreCache", false));
    } else if (streq(method, "stopLoad")) {
        wvcef_browser_stop_load(wv->browser);
    } else if (streq(method, "goBack")) {
        wvcef_browser_go_back(wv->browser);
    } else if (streq(method, "goForward")) {
        wvcef_browser_go_forward(wv->browser);
    } else if (streq(method, "invalidate")) {
        wvcef_browser_invalidate(wv->browser);
    } else {
        return platch_respond_not_implemented(response_handle);
    }

    pump_soon();
    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_set_focus(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    int response = 0;

    wv = webview_for_call(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    wvcef_browser_set_focus(wv->browser, arg_get_bool(args, "focused", true));
    pump_soon();

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_run_javascript(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    const char *code;
    int response = 0;

    wv = webview_for_call(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    code = arg_get_string(args, "code");
    if (code == NULL) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `code` to be a string.");
    }

    wvcef_browser_execute_javascript(wv->browser, code);
    pump_soon();

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_pointer_event(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    const char *phase, *kind;
    double x = 0.0, y = 0.0;
    int64_t pointer = 0, button = 0, click_count = 1, modifiers = 0;
    int response = 0;

    wv = webview_for_call(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    phase = arg_get_string(args, "phase");
    if (phase == NULL) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `phase` to be a string.");
    }

    if (!arg_get_num(args, "x", &x) || !arg_get_num(args, "y", &y)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `x` and `y` to be numbers.");
    }

    kind = arg_get_string(args, "kind");
    arg_get_int(args, "pointer", &pointer);
    arg_get_int(args, "button", &button);
    arg_get_int(args, "clickCount", &click_count);
    arg_get_int(args, "modifiers", &modifiers);

    if (kind != NULL && streq(kind, "touch")) {
        enum wvcef_touch_phase touch_phase;

        if (streq(phase, "down")) {
            touch_phase = WVCEF_TOUCH_PRESSED;
        } else if (streq(phase, "move")) {
            touch_phase = WVCEF_TOUCH_MOVED;
        } else if (streq(phase, "up")) {
            touch_phase = WVCEF_TOUCH_RELEASED;
        } else {
            touch_phase = WVCEF_TOUCH_CANCELLED;
        }

        wvcef_browser_send_touch(wv->browser, (int) pointer, (int) x, (int) y, touch_phase, (uint32_t) modifiers);
    } else {
        if (streq(phase, "move")) {
            wvcef_browser_send_mouse_move(wv->browser, (int) x, (int) y, false, (uint32_t) modifiers);
        } else if (streq(phase, "down")) {
            wvcef_browser_send_mouse_button(
                wv->browser,
                (int) x,
                (int) y,
                (enum wvcef_pointer_button) button,
                false,
                (int) click_count,
                (uint32_t) modifiers
            );
        } else if (streq(phase, "up")) {
            wvcef_browser_send_mouse_button(
                wv->browser,
                (int) x,
                (int) y,
                (enum wvcef_pointer_button) button,
                true,
                (int) click_count,
                (uint32_t) modifiers
            );
        } else {
            // "cancel": pretend the pointer left the view.
            wvcef_browser_send_mouse_move(wv->browser, (int) x, (int) y, true, (uint32_t) modifiers);
        }
    }

    pump_soon();
    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_scroll(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    double x = 0.0, y = 0.0, delta_x = 0.0, delta_y = 0.0;
    int64_t modifiers = 0;
    int response = 0;

    wv = webview_for_call(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    if (!arg_get_num(args, "x", &x) || !arg_get_num(args, "y", &y)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `x` and `y` to be numbers.");
    }

    arg_get_num(args, "deltaX", &delta_x);
    arg_get_num(args, "deltaY", &delta_y);
    arg_get_int(args, "modifiers", &modifiers);

    wvcef_browser_send_mouse_wheel(wv->browser, (int) x, (int) y, (int) delta_x, (int) delta_y, (uint32_t) modifiers);
    pump_soon();

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_key_event(struct std_value *args, FlutterPlatformMessageResponseHandle *response_handle) {
    struct webview *wv;
    const char *phase;
    int64_t key_code = 0, native_key_code = 0, character = 0, modifiers = 0;
    enum wvcef_key_type type;
    int response = 0;

    wv = webview_for_call(args, response_handle, &response);
    if (wv == NULL) {
        return response;
    }

    phase = arg_get_string(args, "phase");
    if (phase == NULL) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `phase` to be a string.");
    }

    arg_get_int(args, "keyCode", &key_code);
    arg_get_int(args, "nativeKeyCode", &native_key_code);
    arg_get_int(args, "character", &character);
    arg_get_int(args, "modifiers", &modifiers);

    if (streq(phase, "down")) {
        type = WVCEF_KEY_DOWN;
    } else if (streq(phase, "up")) {
        type = WVCEF_KEY_UP;
    } else if (streq(phase, "rawDown")) {
        type = WVCEF_KEY_RAWDOWN;
    } else {
        type = WVCEF_KEY_CHAR;
    }

    wvcef_browser_send_key(wv->browser, type, (int) key_code, (int) native_key_code, (uint32_t) character, (uint32_t) modifiers);
    pump_soon();

    return platch_respond_success_std(response_handle, &STDNULL);
}

static int on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    struct std_value *args;
    const char *method;

    (void) channel;

    method = object->method;
    args = &object->std_arg;

    if (streq(method, "init")) {
        return on_init(args, response_handle);
    } else if (streq(method, "create")) {
        return on_create(args, response_handle);
    } else if (streq(method, "dispose")) {
        return on_dispose(args, response_handle);
    } else if (streq(method, "loadUrl")) {
        return on_load_url(args, response_handle);
    } else if (streq(method, "setSize")) {
        return on_set_size(args, response_handle);
    } else if (streq(method, "setFocus")) {
        return on_set_focus(args, response_handle);
    } else if (streq(method, "runJavaScript")) {
        return on_run_javascript(args, response_handle);
    } else if (streq(method, "pointerEvent")) {
        return on_pointer_event(args, response_handle);
    } else if (streq(method, "scroll")) {
        return on_scroll(args, response_handle);
    } else if (streq(method, "keyEvent")) {
        return on_key_event(args, response_handle);
    } else if (streq(method, "reload") || streq(method, "stopLoad") || streq(method, "goBack") || streq(method, "goForward") ||
               streq(method, "invalidate")) {
        return on_navigation(method, args, response_handle);
    }

    return platch_respond_not_implemented(response_handle);
}

// ---------------------------------------------------------------------------
// plugin lifecycle
// ---------------------------------------------------------------------------

enum plugin_init_result webview_cef_init(struct flutterpi *flutterpi, void **userdata_out) {
    struct gl_renderer *renderer;
    EGLDisplay display;
    EGLContext context;
    int ok;

    if (!flutterpi_has_gl_renderer(flutterpi)) {
        LOG_ERROR("The webview plugin needs EGL/OpenGL ES rendering, which is not available. Webviews will not work.\n");
        return PLUGIN_INIT_RESULT_NOT_APPLICABLE;
    }

    renderer = flutterpi_get_gl_renderer(flutterpi);

    display = gl_renderer_get_egl_display(renderer);
    if (display == EGL_NO_DISPLAY) {
        LOG_ERROR("The GL renderer has no EGL display.\n");
        return PLUGIN_INIT_RESULT_NOT_APPLICABLE;
    }

    // Shares the flutter root context, so the textures we fill here can be
    // sampled by the flutter rasterizer.
    context = gl_renderer_create_context(renderer);
    if (context == EGL_NO_CONTEXT) {
        LOG_ERROR("Could not create an EGL context for the webview plugin. eglCreateContext: 0x%04X\n", eglGetError());
        return PLUGIN_INIT_RESULT_ERROR;
    }

    memset(&plugin, 0, sizeof plugin);

    plugin.flutterpi = flutterpi;
    plugin.egl_display = display;
    plugin.egl_context = context;
    plugin.supports_bgra = gl_renderer_supports_gl_extension(renderer, "GL_EXT_texture_format_BGRA8888");

    if (!plugin.supports_bgra) {
        LOG_ERROR("GL_EXT_texture_format_BGRA8888 is missing; webview frames will be converted on the CPU, which is slow.\n");
    }

    pthread_mutex_init(&plugin.pump_mutex, NULL);

    ok = plugin_registry_set_receiver_locked(WEBVIEW_CEF_CHANNEL, kStandardMethodCall, on_receive);
    if (ok != 0) {
        LOG_ERROR("Could not set the webview platform channel receiver: %s\n", strerror(ok));
        pthread_mutex_destroy(&plugin.pump_mutex);
        eglDestroyContext(display, context);
        return PLUGIN_INIT_RESULT_ERROR;
    }

    // CEF itself is only started on the first `init`/`create` call, so a kiosk
    // that never opens a webview doesn't pay for Chromium's ~150MB of RAM.
    //
    // Plugins are initialized before the flutter engine is created though, so
    // starting CEF here means its helper processes are forked out of a process
    // that isn't heavily threaded yet, and Chromium's signal handlers are
    // installed before the engine's. If lazy initialization ever turns out to be
    // flaky, set FLUTTERPI_CEF_EAGER_INIT=1 and configure CEF through the
    // FLUTTERPI_CEF_* environment variables instead of through `init`.
    if (getenv("FLUTTERPI_CEF_EAGER_INIT") != NULL) {
        ok = ensure_cef_initialized(NULL);
        if (ok != 0) {
            LOG_ERROR("Eager CEF initialization failed: %s. Webviews will not work.\n", strerror(ok));
        }
    }

    *userdata_out = &plugin;
    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void webview_cef_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) flutterpi;
    (void) userdata;

    plugin_registry_remove_receiver_locked(WEBVIEW_CEF_CHANNEL);

    // Close every browser and let CEF finish the teardown. Without this,
    // CefShutdown() aborts.
    //
    // wvcef_browser_close() can complete synchronously, in which case
    // on_browser_closed() unlinks and frees the webview -- so remember `next`
    // before closing.
    for (struct webview *wv = plugin.webviews, *next = NULL; wv != NULL; wv = next) {
        next = wv->next;

        wv->disposing = true;

        if (wv->texture != NULL) {
            texture_destroy(wv->texture);
            wv->texture = NULL;
        }
        if (wv->browser != NULL) {
            wvcef_browser_close(wv->browser);
        }
    }

    if (plugin.cef_initialized) {
        // 200 * 10ms = 2s worth of patience.
        for (int i = 0; i < 200 && wvcef_n_live_browsers() > 0; i++) {
            wvcef_do_message_loop_work();

            if (wvcef_n_live_browsers() == 0) {
                break;
            }

            nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 }, NULL);
        }

        if (wvcef_n_live_browsers() > 0) {
            LOG_ERROR("%d webview(s) did not close in time; skipping CEF shutdown.\n", wvcef_n_live_browsers());
        } else {
            wvcef_shutdown();
        }

        plugin.cef_initialized = false;
    }

    // on_browser_closed already freed everything that actually closed.
    while (plugin.webviews != NULL) {
        struct webview *wv = plugin.webviews;

        plugin.webviews = wv->next;
        free(wv->swizzle_buffer);
        free(wv);
    }

    if (plugin.egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(plugin.egl_display, plugin.egl_context);
        plugin.egl_context = EGL_NO_CONTEXT;
    }

    pthread_mutex_destroy(&plugin.pump_mutex);
}

FLUTTERPI_PLUGIN("webview cef", webview_cef_plugin, webview_cef_init, webview_cef_deinit)
