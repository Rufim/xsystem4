/* Copyright (C) 2026 Rufim
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
 * HLL-библиотека `String` игр Ixseal (System 4 v14: Dohna Dohna, Healing
 * Touch). Это методы, вызываемые НА строке: первый аргумент — `ref string`
 * (сама строка), поэтому C-функции получают `struct string **`. Старые игры
 * этой библиотеки не линкуют (у них строковые операции — опкоды VM и `vmString`).
 *
 * Строки движка — CP932/SJIS, поэтому индексы и длина считаются в СИМВОЛАХ
 * (sjis_*), а не в байтах; байтовую длину отдаёт отдельная LengthByte.
 *
 * Перегрузки (GetPart/PadLeft/PadRight/Trim/TrimStart/TrimEnd) различаются
 * только числом аргументов, а линковка HLL сопоставляет функции по ИМЕНИ —
 * все перегрузки получают один и тот же C-указатель. Нужное число аргументов
 * берём в рантайме из `hll_current_nr_args` (ffi.c выставляет его перед
 * вызовом): необъявленный в cif параметр читать нельзя.
 *
 * Регулярочные функции идут через `src/regex_ecma.cpp` (std::regex, ECMAScript):
 * игра компилировалась поверх той же библиотеки, и её шаблоны используют
 * ленивые квантификаторы, которых нет в POSIX ERE. Сопоставление выполняется в
 * UTF-8 (см. sjis_to_utf8_tmp) — в SJIS второй байт символа может совпасть,
 * например, с `[`, и шаблон бы «резал» строку внутри символа.
 *
 * `Split(ref string, string separators, int options)` режет по НАБОРУ
 * символов-разделителей, а не по строке-разделителю целиком: один из сайтов
 * передаёт `"[]|"` (три символа), который как единый разделитель не встретился
 * бы никогда. Третий аргумент — БИТОВАЯ МАСКА, а не булев флаг: у Dohna он
 * принимает 0 (40 сайтов), 1 (2 сайта) и 2 (1 сайт). Бит 0 — «убрать пустые
 * части»: сайт переноса строк бэклога (`AddWrappingLog`) зовёт
 * `Split(text, " ", 1)`, т.е. режет на слова, и пустые куски от подряд идущих
 * пробелов там недопустимы.
 *
 * ★Значение 2 пустые части ОСТАВЛЯЕТ (бит 0 не взведён) — как `StringSplitOptions`
 * .NET, где 1 = RemoveEmptyEntries, 2 = TrimEntries. Пока любое ненулевое
 * значение означало «убрать пустые», ломался единственный сайт с двойкой —
 * `Footer@SetButtonText`: подписи кнопок футера приходят одной строкой через
 * запятую и позиционно («Save,Load,Items,Next→» у дома, `",,,Back"` у магазина).
 * Со схлопнутыми пустыми `",,,Back"` давало ["Back"], и подпись садилась на
 * кнопку 0 — «Back» рисовался СЛЕВА вместо правого края, а кнопки 0–2 не
 * получали `Show(false)` и оставались на экране лишними плашками.
 */

#include <stdlib.h>
#include <string.h>

#include "system4/string.h"
#include "system4/utfsjis.h"

#include "hll.h"
#include "vm/heap.h"
#include "vm/page.h"

// src/regex_ecma.cpp
typedef void (*xs4_regex_sink)(void *user, const char *utf8, size_t len);
int xs4_regex_search(const char *subject, const char *pattern);
int xs4_regex_match(const char *subject, const char *pattern);
int xs4_regex_search_groups(const char *subject, const char *pattern,
			    xs4_regex_sink sink, void *user);
int xs4_regex_search_all(const char *subject, const char *pattern,
			 xs4_regex_sink sink, void *user);
int xs4_regex_replace(const char *subject, const char *pattern, const char *repl,
		      xs4_regex_sink sink, void *user);

static struct string *empty(void)
{
	return string_ref(&EMPTY_STRING);
}

static struct string *self_or_empty(struct string **s)
{
	return (s && *s) ? *s : &EMPTY_STRING;
}

static int String_ToInt(struct string **s)
{
	return string_to_integer(self_or_empty(s));
}

static float String_ToFloat(struct string **s)
{
	return strtof(self_or_empty(s)->text, NULL);
}

static int String_Length(struct string **s)
{
	return sjis_count_char(self_or_empty(s)->text);
}

static int String_LengthByte(struct string **s)
{
	return self_or_empty(s)->size;
}

static bool String_Empty(struct string **s)
{
	return self_or_empty(s)->size == 0;
}

