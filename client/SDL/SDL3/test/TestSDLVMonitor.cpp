#include "../sdl_vmonitor.hpp"

#include <cstdio>

namespace
{
int failures = 0;

#define CHECK(cond)                                                                        \
	do                                                                                      \
	{                                                                                       \
		if (!(cond))                                                                       \
		{                                                                                  \
			printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
			failures++;                                                                    \
		}                                                                                  \
	} while (0)

void expectParseFailure(const char* vmonitors, const char* scale = nullptr)
{
	std::vector<rdpMonitor> monitors;
	std::string error;
	const bool ok = sdl::vmonitor::parse(vmonitors, scale, monitors, error);
	if (ok)
		printf("[FAIL] expected parse failure for vmonitors='%s' scale='%s', but it succeeded\n",
		       vmonitors, scale ? scale : "(null)");
	CHECK(!ok);
	CHECK(monitors.empty());
	CHECK(!error.empty());
}

void testBasicThreeSideBySide()
{
	std::vector<rdpMonitor> m;
	std::string error;
	CHECK(sdl::vmonitor::parse("1920x1080,1920x1080,1920x1080", nullptr, m, error));
	CHECK(m.size() == 3);
	if (m.size() != 3)
		return;

	/* left to right, starting at x=0, as the spec's own first example */
	CHECK(m[0].x == 0 && m[0].y == 0 && m[0].width == 1920 && m[0].height == 1080);
	CHECK(m[1].x == 1920 && m[1].y == 0);
	CHECK(m[2].x == 3840 && m[2].y == 0);

	/* none marked '*': first one defaults to primary, others do not */
	CHECK(m[0].is_primary == 1);
	CHECK(m[1].is_primary == 0);
	CHECK(m[2].is_primary == 0);
}

void testExplicitPositionsAndPortrait()
{
	std::vector<rdpMonitor> m;
	std::string error;
	/* spec's own second example, including a portrait monitor on the right */
	CHECK(sdl::vmonitor::parse("1920x1080,1920x1080+1920+0,1080x1920+3840+0", nullptr, m, error));
	CHECK(m.size() == 3);
	if (m.size() != 3)
		return;

	CHECK(m[0].x == 0 && m[0].y == 0);
	CHECK(m[1].x == 1920 && m[1].y == 0);
	CHECK(m[2].x == 3840 && m[2].y == 0);

	/* portrait: height > width, no special orientation flag needed */
	CHECK(m[2].width == 1080);
	CHECK(m[2].height == 1920);
	CHECK(m[2].attributes.orientation == 0);
}

void testExplicitPrimaryMarker()
{
	std::vector<rdpMonitor> m;
	std::string error;
	CHECK(sdl::vmonitor::parse("1920x1080,1920x1080*,1920x1080", nullptr, m, error));
	CHECK(m.size() == 3);
	if (m.size() != 3)
		return;
	CHECK(m[0].is_primary == 0);
	CHECK(m[1].is_primary == 1);
	CHECK(m[2].is_primary == 0);
}

void testMultiplePrimaryMarkersRejected()
{
	expectParseFailure("1920x1080*,1920x1080*");
}

void testMixedExplicitAndOmittedPositions()
{
	std::vector<rdpMonitor> m;
	std::string error;
	/* first omitted (defaults to x=0), second explicit elsewhere, third
	 * omitted again: continues left-to-right from where #2 ended. */
	CHECK(sdl::vmonitor::parse("800x600,1920x1080+5000+200,640x480", nullptr, m, error));
	CHECK(m.size() == 3);
	if (m.size() != 3)
		return;
	CHECK(m[0].x == 0 && m[0].y == 0);
	CHECK(m[1].x == 5000 && m[1].y == 200);
	CHECK(m[2].x == 5000 + 1920 && m[2].y == 0);
}

void testNegativeAndOverlappingPositionsAccepted()
{
	std::vector<rdpMonitor> m;
	std::string error;
	/* A monitor placed up/left of the primary (negative coordinates) and
	 * a second monitor fully overlapping the first: both must be accepted
	 * as-is. The parser lays out a declared geometry; it is not responsible
	 * for rejecting layouts a real desktop wouldn't produce. */
	CHECK(sdl::vmonitor::parse("1920x1080,1920x1080-1920-1080,1920x1080+0+0", nullptr, m, error));
	CHECK(m.size() == 3);
	if (m.size() != 3)
		return;
	CHECK(m[1].x == -1920 && m[1].y == -1080);
	CHECK(m[2].x == 0 && m[2].y == 0); /* fully overlaps monitor 0 */
}

void testSingleMonitor()
{
	std::vector<rdpMonitor> m;
	std::string error;
	CHECK(sdl::vmonitor::parse("1920x1080", nullptr, m, error));
	CHECK(m.size() == 1);
	if (m.size() == 1)
		CHECK(m[0].is_primary == 1);
}

void testInvalidInputRejected()
{
	expectParseFailure(nullptr);
	expectParseFailure("");
	expectParseFailure("1920");             /* missing 'x' separator */
	expectParseFailure("1920x");             /* missing height */
	expectParseFailure("x1080");             /* missing width */
	expectParseFailure("0x1080");             /* zero width */
	expectParseFailure("1920x0");             /* zero height */
	expectParseFailure("-1920x1080");         /* negative dimension, not a valid position */
	expectParseFailure("1920x1080x3840");     /* extra token */
	expectParseFailure("1920x1080+100");      /* position missing Y sign/value */
	expectParseFailure("1920x1080+100+");     /* position missing Y value */
	expectParseFailure("1920x1080100+200");   /* missing sign before X */
	expectParseFailure("1920x1080+100+200x"); /* trailing garbage after position */
	expectParseFailure("abcxdef");             /* non-numeric dimensions */
}

void testTooManyMonitorsRejected()
{
	std::string spec;
	for (int i = 0; i < 17; i++)
	{
		if (i)
			spec += ",";
		spec += "640x480";
	}
	expectParseFailure(spec.c_str());
}

void testExactlySixteenMonitorsAccepted()
{
	std::string spec;
	for (int i = 0; i < 16; i++)
	{
		if (i)
			spec += ",";
		spec += "640x480";
	}
	std::vector<rdpMonitor> m;
	std::string error;
	CHECK(sdl::vmonitor::parse(spec.c_str(), nullptr, m, error));
	CHECK(m.size() == 16);
}

void testScaleAppliedToAll()
{
	std::vector<rdpMonitor> m;
	std::string error;
	CHECK(sdl::vmonitor::parse("1920x1080,1920x1080,1920x1080", "150", m, error));
	CHECK(m.size() == 3);
	for (const auto& mon : m)
	{
		CHECK(mon.attributes.desktopScaleFactor == 150);
		CHECK(mon.attributes.deviceScaleFactor == 100);
	}
}

void testScalePerMonitor()
{
	std::vector<rdpMonitor> m;
	std::string error;
	CHECK(sdl::vmonitor::parse("1920x1080,1920x1080,1920x1080", "100,125,150", m, error));
	CHECK(m.size() == 3);
	if (m.size() != 3)
		return;
	CHECK(m[0].attributes.desktopScaleFactor == 100);
	CHECK(m[1].attributes.desktopScaleFactor == 125);
	CHECK(m[2].attributes.desktopScaleFactor == 150);
}

void testScaleDefaultsTo100WhenNotGiven()
{
	std::vector<rdpMonitor> m;
	std::string error;
	CHECK(sdl::vmonitor::parse("1920x1080", nullptr, m, error));
	CHECK(m.size() == 1);
	if (m.size() == 1)
		CHECK(m[0].attributes.desktopScaleFactor == 100);
}

void testScaleCountMismatchRejected()
{
	expectParseFailure("1920x1080,1920x1080,1920x1080", "100,125");
}

void testScaleOutOfRangeRejected()
{
	expectParseFailure("1920x1080", "10");  /* below 25 */
	expectParseFailure("1920x1080", "1000"); /* above 500 */
	expectParseFailure("1920x1080", "abc");
}

} // namespace

int TestSDLVMonitor(int argc, char* argv[])
{
	(void)argc;
	(void)argv;

	testBasicThreeSideBySide();
	testExplicitPositionsAndPortrait();
	testExplicitPrimaryMarker();
	testMultiplePrimaryMarkersRejected();
	testMixedExplicitAndOmittedPositions();
	testNegativeAndOverlappingPositionsAccepted();
	testSingleMonitor();
	testInvalidInputRejected();
	testTooManyMonitorsRejected();
	testExactlySixteenMonitorsAccepted();
	testScaleAppliedToAll();
	testScalePerMonitor();
	testScaleDefaultsTo100WhenNotGiven();
	testScaleCountMismatchRejected();
	testScaleOutOfRangeRejected();

	if (failures)
		printf("TestSDLVMonitor: %d check(s) failed\n", failures);
	return failures ? -1 : 0;
}
