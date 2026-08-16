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

#include <limits.h>
#include <string.h>

#include "system4/string.h"

#include "vm.h"
#include "hll.h"
#include "vm/page.h"
#include "vm/heap.h"
#include "xsystem4.h"

static void check_array(struct page *array)
{
	if (array->type != ARRAY_PAGE || array->a_type != AIN_ARRAY_INT || array->array.rank != 1)
		VM_ERROR("Not a flat integer array");
}

//void Array_NV_copy(struct page **nArray, int nNum);
//void Array_NV_add(struct page **nArray, int nNum);
//void Array_NV_sub(struct page **nArray, int nNum);
//void Array_NV_mul(struct page **nArray, int nNum);
//void Array_NV_div(struct page **nArray, int nNum);
//void Array_NV_and(struct page **nArray, int nNum);
//void Array_NV_or(struct page **nArray, int nNum);
//void Array_NV_xor(struct page **nArray, int nNum);
//void Array_NV_min(struct page **nArray, int nNum);
//void Array_NV_max(struct page **nArray, int nNum);
//void Array_NN_copy(struct page **nArray, struct page *nArrayS);
//void Array_NN_add(struct page **nArray, struct page *nArrayS);
//void Array_NN_sub(struct page **nArray, struct page *nArrayS);
//void Array_NN_mul(struct page **nArray, struct page *nArrayS);
//void Array_NN_div(struct page **nArray, struct page *nArrayS);
//void Array_NN_and(struct page **nArray, struct page *nArrayS);
//void Array_NN_or(struct page **nArray, struct page *nArrayS);
//void Array_NN_xor(struct page **nArray, struct page *nArrayS);
//void Array_NN_min(struct page **nArray, struct page *nArrayS);
//void Array_NN_max(struct page **nArray, struct page *nArrayS);
//void Array_NS_copy(struct page **nArray, struct page *sArray, int nMember);
//void Array_NS_add(struct page **nArray, struct page *sArray, int nMember);
//void Array_NS_sub(struct page **nArray, struct page *sArray, int nMember);
//void Array_NS_mul(struct page **nArray, struct page *sArray, int nMember);
//void Array_NS_div(struct page **nArray, struct page *sArray, int nMember);
//void Array_NS_and(struct page **nArray, struct page *sArray, int nMember);
//void Array_NS_or(struct page **nArray, struct page *sArray, int nMember);
//void Array_NS_xor(struct page **nArray, struct page *sArray, int nMember);
//void Array_NS_min(struct page **nArray, struct page *sArray, int nMember);
//void Array_NS_max(struct page **nArray, struct page *sArray, int nMember);
//void Array_SV_copy(struct page **sArray, int nMember, int nNum);
//void Array_SV_add(struct page **sArray, int nMember, int nNum);
//void Array_SV_sub(struct page **sArray, int nMember, int nNum);
//void Array_SV_mul(struct page **sArray, int nMember, int nNum);
//void Array_SV_div(struct page **sArray, int nMember, int nNum);
//void Array_SV_and(struct page **sArray, int nMember, int nNum);
//void Array_SV_or(struct page **sArray, int nMember, int nNum);
//void Array_SV_xor(struct page **sArray, int nMember, int nNum);
//void Array_SV_min(struct page **sArray, int nMember, int nNum);
//void Array_SV_max(struct page **sArray, int nMember, int nNum);
//void Array_SN_copy(struct page **sArray, int nMember, struct page *nArray);
//void Array_SN_add(struct page **sArray, int nMember, struct page *nArray);
//void Array_SN_sub(struct page **sArray, int nMember, struct page *nArray);
//void Array_SN_mul(struct page **sArray, int nMember, struct page *nArray);
//void Array_SN_div(struct page **sArray, int nMember, struct page *nArray);
//void Array_SN_and(struct page **sArray, int nMember, struct page *nArray);
//void Array_SN_or(struct page **sArray, int nMember, struct page *nArray);
//void Array_SN_xor(struct page **sArray, int nMember, struct page *nArray);
//void Array_SN_min(struct page **sArray, int nMember, struct page *nArray);
//void Array_SN_max(struct page **sArray, int nMember, struct page *nArray);
//void Array_SS_copy(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS);
//void Array_SS_add(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS);
//void Array_SS_sub(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS);
//void Array_SS_mul(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS);
//void Array_SS_div(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS);
//void Array_SS_and(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS);
//void Array_SS_or(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS);
//void Array_SS_xor(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS);
//void Array_SS_min(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS);
//void Array_SS_max(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS);

static int Array_NV_eneq(struct page *array, int num)
{
	if (!array)
		return 0;
	check_array(array);
	int count = 0;
	for (int i = 0; i < array->nr_vars; i++) {
		if (array->values[i].i == num)
			count++;
	}
	return count;
}

//int Array_NV_enne(struct page *nArray, int nNum);
//int Array_NV_enlo(struct page *nArray, int nNum);
//int Array_NV_enhi(struct page *nArray, int nNum);
//int Array_NV_enra(struct page *nArray, int nMin, int nMax);
//int Array_NN_eneq(struct page *nArray, struct page *nArrayS);
//int Array_NN_enne(struct page *nArray, struct page *nArrayS);
//int Array_NN_enlo(struct page *nArray, struct page *nArrayS);
//int Array_NN_enhi(struct page *nArray, struct page *nArrayS);
//int Array_NS_eneq(struct page *nArray, struct page *sArray, int nMember);
//int Array_NS_enne(struct page *nArray, struct page *sArray, int nMember);
//int Array_NS_enlo(struct page *nArray, struct page *sArray, int nMember);
//int Array_NS_enhi(struct page *nArray, struct page *sArray, int nMember);
//int Array_SV_eneq(struct page *sArray, int nMember, int nNum);
//int Array_SV_enne(struct page *sArray, int nMember, int nNum);
//int Array_SV_enlo(struct page *sArray, int nMember, int nNum);
//int Array_SV_enhi(struct page *sArray, int nMember, int nNum);
//int Array_SV_enra(struct page *sArray, int nMember, int nMin, int nMax);
//int Array_SN_eneq(struct page *sArray, int nMember, struct page *nArray);
//int Array_SN_enne(struct page *sArray, int nMember, struct page *nArray);
//int Array_SN_enlo(struct page *sArray, int nMember, struct page *nArray);
//int Array_SN_enhi(struct page *sArray, int nMember, struct page *nArray);
//int Array_SS_eneq(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS);
//int Array_SS_enne(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS);
//int Array_SS_enlo(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS);
//int Array_SS_enhi(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS);
//void Array_NV_cheq(struct page **nArray, int nNum, int nChg);
//void Array_NV_chne(struct page **nArray, int nNum, int nChg);
//void Array_NV_chlo(struct page **nArray, int nNum, int nChg);
//void Array_NV_chhi(struct page **nArray, int nNum, int nChg);
//void Array_NV_chra(struct page **nArray, int nMin, int nMax, int nChg);
//void Array_NN_cheq(struct page **nArray, struct page *nArrayS, int nChg);
//void Array_NN_chne(struct page **nArray, struct page *nArrayS, int nChg);
//void Array_NN_chlo(struct page **nArray, struct page *nArrayS, int nChg);
//void Array_NN_chhi(struct page **nArray, struct page *nArrayS, int nChg);
//void Array_NS_cheq(struct page **nArray, struct page *sArray, int nMember, int nChg);
//void Array_NS_chne(struct page **nArray, struct page *sArray, int nMember, int nChg);
//void Array_NS_chlo(struct page **nArray, struct page *sArray, int nMember, int nChg);
//void Array_NS_chhi(struct page **nArray, struct page *sArray, int nMember, int nChg);
//void Array_SV_cheq(struct page **sArray, int nMember, int nNum, int nChg);
//void Array_SV_chne(struct page **sArray, int nMember, int nNum, int nChg);
//void Array_SV_chlo(struct page **sArray, int nMember, int nNum, int nChg);
//void Array_SV_chhi(struct page **sArray, int nMember, int nNum, int nChg);
//void Array_SV_chra(struct page **sArray, int nMember, int nMin, int nMax, int nChg);
//void Array_SN_cheq(struct page **sArray, int nMember, struct page *nArray, int nChg);
//void Array_SN_chne(struct page **sArray, int nMember, struct page *nArray, int nChg);
//void Array_SN_chlo(struct page **sArray, int nMember, struct page *nArray, int nChg);
//void Array_SN_chhi(struct page **sArray, int nMember, struct page *nArray, int nChg);
//void Array_SS_cheq(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS, int nChg);
//void Array_SS_chne(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS, int nChg);
//void Array_SS_chlo(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS, int nChg);
//void Array_SS_chhi(struct page **sArray, int nMember, struct page *sArrayS, int nMemberS, int nChg);
//void Array_NV_fweq(struct page *nArray, int nNum, struct page **nArrayD);
//void Array_NV_fwne(struct page *nArray, int nNum, struct page **nArrayD);
//void Array_NV_fwlo(struct page *nArray, int nNum, struct page **nArrayD);
//void Array_NV_fwhi(struct page *nArray, int nNum, struct page **nArrayD);
//void Array_NV_fwra(struct page *nArray, int nMin, int nMax, struct page **nArrayD);
//void Array_NV_faeq(struct page *nArray, int nNum, struct page **nArrayD);
//void Array_NV_fane(struct page *nArray, int nNum, struct page **nArrayD);
//void Array_NV_falo(struct page *nArray, int nNum, struct page **nArrayD);
//void Array_NV_fahi(struct page *nArray, int nNum, struct page **nArrayD);
//void Array_NV_fara(struct page *nArray, int nMin, int nMax, struct page **nArrayD);
//void Array_NV_foeq(struct page *nArray, int nNum, struct page **nArrayD);
//void Array_NV_fone(struct page *nArray, int nNum, struct page **nArrayD);
//void Array_NV_folo(struct page *nArray, int nNum, struct page **nArrayD);
//void Array_NV_fohi(struct page *nArray, int nNum, struct page **nArrayD);
//void Array_NV_fora(struct page *nArray, int nMin, int nMax, struct page **nArrayD);
//void Array_NN_fweq(struct page *nArray, struct page *nArrayS, struct page **nArrayD);
//void Array_NN_fwne(struct page *nArray, struct page *nArrayS, struct page **nArrayD);
//void Array_NN_fwlo(struct page *nArray, struct page *nArrayS, struct page **nArrayD);
//void Array_NN_fwhi(struct page *nArray, struct page *nArrayS, struct page **nArrayD);
//void Array_NN_faeq(struct page *nArray, struct page *nArrayS, struct page **nArrayD);
//void Array_NN_fane(struct page *nArray, struct page *nArrayS, struct page **nArrayD);
//void Array_NN_falo(struct page *nArray, struct page *nArrayS, struct page **nArrayD);
//void Array_NN_fahi(struct page *nArray, struct page *nArrayS, struct page **nArrayD);
//void Array_NN_foeq(struct page *nArray, struct page *nArrayS, struct page **nArrayD);
//void Array_NN_fone(struct page *nArray, struct page *nArrayS, struct page **nArrayD);
//void Array_NN_folo(struct page *nArray, struct page *nArrayS, struct page **nArrayD);
//void Array_NN_fohi(struct page *nArray, struct page *nArrayS, struct page **nArrayD);
//void Array_NS_fweq(struct page *nArray, struct page *sArray, int nMember, struct page **nArrayD);
//void Array_NS_fwne(struct page *nArray, struct page *sArray, int nMember, struct page **nArrayD);
//void Array_NS_fwlo(struct page *nArray, struct page *sArray, int nMember, struct page **nArrayD);
//void Array_NS_fwhi(struct page *nArray, struct page *sArray, int nMember, struct page **nArrayD);
//void Array_NS_faeq(struct page *nArray, struct page *sArray, int nMember, struct page **nArrayD);
//void Array_NS_fane(struct page *nArray, struct page *sArray, int nMember, struct page **nArrayD);
//void Array_NS_falo(struct page *nArray, struct page *sArray, int nMember, struct page **nArrayD);
//void Array_NS_fahi(struct page *nArray, struct page *sArray, int nMember, struct page **nArrayD);
//void Array_NS_foeq(struct page *nArray, struct page *sArray, int nMember, struct page **nArrayD);
//void Array_NS_fone(struct page *nArray, struct page *sArray, int nMember, struct page **nArrayD);
//void Array_NS_folo(struct page *nArray, struct page *sArray, int nMember, struct page **nArrayD);
//void Array_NS_fohi(struct page *nArray, struct page *sArray, int nMember, struct page **nArrayD);
//void Array_SV_fweq(struct page *sArray, int nMember, int nNum, struct page **nArrayD);
//void Array_SV_fwne(struct page *sArray, int nMember, int nNum, struct page **nArrayD);
//void Array_SV_fwlo(struct page *sArray, int nMember, int nNum, struct page **nArrayD);
//void Array_SV_fwhi(struct page *sArray, int nMember, int nNum, struct page **nArrayD);
//void Array_SV_fwra(struct page *sArray, int nMember, int nMin, int nMax, struct page **nArrayD);
//void Array_SV_faeq(struct page *sArray, int nMember, int nNum, struct page **nArrayD);
//void Array_SV_fane(struct page *sArray, int nMember, int nNum, struct page **nArrayD);
//void Array_SV_falo(struct page *sArray, int nMember, int nNum, struct page **nArrayD);
//void Array_SV_fahi(struct page *sArray, int nMember, int nNum, struct page **nArrayD);
//void Array_SV_fara(struct page *sArray, int nMember, int nMin, int nMax, struct page **nArrayD);
//void Array_SV_foeq(struct page *sArray, int nMember, int nNum, struct page **nArrayD);
//void Array_SV_fone(struct page *sArray, int nMember, int nNum, struct page **nArrayD);
//void Array_SV_folo(struct page *sArray, int nMember, int nNum, struct page **nArrayD);
//void Array_SV_fohi(struct page *sArray, int nMember, int nNum, struct page **nArrayD);
//void Array_SV_fora(struct page *sArray, int nMember, int nMin, int nMax, struct page **nArrayD);
//void Array_SN_fweq(struct page *sArray, int nMember, struct page *nArrayS, struct page **nArrayD);
//void Array_SN_fwne(struct page *sArray, int nMember, struct page *nArrayS, struct page **nArrayD);
//void Array_SN_fwlo(struct page *sArray, int nMember, struct page *nArrayS, struct page **nArrayD);
//void Array_SN_fwhi(struct page *sArray, int nMember, struct page *nArrayS, struct page **nArrayD);
//void Array_SN_faeq(struct page *sArray, int nMember, struct page *nArrayS, struct page **nArrayD);
//void Array_SN_fane(struct page *sArray, int nMember, struct page *nArrayS, struct page **nArrayD);
//void Array_SN_falo(struct page *sArray, int nMember, struct page *nArrayS, struct page **nArrayD);
//void Array_SN_fahi(struct page *sArray, int nMember, struct page *nArrayS, struct page **nArrayD);
//void Array_SN_foeq(struct page *sArray, int nMember, struct page *nArrayS, struct page **nArrayD);
//void Array_SN_fone(struct page *sArray, int nMember, struct page *nArrayS, struct page **nArrayD);
//void Array_SN_folo(struct page *sArray, int nMember, struct page *nArrayS, struct page **nArrayD);
//void Array_SN_fohi(struct page *sArray, int nMember, struct page *nArrayS, struct page **nArrayD);
//void Array_SS_fweq(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS, struct page **nArrayD);
//void Array_SS_fwne(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS, struct page **nArrayD);
//void Array_SS_fwlo(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS, struct page **nArrayD);
//void Array_SS_fwhi(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS, struct page **nArrayD);
//void Array_SS_faeq(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS, struct page **nArrayD);
//void Array_SS_fane(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS, struct page **nArrayD);
//void Array_SS_falo(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS, struct page **nArrayD);
//void Array_SS_fahi(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS, struct page **nArrayD);
//void Array_SS_foeq(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS, struct page **nArrayD);
//void Array_SS_fone(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS, struct page **nArrayD);
//void Array_SS_folo(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS, struct page **nArrayD);
//void Array_SS_fohi(struct page *sArray, int nMember, struct page *sArrayS, int nMemberS, struct page **nArrayD);

