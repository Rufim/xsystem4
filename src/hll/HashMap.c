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
 * HashMap.hll — id-таблицы string→int. Единственный клиент в байткоде —
 * `utility::detail::CHashMap` (Haha Ranman): конструктор зовёт Create и
 * хранит id, деструктор — Release, метод Free — очистка без освобождения id.
 * Save/Load — снапшот ВСЕХ таблиц в сейв-образ `wrap<array<int>>` (зовётся из
 * `gamesave::detail::セーブ実行` между MainEXFile.Save и system.ResumeSave);
 * без него сохранение падает фатальной «Unimplemented HLL function».
 * Формат образа наш собственный (магия "HMP") — сейв читается нашим же
 * загрузчиком, совпадать с образом оригинального HashMap.dll он не обязан
 * (та же конвенция, что у AnteaterADVLogList, магия "ADL").
 */

#include <string.h>

#include "system4.h"
#include "system4/string.h"

#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "hll.h"
#include "iarray.h"

struct hm_entry {
	struct string *key;
	int value;
};

struct hm_map {
	bool alive;
	int nr;
	struct hm_entry *entries;
};

static struct hm_map *maps;
static int nr_maps;

// id — 1-базный: 0 остаётся невалидным (игра хранит id в int-поле,
// обнулённом конструктором структуры).
static struct hm_map *hm_get(int id)
{
	if (id < 1 || id > nr_maps || !maps[id - 1].alive)
		return NULL;
	return &maps[id - 1];
}

static int hm_find(struct hm_map *m, struct string *key)
{
	for (int i = 0; i < m->nr; i++) {
		if (!strcmp(m->entries[i].key->text, key->text))
			return i;
	}
	return -1;
}

static void hm_clear(struct hm_map *m)
{
	for (int i = 0; i < m->nr; i++)
		free_string(m->entries[i].key);
	free(m->entries);
	m->entries = NULL;
	m->nr = 0;
}

static int HashMap_Create(void)
{
	for (int i = 0; i < nr_maps; i++) {
		if (!maps[i].alive) {
			maps[i].alive = true;
			return i + 1;
		}
	}
	int i = nr_maps++;
	maps = xrealloc_array(maps, i, i + 1, sizeof(struct hm_map));
	maps[i].alive = true;
	return i + 1;
}

static void HashMap_Release(int id)
{
	struct hm_map *m = hm_get(id);
	if (!m)
		return;
	hm_clear(m);
	m->alive = false;
}

static void HashMap_Free(int id)
{
	struct hm_map *m = hm_get(id);
	if (m)
		hm_clear(m);
}

static bool HashMap_Add(int id, struct string *key, int value)
{
	struct hm_map *m = hm_get(id);
	if (!m)
		return false;
	int i = hm_find(m, key);
	if (i >= 0) {
		m->entries[i].value = value;
		return true;
	}
	m->entries = xrealloc_array(m->entries, m->nr, m->nr + 1, sizeof(struct hm_entry));
	m->entries[m->nr].key = string_ref(key);
	m->entries[m->nr].value = value;
	m->nr++;
	return true;
}

static void HashMap_Erase(int id, struct string *key)
{
	struct hm_map *m = hm_get(id);
	if (!m)
		return;
	int i = hm_find(m, key);
	if (i < 0)
		return;
	free_string(m->entries[i].key);
	memmove(m->entries + i, m->entries + i + 1, (m->nr - i - 1) * sizeof(struct hm_entry));
	m->nr--;
}

static int HashMap_Numof(int id)
{
	struct hm_map *m = hm_get(id);
	return m ? m->nr : 0;
}

static bool HashMap_Empty(int id)
{
	return HashMap_Numof(id) == 0;
}

static bool HashMap_Any(int id)
{
	return HashMap_Numof(id) > 0;
}

// Перегрузка по АРНОСТИ (как Array.Find#1): Any(id) и Any(id, key) линкуются
// по имени, поэтому 2-аргументной форме нужен свой декорированный экспорт —
// иначе key пришёл бы мусором в несуществующий параметр.
static bool HashMap_AnyKey(int id, struct string *key)
{
	struct hm_map *m = hm_get(id);
	return m && hm_find(m, key) >= 0;
}

static bool HashMap_At(int id, struct string *key, union vm_value *value)
{
	struct hm_map *m = hm_get(id);
	if (!m)
		return false;
	int i = hm_find(m, key);
	if (i < 0)
		return false;
	value->i = m->entries[i].value;
	return true;
}

