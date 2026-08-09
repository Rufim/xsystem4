/* Copyright (C) 2023 Nunuhara Cabbage <nunuhara@haniwa.technology>
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

#include <assert.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "system4/ain.h"
#include "system4/hashtable.h"
#include "system4/string.h"
#include "system4/ex.h"
#include "system4/utfsjis.h"
#include "system4/archive.h"

#include "vm/heap.h"
#include "vm/page.h"
#include "parts.h"
#include "asset_manager.h"
#include "gfx/font.h"
#include "input.h"
#include "xsystem4.h"
#include "hll.h"
#include "iarray.h"

static void PartsEngine_ModuleInit(void)
{
	PE_Init();
}

static void textbox_reset_all(void);

static void PartsEngine_ModuleFini(void)
{
	PE_Reset();
	textbox_reset_all();
}

/*
 * ТЕКСТОВЫЕ ПОЛЯ ввода GUI-тулкита: `TextBox` (однострочное) и `MultiTextBox`
 * (многострочное). Настоящего виджета ввода у движка нет и в обозримом будущем не
 * будет — это полноценный редактор с кареткой, выделением и IME.
 *
 * Но ВСЕ функции обоих семейств были `HLL_TODO_EXPORT`, то есть `.fun = NULL`, а
 * это не заглушка, а ДЫРА: вызов уводит движок в отладочный REPL (FINDINGS §5y).
 * И они достижимы: у Tsumamigui 3 `SetTextBoxText`/`GetTextBoxText` — это поле
 * комментария на экране SAVE, у Escalayer всё семейство `MultiTextBox` строго
 * достижимо (экран анкеты).
 *
 * Реализация — ЧЕСТНОЕ ХРАНИЛИЩЕ: что игра записала, то геттер и вернёт. Так
 * поле ведёт себя как пустая (или заранее заполненная игрой) строка, которую
 * пользователь не может отредактировать. Это заметно меньшее зло, чем и REPL,
 * и выдуманный ввод. О том, что поле не рисуется и не принимает ввод, говорим
 * ВСЛУХ один раз — приём из `SetComponentScrollPos*LinkNumber` (§5x).
 */
#define MAX_TEXTBOXES 64

struct textbox_state {
	bool used;
	bool multi;              // MultiTextBox против TextBox — пространства номеров разные
	int parts_no;
	struct string *text;
	struct string *cg_name;
	int max_length;
	int sel_r, sel_g, sel_b;
	int bg_r, bg_g, bg_b;             // 背景色
	int ro_bg_r, ro_bg_g, ro_bg_b;    // 読み取り専用背景色
	int frame_r, frame_g, frame_b;    // 枠色
	int width, height;
	int char_space;
	bool read_only;
	int caret;               // позиция каретки В СИМВОЛАХ (не в байтах: текст UTF-8)
	// свойства шрифта, как их задаёт Set*FontProperty
	int font_type, font_size, r, g, b, edge_r, edge_g, edge_b;
	float bold_weight, edge_weight;
};

static struct textbox_state textboxes[MAX_TEXTBOXES];
static struct textbox_state *focused_textbox = NULL;

static struct textbox_state *textbox_get(int parts_no, bool multi, bool create)
{
	struct textbox_state *free_slot = NULL;
	for (int i = 0; i < MAX_TEXTBOXES; i++) {
		if (textboxes[i].used && textboxes[i].parts_no == parts_no
		    && textboxes[i].multi == multi)
			return &textboxes[i];
		if (!textboxes[i].used && !free_slot)
			free_slot = &textboxes[i];
	}
	if (!create)
		return NULL;
	if (!free_slot) {
		WARNING("PartsEngine: текстовых полей больше %d, часть %d пропущена",
			MAX_TEXTBOXES, parts_no);
		return NULL;
	}
	*free_slot = (struct textbox_state){
		.used = true, .multi = multi, .parts_no = parts_no,
		// Дефолты как у пустого поля: пределов нет, цвет выделения — системный синий.
		.max_length = 0, .sel_r = 0, .sel_g = 0, .sel_b = 128,
		.font_size = 16, .r = 255, .g = 255, .b = 255,
		.ro_bg_r = 150, .ro_bg_g = 150, .ro_bg_b = 150,
	};
	return free_slot;
}

static void textbox_reset_all(void)
{
	focused_textbox = NULL;
	for (int i = 0; i < MAX_TEXTBOXES; i++) {
		if (textboxes[i].text)
			free_string(textboxes[i].text);
		if (textboxes[i].cg_name)
			free_string(textboxes[i].cg_name);
		textboxes[i] = (struct textbox_state){0};
	}
}

static void textbox_set_string(struct string **dst, struct string *src)
{
	if (*dst)
		free_string(*dst);
	*dst = src ? string_ref(src) : NULL;
}

static void textbox_out_string(struct string **out, struct string *src)
{
	if (!out)
		return;
	if (*out)
		free_string(*out);
	*out = src ? string_ref(src) : string_ref(&EMPTY_STRING);
}

/*
 * ОТРИСОВКА поля. Геометрия снята ЗАМЕРОМ с эталона оригинала
 * (`screenshots/orig-textbox-zoom.png` — экран SAVE Tsumamigui 3, поле COMMENT
 * диалога «Do you want to save here?»), а не подобрана на глаз:
 *   • подложка ровно 186×22 px = `サイズ` того же парта в `セーブ確認.pactex`,
 *     цвет (61,149,220) = его же `背景色` — совпал побитово;
 *   • каретка — ЧЁРНЫЙ столбик шириной 1 px и высотой ровно `フォントサイズ` (16),
 *     левый край на 3 px правее края подложки, по вертикали по центру:
 *     (22 − 16)/2 = 3 px сверху и снизу.
 * Мигание каретки по ОДНОМУ кадру эталона определить нельзя, поэтому рисуем её
 * постоянной. Это не догадка, а отказ гадать: постоянная каретка отличается от
 * мигающей только фазой, а выдуманный период был бы виден.
 */
#define TEXTBOX_INSET 3

/*
 * Мигание каретки. Пользователь смотрел оригинал живьём: каретка мигает,
 * полупериод — полсекунды (полный цикл ≈ 1 с). Это же почти совпадает с системным
 * дефолтом Windows `GetCaretBlinkTime()` = 530 мс, откуда оригинал её и берёт,
 * так что число согласовано с двумя независимыми источниками.
 * Ручка `XSYS4_CARET_BLINK_MS` — для сверки (0 = не мигать).
 */
#define TEXTBOX_BLINK_MS 500
static int caret_blink_timer = 0;
static bool caret_visible = true;

static int textbox_blink_ms(void)
{
	static int ms = -1;
	if (ms < 0) {
		const char *e = getenv("XSYS4_CARET_BLINK_MS");
		ms = e ? atoi(e) : TEXTBOX_BLINK_MS;
		if (ms < 0)
			ms = 0;
	}
	return ms;
}

/*
 * КЕГЛЬ ГЛИФОВ ≠ КЕГЛЬ ПОЛЯ: у поля ввода глифы МЕЛЬЧЕ объявленного номинала.
 * `フォントサイズ` в `.pactex` — номинал, им считается геометрия (высота каретки
 * ровно 16 и по эталону совпала), а глифы оригинал рисует кеглем 12.
 *
 * ★МНОЖИТЕЛЬ ПЕРЕСЧИТАН 2.0 → 0.75 (FINDINGS §5af). Прежние 2.0 калибровались,
 * когда лица 0/1 подменялись шрифтом из `.fnl`: у FNL ASCII приземистый, и
 * увеличение вдвое компенсировало это по одному измерению. После возврата лиц 0/1
 * системному шрифту тот же множитель давал В ДВА РАЗА КРУПНЕЕ оригинала (нашёл
 * пользователь живьём). Направление оказалось ОБРАТНЫМ, и это видно сразу по двум
 * эталонам: текст слота при номинале 14 даёт чернила 11 и шаг 7, а поле при
 * номинале 16 — чернила 9 и шаг 10, то есть объявляет БОЛЬШЕ, а рисует МЕНЬШЕ.
 * Оба числа поля сходятся на кегле 12 одновременно: у системного гротеска на 12 px
 * чернила цифры 9, advance 6, плюс надбавка за обводку `2×ceil(1.25) = 4` даёт
 * шаг ровно 10. Отсюда 12/16 = 0.75.
 * ЗАМЕР ПОСЛЕ: шаг 10.00 = 10.00, суммарная ширина строки `1231231` 60 = 60,
 * групп 7 = 7, чернила 8 против 9 (порогозависимый пиксель).
 * ХВОСТ: чернила стоят на 1 px выше эталона (отн. 5..12 против 6..14 в подложке
 * 22 px). Поправка вертикали ниже — эмпирическая доля от кегля, и она
 * калибровалась при множителе 2.0. Специально НЕ подкручиваю: это тот же класс,
 * что горизонтальный остаток §5z, а подгонка по одному измерению здесь уже
 * приводила к ложным выводам. Ручка `XSYS4_TB_SIZE_MUL` — для сверки.
 */
static float textbox_size_mul(void)
{
	static float mul = -1.0f;
	if (mul < 0.0f) {
		const char *e = getenv("XSYS4_TB_SIZE_MUL");
		mul = e ? strtof(e, NULL) : 0.75f;
		if (mul <= 0.0f)
			mul = 0.75f;
	}
	return mul;
}

static struct text_style textbox_style(const struct textbox_state *t)
{
	struct text_style style = {
		.face = t->font_type,
		.size = t->font_size * textbox_size_mul(),
		.bold_width = t->bold_weight,
		.weight = FW_NORMAL,
		.edge_left = t->edge_weight, .edge_up = t->edge_weight,
		.edge_right = t->edge_weight, .edge_down = t->edge_weight,
		.color = { t->r, t->g, t->b, 255 },
		.edge_color = { t->edge_r, t->edge_g, t->edge_b, 255 },
		.scale_x = 1.0f,
		.space_scale_x = 1.0f,
		.font_spacing = t->char_space,
		.font_size = NULL,
	};
	return style;
}

/*
 * Ширина первых `nr_chars` символов — РОВНО тем же `gfx_size_text`, которым меряет
 * себя путь отрисовки (`build_copy_text` → `gfx_render_text`). Считать по формуле
 * `TSM.GetFontWidth` тут нельзя: она с надбавкой на обводку на КАЖДЫЙ глиф, и на
 * шести символах давала ~180 px вместо 60 — каретка уезжала за правый край поля и
 * молча не рисовалась (проверено замером против эталона).
 */
static int textbox_text_width(const struct textbox_state *t, int nr_chars)
{
	if (!t->text || !t->text->size || nr_chars <= 0)
		return 0;
	struct text_style style = textbox_style(t);
	const char *p = t->text->text;
	for (int i = 0; *p && i < nr_chars; i++)
		p += SJIS_2BYTE((unsigned char)*p) ? 2 : 1;
	if (p == t->text->text)
		return 0;
	/*
	 * Меряем `gfx_size_text` — ровно тем, чем меряет себя путь отрисовки. Две
	 * другие формулы проверены замером и ОТБРОШЕНЫ: сумма
	 * `ceil(gfx_size_char)` даёт 7 px на символ (каретка на 45 вместо 63, эта
	 * величина не несёт надбавки), а формула `TSM.GetFontWidth` с надбавкой на
	 * обводку на каждый глиф — наоборот, ~30 px на символ.
	 * Остаток: у оригинала 10 px на символ, у нас 9.5 — каретка отстаёт на 3 px
	 * к шестому символу. Причина та же, что у сдвига ink на 2 px влево:
	 * наш левый бэринг под обводку 1 px против 3 у оригинала (класс §5f, ФИКС 3).
	 */
	int len = p - t->text->text;
	char *prefix = xmalloc(len + 1);
	memcpy(prefix, t->text->text, len);
	prefix[len] = '\0';
	int w = (int)ceilf(gfx_size_text(&style, prefix));
	free(prefix);
	return w;
}

static int textbox_nr_chars(const struct textbox_state *t)
{
	if (!t->text)
		return 0;
	int n = 0;
	for (const char *p = t->text->text; *p; n++)
		p += SJIS_2BYTE((unsigned char)*p) ? 2 : 1;
	return n;
}

static void textbox_render(struct textbox_state *t)
{
	if (!t || t->width <= 0 || t->height <= 0)
		return;
	int no = t->parts_no;
	PE_ClearPartsConstructionProcess(no, 1);

	// Подложка: либо CG (если игра его задала), либо сплошная заливка 背景色 /
	// 読み取り専用背景色 — так же, как выбирает оригинал.
	if (t->cg_name && t->cg_name->size) {
		PE_AddCreateCGToProcess(no, t->cg_name, 1);
	} else {
		int r = t->read_only ? t->ro_bg_r : t->bg_r;
		int g = t->read_only ? t->ro_bg_g : t->bg_g;
		int b = t->read_only ? t->ro_bg_b : t->bg_b;
		PE_AddCreateToPartsConstructionProcess(no, t->width, t->height, 1);
		PE_AddFillToPartsConstructionProcess(no, 0, 0, t->width, t->height, r, g, b, 1);
	}

	/*
	 * Вертикаль текста. Центрируем ЯЧЕЙКУ шрифта по её же метрике
	 * (`text_style_height`, та самая, которой меряет себя `build_copy_text`), а
	 * затем поднимаем на size/16: чернила сидят в ячейке НИЖЕ её середины.
	 * Дробь перекалибрована после починки выбора face (§5ab): раньше при кегле 32
	 * нужно было поднимать на 4 px, теперь запрошенные 32 ложатся на НАТИВНЫЙ
	 * face 30, и хватает 2 px.
	 * У оригинала ink стоит ровно по центру поля (6..16 в поле 0..21, центр 11).
	 * Дробь 4/32 = 1/8 калибрована на ЕДИНСТВЕННОМ эталоне (поле COMMENT
	 * Tsumamigui 3) — на MultiTextBox Escalayer её надо перепроверить.
	 */
	struct text_style vstyle = textbox_style(t);
	int glyph_size = (int)(t->font_size * textbox_size_mul());
	int text_y = (int)((t->height - text_style_height(&vstyle)) / 2.0f) - glyph_size / 16;
	{	// ручка калибровки вертикали поля против эталона (свип, см. §5z)
		const char *e = getenv("XSYS4_TB_TEXT_Y");
		if (e)
			text_y = atoi(e);
	}
	if (t->text && t->text->size) {
		/*
		 * DrawText, а НЕ CopyText. `CopyText` перед отрисовкой обнуляет альфу
		 * под текстом (`gfx_fill_with_alpha(..., 0)`) — он для текстовых партов,
		 * где под текстом ничего своего нет. В поле ввода это пробивало дыру в
		 * нашей подложке: между буквами просвечивала рамка CG, а не 背景色.
		 * `DrawText` кладёт глифы поверх с блендингом — ровно то, что нужно.
		 */
		PE_AddDrawTextToPartsConstructionProcess(no, TEXTBOX_INSET, text_y, t->text,
			t->font_type, (int)(t->font_size * textbox_size_mul()),
			t->r, t->g, t->b, t->bold_weight,
			t->edge_r, t->edge_g, t->edge_b, t->edge_weight,
			t->char_space, 0, 1);
	}
	// Каретка — только у поля с фокусом: в оригинале её видно ровно в том поле,
	// куда идёт ввод.
	if (getenv("XSYS4_TB_TRACE"))
		NOTICE("TBRENDER part=%d focus=%s caret_visible=%d caret=%d text='%s' w=%d size=%d",
		       no, focused_textbox == t ? "да" : "нет", (int)caret_visible, t->caret,
		       t->text ? t->text->text : "(nil)", t->width, t->font_size);
	if (focused_textbox == t && !t->read_only && caret_visible) {
		int cx = TEXTBOX_INSET + textbox_text_width(t, t->caret);
		// Не даём каретке исчезнуть, если текст упёрся в правый край: настоящее
		// поле в такой ситуации прокручивает содержимое, у нас пока прижимаем
		// каретку к краю — молча пропадать она не должна.
		if (cx > t->width - 2)
			cx = t->width - 2;
		if (cx < 0)
			cx = 0;
		/*
		 * Заливка С АЛЬФОЙ, а не обычная. `build_copy_text` перед отрисовкой
		 * ОБНУЛЯЕТ альфу под текстом (`gfx_fill_with_alpha(..., 0)`), а
		 * `AddFill` красит только RGB и альфу не трогает (§5u) — каретка,
		 * попавшая в очищенную область, выходила полностью прозрачной. На
		 * ПУСТОМ поле её было видно, потому что операции CopyText там нет, и
		 * ровно поэтому дефект так долго выглядел как «каретка пропадает
		 * после набора текста».
		 */
		/*
		 * Каретка живёт по НОМИНАЛУ, а не по кеглю глифов: у эталона она ровно
		 * 16 px (= `フォントサイズ`) и центрирована в поле ((22−16)/2 = 3).
		 * От вертикали текста её надо отвязать — та считается по ячейке кегля 32
		 * и уходит в минус.
		 */
		int caret_y = (t->height - t->font_size) / 2;
		if (caret_y < 0)
			caret_y = 0;
		PE_AddFillWithAlphaToPartsConstructionProcess(no, cx, caret_y, 1, t->font_size,
							      0, 0, 0, 255, 1);
	}
	PE_BuildPartsConstructionProcess(no, 1);
}

/*
 * ВВОД. Пока поле в фокусе, символы SDL идут сюда через `register_input_handler`
 * (тот же механизм, которым пользуется библиотека `InputString`). Список
 * завершающих клавиш игра держит у себя (`終了キー = { 2, 27 }` в `セーブ確認.pactex`
 * — правая кнопка мыши и ESC), поэтому Enter/Escape мы не перехватываем: их
 * обрабатывает игровой диспетчер, а поле лишь теряет фокус.
 */
static void textbox_input_handler(const char *utf8)
{
	struct textbox_state *t = focused_textbox;
	if (!t || t->read_only || !utf8 || !*utf8)
		return;
	char *sjis = utf2sjis(utf8, strlen(utf8));
	if (!sjis)
		return;
	// 最大文字数 = 0 означает «без предела».
	if (t->max_length > 0 && textbox_nr_chars(t) >= t->max_length) {
		free(sjis);
		return;
	}
	struct string *ins = make_string(sjis, strlen(sjis));
	char ins_text[64];
	snprintf(ins_text, sizeof(ins_text), "%s", sjis);
	free(sjis);
	if (!t->text)
		t->text = string_ref(&EMPTY_STRING);
	// Вставка в позицию каретки — в БАЙТАХ, посчитанных по символам.
	int byte_pos = 0;
	{
		const char *p = t->text->text;
		for (int i = 0; i < t->caret && *p; i++)
			p += SJIS_2BYTE((unsigned char)*p) ? 2 : 1;
		byte_pos = p - t->text->text;
	}
	struct string *head = string_copy(t->text, 0, byte_pos);
	struct string *tail = string_copy(t->text, byte_pos, t->text->size - byte_pos);
	struct string *joined = string_concatenate(head, ins);
	struct string *full = string_concatenate(joined, tail);
	free_string(head); free_string(tail); free_string(joined); free_string(ins);
	free_string(t->text);
	t->text = full;
	// Каретка двигается на СТОЛЬКО СИМВОЛОВ, сколько пришло: одно событие
	// SDL_TEXTINPUT несёт целую строку (составной ввод, вставка, наш тестовый
	// `text <строка>`), а не один символ. Инкремент на единицу оставлял каретку
	// после первой буквы — замер против эталона это и поймал.
	for (const char *q = ins_text; *q; t->caret++)
		q += SJIS_2BYTE((unsigned char)*q) ? 2 : 1;
	caret_visible = true;
	caret_blink_timer = 0;
	textbox_render(t);
}

/*
 * Фаза мигания. Зовётся из PartsEngine.Update, поэтому идёт по игровому времени
 * (`passed_time`), а не по стенным часам: на паузе и при промотке каретка ведёт
 * себя так же, как остальная анимация партов.
 */
static void textbox_update_caret(int passed_time)
{
	if (!focused_textbox)
		return;
	int period = textbox_blink_ms();
	if (period == 0) {
		if (!caret_visible) {
			caret_visible = true;
			textbox_render(focused_textbox);
		}
		return;
	}
	caret_blink_timer += passed_time;
	if (caret_blink_timer < period)
		return;
	caret_blink_timer %= period;
	caret_visible = !caret_visible;
	textbox_render(focused_textbox);
}

static void textbox_set_focus(struct textbox_state *t)
{
	if (focused_textbox == t)
		return;
	struct textbox_state *old = focused_textbox;
	focused_textbox = t;
	if (t) {
		// Свежий фокус — каретка видна сразу, фаза с нуля (иначе поле могло бы
		// «открыться» в невидимой половине цикла и выглядеть неактивным).
		caret_visible = true;
		caret_blink_timer = 0;
		register_input_handler(textbox_input_handler);
		SDL_StartTextInput();
	} else {
		clear_input_handler();
		SDL_StopTextInput();
	}
	/*
	 * Уход фокуса = подтверждение ввода. Игра ждёт сообщение FIXED (тип 25),
	 * чтобы прочитать введённое: `C_SAVE_CONFIRM@CommentFixedEvent` (FUNC 7975)
	 * зовёт `Ｐ＿テキストボックス＿テキスト取得`, прячет поле и возвращает подсказку.
	 * Без него набранный комментарий в сейв не попадал вовсе.
	 */
	if (old)
		PE_SendFixedEvent(old->parts_no);
	// Каретка появляется/исчезает — перерисовать оба поля.
	if (old)
		textbox_render(old);
	if (t)
		textbox_render(t);
}

/*
 * Клик по части: если это текстовое поле — забирает фокус, если любая другая —
 * фокус снимается. Зовётся из `parts_update_mouse` (src/parts/input.c) там же,
 * где рассылается MOUSE_CLICK.
 */
void PE_textbox_click(int parts_no)
{
	struct textbox_state *t = textbox_get(parts_no, false, false);
	if (!t)
		t = textbox_get(parts_no, true, false);
	textbox_set_focus(t);
}

// Backspace/Delete приходят не текстом, а кодом клавиши, поэтому их снимает
// PE_Update (движок опрашивает клавиатуру сам).
void PE_textbox_key(int vk)
{
	struct textbox_state *t = focused_textbox;
	if (!t || t->read_only)
		return;
	int nr = textbox_nr_chars(t);
	switch (vk) {
	case VK_RETURN:
		// Штатное подтверждение однострочного поля. У многострочного Enter — это
		// перевод строки, поэтому подтверждаем только TextBox.
		if (!t->multi)
			PE_SendFixedEvent(t->parts_no);
		return;
	case VK_BACK:
		if (t->caret <= 0)
			return;
		t->caret--;
		break;
	case VK_DELETE:
		if (t->caret >= nr)
			return;
		break;
	case VK_LEFT:
		if (t->caret > 0) { t->caret--; textbox_render(t); }
		return;
	case VK_RIGHT:
		if (t->caret < nr) { t->caret++; textbox_render(t); }
		return;
	default:
		return;
	}
	// Удаление символа в позиции каретки.
	const char *p = t->text ? t->text->text : NULL;
	if (!p)
		return;
	for (int i = 0; i < t->caret && *p; i++)
		p += SJIS_2BYTE((unsigned char)*p) ? 2 : 1;
	int start = p - t->text->text;
	int len = SJIS_2BYTE((unsigned char)*p) ? 2 : 1;
	struct string *head = string_copy(t->text, 0, start);
	struct string *tail = string_copy(t->text, start + len, t->text->size - start - len);
	struct string *full = string_concatenate(head, tail);
	free_string(head); free_string(tail); free_string(t->text);
	t->text = full;
	textbox_render(t);
}

// Общие тела: `multi` разводит два семейства, всё остальное совпадает дословно.
static void tb_set_size(int no, bool multi, int w, int h)
{
	struct textbox_state *t = textbox_get(no, multi, true);
	if (t) { t->width = w; t->height = h; textbox_render(t); }
}

static void tb_set_font_property(int no, bool multi, int type, int size, int r, int g, int b,
				 float bold, int er, int eg, int eb, float ew)
{
	struct textbox_state *t = textbox_get(no, multi, true);
	if (!t)
		return;
	t->font_type = type; t->font_size = size;
	t->r = r; t->g = g; t->b = b;
	t->bold_weight = bold;
	t->edge_r = er; t->edge_g = eg; t->edge_b = eb;
	t->edge_weight = ew;
	textbox_render(t);
}

static void tb_get_font_property(int no, bool multi, int *type, int *size, int *r, int *g, int *b,
				 float *bold, int *er, int *eg, int *eb, float *ew)
{
	// Ref-выходы заполняем ВСЕГДА, даже если поля нет: незаполненный выход
	// заставляет игру читать мусор (§7 FINDINGS — так пропадал текст кнопок).
	struct textbox_state *t = textbox_get(no, multi, false);
	static const struct textbox_state empty = { .font_size = 16, .r = 255, .g = 255, .b = 255 };
	if (!t)
		t = (struct textbox_state *)&empty;
	if (type) *type = t->font_type;
	if (size) *size = t->font_size;
	if (r) *r = t->r;
	if (g) *g = t->g;
	if (b) *b = t->b;
	if (bold) *bold = t->bold_weight;
	if (er) *er = t->edge_r;
	if (eg) *eg = t->edge_g;
	if (eb) *eb = t->edge_b;
	if (ew) *ew = t->edge_weight;
}

static void tb_set_text(int no, bool multi, struct string *text)
{
	struct textbox_state *t = textbox_get(no, multi, true);
	if (!t)
		return;
	textbox_set_string(&t->text, text);
	t->caret = textbox_nr_chars(t);   // как в оригинале: курсор в конец подставленного текста
	textbox_render(t);
}

static void tb_get_text(int no, bool multi, struct string **out)
{
	struct textbox_state *t = textbox_get(no, multi, false);
	textbox_out_string(out, t ? t->text : NULL);
}

static void tb_set_max_length(int no, bool multi, int len)
{
	struct textbox_state *t = textbox_get(no, multi, true);
	if (t)
		t->max_length = len;
}

static int tb_get_max_length(int no, bool multi)
{
	struct textbox_state *t = textbox_get(no, multi, false);
	return t ? t->max_length : 0;
}

static void tb_set_select_color(int no, bool multi, int r, int g, int b)
{
	struct textbox_state *t = textbox_get(no, multi, true);
	if (t) { t->sel_r = r; t->sel_g = g; t->sel_b = b; }
}

static void tb_set_cg_name(int no, bool multi, struct string *name)
{
	struct textbox_state *t = textbox_get(no, multi, true);
	if (!t)
		return;
	textbox_set_string(&t->cg_name, name);
	textbox_render(t);
}

static void tb_get_cg_name(int no, bool multi, struct string **out)
{
	struct textbox_state *t = textbox_get(no, multi, false);
	textbox_out_string(out, t ? t->cg_name : NULL);
}

// --- TextBox ---
static void PE_SetTextBoxSize(int no, int w, int h) { tb_set_size(no, false, w, h); }
static void PE_SetTextBoxFontProperty(int no, int type, int size, int r, int g, int b,
	float bold, int er, int eg, int eb, float ew)
	{ tb_set_font_property(no, false, type, size, r, g, b, bold, er, eg, eb, ew); }
static void PE_GetTextBoxFontProperty(int no, int *type, int *size, int *r, int *g, int *b,
	float *bold, int *er, int *eg, int *eb, float *ew)
	{ tb_get_font_property(no, false, type, size, r, g, b, bold, er, eg, eb, ew); }
static void PE_SetTextBoxText(int no, struct string *s) { tb_set_text(no, false, s); }
static void PE_GetTextBoxText(int no, struct string **s) { tb_get_text(no, false, s); }

/*
 * Одноаргументная форма — `string GetTextBoxText(int)` (Haha Ranman; у
 * Tsumamigui 3 та же функция объявлена out-параметром `(int, ref string)`).
 * Формы различаются ТОЛЬКО арностью, линковка по имени вела 1-арговую в
 * out-параметровую: `s` приходил мусорным регистром, а в возврат уходил void —
 * так комментарий сейва «вводился, но не сохранялся» (Ｐ＿テキストボックス＿
 * テキスト取得 отдавал пустоту/мусор).
 */
static struct string *PE_GetTextBoxText1(int no)
{
	struct string *s = NULL;
	tb_get_text(no, false, &s);
	return s;
}

static struct string *PE_GetMultiTextBoxText1(int no)
{
	struct string *s = NULL;
	tb_get_text(no, true, &s);
	return s;
}
static void PE_SetTextBoxMaxTextLength(int no, int len) { tb_set_max_length(no, false, len); }
static int PE_GetTextBoxMaxTextLength(int no) { return tb_get_max_length(no, false); }
static void PE_SetTextBoxSelectColor(int no, int r, int g, int b) { tb_set_select_color(no, false, r, g, b); }
static int PE_GetTextBoxSelectR(int no) { struct textbox_state *t = textbox_get(no, false, false); return t ? t->sel_r : 0; }
static int PE_GetTextBoxSelectG(int no) { struct textbox_state *t = textbox_get(no, false, false); return t ? t->sel_g : 0; }
static int PE_GetTextBoxSelectB(int no) { struct textbox_state *t = textbox_get(no, false, false); return t ? t->sel_b : 128; }
static void PE_SetTextBoxCGName(int no, struct string *s) { tb_set_cg_name(no, false, s); }
static void PE_GetTextBoxCGName(int no, struct string **s) { tb_get_cg_name(no, false, s); }
static void PE_SetTextBoxReadOnly(int no, bool flg)
	{ struct textbox_state *t = textbox_get(no, false, true); if (t) t->read_only = flg; }
static bool PE_IsTextBoxReadOnly(int no)
	{ struct textbox_state *t = textbox_get(no, false, false); return t && t->read_only; }
static void PE_SetTextBoxCharSpace(int no, int space)
	{ struct textbox_state *t = textbox_get(no, false, true); if (t) t->char_space = space; }
static int PE_GetTextBoxCharSpace(int no)
	{ struct textbox_state *t = textbox_get(no, false, false); return t ? t->char_space : 0; }
// Выделение и IME: выделять нечего (каретки нет), переключать тоже нечего.
static void PE_SelectTextBoxAll(int no) { (void)no; }
static void PE_OpenTextBoxIME(int no) { (void)no; }
static void PE_CloseTextBoxIME(int no) { (void)no; }
static bool PE_IsOpenTextBoxIME(int no) { (void)no; return false; }

// --- MultiTextBox ---
static void PE_SetMultiTextBoxSize(int no, int w, int h) { tb_set_size(no, true, w, h); }
static void PE_SetMultiTextBoxFontProperty(int no, int type, int size, int r, int g, int b,
	float bold, int er, int eg, int eb, float ew)
	{ tb_set_font_property(no, true, type, size, r, g, b, bold, er, eg, eb, ew); }
static void PE_GetMultiTextBoxFontProperty(int no, int *type, int *size, int *r, int *g, int *b,
	float *bold, int *er, int *eg, int *eb, float *ew)
	{ tb_get_font_property(no, true, type, size, r, g, b, bold, er, eg, eb, ew); }
static void PE_SetMultiTextBoxText(int no, struct string *s) { tb_set_text(no, true, s); }
static void PE_GetMultiTextBoxText(int no, struct string **s) { tb_get_text(no, true, s); }
static void PE_SetMultiTextBoxMaxTextLength(int no, int len) { tb_set_max_length(no, true, len); }
static int PE_GetMultiTextBoxMaxTextLength(int no) { return tb_get_max_length(no, true); }
static void PE_SetMultiTextBoxSelectColor(int no, int r, int g, int b) { tb_set_select_color(no, true, r, g, b); }
static int PE_GetMultiTextBoxSelectR(int no) { struct textbox_state *t = textbox_get(no, true, false); return t ? t->sel_r : 0; }
static int PE_GetMultiTextBoxSelectG(int no) { struct textbox_state *t = textbox_get(no, true, false); return t ? t->sel_g : 0; }
static int PE_GetMultiTextBoxSelectB(int no) { struct textbox_state *t = textbox_get(no, true, false); return t ? t->sel_b : 128; }
static void PE_SetMultiTextBoxCGName(int no, struct string *s) { tb_set_cg_name(no, true, s); }
static void PE_GetMultiTextBoxCGName(int no, struct string **s) { tb_get_cg_name(no, true, s); }
static void PE_SetMultiTextBoxReadOnly(int no, bool flg)
	{ struct textbox_state *t = textbox_get(no, true, true); if (t) t->read_only = flg; }
static bool PE_IsMultiTextBoxReadOnly(int no)
	{ struct textbox_state *t = textbox_get(no, true, false); return t && t->read_only; }

static void PartsEngine_Update(int passed_time, bool is_skip, bool message_window_show)
{
	textbox_update_caret(passed_time);
	PE_Update(passed_time, message_window_show);
}

static void PartsEngine_Update_Pascha3PC(struct string *xxx1, struct string *xxx2, int passed_time, bool is_skip, bool message_window_show)
{
	PE_Update(passed_time, message_window_show);
}

// Oyako Rankan
static bool PartsEngine_AddDrawCutCGToPartsConstructionProcess_old(int parts_no,
		struct string *cg_name, int dx, int dy, int sx, int sy, int w, int h,
		int state)
{
	return PE_AddDrawCutCGToPartsConstructionProcess(parts_no, cg_name, dx, dy, w, h,
			sx, sy, w, h, 0, state);
}

// Oyako Rankan
static bool PartsEngine_AddCopyCutCGToPartsConstructionProcess_old(int parts_no,
		struct string *cg_name, int dx, int dy, int sx, int sy, int w, int h,
		int state)
{
	return PE_AddCopyCutCGToPartsConstructionProcess(parts_no, cg_name, dx, dy, w, h,
			sx, sy, w, h, 0, state);
}

// Rance 9: argument types changed from int to float
static void PartsEngine_SetComponentPos(int parts_no, float x, float y)
{
	PE_SetPos(parts_no, x, y);
}

// Rance 9: return type changed from int to float
static float PartsEngine_Parts_GetPartsUpperLeftPosX(int parts_no, int state)
{
	return PE_GetPartsUpperLeftPosX(parts_no, state);
}

static float PartsEngine_Parts_GetPartsUpperLeftPosY(int parts_no, int state)
{
	return PE_GetPartsUpperLeftPosY(parts_no, state);
}

/*
 * Двухвыходная форма: `Parts_GetPartsUpperLeftPos(int, wrap<float> PosX,
 * wrap<float> PosY, int State)`. Обе координаты те же, что у отдельных
 * `…PosX`/`…PosY` (те реализованы давно) — не хватало ровно этой обёртки, и
 * `CustomerViewSet@MoveOut` (уход клиента в фазе Hustling у Dohna) падал на
 * «Unimplemented HLL function» через несколько секунд после входа в фазу.
 * `wrap<скаляр>` приходит ссылкой на переменную (две ячейки стека), ffi отдаёт
 * её обычным указателем — как у `TextSurfaceManager.GetFontWidth`.
 */
static void PartsEngine_Parts_GetPartsUpperLeftPos(int parts_no, float *pos_x,
		float *pos_y, int state)
{
	if (pos_x)
		*pos_x = PE_GetPartsUpperLeftPosX(parts_no, state);
	if (pos_y)
		*pos_y = PE_GetPartsUpperLeftPosY(parts_no, state);
}

// Rance 9: return type changed from int to float
static float PartsEngine_Parts_GetComponentPosX(int parts_no)
{
	return PE_GetPartsX(parts_no);
}

static float PartsEngine_GetComponentPosY(int parts_no)
{
	return PE_GetPartsY(parts_no);
}

// Rance 9: X/Y coordinate types changed from int to float
static void PartsEngine_AddComponentMotionPos(int parts_no, float begin_x, float begin_y,
		float end_x, float end_y, int begin_t, int end_t,
		struct string *curve_name)
{
	PE_AddMotionPos_curve(parts_no, begin_x, begin_y, end_x, end_y,
			begin_t, end_t, curve_name);
}

/*
 * Одна операция процедуры построения (構築パーツ). Раньше это был инлайн-switch
 * внутри HLL-обёртки; вынесен отдельно, потому что ТЕ ЖЕ операции приходят из
 * ДВУХ мест: игра добавляет их в рантайме через AddPartsConstructionProcess, а
 * раскладка активности описывает их узлом `手順リスト` (см. act_construction_run).
 * Поля раскладки ложатся на параметры один в один: `コマンド`, `元矩形`,
 * `先矩形`, `色１`, `文字間隔`/`行間隔`, `フォント*`, `テキスト`, `ＣＧ名`.
 */
