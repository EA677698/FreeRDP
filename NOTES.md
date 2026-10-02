# Phase 2 notes: how the SDL3 client's monitor/window/input code connects

Scope: `client/SDL/SDL3/` only (the SDL2 client, `client/SDL/SDL2/`, is a
separate, deprecated codebase — not touched by this project). Base:
FreeRDP 3.32.1 (`bf217a5`). Everything below was read directly from this
tag's source, not assumed from the spec.

## The core problem, stated in code terms

Every place that currently represents "a monitor" is keyed by `SDL_DisplayID`
— a real, SDL-enumerated local display handle:

- `SdlContext::_displays` is `std::map<SDL_DisplayID, rdpMonitor>` — **one
  entry per real local display.** A `std::map` cannot hold two declared
  monitors against the same key, so as written this cannot represent "3
  declared monitors, 1 real display."
- `SdlContext::_windows` is `std::map<SDL_WindowID, SdlWindow>`, and each
  `SdlWindow` stores its originating `_displayID`.
- `SdlContext::_offsets` (`std::map<SDL_DisplayID, pair<SDL_Rect,SDL_Rect>>`)
  — the neighbor-alignment geometry — is keyed the same way.
- `sdl_monitor.cpp`'s whole job is turning **real** SDL display enumeration
  (`SDL_GetDisplays`) into `FreeRDP_MonitorDefArray` entries.

So `/vmonitors` cannot be bolted onto the existing `_displays` map. It needs
a parallel identity for "declared monitor" that is independent of
`SDL_DisplayID`, used only when `/vmonitors` is active. The upstream
multimon path (`/multimon`, `/monitors:`) stays exactly as it is.

## Normal connect-time flow (no `/vmonitors`)

1. `SdlContext::preConnect` (`sdl_context.cpp:138`) calls
   `sdl_detect_monitors()` (`sdl_monitor.cpp:262`).
2. `sdl_detect_monitors` populates `sdl->_monitorIds` from
   `sdl->getDisplayIds()` — real `SDL_GetDisplays()` output — unless
   `/monitors:` gave explicit IDs (then validates them against the real
   list and rejects anything not present). **This is the function the spec
   called out as "sets `FreeRDP_MonitorIds` from
   `SDL_GetNumVideoDisplays()`"** (issue #10433) — confirmed, just the SDL3
   port uses `SDL_GetDisplays()`, not the SDL2-era `SDL_GetNumVideoDisplays`.
3. `sdl_apply_display_properties()` (`sdl_monitor.cpp:186`) converts each
   selected `SDL_DisplayID` into an `rdpMonitor` via `sdl->getDisplay(id)`
   (a lookup into `_displays`, itself populated earlier by
   `detectDisplays()` → `addOrUpdateDisplay()`), then calls
   `freerdp_settings_set_monitor_def_array_sorted()` — this is what actually
   writes `FreeRDP_MonitorDefArray`/`FreeRDP_MonitorCount` (confirms the
   spec's guessed setting names).
4. After connect, `SdlContext::createWindows()` (`sdl_context.cpp:360`)
   iterates `FreeRDP_MonitorCount` entries and for each one calls
   `SdlWindow::create(did, title, flags, w, h)` where `did` is
   `monitorId(x)` — **a real `SDL_DisplayID`, used via
   `SDL_WINDOWPOS_CENTERED_DISPLAY(did)`** to pick the window's initial
   screen position.

## The reusable primitive already in the codebase

`SdlWindow` already has a second `create()` overload, built for RAIL
(RemoteApp) windows, that takes an **explicit `SDL_Rect`** instead of
deriving position from the display ID:

```cpp
// sdl_window.hpp:35
static SdlWindow create(SDL_DisplayID id, const std::string& title,
                         Uint32 flags, const SDL_Rect& rect);
```

Traced into the private constructor (`sdl_window.cpp:29`): it writes
`rect.x`/`rect.y` directly into the SDL window-creation properties
(`SDL_PROP_WINDOW_CREATE_X_NUMBER`/`Y_NUMBER`). The `id` parameter in this
path is **only** used for `SDL_GetDisplayContentScale(id)` (DPI scaling of
the requested size) and for building the initial `rdpMonitor` via
`query(_window, id, true)` — never for positioning.

This means: **the window-creation primitive for `/vmonitors` already
exists.** A `/vmonitors` build of `createWindows()` can call this exact
overload once per declared monitor, passing the parsed spec's rect
directly and the Frame's one real `SDL_DisplayID` for `id` (needed only for
scale lookup) — no new SDL-facing window-creation code needed. Confirmed in
Phase 0: gamescope/VR treats each independently-created SDL window as a
separately movable panel regardless of its initial requested position, so
"centered on the one real display" vs. "at an arbitrary declared x/y" makes
no practical difference for the VR separation behavior — only for where it
starts out before the user moves it.

