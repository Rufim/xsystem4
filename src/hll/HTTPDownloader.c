/* Copyright (C) 2025 kichikuou <KichikuouChrome@gmail.com>
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
 * HTTPDownloader — сеть у нас НЕ РЕАЛИЗОВАНА и реализовывать её мы не собираемся:
 * `Get`/`Post` всегда возвращают false, то есть закачка никогда не стартует.
 * Но остальные функции семейства были `HLL_TODO_EXPORT` (`.fun = NULL`), а это не
 * заглушка, а ДЫРА: вызов уводит движок в отладочный REPL (FINDINGS §5y).
 * И это не абстрактный риск — у Tsumamigui 3 и Escalayer все семь функций
 * СТРОГО достижимы: игра спрашивает на старте «подключиться к сети?», и ответ
 * YES ведёт прямо сюда (экран анкеты アンケート, он же шлёт `SystemService`-сведения
 * о машине).
 *
 * Поэтому семантика ниже — «закачки нет и не было», согласованная с `Get`/`Post`:
 * ничего не бежит, прочитано ноль байт, читать нечего. Игра увидит неудачу
 * загрузки, а не повиснет.
 */
#include <stdio.h>
#include <string.h>

#include "system4/string.h"
#include "system4/utfsjis.h"

#include "hll.h"

HLL_WARN_UNIMPLEMENTED(false, bool, HTTPDownloader, Get, struct string *pIURL);
HLL_WARN_UNIMPLEMENTED(false, bool, HTTPDownloader, Post, struct string *pIURL, struct string *pIParam);

static bool HTTPDownloader_IsRun(void)
{
	return false;   // согласовано с Get/Post: закачка никогда не стартует
}

static void HTTPDownloader_Stop(void) { }

static int HTTPDownloader_GetReadSize(void)
{
	return 0;
}

static bool HTTPDownloader_ReadString(struct string **pIString)
{
	// Выход обязателен даже при false (§7 FINDINGS: ref-геттеры, не заполнившие
	// выход, заставляют игру читать мусор).
	if (pIString) {
		if (*pIString)
			free_string(*pIString);
		*pIString = string_ref(&EMPTY_STRING);
	}
	return false;
}

static bool HTTPDownloader_ReadStringUTF8ToSJIS(struct string **pIString)
{
	return HTTPDownloader_ReadString(pIString);
}

/*
 * Перекодировки. Строки движка живут в Shift-JIS (родная кодировка System4:
 * везде по дереву `utf2sjis`/`display_sjis0`), поэтому преобразования настоящие,
 * через штатные хелперы libsys4. Функции чистые, от сети не зависят —
 * подделывать в них нечего.
 */
static bool HTTPDownloader_convert(struct string **s, char *(*conv)(const char *, size_t))
{
	if (!s || !*s)
		return false;
	char *out = conv((*s)->text, (*s)->size);
	if (!out)
		return false;
	free_string(*s);
	*s = make_string(out, strlen(out));
	free(out);
	return true;
}

static bool HTTPDownloader_SJISToUTF8(struct string **pIString)
{
	return HTTPDownloader_convert(pIString, sjis2utf);
}

static bool HTTPDownloader_UTF8ToSJIS(struct string **pIString)
{
	return HTTPDownloader_convert(pIString, utf2sjis);
}

/*
 * Процентное кодирование URL (`application/x-www-form-urlencoded`) — им игра
 * готовит параметры анкеты для `Post`. Здесь оно настоящее: функция чистая,
 * от сети не зависит, и подделывать в ней нечего.
 */
static bool HTTPDownloader_EncodeString(struct string **pIString)
{
	if (!pIString || !*pIString)
		return false;
	const unsigned char *src = (const unsigned char *)(*pIString)->text;
	size_t len = (*pIString)->size;
	char *out = xmalloc(len * 3 + 1);
	char *p = out;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = src[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
			*p++ = (char)c;
		} else {
			sprintf(p, "%%%02X", c);
			p += 3;
		}
	}
	*p = '\0';
	free_string(*pIString);
	*pIString = make_string(out, p - out);
	free(out);
	return true;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool HTTPDownloader_DecodeString(struct string **pIString)
{
	if (!pIString || !*pIString)
		return false;
	const char *src = (*pIString)->text;
	size_t len = (*pIString)->size;
	char *out = xmalloc(len + 1);
	char *p = out;
	for (size_t i = 0; i < len; i++) {
		if (src[i] == '%' && i + 2 < len) {
			int hi = hexval(src[i + 1]), lo = hexval(src[i + 2]);
			if (hi >= 0 && lo >= 0) {
				*p++ = (char)((hi << 4) | lo);
				i += 2;
				continue;
			}
		}
		*p++ = src[i] == '+' ? ' ' : src[i];
	}
	*p = '\0';
	free_string(*pIString);
	*pIString = make_string(out, p - out);
	free(out);
	return true;
}

HLL_LIBRARY(HTTPDownloader,
	    HLL_EXPORT(Get, HTTPDownloader_Get),
	    HLL_EXPORT(Post, HTTPDownloader_Post),
	    HLL_EXPORT(IsRun, HTTPDownloader_IsRun),
	    HLL_EXPORT(Stop, HTTPDownloader_Stop),
	    HLL_EXPORT(GetReadSize, HTTPDownloader_GetReadSize),
	    HLL_EXPORT(ReadString, HTTPDownloader_ReadString),
	    HLL_EXPORT(ReadStringUTF8ToSJIS, HTTPDownloader_ReadStringUTF8ToSJIS),
	    HLL_EXPORT(UTF8ToSJIS, HTTPDownloader_UTF8ToSJIS),
	    HLL_EXPORT(SJISToUTF8, HTTPDownloader_SJISToUTF8),
	    HLL_EXPORT(EncodeString, HTTPDownloader_EncodeString),
	    HLL_EXPORT(DecodeString, HTTPDownloader_DecodeString)
	    );