static int Array_NV_sceq(struct page *array, int index, int num, int *out_index)
{
	if (!array)
		return 0;
	check_array(array);

	if (index >= 0) {
		for (int i = index; i < array->nr_vars; i++) {
			if (array->values[i].i == num) {
				*out_index = i;
				return 1;
			}
		}
	} else {
		// Negative index means backwards search
		index &= 0x7fffffff;
		if (index >= array->nr_vars)
			return 0;
		for (int i = index; i >= 0; --i) {
			if (array->values[i].i == num) {
				*out_index = i;
				return 1;
			}
		}
	}
	return 0;

}

//int Array_NV_scne(struct page *nArray, int nIndex, int nNum, int *pnIndex);
//int Array_NV_sclo(struct page *nArray, int nIndex, int nNum, int *pnIndex);
//int Array_NV_schi(struct page *nArray, int nIndex, int nNum, int *pnIndex);
//int Array_NV_scra(struct page *nArray, int nIndex, int nMin, int nMax, int *pnIndex);
//int Array_SV_sceq(struct page *sArray, int nMember, int nIndex, int nNum, int *pnIndex);
//int Array_SV_scne(struct page *sArray, int nMember, int nIndex, int nNum, int *pnIndex);
//int Array_SV_sclo(struct page *sArray, int nMember, int nIndex, int nNum, int *pnIndex);
//int Array_SV_schi(struct page *sArray, int nMember, int nIndex, int nNum, int *pnIndex);
//int Array_SV_scra(struct page *sArray, int nMember, int nIndex, int nMin, int nMax, int *pnIndex);
//int Array_NN_sclowest(struct page *nArray, struct page **nArrayD, int *pnIndex);
//int Array_NN_schighest(struct page *nArray, struct page **nArrayD, int *pnIndex);
//int Array_NS_sclowest(struct page *nArray, struct page **sArray, int nMember, int *pnIndex);
//int Array_NS_schighest(struct page *nArray, struct page **sArray, int nMember, int *pnIndex);
//int Array_SN_sclowest(struct page *sArray, int nMember, struct page **nArray, int *pnIndex);
//int Array_SN_schighest(struct page *sArray, int nMember, struct page **nArray, int *pnIndex);
//int Array_SS_sclowest(struct page *sArray, int nMember, struct page **sArrayD, int nMemberS, int *pnIndex);
//int Array_SS_schighest(struct page *sArray, int nMember, struct page **sArrayD, int nMemberS, int *pnIndex);
//int Array_VN_add(struct page *nArray);
//int Array_VN_and(struct page *nArray);
//int Array_VN_or(struct page *nArray);
//int Array_VS_add(struct page *sArray, int nMember);
//int Array_VS_and(struct page *sArray, int nMember);
//int Array_VS_or(struct page *sArray, int nMember);

/*
 * Ixseal generic container API. Newer System 4 games (Healing Touch, Dohna
 * Dohna, …) replaced the array bytecode instructions with this Array library;
 * each method takes the array variable's page by reference as `self`. We wire
 * the methods onto the array_* helpers in page.c, reading the element data
 * type / struct type / rank from the array page itself. Generic element values
 * arrive as a pointer to their raw stack slot (AIN_HLL_PARAM). Comparator and
 * predicate overloads (taking an AIN_HLL_FUNC) are intentionally routed to the
 * same default-order implementations: we never invoke a VM callback, so a
 * missing/garbage function index can never be dereferenced — custom orderings
 * simply degrade to the built-in element comparison.
 */
static int ix_rank(struct page *a) { return (a && a->type == ARRAY_PAGE && a->array.rank > 0) ? a->array.rank : 1; }
static enum ain_data_type ix_dtype(struct page *a) { return (a && a->type == ARRAY_PAGE) ? a->a_type : AIN_ARRAY_INT; }
static int ix_stype(struct page *a) { return (a && a->type == ARRAY_PAGE) ? a->array.struct_type : 0; }

static void ix_resize(struct page **self, int n)
{
	if (!self)
		return;
	// Тип запоминаем ДО realloc: при сжатии в 0 realloc_array освобождает
	// страницу в NULL и тип элемента теряется. Для Ixseal-generic-контейнера
	// пустой массив ДОЛЖЕН оставаться валидной 0-элементной ТИПИЗИРОВАННОЙ
	// страницей — иначе последующий PushBack/EmplaceBack на NULL создаёт массив
	// с дефолт-типом (int) и кладёт слот объекта как сырой int (без владения) →
	// висячий слот → use-after-free (CPartsMessageManager: Clear/Free пула →
	// затем NEW+PushBack элемента). Тот же принцип, что в Array_PopBack.
	enum ain_data_type dt = ix_dtype(*self);
	int st = ix_stype(*self);
	int rank = ix_rank(*self);
	union vm_value dim = { .i = n < 0 ? 0 : n };
	struct page *a = realloc_array(*self, rank, &dim, dt, st, true);
	if (!a && dim.i == 0)
		a = alloc_array(rank, &dim, dt, st, false);
	if (getenv("XSYS4_ARRAY_TRACE"))
		NOTICE("ARRAYTRACE resize -> %d: было %d, стало %d (dt=%d st=%d rank=%d)",
		       n, *self ? array_numof(*self, 1) : -1, a ? array_numof(a, 1) : -1,
		       dt, st, rank);
	*self = a;
}

/*
 * Стереть элемент, СОХРАНИВ типизацию опустевшего контейнера.
 *
 * `array_erase`, удаляя ПОСЛЕДНИЙ элемент, освобождает страницу и возвращает
 * NULL — вместе с ней теряется объявленный тип элемента. Для generic-контейнера
 * Ixseal это тот же инвариант, что уже соблюдают `ix_resize` и `Array_PopBack`:
 * пустой контейнер обязан остаться валидной 0-элементной ТИПИЗИРОВАННОЙ
 * страницей, иначе следующий `PushBack`/`EmplaceBack` создаст массив с
 * дефолт-типом int и положит heap-слот объекта СЫРЫМ int'ом, без владения —
 * первый же `DELETE` временной ссылки игры уносит элемент, слот
 * переиспользуется под другой объект, и обращение к элементу читает чужую
 * страницу.
 *
 * Именно так падал титул Dohna: `Motion::ExecuterCollection@Add` сначала зовёт
 * `EraseEndTask` (= `Array.EraseAll` по предикату), коллекция пустела в NULL,
 * следующий `PushBack` пересоздавал её int-массивом (проверено: `a_type=14
 * struct=0` вместо `17/611`), и `Motion::Executer@IsAlive::get` читал член 5
 * чужой 2-членной страницы → `Out of bounds page index: 17670/5`. Тем же
 * способом ломался `EndEventCallbackCollection`.
 *
 * Правка Ixseal-only по построению: библиотека `Array` объявлена ТОЛЬКО у Dohna
 * (у Tsumamigui 3 и Escalayer её нет вообще), а легаси-опкод `A_ERASE`, где
 * NULL как признак пустого массива — штатное поведение, не встречается ни в
 * одной из трёх игр.
 */
static bool ix_erase_at(struct page **self, int index)
{
	enum ain_data_type dt = ix_dtype(*self);
	int st = ix_stype(*self);
	int rank = ix_rank(*self);
	bool ok = false;
	*self = array_erase(*self, index, &ok);
	if (!*self) {
		union vm_value dim = { .i = 0 };
		*self = alloc_array(rank, &dim, dt, st, false);
	}
	return ok;
}

static void Array_Alloc(struct page **self, int numof) { ix_resize(self, numof); }
static void Array_Realloc(struct page **self, int numof) { ix_resize(self, numof); }
static void Array_Free(struct page **self) { ix_resize(self, 0); }
static void Array_ix_Clear(struct page **self) { ix_resize(self, 0); }
static int  Array_ix_Numof(struct page **self) { return self ? array_numof(*self, 1) : 0; }
static bool Array_Empty(struct page **self) { return !self || array_numof(*self, 1) == 0; }

// Элемент generic-контейнера — heap-объект (владеет счётчиком)?
// struct/string/delegate/iface и вложенные массивы (типизир./generic).
static bool ix_elem_is_object(enum ain_data_type array_dt)
{
	enum ain_data_type et = array_type(array_dt);
	switch (et) {
	case AIN_STRUCT:
	case AIN_STRING:
	case AIN_DELEGATE:
	case AIN_IFACE:
	// wrap<интерфейс>: нижний слот пары — heap-слот объекта, владение как у
	// struct-элемента (верхний слот — просто индекс, ничем не владеет).
	case AIN_IFACE_WRAP:
	// ★wrap<структура> (AIN_WRAP) — владеющий хэндл и в КОНТЕЙНЕРЕ: пока его тут
	// не было, `Array.PushBack` не брал ссылку на элемент `array<wrap<CASTask>>`,
	// и после смерти владельца задачи элемент повисал. Живой случай (Haha Ranman,
	// конфиг → «To Title»): подтверждение `タイトルに戻る確認` → `CASTask@JoinImp`
	// перебирает parentList и читает поле у уже освобождённой страницы — движок
	// падал «Out of bounds page index» в X_REF, игра «останавливалась».
	// Симметрия с variable_fini: wrap-слоты страниц освобождаются (фикс option<>
	// той же линии), значит и контейнер обязан владеть.
	case AIN_WRAP:
	case AIN_ARRAY_TYPE:
		return true;
	default:
		return ain_is_array_data_type(et);
	}
}

// Сколько слотов значения кладёт вызывающий под элемент этого контейнера.
static int ix_value_slots(struct page **self) { return self ? array_elem_slots(*self) : 1; }

/*
 * XSYS4_ARRAY_OWN=<подстрока имени структуры> — ВЛАДЕНИЕ элементом при укладке в
 * контейнер: печатает, взял ли контейнер ссылку на объект, и какой у контейнера
 * тип элемента. Отвечает на «объект умер сразу после того, как его положили в
 * массив»: идиома игры — `NEW T(...); Add/PushBack; DELETE своей временной
 * ссылки`, и если контейнер ссылку НЕ взял, объект гибнет на возврате, а его
 * heap-слот достаётся чужой странице. Читается это очень далеко от причины
 * (поле «молча стало нулём», подписка делегата «сама отвалилась»).
 *
 * ★Фильтр по ИМЕНИ структуры, а не «похоже на объект»: числа в int-массивах
 * (индексы свободных мест в пулах) сплошь совпадают с номерами живых слотов, и
 * проверка «значение указывает на структурную страницу» даёт ложные
 * срабатывания пачками (замер: 12 из 12 первых — пулы `CPartsMessageManager`).
 */
