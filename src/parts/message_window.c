/* Copyright (C) 2026 kichikuou <KichikuouChrome@gmail.com>
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

/*
 * Окно реплик ADV (`メッセージウィンドウ`) — см. большой комментарий у
 * `struct parts_message_window` в parts_internal.h: часть держит фон обычным
 * состоянием CG/Flat, а текст и мигалку ожидания клика несут служебные
 * части-потомки.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "system4.h"
#include "system4/ain.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "xsystem4.h"
#include "parts.h"
#include "parts_internal.h"

static struct parts_message_window *mw_get(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->mw : NULL;
}

/*
 * Часть создаётся загрузчиком раскладки (act_build_part), поэтому обращение к
 * окну, которого нет, — это НЕ «игра ошиблась», а «движок не опознал узел».
 * Молчать нельзя: тихий no-op превратился бы в пустое окно без единой жалобы.
 */
void parts_adopt_to_active_layer(int parts_no);

static struct parts_message_window *mw_require(int parts_no, const char *fn)
{
	// Игра переиспользует окно реплик между сценами: если его слой уже снесён,
	// поднимаем окно на актуальный (иначе оно уходит под фон, см.
	// parts_adopt_to_active_layer).
	parts_adopt_to_active_layer(parts_no);
	struct parts_message_window *mw = mw_get(parts_no);
	if (!mw) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("%s: часть %d не является メッセージウィンドウ "
			        "(узел раскладки не опознан загрузчиком)", fn, parts_no);
		}
	}
	return mw;
}

struct parts_message_window *parts_message_window_alloc(void)
{
	struct parts_message_window *mw = xcalloc(1, sizeof(*mw));
	mw->inactive_multiply_color = (SDL_Color) { 255, 255, 255, 255 };
	mw->text_color = (SDL_Color) { 255, 255, 255, 255 };
	mw->text_parts_no = -1;
	mw->mark_parts_no = -1;
	mw->text_fixed = true;
	mw->msg_num = -1;
	return mw;
}

void parts_message_window_free(struct parts_message_window *mw)
{
	if (!mw)
		return;
	if (mw->cg_name)
		free_string(mw->cg_name);
	if (mw->flat_name)
		free_string(mw->flat_name);
	if (mw->msg_func_name)
		free_string(mw->msg_func_name);
	if (mw->mark_cg_name)
		free_string(mw->mark_cg_name);
	if (mw->mark_flat_name)
		free_string(mw->mark_flat_name);
	free(mw);
}

/*
 * `非アクティブ時の乗算カラー` применяется к самой части: 乗算色 наследуется
 * потомками (parts_update_global_multiply_color), поэтому гаснет и фон, и текст —
 * ровно как в оригинале, где неактивное окно приглушено целиком.
 */
static void mw_apply_active(int parts_no, struct parts_message_window *mw)
{
	if (mw->active)
		PE_SetMultiplyColor(parts_no, 255, 255, 255);
	else
		PE_SetMultiplyColor(parts_no, mw->inactive_multiply_color.r,
		                    mw->inactive_multiply_color.g,
		                    mw->inactive_multiply_color.b);
}

// ------------------------------------------------------------------ фон окна

/*
 * Фон окна — либо CG (`ＣＧ名`), либо flat-анимация (`フラット名`). Приоритет у
 * flat: игра переключается на него, задавая непустое имя, и `IsFlat()` в самой
 * игре (fn7091) определён ровно как «フラット名 не пусто».
 */
static void mw_apply_background(int parts_no, struct parts_message_window *mw)
{
	if (mw->flat_name && mw->flat_name->size) {
		PE_SetPartsFlat(parts_no, mw->flat_name, 1);
		return;
	}
	if (mw->cg_name && mw->cg_name->size)
		PE_SetPartsCG(parts_no, mw->cg_name, 0, 1);
}

// ---------------------------------------------------------------------- текст