static void String_PushBack(struct string **s, int c)
{
	if (s && *s)
		string_push_back(s, c);
}

static void String_PopBack(struct string **s)
{
	if (s && *s && (*s)->size)
		string_pop_back(s);
}

// Удалить `count` символов начиная с символа `index`.
static void String_Erase(struct string **s, int index, int count)
{
	if (!s || !*s)
		return;
	for (int i = 0; i < count; i++)
		string_erase(s, index);
}

static void String_Insert(struct string **s, int index, struct string *t)
{
	if (!s || !*s || !t)
		return;
	int len = sjis_count_char((*s)->text);
	if (index < 0)
		index = 0;
	if (index > len)
		index = len;
	struct string *head = string_copy(*s, 0, index);
	struct string *tail = string_copy(*s, index, len - index);
	string_append(&head, t);
	string_append(&head, tail);
	free_string(tail);
	free_string(*s);
	*s = head;
}

static int String_Find(struct string **s, struct string *t)
{
	if (!t)
		return -1;
	return string_find(self_or_empty(s), t);
}

// Индекс (в символах) ПОСЛЕДНЕГО вхождения.
static int String_FindLast(struct string **s, struct string *t)
{
	struct string *hay = self_or_empty(s);
	if (!t || !t->size)
		return -1;
	int found = -1;
	for (int i = 0, c = 0; i + t->size <= hay->size; c++) {
		if (!memcmp(hay->text + i, t->text, t->size))
			found = c;
		i += SJIS_2BYTE(hay->text[i]) ? 2 : 1;
	}
	return found;
}

static bool String_Contains(struct string **s, struct string *t)
{
	return String_Find(s, t) >= 0;
}

static bool String_StartsWith(struct string **s, struct string *t)
{
	struct string *hay = self_or_empty(s);
	if (!t)
		return false;
	return t->size <= hay->size && !memcmp(hay->text, t->text, t->size);
}

static bool String_EndsWith(struct string **s, struct string *t)
{
	struct string *hay = self_or_empty(s);
	if (!t)
		return false;
	return t->size <= hay->size
		&& !memcmp(hay->text + hay->size - t->size, t->text, t->size);
}

// Замена ВСЕХ вхождений; сама строка не меняется, возвращается результат.
static struct string *String_Replace(struct string **s, struct string *from, struct string *to)
{
	struct string *hay = self_or_empty(s);
	if (!from || !from->size)
		return string_dup(hay);

	struct string *out = make_string("", 0);
	int i = 0;
	while (i < hay->size) {
		if (i + from->size <= hay->size && !memcmp(hay->text + i, from->text, from->size)) {
			if (to)
				string_append(&out, to);
			i += from->size;
			continue;
		}
		int bytes = SJIS_2BYTE(hay->text[i]) ? 2 : 1;
		string_append_cstr(&out, hay->text + i, bytes);
		i += bytes;
	}
	return out;
}

// GetPart(index) — от символа index до конца; GetPart(index, len) — len символов.
static struct string *String_GetPart(struct string **s, int index, int len)
{
	struct string *src = self_or_empty(s);
	int total = sjis_count_char(src->text);
	if (hll_current_nr_args < 3)
		len = total - index;
	if (index < 0)
		index = 0;
	if (len > total - index)
		len = total - index;
	if (len <= 0)
		return empty();
	return string_copy(src, index, len);
}

// Дополнить до `width` СИМВОЛОВ; символ-заполнитель по умолчанию — пробел.
static struct string *pad(struct string *src, int width, int ch, bool left)
{
	int len = sjis_count_char(src->text);
	if (len >= width)
		return string_dup(src);

	struct string *fill = make_string("", 0);
	for (int i = len; i < width; i++)
		string_push_back(&fill, ch);

	struct string *out;
	if (left) {
		out = fill;
		string_append(&out, src);
	} else {
		out = string_dup(src);
		string_append(&out, fill);
		free_string(fill);
	}
	return out;
}

static struct string *String_PadLeft(struct string **s, int width, int ch)
{
	if (hll_current_nr_args < 3)
		ch = ' ';
	return pad(self_or_empty(s), width, ch, true);
}

static struct string *String_PadRight(struct string **s, int width, int ch)
{
	if (hll_current_nr_args < 3)
		ch = ' ';
	return pad(self_or_empty(s), width, ch, false);
}

