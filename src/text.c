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

#include <math.h>

#include "system4.h"
#include "system4/fnl.h"
#include "system4/hashtable.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "gfx/gfx.h"
#include "gfx/font.h"
#include "xsystem4.h"
#include "android_bridge.h"

/*
 * NOTE: There are two different text rendering APIs: SACT2 and DrawGraph.
 *
 *       SACT2 is a stateless API which uses the text_metrics structure.
 *       It is implemented via the single function gfx_render_text and
 *       supports bold and outlined font styles.
 *
 *       DrawGraph is a stateful API that uses the font_metrics structure.
 *       The various gfx_setfont_* and gfx_draw_text* functions implement
 *       this API. It supports bold, underline and strikethrough font
 *       styles and two different rendering modes (pixel map and alpha map)
 */

#define DEFAULT_FONT_GOTHIC "fonts/VL-Gothic-Regular.ttf"
#define DEFAULT_FONT_MINCHO "fonts/HanaMinA.ttf"

#define MAX_FNL_FONTS 32
struct font *font_ttf[2] = {0};
struct font *font_fnl[MAX_FNL_FONTS] = {0};

// Controls whether edge widths are taken into account during text layout.
bool gfx_text_advance_edges = false;

static struct font *load_font(enum font_face type)
{
	static const char *local_font_paths[] = {
		[FONT_GOTHIC] = DEFAULT_FONT_GOTHIC,
		[FONT_MINCHO] = DEFAULT_FONT_MINCHO
	};
	static const char *default_font_paths[] = {
		[FONT_GOTHIC] = XSYS4_DATA_DIR "/" DEFAULT_FONT_GOTHIC,
		[FONT_MINCHO] = XSYS4_DATA_DIR "/" DEFAULT_FONT_MINCHO
	};

	struct font *font;
	// user specified font
	if (config.font_paths[type] && (font = ft_font_load(config.font_paths[type])))
		return font;
	// installed default font
	if ((font = ft_font_load(default_font_paths[type])))
		return font;
	// local default font
	if ((font = ft_font_load(local_font_paths[type])))
		return font;
	ERROR("Failed to load %s font", type == FONT_GOTHIC ? "gothic" : "mincho");
}

static bool font_initialized = false;

void gfx_font_init(void)
{
	if (font_initialized)
		return;
	ft_font_init();
	font_ttf[FONT_GOTHIC] = load_font(FONT_GOTHIC);
	font_ttf[FONT_MINCHO] = load_font(FONT_MINCHO);

	if (config.fnl_path) {
		struct fnl *fnl = fnl_open(config.fnl_path);
		if (!fnl)
			ERROR("Failed to load .fnl font library '%s'", config.fnl_path);
		if (!fnl->nr_fonts)
			ERROR("No fonts in .fnl font library '%s'", config.fnl_path);
		for (unsigned i = 0; i < fnl->nr_fonts && i < MAX_FNL_FONTS; i++) {
			font_fnl[i] = fnl_font_load(fnl, i);
		}
	}
	font_initialized = true;
}

static struct font *get_font(unsigned face)
{
	if (face > 255) {
		face -= 256;
		if (face >= MAX_FNL_FONTS || !font_fnl[face]) {
			WARNING("Invalid fnl face: %u", face);
			return font_ttf[0];
		}
		return font_fnl[face];
	}
	// Prefer the game's embedded .fnl font for the standard faces (0=gothic,
	// 1=mincho) when it is available: this matches the original glyphs, metrics
	// and line spacing far better than a substitute system TTF. Falls back to
	// FreeType when no .fnl is loaded (games without one are unaffected).
	// Toggle off with XSYS4_NO_FNL to compare.
	if (face <= 1 && font_fnl[face] && !getenv("XSYS4_NO_FNL"))
		return font_fnl[face];
	if (face > 1)
		return font_ttf[0];
	return font_ttf[face];
}

struct font_size *gfx_font_get_size(unsigned face, float size)
{
	struct font *font = get_font(face);
	return font->get_size(font, size);
}

static struct font_size *text_style_font_size(struct text_style *ts)
{
	if (ts->font_size)
		return ts->font_size;
	return ts->font_size = gfx_font_get_size(ts->face, ts->size);
}

