// PS5SX2 (2.11, AI-assisted): the benchmark of the disc lag round. The REAL CDVD thread file (pcsx2/CDVD/CDVDdiscThread.cpp, or the frozen build 174 one with
// -DREF_THREAD) and the REAL read-ahead windows (OrbisDiscReadAhead.h) on top of a model of the PS5's disc drive with the numbers the testers' disc drive
// tests measured (a seek ~200 ms, 2.78 MB/s at 2.0x and 4.43 MB/s at 3.2x, a 256-sector read-ahead buffer of its own), driven by a model of PCSX2's
// emulated CDVD (a request and a buffer read per sector, the emulated seek times of CDVD.cpp). It measures what the emulation's CPU thread waits for in
// cdvdGetSector: in the blocking design that wait is a freeze of the whole emulator (no frames, no audio mixing).
//
// The model is a ranking of designs, not a promise: one head, 200 ms seeks, one CPU thread, no real CPU/GS, real sleeps so a single run is +-30%. The
// real engine has a few more stalls than this model shows. It cannot tell a frozen host from a host that runs on with late data: both are the same wait.
//
//   bench_cdvdthread <scenario 0..5> <speed 2.0|3.2> <windows on|off> [switch file ...]     prints one line: RESULT key=value ...
//   (the switch files, as the app's OrbisFlag: discblock, discpipe ...; run_bench.py runs the matrix and checks the relations)
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cdvd_stubs.h"
#include "OrbisDiscReadAhead.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

FakeDrive* g_drive = nullptr;

using clk = std::chrono::steady_clock;
static double NowS() { return std::chrono::duration<double>(clk::now().time_since_epoch()).count(); }
static void SleepS(double s)
{
	if (s > 0)
		std::this_thread::sleep_for(std::chrono::duration<double>(s));
}
static void SleepUntilS(double t) { SleepS(t - NowS()); }

// ------------------------------------------------------------------------------------------------------------------------------------ the drive
struct SimDrive
{
	double rate = 1357.0; // sectors per second (2.78 MB/s = 2.0x)
	double seek = 0.200;  // s
	uint32_t cap = 256;   // sectors the drive reads ahead by itself while idle
	bool valid = false;
	double base = 0, head0 = 0, t0 = 0;
	uint64_t cmds = 0, seeks = 0, sectors = 0;

	double HeadAt(double t) const
	{
		const double h = head0 + (t - t0) * rate;
		return std::min(h, base + cap);
	}
	void Read(uint32_t s, uint32_t n)
	{
		cmds++;
		sectors += n;
		const double t = NowS();
		if (valid && s >= base && s <= HeadAt(t) + 0.5)
		{
			const double h = HeadAt(t);
			const double end = double(s) + n;
			if (end <= h)
			{
				base = end; // all in the drive's buffer
				head0 = h;
				t0 = t;
			}
			else
			{
				const double cost = (end - h) / rate;
				SleepS(cost);
				base = end;
				head0 = end;
				t0 = t + cost;
			}
		}
		else
		{
			seeks++;
			const double cost = seek + n / rate;
			SleepS(cost);
			base = double(s) + n;
			head0 = base;
			t0 = t + cost;
			valid = true;
		}
	}
};

// IOCtlSrc::ReadSectors2048 of the app: m_lock around the read-ahead windows around the drive.
struct SimFake : FakeDrive
{
	std::mutex lock; // IOCtlSrc::m_lock
	SimDrive drive;
	orbis_disc::ReadAhead windows;
	bool Read2048(uint32_t sector, uint32_t count, uint8_t* buffer) override
	{
		std::lock_guard<std::mutex> lk(lock);
		auto rd = [this](uint32_t s, uint32_t c, uint8_t*) {
			drive.Read(s, c);
			return true;
		};
		return windows.Read(sector, count, buffer, total, rd);
	}
};

// ------------------------------------------------------------------------------------------------------------------------------------ the emulated CDVD (the CPU thread)
struct Run
{
	std::vector<double> stalls; // each cdvdGetSector wait over 1 ms
	double elapsed = 0;
	double emu_time = 0; // emulated (IOP) seconds the script covers
};
static Run g_run;
static const double kReadTime = 1.0 / 1080.0; // CLV 1.6x: 675 * 1.6 sectors a second

static void EmuGet(u32 lsn)
{
	const double t = NowS();
	cdvdGetSector(lsn, CDVD_MODE_2048);
	const double d = NowS() - t;
	if (d > 0.001)
		g_run.stalls.push_back(d);
}

static u32 s_prev_end = 0;
static void EmuCommand(u32 start, u32 sectors)
{
	// the emulated seek (CDVD.cpp cdvdStartSeek): contiguous within 16 sectors, else 30 ms (near) or 100 ms (far)
	const u32 delta = start > s_prev_end ? start - s_prev_end : s_prev_end - start;
	const double emu_seek = delta < 16 ? 0.0 : delta >= 14764 ? 0.100 : 0.030;
	cdvdRequestSector(start, CDVD_MODE_2048); // DoCDVDreadTrack at the command
	SleepS(emu_seek);
	double deadline = NowS();
	for (u32 i = 0; i < sectors; i++)
	{
		const u32 lsn = start + i;
		const double t = NowS();
		EmuGet(lsn);
		if (NowS() - t > 0.002)
			deadline = NowS();
		cdvdRequestSector(lsn + 1, CDVD_MODE_2048);
		deadline += kReadTime;
		SleepUntilS(deadline);
	}
	g_run.emu_time += emu_seek + sectors * kReadTime;
	s_prev_end = start + sectors;
}

