// PS5SX2 (2.11, AI-assisted): PC test of the REAL pcsx2/CDVD/CDVDdiscThread.cpp (the disc lag round: the emulation's CPU thread never reads the drive,
// the CDVD thread's wake-up, the 3 s cap, the pipeline switch, the quiesce) on top of a fake drive that the test scripts: it records every read with the
// thread that made it, can park a read on a gate, fail a block, answer "no disc".
//
// No test here depends on how fast the machine is: the waits are gates and polls with long limits; the bounds that are timings (a request served within
// 60 ms after a gate opened; the cap) are an order of magnitude from what the code does.
//
//   -DREF_THREAD: built against the frozen build 174 thread file instead (tests/cdvdthread/ref): only the tests of the two things that file does wrong
//   run (the CPU thread reads the drive itself; a request pushed during the last prefetch read waits 250 ms). test-cdvdthread.sh runs it as a check of the
//   test itself: both must FAIL there.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cdvd_stubs.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;
static double MsSince(clk::time_point t0) { return std::chrono::duration<double, std::milli>(clk::now() - t0).count(); }

FakeDrive* g_drive = nullptr;

static int s_failed = 0;
static void Check(bool ok, const char* what, const std::string& detail = {})
{
	std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  -- ", detail.c_str());
	std::fflush(stdout);
	if (!ok)
		s_failed++;
}
static std::string Num(double v, const char* unit)
{
	char b[48];
	std::snprintf(b, sizeof(b), "%.1f %s", v, unit);
	return b;
}

// ------------------------------------------------------------------------------------------------------------------------------------ the fake drive
// A gate parks the first read of one sector until the test opens it.
struct Gate
{
	std::mutex m;
	std::condition_variable cv;
	u32 sector = 0xFFFFFFFFu;
	bool arrived = false, open = true;
	void Arm(u32 s)
	{
		std::lock_guard<std::mutex> l(m);
		sector = s;
		arrived = false;
		open = false;
	}
	void Open()
	{
		{
			std::lock_guard<std::mutex> l(m);
			open = true;
		}
		cv.notify_all();
	}
	bool WaitArrived(int ms)
	{
		std::unique_lock<std::mutex> l(m);
		return cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return arrived; });
	}
	// called by the drive for each read
	void Pass(u32 s)
	{
		std::unique_lock<std::mutex> l(m);
		if (s != sector || open)
			return;
		arrived = true;
		cv.notify_all();
		cv.wait_for(l, std::chrono::seconds(10), [&] { return open; }); // (a safety limit: a test that forgot to open the gate still ends)
	}
};

struct Call
{
	u32 sector, count;
	std::thread::id tid;
	clk::time_point at;
};

struct Scripted : FakeDrive
{
	std::mutex m;
	std::vector<Call> calls;
	std::atomic<bool> ready{true};
	std::atomic<int> ready_asked{0}; // how often the thread asked (IOCtlSrc::DiscReady: a TEST UNIT READY on the real drive)
	Gate gate;
	std::function<bool(u32 sector)> fail;  // return true: this read fails
	std::function<void(u32 sector)> delay; // may sleep
	bool Ready() override
	{
		ready_asked++;
		return ready.load();
	}
	bool Read2048(u32 sector, u32 count, u8* out) override
	{
		{
			std::lock_guard<std::mutex> l(m);
			calls.push_back({sector, count, std::this_thread::get_id(), clk::now()});
		}
		gate.Pass(sector);
		std::function<void(u32)> d;
		std::function<bool(u32)> f;
		{
			std::lock_guard<std::mutex> l(m);
			d = delay;
			f = fail;
		}
		if (d)
			d(sector);
		if (f && f(sector))
			return false;
		FillSectors(sector, count, out);
		return true;
	}
	std::vector<Call> Calls()
	{
		std::lock_guard<std::mutex> l(m);
		return calls;
	}
	size_t Count()
	{
		std::lock_guard<std::mutex> l(m);
		return calls.size();
	}
	void Clear()
	{
		std::lock_guard<std::mutex> l(m);
		calls.clear();
	}
	void SetFail(std::function<bool(u32)> f)
	{
		std::lock_guard<std::mutex> l(m);
		fail = std::move(f);
	}
	void SetDelay(std::function<void(u32)> d)
	{
		std::lock_guard<std::mutex> l(m);
		delay = std::move(d);
	}
};

