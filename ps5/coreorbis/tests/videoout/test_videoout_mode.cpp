// PS5SX2 (2.11, AI-assisted): a check of the app's own switch of the console's VideoOut to 119.88 Hz (orbis-shims/OrbisVideoOutMode.h)
// on a PC, with a fake VideoOut, clock and marker file. The real calls (OrbisVideoOutMode.cpp) are not run here.
//   tests/videoout/test-videoout.sh       or by hand:
//   g++ -std=c++17 -Wall -Wextra -I../../orbis-shims -o /tmp/test_videoout_mode test_videoout_mode.cpp && /tmp/test_videoout_mode
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "OrbisVideoOutMode.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace orbis_vout;

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

constexpr int kErrNotDeclaredI = static_cast<int>(0x80290016u);
constexpr int kErrNoFormatI = static_cast<int>(0x80290019u);
constexpr int kErrUnknownI = static_cast<int>(0x8029001Eu);
constexpr int kErrOpenI = static_cast<int>(0x80290001u);

// A VideoOut that logs every call. Each vblank wait advances the clock by the next interval of `periods` (or `period_ns`), the first
// wait (the phase) by 5 ms.
struct Fake
{
	std::vector<std::string> log;
	int open_result = 7;                       // the handle, or < 0: the open fails
	std::map<uint32_t, int> supported_by;      // selector -> answer; missing ones answer `supported_default`
	int supported_default = 1;
	int configure_high = 0;                    // ConfigureOutput(15)
	int configure_restore = 0;                 // ConfigureOutput(1)
	uint64_t period_ns = 8'342'000;
	std::vector<uint64_t> periods;             // the intervals of the 6 timed waits, when set
	int fail_wait_at = -1;                     // the nth wait (0 = the phase wait) fails
	uint64_t clock = 1'000'000'000;
	int waits = 0, opens = 0, closes = 0;
	bool marker = false;
	int marker_sets = 0, marker_clears = 0;

	static Fake* Of(void* ctx) { return static_cast<Fake*>(ctx); }

	static int Open(void* ctx)
	{
		Fake* f = Of(ctx);
		f->log.push_back("open");
		if (f->open_result < 0)
			return f->open_result;
		f->opens++;
		return f->open_result;
	}
	static int Close(void* ctx, int handle)
	{
		Fake* f = Of(ctx);
		f->log.push_back("close " + std::to_string(handle));
		f->closes++;
		return 0;
	}
	static int Supported(void* ctx, int, uint32_t selector)
	{
		Fake* f = Of(ctx);
		f->log.push_back("supported " + std::to_string(selector));
		const auto it = f->supported_by.find(selector);
		return it != f->supported_by.end() ? it->second : f->supported_default;
	}
	static int Configure(void* ctx, int, uint32_t selector)
	{
		Fake* f = Of(ctx);
		f->log.push_back("configure " + std::to_string(selector));
		return selector == kSelectorRestore ? f->configure_restore : f->configure_high;
	}
	static int Wait(void* ctx, int)
	{
		Fake* f = Of(ctx);
		const int at = f->waits++;
		f->log.push_back("wait");
		if (at == f->fail_wait_at)
			return kErrOpenI;
		if (at == 0)
			f->clock += 5'000'000;
		else
			f->clock += !f->periods.empty() ? f->periods[(at - 1) % f->periods.size()] : f->period_ns;
		return 0;
	}
	static uint64_t Now(void* ctx) { return Of(ctx)->clock; }
	static void Sleep(void* ctx, uint32_t ms)
	{
		Fake* f = Of(ctx);
		f->log.push_back("sleep " + std::to_string(ms));
		f->clock += static_cast<uint64_t>(ms) * 1'000'000;
	}
	static bool MarkerPresent(void* ctx) { return Of(ctx)->marker; }
	static void MarkerSet(void* ctx)
	{
		Fake* f = Of(ctx);
		f->log.push_back("marker set");
		f->marker = true;
		f->marker_sets++;
	}
	static void MarkerClear(void* ctx)
	{
		Fake* f = Of(ctx);
		f->log.push_back("marker clear");
		f->marker = false;
		f->marker_clears++;
	}

