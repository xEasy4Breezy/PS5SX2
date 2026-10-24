// PS5SX2 (2.11, AI-assisted): the PS5's keyboard through the console's ELF loader (see ProsperoImeRelay.h and ime-relay/ime_relay.c).
//
// The same way as ProsperoNotify.cpp's relay: the embedded payload gets its request written over a marker, the whole file goes to the
// loader on 127.0.0.1:9021, which runs it as a system process. Unlike the notification relay this payload stays up while the keyboard
// does, so a thread holds the connection open until the loader closes it (a closed connection might end the payload), and the result
// comes back through a file the request names: logs/ime-result.txt, with logs/ime-result.txt.started when the dialog came up and
// logs/ime-result.txt.log with what the payload saw.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProsperoImeRelay.h"

#include "OrbisPaths.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef ORBIS_IME_RELAY_ELF
// The relay payload (ime-relay/ime_relay.c), built and named by Makefile.vk.
__asm__(".section .rodata.orbis_ime_relay_elf,\"a\",@progbits\n"
		".balign 16\n"
		".global orbis_ime_relay_elf\norbis_ime_relay_elf:\n"
		".incbin \"" ORBIS_IME_RELAY_ELF "\"\n"
		".global orbis_ime_relay_elf_end\norbis_ime_relay_elf_end:\n"
		".byte 0\n"
		".previous\n");
extern "C" const uint8_t orbis_ime_relay_elf[], orbis_ime_relay_elf_end[];
#endif

namespace
{
constexpr char kMarker[] = "PS5SX2-IME-RELAY-REQUEST-V1";
constexpr size_t kRoom = 4096; // g_request's size in ime_relay.c
constexpr uint16_t kLoaderPort = 9021;

std::mutex g_mutex;
std::atomic<unsigned> g_epoch{0}; // bumped by each request: a cleanup thread of an earlier one stops
bool g_active = false;
std::chrono::steady_clock::time_point g_start;
std::string g_result, g_started;

double Since()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}

bool ReadAll(const std::string& path, std::string& out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	char buf[1024];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < 8192)
		out.append(buf, n);
	std::fclose(f);
	return true;
}

std::string OneLine(std::string s)
{
	for (char& c : s)
		if (static_cast<unsigned char>(c) < 0x20)
			c = ' ';
	return s;
}

#ifdef ORBIS_IME_RELAY_ELF
// Where the request goes in the embedded file: the marker, then zeros to the end of the array.
size_t RequestOffset()
{
	static const size_t offset = [] {
		const size_t size = static_cast<size_t>(orbis_ime_relay_elf_end - orbis_ime_relay_elf);
		const size_t mlen = sizeof(kMarker) - 1;
		for (size_t i = 0; i + kRoom <= size; i++)
		{
			if (std::memcmp(orbis_ime_relay_elf + i, kMarker, mlen) != 0)
				continue;
			size_t j = i + mlen;
			while (j < i + kRoom && orbis_ime_relay_elf[j] == 0)
				j++;
			if (j == i + kRoom)
				return i;
		}
		return SIZE_MAX;
	}();
	return offset;
}
#endif
} // namespace

namespace
{
// A request the app gave up on (the keyboard did not come in time, or it was never answered): the payload may still show its dialog and write what is typed
// (a password too) to the result file. For ten minutes, or until the next request, the files are removed as soon as they appear.
void AbandonRequest(const std::string& result)
{
	const unsigned epoch = g_epoch.load();
	std::thread([result, epoch] {
		for (int i = 0; i < 300 && g_epoch.load() == epoch; i++)
		{
			unlink(result.c_str());
			unlink((result + ".started").c_str());
			unlink((result + ".alive").c_str());
			sleep(2);
		}
	}).detach();
}
} // namespace

bool OrbisImeRelayBuilt()
{
#ifdef ORBIS_IME_RELAY_ELF
	return true;
#else
	return false;
#endif
}

