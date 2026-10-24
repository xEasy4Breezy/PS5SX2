// PS5SX2 (2.11, AI-assisted): runs the real glue of the app's 119.88 Hz switch (orbis-shims/OrbisVideoOutMode.cpp: the flags, the worker
// thread, the join, the marker file, the 5 s waits) on a PC against a fake VideoOut, flags and folder. The sequence itself is checked
// by test_videoout_mode.cpp; this is what decides WHETHER the system is touched at all (the default: never) and how the threads end.
// One scenario a run (the 5 s waits are real time); test-videoout.sh runs them all:
//   g++ -std=c++17 -pthread -I../../include-orbis -I../../orbis-shims test_videoout_glue.cpp ../../orbis-shims/OrbisVideoOutMode.cpp -o t
//   ./t <scenario> <folder for the marker>          (-DTEST_DRIVER_API adds a driver export, scenario driver_api)
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "OrbisDriver.h"
#include "OrbisPaths.h"
#include "OrbisVideoOutMode.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

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

// ---- what the app's own files provide (orbis_paths.cpp, orbis_vk.cpp), faked -------------------------------------------------
static std::set<std::string> g_flags;
static std::string g_dir;
static bool g_radv = false;

std::string OrbisLogPath(const char* name)
{
	return g_dir + "/" + name;
}
bool OrbisFlag(const char* name)
{
	return g_flags.count(name) != 0;
}
bool OrbisDriverIsRADV()
{
	return g_radv;
}
const char* OrbisDriverName()
{
	return g_radv ? "RADV" : "ps5vk";
}

#ifdef TEST_DRIVER_API
extern "C" const char* ps5vk_output_status(void)
{
	return "fake driver status";
}
#endif

// ---- a fake VideoOut ---------------------------------------------------------------------------------------------------------
static std::mutex g_mutex;
static std::vector<std::string> g_calls;
static int g_open_result = 5, g_supported = 1, g_configure_high = 0, g_configure_restore = 0;
static long g_period_us = 8342;

static void Record(const std::string& call)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_calls.push_back(call);
}
static std::vector<std::string> Calls()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_calls;
}
// The calls that are `what` or start with `what` and a space ("configure 1" does not count "configure 15").
static int Count(const std::string& what)
{
	int n = 0;
	for (const std::string& c : Calls())
		n += (c == what || c.rfind(what + " ", 0) == 0) ? 1 : 0;
	return n;
}

extern "C"
{
	int sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void*)
	{
		Record("open " + std::to_string(user) + " " + std::to_string(bus) + " " + std::to_string(index));
		return g_open_result;
	}
	int sceVideoOutClose(int32_t handle)
	{
		Record("close " + std::to_string(handle));
		return 0;
	}
	int sceVideoOutWaitVblank(int32_t)
	{
		Record("wait");
		usleep(static_cast<useconds_t>(g_period_us));
		return 0;
	}
	int sceVideoOutIsOutputSupported(int32_t, uint32_t mode, const void*, const void*, const void*)
	{
		Record("supported " + std::to_string(mode));
		return mode == 15 || mode == 1 ? g_supported : static_cast<int>(0x8029001Eu);
	}
	int sceVideoOutConfigureOutput(int32_t, uint32_t mode, const void*, const void*, const void*)
	{
		Record("configure " + std::to_string(mode));
		return mode == 1 ? g_configure_restore : g_configure_high;
	}
}

static bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}

static double Seconds(std::chrono::steady_clock::time_point since)
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

static bool Has(const std::string& text, const char* part)
{
	return text.find(part) != std::string::npos;
}

