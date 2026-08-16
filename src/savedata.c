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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "cJSON.h"

#include "system4.h"
#include "system4/ain.h"
#include "system4/file.h"
#include "system4/savefile.h"
#include "system4/string.h"

#include "savedata.h"
#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "xsystem4.h"

static int current_global;

#define invalid_save_data(msg, data) {					\
		char *str = data ? cJSON_Print(data) : strdup("NULL");	\
		if (!str) str = strdup("PRINTING FAILED");		\
		WARNING("Invalid save data (%d): " msg ": %s", current_global, data); \
		free(str);						\
	}

int save_json(const char *filename, cJSON *json)
{
	char *path = savedir_path(filename);
	FILE *f = file_open_utf8(path, "w");
	if (!f) {
		WARNING("Failed to open save file: %s: %s", display_utf0(filename), strerror(errno));
		free(path);
		return 0;
	}
	free(path);

	char *str = cJSON_Print(json);

	if (fwrite(str, strlen(str), 1, f) != 1) {
		WARNING("Failed to write save file: %s", strerror(errno));
		free(str);
		return 0;
	}
	if (fclose(f)) {
		WARNING("Error writing save to file: %s: %s", display_utf0(filename), strerror(errno));
		free(str);
		return 0;
	}
	free(str);
	return 1;
}

cJSON *load_json(const char *filename)
{
	char *path = savedir_path(filename);
	char *json = file_read(path, NULL);
	free(path);
	if (!json)
		return NULL;
	cJSON *root = cJSON_Parse(json);
	free(json);
	return root;
}

static int get_group_index(const char *name)
{
	for (int i = 0; i < ain->nr_global_groups; i++) {
		if (!strcmp(ain->global_group_names[i], name))
			return i;
	}
	return -1;
}

static int32_t add_value_to_gsave(enum ain_data_type type, union vm_value val, struct gsave *save);

/*
 * ТОЖДЕСТВО ОБЪЕКТОВ. Формат разворачивает каждую запись как ДЕРЕВО, поэтому
 * объект, на который ссылаются два поля, без реестра сохранялся дважды и при
 * загрузке становился ДВУМЯ разными объектами. Для мира Dohna это неверно:
 * `receiver` делегата, контекст игры, сцены — общие узлы графа, и игра
 * сравнивает их по тождеству.
 *
 * Две таблицы живут ровно одну операцию (save_struct_list / load_struct_list):
 *   сохранение — heap-слот объекта → индекс записи,
 *   загрузка   — индекс записи → heap-слот объекта.
 * Регистрируем ДО заполнения полей: иначе цикл в графе уходит в бесконечную
 * рекурсию (объект ссылается на себя через сцену).
 */
struct obj_rec_pair { int slot; int32_t rec; };
static struct obj_rec_pair *save_obj_map;
static int save_obj_nr;
static struct obj_rec_pair *load_rec_map;
static int load_rec_nr;

static int32_t save_obj_lookup(int slot)
{
	for (int i = 0; i < save_obj_nr; i++) {
		if (save_obj_map[i].slot == slot)
			return save_obj_map[i].rec;
	}
	return -1;
}

static void save_obj_remember(int slot, int32_t rec)
{
	save_obj_map = xrealloc_array(save_obj_map, save_obj_nr, save_obj_nr + 1,
	                              sizeof(*save_obj_map));
	save_obj_map[save_obj_nr].slot = slot;
	save_obj_map[save_obj_nr].rec = rec;
	save_obj_nr++;
}

static int load_rec_lookup(int32_t rec)
{
	for (int i = 0; i < load_rec_nr; i++) {
		if (load_rec_map[i].rec == rec)
			return load_rec_map[i].slot;
	}
	return -1;
}

static void load_rec_remember(int32_t rec, int slot)
{
	load_rec_map = xrealloc_array(load_rec_map, load_rec_nr, load_rec_nr + 1,
	                              sizeof(*load_rec_map));
	load_rec_map[load_rec_nr].rec = rec;
	load_rec_map[load_rec_nr].slot = slot;
	load_rec_nr++;
}

/*
 * ★ИНДЕКСЫ ИЗ ФАЙЛА — ЭТО ВХОДНЫЕ ДАННЫЕ, А НЕ ФАКТ.
 *
 * Загрузчик разыменовывал `struct_defs[rec->struct_index]`, `keyvals[...]`,
 * `records[...]`, `strings[...]` прямо по числу из сейва: у битого файла это
 * чтение за границей массива — то есть падение или, хуже, молча прочитанный
 * мусор, который дальше выглядит как «странные данные игры». Аксессоры ниже
 * возвращают NULL и один раз говорят, ЧТО именно вышло за диапазон; вызывающий
 * дальше идёт своей веткой «данных нет».
 */
// Взводится любым битым индексом; вызывающий (load_struct_list / load_globals)
// по нему отвечает игре «файл не прочитан», а не отдаёт полупустые данные —
// иначе игра идёт дальше с дырами и виснет на чёрном экране (замер: битый
// struct_index в Achievement.asd, титул не появлялся).
static bool save_file_corrupt = false;

static void save_bad_index(const char *what, int idx, int nr)
{
	save_file_corrupt = true;
	static bool warned = false;
	if (warned)
		return;
	warned = true;
	WARNING("сейв повреждён: индекс %s = %d вне диапазона [0, %d) — "
		"дальше загрузка идёт как при отсутствующих данных", what, idx, nr);
}

static struct gsave_struct_def *sd_get(struct gsave *save, int idx)
{
	if (idx < 0 || idx >= save->nr_struct_defs) {
		save_bad_index("описания структуры", idx, save->nr_struct_defs);
		return NULL;
	}
	return &save->struct_defs[idx];
}

static struct gsave_record *rec_get(struct gsave *save, int idx)
{
	if (idx < 0 || idx >= save->nr_records) {
		save_bad_index("записи", idx, save->nr_records);
		return NULL;
	}
	return &save->records[idx];
}

static struct gsave_keyval *kv_get(struct gsave *save, int idx)
{
	if (idx < 0 || idx >= save->nr_keyvals) {
		save_bad_index("поля", idx, save->nr_keyvals);
		return NULL;
	}
	return &save->keyvals[idx];
}

// Класс записи (v7+ — из таблицы описаний, раньше — именем в самой записи).
// Отдельно, потому что у битого файла описания может не оказаться, и тогда имя
// брать неоткуда: `ain_get_struct` с NULL-ключом падает внутри хеш-таблицы.
static int save_rec_struct(struct gsave *save, struct gsave_record *rec)
{
	if (!rec)
		return -1;
	char *name = rec->struct_name;
	if (save->version >= 7) {
		struct gsave_struct_def *d = sd_get(save, rec->struct_index);
		name = d ? d->name : NULL;
	}
	return name ? ain_get_struct(ain, name) : -1;
}

static struct string *save_str_get(struct gsave *save, int idx)
{
	if (idx < 0 || idx >= save->nr_strings || !save->strings[idx]) {
		save_bad_index("строки", idx, save->nr_strings);
		return NULL;
	}
	return save->strings[idx];
}

static void obj_maps_reset(void)
{
	free(save_obj_map); save_obj_map = NULL; save_obj_nr = 0;
	free(load_rec_map); load_rec_map = NULL; load_rec_nr = 0;
}

/*
 * ОБЪЯВЛЕННЫЙ ТИП ЭЛЕМЕНТА поля, которое пишется прямо сейчас (из .ain), либо
 * AIN_VOID, если контекста нет. Нужен ровно для перечислений: в рантайме
 * `array<MenuState#92>` живёт как обычный `array<int>` (см. загрузку, ветка
 * AIN_ENUM), и к моменту записи знание о перечислении в странице не осталось —
 * а оригинал пишет в сейв ИМЕННО 92 (`GameContext.m_menuState` из шести
 * элементов, `BattleBonusCalculator.m_bonuses` — замерено на сейвах
 * Windows-сборки; у нас на тех же полях стояло 10).
 */
static enum ain_data_type save_decl_elem = AIN_VOID;

static void save_decl_elem_note(struct ain_type *t)
{
	struct ain_type *inner = t ? t->array_type : NULL;
	save_decl_elem = inner ? inner->data : AIN_VOID;
}

static struct gsave_flat_array *collect_flat_arrays(struct page *page, struct gsave_flat_array *fa,
		struct gsave *save, enum ain_data_type decl_elem)
{
	if (page->array.rank > 1) {
		for (int i = 0; i < page->nr_vars; i++)
			fa = collect_flat_arrays(heap_get_page(page->values[i].i), fa, save, decl_elem);
		return fa;
	}
	enum ain_data_type type = variable_type(page, 0, NULL, NULL);
	// Перечисление возвращаем к объявленному виду: значение то же целое, но
	// тип в файле совпадёт с оригиналом. Обратная сторона (загрузка) уже
	// принимает 92/91 наравне с 10 — см. save_type_norm.
	if (type == AIN_INT && (decl_elem == AIN_ENUM || decl_elem == AIN_ENUM2))
		type = decl_elem;
	/*
	 * ★ЭЛЕМЕНТ-ССЫЛКА: ТИП ПО ОБЪЯВЛЕНИЮ, ЗНАЧЕНИЕ ОДНО НА ЭЛЕМЕНТ.
	 *
	 * Снято с файла достижений ОРИГИНАЛА (`Achievement.asd`,
	 * `AchievementCollection.m_achievements`): у него 65 значений типа 89
	 * (`ref <интерфейс>`). У нас на том же поле было 130 значений — ровно
	 * вдвое больше, потому что у ИНТЕРФЕЙСНОГО элемента два слота страницы
	 * (объект и база интерфейса, см. array_elem_slots), а мы писали по
	 * слотам. Тип при этом стоял 13.
	 *
	 * Правило: `wrap<интерфейс>`/`ref <интерфейс>` → 89, `wrap<структура>` →
	 * 21 (так оригинал пишет `ActionHistory.m_history`), и в обоих случаях
	 * значение — ОДНО на элемент, то есть слот объекта.
	 */
	int es = array_elem_slots(page);
	/*
	 * ★ПРИЗНАК ПАРЫ — ТИП МАССИВА, А НЕ ШИРИНА ЭЛЕМЕНТА. Два слота на элемент
	 * бывают не только у интерфейсной пары: столько же занимает
	 * `array<option<wrap<Структура>>>` (объект и ТЕГ НАЛИЧИЯ, см.
	 * elem_slots_for_type), а таких объявлений у Dohna 32 — списки работников,
	 * предметов, кнопок. По одной ширине они неотличимы: `variable_type`
	 * нулевого слота у обоих отвечает AIN_STRUCT. Схлопни мы option как пару —
	 * тег наличия не попал бы в файл вовсе, а на загрузке его место заняла бы
	 * «база интерфейса» 0, что для option значит «значение ЕСТЬ» (1 = пусто):
	 * все пустые элементы ожили бы ссылками на мёртвые объекты.
	 */
	bool iface_pair = array_iface_pair_type(page->a_type);
	if (type == AIN_STRUCT && iface_pair)
		type = AIN_IFACE;
	else if (type == AIN_STRUCT && decl_elem == AIN_WRAP)
		type = AIN_REF_STRUCT;
	fa->nr_values = iface_pair ? page->nr_vars / (es > 1 ? es : 1) : page->nr_vars;
	fa->type = type;
	fa->values = xcalloc(fa->nr_values, sizeof(struct gsave_array_value));
	if (iface_pair && es > 1) {
		for (int i = 0; i < fa->nr_values; i++) {
			fa->values[i].type = type;
			fa->values[i].value = add_value_to_gsave(AIN_STRUCT,
					page->values[i * es], save);
		}
		return ++fa;
	}
	for (int i = 0; i < page->nr_vars; i++) {
		fa->values[i].type = type;
		fa->values[i].value = add_value_to_gsave(type, page->values[i], save);
	}
	return ++fa;
}

/*
 * ГДЕ МЫ СЕЙЧАС В ГРАФЕ СОХРАНЕНИЯ — «структура.член» последнего шага.
 * Сериализация рекурсивна и обходит весь мир игры, поэтому сообщение «в слоте не
 * структура» без этого контекста бесполезно: неясно, чьё поле виновато.
 */
static const char *save_path_owner, *save_path_field;

static void save_path_note(const char *owner, const char *field)
{
	save_path_owner = owner;
	save_path_field = field;
}

static const char *save_path_str(void)
{
	static char buf[256];
	if (!save_path_owner)
		return "(вне поля структуры)";
	snprintf(buf, sizeof(buf), "поле '%s' структуры %s",
		 save_path_field ? display_sjis0(save_path_field) : "?",
		 display_sjis1(save_path_owner));
	return buf;
}