static void construction_op(int parts_no, int state, int command, int interp_type,
		int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh,
		int r, int g, int b, int a, int r2, int g2, int b2,
		int char_space, int line_space, int font_type, int font_size,
		int font_r, int font_g, int font_b, int edge_r, int edge_g, int edge_b,
		int full_size, float bold_weight, float edge_weight,
		struct string *text, struct string *cg_name,
		int radius_x, int radius_y, int start_angle, int sweep_angle, int blur)
{
	(void)interp_type; (void)sw; (void)sh;
	switch (command) {
	case 0:  // CASConstructionProcess::SetCreate
		PE_AddCreateToPartsConstructionProcess(parts_no, dw, dh, state);
		break;
	case 1:  // CASConstructionProcess::SetCreatePixelOnly
		PE_AddCreatePixelOnlyToPartsConstructionProcess(parts_no, dw, dh, state);
		break;
	case 2:  // CASConstructionProcess::SetCreateCG
		PE_AddCreateCGToProcess(parts_no, cg_name, state);
		break;
	case 3:  // CASConstructionProcess::SetFill
		PE_AddFillToPartsConstructionProcess(parts_no,
				dx, dy, dw, dh, r, g, b, state);
		break;
	case 4:  // CASConstructionProcess::SetFillAlphaColor
		PE_AddFillAlphaColorToPartsConstructionProcess(parts_no,
				dx, dy, dw, dh, r, g, b, a, state);
		break;
	case 5:  // CASConstructionProcess::SetFillAMap
		PE_AddFillAMapToPartsConstructionProcess(parts_no,
				dx, dy, dw, dh, a, state);
		break;
	case 6:  // CASConstructionProcess::SetFillWithAlpha
		PE_AddFillWithAlphaToPartsConstructionProcess(parts_no,
				dx, dy, dw, dh, r, g, b, a, state);
		break;
	case 7:  // CASConstructionProcess::SetDrawText
		PE_AddDrawTextToPartsConstructionProcess(parts_no,
				dx, dy, text, font_type, font_size,
				font_r, font_g, font_b, bold_weight,
				edge_r, edge_g, edge_b, edge_weight,
				char_space, line_space, state);
		break;
	case 8:  // CASConstructionProcess::SetCopyText
		PE_AddCopyTextToPartsConstructionProcess(parts_no,
				dx, dy, text, font_type, font_size,
				font_r, font_g, font_b, bold_weight,
				edge_r, edge_g, edge_b, edge_weight,
				char_space, line_space, state);
		break;
	case 9:  // CASConstructionProcess::SetFillGradationHorizon
		WARNING("AddConstructProcess: FillGradationHorizon unimplemented");
		break;
	case 10:  // CASConstructionProcess::SetDrawRect
		PE_AddDrawRectToPartsConstructionProcess(parts_no,
				dx, dy, dw, dh, r, g, b, state);
		break;
	case 11:  // CASConstructionProcess::SetCutCGBlend (equal scale)
		PE_AddDrawCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, dw, dh,
				interp_type, state);
		break;
	case 12:  // CASConstructionProcess::SetCutCGCopy (equal scale)
		PE_AddCopyCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, dw, dh,
				interp_type, state);
		break;
	case 13:  // CASConstructionProcess::SetCutCGScaleBlend
		PE_AddDrawCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, sw, sh,
				interp_type, state);
		break;
	case 14:  // CASConstructionProcess::SetCutCGScaleCopy
		PE_AddCopyCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, sw, sh,
				interp_type, state);
		break;
	case 15:  // CASConstructionProcess::SetGrayFilter
		PE_AddGrayFilterToPartsConstructionProcess(parts_no,
				dx, dy, dw, dh, full_size, state);
		break;
	case 16:  // CASConstructionProcess::SetAddFilter
		WARNING("AddConstructProcess: AddFilter unimplemented");
		break;
	case 17:  // CASConstructionProcess::SetMulFilter
		WARNING("AddConstructProcess: MulFilter unimplemented");
		break;
	case 18:  // CASConstructionProcess::SetDrawLine
		WARNING("AddConstructProcess: DrawLine unimplemented");
		break;
	case 19:  // CASConstructionProcess::SetCutCGAlphaBlend (equal scale)
		PE_AddDrawCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, dw, dh,
				interp_type, state);
		break;
	case 20:  // CASConstructionProcess::SetCutCGScaleAlphaBlend
		PE_AddDrawCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, sw, sh,
				interp_type, state);
		break;
	case 21:  // CASConstructionProcess::SetCutCGOnlyAlpha (equal scale)
		PE_AddCopyCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, dw, dh,
				interp_type, state);
		break;
	case 22:  // CASConstructionProcess::SetCutCGScaleOnlyAlpha
		PE_AddCopyCutCGToPartsConstructionProcess(parts_no, cg_name,
				dx, dy, dw, dh, sx, sy, sw, sh,
				interp_type, state);
		break;
	case 23:  // CASConstructionProcess::SetAlphaBlendText
		// Text drawn onto a (usually FillAMap'd, alpha=0) transparent surface — the
		// backlog builds each line this way. Must WRITE the glyph alpha into the
		// destination, so use the copy-text path (RENDER_COPY: RGB=color, alpha=MAX).
		// The draw-text path (RENDER_BLENDED) leaves dst alpha untouched → invisible.
		PE_AddCopyTextToPartsConstructionProcess(parts_no,
				dx, dy, text, font_type, font_size,
				font_r, font_g, font_b, bold_weight,
				edge_r, edge_g, edge_b, edge_weight,
				char_space, line_space, state);
		break;
	case 24:  // CASConstructionProcess::SetOnlyAlphaText
		PE_AddCopyTextToPartsConstructionProcess(parts_no,
				dx, dy, text, font_type, font_size,
				font_r, font_g, font_b, bold_weight,
				edge_r, edge_g, edge_b, edge_weight,
				char_space, line_space, state);
		break;
	case 27:  // CASConstructionProcess::SetHBlurFilter (v14)
	case 28:  // CASConstructionProcess::SetVBlurFilter (v14)
		// Размытый задник экранов Dohna: CreateCG → HBlur → VBlur, сила в `ブラー`.
		PE_AddBlurFilterToPartsConstructionProcess(parts_no, dx, dy, dw, dh,
				full_size, blur, command == 28, state);
		break;
	case 88:  // CASConstructionProcess::SetFillRect (v14) — заливка ЦВЕТОМ
		PE_AddFillToPartsConstructionProcess(parts_no, dx, dy, dw, dh, r, g, b, state);
		break;
	case 90:  // CASConstructionProcess::SetFillRectAMap (v14) — заливка АЛЬФЫ
		PE_AddFillAMapToPartsConstructionProcess(parts_no, dx, dy, dw, dh, a, state);
		break;
	case 102:  // CASConstructionProcess::SetFillCircleAMap (v14)
		// Круг = сектор на 360°: центр в `先矩形`, радиусы в `半径`, альфа из `色１`.
		// Так собраны точки-индикаторы страниц и круглые подложки иконок.
		PE_AddFillPieAMapToPartsConstructionProcess(parts_no, dx, dy,
				radius_x, radius_y, 0, 360, a, state);
		break;
	case 122:  // CASConstructionProcess::SetFillPieAMap (v14)
		// Сектор в альфа-карту: из четырёх таких углов и двух прямоугольников
		// собрана каждая скруглённая подложка интерфейса Dohna.
		PE_AddFillPieAMapToPartsConstructionProcess(parts_no, dx, dy,
				radius_x, radius_y, start_angle, sweep_angle, a, state);
		break;
	default:
		WARNING("AddConstructProcess: unknown command %d", command);
		break;
	}
}


static void PartsEngine_add_construction_process(union vm_value *ints,
		union vm_value *floats, union vm_value *strings, int nr_ints)
{
	int parts_no    = ints[0].i;
	int state       = ints[1].i;
	int command     = ints[2].i;
	int interp_type = ints[3].i;
	int sx          = ints[4].i;
	int sy          = ints[5].i;
	int sw          = ints[6].i;
	int sh          = ints[7].i;
	int dx          = ints[8].i;
	int dy          = ints[9].i;
	int dw          = ints[12].i;
	int dh          = ints[13].i;
	int r           = ints[14].i;
	int g           = ints[15].i;
	int b           = ints[16].i;
	int a           = ints[17].i;
	int r2          = ints[18].i;
	int g2          = ints[19].i;
	int b2          = ints[20].i;
	int char_space  = ints[21].i;
	int line_space  = ints[22].i;
	int font_type   = ints[23].i;
	int font_size   = ints[24].i;
	int font_r      = ints[25].i;
	int font_g      = ints[26].i;
	int font_b      = ints[27].i;
	int edge_r      = ints[28].i;
	int edge_g      = ints[29].i;
	int edge_b      = ints[30].i;
	int full_size   = ints[31].i;
	float bold_weight = floats[0].f;
	float edge_weight = floats[1].f;
	struct string *text    = heap_get_string(strings[0].i);
	struct string *cg_name = heap_get_string(strings[1].i);
	// Поля векторных фигур v14 живут за 32-м слотом классической раскладки —
	// сюда они приходят только из ix-варианта (см. PE_AddPartsConstructionProcess_ix).
	int radius_x = nr_ints > 34 ? ints[34].i : 0;
	int radius_y = nr_ints > 35 ? ints[35].i : 0;
	int start_angle = nr_ints > 36 ? ints[36].i : 0;
	int sweep_angle = nr_ints > 37 ? ints[37].i : 0;
	int blur = nr_ints > 38 ? ints[38].i : 0;  // поле `ブラー` (команды 27/28)

	if (getenv("XSYS4_BL_TRACE") && (command == 7 || command == 8 || command == 23 || command == 24))
		NOTICE("TEXTOP cmd=%d part=%d dx=%d dy=%d ftype=%d fsize=%d col=%d,%d,%d edge=%d,%d,%d ew=%.2f bw=%.2f str0slot=%d str0len=%d text='%s'",
		       command, parts_no, dx, dy, font_type, font_size, font_r, font_g, font_b,
		       edge_r, edge_g, edge_b, edge_weight, bold_weight,
		       strings[0].i, text ? (int)text->size : -1, text ? display_sjis0(text->text) : "(null)");

	construction_op(parts_no, state, command, interp_type, sx, sy, sw, sh,
			dx, dy, dw, dh, r, g, b, a, r2, g2, b2, char_space, line_space,
			font_type, font_size, font_r, font_g, font_b,
			edge_r, edge_g, edge_b, full_size, bold_weight, edge_weight,
			text, cg_name, radius_x, radius_y, start_angle, sweep_angle, blur);
}

// Generic dispatch function for PartsEngine operations.
// func_id selects the operation; arguments and return values are passed
// through three typed arrays (int/bool, float, string).
//
// Calling convention (from game script):
//   1. Push input values into the appropriate arrays.
//   2. Push a placeholder (0 or "") for each output slot.
//   3. Call PartsFunc; outputs are written back into the placeholder slots.
static int PartsEngine_PartsFunc(int func_id, struct page **array_int,
		struct page **array_float, struct page **array_string)
{
	int nr_ints = (array_int && *array_int) ? (*array_int)->nr_vars : 0;
	union vm_value *ints = nr_ints ? (*array_int)->values : NULL;
	int nr_floats = (array_float && *array_float) ? (*array_float)->nr_vars : 0;
	union vm_value *floats = nr_floats ? (*array_float)->values : NULL;
	int nr_strings = (array_string && *array_string) ? (*array_string)->nr_vars : 0;
	union vm_value *strings = nr_strings ? (*array_string)->values : NULL;

#define REQUIRE_INTS(n) \
	if (nr_ints != (n)) VM_ERROR("Invalid arguments for PartsFunc %d: expected %d ints, got %d", func_id, (n), nr_ints)
#define REQUIRE_FLOATS(n) \
	if (nr_floats < (n)) VM_ERROR("Invalid arguments for PartsFunc %d: expected %d floats, got %d", func_id, (n), nr_floats)
#define REQUIRE_STRINGS(n) \
	if (nr_strings < (n)) VM_ERROR("Invalid arguments for PartsFunc %d: expected %d strings, got %d", func_id, (n), nr_strings)

	switch (func_id) {
	case 0:  // void SetActiveLayer(int layer)
		REQUIRE_INTS(1);
		PE_set_active_controller(ints[0].i);
		return 1;
	case 1:  // int GetActiveLayer()
		REQUIRE_INTS(1);
		ints[0].i = PE_get_active_controller();
		return 1;
	case 2:  // int GetSystemOverlayLayer()
		REQUIRE_INTS(1);
		ints[0].i = PE_get_system_controller();
		return 1;
	case 3:  // void PauseMotion(bool pause)
		REQUIRE_INTS(1);
		PE_PauseMotion(!!ints[0].i);
		return 1;
	case 4:  // void SetWantSave(int parts_no, bool want_save)
		REQUIRE_INTS(2);
		PE_parts_set_want_save(ints[0].i, !!ints[1].i);
		return 1;
	case 6:  // bool SaveThumbnail(string filename, int thumbnail_width)
		REQUIRE_INTS(2);
		REQUIRE_STRINGS(1);
		ints[1].i = PE_save_thumbnail(heap_get_string(strings[0].i), ints[0].i);
		return 1;
	case 40:  // float PARTS_GetAbsoluteX(int number)
		REQUIRE_INTS(1);
		REQUIRE_FLOATS(1);
		floats[0].f = PE_parts_get_absolute_x(ints[0].i);
		return 1;
	case 41:  // float PARTS_GetAbsoluteY(int number)
		REQUIRE_INTS(1);
		REQUIRE_FLOATS(1);
		floats[0].f = PE_parts_get_absolute_y(ints[0].i);
		return 1;
	case 42:  // int PARTS_GetAbsoluteZ(int number)
		REQUIRE_INTS(2);
		ints[1].i = PE_parts_get_absolute_z(ints[0].i);
		return 1;
	case 45:  // void PARTS_SetLockInputState(int number, bool lock)
		REQUIRE_INTS(2);
		PE_parts_set_lock_input_state(ints[0].i, !!ints[1].i);
		return 1;
	case 57:  // void AppendChild(int number, int child_number)
		REQUIRE_INTS(2);
		PE_SetParentPartsNumber(ints[1].i, ints[0].i);
		return 1;
	case 91:  // void SetLayoutBoxPadding(int parts_no, int top, int bottom, int left, int right)
		REQUIRE_INTS(5);
		PE_set_layoutbox_padding(ints[0].i, ints[1].i, ints[2].i, ints[3].i, ints[4].i);
		return 1;
	case 92:  // int GetLayoutBoxPaddingTop(int parts_no)
		REQUIRE_INTS(2);
		ints[1].i = PE_get_layoutbox_padding_top(ints[0].i);
		return 1;
	case 93:  // int GetLayoutBoxPaddingBottom(int parts_no)
		REQUIRE_INTS(2);
		ints[1].i = PE_get_layoutbox_padding_bottom(ints[0].i);
		return 1;
	case 94:  // int GetLayoutBoxPaddingLeft(int parts_no)
		REQUIRE_INTS(2);
		ints[1].i = PE_get_layoutbox_padding_left(ints[0].i);
		return 1;
	case 95:  // int GetLayoutBoxPaddingRight(int parts_no)
		REQUIRE_INTS(2);
		ints[1].i = PE_get_layoutbox_padding_right(ints[0].i);
		return 1;
	case 103:  // void GetPartsCGSurfaceArea(int parts_no, int *x, int *y, int *w, int *h, int state)
		REQUIRE_INTS(6);
		PE_GetPartsCGSurfaceArea(ints[0].i, &ints[1].i, &ints[2].i, &ints[3].i, &ints[4].i, ints[5].i);
		return 1;
	case 159:  // AddConstructProcess(ArrayInt[32], ArrayFloat[2], ArrayString[2])
		REQUIRE_INTS(32);
		REQUIRE_FLOATS(2);
		REQUIRE_STRINGS(2);
		PartsEngine_add_construction_process(ints, floats, strings, 32);
		return 1;
	case 162:  // bool InitPartsMovie(int parts_no, int width, int height, int bg_r, int bg_g, int bg_b, int state)
		REQUIRE_INTS(8);
		ints[7].i = PE_init_parts_movie(ints[0].i, ints[1].i, ints[2].i, ints[3].i, ints[4].i, ints[5].i, ints[6].i);
		return 1;
	case 163:  // int GetMovieSprite(int parts_no, int state)
		REQUIRE_INTS(3);
		ints[2].i = PE_get_movie_sprite(ints[0].i, ints[1].i);
		return 1;
	default:
		WARNING("Unknown func_id: %d", func_id);
		return 0;
	}
#undef REQUIRE_INTS
}

HLL_WARN_UNIMPLEMENTED(, void, PartsEngine, StopSoundWithoutSystemSound);
// Per-part on-cursor/click sound-effect names. Audio side-effects are out of
// scope (sound is handled elsewhere / muted); accept and ignore so button setup
// (e.g. C_TITLE@EnableButton -> Ｐ＿オンカーソル効果音設定) proceeds.
static void PE_Parts_SetSoundName(int parts_no, struct string *name, int state) { (void)parts_no; (void)name; (void)state; }
static void PE_Parts_GetSoundName(int parts_no, struct string **out, int state) { (void)parts_no; (void)state; if (out) { if (*out) free_string(*out); *out = string_ref(&EMPTY_STRING); } }
// Опт-аут парта из снимка «画面保管»/BACK SCENE. Игра зовёт это ТОЛЬКО с enable=0 и только
// для служебных оверлеев, которые рисует поверх сцены: отладочные FPS/память, системные
// кнопки, mode-CG, 3D-слой (пять мест в байткоде, все `PUSH 0`) ⇒ дефолт «сохранять».
// ВАЖНО: флаг СВОЙ, а не upstream-овский want_save, по которому фильтруется ИГРОВОЙ сейв.
// Исключив оверлеи и оттуда, мы потеряли бы их после resume-загрузки: конструкторы вьюх
// (CSystemButtonView и пр.) повторно не выполняются, пересоздавать парты некому.
// «Компонент» адресует и парт, и СЛОЙ (контроллер) — см. parts_controller_is_layer.
// Игра гасит слоями весь экран игры на время просмотра бэк-сцены
// (`CBackSceneView@HideAllFrontScene`: для каждого GetControllerID(i) с
// IsComponentShow == true зовётся SetComponentShow(id, false)), а ShowAllFrontScene
// возвращает ровно эти слои.
static void PE_SetComponentShow(int no, bool show)
{
	if (parts_controller_is_layer(no))
		parts_controller_set_show(no, show);
	else
		PE_SetShow(no, show);
}

static bool PE_IsComponentShow(int no)
{
	if (parts_controller_is_layer(no))
		return parts_controller_get_show(no);
	return PE_GetPartsShow(no);
}

static void PE_SetWantSaveBackScene(int parts_no, int enable)
{
	if (getenv("XSYS4_BS_TRACE"))
		NOTICE("BACKSCENE want-save part=%d enable=%d", parts_no, enable);
	PE_parts_set_want_save_back_scene(parts_no, !!enable);
}
static bool PE_IsWantSaveBackScene(int parts_no)
{
	return PE_parts_get_want_save_back_scene(parts_no);
}
// Остальное семейство *ForBackScene правит ВОССТАНОВЛЕННЫЙ снимок: CBackSceneView гасит
// экран, ставит цвет шрифта окна сообщений и скрывает лишние парты. У AliceSoft снимок,
// судя по имени, живёт отдельной библиотекой партов; у нас LoadBackScene восстанавливает
// его в живые парты (см. parts_engine_load), поэтому это ровно обычные сеттеры.
// Эти сеттеры адресуют КОПИЮ парта в пространстве бэк-сцены — ту, что создала LoadBackScene.
static void PE_SetAlphaForBackScene(int parts_no, int alpha)
{
	PE_SetAlpha(parts_no + BACK_SCENE_PARTS_OFFSET, alpha);
}
static void PE_SetShowForBackScene(int parts_no, bool show)
{
	PE_SetShow(parts_no + BACK_SCENE_PARTS_OFFSET, show);
}
static void PE_SetMulColorForBackScene(int parts_no, int r, int g, int b)
{
	PE_SetMultiplyColor(parts_no + BACK_SCENE_PARTS_OFFSET, r, g, b);
}
static void PE_SetFontColorForBackScene(int parts_no, int r, int g, int b, int state)
{
	PE_SetPartsFontColor(parts_no + BACK_SCENE_PARTS_OFFSET, r, g, b, state);
}
// PE_ClearBackScene — в src/parts/save.c: он сносит пространство номеров бэк-сцены.
// Direct SaveThumbnail(filename, width) export. Newer games (Tsumamigui 3) call this via
// AutoSave when opening the town map; without it the unimplemented-HLL error dropped into
// the debugger REPL and the map never became interactive. Второй аргумент — ШИРИНА превью
// (в .ain: ThumbnailWidth, игра передаёт 200); см. PE_save_thumbnail.
static bool PE_SaveThumbnail(struct string *filename, int thumbnail_width)
{
	return PE_save_thumbnail(filename, thumbnail_width);
}
// Батч-форма построения parts (3 массива-параметра). Единый интерфейс, через
// который игра (parts::AddConstructProcess в .ain) добавляет ВСЕ операции
// построения parts-текстур: SetCreate/SetFill/SetDrawText/SetCutCG… Раскодируем
// массивы тем же путём, что и PartsFunc(159), и передаём в дешифратор.
// Раньше был no-op — из-за этого construction-parts (центр диалога 確認枠,
// прокручиваемый текст бэклога Tsumamigui 3) строились в 0×0/пустыми.
static void PE_AddPartsConstructionProcess(struct page **ai, struct page **af, struct page **as) {
	int nr_ints    = (ai && *ai) ? (*ai)->nr_vars : 0;
	int nr_floats  = (af && *af) ? (*af)->nr_vars : 0;
	int nr_strings = (as && *as) ? (*as)->nr_vars : 0;
	// Ожидаемый формат: 32 int, 2 float, 2 string (см. parts::AddConstructProcess).
	// Не падаем на неожиданном формате — просто пропускаем операцию.
	if (nr_ints < 32 || nr_floats < 2 || nr_strings < 2) {
		if (getenv("XSYS4_CP_TRACE"))
			NOTICE("AddPartsConstructionProcess: unexpected arg sizes i=%d f=%d s=%d",
			       nr_ints, nr_floats, nr_strings);
		return;
	}
	union vm_value *ints    = (*ai)->values;
	union vm_value *floats  = (*af)->values;
	union vm_value *strings = (*as)->values;
	if (getenv("XSYS4_CP_TRACE"))
		NOTICE("AddPartsConstructionProcess part=%d state=%d cmd=%d dst=(%d,%d %dx%d) rgba=%d,%d,%d,%d",
		       ints[0].i, ints[1].i, ints[2].i, ints[8].i, ints[9].i,
		       ints[12].i, ints[13].i, ints[14].i, ints[15].i, ints[16].i, ints[17].i);
	// Сырьё — для чтения процедуры обратно (см. parts_cp_save_raw). У классической
	// формы номер части и состояние лежат в ints[0..1], точек нет.
	PE_SaveConstructionRaw(ints[0].i, ints[1].i, ints, nr_ints, floats, nr_floats,
			       strings, nr_strings, NULL, 0);
	PartsEngine_add_construction_process(ints, floats, strings, nr_ints);
}

// Ixseal (System 4 v14) form of the batch construction interface. The classic
// call packs everything into three arrays and carries the part number and state
// in ints[0..1]; v14 passes both explicitly, adds a fourth array (ArrayPos: the
// point list of the new vector-shape commands) and grew ArrayInt from 32 to 40
// entries. Layout read from CASConstructionProcess::ArrayIntIndex::ToString:
//
//   0 Command, 1 InterpolationType, 2-5 Src{X,Y,Width,Height},
//   6-9 Dest{X,Y,X2,Y2}, 10-11 Dest{Width,Height}, 12-15 RGBA, 16-19 RGBA2,
//   20 CharSpace, 21 LineSpace, 22 FontType, 23 FontSize, 24-26 FontColorRGB,
//   27-29 EdgeColorRGB, 30 FullSize, 31 Blur, 32-33 Radius{X,Y}, 34 LineWidth,
//   35 RoundEdge, 36 RoundCorner, 37 Angle, 38 StartAngle, 39 SweepAngle
//
// The classic layout is the same sequence shifted by two, without A2 (new in
// v14) and without the nine trailing shape fields. ArrayFloat (BoldWeight,
// EdgeWeight) and ArrayString (Text, CGName) are unchanged.
//
// The command ids themselves did NOT change: 0..24 mean the same as in v7
// (verified against the Command constant each CASConstructionProcess@Set*
// method writes). v14 appended new ones (25.. : alpha-map gradations, blur and
// inverse filters, whole-CG blend and the line/polygon/circle/ellipse/arc/pie
// families), which the classic dispatcher below has no handler for.
#define IX_NR_CONSTRUCTION_INTS 40
#define NR_CLASSIC_CONSTRUCTION_COMMANDS 25

static void PE_AddPartsConstructionProcess_ix(int parts_no, struct page **ai, struct page **af,
		struct page **as, struct page **ap, int state)
{
	bool trace = !!getenv("XSYS4_CP_TRACE");
	int nr_ints    = (ai && *ai) ? (*ai)->nr_vars : 0;
	int nr_floats  = (af && *af) ? (*af)->nr_vars : 0;
	int nr_strings = (as && *as) ? (*as)->nr_vars : 0;
	if (nr_ints < IX_NR_CONSTRUCTION_INTS || nr_floats < 2 || nr_strings < 2) {
		if (trace)
			NOTICE("AddPartsConstructionProcess(ix): unexpected arg sizes i=%d f=%d s=%d",
			       nr_ints, nr_floats, nr_strings);
		return;
	}
	union vm_value *src = (*ai)->values;
	int command = src[0].i;
	// Сырьё запоминаем СРАЗУ и для любой команды — включая те, что мы ещё не умеем
	// строить: игра читает процедуру обратно (GetPartsConstructionProcess) и ждёт
	// ровно то, что подавала, а не только поддержанное нами.
	PE_SaveConstructionRaw(parts_no, state, src, nr_ints, (*af)->values, nr_floats,
			       (*as)->values, nr_strings,
			       (ap && *ap) ? (*ap)->values : NULL,
			       (ap && *ap) ? (*ap)->nr_vars : 0);
	// Расширения v14 за классическим набором, которые мы УМЕЕМ: размытие (27/28),
	// заливки прямоугольником (88/90) и круг в альфа-карту (102), сектор (122).
	// Остальные по-прежнему отбрасываем — иначе они молча портили бы поверхность.
	static const int v14_ok[] = { 27, 28, 88, 90, 102, 122 };
	bool known_v14 = false;
	for (size_t i = 0; i < sizeof(v14_ok) / sizeof(v14_ok[0]); i++)
		known_v14 |= (command == v14_ok[i]);
	if (command < 0 || (command >= NR_CLASSIC_CONSTRUCTION_COMMANDS && !known_v14)) {
		if (trace)
			NOTICE("AddPartsConstructionProcess(ix): v14-only command %d (part=%d)",
			       command, parts_no);
		return;
	}

	union vm_value ints[38] = {0};
	ints[0].i = parts_no;
	ints[1].i = state;
	ints[2].i = command;
	for (int i = 3; i <= 20; i++)   // InterpolationType .. B2
		ints[i] = src[i - 2];
	for (int i = 21; i <= 31; i++)  // CharSpace .. FullSize (A2 has no classic slot)
		ints[i] = src[i - 1];

	if (trace)
		NOTICE("AddPartsConstructionProcess(ix) part=%d state=%d cmd=%d src=(%d,%d %dx%d) dst=(%d,%d %dx%d) rgba=%d,%d,%d,%d",
		       parts_no, state, command,
		       ints[4].i, ints[5].i, ints[6].i, ints[7].i,
		       ints[8].i, ints[9].i, ints[12].i, ints[13].i,
		       ints[14].i, ints[15].i, ints[16].i, ints[17].i);
	// Слоты 34..37 — наши, для полей фигур v14 (радиусы и углы дуги): классическая
	// раскладка их не знает, а команде 122 они необходимы.
	ints[34] = src[32];  // RadiusX
	ints[35] = src[33];  // RadiusY
	ints[36] = src[38];  // StartAngle
	ints[37] = src[39];  // SweepAngle
	PartsEngine_add_construction_process(ints, (*af)->values, (*as)->values, 38);
}

// --- Подсистема Activity (именованные наборы parts) ---
// Новые игры System 4 группируют parts в именованные «активности». В движке её
// не было; минимальный реестр: имя -> список (имя_parts, номер_parts) + EX-текст.
// intent (遷移): a part with a transition method (遷移方法) closes/transitions the
// activity when clicked. destinations (遷移先) name target .pact pages (empty = just
// close). BindEndEvent registers MouseLClickEvent on parts where IsExistIntentData.
struct pe_act_part {
	struct string *name;
	int number;
	int intent_type;               // 遷移方法 (0 = no transition)
	struct string **intent_dests;  // 遷移先 (non-empty destination names)
	int nr_intent_dests;
};
struct pe_activity {
	struct pe_act_part *parts;
	int nr_parts;
	struct string *ex_text;
	int ex_id;
	/*
	 * `終了キー` — список виртуальных кодов клавиш, закрывающих активность
	 * (у `セーブ確認` это `{ 2, 27 }`: правая кнопка мыши и ESC). Лежит на ВЕРХНЕМ
	 * уровне файла активности, рядом с `アクティビティ`, и в коде игры не
	 * упоминается ни разу — его читает движок и отдаёт наружу этими четырьмя
	 * функциями. Игра сама вешает на каждую клавишу `CASClick`
	 * (`CASPartsActivity@BindEndType`) и по срабатыванию делает `activity::End`.
	 * Пока мы отдавали «клавиш нет», ESC и правая кнопка не закрывали ни один
	 * диалог.
	 */
	int *end_keys;
	int nr_end_keys;
	/*
	 * СЛУЖЕБНЫЕ ЧАСТИ, которые создаёт сам загрузчик раскладки: надписи кнопок и
	 * подписи чекбоксов, три части декора полосы прокрутки, текст и «мигалка»
	 * окна сообщений. Имени в раскладке у них нет, игра о них не знает и снять их
	 * не может — а `ReleaseActivity` шёл только по `parts`, и они оставались
	 * сиротами: родителя сняли, часть жива и рисуется. Так надписи прежней
	 * вкладки CONFIG наслаивались на новую (10 сирот за два переключения).
	 * Отдельным списком, а не записями в `parts`, чтобы не менять ни то, что
	 * игра видит через `NumofActivityParts`/`GetActivityParts`, ни формат сейва.
	 */
	int *helpers;
	int nr_helpers;
};
static struct hash_table *pe_activities;

static struct pe_activity *pe_act_find(struct string *name)
{
	if (!pe_activities)
		return NULL;
	return ht_get(pe_activities, name->text, NULL);
}

static void pe_act_add_helper(struct pe_activity *a, int parts_no)
{
	if (!a)
		return;
	a->helpers = xrealloc_array(a->helpers, a->nr_helpers, a->nr_helpers + 1,
			sizeof(*a->helpers));
	a->helpers[a->nr_helpers++] = parts_no;
}

static void PE_Activity_free(void *p)
{
	struct pe_activity *a = p;
	// ht_foreach_value обходит и пустые слоты (value == NULL).
	if (!a)
		return;
	for (int i = 0; i < a->nr_parts; i++) {
		free_string(a->parts[i].name);
		for (int j = 0; j < a->parts[i].nr_intent_dests; j++)
			free_string(a->parts[i].intent_dests[j]);
		free(a->parts[i].intent_dests);
	}
	free(a->parts);
	free(a->helpers);
	if (a->ex_text)
		free_string(a->ex_text);
	free(a->end_keys);
	free(a);
}

static bool PE_CreateActivity(struct string *name)
{
	if (!pe_activities)
		pe_activities = ht_create(256);
	if (ht_get(pe_activities, name->text, NULL))
		return true;
	struct pe_activity *a = xcalloc(1, sizeof(*a));
	a->ex_id = -1;
	ht_put(pe_activities, name->text, NULL)->value = a;
	if (getenv("XSYS4_ACT_TRACE"))
		NOTICE("ACT CreateActivity(name='%s')", display_sjis0(name->text));
	return true;
}

/*
 * --- Сериализация активностей в сейв-образ партов (XPE v5) ---
 *
 * Оригинальный PartsEngine.dll хранит реестр активностей в своём сейв-образе:
 * после загрузки сейва игра сразу спрашивает GetActivityPartsNumber
 * («ルートパーツ» и др.) — без реестра CActivityWrap ассертил (nonnull) m_root,
 * и загрузка «завершалась чёрным экраном».
 */
struct pe_act_save_ctx { struct iarray_writer *w; int n; };

static void pe_act_count_cb(struct ht_slot *slot, void *data)
{
	if (slot->value)
		(*(int*)data)++;
}

static void pe_act_write_cb(struct ht_slot *slot, void *data)
{
	struct iarray_writer *w = data;
	struct pe_activity *a = slot->value;
	if (!a)
		return;
	struct string *name = cstr_to_string(slot->key);
	iarray_write_string(w, name);
	free_string(name);
	iarray_write(w, a->nr_parts);
	for (int i = 0; i < a->nr_parts; i++) {
		struct pe_act_part *p = &a->parts[i];
		iarray_write_string(w, p->name);
		iarray_write(w, p->number);
		iarray_write(w, p->intent_type);
		iarray_write(w, p->nr_intent_dests);
		for (int j = 0; j < p->nr_intent_dests; j++)
			iarray_write_string(w, p->intent_dests[j]);
	}
	iarray_write_string_or_null(w, a->ex_text);
	iarray_write(w, a->ex_id);
	iarray_write(w, a->nr_end_keys);
	for (int i = 0; i < a->nr_end_keys; i++)
		iarray_write(w, a->end_keys[i]);
}

void pe_activities_save(struct iarray_writer *w)
{
	int n = 0;
	if (pe_activities)
		ht_foreach(pe_activities, pe_act_count_cb, &n);
	iarray_write(w, n);
	if (pe_activities)
		ht_foreach(pe_activities, pe_act_write_cb, w);
}

bool pe_activities_load(struct iarray_reader *r, bool apply)
{
	int n = iarray_read(r);
	if (n < 0 || n > 100000) {
		WARNING("сейв-образ партов: битое число активностей %d", n);
		return false;
	}
	// XSYS4_XPE_TRACE=1 — состав реестра активностей в образе. От него зависит,
	// сможет ли игра ПОСЛЕ загрузки снять наборы обработчиков (ReleaseActivity
	// отдаёт delegate-индексы по партам активности): пустой реестр = подписки
	// останутся жить и будут держать объекты сцены.
	if (getenv("XSYS4_XPE_TRACE"))
		NOTICE("XPE load: активностей в образе: %d (apply=%d)", n, apply);
	if (apply && pe_activities) {
		ht_foreach_value(pe_activities, PE_Activity_free);
		ht_free(pe_activities);
		pe_activities = NULL;
	}
	for (int i = 0; i < n; i++) {
		struct string *name = iarray_read_string(r);
		int nr_parts = iarray_read(r);
		if (nr_parts < 0 || nr_parts > 100000 || r->error) {
			WARNING("сейв-образ партов: битая активность");
			free_string(name);
			return false;
		}
		struct pe_activity *a = NULL;
		if (getenv("XSYS4_XPE_TRACE"))
			NOTICE("XPE load:   активность '%s': %d партов",
			       display_sjis0(name->text), nr_parts);
		if (apply) {
			PE_CreateActivity(name);
			a = pe_act_find(name);
			a->parts = xcalloc(nr_parts ? nr_parts : 1, sizeof(struct pe_act_part));
			a->nr_parts = nr_parts;
		}
		for (int j = 0; j < nr_parts; j++) {
			struct string *pn = iarray_read_string(r);
			int number = iarray_read(r);
			int intent_type = iarray_read(r);
			int nr_dests = iarray_read(r);
			if (nr_dests < 0 || nr_dests > 10000 || r->error) {
				free_string(pn);
				free_string(name);
				return false;
			}
			struct string **dests = NULL;
			if (apply && nr_dests)
				dests = xcalloc(nr_dests, sizeof(struct string*));
			for (int k = 0; k < nr_dests; k++) {
				struct string *d = iarray_read_string(r);
				if (dests)
					dests[k] = d;
				else
					free_string(d);
			}
			if (a) {
				a->parts[j].name = pn;
				a->parts[j].number = number;
				a->parts[j].intent_type = intent_type;
				a->parts[j].intent_dests = dests;
				a->parts[j].nr_intent_dests = nr_dests;
			} else {
				free_string(pn);
			}
		}
		struct string *ex_text = iarray_read_string_or_null(r);
		int ex_id = iarray_read(r);
		int nr_end_keys = iarray_read(r);
		if (nr_end_keys < 0 || nr_end_keys > 1000 || r->error) {
			free_string(name);
			if (ex_text) free_string(ex_text);
			return false;
		}
		int *keys = NULL;
		if (a && nr_end_keys)
			keys = xcalloc(nr_end_keys, sizeof(int));
		for (int k = 0; k < nr_end_keys; k++) {
			int v = iarray_read(r);
			if (keys)
				keys[k] = v;
		}
		if (a) {
			a->ex_text = ex_text;
			a->ex_id = ex_id;
			a->end_keys = keys;
			a->nr_end_keys = nr_end_keys;
		} else if (ex_text) {
			free_string(ex_text);
		}
		free_string(name);
	}
	return !r->error;
}

static bool PE_IsExistActivity(struct string *name)
{
	return pe_act_find(name) != NULL;
}

struct pe_sub_release {
	const char *prefix;
	size_t prefix_len;
	struct string **names;
	int nr_names;
};

static void pe_act_collect_sub_cb(struct ht_slot *slot, void *data)
{
	struct pe_sub_release *ctx = data;
	if (!slot->value || !slot->key)
		return;
	if (strncmp(slot->key, ctx->prefix, ctx->prefix_len))
		return;
	ctx->names = xrealloc_array(ctx->names, ctx->nr_names, ctx->nr_names + 1,
	                            sizeof(struct string *));
	ctx->names[ctx->nr_names++] = cstr_to_string(slot->key);
}

/*
 * Освободить активности, ВЛОЖЕННЫЕ в `name`, и дописать их delegate-индексы в
 * `indices`/`nr` (буфер вызывающего, ёмкость `cap`).
 *
 * Индексы обязаны попасть в тот же массив, что и у корневой активности: игра
 * передаёт его целиком в `CPartsMessageManager@ReleaseFunctionSetList`, и только
 * так снимаются наборы обработчиков. Пока индексы вложенных терялись, подписки
 * кнопок слотов оставались жить (по одной на слот, `env` = окружение
 * конструктора `SaveLoadScene@0`), окружение держало `ref SceneStack`, стек сцены
 * не умирал — и его слой некому было снести.
 */
static void pe_release_sub_activities(struct string *name, int *indices, int *nr, int cap)
{
	if (!pe_activities || !name)
		return;
	// Разделитель — ПОЛНОШИРИННЫЙ слэш «／» (в SJIS 0x815E), тот же, что в
	// именах узлов раскладки.
	char prefix[512];
	int len = snprintf(prefix, sizeof(prefix), "%s\x81\x5e", name->text);
	if (len <= 0 || (size_t)len >= sizeof(prefix))
		return;
	struct pe_sub_release ctx = { prefix, (size_t)len, NULL, 0 };
	ht_foreach(pe_activities, pe_act_collect_sub_cb, &ctx);
	for (int i = 0; i < ctx.nr_names; i++) {
		struct pe_activity *sub = pe_act_find(ctx.names[i]);
		if (sub) {
			for (int j = 0; j < sub->nr_parts; j++) {
				int di = PE_GetDelegateIndex(sub->parts[j].number);
				if (di >= 0 && indices && *nr < cap)
					indices[(*nr)++] = di;
				PE_ReleaseParts(sub->parts[j].number);
			}
			struct ht_slot *slot = ht_put(pe_activities, ctx.names[i]->text, NULL);
			if (slot->value == sub)
				slot->value = NULL;
			PE_Activity_free(sub);
		}
		free_string(ctx.names[i]);
	}
	free(ctx.names);
}

