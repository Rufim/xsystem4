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

#include <string.h>
#include <stdlib.h>

#include "system4/string.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "asset_manager.h"
#include "hll.h"

/*
 * Номера типов — из байткода Dohna (обёртки AFL_AFA_*_Add):
 *   2 = CG, 3 = Flat, 5 = Sound, 8 = 3D (движок такого типа не держит).
 * Поиск заголовков (GetPrefixSearchTitleList и семейство) — то, чем Dohna ищет
 * МУЗЫКУ КАРТЫ: SceneHitokari@PlayMusic → MusicRouter → Music@Exists →
 * resources::detail::CResourceSearcher → afa::detail::GetPrefixSearchTitleList.
 * Пока функции не было, вход в hunting-фазу валил движок фатальным hll_call.
 */
static enum asset_type get_asset_type(int type)
{
	switch (type) {
	case 2: return ASSET_CG;
	case 3: return ASSET_FLAT;
	case 5: return ASSET_SOUND;
	default: return -1;
	}
}

HLL_WARN_UNIMPLEMENTED(false, bool, AFAFactory, IsExistArchive, struct string *ArchiveName);

bool AFAFactory_LoadArchive(int type, struct string *archive_name)
{
	enum asset_type t = get_asset_type(type);
	if (t < 0) {
		WARNING("Unknown asset type: %d", type);
		return false;
	}
	return asset_manager_load_archive(t, archive_name->text);
}

// ---- перечисление и поиск заголовков ----

enum afa_search_mode {
	AFA_SEARCH_EXACT,
	AFA_SEARCH_PREFIX,
	AFA_SEARCH_SUFFIX,
};

struct afa_search_ctx {
	enum afa_search_mode mode;
	const char *needle;
	size_t needle_len;
	// счёт/выбор по индексу
	int count;
	int want_index;
	struct string *found;
	// наполнение массива результата
	struct page **list;
};

static bool title_matches(struct afa_search_ctx *ctx, const char *name)
{
	size_t len = strlen(name);
	switch (ctx->mode) {
	case AFA_SEARCH_EXACT:
		return len == ctx->needle_len && !memcmp(name, ctx->needle, len);
	case AFA_SEARCH_PREFIX:
		return len >= ctx->needle_len && !memcmp(name, ctx->needle, ctx->needle_len);
	case AFA_SEARCH_SUFFIX:
		return len >= ctx->needle_len
			&& !memcmp(name + len - ctx->needle_len, ctx->needle, ctx->needle_len);
	}
	return false;
}

static bool search_cb(const char *name, void *user)
{
	struct afa_search_ctx *ctx = user;
	if (!title_matches(ctx, name))
		return true;
	if (ctx->list) {
		struct string *s = make_string(name, strlen(name));
		union vm_value v = { .i = heap_alloc_string(s) };
		*ctx->list = array_pushback(*ctx->list, v, AIN_ARRAY_STRING, 0);
	}
	if (ctx->want_index >= 0 && ctx->count == ctx->want_index)
		ctx->found = make_string(name, strlen(name));
	ctx->count++;
	return true;
}

static int afa_search(int type, enum afa_search_mode mode, const char *needle,
		int want_index, struct string **found_out, struct page **list)
{
	enum asset_type t = get_asset_type(type);
	if (t < 0) {
		WARNING("Unknown asset type: %d", type);
		return 0;
	}
	struct afa_search_ctx ctx = {
		.mode = mode,
		.needle = needle,
		.needle_len = strlen(needle),
		.want_index = want_index,
		.list = list,
	};
	asset_foreach_name(t, search_cb, &ctx);
	if (found_out)
		*found_out = ctx.found;
	return ctx.count;
}

/*
 * Результат ПОИСКА (SearchTitle/Prefix/Suffix) хранится ДО СЛЕДУЮЩЕГО ПОИСКА
 * своего типа: GetCountOfSearchData/GetSearchTitleByIndex читают его отдельно.
 * Храним параметры запроса, а не копию списка — перечисление дешёвое (архив в
 * памяти), и порядок обхода стабильный.
 */
static struct {
	bool valid;
	enum afa_search_mode mode;
	char *needle;
} afa_last_search[16];

static void remember_search(int type, enum afa_search_mode mode, struct string *name)
{
	if (type < 0 || type >= 16)
		return;
	free(afa_last_search[type].needle);
	afa_last_search[type].needle = strdup(name->text);
	afa_last_search[type].mode = mode;
	afa_last_search[type].valid = true;
}

