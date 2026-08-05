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

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <setjmp.h>
#include <signal.h>
#include <assert.h>
#include <SDL.h> // for system.MsgBox

#include "system4.h"
#include "system4/ain.h"
#include "system4/instructions.h"
#include "system4/file.h"
#include "system4/little_endian.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "android_bridge.h"
#include "debugger.h"
#include "input.h"
#include "savedata.h"
#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "xsystem4.h"

static inline int32_t lint_clamp(int64_t n)
{
	if (n < 0)
		return 0;
	if (n > INT32_MAX)
		return INT32_MAX;
	return (int32_t)n;
}

#define INITIAL_STACK_SIZE 4096

// When the IP is set to VM_RETURN, the VM halts
#define VM_RETURN 0xFFFFFFFF

/*
 * NOTE: The current implementation is a simple bytecode interpreter.
 *       System40.exe uses a JIT compiler, and we should too.
 */

// The stack
union vm_value *stack = NULL; // the stack
int32_t stack_ptr = 0;        // pointer to the top of the stack
static size_t stack_size;     // current size of the stack

// Stack of function call frames
struct function_call call_stack[4096];
int32_t call_stack_ptr = 0;

struct ain *ain;
size_t instr_ptr = 0;

bool vm_reset_once = false;

// Diagnostic: trace entry into specific function indices, set via
// XSYS4_FN_TRACE=<comma-separated fn numbers>. Env-gated, harmless.
static int fn_trace_list[32];
static int fn_trace_count = -1;
// Diagnostic: XSYS4_SP_CHECK — проверять баланс стека на каждом RETURN.
static bool sp_check = false;
static uint8_t sp_check_reported[65536];

/*
 * Сколько слотов стека занимает значение объявленного типа (Ixseal). Для
 * ПЕРЕМЕННЫХ это же число видно по филлерам <void> (decl_slots), но у типа
 * возврата филлеров нет — считаем по структуре типа:
 *   wrap<интерфейс> (82 → 100) — два слота (объект, база интерфейса);
 *   option<T>                  — слоты T плюс тег.
 */
static int type_slots(struct ain_type *t)
{
	if (!t || t->data == AIN_VOID)
		return 0;
	// Ссылка на интерфейс — пара (объект, база интерфейса). Подтверждено тем же
	// правилом филлеров: локал типа 89 всегда идёт с одним `<void>` за ним
	// (напр. var[0]/var[1] в `Motion::ParamAnalyzer@Parse`).
	if (t->data == AIN_IFACE || t->data == AIN_IFACE_WRAP)
		return 2;
	if ((t->data == AIN_WRAP || t->data == AIN_OPTION) && t->array_type) {
		int inner = t->array_type->data == AIN_IFACE_WRAP
			? 2 : type_slots(t->array_type);
		return t->data == AIN_OPTION ? inner + 1 : inner;
	}
	return 1;
}
// Namespace-filtered call trace: XSYS4_FN_TRACE_NS=<substr> logs every entered
// function whose name contains <substr>. Env-gated, harmless.
static const char *fn_trace_ns = (const char *)1; // 1 = not-yet-resolved
static void vm_fn_trace_ns(int fno)
{
	if (fn_trace_ns == (const char *)1)
		fn_trace_ns = getenv("XSYS4_FN_TRACE_NS");
	if (!fn_trace_ns || !*fn_trace_ns)
		return;
	if (fno < 0 || fno >= ain->nr_functions)
		return;
	const char *n = ain->functions[fno].name;
	if (n && strstr(n, fn_trace_ns))
		WARNING("NSTRACE fn %d %s", fno, n);
}
static void vm_fn_trace(int fno, const char *via)
{
	if (fn_trace_count < 0) {
		fn_trace_count = 0;
		const char *e = getenv("XSYS4_FN_TRACE");
		while (e && *e && fn_trace_count < 32) {
			fn_trace_list[fn_trace_count++] = (int)strtol(e, (char**)&e, 0);
			while (*e == ',' || *e == ' ') e++;
		}
	}
	for (int i = 0; i < fn_trace_count; i++) {
		if (fn_trace_list[i] == fno) {
			WARNING("FNTRACE %s -> fn %d (%s)", via, fno,
				(fno >= 0 && fno < ain->nr_functions) ? ain->functions[fno].name : "?");
			return;
		}
	}
}

// Read the opcode at ADDR.
static int16_t get_opcode(size_t addr)
{
	return LittleEndian_getW(ain->code, addr);
}

static const char *current_instruction_name(void)
{
	int16_t opcode = get_opcode(instr_ptr);
	if (opcode >= 0 && opcode < NR_OPCODES)
		return instructions[opcode].name;
	return "UNKNOWN OPCODE";
}

static int local_page_slot(void)
{
	return call_stack[call_stack_ptr-1].page_slot;
}

struct page *local_page(void)
{
	return heap[local_page_slot()].page;
}

struct page *get_local_page(int frame_no)
{
	if (frame_no < 0 || frame_no >= call_stack_ptr)
		return NULL;
	int slot = call_stack[call_stack_ptr - (frame_no + 1)].page_slot;
	return slot < 1 ? NULL : heap[slot].page;
}

union vm_value local_get(int varno)
{
	return local_page()->values[varno];
}

static void local_set(int varno, int32_t value)
{
	local_page()->values[varno].i = value;
}

static union vm_value *local_ptr(int varno)
{
	return local_page()->values + varno;
}

struct page *global_page(void)
{
	return heap[0].page;
}

union vm_value global_get(int varno)
{
	return heap[0].page->values[varno];
}

void global_set(int varno, union vm_value val, bool call_dtors)
{
	switch (ain->globals[varno].type.data) {
	case AIN_STRING:
	case AIN_STRUCT:
	case AIN_ARRAY_TYPE:
		if (heap[0].page->values[varno].i > 0) {
			if (call_dtors)
				heap_unref(heap[0].page->values[varno].i);
			else
				exit_unref(heap[0].page->values[varno].i);
		}
	default:
		break;
	}
	heap[0].page->values[varno] = val;
}

static int32_t struct_page_slot(void)
{
	return call_stack[call_stack_ptr-1].struct_page;
}

static struct page *struct_page(void)
{
	return heap[struct_page_slot()].page;
}

struct page *get_struct_page(int frame_no)
{
	if (frame_no < 0 || frame_no >= call_stack_ptr)
		return NULL;
	int slot = call_stack[call_stack_ptr - (frame_no + 1)].struct_page;
	return slot < 1 ? NULL : heap[slot].page;
}

// Ixseal closures: a lambda captures the LOCAL variables of its lexically
// enclosing function. The delegate it was created from only stores (obj, fun)
// — obj is the enclosing method's `this`, NOT the captured environment. Since
// these lambdas are invoked synchronously by a framework trampoline (e.g.
// OptionalExtensions::Match) while the enclosing frame is still live, the
// captured environment is that frame's local page. We locate it by resolving
// the lambda's lexically-enclosing function (encoded in the mangled name as
// `...<lambda : PARENT(args)(line, col)>`) and walking down the call stack for
// the nearest matching frame.
// Extract the lexically-enclosing function's name from a lambda's mangled name,
// or NULL if `name` is not a lambda. Returns a malloc'd string.
//
// The payload of the outer `<lambda : ...>` is `PARENT(args)(line, col)`, and
// PARENT may ITSELF be a lambda name, so the form nests:
//   `Foo@<lambda : Foo@<lambda : Foo@Bar(string)(160, 23)>(int)(160, 48)>`
// Cutting at the first `(` therefore loses the parent of every nested lambda
// (221 of Dohna's 4031) — they silently fell back to the `this` page. Take the
// payload with `<`/`>` balancing, then strip the two trailing parenthesised
// groups (the argument list and the line/column pair).
static char *lambda_parent_name(const char *name)
{
	const char *lm = name ? strstr(name, "<lambda : ") : NULL;
	if (!lm)
		return NULL;
	const char *begin = lm + strlen("<lambda : ");
	const char *end = begin;
	for (int depth = 1; *end; end++) {
		if (*end == '<') {
			depth++;
		} else if (*end == '>') {
			if (--depth == 0)
				break;
		}
	}
	if (*end != '>' || end == begin)
		return NULL;
	for (int i = 0; i < 2; i++) {
		if (end - begin < 2 || end[-1] != ')')
			return NULL;
		int depth = 0;
		do {
			end--;
			if (*end == ')')
				depth++;
			else if (*end == '(')
				depth--;
		} while (depth > 0 && end > begin);
		if (depth != 0)
			return NULL;
	}
	if (end == begin)
		return NULL;
	size_t len = end - begin;
	char *pname = xmalloc(len + 1);
	memcpy(pname, begin, len);
	pname[len] = '\0';
	return pname;
}

static int lambda_parent_fno(int fno)
{
	static int *cache = NULL; // -1 = uncomputed, -2 = not a lambda / no parent
	if (fno < 0 || fno >= ain->nr_functions)
		return -2;
	if (!cache) {
		cache = xmalloc(sizeof(int) * ain->nr_functions);
		for (int i = 0; i < ain->nr_functions; i++)
			cache[i] = -1;
	}
	if (cache[fno] != -1)
		return cache[fno];

	int result = -2;
	char *pname = lambda_parent_name(ain->functions[fno].name);
	if (pname) {
		// The parent name is NOT unique: OVERLOADS share it, and taking the
		// first match by index picks the wrong one for 153 of Dohna's lambdas.
		// A lambda's body is emitted INSIDE its enclosing function's body, so
		// among same-named candidates the parent is the one with the greatest
		// address BELOW the lambda's own. (Checked over all 4031 lambdas: the
		// 153 ambiguous names all belong to regular functions and all have a
		// candidate below; where the parent is itself a lambda the name is
		// unique — it carries a line/column — and its body may well sit AFTER
		// the child's, so the address rule must not apply there.)
		int32_t self = ain->functions[fno].address;
		int32_t best = 0;
		for (int i = 0; i < ain->nr_functions; i++) {
			if (i == fno || !ain->functions[i].name
					|| strcmp(pname, ain->functions[i].name))
				continue;
			int32_t addr = ain->functions[i].address;
			if (result < 0) {
				result = i;
				best = addr;
			} else if (addr < self && (best >= self || addr > best)) {
				result = i;
				best = addr;
			}
		}
		free(pname);
	}
	cache[fno] = result;
	return result;
}

// Return the heap slot of the local page that holds a lambda's captured
// environment, or -1 if it cannot be located on the current call stack.
static int lambda_env_page_slot(void)
{
	int cur = call_stack[call_stack_ptr-1].fno;
	int parent = lambda_parent_fno(cur);
	if (parent >= 0) {
		for (int i = call_stack_ptr - 2; i >= 0; i--) {
			if (call_stack[i].fno == parent)
				return call_stack[i].page_slot;
		}
	}
	return -1;
}

union vm_value member_get(int varno)
{
	return struct_page()->values[varno];
}

static void member_set(int varno, int32_t value)
{
	struct_page()->values[varno].i = value;
}

union vm_value stack_peek(int n)
{
	return stack[stack_ptr - (1 + n)];
}

union vm_value stack_pop(void)
{
	stack_ptr--;
	return stack[stack_ptr];
}

static union vm_value *stack_peek_ptr(int n)
{
	return &stack[stack_ptr - (1 + n)];
}

// Pop a reference off the stack, returning the address of the referenced object.
static union vm_value *stack_pop_var(void)
{
	int32_t page_index = stack_pop().i;
	int32_t heap_index = stack_pop().i;
	if (unlikely(!heap_index_valid(heap_index)))
		VM_ERROR("Out of bounds heap index: %d/%d", heap_index, page_index);
	if (unlikely(!heap[heap_index].page || page_index >= heap[heap_index].page->nr_vars))
		VM_ERROR("Out of bounds page index: %d/%d", heap_index, page_index);
	return &heap[heap_index].page->values[page_index];
}

union vm_value *stack_peek_var(void)
{
	int32_t page_index = stack_peek(0).i;
	int32_t heap_index = stack_peek(1).i;
	if (unlikely(!heap_index_valid(heap_index)))
		VM_ERROR("Out of bounds heap index: %d/%d", heap_index, page_index);
	if (unlikely(!heap[heap_index].page || page_index >= heap[heap_index].page->nr_vars))
		VM_ERROR("Out of bounds page index: %d/%d", heap_index, page_index);
	return &heap[heap_index].page->values[page_index];
}

static void stack_push_string(struct string *s)
{
	int32_t heap_slot = heap_alloc_slot(VM_STRING);
	heap[heap_slot].s = s;
	stack[stack_ptr++].i = heap_slot;
}

static struct string *stack_peek_string(int n)
{
	return heap[stack_peek(n).i].s;
}

int vm_string_ref(struct string *s)
{
	int slot = heap_alloc_slot(VM_STRING);
	heap[slot].s = string_ref(s);
	return slot;
}

int vm_copy_page(struct page *page)
{
	int slot = heap_alloc_slot(VM_PAGE);
	heap_set_page(slot, copy_page(page));
	return slot;
}

union vm_value vm_copy(union vm_value v, enum ain_data_type type)
{
	switch (type) {
	case AIN_STRING:
		return (union vm_value) { .i = vm_string_ref(heap_get_string(v.i)) };
	case AIN_STRUCT:
	case AIN_DELEGATE:
	case AIN_ARRAY_TYPE:
	// Ixseal generic array (type 79): deep-copy the backing page like a typed
	// array. Without this it fell to the shallow default below, so a struct
	// copy shared its array members' heap slots with the original — destroying
	// either one then freed the shared slot, leaving the other dangling.
	case AIN_ARRAY:
		return (union vm_value) { .i = vm_copy_page(heap_get_page(v.i)) };
	case AIN_REF_TYPE:
		heap_ref(v.i);
		return v;
	default:
		return v;
	}
}

static int get_function_by_name(const char *name)
{
	for (int i = 0; i < ain->nr_functions; i++) {
		if (!strcmp(name, ain->functions[i].name))
			return i;
	}
	return -1;
}

// Check whether a function's signature is compatible with a delegate type.
static bool function_matches_delegate(int dg_no, int fno)
{
	if (dg_no < 0 || dg_no >= ain->nr_delegates)
		return false;
	if (fno < 0 || fno >= ain->nr_functions)
		return false;
	struct ain_function_type *dg = &ain->delegates[dg_no];
	struct ain_function *f = &ain->functions[fno];

	if (dg->return_type.data != f->return_type.data ||
	    dg->return_type.struc != f->return_type.struc)
		return false;
	if (dg->nr_arguments != f->nr_args)
		return false;
	for (int i = 0; i < dg->nr_arguments; i++) {
		struct ain_type *a = &dg->variables[i].type;
		struct ain_type *b = &f->vars[i].type;
		if (a->data != b->data || a->struc != b->struc || a->rank != b->rank)
			return false;
	}
	return true;
}

static int scenario_label_addr(const char *lname)
{
	for (int i = 0; i < ain->nr_scenario_labels; i++) {
		if (!strcmp(ain->scenario_labels[i].name, lname)) {
			return ain->scenario_labels[i].address;
		}
	}
	VM_ERROR("Invalid scenario label: %s", display_sjis0(lname));
}

static int alloc_scenario_page(const char *fname)
{
	int fno, slot;
	struct ain_function *f;

	if ((fno = get_function_by_name(fname)) < 0)
		VM_ERROR("Invalid scenario function: %s", display_sjis0(fname));
	f = &ain->functions[fno];

	slot = heap_alloc_slot(VM_PAGE);
	heap_set_page(slot, alloc_page(LOCAL_PAGE, fno, f->nr_vars));
	for (int i = 0; i < f->nr_vars; i++) {
		heap[slot].page->values[i] = variable_initval_var(heap[slot].page, i, f->vars[i].type.data);
	}
	init_option_vars(heap[slot].page, f->vars, f->nr_vars, 0);
	return slot;
}

void vm_optrace_dump(void);

static void set_struct_page(int slot)
{
	static bool bad_ssp_logged = false;
	if (slot >= 0 && !heap_index_valid(slot) && getenv("XSYS4_OPTRACE") && !bad_ssp_logged) {
		bad_ssp_logged = true;
		uint16_t op = instr_ptr < ain->code_size ? get_opcode(instr_ptr) : 0;
		sys_warning("BAD set_struct_page(%d) ip=0x%06lx op=%s csp=%d\n", slot,
			    (unsigned long)instr_ptr, instructions[op].name ? instructions[op].name : "?", call_stack_ptr);
		vm_optrace_dump();
	}
	call_stack[call_stack_ptr-1].struct_page = slot;
	// Keep `this` alive during the call (from Rance9 onwards).
	if (AIN_VERSION_GTE(ain, 6, 1))
		heap_ref(slot);
}

static void unref_call_frame(struct function_call *frame)
{
	if (frame->struct_page >= 0 && AIN_VERSION_GTE(ain, 6, 1))
		heap_unref(frame->struct_page);
	heap_unref(frame->page_slot);
}

