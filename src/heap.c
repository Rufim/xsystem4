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

#define VM_PRIVATE

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "system4/string.h"
#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "xsystem4.h"

#define INITIAL_HEAP_SIZE  4096
#define HEAP_ALLOC_STEP    4096

struct vm_pointer *heap = NULL;
size_t heap_size = 0;
uint32_t heap_next_seq;

// Heap free list
// This is a list of unused indices into the 'heap' array.
int32_t *heap_free_stack = NULL;
size_t heap_free_ptr = 0;

static const char *vm_ptrtype_strtab[] = {
	[VM_PAGE] = "VM_PAGE",
	[VM_STRING] = "VM_STRING",
};

static const char *vm_ptrtype_string(enum vm_pointer_type type) {
	if (type < NR_VM_POINTER_TYPES)
		return vm_ptrtype_strtab[type];
	return "INVALID POINTER TYPE";
}

void heap_grow(size_t new_size)
{
	assert(new_size > heap_size);
	heap = xrealloc(heap, sizeof(struct vm_pointer) * new_size);
	heap_free_stack = xrealloc(heap_free_stack, sizeof(int32_t) * new_size);
	for (size_t i = heap_size; i < new_size; i++) {
		heap[i].ref = 0;
		heap_free_stack[i] = i;
	}
	heap_size = new_size;
}

/*
 * Поколение VM-образа: инкрементируется при подмене всей кучи/стека
 * (ResumeLoad/Reset). hll_call сверяет его до и после C-вызова: если образ
 * сменился, пост-финализация аргументов и write-back'и должны быть ПРОПУЩЕНЫ —
 * их номера слотов принадлежат СТАРОМУ миру, а в новом по тем же номерам живут
 * чужие объекты (double free слота 1 сразу после загрузки сейва).
 */
uint32_t vm_image_generation;

void heap_init(void)
{
	if (!heap) {
		heap_size = INITIAL_HEAP_SIZE;
		heap = xcalloc(1, INITIAL_HEAP_SIZE * sizeof(struct vm_pointer));
		heap_free_stack = xmalloc(INITIAL_HEAP_SIZE * sizeof(int32_t));
	} else {
		memset(heap, 0, heap_size * sizeof(struct vm_pointer*));
	}

	for (size_t i = 0; i < heap_size; i++) {
		heap_free_stack[i] = i;
	}
	heap_free_ptr = 1; // global page at index 0
	heap_next_seq = 1;
}

// Точечная диагностика владения: XSYS4_HEAP_WATCH=<slot> печатает каждое
// событие жизненного цикла слота вместе с адресом инструкции VM.
// Вотчей МНОГО (по кругу): XSYS4_STRUCT_WATCH ставит вотч на КАЖДЫЙ созданный
// объект типа, и одноместный вотч молча съезжал на последний созданный —
// у предыдущих объектов «терялись» все дальнейшие REF/UNREF.
#define HEAP_WATCH_MAX 512
static int heap_watch_slots[HEAP_WATCH_MAX];
static int heap_watch_nr = -1;

// XSYS4_HEAP_WATCH_FILE=<путь> — события вотча писать в ОТДЕЛЬНЫЙ файл, минуя
// общий лог: тот режется на 200000 строк, а вотч по типу (STRUCT_WATCH=SceneStack)
// даёт ~130k событий за прогон — конец сценария (закрытие экрана, FREE) не влезал.
static FILE *heap_watch_out(void)
{
	static FILE *f;
	static bool tried;
	if (!tried) {
		tried = true;
		const char *p = getenv("XSYS4_HEAP_WATCH_FILE");
		if (p && *p) {
			f = fopen(p, "w");
			if (f)
				setvbuf(f, NULL, _IOLBF, 0);
		}
	}
	return f;
}

