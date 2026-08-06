/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

#include <stdlib.h>

#include "system4.h"
#include "system4/fnl.h"
#include "system4/hashtable.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "gfx/gfx.h"
#include "gfx/font.h"
#include "xsystem4.h"

struct fnl_bitmap_glyph {
	unsigned width;
	unsigned height;
	unsigned advance;
	uint8_t *pixels;
};

struct fnl_bitmap_size {
	struct fnl_font_face *face;
	unsigned nr_glyphs;
	struct fnl_bitmap_glyph **glyphs;
};

struct fnl_font_size {
	struct font_size super;
	struct fnl_bitmap_size *bitmap_size;
	unsigned denominator;
};

struct font_fnl {
	struct font super;
	struct fnl_font *font;
	unsigned nr_bitmap_sizes;
	struct fnl_bitmap_size *bitmap_sizes;
	unsigned nr_sizes;
	struct fnl_font_size *sizes;
};

// .fnl faces are 4x-supersampled bitmaps, so denominator 4 renders a face at its
// native point size (face height == 4 * size). Any other denominator reproduces
// the requested em size out of a *different* face, and the faces are NOT scaled
// copies of each other: they are hand-tuned per point size. Measured on
// Tsumamigui3Font.fnl, full-width kanji keep a constant ink/height ratio (0.625)
// across all ten faces, but latin/cyrillic glyphs are optically scaled — ink
// ratio 0.312 in the native-24 face vs 0.260 in the native-48 face. So
// upscaling a small face (e.g. face_h=96 at denominator 2 for size 48) yields the
// right em but latin/cyrillic ~20% too large, while kanji look identical. That is
// invisible in Japanese and glaring in a translation.
// Ranking: closest size first, then denominator nearest 4 (native), then the
// larger denominator (downscaling a bigger face is sharper than upscaling).
static bool fnl_size_is_better(const struct fnl_font_size *cand,
		const struct fnl_font_size *best, float cand_diff, float best_diff)
{
	if (cand_diff < best_diff - 0.01f)
		return true;
	if (cand_diff > best_diff - 0.01f && cand_diff < best_diff + 0.01f) {
		int cand_off = abs((int)cand->denominator - 4);
		int best_off = abs((int)best->denominator - 4);
		if (cand_off != best_off)
			return cand_off < best_off;
		return cand->denominator > best->denominator;
	}
	return false;
}

static struct font_size *fnl_font_get_size(struct font *_font, float size)
{
	struct font_fnl *font = (struct font_fnl*)_font;

	float min_diff = 9999;
	struct fnl_font_size *closest = &font->sizes[0];
	for (unsigned i = 0; i < font->nr_sizes; i++) {
		float diff = fabsf(font->sizes[i].super.size - size);
		if (fnl_size_is_better(&font->sizes[i], closest, diff, min_diff)) {
			min_diff = diff;
			closest = &font->sizes[i];
		}
	}
	/*
	 * ВЫБОР FACE. Точное совпадение размера НЕЛЬЗЯ покупать любой ценой: у
	 * Tsumamigui 3 для кегля 14 «точный» кандидат — это натив 42 (face_h=168),
	 * сжатый в 12 раз, и цифра в нём вырождается в 3×4 пикселя чернил на блок
	 * 5×14 (трейс `XSYS4_GLYPH_TRACE`). Ближайший по духу кандидат — натив 18
	 * (face_h=72) при denominator 5, то есть кегль 14.4: там цифра 4×6 на блоке
	 * 6×14, и на экране она читается. Поэтому среди кандидатов, попадающих в
	 * допуск по размеру (±15 %), берём того, у кого denominator ближе к 4
	 * (нативный рендер), и лишь при равенстве — того, кто ближе по размеру.
	 * Кегли, у которых face есть нативно (18, 22, 24, 26, 30, 34, 36, 40, 42, 48
	 * у Tsumamigui), правило НЕ трогает: у них denominator и так 4. Проверено:
	 * строка окна сообщений бит в бит прежняя (509 px, 47 групп).
	 * Замер по эталону слота SAVE: COMMENT 41 → 48 px при эталоне 47.
	 * `XSYS4_FNL_RANK=<доля>` меняет допуск, `XSYS4_FNL_RANK=0` возвращает
	 * прежнее правило «точный размер любой ценой».
	 */
	{
		const char *e = getenv("XSYS4_FNL_RANK");
		float tol = size * (e ? strtof(e, NULL) : 0.15f);
		if (tol > 0.0f) {
		struct fnl_font_size *best = NULL;
		for (unsigned i = 0; i < font->nr_sizes; i++) {
			if (fabsf(font->sizes[i].super.size - size) > tol)
				continue;
			if (!best) { best = &font->sizes[i]; continue; }
			int co = abs((int)font->sizes[i].denominator - 4);
			int bo = abs((int)best->denominator - 4);
			if (co < bo || (co == bo &&
			    fabsf(font->sizes[i].super.size - size) < fabsf(best->super.size - size)))
				best = &font->sizes[i];
		}
		if (best)
			closest = best;
		}
	}
	if (getenv("XSYS4_FNL_TRACE"))
		NOTICE("FNL get_size req=%.1f -> chosen=%.2f (denom=%u, face_h=%u)",
		       size, closest->super.size, closest->denominator, closest->bitmap_size->face->height);
	return &closest->super;
}

