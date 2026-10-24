// PS5SX2 (2.11, AI-assisted): when a disc drive is asked "are you ready?" (TEST UNIT READY) while a game plays from it, and what is made of the answer.
//
// Why (disc lag round, build 174): DiscReady is what tells PCSX2 that the tray is open. Any answer but "ready" used to wipe the disc's size and both
// read caches and open the tray (the game sees an eject, the CDVD thread reopens the drive and sends the speed command again). Three things can fake
// that on the PS5's own drive now that it runs at 3.2x by default:
//   - the drive needs ~0.7 s to change speed (the first read after Sony's DB 00 00 32 took 497-710 ms in the disc drive test logs); a TEST UNIT READY
//     in that time may answer NOT READY (2/04, becoming ready). So no TEST UNIT READY is sent for kSettleMs after the speed command;
//   - one odd answer is not an eject. A "not ready" that is neither "no medium" (2/3A) nor a UNIT ATTENTION must come twice in a row (kStrikes, the
//     second kRetryMs after the first) before the tray opens. A real eject answers 2/3A and a real disc change answers UNIT ATTENTION: both act at
//     once, as before. A UNIT ATTENTION is never reinterpreted (two discs of the same size swapped in the drive are two discs), with one exception: the
//     drive may report the speed change as a UNIT ATTENTION later than IOCtlSrc::Reopen looks for it (which takes the ones that are there at once), at the
//     first TEST UNIT READY after the command. That one answer, and only that one (any other first answer ends the expectation), is the drive's own: asked
//     again kRetryMs on, like a doubt. A person cannot swap discs between a Reopen and the first question after it (a swap shows "no medium" first);
//   - a read that reached the drive in the last kProofMs proves that the disc is there and spinning: nothing is asked then. Only reads that really
//     went to the drive count (LastRead::Note is called from the drive read, not from the read-ahead windows' memory hits).
//
// Pure logic (no drive, no clock of its own: the caller passes the time in milliseconds) so the PC test (tests/discready) can check every rule.
// Used under IOCtlSrc's lock (OrbisIOCtlSrc.cpp).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>

namespace orbis_disc
{
// The time of the last read that reached the drive. One per process (OrbisIOCtlSrc.cpp): DiscReady and the keep-alive thread both use it.
class LastRead
{
public:
	void Note(int64_t now_ms) { m_ms.store(now_ms, std::memory_order_relaxed); }
	void Clear() { m_ms.store(-1, std::memory_order_relaxed); }
	// -1: no read yet (or the disc was just swapped).
	int64_t Get() const { return m_ms.load(std::memory_order_relaxed); }

private:
	std::atomic<int64_t> m_ms{-1};
};

class ReadyPolicy
{
public:
	static constexpr int64_t kEveryMs = 500;  // asked at most this often
	static constexpr int64_t kRetryMs = 250;  // after a first doubtful answer
	static constexpr int64_t kProofMs = 2000; // a read that reached the drive this recently proves it is ready
	static constexpr int64_t kSettleMs = 3000; // nothing is asked this long after the speed command
	static constexpr int kStrikes = 2;        // doubtful answers in a row that open the tray

	// What DiscReady does with an answer.
	struct Verdict
	{
		bool ready = false;   // the disc stays: the drive said ready, or said something doubtful for the first time (doubt)
		bool out = false;     // the disc is out (or changed): the caller resets the disc's data and opens the tray
		bool changed = false; // out because of a UNIT ATTENTION (a disc change or a drive reset), not "no medium"
		bool doubt = false;   // not ready, but not taken for an eject yet: the same answer again in kRetryMs will be
		bool own_ua = false;  // (with doubt) the UNIT ATTENTION that follows the app's own speed command, taken for the drive's, not a disc change
	};

	// A new disc is being opened (IOCtlSrc::Reopen): nothing of the old one counts.
	void Reset()
	{
		m_checked = -1;
		m_gap = kEveryMs;
		m_fails = 0;
		m_settle_until = 0;
		m_expect_ua = false;
	}

	// Reopen ran at `now_ms` (from DiscReady): the next question is kEveryMs on. The settle window set by Reopen is kept.
	void Reopened(int64_t now_ms)
	{
		m_checked = now_ms;
		m_gap = kEveryMs;
		m_fails = 0;
	}

	// The speed command was just taken: no TEST UNIT READY for kSettleMs, and the first UNIT ATTENTION to come is the drive's own (see the top).
	void StartSettle(int64_t now_ms)
	{
		m_settle_until = now_ms + kSettleMs;
		m_expect_ua = true;
	}
	bool Settling(int64_t now_ms) const { return now_ms < m_settle_until; }

	// True: send TEST UNIT READY now (the time is noted). False: go on with the last answer. `last_read_ms`: the time of the last read that reached the
	// drive, -1 for none, or when no disc is known to be ready (a read of an older disc proves nothing).
	bool ShouldAsk(int64_t now_ms, int64_t last_read_ms)
	{
		if (now_ms < m_settle_until)
			return false;
		if (m_checked >= 0 && now_ms - m_checked < m_gap)
			return false;
		if (last_read_ms >= 0 && now_ms >= last_read_ms && now_ms - last_read_ms < kProofMs)
		{
			m_fails = 0;
			m_gap = kEveryMs;
			return false;
		}
		m_checked = now_ms;
		return true;
	}

	// The drive's answer to the question: `ready`, else its sense (`sense_valid` false: the command did not reach the drive, or came back short).
	Verdict Answer(bool ready, bool sense_valid, uint8_t key, uint8_t asc)
	{
		Verdict v;
		if (ready)
		{
			m_fails = 0;
			m_gap = kEveryMs;
			m_expect_ua = false; // (the drive answered the first question after its speed command without one: a later one is a change)
			v.ready = true;
			return v;
		}
		if (sense_valid && key == 6) // UNIT ATTENTION
		{
			if (m_expect_ua) // the speed change, reported late: taken, the next question (kRetryMs on) finds the drive ready
			{
				m_expect_ua = false;
				m_gap = kRetryMs;
				v.ready = true;
				v.doubt = true;
				v.own_ua = true;
				return v;
			}
			Out();
			v.out = v.changed = true;
			return v;
		}
		if (sense_valid && key == 2 && asc == 0x3A) // NOT READY, medium not present
		{
			Out();
			v.out = true;
			return v;
		}
		if (++m_fails >= kStrikes)
		{
			Out();
			v.out = true;
			return v;
		}
		m_gap = kRetryMs; // (the time of this question is m_checked: the next one is kRetryMs after it)
		v.ready = true;   // for now the disc stays
		v.doubt = true;
		return v;
	}

private:
	void Out()
	{
		m_fails = 0;
		m_gap = kEveryMs;
		m_settle_until = 0;
		m_expect_ua = false;
	}

	int64_t m_checked = -1;
	int64_t m_gap = kEveryMs;
	int64_t m_settle_until = 0;
	int m_fails = 0;
	bool m_expect_ua = false; // a speed command was taken and no question has been answered since
};
} // namespace orbis_disc
