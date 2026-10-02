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

`sdl_rail*.cpp` (RAIL mode — out of scope, multi-window is for full-desktop
mode only), option parsing in `client/common/cmdline.c` beyond what
`/vmonitors`/`/vmonitor-scale` needed. `sdl_input.cpp`/`sdl_touch.cpp` have
since been read in full — see below.

## Phase 3 increment 1 build/live-test results (2026-10-02)

`/vmonitors`/`/vmonitor-scale` implemented (`sdl_vmonitor.cpp/hpp`, wired
into `preConnect`/`createWindows`/`postConnect` in `sdl_context.cpp`),
plus damage-rect culling in `drawToWindows()`. Confirmed live on the
Frame: 3 windows spawn correctly, each showing the right region, correct
bounding-box desktop size. Two issues found during live testing, both
investigated to a root cause — neither is a bug in this fork's code:

### Input lag / backlog with 3 windows

Severe (multi-second) input lag was observed in the first live 3-window
test, with a 300+ event backlog dumping at once on window close. Traced
with two tools: (1) per-event TRACE logging already in `sdl_run()`
(`sdl_freerdp.cpp`) showed no single processing gap over ~1.2s anywhere,
ruling out our own event loop stalling; (2) comparing against the
Phase 0 spike (trivial render cost) under the exact same conditions
showed the spike handled 3 windows smoothly while the real client did
not, implicating render/update cost specifically, not window count
alone. Added `event.common.timestamp`-based staleness logging (now
permanent, see `sdl_freerdp.cpp`) to measure how long events actually sat
queued inside SDL rather than how long our own loop took between
iterations — this is the more precise signal; the simple "gap between
log lines" measurement can look fine even while a real backlog is
building invisibly in SDL's queue.

A later repeat of the same 3-window test, after the binary had already
been run several times (warm Vulkan shader/pipeline caches, confirmed by
"Gamescope WSI" shader-compile chatter in the first capture but not
later ones), showed near-real-time staleness (~0.5ms average). At the
time this looked like a cold-shader-cache explanation. **That theory was
wrong** — Erick re-ran the same prior binary twice more with no rebuild
in between, and the severe lag was still there both times, ruling out
"first run after rebuild" as the explanation. A subsequent capture with
the same binary showed the real shape of the problem clearly: per-event
staleness climbing from ~4s (first third of a 96s session) to ~11.6s
average / 15.4s peak (last third) — a monotonically growing backlog for
as long as the session ran, not noise.

Root cause found by reading `sdl_freerdp.cpp`'s `SDL_EVENT_USER_UPDATE`
handler: it was draining the rect queue one batch at a time, doing a
full `drawToWindows()` (texture upload + present, per relevant window)
for *every single queued batch separately* via a `do { pop(); draw(); }
while (!empty)` loop. The server pushed ~1168 separate `endPaint()`
notifications over that 96s session (~12/sec) — faster than several
windows can be rendered and presented under gamescope, apparently, so
one expensive draw cycle per queued batch instead of merging them falls
further behind the longer the session runs. Fixed by merging every
currently-queued batch's rects and drawing once per event-loop pass
instead of once per batch (`sdl_freerdp.cpp`) — strictly more correct
too, not just faster, since the surface drawn from always reflects the
latest state regardless of how many batches arrived; the old code could
burn a full render+present cycle on a stale intermediate frame that a
newer already-queued batch immediately superseded anyway.