/*
 * РАЗМЕТКА В ТЕКСТЕ РЕПЛИКИ. Игра передаёт в SetMessageWindowText строку с
 * тегами `${…}`; полный их набор снят со строковой секции .ain (16 шаблонов,
 * тул `alice ain dump -s`):
 *   ${font type=%d|size=%d|r=%d|g=%d|b=%d|bold=%f|edge=%f|er=%d|eg=%d|eb=%d
 *          |tracking=%d|leading=%d} … ${/font}
 *   ${time %d} … ${/time}        (${time 0} — мгновенный вывод, режим скипа)
 * Других форм в .ain нет.
 *
 * Сейчас теги ВЫРЕЗАЮТСЯ, а не исполняются: у части текста один стиль на всю
 * строку, а `${font …}` меняет его посередине — для этого нужны стилевые
 * прогоны внутри parts_text. Оставлять теги в тексте нельзя (они нарисовались бы
 * буквально), поэтому вырезаем и один раз предупреждаем.
 */
static struct string *mw_strip_markup(struct string *src)
{
	struct string *dst = string_ref(&EMPTY_STRING);
	const char *p = src->text;
	const char *end = src->text + src->size;
	const char *run = p;   // начало неразмеченного куска
	bool seen_tag = false;

	// ★Копируем КУСКАМИ, а не побайтно: `string_push_back` принимает СИМВОЛ, и на
	// ведущем байте двухбайтового SJIS (SJIS_2BYTE) дописывает второй байт из
	// старших разрядов — то есть НОЛЬ. Побайтная сборка превращала каждую
	// многобайтовую букву в «ведущий байт + 0x00», и реплика выходила на экран
	// мусором из отдельных латинских глифов.
	while (p < end) {
		if (p[0] == '$' && p + 1 < end && p[1] == '{') {
			const char *close = memchr(p, '}', end - p);
			if (close) {
				if (p > run)
					string_append_cstr(&dst, run, p - run);
				seen_tag = true;
				p = run = close + 1;
				continue;
			}
		}
		p++;
	}
	if (p > run)
		string_append_cstr(&dst, run, p - run);

	if (seen_tag) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("メッセージウィンドウ: разметка ${…} в реплике вырезана, "
			        "а не исполнена (стилевые прогоны внутри строки не реализованы)");
		}
	}
	return dst;
}

static int mw_text_parts(struct parts_message_window *mw)
{
	return mw->text_parts_no;
}

/*
 * ★`テキストエリア` (и `キー待ちマーク／座標`) заданы от ВЕРХНЕГО ЛЕВОГО УГЛА части, а
 * НЕ от её точки привязки 座標. Позиция потомка же складывается с ГЛОБАЛЬНОЙ
 * позицией родителя (parts_update_global_pos), то есть с точкой привязки, —
 * поэтому добавляем origin_offset, сдвиг от привязки к углу (calculate_offset по
 * 原点座標モード).
 *
 * Конвенция снята СЧЁТОМ ПО ДАННЫМ, а не с одной раскладки: у Dohna ровно пять
 * узлов メッセージウィンドウ на все 195 раскладок, и якоря у них РАЗНЫЕ, так что
 * вариант «от угла» — единственный, дающий осмысленный результат в обоих:
 *   AdvMessageWindow_event: 原点座標モード=8 (низ-центр), 座標 (640,720), фон 1280x720
 *     -> угол (0,0), текст (360,600) — внутри нижней полосы окна (её QNT 1280x185
 *     лежит на канве с y=535);
 *   AdvMessageWindow_main:  原点座標モード=5 (центр), 座標 (662,607), фон 862x206
 *     -> угол (231,504), текст (361,566) — отступ 130x62 от края окна.
 * Вариант «от точки привязки» даёт во втором случае x=792 при левом крае окна
 * 231, то есть текст у правой кромки; вариант «экранные координаты» — текст в
 * левом верхнем углу экрана при окне у нижнего края.
 *
 * Ширина/высота области — НЕ обрезка по окну: у main она 940x181 при окне
 * 862x206, то есть это границы переноса, а не рамка. Поэтому surface area
 * задаётся только как размер, а не как клип.
 */
void parts_message_window_relayout(struct parts *parts)
{
	struct parts_message_window *mw = parts->mw;
	if (!mw)
		return;
	Point off = parts->states[PARTS_STATE_DEFAULT].common.origin_offset;
	if (mw->text_parts_no >= 0)
		PE_SetPos(mw->text_parts_no, off.x + mw->text_area.x, off.y + mw->text_area.y);
	if (mw->mark_parts_no >= 0)
		PE_SetPos(mw->mark_parts_no, off.x + mw->mark_pos.x, off.y + mw->mark_pos.y);
}

