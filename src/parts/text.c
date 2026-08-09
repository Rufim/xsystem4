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
#include <string.h>

#include "system4.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "xsystem4.h"
#include "parts_internal.h"

static int extract_sjis_char(const char *src, char *dst)
{
	if (SJIS_2BYTE(*src)) {
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = '\0';
		return 2;
	}
	dst[0] = src[0];
	dst[1] = '\0';
	return 1;
}

struct string *parts_text_line_get(struct parts_text_line *line)
{
	struct string *s = make_string("", 0);
	for (int i = 0; i < line->nr_chars; i++) {
		string_append_cstr(&s, line->chars[i].ch, strlen(line->chars[i].ch));
	}
	return s;
}

struct string *parts_text_get(struct parts_text *t)
{
	struct string *s = make_string("", 0);
	for (int i = 0; i < t->nr_lines; i++) {
		if (i > 0)
			string_push_back(&s, '\n');
		struct parts_text_line *line = &t->lines[i];
		for (int i = 0; i < line->nr_chars; i++) {
			string_append_cstr(&s, line->chars[i].ch, strlen(line->chars[i].ch));
		}
	}
	return s;
}

static Point text_style_offset(struct text_style *ts)
{
	int x = max(ts->bold_width, ts->edge_left) * ts->scale_x;
	int y = max(ts->bold_width, ts->edge_up);
	return (Point){x,y};
}

static const char *parts_text_append_char(struct parts_text *t, const char *str)
{
	if (*str == '\n') {
		t->lines = xrealloc_array(t->lines, t->nr_lines, t->nr_lines + 1,
				sizeof(struct parts_text_line));
		t->lines[t->nr_lines].height = text_style_height(&t->ts);
		t->nr_lines++;
		return str + 1;
	}

	struct parts_text_line *line = &t->lines[t->nr_lines - 1];
	line->chars = xrealloc_array(line->chars, line->nr_chars, line->nr_chars + 1,
			sizeof(struct parts_text_char));
	struct parts_text_char *ch = &line->chars[line->nr_chars++];

	ch->off = text_style_offset(&t->ts);
	int len = extract_sjis_char(str, ch->ch);
	int width = ceilf(text_style_width(&t->ts, ch->ch));
	int height = text_style_height(&t->ts);
	gfx_init_texture_rgba(&ch->t, width, height, (SDL_Color){0,0,0,0});
	ch->advance = gfx_render_textf(&ch->t, 0, 0, ch->ch, &t->ts, false);
	if (getenv("XSYS4_NUMTEXT_TRACE")) {
		NOTICE("TXTW \"%s\" (bytes %02x %02x) face=%u size=%.1f bold=%.2f edge=%.2f "
		       "spacing=%.2f -> width=%d advance=%.2f h=%d",
		       ch->ch, (unsigned char)ch->ch[0], (unsigned char)ch->ch[1],
		       t->ts.face, t->ts.size, t->ts.bold_width, t->ts.edge_left,
		       t->ts.font_spacing, width, ch->advance, height);
	}

	line->width += ch->advance;
	line->height = max(line->height, height);
	return str + len;
}

void parts_text_append(struct parts *parts, struct parts_text *t, struct string *text)
{
	if (!t->nr_lines) {
		t->lines = xcalloc(1, sizeof(struct parts_text_line));
		t->nr_lines = 1;
	}

	const char *msgp = text->text;
	while (*msgp) {
		msgp = parts_text_append_char(t, msgp);
	}

	/*
	 * Ширина строки — сумма ШАГОВ, а шаг несёт в себе `字間隔`: интервал идёт
	 * МЕЖДУ символами, и после последнего его быть не должно. Числовая часть
	 * это уже учитывает (`total - num->space` в parts_numeral_font_update), а
	 * текстовая — нет, и при отрицательном интервале часть выходила уже
	 * нарисованного текста.
	 *
	 * Живой случай — счётчики на экране Garage у Dohna («TALENT 3/5»,
	 * «GARAGE 0/10»). Слэш там отдельная текстовая часть: `テキスト = "／"`,
	 * кегль 14, `字間隔 = -4`. Один символ, шаг 8 при нарисованных 12 — и
	 * горизонтальный бокс ставил знаменатель на 4 px левее, надвигая цифры на
	 * слэш. Замер по кадру оригинала: у него дробь шире нашей ровно на эти 4 px.
	 */
	float f_width = 0;
	int height = 0;
	for (int i = 0; i < t->nr_lines; i++) {
		float line_w = t->lines[i].width;
		if (t->lines[i].nr_chars > 0)
			line_w -= t->ts.font_spacing;
		f_width = max(f_width, line_w);
		if (i > 0)
			height += t->line_space;
		height += t->lines[i].height;
	}
	int width = ceilf(f_width);
	if (!width || !height)
		return;

	parts_set_dims(parts, &t->common, width, height);
}