static int32_t add_value_to_gsave(enum ain_data_type type, union vm_value val, struct gsave *save)
{
	switch (type) {
	/*
	 * ★ДЕЛЕГАТ не сохраняем: в его слоте лежит номер heap-СТРАНИЦЫ (список пар
	 * «объект + метод»), а он привязан к текущему миру. Записав число, мы
	 * получали после загрузки ссылку в пустоту: `Invalid page index` на первом
	 * же `DG_CALL` — у Dohna на `LocalSave@Load`, который сразу после успешной
	 * загрузки зовёт свой `OnLoadEvent`.
	 *
	 * Пусто (−1) здесь безопаснее восстановления: обработчики событий игра
	 * подписывает заново при построении сцен, а вот воскрешать подписки на
	 * объекты старого мира нельзя.
	 */
	case AIN_DELEGATE:
		{
			struct page *page = val.i >= 0 ? heap_get_page(val.i) : NULL;
			if (!page || page->type != DELEGATE_PAGE)
				return -1;
			/*
			 * Пишем ТРОЙКАМИ в обычный массив: (запись получателя, функция,
			 * поколение). Получатель — ИНДЕКС ЗАПИСИ, а не heap-слот: слоты
			 * принадлежат текущему миру, и после загрузки такой номер ведёт в
			 * пустоту (`Invalid page index` на первом же `DG_CALL`). Благодаря
			 * реестру тождества получатель совпадёт с тем же объектом, что
			 * восстановлен по другим ссылкам.
			 *
			 * Окружение лямбды (`env`, четвёртый слот) НЕ сохраняем: это
			 * локальная страница функции, которой после загрузки уже нет.
			 */
			int n = delegate_numof(page);
			struct gsave_array array = {
				.rank = 1,
				.dimensions = xcalloc(1, sizeof(int32_t)),
				.nr_flat_arrays = 1,
			};
			array.dimensions[0] = n * 3;
			array.flat_arrays = xcalloc(1, sizeof(struct gsave_flat_array));
			array.flat_arrays[0].type = AIN_INT;
			array.flat_arrays[0].nr_values = n * 3;
			array.flat_arrays[0].values = xcalloc(n * 3, sizeof(struct gsave_array_value));
			for (int i = 0; i < n; i++) {
				int obj = -1, fun = -1, env = -1;
				delegate_get(page, i, &obj, &fun, &env);
				int32_t obj_rec = -1;
				if (obj >= 0) {
					union vm_value ov = { .i = obj };
					obj_rec = add_value_to_gsave(AIN_STRUCT, ov, save);
				}
				int32_t v[3] = { obj_rec, fun, 0 };
				for (int k = 0; k < 3; k++) {
					array.flat_arrays[0].values[i*3 + k].type = AIN_INT;
					array.flat_arrays[0].values[i*3 + k].value = v[k];
				}
			}
			return gsave_add_array(save, &array);
		}
	case AIN_VOID:
	case AIN_INT:
	case AIN_BOOL:
	case AIN_FUNC_TYPE:
	case AIN_LONG_INT:
	case AIN_FLOAT:
	// Перечисления Ixseal (`enum#N`) — обычные int в слоте; `AIN_ENUM2` тот же
	// тип в роли элемента массива. Без этой ветки поле уходило в default и
	// сохранялось как -1: у Dohna так терялась ФАЗА ИГРЫ (`GamePhase#92`), и
	// после загрузки `DohnaDohna@RunGame` крутил `SWITCH` по пустому значению —
	// экран оставался чёрным, а игра живой (стек по SIGUSR1: Phase::get ←
	// RunGame ← Run ← game_main).
	case AIN_ENUM:
	case AIN_ENUM2:
		return val.i;
	case AIN_STRING:
		// Пустой слот (−1) — законное состояние поля: строку туда ещё не
		// клали, либо это `option<string>` без значения. `heap[-1].s` уносил
		// движок в SEGV прямо в gsave_add_string при сохранении Dohna.
		if (val.i < 0)
			return gsave_add_string(save, &EMPTY_STRING);
		return gsave_add_string(save, heap[val.i].s);
	case AIN_STRUCT:
		{
			// Та же оговорка: объектное поле может быть пустым (−1).
			if (val.i < 0)
				return -1;
			struct page *page = heap_get_page(val.i);
			if (!page)
				return -1;
			/*
			 * ★НЕ СТРУКТУРА В ОБЪЕКТНОМ СЛОТЕ — называем, ЧТО именно сохраняли.
			 * Голый assert сообщал только факт: с ассертами игра падала на
			 * автосохранении, без них (release) сериализация шла по чужой
			 * странице и в сейв уезжал мусор. Контекст (структура и член, а для
			 * делегата — его receiver) ставит save_path_note ниже.
			 */
			if (page->type != STRUCT_PAGE) {
				strict_or_warn("сохранение",
					"в объектном слоте %d не структура, а %s — %s; поле сохранено пустым",
					val.i, pagetype_string(page->type), save_path_str());
				return -1;
			}
			// Этот объект уже записан — отдаём ТУ ЖЕ запись, чтобы после
			// загрузки он остался одним объектом, а не размножился по числу
			// ссылок (см. «тождество объектов» выше).
			int32_t known = save_obj_lookup(val.i);
			if (known >= 0)
				return known;
			struct ain_struct *st = &ain->structures[page->index];
			assert(st->nr_members == page->nr_vars);
			struct gsave_record rec = {
				.type = GSAVE_RECORD_STRUCT,
				.struct_name = strdup(st->name),
				.nr_indices = st->nr_members,
				.indices = xcalloc(st->nr_members, sizeof(int32_t)),
			};
			if (save->version >= 7) {
				rec.struct_index = gsave_get_struct_def(save, st->name);
				if (rec.struct_index < 0)
					rec.struct_index = gsave_add_struct_def(save, st);
			}
			// Запись регистрируем ДО заполнения полей: граф циклический
			// (объект → сцена → обратно), иначе рекурсия не кончится.
			int32_t rec_no = gsave_add_record(save, &rec);
			save_obj_remember(val.i, rec_no);
			struct gsave_record *recp = &save->records[rec_no];
			for (int i = 0; i < page->nr_vars; i++) {
				enum ain_data_type type = st->members[i].type.data;
				int32_t value;
				save_path_note(st->name, st->members[i].name);
				save_decl_elem_note(&st->members[i].type);
				/*
				 * Обёртки Ixseal — `wrap<T>` (82), `option<T>` (86) и ссылка на
				 * интерфейс (89). В слоте у них либо heap-слот объекта, либо
				 * скаляр (для `option<int>`), а «пусто» = −1. Уходя в default,
				 * они сохранялись как −1 ВСЕГДА: после загрузки объектные поля
				 * оказывались пустыми, и первая же попытка игры разыменовать их
				 * валила движок в отладочный REPL («Out of bounds page index»
				 * в `CPartsMessageManager@GetFunctionSet` при пересборке
				 * интерфейса домашней сцены).
				 *
				 * Что внутри — решает объявленный тип, а не догадка по значению:
				 * читать `heap_get_page` от скаляра нельзя. Тег `option`
				 * (последний слот) и база интерфейса (второй слот) объявлены
				 * отдельными членами-филлерами `<void>` и сохраняются числом
				 * сами по себе.
				 */
				if (type == AIN_WRAP || type == AIN_OPTION || type == AIN_IFACE) {
					struct ain_type *inner = st->members[i].type.array_type;
					/*
					 * ★`AIN_WRAP` внутри — ТОЖЕ ОБЪЕКТ. Без него
					 * `option<wrap<T>>` уходил в скалярную ветку и терялся:
					 * в логе прогона это 20 предупреждений «тип wrap<?> не
					 * восстанавливается — поле осталось пустым»
					 * (`PlayerAction.m_targets`, `m_innerAction`). На пустых
					 * полях потери не видно, а сейв посреди боя терял цели.
					 */
					bool is_obj = type == AIN_IFACE || !inner
						|| inner->data == AIN_STRUCT
						|| inner->data == AIN_WRAP
						|| inner->data == AIN_IFACE
						|| inner->data == AIN_IFACE_WRAP;
					/*
					 * ★ПУСТОЙ `option` СОХРАНЯЕМ КАК ПУСТОЙ — по ТЕГУ, а не по
					 * содержимому слота. У пустого option там лежит не −1, а мусор
					 * от последней записи (сайт вида `X_REF 2; A_REF; X_OP_SET`
					 * оставляет номер уже освобождённого временного слота, §5eu).
					 * Без проверки сериализация шла по этому номеру как по
					 * структуре: с ассертами — падение `page->type == STRUCT_PAGE`
					 * на автосохранении, без них — в сейв уезжал ЧУЖОЙ объект.
					 */
					if (type == AIN_OPTION && !option_var_has_value(page, i))
						value = -1;
					else if (!is_obj)
						value = add_value_to_gsave(inner->data, page->values[i], save);
					else if (page->values[i].i < 0)
						value = -1;
					else
						value = add_value_to_gsave(AIN_STRUCT, page->values[i], save);
				} else {
					value = add_value_to_gsave(type, page->values[i], save);
				}
				struct gsave_keyval kv = {
					.name = strdup(st->members[i].name),
					.type = type,
					.value = value,
				};
				// ★Через recp, а не через локальную rec: gsave_add_record уже
				// скопировал структуру в массив сейва, и наши indices должны
				// попасть именно в неё. Массив records при этом может быть
				// перевыделен вложенными записями — берём указатель заново.
				save->records[rec_no].indices[i] = gsave_add_keyval(save, &kv);
			}
			(void)recp;
			return rec_no;
		}
	// Generic-массив (array<?>, Ixseal): контейнер сериализуется так же, как
	// типизированный — ранг, размеры и тип элементов берутся из самой страницы,
	// объявленный тип здесь не нужен. Расходятся они только при ЗАГРУЗКЕ
	// (см. gsave_to_vm_value).
	case AIN_ARRAY:
	case AIN_ARRAY_TYPE:
		{
			struct page *page = heap_get_page(val.i);
			if (!page) {
				struct gsave_array array = { .rank = -1 };
				return gsave_add_array(save, &array);
			}
			assert(page->type == ARRAY_PAGE);
			/*
			 * ★`array<option<T>>` В СЕЙВ НЕ ИДЁТ — И ЭТО ЧЕСТНЕЕ ПОЛУМЕР.
			 *
			 * Элемент такого массива занимает несколько слотов (значение…, тег), а
			 * формат сейва раскладку элемента не хранит: тип угадывается по
			 * ПЕРВОМУ значению плоского массива, и при загрузке контейнер всегда
			 * восстанавливается с шагом 1. То есть круговой прогон невозможен в
			 * принципе, чем его ни заполняй: сериализация «по слотам» уводит
			 * тег-слот (число 0) в объектную ветку — там `heap_get_page(0)` даёт
			 * ГЛОБАЛЬНУЮ страницу; попытка писать пустым значением −1 ломает
			 * загрузку (`save->records[-1]` — чтение за границей массива записей).
			 * Поэтому сохраняем такой массив ПУСТЫМ и говорим об этом вслух:
			 * потеря видна сразу, а не всплывает «пустым пулом после загрузки».
			 */
			if (page->a_type == AIN_OPTION) {
				static bool warned = false;
				if (!warned) {
					warned = true;
					strict_or_warn("сохранение",
						"array<option<...>> не сериализуется (формат не хранит "
						"раскладку элемента) — массив сохранён пустым: %s",
						save_path_str());
				}
				struct gsave_array array = { .rank = -1 };
				return gsave_add_array(save, &array);
			}
			struct gsave_array array = {
				.rank = page->array.rank,
				.dimensions = xcalloc(page->array.rank, sizeof(int32_t)),
				.nr_flat_arrays = 1
			};
			for (struct page *p = page;; p = heap_get_page(page->values[0].i)) {
				array.dimensions[p->array.rank - 1] = p->nr_vars;
				if (p->array.rank == 1)
					break;
				array.nr_flat_arrays *= p->nr_vars;
			}
			array.flat_arrays = xcalloc(array.nr_flat_arrays, sizeof(struct gsave_flat_array));
			// Контекст объявления забираем и ГАСИМ: он относится к этому
			// массиву, а вложенные записи (элементы-структуры) поставят свой.
			enum ain_data_type decl_elem = save_decl_elem;
			save_decl_elem = AIN_VOID;
			collect_flat_arrays(page, array.flat_arrays, save, decl_elem);
			return gsave_add_array(save, &array);
		}
	/*
	 * ★`ref <структура>` — ВЛАДЕЮЩАЯ ССЫЛКА В ОДИН СЛОТ, и сохранять её надо как
	 * объект. Уходя в общую ref-ветку («не сериализуем»), она после загрузки
	 * становилась −1, а игра этого не проверяет НИКОГДА: у неё ссылка не может
	 * быть пустой, раз конструктор её заполнил.
	 *
	 * Живой случай (§5ey): `parts::detail::CSpriteParts { array<int> <vtable>;
	 * ref CParts m_parts; }` — обёртка части, которую держит каждый компонент
	 * интерфейса. `MoneyView` живёт в `GameContext`, то есть попадает в сейв
	 * вместе с ним; после загрузки его `m_parts` пуст, и ПЕРВОЕ ЖЕ начисление
	 * денег (`GameContext@Money::set` → `MoneyView@Set` → `Motion::Create` →
	 * `CSpriteParts@IsValid::get`) роняло движок «Out of bounds heap index: −1/0».
	 * Ловилось это и покупкой в магазине, и в конце фазы hustling
	 * (`ShowIncomeToMoney`) — там пользователь и увидел «краш в конце подсчёта».
	 * Замер, отделивший «не заполнили» от «обнулили»: XSYS4_FIELD_WATCH по полю
	 * `m_parts` дал 103 879 записей за прогон и НИ ОДНОЙ для упавшего объекта —
	 * значит объект пришёл не из конструктора, а из сейва.
	 *
	 * Тождество объектов даёт общий реестр записей (`save_obj_lookup`): если на
	 * тот же объект ссылается ещё и обычное поле, запись будет ОДНА.
	 *
	 * Остальные `ref` (на скаляр, на элемент массива) в один слот не укладываются
	 * — им нужна пара (страница, индекс), которой в формате нет, — и по-прежнему
	 * не сохраняются.
	 */
	case AIN_REF_STRUCT:
		if (val.i < 0)
			return -1;
		return add_value_to_gsave(AIN_STRUCT, val, save);
	case AIN_REF_INT:
	case AIN_REF_FLOAT:
	case AIN_REF_STRING:
	case AIN_REF_ARRAY_INT:
	case AIN_REF_ARRAY_FLOAT:
	case AIN_REF_ARRAY_STRING:
	case AIN_REF_ARRAY_STRUCT:
	case AIN_REF_FUNC_TYPE:
	case AIN_REF_ARRAY_FUNC_TYPE:
	case AIN_REF_BOOL:
	case AIN_REF_ARRAY_BOOL:
	case AIN_REF_LONG_INT:
	case AIN_REF_ARRAY_LONG_INT:
	case AIN_REF_DELEGATE:
	case AIN_REF_ARRAY_DELEGATE:
	case AIN_REF_ARRAY:
		return -1;
	default:
		strict_or_warn("сохранение", "тип %s не сериализуется — поле потеряно",
		                ain_strtype(ain, type, -1));
		return -1;
	}
}

