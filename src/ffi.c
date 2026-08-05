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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ffi.h>
#include "system4/ain.h"
#include "system4/utfsjis.h"
#include "system4/string.h"
#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "xsystem4.h"

#define HLL_MAX_ARGS 64

struct hll_function {
	void *fun;
	ffi_cif cif;
	unsigned int nr_args;
	ffi_type **args;
	ffi_type *return_type;
};

static struct hll_function **libraries = NULL;

// См. hll.h: число аргументов текущего HLL-вызова, чтобы реализация могла
// отличить свои перегрузки (они делят один C-указатель).
int hll_current_nr_args = 0;
struct ain_hll_function *hll_current_fn = NULL;

bool library_exists(int libno)
{
	return libraries[libno];
}

bool library_function_exists(int libno, int fno)
{
	return libraries[libno] && libraries[libno][fno].fun;
}

/*
 * Primitive HLL function tracing facility.
 */
//#define TRACE_HLL
#ifdef TRACE_HLL
#include "system4/string.h"

struct traced_library {
	const char *name;
	bool (*should_trace)(struct ain_hll_function *f, union vm_value **args);
};

static bool chipmunk_should_trace(struct ain_hll_function *f, union vm_value **args)
{
	const char *no_trace[] = {
		"SYSTEM_SetReadMessageSkipping",
		"Update",
		"KeepPreviousView",
		"Sleep",
	};
	for (unsigned i = 0; i < sizeof(no_trace) / sizeof(*no_trace); i++) {
		if (!strcmp(f->name, no_trace[i]))
			return false;
	}
	return true;
}

#include "parts.h"
#include "parts/parts_internal.h"
static bool gui_engine_should_trace(struct ain_hll_function *f, union vm_value **args)
{
	const char *no_trace[] = {
		"Parts_UpdateMotionTime",
		"UpdateInputState",
		"UpdateComponent",
		"UpdateParts",
		"GetMessageType",
		"ReleaseMessage",
		"Parts_SetPartsPixelDecide",
		"Parts_GetPartsShow",
		"Parts_IsCursorIn",
		"Parts_GetPartsAlpha",
	};
	for (unsigned i = 0; i < sizeof(no_trace) / sizeof(*no_trace); i++) {
		if (!strcmp(f->name, no_trace[i]))
			return false;
	}
	if (!strcmp(f->name, "Parts_SetPartsCG")) {
		static char *u = NULL;
		if (!u)
			u = utf2sjis("システム／ボタン／メニュー／通常", 0);
		struct string ***strs = (void*)args;
		struct string *s = *strs[1];
		return strcmp(s->text, u);
	}
	if (!strcmp(f->name, "Parts_SetPos")) {
		if (args[1]->i < 0 || args[2]->i < 0)
			return false;
		return PE_GetPartsX(args[0]->i) != args[1]->i || PE_GetPartsY(args[0]->i) != args[2]->i;
	}
	if (!strcmp(f->name, "Parts_SetZ"))
		return PE_GetPartsZ(args[0]->i) != args[1]->i;
	if (!strcmp(f->name, "Parts_SetShow"))
		return PE_GetPartsShow(args[0]->i) != args[1]->i;
	if (!strcmp(f->name, "Parts_SetAlpha"))
		return PE_GetPartsAlpha(args[0]->i) != args[1]->i;
	if (!strcmp(f->name, "Parts_SetPartsCGSurfaceArea")) {
		return false;
		struct parts *p = parts_try_get(args[0]->i);
		if (!p)
			return true;
		Rectangle *r = &p->states[args[5]->i].common.surface_area;
		return args[1]->i != r->x || args[2]->i != r->y
			|| args[3]->i != r->w || args[4]->i != r->h;
	}
	if (!strcmp(f->name, "Parts_SetAddColor")) {
		struct parts *p = parts_try_get(args[0]->i);
		if (!p)
			return true;
		SDL_Color *c = &p->local.add_color;
		return args[1]->i != c->r || args[2]->i != c->g || args[3]->i != c->b;
	}
	if (!strcmp(f->name, "Parts_GetPartsX"))
		return false;
	return true;
}

static bool parts_engine_should_trace(struct ain_hll_function *f, union vm_value **args)
{
	if (!strcmp(f->name, "Update"))
		return false;
	if (!strcmp(f->name, "SetAddColor"))
		return false;
	if (!strcmp(f->name, "SetAlpha"))
		return PE_GetPartsAlpha(args[0]->i) != args[1]->i;
	if (!strcmp(f->name, "SetPartsCGSurfaceArea"))
		return args[1]->i >= 0 && args[2]->i >= 0;
	if (!strcmp(f->name, "SetPartsConstructionSurfaceArea"))
		return args[1]->i >= 0 && args[2]->i >= 0;
	if (!strcmp(f->name, "SetPartsMagX"))
		return args[1]->f > 1.001 || args[1]->f < -1.001;
	if (!strcmp(f->name, "SetPartsMagY"))
		return args[1]->f > 1.001 || args[1]->f < -1.001;
	if (!strcmp(f->name, "SetPartsOriginPosMode"))
		return PE_GetPartsOriginPosMode(args[0]->i) != args[1]->i;
	if (!strcmp(f->name, "SetPartsRotateZ"))
		return args[1]->f > 0.001 || args[1]->f < -0.001;
	if (!strcmp(f->name, "SetPos"))
		return PE_GetPartsX(args[0]->i) != args[1]->i || PE_GetPartsX(args[0]->i) != args[2]->i;
	if (!strcmp(f->name, "SetShow")) {
		if (args[0]->i == 0)
			return false; // ???
		return PE_GetPartsShow(args[0]->i) != args[1]->i;
	}
	if (!strcmp(f->name, "SetZ"))
		return PE_GetPartsZ(args[0]->i) != args[1]->i;
	return true;
}

static bool sact_should_trace(struct ain_hll_function *f, union vm_value **args)
{
	const char *no_trace[] = {
		"Key_IsDown",
		"Mouse_ClearWheel",
		"Mouse_GetPos",
		"Mouse_GetWheel",
		"SP_GetWidth",
		"SP_GetHeight",
		"SP_ExistAlpha",
		"Update",
	};
	for (unsigned i = 0; i < sizeof(no_trace) / sizeof(*no_trace); i++) {
		if (!strcmp(f->name, no_trace[i]))
			return false;
	}
	return true;
}

