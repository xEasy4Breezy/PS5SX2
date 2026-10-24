// PS5SX2 (2.11, AI-assisted): PC test of OrbisDiscReady.h, the rules for asking a disc drive "are you ready?" while a game plays from it. Pure logic,
// the time is a number that the test moves.
//
// The last section is a small model of IOCtlSrc::DiscReady (OrbisIOCtlSrc.cpp) built from the same calls, run against a drive that behaves as the
// console logs say: busy for 0.7 s after Sony's speed command, answering NOT READY (2/04) to a TEST UNIT READY in that time. It is a model of the glue,
// not the glue itself (that needs FreeBSD's headers); keep the two in step.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisDiscPlaying.h"
#include "OrbisDiscReadAhead.h"
#include "OrbisDiscReady.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

using orbis_disc::LastRead;
using orbis_disc::ReadAhead;
using orbis_disc::ReadyPolicy;

static int s_failed = 0;
static void Check(bool ok, const char* what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		s_failed++;
}

// Asks until `now` reaches `until` in steps of `step` ms, counting the questions.
static int CountAsks(ReadyPolicy& p, int64_t from, int64_t until, int64_t step, int64_t last_read)
{
	int asks = 0;
	for (int64_t t = from; t < until; t += step)
		if (p.ShouldAsk(t, last_read))
			asks++;
	return asks;
}

