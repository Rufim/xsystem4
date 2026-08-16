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

// enable private VM interface
#define VM_PRIVATE

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "cJSON.h"

#include "system4.h"
#include "system4/file.h"
#include "system4/savefile.h"
#include "system4/string.h"

#include "savedata.h"
#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "xsystem4.h"

/*
 * Save/load VM images as JSON.
 */

static const char * const page_type_strtab[] = {
	[GLOBAL_PAGE]   = "globals",
	[LOCAL_PAGE]    = "locals",
	[STRUCT_PAGE]   = "struct",
	[ARRAY_PAGE]    = "array",
	[DELEGATE_PAGE] = "delegate"
};

static enum page_type string_to_page_type(const char *str)
{
	if (!strcmp(str, "globals"))  return GLOBAL_PAGE;
	if (!strcmp(str, "locals"))   return LOCAL_PAGE;
	if (!strcmp(str, "struct"))   return STRUCT_PAGE;
	if (!strcmp(str, "array"))    return ARRAY_PAGE;
	if (!strcmp(str, "delegate")) return DELEGATE_PAGE;
	VM_ERROR("Invalid page type: %s", str);
}

static cJSON *value_to_json(union vm_value v)
{
	return cJSON_CreateNumber(v.i);
}

static int get_number(int i, void *data)
{
	return ((union vm_value*)data)[i].i;
}

static cJSON *resume_page_to_json(struct page *page)
{
	if (!page)
		return cJSON_CreateNull();

	cJSON *json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "type", page_type_strtab[page->type]);
	cJSON_AddNumberToObject(json, "subtype", page->index);
	if (page->type == ARRAY_PAGE) {
		cJSON_AddNumberToObject(json, "struct-type", page->array.struct_type);
		cJSON_AddNumberToObject(json, "rank", page->array.rank);
	}

	cJSON *values = cJSON_CreateIntArray_cb(page->nr_vars, get_number, page->values);

	cJSON_AddItemToObject(json, "values", values);
	return json;
}

static cJSON *heap_item_to_json(int i, possibly_unused void *_)
{
	if (!heap[i].ref)
		return NULL;

	cJSON *item = cJSON_CreateArray();
	cJSON_AddItemToArray(item, cJSON_CreateNumber(i));
	cJSON_AddItemToArray(item, cJSON_CreateNumber(heap[i].ref));
	switch (heap[i].type) {
	case VM_PAGE:
		cJSON_AddItemToArray(item, resume_page_to_json(heap[i].page));
		break;
	case VM_STRING:
		cJSON_AddItemToArray(item, cJSON_CreateString(heap[i].s->text));
		break;
	}
	if (ain->nr_delegates > 0) {
		cJSON_AddItemToArray(item, cJSON_CreateNumber(heap[i].seq));
	}
	return item;
}

static cJSON *heap_to_json(void)
{
	return cJSON_CreateArray_cb(heap_size, heap_item_to_json, NULL);
}

static cJSON *funcall_to_json(struct function_call *call)
{
	cJSON *json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "function", call->fno);
	cJSON_AddNumberToObject(json, "return-address", call->return_address);
	cJSON_AddNumberToObject(json, "local-page", call->page_slot);
	cJSON_AddNumberToObject(json, "struct-page", call->struct_page);
	return json;
}

static cJSON *call_stack_to_json(void)
{
	cJSON *json = cJSON_CreateArray();
	for (int i = 0; i < call_stack_ptr; i++) {
		cJSON_AddItemToArray(json, funcall_to_json(&call_stack[i]));
	}
	return json;
}

static cJSON *stack_to_json(void)
{
	cJSON *json = cJSON_CreateArray();
	for (int i = 0; i < stack_ptr; i++) {
		cJSON_AddItemToArray(json, value_to_json(stack[i]));
	}
	return json;
}

static cJSON *vm_image_to_json(const char *key)
{
	cJSON *image = cJSON_CreateObject();
	cJSON_AddStringToObject(image, "key", key);
	cJSON_AddItemToObject(image, "heap", heap_to_json());
	cJSON_AddItemToObject(image, "call-stack", call_stack_to_json());
	cJSON_AddItemToObject(image, "stack", stack_to_json());
	cJSON_AddNumberToObject(image, "ip", instr_ptr);
	if (ain->nr_delegates > 0) {
		cJSON_AddNumberToObject(image, "next_seq", heap_next_seq);
	}
	return image;
}

/*
 * Каноничное имя функции для сейва: у новых игр имена перегружены (одна
 * строка у разных функций — у Haha Ranman сотни таких), а фреймы и стек
 * вызовов идентифицируются ИМЕНЕМ. Голое имя на загрузке разрешается в
 * ПЕРВУЮ перегрузку — не обязательно ту, что сохранялась («call frame has
 * too many variables», «CRC mismatch»). Для неоднозначных пишем `имя#N` —
 * эту форму ain_get_function понимает.
 */
/*
 * Имя функции для образа.
 *
 * ★В ВЕРСИИ 14 — С СИГНАТУРОЙ, как оригинал: `main()`,
 * `MapView@Move(MapEdge&, DG_Event)`. Именно сигнатура отличает у System40
 * перегрузки друг от друга; в его образе нет ни одного суффикса `#N`, и наше
 * «Foo#1» он бы просто не нашёл. Формат сверен с образом оригинала: все 285
 * имён, встречающихся в двух его сохранённых образах, воспроизводятся нашим
 * генератором дословно (`tools/fname_check.c`).
 *
 * Версии 9 и старше пишут `имя#N` — так эти образы читались до сих пор, и
 * ломать их незачем.
 */
static char *rsave_function_name_ver(struct ain_function *f, int version)
{
	if (version >= 14)
		return ain_function_signature(ain, f);
	int ord = ain_get_function_index(ain, f);
	if (ord > 0) {
		size_t len = strlen(f->name) + 16;
		char *s = xmalloc(len);
		snprintf(s, len, "%s#%d", f->name, ord);
		return s;
	}
	return strdup(f->name);
}

static struct rsave_heap_frame *frame_page_to_rsave(struct page *page, int slot, int version)
{
	struct rsave_heap_frame *o = xcalloc(1, sizeof(struct rsave_heap_frame) + page->nr_vars * sizeof(int32_t));
	o->ref = heap[slot].ref;
	o->seq = heap[slot].seq;
	struct ain_variable *vars;
	if (page->type == GLOBAL_PAGE) {
		o->tag = RSAVE_GLOBALS;
		o->func.id = -1;
		assert(page->nr_vars == ain->nr_globals);
		vars = ain->globals;
	} else {
		struct ain_function *f = &ain->functions[page->index];
		o->tag = RSAVE_LOCALS;
		o->func.name = rsave_function_name_ver(f, version);
		assert(page->nr_vars == f->nr_vars);
		vars = f->vars;
		o->struct_ptr = page->local.struct_ptr;
		// Второе поле кадра в версии 14: «окружения нет» — это -1, а не ноль
		// (ноль указывал бы на слот 0, то есть на страницу глобалей).
		o->env_ptr = -1;
	}
	o->nr_types = page->nr_vars;
	o->types = xcalloc(o->nr_types, sizeof(int32_t));
	for (int i = 0; i < o->nr_types; i++)
		o->types[i] = vars[i].type.data;
	o->nr_slots = page->nr_vars;
	for (int i = 0; i < o->nr_slots; i++)
		o->slots[i] = page->values[i].i;
	return o;
}

static struct rsave_heap_struct *struct_page_to_rsave(struct page *page, int slot)
{
	struct rsave_heap_struct *o = xcalloc(1, sizeof(struct rsave_heap_struct) + page->nr_vars * sizeof(int32_t));
	o->tag = RSAVE_STRUCT;
	o->ref = heap[slot].ref;
	o->seq = heap[slot].seq;
	struct ain_struct *s = &ain->structures[page->index];
	o->ctor.name = strdup(s->constructor >= 0 ? ain->functions[s->constructor].name : "");
	o->dtor.name = strdup(s->destructor >= 0 ? ain->functions[s->destructor].name : "");
	o->uk = 0;
	/*
	 * Версия 14 вместо ctor/dtor и таблицы типов членов держит список СТРАНИЦ
	 * баз-интерфейсов и хвостовой признак. Мы отдельных страниц под базы не
	 * заводим (интерфейс живёт слотами внутри самого объекта), поэтому пишем
	 * пустой список; при ЧТЕНИИ чужого образа оба поля сохраняются как есть.
	 * ★Открытый хвост: у оригинала список непустой у 818 структур из 43 тысяч.
	 */
	o->nr_ifaces = 0;
	o->tail = 0;
	o->struct_type.name = strdup(s->name);
	o->nr_types = s->nr_members;
	o->types = xcalloc(o->nr_types, sizeof(int32_t));
	for (int i = 0; i < o->nr_types; i++)
		o->types[i] = s->members[i].type.data;
	o->nr_slots = page->nr_vars;
	for (int i = 0; i < o->nr_slots; i++)
		o->slots[i] = page->values[i].i;
	return o;
}

/*
 * Тип элемента массива для версии 14 — деревом. Формы, встречающиеся в образе
 * оригинала (обе его сохранёнки, все 36 тысяч массивов), исчерпываются четырьмя:
 *
 *   10 / 12 / 13 / 47 / 21 / 92    — плоский тип;
 *   82<13>                         — wrap<структура>;
 *   82<100>                        — wrap<ИНТЕРФЕЙС> (100 = AIN_IFACE_WRAP);
 *   86<82<13>>                     — option<wrap<структура>>.
 *
 * Имя структуры повторяется на КАЖДОМ уровне, кроме `option` — у того имени нет
 * вовсе. Больше в дереве ничего нет, поэтому его хватает восстановить из пары
 * (тип элемента страницы, индекс структуры), которую страница массива и хранит.
 */