/*
 * ВЕРСИЯ 9 — та сборка движка, которая держит комментарий слота ВНУТРИ файла сейва
 * (FINDINGS §5cp). По версии `.ain` её не отличить: и Dohna, и Haha Ranman — 14, а
 * оригиналы пишут РАЗНОЕ (замер по их же файлам: `SaveData1000.asd` у Dohna — 9,
 * `AFConfig.asd` у Haha Ranman — 7). Разделяет их состав библиотеки System: у Dohna
 * объявлены `SerializeStruct`/`WriteSerializeStructComment`, у Haha Ranman этих
 * функций нет вовсе. То есть комментарий внутри файла заводят ровно там, где игра
 * умеет его писать, — по этому признаку и выбираем.
 */
static bool game_uses_gsave9(void)
{
	static int cached = -1;
	if (cached >= 0)
		return cached;
	// ★Имя библиотеки в .ain — со СТРОЧНОЙ буквы (`system`), хотя реализация в
	// движке зовётся `System`: по «System» поиск молча не находил ничего, и сейв
	// продолжал писаться седьмой версией.
	int libno = ain_get_library(ain, "system");
	if (libno < 0)
		libno = ain_get_library(ain, "System");
	cached = libno >= 0
		&& ain_get_library_function(ain, libno, "WriteSerializeStructComment") >= 0;
	return cached;
}

static int get_gsave_version(void)
{
	if (AIN_VERSION_GTE(ain, 6, 0)) {
		if (config.vm_name)
			return 5;
		return game_uses_gsave9() ? 9 : 7;
	}
	return AIN_VERSION_GTE(ain, 5, 0) ? 5 : 4;
}

int save_globals(const char *keyname, const char *filename, const char *group_name, int *n_out)
{
	int group = -1;
	int nr_vars = 0;
	if (group_name) {
		if ((group = get_group_index(group_name)) < 0) {
			WARNING("Unregistered global group: %s", display_sjis0(group_name));
		} else {
			for (int i = 0; i < ain->nr_globals; i++) {
				if (ain->globals[i].group_index == group)
					nr_vars++;
			}
		}
	} else {
		nr_vars = ain->nr_globals;
	}

	struct gsave *save = gsave_create(get_gsave_version(), keyname, ain->nr_globals, group_name);
	gsave_add_globals_record(save, nr_vars);

	/*
	 * ★Реестр тождества — НА ОПЕРАЦИЮ. Групповые сейвы пишутся сериями
	 * (AFCommon → AFInfo → ...), и без сброса объект, попавший в реестр при
	 * записи ПЕРВОГО файла, во втором отдавался индексом записи, которой в
	 * ЭТОМ файле нет — стартовые бэкапы Haha Ranman выходили битыми.
	 */
	obj_maps_reset();

	if (nr_vars > 0) {
		struct gsave_global *global = save->globals;
		for (int i = 0; i < ain->nr_globals; i++) {
			if (group >= 0 && ain->globals[i].group_index != group)
				continue;
			global->name = strdup(ain->globals[i].name);
			global->type = ain->globals[i].type.data;
			// v9: параметр типа — тип содержимого ТОЛЬКО у AIN_OPTION, иначе
			// дублирует тип (правило снято подсчётом пар в сейве оригинала).
			global->type_param = (ain->globals[i].type.data == AIN_OPTION
					&& ain->globals[i].type.array_type)
				? ain->globals[i].type.array_type->data
				: ain->globals[i].type.data;
			save_decl_elem_note(&ain->globals[i].type);
			global->value = add_value_to_gsave(global->type, global_get(i), save);
			global++;
		}
	}

	char *path = savedir_path(filename);
	FILE *fp = file_open_utf8(path, "wb");
	if (!fp) {
		WARNING("Failed to open save file %s: %s", display_utf0(path), strerror(errno));
		free(path);
		gsave_free(save);
		return 0;
	}
	free(path);

	bool encrypt = !AIN_VERSION_GTE(ain, 6, 0);
	int compression_level = AIN_VERSION_GTE(ain, 6, 0) ? 1 : 9;
	enum savefile_error error = gsave_write(save, fp, encrypt, compression_level);
	if (error != SAVEFILE_SUCCESS)
		WARNING("Failed to write save file: %s", savefile_strerror(error));
	fclose(fp);
	gsave_free(save);
	if (n_out)
		*n_out = nr_vars;
	return error == SAVEFILE_SUCCESS;
}

static union vm_value json_to_vm_value(enum ain_data_type type, enum ain_data_type struct_type, int array_rank, cJSON *json);

void get_array_dims(cJSON *json, int rank, union vm_value *dims)
{
	cJSON *array = json;
	for (int i = 0; i < rank; i++) {
		dims[i].i = cJSON_GetArraySize(array);
		array = cJSON_GetArrayItem(array, 0);
	}
}

void json_load_page(struct page *page, cJSON *vars, bool call_dtors)
{
	int i = 0;
	cJSON *v;
	cJSON_ArrayForEach(v, vars) {
		int struct_type, array_rank;
		enum ain_data_type data_type = variable_type(page, i, &struct_type, &array_rank);
		union vm_value val = json_to_vm_value(data_type, struct_type, array_rank, v);
		if (call_dtors)
			variable_set(page, i, data_type, val);
		else
			page->values[i] = val;
		i++;
	}
}

static union vm_value json_to_vm_value(enum ain_data_type type, enum ain_data_type struct_type, int array_rank, cJSON *json)
{
	char *str;
	int slot;
	union vm_value *dims;
	struct page *page;
	switch (type) {
	case AIN_INT:
	case AIN_BOOL:
	case AIN_LONG_INT:
		if (!cJSON_IsNumber(json)) {
			invalid_save_data("Not a number", json);
			return vm_int(0);
		}
		return vm_int(json->valueint);
	case AIN_FLOAT:
		if (!cJSON_IsNumber(json)) {
			invalid_save_data("Not a number", json);
			return vm_float(0);
		}
		return vm_float(json->valuedouble);
	case AIN_STRING:
		slot = heap_alloc_slot(VM_STRING);
		if (!cJSON_IsString(json)) {
			invalid_save_data("Not a string", json);
			heap[slot].s = string_ref(&EMPTY_STRING);
			return vm_int(slot);
		}
		str = cJSON_GetStringValue(json);
		if (!str[0]) {
			heap[slot].s = string_ref(&EMPTY_STRING);
		} else {
			heap[slot].s = make_string(str, strlen(str));
		}
		return vm_int(slot);
	case AIN_STRUCT:
		slot = alloc_struct(struct_type);
		if (!cJSON_IsArray(json) || cJSON_GetArraySize(json) != ain->structures[struct_type].nr_members) {
			invalid_save_data("Not an array", json);
		} else {
			json_load_page(heap[slot].page, json, false);
		}
		return vm_int(slot);
	case AIN_ARRAY_TYPE:
		slot = heap_alloc_slot(VM_PAGE);
		if (cJSON_IsNull(json)) {
			heap[slot].page = NULL;
			return vm_int(slot);
		}
		if (!cJSON_IsArray(json)) {
			invalid_save_data("Not an array", json);
			heap[slot].page = NULL;
			return vm_int(slot);
		}
		dims = xmalloc(sizeof(union vm_value) * array_rank);
		get_array_dims(json, array_rank, dims);
		page = alloc_array(array_rank, dims, type, struct_type, false);
		heap[slot].page = page;
		free(dims);
		json_load_page(heap[slot].page, json, false);
		return vm_int(slot);
	case AIN_REF_TYPE:
		return vm_int(-1);
	default:
		strict_or_warn("загрузка", "тип %s не восстанавливается — поле осталось пустым",
		                ain_strtype(ain, type, -1));
		return vm_int(-1);
	}
}

static cJSON *read_save_file(const char *path)
{
	FILE *f;
	long len;
	char *buf;

	if (!(f = file_open_utf8(path, "rb"))) {
		WARNING("Failed to open save file: %s: %s", display_utf0(path), strerror(errno));
		return NULL;
	}

	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);

	buf = xmalloc(len+1);
	buf[len] = '\0';
	if (fread(buf, len, 1, f) != 1) {
		WARNING("Failed to read save file: %s", display_utf0(path));
		free(buf);
		return 0;
	}
	fclose(f);

	cJSON *r = cJSON_Parse(buf);
	free(buf);
	return r;
}

static int load_globals_from_json(const char *path, const char *keyname, const char *group_name, int *n)
{
	int retval = 0;
	cJSON *save = read_save_file(path);
	if (!save)
		return 0;
	if (!cJSON_IsObject(save)) {
		invalid_save_data("Not an object", save);
		goto cleanup;
	}

	cJSON *key = cJSON_GetObjectItem(save, "key");
	if (!key || strcmp(keyname, cJSON_GetStringValue(key)))
		VM_ERROR("Attempted to load save data with wrong key: %s", display_sjis0(keyname));

	if (group_name) {
		// TODO?
	}

	cJSON *globals = cJSON_GetObjectItem(save, "globals");
	if (!globals || !cJSON_IsArray(globals)) {
		invalid_save_data("Not an array", globals);
		goto cleanup;
	}

	cJSON *g;
	cJSON_ArrayForEach(g, globals) {
		cJSON *index, *value;
		if (!(index = cJSON_GetObjectItem(g, "index"))) {
			invalid_save_data("Missing index", g);
			goto cleanup;
		}
		if (!(value = cJSON_GetObjectItem(g, "value"))) {
			invalid_save_data("Missing value", g);
			goto cleanup;
		}
		if (!cJSON_IsNumber(index)) {
			invalid_save_data("Not a number", index);
			goto cleanup;
		}

		int i = index->valueint;
		if (i < 0 || i > ain->nr_globals) {
			invalid_save_data("Invalid global index", index);
			goto cleanup;
		}
		current_global = i;

		bool call_dtors = false; // Destructors for old objects are not called.
		global_set(i, json_to_vm_value(ain->globals[i].type.data, ain->globals[i].type.struc, ain->globals[i].type.rank, value), call_dtors);
		if (n)
			(*n)++;
	}

	retval = 1;
cleanup:
	cJSON_Delete(save);
	return retval;
}

