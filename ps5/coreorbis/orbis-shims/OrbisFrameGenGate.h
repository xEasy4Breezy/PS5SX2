// PS5SX2 (2.11, AI-assisted): the first gate that keeps frame generation from generating, for the "[fg] status" line.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
//
// swordpdf on build 175: "framegen at 50fps doesn't seem to work (PAL games should be taken to 100fps?)". They should, on a 119.88 Hz
// output: the pacing (OrbisFrameGenPacing.h) counts refreshes = PS2 vsyncs x display Hz / PS2 Hz and knows no region (a steady 50 fps
// game has 2.40 of them there, a 60 fps one 2.00). So "does nothing" has to be one of the gates below, and boot.log said which only for
// the ones the pacing itself knows (the 5 s state line), not for a display that stayed at 59.94 Hz, a window of duplicate skips or
// throttled presents, a failed interpolator or a game whose frames are not steady for most of the time.
//
// Between "Frame generation: On" and a generated frame on the screen there are these gates, in the order the code meets them
// (GSRenderer.cpp VSync -> GSDeviceVK.cpp OrbisFrameGenRecord -> OrbisFrameGenPacing.h):
//   the setting is read at the game start (main-boot.cpp: "[boot] frame generation: on/off", not a gate of this file: the status line
//   only exists while it is on), then per vsync:
//     1. the vsync presents at all (not skipped: "Skip duplicate frames" and the repeats frame generation itself skips, or
//        the present throttle)                                                                          -> duplicate-skipped, present-throttled
//     2. the picture is not blank                                                                       -> blank-picture
//     3. the picture is a new frame of the game (PCSX2's internal-FPS heuristic)                        -> no-new-frames
//     4. the interpolator could start, the device has a swapchain                                       -> engine-failed, no-swapchain
//     5. the pacing: 8 frames first, 1.97 refreshes a frame, a steady frame length, the limiter at
//        normal speed, no pause after a slowdown                                                        -> warming-up, no-room,
//                                                                                                          unsteady, limiter-not-nominal, paused
// The status line says which of them failed most often in the last 10 s (not a point sample of the pacing: a window that was 30%
// unsteady and 70% generating says so), with the launch path and the display rate's raw readings next to it, so a log answers "why does
// frame generation do nothing here" without a guess. A pure header, so a PC test runs it (tests/fgpacing/test_fg_pacing.cpp).

#pragma once