static struct glyph *font_get_glyph(struct font_size *size, uint32_t code, enum font_weight weight)
{
	if (!size->glyph_table)
		size->glyph_table = ht_create(4096);
	// return cached glyph if available
	struct ht_slot *slot = ht_put_int(size->glyph_table, code, NULL);
	if (slot->value && ((struct glyph*)slot->value)->t[weight].handle)
		return slot->value;
	// alloc if necessary
	if (!slot->value)
		slot->value = xcalloc(1, sizeof(struct glyph));
	// render glyph
	if (!size->font->get_glyph(size, slot->value, code, weight))
		return NULL;
	return slot->value;
}

/*
 * Межбуквенный интервал (letter-spacing) для латиницы и кириллицы.
 *
 * Пропорциональные письменности (латиница, кириллица) в System 4-играх часто
 * рисуются через шрифты, где боковых отступов почти нет — особенно в фан-
 * переводах, где кириллица берётся из полноширинных ячеек японского FNL
 * (напр. Daiteikoku): шаг = ширина ячейки, но широкие чернила (Ж/М/Щ) съедают
 * зазор почти до нуля. Добавляем небольшой интервал глифам латиницы/кириллицы;
 * CJK (кандзи/кана) и полуширинную кану НЕ трогаем — они плотные by design.
 *
 * Письменность определяем по коду символа + charmap лица (SJIS/Unicode),
 * а не по ширине глифа — так корректно обрабатываются и полноширинные
 * кириллические ячейки (Daiteikoku), и узкая пропорциональная латиница.
 *
 * Величина — доля от кегля, двухступенчатая: крупному кеглю (>= порога)
 * нужна бОльшая разрядка, чем мелкому. База — config.font_letter_spacing /
 * env XSYS4_LETTER_SPACING (0 полностью отключает); крупный кегль —
 * XSYS4_LETTER_SPACING_LARGE (деф. 0.10); порог — XSYS4_LETTER_SPACING_SIZE
 * (деф. 25: сообщения Daiteikoku 26 → крупный, Tsumamigui 24 → базовый).
 */
// Межбуквенный интервал: base для обычного кегля, large для крупного (>= threshold).
// −1 = ещё не инициализировано (ленивая инициализация из env при первом обращении).
// На Android переопределяется в рантайме через gfx_set_letter_spacing (настройка «Шрифт»).
static float ls_base = -1.0f, ls_large = -1.0f, ls_threshold = -1.0f;

// Нормализация: отрицательная база → 0; база 0 = полное отключение (large тоже 0);
// large не может быть меньше базы.
static void ls_normalize(void)
{
	if (ls_base < 0.0f)
		ls_base = 0.0f;
	if (ls_base <= 0.0f)
		ls_large = 0.0f;
	else if (ls_large < ls_base)
		ls_large = ls_base;
}

static void ls_init_from_env_once(void)
{
	if (ls_base >= 0.0f)
		return; // уже задано (env или рантайм-сеттером)
	// Flat 0.03 for all sizes. The message window has NO auto-wrap (proven: a long
	// single message renders on one line and overruns the box); its line breaks are
	// authored for native widths, so a large letter-spacing pushed wrapped lines
	// past the right edge. 0.03 keeps glyphs from clumping (our FNL render needs a
	// little spacing) while staying tight enough that the authored lines still fit.
	const char *env = getenv("XSYS4_LETTER_SPACING");
	ls_base = env ? strtof(env, NULL) : 0.03f;
	env = getenv("XSYS4_LETTER_SPACING_LARGE");
	ls_large = env ? strtof(env, NULL) : 0.03f;
	env = getenv("XSYS4_LETTER_SPACING_SIZE");
	ls_threshold = env ? strtof(env, NULL) : 25.0f;
	ls_normalize();
}

void gfx_set_letter_spacing(float base, float large, float threshold)
{
	ls_base = base;
	ls_large = large;
	ls_threshold = threshold;
	ls_normalize();
}

static float letter_spacing_ratio(float size)
{
	ls_init_from_env_once();
	return size >= ls_threshold ? ls_large : ls_base;
}