static int struct_member_index(struct ain_struct *st, const char *name)
{
	for (int i = 0; i < st->nr_members; i++) {
		if (!strcmp(st->members[i].name, name))
			return i;
	}
	return -1;
}

/*
 * `container`/`varno` — ГДЕ лежит восстанавливаемое значение (глобальная,
 * локальная или структурная страница и номер переменной в ней). Нужны ровно
 * одному случаю — generic-массиву (тип 79): у него тип элемента не выводится
 * ни из объявления поля (там просто «array<?>»), ни из значения, и раньше его
 * УГАДЫВАЛИ по сейву. Через контейнер он берётся из .ain, как это делает
 * variable_initval_var. Кто контейнера не знает (вложенный массив) — передаёт
 * NULL/-1 и получает прежнее поведение.
 */
static union vm_value gsave_to_vm_value(struct gsave *save, enum ain_data_type type, int struct_type, int array_rank, int32_t value, struct page *container, int varno);

/*
 * ПЕРЕЧИСЛЕНИЕ РАВНО ЦЕЛОМУ при сверке типов. В сейве Windows-версии Dohna элемент
 * массива и поле структуры могут нести тип 92/91 (`AIN_ENUM`/`AIN_ENUM2`), тогда как
 * страница в игре объявлена как обычный int (перечисление в System4 хранится целым, и
 * generic-массив из них мы и создаём как `array<int>`). Без нормализации сверка
 * `data_type != тип из сейва` роняла загрузку насмерть:
 * `gsave_load_array: Bad save file: unexpected array element type` при загрузке
 * `SaveObject@Load` ← `LocalSave@Load`.
 */
static enum ain_data_type save_type_norm(enum ain_data_type t)
{
	/*
	 * ★`ref <структура>` РАВЕН структуре по той же причине. Элемент
	 * `array<wrap<T>>` оригинал пишет типом 21 (`AIN_REF_STRUCT`), а страница
	 * в игре объявляет его обычным объектным слотом (13): значение в обоих
	 * случаях — индекс записи. Замерено на сейве Windows-сборки:
	 * `ActionHistory.m_history`, `BattlePlayerCollection.m_player`,
	 * `PlayerActionCollection.m_actions` — все с типом элементов 21.
	 */
	if (t == AIN_REF_STRUCT)
		return AIN_STRUCT;
	// Интерфейсный элемент в файле — 89, а нулевой слот пары страница
	// объявляет структурой; значение и там, и там индекс записи.
	if (t == AIN_IFACE)
		return AIN_STRUCT;
	return (t == AIN_ENUM || t == AIN_ENUM2) ? AIN_INT : t;
}

/*
 * База интерфейса для пары (объект, база): позиция таблицы методов интерфейса
 * `iface` в структуре объекта. В рантайме её кладёт X_REF/присваивание
 * (src/vm.c, поиск по `interfaces[] = {struct_type, vtable_offset}`), а при
 * загрузке сейва в файле лежит ТОЛЬКО объект — базу надо вывести самим.
 */
static int32_t iface_base_for(int obj_slot, int iface_struct)
{
	if (iface_struct < 0 || !heap_index_valid(obj_slot))
		return 0;
	struct page *p = heap[obj_slot].page;
	if (!p || p->type != STRUCT_PAGE || p->index < 0 || p->index >= ain->nr_structures)
		return 0;
	struct ain_struct *s = &ain->structures[p->index];
	for (int i = 0; i < s->nr_interfaces; i++) {
		if (s->interfaces[i].struct_type == iface_struct)
			return s->interfaces[i].vtable_offset;
	}
	return 0;
}

// Конкретный класс элемента — по записи сейва (ссылка законно держит производную,
// а в странице стоит либо интерфейс, либо класс первого элемента).
static int elem_struct_by_record(struct gsave *save, int32_t v, int dflt)
{
	if (v < 0 || v >= save->nr_records)
		return dflt;
	int actual = save_rec_struct(save, &save->records[v]);
	return actual >= 0 ? actual : dflt;
}

static struct gsave_flat_array *gsave_load_array(struct gsave *save, struct page *page, struct gsave_flat_array *flat_array)
{
	assert(page->type == ARRAY_PAGE);
	if (page->array.rank > 1) {
		for (int i = 0; i < page->nr_vars; i++)
			flat_array = gsave_load_array(save, heap_get_page(page->values[i].i), flat_array);
		return flat_array;
	}

	/*
	 * ★ПАРА (объект, база интерфейса) — ОДНО значение в файле на ДВА слота.
	 * Так пишет оригинал, так теперь пишем и мы (см. collect_flat_arrays).
	 * Условие — ТИП массива: по одной ширине элемента пару не отличить от
	 * `array<option<...>>`, у которого второй слот это тег наличия.
	 */
	int es = array_elem_slots(page);
	/*
	 * XSYS4_ARRAY_LOAD_TRACE=<подстрока типа> — форма контейнера, в который
	 * ложатся значения из сейва. Отвечает на вопрос, который иначе не задать:
	 * восстановлен ли интерфейсный массив ПАРАМИ или схлопнут в однослотовые
	 * элементы (тогда Array.First отдаёт наружу один слот вместо пары, и стек
	 * вызывающего разъезжается — см. §5fb-13).
	 */
	{
		static const char *alt = (const char *)1;
		if (alt == (const char *)1)
			alt = getenv("XSYS4_ARRAY_LOAD_TRACE");
		if (alt && *alt) {
			const char *sn = (page->array.struct_type >= 0
					  && page->array.struct_type < ain->nr_structures)
				? ain->structures[page->array.struct_type].name : "";
			if (strstr(sn, alt))
				NOTICE("ARRLOAD '%s': a_type=%d слотов на элемент %d, "
				       "слотов страницы %d, значений в файле %d (тип %d)",
				       sn, page->a_type, es, page->nr_vars,
				       flat_array->nr_values, flat_array->type);
		}
	}
	/*
	 * ПАРНАЯ страница, а значений СТОЛЬКО ЖЕ, сколько слотов, — это запись
	 * оригинала «по слотам»: `объект, −1, объект, −1 …`. Кладём значения 1:1,
	 * а вместо −1 восстанавливаем настоящую базу по таблице интерфейсов объекта
	 * (с нулём вместо неё диспетчеризация уходит в чужой метод, см. §5fb-13).
	 */
	if (array_iface_pair_type(page->a_type) && es == 2
			&& flat_array->nr_values == page->nr_vars) {
		int iface = page->array.struct_type;
		for (int i = 0; i < page->nr_vars; i += 2) {
			int32_t v = flat_array->values[i].value;
			union vm_value val = gsave_to_vm_value(save, AIN_STRUCT,
					elem_struct_by_record(save, v, iface), 0, v, page, i);
			page->values[i] = val;
			int32_t base = flat_array->values[i + 1].value;
			page->values[i + 1] = vm_int(base >= 0 ? base
					: iface_base_for(val.i, iface));
		}
		return flat_array + 1;
	}
	if (array_iface_pair_type(page->a_type) && es == 2
			&& flat_array->nr_values == page->nr_vars / 2) {
		/*
		 * ★ТИП ОБЪЕКТА — ИЗ ЗАПИСИ, ИНТЕРФЕЙС ДЛЯ БАЗЫ — ИЗ СТРАНИЦЫ.
		 *
		 * `page->array.struct_type` у generic-контейнера — это КОНКРЕТНЫЙ класс,
		 * подобранный по первому непустому элементу (см. gsave_to_vm_value), а
		 * ссылка законно держит производную: подставь мы его каждому элементу —
		 * загрузка упала бы на сверке имён («structure name mismatch»), как это
		 * уже было с `ref`-элементами. Поэтому класс уточняем поэлементно по
		 * записи, ровно как в ветке AIN_REF_STRUCT ниже.
		 *
		 * База интерфейса ищется в таблице интерфейсов объекта по ИНТЕРФЕЙСУ.
		 * Если в странице стоит конкретный класс, а не интерфейс, искать нечего —
		 * `iface_base_for` вернёт 0, что верно для единственного интерфейса
		 * (он лежит по смещению 0) и остаётся приближением для класса с
		 * несколькими: объявленного типа контейнера на этом шаге нет.
		 */
		int page_struct = page->array.struct_type;
		bool page_is_iface = page_struct >= 0 && page_struct < ain->nr_structures
			&& ain->structures[page_struct].is_interface;
		for (int i = 0; i < flat_array->nr_values; i++) {
			int32_t v = flat_array->values[i].value;
			int elem_struct = page_struct;
			if (v >= 0 && v < save->nr_records) {
				int actual = save_rec_struct(save, &save->records[v]);
				if (actual >= 0)
					elem_struct = actual;
			}
			union vm_value val = gsave_to_vm_value(save, AIN_STRUCT, elem_struct,
					0, v, page, i * 2);
			page->values[i * 2] = val;
			page->values[i * 2 + 1] =
				vm_int(page_is_iface ? iface_base_for(val.i, page_struct) : 0);
		}
		return flat_array + 1;
	}

	// Числа в сообщении: без них «не сходится» не отличить от «не тот массив»
	// — а разошедшийся массив ещё надо найти среди сотен.
	if (page->nr_vars != flat_array->nr_values)
		VM_ERROR("Bad save file: unexpected number of array elements "
		         "(страница %d слотов, тип %d; в файле %d значений типа %d)",
		         page->nr_vars, page->a_type,
		         flat_array->nr_values, flat_array->type);
	for (int i = 0; i < page->nr_vars; i++) {
		int struct_type, array_rank;
		enum ain_data_type data_type = variable_type(page, i, &struct_type, &array_rank);
		if (save_type_norm(data_type) != save_type_norm(flat_array->values[i].type))
			VM_ERROR("Bad save file: unexpected array element type");
		/*
		 * ★У ЭЛЕМЕНТА-ССЫЛКИ ТИП БЕРЁМ ИЗ ЗАПИСИ, а не из объявления.
		 *
		 * Ровно та же природа, что у `ref`-поля структуры (см. ветку
		 * AIN_REF_STRUCT в gsave_fill_struct): ссылка законно держит
		 * ПРОИЗВОДНУЮ, и подстановка объявленного типа роняет загрузку на
		 * сверке имён — «Bad save file: structure name mismatch» в
		 * `AchievementCollection@Load` на старте игры (элементы там разных
		 * наследников `IAchievement`). Тип элемента страницы у нас один на
		 * весь массив, поэтому уточняем поэлементно.
		 */
		if (flat_array->values[i].type == AIN_REF_STRUCT
				&& flat_array->values[i].value >= 0
				&& flat_array->values[i].value < save->nr_records) {
			int actual = save_rec_struct(save,
					&save->records[flat_array->values[i].value]);
			if (actual >= 0)
				struct_type = actual;
		}
		union vm_value val = gsave_to_vm_value(save, data_type, struct_type, array_rank,
		                                       flat_array->values[i].value, page, i);
		page->values[i] = val;
		// Та же проверка, что и у полей структуры: 0 в объектном слоте —
		// heap-слот глобальной страницы, то есть чужое число.
		{
			enum ain_data_type t = data_type;
			bool objref = t == AIN_STRING || t == AIN_STRUCT
				|| t == AIN_ARRAY || t == AIN_ARRAY_INT
				|| t == AIN_ARRAY_STRING || t == AIN_ARRAY_STRUCT;
			if (objref && val.i == 0)
				strict_or_warn("загрузка",
					"элемент %d массива (%s) восстановлен как 0",
					i, ain_strtype(ain, t, -1));
		}
	}
	return flat_array + 1;
}

/*
 * Заполнить УЖЕ СУЩЕСТВУЮЩУЮ страницу структуры значениями из записи сейва.
 *
 * ★Ключевое: вложенные объекты не пересоздаются, если приёмник уже держит объект
 * той же структуры — тогда заполняется ОН САМ. Прежняя версия всегда создавала
 * новые страницы и подставляла их в поля, и граф после загрузки расходился с тем,
 * на что ссылались игра и PartsEngine: они продолжали держать прежние объекты.
 * Ловилось это не там, где ломалось, — падало на посторонней строке `Schedule1`
 * (литерал в SceneHome@SetSchedule, которого в сейве нет вовсе), потому что
 * осиротевшие слоты уходили в переиспользование.
 *
 * Что исключение отдельных типов не помогало, а исключение ВСЕХ объектных полей
 * помогало, и указало на идентичность графа, а не на конкретный тип.
 */
