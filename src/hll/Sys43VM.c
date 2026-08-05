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
 * Библиотека `Sys43VM` (Ixseal) — рефлексия и счётчики самого рантайма.
 *
 * `GetFunctionNameList` реально нужна игре: `activity::detail::AddUserComponent`
 * (@0x27a56) берёт ПОЛНЫЙ список имён функций и отбирает те, что содержат
 * `"::GetComponentName"` (`String.Contains`), а затем делает из каждой делегат
 * через `DG_STR_TO_METHOD` — так игра находит свои пользовательские компоненты.
 * Значит список должен содержать ВСЕ функции .ain, в том виде (SJIS), в каком
 * имена лежат в .ain: игра сравнивает их как строки и потом ищет функцию по
 * этому же имени.
 *
 * Профайлер (`Begin/EndProfiler`) — no-op: у движка своего профайлера нет, а
 * прочитать результат игра может только через `GetActualNumofPage/MemorySize`,
 * у которых сайтов нет вовсе. Их и оставляем HLL_TODO_EXPORT: что именно они
 * считают («страницы» кучи VM или страницы ОС) по байткоду не установлено.
 */

#include <string.h>

#include "system4/ain.h"
#include "system4/string.h"

#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "hll.h"

static bool Sys43VM_GetFunctionNameList(struct page **out)
{
	if (!out)
		return false;
	// Старое содержимое освобождаем целиком: тип страницы у пустого
	// generic-контейнера ещё не выставлен (a_type — заглушка), поэтому проще
	// отдать вызывающему свежую ТИПИЗИРОВАННУЮ страницу, чем доращивать чужую.
	// Обратную запись делает ffi (ветка wrap<объект>).
	if (*out) {
		delete_page_vars(*out);
		free_page(*out);
		*out = NULL;
	}
	union vm_value dim = { .i = 0 };
	struct page *a = alloc_array(1, &dim, AIN_ARRAY_STRING, 0, false);
	for (int i = 0; i < ain->nr_functions; i++) {
		const char *name = ain->functions[i].name;
		if (!name)
			continue;
		union vm_value v = { .i = heap_alloc_string(make_string(name, strlen(name))) };
		a = array_pushback_n(a, &v, 1, AIN_ARRAY_STRING, 0);
	}
	*out = a;
	return true;
}

static bool Sys43VM_BeginProfiler(void) { return true; }
static bool Sys43VM_EndProfiler(void) { return true; }

//static int Sys43VM_GetActualNumofPage(void);
//static int Sys43VM_GetActualMemorySize(void);

HLL_LIBRARY(Sys43VM,
	    HLL_EXPORT(GetFunctionNameList, Sys43VM_GetFunctionNameList),
	    HLL_EXPORT(BeginProfiler, Sys43VM_BeginProfiler),
	    HLL_EXPORT(EndProfiler, Sys43VM_EndProfiler),
	    HLL_TODO_EXPORT(GetActualNumofPage, Sys43VM_GetActualNumofPage),
	    HLL_TODO_EXPORT(GetActualMemorySize, Sys43VM_GetActualMemorySize)
	);
