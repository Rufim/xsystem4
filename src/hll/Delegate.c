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
 * Библиотека `Delegate` (Ixseal, System 4 v14) — методы делегата, которые в
 * старых играх выражались опкодами DG_SET/DG_ADD/DG_NUMOF/DG_ERASE/DG_CLEAR.
 * Тонкая обёртка над готовым delegate_*-API движка (include/vm/page.h).
 *
 * Формы аргументов:
 *  • ресивер — `ref delegate` (тип 67): ОДИН слот с heap-индексом страницы
 *    (сайт кладёт `X_REF 1`), маршалится в `struct page **` с обратной записью
 *    (см. ffi.c) — она обязательна, delegate_append() перевыделяет страницу;
 *  • обработчик — `AIN_HLL_FUNC` (тип 95): пара слотов (страница объекта, номер
 *    функции), как у лямбд Array. Сайт `_system::detail::Init` @0x49AA40:
 *    `S_PUSH "SystemInitializedEvent"; PUSH -1; X_MOV 2 1; DG_STR_TO_METHOD 242`
 *    даёт ровно `(-1, fno)` — receiver отсутствует, это свободная функция.
 *
 * Реализованы функции, у которых есть сайты в Dohna (Empty 765, Set 351,
 * Clear 97, Numof 8, Add 3), плюс Erase/IsExist — у них та же, УЖЕ доказанная
 * форма аргументов и прямое соответствие delegate_erase/delegate_contains.
 * `Equals` (аргумент `wrap<delegate>`, форма не встречалась) и `ToString`
 * (формат строки ничем не задан) оставлены HLL_TODO_EXPORT — пусть падают
 * заметно, а не врут.
 */

#include "hll.h"
#include "vm.h"
#include "vm/page.h"
#include "vm/heap.h"

static void Delegate_Set(struct page **self, union vm_value *fn)
{
	if (!self || !fn)
		return;
	// «Set», а не «Add»: содержимое заменяется целиком (в старых играх это
	// опкод DG_SET). delegate_clear() оставляет страницу валидной и пустой.
	*self = delegate_clear(*self);
	*self = delegate_append(*self, fn[0].i, fn[1].i, vm_lambda_capture_env(fn[1].i));
}

static void Delegate_Add(struct page **self, union vm_value *fn)
{
	if (!self || !fn)
		return;
	*self = delegate_append(*self, fn[0].i, fn[1].i, vm_lambda_capture_env(fn[1].i));
}

static int Delegate_Numof(struct page **self)
{
	return (self && *self) ? delegate_numof(*self) : 0;
}

static bool Delegate_Empty(struct page **self)
{
	return Delegate_Numof(self) == 0;
}

static bool Delegate_IsExist(struct page **self, union vm_value *fn)
{
	if (!self || !*self || !fn)
		return false;
	return delegate_contains(*self, fn[0].i, fn[1].i);
}

static void Delegate_Erase(struct page **self, union vm_value *fn)
{
	if (!self || !*self || !fn)
		return;
	delegate_erase(*self, fn[0].i, fn[1].i);
}

static void Delegate_Clear(struct page **self)
{
	if (self && *self)
		*self = delegate_clear(*self);
}

//static bool Delegate_Equals(struct page **self, struct page **src);
//static struct string *Delegate_ToString(struct page **self);

HLL_LIBRARY(Delegate,
	    HLL_EXPORT(Set, Delegate_Set),
	    HLL_EXPORT(Add, Delegate_Add),
	    HLL_EXPORT(Numof, Delegate_Numof),
	    HLL_EXPORT(Empty, Delegate_Empty),
	    HLL_TODO_EXPORT(Equals, Delegate_Equals),
	    HLL_EXPORT(IsExist, Delegate_IsExist),
	    HLL_EXPORT(Erase, Delegate_Erase),
	    HLL_EXPORT(Clear, Delegate_Clear),
	    HLL_TODO_EXPORT(ToString, Delegate_ToString)
	);