struct traced_library traced_libraries[] = {
	{ "ChipmunkSpriteEngine", chipmunk_should_trace },
	{ "GUIEngine", gui_engine_should_trace },
	{ "PartsEngine", parts_engine_should_trace },
	{ "SACTDX", sact_should_trace },
};

static void print_hll_trace(struct ain_library *lib, struct ain_hll_function *f,
		      struct hll_function *fun, union vm_value *r, void *_args)
{
	sys_message("(%s) ", display_sjis0(ain->functions[call_stack[call_stack_ptr-1].fno].name));
	sys_message("%s.%s(", lib->name, f->name);
	union vm_value **args = _args;
	for (int i = 0; i < f->nr_arguments; i++) {
		if (i > 0) {
			sys_message(", ");
		}
		sys_message("%s=", display_sjis0(f->arguments[i].name));
		switch (f->arguments[i].type.data) {
		case AIN_INT:
		case AIN_LONG_INT:
			sys_message("%d", args[i]->i);
			break;
		case AIN_FLOAT:
			sys_message("%f", args[i]->f);
			break;
		case AIN_STRING: {
			struct string ***strs = _args;
			struct string *s = *strs[i];
			sys_message("\"%s\"", display_sjis0(s->text));
			break;
		}
		case AIN_BOOL:
			sys_message("%s", args[i]->i ? "true" : "false");
			break;
		case AIN_STRUCT:
			sys_message("<struct>");
			break;
		case AIN_ARRAY_TYPE:
			sys_message("<array>");
			break;
		case AIN_REF_TYPE:
			sys_message("<ref>");
			break;
		default:
			sys_message("<%d>", f->arguments[i].type.data);
			break;
		}
	}
	sys_message(")");

	ffi_call(&fun->cif, (void*)fun->fun, r, _args);

	switch (f->return_type.data) {
	case AIN_VOID:
		break;
	case AIN_INT:
		sys_message(" -> %d", r->i);
		break;
	case AIN_FLOAT:
		sys_message(" -> %f", r->f);
		break;
	case AIN_STRING:
		sys_message(" -> \"%s\"", display_sjis0(((struct string*)r->ref)->text));
		break;
	case AIN_BOOL:
		sys_message(" -> %s", r->i ? "true" : "false");
		break;
	default:
		sys_message(" -> <%d>", f->return_type.data);
		break;
	}
	for (int i = 0; i < f->nr_arguments; i++) {
		union vm_value ***args = _args;
		switch (f->arguments[i].type.data) {
		case AIN_REF_INT:
			sys_message(" (%s=%d)", display_sjis0(f->arguments[i].name), (*args[i])->i);
			break;
		case AIN_REF_FLOAT:
			sys_message(" (%s=%f)", display_sjis0(f->arguments[i].name), (*args[i])->f);
			break;
		default:
			break;
		}
	}
	sys_message("\n");
}

static void trace_hll_call(struct ain_library *lib, struct ain_hll_function *f,
		      struct hll_function *fun, union vm_value *r, void *_args)
{
	for (unsigned i = 0; i < sizeof(traced_libraries)/sizeof(*traced_libraries); i++) {
		struct traced_library *l = &traced_libraries[i];
		if (!strcmp(lib->name, l->name)) {
			if (!l->should_trace || l->should_trace(f, _args)) {
				print_hll_trace(lib, f, fun, r, _args);
				return;
			}
			break;
		}
	}

	ffi_call(&fun->cif, (void*)fun->fun, r, _args);
}
#endif /* TRACE_HLL */

// How many stack slots does a call site push for a generic (AIN_HLL_PARAM)
// argument? It cannot be derived from the .ain signature: the very same
// struct-element container is called both with a plain object handle (one slot)
// and with a wrap reference (two slots — the object's heap slot followed by the
// index within it). Ixseal's third CALLHLL operand tells the two apart. It packs
// two 16-bit class codes, the argument form and the element type, in which class
// 3 means "wrap reference"; the forms that occur in Dohna Dohna are
//
//   0x00001  int value                                1 slot
//   0x00002  string handle                            1 slot
//   0x10002  object handle into an object container    1 slot
//   0x10003  wrap<T> element type   (heap slot, 0)     2 slots
//   0x30002  wrap<T> argument form  (heap slot, 0)     2 slots
//
// Games whose CALLHLL has no third operand pass elem_class 0 and always push a
// single slot, so they keep the old behaviour.
/*
 * Живёт ли значение обёрнутого типа в СОБСТВЕННОМ heap-слоте. От этого зависит
 * форма ссылки на него — ровно как у обычных ref-типов движка: скаляр
 * адресуется парой (страница, номер переменной), всё остальное — одним слотом
 * с heap-индексом. Совпадает со списком двухслотовых AIN_REF_* в arg-цикле.
 */
static bool is_wrapped_object_type(enum ain_data_type t)
{
	switch (t) {
	case AIN_INT:
	case AIN_LONG_INT:
	case AIN_BOOL:
	case AIN_FLOAT:
	case AIN_ENUM:
		return false;
	default:
		return true;
	}
}

static int hll_param_slots(int elem_class)
{
	if ((elem_class & 0xffff) == 3 || (elem_class >> 16) == 3)
		return 2;
	return 1;
}