static Scripted s_drive;
static std::atomic<int> s_new_disc_calls{0};

static void BeginRun(std::initializer_list<const char*> flags)
{
	SetFlags(flags);
	s_drive.Clear();
	s_drive.ready = true;
	s_drive.gate.Open();
	s_drive.SetFail(nullptr);
	s_drive.SetDelay(nullptr);
	s_new_disc_calls = 0;
	g_drive = &s_drive;
	newDiscCB = [] { s_new_disc_calls++; };
	src = std::make_unique<IOCtlSrc>("fake");
	src->Reopen(nullptr);
	disc_has_changed = false;
	cdvdStartThread();
}
static void EndRun()
{
	s_drive.gate.Open();
	cdvdStopThread();
	src.reset();
#ifndef REF_THREAD
	cdvdSetWaitCapMs(3000);
#endif
}

static bool WaitFor(const std::function<bool()>& pred, int ms)
{
	const auto until = clk::now() + std::chrono::milliseconds(ms);
	while (clk::now() < until)
	{
		if (pred())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return pred();
}
// The fake has seen no new read for `quiet_ms`.
static bool WaitQuiet(int quiet_ms, int limit_ms)
{
	const auto until = clk::now() + std::chrono::milliseconds(limit_ms);
	size_t seen = s_drive.Count();
	auto since = clk::now();
	while (clk::now() < until)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		const size_t n = s_drive.Count();
		if (n != seen)
		{
			seen = n;
			since = clk::now();
		}
		else if (MsSince(since) >= quiet_ms)
			return true;
	}
	return false;
}
static int CallsBy(const std::vector<Call>& calls, std::thread::id tid, bool equal)
{
	int n = 0;
	for (const Call& c : calls)
		n += (c.tid == tid) == equal;
	return n;
}
static bool Read(u32 sector, u32 count) // a game's reading: request, then the buffer, per sector
{
	bool ok = true;
	for (u32 i = 0; i < count; i++)
	{
		cdvdRequestSector(sector + i, 0);
		const u8* p = cdvdGetSector(sector + i, CDVD_MODE_2048);
		ok = ok && SectorMatches(sector + i, p);
	}
	return ok;
}