// Регистр меняем только у однобайтовых (hankaku) символов: двухбайтовые
// SJIS-последовательности копируются как есть.
static struct string *map_case(struct string *src, bool upper)
{
	struct string *out = string_dup(src);
	for (int i = 0; i < out->size; i++) {
		if (SJIS_2BYTE(out->text[i])) {
			i++;
			continue;
		}
		char c = out->text[i];
		if (upper && c >= 'a' && c <= 'z')
			out->text[i] = c - 'a' + 'A';
		else if (!upper && c >= 'A' && c <= 'Z')
			out->text[i] = c - 'A' + 'a';
	}
	return out;
}

static struct string *String_ToLower(struct string **s) { return map_case(self_or_empty(s), false); }
static struct string *String_ToUpper(struct string **s) { return map_case(self_or_empty(s), true); }

static bool is_trimmed(const struct string *set, char c)
{
	if (!set)
		return c == ' ' || c == '\t' || c == '\r' || c == '\n';
	return memchr(set->text, c, set->size) != NULL;
}

// Обрезаем только однобайтовые символы из набора (по умолчанию — пробельные),
// поэтому двухбайтовый SJIS-текст по краям всегда остаётся нетронутым.
static struct string *trim(struct string *src, const struct string *set, bool start, bool end)
{
	int a = 0, b = src->size;
	if (start) {
		while (a < b && !SJIS_2BYTE(src->text[a]) && is_trimmed(set, src->text[a]))
			a++;
	}
	if (end) {
		while (b > a && !SJIS_2BYTE(src->text[b-1]) && is_trimmed(set, src->text[b-1]))
			b--;
	}
	return make_string(src->text + a, b - a);
}

static const struct string *trim_set(struct string *set)
{
	return hll_current_nr_args >= 2 ? set : NULL;
}

static struct string *String_Trim(struct string **s, struct string *set)
{
	return trim(self_or_empty(s), trim_set(set), true, true);
}

static struct string *String_TrimStart(struct string **s, struct string *set)
{
	return trim(self_or_empty(s), trim_set(set), true, false);
}

static struct string *String_TrimEnd(struct string **s, struct string *set)
{
	return trim(self_or_empty(s), trim_set(set), false, true);
}


/*
 * array<string> Split(ref string self, string separators, bool containSeparator)
 *
 * Разделители — НАБОР символов (SJIS-aware: двухбайтный символ считается одним
 * разделителем); имя параметра в .ain так и стоит — `separators`. Возврат типа
 * 79 уходит на стек как есть, поэтому отдаём heap-СЛОТ страницы, а не указатель
 * (сырой page* прочитался бы как индекс слота); вызывающий владеет слотом и
 * освобождает его своим DELETE.
 *
 * ★ПУСТЫЕ КУСКИ НЕ ОТДАЮТСЯ — подряд идущие разделители съедаются, как у
 * strtok. Прежде третий аргумент читался как биты «removeEmpty|trimEntries»
 * (догадка), и при нуле пустые куски попадали в результат. Замер, который это
 * опроверг: Haha Ranman пакует поля сейва в одну строку через `@savecode@`
 * (`SetSaveFileComment`) и разбирает её этим же Split с третьим аргументом 0,
 * ожидая РОВНО 6 элементов (`GetSaveFileComment` иначе возвращает false). По
 * набору символов {@,s,a,v,e,c,o,d} без пустых кусков выходит ровно 6 — с
 * пустыми выходило больше сорока, игра отбрасывала данные, и у всех слотов
 * оставался незаполненный «score»: LOAD LATEST всегда брал первый слот, а
 * заметки к сейвам не отображались.
 *
 * Третий аргумент — `containSeparator`: разделители возвращаются как отдельные
 * элементы (у Dohna так зовут `Split(" ", 1)` в разборе параметров motion).
 */
static bool is_separator(const char *sep, size_t sep_size, const char *p, int len)
{
	for (size_t i = 0; i < sep_size; ) {
		int slen = SJIS_2BYTE(sep[i]) ? 2 : 1;
		if (slen == len && !memcmp(sep + i, p, len))
			return true;
		i += slen;
	}
	return false;
}

