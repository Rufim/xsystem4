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

#include <string.h>
#include "system4.h"
#include "system4/ain.h"
#include "system4/string.h"
#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"

#define NR_CACHES 8
#define CACHE_SIZE 64

static const char *pagetype_strtab[] = {
	[GLOBAL_PAGE] = "GLOBAL_PAGE",
	[LOCAL_PAGE] = "LOCAL_PAGE",
	[STRUCT_PAGE] = "STRUCT_PAGE",
	[ARRAY_PAGE] = "ARRAY_PAGE",
	[DELEGATE_PAGE] = "DELEGATE_PAGE",
};

const char *pagetype_string(enum page_type type)
{
	if (type < NR_PAGE_TYPES)
		return pagetype_strtab[type];
	return "INVALID PAGE TYPE";
}

struct page_cache {
	unsigned int cached;
	struct page *pages[CACHE_SIZE];
};

struct page_cache page_cache[NR_CACHES];

struct page *_alloc_page(int nr_vars)
{
	int cache_nr = nr_vars - 1;
	if (cache_nr >= 0 && cache_nr < NR_CACHES && page_cache[cache_nr].cached) {
		struct page *page = page_cache[cache_nr].pages[--page_cache[cache_nr].cached];
		memset(page->values, 0, sizeof(union vm_value) * nr_vars);
		return page;
	}
	return xcalloc(1, sizeof(struct page) + sizeof(union vm_value) * nr_vars);
}

void free_page(struct page *page)
{
	int cache_no = page->nr_vars - 1;
	if (cache_no < 0 || cache_no >= NR_CACHES || page_cache[cache_no].cached >= CACHE_SIZE) {
		free(page);
		return;
	}
	page_cache[cache_no].pages[page_cache[cache_no].cached++] = page;
}

struct page *alloc_page(enum page_type type, int type_index, int nr_vars)
{
	struct page *page = _alloc_page(nr_vars);
	page->type = type;
	page->index = type_index;
	page->nr_vars = nr_vars;
	return page;
}

union vm_value variable_initval(enum ain_data_type type)
{
	int slot;
	switch (type) {
	case AIN_STRING:
		slot = heap_alloc_slot(VM_STRING);
		heap[slot].s = string_ref(&EMPTY_STRING);
		return (union vm_value) { .i = slot };
	case AIN_STRUCT:
	case AIN_REF_TYPE:
	// Ixseal generic ref-к-элементу-массива (WRAP, тип 82): это ССЫЛКА
	// (для скаляра — 2 слота [array_slot, idx], для объекта — 1 слот),
	// null-значение = -1. Игра перед присвоением освобождает старое
	// содержимое идиомой «X_REF 1; DELETE»; при инициализации нулём
	// (default-ветка) этот DELETE уносил heap-слот 0 (глобальную
	// страницу) → double free в CASTimerManager@CreateHandle.
	// Ixseal `option<T>` (тип 86): значение занимает slots(T)+1 слотов, из них
	// первый — сам T, а последний — ТЕГ (0 = значение есть, 1 = пусто). Пустое
	// значение игра пишет как -1 (см. init_option_vars ниже).
	case AIN_OPTION:
	case AIN_WRAP:
		return (union vm_value) { .i = -1 };
	case AIN_ARRAY_TYPE:
	case AIN_DELEGATE:
	// новый generic-массив System 4 (тип 79): тоже page-слот пустого массива,
	// иначе член-массив == 0 и паттерн «DELETE old; X_A_INIT» портит чужие слоты
	case AIN_ARRAY:
		slot = heap_alloc_slot(VM_PAGE);
		heap_set_page(slot, NULL);
		return (union vm_value) { .i = slot };
	default:
		return (union vm_value) { .i = 0 };
	}
}

/*
 * Сколько слотов страницы занимает объявленная переменная (Ixseal).
 *
 * Компилятор System 4 v14 показывает это ЯВНО: после многослотового значения он
 * кладёт ровно `slots-1` филлеров `<void>` (AIN_VOID) — и в списке членов
 * структуры, и в локалах/глобалах функции (видно тулом `aintype`). Так
 * `wrap<структура>` = 1 слот, `wrap<интерфейс>` = 2 (объект, база интерфейса),
 * `option<wrap<интерфейс>>` = 3 (объект, база, тег). Гадать по типу не нужно.
 */
int decl_slots(struct ain_variable *vars, int nr_vars, int i)
{
	int n = 1;
	while (i + n < nr_vars && vars[i + n].type.data == AIN_VOID)
		n++;
	return n;
}