static void scenario_jump(int address)
{
	// flush call stack
	for (int i = call_stack_ptr - 1; i >= 0; i--) {
		unref_call_frame(&call_stack[i]);
	}
	call_stack_ptr = 0;
	instr_ptr = address;
}

static void scenario_call(int slot)
{
	int fno = heap[slot].page->index;
	// flush call stack
	for (int i = call_stack_ptr - 1; i >= 0; i--) {
		unref_call_frame(&call_stack[i]);
	}
	call_stack[0] = (struct function_call) {
		.fno = fno,
		.call_address = instr_ptr,
		.return_address = VM_RETURN,
		.page_slot = slot,
		.struct_page = -1,
	};
	call_stack_ptr = 1;
	instr_ptr = ain->functions[fno].address;
}

/*
 * System 4 calling convention:
 *   - caller pushes arguments, in order
 *   - CALLFUNC creates stack frame, pops arguments into local page
 *   - callee pushes return value on the stack
 *   - RETURN jumps to return address (saved in stack frame)
 */
static int _function_call(int fno, int return_address)
{
	struct ain_function *f = &ain->functions[fno];
	int slot = heap_alloc_slot(VM_PAGE);
	heap_set_page(slot, alloc_page(LOCAL_PAGE, fno, f->nr_vars));
	heap[slot].page->local.struct_ptr = -1;

	call_stack[call_stack_ptr++] = (struct function_call) {
		.fno = fno,
		.call_address = instr_ptr,
		.return_address = return_address,
		.page_slot = slot,
		.struct_page = -1,
		// уточняется после снятия аргументов/ресивера (см. function_call/method_call)
		.entry_sp = stack_ptr,
	};
	// initialize local variables
	for (int i = f->nr_args; i < f->nr_vars; i++) {
		heap[slot].page->values[i] = variable_initval_var(heap[slot].page, i, f->vars[i].type.data);
		if (ain->version <= 1 && f->vars[i].type.data == AIN_STRUCT) {
			create_struct(f->vars[i].type.struc, &heap[slot].page->values[i]);
		}
	}
	// Ixseal: локальные option'ы — пустые (аргументы приходят со стека, их не трогаем)
	init_option_vars(heap[slot].page, f->vars, f->nr_vars, f->nr_args);
	// jump to function start
	instr_ptr = ain->functions[fno].address;

	return slot;
}

/*
 * Daiteikoku: режим «бесконечные события» (тумблер в android_bridge).
 *
 * Фаза действий игрока — это сцена событий: App@run делает switch(context.scene),
 * где case 9 -> App@sceneEvent (меню 行動一覧), case 22 -> App@sceneEnemyPhase.
 * Штатно выбор любого события уводит в фазу врага: App@sceneEvent проигрывает
 * событие (EventCaller@call), применяет последствия (GFD_Event_EventPhaseResult)
 * и вызывает App@changeScene(22). Кнопка «Конец фазы» вызывает changeScene(22)
 * напрямую, без GFD_Event_EventPhaseResult.
 *
 * В режиме бесконечных событий на ветке «событие проиграно» подменяем аргумент
 * changeScene 22 -> 9: игра остаётся в сцене событий и, поскольку объект
 * SceneEvent она к этому моменту уже удалила (context[10] = -1), на следующем
 * кадре меню перестраивается со свежим списком. Ветку «Конец фазы» и аварийный
 * выход (App@sceneEvent при context[62] != 0) не трогаем: там нет вызова
 * GFD_Event_EventPhaseResult, поэтому флаг pending не взводится.
 *
 * Все вызовы движка проходят через function_call, поэтому хук здесь один.
 */
#define DTK_SCENE_EVENT 9
#define DTK_SCENE_ENEMY 22

static void infinite_events_hook(int fno)
{
	static int f_change = -2, f_scene = -2, f_result = -2;
	static bool pending = false;
	if (!bridge_infinite_events_enabled()) { pending = false; return; }
	if (f_change == -2) {
		f_change = ain_get_function(ain, (char *)"App@changeScene");
		f_scene  = ain_get_function(ain, (char *)"App@sceneEvent");
		f_result = ain_get_function(ain, (char *)"GFD_Event_EventPhaseResult");
	}
	if (f_change < 0 || f_scene < 0 || f_result < 0)
		return;  // не Daiteikoku (или другая сборка .ain) — режим неприменим
	if (fno == f_scene) { pending = false; return; }  // новый кадр сцены событий
	if (call_stack_ptr < 1 || call_stack[call_stack_ptr - 1].fno != f_scene)
		return;  // интересуют только прямые вызовы из App@sceneEvent
	if (fno == f_result) {
		pending = true;  // событие проиграно в этом кадре
	} else if (fno == f_change && pending) {
		pending = false;
		// на вершине стека — аргумент сцены (22); держим игрока в сцене событий
		if (stack_ptr > 0 && stack[stack_ptr - 1].i == DTK_SCENE_ENEMY)
			stack[stack_ptr - 1].i = DTK_SCENE_EVENT;
	}
}

static void function_call(int fno, int return_address)
{
	infinite_events_hook(fno);
	vm_fn_trace_ns(fno);
	int slot = _function_call(fno, return_address);

	// pop arguments, store in local page
	struct ain_function *f = &ain->functions[fno];
	for (int i = f->nr_args - 1; i >= 0; i--) {
		heap[slot].page->values[i] = stack_pop();
		switch (f->vars[i].type.data) {
		case AIN_REF_TYPE:
			heap_ref(heap[slot].page->values[i].i);
			break;
		default:
			break;
		}
	}
	call_stack[call_stack_ptr-1].entry_sp = stack_ptr;
	if (fn_trace_count != 0) {
		for (int i = 0; i < fn_trace_count; i++) {
			if (fn_trace_list[i] == fno) {
				WARNING("FNARGS fn %d nargs=%d a0=%d a1=%d a2=%d", fno, f->nr_args,
					f->nr_args>0?heap[slot].page->values[0].i:-1,
					f->nr_args>1?heap[slot].page->values[1].i:-1,
					f->nr_args>2?heap[slot].page->values[2].i:-1);
				break;
			}
		}
	}
}

static void method_call(int fno, int return_address)
{
	function_call(fno, return_address);
	int struct_page = stack_pop().i;
	call_stack[call_stack_ptr-1].entry_sp = stack_ptr;
	set_struct_page(struct_page);
	heap[call_stack[call_stack_ptr-1].page_slot].page->local.struct_ptr = struct_page;
}

static void vm_execute(void);

// Number of stack slots a delegate return value occupies — ровно та же мера,
// что у возврата обычной функции, поэтому считаем её тем же `type_slots()`
// (это и инвариант XSYS4_SP_CHECK). Ixseal возвращает из делегатов и
// многослотовые значения: `option<T>` = слоты T плюс тег, `AIN_IFACE`(89) и
// `wrap<интерфейс>`(100) = пара (объект, база интерфейса). Раньше здесь стояла
// частная таблица (void→0, AIN_OPTION→2, иначе 1): она верна для void, скаляров
// и всех 62 `option<delegate>`-делегатов Dohna, но недосчитывала 6 делегатов с
// возвратом `AIN_IFACE` и 18 с `wrap<интерфейс>`.
// Ошибка здесь сдвигает peek-смещения dg_page/dg_index в цикле делегата и
// протаскивает лишний слот вверх по цепочке вызовов: на втором витке DG_CALL
// вместо страницы делегата читался уже инкрементированный dg_index
// (`Not a delegate page: 1` в `ArrayExtensions::Select<ref Motion::IArgument,
// string>`), а до того так же портился ref далеко в конструкторе CSpriteParts@0.
// Старые игры (v6/v7) многослотовых возвратов у делегатов не имеют вовсе, т.е.
// гейт структурный — по самому типу возврата.
// Самый широкий возврат, встречающийся у Ixseal: `option<wrap<интерфейс>>` = 3.
#define DG_MAX_RETURN_SLOTS 4

static int dg_return_slots(int dg_no)
{
	return type_slots(&ain->delegates[dg_no].return_type);
}

// Сколько слотов занимает ЗНАЧЕНИЕ внутри Ixseal-`option` (без тега). Операнд
// X_OP_SET — «класс элемента», та же кодировка, что у 3-го операнда CALLHLL:
// 1=int, 2=string, 0x10002=объект-хэндл — по одному слоту; 0x10003=wrap<интерфейс>
// — два слота (объект, база интерфейса). Других значений в .ain Dohna/HT нет.
static int x_option_value_slots(int elem_class)
{
	switch (elem_class) {
	case 0x00001:
	case 0x00002:
	case 0x10002:
		return 1;
	case 0x10003:
		return 2;
	default:
		WARNING("X_OP_SET: неизвестный класс элемента 0x%x — считаю значение однослотовым",
			elem_class);
		return 1;
	}
}

/*
 * Владеет ли значение этого типа heap-слотом (нужен ли ему счётчик ссылок).
 * Набор ровно тот, который освобождает variable_fini(): если добавить сюда
 * скаляр, heap_ref тронет чужой слот кучи.
 */
static bool slot_owns_heap_ref(enum ain_data_type t)
{
	switch (t) {
	case AIN_STRING:
	case AIN_STRUCT:
	case AIN_DELEGATE:
	case AIN_ARRAY_TYPE:
	case AIN_ARRAY:
	case AIN_REF_TYPE:
	case AIN_IFACE:
		return true;
	default:
		return ain_is_array_data_type(t);
	}
}

// Является ли значение option'а ссылкой на heap-слот (нужен ли учёт ссылок).
// Целые (класс 1) — нет; строки, объекты и wrap<интерфейс> — да (у wrap считается
// нижний слот, верхний — целочисленная база интерфейса).
static bool x_option_class_is_ref(int elem_class)
{
	return elem_class != 0x00001;
}

static void delegate_call(int dg_no, int return_address)
{
	if (dg_no < 0 || dg_no >= ain->nr_delegates)
		VM_ERROR("Invalid delegate index");

	// stack: [arg0, ..., dg_page, dg_index, [return_value(s)]]
	int return_values = dg_return_slots(dg_no);
	int dg_page = stack_peek(1 + return_values).i;
	int dg_index = stack_peek(0 + return_values).i;
	int obj, fun;
	if (delegate_get(heap_get_delegate_page(dg_page), dg_index, &obj, &fun)) {
		if (fn_trace_count != 0)
			vm_fn_trace(fun, "DG_CALL");
		vm_fn_trace_ns(fun);
		if (getenv("XSYS4_DG_TRACE"))
			WARNING("DGALL dg_no=%d idx=%d -> fn %d (%s)", dg_no, dg_index, fun,
				(fun >= 0 && fun < ain->nr_functions) ? ain->functions[fun].name : "?");
		// pop previous return value(s) (2 slots for an AIN_OPTION delegate)
		for (int i = 0; i < return_values; i++)
			stack_pop();
		// increment dg_index
		stack[stack_ptr - 1].i++;

		int slot = _function_call(fun, instr_ptr + instruction_width(DG_CALL));

		// copy arguments into local page
		struct ain_function_type *dg = &ain->delegates[dg_no];
		for (int i = 0; i < dg->nr_arguments; i++) {
			union vm_value arg = stack_peek((dg->nr_arguments + 1) - i);
			heap[slot].page->values[i] = vm_copy(arg, dg->variables[i].type.data);
		}

		set_struct_page(obj);
	} else {
		// call finished: clean up stack and jump to return address
		// Слотов у возврата столько, сколько скажет type_slots(): 2 у
		// `option<T>`/`AIN_IFACE`/`wrap<интерфейс>`, 3 у `option<wrap<интерфейс>>`.
		union vm_value r[DG_MAX_RETURN_SLOTS];
		if (return_values > DG_MAX_RETURN_SLOTS)
			VM_ERROR("Delegate return value too wide: %d slots", return_values);
		for (int i = return_values - 1; i >= 0; i--)
			r[i] = stack_pop();
		stack_pop(); // dg_index
		stack_pop(); // dg_page
		for (int i = ain->delegates[dg_no].nr_variables - 1; i >= 0; i--) {
			union vm_value v = stack_pop();
			enum ain_data_type type = ain->delegates[dg_no].variables[i].type.data;
			switch (type) {
			case AIN_REF_TYPE:
				break;
			default:
				variable_fini(v, type, true);
				break;
			}
		}
		for (int i = 0; i < return_values; i++)
			stack_push(r[i]);
		instr_ptr = get_argument(1);
	}
}

/*
 * Сколько слотов-аргументов объявлено у игровой функции. Для многослотового
 * аргумента (напр. `wrap<интерфейс>`) компилятор объявляет и филлеры `<void>`,
 * поэтому nr_args уже равен числу СЛОТОВ, которые нужно положить на стек.
 */
int vm_hll_func_nr_args(int fno)
{
	if (fno < 0 || fno >= ain->nr_functions)
		return -1;
	return ain->functions[fno].nr_args;
}

/*
 * Синхронно вызвать игровую лямбду из реализации HLL-функции — механизм
 * Ixseal-предикатов и компараторов (`Array.EraseAll/Any/Where/Sort/...`, тип
 * аргумента AIN_HLL_FUNC). Сайт кладёт лямбду ДВУМЯ слотами — (страница
 * объекта, номер функции), — они приходят сюда в `fn`.
 *
 * `argv`/`argc` — уже готовые слоты аргументов; argc обязан совпадать с
 * `vm_hll_func_nr_args(fno)`. Порядок на стеке для метода: [ресивер, арг0..],
 * т.е. ресивер ЛЕЖИТ ПОД аргументами (method_call сначала снимает аргументы,
 * потом ресивер) — поэтому обычный vm_call() здесь не годится, он умеет только
 * методы без аргументов.
 *
 * Возвращается первый слот результата (предикат/компаратор отдают bool).
 */
union vm_value vm_call_hll_func(const union vm_value *fn, const union vm_value *argv, int argc)
{
	union vm_value ret = { .i = 0 };
	if (!fn)
		return ret;
	int obj = fn[0].i;
	int fno = fn[1].i;
	int want = vm_hll_func_nr_args(fno);
	if (want < 0) {
		WARNING("vm_call_hll_func: неверный номер функции %d", fno);
		return ret;
	}
	if (want != argc) {
		WARNING("vm_call_hll_func: fn %d объявляет %d слотов аргументов, передано %d",
			fno, want, argc);
		return ret;
	}

	// Кадр заполняем сами, как delegate_call: аргументы должны попасть в локалы
	// КОПИЯМИ (`vm_copy`). Иначе объектное значение (строка, массив, структура)
	// уходит в лямбду по «сырому» слоту, кадр лямбды при возврате освобождает
	// его как свой, и элемент контейнера остаётся висячим: `Array.EraseAll` над
	// `array<string>` в `Motion::Parser@SplitParams` так убивал строки, которыми
	// владел массив (слот переиспользовался под страницу → бесконечная рекурсия
	// в delete_page). Через function_call/method_call сделать это нельзя — они
	// снимают аргументы со стека как есть.
	size_t saved_ip = instr_ptr;
	int slot = _function_call(fno, VM_RETURN);
	struct ain_function *f = &ain->functions[fno];
	for (int i = 0; i < argc; i++)
		heap[slot].page->values[i] = vm_copy(argv[i], f->vars[i].type.data);
	if (heap_index_valid(obj) && heap[obj].page) {
		set_struct_page(obj);
		heap[slot].page->local.struct_ptr = obj;
	}
	vm_execute();
	instr_ptr = saved_ip;

	if (f->return_type.data != AIN_VOID)
		ret = stack_pop();
	return ret;
}

void vm_call(int fno, int struct_page)
{
	size_t saved_ip = instr_ptr;
	if (struct_page < 0) {
		function_call(fno, VM_RETURN);
	} else {
		stack_push(struct_page);
		method_call(fno, VM_RETURN);
	}
	vm_execute();
	instr_ptr = saved_ip;
}

static void function_return(void)
{
	if (fn_trace_count > 0) {
		int rfno = call_stack[call_stack_ptr-1].fno;
		for (int i = 0; i < fn_trace_count; i++) {
			if (fn_trace_list[i] == rfno) {
				WARNING("FNTRACE RETURN fn %d -> top=%d", rfno,
					stack_ptr > 0 ? stack[stack_ptr-1].i : -999);
				break;
			}
		}
	}
	// XSYS4_SP_CHECK: функция обязана вернуться со стеком «кадр + возвращаемые
	// слоты». Лишний слот сам по себе не падает — он смещает ссылки у ВЫЗЫВАЮЩЕГО,
	// и краш случается далеко от причины (так дважды: S_ASSIGN и X_ASSIGN 0).
	// Печатаем ПЕРВОЕ расхождение по каждой функции, чтобы не залить лог.
	if (sp_check) {
		struct function_call *fc = &call_stack[call_stack_ptr-1];
		struct ain_function *f = &ain->functions[fc->fno];
		int expect = fc->entry_sp + type_slots(&f->return_type);
		if (stack_ptr != expect && !sp_check_reported[fc->fno & 0xffff]) {
			sp_check_reported[fc->fno & 0xffff] = 1;
			WARNING("SPCHECK fn %d (%s) вернулась sp=%d, ожидалось %d (разница %+d)",
				fc->fno, display_sjis0(f->name), stack_ptr, expect,
				stack_ptr - expect);
		}
	}
	unref_call_frame(&call_stack[call_stack_ptr-1]);
	instr_ptr = call_stack[call_stack_ptr-1].return_address;
	call_stack_ptr--;
}