#include "OrbisDisplayHz.h"
#include "OrbisFrameGenPacing.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace orbis_fg
{
	enum class Gate
	{
		None,             // nothing fails: frames are generated
		DuplicateSkipped, // the vsyncs of the window skipped presenting because the picture was the game's last frame again
		PresentThrottled, // the vsyncs of the window skipped presenting for the present throttle
		BlankPicture,     // the vsyncs of the window had a blank picture (no display circuit enabled)
		NoNewFrames,      // the pictures are not counted as new frames of the game (registers_written / DISPFB blits say "no")
		EngineFailed,     // the interpolator could not start in this game (the "[fg] frame generation could not start" line says why)
		NoSwapchain,      // the device has no swapchain
		Warming,          // the start, or a load or a pause: 8 frames first
		NoRoom,           // fewer than 1.97 display refreshes a game frame (a 60 or 50 fps game on a 60 Hz output)
		Unsteady,         // the game's frames are not of one length
		NotNominal,       // the frame limiter is not at normal speed (or a video is being captured)
		Paused,           // the safety net's pause after a slowdown
		NothingShown,     // the pacing generates, but not one generated frame was presented in the window (review: "none" would hide it)
	};

	inline const char* GateName(Gate g)
	{
		switch (g)
		{
			case Gate::None: return "none";
			case Gate::DuplicateSkipped: return "duplicate-skipped";
			case Gate::PresentThrottled: return "present-throttled";
			case Gate::BlankPicture: return "blank-picture";
			case Gate::NoNewFrames: return "no-new-frames";
			case Gate::EngineFailed: return "engine-failed";
			case Gate::NoSwapchain: return "no-swapchain";
			case Gate::Warming: return "warming-up";
			case Gate::NoRoom: return "no-room";
			case Gate::Unsteady: return "unsteady-frame-length";
			case Gate::NotNominal: return "limiter-not-nominal";
			case Gate::Paused: return "paused-after-slowdown";
			case Gate::NothingShown: return "no-generated-frame";
		}
		return "?";
	}

	constexpr int kStates = 6; // the pacing's State values (OrbisFrameGenPacing.h), counted a game frame at a time
	// Window::by_state is indexed by State: a seventh state in the pacing must grow kStates with it, or the counts write past the array.
	static_assert(static_cast<int>(State::Generating) == kStates - 1, "State::Generating must be the last pacing state: kStates counts them");

	// The gate of a pacing state that is not Generating.
	inline Gate GateOfState(State s)
	{
		switch (s)
		{
			case State::Warming: return Gate::Warming;
			case State::NoRoom: return Gate::NoRoom;
			case State::Unsteady: return Gate::Unsteady;
			case State::NotNominal: return Gate::NotNominal;
			case State::Paused: return Gate::Paused;
			case State::Generating: break;
		}
		return Gate::None;
	}

	// What the GS thread saw in the last window (10 s) of vsyncs. Every vsync lands in exactly one counter of the first six.
	struct Window
	{
		uint32_t dup_skipped = 0; // vsyncs that skipped presenting: the picture repeated the game's last frame (GSRenderer.cpp skip_frame)
		uint32_t throttled = 0;   // ...that skipped presenting for the present throttle (ShouldSkipPresentingFrame)
		uint32_t blank = 0;       // ...with a blank picture
		uint32_t not_new = 0;     // ...whose picture was not counted as a new frame of the game
		uint32_t unserved = 0;    // ...new frames the generator could not take (it failed to start, or there is no swapchain)
		uint32_t recorded = 0;    // ...new frames handed to the pacing
		uint32_t engaged = 0;     // ...of those, with the pacing generating
		uint32_t by_state[kStates] = {}; // ...of those, the pacing's state at each (State as an index)
		uint32_t made = 0;        // generated frames presented
		uint32_t presents = 0;    // their presents
		uint32_t counted = 0;     // game frames whose length (in PS2 vsyncs) is in vsync_sum (a load's wait is not a frame)
		double vsync_sum = 0.0;
		double seconds = 0.0;

		// A new frame given to the pacing, which found itself in `state`.
		void Record(State state)
		{
			recorded++;
			const int index = static_cast<int>(state);
			if (index >= 0 && index < kStates)
				by_state[index]++;
			engaged += state == State::Generating ? 1 : 0;
		}
		uint32_t Vsyncs() const { return dup_skipped + throttled + blank + not_new + unserved + recorded; }
	};

	struct StatusInputs
	{
		Window w;
		bool engine_failed = false;
		bool swapchain = true;
		double ps2_hz = 59.94;
		double refreshes = 0.0;    // a game frame's refreshes at the display's rate (Pacing::Refreshes)
		uint32_t steady = 0;       // Pacing::SteadyFrames
		uint32_t warm_frames = 0;  // Pacing::WarmFrames
		DisplayHz display;
		uint32_t driver_mhz = 0;   // what the driver's hook said (0: none)
		uint32_t mode_mhz = 0;     // the mode the swapchain was made for (0: none listed)
		double speed = 100.0;      // percent of the target speed
		std::string launch;        // "main app" or "disc tile /dev/cd0": the title the console started decides the 120 Hz output
		const char* videoout = ""; // OrbisVideoOutSummary(), when the app's own switch is in play (the driver has none)
		std::string gpu;           // "; GPU 3.10 ms a frame ..." (or empty)
	};

	// Generating for 90% of the new frames or more.
	inline bool MostlyEngaged(const Window& w)
	{
		return w.recorded != 0 && w.engaged * 10 >= w.recorded * 9;
	}

	// engaged: "yes" (MostlyEngaged), "part", "no".
	inline const char* EngagedWord(const Window& w)
	{
		if (w.recorded == 0 || w.engaged == 0)
			return "no";
		return MostlyEngaged(w) ? "yes" : "part";
	}

	// Frames in a window generating for 90% or more of them, with no generated frame presented at all, that are called a gate (fewer are
	// a window that is mostly the start).
	constexpr uint32_t kNothingShownFrames = 20;

	// The gate that failed most often in the window (Gate::None: frames are generated, or nearly all the time).
	inline Gate FirstGate(const StatusInputs& in)
	{
		const Window& w = in.w;
		if (in.engine_failed)
			return Gate::EngineFailed;
		if (!in.swapchain)
			return Gate::NoSwapchain;
		if (w.recorded == 0)
		{
			// No picture reached the generator in the whole window: the most common reason among the vsyncs says which gate. The
			// order of a tie is the order the code meets them in.
			uint32_t best = 0;
			Gate gate = Gate::NoNewFrames; // nothing counted at all: still named, never "none"
			const struct
			{
				uint32_t count;
				Gate gate;
			} reasons[] = {{w.throttled, Gate::PresentThrottled}, {w.blank, Gate::BlankPicture}, {w.not_new, Gate::NoNewFrames},
				{w.dup_skipped, Gate::DuplicateSkipped}, {w.unserved, Gate::NoSwapchain}};
			// (unserved here, with the engine well and a swapchain now: the swapchain was gone for the window's frames, then made again)
			for (const auto& r : reasons)
			{
				if (r.count > best)
				{
					best = r.count;
					gate = r.gate;
				}
			}
			return gate;
		}
		if (MostlyEngaged(w))
		{
			// Duplicate skips are how a game with a frame every second vsync gets its generated frames' refreshes. But a pacing that
			// generates for the whole window and puts nothing on the screen is the symptom of "frame generation does nothing" too:
			// every frame's presents used up by the ones the game already made, or an interpolator that made no frame.
			return w.made == 0 && w.recorded >= kNothingShownFrames ? Gate::NothingShown : Gate::None;
		}
		// More vsyncs lost before the pacing than reached it: that reason (a few new frames among a thousand blank pictures).
		uint32_t lost = 0;
		Gate lost_gate = Gate::None;
		const struct
		{
			uint32_t count;
			Gate gate;
		} before[] = {{w.throttled, Gate::PresentThrottled}, {w.blank, Gate::BlankPicture}, {w.not_new, Gate::NoNewFrames},
			{w.unserved, Gate::NoSwapchain}};
		for (const auto& r : before)
		{
			if (r.count > lost)
			{
				lost = r.count;
				lost_gate = r.gate;
			}
		}
		if (lost > w.recorded)
			return lost_gate;
		// Else the pacing's state most of the window's frames were in, other than generating.
		uint32_t best = 0;
		Gate gate = Gate::None;
		for (int i = 0; i < kStates; i++)
		{
			if (i == static_cast<int>(State::Generating))
				continue;
			if (w.by_state[i] > best)
			{
				best = w.by_state[i];
				gate = GateOfState(static_cast<State>(i));
			}
		}
		return gate;
	}

	// "generating 70%, unsteady-frame-length 30%": where the window's new frames were (empty when none was).
	inline std::string StateMix(const Window& w)
	{
		std::string out;
		if (w.recorded == 0)
			return out;
		// generating first, then the gates in the order of State
		const State order[kStates] = {State::Generating, State::Warming, State::NoRoom, State::Unsteady, State::NotNominal, State::Paused};
		for (const State s : order)
		{
			const uint32_t n = w.by_state[static_cast<int>(s)];
			if (n == 0)
				continue;
			char part[64];
			snprintf(part, sizeof(part), "%s%s %.0f%%", out.empty() ? "" : ", ",
				s == State::Generating ? "generating" : GateName(GateOfState(s)), 100.0 * n / w.recorded);
			out += part;
		}
		return out;
	}

	// The display rate as the line shows it: the one used and where it came from, then the raw readings, so a driver that fell back
	// to 59.94 Hz with a 119.88 Hz mode listed shows at a glance.
	inline std::string DisplayText(const StatusInputs& in)
	{
		char driver[48], mode[48];
		if (in.driver_mhz)
			snprintf(driver, sizeof(driver), "the driver says %.2f Hz", in.driver_mhz / 1000.0);
		else
			snprintf(driver, sizeof(driver), "the driver gives no rate");
		if (in.mode_mhz)
			snprintf(mode, sizeof(mode), "the listed mode %.2f Hz", in.mode_mhz / 1000.0);
		else
			snprintf(mode, sizeof(mode), "no mode listed");
		return std::string(DisplayHzSourceName(in.display.source)) + "; " + driver + ", " + mode;
	}

	// The 10 s line. One line, always the same words, so that a tester's log can be searched: "[fg] status:". At most 1400 characters
	// (the deferred log cuts a line at 2047 and then loses its newline).
	inline std::string FormatStatus(const StatusInputs& in)
	{
		const Window& w = in.w;
		const Gate gate = FirstGate(in);
		const double secs = w.seconds > 0.0 ? w.seconds : 1.0;
		const double frame_len = w.counted ? w.vsync_sum / w.counted : 0.0;

		char buf[1024];
		int n = snprintf(buf, sizeof(buf),
			"[fg] status: engaged=%s, first failing gate: %s | launch: %s | display %.2f Hz (%s) | game %.1f fps, %.2f PS2 vsyncs a frame at "
			"%.2f Hz = %.2f refreshes a frame (%.2f needed) | generated %.1f/s (%.1f presents each) | speed %.0f%% | %u vsyncs in %.1f s: "
			"%u new frames, %u not new, %u blank, %u duplicate-skipped, %u present-throttled",
			EngagedWord(w), GateName(gate), in.launch.empty() ? "unknown" : in.launch.c_str(), in.display.hz, DisplayText(in).c_str(),
			w.recorded / secs, frame_len, in.ps2_hz, in.refreshes, Pacing::kEngage, w.made / secs,
			w.made ? static_cast<double>(w.presents) / w.made : 0.0, in.speed, w.Vsyncs(), w.seconds, w.recorded, w.not_new, w.blank,
			w.dup_skipped, w.throttled);
		std::string out(buf, n > 0 ? static_cast<size_t>(n < static_cast<int>(sizeof(buf)) ? n : static_cast<int>(sizeof(buf)) - 1) : 0);

		if (w.unserved)
			out += ", " + std::to_string(w.unserved) + " could not be taken by the generator";
		const std::string mix = StateMix(w);
		if (!mix.empty())
			out += " | pacing: " + mix;

		switch (gate)
		{
			case Gate::NoRoom:
				if (in.display.hz < 90.0)
					out += " | the display is at 60 Hz, so a 60 fps game or a PAL one at 50 fps has no room: the [display] swapchain line says "
					       "why the 120 Hz output was not taken";
				break;
			case Gate::Unsteady:
				out += " | " + std::to_string(in.steady) + " of the last " + std::to_string(Pacing::kWindow) + " frames alike (" +
				       std::to_string(Pacing::kSteadyStart) + " to start, " + std::to_string(Pacing::kSteadyKeep) + " to go on)";
				break;
			case Gate::Warming:
				out += " | " + std::to_string(in.warm_frames) + " of " + std::to_string(Pacing::kWindow) + " frames";
				break;
			case Gate::DuplicateSkipped:
				out += " | every vsync showed the game's last frame again: the internal FPS count sees no new frame";
				break;
			case Gate::PresentThrottled:
				out += " | presenting is throttled: the game runs faster than its target or the limiter is off";
				break;
			case Gate::EngineFailed:
				out += " | the '[fg] frame generation could not start' line says why";
				break;
			case Gate::NothingShown:
				out += " | the pacing generates, but no generated frame was presented in the window: each frame's presents were used up by "
				       "presents the game already made, or the interpolator made no frame";
				break;
			default:
				break;
		}
		if (in.videoout && in.videoout[0])
			out += std::string(" | ") + in.videoout;
		out += in.gpu;
		// One line whatever the texts held (a launch path or a summary with a line break in it would end the log line early and leave the
		// rest as a line of its own, without the "[fg]" a search looks for).
		for (char& c : out)
		{
			if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
				c = ' ';
		}
		if (out.size() > 1400)
		{
			out.resize(1397);
			out += "...";
		}
		return out;
	}

	// The "[fg] not generating: ... no room for another" line (the pacing's state NoRoom, logged once per change), and whether it is the
	// case the 120 Hz output is the answer to: a game with a new frame every vsync (60 fps; PAL's 50) on a display below 90 Hz.
	struct NoRoomText
	{
		std::string line; // without the line break
		bool needs_120hz = false;
	};

	inline NoRoomText FormatNoRoom(double median_vsyncs, double ps2_hz, double refreshes, double display_hz, DisplayHzSource source,
		const std::string& videoout)
	{
		NoRoomText t;
		std::string why;
		if (display_hz < 90.0)
		{
			why = " (at 60 Hz only games at 30 fps or less get generated frames; ";
			if (median_vsyncs < 1.5)
			{
				t.needs_120hz = true;
				char rate[400];
				snprintf(rate, sizeof(rate),
					"a 60 fps game, or a PAL one at 50 fps (shown at 100), needs the 120 Hz output. The display rate is %.2f Hz, from %s; "
					"the [display] swapchain line says why it is not 120.",
					display_hz, DisplayHzSourceName(source));
				why += rate;
				if (!videoout.empty())
					why += " " + videoout; // the app's own switch's summary, when the driver has no output control of its own
			}
			else
				why += "60 fps ones need the 120 Hz output";
			why += ")";
		}
		char head[256];
		snprintf(head, sizeof(head), "[fg] not generating: a new frame every %.1f PS2 vsyncs (%.2f Hz) is %.2f refreshes at %.2f Hz, no room for another",
			median_vsyncs, ps2_hz, refreshes, display_hz);
		t.line = head + why;
		return t;
	}
} // namespace orbis_fg