static void mw_apply_text_area(int parts_no, struct parts_message_window *mw)
{
	parts_message_window_relayout(parts_get(parts_no));
	if (mw->text_area.w > 0 && mw->text_area.h > 0)
		PE_SetPartsTextSurfaceArea(mw->text_parts_no, 0, 0,
		                           mw->text_area.w, mw->text_area.h, 1);
}

/*
 * Создать окно на уже созданной части. Зовётся загрузчиком раскладки
 * (act_build_part), когда у узла `パーツタイプ = メッセージウィンドウ`.
 *
 * Номер служебной части текста берётся из ТОЙ ЖЕ синтетической
 * последовательности, что и номера частей раскладки: игра спрашивает свои части
 * ПО ИМЕНИ (GetActivityPartsNumber), поэтому лишний номер посередине ничего не
 * сдвигает — так же уже сделана подпись чекбокса в act_build_part.
 */
void PE_CreateMessageWindow(int parts_no, int text_parts_no, int mark_parts_no)
{
	struct parts *parts = parts_get(parts_no);
	// XSYS4_MW_TRACE: отличает «игра переиспользует окно» от «строит заново».
	if (getenv("XSYS4_MW_TRACE"))
		NOTICE("MWCREATE окно=%d текст=%d маркер=%d (было mw=%d)",
		       parts_no, text_parts_no, mark_parts_no, parts->mw ? 1 : 0);
	if (parts->mw)
		parts_message_window_free(parts->mw);
	parts->mw = parts_message_window_alloc();
	parts->mw->text_parts_no = text_parts_no;
	parts->mw->mark_parts_no = mark_parts_no;

	PE_SetParentPartsNumber(text_parts_no, parts_no);
	PE_SetZ(text_parts_no, 1);
	PE_SetShow(text_parts_no, true);

	// Значок ожидания клика по умолчанию скрыт: его зажигает сама игра
	// (message::detail::キー待ちマーク表示設定 -> SetKeyWaitShow), когда реплика
	// дочитана.
	PE_SetParentPartsNumber(mark_parts_no, parts_no);
	PE_SetZ(mark_parts_no, 2);
	PE_SetShow(mark_parts_no, false);
}

// --------------------------------------------------- キー待ちマーク (мигалка клика)

/*
 * `キー待ちマーク` — значок «жду клика» в углу окна. Раскладка задаёт его
 * подузлом окна (`ＣＧ名`, `フラット名`, `ループＣＧ開始番号`, `ループＣＧ枚数`,
 * `ループＣＧ切り替え時間`, `座標`), а ПОКАЗОМ управляет сама игра
 * (`message::detail::キー待ちマーク表示設定` -> SetKeyWaitShow), поэтому угадывать
 * правило видимости не нужно: движок только хранит и рисует.
 *
 * Это отдельная часть-потомок, а не второе состояние окна: значок живёт ПОВЕРХ
 * фона одновременно с текстом и мигает своей циклической анимацией.
 */
static int mw_mark_parts(possibly_unused int parts_no, struct parts_message_window *mw)
{
	return mw->mark_parts_no;
}

void PE_SetKeyWaitCGName(int parts_no, struct string *name, int start_no, int nr_cg, int time_per_cg)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetKeyWaitCGName");
	if (!mw)
		return;
	if (mw->mark_cg_name)
		free_string(mw->mark_cg_name);
	mw->mark_cg_name = string_ref(name);
	mw->mark_start_no = start_no;
	mw->mark_nr_cg = nr_cg;
	mw->mark_time_per_cg = time_per_cg;

	int no = mw_mark_parts(parts_no, mw);
	if (!name || !name->size)
		return;
	// `ループＣＧ枚数` = 0 — обычный одиночный CG, иначе циклическая анимация из
	// nr_cg кадров с шагом time_per_cg (у キー待ちマーク／Ａ в архиве лежат кадры
	// `…_01` … `…_04`, у `／Ｂ` — только базовый).
	if (nr_cg > 0)
		PE_SetLoopCG(no, name, start_no, nr_cg, time_per_cg, 1);
	else
		PE_SetPartsCG(no, name, 0, 1);
	parts_message_window_relayout(parts_get(parts_no));
}

