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
 * Библиотека `TextFile` (Ixseal) — простой текстовый файловый ввод-вывод.
 *
 * API скалькирован с .NET-овских TextReader/TextWriter: пофайловые
 * `ReadAll`/`WriteAll` и потоковые `OpenReader`/`CreateWriter` + хэндл.
 * Признак неудачи открытия — **-1**, и это доказано байткодом:
 * `textfile::detail::CTextReader@Open` (@0x1afce) делает
 * `m_handle = OpenReader(name); return m_handle != -1` (`PUSH -1; NOTE`).
 *
 * Путь игра строит сама из `SystemService.GetGameFolderPath()`, т.е. приходит
 * абсолютным; `gamedir_path_icase` это учитывает (абсолютный отдаётся как есть,
 * относительный резолвится от папки игры) и заодно чинит регистр на
 * case-sensitive ФС. Содержимое НЕ перекодируется: строки VM и так SJIS.
 *
 * Что реально зовётся у Dohna (сверено xscan'ом по всем сайтам CALLHLL):
 * `ReadAll` (`ExtableFormatLoader@Load`, `debug::detail::GetGameVersionInfo`),
 * `WriteAll` (через `AFL_TextFile_WriteAll`, 7 вызовов) и связка
 * `CreateWriter`/`WriteLine`/`Close` (`ExSaverTreeNode@Save`). Обёртки
 * `CTextReader`/`CTextWriter` — мёртвый код фреймворка: их не вызывает никто.
 *
 * `Read(handle, wrap<string>)` НЕ реализован намеренно: у .NET `TextReader.Read`
 * — это ОДИН символ, но здесь результат отдаётся строкой, а не int'ом, и
 * различить «один символ» от «весь остаток» по байткоду нельзя — единственный
 * сайт (`CTextReader@Read`) недостижим. Пусть падает заметно.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "system4.h"
#include "system4/string.h"

#include "hll.h"
#include "xsystem4.h"

#define TF_MAX_HANDLES 16

static FILE *tf_handles[TF_MAX_HANDLES];

// `gamedir_path_icase` отдаёт NULL, когда case-insensitive поиск не нашёл файла
// (обычная ситуация: формат-файлы ExTable в поставке игры отсутствуют — они
// нужны только редактору). Для сообщения об ошибке и для СОЗДАНИЯ файла нужен
// прямой путь.
static char *tf_path(struct string *file_name)
{
	char *path = gamedir_path_icase(file_name->text);
	return path ? path : gamedir_path(file_name->text);
}

static FILE *tf_get(int handle)
{
	if (handle < 0 || handle >= TF_MAX_HANDLES)
		return NULL;
	return tf_handles[handle];
}

static int tf_open(struct string *file_name, const char *mode)
{
	int handle = -1;
	for (int i = 0; i < TF_MAX_HANDLES; i++) {
		if (!tf_handles[i]) {
			handle = i;
			break;
		}
	}
	if (handle < 0) {
		WARNING("TextFile: свободных хэндлов нет");
		return -1;
	}
	char *path = tf_path(file_name);
	FILE *f = fopen(path, mode);
	if (!f)
		WARNING("TextFile: не удалось открыть %s (%s): %s", path, mode, strerror(errno));
	free(path);
	if (!f)
		return -1;
	tf_handles[handle] = f;
	return handle;
}

// Заменить содержимое out-строки (форма аргумента `wrap<string>`: ffi отдаёт
// указатель на heap-слот строки и пишет результат обратно — см. AIN_WRAP в ffi.c).
static void tf_set_out(struct string **out, struct string *s)
{
	if (!out) {
		free_string(s);
		return;
	}
	if (*out)
		free_string(*out);
	*out = s;
}

static bool TextFile_ReadAll(struct string *file_name, struct string **text)
{
	char *path = tf_path(file_name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		WARNING("TextFile.ReadAll: нет файла %s: %s", path, strerror(errno));
		free(path);
		tf_set_out(text, string_ref(&EMPTY_STRING));
		return false;
	}
	free(path);

	size_t cap = 4096, len = 0;
	char *buf = xmalloc(cap);
	for (;;) {
		if (len == cap) {
			cap *= 2;
			buf = xrealloc(buf, cap);
		}
		size_t n = fread(buf + len, 1, cap - len, f);
		if (!n)
			break;
		len += n;
	}
	bool ok = !ferror(f);
	fclose(f);
	tf_set_out(text, make_string(buf, len));
	free(buf);
	return ok;
}

static bool TextFile_WriteAll(struct string *file_name, struct string *text)
{
	char *path = tf_path(file_name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		WARNING("TextFile.WriteAll: не удалось создать %s: %s", path, strerror(errno));
		free(path);
		return false;
	}
	free(path);
	bool ok = text->size == 0 || fwrite(text->text, text->size, 1, f) == 1;
	if (fclose(f) != 0)
		ok = false;
	return ok;
}

static int TextFile_CreateWriter(struct string *file_name)
{
	return tf_open(file_name, "wb");
}

static int TextFile_OpenReader(struct string *file_name)
{
	return tf_open(file_name, "rb");
}

static bool TextFile_Write(int handle, struct string *text)
{
	FILE *f = tf_get(handle);
	if (!f)
		return false;
	return text->size == 0 || fwrite(text->text, text->size, 1, f) == 1;
}

static bool TextFile_WriteLine(int handle, struct string *text)
{
	if (!TextFile_Write(handle, text))
		return false;
	return fputc('\n', tf_get(handle)) != EOF;
}

static bool TextFile_ReadLine(int handle, struct string **text)
{
	FILE *f = tf_get(handle);
	if (!f) {
		tf_set_out(text, string_ref(&EMPTY_STRING));
		return false;
	}
	size_t cap = 256, len = 0;
	char *buf = xmalloc(cap);
	int c;
	while ((c = fgetc(f)) != EOF && c != '\n') {
		if (len + 1 >= cap) {
			cap *= 2;
			buf = xrealloc(buf, cap);
		}
		buf[len++] = (char)c;
	}
	// Терминатор CRLF: '\r' в конец строки не входит.
	if (len > 0 && buf[len - 1] == '\r')
		len--;
	bool got = len > 0 || c == '\n';
	tf_set_out(text, make_string(buf, len));
	free(buf);
	return got;
}

static bool TextFile_IsEOF(int handle)
{
	FILE *f = tf_get(handle);
	if (!f)
		return true;
	int c = fgetc(f);
	if (c == EOF)
		return true;
	ungetc(c, f);
	return false;
}

static bool TextFile_Close(int handle)
{
	FILE *f = tf_get(handle);
	if (!f)
		return false;
	tf_handles[handle] = NULL;
	return fclose(f) == 0;
}

//static bool TextFile_Read(int handle, struct string **text);

HLL_LIBRARY(TextFile,
	    HLL_EXPORT(ReadAll, TextFile_ReadAll),
	    HLL_EXPORT(WriteAll, TextFile_WriteAll),
	    HLL_EXPORT(CreateWriter, TextFile_CreateWriter),
	    HLL_EXPORT(OpenReader, TextFile_OpenReader),
	    HLL_EXPORT(Write, TextFile_Write),
	    HLL_EXPORT(WriteLine, TextFile_WriteLine),
	    HLL_EXPORT(ReadLine, TextFile_ReadLine),
	    HLL_EXPORT(IsEOF, TextFile_IsEOF),
	    HLL_EXPORT(Close, TextFile_Close),
	    HLL_TODO_EXPORT(Read, TextFile_Read)
	);
