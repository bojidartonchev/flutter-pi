#define _GNU_SOURCE

#include "plugins/webview_cef.h"
#include "flutter-pi.h"
#include "pluginregistry.h"
#include "util/logging.h"
#include "capi/cef_base_capi.h"
#include "capi/cef_app_capi.h"
#include "capi/cef_client_capi.h"

size_t u16strlen(const char16_t* str) {
    size_t len = 0;
    while (str[len] != 0) {  // Count until null terminator
        len++;
    }
    return len;
}

static int on_init(struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    struct std_value *args;
    char *userAgent;

    args = &object->std_arg;

    if (args != NULL && STDVALUE_IS_STRING(*args)) {
        userAgent = STDVALUE_AS_STRING(*args);
    }

    if(userAgent) {
        LOG_ERROR("Imame userAgent: %s\n", userAgent);
    }

    int ok = platch_respond_success_std(response_handle, &STDNULL);

    return ok;
}

static int on_create(struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    struct std_value *args;
    char *url;

    args = &object->std_arg;

    if (args == NULL || !STDVALUE_IS_STRING(*args)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `url` to be a string.");
    }

    url = STDVALUE_AS_STRING(*args);

    LOG_ERROR("Imame url: %s\n", url);

    //char url[] = "https://www.google.com";
    cef_string_t cef_url = {};
    cef_string_utf8_to_utf16(url, strlen(url), &cef_url);
    
    // Browser settings. It is mandatory to set the
    // "size" member.
    cef_browser_settings_t browser_settings = {};
    browser_settings.size = sizeof(cef_browser_settings_t);
    
    // Client handler and its callbacks
    cef_client_t client = {};
    initialize_cef_client(&client);

    // Create browser asynchronously. There is also a
    // synchronous version of this function available.
    cef_browser_host_create_browser(&window_info, &client, &cef_url,
                                    &browser_settings, NULL, NULL);

    /*cef_window_info_t window_info = {};

    cef_browser_settings_t browser_settings = {};
    cef_string_t url2;
    const char16_t* url3 = u"https://www.google.com";
    cef_string_utf16_set(url3, u16strlen(url3), &url2, 1);  // Set the cef_string_t with the URL

    cef_browser_host_create_browser(&window_info, NULL, &url2, &browser_settings, NULL, NULL);*/

    struct std_value response;

    response.type = kStdList;
    response.size = 2;
    response.list = alloca(sizeof(struct std_value) * response.size);

    response.list[0].type = kStdInt32;
    response.list[0].int32_value = 1;

    response.list[1].type = kStdInt32;
    response.list[1].int32_value = 1;

    int ok = platch_respond_success_std(response_handle, &response);

    return ok;
}

static int on_set_client_focus(struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    (void) object;

    int ok = platch_respond_success_std(response_handle, &STDNULL);

    return ok;
}

static int on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    (void) channel;

    const char *method;
    method = object->method;

    if (streq(method, "init")) {
        return on_init(object, response_handle);
    } else if (streq(method, "create")) {
        return on_create(object, response_handle);
    } else if (streq(method, "setClientFocus")) {
        return on_set_client_focus(object, response_handle);
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