void PE_GetKeyWaitCGName(int parts_no, struct string **name, int *start_no, int *nr_cg,
                         int *time_per_cg)
{
	struct parts_message_window *mw = mw_get(parts_no);
	if (name) {
		if (*name)
			free_string(*name);
		*name = string_ref(mw && mw->mark_cg_name ? mw->mark_cg_name : &EMPTY_STRING);
	}
	if (start_no) *start_no = mw ? mw->mark_start_no : 0;
	if (nr_cg) *nr_cg = mw ? mw->mark_nr_cg : 0;
	if (time_per_cg) *time_per_cg = mw ? mw->mark_time_per_cg : 0;
}

void PE_SetKeyWaitFlatName(int parts_no, struct string *name)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetKeyWaitFlatName");
	if (!mw)
		return;
	if (mw->mark_flat_name)
		free_string(mw->mark_flat_name);
	mw->mark_flat_name = string_ref(name);
	if (name && name->size) {
		PE_SetPartsFlat(mw_mark_parts(parts_no, mw), name, 1);
		parts_message_window_relayout(parts_get(parts_no));
	}
}

struct string *PE_GetKeyWaitFlatName(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return string_ref(mw && mw->mark_flat_name ? mw->mark_flat_name : &EMPTY_STRING);
}

void PE_SetKeyWaitPos(int parts_no, int x, int y, int z)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetKeyWaitPos");
	if (!mw)
		return;
	mw->mark_pos = (Point) { x, y };
	mw->mark_z = z;
	if (mw->mark_parts_no >= 0) {
		PE_SetZ(mw->mark_parts_no, z ? z : 2);
		parts_message_window_relayout(parts_get(parts_no));
	}
}

int PE_GetKeyWaitPosX(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->mark_pos.x : 0;
}

int PE_GetKeyWaitPosY(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->mark_pos.y : 0;
}

int PE_GetKeyWaitPosZ(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->mark_z : 0;
}

void PE_SetKeyWaitShow(int parts_no, bool show)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetKeyWaitShow");
	if (!mw)
		return;
	mw->mark_show = show;
	PE_SetShow(mw_mark_parts(parts_no, mw), show);
}

bool PE_IsKeyWaitShow(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->mark_show : false;
}

// ------------------------------------------------------------- HLL: активность

void PE_SetMessageWindowActive(int parts_no, bool active)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowActive");
	if (!mw)
		return;
	mw->active = active;
	mw_apply_active(parts_no, mw);
}

void PE_SetMessageWindowInactiveMultipleColor(int parts_no, int r, int g, int b)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowInactiveMultipleColor");
	if (!mw)
		return;
	mw->inactive_multiply_color = (SDL_Color) { r, g, b, 255 };
	mw_apply_active(parts_no, mw);
}

int PE_GetMessageWindowInactiveMultipleColorR(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->inactive_multiply_color.r : 255;
}

int PE_GetMessageWindowInactiveMultipleColorG(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->inactive_multiply_color.g : 255;
}

int PE_GetMessageWindowInactiveMultipleColorB(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->inactive_multiply_color.b : 255;
}

// ------------------------------------------------------------------- HLL: фон

void PE_SetMessageWindowCGName(int parts_no, struct string *name)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowCGName");
	if (!mw)
		return;
	if (mw->cg_name)
		free_string(mw->cg_name);
	mw->cg_name = string_ref(name);
	mw_apply_background(parts_no, mw);
}

struct string *PE_GetMessageWindowCGName(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return string_ref(mw && mw->cg_name ? mw->cg_name : &EMPTY_STRING);
}

void PE_SetMessageWindowFlatName(int parts_no, struct string *name)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowFlatName");
	if (!mw)
		return;
	if (mw->flat_name)
		free_string(mw->flat_name);
	mw->flat_name = string_ref(name);
	mw_apply_background(parts_no, mw);
}