static void ix_own_check(const char *who, struct page **self, union vm_value *value, bool took)
{
	static const char *want = (const char *)1;
	if (want == (const char *)1)
		want = getenv("XSYS4_ARRAY_OWN");
	if (!want || !*want || !self || !value)
		return;
	if (value->i <= 0 || !heap_index_valid(value->i) || !heap_slot_is_page(value->i))
		return;
	struct page *p = heap_get_page(value->i);
	if (!p || p->type != STRUCT_PAGE)
		return;
	const char *name = (p->index >= 0 && p->index < ain->nr_structures
			    && ain->structures[p->index].name)
		? ain->structures[p->index].name : "?";
	if (!strstr(name, want))
		return;
	WARNING("ARRAYOWN %s: слот %d (структура '%s') — владение %s "
		"(тип элемента контейнера %d, структура элемента %d, ранг %d, страница %s)"
		" — стек вызовов игры:",
		who, value->i, name, took ? "ВЗЯТО" : "НЕ ВЗЯТО",
		(int)ix_dtype(*self), ix_stype(*self), ix_rank(*self),
		*self ? "есть" : "ОТСУТСТВУЕТ (NULL)");
	vm_stack_trace();
}

static void Array_PushBack(struct page **self, union vm_value *value)
{
	if (!self || !value)
		return;
	// Для объектного элемента контейнер берёт СВОЙ счётчик ссылок: вызывающий
	// обычно NEW-ит объект, PushBack-ит и затем DELETE-ит свою временную ссылку
	// (CPartsMessageManager@GetFunctionSet: `NEW 328; PushBack; DELETE local`).
	// Без heap_ref DELETE уносил бы только что добавленный элемент (ref 1→0) →
	// висячий слот в пуле → use-after-free при следующем чтении пула.
	bool pb_own = ix_elem_is_object(ix_dtype(*self));
	if (pb_own)
		heap_ref(value->i);
	ix_own_check("PushBack", self, value, pb_own);
	// `value` указывает на первый из слотов значения на стеке VM: у
	// wrap<интерфейс> их два (объект, база интерфейса) — оба идут в элемент.
	int pb_slots = ix_value_slots(self);
	enum ain_data_type pb_dt = ix_dtype(*self);
	int pb_st = ix_stype(*self);
	*self = array_pushback_n(*self, value, pb_slots, pb_dt, pb_st);
	const char *w = getenv("XSYS4_PB_WATCH");
	if (w && pb_st == atoi(w)) {
		NOTICE("PBWATCH push value=%d dtype=%d stype=%d slots=%d -> nr_vars=%d elems: %d %d %d %d",
		       value->i, pb_dt, pb_st, pb_slots, *self ? (*self)->nr_vars : -1,
		       (*self && (*self)->nr_vars > 0) ? (*self)->values[0].i : -1,
		       (*self && (*self)->nr_vars > 1) ? (*self)->values[1].i : -1,
		       (*self && (*self)->nr_vars > 2) ? (*self)->values[2].i : -1,
		       (*self && (*self)->nr_vars > 3) ? (*self)->values[3].i : -1);
	}
}

static void Array_PopBack(struct page **self)
{
	if (!self || !*self)
		return;
	// Запоминаем тип ДО popback: если массив опустеет, array_popback
	// освобождает страницу и возвращает NULL. Для Ixseal-generic-контейнера
	// это недопустимо — тип элемента теряется, и следующий EmplaceBack/PushBack
	// уходит в ветку `!*self`→return 0 (материализуется как ref со слотом 0 →
	// receiver=0 → heap_unref глобальной страницы → порча кучи в CASTimerManager
	// GC). Пустой контейнер должен оставаться валидной 0-элементной ТИПИЗИРОВАННОЙ
	// страницей.
	enum ain_data_type dt = ix_dtype(*self);
	int st = ix_stype(*self);
	int rank = ix_rank(*self);
	struct page *a = array_popback(*self);
	if (!a) {
		union vm_value dim = { .i = 0 };
		a = alloc_array(rank, &dim, dt, st, false);
	}
	*self = a;
}

static void Array_ix_Insert(struct page **self, int index, union vm_value *value)
{
	if (!self || !value)
		return;
	// см. Array_PushBack: объектный элемент — контейнер владеет своим счётчиком.
	bool ins_own = ix_elem_is_object(ix_dtype(*self));
	if (ins_own)
		heap_ref(value->i);
	ix_own_check("Insert", self, value, ins_own);
	*self = array_insert_n(*self, index, value, ix_value_slots(self), ix_dtype(*self), ix_stype(*self));
}

static bool Array_ix_Erase(struct page **self, int index, int length)
{
	if (!self || !*self)
		return false;
	bool any = false;
	int n = length > 0 ? length : 1;
	for (int k = 0; k < n; k++) {
		if (!ix_erase_at(self, index))
			break;
		any = true;
	}
	return any;
}

/*
 * `Copy(dst, dstPos, src, srcPos, count)` — пятиаргументная перегрузка (в .ain
 * `(refarray,int,wrap,int,int)`). Линковка идёт по ИМЕНИ, поэтому раньше ВСЕ
 * четыре перегрузки `Copy` попадали в двухаргументный `Array_ix_Copy(self, src)`,
 * а cif собирается по .ain — во втором параметре C-функция получала не приёмник,
 * а int-индекс, и снимок не копировался НИЧЕГО, молча и без ошибки.
 *
 * Чем это ломало Dohna: `parts::detail::CallPartsUpdateEvent` @0x2eaf32 делает
 * `X_A_INIT; Array.Realloc(list, Numof(dataList)); Array.Copy(list, 0, dataList, 0,
 * Numof(dataList))` и дальше диспатчит делегаты ИЗ СНИМКА. Снимок оставался пустым,
 * поэтому НИ ОДНО покадровое событие частей не вызывалось: `CASTask@UpdateEvent`
 * (его регистрирует `CASTask@ExecuteImp` через `AFL_Parts_AddBeginUpdateEvent`)
 * не тикал, значит не тикали таймеры и задачи — `SceneLogo@Run` вечно ждал
 * `DelayedCallback(10, ()=>ShowLogo())`, и сцена логотипа не двигалась.
 *
 * Остальные три перегрузки Dohna не зовёт (тул xscan по CALLHLL: fn33 — 3 сайта,
 * fn30/31/32 — ноль), поэтому реализуется ровно эта форма.
 */
static int Array_ix_Copy5(struct page **self, int dst_i, struct page **src, int src_i, int n)
{
	if (!self || !*self || !src || !*src || n <= 0)
		return 0;
	int dst_cap = array_numof(*self, 1) - dst_i;
	int src_cap = array_numof(*src, 1) - src_i;
	if (dst_i < 0 || src_i < 0 || dst_cap <= 0 || src_cap <= 0)
		return 0;
	if (n > dst_cap)
		n = dst_cap;
	if (n > src_cap)
		n = src_cap;
	array_copy(*self, dst_i, *src, src_i, n);
	return n;
}

static int Array_ix_Copy(struct page **self, struct page **src)
{
	if (!self || !*self || !src || !*src)
		return 0;
	int n = array_numof(*src, 1);
	int cap = array_numof(*self, 1);
	if (n > cap)
		n = cap;
	array_copy(*self, 0, *src, 0, n);
	return n;
}

/*
 * `AddRange(dst, src)` и `Concat(dst, src)` (fn13/fn12, оба `(80,82)`) —
 * ДОПИСАТЬ все элементы src В КОНЕЦ dst, а не переписать начало. Раньше обе
 * вели в `Array_ix_Copy`, который копирует лишь в пределах уже имеющейся длины
 * приёмника; `SceneWorkMatching@GetAllShop` строит список магазинов как
 * «`Add`(свой) + `AddRange`(чужие)», то есть приёмник тут длиной 1 — чужие
 * магазины не добавились бы вовсе, а свой был бы затёрт. Падало раньше: у
 * пустого приёмника, созданного `X_A_INIT 0`, тип элемента ещё не установлен,
 * и `array_copy` отказывался сверять его с типом источника.
 *
 * Элементы кладём через PushBack — он один знает про многослотовый элемент
 * (wrap<интерфейс> — два слота) и про то, что объектный элемент берёт СВОЙ
 * счётчик ссылок.
 */
static void Array_ix_AddRange(struct page **self, struct page **src)
{
	if (!self || !src || !*src)
		return;
	int n = array_numof(*src, 1);
	if (n <= 0)
		return;
	// Приёмник, ещё не знающий своего типа элемента (пустой generic-контейнер),
	// наследует тип источника: иначе PushBack положит слоты объектов как сырые
	// int'ы (без владения), и первое же чтение уйдёт в освобождённую страницу.
	if (*self && array_numof(*self, 1) == 0 && ix_dtype(*self) != ix_dtype(*src)) {
		(*self)->a_type = ix_dtype(*src);
		(*self)->array.struct_type = ix_stype(*src);
	} else if (*self && ix_dtype(*self) != ix_dtype(*src)) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			WARNING("Array.AddRange: тип приёмника %d(s%d) ≠ типа источника %d(s%d)",
				ix_dtype(*self), ix_stype(*self), ix_dtype(*src), ix_stype(*src));
		}
	}
	// Элементы кладутся слот-в-слот, поэтому ширина элемента обязана совпадать
	// (wrap<структура> и array<структура> — оба по слоту; пара wrap<интерфейс> —
	// два). Разной ширины у Dohna не встречалось; если встретится, лучше громко
	// ничего не сделать, чем сдвинуть содержимое приёмника.
	int slots = array_elem_slots(*src);
	if (*self && array_elem_slots(*self) != slots) {
		WARNING("Array.AddRange: элемент приёмника %d слотов, источника %d — пропущено",
			array_elem_slots(*self), slots);
		return;
	}
	for (int i = 0; i < n; i++)
		Array_PushBack(self, &(*src)->values[i*slots]);
}

/*
 * `void Duplicate(ref array self, wrap<array> src)` — приёмник СТАНОВИТСЯ копией
 * источника: длину задаёт ИСТОЧНИК. Тоже вело в `Array_ix_Copy` («скопировать,
 * сколько влезет в НЫНЕШНИЙ размер приёмника»), поэтому на пустом приёмнике не
 * делало ровно ничего, молча. В .ain это ТРЕТЬЯ отдельная функция — `Copy`
 * отличается даже типом возврата (int против void).
 *
 * Чем это ломало Haha Ranman: `CExecutedCommandParam@InitInstantList` делает
 * `Array.Duplicate(InstantList, List)` — снимок списка выполненных команд. `List`
 * конструктор аллоцирует на 12 элементов, `InstantList` пуст, поэтому снимок
 * оставался ПУСТЫМ. Дальше `GetList` при `Instant = true` отдаёт именно
 * `InstantList`, а `■実行済コマンド設定` @0x699aae берёт `Array.At(list, nTimezone)`
 * БЕЗ проверки (в соседней `■実行済コマンド取得` игра его проверяет на -1) и
 * присваивает по полученной ссылке — то есть по ссылке -1. Итог: SIGSEGV в
 * `free_string` уже внутри libsys4, в месте, никак не указывающем на виноватую
 * инструкцию.
 *
 * Реализовано ЧЕРЕЗ `Array_ix_AddRange`, а не своим копированием: вся возня с
 * наследованием типа элемента, многослотовыми элементами и счётчиками ссылок
 * должна жить в ОДНОМ месте.
 *
 * Частота вызовов (`alice ain dump -c` + grep CALLHLL): Haha Ranman — Duplicate 17,
 * Concat 45; Dohna — Duplicate 14, Concat 62, AddRange 16; Tsumamigui 3 не объявляет
 * ни одной, так что старых игр правка не касается.
 */
static void Array_ix_Duplicate(struct page **self, struct page **src)
{
	if (!self)
		return;
	ix_resize(self, 0);            // остаётся валидной 0-элементной страницей
	Array_ix_AddRange(self, src);
}

// XSYS4_BSEARCH_TRACE=miss печатает и переворот: по строке видно, ТУ ЛИ страницу
// переворачивают, которую потом ищут (сверять по адресу страницы и первым элементам),
// а с XSYS4_BSEARCH_FIELDS — и сами величины в элементах (объявление ниже).
static void ix_elem_fields(struct page *a, char *buf, size_t sz);

