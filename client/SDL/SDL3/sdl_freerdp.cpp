/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * FreeRDP SDL UI
 *
 * Copyright 2022 Armin Novak <armin.novak@thincast.com>
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

#include <iostream>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>

#include <freerdp/config.h>

#if defined(__has_include)
#if __has_include(<version>)
#include <version>
#endif
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(__cpp_lib_source_location) && (__cpp_lib_source_location >= 201907L)
#include <source_location>
#endif

#include <freerdp/constants.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/streamdump.h>
#include <freerdp/utils/signal.h>

#include <freerdp/channels/channels.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/file.h>

#include <freerdp/log.h>
#include <winpr/assert.h>
#include <winpr/config.h>
#include <winpr/crt.h>
#include <winpr/synch.h>

#include <SDL3/SDL.h>
#if !defined(__MINGW32__)
#include <SDL3/SDL_main.h>
#endif
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_oldnames.h>
#include <SDL3/SDL_video.h>

#include <sdl_config.hpp>

#include "dialogs/sdl_connection_dialog_hider.hpp"
#include "dialogs/sdl_dialogs.hpp"
#include "scoped_guard.hpp"
#include "sdl_channels.hpp"
#include "sdl_freerdp.hpp"
#include "sdl_context.hpp"
#include "sdl_monitor.hpp"
#include "sdl_pointer.hpp"
#include "sdl_prefs.hpp"
#if defined(_WIN32)
#include "sdl_win32_console.hpp"
#endif
#include "sdl_utils.hpp"

#if defined(_WIN32)
#define SDL_TAG CLIENT_TAG("SDL")
#endif

class ErrorMsg : public std::exception
{
  public:
	ErrorMsg(int rc, Uint32 type, const std::string& msg,
	         const std::string& sdlError = SDL_GetError()
#if defined(__cpp_lib_source_location) && (__cpp_lib_source_location >= 201907L)
	             ,
	         std::source_location loc = std::source_location::current()
#elif defined(__GNUC__)
	             ,
	         const std::string& file = __builtin_FILE(),
	         const std::string& fkt = __builtin_FUNCTION(), size_t line = __builtin_LINE(),
#if defined(__clang__)
	         size_t column = __builtin_COLUMN()
#else
	         size_t column = 0
#endif
#endif
	);

	[[nodiscard]] int rc() const;
	[[nodiscard]] const char* what() const noexcept override;

  private:
	int _rc;
	Uint32 _type;
	std::string _msg;
	std::string _sdlError;
	std::string _buffer;

	std::string _file;
	std::string _fkt;
	size_t _line = 0;
	size_t _column = 0;
};
const char* ErrorMsg::what() const noexcept
{
	return _buffer.c_str();
}

int ErrorMsg::rc() const
{
	return _rc;
}

ErrorMsg::ErrorMsg(int rc, Uint32 type, const std::string& msg, const std::string& sdlError
#if defined(__cpp_lib_source_location) && (__cpp_lib_source_location >= 201907L)
                   ,
                   std::source_location loc
#elif defined(__GNUC__)
                   ,
                   const std::string& file, const std::string& fkt, size_t line, size_t column
#endif
                   )
    : _rc(rc), _type(type), _msg(msg), _sdlError(sdlError)
#if defined(__cpp_lib_source_location) && (__cpp_lib_source_location >= 201907L)
      ,
      _file(loc.file_name()), _fkt(loc.function_name()), _line(loc.line()), _column(loc.column())
#elif defined(__GNUC__)
      ,
      _file(file), _fkt(fkt), _line(line), _column(column)
#endif
{
	std::stringstream ss;
	ss << _msg << " {" << sdl::utils::toString(_type) << "}{SDL:" << sdlError << "}";
	ss << "{" << _fkt << " [" << _file << ":" << _line << "-" << _column << "]}";
	_buffer = ss.str();
}

static void sdl_term_handler([[maybe_unused]] int signum, [[maybe_unused]] const char* signame,
                             [[maybe_unused]] void* context)
{
	std::ignore = sdl_push_quit();
}

/* [loop-stats] diagnostic: every 5s, logs main-loop handler cost per event type and the
 * worst queue staleness. Off unless SDL_FREERDP_LOOP_STATS is set. This is how the
 * multi-window input lag was measured (see NOTES.md). */
