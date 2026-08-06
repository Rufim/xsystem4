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
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include "system4/ain.h"
#include "system4/file.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "xsystem4.h"
#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "hll.h"

static bool FileOperation_ExistFile(struct string *file_name)
{
	char *path = unix_path(file_name->text);
	ustat s;
	bool result = stat_utf8(path, &s) == 0 && S_ISREG(s.st_mode);
	free(path);
	return result;
}

//bool FileOperation_DeleteFile(string FileName);

static bool FileOperation_CopyFile(struct string *dest_file_name, struct string *src_file_name)
{
	char *dest = unix_path(dest_file_name->text);
	char *src = unix_path(src_file_name->text);
	bool result = file_copy(src, dest);
	free(src);
	free(dest);
	return result;
}

//bool FileOperation_GetFileCreationTime(string FileName, ref int nYear, ref int nMonth, ref int nDay, ref int nWeek, ref int nHour, ref int nMin, ref int nSecond);
//bool FileOperation_GetFileLastAccessTime(string FileName, ref int nYear, ref int nMonth, ref int nDay, ref int nWeek, ref int nHour, ref int nMin, ref int nSecond);
//bool FileOperation_GetFileLastWriteTime(string FileName, ref int nYear, ref int nMonth, ref int nDay, ref int nWeek, ref int nHour, ref int nMin, ref int nSecond);

static bool FileOperation_GetFileSize(struct string *file_name, int *size)
{
	char *path = unix_path(file_name->text);
	ustat s;
	if (stat_utf8(path, &s) < 0) {
		WARNING("stat(\"%s\"): %s", display_utf0(path), strerror(errno));
		free(path);
		return false;
	}
	if (!S_ISREG(s.st_mode)) {
		WARNING("stat(\"%s\"): not a regular file", display_utf0(path));
		free(path);
		return false;
	}
	*size = s.st_size;
	free(path);
	return true;
}

static bool FileOperation_ExistFolder(struct string *folder_name)
{
	char *path = unix_path(folder_name->text);
	bool result = is_directory(path);
	free(path);
	return result;
}

static bool FileOperation_CreateFolder(struct string *folder_name)
{
	char *path = unix_path(folder_name->text);
	bool result = mkdir_p(path) == 0;
	if (!result)
		WARNING("mkdir_p(%s): %s", path, strerror(errno));
	free(path);
	return result;
}

static bool rmtree(const char *path)
{
	UDIR *d = opendir_utf8(path);
	if (!d) {
		WARNING("opendir(\"%s\"): %s", display_utf0(path), strerror(errno));
		return false;
	}
	bool ok = true;
	char *d_name;
	while (ok && (d_name = readdir_utf8(d)) != NULL) {
		if (d_name[0] == '.' && (d_name[1] == '\0' || (d_name[1] == '.' && d_name[2] == '\0'))) {
			free(d_name);
			continue;
		}

		char *utf8_path = path_join(path, d_name);
		ustat s;
		if (stat_utf8(utf8_path, &s) < 0) {
			WARNING("stat(\"%s\"): %s", display_utf0(utf8_path), strerror(errno));
			ok = false;
		} else {
			if (S_ISDIR(s.st_mode)) {
				if (!rmtree(utf8_path))
					ok = false;
			} else {
				if (remove_utf8(utf8_path) < 0) {
					WARNING("remove(\"%s\"): %s", display_utf0(utf8_path), strerror(errno));
					ok = false;
				}
			}
		}
		free(utf8_path);
		free(d_name);
	}
	closedir_utf8(d);
	if (ok) {
		if (rmdir_utf8(path) < 0) {
			WARNING("rmdir(\"%s\"): %s", display_utf0(path), strerror(errno));
			ok = false;
		}
	}
	return ok;
}

static bool FileOperation_DeleteFolder(struct string *folder_name)
{
	char *path = unix_path(folder_name->text);
	bool result = rmtree(path);
	free(path);
	return result;
}

static struct page *alloc_name_array(int nr_names, struct string **names)
{
	union vm_value dim = { .i = nr_names };
	struct page *page = alloc_array(1, &dim, AIN_ARRAY_STRING, 0, false);
	for (int i = 0; i < nr_names; i++)
		page->values[i].i = heap_alloc_string(names[i]);
	return page;
}

static bool get_file_list(struct string *folder_name, struct page **out, bool folders)
{
	char *dir_name = unix_path(folder_name->text);

	UDIR *d = opendir_utf8(dir_name);
	if (!d) {
		WARNING("opendir(\"%s\"): %s", display_utf0(dir_name), strerror(errno));
		free(dir_name);
		return false;
	}

	struct string **names = NULL;
	int nr_names = 0;

	char *d_name;
	while ((d_name = readdir_utf8(d)) != NULL) {
		if (d_name[0] == '.') {
			free(d_name);
			continue;
		}

		char *utf8_path = path_join(dir_name, d_name);
		ustat s;
		if (stat_utf8(utf8_path, &s) < 0) {
			WARNING("stat(\"%s\"): %s", display_utf0(utf8_path), strerror(errno));
			goto loop_next;
		}
		if (folders) {
			if (!S_ISDIR(s.st_mode))
				goto loop_next;
		} else {
			if (!S_ISREG(s.st_mode))
				goto loop_next;
		}

		char *sjis_name = utf2sjis(d_name, 0);
		names = xrealloc_array(names, nr_names, nr_names+1, sizeof(struct string*));
		names[nr_names++] = cstr_to_string(sjis_name);
		free(sjis_name);
	loop_next:
		free(utf8_path);
		free(d_name);
	}
	closedir_utf8(d);
	free(dir_name);

	struct page *page = alloc_name_array(nr_names, names);
	free(names);

	if (*out) {
		delete_page_vars(*out);
		free_page(*out);
	}
	*out = page;
	return true;
}