void hll_call(int libno, int fno, int elem_class)
{
	struct ain_hll_function *f = &ain->libraries[libno].functions[fno];

	{
		const char *flt = getenv("XSYS4_HLL_TRACE");
		if (flt && (!*flt || strstr(ain->libraries[libno].name, flt)))
			WARNING("HLLCALL lib%d:fn%d %s.%s", libno, fno, ain->libraries[libno].name, f->name);
	}

	if (!libraries[libno])
		VM_ERROR("Unimplemented HLL function: %s.%s", ain->libraries[libno].name, f->name);

	struct hll_function *fun = &libraries[libno][fno];
	bool lenient_noop = false;
	if (!fun->fun) {
		if (getenv("XSYS4_TRACE_X"))
			WARNING("HLLDIAG libno=%d fno=%d name=%s nr_func=%d",
				libno, fno, f->name, ain->libraries[libno].nr_functions);
		// Lenient mode: instead of aborting on an unimplemented HLL function,
		// pop its arguments and return a zero/empty value. Lets us push past a
		// cascade of missing functions to see how far the game gets.
		if (getenv("XSYS4_LENIENT_HLL")) {
			WARNING("LENIENT: stubbing %s.%s -> 0", ain->libraries[libno].name, f->name);
			lenient_noop = true;
		} else {
			VM_ERROR("Unimplemented HLL function: %s.%s", ain->libraries[libno].name, f->name);
		}
	}

	void *args[HLL_MAX_ARGS];
	void *ptrs[HLL_MAX_ARGS];
	// Copy reference arguments to the stack to protect against heap
	// reallocation during HLL calls.
	void *heap_ptrs[HLL_MAX_ARGS];
	int heap_slots[HLL_MAX_ARGS];
	// Ixseal-лямбда (AIN_HLL_FUNC) — пара (страница объекта, номер функции).
	// Держим КОПИЮ, а не указатель в стек VM: реализация зовёт VM обратно, и
	// та затирает слоты стека ниже stack_ptr.
	union vm_value func_pairs[HLL_MAX_ARGS][2];

	if (getenv("XSYS4_HLL_TRACE"))
		NOTICE("HLL %s.%s", ain->libraries[libno].name, f->name);

	int ref_array_slot = -1;  // heap slot of the AIN_REF_ARRAY arg, for ref-element returns
	int dbg_sp0 = stack_ptr;
	bool dbg_arr = getenv("XSYS4_ARR_TRACE") && !strcmp(ain->libraries[libno].name, "Array");
	if (dbg_arr) {
		NOTICE("ARR %s nr_args=%d sp=%d [%d %d %d %d]", f->name, f->nr_arguments, stack_ptr,
			stack[stack_ptr-1].i, stack[stack_ptr-2].i, stack[stack_ptr-3].i, stack[stack_ptr-4].i);
		for (int k = 0; k < f->nr_arguments; k++)
			NOTICE("  arg[%d] type=%d", k, f->arguments[k].type.data);
	}

	// Diagnostic (XSYS4_HLLP_TRACE) for new generic-argument forms: locate the
	// container argument under both slot counts and report which one actually
	// lands on an array page, next to the element-class operand that decided it.
	if (getenv("XSYS4_HLLP_TRACE")) {
		int g = -1;
		for (int i = 0; i < f->nr_arguments; i++) {
			if (f->arguments[i].type.data == AIN_HLL_PARAM)
				g = i;
		}
		if (g > 0 && f->arguments[0].type.data == AIN_REF_ARRAY) {
			int after = 0;
			for (int i = g + 1; i < f->nr_arguments; i++) {
				int t = f->arguments[i].type.data;
				after += (t == AIN_REF_INT || t == AIN_REF_LONG_INT
					  || t == AIN_REF_BOOL || t == AIN_REF_FLOAT) ? 2 : 1;
			}
			int base = stack_ptr - after - 1;
			int c1 = base - 1 >= 0 ? stack[base - 1].i : -1;
			int c2 = base - 2 >= 0 ? stack[base - 2].i : -1;
			#define PROBE_IS_ARRAY(x) (heap_index_valid(x) && heap[x].page \
					&& heap[x].page->type == ARRAY_PAGE)
			NOTICE("HLLP %s.%s elem_class=0x%x chose=%d slots1=%s(a_type=%d) slots2=%s(a_type=%d)",
			       ain->libraries[libno].name, f->name, elem_class,
			       hll_param_slots(elem_class),
			       PROBE_IS_ARRAY(c1) ? "ARRAY" : "no",
			       PROBE_IS_ARRAY(c1) ? heap[c1].page->index : -1,
			       PROBE_IS_ARRAY(c2) ? "ARRAY" : "no",
			       PROBE_IS_ARRAY(c2) ? heap[c2].page->index : -1);
			#undef PROBE_IS_ARRAY
		}
	}

	for (int i = f->nr_arguments - 1; i >= 0; i--) {
		switch (f->arguments[i].type.data) {
		case AIN_REF_INT:
		case AIN_REF_LONG_INT:
		case AIN_REF_BOOL:
		case AIN_REF_FLOAT: {
			// need to create pointer for immediate ref types
			stack_ptr -= 2;
			int pageno = stack[stack_ptr].i;
			int varno  = stack[stack_ptr+1].i;
			ptrs[i] = &heap[pageno].page->values[varno];
			args[i] = &ptrs[i];
			break;
		}
		case AIN_STRING:
			stack_ptr--;
			args[i] = &heap[stack[stack_ptr].i].s;
			break;
		case AIN_REF_STRING:
			stack_ptr--;
			heap_slots[i] = stack[stack_ptr].i;
			heap_ptrs[i] = heap[stack[stack_ptr].i].s;
			ptrs[i] = &heap_ptrs[i];
			args[i] = &ptrs[i];
			break;
		case AIN_STRUCT:
		case AIN_ARRAY_TYPE:
			stack_ptr--;
			args[i] = &heap[stack[stack_ptr].i].page;
			break;
		case AIN_WRAP:
			// Форма wrap-АРГУМЕНТА зависит от обёрнутого типа — ровно так же,
			// как у wrap-ВОЗВРАТА (см. материализацию ссылки ниже):
			//  • wrap<скаляр> (int/float/bool/…) — ссылка на ПЕРЕМЕННУЮ из ДВУХ
			//    слотов (страница, номер переменной). Это out-параметр, callee
			//    ждёт обычный указатель на значение: `TextSurfaceManager.
			//    GetFontWidth(string, wrap<int> Width, …)` сайт кладёт
			//    `PUSHLOCALPAGE; PUSH <локал>` без X_REF.
			//  • wrap<объект> (массив/структура/строка) — ОДИН слот с heap-
			//    индексом страницы (напр. источник Array.Duplicate); идёт ниже.
			// Раньше двухслотовая форма разбирала аргументы со сдвигом: строка
			// получала слот локальной страницы, и следующий X_REF падал.
			if (f->arguments[i].type.array_type
					&& !is_wrapped_object_type(f->arguments[i].type.array_type->data)) {
				stack_ptr -= 2;
				int pageno = stack[stack_ptr].i;
				int varno  = stack[stack_ptr+1].i;
				ptrs[i] = (heap_index_valid(pageno) && heap[pageno].page)
					? &heap[pageno].page->values[varno] : NULL;
				args[i] = &ptrs[i];
				break;
			}
			/* fallthrough */
		case AIN_REF_STRUCT:
		case AIN_REF_ARRAY:
		case AIN_REF_ARRAY_TYPE:
		// Ixseal: `ref delegate` — тоже страница по ссылке, один слот с heap-
		// индексом (сайт кладёт `X_REF 1`). Обязательно с обратной записью:
		// delegate_append() ПЕРЕВЫДЕЛЯЕТ страницу. Раньше тип 67 уходил в
		// `default` и callee получал адрес слота стека вместо страницы.
		// Аргументов типа 67 у v6/v7 нет вообще (0 против 9 у Dohna — все в
		// библиотеке `Delegate`), так что ветка структурно гейтится.
		case AIN_REF_DELEGATE:
			// Ixseal's generic array-by-reference (AIN_REF_ARRAY) is passed the
			// same way as a struct/typed-array ref: a single stack slot holding
			// the array page's heap index (verified by stack-balance tracing —
			// consuming two slots underflows the caller's stack).
			// AIN_WRAP arguments (wrap<array<T>>, e.g. the four arrays of
			// PartsEngine.AddPartsConstructionProcess or Array.Duplicate's
			// source) are pushed the same way — the call site reads the member
			// with X_REF 1. They used to fall through to the default case, which
			// handed the callee the address of the stack slot instead of the page
			// (a garbage pointer).
			stack_ptr--;
			heap_slots[i] = stack[stack_ptr].i;
			heap_ptrs[i] = heap_index_valid(stack[stack_ptr].i) ? heap[stack[stack_ptr].i].page : NULL;
			ptrs[i] = &heap_ptrs[i];
			args[i] = &ptrs[i];
			if (f->arguments[i].type.data == AIN_REF_ARRAY)
				ref_array_slot = heap_slots[i];
			break;
		case AIN_HLL_PARAM: {
			// Generic container element: pass a pointer to the raw stack slot;
			// the callee interprets it per the array's element type. A wrap
			// reference occupies two slots — the object's heap slot plus the
			// index within it — and the value the callee wants is the lower one.
			// Двухслотовая форма — значение wrap<интерфейс>: (heap-слот
			// объекта, база интерфейса). Оба слота лежат на стеке подряд, и
			// callee (Array.PushBack/Insert) читает их как value[0], value[1].
			int slots = hll_param_slots(elem_class);
			stack_ptr -= slots;
			ptrs[i] = &stack[stack_ptr];
			args[i] = &ptrs[i];
			break;
		}
		case AIN_HLL_FUNC:
			// Ixseal: лямбда/предикат приходит ДВУМЯ слотами — (страница
			// объекта, номер функции): сайт кладёт `PUSHSTRUCTPAGE; PUSH <fno>`
			// (см. Array.EraseAll/Any/Where/Sort — 20 функций, ~740 сайтов).
			// Реализация вызовет VM обратно, поэтому пару КОПИРУЕМ: указатель
			// в стек VM затёрся бы аргументами вложенного вызова.
			// Старые игры аргументов типа 95 не имеют вообще (0 у v6/v7),
			// поэтому ветка структурно гейтится.
			stack_ptr -= 2;
			func_pairs[i][0] = stack[stack_ptr];
			func_pairs[i][1] = stack[stack_ptr + 1];
			ptrs[i] = func_pairs[i];
			args[i] = &ptrs[i];
			break;
		default:
			stack_ptr--;
			args[i] = &stack[stack_ptr];
			break;
		}
	}

	if (dbg_arr)
		NOTICE("ARR %s consumed %d slots (sp %d->%d)", f->name, dbg_sp0 - stack_ptr, dbg_sp0, stack_ptr);

	union vm_value r;
	if (lenient_noop) {
		r.i = 0;
		if (f->return_type.data == AIN_STRING)
			r.ref = string_ref(&EMPTY_STRING);
	} else {
		// Сообщаем реализации её перегрузку (см. hll_current_nr_args в hll.h).
		// Сохраняем/восстанавливаем: HLL-функция может вызвать VM обратно,
		// и вложенный HLL-вызов затрёт значение.
		int saved_nr_args = hll_current_nr_args;
		struct ain_hll_function *saved_fn = hll_current_fn;
		hll_current_nr_args = f->nr_arguments;
		hll_current_fn = f;
#ifdef TRACE_HLL
		trace_hll_call(&ain->libraries[libno], f, fun, &r, args);
#else
		ffi_call(&fun->cif, (void*)fun->fun, &r, args);
#endif
		hll_current_nr_args = saved_nr_args;
		hll_current_fn = saved_fn;
	}


	for (int i = 0, j = 0; i < f->nr_arguments; i++, j++) {
		// XXX: We don't increase the ref count when passing ref arguments to HLL
		//      functions, so we need to avoid decreasing it via variable_fini
		switch (f->arguments[i].type.data) {
		case AIN_REF_INT:
		case AIN_REF_LONG_INT:
		case AIN_REF_BOOL:
		case AIN_REF_FLOAT:
			j++;
			break;
		case AIN_REF_STRING:
			heap[heap_slots[i]].s = heap_ptrs[i];
			break;
		case AIN_REF_STRUCT:
		case AIN_REF_ARRAY:
		case AIN_REF_ARRAY_TYPE:
		case AIN_REF_DELEGATE:
			// Write the (possibly reallocated) array/struct/delegate page back
			// to its slot.
			if (heap_index_valid(heap_slots[i]))
				heap[heap_slots[i]].page = heap_ptrs[i];
			break;
		case AIN_WRAP:
			// Двухслотовый wrap<скаляр> занял на стеке две ячейки — сдвигаем
			// счётчик слотов. Обратная запись не нужна: callee получил
			// указатель прямо в страницу.
			if (f->arguments[i].type.array_type
					&& !is_wrapped_object_type(f->arguments[i].type.array_type->data)) {
				j++;
				break;
			}
			// wrap<объект> маршалится как страница по ссылке (см. ветку
			// AIN_REF_ARRAY выше), и callee вправе её ПЕРЕВЫДЕЛИТЬ: `String.
			// SearchAll(self, wrap<array<string>> matchList, regex)` дописывает
			// найденные токены через array_pushback_n. Без обратной записи
			// heap-слот оставался бы с указателем на освобождённую страницу →
			// double free при разборе кадра (`Motion::Parser@SplitParams`).
			// Раньше это не всплывало: все wrap<объект>-аргументы были
			// ТОЛЬКО источниками (Array.Duplicate/Copy/Concat).
			if (heap_index_valid(heap_slots[i]))
				heap[heap_slots[i]].page = heap_ptrs[i];
			break;
		case AIN_REF_FUNC_TYPE:
		case AIN_HLL_FUNC:
			break;
		case AIN_HLL_PARAM:
			// Generic value was passed by pointer without a refcount bump; the
			// callee copies it into the container, so nothing to finalize here.
			break;
		case AIN_ARRAY_TYPE:
			// Sys41VM doesn't make a copy when passing an array by value.
			if (ain->version <= 1)
				break;
			// fallthrough
		default:
			variable_fini(stack[stack_ptr+j], f->arguments[i].type.data, false);
			break;
		}
	}

	int slot;
	switch (f->return_type.data) {
	case AIN_VOID:
		break;
	case AIN_STRING:
		slot = heap_alloc_slot(VM_STRING);
		heap[slot].s = r.ref;
		stack_push(slot);
		break;
	case AIN_BOOL:
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
		stack_push(*(bool*)&r);
#pragma GCC diagnostic pop
		break;
	case AIN_REF_HLL_PARAM:
	case AIN_WRAP: {
		// A reference to a generic array element (Ixseal At/EmplaceBack/First/
		// Last/... returning t75 or t82/WRAP). The callee returned the element
		// index in r.i. The concrete on-stack reference depends on the element
		// type: a heap-object element (struct/string/array/delegate) is itself a
		// reference, so we push its own 1-value page slot; a scalar element
		// (int/float/bool/...) needs a 2-value ref (array-page slot, index).
		// Only applies when the call actually had a ref-array self argument;
		// otherwise fall through to the plain single-value return.
		struct page *ap = (ref_array_slot >= 0 && heap_index_valid(ref_array_slot))
			? heap[ref_array_slot].page : NULL;
		if (!ap) {
			stack_push(r);
			break;
		}
		int idx = r.i;
		// Массив с элементом wrap<интерфейс>: элемент занимает ДВА слота
		// страницы, поэтому ссылка на него — это всегда пара
		// (слот массива, idx*2); «своего» heap-слота у такого элемента нет.
		int eslots = array_elem_slots(ap);
		if (eslots != 1) {
			if (heap_index_valid(ref_array_slot))
				heap_ref(ref_array_slot);
			stack_push(ref_array_slot);
			stack_push(idx >= 0 ? idx * eslots : idx);
			break;
		}
		// Тип элемента берём из САМОГО МАССИВА, а не из слота по индексу.
		// Индекс бывает -1 (At/First/Find/Min/... ничего не нашли), и тогда
		// прежний `variable_type(ap, idx)` давал AIN_INT: строковый элемент
		// возвращался 2-слотовой скалярной ссылкой вместо 1-слотовой, и стек
		// ВЫЗЫВАЮЩЕГО съезжал на слот. Так `Array.At(list, 1)` на массиве из
		// одного элемента ломал следующий `S_ASSIGN` в
		// `Motion::ParamAnalyzer@0` (`Out of bounds page index: 0/555`).
		// Для валидного индекса результат тот же: variable_type() у ARRAY_PAGE
		// и так возвращает array_type(a_type).
		enum ain_data_type et = array_type(ap->a_type);
		// Классификация элемента: «объект» (собственный heap-слот, 1-значный
		// ref) — это struct/string/delegate/iface и ВЛОЖЕННЫЙ массив (типизир.
		// AIN_ARRAY_* или generic AIN_ARRAY/REF_ARRAY/WRAP/...). Всё остальное
		// (int/float/BOOL/long_int/enum) — СКАЛЯР → 2-значный ref [array_slot, idx].
		// ВАЖНО: нельзя использовать диапазон et∈[AIN_ARRAY_INT..AIN_ARRAY_DELEGATE]
		// — он захватывает скаляры AIN_BOOL(47)/AIN_LONG_INT(55) (bool-массив
		// createdFlagList в CASTimerManager давал 1-знач. ref вместо 2 → порча стека).
		bool elem_is_object;
		switch (et) {
		case AIN_STRUCT:
		case AIN_STRING:
		case AIN_DELEGATE:
		case AIN_IFACE:
		case AIN_ARRAY_TYPE:      // типизированные вложенные массивы
			elem_is_object = true;
			break;
		default:
			elem_is_object = ain_is_array_data_type(et); // generic вложенные массивы
			break;
		}
		/*
		 * У ПУСТОГО массива типа элемента ещё НЕТ: страница создаётся
		 * `variable_initval(AIN_ARRAY)` и её `a_type` остаётся заглушкой
		 * AIN_ARRAY_INT(14), т.е. `et` выше — не факт, а догадка «int» → скаляр
		 * → 2-слотовая ссылка. Но САЙТ знает настоящий класс элемента и
		 * объявляет его третьим операндом CALLHLL (та же кодировка, что у
		 * `hll_param_slots`): 1=int, 2=string, 0x10002=объект, 0x10003=wrap<
		 * интерфейс>. Поэтому объявление сайта важнее догадки по странице.
		 *
		 * Проверено по всем сайтам Dohna: там, где тип массива РЕАЛЬНО известен,
		 * elem_class и `et` всегда согласованы (a_type=50/et=47 bool → 0x1;
		 * a_type=17/et=13 struct и a_type=16/et=12 string → 0x2), так что
		 * приоритет ничего не меняет. Расходятся они ровно на пустом массиве:
		 * `Array.At(arg, 0)` в `Motion::ParamAnalyzer@AnalyzeTime` (idx=-1,
		 * elem_class=0x10002) получал 2-слотовую ссылку вместо 1-слотовой, и
		 * каждый такой вызов оставлял на стеке лишний слот. Два вызова подряд
		 * давали AnalyzeTime +2 (XSYS4_SP_CHECK), из-за чего у ВЫЗЫВАЮЩЕГО
		 * `Motion::ParamCollection@Parse` ссылка `mp` уезжала на 2 слота и
		 * X_ASSIGN бил по чужой странице.
		 */
		switch (elem_class) {
		case 0x00001:               // int — скаляр, ссылка (страница, индекс)
			elem_is_object = false;
			break;
		case 0x00002:               // string (и struct — тот же 1-слотовый хэндл)
		case 0x10002:               // объект
			elem_is_object = true;
			break;
		case 0x10003:
			// wrap<интерфейс> — элемент 2-слотовый, его отдаёт ветка
			// `eslots != 1` выше. Сюда можно попасть только у ПУСТОГО массива
			// (stride ещё не выставлен); форма для этого случая по байткоду не
			// установлена — сообщаем и оставляем прежнее поведение.
			WARNING("Array.%s: elem_class=wrap<интерфейс> на массиве без stride "
				"(a_type=%d) — форма ссылки не установлена", f->name, ap->a_type);
			break;
		default:
			break;              // 0 — старые игры без третьего операнда
		}
		if (getenv("XSYS4_ELEMFORM_TRACE"))
			WARNING("ELEMFORM libno=%d fn=%s a_type=%d et=%d eslots=%d idx=%d "
				"elem_class=0x%x -> %s", libno, f->name, ap->a_type, et, eslots,
				idx, elem_class, elem_is_object ? "объект(1)" : "скаляр(2)");
		if (elem_is_object && idx < 0) {
			// Элемента нет: отдаём null-ссылку одним слотом (форма та же, что
			// у найденного объектного элемента, — иначе стек съедет).
			stack_push(-1);
		} else if (elem_is_object) {
			// The reference is the element's own heap slot; the caller owns it
			// and releases it with DELETE, so hand out a counted reference.
			int es = ap->values[idx].i;
			heap_ref(es);
			stack_push(es);
		} else {
			// Scalar element: a (array-page slot, index) reference. A reference
			// to an array element keeps the backing array alive, so bump its
			// ref count — the caller balances this with a DELETE on the ref.
			// This holds even for an out-of-range (index == -1) result: the
			// caller still stores and later DELETEs the reference, unref-ing the
			// array slot, so it must own a count regardless of the index.
			if (heap_index_valid(ref_array_slot))
				heap_ref(ref_array_slot);
			stack_push(ref_array_slot);
			stack_push(idx);
		}
		break;
	}
	default:
		stack_push(r);
		break;
	}
}