	Ops MakeOps(bool with_marker = true)
	{
		Ops ops;
		ops.ctx = this;
		ops.open = Open;
		ops.close = Close;
		ops.supported = Supported;
		ops.configure = Configure;
		ops.wait_vblank = Wait;
		ops.now_ns = Now;
		ops.sleep_ms = Sleep;
		if (with_marker)
		{
			ops.marker_present = MarkerPresent;
			ops.marker_set = MarkerSet;
			ops.marker_clear = MarkerClear;
		}
		return ops;
	}

	int Count(const std::string& what) const
	{
		int n = 0;
		for (const std::string& line : log)
			n += line == what ? 1 : 0;
		return n;
	}
	int IndexOf(const std::string& what) const
	{
		for (size_t i = 0; i < log.size(); i++)
			if (log[i] == what)
				return static_cast<int>(i);
		return -1;
	}
	int CountPrefix(const char* prefix) const
	{
		int n = 0;
		for (const std::string& line : log)
			n += line.rfind(prefix, 0) == 0 ? 1 : 0;
		return n;
	}
	std::string Joined() const
	{
		std::string text;
		for (const std::string& line : log)
			text += (text.empty() ? "" : " | ") + line;
		return text;
	}
};

static bool Contains(const std::string& text, const char* part)
{
	return text.find(part) != std::string::npos;
}