int main()
{
	const std::thread::id me = std::this_thread::get_id();

	// 1. the CPU thread never reads the drive: every read is the CDVD thread's, and the bytes are right
	{
		BeginRun({});
		s_drive.SetDelay([](u32) { std::this_thread::sleep_for(std::chrono::milliseconds(15)); });
		bool ok = true;
		for (const u32 at : {1008u, 1024u, 90000u, 90016u, 700000u})
			ok = Read(at, 16) && ok;
		const std::vector<Call> calls = s_drive.Calls();
		Check(ok, "slow drive: every sector the CPU thread got has the right bytes");
		Check(CallsBy(calls, me, true) == 0 && CallsBy(calls, me, false) > 0, "slow drive: no drive read was made on the CPU thread, all on the CDVD thread",
			std::to_string(calls.size()) + " reads");
		EndRun();
	}

	// 2. the wake-up: a request pushed while the last prefetch read runs is read at once when that read ends (the original thread, with nobody waiting
	//    when it was notified, went to sleep for 250 ms without looking at the queue)
	{
		BeginRun({});
		const u32 A = 160000, X = 800000;
		s_drive.gate.Arm(A + 16 * 16); // the 16th and last prefetch block after A
		cdvdRequestSector(A, 0);
		const bool parked = s_drive.gate.WaitArrived(3000);
		cdvdRequestSector(X, 0);
		const auto t0 = clk::now();
		s_drive.gate.Open();
		double latency = -1;
		const bool read = WaitFor(
			[&] {
				for (const Call& c : s_drive.Calls())
					if (c.sector == X)
					{
						latency = std::chrono::duration<double, std::milli>(c.at - t0).count();
						return true;
					}
				return false;
			},
			2000);
		Check(parked, "wake-up: the CDVD thread got to the last prefetch read");
		Check(read && latency < 60, "wake-up: a request pushed during the last prefetch read is read within 60 ms of its end", Num(latency, "ms (the original: ~250)"));
		EndRun();
	}

#ifndef REF_THREAD
	// 3. the default prefetch is unchanged: 16 blocks after a request that missed, then nothing
	{
		BeginRun({});
		cdvdRequestSector(48000, 0);
		WaitQuiet(150, 3000);
		const std::vector<Call> calls = s_drive.Calls();
		u32 max_sector = 0;
		for (const Call& c : calls)
			max_sector = std::max(max_sector, c.sector);
		Check(calls.size() == 17 && max_sector == 48000 + 16 * 16, "default prefetch: the request's block and 16 after it, nothing more", std::to_string(calls.size()) + " reads");
		EndRun();
	}

	// 4. the cap: the CDVD thread does not deliver in time, the CPU thread reads the block itself and gets the right bytes
	{
		BeginRun({});
		cdvdSetWaitCapMs(200);
		const u32 B = 64000;
		s_drive.SetDelay([&](u32 s) {
			if (s == B && std::this_thread::get_id() != me)
				std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // (the CDVD thread's read of B)
		});
		cdvdRequestSector(B, 0);
		const auto t0 = clk::now();
		const u8* p = cdvdGetSector(B + 3, CDVD_MODE_2048);
		const double ms = MsSince(t0);
		const bool right = SectorMatches(B + 3, p);
		bool own = false;
		for (const Call& c : s_drive.Calls())
			own = own || (c.sector == B && c.tid == me);
		Check(right && own, "cap: after the cap the CPU thread read the block itself, and the bytes are right");
		Check(ms > 150 && ms < 900, "cap: it waited about the cap (200 ms) and no longer", Num(ms, "ms"));
		EndRun();
	}

	// 5. the disc goes while the CPU thread waits: it gives up and returns within 100 ms
	{
		BeginRun({});
		const u32 P = 32000, B = 96000;
		s_drive.gate.Arm(P);
		cdvdRequestSector(P, 0);
		const bool parked = s_drive.gate.WaitArrived(3000);
		cdvdRequestSector(B, 0); // waits in the queue behind the parked read
		double ms = -1;
		bool right = false;
		std::thread waiter([&] {
			const auto t0 = clk::now();
			const u8* p = cdvdGetSector(B + 5, CDVD_MODE_2048);
			ms = MsSince(t0);
			right = SectorMatches(B + 5, p);
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(30)); // the waiter is waiting
		s_drive.ready = false;
		const auto t_release = clk::now();
		s_drive.gate.Open();
		waiter.join();
		const double since_release = MsSince(t_release);
		Check(parked, "disc gone: the CDVD thread was parked in a read");
		Check(right && since_release < 100, "disc gone: the waiting CPU thread returned within 100 ms of the CDVD thread noticing, with the right bytes",
			Num(since_release, "ms"));
		(void)ms;
		s_drive.ready = true;
		Check(WaitFor([] { return s_new_disc_calls.load() >= 2; }, 3000), "disc gone: tray open then closed again (two new-disc callbacks)");
		EndRun();
	}

	// 6. the queue is cleared by a disc change: the dropped request is asked for again at once, by the CPU thread's wait, not by its own read
	{
		BeginRun({});
		const u32 P = 32000, B = 96000;
		s_drive.gate.Arm(P);
		cdvdRequestSector(P, 0);
		s_drive.gate.WaitArrived(3000);
		cdvdRequestSector(B, 0); // queued behind the parked read
		s_drive.ready = false;
		s_drive.gate.Open();
		WaitFor([] { return s_new_disc_calls.load() >= 1; }, 3000);
		s_drive.ready = true;
		const bool back = WaitFor([] { return s_new_disc_calls.load() >= 2; }, 3000);
		bool served = false;
		for (const Call& c : s_drive.Calls())
			served = served || c.sector == B;
		Check(back && !served, "queue cleared: the request queued before the disc change was dropped, not read");
		s_drive.Clear();
		const auto t0 = clk::now();
		const u8* p = cdvdGetSector(B + 1, CDVD_MODE_2048);
		const double ms = MsSince(t0);
		const std::vector<Call> calls = s_drive.Calls();
		Check(SectorMatches(B + 1, p) && ms < 200 && CallsBy(calls, me, true) == 0, "queue cleared: the CPU thread's wait asks again and the CDVD thread delivers, no read by the CPU thread",
			Num(ms, "ms"));
		EndRun();
	}

	// 7. a block the CDVD thread cannot read: the waiting CPU thread is told, falls back to its own read, no hang; the failure is not remembered
	{
		BeginRun({});
		const u32 B = 128000;
		// the CDVD thread's reads of B fail (whenever it tries), the CPU thread's own read of B works
		s_drive.SetFail([&](u32 s) { return s == B && std::this_thread::get_id() != me; });
		cdvdRequestSector(B, 0);
		const auto t0 = clk::now();
		const u8* p = cdvdGetSector(B, CDVD_MODE_2048);
		const double ms = MsSince(t0);
		const std::vector<Call> calls = s_drive.Calls();
		Check(SectorMatches(B, p) && ms < 1000, "failed block: the CPU thread falls back to its own read and gets the right bytes, no hang", Num(ms, "ms"));
		Check(CallsBy(calls, me, true) >= 1, "failed block: ...and that read was the CPU thread's own");
		// the failure of an earlier wait does not count for a later one
		const u32 C = 144000;
		cdvdRequestSector(C, 0);
		const auto t1 = clk::now();
		const u8* q = cdvdGetSector(C, CDVD_MODE_2048);
		Check(SectorMatches(C, q) && MsSince(t1) < 500, "failed block: the next block is delivered by the CDVD thread as usual");
		// a drive that never reads B: no hang either
		s_drive.SetFail([&](u32 s) { return s == 160000; });
		cdvdRequestSector(160000, 0);
		const auto t2 = clk::now();
		cdvdGetSector(160000, CDVD_MODE_2048);
		Check(MsSince(t2) < 1000, "failed block: a block that never reads returns (stale bytes, as before) without a hang", Num(MsSince(t2), "ms"));
		EndRun();
	}

	// 8. discblock (the switch file): the CPU thread reads on its own thread, as in build 174
	{
		BeginRun({"discblock"});
		const u32 B = 192000;
		const u8* p = cdvdGetSector(B + 2, CDVD_MODE_2048);
		const std::vector<Call> calls = s_drive.Calls();
		bool own = false;
		for (const Call& c : calls)
			own = own || (c.sector == B && c.tid == me);
		Check(SectorMatches(B + 2, p) && own, "discblock: the CPU thread reads the block on its own thread");
		EndRun();
	}

	// 9. quiesce: no new drive read after it; the call waits for the read in flight, at most the time it is given; resume goes on
	{
		BeginRun({});
		const u32 Q = 256000, R = 512000;
		s_drive.gate.Arm(Q);
		cdvdRequestSector(Q, 0);
		s_drive.gate.WaitArrived(3000);
		const auto t0 = clk::now();
		cdvdQuiesce(100); // the read of Q is parked: waits the 100 ms and gives up
		const double ms = MsSince(t0);
		Check(ms >= 90 && ms < 600, "quiesce: with a read in flight the call waits for it, at most max_ms", Num(ms, "ms"));
		cdvdRequestSector(R, 0);
		s_drive.gate.Open();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		bool started = false;
		for (const Call& c : s_drive.Calls())
			started = started || (c.sector != Q);
		Check(!started, "quiesce: the read in flight ended and nothing else was started (no prefetch, no request)");
		const auto t1 = clk::now();
		cdvdResume();
		double lat = -1;
		const bool got = WaitFor(
			[&] {
				for (const Call& c : s_drive.Calls())
					if (c.sector == R)
					{
						lat = std::chrono::duration<double, std::milli>(c.at - t1).count();
						return true;
					}
				return false;
			},
			2000);
		Check(got && lat < 100, "quiesce: after resume the waiting request is read", Num(lat, "ms"));
		// nothing in flight: the call returns at once
		WaitQuiet(150, 3000);
		const auto t2 = clk::now();
		cdvdQuiesce(400);
		Check(MsSince(t2) < 50, "quiesce: with no read in flight it returns at once", Num(MsSince(t2), "ms"));
		// quiesced, the thread asks the drive nothing either (on the real drive DiscReady is a TEST UNIT READY, after a disc change a speed command): the app
		// is about to leave and promises the drive no command from here on
		std::this_thread::sleep_for(std::chrono::milliseconds(30)); // (an iteration that was under way when the flag went up ends)
		const int asked0 = s_drive.ready_asked.load();
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		const int asked_quiet = s_drive.ready_asked.load() - asked0;
		Check(asked_quiet == 0, "quiesce: the drive is not asked 'ready?' while quiesced (250 ms)", std::to_string(asked_quiet) + " asks");
		cdvdResume();
		Check(WaitFor([&] { return s_drive.ready_asked.load() > asked0; }, 1000), "quiesce: after resume the drive is asked again");
		EndRun();
	}

	// 10. the line every 10 s goes to boot.log while the disc is read (test-cdvdthread.sh looks for it in the output): 10.5 s of a game's reading
	{
		BeginRun({});
		std::printf("INFO reading for 10.5 s: the line '[disc] 10 s: ...' must appear\n");
		std::fflush(stdout);
		const auto t0 = clk::now();
		u32 at = 16000;
		bool ok = true;
		while (MsSince(t0) < 10500)
		{
			ok = Read(at, 16) && ok;
			at += 16;
			std::this_thread::sleep_for(std::chrono::milliseconds(40));
		}
		Check(ok, "10 s line: the sectors read meanwhile are right");
		EndRun();
	}

	// ------------------------------------------------------------------------------------------------------------------------ asynchronous reads (switch file discasync)
	// A1. cdvdTryGetSector answers -2 at once while the CDVD thread has the block, then 0 with the right bytes; the CPU thread makes no drive read
	{
		BeginRun({});
		Check(!cdvdAsyncOn(), "discasync: off without the switch file");
		EndRun();
		BeginRun({"discasync"});
		Check(cdvdAsyncOn(), "discasync: on with the switch file");
		const u32 B = 64000;
		s_drive.gate.Arm(B);
		cdvdRequestSector(B, 0);
		const bool parked = s_drive.gate.WaitArrived(3000);
		u8* p = nullptr;
		int minus2 = 0;
		const auto t0 = clk::now();
		for (int i = 0; i < 20; i++)
		{
			if (cdvdTryGetSector(B + 7, CDVD_MODE_2048, &p) == -2)
				minus2++;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		const double ms = MsSince(t0);
		s_drive.gate.Open();
		s32 r = -2;
		WaitFor([&] { return (r = cdvdTryGetSector(B + 7, CDVD_MODE_2048, &p)) == 0; }, 3000);
		Check(parked && minus2 == 20 && ms < 400, "discasync: while the CDVD thread has the block the answer is -2 each time, at once", std::to_string(minus2) + " of 20, " + Num(ms, "ms"));
		Check(r == 0 && SectorMatches(B + 7, p) && CallsBy(s_drive.Calls(), me, true) == 0, "discasync: then the sector, with the right bytes, and no drive read by the CPU thread");
		EndRun();
	}
	// A2. the cap: after 200 ms of -2 the block is read by the caller and the answer is 0
	{
		BeginRun({"discasync"});
		cdvdSetWaitCapMs(200);
		const u32 B = 64000;
		s_drive.SetDelay([&](u32 s) {
			if (s == B && std::this_thread::get_id() != me)
				std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		});
		cdvdRequestSector(B, 0);
		u8* p = nullptr;
		const auto t0 = clk::now();
		s32 r = -2;
		int minus2 = 0;
		while ((r = cdvdTryGetSector(B + 1, CDVD_MODE_2048, &p)) == -2 && MsSince(t0) < 3000)
		{
			minus2++;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const double ms = MsSince(t0);
		Check(r == 0 && SectorMatches(B + 1, p) && ms > 150 && ms < 900 && minus2 > 50, "discasync: after the cap the block is read by the caller, the answer is 0 with the right bytes",
			Num(ms, "ms, ") + std::to_string(minus2) + " answers of -2");
		EndRun();
	}
	// A3. the CDVD thread cannot read the block: -2 then the caller's own read
	{
		BeginRun({"discasync"});
		const u32 B = 128000;
		s_drive.SetFail([&](u32 s) { return s == B && std::this_thread::get_id() != me; });
		cdvdRequestSector(B, 0);
		u8* p = nullptr;
		const auto t0 = clk::now();
		s32 r = -2;
		while ((r = cdvdTryGetSector(B, CDVD_MODE_2048, &p)) == -2 && MsSince(t0) < 3000)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		Check(r == 0 && SectorMatches(B, p) && MsSince(t0) < 500, "discasync: a block the CDVD thread cannot read is read by the caller", Num(MsSince(t0), "ms"));
		EndRun();
	}
	// A4. the disc goes while the answer is -2: the caller does not wait for the cap
	{
		BeginRun({"discasync"});
		const u32 P = 32000, B = 96000;
		s_drive.gate.Arm(P);
		cdvdRequestSector(P, 0);
		s_drive.gate.WaitArrived(3000);
		cdvdRequestSector(B, 0);
		u8* p = nullptr;
		const s32 first = cdvdTryGetSector(B, CDVD_MODE_2048, &p);
		s_drive.ready = false;
		s_drive.gate.Open();
		const auto t0 = clk::now();
		s32 r = -2;
		while ((r = cdvdTryGetSector(B, CDVD_MODE_2048, &p)) == -2 && MsSince(t0) < 3000)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		Check(first == -2 && r == 0 && SectorMatches(B, p) && MsSince(t0) < 200, "discasync: the disc went: the caller reads the block itself within 200 ms", Num(MsSince(t0), "ms"));
		s_drive.ready = true;
		WaitFor([] { return s_new_disc_calls.load() >= 2; }, 3000);
		EndRun();
	}

	// ------------------------------------------------------------------------------------------------------------------------ the pipeline (switch file discpipe)
	// P1. the prefetch follows the emulation up to 24 blocks ahead and no further
	{
		BeginRun({"discpipe"});
		const u32 E = 320000;
		cdvdRequestSector(E, 0);
		WaitQuiet(150, 3000);
		const std::vector<Call> calls = s_drive.Calls();
		u32 max_sector = 0;
		for (const Call& c : calls)
			max_sector = std::max(max_sector, c.sector);
		Check(calls.size() == 24 && max_sector == E + 23 * 16, "pipeline: the request's block and the blocks up to 24 ahead of the emulation, no further",
			std::to_string(calls.size()) + " reads");
		// the emulation moves on through blocks that are in the cache: the pipeline goes on behind it
		for (u32 k = 1; k <= 20; k++)
			cdvdRequestSector(E + k * 16, 0);
		WaitQuiet(150, 3000);
		u32 max2 = 0;
		for (const Call& c : s_drive.Calls())
			max2 = std::max(max2, c.sector);
		Check(max2 == E + 20 * 16 + 23 * 16, "pipeline: the emulation 20 blocks on (all cached): the pipeline read up to 24 ahead of it again");
		EndRun();
	}
	// P2. a jump into a cached region far from the frontier does not make the thread read the way from the old frontier (a prefetch storm)
	{
		BeginRun({"discpipe"});
		const u32 R = 1000000, E = 100000;
		cdvdRequestSector(R, 0);
		WaitQuiet(150, 3000);
		cdvdRequestSector(E, 0);
		WaitQuiet(150, 3000);
		s_drive.Clear();
		cdvdRequestSector(R + 5 * 16, 0); // the emulation jumps back into the first region (cached): the frontier is at E + 24 blocks
		WaitQuiet(200, 5000);
		bool between = false;
		for (const Call& c : s_drive.Calls())
			between = between || (c.sector > E + 24 * 16 && c.sector < R);
		Check(!between, "pipeline: after a jump into a cached region no read between the old frontier and the region", std::to_string(s_drive.Count()) + " reads after the jump");
		EndRun();
	}
	// P3. a failed read stops the prefetch until the next request that missed
	{
		BeginRun({"discpipe"});
		const u32 E = 400000, F = E + 5 * 16;
		s_drive.SetFail([&](u32 s) { return s == F; });
		cdvdRequestSector(E, 0);
		WaitQuiet(200, 3000);
		u32 max_sector = 0;
		int f_reads = 0;
		for (const Call& c : s_drive.Calls())
		{
			max_sector = std::max(max_sector, c.sector);
			f_reads += c.sector == F;
		}
		Check(max_sector == F && f_reads == 2, "pipeline: a block that fails (two tries) stops the prefetch there", "last read " + std::to_string(max_sector));
		s_drive.SetFail(nullptr);
		const size_t before = s_drive.Count();
		cdvdRequestSector(E + 5000 * 16, 0);
		WaitQuiet(200, 3000);
		Check(s_drive.Count() > before + 5, "pipeline: the next request that missed starts it again");
		EndRun();
	}
	// P4. a disc change clears the queue (pipeline)
	{
		BeginRun({"discpipe"});
		const u32 P = 32000, B = 96000;
		s_drive.gate.Arm(P);
		cdvdRequestSector(P, 0);
		s_drive.gate.WaitArrived(3000);
		cdvdRequestSector(B, 0);
		s_drive.ready = false;
		s_drive.gate.Open();
		WaitFor([] { return s_new_disc_calls.load() >= 1; }, 3000);
		s_drive.ready = true;
		const bool back = WaitFor([] { return s_new_disc_calls.load() >= 2; }, 3000);
		WaitQuiet(150, 3000);
		bool served = false;
		for (const Call& c : s_drive.Calls())
			served = served || c.sector == B;
		Check(back && !served, "pipeline: the queue was cleared by the disc change, and the thread idles afterwards");
		const size_t before = s_drive.Count();
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		Check(s_drive.Count() == before, "pipeline: nothing is read once idle (no stale pipeline position, no spin)");
		EndRun();
	}
	// P5. the CPU thread's wait works with the pipeline too
	{
		BeginRun({"discpipe"});
		bool ok = true;
		for (const u32 at : {2000u * 16, 2000u * 16 + 16, 90000u, 700000u})
			ok = Read(at, 16) && ok;
		Check(ok && CallsBy(s_drive.Calls(), me, true) == 0, "pipeline: the CPU thread gets the right bytes and makes no drive read");
		EndRun();
	}
#endif

	std::printf("%s: %d failed\n", s_failed ? "FAILED" : "ALL PASSED", s_failed);
	return s_failed ? 1 : 0;
}