// Бухгалтерия ссылок сводится по паре «какая инструкция в какой функции», поэтому
// каждая строка несёт instr_ptr + опкод + функцию игры; в файл — без префиксов лога.
static void heap_watch_msg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	FILE *f = heap_watch_out();
	if (f) {
		vfprintf(f, fmt, ap);
		fputc('\n', f);
	} else {
		char buf[1024];
		vsnprintf(buf, sizeof(buf), fmt, ap);
		sys_warning("*WATCH*: %s\n", buf);
	}
	va_end(ap);
}
static bool heap_watched(int32_t slot)
{
	if (heap_watch_nr == -1) {
		heap_watch_nr = 0;
		const char *s = getenv("XSYS4_HEAP_WATCH");
		if (s && *s)
			heap_watch_slots[heap_watch_nr++] = atoi(s);
	}
	for (int i = 0; i < heap_watch_nr; i++) {
		if (heap_watch_slots[i] == slot)
			return true;
	}
	return false;
}

bool heap_slot_is_page(int index)
{
	if (index <= 0 || !heap_index_valid(index))
		return false;
	return heap[index].type == VM_PAGE && heap[index].ref > 0 && heap[index].page;
}

bool heap_slot_is_string(int index)
{
	if (index <= 0 || !heap_index_valid(index))
		return false;
	return heap[index].type == VM_STRING && heap[index].ref > 0;
}

void heap_watch_slot_set(int32_t slot)
{
	heap_watched(-1); // инициализировать список из env, если ещё не
	if (heap_watch_nr < HEAP_WATCH_MAX)
		heap_watch_slots[heap_watch_nr++] = slot;
	else
		heap_watch_slots[heap_watch_nr - 1] = slot; // последняя ячейка — по кругу
}

int32_t heap_alloc_slot(enum vm_pointer_type type)
{
	if (heap_free_ptr >= heap_size) {
		heap_grow(heap_size+HEAP_ALLOC_STEP);
	}

	int32_t slot = heap_free_stack[heap_free_ptr++];
	heap[slot].ref = 1;
	heap[slot].seq = heap_next_seq++;
	heap[slot].type = type;
#ifdef DEBUG_HEAP
	heap[slot].alloc_addr = instr_ptr;
	memset(heap[slot].ref_addr, 0, sizeof(heap[slot].ref_addr));
	heap[slot].ref_nr = 0;
	memset(heap[slot].deref_addr, 0, sizeof(heap[slot].deref_addr));
	heap[slot].deref_nr = 0;
	heap[slot].free_addr = 0;
#endif
	if (unlikely(heap_watched(slot)))
		heap_watch_msg("HEAPWATCH %d ALLOC type=%d @%X in %s", slot, type, instr_ptr,
			       display_sjis0(vm_current_function_name()));
	return slot;
}

static void heap_free_slot(int32_t slot)
{
	// Наблюдаемый слот умер — печатаем СТЕК ИГРЫ: момент смерти важнее момента
	// падения (падение при чтении случается позже и в другом месте).
	if (unlikely(heap_watched(slot))) {
		heap_watch_msg("HEAPWATCH %d FREE — стек вызовов игры:", slot);
		FILE *f = heap_watch_out();
		if (f)
			vm_stack_trace_file(f);
		else
			vm_stack_trace();
	}
	heap[slot].seq = 0;
	heap_free_stack[--heap_free_ptr] = slot;
}

static void heap_double_free(int32_t slot)
{
	// Стек вызовов игры — иначе виновника приходится угадывать по номеру слота, а
	// слоты переиспользуются и от прогона к прогону разные.
	WARNING("double free of slot %d (%s); последнее variable_fini: %s — стек вызовов игры:",
		slot, vm_ptrtype_string(heap[slot].type), vm_last_fini_str());
	vm_stack_trace();
#ifdef DEBUG_HEAP
		WARNING("double free of slot %d (%s) ref=%d seq=%u\nOriginally allocated at %X\nOriginally freed at %X",
			 slot, vm_ptrtype_string(heap[slot].type), heap[slot].ref, heap[slot].seq,
			 heap[slot].alloc_addr, heap[slot].free_addr);
		sys_message("  refs(%d):", heap[slot].ref_nr);
		for (int i = 0; i < heap[slot].ref_nr && i < 16; i++)
			sys_message(" %X", heap[slot].ref_addr[i]);
		sys_message("\n  derefs(%d):", heap[slot].deref_nr);
		for (int i = 0; i < heap[slot].deref_nr && i < 16; i++)
			sys_message(" %X", heap[slot].deref_addr[i]);
		sys_message("\n");
#else
		WARNING("double free of slot %d (%s)", slot, vm_ptrtype_string(heap[slot].type));
#endif
}