static bool FileOperation_GetFileList(struct string *folder_name, struct page **out)
{
	return get_file_list(folder_name, out, false);
}

static bool FileOperation_GetFolderList(struct string *folder_name, struct page **out)
{
	return get_file_list(folder_name, out, true);
}

/*
 * Ixseal (System 4 v14) сменил ФОРМУ обеих списковых функций:
 *   v6/v7: bool GetFileList(string, ref array<string> out)   — ret 47, args (12,24)
 *   v14:   array<string> GetFileList(string)                 — ret 79, args (12)
 * FFI маршалит по сигнатуре .ain, поэтому старая реализация получала один
 * аргумент, возвращала bool — и игра клала это ЧИСЛО в локал типа
 * `ref array<string>`. При неудаче (папки ещё нет) число было 0, а heap-слот 0 —
 * это ГЛОБАЛЬНАЯ СТРАНИЦА: на возврате функции `variable_fini` делала
 * `heap_unref(0)`, глобальная страница уничтожалась, и следующее же обращение к
 * глобалу падало «Out of bounds heap index: 0/227» (DohnaDohna::GetContext).
 *
 * Возврат — heap-слот страницы массива (как у Array.Where). Пустой результат
 * обязан быть валидной 0-элементной ТИПИЗИРОВАННОЙ страницей, а не NULL:
 * иначе следующий PushBack пересоздаст контейнер int-массивом.
 */
static int ix_file_list(struct string *folder_name, bool folders)
{
	struct page *page = NULL;
	if (!get_file_list(folder_name, &page, folders) || !page)
		page = alloc_name_array(0, NULL);
	int slot = heap_alloc_slot(VM_PAGE);
	heap_set_page(slot, page);
	return slot;
}

static int FileOperation_ix_GetFileList(struct string *folder_name)
{
	return ix_file_list(folder_name, false);
}

static int FileOperation_ix_GetFolderList(struct string *folder_name)
{
	return ix_file_list(folder_name, true);
}

static void FileOperation_PreLink(void);

HLL_LIBRARY(FileOperation,
	    HLL_EXPORT(_PreLink, FileOperation_PreLink),
	    HLL_EXPORT(ExistFile, FileOperation_ExistFile),
	    HLL_TODO_EXPORT(DeleteFile, FileOperation_DeleteFile),
	    HLL_EXPORT(CopyFile, FileOperation_CopyFile),
	    HLL_TODO_EXPORT(GetFileCreationTime, FileOperation_GetFileCreationTime),
	    HLL_TODO_EXPORT(GetFileLastAccessTime, FileOperation_GetFileLastAccessTime),
	    HLL_TODO_EXPORT(GetFileLastWriteTime, FileOperation_GetFileLastWriteTime),
	    HLL_EXPORT(GetFileSize, FileOperation_GetFileSize),
	    HLL_EXPORT(ExistFolder, FileOperation_ExistFolder),
	    HLL_EXPORT(CreateFolder, FileOperation_CreateFolder),
	    HLL_EXPORT(DeleteFolder, FileOperation_DeleteFolder),
	    HLL_EXPORT(GetFileList, FileOperation_GetFileList),
	    HLL_EXPORT(GetFolderList, FileOperation_GetFolderList));

// Гейт СТРУКТУРНЫЙ — по форме, объявленной в .ain (тип возврата), а не по
// версии движка: у Tsumamigui 3 (v7) и Escalayer (v6) обе функции объявлены
// `ret=47 (12,24)`, у Dohna — `ret=79 (12)`.
static void FileOperation_PreLink(void)
{
	int libno = ain_get_library(ain, "FileOperation");
	if (libno < 0)
		return;
	static const struct { const char *name; void *ix; } list_getters[] = {
		{ "GetFileList",   FileOperation_ix_GetFileList },
		{ "GetFolderList", FileOperation_ix_GetFolderList },
	};
	for (unsigned i = 0; i < sizeof(list_getters) / sizeof(*list_getters); i++) {
		int fno = ain_get_library_function(ain, libno, list_getters[i].name);
		if (fno < 0)
			continue;
		if (ain->libraries[libno].functions[fno].return_type.data != AIN_ARRAY)
			continue;
		static_library_replace(&lib_FileOperation, list_getters[i].name,
				       list_getters[i].ix);
	}
}