static void gsave_fill_struct(struct gsave *save, struct page *page,
		struct gsave_record *rec, struct gsave_struct_def *sd,
		const char *struct_name, struct ain_struct *st)
{
	/*
	 * XSYS4_STRUCT_TRACE=<подстрока имени структуры> — ЧТО ИМЕННО пришло в поля
	 * из сейва, со строками в текстовом виде. Нужна, когда сравнение в игре идёт
	 * по строковому ключу и «не совпало» невозможно отличить от «прочиталось не
	 * то» (§5fb-13: предикат ищет достижение по `<Id>` состояния).
	 */
	{
		static const char *sT = (const char *)1;
		if (sT == (const char *)1)
			sT = getenv("XSYS4_STRUCT_TRACE");
		if (sT && *sT && struct_name && strstr(struct_name, sT)) {
			NOTICE("STRUCT '%s': полей %d", struct_name, rec->nr_indices);
			for (int i = 0; i < rec->nr_indices; i++) {
				struct gsave_keyval *kv = kv_get(save, rec->indices[i]);
				if (!kv)
					continue;
				const char *fn = sd ? sd->fields[i].name : kv->name;
				enum ain_data_type ft = sd ? sd->fields[i].type : kv->type;
				if (ft == AIN_STRING && kv->value >= 0
						&& kv->value < save->nr_strings
						&& save->strings[kv->value])
					NOTICE("   %s (тип %d) = \"%s\"", fn ? fn : "?", ft,
					       display_sjis0(save->strings[kv->value]->text));
				else
					NOTICE("   %s (тип %d) = %d", fn ? fn : "?", ft, kv->value);
			}
		}
	}
	for (int i = 0; i < rec->nr_indices; i++) {
		struct gsave_keyval *kv = kv_get(save, rec->indices[i]);
		if (!kv)
			continue;  // битый индекс поля: см. save_bad_index
		const char *field_name = sd ? sd->fields[i].name : kv->name;
		enum ain_data_type field_type = sd ? sd->fields[i].type : kv->type;
		/*
		 * ПОЛЕ БЕЗ ИМЕНИ — сопоставляем ПО ПОЗИЦИИ. Windows-сборка Dohna
		 * (сейв версии 9) не пишет имя у полей-делегатов: у `GameContext`
		 * восьмое поле имеет тип 63 (`AIN_DELEGATE`) и пустое имя, хотя в .ain
		 * это `DG_EventP<int> <OnChangeMoneyEvent>`; у `WorkerHistory` таких
		 * полей два. Порядок полей в struct-def совпадает с порядком членов
		 * структуры (проверено на GameContext: девять полей подряд, тип в тип),
		 * поэтому позиция — надёжный ключ. Без этого поля просто терялись.
		 */
		/*
		 * ★А ИМЕНА У СЛУЖЕБНЫХ ПОЛЕЙ НЕ УНИКАЛЬНЫ. Тег-филлер каждого
		 * `option<...>` зовётся в .ain одинаково — `<void>`, — и поиск по
		 * имени отдавал ВСЕГДА ПЕРВЫЙ такой член. У `WorkerCollection`
		 * филлеров два (теги `m_limit` и `ShowcaseId`): тег второго попадал в
		 * слот первого, `m_limit` оказывался «пустым», и следующее сохранение
		 * записывало в файл уже настоящую пустоту — вместимость комнат
		 * терялась НАВСЕГДА (панель показывала `TALENT 3/-1` вместо `3/5`).
		 * Терялись так же рекорд дневного дохода и скидки магазина.
		 *
		 * Поэтому сначала пробуем ПОЗИЦИЮ: порядок полей в struct-def равен
		 * порядку членов структуры, и если имя на этой позиции совпадает —
		 * это и есть нужный член, каким бы неуникальным имя ни было. Поиск по
		 * имени остаётся запасным путём: он спасает, когда схема съехала.
		 */
		int index;
		if ((!field_name || !*field_name) && sd && i < st->nr_members)
			index = i;
		else if (sd && i < st->nr_members && field_name
				&& !strcmp(st->members[i].name, field_name))
			index = i;
		else
			index = struct_member_index(st, field_name);
		if (index < 0) {
			strict_or_warn("загрузка", "у структуры %s нет члена %s — поле сейва потеряно",
			               display_sjis0(struct_name), display_sjis1(field_name));
			continue;
		}
		int f_struct_type, f_array_rank;
		enum ain_data_type data_type = variable_type(page, index, &f_struct_type, &f_array_rank);
		if (save_type_norm(data_type) != save_type_norm(field_type))
			VM_ERROR("Bad save file: structure member type mismatch");


		// Обёртки — зеркало записи (см. add_value_to_gsave): пусто оставляем
		// как −1, объект поднимаем записью структуры, скаляр читаем по
		// внутреннему типу. Тип записи в сейве остаётся «обёрточным», так что
		// сверка выше проходит.
		int32_t rec_index = -1;         // запись структуры, если поле объектное
		int obj_struct_type = -1;
		if (data_type == AIN_WRAP || data_type == AIN_OPTION || data_type == AIN_IFACE) {
			struct ain_type *inner = st->members[index].type.array_type;
			/*
			 * ★ЗАГРУЗКА СИММЕТРИЧНА СОХРАНЕНИЮ: прежнее значение ПУСТОГО option
			 * освобождать нельзя — в его слоте не хэндл, а мусор от последней
			 * записи (разбор у option_var_has_value). Иначе загрузка посреди
			 * игры роняет счётчик чужого объекта — тот же §5eu, только в
			 * загрузчике: сохранение чинили, а эту сторону нет.
			 */
			bool stale_option = data_type == AIN_OPTION
				&& !option_var_has_value(page, index);
			if (stale_option)
				page->values[index].i = -1;
			// зеркало записи: `wrap` внутри option — объект, а не скаляр
			bool is_obj = data_type == AIN_IFACE || !inner
				|| inner->data == AIN_STRUCT
				|| inner->data == AIN_WRAP
				|| inner->data == AIN_IFACE
				|| inner->data == AIN_IFACE_WRAP;
			if (!is_obj) {
				variable_fini(page->values[index], inner->data, false);
				page->values[index] = gsave_to_vm_value(save, inner->data,
					inner->struc, inner->rank, kv->value, page, index);
				continue;
			}
			if (kv->value < 0) {
				variable_fini(page->values[index], AIN_STRUCT, false);
				page->values[index].i = -1;
				continue;
			}
			/*
			 * ★ГРАНИЦЫ ПРОВЕРЯЕМ ДО РАЗЫМЕНОВАНИЯ. Поле, объявленное
			 * объектным СЕЙЧАС, в файле СТАРОЙ сборки могло лежать скаляром:
			 * `option<wrap<T>>` до правки уходил в скалярную ветку, и в этом
			 * слоте оказывался не индекс записи, а heap-индекс времён записи.
			 * Без проверки чтение уходит за массив записей — вместо честного
			 * «поле не восстановилось» получаем порчу памяти на чужом сейве.
			 */
			if (kv->value >= save->nr_records) {
				strict_or_warn("загрузка", "обёртка %s: индекс записи %d вне файла (записей %d)",
					display_sjis1(field_name), kv->value, save->nr_records);
				continue;
			}
			// Имя структуры знает сама запись — у интерфейсной ссылки
			// статического типа нет.
			struct gsave_record *r = &save->records[kv->value];
			struct gsave_struct_def *d = save->version >= 7
				? sd_get(save, r->struct_index) : NULL;
			obj_struct_type = ain_get_struct(ain, d ? d->name : r->struct_name);
			if (obj_struct_type < 0) {
				strict_or_warn("загрузка", "обёртка %s: неизвестная структура %s",
					display_sjis1(field_name),
					display_sjis0(d ? d->name : r->struct_name));
				continue;
			}
			rec_index = kv->value;
		} else if (data_type == AIN_STRUCT && kv->value >= 0) {
			rec_index = kv->value;
			obj_struct_type = f_struct_type;
		} else if (data_type == AIN_REF_STRUCT && kv->value >= 0) {
			/*
			 * `ref <структура>` — тот же объектный путь, что и у обычного поля
			 * (один слот с heap-индексом, владеющая ссылка), НО тип берём ИЗ
			 * САМОЙ ЗАПИСИ, а не из объявления члена.
			 *
			 * ★Разница принципиальная: у поля-значения динамический тип всегда
			 * равен статическому, а ССЫЛКА законно держит производную (`ref
			 * CParts` — и `CSpriteParts`, и прочие наследники). Сохранение
			 * пишет ФАКТИЧЕСКИЙ тип объекта, и если при загрузке подставить
			 * объявленный, `gsave_to_vm_value` сверит имя записи с именем базы
			 * и упадёт «Bad save file: structure name mismatch» — то есть сейв
			 * перестанет грузиться совсем. Читаем так же, как соседняя ветка
			 * обёрток (wrap/option/iface), у которых та же природа.
			 */
			if (kv->value >= save->nr_records) {
				strict_or_warn("загрузка", "ref-поле %s: индекс записи %d вне файла (записей %d)",
					display_sjis1(field_name), kv->value, save->nr_records);
				continue;
			}
			struct gsave_record *r = &save->records[kv->value];
			struct gsave_struct_def *d = save->version >= 7
				? sd_get(save, r->struct_index) : NULL;
			obj_struct_type = ain_get_struct(ain, d ? d->name : r->struct_name);
			if (obj_struct_type < 0) {
				strict_or_warn("загрузка", "ref-поле %s: неизвестная структура %s",
					display_sjis1(field_name),
					display_sjis0(d ? d->name : r->struct_name));
				continue;
			}
			rec_index = kv->value;
		}
		/*
		 * `XSYS4_REF_TRACE=1` — что пришло в поля `ref <структура>`. Ими игра
		 * пользуется БЕЗ проверки на пустоту (см. §5ey), поэтому надо видеть
		 * ровно две вещи: пишет ли их сейв вообще (у ОРИГИНАЛЬНЫХ файлов это
		 * отдельный вопрос) и не осталось ли поле пустым после загрузки.
		 */
		if (data_type == AIN_REF_STRUCT && getenv("XSYS4_REF_TRACE"))
			NOTICE("REF %s.%s <- запись %d%s", display_sjis0(struct_name),
			       display_sjis1(field_name), kv->value,
			       kv->value < 0 ? " — В СЕЙВЕ ПУСТО (поле останется -1)" : "");
		/*
		 * ★ПУСТОЕ `ref <структура>` В ФАЙЛЕ. Две разные причины дают одно и то же
		 * −1, и различить их по файлу нельзя: (а) сейв записан сборкой ДО правки
		 * §5ey, когда такие поля не сериализовались вовсе, — тогда объект в файл
		 * не попал и восстановить его нечем; (б) поле и правда было пустым.
		 * Поэтому НЕ утверждаем ни возраст сейва, ни неизбежность падения — но
		 * сказать надо: если игра к такому полю обратится (а пустоты она не ждёт,
		 * ссылку ей заполнил конструктор), движок упадёт далеко от загрузки, и
		 * связать «краш в магазине» с этим местом иначе невозможно.
		 */
		if (data_type == AIN_REF_STRUCT && kv->value < 0) {
			static bool warned = false;
			if (!warned) {
				warned = true;
				WARNING("поле %s.%s (ref) в сейве ПУСТО. Если игра к нему "
					"обратится, движок упадёт («Out of bounds heap index: -1»); "
					"частая причина — сейв, записанный сборкой до §5ey, где "
					"ref-поля не сохранялись вовсе.",
					display_sjis0(struct_name), display_sjis1(field_name));
			}
		}

		if (rec_index >= 0) {
			// ★Приёмник уже держит объект нужной структуры — заполняем ЕГО,
			// сохраняя идентичность: на этот объект могут ссылаться и игра,
			// и движок.
			struct page *cur = page->values[index].i >= 0
				? heap_get_page(page->values[index].i) : NULL;
			struct gsave_record *r = &save->records[rec_index];
			struct gsave_struct_def *d = save->version >= 7
				? sd_get(save, r->struct_index) : NULL;
			const char *sname = d ? d->name : r->struct_name;
			if (cur && cur->type == STRUCT_PAGE && cur->index == obj_struct_type) {
				gsave_fill_struct(save, cur, r, d, sname,
				                  &ain->structures[obj_struct_type]);
				continue;
			}
			variable_fini(page->values[index], AIN_STRUCT, false);
			page->values[index] = gsave_to_vm_value(save, AIN_STRUCT,
				obj_struct_type, 0, rec_index, page, index);
			continue;
		}

		// Прежнее значение освобождаем: иначе счётчик ссылок не сходится и
		// слот уходит в переиспользование (см. историю с `Schedule1`).
		variable_fini(page->values[index], data_type, false);
		page->values[index] = gsave_to_vm_value(save, data_type, f_struct_type,
		                                        f_array_rank, kv->value, page, index);
	}
	/*
	 * Проверка на месте: в ОБЪЕКТНОМ слоте не может лежать 0 — это heap-слот
	 * глобальной страницы. Первое же освобождение такого поля печатает
	 * «попытка освободить heap-слот 0», а следующее обращение валит движок.
	 */
	for (int i = 0; i < page->nr_vars && i < st->nr_members; i++) {
		enum ain_data_type t = st->members[i].type.data;
		// (AIN_ARRAY_TYPE — макрос case-меток, в выражении не годится.)
		bool objref = t == AIN_STRING || t == AIN_STRUCT
			|| t == AIN_ARRAY || t == AIN_ARRAY_INT
			|| t == AIN_ARRAY_STRING || t == AIN_ARRAY_STRUCT
			|| t == AIN_WRAP || t == AIN_OPTION || t == AIN_IFACE;
		if (objref && page->values[i].i == 0)
			strict_or_warn("загрузка", "%s.%s: в объектный слот (%s) попал 0",
				display_sjis0(st->name), display_sjis1(st->members[i].name),
				ain_strtype(ain, t, -1));
	}
}