struct string *PE_GetMessageWindowFlatName(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return string_ref(mw && mw->flat_name ? mw->flat_name : &EMPTY_STRING);
}

void PE_SetMessageWindowFlatShowWaitFrameNumber(int parts_no, int frame)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowFlatShowWaitFrameNumber");
	if (!mw)
		return;
	mw->flat_show_wait_frames = frame;
}

int PE_GetMessageWindowFlatShowWaitFrameNumber(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->flat_show_wait_frames : 0;
}

/*
 * Кадр «окно открылось» у flat-фона: игра ждёт, пока анимация появления
 * доиграет до `フラット表示待ちフレーム数`, и только потом печатает реплику
 * (message::detail::CMessageWindow@IsOverFlatWaitFrame/BackFlatBeginFrame/
 * StepFlatFinalFrame). У Dohna フラット名 пусто во ВСЕХ 195 раскладках, то есть
 * ни одно окно игры не идёт этим путём; чтобы не выдумывать семантику покадровой
 * перемотки flat, отвечаем «ждать нечего» и предупреждаем один раз, если flat
 * всё-таки задан.
 */
static bool mw_flat_unimplemented(int parts_no, const char *fn)
{
	struct parts_message_window *mw = mw_get(parts_no);
	if (!mw || !mw->flat_name || !mw->flat_name->size)
		return false;
	static bool warned = false;
	if (!warned) {
		warned = true;
		WARNING("%s: покадровое ожидание flat-фона окна не реализовано", fn);
	}
	return true;
}

bool PE_IsOverMessageWindowFlatShowWaitFrame(int parts_no)
{
	mw_flat_unimplemented(parts_no, "IsOverMessageWindowFlatShowWaitFrame");
	return true;
}

bool PE_BackMessageWindowFlatBeginFrame(int parts_no)
{
	return !mw_flat_unimplemented(parts_no, "BackMessageWindowFlatBeginFrame");
}

bool PE_StepMessageWindowFlatFinalFrame(int parts_no)
{
	return !mw_flat_unimplemented(parts_no, "StepMessageWindowFlatFinalFrame");
}

// ----------------------------------------------------------------- HLL: текст

static int mw_global_by_name(const char *u8)
{
	char *sjis = utf2sjis(u8, strlen(u8));
	int no = ain_get_global(ain, sjis);
	free(sjis);
	return no;
}

/*
 * Включена ли 既読-перекраска.
 *
 * Настройка живёт ПО-РАЗНОМУ. У части игр это отдельный глобал
 * `g_既読メッセージ色変更`, а у Dohna — ПОЛЕ структуры конфига:
 * `g_AFConfig` типа `CASConfigData`, поле `既読メッセージ色変更` (в байткоде
 * видно только `.STRUCTASSIGN CASConfigData 既読メッセージ色変更 1` в
 * конструкторе дефолтов, отдельного глобала с таким именем нет вовсе).
 * Пока искали лишь глобал, режим был выключен ВСЕГДА: замер `XSYS4_MW_TRACE`
 * на прологе давал `read=1 mode=0` — флаг «прочитано» стоял, а перекраска не
 * включалась, и уже читанные реплики шли базовым белым.
 *
 * Оба места ищем по одному разу: имя глобала и слот поля кэшируются.
 */
static bool mw_read_color_mode(void)
{
	static int varno = -2;      // отдельный глобал, если он есть
	static int cfg_varno = -2;  // g_AFConfig
	static int cfg_slot = -1;   // слот поля внутри него

	if (varno == -2)
		varno = mw_global_by_name("g_既読メッセージ色変更");
	if (varno >= 0)
		return global_get(varno).i != 0;

	if (cfg_varno == -2) {
		cfg_varno = mw_global_by_name("g_AFConfig");
		if (cfg_varno >= 0) {
			struct page *page = heap_get_page(global_get(cfg_varno).i);
			if (page && page->type == STRUCT_PAGE && page->index >= 0
					&& page->index < ain->nr_structures) {
				struct ain_struct *s = &ain->structures[page->index];
				const char *u8 = "既読メッセージ色変更";
				char *sjis = utf2sjis(u8, strlen(u8));
				for (int i = 0; i < s->nr_members; i++) {
					if (!strcmp(s->members[i].name, sjis)) {
						cfg_slot = i;
						break;
					}
				}
				free(sjis);
			}
		}
	}
	if (cfg_varno < 0 || cfg_slot < 0)
		return false;
	struct page *page = heap_get_page(global_get(cfg_varno).i);
	if (!page || cfg_slot >= page->nr_vars)
		return false;
	return page->values[cfg_slot].i != 0;
}

