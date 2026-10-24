// PS5SX2 (2.11, AI-assisted): the refresh rate frame generation paces against.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Frame generation (OrbisFrameGenPacing.h) counts the display's refreshes a game frame has: refreshes = PS2 vsyncs x display Hz / PS2 Hz.
// A 60 fps game needs a display at 119.88 Hz to have two; a PAL game at 50 fps has 2.4 there (one generated frame before each of
// its own: 50 shown as 100) and 1.2 at 59.94 Hz (no room). Until 2.11 the rate was the driver's own measurement
// (ps5vk_output_refresh_millihertz, a function the driver of 2.02 had) or 59.94 Hz, whatever the display really was: a driver that
// lists the 119.88 Hz mode through VK_KHR_display (the upstream PS5_Vulkan's design, RADV) still got a pacing that never saw room.
// The sources, first one that gives a rate a display can have:
//   1. the driver's measurement of the output its swapchain presents to (the truth about the display)
//   2. PS5SX2's own switch (OrbisVideoOutMode.cpp, flag fg120): its measured vblank period
//   3. the refresh rate of the mode the swapchain was made for (VKSwapChain.cpp)
//   4. 59.94 Hz
// The console's two rates have other names than the ones a driver lists them under (ps5vk lists its 59.94 Hz mode as 60.00; a
// panel's 120 Hz is 119.88 on the PS5), so a rate near one of them is that one. A pure header, so a PC test runs it (tests/fgpacing).

#pragma once

#include <cstdint>

namespace orbis_fg
{
	enum class DisplayHzSource
	{
		Assumed,    // nothing gave a rate: 59.94 Hz
		Driver,     // the driver's measurement of the output
		Switch,     // PS5SX2's own VideoOut switch
		ListedMode, // the display mode the swapchain was made for
	};

	struct DisplayHz
	{
		double hz = 59.94;
		DisplayHzSource source = DisplayHzSource::Assumed;
	};

	// What a display can refresh at, in millihertz (the pacing's own limits are 20 to 250 Hz).
	inline bool DisplayMilliHzUsable(uint32_t mhz)
	{
		return mhz >= 20000 && mhz <= 250000;
	}

	// The PS5's 60 and 120 Hz outputs are 59.94 and 119.88 Hz.
	inline double SnapDisplayHz(double hz)
	{
		if (hz >= 59.8 && hz <= 60.1)
			return 59.94;
		if (hz >= 119.5 && hz <= 120.1)
			return 119.88;
		return hz;
	}

	inline DisplayHz PickDisplayHzSource(uint32_t hook_mhz, uint32_t switch_mhz, uint32_t mode_mhz)
	{
		DisplayHz d;
		if (DisplayMilliHzUsable(hook_mhz))
		{
			d.hz = SnapDisplayHz(hook_mhz / 1000.0);
			d.source = DisplayHzSource::Driver;
		}
		else if (DisplayMilliHzUsable(switch_mhz))
		{
			d.hz = SnapDisplayHz(switch_mhz / 1000.0);
			d.source = DisplayHzSource::Switch;
		}
		else if (DisplayMilliHzUsable(mode_mhz))
		{
			d.hz = SnapDisplayHz(mode_mhz / 1000.0);
			d.source = DisplayHzSource::ListedMode;
		}
		return d;
	}

	inline double PickDisplayHz(uint32_t hook_mhz, uint32_t switch_mhz, uint32_t mode_mhz)
	{
		return PickDisplayHzSource(hook_mhz, switch_mhz, mode_mhz).hz;
	}

	inline const char* DisplayHzSourceName(DisplayHzSource source)
	{
		switch (source)
		{
			case DisplayHzSource::Driver:
				return "the driver's measurement";
			case DisplayHzSource::Switch:
				return "PS5SX2's own 120 Hz switch";
			case DisplayHzSource::ListedMode:
				return "the display mode the driver lists";
			case DisplayHzSource::Assumed:
				break;
		}
		return "assumed: the driver gave no rate and lists no 120 Hz mode";
	}
} // namespace orbis_fg
