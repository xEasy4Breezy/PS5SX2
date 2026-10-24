// PS5SX2 (2.11, AI-assisted): a check of the display rate frame generation paces against (orbis-shims/OrbisDisplayHz.h) on a PC,
// and of what the pacing (OrbisFrameGenPacing.h) does with each rate it can give, PAL 50 -> 100 included.
//   g++ -std=c++17 -Wall -Wextra -I../../orbis-shims -o /tmp/test_display_hz test_display_hz.cpp && /tmp/test_display_hz
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "OrbisDisplayHz.h"
#include "OrbisFrameGenPacing.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using orbis_fg::DisplayHz;
using orbis_fg::DisplayHzSource;
using orbis_fg::PickDisplayHz;
using orbis_fg::PickDisplayHzSource;

static int s_failures = 0;
static int s_checks = 0;
#define CHECK(cond)                                                    \
	do                                                                 \
	{                                                                  \
		s_checks++;                                                    \
		if (!(cond))                                                   \
		{                                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
			s_failures++;                                              \
		}                                                              \
	} while (0)

// What the pacing does for a steady game of `vsyncs` PS2 vsyncs a frame at the PS2's rate on a display at `hz`.
static unsigned GeneratedPresents(uint32_t vsyncs, double ps2_hz, double hz)
{
	orbis_fg::Pacing p;
	orbis_fg::Decision d;
	double now = 100.0;
	for (int i = 0; i < 50; i++)
	{
		now += vsyncs / ps2_hz;
		d = p.Frame(vsyncs, 1 + (vsyncs - 1) / 4, ps2_hz, hz, now, 100.0, true);
	}
	return d.engaged ? d.presents : 0;
}

