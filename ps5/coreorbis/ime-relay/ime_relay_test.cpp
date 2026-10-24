// PS5SX2 (2.11, AI-assisted): host test of ProsperoImeRelay.cpp with a fake ELF loader on 127.0.0.1:9021 (no console).
// The fake loader reads the whole payload, finds the request the app wrote over the marker (the way the real payload does), checks its
// fields and answers the way ime_relay.c would: a ".started" file, then the result file.
//
// Build + run: test-ime-relay.sh
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../orbis-shims/ProsperoImeRelay.cpp" // the code under test, with ORBIS_IME_RELAY_ELF set to a file that holds the marker

#include <atomic>
#include <cstdlib>
#include <utime.h>

std::string OrbisLogPath(const char* name)
{
	return std::string(std::getenv("IME_TEST_DIR")) + "/" + name;
}

namespace
{
int g_fail = 0;
#define CHECK(cond, ...)                                                                                                                   \
	do                                                                                                                                     \
	{                                                                                                                                      \
		if (!(cond))                                                                                                                       \
		{                                                                                                                                  \
			g_fail++;                                                                                                                      \
			std::printf("FAIL line %d: %s -- ", __LINE__, #cond);                                                                          \
			std::printf(__VA_ARGS__);                                                                                                      \
			std::printf("\n");                                                                                                             \
		}                                                                                                                                  \
	} while (0)

void WriteFile(const std::string& path, const std::string& text)
{
	FILE* f = std::fopen(path.c_str(), "wb");
	if (f)
	{
		std::fwrite(text.data(), 1, text.size(), f);
		std::fclose(f);
	}
}

// What the fake loader does with a payload: "started" (after a delay), then the answer the test chose.
struct Script
{
	bool start = true;        // write the ".started" file
	std::string answer;       // the result file's content ("" = never written)
	int answer_delay_ms = 300;
	bool hold_open = true;    // keep the connection until the answer is written
	int alive_age_s = -1;     // >= 0: write the ".alive" heartbeat file, as old as this many seconds
};
std::atomic<int> g_connections{0};
std::string g_last_request;
Script g_script;

void Loader(int server)
{
	for (;;)
	{
		const int c = accept(server, nullptr, nullptr);
		if (c < 0)
			return;
		g_connections++;
		std::string all;
		char buf[65536];
		ssize_t n;
		while ((n = recv(c, buf, sizeof(buf), 0)) > 0)
			all.append(buf, static_cast<size_t>(n));
		const size_t at = all.find("PS5SX2-IME-RELAY-REQUEST-V1\nuser=");
		std::string req = at == std::string::npos ? std::string() : all.substr(at, all.find('\0', at) - at);
		g_last_request = req;
		const size_t r = req.find("\nresult=");
		const std::string result = r == std::string::npos ? std::string() : req.substr(r + 8, req.find('\n', r + 8) - (r + 8));
		if (!result.empty())
		{
			if (g_script.start)
			WriteFile(result + ".started", "1\n");
			if (g_script.alive_age_s >= 0)
			{
				WriteFile(result + ".alive", "1\n");
				utimbuf old_time;
				old_time.actime = old_time.modtime = std::time(nullptr) - g_script.alive_age_s;
				utime((result + ".alive").c_str(), &old_time);
			}
			usleep(static_cast<useconds_t>(g_script.answer_delay_ms) * 1000);
			if (!g_script.answer.empty())
				WriteFile(result, g_script.answer);
		}
		if (g_script.hold_open)
			usleep(200000);
		close(c);
	}
}

int StartLoader()
{
	const int s = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons(9021);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(s, 4) != 0)
	{
		std::printf("can't listen on 127.0.0.1:9021 (errno %d)\n", errno);
		std::exit(2);
	}
	std::thread(Loader, s).detach();
	return s;
}

// Polls until it isn't 0 (up to `limit_s`).
int Wait(std::string& text, std::string& why, double limit_s)
{
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		const int r = OrbisImeRelayPoll(text, why);
		if (r != 0)
			return r;
		if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > limit_s)
			return 0;
		usleep(20000);
	}
}
} // namespace