/*
 * Инициализировать все `option`-переменные страницы ПУСТЫМИ (Ixseal).
 *
 * Раскладка `option<T>` = [слоты T..., тег], тег 0 = значение есть, 1 = пусто.
 * Полярность снята с байткода игры: `OptionalExtensions::HasValue<T&>` читает
 * слот `+slots(T)` и при `тег >= 1` подменяет значение на -1 (и возвращает
 * false), а сайты `X_OP_SET` пишут либо `<значение>, 0`, либо `-1[, -1], 1`.
 *
 * Без этого все слоты option'а оставались нулями, т.е. «тег 0» = «значение
 * есть», и HasValue врал: `CModeMark@Create` считал спрайт уже созданным,
 * пропускал ветку создания и диспатчил по мусорной паре (объект, база) →
 * `Out of bounds page index: 1/33`.
 *
 * Вызывается ВТОРЫМ проходом: филлеры <void> идут ПОСЛЕ самой переменной, и
 * основной цикл инициализации затёр бы записанный тег нулём.
 */
void init_option_vars(struct page *page, struct ain_variable *vars, int nr_vars, int from)
{
	for (int i = from; i < nr_vars; i++) {
		if (vars[i].type.data != AIN_OPTION)
			continue;
		int slots = decl_slots(vars, nr_vars, i);
		for (int k = 0; k < slots - 1; k++)
			page->values[i + k].i = -1;
		page->values[i + slots - 1].i = slots > 1 ? 1 : -1;
		if (getenv("XSYS4_OPT_TRACE"))
			WARNING("OPT init page-type=%d var=%d slots=%d", page->type, i, slots);
	}
}

void variable_fini(union vm_value v, enum ain_data_type type, bool call_dtor)
{
	switch (type) {
	case AIN_STRING:
	case AIN_STRUCT:
	case AIN_DELEGATE:
	case AIN_ARRAY_TYPE:
	// Ixseal generic array (type 79): release the backing page slot on destruction,
	// mirroring variable_initval/vm_copy which also special-case AIN_ARRAY.
	case AIN_ARRAY:
	case AIN_REF_TYPE:
		if (v.i == -1)
			break;
		if (call_dtor)
			heap_unref(v.i);
		else
			exit_unref(v.i);
		break;
	default:
		break;
	}
}

enum ain_data_type array_type(enum ain_data_type type)
{
	switch (type) {
	case AIN_ARRAY_INT:
	case AIN_REF_ARRAY_INT:
		return AIN_INT;
	case AIN_ARRAY_FLOAT:
	case AIN_REF_ARRAY_FLOAT:
		return AIN_FLOAT;
	case AIN_ARRAY_STRING:
	case AIN_REF_ARRAY_STRING:
		return AIN_STRING;
	case AIN_ARRAY_STRUCT:
	case AIN_REF_ARRAY_STRUCT:
		return AIN_STRUCT;
	case AIN_ARRAY_FUNC_TYPE:
	case AIN_REF_ARRAY_FUNC_TYPE:
		return AIN_FUNC_TYPE;
	case AIN_ARRAY_BOOL:
	case AIN_REF_ARRAY_BOOL:
		return AIN_BOOL;
	case AIN_ARRAY_LONG_INT:
	case AIN_REF_ARRAY_LONG_INT:
		return AIN_LONG_INT;
	case AIN_ARRAY_DELEGATE:
	case AIN_REF_ARRAY_DELEGATE:
		return AIN_DELEGATE;
	// Ixseal: массив, элемент которого — wrap<интерфейс>. Элемент занимает ДВА
	// слота страницы, поэтому единого «типа элемента» у него нет: тип каждого
	// слота даёт variable_type() по чётности. Маркер возвращаем как есть.
	case AIN_IFACE_WRAP:
		return AIN_IFACE_WRAP;
	default:
		WARNING("Unknown/invalid array type: %d", type);
		return type;
	}
}

/*
 * Ixseal (System 4 v14): значение типа `wrap<интерфейс>` — «толстый указатель»
 * из ДВУХ слотов: (heap-слот объекта, база интерфейса в таблице методов этого
 * объекта). Поэтому в generic-массиве такого типа на ОДИН элемент приходится
 * два слота страницы; сама страница помечена `a_type == AIN_IFACE_WRAP`.
 * Байт-код так его и читает: `index*2`, затем ссылка (страница, 2k) —
 * см. foreach в debug::detail::CDebugFPSGraph@SetFont.
 *
 * У всех остальных массивов — в том числе wrap<структура>, где ссылка это
 * обычный heap-слот, — шаг равен одному слоту, так что для старых игр
 * (у которых типа 100 в массивах нет вовсе) поведение не меняется.
 */
int array_elem_slots(struct page *page)
{
	if (!page || page->type != ARRAY_PAGE)
		return 1;
	return (page->array.rank == 1 && page->a_type == AIN_IFACE_WRAP) ? 2 : 1;
}

static int elem_slots_for_type(enum ain_data_type data_type, int rank)
{
	return (rank == 1 && data_type == AIN_IFACE_WRAP) ? 2 : 1;
}

