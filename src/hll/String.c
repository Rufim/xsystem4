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
 * Регулярочные функции (Search/SearchAll/Match/ReplaceRegex) и Split НЕ
 * реализованы намеренно: их точная семантика по байт-коду пока не установлена,
 * а тихая заглушка врала бы вызывающему. Они остаются TODO и падают явно.
 */

#include <stdlib.h>
#include <string.h>

#include "system4/string.h"
#include "system4/utfsjis.h"

#include "hll.h"

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
	    // Семантика по байт-коду пока не установлена — падаем явно, а не врём.
	    HLL_TODO_EXPORT(Search, String_Search),
	    HLL_TODO_EXPORT(SearchAll, String_SearchAll),
	    HLL_TODO_EXPORT(Match, String_Match),
	    HLL_TODO_EXPORT(ReplaceRegex, String_ReplaceRegex),
	    HLL_TODO_EXPORT(Split, String_Split)
	    );