int main()
{
	CHECK(OrbisImeRelayBuilt(), "built");
	StartLoader();
	std::string text, why;

	// 1. A keyboard that returns text (with a multi-byte letter and an emoji).
	g_script = {};
	g_script.answer = "status 2\nend 0\nrc 0\ntext H\xC3\xA9llo w\xC3\xB6rld \xF0\x9F\x8E\xAE\n";
	CHECK(OrbisImeRelayStart("Search games", "old text", false, 64, 0x1234, why), "start: %s", why.c_str());
	CHECK(OrbisImeRelayPoll(text, why) == 0, "polls 0 while waiting");
	CHECK(Wait(text, why, 5.0) == 1, "text comes back (%s)", why.c_str());
	CHECK(text == "H\xC3\xA9llo w\xC3\xB6rld \xF0\x9F\x8E\xAE", "text is '%s'", text.c_str());
	CHECK(g_last_request.find("\nuser=4660\n") != std::string::npos, "user id in the request: %s", g_last_request.c_str());
	CHECK(g_last_request.find("\nmax=64\n") != std::string::npos, "max");
	CHECK(g_last_request.find("\ntype=0\npassword=0\n") != std::string::npos, "type/password for plain text");
	CHECK(g_last_request.find("\ntitle=Search games\n") != std::string::npos, "title");
	CHECK(g_last_request.find("\ntext=old text\n") != std::string::npos, "text");
	CHECK(g_last_request.find(std::string("\nresult=") + OrbisLogPath("ime-result.txt") + "\n") != std::string::npos, "result path");
	CHECK(OrbisImeRelayPoll(text, why) == -1, "no request pending afterwards");

	// 2. Cancelled with the controller's Circle: no text, "cancelled".
	g_script = {};
	g_script.answer = "status 2\nend 1\nrc 0\ntext \n";
	CHECK(OrbisImeRelayStart("Search games", "", false, 64, 1, why), "start 2");
	const int r2 = Wait(text, why, 5.0);
	CHECK(r2 == -1 && why == "cancelled", "cancel: %d '%s'", r2, why.c_str());

	// 3. A password: the basic Latin type and the password flag.
	g_script = {};
	g_script.answer = "status 2\nend 0\nrc 0\ntext hunter2\n";
	CHECK(OrbisImeRelayStart("Password", "", true, 32, 1, why), "start 3");
	CHECK(Wait(text, why, 5.0) == 1 && text == "hunter2", "password text '%s'", text.c_str());
	CHECK(g_last_request.find("\ntype=1\npassword=1\n") != std::string::npos, "type/password for a password: %s", g_last_request.c_str());

	// 4. The dialog doesn't open (the payload says so): not "cancelled".
	g_script = {};
	g_script.answer = "status -1\nend 0\nrc 80020063\ntext \n";
	CHECK(OrbisImeRelayStart("x", "", false, 64, 1, why), "start 4");
	const int r4 = Wait(text, why, 5.0);
	CHECK(r4 == -1 && why != "cancelled" && !why.empty(), "failure: %d '%s'", r4, why.c_str());

	// 5. A request line with a newline in the title/text can't break the format.
	g_script = {};
	g_script.answer = "status 2\nend 0\nrc 0\ntext ok\n";
	CHECK(OrbisImeRelayStart("two\nlines", "a\nb\r\nc", false, 64, 1, why), "start 5");
	CHECK(Wait(text, why, 5.0) == 1, "5 done");
	CHECK(g_last_request.find("\ntitle=two lines\n") != std::string::npos, "title one line: %s", g_last_request.c_str());
	CHECK(g_last_request.find("\ntext=a b  c\n") != std::string::npos, "text one line");

	// 6. A payload that never shows the dialog (no ".started"): gives up after 5 s with a reason.
	g_script = {};
	g_script.start = false;
	CHECK(OrbisImeRelayStart("x", "", false, 64, 1, why), "start 6");
	const auto t0 = std::chrono::steady_clock::now();
	const int r6 = Wait(text, why, 8.0);
	const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	CHECK(r6 == -1 && why.find("did not come up") != std::string::npos, "never started: %d '%s'", r6, why.c_str());
	CHECK(took > 4.5 && took < 6.5, "the wait is about 5 s (%.1f)", took);

	// 7. A very long title and text are cut to fit the payload's request room (the marker and zeros up to 4 KiB).
	g_script = {};
	g_script.answer = "status 2\nend 0\nrc 0\ntext fine\n";
	std::string big(2000, 'x');
	CHECK(OrbisImeRelayStart(big, big, false, 64, 1, why), "long title and text: %s", why.c_str());
	CHECK(Wait(text, why, 5.0) == 1 && text == "fine", "7 done");
	CHECK(g_last_request.size() < 4096 && g_last_request.find("\ntext=") != std::string::npos, "the request fits (%zu bytes)", g_last_request.size());

	// 8. (code review) The keyboard ending any way but the person's Circle is a failure, not "cancelled": the system closing it (end 2), a result that is no
	// success (rc), a status that is not 2.
	g_script = {};
	g_script.answer = "status 2\nend 2\nrc 0\ntext \n";
	CHECK(OrbisImeRelayStart("x", "", false, 64, 1, why), "start 8");
	CHECK(Wait(text, why, 5.0) == -1 && why != "cancelled" && why.find("system") != std::string::npos, "aborted by the system: '%s'", why.c_str());
	g_script = {};
	g_script.answer = "status 2\nend 0\nrc 80020001\ntext \n";
	CHECK(OrbisImeRelayStart("x", "", false, 64, 1, why), "start 9");
	CHECK(Wait(text, why, 5.0) == -1 && why != "cancelled", "a failed result: '%s'", why.c_str());
	g_script = {};
	g_script.answer = "status 0\nend 0\nrc 0\ntext \n";
	CHECK(OrbisImeRelayStart("x", "", false, 64, 1, why), "start 10");
	CHECK(Wait(text, why, 5.0) == -1 && why != "cancelled", "a status that is not 2: '%s'", why.c_str());
	// 9. A payload that stopped after its dialog came up (its heartbeat is a minute old, no answer): given up on at once, not after 700 s.
	g_script = {};
	g_script.alive_age_s = 60;
	CHECK(OrbisImeRelayStart("x", "", false, 64, 1, why), "start 11");
	const auto tg = std::chrono::steady_clock::now();
	const int rg = Wait(text, why, 6.0);
	CHECK(rg == -1 && why.find("stopped") != std::string::npos, "a dead payload: %d '%s'", rg, why.c_str());
	CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - tg).count() < 3.0, "...and quickly");
	// a request given up on is cleaned: a late answer file is removed
std::this_thread::sleep_for(std::chrono::milliseconds(800)); // the fake loader finishes the last request before the script changes
	g_script = {};
	g_script.start = false;
	g_script.answer_delay_ms = 7000; // after the 5 s give-up
	g_script.answer = "status 2\nend 0\nrc 0\ntext secret\n";
	CHECK(OrbisImeRelayStart("x", "", true, 64, 1, why), "start 12");
	CHECK(Wait(text, why, 8.0) == -1, "gave up (no keyboard in 5 s)");
	bool gone = false;
	for (int i = 0; i < 40 && !gone; i++)
	{
		usleep(250000);
		gone = Exists(OrbisLogPath("ime-result.txt")) == false;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(2500)); // the cleanup runs every 2 s
	CHECK(!Exists(OrbisLogPath("ime-result.txt")), "the late answer (a typed password) did not stay in a file");
	std::printf(g_fail ? "ime relay test: %d FAILED\n" : "ime relay test: all passed (%d connections)\n", g_fail ? g_fail : g_connections.load());
	return g_fail ? 1 : 0;
}