void heap_ref(int32_t slot)
{
	if (slot == -1)
		return;
	// XSYS4_REF_DEAD_TRACE=1 — детектор use-after-free ПО ВЛАДЕНИЮ: взятие ссылки
	// на уже освобождённый слот означает, что чей-то хэндл пережил объект. Ловит
	// момент ДО падения (падение случается позже и в другом месте — при чтении).
	if (unlikely(heap[slot].ref <= 0) && getenv("XSYS4_REF_DEAD_TRACE")) {
		WARNING("REF-DEAD слот %d (ref=%d) — стек вызовов игры:", slot, heap[slot].ref);
		vm_stack_trace();
	}
	heap[slot].ref++;
	if (unlikely(heap_watched(slot)))
		heap_watch_msg("HEAPWATCH %d REF -> %d @%X [%s] in %s", slot, heap[slot].ref, instr_ptr, vm_current_instruction_name(), display_sjis0(vm_current_function_name()));
#ifdef DEBUG_HEAP
	heap[slot].ref_addr[heap[slot].ref_nr++ % 16] = instr_ptr;
#endif
}

void heap_unref(int slot)
{
	if (unlikely(heap_watched(slot)))
		heap_watch_msg("HEAPWATCH %d UNREF (ref=%d) @%X [%s] in %s", slot, heap[slot].ref, instr_ptr, vm_current_instruction_name(), display_sjis0(vm_current_function_name()));
	if (unlikely(heap[slot].ref <= 0)) {
		heap_double_free(slot);
		VM_ERROR("double free");
	}
	if (heap[slot].ref > 1) {
#ifdef DEBUG_HEAP
		heap[slot].deref_addr[heap[slot].deref_nr++ % 16] = instr_ptr;
#endif
		heap[slot].ref--;
		return;
	}
#ifdef DEBUG_HEAP
	heap[slot].free_addr = instr_ptr;
#endif
	switch (heap[slot].type) {
	case VM_PAGE:
		if (heap[slot].page) {
			delete_page(slot);
		}
		break;
	case VM_STRING:
		free_string(heap[slot].s);
		break;
	}
	heap[slot].ref = 0;
	heap_free_slot(slot);
}