extern struct static_library lib_ACXLoader;
extern struct static_library lib_ACXLoaderP2;
extern struct static_library lib_ADVSYS;
extern struct static_library lib_AFAFactory;
extern struct static_library lib_AliceLogo;
extern struct static_library lib_AliceLogo2;
extern struct static_library lib_AliceLogo3;
extern struct static_library lib_AliceLogo4;
extern struct static_library lib_AliceLogo5;
extern struct static_library lib_AnteaterADVEngine;
extern struct static_library lib_Array;
extern struct static_library lib_BanMisc;
extern struct static_library lib_Bitarray;
extern struct static_library lib_CalcTable;
extern struct static_library lib_CGManager;
extern struct static_library lib_ChipmunkSpriteEngine;
extern struct static_library lib_ChrLoader;
extern struct static_library lib_CommonSystemData;
extern struct static_library lib_Confirm;
extern struct static_library lib_Confirm2;
extern struct static_library lib_Confirm3;
extern struct static_library lib_CrayfishLogViewer;
extern struct static_library lib_Cursor;
extern struct static_library lib_DALKDemo;
extern struct static_library lib_DALKEDemo;
extern struct static_library lib_Data;
extern struct static_library lib_DataFile;
extern struct static_library lib_Delegate;
extern struct static_library lib_Discord;
extern struct static_library lib_DrawDungeon;
extern struct static_library lib_DrawDungeon2;
extern struct static_library lib_DrawDungeon14;
extern struct static_library lib_DrawEffect;
extern struct static_library lib_DrawField;
extern struct static_library lib_DrawGraph;
extern struct static_library lib_DrawMovie;
extern struct static_library lib_DrawMovie2;
extern struct static_library lib_DrawMovie3;
extern struct static_library lib_DrawNumeral;
extern struct static_library lib_DrawPluginManager;
extern struct static_library lib_DrawRain;
extern struct static_library lib_DrawRipple;
extern struct static_library lib_DrawSimpleText;
extern struct static_library lib_DrawSnow;
extern struct static_library lib_File;
extern struct static_library lib_File2;
extern struct static_library lib_FileOperation;
extern struct static_library lib_FillAngle;
extern struct static_library lib_GoatGUIEngine;
extern struct static_library lib_Gpx2Plus;
extern struct static_library lib_GUIEngine;
extern struct static_library lib_HTTPDownloader;
extern struct static_library lib_IbisInputEngine;
extern struct static_library lib_InputDevice;
extern struct static_library lib_InputString;
extern struct static_library lib_KiwiSoundEngine;
extern struct static_library lib_LoadCG;
extern struct static_library lib_MADLoader;
extern struct static_library lib_MainEXFile;
extern struct static_library lib_MainSurface;
extern struct static_library lib_MamanyoDemo;
extern struct static_library lib_MamanyoSDemo;
extern struct static_library lib_MarmotModelEngine;
extern struct static_library lib_Math;
extern struct static_library lib_MapLoader;
extern struct static_library lib_MenuMsg;
extern struct static_library lib_MonsterInfo;
extern struct static_library lib_MsgLogManager;
extern struct static_library lib_MsgLogViewer;
extern struct static_library lib_MsgSkip;
extern struct static_library lib_MusicSystem;
extern struct static_library lib_NewFont;
extern struct static_library lib_OutputLog;
extern struct static_library lib_P3MapSprite;
extern struct static_library lib_P3SquareSprite;
extern struct static_library lib_PassRegister;
extern struct static_library lib_PastelChime2;
extern struct static_library lib_PartsEngine;
extern struct static_library lib_PixelRestore;
extern struct static_library lib_PlayDemo;
extern struct static_library lib_PlayMovie;
extern struct static_library lib_ReignEngine;
extern struct static_library lib_SACT2;
extern struct static_library lib_SACTDX;
extern struct static_library lib_SealEngine;
extern struct static_library lib_SengokuRanceFont;
extern struct static_library lib_Sound2ex;
extern struct static_library lib_SoundFilePlayer;
extern struct static_library lib_String;
extern struct static_library lib_StoatSpriteEngine;
extern struct static_library lib_StretchHelper;
extern struct static_library lib_system;
extern struct static_library lib_SystemService;
extern struct static_library lib_Sys43VM;
extern struct static_library lib_TextFile;
extern struct static_library lib_SystemServiceEx;
extern struct static_library lib_TextSurfaceManager;
extern struct static_library lib_TapirEngine;
extern struct static_library lib_Timer;
extern struct static_library lib_Toushin3Loader;
extern struct static_library lib_vmAnime;
extern struct static_library lib_vmArray;
extern struct static_library lib_vmCG;
extern struct static_library lib_vmChrLoader;
extern struct static_library lib_vmCursor;
extern struct static_library lib_vmDalkGaiden;
extern struct static_library lib_vmData;
extern struct static_library lib_vmDialog;
extern struct static_library lib_vmDrawGauge;
extern struct static_library lib_vmDrawMsg;
extern struct static_library lib_vmDrawNumber;
extern struct static_library lib_vmFile;
extern struct static_library lib_vmGraph;
extern struct static_library lib_vmGraphQuake;
extern struct static_library lib_vmKey;
extern struct static_library lib_vmMapLoader;
extern struct static_library lib_vmMsgLog;
extern struct static_library lib_vmMsgSkip;
extern struct static_library lib_vmMusic;
extern struct static_library lib_vmSound;
extern struct static_library lib_vmSprite;
extern struct static_library lib_vmString;
extern struct static_library lib_vmSurface;
extern struct static_library lib_vmSystem;
extern struct static_library lib_vmTimer;
extern struct static_library lib_ValueEncryption;
extern struct static_library lib_VSFile;