static void Array_Reverse(struct page **self)
{
	if (!self)
		return;
	array_reverse(*self);
	const char *tr = getenv("XSYS4_BSEARCH_TRACE");
	if (tr && !strcmp(tr, "miss") && *self) {
		struct page *p = *self;
		int s = array_elem_slots(p);
		char flds[1024];
		ix_elem_fields(p, flds, sizeof(flds));
		NOTICE("REVERSE page=%p n=%d slots=%d элементы: %d %d … %d%s%s", (void *)p,
		       p->nr_vars / (s ? s : 1), s,
		       p->nr_vars > 0 ? p->values[0].i : -1,
		       p->nr_vars > s ? p->values[s].i : -1,
		       p->nr_vars > 0 ? p->values[p->nr_vars - s].i : -1,
		       *flds ? " поля:" : "", flds);
	}
}
static void Array_Shuffle(struct page **self, int seed) { if (self) array_shuffle(*self, seed); }

static int Array_ix_Fill(struct page **self, union vm_value *value)
{
	if (!self || !*self || !value)
		return 0;
	return array_fill(*self, 0, array_numof(*self, 1), *value);
}

/*
 * --- Предикаты и компараторы (Ixseal, тип аргумента AIN_HLL_FUNC) ---
 *
 * Сайт кладёт лямбду ДВУМЯ слотами — (страница объекта, номер функции), —
 * и ffi отдаёт реализации указатель на эту пару (см. ffi.c). Сигнатуры лямбд
 * взяты из .ain: предикат — ОДИН аргумент-элемент и возврат bool, компаратор —
 * ДВА аргумента и тоже bool, т.е. `less(a, b)` как в std::sort.
 *
 * Перегрузки «по значению» и «по предикату» имеют ОДИНАКОВУЮ арность и
 * одинаковый тип возврата (напр. `Find(self, 74)` и `Find(self, 95)`), поэтому
 * различить их можно только по объявленным типам — через hll_current_fn.
 */
static bool ix_arg_is_func(int i)
{
	return hll_current_fn && i < (int)hll_current_fn->nr_arguments
		&& hll_current_fn->arguments[i].type.data == AIN_HLL_FUNC;
}

// Позвать предикат для элемента `index`. Аргументы лямбды — слоты элемента
// (у wrap<интерфейса> их два, и компилятор объявляет оба).
static bool ix_pred(union vm_value *fn, struct page *a, int index)
{
	int slots = array_elem_slots(a);
	int argc = vm_hll_func_nr_args(fn[1].i);
	/*
	 * ★ПРЕДИКАТ, ОБЪЯВИВШИЙ ПАРУ, ДОЛЖЕН ЕЁ И ПОЛУЧИТЬ.
	 *
	 * Аргумент-интерфейс — это два слота (объект, база интерфейса), и лямбда
	 * диспетчеризует через них: `vtable = obj[0]; fno = vtable[base + N]`.
	 * Страница же бывает однослотовой — так приходит массив, поднятый из сейва
	 * ОРИГИНАЛА (замер `XSYS4_ELEM_TRACE`: `AchievementClearTurn`, `a_type=17`).
	 * Обрезая argc до ширины СТРАНИЦЫ, мы звали предикат с одним слотом (в логе
	 * это видно как «fn 36550 объявляет 2 слотов аргументов, передано 1»), и он
	 * сравнивал мусор: поиск достижения по строковому `<Id>` не совпадал ни с
	 * одним элементом, сайт получал −1 и падал при разыменовании (§5fb-13).
	 *
	 * Недостающую базу берём из таблицы интерфейсов самого объекта — тем же
	 * правилом, что и выдача элемента наружу в ffi.c.
	 */
	if (argc == 2 && slots == 1) {
		union vm_value argv[2];
		argv[0] = a->values[index];
		argv[1] = vm_int(0);
		int obj = argv[0].i;
		if (obj > 0 && heap_slot_is_page(obj)) {
			struct page *op = heap[obj].page;
			if (op && op->type == STRUCT_PAGE && op->index >= 0
					&& op->index < ain->nr_structures) {
				struct ain_struct *st = &ain->structures[op->index];
				if (st->nr_interfaces == 1)
					argv[1] = vm_int(st->interfaces[0].vtable_offset);
			}
		}
		return vm_call_hll_func(fn, argv, 2).i != 0;
	}
	if (argc > slots)
		argc = slots;
	return vm_call_hll_func(fn, &a->values[index * slots], argc).i != 0;
}

// Компаратор `less(a, b)`: слоты обоих элементов подряд.
static bool ix_less(union vm_value *fn, struct page *a, int i, int j)
{
	int slots = array_elem_slots(a);
	union vm_value argv[4];
	int argc = vm_hll_func_nr_args(fn[1].i);
	if (argc > 2 * slots)
		argc = 2 * slots;
	for (int k = 0; k < slots && k < 2; k++) {
		argv[k] = a->values[i * slots + k];
		argv[slots + k] = a->values[j * slots + k];
	}
	bool r = vm_call_hll_func(fn, argv, argc).i != 0;
	if (getenv("XSYS4_SORT_TRACE"))
		NOTICE("SORT less(%d,%d) argc=%d obj=%d fno=%d -> %d",
		       i, j, argc, fn[0].i, fn[1].i, (int)r);
	return r;
}

// Индекс первого/последнего элемента [begin, end), удовлетворяющего предикату
// (-1 если нет).
static int ix_find_pred_range(struct page **self, union vm_value *fn, int begin, int end, bool last)
{
	if (!self || !*self || !fn)
		return -1;
	int n = array_numof(*self, 1);
	if (begin < 0)
		begin = 0;
	if (end > n)
		end = n;
	int found = -1;
	for (int i = begin; i < end; i++) {
		// Массив может быть перевыделен лямбдой — берём страницу каждый раз.
		if (!*self || i >= array_numof(*self, 1))
			break;
		if (ix_pred(fn, *self, i)) {
			found = i;
			if (!last)
				break;
		}
	}
	return found;
}

static int ix_find_pred(struct page **self, union vm_value *fn, bool last)
{
	return ix_find_pred_range(self, fn, 0, INT_MAX, last);
}

static int Array_ix_Find(struct page **self, union vm_value *search)
{
	if (!self || !*self || !search)
		return -1;
	if (ix_arg_is_func(1))
		return ix_find_pred(self, search, false);
	return array_find(*self, 0, array_numof(*self, 1), *search, 0);
}

static int Array_ix_FindLast(struct page **self, union vm_value *search)
{
	if (!self || !*self || !search)
		return -1;
	if (ix_arg_is_func(1))
		return ix_find_pred(self, search, true);
	// Значение-перегрузка: ищем последнее вхождение перебором.
	int n = array_numof(*self, 1);
	int found = -1;
	for (int i = 0; i < n; i++) {
		if (array_find(*self, i, i + 1, *search, 0) >= 0)
			found = i;
	}
	return found;
}

/*
 * Четырёхаргументные Find/FindLast(self, begin, end, search|предикат).
 * Линковка идёт по имени, поэтому раньше они попадали в двухаргументный
 * Array_ix_Find, а cif собирается по .ain — вторым C-параметром приходил
 * begin (обычно 0 → NULL) вместо search, и поиск ВСЕГДА отвечал «не найдено»,
 * молча. Чем это ломало Haha Ranman: CMessageWindowStatusManager@
 * HideMessageWindow ищет имя окна через Find(list, 0, Numof, имя) и, не найдя,
 * не удалял его из showing-списка — CreateByShowingList каждым обновлением
 * показывал окно сообщений обратно, и оно не пряталось на слайдах пролога
 * (■枠消し). Haha Ranman: Find#1 — 22 сайта; форма с предикатом (Find#3) в
 * байткоде не встречается, но различается штатно — через ix_arg_is_func.
 */
static int Array_ix_Find4(struct page **self, int begin, int end, union vm_value *search)
{
	if (!self || !*self || !search)
		return -1;
	if (ix_arg_is_func(3))
		return ix_find_pred_range(self, search, begin, end, false);
	return array_find(*self, begin, end, *search, 0);
}

static int Array_ix_FindLast4(struct page **self, int begin, int end, union vm_value *search)
{
	if (!self || !*self || !search)
		return -1;
	if (ix_arg_is_func(3))
		return ix_find_pred_range(self, search, begin, end, true);
	int found = -1;
	for (int i = begin < 0 ? 0 : begin; i < end; i++) {
		if (array_find(*self, i, i + 1, *search, 0) >= 0)
			found = i;
	}
	return found;
}

static bool Array_ix_IsExist(struct page **self, union vm_value *search)
{
	return Array_ix_Find(self, search) >= 0;
}

// bool Any(self) — «есть хоть один элемент»; bool Any(self, предикат) — «есть
// подходящий». bool All(self, предикат) — «все подходят» (на пустом — true,
// как принято у all_of).
static bool Array_ix_Any(struct page **self, union vm_value *fn)
{
	bool with_pred = hll_current_nr_args >= 2 && ix_arg_is_func(1);
	bool r = with_pred ? ix_find_pred(self, fn, false) >= 0
			   : (self && *self && array_numof(*self, 1) > 0);
	/*
	 * XSYS4_ANY_TRACE=<подстрока имени функции игры> — размер массива и ответ
	 * `Any`. Нужен, когда игра принимает решение по «есть ли хоть один
	 * подходящий», а решение выходит неверным: по одному ответу не понять, пуст
	 * ли контейнер или предикат не сработал ни на одном элементе.
	 */
	{
		static const char *w = (const char *)1;
		if (w == (const char *)1)
			w = getenv("XSYS4_ANY_TRACE");
		if (w && *w) {
			const char *fname = vm_current_function_name();
			if (strstr(fname, w))
				NOTICE("ANYTRACE n=%d pred=%d -> %d в %s",
				       (self && *self) ? array_numof(*self, 1) : -1,
				       (int)with_pred, (int)r, display_sjis0(fname));
		}
	}
	return r;
}

static bool Array_ix_All(struct page **self, union vm_value *fn)
{
	if (!self || !*self || !fn)
		return true;
	int n = array_numof(*self, 1);
	for (int i = 0; i < n; i++) {
		if (!*self || i >= array_numof(*self, 1))
			break;
		if (!ix_pred(fn, *self, i))
			return false;
	}
	return true;
}

// int Numof/Count(self) — размер; с предикатом — сколько подходит.
static int Array_ix_Count(struct page **self, union vm_value *fn)
{
	if (!self || !*self)
		return 0;
	if (hll_current_nr_args < 2 || !ix_arg_is_func(1))
		return array_numof(*self, 1);
	int n = array_numof(*self, 1), c = 0;
	for (int i = 0; i < n; i++) {
		if (!*self || i >= array_numof(*self, 1))
			break;
		if (ix_pred(fn, *self, i))
			c++;
	}
	return c;
}

// bool EraseAll(self, предикат) / bool Erase(self, предикат) — удалить ВСЕ
// подходящие элементы. Идём с конца, чтобы индексы не съезжали.
static bool Array_ix_EraseAll(struct page **self, union vm_value *fn)
{
	if (!self || !*self || !fn)
		return false;
	bool any = false;
	for (int i = array_numof(*self, 1) - 1; i >= 0; i--) {
		if (!*self || i >= array_numof(*self, 1))
			continue;
		if (!ix_pred(fn, *self, i))
			continue;
		any = ix_erase_at(self, i) || any;
	}
	return any;
}

/*
 * bool Erase(self, предикат) — удалить ПЕРВЫЙ подходящий элемент (в отличие от
 * `EraseAll`, который выносит все). Живой случай: `GameConfig@EraseShortcut` у
 * Dohna снимает ярлык ADV из `m_shortcutButton` лямбдой-сравнением, и без этой
 * перегрузки вызов уходил в `Erase(self, index, length)` — список не менялся,
 * а игра продолжала считать, что ярлыков четыре, и держала остальные пункты
 * System Menu недоступными.
 */
static bool Array_ix_EraseIf(struct page **self, union vm_value *fn)
{
	if (!self || !*self || !fn)
		return false;
	for (int i = 0; i < array_numof(*self, 1); i++) {
		if (!ix_pred(fn, *self, i))
			continue;
		return ix_erase_at(self, i);
	}
	return false;
}

// bool Remain(self, предикат) — оставить только подходящие (инверсия EraseAll).
static bool Array_ix_Remain(struct page **self, union vm_value *fn)
{
	if (!self || !*self || !fn)
		return false;
	bool any = false;
	for (int i = array_numof(*self, 1) - 1; i >= 0; i--) {
		if (!*self || i >= array_numof(*self, 1))
			continue;
		if (ix_pred(fn, *self, i))
			continue;
		any = ix_erase_at(self, i) || any;
	}
	return any;
}

// Min/Max по компаратору less: индекс наименьшего/наибольшего элемента.
static int ix_extreme(struct page **self, union vm_value *fn, bool want_max)
{
	if (!self || !*self)
		return -1;
	int n = array_numof(*self, 1);
	if (n == 0)
		return -1;
	int best = 0;
	for (int i = 1; i < n; i++) {
		bool i_less_best = ix_less(fn, *self, i, best);
		if (want_max ? ix_less(fn, *self, best, i) : i_less_best)
			best = i;
	}
	return best;
}