// Nothing may have touched the system: no VideoOut call, no marker.
static void ExpectUntouched()
{
	CHECK(Calls().empty());
	CHECK(!Exists(g_dir + "/output-120.txt"));
	CHECK(OrbisVideoOutRefreshMilliHz() == 0);
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		printf("usage: %s <scenario> <folder>\n", argv[0]);
		return 2;
	}
	const std::string scenario = argv[1];
	g_dir = argv[2];
	const std::string marker = g_dir + "/output-120.txt";

	// The default: Frame generation on, no flag, a driver with no output code. Nothing touches VideoOut.
	if (scenario == "default")
	{
		OrbisVideoOutBootCheck();
		OrbisVideoOutStart(true);
		CHECK(!OrbisVideoOutWait());
		OrbisVideoOutNoteSurfaceClosed();
		CHECK(!OrbisVideoOutWait());
		OrbisVideoOutRestore();
		ExpectUntouched();
		CHECK(Has(OrbisVideoOutSummary(), "opt-in") && Has(OrbisVideoOutSummary(), "fg120"));
	}
	// Frame generation off, whatever the flags say.
	else if (scenario == "fg_off")
	{
		g_flags = {"fg120"};
		OrbisVideoOutStart(false);
		CHECK(!OrbisVideoOutWait());
		OrbisVideoOutRestore();
		ExpectUntouched();
		CHECK(Has(OrbisVideoOutSummary(), "frame generation is off"));
	}
	// The kill flag beats the opt-in.
	else if (scenario == "kill")
	{
		g_flags = {"fg120", "nofg120"};
		OrbisVideoOutStart(true);
		CHECK(!OrbisVideoOutWait());
		ExpectUntouched();
		CHECK(Has(OrbisVideoOutSummary(), "nofg120"));
	}
	// The driver has an output control of its own: the app stays out (built with -DTEST_DRIVER_API).
	else if (scenario == "driver_api")
	{
		g_flags = {"fg120"};
		OrbisVideoOutStart(true);
		CHECK(!OrbisVideoOutWait());
		OrbisVideoOutRestore();
		ExpectUntouched();
		CHECK(Has(OrbisVideoOutSummary(), "driver has its own output control"));
	}
	// RADV lists its modes itself.
	else if (scenario == "radv")
	{
		g_flags = {"fg120"};
		g_radv = true;
		OrbisVideoOutStart(true);
		CHECK(!OrbisVideoOutWait());
		ExpectUntouched();
		CHECK(Has(OrbisVideoOutSummary(), "RADV"));
	}
	// The switch, with the flag: 119.88 Hz taken, 5 s before the swapchain, 5 s after a close, restored on the way out.
	else if (scenario == "switch")
	{
		g_flags = {"fg120"};
		const auto t0 = std::chrono::steady_clock::now();
		OrbisVideoOutStart(true);
		OrbisVideoOutStart(true); // a second start is ignored: one thread, one switch (the call count below)
		CHECK(Seconds(t0) < 1.0); // the thread does the work: Start returns at once
		CHECK(OrbisVideoOutRefreshMilliHz() == 0); // nothing is claimed before the switch is done
		CHECK(OrbisVideoOutWait());
		const double waited = Seconds(t0);
		CHECK(waited >= 5.0 && waited < 8.0); // the 5 s before the driver opens VideoOut
		const uint32_t mhz = OrbisVideoOutRefreshMilliHz();
		CHECK(mhz == 119880); // a sleep's jitter (8.4 ms and more here) is still the console's 119.88 Hz
		CHECK(Exists(marker));
		const std::vector<std::string> calls = Calls();
		CHECK(calls.size() == 11);
		if (calls.size() == 11)
		{
			CHECK(calls[0] == "open 255 0 0" && calls[1] == "supported 15" && calls[2] == "configure 15" && calls[10] == "close 5");
			for (int i = 3; i < 10; i++)
				CHECK(calls[i] == "wait");
		}
		CHECK(Has(OrbisVideoOutSummary(), "119.88 Hz selected"));
		// a second wait with nothing closed: no new wait
		const auto t1 = std::chrono::steady_clock::now();
		CHECK(OrbisVideoOutWait() && Seconds(t1) < 0.5);
		// the swapchain closed (a GS reopen): the next surface waits for the 5 s from the close
		OrbisVideoOutNoteSurfaceClosed();
		const auto t2 = std::chrono::steady_clock::now();
		CHECK(OrbisVideoOutWait());
		const double again = Seconds(t2);
		CHECK(again >= 4.5 && again < 7.5);
		CHECK(Count("open") == 1); // the wait did not touch VideoOut
		// the way out: the default output back on a handle of its own, the marker gone, the rate forgotten
		OrbisVideoOutRestore();
		CHECK(Count("open") == 2 && Count("configure 1") == 1 && Count("close") == 2);
		CHECK(!Exists(marker));
		CHECK(OrbisVideoOutRefreshMilliHz() == 0);
		CHECK(Has(OrbisVideoOutSummary(), "put back"));
		OrbisVideoOutRestore(); // every exit path calls it: the second does nothing
		CHECK(Count("open") == 2);
		CHECK(!OrbisVideoOutWait());
	}
	// Restoring while the thread still sleeps its 5 s: the restore joins it first, then puts the output back.
	else if (scenario == "restore_early")
	{
		g_flags = {"fg120"};
		OrbisVideoOutStart(true);
		usleep(300000); // the thread is in its 5 s
		CHECK(OrbisVideoOutRefreshMilliHz() == 0);
		const auto t0 = std::chrono::steady_clock::now();
		OrbisVideoOutRestore();
		CHECK(Seconds(t0) >= 4.0); // it waited for the thread
		CHECK(Count("configure 15") == 1 && Count("configure 1") == 1 && Count("open") == 2 && Count("close") == 2);
		CHECK(!Exists(marker));
		CHECK(OrbisVideoOutRefreshMilliHz() == 0 && !OrbisVideoOutWait());
	}
	// The console still refreshes at 60 Hz after the mode was accepted: put back at once, no 5 s, no claim of 120 Hz.
	else if (scenario == "slow")
	{
		g_flags = {"fg120"};
		g_period_us = 16683;
		const auto t0 = std::chrono::steady_clock::now();
		OrbisVideoOutStart(true);
		CHECK(!OrbisVideoOutWait());
		CHECK(Seconds(t0) < 3.0);
		CHECK(Count("configure 15") == 1 && Count("configure 1") == 1 && Count("close") == 1 && Count("open") == 1);
		CHECK(!Exists(marker) && OrbisVideoOutRefreshMilliHz() == 0);
		CHECK(Has(OrbisVideoOutSummary(), "not refreshing at 120 Hz"));
		OrbisVideoOutRestore();
		CHECK(Count("open") == 1); // nothing was switched: nothing to restore
	}
	// The console refuses the mode.
	else if (scenario == "refused")
	{
		g_flags = {"fg120"};
		g_configure_high = static_cast<int>(0x80290019u);
		OrbisVideoOutStart(true);
		CHECK(!OrbisVideoOutWait());
		CHECK(Count("configure 15") == 1 && Count("configure 1") == 0 && Count("wait") == 0 && Count("close") == 1);
		CHECK(!Exists(marker) && OrbisVideoOutRefreshMilliHz() == 0);
		CHECK(Has(OrbisVideoOutSummary(), "0x80290019"));
	}
	// The console does not offer the mode (1080p, or a TV without 120 Hz): never configured.
	else if (scenario == "not_offered")
	{
		g_flags = {"fg120"};
		g_supported = 0;
		OrbisVideoOutStart(true);
		CHECK(!OrbisVideoOutWait());
		CHECK(Count("configure") == 0 && Count("close") == 1 && !Exists(marker));
		CHECK(Has(OrbisVideoOutSummary(), "not offered"));
	}
	// VideoOut does not open: three tries, 500 ms apart, then nothing.
	else if (scenario == "open_fail")
	{
		g_flags = {"fg120"};
		g_open_result = static_cast<int>(0x80290001u);
		const auto t0 = std::chrono::steady_clock::now();
		OrbisVideoOutStart(true);
		CHECK(!OrbisVideoOutWait());
		const double took = Seconds(t0);
		CHECK(took >= 0.9 && took < 3.0);
		CHECK(Count("open") == 3 && Count("close") == 0 && Count("configure") == 0 && !Exists(marker));
		CHECK(Has(OrbisVideoOutSummary(), "sceVideoOutOpen failed"));
	}
	// The last run left the output at 120 Hz (the marker): put back at the start, 5 s passed, and no switch in this run.
	else if (scenario == "boot_marker")
	{
		g_flags = {"fg120"};
		if (FILE* f = fopen(marker.c_str(), "w"))
			fclose(f);
		const auto t0 = std::chrono::steady_clock::now();
		OrbisVideoOutBootCheck();
		CHECK(Seconds(t0) >= 4.5);
		CHECK(!Exists(marker));
		CHECK(Count("open") == 1 && Count("configure 1") == 1 && Count("close") == 1 && Count("configure 15") == 0);
		OrbisVideoOutStart(true);
		CHECK(!OrbisVideoOutWait());
		CHECK(Count("open") == 1 && Count("configure 15") == 0 && !Exists(marker));
		CHECK(Has(OrbisVideoOutSummary(), "last run ended with the output at 120 Hz"));
	}
	// A marker and a restore that fails: the marker stays.
	else if (scenario == "boot_marker_fail")
	{
		g_configure_restore = static_cast<int>(0x80290001u);
		if (FILE* f = fopen(marker.c_str(), "w"))
			fclose(f);
		const auto t0 = std::chrono::steady_clock::now();
		OrbisVideoOutBootCheck();
		CHECK(Seconds(t0) < 3.0); // no settle after a restore that failed
		CHECK(Exists(marker));
		CHECK(Count("configure 1") == 1);
	}
	// Flag videoprobe: the console is asked about 64 selectors, nothing is configured.
	else if (scenario == "probe")
	{
		g_flags = {"videoprobe"};
		OrbisVideoOutBootCheck();
		CHECK(Count("open") == 1 && Count("supported") == 64 && Count("close") == 1);
		CHECK(Count("configure") == 0 && Count("wait") == 0 && !Exists(marker));
		OrbisVideoOutStart(true);
		CHECK(!OrbisVideoOutWait());
		CHECK(Count("open") == 1); // frame generation without the opt-in flag: still nothing
	}
	else
	{
		printf("unknown scenario %s\n", scenario.c_str());
		return 2;
	}

	printf("%s: %s: %d of %d checks passed\n", s_failures ? "FAIL" : "PASS", scenario.c_str(), s_checks - s_failures, s_checks);
	return s_failures ? 1 : 0;
}