int main()
{
	// Nothing gives a rate: 59.94, said to be assumed.
	{
		const DisplayHz d = PickDisplayHzSource(0, 0, 0);
		CHECK(d.hz == 59.94 && d.source == DisplayHzSource::Assumed);
		CHECK(PickDisplayHz(0, 0, 0) == 59.94);
	}

	// The order: the driver's measurement beats the app's own switch, which beats the listed mode.
	{
		DisplayHz d = PickDisplayHzSource(119880, 59940, 59940);
		CHECK(d.hz == 119.88 && d.source == DisplayHzSource::Driver);
		d = PickDisplayHzSource(59940, 119880, 119880); // a driver that measures 60 Hz is the truth, whatever else says
		CHECK(d.hz == 59.94 && d.source == DisplayHzSource::Driver);
		d = PickDisplayHzSource(0, 119880, 59940);
		CHECK(d.hz == 119.88 && d.source == DisplayHzSource::Switch);
		d = PickDisplayHzSource(0, 0, 119880);
		CHECK(d.hz == 119.88 && d.source == DisplayHzSource::ListedMode);
		d = PickDisplayHzSource(0, 0, 59940);
		CHECK(d.hz == 59.94 && d.source == DisplayHzSource::ListedMode);
		CHECK(PickDisplayHz(0, 119880, 59940) == 119.88);
		CHECK(PickDisplayHz(0, 0, 119880) == 119.88);
	}

	// Rates a display cannot have are skipped, to the next source and in the end to 59.94.
	{
		DisplayHz d = PickDisplayHzSource(10000, 0, 119880); // 10 Hz from the driver: not a display
		CHECK(d.hz == 119.88 && d.source == DisplayHzSource::ListedMode);
		d = PickDisplayHzSource(300000, 119880, 0);
		CHECK(d.hz == 119.88 && d.source == DisplayHzSource::Switch);
		d = PickDisplayHzSource(0, 19999, 250001);
		CHECK(d.hz == 59.94 && d.source == DisplayHzSource::Assumed);
		d = PickDisplayHzSource(20000, 0, 0); // the edges are in
		CHECK(std::fabs(d.hz - 20.0) < 1e-9 && d.source == DisplayHzSource::Driver);
		d = PickDisplayHzSource(0, 0, 250000);
		CHECK(std::fabs(d.hz - 250.0) < 1e-9 && d.source == DisplayHzSource::ListedMode);
		CHECK(!orbis_fg::DisplayMilliHzUsable(0) && !orbis_fg::DisplayMilliHzUsable(19999) && orbis_fg::DisplayMilliHzUsable(20000) &&
			  orbis_fg::DisplayMilliHzUsable(250000) && !orbis_fg::DisplayMilliHzUsable(250001));
	}

	// The console's two rates under the names a driver lists them by: 60.00 is 59.94, 120.00 is 119.88.
	{
		CHECK(PickDisplayHz(0, 0, 60000) == 59.94);  // ps5vk lists its 59.94 Hz mode as 60.00 (the 163 log's "3840x2160 at 60.00 Hz")
		CHECK(PickDisplayHz(0, 0, 59940) == 59.94);
		CHECK(PickDisplayHz(0, 0, 59800) == 59.94);
		CHECK(PickDisplayHz(0, 0, 60100) == 59.94);
		CHECK(std::fabs(PickDisplayHz(0, 0, 59799) - 59.799) < 1e-9);
		CHECK(std::fabs(PickDisplayHz(0, 0, 60101) - 60.101) < 1e-9);
		CHECK(PickDisplayHz(0, 0, 119880) == 119.88);
		CHECK(PickDisplayHz(0, 0, 120000) == 119.88);
		CHECK(PickDisplayHz(0, 0, 119500) == 119.88);
		CHECK(PickDisplayHz(0, 0, 120100) == 119.88);
		CHECK(std::fabs(PickDisplayHz(0, 0, 119499) - 119.499) < 1e-9);
		CHECK(std::fabs(PickDisplayHz(0, 0, 120101) - 120.101) < 1e-9);
		CHECK(std::fabs(PickDisplayHz(0, 0, 144000) - 144.0) < 1e-9); // other displays are not touched
		CHECK(std::fabs(PickDisplayHz(0, 0, 100000) - 100.0) < 1e-9);
		CHECK(std::fabs(PickDisplayHz(0, 0, 50000) - 50.0) < 1e-9);
		// A measured period (8.342 ms -> 119.9) and one that came out a hair off:
		CHECK(PickDisplayHz(0, 119884, 0) == 119.88);
		CHECK(PickDisplayHz(119700, 0, 0) == 119.88);
	}

	// The sources have names for the log.
	{
		const char* const names[] = {orbis_fg::DisplayHzSourceName(DisplayHzSource::Assumed), orbis_fg::DisplayHzSourceName(DisplayHzSource::Driver),
			orbis_fg::DisplayHzSourceName(DisplayHzSource::Switch), orbis_fg::DisplayHzSourceName(DisplayHzSource::ListedMode)};
		for (int i = 0; i < 4; i++)
		{
			CHECK(names[i] && names[i][0]);
			for (int j = i + 1; j < 4; j++)
				CHECK(std::strcmp(names[i], names[j]) != 0);
		}
	}

	// Rate -> pacing: the listed mode alone decides whether a 60 fps or a PAL 50 fps game has room for a generated frame.
	{
		const double hz120 = PickDisplayHz(0, 0, 119880), hz120_label = PickDisplayHz(0, 0, 120000), hz60 = PickDisplayHz(0, 0, 60000);
		CHECK(GeneratedPresents(1, 59.94, hz120) == 1);   // NTSC 60 fps on the 119.88 Hz mode: two refreshes a frame
		CHECK(GeneratedPresents(1, 59.94, hz120_label) == 1);
		CHECK(GeneratedPresents(1, 50.0, hz120) == 1);    // PAL 50 -> 100: 2.4 refreshes a frame, one generated before each
		CHECK(GeneratedPresents(1, 49.76, hz120) == 1);   // PAL non-interlaced
		CHECK(GeneratedPresents(2, 50.0, hz120) == 2);    // PAL 25 fps on 120 Hz: the generated frame twice
		CHECK(GeneratedPresents(1, 59.94, hz60) == 0);    // a 60 fps game on the 60 Hz mode: none (the 60.00 label is 59.94)
		CHECK(GeneratedPresents(1, 50.0, hz60) == 0);     // PAL 50 fps on 60 Hz: 1.2 refreshes, no room
		CHECK(GeneratedPresents(2, 59.94, hz60) == 1);    // 30 fps at 60 Hz
		CHECK(GeneratedPresents(2, 50.0, hz60) == 1);     // PAL 25 fps at 60 Hz
	}

	printf("%s: %d of %d checks passed (the display rate of frame generation)\n", s_failures ? "FAIL" : "PASS", s_checks - s_failures,
		s_checks);
	return s_failures ? 1 : 0;
}