enum ain_data_type variable_type(struct page *page, int varno, int *struct_type, int *array_rank)
{
	switch (page->type) {
	case GLOBAL_PAGE:
		if (struct_type)
			*struct_type = ain->globals[varno].type.struc;
		if (array_rank)
			*array_rank = ain->globals[varno].type.rank;
		return ain->globals[varno].type.data;
	case LOCAL_PAGE:
		if (struct_type)
			*struct_type = ain->functions[page->index].vars[varno].type.struc;
		if (array_rank)
			*array_rank = ain->functions[page->index].vars[varno].type.rank;
		return ain->functions[page->index].vars[varno].type.data;
	case STRUCT_PAGE:
		if (struct_type)
			*struct_type = ain->structures[page->index].members[varno].type.struc;
		if (array_rank)
			*array_rank = ain->structures[page->index].members[varno].type.rank;
		return ain->structures[page->index].members[varno].type.data;
	case ARRAY_PAGE:
		if (struct_type)
			*struct_type = page->array.struct_type;
		if (array_rank)
			*array_rank = page->array.rank - 1;
		// Двухслотовый элемент wrap<интерфейс> (Ixseal): нижний слот — heap-слот
		// объекта (владение и копирование как у struct-элемента), верхний —
		// целочисленная база интерфейса. Благодаря этому delete_page_vars /
		// copy_page / variable_set работают с такой страницей без изменений.
		if (array_elem_slots(page) == 2)
			return (varno & 1) ? AIN_INT : AIN_STRUCT;
		return page->array.rank > 1 ? page->a_type : array_type(page->a_type);
	case DELEGATE_PAGE:
		// XXX: we return void here because objects in a delegate page aren't
		//      reference counted
		return AIN_VOID;
	}
	return AIN_VOID;
}

void variable_set(struct page *page, int varno, enum ain_data_type type, union vm_value val)
{
	variable_fini(page->values[varno], type, true);
	page->values[varno] = val;
}

void delete_page_vars(struct page *page)
{
	for (int i = page->nr_vars - 1; i >= 0; i--) {
		variable_fini(page->values[i], variable_type(page, i, NULL, NULL), true);
	}
}

void delete_page(int slot)
{
	struct page *page = heap_get_page(slot);
	if (!page)
		return;
	if (page->type == STRUCT_PAGE) {
		delete_struct(page->index, slot);
	}
	delete_page_vars(page);
	free_page(page);
}

/*
 * Recursively copy a page.
 */
struct page *copy_page(struct page *src)
{
	if (!src)
		return NULL;
	struct page *dst = alloc_page(src->type, src->index, src->nr_vars);
	dst->array = src->array;

	for (int i = 0; i < src->nr_vars; i++) {
		dst->values[i] = vm_copy(src->values[i], variable_type(src, i, NULL, NULL));
	}
	return dst;
}

int alloc_struct(int no)
{
	struct ain_struct *s = &ain->structures[no];
	int slot = heap_alloc_slot(VM_PAGE);
	heap_set_page(slot, alloc_page(STRUCT_PAGE, no, s->nr_members));
	for (int i = 0; i < s->nr_members; i++) {
		if (s->members[i].type.data == AIN_STRUCT) {
			heap[slot].page->values[i].i = alloc_struct(s->members[i].type.struc);
		} else {
			heap[slot].page->values[i] = variable_initval(s->members[i].type.data);
		}
	}
	init_option_vars(heap[slot].page, s->members, s->nr_members, 0);
	return slot;
}

void init_struct(int no, int slot)
{
	struct ain_struct *s = &ain->structures[no];
	for (int i = 0; i < s->nr_members; i++) {
		if (s->members[i].type.data == AIN_STRUCT) {
			init_struct(s->members[i].type.struc, heap[slot].page->values[i].i);
		}
	}
	if (s->constructor > 0) {
		vm_call(s->constructor, slot);
	}
}

void delete_struct(int no, int slot)
{
	struct ain_struct *s = &ain->structures[no];
	if (s->destructor > 0) {
		vm_call(s->destructor, slot);
	}
}

void create_struct(int no, union vm_value *var)
{
	var->i = alloc_struct(no);
	init_struct(no, var->i);
}

static enum ain_data_type unref_array_type(enum ain_data_type type)
{
	switch (type) {
	case AIN_REF_ARRAY_INT:       return AIN_ARRAY_INT;
	case AIN_REF_ARRAY_FLOAT:     return AIN_ARRAY_FLOAT;
	case AIN_REF_ARRAY_STRING:    return AIN_ARRAY_STRING;
	case AIN_REF_ARRAY_STRUCT:    return AIN_ARRAY_STRUCT;
	case AIN_REF_ARRAY_FUNC_TYPE: return AIN_ARRAY_FUNC_TYPE;
	case AIN_REF_ARRAY_BOOL:      return AIN_ARRAY_BOOL;
	case AIN_REF_ARRAY_LONG_INT:  return AIN_ARRAY_LONG_INT;
	case AIN_REF_ARRAY_DELEGATE:  return AIN_ARRAY_DELEGATE;
	case AIN_ARRAY_TYPE:          return type;
	// Ixseal: маркер массива двухслотовых wrap<интерфейс>-элементов.
	case AIN_IFACE_WRAP:          return AIN_IFACE_WRAP;
	default: VM_ERROR("Attempt to array allocate non-array type");
	}
}

