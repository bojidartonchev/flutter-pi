# webview_cef

A webview for flutter-pi, backed by the Chromium Embedded Framework.

## How it works

flutter-pi has no platform view support and owns the DRM master, so a browser
cannot be given a window of its own. Instead:

1. CEF renders the page **off-screen** (`windowless_rendering_enabled`) and hands
   flutter-pi a BGRA pixel buffer through `CefRenderHandler::OnPaint`.
2. The plugin uploads that buffer into a GL texture on a context that shares
   flutter's root context, and publishes it through flutter-pi's texture
   registry.
3. Dart shows it with a plain `Texture` widget and forwards pointer/keyboard
   input over the platform channel, because an off-screen browser gets no input
   of its own.

```
 dart                    flutter-pi (platform thread)                CEF
 ─────                   ─────────────────────────────               ────
 Texture(id) ◄───── texture_push_frame ◄── on_paint ◄──────── OnPaint (BGRA)
 Listener   ─────► pointerEvent ─────────► SendMouseClickEvent ────►
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

The sandbox is off (`no_sandbox`), which avoids shipping the setuid
`chrome-sandbox` helper on a kiosk image.

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
`libcef_dll_wrapper.a`. Leave it unset to search the (cross) sysroot, which is
what the `cef` recipe in meta-egt populates.

Two more cache variables describe the **target** layout:

| variable | default | meaning |
| --- | --- | --- |
| `WEBVIEW_CEF_RUNTIME_DIR` | `/usr/lib/cef` | where `libcef.so`, `icudtl.dat`, `*.pak`, `*.bin` and `locales/` live |
| `WEBVIEW_CEF_HELPER_PATH` | `/usr/bin/flutter-pi-cef-helper` | the subprocess helper |

CEF 120 or newer is required; see the version check at the top of
`cef_bridge.cpp`.

## Runtime configuration

Both paths can be overridden without recompiling, which is handy when bringing
the thing up:

| environment variable | overrides |
| --- | --- |
| `FLUTTERPI_CEF_RUNTIME_DIR` | `WEBVIEW_CEF_RUNTIME_DIR` |
| `FLUTTERPI_CEF_HELPER` | `WEBVIEW_CEF_HELPER_PATH` |
| `FLUTTERPI_CEF_SWITCHES` | extra Chromium switches, comma separated, no `--` |
| `FLUTTERPI_CEF_LOG_FILE` | Chromium's log file (default: stderr) |
| `FLUTTERPI_CEF_EAGER_INIT` | if set, start CEF during plugin init instead of on the first `init`/`create` call |

`FLUTTERPI_CEF_EAGER_INIT` exists because plugins are initialized *before* the
flutter engine is created. Starting CEF there means Chromium forks its helper
processes out of a process that is not heavily threaded yet, and installs its
signal handlers first. It is the safer ordering, at the cost of paying
Chromium's memory and startup even if no webview is ever opened -- and of only
being configurable through the environment, since dart hasn't run yet.

The switches the plugin passes by default:

```
--ozone-platform=headless           there is no X server and no compositor
--disable-gpu                       don't start a GPU process next to flutter-pi
--disable-gpu-compositing
--disable-dev-shm-usage
--autoplay-policy=no-user-gesture-required
```

The first three are the ones that matter. If the page renders but stays blank,
or Chromium dies during startup, this is the list to experiment with, e.g.:

```bash
FLUTTERPI_CEF_SWITCHES=enable-logging=stderr,v=1 flutter-pi --release /flutter/kiosk/*/release
```

## Platform channel

Channel `egt/webview`, standard method codec. Arguments are always a map.
Sizes and pointer coordinates are in **logical** pixels; the plugin scales them
by `pixelRatio` internally.

### dart → flutter-pi

| method | arguments | returns |
| --- | --- | --- |
| `init` | `cachePath?`, `userAgent?`, `switches?`, `useDefaultSwitches?`, `logSeverity?`, `runtimeDir?`, `helperPath?`, `logFile?` | null |
| `create` | `url`, `width`, `height`, `pixelRatio?`, `frameRate?` | `int` texture id |
| `dispose` | `textureId` | null |
| `loadUrl` | `textureId`, `url` | null |
| `setSize` | `textureId`, `width`, `height`, `pixelRatio?` | null |
| `reload` | `textureId`, `ignoreCache?` | null |
| `stopLoad` / `goBack` / `goForward` / `invalidate` | `textureId` | null |
| `setFocus` | `textureId`, `focused` | null |
| `runJavaScript` | `textureId`, `code` | null |
| `pointerEvent` | `textureId`, `kind` (`mouse`/`touch`), `phase` (`down`/`move`/`up`/`cancel`), `x`, `y`, `pointer?`, `button?`, `clickCount?`, `modifiers?` | null |
| `scroll` | `textureId`, `x`, `y`, `deltaX`, `deltaY`, `modifiers?` | null |
| `keyEvent` | `textureId`, `phase` (`rawDown`/`down`/`up`/`char`), `keyCode?`, `nativeKeyCode?`, `character?`, `modifiers?` | null |

`init` is optional -- `create` initializes CEF if it hasn't happened yet -- but
it is the only way to pass process-wide options, and calling it early moves
Chromium's startup cost out of the first `create`.

`modifiers` is a `cef_event_flags_t` bitmask (shift `1<<1`, ctrl `1<<2`,
alt `1<<3`, left mouse button `1<<4`).

### flutter-pi → dart

Sent as method calls on the same channel; every one carries `textureId`.

| method | arguments |
| --- | --- |
| `onLoadingStateChanged` | `isLoading`, `canGoBack`, `canGoForward` |
| `onUrlChanged` | `url` |
| `onTitleChanged` | `title` |
| `onLoadError` | `errorCode`, `errorText`, `failedUrl` |

## Known limitations

- **No JS result values.** `runJavaScript` is fire-and-forget. Reading a value
  back needs a `CefRenderProcessHandler` in the helper plus IPC.
- **One GL texture per webview, reused every frame.** If flutter's rasterizer is
  sampling frame N while frame N+1 is uploaded, that frame can tear. Double
  buffering would fix it at the cost of VRAM.
- **Dirty rects are ignored**; every paint uploads the whole surface. GLES2 has
  no `GL_UNPACK_ROW_LENGTH`, so partial uploads would need a per-row loop.
- **`--disable-gpu`**, so page compositing is on the CPU. Fine for forms and
  text, not for WebGL or heavy CSS animation.
- **Popups** (`<select>` dropdowns) are composited on the CPU into the view
  buffer while they are open, which costs one extra full-frame copy per paint.
  Nothing is copied when no popup is open.
- **`target=_blank` and `window.open()`** load into the same view instead of
  opening a second window.