// XXX: special version of heap_unref which avoids calling destructors
void exit_unref(int slot)
{
	if (unlikely(heap_watched(slot))) {
		heap_watch_msg("HEAPWATCH %d EXIT_UNREF (ref=%d) @%X in %s", slot, heap[slot].ref,
			       instr_ptr, display_sjis0(vm_current_function_name()));
		FILE *f = heap_watch_out();
		if (f)
			vm_stack_trace_file(f);
		else
			vm_stack_trace();
	}
	if (slot < 0 || (size_t)slot >= heap_size) {
		WARNING("out of bounds heap index: %d", slot);
		return;
	}
	if (heap[slot].ref <= 0) {
		heap_double_free(slot);
		return;
	}
	if (heap[slot].ref > 1) {
#ifdef DEBUG_HEAP
		heap[slot].deref_addr[heap[slot].deref_nr++ % 16] = 0xDEADC0DE;
#endif
		heap[slot].ref--;
		return;
	}
	switch (heap[slot].type) {
	case VM_PAGE:
		if (heap[slot].page) {
			struct page *page = heap[slot].page;
			for (int i = 0; i < page->nr_vars; i++) {
				switch (variable_type(page, i, NULL, NULL)) {
				case AIN_STRING:
				case AIN_STRUCT:
				case AIN_DELEGATE:
				case AIN_ARRAY_TYPE:
				case AIN_REF_TYPE:
					if (page->values[i].i == -1)
						break;
					/*
					 * Слот 0 — ГЛОБАЛЬНАЯ СТРАНИЦА: аллокатор его не выдаёт
					 * (heap_free_ptr стартует с 1), владеть им не может НИ ОДНО
					 * поле. Ноль в объектном поле — всегда чужое число, и
					 * освобождать его нельзя: уносит ВСЕ глобалы, а падает это
					 * далеко от причины — у Dohna слот 0 успевал
					 * переиспользоваться под строку, и чтение глобалов давало
					 * ASCII-мусор (SIGSEGV в heap_ref со «слотом» 0x30303031).
					 * В `variable_fini` такая проверка есть давно, но
					 * рекурсивный обход полей в exit_unref её обходил.
					 */
					if (page->values[i].i == 0) {
						static bool warned;
						if (!warned) {
							warned = true;
							WARNING("exit_unref: в объектном поле %d страницы %s лежит "
								"heap-слот 0 (глобальная страница) — чужое число; "
								"поле не освобождаем. Стек вызовов игры:", i,
								page->type == STRUCT_PAGE && page->index >= 0
								&& page->index < ain->nr_structures
								? display_sjis0(ain->structures[page->index].name)
								: "(не структура)");
							vm_stack_trace();
						}
						break;
					}
					exit_unref(page->values[i].i);
					break;
				default:
					break;
				}
			}
			free_page(page);
		}
		break;
	case VM_STRING:
		free_string(heap[slot].s);
		break;
	}
	heap[slot].ref = 0;
	heap_free_slot(slot);
}

uint32_t heap_get_seq(int slot)
{
	return heap_index_valid(slot) ? heap[slot].seq : 0;
}

bool heap_index_valid(int index)
{
	return index >= 0 && (size_t)index < heap_size && heap[index].ref > 0;
}

bool page_index_valid(int index)
{
	return heap_index_valid(index) && heap[index].type == VM_PAGE;
}

bool string_index_valid(int index)
{
	return heap_index_valid(index) && heap[index].type == VM_STRING;
}

struct page *heap_get_page(int index)
{
	if (unlikely(!page_index_valid(index)))
		VM_ERROR("Invalid page index: %d", index);
	return heap[index].page;
}

struct string *heap_get_string(int index)
{
	if (unlikely(!string_index_valid(index)))
		VM_ERROR("Invalid string index: %d", index);
	return heap[index].s;
}

struct page *heap_get_delegate_page(int index)
{
	struct page *page = heap_get_page(index);
	if (unlikely(page && page->type != DELEGATE_PAGE))
		VM_ERROR("Not a delegate page: %d", index);
	return page;
}

void heap_set_page(int slot, struct page *page)
{
#ifdef DEBUG_HEAP
	if (unlikely(!page_index_valid(slot)))
		VM_ERROR("Invalid page index: %d", index);
#endif
	heap[slot].page = page;
}

void heap_string_assign(int slot, struct string *string)
{
	/*
	 * Присваивание по НУЛЕВОЙ ссылке — ошибка VM, а не SIGSEGV. `heap[-1]` даёт
	 * мусорный указатель, и падение происходило внутри `free_string` в libsys4,
	 * то есть в месте, никак не указывающем на виноватую инструкцию. У соседнего
	 * `heap_struct_assign` такая проверка есть с самого начала — здесь её не было.
	 * Живой случай: Haha Ranman, `■実行済コマンド設定` @0x699aae делает
	 * `Array.At(list, nTimezone)` без проверки результата (в соседней
	 * `■実行済コマンド取得` игра его проверяет на -1) и присваивает по ссылке.
	 */
	if (unlikely(slot < 0))
		VM_ERROR("Assignment to null string reference");
#ifdef DEBUG_HEAP
	if (unlikely(!string_index_valid(slot)))
		VM_ERROR("Tried to assign string to non-string slot %d (type=%s ref=%d)",
			 slot, slot >= 0 && (size_t)slot < heap_size ? vm_ptrtype_string(heap[slot].type) : "OOB",
			 slot >= 0 && (size_t)slot < heap_size ? heap[slot].ref : -1);
#endif
	if (heap[slot].s) {
		free_string(heap[slot].s);
	}
	heap[slot].s = string_ref(string);
}