static int String_Split(struct string **s, struct string *sep, int contain_separator)
{
	struct string *src = self_or_empty(s);
	int slot = heap_alloc_slot(VM_PAGE);
	union vm_value dim = { .i = 0 };
	struct page *out = alloc_array(1, &dim, AIN_ARRAY_STRING, 0, false);

	int start = 0, i = 0;
	while (i <= src->size) {
		int clen = (i < src->size && SJIS_2BYTE(src->text[i])) ? 2 : 1;
		bool at_end = i >= src->size;
		if (!at_end && (!sep || !is_separator(sep->text, sep->size, src->text + i, clen))) {
			i += clen;
			continue;
		}
		// XSYS4_SPLIT_KEEP_EMPTY=1 — вернуть прежнее поведение (пустые куски
		// попадают в результат): откат для замеров, если где-то игра ждёт
		// позиционный список, а не сжатый.
		static int keep_empty = -1;
		if (keep_empty < 0)
			keep_empty = getenv("XSYS4_SPLIT_KEEP_EMPTY") ? 1 : 0;
		if (i > start || keep_empty) {
			struct string *part = make_string(src->text + start, i - start);
			union vm_value v = { .i = heap_alloc_string(part) };
			out = array_pushback_n(out, &v, 1, AIN_ARRAY_STRING, 0);
		}
		if (at_end)
			break;
		if (contain_separator) {
			struct string *part = make_string(src->text + i, clen);
			union vm_value v = { .i = heap_alloc_string(part) };
			out = array_pushback_n(out, &v, 1, AIN_ARRAY_STRING, 0);
		}
		i += clen;
		start = i;
	}
	heap_set_page(slot, out);
	return slot;
}

/*
 * --- Регулярные выражения (см. src/regex_ecma.cpp) ---
 *
 * Сопоставление идёт в UTF-8: SJIS небезопасен, потому что второй байт символа
 * может совпасть с ASCII-метасимволом шаблона (например `[` = 0x5B).
 */
static char *sjis_to_utf8_tmp(struct string *s)
{
	return sjis2utf(s ? s->text : "", s ? s->size : 0);
}

// Приёмник совпадений: конвертирует UTF-8 обратно в SJIS и добавляет элемент
// в generic-массив `array<string>` вызывающего.
struct match_sink_ctx {
	struct page **list;
};

static void match_to_array(void *user, const char *utf8, size_t len)
{
	struct match_sink_ctx *ctx = user;
	char *sjis = utf2sjis(utf8, len);
	struct string *s = make_string(sjis, strlen(sjis));
	free(sjis);
	union vm_value v = { .i = heap_alloc_string(s) };
	// Массив владеет своим счётчиком ссылок на элемент-строку (как Array.PushBack).
	*ctx->list = array_pushback_n(*ctx->list, &v, 1,
				      (*ctx->list && (*ctx->list)->type == ARRAY_PAGE)
					      ? (*ctx->list)->a_type : AIN_ARRAY_STRING,
				      0);
}

/*
 * Форма с `matchList` (3 аргумента) в Dohna и Healing Touch НЕ вызывается ни
 * разу — все сайты Search/Match идут по 2-арг. форме. Состав списка поэтому
 * ЭКСТРАПОЛИРОВАН из std::smatch (совпадение целиком, затем подгруппы) и при
 * первом реальном вызове сообщает о себе в лог, чтобы догадка не прошла тихо.
 */
static void warn_unproven_matchlist(const char *fname)
{
	static bool warned;
	if (!warned) {
		warned = true;
		WARNING("String.%s: форма с matchList не встречалась в байт-коде — "
			"состав списка (совпадение + подгруппы) взят по std::smatch, "
			"проверить по сайту вызова", fname);
	}
}

// bool Search(ref string self, string regex)
// bool Search(ref string self, wrap<array<string>> matchList, string regex)
// Перегрузки различаются только числом аргументов — берём hll_current_nr_args
// (у 2-арг. формы третьего параметра в cif НЕТ, читать его нельзя).
static bool String_Search(struct string **s, void *a1, struct string *a2)
{
	char *subject = sjis_to_utf8_tmp(self_or_empty(s));
	bool found;
	if (hll_current_nr_args >= 3) {
		warn_unproven_matchlist("Search");
		char *pattern = sjis_to_utf8_tmp(a2);
		struct match_sink_ctx ctx = { .list = (struct page **)a1 };
		int n = xs4_regex_search_groups(subject, pattern, match_to_array, &ctx);
		if (n < 0)
			WARNING("String.Search: некорректный шаблон");
		found = n > 0;
		free(pattern);
	} else {
		char *pattern = sjis_to_utf8_tmp((struct string *)a1);
		int r = xs4_regex_search(subject, pattern);
		if (r < 0)
			WARNING("String.Search: некорректный шаблон");
		found = r > 0;
		free(pattern);
	}
	free(subject);
	return found;
}