void parts_text_free(struct parts_text *t)
{
	gfx_delete_texture(&t->common.texture);
	for (int i = 0; i < t->nr_lines; i++) {
		struct parts_text_line *line = &t->lines[i];
		for (int i = 0; i < line->nr_chars; i++) {
			gfx_delete_texture(&line->chars[i].t);
		}
		free(line->chars);
	}
	free(t->lines);
}

static void parts_text_rerender(struct parts *parts, struct parts_text *t)
{
	if (!t->nr_lines)
		return;
	struct string *text = parts_text_get(t);
	parts_text_free(t);
	t->lines = NULL;
	t->nr_lines = 0;
	parts_text_append(parts, t, text);
	free_string(text);
	parts_dirty(parts);
}

static void parts_text_clear(struct parts *parts, int state)
{
	struct parts_text *text = parts_get_text(parts, state);
	parts_text_free(text);
	text->lines = NULL;
	text->nr_lines = 0;
	text->cursor.x = 0;
	text->cursor.y = 0;
}

bool PE_SetText(int parts_no, struct string *text, int state)
{
	// XSYS4_SETTEXT_TRACE=1 — кто и какой текст кладёт в текстовые части.
	// Отделяет «игра не пишет подпись» от «пишет, но не в ту часть/состояние»
	// (на экране остаётся образец из раскладки — «パーツテキスト», «TALENT»).
	if (getenv("XSYS4_SETTEXT_TRACE"))
		NOTICE("SETTEXT no=%d state=%d text='%s'", parts_no, state,
		       text ? display_sjis0(text->text) : "(nil)");
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	parts_text_clear(parts, state);
	parts_text_append(parts, parts_get_text(parts, state), text);
	return true;
}

// Текст, ранее заданный SetText/AddPartsText. Ixseal-игры читают его обратно
// (CTextParts@Text::get), напр. чтобы измерить ширину строки.
struct string *PE_GetTextPartsText(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return string_ref(&EMPTY_STRING);

	struct parts *parts = parts_get(parts_no);
	return parts_text_get(parts_get_text(parts, state));
}

bool PE_AddPartsText(int parts_no, struct string *text, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	parts_text_append(parts, parts_get_text(parts, state), text);
	return true;
}

/*
 * `Parts_SetTextEnableTag(number, enable, state)` — разрешить РАЗМЕТКУ в тексте
 * части. Игра ставит флаг и читает его обратно (`Parts_IsTextEnableTag`), то есть
 * гейтится по нему, поэтому хранить значение обязательно.
 *
 * Без этих двух функций Haha Ranman валилась в debug-REPL на первом же экране
 * («Unimplemented HLL function: PartsEngine.Parts_SetTextEnableTag»), и запустить
 * её можно было только костылём `XSYS4_LENIENT_HLL=1`.
 *
 * ★Сам разбор тегов НЕ реализован: движок кладёт строку как есть. Если игра
 * включит теги и подаст разметку, она отрисуется буквально — тогда и надо будет
 * реализовать разбор, но сперва увидеть такой текст живьём (одноразовый WARNING
 * ниже это покажет).
 */
void PE_SetTextEnableTag(int parts_no, bool enable, int state)
{
	if (!parts_state_valid(--state))
		return;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	text->enable_tag = enable;
	if (enable) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("Parts_SetTextEnableTag: разбор тегов в тексте части не "
				"реализован — разметка отрисуется как обычный текст");
		}
	}
}

bool PE_IsTextEnableTag(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return false;
	return parts->states[state].text.enable_tag;
}

bool PE_SetPartsTextSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	parts_set_surface_area(parts, &text->common, x, y, w, h);
	return true;
}

