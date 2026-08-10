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

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "system4.h"
#include "system4/hashtable.h"
#include "system4/string.h"

#include "hll.h"

// Общие данные системы (громкость, скорость текста и т.п.). В оригинале
// LoadAtStartup/сохранение были заглушками — данные жили только в памяти и не
// переживали перезапуск. Здесь реализована персистентность в файл save_path:
// авто-загрузка при установке пути и запись при каждом изменении.

enum csd_type { CSD_INT, CSD_FLOAT, CSD_STRING, CSD_BOOL };

struct csd_entry {
	enum csd_type type;
	union {
		int i;
		float f;
		struct string *s;
	} v;
};

static struct string *save_path = NULL;

static struct hash_table *csd_table(void)
{
	static struct hash_table *table = NULL;
	if (!table) {
		table = ht_create(64);
	}
	return table;
}

static struct csd_entry *csd_slot(const char *key)
{
	struct ht_slot *slot = ht_put(csd_table(), key, NULL);
	struct csd_entry *e = slot->value;
	if (!e) {
		e = xcalloc(1, sizeof(struct csd_entry));
		slot->value = e;
	}
	return e;
}

static void csd_entry_clear_string(struct csd_entry *e)
{
	if (e->type == CSD_STRING && e->v.s) {
		free_string(e->v.s);
		e->v.s = NULL;
	}
}

static bool csd_get(const char *key, struct csd_entry **out)
{
	void *p;
	if (!_ht_get(csd_table(), key, &p) || !p)
		return false;
	*out = p;
	return true;
}

// --- сериализация ---
// Формат: "CSD1" | uint32 count | { uint8 type, uint32 klen, key,
//   [INT/BOOL: int32] [FLOAT: float] [STRING: uint32 len, bytes] } * count

struct csd_ctx { FILE *fp; uint32_t count; };

static void csd_count_cb(struct ht_slot *slot, void *data)
{
	if (slot->value)
		(*(uint32_t *)data)++;
}

static void csd_write_cb(struct ht_slot *slot, void *data)
{
	FILE *fp = data;
	struct csd_entry *e = slot->value;
	if (!e)
		return;
	const char *key = slot->key;
	uint32_t klen = (uint32_t)strlen(key);
	uint8_t t = (uint8_t)e->type;
	fwrite(&t, 1, 1, fp);
	fwrite(&klen, 4, 1, fp);
	fwrite(key, 1, klen, fp);
	if (e->type == CSD_STRING) {
		uint32_t slen = e->v.s ? (uint32_t)e->v.s->size : 0;
		fwrite(&slen, 4, 1, fp);
		if (slen)
			fwrite(e->v.s->text, 1, slen, fp);
	} else if (e->type == CSD_FLOAT) {
		fwrite(&e->v.f, 4, 1, fp);
	} else {
		int32_t iv = e->v.i;
		fwrite(&iv, 4, 1, fp);
	}
}

static void csd_save(void)
{
	if (getenv("XSYS4_CSD_TRACE"))
		NOTICE("CSD save -> %s", save_path ? save_path->text : "(путь НЕ ЗАДАН)");
	if (!save_path)
		return;
	FILE *fp = fopen(save_path->text, "wb");
	if (!fp) {
		WARNING("CommonSystemData: не удалось записать %s", save_path->text);
		return;
	}
	fwrite("CSD1", 1, 4, fp);
	uint32_t count = 0;
	ht_foreach(csd_table(), csd_count_cb, &count);
	fwrite(&count, 4, 1, fp);
	ht_foreach(csd_table(), csd_write_cb, fp);
	fclose(fp);
}

static bool csd_load(void)
{
	if (!save_path)
		return false;
	FILE *fp = fopen(save_path->text, "rb");
	if (!fp)
		return false;
	bool ok = false;
	char magic[4];
	uint32_t count = 0;
	if (fread(magic, 1, 4, fp) != 4 || memcmp(magic, "CSD1", 4) != 0)
		goto out;
	if (fread(&count, 4, 1, fp) != 1)
		goto out;
	for (uint32_t n = 0; n < count; n++) {
		uint8_t t;
		uint32_t klen;
		if (fread(&t, 1, 1, fp) != 1)
			break;
		if (fread(&klen, 4, 1, fp) != 1 || klen > 4096)
			break;
		char *key = xmalloc(klen + 1);
		if (klen && fread(key, 1, klen, fp) != klen) {
			free(key);
			break;
		}
		key[klen] = '\0';
		if (t == CSD_STRING) {
			uint32_t slen;
			if (fread(&slen, 4, 1, fp) != 1 || slen > (1u << 20)) {
				free(key);
				break;
			}
			char *sv = xmalloc(slen + 1);
			if (slen && fread(sv, 1, slen, fp) != slen) {
				free(sv);
				free(key);
				break;
			}
			sv[slen] = '\0';
			struct csd_entry *e = csd_slot(key);
			csd_entry_clear_string(e);
			e->type = CSD_STRING;
			e->v.s = make_string(sv, slen);
			free(sv);
		} else if (t == CSD_FLOAT) {
			float fv;
			if (fread(&fv, 4, 1, fp) != 1) {
				free(key);
				break;
			}
			struct csd_entry *e = csd_slot(key);
			csd_entry_clear_string(e);
			e->type = CSD_FLOAT;
			e->v.f = fv;
		} else {
			int32_t iv;
			if (fread(&iv, 4, 1, fp) != 1) {
				free(key);
				break;
			}
			struct csd_entry *e = csd_slot(key);
			csd_entry_clear_string(e);
			e->type = (t == CSD_BOOL) ? CSD_BOOL : CSD_INT;
			e->v.i = iv;
		}
		free(key);
	}
	ok = true;
out:
	fclose(fp);
	return ok;
}