static struct font_size *fnl_font_get_size_round_down(struct font *_font, float size)
{
	struct font_fnl *font = (struct font_fnl*)_font;

	float min_diff = 9999;
	struct fnl_font_size *closest = &font->sizes[0];
	for (unsigned i = 0; i < font->nr_sizes; i++) {
		if (font->sizes[i].super.size > size)
			continue;
		float diff = fabsf(font->sizes[i].super.size - size);
		if (diff < min_diff) {
			min_diff = diff;
			closest = &font->sizes[i];
		}
	}
	return &closest->super;
}

static float fnl_font_get_actual_size(struct font *font, float size)
{
	return fnl_font_get_size(font, size)->size;
}

static float fnl_font_get_actual_size_round_down(struct font *font, float size)
{
	return fnl_font_get_size_round_down(font, size)->size;
}

static struct fnl_bitmap_glyph *fnl_get_bitmap_glyph(struct fnl_bitmap_size *bitmap, unsigned index)
{
	if (bitmap->glyphs[index])
		return bitmap->glyphs[index];

	struct fnl_glyph *glyph = &bitmap->face->glyphs[index];
	uint8_t *data = fnl_glyph_data(bitmap->face->font->fnl, glyph);

	const int glyph_w = fnl_glyph_stride(glyph) * 8;
	const int glyph_h = bitmap->face->height;
	if (getenv("XSYS4_GLYPH_TRACE"))
		NOTICE("GLYPHDATA idx=%u: face_h=%u glyph_h=%u glyph_w(advance)=%u stride*8=%d",
		       index, bitmap->face->height, glyph->height, glyph->width, glyph_w);

	struct fnl_bitmap_glyph *out = xmalloc(sizeof(struct fnl_bitmap_glyph));
	out->width = glyph_w;
	out->height = glyph_h;
	out->advance = glyph->width;
	out->pixels = xcalloc(glyph_w, glyph_h);

	// expand 1-bit bitmap to 8-bit
	for (unsigned row = 0; row < glyph_h; row++) {
		uint8_t *dst = out->pixels + ((glyph_h - 1) - row) * glyph_w;
		for (unsigned col = 0; col < glyph->width; col++) {
			unsigned i = row * glyph_w + col;
			bool on = data[i/8] & (1 << (7 - i % 8));
			dst[col] = on ? 255 : 0;
		}
	}

	free(data);
	bitmap->glyphs[index] = out;
	return out;
}

#define GLYPH_BORDER_SIZE 4

// Downscale filter tuning (see comment in fnl_font_get_glyph); overridable
// for A/B comparison via XSYS4_FNL_GAMMA=<float> and XSYS4_FNL_SHARP=0/1.
//
// A plain box average of the 4x supersamples is what AliceSoft's engine does, so the
// physically correct value here is gamma = 1.0, i.e. no gamma at all. An earlier
// calibration (FINDINGS §5i) landed on 0.7, but that was measured while the bold /
// outline dilation was a no-op (§5k): the gamma was silently standing in for the
// weight the dilation pass failed to add. With max-plus dilation fixed, the boost
// became a double count and left our half-tone fringe ~18% wider than the original's.
//
// Re-swept against BOTH references at once (FINDINGS §5v), mean |pixel difference|,
// smaller = closer; one clean minimum at 1.0 in both, no plateau:
//   gamma        0.7     0.9    0.95   *1.0*   1.05    1.1
//   back log    3.844   3.259  3.169  *3.073* 3.167  3.280
//   msg window 21.144  19.750 19.526 *19.305* 19.782 20.341
// At 1.0 the message-window line is pixel-identical to the original (ink 921 = 921,
// mean |difference| over the glyphs 0.054), and the back log's stroke widths and line
// widths match exactly. The smoothstep was added to stop glyphs clumping, but that had
// two real causes — the wrong .fnl face (ink ~20% too wide for its advance) and the
// dropped outline advance — both since fixed, so the sharpening is no longer needed.
static float fnl_gamma = 1.0f;
static bool fnl_sharpen = false;
static bool fnl_filter_initialized = false;