## Rendering: one shared surface, but NOT one shared GPU upload

`SdlContext::_primary` (one `SDL_Surface`, built in `createPrimary()`,
`sdl_context.cpp:340`) is genuinely shared CPU-side — this part already
matches the spec's "upload once" intent. But each `SdlWindow` has its own
`SDL_Renderer` **and its own `_gdiTexture`**, sized to the *entire* shared
surface (`ensureGdiTexture`, `sdl_window.cpp:531`:
`SDL_CreateTexture(_renderer, surface->format, STREAMING, surface->w, surface->h)`
— not the window's own crop size). `blit()` (`sdl_window.cpp:550`) uploads
only the dirty sub-rect via `SDL_UpdateTexture(&srcRect, ...)`, which is
damage-aware, but it's a **separate GPU upload per window**, not a shared
GPU resource — real work, confirmed, not reused across windows.

Worse for the efficiency requirement: `SdlContext::drawToWindows()`
(`sdl_context.cpp:1644`) loops over **every** window and calls
`drawToWindow()` for **every** damaged rect, with **no rectangle
intersection test against that window's own monitor region**. A damage
rect that only touches monitor 1 still gets uploaded+drawn by windows 2 and
3 too (clipped only by the renderer's own bounds, not skipped). Spec
section 6 ("efficiency") explicitly asks for both of these — shared
GPU upload and damage-rect culling per window — and neither exists today.
This is real new work for Phase 3, not a misunderstanding on the spec's
part.

## Window geometry / coordinate mapping already in place

- `SdlWindow::offsetX()/offsetY()` + `SdlContext::applyMonitorOffset()`
  (`sdl_context.cpp:801`) — already exactly the "window-local → remote
  layout" offset math the spec's requirement 4 asks for, just driven today
  by real display positions.
- `updateDisplayOffsetsForNeighbours()` (`sdl_context.cpp:830`, confirmed to
  really exist, contrary to my initial grep against an older tag — it's
  real, and it **is** the PR #13507 wlroots fix) walks `_offsets` and
  aligns adjacent monitors edge-to-edge, correcting for logical-vs-pixel
  scale mismatches between neighbors. The algorithm (given a set of monitor
  rects, propagate aligned offsets outward from the primary) is reusable
  as-is for `/vmonitors`'s declared layout — it doesn't care *where* the
  rects came from, only that `_offsets` is populated. The blocker is still
  the `SDL_DisplayID` key type, not the alignment math itself.
- `SdlContext::screenToPixel()`/`pixelToScreen()` (`sdl_context.cpp:1428`,
  `1451`) go through `SDL_RenderCoordinatesFromWindow`/`ToWindow` — i.e.
  pointer coordinate mapping is already per-window (keyed by
  `SDL_WindowID`, not display), with an optional `_localScale` correction
  layered on top (`applyLocalScaling`/`removeLocalScaling`) for the
  non-multimon "scale to fit window" mode. This path should need little to
  no change for `/vmonitors` — it's already window-relative.

## Display Control channel (`sdl_disp.cpp`)

Confirmed real, matches spec section 4's description. `sendLayout()`
(`sdl_disp.cpp:188`) sends `DISPLAYCONTROL_MONITOR_LAYOUT_PDU` updates after
connect. `updateMonitors()`/`handleEvent(const SDL_DisplayEvent&)`
(`sdl_disp.cpp:298`, `326`) are what currently trigger a layout resend on
**real** display hotplug — irrelevant to virtual monitors, which never fire
`SDL_EVENT_DISPLAY_*`. For the default (static layout, no
`/dynamic-resolution`), this channel needs no changes. For the optional
dynamic-resolution feature (spec 6.10), `sendLayout`/`sendResize`
(`sdl_disp.cpp:69`) are the functions to call manually when a `/vmonitors`
window is resized.

## Spec corrections from this reading pass

- `updateDisplayOffsetsForNeighbours` **does exist** — my earlier check
  against FreeRDP 3.17.2 (before the fork-base decision) didn't find it
  because that tag predates PR #13507. On 3.32.1 it's real, in
  `sdl_context.cpp`, and is a `SdlContext` method, not a free function.
- `SdlWindow::rect()` exists as described, in `sdl_window.cpp`/`.hpp`,
  confirmed real on 3.32.1.
- No `sdl_disp.*` surprises — matches the spec's "probably sdl_disp.*"
  guess exactly.
- Not yet found anywhere: a function matching "sets `FreeRDP_MonitorIds`
  from `SDL_GetNumVideoDisplays()`" *by that literal name* — the real
  equivalent is `sdl_detect_monitors()` using `SDL_GetDisplays()` (SDL3's
  renamed API), which is the same job, just not the SDL2-era function name
  the spec's source issue (#10433) used.

## What must change for `/vmonitors` (first-pass list, not final)

1. **New data path, not a patch to `_displays`.** Add a
   `/vmonitors`-populated monitor list that bypasses
   `sdl_detect_monitors()`/`addOrUpdateDisplay()` entirely — built straight
   from the parsed spec into `rdpMonitor` structs and
   `freerdp_settings_set_monitor_def_array_sorted()`, the same end call the
   existing path uses. `_displays`/`_offsets` being keyed by `SDL_DisplayID`
   is the crux of why this can't reuse the existing detection path.
2. **`createWindows()`** (`sdl_context.cpp:360`): under `/vmonitors`, call
   the existing rect-based `SdlWindow::create(realDisplayId, title, flags,
   rect)` overload per declared monitor instead of computing a per-monitor
   `SDL_DisplayID`. `realDisplayId` can just be the one real display, used
   only for DPI lookup.
3. **Offset/neighbor math**: either feed `_offsets` with synthetic entries
   so `updateDisplayOffsetsForNeighbours`/`updateMonitorDataFromOffsets`
   keep working unmodified, or compute offsets directly from the parsed
   spec's declared positions (simpler, since `/vmonitors` positions are
   already explicit — no detection/alignment needed, unlike real hardware
   with ambiguous OS-reported positions).
4. **Rendering efficiency gap (section 6)**: add rect-intersection culling
   in `drawToWindows()` before calling `drawToWindow()` per window; the
   shared-GPU-upload goal is a bigger, separate change (would mean
   restructuring away from one `SDL_Renderer`+texture per `SdlWindow`) —
   flag as a larger follow-up, not a quick fix alongside the rest.
5. **Display Control channel**: no change needed for the static-layout
   default. Dynamic-resolution (optional, off by default) would call
   `sendLayout`/`sendResize` manually from a `/vmonitors`-aware resize
   handler.

## Not yet read in depth (deferred, not blocking the plan above)

`sdl_input.cpp` (pointer button plumbing — relevant to the Phase 1 VR
left-click bug, logged separately), `sdl_touch.cpp`, `sdl_rail*.cpp` (RAIL
mode — out of scope, multi-window is for full-desktop mode only), option
parsing in `client/common/cmdline.c` (where `/vmonitors` itself will be
added). These will get a focused read in Phase 3 when the corresponding
code is actually touched, rather than up front.