static struct rsave_array_type *array_type_to_rsave(enum ain_data_type data_type, int struct_type)
{
	const char *name = struct_type >= 0 ? ain->structures[struct_type].name : NULL;
	switch (data_type) {
	case AIN_OPTION:
		// у option имени нет, внутри — wrap над тем же типом
		return rsave_array_type_new(AIN_OPTION, NULL,
					    array_type_to_rsave(AIN_WRAP, struct_type));
	/*
	 * ★`wrap<интерфейс>` приходит сюда типом элемента AIN_IFACE_WRAP (100), а не
	 * AIN_WRAP: маркер AIN_WRAP движок ставит только над структурой
	 * (array_resolve_var_type, src/page.c). Поэтому оба случая ведут в одну
	 * ветку — иначе интерфейсные массивы уходили бы в файл плоским типом `100`
	 * там, где оригинал пишет дерево `82<100>` (в его образе таких 56 штук).
	 */
	case AIN_WRAP:
	case AIN_IFACE_WRAP: {
		bool iface = data_type == AIN_IFACE_WRAP
			|| (struct_type >= 0 && ain->structures[struct_type].is_interface);
		struct rsave_array_type *sub =
			rsave_array_type_new(iface ? AIN_IFACE_WRAP : AIN_STRUCT, name, NULL);
		return rsave_array_type_new(AIN_WRAP, name, sub);
	}
	default:
		return rsave_array_type_new(data_type, name, NULL);
	}
}

static struct rsave_heap_array *array_page_to_rsave(struct page *page, int slot)
{
	struct rsave_heap_array *o = xcalloc(1, sizeof(struct rsave_heap_array) + page->nr_vars * sizeof(int32_t));
	o->tag = RSAVE_ARRAY;
	o->ref = heap[slot].ref;
	o->seq = heap[slot].seq;
	o->rank_minus_1 = page->array.rank - 1;
	o->data_type = page->index;
	if (page->array.struct_type >= 0)
		o->struct_type.name = strdup(ain->structures[page->array.struct_type].name);
	else
		o->struct_type.name = strdup("");
	o->root_rank = page->array.rank;  // FIXME: this is incorrect for subarrays
	o->type = array_type_to_rsave(page->index, page->array.struct_type);
	o->is_not_empty = page->nr_vars ? 1 : 0;
	o->nr_slots = page->nr_vars;
	for (int i = 0; i < o->nr_slots; i++)
		o->slots[i] = page->values[i].i;
	return o;
}

/*
 * В файл делегат пишется в ФОРМАТЕ ИГРЫ — тройками (obj, fun, seq). Четвёртый
 * слот (env, окружение лямбды; см. vm/page.h) внутренний: сохранять его некуда,
 * а главное — heap-слот локальной страницы после загрузки уже ничего не значит.
 * После загрузки env = -1, и X_GETENV откатывается на поиск по стеку вызовов.
 */
/*
 * ★ОКРУЖЕНИЕ ЛЯМБД В ОБРАЗЕ. Формат AliceSoft хранит запись делегата ТРОЙКОЙ
 * (receiver, функция, поколение) — четвёртого слота, нашего `env` (heap-слот
 * локальной страницы объемлющей функции, из которого лямбда читает захваченные
 * переменные), в нём нет. Пока мы его теряли, ломались ДВЕ вещи разом:
 *
 *  - поведение: после загрузки лямбда-обработчик получал env = -1, то есть
 *    исполнялся без захваченного окружения;
 *  - ВЛАДЕНИЕ: `delegate_append` берёт ссылку на страницу окружения
 *    (`delegate_env_ref`), и эта ссылка входит в сохранённый refcount страницы —
 *    а записей, которые её отпустят (`delegate_release_env`), после загрузки
 *    больше нет. Счётчик оставался завышенным НАВСЕГДА.
 *
 * Чем это кончалось у Haha Ranman: страница-окружение конструктора
 * `SaveLoadScene@0` (ref=10 при НОЛЕ реальных держателей — замер `XSYS4_WHO_REFS`)
 * держала аргумент `ref SceneStack`, стек сцен экрана сохранения не умирал, его
 * `EraseLayer` не вызывался — и после загрузки поверх игры навсегда оставался
 * экран сейвов (153 парта слоя 18).
 *
 * Пишем ЧЕТВЁРКАМИ под маркером в нулевом слоте: у AliceSoft там лежит receiver
 * (heap-слот, всегда >= -1), поэтому отрицательный маркер ни с чем не спутать, а
 * чужие сейвы по-прежнему читаются как тройки.
 */
#define RSAVE_DG_ENV_MAGIC (-777)

static struct rsave_heap_delegate *delegate_page_to_rsave(struct page *page, int slot, int version)
{
	int nr_entries = page->nr_vars / DG_ENTRY_SLOTS;

	/*
	 * Версия 14 держит записи делегата отдельным списком, а метод в них назван
	 * ИМЕНЕМ функции. Поколение объекта (`seq`) в этот формат не влезает —
	 * его там нет и у самих объектов, — а вот ОКРУЖЕНИЕ лямбды терять нельзя:
	 * без него после загрузки ломается и поведение, и владение (см. длинный
	 * комментарий выше про RSAVE_DG_ENV_MAGIC). Кладём его в третье поле
	 * записи: у оригинала там −1 во всех 7918 записях из 7919, так что −1
	 * остаётся значением «окружения нет».
	 */
	if (version >= 14) {
		struct rsave_heap_delegate *o = xcalloc(1, sizeof(struct rsave_heap_delegate));
		o->tag = RSAVE_DELEGATE;
		o->ref = heap[slot].ref;
		o->entries = xcalloc(nr_entries, sizeof(struct rsave_delegate_entry));
		/*
		 * ★ПРОТУХШИЕ ПОДПИСКИ В ОБРАЗ НЕ ПИШЕМ.
		 *
		 * Список чистится амортизированно (delegate_compact вызывается при
		 * добавлении и рассылке), поэтому в момент снимка в нём всегда лежат
		 * записи с уже умершими получателями — их и опознаёт сверка поколений
		 * `heap_get_seq(obj) == values[i+2]`. В файл им ехать незачем: своего
		 * поколения формат версии 14 не хранит, и на загрузке такая запись
		 * неотличима от живой (§5fb-10a — из-за этого движок и падал).
		 *
		 * Сколько их: на снимке титула Dohna — 465 протухших против 325 живых,
		 * то есть больше половины содержимого делегатов было мусором.
		 */
		int w = 0, dropped = 0;
		for (int i = 0; i < nr_entries; i++) {
			int obj = page->values[i*DG_ENTRY_SLOTS + 0].i;
			if (heap_get_seq(obj) != (uint32_t)page->values[i*DG_ENTRY_SLOTS + 2].i) {
				dropped++;
				continue;
			}
			int fun = page->values[i*DG_ENTRY_SLOTS + 1].i;
			o->entries[w].obj = obj;
			o->entries[w].method = fun >= 0 && fun < ain->nr_functions
				? rsave_function_name_ver(&ain->functions[fun], version)
				: strdup("");
			o->entries[w].uk = page->values[i*DG_ENTRY_SLOTS + 3].i;
			w++;
		}
		o->nr_entries = w;
		if (dropped && getenv("XSYS4_SAVE_TRACE"))
			NOTICE("SAVETRACE делегат слот %d: записей %d, протухших отброшено %d",
			       slot, w, dropped);
		return o;
	}

	int nr_slots = 1 + nr_entries * 4;
	struct rsave_heap_delegate *o = xcalloc(1, sizeof(struct rsave_heap_delegate) + nr_slots * sizeof(int32_t));
	o->tag = RSAVE_DELEGATE;
	o->ref = heap[slot].ref;
	o->seq = heap[slot].seq;
	o->nr_slots = nr_slots;
	o->slots[0] = RSAVE_DG_ENV_MAGIC;
	for (int i = 0; i < nr_entries; i++) {
		for (int k = 0; k < 4; k++)
			o->slots[1 + i*4 + k] = page->values[i*DG_ENTRY_SLOTS + k].i;
	}
	return o;
}

static void *heap_item_to_rsave(int i, int version)
{
	if (!heap[i].ref)
		return rsave_null;
	if (heap[i].type == VM_STRING) {
		int len = heap[i].s->size + 1;
		struct rsave_heap_string *s = xmalloc(sizeof(struct rsave_heap_string) + len);
		s->tag = RSAVE_STRING;
		s->ref = heap[i].ref;
		s->seq = heap[i].seq;
		s->uk = 0;
		s->len = len;
		memcpy(s->text, heap[i].s->text, len);
		return s;
	}
	struct page *page = heap[i].page;
	if (!page) {
		// Empty array.
		struct rsave_heap_array *o = xcalloc(1, sizeof(struct rsave_heap_array));
		o->tag = RSAVE_ARRAY;
		o->ref = heap[i].ref;
		o->seq = heap[i].seq;
		o->rank_minus_1 = -1;

		// FIXME: System40.exe populates them but we don't have the type
		// information of the array here.
		o->data_type = 0;
		o->struct_type.name = strdup("");

		o->root_rank = -1;
		o->is_not_empty = 0;
		o->nr_slots = 0;
		return o;
	}
	switch (page->type) {
	case GLOBAL_PAGE:
	case LOCAL_PAGE:
		return frame_page_to_rsave(page, i, version);
	case STRUCT_PAGE:
		return struct_page_to_rsave(page, i);
	case ARRAY_PAGE:
		/*
		 * Замер перед тем, как решать судьбу `elems_shared` (см. известное
		 * ограничение): сколько ПОВЕРХНОСТНЫХ копий вообще попадает в образ.
		 * Признак живёт только в памяти движка, у оригинала такого поля в
		 * формате нет — значит цена его потери определяется тем, встречаются
		 * ли такие страницы в снимке вообще.
		 */
		if (page->elems_shared) {
			static int shared_seen;
			static const char *tr = (const char *)1;
			if (tr == (const char *)1)
				tr = getenv("XSYS4_SHARED_TRACE");
			shared_seen++;
			if (tr && *tr && shared_seen <= 8)
				NOTICE("SHARED: в образ идёт поверхностная копия — слот %d, "
				       "элементов %d, тип %d (признак elems_shared будет потерян)",
				       i, page->nr_vars, page->a_type);
			if (tr && *tr && shared_seen == 1)
				vm_stack_trace();
		}
		return array_page_to_rsave(page, i);
	case DELEGATE_PAGE:
		return delegate_page_to_rsave(page, i, version);
	default:
		ERROR("unsupported type %d", page->type);
	}
}