static bool PE_ReleaseActivity(struct string *name, struct page **out)
{
	struct pe_activity *a = pe_act_find(name);
	int n = a ? a->nr_parts : 0;
	if (getenv("XSYS4_BS_TRACE")) {
		NOTICE("BS ReleaseActivity('%s'): %d партов", display_sjis0(name->text), n);
		for (int i = 0; i < n; i++)
			NOTICE("BS   act part=%d '%s'", a->parts[i].number,
			       display_sjis1(a->parts[i].name->text));
	}
	// Массив — DELEGATE-ИНДЕКСЫ, не номера партов: игра передаёт его прямо в
	// `CPartsMessageManager@ReleaseFunctionSetList` (см. разбор в
	// PE_RemoveController). Парты без набора обработчиков в список не попадают.
	int nr = 0;
	// Ёмкость с запасом: в тот же массив идут индексы ВЛОЖЕННЫХ активностей
	// (у экрана сохранения их два десятка — слоты и кнопки страниц).
	int cap = n + 1024;
	int *indices = xcalloc(cap, sizeof(int));
	for (int i = 0; i < n; i++) {
		int di = PE_GetDelegateIndex(a->parts[i].number);
		if (di >= 0 && nr < cap)
			indices[nr++] = di;
	}
	if (a && !getenv("XSYS4_RA_KEEP_PARTS")) {
		/*
		 * ★ОСВОБОЖДАЕМ И САМИ ПАРТЫ активности (вместе с вложенными). Прежде
		 * чистился только реестр, и после загрузки сейва получался ЗАМКНУТЫЙ
		 * КРУГ: парты экрана сохранения живы → живы их наборы обработчиков →
		 * те держат окружение конструктора сцены (`SaveLoadScene@0`) → жив
		 * `SceneStack` → его `EraseLayer` не вызывается → парты живы. В обычной
		 * игре круг рвёт закрытие сцены (снос слоя по кнопке), а после resume
		 * закрывать её некому — и экран сохранения оставался поверх игры.
		 * Индексы собираем ДО освобождения: у снятой части их уже не спросишь.
		 * Откат для замеров: `XSYS4_RA_KEEP_PARTS=1`.
		 */
		for (int i = 0; i < n; i++)
			PE_ReleaseParts(a->parts[i].number);
		// Служебные части загрузчика (см. `helpers`): игра их не знает, снять
		// может только движок — иначе они остаются сиротами на экране.
		for (int i = 0; i < a->nr_helpers; i++)
			PE_ReleaseParts(a->helpers[i]);
		// Вместе с активностью уходят и ВЛОЖЕННЫЕ: игра именует их
		// «<родитель>／<узел>[:<индекс>]» (у экрана сохранения это `／項目:0..8`
		// — рамки слотов и `／ページボタン:0..9` — кнопки страниц). Их
		// собственный ReleaseActivity после загрузки уже не придёт: обёртки,
		// которые его зовут, держит тот же круг.
		pe_release_sub_activities(name, indices, &nr, cap);
	}
	if (getenv("XSYS4_BS_TRACE"))
		NOTICE("BS ReleaseActivity('%s'): отдано delegate-индексов %d (партов у корня %d)",
		       display_sjis0(name->text), nr, n);
	union vm_value dim = { .i = nr };
	struct page *page = alloc_array(1, &dim, AIN_ARRAY_INT, 0, false);
	for (int i = 0; i < nr; i++)
		page->values[i].i = indices[i];
	free(indices);
	if (*out) {
		delete_page_vars(*out);
		free_page(*out);
	}
	*out = page;
	if (a) {
		// освободить активность и убрать из реестра (перезапись пустышкой-NULL)
		struct ht_slot *slot = ht_put(pe_activities, name->text, NULL);
		if (slot->value == a)
			slot->value = NULL;
		PE_Activity_free(a);
	}
	return true;
}

static bool PE_AddActivityParts(struct string *act, struct string *parts_name, int number)
{
	struct pe_activity *a = pe_act_find(act);
	if (!a) { PE_CreateActivity(act); a = pe_act_find(act); }
	a->parts = xrealloc_array(a->parts, a->nr_parts, a->nr_parts + 1, sizeof(*a->parts));
	a->parts[a->nr_parts].name = string_ref(parts_name);
	a->parts[a->nr_parts].number = number;
	a->nr_parts++;
	if (getenv("XSYS4_ACT_TRACE"))
		NOTICE("ACT AddActivityParts(act='%s', name='%s', number=%d)",
		       display_sjis0(act->text), display_sjis1(parts_name->text), number);
	return true;
}

/*
 * Activity layout loading (.pactex). A .pactex file is in the .ex container
 * format: a tree of parts (block "アクティビティ" -> "ルートパーツ") where each
 * node has position/visibility/alpha and type-specific info (CG name per state,
 * or button CG/flat/text), plus nested "子パーツ" children. ReadActivityFile
 * parses it and creates the real parts so the dialog/menu actually renders.
 */
static int pe_act_part_seq = 90000000;  // unique base for activity-created parts

// look up a child node by UTF-8 name (converted to the .ex file's SJIS encoding)
static struct ex_tree *act_child(struct ex_tree *t, const char *utf8)
{
	if (!t || t->is_leaf)
		return NULL;
	char *sjis = utf2sjis(utf8, strlen(utf8));
	struct ex_tree *r = ex_tree_get_child(t, sjis);
	free(sjis);
	return r;
}

static int act_int(struct ex_tree *t, const char *utf8, int dflt)
{
	struct ex_tree *c = act_child(t, utf8);
	if (c && c->is_leaf && c->leaf.value.type == EX_INT)
		return c->leaf.value.i;
	return dflt;
}

static float act_float(struct ex_tree *t, const char *utf8, float dflt)
{
	struct ex_tree *c = act_child(t, utf8);
	if (c && c->is_leaf) {
		if (c->leaf.value.type == EX_FLOAT)
			return c->leaf.value.f;
		if (c->leaf.value.type == EX_INT)
			return (float)c->leaf.value.i;
	}
	return dflt;
}

static struct string *act_str(struct ex_tree *t, const char *utf8)
{
	struct ex_tree *c = act_child(t, utf8);
	if (c && c->is_leaf && c->leaf.value.type == EX_STRING)
		return c->leaf.value.s;
	return NULL;
}

/*
 * ★У IXSEAL `パーツタイプ` В РАСКЛАДКЕ — СТРОКА, А НЕ ЧИСЛО.
 *
 * Tsumamigui 3 хранит тип части числом (`パーツタイプ = 0`), Dohna — ИМЕНЕМ
 * (`パーツタイプ = "ボタン"`), и `act_int` на строковом узле честно отдавал
 * дефолт -1. Из-за этого весь разбор типа у Dohna молча не работал: ни одна
 * часть не опознавалась кнопкой, поэтому `parts::detail::GetComponentType` не
 * отдавал 0, а `activity::detail::CActivityWrap@CompParts` (@0x1fd54) сравнивает
 * его с запрошенным типом и при несовпадении заставляет `GetButton` вернуть
 * ПУСТОЙ wrap `(-1, 0)`. Титул строит список кнопок как
 * `ArrayExtensions::Select(имена, name => activity.GetButton(name))`, то есть
 * получал 8 пустых элементов. Проверялось: части НАХОДЯТСЯ
 * (`ButtonOmake -> 90000021` … `ButtonExit -> 90000025`), ломался только гейт
 * по типу. Логотип рисовался, потому что CG-части и панели и без разбора типа
 * уходят в общую ветку состояний.
 *
 * Порядок имён — не догадка: снят с SWITCH-таблицы самой игры в
 * `parts::detail::GetComponentTypeName` (@0x2edf18, switch 312, 31 ветка),
 * значение case = id типа.
 *
 * Нумерация v14 отличается от классической: у классического семейства (id 18+,
 * собственно `パーツ`) сдвиг на 8, а виджеты 10-17 классического аналога не
 * имеют — тот же перевод, что `component_type_to_classic` в src/parts/parts.c.
 * Возвращаем КЛАССИЧЕСКИЙ id, потому что именно им пользуются ветки ниже
 * (0/1/2/3 у виджетов совпадают в обеих нумерациях; テキストパーツ 21→13 и
 * 構築パーツ 26→18 совпадают с проверками в act_set_state_cg).
 */
static const char *const act_component_type_names[] = {
	"ボタン", "チェックボックス", "縦スクロールバー", "横スクロールバー",
	"テキストボックス", "リストボックス", "コンボボックス",
	"マルチラインテキストボックス", "レイアウトボックス", "ラジオボタンボックス",
	"メッセージウィンドウ", "スピンボックス", "縦スライダーバー",
	"横スライダーバー", "パネル", "フォーム", "フォームグループ",
	"ユーザコンポーネント", "低レベルパーツ", "ＣＧパーツ", "ループＣＧパーツ",
	"テキストパーツ", "横ゲージパーツ", "縦ゲージパーツ", "数字パーツ",
	"矩形パーツ", "構築パーツ", "ＣＧ判定パーツ", "フラットパーツ",
	"３Ｄレイヤパーツ", "ムービーパーツ",
};
#define ACT_COMPONENT_TYPE_SHIFT 8

static int act_parts_type(struct ex_tree *t)
{
	struct ex_tree *c = act_child(t, "パーツタイプ");
	if (!c || !c->is_leaf)
		return -1;
	if (c->leaf.value.type == EX_INT)
		return c->leaf.value.i;  // v6/v7: тип уже число классической нумерации
	if (c->leaf.value.type != EX_STRING || !c->leaf.value.s)
		return -1;
	const char *name = c->leaf.value.s->text;
	int v14 = -1;
	for (unsigned i = 0; i < sizeof(act_component_type_names) / sizeof(*act_component_type_names); i++) {
		char *sjis = utf2sjis(act_component_type_names[i], strlen(act_component_type_names[i]));
		bool hit = !strcmp(name, sjis);
		free(sjis);
		if (hit) {
			v14 = i;
			break;
		}
	}
	if (v14 < 0) {
		// Имя вне таблицы игры — не молчаливый дефолт, а проверка допущения.
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("act_parts_type: неизвестный パーツタイプ '%s'", display_sjis0(name));
		}
		return -1;
	}
	if (v14 < 10)
		return v14;                              // виджеты: нумерация общая
	if (v14 < 10 + ACT_COMPONENT_TYPE_SHIFT)
		return -1;                               // виджет только v14 (напр. パネル)
	return v14 - ACT_COMPONENT_TYPE_SHIFT;       // классическое семейство パーツ
}

/*
 * Сырой номер типа по нумерации v14 — для виджетов, у которых классического
 * аналога нет и act_parts_type честно отдаёт -1 (メッセージウィンドウ = 10,
 * パネル = 14 и т.д.). Отдельная функция, а не новое значение act_parts_type:
 * её результат сравнивается с классическими id по всему загрузчику.
 */
static int act_parts_type_v14(struct ex_tree *t)
{
	struct ex_tree *c = act_child(t, "パーツタイプ");
	if (!c || !c->is_leaf || c->leaf.value.type != EX_STRING || !c->leaf.value.s)
		return -1;
	const char *name = c->leaf.value.s->text;
	for (unsigned i = 0; i < sizeof(act_component_type_names) / sizeof(*act_component_type_names); i++) {
		char *sjis = utf2sjis(act_component_type_names[i], strlen(act_component_type_names[i]));
		bool hit = !strcmp(name, sjis);
		free(sjis);
		if (hit)
			return i;
	}
	return -1;
}

// nth element of a list-valued child, as int (floats truncated)
static int act_list_int(struct ex_tree *t, const char *utf8, int idx, int dflt)
{
	struct ex_tree *c = act_child(t, utf8);
	if (!c || !c->is_leaf || c->leaf.value.type != EX_LIST)
		return dflt;
	struct ex_list *l = c->leaf.value.list;
	if (idx < 0 || (unsigned)idx >= l->nr_items)
		return dflt;
	struct ex_value *v = &l->items[idx].value;
	if (v->type == EX_INT)
		return v->i;
	if (v->type == EX_FLOAT)
		return (int)v->f;
	return dflt;
}

// nth element of a list-valued child, as float
static float act_list_float(struct ex_tree *t, const char *utf8, int idx, float dflt)
{
	struct ex_tree *c = act_child(t, utf8);
	if (!c || !c->is_leaf || c->leaf.value.type != EX_LIST)
		return dflt;
	struct ex_list *l = c->leaf.value.list;
	if (idx < 0 || (unsigned)idx >= l->nr_items)
		return dflt;
	struct ex_value *v = &l->items[idx].value;
	if (v->type == EX_FLOAT)
		return v->f;
	if (v->type == EX_INT)
		return (float)v->i;
	return dflt;
}

static float act_float(struct ex_tree *t, const char *utf8, float dflt);

/*
 * Выполнить процедуру построения, ОПИСАННУЮ В РАСКЛАДКЕ (`手順リスト` у состояния
 * `構築パーツ`). Раньше не выполнялась ни одна: часть превращалась в
 * прямоугольную маску, и всё, что игра рисует этим механизмом, с экрана
 * пропадало. У Dohna таких состояний 178 в 109 раскладках — это подложки
 * счётчиков («Round128x40» под «TALENT 3/3»), рамки, стрелки навигации,
 * размытия фона.
 *
 * Узел `手順N` описывает ровно ту же операцию, что игра шлёт в рантайме через
 * `AddPartsConstructionProcess`, поле в поле, поэтому обе дороги сходятся в
 * `construction_op`. Возвращает число ВЫПОЛНЕННЫХ операций (неизвестные команды
 * не в счёт) — вызывающему это нужно, чтобы понять, осталась ли часть пустой.
 * `blurred` — сколько шагов размытия встретилось: по нему вызывающий узнаёт, что
 * процедура строит РАЗМЫТЫЙ ЗАДНИК экрана, а не элемент интерфейса.
 */
static int act_construction_run(int no, int state, struct ex_tree *proc, int *skipped, int *blurred)
{
	int done = 0;
	*skipped = 0;
	*blurred = 0;
	for (int i = 1; ; i++) {
		char key[32];
		snprintf(key, sizeof(key), "手順%d", i);
		struct ex_tree *op = act_child(proc, key);
		if (!op)
			break;
		int command = act_int(op, "コマンド", -1);
		if (command < 0)
			continue;
		// Команды за пределами реализованного набора пропускаем ЯВНО: пусть их
		// перечисляет один WARNING, а не тихий «unknown command» на каждую часть.
		if (command > 24 && command != 27 && command != 28 && command != 88
				&& command != 90 && command != 102 && command != 122) {
			static bool warned = false;
			if (!warned) {
				warned = true;
				WARNING("構築パーツ: команда %d раскладки не реализована "
					"(часть %d) — этот шаг пропущен", command, no);
			}
			(*skipped)++;
			continue;
		}
		if (command == 27 || command == 28)
			(*blurred)++;
		struct string *text = act_str(op, "テキスト");
		struct string *cg = act_str(op, "ＣＧ名");
		construction_op(no, state, command, act_int(op, "補間タイプ", 0),
			act_list_int(op, "元矩形", 0, 0), act_list_int(op, "元矩形", 1, 0),
			act_list_int(op, "元矩形", 2, 0), act_list_int(op, "元矩形", 3, 0),
			act_list_int(op, "先矩形", 0, 0), act_list_int(op, "先矩形", 1, 0),
			act_list_int(op, "先矩形", 4, 0), act_list_int(op, "先矩形", 5, 0),
			act_list_int(op, "色１", 0, 0), act_list_int(op, "色１", 1, 0),
			act_list_int(op, "色１", 2, 0), act_list_int(op, "色１", 3, 255),
			act_list_int(op, "色２", 0, 0), act_list_int(op, "色２", 1, 0),
			act_list_int(op, "色２", 2, 0),
			act_int(op, "文字間隔", 0), act_int(op, "行間隔", 0),
			act_int(op, "フォントタイプ", 0), act_int(op, "フォントサイズ", 16),
			act_list_int(op, "フォント色", 0, 255), act_list_int(op, "フォント色", 1, 255),
			act_list_int(op, "フォント色", 2, 255),
			act_list_int(op, "フォント縁取り色", 0, 0),
			act_list_int(op, "フォント縁取り色", 1, 0),
			act_list_int(op, "フォント縁取り色", 2, 0),
			act_int(op, "全体", 0), act_float(op, "フォント太さ", 0.0f),
			act_float(op, "フォント縁取り", 0.0f), text, cg,
			act_list_int(op, "半径", 0, 0), act_list_int(op, "半径", 1, 0),
			act_list_int(op, "円弧角度", 0, 0), act_list_int(op, "円弧角度", 1, 0),
			act_int(op, "ブラー", 0));
		done++;
	}
	return done;
}

/*
 * РЕДАКТОРСКАЯ ЗАГЛУШКА в поле `テキスト` раскладки. Обычный текст оттуда нужен —
 * подписи кнопок выбора фазы («Hustling Phase») лежат именно там, и игра их не
 * переустанавливает (проверено XSYS4_SETTEXT_TRACE: `SetText` для них не приходит).
 * Но у части заготовок текст — буквальный placeholder редактора: `パーツテキスト`
 * («текст части») у строки-подсказки футера, `ボタン` («кнопка») у подписи
 * FooterButton. Игра переписывает их, ТОЛЬКО если ей есть что показать: у футера
 * обучения подсказки нет (в EX-таблице `FooterText` нет записи для этой сцены), и
 * заглушка оставалась на экране вместе с белой подложкой, которой у оригинала нет.
 */
static bool act_is_placeholder_text(struct string *txt)
{
	static const char *const placeholders[] = {
		"パーツテキスト",  // «текст части» — подпись-заготовка в Footer.pactex
		"ボタン",          // «кнопка» — подпись-заготовка в FooterButton.pactex
	};
	for (size_t i = 0; i < sizeof(placeholders) / sizeof(placeholders[0]); i++) {
		char *sjis = utf2sjis(placeholders[i], strlen(placeholders[i]));
		size_t n = strlen(sjis);
		// ★Сравнение по ДЛИНЕ, а не strcmp: строки из EX-дерева не обязаны быть
		// NUL-терминированными, и strcmp читал за границей буфера (движок падал
		// молча на загрузке раскладок, ещё до титула).
		bool hit = txt->size >= 0 && (size_t)txt->size == n && !memcmp(txt->text, sjis, n);
		free(sjis);
		if (hit)
			return true;
	}
	return false;
}

static void act_set_state_cg(int no, struct ex_tree *ti, const char *state_utf8, int state)
{
	struct ex_tree *st = act_child(ti, state_utf8);
	if (!st)
		return;
	// A state may be a Flat (パーツタイプ=20), a CG (11), or Text (13).
	struct string *flat = act_str(st, "フラット名");
	if (flat && flat->size) {
		PE_SetPartsFlat(no, flat, state);
		/*
		 * `FPS` из описания состояния мы НЕ применяем как скорость
		 * воспроизведения — проверено и опровергнуто. У титульного фона
		 * Tsumamigui 3 там стоит 1.0, и при такой скорости заставка растягивается
		 * на минуту: игра не листает флэт сама, а ЧИТАЕТ его текущий кадр
		 * (`AFL_Parts_GetPartsFlatCurrentFrameNumber` в цикле
		 * `C_TITLE@LoadedActivityEvent`) и по нему подгоняет прозрачность и
		 * отступы кнопок. То есть темп задаёт сам флэт своим заголовком, а что
		 * означает это поле — пока не установлено.
		 */
		// Начальный кадр состояния (`カレントフレーム`).
		int cur = act_int(st, "カレントフレーム", 0);
		if (cur > 0)
			PE_GoFramePartsFlat(no, cur, state);
		return;
	}
	// Text state: テキスト + テキスト装飾 (font). Used e.g. by the config sample
	// message ("サンプル") whose 通常状態 is a パーツタイプ=13 text sub-node.
	if (act_parts_type(st) == 13) {
		struct string *txt = act_str(st, "テキスト");
		// ★Шрифт настраиваем ДАЖЕ для заглушки: без него часть остаётся без
		// метрик, и когда игра позже кладёт в неё свой текст, рендер падает
		// молча (движок умирал на загрузке ADV-сцены, 0 *ERROR* в логе).
		bool placeholder = txt && act_is_placeholder_text(txt);
		if (txt && txt->size) {
			struct ex_tree *fd = act_child(st, "テキスト装飾");
			if (fd) {
				PE_SetFont(no, act_int(fd, "フォントタイプ", 0),
					act_int(fd, "フォントサイズ", 16),
					act_list_int(fd, "フォント色", 0, 255),
					act_list_int(fd, "フォント色", 1, 255),
					act_list_int(fd, "フォント色", 2, 255),
					act_float(fd, "フォント太さ", 0.0f),
					act_list_int(fd, "フォント縁取り色", 0, 0),
					act_list_int(fd, "フォント縁取り色", 1, 0),
					act_list_int(fd, "フォント縁取り色", 2, 0),
					act_float(fd, "フォント縁取り", 0.0f), state);
				/*
				 * 行間隔 (межстрочный) применяем, 字間隔 (межбуквенный) — НЕТ.
				 * Замер против эталона (экран SAVE, `Снимок экрана_20260806_140007.png`,
				 * слот 3): у оригинала строка DATE «8月01日 朝» занимает 96 px, а
				 * COMMENT «1231231» — 47 px. В данных у обеих частей
				 * `フォントサイズ = 14` и `字間隔 = -4`. Без вычитания получается
				 * 7 знаков × 14 px = 98 для полноширинной строки и 7 × 7 px = 49
				 * для полуширинных цифр — обе сходятся с эталоном. С вычитанием
				 * −4 не сходится НИ ОДНА: 70 и 21 соответственно, а у нас на
				 * экране выходило 57 и 17, и комментарий превращался в
				 * нечитаемую кляксу (буквы налезали друг на друга).
				 * Что означает `字間隔` в декорации активности — не установлено;
				 * пиксельной надбавкой к каждому глифу оно точно не является.
				 * Ручка `XSYS4_ACT_CHARSPACE=1` возвращает прежнее поведение для A/B.
				 */
				// ★ВЫВОД ВЫШЕ ПЕРЕСМОТРЕН (FINDINGS §5af): `字間隔` — это ИМЕННО
				// пиксельная поправка к шагу глифа, применять её НАДО. Не сходилась
				// она из-за ДВУХ других ошибок, обе с тех пор закрыты: GUI-части
				// рисовались шрифтом из `.fnl` вместо системного (лица 0/1 —
				// встроенные, см. `get_font` в `src/text.c`), и обводка входила в шаг
				// ДРОБНОЙ величиной вместо целой на сторону. С обеими правками
				// арифметика сходится сразу на трёх эталонах: слот
				// `14 + 2×2 − 4 = 14` и `7 + 2×2 − 4 = 7`, бэклог `ceil(A) + 2×2 − 1`.
				// Отключение `字間隔` попутно РАЗЪЕЗЖАЛО бэклог: у его текста
				// `字間隔 = −1`, и без поправки строки выходили на 1 px на глиф шире
				// оригинала (462 px против 420 на строке из 43 знаков).
				// Ручка `XSYS4_ACT_NO_CHARSPACE=1` возвращает прежнее поведение.
				if (!getenv("XSYS4_ACT_NO_CHARSPACE"))
					PE_SetTextCharSpace(no, act_int(fd, "字間隔", 0), state);
				PE_SetTextLineSpace(no, act_int(fd, "行間隔", 0), state);
			}
			if (!placeholder)
				PE_SetText(no, txt, state);
		}
		return;
	}
	// Numeral state (パーツタイプ=16): цифры рисуются набором CG по шаблону имени
	// (`ＣＧ名 = "バックシーン／数字／%02d"` → …／00…／11, где 10 = минус, 11 = запятая).
	// Так устроен счётчик страниц BACK SCENE (`シーン数 ００３／００３`): числитель и
	// знаменатель — numeral-парты с 桁数=3 и ゼロパディング=1, между ними отдельный
	// CG-парт со слэшем. Загрузчик активности этот тип состояния не разбирал вовсе,
	// поэтому цифр не было ни одной. `フォント*`-поля тут для варианта «цифры текстом»
	// (全角=0, у Tsumamigui 3 используется именно CG-вариант).
	// act_parts_type, а не act_int: у Ixseal/Dohna тип состояния в .pactex — СТРОКА
	// («数字パーツ»), и act_int молча отдавал бы -1, из-за чего 112 numeral-состояний
	// Dohna не создавались бы вовсе.
	if (act_parts_type(st) == 16) {
		struct string *cg = act_str(st, "ＣＧ名");
		/*
		 * `表示タイプ = 1` — ЕДИНАЯ ЛЕНТА цифр: `ＣＧ名` указывает на один CG
		 * со всеми глифами подряд, а `幅リスト` даёт 12 ширин (0-9, минус,
		 * запятая). Раскладке нужен PE_SetNumeralLinkedCGNumberWidthWidthList,
		 * а не PE_SetNumeralCG: у имени нет %d-плейсхолдера, и separate-
		 * загрузчик находил ПО ЭТОМУ ИМЕНИ САМУ ЛЕНТУ для всех 12 глифов —
		 * каждая цифра рисовалась целой лентой «0123456789…» (кнопки страниц
		 * экрана сейвов Haha Ranman, «шум из цифр» вместо номеров), причём
		 * без единого предупреждения — CG-то существует.
		 */
		if (cg && cg->size && act_int(st, "表示タイプ", 0) == 1) {
			int w[12];
			for (int i = 0; i < 12; i++)
				w[i] = act_list_int(st, "幅リスト", i, 0);
			PE_SetNumeralLinkedCGNumberWidthWidthList(no, cg,
				w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
				w[8], w[9], w[10], w[11], state);
		} else if (cg && cg->size)
			PE_SetNumeralCG(no, cg, state);
		/*
		 * `表示タイプ = 2` — цифры рисуются ШРИФТОМ, а не набором CG (`ＣＧ名`
		 * при таком режиме пуст). У Dohna так сделаны ВСЕ счётчики интерфейса:
		 * «TALENT 3/3» и «Client 1/4» на экране подбора талантов — это части
		 * `Remain`/`Max` из `PlayerShopView.pactex` с `表示タイプ = 2`,
		 * `フォントタイプ = 258`, `フォントサイズ = 24`. Загрузчик режима не знал,
		 * шрифтовые поля игнорировал — и на месте чисел не было ничего (сверено
		 * с оригиналом на экране Hustling). У Tsumamigui 3 режим CG-цифр, эти
		 * поля там не используются.
		 */
		if (act_int(st, "表示タイプ", 0) == 2) {
			PE_SetNumeralFont(no, act_int(st, "フォントタイプ", 0),
				act_int(st, "フォントサイズ", 16),
				act_list_int(st, "フォント色", 0, 255),
				act_list_int(st, "フォント色", 1, 255),
				act_list_int(st, "フォント色", 2, 255),
				act_float(st, "フォント太さ", 0.0f),
				act_list_int(st, "フォント縁取り色", 0, 0),
				act_list_int(st, "フォント縁取り色", 1, 0),
				act_list_int(st, "フォント縁取り色", 2, 0),
				act_float(st, "フォント縁取り", 0.0f), state);
		}
		PE_SetNumeralSpace(no, act_int(st, "字間隔", 0), state);
		PE_SetNumeralShowComma(no, act_int(st, "コンマ表示", 0), state);
		// 桁数 задаёт разрядность, но дополнять нулями можно только с ゼロパディング=1:
		// иначе движок сам допишет ведущие нули там, где игра их не ждёт.
		if (act_int(st, "ゼロパディング", 0))
			PE_SetNumeralLength(no, act_int(st, "桁数", 0), state);
		PE_SetNumeralNumber(no, act_int(st, "数値", 0), state);
		return;
	}
	/*
	 * Гейдж-состояние (横ゲージパーツ = 14, 縦ゲージパーツ = 15). Загрузчик его не
	 * разбирал: `ＣＧ名` уходил в общую CG-ветку внизу, часть становилась
	 * `PARTS_CG` — и игра переставала её НАХОДИТЬ, потому что ищет сравнением
	 * типа компонента (`CActivityWrap@GetHGauge` @fn603 → `CompParts(имя, 22,
	 * state)`, у вертикального — 23). На этом падал игровой ассерт
	 * `DecisionTimerView.jaf:19: (nonnull) m_act.GetHGauge("Gauge")` при входе в
	 * магазин Dohna: у части «Gauge» узел `通常状態` — как раз 横ゲージパーツ с
	 * `ＣＧ名 = "システム／ハルウリ／時間ゲージ"`. Тот же класс дефекта, что §5ak,
	 * но чинится в ЗАГРУЗЧИКЕ: тип компонента у `PARTS_HGAUGE` уже верный.
	 *
	 * Порядок важен: `分子/分母` ставятся ПОСЛЕ CG — установка CG заводит
	 * состояние гейджа заново.
	 */
	if (act_parts_type(st) == 14 || act_parts_type(st) == 15) {
		bool horizontal = act_parts_type(st) == 14;
		struct string *cg = act_str(st, "ＣＧ名");
		if (cg && cg->size) {
			if (horizontal)
				PE_SetHGaugeCG(no, cg, state);
			else
				PE_SetVGaugeCG(no, cg, state);
		}
		float denom = act_float(st, "分母", 1.0f);
		float numer = act_float(st, "分子", 0.0f);
		if (horizontal)
			PE_SetHGaugeRate(no, numer, denom, state);
		else
			PE_SetVGaugeRate(no, numer, denom, state);
		int sx = act_list_int(st, "サーフェイスエリア", 0, 0);
		int sy = act_list_int(st, "サーフェイスエリア", 1, 0);
		int sw = act_list_int(st, "サーフェイスエリア", 2, 0);
		int sh = act_list_int(st, "サーフェイスエリア", 3, 0);
		if (sw > 0 && sh > 0) {
			if (horizontal)
				PE_SetHGaugeSurfaceArea(no, sx, sy, sw, sh, state);
			else
				PE_SetVGaugeSurfaceArea(no, sx, sy, sw, sh, state);
		}
		// 反転 (расти справа налево / снизу вверх) движок не поддерживает вовсе —
		// соответствующего PE_-вызова нет. Молчать об этом нельзя: гейдж будет
		// расти не в ту сторону. У всех гейджей Dohna в раскладках стоит 0.
		if (act_int(st, "反転", 0)) {
			static bool warned = false;
			if (!warned) {
				warned = true;
				WARNING("act_set_state_cg: гейдж с 反転=1 не поддержан (parts %d)", no);
			}
		}
		return;
	}
	// Construction-process viewport (パーツタイプ=18): used as a clip region for
	// preview content (e.g. the config message-window sample). We don't run the
	// full compositing procedure; we only need an opaque mask of the viewport
	// rect so it can serve as an alpha-clipper (rectangular clip) for siblings.
	// The 手順1 create op's 先矩形 gives the size [.,.,.,.,w,h].
	if (act_parts_type(st) == 18) {
		// Тип состояния выставляем ВСЕГДА, даже когда `手順リスト` пуст (так у
		// «PlayerC» в StandView): игра ищет такие части сравнением типа
		// компонента (`CActivityWrap@GetConstruction`), и без этого падал ассерт
		// `StandView.jaf:52: (nonnull) m_act.GetConstruction("PlayerC")`.
		PE_ClearPartsConstructionProcess(no, state);
		struct ex_tree *proc = act_child(st, "手順リスト");
		struct ex_tree *step1 = proc ? act_child(proc, "手順1") : NULL;
		/*
		 * Операции процедуры ЗАПИСЫВАЕМ всегда, а СОБИРАЕМ поверхность — нет.
		 * Разделение не косметическое, оно доказано A/B-прогонами:
		 *  • только запись (сборку делает сама игра, когда ей нужно) — на экране
		 *    появляются логотип магазина, эмблемы над карточками, подпись
		 *    «TIME LEFT» и стрелки навигации, всё как у оригинала;
		 *  • плюс наша сборка прямо из загрузчика — добавляются скруглённые
		 *    подложки счётчиков, НО часть процедур создаёт полноэкранные
		 *    поверхности (`CP BUILD` показывает 1380×820 и 1480×920), они
		 *    перекрывают верхние слои, и логотип пропадает.
		 * ★Причина того перекрытия — НЕ z, а незаконченная процедура: те
		 * полноэкранные поверхности строят размытый задник экрана
		 * (`SetCreateCG` → `Set{H,V}BlurFilter` → `SetFillAlphaColor`), а команды
		 * размытия не были реализованы — шаг пропускался, и вместо задника
		 * оставался чёрный непрозрачный прямоугольник. С реализованными 27/28
		 * (см. `PARTS_CP_BLUR_FILTER`) процедура доходит до конца, поэтому сборка
		 * из загрузчика включена ПО УМОЛЧАНИЮ; отключить для A/B —
		 * XSYS4_CP_NO_ACT_BUILD=1.
		 */
		if (step1) {
			int skipped = 0, blurred = 0;
			int nr = act_construction_run(no, state, proc, &skipped, &blurred);
			// Операции только НАКАПЛИВАЮТСЯ; поверхность появляется лишь после
			// сборки. В рантайме её запускает сама игра
			// (`Parts_BuildPartsConstructionProcess`), а для процедуры из
			// раскладки звать некому — иначе часть остаётся пустой, и вся работа
			// выше не видна на экране.
			/*
			 * Собираем и ПОКАЗЫВАЕМ часть только если выполнены ВСЕ шаги её
			 * процедуры. Недостроенная поверхность — это не «частично верная
			 * картинка», а чёрный непрозрачный прямоугольник: у титула Dohna
			 * есть такая часть 1480×920 с `表示 = 1, アルファ = 255, z = 29`,
			 * и, построенная наполовину, она накрывала весь экран (логотип
			 * магазина и эмблемы на экране подбора талантов исчезали).
			 * Пропущен хоть один шаг — ведём себя как раньше, прямоугольной
			 * маской альфа-клиппера.
			 * XSYS4_CP_NO_ACT_BUILD=1 — A/B-ручка, отключает сборку целиком.
			 */
			// ★По умолчанию собираем ТОЛЬКО задники — процедуры с размытием
			// (`Set{H,V}BlurFilter`): без них экран остаётся без фона, а сквозь
			// панель просвечивает предыдущая сцена. Прочие процедуры (подложки
			// счётчиков и т.п.) по-прежнему под ручкой XSYS4_CP_ACT_BUILD=1:
			// с ними на экране подбора талантов ПРОПАДАЕТ логотип магазина
			// (часть 90000941 остаётся `lshow=1 gshow=1 alpha=255`, то есть её
			// перекрывают, а чем — пока не выяснено; в слое титула висит
			// собранная поверхность 1480×920 `90000041` с alpha=255).
			/*
			 * ★Плюс процедуры, которые строят поверхность НЕ БОЛЬШЕ экрана:
			 * поломка выше — про полноэкранные задники (1380×820 и 1480×920
			 * при экране 1280×720), они перекрывают верхние слои. Элемент
			 * интерфейса такого размера не бывает, и без сборки он остаётся
			 * пустым: превью страницы `Text Area UI` — часть 640×180, чья
			 * процедура берёт полосу `背景／ナユタ` (元矩形 0,360,1280,360) и
			 * ужимает её в размер превью (команда 13). Без сборки на месте
			 * картинки города был чёрный прямоугольник, а у оригинала там
			 * сцена с рамкой окна реплик.
			 */
			int cw = act_list_int(step1, "先矩形", 4, 0);
			int ch = act_list_int(step1, "先矩形", 5, 0);
			bool fits_screen = cw > 0 && ch > 0
				&& cw <= config.view_width && ch <= config.view_height;
			bool build = blurred > 0 || fits_screen || getenv("XSYS4_CP_ACT_BUILD");
			if (nr > 0 && skipped == 0 && build && !getenv("XSYS4_CP_NO_ACT_BUILD"))
				PE_BuildPartsConstructionProcess(no, state);
			// Процедуры без единой ВЫПОЛНЕННОЙ операции (все команды
			// неизвестны) оставляем прежним поведением — прямоугольной
			// маской альфа-клиппера: лучше клип по габаритам, чем пустая
			// часть. Заодно так ведут себя `表示 = 0`-клипперы Tsumamigui 3.
			if (nr == 0 || skipped > 0) {
				int w = act_list_int(step1, "先矩形", 4, 0);
				int h = act_list_int(step1, "先矩形", 5, 0);
				if (w > 0 && h > 0) {
					PE_SetPartsConstructionFill(no, w, h, state);
					PE_SetPartsConstructionMask(no);
				}
			}
		}
		return;
	}
	/*
	 * `ＣＧ判定パーツ` (классический id 19) — область попадания по форме
	 * картинки: сама она не рисуется, а её непрозрачные пиксели задают
	 * hit-область. Игра ищет такие части сравнением типа компонента
	 * (`CActivityWrap@GetCGDetection` → CompParts(имя, 27, 1)), поэтому без
	 * своей ветки состояние оставалось обычным CG и падал ассерт
	 * `FooterButton.jaf:53: (nonnull) m_act.GetCGDetection("Detector")`.
	 */
	if (act_parts_type(st) == 19) {
		struct string *cg = act_str(st, "ＣＧ名");
		PE_SetPartsCGDetectionSize(no, cg, state);
		// `サーフェイスエリア` у всех семи таких состояний во всех 195 раскладках
		// нулевая, а общий сеттер площади пришлось бы звать через parts_get_cg,
		// который сбросил бы тип состояния обратно в CG. Ненулевую отмечаем.
		if (act_list_int(st, "サーフェイスエリア", 2, 0) > 0
		    || act_list_int(st, "サーフェイスエリア", 3, 0) > 0) {
			static bool warned = false;
			if (!warned) {
				warned = true;
				WARNING("ＣＧ判定パーツ: ненулевая サーフェイスエリア не применяется");
			}
		}
		return;
	}
	/*
	 * Прямоугольная часть (`矩形パーツ`, классический id 17) — не картинка, а
	 * область определения попадания, заданная ЧЕТЫРЬМЯ УГЛАМИ
	 * (左上/右上/左下/右下), а не парой ширина-высота.
	 *
	 * Без этой ветки состояние оставалось «CG», и игра, которая гейтится по типу
	 * компонента (`activity::detail::CActivityWrap@GetRect` → `CompParts(имя, 25, 1)`
	 * → сравнение с `parts::detail::GetComponentType`), получала из `GetRect`
	 * ПУСТОЙ wrap: так падал `ArrayExtensions::Select<IRectParts&, string>` в
	 * `TitleCharacterView@Attach`.
	 *
	 * Данные по всем 195 раскладкам Dohna: 216 таких состояний, у ВСЕХ
	 * `矩形モード = 1` и `左上 = (0,0)`. Смысл режима и ненулевого начала координат
	 * не установлен — вместо тихого дефолта стоит проверка допущения.
	 */
	if (act_parts_type(st) == 17) {
		int x0 = act_list_int(st, "左上", 0, 0);
		int y0 = act_list_int(st, "左上", 1, 0);
		int w = act_list_int(st, "右上", 0, 0) - x0;
		int h = act_list_int(st, "左下", 1, 0) - y0;
		int mode = act_int(st, "矩形モード", 1);
		if (mode != 1) {
			static bool warned_mode = false;
			if (!warned_mode) {
				warned_mode = true;
				WARNING("矩形パーツ: 矩形モード=%d, смысл не установлен "
					"(во всех раскладках Dohna было 1)", mode);
			}
		}
		if (x0 || y0) {
			static bool warned_org = false;
			if (!warned_org) {
				warned_org = true;
				WARNING("矩形パーツ: 左上=(%d,%d) не в начале координат, "
					"смещение не учитывается", x0, y0);
			}
		}
		if (w > 0 && h > 0) {
			// Часть ещё не существует: у прямоугольного состояния нет
			// картинки, а значит и сеттера, который создал бы её попутно.
			PE_EnsureParts(no);
			PE_SetPartsRectangleDetectionSize(no, w, h, state);
		}
		return;
	}
	struct string *cg = act_str(st, "ＣＧ名");
	if (cg && cg->size)
		PE_SetPartsCG(no, cg, 0, state);
}