int main()
{
	// The constants the plan took from the 2.02 driver and the upstream driver.
	CHECK(kSelectorRestore == 1 && kSelectorHighFrameRate == 15);
	CHECK(kPeriodIntervals == 6 && kHighPeriodLimitNs == 12'500'000);
	CHECK(kReopenSettleMs == 5000); // 5,000,000,000 ns, read from the 2.02 driver (not the 2 s of the first guess)

	// Active: offered, accepted, 8.34 ms periods. Order: open, supported, marker, configure, the waits, close, then the 5 s sleep.
	{
		Fake f;
		const Result r = SelectHighFrameRate(f.MakeOps());
		CHECK(r.outcome == Outcome::Active);
		CHECK(r.supported == 1 && r.configured == 0 && !r.restore_ran);
		CHECK(r.period_ns == 8'342'000);
		CHECK(std::fabs(r.refresh_mhz / 1000.0 - 119.88) < 0.1);
		CHECK(f.Joined() == "open | supported 15 | marker set | configure 15 | wait | wait | wait | wait | wait | wait | wait | close 7 | sleep 5000");
		CHECK(f.Count("sleep 5000") == 1 && f.IndexOf("sleep 5000") > f.IndexOf("close 7"));
		CHECK(f.Count("configure 1") == 0); // not restored: the output stays at 119.88 Hz
		CHECK(f.marker && f.marker_clears == 0);
		CHECK(f.opens == f.closes);
		CHECK(Contains(Describe(r), "119.88 Hz selected") && Contains(Describe(r), "8.342 ms") && Contains(Describe(r), "5 s"));
	}

	// The rate reported: a period near 8.342 ms is the console's 119.88 Hz exactly (a late wait must not read as 118.2 Hz, which would take
	// a 60 fps game's second refresh away from the pacing); one far from it is what was measured.
	{
		const uint64_t periods[] = {8'341'675, 8'462'000, 7'600'000, 9'100'000};
		for (uint64_t p : periods)
		{
			Fake f;
			f.period_ns = p;
			const Result r = SelectHighFrameRate(f.MakeOps());
			CHECK(r.outcome == Outcome::Active && r.refresh_mhz == kHighRefreshMilliHz);
		}
		Fake slow;
		slow.period_ns = 12'000'000; // accepted by the 12.5 ms limit, yet not the 119.88 Hz output: its own rate
		const Result r = SelectHighFrameRate(slow.MakeOps());
		CHECK(r.outcome == Outcome::Active && r.refresh_mhz >= 83300 && r.refresh_mhz <= 83400);
		Fake fast;
		fast.period_ns = 4'000'000;
		const Result q = SelectHighFrameRate(fast.MakeOps());
		CHECK(q.outcome == Outcome::Active && q.refresh_mhz == 250000);
		Fake absurd;
		absurd.period_ns = 1; // waits that return at once: a rate no display has, and no overflow of the uint32_t on the way
		const Result a = SelectHighFrameRate(absurd.MakeOps());
		CHECK(a.outcome == Outcome::Active && a.refresh_mhz == 4'000'000'000u);
	}

	// Not offered: IsOutputSupported(15) answers 0 (1080p at the console), or an error. Nothing is configured.
	{
		Fake f;
		f.supported_by[15] = 0;
		const Result r = SelectHighFrameRate(f.MakeOps());
		CHECK(r.outcome == Outcome::NotOffered && r.supported == 0);
		CHECK(f.CountPrefix("configure") == 0);
		CHECK(f.Joined() == "open | supported 15 | close 7");
		CHECK(f.Count("sleep 5000") == 0 && f.marker_sets == 0 && f.waits == 0);
		CHECK(Contains(Describe(r), "not offered") && Contains(Describe(r), "answered 0") && Contains(Describe(r), "4K or 1440p"));
		CHECK(f.opens == f.closes);

		Fake g;
		g.supported_by[15] = kErrNotDeclaredI;
		const Result e = SelectHighFrameRate(g.MakeOps());
		CHECK(e.outcome == Outcome::NotOffered && g.CountPrefix("configure") == 0);
		CHECK(Contains(Describe(e), "0x80290016") && Contains(Describe(e), "attribute3 0x80040"));
		CHECK(g.opens == g.closes && g.marker_sets == 0);
	}

	// Refused: supported, but ConfigureOutput(15) returns an error. The codes are named; no restore (nothing changed); no sleep.
	{
		const int codes[] = {kErrNotDeclaredI, kErrNoFormatI, kErrUnknownI, kErrOpenI};
		const char* const words[] = {"does not declare high frame rates", "no 120 Hz format at its resolution", "does not know this output", "0x80290001"};
		for (int i = 0; i < 4; i++)
		{
			Fake f;
			f.configure_high = codes[i];
			const Result r = SelectHighFrameRate(f.MakeOps());
			CHECK(r.outcome == Outcome::Refused && r.configured == codes[i] && !r.restore_ran);
			CHECK(f.Count("configure 1") == 0 && f.Count("configure 15") == 1);
			CHECK(f.Count("sleep 5000") == 0 && f.waits == 0);
			CHECK(f.opens == f.closes);
			CHECK(Contains(Describe(r), "refused") && Contains(Describe(r), words[i]));
			CHECK(!f.marker && f.marker_sets == 1 && f.marker_clears == 1); // written before the configure, gone again
		}
		Result named;
		named.outcome = Outcome::Refused;
		named.configured = kErrNoFormatI;
		CHECK(Contains(Describe(named), "0x80290019"));
	}

	// Accepted, but the display still refreshes at 60 Hz (16.68 ms): the default output goes back, no 5 s settle.
	{
		Fake f;
		f.period_ns = 16'683'000;
		const Result r = SelectHighFrameRate(f.MakeOps());
		CHECK(r.outcome == Outcome::SlowVblank && r.restore_ran && r.restored == 0);
		CHECK(f.Count("configure 1") == 1 && f.IndexOf("configure 1") < f.IndexOf("close 7"));
		CHECK(f.Count("sleep 5000") == 0);
		CHECK(!f.marker && f.marker_clears == 1);
		CHECK(f.opens == f.closes);
		CHECK(Contains(Describe(r), "16.683 ms") && Contains(Describe(r), "put back"));
		// the limit itself: 12.5 ms is slow, 12.499 is fast
		Fake g;
		g.period_ns = 12'500'000;
		CHECK(SelectHighFrameRate(g.MakeOps()).outcome == Outcome::Active); // not above the limit
		Fake h;
		h.period_ns = 12'500'001;
		CHECK(SelectHighFrameRate(h.MakeOps()).outcome == Outcome::SlowVblank);
	}

	// A vblank wait fails (the phase wait or a timed one): restored, VblankFailed, no sleep.
	for (int fail_at = 0; fail_at <= 6; fail_at++)
	{
		Fake f;
		f.fail_wait_at = fail_at;
		const Result r = SelectHighFrameRate(f.MakeOps());
		CHECK(r.outcome == Outcome::VblankFailed && r.period_ns == 0 && r.restore_ran && r.restored == 0);
		CHECK(f.Count("configure 1") == 1 && f.Count("sleep 5000") == 0 && !f.marker);
		CHECK(f.opens == f.closes);
		CHECK(Contains(Describe(r), "wait failed"));
	}

	// The restore after a failed check itself failing: the marker stays for the next start.
	{
		Fake f;
		f.period_ns = 16'683'000;
		f.configure_restore = kErrOpenI;
		const Result r = SelectHighFrameRate(f.MakeOps());
		CHECK(r.outcome == Outcome::SlowVblank && r.restored == kErrOpenI);
		CHECK(f.marker && f.marker_clears == 0);
		CHECK(f.opens == f.closes);
	}

	// sceVideoOutOpen fails: nothing else is called, nothing to close.
	{
		Fake f;
		f.open_result = kErrOpenI;
		const Result r = SelectHighFrameRate(f.MakeOps());
		CHECK(r.outcome == Outcome::OpenFailed && r.open_code == kErrOpenI);
		CHECK(f.Joined() == "open" && f.closes == 0);
		CHECK(Contains(Describe(r), "sceVideoOutOpen failed") && Contains(Describe(r), "0x80290001"));
	}

	// The median: one slow or one fast outlier among the 6 intervals does not decide; a tie goes to the slower value.
	{
		Fake f;
		f.periods = {8'300'000, 8'300'000, 8'300'000, 40'000'000, 8'300'000, 8'300'000};
		CHECK(SelectHighFrameRate(f.MakeOps()).outcome == Outcome::Active);
		Fake g;
		g.periods = {8'300'000, 1'000'000, 8'300'000, 8'300'000, 8'300'000, 8'300'000};
		CHECK(SelectHighFrameRate(g.MakeOps()).outcome == Outcome::Active);
		Fake h;
		h.periods = {16'600'000, 16'600'000, 8'300'000, 8'300'000, 8'300'000, 8'300'000};
		CHECK(SelectHighFrameRate(h.MakeOps()).outcome == Outcome::Active);
		Fake i;
		i.periods = {16'600'000, 16'600'000, 16'600'000, 8'300'000, 8'300'000, 8'300'000};
		const Result r = SelectHighFrameRate(i.MakeOps());
		CHECK(r.outcome == Outcome::SlowVblank && r.period_ns == 16'600'000);
		Fake j;
		j.periods = {8'300'000, 8'300'000, 8'300'000, 8'300'000, 16'600'000, 16'600'000};
		CHECK(SelectHighFrameRate(j.MakeOps()).outcome == Outcome::Active);
		// a clock that goes backwards counts as a zero interval, not a huge one
		Fake k;
		k.periods = {8'300'000, 8'300'000, 8'300'000, 8'300'000, 8'300'000, 8'300'000};
		CHECK(MedianVblankPeriod(k.MakeOps(), 7) == 8'300'000);
	}

	// No marker functions (null): the same sequence.
	{
		Fake f;
		const Result r = SelectHighFrameRate(f.MakeOps(false));
		CHECK(r.outcome == Outcome::Active);
		CHECK(f.Joined() == "open | supported 15 | configure 15 | wait | wait | wait | wait | wait | wait | wait | close 7 | sleep 5000");
	}

	// Every combination of answers: a handle is closed once per open; configure(15) only after a positive answer; a sleep only after
	// an Active close; the marker is there at the end exactly when the output may still be at 120 Hz.
	{
		const int supported_values[] = {-5, 0, 1, 2};
		const int configure_values[] = {0, kErrNotDeclaredI};
		const uint64_t period_values[] = {8'342'000, 16'683'000, 12'500'000, 12'500'001};
		const int restore_values[] = {0, kErrOpenI};
		const int fail_values[] = {-1, 0, 4};
		const int open_values[] = {7, kErrOpenI};
		int cases = 0;
		for (int open_result : open_values)
			for (int supported : supported_values)
				for (int configure : configure_values)
					for (uint64_t period : period_values)
						for (int restore : restore_values)
							for (int fail : fail_values)
							{
								Fake f;
								f.open_result = open_result;
								f.supported_by[15] = supported;
								f.configure_high = configure;
								f.period_ns = period;
								f.configure_restore = restore;
								f.fail_wait_at = fail;
								const Result r = SelectHighFrameRate(f.MakeOps());
								cases++;
								CHECK(f.opens == f.closes);
								CHECK(f.Count("configure 15") == (open_result >= 0 && supported > 0 ? 1 : 0));
								CHECK(f.Count("sleep 5000") == (r.outcome == Outcome::Active ? 1 : 0));
								if (r.outcome == Outcome::Active)
								{
									CHECK(f.IndexOf("sleep 5000") > f.IndexOf("close 7") && f.Count("configure 1") == 0 && f.marker);
								}
								else
								{
									const bool may_be_at_120 = r.restore_ran && r.restored != 0;
									CHECK(f.marker == may_be_at_120);
									CHECK(f.Count("configure 1") == (r.restore_ran ? 1 : 0));
								}
								CHECK(!Describe(r).empty());
							}
		CHECK(cases == 2 * 4 * 2 * 4 * 2 * 3);
	}

	// RestoreDefault: open, ConfigureOutput(1), close; no settle (the process is ending); the marker goes with a restore that worked.
	{
		Fake f;
		f.marker = true;
		int code = -1;
		CHECK(RestoreDefault(f.MakeOps(), &code) && code == 0);
		CHECK(f.Joined() == "open | configure 1 | close 7 | marker clear");
		CHECK(f.Count("sleep 5000") == 0 && f.opens == f.closes && !f.marker);

		Fake g;
		g.marker = true;
		g.configure_restore = kErrOpenI;
		CHECK(!RestoreDefault(g.MakeOps(), &code) && code == kErrOpenI);
		CHECK(g.marker && g.opens == g.closes); // it stays for the next start

		Fake h;
		h.open_result = kErrNoFormatI;
		h.marker = true;
		CHECK(!RestoreDefault(h.MakeOps(), &code) && code == kErrNoFormatI);
		CHECK(h.Joined() == "open" && h.marker);
	}

	// The check at the start: no marker, no VideoOut call at all.
	{
		Fake f;
		CHECK(RestoreAtBoot(f.MakeOps()) == BootCheck::NoMarker);
		CHECK(f.log.empty());
		Fake g;
		g.marker = true;
		CHECK(RestoreAtBoot(g.MakeOps(false)) == BootCheck::NoMarker); // no marker functions: nothing to find
		CHECK(g.log.empty());
	}
	// A marker: the default output back, the marker removed, then the 5 s before the driver opens VideoOut in this process.
	{
		Fake f;
		f.marker = true;
		int code = -1;
		CHECK(RestoreAtBoot(f.MakeOps(), &code) == BootCheck::Restored && code == 0);
		CHECK(f.Joined() == "open | configure 1 | close 7 | marker clear | sleep 5000");
		CHECK(!f.marker && f.opens == f.closes);
		// and the run that follows does not switch (OrbisVideoOutMode.cpp's s_skip_this_run); a second check finds nothing
		CHECK(RestoreAtBoot(f.MakeOps()) == BootCheck::NoMarker);
	}
	// A marker and a restore that fails: the marker stays, no settle.
	{
		Fake f;
		f.marker = true;
		f.configure_restore = kErrNoFormatI;
		int code = 0;
		CHECK(RestoreAtBoot(f.MakeOps(), &code) == BootCheck::RestoreFailed && code == kErrNoFormatI);
		CHECK(f.marker && f.Count("sleep 5000") == 0 && f.opens == f.closes);
		Fake g;
		g.marker = true;
		g.open_result = kErrOpenI;
		CHECK(RestoreAtBoot(g.MakeOps(), &code) == BootCheck::RestoreFailed && code == kErrOpenI);
		CHECK(g.marker && g.Joined() == "open");
	}

	// Switch, kill the app (the marker stays), start again: restored, and not switched in that run.
	{
		Fake f;
		CHECK(SelectHighFrameRate(f.MakeOps()).outcome == Outcome::Active);
		CHECK(f.marker);
		f.log.clear();
		CHECK(RestoreAtBoot(f.MakeOps()) == BootCheck::Restored);
		CHECK(!f.marker);
	}

	// The 5 s a driver open has to wait after the 120 Hz output closed.
	{
		const uint64_t t = 10'000'000'000ull;
		CHECK(SettleLeftMs(0, t) == 0);
		CHECK(SettleLeftMs(t, t) == 5000);
		CHECK(SettleLeftMs(t, t + 1'000'000'000ull) == 4000);
		CHECK(SettleLeftMs(t, t + 4'999'999'999ull) == 1);
		CHECK(SettleLeftMs(t, t + 5'000'000'000ull) == 0);
		CHECK(SettleLeftMs(t, t + 60'000'000'000ull) == 0);
		CHECK(SettleLeftMs(t, t - 1) == 5000); // a clock that went back: the whole wait, not none
	}

	// Flag videoprobe: every selector 0..63 asked about on one handle, nothing configured.
	{
		Fake f;
		f.supported_default = kErrUnknownI;
		f.supported_by[1] = 1;
		f.supported_by[15] = 1;
		for (uint32_t s : {4u, 7u, 8u, 12u, 13u, 14u, 16u, 17u, 18u, 19u})
			f.supported_by[s] = kErrNotDeclaredI;
		int open_code = -1;
		const std::vector<ProbeRow> rows = Probe(f.MakeOps(), &open_code);
		CHECK(open_code == 7 && rows.size() == 64);
		bool in_order = rows.size() == 64;
		for (size_t i = 0; i < rows.size(); i++)
			in_order = in_order && rows[i].selector == i;
		CHECK(in_order);
		CHECK(f.CountPrefix("supported") == 64 && f.CountPrefix("configure") == 0 && f.waits == 0);
		CHECK(f.log.front() == "open" && f.log.back() == "close 7" && f.opens == f.closes);
		CHECK(rows[1].answer == 1 && rows[15].answer == 1 && rows[4].answer == kErrNotDeclaredI && rows[0].answer == kErrUnknownI);
		const std::string text = DescribeProbe(rows, open_code);
		CHECK(text.find("supported (1) for 1 15") == 0); // the positive answers first
		CHECK(Contains(text, "0x80290016") && Contains(text, "for 4 7 8 12 13 14 16 17 18 19"));
		CHECK(Contains(text, "0x8029001e") && Contains(text, "for 0 2 3 5 6"));

		Fake g;
		g.open_result = kErrOpenI;
		int code = 0;
		const std::vector<ProbeRow> none = Probe(g.MakeOps(), &code);
		CHECK(none.empty() && code == kErrOpenI && g.Joined() == "open");
		CHECK(Contains(DescribeProbe(none, code), "sceVideoOutOpen failed"));
	}

	// The texts for the log.
	{
		CHECK(CodeText(5) == "5");
		CHECK(CodeText(0) == "0");
		CHECK(CodeText(kErrOpenI) == "0x80290001");
		CHECK(Contains(CodeText(kErrNoFormatI), "no 120 Hz format"));
		CHECK(Hex(0x1u) == "0x00000001");
		for (Outcome o : {Outcome::Active, Outcome::NotOffered, Outcome::Refused, Outcome::SlowVblank, Outcome::VblankFailed, Outcome::OpenFailed})
		{
			Result r;
			r.outcome = o;
			r.period_ns = 8'342'000;
			CHECK(Describe(r).size() > 20);
		}
	}

	printf("%s: %d of %d checks passed (the app's 119.88 Hz switch)\n", s_failures ? "FAIL" : "PASS", s_checks - s_failures, s_checks);
	return s_failures ? 1 : 0;
}