// Инициализация одного элемента массива по его индексу (в ЭЛЕМЕНТАХ).
static void init_array_elem(struct page *page, int elem, enum ain_data_type type,
			    int struct_type, bool init_structs)
{
	if (array_elem_slots(page) == 2) {
		// Пустая ссылка wrap<интерфейс>: объекта нет, база интерфейса 0.
		page->values[elem*2].i = -1;
		page->values[elem*2 + 1].i = 0;
	} else if (type == AIN_STRUCT && init_structs) {
		create_struct(struct_type, &page->values[elem]);
	} else {
		page->values[elem] = variable_initval(type);
	}
}

struct page *alloc_array(int rank, union vm_value *dimensions, enum ain_data_type data_type, int struct_type, bool init_structs)
{
	if (rank < 1)
		return NULL;

	data_type = unref_array_type(data_type);
	enum ain_data_type type = array_type(data_type);
	// `dimensions` задаёт число ЭЛЕМЕНТОВ; слотов страницы может быть вдвое
	// больше (Ixseal wrap<интерфейс> — см. array_elem_slots).
	int slots = elem_slots_for_type(data_type, rank);
	struct page *page = alloc_page(ARRAY_PAGE, data_type, max(0, dimensions->i) * slots);
	page->array.struct_type = struct_type;
	page->array.rank = rank;

	for (int i = 0; i < dimensions->i; i++) {
		if (rank == 1) {
			init_array_elem(page, i, type, struct_type, init_structs);
		} else {
			struct page *child = alloc_array(rank - 1, dimensions + 1, data_type, struct_type, init_structs);
			int slot = heap_alloc_slot(VM_PAGE);
			heap_set_page(slot, child);
			page->values[i].i = slot;
		}
	}
	return page;
}

struct page *realloc_array(struct page *src, int rank, union vm_value *dimensions, enum ain_data_type data_type, int struct_type, bool init_structs)
{
	if (rank < 1)
		VM_ERROR("Tried to allocate 0-rank array");
	if (!src && !dimensions->i)
		return NULL;
	if (!src)
		return alloc_array(rank, dimensions, data_type, struct_type, init_structs);
	if (src->type != ARRAY_PAGE)
		VM_ERROR("Not an array");
	if (src->array.rank != rank)
		VM_ERROR("Attempt to reallocate array with different rank");
	if (!dimensions->i) {
		delete_page_vars(src);
		free_page(src);
		return NULL;
	}

	// `dimensions` — в ЭЛЕМЕНТАХ; переводим в слоты страницы (шаг берём из самой
	// страницы, а не из data_type: вызывающие передают сюда и dst->a_type, и
	// объявленный тип, а маркер wrap<интерфейс> надёжно хранится в странице).
	int slots = array_elem_slots(src);
	int old_vars = src->nr_vars;
	int new_vars = dimensions->i * slots;

	// if shrinking array, unref orphaned children
	if (new_vars < old_vars) {
		for (int i = new_vars; i < old_vars; i++) {
			variable_fini(src->values[i], variable_type(src, i, NULL, NULL), true);
		}
	}

	src = xrealloc(src, sizeof(struct page) + sizeof(union vm_value) * new_vars);

	// if growing array, init new children
	enum ain_data_type type = array_type(data_type);
	if (new_vars > old_vars) {
		src->nr_vars = new_vars;
		for (int i = old_vars / slots; i < dimensions->i; i++) {
			if (rank == 1) {
				init_array_elem(src, i, type, struct_type, init_structs);
			} else {
				struct page *child = alloc_array(rank - 1, dimensions + 1, data_type, struct_type, init_structs);
				int slot = heap_alloc_slot(VM_PAGE);
				heap_set_page(slot, child);
				src->values[i].i = slot;
			}
		}
	}

	src->nr_vars = new_vars;
	return src;
}

int array_numof(struct page *page, int rank)
{
	if (!page)
		return 0;
	if (rank < 1 || rank > page->array.rank)
		return 0;
	if (rank == 1) {
		return page->nr_vars / array_elem_slots(page);
	}
	return array_numof(heap[page->values[0].i].page, rank - 1);
}

// `i` — индекс ЭЛЕМЕНТА (не слота страницы).
static bool array_index_ok(struct page *array, int i)
{
	return i >= 0 && i < array->nr_vars / array_elem_slots(array);
}