// Forward-declared vertical-scrollbar state setters (defined further below).
static void PartsEngine_SetVScrollbarTotalSize(int parts_no, int size);
static void PartsEngine_SetVScrollbarViewSize(int parts_no, int size);
static void PartsEngine_SetVScrollbarScrollRate(int parts_no, float rate);
struct pe_vscrollbar {
	int parts_no;       // the knob part (its CG is ／バー)
	int total_size;
	int view_size;
	int scroll_pos;
	int up_no, down_no; // the ∧/∨ arrow decoration parts (-1 if none)
	struct string *cg_base;  // base CG name for switching arrow 通常/無効
	int enabled;        // -1 unknown, 0 disabled (nothing to scroll), 1 enabled
};
static struct pe_vscrollbar *pe_vscrollbar_get(int parts_no, bool create);
static void pe_vscrollbar_apply_enabled(struct pe_vscrollbar *sb);
struct pe_button_state {
	int parts_no;
	struct string *cg_base;
	int enabled;  // -1 ещё не спрашивали, 0 выключена, 1 включена
	/*
	 * Подпись НА кнопке — отдельная текстовая часть-ребёнок, созданная при
	 * разборе раскладки (шрифт и цвет оттуда же). Игра меняет только СТРОКУ,
	 * через `SetButtonText`, поэтому храним номер части и геометрию кнопки,
	 * чтобы каждый раз перецентрировать надпись.
	 */
	int text_no;
	int text_x, text_y, box_w, box_h, font_size;
};

static struct pe_button_state *pe_button_get(int parts_no, bool create);
// Запоминает базовое имя CG кнопки — по нему SetButtonEnable подставляет `／無効`.
static void pe_button_remember_cg(int parts_no, struct string *cg_base);

// base CG name + UTF-8 suffix -> new SJIS string (caller frees). For composite
// widgets whose sub-CGs are stored as "<base><suffix>" in the CG archive
// (e.g. scrollbar "<base>／バー／通常", "<base>／背景", "<base>／上ボタン／通常").
static struct string *act_cg_suffix(struct string *base, const char *utf8_suffix)
{
	char *sjis = utf2sjis(utf8_suffix, strlen(utf8_suffix));
	struct string *suf = make_string(sjis, strlen(sjis));
	struct string *full = string_concatenate(base, suf);
	free_string(suf);
	free(sjis);
	return full;
}

/*
 * `オンカーソル表示連動` — ИМЯ соседней части, при наведении на которую эта часть
 * показывается. Загрузчик поле не читал вовсе, и подсказки, которым положено
 * появляться по очереди, висели на экране все сразу: на странице `Window` конфига
 * Dohna три пояснения (`Scaling`, `Display Mode`, `Fullscreen Mode Aspect Ratio`)
 * печатались одно поверх другого в блоке `Hint`. Непустое поле есть ровно у четырёх
 * частей во всей игре, и все четыре — в `コンフィグ／０６／ウィンドウページ.x`.
 *
 * Разрешаем ОТЛОЖЕННО: цель — сосед по дереву и может быть построена позже нас.
 * Ищем по именам ЭТОЙ постройки, а не по регистру активности (`a->parts`): у одной
 * раскладки живёт несколько экземпляров, и второй получил бы ссылку на часть первого.
 * Списки сохраняются/восстанавливаются вокруг вложенного `ReadActivityFile`
 * (раскладка тянет за собой активности своих user-компонентов) — иначе пара уехала бы
 * в чужое дерево, где имена самые ходовые.
 */
struct act_hover_pending { int parts_no; struct string *target_name; };
static struct act_hover_pending *act_hover_pending;
static int act_hover_nr;
struct act_built_name { struct string *name; int no; };
static struct act_built_name *act_built;
static int act_built_nr;

// Строки из EX-дерева не обязаны быть NUL-терминированными (§5be: strcmp на них
// молча ронял загрузку раскладок) — сравниваем по длине и memcmp.
static bool act_name_eq(struct string *a, struct string *b)
{
	return a && b && a->size == b->size && !memcmp(a->text, b->text, a->size);
}

static void act_apply_pending_hover_links(void)
{
	for (int i = 0; i < act_hover_nr; i++) {
		int target_no = -1;
		for (int j = 0; j < act_built_nr; j++) {
			if (act_name_eq(act_built[j].name, act_hover_pending[i].target_name)) {
				target_no = act_built[j].no;
				break;
			}
		}
		if (target_no < 0) {
			WARNING("act_build_part: hover-link target '%s' not found in this activity",
			        display_sjis0(act_hover_pending[i].target_name->text));
			continue;
		}
		PE_set_on_cursor_show_link(act_hover_pending[i].parts_no, target_no);
		if (getenv("XSYS4_PARTS_TRACE") || getenv("XSYS4_ACT_TRACE"))
			NOTICE("ACT hover-link: part %d shown while cursor over '%s' (part %d)",
			       act_hover_pending[i].parts_no,
			       display_sjis0(act_hover_pending[i].target_name->text), target_no);
	}
	free(act_hover_pending);
	act_hover_pending = NULL;
	act_hover_nr = 0;
	free(act_built);
	act_built = NULL;
	act_built_nr = 0;
}

// Returns the (top-level) parts number built for `node`, or -1 for a leaf/null.
static int act_build_part(struct pe_activity *a, struct ex_tree *node, int parent_no)
{
	if (!node || node->is_leaf)
		return -1;
	int no = ++pe_act_part_seq;

	act_built = xrealloc_array(act_built, act_built_nr, act_built_nr + 1, sizeof(*act_built));
	act_built[act_built_nr].name = node->name;
	act_built[act_built_nr].no = no;
	act_built_nr++;
	struct string *hover_target = act_str(node, "オンカーソル表示連動");
	if (hover_target && hover_target->size > 0) {
		act_hover_pending = xrealloc_array(act_hover_pending, act_hover_nr,
		                                   act_hover_nr + 1, sizeof(*act_hover_pending));
		act_hover_pending[act_hover_nr].parts_no = no;
		act_hover_pending[act_hover_nr].target_name = hover_target;
		act_hover_nr++;
	}

	// register name -> number so the game can resolve it (GetActivityPartsNumber)
	a->parts = xrealloc_array(a->parts, a->nr_parts, a->nr_parts + 1, sizeof(*a->parts));
	struct pe_act_part *ap = &a->parts[a->nr_parts];
	ap->name = string_ref(node->name);
	ap->number = no;
	ap->intent_type = 0;
	ap->intent_dests = NULL;
	ap->nr_intent_dests = 0;
	a->nr_parts++;

	// intent (遷移): 遷移方法 = transition method, 遷移先 = destination list.
	// A part with a transition method acts as an "end/transition" element: when
	// clicked, MouseLClickEvent builds an intent from this and the framework
	// closes/transitions the activity. (BindEndEvent gates on IsExistIntentData.)
	// NOTE: ap is stable here — the recursion below reallocs a->parts, so we must
	// finish filling ap before recursing.
	ap->intent_type = act_int(node, "遷移方法", 0);
	struct ex_tree *dst = act_child(node, "遷移先");
	if (dst && dst->is_leaf && dst->leaf.value.type == EX_LIST) {
		struct ex_list *l = dst->leaf.value.list;
		for (unsigned i = 0; i < l->nr_items; i++) {
			struct ex_value *v = &l->items[i].value;
			if (v->type == EX_STRING && v->s && v->s->size > 0) {
				ap->intent_dests = xrealloc_array(ap->intent_dests,
					ap->nr_intent_dests, ap->nr_intent_dests + 1, sizeof(struct string *));
				ap->intent_dests[ap->nr_intent_dests++] = string_ref(v->s);
			}
		}
	}

	struct ex_tree *ti = act_child(node, "種類別情報");
	int ptype = ti ? act_parts_type(ti) : -1;
	if (getenv("XSYS4_PT_TRACE")) {
		struct string *dcg = ti ? act_str(ti, "ＣＧ名") : NULL;
		NOTICE("PT part '%s' no=%d ptype=%d cg='%s'", display_sjis0(node->name->text),
		       no, ptype, dcg ? display_sjis1(dcg->text) : "(nil)");
	}

	if (ptype == 0 && ti) {
		// button (パーツタイプ=0): the .pactex gives a *base* CG name; the actual
		// per-state assets append a state suffix — normal/hover/down are stored as
		// "<base>／通常", "<base>／オン", "<base>／ダウン" in the CG archive.
		// (state param: 1=DEFAULT/通常, 2=HOVERED/オン, 3=CLICKED/ダウン)
		static const char *const btn_sfx[4] = { NULL, "／通常", "／オン", "／ダウン" };
		struct string *cg = act_str(ti, "ＣＧ名");
		struct string *flat = act_str(ti, "フラット名");
		// Запоминаем БАЗУ имени: по ней SetButtonEnable подставит `／無効`
		// (серый вид недоступной кнопки), которого среди трёх состояний нет.
		pe_button_remember_cg(no, cg);
		int bw = act_list_int(ti, "サイズ", 0, 0);
		int bh = act_list_int(ti, "サイズ", 1, 0);
		for (int s = 1; s <= 3; s++) {
			bool have_visual = false;
			if (cg && cg->size) {
				char *sjis = utf2sjis(btn_sfx[s], strlen(btn_sfx[s]));
				struct string *suf = make_string(sjis, strlen(sjis));
				struct string *full = string_concatenate(cg, suf);
				have_visual = PE_SetPartsCG(no, full, 0, s);
				free_string(full);
				free_string(suf);
				free(sjis);
			} else if (flat && flat->size) {
				have_visual = PE_SetPartsFlat(no, flat, s);
			}
			// Fallback hit-area from the declared size ONLY when no CG/flat loaded.
			// (SetPartsRectangleDetectionSize resets the state to RECT_DETECTION,
			// which would erase a CG; a loaded CG provides its own hitbox.)
			if (!have_visual && bw > 0 && bh > 0)
				PE_SetPartsRectangleDetectionSize(no, bw, bh, s);
		}
		PE_SetClickable(no, true);
		PE_SetPartsIsButton(no, true);  // report component type 0 (button) to the game
		/*
		 * Подпись НА кнопке. У типа `ボタン` она лежит в самой кнопке
		 * (`テキスト` + свой шрифт + `テキスト位置`), а не в отдельной текстовой
		 * части, и раньше не рисовалась вовсе: в CONFIG плашки «Reset» выходили
		 * пустыми белыми прямоугольниками. Рисуем так же, как подпись чекбокса —
		 * отдельной текстовой частью-ребёнком, но ПО ЦЕНТРУ кнопки
		 * (`テキスト位置 = 5` у всех кнопок Dohna — то же значение, что
		 * `原点座標モード` для центра).
		 */
		struct string *btxt = act_str(ti, "テキスト");
		if (btxt && btxt->size && !act_is_placeholder_text(btxt)) {
			int cw = PE_GetPartsWidth(no, 1);
			int ch = PE_GetPartsHeight(no, 1);
			if (cw <= 0) cw = bw;
			if (ch <= 0) ch = bh;
			int fsize = act_int(ti, "フォントサイズ", 16);
			int tno = ++pe_act_part_seq;
			pe_act_add_helper(a, tno);
			PE_SetText(tno, btxt, 1);
			PE_SetFont(tno, act_int(ti, "フォントタイプ", 0), fsize,
				act_list_int(ti, "フォント色", 0, 0),
				act_list_int(ti, "フォント色", 1, 0),
				act_list_int(ti, "フォント色", 2, 0),
				act_float(ti, "フォント太さ", 0.0f),
				act_list_int(ti, "フォント縁取り色", 0, 0),
				act_list_int(ti, "フォント縁取り色", 1, 0),
				act_list_int(ti, "フォント縁取り色", 2, 0),
				act_float(ti, "フォント縁取り", 0.0f), 1);
			/*
			 * ★Подпись — ребёнок САМОЙ КНОПКИ, а не её родителя. Пока она
			 * висела сестрой, она жила своей жизнью: показ, масштаб и альфа
			 * кнопки её не касались. Что это давало на экране CONFIG у Dohna:
			 *  — `SYS_背景音声ボタン` (`エディタ上表示 = 0`) скрыт правилом
			 *    части-образца, плашки нет — а надпись `初期化` осталась висеть
			 *    поверх фона под кнопкой Reset у Voices (замер: скрытая часть
			 *    1048,352 130x44, подпись 1089,366 48x16, lshow=1 при lshow=0
			 *    у хозяина);
			 *  — у мелких кнопок Reset (`拡大縮小 = 0.8`) плашка ужималась, а
			 *    подпись рисовалась в натуральный кегль и вылезала за неё.
			 * Ребёнку и показ, и масштаб, и позиция достаются от родителя
			 * (parts_update_global_*), поэтому координаты теперь ОТНОСИТЕЛЬНЫЕ:
			 * centering считается внутри бокса кнопки, а не от её места на экране.
			 */
			int tw = PE_GetPartsWidth(tno, 1);
			int tx = (cw > tw ? (cw - tw) / 2 : 0);
			int ty = (ch > fsize ? (ch - fsize) / 2 : 0);
			PE_SetPos(tno, tx, ty);
			PE_SetParentPartsNumber(tno, no);
			// ★Подпись лежит ПОВЕРХ плашки кнопки: с равным z она уходила под
			// картинку кнопки и не была видна (плашки «Reset» в CONFIG выходили
			// пустыми, хотя текст в часть ставился — видно в XSYS4_SETTEXT_TRACE).
			// Z у ребёнка складывается с родительским, поэтому здесь просто +1.
			PE_SetZ(tno, 1);
			PE_SetShow(tno, 1);
			struct pe_button_state *b = pe_button_get(no, true);
			b->text_no = tno;
			b->text_x = 0;
			b->text_y = 0;
			b->box_w = cw;
			b->box_h = ch;
			b->font_size = fsize;
		}
	} else if (ptype == 3 && ti) {
		// horizontal scrollbar / slider (パーツタイプ=3). ＣＧ名 is a *base* name;
		// the draggable knob ("bar") is stored per-state as "<base>／バー／通常",
		// "<base>／バー／オン", "<base>／バー／ダウン" in the CG archive (the groove
		// "<base>／背景" and the frame CG provide the rail). Track geometry
		// (length/width/total/view/rate) lives here in 種類別情報.
		static const char *const bar_sfx[4] = { NULL, "／バー／通常", "／バー／オン", "／バー／ダウン" };
		struct string *cg = act_str(ti, "ＣＧ名");
		struct string *flat = act_str(ti, "フラット名");
		/*
		 * `上書きバーＣＧ名` — ГОТОВОЕ имя картинки бегунка, взамен производного
		 * `<база>／バー／…`. Поле не читалось, и у Dohna слайдеры конфига оставались
		 * без белого кружка: их база называется `システム／スクロールバーダミー`
		 * («заглушка») и в архиве у неё есть только `／背景` — жёлоб, — а сам
		 * бегунок лежит отдельно, `システム／スクロールボタン／通常|オン|ダウン`.
		 * Суффиксы состояний у переопределения БЕЗ `／バー`.
		 * Счёт по раскладкам: 13 горизонтальных полос (10× `スクロールボタン`,
		 * 3× `スクロールボタン小`) и 6 вертикальных (BACK LOG, BACK SCENE, вьювер
		 * поз, три `ScrollBarUnit*`). `上書き前/次/背景ＣＧ名` пусты во всех —
		 * рельс и стрелки по-прежнему берутся от базы.
		 */
		static const char *const knob_sfx[4] = { NULL, "／通常", "／オン", "／ダウン" };
		struct string *bar_cg = act_str(ti, "上書きバーＣＧ名");
		struct string *knob_cg = cg;
		const char *const *sfx = bar_sfx;
		if (bar_cg && bar_cg->size) {
			knob_cg = bar_cg;
			sfx = knob_sfx;
		}
		if (knob_cg && knob_cg->size) {
			for (int s = 1; s <= 3; s++) {
				struct string *full = act_cg_suffix(knob_cg, sfx[s]);
				PE_SetPartsCG(no, full, 0, s);
				free_string(full);
			}
		} else if (flat && flat->size) {
			PE_SetPartsFlat(no, flat, 1);
		}
		int base_x = act_list_int(node, "座標", 0, 0);
		int base_y = act_list_int(node, "座標", 1, 0);
		PE_InitPartsHScrollbar(no, base_x, base_y,
			act_int(ti, "長さ", 0), act_int(ti, "幅", 0),
			act_int(ti, "全体スクロール量", 0), act_int(ti, "表示量", 1),
			act_float(ti, "スクロールレート", 0.0f));
		PE_SetClickable(no, true);
	} else if (ptype == 4 && ti) {
		/*
		 * Текстовое поле ввода (パーツタイプ=4). Всё описание лежит прямо здесь,
		 * гадать не нужно — образец из `セーブ確認.pactex` (поле COMMENT диалога
		 * «сохранить сюда?» у Tsumamigui 3): サイズ 186×22, шрифт 16 белый с
		 * обводкой 1.25, 背景色 (61,149,220), 最大文字数 10.
		 */
		struct textbox_state *t = textbox_get(no, false, true);
		if (t) {
			t->width  = act_list_int(ti, "サイズ", 0, 0);
			t->height = act_list_int(ti, "サイズ", 1, 0);
			t->font_type   = act_int(ti, "フォントタイプ", 0);
			t->font_size   = act_int(ti, "フォントサイズ", 16);
			t->r = act_list_int(ti, "フォント色", 0, 255);
			t->g = act_list_int(ti, "フォント色", 1, 255);
			t->b = act_list_int(ti, "フォント色", 2, 255);
			t->bold_weight = act_float(ti, "フォント太さ", 0.0f);
			t->edge_weight = act_float(ti, "フォント縁取り", 0.0f);
			t->edge_r = act_list_int(ti, "フォント縁取り色", 0, 0);
			t->edge_g = act_list_int(ti, "フォント縁取り色", 1, 0);
			t->edge_b = act_list_int(ti, "フォント縁取り色", 2, 0);
			t->read_only  = act_int(ti, "読み取り専用", 0) != 0;
			t->max_length = act_int(ti, "最大文字数", 0);
			t->bg_r = act_list_int(ti, "背景色", 0, 0);
			t->bg_g = act_list_int(ti, "背景色", 1, 0);
			t->bg_b = act_list_int(ti, "背景色", 2, 0);
			t->ro_bg_r = act_list_int(ti, "読み取り専用背景色", 0, 150);
			t->ro_bg_g = act_list_int(ti, "読み取り専用背景色", 1, 150);
			t->ro_bg_b = act_list_int(ti, "読み取り専用背景色", 2, 150);
			t->frame_r = act_list_int(ti, "枠色", 0, 0);
			t->frame_g = act_list_int(ti, "枠色", 1, 0);
			t->frame_b = act_list_int(ti, "枠色", 2, 0);
			t->sel_r = act_list_int(ti, "選択色", 0, 200);
			t->sel_g = act_list_int(ti, "選択色", 1, 200);
			t->sel_b = act_list_int(ti, "選択色", 2, 200);
			t->char_space = act_int(ti, "文字間隔", 0);
			struct string *cg = act_str(ti, "ＣＧ名");
			if (cg && cg->size)
				textbox_set_string(&t->cg_name, cg);
			struct string *txt = act_str(ti, "テキスト");
			if (txt)
				textbox_set_string(&t->text, txt);
			t->caret = textbox_nr_chars(t);
			textbox_render(t);
		}
		PE_SetClickable(no, true);
	} else if (ptype == 2 && ti) {
		// vertical scrollbar (パーツタイプ=2). Geometry from 種類別情報: 長さ=track
		// length (Y), 幅=track width (X), 前/次サイズ=∧/∨ arrow-button heights,
		// 全体スクロール量/表示量=scroll amounts (also drive how many log lines the
		// BACK LOG viewer instantiates: SetLineIndex loops i in 0..ViewSize).
		// ＣＧ名 is a *base* name; the draggable knob is stored per-state as
		// "<base>／バー／通常|オン|ダウン" (like the horizontal slider). The groove
		// "<base>／背景" and arrow buttons "<base>／上ボタン|下ボタン" are Cut 2.
		static const char *const vbar_sfx[4] = { NULL, "／バー／通常", "／バー／オン", "／バー／ダウン" };
		struct string *cg = act_str(ti, "ＣＧ名");
		struct string *flat = act_str(ti, "フラット名");
		int base_x = act_list_int(node, "座標", 0, 0);
		int base_y = act_list_int(node, "座標", 1, 0);
		int base_z = act_list_int(node, "座標", 2, 0);
		int length = act_int(ti, "長さ", 0);
		int width  = act_int(ti, "幅", 0);
		int up_sz  = act_int(ti, "前サイズ", 0);
		int down_sz = act_int(ti, "次サイズ", 0);
		// The knob is the part's own CG (per-state ／バー) — либо ГОТОВОЕ имя из
		// `上書きバーＣＧ名` с суффиксами состояний без `／バー` (см. подробный
		// разбор у горизонтальной полосы: у Dohna база — «заглушка» с одним
		// только `／背景`, а бегунок лежит отдельной картинкой).
		// ★Переопределение касается ТОЛЬКО бегунка: рельс, стрелки и переключение
		// 通常/無効 ниже по-прежнему считаются от базового `cg` (иначе рельс уехал бы
		// в `システム／スクロールボタン縦／背景`, которого в архиве нет).
		static const char *const vknob_sfx[4] = { NULL, "／通常", "／オン", "／ダウン" };
		struct string *vbar_cg = act_str(ti, "上書きバーＣＧ名");
		struct string *vknob_cg = cg;
		const char *const *vsfx = vbar_sfx;
		if (vbar_cg && vbar_cg->size) {
			vknob_cg = vbar_cg;
			vsfx = vknob_sfx;
		}
		if (vknob_cg && vknob_cg->size) {
			for (int s = 1; s <= 3; s++) {
				struct string *full = act_cg_suffix(vknob_cg, vsfx[s]);
				PE_SetPartsCG(no, full, 0, s);
				free_string(full);
			}
		} else if (flat && flat->size) {
			PE_SetPartsFlat(no, flat, 1);
		}
		int total = act_int(ti, "全体スクロール量", 0);
		int view  = act_int(ti, "表示量", 1);
		float rate = act_float(ti, "スクロールレート", 0.0f);
		// Шаг кнопок-стрелок: без него 前へ/次へ не листают (см. sb_move_by_button).
		PE_SetVScrollbarMoveSizeByButton(no, act_int(ti, "ボタンクリック移動量", 1));
		PartsEngine_SetVScrollbarTotalSize(no, total);
		PartsEngine_SetVScrollbarViewSize(no, view);
		PartsEngine_SetVScrollbarScrollRate(no, rate);
		PE_InitPartsVScrollbar(no, base_x, base_y, length, width, up_sz, down_sz, total, view, rate);
		// Cut 2 — static sibling parts for the rail background and ∧/∨ arrow buttons.
		// The knob (part `no`) renders above the rail; the arrow buttons sit at the
		// top/bottom ends of the track (前サイズ/次サイズ tall). They are decorative
		// (not registered in a->parts) — same pattern as the checkbox text label.
		int up_no = -1, down_no = -1;
		if (cg && cg->size) {
			// Layout down the track: ∧ button (up_sz tall) at the top, the rail
			// background (length−up−down tall) in the middle, ∨ button (down_sz)
			// at the bottom. CG sizes confirm this: 60×60 / 60×300 / 60×60 = 420.
			const struct { const char *sfx; int x, y, z; } deco[3] = {
				{ "／上ボタン／通常", base_x, base_y,                     base_z     },
				{ "／背景",           base_x, base_y + up_sz,             base_z - 1 },
				{ "／下ボタン／通常", base_x, base_y + length - down_sz,  base_z     },
			};
			for (int d = 0; d < 3; d++) {
				struct string *full = act_cg_suffix(cg, deco[d].sfx);
				int dno = ++pe_act_part_seq;
				pe_act_add_helper(a, dno);
				bool ok = PE_SetPartsCG(dno, full, 0, 1);
				free_string(full);
				if (ok) {
					PE_SetPos(dno, deco[d].x, deco[d].y);
					PE_SetZ(dno, deco[d].z);
					if (parent_no >= 0)
						PE_SetParentPartsNumber(dno, parent_no);
					PE_SetShow(dno, 1);
					if (d == 0) up_no = dno;
					else if (d == 2) down_no = dno;
				}
			}
		}
		// Record arrow parts + base CG so the scrollbar can switch enabled/disabled
		// (通常/無効 arrows + knob shown/hidden) as the log grows/shrinks.
		{
			struct pe_vscrollbar *sb = pe_vscrollbar_get(no, true);
			sb->up_no = up_no;
			sb->down_no = down_no;
			if (sb->cg_base)
				free_string(sb->cg_base);
			sb->cg_base = (cg && cg->size) ? string_ref(cg) : NULL;
			pe_vscrollbar_apply_enabled(sb);
		}
		if (getenv("XSYS4_PT_TRACE"))
			NOTICE("PT vscrollbar no=%d base=(%d,%d) len=%d w=%d up=%d down=%d total=%d view=%d rate=%.3f",
			       no, base_x, base_y, length, width, up_sz, down_sz, total, view, rate);
		PE_SetClickable(no, true);
	} else if (ptype == 8 && ti) {
		/*
		 * `レイアウトボックス` (パーツタイプ=8) — контейнер, сам раскладывающий детей.
		 * Загрузчик его не настраивал вовсе: тип раскладки, выравнивание, перенос и
		 * поля оставались нулевыми, и `parts_do_layout` укладывал кнопки слева
		 * направо от точки привязки независимо от того, что написано в раскладке.
		 *
		 * Симптом — системные кнопки ADV (`AdvSystemButton.x`): у `LayoutRight`
		 * (`座標` 1263,653, `原点座標モード` 3 — привязка к правому краю) кнопки
		 * должны расти ВЛЕВО от 1263, а уезжали вправо за экран: `ButtonAuto` в
		 * x=1315, `ButtonBackLog` в x=1523 при ширине экрана 1280 — то есть AUTO и
		 * History просто не попадали в кадр. У `LayoutLeft` иконки, наоборот, стояли
		 * на 104 px правее оригинальных: места ушедших в `LayoutHide` соседей
		 * (`BackScene`, `Config`) оставались зарезервированными.
		 *
		 * Поля ложатся на HLL-сеттеры один в один, ими же потом пользуется игра.
		 * `折り返しサイズ` — вещественное, но `SetLayoutBoxReturn` принимает int:
		 * во всех раскладках Dohna значения целые (200.0 и т.п.).
		 */
		PE_SetLayoutBoxLayoutType(no, act_int(ti, "レイアウトタイプ", 0));
		PE_SetLayoutBoxAlign(no, act_int(ti, "配置", 0));
		PE_SetLayoutBoxReturn(no, act_int(ti, "折り返し許可", 0) != 0,
			(int)act_float(ti, "折り返しサイズ", 0.0f));
		PE_set_layoutbox_padding(no,
			act_list_int(ti, "パディング", 0, 0),
			act_list_int(ti, "パディング", 1, 0),
			act_list_int(ti, "パディング", 2, 0),
			act_list_int(ti, "パディング", 3, 0));
		if (getenv("XSYS4_PT_TRACE"))
			NOTICE("PT layoutbox no=%d type=%d align=%d wrap=%d/%d", no,
			       act_int(ti, "レイアウトタイプ", 0), act_int(ti, "配置", 0),
			       act_int(ti, "折り返し許可", 0), (int)act_float(ti, "折り返しサイズ", 0.0f));
	} else if (ptype == 1 && ti) {
		// checkbox (パーツタイプ=1): ＣＧ名 is a base; the box has checked/unchecked
		// variants. テキスト is the label drawn to the right of the box.
		struct string *cg = act_str(ti, "ＣＧ名");
		bool checked = act_int(ti, "チェック状態", 0) != 0;
		if (cg && cg->size) {
			PE_InitPartsCheckBox(no, cg, checked);
		} else {
			// No CG: a colour-swatch button — render a solid fill of its size,
			// tinted by the checkbox colour the game sets (SetCheckBoxColor).
			int sw = act_list_int(ti, "サイズ", 0, 0);
			int sh = act_list_int(ti, "サイズ", 1, 0);
			if (sw > 0 && sh > 0)
				PE_SetPartsColorFill(no, sw, sh);
		}
		PE_SetClickable(no, true);
		struct string *txt = act_str(ti, "テキスト");
		if (txt && txt->size) {
			int box_w = PE_GetPartsWidth(no, 1);
			if (box_w <= 0) box_w = act_list_int(ti, "サイズ", 0, 0);
			if (box_w <= 0) box_w = 32;
			int box_h = PE_GetPartsHeight(no, 1);
			int fsize = act_int(ti, "フォントサイズ", 16);
			int tno = ++pe_act_part_seq;
			pe_act_add_helper(a, tno);
			PE_SetText(tno, txt, 1);
			PE_SetFont(tno, act_int(ti, "フォントタイプ", 0), fsize,
				act_list_int(ti, "フォント色", 0, 255),
				act_list_int(ti, "フォント色", 1, 255),
				act_list_int(ti, "フォント色", 2, 255),
				act_float(ti, "フォント太さ", 0.0f),
				act_list_int(ti, "フォント縁取り色", 0, 0),
				act_list_int(ti, "フォント縁取り色", 1, 0),
				act_list_int(ti, "フォント縁取り色", 2, 0),
				act_float(ti, "フォント縁取り", 0.0f), 1);
			/*
			 * ПОДПИСЬ — РЕБЁНОК САМОГО БОКСА, координаты локальные. Прежде она
			 * висела на том же родителе, что и бокс, с абсолютной позицией из
			 * раскладки: под свободной раскладкой это совпадало, но у
			 * контейнера с `レイアウトタイプ` подпись стала ОТДЕЛЬНЫМ элементом
			 * ряда — вертикальная раскладка ставила её своей строкой, и пара
			 * «бокс + подпись» разъезжалась (вкладка 入力 в CONFIG: шаг 90 и
			 * подписи не на своих местах вместо ряда с шагом 55, сверено с
			 * `screenshots/orig-haharanman-config-input.png`). Ребёнком она
			 * едет вместе с боксом, куда бы тот ни встал.
			 */
			int ty = box_h > fsize ? (box_h - fsize) / 2 : 0;
			PE_SetPos(tno, box_w + 4, ty);
			PE_SetParentPartsNumber(tno, no);
			PE_SetShow(tno, 1);
		}
	} else if (ti && act_parts_type_v14(ti) == 14) {
		/*
		 * Панель (`パネル`, тип компонента v14 = 14) — прямоугольная заливка
		 * цветом с альфа-градиентом по краям (src/parts/panel.c). Классического
		 * аналога нет, поэтому act_parts_type отдаёт -1, и раньше панель уходила
		 * в общую CG-ветку: `CActivityWrap@GetPanel` (сравнение типа с 14)
		 * получал null, и `StripDialogBase@SetHeight` падал на пустом
		 * интерфейсе. Узел кладём теми же сеттерами, что зовёт игра.
		 */
		PE_SetPanelSize(no, act_list_int(ti, "サイズ", 0, 0),
			act_list_int(ti, "サイズ", 1, 0));
		PE_SetPanelColor(no, act_list_int(ti, "色", 0, 255),
			act_list_int(ti, "色", 1, 255),
			act_list_int(ti, "色", 2, 255),
			act_list_int(ti, "色", 3, 255));
		// Порядок четвёрки `アルファグラデーション` взят из порядка ОБЪЯВЛЕНИЯ пары
		// сеттеров/геттеров в библиотеке (fn639-646: Top, Bottom, Left, Right).
		// Значение движок только хранит и на непустом печатает одноразовый
		// WARNING (src/parts/panel.c) — там же оговорка, что смысл не установлен.
		PE_SetPanelAlphaGradationTop(no, act_list_int(ti, "アルファグラデーション", 0, 0));
		PE_SetPanelAlphaGradationBottom(no, act_list_int(ti, "アルファグラデーション", 1, 0));
		PE_SetPanelAlphaGradationLeft(no, act_list_int(ti, "アルファグラデーション", 2, 0));
		PE_SetPanelAlphaGradationRight(no, act_list_int(ti, "アルファグラデーション", 3, 0));
	} else if (ti && act_parts_type_v14(ti) == 17) {
		/*
		 * `ユーザコンポーネント` (тип компонента v14 = 17) — место под ОТДЕЛЬНУЮ
		 * активность (шапка, футер, полоса фазы). Классического аналога нет,
		 * поэтому act_parts_type отдаёт -1 и часть уходила в CG-ветку, где у
		 * неё нет ни одного узла состояния; игра же ищет её сравнением типа
		 * (`CActivityWrap@CompParts(имя, 17, 1)` @0x1fd54) и, не найдя,
		 * возвращала null-компонент — на нём и падал `SceneAzito@0`
		 * (`PhaseBar@Text::set`, PUSHSTRUCTPAGE = -1).
		 *
		 * Читаем узел теми же сеттерами, которыми потом пользуется игра.
		 * `データ` — плоский список «ключ, значение» (по всем 195 раскладках
		 * 270 таких частей, у 152 из них список есть, других полей нет вовсе).
		 */
		/*
		 * `エディタ上表示 = 0` у части-компонента — ОФОРМИТЕЛЬСКАЯ ЗАГОТОВКА МАКЕТА,
		 * и компонент по ней создавать не надо. В пяти раскладках-вьюхах
		 * (`SceneTutorial`, `StatusHomeView`, `EventHintView`, `ItemOperation`,
		 * `SendOperation`) в макет вставлены шапка и подвал, чтобы дизайнер видел
		 * экран целиком, хотя настоящие приходят из главной сцены. Таких девять:
		 * компоненты `Fotter` (опечатка авторов в `Footer::GetComponentName`) и
		 * `Header` при частях с редакторскими именами и без списка `データ`.
		 *
		 * Симптом: на экране обучения поверх полосатой подложки лежал ВТОРОЙ,
		 * полноразмерный футер — белая полоса подсказки во всю ширину и розовые
		 * плашки-пустышки (замер: часть `90000429` в слое сцены обучения, масштаб
		 * 1.0, поверх уменьшенного до 0.85 футера `90000372` домашней сцены). У
		 * оригинала там штриховка и точки страниц: 288 кадров записи оригинала, ни
		 * одного с этой подложкой.
		 *
		 * ★Скрыть часть-ХОЗЯИНА мало: активность компонента всё равно создаётся, и
		 * её детали (тени и подложки кнопок) остаются на экране серой рамкой вокруг
		 * `Prev`/`Next` — проверено кадром. Поэтому компонент не заводим вовсе.
		 *
		 * ★Флаг стоит и у 125 других частей, но там он значит иное, и прятать их
		 * нельзя — проверено кадрами: `構築パーツ_000` (фон страниц CONFIG) уносит фон
		 * конфига, `ＣＧパーツ_000` в `AdvMessageWindow_main`/`_mainB` (CG
		 * `メッセージウィンドウ／Ａ案`) — подложку окна реплик вместе с текстом; в прологе
		 * текст при этом оставался, потому что там другое окно (`_plot`), где такой
		 * части нет. Поэтому правило ограничено типом «пользовательский компонент».
		 */
		if (!act_int(node, "エディタ上表示", 1)) {
			if (getenv("XSYS4_ACT_TRACE") || getenv("XSYS4_PARTS_TRACE"))
				NOTICE("ACT: заготовка макета '%s' (%s) не создаётся",
				       display_sjis0(node->name ? node->name->text : ""),
				       display_sjis0(act_str(ti, "ユーザコンポーネント名") ?
				                     act_str(ti, "ユーザコンポーネント名")->text : ""));
			return no;
		}
		struct string *uc_name = act_str(ti, "ユーザコンポーネント名");
		PE_SetUserComponentName(no, uc_name);
		struct ex_tree *data = act_child(ti, "データ");
		if (data && data->is_leaf && data->leaf.value.type == EX_LIST) {
			struct ex_list *l = data->leaf.value.list;
			if (l->nr_items % 2) {
				static bool warned = false;
				if (!warned) {
					warned = true;
					WARNING("ユーザコンポーネント: нечётный список データ (%u) — "
						"пары «ключ, значение» не складываются", l->nr_items);
				}
			}
			for (unsigned i = 0; i + 1 < l->nr_items; i += 2) {
				struct ex_value *k = &l->items[i].value;
				struct ex_value *v = &l->items[i + 1].value;
				if (k->type != EX_STRING || v->type != EX_STRING)
					continue;
				PE_SetUserComponentData(no, k->s, v->s);
			}
		}
	} else if (ti && act_parts_type_v14(ti) == 10) {
		/*
		 * Окно реплик ADV (`メッセージウィンドウ`, тип компонента v14 = 10) — тип,
		 * у которого классического аналога нет вовсе, поэтому act_parts_type
		 * отдаёт -1 и часть уходила в общую CG-ветку, где у неё нет ни одного
		 * знакомого узла состояния: окно оставалось пустым, а первое же
		 * обращение игры (SetMessageWindowActive) роняло движок в REPL.
		 *
		 * Узел `種類別情報` один-в-один ложится на HLL-функции, поэтому читаем
		 * его теми же сеттерами, которыми потом пользуется игра — никакой
		 * второй дороги к тем же полям.
		 */
		int mw_text_no = ++pe_act_part_seq;
		int mw_mark_no = ++pe_act_part_seq;
		pe_act_add_helper(a, mw_text_no);
		pe_act_add_helper(a, mw_mark_no);
		PE_CreateMessageWindow(no, mw_text_no, mw_mark_no);
		PE_SetMessageWindowInactiveMultipleColor(no,
			act_list_int(ti, "非アクティブ時の乗算カラー", 0, 255),
			act_list_int(ti, "非アクティブ時の乗算カラー", 1, 255),
			act_list_int(ti, "非アクティブ時の乗算カラー", 2, 255));
		struct string *mw_cg = act_str(ti, "ＣＧ名");
		if (mw_cg)
			PE_SetMessageWindowCGName(no, mw_cg);
		struct string *mw_flat = act_str(ti, "フラット名");
		if (mw_flat)
			PE_SetMessageWindowFlatName(no, mw_flat);
		// `フラット表示待ちフレーム数` в раскладке НЕТ (проверено по всем пяти узлам
		// メッセージウィンドウ во всех 195 .pactex) — это чисто рантаймовое свойство,
		// игра задаёт его сама через SetMessageWindowFlatShowWaitFrameNumber.
		PE_SetMessageWindowTextFont(no,
			act_int(ti, "フォントタイプ", 0),
			act_int(ti, "フォントサイズ", 16),
			act_list_int(ti, "フォント色", 0, 255),
			act_list_int(ti, "フォント色", 1, 255),
			act_list_int(ti, "フォント色", 2, 255),
			act_float(ti, "フォント太さ", 0.0f),
			act_list_int(ti, "フォント縁取り色", 0, 0),
			act_list_int(ti, "フォント縁取り色", 1, 0),
			act_list_int(ti, "フォント縁取り色", 2, 0),
			act_float(ti, "フォント縁取り", 0.0f));
		PE_SetMessageWindowTextSpace(no, act_int(ti, "文字間隔", 0),
			act_int(ti, "行間隔", 0));
		PE_SetMessageWindowTextArea(no,
			act_list_int(ti, "テキストエリア", 0, 0),
			act_list_int(ti, "テキストエリア", 1, 0),
			act_list_int(ti, "テキストエリア", 2, 0),
			act_list_int(ti, "テキストエリア", 3, 0));
		PE_SetMessageWindowTextOriginPosMode(no, act_int(ti, "テキスト位置", 1));
		PE_SetMessageWindowTextSpeed(no, act_int(ti, "字速度", 0));
		struct ex_tree *ruby = act_child(ti, "ルビ");
		if (ruby && !ruby->is_leaf) {
			PE_SetMessageWindowRubyFont(no,
				act_int(ruby, "フォントタイプ", 0),
				act_int(ruby, "フォントサイズ", 10),
				act_list_int(ruby, "フォント色", 0, 255),
				act_list_int(ruby, "フォント色", 1, 255),
				act_list_int(ruby, "フォント色", 2, 255),
				act_float(ruby, "フォント太さ", 0.0f),
				act_list_int(ruby, "フォント縁取り色", 0, 0),
				act_list_int(ruby, "フォント縁取り色", 1, 0),
				act_list_int(ruby, "フォント縁取り色", 2, 0),
				act_float(ruby, "フォント縁取り", 0.0f));
			PE_SetMessageWindowRubyCharSpace(no, act_int(ruby, "文字間隔", 0));
			PE_SetMessageWindowRubyLineSpace(no, act_int(ruby, "行間隔", 0));
		}
		PE_SetEnableMessageWindowTextWrapping(no, act_int(ti, "折り返し", 0));
		struct ex_tree *mark = act_child(ti, "キー待ちマーク");
		if (mark && !mark->is_leaf) {
			PE_SetKeyWaitPos(no, act_list_int(mark, "座標", 0, 0),
				act_list_int(mark, "座標", 1, 0),
				act_list_int(mark, "座標", 2, 0));
			struct string *mcg = act_str(mark, "ＣＧ名");
			if (mcg)
				PE_SetKeyWaitCGName(no, mcg,
					act_int(mark, "ループＣＧ開始番号", 0),
					act_int(mark, "ループＣＧ枚数", 0),
					act_int(mark, "ループＣＧ切り替え時間", 0));
			struct string *mflat = act_str(mark, "フラット名");
			if (mflat)
				PE_SetKeyWaitFlatName(no, mflat);
		}
		/*
		 * `テキスト` из раскладки — ОБРАЗЕЦ реплики. Его не подставляли, считая
		 * заготовкой редактора, и в превью страницы `Text Area UI` рамка окна
		 * оставалась пустой, тогда как у оригинала внутри стоят три строки
		 * («Hey there, sexy! I'm some example text…»).
		 *
		 * Что это именно экранный текст, видно по данным: части типа
		 * `メッセージウィンドウ` во всей игре пять, у всех пяти поле непустое, и
		 * ровно у конфиговой оно ПЕРЕВЕДЕНО на английский — у четырёх ADV-окон там
		 * осталась японская рыба (`これはサンプルメッセージです`,
		 * `あいうえお…`, `透過率／明るさを確認してください`). Заготовку редактора
		 * локализовывать бы не стали.
		 *
		 * ADV-окна рыбу не показывают: до первой реплики окно не выведено, а с
		 * первой же игра перезаписывает текст своим (`SetMessageWindowText`).
		 */
		struct string *mw_text = act_str(ti, "テキスト");
		if (mw_text && mw_text->size > 0)
			PE_SetMessageWindowText(no, mw_text, 0, NULL, 0, 0);
		PE_SetMessageWindowActive(no, act_int(ti, "アクティブ", 0));
	} else if (ti) {
		// CG part: per-state CG names
		act_set_state_cg(no, ti, "通常状態", 1);
		act_set_state_cg(no, ti, "オンカーソル状態", 2);
		act_set_state_cg(no, ti, "キーダウン状態", 3);
	}

	PE_SetPos(no, act_list_int(node, "座標", 0, 0), act_list_int(node, "座標", 1, 0));
	PE_SetZ(no, act_list_int(node, "座標", 2, 0));
	// 原点座標モード (origin/anchor mode): which point of the part 座標 refers to, on
	// the 1-9 grid (1=top-left ... 5=middle-center ... 9=bottom-right) — the same
	// numbering as the engine's calculate_offset() and as the flat layer origin_mode.
	// Must be applied AFTER the CG/flat/panel is loaded above, because the offset is
	// derived from the state's width/height. Dropping it drew every centre-anchored
	// part with its top-left corner at the anchor point: Dohna's SceneLogo anchors the
	// AliceSoft 30th badge at its centre (原点座標モード=5), so the logo landed half a
	// texture down-right of where it belongs. Absent field -> 1, which is the engine
	// default (parts.c parts_alloc), so layouts that predate it are unaffected.
	// Counts (tool: alice ex dump over every .pactex): Dohna 1942×mode1 + 766 non-1
	// across 195 layouts; Tsumamigui 3 614×mode1 + 15 non-1 across 25 layouts.
	// То же по Tsumamigui 3: у трёх партов счётчика страниц BACK SCENE モード = 9, и без
	// этого вызова слэш «／» уезжал вправо-вниз ровно на свою ширину и высоту (замер:
	// x 965..976 против 939..954 у оригинала) — FINDINGS §5t. Вызов ЕДИНСТВЕННЫЙ: при
	// слиянии ветвей он задваивался (идемпотентно, но это мусор).
	PE_SetPartsOriginPosMode(no, act_int(node, "原点座標モード", 1));
	// 原点座標 (explicit origin coordinate) is (0,0) in all 2708 parts of all 195 Dohna
	// layouts and absent entirely in Tsumamigui 3, so its meaning is not established —
	// check the assumption instead of silently ignoring a non-zero value.
	if (act_list_int(node, "原点座標", 0, 0) || act_list_int(node, "原点座標", 1, 0)) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("act_build_part: part '%s' has non-zero 原点座標 (%d,%d) — meaning not established, ignored",
			        display_sjis0(node->name->text),
			        act_list_int(node, "原点座標", 0, 0),
			        act_list_int(node, "原点座標", 1, 0));
		}
	}
	/*
	 * `マージン` — отступы части, которые учитывает раскладка контейнера
	 * (`parts_do_layout` прибавляет их к размеру ребёнка). Поле не читалось, и
	 * кнопки в layout box стояли вплотную: у системных кнопок ADV каждая объявляет
	 * `マージン = (0, 0, 0, 8)`, то есть 8 px справа, — без них шаг выходил 44
	 * вместо 52. Порядок значений — как у `パディング` бокса (верх, низ, лево, право).
	 * По всей игре поле ненулевое у 120 с небольшим частей, у остальных нули.
	 */
	PE_SetComponentMargin(no,
		act_list_int(node, "マージン", 0, 0),
		act_list_int(node, "マージン", 1, 0),
		act_list_int(node, "マージン", 2, 0),
		act_list_int(node, "マージン", 3, 0));
	PE_SetAlpha(no, act_int(node, "アルファ", 255));
	// 拡大縮小 (scale x,y): e.g. the config sample-window system icons are 0.5.
	float sx = act_list_float(node, "拡大縮小", 0, 1.0f);
	float sy = act_list_float(node, "拡大縮小", 1, 1.0f);
	if (sx != 1.0f)
		PE_SetPartsMagX(no, sx);
	if (sy != 1.0f)
		PE_SetPartsMagY(no, sy);
	if (act_int(node, "クリック許可", 0))
		PE_SetClickable(no, true);
	/*
	 * `オンカーソル透過` — «пропускать курсор сквозь себя». Загрузчик его НЕ ЧИТАЛ,
	 * поэтому каждая часть перехватывала hover и клик у всего, что под ней, хотя
	 * данные игры говорят обратное. Стоило это целого механизма: в диалоге
	 * «сохранить сюда?» подсказка `コメント` (z=7, `オンカーソル透過 = 1`) лежит
	 * поверх рамки-кнопки `文字入力` (z=6) и съедала клик по ней — а именно её
	 * `ButtonLClickEvent` показывает поле ввода и ставит на него фокус
	 * (байткод `C_SAVE_CONFIRM@ButtonLClickEvent`, FUNC 7974). Из-за одного
	 * непрочитанного поля поле ввода комментария не появлялось вообще.
	 */
	if (act_int(node, "オンカーソル透過", 0))
		PE_SetPassCursor(no, true);
	// `マウスカーソルピクセル判定`: hit-тест по непрозрачным пикселям, а не по боксу
	// (см. parts->pixel_hittest — без него перекрывающиеся диагональные полосы меню
	// титула Dohna воровали клик друг у друга).
	if (act_int(node, "マウスカーソルピクセル判定", 0))
		PE_SetPartsPixelHitTest(no, true);
	if (parent_no >= 0)
		PE_SetParentPartsNumber(no, parent_no);
	/*
	 * `エディタ上表示 = 0` — часть-ОБРАЗЕЦ: живёт в макете ради редактора, а в игре не
	 * рисуется. Поле читалось как «показ в редакторе» и игнорировалось, отчего на
	 * экран лезло всё, что дизайнер оставил себе для наглядности. По раскладкам
	 * Dohna таких частей 134 из 2708, и список сам себя объясняет: девять заготовок
	 * шапки и подвала (`ユーザコンポーネント_000/001/002` → компоненты `Fotter`/`Header`)
	 * в пяти вьюхах, семь образцов подписи `CaptionConfig2` у заголовка CONFIG,
	 * четыре части `背景音声` на вкладке Sound, направляющие `Guide`/`ガイド`,
	 * 21 разметочная `BaseLine` в фонах битв.
	 *
	 * Симптом, с которого начали: на экране обучения поверх полосатой подложки лежал
	 * ВТОРОЙ, полноразмерный футер — белая полоса подсказки во всю ширину и розовые
	 * плашки-пустышки (замер: часть `90000429` в слое сцены обучения, масштаб 1.0,
	 * поверх уменьшенного до 0.85 футера `90000372` домашней сцены). У оригинала там
	 * штриховка и точки страниц: 288 кадров записи, ни одного с этой подложкой.
	 */
	/*
	 * `エディタ上表示 = 0` — часть-ОБРАЗЕЦ: живёт в макете ради редактора, а в игре не
	 * рисуется. Таких 134 из 2708 по раскладкам Dohna, и список сам себя объясняет:
	 * семь образцов подписи `CaptionConfig2` у заголовка CONFIG, четыре части
	 * `背景音声` на вкладке Sound, направляющие `Guide`/`ガイド`, 21 разметочная
	 * `BaseLine` в фонах битв, безымянные `ＣＧパーツ_000/001`.
	 *
	 * ★У части-КОМПОНЕНТА тот же флаг значит больше: там мало не показать хозяина —
	 * активность всё равно создаётся, и её детали остаются на экране (серая рамка
	 * вокруг `Prev`/`Next` в обучении). Такие компоненты не заводятся вовсе — см.
	 * ветку `ユーザコンポーネント` выше.
	 *
	 * ★ПОД НАБЛЮДЕНИЕМ: тот же флаг стоит у подложки окна реплик
	 * (`ＣＧパーツ_000` в `AdvMessageWindow_main`/`_mainB`, CG `メッセージウィンドウ／Ａ案`)
	 * и у фона страниц CONFIG (`構築パーツ_000`). Если окно реплик или фон пропадут —
	 * правило придётся сузить обратно по типу части.
	 */
	PE_SetShow(no, act_int(node, "表示", 1) && act_int(node, "エディタ上表示", 1));

	// recurse into child parts. A パーツタイプ=18 "display range" child acts as a
	// clip viewport: subsequent siblings (the preview scene/message-window/icons)
	// are clipped to its rect via the alpha-clipper. This mirrors how the engine
	// confines preview content to a box (used by config message-window previews).
	struct ex_tree *kids = act_child(node, "子パーツ");
	if (kids && !kids->is_leaf) {
		int clip_viewport_no = -1;
		for (unsigned i = 0; i < kids->nr_children; i++) {
			int cno = act_build_part(a, &kids->children[i], no);
			if (cno < 0)
				continue;
			struct ex_tree *cti = act_child(&kids->children[i], "種類別情報");
			struct ex_tree *cnorm = cti ? act_child(cti, "通常状態") : NULL;
			/*
			 * «Область отображения» (классический тип 18) работает как
			 * clip viewport: следующие соседи обрезаются по ней.
			 *
			 * ★Только для СТАРЫХ раскладок, где тип записан ЧИСЛОМ. У v14 тип
			 * записан именем, и 18 в классической нумерации — это `構築パーツ`
			 * (см. таблицу act_component_type_names, 26→18), то есть обычная
			 * собираемая поверхность. Из-за этого фон страниц CONFIG у Dohna
			 * назначался маской 29 соседним частям и переставал рисоваться сам:
			 * в дампе у него `clip=0 isclip=1`, поверхность построена
			 * (`XSYS4_CP_DUMP`: размытый `背景／ナユタ` с чёрной заливкой по
			 * альфе 180), а на экране вместо затемнённого задника просвечивал
			 * титул в полную яркость.
			 */
			struct ex_tree *cpt = cnorm ? act_child(cnorm, "パーツタイプ") : NULL;
			bool numeric_type = cpt && cpt->is_leaf && cpt->leaf.value.type == EX_INT;
			bool is_viewport = numeric_type && act_parts_type(cnorm) == 18;
			if (is_viewport)
				clip_viewport_no = cno;
			else if (clip_viewport_no >= 0)
				PE_SetPartsAlphaClipperPartsNumber(cno, clip_viewport_no);
		}
	}
	return no;
}