static const SDL_MessageBoxButtonData ok_cancel_buttons[] = {
	{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "OK" },
	{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
};

static const SDL_MessageBoxButtonData stop_continue_buttons[] = {
	{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Stop" },
	{ 0, 0, "Continue" },
};

static struct string *get_func_stack_name(int index)
{
	int i = call_stack_ptr - (1 + index);
	if (i < 0 || i >= call_stack_ptr) {
		return string_ref(&EMPTY_STRING);
	}
	struct function_call *call = &call_stack[i];
	struct ain_function *fun = &ain->functions[call->fno];
	return cstr_to_string(fun->name);
}

static void system_call(enum syscall_code code)
{
	switch (code) {
	case SYS_EXIT: {// system.Exit(int nResult)
		vm_exit(stack_pop().i);
		break;
	}
	case SYS_GLOBAL_SAVE: { // system.GlobalSave(string szKeyName, string szFileName)
		int filename = stack_pop().i;
		int keyname = stack_pop().i;
		stack_push(save_globals(heap_get_string(keyname)->text, heap_get_string(filename)->text, NULL, NULL));
		heap_unref(filename);
		heap_unref(keyname);
		break;
	}
	case SYS_GLOBAL_LOAD: { // system.GlobalLoad(string szKeyName, string szFileName)
		int filename = stack_pop().i;
		int keyname = stack_pop().i;
		stack_push(load_globals(heap_get_string(keyname)->text, heap_get_string(filename)->text, NULL, NULL));
		heap_unref(filename);
		heap_unref(keyname);
		break;
	}
	case SYS_LOCK_PEEK: // system.LockPeek(void)
	case SYS_UNLOCK_PEEK: {// system.UnlockPeek(void)
		stack_push(1);
		break;
	}
	case SYS_RESET: {
		vm_reset();
		break;
	}
	case SYS_OUTPUT: {// system.Output(string szText)
		struct string *str = stack_peek_string(0);
		log_message("stdout", "%s", display_sjis0(str->text));
		// XXX: caller S_POPs
		break;
	}
	case SYS_MSGBOX: {
		struct string *str = stack_peek_string(0);
		char *utf = sjis2utf(str->text, str->size);
		// Test mode: skip the native modal (it blocks the loop and renders on the
		// default display, bypassing a virtual X server) — just log the text.
		if (getenv("XSYS4_AUTO_MSGBOX"))
			NOTICE("AUTO_MSGBOX: '%s'", utf);
		else
			SDL_ShowSimpleMessageBox(0, "xsystem4", utf, NULL);
		free(utf);
		// XXX: caller S_POPs
		break;
	}
	case SYS_MSGBOX_OK_CANCEL: {
		int result = 0;
		struct string *str = stack_peek_string(0);
		char *utf = sjis2utf(str->text, str->size);

		const SDL_MessageBoxData mbox = {
			SDL_MESSAGEBOX_INFORMATION,
			NULL,
			"xsystem4",
			utf,
			SDL_arraysize(ok_cancel_buttons),
			ok_cancel_buttons,
			NULL
		};
		if (getenv("XSYS4_AUTO_MSGBOX")) {
			NOTICE("AUTO_MSGBOX_OK_CANCEL: '%s' -> OK", utf);
			result = 1;  // auto-confirm (OK) for automated testing
		} else if (SDL_ShowMessageBox(&mbox, &result)) {
			WARNING("Error displaying message box");
		}
		free(utf);
		heap_unref(stack_pop().i);
		stack_push(result);
		break;
	}
	case SYS_RESUME_SAVE: {
		union vm_value *success = stack_pop_var();
		struct string *filename = stack_peek_string(0);
		struct string *keyname = stack_peek_string(1);
		success->i = vm_save_image(keyname->text, filename->text);
		heap_unref(stack_pop().i);
		heap_unref(stack_pop().i);
		stack_push(1);
		break;
	}
	case SYS_RESUME_LOAD: {
		int filename_slot = stack_pop().i;
		int key_slot = stack_pop().i;
		vm_load_image(heap_get_string(key_slot)->text, heap_get_string(filename_slot)->text);
		stack_push(0);
		break;
	}
	case SYS_EXISTS_FILE: { // system.ExistsFile(string szFileName)
		int str = stack_pop().i;
		char *path = gamedir_path(heap_get_string(str)->text);
		int len = strlen(path);
		// Return true for directories, but false if the path ends with a directory separator.
		if (len > 0 && (path[len-1] == '/' || path[len-1] == '\\')) {
			stack_push(0);
		} else {
			stack_push(file_exists(path));
		}
		heap_unref(str);
		free(path);
		break;
	}
	case SYS_OPEN_WEB: {
#if !SDL_VERSION_ATLEAST(2, 0, 14)
		WARNING("SDL_OpenURL not available");
#else
		struct string *url = stack_peek_string(0);
		if (SDL_OpenURL(url->text) < 0) {
			WARNING("SDL_OpenURL failed: %s", SDL_GetError());
		}
#endif
		heap_unref(stack_pop().i);
		break;
	};
	case SYS_GET_SAVE_FOLDER_NAME: {// system.GetSaveFolderName(void)
		if (config.save_dir) {
			char *sjis = utf2sjis(config.save_dir, strlen(config.save_dir));
			stack_push_string(make_string(sjis, strlen(sjis)));
			free(sjis);
		} else {
			stack_push_string(string_ref(&EMPTY_STRING));
		}
		break;
	}
	case SYS_GET_TIME: {// system.GetTime(void)
		stack_push(vm_time());
		break;
	}
	case SYS_GET_GAME_NAME: {// system.GetGameName(void)
		stack_push_string(make_string(config.game_name, strlen(config.game_name)));
		break;
	}
	case SYS_ERROR: {// system.Error(string szText)
		int result = 0;
		struct string *str = stack_peek_string(0);
		char *utf = sjis2utf(str->text, str->size);
		sys_warning("*GAME ERROR*: %s\n", utf);
		const SDL_MessageBoxData mbox = {
			SDL_MESSAGEBOX_ERROR,
			NULL,
			"Game Error - xsystem4",
			utf,
			SDL_arraysize(stop_continue_buttons),
			stop_continue_buttons,
			NULL
		};
		// По умолчанию авто-Continue: system.Error — это assert самого скрипта игры
		// (у него есть кнопка Continue). Модальный диалог на каждый assert (напр.
		// Tsumamigui 3 при инициализации сыплет min>max) блокировал бы игру, особенно
		// на Android. Для отладки можно вернуть диалог через XSYS4_STOP_ON_GAME_ERROR.
		if (getenv("XSYS4_STOP_ON_GAME_ERROR")) {
			if (SDL_ShowMessageBox(&mbox, &result))
				WARNING("Error displaying message box");
		} else {
			result = 0;
		}
		free(utf);
		if (result == 1) {
			// stop execution
			vm_exit(1);
		}
		// XXX: caller S_POPs
		break;
	}
	case SYS_EXISTS_SAVE_FILE: {
		int slot = stack_pop().i;
		char *path = savedir_path(heap_get_string(slot)->text);
		stack_push(file_exists(path));
		heap_unref(slot);
		free(path);
		break;
	}
	case SYS_IS_DEBUG_MODE: {// system.IsDebugMode(void)
		stack_push(0);
		break;
	}
	case SYS_GET_FUNC_STACK_NAME: { // system.GetFuncStackName(int nIndex)
		stack_push_string(get_func_stack_name(stack_pop().i));
		break;
	}
	case SYS_PEEK: {// system.Peek(void)
		handle_events();
		break;
	}
	case SYS_SLEEP: {// system.Sleep(int nSleep)
		int ms = stack_pop().i;
		vm_sleep(ms);
		break;
	}
	case SYS_RESUME_READ_COMMENT: {// system.ResumeReadComment(string szKeyName, string szFileName, ref array@string aszComment)
		int success;
		int comment = stack_pop().i;
		int filename = stack_pop().i;
		int keyname = stack_pop().i;
		// FIXME: free ref'd array if allocated
		heap_set_page(comment, vm_load_image_comments(heap_get_string(keyname)->text,
							      heap_get_string(filename)->text,
							      &success));
		heap_unref(filename);
		heap_unref(keyname);
		stack_push(success);
		break;
	}
	case SYS_RESUME_WRITE_COMMENT: { // system.ResumeWriteComment(string szKeyName, string szFileName, ref array@string aszComment)
		int comment = stack_pop().i;
		int filename = stack_pop().i;
		int keyname = stack_pop().i;
		stack_push(vm_write_image_comments(heap_get_string(keyname)->text,
						   heap_get_string(filename)->text,
						   heap_get_page(comment)));
		heap_unref(filename);
		heap_unref(keyname);
		break;
	}
	case SYS_GROUP_SAVE: { // system.GroupSave(string szKeyName, string szFileName, string szGroupName, ref int nNumofLoad)
		union vm_value *n = stack_pop_var();
		int groupname = stack_pop().i;
		int filename = stack_pop().i;
		int keyname = stack_pop().i;
		stack_push(save_globals(heap_get_string(keyname)->text,
				      heap_get_string(filename)->text,
				      heap_get_string(groupname)->text,
				      &n->i));
		heap_unref(groupname);
		heap_unref(filename);
		heap_unref(keyname);
		break;
	}
	case SYS_GROUP_LOAD: { // system.GroupLoad(string szKeyName, string szFileName, string szGroupName, ref int nNumofLoad)
		union vm_value *n = stack_pop_var();
		int groupname = stack_pop().i;
		int filename = stack_pop().i;
		int keyname = stack_pop().i;
		stack_push(load_globals(heap_get_string(keyname)->text,
					heap_get_string(filename)->text,
					heap_get_string(groupname)->text,
					&n->i));
		heap_unref(groupname);
		heap_unref(filename);
		heap_unref(keyname);
		break;
	}
	case SYS_DELETE_SAVE_FILE: { // system.DeleteSaveFile(string szFileName)
		int filename = stack_pop().i;
		stack_push(delete_save_file(heap_get_string(filename)->text));
		heap_unref(filename);
		break;
	}
	case SYS_EXIST_FUNC: { // system.ExistFunc(string szFuncName)
		int funcname = stack_pop().i;
		stack_push(ain_get_function(ain, heap_get_string(funcname)->text) > 0);
		heap_unref(funcname);
		break;
	}
	case SYS_COPY_SAVE_FILE: { // system.CopySaveFile(string szDestFileName, string szSourceFileName)
		int src = stack_pop().i;
		int dst = stack_pop().i;
		char *u_src = savedir_path(heap_get_string(src)->text);
		char *u_dst = savedir_path(heap_get_string(dst)->text);
		stack_push(file_copy(u_src, u_dst));
		free(u_src);
		free(u_dst);
		heap_unref(src);
		heap_unref(dst);
		break;
	}
	default:
		// xsystem4-specific system calls (used for hacks)
		switch ((enum vm_extra_syscall)code) {
		case VM_XSYS_KEY_IS_DOWN:
			stack_push(key_is_down(stack_pop().i));
			break;
		default:
			VM_ERROR("Unimplemented syscall: 0x%X", code);
		}
	}
}

uint32_t get_switch_address(int no, int val)
{
	struct ain_switch *s = &ain->switches[no];
	for (int i = 0; i < s->nr_cases; i++) {
		if (s->cases[i].value == val) {
			return s->cases[i].address;
		}
	}
	if (s->default_address > 0)
		return s->default_address;
	else
		return instr_ptr + instruction_width(SWITCH);
}

uint32_t get_strswitch_address(int no, struct string *str)
{
	struct ain_switch *s = &ain->switches[no];
	for (int i = 0; i < s->nr_cases; i++) {
		if (!strcmp(str->text, ain->strings[s->cases[i].value]->text)) {
			return s->cases[i].address;
		}
	}
	if (s->default_address > 0)
		return s->default_address;
	else
		return instr_ptr + instruction_width(STRSWITCH);
}

static void echo_message(int i)
{
	NOTICE("MSG %d: %s", i, display_sjis0(ain->messages[i]->text));
}

static enum opcode execute_instruction(enum opcode opcode)
{
	switch (opcode) {
	//
	// --- Stack Management ---
	//
	case PUSH: {
		stack_push(get_argument(0));
		break;
	}
	case POP: {
		stack_pop();
		break;
	}
	case F_PUSH: {
		stack_push(get_argument_float(0));
		break;
	}
	case REF: {
		// Dereference a reference to a value.
		stack_push(stack_pop_var()->i);
		break;
	}
	case REFREF: {
		// Dereference a reference to a reference.
		union vm_value *ref = stack_pop_var();
		stack_push(ref[0].i);
		stack_push(ref[1].i);
		break;
	}
	case DUP: {
		// A -> AA
		stack_push(stack_peek(0).i);
		break;
	}
	case DUP2: {
		// AB -> ABAB
		int a = stack_peek(1).i;
		int b = stack_peek(0).i;
		stack_push(a);
		stack_push(b);
		break;
	}
	case DUP_X2: {
		// ABC -> CABC
		int a = stack_peek(2).i;
		int b = stack_peek(1).i;
		int c = stack_peek(0).i;
		stack_set(2, c);
		stack_set(1, a);
		stack_set(0, b);
		stack_push(c);
		break;
	}
	case DUP2_X1: {
		// ABC -> BCABC
		int a = stack_peek(2).i;
		int b = stack_peek(1).i;
		int c = stack_peek(0).i;
		stack_set(2, b);
		stack_set(1, c);
		stack_set(0, a);
		stack_push(b);
		stack_push(c);
		break;
	}
	case DUP_U2: {
		// AB -> ABA
		stack_push(stack_peek(1).i);
		break;
	}
	case SWAP: {
		int a = stack_peek(1).i;
		stack_set(1, stack_peek(0));
		stack_set(0, a);
		break;
	}
	//
	// --- System 4 «X_*» инструкции (новые релизы: Dohna Dohna, Healing Touch) ---
	//
	case X_DUP: {
		// DUP для нескольких значений: копирует верхние N слотов блоком.
		// Обобщает DUP (N=1) и DUP2 (N=2): [.. v0..vN-1] -> [.. v0..vN-1 v0..vN-1].
		int n = get_argument(0);
		int base = stack_ptr - n;
		for (int i = 0; i < n; i++)
			stack[stack_ptr++] = stack[base + i];
		break;
	}
	case X_REF: {
		// REF для нескольких значений: снимает ссылку (page,var) и кладёт
		// N подряд идущих значений из неё (N=1 эквивалентно обычному REF).
		int n = get_argument(0);
		union vm_value *ref = stack_pop_var();
		for (int i = 0; i < n; i++)
			stack[stack_ptr++] = ref[i];
		break;
	}
	case X_ASSIGN: {
		// ASSIGN для нескольких значений: [ref(2 слота) v0..vN-1] — снять N значений,
		// снять ссылку, записать N значений в ref[0..N-1], вернуть их на стек
		// (как обычный ASSIGN возвращает значение). N=1 эквивалентно ASSIGN.
		int n = get_argument(0);
		// n == 0 — пакетная инициализация ПУСТОГО массива
		// (`PUSH 0; X_A_INIT; PUSH 0; X_ASSIGN 0`, все 12 сайтов такие):
		// писать нечего, а разыменовывать ссылку НЕЛЬЗЯ — у массива нулевой
		// длины индекс 0 уже вне границ (`Out of bounds page index: 223/0`
		// в CASCommonData@2). Просто снимаем ссылку.
		if (n == 0) {
			stack_pop();
			stack_pop();
			break;
		}
		union vm_value vals[n];
		for (int i = n - 1; i >= 0; i--)
			vals[i] = stack_pop();
		union vm_value *ref = stack_pop_var();
		for (int i = 0; i < n; i++)
			ref[i] = vals[i];
		for (int i = 0; i < n; i++)
			stack[stack_ptr++] = vals[i];
		break;
	}
	case X_A_INIT: {
		// Initialise a generic array (type 79). Stack: [page, var, size].
		// Create a fresh but correctly-TYPED array page in the referenced
		// variable — the element type/struct-type/rank come from the variable's
		// declared type. Using a bare NULL page here would lose the element type,
		// so a later Array.Alloc couldn't tell whether elements are ints, strings
		// or structs (it would default to int and corrupt struct arrays).
		// РАЗМЕР со стека НЕ игнорируется: паттерн bulk-инициализации массива
		// (`PUSH n; X_A_INIT; PUSH v0..v(n-1); X_ASSIGN n`) требует, чтобы массив
		// был создан РАЗМЕРА n (иначе последующая пакетная запись пишет за границы —
		// напр. CSpriteParts@2 строит таблицу из ~350 индексов методов → «376/0»).
		// size==0 (напр. в конструкторе CASTimerManager) даёт пустой массив,
		// который потом растят через EmplaceBack — поведение прежнее.
		int size = stack_pop().i;
		int page_index = stack_pop().i;
		int heap_index = stack_pop().i;
		struct page *container = heap_index_valid(heap_index) ? heap[heap_index].page : NULL;
		int slot = heap_alloc_slot(VM_PAGE);
		if (container && page_index >= 0 && page_index < container->nr_vars) {
			int struct_type = 0, rank = 1;
			bool ref_elem = false;
			enum ain_data_type dt = array_resolve_var_type(container, page_index, &struct_type,
								       &rank, &ref_elem);
			union vm_value dim = { .i = size > 0 ? size : 0 };
			// Элемент, объявленный ССЫЛКОЙ (`array<ref Структура>`), пуст до
			// присваивания — предзаполнять его сконструированными объектами
			// нельзя (в Ixseal объекты конструирует ИГРА, см. ПРОДВИЖЕНИЕ 14).
			heap_set_page(slot, alloc_array(rank, &dim, dt, struct_type, !ref_elem));
			container->values[page_index].i = slot;
		} else {
			heap_set_page(slot, NULL);
		}
		stack_push(slot);
		break;
	}
	case X_MOV: {
		// X_MOV <a> <b>: rotate the top `a` stack values so the top `b` of them
		// move to the bottom of that window and the lower `a-b` shift up to the
		// top (preserving relative order within each group). For a=2,b=1 this is
		// a swap of the top two — e.g. `X_MOV 2 1; ITOF; X_MOV 2 1; F_MUL` brings
		// the deeper operand up to be int->float converted, then restores order.
		int a = get_argument(0);
		int b = get_argument(1);
		if (a > 0 && b > 0 && a <= b + 32 && b < a && stack_ptr >= a) {
			union vm_value tmp[a];
			for (int i = 0; i < b; i++)
				tmp[i] = stack[stack_ptr - b + i];
			for (int i = 0; i < a - b; i++)
				tmp[b + i] = stack[stack_ptr - a + i];
			for (int i = 0; i < a; i++)
				stack[stack_ptr - a + i] = tmp[i];
		} else if (a > 0 && b > 0 && stack_ptr >= a) {
			WARNING("X_MOV: unexpected operands a=%d b=%d sp=%d", a, b, stack_ptr);
		}
		break;
	}
	case X_GETENV: {
		// Замыкание (Ixseal): получить ЗАХВАЧЕННОЕ окружение лямбды —
		// локальную страницу лексически-объемлющей функции. Делегат хранит
		// только (obj, fun), где obj = `this` объемлющего метода, а НЕ
		// окружение; поэтому env восстанавливаем по стеку вызовов (лямбда
		// вызывается синхронно трамплином-фреймворком, объемлющий кадр жив).
		// Идиома `PUSHLOCALPAGE; X_GETENV; PUSH n; X_REF 1` читает env.member_n.
		// net 0: снять локальную страницу (не нужна), положить env-страницу.
		stack_pop();
		int env = lambda_env_page_slot();
		stack_push(env >= 0 ? env : struct_page_slot());
		break;
	}
	case X_A_SIZE: {
		// Размер generic-массива (Ixseal). На стеке — ЗНАЧЕНИЕ переменной-массива
		// (heap-слот страницы), т.е. сайт выглядит как `...; X_REF 1; X_A_SIZE`.
		// Возвращается число ЭЛЕМЕНТОВ: у массива с элементом wrap<интерфейс>
		// на элемент приходится два слота страницы, и array_numof это учитывает.
		// Прежний стаб клал сюда struct_page_slot() — foreach получал мусорную
		// границу и уходил за конец массива.
		int slot = stack_pop().i;
		struct page *p = heap_index_valid(slot) ? heap[slot].page : NULL;
		stack_push((p && p->type == ARRAY_PAGE) ? array_numof(p, 1) : 0);
		break;
	}
	case X_TO_STR: {
		/*
		 * Неявное приведение к строке (`"tag" + (i + 1)`). Снимает ОДИН слот
		 * значения и кладёт слот строки; ОПЕРАНД — тип ИСТОЧНИКА. У Dohna
		 * встречаются ровно три: 10=int (460 сайтов), 11=float (101), 47=bool
		 * (14) — сверено `XSCAN_MAX=99999 xscan <ain> X_TO_STR`.
		 *
		 * Точность у float на стек НЕ кладётся (в отличие от легаси FTOS):
		 * System40.exe в таком случае пушит -1, а это по libsys4 значит 6
		 * знаков после запятой — та же ветка, что у старых игр.
		 *
		 * bool хранится int'ом, и формат («1» против «true») игра прочитать
		 * обратно не может: все 14 bool-сайтов — это `@ToString`/лог
		 * (напр. `"apply result:" + m_applied` → system.Output в
		 * `ActionFrameController@CatchUpFrame`), никто их не парсит.
		 *
		 * Прежний стаб отдавал слот СТРАНИЦЫ структуры — следующий S_ADD читал
		 * его как строку и падал (`Invalid string index` в `StandNameTags@0`).
		 */
		int src_type = get_argument(0);
		union vm_value v = stack_pop();
		switch (src_type) {
		case AIN_FLOAT:
			stack_push_string(float_to_string(v.f, -1));
			break;
		case AIN_INT:
		case AIN_BOOL:
			stack_push_string(integer_to_string(v.i));
			break;
		default: {
			static bool xtostr_logged = false;
			if (!xtostr_logged) {
				xtostr_logged = true;
				WARNING("X_TO_STR: тип источника %d не установлен, "
					"приводим как int", src_type);
			}
			stack_push_string(integer_to_string(v.i));
			break;
		}
		}
		break;
	}
	case X_OP_SET: {
		// Ixseal: записать значение в `option<T>`. Стек: [ссылка (2 слота),
		// v0..v(n-1), тег] — в память идут n слотов значения и СЛЕДОМ тег
		// (0 = значение есть, 1 = пусто), после чего всё возвращается на стек
		// (сайты снимают ровно n+1 POP'ов, как X_ASSIGN возвращает значение).
		//
		// Операнд — «класс элемента», тот же, что 3-й операнд CALLHLL:
		// 1=int, 2=string, 0x10002=объект-хэндл (по 1 слоту значения),
		// 0x10003=wrap<интерфейс> (2 слота: объект, база интерфейса).
		// Формы сайтов: «PUSH val; PUSH 0» (Some) против
		// «PUSH -1[; PUSH -1]; PUSH 1» (None).
		int elem_class = get_argument(0);
		int n = x_option_value_slots(elem_class);
		if (getenv("XSYS4_OPT_TRACE")) {
			WARNING("OPT pre-set class=0x%x n=%d sp=%d top=[%d %d %d %d %d %d]",
				elem_class, n, stack_ptr,
				stack_ptr>0?stack[stack_ptr-1].i:0, stack_ptr>1?stack[stack_ptr-2].i:0,
				stack_ptr>2?stack[stack_ptr-3].i:0, stack_ptr>3?stack[stack_ptr-4].i:0,
				stack_ptr>4?stack[stack_ptr-5].i:0, stack_ptr>5?stack[stack_ptr-6].i:0);
		}
		union vm_value vals[n + 1];
		for (int i = n; i >= 0; i--)
			vals[i] = stack_pop();
		union vm_value *ref = stack_pop_var();
		if (getenv("XSYS4_OPT_TRACE")) {
			WARNING("OPT set class=0x%x n=%d tag=%d val=%d (old tag=%d val=%d)",
				elem_class, n, vals[n].i, vals[0].i, ref[n].i, ref[0].i);
		}
		for (int i = 0; i <= n; i++)
			ref[i] = vals[i];
		// Взять ВЛАДЕНИЕ значением. Дисциплина ссылок в Ixseal явная (SP_INC /
		// DELETE), и X_OP_SET обязан считаться владельцем: возврат
		// `AFL_Parts_CreateSprite` приходит с +1 (SP_INC перед RETURN), а сразу
		// после X_OP_SET сайт освобождает временный локал («X_REF 1; DELETE») —
		// без своей ссылки option оставался бы висячим.
		// Прежнее содержимое НЕ освобождаем: сайты установки в None
		// (`PUSH -1; PUSH -1; PUSH 1`) не делают этого сами, но семантика по
		// байткоду не установлена — лишний unref дал бы double free, а лишняя
		// ссылка только течёт.
		if (x_option_class_is_ref(elem_class) && vals[n].i == 0 && vals[0].i != -1)
			heap_ref(vals[0].i);
		for (int i = 0; i <= n; i++)
			stack[stack_ptr++] = vals[i];
		break;
	}
	case X_SET: {
		// Ixseal: присваивание с ВЗЯТИЕМ ВЛАДЕНИЯ. Стек: [ссылка (2 слота),
		// значение]; значение возвращается на стек (сайты снимают его POP'ом
		// или DELETE'ом).
		//
		// От `X_ASSIGN 1` отличается именно владением. X_ASSIGN — сырая запись
		// слота, счётчики ведёт сам компилятор (SP_INC/DELETE), и перед ним
		// всегда стоит идиома освобождения старого значения
		// («X_DUP 2; X_REF 1; DELETE»). У X_SET её нет, а значение — либо
		// одалживаемый аргумент (`Params::set`: `X_REF 1; X_SET; POP`), либо
		// свежая копия, которую сайт сразу освобождает
		// (`A_REF; X_SET; DELETE`, `Array.Where(...); X_SET; DELETE`).
		// Без своей ссылки член в обоих случаях остался бы висячим.
		//
		// Счётчик берём ТОЛЬКО для типов, которые владеют heap-слотом (набор
		// тот же, что освобождает variable_fini): для int/float это затронуло
		// бы чужой слот кучи. Тип берётся из ОБЪЯВЛЕНИЯ приёмника.
		// Прежнее содержимое не освобождаем — как и в X_OP_SET: семантика по
		// байт-коду не установлена, лишний unref дал бы double free.
		int val = stack_pop().i;
		int page_index = stack_pop().i;
		int heap_index = stack_pop().i;
		if (unlikely(!heap_index_valid(heap_index) || !heap[heap_index].page
			    || page_index < 0 || page_index >= heap[heap_index].page->nr_vars))
			VM_ERROR("X_SET: out of bounds page index: %d/%d", heap_index, page_index);
		struct page *page = heap[heap_index].page;
		if (val != -1 && slot_owns_heap_ref(variable_type(page, page_index, NULL, NULL)))
			heap_ref(val);
		page->values[page_index].i = val;
		stack_push(val);
		break;
	}
	case X_ICAST: {
		/*
		 * Приведение ссылки к типу-операнду (оператор «as»): снимает ОДИН слот
		 * (хэндл объекта) и кладёт ТРИ — `[obj, base, тег]`:
		 *   тег 0 = приведение удалось (та же конвенция, что у `option<T>`:
		 *           0 = значение ЕСТЬ), >=1 = не удалось;
		 *   base  = смещение методов целевого интерфейса в vtable объекта, т.е.
		 *           вторая половина 2-слотовой пары `wrap<интерфейс>`;
		 *   obj   = сам объект (на провале -1).
		 *
		 * Форма снята с байткода. Сайт с интерфейсной целью
		 * (`activityeditor::detail::CInstanceItem@GetSprite` @0x5C50A,
		 * цель 394=ISpriteParts) прямо показывает и число слотов, и смысл тега:
		 *   X_ICAST 394; PUSH 1; GTE; IFNZ fail
		 *   fail: POP; POP; PUSH -1; PUSH 0      // пара → null-интерфейс
		 *   ok:   X_DUP 2 ...                    // пара идёт в дело
		 * т.е. проверяется `тег >= 1` = НЕ удалось, а на успехе остаётся пара.
		 * Сайт со структурной целью (`Motion::EasingArgumentAnalyzer@
		 * AnalyzeEasingType` @0x597D4E, цель 623=Motion::ArgumentEasingType)
		 * делает `X_ICAST 623; X_MOV 2 1; POP; POP` — отбрасывает base и тег и
		 * оставляет obj, а провал ловит сравнением `obj != -1`; поэтому на
		 * провале obj обязан быть -1. Оба варианта требуют ровно +2 слота к
		 * входному — это же следует из баланса стека обеих функций.
		 *
		 * `base` берётся из .ain: у структуры есть список реализованных
		 * интерфейсов `interfaces[] = {struct_type, vtable_offset}` (libsys4
		 * его давно читает, движок не использовал). Проверено: у
		 * Motion::ArgumentEasingType(623) и ArgumentDigit(621) — по одному
		 * интерфейсу Motion::IArgument(620) с vtable_offset=0.
		 */
		int target = get_argument(0);
		if (instructions[opcode].nr_args >= 2 && get_argument(1) != 0)
			WARNING("X_ICAST: второй операнд %d != 0 — назначение не установлено",
				get_argument(1));
		int src = stack_pop().i;
		int base = -1;
		if (heap_index_valid(src) && heap[src].page
		    && heap[src].page->type == STRUCT_PAGE) {
			// index у STRUCT_PAGE — номер структуры объекта.
			int st = heap[src].page->index;
			if (st == target) {
				// Приведение к собственному типу (в т.ч. обратное
				// приведение интерфейс→конкретный класс): методы объекта
				// лежат с начала его vtable.
				base = 0;
			} else if (st >= 0 && st < ain->nr_structures) {
				struct ain_struct *s = &ain->structures[st];
				for (int i = 0; i < s->nr_interfaces; i++) {
					if (s->interfaces[i].struct_type == target) {
						base = s->interfaces[i].vtable_offset;
						break;
					}
				}
			}
		}
		if (base < 0) {
			// Не удалось: null-объект и тег «пусто».
			stack_push(-1);
			stack_push(0);
			stack_push(1);
		} else {
			stack_push(src);
			stack_push(base);
			stack_push(0);
		}
		break;
	}
	//
	// --- Variables ---
	//
	case PUSHGLOBALPAGE: {
		stack_push(0);
		break;
	}
	case PUSHLOCALPAGE: {
		stack_push(local_page_slot());
		break;
	}
	case PUSHSTRUCTPAGE: {
		stack_push(struct_page_slot());
		break;
	}
	case ASSIGN:
	case F_ASSIGN: {
		union vm_value val = stack_pop();
		stack_pop_var()[0] = val;
		stack_push(val);
		break;
	}
	case SH_GLOBALREF: { // VARNO
		stack_push(global_get(get_argument(0)).i);
		break;
	}
	case SH_LOCALREF: { // VARNO
		stack_push(local_get(get_argument(0)).i);
		break;
	}
	case _EOF: // In Ain v0, opcode 0x62 is not EOF but SH_STRUCTREF
		if (ain->version != 0)
			VM_ERROR("Illegal opcode: 0x%04x", opcode);
		// fallthrough
	case SH_STRUCTREF: { // VARNO
		stack_push(member_get(get_argument(0)));
		break;
	}
	case SH_LOCALASSIGN: { // VARNO, VALUE
		local_set(get_argument(0), get_argument(1));
		break;
	}
	case SH_LOCALINC: { // VARNO
		int varno = get_argument(0);
		local_set(varno, local_get(varno).i+1);
		break;
	}
	case SH_LOCALDEC: { // VARNO
		int varno = get_argument(0);
		local_set(varno, local_get(varno).i-1);
		break;
	}
	case SH_LOCALDELETE: {
		int slot = local_get(get_argument(0)).i;
		if (slot != -1) {
			heap_unref(slot);
			local_set(get_argument(0), -1);
		}
		break;
	}
	case SH_LOCALCREATE: { // VARNO, STRUCTNO
		create_struct(get_argument(1), local_ptr(get_argument(0)));
		break;
	}
	case R_ASSIGN: {
		int src_var = stack_pop().i;
		int src_page = stack_pop().i;
		int dst_var = stack_pop().i;
		struct page *dst = heap_get_page(stack_pop().i);
		page_set_var(dst, dst_var, src_page);
		page_set_var(dst, dst_var+1, src_var);
		stack_push(src_page);
		stack_push(src_var);
		break;
	}
	case R_EQUALE: {
		int rhs_var = stack_pop().i;
		int rhs_page = stack_pop().i;
		int lhs_var = stack_pop().i;
		int lhs_page = stack_pop().i;
		stack_push(lhs_page == rhs_page && lhs_var == rhs_var ? 1 : 0);
		break;
	}
	case R_NOTE: {
		int rhs_var = stack_pop().i;
		int rhs_page = stack_pop().i;
		int lhs_var = stack_pop().i;
		int lhs_page = stack_pop().i;
		stack_push(lhs_page == rhs_page && lhs_var == rhs_var ? 0 : 1);
		break;
	}
	case NEW: {
		// Older System 4 pops the struct type off the stack; Ixseal passes it as
		// an instruction operand instead (NEW <struct> <ctor-fno-or-(-1)>). Popping
		// the stack in the operand form consumes an unrelated slot and corrupts
		// the surrounding expression (e.g. `global[x] = new T()` loses its lvalue
		// reference). Pick the source based on the linked operand count.
		union vm_value v;
		if (instructions[NEW].nr_args >= 2 && get_argument(1) >= 0) {
			// Ixseal: NEW <struct_type> <ctor_fno> — вызвать КОНКРЕТНЫЙ (в т.ч.
			// принимающий аргументы) конструктор. Его аргументы уже лежат на
			// стеке. Выделяем объект БЕЗ дефолт-конструктора, вставляем новый
			// объект как receiver ПОД аргументы ctor, синхронно исполняем ctor
			// (как vm_call), затем оставляем объект на стеке. Без этого аргументы
			// ctor оставались на стеке, и следующий X_ASSIGN читал их как битую
			// ссылку (напр. new CASColor(255,255,255,255) → «Out of bounds page
			// index: 255/255»).
			int struct_type = get_argument(0);
			int ctor = get_argument(1);
			int slot = alloc_struct(struct_type);
			int nargs = (ctor >= 0 && ctor < ain->nr_functions)
				? ain->functions[ctor].nr_args : 0;
			for (int k = 0; k < nargs; k++)
				stack[stack_ptr - k] = stack[stack_ptr - 1 - k];
			stack[stack_ptr - nargs].i = slot;
			stack_ptr++;
			size_t saved_ip = instr_ptr;
			method_call(ctor, VM_RETURN);
			vm_execute();
			instr_ptr = saved_ip;
			v.i = slot;
		} else {
			int struct_type = instructions[NEW].nr_args >= 1 ? get_argument(0) : stack_pop().i;
			create_struct(struct_type, &v);
		}
		stack_push(v);
		break;
	}
	case DELETE: {
		int slot = stack_pop().i;
		if (slot != -1)
			heap_unref(slot);
		break;
	}
	case SP_INC: {
		heap_ref(stack_pop().i);
		break;
	}
	case OBJSWAP: {
		// Ixseal passes the type as an operand rather than on the stack.
		if (instructions[OBJSWAP].nr_args < 1)
			stack_pop(); // type (older form: on the stack)
		union vm_value *b = stack_pop_var();
		union vm_value *a = stack_pop_var();
		union vm_value tmp = *a;
		*a = *b;
		*b = tmp;
		break;
	}
	//
	// --- Control Flow ---
	//
	case CALLFUNC: {
		if (fn_trace_count != 0) vm_fn_trace(get_argument(0), "CALLFUNC");
		function_call(get_argument(0), instr_ptr + instruction_width(CALLFUNC));
		break;
	}
	case CALLFUNC2: {
		stack_pop(); // function-type index (only needed for compilation)
		function_call(stack_pop().i, instr_ptr + instruction_width(CALLFUNC2));
		break;
	}
	case CALLMETHOD: {
		int fno;
		if (instructions[CALLMETHOD].args[0] == T_INT) {
			// Ixseal (ain v>=11): the operand is the ARGUMENT COUNT and the
			// method index is passed on the stack, below the arguments:
			//   PUSH receiver; PUSH method_idx; PUSH arg0..arg(n-1); CALLMETHOD n
			// Read the method index from below the args and splice its slot out
			// so the stack is left as [receiver, arg0..arg(n-1)] — exactly what
			// method_call() consumes (pop args, then pop receiver).
			int nargs = get_argument(0);
			int mi_pos = stack_ptr - 1 - nargs;
			fno = stack[mi_pos].i;
			for (int k = mi_pos; k < stack_ptr - 1; k++)
				stack[k] = stack[k + 1];
			stack_ptr--;
		} else {
			// Older releases: the operand is the method index; 0 means dynamic
			// dispatch with the index on top of the stack.
			fno = get_argument(0);
			if (fno == 0)
				fno = stack_pop().i;
		}
		if (fn_trace_count != 0) vm_fn_trace(fno, "CALLMETHOD");
		method_call(fno, instr_ptr + instruction_width(CALLMETHOD));
		break;
	}
	case CALLHLL: {
		// Ixseal added a third operand describing the element type of a generic
		// container (see hll_call). Older games emit only two.
		int elem_class = instructions[CALLHLL].nr_args >= 3 ? get_argument(2) : 0;
		hll_call(get_argument(0), get_argument(1), elem_class);
		break;
	}
	case RETURN: {
		function_return();
		break;
	}
	case CALLSYS: {
		system_call(get_argument(0));
		break;
	}
	case CALLONJUMP: {
		int str = stack_pop().i;
		if (ain->scenario_labels) {
			stack_push(scenario_label_addr(heap_get_string(str)->text));
		} else {
			// XXX: I am GUESSING that the VM pre-allocates the scenario function's
			//      local page here. It certainly pushes what appears to be a page
			//      index to the stack.
			stack_push(alloc_scenario_page(heap_get_string(str)->text));
		}
		heap_unref(str);
		break;
	}
	case SJUMP: {
		if (ain->scenario_labels) {
			scenario_jump(stack_pop().i);
		} else {
			scenario_call(stack_pop().i);
		}
		break;
	}
	case _MSG: {
		if (config.echo)
			echo_message(get_argument(0));
		if (ain->msgf < 0)
			break;
		stack_push(get_argument(0));
		stack_push(ain->nr_messages);
		stack_push_string(string_ref(ain->messages[get_argument(0)]));
		function_call(ain->msgf, instr_ptr + instruction_width(_MSG));
		break;
	}
	case JUMP: { // ADDR
		instr_ptr = get_argument(0);
		break;
	}
	case IFZ: { // ADDR
		if (!stack_pop().i)
			instr_ptr = get_argument(0);
		else
			instr_ptr += instruction_width(IFZ);
		break;
	}
	case IFNZ: { // ADDR
		if (stack_pop().i)
			instr_ptr = get_argument(0);
		else
			instr_ptr += instruction_width(IFNZ);
		break;
	}
	case SWITCH: {
		instr_ptr = get_switch_address(get_argument(0), stack_pop().i);
		break;
	}
	case STRSWITCH: {
		int str = stack_pop().i;
		instr_ptr = get_strswitch_address(get_argument(0), heap_get_string(str));
		heap_unref(str);
		break;
	}
	case ASSERT: {
		int line = stack_pop().i; // line number
		int file = stack_pop().i; // filename
		int expr = stack_pop().i; // expression
		if (!stack_pop().i) {
			sys_message("Assertion failed at %s:%d: %s\n",
					display_sjis0(heap_get_string(file)->text),
					line,
					display_sjis1(heap_get_string(expr)->text));
			vm_exit(1);
		}
		heap_unref(file);
		heap_unref(expr);
		break;
	}
	//
	// --- Arithmetic ---
	//
	case INV: {
		stack[stack_ptr-1].i = -stack[stack_ptr-1].i;
		break;
	}
	case NOT: {
		stack[stack_ptr-1].i = !stack[stack_ptr-1].i;
		break;
	}
	case COMPL: {
		stack[stack_ptr-1].i = ~stack[stack_ptr-1].i;
		break;
	}
	case ADD: {
		stack[stack_ptr-2].i += stack[stack_ptr-1].i;
		stack_ptr--;
		break;
	}
	case SUB: {
		stack[stack_ptr-2].i -= stack[stack_ptr-1].i;
		stack_ptr--;
		break;
	}
	case MUL: {
		stack[stack_ptr-2].i *= stack[stack_ptr-1].i;
		stack_ptr--;
		break;
	}
	case DIV: {
		if (!stack[stack_ptr-1].i) {
			stack[stack_ptr-2].i = 0;
		} else {
			stack[stack_ptr-2].i /= stack[stack_ptr-1].i;
		}
		stack_ptr--;
		break;
	}
	case MOD: {
		if (!stack[stack_ptr-1].i) {
			stack[stack_ptr-2].i = 0;
		} else {
			stack[stack_ptr-2].i %= stack[stack_ptr-1].i;
		}
		stack_ptr--;
		break;
	}
	case AND: {
		stack[stack_ptr-2].i &= stack[stack_ptr-1].i;
		stack_ptr--;
		break;
	}
	case OR: {
		stack[stack_ptr-2].i |= stack[stack_ptr-1].i;
		stack_ptr--;
		break;
	}
	case XOR: {
		stack[stack_ptr-2].i ^= stack[stack_ptr-1].i;
		stack_ptr--;
		break;
	}
	case LSHIFT: {
		stack[stack_ptr-2].i <<= stack[stack_ptr-1].i;
		stack_ptr--;
		break;
	}
	case RSHIFT: {
		stack[stack_ptr-2].i >>= stack[stack_ptr-1].i;
		stack_ptr--;
		break;
	}
	// Numeric Comparisons
	case LT: {
		int32_t b = stack_pop().i;
		int32_t a = stack_pop().i;
		stack_push(a < b ? 1 : 0);
		break;
	}
	case GT: {
		int32_t b = stack_pop().i;
		int32_t a = stack_pop().i;
		stack_push(a > b ? 1 : 0);
		break;
	}
	case LTE: {
		int32_t b = stack_pop().i;
		int32_t a = stack_pop().i;
		stack_push(a <= b ? 1 : 0);
		break;
	}
	case GTE: {
		int32_t b = stack_pop().i;
		int32_t a = stack_pop().i;
		stack_push(a >= b ? 1 : 0);
		break;
	}
	case NOTE: {
		int32_t b = stack_pop().i;
		int32_t a = stack_pop().i;
		stack_push(a != b ? 1 : 0);
		break;
	}
	case EQUALE: {
		int32_t b = stack_pop().i;
		int32_t a = stack_pop().i;
		stack_push(a == b ? 1 : 0);
		break;
	}
	// +=, -=, etc.
	case PLUSA: {
		int32_t n = stack_pop().i;
		stack_push(stack_pop_var()->i += n);
		break;
	}
	case MINUSA: {
		int32_t n = stack_pop().i;
		stack_push(stack_pop_var()->i -= n);
		break;
	}
	case MULA: {
		int32_t n = stack_pop().i;
		stack_push(stack_pop_var()->i *= n);
		break;
	}
	case DIVA: {
		int32_t n = stack_pop().i;
		stack_push(n ? stack_pop_var()->i /= n : 0);
		break;
	}
	case MODA: {
		int32_t n = stack_pop().i;
		stack_push(stack_pop_var()->i %= n);
		break;
	}
	case ANDA: {
		int32_t n = stack_pop().i;
		stack_push(stack_pop_var()->i &= n);
		break;
	}
	case ORA: {
		int32_t n = stack_pop().i;
		stack_push(stack_pop_var()->i |= n);
		break;
	}
	case XORA: {
		int32_t n = stack_pop().i;
		stack_push(stack_pop_var()->i ^= n);
		break;
	}
	case LSHIFTA: {
		int32_t n = stack_pop().i;
		stack_push(stack_pop_var()->i <<= n);
		break;
	}
	case RSHIFTA: {
		int32_t n = stack_pop().i;
		stack_push(stack_pop_var()->i >>= n);
		break;
	}
	case INC: {
		stack_pop_var()[0].i++;
		break;
	}
	case DEC: {
		stack_pop_var()[0].i--;
		break;
	}
	case ITOB: {
		stack_set(0, !!stack_peek(0).i);
		break;
	}
	//
	// --- 64-bit integers ---
	//
	case ITOLI: {
		stack_set(0, lint_clamp(stack_peek(0).i));
		break;
	}
	case LI_ADD: {
		int64_t a = stack[stack_ptr-2].i;
		int64_t b = stack[stack_ptr-1].i;
		stack[stack_ptr-2].i = lint_clamp(a + b);
		stack_ptr--;
		break;
	}
	case LI_SUB: {
		int64_t a = stack[stack_ptr-2].i;
		int64_t b = stack[stack_ptr-1].i;
		stack[stack_ptr-2].i = lint_clamp(a - b);
		stack_ptr--;
		break;
	}
	case LI_MUL: {
		int64_t a = stack[stack_ptr-2].i;
		int64_t b = stack[stack_ptr-1].i;
		stack[stack_ptr-2].i = lint_clamp(a * b);
		stack_ptr--;
		break;
	}
	case LI_DIV: {
		int64_t a = stack[stack_ptr-2].i;
		int64_t b = stack[stack_ptr-1].i;
		stack[stack_ptr-2].i = b ? lint_clamp(a / b) : 0;
		stack_ptr--;
		break;
	}
	case LI_MOD: {
		int64_t a = stack[stack_ptr-2].i;
		int64_t b = stack[stack_ptr-1].i;
		stack[stack_ptr-2].i = lint_clamp(a % b);
		stack_ptr--;
		break;
	}
	case LI_ASSIGN: {
		int64_t v = stack_pop().i;
		stack_push(stack_pop_var()->i = lint_clamp(v));
		break;
	}
	case LI_PLUSA: {
		int64_t n = stack_pop().i;
		union vm_value *v = stack_pop_var();
		stack_push(v->i = lint_clamp((int64_t)v->i + n));
		break;
	}
	case LI_MINUSA: {
		int64_t n = stack_pop().i;
		union vm_value *v = stack_pop_var();
		stack_push(v->i = lint_clamp((int64_t)v->i - n));
		break;
	}
	case LI_MULA: {
		int64_t n = stack_pop().i;
		union vm_value *v = stack_pop_var();
		stack_push(v->i = lint_clamp((int64_t)v->i * n));
		break;
	}
	case LI_DIVA: {
		int64_t n = stack_pop().i;
		union vm_value *v = stack_pop_var();
		stack_push(v->i = (n ? lint_clamp((int64_t)v->i / n) : 0));
		break;
	}
	case LI_MODA: {
		int64_t n = stack_pop().i;
		union vm_value *v = stack_pop_var();
		stack_push(v->i = lint_clamp((int64_t)v->i % n));
		break;
	}
	case LI_ANDA: {
		int64_t n = stack_pop().i;
		union vm_value *v = stack_pop_var();
		stack_push(v->i = lint_clamp((int64_t)v->i & n));
		break;
	}
	case LI_ORA: {
		int64_t n = stack_pop().i;
		union vm_value *v = stack_pop_var();
		stack_push(v->i = lint_clamp((int64_t)v->i | n));
		break;
	}
	case LI_XORA: {
		int64_t n = stack_pop().i;
		union vm_value *v = stack_pop_var();
		stack_push(v->i = lint_clamp((int64_t)v->i ^ n));
		break;
	}
	case LI_LSHIFTA: {
		int64_t n = stack_pop().i;
		union vm_value *v = stack_pop_var();
		stack_push(v->i = lint_clamp((int64_t)v->i << n));
		break;
	}
	case LI_RSHIFTA: {
		int64_t n = stack_pop().i;
		union vm_value *v = stack_pop_var();
		stack_push(v->i = lint_clamp((int64_t)v->i >> n));
		break;
	}
	case LI_INC: {
		union vm_value *v = stack_pop_var();
		v->i = lint_clamp((int64_t)v->i + (int64_t)1);
		break;
	}
	case LI_DEC: {
		union vm_value *v = stack_pop_var();
		v->i = lint_clamp((int64_t)v->i - (int64_t)1);
		break;
	}
	//
	// --- Floating Point Arithmetic ---
	//
	case FTOI: {
		stack_set(0, (int32_t)stack_peek(0).f);
		break;
	}
	case ITOF: {
		stack_set(0, (float)stack_peek(0).i);
		break;
	}
	case F_INV: {
		stack_set(0, -stack_peek(0).f);
		break;
	}
	case F_ADD: {
		float f = stack_pop().f;
		stack_set(0, stack_peek(0).f + f);
		break;
	}
	case F_SUB: {
		float f = stack_pop().f;
		stack_set(0, stack_peek(0).f - f);
		break;
	}
	case F_MUL: {
		float f = stack_pop().f;
		stack_set(0, stack_peek(0).f * f);
		break;
	}
	case F_DIV: {
		float f = stack_pop().f;
		stack_set(0, stack_peek(0).f / f);
		break;
	}
	// floating point comparison
	case F_LT: {
		float f = stack_pop().f;
		stack_set(0, stack_peek(0).f < f ? 1 : 0);
		break;
	}
	case F_GT: {
		float f = stack_pop().f;
		stack_set(0, stack_peek(0).f > f ? 1 : 0);
		break;
	}
	case F_LTE: {
		float f = stack_pop().f;
		stack_set(0, stack_peek(0).f <= f ? 1 : 0);
		break;
	}
	case F_GTE: {
		float f = stack_pop().f;
		stack_set(0, stack_peek(0).f >= f ? 1 : 0);
		break;
	}
	case F_NOTE: {
		float f = stack_pop().f;
		stack_set(0, stack_peek(0).f != f ? 1 : 0);
		break;
	}
	case F_EQUALE: {
		float f = stack_pop().f;
		stack_set(0, stack_peek(0).f == f ? 1 : 0);
		break;
	}
	case F_PLUSA: {
		float n = stack_pop().f;
		stack_push(stack_pop_var()->f += n);
		break;
	}
	case F_MINUSA: {
		float n = stack_pop().f;
		stack_push(stack_pop_var()->f -= n);
		break;
	}
	case F_MULA: {
		float n = stack_pop().f;
		stack_push(stack_pop_var()->f *= n);
		break;
	}
	case F_DIVA: {
		float n = stack_pop().f;
		stack_push(stack_pop_var()->f /= n);
		break;
	}
	//
	// --- Strings ---
	//
	case S_PUSH: {
		if (ain->version == 0)
			stack_push_string(string_ref(ain->messages[get_argument(0)]));
		else
			stack_push_string(string_ref(ain->strings[get_argument(0)]));
		break;
	}
	case S_POP: {
		heap_unref(stack_pop().i);
		break;
	}
	case S_REF: {
		// Dereference a reference to a string
		int str = stack_pop_var()->i;
		stack_push_string(string_ref(heap_get_string(str)));
		break;
	}
	//case S_REFREF: // ???: why/how is this different from regular REFREF?
	case S_ASSIGN: { // A = B
		// Ixseal (v11+) сменил ФОРМУ lvalue, как и у SR_ASSIGN: вместо
		// разыменованного слота строки на стеке лежит ДВУСЛОТОВАЯ ссылка
		// (страница, индекс) — сайты выглядят как `PUSHSTRUCTPAGE; PUSH <член>;
		// S_PUSH ...; S_ASSIGN; DELETE` или `...X_REF 1; PUSH <элемент>;
		// ...; A_REF; S_ASSIGN; DELETE`, т.е. БЕЗ классического REF/S_REF
		// (в v6/v7 он всегда есть — сверено xscan'ом по трём .ain).
		// Классический обработчик снимал (rval, index) и писал строку в
		// heap[index] — то есть в ЧУЖОЙ слот кучи (порча!), да ещё оставлял
		// на стеке лишний слот: конструктор структуры 418 возвращался с
		// перекошенным стеком, и следующий `X_ASSIGN` в глобальной
		// инициализации падал с `Out of bounds page index: 125/186`.
		if (instructions[CALLMETHOD].args[0] == T_INT) {
			int rval = stack_pop().i;
			union vm_value *ref = stack_pop_var();
			heap_string_assign(ref->i, heap_get_string(rval));
			stack_push(rval); // оставить B: сайт освобождает его DELETE'ом
			break;
		}
		int rval = stack_peek(0).i;
		int lval = stack_peek(1).i;
		heap_string_assign(lval, heap_get_string(rval));
		// remove A from the stack, but leave B
		stack_set(1, rval);
		stack_pop();
		break;
	}
	case S_PLUSA:
	case S_PLUSA2: {
		int a = stack_peek(1).i;
		int b = stack_peek(0).i;
		string_append(&heap[a].s, heap[b].s);
		heap_unref(b);
		stack_pop();
		stack_pop();
		stack_push_string(string_ref(heap[a].s));
		break;
	}
	case S_ADD: {
		int b = stack_pop().i;
		int a = stack_pop().i;
		// TODO: can use string_append here?
		stack_push_string(string_concatenate(heap_get_string(a), heap_get_string(b)));
		heap_unref(a);
		heap_unref(b);
		break;
	}
	case S_LT: {
		bool lt = strcmp(stack_peek_string(1)->text, stack_peek_string(0)->text) < 0;
		heap_unref(stack_pop().i);
		heap_unref(stack_pop().i);
		stack_push(lt);
		break;
	}
	case S_GT: {
		bool gt = strcmp(stack_peek_string(1)->text, stack_peek_string(0)->text) > 0;
		heap_unref(stack_pop().i);
		heap_unref(stack_pop().i);
		stack_push(gt);
		break;
	}
	case S_LTE: {
		bool lte = strcmp(stack_peek_string(1)->text, stack_peek_string(0)->text) <= 0;
		heap_unref(stack_pop().i);
		heap_unref(stack_pop().i);
		stack_push(lte);
		break;
	}
	case S_GTE: {
		bool gte = strcmp(stack_peek_string(1)->text, stack_peek_string(0)->text) >= 0;
		heap_unref(stack_pop().i);
		heap_unref(stack_pop().i);
		stack_push(gte);
		break;
	}
	case S_NOTE: {
		bool noteq = !!strcmp(stack_peek_string(1)->text, stack_peek_string(0)->text);
		heap_unref(stack_pop().i);
		heap_unref(stack_pop().i);
		stack_push(noteq);
		break;
	}
	case S_EQUALE: {
		bool eq = !strcmp(stack_peek_string(1)->text, stack_peek_string(0)->text);
		heap_unref(stack_pop().i);
		heap_unref(stack_pop().i);
		stack_push(eq);
		break;
	}
	case S_LENGTH: {
		int str = stack_pop_var()->i;
		stack_push(sjis_count_char(heap_get_string(str)->text));
		break;
	}
	case S_LENGTH2: {
		int str = stack_pop().i;
		stack_push(sjis_count_char(heap_get_string(str)->text));
		heap_unref(str);
		break;
	}
	case S_LENGTHBYTE: {
		int str = stack_pop_var()->i;
		stack_push(heap_get_string(str)->size);
		break;
	}
	case S_EMPTY: {
		bool empty = !stack_peek_string(0)->size;
		heap_unref(stack_pop().i);
		stack_push(empty);
		break;
	}
	case S_FIND: {
		int i = string_find(stack_peek_string(1), stack_peek_string(0));
		heap_unref(stack_pop().i);
		heap_unref(stack_pop().i);
		stack_push(i);
		break;
	}
	case S_GETPART: {
		int len = stack_pop().i; // length
		int i = stack_pop().i; // index
		struct string *s = string_copy(stack_peek_string(0), i, len);
		heap_unref(stack_pop().i);
		stack_push_string(s);
		break;
	}
	//case S_PUSHBACK: // ???
	case S_PUSHBACK2: {
		int c = stack_pop().i;
		int str = stack_pop().i;
		string_push_back(&heap[str].s, c);
		break;
	}
	//case S_POPBACK: // ???
	case S_POPBACK2: {
		int str = stack_pop().i;
		string_pop_back(&heap[str].s);
		break;
	}
	//case S_ERASE: // ???
	case S_ERASE2: {
		stack_pop(); // ???
		int i = stack_pop().i; // index
		int str = stack_pop().i;
		string_erase(&heap[str].s, i);
		break;
	}
	case S_MOD: {
		// Ixseal moved the value type from the stack to an instruction operand
		// (like NEW). Popping it in the operand form eats an unrelated slot and
		// corrupts the stack — and S_MOD runs for every formatted string.
		int type = instructions[S_MOD].nr_args >= 1 ? get_argument(0) : stack_pop().i;
		union vm_value val = stack_pop();
		int fmt = stack_pop().i;
		int dst = heap_alloc_slot(VM_STRING);
		heap[dst].s = string_format(heap[fmt].s, val, type);
		heap_unref(fmt);
		stack_push(dst);
		break;
	}
	case I_STRING: {
		stack_push_string(integer_to_string(stack_pop().i));
		break;
	}
	case FTOS: {
		int precision = stack_pop().i;
		stack_push_string(float_to_string(stack_pop().f, precision));
		break;
	}
	case STOI: {
		int str = stack_pop().i;
		stack_push(string_to_integer(heap[str].s));
		heap_unref(str);
		break;
	}
	case FT_ASSIGNS: {
		//int functype = stack_pop().i;
		stack_pop();
		int str = stack_pop().i;
		int fno = get_function_by_name(heap_get_string(str)->text);
		stack_pop_var()->i = fno > 0 ? fno : 0;
		stack_push(str);
		break;
	}
	// --- Characters ---
	case C_REF: {
		int i = stack_pop().i;
		int str = stack_pop().i;
		int32_t ch = string_get_char(heap_get_string(str), i);
		// 'é' for EN Rance Quest
		if (game_rance8_mg && ch == -91)
			stack_push(165);
		else
			stack_push(ch);
		break;
	}
	case C_ASSIGN: {
		int c = stack_pop().i;
		int i = stack_pop().i;
		int str = stack_pop().i;
		string_set_char(&heap[str].s, i, c);
		stack_push(c);
		break;
	}
	//
	// --- Structs/Classes ---
	//
	case SR_REF: {
		stack_push(vm_copy_page(heap[stack_pop_var()->i].page));
		break;
	}
	case SR_REF2: {
		stack_push(vm_copy_page(heap[stack_pop().i].page));
		break;
	}
	case SR_POP: {
		heap_unref(stack_pop().i);
		break;
	}
	case SR_ASSIGN: {
		if (instructions[CALLMETHOD].args[0] == T_INT) {
			// Ixseal (System 4 v11+): struct-member struct-copy assignment.
			// The lvalue is expressed as a TWO-slot member reference
			// (struct page + member index) — NOT a pre-resolved struct slot —
			// and there is NO trailing struct-type slot. Codegen for an
			// embedded-struct member `this.member[m] = src`:
			//   PUSHSTRUCTPAGE; PUSH <m>; <src struct page>; SR_ASSIGN; DELETE
			// Interpreting this with the legacy [lval,rval,struct_type] layout
			// pops (src, m, this) and does heap_struct_assign(this, m), which
			// overwrites the WHOLE parent struct (e.g. a CASFont) with a copy
			// of some unrelated heap[m] page and drops the real value — that is
			// what turned CASFont slot 53 into an empty int array and crashed
			// CTextParts@Font::set with `323/2`. Resolve the destination as
			// this.member[m] (an embedded struct slot) and copy src into it.
			// The pushed src is the caller's temporary (A_REF/NEW) copy, which
			// the following DELETE frees; heap_struct_assign deep-copies it, so
			// there is no double free. Older releases still push
			// [lval, rval, struct_type] with the lvalue pre-resolved via
			// SR_REF/REF (handled below, unchanged — e.g. Escalayer v6).
			int src = stack_pop().i;
			int member = stack_pop().i;
			int dst_page = stack_pop().i;
			if (unlikely(!heap_index_valid(dst_page) || !heap[dst_page].page
					|| member < 0 || member >= heap[dst_page].page->nr_vars))
				VM_ERROR("SR_ASSIGN: bad member ref %d/%d", dst_page, member);
			heap_struct_assign(heap[dst_page].page->values[member].i, src);
			stack_push(src);
			break;
		}
		if (ain->version > 1)
			stack_pop(); // struct type
		int rval = stack_pop().i;
		int lval = stack_pop().i;
		heap_struct_assign(lval, rval);
		stack_push(rval);
		break;
	}
	//
	// -- Arrays --
	//
	case A_ALLOC: {
		int struct_type;
		int rank = stack_pop().i;
		int varno = stack_peek(rank).i;
		int pageno = stack_peek(rank+1).i;
		int array = heap[pageno].page->values[varno].i;
		enum ain_data_type data_type = variable_type(heap[pageno].page, varno, &struct_type, NULL);
		if (heap[array].page) {
			delete_page_vars(heap[array].page);
			free_page(heap[array].page);
		}
		heap_set_page(array, alloc_array(rank, stack_peek_ptr(rank-1), data_type, struct_type, true));
		stack_ptr -= rank + 2;
		break;
	}
	case A_REALLOC: {
		int struct_type;
		int rank = stack_pop().i; // rank
		int varno = stack_peek(rank).i;
		int pageno = stack_peek(rank+1).i;
		int array = heap[pageno].page->values[varno].i;
		enum ain_data_type data_type = variable_type(heap[pageno].page, varno, &struct_type, NULL);
		heap_set_page(array, realloc_array(heap[array].page, rank, stack_peek_ptr(rank-1), data_type, struct_type, true));
		stack_ptr -= rank + 2;
		break;
	}
	case A_FREE: {
		int array = stack_pop_var()->i;
		if (heap[array].page) {
			delete_page_vars(heap[array].page);
			free_page(heap[array].page);
			heap_set_page(array, NULL);
		}
		break;
	}
	case A_REF: {
		int array = stack_pop().i;
		/* Ixseal (System 4 v14+) reuses A_REF as a polymorphic "duplicate
		 * reference value": the operand may be a string heap slot (e.g. a
		 * string function argument) as well as an array page. Branch on the
		 * actual heap slot type so we don't cast a struct string* to a page. */
		if (heap_index_valid(array) && heap[array].type == VM_STRING) {
			stack_push(vm_string_ref(heap[array].s));
		} else {
			int slot = heap_alloc_slot(VM_PAGE);
			heap_set_page(slot, copy_page(heap_index_valid(array) ? heap[array].page : NULL));
			stack_push(slot);
		}
		break;
	}
	case A_NUMOF: {
		int rank = stack_pop().i; // rank
		int array = stack_pop_var()->i;
		stack_push(array_numof(heap[array].page, rank));
		break;
	}
	case A_COPY: {
		int n = stack_pop().i;
		int src_i = stack_pop().i;
		int src = stack_pop().i;
		int dst_i = stack_pop().i;
		int dst = stack_pop_var()->i;
		array_copy(heap[dst].page, dst_i, heap[src].page, src_i, n);
		stack_push(n);
		break;
	}
	case A_FILL: {
		union vm_value val = stack_pop();
		int n = stack_pop().i;
		int i = stack_pop().i;
		int array = stack_pop_var()->i;
		stack_push(array_fill(heap[array].page, i, n, val));
		break;
	}
	case A_PUSHBACK: {
		int struct_type;
		union vm_value val = stack_pop();
		int varno = stack_pop().i;
		int pageno = stack_pop().i;
		int array = heap[pageno].page->values[varno].i;
		enum ain_data_type data_type = variable_type(heap[pageno].page, varno, &struct_type, NULL);
		heap_set_page(array, array_pushback(heap[array].page, val, data_type, struct_type));
		break;
	}
	case A_POPBACK: {
		int array = stack_pop_var()->i;
		heap_set_page(array, array_popback(heap[array].page));
		break;
	}
	case A_EMPTY: {
		struct page *array = heap_get_page(stack_pop_var()->i);
		stack_push(!array || !array->nr_vars);
		break;
	}
	case A_ERASE: {
		int i = stack_pop().i;
		int array = stack_pop_var()->i;
		bool success = false;
		heap_set_page(array, array_erase(heap[array].page, i, &success));
		stack_push(success);
		break;
	}
	case A_INSERT: {
		int struct_type;
		union vm_value val = stack_pop();
		int i = stack_pop().i;
		int varno = stack_pop().i;
		int pageno = stack_pop().i;
		int array = heap[pageno].page->values[varno].i;
		enum ain_data_type data_type = variable_type(heap[pageno].page, varno, &struct_type, NULL);
		heap_set_page(array, array_insert(heap[array].page, i, val, data_type, struct_type));
		break;
	}
	case A_SORT: {
		int fno = stack_pop().i;
		int array = stack_pop_var()->i;
		array_sort(heap[array].page, fno);
		break;
	}
	case A_FIND: {
		int fno = stack_pop().i;
		union vm_value v = stack_pop();
		int end = stack_pop().i;
		int start = stack_pop().i;
		struct page *array = heap_get_page(stack_pop_var()->i);
		stack_push(array_find(array, start, end, v, fno));
		// FIXME: string key isn't freed if array is empty
		if (array && array_type(array->a_type) == AIN_STRING) {
			heap_unref(v.i);
		}
		break;
	}
	case A_REVERSE: {
		int array = stack_pop_var()->i;
		array_reverse(heap[array].page);
		break;
	}
	//
	// -- Shorthand Instructions (added in Alice 2010) ---
	//
	case SH_SR_ASSIGN: {
		int rval = stack_pop_var()->i;
		int lval = stack_pop().i;
		heap_struct_assign(lval, rval);
		break;
	}
	case SH_MEM_ASSIGN_LOCAL: {
		member_set(get_argument(0), local_get(get_argument(1)).i);
		break;
	}
	case A_NUMOF_GLOB_1: {
		int array = global_get(get_argument(0)).i;
		stack_push(array_numof(heap_get_page(array), 1));
		break;
	}
	case A_NUMOF_STRUCT_1: {
		int array = member_get(get_argument(0)).i;
		stack_push(array_numof(heap_get_page(array), 1));
		break;
	}
	case SH_MEM_ASSIGN_IMM: {
		member_set(get_argument(0), get_argument(1));
		break;
	}
	case SH_LOCALREFREF: {
		stack_push(local_get(get_argument(0)));
		stack_push(local_get(get_argument(0)+1));
		break;
	}
	case SH_LOCALASSIGN_SUB_IMM: {
		int n = get_argument(0);
		local_set(n, local_get(n).i - get_argument(1));
		break;
	}
	case SH_IF_LOC_LT_IMM: {
		if (local_get(get_argument(0)).i < get_argument(1))
			instr_ptr = get_argument(2);
		else
			instr_ptr += instruction_width(SH_IF_LOC_LT_IMM);
		break;
	}
	case SH_IF_LOC_GE_IMM: {
		if (local_get(get_argument(0)).i >= get_argument(1))
			instr_ptr = get_argument(2);
		else
			instr_ptr += instruction_width(SH_IF_LOC_GE_IMM);
		break;
	}
	case SH_LOCREF_ASSIGN_MEM: {
		struct page *page = heap_get_page(local_get(get_argument(0)).i);
		int index = local_get(get_argument(0)+1).i;
		page_set_var(page, index, member_get(get_argument(1)));
		break;
	}
	case PAGE_REF: {
		struct page *page = heap_get_page(stack_pop().i);
		stack_push(page_get_var(page, get_argument(0)));
		break;
	}
	case SH_GLOBAL_ASSIGN_LOCAL: {
		global_set(get_argument(0), local_get(get_argument(1)), true);
		break;
	}
	case SH_STRUCTREF_GT_IMM: {
		stack_push(member_get(get_argument(0)).i > get_argument(1) ? 1 : 0);
		break;
	}
	case SH_STRUCT_ASSIGN_LOCALREF_ITOB: {
		member_set(get_argument(0), !!local_get(get_argument(1)).i);
		break;
	}
	case SH_LOCAL_ASSIGN_STRUCTREF: {
		local_set(get_argument(0), member_get(get_argument(1)).i);
		break;
	}
	case SH_IF_STRUCTREF_NE_LOCALREF: {
		if (member_get(get_argument(0)).i != local_get(get_argument(1)).i)
			instr_ptr = get_argument(2);
		else
			instr_ptr += instruction_width(SH_IF_STRUCTREF_NE_LOCALREF);
		break;
	}
	case SH_IF_STRUCTREF_GT_IMM: {
		if (member_get(get_argument(0)).i > get_argument(1))
			instr_ptr = get_argument(2);
		else
			instr_ptr += instruction_width(SH_IF_STRUCTREF_GT_IMM);
		break;
	}
	case SH_STRUCTREF_CALLMETHOD_NO_PARAM: {
		int memb_page = member_get(get_argument(0)).i;
		function_call(get_argument(1), instr_ptr + instruction_width(SH_STRUCTREF_CALLMETHOD_NO_PARAM));
		set_struct_page(memb_page);
		break;
	}
	case SH_STRUCTREF2: {
		int memb = member_get(get_argument(0)).i;
		stack_push(page_get_var(heap_get_page(memb), get_argument(1)));
		break;
	}
	case SH_REF_STRUCTREF2: {
		int page = stack_pop().i;
		int memb = page_get_var(heap_get_page(page), get_argument(0)).i;
		stack_push(page_get_var(heap_get_page(memb), get_argument(1)));
		break;
	}
	case SH_STRUCTREF3: {
		int memb0 = member_get(get_argument(0)).i;
		int memb1 = page_get_var(heap_get_page(memb0), get_argument(1)).i;
		stack_push(page_get_var(heap_get_page(memb1), get_argument(2)));
		break;
	}
	case SH_STRUCTREF2_CALLMETHOD_NO_PARAM: {
		int memb1 = member_get(get_argument(0)).i;
		int memb2 = page_get_var(heap_get_page(memb1), get_argument(1)).i;
		function_call(get_argument(2), instr_ptr + instruction_width(SH_STRUCTREF2_CALLMETHOD_NO_PARAM));
		set_struct_page(memb2);
		break;
	}
	case SH_IF_STRUCTREF_Z: {
		if (!member_get(get_argument(0)).i)
			instr_ptr = get_argument(1);
		else
			instr_ptr += instruction_width(SH_IF_STRUCTREF_Z);
		break;
	}
	case SH_IF_STRUCT_A_NOT_EMPTY: {
		struct page *array = heap_get_page(member_get(get_argument(0)).i);
		if (array && array->nr_vars)
			instr_ptr = get_argument(1);
		else
			instr_ptr += instruction_width(SH_IF_STRUCT_A_NOT_EMPTY);
		break;
	}
	case SH_IF_LOC_GT_IMM: {
		if (local_get(get_argument(0)).i > get_argument(1))
			instr_ptr = get_argument(2);
		else
			instr_ptr += instruction_width(SH_IF_LOC_GT_IMM);
		break;
	}
	case SH_IF_STRUCTREF_NE_IMM: {
		if (member_get(get_argument(0)).i != get_argument(1))
			instr_ptr = get_argument(2);
		else
			instr_ptr += instruction_width(SH_IF_STRUCTREF_NE_IMM);
		break;
	}
	case THISCALLMETHOD_NOPARAM: {
		int this_page = struct_page_slot();
		function_call(get_argument(0), instr_ptr + instruction_width(THISCALLMETHOD_NOPARAM));
		set_struct_page(this_page);
		break;
	}
	case SH_IF_LOC_NE_IMM: {
		if (local_get(get_argument(0)).i != get_argument(1))
			instr_ptr = get_argument(2);
		else
			instr_ptr += instruction_width(SH_IF_LOC_NE_IMM);
		break;
	}
	case SH_IF_STRUCTREF_EQ_IMM: {
		if (member_get(get_argument(0)).i == get_argument(1))
			instr_ptr = get_argument(2);
		else
			instr_ptr += instruction_width(SH_IF_STRUCTREF_EQ_IMM);
		break;
	}
	case SH_GLOBAL_ASSIGN_IMM: {
		global_set(get_argument(0), (union vm_value) { .i = get_argument(1) }, false);
		break;
	}
	case SH_LOCALSTRUCT_ASSIGN_IMM: {
		struct page *page = heap_get_page(local_get(get_argument(0)).i);
		page_set_var(page, get_argument(1), get_argument(2));
		break;
	}
	case SH_STRUCT_A_PUSHBACK_LOCAL_STRUCT: {
		int struct_type;
		int array = member_get(get_argument(0)).i;
		union vm_value val = vm_copy(local_get(get_argument(1)), AIN_STRUCT);
		enum ain_data_type data_type = variable_type(struct_page(), get_argument(0), &struct_type, NULL);
		heap_set_page(array, array_pushback(heap_get_page(array), val, data_type, struct_type));
		break;
	}
	case SH_GLOBAL_A_PUSHBACK_LOCAL_STRUCT: {
		int struct_type;
		int array = global_get(get_argument(0)).i;
		union vm_value val = vm_copy(local_get(get_argument(1)), AIN_STRUCT);
		enum ain_data_type data_type = variable_type(global_page(), get_argument(0), &struct_type, NULL);
		heap_set_page(array, array_pushback(heap_get_page(array), val, data_type, struct_type));
		break;
	}
	case SH_LOCAL_A_PUSHBACK_LOCAL_STRUCT: {
		int struct_type;
		int array = local_get(get_argument(0)).i;
		union vm_value val = vm_copy(local_get(get_argument(1)), AIN_STRUCT);
		enum ain_data_type data_type = variable_type(local_page(), get_argument(0), &struct_type, NULL);
		heap_set_page(array, array_pushback(heap_get_page(array), val, data_type, struct_type));
		break;
	}
	case SH_IF_SREF_NE_STR0: {
		struct string *a = heap_get_string(stack_pop_var()->i);
		struct string *b = ain->strings[get_argument(0)];
		if (strcmp(a->text, b->text))
			instr_ptr = get_argument(1);
		else
			instr_ptr += instruction_width(SH_IF_SREF_NE_STR0);
		break;
	}
	case SH_S_ASSIGN_REF: {
		int rval = stack_pop_var()->i;
		int lval = stack_pop().i;
		heap_string_assign(lval, heap_get_string(rval));
		break;
	}
	case SH_A_FIND_SREF: {
		union vm_value *v = stack_pop_var();
		int end = stack_pop().i;
		int start = stack_pop().i;
		int array = stack_pop_var()->i;
		stack_push(array_find(heap_get_page(array), start, end, *v, 0));
		break;
	}
	case SH_SREF_EMPTY: {
		stack_push(!heap_get_string(stack_pop_var()->i)->size);
		break;
	}
	case SH_STRUCTSREF_EQ_LOCALSREF: {
		struct string *a = heap_get_string(member_get(get_argument(0)).i);
		struct string *b = heap_get_string(local_get(get_argument(1)).i);
		stack_push(!strcmp(a->text, b->text));
		break;
	}
	case SH_LOCALSREF_EQ_STR0: {
		struct string *a = heap_get_string(local_get(get_argument(0)).i);
		struct string *b = ain->strings[get_argument(1)];
		stack_push(!strcmp(a->text, b->text));
		break;
	}
	case SH_STRUCTSREF_NE_LOCALSREF: {
		struct string *a = heap_get_string(member_get(get_argument(0)).i);
		struct string *b = heap_get_string(local_get(get_argument(1)).i);
		stack_push(!!strcmp(a->text, b->text));
		break;
	}
	case SH_LOCALSREF_NE_STR0: {
		struct string *a = heap_get_string(local_get(get_argument(0)).i);
		struct string *b = ain->strings[get_argument(1)];
		stack_push(!!strcmp(a->text, b->text));
		break;
	}
	case SH_STRUCT_SR_REF: {
		int sr = member_get(get_argument(0)).i;
		stack_push(vm_copy_page(heap_get_page(sr)));
		// NOTE: argument 1 (struct type) not used
		break;
	}
	case SH_STRUCT_S_REF: {
		int str = member_get(get_argument(0)).i;
		stack_push_string(string_ref(heap_get_string(str)));
		break;
	}
	case S_REF2: {
		struct page *page = heap_get_page(stack_pop().i);
		struct string *s = heap_get_string(page_get_var(page, get_argument(0)).i);
		stack_push_string(string_ref(s));
		break;
	}
	case SH_REF_LOCAL_ASSIGN_STRUCTREF2: {
		struct page *memb = heap_get_page(member_get(get_argument(0)).i);
		int page = local_get(get_argument(1)).i;
		int var = local_get(get_argument(1) + 1).i;
		page_set_var(heap_get_page(page), var, page_get_var(memb, get_argument(2)));
		break;
	}
	case SH_GLOBAL_S_REF: {
		int str = global_get(get_argument(0)).i;
		stack_push_string(string_ref(heap_get_string(str)));
		break;
	}
	case SH_LOCAL_S_REF: {
		int str = local_get(get_argument(0)).i;
		stack_push_string(string_ref(heap_get_string(str)));
		break;
	}
	case SH_LOCALREF_SASSIGN_LOCALSREF: {
		int lval = local_get(get_argument(0)).i;
		int rval = local_get(get_argument(1)).i;
		heap_string_assign(lval, heap_get_string(rval));
		break;
	}
	case SH_LOCAL_APUSHBACK_LOCALSREF: {
		int array = local_get(get_argument(0)).i;
		union vm_value val = vm_copy(local_get(get_argument(1)), AIN_STRING);
		heap_set_page(array, array_pushback(heap_get_page(array), val, AIN_ARRAY_STRING, -1));
		break;
	}
	case SH_S_ASSIGN_CALLSYS19: {
		struct string *name = get_func_stack_name(stack_pop().i);
		heap_string_assign(stack_pop().i, name);
		free_string(name);
		break;
	}
	case SH_S_ASSIGN_STR0: {
		int lval = stack_pop().i;
		heap_string_assign(lval, ain->strings[get_argument(0)]);
		break;
	}
	case SH_SASSIGN_LOCALSREF: {
		int lval = stack_pop().i;
		struct string *rval = heap_get_string(local_get(get_argument(0)).i);
		heap_string_assign(lval, rval);
		break;
	}
	case SH_STRUCTREF_SASSIGN_LOCALSREF: {
		int lval = member_get(get_argument(0)).i;
		int rval = local_get(get_argument(1)).i;
		heap_string_assign(lval, heap_get_string(rval));
		break;
	}
	case SH_LOCALSREF_EMPTY: {
		stack_push(!heap_get_string(local_get(get_argument(0)).i)->size);
		break;
	}
	case SH_GLOBAL_APUSHBACK_LOCALSREF: {
		int array = global_get(get_argument(0)).i;
		union vm_value val = vm_copy(local_get(get_argument(1)), AIN_STRING);
		heap_set_page(array, array_pushback(heap_get_page(array), val, AIN_ARRAY_STRING, -1));
		break;
	}
	case SH_STRUCT_APUSHBACK_LOCALSREF: {
		int array = member_get(get_argument(0)).i;
		union vm_value val = vm_copy(local_get(get_argument(1)), AIN_STRING);
		heap_set_page(array, array_pushback(heap_get_page(array), val, AIN_ARRAY_STRING, -1));
		break;
	}
	case SH_STRUCTSREF_EMPTY: {
		stack_push(!heap_get_string(member_get(get_argument(0)).i)->size);
		break;
	}
	case SH_GLOBALSREF_EMPTY: {
		stack_push(!heap_get_string(global_get(get_argument(0)).i)->size);
		break;
	}
	case SH_SASSIGN_STRUCTSREF: {
		int lval = stack_pop().i;
		int rval = member_get(get_argument(0)).i;
		heap_string_assign(lval, heap_get_string(rval));
		break;
	}
	case SH_SASSIGN_GLOBALSREF: {
		int lval = stack_pop().i;
		int rval = global_get(get_argument(0)).i;
		heap_string_assign(lval, heap_get_string(rval));
		break;
	}
	case SH_STRUCTSREF_NE_STR0: {
		struct string *a = heap_get_string(member_get(get_argument(0)).i);
		struct string *b = ain->strings[get_argument(1)];
		stack_push(!!strcmp(a->text, b->text));
		break;
	}
	case SH_GLOBALSREF_NE_STR0: {
		struct string *a = heap_get_string(global_get(get_argument(0)).i);
		struct string *b = ain->strings[get_argument(1)];
		stack_push(!!strcmp(a->text, b->text));
		break;
	}
	case SH_LOC_LT_IMM_OR_LOC_GE_IMM: {
		int i = local_get(get_argument(0)).i;
		stack_push(i < get_argument(1) || i >= get_argument(2));
		break;
	}
	case A_SORT_MEM: {
		int mno = stack_pop().i;
		int array = stack_pop_var()->i;
		array_sort_mem(heap[array].page, mno);
		break;
	}
	case DG_SET: {
		int fun = stack_pop().i;
		int obj = stack_pop().i;
		int dg_i = stack_pop().i;
		delete_page(dg_i);
		heap_set_page(dg_i, delegate_new_from_method(obj, fun));
		break;
	}
	case DG_ADD: {
		int fun = stack_pop().i;
		int obj = stack_pop().i;
		int dg_i = stack_pop().i;
		struct page *dg = heap_get_delegate_page(dg_i);
		heap_set_page(dg_i, delegate_append(dg, obj, fun));
		break;
	}
	case DG_CALL: { // DG_TYPE, ADDR
		delegate_call(get_argument(0), get_argument(1));
		break;
	}
	case DG_NUMOF: {
		int dg = stack_pop().i;
		stack_push(delegate_numof(heap_get_delegate_page(dg)));
		break;
	}
	case DG_EXIST: {
		int fun = stack_pop().i;
		int obj = stack_pop().i;
		int dg_i = stack_pop().i;
		stack_push(delegate_contains(heap_get_delegate_page(dg_i), obj, fun));
		break;
	}
	case DG_ERASE: {
		int fun = stack_pop().i;
		int obj = stack_pop().i;
		int dg_i = stack_pop().i;
		delegate_erase(heap_get_delegate_page(dg_i), obj, fun);
		break;
	}
	case DG_CLEAR: {
		int slot = stack_pop().i;
		if (!slot)
			break;
		heap_set_page(slot, delegate_clear(heap_get_delegate_page(slot)));
		break;
	}
	case DG_COPY: {
		stack_push(vm_copy_page(heap_get_delegate_page(stack_pop().i)));
		break;
	}
	case DG_ASSIGN: {
		if (instructions[CALLMETHOD].args[0] == T_INT) {
			// Ixseal: как у DG_PLUSA/S_ASSIGN/SR_ASSIGN, lvalue — ДВУСЛОТОВАЯ
			// ссылка (страница, член), а не разыменованный слот делegate-страницы
			// (все 51 сайт Dohna: `...X_REF 1; PUSH <член>; ...; A_REF;
			// DG_ASSIGN; DELETE`; у v7 перед DG_ASSIGN всегда классический REF).
			// Классический путь снимал (rval, член), т.е. считал НОМЕР ЧЛЕНА
			// heap-слотом страницы, и оставлял лишний слот на стеке: цепочка
			// AddPartsUpdateEvent → AddEndUpdateEvent → RCASTimerManager@0
			// возвращалась с +1, и X_OP_SET в RCASTimerManager::Instance читал
			// сдвинутую ссылку (`Out of bounds page index: 210/528`).
			// Пустой делегат-lvalue хранит 0 и страницы ещё не имеет.
			int set_i = stack_pop().i;
			union vm_value *dst = stack_pop_var();
			struct page *new_dg = copy_page(heap_get_delegate_page(set_i));
			if (dst->i > 0 && heap_index_valid(dst->i) && heap[dst->i].page &&
			    heap[dst->i].page->type == DELEGATE_PAGE) {
				delete_page(dst->i);
				heap_set_page(dst->i, new_dg);
			} else {
				dst->i = heap_alloc_page(new_dg);
			}
			stack_push(set_i);
			break;
		}
		int set_i = stack_pop().i;
		int dst_i = stack_pop().i;
		struct page *set = heap_get_delegate_page(set_i);
		struct page *new_dg = copy_page(set);
		delete_page(dst_i);
		heap_set_page(dst_i, new_dg);
		stack_push(set_i);
		break;
	}
	case DG_PLUSA: {
		if (instructions[CALLMETHOD].args[0] == T_INT) {
			// Ixseal (ain v>=11): the destination is a 2-slot ref-lvalue
			// (heap_idx, member_idx) to a delegate variable; the value to add
			// is on top. Idiom in a closure body:
			//   PUSHLOCALPAGE; PUSH n;          <- ref to the lvalue delegate
			//   ...compute add-delegate...; A_REF
			//   DG_PLUSA
			// An empty delegate lvalue holds 0 and has no page yet, so we must
			// allocate a fresh one instead of dereferencing slot 0.
			int add_i = stack_pop().i;
			union vm_value *dst = stack_pop_var();
			struct page *add = heap_get_delegate_page(add_i);
			if (dst->i > 0 && heap_index_valid(dst->i) && heap[dst->i].page &&
			    heap[dst->i].page->type == DELEGATE_PAGE) {
				heap_set_page(dst->i, delegate_plusa(heap[dst->i].page, add));
			} else {
				dst->i = heap_alloc_page(delegate_plusa(NULL, add));
			}
			// `dg += x` is an expression: push the added value back as the
			// rvalue, exactly like the legacy form (libsys4 types DG_PLUSA as
			// (T_PAGE, T_PAGE) -> (T_PAGE)). Closures that wrap the result in an
			// option rely on this slot: e.g. `... DG_PLUSA; PUSH 0; RETURN`
			// builds a two-slot AIN_OPTION [added_delegate, tag]. Suppressing
			// the push made such lambdas return one slot too few, which only
			// surfaced as a corrupted ref much later (CSpriteParts@0 ctor). The
			// added page is a caller-owned temporary (delegate_plusa copies from
			// it without taking ownership), so the eventual DELETE frees it.
			stack_push(add_i);
			break;
		}
		int add_i = stack_pop().i;
		int dst_i = stack_pop().i;
		struct page *add = heap_get_delegate_page(add_i);
		struct page *dst = heap_get_delegate_page(dst_i);
		heap_set_page(dst_i, delegate_plusa(dst, add));
		stack_push(add_i);
		break;
	}
	case DG_MINUSA: {
		if (instructions[CALLMETHOD].args[0] == T_INT) {
			// Ixseal: приёмник — такая же ДВУСЛОТОВАЯ ссылка-lvalue
			// (heap-слот, номер члена), как у DG_PLUSA выше. Сайт
			// `parts::detail::RemovePartsUpdateEvent` @0x2ead24:
			//   PUSHLOCALPAGE; PUSH 4; X_REF 1   <- страница `data`
			//   PUSH 1                           <- номер члена (делегат), БЕЗ REF
			//   ...вычислить вычитаемый делегат...; A_REF
			//   DG_MINUSA; DELETE
			// Легаси-ветка снимала номер члена как heap-индекс и звала
			// heap_get_delegate_page(1) → `Not a delegate page: 1`.
			int minus_i = stack_pop().i;
			union vm_value *dst = stack_pop_var();
			struct page *minus = heap_get_delegate_page(minus_i);
			// Пустой приёмник (делегату ещё не назначали обработчиков) —
			// вычитать не из чего, оставляем как есть.
			if (dst->i > 0 && heap_index_valid(dst->i) && heap[dst->i].page &&
			    heap[dst->i].page->type == DELEGATE_PAGE) {
				heap_set_page(dst->i, delegate_minusa(heap[dst->i].page, minus));
			}
			// `dg -= x` — выражение: возвращаем вычитаемое (сайт освобождает
			// его следующим DELETE), ровно как у DG_PLUSA.
			stack_push(minus_i);
			break;
		}
		int minus_i = stack_pop().i;
		int dst_i = stack_pop().i;
		struct page *minus = heap_get_delegate_page(minus_i);
		struct page *dst = heap_get_delegate_page(dst_i);
		heap_set_page(dst_i, delegate_minusa(dst, minus));
		stack_push(minus_i);
		break;
	}
	case DG_POP: {
		heap_unref(stack_pop().i);
		break;
	}
	case DG_NEW_FROM_METHOD: {
		int fun = stack_pop().i;
		int obj = stack_pop().i;
		stack_push(heap_alloc_page(delegate_new_from_method(obj, fun)));
		break;
	}
	case DG_CALLBEGIN: { // DG_TYPE
		int dg_no = get_argument(0);
		if (dg_no < 0 || dg_no >= ain->nr_delegates)
			VM_ERROR("Invalid delegate index");
		struct ain_function_type *dg = &ain->delegates[dg_no];

		// Stack before: [dg_page, arg0, ...]
		// Stack after:  [arg0, ..., dg_page, 0(dg_index)]
		int dg_page = stack_peek(dg->nr_arguments).i;
		for (int i = 0; i < dg->nr_arguments; i++) {
			int pos = (stack_ptr - dg->nr_arguments) + i;
			stack[pos-1] = stack[pos];
		}
		stack[stack_ptr-1].i = dg_page;
		stack_push(0);

		// Push one dummy per return slot so DG_CALL can replace them. An
		// AIN_OPTION return (Ixseal option, e.g. DG_DeletedHandler?) occupies
		// two slots, so two dummies are needed; must stay in lock-step with the
		// return_values count used by delegate_call.
		for (int i = 0; i < dg_return_slots(dg_no); i++)
			stack_push(0);
		break;
	}
	case DG_NEW: {
		stack_push(heap_alloc_page(alloc_page(DELEGATE_PAGE, 0, 0)));
		break;
	}
	case DG_STR_TO_METHOD: {
		// Ixseal passes the delegate type as an operand rather than on the stack.
		int dg_no = instructions[DG_STR_TO_METHOD].nr_args >= 1 ? get_argument(0) : stack_pop().i;
		int str = stack_pop().i;
		int fno = get_function_by_name(heap_get_string(str)->text);
		stack_push(function_matches_delegate(dg_no, fno) ? fno : 0);
		heap_unref(str);
		break;
	}
	// -- NOOPs ---
	case FUNC:
		break;
	case CHECKUDO:
		// System 4: снимает со стека 1 значение (старый объект-ссылку перед
		// переприсваиванием, паттерн `lhs = obj.method()`). Раньше принимали за
		// no-op — стек сдвигался, метод получал неверный this (-1). Значение не
		// разыменовываем/не unref'аем (может быть null или не владеющая ссылка).
		stack_pop();
		break;
	default:
#ifdef DEBUGGER_ENABLED
		if ((opcode & OPTYPE_MASK) == BREAKPOINT) {
			dbg_handle_breakpoint();
			return execute_instruction(opcode & ~OPTYPE_MASK);
		}
#endif
		VM_ERROR("Illegal opcode: 0x%04x", opcode);
	}
	return opcode;
}

// Ground-truth opcode ring buffer (env XSYS4_OPTRACE): the disassembler
// desyncs on the newer variable-width opcodes, so record the actually-executed
// (ip, opcode, stack_ptr) here and dump the tail on a VM error.
#define OPTRACE_SIZE 128
struct optrace_entry { uint32_t ip; uint16_t op; int32_t sp; };
static struct optrace_entry optrace_ring[OPTRACE_SIZE];
static uint32_t optrace_pos;
static int optrace_on = -1;
static bool optrace_underflow_logged = false;

void vm_optrace_dump(void)
{
	if (optrace_on <= 0)
		return;
	sys_warning("=== last %d executed opcodes (ip opcode sp) ===\n", OPTRACE_SIZE);
	for (int k = 0; k < OPTRACE_SIZE; k++) {
		struct optrace_entry *e = &optrace_ring[(optrace_pos + k) % OPTRACE_SIZE];
		if (!e->ip && !e->op)
			continue;
		sys_warning("  0x%06x  %-16s sp=%d\n", e->ip,
			    instructions[e->op].name ? instructions[e->op].name : "?", e->sp);
	}
}

static void vm_execute(void)
{
	if (optrace_on < 0) {
		optrace_on = getenv("XSYS4_OPTRACE") ? 1 : 0;
		sp_check = getenv("XSYS4_SP_CHECK") != NULL;
	}
	for (;;) {
		uint16_t opcode;
		if (instr_ptr == VM_RETURN)
			return;
		if (unlikely(instr_ptr >= ain->code_size)) {
			VM_ERROR("Illegal instruction pointer: 0x%08lX", instr_ptr);
		}
		opcode = get_opcode(instr_ptr);
		uint32_t rec_ip = instr_ptr;
		uint16_t rec_op = opcode;
		int rec_sp = stack_ptr;
		if (optrace_on > 0) {
			optrace_ring[optrace_pos % OPTRACE_SIZE] =
				(struct optrace_entry){ instr_ptr, opcode, stack_ptr };
			optrace_pos++;
		}
		opcode = execute_instruction(opcode);
		if (optrace_on > 0 && rec_ip >= 0x66a5d0 && rec_ip <= 0x66a600)
			sys_warning("FN6 0x%06x %-14s sp %d->%d\n", rec_ip,
				    instructions[rec_op].name ? instructions[rec_op].name : "?", rec_sp, stack_ptr);
		if (optrace_on > 0 && !optrace_underflow_logged && stack_ptr < 0) {
			optrace_underflow_logged = true;
			sys_warning("=== FIRST STACK UNDERFLOW: after %s @0x%06x (sp %d -> %d) ===\n",
				    instructions[rec_op].name ? instructions[rec_op].name : "?",
				    rec_ip, rec_sp, stack_ptr);
			vm_optrace_dump();
		}
		instr_ptr += instructions[opcode].ip_inc;
	}
}

static void call_global_destructors(void)
{
	if (heap_size <= 0 || heap[0].ref <= 0)
		return;
	struct page *global_page = heap_get_page(0);
	// Call global variable destructors, but do not unref them because the
	// destructors may reference other global variables.
	for (int i = global_page->nr_vars - 1; i >= 0; i--) {
		if (variable_type(global_page, i, NULL, NULL) != AIN_STRUCT)
			continue;
		int slot = global_page->values[i].i;
		delete_struct(heap_get_page(slot)->index, slot);
	}
}

static void vm_free(void)
{
	if (game_dungeons_and_dolls) {
		// Dungeons & Dolls saves the game state in destructors of global variables
		call_global_destructors();
	}

	// call library exit routines
	exit_libraries();
	// flush call stack
	for (int i = call_stack_ptr - 1; i >= 0; i--) {
		if (call_stack[i].struct_page >= 0 && AIN_VERSION_GTE(ain, 6, 1))
			exit_unref(call_stack[i].struct_page);
		exit_unref(call_stack[i].page_slot);
	}
	// free globals
	if (heap_size > 0 && heap[0].ref > 0)
		exit_unref(0);

	vm_reset_once = true;
}

static jmp_buf reset_buf;

_Noreturn void vm_reset(void)
{
	vm_free();
	longjmp(reset_buf, 1);
}

static void vm_sigusr1_handler(int sig)
{
	(void)sig;
	sys_warning("=== SIGUSR1: VM call stack (top first) ===\n");
	vm_stack_trace();
	sys_warning("=== end VM call stack (instr_ptr=0x%zx) ===\n", instr_ptr);
}

int vm_execute_ain(struct ain *program)
{
	ain = program;
	signal(SIGUSR1, vm_sigusr1_handler);
	setjmp(reset_buf);

	// initialize VM state
	if (!stack) {
		stack_size = INITIAL_STACK_SIZE;
		stack = xmalloc(INITIAL_STACK_SIZE * sizeof(union vm_value));
	}
	stack_ptr = 0;
	call_stack_ptr = 0;

	heap_init();
	init_libraries();

	/*
	 * Ixseal (System 4 v11+) сам конструирует struct-глобалы: функция "0"
	 * (`ain->alloc`) для КАЖДОГО из них выполняет `DELETE old; NEW <s>;
	 * X_ASSIGN 1` (проверено по всем 78 struct-глобалам Dohna — четыре из них
	 * получают объект из вызова, напр. `DamageNumber::CreateFont`, но идиома та
	 * же). Поэтому легаси-схема «alloc_struct до, init_struct после» здесь ломает
	 * сразу две вещи:
	 *   - `DELETE old` уносит ПРЕДварительно выделенную страницу, вызывая её
	 *     ДЕСТРУКТОР на объекте, чей конструктор никогда не работал: `CASTimer@1`
	 *     звал `CASTimerManager@ReleaseHandle(handle=0)` до первого CreateHandle
	 *     → 53 игровых ассерта «CASTimerManager - Bundle Error» и путаница в
	 *     учёте хэндлов (ReleaseHandle вызывался чаще CreateHandle);
	 *   - `init_struct` после функции "0" вызывает конструктор ВТОРОЙ раз, уже на
	 *     объекте, построенном игрой: `CGlobalObject@0` проверяет
	 *     `assert( ! m_Created )` (global[141]) и падает.
	 * Легаси-игры (v6/v7) в функции "0" struct-глобалы не конструируют — там
	 * обе фазы обязательны, поэтому гейт структурный (v11+).
	 */
	bool ix_globals_self_ctor = instructions[CALLMETHOD].args[0] == T_INT;

	// Initialize globals
	heap[0].ref = 1;
	heap[0].seq = heap_next_seq++;
	heap_set_page(0, alloc_page(GLOBAL_PAGE, 0, ain->nr_globals));
	for (int i = 0; i < ain->nr_globals; i++) {
		if (ain->globals[i].type.data == AIN_STRUCT) {
			// Ixseal: объекта ещё нет — null-маркер -1 (DELETE его игнорирует;
			// ноль здесь означал бы heap-слот 0, т.е. саму глобальную страницу).
			if (ix_globals_self_ctor) {
				heap[0].page->values[i].i = -1;
				continue;
			}
			// XXX: need to allocate storage for global structs BEFORE calling
			//      constructors.
			heap[0].page->values[i].i = alloc_struct(ain->globals[i].type.struc);
		} else {
			heap[0].page->values[i] = variable_initval_var(heap[0].page, i, ain->globals[i].type.data);
		}
	}
	init_option_vars(heap[0].page, ain->globals, ain->nr_globals, 0);
	for (int i = 0; i < ain->nr_initvals; i++) {
		int32_t index;
		struct ain_initval *v = &ain->global_initvals[i];
		switch (v->data_type) {
		case AIN_STRING:
			index = heap_alloc_slot(VM_STRING);
			heap[0].page->values[v->global_index].i = index;
			heap[index].s = make_string(v->string_value, strlen(v->string_value));
			break;
		default:
			heap[0].page->values[v->global_index].i = v->int_value;
			break;
		}
	}

	if (ain->alloc >= 0)
		vm_call(ain->alloc, -1); // function "0": allocate global arrays

	if (ix_globals_self_ctor) {
		// Конструкторы уже отработали внутри функции "0". Здесь только проверяем
		// допущение: если какой-то struct-глобал остался null, значит функция "0"
		// его НЕ построила и схема для этой игры другая — молчать нельзя.
		int unbuilt = 0;
		for (int i = 0; i < ain->nr_globals; i++) {
			if (ain->globals[i].type.data != AIN_STRUCT)
				continue;
			if (heap[0].page->values[i].i >= 0)
				continue;
			if (++unbuilt <= 4)
				WARNING("struct-глобал g[%d] %s не построен функцией \"0\"",
					i, display_sjis0(ain->globals[i].name));
		}
		if (unbuilt > 4)
			WARNING("...и ещё %d непостроенных struct-глобалов", unbuilt - 4);
	} else {
		// XXX: global constructors must be called AFTER initializing non-struct variables
		//      otherwise a global set in a constructor will be clobbered by its initval
		for (int i = 0; i < ain->nr_globals; i++) {
			if (ain->globals[i].type.data == AIN_STRUCT)
				init_struct(ain->globals[i].type.struc, heap[0].page->values[i].i);
		}
	}

	vm_call(ain->main, -1);
	return stack_pop().i;
}

void vm_stack_trace(void)
{
	for (int i = call_stack_ptr - 1; i >= 0; i--) {
		struct ain_function *f = &ain->functions[call_stack[i].fno];
		uint32_t addr = (i == call_stack_ptr - 1) ? instr_ptr : call_stack[i+1].call_address;
		sys_warning("\t0x%08x in %s\n", addr, display_sjis0(f->name));
	}
}

_Noreturn void _vm_error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sys_vwarning(fmt, ap);
	va_end(ap);
	sys_warning("at %s (0x%X) in:\n", current_instruction_name(), instr_ptr);
	vm_stack_trace();
	vm_optrace_dump();

	char msg[1024];
	va_start(ap, fmt);
	vsnprintf(msg, 1024, fmt, ap);
	va_end(ap);

	dbg_repl(DBG_STOP_ERROR, msg);
	sys_exit(1);
}

int vm_time(void)
{
	return SDL_GetTicks();
}

void vm_sleep(int ms)
{
	SDL_Delay(ms);
}

_Noreturn void vm_exit(int code)
{
	vm_free();
#ifdef DEBUG_HEAP
	for (size_t i = 0; i < heap_size; i++) {
		if (heap[i].ref > 0)
			heap_describe_slot(i);
	}
	sys_message("Number of leaked objects: %d\n", heap_free_ptr);
#endif
	sys_exit(code);
}
