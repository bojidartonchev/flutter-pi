#define _GNU_SOURCE

#include "plugins/webview_cef.h"
#include "flutter-pi.h"
#include "pluginregistry.h"
#include "util/logging.h"

static int on_init(struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    struct std_value *args;
    char *userAgent;

    args = &object->std_arg;

    if (args != NULL && STDVALUE_IS_STRING(*args)) {
        userAgent = STDVALUE_AS_STRING(*args);
    }

    int ok = platch_respond_success_std(response_handle, &STDINT32(1));

    return ok;
}

static int on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    (void) channel;

    const char *method;
    method = object->method;

    if (streq(method, "init")) {
        return on_init(object, response_handle);
    }

    return platch_respond_not_implemented(response_handle);
}

enum plugin_init_result webview_cef_init(struct flutterpi *flutterpi, void **userdata_out) {
    (void) flutterpi;

    int ok;

    ok = plugin_registry_set_receiver_locked(WEBVIEW_CEF_CHANNEL, kStandardMethodCall, on_receive);
    if (ok != 0) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    *userdata_out = NULL;

    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void webview_cef_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) userdata;

    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), WEBVIEW_CEF_CHANNEL);
}

FLUTTERPI_PLUGIN("webview cef plugin", webview_cef_plugin, webview_cef_init, webview_cef_deinit)