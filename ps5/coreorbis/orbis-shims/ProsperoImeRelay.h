// PS5SX2 (2.11, AI-assisted): the PS5's keyboard through the console's ELF loader, for a process that can't load libSceImeDialog itself
// (firmware 11.40 and later refuse it: 0x80020063). See ime-relay/ime_relay.c and ProsperoImeRelay.cpp. An experiment: used only when the
// flag file flags/imerelay is there.
#pragma once

#include <string>

// True when this build embeds the relay payload.
bool OrbisImeRelayBuilt();

// Sends the request to the ELF loader (127.0.0.1:9021). True once it went out; the keyboard then comes up if the console lets a payload show
// one. `why` says what failed otherwise.
bool OrbisImeRelayStart(const std::string& title, const std::string& text, bool password, unsigned max_length, int user, std::string& why);

// 0 while the keyboard is up (or on its way), 1 when text came back (in `text`), -1 when it closed without text, never came up (within 5 s) or
// failed (`why`).
int OrbisImeRelayPoll(std::string& text, std::string& why);