// bool SearchAll(ref string self, wrap<array<string>> matchList, string regex)
// В matchList уходят совпадения ЦЕЛИКОМ (единственный сайт —
// Motion::Parser@SplitParams — режет строку на токены).
static bool String_SearchAll(struct string **s, struct page **list, struct string *regex)
{
	char *subject = sjis_to_utf8_tmp(self_or_empty(s));
	char *pattern = sjis_to_utf8_tmp(regex);
	struct match_sink_ctx ctx = { .list = list };
	int n = xs4_regex_search_all(subject, pattern, match_to_array, &ctx);
	if (n < 0)
		WARNING("String.SearchAll: некорректный шаблон");
	free(subject);
	free(pattern);
	return n > 0;
}

// bool Match(ref string self, [wrap<array<string>> matchList,] string regex)
// В отличие от Search, шаблон должен покрыть строку ЦЕЛИКОМ (std::regex_match).
static bool String_Match(struct string **s, void *a1, struct string *a2)
{
	char *subject = sjis_to_utf8_tmp(self_or_empty(s));
	bool found;
	if (hll_current_nr_args >= 3) {
		warn_unproven_matchlist("Match");
		char *pattern = sjis_to_utf8_tmp(a2);
		// Группы нужны только при полном совпадении, поэтому сначала проверяем
		// анкоренный match, а состав групп берём тем же поиском.
		int m = xs4_regex_match(subject, pattern);
		if (m > 0) {
			struct match_sink_ctx ctx = { .list = (struct page **)a1 };
			xs4_regex_search_groups(subject, pattern, match_to_array, &ctx);
		} else if (m < 0) {
			WARNING("String.Match: некорректный шаблон");
		}
		found = m > 0;
		free(pattern);
	} else {
		char *pattern = sjis_to_utf8_tmp((struct string *)a1);
		int r = xs4_regex_match(subject, pattern);
		if (r < 0)
			WARNING("String.Match: некорректный шаблон");
		found = r > 0;
		free(pattern);
	}
	free(subject);
	return found;
}

static void replace_sink(void *user, const char *utf8, size_t len)
{
	struct string **out = user;
	char *sjis = utf2sjis(utf8, len);
	*out = make_string(sjis, strlen(sjis));
	free(sjis);
}

// string ReplaceRegex(ref string self, string regex, string replacement)
static struct string *String_ReplaceRegex(struct string **s, struct string *regex,
					  struct string *repl)
{
	char *subject = sjis_to_utf8_tmp(self_or_empty(s));
	char *pattern = sjis_to_utf8_tmp(regex);
	char *with = sjis_to_utf8_tmp(repl);
	struct string *out = NULL;
	if (xs4_regex_replace(subject, pattern, with, replace_sink, &out) < 0)
		WARNING("String.ReplaceRegex: некорректный шаблон");
	free(subject);
	free(pattern);
	free(with);
	return out ? out : string_ref(self_or_empty(s));
}

HLL_LIBRARY(String,
	    HLL_EXPORT(ToInt, String_ToInt),
	    HLL_EXPORT(ToFloat, String_ToFloat),
	    HLL_EXPORT(Length, String_Length),
	    HLL_EXPORT(LengthByte, String_LengthByte),
	    HLL_EXPORT(Empty, String_Empty),
	    HLL_EXPORT(PushBack, String_PushBack),
	    HLL_EXPORT(PopBack, String_PopBack),
	    HLL_EXPORT(Erase, String_Erase),
	    HLL_EXPORT(Insert, String_Insert),
	    HLL_EXPORT(Find, String_Find),
	    HLL_EXPORT(FindLast, String_FindLast),
	    HLL_EXPORT(Contains, String_Contains),
	    HLL_EXPORT(StartsWith, String_StartsWith),
	    HLL_EXPORT(EndsWith, String_EndsWith),
	    HLL_EXPORT(Replace, String_Replace),
	    HLL_EXPORT(GetPart, String_GetPart),
	    HLL_EXPORT(PadLeft, String_PadLeft),
	    HLL_EXPORT(PadRight, String_PadRight),
	    HLL_EXPORT(ToLower, String_ToLower),
	    HLL_EXPORT(ToUpper, String_ToUpper),
	    HLL_EXPORT(Trim, String_Trim),
	    HLL_EXPORT(TrimStart, String_TrimStart),
	    HLL_EXPORT(TrimEnd, String_TrimEnd),
	    HLL_EXPORT(Search, String_Search),
	    HLL_EXPORT(SearchAll, String_SearchAll),
	    HLL_EXPORT(Match, String_Match),
	    HLL_EXPORT(ReplaceRegex, String_ReplaceRegex),
	    HLL_EXPORT(Split, String_Split)
	    );