/*
 * 既読-перекраска: уже прочитанная реплика рисуется цветом Ｅ＿既読メッセージ色
 * из главного .ex. У оригинала это делает САМ ДВИЖОК: у message::detail::
 * GetReadMessageTextColor нет ни одного вызова в байткоде — это контракт SDK,
 * и вся игровая логика вокруг g_既読メッセージ касается только скип-режима.
 * Флаг «прочитано» — база MsgSkip: игра сама ставит его после проявления
 * реплики (CReadMessageTextManager → MsgSkip.SetFlag), поэтому при первом
 * показе реплика ещё «не прочитана» и идёт базовым цветом раскладки.
 * Цвет ставится ПЕРЕД PE_SetText: PE_SetPartsFontColor меняет стиль текстовой
 * части насовсем, так что для непрочитанных базовый цвет возвращаем явно.
 */
static void mw_apply_read_color(struct parts_message_window *mw)
{
	int tp = mw_text_parts(mw);
	if (tp < 0)
		return;
	int r = mw->text_color.r, g = mw->text_color.g, b = mw->text_color.b;
	if (getenv("XSYS4_MW_TRACE")) {
		// Три гейта перекраски по отдельности: без этого не отличить «реплика не
		// числится прочитанной» от «режим выключен в конфиге».
		NOTICE("READCOLOR part=%d msg=%d read=%d mode=%d базовый=%d,%d,%d",
		       tp, mw->msg_num,
		       mw->msg_num >= 0 ? (int)msgskip_message_is_read(mw->msg_num) : -1,
		       (int)mw_read_color_mode(), r, g, b);
	}
	if (mw->msg_num >= 0 && msgskip_message_is_read(mw->msg_num)
	    && mw_read_color_mode()) {
		// Дефолты — как у config::detail::GetReadTextColor.
		r = 255; g = 127; b = 127;
		mainex_list_int_get("Ｅ＿既読メッセージ色", 0, &r);
		mainex_list_int_get("Ｅ＿既読メッセージ色", 1, &g);
		mainex_list_int_get("Ｅ＿既読メッセージ色", 2, &b);
		// XSYS4_READCOLOR=r,g,b — подменить цвет прочитанного (замер: виден ли
		// вообще эффект перекраски на экране).
		const char *e = getenv("XSYS4_READCOLOR");
		if (e)
			sscanf(e, "%d,%d,%d", &r, &g, &b);
		if (getenv("XSYS4_MW_TRACE"))
			NOTICE("READCOLOR применяю %d,%d,%d к части %d", r, g, b, tp);
	}
	PE_SetPartsFontColor(tp, r, g, b, 1);
}

void PE_SetMessageWindowText(int parts_no, struct string *text, int msg_num,
                             struct string *func_name, int ver, int step)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowText");
	if (!mw)
		return;

	mw->msg_num = msg_num;
	if (mw->msg_func_name)
		free_string(mw->msg_func_name);
	mw->msg_func_name = func_name ? string_ref(func_name) : NULL;
	mw->msg_ver = ver;
	mw->msg_step = step;
	if (getenv("XSYS4_MW_TRACE"))
		NOTICE("MWTEXT part=%d msg=%d ver=%d step=%d raw='%s'", parts_no, msg_num,
		       ver, step, text ? display_sjis0(text->text) : "(nil)");

	mw_apply_read_color(mw);
	struct string *plain = mw_strip_markup(text);
	PE_SetText(mw_text_parts(mw), plain, 1);
	free_string(plain);

	/*
	 * `字速度` (скорость посимвольного проявления) пока не исполняется: текст
	 * появляется целиком. Это НЕ тихий дефолт — режим мгновенного вывода у игры
	 * законный (тег `${time 0}` и скип), но раскладки задают 23, поэтому
	 * предупреждаем, что видимое поведение отличается от оригинального.
	 */
	if (mw->text_speed > 0) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("メッセージウィンドウ: 字速度 %d не исполняется, реплика "
			        "показывается целиком", mw->text_speed);
		}
	}
	mw->text_fixed = true;
}

