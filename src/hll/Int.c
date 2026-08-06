/* Copyright (C) 2026 xsystem4 contributors
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
 * Библиотека `Int` (Ixseal, System 4 v14) — методы-расширения целого числа.
 * Объявление у Dohna Dohna:
 *   ToString          ret=12 (18,10)
 *   ToWideString      ret=12 (18,10)
 *   ToHexString       ret=12 (18,10)
 *   ToLowerHexString  ret=12 (18,10)
 *   ToCharacter       ret=12 (18)
 * Первый аргумент — `ref int` (само число, ffi отдаёт обычным указателем).
 *
 * Пара ToString/ToWideString повторяет классическую `vmString.IntToStringA`
 * (полуширинные цифры) и `vmString.IntToString` (полноширинные, 全角): «Wide»
 * в имени и есть zenkaku. Второй аргумент — число знаков (как `figure` у
 * `vmString.IntToStringEx`); ДОКАЗАНО только значение 0: все 47 сайтов вызова
 * в dohnadohna.ain передают литерал 0, поэтому на любое другое значение стоит
 * одноразовый WARNING — форма не проверена данными.
 *
 * ToHexString/ToLowerHexString/ToCharacter в Dohna не вызываются ни разу и
 * оставлены честной ошибкой: гадать про их аргумент незачем.
 */

#include <stdbool.h>

#include "system4/string.h"

#include "hll.h"

static struct string *format_int(int *value, int figures, bool zenkaku)
{
	if (figures != 0) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("Int.ToString: второй аргумент %d — форма не проверена "
				"данными (все известные сайты передают 0), "
				"трактуется как число знаков с нулевым заполнением", figures);
		}
	}
	char buf[64];
	int_to_cstr(buf, sizeof(buf), value ? *value : 0, figures, true, zenkaku);
	return cstr_to_string(buf);
}

static struct string *Int_ToString(int *value, int figures)
{
	return format_int(value, figures, false);
}

static struct string *Int_ToWideString(int *value, int figures)
{
	return format_int(value, figures, true);
}

//struct string *Int_ToHexString(int *value, int figures);
//struct string *Int_ToLowerHexString(int *value, int figures);
//struct string *Int_ToCharacter(int *value);

HLL_LIBRARY(Int,
	    HLL_EXPORT(ToString, Int_ToString),
	    HLL_EXPORT(ToWideString, Int_ToWideString),
	    HLL_TODO_EXPORT(ToHexString, Int_ToHexString),
	    HLL_TODO_EXPORT(ToLowerHexString, Int_ToLowerHexString),
	    HLL_TODO_EXPORT(ToCharacter, Int_ToCharacter));
