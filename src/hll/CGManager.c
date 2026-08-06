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

#include "system4/cg.h"
#include "system4/string.h"
#include "asset_manager.h"
#include "xsystem4.h"
#include "hll.h"

static bool CGManager_Init(void *imain_system, int cg_cache_size)
{
	return true;
}

static bool CGManager_LoadArchive(struct string *archive_name)
{
	char *name = gamedir_path(archive_name->text);
	bool r = asset_manager_load_archive(ASSET_CG, name);
	free(name);
	return r;
}

static bool CGManager_IsExist(struct string *cg_name)
{
	if (!cg_name)
		return false;
	return asset_exists_by_name(ASSET_CG, cg_name->text, NULL);
}

static bool CGManager_GetInfo(struct string *cg_name, int *width, int *height, int *bpp, bool *exist_pixel, bool *exist_alpha)
{
	struct cg_metrics metrics;
	if (!cg_name || !asset_cg_get_metrics_by_name(cg_name->text, &metrics))
		return false;
	*width = metrics.w;
	*height = metrics.h;
	*bpp = metrics.bpp;
	*exist_pixel = metrics.has_pixel;
	*exist_alpha = metrics.has_alpha;
	return true;
}

/*
 * ★ФОРМЫ IXSEAL (v14). Библиотека переименовала и раздробила запросы метрик, а
 * линковка идёт ПО ИМЕНИ, поэтому это чистое добавление — старые `IsExist`/
 * `GetInfo` остаются нетронутыми (у Tsumamigui 3 объявлены ровно они две,
 * у Dohna — ровно четыре новые; тул ainlibbyname):
 *   v7:  bool IsExist(string)          bool GetInfo(string, ref int ×3, ref bool ×2)
 *   v14: bool IsExistCG(string)        void GetSize(string, wrap<int>, wrap<int>)
 *                                      int  GetWidth(string) / GetHeight(string)
 * Первым же сайтом стал `Ａ＿ＣＧ存在確認` <- PartsHelper::ExistsCg <-
 * AdvNamePlate@IsExistFaceCg: табличка с именем говорящего проверяет, есть ли
 * у персонажа портрет-лицо.
 *
 * Несуществующий CG: размеры 0 (а не мусор из неинициализированной метрики) —
 * ровно то, что видит игра, когда спрашивает размер отсутствующего файла.
 */
static bool CGManager_IsExistCG(struct string *cg_name)
{
	return CGManager_IsExist(cg_name);
}

static void CGManager_GetSize(struct string *cg_name, int *width, int *height)
{
	struct cg_metrics metrics;
	if (!cg_name || !asset_cg_get_metrics_by_name(cg_name->text, &metrics)) {
		if (width) *width = 0;
		if (height) *height = 0;
		return;
	}
	if (width) *width = metrics.w;
	if (height) *height = metrics.h;
}

static int CGManager_GetWidth(struct string *cg_name)
{
	int w = 0, h;
	CGManager_GetSize(cg_name, &w, &h);
	return w;
}

static int CGManager_GetHeight(struct string *cg_name)
{
	int w, h = 0;
	CGManager_GetSize(cg_name, &w, &h);
	return h;
}

//static int CGManager_GetCountOfDataFromArchive(void);
//static void CGManager_GetTitleByIndexFromArchive(int index, struct string **cg_name);
//static int CGManager_SearchTitleFromArchive(struct string *cg_name);
//static int CGManager_PrefixSearchTitleFromArchive(struct string *cg_name);
//static int CGManager_SuffixSearchTitleFromArchive(struct string *cg_name);
//static int CGManager_GetCountOfSearchDataFromArchive(void);
//static void CGManager_GetSearchTitleByIndexFromArchive(int index, struct string **cg_name);

HLL_LIBRARY(CGManager,
	    HLL_EXPORT(Init, CGManager_Init),
	    HLL_EXPORT(LoadArchive, CGManager_LoadArchive),
	    HLL_EXPORT(IsExist, CGManager_IsExist),
	    HLL_EXPORT(GetInfo, CGManager_GetInfo),
	    HLL_EXPORT(IsExistCG, CGManager_IsExistCG),
	    HLL_EXPORT(GetSize, CGManager_GetSize),
	    HLL_EXPORT(GetWidth, CGManager_GetWidth),
	    HLL_EXPORT(GetHeight, CGManager_GetHeight),
	    HLL_TODO_EXPORT(GetCountOfDataFromArchive, CGManager_GetCountOfDataFromArchive),
	    HLL_TODO_EXPORT(GetTitleByIndexFromArchive, CGManager_GetTitleByIndexFromArchive),
	    HLL_TODO_EXPORT(SearchTitleFromArchive, CGManager_SearchTitleFromArchive),
	    HLL_TODO_EXPORT(PrefixSearchTitleFromArchive, CGManager_PrefixSearchTitleFromArchive),
	    HLL_TODO_EXPORT(SuffixSearchTitleFromArchive, CGManager_SuffixSearchTitleFromArchive),
	    HLL_TODO_EXPORT(GetCountOfSearchDataFromArchive, CGManager_GetCountOfSearchDataFromArchive),
	    HLL_TODO_EXPORT(GetSearchTitleByIndexFromArchive, CGManager_GetSearchTitleByIndexFromArchive));

