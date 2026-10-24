// PS5SX2 (2.11, AI-assisted): the relaunch payload for "back to the menu" (L1 + R1 + Touch). A tiny program the console's ELF loader runs in a process of
// its own: it waits until this app has been closed by the system and then starts the main app again, which opens the shelf. See relaunch/relaunch.c,
// ProsperoRelaunch.cpp and orbis-shims/OrbisLeavePlan.h (how the app leaves).
#pragma once

#include "OrbisLeavePlan.h"

#include <string>

// True when this build embeds the payload.
bool OrbisRelaunchBuilt();

// 176: the payload's timing, written into its request (relaunch/relaunch.c): the wait after the shell has forgotten the closing title, the earliest launch
// after the closing process' exit, the pause between refused launches. The defaults are orbis_leave's (the flag menusettle sets settle_ms).
struct OrbisRelaunchTiming
{
	int settle_ms = orbis_leave::kSettleDefaultMs;
	int min_exit_ms = orbis_leave::kMinExitMs;
	int retry_ms = orbis_leave::kRetryMs;
};

// Sends the payload to the ELF loader (127.0.0.1:9021) and waits (at most about 3 s) until it says it is running ("<state>.started"). `closing_title` is
// the title the system will close (the disc tile's ID, "PPSA99203" for the main app, "" when not known: the payload then goes by the running big app),
// `launch_title` the title to start afterwards, `pid` this process. True when the payload is up: the caller may then leave through the plain close
// (LoadExec("exit")). False (`why` says what): nothing is left running that would start anything, the caller keeps the old way out.
bool OrbisRelaunchStart(const char* closing_title, const char* launch_title, int pid, std::string& why, const OrbisRelaunchTiming& timing = OrbisRelaunchTiming());

// Takes the request back (a note the payload checks every 100 ms until it launches): used when the close was refused and the app stays up. Harmless
// without a request, and more than once.
void OrbisRelaunchCancel();