// Письменности, которым добавляем интервал: латиница и кириллица.
static bool is_spacing_script(struct font_size *size, uint32_t code)
{
	if (size->font->charmap == CHARMAP_SJIS) {
		// полуширинная ASCII-латиница (буквы/цифры/пунктуация)
		if (code >= 0x20 && code <= 0x7e)
			return true;
		// кириллица в SJIS (блок JIS X 0208, ряд 7): 0x8440..0x8491
		if (code >= 0x8440 && code <= 0x8491)
			return true;
		return false;
	}
	// Unicode: Basic Latin + Latin-1 + Extended-A/B, и кириллица
	if (code >= 0x20 && code <= 0x24f)
		return true;
	if (code >= 0x400 && code <= 0x4ff)
		return true;
	return false;
}

float gfx_letter_spacing_extra(struct font_size *size, uint32_t code, float advance)
{
	float r = letter_spacing_ratio(size->size);
	if (r <= 0.0f || advance <= 0.0f)
		return 0.0f;
	if (!is_spacing_script(size, code))
		return 0.0f;
	// em-относительный интервал (доля от кегля) с полом в 1px: на мелком
	// кегле доля даёт <1px, а int-пути игр (SP_GetFontWidth и т.п.)
	// отбрасывают дробную часть — без пола интервал исчезал бы именно там,
	// где текст сливается сильнее всего.
	return fmaxf(1.0f, size->size * r);
}

static float font_size_char(struct font_size *size, uint32_t code)
{
	float advance = size->font->size_char(size, code);
	return advance + gfx_letter_spacing_extra(size, code, advance);
}

static uint32_t char_to_code(const char *ch, enum charmap charmap)
{
	if (charmap == CHARMAP_SJIS)
		return sjis_code(ch);
	int c;
	sjis_char2unicode(ch, &c);
	if (c == '^' && game_rance02_mg)
		return 0xE9; // é
	// half-width katakana 'no' (ﾉ)
	if (c == 0xFF89 && game_rance6_mg)
		return 0xE9; // é
	return c;
}

float gfx_size_char(struct text_style *ts, const char *ch)
{
	struct font_size *size = text_style_font_size(ts);
	return font_size_char(size, char_to_code(ch, size->font->charmap));
}

float gfx_size_char_kerning(struct text_style *ts, uint32_t code, uint32_t code_next)
{
	struct font_size *size = text_style_font_size(ts);
	float advance = size->font->size_char_kerning(size, code, code_next);
	return advance + gfx_letter_spacing_extra(size, code, advance);
}

float gfx_size_text(struct text_style *ts, const char *text)
{
	struct font_size *size = text_style_font_size(ts);
	// NOTE: letter spacing / advance is our own mechanism — the glyph EDGE (outline)
	// width must NOT enter it, or adding an outline would widen letter spacing. Only
	// the bold width contributes here; the edge is drawn (gfx_render_textf) but never
	// reserves advance.
	float edge_advance = gfx_text_advance_edges ? ceilf(ts->bold_width) : 0.0f;
	float x = 0.0f;
	while (*text) {
		x += font_size_char(size, char_to_code(text, size->font->charmap));
		x += edge_advance;
		text = sjis_skip_char(text);
	}
	return x;
}

float gfx_get_actual_font_size(unsigned face, float size)
{
	struct font *font = get_font(face);
	return font->get_actual_size(font, size);
}

float gfx_get_actual_font_size_round_down(unsigned face, float size)
{
	struct font *font = get_font(face);
	return font->get_actual_size_round_down(font, size);
}