static union vm_value gsave_to_vm_value(struct gsave *save, enum ain_data_type type, int struct_type, int array_rank, int32_t value, struct page *container, int varno)
{
	switch (type) {
	case AIN_VOID:
	case AIN_INT:
	case AIN_BOOL:
	case AIN_LONG_INT:
	case AIN_FLOAT:
	// Зеркало add_value_to_gsave: перечисления Ixseal и хэндлы функций/делегатов
	// лежат в слоте целым числом. Запись их и раньше работала, а вот чтение
	// падало в default — значение терялось, и `GamePhase#92` возвращался пустым.
	case AIN_ENUM:
	case AIN_ENUM2:
	case AIN_FUNC_TYPE:
		return vm_int(value);
	case AIN_DELEGATE:
		{
			// Зеркало записи: тройки (запись получателя, функция, поколение).
			// Получателя поднимаем через реестр тождества — он окажется ТЕМ ЖЕ
			// объектом, что и в остальном восстановленном графе.
			if (value < 0)
				return vm_int(-1);
			/*
			 * ★ЛЕГАСИ-СЕЙВЫ: до формата троек делегат писался СЫРЫМ числом
			 * (heap-слот старого мира) — прежний код и читал его как число.
			 * Такое значение НЕ индекс массива: трактовка «как тройки» читала
			 * мусор из save->arrays и строила делегат из случайных пар
			 * (obj, fun) — Haha Ranman падала на СТАРТЕ при живой сейв-папке
			 * («Out of bounds page index: 7184/1» в CallPartsUpdateEvent).
			 * Проверяем форму: валидный индекс + ранг 1 + один flat-массив
			 * целых, кратный тройке. Всё прочее — легаси-число, подписки
			 * игра восстановит сама при построении сцен: пусто (−1).
			 */
			if ((size_t)value >= (size_t)save->nr_arrays) {
				NOTICE("делегат из сейва: значение %d вне arrays (%d) — легаси-формат, пусто", value, save->nr_arrays);
				return vm_int(-1);
			}
			struct gsave_array *array = &save->arrays[value];
			if (array->rank != 1 || array->nr_flat_arrays != 1
			    || array->flat_arrays[0].type != AIN_INT
			    || array->flat_arrays[0].nr_values % 3 != 0) {
				NOTICE("делегат из сейва: массив %d — не тройки, легаси-формат, пусто", value);
				return vm_int(-1);
			}
			int slot = heap_alloc_slot(VM_PAGE);
			struct page *page = alloc_page(DELEGATE_PAGE, 0, 0);
			{
				struct gsave_flat_array *fa = &array->flat_arrays[0];
				for (int i = 0; i + 2 < fa->nr_values; i += 3) {
					int32_t obj_rec = fa->values[i].value;
					int fun = fa->values[i + 1].value;
					int obj = -1;
					if (obj_rec >= 0 && obj_rec < save->nr_records) {
						struct gsave_record *r = &save->records[obj_rec];
						struct gsave_struct_def *d = save->version >= 7
							? sd_get(save, r->struct_index) : NULL;
						int sno = ain_get_struct(ain, d ? d->name : r->struct_name);
						if (sno >= 0)
							obj = gsave_to_vm_value(save, AIN_STRUCT, sno, 0, obj_rec, NULL, -1).i;
					}
					page = delegate_append(page, obj, fun, -1);
				}
			}
			heap_set_page(slot, page);
			return vm_int(slot);
		}
	case AIN_STRING:
		{
			int slot = heap_alloc_slot(VM_STRING);
			/*
			 * ПУСТАЯ СТРОКА ЗАПИСЫВАЕТСЯ ДВУМЯ СПОСОБАМИ. xsystem4 пишет
			 * `GSAVE7_EMPTY_STRING` (0x7fffffff), а Windows-сборка Dohna в сейве
			 * версии 9 — обычное −1. Раньше −1 уходило индексом в `save->strings`,
			 * и `string_ref` получал мусорный указатель: движок падал МОЛЧА
			 * (стек по gdb: string_ref ← gsave_to_vm_value(AIN_STRING, value=-1)
			 * ← gsave_fill_struct «WorkerHistory» ← … ← load_struct_list).
			 * Любой отрицательный индекс — это «строки нет».
			 */
			bool empty = value == GSAVE7_EMPTY_STRING || value < 0
				|| value >= save->nr_strings;
			heap[slot].s = string_ref(empty ? &EMPTY_STRING : save->strings[value]);
			return vm_int(slot);
		}
	case AIN_STRUCT:
		{
			// Пустое объектное поле записано как −1 (см. add_value_to_gsave).
			if (value < 0)
				return vm_int(-1);
			struct gsave_record *rec = &save->records[value];
			struct ain_struct *st = &ain->structures[struct_type];
			struct gsave_struct_def *sd =
				save->version >= 7 ? sd_get(save, rec->struct_index) : NULL;
			const char *struct_name = sd ? sd->name : rec->struct_name;
			if (strcmp(struct_name, st->name))
				VM_ERROR("Bad save file: structure name mismatch");
			// Эту запись уже поднимали — отдаём ТОТ ЖЕ объект (см. «тождество
			// объектов»). Ссылку добавляем: поле-владелец освободит её само.
			int known = load_rec_lookup(value);
			if (known >= 0) {
				heap_ref(known);
				return vm_int(known);
			}
			int slot = alloc_struct(struct_type);
			// Регистрируем ДО заполнения: граф циклический.
			load_rec_remember(value, slot);
			gsave_fill_struct(save, heap[slot].page, rec, sd, struct_name, st);
			return vm_int(slot);
		}
	case AIN_ARRAY_TYPE:
		{
			struct gsave_array *array = &save->arrays[value];
			int slot = heap_alloc_slot(VM_PAGE);
			if (array->rank == -1) {
				heap[slot].page = NULL;
				return vm_int(slot);
			}
			if (array_rank != array->rank)
				VM_ERROR("Bad save file: array rank mismatch");
			union vm_value *dims = xmalloc(sizeof(union vm_value) * array_rank);
			for (int i = 0; i < array_rank; i++)
				dims[i].i = array->dimensions[array_rank - 1 - i];
			struct page *page = alloc_array(array_rank, dims, type, struct_type, false);
			heap[slot].page = page;
			free(dims);
			gsave_load_array(save, page, array->flat_arrays);
			return vm_int(slot);
		}
	// Generic-массив (array<?>): у ЗНАЧЕНИЯ нет ни ранга, ни типа элемента —
	// и то и другое берём из СЕЙВА (тип элемента — из flat-массива; индекс
	// структуры — по имени из записи первого элемента). Страница
	// материализуется типизированной, как это делает Array.PushBack.
	//
	// ★ПУСТОЙ массив обязан остаться ТИПИЗИРОВАННОЙ 0-элементной страницей, и
	// тип для неё берётся из ОБЪЯВЛЕНИЯ (`container`/`varno` → .ain), а не из
	// сейва — ровно как в variable_initval_var. Пока пустой массив
	// восстанавливался NULL-страницей, объявленный тип элемента терялся
	// НАСОВСЕМ, а `Array.PushBack` на NULL угадывает его как int
	// (ix_dtype(NULL) == AIN_ARRAY_INT). Дальше всё разваливалось тихо:
	// heap-слоты строк ложились в int-массив СЫРЫМИ числами, без владения,
	// первый же DELETE уносил строку, слот уходил в переиспользование под
	// страницу — и `S_EQUALE` звал strcmp(NULL). Живой случай (Haha Ranman):
	// CollectedCgList.m_aszBase/m_aszCutIn → падение по нажатию CG на титуле
	// (`CollectedCgList@GetIndex` ← `■ＣＧモードデータ更新`). Порча ещё и
	// ЗАПИСЫВАЛАСЬ: в Collection.sav у этих полей flat-тип int и сырые номера
	// слотов вместо строк (strings=0 при 20 именах CG).
	case AIN_ARRAY:
		{
			int slot = heap_alloc_slot(VM_PAGE);
			// Тип элемента по объявлению; AIN_VOID — контейнер неизвестен
			// (вложенный массив), тогда работаем как раньше, по сейву.
			int decl_struct = 0, decl_rank = 1;
			enum ain_data_type decl_array = AIN_VOID;
			if (container && varno >= 0
					&& (container->type == GLOBAL_PAGE
					    || container->type == LOCAL_PAGE
					    || container->type == STRUCT_PAGE))
				decl_array = array_resolve_var_type(container, varno, &decl_struct,
				                                    &decl_rank, NULL);
			struct gsave_array *array = value >= 0 ? &save->arrays[value] : NULL;
			// Пусто: -1 (сейв старой сборки, где тип 79 не сериализовался),
			// rank -1 (NULL-страница на момент записи) или ранг не 1.
			bool empty = !array || array->rank != 1;
			if (array && array->rank > 1)
				WARNING("array<?> ранга %d в сейве не поддержан", array->rank);
			if (!empty && !array->flat_arrays->nr_values)
				empty = true;
			if (empty) {
				// ★Размерностей ровно `decl_rank` штук: указатель на ОДНО
				// значение здесь читался за границей и ронял кучу (у ранга 1
				// это незаметно, у большего — сразу).
				if (decl_array != AIN_VOID) {
					union vm_value *dims = xcalloc(decl_rank < 1 ? 1 : decl_rank,
					                               sizeof(union vm_value));
					heap[slot].page = alloc_array(decl_rank, dims, decl_array,
					                              decl_struct, false);
					free(dims);
				} else {
					heap[slot].page = NULL;
				}
				return vm_int(slot);
			}
			struct gsave_flat_array *fa = array->flat_arrays;
			enum ain_data_type elem = fa->values[0].type;
			/*
			 * ★Сейв, записанный ДО этой правки: объявлен массив ОБЪЕКТОВ, а в
			 * файле лежат числа — это те самые сырые heap-слоты, потерявшие
			 * смысл вместе с миром, в котором писались. Восстанавливать их
			 * нельзя (получим висячие ссылки и падение), склеить обратно не из
			 * чего — строк в файле нет вовсе. Отдаём пустой ТИПИЗИРОВАННЫЙ
			 * массив: игра наполнит его заново.
			 */
			if (decl_array != AIN_VOID) {
				enum ain_data_type decl_elem = array_type(decl_array);
				bool decl_obj = decl_elem == AIN_STRING || decl_elem == AIN_STRUCT
					|| decl_elem == AIN_DELEGATE || ain_is_array_data_type(decl_elem);
				bool save_scalar = elem == AIN_VOID || elem == AIN_INT
					|| elem == AIN_BOOL || elem == AIN_LONG_INT || elem == AIN_FLOAT;
				if (decl_obj && save_scalar) {
					// (ain_strtype отдаёт СТАТИЧЕСКИЙ буфер — два вызова в одном
					// printf печатают одно и то же имя; тип из сейва даём числом.)
					WARNING("array<%s>: в сейве элементы лежат скаляром (тип %d) — "
					        "это сейв старой сборки, содержимое потеряно, "
					        "восстанавливаю массив пустым",
					        ain_strtype(ain, decl_elem, -1), (int)elem);
					union vm_value *dims = xcalloc(decl_rank < 1 ? 1 : decl_rank,
					                               sizeof(union vm_value));
					heap[slot].page = alloc_array(decl_rank, dims, decl_array,
					                              decl_struct, false);
					free(dims);
					return vm_int(slot);
				}
			}
			// (переменная названа guessed, а не container: имя container
			// теперь занято страницей-владельцем значения.)
			enum ain_data_type guessed;
			int elem_struct = -1;
			// Элемент-интерфейс занимает пару слотов: значений в файле вдвое
			// меньше, чем слотов страницы (см. ветку AIN_IFACE ниже).
			bool iface_pair = false;
			switch (elem) {
			case AIN_VOID:
			case AIN_INT:       guessed = AIN_ARRAY_INT; break;
			case AIN_BOOL:      guessed = AIN_ARRAY_BOOL; break;
			case AIN_LONG_INT:  guessed = AIN_ARRAY_LONG_INT; break;
			case AIN_FLOAT:     guessed = AIN_ARRAY_FLOAT; break;
			case AIN_STRING:    guessed = AIN_ARRAY_STRING; break;
			case AIN_FUNC_TYPE: guessed = AIN_ARRAY_FUNC_TYPE; break;
			case AIN_DELEGATE:  guessed = AIN_ARRAY_DELEGATE; break;
			// Перечисление хранится целым, и массив из них — обычный array<int>.
			// Живой случай: `GameContext.m_menuState` = `array<MenuState#92>` в
			// сейве Windows-версии; без этой ветки массив приходил пустым
			// (heap[slot].page = NULL) и сцена не строилась вовсе.
			case AIN_ENUM:
			case AIN_ENUM2:     guessed = AIN_ARRAY_INT; break;
			/*
			 * ★`ref <структура>` В ЭЛЕМЕНТЕ — ТОТ ЖЕ ОБЪЕКТНЫЙ ПУТЬ.
			 *
			 * Так оригинал пишет `array<wrap<T>>` (замер: `ActionHistory.
			 * m_history`, `BattlePlayerCollection.m_player` — тип элементов 21),
			 * и с недавних пор так же пишем мы. Без этой ветки чтение уходило в
			 * default: «array<?> с элементом ref hll_struct не поддержан»,
			 * массив приходил ПУСТЫМ, и первый же вызов делегата по его
			 * элементу ронял движок — `Not a delegate page` в
			 * `AchievementCollection@GetCleared` на СТАРТЕ игры, стоило
			 * появиться своему `Achievement.asd`.
			 */
			/*
			 * ★ЭЛЕМЕНТ-ИНТЕРФЕЙС (89): ПАРА СЛОТОВ НА ЭЛЕМЕНТ.
			 *
			 * Так оригинал пишет `array<wrap<интерфейс>>` (замер:
			 * `AchievementCollection.m_achievements` в его `Achievement.asd` —
			 * 65 значений типа 89). Страницу поднимаем интерфейсной, иначе
			 * элементы съезжают вдвое: у пары два слота (объект и база в его
			 * таблице методов, см. array_elem_slots).
			 */
			case AIN_IFACE:
				guessed = decl_array != AIN_VOID ? decl_array : AIN_IFACE_WRAP;
				iface_pair = true;
				// fallthrough — структуру элемента ищем так же, по записи
			case AIN_REF_STRUCT:
			case AIN_STRUCT:
				if (!iface_pair)
					guessed = AIN_ARRAY_STRUCT;
				// ★Тип элемента угадываем по ПЕРВОЙ НЕПУСТОЙ записи: в объектном
				// слоте законно лежит −1 («значения нет»), и `records[-1]` — это
				// чтение за границей массива записей с мусорным struct_index.
				int probe = -1;
				for (int i = 0; i < fa->nr_values; i++) {
					if (fa->values[i].value >= 0) {
						probe = fa->values[i].value;
						break;
					}
				}
				struct gsave_record *rec = probe >= 0 ? rec_get(save, probe) : NULL;
				if (rec) {
					struct gsave_struct_def *psd = save->version >= 7
						? sd_get(save, rec->struct_index) : NULL;
					char *name = (save->version >= 7)
						? (psd ? psd->name : NULL) : rec->struct_name;
					elem_struct = name ? ain_get_struct(ain, name) : -1;
					if (elem_struct < 0) {
						WARNING("array<?>: неизвестная структура %s",
						        name ? display_sjis0(name) : "(нет описания)");
						heap[slot].page = NULL;
						return vm_int(slot);
					}
				}
				break;
			default:
				strict_or_warn("загрузка", "array<?> с элементом %s не поддержан",
				               ain_strtype(ain, elem, -1));
				heap[slot].page = NULL;
				return vm_int(slot);
			}
			/*
			 * ★ТИП СТРАНИЦЫ БЕРЁМ ИЗ САМОГО СЕЙВА, а не из объявления.
			 *
			 * Соблазн «объявление точнее» проверен и отвергнут замером: у
			 * `array<wrap<интерфейс>>` объявление даёт ДВА слота на элемент
			 * (см. array_elem_slots), а в файле — и у нас, и у ОРИГИНАЛА —
			 * значений ровно по числу элементов (`Achievement.asd`: размер
			 * 130, значений 130; у оригинала 9 при размере 9). То есть в
			 * рантайме такая страница ОДНОСЛОТОВАЯ, и подъём «по объявлению»
			 * ронял загрузку на «unexpected number of array elements».
			 */
			/*
			 * ★МАССИВ ИНТЕРФЕЙСОВ: РАЗМЕР В ФАЙЛЕ — В СЛОТАХ, А НЕ В ЭЛЕМЕНТАХ.
			 *
			 * Оригинал пишет каждый элемент ПАРОЙ значений — слот объекта и −1
			 * вместо базы интерфейса. Видно прямо в файле (`Achievement.asd`,
			 * `AchievementCollection.m_achievements`): 130 значений типа 21,
			 * идущих как `2 −1 4 −1 6 −1 …`, при 65 реальных достижениях.
			 *
			 * Тип значений (21) про интерфейс ничего не говорит, поэтому случай
			 * узнаём по ОБЪЯВЛЕНИЮ поля: `array<IAchievement>` — элемент-интерфейс,
			 * значит страница обязана быть парной. Подняв её однослотовой, мы
			 * получали 130 «элементов», где каждый второй — та самая −1: поиск по
			 * такому массиву звал предикат на пустом элементе, и первый же
			 * `X_REF` ронял игру (§5fb-13).
			 */
			bool decl_iface = decl_struct >= 0 && decl_struct < ain->nr_structures
				&& ain->structures[decl_struct].is_interface;
			union vm_value dim = { .i = array->dimensions[0] };
			if (decl_iface && !iface_pair && dim.i > 0 && (dim.i % 2) == 0
					&& array->flat_arrays[0].nr_values == dim.i) {
				guessed = AIN_IFACE_WRAP;
				elem_struct = decl_struct;
				dim.i /= 2;
			}
			struct page *page = alloc_array(1, &dim, guessed, elem_struct, false);
			heap[slot].page = page;
			gsave_load_array(save, page, array->flat_arrays);
			return vm_int(slot);
		}
	// Зеркало записи (см. add_value_to_gsave): `ref <структура>` поднимается как
	// объект и через тот же реестр тождества — ссылка окажется на ТОТ ЖЕ объект,
	// что и у остальных полей восстановленного графа (§5ey).
	case AIN_REF_STRUCT:
		return gsave_to_vm_value(save, AIN_STRUCT, struct_type, array_rank, value,
		                         container, varno);
	case AIN_REF_INT:
	case AIN_REF_FLOAT:
	case AIN_REF_STRING:
	case AIN_REF_ARRAY_INT:
	case AIN_REF_ARRAY_FLOAT:
	case AIN_REF_ARRAY_STRING:
	case AIN_REF_ARRAY_STRUCT:
	case AIN_REF_FUNC_TYPE:
	case AIN_REF_ARRAY_FUNC_TYPE:
	case AIN_REF_BOOL:
	case AIN_REF_ARRAY_BOOL:
	case AIN_REF_LONG_INT:
	case AIN_REF_ARRAY_LONG_INT:
	case AIN_REF_DELEGATE:
	case AIN_REF_ARRAY_DELEGATE:
	case AIN_REF_ARRAY:
		return vm_int(-1);
	default:
		strict_or_warn("загрузка", "тип %s не восстанавливается — поле осталось пустым",
		                ain_strtype(ain, type, -1));
		return vm_int(-1);
	}
}