**Re-tested: the coalescing fix did not help.** Staleness still grew
monotonically (~10.9s avg mid-session, ~20.6s avg / 24.2s peak late
session, over a 112s run) — if anything slightly worse than before,
within session-to-session noise. Root cause: `SDL_EVENT_USER_UPDATE`
counts are nearly identical before/after the fix (1168 vs 1263 over
similar-length sessions) — there wasn't a backlog of redundant queued
draws to coalesce in the first place. Each update was already being
handled close to one at a time; the bottleneck is the cost of each
individual draw+present cycle itself under gamescope, not wasted
duplicate work. Added direct timing instrumentation around
`drawRects()` (texture upload) and `updateSurface()` (the actual
`SDL_RenderPresent`) separately in `drawToWindow()` (`sdl_context.cpp`)
to measure which half is actually expensive, rather than guessing.
**Result: rendering is not the bottleneck either.** Captured 423 draws
across a full session: total upload+present time summed to 1.6 seconds
(avg 3.8ms/draw, p99 30ms, worst single draw 51ms). Nowhere close to
explaining 20+ seconds of staleness. This rules out everything within
this client's control that was plausible: the event loop isn't
stalling (checked earlier via inter-event gaps), and now render/present
cost isn't either (checked directly via per-call timing).

**Conclusion: the delay is upstream of this process, in gamescope's own
event delivery, not fixable from this fork.** Each event's "staleness"
is measured against a timestamp SDL assigns when it receives the event
from the compositor, not when this code gets around to handling it --
if that timestamp is already old on arrival, the delay happened before
any of this client's code ever saw the event. Matches the two open
upstream gamescope issues above (#437, #1709) exactly: multi-window-
per-process handling genuinely degraded in gamescope itself, worsening
over a sustained session. Not pursuing further client-side
instrumentation for this -- the remaining options are living with it,
raising it upstream with gamescope, or a much larger architectural
pivot (e.g. separate OS processes per window, untested, unproven
against this specific cause) if it blocks real use.

One confirmed, real, and separate contributing factor either way: two
open upstream gamescope issues describe exactly this class of problem —
[#437](https://github.com/ValveSoftware/gamescope/issues/437) ("gets
confused and rapidly switching between windows" when one process has
several, open since 2022, updated as recently as July 2026) and
[#1709](https://github.com/ValveSoftware/gamescope/issues/1709)
(multi-window app freeze/stall specifically on window close, open). If
severe lag does recur, it's gamescope's own multi-window handling, not
something fixable from this fork's code.

`SDL_EVENT_MOUSE_WHEEL`'s `event.common.timestamp` comes from a different
clock than other event types on this platform/driver (produces nonsense
multi-second "staleness" if compared naively against `SDL_GetTicks()`) —
exclude wheel events from any future staleness analysis.

### Left-click does nothing (confirmed root cause, not fixable here)

Read `sdl_touch.cpp`'s `SdlTouch::handleEvent(SdlContext*, const
SDL_MouseButtonEvent&)` (the actual SDL-button-to-RDP-PTR_FLAGS mapping)
and `SdlContext::handleEvent(const SDL_MouseButtonEvent&)` (the window
lookup/coordinate-conversion path that calls it) in full. Both are
correct and symmetric — button 1 (left) and button 3 (right) go through
identical code paths with no special-casing that would explain one
working and the other not.

Added temporary logging at every point either function could silently
drop an event (window lookup miss, coordinate conversion failure) plus
the raw SDL button number, and had Erick reproduce on real hardware.
Result: **SDL button value 1 (left) never appears in the event stream at
all**, in any capture, ever — only button 2 (middle) and button 3
(right), both processed and forwarded successfully 100% of the time with
zero drops. The bug is not in this fork's code, not in event delivery
timing, and not in the button-to-PTR_FLAGS mapping — SDL is simply never
handed a left-button event by the compositor in the first place.

Likely explanation, matching documented SteamVR dashboard/overlay
behavior: the controller trigger's primary function in the VR dashboard
is grabbing and moving the panel itself (a compositor-level gesture),
which would consume the press before any left-click event reaches the
app. This is a VR input-model question outside this fork's reach, not a
client bug — parking it as a known limitation rather than continuing to
chase it in client code. If Erick finds a controller gesture or SteamVR
binding that produces a real button-1 event, revisit; otherwise this
isn't fixable from here.

## Flagged, unresolved: input coordinate mapping for negative-origin layouts

`createWindows()` computes each window's `offsetX/Y` as `originX -
monitor->x` (`originX` = min x across all declared monitors, 0 if none
negative). `applyMonitorOffset()` (called from the mouse/touch event
handlers) then computes `remote_coord = local_pixel - offsetX`. For the
common case (all monitors at x >= 0, `originX == 0`), this reduces to
`remote_coord = local_pixel + monitor->x`, which is correct by hand
calculation and matches every layout actually tested live so far
(including `/vmonitors` runs — none used a negative position).

Hand-deriving the case where some monitor has a negative declared x
(`originX < 0`, e.g. a monitor placed left of or above the primary) gives
what looks like wrong remote coordinates for *every* window, not just the
negative one. Did not chase this further into the real-multimon-only
`updateDisplayOffsetsForNeighbours`/`_offsets` neighbor-alignment path
(separate machinery that also writes `monitor->x/y` before `createWindows`
runs, only for real multimon, not `/vmonitors`) to see whether it changes
the picture — that's a deeper read than was worth doing speculatively.

This is **pre-existing, shared code, not something `/vmonitors` changes**
— so not a regression, but it directly matters here since negative
positions are an explicitly intended `/vmonitors` use case (spec section
6.1's own examples, and the required test list includes "negative
positions"). Needs an empirical check, not more paper derivation:

**Test to run:** `/vmonitors:1920x1080,1920x1080-1920+0`, then click near
the left edge and right edge of *each* window and confirm the remote
cursor lands in the corresponding place on the actual Windows desktop
(e.g. drag a window to each clicked spot, or watch the OS cursor
position). If clicks land offset by a consistent amount, this formula is
the place to fix it.

## Session paused here (2026-10-02) — jumped to Phase 5 packaging

Erick's explicit call: stop Phase 3 here (input lag unresolved, not
fixable client-side per above; left-click same), skip Phase 4 for now,
jump to Phase 5 (packaging) early. Resume Phase 3 at "aspect-ratio
letterbox resize" (spec functional requirement 3) if picked back up —
everything before that in Phase 3's checklist is done. See the
project-root `README.md` for the full current status summary; this file
stays the detailed engineering log.

### Phase 5: portable package

Built a relocatable, standalone package at `~/sdl-freerdp-portable` on
the Frame (sibling to `~/rdp-client`, not nested in it) — just
`bin/sdl-freerdp` plus the 5 shared libraries it needs beyond what's
already on the system (FreeRDP's own `libfreerdp3`/`libfreerdp-client3`/
`libwinpr3`, plus `libSDL3`/`libSDL3_ttf` since those are this project's
own local build, not a system package). ~12MB total.

Used `cmake --install <build> --prefix <dir>` rather than hand-bundling
libraries — FreeRDP's own build already sets `$ORIGIN`-relative RPATH on
install (`cmake/ConfigureRPATH.cmake`'s `installWithRPATH()`, used
throughout the tree), so an installed binary resolves its libraries
relative to its own location automatically. The only two libraries
`ldd` reported as `not found` after a plain install were `libSDL3.so.0`/
`libSDL3_ttf.so.0` (copied in from `~/local/sdl3/lib`); everything else
`cmake --install` or the binary needed was either already bundled
correctly via RPATH or a legitimate system library already present.

`cmake --install` pulls in far more than this package needs by default
(xfreerdp/wlfreerdp/sfreerdp/winpr CLI tools, dev headers, cmake/
pkgconfig files) — all deleted after confirming via `ldd ... | grep
"not found"` that removing them didn't break anything `sdl-freerdp`
itself needs. Exact prune list and full rebuild steps are in
`packaging/README.md` (local copy) / the matching README shipped
alongside the binary on the Frame.

Verified non-visually end to end after trimming: launches, logs the
correct bounding-box desktop size for the given `/vmonitors` layout,
reaches the network layer. Added to Erick's Steam Library on the Frame
as a non-Steam shortcut pointing at `run-vmonitors.sh`, with connection
args (`/v:`, `/u:`, `/vmonitors:`) in Steam's own Launch Options field
rather than hardcoded in the script — so the same package works for any
host/layout without editing.
