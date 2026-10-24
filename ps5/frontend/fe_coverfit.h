// PS5 port frontend: how a picture is fitted to the face of the case it goes on (2.11, AI-assisted; swordpdf: "the covers in the shelf need to be
// proportionate to the case they are meant to be in"). Pure arithmetic, header only: the cover service plans with it while it decodes (fe_covers.cpp),
// and host/coverfit_test.cpp checks the plans on a PC.
//
// The case's front and back faces are 135 x 190 mm and its spine 14 x 190 mm (kBoxHalfW/H/D in fe_renderer.h), and the box mesh maps a texture over the
// whole face (u, v = 0..1), so a texture of another proportion came out stretched. The shader and the mesh are left as they are; the picture is made the
// face's shape instead, without ever changing its own proportions:
//   Stretch  nearly the right shape already (a few percent off: the usual scans): left as it is, the stretch cannot be seen;
//   Crop     a little off: the middle of the picture, cut to the face's shape (for a cover at most 9 % of one side, 4.5 % each end), fills the face;
//   Pad      far off (a square scan, a banner): the whole picture, as large as fits, centred, with the case's plastic colour round it.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>

namespace fe
{
constexpr float kFaceAspectFront = 135.0f / 190.0f; // the front and the back: width / height (fe_app.cpp static_asserts it against kBoxHalfW / kBoxHalfH)
constexpr float kFaceAspectSpine = 14.0f / 190.0f;  // (against kBoxHalfD / kBoxHalfH)

// How far off the face's shape (the larger of w/h over the face and its inverse) a picture may be to be stretched / cropped; beyond the second it is padded.
// A cover's own shape is exact; a spine is a thin strip whose scans vary more and whose edges are blank margins.
constexpr double kStretchFront = 1.04, kCropFront = 1.10;
constexpr double kStretchSpine = 1.12, kCropSpine = 1.60;

enum class CoverFit
{
	None, // as decoded (the achievement badges)
	Face, // a front or a back cover
	Spine
};

struct FitPlan
{
	enum Mode
	{
		Stretch,
		Crop,
		Pad
	};
	Mode mode = Stretch;
	int sx = 0, sy = 0, sw = 0, sh = 0; // the part of the source that is used
	int cw = 0, ch = 0;                 // the texture that results
	int dx = 0, dy = 0, dw = 0, dh = 0; // where that part lands in it (the rest of a Pad texture is the bars)
};

// The plan for a `w` x `h` picture, the result at most `max_h` rows tall (what Decode did before: a taller picture is scaled down).
inline FitPlan PlanFit(int w, int h, int max_h, CoverFit fit)
{
	FitPlan p;
	w = std::max(w, 1);
	h = std::max(h, 1);
	max_h = std::max(max_h, 1);
	p.sx = p.sy = 0;
	p.sw = w;
	p.sh = h;
	auto scaled = [&](int sw, int sh, int& cw, int& ch) { // the size Decode gives a picture `sw` x `sh`
		if (sh > max_h)
		{
			cw = std::max(1, static_cast<int>(std::lround(static_cast<double>(sw) * max_h / sh)));
			ch = max_h;
		}
		else
		{
			cw = sw;
			ch = sh;
		}
	};
	if (fit == CoverFit::None)
	{
		scaled(w, h, p.cw, p.ch);
		p.dw = p.cw;
		p.dh = p.ch;
		return p;
	}
	const double face = fit == CoverFit::Spine ? kFaceAspectSpine : kFaceAspectFront;
	const double stretch_to = fit == CoverFit::Spine ? kStretchSpine : kStretchFront, crop_to = fit == CoverFit::Spine ? kCropSpine : kCropFront;
	const double r = (static_cast<double>(w) / h) / face; // > 1: wider than the face
	const double off = r >= 1.0 ? r : 1.0 / r;           // how far off, either way
	if (off <= stretch_to)
	{
		scaled(w, h, p.cw, p.ch);
		p.dw = p.cw;
		p.dh = p.ch;
		return p;
	}
	if (off <= crop_to)
	{
		p.mode = FitPlan::Crop;
		if (r > 1.0) // too wide: cut the sides
		{
			p.sh = h;
			p.sw = std::min(w, std::max(1, static_cast<int>(std::lround(h * face))));
		}
		else // too tall: cut the top and the bottom
		{
			p.sw = w;
			p.sh = std::min(h, std::max(1, static_cast<int>(std::lround(w / face))));
		}
		p.sx = (w - p.sw) / 2;
		p.sy = (h - p.sh) / 2;
		scaled(p.sw, p.sh, p.cw, p.ch);
		p.dw = p.cw;
		p.dh = p.ch;
		return p;
	}
	p.mode = FitPlan::Pad;
	p.ch = r > 1.0 ? std::max(1, static_cast<int>(std::lround(w / face))) : h; // as large as the picture allows ...
	p.ch = std::min(p.ch, max_h);                                              // ... but not taller than a texture may be
	p.cw = std::max(1, static_cast<int>(std::lround(p.ch * face)));
	const double s = std::min(static_cast<double>(p.cw) / w, static_cast<double>(p.ch) / h);
	p.dw = std::min(p.cw, std::max(1, static_cast<int>(std::lround(w * s))));
	p.dh = std::min(p.ch, std::max(1, static_cast<int>(std::lround(h * s))));
	p.dx = (p.cw - p.dw) / 2;
	p.dy = (p.ch - p.dh) / 2;
	return p;
}
} // namespace fe