int load_globals_from_gsave(struct gsave *save, const char *keyname, const char *group_name, int *n)
{
	if (strcmp(keyname, save->key))
		VM_ERROR("Attempted to load save data with wrong key: %s", display_sjis0(keyname));
	if (!group_name)
		group_name = "";
	if (!save->group)
		save->group = strdup("");
	if (strcmp(group_name, save->group))
		VM_ERROR("Attempted to load save data with wrong group name: '%s'", display_sjis0(group_name));

	/*
	 * ★Зеркало save_globals: реестр «запись → объект» живёт одну операцию.
	 * Индексы записей НАЧИНАЮТСЯ ЗАНОВО в каждом файле, и без сброса запись
	 * №N второго файла находила в реестре объект из ПЕРВОГО — при полной
	 * сейв-папке (несколько групповых загрузок подряд) весь граф расползался;
	 * один файл в одиночку падения не давал, что и путало бисект.
	 */
	obj_maps_reset();

	for (struct gsave_global *g = save->globals; g < save->globals + save->nr_globals; g++) {
		int global_index = ain_get_global(ain, g->name);
		if (global_index < 0) {
			WARNING("invalid global name %s", display_sjis0(g->name));
			return 0;
		}
		struct ain_type *type = &ain->globals[global_index].type;
		if (type->data != g->type) {
			WARNING("%s: type mismatch", display_sjis0(g->name));
			return 0;
		}
		bool call_dtors = false; // Destructors for old objects should not be called.
		global_set(global_index, gsave_to_vm_value(save, type->data, type->struc, type->rank,
		                                           g->value, heap_get_page(0), global_index), call_dtors);
		if (n)
			(*n)++;
	}

	return 1;
}

int load_globals(const char *keyname, const char *filename, const char *group_name, int *n)
{
	char *path = savedir_path(filename);
	int retval;

	// First, try reading as a gsave.
	enum savefile_error error;
	struct gsave *save = gsave_read(path, &error);
	switch (error) {
	case SAVEFILE_SUCCESS:
		free(path);
		retval = load_globals_from_gsave(save, keyname, group_name, n);
		gsave_free(save);
		return retval;
	case SAVEFILE_INVALID_SIGNATURE:
		// If not a System4 save file, try reading as a json.
		retval = load_globals_from_json(path, keyname, group_name, n);
		free(path);
		return retval;
	default:
		WARNING("%s: %s", display_utf0(path), savefile_strerror(error));
		free(path);
		return 0;
	}
}

/*
 * `system.SerializeStruct(fileName, array<int> structPageList, bool saveFolder)` —
 * сохранение ПРОИЗВОЛЬНОГО НАБОРА СТРУКТУР, отдельное от глобалов и от образа VM.
 * Через него Dohna пишет метаданные слота (`SaveObject`: миниатюра, комментарий,
 * день, деньги) — `LocalSave@Save` → `SaveObject@Save` → `AFL_GameSave_StructSave`.
 * Пока функция была заглушкой, игра получала false и показывала «Failed to save
 * SaveData1000.asd»: сохраниться было нельзя вообще.
 *
 * Список приходит СТРАНИЦАМИ (не парами «объект + база интерфейса»): его готовит
 * `Array.SYSTEMONLY_GetStructPageList`. Каждую структуру кладём в gsave тем же
 * `add_value_to_gsave`, что и глобалы, — формат уже умеет записи структур с
 * определениями полей (`gsave_add_struct_def`), так что своего формата не нужно.
 * Имена «глобалов» здесь служебные (порядковый номер): при загрузке они не ищутся
 * в `ain->globals`, разбор идёт по позиции.
 */