namespace
{
	class LoopStats
	{
	  public:
		explicit LoopStats(wLog* log)
		    : _log(log), _enabled(SDL_getenv("SDL_FREERDP_LOOP_STATS") != nullptr),
		      _start(SDL_GetTicksNS()), _lastReport(_start)
		{
		}

		void add(Uint32 type, Uint64 ns, Sint64 staleMs)
		{
			if (!_enabled)
				return;
			auto& e = _byType[type];
			e.count++;
			e.totalNs += ns;
			e.maxNs = std::max(e.maxNs, ns);
			if ((type != SDL_EVENT_MOUSE_WHEEL) && (staleMs >= 0) && (staleMs < 1000000))
				_maxStaleMs = std::max(_maxStaleMs, staleMs);
		}

		/* Round trip for a discrete input (key, button, finger down): time until the next
		 * screen update is posted by the RDP thread (network + server + decode), and until
		 * that update has been drawn and presented. User events are stamped when pushed. */
		/* Damaged area of the update just drawn: caret blinks and other small updates
		 * shouldn't count as the response to an input. */
		void setUpdateArea(const std::vector<SDL_Rect>& rects)
		{
			_updateArea = 0;
			for (const auto& r : rects)
				_updateArea = std::max<Sint64>(_updateArea, static_cast<Sint64>(r.w) * r.h);
		}

		void trackLatency(const SDL_Event& ev, Uint64 handledNs)
		{
			if (!_enabled)
				return;
			switch (ev.type)
			{
				case SDL_EVENT_KEY_DOWN:
				case SDL_EVENT_MOUSE_BUTTON_DOWN:
				case SDL_EVENT_FINGER_DOWN:
					if (_pendingInputNs == 0)
						_pendingInputNs = ev.common.timestamp;
					break;
				case SDL_EVENT_USER_UPDATE:
					if ((_pendingInputNs != 0) && (ev.common.timestamp > _pendingInputNs) &&
					    (_updateArea >= 200 * 200))
					{
						WLog_Print(_log, WLOG_INFO,
						           "[latency] input->update=%.1fms input->presented=%.1fms",
						           (ev.common.timestamp - _pendingInputNs) / 1e6,
						           (handledNs - _pendingInputNs) / 1e6);
						_pendingInputNs = 0;
					}
					break;
				default:
					break;
			}
		}

		void addWait(Uint64 ns)
		{
			if (_enabled)
				_waitNs += ns;
		}

		void maybeReport()
		{
			if (!_enabled)
				return;
			const auto now = SDL_GetTicksNS();
			if (now - _lastReport < SDL_NS_PER_SECOND * 5)
				return;
			Uint64 busy = 0;
			for (const auto& [type, e] : _byType)
			{
				busy += e.totalNs;
				WLog_Print(_log, WLOG_INFO,
				           "[loop-stats] %-36s n=%6" PRIu64 " total=%7.1fms avg=%6.2fms max=%6.1fms",
				           sdl::utils::toString(type).c_str(), e.count, e.totalNs / 1e6,
				           e.totalNs / 1e6 / static_cast<double>(e.count), e.maxNs / 1e6);
			}
			WLog_Print(_log, WLOG_INFO,
			           "[loop-stats] window=%.1fs busy=%.1fms wait/pump=%.1fms max_staleness=%" PRId64
			           "ms (t=%.0fs)",
			           (now - _lastReport) / 1e9, busy / 1e6, _waitNs / 1e6, _maxStaleMs,
			           (now - _start) / 1e9);
			_byType.clear();
			_waitNs = 0;
			_maxStaleMs = 0;
			_lastReport = now;
		}

	  private:
		struct Entry
		{
			Uint64 count = 0;
			Uint64 totalNs = 0;
			Uint64 maxNs = 0;
		};
		wLog* _log;
		bool _enabled;
		Uint64 _start;
		Uint64 _lastReport;
		Uint64 _waitNs = 0;
		Sint64 _maxStaleMs = 0;
		Uint64 _pendingInputNs = 0;
		Sint64 _updateArea = 0;
		std::map<Uint32, Entry> _byType;
	};
} // namespace

/* [debug-script] temporary: if $SDL_FREERDP_DEBUG_SCRIPT names a file that exists, run its
 * lines ("down X Y", "move X Y", "up X Y", "sleep MS", "snap PATH") as raw RDP left-button
 * input in remote desktop coordinates, then delete it. */