/*
 * `XSYS4_HEAP_STATS=<сколько строк>` — ЧТО ЛЕЖИТ В ОБРАЗЕ ВОЗОБНОВЛЕНИЯ.
 *
 * Зачем. Образ `ResumeSave` пишется игрой на КАЖДОМ шаге по карте данжа, и его
 * размер — это прямо длительность фриза (§5fb). У нас он оказался кратно больше
 * оригинального (замер по заголовкам GD-контейнера: 23,8 МБ против 7,8 МБ на
 * сопоставимом месте и 305 МБ против тех же 7,8 МБ после получаса игры), а
 * ФАЙЛ разобрать нечем — наш контейнер существующие скрипты не читают. Значит
 * считать надо изнутри, в момент записи: сколько живых объектов и каких.
 *
 * Печатается три числа на строку: живых объектов типа, их доля от всех живых и
 * суммарное число слотов значений (грубая мера веса в файле).
 */
static void heap_stats_report(void)
{
	static int top = -1;
	if (top < 0) {
		const char *e = getenv("XSYS4_HEAP_STATS");
		top = e && *e ? atoi(e) : 0;
	}
	if (top <= 0)
		return;

	// Ключ статистики: имя структуры либо род страницы. Структур в .ain конечное
	// число, поэтому таблица — простой массив по индексу структуры плюс пять
	// корзин под остальные рода.
	enum { B_STRING = 0, B_GLOBAL, B_LOCAL, B_ARRAY, B_DELEGATE, B_EXTRA };
	int n = ain->nr_structures + B_EXTRA;
	int *cnt = xcalloc(n, sizeof(int));
	long *slots = xcalloc(n, sizeof(long));
	long live = 0, total_slots = 0;
	for (size_t i = 0; i < heap_size; i++) {
		if (!heap[i].ref)
			continue;
		live++;
		int k;
		long sl = 1;
		if (heap[i].type == VM_STRING) {
			k = B_STRING;
			sl = heap[i].s ? heap[i].s->size / 4 + 1 : 1;
		} else if (!heap[i].page) {
			k = B_ARRAY;
		} else {
			struct page *p = heap[i].page;
			sl = p->nr_vars;
			switch (p->type) {
			case GLOBAL_PAGE:   k = B_GLOBAL; break;
			case LOCAL_PAGE:    k = B_LOCAL; break;
			case ARRAY_PAGE:    k = B_ARRAY; break;
			case DELEGATE_PAGE: k = B_DELEGATE; break;
			case STRUCT_PAGE:
				k = (p->index >= 0 && p->index < ain->nr_structures)
					? B_EXTRA + p->index : B_EXTRA;
				break;
			default: k = B_EXTRA; break;
			}
		}
		cnt[k]++;
		slots[k] += sl;
		total_slots += sl;
	}

	NOTICE("HEAPSTATS образ: слотов кучи %zu, живых объектов %ld, слотов значений %ld",
	       heap_size, live, total_slots);
	for (int r = 0; r < top; r++) {
		int best = -1;
		for (int i = 0; i < n; i++)
			if (cnt[i] > 0 && (best < 0 || slots[i] > slots[best]))
				best = i;
		if (best < 0)
			break;
		const char *name;
		switch (best) {
		case B_STRING:   name = "<строки>"; break;
		case B_GLOBAL:   name = "<глобальные страницы>"; break;
		case B_LOCAL:    name = "<локальные страницы>"; break;
		case B_ARRAY:    name = "<массивы>"; break;
		case B_DELEGATE: name = "<делегаты>"; break;
		default:
			name = ain->structures[best - B_EXTRA].name;
			break;
		}
		NOTICE("  HEAPSTATS %-46s объектов %7d (%4.1f%%), слотов значений %8ld (%4.1f%%)",
		       name, cnt[best], 100.0 * cnt[best] / (live ? live : 1),
		       slots[best], 100.0 * slots[best] / (total_slots ? total_slots : 1));
		cnt[best] = 0;
		slots[best] = 0;
	}
	free(cnt);
	free(slots);
}

static void save_heap_to_rsave(struct rsave *rs)
{
	heap_stats_report();
	rs->nr_heap_objs = heap_size;
	rs->heap = xcalloc(heap_size, sizeof(void*));
	for (size_t i = 0; i < heap_size; i++) {
		rs->heap[i] = heap_item_to_rsave(i, rs->version);
	}
}

static void save_call_stack_to_rsave(struct rsave *rs)
{
	rs->nr_call_frames = call_stack_ptr + 1;
	rs->call_frames = xcalloc(rs->nr_call_frames, sizeof(struct rsave_call_frame));
	rs->call_frames[0].type = RSAVE_CALL_STACK_BOTTOM;
	rs->call_frames[0].local_ptr = -1;
	rs->nr_return_records = call_stack_ptr;
	rs->return_records = xcalloc(rs->nr_return_records + 1, sizeof(struct rsave_return_record));
	rs->return_records[0].return_addr = -1;

	struct ain_function *prev_func = NULL;
	for (int i = 0; i < call_stack_ptr; i++) {
		struct function_call *call = &call_stack[i];
		struct ain_function *func = &ain->functions[call->fno];
		if (call->struct_page >= 0) {
			rs->call_frames[i + 1].type = RSAVE_METHOD_CALL;
		} else if (!strcmp(func->name, "main")) {
			rs->call_frames[i + 1].type = RSAVE_ENTRY_POINT;
		} else {
			rs->call_frames[i + 1].type = RSAVE_FUNCTION_CALL;
		}
		rs->call_frames[i + 1].local_ptr = call->page_slot;
		rs->call_frames[i + 1].struct_ptr = call->struct_page;
		if (i > 0) {
			/*
			 * Кадр, вызванный ДВИЖКОМ (vm_call: NEW-конструктор, делегат)
			 * возвращается в VM_RETURN (-1). Формат AliceSoft для записи с
			 * return_addr == -1 не пишет имя/local_addr/crc — информация о
			 * кадре терялась, и такой сейв не загружался (у Haha Ranman игра
			 * сохраняется ИЗНУТРИ конструктора сцены: NEW CActionSelectScene
			 * → цепочка событий → セーブ実行). Пишем маркер -2: поля
			 * сохраняются, загрузчик восстанавливает VM_RETURN.
			 */
			if (call->return_address == -1) {
				rs->return_records[i].return_addr = -2;
				rs->return_records[i].local_addr = -2;
			} else {
				rs->return_records[i].return_addr = call->return_address;
				rs->return_records[i].local_addr = call->return_address - prev_func->address;
			}
		}
		rs->return_records[i + 1].caller_func = rsave_function_name_ver(func, rs->version);
		rs->return_records[i + 1].crc = func->crc;
		prev_func = func;
	}
	rs->ip = rs->return_records[call_stack_ptr];
	rs->ip.return_addr = instr_ptr + 6;
	rs->ip.local_addr = instr_ptr - prev_func->address + 6;
}

/*
 * Снимок value-стека зависит от того, КАК позвали ResumeSave:
 *  - CALLSYS (старые игры): аргументы (key, filename) в момент vm_save_image
 *    ЕЩЁ на стеке — их надо исключить («exclude two values»), а возврат после
 *    резюма кладёт load-время CALLSYS RESUME_LOAD (`stack_push(0)`).
 *  - CALLHLL (v14, system.ResumeSave): ffi снял ВСЕ аргументы до вызова —
 *    резать нечего (резалось два ЧУЖИХ значения, и после загрузки ссылки на
 *    стеке съезжали: «Out of bounds page index» в первом же X_REF), а возврат
 *    никто не положит — load-время ResumeLoad объявлен void. Поэтому кладём
 *    его В СНИМОК сами: 0 = «возврат из загрузки» (семантика fork, как у
 *    CALLSYS-пути; по нему セーブ実行 уходит в ロード後復帰処理).
 */
static void save_stack_to_rsave(struct rsave *rs, bool hll_convention)
{
	rs->stack_size = hll_convention ? stack_ptr + 1 : stack_ptr - 2;

	rs->stack = xcalloc(rs->stack_size, sizeof(int32_t));
	for (int i = 0; i < rs->stack_size && i < stack_ptr; i++) {
		rs->stack[i] = stack[i].i;
	}
	if (hll_convention)
		rs->stack[stack_ptr] = 0;
}

static void save_func_names_to_rsave(struct rsave *rs)
{
	rs->nr_func_names = ain->nr_functions;
	rs->func_names = xcalloc(rs->nr_func_names, sizeof(char *));
	for (int i = 0; i < ain->nr_functions; i++) {
		rs->func_names[i] = strdup(ain->functions[i].name);
	}
}

static struct rsave *make_rsave(const char *key)
{
	struct rsave *save = xcalloc(1, sizeof(struct rsave));
	/*
	 * ain 14 («Ixseal», Dohna) — образ пишем ВЕРСИЕЙ 14, как оригинал.
	 * Разница с девятой не косметическая: там нет ни `seq` у каждого объекта,
	 * ни таблицы имён всех функций в хвосте (у Dohna это 37 737 имён, 1,77 МБ
	 * в КАЖДОМ файле), ни имён ctor/dtor и таблиц типов членов у структур.
	 * Формат разобран и сверен с образом оригинала побайтово (§5fb-2).
	 */
	if (ain->version >= 14) {
		save->version = 14;
	} else if (ain->nr_delegates > 0) {
		save->version = 9;
	} else {
		// FIXME: This is not always correct. Pastel Chime Continue is AIN v4
		//        but uses RSM v7.
		save->version = ain->version <= 4 ? 6 : 7;
	}
	save->key = strdup(key);
	return save;
}

static struct rsave *vm_image_to_rsave(const char *key, bool hll_convention)
{
	struct rsave *save = make_rsave(key);
	save->next_seq = heap_next_seq;
	save_heap_to_rsave(save);
	save_call_stack_to_rsave(save);
	save_stack_to_rsave(save, hll_convention);
	// Версия 14 таблицу имён функций не несёт вовсе.
	if (save->version < 14)
		save_func_names_to_rsave(save);
	return save;
}

static int save_rsave_image(const char *key, const char *path, bool hll_convention)
{
	struct rsave *save = vm_image_to_rsave(key, hll_convention);
	if (!save)
		return 0;
	char *full_path = savedir_path(path);
	FILE *fp = file_open_utf8(full_path, "wb");
	if (!fp) {
		WARNING("Failed to open save file %s: %s", display_utf0(full_path), strerror(errno));
		free(full_path);
		rsave_free(save);
		return 0;
	}
	free(full_path);

	/*
	 * ★Версия 14 у оригинала НЕ ШИФРУЕТСЯ: тело GD-контейнера начинается прямо
	 * с zlib-заголовка `78 01`, тогда как у шифрованного первый байт 0x1a.
	 * Наш загрузчик понимает оба варианта, а вот System40 на наш образ мы
	 * рассчитываем натравить, поэтому пишем ровно как он.
	 */
	bool encrypt = save->version < 14;
	int compression_level = 1;
	enum savefile_error error = rsave_write(save, fp, encrypt, compression_level);
	if (error != SAVEFILE_SUCCESS)
		WARNING("Failed to write save file: %s", savefile_strerror(error));
	fclose(fp);
	rsave_free(save);
	return 1;
}