static bool PE_ReadActivityFile(struct string *activity_name, struct string *file_name)
{
	struct pe_activity *a = pe_act_find(activity_name);
	if (!a) { PE_CreateActivity(activity_name); a = pe_act_find(activity_name); }

	struct archive_data *dfile = asset_get_by_name(ASSET_PACT, file_name->text, NULL);
	if (!dfile) {
		WARNING("ReadActivityFile: '%s' not found in Pact archive", display_sjis0(file_name->text));
		return false;
	}
	struct ex *ex = ex_read(dfile->data, dfile->size);
	archive_free_data(dfile);
	if (!ex) {
		WARNING("ReadActivityFile: failed to parse '%s'", display_sjis0(file_name->text));
		return false;
	}

	/*
	 * `終了キー` лежит на верхнем уровне файла, отдельным списком рядом с
	 * `アクティビティ`. Читаем ДО постройки частей, чтобы игра увидела клавиши,
	 * когда позовёт `BindEndType`.
	 */
	{
		char *k = utf2sjis("終了キー", strlen("終了キー"));
		struct ex_value *keys = ex_get(ex, k);
		free(k);
		if (a && keys && keys->type == EX_LIST && keys->list) {
			free(a->end_keys);
			a->nr_end_keys = 0;
			a->end_keys = xcalloc(keys->list->nr_items, sizeof(int));
			for (unsigned i = 0; i < keys->list->nr_items; i++) {
				struct ex_value *v = &keys->list->items[i].value;
				if (v->type == EX_INT)
					a->end_keys[a->nr_end_keys++] = v->i;
			}
			if (getenv("XSYS4_ACT_TRACE"))
				NOTICE("ACT '%s': завершающих клавиш %d",
				       display_sjis0(file_name->text), a->nr_end_keys);
		}
	}

	char *sjis = utf2sjis("アクティビティ", strlen("アクティビティ"));
	struct ex_value *act = ex_get(ex, sjis);
	free(sjis);
	if (act && act->type == EX_TREE && !act->tree->is_leaf && act->tree->nr_children >= 1) {
		if (getenv("XSYS4_PARTS_TRACE") || getenv("XSYS4_CTRL_TRACE") || getenv("XSYS4_ACT_TRACE")) {
			NOTICE("ReadActivityFile '%s' -> building parts (active ctrl=%d), pre-registered nr_parts=%d",
			       display_sjis0(file_name->text), PE_get_active_controller(), a->nr_parts);
			for (int i = 0; i < a->nr_parts; i++)
				NOTICE("   pre-reg[%d] name='%s' number=%d",
				       i, display_sjis0(a->parts[i].name->text), a->parts[i].number);
		}
		// Списки hover-связей — на ЭТУ постройку; вложенные вызовы (активности
		// user-компонентов) не должны видеть чужие пары.
		struct act_hover_pending *saved_pending = act_hover_pending;
		int saved_pending_nr = act_hover_nr;
		struct act_built_name *saved_built = act_built;
		int saved_built_nr = act_built_nr;
		act_hover_pending = NULL; act_hover_nr = 0;
		act_built = NULL; act_built_nr = 0;

		act_build_part(a, &act->tree->children[0], -1);  // ルートパーツ
		act_apply_pending_hover_links();

		act_hover_pending = saved_pending; act_hover_nr = saved_pending_nr;
		act_built = saved_built; act_built_nr = saved_built_nr;
		if (getenv("XSYS4_CTRL_TRACE") || getenv("XSYS4_DUMP_PARTS")) {
			NOTICE("=== parts right after ReadActivityFile build ===");
			parts_debug_dump();
		}
	}
	ex_free(ex);
	return true;
}

// Клавиши, закрывающие активность (см. комментарий у `end_keys` в struct pe_activity).
static int act_end_key_index(struct pe_activity *a, int key)
{
	for (int i = 0; i < a->nr_end_keys; i++) {
		if (a->end_keys[i] == key)
			return i;
	}
	return -1;
}

static void PE_SetActivityEndKey(struct string *act, int key)
{
	struct pe_activity *a = pe_act_find(act);
	if (!a || act_end_key_index(a, key) >= 0)
		return;
	a->end_keys = xrealloc_array(a->end_keys, a->nr_end_keys, a->nr_end_keys + 1,
				     sizeof(int));
	a->end_keys[a->nr_end_keys++] = key;
}

static void PE_EraseActivityEndKey(struct string *act, int key)
{
	struct pe_activity *a = pe_act_find(act);
	if (!a)
		return;
	int i = act_end_key_index(a, key);
	if (i < 0)
		return;
	memmove(&a->end_keys[i], &a->end_keys[i + 1], (a->nr_end_keys - i - 1) * sizeof(int));
	a->nr_end_keys--;
}

static bool PE_IsExistActivityEndKey(struct string *act, int key)
{
	struct pe_activity *a = pe_act_find(act);
	return a && act_end_key_index(a, key) >= 0;
}

static int PE_NumofActivityEndKey(struct string *act)
{
	struct pe_activity *a = pe_act_find(act);
	return a ? a->nr_end_keys : 0;
}

static int PE_GetActivityEndKey(struct string *act, int index)
{
	struct pe_activity *a = pe_act_find(act);
	if (!a || index < 0 || index >= a->nr_end_keys)
		return 0;
	return a->end_keys[index];
}

// Recompute transform matrices for the frame. Our renderer derives transforms
// from part params directly, so this is a no-op.
static void PE_UpdateMatrix(bool b) { (void)b; }

// Activity "close parts" (parts whose click closes the activity) and "intent
// data" (data passed between activities) — activity metadata not tracked yet.
// The dialogs we target are driven by their on-screen buttons, so report empty.
static void PE_AddActivityCloseParts(struct string *a, struct string *p) { (void)a; (void)p; }
static void PE_RemoveActivityCloseParts(struct string *a, struct string *p) { (void)a; (void)p; }
static void PE_RemoveAllActivityCloseParts(struct string *a) { (void)a; }
static bool PE_IsExistActivityCloseParts(struct string *a, struct string *p) { (void)a; (void)p; return false; }
static int pe_act_find_by_name(struct pe_activity *a, struct string *pn);

// Intent data is populated by ReadActivityFile from each part's 遷移方法/遷移先.
// The game never calls Set/Add here (it only queries), so those stay no-ops.
static void PE_SetActivityIntentData(struct string *a, struct string *b, struct string *c, int d) { (void)a; (void)b; (void)c; (void)d; }
static void PE_AddActivityIntentDataDestination(struct string *a, struct string *b, struct string *c) { (void)a; (void)b; (void)c; }

static struct pe_act_part *pe_act_intent_part(struct string *act, struct string *pn)
{
	struct pe_activity *a = pe_act_find(act);
	int idx = pe_act_find_by_name(a, pn);
	return idx >= 0 ? &a->parts[idx] : NULL;
}

// A part "has intent data" iff it declares a transition method (遷移方法 != 0).
// BindEndEvent gates MouseLClickEvent registration on this, so it must be true
// for buttons that close/transition the activity.
static bool PE_IsExistActivityIntentData(struct string *act, struct string *pn)
{
	struct pe_act_part *p = pe_act_intent_part(act, pn);
	bool r = p && p->intent_type != 0;
	if (getenv("XSYS4_ACT_TRACE"))
		NOTICE("ACT IsExistActivityIntentData(act='%s', name='%s') -> %d",
		       display_sjis0(act->text), display_sjis1(pn->text), r);
	return r;
}
static int PE_NumofActivityIntentDataDestination(struct string *act, struct string *pn)
{
	struct pe_act_part *p = pe_act_intent_part(act, pn);
	return p ? p->nr_intent_dests : 0;
}
static void PE_GetActivityIntentDataDestination(struct string **out, int i, struct string *act, struct string *pn)
{
	struct pe_act_part *p = pe_act_intent_part(act, pn);
	if (*out) free_string(*out);
	*out = string_ref(p && i >= 0 && i < p->nr_intent_dests ? p->intent_dests[i] : &EMPTY_STRING);
}
static int PE_GetActivityIntentDataType(struct string *act, struct string *pn)
{
	struct pe_act_part *p = pe_act_intent_part(act, pn);
	return p ? p->intent_type : 0;
}

static int pe_act_find_by_name(struct pe_activity *a, struct string *pn)
{
	for (int i = 0; a && i < a->nr_parts; i++)
		if (!strcmp(a->parts[i].name->text, pn->text))
			return i;
	return -1;
}

static bool PE_RemoveActivityParts(struct string *act, struct string *parts_name)
{
	struct pe_activity *a = pe_act_find(act);
	int idx = pe_act_find_by_name(a, parts_name);
	if (idx < 0)
		return false;
	free_string(a->parts[idx].name);
	memmove(&a->parts[idx], &a->parts[idx+1], (a->nr_parts - idx - 1) * sizeof(*a->parts));
	a->nr_parts--;
	return true;
}

static bool PE_RemoveAllActivityParts(struct string *act)
{
	struct pe_activity *a = pe_act_find(act);
	if (!a)
		return false;
	for (int i = 0; i < a->nr_parts; i++)
		free_string(a->parts[i].name);
	a->nr_parts = 0;
	return true;
}

static int PE_NumofActivityParts(struct string *act)
{
	struct pe_activity *a = pe_act_find(act);
	return a ? a->nr_parts : 0;
}

static bool PE_GetActivityParts(int index, struct string *act, struct string **name, int *number)
{
	struct pe_activity *a = pe_act_find(act);
	if (!a || index < 0 || index >= a->nr_parts)
		return false;
	if (*name) free_string(*name);
	*name = string_ref(a->parts[index].name);
	*number = a->parts[index].number;
	return true;
}

static bool PE_IsExistActivityPartsByName(struct string *act, struct string *pn)
{
	return pe_act_find_by_name(pe_act_find(act), pn) >= 0;
}

static bool PE_IsExistActivityPartsByNumber(struct string *act, int number)
{
	struct pe_activity *a = pe_act_find(act);
	for (int i = 0; a && i < a->nr_parts; i++)
		if (a->parts[i].number == number)
			return true;
	return false;
}

static int PE_GetActivityPartsNumber(struct string *act, struct string *pn)
{
	struct pe_activity *a = pe_act_find(act);
	int idx = pe_act_find_by_name(a, pn);
	int r = idx >= 0 ? a->parts[idx].number : -1;
	if (getenv("XSYS4_ACT_TRACE"))
		NOTICE("ACT GetActivityPartsNumber(act='%s', name='%s') -> %d",
		       display_sjis0(act->text), display_sjis1(pn->text), r);
	return r;
}

static void PE_GetActivityPartsName(struct string **out, struct string *act, int number)
{
	struct pe_activity *a = pe_act_find(act);
	if (*out) free_string(*out);
	*out = NULL;
	for (int i = 0; a && i < a->nr_parts; i++) {
		if (a->parts[i].number == number) {
			*out = string_ref(a->parts[i].name);
			return;
		}
	}
	*out = string_ref(&EMPTY_STRING);
}

static void PE_SetActivityEXText(struct string *act, struct string *text)
{
	struct pe_activity *a = pe_act_find(act);
	if (!a) { PE_CreateActivity(act); a = pe_act_find(act); }
	if (a->ex_text) free_string(a->ex_text);
	a->ex_text = string_ref(text);
}

static void PE_GetActivityEXText(struct string *act, struct string **out)
{
	struct pe_activity *a = pe_act_find(act);
	if (*out) free_string(*out);
	*out = string_ref(a && a->ex_text ? a->ex_text : &EMPTY_STRING);
}

static int PE_GetActivityEXID(struct string *act)
{
	struct pe_activity *a = pe_act_find(act);
	return a ? a->ex_id : -1;
}

// Проверка наличия файла активности: ищем запись в Pact-архиве так же, как это
// делает ReadActivityFile. Без этого CConfigView@Execute (и другие экраны-меню)
// получают пустое имя активности и рисуют чёрный экран.
static bool PE_IsExistActivityFile(struct string *a)
{
	if (!a || !a->text || !a->text[0])
		return false;
	struct archive_data *dfile = asset_get_by_name(ASSET_PACT, a->text, NULL);
	if (!dfile)
		return false;
	archive_free_data(dfile);
	return true;
}

// Остальные файловые операции активностей — не поддержаны: безопасные заглушки.
HLL_QUIET_UNIMPLEMENTED(true,  bool, PartsEngine, WriteActivityFile, struct string *a, struct string *b);
HLL_QUIET_UNIMPLEMENTED(false, bool, PartsEngine, SaveActivityEXText, struct string **a, struct string *b);
HLL_QUIET_UNIMPLEMENTED(false, bool, PartsEngine, LoadActivityEXText, struct string *a, struct string *b);

// --- Button-виджеты (GUI-тулкит новых игр). Внешний вид пока не реализуем;
// сеттеры — no-op, геттеры — разумные дефолты. Кнопки считаем включёнными,
// чтобы логика меню могла по ним кликать. ---
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetButtonSize, int a, int b, int c);
/*
 * Доступность кнопки. В архиве у кнопочного CG ЧЕТЫРЕ варианта:
 * `<base>／通常`, `／オン`, `／ダウン` и `／無効` — последний как раз серый. При
 * создании из раскладки грузятся только первые три, поэтому недоступные кнопки
 * оставались цветными: у Dohna на выборе фазы дня «Hunting Phase» должна быть
 * СЕРОЙ (в первый день недоступна), на домашнем экране — «Squad» и «Shop».
 * Заодно снимаем кликабельность, иначе по серой кнопке можно нажать.
 *
 * Базовое имя CG запоминаем при разборе раскладки (act_build_part): в рантайме
 * его взять уже негде — часть хранит только имя загруженного файла с суффиксом.
 * Приём тот же, что у скроллбара (`pe_vscrollbar.cg_base`).
 */
static struct pe_button_state *pe_buttons;
static int nr_pe_buttons;

static struct pe_button_state *pe_button_get(int parts_no, bool create)
{
	for (int i = 0; i < nr_pe_buttons; i++)
		if (pe_buttons[i].parts_no == parts_no)
			return &pe_buttons[i];
	if (!create)
		return NULL;
	pe_buttons = xrealloc_array(pe_buttons, nr_pe_buttons, nr_pe_buttons + 1,
			sizeof(*pe_buttons));
	struct pe_button_state *b = &pe_buttons[nr_pe_buttons++];
	*b = (struct pe_button_state){ .parts_no = parts_no, .enabled = -1 };
	return b;
}

/*
 * Снос кнопки уносит и её подпись. `parts_release` намеренно НЕ рекурсивен
 * (см. оговорку там же — «Release забирает всё поддерево» ломает семантику у
 * живых игровых обёрток), а подпись из раскладки — не игровая часть, а наша
 * служебная, и без хозяина она сирота.
 *
 * Так на экране CONFIG у Dohna оставалась надпись `デバッグ` в правом верхнем
 * углу: кнопку отладки (`デバッグボタン`, 1148,12, у неё ни ＣＧ名, ни フラット名)
 * релизная игра сносит сама, и в дампе частей её уже нет — а подпись, заведённая
 * сестрой, продолжала висеть. У оригинала там пусто.
 */
static void PartsEngine_ReleaseParts(int parts_no)
{
	struct pe_button_state *b = pe_button_get(parts_no, false);
	if (b && b->text_no) {
		PE_ReleaseParts(b->text_no);
		b->text_no = 0;
	}
	PE_ReleaseParts(parts_no);
}

static void pe_button_remember_cg(int parts_no, struct string *cg_base)
{
	if (!cg_base || !cg_base->size)
		return;
	struct pe_button_state *b = pe_button_get(parts_no, true);
	if (b->cg_base)
		free_string(b->cg_base);
	b->cg_base = string_ref(cg_base);
}

static void PartsEngine_SetButtonEnable(int parts_no, bool enable)
{
	struct pe_button_state *b = pe_button_get(parts_no, true);
	int en = enable ? 1 : 0;
	if (b->enabled == en)
		return;
	b->enabled = en;
	PE_SetClickable(parts_no, enable);
	if (!b->cg_base)
		return;
	// Серый вид — отдельный файл `／無効`; включённой кнопке возвращаем `／通常`.
	struct string *f = act_cg_suffix(b->cg_base, enable ? "／通常" : "／無効");
	PE_SetPartsCG(parts_no, f, 0, 1);
	free_string(f);
}

static bool PartsEngine_IsButtonEnable(int parts_no)
{
	struct pe_button_state *b = pe_button_get(parts_no, false);
	return !b || b->enabled != 0;
}
// Чекбоксы — так же: «доступность» не рисуем, но считаем включёнными. Без этих
// двух System Menu в ADV (SceneAdvButtonMenu@UpdateCheckable) валит движок.
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetCheckBoxEnable, int a, bool b);
HLL_QUIET_UNIMPLEMENTED(true, bool, PartsEngine, IsCheckBoxEnable, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetButtonColor, int a, int b, int c, int d);
HLL_QUIET_UNIMPLEMENTED(255, int, PartsEngine, GetButtonR, int a);
HLL_QUIET_UNIMPLEMENTED(255, int, PartsEngine, GetButtonG, int a);
HLL_QUIET_UNIMPLEMENTED(255, int, PartsEngine, GetButtonB, int a);
/*
 * Шрифт подписи кнопки. Игра задаёт его сама (`Ｐ＿ボタン＿フォント設定`), и пока
 * функция была заглушкой, подпись рисовалась шрифтом ИЗ РАСКЛАДКИ: в CONFIG
 * «Reset» выходил заметно крупнее и жирнее, чем у оригинала. Порядок аргументов —
 * как у остальных `*FontProperty`: тип, размер, RGB, жирность, RGB окантовки,
 * ширина окантовки.
 */
static void PartsEngine_SetButtonFontProperty(int parts_no, int type, int size,
		int r, int g, int b, float bold_weight,
		int edge_r, int edge_g, int edge_b, float edge_weight)
{
	struct pe_button_state *btn = pe_button_get(parts_no, false);
	if (!btn || !btn->text_no)
		return;
	PE_SetFont(btn->text_no, type, size, r, g, b, bold_weight,
			edge_r, edge_g, edge_b, edge_weight, 1);
	btn->font_size = size;
	// Кегль изменился — надпись надо перецентрировать по кнопке.
	int tw = PE_GetPartsWidth(btn->text_no, 1);
	int tx = btn->text_x + (btn->box_w > tw ? (btn->box_w - tw) / 2 : 0);
	int ty = btn->text_y + (btn->box_h > size ? (btn->box_h - size) / 2 : 0);
	PE_SetPos(btn->text_no, tx, ty);
}
// ВАЖНО: ref-output геттеры ОБЯЗАНЫ заполнять выходы — иначе игра читает
// неинициализированный локал (мусор INT_MIN) и, напр., сохраняет как размер
// шрифта → текст не рисуется. Пишем разумные дефолты (размер шрифта 16).
static void PE_GetButtonFontProperty(int a, int *type, int *size, int *r, int *g, int *b,
	float *weight, int *sr, int *sg, int *sb, float *sw) {
	(void)a;
	if (type) *type = 0;
	if (size) *size = 16;
	if (r) *r = 255; if (g) *g = 255; if (b) *b = 255;
	if (weight) *weight = 1.0f;
	if (sr) *sr = 0; if (sg) *sg = 0; if (sb) *sb = 0;
	if (sw) *sw = 0.0f;
}
static void PE_str_out(struct string **out) {
	if (*out) free_string(*out);
	*out = string_ref(&EMPTY_STRING);
}
static void PE_GetButtonCGName(int a, struct string **b) { (void)a; PE_str_out(b); }
static void PE_GetButtonFlatName(int a, struct string **b) { (void)a; PE_str_out(b); }
static void PE_GetButtonText(int a, struct string **b) { (void)a; PE_str_out(b); }