/*
 * ★`First`/`Last` С ПРЕДИКАТОМ, который НИЧЕГО не нашёл, — почти всегда дефект
 * ДАННЫХ, а не нормальная ветка: сайт получает ссылку из индекса −1 и падает при
 * первом же разыменовании, причём далеко от причины. Живой случай: клик по узлу
 * данжа Dohna — `SceneMap@OnClickNode` берёт ребро как
 * `Array.First(m_map.Edges, obj => obj.NodeFrom == idFrom && obj.NodeTo == nodeId)`
 * и БЕЗ проверки зовёт `MapView@Move(edge)`, а тот сразу `edge.GetRoutePos()` —
 * в логе это выглядело как «`Out of bounds heap index: -1/2` в `MapEdge@GetRoutePos`».
 * Поэтому промах называем СРАЗУ и печатаем стек игры (один раз на прогон, чтобы
 * не залить лог), а с `XSYS4_BSEARCH_FIELDS=<поля>` — ещё и содержимое элементов,
 * по которым шёл поиск (для рёбер это члены 0/1 — `m_nodeFrom`/`m_nodeTo`).
 */
/*
 * ★КЛЮЧИ поиска лежат в ЛОКАЛЬНОЙ странице ВЫЗЫВАЮЩЕЙ функции: предикат-лямбда
 * читает их через `X_GETENV`, а сама HLL-функция кадра не заводит, поэтому
 * `local_page()` здесь — это кадр сайта. Печатаем имя функции и её целочисленные
 * локальные С ИМЕНАМИ из `.ain`: у `SceneMap@OnClickNode` это сразу даёт
 * `nodeId` и `idFrom`, то есть ЧТО искали, а не только среди чего.
 */
static void ix_pred_site(char *buf, size_t sz)
{
	buf[0] = '\0';
	struct page *lp = local_page();
	if (!lp || lp->type != LOCAL_PAGE || lp->index < 0
			|| lp->index >= ain->nr_functions)
		return;
	struct ain_function *f = &ain->functions[lp->index];
	size_t o = snprintf(buf, sz, " сайт %s:", display_sjis0(f->name));
	for (int i = 0; i < lp->nr_vars && i < f->nr_vars && o + 48 < sz; i++) {
		enum ain_data_type t = f->vars[i].type.data;
		if (t != AIN_INT && t != AIN_BOOL)
			continue;
		o += snprintf(buf + o, sz - o, " %s=%d",
			      display_sjis0(f->vars[i].name), lp->values[i].i);
	}
}

/*
 * `XSYS4_FIRST_TRACE=<подстрока имени функции-сайта>` — печатать и УДАЧНЫЕ поиски
 * с предикатом: найденный индекс, сайт с его локальными и содержимое элементов.
 * Промах виден и без этого (см. `ix_pred_miss`), а вот «нашёл, но НЕ ТО» — только
 * так: `MapStructure@FindNodeFromId(3)` отдавал узел с `Id = 2` (первый в списке),
 * из-за чего клик по законному узлу данжа падал (§5ed).
 */
static void ix_pred_hit(const char *what, struct page **self, int idx)
{
	static const char *pat = (const char *)1;
	if (pat == (const char *)1)
		pat = getenv("XSYS4_FIRST_TRACE");
	if (!pat || !*pat)
		return;
	char site[512];
	ix_pred_site(site, sizeof(site));
	if (!strstr(site, pat))
		return;
	char flds[1024];
	ix_elem_fields(self ? *self : NULL, flds, sizeof(flds));
	NOTICE("Array.%s -> индекс %d.%s%s%s", what, idx, site,
	       *flds ? " Элементы:" : "", flds);
}

static void ix_pred_miss(const char *what, struct page **self)
{
	static bool warned = false;
	struct page *p = self ? *self : NULL;
	char flds[1024], site[512];
	ix_elem_fields(p, flds, sizeof(flds));
	ix_pred_site(site, sizeof(site));
	if (!warned) {
		warned = true;
		WARNING("Array.%s(предикат) не нашёл НИ ОДНОГО элемента из %d: сайт "
			"получит ссылку −1 и упадёт при разыменовании.%s%s%s", what,
			p ? array_numof(p, 1) : 0, site,
			*flds ? " Элементы:" : "", flds);
		vm_stack_trace();
	} else {
		// Повторы — только под ручкой и только для интересующего сайта: у игры
		// полно ШТАТНЫХ промахов (`Motion::EasingArgumentAnalyzer` и подобные),
		// и без фильтра они заливают лог тысячами строк, пряча настоящий сбой.
		static const char *pat = (const char *)1;
		if (pat == (const char *)1)
			pat = getenv("XSYS4_FIRST_TRACE");
		if (pat && *pat && strstr(site, pat))
			NOTICE("Array.%s(предикат) промах.%s%s%s", what, site,
			       *flds ? " Элементы:" : "", flds);
	}
}

// int First/Last(self[, предикат]) — индекс; ffi материализует из него ссылку
// на элемент по типу элемента (см. AIN_REF_HLL_PARAM в ffi.c).
static int Array_ix_First(struct page **self, union vm_value *fn)
{
	if (!self || !*self)
		return -1;
	if (hll_current_nr_args >= 2 && ix_arg_is_func(1)) {
		int i = ix_find_pred(self, fn, false);
		if (i < 0)
			ix_pred_miss("First", self);
		else
			ix_pred_hit("First", self, i);
		return i;
	}
	return array_numof(*self, 1) > 0 ? 0 : -1;
}

static int Array_ix_Last(struct page **self, union vm_value *fn)
{
	if (!self || !*self)
		return -1;
	if (hll_current_nr_args >= 2 && ix_arg_is_func(1))
		return ix_find_pred(self, fn, true);
	int n = array_numof(*self, 1);
	return n > 0 ? n - 1 : -1;
}

/*
 * Min/Max БЕЗ компаратора — порядок ПО САМОМУ ЗНАЧЕНИЮ. Форма встречается:
 * `PlayerCollection@GetMaxFeelEventLevel` (@FUNC 27792) собирает `array<int>`
 * уровней через `ArrayExtensions::Select<int, Player&>` и зовёт `Array Max 1`.
 * Пока форма только печатала WARNING и отдавала -1, игра шла по ветке «пусто» и
 * дальше валилась в `PartySkillView@Set` на `X_REF` с индексом кучи -1 —
 * это и роняло экран FEEL-события (иконка справа от Garage).
 *
 * Сравниваем как число: у элемента-скаляра (int/bool/float/long) значение лежит
 * в одном слоте, и естественный порядок для него определён однозначно. Для
 * СТРУКТУР и строк порядка нет — там по-прежнему честный WARNING, а не выдумка:
 * такие сайты в байт-коде обеих игр кладут лямбду.
 *
 * Возвращается ИНДЕКС элемента: `hll_call` материализует из него двухслотовую
 * ссылку по типу возврата (AIN_REF_HLL_PARAM, как у `Array.At`), а -1 игра
 * проверяет сама — `PUSH -1; EQUALE` сразу после вызова.
 */
static bool ix_elem_is_scalar(enum ain_data_type dt)
{
	switch (array_type(dt)) {
	case AIN_INT: case AIN_BOOL: case AIN_FLOAT: case AIN_LONG_INT:
		return true;
	default:
		return false;
	}
}

static bool ix_value_less(struct page *a, int i, int j)
{
	int slots = array_elem_slots(a);
	union vm_value x = a->values[i * slots];
	union vm_value y = a->values[j * slots];
	if (array_type(ix_dtype(a)) == AIN_FLOAT)
		return x.f < y.f;
	return x.i < y.i;
}

static int ix_extreme_by_value(struct page **self, bool want_max, const char *who)
{
	if (!self || !*self)
		return -1;
	struct page *a = *self;
	if (!ix_elem_is_scalar(ix_dtype(a))) {
		WARNING("Array.%s без компаратора: у элемента этого типа порядок не определён", who);
		return -1;
	}
	int n = array_numof(a, 1);
	if (n == 0)
		return -1;
	int best = 0;
	for (int i = 1; i < n; i++) {
		if (want_max ? ix_value_less(a, best, i) : ix_value_less(a, i, best))
			best = i;
	}
	return best;
}

static int Array_ix_Min(struct page **self, union vm_value *fn)
{
	if (hll_current_nr_args < 2 || !ix_arg_is_func(1))
		return ix_extreme_by_value(self, false, "Min");
	return ix_extreme(self, fn, false);
}

static int Array_ix_Max(struct page **self, union vm_value *fn)
{
	if (hll_current_nr_args < 2 || !ix_arg_is_func(1))
		return ix_extreme_by_value(self, true, "Max");
	return ix_extreme(self, fn, true);
}

/*
 * array<T> Where(self, предикат) — НОВЫЙ массив из подходящих элементов.
 * Возврат типа 79 уходит на стек как есть (ffi: default-ветка), поэтому отдаём
 * heap-СЛОТ страницы, а не сам указатель: сырой page* был бы прочитан как
 * индекс слота. Вызывающий владеет слотом и освобождает его своим DELETE.
 */
static int Array_ix_Where(struct page **self, union vm_value *fn)
{
	int slot = heap_alloc_slot(VM_PAGE);
	if (!self || !*self || !fn) {
		heap_set_page(slot, NULL);
		return slot;
	}
	struct page *src = *self;
	enum ain_data_type dt = ix_dtype(src);
	int st = ix_stype(src);
	int eslots = array_elem_slots(src);
	union vm_value dim = { .i = 0 };
	struct page *out = alloc_array(ix_rank(src), &dim, dt, st, false);

	int n = array_numof(src, 1);
	for (int i = 0; i < n; i++) {
		if (!*self || i >= array_numof(*self, 1))
			break;
		src = *self;
		if (!ix_pred(fn, src, i))
			continue;
		// Элемент-объект попадает в новый контейнер по ссылке — он должен
		// получить свой счётчик (как в Array_PushBack).
		if (ix_elem_is_object(dt))
			heap_ref(src->values[i * eslots].i);
		out = array_pushback_n(out, &src->values[i * eslots], eslots, dt, st);
	}
	heap_set_page(slot, out);
	return slot;
}

// Сортировка компаратором `less` (Ixseal). Вставками: устойчиво, не зависит от
// согласованности лямбды (qsort с «плохим» компаратором может выйти за границы),
// а списки здесь — интерфейсные, короткие.
static void ix_swap_elems(struct page *a, int i, int j, int slots)
{
	for (int k = 0; k < slots; k++) {
		union vm_value t = a->values[i * slots + k];
		a->values[i * slots + k] = a->values[j * slots + k];
		a->values[j * slots + k] = t;
	}
}

static void ix_sort_pred(struct page **self, union vm_value *fn)
{
	if (!self || !*self)
		return;
	int slots = array_elem_slots(*self);
	int n = array_numof(*self, 1);
	for (int i = 1; i < n; i++) {
		for (int j = i; j > 0 && ix_less(fn, *self, j, j - 1); j--)
			ix_swap_elems(*self, j, j - 1, slots);
	}
}

// These sort in place. The .ain declares an array/wrap return (for fluent
// chaining), but a raw page pointer must never be pushed as a VM value (it would
// be misread as a heap-slot index). Returning null is safe for the common
// statement-style usage; callers that chain would need the real handle, which
// isn't available here.
static struct page *Array_ix_Sort(struct page **self, union vm_value *fn)
{
	if (!self)
		return NULL;
	// Sort/QuickSort(self, компаратор): лямбда — `less(a, b)` -> bool (взято из
	// её сигнатуры в .ain: два аргумента-элемента, возврат 47).
	bool with_pred = hll_current_nr_args >= 2 && ix_arg_is_func(1);
	if (getenv("XSYS4_SORT_TRACE") && *self) {
		int n = array_numof(*self, 1), slots = array_elem_slots(*self);
		char buf[256] = "";
		for (int i = 0; i < n && i < 12; i++)
			snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " %d",
				 (*self)->values[i * slots].i);
		NOTICE("SORT до: nr_args=%d pred=%d n=%d slots=%d elems:%s",
		       hll_current_nr_args, (int)with_pred, n, slots, buf);
	}
	if (with_pred)
		ix_sort_pred(self, fn);
	else
		array_sort(*self, 0);
	if (getenv("XSYS4_SORT_TRACE") && *self) {
		int n = array_numof(*self, 1), slots = array_elem_slots(*self);
		char buf[256] = "";
		for (int i = 0; i < n && i < 12; i++)
			snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " %d",
				 (*self)->values[i * slots].i);
		NOTICE("SORT после: elems:%s", buf);
	}
	return NULL;
}

/*
 * Сортированные операции: BinarySearch / LowerBound.
 *
 * Перегрузка «с лямбдой» — это НЕ `less(a, b)`, а ТРЁХЗНАЧНЫЙ компаратор от
 * ОДНОГО элемента: ключ лямбда ЗАХВАТЫВАЕТ, среди аргументов его нет. Форма
 * снята с байткода: `IdArray<string, Dungeon>@FindIndex` (@0x9ad76a) строит
 * `int(ref Dungeon obj) => this.CompareFunc(obj, <захваченный id>)`, а сама
 * `CompareFunc` (@0x9ad802) возвращает -1 по `S_LT`, +1 по `S_GT`, иначе 0.
 * Сигнатура лямбды в .ain это подтверждает: nr_args=1, возврат int (10).
 * Знак — «элемент относительно ключа», как у strcmp.
 */
static int ix_cmp3(union vm_value *fn, struct page *a, int index)
{
	int slots = array_elem_slots(a);
	int argc = vm_hll_func_nr_args(fn[1].i);
	if (argc > slots)
		argc = slots;
	return vm_call_hll_func(fn, &a->values[index * slots], argc).i;
}