static bool count_all_cb(const char *name, void *user)
{
	(void)name;
	(*(int*)user)++;
	return true;
}

int AFAFactory_GetCountOfData(int type)
{
	enum asset_type t = get_asset_type(type);
	if (t < 0) {
		WARNING("Unknown asset type: %d", type);
		return 0;
	}
	int n = 0;
	asset_foreach_name(t, count_all_cb, &n);
	return n;
}

struct afa_index_ctx { int want; int cur; struct string *found; };

static bool index_cb(const char *name, void *user)
{
	struct afa_index_ctx *ctx = user;
	if (ctx->cur++ == ctx->want) {
		ctx->found = make_string(name, strlen(name));
		return false;
	}
	return true;
}

struct string *AFAFactory_GetTitleByIndex(int type, int index)
{
	enum asset_type t = get_asset_type(type);
	struct afa_index_ctx ctx = { .want = index };
	if (t >= 0 && index >= 0)
		asset_foreach_name(t, index_cb, &ctx);
	return ctx.found ? ctx.found : string_ref(&EMPTY_STRING);
}

int AFAFactory_SearchTitle(int type, struct string *name)
{
	remember_search(type, AFA_SEARCH_EXACT, name);
	return afa_search(type, AFA_SEARCH_EXACT, name->text, -1, NULL, NULL);
}

int AFAFactory_PrefixSearchTitle(int type, struct string *name)
{
	remember_search(type, AFA_SEARCH_PREFIX, name);
	return afa_search(type, AFA_SEARCH_PREFIX, name->text, -1, NULL, NULL);
}

int AFAFactory_SuffixSearchTitle(int type, struct string *name)
{
	remember_search(type, AFA_SEARCH_SUFFIX, name);
	return afa_search(type, AFA_SEARCH_SUFFIX, name->text, -1, NULL, NULL);
}

int AFAFactory_GetCountOfSearchData(int type)
{
	if (type < 0 || type >= 16 || !afa_last_search[type].valid)
		return 0;
	return afa_search(type, afa_last_search[type].mode,
			afa_last_search[type].needle, -1, NULL, NULL);
}

struct string *AFAFactory_GetSearchTitleByIndex(int type, int index)
{
	if (type < 0 || type >= 16 || !afa_last_search[type].valid || index < 0)
		return string_ref(&EMPTY_STRING);
	struct string *found = NULL;
	afa_search(type, afa_last_search[type].mode, afa_last_search[type].needle,
			index, &found, NULL);
	return found ? found : string_ref(&EMPTY_STRING);
}

static void AFAFactory_GetSearchTitleList(int type, struct string *name, struct page **list)
{
	afa_search(type, AFA_SEARCH_EXACT, name->text, -1, NULL, list);
}

static void AFAFactory_GetPrefixSearchTitleList(int type, struct string *name, struct page **list)
{
	afa_search(type, AFA_SEARCH_PREFIX, name->text, -1, NULL, list);
}

static void AFAFactory_GetSuffixSearchTitleList(int type, struct string *name, struct page **list)
{
	afa_search(type, AFA_SEARCH_SUFFIX, name->text, -1, NULL, list);
}

HLL_LIBRARY(AFAFactory,
	    HLL_EXPORT(IsExistArchive, AFAFactory_IsExistArchive),
	    HLL_EXPORT(LoadArchive, AFAFactory_LoadArchive),
	    HLL_EXPORT(GetCountOfData, AFAFactory_GetCountOfData),
	    HLL_EXPORT(GetTitleByIndex, AFAFactory_GetTitleByIndex),
	    HLL_EXPORT(SearchTitle, AFAFactory_SearchTitle),
	    HLL_EXPORT(PrefixSearchTitle, AFAFactory_PrefixSearchTitle),
	    HLL_EXPORT(SuffixSearchTitle, AFAFactory_SuffixSearchTitle),
	    HLL_EXPORT(GetCountOfSearchData, AFAFactory_GetCountOfSearchData),
	    HLL_EXPORT(GetSearchTitleByIndex, AFAFactory_GetSearchTitleByIndex),
	    HLL_EXPORT(GetSearchTitleList, AFAFactory_GetSearchTitleList),
	    HLL_EXPORT(GetPrefixSearchTitleList, AFAFactory_GetPrefixSearchTitleList),
	    HLL_EXPORT(GetSuffixSearchTitleList, AFAFactory_GetSuffixSearchTitleList)
	    );