static struct static_library *static_libraries[] = {
	&lib_ACXLoader,
	&lib_ACXLoaderP2,
	&lib_ADVSYS,
	&lib_AFAFactory,
	&lib_AliceLogo,
	&lib_AliceLogo2,
	&lib_AliceLogo3,
	&lib_AliceLogo4,
	&lib_AliceLogo5,
	&lib_AnteaterADVEngine,
	&lib_Array,
	&lib_BanMisc,
	&lib_Bitarray,
	&lib_CalcTable,
	&lib_CGManager,
	&lib_ChipmunkSpriteEngine,
	&lib_ChrLoader,
	&lib_CommonSystemData,
	&lib_Confirm,
	&lib_Confirm2,
	&lib_Confirm3,
	&lib_CrayfishLogViewer,
	&lib_Cursor,
	&lib_DALKDemo,
	&lib_DALKEDemo,
	&lib_Data,
	&lib_DataFile,
	&lib_Delegate,
	&lib_Discord,
	&lib_DrawDungeon,
	&lib_DrawDungeon2,
	&lib_DrawDungeon14,
	&lib_DrawEffect,
	&lib_DrawField,
	&lib_DrawGraph,
	&lib_DrawMovie,
	&lib_DrawMovie2,
	&lib_DrawMovie3,
	&lib_DrawNumeral,
	&lib_DrawPluginManager,
	&lib_DrawRain,
	&lib_DrawRipple,
	&lib_DrawSimpleText,
	&lib_DrawSnow,
	&lib_File,
	&lib_File2,
	&lib_FileOperation,
	&lib_FillAngle,
	&lib_GoatGUIEngine,
	&lib_Gpx2Plus,
	&lib_GUIEngine,
	&lib_HTTPDownloader,
	&lib_IbisInputEngine,
	&lib_InputDevice,
	&lib_InputString,
	&lib_KiwiSoundEngine,
	&lib_LoadCG,
	&lib_MADLoader,
	&lib_MainEXFile,
	&lib_MainSurface,
	&lib_MamanyoDemo,
	&lib_MamanyoSDemo,
	&lib_MarmotModelEngine,
	&lib_Math,
	&lib_MapLoader,
	&lib_MenuMsg,
	&lib_MonsterInfo,
	&lib_MsgLogManager,
	&lib_MsgLogViewer,
	&lib_MsgSkip,
	&lib_MusicSystem,
	&lib_NewFont,
	&lib_OutputLog,
	&lib_P3MapSprite,
	&lib_P3SquareSprite,
	&lib_PassRegister,
	&lib_PastelChime2,
	&lib_PartsEngine,
	&lib_PixelRestore,
	&lib_PlayDemo,
	&lib_PlayMovie,
	&lib_ReignEngine,
	&lib_SACT2,
	&lib_SACTDX,
	&lib_SealEngine,
	&lib_SengokuRanceFont,
	&lib_Sound2ex,
	&lib_SoundFilePlayer,
	&lib_StoatSpriteEngine,
	&lib_StretchHelper,
	&lib_String,
	&lib_system,
	&lib_SystemService,
	&lib_Sys43VM,
	&lib_TextFile,
	&lib_SystemServiceEx,
	&lib_TextSurfaceManager,
	&lib_TapirEngine,
	&lib_Timer,
	&lib_Toushin3Loader,
	&lib_vmAnime,
	&lib_vmArray,
	&lib_vmCG,
	&lib_vmChrLoader,
	&lib_vmCursor,
	&lib_vmDalkGaiden,
	&lib_vmData,
	&lib_vmDialog,
	&lib_vmDrawGauge,
	&lib_vmDrawMsg,
	&lib_vmDrawNumber,
	&lib_vmFile,
	&lib_vmGraph,
	&lib_vmGraphQuake,
	&lib_vmKey,
	&lib_vmMapLoader,
	&lib_vmMsgLog,
	&lib_vmMsgSkip,
	&lib_vmMusic,
	&lib_vmSound,
	&lib_vmSprite,
	&lib_vmString,
	&lib_vmSurface,
	&lib_vmSystem,
	&lib_vmTimer,
	&lib_ValueEncryption,
	&lib_VSFile,
	NULL
};