void array_copy(struct page *dst, int dst_i, struct page *src, int src_i, int n)
{
	if (n <= 0)
		return;
	if (!dst || !src)
		VM_ERROR("Array is NULL");
	if (dst->type != ARRAY_PAGE || src->type != ARRAY_PAGE)
		VM_ERROR("Not an array");
	if (!array_index_ok(dst, dst_i) || !array_index_ok(src, src_i))
		VM_ERROR("Out of bounds array access");
	if (!array_index_ok(dst, dst_i + n - 1) || !array_index_ok(src, src_i + n - 1))
		VM_ERROR("Out of bounds array access");
	if (dst->array.rank != 1 || src->array.rank != 1)
		VM_ERROR("Tried to copy to/from a multi-dimensional array");
	if (dst->a_type != src->a_type)
		VM_ERROR("Array types do not match");

	// Копируем послотно: у двухслотового элемента тип слота зависит от чётности
	// (объект / база интерфейса), variable_type() это учитывает.
	int slots = array_elem_slots(dst);
	for (int i = 0; i < n * slots; i++) {
		int di = dst_i*slots + i, si = src_i*slots + i;
		enum ain_data_type type = variable_type(dst, di, NULL, NULL);
		variable_set(dst, di, type, vm_copy(src->values[si], type));
	}
}

int array_fill(struct page *dst, int dst_i, int n, union vm_value v)
{
	if (!dst)
		return 0;
	if (dst->type != ARRAY_PAGE)
		VM_ERROR("Not an array");
	if (array_elem_slots(dst) != 1) {
		// Заполнение одним значением бессмысленно для двухслотового
		// wrap<интерфейс>-элемента (нужна пара). Игра этого пути не использует;
		// сообщаем явно, вместо того чтобы молча испортить пары.
		WARNING("array_fill: не поддержано для массива wrap<интерфейс>");
		return 0;
	}

	// clamp (dst_i, dst_i+n) to range of array
	if (dst_i < 0) {
		n += dst_i;
		dst_i = 0;
	}
	if (dst_i >= dst->nr_vars)
		return 0;
	if (dst_i + n >= dst->nr_vars)
		n = dst->nr_vars - dst_i;

	enum ain_data_type type = array_type(dst->a_type);
	for (int i = 0; i < n; i++) {
		variable_set(dst, dst_i+i, type, vm_copy(v, type));
	}
	variable_fini(v, type, true);
	return n;
}

// Запись слотов одного элемента: `v[0..nvals-1]`, недостающие слоты — нулями.
static void array_set_elem(struct page *page, int elem, const union vm_value *v, int nvals)
{
	int slots = array_elem_slots(page);
	for (int k = 0; k < slots; k++) {
		union vm_value val = k < nvals ? v[k] : (union vm_value) { .i = 0 };
		int varno = elem*slots + k;
		variable_set(page, varno, variable_type(page, varno, NULL, NULL), val);
	}
}

/*
 * Добавить элемент в конец. `v` указывает на `nvals` подряд идущих слотов
 * значения: обычный элемент — один слот, Ixseal wrap<интерфейс> — два
 * (объект, база интерфейса).
 */
struct page *array_pushback_n(struct page *dst, const union vm_value *v, int nvals,
			      enum ain_data_type data_type, int struct_type)
{
	if (dst) {
		if (dst->type != ARRAY_PAGE)
			VM_ERROR("Not an array");
		if (dst->array.rank != 1)
			VM_ERROR("Tried pushing to a multi-dimensional array");

		int index = dst->nr_vars / array_elem_slots(dst);
		union vm_value dims[1] = { (union vm_value) { .i = index + 1 } };
		dst = realloc_array(dst, 1, dims, dst->a_type, dst->array.struct_type, false);
		array_set_elem(dst, index, v, nvals);
	} else {
		union vm_value dims[1] = { (union vm_value) { .i = 1 } };
		dst = alloc_array(1, dims, data_type, struct_type, false);
		array_set_elem(dst, 0, v, nvals);
	}
	return dst;
}

struct page *array_pushback(struct page *dst, union vm_value v, enum ain_data_type data_type, int struct_type)
{
	return array_pushback_n(dst, &v, 1, data_type, struct_type);
}

struct page *array_popback(struct page *dst)
{
	if (!dst)
		return NULL;
	if (dst->type != ARRAY_PAGE)
		VM_ERROR("Not an array");
	if (dst->array.rank != 1)
		VM_ERROR("Tried popping from a multi-dimensional array");

	union vm_value dims[1] = { (union vm_value) { .i = dst->nr_vars / array_elem_slots(dst) - 1 } };
	dst = realloc_array(dst, 1, dims, dst->a_type, dst->array.struct_type, false);
	return dst;
}