// GetPartsTextFontProperty: геттер свойств шрифта текст-парта (форма как у
// GetButtonFontProperty + хвостовой State). Полноценного хранилища свойств
// текст-парта в движке нет — возвращаем разумные дефолты, чтобы вызывающий код
// (диалоги/бэклог) не читал мусор из ref-out и не падал.
static void PE_GetPartsTextFontProperty(int a, int *type, int *size, int *r, int *g, int *b,
	float *weight, int *er, int *eg, int *eb, float *ew, int state) {
	// Sensible defaults first (ref-out getters MUST fill outputs, else callers read
	// garbage — e.g. a bad size leaves text unrendered).
	if (type) *type = 0;
	if (size) *size = 16;
	if (r) *r = 255; if (g) *g = 255; if (b) *b = 255;
	if (weight) *weight = 0.0f;
	if (er) *er = 0; if (eg) *eg = 0; if (eb) *eb = 0;
	if (ew) *ew = 0.0f;
	// Tsumamigui 3 reads THIS on the message-window text parts to pick the BACK LOG
	// font size, then builds the log construction ops at the returned size. Return
	// the part's ACTUAL font SIZE so the log matches the message window (30) instead
	// of a hardcoded 16.
	//
	// ТИП ТОЖЕ ОТДАЁМ НАСТОЯЩИЙ (FINDINGS §5af). Прежде здесь стояло 0 с
	// объяснением «ts.face — внутренний id движка (256), а не HLL-тип, и от него
	// лог выходит нерисуемым». Посылка неверна: 256 и ЕСТЬ HLL-тип — игра сама его
	// так вычисляет (`parts::detail::GetFontNumber` возвращает `256 + индекс` в
	// `Ｅ＿外部フォント名`). Симптом «пустой лог» шёл от другой причины — выбор face
	// FNL был сломан (§5f/§5ab) и на запрошенный кегль приходил вырожденный глиф.
	// А с нулём лог просил ВСТРОЕННЫЙ системный шрифт: пока лица 0/1 подменялись на
	// FNL, это случайно совпадало, но после возврата их системному шрифту текст лога
	// поехал (кегль 30 у TTF против ~20 эффективных у коробки FNL — строки полезли
	// за панель).
	int real_size = 16, real_type = 0;
	float real_weight = 0.0f, real_edge = 0.0f;
	int real_er = 0, real_eg = 0, real_eb = 0;
	PE_GetTextFontProps(a, state, &real_type, &real_size, NULL, NULL, NULL,
			&real_weight, &real_edge, &real_er, &real_eg, &real_eb);
	if (type) *type = real_type;
	// Размер отдаём как есть: у лога Tsumamigui 3 это 30 — значение テキストパーツ
	// из バックログ.pactex, и оригинал рисует лог именно этим кеглем (сверено по
	// ink-габаритам 【 】 и cap-height, FINDINGS §5j). Ручка для перебора кеглей
	// при сверке с эталоном сохранена.
	{
		const char *s = getenv("XSYS4_LOG_SIZE");
		if (s)
			real_size = atoi(s);
		if (getenv("XSYS4_BL_TRACE"))
			NOTICE("FONTPROP part=%d state=%d -> size=%d bold=%.2f edge=%.2f",
			       a, state, real_size, real_weight, real_edge);
	}
	if (size) *size = real_size;
	// Жирность и обводка: отдаём ФАКТИЧЕСКИЕ значения парта (у Tsumamigui 3 они
	// приходят из テキストパーツ в .pactex активности: 太さ 0.5, 縁取り 2.0).
	// Это не косметика: BACK LOG считает по ним высоту юнита —
	// GetPixelHeight() = size + 2×max(ceil(太さ), ceil(縁取り)) (байткод FUNC 5558),
	// т.е. 30 + 2×2 = 34, ровно как у оригинала. Прежние выдуманные 0.0/1.0
	// (значения メッセージテキスト из .ex) давали бокс 32 и разъезжающиеся строки.
	// Ручки XSYS4_LOG_BOLD / XSYS4_LOG_EDGE оставлены для A/B.
	{
		const char *b = getenv("XSYS4_LOG_BOLD");
		const char *e = getenv("XSYS4_LOG_EDGE");
		if (weight) *weight = b ? strtof(b, NULL) : real_weight;
		if (ew) *ew = e ? strtof(e, NULL) : real_edge;
		if (er) *er = real_er; if (eg) *eg = real_eg; if (eb) *eb = real_eb;
	}
}
// Фокус ввода (клавиатурная навигация по кнопкам) в движке не реализован —
// косметика. Явные заглушки, видимые в исходнике.
/*
 * ФОКУС ВВОДА. Раньше все четыре функции были вечными заглушками (-1 / false /
 * no-op), и это ломало не «клавиатурную навигацию по кнопкам», как считалось, а
 * ТЕКСТОВЫЕ ПОЛЯ: диалог «сохранить сюда?» у Tsumamigui 3 держит поле ввода
 * (`コメント編集`, часть 90000105) погашенным и показывает его, только когда движок
 * подтвердит, что фокус на нём. С `IsFocus() == false` поле не появлялось никогда —
 * в оригинале же каретка в нём видна.
 */
static int focus_parts_no = -1;

static void PE_SetFocusPartsNumber(int parts_no)
{
	focus_parts_no = parts_no;
	/*
	 * Фокус ставит САМА ИГРА, и для текстового поля это и есть команда «начинай
	 * принимать ввод». Разобрано по байткоду `C_SAVE_CONFIRM@ButtonLClickEvent`
	 * (FUNC 7974): клик по рамке `文字入力` → `Ｐ＿ボタン＿有効設定(文字入力, 0)`
	 * → `Ｐ＿表示設定(コメント, 0)` (спрятать подсказку «■１０文字まで！！■»)
	 * → `Ｐ＿表示設定(コメント編集, 1)` (показать поле) → `Ｐ＿フォーカス設定(コメント編集)`.
	 * То есть поле оживает не от нашего клика по нему, а вот отсюда.
	 */
	struct textbox_state *t = textbox_get(parts_no, false, false);
	if (!t)
		t = textbox_get(parts_no, true, false);
	textbox_set_focus(t);
}

static int PE_GetFocusPartsNumber(void) { return focus_parts_no; }
static bool PE_IsFocus(int parts_no) { return focus_parts_no == parts_no; }

// `GetActiveParts` — «номер активной части», 0 = нет такой. У Tsumamigui 3 её зовёт
// РОВНО одно место: `activityeditor::detail::CAESelectOriginDialog@MouseLClickEvent`
// (встроенный редактор активностей AliceSoft), где `== 0` закрывает диалог. Редактор
// в обычной игре не открывается, а понятия «активной» части у нас нет вовсе (фокус —
// заглушка выше), поэтому честнее вернуть 0, чем выдумать семантику: 0 означает «нет
// активной части», и это согласовано с нашей нумерацией (номера начинаются от 9e7).
// Экспорт нужен потому, что ОТСУТСТВИЕ функции уводит движок в REPL (FINDINGS §5x).
static int PE_GetActiveParts(void) { return 0; }

// IME (переключение на полноширинный ввод в текстовом поле) — у нас ввод идёт через
// SDL без IME, переключать нечего. Настоящий no-op, а не заглушка-недоделка.
static void PE_SetOpenTextBoxIME(int parts_no, bool open) { (void)parts_no; (void)open; }


static void PE_SetComponentScrollPosXLinkNumber(int parts_no, int link)
{
	PE_set_component_scroll_pos_link(parts_no, link, false);
}

static void PE_SetComponentScrollPosYLinkNumber(int parts_no, int link)
{
	PE_set_component_scroll_pos_link(parts_no, link, true);
}

static int PE_GetComponentScrollPosXLinkNumber(int parts_no)
{
	return PE_get_component_scroll_pos_link(parts_no, false);
}

static int PE_GetComponentScrollPosYLinkNumber(int parts_no)
{
	return PE_get_component_scroll_pos_link(parts_no, true);
}

// Button parts are backed by the normal parts state machine (3 states:
// default/hovered/clicked). Setting a button's flat/CG name must actually
// load the resource into all states so the button renders — otherwise the
// title menu (built entirely from flat buttons) stays invisible (black).
/*
 * ★ИМЯ У КНОПКИ — БАЗОВОЕ, состояния лежат в архиве с СУФФИКСОМ:
 * `<base>／通常`, `<base>／オン`, `<base>／ダウン`. Ровно эту конвенцию уже применяет
 * загрузчик раскладок к `パーツタイプ=0` (см. act_build_part), а рантайм-путь клал во все
 * три состояния ОДНО И ТО ЖЕ базовое имя — CG с таким именем в архиве нет вовсе, парт
 * оставался с нулевой текстурой.
 *
 * Живой случай: у Haha Ranman системные кнопки ADV (`ＡＤＶ／システムボタン／クイックセーブ`
 * и ещё 15) создаются игрой в рантайме, а не из раскладки. На эталонном кадре оригинала
 * у нижней панели и правого пульта есть подписи Quick Save / Save / Load / Backlog /
 * Back Scene и Auto / Skip / Voice / Config, а у нас были голые плашки: части были
 * созданы (`btn=1`), но размера 0×0 — картинка не грузилась.
 *
 * Если суффиксного CG нет, откатываемся на голое имя: у части игр (и у `無効`-состояний)
 * ассет лежит ровно под базовым именем.
 */
static void PE_SetButtonCGName(int parts_no, struct string *name) {
	static const char *const sfx[4] = { NULL, "／通常", "／オン", "／ダウン" };
	for (int st = 1; st <= 3; st++) {  // default/hovered/clicked
		bool ok = false;
		if (name && name->size) {
			char *sjis = utf2sjis(sfx[st], strlen(sfx[st]));
			struct string *suf = make_string(sjis, strlen(sjis));
			struct string *full = string_concatenate(name, suf);
			ok = PE_SetPartsCG(parts_no, full, 0, st);
			free_string(full);
			free_string(suf);
			free(sjis);
		}
		if (!ok)
			PE_SetPartsCG(parts_no, name, 0, st);
	}
}
static void PE_SetButtonFlatName(int parts_no, struct string *name) {
	for (int st = 1; st <= 3; st++)  // default/hovered/clicked
		PE_SetPartsFlat(parts_no, name, st);
}

// Newer movie-parts API (CreatePartsMovie/PlayPartsMovie/IsEndPartsMovie/
// ReleasePartsMovie). Video playback for these is not implemented yet, so we
// treat the movie as instantly finished — this SKIPS the OP movie so the game
// proceeds to the title menu instead of waiting forever on a black screen.
static bool PE_CreatePartsMovie(int parts_no, struct string *filename, int a, int b, int c, int d, int e, int f) {
	(void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
	if (getenv("XSYS4_PARTS_TRACE"))
		NOTICE("PARTS CreatePartsMovie(%d, '%s') [skip]", parts_no,
		       filename ? filename->text : "(null)");
	return true;
}
static bool PE_PlayPartsMovie(int parts_no, int state) {
	(void)state;
	if (getenv("XSYS4_PARTS_TRACE"))
		NOTICE("PARTS PlayPartsMovie(%d) [skip]", parts_no);
	return true;
}
static bool PE_IsEndPartsMovie(int parts_no, int state) {
	(void)parts_no; (void)state;
	return true;  // report finished immediately so the OP is skipped
}
static bool PE_ReleasePartsMovie(int parts_no, int state) {
	(void)parts_no; (void)state;
	return true;
}
/*
 * Игра меняет подпись кнопки этой функцией — например в CONFIG кнопки «Reset»
 * (`SYS_マスターボタン` и соседние) приходят из раскладки с текстом, а игра
 * переустанавливает его при построении страницы. Пока функция была заглушкой,
 * плашки «Reset» оставались пустыми белыми прямоугольниками.
 *
 * Рисуем в ту же текстовую часть, которую загрузчик создал для подписи (шрифт и
 * цвет — из раскладки), и заново центрируем: ширина строки меняется вместе с
 * текстом. Если подписи у кнопки не было (текст в раскладке пуст), рисовать
 * нечем — тогда молча выходим, как раньше.
 */
static void PartsEngine_SetButtonText(int parts_no, struct string *text)
{
	struct pe_button_state *b = pe_button_get(parts_no, false);
	if (!b || !b->text_no || !text)
		return;
	PE_SetText(b->text_no, text, 1);
	int tw = PE_GetPartsWidth(b->text_no, 1);
	int tx = b->text_x + (b->box_w > tw ? (b->box_w - tw) / 2 : 0);
	int ty = b->text_y + (b->box_h > b->font_size ? (b->box_h - b->font_size) / 2 : 0);
	PE_SetPos(b->text_no, tx, ty);
}
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetButtonTextOriginPosMode, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetButtonTextOriginPosMode, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetButtonCharSpace, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetButtonCharSpace, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetButtonLineSpace, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetButtonLineSpace, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetCheckBoxButtonWidth, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetCheckBoxButtonHeight, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetCheckBoxButtonMode, int a, bool b);
HLL_QUIET_UNIMPLEMENTED(false, bool, PartsEngine, IsCheckBoxButtonMode, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarMoveSizeByButton, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarMoveSizeByButton, int a);
// Скроллбары (BACK LOG и пр.): вертикального скроллбара в parts пока нет
// (горизонтальный-слайдер есть — PE_*HScrollbarScrollRate). Тихие заглушки,
// чтобы UI открывался без падения в REPL; сам ползунок не отрисовывается.
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarOnCursorSoundNumber, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarClickSoundNumber, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetVScrollbarOnCursorSoundNumber, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetVScrollbarClickSoundNumber, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarSize, int a, int b, int c);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarUpHeight, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarDownHeight, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetVScrollbarUpHeight, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetVScrollbarDownHeight, int a);
// --- Вертикальный скроллбар: минимальная модель состояния (total/view/pos),
// keyed по номеру parts. Нужна бэклогу Tsumamigui 3: CBackLogView@SetLineIndex
// строит видимые строки в цикле «for i in 0..GetVScrollbarViewSize()»; при
// заглушке-0 не создавалось ни одной строки → пустой список. Отрисовку самого
// ползунка не трогаем (её и не было); модель нужна для логики прокрутки/выдачи. ---
// Declared in parts_internal.h (not included here); needed to post a Scroll
// message when the scrollbar position changes.
struct parts *parts_try_get(int parts_no);
void parts_msg_push(struct parts *parts, int type, const char *fmt, ...);
#define PE_PARTS_MSG_SCROLL 20  // == PARTS_MSG_SCROLL (game CallDelegate case 19 + 1)

static struct pe_vscrollbar *pe_vscrollbars = NULL;
static int nr_pe_vscrollbars = 0;

static struct pe_vscrollbar *pe_vscrollbar_get(int parts_no, bool create)
{
	for (int i = 0; i < nr_pe_vscrollbars; i++)
		if (pe_vscrollbars[i].parts_no == parts_no)
			return &pe_vscrollbars[i];
	if (!create)
		return NULL;
	pe_vscrollbars = xrealloc_array(pe_vscrollbars, nr_pe_vscrollbars,
			nr_pe_vscrollbars + 1, sizeof(*pe_vscrollbars));
	struct pe_vscrollbar *sb = &pe_vscrollbars[nr_pe_vscrollbars++];
	*sb = (struct pe_vscrollbar){ .parts_no = parts_no, .up_no = -1, .down_no = -1, .enabled = -1 };
	return sb;
}

// Enable/disable the scrollbar visuals by whether there is anything to scroll
// (max = total - view > 0). The original hides the knob and greys the arrows (無効)
// when the log fits the view; shows the knob and 通常 arrows when it can scroll.
static void pe_vscrollbar_apply_enabled(struct pe_vscrollbar *sb)
{
	int max = sb->total_size - sb->view_size;
	int en = max > 0 ? 1 : 0;
	if (en == sb->enabled)
		return;
	sb->enabled = en;
	PE_SetShow(sb->parts_no, en);  // knob (／バー) visible only when scrollable
	if (sb->cg_base) {
		if (sb->up_no >= 0) {
			struct string *f = act_cg_suffix(sb->cg_base,
					en ? "／上ボタン／通常" : "／上ボタン／無効");
			PE_SetPartsCG(sb->up_no, f, 0, 1);
			free_string(f);
		}
		if (sb->down_no >= 0) {
			struct string *f = act_cg_suffix(sb->cg_base,
					en ? "／下ボタン／通常" : "／下ボタン／無効");
			PE_SetPartsCG(sb->down_no, f, 0, 1);
			free_string(f);
		}
	}
}

static void PartsEngine_SetVScrollbarTotalSize(int parts_no, int size)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, true);
	sb->total_size = size;
	pe_vscrollbar_apply_enabled(sb);
	if (getenv("XSYS4_BL_TRACE"))
		NOTICE("VScrollbar[%d] TotalSize=%d", parts_no, size);
}
static void PartsEngine_SetVScrollbarViewSize(int parts_no, int size)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, true);
	sb->view_size = size;
	pe_vscrollbar_apply_enabled(sb);
	if (getenv("XSYS4_BL_TRACE"))
		NOTICE("VScrollbar[%d] ViewSize=%d", parts_no, size);
}
static void PartsEngine_SetVScrollbarScrollPos(int parts_no, int pos)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, true);
	int max = sb->total_size - sb->view_size;
	if (max < 0) max = 0;
	if (pos < 0) pos = 0;
	if (pos > max) pos = max;
	sb->scroll_pos = pos;
	if (getenv("XSYS4_BL_TRACE"))
		NOTICE("VScrollbar[%d] SetScrollPos req=%d -> %d (total=%d view=%d max=%d)",
		       parts_no, pos, sb->scroll_pos, sb->total_size, sb->view_size, max);
	// Notify the game's scroll delegate (e.g. backlog SetLineIndex) that the bar
	// moved. The real engine posts a Scroll message when a scrollbar's position is
	// set; the game's CPartsMessageManager dispatches it to the registered
	// ScrollEvent. Post (ScrollPos, TotalSize) UNCONDITIONALLY — not only on change.
	// The backlog's InitVScrollbar sets ScrollPos to NumofLine to trigger the very
	// first SetLineIndex via ScrollEvent; with a small history (NumofLine <= ViewSize)
	// the clamped pos stays 0 == old, so gating on change suppressed the initial
	// render and the log stayed empty until enough lines accrued (>ViewSize). No feedback
	// loop: SetLineIndex never calls back into SetVScrollbarScrollPos.
	// Reposition the visual knob (if this scrollbar has one) to match the new pos.
	PE_SetPartsVScrollbarRate(parts_no, max > 0 ? (float)sb->scroll_pos / (float)max : 0.0f);
	struct parts *p = parts_try_get(parts_no);
	if (p)
		parts_msg_push(p, PE_PARTS_MSG_SCROLL, "ii", pos, sb->total_size);
}
static void PartsEngine_SetVScrollbarScrollRate(int parts_no, float rate)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, true);
	int max = sb->total_size - sb->view_size;
	if (max < 0) max = 0;
	if (rate < 0.0f) rate = 0.0f;
	if (rate > 1.0f) rate = 1.0f;
	sb->scroll_pos = (int)(rate * max + 0.5f);
	// Keep the visual knob in sync with the rate.
	PE_SetPartsVScrollbarRate(parts_no, rate);
}

// Called from the parts input layer when the user drags the vertical scrollbar knob.
// Converts the visual rate to a scroll position and notifies the game's scroll
// delegate (posts PARTS_MSG_SCROLL -> backlog SetLineIndex rebuilds the view). The
// knob's own visual position is already updated by parts_vscrollbar_drag_to.
void PE_OnVScrollbarDragged(int parts_no, float rate)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, false);
	if (!sb)
		return;
	int max = sb->total_size - sb->view_size;
	if (max < 0) max = 0;
	if (rate < 0.0f) rate = 0.0f;
	if (rate > 1.0f) rate = 1.0f;
	sb->scroll_pos = (int)(rate * max + 0.5f);
	struct parts *p = parts_try_get(parts_no);
	if (p)
		parts_msg_push(p, PE_PARTS_MSG_SCROLL, "ii", sb->scroll_pos, sb->total_size);
}
static int PartsEngine_GetVScrollbarTotalSize(int parts_no)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, false);
	return sb ? sb->total_size : 0;
}
static int PartsEngine_GetVScrollbarViewSize(int parts_no)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, false);
	int v = sb ? sb->view_size : 0;
	if (getenv("XSYS4_BL_TRACE"))
		NOTICE("VScrollbar[%d] GetViewSize -> %d", parts_no, v);
	return v;
}
static int PartsEngine_GetVScrollbarScrollPos(int parts_no)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, false);
	int p = sb ? sb->scroll_pos : 0;
	if (getenv("XSYS4_BL_TRACE"))
		NOTICE("VScrollbar[%d] GetScrollPos -> %d", parts_no, p);
	return p;
}
// GetVScrollbarScrollRate is the VISIBLE FRACTION (view/total), i.e. the knob's
// size ratio — NOT the scroll position. Reverse of Tsumamigui3.ain:
// backlog::detail::CBackLogView@SetLineIndex ends with
//     m_ViewLastScene = (1.0 <= GetVScrollbarScrollRate(scrollbar))
// and MouseWheelEvent closes the whole BACK LOG (AFL_Activity_End, the same path as
// the SYS_戻る button) when `m_ViewLastScene && Back - Forward > 0`. So the rate MUST
// reach 1.0 when everything fits, or wheel-down never closes a short log.
// The knob's on-screen position is a separate quantity (PE_SetPartsVScrollbarRate,
// fed with scroll_pos/max).
static float PartsEngine_GetVScrollbarScrollRate(int parts_no)
{
	struct pe_vscrollbar *sb = pe_vscrollbar_get(parts_no, false);
	if (!sb) return 1.0f;
	if (sb->total_size <= 0 || sb->view_size >= sb->total_size)
		return 1.0f;
	return (float)sb->view_size / (float)sb->total_size;
}
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarCGName, int a, struct string *b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, GetVScrollbarCGName, int a, struct string **b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetVScrollbarFlatName, int a, struct string *b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, GetVScrollbarFlatName, int a, struct string **b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarOnCursorSoundNumber, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarClickSoundNumber, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarOnCursorSoundNumber, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarClickSoundNumber, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarSize, int a, int b, int c);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarLeftWidth, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarRightWidth, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarLeftWidth, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarRightWidth, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarTotalSize, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarViewSize, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarScrollPos, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarTotalSize, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarViewSize, int a);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, GetHScrollbarScrollPos, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarCGName, int a, struct string *b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, GetHScrollbarCGName, int a, struct string **b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, SetHScrollbarFlatName, int a, struct string *b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, GetHScrollbarFlatName, int a, struct string **b);
HLL_QUIET_UNIMPLEMENTED(false, bool, PartsEngine, IsRadioButtonBoxExistGUI, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, ClearRadioButtonBoxChild, int a);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, AddRadioButtonBoxChild, int a, int b);
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, RemoveRadioButtonBoxChild, int a, int b);
HLL_QUIET_UNIMPLEMENTED(0, int, PartsEngine, NumofRadioButtonBoxChild, int a);
HLL_QUIET_UNIMPLEMENTED(-1, int, PartsEngine, GetRadioButtonBoxChild, int a, int b);

// --- Дочерние parts. Иерархия (для рендера) через готовый parent-указатель;
// запросы количества/по-индексу минимальны. ---
static void PE_AddChild(int parent, int child) { PE_SetParentPartsNumber(child, parent); }
static void PE_InsertChild(int parent, int child, int index) { (void)index; PE_SetParentPartsNumber(child, parent); }
static void PE_RemoveChild(int parent, int child) { (void)parent; PE_SetParentPartsNumber(child, -1); }
static bool PE_IsExistChild(int parent, int child) { return PE_GetParentPartsNumber(child) == parent; }
// Parts_StopSwipe() — отменяет свайп-инерцию. Зовётся первым в
// CBackLogView@MouseWheelEvent (и swipe-обработчиках). Свайп-инерцию не
// моделируем, поэтому no-op; без него колесо в бэклоге падало в REPL.
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, Parts_StopSwipe, void);
// Ixseal CParts@Comment: attaches a debug/author comment string to a part.
// Purely diagnostic metadata — safe no-op for reaching the screen.
HLL_QUIET_UNIMPLEMENTED(, void, PartsEngine, Parts_SetComment, int a, struct string *b);

/*
 * `Parts_GetPartsSize(no, wrap<int> w, wrap<int> h, state)` — ширина и высота
 * состояния одним вызовом, через два out-параметра. Обычные `Parts_GetParts{Width,
 * Height}` у движка уже есть и реально считают размер по состоянию; не хватало
 * только парной формы, которой пользуется `parts::detail::CParts@Size::get`
 * (через `AFL_Parts_GetSize`) — на ней вставал `SceneTitle@FadeInButton`.
 * `wrap<скаляр>` ffi отдаёт как обычный указатель на значение (см. ffi.c).
 */
static void PE_Parts_GetPartsSize(int parts_no, int *w, int *h, int state)
{
	if (w)
		*w = PE_GetPartsWidth(parts_no, state);
	if (h)
		*h = PE_GetPartsHeight(parts_no, state);
}

/*
 * Покомпонентные геттеры сложения/умножения цвета части. Сеттеры
 * (`SetComponentAddColor`/`SetComponentMulColor` → `PE_SetAddColor`/
 * `PE_SetMultiplyColor`) хранят значения по-настоящему, и агрегатные
 * `PE_GetAddColor`/`PE_GetMultiplyColor` их читают — не хватало только
 * покомпонентной формы, которой объявлены эти функции в .ain (одинаково у v7 и
 * v14: `ret=10 (int)`, сдвига формы тут нет, просто были `HLL_TODO_EXPORT`).
 *
 * Нужны именно как геттеры: motion-движок читает ТЕКУЩЕЕ значение как начало
 * интерполяции (`Motion::Executer@InitializeParams` ← `GetValue<float>` ←
 * `CSpriteParts@MulColorR::get`), поэтому без них не работает ни один
 * цветовой фейд. Дефолт части — (255,255,255), нейтральное умножение.
 */
static int PE_GetComponentAddColorR(int parts_no)
{
	int r, g, b;
	PE_GetAddColor(parts_no, &r, &g, &b);
	return r;
}
static int PE_GetComponentAddColorG(int parts_no)
{
	int r, g, b;
	PE_GetAddColor(parts_no, &r, &g, &b);
	return g;
}
static int PE_GetComponentAddColorB(int parts_no)
{
	int r, g, b;
	PE_GetAddColor(parts_no, &r, &g, &b);
	return b;
}
static int PE_GetComponentMulColorR(int parts_no)
{
	int r, g, b;
	PE_GetMultiplyColor(parts_no, &r, &g, &b);
	return r;
}
static int PE_GetComponentMulColorG(int parts_no)
{
	int r, g, b;
	PE_GetMultiplyColor(parts_no, &r, &g, &b);
	return g;
}
static int PE_GetComponentMulColorB(int parts_no)
{
	int r, g, b;
	PE_GetMultiplyColor(parts_no, &r, &g, &b);
	return b;
}

/*
 * Имя SE «клик мимо» — глобальное (не по части) имя звука, который парт-движок
 * играет, когда клик не попал ни в одну часть. Хранится по-настоящему, потому
 * что у сеттера ЕСТЬ геттер (`GetClickMissSoundName`), то есть no-op отличим:
 * `parts::detail::WaitForClick` ставит своё имя на время ожидания и обязано
 * вернуть прежнее. Само воспроизведение здесь не появляется — движок не играет
 * ни одного parts-звука (`Parts_SetSoundName`/`Parts_PlaySound` тоже no-op),
 * это отдельный слой.
 */
static struct string *pe_click_miss_sound = NULL;

static void PE_SetClickMissSoundName(struct string *name)
{
	if (pe_click_miss_sound)
		free_string(pe_click_miss_sound);
	pe_click_miss_sound = string_ref(name ? name : &EMPTY_STRING);
}

static struct string *pe_click_miss_sound_get(void)
{
	return string_ref(pe_click_miss_sound ? pe_click_miss_sound : &EMPTY_STRING);
}

static void PE_GetClickMissSoundName(struct string **out)
{
	if (*out)
		free_string(*out);
	*out = pe_click_miss_sound_get();
}

static struct string *PE_GetClickMissSoundName_ix(void)
{
	return pe_click_miss_sound_get();
}

/*
 * ★СТРОКОВЫЕ ГЕТТЕРЫ СМЕНИЛИ ФОРМУ (тот же класс, что `GetGameVersionByText`
 * в SystemService): v6/v7 объявляют `ret=0 (..., ref string out, ...)`, а
 * Ixseal — `ret=12 (...)`, то есть строку ВОЗВРАТОМ. Линковка идёт по ИМЕНИ, а
 * cif строится по .ain, поэтому C-функция с out-параметром получает мусорный
 * указатель, пишет по нему и возвращает мусор → слот строки с `s = NULL` и SEGV
 * в `string_ref(NULL)`, а НЕ понятная ошибка.
 *
 * Расхождение посчитано тулом (`ainliball <ain> <libno PartsEngine>` против
 * C-сигнатур этого файла): у Dohna 81 функция с `ret=12`, из них движок реально
 * экспортирует 13; 12 были в форме v7 (остальные 68 — либо `HLL_TODO_EXPORT`
 * с `.fun = NULL`, то есть честная ошибка вместо SEGV, либо не экспортируются
 * вовсе). Форма каждой сверена по трём .ain: Dohna `ret=12`, Tsumamigui 3 и
 * Escalayer — `ret=0` с `ref string`.
 *
 * Гейт структурный: подмена в `_PreLink` только если .ain объявил возврат
 * строкой (`return_type.data == AIN_STRING`), поэтому у v6/v7 остаётся прежняя
 * форма с out-параметром.
 */
static struct string *PE_GetActivityPartsName_ix(struct string *act, int number)
{
	struct string *out = NULL;
	PE_GetActivityPartsName(&out, act, number);
	return out;
}

static struct string *PE_GetActivityEXText_ix(struct string *act)
{
	struct string *out = NULL;
	PE_GetActivityEXText(act, &out);
	return out;
}

static struct string *PE_GetMessageVariableString_ix(int index)
{
	struct string *out = NULL;
	PE_GetMessageVariableString(index, &out);
	return out;
}

static struct string *PE_Parts_GetSoundName_ix(int parts_no, int state)
{
	struct string *out = NULL;
	PE_Parts_GetSoundName(parts_no, &out, state);
	return out;
}

static struct string *PE_GetPartsCGName_ix(int parts_no, int state)
{
	// v7-форма НЕ пишет out, когда у состояния нет CG (и оставляет прежнее
	// значение вызывающего); возвратной форме нужна пустая строка.
	struct string *out = NULL;
	PE_GetPartsCGName(parts_no, &out, state);
	return out ? out : string_ref(&EMPTY_STRING);
}

// Заглушки-геттеры (у движка нет хранилища этих имён): пустая строка вместо
// мусора. Смысл — тот же, что у прежней out-формы, менялась только форма.
static struct string *PE_empty_string_ix(void)
{
	return string_ref(&EMPTY_STRING);
}

/*
 * ЗЕРКАЛЬНЫЙ СЛУЧАЙ: `GetTextPartsText` реализован СРАЗУ в Ixseal-форме
 * (`struct string *PE_GetTextPartsText(int, int)`), а v6/v7 объявляют её
 * `ret=0 (ref string, int, int)` — значит для СТАРЫХ игр она была сломана тем
 * же способом. Подставляем out-форму, когда .ain объявил возврат не строкой.
 */
static void PE_GetTextPartsText_v7(struct string **out, int parts_no, int state)
{
	struct string *s = PE_GetTextPartsText(parts_no, state);
	if (*out)
		free_string(*out);
	*out = s;
}

static void PartsEngine_PreLink(void);

// --- «Асинхронная» загрузка ресурса в часть (Ixseal-игры).
// Делегат завершения игра вызывает САМА: в .ain у parts::detail::SetPartsCGThread
// после CALLHLL идёт проверка Delegate.Empty и вызов лямбд. Движку остаётся
// загрузить ресурс — делаем это синхронно, и на «готово?» отвечаем сразу да.
static bool PartsEngine_Parts_SetPartsCGThread(int parts_no, struct string *cg_name, int state)
{
	return PE_SetPartsCG(parts_no, cg_name, 0, state);
}

static bool PartsEngine_Parts_SetPartsFlatThread(int parts_no, struct string *filename, int state)
{
	return PE_SetPartsFlat(parts_no, filename, state);
}

HLL_QUIET_UNIMPLEMENTED(false, bool, PartsEngine, Parts_IsThreadLoading, int a, int b);