static void fnl_filter_init(void)
{
	if (fnl_filter_initialized)
		return;
	fnl_filter_initialized = true;
	const char *g = getenv("XSYS4_FNL_GAMMA");
	if (g)
		fnl_gamma = strtof(g, NULL);
	const char *s = getenv("XSYS4_FNL_SHARP");
	if (s)
		fnl_sharpen = atoi(s) != 0;
}

static bool fnl_font_get_glyph(struct font_size *_size, struct glyph *glyph, uint32_t code, enum font_weight weight)
{
	fnl_filter_init();
	struct fnl_font_size *size = (struct fnl_font_size*)_size;
	unsigned index = fnl_char_to_index(code);
	if (index > size->bitmap_size->nr_glyphs)
		return false;
	if (!size->bitmap_size->face->glyphs[index].data_pos)
		return false;

	struct fnl_bitmap_glyph *fullsize = fnl_get_bitmap_glyph(size->bitmap_size, index);
	const unsigned block_width = fullsize->width / size->denominator;
	const unsigned block_height = fullsize->height / size->denominator;
	const unsigned width = block_width + GLYPH_BORDER_SIZE*2;
	const unsigned height = block_height + GLYPH_BORDER_SIZE*2;
	const unsigned off_x = GLYPH_BORDER_SIZE;
	const unsigned off_y = GLYPH_BORDER_SIZE;

	// Sample each pixel in size->denominator`-sized blocks and compute the average.
	// TODO: no need to sample every pixel; 4 should be fine?
	uint8_t *pixels = xcalloc(1, width * height);
	unsigned *acc = xcalloc(block_width * block_height, sizeof(unsigned));
	for (unsigned i = 0; i < block_width * block_height; i++) {
		unsigned dst_row = i / block_width;
		unsigned dst_col = i % block_width;
		for (unsigned r = 0; r < size->denominator; r++) {
			unsigned src_row = dst_row * size->denominator + r;
			for (unsigned c = 0; c < size->denominator; c++) {
				unsigned src_col = dst_col * size->denominator + c;
				acc[i] += fullsize->pixels[src_row*fullsize->width + src_col];
			}
		}
		unsigned avg = acc[i] / (size->denominator * size->denominator);
		// The .fnl faces are 4x-supersampled bitmaps (face height = 4*point
		// size); AliceSoft's engine box-filters them down, i.e. linear
		// coverage, and the plain average above already is that filter. The
		// tuning below is off by default (gamma 1.0, no sharpening) and only
		// exists for A/B comparison — see the calibration table above.
		float t = avg / 255.0f;
		if (fnl_sharpen)
			t = t*t*(3.0f - 2.0f*t); // smoothstep, fixed point at 0.5
		if (fnl_gamma != 1.0f)
			t = powf(t, fnl_gamma);
		uint8_t p = (uint8_t)(255.0f * t + 0.5f);
		pixels[(dst_row+off_y)*width + (dst_col+off_x)] = p;
	}

	if (getenv("XSYS4_GLYPH_TRACE") && code >= '0' && code <= '9') {
		int ix0=9999,ix1=-1,iy0=9999,iy1=-1;
		for (unsigned y = 0; y < height; y++)
			for (unsigned x = 0; x < width; x++)
				if (pixels[y*width+x] > 32) {
					if ((int)x<ix0) ix0=x; if ((int)x>ix1) ix1=x;
					if ((int)y<iy0) iy0=y; if ((int)y>iy1) iy1=y;
				}
		NOTICE("GLYPH '%c': face_h=%u denom=%u block=%ux%u чернила %dx%d (y %d..%d)",
		       (char)code, size->bitmap_size->face->height, size->denominator,
		       block_width, block_height,
		       ix1>=0?ix1-ix0+1:0, iy1>=0?iy1-iy0+1:0, iy0, iy1);
	}
	gfx_init_texture_rmap(&glyph->t[weight], width, height, pixels);
	glyph->rect.x = off_x;
	glyph->rect.y = off_y;
	glyph->rect.w = block_width;
	glyph->rect.h = block_height;
	glyph->advance = fullsize->advance / (float)size->denominator;

	free(pixels);
	free(acc);
	return true;
}