static int save_json_image(const char *key, const char *path)
{
	cJSON *image = vm_image_to_json(key);
	int r = save_json(path, image);
	cJSON_Delete(image);
	return r;
}

int vm_save_image(const char *key, const char *path, bool hll_convention)
{
	/*
	 * ★`XSYS4_KEEP_SUSPEND=hold` ЗАПРЕЩАЕТ И ПЕРЕЗАПИСЬ ОБРАЗА.
	 *
	 * Держать файл от удаления мало: игра пишет `SystemSuspend` при каждом
	 * снимке и при закрытии окна, поэтому отложенный образ интересного места
	 * затирается следующим же выходом — стенд живёт до первого выхода вместо
	 * «сколько угодно перезапусков». Отказ здесь безвреден: запись образа
	 * возобновления игре ничего не возвращает, кроме признака успеха.
	 */
	static const char *hold = (const char *)1;
	if (hold == (const char *)1)
		hold = getenv("XSYS4_KEEP_SUSPEND");
	if (hold && !strcmp(hold, "hold") && path && strstr(path, "SystemSuspend")) {
		NOTICE("XSYS4_KEEP_SUSPEND=hold: запись '%s' пропущена — образ сохранён "
		       "как есть", display_utf0(path));
		return 1;
	}
	switch (config.save_format) {
	case SAVE_FORMAT_RSM:
		return save_rsave_image(key, path, hll_convention);
	case SAVE_FORMAT_JSON:
		return save_json_image(key, path);
	}
	return 0;
}