static void sdl_run_debug_script(SdlContext* sdl)
{
	static const char* path = SDL_getenv("SDL_FREERDP_DEBUG_SCRIPT");
	if (!path)
		return;
	if ((sdl->_dbgSnapAt != 0) && (SDL_GetTicks() >= sdl->_dbgSnapAt))
	{
		sdl->_dbgSnapAt = 0;
		const auto snap = std::string(path) + ".release" + std::to_string(++sdl->_dbgSnapCount) + ".bmp";
		WLog_Print(sdl->getWLog(), WLOG_INFO, "[drag-debug] snapshot %s -> %d", snap.c_str(),
		           sdl->debugSaveSnapshot(snap.c_str()));
	}
	FILE* fp = fopen(path, "r");
	if (!fp)
		return;
	char line[512] = {};
	while (fgets(line, sizeof(line), fp))
	{
		char cmd[32] = {};
		char arg[400] = {};
		int x = 0;
		int y = 0;
		if (sscanf(line, "%31s", cmd) != 1)
			continue;
		BOOL rc = TRUE;
		if ((strcmp(cmd, "down") == 0) && (sscanf(line, "%*s %d %d", &x, &y) == 2))
		{
			rc = freerdp_client_send_button_event(sdl->common(), FALSE, PTR_FLAGS_MOVE, x, y) &&
			     freerdp_client_send_button_event(sdl->common(), FALSE,
			                                      PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON1, x, y);
		}
		else if ((strcmp(cmd, "move") == 0) && (sscanf(line, "%*s %d %d", &x, &y) == 2))
			rc = freerdp_client_send_button_event(sdl->common(), FALSE, PTR_FLAGS_MOVE, x, y);
		else if ((strcmp(cmd, "up") == 0) && (sscanf(line, "%*s %d %d", &x, &y) == 2))
		{
			rc = freerdp_client_send_button_event(sdl->common(), FALSE, PTR_FLAGS_MOVE, x, y) &&
			     freerdp_client_send_button_event(sdl->common(), FALSE, PTR_FLAGS_BUTTON1, x, y);
		}
		else if ((strcmp(cmd, "sleep") == 0) && (sscanf(line, "%*s %d", &x) == 1))
			SDL_Delay(static_cast<Uint32>(x));
		else if ((strcmp(cmd, "snap") == 0) && (sscanf(line, "%*s %399s", arg) == 1))
			rc = sdl->debugSaveSnapshot(arg);
		WLog_Print(sdl->getWLog(), WLOG_INFO, "[debug-script] %s -> %d", cmd, rc);
	}
	fclose(fp);
	std::ignore = remove(path);
}