HLL_LIBRARY(PartsEngine,
	    HLL_EXPORT(_PreLink, PartsEngine_PreLink),
	    // for versions without PartsEngine.Init
	    HLL_EXPORT(_ModuleInit, PartsEngine_ModuleInit),
	    HLL_EXPORT(_ModuleFini, PartsEngine_ModuleFini),
	    // Oyako Rankan
	    HLL_EXPORT(Init, PE_Init),
	    HLL_EXPORT(Update, PartsEngine_Update),
	    HLL_TODO_EXPORT(UpdateGUIController, PartsEngine_UpdateGUIController),
	    HLL_EXPORT(SetWantSaveBackScene, PE_SetWantSaveBackScene),
	    HLL_EXPORT(SaveBackScene, PE_SaveBackScene),
	    // Пары не было вовсе: просмотр BACK SCENE (CBackSceneView@SetSceneIndex →
	    // parts::detail::LoadBackScene) уходил в «Unimplemented HLL function» → REPL.
	    HLL_EXPORT(LoadBackScene, PE_LoadBackScene),
	    HLL_EXPORT(IsWantSaveBackScene, PE_IsWantSaveBackScene),
	    HLL_EXPORT(ClearBackScene, PE_ClearBackScene),
	    HLL_EXPORT(SetAlphaForBackScene, PE_SetAlphaForBackScene),
	    HLL_EXPORT(SetShowForBackScene, PE_SetShowForBackScene),
	    HLL_EXPORT(SetMulColorForBackScene, PE_SetMulColorForBackScene),
	    HLL_EXPORT(SetFontColorForBackScene, PE_SetFontColorForBackScene),
	    HLL_EXPORT(SaveThumbnail, PE_SaveThumbnail),
	    HLL_EXPORT(GetFreeSystemPartsNumber, PE_GetFreeNumber),
	    // FIXME: what is the difference?
	    HLL_EXPORT(GetFreeSystemPartsNumberNotSaved, PE_GetFreeNumber),
	    HLL_EXPORT(IsExistParts, PE_IsExist),
	    HLL_EXPORT(SetPartsCG, PE_SetPartsCG),
	    HLL_EXPORT(CreatePartsMovie, PE_CreatePartsMovie),
	    HLL_EXPORT(PlayPartsMovie, PE_PlayPartsMovie),
	    HLL_EXPORT(IsEndPartsMovie, PE_IsEndPartsMovie),
	    HLL_EXPORT(ReleasePartsMovie, PE_ReleasePartsMovie),
	    HLL_EXPORT(GetPartsCGName, PE_GetPartsCGName),
	    HLL_EXPORT(SetPartsCGSurfaceArea, PE_SetPartsCGSurfaceArea),
	    HLL_EXPORT(SetLoopCG, PE_SetLoopCG),
	    HLL_EXPORT(SetLoopCGSurfaceArea, PE_SetLoopCGSurfaceArea),
	    HLL_EXPORT(SetText, PE_SetText),
	    HLL_EXPORT(AddPartsText, PE_AddPartsText),
	    HLL_EXPORT(GetTextPartsText, PE_GetTextPartsText),
	    HLL_TODO_EXPORT(DeletePartsTopTextLine, PartsEngine_DeletePartsTopTextLine),
	    HLL_EXPORT(SetPartsTextSurfaceArea, PE_SetPartsTextSurfaceArea),
	    HLL_TODO_EXPORT(SetPartsTextHighlight, PartsEngine_SetPartsTextHighlight),
	    HLL_TODO_EXPORT(AddPartsTextHighlight, PartsEngine_AddPartsTextHighlight),
	    HLL_TODO_EXPORT(ClearPartsTextHighlight, PartsEngine_ClearPartsTextHighlight),
	    HLL_TODO_EXPORT(SetPartsTextCountReturn, PartsEngine_SetPartsTextCountReturn),
	    HLL_TODO_EXPORT(GetPartsTextCountReturn, PartsEngine_GetPartsTextCountReturn),
	    HLL_EXPORT(SetFont, PE_SetFont),
	    HLL_EXPORT(SetPartsFontType, PE_SetPartsFontType),
	    HLL_EXPORT(SetPartsFontSize, PE_SetPartsFontSize),
	    HLL_EXPORT(SetPartsFontColor, PE_SetPartsFontColor),
	    HLL_EXPORT(SetPartsFontBoldWeight, PE_SetPartsFontBoldWeight),
	    HLL_EXPORT(SetPartsFontEdgeColor, PE_SetPartsFontEdgeColor),
	    HLL_EXPORT(SetPartsFontEdgeWeight, PE_SetPartsFontEdgeWeight),
	    HLL_EXPORT(SetTextCharSpace, PE_SetTextCharSpace),
	    HLL_EXPORT(SetTextLineSpace, PE_SetTextLineSpace),
	    HLL_EXPORT(GetTextCharSpace, PE_GetTextCharSpace),
	    HLL_EXPORT(GetTextLineSpace, PE_GetTextLineSpace),
	    HLL_EXPORT(SetHGaugeCG, PE_SetHGaugeCG),
	    HLL_EXPORT(SetHGaugeRate, PE_SetHGaugeRate_int),
	    HLL_EXPORT(SetVGaugeCG, PE_SetVGaugeCG),
	    HLL_EXPORT(SetVGaugeRate, PE_SetVGaugeRate_int),
	    HLL_EXPORT(GetHGaugeNumerator, PE_GetHGaugeNumerator),
	    HLL_EXPORT(GetHGaugeDenominator, PE_GetHGaugeDenominator),
	    HLL_EXPORT(GetVGaugeNumerator, PE_GetVGaugeNumerator),
	    HLL_EXPORT(GetVGaugeDenominator, PE_GetVGaugeDenominator),
	    HLL_EXPORT(SetHGaugeSurfaceArea, PE_SetHGaugeSurfaceArea),
	    HLL_EXPORT(SetVGaugeSurfaceArea, PE_SetVGaugeSurfaceArea),
	    HLL_EXPORT(SetNumeralCG, PE_SetNumeralCG),
	    HLL_EXPORT(SetNumeralLinkedCGNumberWidthWidthList, PE_SetNumeralLinkedCGNumberWidthWidthList),
	    HLL_TODO_EXPORT(SetNumeralFont, PartsEngine_SetNumeralFont),
	    HLL_EXPORT(SetNumeralNumber, PE_SetNumeralNumber),
	    HLL_EXPORT(SetNumeralShowComma, PE_SetNumeralShowComma),
	    HLL_EXPORT(SetNumeralFont, PE_SetNumeralFont),
	    HLL_EXPORT(SetNumeralShowType, PE_SetNumeralShowType),
	    HLL_EXPORT(SetNumeralSpace, PE_SetNumeralSpace),
	    HLL_EXPORT(SetNumeralLength, PE_SetNumeralLength),
	    HLL_EXPORT(SetNumeralSurfaceArea, PE_SetNumeralSurfaceArea),
	    HLL_EXPORT(SetPartsRectangleDetectionSize, PE_SetPartsRectangleDetectionSize),
	    HLL_TODO_EXPORT(SetPartsRectangleDetectionSurfaceArea, PartsEngine_SetPartsRectangleDetectionSurfaceArea),
	    HLL_EXPORT(SetPartsCGDetectionSize, PE_SetPartsCGDetectionSize),
	    HLL_TODO_EXPORT(SetPartsCGDetectionSurfaceArea, PartsEngine_SetPartsCGDetectionSurfaceArea),
	    HLL_EXPORT(SetPartsFlash, PE_SetPartsFlash),
	    HLL_EXPORT(IsPartsFlashEnd, PE_IsPartsFlashEnd),
	    HLL_EXPORT(GetPartsFlashCurrentFrameNumber, PE_GetPartsFlashCurrentFrameNumber),
	    HLL_EXPORT(BackPartsFlashBeginFrame, PE_BackPartsFlashBeginFrame),
	    HLL_EXPORT(StepPartsFlashFinalFrame, PE_StepPartsFlashFinalFrame),
	    HLL_TODO_EXPORT(SetPartsFlashSurfaceArea, PE_SetPartsFlashSurfaceArea),
	    HLL_EXPORT(SetPartsFlashAndStop, PE_SetPartsFlashAndStop),
	    HLL_EXPORT(StopPartsFlash, PE_StopPartsFlash),
	    HLL_EXPORT(StartPartsFlash, PE_StartPartsFlash),
	    HLL_EXPORT(GoFramePartsFlash, PE_GoFramePartsFlash),
	    HLL_EXPORT(GetPartsFlashEndFrame, PE_GetPartsFlashEndFrame),
	    HLL_EXPORT(ExistsFlashFile, PE_ExistsFlashFile),
	    HLL_EXPORT(ClearPartsConstructionProcess, PE_ClearPartsConstructionProcess),
	    HLL_EXPORT(AddCreateToPartsConstructionProcess, PE_AddCreateToPartsConstructionProcess),
	    HLL_EXPORT(AddCreatePixelOnlyToPartsConstructionProcess, PE_AddCreatePixelOnlyToPartsConstructionProcess),
	    HLL_EXPORT(AddCreateCGToProcess, PE_AddCreateCGToProcess),
	    HLL_EXPORT(AddFillToPartsConstructionProcess, PE_AddFillToPartsConstructionProcess),
	    HLL_EXPORT(AddFillAlphaColorToPartsConstructionProcess, PE_AddFillAlphaColorToPartsConstructionProcess),
	    HLL_EXPORT(AddFillAMapToPartsConstructionProcess, PE_AddFillAMapToPartsConstructionProcess),
	    HLL_EXPORT(AddFillWithAlphaToPartsConstructionProcess, PE_AddFillWithAlphaToPartsConstructionProcess),
	    HLL_EXPORT(AddFillGradationHorizonToPartsConstructionProcess, PE_AddFillGradationHorizonToPartsConstructionProcess),
	    HLL_EXPORT(AddDrawRectToPartsConstructionProcess, PE_AddDrawRectToPartsConstructionProcess),
	    HLL_EXPORT(AddDrawCutCGToPartsConstructionProcess, PartsEngine_AddDrawCutCGToPartsConstructionProcess_old),
	    HLL_EXPORT(AddCopyCutCGToPartsConstructionProcess, PartsEngine_AddCopyCutCGToPartsConstructionProcess_old),
	    HLL_EXPORT(AddGrayFilterToPartsConstructionProcess, PE_AddGrayFilterToPartsConstructionProcess),
	    HLL_TODO_EXPORT(AddAddFilterToPartsConstructionProcess, PartsEngine_AddAddFilterToPartsConstructionProcess),
	    HLL_EXPORT(AddMulFilterToPartsConstructionProcess, PE_AddMulFilterToPartsConstructionProcess),
	    HLL_EXPORT(BuildPartsConstructionProcess, PE_BuildPartsConstructionProcess),
	    HLL_EXPORT(AddDrawTextToPartsConstructionProcess, PE_AddDrawTextToPartsConstructionProcess),
	    HLL_EXPORT(AddCopyTextToPartsConstructionProcess, PE_AddCopyTextToPartsConstructionProcess),
	    HLL_EXPORT(SetPartsConstructionSurfaceArea, PE_SetPartsConstructionSurfaceArea),
	    HLL_EXPORT(ReleaseParts, PartsEngine_ReleaseParts),
	    HLL_EXPORT(ReleaseAllParts, PE_ReleaseAllParts),
	    HLL_EXPORT(ReleaseAllPartsWithoutSystem, PE_ReleaseAllPartsWithoutSystem),
	    HLL_EXPORT(SetPos, PE_SetPos),
	    HLL_EXPORT(SetZ, PE_SetZ),
	    HLL_EXPORT(SetShow, PE_SetShow),
	    HLL_EXPORT(SetAlpha, PE_SetAlpha),
	    HLL_EXPORT(SetPartsDrawFilter, PE_SetPartsDrawFilter),
	    HLL_EXPORT(SetAddColor, PE_SetAddColor),
	    HLL_EXPORT(SetMultiplyColor, PE_SetMultiplyColor),
	    HLL_EXPORT(SetPassCursor, PE_SetPassCursor),
	    HLL_EXPORT(Parts_SetWheelable, PE_SetPartsWheelable),
	    HLL_EXPORT(SetComponentEnableClipArea, PE_SetComponentEnableClipArea),
	    HLL_EXPORT(IsComponentEnableClipArea, PE_IsComponentEnableClipArea),
	    HLL_EXPORT(SetComponentClipArea, PE_SetComponentClipArea),
	    HLL_EXPORT(GetComponentClipAreaPosX, PE_GetComponentClipAreaPosX),
	    HLL_EXPORT(GetComponentClipAreaPosY, PE_GetComponentClipAreaPosY),
	    HLL_EXPORT(GetComponentClipAreaPosWidth, PE_GetComponentClipAreaPosWidth),
	    HLL_EXPORT(GetComponentClipAreaPosHeight, PE_GetComponentClipAreaPosHeight),
	    HLL_EXPORT(SetEnableInputProcess, PE_SetEnableInputProcess),
	    HLL_EXPORT(IsEnableInputProcess, PE_IsEnableInputProcess),
	    HLL_EXPORT(SetEnableInput, PE_SetEnableInput),
	    HLL_EXPORT(IsEnableInput, PE_IsEnableInput),
	    HLL_EXPORT(SetClickable, PE_SetClickable),
	    HLL_EXPORT(SetSpeedupRateByMessageSkip, PE_SetSpeedupRateByMessageSkip),
	    HLL_TODO_EXPORT(SetResetTimerByChangeInputStatus, PartsEngine_SetResetTimerByChangeInputStatus),
	    HLL_EXPORT(GetPartsX, PE_GetPartsX),
	    HLL_EXPORT(GetPartsY, PE_GetPartsY),
	    HLL_EXPORT(GetPartsZ, PE_GetPartsZ),
	    HLL_EXPORT(GetPartsShow, PE_GetPartsShow),
	    HLL_EXPORT(GetPartsAlpha, PE_GetPartsAlpha),
	    HLL_TODO_EXPORT(GetAddColor, PartsEngine_GetAddColor),
	    HLL_TODO_EXPORT(GetMultiplyColor, PartsEngine_GetMultiplyColor),
	    HLL_EXPORT(GetPartsPassCursor, PE_GetPartsPassCursor),
	    HLL_EXPORT(GetPartsClickable, PE_GetPartsClickable),
	    HLL_TODO_EXPORT(GetPartsSpeedupRateByMessageSkip, PartsEngine_GetPartsSpeedupRateByMessageSkip),
	    HLL_TODO_EXPORT(GetResetTimerByChangeInputStatus, PartsEngine_GetResetTimerByChangeInputStatus),
	    HLL_EXPORT(GetPartsUpperLeftPosX, PE_GetPartsUpperLeftPosX),
	    HLL_EXPORT(GetPartsUpperLeftPosY, PE_GetPartsUpperLeftPosY),
	    HLL_EXPORT(GetPartsWidth, PE_GetPartsWidth),
	    HLL_EXPORT(GetPartsHeight, PE_GetPartsHeight),
	    HLL_EXPORT(SetInputState, PE_SetInputState),
	    HLL_EXPORT(GetInputState, PE_GetInputState),
	    HLL_EXPORT(SetPartsOriginPosMode, PE_SetPartsOriginPosMode),
	    HLL_EXPORT(GetPartsOriginPosMode, PE_GetPartsOriginPosMode),
	    HLL_EXPORT(SetParentPartsNumber, PE_SetParentPartsNumber),
	    HLL_EXPORT(Parts_GetParentPartsNumber, PE_GetParentPartsNumber),
	    HLL_EXPORT(SetPartsGroupNumber, PE_SetPartsGroupNumber),
	    HLL_EXPORT(SetPartsGroupDecideOnCursor, PE_SetPartsGroupDecideOnCursor),
	    HLL_EXPORT(SetPartsGroupDecideClick, PE_SetPartsGroupDecideClick),
	    HLL_EXPORT(SetOnCursorShowLinkPartsNumber, PE_SetOnCursorShowLinkPartsNumber),
	    HLL_EXPORT(SetPartsMessageWindowShowLink, PE_SetPartsMessageWindowShowLink),
	    HLL_EXPORT(GetPartsMessageWindowShowLink, PE_GetPartsMessageWindowShowLink),
	    HLL_EXPORT(AddMotionPos, PE_AddMotionPos_curve),
	    HLL_EXPORT(AddMotionAlpha, PE_AddMotionAlpha_curve),
	    HLL_TODO_EXPORT(AddMotionCG, PartsEngine_AddMotionCG),
	    HLL_EXPORT(AddMotionHGaugeRate, PE_AddMotionHGaugeRate_curve),
	    HLL_EXPORT(AddMotionVGaugeRate, PE_AddMotionVGaugeRate_curve),
	    HLL_EXPORT(AddMotionNumeralNumber, PE_AddMotionNumeralNumber_curve),
	    HLL_EXPORT(AddMotionMagX, PE_AddMotionMagX_curve),
	    HLL_EXPORT(AddMotionMagY, PE_AddMotionMagY_curve),
	    HLL_EXPORT(AddMotionRotateX, PE_AddMotionRotateX_curve),
	    HLL_EXPORT(AddMotionRotateY, PE_AddMotionRotateY_curve),
	    HLL_EXPORT(AddMotionRotateZ, PE_AddMotionRotateZ_curve),
	    HLL_EXPORT(AddMotionVibrationSize, PE_AddMotionVibrationSize),
	    HLL_EXPORT(AddWholeMotionVibrationSize, PE_AddWholeMotionVibrationSize),
	    HLL_EXPORT(AddMotionSound, PE_AddMotionSound),
	    HLL_TODO_EXPORT(SetSoundNumber, PartsEngine_SetSoundNumber),
	    HLL_TODO_EXPORT(GetSoundNumber, PartsEngine_GetSoundNumber),
	    HLL_EXPORT(SetClickMissSoundNumber, PE_SetClickMissSoundNumber),
	    HLL_TODO_EXPORT(GetClickMissSoundNumber, PartsEngine_GetClickMissSoundNumber),
	    HLL_EXPORT(BeginMotion, PE_BeginMotion),
	    HLL_EXPORT(EndMotion, PE_EndMotion),
	    HLL_EXPORT(PauseMotion, PE_PauseMotion),
	    HLL_EXPORT(IsMotion, PE_IsMotion),
	    HLL_EXPORT(SeekEndMotion, PE_SeekEndMotion),
	    HLL_EXPORT(UpdateMotionTime, PE_UpdateMotionTime),
	    HLL_EXPORT(BeginInput, PE_BeginInput),
	    HLL_EXPORT(EndInput, PE_EndInput),
	    HLL_EXPORT(GetClickPartsNumber, PE_GetClickPartsNumber),
	    HLL_EXPORT(GetFocusPartsNumber, PE_GetFocusPartsNumber),
	    HLL_EXPORT(SetFocusPartsNumber, PE_SetFocusPartsNumber),
	    HLL_EXPORT(GetPartsTextFontProperty, PE_GetPartsTextFontProperty),
	    HLL_TODO_EXPORT(PushGUIController, PartsEngine_PushGUIController),
	    HLL_TODO_EXPORT(PopGUIController, PartsEngine_PopGUIController),
	    HLL_EXPORT(SetPartsMagX, PE_SetPartsMagX),
	    HLL_EXPORT(SetPartsMagY, PE_SetPartsMagY),
	    HLL_EXPORT(SetPartsRotateX, PE_SetPartsRotateX),
	    HLL_EXPORT(SetPartsRotateY, PE_SetPartsRotateY),
	    HLL_EXPORT(SetPartsRotateZ, PE_SetPartsRotateZ),
	    HLL_EXPORT(SetPartsAlphaClipperPartsNumber, PE_SetPartsAlphaClipperPartsNumber),
	    HLL_EXPORT(SetPartsPixelDecide, PE_SetPartsPixelDecide),
	    HLL_EXPORT(IsCursorIn, PE_IsCursorIn),
	    HLL_EXPORT(SetThumbnailReductionSize, PE_SetThumbnailReductionSize),
	    HLL_EXPORT(SetThumbnailMode, PE_SetThumbnailMode),
	    HLL_EXPORT(Save, PE_Save),
	    HLL_EXPORT(SaveWithoutHideParts, PE_SaveWithoutHideParts),
	    HLL_EXPORT(Load, PE_Load),
	    // Rance 9
	    HLL_EXPORT(PartsFunc, PartsEngine_PartsFunc),
	    HLL_EXPORT(AddPartsConstructionProcess, PE_AddPartsConstructionProcess),
	    HLL_EXPORT(Parts_GetPartsConstructionProcessCount, PE_GetPartsConstructionProcessCount),
	    HLL_EXPORT(GetPartsConstructionProcess, PE_GetPartsConstructionProcess),
	    HLL_EXPORT(Release, PartsEngine_ReleaseParts),
	    HLL_TODO_EXPORT(ReleaseAll, PartsEngine_ReleaseAll),
	    HLL_EXPORT(ReleaseAllWithoutSystem, PE_ReleaseAllWithoutSystem),
	    HLL_EXPORT(GetFreeNumber, PE_GetFreeNumber),
	    HLL_EXPORT(IsExist, PE_IsExist),
	    HLL_EXPORT(AddController, PE_AddController),
	    HLL_EXPORT(SetActiveController, PE_set_active_controller),
	    HLL_EXPORT(GetActiveController, PE_get_active_controller),
	    HLL_EXPORT(GetControllerLength, PE_get_controller_length),
	    HLL_EXPORT(GetControllerIndex, PE_get_controller_index),
	    HLL_EXPORT(GetControllerID, PE_get_controller_id),
	    // Слой системного оверлея — на нём игра рисует полноэкранные эффекты
	    // (`全画面色` → `■フラッシュ`, вспышка при событии). Реализация была, но
	    // висела ТОЛЬКО на диспетчере `PartsFunc` (func_id 2, GetSystemOverlayLayer);
	    // Tsumamigui 3 зовёт HLL-функцию напрямую, и её отсутствие уводило движок в
	    // отладочный REPL посреди сцены — со стороны это выглядит как зависание игры
	    // (процесс жив и крутит 100 % CPU).
	    HLL_EXPORT(GetSystemOverlayController, PE_get_system_controller),
	    // Ниже — функции, ДОСТИЖИМЫЕ у Tsumamigui 3 по графу вызовов от `main`
	    // (scripts/ain_reachable.py, FINDINGS §5x). Первые три уже были реализованы
	    // и висели только на диспетчере `PartsFunc`, как и GetSystemOverlayController.
	    HLL_EXPORT(SetLayoutBoxPadding, PE_set_layoutbox_padding),
	    HLL_EXPORT(GetLayoutBoxPaddingTop, PE_get_layoutbox_padding_top),
	    HLL_EXPORT(GetLayoutBoxPaddingBottom, PE_get_layoutbox_padding_bottom),
	    HLL_EXPORT(GetLayoutBoxPaddingLeft, PE_get_layoutbox_padding_left),
	    HLL_EXPORT(GetLayoutBoxPaddingRight, PE_get_layoutbox_padding_right),
	    HLL_EXPORT(GetComponentAbsolutePosX, PE_parts_get_absolute_x),
	    HLL_EXPORT(GetComponentAbsolutePosY, PE_parts_get_absolute_y),
	    HLL_EXPORT(GetComponentAbsolutePosZ, PE_parts_get_absolute_z),
	    // Реализация есть и используется диспетчером PartsFunc (case 103); у
	    // Tsumamigui 3 по графу вызовов НЕдостижима, но экспорт бесплатный и
	    // страхует другие игры — ref-выходы функция заполняет сама.
	    HLL_EXPORT(GetPartsCGSurfaceArea, PE_GetPartsCGSurfaceArea),
	    HLL_EXPORT(GetActiveParts, PE_GetActiveParts),
	    HLL_EXPORT(SetOpenTextBoxIME, PE_SetOpenTextBoxIME),
	    HLL_EXPORT(SetComponentScrollPosXLinkNumber, PE_SetComponentScrollPosXLinkNumber),
	    HLL_EXPORT(SetComponentScrollPosYLinkNumber, PE_SetComponentScrollPosYLinkNumber),
	    HLL_EXPORT(GetComponentScrollPosXLinkNumber, PE_GetComponentScrollPosXLinkNumber),
	    HLL_EXPORT(GetComponentScrollPosYLinkNumber, PE_GetComponentScrollPosYLinkNumber),
	    HLL_EXPORT(CreateActivity, PE_CreateActivity),
	    HLL_EXPORT(IsExistActivity, PE_IsExistActivity),
	    HLL_EXPORT(ReleaseActivity, PE_ReleaseActivity),
	    HLL_EXPORT(AddActivityParts, PE_AddActivityParts),
	    HLL_EXPORT(RemoveActivityParts, PE_RemoveActivityParts),
	    HLL_EXPORT(RemoveAllActivityParts, PE_RemoveAllActivityParts),
	    HLL_EXPORT(NumofActivityParts, PE_NumofActivityParts),
	    HLL_EXPORT(GetActivityParts, PE_GetActivityParts),
	    HLL_EXPORT(IsExistActivityPartsByName, PE_IsExistActivityPartsByName),
	    HLL_EXPORT(IsExistActivityPartsByNumber, PE_IsExistActivityPartsByNumber),
	    HLL_EXPORT(GetActivityPartsNumber, PE_GetActivityPartsNumber),
	    HLL_EXPORT(GetActivityPartsName, PE_GetActivityPartsName),
	    HLL_EXPORT(SetActivityEXText, PE_SetActivityEXText),
	    HLL_EXPORT(GetActivityEXText, PE_GetActivityEXText),
	    HLL_EXPORT(GetActivityEXID, PE_GetActivityEXID),
	    HLL_EXPORT(IsExistActivityFile, PE_IsExistActivityFile),
	    HLL_EXPORT(ReadActivityFile, PE_ReadActivityFile),
	    HLL_EXPORT(SetActivityEndKey, PE_SetActivityEndKey),
	    HLL_EXPORT(EraseActivityEndKey, PE_EraseActivityEndKey),
	    HLL_EXPORT(IsExistActivityEndKey, PE_IsExistActivityEndKey),
	    HLL_EXPORT(NumofActivityEndKey, PE_NumofActivityEndKey),
	    HLL_EXPORT(GetComponentAbsoluteMaxPosZ, PE_GetComponentAbsoluteMaxPosZ),
	    HLL_EXPORT(UpdateMatrix, PE_UpdateMatrix),
	    HLL_EXPORT(GetActivityEndKey, PE_GetActivityEndKey),
	    HLL_EXPORT(AddActivityCloseParts, PE_AddActivityCloseParts),
	    HLL_EXPORT(RemoveActivityCloseParts, PE_RemoveActivityCloseParts),
	    HLL_EXPORT(RemoveAllActivityCloseParts, PE_RemoveAllActivityCloseParts),
	    HLL_EXPORT(IsExistActivityCloseParts, PE_IsExistActivityCloseParts),
	    HLL_EXPORT(SetActivityIntentData, PE_SetActivityIntentData),
	    HLL_EXPORT(AddActivityIntentDataDestination, PE_AddActivityIntentDataDestination),
	    HLL_EXPORT(IsExistActivityIntentData, PE_IsExistActivityIntentData),
	    HLL_EXPORT(NumofActivityIntentDataDestination, PE_NumofActivityIntentDataDestination),
	    HLL_EXPORT(GetActivityIntentDataDestination, PE_GetActivityIntentDataDestination),
	    HLL_EXPORT(GetActivityIntentDataType, PE_GetActivityIntentDataType),
	    HLL_EXPORT(WriteActivityFile, PartsEngine_WriteActivityFile),
	    HLL_EXPORT(SaveActivityEXText, PartsEngine_SaveActivityEXText),
	    HLL_EXPORT(LoadActivityEXText, PartsEngine_LoadActivityEXText),
	    HLL_EXPORT(SetButtonSize, PartsEngine_SetButtonSize),
	    HLL_EXPORT(SetButtonEnable, PartsEngine_SetButtonEnable),
	    HLL_EXPORT(IsButtonEnable, PartsEngine_IsButtonEnable),
	    HLL_EXPORT(SetButtonColor, PartsEngine_SetButtonColor),
	    HLL_EXPORT(GetButtonR, PartsEngine_GetButtonR),
	    HLL_EXPORT(GetButtonG, PartsEngine_GetButtonG),
	    HLL_EXPORT(GetButtonB, PartsEngine_GetButtonB),
	    HLL_EXPORT(SetButtonFontProperty, PartsEngine_SetButtonFontProperty),
	    HLL_EXPORT(GetButtonFontProperty, PE_GetButtonFontProperty),
	    HLL_EXPORT(SetButtonCGName, PE_SetButtonCGName),
	    HLL_EXPORT(GetButtonCGName, PE_GetButtonCGName),
	    HLL_EXPORT(SetButtonFlatName, PE_SetButtonFlatName),
	    HLL_EXPORT(GetButtonFlatName, PE_GetButtonFlatName),
	    HLL_EXPORT(SetButtonText, PartsEngine_SetButtonText),
	    HLL_EXPORT(GetButtonText, PE_GetButtonText),
	    HLL_EXPORT(SetButtonTextOriginPosMode, PartsEngine_SetButtonTextOriginPosMode),
	    HLL_EXPORT(GetButtonTextOriginPosMode, PartsEngine_GetButtonTextOriginPosMode),
	    HLL_EXPORT(SetButtonCharSpace, PartsEngine_SetButtonCharSpace),
	    HLL_EXPORT(GetButtonCharSpace, PartsEngine_GetButtonCharSpace),
	    HLL_EXPORT(SetButtonLineSpace, PartsEngine_SetButtonLineSpace),
	    HLL_EXPORT(GetButtonLineSpace, PartsEngine_GetButtonLineSpace),
	    HLL_EXPORT(GetCheckBoxButtonWidth, PartsEngine_GetCheckBoxButtonWidth),
	    HLL_EXPORT(GetCheckBoxButtonHeight, PartsEngine_GetCheckBoxButtonHeight),
	    HLL_EXPORT(SetCheckBoxButtonMode, PartsEngine_SetCheckBoxButtonMode),
	    HLL_EXPORT(IsCheckBoxButtonMode, PartsEngine_IsCheckBoxButtonMode),
	    HLL_EXPORT(SetVScrollbarMoveSizeByButton, PE_SetVScrollbarMoveSizeByButton),
	    HLL_EXPORT(GetVScrollbarMoveSizeByButton, PE_GetVScrollbarMoveSizeByButton),
	    HLL_EXPORT(SetHScrollbarMoveSizeByButton, PartsEngine_SetHScrollbarMoveSizeByButton),
	    HLL_EXPORT(GetHScrollbarMoveSizeByButton, PartsEngine_GetHScrollbarMoveSizeByButton),
	    HLL_EXPORT(IsRadioButtonBoxExistGUI, PartsEngine_IsRadioButtonBoxExistGUI),
	    HLL_EXPORT(ClearRadioButtonBoxChild, PartsEngine_ClearRadioButtonBoxChild),
	    HLL_EXPORT(AddRadioButtonBoxChild, PartsEngine_AddRadioButtonBoxChild),
	    HLL_EXPORT(RemoveRadioButtonBoxChild, PartsEngine_RemoveRadioButtonBoxChild),
	    HLL_EXPORT(NumofRadioButtonBoxChild, PartsEngine_NumofRadioButtonBoxChild),
	    HLL_EXPORT(GetRadioButtonBoxChild, PartsEngine_GetRadioButtonBoxChild),
	    HLL_EXPORT(AddChild, PE_AddChild),
	    HLL_EXPORT(InsertChild, PE_InsertChild),
	    HLL_EXPORT(RemoveChild, PE_RemoveChild),
	    HLL_EXPORT(IsExistChild, PE_IsExistChild),
	    HLL_EXPORT(ClearChild, PE_ClearChild),
	    HLL_EXPORT(NumofChild, PE_NumofChild),
	    HLL_EXPORT(GetChild, PE_GetChild),
	    HLL_EXPORT(GetChildIndex, PE_GetChildIndex),
	    HLL_EXPORT(SetEventID, PE_SetEventID),
	    HLL_EXPORT(Parts_StopSwipe, PartsEngine_Parts_StopSwipe),
	    HLL_EXPORT(Parts_SetSwipeType, PE_SetSwipeType),
	    HLL_EXPORT(Parts_GetSwipeType, PE_GetSwipeType),
	    HLL_EXPORT(Parts_SetComment, PartsEngine_Parts_SetComment),
	    HLL_EXPORT(RemoveController, PE_RemoveController),
	    HLL_EXPORT(UpdateComponent, PartsEngine_Update),
	    HLL_EXPORT(Parts_SetThumbnailReductionSize, PE_SetThumbnailReductionSize),
	    HLL_EXPORT(Parts_SetThumbnailMode, PE_SetThumbnailMode),
	    HLL_EXPORT(GetClickNumber, PE_GetClickPartsNumber),
	    HLL_EXPORT(SetClickMissSoundName, PE_SetClickMissSoundName),
	    HLL_EXPORT(GetClickMissSoundName, PE_GetClickMissSoundName),
	    HLL_EXPORT(StopSoundWithoutSystemSound, PartsEngine_StopSoundWithoutSystemSound),
	    HLL_EXPORT(Parts_SetSoundName, PE_Parts_SetSoundName),
	    HLL_EXPORT(Parts_GetSoundName, PE_Parts_GetSoundName),
	    HLL_TODO_EXPORT(CrateActivityBinary, PartsEngine_CrateActivityBinary),
	    HLL_TODO_EXPORT(ReadActivityBinary, PartsEngine_ReadActivityBinary),
	    HLL_EXPORT(ReleaseMessage, PE_ReleaseMessage),
	    HLL_EXPORT(PopMessage, PE_PopMessage),
	    HLL_EXPORT(GetMessagePartsNumber, PE_GetMessagePartsNumber),
	    HLL_EXPORT(GetMessageDelegateIndex, PE_GetMessageDelegateIndex),
	    HLL_EXPORT(GetMessageUniqueID, PE_GetMessageUniqueID),
	    HLL_EXPORT(GetDelegateIndex, PE_GetDelegateIndex),
	    HLL_EXPORT(GetMessageType, PE_GetMessageType),
	    HLL_EXPORT(GetMessageVariableCount, PE_GetMessageVariableCount),
	    HLL_EXPORT(GetMessageVariableType, PE_GetMessageVariableType),
	    HLL_EXPORT(GetMessageVariableInt, PE_GetMessageVariableInt),
	    HLL_EXPORT(GetMessageVariableFloat, PE_GetMessageVariableFloat),
	    HLL_EXPORT(GetMessageVariableBool, PE_GetMessageVariableBool),
	    HLL_EXPORT(GetMessageVariableString, PE_GetMessageVariableString),
	    HLL_EXPORT(SetDelegateIndex, PE_SetDelegateIndex),
	    HLL_EXPORT(SetFocus, PE_SetFocusPartsNumber),
	    HLL_EXPORT(IsFocus, PE_IsFocus),
	    HLL_EXPORT(SetComponentReverseLR, PE_SetComponentReverseLR),
	    HLL_EXPORT(SetComponentReverseTB, PE_SetComponentReverseTB),
	    HLL_EXPORT(GetComponentReverseLR, PE_GetComponentReverseLR),
	    HLL_EXPORT(GetComponentReverseTB, PE_GetComponentReverseTB),
	    HLL_EXPORT(SetUserComponentName, PE_SetUserComponentName),
	    HLL_EXPORT(GetUserComponentName, PE_GetUserComponentName),
	    HLL_EXPORT(SetUserComponentData, PE_SetUserComponentData),
	    HLL_EXPORT(GetUserComponentData, PE_GetUserComponentData),
	    HLL_EXPORT(SetComponentType, PE_SetComponentType),
	    HLL_EXPORT(GetComponentType, PE_GetComponentType),
	    HLL_EXPORT(SetComponentPos, PartsEngine_SetComponentPos),
	    HLL_EXPORT(SetComponentPosZ, PE_SetZ),
	    HLL_EXPORT(GetComponentPosX, PartsEngine_Parts_GetComponentPosX),
	    HLL_EXPORT(GetComponentPosY, PartsEngine_GetComponentPosY),
	    HLL_EXPORT(GetComponentPosZ, PE_GetPartsZ),
	    HLL_EXPORT(Parts_GetPartsUpperLeftPos, PartsEngine_Parts_GetPartsUpperLeftPos),
	    HLL_EXPORT(Parts_GetPartsUpperLeftPosX, PartsEngine_Parts_GetPartsUpperLeftPosX),
	    HLL_EXPORT(Parts_GetPartsUpperLeftPosY, PartsEngine_Parts_GetPartsUpperLeftPosY),
	    HLL_EXPORT(SetComponentOriginPosMode, PE_SetPartsOriginPosMode),
	    HLL_EXPORT(GetComponentOriginPosMode, PE_GetPartsOriginPosMode),
	    HLL_TODO_EXPORT(GetComponentWidth, PartsEngine_GetComponentWidth),
	    HLL_TODO_EXPORT(GetComponentHeight, PartsEngine_GetComponentHeight),
	    HLL_EXPORT(Parts_GetPartsSize, PE_Parts_GetPartsSize),
	    HLL_EXPORT(Parts_GetPartsWidth, PE_GetPartsWidth),
	    HLL_EXPORT(Parts_GetPartsHeight, PE_GetPartsHeight),
	    HLL_EXPORT(SetComponentShow, PE_SetComponentShow),
	    HLL_EXPORT(IsComponentShow, PE_IsComponentShow),
	    HLL_EXPORT(SetComponentMessageWindowShowLink, PE_SetPartsMessageWindowShowLink),
	    HLL_EXPORT(IsComponentMessageWindowShowLink, PE_GetPartsMessageWindowShowLink),
	    // Окно реплик ADV (`メッセージウィンドウ`) — src/parts/message_window.c.
	    HLL_EXPORT(SetMessageWindowActive, PE_SetMessageWindowActive),
	    HLL_EXPORT(SetMessageWindowInactiveMultipleColor, PE_SetMessageWindowInactiveMultipleColor),
	    HLL_EXPORT(GetMessageWindowInactiveMultipleColorR, PE_GetMessageWindowInactiveMultipleColorR),
	    HLL_EXPORT(GetMessageWindowInactiveMultipleColorG, PE_GetMessageWindowInactiveMultipleColorG),
	    HLL_EXPORT(GetMessageWindowInactiveMultipleColorB, PE_GetMessageWindowInactiveMultipleColorB),
	    HLL_EXPORT(SetMessageWindowCGName, PE_SetMessageWindowCGName),
	    HLL_EXPORT(GetMessageWindowCGName, PE_GetMessageWindowCGName),
	    HLL_EXPORT(SetMessageWindowFlatName, PE_SetMessageWindowFlatName),
	    HLL_EXPORT(GetMessageWindowFlatName, PE_GetMessageWindowFlatName),
	    HLL_EXPORT(SetMessageWindowFlatShowWaitFrameNumber, PE_SetMessageWindowFlatShowWaitFrameNumber),
	    HLL_EXPORT(GetMessageWindowFlatShowWaitFrameNumber, PE_GetMessageWindowFlatShowWaitFrameNumber),
	    HLL_EXPORT(IsOverMessageWindowFlatShowWaitFrame, PE_IsOverMessageWindowFlatShowWaitFrame),
	    HLL_EXPORT(BackMessageWindowFlatBeginFrame, PE_BackMessageWindowFlatBeginFrame),
	    HLL_EXPORT(StepMessageWindowFlatFinalFrame, PE_StepMessageWindowFlatFinalFrame),
	    HLL_EXPORT(SetMessageWindowText, PE_SetMessageWindowText),
	    HLL_EXPORT(GetMessageWindowText, PE_GetMessageWindowText),
	    HLL_EXPORT(FixMessageWindowText, PE_FixMessageWindowText),
	    HLL_EXPORT(IsFixedMessageWindowText, PE_IsFixedMessageWindowText),
	    HLL_EXPORT(SetMessageWindowTextArea, PE_SetMessageWindowTextArea),
	    HLL_EXPORT(GetMessageWindowTextArea, PE_GetMessageWindowTextArea),
	    HLL_EXPORT(SetMessageWindowTextOriginPosMode, PE_SetMessageWindowTextOriginPosMode),
	    HLL_EXPORT(GetMessageWindowTextOriginPosMode, PE_GetMessageWindowTextOriginPosMode),
	    HLL_EXPORT(SetMessageWindowTextFont, PE_SetMessageWindowTextFont),
	    HLL_EXPORT(GetMessageWindowTextFont, PE_GetMessageWindowTextFont),
	    HLL_EXPORT(SetMessageWindowTextSpeed, PE_SetMessageWindowTextSpeed),
	    HLL_EXPORT(GetMessageWindowTextSpeed, PE_GetMessageWindowTextSpeed),
	    HLL_EXPORT(SetMessageWindowTextSpace, PE_SetMessageWindowTextSpace),
	    HLL_EXPORT(GetMessageWindowTextSpace, PE_GetMessageWindowTextSpace),
	    HLL_EXPORT(SetMessageWindowRubyFont, PE_SetMessageWindowRubyFont),
	    HLL_EXPORT(GetMessageWindowRubyFont, PE_GetMessageWindowRubyFont),
	    HLL_EXPORT(SetMessageWindowRubyCharSpace, PE_SetMessageWindowRubyCharSpace),
	    HLL_EXPORT(GetMessageWindowRubyCharSpace, PE_GetMessageWindowRubyCharSpace),
	    HLL_EXPORT(SetMessageWindowRubyLineSpace, PE_SetMessageWindowRubyLineSpace),
	    HLL_EXPORT(GetMessageWindowRubyLineSpace, PE_GetMessageWindowRubyLineSpace),
	    HLL_EXPORT(SetEnableMessageWindowTextWrapping, PE_SetEnableMessageWindowTextWrapping),
	    HLL_EXPORT(IsEnableMessageWindowTextWrapping, PE_IsEnableMessageWindowTextWrapping),
	    HLL_EXPORT(SetKeyWaitCGName, PE_SetKeyWaitCGName),
	    HLL_EXPORT(GetKeyWaitCGName, PE_GetKeyWaitCGName),
	    HLL_EXPORT(SetKeyWaitFlatName, PE_SetKeyWaitFlatName),
	    HLL_EXPORT(GetKeyWaitFlatName, PE_GetKeyWaitFlatName),
	    HLL_EXPORT(SetKeyWaitPos, PE_SetKeyWaitPos),
	    HLL_EXPORT(GetKeyWaitPosX, PE_GetKeyWaitPosX),
	    HLL_EXPORT(GetKeyWaitPosY, PE_GetKeyWaitPosY),
	    HLL_EXPORT(GetKeyWaitPosZ, PE_GetKeyWaitPosZ),
	    HLL_EXPORT(SetKeyWaitShow, PE_SetKeyWaitShow),
	    HLL_EXPORT(IsKeyWaitShow, PE_IsKeyWaitShow),
	    HLL_EXPORT(SetComponentAlpha, PE_SetAlpha),
	    HLL_EXPORT(GetComponentAlpha, PE_GetPartsAlpha),
	    HLL_EXPORT(SetComponentAddColor, PE_SetAddColor),
	    HLL_EXPORT(GetComponentAddColorR, PE_GetComponentAddColorR),
	    HLL_EXPORT(GetComponentAddColorG, PE_GetComponentAddColorG),
	    HLL_EXPORT(GetComponentAddColorB, PE_GetComponentAddColorB),
	    HLL_EXPORT(SetComponentMulColor, PE_SetMultiplyColor),
	    HLL_EXPORT(GetComponentMulColorR, PE_GetComponentMulColorR),
	    HLL_EXPORT(GetComponentMulColorG, PE_GetComponentMulColorG),
	    HLL_EXPORT(GetComponentMulColorB, PE_GetComponentMulColorB),
	    HLL_EXPORT(SetComponentDrawFilter, PE_SetPartsDrawFilter),
	    HLL_EXPORT(GetComponentDrawFilter, PE_GetPartsDrawFilter),
	    HLL_EXPORT(SetComponentMagX, PE_SetPartsMagX),
	    HLL_EXPORT(SetComponentMagY, PE_SetPartsMagY),
	    HLL_EXPORT(GetComponentMagX, PE_GetPartsMagX),
	    HLL_EXPORT(GetComponentMagY, PE_GetPartsMagY),
	    HLL_EXPORT(SetComponentRotateX, PE_SetPartsRotateX),
	    HLL_EXPORT(SetComponentRotateY, PE_SetPartsRotateY),
	    HLL_EXPORT(SetComponentRotateZ, PE_SetPartsRotateZ),
	    HLL_EXPORT(GetComponentRotateX, PE_GetPartsRotateX),
	    HLL_EXPORT(GetComponentRotateY, PE_GetPartsRotateY),
	    HLL_EXPORT(GetComponentRotateZ, PE_GetPartsRotateZ),
	    HLL_EXPORT(SetComponentMargin, PE_SetComponentMargin),
	    HLL_EXPORT(GetComponentMarginTop, PE_GetComponentMarginTop),
	    HLL_EXPORT(GetComponentMarginBottom, PE_GetComponentMarginBottom),
	    HLL_EXPORT(GetComponentMarginLeft, PE_GetComponentMarginLeft),
	    HLL_EXPORT(GetComponentMarginRight, PE_GetComponentMarginRight),
	    HLL_EXPORT(SetComponentAlphaClipper, PE_SetPartsAlphaClipperPartsNumber),
	    HLL_EXPORT(GetComponentAlphaClipper, PE_GetPartsAlphaClipperPartsNumber),
	    HLL_TODO_EXPORT(SetComponentTextureFilterType, PartsEngine_SetComponentTextureFilterType),
	    HLL_TODO_EXPORT(GetComponentTextureFilterType, PartsEngine_GetComponentTextureFilterType),
	    HLL_TODO_EXPORT(SetComponentMipmap, PartsEngine_SetComponentMipmap),
	    HLL_TODO_EXPORT(IsComponentMipmap, PartsEngine_IsComponentMipmap),
	    HLL_EXPORT(SetComponentSpeedupRateByMessageSkip, PE_SetSpeedupRateByMessageSkip),
	    HLL_TODO_EXPORT(GetComponentSpeedupRateByMessageSkip, PartsEngine_GetComponentSpeedupRateByMessageSkip),
	    HLL_EXPORT(AddComponentMotionPos, PartsEngine_AddComponentMotionPos),
	    HLL_EXPORT(AddComponentMotionAlpha, PE_AddMotionAlpha_curve),
	    HLL_TODO_EXPORT(AddComponentMotionCG, PartsEngine_AddComponentMotionCG),
	    HLL_TODO_EXPORT(AddComponentMotionCGTermination, PartsEngine_AddComponentMotionCGTermination),
	    HLL_EXPORT(AddComponentMotionHGaugeRate, PE_AddMotionHGaugeRate_curve),
	    HLL_EXPORT(AddComponentMotionVGaugeRate, PE_AddMotionVGaugeRate_curve),
	    HLL_EXPORT(AddComponentMotionNumeralNumber, PE_AddMotionNumeralNumber_curve),
	    HLL_EXPORT(AddComponentMotionMagX, PE_AddMotionMagX_curve),
	    HLL_EXPORT(AddComponentMotionMagY, PE_AddMotionMagY_curve),
	    HLL_EXPORT(AddComponentMotionRotateX, PE_AddMotionRotateX_curve),
	    HLL_EXPORT(AddComponentMotionRotateY, PE_AddMotionRotateY_curve),
	    HLL_EXPORT(AddComponentMotionRotateZ, PE_AddMotionRotateZ_curve),
	    HLL_EXPORT(AddComponentMotionVibrationSize, PE_AddMotionVibrationSize),
	    HLL_TODO_EXPORT(SuspendBuildView, PartsEngine_SuspendBuildView),
	    HLL_TODO_EXPORT(SuspendBuildViewAt, PartsEngine_SuspendBuildViewAt),
	    HLL_TODO_EXPORT(ResumeBuildView, PartsEngine_ResumeBuildView),
	    HLL_TODO_EXPORT(SetButtonDrag, PartsEngine_SetButtonDrag),
	    HLL_TODO_EXPORT(IsButtonDrag, PartsEngine_IsButtonDrag),
	    HLL_TODO_EXPORT(SetButtonPixelDecide, PartsEngine_SetButtonPixelDecide),
	    HLL_TODO_EXPORT(IsButtonPixelDecide, PartsEngine_IsButtonPixelDecide),
	    HLL_TODO_EXPORT(SetButtonOnCursorSoundNumber, PartsEngine_SetButtonOnCursorSoundNumber),
	    HLL_TODO_EXPORT(SetButtonClickSoundNumber, PartsEngine_SetButtonClickSoundNumber),
	    HLL_TODO_EXPORT(GetButtonOnCursorSoundNumber, PartsEngine_GetButtonOnCursorSoundNumber),
	    HLL_TODO_EXPORT(GetButtonClickSoundNumber, PartsEngine_GetButtonClickSoundNumber),
	    HLL_TODO_EXPORT(SetCheckBoxSize, PartsEngine_SetCheckBoxSize),
	    HLL_TODO_EXPORT(SetCheckBoxDrag, PartsEngine_SetCheckBoxDrag),
	    HLL_TODO_EXPORT(IsCheckBoxDrag, PartsEngine_IsCheckBoxDrag),
	    HLL_EXPORT(SetCheckBoxEnable, PartsEngine_SetCheckBoxEnable),
	    HLL_EXPORT(IsCheckBoxEnable, PartsEngine_IsCheckBoxEnable),
	    HLL_EXPORT(CheckBoxChecked, PE_SetPartsCheckBoxChecked),
	    HLL_EXPORT(IsCheckBoxChecked, PE_GetPartsCheckBoxChecked),
	    HLL_EXPORT(SetCheckBoxColor, PE_SetPartsCheckBoxColor),
	    HLL_EXPORT(GetCheckBoxR, PE_GetPartsCheckBoxR),
	    HLL_EXPORT(GetCheckBoxG, PE_GetPartsCheckBoxG),
	    HLL_EXPORT(GetCheckBoxB, PE_GetPartsCheckBoxB),
	    HLL_TODO_EXPORT(SetCheckBoxFontProperty, PartsEngine_SetCheckBoxFontProperty),
	    HLL_TODO_EXPORT(GetCheckBoxFontProperty, PartsEngine_GetCheckBoxFontProperty),
	    HLL_TODO_EXPORT(SetCheckBoxOnCursorSoundNumber, PartsEngine_SetCheckBoxOnCursorSoundNumber),
	    HLL_TODO_EXPORT(SetCheckBoxClickSoundNumber, PartsEngine_SetCheckBoxClickSoundNumber),
	    HLL_TODO_EXPORT(GetCheckBoxOnCursorSoundNumber, PartsEngine_GetCheckBoxOnCursorSoundNumber),
	    HLL_TODO_EXPORT(GetCheckBoxClickSoundNumber, PartsEngine_GetCheckBoxClickSoundNumber),
	    HLL_TODO_EXPORT(SetCheckBoxCGName, PartsEngine_SetCheckBoxCGName),
	    HLL_TODO_EXPORT(GetCheckBoxCGName, PartsEngine_GetCheckBoxCGName),
	    HLL_TODO_EXPORT(SetCheckBoxText, PartsEngine_SetCheckBoxText),
	    HLL_TODO_EXPORT(GetCheckBoxText, PartsEngine_GetCheckBoxText),
	    HLL_EXPORT(SetVScrollbarOnCursorSoundNumber, PartsEngine_SetVScrollbarOnCursorSoundNumber),
	    HLL_EXPORT(SetVScrollbarClickSoundNumber, PartsEngine_SetVScrollbarClickSoundNumber),
	    HLL_EXPORT(GetVScrollbarOnCursorSoundNumber, PartsEngine_GetVScrollbarOnCursorSoundNumber),
	    HLL_EXPORT(GetVScrollbarClickSoundNumber, PartsEngine_GetVScrollbarClickSoundNumber),
	    HLL_EXPORT(SetVScrollbarSize, PartsEngine_SetVScrollbarSize),
	    HLL_EXPORT(SetVScrollbarUpHeight, PartsEngine_SetVScrollbarUpHeight),
	    HLL_EXPORT(SetVScrollbarDownHeight, PartsEngine_SetVScrollbarDownHeight),
	    HLL_EXPORT(GetVScrollbarUpHeight, PartsEngine_GetVScrollbarUpHeight),
	    HLL_EXPORT(GetVScrollbarDownHeight, PartsEngine_GetVScrollbarDownHeight),
	    HLL_EXPORT(SetVScrollbarTotalSize, PartsEngine_SetVScrollbarTotalSize),
	    HLL_EXPORT(SetVScrollbarViewSize, PartsEngine_SetVScrollbarViewSize),
	    HLL_EXPORT(SetVScrollbarScrollPos, PartsEngine_SetVScrollbarScrollPos),
	    HLL_EXPORT(SetVScrollbarScrollRate, PartsEngine_SetVScrollbarScrollRate),
	    HLL_EXPORT(GetVScrollbarTotalSize, PartsEngine_GetVScrollbarTotalSize),
	    HLL_EXPORT(GetVScrollbarViewSize, PartsEngine_GetVScrollbarViewSize),
	    HLL_EXPORT(GetVScrollbarScrollPos, PartsEngine_GetVScrollbarScrollPos),
	    HLL_EXPORT(GetVScrollbarScrollRate, PartsEngine_GetVScrollbarScrollRate),
	    HLL_EXPORT(SetVScrollbarCGName, PartsEngine_SetVScrollbarCGName),
	    HLL_EXPORT(GetVScrollbarCGName, PartsEngine_GetVScrollbarCGName),
	    HLL_EXPORT(SetVScrollbarFlatName, PartsEngine_SetVScrollbarFlatName),
	    HLL_EXPORT(GetVScrollbarFlatName, PartsEngine_GetVScrollbarFlatName),
	    HLL_EXPORT(SetHScrollbarOnCursorSoundNumber, PartsEngine_SetHScrollbarOnCursorSoundNumber),
	    HLL_EXPORT(SetHScrollbarClickSoundNumber, PartsEngine_SetHScrollbarClickSoundNumber),
	    HLL_EXPORT(GetHScrollbarOnCursorSoundNumber, PartsEngine_GetHScrollbarOnCursorSoundNumber),
	    HLL_EXPORT(GetHScrollbarClickSoundNumber, PartsEngine_GetHScrollbarClickSoundNumber),
	    HLL_EXPORT(SetHScrollbarSize, PartsEngine_SetHScrollbarSize),
	    HLL_EXPORT(SetHScrollbarLeftWidth, PartsEngine_SetHScrollbarLeftWidth),
	    HLL_EXPORT(SetHScrollbarRightWidth, PartsEngine_SetHScrollbarRightWidth),
	    HLL_EXPORT(GetHScrollbarLeftWidth, PartsEngine_GetHScrollbarLeftWidth),
	    HLL_EXPORT(GetHScrollbarRightWidth, PartsEngine_GetHScrollbarRightWidth),
	    HLL_EXPORT(SetHScrollbarTotalSize, PartsEngine_SetHScrollbarTotalSize),
	    HLL_EXPORT(SetHScrollbarViewSize, PartsEngine_SetHScrollbarViewSize),
	    HLL_EXPORT(SetHScrollbarScrollPos, PartsEngine_SetHScrollbarScrollPos),
	    HLL_EXPORT(SetHScrollbarScrollRate, PE_SetPartsHScrollbarScrollRate),
	    HLL_EXPORT(SetHSliderBarScrollRate, PE_SetHSliderBarScrollRate),
	    HLL_EXPORT(GetHSliderBarScrollRate, PE_GetHSliderBarScrollRate),
	    HLL_EXPORT(GetHScrollbarTotalSize, PartsEngine_GetHScrollbarTotalSize),
	    HLL_EXPORT(GetHScrollbarViewSize, PartsEngine_GetHScrollbarViewSize),
	    HLL_EXPORT(GetHScrollbarScrollPos, PartsEngine_GetHScrollbarScrollPos),
	    HLL_EXPORT(GetHScrollbarScrollRate, PE_GetPartsHScrollbarScrollRate),
	    HLL_EXPORT(SetHScrollbarCGName, PartsEngine_SetHScrollbarCGName),
	    HLL_EXPORT(GetHScrollbarCGName, PartsEngine_GetHScrollbarCGName),
	    HLL_EXPORT(SetHScrollbarFlatName, PartsEngine_SetHScrollbarFlatName),
	    HLL_EXPORT(GetHScrollbarFlatName, PartsEngine_GetHScrollbarFlatName),
	    HLL_EXPORT(SetTextBoxSize, PE_SetTextBoxSize),
	    HLL_EXPORT(SetTextBoxFontProperty, PE_SetTextBoxFontProperty),
	    HLL_EXPORT(GetTextBoxFontProperty, PE_GetTextBoxFontProperty),
	    HLL_EXPORT(SetTextBoxText, PE_SetTextBoxText),
	    HLL_EXPORT_N(GetTextBoxText, 1, PE_GetTextBoxText1),
	    HLL_EXPORT(GetTextBoxText, PE_GetTextBoxText),
	    HLL_EXPORT(SetTextBoxMaxTextLength, PE_SetTextBoxMaxTextLength),
	    HLL_EXPORT(GetTextBoxMaxTextLength, PE_GetTextBoxMaxTextLength),
	    HLL_EXPORT(SetTextBoxSelectColor, PE_SetTextBoxSelectColor),
	    HLL_EXPORT(GetTextBoxSelectR, PE_GetTextBoxSelectR),
	    HLL_EXPORT(GetTextBoxSelectG, PE_GetTextBoxSelectG),
	    HLL_EXPORT(GetTextBoxSelectB, PE_GetTextBoxSelectB),
	    HLL_EXPORT(SetTextBoxCGName, PE_SetTextBoxCGName),
	    HLL_EXPORT(GetTextBoxCGName, PE_GetTextBoxCGName),
	    HLL_EXPORT(OpenTextBoxIME, PE_OpenTextBoxIME),
	    HLL_EXPORT(CloseTextBoxIME, PE_CloseTextBoxIME),
	    HLL_EXPORT(IsOpenTextBoxIME, PE_IsOpenTextBoxIME),
	    HLL_EXPORT(SelectTextBoxAll, PE_SelectTextBoxAll),
	    HLL_EXPORT(SetTextBoxReadOnly, PE_SetTextBoxReadOnly),
	    HLL_EXPORT(IsTextBoxReadOnly, PE_IsTextBoxReadOnly),
	    HLL_EXPORT(SetTextBoxCharSpace, PE_SetTextBoxCharSpace),
	    HLL_EXPORT(GetTextBoxCharSpace, PE_GetTextBoxCharSpace),
	    HLL_TODO_EXPORT(SetListBoxSize, PartsEngine_SetListBoxSize),
	    HLL_TODO_EXPORT(SetListBoxLineHeight, PartsEngine_SetListBoxLineHeight),
	    HLL_TODO_EXPORT(GetListBoxLineHeight, PartsEngine_GetListBoxLineHeight),
	    HLL_TODO_EXPORT(SetListBoxMargin, PartsEngine_SetListBoxMargin),
	    HLL_TODO_EXPORT(GetListBoxWidthMargin, PartsEngine_GetListBoxWidthMargin),
	    HLL_TODO_EXPORT(GetListBoxHeightMargin, PartsEngine_GetListBoxHeightMargin),
	    HLL_TODO_EXPORT(SetListBoxCGName, PartsEngine_SetListBoxCGName),
	    HLL_TODO_EXPORT(GetListBoxCGName, PartsEngine_GetListBoxCGName),
	    HLL_TODO_EXPORT(SetListBoxScrollPos, PartsEngine_SetListBoxScrollPos),
	    HLL_TODO_EXPORT(GetListBoxScrollPos, PartsEngine_GetListBoxScrollPos),
	    HLL_TODO_EXPORT(AddListBoxItem, PartsEngine_AddListBoxItem),
	    HLL_TODO_EXPORT(InsertListBoxItem, PartsEngine_InsertListBoxItem),
	    HLL_TODO_EXPORT(SetListBoxItem, PartsEngine_SetListBoxItem),
	    HLL_TODO_EXPORT(GetListBoxItemCount, PartsEngine_GetListBoxItemCount),
	    HLL_TODO_EXPORT(GetListBoxItem, PartsEngine_GetListBoxItem),
	    HLL_TODO_EXPORT(EraseListBoxItem, PartsEngine_EraseListBoxItem),
	    HLL_TODO_EXPORT(ClearListBoxItem, PartsEngine_ClearListBoxItem),
	    HLL_TODO_EXPORT(GetListBoxOnCursorItemIndex, PartsEngine_GetListBoxOnCursorItemIndex),
	    HLL_TODO_EXPORT(GetListBoxOnCursorItem, PartsEngine_GetListBoxOnCursorItem),
	    HLL_TODO_EXPORT(SetListBoxFontProperty, PartsEngine_SetListBoxFontProperty),
	    HLL_TODO_EXPORT(GetListBoxFontProperty, PartsEngine_GetListBoxFontProperty),
	    HLL_TODO_EXPORT(SetListBoxSelectIndex, PartsEngine_SetListBoxSelectIndex),
	    HLL_TODO_EXPORT(GetListBoxSelectIndex, PartsEngine_GetListBoxSelectIndex),
	    HLL_TODO_EXPORT(SetComboBoxSize, PartsEngine_SetComboBoxSize),
	    HLL_TODO_EXPORT(SetComboBoxTextMargin, PartsEngine_SetComboBoxTextMargin),
	    HLL_TODO_EXPORT(GetComboBoxTextWidthMargin, PartsEngine_GetComboBoxTextWidthMargin),
	    HLL_TODO_EXPORT(GetComboBoxTextHeightMargin, PartsEngine_GetComboBoxTextHeightMargin),
	    HLL_TODO_EXPORT(SetComboBoxCGName, PartsEngine_SetComboBoxCGName),
	    HLL_TODO_EXPORT(GetComboBoxCGName, PartsEngine_GetComboBoxCGName),
	    HLL_TODO_EXPORT(SetComboBoxText, PartsEngine_SetComboBoxText),
	    HLL_TODO_EXPORT(GetComboBoxText, PartsEngine_GetComboBoxText),
	    HLL_TODO_EXPORT(SetComboBoxFontProperty, PartsEngine_SetComboBoxFontProperty),
	    HLL_TODO_EXPORT(GetComboBoxFontProperty, PartsEngine_GetComboBoxFontProperty),
	    HLL_EXPORT(SetMultiTextBoxSize, PE_SetMultiTextBoxSize),
	    HLL_EXPORT(SetMultiTextBoxFontProperty, PE_SetMultiTextBoxFontProperty),
	    HLL_EXPORT(GetMultiTextBoxFontProperty, PE_GetMultiTextBoxFontProperty),
	    HLL_EXPORT(SetMultiTextBoxText, PE_SetMultiTextBoxText),
	    HLL_EXPORT_N(GetMultiTextBoxText, 1, PE_GetMultiTextBoxText1),
	    HLL_EXPORT(GetMultiTextBoxText, PE_GetMultiTextBoxText),
	    HLL_EXPORT(SetMultiTextBoxMaxTextLength, PE_SetMultiTextBoxMaxTextLength),
	    HLL_EXPORT(GetMultiTextBoxMaxTextLength, PE_GetMultiTextBoxMaxTextLength),
	    HLL_EXPORT(SetMultiTextBoxSelectColor, PE_SetMultiTextBoxSelectColor),
	    HLL_EXPORT(GetMultiTextBoxSelectR, PE_GetMultiTextBoxSelectR),
	    HLL_EXPORT(GetMultiTextBoxSelectG, PE_GetMultiTextBoxSelectG),
	    HLL_EXPORT(GetMultiTextBoxSelectB, PE_GetMultiTextBoxSelectB),
	    HLL_EXPORT(SetMultiTextBoxCGName, PE_SetMultiTextBoxCGName),
	    HLL_EXPORT(GetMultiTextBoxCGName, PE_GetMultiTextBoxCGName),
	    HLL_EXPORT(SetMultiTextBoxReadOnly, PE_SetMultiTextBoxReadOnly),
	    HLL_EXPORT(IsMultiTextBoxReadOnly, PE_IsMultiTextBoxReadOnly),
	    HLL_EXPORT(SetLayoutBoxLayoutType, PE_SetLayoutBoxLayoutType),
	    HLL_EXPORT(GetLayoutBoxLayoutType, PE_GetLayoutBoxLayoutType),
	    HLL_EXPORT(SetLayoutBoxReturn, PE_SetLayoutBoxReturn),
	    HLL_EXPORT(IsLayoutBoxReturn, PE_IsLayoutBoxReturn),
	    HLL_EXPORT(GetLayoutBoxReturnSize, PE_GetLayoutBoxReturnSize),
	    HLL_EXPORT(SetLayoutBoxAlign, PE_SetLayoutBoxAlign),
	    // Панель (Ixseal, component type 14) — src/parts/panel.c
	    HLL_EXPORT(SetPanelSize, PE_SetPanelSize),
	    HLL_EXPORT(SetPanelColor, PE_SetPanelColor),
	    HLL_EXPORT(GetPanelR, PE_GetPanelR),
	    HLL_EXPORT(GetPanelG, PE_GetPanelG),
	    HLL_EXPORT(GetPanelB, PE_GetPanelB),
	    HLL_EXPORT(GetPanelA, PE_GetPanelA),
	    HLL_EXPORT(SetPanelAlphaGradationTop, PE_SetPanelAlphaGradationTop),
	    HLL_EXPORT(SetPanelAlphaGradationBottom, PE_SetPanelAlphaGradationBottom),
	    HLL_EXPORT(SetPanelAlphaGradationLeft, PE_SetPanelAlphaGradationLeft),
	    HLL_EXPORT(SetPanelAlphaGradationRight, PE_SetPanelAlphaGradationRight),
	    HLL_EXPORT(GetPanelAlphaGradationTop, PE_GetPanelAlphaGradationTop),
	    HLL_EXPORT(GetPanelAlphaGradationBottom, PE_GetPanelAlphaGradationBottom),
	    HLL_EXPORT(GetPanelAlphaGradationLeft, PE_GetPanelAlphaGradationLeft),
	    HLL_EXPORT(GetPanelAlphaGradationRight, PE_GetPanelAlphaGradationRight),
	    HLL_EXPORT(GetLayoutBoxAlign, PE_GetLayoutBoxAlign),
	    HLL_EXPORT(Parts_SetPartsCG, PE_SetPartsCG),
	    HLL_EXPORT(Parts_SetPartsCGThread, PartsEngine_Parts_SetPartsCGThread),
	    HLL_EXPORT(Parts_IsThreadLoading, PartsEngine_Parts_IsThreadLoading),
	    HLL_EXPORT(Parts_GetPartsCGName, PE_GetPartsCGName),
	    HLL_EXPORT(Parts_GetPartsCGDeform, PE_GetPartsCGDeform),
	    HLL_EXPORT(Parts_SetPartsCGSurfaceArea, PE_SetPartsCGSurfaceArea),
	    HLL_EXPORT(Parts_SetLoopCG, PE_SetLoopCG),
	    HLL_EXPORT(Parts_SetLoopCGSurfaceArea, PE_SetLoopCGSurfaceArea),
	    HLL_EXPORT(Parts_SetText, PE_SetText),
	    HLL_EXPORT(Parts_AddPartsText, PE_AddPartsText),
	    HLL_EXPORT(Parts_SetTextEnableTag, PE_SetTextEnableTag),
	    HLL_EXPORT(Parts_IsTextEnableTag, PE_IsTextEnableTag),
	    HLL_TODO_EXPORT(Parts_DeletePartsTopTextLine, PartsEngine_Parts_DeletePartsTopTextLine),
	    HLL_EXPORT(Parts_SetPartsTextSurfaceArea, PE_SetPartsTextSurfaceArea),
	    HLL_TODO_EXPORT(Parts_SetPartsTextHighlight, PartsEngine_Parts_SetPartsTextHighlight),
	    HLL_TODO_EXPORT(Parts_AddPartsTextHighlight, PartsEngine_Parts_AddPartsTextHighlight),
	    HLL_TODO_EXPORT(Parts_ClearPartsTextHighlight, PartsEngine_Parts_ClearPartsTextHighlight),
	    HLL_TODO_EXPORT(Parts_SetPartsTextCountReturn, PartsEngine_Parts_SetPartsTextCountReturn),
	    HLL_TODO_EXPORT(Parts_GetPartsTextCountReturn, PartsEngine_Parts_GetPartsTextCountReturn),
	    HLL_EXPORT(Parts_SetFont, PE_SetFont),
	    HLL_EXPORT(Parts_SetPartsFontType, PE_SetPartsFontType),
	    HLL_EXPORT(Parts_SetPartsFontSize, PE_SetPartsFontSize),
	    HLL_EXPORT(Parts_SetPartsFontColor, PE_SetPartsFontColor),
	    HLL_EXPORT(Parts_SetPartsFontBoldWeight, PE_SetPartsFontBoldWeight),
	    HLL_EXPORT(Parts_SetPartsFontEdgeColor, PE_SetPartsFontEdgeColor),
	    HLL_EXPORT(Parts_SetPartsFontEdgeWeight, PE_SetPartsFontEdgeWeight),
	    HLL_EXPORT(Parts_SetTextCharSpace, PE_SetTextCharSpace),
	    HLL_EXPORT(Parts_SetTextLineSpace, PE_SetTextLineSpace),
	    HLL_EXPORT(Parts_GetTextCharSpace, PE_GetTextCharSpace),
	    HLL_EXPORT(Parts_GetTextLineSpace, PE_GetTextLineSpace),
	    HLL_EXPORT(Parts_SetHGaugeCG, PE_SetHGaugeCG),
	    HLL_EXPORT(Parts_SetHGaugeRate, PE_SetHGaugeRate),
	    HLL_EXPORT(Parts_SetVGaugeCG, PE_SetVGaugeCG),
	    HLL_EXPORT(Parts_SetVGaugeRate, PE_SetVGaugeRate),
	    HLL_EXPORT(Parts_GetHGaugeNumerator, PE_GetHGaugeNumerator),
	    HLL_EXPORT(Parts_GetHGaugeDenominator, PE_GetHGaugeDenominator),
	    HLL_EXPORT(Parts_GetVGaugeNumerator, PE_GetVGaugeNumerator),
	    HLL_EXPORT(Parts_GetVGaugeDenominator, PE_GetVGaugeDenominator),
	    HLL_EXPORT(Parts_SetHGaugeSurfaceArea, PE_SetHGaugeSurfaceArea),
	    HLL_EXPORT(Parts_SetVGaugeSurfaceArea, PE_SetVGaugeSurfaceArea),
	    HLL_EXPORT(Parts_SetHGaugeReverse, PE_SetHGaugeReverse),
	    HLL_EXPORT(Parts_IsHGaugeReverse, PE_IsHGaugeReverse),
	    HLL_EXPORT(Parts_SetVGaugeReverse, PE_SetVGaugeReverse),
	    HLL_EXPORT(Parts_IsVGaugeReverse, PE_IsVGaugeReverse),
	    HLL_EXPORT(Parts_SetNumeralCG, PE_SetNumeralCG),
	    HLL_EXPORT(Parts_SetNumeralLinkedCGNumberWidthWidthList, PE_SetNumeralLinkedCGNumberWidthWidthList),
	    HLL_TODO_EXPORT(Parts_SetNumeralFont, PartsEngine_Parts_SetNumeralFont),
	    HLL_EXPORT(Parts_SetNumeralNumber, PE_SetNumeralNumber),
	    HLL_EXPORT(Parts_SetNumeralShowComma, PE_SetNumeralShowComma),
	    HLL_EXPORT(Parts_SetNumeralSpace, PE_SetNumeralSpace),
	    HLL_EXPORT(Parts_SetNumeralLength, PE_SetNumeralLength),
	    HLL_EXPORT(Parts_SetNumeralSurfaceArea, PE_SetNumeralSurfaceArea),
	    HLL_EXPORT(Parts_SetPartsRectangleDetectionSize, PE_SetPartsRectangleDetectionSize),
	    HLL_TODO_EXPORT(Parts_SetPartsRectangleDetectionSurfaceArea, PartsEngine_Parts_SetPartsRectangleDetectionSurfaceArea),
	    HLL_EXPORT(Parts_SetPartsCGDetectionSize, PE_SetPartsCGDetectionSize),
	    HLL_EXPORT(Parts_GetNumeralNumber, PE_GetNumeralNumber),
	    HLL_EXPORT(Parts_IsNumeralShowComma, PE_IsNumeralShowComma),
	    HLL_EXPORT(Parts_GetNumeralSpace, PE_GetNumeralSpace),
	    HLL_EXPORT(GetNumeralLength, PE_GetNumeralLength),
	    HLL_TODO_EXPORT(Parts_SetPartsCGDetectionSurfaceArea, PartsEngine_Parts_SetPartsCGDetectionSurfaceArea),
	    HLL_EXPORT(Parts_SetPartsFlat, PE_SetPartsFlat),
	    HLL_EXPORT(Parts_SetPartsFlatThread, PartsEngine_Parts_SetPartsFlatThread),
	    HLL_EXPORT(Parts_IsPartsFlatEnd, PE_IsPartsFlatEnd),
	    HLL_EXPORT(Parts_GetPartsFlatCurrentFrameNumber, PE_GetPartsFlatCurrentFrameNumber),
	    HLL_EXPORT(Parts_BackPartsFlatBeginFrame, PE_BackPartsFlatBeginFrame),
	    HLL_EXPORT(Parts_StepPartsFlatFinalFrame, PE_StepPartsFlatFinalFrame),
	    HLL_EXPORT(Parts_SetPartsFlatSurfaceArea, PE_SetPartsFlatSurfaceArea),
	    HLL_EXPORT(Parts_SetPartsFlatAndStop, PE_SetPartsFlatAndStop),
	    HLL_EXPORT(Parts_StopPartsFlat, PE_StopPartsFlat),
	    HLL_EXPORT(Parts_StartPartsFlat, PE_StartPartsFlat),
	    HLL_EXPORT(Parts_GoFramePartsFlat, PE_GoFramePartsFlat),
	    HLL_EXPORT(Parts_GetPartsFlatEndFrame, PE_GetPartsFlatEndFrame),
	    HLL_EXPORT(Parts_ExistsFlatFile, PE_ExistsFlatFile),
	    HLL_TODO_EXPORT(Parts_GetPartsFlatDataInfo, PartsEngine_Parts_GetPartsFlatDataInfo),
	    HLL_TODO_EXPORT(Parts_ChangeFlatCG, PartsEngine_Parts_ChangeFlatCG),
	    HLL_TODO_EXPORT(Parts_ChangeFlatSound, PartsEngine_Parts_ChangeFlatSound),
	    HLL_EXPORT(Parts_ClearPartsConstructionProcess, PE_ClearPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddCreateToPartsConstructionProcess, PE_AddCreateToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddCreatePixelOnlyToPartsConstructionProcess, PE_AddCreatePixelOnlyToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddCreateCGToProcess, PE_AddCreateCGToProcess),
	    HLL_EXPORT(Parts_AddFillToPartsConstructionProcess, PE_AddFillToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddFillAlphaColorToPartsConstructionProcess, PE_AddFillAlphaColorToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddFillAMapToPartsConstructionProcess, PE_AddFillAMapToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddFillWithAlphaToPartsConstructionProcess, PE_AddFillWithAlphaToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddFillGradationHorizonToPartsConstructionProcess, PE_AddFillGradationHorizonToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddDrawRectToPartsConstructionProcess, PE_AddDrawRectToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddDrawCutCGToPartsConstructionProcess, PE_AddDrawCutCGToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddCopyCutCGToPartsConstructionProcess, PE_AddCopyCutCGToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddGrayFilterToPartsConstructionProcess, PE_AddGrayFilterToPartsConstructionProcess),
	    HLL_TODO_EXPORT(Parts_AddAddFilterToPartsConstructionProcess, PartsEngine_Parts_AddAddFilterToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddMulFilterToPartsConstructionProcess, PE_AddMulFilterToPartsConstructionProcess),
	    HLL_TODO_EXPORT(Parts_AddDrawLineToPartsConstructionProcess, PartsEngine_Parts_AddDrawLineToPartsConstructionProcess),
	    HLL_EXPORT(Parts_BuildPartsConstructionProcess, PE_BuildPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddDrawTextToPartsConstructionProcess, PE_AddDrawTextToPartsConstructionProcess),
	    HLL_EXPORT(Parts_AddCopyTextToPartsConstructionProcess, PE_AddCopyTextToPartsConstructionProcess),
	    HLL_EXPORT(Parts_SetPartsConstructionSurfaceArea, PE_SetPartsConstructionSurfaceArea),
	    HLL_EXPORT(Parts_CreateParts3DLayerPluginID, PE_CreateParts3DLayerPluginID),
	    HLL_EXPORT(Parts_GetParts3DLayerPluginID, PE_GetParts3DLayerPluginID),
	    HLL_EXPORT(Parts_ReleaseParts3DLayerPluginID, PE_ReleaseParts3DLayerPluginID),
	    HLL_EXPORT(Parts_SetPassCursor, PE_SetPassCursor),
	    HLL_EXPORT(Parts_SetClickable, PE_SetClickable),
	    HLL_TODO_EXPORT(Parts_SetResetTimerByChangeInputStatus, PartsEngine_Parts_SetResetTimerByChangeInputStatus),
	    HLL_EXPORT(Parts_SetDrag, PE_SetDrag),
	    HLL_EXPORT(Parts_SetParentPartsNumber, PE_SetParentPartsNumber),
	    HLL_EXPORT(Parts_SetInputState, PE_SetInputState),
	    HLL_EXPORT(Parts_SetOnCursorShowLinkPartsNumber, PE_SetOnCursorShowLinkPartsNumber),
	    HLL_EXPORT(Parts_SetSoundNumber, PE_Parts_SetSoundNumber),
	    HLL_EXPORT(Parts_SetPartsPixelDecide, PE_SetPartsPixelDecide),
	    HLL_EXPORT(Parts_GetPartsPassCursor, PE_GetPartsPassCursor),
	    HLL_EXPORT(Parts_GetPartsClickable, PE_GetPartsClickable),
	    HLL_TODO_EXPORT(Parts_GetResetTimerByChangeInputStatus, PartsEngine_Parts_GetResetTimerByChangeInputStatus),
	    HLL_TODO_EXPORT(Parts_GetPartsDrag, PartsEngine_Parts_GetPartsDrag),
	    HLL_EXPORT(Parts_GetInputState, PE_GetInputState),
	    HLL_EXPORT(Parts_GetOnCursorShowLinkPartsNumber, PE_GetOnCursorShowLinkPartsNumber),
	    HLL_EXPORT(Parts_GetSoundNumber, PE_Parts_GetSoundNumber),
	    HLL_TODO_EXPORT(Parts_IsPartsPixelDecide, PartsEngine_Parts_IsPartsPixelDecide),
	    HLL_EXPORT(Parts_IsCursorIn, PE_IsCursorIn),
	    HLL_TODO_EXPORT(SaveParts, PartsEngine_SaveParts),
	    HLL_TODO_EXPORT(LoadParts, PartsEngine_LoadParts)
	    );