//bool PE_DeletePartsTopTextLine(int PartsNumber, int State);

bool PE_SetFont(int parts_no, int type, int size, int r, int g, int b, float bold_weight, int edge_r, int edge_g, int edge_b, float edge_weight, int state)
{
	if (!parts_state_valid(--state))
		return false;
	if (getenv("XSYS4_FONT_TRACE")) NOTICE("SETFONT part=%d type=%d size=%d edge=%.1f bold=%.2f", parts_no, type, size, edge_weight, bold_weight);

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	text->ts.face = type;
	/*
	 * НОМИНАЛ → 実サイズ. Через `Ｐ＿フォント設定` игра передаёт НОМИНАЛЬНЫЙ кегль, а
	 * наш выбор face меряет размер ВЫСОТОЙ КОРОБКИ битмапа, которая у всех faces
	 * FNL ровно в 1.5 раза больше настоящего кегля (замер по десяти faces: высота
	 * 72/96/120/192 против полноширинного advance 48/64/81/129 — отношение
	 * 1.48…1.50). Поэтому запрос «14» без множителя давал face с настоящим кеглем
	 * ≈9.4, и текст в слотах SAVE выходил вдвое мельче эталонного.
	 * Проверено по двум эталонам оригинала с ОДНОЙ строкой «1231231» на разных
	 * кеглях: чернила 13 px при запросе 14 и 16 px при запросе 16, то есть у
	 * оригинала высота чернил ≈ запрошенному числу. С множителем 1.5 запрос 14
	 * попадает на face 88 (коробка 22, настоящий кегль 14.8) — он и даёт 13.
	 * НО ПРОВЕРКА ЗАМЕРОМ ЭТУ ПРАВКУ ОТВЕРГЛА, и множитель по умолчанию 1.0.
	 * С ×1.5 строка DATE («8月01日 朝», полноширинная) раздувается вдвое —
	 * 178 px против 96 у эталона, — а цифры комментария почти не меняются
	 * (55 против 47 по ширине, 9 против 13 по высоте). Причина в самом FNL:
	 * у него ASCII-глифы приземистые на ЛЮБОМ face (отношение высота чернил к
	 * шагу ≈ 1.1), а у оригинала цифры высокие и узкие (13/6.7 ≈ 1.9). Такой
	 * пропорции в этом шрифте нет ни при каком кегле, значит оригинал берёт для
	 * ASCII в GUI-частях НЕ FNL. См. FINDINGS §5ab.
	 * Ручка `XSYS4_PARTS_SIZE_MUL` оставлена для A/B.
	 */
	{
		const char *e = getenv("XSYS4_PARTS_SIZE_MUL");
		text->ts.size = e ? size * strtof(e, NULL) : size;
	}
	text->ts.color = (SDL_Color) { r, g, b, 255 };
	/*
	 * `フォント太さ` — ДИЛАТАЦИЯ ШТРИХА в пикселях, и её место — `bold_width`.
	 * Раньше толщина уходила только в `weight` (= 太さ×1000), а тот при 0.4 даёт
	 * 400, то есть обычное начертание (gfx_int_to_font_weight: всё до 550 —
	 * NORMAL). Итог: жирность не рисовалась вовсе, а заголовки выходили тоньше и
	 * у́же оригинала. `weight` оставляем как был — он для игр, где вес приходит
	 * тысячными долями.
	 *
	 * Эталон: заголовок достижения «Nicely Dohna» (`太さ = 0.4`, `縁取り = 0`,
	 * `字間隔 = −4`) — у оригинала строка 94 px и 783 тёмных пикселя, у нас было
	 * 72 px и 562. Разница по ширине ложилась ровно на промежутки между глифами:
	 * +10 px на шести буквах «Nicely» (5 промежутков) и +8 на пяти «Dohna»
	 * (4 промежутка) — по 2 px на промежуток, то есть ceil(0.4) = 1 px на сторону
	 * (см. text_style_advance_padding).
	 * Ручка XSYS4_NO_BOLD_WIDTH=1 возвращает прежнее поведение для A/B.
	 */
	text->ts.bold_width = getenv("XSYS4_NO_BOLD_WIDTH") ? 0.0f : bold_weight;
	text->ts.weight = bold_weight * 1000;
	text->ts.edge_color = (SDL_Color) { edge_r, edge_g, edge_b, 255 };
	text_style_set_edge_width(&text->ts, edge_weight);
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetPartsFontType(int parts_no, int type, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	if (text->ts.face == type)
		return true;
	text->ts.face = type;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetPartsFontSize(int parts_no, int size, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	if (text->ts.size == size)
		return true;
	text->ts.size = size;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetPartsFontColor(int parts_no, int r, int g, int b, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	SDL_Color color = { r, g, b, 255 };
	if (!memcmp(&text->ts.color, &color, sizeof(SDL_Color)))
		return true;
	text->ts.color = color;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetPartsFontBoldWeight(int parts_no, float bold_weight, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	if (text->ts.bold_width == bold_weight)
		return true;
	text->ts.bold_width = bold_weight;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetPartsFontEdgeColor(int parts_no, int r, int g, int b, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	SDL_Color color = { r, g, b, 255 };
	if (!memcmp(&text->ts.edge_color, &color, sizeof(SDL_Color)))
		return true;
	text->ts.edge_color = color;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetPartsFontEdgeWeight(int parts_no, float edge_weight, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	float old_left = text->ts.edge_left;
	float old_up = text->ts.edge_up;
	float old_right = text->ts.edge_right;
	float old_down = text->ts.edge_down;
	text_style_set_edge_width(&text->ts, edge_weight);
	if (text->ts.edge_left == old_left && text->ts.edge_up == old_up &&
	    text->ts.edge_right == old_right && text->ts.edge_down == old_down)
		return true;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetTextCharSpace(int parts_no, int char_space, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	if (text->ts.font_spacing == char_space)
		return true;
	text->ts.font_spacing = char_space;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetTextLineSpace(int parts_no, int line_space, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	if (text->line_space == line_space)
		return true;
	text->line_space = line_space;
	parts_text_rerender(parts, text);
	return true;
}

// Геттеры интервалов текста. Нужны бэклогу Tsumamigui 3
// (CBackLogView@CreateBacklogTextList -> Ｐ＿テキスト字間隔取得/行間隔取得);
// без них Parts_GetTextCharSpace бросал «Unimplemented HLL function» и ронял
// движок в отладчик при открытии BACK LOG.
int PE_GetTextCharSpace(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0;
	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	return (int)text->ts.font_spacing;
}

int PE_GetTextLineSpace(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0;
	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	return text->line_space;
}

// Fill the actual font properties of a text part's state, for
// PartsEngine.GetPartsTextFontProperty. Tsumamigui 3 calls that getter on the
// message-window text parts to learn the font size for the BACK LOG, then builds
// the log at the returned size — so returning the part's real ts.size (instead of
// a hardcoded default) makes the log font match the message window. Only fills
// non-NULL outputs that we actually track; leaves the caller's defaults otherwise.
void PE_GetTextFontProps(int parts_no, int state, int *type, int *size,
		int *r, int *g, int *b, float *weight, float *edge_weight,
		int *edge_r, int *edge_g, int *edge_b)
{
	if (!parts_state_valid(--state))
		return;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return;
	struct parts_text *text = parts_get_text(parts, state);
	if (type) *type = text->ts.face;
	if (size) *size = (int)text->ts.size;
	if (r) *r = text->ts.color.r;
	if (g) *g = text->ts.color.g;
	if (b) *b = text->ts.color.b;
	if (weight) *weight = text->ts.weight / 1000.0f;
	// Обводка/жирность нужны игре не только для рисования: BACK LOG считает
	// высоту юнита как CASCharSpriteProperty@GetPixelHeight() =
	// size(до чётного вверх) + 2×max(ceil(太さ), ceil(縁取り幅)) — байткод
	// FUNC 5558 (разбор — FINDINGS §5j). У Tsumamigui 3 текст-парт лога
	// в バックログ.pactex несёт
	// 太さ 0.5 и 縁取り 2.0, что даёт ровно 30 + 2×2 = 34 — замеренную высоту
	// бокса оригинала. Отдавать сюда выдуманные значения — ломать раскладку.
	if (edge_weight) *edge_weight = text->ts.edge_left;
	if (edge_r) *edge_r = text->ts.edge_color.r;
	if (edge_g) *edge_g = text->ts.edge_color.g;
	if (edge_b) *edge_b = text->ts.edge_color.b;
}


