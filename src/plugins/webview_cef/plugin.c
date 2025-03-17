#define _GNU_SOURCE

#include "plugins/webview_ceh.h"
#include "flutter-pi.h"
#include "pluginregistry.h"
#include "util/logging.h"

static int on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    (void) channel;

    const char *method;
    method = object->method;

    if (streq(method, "init")) {
        LOG_ERROR("Shte hendalvame init metoda.\n");
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