bool OrbisImeRelayStart(const std::string& title, const std::string& text, bool password, unsigned max_length, int user, std::string& why)
{
#ifndef ORBIS_IME_RELAY_ELF
	(void)title;
	(void)text;
	(void)password;
	(void)max_length;
	(void)user;
	why = "this build has no relay";
	return false;
#else
	std::lock_guard<std::mutex> lock(g_mutex);
	const size_t offset = RequestOffset();
	if (offset == SIZE_MAX)
	{
		why = "the embedded relay has no request marker";
		return false;
	}
	g_epoch++;
	g_result = OrbisLogPath("ime-result.txt");
	g_started = g_result + ".started";
	unlink(g_result.c_str());
	unlink(g_started.c_str());
	unlink((g_result + ".alive").c_str());
	unlink((g_result + ".log").c_str());
	// A password needs the basic Latin keyboard; anything else the default one (all the languages the console has).
	const std::string request = std::string(kMarker) + "\nuser=" + std::to_string(user) + "\nmax=" + std::to_string(max_length) +
	                            "\ntype=" + (password ? "1" : "0") + "\npassword=" + (password ? "1" : "0") + "\nresult=" + g_result +
	                            "\ntitle=" + OneLine(title).substr(0, 190) + "\ntext=" + OneLine(text).substr(0, 590) + "\n";
	if (request.size() >= kRoom)
	{
		why = "the request is too large";
		return false;
	}
	std::vector<uint8_t> elf(orbis_ime_relay_elf, orbis_ime_relay_elf_end);
	std::memcpy(elf.data() + offset, request.data(), request.size());
	elf[offset + request.size()] = 0;

	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		why = "socket: errno " + std::to_string(errno);
		return false;
	}
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kLoaderPort);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	const auto fail = [&](const char* step) {
		why = std::string(step) + ": errno " + std::to_string(errno);
		close(fd);
		return false;
	};
	const int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
		return fail("fcntl");
	if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 && errno != EINPROGRESS)
		return fail("connect"); // no ELF loader on this console
	struct pollfd p = {fd, POLLOUT, 0};
	if (poll(&p, 1, 500) != 1)
	{
		errno = ETIMEDOUT;
		return fail("connect");
	}
	int err = 0;
	socklen_t len = sizeof(err);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0)
	{
		errno = err;
		return fail("connect");
	}
	fcntl(fd, F_SETFL, flags);
	for (size_t sent = 0; sent < elf.size();)
	{
		const ssize_t n = send(fd, elf.data() + sent, elf.size() - sent, 0);
		if (n <= 0)
			return fail("send");
		sent += static_cast<size_t>(n);
	}
	shutdown(fd, SHUT_WR);
	// The payload stays up while the keyboard does: a thread keeps the connection open until the loader closes it (or ten minutes pass).
	std::thread([fd] {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
		for (;;)
		{
			const int left = static_cast<int>(
				std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
			struct pollfd q = {fd, POLLIN, 0};
			if (left <= 0 || poll(&q, 1, left) != 1)
				break;
			char buf[256];
			if (recv(fd, buf, sizeof(buf), 0) <= 0)
				break;
		}
		close(fd);
	}).detach();
	g_active = true;
	g_start = std::chrono::steady_clock::now();
	std::printf("[ime] relay: the request went to the ELF loader (127.0.0.1:%u), result file %s\n", static_cast<unsigned>(kLoaderPort), g_result.c_str());
	std::fflush(stdout);
	return true;
#endif
}

int OrbisImeRelayPoll(std::string& text, std::string& why)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_active)
	{
		why = "no request";
		return -1;
	}
	std::string body;
	if (ReadAll(g_result, body) && !body.empty())
	{
		g_active = false;
		int status = -1, end = -1;
		unsigned rc = 0xffffffffu;
		std::string typed;
		size_t at = 0;
		while (at < body.size())
		{
			size_t nl = body.find('\n', at);
			if (nl == std::string::npos)
				nl = body.size();
			const std::string line = body.substr(at, nl - at);
			if (line.compare(0, 7, "status ") == 0)
				status = std::atoi(line.c_str() + 7);
			else if (line.compare(0, 4, "end ") == 0)
				end = std::atoi(line.c_str() + 4);
			else if (line.compare(0, 3, "rc ") == 0)
				rc = static_cast<unsigned>(std::strtoul(line.c_str() + 3, nullptr, 16));
			else if (line.compare(0, 5, "text ") == 0)
				typed = line.substr(5);
			at = nl + 1;
		}
		std::printf("[ime] relay: the keyboard closed: status %d, end %d, rc %#x, %zu bytes typed\n", status, end, rc, typed.size());
		std::fflush(stdout);
		unlink(g_result.c_str());
		unlink(g_started.c_str());
		unlink((g_result + ".alive").c_str());
		if (status == 2 && end == 0 && rc == 0)
		{
			text = typed;
			return 1;
		}
		// Only the person's own Circle is "cancelled"; anything else is the keyboard not working (the shelf then uses its own for the rest of the session).
		if (status == 2 && end == 1 && rc == 0)
			why = "cancelled";
		else if (status == -1)
			why = "the keyboard would not open (see ime-result.txt.log)";
		else if (status == 2 && rc == 0 && end == 2)
			why = "the system closed the keyboard";
		else
			why = "the keyboard ended with status " + std::to_string(status) + ", end " + std::to_string(end) + ", result " + std::to_string(rc);
		return -1;
	}
	const bool started = Exists(g_started);
	if (!started && Since() > 5.0)
	{
		g_active = false;
		std::string log;
		ReadAll(g_result + ".log", log);
		why = "the keyboard did not come up in 5 s" + (log.empty() ? std::string(" (the payload wrote no log: it may not have started)") : ": " + OneLine(log));
		std::printf("[ime] relay: %s\n", why.c_str());
		std::fflush(stdout);
		AbandonRequest(g_result);
		return -1;
	}
	if (started)
	{
		// The payload stamps <result>.alive every second while the dialog is up: no stamp for 20 s is a payload that has stopped without an answer.
		struct stat st = {};
		time_t last = 0;
		if (stat((g_result + ".alive").c_str(), &st) == 0)
			last = st.st_mtime;
		else if (stat(g_started.c_str(), &st) == 0)
			last = st.st_mtime;
		if (last != 0 && std::time(nullptr) - last > 20)
		{
			g_active = false;
			why = "the keyboard's process stopped without an answer";
			std::printf("[ime] relay: %s\n", why.c_str());
			std::fflush(stdout);
			AbandonRequest(g_result);
			return -1;
		}
	}
	if (Since() > 700.0)
	{
		g_active = false;
		why = "timed out";
		AbandonRequest(g_result);
		return -1;
	}
	return 0;
}