[[nodiscard]] static int sdl_run(SdlContext* sdl)
{
	int rc = -1;
	WINPR_ASSERT(sdl);
	LoopStats stats(sdl->getWLog());

	try
	{
		while (!sdl->shallAbort())
		{
			sdl_run_debug_script(sdl);
			SDL_Event windowEvent = {};
			Uint64 waitStart = SDL_GetTicksNS();
			while (!sdl->shallAbort() && SDL_WaitEventTimeout(nullptr, 1000))
			{
				sdl_run_debug_script(sdl);
				const Uint64 handleStart = SDL_GetTicksNS();
				stats.addWait(handleStart - waitStart);
				struct StatsGuard
				{
					LoopStats& s;
					const SDL_Event& ev;
					Uint64 begin;
					Uint64& waitStart;
					~StatsGuard()
					{
						waitStart = SDL_GetTicksNS();
						const Sint64 stale = static_cast<Sint64>(SDL_GetTicks()) -
						                     static_cast<Sint64>(ev.common.timestamp / 1000000ULL);
						s.add(ev.type, waitStart - begin, stale);
						s.trackLatency(ev, waitStart);
						s.maybeReport();
					}
				} statsGuard{ stats, windowEvent, handleStart, waitStart };

				/* Only poll standard SDL events and SDL_EVENT_USERS meant to create
				 * dialogs. do not process the dialog return value events here.
				 */
				const int prc = SDL_PeepEvents(&windowEvent, 1, SDL_GETEVENT, SDL_EVENT_FIRST,
				                               SDL_EVENT_USER_RETRY_DIALOG);
				if (prc < 0)
				{
					if (sdl_log_error(prc, sdl->getWLog(), "SDL_PeepEvents"))
						continue;
				}

				/* windowEvent.common.timestamp is nanoseconds from SDL_GetTicksNS(), the
				 * clock SDL stamps events with at generation time. Comparing it to "now"
				 * shows how long this event sat queued inside SDL before we dequeued it --
				 * not the same thing as the gap between consecutive log lines, which only
				 * shows how long we took to loop back around, not how deep a backlog grew
				 * inside SDL's queue while we were busy. */
				{
					const Sint64 nowMs = static_cast<Sint64>(SDL_GetTicks());
					const Sint64 eventMs =
					    static_cast<Sint64>(windowEvent.common.timestamp / 1000000ULL);
					WLog_Print(sdl->getWLog(), WLOG_TRACE,
					           "got event %s [0x%08" PRIx32 "] staleness=%" PRId64 "ms",
					           sdl::utils::toString(windowEvent.type).c_str(), windowEvent.type,
					           nowMs - eventMs);
				}
				if (sdl->shallAbort(true))
					continue;

				if (sdl->getDialog().handleEvent(windowEvent))
					continue;

				if (!sdl->handleEvent(windowEvent))
					throw ErrorMsg{ -1, windowEvent.type, "sdl->handleEvent" };

				switch (windowEvent.type)
				{
					case SDL_EVENT_QUIT:
						std::ignore = freerdp_abort_connect_context(sdl->context());
						break;
					case SDL_EVENT_USER_CERT_DIALOG:
					{
						SDLConnectionDialogHider hider(sdl);
						auto title = static_cast<const char*>(windowEvent.user.data1);
						auto msg = static_cast<const char*>(windowEvent.user.data2);
						if (!sdl_cert_dialog_show(title, msg))
							throw ErrorMsg{ -1, windowEvent.type, "sdl_cert_dialog_show" };
					}
					break;
					case SDL_EVENT_USER_SHOW_DIALOG:
					{
						SDLConnectionDialogHider hider(sdl);
						auto title = static_cast<const char*>(windowEvent.user.data1);
						auto msg = static_cast<const char*>(windowEvent.user.data2);
						if (!sdl_message_dialog_show(title, msg, windowEvent.user.code))
							throw ErrorMsg{ -1, windowEvent.type, "sdl_message_dialog_show" };
					}
					break;
					case SDL_EVENT_USER_SCARD_DIALOG:
					{
						SDLConnectionDialogHider hider(sdl);
						auto title = static_cast<const char*>(windowEvent.user.data1);
						auto msg = static_cast<const char**>(windowEvent.user.data2);
						if (!sdl_scard_dialog_show(title, windowEvent.user.code, msg))
							throw ErrorMsg{ -1, windowEvent.type, "sdl_scard_dialog_show" };
					}
					break;
					case SDL_EVENT_USER_AUTH_DIALOG:
					{
						SDLConnectionDialogHider hider(sdl);
						if (!sdl_auth_dialog_show(
						        reinterpret_cast<const SDL_UserAuthArg*>(windowEvent.padding)))
							throw ErrorMsg{ -1, windowEvent.type, "sdl_auth_dialog_show" };
					}
					break;
					case SDL_EVENT_USER_UPDATE:
					{
						/* Merge every currently-queued damage batch into one drawToWindows()
						 * call instead of one full render+present cycle per batch. The server
						 * can push endPaint() notifications faster than several windows can be
						 * rendered and presented under gamescope; doing one expensive cycle per
						 * queued batch instead of coalescing them falls behind a sustained
						 * update rate and the backlog grows for as long as the session runs (seen
						 * live: per-event staleness climbing from ~4s to ~15s over a 96s session).
						 * The surface we draw from always reflects the latest state regardless of
						 * how many batches arrived, so drawing once for the union of all pending
						 * rects is strictly more correct, not just faster -- it never paints a
						 * stale intermediate frame that a newer pending batch already supersedes.
						 */
						std::vector<SDL_Rect> merged;
						while (true)
						{
							auto rectangles = sdl->pop();
							if (rectangles.empty())
								break;
							merged.insert(merged.end(), rectangles.begin(), rectangles.end());
						}
						stats.setUpdateArea(merged);
						if (!sdl->drawToWindows(merged))
							throw ErrorMsg{ -1, windowEvent.type, "sdl->drawToWindows" };
					}
					break;
					case SDL_EVENT_USER_CREATE_WINDOWS:
					{
						auto ctx = static_cast<SdlContext*>(windowEvent.user.data1);
						if (!ctx->createWindows())
							throw ErrorMsg{ -1, windowEvent.type, "sdl->createWindows" };
					}
					break;
					case SDL_EVENT_USER_WINDOW_RESIZEABLE:
					{
						auto window = static_cast<SdlWindow*>(windowEvent.user.data1);
						const bool use = windowEvent.user.code != 0;
						if (window)
							window->resizeable(use);
					}
					break;
					case SDL_EVENT_USER_WINDOW_FULLSCREEN:
					{
						auto window = static_cast<SdlWindow*>(windowEvent.user.data1);
						const bool enter = windowEvent.user.code != 0;
						const bool forceOriginalDisplay = windowEvent.user.data2 != nullptr;
						if (window)
							window->fullscreen(enter, forceOriginalDisplay);
					}
					break;
					case SDL_EVENT_USER_WINDOW_MINIMIZE:
						if (!sdl->minimizeAllWindows())
							throw ErrorMsg{ -1, windowEvent.type, "sdl->minimizeAllWindows" };
						break;
					case SDL_EVENT_USER_POINTER_NULL:
						if (!sdl->setCursor(SdlContext::CURSOR_NULL))
							throw ErrorMsg{ -1, windowEvent.type, "sdl->setCursor" };
						break;
					case SDL_EVENT_USER_POINTER_DEFAULT:
						if (!sdl->setCursor(SdlContext::CURSOR_DEFAULT))
							throw ErrorMsg{ -1, windowEvent.type, "sdl->setCursor" };
						break;
					case SDL_EVENT_USER_POINTER_POSITION:
					{
						const auto x =
						    static_cast<INT32>(reinterpret_cast<uintptr_t>(windowEvent.user.data1));
						const auto y =
						    static_cast<INT32>(reinterpret_cast<uintptr_t>(windowEvent.user.data2));
						if (!sdl->moveMouseTo(
						        { static_cast<float>(x) * 1.0f, static_cast<float>(y) * 1.0f }))
							throw ErrorMsg{ -1, windowEvent.type, "sdl->moveMouseTo" };
					}
					break;
					case SDL_EVENT_USER_POINTER_SET:
						if (!sdl->setCursor(static_cast<rdpPointer*>(windowEvent.user.data1)))
							throw ErrorMsg{ -1, windowEvent.type, "sdl->setCursor" };
						break;
					case SDL_EVENT_USER_RAIL_MOVE:
					{
						const auto id = static_cast<uint32_t>(
						    reinterpret_cast<uintptr_t>(windowEvent.user.data1));
						const auto moveType = static_cast<uint16_t>(windowEvent.user.code);
						sdl->getRailChannelContext().handleLocalMoveRequested(id, moveType);
					}
					break;
					case SDL_EVENT_USER_TOUCH_RELEASE:
						if (!sdl->flushPendingTouchRelease(windowEvent.user.code))
							throw ErrorMsg{ -1, windowEvent.type, "sdl->flushPendingTouchRelease" };
						break;
					case SDL_EVENT_USER_QUIT:
					default:
						break;
				}
			}
		}
		rc = 1;
	}
	catch (ErrorMsg& msg)
	{
		WLog_Print(sdl->getWLog(), WLOG_ERROR, "[exception] %s", msg.what());
		rc = msg.rc();
	}

	return rc;
}