// Возврат массива — готовым heap-слотом (конвенция String.Split/ShallowCopy).
static int HashMap_GetKeyList(int id)
{
	int slot = heap_alloc_slot(VM_PAGE);
	union vm_value dim = { .i = 0 };
	struct page *out = alloc_array(1, &dim, AIN_ARRAY_STRING, 0, false);
	struct hm_map *m = hm_get(id);
	for (int i = 0; m && i < m->nr; i++) {
		union vm_value v = { .i = heap_alloc_string(string_ref(m->entries[i].key)) };
		out = array_pushback_n(out, &v, 1, AIN_ARRAY_STRING, 0);
	}
	heap_set_page(slot, out);
	return slot;
}

static int HashMap_GetValueList(int id)
{
	int slot = heap_alloc_slot(VM_PAGE);
	union vm_value dim = { .i = 0 };
	struct page *out = alloc_array(1, &dim, AIN_ARRAY_INT, 0, false);
	struct hm_map *m = hm_get(id);
	for (int i = 0; m && i < m->nr; i++) {
		union vm_value v = { .i = m->entries[i].value };
		out = array_pushback_n(out, &v, 1, AIN_ARRAY_INT, 0);
	}
	heap_set_page(slot, out);
	return slot;
}

static bool HashMap_Save(struct page **buffer)
{
	struct iarray_writer w;
	iarray_init_writer(&w, "HMP");
	iarray_write(&w, 0); // версия формата
	iarray_write(&w, nr_maps);
	for (int i = 0; i < nr_maps; i++) {
		iarray_write(&w, maps[i].alive);
		iarray_write(&w, maps[i].nr);
		for (int j = 0; j < maps[i].nr; j++) {
			iarray_write_string(&w, maps[i].entries[j].key);
			iarray_write(&w, maps[i].entries[j].value);
		}
	}
	if (*buffer) {
		delete_page_vars(*buffer);
		free_page(*buffer);
	}
	*buffer = iarray_to_page(&w);
	iarray_free_writer(&w);
	return true;
}

static bool HashMap_Load(struct page **buffer)
{
	if (!*buffer)
		return false;
	struct iarray_reader r;
	iarray_init_reader(&r, *buffer, "HMP");
	if (iarray_read(&r))
		return false;

	for (int i = 0; i < nr_maps; i++)
		hm_clear(&maps[i]);
	free(maps);
	maps = NULL;
	nr_maps = 0;

	int n = iarray_read(&r);
	if (n < 0 || n > 100000) {
		WARNING("HashMap.Load: подозрительное число таблиц %d", n);
		return false;
	}
	nr_maps = n;
	maps = xcalloc(n ? n : 1, sizeof(struct hm_map));
	for (int i = 0; i < n; i++) {
		maps[i].alive = iarray_read(&r);
		int nr = iarray_read(&r);
		if (nr < 0 || nr > 1000000 || r.error) {
			WARNING("HashMap.Load: битый образ (таблица %d, nr=%d)", i, nr);
			return false;
		}
		maps[i].nr = nr;
		maps[i].entries = xcalloc(nr ? nr : 1, sizeof(struct hm_entry));
		for (int j = 0; j < nr; j++) {
			maps[i].entries[j].key = iarray_read_string(&r);
			maps[i].entries[j].value = iarray_read(&r);
		}
	}
	return !r.error;
}

HLL_LIBRARY(HashMap,
	    HLL_EXPORT(Create, HashMap_Create),
	    HLL_EXPORT(Release, HashMap_Release),
	    HLL_EXPORT(Free, HashMap_Free),
	    HLL_EXPORT(Add, HashMap_Add),
	    HLL_EXPORT(Erase, HashMap_Erase),
	    HLL_EXPORT(Numof, HashMap_Numof),
	    HLL_EXPORT(Empty, HashMap_Empty),
	    HLL_EXPORT_N(Any, 2, HashMap_AnyKey),
	    HLL_EXPORT(Any, HashMap_Any),
	    HLL_EXPORT(At, HashMap_At),
	    HLL_EXPORT(GetKeyList, HashMap_GetKeyList),
	    HLL_EXPORT(GetValueList, HashMap_GetValueList),
	    HLL_EXPORT(Save, HashMap_Save),
	    HLL_EXPORT(Load, HashMap_Load));