void heap_struct_assign(int lval, int rval)
{
	if (unlikely(lval == -1))
		VM_ERROR("Assignment to null-pointer");
	if (lval == rval)
		return;
#ifdef DEBUG_HEAP
	if (unlikely(!page_index_valid(lval)))
		VM_ERROR("Invalid page index: %d", lval);
	if (unlikely(!page_index_valid(rval)))
		VM_ERROR("Invalid page index: %d", rval);
	if (unlikely(heap[lval].page && heap[lval].page->type != STRUCT_PAGE))
		VM_ERROR("SR_ASSIGN to non-struct page");
	if (unlikely(heap[rval].page && heap[rval].page->type != STRUCT_PAGE))
		VM_ERROR("SR_ASSIGN from non-struct page");
	if (unlikely(heap[lval].page && heap[rval].page && heap[lval].page->index != heap[rval].page->index))
		VM_ERROR("SR_ASSIGN with different struct types");
#endif
	if (heap[lval].page) {
		delete_page(lval);
	}
	heap_set_page(lval, copy_page(heap[rval].page));
}

int32_t heap_alloc_string(struct string *s)
{
	int slot = heap_alloc_slot(VM_STRING);
	heap[slot].s = s;
	return slot;
}

int32_t heap_alloc_page(struct page *page)
{
	int slot = heap_alloc_slot(VM_PAGE);
	heap[slot].page = page;
	return slot;
}

static void describe_page(struct page *page)
{
	if (!page) {
		sys_message("NULL_PAGE\n");
		return;
	}

	switch (page->type) {
	case GLOBAL_PAGE:
		sys_message("GLOBAL_PAGE\n");
		break;
	case LOCAL_PAGE:
		sys_message("LOCAL_PAGE: %s\n", display_sjis0(ain->functions[page->index].name));
		break;
	case STRUCT_PAGE:
		sys_message("STRUCT_PAGE: %s\n", display_sjis0(ain->structures[page->index].name));
		break;
	case ARRAY_PAGE:
		sys_message("ARRAY_PAGE: %s\n", display_sjis0(ain_strtype(ain, page->a_type, page->array.struct_type)));
		break;
	case DELEGATE_PAGE:
		// TODO: list function names
		sys_message("DELEGATE_PAGE\n");
		break;
	}
}

void heap_describe_slot(int slot)
{
	if (heap[slot].type == VM_STRING && heap[slot].s == &EMPTY_STRING)
		return;
#ifdef DEBUG_HEAP
	sys_message("[%d](%d)(%08X)[", slot, heap[slot].ref, heap[slot].alloc_addr);
	for (int i = 0; i < heap[slot].ref_nr && i < 16; i++) {
		if (i > 0)
			sys_message(",");
		sys_message("%08X", heap[slot].ref_addr[i]);
	}
	sys_message("][");
	for (int i = 0; i < heap[slot].deref_nr && i < 16; i++) {
		if (i > 0)
			sys_message(",");
		sys_message("%08X", heap[slot].deref_addr[i]);
	}
	sys_message("] = ");
#else
	sys_message("[%d](%d) = ", slot, heap[slot].ref);
#endif
	switch (heap[slot].type) {
	case VM_PAGE:
		describe_page(heap[slot].page);
		break;
	case VM_STRING:
		if (heap[slot].s) {
			sys_message("STRING: %s\n", display_sjis0(heap[slot].s->text));
		} else {
			sys_message("STRING: NULL\n");
		}
		break;
	default:
		sys_message("???\n");
		break;
	}
}