// Сравнение элемента с ключом-ЗНАЧЕНИЕМ (перегрузки `(self, значение)`).
// INT_MIN = порядок для этого типа элемента не установлен.
static int ix_value_cmp(struct page *a, int index, union vm_value *key)
{
	switch (array_type(a->a_type)) {
	case AIN_INT:
	case AIN_BOOL:
	case AIN_LONG_INT:
		return (a->values[index].i > key->i) - (a->values[index].i < key->i);
	case AIN_FLOAT:
		return (a->values[index].f > key->f) - (a->values[index].f < key->f);
	case AIN_STRING:
		return strcmp(heap_get_string(a->values[index].i)->text,
			      heap_get_string(key->i)->text);
	default: {
		static bool logged = false;
		if (!logged) {
			logged = true;
			WARNING("Array.LowerBound/BinarySearch по ЗНАЧЕНИЮ: порядок для "
				"элемента типа %d не установлен", array_type(a->a_type));
		}
		return INT_MIN;
	}
	}
}

/*
 * `XSYS4_BSEARCH_TRACE` — построчный лог зондов BinarySearch/LowerBound.
 * У элемента-структуры дополнительно печатается член 0: у контейнеров
 * `IdArray<string, T>` это id-строка, и по ней сразу видно, отсортирован ли
 * массив (так нашлась порча порядка от `array_insert_n`).
 */
static const char *ix_elem_id(struct page *a, int index)
{
	if (!a)
		return NULL;
	int pgi = a->values[index * array_elem_slots(a)].i;
	if (pgi <= 0 || (size_t)pgi >= heap_size || heap[pgi].type != VM_PAGE)
		return NULL;
	struct page *e = heap[pgi].page;
	if (!e || e->nr_vars != 1)
		return NULL;
	int s0 = e->values[0].i;
	if (s0 <= 0 || (size_t)s0 >= heap_size || heap[s0].type != VM_STRING)
		return NULL;
	return display_sjis0(heap_get_string(s0)->text);
}

/*
 * `XSYS4_BSEARCH_FIELDS=<n>[,<m>…]` — печатать у элементов массива значения
 * указанных ПОЛЕЙ структуры (номер члена), а не только heap-id объекта. Без
 * этого промах виден лишь как «объекты не те», а вопрос «что в них лежит»
 * остаётся открытым: в §5dy у кадров `FrameInfo` нужны члены 24/25
 * (`StartTime`/`EndTime`) — по ним и видно, стыкуются ли интервалы кадров и
 * доходит ли до этих объектов запись из `FrameInfoCollection@SetFrameEndTime`.
 * Формат: ` <heap-id>:<поле>/<поле>…`, `?` — поля у элемента нет.
 */
static void ix_elem_fields(struct page *a, char *buf, size_t sz)
{
	buf[0] = '\0';
	const char *f = getenv("XSYS4_BSEARCH_FIELDS");
	if (!f || !*f || !a || sz < 32)
		return;
	int fields[8], nf = 0;
	for (const char *p = f; *p && nf < 8; ) {
		fields[nf++] = atoi(p);
		while (*p && *p != ',')
			p++;
		if (*p == ',')
			p++;
	}
	int slots = array_elem_slots(a);
	if (slots < 1)
		return;
	int n = a->nr_vars / slots;
	size_t o = 0;
	// Тип элемента — по номеру структуры первой страницы: без него по одним
	// heap-id не понять, ТОТ ли это контейнер, за которым идёт охота.
	{
		int p0 = n > 0 ? a->values[0].i : -1;
		struct page *e0 = (p0 > 0 && (size_t)p0 < heap_size
				   && heap[p0].type == VM_PAGE) ? heap[p0].page : NULL;
		if (e0 && e0->type == STRUCT_PAGE && e0->index >= 0
		    && e0->index < ain->nr_structures)
			o += snprintf(buf + o, sz - o, " <%s,членов=%d>",
				      ain->structures[e0->index].name, e0->nr_vars);
	}
	for (int i = 0; i < n && o + 32 < sz; i++) {
		int pgi = a->values[i * slots].i;
		struct page *e = (pgi > 0 && (size_t)pgi < heap_size
				  && heap[pgi].type == VM_PAGE) ? heap[pgi].page : NULL;
		o += snprintf(buf + o, sz - o, " %d:", pgi);
		for (int k = 0; k < nf && o + 12 < sz; k++) {
			if (e && fields[k] < e->nr_vars)
				o += snprintf(buf + o, sz - o, "%s%d", k ? "/" : "",
					      e->values[fields[k]].i);
			else
				o += snprintf(buf + o, sz - o, "%s?", k ? "/" : "");
		}
	}
}

static void ix_probe_trace(const char *what, struct page *a, int n, int lo, int hi,
			   int mid, int c)
{
	const char *id = ix_elem_id(a, mid);
	NOTICE("%s n=%d lo=%d hi=%d mid=%d cmp=%d%s%s%s", what, n, lo, hi, mid, c,
		id ? " elem=\"" : "", id ? id : "", id ? "\"" : "");
}

// Индекс совпавшего элемента (компаратор вернул 0), иначе -1.
static int Array_ix_BinarySearch(struct page **self, union vm_value *key)
{
	if (!self || !*self || !key)
		return -1;
	int n = array_numof(*self, 1);
	// Перегрузка «по значению» (fn54) в Dohna не встречается ни разу, поэтому
	// упорядоченность массива для неё не доказана: ищем равенство перебором —
	// на отсортированном массиве ответ тот же, на неотсортированном корректнее.
	if (!ix_arg_is_func(1))
		return array_find(*self, 0, n, *key, 0);
	/*
	 * `XSYS4_BSEARCH_TRACE=miss` — печатать ТОЛЬКО промахи (результат −1), по
	 * одной строке на вызов. Полный лог зондов на живом прогоне Dohna — десятки
	 * мегабайт (поиск идёт каждый кадр по каждому юниту), а вопрос обычно один:
	 * «почему именно этот поиск не нашёл». Именно так разбиралась замершая
	 * покадровая анимация отряда в данже: `FrameInfoCollection@GetIndexFromTime`
	 * зовёт BinarySearch с ТРЁХЗНАЧНЫМ компаратором и получает −1 (§5dy).
	 */
	const char *tr_env = getenv("XSYS4_BSEARCH_TRACE");
	bool miss_only = tr_env && !strcmp(tr_env, "miss");
	bool trace = tr_env && !miss_only;
	int lo = 0, hi = n - 1;
	char probes[128];
	int np = 0;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		// Лямбда может перевыделить страницу — сверяемся каждый шаг.
		if (!*self || mid >= array_numof(*self, 1))
			return -1;
		int c = ix_cmp3(key, *self, mid);
		if (miss_only && np < (int)sizeof(probes) - 12)
			np += snprintf(probes + np, sizeof(probes) - np, " [%d]=%d", mid, c);
		if (trace)
			ix_probe_trace("BSEARCH", *self, n, lo, hi, mid, c);
		if (c == 0)
			return mid;
		if (c < 0)
			lo = mid + 1;   // элемент МЕНЬШЕ ключа → искать правее
		else
			hi = mid - 1;
	}
	if (miss_only) {
		struct page *p = *self;
		int s = array_elem_slots(p);
		char flds[1024];
		ix_elem_fields(p, flds, sizeof(flds));
		NOTICE("BSEARCH промах: page=%p n=%d elem_slots=%d nr_args=%d элементы: %d %d … %d зонды:%s%s%s",
		       (void *)p, n, s, vm_hll_func_nr_args(key[1].i),
		       p->nr_vars > 0 ? p->values[0].i : -1,
		       p->nr_vars > s ? p->values[s].i : -1,
		       p->nr_vars > 0 ? p->values[p->nr_vars - s].i : -1,
		       np ? probes : " нет",
		       *flds ? " поля:" : "", flds);
	}
	return -1;
}

// Первый индекс, где элемент уже НЕ меньше ключа (std::lower_bound); `n`, если
// такого нет. Сайт `resources::detail::ResourceInfoCollection@Add` (@0x156124)
// использует результат прямо как позицию для последующего `Array.Insert`, т.е.
// «не найдено» обязано означать «в конец».
static int Array_ix_LowerBound(struct page **self, union vm_value *key)
{
	if (!self || !*self || !key)
		return 0;
	int n = array_numof(*self, 1);
	bool by_func = ix_arg_is_func(1);
	bool lb_trace = getenv("XSYS4_BSEARCH_TRACE") && by_func;
	int lo = 0, hi = n;
	while (lo < hi) {
		int mid = lo + (hi - lo) / 2;
		if (!*self || mid >= array_numof(*self, 1))
			break;
		int c = by_func ? ix_cmp3(key, *self, mid) : ix_value_cmp(*self, mid, key);
		if (c == INT_MIN)
			return n;
		if (lb_trace)
			ix_probe_trace("LOWERBOUND", *self, n, lo, hi, mid, c);
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lb_trace)
		NOTICE("LOWERBOUND n=%d -> %d", n, lo);
	return lo;
}

/*
 * `ShallowCopy(self) -> array` — НОВЫЙ контейнер с ТЕМИ ЖЕ элементами.
 *
 * Именно поверхностная: объектные элементы не клонируются, у них лишь растёт
 * счётчик ссылок. Это отличает функцию от соседней `Duplicate` (тот же приём,
 * что `Unique` против `UniqueSorted`), и подтверждается сайтом
 * `PlayerCollection@GetAllInstances` @0x5b4cfa — он отдаёт наружу список тех же
 * самых объектов-игроков, по которым игра потом сверяет тождество. Через
 * copy_page/vm_copy тут идти нельзя: для элемента AIN_STRUCT они делают ГЛУБОКУЮ
 * копию страницы, т.е. вернули бы клонов.
 *
 * Возврат типа `array` (ret=79) отдаётся ГОТОВЫМ heap-слотом, а не указателем на
 * страницу: ffi для таких типов кладёт значение на стек как есть, и ту же
 * конвенцию уже использует `String.Split`. Слотом владеет вызывающий.
 */
static int Array_ix_ShallowCopy(struct page **self)
{
	int slot = heap_alloc_slot(VM_PAGE);
	if (!self || !*self) {
		heap_set_page(slot, NULL);
		return slot;
	}
	struct page *src = *self;
	struct page *dst = alloc_page(ARRAY_PAGE, src->a_type, src->nr_vars);
	dst->array = src->array;
	// ★Элементы ОБЩИЕ с источником — это и есть смысл функции. Признак нужен
	// дальше: копия ЭТОЙ страницы (`A_REF` → `copy_page`) обязана ссылаться на
	// те же объекты, иначе запись по элементу копии теряется (§5dy: `EndTime`
	// кадров уходил в клоны, и анимации стояли).
	// Откат для замеров A/B на одном бинаре: `XSYS4_NO_SHALLOW_ELEMS=1` — не
	// помечать (копия страницы снова будет клонировать элементы).
	{
		static const char *off = (const char *)1;
		if (off == (const char *)1)
			off = getenv("XSYS4_NO_SHALLOW_ELEMS");
		dst->elems_shared = !(off && *off);
	}
	int slots = array_elem_slots(src);
	bool obj = ix_elem_is_object(src->a_type);
	for (int i = 0; i < src->nr_vars; i++) {
		dst->values[i] = src->values[i];
		// У 2-слотового элемента (wrap<интерфейс>) владеет только НИЖНИЙ слот —
		// верхний это база интерфейса, обычное число.
		if (obj && i % slots == 0 && src->values[i].i != -1)
			heap_ref(src->values[i].i);
	}
	heap_set_page(slot, dst);
	return slot;
}

static struct page *Array_ix_DescSort(struct page **self)
{
	if (self) {
		array_sort(*self, 0);
		array_reverse(*self);
	}
	return NULL;
}

// Append a default-constructed element and return a reference to it (its heap
// slot for element types that live on the heap, e.g. structs). Used by Ixseal
// constructors as `arr.EmplaceBack()` followed by member initialisation.
static int Array_EmplaceBack(struct page **self)
{
	if (!self || !*self)
		return 0;
	struct page *a = *self;
	enum ain_data_type dt = a->a_type;
	int st = a->array.struct_type;
	int slots = array_elem_slots(a);
	union vm_value v[2];
	if (slots == 2) {
		// Пустая пара wrap<интерфейс>: объекта нет, база интерфейса 0.
		v[0].i = -1;
		v[1].i = 0;
	} else if (array_type(dt) == AIN_STRUCT) {
		create_struct(st, &v[0]);
	} else {
		v[0] = variable_initval(array_type(dt));
	}
	*self = array_pushback_n(a, v, slots, dt, st);
	a = *self;
	// EmplaceBack returns a WRAP (ref) to the newly-added element; hll_call
	// materialises the concrete reference from this element index (a struct
	// element becomes a 1-value page-slot ref, a scalar a 2-value ref).
	int n = array_numof(a, 1);
	return n > 0 ? n - 1 : -1;
}