struct page *array_erase(struct page *page, int i, bool *success)
{
	*success = false;
	if (!page)
		return NULL;
	if (page->type != ARRAY_PAGE)
		VM_ERROR("Not an array");
	if (page->array.rank != 1)
		VM_ERROR("Tried erasing from a multi-dimensional array");
	if (!array_index_ok(page, i))
		return page;

	int slots = array_elem_slots(page);

	// if array will be empty...
	if (page->nr_vars == slots) {
		delete_page_vars(page);
		free_page(page);
		*success = true;
		return NULL;
	}

	// delete variable(s), shift subsequent variables, then realloc page
	for (int k = 0; k < slots; k++) {
		int varno = i*slots + k;
		variable_fini(page->values[varno], variable_type(page, varno, NULL, NULL), true);
	}
	for (int j = (i + 1) * slots; j < page->nr_vars; j++) {
		page->values[j-slots] = page->values[j];
	}
	page->nr_vars -= slots;
	page = xrealloc(page, sizeof(struct page) + sizeof(union vm_value) * page->nr_vars);

	*success = true;
	return page;
}

// `v` — `nvals` подряд идущих слотов значения (см. array_pushback_n).
struct page *array_insert_n(struct page *page, int i, const union vm_value *v, int nvals,
			    enum ain_data_type data_type, int struct_type)
{
	if (!page) {
		return array_pushback_n(NULL, v, nvals, data_type, struct_type);
	}
	if (page->type != ARRAY_PAGE)
		VM_ERROR("Not an array");
	if (page->array.rank != 1)
		VM_ERROR("Tried inserting into a multi-dimensional array");

	int slots = array_elem_slots(page);
	int n = page->nr_vars / slots;

	// NOTE: you cannot insert at the end of an array due to how i is clamped
	if (i >= n)
		i = n - 1;
	if (i < 0)
		i = 0;

	page->nr_vars += slots;
	page = xrealloc(page, sizeof(struct page) + sizeof(union vm_value) * page->nr_vars);
	for (int j = page->nr_vars - 1; j >= (i + 1) * slots; j--) {
		page->values[j] = page->values[j-slots];
	}
	for (int k = 0; k < slots; k++)
		page->values[i*slots + k] = k < nvals ? v[k] : (union vm_value) { .i = 0 };
	return page;
}

struct page *array_insert(struct page *page, int i, union vm_value v, enum ain_data_type data_type, int struct_type)
{
	return array_insert_n(page, i, &v, 1, data_type, struct_type);
}

static int array_compare_int(const void *_a, const void *_b)
{
	union vm_value a = *((union vm_value*)_a);
	union vm_value b = *((union vm_value*)_b);
	return (a.i > b.i) - (a.i < b.i);
}

static int array_compare_float(const void *_a, const void *_b)
{
	union vm_value a = *((union vm_value*)_a);
	union vm_value b = *((union vm_value*)_b);
	return (a.f > b.f) - (a.f < b.f);
}

static int array_compare_string(const void *_a, const void *_b)
{
	union vm_value a = *((union vm_value*)_a);
	union vm_value b = *((union vm_value*)_b);
	return strcmp(heap_get_string(a.i)->text, heap_get_string(b.i)->text);
}

// Used for stable sorting arrays with qsort()
struct sortable {
	union vm_value v;
	int index;
};

static int current_sort_function;

static int array_compare_custom(const void *_a, const void *_b)
{
	const struct sortable *a = _a;
	const struct sortable *b = _b;
	stack_push(a->v);
	stack_push(b->v);
	vm_call(current_sort_function, -1);
	int d = stack_pop().i;
	return d ? d : a->index - b->index;
}

static int array_compare_custom_string(const void *_a, const void *_b)
{
	const struct sortable *a = _a;
	const struct sortable *b = _b;
	stack_push(vm_string_ref(heap_get_string(a->v.i)));
	stack_push(vm_string_ref(heap_get_string(b->v.i)));
	vm_call(current_sort_function, -1);
	int d = stack_pop().i;
	return d ? d : a->index - b->index;
}

