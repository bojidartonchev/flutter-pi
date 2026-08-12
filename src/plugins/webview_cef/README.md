# webview_cef

Platform side of the [`webview_cef`](https://pub.dev/packages/webview_cef) pub
package, so a flutter app can embed web pages on flutter-pi with the same dart
code it uses on desktop.

Off by default; enable with `-DBUILD_WEBVIEW_CEF_PLUGIN=ON`.

## How it works

flutter-pi has no platform view support and owns the DRM master, so a browser
cannot be given a window of its own. Instead:

1. CEF renders the page **off-screen** (`windowless_rendering_enabled`) and hands
   flutter-pi a BGRA pixel buffer through `CefRenderHandler::OnPaint`.
2. The plugin uploads that buffer into a GL texture on a context that shares
   flutter's root context, and publishes it through flutter-pi's texture
   registry.
3. The package's dart side shows it with a plain `Texture` widget and forwards
   pointer input over the platform channel, because an off-screen browser gets no
   input of its own.

```
 dart                     flutter-pi (platform thread)                CEF
 ─────                    ─────────────────────────────               ────
 Texture(id)  ◄──── texture_push_frame ◄── on_paint ◄──────── OnPaint (BGRA)
 Listener     ────► cursorClickDown ──────► SendMouseClickEvent ────►
                    flutterpi_post_platform_task_with_time
                             └────► CefDoMessageLoopWork ◄── OnScheduleMessagePumpWork
```

### Threading

CEF is initialized with `external_message_pump`, on flutter-pi's platform
thread. That makes the platform thread CEF's *UI thread*, which means every CEF
callback -- including `OnPaint` -- lands on the platform thread and can touch
flutter-pi state directly. No locks, no marshalling.

The pump itself is driven from flutter-pi's `sd_event` loop:
`OnScheduleMessagePumpWork(delay_ms)` posts a delayed platform task that calls
`CefDoMessageLoopWork()`. That callback is the one thing CEF may invoke from
another thread, so it is the only place with a mutex.

### Processes

Chromium is multi-process, and on Linux it starts subprocesses by re-executing a
binary. If that binary were flutter-pi, every renderer would try to boot a
flutter engine. So the plugin builds a separate `flutter-pi-cef-helper`
executable and points `CefSettings::browser_subprocess_path` at it.

The sandbox is off (`no_sandbox`), which avoids having to ship the setuid
`chrome-sandbox` helper.

## Files

| file | what it is |
| --- | --- |
| `plugin.c` | the flutter-pi side: platform channel, textures, message pump |
| `cef_bridge.h` | plain-C ABI between the two halves |
| `cef_bridge.cpp` | the CEF side: `CefApp`, `CefClient`, off-screen rendering |
| `helper_main.cpp` | `main()` of the subprocess helper |

The split exists because flutter-pi's headers cannot be included from C++ --
`platformchannel.h` and friends use compound literals, which C++ does not have.

## Building

```
cmake -B build -DBUILD_WEBVIEW_CEF_PLUGIN=ON -DCEF_ROOT=/path/to/cef_binary_..._linux64_minimal
```

`CEF_ROOT` must contain `include/cef_version.h`, `libcef.so` and
`libcef_dll_wrapper.a`. Leave it unset to search the (cross) sysroot instead.
`libcef_dll_wrapper` is the static library that translates CEF's C++ API to its
stable C ABI; CEF expects you to build it yourself with your own compiler, which
is why gcc/libstdc++ against a clang/libc++ `libcef.so` is fine.

CEF 126 or newer is required -- that's when `OnBeforePopup` got its `popup_id`
parameter. There is a `#error` guarding it at the top of `cef_bridge.cpp`.
Developed against 132.3.2.

Two more cache variables describe the **target** layout:

| variable | default | meaning |
| --- | --- | --- |
| `WEBVIEW_CEF_RUNTIME_DIR` | `/usr/lib/cef` | where `libcef.so`, `icudtl.dat`, `*.pak`, `*.bin` and `locales/` live |
| `WEBVIEW_CEF_HELPER_PATH` | `/usr/bin/flutter-pi-cef-helper` | the subprocess helper |

## Runtime configuration

The `webview_cef` channel only carries a user agent, so everything else is
configured through the environment. This is also handy while bringing the thing
up, since none of it needs a recompile.

| environment variable | meaning |
| --- | --- |
| `FLUTTERPI_CEF_RUNTIME_DIR` | overrides `WEBVIEW_CEF_RUNTIME_DIR` |
| `FLUTTERPI_CEF_HELPER` | overrides `WEBVIEW_CEF_HELPER_PATH` |
| `FLUTTERPI_CEF_CACHE_PATH` | where Chromium may persist its cache. Unset = in-memory profile |
| `FLUTTERPI_CEF_SWITCHES` | extra Chromium switches, comma separated, without `--` |
| `FLUTTERPI_CEF_NO_DEFAULT_SWITCHES` | if set, don't pass the default switches below |
| `FLUTTERPI_CEF_LOG_FILE` | Chromium's log file (default: stderr) |
| `FLUTTERPI_CEF_LOG_SEVERITY` | 1 verbose, 2 info, 3 warning, 4 error, 5 fatal, 99 off |
| `FLUTTERPI_CEF_FRAME_RATE` | `windowless_frame_rate`, 1..60 (default 30) |
| `FLUTTERPI_CEF_EAGER_INIT` | if set, start CEF during plugin init instead of on the first `init`/`create` |

`FLUTTERPI_CEF_EAGER_INIT` exists because plugins are initialized *before* the
flutter engine is created. Starting CEF there means Chromium forks its helper
processes out of a process that is not heavily threaded yet, and installs its
signal handlers first. It is the safer ordering, and it moves `CefInitialize`'s
half second off the first `init` call -- at the cost of paying Chromium's memory
even if no webview is ever opened.

The switches passed by default:

```
--ozone-platform=headless           there is no X server and no compositor
--disable-gpu                       don't start a GPU process next to flutter-pi
--disable-gpu-compositing
--disable-dev-shm-usage
--autoplay-policy=no-user-gesture-required
```

The first three are the ones that matter. If the page stays blank, or Chromium
dies during startup, this is the list to experiment with:

```bash
FLUTTERPI_CEF_LOG_SEVERITY=1 FLUTTERPI_CEF_SWITCHES=enable-logging=stderr,v=1 flutter-pi --release /path/to/bundle
```

## Implemented channel methods

Channel `webview_cef`, standard method codec. Arguments are positional lists, or
a bare value for the single-argument methods -- that is what the package's dart
side sends.

| method | arguments | returns |
| --- | --- | --- |
| `init` | userAgent (String), or nothing | null |
| `create` | url (String) | `[browserId, textureId]` |
| `close` | browserId | null |
| `loadUrl` | `[browserId, url]` | null |
| `reload` / `goBack` / `goForward` | browserId | null |
| `setSize` | `[browserId, dpi, width, height]` | null |
| `cursorMove` / `cursorDragging` | `[browserId, x, y]` | null |
| `cursorClickDown` / `cursorClickUp` | `[browserId, x, y]` | null |
| `setScrollDelta` | `[browserId, x, y, deltaX, deltaY]` | null |
| `setClientFocus` | `[browserId, focus]` | null |
| `executeJavaScript` | `[browserId, code]` | null |
| `imeCommitText` / `imeSetComposition` | `[browserId, text]` | null |
| `quit` | none | null |

Sizes and coordinates are logical pixels; `dpi` is the device pixel ratio.

Events back to dart, as method calls on the same channel, all carrying
`browserId` in a map: `urlChanged`, `titleChanged`, `onLoadStart`, `onLoadEnd`,
`onTooltip`, `onCursorChanged`, `onConsoleMessage`.

`quit` only closes the browsers, it does not call `CefShutdown` -- CEF cannot be
initialized twice in one process, so shutting down on `quit` would break every
later webview. CEF is torn down in the plugin's deinit instead.

## Not implemented

These respond with "not implemented", so the dart side gets a clear
`MissingPluginException` rather than silence:

- `evaluateJavascript`, `setJavaScriptChannels`, `sendJavaScriptChannelCallBack`
  -- need a `CefRenderProcessHandler` in the helper plus IPC to get values back
  out of V8. `executeJavaScript` (fire and forget) does work.
- `openDevTools` -- needs a real window to put the inspector in.
- `setCookie`, `deleteCookie`, `visitAllCookies`, `visitUrlCookies` --
  straightforward on top of `CefCookieManager`, just not done yet.
- The `onFocusedNodeChangeMessage` and `onImeCompositionRangeChangedMessage`
  events -- `CefRenderProcessHandler::OnFocusedNodeChanged` fires in the render
  process, so reporting it needs process messages between the helper and here.

### Typing into a page

`imeCommitText` and `imeSetComposition` are implemented, so text input works --
but the package's dart side only attaches flutter's text input client after it
receives `onFocusedNodeChangeMessage`, which is in the list above. So a page that
focuses an `<input>` on its own does **not** get a keyboard yet.

An app with its own on-screen keyboard can drive it directly in the meantime:

```dart
controller.imeCommitText('5');
```

Wiring up the focus notification is the missing piece for automatic keyboard
handling, and it's the main thing left to do here.

## Other limitations

- **One GL texture per webview, reused every frame.** If flutter's rasterizer is
  sampling frame N while frame N+1 is uploaded, that frame can tear. Double
  buffering would fix it at the cost of memory.
- **Dirty rects are ignored**; every paint uploads the whole surface. GLES2 has
  no `GL_UNPACK_ROW_LENGTH`, so partial uploads would need a per-row loop.
- **`--disable-gpu`**, so page compositing happens on the CPU. Fine for forms and
  text, not for WebGL or heavy CSS animation.
- **Popups** (`<select>` dropdowns) are composited into the view buffer while
  they are open, which costs one extra full-frame copy per paint. Nothing is
  copied when no popup is open.
- **`target=_blank` and `window.open()`** load into the same view instead of
  opening a second window.
- **Touch input arrives as mouse input**, because that is what the package's dart
  side sends. Multi-touch gestures inside the page are not available.
