/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client, user-declared virtual monitor layout (/vmonitors, /vmonitor-scale)
 *
 * Copyright 2026 Erick
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <freerdp/settings_types.h>

namespace sdl::vmonitor
{
/** Maximum number of monitors a single RDP session can declare (MS-RDPBCGR
 *  TS_UD_CS_MONITOR caps this at 16 MONITOR_DEF entries). Matches the
 *  existing /monitors: option's cap in client/common/cmdline.c. */
constexpr size_t kMaxMonitors = 16;

/**
 * Parse a /vmonitors argument (and optional parallel /vmonitor-scale
 * argument) into a list of rdpMonitor entries, ready for
 * freerdp_settings_set_monitor_def_array_sorted().
 *
 * /vmonitors syntax: <spec>[,<spec>...], each spec is WxH[+X+Y][*]:
 *   - WxH: positive width/height, required.
 *   - +X+Y or +X-Y or -X+Y or -X-Y: optional explicit position, X11
 *     geometry style (explicit sign required on both axes together).
 *     If omitted, the monitor is placed left-to-right at y=0, continuing
 *     from wherever the previous monitor (explicit or defaulted) ended.
 *   - trailing '*': marks this monitor primary. At most one. If none of
 *     the specs are marked, the first one becomes primary.
 *
 * /vmonitor-scale syntax: <percent>[,<percent>...], either exactly one
 * value (applied to every monitor) or exactly one value per monitor
 * (applied positionally). Each value must be in [25, 500].
 *
 * Pure parsing/layout logic: no SDL, no settings, no I/O. This is what
 * makes it unit-testable without a display.
 *
 * Returns true and fills `monitors` on success. Returns false and fills
 * `error` with a human-readable reason on failure; `monitors` is left
 * empty.
 */
[[nodiscard]] bool parse(const char* vmonitorsArg, const char* vmonitorScaleArg,
                         std::vector<rdpMonitor>& monitors, std::string& error);

} // namespace sdl::vmonitor