// Return a reference to element `index` — its value/heap-slot (a struct element
// is a heap page, so its slot is a usable reference).
// Returns a reference to element `index`. The reference is a two-value stack
// entity (array-page slot, element index); the C function returns only the
// element index (or -1 when out of range, which callers compare against), and
// hll_call() supplies the array slot for the AIN_REF_HLL_PARAM return.
static int Array_At(struct page **self, int index)
{
	if (!self || !*self) {
		if (getenv("XSYS4_ARRAY_TRACE"))
			NOTICE("ARRAYTRACE At(index=%d) -> -1 (массива нет)", index);
		return -1;
	}
	struct page *a = *self;
	if (index < 0 || index >= array_numof(a, 1)) {
		if (getenv("XSYS4_ARRAY_TRACE"))
			NOTICE("ARRAYTRACE At(index=%d) -> -1 (размер %d)",
			       index, array_numof(a, 1));
		return -1;
	}
	return index;
}

/*
 * Unique(self) / Unique(self, равенство) — удалить ДУБЛИКАТЫ, сохранив порядок.
 *
 * Что это именно «удалить все дубликаты», а не std::unique (только соседние),
 * следует из того, что в библиотеке ОТДЕЛЬНО объявлена `UniqueSorted` — иначе
 * две функции совпадали бы. Лямбда-перегрузка получает ДВА элемента и отдаёт
 * bool, причём это предикат РАВЕНСТВА, а не «меньше»: тело лямбды на сайте
 * @0x1fb27e — ровно `lhs.IsSame(rhs)` (`elkeditor::detail::CEmitterKey@IsSame`).
 *
 * Идём с конца, чтобы удалялся ПОЗДНИЙ из совпавших (первое вхождение остаётся
 * на месте) и чтобы удаление не сдвигало ещё не просмотренные индексы.
 */
static void ix_unique(struct page **self, union vm_value *fn, bool use_pred)
{
	if (!self || !*self)
		return;
	int slots = array_elem_slots(*self);
	for (int i = array_numof(*self, 1) - 1; i >= 1; i--) {
		bool dup = false;
		if (use_pred) {
			for (int j = 0; j < i && !dup; j++)
				dup = ix_less(fn, *self, j, i);
		} else {
			dup = array_find(*self, 0, i, (*self)->values[i * slots], 0) >= 0;
		}
		if (!dup)
			continue;
		ix_erase_at(self, i);
	}
}

static void Array_ix_Unique(struct page **self, union vm_value *fn)
{
	ix_unique(self, fn, hll_current_nr_args >= 2 && ix_arg_is_func(1));
}

/*
 * `UniqueSorted(self)` / `UniqueSorted(self, равенство)` — убрать дубликаты в
 * УЖЕ УПОРЯДОЧЕННОМ массиве, то есть классический `std::unique`: сравнивать
 * только СОСЕДЕЙ. Отдельной от `Unique` эта функция и объявлена ровно потому,
 * что там сравнение со всеми предыдущими, а здесь — с одним предыдущим; на
 * отсортированных данных результат тот же, но за один проход.
 * Без неё игра падала фаталом `Unimplemented HLL function: Array.UniqueSorted`
 * в битве Dohna (§5ef).
 * Идём с конца: удаление не сдвигает ещё не просмотренные индексы, и из пары
 * совпавших остаётся ПЕРВЫЙ — как у `Unique`.
 */
static void Array_ix_UniqueSorted(struct page **self, union vm_value *fn)
{
	if (!self || !*self)
		return;
	bool use_pred = hll_current_nr_args >= 2 && ix_arg_is_func(1);
	int slots = array_elem_slots(*self);
	for (int i = array_numof(*self, 1) - 1; i >= 1; i--) {
		bool dup;
		if (use_pred) {
			dup = ix_less(fn, *self, i - 1, i);
		} else {
			// Без предиката равенство — по ЗНАЧЕНИЮ элемента: ищем предыдущий
			// в диапазоне [i-1, i), то есть сравниваем ровно с соседом.
			dup = array_find(*self, i - 1, i, (*self)->values[i * slots], 0) >= 0;
		}
		if (dup)
			ix_erase_at(self, i);
	}
}

/*
 * array SYSTEMONLY_GetStructPageList(self) — «сырые» страницы объектов из
 * контейнера интерфейсных ссылок. Служебная функция сериализации: единственный
 * сайт — `AFL_GameSave_StructLoad` (@0x227e56), который собирает
 * `array<wrap<интерфейс>>` из одного элемента (`dest`) и передаёт результат в
 * `system.DeserializeStruct(fileName, <этот список>, 1)`; та объявлена как
 * `(string, array, bool)`, т.е. ждёт именно список СТРАНИЦ, а не пар.
 *
 * Поэтому берём нижний слот каждого элемента (heap-слот объекта) и отбрасываем
 * верхний (базу интерфейса) — ровно обратное тому, что делает X_ICAST. Новый
 * контейнер владеет своей ссылкой на объект (как Array_PushBack/Where).
 *
 * Возврат — heap-СЛОТ страницы (тип 79 уходит на стек как есть, см. Where).
 * Сайт его не освобождает, так что слот на вызов утекает; DeserializeStruct
 * пока заглушка, так что цена нулевая, но это стоит помнить.
 */
static int Array_SYSTEMONLY_GetStructPageList(struct page **self)
{
	int slot = heap_alloc_slot(VM_PAGE);
	if (!self || !*self) {
		heap_set_page(slot, NULL);
		return slot;
	}
	struct page *src = *self;
	int eslots = array_elem_slots(src);
	int n = array_numof(src, 1);
	union vm_value dim = { .i = 0 };
	struct page *out = alloc_array(1, &dim, AIN_ARRAY_STRUCT, ix_stype(src), false);
	for (int i = 0; i < n; i++) {
		union vm_value obj = src->values[i * eslots];
		heap_ref(obj.i);
		out = array_pushback_n(out, &obj, 1, AIN_ARRAY_STRUCT, ix_stype(src));
	}
	heap_set_page(slot, out);
	return slot;
}