static ffi_type *ain_to_ffi_type(enum ain_data_type type)
{
	switch (type) {
	case AIN_VOID:
		return &ffi_type_void;
	case AIN_INT:
	case AIN_BOOL:
		return &ffi_type_sint32;
	case AIN_LONG_INT:
		return &ffi_type_sint64;
	case AIN_FLOAT:
		return &ffi_type_float;
	case AIN_STRING:
	case AIN_STRUCT:
	case AIN_FUNC_TYPE:
	case AIN_DELEGATE:
	case AIN_ARRAY_TYPE:
	case AIN_REF_TYPE:
	case AIN_IMAIN_SYSTEM: // ???
	// page-based типы новых System 4 (Healing Touch и др.): в FFI — указатель.
	// generic-массив AIN_ARRAY не входит в макрос AIN_ARRAY_TYPE (AIN_REF_ARRAY — в AIN_REF_TYPE).
	case AIN_ARRAY:
	case AIN_WRAP:
	case AIN_OPTION:
	case AIN_UNKNOWN_TYPE_87:
	case AIN_IFACE_WRAP:
		return &ffi_type_pointer;
	// Ixseal generic-container types (Array HLL): a generic element value is
	// passed as a pointer to its stack slot (interpreted per the array's element
	// type by the callee); an HLL function reference is passed as its integer
	// function index.
	case AIN_HLL_PARAM:
		return &ffi_type_pointer;
	// Ixseal-лямбда: реализация получает УКАЗАТЕЛЬ на пару (страница, fno) —
	// см. AIN_HLL_FUNC в hll_call. Как тип ВОЗВРАТА тип 95 не встречается ни в
	// одной библиотеке ни одной из игр (проверено ainliball), так что смена
	// ffi-типа затрагивает только аргументы.
	case AIN_HLL_FUNC:
		return &ffi_type_pointer;
	// A reference to a generic element (e.g. Array.At's return) is represented
	// as the element's own value/heap-slot — a single integer slot.
	case AIN_REF_HLL_PARAM:
		return &ffi_type_sint32;
	default:
		ERROR("Unhandled type in HLL function: %s", ain_strtype(ain, type, -1));
	}
}