/* Optional global initializer.
 * Here we just register a signal handler to print out stack traces
 * if available. */
[[nodiscard]] static BOOL sdl_client_global_init()
{
#if defined(_WIN32)
	WSADATA wsaData = {};
	const DWORD wVersionRequested = MAKEWORD(1, 1);
	const int rc = WSAStartup(wVersionRequested, &wsaData);
	if (rc != 0)
	{
		WLog_ERR(SDL_TAG, "WSAStartup failed with %s [%d]", gai_strerrorA(rc), rc);
		return FALSE;
	}
#endif

	return (freerdp_handle_signals() == 0);
}

/* Optional global tear down */
static void sdl_client_global_uninit()
{
#if defined(_WIN32)
	WSACleanup();
#endif
}

[[nodiscard]] static BOOL sdl_client_new(freerdp* instance, rdpContext* context)
{
	auto sdl = reinterpret_cast<sdl_rdp_context*>(context);

	if (!instance || !context)
		return FALSE;

	sdl->sdl = new SdlContext(context);
	return sdl->sdl != nullptr;
}

static void sdl_client_free([[maybe_unused]] freerdp* instance, rdpContext* context)
{
	auto sdl = reinterpret_cast<sdl_rdp_context*>(context);

	if (!context)
		return;

	delete sdl->sdl;
}