#define _invalid_save_data(file, func, line, fmt, ...)	\
	_vm_error("*ERROR*(%s:%s:%d): " fmt "\n", file, func, line, ##__VA_ARGS__)
#define invalid_save_data(fmt, ...) \
	_invalid_save_data(__FILE__, __func__, __LINE__, fmt, ##__VA_ARGS__)

static const char *json_strtype(int type)
{
	switch (type & 0xFF) {
	case cJSON_False:
	case cJSON_True:
		return "boolean";
	case cJSON_NULL:   return "null";
	case cJSON_Number: return "number";
	case cJSON_String: return "string";
	case cJSON_Array:  return "array";
	case cJSON_Object: return "object";
	}
	return "unknown-type";
}

static cJSON *_type_check(const char *file, const char *func, int line, int type, cJSON *json)
{
	if (!json)
		_invalid_save_data(file, func, line, "Expected %s but got NULL", json_strtype(type));
	if (!(json->type & type))
		_invalid_save_data(file, func, line, "Expected %s but value is of type %s", json_strtype(type), json_strtype(json->type));
	return json;
}

#define type_check(type, json) _type_check(__FILE__, __func__, __LINE__, type, json)

static void load_json_page(int slot, cJSON *json)
{
	int struct_type = 0, rank = 0;

	// unpack
	cJSON *type    = type_check(cJSON_String, cJSON_GetObjectItem(json, "type"));
	cJSON *subtype = type_check(cJSON_Number, cJSON_GetObjectItem(json, "subtype"));
	cJSON *values  = type_check(cJSON_Array,  cJSON_GetObjectItem(json, "values"));

	enum page_type page_type = string_to_page_type(cJSON_GetStringValue(type));
	if (page_type == ARRAY_PAGE) {
		cJSON *_struct_type = type_check(cJSON_Number, cJSON_GetObjectItem(json, "struct-type"));
		cJSON *_rank        = type_check(cJSON_Number, cJSON_GetObjectItem(json, "rank"));
		struct_type = _struct_type->valueint;
		rank = _rank->valueint;
	}

	// allocate page
	struct page *page = alloc_page(page_type, subtype->valueint, cJSON_GetArraySize(values));
	page->array.struct_type = struct_type;
	page->array.rank = rank;

	// init page variables
	int i = 0;
	cJSON *item;
	cJSON_ArrayForEach(item, values) {
		page->values[i].i = item->valueint;
		i++;
	}

	heap[slot].page = page;
	heap[slot].type = VM_PAGE;
}

static void load_json_string(int slot, cJSON *json)
{
	const char *str = cJSON_GetStringValue(json);
	heap[slot].s = make_string(str, strlen(str));
	heap[slot].type = VM_STRING;
}

static void delete_heap(void)
{
	// free heap
	for (size_t i = 0; i < heap_size; i++) {
		if (!heap[i].ref)
			continue;
		switch (heap[i].type) {
		case VM_PAGE:
			if (heap[i].page)
				free_page(heap[i].page);
			break;
		case VM_STRING:
			free_string(heap[i].s);
			break;
		}
		heap[i].ref = 0;
		heap[i].seq = 0;
	}

	heap_free_ptr = 0;
	for (size_t i = 0; i < heap_size; i++) {
		heap_free_stack[i] = i;
	}
}

// Allocate a specific heap slot
// XXX: This only works while heap_free_stack[i]=i
//      After calling delete_heap, this can be called for INCREASING indices.
//      Out-of-order allocations would break the above assumption!
/*
 * ★СЛОТ ЗАНИМАЕТСЯ ТОЛЬКО ПОДРЯД — иначе свободный список ломается.
 *
 * Инвариант аллокатора: `heap_free_stack[i] == i` для нетронутой кучи, занятые
 * слоты — префикс [0, heap_free_ptr). Прежний код снимал с ВЕРШИНЫ чужой номер
 * (`heap_free_stack[slot] = heap_free_stack[heap_free_ptr]; heap_free_ptr++`),
 * что верно ровно пока `slot == heap_free_ptr`.
 *
 * А образ идёт С ДЫРАМИ: пустых записей (`RSAVE_NULL`) у нас 1682, у оригинала
 * 1629 — их загрузчик пропускал, следующий занятый слот оказывался больше
 * границы, и в свободном списке появлялся ДУБЛИКАТ номера: позже аллокатор
 * выдал бы его дважды. Замер после правки: предупреждений о рассогласовании
 * ноль (диагностика ниже), то есть список согласован.
 *
 * ★Отдельно: падение `double free` после возобновления этим НЕ лечится —
 * там объект из образа с `ref=1` освобождают двое (§5fb-10, открыто).
 *
 * Поэтому дыры теперь тоже ЗАНИМАЮТСЯ (heap[slot] остаётся пустой страницей с
 * ref=0), а список остаётся согласованным: префикс занят, дальше — свободное.
 */
static void alloc_heap_slot(int slot)
{
	if ((size_t)slot >= heap_size) {
		size_t next_size = heap_size * 2;
		while ((size_t)slot >= next_size)
			next_size *= 2;
		heap_grow(next_size);
	}
	if ((size_t)slot != heap_free_ptr) {
		WARNING("восстановление кучи: слот %d при границе %zu — "
		        "свободный список рассогласован", slot, heap_free_ptr);
	}
	heap_free_stack[slot] = heap_free_stack[heap_free_ptr];
	heap_free_ptr++;
}

static void load_json_heap(cJSON *json)
{
	delete_heap();

	cJSON *item;
	cJSON_ArrayForEach(item, json) {
		type_check(cJSON_Array, item);
		if (cJSON_GetArraySize(item) < 3)
			invalid_save_data("Invalid heap data");

		int slot = type_check(cJSON_Number, cJSON_GetArrayItem(item, 0))->valueint;
		int ref  = type_check(cJSON_Number, cJSON_GetArrayItem(item, 1))->valueint;
		cJSON *value = cJSON_GetArrayItem(item, 2);
		int seq = ain->nr_delegates > 0
			? type_check(cJSON_Number, cJSON_GetArrayItem(item, 3))->valueint
			: slot;

		alloc_heap_slot(slot);
		heap[slot].ref = ref;
		heap[slot].seq = seq;

		if (cJSON_IsString(value)) {
			load_json_string(slot, value);
		} else if (cJSON_IsObject(value)) {
			load_json_page(slot, value);
		} else if (cJSON_IsNull(value)) {
			heap[slot].type = VM_PAGE;
			heap[slot].page = NULL;
		} else {
			invalid_save_data("Invalid heap data");
		}
	}
}

/*
 * Имя из образа версии 14 несёт СИГНАТУРУ (`MapView@Move(MapEdge&, DG_Event)`),
 * и по нему функция определяется однозначно — в отличие от голого имени, на
 * котором `ain_get_function` отдаёт первую попавшуюся перегрузку.
 *
 * Базовое имя — всё до ПОСЛЕДНЕЙ пары скобок: у лямбд скобки есть и внутри
 * имени (`MapNodeView@<lambda : MapNodeView@RegisterEvent()(63, 38)>(EPartsState)`),
 * так что отрезать по первой нельзя. Дальше перебираем перегрузки `имя`,
 * `имя#1`, `имя#2`… и сравниваем их сигнатуры с искомой.
 */
static int resolve_func_by_signature(const char *name)
{
	size_t len = strlen(name);
	if (!len || name[len-1] != ')')
		return -1;
	int depth = 0;
	size_t open = 0;
	bool found = false;
	for (size_t i = len; i-- > 0; ) {
		if (name[i] == ')') {
			depth++;
		} else if (name[i] == '(') {
			if (--depth == 0) {
				open = i;
				found = true;
				break;
			}
		}
	}
	if (!found || !open)
		return -1;

	char *base = xmalloc(open + 1);
	memcpy(base, name, open);
	base[open] = '\0';

	int result = -1;
	char buf[1024];
	for (int n = 0; ; n++) {
		int cand = n == 0 ? ain_get_function(ain, base)
				  : (snprintf(buf, sizeof(buf), "%s#%d", base, n),
				     ain_get_function(ain, buf));
		if (cand < 0)
			break;
		char *sig = ain_function_signature(ain, &ain->functions[cand]);
		bool hit = !strcmp(sig, name);
		free(sig);
		if (hit) {
			result = cand;
			break;
		}
	}
	free(base);
	return result;
}

static int resolve_func_symbol(struct rsave_symbol *sym)
{
	if (!sym->name)
		return sym->id;
	int by_sig = resolve_func_by_signature(sym->name);
	if (by_sig >= 0)
		return by_sig;
	return ain_get_function(ain, sym->name);
}

/*
 * Функция кадра стека по имени из образа. Путей два, и они не взаимозаменяемы:
 *
 *  - версия 14 пишет СИГНАТУРУ (`SceneAzito@Run(int)`) — тогда перегрузка
 *    определена именем, и `ain_get_function` на нём не найдёт НИЧЕГО (в .ain
 *    имена голые). Это и роняло возобновление: игра сама писала SystemSuspend,
 *    а наш же загрузчик отвечал «Invalid save file».
 *  - наши прежние версии писали голое имя, и перегрузку приходится добирать
 *    перебором `имя#N` по CRC.
 *
 * CRC сверяем в обоих случаях: он ловит несовпадение .ain с образом.
 */
static int resolve_caller_func(const char *name, int32_t crc)
{
	int fno = resolve_func_by_signature(name);
	if (fno >= 0)
		return ain->functions[fno].crc == crc ? fno : -1;

	fno = ain_get_function(ain, (char *)name);
	if (fno < 0)
		return -1;
	if (ain->functions[fno].crc == crc)
		return fno;
	// ain_get_function уже отрезал `#N`, так что name здесь — базовое имя.
	char buf[512];
	for (int n = 0; ; n++) {
		snprintf(buf, sizeof(buf), "%s#%d", name, n);
		int cand = ain_get_function(ain, buf);
		if (cand < 0)
			break;
		if (ain->functions[cand].crc == crc)
			return cand;
	}
	return -1;
}

static int resolve_struct_symbol(struct rsave_symbol *sym)
{
	if (!sym->name)
		return sym->id;
	return ain_get_struct(ain, sym->name);
}

// Подходит ли функция под сохранённый фрейм: типы переменных совпадают.
static bool rsave_frame_fits(int func, struct rsave_heap_frame *f)
{
	if (func < 0 || ain->functions[func].nr_vars < f->nr_types)
		return false;
	/*
	 * ★В версии 14 таблицы типов у кадра нет вовсе (`nr_types == 0`), и без
	 * неё эта проверка проходила бы тривиально для ЛЮБОЙ перегрузки — а имя в
	 * образе голое, так что `ain_get_function` вернёт первую попавшуюся.
	 * Единственное, что о кадре известно помимо имени, — сколько в нём
	 * переменных; по нему перегрузки и различаем.
	 */
	if (!f->nr_types)
		return ain->functions[func].nr_vars == f->nr_slots;
	for (int i = 0; i < f->nr_types; i++) {
		if (f->types[i] != ain->functions[func].vars[i].type.data)
			return false;
	}
	return true;
}

static void load_rsave_frame(int slot, struct rsave_heap_frame *f)
{
	struct page *page;
	int nr_vars;
	struct ain_variable *vars;
	if (f->tag == RSAVE_GLOBALS) {
		page = alloc_page(GLOBAL_PAGE, 0, f->nr_slots);
		nr_vars = ain->nr_globals;
		vars = ain->globals;
	} else {
		int func = resolve_func_symbol(&f->func);
		/*
		 * Сейвы, записанные до канонизации `имя#N` (см. frame_page_to_rsave),
		 * хранят у перегруженных функций голое имя — оно разрешается в первую
		 * перегрузку, не обязательно ту. Подбираем перегрузку по сохранённым
		 * ТИПАМ переменных фрейма. resolve_func_symbol уже отрезал `#N` от
		 * имени, так что f->func.name здесь — базовое.
		 */
		if (f->func.name && !rsave_frame_fits(func, f)) {
			char buf[512];
			for (int n = 0; ; n++) {
				snprintf(buf, sizeof(buf), "%s#%d", f->func.name, n);
				int cand = ain_get_function(ain, buf);
				if (cand < 0)
					break;
				if (rsave_frame_fits(cand, f)) {
					func = cand;
					break;
				}
			}
		}
		page = alloc_page(LOCAL_PAGE, func, f->nr_slots);
		nr_vars = ain->functions[func].nr_vars;
		vars = ain->functions[func].vars;
		page->local.struct_ptr = f->struct_ptr;
	}

	// type check
	if (nr_vars < f->nr_types) {
		invalid_save_data("call frame has too many variables");
	}
	for (int i = 0; i < f->nr_types; i++) {
		if (f->types[i] != vars[i].type.data) {
			invalid_save_data(
				"variable type mismatch. Expected %d, got %d",
				vars[i].type.data, f->types[i]);
		}
	}

	for (int i = 0; i < f->nr_slots; i++) {
		// TODO: update function pointers using rsave->func_names
		page->values[i].i = f->slots[i];
	}
	alloc_heap_slot(slot);
	heap[slot].ref = f->ref;
	heap[slot].seq = f->seq;
	heap[slot].type = VM_PAGE;
	heap[slot].page = page;
}

static void load_rsave_string(int slot, struct rsave_heap_string *s)
{
	alloc_heap_slot(slot);
	heap[slot].ref = s->ref;
	heap[slot].seq = s->seq;
	heap[slot].type = VM_STRING;
	heap[slot].s = make_string(s->text, s->len - 1);
}

static void load_rsave_array(int slot, struct rsave_heap_array *a)
{
	if (a->rank_minus_1 < 0) {
		/*
		 * ★ПУСТОЙ МАССИВ ВОССТАНАВЛИВАЕМ ТИПИЗИРОВАННЫМ, А НЕ NULL-СТРАНИЦЕЙ.
		 *
		 * `rank_minus_1 = -1` в образе значит «страница не размещена», но ТИП
		 * элемента оригинал при этом ПИШЕТ (`data_type`/`struct_type`/`root_rank`;
		 * в снимке ADV таких массивов 10112 из 68503). Восстановленный как голый
		 * NULL, массив теряет тип, и первая же вставка идёт мимо владения:
		 * `ix_dtype(NULL)` отвечает «массив int», `Array.Insert` не берёт ссылку
		 * на объект, объект умирает сразу после вставки, а слот переиспользуется
		 * под чужую страницу.
		 *
		 * Чем это ломало Dohna (§5fb-10, «после F8 ввод мёртв»): очередь
		 * прочитанных реплик `CReadMessageTextManager::m_readMessageTextQueue`
		 * приходит из образа пустой. Первый клик → `SetReadMessage` кладёт туда
		 * `SReadMessageTextInfo` без владения, объект гибнет, слот достаётся
		 * ЛОКАЛЬНОЙ СТРАНИЦЕ следующего вызова — и `EraseMessageNumber`, проходя
		 * очередь, пишет «прочитано» прямо в СЧЁТЧИК СВОЕГО ЖЕ ЦИКЛА (локал 1).
		 * Счётчик вечно возвращается в 1, игра уходит в петлю: экран замирает,
		 * ввод не отвечает. Замер: `XSYS4_ARRAY_OWN=SReadMessageTextInfo` —
		 * «владение НЕ ВЗЯТО (тип элемента 14, страница ОТСУТСТВУЕТ (NULL))».
		 *
		 * Поведенчески NULL и 0-элементная типизированная страница неотличимы
		 * (`Numof`/`Empty` дают одно и то же) — ровно этим правилом уже живут
		 * `ix_resize` и `ix_erase_at`, которые пересоздают типизированную
		 * страницу при сжатии до нуля.
		 */
		struct page *empty = NULL;
		// Откат прежнего поведения для бисекта и проверки диагностики:
		// XSYS4_EMPTY_ARRAY_NULL=1 — поднимать пустой массив голым NULL, как
		// это делалось до §5fb-12 (тогда тип элемента терялся).
		static const char *no_typed = (const char *)1;
		if (no_typed == (const char *)1)
			no_typed = getenv("XSYS4_EMPTY_ARRAY_NULL");
		if (a->data_type && !(no_typed && *no_typed)) {
			union vm_value dim = { .i = 0 };
			int rank = a->root_rank >= 0 ? a->root_rank + 1 : 1;
			empty = alloc_array(rank, &dim, a->data_type,
					    resolve_struct_symbol(&a->struct_type), false);
			if (empty) {
				empty->a_type = a->data_type;
				empty->array.struct_type = resolve_struct_symbol(&a->struct_type);
				empty->array.rank = rank;
			}
		}
		alloc_heap_slot(slot);
		heap[slot].ref = a->ref;
		heap[slot].seq = a->seq;
		heap[slot].type = VM_PAGE;
		heap[slot].page = empty;
		return;
	}
	struct page *page = alloc_page(ARRAY_PAGE, a->data_type, a->nr_slots);
	page->array.struct_type = resolve_struct_symbol(&a->struct_type);
	page->array.rank = a->rank_minus_1 + 1;
	for (int i = 0; i < a->nr_slots; i++)
		page->values[i].i = a->slots[i];

	alloc_heap_slot(slot);
	heap[slot].ref = a->ref;
	heap[slot].seq = a->seq;
	heap[slot].type = VM_PAGE;
	heap[slot].page = page;
}

static void load_rsave_struct(int slot, struct rsave_heap_struct *s)
{
	int struct_index = resolve_struct_symbol(&s->struct_type);
	struct page *page = alloc_page(STRUCT_PAGE, struct_index, s->nr_slots);

	// type check
	struct ain_struct *as = &ain->structures[struct_index];
	if (as->nr_members < s->nr_types) {
		invalid_save_data("Struct %s has too many members", as->name);
	}
	for (int i = 0; i < s->nr_types; i++) {
		if (s->types[i] != as->members[i].type.data) {
			invalid_save_data(
				"%s.%s: member type mismatch. Expected %d, got %d",
				as->name, as->members[i].name,
				as->members[i].type.data, s->types[i]);
		}
	}

	for (int i = 0; i < s->nr_slots; i++) {
		// TODO: update function pointers using rsave->func_names
		page->values[i].i = s->slots[i];
	}

	alloc_heap_slot(slot);
	heap[slot].ref = s->ref;
	heap[slot].seq = s->seq;
	heap[slot].type = VM_PAGE;
	heap[slot].page = page;

	/*
	 * `XSYS4_STRUCT_WATCH` ставит вотч в `alloc_struct`, то есть только на
	 * объекты, СОЗДАННЫЕ в этом прогоне. Объекты, ПРИШЕДШИЕ ИЗ СЕЙВА, мимо него
	 * проходят — а именно за ними надо следить после загрузки (у Haha Ranman
	 * внутренний `SceneStack` SAVE-экрана не разрушается, и его слой висит на
	 * экране поверх ADV). Ставим вотч и здесь, по тому же имени типа.
	 */
	const char *w = getenv("XSYS4_STRUCT_WATCH");
	if (w && *w && as->name) {
		bool hit = (w[0] == '=') ? !strcmp(as->name, w + 1) : !!strstr(as->name, w);
		if (hit) {
			NOTICE("STRUCTWATCH из сейва: '%s' в слоте %d (ref=%d) — следим",
			       as->name, slot, heap[slot].ref);
			heap_watch_slot_set(slot);
		}
	}
}

static void load_rsave_delegate(int slot, struct rsave_heap_delegate *d)
{
	/*
	 * Версия 14 (образ оригинала) держит записи делегата отдельным списком, и
	 * метод в них назван ИМЕНЕМ, а не индексом функции: имена переживают
	 * пересборку .ain, индексы — нет. Окружения лямбды в записи нет (у
	 * оригинала оно, судя по всему, лежит в самом кадре, см. env_ptr в
	 * savefile.h), поэтому ставим -1, как и для чужих троек.
	 */
	if (d->nr_entries) {
		struct page *page = alloc_page(DELEGATE_PAGE, 0, d->nr_entries * DG_ENTRY_SLOTS);
		for (int i = 0; i < d->nr_entries; i++) {
			struct rsave_symbol sym = { .name = d->entries[i].method };
			page->values[i*DG_ENTRY_SLOTS + 0].i = d->entries[i].obj;
			page->values[i*DG_ENTRY_SLOTS + 1].i = resolve_func_symbol(&sym);
			/*
			 * Третий слот записи — ПОКОЛЕНИЕ получателя, по нему
			 * delegate_compact/delegate_get отличают живую запись от
			 * повисшей (`heap_get_seq(obj) == values[i+2]`, src/page.c).
			 * Поколений в четырнадцатой версии нет: все объекты приходят
			 * с нулём, значит и здесь обязан быть ноль — иначе первая же
			 * рассылка выбросит ВСЕ записи делегата как протухшие.
			 * ★Именно тут раньше лежало поле `uk` из файла (у оригинала
			 * оно −1), и делегаты умирали молча.
			 */
			page->values[i*DG_ENTRY_SLOTS + 2].i = heap_get_seq(d->entries[i].obj);
			// Окружение лямбды мы кладём в то же третье поле записи файла.
			page->values[i*DG_ENTRY_SLOTS + 3].i = d->entries[i].uk;
		}
		alloc_heap_slot(slot);
		heap[slot].ref = d->ref;
		heap[slot].seq = d->seq;
		heap[slot].type = VM_PAGE;
		heap[slot].page = page;
		return;
	}

	// Наш образ (маркер в нулевом слоте, см. delegate_page_to_rsave) — четвёрки с
	// окружением; чужой/старый — тройки, окружение в нём не сохранено (env = -1).
	bool with_env = d->nr_slots > 0 && d->slots[0] == RSAVE_DG_ENV_MAGIC;
	int nr_entries = with_env ? (d->nr_slots - 1) / 4 : d->nr_slots / 3;
	struct page *page = alloc_page(DELEGATE_PAGE, 0, nr_entries * DG_ENTRY_SLOTS);
	for (int i = 0; i < nr_entries; i++) {
		if (with_env) {
			for (int k = 0; k < 4; k++)
				page->values[i*DG_ENTRY_SLOTS + k].i = d->slots[1 + i*4 + k];
		} else {
			for (int k = 0; k < 3; k++)
				page->values[i*DG_ENTRY_SLOTS + k].i = d->slots[i*3 + k];
			page->values[i*DG_ENTRY_SLOTS + 3].i = -1;
		}
	}

	alloc_heap_slot(slot);
	heap[slot].ref = d->ref;
	heap[slot].seq = d->seq;
	heap[slot].type = VM_PAGE;
	heap[slot].page = page;
}

/*
 * ★После загрузки образа heap-слот 0 ОБЯЗАН быть занят ГЛОБАЛЬНОЙ СТРАНИЦЕЙ.
 * `delete_heap` возвращает В СПИСОК СВОБОДНЫХ ВСЕ слоты, включая нулевой, и
 * корректность держится только на том, что в образе есть запись для слота 0
 * (`RSAVE_GLOBALS`), которая занимает его через `alloc_heap_slot`. Если записи
 * нет, слот 0 остаётся выдаваемым — и очередное выделение ПОДМЕНИТ глобальную
 * страницу чужой, а падать игра будет много позже и в другом месте (§5ee).
 * Поэтому проверяем прямо здесь: чиним слот и говорим об этом громко.
 */
static void check_global_slot_after_load(void)
{
	if (heap_size > 0 && heap[0].ref > 0 && heap[0].type == VM_PAGE
			&& heap[0].page && heap[0].page->type == GLOBAL_PAGE)
		return;
	WARNING("после загрузки образа heap-слот 0 НЕ занят глобальной страницей "
		"(ref=%d type=%d) — в образе не было записи глобалей; ставлю пустую "
		"страницу и занимаю слот, иначе его выдаст аллокатор",
		heap_size > 0 ? heap[0].ref : -1,
		heap_size > 0 ? (int)heap[0].type : -1);
	// Убрать 0 из списка свободных: он лежит где-то в хвосте [heap_free_ptr, …).
	for (size_t i = heap_free_ptr; i < heap_size; i++) {
		if (heap_free_stack[i] != 0)
			continue;
		heap_free_stack[i] = heap_free_stack[heap_free_ptr];
		heap_free_stack[heap_free_ptr] = 0;
		heap_free_ptr++;
		break;
	}
	heap[0].ref = 1;
	heap[0].seq = 0;
	heap[0].type = VM_PAGE;
	heap[0].page = alloc_page(GLOBAL_PAGE, 0, ain->nr_globals);
}

/*
 * ★ПОКОЛЕНИЯ СЛОТОВ ПОСЛЕ ЗАГРУЗКИ ОБРАЗА ВЕРСИИ 14 — БЕЗ НИХ ДЕЛЕГАТЫ СЛЕПНУТ.
 *
 * Подписка делегата хранит поколение получателя, и `delegate_compact`/
 * `delegate_get` отличают живую запись от повисшей сверкой
 * `heap_get_seq(obj) == values[i+2]` (src/page.c). В формате версии 14
 * поколений нет вовсе: все объекты приходят с нулём, у СВОБОДНОГО слота
 * `heap_get_seq` тоже ноль — и сверка `0 == 0` объявляет годной ЛЮБУЮ запись,
 * включая ту, чей получатель давно мёртв. Смерть после возобновления тоже
 * перестаёт замечаться.
 *
 * Замерено (XSYS4_REF_DEAD_TRACE, стенд «снимок → перезапуск»): на обычном
 * прогоне взятий ссылки на мёртвый слот НОЛЬ, после возобновления —
 * «REF-DEAD слот 132189 (ref=0)» в `RCASTimer@AddTime` ← `RCASTimerManager@
 * <lambda>` ← `CallPartsUpdateEvent`, то есть подписку вызвали с мёртвым
 * получателем. Через десяток кадров это давало `double free`.
 *
 * Поэтому поколения назначаем сами: живому объекту — номер его слота плюс
 * один (уникально и ненулевое; `heap_next_seq` для версии 14 стоит за
 * пределами занятого, так что новые объекты не столкнутся), дыре — ноль, она
 * и есть «мёртвый». Записям делегата проставляем актуальное поколение, а
 * запись с мёртвым получателем помечаем заведомо не совпадающим значением:
 * штатное уплотнение выбросит её само и штатно же отпустит окружение.
 */
static void rsave_fixup_generations(struct rsave *save)
{
	if (save->version < 14)
		return;

	for (int slot = 0; slot < save->nr_heap_objs; slot++) {
		if (heap[slot].ref <= 0)
			continue;
		// Дыра образа (RSAVE_NULL) — пустая страница: пусть остаётся мёртвой.
		if (heap[slot].type == VM_PAGE && !heap[slot].page)
			continue;
		heap[slot].seq = (uint32_t)slot + 1;
	}

	int fixed = 0, dropped = 0;
	for (int slot = 0; slot < save->nr_heap_objs; slot++) {
		if (heap[slot].ref <= 0 || heap[slot].type != VM_PAGE)
			continue;
		struct page *p = heap[slot].page;
		if (!p || p->type != DELEGATE_PAGE)
			continue;
		for (int i = 0; i + DG_ENTRY_SLOTS <= p->nr_vars; i += DG_ENTRY_SLOTS) {
			int obj = p->values[i].i;
			/*
			 * ★ПОДПИСКА БЕЗ ПОЛУЧАТЕЛЯ — ЗАКОННАЯ. Свободная функция (и
			 * лямбда без `this`) подписывается с obj = −1, и поколение у неё
			 * нулевое по построению: `heap_get_seq(−1)` = 0, сверка
			 * `0 == 0` проходит. Без этой проверки такие записи уходили под
			 * нож как «протухшие» — 166 штук на снимке титула при НУЛЕ
			 * отброшенных на записи, что и выдало ошибку.
			 */
			if (obj < 0) {
				p->values[i+2].i = 0;
				fixed++;
				continue;
			}
			uint32_t seq = heap_get_seq(obj);
			if (seq) {
				p->values[i+2].i = (int32_t)seq;
				fixed++;
			} else {
				p->values[i+2].i = -1;  // не совпадёт ни с чем — под нож
				dropped++;
			}
		}
	}
	if (getenv("XSYS4_RESUME_TRACE"))
		NOTICE("RESUME поколения: подписок живых %d, протухших %d", fixed, dropped);
}

static void load_rsave_heap(struct rsave *save)
{
	delete_heap();

	for (int slot = 0; slot < save->nr_heap_objs; slot++) {
		enum rsave_heap_tag *tag = save->heap[slot];
		switch (*tag) {
		case RSAVE_GLOBALS:
		case RSAVE_LOCALS:   load_rsave_frame(slot, save->heap[slot]);    break;
		case RSAVE_STRING:   load_rsave_string(slot, save->heap[slot]);   break;
		case RSAVE_ARRAY:    load_rsave_array(slot, save->heap[slot]);    break;
		case RSAVE_STRUCT:   load_rsave_struct(slot, save->heap[slot]);   break;
		case RSAVE_DELEGATE: load_rsave_delegate(slot, save->heap[slot]); break;
		case RSAVE_NULL:
			/*
			 * ★ДЫРУ ЗАНИМАЕМ, А НЕ ПРОПУСКАЕМ: иначе ломается свободный
			 * список (разбор — у alloc_heap_slot). Дыр в образе много: у нас
			 * 1682, у оригинала 1629.
			 *
			 * Счётчик 1, а не 0: слот отдаётся как обычный мёртвый объект и
			 * при первом же освобождении честно возвращается в свободный
			 * список, вместо того чтобы уводить счётчик ниже нуля.
			 */
			alloc_heap_slot(slot);
			heap[slot].ref = 1;
			heap[slot].type = VM_PAGE;
			heap[slot].page = NULL;
			break;
		}
	}
	// Поколения назначаем ПОСЛЕ всей кучи: на момент загрузки делегата его
	// получатель мог быть ещё не восстановлен (слоты идут по возрастанию).
	rsave_fixup_generations(save);
	check_global_slot_after_load();
	// Куча поднята и поколения расставлены — самое место спросить, куда на самом
	// деле смотрят объектные поля (XSYS4_HEAP_AUDIT=1). Именно здесь рождаются
	// висячие ссылки образа: §5fb-12 нашёлся бы этим аудитом сразу, а не через
	// петлю в игровом коде.
	heap_audit_ownership("после подъёма кучи из образа");
	/*
	 * Поколения слотов (`seq`) — наша диагностика висячих ссылок
	 * (`deref_trail`, src/vm.c), в формате версии 14 их нет. Тогда все слоты
	 * приходят с нулём, и счётчик надо завести ЗА пределами занятого
	 * диапазона, иначе первый же новый объект получит поколение, совпадающее
	 * с чужим, и «слот переиспользован» перестанет ловиться.
	 */
	heap_next_seq = save->version >= 14 ? (uint32_t)save->nr_heap_objs + 1
					    : (uint32_t)save->next_seq;
}

static void load_json_call_stack(cJSON *json)
{
	call_stack_ptr = 0;
	cJSON *item;
	cJSON_ArrayForEach(item, json) {
		type_check(cJSON_Object, item);
		call_stack[call_stack_ptr++] = (struct function_call) {
			.fno            = type_check(cJSON_Number, cJSON_GetObjectItem(item, "function"))->valueint,
			.return_address = type_check(cJSON_Number, cJSON_GetObjectItem(item, "return-address"))->valueint,
			.page_slot      = type_check(cJSON_Number, cJSON_GetObjectItem(item, "local-page"))->valueint,
			.struct_page    = type_check(cJSON_Number, cJSON_GetObjectItem(item, "struct-page"))->valueint,
		};
	}
}

/*
 * ПРИГОДЕН ЛИ СЕЙВ — проверка ДО применения. `load_rsave_*` перезаписывают кучу
 * и стек, поэтому отказ на середине восстановления некуда откатить: раньше
 * непригодный сейв валил движок в REPL (`VM_ERROR`), и игра вставала насмерть.
 * Игре отказ отдавать НЕКУДА, кроме возврата из `system.ResumeLoad`: сразу за
 * этим вызовом у неё стоит своё сообщение (`gamesave::detail::ロード実行`:
 * `CALLHLL system ResumeLoad` → `S_PUSH "ロードに失敗しました"` → `system.Error`).
 * Живой случай: пункт LOAD LATEST берёт самый свежий слот, а им оказывается
 * сейв сборки, которая ещё не писала маркер кадров движка.
 *
 * Условия — те же, что у `load_rsave_call_stack` ниже; расхождение между ними
 * означает падение вместо отказа, поэтому менять их надо парой.
 */
static bool rsave_call_stack_loadable(struct rsave *save)
{
	/*
	 * ★ГЛУБИНА ИЗ ФАЙЛА — ТОЖЕ ВХОДНЫЕ ДАННЫЕ. `call_stack` — массив
	 * фиксированной длины, и загрузчик пушит в него столько кадров, сколько
	 * записано в образе: запись за границей затирает соседние глобалы движка
	 * (первым уезжает указатель `ain`) и роняет процесс далеко от причины.
	 * Игра сама доходила до 9145 кадров при ёмкости 4096 (§5fb-12), так что
	 * образ с такой глубиной — не выдумка: отказываем как от нечитаемого,
	 * и игра получит своё «загрузка не удалась» вместо падения.
	 */
	if (save->nr_call_frames < 0 || save->nr_call_frames > CALL_STACK_MAX)
		return false;
	if (save->nr_return_records == 0 || save->return_records[0].return_addr != -1)
		return false;
	struct rsave_return_record *rr = save->return_records;
	if (save->nr_return_records == save->nr_call_frames)
		rr++;
	else if (save->nr_return_records != save->nr_call_frames - 1)
		return false;

	for (int i = 0; i < save->nr_call_frames; i++) {
		if (save->call_frames[i].type != RSAVE_CALL_STACK_BOTTOM) {
			if (!rr->caller_func)
				return false;
			if (resolve_caller_func(rr->caller_func, rr->crc) < 0)
				return false;
		}
		if (++rr == save->return_records + save->nr_return_records)
			rr = &save->ip;
	}
	return true;
}

static void load_rsave_call_stack(struct rsave *save)
{
	call_stack_ptr = 0;

	// Пара к rsave_call_stack_loadable (см. там про глубину).
	if (save->nr_call_frames < 0 || save->nr_call_frames > CALL_STACK_MAX)
		ERROR("invalid resume save: кадров %d при ёмкости %d",
		      save->nr_call_frames, CALL_STACK_MAX);
	if (save->nr_return_records == 0 || save->return_records[0].return_addr != -1)
		ERROR("invalid resume save");
	struct rsave_return_record *rr = save->return_records;
	if (save->nr_return_records == save->nr_call_frames)
		rr++;
	else if (save->nr_return_records != save->nr_call_frames - 1)
		ERROR("invalid resume save");

	int32_t return_address = -1;
	for (int i = 0; i < save->nr_call_frames; i++) {
		if (save->call_frames[i].type != RSAVE_CALL_STACK_BOTTOM) {
			// Голая запись -1 посреди стека — сейв сборки, которая ещё не
			// писала маркер -2 для кадров движка (см. save_call_stack_to_rsave):
			// имя/crc кадра в файле отсутствуют, стек не восстановить.
			if (!rr->caller_func)
				VM_ERROR("Failed to load VM image: сейв сделан сборкой без "
				         "поддержки кадров движка в стеке — пересохранитесь");
			// Имя версии 14 несёт сигнатуру, прежних версий — голое;
			// перегрузка добирается по CRC. См. resolve_caller_func.
			int fno = resolve_caller_func(rr->caller_func, rr->crc);
			if (fno < 0)
				VM_ERROR("Failed to load VM image: function '%s' not found", display_sjis0(rr->caller_func));
			call_stack[call_stack_ptr++] = (struct function_call) {
				.fno            = fno,
				.return_address = return_address,
				.page_slot      = save->call_frames[i].local_ptr,
				.struct_page    = save->call_frames[i].struct_ptr,
				/*
				 * ★ОКРУЖЕНИЕ ЛЯМБДЫ — ЯВНО «НЕТ», а не ноль по умолчанию.
				 * `X_GETENV` считает окружением отсутствующим только
				 * отрицательное значение (src/vm.c: `if (env < 0)`), а ноль —
				 * это heap-слот ГЛОБАЛЬНОЙ страницы: восстановленный кадр
				 * лямбды читал бы захваченные переменные из глобалей по их
				 * индексам, а присваивание туда снимало бы чужие ссылки.
				 * В образе окружение кадра не хранится (`env_ptr` мы только
				 * переносим), так что взять его неоткуда — но и врать нельзя.
				 */
				.env_page       = -1,
			};
			// Calculate return address from the function address and offset
			// to make it robust to ain changes.
			// -2 — кадр движка (vm_call): возврат в VM_RETURN, не в байткод.
			if (rr->local_addr == -2)
				return_address = -1;
			else
				return_address = ain->functions[fno].address + rr->local_addr;
			// XSYS4_RESUME_TRACE=1 — восстановленный стек с пометкой кадров,
			// вызванных ДВИЖКОМ. На таком кадре выполнение после resume
			// ОСТАНАВЛИВАЕТСЯ (VM_RETURN), то есть всё, что было выше него в
			// момент сохранения, не доигрывается: у Haha Ranman так остаётся
			// неразрушенным второй стек сцен SAVE-экрана (его слой висит на
			// экране поверх ADV).
			if (getenv("XSYS4_RESUME_TRACE"))
				NOTICE("RESUME frame %2d: %s%s", call_stack_ptr - 1,
				       display_sjis0(ain->functions[fno].name),
				       return_address == -1 ? "   <- кадр движка (VM_RETURN)" : "");
		}
		if (++rr == save->return_records + save->nr_return_records)
			rr = &save->ip;
	}
	// RSM records instruction pointer after the CALLSYS instruction, but
	// xsystem4 expects the address of the CALLSYS instruction, so -6.
	instr_ptr = return_address - 6;
}

static void load_json_stack(cJSON *json)
{
	stack_ptr = 0;

	cJSON *item;
	cJSON_ArrayForEach(item, json) {
		type_check(cJSON_Number, item);
		stack_push_value(vm_int(item->valueint));
	}
	// Pop the arguments of SYS_RESUME_SAVE.
	//heap_unref(stack_pop().i);
	//heap_unref(stack_pop().i);
	stack_pop();
	stack_pop();
}

static void load_rsave_stack(struct rsave *save)
{
	stack_ptr = 0;

	for (int i = 0; i < save->stack_size; i++) {
		stack_push_value(vm_int(save->stack[i]));
	}
}

static cJSON *read_json_image(const char *keyname, const char *path)
{
	char *full_path = savedir_path(path);
	char *save_file = file_read(full_path, NULL);
	if (!save_file) {
		free(save_file);
		free(full_path);
		return NULL;
	}

	cJSON *save = cJSON_Parse(save_file);
	cJSON *key = type_check(cJSON_String, cJSON_GetObjectItem(save, "key"));
	if (strcmp(keyname, cJSON_GetStringValue(key)))
		invalid_save_data("Key doesn't match");

	free(save_file);
	free(full_path);
	return type_check(cJSON_Object, save);
}

static void load_json_image(const char *key, const char *path)
{
	cJSON *save = read_json_image(key, path);
	if (!save) {
		VM_ERROR("Failed to read VM image: '%s'", display_sjis0(path));
	}
	cJSON *ip = type_check(cJSON_Number, cJSON_GetObjectItem(save, "ip"));
	load_json_heap(type_check(cJSON_Array, cJSON_GetObjectItem(save, "heap")));
	load_json_call_stack(type_check(cJSON_Array, cJSON_GetObjectItem(save, "call-stack")));
	load_json_stack(type_check(cJSON_Array, cJSON_GetObjectItem(save, "stack")));
	instr_ptr = ip->valueint;
	if (ain->nr_delegates > 0) {
		cJSON *next_seq = type_check(cJSON_Number, cJSON_GetObjectItem(save, "next_seq"));
		heap_next_seq = next_seq->valueint;
	} else {
		heap_next_seq = heap_size;
	}
	vm_image_generation++;
	cJSON_Delete(save);
}

static enum savefile_error load_rsave_image(const char *key, const char *path)
{
	char *full_path = savedir_path(path);
	enum savefile_error error;
	struct rsave *save = rsave_read(full_path, RSAVE_READ_ALL, &error);
	free(full_path);
	if (error != SAVEFILE_SUCCESS)
		return error;
	if (strcmp(key, save->key))
		invalid_save_data("Key doesn't match");
	// Пригодность — ДО применения: дальше куча и стек перезаписываются,
	// откатывать нечем (см. rsave_call_stack_loadable).
	if (!rsave_call_stack_loadable(save)) {
		rsave_free(save);
		return SAVEFILE_INVALID;
	}
	load_rsave_heap(save);
	load_rsave_call_stack(save);
	load_rsave_stack(save);
	rsave_free(save);
	vm_image_generation++;
	/*
	 * Приём кликов — состояние движка, в образе его нет; берём из
	 * ВОССТАНОВЛЕННОГО стека (разбор — у самой функции). Без этого интерфейс
	 * после возобновления не получает ни одного сообщения.
	 */
	void parts_input_resync_after_resume(void);
	parts_input_resync_after_resume();
	// Диагностика: XSYS4_HEAP_WATCH_AFTER_LOAD=<slot> — вотч на слот
	// ВОССТАНОВЛЕННОЙ кучи (обычный XSYS4_HEAP_WATCH зашумляет лог жизнью
	// слота до загрузки — номера переиспользуются).
	{
		const char *w = getenv("XSYS4_HEAP_WATCH_AFTER_LOAD");
		if (w && *w)
			heap_watch_slot_set(atoi(w));
	}
	return SAVEFILE_SUCCESS;
}

void vm_load_image(const char *key, const char *path)
{
	// First, try to read as a rsave.
	enum savefile_error error = load_rsave_image(key, path);
	switch (error) {
	case SAVEFILE_SUCCESS:
		return;
	case SAVEFILE_INVALID_SIGNATURE:
		// If not a System4 resume save file, read as a json.
		load_json_image(key, path);
		return;
	default:
		/*
		 * ★ДИАЛОГ И ВЫХОД — как в оригинале. Прежде здесь стоял `VM_ERROR`, и
		 * негодный сейв ронял движок в REPL: окно оставалось на экране, игра
		 * замирала навсегда и о причине не сообщала.
		 *
		 * Продолжить игру на этом месте НЕЛЬЗЯ, и это замер, а не догадка:
		 * начиная загрузку, игра гасит приём ввода у всего экрана
		 * (пофрагментно, `SetEnableInputProcess`) и рассчитывает, что состояние
		 * вернёт сам образ сейва. На отказе включать некому — экран остаётся
		 * целым, но НЕМЫМ (пункты титула живы, `clk=1`, `eip=0`; клик доходит
		 * до движка и не попадает ни в одну часть). Игра к возврату не готова
		 * принципиально: в `Ａ＿選択右クリック関数登録` она на этом месте сама
		 * печатает `system.Error("…ロード処理がおこなわれませんでした")`, то
		 * есть считает возврат из загрузки аварией.
		 *
		 * Поэтому: сказать игроку, ЧТО случилось, и закрыться по OK.
		 * Состояние на этот момент не тронуто — пригодность сейва проверяется
		 * до применения (см. rsave_call_stack_loadable).
		 */
		sys_error("Failed to load VM image '%s': %s\n", display_sjis0(path),
		          savefile_strerror(error));
	}
}

static struct page *load_json_image_comments(const char *key, const char *path, int *success)
{
	cJSON *save = read_json_image(key, path);
	if (!save) {
		*success = 0;
		return NULL;
	}
	cJSON *comments = cJSON_GetObjectItem(save, "comments");
	if (comments)
		type_check(cJSON_Array, comments);
	if (!comments || cJSON_GetArraySize(comments) == 0) {
		*success = 1;
		return NULL;
		//union vm_value dims = { .i = 0 };
		//return alloc_array(1, &dims, AIN_ARRAY_STRING, 0, false);
	}

	// read comments from JSON array into VM array
	union vm_value dims = { .i = cJSON_GetArraySize(comments) };
	struct page *array = alloc_array(1, &dims, AIN_ARRAY_STRING, 0, false);
	for (int i = 0; i < dims.i; i++) {
		int slot = heap_alloc_slot(VM_STRING);
		cJSON *jstr = type_check(cJSON_String, cJSON_GetArrayItem(comments, i));
		char *str = cJSON_GetStringValue(jstr);
		if (!str[0]) {
			heap[slot].s = string_ref(&EMPTY_STRING);
		} else {
			heap[slot].s = make_string(str, strlen(str));
		}
		array->values[i].i = slot;
	}

	*success = 1;
	cJSON_Delete(save);
	return array;
}

static struct page *load_rsave_image_comments(const char *key, const char *path, enum savefile_error *error)
{
	char *full_path = savedir_path(path);
	struct rsave *save = rsave_read(full_path, RSAVE_READ_COMMENTS, error);
	free(full_path);
	if (*error != SAVEFILE_SUCCESS)
		return NULL;
	if (strcmp(key, save->key))
		invalid_save_data("Key doesn't match");
	if (save->nr_comments == 0) {
		rsave_free(save);
		return NULL;
	}
	union vm_value dims = { .i = save->nr_comments };
	struct page *array = alloc_array(1, &dims, AIN_ARRAY_STRING, 0, false);
	for (int i = 0; i < dims.i; i++) {
		int slot = heap_alloc_slot(VM_STRING);
		if (!save->comments[i][0]) {
			heap[slot].s = string_ref(&EMPTY_STRING);
		} else {
			heap[slot].s = make_string(save->comments[i], strlen(save->comments[i]));
		}
		array->values[i].i = slot;
	}
	rsave_free(save);
	return array;
}

struct page *vm_load_image_comments(const char *key, const char *path, int *success)
{
	// First, try to read as a rsave.
	enum savefile_error error;
	struct page *page = load_rsave_image_comments(key, path, &error);
	if (error == SAVEFILE_SUCCESS) {
		*success = 1;
		return page;
	}

	if (error == SAVEFILE_INVALID_SIGNATURE) {
		// If not a System4 resume save file, read as a json.
		return load_json_image_comments(key, path, success);
	}
	*success = 0;
	return NULL;
}

static int write_rsave_image_comments(const char *key, const char *path, struct page *comments)
{
	char *full_path = savedir_path(path);
	enum savefile_error error;
	struct rsave *save = rsave_read(full_path, RSAVE_READ_ALL, &error);
	switch (error) {
	case SAVEFILE_SUCCESS:
		if (strcmp(key, save->key))
			invalid_save_data("Key doesn't match");
		break;
	case SAVEFILE_FILE_ERROR:
		save = make_rsave(key);
		save->comments_only = true;
		break;
	default:
		free(full_path);
		return 0;
	}

	// Comments were added in RSM v7, so if the save is older, upgrade it.
	if (save->version <= 6) {
		save->version = 7;
	}

	for (int i = 0; i < save->nr_comments; i++) {
		free(save->comments[i]);
	}
	free(save->comments);
	save->nr_comments = comments->nr_vars;
	save->comments = xcalloc(save->nr_comments, sizeof(char*));
	for (int i = 0; i < save->nr_comments; i++) {
		save->comments[i] = xstrdup(heap_get_string(comments->values[i].i)->text);
	}

	FILE *fp = file_open_utf8(full_path, "wb");
	if (!fp) {
		WARNING("Failed to open save file %s: %s", display_utf0(full_path), strerror(errno));
		free(full_path);
		rsave_free(save);
		return 0;
	}
	free(full_path);

	// см. save_rsave_image: версия 14 у оригинала не шифрована
	bool encrypt = save->version < 14;
	int compression_level = 1;
	error = rsave_write(save, fp, encrypt, compression_level);
	if (error != SAVEFILE_SUCCESS)
		WARNING("Failed to write save file: %s", savefile_strerror(error));
	fclose(fp);
	rsave_free(save);
	return 1;
}

static int write_json_image_comments(const char *key, const char *path, struct page *comments)
{
	cJSON *save = read_json_image(key, path);
	if (!save) {
		// create blank save
		save = cJSON_CreateObject();
		cJSON_AddStringToObject(save, "key", key);
	}
	cJSON_DeleteItemFromObject(save, "comments");

	// TODO: check that comments is an array of strings
	cJSON *array = cJSON_CreateArray();
	for (int i = 0; i < comments->nr_vars; i++) {
		cJSON *s = cJSON_CreateString(heap_get_string(comments->values[i].i)->text);
		cJSON_AddItemToArray(array, s);
	}
	cJSON_AddItemToObject(save, "comments", array);

	save_json(path, save);
	cJSON_Delete(save);
	return 1;
}

int vm_write_image_comments(const char *key, const char *path, struct page *comments)
{
	switch (config.save_format) {
	case SAVE_FORMAT_RSM:
		return write_rsave_image_comments(key, path, comments);
	case SAVE_FORMAT_JSON:
		return write_json_image_comments(key, path, comments);
	}
	return 0;
}