int save_struct_list(const char *filename, struct page *list)
{
	obj_maps_reset();
	int n = list && list->type == ARRAY_PAGE ? list->nr_vars : 0;
	/*
	 * ЗАГОЛОВОК ПИШЕМ КАК WINDOWS-СБОРКА, иначе её загрузчик получит чужие значения.
	 * Снято сравнением четырёх пар файлов (`SaveData1000.asd`, `AFConfig.asd`,
	 * `AFInfo.asd`, `Collection.asd`) — расхождения были во всех четырёх одинаковы:
	 *   key и group — «serialize_struct», а не пустые строки;
	 *   nr_ain_globals — 1, а не полное число глобалов .ain;
	 *   имя «глобала» — ПУСТОЕ, а не порядковый номер (при загрузке имена всё равно
	 *     не ищутся: разбор идёт по позиции, см. load_struct_list);
	 *   глобалов на ОДИН больше — последний нулевой, замыкающий.
	 * Записи (`records`) при этом совпадали с оригиналом один в один.
	 */
	bool v9 = get_gsave_version() >= 9;
	struct gsave *save = gsave_create(get_gsave_version(),
			v9 ? "serialize_struct" : "",
			v9 ? 1 : ain->nr_globals,
			v9 ? "serialize_struct" : NULL);
	gsave_add_globals_record(save, n);
	if (v9) {
		// ★Запись GLOBALS ссылается на n объектов, а самих глобалов n+1: у оригинала
		// `record 0` имеет ровно один индекс при двух глобалах. Расширяем ТОЛЬКО
		// массив глобалов, список индексов записи не трогаем.
		save->globals = xrealloc_array(save->globals, n, n + 1,
				sizeof(struct gsave_global));
		save->nr_globals = n + 1;
	}
	for (int i = 0; i < n; i++) {
		char name[16];
		snprintf(name, sizeof(name), "%d", i);
		struct gsave_global *g = &save->globals[i];
		g->name = strdup(v9 ? "" : name);
		g->type = AIN_STRUCT;
		g->type_param = AIN_STRUCT;   // v9: параметр типа (у не-обёрток дублирует тип)
		g->value = add_value_to_gsave(AIN_STRUCT, list->values[i], save);
	}
	if (v9) {
		// Замыкающий глобал: тип 0, значение 0, имя пустое — ровно как у оригинала.
		struct gsave_global *g = &save->globals[n];
		g->name = strdup("");
		g->type = AIN_VOID;
		g->type_param = AIN_VOID;
		g->value = 0;
	}

	char *path = savedir_path(filename);
	FILE *fp = file_open_utf8(path, "wb");
	if (!fp) {
		WARNING("SerializeStruct: не открыть %s: %s", display_utf0(path), strerror(errno));
		free(path);
		gsave_free(save);
		return 0;
	}
	free(path);

	bool encrypt = !AIN_VERSION_GTE(ain, 6, 0);
	int compression_level = AIN_VERSION_GTE(ain, 6, 0) ? 1 : 9;
	enum savefile_error error = gsave_write(save, fp, encrypt, compression_level);
	if (error != SAVEFILE_SUCCESS)
		WARNING("SerializeStruct: %s", savefile_strerror(error));
	fclose(fp);
	gsave_free(save);
	return error == SAVEFILE_SUCCESS;
}

/*
 * Обратная сторона: структуры ЗАПОЛНЯЮТСЯ на месте. Игра передаёт список уже
 * созданных объектов-приёмников и держит на них ссылки (`wrap<ISerializable>`),
 * поэтому подменяем СТРАНИЦУ в heap-слоте приёмника, а не сам слот — иначе
 * ссылки вызывающей стороны указывали бы на старое, незаполненное содержимое.
 */
int load_struct_list(const char *filename, struct page *list)
{
	obj_maps_reset();
	save_file_corrupt = false;
	int n = list && list->type == ARRAY_PAGE ? list->nr_vars : 0;
	char *path = savedir_path(filename);
	enum savefile_error error;
	struct gsave *save = gsave_read(path, &error);
	if (error != SAVEFILE_SUCCESS) {
		// «Файла нет» — обычное дело: игра каждый запуск пробует прочитать
		// Collection/CommonSystemData, которых на чистом профиле ещё нет.
		if (error != SAVEFILE_FILE_ERROR)
			WARNING("DeserializeStruct: %s: %s", display_utf0(path),
			        savefile_strerror(error));
		free(path);
		return 0;
	}
	free(path);

	// A/B: XSYS4_DESER_DRY=1 — файл читается (значит игра считает загрузку
	// удавшейся и идёт дальше), но в приёмники НИЧЕГО не пишется. Разделяет две
	// версии падения после загрузки: «испорчены восстановленные данные» против
	// «ломается сам путь загрузки, что бы мы туда ни положили».
	int nr = getenv("XSYS4_DESER_DRY") ? 0 : (save->nr_globals < n ? save->nr_globals : n);
	for (int i = 0; i < nr; i++) {
		int dst_slot = list->values[i].i;
		struct page *dst = heap_get_page(dst_slot);
		if (!dst || dst->type != STRUCT_PAGE) {
			WARNING("DeserializeStruct: приёмник %d не структура", i);
			continue;
		}
		/*
		 * ★Заполняем ПРИЁМНИК НА МЕСТЕ, а не строим временный объект с переносом
		 * значений. Игра держит эти структуры не только heap-слотом, но и
		 * прямыми ссылками (`wrap<ISerializable>`, интерфейсные пары), и любая
		 * подстановка чужих объектов рвала граф: падало потом на посторонней
		 * строке `Schedule1` в `SceneHome@SetSchedule` — литерале, которого в
		 * сейве нет вовсе (осиротевшие слоты уходили в переиспользование).
		 */
		int32_t rec_index = save->globals[i].value;
		if (rec_index < 0)
			continue;
		struct gsave_record *rec = &save->records[rec_index];
		struct gsave_struct_def *sd = save->version >= 7
			? sd_get(save, rec->struct_index) : NULL;
		const char *sname = sd ? sd->name : rec->struct_name;
		struct ain_struct *st = &ain->structures[dst->index];
		if (strcmp(sname, st->name)) {
			strict_or_warn("загрузка", "приёмник %d — %s, а в сейве %s",
			               i, display_sjis0(st->name), display_sjis1(sname));
			continue;
		}
		gsave_fill_struct(save, dst, rec, sd, sname, st);
	}
	gsave_free(save);
	obj_maps_reset();
	if (save_file_corrupt) {
		// Данные частично не разобраны — честнее отдать отказ: игра покажет
		// своё «загрузка не удалась» и останется работоспособной.
		WARNING("DeserializeStruct: '%s' повреждён — отвечаю игре отказом",
			display_utf0(filename));
		return 0;
	}
	return 1;
}

/*
 * Комментарий слота — ОТДЕЛЬНАЯ пара вызовов
 * (`system.Write/ReadSerializeStructComment`), а не поле внутри структуры: игра
 * пишет его сразу после сохранения (`AFL_GameSave_WriteStructSaveComment`) и
 * читает при построении списка слотов (`LocalSave::GetComment`). Пока обе были
 * заглушками, слот показывал заготовки конструктора `SaveObjectParam@0` —
 * «Day0 <None>», хотя сам сейв уже читался.
 *
 * Храним сайдкаром рядом с сейвом (`<имя>.cmt`, SJIS как есть). Формат наш:
 * оригинальный .asd, судя по всему, держит комментарий внутри, но лезть в
 * готовый gsave ради строки — значит переписывать записи и рисковать сейвом,
 * который в остальном совместим.
 */
static char *comment_path(const char *filename)
{
	char *name = xmalloc(strlen(filename) + 5);
	sprintf(name, "%s.cmt", filename);
	char *path = savedir_path(name);
	free(name);
	return path;
}

/*
 * Записать комментарий ВНУТРЬ сейва (формат версии 9). Игра зовёт
 * `WriteSerializeStructComment` сразу после `SerializeStruct`, поэтому файл уже
 * лежит на диске: перечитываем его, подставляем строку и пишем обратно тем же
 * `gsave_write`. Дороже сайдкара на одну перезапись файла, зато сейв получается
 * такой же, как у Windows-сборки, и читается ею.
 */
static int save_struct_comment_inline(const char *filename, struct string *comment)
{
	char *path = savedir_path(filename);
	enum savefile_error err;
	struct gsave *gs = gsave_read(path, &err);
	if (!gs) {
		WARNING("WriteSerializeStructComment: не прочитать %s: %s",
		        display_utf0(path), savefile_strerror(err));
		free(path);
		return 0;
	}
	for (int i = 0; i < gs->nr_comments; i++) {
		if (gs->comments[i])
			free_string(gs->comments[i]);
	}
	free(gs->comments);
	gs->nr_comments = 1;
	gs->comments = xcalloc(1, sizeof(struct string *));
	gs->comments[0] = string_ref(comment ? comment : &EMPTY_STRING);

	int ok = 0;
	FILE *fp = file_open_utf8(path, "wb");
	if (fp) {
		bool encrypt = !AIN_VERSION_GTE(ain, 6, 0);
		int compression_level = AIN_VERSION_GTE(ain, 6, 0) ? 1 : 9;
		err = gsave_write(gs, fp, encrypt, compression_level);
		fclose(fp);
		ok = err == SAVEFILE_SUCCESS;
		if (!ok)
			WARNING("WriteSerializeStructComment: %s", savefile_strerror(err));
	} else {
		WARNING("WriteSerializeStructComment: не открыть %s: %s",
		        display_utf0(path), strerror(errno));
	}
	free(path);
	gsave_free(gs);
	return ok;
}

int save_struct_comment(const char *filename, struct string *comment)
{
	if (get_gsave_version() >= 9)
		return save_struct_comment_inline(filename, comment);

	char *path = comment_path(filename);
	FILE *fp = file_open_utf8(path, "wb");
	if (!fp) {
		WARNING("WriteSerializeStructComment: не открыть %s: %s",
		        display_utf0(path), strerror(errno));
		free(path);
		return 0;
	}
	free(path);
	size_t n = comment && comment->size > 0 ? (size_t)comment->size : 0;
	bool ok = !n || fwrite(comment->text, n, 1, fp) == 1;
	fclose(fp);
	return ok;
}

struct string *load_struct_comment(const char *filename)
{
	char *path = comment_path(filename);
	size_t len = 0;
	char *buf = file_read(path, &len);
	free(path);
	if (buf) {
		struct string *s = make_string(buf, len);
		free(buf);
		return s;
	}
	/*
	 * Сайдкара нет — значит сейв не наш, а от Windows-сборки: там комментарий
	 * лежит ВНУТРИ файла, отдельной секцией версии 9 (FINDINGS §5cp). Без этой
	 * ветки слот из папки оригинала показывался как «Day0 <None>», хотя сам сейв
	 * уже читался и грузился.
	 */
	char *save_path = savedir_path(filename);
	enum savefile_error err;
	struct gsave *gs = gsave_read(save_path, &err);
	free(save_path);
	if (!gs)
		return NULL;
	struct string *s = NULL;
	if (gs->nr_comments > 0 && gs->comments && gs->comments[0])
		s = string_ref(gs->comments[0]);
	gsave_free(gs);
	return s;
}

/*
 * ★ОБРАЗ ВОЗОБНОВЛЕНИЯ ОДНОРАЗОВЫЙ — И ЭТО МЕШАЕТ ОТЛАДКЕ.
 *
 * `SystemSuspend.asd` удаляет САМА игра сразу после восстановления (штатное
 * поведение, оригинал делает так же). Для замеров это дорого: каждый прогон
 * цикла «дойти до места → F8 → перезапуск» приходится проходить заново, а
 * снимок нужного экрана живёт ровно один запуск — за сессию мы дважды теряли
 * его и переигрывали путь до ADV с нуля.
 *
 * `XSYS4_KEEP_SUSPEND=copy` — перед удалением отложить копию рядом
 * (`<имя>.kept`): игра ведёт себя ровно как обычно, а файл остаётся стендом,
 * который можно вернуть на место сколько угодно раз.
 * `XSYS4_KEEP_SUSPEND=hold` — не удалять вовсе: каждый следующий запуск снова
 * поднимется из того же образа (повторяемый прогон одного и того же места).
 */
static bool keep_suspend_file(const char *filename, const char *path)
{
	static const char *mode = (const char *)1;
	if (mode == (const char *)1)
		mode = getenv("XSYS4_KEEP_SUSPEND");
	if (!mode || !*mode || !strstr(filename, "SystemSuspend"))
		return false;
	if (!strcmp(mode, "hold")) {
		NOTICE("XSYS4_KEEP_SUSPEND=hold: '%s' НЕ удаляю — следующий запуск "
		       "поднимется из того же образа", display_utf0(path));
		return true;
	}
	// copy (и любое иное значение): откладываем копию, удаление оставляем игре
	char *kept = xmalloc(strlen(path) + 6);
	sprintf(kept, "%s.kept", path);
	size_t len = 0;
	uint8_t *data = file_read(path, &len);
	if (data && file_write(kept, data, len))
		NOTICE("XSYS4_KEEP_SUSPEND: копия образа отложена в '%s' (%zu байт)",
		       display_utf0(kept), len);
	else
		WARNING("XSYS4_KEEP_SUSPEND: не удалось отложить копию '%s'",
			display_utf0(kept));
	free(data);
	free(kept);
	return false;
}

int delete_save_file(const char *filename)
{
	char *path = savedir_path(filename);
	if (!file_exists(path)) {
		free(path);
		return 0;
	}
	if (keep_suspend_file(filename, path)) {
		free(path);
		return 1;  // игре отвечаем «удалено»: её логика не должна меняться
	}
	if (remove_utf8(path)) {
		WARNING("remove(\"%s\"): %s", display_utf0(path), strerror(errno));
		free(path);
		return 0;
	}
	free(path);
	return 1;
}