void array_sort(struct page *page, int compare_fno)
{
	if (!page)
		return;
	if (array_elem_slots(page) != 1) {
		// Сортировка перемешала бы половинки двухслотовых элементов, а
		// упорядочивать ссылки wrap<интерфейс> по heap-слоту бессмысленно.
		WARNING("array_sort: пропущено для массива wrap<интерфейс>");
		return;
	}

	if (compare_fno) {
		struct sortable *values = xcalloc(page->nr_vars, sizeof(struct sortable));
		for (int i = 0; i < page->nr_vars; i++) {
			values[i].v = page->values[i];
			values[i].index = i;
		}
		current_sort_function = compare_fno;
		qsort(values, page->nr_vars, sizeof(struct sortable),
			page->a_type == AIN_ARRAY_STRING ? array_compare_custom_string : array_compare_custom);
		for (int i = 0; i < page->nr_vars; i++) {
			page->values[i] = values[i].v;
		}
		free(values);
	} else {
		switch (page->a_type) {
		case AIN_ARRAY_INT:
		case AIN_ARRAY_LONG_INT:
			qsort(page->values, page->nr_vars, sizeof(union vm_value), array_compare_int);
			break;
		case AIN_ARRAY_FLOAT:
			qsort(page->values, page->nr_vars, sizeof(union vm_value), array_compare_float);
			break;
		case AIN_ARRAY_STRING:
			qsort(page->values, page->nr_vars, sizeof(union vm_value), array_compare_string);
			break;
		default:
			VM_ERROR("A_SORT(&NULL) called on ain_data_type %d", page->a_type);
		}
	}
}

static int current_sort_member;

static int array_compare_member(const void *_a, const void *_b)
{
	const struct sortable *a = _a;
	const struct sortable *b = _b;
	int32_t a_i = heap_get_page(a->v.i)->values[current_sort_member].i;
	int32_t b_i = heap_get_page(b->v.i)->values[current_sort_member].i;
	int d = (a_i > b_i) - (a_i < b_i);
	return d ? d : a->index - b->index;
}

static int array_compare_member_string(const void *_a, const void *_b)
{
	const struct sortable *a = _a;
	const struct sortable *b = _b;
	int32_t a_i = heap_get_page(a->v.i)->values[current_sort_member].i;
	int32_t b_i = heap_get_page(b->v.i)->values[current_sort_member].i;
	int d = strcmp(heap_get_string(a_i)->text, heap_get_string(b_i)->text);
	return d ? d : a->index - b->index;
}

void array_sort_mem(struct page *page, int member_no)
{
	if (!page)
		return;
	if (page->type != ARRAY_PAGE || array_type(page->a_type) != AIN_STRUCT)
		VM_ERROR("A_SORT_MEM called on something other than an array of structs");

	struct ain_struct *s = &ain->structures[page->array.struct_type];
	if (member_no < 0 || member_no >= s->nr_members)
		VM_ERROR("A_SORT_MEM called with invalid member index");

	struct sortable *values = xcalloc(page->nr_vars, sizeof(struct sortable));
	for (int i = 0; i < page->nr_vars; i++) {
		values[i].v = page->values[i];
		values[i].index = i;
	}
	current_sort_member = member_no;
	if (s->members[member_no].type.data == AIN_STRING)
		qsort(values, page->nr_vars, sizeof(struct sortable), array_compare_member_string);
	else
		qsort(values, page->nr_vars, sizeof(struct sortable), array_compare_member);
	for (int i = 0; i < page->nr_vars; i++) {
		page->values[i] = values[i].v;
	}
	free(values);
}

int array_find(struct page *page, int start, int end, union vm_value v, int compare_fno)
{
	if (!page)
		return -1;

	int slots = array_elem_slots(page);
	start = max(start, 0);
	end = min(end, page->nr_vars / slots);

	if (slots != 1) {
		// Двухслотовый wrap<интерфейс>: сравниваем по heap-слоту объекта
		// (нижний слот), база интерфейса у одного объекта постоянна.
		for (int i = start; i < end; i++) {
			if (page->values[i*slots].i == v.i)
				return i;
		}
		return -1;
	}

	// if no compare function given, compare integer/string values
	if (!compare_fno) {
		if (array_type(page->a_type) == AIN_STRING) {
			struct string *v_str = heap_get_string(v.i);
			for (int i = start; i < end; i++) {
				if (!strcmp(v_str->text, heap_get_string(page->values[i].i)->text))
					return i;
			}
		} else {
			for (int i = start; i < end; i++) {
				if (page->values[i].i == v.i)
					return i;
			}
		}
		return -1;
	}

	for (int i = start; i < end; i++) {
		stack_push(v);
		stack_push(page->values[i]);
		vm_call(compare_fno, -1);
		if (stack_pop().i)
			return i;
	}

	return -1;
}

void array_reverse(struct page *page)
{
	if (!page)
		return;

	// Переворачиваем поэлементно: у двухслотового элемента пара слотов должна
	// остаться в исходном порядке (объект, база интерфейса).
	int slots = array_elem_slots(page);
	for (int start = 0, end = page->nr_vars/slots - 1; start < end; start++, end--) {
		for (int k = 0; k < slots; k++) {
			union vm_value tmp = page->values[start*slots + k];
			page->values[start*slots + k] = page->values[end*slots + k];
			page->values[end*slots + k] = tmp;
		}
	}
}

struct page *delegate_new_from_method(int obj, int fun)
{
	if (fun < 1)
		return alloc_page(DELEGATE_PAGE, 0, 0);
	struct page *page = alloc_page(DELEGATE_PAGE, 0, 3);
	page->values[0].i = obj;
	page->values[1].i = fun;
	page->values[2].i = heap_get_seq(obj);
	return page;
}