float _gfx_render_text(Texture *dst, char *msg, struct text_render_metrics *tm)
{
	bridge_newfont_draw();
	bridge_text_drawn(msg);
	float pos_x = tm->x;
	int pos_y = tm->y + tm->font_size->y_offset;

	static int text_trace = -1;
	if (text_trace < 0)
		text_trace = getenv("XSYS4_TEXT_TRACE") ? 1 : 0;

	while (*msg) {
		pos_x += tm->edge_spacing;
		// get glyph for character
		float scale_x = *msg == ' ' ? tm->space_scale_x : tm->scale_x;
		uint32_t code = char_to_code(msg, tm->font_size->font->charmap);
		msg += SJIS_2BYTE(*msg) ? 2 : 1;
		struct glyph *glyph = font_get_glyph(tm->font_size, code, tm->weight);
		if (!glyph)
			continue;

		if (text_trace)
			NOTICE("TXT code=%04X fsize=%.1f adv=%.2f lsp=%.2f rect=%dx%d scale_x=%.3f xxs=%.3f spacing=%.2f edge_sp=%.2f pos=%.1f",
			       code, tm->font_size->size, glyph->advance,
			       gfx_letter_spacing_extra(tm->font_size, code, glyph->advance),
			       glyph->rect.w, glyph->rect.h, scale_x,
			       config.text_x_scale, tm->font_spacing, tm->edge_spacing, pos_x);

		// render glyph
		Texture *t = &glyph->t[tm->weight];
		if (tm->mode == RENDER_COPY) {
			float x = pos_x - glyph->rect.x;
			int y = pos_y - glyph->rect.y;
			gfx_draw_glyph(dst, x, y, t, tm->color, config.text_x_scale, tm->edge_width, false);
		} else if (tm->mode == RENDER_BLENDED) {
			float x = pos_x - glyph->rect.x;
			int y = pos_y - glyph->rect.y;
			gfx_draw_glyph(dst, x, y, t, tm->color, config.text_x_scale, tm->edge_width, true);
		} else if (tm->mode == RENDER_PMAP) {
			gfx_draw_glyph_to_pmap(dst, pos_x, pos_y, t, glyph->rect, tm->color, config.text_x_scale);
		} else if (tm->mode == RENDER_AMAP) {
			gfx_draw_glyph_to_amap(dst, pos_x, pos_y, t, glyph->rect, config.text_x_scale);
		}

		// advance
		pos_x += glyph->advance * scale_x * config.text_x_scale + tm->font_spacing;
		// тот же межбуквенный интервал, что возвращает font_size_char,
		// чтобы путь цельной строки был консистентен с поглифовым
		pos_x += gfx_letter_spacing_extra(tm->font_size, code, glyph->advance);
		pos_x += tm->edge_spacing;
	}
	return pos_x - tm->x;
}

enum font_weight gfx_int_to_font_weight(int weight)
{
	// 0 -> 550 = LIGHT
	if (weight <= 550)
		return FONT_WEIGHT_NORMAL;
	// 551 -> 999 = BOLD
	if (weight <= 999)
		return FONT_WEIGHT_BOLD;
	// 1000 = LIGHT
	if (weight == 1000)
		return FONT_WEIGHT_NORMAL;
	// x001 -> x550 = MEDIUM
	// x551 -> x999 = HEAVY-BOLD
	// NOTE: we don't distinguish between MEDIUM and BOLD (they are nearly identical)
	return (weight % 1000) < 551 ? FONT_WEIGHT_BOLD : FONT_WEIGHT_HEAVY;
}

float gfx_render_textf(Texture *dst, float x, int y, char *msg, struct text_style *ts, bool blend)
{
	enum font_weight weight = gfx_int_to_font_weight(ts->weight);
	struct font_size *font_size = text_style_font_size(ts);
	float edge_width = text_style_edge_width(ts);

	// Edge (outline) width must NOT feed into advance (our letter-spacing mechanism);
	// only bold width does. The outline is still drawn below when edge_width > 0.
	float edge_advance = gfx_text_advance_edges ? ceilf(ts->bold_width) : 0.f;

	enum text_render_mode mode = blend ? RENDER_BLENDED : RENDER_COPY;
	if (edge_width > 0.01f) {
		struct text_render_metrics metrics = {
			.x = x,
			.y = y,
			.color = ts->edge_color,
			.weight = weight,
			.edge_width = edge_width,
			.scale_x = ts->scale_x,
			.space_scale_x = ts->space_scale_x,
			.font_spacing = ts->font_spacing,
			.edge_spacing = edge_advance,
			.mode = mode,
			.font_size = font_size,
		};
		_gfx_render_text(dst, msg, &metrics);
		mode = RENDER_BLENDED;  // core text is blended on top of the edge
	}
	struct text_render_metrics metrics = {
		.x = x,
		.y = y,
		.color = ts->color,
		.weight = weight,
		.edge_width = ts->bold_width,
		.scale_x = ts->scale_x,
		.space_scale_x = ts->space_scale_x,
		.font_spacing = ts->font_spacing,
		.edge_spacing = edge_advance,
		.mode = mode,
		.font_size = font_size,
	};
	return _gfx_render_text(dst, msg, &metrics);
}

int gfx_render_text(Texture *dst, float x, int y, char *msg, struct text_style *ts, bool blend)
{
	return lroundf(gfx_render_textf(dst, x, y, msg, ts, blend));
}