[[nodiscard]] static int sdl_client_start(rdpContext* context)
{
	auto sdl = get_context(context);
	WINPR_ASSERT(sdl);
	return sdl->start();
}

[[nodiscard]] static int sdl_client_stop(rdpContext* context)
{
	auto sdl = get_context(context);
	WINPR_ASSERT(sdl);
	return sdl->join();
}

static void RdpClientEntry(RDP_CLIENT_ENTRY_POINTS* pEntryPoints)
{
	WINPR_ASSERT(pEntryPoints);

	ZeroMemory(pEntryPoints, sizeof(RDP_CLIENT_ENTRY_POINTS));
	pEntryPoints->Version = RDP_CLIENT_INTERFACE_VERSION;
	pEntryPoints->Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
	pEntryPoints->GlobalInit = sdl_client_global_init;
	pEntryPoints->GlobalUninit = sdl_client_global_uninit;
	pEntryPoints->ContextSize = sizeof(sdl_rdp_context);
	pEntryPoints->ClientNew = sdl_client_new;
	pEntryPoints->ClientFree = sdl_client_free;
	pEntryPoints->ClientStart = sdl_client_start;
	pEntryPoints->ClientStop = sdl_client_stop;
}

static void context_free(sdl_rdp_context* sdl)
{
	if (sdl)
		freerdp_client_context_free(&sdl->common.context);
}

[[nodiscard]] static const char* category2str(int category)
{
	switch (category)
	{
		case SDL_LOG_CATEGORY_APPLICATION:
			return "SDL_LOG_CATEGORY_APPLICATION";
		case SDL_LOG_CATEGORY_ERROR:
			return "SDL_LOG_CATEGORY_ERROR";
		case SDL_LOG_CATEGORY_ASSERT:
			return "SDL_LOG_CATEGORY_ASSERT";
		case SDL_LOG_CATEGORY_SYSTEM:
			return "SDL_LOG_CATEGORY_SYSTEM";
		case SDL_LOG_CATEGORY_AUDIO:
			return "SDL_LOG_CATEGORY_AUDIO";
		case SDL_LOG_CATEGORY_VIDEO:
			return "SDL_LOG_CATEGORY_VIDEO";
		case SDL_LOG_CATEGORY_RENDER:
			return "SDL_LOG_CATEGORY_RENDER";
		case SDL_LOG_CATEGORY_INPUT:
			return "SDL_LOG_CATEGORY_INPUT";
		case SDL_LOG_CATEGORY_TEST:
			return "SDL_LOG_CATEGORY_TEST";
		case SDL_LOG_CATEGORY_GPU:
			return "SDL_LOG_CATEGORY_GPU";
		case SDL_LOG_CATEGORY_RESERVED2:
			return "SDL_LOG_CATEGORY_RESERVED2";
		case SDL_LOG_CATEGORY_RESERVED3:
			return "SDL_LOG_CATEGORY_RESERVED3";
		case SDL_LOG_CATEGORY_RESERVED4:
			return "SDL_LOG_CATEGORY_RESERVED4";
		case SDL_LOG_CATEGORY_RESERVED5:
			return "SDL_LOG_CATEGORY_RESERVED5";
		case SDL_LOG_CATEGORY_RESERVED6:
			return "SDL_LOG_CATEGORY_RESERVED6";
		case SDL_LOG_CATEGORY_RESERVED7:
			return "SDL_LOG_CATEGORY_RESERVED7";
		case SDL_LOG_CATEGORY_RESERVED8:
			return "SDL_LOG_CATEGORY_RESERVED8";
		case SDL_LOG_CATEGORY_RESERVED9:
			return "SDL_LOG_CATEGORY_RESERVED9";
		case SDL_LOG_CATEGORY_RESERVED10:
			return "SDL_LOG_CATEGORY_RESERVED10";
		case SDL_LOG_CATEGORY_CUSTOM:
		default:
			return "SDL_LOG_CATEGORY_CUSTOM";
	}
}