bool delegate_contains(struct page *dst, int obj, int fun)
{
	if (!dst)
		return false;
	for (int i = 0; i < dst->nr_vars; i += 3) {
		if (dst->values[i].i == obj &&
		    dst->values[i+1].i == fun &&
		    dst->values[i+2].i == heap_get_seq(obj))
			return true;
	}
	return false;
}

struct page *delegate_append(struct page *dst, int obj, int fun)
{
	if (fun < 1)
		return dst;
	if (!dst)
		return delegate_new_from_method(obj, fun);
	if (dst->type != DELEGATE_PAGE)
		VM_ERROR("Not a delegate");
	if (delegate_contains(dst, obj, fun))
		return dst;

	dst = xrealloc(dst, sizeof(struct page) + sizeof(union vm_value) * (dst->nr_vars + 3));
	dst->values[dst->nr_vars+0].i = obj;
	dst->values[dst->nr_vars+1].i = fun;
	dst->values[dst->nr_vars+2].i = heap_get_seq(obj);
	dst->nr_vars += 3;
	return dst;
}

int delegate_numof(struct page *page)
{
	if (!page)
		return 0;
	if (page->type != DELEGATE_PAGE)
		VM_ERROR("Not a delegate");

	// garbage collection
	for (int i = 0; i < page->nr_vars; i += 3) {
		if (heap_get_seq(page->values[i].i) != page->values[i+2].i) {
			for (int j = i+3; j < page->nr_vars; j += 3) {
				page->values[j-3].i = page->values[j+0].i;
				page->values[j-2].i = page->values[j+1].i;
				page->values[j-1].i = page->values[j+2].i;
			}
			page->nr_vars -= 3;
			i -= 3;
		}
	}
	return page->nr_vars / 3;
}

void delegate_erase(struct page *page, int obj, int fun)
{
	if (!page)
		return;
	if (page->type != DELEGATE_PAGE)
		VM_ERROR("Not a delegate");
	for (int i = 0; i < page->nr_vars; i += 3) {
		if (page->values[i].i == obj && page->values[i+1].i == fun) {
			for (int j = i+3; j < page->nr_vars; j += 3) {
				page->values[j-3].i = page->values[j+0].i;
				page->values[j-2].i = page->values[j+1].i;
				page->values[j-1].i = page->values[j+2].i;
			}
			page->nr_vars -= 3;
			break;
		}
	}
}

struct page *delegate_plusa(struct page *dst, struct page *add)
{
	if (!add)
		return dst;
	if ((dst && dst->type != DELEGATE_PAGE) || add->type != DELEGATE_PAGE)
		VM_ERROR("Not a delegate");

	for (int i = 0; i < add->nr_vars; i += 3) {
		if (heap_get_seq(add->values[i].i) == add->values[i+2].i)
			dst = delegate_append(dst, add->values[i].i, add->values[i+1].i);
	}
	return dst;
}

struct page *delegate_minusa(struct page *dst, struct page *minus)
{
	if (!dst)
		return NULL;
	if (!minus)
		return dst;
	if (dst->type != DELEGATE_PAGE || minus->type != DELEGATE_PAGE)
		VM_ERROR("Not a delegate");

	for (int i = 0; i < minus->nr_vars; i += 3) {
		if (heap_get_seq(minus->values[i].i) == minus->values[i+2].i)
			delegate_erase(dst, minus->values[i].i, minus->values[i+1].i);
	}

	return dst;
}

struct page *delegate_clear(struct page *page)
{
	if (!page)
		return NULL;
	if (page->type != DELEGATE_PAGE)
		VM_ERROR("Not a delegate");
	for (int i = 0; i < page->nr_vars; i += 3) {
		page->values[i].i = -1;
		page->values[i+1].i = -1;
		page->values[i+2].i = 0;
	}
	page->index = 0;
	page->nr_vars = 0;
	return page;
}

bool delegate_get(struct page *page, int i, int *obj_out, int *fun_out)
{
	if (!page)
		return false;
	if (page->type != DELEGATE_PAGE)
		VM_ERROR("Not a delegate");
	while (i*3 < page->nr_vars) {
		if (heap_get_seq(page->values[i*3].i) == page->values[i*3+2].i) {
			*obj_out = page->values[i*3].i;
			*fun_out = page->values[i*3+1].i;
			return true;
		}
		for (int j = (i + 1) * 3; j < page->nr_vars; j += 3) {
			page->values[j-3].i = page->values[j+0].i;
			page->values[j-2].i = page->values[j+1].i;
			page->values[j-1].i = page->values[j+2].i;
		}
		page->nr_vars -= 3;
	}
	return false;
}