struct string *PE_GetMessageWindowText(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	if (!mw || mw->text_parts_no < 0)
		return string_ref(&EMPTY_STRING);
	return PE_GetTextPartsText(mw->text_parts_no, 1);
}

void PE_FixMessageWindowText(int parts_no)
{
	struct parts_message_window *mw = mw_require(parts_no, "FixMessageWindowText");
	if (!mw)
		return;
	mw->text_fixed = true;
}

bool PE_IsFixedMessageWindowText(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->text_fixed : true;
}

void PE_SetMessageWindowTextArea(int parts_no, int x, int y, int w, int h)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowTextArea");
	if (!mw)
		return;
	mw->text_area = (Rectangle) { x, y, w, h };
	mw_apply_text_area(parts_no, mw);
}

void PE_GetMessageWindowTextArea(int parts_no, int *x, int *y, int *w, int *h)
{
	struct parts_message_window *mw = mw_get(parts_no);
	if (x) *x = mw ? mw->text_area.x : 0;
	if (y) *y = mw ? mw->text_area.y : 0;
	if (w) *w = mw ? mw->text_area.w : 0;
	if (h) *h = mw ? mw->text_area.h : 0;
}

/*
 * `テキスト位置` — выравнивание текста внутри テキストエリア по той же сетке 1-9,
 * что и 原点座標モード частей (1 = верх-лево … 5 = центр … 9 = низ-право). Во всех
 * 195 раскладках Dohna значение равно 1, и игра переустанавливает его тем же 1
 * (message::detail::CMessageTextView@CreateDrawChar), поэтому реализовано только
 * оно; на любое другое — одноразовый WARNING вместо выдуманного выравнивания.
 */
void PE_SetMessageWindowTextOriginPosMode(int parts_no, int mode)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowTextOriginPosMode");
	if (!mw)
		return;
	mw->text_origin_pos_mode = mode;
	if (mode != 1) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("メッセージウィンドウ: テキスト位置 %d не реализовано "
			        "(текст выравнивается по верхнему левому углу области)", mode);
		}
	}
}

int PE_GetMessageWindowTextOriginPosMode(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->text_origin_pos_mode : 0;
}

void PE_SetMessageWindowTextFont(int parts_no, int type, int size, int r, int g, int b,
                                 float bold_weight, int edge_r, int edge_g, int edge_b,
                                 float edge_weight)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowTextFont");
	if (!mw)
		return;
	mw->text_color = (SDL_Color) { r, g, b, 255 };
	PE_SetFont(mw_text_parts(mw), type, size, r, g, b, bold_weight,
	           edge_r, edge_g, edge_b, edge_weight, 1);
}

void PE_GetMessageWindowTextFont(int parts_no, int *type, int *size, int *r, int *g, int *b,
                                 float *bold_weight, int *edge_r, int *edge_g, int *edge_b,
                                 float *edge_weight)
{
	struct parts_message_window *mw = mw_get(parts_no);
	if (!mw || mw->text_parts_no < 0)
		return;
	PE_GetTextFontProps(mw->text_parts_no, 1, type, size, r, g, b, bold_weight,
	                    edge_weight, edge_r, edge_g, edge_b);
}

void PE_SetMessageWindowTextSpeed(int parts_no, int speed)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowTextSpeed");
	if (!mw)
		return;
	mw->text_speed = speed;
}

int PE_GetMessageWindowTextSpeed(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->text_speed : 0;
}

void PE_SetMessageWindowTextSpace(int parts_no, int letter_space, int line_space)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowTextSpace");
	if (!mw)
		return;
	int tno = mw_text_parts(mw);
	PE_SetTextCharSpace(tno, letter_space, 1);
	PE_SetTextLineSpace(tno, line_space, 1);
}