int main()
{
	// 1. idle: one question per 500 ms, however often DiscReady is called (the CDVD thread calls it every few ms)
	{
		ReadyPolicy p;
		p.Reset();
		const int asks = CountAsks(p, 10000, 20000, 5, -1);
		Check(asks == 20, "idle: 20 questions in 10 s (one per 500 ms) when called every 5 ms");
		ReadyPolicy q;
		Check(q.ShouldAsk(0, -1) && !q.ShouldAsk(499, -1) && q.ShouldAsk(500, -1), "idle: the first question at once, the next 500 ms on");
	}
	// 2. a read that reached the drive within 2 s proves it: no question
	{
		ReadyPolicy p;
		p.Reset();
		// reads every 1.5 s for 60 s
		int asks = 0;
		int64_t last = -1;
		for (int64_t t = 1000; t < 61000; t += 10)
		{
			if ((t - 1000) % 1500 == 0)
				last = t;
			if (p.ShouldAsk(t, last))
				asks++;
		}
		Check(asks == 0, "a read every 1.5 s for 60 s: the drive is never asked");
		// the proof ends after 2 s
		ReadyPolicy q;
		q.Reset();
		Check(!q.ShouldAsk(5000, 4500) && !q.ShouldAsk(6499, 4500) && q.ShouldAsk(6500, 4500), "a read 500 ms ago: no question until it is 2 s old, then one");
		// a time stamp from the future (a clock that went back) proves nothing
		ReadyPolicy r;
		r.Reset();
		Check(r.ShouldAsk(5000, 9000), "a read 'in the future' is not a proof");
		// no read yet (-1): asks
		ReadyPolicy s;
		s.Reset();
		Check(s.ShouldAsk(0, -1), "no read yet: asks");
	}
	// 3. the settle window: nothing asked for 3 s after the speed command, then the questions go on
	{
		ReadyPolicy p;
		p.Reset();
		p.StartSettle(10000);
		Check(p.Settling(10000) && p.Settling(12999) && !p.Settling(13000), "settle: 3000 ms");
		Check(CountAsks(p, 10000, 13000, 10, -1) == 0, "settle: no question in the 3 s after the speed command");
		Check(p.ShouldAsk(13000, -1), "settle: a question when the window ends");
		// the reads in the window do not matter, the settle wins
		ReadyPolicy q;
		q.Reset();
		q.StartSettle(0);
		Check(CountAsks(q, 0, 3000, 1, -1) == 0, "settle: still none when asked every ms");
		// Reset (a new disc) forgets the window
		q.Reset();
		Check(q.ShouldAsk(100, -1), "Reset clears the settle window");
		// Reopened keeps it (Reopen sets it, DiscReady then notes the time)
		ReadyPolicy r;
		r.Reset();
		r.StartSettle(1000);
		r.Reopened(1100);
		Check(!r.ShouldAsk(2000, -1) && !r.ShouldAsk(3999, -1) && r.ShouldAsk(4000, -1), "Reopened keeps the settle window");
	}
	// 4. one 2/04 (becoming ready) is not an eject: it keeps the disc and asks again 250 ms on; the second in a row opens
	{
		ReadyPolicy p;
		p.Reset();
		Check(p.ShouldAsk(1000, -1), "2/04: asked");
		ReadyPolicy::Verdict v = p.Answer(false, true, 2, 0x04);
		Check(v.ready && !v.out && v.doubt, "2/04 the first time: the disc stays (doubt)");
		Check(!p.ShouldAsk(1100, -1) && !p.ShouldAsk(1249, -1) && p.ShouldAsk(1250, -1), "2/04: the next question comes 250 ms later (not 500)");
		v = p.Answer(false, true, 2, 0x04);
		Check(v.out && !v.changed && !v.ready, "2/04 the second time in a row: the tray opens");
		// after opening, the normal rate again
		Check(!p.ShouldAsk(1300, -1) && p.ShouldAsk(1750, -1), "2/04 twice: back to a question every 500 ms");
	}
	// 5. a good answer between two doubtful ones resets the count
	{
		ReadyPolicy p;
		p.Reset();
		p.ShouldAsk(0, -1);
		p.Answer(false, true, 2, 0x04);
		p.ShouldAsk(250, -1);
		ReadyPolicy::Verdict v = p.Answer(true, false, 0, 0);
		Check(v.ready && !v.out && !v.doubt, "ready after a doubt: fine");
		Check(!p.ShouldAsk(300, -1) && p.ShouldAsk(750, -1), "ready after a doubt: back to 500 ms");
		v = p.Answer(false, true, 2, 0x04);
		Check(v.ready && v.doubt && !v.out, "a doubt after a good answer is a first doubt again");
	}
	// 6. a read between two doubtful answers resets the count too (it proved the disc)
	{
		ReadyPolicy p;
		p.Reset();
		p.ShouldAsk(0, -1);
		p.Answer(false, true, 2, 0x04);
		Check(!p.ShouldAsk(300, 200), "a read 100 ms ago after a doubt: no question");
		p.ShouldAsk(5000, -1);
		ReadyPolicy::Verdict v = p.Answer(false, true, 2, 0x04);
		Check(v.doubt && !v.out, "the doubt before the read was forgotten: the next one is a first again");
	}
	// 7. no medium (2/3A) opens at once, so does a UNIT ATTENTION, and neither is softened
	{
		ReadyPolicy p;
		p.Reset();
		p.ShouldAsk(0, -1);
		ReadyPolicy::Verdict v = p.Answer(false, true, 2, 0x3A);
		Check(v.out && !v.changed && !v.ready && !v.doubt, "2/3A: out at once");
		ReadyPolicy q;
		q.Reset();
		q.ShouldAsk(0, -1);
		v = q.Answer(false, true, 6, 0x29);
		Check(v.out && v.changed && !v.ready && !v.doubt, "UNIT ATTENTION 6/29: out at once, reported as a change");
		ReadyPolicy r;
		r.Reset();
		r.ShouldAsk(0, -1);
		v = r.Answer(false, true, 6, 0x2A);
		Check(v.out && v.changed, "UNIT ATTENTION 6/2A: out at once, reported as a change");
		// a UNIT ATTENTION is a change whatever else is going on: after a doubt, in the settle window's aftermath
		ReadyPolicy s;
		s.Reset();
		s.ShouldAsk(0, -1);
		s.Answer(false, true, 2, 0x04);
		s.ShouldAsk(250, -1);
		v = s.Answer(false, true, 6, 0x29);
		Check(v.out && v.changed, "UNIT ATTENTION after a doubt: still a change");
		// the answer has no notion of a disc's size: nothing reinterprets it
		Check(!v.ready, "UNIT ATTENTION: never taken for the same disc");
	}
	// 8. a command that did not get through (no sense data) counts as a doubt, twice opens
	{
		ReadyPolicy p;
		p.Reset();
		p.ShouldAsk(0, -1);
		ReadyPolicy::Verdict v = p.Answer(false, false, 0, 0);
		Check(v.doubt && !v.out, "no sense data once: a doubt");
		p.ShouldAsk(250, -1);
		v = p.Answer(false, false, 0, 0);
		Check(v.out && !v.changed, "no sense data twice: out");
	}
	// 9. a different "not ready" (key 2, another code; 3/xx medium error; 5/xx) counts as a doubt, not as 'no medium'
	{
		ReadyPolicy p;
		p.Reset();
		p.ShouldAsk(0, -1);
		ReadyPolicy::Verdict v = p.Answer(false, true, 2, 0x3E); // logical unit has not self-configured
		Check(v.doubt && !v.out, "2/3E: a doubt, not an eject");
		v = p.Answer(false, true, 3, 0x11);
		Check(v.out, "a second doubtful answer of any kind: out");
	}
	// 10. out clears the settle window (a disc that went out is not 'settling')
	{
		ReadyPolicy p;
		p.Reset();
		p.StartSettle(0);
		p.Answer(false, true, 2, 0x3A);
		Check(!p.Settling(1000), "out clears the settle window");
	}
	// 10b. the speed change reported late: the first UNIT ATTENTION after the app's own speed command is the drive's, one time only
	{
		ReadyPolicy p;
		p.Reset();
		p.StartSettle(0);
		Check(p.ShouldAsk(3000, -1), "own UA: the first question comes when the settle window ends");
		ReadyPolicy::Verdict v = p.Answer(false, true, 6, 0x2A);
		Check(v.ready && v.doubt && v.own_ua && !v.out && !v.changed, "own UA: the first UNIT ATTENTION after the speed command is taken, the disc stays");
		Check(!p.ShouldAsk(3100, -1) && !p.ShouldAsk(3249, -1) && p.ShouldAsk(3250, -1), "own UA: asked again 250 ms on");
		v = p.Answer(true, false, 0, 0);
		Check(v.ready && !v.doubt && !v.out, "own UA: then the drive is ready");
		p.ShouldAsk(3750, -1);
		v = p.Answer(false, true, 6, 0x28);
		Check(v.out && v.changed && !v.own_ua, "own UA: only the first one: the next UNIT ATTENTION is a disc change");

		ReadyPolicy q; // no speed command, no exception
		q.Reset();
		q.ShouldAsk(0, -1);
		v = q.Answer(false, true, 6, 0x28);
		Check(v.out && v.changed && !v.own_ua, "own UA: without a speed command a UNIT ATTENTION is a change");

		ReadyPolicy r; // the first question was answered ready: the expectation is over
		r.Reset();
		r.StartSettle(0);
		r.ShouldAsk(3000, -1);
		r.Answer(true, false, 0, 0);
		r.ShouldAsk(3500, -1);
		v = r.Answer(false, true, 6, 0x28);
		Check(v.out && v.changed && !v.own_ua, "own UA: after a 'ready' as the first answer, a UNIT ATTENTION is a change");

		ReadyPolicy s; // a 2/04 first keeps the expectation (the speed change was still going on)
		s.Reset();
		s.StartSettle(0);
		s.ShouldAsk(3000, -1);
		s.Answer(false, true, 2, 0x04);
		s.ShouldAsk(3250, -1);
		v = s.Answer(false, true, 6, 0x28);
		Check(v.doubt && v.own_ua && !v.out, "own UA: a 2/04 first and then the UNIT ATTENTION is still the drive's own");

		ReadyPolicy t; // Reset (a new disc is being opened) forgets it
		t.Reset();
		t.StartSettle(0);
		t.Reset();
		t.ShouldAsk(0, -1);
		v = t.Answer(false, true, 6, 0x28);
		Check(v.out && v.changed && !v.own_ua, "own UA: Reset forgets the expectation");

		ReadyPolicy u; // 'no medium' ends it: what comes after is a new disc
		u.Reset();
		u.StartSettle(0);
		u.ShouldAsk(3000, -1);
		u.Answer(false, true, 2, 0x3A);
		u.ShouldAsk(3500, -1);
		v = u.Answer(false, true, 6, 0x28);
		Check(v.out && v.changed && !v.own_ua, "own UA: after 'no medium' the expectation is gone");
	}
	// 11. window hits do not extend the proof: a read served from a read-ahead window never reached the drive
	{
		ReadAhead ra;
		LastRead last;
		int64_t clock = 0;
		uint32_t drive_reads = 0;
		auto read = [&](uint32_t, uint32_t, uint8_t*) {
			drive_reads++;
			last.Note(clock); // as Pread2048 does when the drive delivered
			return true;
		};
		std::vector<uint8_t> buf(16 * 2048);
		clock = 1000;
		ra.Read(5000, 16, buf.data(), 100000, read); // a miss: the first window, from the drive
		Check(drive_reads == 1 && last.Get() == 1000, "a window read from the drive notes the time");
		// 3 s of 16-sector blocks served from the window (64 sectors = 4 blocks only, so use the whole 64: 4 hits)
		clock = 1500;
		ra.Read(5016, 16, buf.data(), 100000, read);
		clock = 2000;
		ra.Read(5032, 16, buf.data(), 100000, read);
		clock = 2500;
		ra.Read(5048, 16, buf.data(), 100000, read);
		Check(drive_reads == 1 && last.Get() == 1000, "three hits in the window: the drive was not read, the time is the old one");
		ReadyPolicy p;
		p.Reset();
		Check(p.ShouldAsk(3100, last.Get()), "window hits do not extend the proof: 2.1 s after the real read the drive is asked");
		last.Clear();
		Check(last.Get() == -1, "LastRead::Clear");
	}
	// 12. the glue, as a model: 60 s of DiscReady every 100 ms while a game reads every 200 ms never says 'out', with a drive that answers 2/04 for 0.7 s after
	//     the speed command; and a real eject is seen within about 3.5 s
	{
		struct Model
		{
			ReadyPolicy policy;
			LastRead last;
			bool ready = false;     // IOCtlSrc::m_ready
			bool have = true;       // m_sectors != 0
			int asks = 0;
			int outs = 0;
			int64_t busy_until = 0; // the drive answers 2/04 until then
			bool ejected = false;
			int64_t out_at = -1;
			bool ua_next = false; // the next answer is a UNIT ATTENTION (6/2A)

			void Reopen(int64_t now, bool speed_command)
			{
				policy.Reset();
				last.Clear();
				have = true;
				ready = true;
				if (speed_command)
				{
					busy_until = now + 700;
					policy.StartSettle(now);
				}
			}
			bool DiscReady(int64_t now)
			{
				const int64_t proof = have && ready ? last.Get() : -1;
				if (!policy.ShouldAsk(now, proof))
					return ready && have;
				asks++;
				bool ok = !ejected && now >= busy_until;
				uint8_t key = 2;
				uint8_t asc = ejected ? 0x3A : 0x04; // 2/3A: no medium; 2/04: becoming ready
				if (ua_next) // the speed change reported late: a UNIT ATTENTION as the answer to the next question
				{
					ua_next = false;
					ok = false;
					key = 6;
					asc = 0x2A;
				}
				const ReadyPolicy::Verdict v = policy.Answer(ok, true, key, asc);
				if (v.out)
				{
					outs++;
					if (out_at < 0)
						out_at = now;
					ready = false;
					have = false;
					last.Clear();
					return false;
				}
				if (v.doubt)
					return ready && have;
				ready = true;
				return have;
			}
		};
		{
			Model m;
			m.Reopen(0, true);
			bool never_false = true;
			for (int64_t t = 0; t < 60000; t += 100)
			{
				if (t % 200 == 0 && !m.ejected)
					m.last.Note(t); // a real read reached the drive
				never_false &= m.DiscReady(t);
			}
			Check(never_false && m.outs == 0, "60 s with reads every 200 ms and DiscReady every 100 ms: never out");
			Check(m.asks == 0, "...and the drive was never asked (the reads proved it)");
		}
		{
			// the same with nothing read (a loading screen that waits): the first question comes after the settle window, when the drive is ready again
			Model m;
			m.Reopen(0, true);
			bool never_false = true;
			for (int64_t t = 0; t < 60000; t += 100)
				never_false &= m.DiscReady(t);
			Check(never_false && m.outs == 0, "60 s idle after a speed command: never out");
			Check(m.asks >= 100 && m.asks <= 120, "...asked about every 500 ms once the settle window was over");
		}
		{
			// the speed change reported late, as a UNIT ATTENTION at the first question after the settle window (nothing was read to take it): the drive's own
			Model m;
			m.Reopen(0, true);
			m.ua_next = true;
			bool never_false = true;
			for (int64_t t = 0; t < 60000; t += 100)
				never_false &= m.DiscReady(t);
			Check(never_false && m.outs == 0, "60 s idle after a speed command that reports a late UNIT ATTENTION: never out");
			// only the first one: a second UNIT ATTENTION later is a disc change
			m.ua_next = true;
			for (int64_t t = 60000; t < 65000; t += 100)
				m.DiscReady(t);
			Check(m.outs == 1, "...and a second UNIT ATTENTION later is a disc change (out)");
		}
		{
			// what the settle window is for: the same drive without it (asked at 0 and 250 ms, both inside the 0.7 s) opens the tray even with two strikes
			Model m;
			m.Reopen(0, false);
			m.busy_until = 700;
			for (int64_t t = 0; t < 5000; t += 100)
				m.DiscReady(t);
			Check(m.outs == 1, "a drive busy for 0.7 s and no settle window: two strikes 250 ms apart are not enough (so the window is there)");
		}
		{
			// a real eject while a game reads every 200 ms: seen within 2 s (proof) + 0.5 s + 0.25 s
			Model m;
			m.Reopen(0, true);
			int64_t eject_at = 10000;
			for (int64_t t = 0; t < 20000; t += 100)
			{
				if (t == eject_at)
					m.ejected = true;
				if (t % 200 == 0 && !m.ejected)
					m.last.Note(t);
				m.DiscReady(t);
			}
			Check(m.outs >= 1 && m.out_at >= eject_at && m.out_at - eject_at <= 3500, "a real eject during play: out within 3.5 s");
		}
		{
			// 2/3A at once, no strikes: with nothing read, the eject is seen at the next question
			Model m;
			m.Reopen(0, false);
			int64_t eject_at = 10000;
			for (int64_t t = 0; t < 20000; t += 100)
			{
				if (t == eject_at)
					m.ejected = true;
				m.DiscReady(t);
			}
			Check(m.outs >= 1 && m.out_at - eject_at <= 500, "a real eject with nothing being read: out within 500 ms");
		}
	}

	// 13. logs/disc-playing.txt, the note for the disc helper (OrbisDiscPlaying.h): the exact line, written whole through a temporary file, replaced in place
	{
		Check(orbis_disc::PlayingLine(1760611200, "/dev/cd0") == "1760611200 /dev/cd0 play\n", "playing note: the line is '<unix time> <device> play\\n'");
		long long when = 0;
		std::string dev;
		Check(orbis_disc::ParsePlayingLine("1760611200 /dev/cd1 play\n", &when, &dev) && when == 1760611200 && dev == "/dev/cd1", "playing note: the line parses back");
		Check(!orbis_disc::ParsePlayingLine("1760611200 /dev/cd1 pla", nullptr, nullptr) && !orbis_disc::ParsePlayingLine("17606x 1 /dev/cd1 play\n", nullptr, nullptr) &&
				!orbis_disc::ParsePlayingLine("play\n", nullptr, nullptr) && !orbis_disc::ParsePlayingLine("1760611200  play\n", nullptr, nullptr) &&
				!orbis_disc::ParsePlayingLine("", nullptr, nullptr),
			"playing note: half a line or a different line does not parse");
		char tmpl[] = "/tmp/discplayingXXXXXX";
		const char* dir = mkdtemp(tmpl);
		Check(dir != nullptr, "playing note: a temporary folder");
		if (dir)
		{
			const std::string path = std::string(dir) + "/disc-playing.txt";
			Check(orbis_disc::WritePlayingStamp(path, 1000, "/dev/cd0"), "playing note: written");
			std::ifstream in(path);
			std::stringstream ss;
			ss << in.rdbuf();
			Check(ss.str() == "1000 /dev/cd0 play\n", "playing note: the file holds exactly the line");
			struct stat st;
			Check(stat((path + ".tmp").c_str(), &st) != 0, "playing note: no temporary file is left behind");
			Check(orbis_disc::WritePlayingStamp(path, 1002, "/dev/cd0"), "playing note: written again (every 2 s)");
			std::ifstream in2(path);
			std::stringstream ss2;
			ss2 << in2.rdbuf();
			Check(ss2.str() == "1002 /dev/cd0 play\n" && stat((path + ".tmp").c_str(), &st) != 0, "playing note: replaced in place, still no temporary file");
			Check(!orbis_disc::WritePlayingStamp(std::string(dir) + "/no-such-folder/disc-playing.txt", 1, "/dev/cd0"), "playing note: a folder that is not there is a no, not a crash");
			unlink(path.c_str());
			Check(stat(path.c_str(), &st) != 0, "playing note: removed");
			rmdir(dir);
		}
	}

	std::printf("%s: %d failed\n", s_failed ? "FAILED" : "ALL PASSED", s_failed);
	return s_failed ? 1 : 0;
}