static void Think(double s) { SleepS(s); }

// ------------------------------------------------------------------------------------------------------------------------------------ scenarios
static int g_loader_files = 0;
static void ScenarioStream() // one file read start to end
{
	u32 at = 300000;
	for (int i = 0; i < 48; i++) // 48 x 256 sectors = 12288 sectors = 11.4 s
	{
		EmuCommand(at, 256);
		at += 256;
	}
}
static void ScenarioLoader() // 24 files, each at another place
{
	std::mt19937 rng(7);
	for (int i = 0; i < 24; i++)
	{
		const u32 start = 100000 + (rng() % 1800000) / 16 * 16;
		const u32 len = 128 + (rng() % 385);
		EmuCommand(start, len);
		Think(0.03);
		g_loader_files++;
	}
}
static void ScenarioTwoStreams() // music and data: one block from each place in turn
{
	u32 a = 300000, b = 1500000;
	for (int i = 0; i < 60; i++)
	{
		EmuCommand(a, 16);
		a += 16;
		EmuCommand(b, 16);
		b += 16;
	}
}
static void ScenarioNearTwo16() // two streams 10000 sectors apart (a fast seek in the emulation: 30 ms), one block from each in turn
{
	u32 a = 300000, b = 310000;
	for (int i = 0; i < 60; i++)
	{
		EmuCommand(a, 16);
		a += 16;
		EmuCommand(b, 16);
		b += 16;
	}
}
static void ScenarioNearTwo64() // the same with 64 sectors (128 KiB) at a time
{
	u32 a = 300000, b = 310000;
	for (int i = 0; i < 60; i++)
	{
		EmuCommand(a, 64);
		a += 64;
		EmuCommand(b, 64);
		b += 64;
	}
}
static void ScenarioStreamHops() // a stream with a jump to somewhere else every 1.5 s
{
	std::mt19937 rng(11);
	for (int hop = 0; hop < 7; hop++)
	{
		u32 at = 100000 + (rng() % 1800000) / 16 * 16;
		for (int i = 0; i < 6; i++) // 6 x 256 sectors = 1536 sectors = 1.4 s
		{
			EmuCommand(at, 256);
			at += 256;
		}
	}
}

struct Scenario
{
	const char* name;
	void (*fn)();
};
static const Scenario kScenarios[] = {{"stream", ScenarioStream}, {"loader", ScenarioLoader}, {"two-streams", ScenarioTwoStreams},
	{"stream+hops", ScenarioStreamHops}, {"near-two-16", ScenarioNearTwo16}, {"near-two-64", ScenarioNearTwo64}};

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::fprintf(stderr, "usage: bench_cdvdthread <scenario 0..5> <speed 2.0|3.2> <windows on|off> [switch file ...]\n");
		return 2;
	}
	const int sc = std::atoi(argv[1]);
	const bool fast = std::string(argv[2]) == "3.2";
	const bool windows = std::string(argv[3]) == "on";
	std::set<std::string> flags;
	for (int i = 4; i < argc; i++)
		flags.insert(argv[i]);
	if (sc < 0 || sc >= static_cast<int>(sizeof(kScenarios) / sizeof(kScenarios[0])))
		return 2;
	{
		std::lock_guard<std::mutex> l(g_flags_lock);
		g_flags = flags;
	}

	SimFake fake;
	fake.drive.rate = fast ? 2162.0 : 1357.0;
	fake.windows.SetEnabled(windows);
	fake.total = 2000000;
	g_drive = &fake;
	newDiscCB = [] {};
	src = std::make_unique<IOCtlSrc>("sim");
	src->Reopen(nullptr);
	cdvdStartThread();
	const double t0 = NowS();
	kScenarios[sc].fn();
	g_run.elapsed = NowS() - t0;
	cdvdStopThread();
	src.reset();

	std::vector<double> st = g_run.stalls;
	std::sort(st.begin(), st.end());
	double sum = 0;
	int over33 = 0, over100 = 0;
	for (double d : st)
	{
		sum += d;
		over33 += d > 0.033;
		over100 += d > 0.100;
	}
	std::printf("RESULT scenario=%s wait_ms=%.0f max_ms=%.0f p95_ms=%.0f over33=%d over100=%d run_s=%.1f emu_s=%.1f speed_pct=%.0f cmds=%llu seeks=%llu mb=%.1f files=%d\n",
		kScenarios[sc].name, sum * 1000, st.empty() ? 0.0 : st.back() * 1000, st.empty() ? 0.0 : st[size_t(st.size() * 0.95)] * 1000, over33, over100,
		g_run.elapsed, g_run.emu_time, 100.0 * g_run.emu_time / g_run.elapsed, (unsigned long long)fake.drive.cmds, (unsigned long long)fake.drive.seeks,
		fake.drive.sectors * 2048 / 1e6, g_loader_files);
	return 0;
}
