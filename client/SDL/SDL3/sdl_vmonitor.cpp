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
#include "sdl_vmonitor.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>

namespace sdl::vmonitor
{
namespace
{
std::vector<std::string> splitTopLevel(const std::string& s, char sep)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= s.size())
	{
		auto pos = s.find(sep, start);
		if (pos == std::string::npos)
		{
			out.push_back(s.substr(start));
			break;
		}
		out.push_back(s.substr(start, pos - start));
		start = pos + 1;
	}
	return out;
}

bool isAllDigits(const std::string& s)
{
	if (s.empty())
		return false;
	for (char c : s)
	{
		if (std::isdigit(static_cast<unsigned char>(c)) == 0)
			return false;
	}
	return true;
}

bool parseDimension(const std::string& s, int32_t& out, std::string& error, const char* which)
{
	if (!isAllDigits(s))
	{
		error = std::string(which) + " value '" + s + "' is not a positive integer";
		return false;
	}

	errno = 0;
	char* end = nullptr;
	const long val = std::strtol(s.c_str(), &end, 10);
	if ((errno != 0) || !end || (*end != '\0') || (val <= 0) || (val > 65535))
	{
		error = std::string(which) + " value '" + s + "' must be between 1 and 65535";
		return false;
	}
	out = static_cast<int32_t>(val);
	return true;
}

/* Parses an X11-geometry-style "+X+Y" / "+X-Y" / "-X+Y" / "-X-Y" position.
 * An explicit sign is required on both axes; there is no unsigned form. */
bool parsePosition(const std::string& s, int32_t& x, int32_t& y, std::string& error)
{
	if (s.empty() || ((s[0] != '+') && (s[0] != '-')))
	{
		error = "position '" + s + "' must start with '+' or '-' (expected +X+Y style)";
		return false;
	}

	size_t i = 1;
	while ((i < s.size()) && (std::isdigit(static_cast<unsigned char>(s[i])) != 0))
		i++;
	if (i == 1)
	{
		error = "position '" + s + "' is missing a value after the first sign";
		return false;
	}

	if ((i >= s.size()) || ((s[i] != '+') && (s[i] != '-')))
	{
		error = "position '" + s + "' is missing a sign before the Y value (expected +X+Y style)";
		return false;
	}

	const size_t ySignPos = i;
	size_t j = i + 1;
	while ((j < s.size()) && (std::isdigit(static_cast<unsigned char>(s[j])) != 0))
		j++;
	if (j == ySignPos + 1)
	{
		error = "position '" + s + "' is missing a value after the second sign";
		return false;
	}
	if (j != s.size())
	{
		error = "position '" + s + "' has unexpected trailing characters '" + s.substr(j) + "'";
		return false;
	}

	try
	{
		x = std::stoi(s.substr(0, ySignPos));
		y = std::stoi(s.substr(ySignPos));
	}
	catch (const std::exception&)
	{
		error = "position '" + s + "' is out of range";
		return false;
	}
	return true;
}

/* Splits one `WxH[+X+Y][*]` token. Position detection relies on width/height
 * being plain unsigned digit strings, so the first '+'/'-' in the token
 * (after stripping a trailing '*') always starts the position, never a
 * dimension. */
bool parseOneSpec(const std::string& spec, rdpMonitor& m, bool& hasPos, bool& isPrimary,
                  std::string& error)
{
	std::string s = spec;
	isPrimary = false;
	if (!s.empty() && (s.back() == '*'))
	{
		isPrimary = true;
		s.pop_back();
	}

	const auto signPos = s.find_first_of("+-");
	const std::string dims = (signPos == std::string::npos) ? s : s.substr(0, signPos);
	const std::string pos = (signPos == std::string::npos) ? std::string() : s.substr(signPos);

	const auto xSep = dims.find('x');
	if (xSep == std::string::npos)
	{
		error = "'" + spec + "' is missing 'x' between width and height (expected WxH)";
		return false;
	}

	int32_t w = 0;
	int32_t h = 0;
	if (!parseDimension(dims.substr(0, xSep), w, error, "width"))
		return false;
	if (!parseDimension(dims.substr(xSep + 1), h, error, "height"))
		return false;

	int32_t x = 0;
	int32_t y = 0;
	hasPos = false;
	if (!pos.empty())
	{
		if (!parsePosition(pos, x, y, error))
			return false;
		hasPos = true;
	}

	m = {};
	m.width = w;
	m.height = h;
	m.x = x;
	m.y = y;
	m.attributes.physicalWidth = static_cast<uint32_t>(w);
	m.attributes.physicalHeight = static_cast<uint32_t>(h);
	m.attributes.orientation = 0; /* landscape; portrait is expressed via height > width */
	m.attributes.deviceScaleFactor = 100;
	m.attributes.desktopScaleFactor = 100; /* overridden below if /vmonitor-scale was given */
	return true;
}
} // namespace

