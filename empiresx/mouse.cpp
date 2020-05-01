/* Copyright 2016-2020 the Age of Empires Free Software Remake authors. See LEGAL for legal info */

#include "mouse.hpp"

#include <cassert>

#include "engine.hpp"
#include "drs.hpp"

#include <SDL2/SDL_syswm.h>

namespace genie {

Cursor::Cursor(CursorId id) : handle(nullptr, &SDL_FreeCursor) {
	change(id);
}

Cursor::~Cursor() {
	change(CursorId::os_default);
}

ClipControl clip_control;

void Cursor::change(CursorId id) {
	if (!(unsigned)id) {
		SDL_SetCursor(SDL_GetDefaultCursor());
		handle.reset();
		return;
	}

	SDL_Cursor *newhandle;
	Animation icons(eng->assets->open_slp(eng->assets->pal_default, 51000));

	auto &img = icons.subimage((unsigned)id - 1);
	if (!(newhandle = SDL_CreateColorCursor(img.surface.data(), img.hotspot_x, img.hotspot_y))) {
		fprintf(stderr, "could not change cursor to %u\n", (unsigned)id);
		return;
	}

	SDL_SetCursor(newhandle);
	handle.reset(newhandle);
}

void ClipControl::noclip() {
	// invalidate area
	area.h = 0;
#if windows
	ClipCursor(NULL);
	clipping = false;
#elif linux
	fprintf(stderr, "%s: stub\n", __func__);
#else
	fprintf(stderr, "%s: operation not supported\n", __func__);
#endif
}

void ClipControl::focus_gained() {
	if (clipping)
		clip(enhanced);
	else
		noclip();
}

void ClipControl::clip(bool enhanced) {
#if windows
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);

	if (!SDL_GetWindowWMInfo(eng->w->data(), &info)) {
		fprintf(stderr, "%s: cannot get window info: %s\n", __func__, SDL_GetError());
		return;
	}

	HWND h = info.info.win.window;
	RECT r;
	POINT p{ 0, 0 };


	if (!ClientToScreen(h, &p) || !GetClientRect(h, &r)) {
		fprintf(stderr, "%s: could not get clipping area\n", __func__);
		return;
	}

	// adjust clipping area to attached display
	if (enhanced) {
		r.left += p.x; r.right += p.x;
		r.top += p.y; r.bottom += p.y;
	} else {
		auto &ren = eng->w->render();
		SDL_Rect ref(enhanced ? ren.dim.rel_bnds : ren.dim.lgy_bnds);

		// adjust clipping area
		r.left = p.x + ref.x; r.right = r.left + ref.w;
		r.top = p.y + ref.y; r.bottom = r.top + ref.h;
	}

	ClipCursor(&r);
	clipping = true;
#elif linux
	// https://tronche.com/gui/x/xlib/input/XGrabPointer.html
	fprintf(stderr, "%s: stub\n", __func__);
#else
	fprintf(stderr, "%s: operation not supported\n", __func__);
#endif
}

}