static void link_static_library_function(struct hll_function *dst, struct ain_hll_function *src, void *funcptr)
{
	dst->fun = funcptr;
	dst->nr_args = src->nr_arguments;
	dst->args = xcalloc(dst->nr_args, sizeof(ffi_type*));

	for (unsigned int i = 0; i < dst->nr_args; i++) {
		dst->args[i] = ain_to_ffi_type(src->arguments[i].type.data);
	}
	dst->return_type = ain_to_ffi_type(src->return_type.data);

	if (ffi_prep_cif(&dst->cif, FFI_DEFAULT_ABI, dst->nr_args, dst->return_type, dst->args) != FFI_OK)
		ERROR("Failed to link HLL function");
}

static void *static_library_lookup(struct static_library *lib, const char *name)
{
	for (int j = 0; lib->functions[j].name; j++) {
		if (!strcmp(name, lib->functions[j].name))
			return lib->functions[j].fun;
	}
	return NULL;
}

/*
 * "Link" a library that has been compiled into the xsystem4 executable.
 */
static struct hll_function *link_static_library(struct ain_library *ainlib, struct static_library *lib)
{
	struct hll_function *dst = xcalloc(ainlib->nr_functions, sizeof(struct hll_function));

	for (int i = 0; i < ainlib->nr_functions; i++) {
		struct ain_hll_function *f = &ainlib->functions[i];
		void *fun = NULL;
		// Перегрузка по АРНОСТИ (см. HLL_EXPORT_N в hll.h): сначала пробуем имя,
		// декорированное числом аргументов — `Имя@<n>`. Нужно там, где у перегрузок
		// разъезжаются ПОЗИЦИИ параметров и одной C-функцией их не обслужить
		// (cif строится по .ain): Ixseal-овские четыре `Array.Copy`.
		{
			char decorated[256];
			snprintf(decorated, sizeof(decorated), "%s@%d", f->name, f->nr_arguments);
			fun = static_library_lookup(lib, decorated);
		}
		// Перегрузка по ТИПУ (см. HLL_EXPORT_F в hll.h): для функции,
		// возвращающей float, сначала пробуем декорированное имя `Имя@f`.
		// Если библиотека такого не экспортирует — обычное имя, как раньше.
		if (!fun && f->return_type.data == AIN_FLOAT) {
			char decorated[256];
			snprintf(decorated, sizeof(decorated), "%s@f", f->name);
			fun = static_library_lookup(lib, decorated);
		}
		if (!fun)
			fun = static_library_lookup(lib, f->name);
		if (fun)
			link_static_library_function(&dst[i], f, fun);
		if (!dst[i].fun) {
			if (getenv("XSYS4_LIST_UNIMPL"))
				WARNING("UNIMPL: %s.%s", ainlib->name, ainlib->functions[i].name);
		}
		else if (ainlib->functions[i].nr_arguments >= HLL_MAX_ARGS)
			ERROR("Too many arguments to library function: %s", ainlib->functions[i].name);
	}