bool parse(const char* vmonitorsArg, const char* vmonitorScaleArg,
           std::vector<rdpMonitor>& monitors, std::string& error)
{
	monitors.clear();
	error.clear();

	if (!vmonitorsArg || (*vmonitorsArg == '\0'))
	{
		error = "empty /vmonitors value";
		return false;
	}

	const auto specs = splitTopLevel(vmonitorsArg, ',');
	if (specs.size() > kMaxMonitors)
	{
		error = "too many monitors declared (" + std::to_string(specs.size()) +
		        "), the RDP protocol supports at most " + std::to_string(kMaxMonitors);
		return false;
	}

	std::vector<rdpMonitor> result;
	std::vector<bool> hasPos;
	std::vector<bool> isPrimaryFlags;
	result.reserve(specs.size());
	hasPos.reserve(specs.size());
	isPrimaryFlags.reserve(specs.size());

	int primaryCount = 0;
	for (size_t i = 0; i < specs.size(); i++)
	{
		rdpMonitor m{};
		bool hp = false;
		bool prim = false;
		if (!parseOneSpec(specs[i], m, hp, prim, error))
		{
			error = "monitor " + std::to_string(i + 1) + ": " + error;
			return false;
		}
		m.orig_screen = static_cast<uint32_t>(i);
		if (prim)
			primaryCount++;
		result.push_back(m);
		hasPos.push_back(hp);
		isPrimaryFlags.push_back(prim);
	}

	if (primaryCount > 1)
	{
		error = "more than one monitor marked primary with '*'";
		return false;
	}

	/* Default layout: left-to-right at y=0, continuing from wherever the
	 * previous monitor (explicit or defaulted) ended. Matches the spec's
	 * "if positions are omitted, lay monitors out left to right starting
	 * at x=0" for the all-omitted case, and gives predictable behavior for
	 * a mix of explicit and omitted positions too. */
	int32_t cursorX = 0;
	for (size_t i = 0; i < result.size(); i++)
	{
		if (!hasPos[i])
		{
			result[i].x = cursorX;
			result[i].y = 0;
		}
		cursorX = result[i].x + result[i].width;
	}

	if (primaryCount == 0)
		isPrimaryFlags[0] = true;
	for (size_t i = 0; i < result.size(); i++)
		result[i].is_primary = isPrimaryFlags[i] ? 1 : 0;

	if (vmonitorScaleArg && (*vmonitorScaleArg != '\0'))
	{
		const auto scaleTokens = splitTopLevel(vmonitorScaleArg, ',');
		if ((scaleTokens.size() != 1) && (scaleTokens.size() != result.size()))
		{
			error = "/vmonitor-scale has " + std::to_string(scaleTokens.size()) +
			        " value(s), expected 1 (applied to all monitors) or " +
			        std::to_string(result.size()) + " (one per monitor)";
			return false;
		}

		std::vector<uint32_t> scales;
		scales.reserve(scaleTokens.size());
		for (const auto& tok : scaleTokens)
		{
			if (!isAllDigits(tok))
			{
				error = "/vmonitor-scale value '" + tok + "' is not a positive integer percentage";
				return false;
			}

			errno = 0;
			char* end = nullptr;
			const long val = std::strtol(tok.c_str(), &end, 10);
			if ((errno != 0) || !end || (*end != '\0') || (val < 25) || (val > 500))
			{
				error = "/vmonitor-scale value '" + tok + "' must be between 25 and 500 (percent)";
				return false;
			}
			scales.push_back(static_cast<uint32_t>(val));
		}

		for (size_t i = 0; i < result.size(); i++)
			result[i].attributes.desktopScaleFactor = (scales.size() == 1) ? scales[0] : scales[i];
	}

	monitors = std::move(result);
	return true;
}

} // namespace sdl::vmonitor
