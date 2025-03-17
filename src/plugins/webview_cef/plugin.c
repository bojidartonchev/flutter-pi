#define _GNU_SOURCE

#include "plugins/webview_cef.h"
#include "flutter-pi.h"
#include "pluginregistry.h"
#include "util/logging.h"
#include "common/webview_plugin.h"
#include "keyevent.h"
#include "texture.h"

static struct plugin {
    //pthread_mutex_t lock;

    struct flutterpi *flutterpi;
    bool initialized;
    //webview_cef::WebviewPlugin *webview;
} plugin;

static WValue *encode_stdvalue_to_wvalue(struct std_value *args)
{
    if(STDVALUE_IS_BOOL(args)) {
        return webview_value_new_bool(STDVALUE_AS_BOOL(args));
    }

    if(STDVALUE_IS_INT(args)) {
        return webview_value_new_int(STDVALUE_AS_INT(args));
    }

    //TODO Handle everything

    return webview_value_new_null();
}

static int on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    (void) channel;

    const char *method;
    method = object->method;

    WValue *encodeArgs = encode_stdvalue_to_wvalue(&object->std_arg);

    //TODO Forward to plugin

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

    plugin.flutterpi = flutterpi;
    plugin.initialized = false;
    //plugin.webview = new webview_cef::WebviewPlugin();

    //TODO Initialize

    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void webview_cef_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) userdata;

    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), WEBVIEW_CEF_CHANNEL);

    //TODO Destroy
    //if(plugin.webview) {
    //    free(plugin.webview);
    //}
}

FLUTTERPI_PLUGIN("webview cef plugin", webview_cef_plugin, webview_cef_init, webview_cef_deinit)