static void CommonSystemData_SetFullPathSaveFileName(struct string *filename)
{
	if (save_path)
		free_string(save_path);
	save_path = string_dup(filename);
	// XSYS4_CSD_TRACE=1 — куда игра назначила файл общих настроек и что в него пишет.
	if (getenv("XSYS4_CSD_TRACE"))
		NOTICE("CSD путь <- '%s'", save_path->text);
	// Авто-загрузка сохранённых настроек (на случай, если игра не зовёт LoadAtStartup).
	csd_load();
}

static bool CommonSystemData_LoadAtStartup(void)
{
	return csd_load();
}

static bool CommonSystemData_SetDataInt(struct string *name, int value)
{
	struct csd_entry *e = csd_slot(name->text);
	csd_entry_clear_string(e);
	e->type = CSD_INT;
	e->v.i = value;
	csd_save();
	return true;
}

static bool CommonSystemData_SetDataFloat(struct string *name, float value)
{
	struct csd_entry *e = csd_slot(name->text);
	csd_entry_clear_string(e);
	e->type = CSD_FLOAT;
	e->v.f = value;
	csd_save();
	return true;
}

static bool CommonSystemData_SetDataString(struct string *name, struct string *value)
{
	struct csd_entry *e = csd_slot(name->text);
	csd_entry_clear_string(e);
	e->type = CSD_STRING;
	e->v.s = string_dup(value);
	csd_save();
	return true;
}

static bool CommonSystemData_SetDataBool(struct string *name, bool value)
{
	struct csd_entry *e = csd_slot(name->text);
	csd_entry_clear_string(e);
	e->type = CSD_BOOL;
	e->v.i = value;
	csd_save();
	return true;
}

static bool CommonSystemData_GetDataInt(struct string *name, int *value)
{
	struct csd_entry *e;
	if (csd_get(name->text, &e)) {
		*value = e->v.i;
		return true;
	}
	return false;
}

static bool CommonSystemData_GetDataFloat(struct string *name, float *value)
{
	struct csd_entry *e;
	if (csd_get(name->text, &e)) {
		*value = e->v.f;
		return true;
	}
	return false;
}

static bool CommonSystemData_GetDataString(struct string *name, struct string **value)
{
	struct csd_entry *e;
	if (csd_get(name->text, &e) && e->type == CSD_STRING && e->v.s) {
		if (*value)
			free_string(*value);
		*value = string_dup(e->v.s);
		return true;
	}
	return false;
}

static bool CommonSystemData_GetDataBool(struct string *name, bool *value)
{
	struct csd_entry *e;
	if (csd_get(name->text, &e)) {
		*value = e->v.i;
		return true;
	}
	return false;
}

HLL_LIBRARY(CommonSystemData,
	    HLL_EXPORT(SetFullPathSaveFileName, CommonSystemData_SetFullPathSaveFileName),
	    HLL_EXPORT(LoadAtStartup, CommonSystemData_LoadAtStartup),
	    HLL_EXPORT(SetDataInt, CommonSystemData_SetDataInt),
	    HLL_EXPORT(SetDataFloat, CommonSystemData_SetDataFloat),
	    HLL_EXPORT(SetDataString, CommonSystemData_SetDataString),
	    HLL_EXPORT(SetDataBool, CommonSystemData_SetDataBool),
	    HLL_EXPORT(GetDataInt, CommonSystemData_GetDataInt),
	    HLL_EXPORT(GetDataFloat, CommonSystemData_GetDataFloat),
	    HLL_EXPORT(GetDataString, CommonSystemData_GetDataString),
	    HLL_EXPORT(GetDataBool, CommonSystemData_GetDataBool));