void PE_GetMessageWindowTextSpace(int parts_no, int *letter_space, int *line_space)
{
	struct parts_message_window *mw = mw_get(parts_no);
	if (!mw || mw->text_parts_no < 0) {
		if (letter_space) *letter_space = 0;
		if (line_space) *line_space = 0;
		return;
	}
	if (letter_space) *letter_space = PE_GetTextCharSpace(mw->text_parts_no, 1);
	if (line_space) *line_space = PE_GetTextLineSpace(mw->text_parts_no, 1);
}

// ------------------------------------------------------------------ HLL: руби

/*
 * Руби (фуригана) хранится, но не рисуется: во всей строковой секции .ain Dohna
 * НЕТ ни одного тега руби (единственные формы разметки — `${font …}` и
 * `${time …}`), то есть чем именно реплика помечает чтение над иероглифом,
 * доказательств нет. Геттеры игрой читаются, поэтому значения обязаны храниться
 * по-настоящему.
 */
void PE_SetMessageWindowRubyFont(int parts_no, int type, int size, int r, int g, int b,
                                 float bold_weight, int edge_r, int edge_g, int edge_b,
                                 float edge_weight)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowRubyFont");
	if (!mw)
		return;
	mw->ruby_ts.face = type;
	mw->ruby_ts.size = size;
	mw->ruby_ts.color = (SDL_Color) { r, g, b, 255 };
	mw->ruby_ts.weight = bold_weight * 1000;
	mw->ruby_ts.edge_color = (SDL_Color) { edge_r, edge_g, edge_b, 255 };
	text_style_set_edge_width(&mw->ruby_ts, edge_weight);
}

void PE_GetMessageWindowRubyFont(int parts_no, int *type, int *size, int *r, int *g, int *b,
                                 float *bold_weight, int *edge_r, int *edge_g, int *edge_b,
                                 float *edge_weight)
{
	struct parts_message_window *mw = mw_get(parts_no);
	if (!mw)
		return;
	if (type) *type = mw->ruby_ts.face;
	if (size) *size = (int)mw->ruby_ts.size;
	if (r) *r = mw->ruby_ts.color.r;
	if (g) *g = mw->ruby_ts.color.g;
	if (b) *b = mw->ruby_ts.color.b;
	if (bold_weight) *bold_weight = mw->ruby_ts.weight / 1000.0f;
	if (edge_r) *edge_r = mw->ruby_ts.edge_color.r;
	if (edge_g) *edge_g = mw->ruby_ts.edge_color.g;
	if (edge_b) *edge_b = mw->ruby_ts.edge_color.b;
	if (edge_weight) *edge_weight = mw->ruby_ts.edge_left;
}

void PE_SetMessageWindowRubyCharSpace(int parts_no, int space)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowRubyCharSpace");
	if (mw)
		mw->ruby_char_space = space;
}

int PE_GetMessageWindowRubyCharSpace(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->ruby_char_space : 0;
}

void PE_SetMessageWindowRubyLineSpace(int parts_no, int space)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetMessageWindowRubyLineSpace");
	if (mw)
		mw->ruby_line_space = space;
}

int PE_GetMessageWindowRubyLineSpace(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->ruby_line_space : 0;
}

// -------------------------------------------------------------- HLL: перенос

/*
 * `折り返し` — автоперенос по ширине テキストエリア. Ни одна из 195 раскладок
 * Dohna его не включает, и обе функции не вызываются игрой ни разу (тул xscan по
 * CALLHLL: 0 сайтов), то есть реплики приходят с авторскими переносами `\n` —
 * ровно как у Tsumamigui 3 (см. память system4-ain-linebreaks). Значение
 * хранится; на попытку включить — одноразовый WARNING, потому что переносить
 * движок пока не умеет.
 */
void PE_SetEnableMessageWindowTextWrapping(int parts_no, bool enable)
{
	struct parts_message_window *mw = mw_require(parts_no, "SetEnableMessageWindowTextWrapping");
	if (!mw)
		return;
	mw->text_wrapping = enable;
	if (enable) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("メッセージウィンドウ: 折り返し включён, но автоперенос по "
			        "ширине текстовой области не реализован");
		}
	}
}

bool PE_IsEnableMessageWindowTextWrapping(int parts_no)
{
	struct parts_message_window *mw = mw_get(parts_no);
	return mw ? mw->text_wrapping : false;
}
