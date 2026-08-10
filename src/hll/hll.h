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

#ifndef SYSTEM4_HLL_H
#define SYSTEM4_HLL_H

/*
 * DSL for implementing libraries.
 */

#include "vm.h"
#include "system4.h"

void static_library_replace(struct static_library *lib, const char *name, void *fun);

/*
 * Число аргументов, объявленное в .ain для ВЫПОЛНЯЕМОГО СЕЙЧАС HLL-вызова.
 * Линковка сопоставляет функции по имени, поэтому все перегрузки библиотеки
 * (напр. `String.GetPart(index)` и `String.GetPart(index, len)`) делят один
 * C-указатель, а cif у каждой свой. Читать параметр, которого нет в cif,
 * нельзя — этот счётчик позволяет реализации узнать свою перегрузку.
 */
extern int hll_current_nr_args;

/*
 * Текущая объявленная HLL-функция (та же перегрузка, что вызывается сейчас).
 * Нужна там, где перегрузки отличаются ТИПАМИ аргументов при ОДИНАКОВОЙ арности
 * и одинаковом типе возврата — hll_current_nr_args там бессилен, а декорация
 * имени (HLL_EXPORT_F) различает только float-возврат. Пример: Ixseal объявляет
 * `Array.Find(self, значение)` и `Array.Find(self, предикат)` — обе с двумя
 * аргументами и int-возвратом, отличается лишь тип второго (74 против 95).
 */
extern struct ain_hll_function *hll_current_fn;

#define HLL_WARN_UNIMPLEMENTED(rval, rtype, libname, fname, ...)	\
	static rtype libname ## _ ## fname(__VA_ARGS__) {		\
		WARNING("Unimplemented HLL function: " #libname "." #fname); \
		return rval;						\
	}

#define HLL_QUIET_UNIMPLEMENTED(rval, rtype, libname, fname, ...)	\
	static rtype libname ## _ ## fname(__VA_ARGS__) {		\
		return rval;						\
	}

#define HLL_EXPORT(fname, funptr) { .name = #fname, .fun = funptr }
/*
 * Перегрузка, которая отличается от одноимённой ТИПОМ, а не числом аргументов
 * (hll_current_nr_args тут не помогает — арность у них совпадает, а cif строится
 * по .ain, так что int- и float-вариант нельзя обслужить одной C-функцией).
 * Ixseal объявляет такие пары в Math: `Abs(int)->int` и `Abs(float)->float`,
 * `Min/Max/Clamp` в обоих типах. Линковка (ffi.c) для функции, ВОЗВРАЩАЮЩЕЙ
 * float, сначала ищет декорированное имя `Имя@f` и только потом обычное.
 */
#define HLL_EXPORT_F(fname, funptr) { .name = #fname "@f", .fun = funptr }
/*
 * Перегрузка по ЧИСЛУ АРГУМЕНТОВ, у которой ПОЗИЦИИ параметров разъезжаются, —
 * `hll_current_nr_args` тут тоже не спасает (им можно лишь не читать непереданный
 * хвост, как в String.c, а тут конфликтует ТИП одного и того же по счёту
 * параметра). Образец: Ixseal объявляет четыре `Array.Copy` —
 * `(refarray,wrap)`, `(refarray,int,wrap)`, `(refarray,wrap,int,int)` и
 * `(refarray,int,wrap,int,int)`: во втором параметре то приёмник, то индекс.
 * Линковка (ffi.c) сначала ищет имя, декорированное АРНОСТЬЮ (`Имя@<n>`), и лишь
 * потом `Имя@f` / обычное `Имя`.
 */
#define HLL_EXPORT_N(fname, nargs, funptr) { .name = #fname "@" #nargs, .fun = funptr }
/*
 * Перегрузка ОДНОЙ АРНОСТИ, различимая только ТИПОМ аргумента: `Имя@<n>f` берётся,
 * когда среди аргументов есть лямбда (`AIN_HLL_FUNC`). Образец: у Dohna три
 * `Array.Erase` — `(refarray,int,int)`, `(refarray,hll_func)` и
 * `(refarray,wrap<array>)`; вторая и третья обе двухаргументные, и по одной лишь
 * арности их не развести. Из-за этого `GameConfig@EraseShortcut` (удаление ярлыка
 * ADV предикатом) уходил в вариант «по индексу» и не удалял НИЧЕГО: галочка в
 * System Menu снималась, а список ярлыков оставался прежним.
 */
#define HLL_EXPORT_NF(fname, nargs, funptr) { .name = #fname "@" #nargs "f", .fun = funptr }
#define HLL_TODO_EXPORT(fname, funptr) { .name = #fname, .fun = NULL }

#define HLL_LIBRARY(lname, ...)				\
	struct static_library lib_ ## lname = {		\
		.name = #lname,				\
		.functions = {				\
			__VA_ARGS__,			\
			{ .name = NULL, .fun = NULL }	\
		}					\
	}

#endif /* SYSTEM4_HLL_H */