[[nodiscard]] static SDL_LogPriority wloglevel2dl(DWORD level)
{
	switch (level)
	{
		case WLOG_TRACE:
			return SDL_LOG_PRIORITY_VERBOSE;
		case WLOG_DEBUG:
			return SDL_LOG_PRIORITY_DEBUG;
		case WLOG_INFO:
			return SDL_LOG_PRIORITY_INFO;
		case WLOG_WARN:
			return SDL_LOG_PRIORITY_WARN;
		case WLOG_ERROR:
			return SDL_LOG_PRIORITY_ERROR;
		case WLOG_FATAL:
			return SDL_LOG_PRIORITY_CRITICAL;
		case WLOG_OFF:
		default:
			return SDL_LOG_PRIORITY_VERBOSE;
	}
}

[[nodiscard]] static DWORD sdlpriority2wlog(SDL_LogPriority priority)
{
	DWORD level = WLOG_OFF;
	switch (priority)
	{
		case SDL_LOG_PRIORITY_VERBOSE:
			level = WLOG_TRACE;
			break;
		case SDL_LOG_PRIORITY_DEBUG:
			level = WLOG_DEBUG;
			break;
		case SDL_LOG_PRIORITY_INFO:
			level = WLOG_INFO;
			break;
		case SDL_LOG_PRIORITY_WARN:
			level = WLOG_WARN;
			break;
		case SDL_LOG_PRIORITY_ERROR:
			level = WLOG_ERROR;
			break;
		case SDL_LOG_PRIORITY_CRITICAL:
			level = WLOG_FATAL;
			break;
		default:
			break;
	}

	return level;
}

static void SDLCALL winpr_LogOutputFunction(void* userdata, int category, SDL_LogPriority priority,
                                            const char* message)
{
	auto sdl = static_cast<SdlContext*>(userdata);
	WINPR_ASSERT(sdl);

	const DWORD level = sdlpriority2wlog(priority);
	auto log = sdl->getWLog();
	if (!WLog_IsLevelActive(log, level))
		return;

	WLog_PrintTextMessage(log, level, __LINE__, __FILE__, __func__, "[%s] %s",
	                      category2str(category), message);
}

static void sdl_quit()
{
	const auto cat = SDL_LOG_CATEGORY_APPLICATION;
	SDL_Event ev = {};
	ev.type = SDL_EVENT_QUIT;
	if (!SDL_PushEvent(&ev))
	{
		SDL_LogError(cat, "An error occurred: %s", SDL_GetError());
	}
}

static void SDLCALL rdp_file_cb(void* userdata, const char* const* filelist,
                                WINPR_ATTR_UNUSED int filter)
{
	auto rdp = static_cast<std::string*>(userdata);

	const auto cat = SDL_LOG_CATEGORY_APPLICATION;
	if (!filelist)
	{
		SDL_LogError(cat, "An error occurred: %s", SDL_GetError());
		sdl_quit();
		return;
	}
	else if (!*filelist)
	{
		SDL_LogError(cat, "The user did not select any file.");
		SDL_LogError(cat, "Most likely, the dialog was canceled.");
		sdl_quit();
		return;
	}

	while (*filelist)
	{
		SDL_LogError(cat, "Full path to selected file: '%s'", *filelist);
		*rdp = *filelist;
		filelist++;
	}

	sdl_quit();
}

[[nodiscard]] static std::string getRdpFile()
{
	const auto flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS;
	SDL_DialogFileFilter filters[] = { { "RDP files", "rdp;rdpw" } };
	std::string rdp;

	bool running = true;
	if (!SDL_Init(flags))
		return {};

	auto props = SDL_CreateProperties();
	SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING,
	                      "SDL Freerdp - Open a RDP file");
	SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);
	SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, filters);
	SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, ARRAYSIZE(filters));
	SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, rdp_file_cb, &rdp, props);
	SDL_DestroyProperties(props);

	do
	{
		SDL_Event event = {};
		std::ignore = SDL_PollEvent(&event);

		switch (event.type)
		{
			case SDL_EVENT_QUIT:
				running = false;
				break;
			default:
				break;
		}
	} while (running);
	SDL_Quit();
	return rdp;
}