static struct ain_hll_function *get_fun(int libno, const char *name)
{
	int fno = ain_get_library_function(ain, libno, name);
	return fno >= 0 ? &ain->libraries[libno].functions[fno] : NULL;
}

static void PartsEngine_PreLink(void)
{
	struct ain_hll_function *fun;
	int libno = ain_get_library(ain, "PartsEngine");
	assert(libno >= 0);

	fun = get_fun(libno, "AddDrawCutCGToPartsConstructionProcess");
	if (fun && fun->nr_arguments == 12) {
		static_library_replace(&lib_PartsEngine, "AddDrawCutCGToPartsConstructionProcess",
				PE_AddDrawCutCGToPartsConstructionProcess);
	}
	fun = get_fun(libno, "AddCopyCutCGToPartsConstructionProcess");
	if (fun && fun->nr_arguments == 12) {
		static_library_replace(&lib_PartsEngine, "AddCopyCutCGToPartsConstructionProcess",
				PE_AddCopyCutCGToPartsConstructionProcess);
	}
	// Ixseal passes the part number, state and a fourth (point list) array
	// explicitly: AddPartsConstructionProcess(int, ArrayInt, ArrayFloat,
	// ArrayString, ArrayPos, int) instead of the classic three arrays.
	fun = get_fun(libno, "AddPartsConstructionProcess");
	if (fun && fun->nr_arguments == 6) {
		static_library_replace(&lib_PartsEngine, "AddPartsConstructionProcess",
				PE_AddPartsConstructionProcess_ix);
	}
	fun = get_fun(libno, "Update");
	if (fun && fun->nr_arguments == 5) {
		static_library_replace(&lib_PartsEngine, "Update",
				PartsEngine_Update_Pascha3PC);
	}
	if (get_fun(libno, "AddController")) {
		PE_enable_multi_controller();
	}
	// В новом message-API «сообщений больше нет» = GetMessageType() == -1, а не 0,
	// и САМИ НОМЕРА типов сообщений другие (см. src/parts/message.c). SeekMessage
	// объявлена только этим API.
	if (get_fun(libno, "SeekMessage")) {
		PE_set_message_empty_type_minus_one();
		PE_set_message_types_ixseal();
	}

	// Строковые геттеры: Ixseal отдаёт строку ВОЗВРАТОМ, v6/v7 — через
	// out-параметр `ref string` (см. большой комментарий выше). Гейт — форма,
	// объявленная в .ain, а не версия движка.
	static const struct { const char *name; void *ix; } str_getters[] = {
		{ "GetClickMissSoundName",  PE_GetClickMissSoundName_ix },
		{ "GetActivityPartsName",   PE_GetActivityPartsName_ix },
		{ "GetActivityEXText",      PE_GetActivityEXText_ix },
		{ "GetMessageVariableString", PE_GetMessageVariableString_ix },
		{ "Parts_GetSoundName",     PE_Parts_GetSoundName_ix },
		{ "GetPartsCGName",         PE_GetPartsCGName_ix },
		{ "Parts_GetPartsCGName",   PE_GetPartsCGName_ix },
		{ "GetButtonCGName",        PE_empty_string_ix },
		{ "GetButtonFlatName",      PE_empty_string_ix },
		{ "GetButtonText",          PE_empty_string_ix },
		{ "GetVScrollbarCGName",    PE_empty_string_ix },
		{ "GetVScrollbarFlatName",  PE_empty_string_ix },
		{ "GetHScrollbarCGName",    PE_empty_string_ix },
		{ "GetHScrollbarFlatName",  PE_empty_string_ix },
	};
	for (unsigned i = 0; i < sizeof(str_getters) / sizeof(*str_getters); i++) {
		fun = get_fun(libno, str_getters[i].name);
		if (fun && fun->return_type.data == AIN_STRING) {
			static_library_replace(&lib_PartsEngine, str_getters[i].name,
					str_getters[i].ix);
		}
	}

	// Зеркально: GetTextPartsText реализована в Ixseal-форме, поэтому старым
	// играм нужна форма с out-параметром.
	fun = get_fun(libno, "GetTextPartsText");
	if (fun && fun->return_type.data != AIN_STRING) {
		static_library_replace(&lib_PartsEngine, "GetTextPartsText",
				PE_GetTextPartsText_v7);
	}
}
