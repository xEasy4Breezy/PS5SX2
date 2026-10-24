// PS5 port frontend: LaunchBox Games Database art by PS2 serial (2.11, AI-assisted; swordpdf: "we can use this for the best covers, we can also use
// the circular disc cover for when a game is dumping").
//
// The index is a text file made on a PC from LaunchBox's daily bulk export (ps5/tools/make_launchbox_covers.py): one line per serial,
//   SLUS-21005 f=<file>* b=<file>* d=<file> s=<file> m=1
// f the box front, b the box back, d the disc (a PNG with a transparent surround), s the spine (the case's side); a trailing * marks art from the serial's own region (North
// America for SLUS, Japan for SLPM, ...); m is how sure the title match is (1 most; the app replaces a cover it already has only on a 1 with a
// front from the serial's own region). The files are on LaunchBox's image servers; the app downloads one image at a time, only for a game the
// shelf shows, and keeps it in the cover cache (fe_covers.cpp). A second file of the same shape in the cache folder replaces entries.
//
// Header only: the cover service and its tests use it without more to link.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_map>

namespace fe
{
struct LaunchboxArt
{
	std::string front, back, disc, spine; // file names on LaunchBox's servers; "" for none
	bool front_exact = false, back_exact = false, disc_exact = false, spine_exact = false; // from the serial's own region
	int tier = 0;                                                      // 1 (title matched as is) .. 3; 0 when the line has none
};

class LaunchboxIndex
{
public:
	// Adds the entries of `text` (the file's lines; blank lines and # comments are skipped), replacing those of the same serials. Lines that
	// aren't of the shape are skipped; a file name with anything but letters, digits, '.', '_' and '-' is refused (it ends up in an address).
	// Returns how many entries were added.
	size_t Add(const std::string& text)
	{
		size_t added = 0;
		size_t pos = 0;
		while (pos < text.size())
		{
			size_t end = text.find('\n', pos);
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(pos, end - pos);
			pos = end + 1;
			while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
				line.pop_back();
			if (line.empty() || line[0] == '#')
				continue;
			LaunchboxArt art;
			std::string serial;
			size_t at = 0;
			bool first = true, ok = true;
			while (at < line.size() && ok)
			{
				size_t sp = line.find(' ', at);
				if (sp == std::string::npos)
					sp = line.size();
				const std::string tok = line.substr(at, sp - at);
				at = sp + 1;
				if (tok.empty())
					continue;
				if (first)
				{
					first = false;
					serial = tok;
					ok = ValidSerial(serial);
					continue;
				}
				if (tok.size() < 3 || tok[1] != '=')
					continue; // a field this version doesn't know
				std::string val = tok.substr(2);
				bool exact = false;
				if (!val.empty() && val.back() == '*')
				{
					exact = true;
					val.pop_back();
				}
				switch (tok[0])
				{
					case 'f':
						ok = ValidFile(val);
						art.front = val;
						art.front_exact = exact;
						break;
					case 'b':
						ok = ValidFile(val);
						art.back = val;
						art.back_exact = exact;
						break;
					case 'd':
					ok = ValidFile(val);
					art.disc = val;
					art.disc_exact = exact;
					break;
					case 's':
					ok = ValidFile(val);
					art.spine = val;
					art.spine_exact = exact;
					break;
					case 'm':
						art.tier = val.empty() ? 0 : val[0] - '0';
						break;
					default:
						break;
				}
			}
			if (!ok || serial.empty() || (art.front.empty() && art.back.empty() && art.disc.empty() && art.spine.empty()))
				continue;
			m_map[serial] = std::move(art);
			added++;
		}
		return added;
	}

	const LaunchboxArt* Find(const std::string& serial) const
	{
		const auto it = m_map.find(serial);
		return it == m_map.end() ? nullptr : &it->second;
	}

	size_t Size() const { return m_map.size(); }

	// The address of one of the files: the "r2_" ones are served by LaunchBox's newer image host (the Games Database site links them there), the
	// rest by the older one. Both answer for either name; this follows the site.
	static std::string Url(const std::string& file)
	{
		return std::string(file.compare(0, 3, "r2_") == 0 ? "https://gamesdb-images.launchbox.gg/" : "https://images.launchbox-app.com/") + file;
	}

	// The art is sure enough to replace a cover the shelf already has (a download of another source): the title matched as it stands and the front is
	// from the serial's own region.
	static bool CanUpgrade(const LaunchboxArt& art) { return art.tier == 1 && !art.front.empty() && art.front_exact; }

private:
	static bool ValidSerial(const std::string& s)
	{
		if (s.size() < 5 || s.size() > 24)
			return false;
		for (const char c : s)
			if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
				return false;
		return true;
	}
	static bool ValidFile(const std::string& s)
	{
		if (s.size() < 5 || s.size() > 80)
			return false;
		for (const char c : s)
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
				return false;
		return s.find("..") == std::string::npos;
	}

	std::unordered_map<std::string, LaunchboxArt> m_map;
};
} // namespace fe