HLL_LIBRARY(Array,
	    HLL_EXPORT(Alloc, Array_Alloc),
	    HLL_EXPORT(SYSTEMONLY_GetStructPageList, Array_SYSTEMONLY_GetStructPageList),
	    HLL_EXPORT(Unique, Array_ix_Unique),
	    HLL_EXPORT(UniqueSorted, Array_ix_UniqueSorted),
	    HLL_EXPORT(EmplaceBack, Array_EmplaceBack),
	    HLL_EXPORT(At, Array_At),
	    HLL_EXPORT(First, Array_ix_First),
	    HLL_EXPORT(Last, Array_ix_Last),
	    HLL_EXPORT(Min, Array_ix_Min),
	    HLL_EXPORT(Max, Array_ix_Max),
	    HLL_EXPORT(Realloc, Array_Realloc),
	    HLL_EXPORT(Free, Array_Free),
	    HLL_EXPORT(Clear, Array_ix_Clear),
	    HLL_EXPORT(Numof, Array_ix_Count),
	    HLL_EXPORT(Count, Array_ix_Count),
	    HLL_EXPORT(Any, Array_ix_Any),
	    HLL_EXPORT(All, Array_ix_All),
	    HLL_EXPORT(Where, Array_ix_Where),
	    HLL_EXPORT(EraseAll, Array_ix_EraseAll),
	    HLL_EXPORT(Remain, Array_ix_Remain),
	    HLL_EXPORT(Empty, Array_Empty),
	    HLL_EXPORT(PushBack, Array_PushBack),
	    HLL_EXPORT(Add, Array_PushBack),
	    HLL_EXPORT(PopBack, Array_PopBack),
	    HLL_EXPORT(Insert, Array_ix_Insert),
	    HLL_EXPORT_NF(Erase, 2, Array_ix_EraseIf),
	    HLL_EXPORT(Erase, Array_ix_Erase),
	    HLL_EXPORT_N(Copy, 5, Array_ix_Copy5),
	    HLL_EXPORT(Copy, Array_ix_Copy),
	    HLL_EXPORT(Duplicate, Array_ix_Duplicate),
	    HLL_EXPORT(Concat, Array_ix_AddRange),
	    HLL_EXPORT(AddRange, Array_ix_AddRange),
	    HLL_EXPORT(Reverse, Array_Reverse),
	    HLL_EXPORT(Shuffle, Array_Shuffle),
	    HLL_EXPORT(Fill, Array_ix_Fill),
	    HLL_EXPORT_N(Find, 4, Array_ix_Find4),
	    HLL_EXPORT(Find, Array_ix_Find),
	    HLL_EXPORT_N(FindLast, 4, Array_ix_FindLast4),
	    HLL_EXPORT(FindLast, Array_ix_FindLast),
	    HLL_EXPORT(IsExist, Array_ix_IsExist),
	    HLL_EXPORT(ShallowCopy, Array_ix_ShallowCopy),
	    HLL_EXPORT(BinarySearch, Array_ix_BinarySearch),
	    HLL_EXPORT(LowerBound, Array_ix_LowerBound),
	    HLL_EXPORT(Sort, Array_ix_Sort),
	    HLL_EXPORT(AscSort, Array_ix_Sort),
	    HLL_EXPORT(QuickSort, Array_ix_Sort),
	    HLL_EXPORT(DescSort, Array_ix_DescSort),
	    HLL_TODO_EXPORT(NV_copy, Array_NV_copy),
	    HLL_TODO_EXPORT(NV_add, Array_NV_add),
	    HLL_TODO_EXPORT(NV_sub, Array_NV_sub),
	    HLL_TODO_EXPORT(NV_mul, Array_NV_mul),
	    HLL_TODO_EXPORT(NV_div, Array_NV_div),
	    HLL_TODO_EXPORT(NV_and, Array_NV_and),
	    HLL_TODO_EXPORT(NV_or, Array_NV_or),
	    HLL_TODO_EXPORT(NV_xor, Array_NV_xor),
	    HLL_TODO_EXPORT(NV_min, Array_NV_min),
	    HLL_TODO_EXPORT(NV_max, Array_NV_max),
	    HLL_TODO_EXPORT(NN_copy, Array_NN_copy),
	    HLL_TODO_EXPORT(NN_add, Array_NN_add),
	    HLL_TODO_EXPORT(NN_sub, Array_NN_sub),
	    HLL_TODO_EXPORT(NN_mul, Array_NN_mul),
	    HLL_TODO_EXPORT(NN_div, Array_NN_div),
	    HLL_TODO_EXPORT(NN_and, Array_NN_and),
	    HLL_TODO_EXPORT(NN_or, Array_NN_or),
	    HLL_TODO_EXPORT(NN_xor, Array_NN_xor),
	    HLL_TODO_EXPORT(NN_min, Array_NN_min),
	    HLL_TODO_EXPORT(NN_max, Array_NN_max),
	    HLL_TODO_EXPORT(NS_copy, Array_NS_copy),
	    HLL_TODO_EXPORT(NS_add, Array_NS_add),
	    HLL_TODO_EXPORT(NS_sub, Array_NS_sub),
	    HLL_TODO_EXPORT(NS_mul, Array_NS_mul),
	    HLL_TODO_EXPORT(NS_div, Array_NS_div),
	    HLL_TODO_EXPORT(NS_and, Array_NS_and),
	    HLL_TODO_EXPORT(NS_or, Array_NS_or),
	    HLL_TODO_EXPORT(NS_xor, Array_NS_xor),
	    HLL_TODO_EXPORT(NS_min, Array_NS_min),
	    HLL_TODO_EXPORT(NS_max, Array_NS_max),
	    HLL_TODO_EXPORT(SV_copy, Array_SV_copy),
	    HLL_TODO_EXPORT(SV_add, Array_SV_add),
	    HLL_TODO_EXPORT(SV_sub, Array_SV_sub),
	    HLL_TODO_EXPORT(SV_mul, Array_SV_mul),
	    HLL_TODO_EXPORT(SV_div, Array_SV_div),
	    HLL_TODO_EXPORT(SV_and, Array_SV_and),
	    HLL_TODO_EXPORT(SV_or, Array_SV_or),
	    HLL_TODO_EXPORT(SV_xor, Array_SV_xor),
	    HLL_TODO_EXPORT(SV_min, Array_SV_min),
	    HLL_TODO_EXPORT(SV_max, Array_SV_max),
	    HLL_TODO_EXPORT(SN_copy, Array_SN_copy),
	    HLL_TODO_EXPORT(SN_add, Array_SN_add),
	    HLL_TODO_EXPORT(SN_sub, Array_SN_sub),
	    HLL_TODO_EXPORT(SN_mul, Array_SN_mul),
	    HLL_TODO_EXPORT(SN_div, Array_SN_div),
	    HLL_TODO_EXPORT(SN_and, Array_SN_and),
	    HLL_TODO_EXPORT(SN_or, Array_SN_or),
	    HLL_TODO_EXPORT(SN_xor, Array_SN_xor),
	    HLL_TODO_EXPORT(SN_min, Array_SN_min),
	    HLL_TODO_EXPORT(SN_max, Array_SN_max),
	    HLL_TODO_EXPORT(SS_copy, Array_SS_copy),
	    HLL_TODO_EXPORT(SS_add, Array_SS_add),
	    HLL_TODO_EXPORT(SS_sub, Array_SS_sub),
	    HLL_TODO_EXPORT(SS_mul, Array_SS_mul),
	    HLL_TODO_EXPORT(SS_div, Array_SS_div),
	    HLL_TODO_EXPORT(SS_and, Array_SS_and),
	    HLL_TODO_EXPORT(SS_or, Array_SS_or),
	    HLL_TODO_EXPORT(SS_xor, Array_SS_xor),
	    HLL_TODO_EXPORT(SS_min, Array_SS_min),
	    HLL_TODO_EXPORT(SS_max, Array_SS_max),
	    HLL_EXPORT(NV_eneq, Array_NV_eneq),
	    HLL_TODO_EXPORT(NV_enne, Array_NV_enne),
	    HLL_TODO_EXPORT(NV_enlo, Array_NV_enlo),
	    HLL_TODO_EXPORT(NV_enhi, Array_NV_enhi),
	    HLL_TODO_EXPORT(NV_enra, Array_NV_enra),
	    HLL_TODO_EXPORT(NN_eneq, Array_NN_eneq),
	    HLL_TODO_EXPORT(NN_enne, Array_NN_enne),
	    HLL_TODO_EXPORT(NN_enlo, Array_NN_enlo),
	    HLL_TODO_EXPORT(NN_enhi, Array_NN_enhi),
	    HLL_TODO_EXPORT(NS_eneq, Array_NS_eneq),
	    HLL_TODO_EXPORT(NS_enne, Array_NS_enne),
	    HLL_TODO_EXPORT(NS_enlo, Array_NS_enlo),
	    HLL_TODO_EXPORT(NS_enhi, Array_NS_enhi),
	    HLL_TODO_EXPORT(SV_eneq, Array_SV_eneq),
	    HLL_TODO_EXPORT(SV_enne, Array_SV_enne),
	    HLL_TODO_EXPORT(SV_enlo, Array_SV_enlo),
	    HLL_TODO_EXPORT(SV_enhi, Array_SV_enhi),
	    HLL_TODO_EXPORT(SV_enra, Array_SV_enra),
	    HLL_TODO_EXPORT(SN_eneq, Array_SN_eneq),
	    HLL_TODO_EXPORT(SN_enne, Array_SN_enne),
	    HLL_TODO_EXPORT(SN_enlo, Array_SN_enlo),
	    HLL_TODO_EXPORT(SN_enhi, Array_SN_enhi),
	    HLL_TODO_EXPORT(SS_eneq, Array_SS_eneq),
	    HLL_TODO_EXPORT(SS_enne, Array_SS_enne),
	    HLL_TODO_EXPORT(SS_enlo, Array_SS_enlo),
	    HLL_TODO_EXPORT(SS_enhi, Array_SS_enhi),
	    HLL_TODO_EXPORT(NV_cheq, Array_NV_cheq),
	    HLL_TODO_EXPORT(NV_chne, Array_NV_chne),
	    HLL_TODO_EXPORT(NV_chlo, Array_NV_chlo),
	    HLL_TODO_EXPORT(NV_chhi, Array_NV_chhi),
	    HLL_TODO_EXPORT(NV_chra, Array_NV_chra),
	    HLL_TODO_EXPORT(NN_cheq, Array_NN_cheq),
	    HLL_TODO_EXPORT(NN_chne, Array_NN_chne),
	    HLL_TODO_EXPORT(NN_chlo, Array_NN_chlo),
	    HLL_TODO_EXPORT(NN_chhi, Array_NN_chhi),
	    HLL_TODO_EXPORT(NS_cheq, Array_NS_cheq),
	    HLL_TODO_EXPORT(NS_chne, Array_NS_chne),
	    HLL_TODO_EXPORT(NS_chlo, Array_NS_chlo),
	    HLL_TODO_EXPORT(NS_chhi, Array_NS_chhi),
	    HLL_TODO_EXPORT(SV_cheq, Array_SV_cheq),
	    HLL_TODO_EXPORT(SV_chne, Array_SV_chne),
	    HLL_TODO_EXPORT(SV_chlo, Array_SV_chlo),
	    HLL_TODO_EXPORT(SV_chhi, Array_SV_chhi),
	    HLL_TODO_EXPORT(SV_chra, Array_SV_chra),
	    HLL_TODO_EXPORT(SN_cheq, Array_SN_cheq),
	    HLL_TODO_EXPORT(SN_chne, Array_SN_chne),
	    HLL_TODO_EXPORT(SN_chlo, Array_SN_chlo),
	    HLL_TODO_EXPORT(SN_chhi, Array_SN_chhi),
	    HLL_TODO_EXPORT(SS_cheq, Array_SS_cheq),
	    HLL_TODO_EXPORT(SS_chne, Array_SS_chne),
	    HLL_TODO_EXPORT(SS_chlo, Array_SS_chlo),
	    HLL_TODO_EXPORT(SS_chhi, Array_SS_chhi),
	    HLL_TODO_EXPORT(NV_fweq, Array_NV_fweq),
	    HLL_TODO_EXPORT(NV_fwne, Array_NV_fwne),
	    HLL_TODO_EXPORT(NV_fwlo, Array_NV_fwlo),
	    HLL_TODO_EXPORT(NV_fwhi, Array_NV_fwhi),
	    HLL_TODO_EXPORT(NV_fwra, Array_NV_fwra),
	    HLL_TODO_EXPORT(NV_faeq, Array_NV_faeq),
	    HLL_TODO_EXPORT(NV_fane, Array_NV_fane),
	    HLL_TODO_EXPORT(NV_falo, Array_NV_falo),
	    HLL_TODO_EXPORT(NV_fahi, Array_NV_fahi),
	    HLL_TODO_EXPORT(NV_fara, Array_NV_fara),
	    HLL_TODO_EXPORT(NV_foeq, Array_NV_foeq),
	    HLL_TODO_EXPORT(NV_fone, Array_NV_fone),
	    HLL_TODO_EXPORT(NV_folo, Array_NV_folo),
	    HLL_TODO_EXPORT(NV_fohi, Array_NV_fohi),
	    HLL_TODO_EXPORT(NV_fora, Array_NV_fora),
	    HLL_TODO_EXPORT(NN_fweq, Array_NN_fweq),
	    HLL_TODO_EXPORT(NN_fwne, Array_NN_fwne),
	    HLL_TODO_EXPORT(NN_fwlo, Array_NN_fwlo),
	    HLL_TODO_EXPORT(NN_fwhi, Array_NN_fwhi),
	    HLL_TODO_EXPORT(NN_faeq, Array_NN_faeq),
	    HLL_TODO_EXPORT(NN_fane, Array_NN_fane),
	    HLL_TODO_EXPORT(NN_falo, Array_NN_falo),
	    HLL_TODO_EXPORT(NN_fahi, Array_NN_fahi),
	    HLL_TODO_EXPORT(NN_foeq, Array_NN_foeq),
	    HLL_TODO_EXPORT(NN_fone, Array_NN_fone),
	    HLL_TODO_EXPORT(NN_folo, Array_NN_folo),
	    HLL_TODO_EXPORT(NN_fohi, Array_NN_fohi),
	    HLL_TODO_EXPORT(NS_fweq, Array_NS_fweq),
	    HLL_TODO_EXPORT(NS_fwne, Array_NS_fwne),
	    HLL_TODO_EXPORT(NS_fwlo, Array_NS_fwlo),
	    HLL_TODO_EXPORT(NS_fwhi, Array_NS_fwhi),
	    HLL_TODO_EXPORT(NS_faeq, Array_NS_faeq),
	    HLL_TODO_EXPORT(NS_fane, Array_NS_fane),
	    HLL_TODO_EXPORT(NS_falo, Array_NS_falo),
	    HLL_TODO_EXPORT(NS_fahi, Array_NS_fahi),
	    HLL_TODO_EXPORT(NS_foeq, Array_NS_foeq),
	    HLL_TODO_EXPORT(NS_fone, Array_NS_fone),
	    HLL_TODO_EXPORT(NS_folo, Array_NS_folo),
	    HLL_TODO_EXPORT(NS_fohi, Array_NS_fohi),
	    HLL_TODO_EXPORT(SV_fweq, Array_SV_fweq),
	    HLL_TODO_EXPORT(SV_fwne, Array_SV_fwne),
	    HLL_TODO_EXPORT(SV_fwlo, Array_SV_fwlo),
	    HLL_TODO_EXPORT(SV_fwhi, Array_SV_fwhi),
	    HLL_TODO_EXPORT(SV_fwra, Array_SV_fwra),
	    HLL_TODO_EXPORT(SV_faeq, Array_SV_faeq),
	    HLL_TODO_EXPORT(SV_fane, Array_SV_fane),
	    HLL_TODO_EXPORT(SV_falo, Array_SV_falo),
	    HLL_TODO_EXPORT(SV_fahi, Array_SV_fahi),
	    HLL_TODO_EXPORT(SV_fara, Array_SV_fara),
	    HLL_TODO_EXPORT(SV_foeq, Array_SV_foeq),
	    HLL_TODO_EXPORT(SV_fone, Array_SV_fone),
	    HLL_TODO_EXPORT(SV_folo, Array_SV_folo),
	    HLL_TODO_EXPORT(SV_fohi, Array_SV_fohi),
	    HLL_TODO_EXPORT(SV_fora, Array_SV_fora),
	    HLL_TODO_EXPORT(SN_fweq, Array_SN_fweq),
	    HLL_TODO_EXPORT(SN_fwne, Array_SN_fwne),
	    HLL_TODO_EXPORT(SN_fwlo, Array_SN_fwlo),
	    HLL_TODO_EXPORT(SN_fwhi, Array_SN_fwhi),
	    HLL_TODO_EXPORT(SN_faeq, Array_SN_faeq),
	    HLL_TODO_EXPORT(SN_fane, Array_SN_fane),
	    HLL_TODO_EXPORT(SN_falo, Array_SN_falo),
	    HLL_TODO_EXPORT(SN_fahi, Array_SN_fahi),
	    HLL_TODO_EXPORT(SN_foeq, Array_SN_foeq),
	    HLL_TODO_EXPORT(SN_fone, Array_SN_fone),
	    HLL_TODO_EXPORT(SN_folo, Array_SN_folo),
	    HLL_TODO_EXPORT(SN_fohi, Array_SN_fohi),
	    HLL_TODO_EXPORT(SS_fweq, Array_SS_fweq),
	    HLL_TODO_EXPORT(SS_fwne, Array_SS_fwne),
	    HLL_TODO_EXPORT(SS_fwlo, Array_SS_fwlo),
	    HLL_TODO_EXPORT(SS_fwhi, Array_SS_fwhi),
	    HLL_TODO_EXPORT(SS_faeq, Array_SS_faeq),
	    HLL_TODO_EXPORT(SS_fane, Array_SS_fane),
	    HLL_TODO_EXPORT(SS_falo, Array_SS_falo),
	    HLL_TODO_EXPORT(SS_fahi, Array_SS_fahi),
	    HLL_TODO_EXPORT(SS_foeq, Array_SS_foeq),
	    HLL_TODO_EXPORT(SS_fone, Array_SS_fone),
	    HLL_TODO_EXPORT(SS_folo, Array_SS_folo),
	    HLL_TODO_EXPORT(SS_fohi, Array_SS_fohi),
	    HLL_EXPORT(NV_sceq, Array_NV_sceq),
	    HLL_TODO_EXPORT(NV_scne, Array_NV_scne),
	    HLL_TODO_EXPORT(NV_sclo, Array_NV_sclo),
	    HLL_TODO_EXPORT(NV_schi, Array_NV_schi),
	    HLL_TODO_EXPORT(NV_scra, Array_NV_scra),
	    HLL_TODO_EXPORT(SV_sceq, Array_SV_sceq),
	    HLL_TODO_EXPORT(SV_scne, Array_SV_scne),
	    HLL_TODO_EXPORT(SV_sclo, Array_SV_sclo),
	    HLL_TODO_EXPORT(SV_schi, Array_SV_schi),
	    HLL_TODO_EXPORT(SV_scra, Array_SV_scra),
	    HLL_TODO_EXPORT(NN_sclowest, Array_NN_sclowest),
	    HLL_TODO_EXPORT(NN_schighest, Array_NN_schighest),
	    HLL_TODO_EXPORT(NS_sclowest, Array_NS_sclowest),
	    HLL_TODO_EXPORT(NS_schighest, Array_NS_schighest),
	    HLL_TODO_EXPORT(SN_sclowest, Array_SN_sclowest),
	    HLL_TODO_EXPORT(SN_schighest, Array_SN_schighest),
	    HLL_TODO_EXPORT(SS_sclowest, Array_SS_sclowest),
	    HLL_TODO_EXPORT(SS_schighest, Array_SS_schighest),
	    HLL_TODO_EXPORT(VN_add, Array_VN_add),
	    HLL_TODO_EXPORT(VN_and, Array_VN_and),
	    HLL_TODO_EXPORT(VN_or, Array_VN_or),
	    HLL_TODO_EXPORT(VS_add, Array_VS_add),
	    HLL_TODO_EXPORT(VS_and, Array_VS_and),
	    HLL_TODO_EXPORT(VS_or, Array_VS_or)
	    );
