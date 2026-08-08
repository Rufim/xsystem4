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

#ifndef SYSTEM4_HEAP_H
#define SYSTEM4_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct string;
struct page;

enum vm_pointer_type {
	VM_PAGE,
	VM_STRING
};
#define NR_VM_POINTER_TYPES (VM_STRING+1)

// Heap-backed objects. Reference counted.
struct vm_pointer {
	int ref;
	uint32_t seq;
	enum vm_pointer_type type;
	union {
		struct string *s;
		struct page *page;
	};
#ifdef DEBUG_HEAP
	size_t alloc_addr;
	size_t ref_addr[16];
	size_t ref_nr;
	size_t deref_addr[16];
	size_t deref_nr;
	size_t free_addr;
#endif
};

extern struct vm_pointer *heap;
extern size_t heap_size;

void heap_init(void);
extern uint32_t vm_image_generation;
void heap_delete(void);
void heap_grow(size_t new_size);

int32_t heap_alloc_slot(enum vm_pointer_type type);
void heap_ref(int slot);
void heap_unref(int slot);
// Диагностика владения: следить за этим слотом (как XSYS4_HEAP_WATCH, но слот
// задаётся из кода — номер слота переиспользуется, поэтому по нему объект не
// опознать; ставится при создании структуры, чьё имя совпало с XSYS4_STRUCT_WATCH).
void heap_watch_slot_set(int32_t slot);
void exit_unref(int slot);

uint32_t heap_get_seq(int slot);

bool heap_index_valid(int index);
bool page_index_valid(int index);
bool string_index_valid(int index);

struct page *heap_get_page(int index);
// Безопасная проверка: лежит ли в слоте СТРАНИЦА (а не строка/пусто). Нужна там,
// где слот мог быть уже освобождён: `heap_get_page` на чужом типе — фатальная
// ошибка VM, а не NULL.
bool heap_slot_is_page(int index);
// Лежит ли в слоте СТРОКА (парный предикат к heap_slot_is_page).
bool heap_slot_is_string(int index);
struct page *heap_get_delegate_page(int index);
struct string *heap_get_string(int index);
void heap_set_page(int slot, struct page *page);
void heap_string_assign(int slot, struct string *string);
void heap_struct_assign(int lval, int rval);
int32_t heap_alloc_page(struct page *page);
int32_t heap_alloc_string(struct string *s);

void heap_describe_slot(int slot);

#ifdef VM_PRIVATE

extern uint32_t heap_next_seq;
extern int32_t *heap_free_stack;
extern size_t heap_free_ptr;

#endif /* VM_PRIVATE */
#endif /* SYSTEM4_HEAP_H */