	return dst;
}

static void library_run(struct static_library *lib, const char *name)
{
	for (int i = 0; lib->functions[i].name; i++) {
		if (!strcmp(lib->functions[i].name, name)) {
			((void(*)(void))lib->functions[i].fun)();
			break;
		}
	}
}

static void library_run_all(const char *name)
{
	for (int i = 0; i < ain->nr_libraries; i++) {
		for (int j = 0; static_libraries[j]; j++) {
			if (!strcmp(ain->libraries[i].name, static_libraries[j]->name)) {
				library_run(static_libraries[j], name);
				break;
			}
		}
	}
}

static void link_libraries(void)
{
	if (libraries)
		return;

	libraries = xcalloc(ain->nr_libraries, sizeof(struct hll_function*));

	for (int i = 0; i < ain->nr_libraries; i++) {
		for (int j = 0; static_libraries[j]; j++) {
			if (!strcmp(ain->libraries[i].name, static_libraries[j]->name)) {
				libraries[i] = link_static_library(&ain->libraries[i], static_libraries[j]);
				break;
			}
		}
		if (!libraries[i])
			WARNING("Unimplemented library: %s", ain->libraries[i].name);
	}
}

bool libraries_initialized = false;

void init_libraries(void)
{
	library_run_all("_PreLink");
	link_libraries();
	library_run_all("_ModuleInit");
	libraries_initialized = true;
}

void exit_libraries(void)
{
	if (libraries_initialized)
		library_run_all("_ModuleFini");
}

void static_library_replace(struct static_library *lib, const char *name, void *fun)
{
	for (int i = 0; lib->functions[i].name; i++) {
		if (!strcmp(lib->functions[i].name, name)) {
			lib->functions[i].fun = fun;
			return;
		}
	}
	ERROR("No library function '%s.%s'", lib->name, name);
}