void gfx_render_dash_text(Texture *dst, struct text_style *ts)
{
	Texture glyph;
	gfx_init_texture_rgba(&glyph, dst->w, dst->h, COLOR(0, 0, 0, 0));

	// draw a horizontal line
	int x = lroundf(ts->size * 0.12f);
	int w = dst->w - x * 2;
	int h = max(1, lroundf(ts->size * 0.06f));
	int y = (ts->size - h) / 2;
	gfx_fill(&glyph, x, y, w, h, 255, 255, 255);

	float edge_width = text_style_edge_width(ts);
	if (edge_width > 0.01f) {
		gfx_draw_glyph(dst, 0, 0, &glyph, ts->edge_color, 1.f, edge_width, false);
	}
	gfx_draw_glyph(dst, 0, 0, &glyph, ts->color, 1.f, ts->bold_width, false);

	gfx_delete_texture(&glyph);
}

struct font_metrics {
	int size;
	enum font_face face;
	int weight;
	bool underline;
	bool strikeout;
	int space;
	SDL_Color color;
};

// current font state
static struct font_metrics font_metrics = {
	.size = 16,
	.face = FONT_GOTHIC,
	.weight = FW_BOLD,
	.underline = false,
	.strikeout = false,
	.space = 0,
	.color = {
		.r = 255,
		.g = 255,
		.b = 255,
		.a = 255
	}
};

static void gfx_draw_text(Texture *dst, int x, int y, char *text, enum text_render_mode mode)
{
	struct font_size *font_size = gfx_font_get_size(font_metrics.face, font_metrics.size);
	struct text_render_metrics metrics = {
		.x = x,
		.y = y,
		.color = font_metrics.color,
		.weight = gfx_int_to_font_weight(font_metrics.weight),
		.edge_width = 0.0f,
		.scale_x = 1.0f,
		.space_scale_x = 1.0f,
		.font_spacing = font_metrics.space,
		.mode = mode,
		.font_size = font_size,
	};
	// TODO: underline and strikethrough
	_gfx_render_text(dst, text, &metrics);
}

void gfx_draw_text_to_amap(Texture *dst, int x, int y, char *text)
{
	gfx_draw_text(dst, x, y, text, RENDER_AMAP);
}

void gfx_draw_text_to_pmap(Texture *dst, int x, int y, char *text)
{
	gfx_draw_text(dst, x, y, text, RENDER_PMAP);
}

bool gfx_set_font_size(unsigned int size)
{
	font_metrics.size = size;
	return true;
}

int gfx_get_font_size(void)
{
	return font_metrics.size;
}

bool gfx_set_font_face(enum font_face face)
{
	font_metrics.face = face;
	return true;
}

enum font_face gfx_get_font_face(void)
{
	return font_metrics.face;
}

bool gfx_set_font_weight(int weight)
{
	font_metrics.weight = weight;
	return true;
}

int gfx_get_font_weight(void)
{
	return font_metrics.weight;
}

bool gfx_set_font_underline(bool on)
{
	font_metrics.underline = on;
	return true;
}

bool gfx_get_font_underline(void)
{
	return font_metrics.underline;
}

bool gfx_set_font_strikeout(bool on)
{
	font_metrics.strikeout = on;
	return true;
}

bool gfx_get_font_strikeout(void)
{
	return font_metrics.strikeout;
}

bool gfx_set_font_space(int space)
{
	font_metrics.space = space;
	return true;
}

int gfx_get_font_space(void)
{
	return font_metrics.space;
}

bool gfx_set_font_color(SDL_Color color)
{
	font_metrics.color = color;
	return true;
}

SDL_Color gfx_get_font_color(void)
{
	return font_metrics.color;
}

void gfx_set_font_name(const char *name)
{
	static const char mincho_name[] = { 0x82, 0x6c, 0x82, 0x72, 0x20, 0x96, 0xbe, 0x92, 0xa9, 0 };
	static const char gothic_name[] = { 0x82, 0x6c, 0x82, 0x72, 0x20, 0x83, 0x53, 0x83, 0x56, 0x83, 0x62, 0x83, 0x4e, 0 };
	if (!strcmp(name, mincho_name)) {
		font_metrics.face = FONT_MINCHO;
	} else if (!strcmp(name, gothic_name)) {
		font_metrics.face = FONT_GOTHIC;
	} else {
		WARNING("Unhandled font name: \"%s\"", display_sjis0(name));
	}
}
