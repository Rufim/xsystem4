/* Copyright (C) 2026  xsystem4-android
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

// TextSurfaceManager: holds the current font style used when laying out ADV
// message text, and measures text width. Tsumamigui 3's ADV loop calls
// SetFont* before rendering each message segment (via PartsEngine.Parts_SetText)
// and GetFontWidth for wrapping/layout. Without this the game aborts on the
// first message (SetFontType -> unimplemented).

#include <math.h>
#include <stdbool.h>

#include "system4/string.h"
#include "system4/utfsjis.h"

#include "hll.h"
#include "gfx/font.h"

static struct text_style ts = {
	.face = FONT_GOTHIC,
	.size = 24,
	.bold_width = 0,
	.weight = FW_NORMAL,
	.edge_left = 0, .edge_up = 0, .edge_right = 0, .edge_down = 0,
	.color = { 255, 255, 255, 255 },
	.edge_color = { 0, 0, 0, 255 },
	.scale_x = 1.0f,
	.space_scale_x = 1.0f,
	.font_spacing = 0,
	.font_size = NULL,
};

static void TextSurfaceManager_SetFontType(int nType)
{
	ts.face = nType;
	ts.font_size = NULL;  // invalidate cached size
}

static void TextSurfaceManager_SetFontSize(int nSize)
{
	if (getenv("XSYS4_FNL_TRACE")) NOTICE("TSM SetFontSize %d", nSize);
	ts.size = nSize;
	ts.font_size = NULL;
}

static void TextSurfaceManager_SetFontColor(int nR, int nG, int nB)
{
	ts.color = (SDL_Color){ nR, nG, nB, 255 };
}

static void TextSurfaceManager_SetFontBoldWeight(float fBoldWeight)
{
	ts.bold_width = fBoldWeight;
}

static void TextSurfaceManager_SetFontEdgeWeight(float fEdgeWeight)
{
	ts.edge_left = ts.edge_up = ts.edge_right = ts.edge_down = fEdgeWeight;
}

static void TextSurfaceManager_SetFontEdgeColor(int nR, int nG, int nB)
{
	ts.edge_color = (SDL_Color){ nR, nG, nB, 255 };
}

static bool TextSurfaceManager_GetFontWidth(struct string *text, int *width)
{
	// The glyph outline reserves advance, exactly as in upstream's gfx_size_text.
	// Tsumamigui 3's ADV loop draws the message one character per parts-text and
	// advances its own cursor by this width (it never asks the engine for a parts
	// width), so omitting the edge term put every character 1px too close to the
	// next one: identical glyph ink to the original but 1px inter-letter gaps
	// instead of 2px, and a line 42px short. See FINDINGS §5f.
	float edge_advance = gfx_text_advance_edges
		? ts.edge_left + ts.edge_right + ceilf(ts.bold_width)
		: 0.0f;
	// Per-glyph advance is integral, rounded UP: an .fnl advance is width/denominator
	// and lands on a quarter pixel, and rounding to nearest lost 1px on every glyph
	// whose fraction was .25 — measured as a line 12px short of the original over 42
	// characters (~25% of them). Ceiling per glyph reproduces the original exactly.
	float w = 0.0f;
	const char *p = text->text;
	while (*p) {
		int len = SJIS_2BYTE((unsigned char)*p) ? 2 : 1;
		w += ceilf(gfx_size_char(&ts, p) + edge_advance);
		p += len;
	}
	*width = (int)w;
	return true;
}

HLL_LIBRARY(TextSurfaceManager,
	    HLL_EXPORT(SetFontType, TextSurfaceManager_SetFontType),
	    HLL_EXPORT(SetFontSize, TextSurfaceManager_SetFontSize),
	    HLL_EXPORT(SetFontColor, TextSurfaceManager_SetFontColor),
	    HLL_EXPORT(SetFontBoldWeight, TextSurfaceManager_SetFontBoldWeight),
	    HLL_EXPORT(SetFontEdgeWeight, TextSurfaceManager_SetFontEdgeWeight),
	    HLL_EXPORT(SetFontEdgeColor, TextSurfaceManager_SetFontEdgeColor),
	    HLL_EXPORT(GetFontWidth, TextSurfaceManager_GetFontWidth)
	);