static float fnl_font_size_char(struct font_size *_size, uint32_t code)
{
	struct fnl_font_size *size = (struct fnl_font_size*)_size;
	unsigned index = fnl_char_to_index(code);
	if (index > size->bitmap_size->nr_glyphs)
		return 0.0;
	if (!size->bitmap_size->face->glyphs[index].data_pos)
		return 0.0;
	float r = (float)size->bitmap_size->face->glyphs[index].width / (float)size->denominator;
	if (code == '\t')
		r *= 2.88;
	return r;
}

static float fnl_font_size_char_kerning(struct font_size *size, uint32_t code,
		uint32_t code_next)
{
	return fnl_font_size_char(size, code);
}

struct font *fnl_font_load(struct fnl *lib, unsigned index)
{
	if (index >= lib->nr_fonts)
		return NULL;

	struct fnl_font *lib_font = &lib->fonts[index];
	struct font_fnl *font = xcalloc(1, sizeof(struct font_fnl));

	// initialize bitmap sizes
	font->nr_bitmap_sizes = lib_font->nr_faces;
	font->bitmap_sizes = xcalloc(font->nr_bitmap_sizes, sizeof(struct fnl_bitmap_size));
	for (unsigned i = 0; i < font->nr_bitmap_sizes; i++) {
		struct fnl_bitmap_size *bitsize = &font->bitmap_sizes[i];
		bitsize->face = &lib_font->faces[i];
		bitsize->nr_glyphs = bitsize->face->nr_glyphs;
		bitsize->glyphs = xcalloc(bitsize->nr_glyphs, sizeof(struct fnl_bitmap_glyph*));
	}

	// initialize render sizes (downscaled)
	font->nr_sizes = font->nr_bitmap_sizes * 12;
	font->sizes = xcalloc(font->nr_sizes, sizeof(struct fnl_font_size));
	for (unsigned i = 0; i < font->nr_bitmap_sizes; i++) {
		struct fnl_bitmap_size *bitsize = &font->bitmap_sizes[i];
		struct fnl_font_size *sizes = &font->sizes[i*12];
		for (unsigned i = 0; i < 12; i++) {
			sizes[i].bitmap_size = bitsize;
			sizes[i].super.size = bitsize->face->height / (float)(i+1);
			sizes[i].super.font = &font->super;
			sizes[i].denominator = i + 1;
		}
	}

	if (getenv("XSYS4_FNL_TRACE")) {
		// Сверяем ДВА «размера» face: по высоте битмапа и по advance глифов.
		unsigned i_zero = fnl_char_to_index('0');
		unsigned i_kanji = fnl_char_to_index(0x8c8e);  // 月 в Shift-JIS
		for (unsigned i = 0; i < font->nr_bitmap_sizes; i++) {
			struct fnl_font_face *f = font->bitmap_sizes[i].face;
			unsigned a0 = i_zero < f->nr_glyphs ? f->glyphs[i_zero].width : 0;
			unsigned ak = i_kanji < f->nr_glyphs ? f->glyphs[i_kanji].width : 0;
			NOTICE("FNL face[%u]: height=%u (h/4=%u)  advance '0'=%u (/4=%.1f)  advance 月=%u (/4=%.1f)",
			       i, f->height, f->height / 4, a0, a0 / 4.0, ak, ak / 4.0);
		}
	}
	font->super.charmap = CHARMAP_SJIS;
	font->super.get_size = fnl_font_get_size;
	font->super.get_actual_size = fnl_font_get_actual_size;
	font->super.get_actual_size_round_down = fnl_font_get_actual_size_round_down;
	font->super.get_glyph = fnl_font_get_glyph;
	font->super.size_char = fnl_font_size_char;
	font->super.size_char_kerning = fnl_font_size_char_kerning;

	if (!game_rance7_mg)
		gfx_text_advance_edges = true;

	return &font->super;
}