int main(int argc, char* argv[])
{
#if defined(_WIN32)
	sdl::win32::release_transient_console();
#endif

	int rc = -1;
	RDP_CLIENT_ENTRY_POINTS clientEntryPoints = {};

	/* Allocate the RDP context first, we need to pass it to SDL */
	RdpClientEntry(&clientEntryPoints);
	std::unique_ptr<sdl_rdp_context, void (*)(sdl_rdp_context*)> sdl_rdp(
	    reinterpret_cast<sdl_rdp_context*>(freerdp_client_context_new(&clientEntryPoints)),
	    context_free);

	if (!sdl_rdp)
		return -1;
	auto sdl = sdl_rdp->sdl;

	auto settings = sdl->context()->settings;
	WINPR_ASSERT(settings);

	std::string rdp_file;
	std::vector<char*> args;
	args.reserve(WINPR_ASSERTING_INT_CAST(size_t, argc));
	for (auto x = 0; x < argc; x++)
		args.push_back(argv[x]);

	if (argc == 1)
	{
		rdp_file = getRdpFile();
		if (!rdp_file.empty())
		{
			args.push_back(rdp_file.data());
		}
	}

	auto status = freerdp_client_settings_parse_command_line_ex(
	    settings, WINPR_ASSERTING_INT_CAST(int, args.size()), args.data(), FALSE, sdl->args(),
	    sdl->argsCount(), &SdlContext::argumentHandler, sdl);
	sdl_rdp->sdl->setMetadata();
	if (status)
	{
		rc = freerdp_client_settings_command_line_status_print_ex(settings, status, argc, argv,
		                                                          sdl->args());
		if (freerdp_settings_get_bool(settings, FreeRDP_ListMonitors))
		{
			if (!sdl_list_monitors(sdl))
				return -1;
		}
		else
		{
			switch (status)
			{
				case COMMAND_LINE_STATUS_PRINT:
				case COMMAND_LINE_STATUS_PRINT_VERSION:
				case COMMAND_LINE_STATUS_PRINT_BUILDCONFIG:
					break;
				case COMMAND_LINE_STATUS_PRINT_HELP:
					SdlPref::print_config_file_help(3);
					break;
				default:
					break;
			}
		}
		return rc;
	}

	if (!SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0"))
		return -1;
	if (!SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0"))
		return -1;
	if (!SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "0"))
		return -1;
	if (!SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0"))
		return -1;
	if (!SDL_SetHint(SDL_HINT_PEN_TOUCH_EVENTS, "1"))
		return -1;
	if (!SDL_SetHint(SDL_HINT_TRACKPAD_IS_TOUCH_ONLY, "1"))
		return -1;
	if (!SDL_SetHint(SDL_HINT_SCREENSAVER_INHIBIT_ACTIVITY_NAME, "RDP session running"))
		return -1;
#if SDL_VERSION_ATLEAST(3, 4, 0)
	if (!SDL_SetHint(SDL_HINT_MOUSE_DPI_SCALE_CURSORS, "1"))
		return -1;
#endif

	/* Basic SDL initialization */
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
		return -1;

	/* Redirect SDL log messages to wLog */
	SDL_SetLogOutputFunction(winpr_LogOutputFunction, sdl);
	auto level = WLog_GetLogLevel(sdl->getWLog());
	SDL_SetLogPriorities(wloglevel2dl(level));

	auto backend = SDL_GetCurrentVideoDriver();
	WLog_Print(sdl->getWLog(), WLOG_DEBUG, "client is using backend '%s'", backend);
	sdl_dialogs_init();

	/* SDL cleanup code if the client exits */
	ScopeGuard guard(
	    [&]()
	    {
		    sdl->cleanup();
		    freerdp_del_signal_cleanup_handler(sdl->context(), sdl_term_handler);
		    sdl_dialogs_uninit();
		    SDL_Quit();
	    });
	if (!sdl->detectDisplays())
		return -1;

	/* Initialize RDP */
	auto context = sdl->context();
	WINPR_ASSERT(context);

	if (!stream_dump_register_handlers(context, CONNECTION_STATE_MCS_CREATE_REQUEST, FALSE))
		return -1;

	if (freerdp_client_start(context) != 0)
		return -1;

	rc = sdl_run(sdl);

	if (freerdp_client_stop(context) != 0)
		return -1;

	if (sdl->exitCode() != 0)
		rc = sdl->exitCode();

	return rc;
}
